// SPDX-License-Identifier: GPL-2.0
/*
 * NVMe over Fabrics TCP target.
 * Copyright (c) 2018 Lightbits Labs. All rights reserved.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/crc32c.h>
#include <linux/err.h>
#include <linux/nvme-tcp.h>
#include <linux/nvme-keyring.h>
#include <net/sock.h>
#include <net/tcp.h>
#include <net/tls.h>
#include <net/tls_prot.h>
#include <net/handshake.h>
#include <linux/inet.h>
#include <linux/llist.h>
#include <linux/sizes.h>
#include <net/page_pool/helpers.h>
#include <net/rx_loan.h>
#include <trace/events/sock.h>

#include "nvmet.h"

#define NVMET_TCP_DEF_INLINE_DATA_SIZE	(4 * PAGE_SIZE)
#define NVMET_TCP_MAXH2CDATA		0x400000 /* 16M arbitrary limit */

/* rx_loan region receive: loan entries per KiB, so 1448-byte frames fit */
#define NVMET_TCP_RX_ENTRY_BYTES	1024
/* cap on a cost sample, region pages per 100 pages (as kNFSd) */
#define NVMET_TCP_RX_FACTOR_MAX		12800U
/* region pages the transport asks to be set aside for it */
#define NVMET_TCP_RX_FLOOR		(SZ_4M >> PAGE_SHIFT)
/*
 * The receive buffer a queue is held to once it receives region pages,
 * and its port's listener with it (rx_loan_hold_apply()): kNFSd's default.
 */
#define NVMET_TCP_RX_RCVBUF		SZ_256K
/*
 * Loan entries per command at most: a write needing more is copied whole,
 * and counted.  2048 bio_vecs are 32 KiB, past SLUB's kmalloc caches: an
 * order-3 allocation from the page allocator; the two bitmaps are 256 bytes
 * each.  That covers 2046 KiB: a Linux host's default max_sectors_kb (1280)
 * is borrowed whole.  The table is also bounded by the region's size.
 */
#define NVMET_TCP_RX_TABLE_MAX		2048
static_assert((NVMET_TCP_RX_TABLE_MAX - 2) * NVMET_TCP_RX_ENTRY_BYTES >=
	      SZ_1M + SZ_256K);
static_assert(NVMET_TCP_RX_TABLE_MAX * sizeof(struct bio_vec) <= SZ_32K);
#define NVMET_TCP_BACKLOG 128

static int param_store_val(const char *str, int *val, int min, int max)
{
	int ret, new_val;

	ret = kstrtoint(str, 10, &new_val);
	if (ret)
		return -EINVAL;

	if (new_val < min || new_val > max)
		return -EINVAL;

	*val = new_val;
	return 0;
}

static int set_params(const char *str, const struct kernel_param *kp)
{
	return param_store_val(str, kp->arg, 0, INT_MAX);
}

static const struct kernel_param_ops set_param_ops = {
	.set	= set_params,
	.get	= param_get_int,
};

/* Define the socket priority to use for connections were it is desirable
 * that the NIC consider performing optimized packet processing or filtering.
 * A non-zero value being sufficient to indicate general consideration of any
 * possible optimization.  Making it a module param allows for alternative
 * values that may be unique for some NIC implementations.
 */
static int so_priority;
device_param_cb(so_priority, &set_param_ops, &so_priority, 0644);
MODULE_PARM_DESC(so_priority, "nvmet tcp socket optimize priority: Default 0");

/* Define a time period (in usecs) that io_work() shall sample an activated
 * queue before determining it to be idle.  This optional module behavior
 * can enable NIC solutions that support socket optimized packet processing
 * using advanced interrupt moderation techniques.
 */
static int idle_poll_period_usecs;
device_param_cb(idle_poll_period_usecs, &set_param_ops,
		&idle_poll_period_usecs, 0644);
MODULE_PARM_DESC(idle_poll_period_usecs,
		"nvmet tcp io_work poll till idle time period in usecs: Default 0");

#ifdef CONFIG_NVME_TARGET_TCP_TLS
/*
 * TLS handshake timeout
 */
static int tls_handshake_timeout = 10;
module_param(tls_handshake_timeout, int, 0644);
MODULE_PARM_DESC(tls_handshake_timeout,
		 "nvme TLS handshake timeout in seconds (default 10)");
#endif

#define NVMET_TCP_RECV_BUDGET		8
#define NVMET_TCP_SEND_BUDGET		8
#define NVMET_TCP_IO_WORK_BUDGET	64

enum nvmet_tcp_send_state {
	NVMET_TCP_SEND_DATA_PDU,
	NVMET_TCP_SEND_DATA,
	NVMET_TCP_SEND_R2T,
	NVMET_TCP_SEND_DDGST,
	NVMET_TCP_SEND_RESPONSE
};

enum nvmet_tcp_recv_state {
	NVMET_TCP_RECV_PDU,
	NVMET_TCP_RECV_DATA,
	NVMET_TCP_RECV_DDGST,
	NVMET_TCP_RECV_ERR,
};

enum {
	NVMET_TCP_F_INIT_FAILED = (1 << 0),
	NVMET_TCP_F_R2T_SENT	= (1 << 1),
	/* WRITE data is received into cmd->lt (region loans) */
	NVMET_TCP_F_RX_LOAN	= (1 << 2),
	/* ->data_done ran in hard irq: return pending on queue->rx_done */
	NVMET_TCP_F_RX_DEFER	= (1 << 3),
	/* no memory to copy into: the solicited data is discarded */
	NVMET_TCP_F_RX_SINK	= (1 << 4),
};

struct nvmet_tcp_cmd {
	struct nvmet_tcp_queue		*queue;
	struct nvmet_req		req;

	struct nvme_tcp_cmd_pdu		*cmd_pdu;
	struct nvme_tcp_rsp_pdu		*rsp_pdu;
	struct nvme_tcp_data_pdu	*data_pdu;
	struct nvme_tcp_r2t_pdu		*r2t_pdu;

	u32				rbytes_done;
	u32				wbytes_done;

	u32				pdu_len;
	u32				pdu_recv;
	int				sg_idx;
	char				recv_cbuf[CMSG_LEN(sizeof(char))];
	struct msghdr			recv_msg;
	struct bio_vec			*iov;
	u32				flags;

	struct list_head		entry;
	struct llist_node		lentry;

	/* send state */
	u32				offset;
	struct scatterlist		*cur_sg;
	enum nvmet_tcp_send_state	state;

	__le32				exp_ddgst;
	__le32				recv_ddgst;

	/* rx_loan region receive (NVMET_TCP_F_RX_LOAN) */
	struct rx_loan_table		lt;
	struct rx_loan_credit		cr;
	u32				r2t_end;	/* end of the solicited unit */
	struct llist_node		rx_done_node;
};

enum nvmet_tcp_queue_state {
	NVMET_TCP_Q_CONNECTING,
	NVMET_TCP_Q_TLS_HANDSHAKE,
	NVMET_TCP_Q_LIVE,
	NVMET_TCP_Q_DISCONNECTING,
	NVMET_TCP_Q_FAILED,
};

struct nvmet_tcp_queue {
	struct socket		*sock;
	struct nvmet_tcp_port	*port;
	struct work_struct	io_work;
	struct nvmet_cq		nvme_cq;
	struct nvmet_sq		nvme_sq;
	struct kref		kref;

	/* send state */
	struct nvmet_tcp_cmd	*cmds;
	unsigned int		nr_cmds;
	struct list_head	free_list;
	struct llist_head	resp_list;
	struct list_head	resp_send_list;
	int			send_list_len;
	struct nvmet_tcp_cmd	*snd_cmd;

	/* recv state */
	int			offset;
	int			left;
	enum nvmet_tcp_recv_state rcv_state;
	struct nvmet_tcp_cmd	*cmd;
	union nvme_tcp_pdu	pdu;

	/* digest state */
	bool			hdr_digest;
	bool			data_digest;

	/* TLS state */
	key_serial_t		tls_pskid;
	struct delayed_work	tls_handshake_tmo_work;

	unsigned long           poll_end;

	spinlock_t		state_lock;
	enum nvmet_tcp_queue_state state;

	struct sockaddr_storage	sockaddr;
	struct sockaddr_storage	sockaddr_peer;
	struct work_struct	release_work;

	int			idx;
	struct list_head	queue_list;

	struct nvmet_tcp_cmd	connect;

	struct page_frag_cache	pf_cache;

	void (*data_ready)(struct sock *);
	void (*state_change)(struct sock *);
	void (*write_space)(struct sock *);

	/* rx_loan region receive: io_work context unless noted */
	bool			rx_region;
	bool			rx_kicked;	/* set by nvmet_tcp_rx_wake() */
	struct rx_loan_budget	*rx_budget;
	struct nvmet_tcp_cmd	*rx_unit;	/* owns the unit until its data is in */
	struct list_head	rx_withheld;	/* R2Ts waiting for the unit */
	struct llist_head	rx_done;	/* deferred ->data_done returns */
	struct rx_loan_waiter	rx_wait;
	/* the receive-buffer hold; pending is set by the actor */
	struct rx_loan_hold	rx_hold;
};

struct nvmet_tcp_port {
	struct socket		*sock;
	struct work_struct	accept_work;
	struct nvmet_port	*nport;
	struct sockaddr_storage addr;
	void (*data_ready)(struct sock *);
	/* the listener's receive-buffer hold: accept_work only */
	struct rx_loan_hold	rx_hold;
};

static DEFINE_IDA(nvmet_tcp_queue_ida);
static LIST_HEAD(nvmet_tcp_queue_list);
static DEFINE_MUTEX(nvmet_tcp_queue_mutex);

static struct workqueue_struct *nvmet_tcp_wq;
static const struct nvmet_fabrics_ops nvmet_tcp_ops;
static void nvmet_tcp_free_cmd(struct nvmet_tcp_cmd *c);
static void nvmet_tcp_free_cmd_buffers(struct nvmet_tcp_cmd *cmd);

static inline u16 nvmet_tcp_cmd_tag(struct nvmet_tcp_queue *queue,
		struct nvmet_tcp_cmd *cmd)
{
	if (unlikely(!queue->nr_cmds)) {
		/* We didn't allocate cmds yet, send 0xffff */
		return USHRT_MAX;
	}

	return cmd - queue->cmds;
}

static inline bool nvmet_tcp_has_data_in(struct nvmet_tcp_cmd *cmd)
{
	return nvme_is_write(cmd->req.cmd) &&
		cmd->rbytes_done < cmd->req.transfer_len;
}

static inline bool nvmet_tcp_need_data_in(struct nvmet_tcp_cmd *cmd)
{
	return nvmet_tcp_has_data_in(cmd) && !cmd->req.cqe->status;
}

static inline bool nvmet_tcp_need_data_out(struct nvmet_tcp_cmd *cmd)
{
	return !nvme_is_write(cmd->req.cmd) &&
		cmd->req.transfer_len > 0 &&
		!cmd->req.cqe->status;
}

static inline bool nvmet_tcp_has_inline_data(struct nvmet_tcp_cmd *cmd)
{
	return nvme_is_write(cmd->req.cmd) && cmd->pdu_len &&
		!cmd->rbytes_done;
}

static inline struct nvmet_tcp_cmd *
nvmet_tcp_get_cmd(struct nvmet_tcp_queue *queue)
{
	struct nvmet_tcp_cmd *cmd;

	cmd = list_first_entry_or_null(&queue->free_list,
				struct nvmet_tcp_cmd, entry);
	if (!cmd)
		return NULL;
	list_del_init(&cmd->entry);

	cmd->rbytes_done = cmd->wbytes_done = 0;
	cmd->pdu_len = 0;
	cmd->pdu_recv = 0;
	cmd->iov = NULL;
	cmd->flags = 0;
	return cmd;
}

static inline void nvmet_tcp_put_cmd(struct nvmet_tcp_cmd *cmd)
{
	if (unlikely(cmd == &cmd->queue->connect))
		return;

	list_add_tail(&cmd->entry, &cmd->queue->free_list);
}

static inline int queue_cpu(struct nvmet_tcp_queue *queue)
{
	return queue->sock->sk->sk_incoming_cpu;
}

static inline u8 nvmet_tcp_hdgst_len(struct nvmet_tcp_queue *queue)
{
	return queue->hdr_digest ? NVME_TCP_DIGEST_LENGTH : 0;
}

static inline u8 nvmet_tcp_ddgst_len(struct nvmet_tcp_queue *queue)
{
	return queue->data_digest ? NVME_TCP_DIGEST_LENGTH : 0;
}

static inline void nvmet_tcp_hdgst(void *pdu, size_t len)
{
	put_unaligned_le32(~crc32c(~0, pdu, len), pdu + len);
}

static int nvmet_tcp_verify_hdgst(struct nvmet_tcp_queue *queue,
	void *pdu, size_t len)
{
	struct nvme_tcp_hdr *hdr = pdu;
	__le32 recv_digest;
	__le32 exp_digest;

	if (unlikely(!(hdr->flags & NVME_TCP_F_HDGST))) {
		pr_err("queue %d: header digest enabled but no header digest\n",
			queue->idx);
		return -EPROTO;
	}

	recv_digest = *(__le32 *)(pdu + hdr->hlen);
	nvmet_tcp_hdgst(pdu, len);
	exp_digest = *(__le32 *)(pdu + hdr->hlen);
	if (recv_digest != exp_digest) {
		pr_err("queue %d: header digest error: recv %#x expected %#x\n",
			queue->idx, le32_to_cpu(recv_digest),
			le32_to_cpu(exp_digest));
		return -EPROTO;
	}

	return 0;
}

static int nvmet_tcp_check_ddgst(struct nvmet_tcp_queue *queue, void *pdu)
{
	struct nvme_tcp_hdr *hdr = pdu;
	u8 digest_len = nvmet_tcp_hdgst_len(queue);
	u32 len;

	len = le32_to_cpu(hdr->plen) - hdr->hlen -
		(hdr->flags & NVME_TCP_F_HDGST ? digest_len : 0);

	if (unlikely(len && !(hdr->flags & NVME_TCP_F_DDGST))) {
		pr_err("queue %d: data digest flag is cleared\n", queue->idx);
		return -EPROTO;
	}

	return 0;
}

static void nvmet_tcp_release_queue(struct kref *kref);
static void nvmet_tcp_queue_response(struct nvmet_req *req);

/*
 * rx_loan region receive.
 *
 * A queue receives WRITE data into region loans (struct rx_loan_table) when
 * a fixed receive region exists and neither TLS nor a data digest is in
 * use; every other queue, and every command whose backend cannot take a
 * bvec, takes the stock path unchanged.  Only init_net: the transport
 * registers one consumer, on init_net, at its first queue that could lend
 * (so a loaded transport that never had one leaves every other consumer's
 * share of the region as it was; once registered it stays until unload),
 * and queues in other namespaces stay on the stock
 * path.  Its queues draw on a write budget of their own
 * (nvmet_tcp_rx_budget), the region's write share, which no sunrpc setting
 * turns off; kNFSd's budgets are its own and unchanged by it.  The region
 * caps every budget together at its size, and nvmet-tcp's admissions
 * (write_capped) at its write partition with kNFSd's write credit counted,
 * so nvmet-tcp never takes write credit past it; kNFSd's, uncapped as
 * before, still can.
 *
 * Admission sits at R2T time.  One command owns the queue's unit
 * (queue->rx_unit) from its first R2T until all its data is in, as kNFSd
 * takes one record at a time per socket: its later R2Ts need no admission,
 * so the oldest record on the queue always completes.  The R2Ts of other
 * commands wait on queue->rx_withheld in arrival order, while the queue's
 * one waiter holds its place on the budget's FIFO.  Each R2T solicits at
 * most what the budget's current reserve covers (the reserve rule);
 * maxh2cdata stays stock.  A record is admitted at its estimated cost and
 * charged per skb as it is received; a charge that cannot be met stops
 * reading the socket (backpressure, never a drop) until the waiter is
 * woken.  The credit and the borrowed pages are returned when the backend
 * is done with the data (->data_done), else when the command is freed.
 */
static struct rx_loan_consumer nvmet_tcp_rx_consumer;
static struct rx_loan_budget nvmet_tcp_rx_budget;
static DEFINE_MUTEX(nvmet_tcp_rx_consumer_lock);
static bool nvmet_tcp_rx_consumer_on;

/* Register the consumer, once, at the first queue that could lend. */
static void nvmet_tcp_rx_consumer_start(void)
{
	mutex_lock(&nvmet_tcp_rx_consumer_lock);
	if (!nvmet_tcp_rx_consumer_on) {
		/* Region demand only: the region is carved at a queue's first use. */
		rx_loan_consumer_register(&nvmet_tcp_rx_consumer,
					  rx_loan_net(&init_net), "nvmet-tcp",
					  NVMET_TCP_RX_FLOOR);
		nvmet_tcp_rx_consumer_on = true;
	}
	mutex_unlock(&nvmet_tcp_rx_consumer_lock);
}

/*
 * A write budget of the region's write share, its own, as no namespace's
 * (rx_loan_region_write_default()).  @lb is zeroed: the default is set
 * before rx_loan_budget_init() lists it for region kicks.
 */
static int nvmet_tcp_rx_budget_init(struct rx_loan_budget *lb)
{
	WRITE_ONCE(lb->cfg_default, true);
	/* kNFSd's write budget is the write share too: share the partition. */
	lb->write_capped = true;
	return rx_loan_budget_init(lb, false);
}

static int nvmet_tcp_rx_budget_stats_get(char *buf,
					 const struct kernel_param *kp)
{
	struct rx_loan_budget *lb = &nvmet_tcp_rx_budget;
	struct rx_loan_stock_sum sum;
	unsigned int waiters = 0;
	struct rx_loan_waiter *pos;
	int len;

	/* Readable before init and after exit: the lock is set up first. */
	if (!smp_load_acquire(&lb->stock))
		return scnprintf(buf, PAGE_SIZE, "none\n");
	spin_lock_bh(&lb->lock);
	if (!lb->stock) {
		spin_unlock_bh(&lb->lock);
		return scnprintf(buf, PAGE_SIZE, "none\n");
	}
	list_for_each_entry(pos, &lb->waiters, node)
		waiters++;
	/* Stocked credit is available; stocked records are in flight. */
	rx_loan_stock_sum(lb, &sum);
	len = scnprintf(buf, PAGE_SIZE,
			"budget %u reserve_size %u avail %ld reserve %ld inflight %ld waiters %u admitted %llu denied %llu paused %llu overrun %llu orphaned %llu cost_factor %u hold_factor %u lent %u\n",
			lb->budget, lb->reserve_size, lb->avail + sum.pages,
			lb->reserve, (long)lb->inflight_count + sum.records,
			waiters, lb->admitted + sum.admitted, lb->denied,
			lb->paused, lb->overrun, lb->orphaned, lb->cost_factor,
			READ_ONCE(lb->hold_factor),
			lb->lent + (unsigned int)sum.lent);
	spin_unlock_bh(&lb->lock);
	return len;
}

static const struct kernel_param_ops nvmet_tcp_rx_budget_stats_ops = {
	.get = nvmet_tcp_rx_budget_stats_get,
};
module_param_cb(rx_budget_stats, &nvmet_tcp_rx_budget_stats_ops, NULL, 0444);
MODULE_PARM_DESC(rx_budget_stats,
		 "State of nvmet-tcp's rx region write budget");

enum {
	NVMET_TCP_RX_GO,
	NVMET_TCP_RX_WAIT,	/* a charge paused: the waiter is queued */
	NVMET_TCP_RX_COPY,	/* not borrowable: copy, the stock way */
};

struct nvmet_tcp_rx_desc {
	struct nvmet_tcp_cmd	*cmd;
	int			stop;
};

static u32 nvmet_tcp_rx_entries(u32 bytes)
{
	/* DIV_ROUND_UP() would wrap for @bytes near U32_MAX. */
	return bytes / NVMET_TCP_RX_ENTRY_BYTES +
	       !!(bytes % NVMET_TCP_RX_ENTRY_BYTES) + 2;
}

/**
 * nvmet_tcp_rx_unit_max() - bytes one R2T may solicit
 * @reserve: the budget's reserve, in region pages
 * @factor: learned region pages per 100 pages of data, 0 while unlearned
 *
 * The reserve rule: a unit is sized so that its admission cost never
 * exceeds the reserve.  Only the oldest record may draw the reserve, and it
 * always may, so a unit that fits the reserve always completes once it is
 * the oldest, whatever else holds credit.  The reserve is shared and set
 * by the budget's configuration, so nvmet sizes its units to fit it, from
 * its current value at each R2T, rather than the other way round.
 *
 * Return: the unit, page aligned, within [PAGE_SIZE, NVMET_TCP_MAXH2CDATA].
 */
static u32 nvmet_tcp_rx_unit_max(unsigned int reserve, unsigned int factor)
{
	u64 bytes;

	if (factor)
		bytes = div_u64((u64)(reserve > 1 ? reserve - 1 : 0) * 100,
				factor) << PAGE_SHIFT;
	else
		bytes = (u64)(reserve > 2 ? reserve - 2 : 0) *
			NVMET_TCP_RX_ENTRY_BYTES;
	bytes = round_down(bytes, PAGE_SIZE);
	return clamp_t(u64, bytes, PAGE_SIZE, NVMET_TCP_MAXH2CDATA);
}

/* The unit the budget's reserve covers now. */
static u32 nvmet_tcp_rx_unit(struct rx_loan_budget *lb)
{
	return nvmet_tcp_rx_unit_max(READ_ONCE(lb->reserve_size),
				     READ_ONCE(lb->cost_factor));
}

/* A unit's admission cost, as kNFSd's, at most a table's @cap entries. */
static u32 nvmet_tcp_rx_cost(u32 bytes, unsigned int factor, u32 cap)
{
	u64 cost = nvmet_tcp_rx_entries(bytes);

	if (factor)
		cost = min_t(u64, cost,
			     DIV_ROUND_UP_ULL((u64)DIV_ROUND_UP(bytes, PAGE_SIZE) *
					      factor, 100) + 1);
	return min_t(u64, cost, cap);
}

/* The cost sample a returning record teaches the budget (as kNFSd's). */
static unsigned int nvmet_tcp_rx_sample(const struct rx_loan_table *lt)
{
	u32 pages = DIV_ROUND_UP(lt->body_bytes, PAGE_SIZE);
	unsigned int sample;

	if (!lt->region_count || !pages)
		return 0;
	sample = min_t(u64, DIV_ROUND_UP_ULL((u64)(lt->region_count - 1) * 100,
					     pages),
		       NVMET_TCP_RX_FACTOR_MAX);
	return max(sample, 100U);
}

/*
 * Is the queue being released?  Then it admits nothing more: a refused
 * admission would queue its waiter again after release took it off the
 * budget's FIFO.
 */
static bool nvmet_tcp_rx_dying(struct nvmet_tcp_queue *queue)
{
	return READ_ONCE(queue->state) == NVMET_TCP_Q_DISCONNECTING ||
	       READ_ONCE(queue->rcv_state) == NVMET_TCP_RECV_ERR;
}

static bool nvmet_tcp_rx_pin(struct rx_loan_waiter *w)
{
	struct nvmet_tcp_queue *queue =
		container_of(w, struct nvmet_tcp_queue, rx_wait);

	/* Declined once release has begun: it unwaits before freeing. */
	return kref_get_unless_zero(&queue->kref);
}

static void nvmet_tcp_rx_wake(struct rx_loan_waiter *w)
{
	struct nvmet_tcp_queue *queue =
		container_of(w, struct nvmet_tcp_queue, rx_wait);

	/* queue_work_on() orders the store before io_work reads it. */
	WRITE_ONCE(queue->rx_kicked, true);
	queue_work_on(queue_cpu(queue), nvmet_tcp_wq, &queue->io_work);
	kref_put(&queue->kref, nvmet_tcp_release_queue);
}

static const struct rx_loan_waiter_ops nvmet_tcp_rx_waiter_ops = {
	.pin	= nvmet_tcp_rx_pin,
	.wake	= nvmet_tcp_rx_wake,
};

/*
 * May the queue retry admission or a charge?  Yes unless its waiter is
 * queued and has not been woken since: retrying then only re-queues it.
 */
static bool nvmet_tcp_rx_retry(struct nvmet_tcp_queue *queue)
{
	if (!rx_loan_waiting(&queue->rx_wait))
		return true;
	if (!READ_ONCE(queue->rx_kicked))
		return false;
	WRITE_ONCE(queue->rx_kicked, false);
	return true;
}

/* @cmd gives up the queue's unit: let the oldest withheld R2T try. */
static void nvmet_tcp_rx_unit_free(struct nvmet_tcp_queue *queue,
				   struct nvmet_tcp_cmd *cmd)
{
	if (queue->rx_unit != cmd)
		return;
	queue->rx_unit = NULL;
	/* Not once release has begun: io_work must not run again. */
	if (!list_empty(&queue->rx_withheld) && !nvmet_tcp_rx_dying(queue)) {
		WRITE_ONCE(queue->rx_kicked, true);
		queue_work_on(queue_cpu(queue), nvmet_tcp_wq, &queue->io_work);
	}
}

/*
 * Return @cmd's borrowed pages and its credit.  Idempotent: the table's
 * borrowed bits and cr.linked say what is still held, so a later call (the
 * free after ->data_done) returns nothing twice.
 */
static void nvmet_tcp_rx_return(struct nvmet_tcp_cmd *cmd)
{
	unsigned int sample = nvmet_tcp_rx_sample(&cmd->lt);
	u32 held = cmd->lt.region_count;
	struct rx_loan_lend out = {};

	/*
	 * Never put a page the table cannot account for: orphan the record,
	 * its pages and credit stay out.
	 */
	if (!rx_loan_table_refs_valid(&cmd->lt) ||
	    rx_loan_table_release(&cmd->lt)) {
		if (cmd->cr.linked)
			rx_loan_orphan(&cmd->cr, held);
	} else if (cmd->cr.linked) {
		rx_loan_return(&cmd->cr, sample, 0, &out);
	}
	rx_loan_table_reset_consumer(&cmd->lt, &nvmet_tcp_rx_consumer);
}

/* Return what @cmd holds, free its table: it is not loaned any more. */
static void nvmet_tcp_rx_drop(struct nvmet_tcp_cmd *cmd)
{
	nvmet_tcp_rx_return(cmd);
	rx_loan_table_free(&cmd->lt);
	memset(&cmd->lt, 0, sizeof(cmd->lt));
	cmd->flags &= ~NVMET_TCP_F_RX_LOAN;
}

/* Returns ->data_done deferred from hard irq; io_work or teardown. */
static void nvmet_tcp_rx_drain_done(struct nvmet_tcp_queue *queue)
{
	struct llist_node *node = llist_del_all(&queue->rx_done);
	struct nvmet_tcp_cmd *cmd, *tmp;

	llist_for_each_entry_safe(cmd, tmp, node, rx_done_node) {
		cmd->flags &= ~NVMET_TCP_F_RX_DEFER;
		nvmet_tcp_rx_return(cmd);
	}
}

/* The command is freed: return what it holds and free its table. */
static void nvmet_tcp_rx_put(struct nvmet_tcp_cmd *cmd)
{
	struct nvmet_tcp_queue *queue = cmd->queue;

	/*
	 * A deferred return was queued before the command's response (the
	 * llist_add() is a full barrier), so it is on rx_done by now.
	 */
	if (cmd->flags & NVMET_TCP_F_RX_DEFER)
		nvmet_tcp_rx_drain_done(queue);
	nvmet_tcp_rx_unit_free(queue, cmd);
	nvmet_tcp_rx_drop(cmd);
}

/**
 * nvmet_tcp_data_done() - the backend is done with a WRITE's data
 * @req: the request whose data was lent (nvmet_req_set_data_bvec())
 *
 * Return the region loans and the credit of a command received into a
 * loan table, once.  The backend's completion may run in hard irq (bio
 * end_io), where the budget's lock (taken with bottom halves disabled)
 * cannot be: there the return is handed to io_work.  Setting
 * NVMET_TCP_F_RX_DEFER from hard irq is safe because io_work no longer
 * writes cmd->flags once the command has been executed.
 */
static void nvmet_tcp_data_done(struct nvmet_req *req)
{
	struct nvmet_tcp_cmd *cmd = container_of(req, struct nvmet_tcp_cmd, req);
	struct nvmet_tcp_queue *queue = cmd->queue;

	if (!(cmd->flags & NVMET_TCP_F_RX_LOAN) ||
	    (cmd->flags & NVMET_TCP_F_RX_DEFER))
		return;
	if (in_hardirq() || irqs_disabled()) {
		cmd->flags |= NVMET_TCP_F_RX_DEFER;
		llist_add(&cmd->rx_done_node, &queue->rx_done);
		queue_work_on(queue_cpu(queue), nvmet_tcp_wq, &queue->io_work);
		return;
	}
	nvmet_tcp_rx_return(cmd);
}

/*
 * Copy fallback: move @cmd to the stock sgl and return its loans and
 * credit.  What it has borrowed so far is laid out page-tiled in the sgl's
 * pages by rx_loan_tile(), each loan returned as soon as it is consumed; a
 * table whose references do not check is copied entry by entry and
 * orphaned (nvmet_tcp_rx_return()).  Whether all its data is in or the
 * stock path receives the rest, the command counts as one record that
 * copied transfer_len bytes, to nvmet-tcp's copies and the host-wide total,
 * at the table's reset.  The backend then takes the sgl: one that refused
 * the loan's layout (nvmet_tcp_rx_finish()) takes it as stock does.  On
 * failure @cmd is unchanged.
 */
static int nvmet_tcp_rx_to_sgl(struct nvmet_tcp_cmd *cmd)
{
	struct rx_loan_table *lt = &cmd->lt;
	struct scatterlist *sg;
	u32 skip = 0;
	int i;

	cmd->req.sg = sgl_alloc(cmd->req.transfer_len, GFP_KERNEL | __GFP_NOWARN,
				&cmd->req.sg_cnt);
	if (!cmd->req.sg)
		return -ENOMEM;
	cmd->iov = kmalloc_objs(*cmd->iov, cmd->req.sg_cnt,
				GFP_KERNEL | __GFP_NOWARN);
	if (!cmd->iov) {
		sgl_free(cmd->req.sg);
		cmd->req.sg = NULL;
		return -ENOMEM;
	}
	if (lt->body_bytes && rx_loan_table_refs_valid(lt)) {
		/*
		 * sgl_alloc() gives whole pages in data order: tile into them
		 * from offset 0.  cmd->iov is scratch until the receive of the
		 * rest, if any, sets it again (nvmet_tcp_rx_sgl_iovec()).
		 */
		for_each_sg(cmd->req.sg, sg, cmd->req.sg_cnt, i)
			cmd->iov[i].bv_page = sg_page(sg);
		rx_loan_tile(lt, 0, 0, lt->body_bytes, NULL, cmd->iov, 0,
			     cmd->iov, NULL);
	} else {
		for (i = 0; i < lt->count; i++) {
			struct bio_vec *bv = &lt->bvec[i];
			void *p = bvec_kmap_local(bv);

			sg_pcopy_from_buffer(cmd->req.sg, cmd->req.sg_cnt, p,
					     bv->bv_len, skip);
			kunmap_local(p);
			skip += bv->bv_len;
		}
	}
	nvmet_tcp_rx_return(cmd);
	/* One record: the bytes tiled and those the stock path receives. */
	lt->copied_bytes = cmd->req.transfer_len;
	nvmet_tcp_rx_drop(cmd);
	return 0;
}

/* After a fallback mid-PDU: receive the PDU's rest into the sgl. */
static int nvmet_tcp_rx_sgl_iovec(struct nvmet_tcp_cmd *cmd)
{
	u32 left = cmd->pdu_len - cmd->pdu_recv, skip = cmd->rbytes_done;
	struct scatterlist *sg;
	int i, nr = 0;

	for_each_sg(cmd->req.sg, sg, cmd->req.sg_cnt, i) {
		u32 take;

		if (!left)
			break;
		if (skip >= sg->length) {
			skip -= sg->length;
			continue;
		}
		take = min(left, sg->length - skip);
		bvec_set_page(&cmd->iov[nr++], sg_page(sg), take,
			      sg->offset + skip);
		left -= take;
		skip = 0;
	}
	if (left)
		return -EPROTO;
	iov_iter_bvec(&cmd->recv_msg.msg_iter, ITER_DEST, cmd->iov, nr,
		      cmd->pdu_len - cmd->pdu_recv);
	return 0;
}

/*
 * No memory for the copy fallback: return what @cmd holds and sink the
 * rest of what was solicited from it (to r2t_end), then fail it alone
 * (nvmet_tcp_rx_unit_done()).  The queue goes on.  No further R2T is sent
 * for it; a later H2CData PDU for its ttag finds no buffers and errors
 * the queue, as stock does for a host that sends unsolicited data.
 */
static void nvmet_tcp_rx_sink(struct nvmet_tcp_cmd *cmd)
{
	if (cmd->flags & NVMET_TCP_F_RX_LOAN)
		nvmet_tcp_rx_drop(cmd);
	cmd->flags |= NVMET_TCP_F_RX_SINK;
	iov_iter_discard(&cmd->recv_msg.msg_iter, ITER_DEST,
			 cmd->pdu_len - cmd->pdu_recv);
}

/* Mid-PDU: go on with the stock sgl, else sink. */
static int nvmet_tcp_rx_fallback(struct nvmet_tcp_cmd *cmd)
{
	if (!nvmet_tcp_rx_to_sgl(cmd))
		return nvmet_tcp_rx_sgl_iovec(cmd);
	nvmet_tcp_rx_sink(cmd);
	return 0;
}

/*
 * Take the queue's unit for @cmd's next R2T, admitting it if it holds no
 * credit yet.  0: the R2T may go out (loaned, or moved to the stock path
 * because the budget was turned off); -EAGAIN: the waiter waits.
 */
static int nvmet_tcp_rx_take_unit(struct nvmet_tcp_cmd *cmd)
{
	struct nvmet_tcp_queue *queue = cmd->queue;
	struct rx_loan_budget *lb = queue->rx_budget;
	unsigned int factor;
	u32 bytes;

	if (unlikely(nvmet_tcp_rx_dying(queue))) {
		/* Off the FIFO now: a dying queue's waiter would block it. */
		rx_loan_unwait(&queue->rx_wait);
		return -EAGAIN;
	}
	if (unlikely(!READ_ONCE(lb->budget))) {
		rx_loan_unwait(&queue->rx_wait);
		if (!nvmet_tcp_rx_to_sgl(cmd))
			return 0;
		/*
		 * Earlier units are all in and the next R2T was never sent,
		 * so nothing more can arrive for it: fail it alone.
		 */
		nvmet_tcp_rx_unit_free(queue, cmd);
		nvmet_req_complete(&cmd->req, NVME_SC_INTERNAL);
		return -ENOMEM;
	}
	factor = READ_ONCE(lb->cost_factor);
	bytes = min(cmd->req.transfer_len - cmd->rbytes_done,
		    nvmet_tcp_rx_unit_max(READ_ONCE(lb->reserve_size), factor));
	if (!cmd->cr.linked &&
	    !rx_loan_admit(lb, &nvmet_tcp_rx_consumer, &queue->rx_wait,
			   &cmd->cr,
			   nvmet_tcp_rx_cost(bytes, factor, cmd->lt.capacity),
			   rx_loan_cost_warm(lb)))
		return -EAGAIN;
	cmd->r2t_end = cmd->rbytes_done + bytes;
	queue->rx_unit = cmd;
	return 0;
}

/*
 * May @cmd's R2T go out now?  False: it was withheld (or failed and was
 * completed) and the next response is tried.  The unit's owner always may;
 * another command only when the unit is free and nothing older is
 * withheld.
 */
static bool nvmet_tcp_rx_r2t_ok(struct nvmet_tcp_cmd *cmd)
{
	struct nvmet_tcp_queue *queue = cmd->queue;
	int ret;

	if (!(cmd->flags & NVMET_TCP_F_RX_LOAN))
		return true;
	if (queue->rx_unit == cmd ||
	    (!queue->rx_unit && list_empty(&queue->rx_withheld))) {
		ret = nvmet_tcp_rx_take_unit(cmd);
		if (ret != -EAGAIN)
			return !ret;
	}
	/*
	 * Arrival order: the head is the one the waiter queued for.  (An
	 * owner refused because the queue is dying stays here unserved.)
	 */
	list_add_tail(&cmd->entry, &queue->rx_withheld);
	return false;
}

/* The oldest withheld R2T, once the unit is free and it is admitted. */
static struct nvmet_tcp_cmd *
nvmet_tcp_rx_next_withheld(struct nvmet_tcp_queue *queue)
{
	struct nvmet_tcp_cmd *cmd;
	int ret;

	if (unlikely(nvmet_tcp_rx_dying(queue))) {
		rx_loan_unwait(&queue->rx_wait);
		return NULL;
	}
	cmd = list_first_entry_or_null(&queue->rx_withheld,
				       struct nvmet_tcp_cmd, entry);
	if (!cmd || queue->rx_unit || !nvmet_tcp_rx_retry(queue))
		return NULL;
	ret = nvmet_tcp_rx_take_unit(cmd);
	if (ret == -EAGAIN)
		return NULL;
	/* Taken, or failed and completed: it is withheld no longer. */
	list_del_init(&cmd->entry);
	return ret ? NULL : cmd;
}

/*
 * A PDU of @cmd is in.  At the end of what was solicited, solicit more
 * (the owner keeps the unit) or free the unit.  True: @cmd was sunk and is
 * failed here, or is still sinking; it must not be executed.
 */
static bool nvmet_tcp_rx_unit_done(struct nvmet_tcp_cmd *cmd)
{
	struct nvmet_tcp_queue *queue = cmd->queue;

	if (unlikely(cmd->flags & NVMET_TCP_F_RX_SINK)) {
		if (cmd->rbytes_done < cmd->r2t_end)
			return true;
		cmd->flags &= ~NVMET_TCP_F_RX_SINK;
		nvmet_tcp_rx_unit_free(queue, cmd);
		nvmet_req_complete(&cmd->req, NVME_SC_INTERNAL);
		return true;
	}
	if (queue->rx_unit != cmd || cmd->rbytes_done < cmd->r2t_end)
		return false;
	if (cmd->rbytes_done < cmd->req.transfer_len)
		nvmet_tcp_queue_response(&cmd->req);
	else
		nvmet_tcp_rx_unit_free(queue, cmd);
	return false;
}

/* Can bytes [@offset, @offset + @len) of @skb be borrowed in place? */
static bool nvmet_tcp_rx_borrowable(struct sk_buff *skb, unsigned int offset,
				    unsigned int len)
{
	unsigned int cursor = skb_headlen(skb), i;

	if (skb_has_frag_list(skb) || skb_zcopy(skb) ||
	    !skb_frags_readable(skb))
		return false;
	/* A kmalloc'ed or shared head cannot be lent. */
	if (offset < cursor && skb_head_is_locked(skb))
		return false;
	for (i = 0; i < skb_shinfo(skb)->nr_frags && cursor < offset + len;
	     i++) {
		const skb_frag_t *frag = &skb_shinfo(skb)->frags[i];

		if (cursor + skb_frag_size(frag) > offset &&
		    skb_frag_is_net_iov(frag))
			return false;
		cursor += skb_frag_size(frag);
	}
	return true;
}

/* Borrow @len bytes at @offset of @page, a page at a time. */
static int nvmet_tcp_rx_borrow(struct rx_loan_table *lt, struct page *page,
			       unsigned int offset, unsigned int len,
			       bool pp_recycle, unsigned int *done)
{
	while (len) {
		struct page *p = page + (offset >> PAGE_SHIFT);
		unsigned int off = offset_in_page(offset);
		unsigned int take = min_t(unsigned int, len, PAGE_SIZE - off);
		int ret;

		ret = rx_loan_table_append(lt, p, off, take, true, pp_recycle &&
					   page_pool_page_is_pp(compound_head(p)));
		if (ret)
			return ret;
		offset += take;
		len -= take;
		*done += take;
	}
	return 0;
}

/*
 * Loan entries bytes [@offset, @offset + @len) of @skb take, at most: one
 * per page piece, as nvmet_tcp_rx_borrow() appends them.
 */
static unsigned int nvmet_tcp_rx_span_entries(const struct sk_buff *skb,
					      unsigned int offset,
					      unsigned int len)
{
	unsigned int cursor = skb_headlen(skb), end = offset + len, n = 0, i;

	if (offset < cursor)
		n += DIV_ROUND_UP(offset_in_page(skb->data + offset) +
				  min(end, cursor) - offset, PAGE_SIZE);
	for (i = 0; i < skb_shinfo(skb)->nr_frags && cursor < end; i++) {
		const skb_frag_t *frag = &skb_shinfo(skb)->frags[i];
		unsigned int size = skb_frag_size(frag), s, e;

		if (cursor + size > offset) {
			s = max(offset, cursor) - cursor;
			e = min(end, cursor + size) - cursor;
			n += DIV_ROUND_UP(offset_in_page(skb_frag_off(frag) + s) +
					  e - s, PAGE_SIZE);
		}
		cursor += size;
	}
	return n;
}

/*
 * Receive-buffer holds, as kNFSd's (rx_loan_hold_*()).  A queue that
 * receives region pages holds its receive buffer to NVMET_TCP_RX_RCVBUF,
 * at what its skbs were measured to occupy, so its unread data stays its
 * share of the region.  The port's listener is held too, once the region
 * is in use: TCP sizes a connection's first window from it (accept_work).
 * A queue that will never lend gives its hold back and keeps TCP's own
 * buffer (nvmet_tcp_rx_setup()).
 */
static void nvmet_tcp_rx_hold_apply(struct rx_loan_hold *h, struct sock *sk,
				    struct rx_loan_budget *lb)
{
	int rcvbuf, clamp, err;

	err = rx_loan_hold_apply(h, sk, NVMET_TCP_RX_RCVBUF,
				 rx_loan_hold_factor(h, lb), &rcvbuf, &clamp);
	if (err && err != -EALREADY)
		pr_warn_once("receive window clamp failed\n");
}

/*
 * The actor saw @skb's bytes: mark the hold to apply once region pages are
 * seen (@region), or again once the factor it was held to is stale; and
 * measure what the skb occupies for the @used bytes read.  Applied by
 * nvmet_tcp_rx_recv() with the socket unlocked.
 */
static void nvmet_tcp_rx_hold_account(struct nvmet_tcp_queue *queue,
				      struct sk_buff *skb, unsigned int used,
				      bool region)
{
	struct rx_loan_hold *h = &queue->rx_hold;

	if (!h->held && !h->pending && region) {
		h->pending = true;
	} else if (h->held &&
		   rx_loan_hold_stale(h, rx_loan_hold_factor(h, queue->rx_budget))) {
		h->held = false;
		h->pending = true;
	}
	if (!used || !(h->held || h->pending))
		return;
	/* A new sample is where a queue of the budget that has none is held. */
	if (rx_loan_hold_account(h, skb->truesize, skb->len, used))
		WRITE_ONCE(queue->rx_budget->hold_factor,
			   rx_loan_hold_measured(h));
}

/* read_sock actor: borrow the PDU's bytes of @skb into the loan table. */
static int nvmet_tcp_rx_actor(read_descriptor_t *desc, struct sk_buff *skb,
			      unsigned int offset, size_t len)
{
	struct nvmet_tcp_rx_desc *rd = desc->arg.data;
	struct nvmet_tcp_cmd *cmd = rd->cmd;
	struct rx_loan_table *lt = &cmd->lt;
	struct page *prev = lt->region_last;
	unsigned int want = min_t(size_t, len, desc->count);
	struct rx_loan_waiter *w = &cmd->queue->rx_wait;
	bool owner = cmd->queue->rx_unit == cmd;
	unsigned int cursor, done = 0, i;
	int ret = 0;
	u32 need;

	/* Charge only what the table has room for: past it, copy. */
	if (!nvmet_tcp_rx_borrowable(skb, offset, want) ||
	    lt->count + nvmet_tcp_rx_span_entries(skb, offset, want) >
	    lt->capacity) {
		rd->stop = NVMET_TCP_RX_COPY;
		goto stop;
	}
	/*
	 * The waiter queued holds another record's place (the owner's paused
	 * charge, or a withheld R2T): a charge through it, failed or not,
	 * would change that place.  Copy without charging.
	 */
	if (!owner && rx_loan_waiting(w)) {
		rd->stop = NVMET_TCP_RX_COPY;
		goto stop;
	}
	need = rx_loan_skb_region_pages(skb, 0, offset, offset + want, &prev);
	if (!rx_loan_charge(w, &cmd->cr, lt->region_count + need)) {
		/*
		 * Only the unit's owner may wait: another record's data may
		 * sit in front of the owner's, which the credit waits for.
		 */
		if (owner) {
			rd->stop = NVMET_TCP_RX_WAIT;
			goto stop;
		}
		rx_loan_unwait(w);
		rd->stop = NVMET_TCP_RX_COPY;
		goto stop;
	}
	cursor = skb_headlen(skb);
	if (offset < cursor) {
		const unsigned char *data = skb->data + offset;

		ret = nvmet_tcp_rx_borrow(lt, virt_to_page(data),
					  offset_in_page(data),
					  min(want, cursor - offset),
					  skb->pp_recycle, &done);
	}
	for (i = 0; !ret && done < want && i < skb_shinfo(skb)->nr_frags; i++) {
		const skb_frag_t *frag = &skb_shinfo(skb)->frags[i];
		unsigned int size = skb_frag_size(frag);
		unsigned int start = offset + done;

		if (start < cursor + size)
			ret = nvmet_tcp_rx_borrow(lt, skb_frag_page(frag),
						  skb_frag_off(frag) + start - cursor,
						  min(cursor + size - start,
						      want - done),
						  skb->pp_recycle, &done);
		cursor += size;
	}
	/* -ENOSPC: the table is full; copy the rest. */
	if (ret)
		rd->stop = NVMET_TCP_RX_COPY;
	nvmet_tcp_rx_hold_account(cmd->queue, skb, done, need);
	cmd->pdu_recv += done;
	cmd->rbytes_done += done;
	desc->count = rd->stop ? 0 : desc->count - done;
	return done;
stop:
	desc->count = 0;
	return 0;
}

/*
 * In-capsule data is admitted as it arrives.  It may wait for credit only
 * when no command on this queue holds credit with data still to come,
 * that is when no command owns the unit: then all this queue holds is
 * received and will be returned.  Otherwise the owner's data may be queued
 * behind this capsule's, so waiting could deadlock: admit at the table's
 * whole cost now or copy.  It never goes through the queue's waiter while
 * that is queued for a withheld R2T: admission would take its place.
 * Return: 0 admitted, 1 moved off the loan path (iterator ready), else an
 * error or -EAGAIN (waiting).
 */
static int nvmet_tcp_rx_admit_capsule(struct nvmet_tcp_cmd *cmd)
{
	struct nvmet_tcp_queue *queue = cmd->queue;
	struct rx_loan_budget *lb = queue->rx_budget;
	bool queued = rx_loan_waiting(&queue->rx_wait);
	bool wait = !queue->rx_unit;
	u32 cost = cmd->lt.capacity;
	int ret;

	cmd->r2t_end = cmd->pdu_len;
	if (unlikely(nvmet_tcp_rx_dying(queue))) {
		rx_loan_unwait(&queue->rx_wait);
		return -EAGAIN;
	}
	if (!READ_ONCE(lb->budget)) {
		rx_loan_unwait(&queue->rx_wait);
		goto copy;
	}
	/*
	 * The in-capsule reserve: data beyond one unit is copied, and counted
	 * (nvmet_tcp_rx_want() refused it then; the unit may have shrunk).
	 */
	if (cmd->pdu_len > nvmet_tcp_rx_unit(lb))
		goto copy;
	if (!wait && queued)
		goto copy;
	if (wait)
		cost = nvmet_tcp_rx_cost(cmd->pdu_len,
					 READ_ONCE(lb->cost_factor), cost);
	if (rx_loan_admit(lb, &nvmet_tcp_rx_consumer, &queue->rx_wait,
			  &cmd->cr, cost, rx_loan_cost_warm(lb))) {
		if (wait)
			queue->rx_unit = cmd;
		return 0;
	}
	if (wait)
		return -EAGAIN;
	rx_loan_unwait(&queue->rx_wait);
copy:
	ret = nvmet_tcp_rx_fallback(cmd);
	return ret ?: 1;
}

/*
 * Receive the current PDU of a loaned command.  0: the PDU is in, or the
 * command moved off the loan path (its iterator set for the PDU's rest);
 * -EAGAIN: no data, or waiting for credit (backpressure).
 */
static int nvmet_tcp_rx_recv(struct nvmet_tcp_queue *queue,
			     struct nvmet_tcp_cmd *cmd)
{
	struct nvmet_tcp_rx_desc rd = { .cmd = cmd };
	read_descriptor_t desc = { .arg.data = &rd };
	struct sock *sk = queue->sock->sk;
	int ret;

	if (!cmd->cr.linked) {
		/* The admit-or-copy path never waits: no gate for it. */
		if (!queue->rx_unit && !nvmet_tcp_rx_retry(queue))
			return -EAGAIN;
		ret = nvmet_tcp_rx_admit_capsule(cmd);
		if (ret)
			return ret > 0 ? 0 : ret;
	} else if (queue->rx_unit == cmd && !nvmet_tcp_rx_retry(queue)) {
		/* Only the unit's owner can have paused on a charge. */
		return -EAGAIN;
	}
	while (cmd->pdu_recv < cmd->pdu_len) {
		rd.stop = NVMET_TCP_RX_GO;
		desc.count = cmd->pdu_len - cmd->pdu_recv;
		lock_sock(sk);
		ret = queue->sock->ops->read_sock(sk, &desc, nvmet_tcp_rx_actor);
		release_sock(sk);
		if (queue->rx_hold.pending)
			nvmet_tcp_rx_hold_apply(&queue->rx_hold, sk,
						queue->rx_budget);
		if (rd.stop == NVMET_TCP_RX_COPY)
			return nvmet_tcp_rx_fallback(cmd);
		if (ret < 0)
			return ret;
		if (rd.stop == NVMET_TCP_RX_WAIT || !ret)
			return -EAGAIN;
	}
	return 0;
}

/*
 * All of @cmd's data is in: lend the table to the request, or copy.
 * Nonzero: no memory to copy; @cmd was failed alone, do not execute it.
 */
static int nvmet_tcp_rx_finish(struct nvmet_tcp_cmd *cmd)
{
	if (!(cmd->flags & NVMET_TCP_F_RX_LOAN))
		return 0;
	if (nvmet_req_set_data_bvec(&cmd->req, cmd->lt.bvec, cmd->lt.count))
		return 0;
	/*
	 * The backend cannot take the loan's layout (an unaligned piece, or
	 * an sgl-only command): tile the loans into the stock sgl, a counted
	 * copy (nvmet_tcp_rx_to_sgl()).  The holds are applied as the data
	 * arrives (nvmet_tcp_rx_recv()).
	 */
	if (!nvmet_tcp_rx_to_sgl(cmd))
		return 0;
	nvmet_tcp_rx_drop(cmd);
	nvmet_req_complete(&cmd->req, NVME_SC_INTERNAL);
	return -ENOMEM;
}

/* At ICReq: does this queue receive into region loans? */
static void nvmet_tcp_rx_setup(struct nvmet_tcp_queue *queue)
{
	struct rx_loan_budget *lb = &nvmet_tcp_rx_budget;

	/* TLS and the data digest need the bytes in buffers of their own. */
	if (queue->data_digest || queue->tls_pskid ||
	    !net_eq(sock_net(queue->sock->sk), &init_net))
		goto stock;
	/*
	 * Process context, before the queue's first admission: carve the
	 * region, then size the write budget from it.  The budget is only
	 * sized on admission, which this queue would never reach at 0.
	 */
	nvmet_tcp_rx_consumer_start();
	rx_loan_region_carve();
	if (!rx_loan_region_size())
		goto stock;
	rx_loan_budget_reconfigure(lb);
	if (!READ_ONCE(lb->budget))
		goto stock;
	queue->rx_budget = lb;
	queue->rx_region = true;
	return;
stock:
	/* Never lends: not held to its listener's hold either. */
	rx_loan_hold_release(queue->sock->sk);
}

/*
 * Count @len bytes that the stock path will copy, decided up front, to
 * nvmet-tcp's copies and the host-wide total: as the reset of a table that
 * copied them.
 */
static void nvmet_tcp_rx_count_copy(u32 len)
{
	struct rx_loan_table t = { .copied_bytes = len };

	rx_loan_table_reset_consumer(&t, &nvmet_tcp_rx_consumer);
}

/* Receive @cmd's @len bytes of WRITE data into a loan table? */
static bool nvmet_tcp_rx_want(struct nvmet_tcp_cmd *cmd, u32 len)
{
	struct nvmet_tcp_queue *queue = cmd->queue;
	u32 entries, bound;

	if (!queue->rx_region || !nvme_is_write(cmd->req.cmd) ||
	    !cmd->req.data_bvec_ok || (cmd->flags & NVMET_TCP_F_INIT_FAILED) ||
	    !READ_ONCE(queue->rx_budget->budget))
		return false;
	/*
	 * A table for the whole write, bounded by NVMET_TCP_RX_TABLE_MAX and
	 * by what the region could hold.  A write needing more, a capsule
	 * larger than a unit, or a failed allocation is decided here: the
	 * stock path copies it whole, and it is counted.  Nothing is borrowed
	 * first.
	 */
	entries = nvmet_tcp_rx_entries(len);
	bound = min_t(u64, (u64)rx_loan_region_size() *
		      (PAGE_SIZE / NVMET_TCP_RX_ENTRY_BYTES) + 2,
		      NVMET_TCP_RX_TABLE_MAX);
	if (entries > bound ||
	    cmd->pdu_len > nvmet_tcp_rx_unit(queue->rx_budget) ||
	    rx_loan_table_alloc(&cmd->lt, entries,
				GFP_KERNEL | __GFP_NOWARN, NUMA_NO_NODE)) {
		nvmet_tcp_rx_count_copy(len);
		return false;
	}
	cmd->flags |= NVMET_TCP_F_RX_LOAN;
	return true;
}

/* If cmd buffers are NULL, no operation is performed */
static void nvmet_tcp_free_cmd_buffers(struct nvmet_tcp_cmd *cmd)
{
	if (cmd->flags & NVMET_TCP_F_RX_LOAN)
		nvmet_tcp_rx_put(cmd);
	else
		nvmet_tcp_rx_unit_free(cmd->queue, cmd);
	kfree(cmd->iov);
	sgl_free(cmd->req.sg);
	cmd->iov = NULL;
	cmd->req.sg = NULL;
}

static int nvmet_tcp_build_pdu_iovec(struct nvmet_tcp_cmd *cmd)
{
	struct bio_vec *iov = cmd->iov;
	struct scatterlist *sg;
	u32 length, offset, sg_offset;
	unsigned int sg_remaining;
	int nr_pages;

	length = cmd->pdu_len;
	nr_pages = DIV_ROUND_UP(length, PAGE_SIZE);
	offset = cmd->rbytes_done;
	cmd->sg_idx = offset / PAGE_SIZE;
	sg_offset = offset % PAGE_SIZE;
	if (!cmd->req.sg_cnt || cmd->sg_idx >= cmd->req.sg_cnt)
		return -EPROTO;

	sg = &cmd->req.sg[cmd->sg_idx];
	sg_remaining = cmd->req.sg_cnt - cmd->sg_idx;

	while (length) {
		if (!sg_remaining)
			return -EPROTO;

		if (!sg->length || sg->length <= sg_offset)
			return -EPROTO;

		u32 iov_len = min_t(u32, length, sg->length - sg_offset);

		bvec_set_page(iov, sg_page(sg), iov_len,
				sg->offset + sg_offset);

		length -= iov_len;
		sg = sg_next(sg);
		sg_remaining--;
		iov++;
		sg_offset = 0;
	}

	iov_iter_bvec(&cmd->recv_msg.msg_iter, ITER_DEST, cmd->iov,
		      nr_pages, cmd->pdu_len);
	return 0;
}

static void nvmet_tcp_socket_error(struct nvmet_tcp_queue *queue, int status)
{
	/*
	 * Keep rcv_state at RECV_ERR even for the internal -ESHUTDOWN path.
	 * nvmet_tcp_handle_icreq() can return -ESHUTDOWN after the ICReq has
	 * already been consumed and queue teardown has started.
	 *
	 * If nvmet_tcp_data_ready() or nvmet_tcp_write_space() queues
	 * nvmet_tcp_io_work() again before nvmet_tcp_release_queue_work()
	 * cancels it, the queue must not keep that old receive state.
	 * Otherwise the next nvmet_tcp_io_work() run can reach
	 * nvmet_tcp_done_recv_pdu() and try to handle the same ICReq again.
	 *
	 * That is why queue->rcv_state needs to be updated before we return.
	 */
	queue->rcv_state = NVMET_TCP_RECV_ERR;
	if (status == -EPIPE || status == -ECONNRESET || !queue->nvme_sq.ctrl)
		kernel_sock_shutdown(queue->sock, SHUT_RDWR);
	else
		nvmet_ctrl_fatal_error(queue->nvme_sq.ctrl);
}

static int nvmet_tcp_map_data(struct nvmet_tcp_cmd *cmd)
{
	struct nvme_sgl_desc *sgl = &cmd->req.cmd->common.dptr.sgl;
	u32 len = le32_to_cpu(sgl->length);

	if (!len)
		return 0;

	/*
	 * inline_data_size only bounds the in-capsule (type 0x01) SGL
	 * descriptor below. A non-inline transport SGL data-block
	 * descriptor skips that check entirely and would otherwise reach
	 * sgl_alloc() with an attacker-controlled len of up to 4 GiB,
	 * pinning that much kernel memory for a command that may never
	 * complete. Bound every descriptor type here, before allocating
	 * anything, using the same ceiling this file already applies to
	 * per-PDU H2C data.
	 */
	if (len > NVMET_TCP_MAXH2CDATA)
		return NVME_SC_SGL_INVALID_DATA | NVME_STATUS_DNR;

	if (sgl->type == ((NVME_SGL_FMT_DATA_DESC << 4) |
			  NVME_SGL_FMT_OFFSET)) {
		if (!nvme_is_write(cmd->req.cmd))
			return NVME_SC_INVALID_FIELD | NVME_STATUS_DNR;

		if (len > cmd->req.port->inline_data_size)
			return NVME_SC_SGL_INVALID_OFFSET | NVME_STATUS_DNR;
		cmd->pdu_len = len;
	}
	cmd->req.transfer_len += len;

	if (nvmet_tcp_rx_want(cmd, len))
		return 0;

	cmd->req.sg = sgl_alloc(len, GFP_KERNEL | __GFP_NOWARN,
				&cmd->req.sg_cnt);
	if (!cmd->req.sg)
		return NVME_SC_INTERNAL;
	cmd->cur_sg = cmd->req.sg;

	if (nvmet_tcp_has_data_in(cmd)) {
		cmd->iov = kmalloc_objs(*cmd->iov, cmd->req.sg_cnt,
					GFP_KERNEL | __GFP_NOWARN);
		if (!cmd->iov)
			goto err;
	}

	return 0;
err:
	nvmet_tcp_free_cmd_buffers(cmd);
	return NVME_SC_INTERNAL;
}

static void nvmet_tcp_calc_ddgst(struct nvmet_tcp_cmd *cmd)
{
	size_t total_len = cmd->req.transfer_len;
	struct scatterlist *sg = cmd->req.sg;
	u32 crc = ~0;

	while (total_len) {
		size_t len = min_t(size_t, total_len, sg->length);

		/*
		 * Note that the scatterlist does not contain any highmem pages,
		 * as it was allocated by sgl_alloc() with GFP_KERNEL.
		 */
		crc = crc32c(crc, sg_virt(sg), len);
		total_len -= len;
		sg = sg_next(sg);
	}
	cmd->exp_ddgst = cpu_to_le32(~crc);
}

static void nvmet_setup_c2h_data_pdu(struct nvmet_tcp_cmd *cmd)
{
	struct nvme_tcp_data_pdu *pdu = cmd->data_pdu;
	struct nvmet_tcp_queue *queue = cmd->queue;
	u8 hdgst = nvmet_tcp_hdgst_len(cmd->queue);
	u8 ddgst = nvmet_tcp_ddgst_len(cmd->queue);

	cmd->offset = 0;
	cmd->state = NVMET_TCP_SEND_DATA_PDU;

	pdu->hdr.type = nvme_tcp_c2h_data;
	pdu->hdr.flags = NVME_TCP_F_DATA_LAST | (queue->nvme_sq.sqhd_disabled ?
						NVME_TCP_F_DATA_SUCCESS : 0);
	pdu->hdr.hlen = sizeof(*pdu);
	pdu->hdr.pdo = pdu->hdr.hlen + hdgst;
	pdu->hdr.plen =
		cpu_to_le32(pdu->hdr.hlen + hdgst +
				cmd->req.transfer_len + ddgst);
	pdu->command_id = cmd->req.cqe->command_id;
	pdu->data_length = cpu_to_le32(cmd->req.transfer_len);
	pdu->data_offset = cpu_to_le32(cmd->wbytes_done);

	if (queue->data_digest) {
		pdu->hdr.flags |= NVME_TCP_F_DDGST;
		nvmet_tcp_calc_ddgst(cmd);
	}

	if (cmd->queue->hdr_digest) {
		pdu->hdr.flags |= NVME_TCP_F_HDGST;
		nvmet_tcp_hdgst(pdu, sizeof(*pdu));
	}
}

static void nvmet_setup_r2t_pdu(struct nvmet_tcp_cmd *cmd)
{
	struct nvme_tcp_r2t_pdu *pdu = cmd->r2t_pdu;
	u8 hdgst = nvmet_tcp_hdgst_len(cmd->queue);

	cmd->offset = 0;
	cmd->state = NVMET_TCP_SEND_R2T;

	pdu->hdr.type = nvme_tcp_r2t;
	pdu->hdr.flags = 0;
	pdu->hdr.hlen = sizeof(*pdu);
	pdu->hdr.pdo = 0;
	pdu->hdr.plen = cpu_to_le32(pdu->hdr.hlen + hdgst);

	pdu->command_id = cmd->req.cmd->common.command_id;
	pdu->ttag = nvmet_tcp_cmd_tag(cmd->queue, cmd);
	/* A loaned command solicits the unit it was admitted for. */
	if (!(cmd->flags & NVMET_TCP_F_RX_LOAN))
		cmd->r2t_end = cmd->req.transfer_len;
	pdu->r2t_length = cpu_to_le32(cmd->r2t_end - cmd->rbytes_done);
	pdu->r2t_offset = cpu_to_le32(cmd->rbytes_done);
	if (cmd->queue->hdr_digest) {
		pdu->hdr.flags |= NVME_TCP_F_HDGST;
		nvmet_tcp_hdgst(pdu, sizeof(*pdu));
	}
}

static void nvmet_setup_response_pdu(struct nvmet_tcp_cmd *cmd)
{
	struct nvme_tcp_rsp_pdu *pdu = cmd->rsp_pdu;
	u8 hdgst = nvmet_tcp_hdgst_len(cmd->queue);

	cmd->offset = 0;
	cmd->state = NVMET_TCP_SEND_RESPONSE;

	pdu->hdr.type = nvme_tcp_rsp;
	pdu->hdr.flags = 0;
	pdu->hdr.hlen = sizeof(*pdu);
	pdu->hdr.pdo = 0;
	pdu->hdr.plen = cpu_to_le32(pdu->hdr.hlen + hdgst);
	if (cmd->queue->hdr_digest) {
		pdu->hdr.flags |= NVME_TCP_F_HDGST;
		nvmet_tcp_hdgst(pdu, sizeof(*pdu));
	}
}

static void nvmet_tcp_process_resp_list(struct nvmet_tcp_queue *queue)
{
	struct llist_node *node;
	struct nvmet_tcp_cmd *cmd;

	for (node = llist_del_all(&queue->resp_list); node; node = node->next) {
		cmd = llist_entry(node, struct nvmet_tcp_cmd, lentry);
		list_add(&cmd->entry, &queue->resp_send_list);
		queue->send_list_len++;
	}
}

static struct nvmet_tcp_cmd *nvmet_tcp_fetch_cmd(struct nvmet_tcp_queue *queue)
{
	struct nvmet_tcp_cmd *cmd = nvmet_tcp_rx_next_withheld(queue);

	if (cmd) {
		queue->snd_cmd = cmd;
		nvmet_setup_r2t_pdu(cmd);
		return cmd;
	}
again:
	queue->snd_cmd = list_first_entry_or_null(&queue->resp_send_list,
				struct nvmet_tcp_cmd, entry);
	if (!queue->snd_cmd) {
		nvmet_tcp_process_resp_list(queue);
		queue->snd_cmd =
			list_first_entry_or_null(&queue->resp_send_list,
					struct nvmet_tcp_cmd, entry);
		if (unlikely(!queue->snd_cmd))
			return NULL;
	}

	list_del_init(&queue->snd_cmd->entry);
	queue->send_list_len--;

	if (nvmet_tcp_need_data_out(queue->snd_cmd))
		nvmet_setup_c2h_data_pdu(queue->snd_cmd);
	else if (nvmet_tcp_need_data_in(queue->snd_cmd)) {
		if (!nvmet_tcp_rx_r2t_ok(queue->snd_cmd)) {
			queue->snd_cmd = NULL;
			goto again;
		}
		nvmet_setup_r2t_pdu(queue->snd_cmd);
	} else {
		nvmet_setup_response_pdu(queue->snd_cmd);
	}

	return queue->snd_cmd;
}

static void nvmet_tcp_queue_response(struct nvmet_req *req)
{
	struct nvmet_tcp_cmd *cmd =
		container_of(req, struct nvmet_tcp_cmd, req);
	struct nvmet_tcp_queue	*queue = cmd->queue;
	enum nvmet_tcp_recv_state queue_state;
	struct nvmet_tcp_cmd *queue_cmd;
	struct nvme_sgl_desc *sgl;
	u32 len;

	/* Pairs with store_release in nvmet_prepare_receive_pdu() */
	queue_state = smp_load_acquire(&queue->rcv_state);
	queue_cmd = READ_ONCE(queue->cmd);

	if (unlikely(cmd == queue_cmd)) {
		sgl = &cmd->req.cmd->common.dptr.sgl;
		len = le32_to_cpu(sgl->length);

		/*
		 * Wait for inline data before processing the response.
		 * Avoid using helpers, this might happen before
		 * nvmet_req_init is completed.
		 */
		if (queue_state == NVMET_TCP_RECV_PDU &&
		    len && len <= cmd->req.port->inline_data_size &&
		    nvme_is_write(cmd->req.cmd))
			return;
	}

	llist_add(&cmd->lentry, &queue->resp_list);
	queue_work_on(queue_cpu(queue), nvmet_tcp_wq, &cmd->queue->io_work);
}

static void nvmet_tcp_execute_request(struct nvmet_tcp_cmd *cmd)
{
	if (unlikely(cmd->flags & NVMET_TCP_F_INIT_FAILED))
		nvmet_tcp_queue_response(&cmd->req);
	else
		cmd->req.execute(&cmd->req);
}

static int nvmet_try_send_data_pdu(struct nvmet_tcp_cmd *cmd)
{
	struct msghdr msg = {
		.msg_flags = MSG_DONTWAIT | MSG_MORE | MSG_SPLICE_PAGES,
	};
	struct bio_vec bvec;
	u8 hdgst = nvmet_tcp_hdgst_len(cmd->queue);
	int left = sizeof(*cmd->data_pdu) - cmd->offset + hdgst;
	int ret;

	bvec_set_virt(&bvec, (void *)cmd->data_pdu + cmd->offset, left);
	iov_iter_bvec(&msg.msg_iter, ITER_SOURCE, &bvec, 1, left);
	ret = sock_sendmsg(cmd->queue->sock, &msg);
	if (ret <= 0)
		return ret;

	cmd->offset += ret;
	left -= ret;

	if (left)
		return -EAGAIN;

	cmd->state = NVMET_TCP_SEND_DATA;
	cmd->offset  = 0;
	return 1;
}

static int nvmet_try_send_data(struct nvmet_tcp_cmd *cmd, bool last_in_batch)
{
	struct nvmet_tcp_queue *queue = cmd->queue;
	int ret;

	while (cmd->cur_sg) {
		struct msghdr msg = {
			.msg_flags = MSG_DONTWAIT | MSG_SPLICE_PAGES,
		};
		struct page *page = sg_page(cmd->cur_sg);
		struct bio_vec bvec;
		u32 left = cmd->cur_sg->length - cmd->offset;

		if ((!last_in_batch && cmd->queue->send_list_len) ||
		    cmd->wbytes_done + left < cmd->req.transfer_len ||
		    queue->data_digest || !queue->nvme_sq.sqhd_disabled)
			msg.msg_flags |= MSG_MORE;

		bvec_set_page(&bvec, page, left, cmd->offset);
		iov_iter_bvec(&msg.msg_iter, ITER_SOURCE, &bvec, 1, left);
		ret = sock_sendmsg(cmd->queue->sock, &msg);
		if (ret <= 0)
			return ret;

		cmd->offset += ret;
		cmd->wbytes_done += ret;

		/* Done with sg?*/
		if (cmd->offset == cmd->cur_sg->length) {
			cmd->cur_sg = sg_next(cmd->cur_sg);
			cmd->offset = 0;
		}
	}

	if (queue->data_digest) {
		cmd->state = NVMET_TCP_SEND_DDGST;
		cmd->offset = 0;
	} else {
		if (queue->nvme_sq.sqhd_disabled) {
			cmd->queue->snd_cmd = NULL;
			nvmet_tcp_put_cmd(cmd);
		} else {
			nvmet_setup_response_pdu(cmd);
		}
	}

	if (queue->nvme_sq.sqhd_disabled)
		nvmet_tcp_free_cmd_buffers(cmd);

	return 1;

}

static int nvmet_try_send_response(struct nvmet_tcp_cmd *cmd,
		bool last_in_batch)
{
	struct msghdr msg = { .msg_flags = MSG_DONTWAIT | MSG_SPLICE_PAGES, };
	struct bio_vec bvec;
	u8 hdgst = nvmet_tcp_hdgst_len(cmd->queue);
	int left = sizeof(*cmd->rsp_pdu) - cmd->offset + hdgst;
	int ret;

	if (!last_in_batch && cmd->queue->send_list_len)
		msg.msg_flags |= MSG_MORE;
	else
		msg.msg_flags |= MSG_EOR;

	bvec_set_virt(&bvec, (void *)cmd->rsp_pdu + cmd->offset, left);
	iov_iter_bvec(&msg.msg_iter, ITER_SOURCE, &bvec, 1, left);
	ret = sock_sendmsg(cmd->queue->sock, &msg);
	if (ret <= 0)
		return ret;
	cmd->offset += ret;
	left -= ret;

	if (left)
		return -EAGAIN;

	nvmet_tcp_free_cmd_buffers(cmd);
	cmd->queue->snd_cmd = NULL;
	nvmet_tcp_put_cmd(cmd);
	return 1;
}

static int nvmet_try_send_r2t(struct nvmet_tcp_cmd *cmd, bool last_in_batch)
{
	struct msghdr msg = { .msg_flags = MSG_DONTWAIT | MSG_SPLICE_PAGES, };
	struct bio_vec bvec;
	u8 hdgst = nvmet_tcp_hdgst_len(cmd->queue);
	int left = sizeof(*cmd->r2t_pdu) - cmd->offset + hdgst;
	int ret;

	if (!last_in_batch && cmd->queue->send_list_len)
		msg.msg_flags |= MSG_MORE;
	else
		msg.msg_flags |= MSG_EOR;

	bvec_set_virt(&bvec, (void *)cmd->r2t_pdu + cmd->offset, left);
	iov_iter_bvec(&msg.msg_iter, ITER_SOURCE, &bvec, 1, left);
	ret = sock_sendmsg(cmd->queue->sock, &msg);
	if (ret <= 0)
		return ret;
	cmd->offset += ret;
	left -= ret;

	if (left)
		return -EAGAIN;

	cmd->queue->snd_cmd = NULL;
	cmd->flags |= NVMET_TCP_F_R2T_SENT;
	return 1;
}

static int nvmet_try_send_ddgst(struct nvmet_tcp_cmd *cmd, bool last_in_batch)
{
	struct nvmet_tcp_queue *queue = cmd->queue;
	int left = NVME_TCP_DIGEST_LENGTH - cmd->offset;
	struct msghdr msg = { .msg_flags = MSG_DONTWAIT };
	struct kvec iov = {
		.iov_base = (u8 *)&cmd->exp_ddgst + cmd->offset,
		.iov_len = left
	};
	int ret;

	if (!last_in_batch && cmd->queue->send_list_len)
		msg.msg_flags |= MSG_MORE;
	else
		msg.msg_flags |= MSG_EOR;

	ret = kernel_sendmsg(queue->sock, &msg, &iov, 1, iov.iov_len);
	if (unlikely(ret <= 0))
		return ret;

	cmd->offset += ret;
	left -= ret;

	if (left)
		return -EAGAIN;

	if (queue->nvme_sq.sqhd_disabled) {
		cmd->queue->snd_cmd = NULL;
		nvmet_tcp_put_cmd(cmd);
	} else {
		nvmet_setup_response_pdu(cmd);
	}
	return 1;
}

static int nvmet_tcp_try_send_one(struct nvmet_tcp_queue *queue,
		bool last_in_batch)
{
	struct nvmet_tcp_cmd *cmd = queue->snd_cmd;
	int ret = 0;

	if (!cmd || queue->state == NVMET_TCP_Q_DISCONNECTING) {
		cmd = nvmet_tcp_fetch_cmd(queue);
		if (unlikely(!cmd))
			return 0;
	}

	if (cmd->state == NVMET_TCP_SEND_DATA_PDU) {
		ret = nvmet_try_send_data_pdu(cmd);
		if (ret <= 0)
			goto done_send;
	}

	if (cmd->state == NVMET_TCP_SEND_DATA) {
		ret = nvmet_try_send_data(cmd, last_in_batch);
		if (ret <= 0)
			goto done_send;
	}

	if (cmd->state == NVMET_TCP_SEND_DDGST) {
		ret = nvmet_try_send_ddgst(cmd, last_in_batch);
		if (ret <= 0)
			goto done_send;
	}

	if (cmd->state == NVMET_TCP_SEND_R2T) {
		ret = nvmet_try_send_r2t(cmd, last_in_batch);
		if (ret <= 0)
			goto done_send;
	}

	if (cmd->state == NVMET_TCP_SEND_RESPONSE)
		ret = nvmet_try_send_response(cmd, last_in_batch);

done_send:
	if (ret < 0) {
		if (ret == -EAGAIN)
			return 0;
		return ret;
	}

	return 1;
}

static int nvmet_tcp_try_send(struct nvmet_tcp_queue *queue,
		int budget, int *sends)
{
	int i, ret = 0;

	for (i = 0; i < budget; i++) {
		ret = nvmet_tcp_try_send_one(queue, i == budget - 1);
		if (unlikely(ret < 0)) {
			nvmet_tcp_socket_error(queue, ret);
			goto done;
		} else if (ret == 0) {
			break;
		}
		(*sends)++;
	}
done:
	return ret;
}

static void nvmet_prepare_receive_pdu(struct nvmet_tcp_queue *queue)
{
	queue->offset = 0;
	queue->left = sizeof(struct nvme_tcp_hdr);
	WRITE_ONCE(queue->cmd, NULL);
	/* Ensure rcv_state is visible only after queue->cmd is set */
	smp_store_release(&queue->rcv_state, NVMET_TCP_RECV_PDU);
}

static int nvmet_tcp_handle_icreq(struct nvmet_tcp_queue *queue)
{
	struct nvme_tcp_icreq_pdu *icreq = &queue->pdu.icreq;
	struct nvme_tcp_icresp_pdu *icresp = &queue->pdu.icresp;
	struct msghdr msg = {};
	struct kvec iov;
	int ret;

	if (le32_to_cpu(icreq->hdr.plen) != sizeof(struct nvme_tcp_icreq_pdu)) {
		pr_err("bad nvme-tcp pdu length (%d)\n",
			le32_to_cpu(icreq->hdr.plen));
		return -EPROTO;
	}

	if (icreq->pfv != NVME_TCP_PFV_1_0) {
		pr_err("queue %d: bad pfv %d\n", queue->idx, icreq->pfv);
		return -EPROTO;
	}

	if (icreq->hpda != 0) {
		pr_err("queue %d: unsupported hpda %d\n", queue->idx,
			icreq->hpda);
		return -EPROTO;
	}

	queue->hdr_digest = !!(icreq->digest & NVME_TCP_HDR_DIGEST_ENABLE);
	queue->data_digest = !!(icreq->digest & NVME_TCP_DATA_DIGEST_ENABLE);

	nvmet_tcp_rx_setup(queue);

	memset(icresp, 0, sizeof(*icresp));
	icresp->hdr.type = nvme_tcp_icresp;
	icresp->hdr.hlen = sizeof(*icresp);
	icresp->hdr.pdo = 0;
	icresp->hdr.plen = cpu_to_le32(icresp->hdr.hlen);
	icresp->pfv = cpu_to_le16(NVME_TCP_PFV_1_0);
	icresp->maxdata = cpu_to_le32(NVMET_TCP_MAXH2CDATA);
	icresp->cpda = 0;
	if (queue->hdr_digest)
		icresp->digest |= NVME_TCP_HDR_DIGEST_ENABLE;
	if (queue->data_digest)
		icresp->digest |= NVME_TCP_DATA_DIGEST_ENABLE;

	iov.iov_base = icresp;
	iov.iov_len = sizeof(*icresp);
	ret = kernel_sendmsg(queue->sock, &msg, &iov, 1, iov.iov_len);
	if (ret < 0) {
		spin_lock_bh(&queue->state_lock);
		if (queue->state == NVMET_TCP_Q_DISCONNECTING) {
			spin_unlock_bh(&queue->state_lock);
			return -ESHUTDOWN;
		}
		queue->state = NVMET_TCP_Q_FAILED;
		spin_unlock_bh(&queue->state_lock);
		return ret; /* queue removal will cleanup */
	}

	spin_lock_bh(&queue->state_lock);
	if (queue->state == NVMET_TCP_Q_DISCONNECTING) {
		spin_unlock_bh(&queue->state_lock);
		/* Tell nvmet_tcp_socket_error() teardown is in progress. */
		return -ESHUTDOWN;
	}
	queue->state = NVMET_TCP_Q_LIVE;
	spin_unlock_bh(&queue->state_lock);
	nvmet_prepare_receive_pdu(queue);
	return 0;
}

static int nvmet_tcp_handle_req_failure(struct nvmet_tcp_queue *queue,
		struct nvmet_tcp_cmd *cmd, struct nvmet_req *req)
{
	size_t data_len = le32_to_cpu(req->cmd->common.dptr.sgl.length);
	int ret;

	/*
	 * This command has not been processed yet, hence we are trying to
	 * figure out if there is still pending data left to receive. If
	 * we don't, we can simply prepare for the next pdu and bail out,
	 * otherwise we will need to prepare a buffer and receive the
	 * stale data before continuing forward.
	 */
	if (!nvme_is_write(cmd->req.cmd) || !data_len ||
	    data_len > cmd->req.port->inline_data_size) {
		nvmet_prepare_receive_pdu(queue);
		return 0;
	}

	/* Set first: the stale data is sunk into a buffer, never loaned. */
	cmd->flags |= NVMET_TCP_F_INIT_FAILED;
	ret = nvmet_tcp_map_data(cmd);
	if (unlikely(ret)) {
		pr_err("queue %d: failed to map data\n", queue->idx);
		return -EPROTO;
	}

	queue->rcv_state = NVMET_TCP_RECV_DATA;
	ret = nvmet_tcp_build_pdu_iovec(cmd);
	if (unlikely(ret))
		pr_err("queue %d: failed to build PDU iovec\n", queue->idx);

	return ret;
}

static int nvmet_tcp_handle_h2c_data_pdu(struct nvmet_tcp_queue *queue)
{
	struct nvme_tcp_data_pdu *data = &queue->pdu.data;
	struct nvmet_tcp_cmd *cmd;
	unsigned int exp_data_len;

	if (likely(queue->nr_cmds)) {
		if (unlikely(data->ttag >= queue->nr_cmds)) {
			pr_err("queue %d: received out of bound ttag %u, nr_cmds %u\n",
				queue->idx, data->ttag, queue->nr_cmds);
			goto err_proto;
		}
		cmd = &queue->cmds[data->ttag];
	} else {
		cmd = &queue->connect;
	}

	if (unlikely(!(cmd->flags & NVMET_TCP_F_R2T_SENT))) {
		pr_err("queue %d: unsolicited H2CData (ttag %u)\n",
		       queue->idx, data->ttag);
		goto err_proto;
	}

	if (le32_to_cpu(data->data_offset) != cmd->rbytes_done) {
		pr_err("ttag %u unexpected data offset %u (expected %u)\n",
			data->ttag, le32_to_cpu(data->data_offset),
			cmd->rbytes_done);
		goto err_proto;
	}

	exp_data_len = le32_to_cpu(data->hdr.plen) -
			nvmet_tcp_hdgst_len(queue) -
			nvmet_tcp_ddgst_len(queue) -
			sizeof(*data);

	cmd->pdu_len = le32_to_cpu(data->data_length);
	if (unlikely(cmd->pdu_len != exp_data_len ||
		     cmd->pdu_len == 0 ||
		     cmd->pdu_len > NVMET_TCP_MAXH2CDATA)) {
		pr_err("H2CData PDU len %u is invalid\n", cmd->pdu_len);
		goto err_proto;
	}
	if (cmd->flags & (NVMET_TCP_F_RX_LOAN | NVMET_TCP_F_RX_SINK)) {
		if (unlikely(queue->rx_unit != cmd ||
			     cmd->rbytes_done + cmd->pdu_len > cmd->r2t_end)) {
			pr_err("queue %d: H2CData PDU outside its R2T (ttag %u)\n",
			       queue->idx, data->ttag);
			goto err_proto;
		}
		cmd->pdu_recv = 0;
		if (cmd->flags & NVMET_TCP_F_RX_SINK)
			iov_iter_discard(&cmd->recv_msg.msg_iter, ITER_DEST,
					 cmd->pdu_len);
		queue->cmd = cmd;
		queue->rcv_state = NVMET_TCP_RECV_DATA;
		return 0;
	}
       /*
	* Ensure command data structures are initialized. We must check both
	* cmd->req.sg and cmd->iov because they can have different NULL states:
	* - Uninitialized commands: both NULL
	* - READ commands: cmd->req.sg allocated, cmd->iov NULL
	* - WRITE commands: both allocated
	*/
	if (unlikely(!cmd->req.sg || !cmd->iov)) {
		pr_err("queue %d: H2CData PDU received for invalid command state (ttag %u)\n",
			queue->idx, data->ttag);
		goto err_proto;
	}
	cmd->pdu_recv = 0;
	if (unlikely(nvmet_tcp_build_pdu_iovec(cmd))) {
		pr_err("queue %d: failed to build PDU iovec\n", queue->idx);
		goto err_proto;
	}
	queue->cmd = cmd;
	queue->rcv_state = NVMET_TCP_RECV_DATA;

	return 0;

err_proto:
	/* FIXME: use proper transport errors */
	return -EPROTO;
}

static int nvmet_tcp_done_recv_pdu(struct nvmet_tcp_queue *queue)
{
	struct nvme_tcp_hdr *hdr = &queue->pdu.cmd.hdr;
	struct nvme_command *nvme_cmd = &queue->pdu.cmd.cmd;
	struct nvmet_req *req;
	int ret;

	if (unlikely(queue->state == NVMET_TCP_Q_CONNECTING)) {
		if (hdr->type != nvme_tcp_icreq) {
			pr_err("unexpected pdu type (%d) before icreq\n",
				hdr->type);
			return -EPROTO;
		}
		return nvmet_tcp_handle_icreq(queue);
	}

	if (unlikely(hdr->type == nvme_tcp_icreq)) {
		pr_err("queue %d: received icreq pdu in state %d\n",
			queue->idx, queue->state);
		return -EPROTO;
	}

	if (hdr->type == nvme_tcp_h2c_data) {
		ret = nvmet_tcp_handle_h2c_data_pdu(queue);
		if (unlikely(ret))
			return ret;
		return 0;
	}

	queue->cmd = nvmet_tcp_get_cmd(queue);
	if (unlikely(!queue->cmd)) {
		/* This should never happen */
		pr_err("queue %d: out of commands (%d) send_list_len: %d, opcode: %d",
			queue->idx, queue->nr_cmds, queue->send_list_len,
			nvme_cmd->common.opcode);
		return -ENOMEM;
	}

	req = &queue->cmd->req;
	memcpy(req->cmd, nvme_cmd, sizeof(*nvme_cmd));

	if (unlikely(!nvmet_req_init(req, &queue->nvme_sq, &nvmet_tcp_ops))) {
		pr_err("failed cmd %p id %d opcode %d, data_len: %d, status: %04x\n",
			req->cmd, req->cmd->common.command_id,
			req->cmd->common.opcode,
			le32_to_cpu(req->cmd->common.dptr.sgl.length),
			le16_to_cpu(req->cqe->status));

		return nvmet_tcp_handle_req_failure(queue, queue->cmd, req);
	}

	ret = nvmet_tcp_map_data(queue->cmd);
	if (unlikely(ret)) {
		pr_err("queue %d: failed to map data\n", queue->idx);
		if (nvmet_tcp_has_inline_data(queue->cmd))
			return -EPROTO;

		nvmet_req_complete(req, ret);
		ret = -EAGAIN;
		goto out;
	}

	if (nvmet_tcp_need_data_in(queue->cmd)) {
		if (nvmet_tcp_has_inline_data(queue->cmd)) {
			queue->rcv_state = NVMET_TCP_RECV_DATA;
			if (queue->cmd->flags & NVMET_TCP_F_RX_LOAN)
				return 0;
			ret = nvmet_tcp_build_pdu_iovec(queue->cmd);
			if (unlikely(ret))
				pr_err("queue %d: failed to build PDU iovec\n",
					queue->idx);
			return ret;
		}
		/* send back R2T */
		nvmet_tcp_queue_response(&queue->cmd->req);
		goto out;
	}

	queue->cmd->req.execute(&queue->cmd->req);
out:
	nvmet_prepare_receive_pdu(queue);
	return ret;
}

static const u8 nvme_tcp_pdu_sizes[] = {
	[nvme_tcp_icreq]	= sizeof(struct nvme_tcp_icreq_pdu),
	[nvme_tcp_cmd]		= sizeof(struct nvme_tcp_cmd_pdu),
	[nvme_tcp_h2c_data]	= sizeof(struct nvme_tcp_data_pdu),
};

static inline u8 nvmet_tcp_pdu_size(u8 type)
{
	size_t idx = type;

	return (idx < ARRAY_SIZE(nvme_tcp_pdu_sizes) &&
		nvme_tcp_pdu_sizes[idx]) ?
			nvme_tcp_pdu_sizes[idx] : 0;
}

static inline bool nvmet_tcp_pdu_valid(u8 type)
{
	switch (type) {
	case nvme_tcp_icreq:
	case nvme_tcp_cmd:
	case nvme_tcp_h2c_data:
		/* fallthru */
		return true;
	}

	return false;
}

static int nvmet_tcp_tls_record_ok(struct nvmet_tcp_queue *queue,
		struct msghdr *msg, char *cbuf)
{
	struct cmsghdr *cmsg = (struct cmsghdr *)cbuf;
	u8 ctype, level, description;
	int ret = 0;

	ctype = tls_get_record_type(queue->sock->sk, cmsg);
	switch (ctype) {
	case 0:
		break;
	case TLS_RECORD_TYPE_DATA:
		break;
	case TLS_RECORD_TYPE_ALERT:
		tls_alert_recv(queue->sock->sk, msg, &level, &description);
		if (level == TLS_ALERT_LEVEL_FATAL) {
			pr_err("queue %d: TLS Alert desc %u\n",
			       queue->idx, description);
			ret = -ENOTCONN;
		} else {
			pr_warn("queue %d: TLS Alert desc %u\n",
			       queue->idx, description);
			ret = -EAGAIN;
		}
		break;
	default:
		/* discard this record type */
		pr_err("queue %d: TLS record %d unhandled\n",
		       queue->idx, ctype);
		ret = -EAGAIN;
		break;
	}
	return ret;
}

static int nvmet_tcp_try_recv_pdu(struct nvmet_tcp_queue *queue)
{
	struct nvme_tcp_hdr *hdr = &queue->pdu.cmd.hdr;
	int len, ret;
	struct kvec iov;
	char cbuf[CMSG_LEN(sizeof(char))] = {};
	struct msghdr msg = { .msg_flags = MSG_DONTWAIT };

recv:
	iov.iov_base = (void *)&queue->pdu + queue->offset;
	iov.iov_len = queue->left;
	if (queue->tls_pskid) {
		msg.msg_control = cbuf;
		msg.msg_controllen = sizeof(cbuf);
	}
	len = kernel_recvmsg(queue->sock, &msg, &iov, 1,
			iov.iov_len, msg.msg_flags);
	if (unlikely(len < 0))
		return len;
	if (queue->tls_pskid) {
		ret = nvmet_tcp_tls_record_ok(queue, &msg, cbuf);
		if (ret < 0)
			return ret;
	}

	queue->offset += len;
	queue->left -= len;
	if (queue->left)
		return -EAGAIN;

	if (queue->offset == sizeof(struct nvme_tcp_hdr)) {
		u8 hdgst = nvmet_tcp_hdgst_len(queue);

		if (unlikely(!nvmet_tcp_pdu_valid(hdr->type))) {
			pr_err("unexpected pdu type %d\n", hdr->type);
			return -EIO;
		}

		if (unlikely(hdr->hlen != nvmet_tcp_pdu_size(hdr->type))) {
			pr_err("pdu %d bad hlen %d\n", hdr->type, hdr->hlen);
			return -EIO;
		}

		queue->left = hdr->hlen - queue->offset + hdgst;
		if (queue->left > sizeof(queue->pdu) - queue->offset)
			return -EPROTO;
		goto recv;
	}

	if (queue->hdr_digest &&
	    nvmet_tcp_verify_hdgst(queue, &queue->pdu, hdr->hlen))
		return -EPROTO;

	if (queue->data_digest &&
	    nvmet_tcp_check_ddgst(queue, &queue->pdu))
		return -EPROTO;

	return nvmet_tcp_done_recv_pdu(queue);
}

static void nvmet_tcp_prep_recv_ddgst(struct nvmet_tcp_cmd *cmd)
{
	struct nvmet_tcp_queue *queue = cmd->queue;

	nvmet_tcp_calc_ddgst(cmd);
	queue->offset = 0;
	queue->left = NVME_TCP_DIGEST_LENGTH;
	queue->rcv_state = NVMET_TCP_RECV_DDGST;
}

static int nvmet_tcp_try_recv_data(struct nvmet_tcp_queue *queue)
{
	struct nvmet_tcp_cmd  *cmd = queue->cmd;
	int len, ret;

	if (cmd->flags & NVMET_TCP_F_RX_LOAN) {
		ret = nvmet_tcp_rx_recv(queue, cmd);
		if (ret)
			return ret;
		/* Still loaned: the PDU is in; else the rest is copied below */
		if (cmd->flags & NVMET_TCP_F_RX_LOAN)
			goto done;
	}

	while (msg_data_left(&cmd->recv_msg)) {
		len = sock_recvmsg(cmd->queue->sock, &cmd->recv_msg,
			cmd->recv_msg.msg_flags);
		if (len <= 0)
			return len;
		if (queue->tls_pskid) {
			ret = nvmet_tcp_tls_record_ok(cmd->queue,
					&cmd->recv_msg, cmd->recv_cbuf);
			if (ret < 0)
				return ret;
		}

		cmd->pdu_recv += len;
		cmd->rbytes_done += len;
	}

done:
	if (queue->data_digest) {
		nvmet_tcp_prep_recv_ddgst(cmd);
		return 0;
	}

	if (!nvmet_tcp_rx_unit_done(cmd) &&
	    cmd->rbytes_done == cmd->req.transfer_len &&
	    !nvmet_tcp_rx_finish(cmd))
		nvmet_tcp_execute_request(cmd);

	nvmet_prepare_receive_pdu(queue);
	return 0;
}

static int nvmet_tcp_try_recv_ddgst(struct nvmet_tcp_queue *queue)
{
	struct nvmet_tcp_cmd *cmd = queue->cmd;
	int ret, len;
	char cbuf[CMSG_LEN(sizeof(char))] = {};
	struct msghdr msg = { .msg_flags = MSG_DONTWAIT };
	struct kvec iov = {
		.iov_base = (void *)&cmd->recv_ddgst + queue->offset,
		.iov_len = queue->left
	};

	if (queue->tls_pskid) {
		msg.msg_control = cbuf;
		msg.msg_controllen = sizeof(cbuf);
	}
	len = kernel_recvmsg(queue->sock, &msg, &iov, 1,
			iov.iov_len, msg.msg_flags);
	if (unlikely(len < 0))
		return len;
	if (queue->tls_pskid) {
		ret = nvmet_tcp_tls_record_ok(queue, &msg, cbuf);
		if (ret < 0)
			return ret;
	}

	queue->offset += len;
	queue->left -= len;
	if (queue->left)
		return -EAGAIN;

	if (queue->data_digest && cmd->exp_ddgst != cmd->recv_ddgst) {
		pr_err("queue %d: cmd %d pdu (%d) data digest error: recv %#x expected %#x\n",
			queue->idx, cmd->req.cmd->common.command_id,
			queue->pdu.cmd.hdr.type, le32_to_cpu(cmd->recv_ddgst),
			le32_to_cpu(cmd->exp_ddgst));
		if (!(cmd->flags & NVMET_TCP_F_INIT_FAILED)) {
			cmd->req.cqe->status = NVME_SC_CMD_SEQ_ERROR;
			nvmet_req_uninit(&cmd->req);
		}
		nvmet_tcp_free_cmd_buffers(cmd);
		ret = -EPROTO;
		goto out;
	}

	if (cmd->rbytes_done == cmd->req.transfer_len)
		nvmet_tcp_execute_request(cmd);

	ret = 0;
out:
	nvmet_prepare_receive_pdu(queue);
	return ret;
}

static int nvmet_tcp_try_recv_one(struct nvmet_tcp_queue *queue)
{
	int result = 0;

	if (unlikely(queue->rcv_state == NVMET_TCP_RECV_ERR))
		return 0;

	if (queue->rcv_state == NVMET_TCP_RECV_PDU) {
		result = nvmet_tcp_try_recv_pdu(queue);
		if (result != 0)
			goto done_recv;
	}

	if (queue->rcv_state == NVMET_TCP_RECV_DATA) {
		result = nvmet_tcp_try_recv_data(queue);
		if (result != 0)
			goto done_recv;
	}

	if (queue->rcv_state == NVMET_TCP_RECV_DDGST) {
		result = nvmet_tcp_try_recv_ddgst(queue);
		if (result != 0)
			goto done_recv;
	}

done_recv:
	if (result < 0) {
		if (result == -EAGAIN)
			return 0;
		return result;
	}
	return 1;
}

static int nvmet_tcp_try_recv(struct nvmet_tcp_queue *queue,
		int budget, int *recvs)
{
	int i, ret = 0;

	for (i = 0; i < budget; i++) {
		ret = nvmet_tcp_try_recv_one(queue);
		if (unlikely(ret < 0)) {
			nvmet_tcp_socket_error(queue, ret);
			goto done;
		} else if (ret == 0) {
			break;
		}
		(*recvs)++;
	}
done:
	return ret;
}

static void nvmet_tcp_release_queue(struct kref *kref)
{
	struct nvmet_tcp_queue *queue =
		container_of(kref, struct nvmet_tcp_queue, kref);

	WARN_ON(queue->state != NVMET_TCP_Q_DISCONNECTING);
	queue_work(nvmet_wq, &queue->release_work);
}

static void nvmet_tcp_schedule_release_queue(struct nvmet_tcp_queue *queue)
{
	spin_lock_bh(&queue->state_lock);
	if (queue->state == NVMET_TCP_Q_TLS_HANDSHAKE) {
		/* Socket closed during handshake */
		tls_handshake_cancel(queue->sock->sk);
	}
	if (queue->state != NVMET_TCP_Q_DISCONNECTING) {
		queue->state = NVMET_TCP_Q_DISCONNECTING;
		kref_put(&queue->kref, nvmet_tcp_release_queue);
	}
	spin_unlock_bh(&queue->state_lock);
}

static inline void nvmet_tcp_arm_queue_deadline(struct nvmet_tcp_queue *queue)
{
	queue->poll_end = jiffies + usecs_to_jiffies(idle_poll_period_usecs);
}

static bool nvmet_tcp_check_queue_deadline(struct nvmet_tcp_queue *queue,
		int ops)
{
	if (!idle_poll_period_usecs)
		return false;

	if (ops)
		nvmet_tcp_arm_queue_deadline(queue);

	return !time_after(jiffies, queue->poll_end);
}

static void nvmet_tcp_io_work(struct work_struct *w)
{
	struct nvmet_tcp_queue *queue =
		container_of(w, struct nvmet_tcp_queue, io_work);
	bool pending;
	int ret, ops = 0;

	if (!llist_empty(&queue->rx_done))
		nvmet_tcp_rx_drain_done(queue);

	do {
		pending = false;

		ret = nvmet_tcp_try_recv(queue, NVMET_TCP_RECV_BUDGET, &ops);
		if (ret > 0)
			pending = true;
		else if (ret < 0)
			return;

		ret = nvmet_tcp_try_send(queue, NVMET_TCP_SEND_BUDGET, &ops);
		if (ret > 0)
			pending = true;
		else if (ret < 0)
			return;

	} while (pending && ops < NVMET_TCP_IO_WORK_BUDGET);

	/*
	 * Requeue the worker if idle deadline period is in progress or any
	 * ops activity was recorded during the do-while loop above.
	 */
	if (nvmet_tcp_check_queue_deadline(queue, ops) || pending)
		queue_work_on(queue_cpu(queue), nvmet_tcp_wq, &queue->io_work);
}

static int nvmet_tcp_alloc_cmd(struct nvmet_tcp_queue *queue,
		struct nvmet_tcp_cmd *c)
{
	u8 hdgst = nvmet_tcp_hdgst_len(queue);

	c->queue = queue;
	c->req.port = queue->port->nport;

	c->cmd_pdu = page_frag_alloc(&queue->pf_cache,
			sizeof(*c->cmd_pdu) + hdgst, GFP_KERNEL | __GFP_ZERO);
	if (!c->cmd_pdu)
		return -ENOMEM;
	c->req.cmd = &c->cmd_pdu->cmd;

	c->rsp_pdu = page_frag_alloc(&queue->pf_cache,
			sizeof(*c->rsp_pdu) + hdgst, GFP_KERNEL | __GFP_ZERO);
	if (!c->rsp_pdu)
		goto out_free_cmd;
	c->req.cqe = &c->rsp_pdu->cqe;

	c->data_pdu = page_frag_alloc(&queue->pf_cache,
			sizeof(*c->data_pdu) + hdgst, GFP_KERNEL | __GFP_ZERO);
	if (!c->data_pdu)
		goto out_free_rsp;

	c->r2t_pdu = page_frag_alloc(&queue->pf_cache,
			sizeof(*c->r2t_pdu) + hdgst, GFP_KERNEL | __GFP_ZERO);
	if (!c->r2t_pdu)
		goto out_free_data;

	if (queue->state == NVMET_TCP_Q_TLS_HANDSHAKE) {
		c->recv_msg.msg_control = c->recv_cbuf;
		c->recv_msg.msg_controllen = sizeof(c->recv_cbuf);
	}
	c->recv_msg.msg_flags = MSG_DONTWAIT | MSG_NOSIGNAL;

	list_add_tail(&c->entry, &queue->free_list);

	return 0;
out_free_data:
	page_frag_free(c->data_pdu);
out_free_rsp:
	page_frag_free(c->rsp_pdu);
out_free_cmd:
	page_frag_free(c->cmd_pdu);
	return -ENOMEM;
}

static void nvmet_tcp_free_cmd(struct nvmet_tcp_cmd *c)
{
	page_frag_free(c->r2t_pdu);
	page_frag_free(c->data_pdu);
	page_frag_free(c->rsp_pdu);
	page_frag_free(c->cmd_pdu);
}

static int nvmet_tcp_alloc_cmds(struct nvmet_tcp_queue *queue)
{
	struct nvmet_tcp_cmd *cmds;
	int i, ret = -EINVAL, nr_cmds = queue->nr_cmds;

	cmds = kvzalloc_objs(struct nvmet_tcp_cmd, nr_cmds);
	if (!cmds)
		goto out;

	for (i = 0; i < nr_cmds; i++) {
		ret = nvmet_tcp_alloc_cmd(queue, cmds + i);
		if (ret)
			goto out_free;
	}

	queue->cmds = cmds;

	return 0;
out_free:
	while (--i >= 0)
		nvmet_tcp_free_cmd(cmds + i);
	kvfree(cmds);
out:
	return ret;
}

static void nvmet_tcp_free_cmds(struct nvmet_tcp_queue *queue)
{
	struct nvmet_tcp_cmd *cmds = queue->cmds;
	int i;

	for (i = 0; i < queue->nr_cmds; i++)
		nvmet_tcp_free_cmd(cmds + i);

	nvmet_tcp_free_cmd(&queue->connect);
	kvfree(cmds);
}

static void nvmet_tcp_restore_socket_callbacks(struct nvmet_tcp_queue *queue)
{
	struct socket *sock = queue->sock;

	if (!queue->state_change)
		return;

	write_lock_bh(&sock->sk->sk_callback_lock);
	sock->sk->sk_data_ready =  queue->data_ready;
	sock->sk->sk_state_change = queue->state_change;
	sock->sk->sk_write_space = queue->write_space;
	sock->sk->sk_user_data = NULL;
	write_unlock_bh(&sock->sk->sk_callback_lock);
}

static void nvmet_tcp_uninit_data_in_cmds(struct nvmet_tcp_queue *queue)
{
	struct nvmet_tcp_cmd *cmd = queue->cmds;
	int i;

	for (i = 0; i < queue->nr_cmds; i++, cmd++) {
		if (nvmet_tcp_need_data_in(cmd))
			nvmet_req_uninit(&cmd->req);
	}

	if (!queue->nr_cmds && nvmet_tcp_need_data_in(&queue->connect)) {
		/* failed in connect */
		nvmet_req_uninit(&queue->connect.req);
	}
}

static void nvmet_tcp_free_cmd_data_in_buffers(struct nvmet_tcp_queue *queue)
{
	struct nvmet_tcp_cmd *cmd = queue->cmds;
	int i;

	for (i = 0; i < queue->nr_cmds; i++, cmd++)
		nvmet_tcp_free_cmd_buffers(cmd);
	nvmet_tcp_free_cmd_buffers(&queue->connect);
}

static void nvmet_tcp_release_queue_work(struct work_struct *w)
{
	struct nvmet_tcp_queue *queue =
		container_of(w, struct nvmet_tcp_queue, release_work);

	mutex_lock(&nvmet_tcp_queue_mutex);
	list_del_init(&queue->queue_list);
	mutex_unlock(&nvmet_tcp_queue_mutex);

	nvmet_tcp_restore_socket_callbacks(queue);
	cancel_delayed_work_sync(&queue->tls_handshake_tmo_work);
	cancel_work_sync(&queue->io_work);
	/* stop accepting incoming data */
	queue->rcv_state = NVMET_TCP_RECV_ERR;
	/*
	 * Off the budget's FIFO at once, not after nvmet_sq_destroy(): the
	 * kref is at zero, so a kick could not pass a queued waiter on.
	 * io_work admits nothing more (nvmet_tcp_rx_dying()).
	 */
	rx_loan_unwait(&queue->rx_wait);

	nvmet_sq_put_tls_key(&queue->nvme_sq);
	nvmet_tcp_uninit_data_in_cmds(queue);
	nvmet_sq_destroy(&queue->nvme_sq);
	nvmet_cq_put(&queue->nvme_cq);
	cancel_work_sync(&queue->io_work);
	nvmet_tcp_rx_drain_done(queue);
	/* Backstop: io_work can run no more. */
	rx_loan_unwait(&queue->rx_wait);
	nvmet_tcp_free_cmd_data_in_buffers(queue);
	/* ->sock will be released by fput() */
	fput(queue->sock->file);
	nvmet_tcp_free_cmds(queue);
	ida_free(&nvmet_tcp_queue_ida, queue->idx);
	page_frag_cache_drain(&queue->pf_cache);
	kfree(queue);
}

static void nvmet_tcp_data_ready(struct sock *sk)
{
	struct nvmet_tcp_queue *queue;

	trace_sk_data_ready(sk);

	read_lock_bh(&sk->sk_callback_lock);
	queue = sk->sk_user_data;
	if (likely(queue)) {
		if (queue->data_ready)
			queue->data_ready(sk);
		if (queue->state != NVMET_TCP_Q_TLS_HANDSHAKE)
			queue_work_on(queue_cpu(queue), nvmet_tcp_wq,
				      &queue->io_work);
	}
	read_unlock_bh(&sk->sk_callback_lock);
}

static void nvmet_tcp_write_space(struct sock *sk)
{
	struct nvmet_tcp_queue *queue;

	read_lock_bh(&sk->sk_callback_lock);
	queue = sk->sk_user_data;
	if (unlikely(!queue))
		goto out;

	if (unlikely(queue->state == NVMET_TCP_Q_CONNECTING)) {
		queue->write_space(sk);
		goto out;
	}

	if (sk_stream_is_writeable(sk)) {
		clear_bit(SOCK_NOSPACE, &sk->sk_socket->flags);
		queue_work_on(queue_cpu(queue), nvmet_tcp_wq, &queue->io_work);
	}
out:
	read_unlock_bh(&sk->sk_callback_lock);
}

static void nvmet_tcp_state_change(struct sock *sk)
{
	struct nvmet_tcp_queue *queue;

	read_lock_bh(&sk->sk_callback_lock);
	queue = sk->sk_user_data;
	if (!queue)
		goto done;

	switch (sk->sk_state) {
	case TCP_FIN_WAIT2:
	case TCP_LAST_ACK:
	case TCP_CLOSING:
		break;
	case TCP_FIN_WAIT1:
	case TCP_CLOSE_WAIT:
	case TCP_CLOSE:
		/* FALLTHRU */
		nvmet_tcp_schedule_release_queue(queue);
		break;
	default:
		pr_warn("queue %d unhandled state %d\n",
			queue->idx, sk->sk_state);
	}
done:
	read_unlock_bh(&sk->sk_callback_lock);
}

static int nvmet_tcp_set_queue_sock(struct nvmet_tcp_queue *queue)
{
	struct socket *sock = queue->sock;
	struct inet_sock *inet = inet_sk(sock->sk);
	int ret;

	ret = kernel_getsockname(sock,
		(struct sockaddr *)&queue->sockaddr);
	if (ret < 0)
		return ret;

	ret = kernel_getpeername(sock,
		(struct sockaddr *)&queue->sockaddr_peer);
	if (ret < 0)
		return ret;

	/*
	 * Cleanup whatever is sitting in the TCP transmit queue on socket
	 * close. This is done to prevent stale data from being sent should
	 * the network connection be restored before TCP times out.
	 */
	sock_no_linger(sock->sk);

	if (so_priority > 0)
		sock_set_priority(sock->sk, so_priority);

	/* Set socket type of service */
	if (inet->rcv_tos > 0)
		ip_sock_set_tos(sock->sk, inet->rcv_tos);

	/*
	 * Opened with the held listener's buffer, unlocked: hold it before the
	 * first read, as a lending queue is held; nvmet_tcp_rx_setup() gives
	 * it back if the queue never lends.
	 */
	if (READ_ONCE(queue->port->rx_hold.held))
		nvmet_tcp_rx_hold_apply(&queue->rx_hold, sock->sk,
					&nvmet_tcp_rx_budget);

	ret = 0;
	write_lock_bh(&sock->sk->sk_callback_lock);
	if (sock->sk->sk_state != TCP_ESTABLISHED) {
		/*
		 * If the socket is already closing, don't even start
		 * consuming it
		 */
		ret = -ENOTCONN;
	} else {
		sock->sk->sk_user_data = queue;
		queue->data_ready = sock->sk->sk_data_ready;
		sock->sk->sk_data_ready = nvmet_tcp_data_ready;
		queue->state_change = sock->sk->sk_state_change;
		sock->sk->sk_state_change = nvmet_tcp_state_change;
		queue->write_space = sock->sk->sk_write_space;
		sock->sk->sk_write_space = nvmet_tcp_write_space;
		if (idle_poll_period_usecs)
			nvmet_tcp_arm_queue_deadline(queue);
		queue_work_on(queue_cpu(queue), nvmet_tcp_wq, &queue->io_work);
	}
	write_unlock_bh(&sock->sk->sk_callback_lock);

	return ret;
}

#ifdef CONFIG_NVME_TARGET_TCP_TLS
static int nvmet_tcp_try_peek_pdu(struct nvmet_tcp_queue *queue)
{
	struct nvme_tcp_hdr *hdr = &queue->pdu.cmd.hdr;
	int len, ret;
	struct kvec iov = {
		.iov_base = (u8 *)&queue->pdu + queue->offset,
		.iov_len = sizeof(struct nvme_tcp_hdr),
	};
	char cbuf[CMSG_LEN(sizeof(char))] = {};
	struct msghdr msg = {
		.msg_control = cbuf,
		.msg_controllen = sizeof(cbuf),
		.msg_flags = MSG_PEEK,
	};

	if (nvmet_port_secure_channel_required(queue->port->nport))
		return 0;

	len = kernel_recvmsg(queue->sock, &msg, &iov, 1,
			iov.iov_len, msg.msg_flags);
	if (unlikely(len < 0)) {
		pr_debug("queue %d: peek error %d\n",
			 queue->idx, len);
		return len;
	}

	ret = nvmet_tcp_tls_record_ok(queue, &msg, cbuf);
	if (ret < 0)
		return ret;

	if (len < sizeof(struct nvme_tcp_hdr)) {
		pr_debug("queue %d: short read, %d bytes missing\n",
			 queue->idx, (int)iov.iov_len - len);
		return -EAGAIN;
	}
	pr_debug("queue %d: hdr type %d hlen %d plen %d size %d\n",
		 queue->idx, hdr->type, hdr->hlen, hdr->plen,
		 (int)sizeof(struct nvme_tcp_icreq_pdu));
	if (hdr->type == nvme_tcp_icreq &&
	    hdr->hlen == sizeof(struct nvme_tcp_icreq_pdu) &&
	    hdr->plen == cpu_to_le32(sizeof(struct nvme_tcp_icreq_pdu))) {
		pr_debug("queue %d: icreq detected\n",
			 queue->idx);
		return len;
	}
	return 0;
}

static int nvmet_tcp_tls_key_lookup(struct nvmet_tcp_queue *queue,
				    key_serial_t peerid)
{
	struct key *tls_key = nvme_tls_key_lookup(peerid);
	int status = 0;

	if (IS_ERR(tls_key)) {
		pr_warn("%s: queue %d failed to lookup key %x\n",
			__func__, queue->idx, peerid);
		spin_lock_bh(&queue->state_lock);
		queue->state = NVMET_TCP_Q_FAILED;
		spin_unlock_bh(&queue->state_lock);
		status = PTR_ERR(tls_key);
	} else {
		pr_debug("%s: queue %d using TLS PSK %x\n",
			 __func__, queue->idx, peerid);
		queue->nvme_sq.tls_key = tls_key;
	}
	return status;
}

static void nvmet_tcp_tls_handshake_done(void *data, int status,
					 key_serial_t peerid)
{
	struct nvmet_tcp_queue *queue = data;

	pr_debug("queue %d: TLS handshake done, key %x, status %d\n",
		 queue->idx, peerid, status);
	spin_lock_bh(&queue->state_lock);
	if (WARN_ON(queue->state != NVMET_TCP_Q_TLS_HANDSHAKE)) {
		spin_unlock_bh(&queue->state_lock);
		return;
	}
	if (!status) {
		queue->tls_pskid = peerid;
		queue->state = NVMET_TCP_Q_CONNECTING;
	} else
		queue->state = NVMET_TCP_Q_FAILED;
	spin_unlock_bh(&queue->state_lock);

	cancel_delayed_work_sync(&queue->tls_handshake_tmo_work);

	if (!status)
		status = nvmet_tcp_tls_key_lookup(queue, peerid);

	if (!status)
		status = nvmet_tcp_set_queue_sock(queue);

	if (status)
		nvmet_tcp_schedule_release_queue(queue);
	kref_put(&queue->kref, nvmet_tcp_release_queue);
}

static void nvmet_tcp_tls_handshake_timeout(struct work_struct *w)
{
	struct nvmet_tcp_queue *queue = container_of(to_delayed_work(w),
			struct nvmet_tcp_queue, tls_handshake_tmo_work);

	pr_warn("queue %d: TLS handshake timeout\n", queue->idx);
	/*
	 * If tls_handshake_cancel() fails we've lost the race with
	 * nvmet_tcp_tls_handshake_done() */
	if (!tls_handshake_cancel(queue->sock->sk))
		return;
	spin_lock_bh(&queue->state_lock);
	if (WARN_ON(queue->state != NVMET_TCP_Q_TLS_HANDSHAKE)) {
		spin_unlock_bh(&queue->state_lock);
		return;
	}
	queue->state = NVMET_TCP_Q_FAILED;
	spin_unlock_bh(&queue->state_lock);
	nvmet_tcp_schedule_release_queue(queue);
	kref_put(&queue->kref, nvmet_tcp_release_queue);
}

static int nvmet_tcp_tls_handshake(struct nvmet_tcp_queue *queue)
{
	int ret = -EOPNOTSUPP;
	struct tls_handshake_args args;

	if (queue->state != NVMET_TCP_Q_TLS_HANDSHAKE) {
		pr_warn("cannot start TLS in state %d\n", queue->state);
		return -EINVAL;
	}

	kref_get(&queue->kref);
	pr_debug("queue %d: TLS ServerHello\n", queue->idx);
	memset(&args, 0, sizeof(args));
	args.ta_sock = queue->sock;
	args.ta_done = nvmet_tcp_tls_handshake_done;
	args.ta_data = queue;
	args.ta_keyring = key_serial(queue->port->nport->keyring);
	args.ta_timeout_ms = tls_handshake_timeout * 1000;

	ret = tls_server_hello_psk(&args, GFP_KERNEL);
	if (ret) {
		kref_put(&queue->kref, nvmet_tcp_release_queue);
		pr_err("failed to start TLS, err=%d\n", ret);
	} else {
		queue_delayed_work(nvmet_wq, &queue->tls_handshake_tmo_work,
				   tls_handshake_timeout * HZ);
	}
	return ret;
}
#else
static void nvmet_tcp_tls_handshake_timeout(struct work_struct *w) {}
#endif

static void nvmet_tcp_alloc_queue(struct nvmet_tcp_port *port,
		struct socket *newsock)
{
	struct nvmet_tcp_queue *queue;
	struct file *sock_file = NULL;
	int ret;

	queue = kzalloc_obj(*queue);
	if (!queue) {
		ret = -ENOMEM;
		goto out_release;
	}

	INIT_WORK(&queue->release_work, nvmet_tcp_release_queue_work);
	INIT_WORK(&queue->io_work, nvmet_tcp_io_work);
	kref_init(&queue->kref);
	queue->sock = newsock;
	queue->port = port;
	queue->nr_cmds = 0;
	spin_lock_init(&queue->state_lock);
	if (queue->port->nport->disc_addr.tsas.tcp.sectype ==
	    NVMF_TCP_SECTYPE_TLS13)
		queue->state = NVMET_TCP_Q_TLS_HANDSHAKE;
	else
		queue->state = NVMET_TCP_Q_CONNECTING;
	INIT_LIST_HEAD(&queue->free_list);
	init_llist_head(&queue->resp_list);
	INIT_LIST_HEAD(&queue->resp_send_list);
	INIT_LIST_HEAD(&queue->rx_withheld);
	init_llist_head(&queue->rx_done);
	INIT_LIST_HEAD(&queue->rx_wait.node);
	queue->rx_wait.ops = &nvmet_tcp_rx_waiter_ops;

	sock_file = sock_alloc_file(queue->sock, O_CLOEXEC, NULL);
	if (IS_ERR(sock_file)) {
		ret = PTR_ERR(sock_file);
		goto out_free_queue;
	}

	queue->idx = ida_alloc(&nvmet_tcp_queue_ida, GFP_KERNEL);
	if (queue->idx < 0) {
		ret = queue->idx;
		goto out_sock;
	}

	ret = nvmet_tcp_alloc_cmd(queue, &queue->connect);
	if (ret)
		goto out_ida_remove;

	nvmet_cq_init(&queue->nvme_cq);
	ret = nvmet_sq_init(&queue->nvme_sq, &queue->nvme_cq);
	if (ret)
		goto out_free_connect;

	nvmet_prepare_receive_pdu(queue);

	mutex_lock(&nvmet_tcp_queue_mutex);
	list_add_tail(&queue->queue_list, &nvmet_tcp_queue_list);
	mutex_unlock(&nvmet_tcp_queue_mutex);

	INIT_DELAYED_WORK(&queue->tls_handshake_tmo_work,
			  nvmet_tcp_tls_handshake_timeout);
#ifdef CONFIG_NVME_TARGET_TCP_TLS
	if (queue->state == NVMET_TCP_Q_TLS_HANDSHAKE) {
		struct sock *sk = queue->sock->sk;

		/* Restore the default callbacks before starting upcall */
		write_lock_bh(&sk->sk_callback_lock);
		sk->sk_user_data = NULL;
		sk->sk_data_ready = port->data_ready;
		write_unlock_bh(&sk->sk_callback_lock);
		if (!nvmet_tcp_try_peek_pdu(queue)) {
			if (!nvmet_tcp_tls_handshake(queue))
				return;
			/* TLS handshake failed, terminate the connection */
			goto out_destroy_sq;
		}
		/* Not a TLS connection, continue with normal processing */
		queue->state = NVMET_TCP_Q_CONNECTING;
	}
#endif

	ret = nvmet_tcp_set_queue_sock(queue);
	if (ret)
		goto out_destroy_sq;

	return;
out_destroy_sq:
	mutex_lock(&nvmet_tcp_queue_mutex);
	list_del_init(&queue->queue_list);
	mutex_unlock(&nvmet_tcp_queue_mutex);
	nvmet_sq_destroy(&queue->nvme_sq);
out_free_connect:
	nvmet_cq_put(&queue->nvme_cq);
	nvmet_tcp_free_cmd(&queue->connect);
out_ida_remove:
	ida_free(&nvmet_tcp_queue_ida, queue->idx);
	/*
	 * Drain the page fragment cache if any allocations were done.
	 * The first allocation using pf_cache is nvmet_tcp_alloc_cmd()
	 * for queue->connect after ida_alloc().
	 */
	page_frag_cache_drain(&queue->pf_cache);
out_sock:
	fput(queue->sock->file);
out_free_queue:
	kfree(queue);
out_release:
	pr_err("failed to allocate queue, error %d\n", ret);
	if (!sock_file)
		sock_release(newsock);
}

static void nvmet_tcp_accept_work(struct work_struct *w)
{
	struct nvmet_tcp_port *port =
		container_of(w, struct nvmet_tcp_port, accept_work);
	struct rx_loan_budget *lb = &nvmet_tcp_rx_budget;
	struct socket *newsock;
	bool lends;
	int ret;

	/*
	 * Once a queue has carved the region (nvmet_tcp_rx_setup()), hold the
	 * listener's first window (rx_loan_hold_listen(), which leaves the
	 * window scale stock); each connection is then held before its first
	 * read (nvmet_tcp_set_queue_sock()).  A port none of whose queues can
	 * lend (TLS only, or the region or budget gone) keeps or gets back a
	 * stock listener.
	 */
	lends = net_eq(sock_net(port->sock->sk), &init_net) &&
		!nvmet_port_secure_channel_required(port->nport) &&
		rx_loan_region_size() && READ_ONCE(lb->budget);
	if (lends && !port->rx_hold.held) {
		rx_loan_hold_listen(&port->rx_hold, port->sock->sk,
				    NVMET_TCP_RX_RCVBUF,
				    rx_loan_hold_factor(&port->rx_hold, lb));
	} else if (!lends && port->rx_hold.held) {
		rx_loan_hold_release(port->sock->sk);
		WRITE_ONCE(port->rx_hold.held, false);
		port->rx_hold.factor = 0;
	}

	while (true) {
		ret = kernel_accept(port->sock, &newsock, O_NONBLOCK);
		if (ret < 0) {
			if (ret != -EAGAIN)
				pr_warn("failed to accept err=%d\n", ret);
			return;
		}
		nvmet_tcp_alloc_queue(port, newsock);
	}
}

static void nvmet_tcp_listen_data_ready(struct sock *sk)
{
	struct nvmet_tcp_port *port;

	trace_sk_data_ready(sk);

	if (sk->sk_state != TCP_LISTEN)
		return;

	read_lock_bh(&sk->sk_callback_lock);
	port = sk->sk_user_data;
	if (port)
		queue_work(nvmet_wq, &port->accept_work);
	read_unlock_bh(&sk->sk_callback_lock);
}

static int nvmet_tcp_add_port(struct nvmet_port *nport)
{
	struct nvmet_tcp_port *port;
	__kernel_sa_family_t af;
	int ret;

	port = kzalloc_obj(*port);
	if (!port)
		return -ENOMEM;

	switch (nport->disc_addr.adrfam) {
	case NVMF_ADDR_FAMILY_IP4:
		af = AF_INET;
		break;
	case NVMF_ADDR_FAMILY_IP6:
		af = AF_INET6;
		break;
	default:
		pr_err("address family %d not supported\n",
				nport->disc_addr.adrfam);
		ret = -EINVAL;
		goto err_port;
	}

	ret = inet_pton_with_scope(&init_net, af, nport->disc_addr.traddr,
			nport->disc_addr.trsvcid, &port->addr);
	if (ret) {
		pr_err("malformed ip/port passed: %s:%s\n",
			nport->disc_addr.traddr, nport->disc_addr.trsvcid);
		goto err_port;
	}

	port->nport = nport;
	INIT_WORK(&port->accept_work, nvmet_tcp_accept_work);
	if (port->nport->inline_data_size < 0)
		port->nport->inline_data_size = NVMET_TCP_DEF_INLINE_DATA_SIZE;

	ret = sock_create(port->addr.ss_family, SOCK_STREAM,
				IPPROTO_TCP, &port->sock);
	if (ret) {
		pr_err("failed to create a socket\n");
		goto err_port;
	}

	port->sock->sk->sk_user_data = port;
	port->data_ready = port->sock->sk->sk_data_ready;
	port->sock->sk->sk_data_ready = nvmet_tcp_listen_data_ready;
	sock_set_reuseaddr(port->sock->sk);
	tcp_sock_set_nodelay(port->sock->sk);
	if (so_priority > 0)
		sock_set_priority(port->sock->sk, so_priority);

	ret = kernel_bind(port->sock, (struct sockaddr_unsized *)&port->addr,
			sizeof(port->addr));
	if (ret) {
		pr_err("failed to bind port socket %d\n", ret);
		goto err_sock;
	}

	ret = kernel_listen(port->sock, NVMET_TCP_BACKLOG);
	if (ret) {
		pr_err("failed to listen %d on port sock\n", ret);
		goto err_sock;
	}

	nport->priv = port;
	pr_info("enabling port %d (%pISpc)\n",
		le16_to_cpu(nport->disc_addr.portid), &port->addr);

	return 0;

err_sock:
	sock_release(port->sock);
err_port:
	kfree(port);
	return ret;
}

static void nvmet_tcp_destroy_port_queues(struct nvmet_tcp_port *port)
{
	struct nvmet_tcp_queue *queue;

	mutex_lock(&nvmet_tcp_queue_mutex);
	list_for_each_entry(queue, &nvmet_tcp_queue_list, queue_list)
		if (queue->port == port)
			kernel_sock_shutdown(queue->sock, SHUT_RDWR);
	mutex_unlock(&nvmet_tcp_queue_mutex);
}

static void nvmet_tcp_remove_port(struct nvmet_port *nport)
{
	struct nvmet_tcp_port *port = nport->priv;

	write_lock_bh(&port->sock->sk->sk_callback_lock);
	port->sock->sk->sk_data_ready = port->data_ready;
	port->sock->sk->sk_user_data = NULL;
	write_unlock_bh(&port->sock->sk->sk_callback_lock);
	cancel_work_sync(&port->accept_work);
	/*
	 * Destroy the remaining queues, which are not belong to any
	 * controller yet.
	 */
	nvmet_tcp_destroy_port_queues(port);

	sock_release(port->sock);
	kfree(port);
}

static void nvmet_tcp_delete_ctrl(struct nvmet_ctrl *ctrl)
{
	struct nvmet_tcp_queue *queue;

	mutex_lock(&nvmet_tcp_queue_mutex);
	list_for_each_entry(queue, &nvmet_tcp_queue_list, queue_list)
		if (queue->nvme_sq.ctrl == ctrl)
			kernel_sock_shutdown(queue->sock, SHUT_RDWR);
	mutex_unlock(&nvmet_tcp_queue_mutex);
}

static u16 nvmet_tcp_install_queue(struct nvmet_sq *sq)
{
	struct nvmet_tcp_queue *queue =
		container_of(sq, struct nvmet_tcp_queue, nvme_sq);

	if (sq->qid == 0) {
		struct nvmet_tcp_queue *q;
		int pending = 0;

		/* Check for pending controller teardown */
		mutex_lock(&nvmet_tcp_queue_mutex);
		list_for_each_entry(q, &nvmet_tcp_queue_list, queue_list) {
			if (q->nvme_sq.ctrl == sq->ctrl &&
			    q->state == NVMET_TCP_Q_DISCONNECTING)
				pending++;
		}
		mutex_unlock(&nvmet_tcp_queue_mutex);
		if (pending > NVMET_TCP_BACKLOG)
			return NVME_SC_CONNECT_CTRL_BUSY;
	}

	queue->nr_cmds = sq->size * 2;
	if (nvmet_tcp_alloc_cmds(queue)) {
		queue->nr_cmds = 0;
		return NVME_SC_INTERNAL;
	}
	return 0;
}

static void nvmet_tcp_disc_port_addr(struct nvmet_req *req,
		struct nvmet_port *nport, char *traddr)
{
	struct nvmet_tcp_port *port = nport->priv;

	if (inet_addr_is_any(&port->addr)) {
		struct nvmet_tcp_cmd *cmd =
			container_of(req, struct nvmet_tcp_cmd, req);
		struct nvmet_tcp_queue *queue = cmd->queue;

		sprintf(traddr, "%pISc", (struct sockaddr *)&queue->sockaddr);
	} else {
		memcpy(traddr, nport->disc_addr.traddr, NVMF_TRADDR_SIZE);
	}
}

static ssize_t nvmet_tcp_host_port_addr(struct nvmet_ctrl *ctrl,
			char *traddr, size_t traddr_len)
{
	struct nvmet_sq *sq = ctrl->sqs[0];
	struct nvmet_tcp_queue *queue =
		container_of(sq, struct nvmet_tcp_queue, nvme_sq);

	if (queue->sockaddr_peer.ss_family == AF_UNSPEC)
		return -EINVAL;
	return snprintf(traddr, traddr_len, "%pISc",
			(struct sockaddr *)&queue->sockaddr_peer);
}

static const struct nvmet_fabrics_ops nvmet_tcp_ops = {
	.owner			= THIS_MODULE,
	.type			= NVMF_TRTYPE_TCP,
	.msdbd			= 1,
	.add_port		= nvmet_tcp_add_port,
	.remove_port		= nvmet_tcp_remove_port,
	.queue_response		= nvmet_tcp_queue_response,
	.data_done		= nvmet_tcp_data_done,
	.delete_ctrl		= nvmet_tcp_delete_ctrl,
	.install_queue		= nvmet_tcp_install_queue,
	.disc_traddr		= nvmet_tcp_disc_port_addr,
	.host_traddr		= nvmet_tcp_host_port_addr,
};

static int __init nvmet_tcp_init(void)
{
	int ret;

	nvmet_tcp_wq = alloc_workqueue("nvmet_tcp_wq",
				WQ_MEM_RECLAIM | WQ_HIGHPRI | WQ_PERCPU, 0);
	if (!nvmet_tcp_wq)
		return -ENOMEM;

	ret = nvmet_tcp_rx_budget_init(&nvmet_tcp_rx_budget);
	if (ret)
		goto err_wq;

	ret = nvmet_register_transport(&nvmet_tcp_ops);
	if (ret)
		goto err;

	return 0;
err:
	rx_loan_budget_destroy(&nvmet_tcp_rx_budget);
err_wq:
	destroy_workqueue(nvmet_tcp_wq);
	return ret;
}

static void __exit nvmet_tcp_exit(void)
{
	struct nvmet_tcp_queue *queue;

	nvmet_unregister_transport(&nvmet_tcp_ops);

	flush_workqueue(nvmet_wq);
	mutex_lock(&nvmet_tcp_queue_mutex);
	list_for_each_entry(queue, &nvmet_tcp_queue_list, queue_list)
		kernel_sock_shutdown(queue->sock, SHUT_RDWR);
	mutex_unlock(&nvmet_tcp_queue_mutex);
	flush_workqueue(nvmet_wq);
	mutex_lock(&nvmet_tcp_rx_consumer_lock);
	if (nvmet_tcp_rx_consumer_on)
		rx_loan_consumer_unregister(&nvmet_tcp_rx_consumer);
	nvmet_tcp_rx_consumer_on = false;
	mutex_unlock(&nvmet_tcp_rx_consumer_lock);
	/* its queues, and so the commands counting copies to it, are gone */
	rx_loan_consumer_destroy(&nvmet_tcp_rx_consumer);
	rx_loan_budget_destroy(&nvmet_tcp_rx_budget);

	destroy_workqueue(nvmet_tcp_wq);
	ida_destroy(&nvmet_tcp_queue_ida);
}

module_init(nvmet_tcp_init);
module_exit(nvmet_tcp_exit);

MODULE_DESCRIPTION("NVMe target TCP transport driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("nvmet-transport-3"); /* 3 == NVMF_TRTYPE_TCP */

#if IS_ENABLED(CONFIG_NVME_TARGET_TCP_KUNIT_TEST)
#include "tcp_rx_loan_kunit.c"
#endif
