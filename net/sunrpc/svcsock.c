// SPDX-License-Identifier: GPL-2.0-only
/*
 * linux/net/sunrpc/svcsock.c
 *
 * These are the RPC server socket internals.
 *
 * The server scheduling algorithm does not always distribute the load
 * evenly when servicing a single client. May need to modify the
 * svc_xprt_enqueue procedure...
 *
 * Copyright (C) 1995, 1996 Olaf Kirch <okir@monad.swb.de>
 */

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/sysfs.h>
#include <linux/math64.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/net.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/udp.h>
#include <linux/tcp.h>
#include <linux/unistd.h>
#include <linux/slab.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/skbuff_ref.h>
#include <linux/file.h>
#include <linux/freezer.h>
#include <linux/bvec.h>

#include <net/sock.h>
#include <net/checksum.h>
#include <net/ip.h>
#include <net/ipv6.h>
#include <net/udp.h>
#include <net/tcp.h>
#include <net/tcp_states.h>
#include <net/page_pool/helpers.h>
#include <net/rx_loan.h>
#include <net/xdp.h>
#include <net/netns/generic.h>
#include <net/tls_prot.h>
#include <net/handshake.h>
#include <linux/uaccess.h>
#include <linux/highmem.h>
#include <asm/ioctls.h>
#include <linux/key.h>
#include <linux/bitmap.h>
#include <linux/jump_label.h>
#include <kunit/visibility.h>

#include <linux/sunrpc/types.h>
#include <linux/sunrpc/clnt.h>
#include <linux/sunrpc/xdr.h>
#include <linux/sunrpc/msg_prot.h>
#include <linux/sunrpc/svcsock.h>
#include <linux/sunrpc/stats.h>
#include <linux/sunrpc/xprt.h>

#include <trace/events/sock.h>
#include <trace/events/sunrpc.h>

#include "socklib.h"
#include "sunrpc.h"
#include "netns.h"
#include <linux/debugfs.h>

#define RPCDBG_FACILITY	RPCDBG_SVCXPRT

/*
 * A whole record this small (an NFSv4 SEQUENCE, GETATTR or COMMIT
 * compound) costs less to copy than a loaned receive page, and a loan
 * would pin that page until the RPC completes, storage waits included.
 */
VISIBLE_IF_KUNIT unsigned int svc_tcp_rx_copy_below __read_mostly = 1024;
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_copy_below);
module_param(svc_tcp_rx_copy_below, uint, 0644);
MODULE_PARM_DESC(svc_tcp_rx_copy_below,
		 "Copy, rather than loan, whole RPC records of at most this many bytes (0: loan all)");

/*
 * Asked once the marker is in.  The record is whole in this fragment when
 * every body octet received so far is this fragment's: sk_datalen also
 * counts the octets earlier actor calls took of it (a prefix split across
 * skbs), so it alone would loan a small call, and a loaned call's READ
 * cannot be confined.
 */
static bool svc_tcp_rx_small_record(struct svc_sock *svsk)
{
	return svc_sock_final_rec(svsk) &&
	       svsk->sk_datalen + sizeof(rpc_fraghdr) == svsk->sk_tcplen &&
	       svc_sock_reclen(svsk) <= READ_ONCE(svc_tcp_rx_copy_below);
}

#if IS_ENABLED(CONFIG_KUNIT)
static enum svc_tcp_rx_kunit_fault svc_tcp_rx_kunit_fault;
static unsigned int svc_tcp_rx_kunit_fault_skip;
static u32 svc_tcp_rx_kunit_fault_acquired;
static u32 svc_tcp_rx_kunit_fault_released;

void svc_tcp_rx_kunit_fault_set(enum svc_tcp_rx_kunit_fault fault,
				unsigned int skip)
{
	svc_tcp_rx_kunit_fault = fault;
	svc_tcp_rx_kunit_fault_skip = skip;
	svc_tcp_rx_kunit_fault_acquired = 0;
	svc_tcp_rx_kunit_fault_released = 0;
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_kunit_fault_set);

void svc_tcp_rx_kunit_fault_observed(u32 *acquired, u32 *released)
{
	*acquired = svc_tcp_rx_kunit_fault_acquired;
	*released = svc_tcp_rx_kunit_fault_released;
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_kunit_fault_observed);

static bool
svc_tcp_rx_kunit_should_fail(enum svc_tcp_rx_kunit_fault fault,
			     const struct svc_tcp_rx_state *state)
{
	if (svc_tcp_rx_kunit_fault != fault)
		return false;
	if (svc_tcp_rx_kunit_fault_skip) {
		svc_tcp_rx_kunit_fault_skip--;
		return false;
	}
	svc_tcp_rx_kunit_fault = SVC_TCP_RX_KUNIT_FAULT_NONE;
	svc_tcp_rx_kunit_fault_acquired = state->lt.refs_acquired;
	svc_tcp_rx_kunit_fault_released = state->lt.refs_released;
	return true;
}
#endif

/*
 * For UDP:
 * 1 for header page
 * enough pages for RPCSVC_MAXPAYLOAD_UDP
 * 1 in case payload is not aligned
 * 1 for tail page
 */
enum {
	SUNRPC_MAX_UDP_SENDPAGES = 1 + RPCSVC_MAXPAYLOAD_UDP / PAGE_SIZE + 1 + 1
};

/* To-do: to avoid tying up an nfsd thread while waiting for a
 * handshake request, the request could instead be deferred.
 */
enum {
	SVC_HANDSHAKE_TO	= 5U * HZ
};

static struct svc_sock *svc_setup_socket(struct svc_serv *, struct socket *,
					 int flags);
static int		svc_udp_recvfrom(struct svc_rqst *);
static int		svc_udp_sendto(struct svc_rqst *);
static void		svc_sock_detach(struct svc_xprt *);
static void		svc_tcp_sock_detach(struct svc_xprt *);
static void		svc_sock_free(struct svc_xprt *);

static struct svc_xprt *svc_create_socket(struct svc_serv *, int,
					  struct net *, struct sockaddr *,
					  int, int);
#ifdef CONFIG_DEBUG_LOCK_ALLOC
static struct lock_class_key svc_key[2];
static struct lock_class_key svc_slock_key[2];

static void svc_reclassify_socket(struct socket *sock)
{
	struct sock *sk = sock->sk;

	if (WARN_ON_ONCE(!sock_allow_reclassification(sk)))
		return;

	switch (sk->sk_family) {
	case AF_INET:
		sock_lock_init_class_and_name(sk, "slock-AF_INET-NFSD",
					      &svc_slock_key[0],
					      "sk_xprt.xpt_lock-AF_INET-NFSD",
					      &svc_key[0]);
		break;

	case AF_INET6:
		sock_lock_init_class_and_name(sk, "slock-AF_INET6-NFSD",
					      &svc_slock_key[1],
					      "sk_xprt.xpt_lock-AF_INET6-NFSD",
					      &svc_key[1]);
		break;

	default:
		BUG();
	}
}
#else
static void svc_reclassify_socket(struct socket *sock)
{
}
#endif

/*
 * Region credit admission.  The budget is the limit that is configured;
 * credit is what one record draws against it and returns when it ends.
 *
 * Each network namespace has a budget of pages of
 * the fixed receive region that its TCP receive loans may hold, and one
 * default budget covers namespaces without one of their own; the budgets
 * together may not exceed the region's write partition, so a read partition
 * split off with page_pool.rx_region_read_pages never takes their pressure.
 * A namespace's budget is overridden in debugfs, rx_loan/<netns>/write/pages
 * and reserve (the initial namespace's kept in svc_tcp_rx_budget_pages and
 * svc_tcp_rx_budget_reserve); no module parameter or sysctl sets one.  The
 * default budget is set by KUnit only: in service it stays 0, and a
 * namespace without a budget copies its region records.
 *
 * A loaned record is admitted at its first skb that carries region pages,
 * against an estimated page cost, each later skb is charged the region loan
 * entries it can add, and a transport without credit stops reading so its
 * bytes stay in the TCP receive queue and the advertised window closes.
 * The estimate is learned: each budget keeps the most region pages a record
 * of its last one to two thousand held for each 100 pages of record.  How
 * many pages a record holds follows from how the device cuts its pages into
 * receive buffers and how connections' frames interleave in them, which
 * only the records show.  Until a budget's first record has ended, a record
 * is charged all it could hold, so records go one at a time.
 * Records that never carry a region page are never admitted.  A reserve that
 * ordinary admission cannot take lets the oldest in-flight record always
 * finish, so the receive path cannot deadlock.  Credit returns when a record
 * ends; waiting transports are woken oldest first.
 *
 * The budget is also what turns loans on: a record whose transport would
 * draw on a budget of 0 is copied whole (reason "no_budget"), whether its
 * pages are the region's or not, and with every budget at 0, the default,
 * nothing is loaned.  Pages from outside the region draw no credit.
 *
 * No region page is loaned without credit.  When the budget a transport
 * would draw on is 0, its region records are copied rather than loaned,
 * whether or not any other budget is set: with every budget at 0, loans of
 * region pages would be unbounded, and connections that each hold part of
 * a record can between them hold the whole region, so that no record can
 * receive the frames it still needs and none ever ends.  A copied record
 * holds region pages only while it is being copied, so reading always
 * returns them.
 *
 * Credit is direction-neutral.  A READ that places its payload in the region
 * draws on its namespace's READ budget (rx_loan_net()->read; for the
 * initial namespace the read pool, svc_rx_budget_read), under the
 * region-wide cap and with no parameters of its own, reported in
 * debugfs rx_loan/<netns inum>/read/stats, or on its transport's budget;
 * svc_tcp_rx_budget_read_for() picks:
 *  - split region: every READ draws on the read pool, whose budget is the
 *    read partition; namespaces are then isolated for receive only;
 *  - shared region, transport with a budget (its namespace's, else the
 *    default): the READ draws on that budget, as receive does;
 *  - shared region, transport whose budget is 0: the READ draws on the read
 *    pool, whose budget is then what the configured budgets leave of the
 *    region.
 * The read pool has no reserve: as for every budget, a reserve of 0 means
 * the oldest in-flight record overruns the budget rather than stall.  The
 * receive clamp (svc_tcp_rx_rcvbuf) bounds unread receive data, which READs
 * do not have, so it never gates the read pool.  The pool is configured when
 * the TCP transport class registers and again whenever a budget changes.
 */
static unsigned int svc_tcp_rx_budget_pages;
static unsigned int svc_tcp_rx_budget_reserve;
static unsigned int svc_tcp_rx_budget_default_pages;
static unsigned int svc_tcp_rx_budget_default_reserve;

/*
 * Two estimates that used to be one setting are found out as the server
 * runs: each budget learns what its loaned records hold
 * (svc_tcp_rx_cost_learn_locked()) and each transport measures what its
 * skbs occupy (svc_tcp_rx_geometry_account()).  Both are kept as a factor,
 * so many to the 100, and neither can be set: they follow from how the
 * device cuts its pages into receive buffers, from the MTU and from how
 * connections' frames interleave, and a value set by hand is wrong as soon
 * as one of those changes.
 */
/* Largest factor with a positive TCP_RMEM_TO_WIN_SCALE ratio. */
#define SVC_TCP_RX_CREDIT_FACTOR_MAX 12800U
/* What a receive buffer is held to before anything has been measured. */
#define SVC_TCP_RX_CREDIT_FACTOR_UNLEARNED	150U

/* The KUnit suite fixes the factor, to have charges it can count. */
#if IS_ENABLED(CONFIG_KUNIT)
static unsigned int svc_tcp_rx_kunit_factor;
#define svc_tcp_rx_fixed_factor()	READ_ONCE(svc_tcp_rx_kunit_factor)
#else
#define svc_tcp_rx_fixed_factor()	0U
#endif

static unsigned int svc_tcp_rx_rcvbuf;
/* svc_tcp_rx_rcvbuf was set (a parameter, or KUnit): it is what applies */
static bool svc_tcp_rx_rcvbuf_given;
/* a write budget, configured or computed, wants loans (loans_update) */
static bool svc_tcp_rx_rcvbuf_wanted;
static void svc_tcp_rx_budget_apply_all(void);
static bool svc_tcp_rx_hold_stale(struct svc_sock *svsk);
VISIBLE_IF_KUNIT void svc_tcp_rx_geometry_account(struct svc_sock *svsk,
						  unsigned int truesize,
						  unsigned int skb_len,
						  unsigned int used);
static void svc_tcp_rx_budget_init(void);

/* A clamp below SVC_TCP_RX_RCVBUF_MIN turns every budget off. */
static int svc_tcp_rx_rcvbuf_set(const char *val,
				 const struct kernel_param *kp)
{
	int ret = param_set_uint(val, kp);

	if (!ret) {
		WRITE_ONCE(svc_tcp_rx_rcvbuf_given, true);
		svc_tcp_rx_budget_apply_all();
	}
	return ret;
}

static const struct kernel_param_ops svc_tcp_rx_rcvbuf_ops = {
	.set = svc_tcp_rx_rcvbuf_set,
	.get = param_get_uint,
};
module_param_cb(svc_tcp_rx_rcvbuf, &svc_tcp_rx_rcvbuf_ops,
		&svc_tcp_rx_rcvbuf, 0644);
MODULE_PARM_DESC(svc_tcp_rx_rcvbuf,
		 "Receive buffer clamp in bytes for TCP transports set up afterwards (0: none)");

/*
 * A thread that takes a partly received record back from a socket leaves
 * its own pages in the socket's sk_pages[], for the next thread that parks
 * a record there to take in exchange.  The pages a socket keeps that way
 * are bounded, across all sockets, by this many bytes; past it a restore
 * frees the thread's pages as it would with no exchange.  0 keeps none.
 */
static unsigned long svc_tcp_spare_bytes_max = SZ_64M;
module_param(svc_tcp_spare_bytes_max, ulong, 0644);
MODULE_PARM_DESC(svc_tcp_spare_bytes_max,
		 "Bytes of pages TCP sockets keep, across all sockets, to exchange with threads that receive a record in pieces (0: none)");

static atomic_long_t svc_tcp_spare_pages = ATOMIC_LONG_INIT(0);

/*
 * Reserve room for up to @n spare pages under svc_tcp_spare_bytes_max.
 * Returns how many of them a socket may keep.  Reservations that race
 * may each keep fewer than there is room for, never more.
 */
static unsigned int svc_tcp_spare_reserve(unsigned int n)
{
	long max = READ_ONCE(svc_tcp_spare_bytes_max) >> PAGE_SHIFT;
	long total, over;

	if (!n)
		return 0;
	total = atomic_long_add_return(n, &svc_tcp_spare_pages);
	if (total <= max)
		return n;
	over = min_t(long, total - max, n);
	atomic_long_sub(over, &svc_tcp_spare_pages);
	return n - over;
}

static void svc_tcp_spare_release(struct svc_sock *svsk, unsigned int n)
{
	if (WARN_ON_ONCE(n > svsk->sk_spare_pages))
		n = svsk->sk_spare_pages;
	svsk->sk_spare_pages -= n;
	atomic_long_sub(n, &svc_tcp_spare_pages);
}

#if IS_ENABLED(CONFIG_KUNIT)
/* Set svc_tcp_spare_bytes_max; returns what it was. */
unsigned long svc_tcp_spare_kunit_set_max(unsigned long bytes)
{
	return xchg(&svc_tcp_spare_bytes_max, bytes);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_spare_kunit_set_max);

/* The pages all sockets keep for exchange. */
long svc_tcp_spare_kunit_pages(void)
{
	return atomic_long_read(&svc_tcp_spare_pages);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_spare_kunit_pages);
#endif

/*
 * The region budget bounds loans; only svc_tcp_rx_rcvbuf bounds unread data. A
 * smaller clamp holds so few page-pool segments that one short segment
 * overflows it, so budgets stay off below this.
 */
#define SVC_TCP_RX_RCVBUF_MIN	(128 * 1024)
/*
 * The clamp with no svc_tcp_rx_rcvbuf given, while a write budget wants
 * loans: what the qualified sizing ran with.
 */
#define SVC_TCP_RX_RCVBUF_DEFAULT	(256 * 1024)

/*
 * The receive clamp in force: svc_tcp_rx_rcvbuf if it was given; else
 * SVC_TCP_RX_RCVBUF_DEFAULT while a write budget, configured or computed,
 * wants loans and (@region) a region exists; else 0, stock autotuning.
 * The budget gates pass @region false: a budget staged before the region
 * is carved lends from it once it is, without a restage.
 */
static unsigned int __svc_tcp_rx_rcvbuf_effective(bool region)
{
	if (READ_ONCE(svc_tcp_rx_rcvbuf_given))
		return READ_ONCE(svc_tcp_rx_rcvbuf);
	if (!READ_ONCE(svc_tcp_rx_rcvbuf_wanted) ||
	    (region && !rx_loan_region_write_default()))
		return 0;
	return SVC_TCP_RX_RCVBUF_DEFAULT;
}

static unsigned int svc_tcp_rx_rcvbuf_effective(void)
{
	return __svc_tcp_rx_rcvbuf_effective(true);
}

/*
 * NFSD READ confinement: direct READ payloads in rx region pages, sent
 * zero-copy and returned when TCP frees the reply; a READ without pages
 * parks off-thread, never retried by the client.
 */
static bool svc_tcp_rx_region_reads __read_mostly;
module_param(svc_tcp_rx_region_reads, bool, 0644);
MODULE_PARM_DESC(svc_tcp_rx_region_reads,
		 "Confine NFSD READ payloads to the rx region, served by direct I/O (default: N)");

static unsigned int svc_tcp_rx_region_conn_share __read_mostly;
/* rx_loan/sunrpc/conn_share, one for all; 0: twice the largest READ */

/*
 * Q-C (decided): 10 s by default, a deliberate deviation from stock NFSD,
 * which sets no TCP_USER_TIMEOUT.
 */
static unsigned int svc_tcp_rx_region_user_timeout __read_mostly = 10000;
module_param(svc_tcp_rx_region_user_timeout, uint, 0644);
MODULE_PARM_DESC(svc_tcp_rx_region_user_timeout,
		 "TCP_USER_TIMEOUT in ms for connections accepted while READs are confined: the server resets a connection silent for this long (default: 10000, i.e. 10 s; 0: none, the stock behaviour)");

/*
 * A namespace's budget: the shared receive-loan budget (struct
 * rx_loan_budget, first, see there for its cache lines) and what sunrpc
 * keeps beside it.  Every record's lookup of its budget reads net, which
 * changes only with the configuration, so it is kept off the lines that
 * admission and release write.
 */
struct svc_tcp_rx_budget {
	/* only the default budget's and KUnit's: lb points here */
	struct rx_loan_budget	own;
	/*
	 * The budget it lends from: for a namespace, rx_loan_net()'s write
	 * budget (the read pool: the initial namespace's READ budget), which
	 * every consumer in the namespace shares; else own.
	 */
	struct rx_loan_budget	*lb ____cacheline_aligned_in_smp;
	/* NULL for the default budget and the read pool */
	struct net		*net;
	/* the namespace's rx_loan budget lb points at (KUnit may swap in own) */
	struct rx_loan_budget	*shared;
	/* a namespace's READ budget (rx_loan_net()->read), or the read pool */
	struct svc_tcp_rx_budget *read;
	/* the server that registered the consumer (svc_tcp_rx_serv_threads()) */
	struct svc_serv		*consumer_serv;
	struct list_head	node;		/* svc_rx_budget_pools */
	struct dentry		*debugfs;	/* rx_loan/<netns>/ */
	/* the namespace's server (nfsd), registered as it starts */
	struct rx_loan_consumer	consumer;
	atomic64_t		bounced ____cacheline_aligned_in_smp;
	atomic64_t		bounce_refused;
	/* confined NFSD READs (svc_tcp_rx_region_read) */
	atomic64_t		read_unconfined;
	atomic64_t		read_short;
	atomic64_t		share_stalls;
};

static DEFINE_PER_CPU(struct rx_loan_stock, svc_rx_stock_default);
static DEFINE_PER_CPU(struct rx_loan_stock, svc_rx_stock_read);

static struct svc_tcp_rx_budget svc_rx_budget_default = {
	.lb		= &svc_rx_budget_default.own,
	.own.owner	= &svc_rx_budget_default,
	.own.stock		= &svc_rx_stock_default,
	.own.lock		= __SPIN_LOCK_UNLOCKED(svc_rx_budget_default.lock),
	.own.next_ticket	= 1,
	.node		= LIST_HEAD_INIT(svc_rx_budget_default.node),
	.own.inflight	= LIST_HEAD_INIT(svc_rx_budget_default.own.inflight),
	.own.waiters	= LIST_HEAD_INIT(svc_rx_budget_default.own.waiters),
	.own.kick_work	= __WORK_INITIALIZER(svc_rx_budget_default.own.kick_work,
					     rx_loan_kick_fn),
};

/*
 * The global read pool: the read partition when split, else what the
 * configured budgets leave of the region.
 */
static struct svc_tcp_rx_budget svc_rx_budget_read = {
	.lb		= &svc_rx_budget_read.own,
	.own.owner	= &svc_rx_budget_read,
	.own.stock		= &svc_rx_stock_read,
	.own.lock		= __SPIN_LOCK_UNLOCKED(svc_rx_budget_read.lock),
	.own.read		= true,
	.own.next_ticket	= 1,
	.node		= LIST_HEAD_INIT(svc_rx_budget_read.node),
	.own.inflight	= LIST_HEAD_INIT(svc_rx_budget_read.own.inflight),
	.own.waiters	= LIST_HEAD_INIT(svc_rx_budget_read.own.waiters),
	.own.kick_work	= __WORK_INITIALIZER(svc_rx_budget_read.own.kick_work,
					     rx_loan_kick_fn),
};

/* @c's per-CPU stocks summed; exact only under c->lock with @c contended. */
struct svc_tcp_rx_stock_sum {
	long	pages, records, read_inflight, lent;
	u64	admitted, read_admitted, read_completed;
};

static void svc_tcp_rx_stock_sum(struct svc_tcp_rx_budget *c,
				 struct svc_tcp_rx_stock_sum *sum)
{
	int cpu;

	memset(sum, 0, sizeof(*sum));
	for_each_possible_cpu(cpu) {
		struct rx_loan_stock *st = per_cpu_ptr(c->lb->stock, cpu);

		sum->pages += atomic_long_read(&st->pages);
		sum->records += READ_ONCE(st->records);
		sum->read_inflight += READ_ONCE(st->read_inflight);
		sum->lent += READ_ONCE(st->lent);
		sum->admitted += READ_ONCE(st->admitted);
		sum->read_admitted += READ_ONCE(st->read_admitted);
		sum->read_completed += READ_ONCE(st->read_completed);
	}
}

static long svc_tcp_rx_read_inflight(struct svc_tcp_rx_budget *c)
{
	return rx_loan_read_inflight(c->lb);
}

static long svc_tcp_rx_stocked_records(struct svc_tcp_rx_budget *c)
{
	return rx_loan_stocked_records(c->lb);
}

static void svc_tcp_rx_stock_drain_locked(struct svc_tcp_rx_budget *c)
{
	rx_loan_stock_drain_locked(c->lb);
}

static long svc_tcp_rx_stock_put(struct svc_tcp_rx_budget *c, long n)
{
	return rx_loan_stock_put(c->lb, n);
}

/* Namespace budgets, the initial one included. */
static LIST_HEAD(svc_rx_budget_pools);
static struct svc_tcp_rx_budget *svc_rx_budget_init;
static DEFINE_MUTEX(svc_rx_budget_cfg_lock);
/* Sum of the configured budgets; written under svc_rx_budget_cfg_lock. */
static unsigned long svc_rx_budget_configured_total;

static struct svc_tcp_rx_budget *svc_tcp_rx_budget_net(const struct net *net)
{
	return ((struct sunrpc_net *)net_generic(net, sunrpc_net_id))->rx_budget;
}

/* The budget and reserve requested for @c. */
static void svc_tcp_rx_budget_requested(const struct svc_tcp_rx_budget *c,
					unsigned int *pages,
					unsigned int *reserve)
{
	if (c->lb->read) {
		*pages = rx_loan_read_pool_pages(READ_ONCE(svc_rx_budget_configured_total));
		*reserve = 0;
	} else if (!c->net) {
		*pages = READ_ONCE(svc_tcp_rx_budget_default_pages);
		*reserve = READ_ONCE(svc_tcp_rx_budget_default_reserve);
	} else if (net_eq(c->net, &init_net)) {
		*pages = READ_ONCE(svc_tcp_rx_budget_pages);
		*reserve = READ_ONCE(svc_tcp_rx_budget_reserve);
	} else {
		*pages = READ_ONCE(c->lb->cfg_pages);
		*reserve = READ_ONCE(c->lb->cfg_reserve);
	}
}

/*
 * The pages @c lends: what is requested of it, else, for a namespace's
 * write budget nobody configured, the region's write share once a region
 * exists (rx_loan_region_write_default()), so that kNFSd loans by default.
 */
static unsigned long svc_tcp_rx_budget_in_force(const struct svc_tcp_rx_budget *c)
{
	unsigned int pages, reserve;

	svc_tcp_rx_budget_requested(c, &pages, &reserve);
	if (pages || !c->net || c->lb->read || !READ_ONCE(c->lb->cfg_default) ||
	    __svc_tcp_rx_rcvbuf_effective(false) < SVC_TCP_RX_RCVBUF_MIN)
		return pages;
	return rx_loan_region_write_default();
}

static bool svc_tcp_rx_budget_is_init(const struct svc_tcp_rx_budget *c)
{
	return c->net && net_eq(c->net, &init_net);
}

/*
 * On while any budget is above 0.  With every budget at 0, the default,
 * nothing is loaned, and a record goes to the copy by a branch that is
 * patched out rather than by looking up the budget its transport would
 * draw on.
 */
static DEFINE_STATIC_KEY_FALSE(svc_tcp_rx_loans);

/* Turn svc_tcp_rx_loans on or off for the budgets as now requested. */
static void svc_tcp_rx_loans_update_locked(void)
{
	struct svc_tcp_rx_budget *pos;
	unsigned int pages, reserve;
	bool on;

	lockdep_assert_held(&svc_rx_budget_cfg_lock);
	on = READ_ONCE(svc_tcp_rx_budget_pages) ||
	     READ_ONCE(svc_tcp_rx_budget_default_pages);
	list_for_each_entry(pos, &svc_rx_budget_pools, node) {
		svc_tcp_rx_budget_requested(pos, &pages, &reserve);
		on |= pages != 0;
		/* a server with the computed default: on, copying until carved */
		on |= READ_ONCE(pos->consumer.registered) &&
		      READ_ONCE(pos->lb->cfg_default);
	}
	WRITE_ONCE(svc_tcp_rx_rcvbuf_wanted, on);
	if (on)
		static_branch_enable(&svc_tcp_rx_loans);
	else
		static_branch_disable(&svc_tcp_rx_loans);
}

static void svc_tcp_rx_budget_apply(struct svc_tcp_rx_budget *c);
static void svc_tcp_rx_budget_stage(struct svc_tcp_rx_budget *c);
#if IS_ENABLED(CONFIG_KUNIT)
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
static void svc_tcp_rx_budget_kunit_stage(void);
#else
static inline void svc_tcp_rx_budget_kunit_stage(void) { }
#endif

/* NULL until the initial namespace's sunrpc state exists. */
static struct svc_tcp_rx_budget *svc_tcp_rx_budget_initial(void)
{
	return svc_rx_budget_init;
}

/*
 * Set the budget of @target to @pages, unless the budgets together would
 * exceed the region's write partition.  @target is the default budget, a namespace's, or NULL
 * for the initial namespace's before it exists (set before it exists).
 */
static int svc_tcp_rx_budget_set(struct svc_tcp_rx_budget *target,
				 unsigned int pages)
{
	unsigned long region = rx_loan_region_size(), total = 0;
	unsigned long write = region - rx_loan_region_read_pages();
	bool init_target = !target || svc_tcp_rx_budget_is_init(target);
	struct svc_tcp_rx_budget *pos;
	unsigned int cur, reserve;
	bool init_counted = false;

	mutex_lock(&svc_rx_budget_cfg_lock);
	svc_tcp_rx_budget_requested(&svc_rx_budget_default, &cur, &reserve);
	total += target == &svc_rx_budget_default ? pages : cur;
	list_for_each_entry(pos, &svc_rx_budget_pools, node) {
		svc_tcp_rx_budget_requested(pos, &cur, &reserve);
		if (svc_tcp_rx_budget_is_init(pos)) {
			init_counted = true;
			if (init_target)
				cur = pages;
		} else if (pos == target) {
			cur = pages;
		}
		total += cur;
	}
	if (!init_counted)
		total += init_target ? pages : READ_ONCE(svc_tcp_rx_budget_pages);
	if (region && total > write) {
		mutex_unlock(&svc_rx_budget_cfg_lock);
		return -ENOSPC;
	}
	if (target == &svc_rx_budget_default) {
		WRITE_ONCE(svc_tcp_rx_budget_default_pages, pages);
	} else if (init_target) {
		WRITE_ONCE(svc_tcp_rx_budget_pages, pages);
	} else {
		/* under the lock its stage writes back under */
		spin_lock_bh(&target->lb->lock);
		WRITE_ONCE(target->lb->cfg_pages, pages);
		spin_unlock_bh(&target->lb->lock);
	}
	WRITE_ONCE(svc_rx_budget_configured_total, total);
	svc_tcp_rx_loans_update_locked();
	mutex_unlock(&svc_rx_budget_cfg_lock);
	svc_tcp_rx_budget_apply(init_target ? svc_tcp_rx_budget_initial() :
				target);
	svc_tcp_rx_budget_apply(&svc_rx_budget_read);
	return 0;
}


static int svc_tcp_rx_budget_format(struct svc_tcp_rx_budget *c, char *buf,
				    size_t size)
{
	unsigned int waiters = 0, read_waiters = 0;
	struct svc_tcp_rx_stock_sum sum;
	struct rx_loan_waiter *pos;
	u64 copies, copy_bytes;
	int len;

	if (!c)
		return scnprintf(buf, size, "none\n");
	rx_loan_copies_read(&copies, &copy_bytes);
	spin_lock_bh(&c->lb->lock);
	list_for_each_entry(pos, &c->lb->waiters, node) {
		if (pos->exact)
			read_waiters++;
		else
			waiters++;
	}
	/* Stocked credit is available; stocked records are in flight. */
	svc_tcp_rx_stock_sum(c, &sum);
	/* Q-B: READ fields appended to every budget's line */
	len = scnprintf(buf, size,
			"budget %u reserve_size %u avail %ld reserve %ld inflight %ld waiters %u admitted %llu denied %llu paused %llu overrun %llu orphaned %llu cost_factor %u hold_factor %u lent %u bounced %llu bounce_refused %llu"
			" read_inflight %ld read_waiters %u read_admitted %llu read_parked %llu read_granted %llu read_completed %llu read_unconfined %llu read_short %llu share_stalls %llu copies %llu copy_bytes %llu\n",
			c->lb->budget, c->lb->reserve_size, c->lb->avail + sum.pages,
			c->lb->reserve, (long)c->lb->inflight_count + sum.records,
			waiters, c->lb->admitted + sum.admitted, c->lb->denied,
			c->lb->paused, c->lb->overrun, c->lb->orphaned, c->lb->cost_factor,
			READ_ONCE(c->lb->hold_factor),
			c->lb->lent + (unsigned int)sum.lent,
			(u64)atomic64_read(&c->bounced),
			(u64)atomic64_read(&c->bounce_refused),
			sum.read_inflight, read_waiters, sum.read_admitted,
			c->lb->read_parked, c->lb->read_granted, sum.read_completed,
			(u64)atomic64_read(&c->read_unconfined),
			(u64)atomic64_read(&c->read_short),
			(u64)atomic64_read(&c->share_stalls), copies, copy_bytes);
	spin_unlock_bh(&c->lb->lock);
	return len;
}


/*
 * rx_loan/<netns>/: the namespace's overrides and state, in debugfs (no
 * module parameter or sysctl sizes the region or a budget any more).
 * write/{pages,reserve} override the namespace's write budget;
 * write/stats, read/stats and nfsd/stats show the budgets and the
 * server's consumer as they stand.  rx_loan/sunrpc/conn_share, one
 * connection's READ share, is one for every namespace.
 */
static struct dentry *svc_rx_debugfs_root;
/* rx_loan/sunrpc/: knobs of every namespace's, while one exists */
static struct dentry *svc_rx_debugfs_sunrpc;

static ssize_t svc_rx_debugfs_out(char __user *ubuf, size_t count,
				  loff_t *ppos, const char *buf, int len)
{
	return simple_read_from_buffer(ubuf, count, ppos, buf,
				       len < 0 ? 0 : len);
}

static ssize_t svc_rx_debugfs_cfg_read(struct file *file, char __user *ubuf,
				       size_t count, loff_t *ppos)
{
	struct svc_tcp_rx_budget *c = file->private_data;
	bool is_reserve = file_inode(file)->i_private != c;
	unsigned int pages, reserve;
	char buf[24];

	svc_tcp_rx_budget_requested(c, &pages, &reserve);
	return svc_rx_debugfs_out(ubuf, count, ppos, buf,
				  scnprintf(buf, sizeof(buf), "%u\n",
					    is_reserve ? reserve : pages));
}

static ssize_t svc_rx_debugfs_cfg_write(struct file *file,
					const char __user *ubuf, size_t count,
					loff_t *ppos)
{
	struct svc_tcp_rx_budget *c = file->private_data;
	bool is_reserve = file_inode(file)->i_private != c;
	unsigned int val;
	int ret = kstrtouint_from_user(ubuf, count, 0, &val);

	if (ret)
		return ret;
	if (!is_reserve) {
		ret = svc_tcp_rx_budget_set(c, val);
		if (ret)
			return ret;
		/* configured: no computed default any more, even at 0 */
		WRITE_ONCE(c->lb->cfg_default, false);
		svc_tcp_rx_budget_apply(c);
		return count;
	}
	if (svc_tcp_rx_budget_is_init(c)) {
		WRITE_ONCE(svc_tcp_rx_budget_reserve, val);
	} else {
		/* under the lock its stage writes back under */
		spin_lock_bh(&c->lb->lock);
		WRITE_ONCE(c->lb->cfg_reserve, val);
		spin_unlock_bh(&c->lb->lock);
	}
	svc_tcp_rx_budget_apply(c);
	return count;
}

static int svc_rx_debugfs_cfg_open(struct inode *inode, struct file *file)
{
	/* reserve's i_private is the budget's address plus one */
	file->private_data = (void *)((unsigned long)inode->i_private & ~1UL);
	return nonseekable_open(inode, file);
}

static const struct file_operations svc_rx_debugfs_cfg_fops = {
	.owner	= THIS_MODULE,
	.open	= svc_rx_debugfs_cfg_open,
	.read	= svc_rx_debugfs_cfg_read,
	.write	= svc_rx_debugfs_cfg_write,
};

static ssize_t svc_rx_debugfs_stats_read(struct file *file, char __user *ubuf,
					 size_t count, loff_t *ppos)
{
	char *buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	ssize_t ret;

	if (!buf)
		return -ENOMEM;
	ret = svc_rx_debugfs_out(ubuf, count, ppos, buf,
				 svc_tcp_rx_budget_format(file->private_data,
							  buf, PAGE_SIZE));
	kfree(buf);
	return ret;
}

static const struct file_operations svc_rx_debugfs_stats_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.read	= svc_rx_debugfs_stats_read,
};

static ssize_t svc_rx_debugfs_consumer_read(struct file *file,
					    char __user *ubuf, size_t count,
					    loff_t *ppos)
{
	struct rx_loan_consumer *rc = file->private_data;
	char buf[256];

	return svc_rx_debugfs_out(ubuf, count, ppos, buf,
		scnprintf(buf, sizeof(buf),
			  "registered %d floor %lu share %lu out_write %ld out_read %ld overrun_share %ld share_denied %ld\n",
			  READ_ONCE(rc->registered), rc->floor,
			  rx_loan_consumer_share(rc),
			  atomic_long_read(&rc->out_write),
			  atomic_long_read(&rc->out_read),
			  atomic_long_read(&rc->overrun_share),
			  atomic_long_read(&rc->share_denied)));
}

static const struct file_operations svc_rx_debugfs_consumer_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.read	= svc_rx_debugfs_consumer_read,
};

static void svc_rx_debugfs_net_init(struct svc_tcp_rx_budget *c,
				    struct net *net)
{
	struct dentry *w, *r, *nd;
	char name[16];

	if (!svc_rx_debugfs_root) {
		struct dentry *d = debugfs_lookup("rx_loan", NULL);

		/*
		 * the directory stays while it holds anything: no reference.
		 * Without debugfs both give an error, which every create
		 * below passes through and debugfs_remove() ignores.
		 */
		if (IS_ERR_OR_NULL(d))
			d = debugfs_create_dir("rx_loan", NULL);
		else
			dput(d);
		svc_rx_debugfs_root = d;
	}
	if (!svc_rx_debugfs_sunrpc) {
		/* one connection share for every namespace: one file */
		svc_rx_debugfs_sunrpc = debugfs_create_dir("sunrpc",
							   svc_rx_debugfs_root);
		debugfs_create_u32("conn_share", 0644, svc_rx_debugfs_sunrpc,
				   &svc_tcp_rx_region_conn_share);
	}
	snprintf(name, sizeof(name), "%u", net->ns.inum);
	c->debugfs = debugfs_create_dir(name, svc_rx_debugfs_root);
	w = debugfs_create_dir("write", c->debugfs);
	debugfs_create_file("pages", 0644, w, c, &svc_rx_debugfs_cfg_fops);
	debugfs_create_file("reserve", 0644, w, (void *)((unsigned long)c | 1),
			    &svc_rx_debugfs_cfg_fops);
	debugfs_create_file("stats", 0444, w, c, &svc_rx_debugfs_stats_fops);
	r = debugfs_create_dir("read", c->debugfs);
	debugfs_create_file("stats", 0444, r, c->read ?: &svc_rx_budget_read,
			    &svc_rx_debugfs_stats_fops);
	nd = debugfs_create_dir("nfsd", c->debugfs);
	debugfs_create_file("stats", 0444, nd, &c->consumer,
			    &svc_rx_debugfs_consumer_fops);
}

/*
 * The server's floor: every thread may hold up to four records of the
 * largest payload, each with its alignment page.
 */
VISIBLE_IF_KUNIT unsigned long svc_tcp_rx_consumer_floor(unsigned int nrthreads,
							 unsigned int max_payload)
{
	return (unsigned long)nrthreads * 4 *
	       (DIV_ROUND_UP(max_payload, PAGE_SIZE) + 1);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_consumer_floor);

/* @serv's threads: its pools' maxima, or the threads running if more. */
static unsigned int svc_tcp_rx_serv_nthreads(struct svc_serv *serv)
{
	unsigned int i, threads = 0, nrpools = svc_serv_nrpools(serv);

	for (i = 0; i < nrpools; i++)
		threads += READ_ONCE(serv->sv_pools[i].sp_nrthrmax);
	return max(threads, READ_ONCE(serv->sv_nrthreads));
}

/**
 * svc_tcp_rx_consumer_start - a server starts lending from its namespace
 * @net: the namespace whose region budgets it draws on
 * @serv: the server: the consumer's floor follows its threads
 *
 * nfsd's server is its namespace's one consumer of region loans, and
 * registers from nfsd_startup_net(); no other service (lockd, the NFSv4
 * callback service) does.  A new server re-binds the consumer: its floor
 * is then the new server's.  Before the threads start the floor is
 * usually 0, svc_tcp_rx_serv_threads() sets it as they do, and nothing is
 * carved here.
 *
 * Context: process context; takes mutexes.
 */
void svc_tcp_rx_consumer_start(struct net *net, struct svc_serv *serv)
{
	struct svc_tcp_rx_budget *c = svc_tcp_rx_budget_net(net);
	unsigned long floor;

	if (!c)
		return;
	floor = svc_tcp_rx_consumer_floor(svc_tcp_rx_serv_nthreads(serv),
					  serv->sv_max_payload);
	mutex_lock(&svc_rx_budget_cfg_lock);
	c->consumer_serv = serv;
	if (c->consumer.registered) {
		rx_loan_consumer_set_floor(&c->consumer, floor);
	} else {
		rx_loan_consumer_register(&c->consumer, rx_loan_net(net),
					  "nfsd", floor);
		svc_tcp_rx_loans_update_locked();
	}
	mutex_unlock(&svc_rx_budget_cfg_lock);
}
EXPORT_SYMBOL_GPL(svc_tcp_rx_consumer_start);

/**
 * svc_tcp_rx_consumer_stop - @net's server stops lending
 * @net: as started
 *
 * Called once the server's transports are destroyed (nfsd_destroy_serv()),
 * so their records' credit is back.  Credit lent on past them (a WRITE
 * bounce, a READ's transmit) returns to the consumer's counts as it lands:
 * unregistering keeps them, and the consumer lives as long as @net.
 *
 * Context: process context; takes mutexes.
 */
void svc_tcp_rx_consumer_stop(struct net *net)
{
	struct svc_tcp_rx_budget *c = svc_tcp_rx_budget_net(net);

	if (!c)
		return;
	mutex_lock(&svc_rx_budget_cfg_lock);
	c->consumer_serv = NULL;
	if (c->consumer.registered) {
		rx_loan_consumer_unregister(&c->consumer);
		svc_tcp_rx_loans_update_locked();
	}
	mutex_unlock(&svc_rx_budget_cfg_lock);
}
EXPORT_SYMBOL_GPL(svc_tcp_rx_consumer_stop);

/**
 * svc_tcp_rx_serv_threads - @serv's thread count is being set
 * @serv: the server whose pool threads svc_set_pool_threads() sets
 *
 * The floor of the consumer @serv registered follows its threads (the
 * pools' maxima, or the threads running if more), so that the region,
 * carved at the first connection or READ, is sized for them.  Once carved
 * the region does not change.  Control path: takes mutexes.
 */
void svc_tcp_rx_serv_threads(struct svc_serv *serv)
{
	unsigned int threads = svc_tcp_rx_serv_nthreads(serv);
	struct svc_tcp_rx_budget *pos;

	mutex_lock(&svc_rx_budget_cfg_lock);
	list_for_each_entry(pos, &svc_rx_budget_pools, node)
		if (pos->consumer_serv == serv && pos->consumer.registered)
			rx_loan_consumer_set_floor(&pos->consumer,
				svc_tcp_rx_consumer_floor(threads,
							  serv->sv_max_payload));
	mutex_unlock(&svc_rx_budget_cfg_lock);
}

/* @svsk's namespace's consumer, registered or not. */
static struct rx_loan_consumer *svc_tcp_rx_net_consumer(struct svc_sock *svsk)
{
	struct svc_tcp_rx_budget *c = svc_tcp_rx_budget_net(svsk->sk_xprt.xpt_net);

	return c ? &c->consumer : NULL;
}

/* The consumer @svsk's credit is counted against, or NULL before one. */
static struct rx_loan_consumer *svc_tcp_rx_consumer(struct svc_sock *svsk)
{
	struct svc_tcp_rx_budget *c = svc_tcp_rx_budget_net(svsk->sk_xprt.xpt_net);

	return c && READ_ONCE(c->consumer.registered) ? &c->consumer : NULL;
}

/*
 * A namespace's budget: sunrpc's view of @shared, the namespace's
 * rx_loan budget, or with @shared NULL its own budget with its per-CPU
 * stock (allocated whatever the budget, even 0, so a reconfiguration never
 * allocates; KUnit's).
 */
static struct svc_tcp_rx_budget *svc_tcp_rx_budget_alloc(struct net *net,
						struct rx_loan_budget *shared)
{
	struct svc_tcp_rx_budget *c = kzalloc(sizeof(*c), GFP_KERNEL);

	if (!c)
		return NULL;
	c->shared = shared;
	if (shared) {
		c->lb = shared;
	} else {
		/* on the region's list, so its frees kick this budget's waiters */
		if (rx_loan_budget_init(&c->own, false)) {
			kfree(c);
			return NULL;
		}
		c->lb = &c->own;
	}
	WRITE_ONCE(c->lb->owner, c);
	c->net = net;
	return c;
}

/*
 * Free a budget nothing uses any more.  Its transports, and so their
 * records, READ replies and parked READs, are gone; a dropped park's kick
 * may still be queued.  Stocked credit goes back before the check, and no
 * stocked holder may remain.
 */
static void svc_tcp_rx_budget_free(struct svc_tcp_rx_budget *c)
{
	flush_work(&c->lb->kick_work);
	spin_lock_bh(&c->lb->lock);
	svc_tcp_rx_stock_drain_locked(c);
	spin_unlock_bh(&c->lb->lock);
	WARN_ON_ONCE(c->lb->inflight_count || c->lb->lent || !list_empty(&c->lb->waiters) ||
		     svc_tcp_rx_read_inflight(c) ||
		     svc_tcp_rx_stocked_records(c));
	if (c->lb == &c->own)
		rx_loan_budget_destroy(&c->own);
	else	/* the namespace's: rx_loan's pernet exit tears it down */
		WRITE_ONCE(c->lb->owner, NULL);
	kfree(c);
}

int svc_tcp_rx_budget_net_init(struct net *net)
{
	struct sunrpc_net *sn = net_generic(net, sunrpc_net_id);
	struct rx_loan_net *ln = rx_loan_net(net);
	struct svc_tcp_rx_budget *c;

	/* admission and grants draw on the namespace's rx_loan budgets */
	c = svc_tcp_rx_budget_alloc(net, &ln->write);
	if (!c)
		return -ENOMEM;
	if (net_eq(net, &init_net)) {
		c->read = &svc_rx_budget_read;
	} else {
		c->read = svc_tcp_rx_budget_alloc(net, &ln->read);
		if (!c->read) {
			svc_tcp_rx_budget_free(c);
			return -ENOMEM;
		}
		svc_tcp_rx_budget_apply(c->read);
	}
	mutex_lock(&svc_rx_budget_cfg_lock);
	svc_rx_debugfs_net_init(c, net);
	list_add_tail(&c->node, &svc_rx_budget_pools);
	if (net_eq(net, &init_net))
		svc_rx_budget_init = c;
	svc_tcp_rx_budget_stage(c);
	mutex_unlock(&svc_rx_budget_cfg_lock);
	sn->rx_budget = c;
	return 0;
}

void svc_tcp_rx_budget_net_exit(struct net *net)
{
	struct sunrpc_net *sn = net_generic(net, sunrpc_net_id);
	struct svc_tcp_rx_budget *c = sn->rx_budget;

	if (!c)
		return;
	/* the namespace's transports, and so its credit, are gone */
	if (c->consumer.registered)
		rx_loan_consumer_unregister(&c->consumer);
	mutex_lock(&svc_rx_budget_cfg_lock);
	debugfs_remove(c->debugfs);
	list_del(&c->node);
	if (list_empty(&svc_rx_budget_pools)) {
		/* the last namespace: sunrpc's knobs go with it */
		debugfs_remove(svc_rx_debugfs_sunrpc);
		svc_rx_debugfs_sunrpc = NULL;
	}
	if (svc_rx_budget_init == c)
		svc_rx_budget_init = NULL;
	else
		WRITE_ONCE(svc_rx_budget_configured_total,
			   svc_rx_budget_configured_total -
			   min_t(unsigned long, READ_ONCE(c->lb->cfg_pages),
				 svc_rx_budget_configured_total));
	svc_tcp_rx_loans_update_locked();
	mutex_unlock(&svc_rx_budget_cfg_lock);
	svc_tcp_rx_budget_apply(&svc_rx_budget_read);
	sn->rx_budget = NULL;
	if (c->read != &svc_rx_budget_read)
		svc_tcp_rx_budget_free(c->read);
	svc_tcp_rx_budget_free(c);
}

#if IS_ENABLED(CONFIG_KUNIT)
static bool svc_tcp_rx_budget_kunit_nowake;

/* KUnit's budgets, on no namespace list; they read the initial's config. */
static LIST_HEAD(svc_rx_budget_kunit_list);
static DEFINE_SPINLOCK(svc_rx_budget_kunit_lock);

static void svc_tcp_rx_budget_kunit_stage(void)
{
	struct svc_tcp_rx_budget *pos;

	spin_lock(&svc_rx_budget_kunit_lock);
	list_for_each_entry(pos, &svc_rx_budget_kunit_list, node)
		svc_tcp_rx_budget_stage(pos);
	spin_unlock(&svc_rx_budget_kunit_lock);
}

/* Request every budget's configuration, to apply at its next locked op. */
static void svc_tcp_rx_budget_kunit_stage_all(void)
{
	struct svc_tcp_rx_budget *pos;

	svc_tcp_rx_budget_stage(&svc_rx_budget_default);
	svc_tcp_rx_budget_stage(&svc_rx_budget_read);
	mutex_lock(&svc_rx_budget_cfg_lock);
	list_for_each_entry(pos, &svc_rx_budget_pools, node)
		svc_tcp_rx_budget_stage(pos);
	mutex_unlock(&svc_rx_budget_cfg_lock);
	svc_tcp_rx_budget_kunit_stage();
}

static void svc_tcp_rx_cost_forget(struct svc_tcp_rx_budget *c)
{
	int cpu;

	spin_lock_bh(&c->lb->lock);
	WRITE_ONCE(c->lb->cost_factor, 0);
	WRITE_ONCE(c->lb->hold_factor, 0);
	c->lb->cost_peak = 0;
	c->lb->cost_last_peak = 0;
	c->lb->cost_records = 0;
	c->lb->cost_warm = 0;
	for_each_possible_cpu(cpu)
		atomic64_set(&per_cpu_ptr(c->lb->stock, cpu)->cost_sample, 0);
	spin_unlock_bh(&c->lb->lock);
}

static void svc_tcp_rx_budget_kunit_drain(struct svc_tcp_rx_budget *c)
{
	spin_lock_bh(&c->lb->lock);
	svc_tcp_rx_stock_drain_locked(c);
	spin_unlock_bh(&c->lb->lock);
}

/*
 * Tests change parameters between records and READs: no stock outlives
 * them.  Stocking resumes at the next kick that finds no waiter.
 */
static void svc_tcp_rx_budget_kunit_unstock(void)
{
	struct svc_tcp_rx_budget *pos;

	svc_tcp_rx_budget_kunit_drain(&svc_rx_budget_default);
	svc_tcp_rx_budget_kunit_drain(&svc_rx_budget_read);
	mutex_lock(&svc_rx_budget_cfg_lock);
	list_for_each_entry(pos, &svc_rx_budget_pools, node)
		svc_tcp_rx_budget_kunit_drain(pos);
	mutex_unlock(&svc_rx_budget_cfg_lock);
}

void svc_tcp_rx_budget_kunit_configure(unsigned int pages,
				       unsigned int reserve,
				       unsigned int factor)
{
	svc_tcp_rx_budget_kunit_unstock();
	WRITE_ONCE(svc_tcp_rx_budget_pages, pages);
	WRITE_ONCE(svc_tcp_rx_budget_reserve, reserve);
	WRITE_ONCE(svc_tcp_rx_kunit_factor, factor);
	WRITE_ONCE(svc_tcp_rx_rcvbuf, pages ? SVC_TCP_RX_RCVBUF_MIN : 0);
	WRITE_ONCE(svc_tcp_rx_rcvbuf_given, true);
	svc_tcp_rx_budget_kunit_nowake = pages != 0;
	/* Each test starts with nothing learned. */
	svc_tcp_rx_cost_forget(svc_tcp_rx_budget_initial());
	svc_tcp_rx_cost_forget(&svc_rx_budget_default);
	svc_tcp_rx_budget_kunit_stage_all();
	mutex_lock(&svc_rx_budget_cfg_lock);
	svc_tcp_rx_loans_update_locked();
	mutex_unlock(&svc_rx_budget_cfg_lock);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_budget_kunit_configure);

void svc_tcp_rx_budget_kunit_rcvbuf(unsigned int bytes)
{
	WRITE_ONCE(svc_tcp_rx_rcvbuf, bytes);
	WRITE_ONCE(svc_tcp_rx_rcvbuf_given, true);
	svc_tcp_rx_budget_kunit_stage_all();
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_budget_kunit_rcvbuf);

void svc_tcp_rx_budget_kunit_default(unsigned int pages, unsigned int reserve)
{
	svc_tcp_rx_budget_kunit_unstock();
	WRITE_ONCE(svc_tcp_rx_budget_default_pages, pages);
	WRITE_ONCE(svc_tcp_rx_budget_default_reserve, reserve);
	svc_tcp_rx_budget_kunit_stage_all();
	mutex_lock(&svc_rx_budget_cfg_lock);
	svc_tcp_rx_loans_update_locked();
	mutex_unlock(&svc_rx_budget_cfg_lock);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_budget_kunit_default);

/* Set the initial namespace's budget as its parameter does, waking freely. */
int svc_tcp_rx_budget_kunit_set(unsigned int pages)
{
	svc_tcp_rx_budget_kunit_nowake = false;
	svc_tcp_rx_budget_kunit_unstock();
	return svc_tcp_rx_budget_set(svc_tcp_rx_budget_initial(), pages);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_budget_kunit_set);

/*
 * The suites run in a live kernel, whose nfsd serves from these settings:
 * keep them as a suite found them and put them back when it ends.
 */
static struct {
	unsigned int rcvbuf;
	unsigned int budget_pages;
	unsigned int budget_reserve;
	unsigned int factor;
	unsigned int default_pages;
	unsigned int default_reserve;
	bool nowake;
	bool cfg_default;
	bool own_init, own_read;
	bool rcvbuf_given;
} svc_tcp_rx_kunit_saved;

static bool svc_tcp_rx_budget_list(struct svc_tcp_rx_budget *c, bool read);

/*
 * The suites cast a record's rx_loan budget to sunrpc's, which holds only
 * for a budget whose lb is its own (the first member).  For the suite the
 * initial namespace's budget and the read pool lend from their own storage
 * rather than from rx_loan_net()'s; they go back at restore.
 */
static void svc_tcp_rx_kunit_own(void)
{
	struct svc_tcp_rx_budget *c = svc_tcp_rx_budget_initial();

	mutex_lock(&svc_rx_budget_cfg_lock);
	if (c && c->lb != &c->own && !rx_loan_budget_init(&c->own, false)) {
		c->own.owner = c;
		WRITE_ONCE(c->lb, &c->own);
		svc_tcp_rx_kunit_saved.own_init = true;
		svc_tcp_rx_budget_stage(c);
	}
	if (svc_rx_budget_read.lb != &svc_rx_budget_read.own) {
		svc_rx_budget_read.lb = &svc_rx_budget_read.own;
		svc_tcp_rx_kunit_saved.own_read =
			svc_tcp_rx_budget_list(&svc_rx_budget_read, true);
		svc_tcp_rx_budget_stage(&svc_rx_budget_read);
	}
	mutex_unlock(&svc_rx_budget_cfg_lock);
}

static void svc_tcp_rx_kunit_shared(void)
{
	struct svc_tcp_rx_budget *c = svc_tcp_rx_budget_initial();

	mutex_lock(&svc_rx_budget_cfg_lock);
	if (c && svc_tcp_rx_kunit_saved.own_init) {
		WRITE_ONCE(c->lb, c->shared);
		svc_tcp_rx_kunit_saved.own_init = false;
		svc_tcp_rx_budget_stage(c);
		mutex_unlock(&svc_rx_budget_cfg_lock);
		flush_work(&c->own.kick_work);
		rx_loan_budget_destroy(&c->own);
		mutex_lock(&svc_rx_budget_cfg_lock);
	}
	if (svc_rx_budget_read.lb == &svc_rx_budget_read.own) {
		svc_rx_budget_read.lb = &rx_loan_net(&init_net)->read;
		svc_tcp_rx_budget_stage(&svc_rx_budget_read);
		mutex_unlock(&svc_rx_budget_cfg_lock);
		if (svc_tcp_rx_kunit_saved.own_read)
			rx_loan_budget_destroy(&svc_rx_budget_read.own);
		else
			flush_work(&svc_rx_budget_read.own.kick_work);
		svc_tcp_rx_kunit_saved.own_read = false;
		mutex_lock(&svc_rx_budget_cfg_lock);
	}
	mutex_unlock(&svc_rx_budget_cfg_lock);
}

void svc_tcp_rx_kunit_save(void)
{
	svc_tcp_rx_kunit_saved.rcvbuf = READ_ONCE(svc_tcp_rx_rcvbuf);
	svc_tcp_rx_kunit_saved.rcvbuf_given = READ_ONCE(svc_tcp_rx_rcvbuf_given);
	/* The suites' clamp is what they write: as given, even 0. */
	WRITE_ONCE(svc_tcp_rx_rcvbuf_given, true);
	svc_tcp_rx_kunit_saved.budget_pages = READ_ONCE(svc_tcp_rx_budget_pages);
	svc_tcp_rx_kunit_saved.budget_reserve = READ_ONCE(svc_tcp_rx_budget_reserve);
	svc_tcp_rx_kunit_saved.factor = READ_ONCE(svc_tcp_rx_kunit_factor);
	svc_tcp_rx_kunit_saved.default_pages =
		READ_ONCE(svc_tcp_rx_budget_default_pages);
	svc_tcp_rx_kunit_saved.default_reserve =
		READ_ONCE(svc_tcp_rx_budget_default_reserve);
	svc_tcp_rx_kunit_saved.nowake = svc_tcp_rx_budget_kunit_nowake;
	/* The suites' budgets are what they configure: no computed default. */
	svc_tcp_rx_kunit_saved.cfg_default =
		READ_ONCE(rx_loan_net(&init_net)->write.cfg_default);
	WRITE_ONCE(rx_loan_net(&init_net)->write.cfg_default, false);
	svc_tcp_rx_kunit_own();
	svc_tcp_rx_budget_apply_all();
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_kunit_save);

void svc_tcp_rx_kunit_restore(void)
{
	WRITE_ONCE(svc_tcp_rx_rcvbuf, svc_tcp_rx_kunit_saved.rcvbuf);
	WRITE_ONCE(svc_tcp_rx_rcvbuf_given, svc_tcp_rx_kunit_saved.rcvbuf_given);
	WRITE_ONCE(svc_tcp_rx_budget_pages, svc_tcp_rx_kunit_saved.budget_pages);
	WRITE_ONCE(svc_tcp_rx_budget_reserve, svc_tcp_rx_kunit_saved.budget_reserve);
	WRITE_ONCE(svc_tcp_rx_kunit_factor, svc_tcp_rx_kunit_saved.factor);
	WRITE_ONCE(svc_tcp_rx_budget_default_pages,
		   svc_tcp_rx_kunit_saved.default_pages);
	WRITE_ONCE(svc_tcp_rx_budget_default_reserve,
		   svc_tcp_rx_kunit_saved.default_reserve);
	svc_tcp_rx_budget_kunit_nowake = svc_tcp_rx_kunit_saved.nowake;
	svc_tcp_rx_kunit_shared();
	WRITE_ONCE(rx_loan_net(&init_net)->write.cfg_default,
		   svc_tcp_rx_kunit_saved.cfg_default);
	mutex_lock(&svc_rx_budget_cfg_lock);
	svc_tcp_rx_loans_update_locked();
	mutex_unlock(&svc_rx_budget_cfg_lock);
	svc_tcp_rx_kunit_fault_set(SVC_TCP_RX_KUNIT_FAULT_NONE, 0);
	svc_tcp_rx_budget_apply_all();
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_kunit_restore);

/*
 * @on: the initial namespace's budget and the read pool lend through
 * rx_loan_net(&init_net)'s write and READ budgets, as outside the suite;
 * off: from their own storage again, as svc_tcp_rx_kunit_save() left them.
 */
void svc_tcp_rx_kunit_netns_alias(bool on)
{
	svc_tcp_rx_budget_kunit_unstock();
	if (on)
		svc_tcp_rx_kunit_shared();
	else
		svc_tcp_rx_kunit_own();
	svc_tcp_rx_budget_apply_all();
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_kunit_netns_alias);

/*
 * The pages rx_loan_net(&init_net)'s write (or, @read, READ) budget has
 * free, its avail and reserve with every stock drained, and whether a
 * sunrpc budget lends through it.
 */
long svc_tcp_rx_kunit_netns_free(bool read, bool *aliased)
{
	struct rx_loan_net *ln = rx_loan_net(&init_net);
	struct rx_loan_budget *lb = read ? &ln->read : &ln->write;
	long free;

	svc_tcp_rx_budget_kunit_unstock();
	spin_lock_bh(&lb->lock);
	free = lb->avail + lb->reserve;
	spin_unlock_bh(&lb->lock);
	*aliased = READ_ONCE(lb->owner) != NULL;
	return free;
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_kunit_netns_free);

/* As if no svc_tcp_rx_rcvbuf was given: the clamp then in force. */
unsigned int svc_tcp_rx_kunit_rcvbuf_ungiven(void)
{
	WRITE_ONCE(svc_tcp_rx_rcvbuf, 0);
	WRITE_ONCE(svc_tcp_rx_rcvbuf_given, false);
	mutex_lock(&svc_rx_budget_cfg_lock);
	svc_tcp_rx_loans_update_locked();
	mutex_unlock(&svc_rx_budget_cfg_lock);
	svc_tcp_rx_budget_kunit_stage_all();
	return svc_tcp_rx_rcvbuf_effective();
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_kunit_rcvbuf_ungiven);

/*
 * Stand in for a region of @pages pages with @read_pages split off for
 * reads (0, 0: the real region, if any).  Nothing is applied: the read pool
 * picks it up lazily, or from svc_tcp_rx_budget_kunit_init().
 */
void svc_tcp_rx_budget_kunit_region(unsigned long pages,
				    unsigned long read_pages)
{
	svc_tcp_rx_budget_kunit_unstock();
	rx_loan_kunit_region(pages, read_pages);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_budget_kunit_region);

/* What svc_init_xprt_sock() does for the budgets. */
void svc_tcp_rx_budget_kunit_init(void)
{
	svc_tcp_rx_budget_kunit_unstock();
	svc_tcp_rx_budget_init();
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_budget_kunit_init);

struct svc_tcp_rx_budget *svc_tcp_rx_budget_kunit_read(void)
{
	return &svc_rx_budget_read;
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_budget_kunit_read);

int svc_tcp_rx_budget_kunit_stats(struct svc_tcp_rx_budget *c, char *buf,
				  size_t size)
{
	return svc_tcp_rx_budget_format(c, buf, size);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_budget_kunit_stats);
#endif

/*
 * What @c's budget is to apply, from the namespace's debugfs overrides
 * and the receive clamp; a read pool takes what the configured budgets use.
 */
static void svc_tcp_rx_budget_cfg(struct svc_tcp_rx_budget *c,
				  unsigned long *pages, unsigned int *reserve,
				  bool *off)
{
	unsigned int budget;

	if (c->lb->read) {
		*pages = READ_ONCE(svc_rx_budget_configured_total);
		*reserve = 0;
		*off = false;
		return;
	}
	svc_tcp_rx_budget_requested(c, &budget, reserve);
	*pages = budget;
	/* The receive clamp bounds unread receive data; READs have none. */
	*off = __svc_tcp_rx_rcvbuf_effective(false) < SVC_TCP_RX_RCVBUF_MIN;
}

/* Request @c's configuration, to apply at its next locked op. */
static void svc_tcp_rx_budget_stage(struct svc_tcp_rx_budget *c)
{
	unsigned int reserve;
	unsigned long pages;
	bool off;

	if (!c)
		return;
	/* derived under the lock: the last stage sees the last write */
	spin_lock_bh(&c->lb->lock);
	svc_tcp_rx_budget_cfg(c, &pages, &reserve, &off);
	rx_loan_budget_stage_locked(c->lb, pages, reserve, off);
	spin_unlock_bh(&c->lb->lock);
}

/*
 * The pages per 100 of record a record of @c is charged at admission: what
 * @c has learned, or 0 (a KUnit test's fixed factor goes before it).
 */
static unsigned int svc_tcp_rx_cost_factor(const struct svc_tcp_rx_budget *c)
{
	return svc_tcp_rx_fixed_factor() ?: READ_ONCE(c->lb->cost_factor);
}

static u32 svc_tcp_rx_credit_cost(const struct svc_tcp_rx_budget *c,
				  const struct svc_tcp_rx_state *state,
				  u32 pages)
{
	unsigned int factor = svc_tcp_rx_cost_factor(c);
	u64 cost;

	/* Nothing learned yet: all a record can hold. */
	if (!factor)
		return state->lt.capacity;
	cost = DIV_ROUND_UP_ULL((u64)pages * factor, 100) + 1;
	return min_t(u64, cost, state->lt.capacity);
}

/*
 * A record of @c has ended: learn from the region pages it was lent.
 * @state->region_count is an upper bound on the pages it held and the
 * bytes it received are the record, all of it unless it was cut short.
 * The sample is the factor that would have charged it what it held, less
 * the page every charge adds.
 *
 * A record charged too little stops part-way, holding what it has, so the
 * estimate has to cover the records that hold the most: frames of several
 * connections share pages, and how many of a record's do varies from one
 * record to the next.  A higher sample is taken at once.  The estimate
 * comes down only when a whole window of records, and the one before it,
 * stayed below it, so a record or a thousand whose frames happened to
 * share their pages do not lower it.  A record that was copied part-way
 * shows fewer pages than a loaned one and cannot lower it either while
 * loaned ones are still ending.
 */
static unsigned int svc_tcp_rx_cost_sample(const struct svc_tcp_rx_state *state)
{
	u32 pages = DIV_ROUND_UP((u64)state->fixed_bytes + state->lt.body_bytes,
				 PAGE_SIZE);
	unsigned int sample;

	if (!state->lt.region_count || !pages)
		return 0;
	sample = min_t(u64, DIV_ROUND_UP_ULL((u64)(state->lt.region_count - 1) *
					     100, pages),
		       SVC_TCP_RX_CREDIT_FACTOR_MAX);
	return max(sample, 100U);
}

/* A KUnit test's fixed factor needs no warm-up. */
static bool svc_tcp_rx_cost_warm(const struct svc_tcp_rx_budget *c)
{
	return svc_tcp_rx_fixed_factor() || rx_loan_cost_warm(c->lb);
}

static struct svc_tcp_rx_budget *svc_tcp_rx_budget_of(struct rx_loan_budget *lb)
{
	return lb ? READ_ONCE(lb->owner) : NULL;
}

/* Take a reference on the oldest waiting transport, to wake it unlocked. */
static bool svc_tcp_rx_waiter_pin(struct rx_loan_waiter *w)
{
	struct svc_sock *svsk = container_of(w, struct svc_sock, sk_rx_wait);

#if IS_ENABLED(CONFIG_KUNIT)
	if (svc_tcp_rx_budget_kunit_nowake)
		return false;
#endif
	svc_xprt_get(&svsk->sk_xprt);
	return true;
}

static void svc_tcp_rx_waiter_wake(struct rx_loan_waiter *w)
{
	struct svc_sock *svsk = container_of(w, struct svc_sock, sk_rx_wait);

	set_bit(XPT_DATA, &svsk->sk_xprt.xpt_flags);
	svc_xprt_enqueue(&svsk->sk_xprt);
	svc_xprt_put(&svsk->sk_xprt);
}

static const struct rx_loan_waiter_ops svc_tcp_rx_waiter_ops = {
	.pin	= svc_tcp_rx_waiter_pin,
	.wake	= svc_tcp_rx_waiter_wake,
};

/* @svsk waits on no budget yet. */
VISIBLE_IF_KUNIT void svc_tcp_rx_wait_init(struct svc_sock *svsk)
{
	INIT_LIST_HEAD(&svsk->sk_rx_wait.node);
	svsk->sk_rx_wait.ops = &svc_tcp_rx_waiter_ops;
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_wait_init);

/*
 * READ credit (NFSD READ confinement): a parked READ is an exact waiter on
 * its budget's FIFO.  KUnit's nowake suppresses only the transport wake,
 * never a READ grant.
 */
static bool svc_rx_region_park_pin(struct rx_loan_waiter *w)
{
	container_of(w, struct svc_rx_region_park, w)->granted = true;
	return true;
}

/* Replay a READ granted while parked; may free it. */
static void svc_rx_region_park_wake(struct rx_loan_waiter *w)
{
	struct svc_rx_region_park *park =
		container_of(w, struct svc_rx_region_park, w);

	svc_revisit_rx_region(container_of(park, struct svc_deferred_req,
					   rx_region));
}

static const struct rx_loan_waiter_ops svc_rx_region_park_ops = {
	.pin	= svc_rx_region_park_pin,
	.wake	= svc_rx_region_park_wake,
};

/*
 * Every site that returns or changes credit kicks instead of only waking,
 * svc_tcp_rx_bounce_done() included.
 */
static struct rx_loan_waiter *
svc_tcp_rx_credit_kick_locked(struct svc_tcp_rx_budget *c,
			      struct list_head *granted)
{
	return rx_loan_kick_locked(c->lb, NULL, granted);
}

/* Return a READ's credit (not its overrun) and pass it on. */
static void
svc_tcp_rx_budget_read_return(struct svc_tcp_rx_budget *c,
			      struct rx_loan_consumer *rc, long credit,
			      long reserve)
{
	rx_loan_grant_return(c->lb, rc, credit, reserve);
}

/*
 * Apply @c's parameters now.  A change is a wake event: with no record in
 * flight nothing else runs the pool, and data arriving on a waiting
 * transport does not queue it, so its waiters would be stranded.
 */
static void svc_tcp_rx_budget_apply(struct svc_tcp_rx_budget *c)
{
	svc_tcp_rx_budget_kunit_stage();
	if (!c)
		return;
	svc_tcp_rx_budget_stage(c);
	rx_loan_budget_reconfigure(c->lb);
}

/*
 * At TCP transport class registration: the read pool needs no rcvbuf, so
 * configure it now, not first when the clamp is written.
 */
static void svc_tcp_rx_budget_init(void)
{
	static_assert(PAGE_POOL_RX_REGION_READ_MIN * PAGE_SIZE >=
		      RPCSVC_MAXPAYLOAD + PAGE_SIZE);
	svc_tcp_rx_budget_stage(&svc_rx_budget_default);
	svc_tcp_rx_budget_apply(&svc_rx_budget_read);
}

static void svc_tcp_rx_budget_apply_all(void)
{
	struct svc_tcp_rx_budget *pos;

	svc_tcp_rx_budget_apply(&svc_rx_budget_default);
	svc_tcp_rx_budget_apply(&svc_rx_budget_read);
	mutex_lock(&svc_rx_budget_cfg_lock);
	list_for_each_entry(pos, &svc_rx_budget_pools, node)
		svc_tcp_rx_budget_apply(pos);
	mutex_unlock(&svc_rx_budget_cfg_lock);
}

VISIBLE_IF_KUNIT bool svc_tcp_rx_budget_on(struct svc_tcp_rx_budget *c)
{
	unsigned int pages, reserve;
	bool configured;

	svc_tcp_rx_budget_requested(c, &pages, &reserve);
	configured = pages != 0;
	/* The receive clamp does not gate the read pool. */
	if (configured && !c->lb->read &&
	    __svc_tcp_rx_rcvbuf_effective(false) < SVC_TCP_RX_RCVBUF_MIN) {
		pr_warn_once("svc: a region budget needs svc_tcp_rx_rcvbuf >= %u; budgets off\n",
			     SVC_TCP_RX_RCVBUF_MIN);
		configured = false;
	}
	return configured || READ_ONCE(c->lb->budget);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_budget_on);

/*
 * The budget @svsk draws on: the one it waits on, else its namespace's if
 * one is requested there, else the default one.  The budget in force is
 * updated only when its pool is next used, so it is no guide here: a pool
 * being drained keeps its waiters (sk_rx_wait.budget) and its records in flight
 * (credit_budget), and takes no new ones.
 */
static struct svc_tcp_rx_budget *svc_tcp_rx_budget_for(struct svc_sock *svsk)
{
	struct svc_tcp_rx_budget *c =
		svc_tcp_rx_budget_of(READ_ONCE(svsk->sk_rx_wait.budget));

	if (c)
		return c;
	c = svc_tcp_rx_budget_net(svsk->sk_xprt.xpt_net);
	if (c && svc_tcp_rx_budget_in_force(c))
		return c;
	return &svc_rx_budget_default;
}

/* @c, the budget a transport would draw on, is 0. */
static bool svc_tcp_rx_budget_zero(const struct svc_tcp_rx_budget *c)
{
	if (!static_branch_unlikely(&svc_tcp_rx_loans))
		return true;
	return !svc_tcp_rx_budget_in_force(c);
}

/* The budget @svsk would draw on is 0, so its records are copied. */
static bool svc_tcp_rx_budget_none(struct svc_sock *svsk)
{
	if (!static_branch_unlikely(&svc_tcp_rx_loans))
		return true;
	return svc_tcp_rx_budget_zero(svc_tcp_rx_budget_for(svsk));
}
/**
 * svc_tcp_rx_budget_read_for - the budget a READ on @svsk draws on
 * @svsk: the transport the READ arrived on
 *
 * Return: the read partition's pool when the region is split, else the
 * budget @svsk's receive draws on (its namespace's or the default), or the
 * read pool when that budget is 0.
 */
struct svc_tcp_rx_budget *svc_tcp_rx_budget_read_for(struct svc_sock *svsk)
{
	struct svc_tcp_rx_budget *c, *n;

	/* the namespace's READ budget; the initial one's is the read pool */
	n = svc_tcp_rx_budget_net(svsk->sk_xprt.xpt_net);
	n = n && n->read ? n->read : &svc_rx_budget_read;
	if (rx_loan_region_read_pages())
		return n;
	c = svc_tcp_rx_budget_for(svsk);
	/* A transport whose budget is 0 draws on the READ budget. */
	if (!svc_tcp_rx_budget_in_force(c))
		return n;
	return c;
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_budget_read_for);

VISIBLE_IF_KUNIT void svc_tcp_rx_credit_forget(struct svc_sock *svsk);

/*
 * The budget @svsk would draw on is 0: its region records are copied, so
 * they hold region pages only while being copied.  That holds with no
 * budget set anywhere too; see "Region credit admission" above.  A
 * transport that waited (its budget went to 0 while it waited) no longer
 * waits: it reads on without admission, and the next waiter is woken.
 * @c is the budget @svsk draws on, looked up once for this check and the
 * admission after it.
 */
static bool svc_tcp_rx_budget_copy_from(struct svc_sock *svsk,
					const struct svc_tcp_rx_budget *c)
{
	if (!svc_tcp_rx_budget_zero(c))
		return false;
	svc_tcp_rx_credit_forget(svsk);
	return true;
}

/*
 * Admit a record at its first skb that carries region pages, against @c,
 * the budget @svsk draws on.  False: leave it unread from there; the
 * transport is woken, oldest first, when credit returns.
 */
static bool svc_tcp_rx_credit_admit_to(struct svc_sock *svsk,
				       struct svc_tcp_rx_budget *c,
				       struct svc_tcp_rx_state *state,
				       u32 reclen)
{
	u32 pages = DIV_ROUND_UP(reclen, PAGE_SIZE);

	if (!svc_tcp_rx_budget_on(c) && list_empty(&svsk->sk_rx_wait.node))
		return true;
	if (WARN_ON_ONCE(state->cr.linked))
		return true;
	return rx_loan_admit(c->lb, svc_tcp_rx_consumer(svsk), &svsk->sk_rx_wait, &state->cr,
			     svc_tcp_rx_credit_cost(c, state, pages),
			     svc_tcp_rx_cost_warm(c));
}

#if IS_ENABLED(CONFIG_KUNIT)
/* The copy check and the admission, each looking up the budget itself. */
bool svc_tcp_rx_budget_copy(struct svc_sock *svsk)
{
	return svc_tcp_rx_budget_copy_from(svsk, svc_tcp_rx_budget_for(svsk));
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_budget_copy);

bool svc_tcp_rx_credit_admit(struct svc_sock *svsk,
			     struct svc_tcp_rx_state *state, u32 reclen)
{
	return svc_tcp_rx_credit_admit_to(svsk, svc_tcp_rx_budget_for(svsk),
					  state, reclen);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_credit_admit);
#endif

/*
 * Before an admitted record consumes an skb: make sure its credit covers the
 * region pages @need more could hold.  False: pause the record at this skb
 * boundary; the transport is woken when credit returns.
 */
VISIBLE_IF_KUNIT bool svc_tcp_rx_credit_charge(struct svc_sock *svsk,
					       struct svc_tcp_rx_state *state,
					       u32 need)
{
	return rx_loan_charge(&svsk->sk_rx_wait, &state->cr,
			      state->lt.region_count + need);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_credit_charge);

/*
 * Return a record's credit, keeping @lend units of it lent to a direct-write
 * bounce (never new credit): its ordinary credit first, then its reserve;
 * credit past the budget (credit_over) is not converted.  The conversion
 * shares the section that returns the rest, so a bounce costs the budget's
 * lock no more than before.  The caller checked that @lend is covered.
 */
static void svc_tcp_rx_credit_return(struct svc_tcp_rx_state *state, u32 lend)
{
	struct rx_loan_lend out = {};

	if (!state || !state->cr.linked)
		return;
	rx_loan_return(&state->cr, svc_tcp_rx_cost_sample(state), lend, &out);
	if (out.budget) {
		state->bounce_budget = svc_tcp_rx_budget_of(out.budget);
		state->bounce_consumer = out.consumer;
		state->bounce_credit = out.credit;
		state->bounce_reserve = out.reserve;
		state->bounce_stocked = out.stocked;
	}
}

/* Return a record's credit; exactly once, from every terminal path. */
VISIBLE_IF_KUNIT void svc_tcp_rx_credit_release(struct svc_tcp_rx_state *state)
{
	svc_tcp_rx_credit_return(state, 0);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_credit_release);

/*
 * A record whose loaned pages could not be accounted for is abandoned, @held
 * of its pages possibly still out: keep that much of its credit spent, but
 * take it out of the in-flight order so the next record can be the oldest
 * and draw the reserve (rx_loan_orphan()).
 */
static void svc_tcp_rx_credit_orphan(struct svc_tcp_rx_state *state, u32 held)
{
	if (state)
		rx_loan_orphan(&state->cr, held);
}

/* A transport going away stops waiting. */
VISIBLE_IF_KUNIT void svc_tcp_rx_credit_forget(struct svc_sock *svsk)
{
	rx_loan_unwait(&svsk->sk_rx_wait);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_credit_forget);

#if IS_ENABLED(CONFIG_KUNIT)
/* Credit in @c's stocks. */
static long svc_tcp_rx_stocked(struct svc_tcp_rx_budget *c)
{
	long n = 0;
	int cpu;

	for_each_possible_cpu(cpu)
		n += atomic_long_read(&per_cpu_ptr(c->lb->stock, cpu)->pages);
	return n;
}

/* The stock primitives on @c, on the running CPU (the caller pins it). */
long svc_tcp_rx_budget_kunit_stock(struct svc_tcp_rx_budget *c,
				   enum svc_tcp_rx_kunit_stock_op op, long n)
{
	struct rx_loan_waiter *wake;
	LIST_HEAD(granted);
	long ret = 0;

	switch (op) {
	case SVC_TCP_RX_KUNIT_STOCK_APPLY:	/* configure and kick, as a return */
		spin_lock_bh(&c->lb->lock);
		rx_loan_budget_update_locked(c->lb);
		wake = svc_tcp_rx_credit_kick_locked(c, &granted);
		spin_unlock_bh(&c->lb->lock);
		rx_loan_wake(wake);
		rx_loan_wake_granted(&granted);
		break;
	case SVC_TCP_RX_KUNIT_STOCK_FILL:	/* what an admission refills */
		spin_lock_bh(&c->lb->lock);
		ret = rx_loan_stock_refill_locked(c->lb);
		spin_unlock_bh(&c->lb->lock);
		break;
	case SVC_TCP_RX_KUNIT_STOCK_TAKE:
		ret = rx_loan_stock_take(c->lb, n);
		break;
	case SVC_TCP_RX_KUNIT_STOCK_PUT:	/* a spill goes to avail */
		ret = svc_tcp_rx_stock_put(c, n);
		spin_lock_bh(&c->lb->lock);
		c->lb->avail += ret;
		spin_unlock_bh(&c->lb->lock);
		break;
	case SVC_TCP_RX_KUNIT_STOCK_DRAIN:	/* what the first waiter does */
		svc_tcp_rx_budget_kunit_drain(c);
		break;
	case SVC_TCP_RX_KUNIT_STOCK_STOCKED:
		ret = svc_tcp_rx_stocked(c);
		break;
	case SVC_TCP_RX_KUNIT_STOCK_CONTENDED:
		ret = READ_ONCE(c->lb->contended);
		break;
	}
	return ret;
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_budget_kunit_stock);

/*
 * A budget as a namespace's is made (net_init), configured as the initial
 * namespace's but on no list; svc_tcp_rx_budget_kunit_free() tears it down
 * as net_exit does.
 */
struct svc_tcp_rx_budget *svc_tcp_rx_budget_kunit_alloc(void)
{
	struct svc_tcp_rx_budget *c = svc_tcp_rx_budget_alloc(&init_net, NULL);

	if (!c)
		return NULL;
	spin_lock(&svc_rx_budget_kunit_lock);
	list_add_tail(&c->node, &svc_rx_budget_kunit_list);
	svc_tcp_rx_budget_stage(c);
	spin_unlock(&svc_rx_budget_kunit_lock);
	return c;
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_budget_kunit_alloc);


void svc_tcp_rx_budget_kunit_free(struct svc_tcp_rx_budget *c)
{
	spin_lock(&svc_rx_budget_kunit_lock);
	list_del(&c->node);
	spin_unlock(&svc_rx_budget_kunit_lock);
	svc_tcp_rx_budget_free(c);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_budget_kunit_free);

/* Stocked credit is available. */
long svc_tcp_rx_budget_kunit_avail(void)
{
	struct svc_tcp_rx_budget *c = svc_tcp_rx_budget_initial();

	return READ_ONCE(c->lb->avail) + svc_tcp_rx_stocked(c);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_budget_kunit_avail);

long svc_tcp_rx_budget_kunit_default_avail(void)
{
	return READ_ONCE(svc_rx_budget_default.lb->avail) +
	       svc_tcp_rx_stocked(&svc_rx_budget_default);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_budget_kunit_default_avail);

long svc_tcp_rx_budget_avail(void)
{
	struct svc_tcp_rx_budget *c = svc_tcp_rx_budget_initial();
	long n = READ_ONCE(c->lb->avail);
	int cpu;

	for_each_possible_cpu(cpu)
		n += atomic_long_read(&per_cpu_ptr(c->lb->stock, cpu)->pages);
	return n;
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_budget_avail);

/*
 * The estimate with every ended record learned: the samples of records
 * released to a stock are folded in first, as the next refill or drain
 * would fold them.
 */
unsigned int svc_tcp_rx_budget_cost_factor(void)
{
	struct svc_tcp_rx_budget *c = svc_tcp_rx_budget_initial();
	int cpu;

	spin_lock_bh(&c->lb->lock);
	for_each_possible_cpu(cpu)
		rx_loan_cost_fold_cpu_locked(c->lb, per_cpu_ptr(c->lb->stock, cpu));
	spin_unlock_bh(&c->lb->lock);
	return READ_ONCE(c->lb->cost_factor);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_budget_cost_factor);

long svc_tcp_rx_budget_reserve_avail(void)
{
	return READ_ONCE(svc_tcp_rx_budget_initial()->lb->reserve);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_budget_reserve_avail);

long svc_tcp_rx_budget_default_avail(void)
{
	return READ_ONCE(svc_rx_budget_default.lb->avail);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_budget_default_avail);

/*
 * The initial namespace's server consumer, as a server of @nrthreads
 * starting would register (or re-bind) it.
 */
struct rx_loan_consumer *svc_tcp_rx_kunit_consumer(unsigned int nrthreads,
						   unsigned int max_payload)
{
	/* unpooled: svc_serv_nrpools() is 1, so it needs its one pool */
	struct svc_pool pool = { .sp_nrthrmax = nrthreads };
	struct svc_serv serv = {
		.sv_nrthreads	= nrthreads,
		.sv_max_payload	= max_payload,
		.sv_pools	= &pool,
	};
	struct svc_tcp_rx_budget *c = svc_tcp_rx_budget_initial();

	if (!c)
		return NULL;
	svc_tcp_rx_consumer_start(&init_net, &serv);
	mutex_lock(&svc_rx_budget_cfg_lock);
	c->consumer_serv = NULL;	/* @serv is this frame's */
	mutex_unlock(&svc_rx_budget_cfg_lock);
	return &c->consumer;
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_kunit_consumer);

/* The initial namespace's server stops, as nfsd_destroy_serv() stops it. */
void svc_tcp_rx_kunit_consumer_stop(void)
{
	svc_tcp_rx_consumer_stop(&init_net);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_kunit_consumer_stop);

/* An exact grant of @pages from @c to @rc, as a READ's. */
int svc_tcp_rx_budget_kunit_grant(struct svc_tcp_rx_budget *c,
				  struct rx_loan_consumer *rc,
				  struct rx_loan_waiter *w, long pages)
{
	memset(w, 0, sizeof(*w));
	INIT_LIST_HEAD(&w->node);
	w->need = pages;
	return rx_loan_grant(c->lb, rc, w);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_budget_kunit_grant);

void svc_tcp_rx_budget_kunit_grant_return(struct svc_tcp_rx_budget *c,
					  struct rx_loan_waiter *w)
{
	rx_loan_grant_return(c->lb, w->consumer, w->credit, w->reserve);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_budget_kunit_grant_return);

/* The initial namespace's server sets @nrthreads, as svc_set_pool_threads(). */
void svc_tcp_rx_kunit_threads(unsigned int nrthreads, unsigned int max_payload)
{
	/* unpooled: svc_serv_nrpools() is 1, so it needs its one pool */
	struct svc_pool pool = { .sp_nrthrmax = nrthreads };
	struct svc_serv serv = {
		.sv_nrthreads	= nrthreads,
		.sv_max_payload	= max_payload,
		.sv_pools	= &pool,
	};
	struct svc_tcp_rx_budget *c = svc_tcp_rx_budget_initial();
	struct svc_serv *was;

	if (!c)
		return;
	mutex_lock(&svc_rx_budget_cfg_lock);
	was = c->consumer_serv;
	c->consumer_serv = &serv;
	mutex_unlock(&svc_rx_budget_cfg_lock);
	svc_tcp_rx_serv_threads(&serv);
	mutex_lock(&svc_rx_budget_cfg_lock);
	c->consumer_serv = was;
	mutex_unlock(&svc_rx_budget_cfg_lock);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_kunit_threads);

/*
 * Turn the initial namespace's computed default @on or off: the pages its
 * write budget then lends, and the budget it shares with rx_loan_net().
 */
unsigned long svc_tcp_rx_kunit_computed_default(bool on,
						struct rx_loan_budget **lb)
{
	struct svc_tcp_rx_budget *c = svc_tcp_rx_budget_initial();

	if (!c)
		return 0;
	WRITE_ONCE(c->lb->cfg_default, on);
	*lb = c->shared;
	return svc_tcp_rx_budget_in_force(c);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_kunit_computed_default);
#endif

/*
 * Upper bound on the region pages bytes [first, last) of @skb can add:
 * only pages from a pool bound to the fixed receive region count, and a
 * page counts once however many consecutive fragments lie in it.
 */
VISIBLE_IF_KUNIT u32 svc_tcp_rx_skb_region_pages(const struct sk_buff *skb,
						 u32 base, u32 first, u32 last)
{
	struct page *prev = NULL;

	return rx_loan_skb_region_pages(skb, base, first, last, &prev);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_skb_region_pages);

static bool svc_tcp_rx_is_empty(const struct svc_tcp_rx_state *state)
{
	return state && state->mode == SVC_TCP_RX_EMPTY && !state->lt.count &&
		!state->lt.body_bytes && !state->lt.borrowed_count &&
		state->lt.refs_acquired == state->lt.refs_released &&
		!state->terminal;
}

/*
 * @capacity is the page count of the copy arena (rq_pages) the state
 * may fall back to. A loan takes one bvec per received fragment, and
 * fragments do not grow with PAGE_SIZE, so the bvec table is sized by
 * svc_pages_to_bvecs() rather than by the page count: on a 64 KiB
 * kernel a table of one entry per page fills after a few segments of
 * a large WRITE and forces the whole record to be copied.
 *
 * svc_tcp_rx_state_alloc_entries() takes the table size itself:
 * svc_serv_maxbvecs() for a server, which fits the table to a maximum
 * message in 9000-byte MTU frames.
 */
struct svc_tcp_rx_state *svc_tcp_rx_state_alloc(unsigned long capacity,
						gfp_t gfp, int node)
{
	if (!capacity || capacity > U32_MAX)
		return NULL;
	return svc_tcp_rx_state_alloc_entries(svc_pages_to_bvecs(capacity),
					      capacity, gfp, node);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_state_alloc);

struct svc_tcp_rx_state *svc_tcp_rx_state_alloc_entries(unsigned long entries,
							unsigned long capacity,
							gfp_t gfp, int node)
{
	struct svc_tcp_rx_state *state;

	if (!capacity || capacity > U32_MAX || !entries || entries > U32_MAX)
		return NULL;
	state = kzalloc_node(sizeof(*state), gfp, node);
	if (!state)
		return NULL;
	if (rx_loan_table_alloc(&state->lt, entries, gfp, node)) {
		kfree(state);
		return NULL;
	}
	state->arena_pages = capacity;
	return state;
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_state_alloc_entries);

VISIBLE_IF_KUNIT void svc_tcp_rx_reset(struct svc_tcp_rx_state *state)
{
	unsigned int arena_pages = state->arena_pages;

	if (WARN_ON_ONCE(!state->terminal || state->lt.borrowed_count ||
			 state->lt.refs_acquired != state->lt.refs_released ||
			 state->bounce_pages))
		return;
	svc_tcp_rx_credit_release(state);
	rx_loan_table_reset(&state->lt);
	memset_after(state, 0, lt);
	state->arena_pages = arena_pages;
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_reset);

static int svc_tcp_rx_append(struct svc_tcp_rx_state *state,
			     struct page *page, unsigned int offset,
			     unsigned int len, bool borrowed, bool page_pool)
{
	if (WARN_ON_ONCE(state->mode != SVC_TCP_RX_MIXED || state->terminal) ||
	    !rx_loan_table_append_ok(&state->lt, page, offset, len, borrowed,
				     page_pool))
		return -EINVAL;
#if IS_ENABLED(CONFIG_KUNIT)
	if (borrowed &&
	    svc_tcp_rx_kunit_should_fail(SVC_TCP_RX_KUNIT_FAULT_APPEND, state))
		return -EIO;
#endif
	return __rx_loan_table_append(&state->lt, page, offset, len, borrowed,
				      page_pool);
}

VISIBLE_IF_KUNIT int
svc_tcp_rx_append_borrowed(struct svc_tcp_rx_state *state, struct page *page,
			   unsigned int offset, unsigned int len,
			   bool page_pool)
{
	return svc_tcp_rx_append(state, page, offset, len, true, page_pool);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_append_borrowed);

VISIBLE_IF_KUNIT int
svc_tcp_rx_append_copied(struct svc_tcp_rx_state *state, struct page *page,
			 unsigned int offset, unsigned int len)
{
	return svc_tcp_rx_append(state, page, offset, len, false, false);
}

static int svc_tcp_rx_copy_to_arena(struct page **pages, unsigned int capacity,
				    u32 body_offset,
				    const struct bio_vec *source)
{
	unsigned int source_offset = 0;
	unsigned int remaining = source->bv_len;

	while (remaining) {
		unsigned int page_index = body_offset >> PAGE_SHIFT;
		unsigned int destination_offset = offset_in_page(body_offset);
		unsigned int len = min_t(unsigned int, remaining,
					 PAGE_SIZE - destination_offset);
		void *source_addr, *destination_addr;

		if (page_index >= capacity || !pages[page_index])
			return -EFAULT;
		source_addr = kmap_local_page(source->bv_page);
		destination_addr = kmap_local_page(pages[page_index]);
		memcpy(destination_addr + destination_offset,
		       source_addr + source->bv_offset + source_offset, len);
		kunmap_local(destination_addr);
		kunmap_local(source_addr);
		body_offset += len;
		source_offset += len;
		remaining -= len;
	}
	return 0;
}

VISIBLE_IF_KUNIT int svc_tcp_rx_materialize(struct svc_tcp_rx_state *state,
					    struct page **pages)
{
	u32 body_offset = state->fixed_bytes;
	unsigned int index;

	if (WARN_ON_ONCE(state->mode != SVC_TCP_RX_MIXED || state->terminal))
		return -EINVAL;
	for (index = 0; index < state->lt.count; index++) {
		struct bio_vec *bvec = &state->lt.bvec[index];
		int ret;
#if IS_ENABLED(CONFIG_KUNIT)
		enum svc_tcp_rx_kunit_fault fault =
			SVC_TCP_RX_KUNIT_FAULT_MATERIALIZE;
#endif

		if (WARN_ON_ONCE(!bvec->bv_page || !bvec->bv_len ||
				 bvec->bv_offset > PAGE_SIZE ||
				 bvec->bv_len > PAGE_SIZE - bvec->bv_offset))
			return -EINVAL;
		if (test_bit(index, state->lt.borrowed)) {
#if IS_ENABLED(CONFIG_KUNIT)
			if (svc_tcp_rx_kunit_should_fail(fault, state))
				return -EFAULT;
#endif
			ret = svc_tcp_rx_copy_to_arena(pages, state->arena_pages,
						       body_offset, bvec);
			if (ret)
				return ret;
			rx_loan_put_borrowed(bvec->bv_page,
					     test_bit(index,
						      state->lt.page_pool));
			__clear_bit(index, state->lt.borrowed);
			state->lt.region_last = NULL;
			state->lt.refs_released++;
			state->lt.borrowed_count--;
			state->lt.borrowed_bytes -= bvec->bv_len;
			state->lt.copied_bytes += bvec->bv_len;
			state->materialized_bytes += bvec->bv_len;
			if (test_bit(index, state->lt.page_pool)) {
				__clear_bit(index, state->lt.page_pool);
				state->lt.page_pool_bytes -= bvec->bv_len;
			}
		}
		body_offset += bvec->bv_len;
	}
	if (WARN_ON_ONCE(body_offset != state->lt.body_bytes ||
			 state->lt.borrowed_count || state->lt.borrowed_bytes ||
			 state->lt.page_pool_bytes ||
			 state->lt.refs_acquired != state->lt.refs_released ||
			 !bitmap_empty(state->lt.borrowed, state->lt.count) ||
			 !bitmap_empty(state->lt.page_pool, state->lt.count)))
		return -EINVAL;
	state->lt.count = 0;
	state->mode = SVC_TCP_RX_COPY;
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_materialize);

/* The record's counts agree; checks no entry. */
static bool svc_tcp_rx_counts_valid(const struct svc_tcp_rx_state *state)
{
	return state && state->mode != SVC_TCP_RX_EMPTY && !state->terminal &&
	       rx_loan_table_counts_valid(&state->lt);
}

static bool svc_tcp_rx_refs_valid(const struct svc_tcp_rx_state *state)
{
	return svc_tcp_rx_counts_valid(state) &&
	       rx_loan_table_refs_valid(&state->lt);
}

VISIBLE_IF_KUNIT int svc_tcp_rx_release_refs(struct svc_tcp_rx_state *state)
{
	int ret;

	/* None of its pages went back: all of them may still be out. */
	if (WARN_ON_ONCE(!svc_tcp_rx_refs_valid(state))) {
		svc_tcp_rx_credit_orphan(state, U32_MAX);
		return -EINVAL;
	}
	ret = rx_loan_table_release(&state->lt);
	state->terminal = true;
	if (ret) {
		u32 acquired = state->lt.refs_acquired;
		u32 released = state->lt.refs_released;

		/*
		 * Every borrowed entry went back to its pool: only references
		 * taken and never released can still hold a page.
		 */
		svc_tcp_rx_credit_orphan(state, acquired > released ?
					 acquired - released : 0);
		return ret;
	}
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_release_refs);

VISIBLE_IF_KUNIT int
svc_tcp_rx_exchange(struct svc_tcp_rx_state **active,
		    struct svc_tcp_rx_state **empty)
{
	struct svc_tcp_rx_state *state = *active;

	/*
	 * A record is saved and restored on each pass that reads part of
	 * it, so only its counts are checked here. Each borrowed entry is
	 * checked once, by svc_tcp_rx_release_refs(), before its page goes
	 * back.
	 */
	if (WARN_ON(!state || !*empty ||
		    state->lt.capacity != (*empty)->lt.capacity ||
		    state->arena_pages != (*empty)->arena_pages ||
		    svc_tcp_rx_is_empty(state) ||
		    state->mode == SVC_TCP_RX_PUBLISHED ||
		    !svc_tcp_rx_counts_valid(state) ||
		    !svc_tcp_rx_is_empty(*empty)))
		return -EINVAL;
	swap(*active, *empty);
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_exchange);

void svc_tcp_rx_state_free(struct svc_tcp_rx_state *state)
{
	if (!state)
		return;
	if (WARN_ON_ONCE(!svc_tcp_rx_is_empty(state))) {
		if (svc_tcp_rx_release_refs(state))
			return;
		svc_tcp_rx_reset(state);
	}
	rx_loan_table_free(&state->lt);
	kfree(state);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_state_free);

static void svc_tcp_rx_clear_xdr(struct svc_rqst *rqstp)
{
	rqstp->rq_arg.page_mode = XDRBUF_PAGE_ARRAY;
	rqstp->rq_arg.bvec = NULL;
	rqstp->rq_arg.bvec_count = 0;
	rqstp->rq_arg.bvec_offset = 0;
	rqstp->rq_arg.page_base = 0;
	rqstp->rq_arg.page_len = 0;
}

VISIBLE_IF_KUNIT int
svc_tcp_rx_abort(struct svc_sock *svsk, struct svc_rqst *rqstp,
		 struct svc_tcp_rx_state *state, enum svc_tcp_rx_action action,
		 enum svc_tcp_rx_reason reason)
{
	if (WARN_ON_ONCE(!state || svc_tcp_rx_is_empty(state)))
		return -EINVAL;
	if (state->reason == SVC_TCP_RX_REASON_NONE)
		state->reason = reason;
	if (svc_tcp_rx_release_refs(state)) {
		state->reason = SVC_TCP_RX_REASON_INVARIANT;
		trace_svcsock_tcp_rx_lifetime(&svsk->sk_xprt, rqstp, state, SVC_TCP_RX_ERROR);
		return -EINVAL;
	}
	trace_svcsock_tcp_rx_lifetime(&svsk->sk_xprt, rqstp, state, action);
	if (rqstp)
		svc_tcp_rx_clear_xdr(rqstp);
	svc_tcp_rx_reset(state);
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_abort);

static int svc_tcp_rx_abort_active(struct svc_sock *svsk,
				   struct svc_rqst *rqstp,
				   enum svc_tcp_rx_action action,
				   enum svc_tcp_rx_reason reason,
				   bool allow_no_record)
{
	bool request_active = rqstp && rqstp->rq_tcp_rx &&
		!svc_tcp_rx_is_empty(rqstp->rq_tcp_rx);
	bool socket_active = svsk->sk_rx &&
		!svc_tcp_rx_is_empty(svsk->sk_rx);

	if (!request_active && !socket_active) {
		if (allow_no_record)
			return 0;
		WARN_ON_ONCE(1);
		return -EINVAL;
	}
	if (WARN_ON_ONCE(request_active == socket_active))
		return -EINVAL;
	if (request_active)
		return svc_tcp_rx_abort(svsk, rqstp, rqstp->rq_tcp_rx,
					action, reason);
	return svc_tcp_rx_abort(svsk, NULL, svsk->sk_rx, action, reason);
}

/**
 * svc_tcp_release_ctxt - Release transport-related resources
 * @xprt: the transport which owned the context
 * @ctxt: the context from rqstp->rq_xprt_ctxt or dr->xprt_ctxt
 *
 */
VISIBLE_IF_KUNIT void svc_tcp_release_ctxt(struct svc_xprt *xprt, void *ctxt)
{
	struct svc_tcp_rx_state *state = ctxt;
	struct svc_sock *svsk;
	struct svc_rqst *rqstp;

	if (!state)
		return;
	svsk = container_of(xprt, struct svc_sock, sk_xprt);
	rqstp = state->rqstp;
	if (WARN_ON_ONCE(state->mode != SVC_TCP_RX_PUBLISHED || !rqstp ||
			 rqstp->rq_tcp_rx != state ||
			 rqstp->rq_xprt_ctxt != state)) {
		if (!svc_tcp_rx_is_empty(state))
			svc_tcp_rx_abort(svsk, rqstp, state, SVC_TCP_RX_ERROR,
					 SVC_TCP_RX_REASON_INVARIANT);
		return;
	}
	/* A payload released early (terminal) holds no loaned pages. */
	if (!state->terminal && svc_tcp_rx_release_refs(state)) {
		state->reason = SVC_TCP_RX_REASON_INVARIANT;
		trace_svcsock_tcp_rx_lifetime(xprt, rqstp, state, SVC_TCP_RX_ERROR);
		return;
	}
	trace_svcsock_tcp_rx_lifetime(xprt, rqstp, state, SVC_TCP_RX_RELEASE);
	svc_tcp_rx_clear_xdr(rqstp);
	svc_tcp_rx_reset(state);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_release_ctxt);

/**
 * svc_tcp_rx_release_payload - Return a request's loaned payload early
 * @svsk: the transport that loaned it
 * @rqstp: the request whose payload the caller has consumed
 *
 * Drops the borrowed page references and returns the record's region
 * credit, exactly once, then detaches the argument page vector so nothing
 * reads it. svc_tcp_release_ctxt() later finds no payload to release.
 */
VISIBLE_IF_KUNIT void svc_tcp_rx_release_payload(struct svc_sock *svsk,
						 struct svc_rqst *rqstp)
{
	struct svc_tcp_rx_state *state = rqstp->rq_tcp_rx;

	if (!state || state->mode != SVC_TCP_RX_PUBLISHED || state->terminal ||
	    rqstp->rq_xprt_ctxt != state || rqstp->rq_deferred)
		return;
	if (svc_tcp_rx_release_refs(state)) {
		state->reason = SVC_TCP_RX_REASON_INVARIANT;
		trace_svcsock_tcp_rx_lifetime(&svsk->sk_xprt, rqstp, state,
					      SVC_TCP_RX_ERROR);
		return;
	}
	svc_tcp_rx_credit_release(state);
	trace_svcsock_tcp_rx_lifetime(&svsk->sk_xprt, rqstp, state,
				      SVC_TCP_RX_PAYLOAD_RELEASE);
	svc_tcp_rx_clear_xdr(rqstp);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_release_payload);

static void svc_tcp_release_payload(struct svc_rqst *rqstp)
{
	svc_tcp_rx_release_payload(container_of(rqstp->rq_xprt,
						struct svc_sock, sk_xprt),
				   rqstp);
}

/* Copy @len bytes at @src to byte @dest_offset of @dest (or @region). */
static void svc_tcp_rx_bounce_copy(struct page **dest,
				   const struct bio_vec *region,
				   u32 dest_offset, const struct bio_vec *src,
				   u32 src_offset, u32 len)
{
	while (len) {
		u32 index = dest_offset >> PAGE_SHIFT;
		u32 in_page = offset_in_page(dest_offset);
		u32 n = min_t(u32, len, PAGE_SIZE - in_page);
		struct page *page = region ? region[index].bv_page : dest[index];

		memcpy_page(page, in_page, src->bv_page,
			    src->bv_offset + src_offset, n);
		dest_offset += n;
		src_offset += n;
		len -= n;
	}
}

/*
 * Take @n pages of the region's write partition into @out[].bv_page, all or
 * nothing, without a second array: page_pool_rx_region_take_pages() packs
 * the page pointers at the start of @out, and they are spread out from the
 * last, so no pointer is overwritten before it is read.
 */
static bool svc_tcp_rx_bounce_take(struct bio_vec *out, unsigned int n)
{
	struct page **pages = (struct page **)out;
	unsigned int i;

	BUILD_BUG_ON(sizeof(*pages) > sizeof(*out));
	if (page_pool_rx_region_take_pages(false, pages, n) != n)
		return false;
	for (i = n; i-- > 0;)
		out[i].bv_page = pages[i];
	return true;
}

/* Give the bounce's pages back; packs them at the start of @bvec. */
static void svc_tcp_rx_bounce_give(struct bio_vec *bvec, unsigned int n)
{
	struct page **pages = (struct page **)bvec;
	unsigned int i;

	for (i = 0; i < n; i++)
		pages[i] = bvec[i].bv_page;
	page_pool_rx_region_give_pages(pages, n);
}

static int svc_tcp_rx_bounce_refuse(struct svc_sock *svsk,
				    struct svc_tcp_rx_state *state,
				    struct svc_tcp_rx_budget *c,
				    const char *why)
{
	trace_svcsock_tcp_rx_bounce_refused(&svsk->sk_xprt, state, why);
	atomic64_inc(&c->bounce_refused);
	return -ENOSPC;
}

/**
 * svc_tcp_rx_bounce - lay a loaned WRITE payload out page-tiled
 * @svsk: the transport that loaned it
 * @rqstp: the request holding the loan
 * @payload: the WRITE payload, a view into the loan
 * @align: in-page offset for the first byte (the file position's)
 * @mem_align: the memory alignment, against the file position, that the
 *	       copy's start and every page joint must meet
 * @out: receives the page-tiled payload
 * @out_size: entries in @out
 *
 * Whole pages joined only at page boundaries, as direct I/O needs.  A record
 * holding region credit is copied into pages from the region's write
 * partition, starting at in-page offset @align, so the direct middle starts
 * memory-aligned, and @span units of its credit are converted to cover them;
 * a record short of credit or a partition short of pages refuses, and the
 * payload never leaves the region.  Any other loan has its holes filled in
 * the request's own arena at their record-body offsets, as a capacity
 * fallback does, but only when that layout meets @mem_align for this file
 * position.  Nothing waits.  A refusal changes nothing.  Each loaned page
 * whose bytes the copy consumed is returned as it passes, then the rest of
 * the loan as svc_tcp_rx_release_payload() does.
 *
 * Return: entries in @out, or a negative errno (nothing changed).
 */
VISIBLE_IF_KUNIT int svc_tcp_rx_bounce(struct svc_sock *svsk,
				       struct svc_rqst *rqstp,
				       const struct xdr_buf *payload,
				       unsigned int align, unsigned int mem_align,
				       struct bio_vec *out, unsigned int out_size)
{
	struct svc_tcp_rx_state *state = rqstp->rq_tcp_rx;
	u32 index, first, start, end, span, b, n;
	struct svc_tcp_rx_budget *c;
	struct bio_vec *region = NULL;

	if (!state || state->mode != SVC_TCP_RX_PUBLISHED || state->terminal ||
	    rqstp->rq_xprt_ctxt != state || rqstp->rq_deferred ||
	    state->bounce_pages ||
	    test_bit(RQ_PAYLOAD_SHARED, &rqstp->rq_flags) ||
	    payload->page_mode != XDRBUF_PAGE_BVECS ||
	    payload->head[0].iov_len || payload->tail[0].iov_len ||
	    !payload->page_len || payload->bvec < state->lt.bvec ||
	    payload->bvec >= state->lt.bvec + state->lt.count ||
	    align >= PAGE_SIZE)
		return -EOPNOTSUPP;
	/* The copy returns loaned pages as it goes: the loan must be sound. */
	if (!svc_tcp_rx_refs_valid(state))
		return -EINVAL;
	first = payload->bvec - state->lt.bvec;
	start = state->fixed_bytes + payload->bvec_offset;
	for (index = 0; index < first; index++)
		start += state->lt.bvec[index].bv_len;
	if (check_add_overflow(start, payload->page_len, &end) ||
	    end > state->lt.body_bytes)
		return -EINVAL;

	if (state->cr.linked) {
		c = svc_tcp_rx_budget_of(state->cr.budget);
		span = DIV_ROUND_UP(align + payload->page_len, PAGE_SIZE);
		if (span > out_size)
			return svc_tcp_rx_bounce_refuse(svsk, state, c,
							"capacity");
		/*
		 * Only the owning thread changes the record's credit once it
		 * is published, so it can be checked unlocked; it converts
		 * below, with the rest of the record's credit returned.
		 */
		if (state->cr.credit - state->cr.over < span)
			return svc_tcp_rx_bounce_refuse(svsk, state, c,
							"credit");
		if (!svc_tcp_rx_bounce_take(out, span))
			return svc_tcp_rx_bounce_refuse(svsk, state, c,
							"region");
		region = out;
	} else {
		c = svc_tcp_rx_budget_for(svsk);
		span = ((end - 1) >> PAGE_SHIFT) - (start >> PAGE_SHIFT) + 1;
		/* The arena layout must fit @out and meet the device's alignment. */
		if (span > out_size || ((start ^ align) & (max(mem_align, 1U) - 1)) ||
		    (end - 1) >> PAGE_SHIFT >= state->arena_pages)
			return -EOPNOTSUPP;
		for (b = start >> PAGE_SHIFT; b <= (end - 1) >> PAGE_SHIFT; b++)
			if (WARN_ON_ONCE(!rqstp->rq_pages[b]))
				return -EOPNOTSUPP;
	}

	for (b = state->fixed_bytes, index = 0;
	     index < state->lt.count && b < end; index++) {
		struct bio_vec *bvec = &state->lt.bvec[index];
		u32 lo = max(b, start), hi = min(b + bvec->bv_len, end);
		bool borrowed = test_bit(index, state->lt.borrowed);

		if (lo < hi && (region || borrowed))
			svc_tcp_rx_bounce_copy(rqstp->rq_pages, region,
					       region ? lo - start + align : lo,
					       bvec, lo - b, hi - lo);
		/* Nothing reads a consumed loaned page again: return it now. */
		if (borrowed && lo < hi && b + bvec->bv_len <= end)
			rx_loan_table_return_entry(&state->lt, index);
		b += bvec->bv_len;
	}

	/* @b walks the destination: region from @align, arena by body offset. */
	for (b = region ? align : start, n = 0, end = b + payload->page_len;
	     b < end; n++) {
		u32 len = min_t(u32, end - b, PAGE_SIZE - offset_in_page(b));
		struct page *page = region ? region[n].bv_page :
					     rqstp->rq_pages[b >> PAGE_SHIFT];

		bvec_set_page(&out[n], page, len, offset_in_page(b));
		b += len;
	}

	/* @out is a complete copy whatever happens to the loan's bookkeeping. */
	if (WARN_ON_ONCE(svc_tcp_rx_release_refs(state))) {
		state->reason = SVC_TCP_RX_REASON_INVARIANT;
		trace_svcsock_tcp_rx_lifetime(&svsk->sk_xprt, rqstp, state,
					      SVC_TCP_RX_ERROR);
	}
	svc_tcp_rx_credit_return(state, region ? span : 0);
	state->bounce_pages = region ? n : 0;
	atomic64_inc(&c->bounced);
	trace_svcsock_tcp_rx_lifetime(&svsk->sk_xprt, rqstp, state,
				      SVC_TCP_RX_BOUNCE);
	svc_tcp_rx_clear_xdr(rqstp);
	return n;
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_bounce);

/**
 * svc_tcp_rx_bounce_done - return a bounce's region pages and credit
 * @rqstp: the request that bounced
 * @bvec: what svc_tcp_rx_bounce() filled; clobbered
 * @nvecs: its return value
 *
 * Call once the device has the data.  The converted credit returns to the
 * budget it came from, which may wake that budget's oldest waiter or
 * grant its parked READs.
 */
VISIBLE_IF_KUNIT void svc_tcp_rx_bounce_done(struct svc_rqst *rqstp,
					     struct bio_vec *bvec,
					     unsigned int nvecs)
{
	struct svc_tcp_rx_state *state = rqstp->rq_tcp_rx;
	struct svc_tcp_rx_budget *c;
	struct rx_loan_waiter *wake;
	LIST_HEAD(granted);
	u32 lent;

	if (!state || !state->bounce_pages)
		return;
	WARN_ON_ONCE(nvecs != state->bounce_pages);
	svc_tcp_rx_bounce_give(bvec, state->bounce_pages);
	c = state->bounce_budget;
	lent = state->bounce_credit + state->bounce_reserve;
	/* The consumer first, so the kick below sees it under its share. */
	if (c && lent)
		rx_loan_consumer_put(state->bounce_consumer, c->lb->read, lent);
	if (c && lent && state->bounce_stocked) {
		/* To this CPU's stock; the record is uncounted once it is back. */
		long back = svc_tcp_rx_stock_put(c, lent);

		this_cpu_sub(c->lb->stock->lent, lent);
		if (back) {
			spin_lock_bh(&c->lb->lock);
			c->lb->avail += back;
			this_cpu_dec(c->lb->stock->records);
			rx_loan_budget_update_locked(c->lb);
			wake = svc_tcp_rx_credit_kick_locked(c, &granted);
			spin_unlock_bh(&c->lb->lock);
			rx_loan_wake(wake);
			rx_loan_wake_granted(&granted);
		} else {
			this_cpu_dec(c->lb->stock->records);
		}
	} else if (c && lent) {
		spin_lock_bh(&c->lb->lock);
		c->lb->avail += state->bounce_credit;
		c->lb->reserve += state->bounce_reserve;
		c->lb->lent -= lent;
		rx_loan_budget_update_locked(c->lb);
		wake = svc_tcp_rx_credit_kick_locked(c, &granted);
		spin_unlock_bh(&c->lb->lock);
		rx_loan_region_put(lent);
		rx_loan_wake(wake);
		rx_loan_wake_granted(&granted);
	}
	state->bounce_budget = NULL;
	state->bounce_consumer = NULL;
	state->bounce_pages = 0;
	state->bounce_credit = 0;
	state->bounce_reserve = 0;
	state->bounce_stocked = false;
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_bounce_done);

static int svc_tcp_bounce_payload(struct svc_rqst *rqstp,
				  const struct xdr_buf *payload,
				  unsigned int align, unsigned int mem_align,
				  struct bio_vec *bvec, unsigned int bvec_size)
{
	return svc_tcp_rx_bounce(container_of(rqstp->rq_xprt, struct svc_sock,
					      sk_xprt),
				 rqstp, payload, align, mem_align, bvec,
				 bvec_size);
}

/*
 * A confined READ reply: its region pages and the credit they hold, kept
 * until TCP frees the reply's last socket buffer (ubuf_info completion).
 * Nothing is allocated per READ: every page of the partition READs draw on
 * has a descriptor and a link, set up once with the region.  A reply uses
 * its first page's descriptor, and the links chain its pages by index.
 * Overrun (park->over) is never returned, so it is not kept here.
 */
struct svc_rx_region_tx {
	struct ubuf_info		ubuf;
	struct llist_node		done;
	struct svc_tcp_rx_budget	*budget;	/* the credit's */
	struct svc_sock			*svsk;	/* holds a transport reference */
	__be32				xid;
	u16				count;
	u16				credit;	/* from avail */
	u16				reserve;	/* from the reserve */
	u16				tries;	/* waits for page references */
	u32				share;	/* the connection's, at admission */
	bool				shared;	/* counted in the connection share */
	bool				sent;
	/*
	 * the credit is charged to the namespace's consumer (as granted:
	 * it may have unregistered since).  A flag, not the pointer, keeps
	 * the descriptor in one cacheline; the consumer is the namespace's.
	 */
	bool				charged;
};
static_assert(sizeof(struct svc_rx_region_tx) <= 64);

static struct svc_rx_region_tx *svc_rx_region_txs;
static u32 *svc_rx_region_links;	/* each page's successor */
static unsigned long svc_rx_region_txs_pfn;
static unsigned long svc_rx_region_txs_nr;	/* pages READs draw on */
static bool svc_rx_region_txs_failed;
static DEFINE_MUTEX(svc_rx_region_txs_mutex);

#if IS_ENABLED(CONFIG_KUNIT)
/*
 * KUnit stand-in for the region's READ partition: a free-page stack over
 * pages indexed by their place in @svc_rx_region_kunit_all.
 */
static struct page **svc_rx_region_kunit_free, **svc_rx_region_kunit_all;
static struct svc_rx_region_tx *svc_rx_region_kunit_txs;
static u32 *svc_rx_region_kunit_links;
static unsigned int svc_rx_region_kunit_nfree, svc_rx_region_kunit_capacity;
static unsigned int svc_rx_region_kunit_escaped;
static DEFINE_SPINLOCK(svc_rx_region_kunit_lock);

void svc_tcp_rx_region_kunit_read_region(struct page **pages, unsigned int n)
{
	struct svc_rx_region_tx *txs = NULL;
	struct page **all = NULL;
	u32 *links = NULL;

	if (pages) {
		all = kmemdup_array(pages, n, sizeof(*pages), GFP_KERNEL);
		txs = kcalloc(n, sizeof(*txs), GFP_KERNEL);
		links = kcalloc(n, sizeof(*links), GFP_KERNEL);
		if (WARN_ON(!all || !txs || !links)) {
			kfree(all);
			kfree(txs);
			kfree(links);
			return;
		}
	}
	spin_lock_bh(&svc_rx_region_kunit_lock);
	swap(svc_rx_region_kunit_all, all);
	swap(svc_rx_region_kunit_txs, txs);
	swap(svc_rx_region_kunit_links, links);
	svc_rx_region_kunit_free = pages;
	svc_rx_region_kunit_nfree = n;
	svc_rx_region_kunit_capacity = n;
	svc_rx_region_kunit_escaped = 0;
	spin_unlock_bh(&svc_rx_region_kunit_lock);
	kfree(all);
	kfree(txs);
	kfree(links);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_region_kunit_read_region);

unsigned int svc_tcp_rx_region_kunit_read_free(void)
{
	return READ_ONCE(svc_rx_region_kunit_nfree);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_region_kunit_read_free);

/* @n stand-in pages escape, as page_pool_rx_region_capacity() counts them. */
void svc_tcp_rx_region_kunit_escape(unsigned int n)
{
	WRITE_ONCE(svc_rx_region_kunit_escaped, n);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_region_kunit_escape);
#endif

static unsigned int svc_rx_region_take_pages(struct page **pages,
					     unsigned int n)
{
#if IS_ENABLED(CONFIG_KUNIT)
	if (svc_rx_region_kunit_free) {
		unsigned int i;

		spin_lock_bh(&svc_rx_region_kunit_lock);
		if (svc_rx_region_kunit_nfree < n) {
			spin_unlock_bh(&svc_rx_region_kunit_lock);
			return 0;
		}
		for (i = 0; i < n; i++)
			pages[i] = svc_rx_region_kunit_free[--svc_rx_region_kunit_nfree];
		spin_unlock_bh(&svc_rx_region_kunit_lock);
		return n;
	}
#endif
	return page_pool_rx_region_take_pages(true, pages, n);
}

static void svc_rx_region_give_pages(struct page **pages, unsigned int n)
{
#if IS_ENABLED(CONFIG_KUNIT)
	if (svc_rx_region_kunit_free) {
		unsigned int i;

		spin_lock_bh(&svc_rx_region_kunit_lock);
		for (i = 0; i < n; i++)
			if (!WARN_ON_ONCE(svc_rx_region_kunit_nfree >=
					  svc_rx_region_kunit_capacity))
				svc_rx_region_kunit_free[svc_rx_region_kunit_nfree++] = pages[i];
		spin_unlock_bh(&svc_rx_region_kunit_lock);
		return;
	}
#endif
	page_pool_rx_region_give_pages(pages, n);
}

/*
 * Pages the partition READs draw on can still lend, its escaped pages
 * excluded: a READ needing more could never be confined, so it is served
 * unconfined rather than waiting.  Zero until svc_rx_region_bind_reads()
 * succeeds.
 */
static unsigned long svc_rx_region_read_span(void)
{
#if IS_ENABLED(CONFIG_KUNIT)
	if (svc_rx_region_kunit_free)
		return svc_rx_region_kunit_capacity -
		       min(READ_ONCE(svc_rx_region_kunit_escaped),
			   svc_rx_region_kunit_capacity);
#endif
	if (!READ_ONCE(svc_rx_region_txs_nr))
		return 0;
	return page_pool_rx_region_capacity(true);
}

/*
 * The READ partition's descriptors and links, made once a boot by the first
 * READ (which also makes the region if no listed device has).  A failure is
 * not retried: READs are then served unconfined.
 */
static bool svc_rx_region_bind_reads(void)
{
	struct svc_rx_region_tx *txs;
	unsigned long pfn, nr;
	u32 *links;

#if IS_ENABLED(CONFIG_KUNIT)
	if (svc_rx_region_kunit_free)
		return true;
#endif
	if (smp_load_acquire(&svc_rx_region_txs))
		return true;
	if (READ_ONCE(svc_rx_region_txs_failed) ||
	    !page_pool_rx_region_bind_reads())
		return false;
	mutex_lock(&svc_rx_region_txs_mutex);
	if (!svc_rx_region_txs && !svc_rx_region_txs_failed) {
		nr = page_pool_rx_region_span(true, &pfn);
		txs = nr ? kvcalloc(nr, sizeof(*txs), GFP_KERNEL) : NULL;
		links = nr ? kvcalloc(nr, sizeof(*links), GFP_KERNEL) : NULL;
		if (txs && links) {
			svc_rx_region_txs_pfn = pfn;
			svc_rx_region_links = links;
			WRITE_ONCE(svc_rx_region_txs_nr, nr);
			smp_store_release(&svc_rx_region_txs, txs);
		} else {
			pr_warn("svc: no READ descriptors for %lu rx region pages\n",
				nr);
			kvfree(txs);
			kvfree(links);
			WRITE_ONCE(svc_rx_region_txs_failed, true);
		}
	}
	mutex_unlock(&svc_rx_region_txs_mutex);
	return smp_load_acquire(&svc_rx_region_txs);
}

static struct svc_rx_region_tx *svc_rx_region_tx_at(u32 idx)
{
#if IS_ENABLED(CONFIG_KUNIT)
	if (svc_rx_region_kunit_free)
		return &svc_rx_region_kunit_txs[idx];
#endif
	return &svc_rx_region_txs[idx];
}

static u32 *svc_rx_region_link(u32 idx)
{
#if IS_ENABLED(CONFIG_KUNIT)
	if (svc_rx_region_kunit_free)
		return &svc_rx_region_kunit_links[idx];
#endif
	return &svc_rx_region_links[idx];
}

static u32 svc_rx_region_tx_index(const struct svc_rx_region_tx *tx)
{
	return tx - svc_rx_region_tx_at(0);
}

static u32 svc_rx_region_page_index(struct page *page)
{
#if IS_ENABLED(CONFIG_KUNIT)
	if (svc_rx_region_kunit_free) {
		u32 i;

		for (i = 0; i < svc_rx_region_kunit_capacity; i++)
			if (svc_rx_region_kunit_all[i] == page)
				return i;
		WARN_ON_ONCE(1);
		return 0;
	}
#endif
	return page_to_pfn(page) - svc_rx_region_txs_pfn;
}

static struct page *svc_rx_region_page(u32 idx)
{
#if IS_ENABLED(CONFIG_KUNIT)
	if (svc_rx_region_kunit_free)
		return svc_rx_region_kunit_all[idx];
#endif
	return pfn_to_page(svc_rx_region_txs_pfn + idx);
}

/*
 * Pages given back per call: a batch of 32 or more goes straight to the
 * region's shards, so a large READ's completion skips the CPU caches.
 */
#define SVC_RX_REGION_BATCH	64

/* Return @n pages chained from @idx; each link is read before its page goes. */
static void svc_rx_region_chain_give(u32 idx, unsigned int n)
{
	struct page *batch[SVC_RX_REGION_BATCH];
	unsigned int k = 0;

	while (n--) {
		batch[k++] = svc_rx_region_page(idx);
		idx = *svc_rx_region_link(idx);
		if (k == SVC_RX_REGION_BATCH || !n) {
			svc_rx_region_give_pages(batch, k);
			k = 0;
		}
	}
}

static const struct ubuf_info_ops svc_rx_region_tx_ops;

/*
 * The reply for the READ @w describes, which holds @pages and w's credit:
 * link the pages and fill in the first one's descriptor.  The pages are
 * the caller's alone, so no lock is needed.
 */
static struct svc_rx_region_tx *
svc_rx_region_tx_make(struct svc_rx_region_park *w, struct page **pages)
{
	u32 first = svc_rx_region_page_index(pages[0]), prev = first, idx;
	struct svc_rx_region_tx *tx = svc_rx_region_tx_at(first);
	unsigned int i;

	for (i = 1; i < w->count; i++) {
		idx = svc_rx_region_page_index(pages[i]);
		*svc_rx_region_link(prev) = idx;
		prev = idx;
	}
	tx->ubuf.ops = &svc_rx_region_tx_ops;
	tx->ubuf.flags = SKBFL_ZEROCOPY_FRAG | SKBFL_DONT_ORPHAN;
	refcount_set(&tx->ubuf.refcnt, 1);
	tx->budget = svc_tcp_rx_budget_of(w->w.budget);
	svc_xprt_get(&w->svsk->sk_xprt);
	tx->svsk = w->svsk;
	tx->xid = w->xid;
	tx->count = w->count;
	tx->credit = w->w.credit;
	tx->reserve = w->w.reserve;
	tx->tries = 0;
	tx->share = w->share;
	tx->shared = w->shared;	/* the share goes with the pages */
	tx->charged = w->w.consumer;
	tx->sent = false;
	w->shared = false;
	return tx;
}

/*
 * Most region pages one connection may hold: auto is two largest READs,
 * each with its direct-I/O alignment page.
 */
static unsigned int svc_rx_region_share(const struct svc_sock *svsk)
{
	return READ_ONCE(svc_tcp_rx_region_conn_share) ?:
	       2 * ((unsigned int)DIV_ROUND_UP(svsk->sk_xprt.xpt_server->sv_max_payload,
					       PAGE_SIZE) + 1);
}

/*
 * Reply releases: TCP's completions land on the completing CPU's list; a
 * reply whose pages are still referenced (a frag downgraded from managed,
 * which skb_release_data() drops after the reply's reference) waits on the
 * busy list, retried each jiffy and given up after svc_rx_region_busy_tries.
 * The list's work runs at normal priority on the completing CPU, so it
 * does not preempt the nfsd threads there.  It can run before
 * skb_release_data() has dropped the frags' references, so a reply first
 * found busy is queued again at once; only later retries wait a jiffy.
 */
struct svc_rx_region_done {
	struct llist_head	list;
	struct work_struct	work;
};

static DEFINE_PER_CPU(struct svc_rx_region_done, svc_rx_region_done);
static LLIST_HEAD(svc_rx_region_busy);
static void svc_rx_region_busy_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(svc_rx_region_busy_work, svc_rx_region_busy_fn);
#define svc_rx_region_busy_tries	(10 * HZ)

static void svc_rx_region_tx_release(struct svc_rx_region_tx *tx);

static void svc_rx_region_tx_queue(struct svc_rx_region_tx *tx)
{
	struct svc_rx_region_done *d = get_cpu_ptr(&svc_rx_region_done);

	if (llist_add(&tx->done, &d->list))
		queue_work_on(smp_processor_id(), system_percpu_wq, &d->work);
	put_cpu_ptr(&svc_rx_region_done);
}

static void svc_rx_region_tx_complete(struct sk_buff *skb,
				      struct ubuf_info *uarg,
				      bool zerocopy_success)
{
	struct svc_rx_region_tx *tx = container_of(uarg, struct svc_rx_region_tx,
						   ubuf);

	if (refcount_dec_and_test(&uarg->refcnt))
		svc_rx_region_tx_queue(tx);
}

static const struct ubuf_info_ops svc_rx_region_tx_ops = {
	.complete	= svc_rx_region_tx_complete,
};

/* Is any of @tx's pages still referenced beyond its region ownership? */
static bool svc_rx_region_tx_busy(const struct svc_rx_region_tx *tx)
{
	u32 idx = svc_rx_region_tx_index(tx);
	unsigned int i;

	for (i = 0; i < tx->count; i++) {
		if (page_ref_count(svc_rx_region_page(idx)) != 1)
			return true;
		idx = *svc_rx_region_link(idx);
	}
	return false;
}

static void svc_rx_region_tx_finish(struct llist_node *list)
{
	struct svc_rx_region_tx *tx, *next;

	llist_for_each_entry_safe(tx, next, list, done) {
		if (svc_rx_region_tx_busy(tx) &&
		    ++tx->tries < svc_rx_region_busy_tries) {
			if (tx->tries == 1)	/* once more, at once */
				svc_rx_region_tx_queue(tx);
			else if (llist_add(&tx->done, &svc_rx_region_busy))
				mod_delayed_work(system_dfl_wq,
						 &svc_rx_region_busy_work, 1);
			continue;
		}
		/* Still referenced after the limit: the pages escape. */
		svc_rx_region_tx_release(tx);
	}
}

static void svc_rx_region_done_fn(struct work_struct *work)
{
	struct svc_rx_region_done *d = container_of(work,
						    struct svc_rx_region_done,
						    work);

	svc_rx_region_tx_finish(llist_del_all(&d->list));
}

static void svc_rx_region_busy_fn(struct work_struct *work)
{
	svc_rx_region_tx_finish(llist_del_all(&svc_rx_region_busy));
}

/* Clear @svsk's stall once, and let it take requests again. */
static void svc_rx_region_share_unstall(struct svc_sock *svsk)
{
	if (READ_ONCE(svsk->sk_rx_region_stalled) &&
	    xchg(&svsk->sk_rx_region_stalled, false))
		svc_xprt_enqueue(&svsk->sk_xprt);
}

/*
 * Stop @svsk taking new requests: it holds @held pages of its @share.  A
 * reply that returned its pages after the reservation but before the flag
 * saw no stall, and nothing else would clear it, so recheck after setting
 * it.  The barrier pairs with atomic_sub_return() in
 * svc_rx_region_share_put(): one side or both see the other.
 */
VISIBLE_IF_KUNIT void svc_rx_region_share_stall(struct svc_sock *svsk,
						unsigned int share,
						unsigned int held, __be32 xid)
{
	if (READ_ONCE(svsk->sk_rx_region_stalled))
		return;
	WRITE_ONCE(svsk->sk_rx_region_stalled, true);
	smp_mb();	/* flag before inflight; see svc_rx_region_share_put() */
	if (atomic_read(&svsk->sk_rx_region_inflight) < share) {
		svc_rx_region_share_unstall(svsk);
		return;
	}
	atomic64_inc(&svc_tcp_rx_budget_read_for(svsk)->share_stalls);
	trace_svcsock_tcp_rx_region_read(&svsk->sk_xprt, xid, held,
					 "share-stall", "-");
}
EXPORT_SYMBOL_IF_KUNIT(svc_rx_region_share_stall);

/*
 * Reserve @n pages of @svsk's @share.  A connection holding nothing may
 * take a READ larger than its share; otherwise the share is a hard bound.
 */
static bool svc_rx_region_share_get(struct svc_sock *svsk, unsigned int n,
				    unsigned int share, __be32 xid)
{
	int cur = atomic_read(&svsk->sk_rx_region_inflight);

	do {
		if (cur && cur + n > share)
			return false;
	} while (!atomic_try_cmpxchg(&svsk->sk_rx_region_inflight, &cur,
				     cur + n));
	if (cur + n >= share)
		svc_rx_region_share_stall(svsk, share, cur + n, xid);
	return true;
}

/*
 * Return @n pages of the share and replay every READ that waited for room:
 * each re-reserves, and one that still does not fit parks again behind a
 * reply in flight, whose return wakes it.  None is left without a waker.
 */
static void svc_rx_region_share_put(struct svc_sock *svsk, unsigned int n,
				    unsigned int share)
{
	struct svc_rx_region_park *park, *next;
	LIST_HEAD(wake);

	if (atomic_sub_return(n, &svsk->sk_rx_region_inflight) < share)
		svc_rx_region_share_unstall(svsk);
	spin_lock_bh(&svsk->sk_rx_region_lock);
	list_splice_init(&svsk->sk_rx_region_waiters, &wake);
	spin_unlock_bh(&svsk->sk_rx_region_lock);
	list_for_each_entry_safe(park, next, &wake, node) {
		list_del_init(&park->node);
		svc_revisit_rx_region(container_of(park, struct svc_deferred_req,
						   rx_region));
	}
}

/*
 * Return a reply's pages and credit, and its share of the connection.  The
 * descriptor belongs to whoever takes its first page next: read it first.
 */
static void svc_rx_region_tx_release(struct svc_rx_region_tx *tx)
{
	struct svc_tcp_rx_budget *c = tx->budget;
	struct svc_sock *svsk = tx->svsk;
	unsigned int count = tx->count, share = tx->share;
	long credit = tx->credit, reserve = tx->reserve;
	bool shared = tx->shared, charged = tx->charged;

	if (tx->sent)
		trace_svcsock_tcp_rx_region_read(&svsk->sk_xprt, tx->xid, count,
						 "complete", "-");
	svc_rx_region_chain_give(svc_rx_region_tx_index(tx), count);
	/* the consumer it was granted to, registered or not now */
	svc_tcp_rx_budget_read_return(c, charged ?
				      svc_tcp_rx_net_consumer(svsk) : NULL,
				      credit, reserve);
	if (shared)
		svc_rx_region_share_put(svsk, count, share);
	svc_xprt_put(&svsk->sk_xprt);
}

/*
 * @rqstp now holds @tx's pages, listed in rq_rx_region_pages and installed
 * by each READ as it runs.
 */
static void svc_rx_region_hold(struct svc_rqst *rqstp,
			       struct svc_rx_region_tx *tx)
{
	rqstp->rq_rx_region_tx = tx;
	rqstp->rq_rx_region_count = tx->count;
	rqstp->rq_rx_region_used = 0;
}

static struct svc_rx_region_tx *svc_rx_region_unhold(struct svc_rqst *rqstp)
{
	struct svc_rx_region_tx *tx = rqstp->rq_rx_region_tx;

	rqstp->rq_rx_region_tx = NULL;
	rqstp->rq_rx_region_count = 0;
	rqstp->rq_rx_region_used = 0;
	return tx;
}

/*
 * xpo_rx_region_unconfine: the region pages are not sent after all (a file
 * without direct I/O, or a reply that is never sent): empty their slots,
 * put the request's own pages back, and return the pages and credit.
 */
VISIBLE_IF_KUNIT void svc_tcp_rx_region_unconfine(struct svc_rqst *rqstp)
{
	if (!svc_rqst_rx_region_held(rqstp))
		return;
	svc_rqst_rx_region_uninstall(rqstp, true);
	svc_rx_region_tx_release(svc_rx_region_unhold(rqstp));
}

/* xpo_rx_region_unconfine: a READ served from ordinary pages is counted. */
VISIBLE_IF_KUNIT void svc_tcp_rx_region_unconfine_op(struct svc_rqst *rqstp,
						     bool served)
{
	struct svc_sock *svsk = container_of(rqstp->rq_xprt, struct svc_sock,
					     sk_xprt);

	if (served && svc_rqst_rx_region_held(rqstp))
		atomic64_inc(&svc_tcp_rx_budget_read_for(svsk)->read_unconfined);
	svc_tcp_rx_region_unconfine(rqstp);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_region_unconfine_op);

/* xpo_rx_region_park_free: a parked READ is dropped before its replay. */
VISIBLE_IF_KUNIT void svc_tcp_rx_region_park_free(struct svc_rx_region_park *park)
{
	/* Granted while parked: its credit (never its overrun) goes back. */
	if (park->granted)
		rx_loan_grant_drop(park->w.budget, park->w.consumer,
				   park->w.credit, park->w.reserve);
	if (park->shared)
		svc_rx_region_share_put(park->svsk, park->count, park->share);
}

/*
 * Confinement needs a plain TCP stream the CPU never touches: why a READ
 * cannot have it, or NULL.
 */
static const char *svc_rx_region_read_ineligible(struct svc_rqst *rqstp,
						 unsigned int pages)
{
	switch (svc_auth_flavor(rqstp)) {
	case RPC_AUTH_GSS_KRB5:
	case RPC_AUTH_GSS_KRB5I:
	case RPC_AUTH_GSS_KRB5P:
		return "gss";
	}
	if (test_bit(XPT_TLS_SESSION, &rqstp->rq_xprt->xpt_flags))
		return "tls";
	/* Parking saves head-only calls: a loaned WRITE payload stays out. */
	if (rqstp->rq_arg.page_len)
		return "arg-pages";
	if (pages > U16_MAX ||
	    pages >= rqstp->rq_page_end - rqstp->rq_next_page)
		return "reply-pages";
	return NULL;
}

/* Park @rqstp until its connection's share has room for it. */
static int svc_rx_region_conn_park(struct svc_rqst *rqstp,
				   struct svc_sock *svsk, unsigned int pages)
{
	struct svc_rx_region_park *park;
	struct svc_deferred_req *dr;
	bool now = false;

	dr = svc_defer_rx_region(rqstp);
	if (!dr)
		return -ENOMEM;
	park = &dr->rx_region;
	*park = (struct svc_rx_region_park){ .svsk = svsk,
					     .xid = rqstp->rq_xid,
					     .count = pages, .parked = true };
	spin_lock_bh(&svsk->sk_rx_region_lock);
	list_add_tail(&park->node, &svsk->sk_rx_region_waiters);
	/*
	 * The connection's last reply may have been released meanwhile, or
	 * the connection drained (XPT_DEAD is set before the drain takes this
	 * lock): replay now, which drops it on a dead connection.
	 */
	if (!atomic_read(&svsk->sk_rx_region_inflight) ||
	    test_bit(XPT_DEAD, &svsk->sk_xprt.xpt_flags)) {
		list_del_init(&park->node);
		now = true;
	}
	spin_unlock_bh(&svsk->sk_rx_region_lock);
	trace_svcsock_tcp_rx_region_read(&svsk->sk_xprt, rqstp->rq_xid, pages,
					 "conn-park", "-");
	if (now)
		svc_revisit_rx_region(dr);
	return -EAGAIN;
}

/* A closing connection replays its share waiters, which are dropped. */
static void svc_rx_region_conn_drain(struct svc_sock *svsk)
{
	struct svc_rx_region_park *park, *next;
	LIST_HEAD(drain);

	spin_lock_bh(&svsk->sk_rx_region_lock);
	list_splice_init(&svsk->sk_rx_region_waiters, &drain);
	spin_unlock_bh(&svsk->sk_rx_region_lock);
	list_for_each_entry_safe(park, next, &drain, node) {
		list_del_init(&park->node);
		svc_revisit_rx_region(container_of(park, struct svc_deferred_req,
						   rx_region));
	}
}

/*
 * Park @rqstp, which @w describes, on @c until it is granted.  The park is
 * linked under the same lock hold that re-checks credit, so a return that
 * raced the first attempt grants it at once.
 */
static int svc_rx_region_read_park(struct svc_rqst *rqstp,
				   struct svc_tcp_rx_budget *c,
				   struct svc_rx_region_park *w)
{
	struct svc_xprt *xprt = rqstp->rq_xprt;
	unsigned int pages = w->count;
	__be32 xid = rqstp->rq_xid;
	struct svc_rx_region_park *park;
	struct svc_deferred_req *dr;

	dr = svc_defer_rx_region(rqstp);
	if (!dr)
		return -ENOMEM;
	park = &dr->rx_region;
	*park = *w;
	park->parked = true;
	w->shared = false;	/* the park has the share now */
	park->w.ops = &svc_rx_region_park_ops;
	rx_loan_park(c->lb, svc_tcp_rx_consumer(container_of(xprt, struct svc_sock,
							       sk_xprt)), &park->w);
	trace_svcsock_tcp_rx_region_read(xprt, xid, pages, "park", "-");
	return -EAGAIN;
}

/*
 * Pages for a READ that holds credit, by its own thread: all or none, into
 * rq_rx_region_pages.  False when the partition is short (device rings and
 * pool caches hold pages beyond the budget): the credit goes back, and the
 * caller returns the share and serves the READ unconfined (read_short, Q-D).
 */
static bool svc_rx_region_read_pages(struct svc_rqst *rqstp,
				     struct svc_rx_region_park *w,
				     const char *how)
{
	struct svc_tcp_rx_budget *c = svc_tcp_rx_budget_of(w->w.budget);

	if (svc_rx_region_take_pages(rqstp->rq_rx_region_pages, w->count) !=
	    w->count) {
		atomic64_inc(&c->read_short);
		svc_tcp_rx_budget_read_return(c, w->w.consumer, w->w.credit,
					      w->w.reserve);
		return false;
	}
	svc_rx_region_hold(rqstp, svc_rx_region_tx_make(w,
						rqstp->rq_rx_region_pages));
	trace_svcsock_tcp_rx_region_read(&w->svsk->sk_xprt, rqstp->rq_xid,
					 w->count, how, "-");
	return true;
}

/*
 * xpo_rx_region_read: take credit and region pages for a decoded call's
 * READ payloads (installed later, by each READ as it runs), or park it, or
 * leave it unconfined.  Runs before the duplicate-reply cache and before
 * any NFS operation.
 */
VISIBLE_IF_KUNIT int svc_tcp_rx_region_read(struct svc_rqst *rqstp,
					    unsigned int pages)
{
	struct svc_sock *svsk = container_of(rqstp->rq_xprt, struct svc_sock,
					     sk_xprt);
	struct svc_deferred_req *dr = rqstp->rq_deferred;
	struct svc_tcp_rx_budget *c = svc_tcp_rx_budget_read_for(svsk);
	struct svc_rx_region_park want = {
		.svsk = svsk, .xid = rqstp->rq_xid, .count = pages,
	};
	const char *why;
	int ret;

	if (!READ_ONCE(svc_tcp_rx_region_reads))
		return 0;
	if (dr && dr->rx_region.parked) {
		struct svc_rx_region_park *park = &dr->rx_region;

		park->parked = false;
		if (park->granted) {
			/* The replay of a READ granted credit while parked. */
			park->granted = false;
			c = svc_tcp_rx_budget_of(park->w.budget);
			if (svc_rx_region_read_pages(rqstp, park, "grant"))
				return 0;
			if (park->shared) {
				park->shared = false;
				svc_rx_region_share_put(svsk, park->count,
							park->share);
			}
			why = "short";
			goto unconfined;
		}
		/* It waited for its connection's share: admit it afresh. */
	}
	why = svc_rx_region_read_ineligible(rqstp, pages);
	if (!why && !svc_rx_region_bind_reads())
		why = "unbound";
	/* A READ larger than the partition could never be confined (1b). */
	if (!why && pages > svc_rx_region_read_span())
		why = "span";
	if (why)
		goto unconfined;
	want.share = svc_rx_region_share(svsk);
	if (!svc_rx_region_share_get(svsk, pages, want.share, rqstp->rq_xid)) {
		ret = svc_rx_region_conn_park(rqstp, svsk, pages);
		if (ret == -EAGAIN)
			return ret;
		why = "no-defer";
		goto unconfined;
	}
	want.shared = true;
	want.w.need = pages;
	ret = rx_loan_grant(c->lb, svc_tcp_rx_consumer(svsk), &want.w);
	switch (ret) {
	case 0:
		if (svc_rx_region_read_pages(rqstp, &want, "admit"))
			return 0;
		why = "short";
		break;
	case -EAGAIN:
		ret = svc_rx_region_read_park(rqstp, c, &want);
		if (ret == -EAGAIN)
			return ret;
		why = "no-defer";
		break;
	default:
		why = "over-budget";
		break;
	}
	svc_rx_region_share_put(svsk, pages, want.share);
unconfined:
	atomic64_inc(&c->read_unconfined);
	trace_svcsock_tcp_rx_region_read(&svsk->sk_xprt, rqstp->rq_xid, pages,
					 "unconfined", why);
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_region_read);
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_region_unconfine);
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_region_park_free);

/* Hand the reply's region pages to TCP; they return on completion. */
static void svc_rx_region_handoff(struct svc_rqst *rqstp)
{
	struct svc_rx_region_tx *tx = svc_rx_region_unhold(rqstp);

	svc_rqst_rx_region_uninstall(rqstp, false);
	tx->sent = true;
	net_zcopy_put(&tx->ubuf);
}

static void svc_rx_region_init(void)
{
	int cpu;

	BUILD_BUG_ON(svc_rx_region_busy_tries > U16_MAX);
	for_each_possible_cpu(cpu) {
		struct svc_rx_region_done *d = per_cpu_ptr(&svc_rx_region_done,
							   cpu);

		init_llist_head(&d->list);
		INIT_WORK(&d->work, svc_rx_region_done_fn);
	}
}

static void svc_rx_region_cleanup(void)
{
	int cpu;

	for_each_possible_cpu(cpu)
		flush_work(&per_cpu_ptr(&svc_rx_region_done, cpu)->work);
	flush_delayed_work(&svc_rx_region_busy_work);
	kvfree(svc_rx_region_txs);
	kvfree(svc_rx_region_links);
}

#if IS_ENABLED(CONFIG_KUNIT)
/*
 * What svc_tcp_sendmsg_rx_region() leaves behind: the reply's pages handed
 * to TCP, holding @skbs references for queued socket buffers, and the
 * sender's reference dropped.  Returns the reply's ubuf_info for the test
 * to free those buffers with.
 */
struct ubuf_info *svc_tcp_rx_region_kunit_send(struct svc_rqst *rqstp,
					       unsigned int skbs)
{
	struct svc_rx_region_tx *tx = rqstp->rq_rx_region_tx;

	refcount_add(skbs, &tx->ubuf.refcnt);
	svc_rx_region_handoff(rqstp);
	return &tx->ubuf;
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_region_kunit_send);

static void svc_rx_region_kunit_flush_kicks(void)
{
	struct svc_tcp_rx_budget *pos;

	flush_work(&svc_rx_budget_default.lb->kick_work);
	flush_work(&svc_rx_budget_read.lb->kick_work);
	mutex_lock(&svc_rx_budget_cfg_lock);
	list_for_each_entry(pos, &svc_rx_budget_pools, node)
		flush_work(&pos->lb->kick_work);
	mutex_unlock(&svc_rx_budget_cfg_lock);
}

/* Run pending reply releases and dropped-park kicks now. */
void svc_tcp_rx_region_kunit_flush(void)
{
	int cpu;

	for_each_possible_cpu(cpu)
		flush_work(&per_cpu_ptr(&svc_rx_region_done, cpu)->work);
	flush_delayed_work(&svc_rx_region_busy_work);
	for_each_possible_cpu(cpu)
		flush_work(&per_cpu_ptr(&svc_rx_region_done, cpu)->work);
	svc_rx_region_kunit_flush_kicks();
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_region_kunit_flush);

void svc_tcp_rx_region_kunit_reads(bool on)
{
	WRITE_ONCE(svc_tcp_rx_region_reads, on);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_region_kunit_reads);

static int svc_tcp_has_wspace(struct svc_xprt *xprt);

int svc_tcp_rx_region_kunit_has_wspace(struct svc_sock *svsk)
{
	return svc_tcp_has_wspace(&svsk->sk_xprt);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_region_kunit_has_wspace);

void svc_tcp_rx_region_kunit_conn_drain(struct svc_sock *svsk)
{
	svc_rx_region_conn_drain(svsk);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_region_kunit_conn_drain);

static void svc_rx_region_kunit_drain_budget(struct svc_tcp_rx_budget *c)
{
	struct svc_rx_region_park *park, *next;
	struct rx_loan_waiter *w, *n;
	LIST_HEAD(drain);

	spin_lock_bh(&c->lb->lock);
	list_for_each_entry_safe(w, n, &c->lb->waiters, node)
		if (w->exact)
			list_move_tail(&w->node, &drain);
	spin_unlock_bh(&c->lb->lock);
	list_for_each_entry_safe(park, next, &drain, w.node) {
		struct svc_deferred_req *dr =
			container_of(park, struct svc_deferred_req, rx_region);

		list_del(&park->w.node);
		if (park->shared)
			svc_rx_region_share_put(park->svsk, park->count,
						park->share);
		svc_xprt_put(dr->xprt);
		kfree(dr);
	}
}

/* A failed test must not leave READs parked in a budget for the next. */
void svc_tcp_rx_region_kunit_drain_waiters(void)
{
	struct svc_tcp_rx_budget *pos;

	svc_rx_region_kunit_drain_budget(&svc_rx_budget_default);
	svc_rx_region_kunit_drain_budget(&svc_rx_budget_read);
	mutex_lock(&svc_rx_budget_cfg_lock);
	list_for_each_entry(pos, &svc_rx_budget_pools, node)
		svc_rx_region_kunit_drain_budget(pos);
	mutex_unlock(&svc_rx_budget_cfg_lock);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_region_kunit_drain_waiters);

/*
 * The budget @which names, for svc_tcp_rx_budget_kunit_stats(): the one
 * READs on @svsk draw on (svc_tcp_rx_budget_read_for()), the initial
 * namespace's, the default, or the read pool.
 */
struct svc_tcp_rx_budget *
svc_tcp_rx_region_kunit_budget(struct svc_sock *svsk,
			       enum svc_tcp_rx_kunit_stats which)
{
	switch (which) {
	case SVC_TCP_RX_STATS_INIT:
		return svc_tcp_rx_budget_initial();
	case SVC_TCP_RX_STATS_DEFAULT:
		return &svc_rx_budget_default;
	case SVC_TCP_RX_STATS_POOL:
		return svc_tcp_rx_budget_kunit_read();
	default:
		return svc_tcp_rx_budget_read_for(svsk);
	}
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_region_kunit_budget);

/* Make a sent reply's next busy check its last. */
void svc_tcp_rx_region_kunit_age(struct ubuf_info *ubuf)
{
	container_of(ubuf, struct svc_rx_region_tx, ubuf)->tries =
		svc_rx_region_busy_tries - 1;
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_region_kunit_age);
#endif

/**
 * svc_udp_release_ctxt - Release transport-related resources
 * @xprt: the transport which owned the context
 * @ctxt: the context from rqstp->rq_xprt_ctxt or dr->xprt_ctxt
 *
 */
static void svc_udp_release_ctxt(struct svc_xprt *xprt, void *ctxt)
{
	struct sk_buff *skb = ctxt;

	if (skb)
		consume_skb(skb);
}

union svc_pktinfo_u {
	struct in_pktinfo pkti;
	struct in6_pktinfo pkti6;
};
#define SVC_PKTINFO_SPACE \
	CMSG_SPACE(sizeof(union svc_pktinfo_u))

static void svc_set_cmsg_data(struct svc_rqst *rqstp, struct cmsghdr *cmh)
{
	struct svc_sock *svsk =
		container_of(rqstp->rq_xprt, struct svc_sock, sk_xprt);
	switch (svsk->sk_sk->sk_family) {
	case AF_INET: {
			struct in_pktinfo *pki = CMSG_DATA(cmh);

			cmh->cmsg_level = SOL_IP;
			cmh->cmsg_type = IP_PKTINFO;
			pki->ipi_ifindex = 0;
			pki->ipi_spec_dst.s_addr =
				 svc_daddr_in(rqstp)->sin_addr.s_addr;
			cmh->cmsg_len = CMSG_LEN(sizeof(*pki));
		}
		break;

	case AF_INET6: {
			struct in6_pktinfo *pki = CMSG_DATA(cmh);
			struct sockaddr_in6 *daddr = svc_daddr_in6(rqstp);

			cmh->cmsg_level = SOL_IPV6;
			cmh->cmsg_type = IPV6_PKTINFO;
			pki->ipi6_ifindex = daddr->sin6_scope_id;
			pki->ipi6_addr = daddr->sin6_addr;
			cmh->cmsg_len = CMSG_LEN(sizeof(*pki));
		}
		break;
	}
}

static int svc_sock_result_payload(struct svc_rqst *rqstp, unsigned int offset,
				   unsigned int length)
{
	return 0;
}

/*
 * Report socket names for nfsdfs
 */
static int svc_one_sock_name(struct svc_sock *svsk, char *buf, int remaining)
{
	const struct sock *sk = svsk->sk_sk;
	const char *proto_name = sk->sk_protocol == IPPROTO_UDP ?
							"udp" : "tcp";
	int len;

	switch (sk->sk_family) {
	case PF_INET:
		len = snprintf(buf, remaining, "ipv4 %s %pI4 %d\n",
				proto_name,
				&inet_sk(sk)->inet_rcv_saddr,
				inet_sk(sk)->inet_num);
		break;
#if IS_ENABLED(CONFIG_IPV6)
	case PF_INET6:
		len = snprintf(buf, remaining, "ipv6 %s %pI6 %d\n",
				proto_name,
				&sk->sk_v6_rcv_saddr,
				inet_sk(sk)->inet_num);
		break;
#endif
	default:
		len = snprintf(buf, remaining, "*unknown-%d*\n",
				sk->sk_family);
	}

	if (len >= remaining) {
		*buf = '\0';
		return -ENAMETOOLONG;
	}
	return len;
}

/*
 * The ->read_sock data path invokes neither security_socket_recvmsg()
 * nor the sock:sock_recv_length tracepoint. Dispatch ->recvmsg
 * directly so the whole receive path behaves one way.
 */
static int svc_tcp_recv_cmsg(struct socket *sock, int flags,
			     struct kvec *payload, u8 *type,
			     unsigned int *msg_flags)
{
	const struct proto_ops *ops = READ_ONCE(sock->ops);
	union {
		struct cmsghdr	cmsg;
		u8		buf[CMSG_SPACE(sizeof(u8))];
	} u = {};
	struct msghdr msg = {
		.msg_control	= &u,
		.msg_controllen	= sizeof(u),
	};
	int ret;

	iov_iter_kvec(&msg.msg_iter, ITER_DEST, payload, 1, payload->iov_len);
	ret = ops->recvmsg(sock, &msg, msg_data_left(&msg), flags);
	if (ret < 0)
		return ret;
	*msg_flags = msg.msg_flags;
	*type = tls_get_record_type(sock->sk, &u.cmsg);
	if (!*type && ret)
		return -EBADMSG;
	return ret;
}

static int svc_tcp_recv_ctrl_record(struct svc_sock *svsk)
{
	u8 alert[2], type, level, description;
	struct socket *sock = svsk->sk_sock;
	struct kvec recv_kvec = {
		.iov_base	= alert,
		.iov_len	= sizeof(alert),
	};
	unsigned int msg_flags;
	struct msghdr msg = {};
	int ret;

	if (!test_bit(XPT_TLS_SESSION, &svsk->sk_xprt.xpt_flags))
		return 0;

	/* A data record can become ready between ->read_sock returning
	 * and this probe. A plain receive would take two octets of it
	 * as RPC payload, so peek.
	 */
	ret = svc_tcp_recv_cmsg(sock, MSG_DONTWAIT | MSG_PEEK,
				&recv_kvec, &type, &msg_flags);
	if (ret == -EAGAIN || (!ret && !type))
		return 0;
	if (ret < 0)
		return ret;
	if (type == TLS_RECORD_TYPE_DATA) {
		/* The peek parks the decrypted record on ctx->rx_list,
		 * where it draws no further data_ready. Re-arm or the
		 * RPC hangs until the client times out.
		 */
		set_bit(XPT_DATA, &svsk->sk_xprt.xpt_flags);
		return 0;
	}
	if (type != TLS_RECORD_TYPE_ALERT)
		return -EPROTO;

	ret = svc_tcp_recv_cmsg(sock, MSG_DONTWAIT, &recv_kvec, &type,
				&msg_flags);
	/* The peek found a record at the head, so an -EAGAIN here is
	 * spurious. Propagating it strands the record with no later
	 * announcement, so return -EBADMSG, which closes the transport.
	 */
	if (ret == -EAGAIN)
		return -EBADMSG;
	if (ret < 0)
		return ret;

	/* An Alert record carries exactly one two-octet message (RFC
	 * 8446 Section 5.1). recv_kvec caps the receive at two, so a
	 * longer record produces the same count. MSG_EOR appears only
	 * once kTLS has drained the whole record.
	 */
	if (ret != sizeof(alert) || !(msg_flags & MSG_EOR))
		return -EBADMSG;

	iov_iter_kvec(&msg.msg_iter, ITER_DEST, &recv_kvec, 1,
		      recv_kvec.iov_len);
	tls_alert_recv(sock->sk, &msg, &level, &description);

	/* RFC 8446 Section 6: every alert but a closure alert is
	 * an error alert. kTLS raises no data_ready for records it
	 * already holds, so re-arm for what sits behind the alert.
	 */
	switch (description) {
	case TLS_ALERT_DESC_CLOSE_NOTIFY:
	case TLS_ALERT_DESC_USER_CANCELED:
		set_bit(XPT_DATA, &svsk->sk_xprt.xpt_flags);
		return -EAGAIN;
	default:
		return -ENOTCONN;
	}
}

/*
 * Set socket snd and rcv buffer lengths
 */
static void svc_sock_setbufsize(struct svc_sock *svsk, unsigned int nreqs)
{
	unsigned int max_mesg = svsk->sk_xprt.xpt_server->sv_max_mesg;
	struct socket *sock = svsk->sk_sock;

	nreqs = min(nreqs, INT_MAX / 2 / max_mesg);

	lock_sock(sock->sk);
	sock->sk->sk_sndbuf = nreqs * max_mesg * 2;
	sock->sk->sk_rcvbuf = nreqs * max_mesg * 2;
	sock->sk->sk_write_space(sock->sk);
	release_sock(sock->sk);
}

static void svc_sock_secure_port(struct svc_rqst *rqstp)
{
	assign_bit(RQ_SECURE, &rqstp->rq_flags,
		   svc_port_is_privileged(svc_addr(rqstp)));
}

/*
 * A transport waiting for region credit has stopped reading, and the credit
 * wake, not each arriving skb, runs it again: rx_loan_wake() sets
 * XPT_DATA and enqueues it when credit returns.  Read without the credit
 * lock; a stale answer costs one extra enqueue, or none that the wake does
 * not make.
 */
static bool svc_tcp_rx_credit_waiting(struct svc_sock *svsk)
{
	return !list_empty_careful(&svsk->sk_rx_wait.node);
}

/*
 * An incomplete loaned record waits for its rest: what TCP has queued since
 * the last read is less than the record lacks.  TCP's own readiness test
 * also reports the socket readable under memory pressure or with the
 * receive window nearly closed, so a record that cannot arrive whole is
 * still read.  The mark stays out of sk_rcvlowat, which would stop TCP
 * delaying its ACKs.
 */
static bool svc_tcp_rx_wait_rest(struct svc_sock *svsk, struct sock *sk)
{
	u32 want = READ_ONCE(svsk->sk_rx_want);

	return want && !sock_flag(sk, SOCK_DONE) && !tcp_epollin_ready(sk, want);
}

/*
 * INET callback when data has been received on the socket.
 */
VISIBLE_IF_KUNIT void svc_data_ready(struct sock *sk)
{
	struct svc_sock	*svsk = (struct svc_sock *)sk->sk_user_data;

	trace_sk_data_ready(sk);

	if (svsk) {
		/* Refer to svc_setup_socket() for details. */
		rmb();
		svsk->sk_odata(sk);
		trace_svcsock_data_ready(&svsk->sk_xprt, 0);
		if (test_bit(XPT_HANDSHAKE, &svsk->sk_xprt.xpt_flags))
			return;
		if (svc_tcp_rx_credit_waiting(svsk))
			return;
		if (svc_tcp_rx_wait_rest(svsk, sk))
			return;
		/*
		 * Data usually arrives while XPT_DATA is still set, and the
		 * atomic would take the flags' cache line from the thread
		 * that clears it. The receiver clears XPT_DATA before it
		 * takes the lock this data was queued under, so a plain read
		 * that still sees the bit set is followed by a read that
		 * finds this data.
		 */
		if (!test_bit(XPT_DATA, &svsk->sk_xprt.xpt_flags) &&
		    !test_and_set_bit(XPT_DATA, &svsk->sk_xprt.xpt_flags))
			svc_xprt_enqueue(&svsk->sk_xprt);
	}
}
EXPORT_SYMBOL_IF_KUNIT(svc_data_ready);

/*
 * INET callback when space is newly available on the socket.
 */
static void svc_write_space(struct sock *sk)
{
	struct svc_sock	*svsk = (struct svc_sock *)(sk->sk_user_data);

	if (svsk) {
		/* Refer to svc_setup_socket() for details. */
		rmb();
		trace_svcsock_write_space(&svsk->sk_xprt, 0);
		svsk->sk_owspace(sk);
		svc_xprt_enqueue(&svsk->sk_xprt);
	}
}

static int svc_tcp_has_wspace(struct svc_xprt *xprt)
{
	struct svc_sock *svsk = container_of(xprt, struct svc_sock, sk_xprt);

	if (test_bit(XPT_LISTENER, &xprt->xpt_flags))
		return 1;
	/*
	 * Over its region share: take no new request until replies are
	 * acked.  Parked READs granted credit still replay (deferred requests
	 * are served before new data): they hold credit and share, and only
	 * their replies return them.  Test the list, not XPT_DEFERRED, which
	 * stays set until a dequeue finds the list empty.
	 */
	if (READ_ONCE(svsk->sk_rx_region_stalled) &&
	    list_empty_careful(&xprt->xpt_deferred))
		return 0;
	return !test_bit(SOCK_NOSPACE, &svsk->sk_sock->flags);
}

static void svc_tcp_kill_temp_xprt(struct svc_xprt *xprt)
{
	struct svc_sock *svsk = container_of(xprt, struct svc_sock, sk_xprt);

	sock_no_linger(svsk->sk_sock->sk);
}

/**
 * svc_tcp_handshake_done - Handshake completion handler
 * @data: address of xprt to wake
 * @status: status of handshake
 * @peerid: serial number of key containing the remote peer's identity
 *
 * If a security policy is specified as an export option, we don't
 * have a specific export here to check. So we set a "TLS session
 * is present" flag on the xprt and let an upper layer enforce local
 * security policy.
 */
static void svc_tcp_handshake_done(void *data, int status, key_serial_t peerid)
{
	struct svc_xprt *xprt = data;
	struct svc_sock *svsk = container_of(xprt, struct svc_sock, sk_xprt);

	if (!status) {
		if (peerid != TLS_NO_PEERID)
			set_bit(XPT_PEER_AUTH, &xprt->xpt_flags);
		set_bit(XPT_TLS_SESSION, &xprt->xpt_flags);
	}
	clear_bit(XPT_HANDSHAKE, &xprt->xpt_flags);
	complete_all(&svsk->sk_handshake_done);
	svc_xprt_put(xprt);
}

/**
 * svc_tcp_handshake - Perform a transport-layer security handshake
 * @xprt: connected transport endpoint
 *
 */
static void svc_tcp_handshake(struct svc_xprt *xprt)
{
	struct svc_sock *svsk = container_of(xprt, struct svc_sock, sk_xprt);
	struct sock *sk = svsk->sk_sock->sk;
	struct tls_handshake_args args = {
		.ta_sock	= svsk->sk_sock,
		.ta_done	= svc_tcp_handshake_done,
		.ta_data	= xprt,
	};
	int ret;

	trace_svc_tls_upcall(xprt);

	clear_bit(XPT_TLS_SESSION, &xprt->xpt_flags);
	init_completion(&svsk->sk_handshake_done);

	/* Pin the transport across the asynchronous handshake callback. */
	svc_xprt_get(xprt);

	ret = tls_server_hello_x509(&args, GFP_KERNEL);
	if (ret) {
		trace_svc_tls_not_started(xprt);
		svc_xprt_put(xprt);
		goto out_failed;
	}

	ret = wait_for_completion_interruptible_timeout(&svsk->sk_handshake_done,
							SVC_HANDSHAKE_TO);
	if (ret <= 0) {
		if (tls_handshake_cancel(sk)) {
			trace_svc_tls_timed_out(xprt);
			svc_xprt_put(xprt);
			goto out_close;
		}
		/* Cancellation lost to handshake_complete(): the
		 * callback is in flight and should finish quickly.
		 */
		wait_for_completion(&svsk->sk_handshake_done);
	}

	if (!test_bit(XPT_TLS_SESSION, &xprt->xpt_flags)) {
		trace_svc_tls_unavailable(xprt);
		goto out_close;
	}

	/* Mark the transport ready in case the remote sent RPC
	 * traffic before the kernel received the handshake
	 * completion downcall.
	 */
	set_bit(XPT_DATA, &xprt->xpt_flags);
	svc_xprt_enqueue(xprt);
	return;

out_close:
	set_bit(XPT_CLOSE, &xprt->xpt_flags);
out_failed:
	clear_bit(XPT_HANDSHAKE, &xprt->xpt_flags);
	set_bit(XPT_DATA, &xprt->xpt_flags);
	svc_xprt_enqueue(xprt);
}

/*
 * See net/ipv6/ip_sockglue.c : ip_cmsg_recv_pktinfo
 */
static int svc_udp_get_dest_address4(struct svc_rqst *rqstp,
				     struct cmsghdr *cmh)
{
	struct in_pktinfo *pki = CMSG_DATA(cmh);
	struct sockaddr_in *daddr = svc_daddr_in(rqstp);

	if (cmh->cmsg_type != IP_PKTINFO)
		return 0;

	daddr->sin_family = AF_INET;
	daddr->sin_addr.s_addr = pki->ipi_spec_dst.s_addr;
	return 1;
}

/*
 * See net/ipv6/datagram.c : ip6_datagram_recv_ctl
 */
static int svc_udp_get_dest_address6(struct svc_rqst *rqstp,
				     struct cmsghdr *cmh)
{
	struct in6_pktinfo *pki = CMSG_DATA(cmh);
	struct sockaddr_in6 *daddr = svc_daddr_in6(rqstp);

	if (cmh->cmsg_type != IPV6_PKTINFO)
		return 0;

	daddr->sin6_family = AF_INET6;
	daddr->sin6_addr = pki->ipi6_addr;
	daddr->sin6_scope_id = pki->ipi6_ifindex;
	return 1;
}

/*
 * Copy the UDP datagram's destination address to the rqstp structure.
 * The 'destination' address in this case is the address to which the
 * peer sent the datagram, i.e. our local address. For multihomed
 * hosts, this can change from msg to msg. Note that only the IP
 * address changes, the port number should remain the same.
 */
static int svc_udp_get_dest_address(struct svc_rqst *rqstp,
				    struct cmsghdr *cmh)
{
	switch (cmh->cmsg_level) {
	case SOL_IP:
		return svc_udp_get_dest_address4(rqstp, cmh);
	case SOL_IPV6:
		return svc_udp_get_dest_address6(rqstp, cmh);
	}

	return 0;
}

/**
 * svc_udp_recvfrom - Receive a datagram from a UDP socket.
 * @rqstp: request structure into which to receive an RPC Call
 *
 * Called in a loop when XPT_DATA has been set.
 *
 * Returns:
 *   On success, the number of bytes in a received RPC Call, or
 *   %0 if a complete RPC Call message was not ready to return
 */
static int svc_udp_recvfrom(struct svc_rqst *rqstp)
{
	struct svc_sock	*svsk =
		container_of(rqstp->rq_xprt, struct svc_sock, sk_xprt);
	struct svc_serv	*serv = svsk->sk_xprt.xpt_server;
	struct sk_buff	*skb;
	union {
		struct cmsghdr	hdr;
		long		all[SVC_PKTINFO_SPACE / sizeof(long)];
	} buffer;
	struct cmsghdr *cmh = &buffer.hdr;
	struct msghdr msg = {
		.msg_name = svc_addr(rqstp),
		.msg_control = cmh,
		.msg_controllen = sizeof(buffer),
		.msg_flags = MSG_DONTWAIT,
	};
	size_t len;
	int err;

	if (test_and_clear_bit(XPT_CHNGBUF, &svsk->sk_xprt.xpt_flags))
	    /* udp sockets need large rcvbuf as all pending
	     * requests are still in that buffer.  sndbuf must
	     * also be large enough that there is enough space
	     * for one reply per thread.  We count all threads
	     * rather than threads in a particular pool, which
	     * provides an upper bound on the number of threads
	     * which will access the socket.
	     */
	    svc_sock_setbufsize(svsk, serv->sv_nrthreads + 3);

	clear_bit(XPT_DATA, &svsk->sk_xprt.xpt_flags);
	err = kernel_recvmsg(svsk->sk_sock, &msg, NULL,
			     0, 0, MSG_PEEK | MSG_DONTWAIT);
	if (err < 0)
		goto out_recv_err;
	skb = skb_recv_udp(svsk->sk_sk, MSG_DONTWAIT, &err);
	if (!skb)
		goto out_recv_err;

	len = svc_addr_len(svc_addr(rqstp));
	rqstp->rq_addrlen = len;
	if (skb->tstamp == 0) {
		skb->tstamp = ktime_get_real();
		/* Don't enable netstamp, sunrpc doesn't
		   need that much accuracy */
	}
	sock_write_timestamp(svsk->sk_sk, skb->tstamp);
	set_bit(XPT_DATA, &svsk->sk_xprt.xpt_flags); /* there may be more data... */

	len = skb->len;
	rqstp->rq_arg.len = len;
	trace_svcsock_udp_recv(&svsk->sk_xprt, len);

	rqstp->rq_prot = IPPROTO_UDP;

	if (!svc_udp_get_dest_address(rqstp, cmh))
		goto out_cmsg_err;
	rqstp->rq_daddrlen = svc_addr_len(svc_daddr(rqstp));

	if (skb_is_nonlinear(skb)) {
		/* we have to copy */
		local_bh_disable();
		if (csum_partial_copy_to_xdr(&rqstp->rq_arg, skb))
			goto out_bh_enable;
		local_bh_enable();
		consume_skb(skb);
	} else {
		/* we can use it in-place */
		rqstp->rq_arg.head[0].iov_base = skb->data;
		rqstp->rq_arg.head[0].iov_len = len;
		if (skb_checksum_complete(skb))
			goto out_free;
		rqstp->rq_xprt_ctxt = skb;
	}

	rqstp->rq_arg.page_base = 0;
	if (len <= rqstp->rq_arg.head[0].iov_len) {
		rqstp->rq_arg.head[0].iov_len = len;
		rqstp->rq_arg.page_len = 0;
	} else {
		rqstp->rq_arg.page_len = len - rqstp->rq_arg.head[0].iov_len;
	}

	if (serv->sv_stats)
		serv->sv_stats->netudpcnt++;

	svc_sock_secure_port(rqstp);
	svc_xprt_received(rqstp->rq_xprt);
	return len;

out_recv_err:
	if (err != -EAGAIN) {
		/* possibly an icmp error */
		set_bit(XPT_DATA, &svsk->sk_xprt.xpt_flags);
	}
	trace_svcsock_udp_recv_err(&svsk->sk_xprt, err);
	goto out_clear_busy;
out_cmsg_err:
	net_warn_ratelimited("svc: received unknown control message %d/%d; dropping RPC reply datagram\n",
			     cmh->cmsg_level, cmh->cmsg_type);
	goto out_free;
out_bh_enable:
	local_bh_enable();
out_free:
	kfree_skb(skb);
out_clear_busy:
	svc_xprt_received(rqstp->rq_xprt);
	return 0;
}

/**
 * svc_udp_sendto - Send out a reply on a UDP socket
 * @rqstp: completed svc_rqst
 *
 * xpt_mutex ensures @rqstp's whole message is written to the socket
 * without interruption.
 *
 * Returns the number of bytes sent, or a negative errno.
 */
static int svc_udp_sendto(struct svc_rqst *rqstp)
{
	struct svc_xprt *xprt = rqstp->rq_xprt;
	struct svc_sock	*svsk = container_of(xprt, struct svc_sock, sk_xprt);
	struct xdr_buf *xdr = &rqstp->rq_res;
	union {
		struct cmsghdr	hdr;
		long		all[SVC_PKTINFO_SPACE / sizeof(long)];
	} buffer;
	struct cmsghdr *cmh = &buffer.hdr;
	struct msghdr msg = {
		.msg_name	= &rqstp->rq_addr,
		.msg_namelen	= rqstp->rq_addrlen,
		.msg_control	= cmh,
		.msg_flags	= MSG_SPLICE_PAGES,
		.msg_controllen	= sizeof(buffer),
	};
	int count;
	int err;

	svc_udp_release_ctxt(xprt, rqstp->rq_xprt_ctxt);
	rqstp->rq_xprt_ctxt = NULL;

	svc_set_cmsg_data(rqstp, cmh);

	mutex_lock(&xprt->xpt_mutex);

	if (svc_xprt_is_dead(xprt))
		goto out_notconn;

	count = xdr_buf_to_bvec(svsk->sk_bvec, SUNRPC_MAX_UDP_SENDPAGES, xdr);
	if (count < 0) {
		err = count;
		goto out_trace;
	}

	iov_iter_bvec(&msg.msg_iter, ITER_SOURCE, svsk->sk_bvec,
		      count, rqstp->rq_res.len);
	err = sock_sendmsg(svsk->sk_sock, &msg);
	if (err == -ECONNREFUSED) {
		/* ICMP error on earlier request. */
		iov_iter_bvec(&msg.msg_iter, ITER_SOURCE, svsk->sk_bvec,
			      count, rqstp->rq_res.len);
		err = sock_sendmsg(svsk->sk_sock, &msg);
	}

out_trace:
	trace_svcsock_udp_send(xprt, err);

	mutex_unlock(&xprt->xpt_mutex);
	return err;

out_notconn:
	mutex_unlock(&xprt->xpt_mutex);
	return -ENOTCONN;
}

static int svc_udp_has_wspace(struct svc_xprt *xprt)
{
	struct svc_sock *svsk = container_of(xprt, struct svc_sock, sk_xprt);
	struct svc_serv	*serv = xprt->xpt_server;
	unsigned long required;

	/*
	 * Set the SOCK_NOSPACE flag before checking the available
	 * sock space.
	 */
	set_bit(SOCK_NOSPACE, &svsk->sk_sock->flags);
	required = atomic_read(&svsk->sk_xprt.xpt_reserved) + serv->sv_max_mesg;
	if (required*2 > sock_wspace(svsk->sk_sk))
		return 0;
	clear_bit(SOCK_NOSPACE, &svsk->sk_sock->flags);
	return 1;
}

static struct svc_xprt *svc_udp_accept(struct svc_xprt *xprt)
{
	BUG();
	return NULL;
}

static void svc_udp_kill_temp_xprt(struct svc_xprt *xprt)
{
}

static struct svc_xprt *svc_udp_create(struct svc_serv *serv,
				       struct net *net,
				       struct sockaddr *sa, int salen,
				       int flags)
{
	return svc_create_socket(serv, IPPROTO_UDP, net, sa, salen, flags);
}

static const struct svc_xprt_ops svc_udp_ops = {
	.xpo_create = svc_udp_create,
	.xpo_recvfrom = svc_udp_recvfrom,
	.xpo_sendto = svc_udp_sendto,
	.xpo_result_payload = svc_sock_result_payload,
	.xpo_release_ctxt = svc_udp_release_ctxt,
	.xpo_detach = svc_sock_detach,
	.xpo_free = svc_sock_free,
	.xpo_has_wspace = svc_udp_has_wspace,
	.xpo_accept = svc_udp_accept,
	.xpo_kill_temp_xprt = svc_udp_kill_temp_xprt,
};

static struct svc_xprt_class svc_udp_class = {
	.xcl_name = "udp",
	.xcl_owner = THIS_MODULE,
	.xcl_ops = &svc_udp_ops,
	.xcl_max_payload = RPCSVC_MAXPAYLOAD_UDP,
	.xcl_ident = XPRT_TRANSPORT_UDP,
	.xcl_flags = SVC_XPRT_FLAG_WSPACE_RESERVE,
};

static void svc_udp_init(struct svc_sock *svsk, struct svc_serv *serv)
{
	svc_xprt_init(sock_net(svsk->sk_sock->sk), &svc_udp_class,
		      &svsk->sk_xprt, serv);
	clear_bit(XPT_CACHE_AUTH, &svsk->sk_xprt.xpt_flags);
	svsk->sk_sk->sk_data_ready = svc_data_ready;
	svsk->sk_sk->sk_write_space = svc_write_space;

	/* initialise setting must have enough space to
	 * receive and respond to one request.
	 * svc_udp_recvfrom will re-adjust if necessary
	 */
	svc_sock_setbufsize(svsk, 3);

	/* data might have come in before data_ready set up */
	set_bit(XPT_DATA, &svsk->sk_xprt.xpt_flags);
	set_bit(XPT_CHNGBUF, &svsk->sk_xprt.xpt_flags);
	set_bit(XPT_RPCB_UNREG, &svsk->sk_xprt.xpt_flags);

	/* make sure we get destination address info */
	switch (svsk->sk_sk->sk_family) {
	case AF_INET:
		ip_sock_set_pktinfo(svsk->sk_sock->sk);
		break;
	case AF_INET6:
		ip6_sock_set_recvpktinfo(svsk->sk_sock->sk);
		break;
	default:
		BUG();
	}
}

/*
 * A data_ready event on a listening socket means there's a connection
 * pending. Do not use state_change as a substitute for it.
 */
static void svc_tcp_listen_data_ready(struct sock *sk)
{
	struct svc_sock	*svsk = (struct svc_sock *)sk->sk_user_data;

	trace_sk_data_ready(sk);

	/*
	 * This callback may called twice when a new connection
	 * is established as a child socket inherits everything
	 * from a parent LISTEN socket.
	 * 1) data_ready method of the parent socket will be called
	 *    when one of child sockets become ESTABLISHED.
	 * 2) data_ready method of the child socket may be called
	 *    when it receives data before the socket is accepted.
	 * In case of 2, we should ignore it silently and DO NOT
	 * dereference svsk.
	 */
	if (sk->sk_state != TCP_LISTEN)
		return;

	if (svsk) {
		/* Refer to svc_setup_socket() for details. */
		rmb();
		svsk->sk_odata(sk);
		set_bit(XPT_CONN, &svsk->sk_xprt.xpt_flags);
		svc_xprt_enqueue(&svsk->sk_xprt);
	}
}

/*
 * A state change on a connected socket means it's dying or dead.
 */
static void svc_tcp_state_change(struct sock *sk)
{
	struct svc_sock	*svsk = (struct svc_sock *)sk->sk_user_data;

	if (svsk) {
		/* Refer to svc_setup_socket() for details. */
		rmb();
		svsk->sk_ostate(sk);
		trace_svcsock_tcp_state(&svsk->sk_xprt, svsk->sk_sock);
		if (sk->sk_state != TCP_ESTABLISHED)
			svc_xprt_deferred_close(&svsk->sk_xprt);
	}
}

/*
 * Accept a TCP connection
 */
static struct svc_xprt *svc_tcp_accept(struct svc_xprt *xprt)
{
	struct svc_sock *svsk = container_of(xprt, struct svc_sock, sk_xprt);
	struct sockaddr_storage addr;
	struct sockaddr	*sin = (struct sockaddr *) &addr;
	struct svc_serv	*serv = svsk->sk_xprt.xpt_server;
	struct socket	*sock = svsk->sk_sock;
	struct socket	*newsock;
	struct svc_sock	*newsvsk;
	int		err, slen;

	if (!sock)
		return NULL;

	clear_bit(XPT_CONN, &svsk->sk_xprt.xpt_flags);
	/* Before a connection's first record: the region, sized for the threads. */
	rx_loan_region_carve();
	err = kernel_accept(sock, &newsock, O_NONBLOCK);
	if (err < 0) {
		if (err != -EAGAIN)
			trace_svcsock_accept_err(xprt, serv->sv_name, err);
		return NULL;
	}
	if (IS_ERR(sock_alloc_file(newsock, O_NONBLOCK, NULL)))
		return NULL;

	set_bit(XPT_CONN, &svsk->sk_xprt.xpt_flags);

	err = kernel_getpeername(newsock, sin);
	if (err < 0) {
		trace_svcsock_getpeername_err(xprt, serv->sv_name, err);
		goto failed;		/* aborted connection or whatever */
	}
	slen = err;

	/* Reset the inherited callbacks before calling svc_setup_socket */
	newsock->sk->sk_state_change = svsk->sk_ostate;
	newsock->sk->sk_data_ready = svsk->sk_odata;
	newsock->sk->sk_write_space = svsk->sk_owspace;

	/* make sure that a write doesn't block forever when
	 * low on memory
	 */
	newsock->sk->sk_sndtimeo = HZ*30;

	newsvsk = svc_setup_socket(serv, newsock,
				 (SVC_SOCK_ANONYMOUS | SVC_SOCK_TEMPORARY));
	if (IS_ERR(newsvsk))
		goto failed;
	svc_xprt_set_remote(&newsvsk->sk_xprt, sin, slen);
	err = kernel_getsockname(newsock, sin);
	slen = err;
	if (unlikely(err < 0))
		slen = offsetof(struct sockaddr, sa_data);
	svc_xprt_set_local(&newsvsk->sk_xprt, sin, slen);

	assign_bit(XPT_LOCAL, &newsvsk->sk_xprt.xpt_flags,
		   sock_is_loopback(newsock->sk));
	if (serv->sv_stats)
		serv->sv_stats->nettcpconn++;

	return &newsvsk->sk_xprt;

failed:
	sockfd_put(newsock);
	return NULL;
}

VISIBLE_IF_KUNIT int svc_tcp_restore_pages(struct svc_sock *svsk,
					   struct svc_rqst *rqstp)
{
	size_t len = svsk->sk_datalen;
	unsigned int i, npages, keep;
	int ret;

	if (!len) {
		if (WARN_ON_ONCE(!svc_tcp_rx_is_empty(svsk->sk_rx) ||
				 !svc_tcp_rx_is_empty(rqstp->rq_tcp_rx)))
			return -EINVAL;
		return 0;
	}
	npages = (len + PAGE_SIZE - 1) >> PAGE_SHIFT;
	/*
	 * Trade pages with the socket: the pages this thread gives up wait
	 * in sk_pages[] for the thread that next saves a partial record
	 * here, as many as svc_tcp_spare_bytes_max leaves room for.  The
	 * rest are freed.
	 */
	for (i = 0, keep = 0; i < npages; i++)
		if (rqstp->rq_pages[i])
			keep++;
	keep = svc_tcp_spare_reserve(keep);
	for (i = 0; i < npages; i++) {
		struct page *page = rqstp->rq_pages[i];

		BUG_ON(svsk->sk_pages[i] == NULL);
		rqstp->rq_pages[i] = svsk->sk_pages[i];
		svsk->sk_pages[i] = NULL;
		if (!page)
			continue;
		if (keep) {
			svsk->sk_pages[i] = page;
			svsk->sk_spare_pages++;
			keep--;
		} else {
			svc_rqst_page_release(rqstp, page);
		}
	}
	ret = svc_tcp_rx_exchange(&svsk->sk_rx, &rqstp->rq_tcp_rx);
	if (ret)
		return ret;
	if (WARN_ON_ONCE(rqstp->rq_tcp_rx->lt.body_bytes != len))
		return -EINVAL;
	rqstp->rq_arg.head[0].iov_base = page_address(rqstp->rq_pages[0]);
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_restore_pages);

VISIBLE_IF_KUNIT int svc_tcp_save_pages(struct svc_sock *svsk,
					struct svc_rqst *rqstp)
{
	unsigned int i, len, npages, taken = 0;
	int ret;

	if (svsk->sk_datalen == 0)
		return 0;
	len = svsk->sk_datalen;
	if (WARN_ON_ONCE(rqstp->rq_tcp_rx->lt.body_bytes != len))
		return -EINVAL;
	npages = (len + PAGE_SIZE - 1) >> PAGE_SHIFT;
	/*
	 * Take the pages an earlier restore left here in exchange.
	 * svc_alloc_arg() fills whichever of these entries is still NULL.
	 */
	for (i = 0; i < npages; i++) {
		if (svsk->sk_pages[i])
			taken++;
		swap(svsk->sk_pages[i], rqstp->rq_pages[i]);
	}
	svc_tcp_spare_release(svsk, taken);
	rqstp->rq_pages_nfree = npages;
	ret = svc_tcp_rx_exchange(&rqstp->rq_tcp_rx, &svsk->sk_rx);
	return ret;
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_save_pages);

VISIBLE_IF_KUNIT void svc_tcp_clear_pages(struct svc_sock *svsk)
{
	unsigned int i, len, npages;

	if (svc_tcp_rx_abort_active(svsk, NULL, SVC_TCP_RX_ABORT,
				    SVC_TCP_RX_REASON_NONE,
				    svsk->sk_datalen == 0))
		return;

	/* A partial record's pages, and the pages left by a restore. */
	svc_tcp_spare_release(svsk, svsk->sk_spare_pages);
	len = svsk->sk_datalen;
	npages = (len + PAGE_SIZE - 1) >> PAGE_SHIFT;
	for (i = 0; i < svsk->sk_maxpages; i++) {
		if (svsk->sk_pages[i] == NULL) {
			WARN_ON_ONCE(i < npages);
			continue;
		}
		put_page(svsk->sk_pages[i]);
		svsk->sk_pages[i] = NULL;
	}
	svsk->sk_tcplen = 0;
	svsk->sk_datalen = 0;
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_clear_pages);

static int receive_cb_reply(struct svc_sock *svsk, struct svc_rqst *rqstp)
{
	struct rpc_xprt *bc_xprt = svsk->sk_xprt.xpt_bc_xprt;
	struct rpc_rqst *req = NULL;
	struct kvec *src, *dst;
	__be32 *p = (__be32 *)rqstp->rq_arg.head[0].iov_base;
	__be32 xid = *p;

	if (!bc_xprt)
		return -EAGAIN;
	spin_lock(&bc_xprt->queue_lock);
	req = xprt_lookup_rqst(bc_xprt, xid);
	if (!req)
		goto unlock_eagain;

	memcpy(&req->rq_private_buf, &req->rq_rcv_buf, sizeof(struct xdr_buf));
	/*
	 * XXX!: cheating for now!  Only copying HEAD.
	 * But we know this is good enough for now (in fact, for any
	 * callback reply in the forseeable future).
	 */
	dst = &req->rq_private_buf.head[0];
	src = &rqstp->rq_arg.head[0];
	if (dst->iov_len < src->iov_len)
		goto unlock_eagain; /* whatever; just giving up. */
	memcpy(dst->iov_base, src->iov_base, src->iov_len);
	xprt_complete_rqst(req->rq_task, rqstp->rq_arg.len);
	rqstp->rq_arg.len = 0;
	spin_unlock(&bc_xprt->queue_lock);
	return 0;
unlock_eagain:
	spin_unlock(&bc_xprt->queue_lock);
	return -EAGAIN;
}

static void svc_tcp_fragment_received(struct svc_sock *svsk)
{
	svsk->sk_tcplen = 0;
	svsk->sk_marker = xdr_zero;
}

VISIBLE_IF_KUNIT void svc_tcp_recv_record_done(struct svc_sock *svsk)
{
	svsk->sk_datalen = 0;
	svc_tcp_fragment_received(svsk);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_recv_record_done);

/*
 * A non-final fragment carries four octets of marker and may carry
 * no payload at all. sk_datalen advances only by the payload, so a
 * run of tiny fragments exhausts ->read_sock's byte budget before
 * the sv_max_mesg check trips, and a run of empty ones never trips
 * it. Cap the fragments per socket-lock hold. The cap leaves the
 * record incomplete, and svc_tcp_recvfrom() resumes it on the next
 * call.
 */
#define SVC_TCP_MAX_FRAGS		256

struct svc_tcp_recv_ctx {
	struct svc_rqst		*rqstp;
	unsigned int		frags;
	bool			complete;
	/* stopped for region credit: its wake-up re-arms XPT_DATA */
	bool			credit_wait;
};

/*
 * Nothing reads the message body before the message is complete, and
 * partial receives refill the same pages. Flush once here, after the
 * socket lock is released, rather than once per copy in the actor.
 */
static void svc_tcp_flush_pages(struct svc_sock *svsk,
				struct svc_rqst *rqstp)
{
	struct svc_tcp_rx_state *state = rqstp->rq_tcp_rx;
	unsigned int pg, pages = DIV_ROUND_UP(svsk->sk_datalen, PAGE_SIZE);

	if (state->mode == SVC_TCP_RX_MIXED) {
		struct page *last = rqstp->rq_pages[0];

		flush_dcache_page(last);
		for (pg = 0; pg < state->lt.count; pg++) {
			struct page *page;

			if (test_bit(pg, state->lt.borrowed))
				continue;
			page = state->lt.bvec[pg].bv_page;
			if (page != last) {
				flush_dcache_page(page);
				last = page;
			}
		}
		return;
	}
	for (pg = 0; pg < pages; pg++)
		flush_dcache_page(rqstp->rq_pages[pg]);
}

/*
 * Mapping the message's unfilled remainder would re-map untouched
 * pages on every call, at a cost that grows with the message rather
 * than with the octets copied.
 */
static void svc_tcp_recv_iter_init(struct svc_rqst *rqstp,
				   struct iov_iter *iter, size_t body_off,
				   size_t len)
{
	unsigned int first = body_off >> PAGE_SHIFT;
	size_t seek = offset_in_page(body_off);
	unsigned int i, pages = DIV_ROUND_UP(seek + len, PAGE_SIZE);

	for (i = 0; i < pages; i++)
		bvec_set_page(&rqstp->rq_bvec[i], rqstp->rq_pages[first + i],
			      PAGE_SIZE, 0);

	iov_iter_bvec(iter, ITER_DEST, rqstp->rq_bvec, pages, seek + len);
	iov_iter_advance(iter, seek);
}

#define SVC_TCP_RX_FIXED_BYTES	(7 * XDR_UNIT)

static int svc_tcp_rx_start(struct svc_sock *svsk, struct svc_rqst *rqstp)
{
	struct svc_tcp_rx_state *state = rqstp->rq_tcp_rx;

	if (WARN_ON_ONCE(!state || !svc_tcp_rx_is_empty(state) ||
			 !svc_tcp_rx_is_empty(svsk->sk_rx)))
		return -EINVAL;
	if (WARN_ON_ONCE(++svsk->sk_rx_sequence == 0))
		return -EOVERFLOW;
	state->record_seq = svsk->sk_rx_sequence;
	state->mode = SVC_TCP_RX_PREFIX;
	return 0;
}

static unsigned int
svc_tcp_rx_copied_entries(const struct svc_rqst *rqstp,
			  const struct svc_tcp_rx_state *state,
			  u32 body_offset, unsigned int len)
{
	unsigned int entries = DIV_ROUND_UP(offset_in_page(body_offset) + len,
						PAGE_SIZE);

	if (state->lt.count) {
		const struct bio_vec *last =
			&state->lt.bvec[state->lt.count - 1];
		struct page *page = rqstp->rq_pages[body_offset >> PAGE_SHIFT];

		if (!test_bit(state->lt.count - 1, state->lt.borrowed) &&
		    last->bv_page == page &&
		    last->bv_offset + last->bv_len == offset_in_page(body_offset))
			entries--;
	}
	return entries;
}

static int svc_tcp_rx_append_arena(struct svc_rqst *rqstp,
				   struct svc_tcp_rx_state *state,
				   u32 body_offset, unsigned int len)
{
	while (len) {
		unsigned int page_index = body_offset >> PAGE_SHIFT;
		unsigned int offset = offset_in_page(body_offset);
		unsigned int take = min_t(unsigned int, len, PAGE_SIZE - offset);
		int ret;

		if (WARN_ON_ONCE(page_index >= state->arena_pages ||
				 !rqstp->rq_pages[page_index]))
			return -EINVAL;
		ret = svc_tcp_rx_append_copied(state, rqstp->rq_pages[page_index],
					       offset, take);
		if (ret)
			return ret;
		body_offset += take;
		len -= take;
	}
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_append_copied);

static int svc_tcp_rx_copy_skb(struct svc_sock *svsk, struct svc_rqst *rqstp,
			       struct sk_buff *skb, unsigned int offset,
			       unsigned int len)
{
	struct svc_tcp_rx_state *state = rqstp->rq_tcp_rx;
	u32 body_offset = state->lt.body_bytes;
	struct iov_iter iter;
	u32 total;
	int ret;

	if (WARN_ON_ONCE(!len || state->mode == SVC_TCP_RX_EMPTY ||
			 check_add_overflow(body_offset, len, &total)))
		return -EINVAL;
	if (state->mode == SVC_TCP_RX_MIXED &&
	    svc_tcp_rx_copied_entries(rqstp, state, body_offset, len) >
			state->lt.capacity - state->lt.count) {
		state->reason = SVC_TCP_RX_REASON_CAPACITY;
		ret = svc_tcp_rx_materialize(state, rqstp->rq_pages);
		if (ret)
			return ret;
		trace_svcsock_tcp_rx_lifetime(&svsk->sk_xprt, rqstp, state,
					       SVC_TCP_RX_MATERIALIZE);
	}

	svc_tcp_recv_iter_init(rqstp, &iter, body_offset, len);
	if (skb_copy_datagram_iter(skb, offset, &iter, len)) {
		state->reason = skb_frags_readable(skb) ?
			SVC_TCP_RX_REASON_COPY_FAULT : SVC_TCP_RX_REASON_UNREADABLE;
		return -EFAULT;
	}

	switch (state->mode) {
	case SVC_TCP_RX_PREFIX:
		state->lt.body_bytes = total;
		state->fixed_bytes += len;
		break;
	case SVC_TCP_RX_MIXED:
		ret = svc_tcp_rx_append_arena(rqstp, state, body_offset, len);
		if (ret)
			return ret;
		break;
	case SVC_TCP_RX_COPY:
		state->lt.body_bytes = total;
		state->lt.copied_bytes += len;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static enum svc_tcp_rx_reason
svc_tcp_rx_classify(struct svc_sock *svsk, struct svc_rqst *rqstp)
{
	const struct svc_version *versp;
	const struct svc_procedure *procp;
	const struct svc_program *progp = NULL;
	struct svc_tcp_rx_state *state = rqstp->rq_tcp_rx;
	const __be32 *words = page_address(rqstp->rq_pages[0]);
	struct svc_serv *serv = rqstp->rq_server;
	u32 program, version, procedure, auth;
	__be32 status;
	unsigned int index;

	if (WARN_ON_ONCE(state->mode != SVC_TCP_RX_PREFIX ||
			 state->fixed_bytes != SVC_TCP_RX_FIXED_BYTES ||
			 state->lt.body_bytes != SVC_TCP_RX_FIXED_BYTES))
		return SVC_TCP_RX_REASON_INVARIANT;
	state->xid = be32_to_cpu(words[0]);
	if (svc_tcp_rx_budget_none(svsk))
		return SVC_TCP_RX_REASON_NO_BUDGET;
	if (test_bit(XPT_TLS_SESSION, &svsk->sk_xprt.xpt_flags))
		return SVC_TCP_RX_REASON_TLS;
	if (svc_tcp_rx_small_record(svsk))
		return SVC_TCP_RX_REASON_SMALL;
	if (words[1] != rpc_call)
		return SVC_TCP_RX_REASON_DIRECTION;
	if (words[2] != cpu_to_be32(RPC_VERSION))
		return SVC_TCP_RX_REASON_RPC_VERSION;

	program = be32_to_cpu(words[3]);
	version = be32_to_cpu(words[4]);
	procedure = be32_to_cpu(words[5]);
	auth = be32_to_cpu(words[6]);
	for (index = 0; index < serv->sv_nprogs; index++)
		if (serv->sv_programs[index].pg_prog == program)
			progp = &serv->sv_programs[index];
	if (!progp)
		return SVC_TCP_RX_REASON_PROGRAM;
	status = svc_proc_lookup(progp, version, procedure, &versp, &procp);
	if (status == rpc_prog_mismatch)
		return SVC_TCP_RX_REASON_VERSION;
	if (status != rpc_success)
		return SVC_TCP_RX_REASON_PROCEDURE;
	if (!svc_proc_accepts_xdr_bvec(procp, auth))
		return SVC_TCP_RX_REASON_AUTH;
	return SVC_TCP_RX_REASON_NONE;
}

static int svc_tcp_rx_finish_classify(struct svc_sock *svsk,
				      struct svc_rqst *rqstp)
{
	struct svc_tcp_rx_state *state = rqstp->rq_tcp_rx;
	enum svc_tcp_rx_reason reason = svc_tcp_rx_classify(svsk, rqstp);

	state->reason = reason;
	state->mode = reason == SVC_TCP_RX_REASON_NONE ?
		SVC_TCP_RX_MIXED : SVC_TCP_RX_COPY;
	trace_svcsock_tcp_rx_lifetime(&svsk->sk_xprt, rqstp, state,
				       SVC_TCP_RX_CLASSIFY);
	return reason == SVC_TCP_RX_REASON_INVARIANT ? -EINVAL : 0;
}

struct svc_tcp_rx_walk {
	struct svc_sock		*svsk;
	struct svc_rqst		*rqstp;
	struct sk_buff		*root;
	u32			first;
	u32			last;
	u32			body_base;
	bool			pp_recycle;	/* current skb holds page-pool refs */
};

static int svc_tcp_rx_materialize_fallback(struct svc_tcp_rx_walk *walk,
					   enum svc_tcp_rx_reason reason)
{
	struct svc_tcp_rx_state *state = walk->rqstp->rq_tcp_rx;
	int ret;

	if (state->mode == SVC_TCP_RX_COPY)
		return 0;
	if (WARN_ON_ONCE(state->mode != SVC_TCP_RX_MIXED))
		return -EINVAL;
	state->reason = reason;
	ret = svc_tcp_rx_materialize(state, walk->rqstp->rq_pages);
	if (ret)
		return ret;
	trace_svcsock_tcp_rx_lifetime(&walk->svsk->sk_xprt, walk->rqstp,
				       state, SVC_TCP_RX_MATERIALIZE);
	return 0;
}

static int svc_tcp_rx_copy_segment(struct svc_tcp_rx_walk *walk,
				   u32 start, unsigned int len,
				   enum svc_tcp_rx_reason reason)
{
	struct svc_tcp_rx_state *state = walk->rqstp->rq_tcp_rx;
	u32 expected = walk->body_base + start - walk->first;

	if (WARN_ON_ONCE(state->lt.body_bytes != expected))
		return -EINVAL;
	if (state->mode == SVC_TCP_RX_MIXED &&
	    state->reason == SVC_TCP_RX_REASON_NONE)
		state->reason = reason;
	return svc_tcp_rx_copy_skb(walk->svsk, walk->rqstp, walk->root,
				   start, len);
}

static int svc_tcp_rx_borrow_segment(struct svc_tcp_rx_walk *walk,
				     u32 start, struct page *page,
				     unsigned int offset, unsigned int len)
{
	struct svc_tcp_rx_state *state = walk->rqstp->rq_tcp_rx;
	unsigned int consumed = 0;
	int ret;

	while (consumed < len) {
		unsigned int page_offset = offset_in_page(offset + consumed);
		unsigned int take = min_t(unsigned int, len - consumed,
					       PAGE_SIZE - page_offset);
		struct page *span_page = page + ((offset + consumed) >> PAGE_SHIFT);
		u32 expected = walk->body_base + start + consumed - walk->first;
		bool page_pool = walk->pp_recycle &&
			page_pool_page_is_pp(compound_head(span_page));

		if (WARN_ON_ONCE(state->lt.body_bytes != expected))
			return -EINVAL;
		if (state->mode == SVC_TCP_RX_COPY)
			return svc_tcp_rx_copy_segment(walk, start + consumed,
						       len - consumed,
						       SVC_TCP_RX_REASON_CAPACITY);
		ret = svc_tcp_rx_append_borrowed(state, span_page, page_offset,
						 take, page_pool);
		if (ret == -ENOSPC) {
			ret = svc_tcp_rx_materialize_fallback(walk,
						  SVC_TCP_RX_REASON_CAPACITY);
			if (ret)
				return ret;
			return svc_tcp_rx_copy_segment(walk, start + consumed,
						       len - consumed,
						       SVC_TCP_RX_REASON_CAPACITY);
		}
		if (ret)
			return ret;
		consumed += take;
	}
	return 0;
}

static int svc_tcp_rx_walk_skb(struct svc_tcp_rx_walk *walk,
			       struct sk_buff *skb, u32 base)
{
	struct svc_tcp_rx_state *state = walk->rqstp->rq_tcp_rx;
	u32 skb_end = base + skb->len;
	u32 cursor = base;
	u32 start, end;
	struct sk_buff *frag_skb;
	unsigned int index;

	start = max(walk->first, base);
	end = min(walk->last, skb_end);
	if (start >= end)
		return 0;
	walk->pp_recycle = skb->pp_recycle;
	if (state->mode == SVC_TCP_RX_COPY)
		return svc_tcp_rx_copy_segment(walk, start, end - start,
					       state->reason);
	if (skb_zcopy(skb) || skb_zcopy_pure(skb) ||
	    skb_zcopy_managed(skb)) {
		int ret = svc_tcp_rx_materialize_fallback(walk,
							SVC_TCP_RX_REASON_MANAGED_FRAGS);

		if (ret)
			return ret;
		return svc_tcp_rx_copy_segment(walk, start, end - start,
					       SVC_TCP_RX_REASON_MANAGED_FRAGS);
	}

	if (skb_headlen(skb)) {
		u32 head_end = cursor + skb_headlen(skb);
		u32 lo = max(walk->first, cursor);
		u32 hi = min(walk->last, head_end);

		if (lo < hi) {
			int ret;

			if (skb_head_is_locked(skb)) {
				/*
				 * A locked head's bytes are copied into the
				 * arena as their own bvec, beside the loans
				 * around them.  Whether that geometry can go
				 * to the device in place is the queue's call
				 * (nfsd splits on its real segment boundary),
				 * not a reason to copy the neighbouring loans.
				 */
				ret = svc_tcp_rx_copy_segment(walk, lo, hi - lo,
					SVC_TCP_RX_REASON_LOCKED_HEAD);
			} else {
				unsigned char *data = skb->data + lo - cursor;
				unsigned int remaining = hi - lo;
				u32 position = lo;

				ret = 0;
				while (remaining) {
					struct page *page = virt_to_page(data);
					unsigned int offset = offset_in_page(data);
					unsigned int take = min_t(unsigned int, remaining,
								  PAGE_SIZE - offset);

					ret = svc_tcp_rx_borrow_segment(walk, position,
								page, offset,
								take);
					if (ret)
						break;
					data += take;
					position += take;
					remaining -= take;
				}
			}
			if (ret)
				return ret;
		}
		cursor = head_end;
	}

	if (cursor < walk->last && !skb_frags_readable(skb)) {
		state->reason = SVC_TCP_RX_REASON_UNREADABLE;
		return -EFAULT;
	}
	for (index = 0; index < skb_shinfo(skb)->nr_frags; index++) {
		const skb_frag_t *frag = &skb_shinfo(skb)->frags[index];
		u32 frag_end = cursor + skb_frag_size(frag);
		u32 lo = max(walk->first, cursor);
		u32 hi = min(walk->last, frag_end);

		if (lo < hi) {
			struct page *page;
			u32 page_offset, page_len, copied;

			if (skb_frag_is_net_iov(frag)) {
				state->reason = SVC_TCP_RX_REASON_NET_IOV;
				return -EREMOTEIO;
			}
			page = skb_frag_page(frag);
			if (!page) {
				state->reason = SVC_TCP_RX_REASON_NULL_PAGE;
				return -EFAULT;
			}
			skb_frag_foreach_page(frag,
					      skb_frag_off(frag) + lo - cursor,
					      hi - lo, page, page_offset,
					      page_len, copied) {
				int ret = svc_tcp_rx_borrow_segment(walk, lo + copied,
								page, page_offset,
								page_len);

				if (ret)
					return ret;
			}
		}
		cursor = frag_end;
	}

	skb_walk_frags(skb, frag_skb) {
		int ret = svc_tcp_rx_walk_skb(walk, frag_skb, cursor);

		if (ret)
			return ret;
		cursor += frag_skb->len;
	}
	if (WARN_ON_ONCE(cursor != skb_end))
		return -EINVAL;
	return 0;
}

static int svc_tcp_rx_walk(struct svc_sock *svsk, struct svc_rqst *rqstp,
			   struct sk_buff *skb, unsigned int offset,
			   unsigned int len)
{
	struct svc_tcp_rx_walk walk = {
		.svsk = svsk,
		.rqstp = rqstp,
		.root = skb,
		.first = offset,
		.last = offset + len,
		.body_base = rqstp->rq_tcp_rx->lt.body_bytes,
	};
	int ret;

	if (WARN_ON_ONCE(walk.last < walk.first || walk.last > skb->len))
		return -EINVAL;
	ret = svc_tcp_rx_walk_skb(&walk, skb, 0);
	if (ret)
		return ret;
	if (WARN_ON_ONCE(rqstp->rq_tcp_rx->lt.body_bytes !=
			 walk.body_base + len))
		return -EINVAL;
	return 0;
}

static int svc_tcp_rx_validate_mixed(struct svc_sock *svsk,
				     struct svc_rqst *rqstp)
{
	struct svc_tcp_rx_state *state = rqstp->rq_tcp_rx;
	u32 bytes = 0;
	unsigned int index;

	if (state->mode != SVC_TCP_RX_MIXED ||
	    state->terminal ||
	    state->fixed_bytes != SVC_TCP_RX_FIXED_BYTES ||
	    state->lt.body_bytes != svsk->sk_datalen ||
	    state->lt.borrowed_bytes + state->lt.copied_bytes !=
		state->lt.body_bytes - state->fixed_bytes ||
	    state->lt.refs_acquired - state->lt.refs_released !=
		state->lt.borrowed_count ||
	    bitmap_weight(state->lt.borrowed, state->lt.capacity) !=
		state->lt.borrowed_count ||
	    !bitmap_subset(state->lt.page_pool, state->lt.borrowed,
			   state->lt.capacity) ||
	    state->lt.page_pool_bytes > state->lt.borrowed_bytes ||
	    rqstp->rq_xprt_ctxt)
		goto invalid;
	for (index = 0; index < state->lt.count; index++) {
		const struct bio_vec *bvec = &state->lt.bvec[index];

		if (!bvec->bv_page || !bvec->bv_len ||
		    bvec->bv_offset >= PAGE_SIZE ||
		    bvec->bv_len > PAGE_SIZE - bvec->bv_offset ||
		    check_add_overflow(bytes, bvec->bv_len, &bytes))
			goto invalid;
	}
	if (bytes != state->lt.body_bytes - state->fixed_bytes)
		goto invalid;
	return 0;

invalid:
	WARN_ON_ONCE(1);
	state->reason = SVC_TCP_RX_REASON_INVARIANT;
	return -EINVAL;
}

VISIBLE_IF_KUNIT int svc_tcp_rx_publish(struct svc_sock *svsk,
					struct svc_rqst *rqstp)
{
	struct svc_tcp_rx_state *state = rqstp->rq_tcp_rx;
	struct xdr_buf *arg = &rqstp->rq_arg;

	if (state->mode == SVC_TCP_RX_PREFIX) {
		state->reason = SVC_TCP_RX_REASON_SHORT_PREFIX;
		state->mode = SVC_TCP_RX_COPY;
		trace_svcsock_tcp_rx_lifetime(&svsk->sk_xprt, rqstp, state,
					       SVC_TCP_RX_CLASSIFY);
	}
	arg->len = svsk->sk_datalen;
	arg->page_base = 0;
	if (state->mode == SVC_TCP_RX_MIXED &&
	    state->lt.body_bytes == SVC_TCP_RX_FIXED_BYTES) {
		/* No bvec or page reference exists for a prefix-only record. */
		if (svc_tcp_rx_validate_mixed(svsk, rqstp))
			return -EINVAL;
		state->mode = SVC_TCP_RX_COPY;
	}
	if (state->mode == SVC_TCP_RX_COPY) {
		if (WARN_ON_ONCE(state->lt.body_bytes != svsk->sk_datalen ||
				 state->lt.borrowed_count ||
				 state->lt.refs_acquired !=
				 state->lt.refs_released))
			return -EINVAL;
		arg->page_mode = XDRBUF_PAGE_ARRAY;
		arg->bvec = NULL;
		arg->bvec_count = 0;
		arg->bvec_offset = 0;
		if (arg->len <= arg->head[0].iov_len) {
			arg->head[0].iov_len = arg->len;
			arg->page_len = 0;
		} else {
			arg->page_len = arg->len - arg->head[0].iov_len;
		}
		if (svc_tcp_rx_release_refs(state))
			return -EINVAL;
		trace_svcsock_tcp_rx_lifetime(&svsk->sk_xprt, rqstp, state,
					       SVC_TCP_RX_PUBLISH);
		svc_tcp_rx_reset(state);
		rqstp->rq_xprt_ctxt = NULL;
		return 0;
	}
	if (svc_tcp_rx_validate_mixed(svsk, rqstp))
		return -EINVAL;

	arg->head[0].iov_len = SVC_TCP_RX_FIXED_BYTES;
	arg->page_mode = XDRBUF_PAGE_BVECS;
	arg->bvec = state->lt.bvec;
	arg->bvec_count = state->lt.count;
	arg->bvec_offset = 0;
	arg->page_len = state->lt.body_bytes - SVC_TCP_RX_FIXED_BYTES;
	state->rqstp = rqstp;
	state->mode = SVC_TCP_RX_PUBLISHED;
	rqstp->rq_xprt_ctxt = state;
	trace_svcsock_tcp_rx_lifetime(&svsk->sk_xprt, rqstp, state,
				       SVC_TCP_RX_PUBLISH);
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_publish);

/*
 * ->read_sock actor, called under the socket lock. sk_datalen is both
 * the count of body octets received so far and their write offset into
 * rq_pages.
 */
static int svc_tcp_recv_skb(read_descriptor_t *desc, struct sk_buff *skb,
			    unsigned int offset, size_t len)
{
	struct svc_tcp_recv_ctx *ctx = desc->arg.data;
	struct svc_rqst *rqstp = ctx->rqstp;
	struct svc_sock *svsk =
		container_of(rqstp->rq_xprt, struct svc_sock, sk_xprt);
	size_t reclen, received, want, take, n;
	size_t consumed = 0;
	bool paused = false;
	int error = -EFAULT;

	/*
	 * A transport holds its receive buffer to svc_tcp_rx_rcvbuf once it
	 * receives region pages; svc_tcp_recvfrom() applies it after the
	 * socket lock is released.
	 */
	if (!svsk->sk_rx_held && !svsk->sk_rx_hold_pending &&
	    svc_tcp_rx_rcvbuf_effective() &&
	    svc_tcp_rx_skb_region_pages(skb, 0, offset, offset + len))
		svsk->sk_rx_hold_pending = true;
	else if (svsk->sk_rx_held && svc_tcp_rx_hold_stale(svsk)) {
		svsk->sk_rx_held = false;
		svsk->sk_rx_hold_pending = true;
	}

	if (!desc->count)
		return 0;

	len = min(len, desc->count);

	if (svsk->sk_tcplen < sizeof(rpc_fraghdr)) {
		want = sizeof(rpc_fraghdr) - svsk->sk_tcplen;
		n = min(want, len);

		if (skb_copy_bits(skb, offset,
				  (char *)&svsk->sk_marker + svsk->sk_tcplen,
				  n))
			goto fault;
		svsk->sk_tcplen += n;
		offset += n;
		len -= n;
		consumed += n;
		desc->count -= n;

		if (svsk->sk_tcplen < sizeof(rpc_fraghdr))
			return consumed;

		trace_svcsock_marker(&svsk->sk_xprt, svsk->sk_marker);
		if (svc_sock_reclen(svsk) + svsk->sk_datalen >
		    svsk->sk_xprt.xpt_server->sv_max_mesg) {
			net_notice_ratelimited("svc: %s oversized RPC fragment (%u octets) from %pISpc\n",
					       svsk->sk_xprt.xpt_server->sv_name,
					       svc_sock_reclen(svsk),
					       (struct sockaddr *)&svsk->sk_xprt.xpt_remote);
			desc->error = -EMSGSIZE;
			desc->count = 0;
			return consumed;
		}
	}

	reclen = svc_sock_reclen(svsk);
	received = svsk->sk_tcplen - sizeof(rpc_fraghdr);
	want = reclen - received;
	take = min(want, len);

	if (take) {
		unsigned int body_offset = offset;
		unsigned int remaining = take;
		struct svc_tcp_rx_state *state = rqstp->rq_tcp_rx;

		if (state->mode == SVC_TCP_RX_EMPTY) {
			error = svc_tcp_rx_start(svsk, rqstp);
			if (error)
				goto fault;
		}
		if (state->lt.body_bytes < SVC_TCP_RX_FIXED_BYTES) {
			unsigned int prefix = min_t(unsigned int, remaining,
					SVC_TCP_RX_FIXED_BYTES -
					state->lt.body_bytes);

			error = svc_tcp_rx_copy_skb(svsk, rqstp, skb, body_offset,
						    prefix);
			if (error)
				goto fault;
			body_offset += prefix;
			remaining -= prefix;
			if (state->fixed_bytes == SVC_TCP_RX_FIXED_BYTES) {
				error = svc_tcp_rx_finish_classify(svsk, rqstp);
				if (error)
					goto fault;
			}
		}
		/*
		 * Only region pages draw on region credit: a loaned record is
		 * admitted at its first skb that carries one, and charged for
		 * each later one.  Refused, it pauses at this skb.
		 */
		if (remaining && state->mode == SVC_TCP_RX_MIXED) {
			struct page *prev = state->lt.region_last;
			struct svc_tcp_rx_budget *c = NULL;
			u32 need;

			rx_loan_prefetch_pages(skb, 0, body_offset,
					       body_offset + remaining);
			need = rx_loan_skb_region_pages(skb, 0, body_offset,
							body_offset + remaining,
							&prev);

			/* One lookup for the copy check and the admission. */
			if (need && !state->cr.linked)
				c = svc_tcp_rx_budget_for(svsk);
			if (c && svc_tcp_rx_budget_copy_from(svsk, c)) {
				/* No budget to draw on: copy, do not loan. */
				state->reason = SVC_TCP_RX_REASON_NO_BUDGET;
				error = svc_tcp_rx_materialize(state,
							       rqstp->rq_pages);
				if (error)
					goto fault;
				trace_svcsock_tcp_rx_lifetime(&svsk->sk_xprt,
							       rqstp, state,
							       SVC_TCP_RX_MATERIALIZE);
			} else if (need &&
			    ((c && !svc_tcp_rx_credit_admit_to(svsk, c, state,
							       reclen)) ||
			     !svc_tcp_rx_credit_charge(svsk, state, need))) {
				/* Paused at this skb: keep only the prefix taken. */
				take -= remaining;
				remaining = 0;
				paused = true;
			}
		}
		/*
		 * A transport that waited and now reads on without being
		 * admitted (its queue was collapsed out of the region while
		 * it waited, so this skb draws no credit) no longer waits.
		 * Left on the list it would stay the oldest waiter, never
		 * run again, and strand every transport behind it.
		 */
		if (!paused && svc_tcp_rx_credit_waiting(svsk))
			svc_tcp_rx_credit_forget(svsk);
		if (remaining) {
			if (state->mode == SVC_TCP_RX_COPY)
				error = svc_tcp_rx_copy_skb(svsk, rqstp, skb,
							    body_offset, remaining);
			else
				error = svc_tcp_rx_walk(svsk, rqstp, skb,
							body_offset, remaining);
			if (error)
				goto fault;
		}
		if (WARN_ON_ONCE(state->lt.body_bytes !=
				 svsk->sk_datalen + take)) {
			error = -EINVAL;
			goto fault;
		}
		svsk->sk_datalen += take;
		svsk->sk_tcplen += take;
		consumed += take;
		desc->count -= take;
	}

	if (paused) {
		ctx->credit_wait = true;
		desc->count = 0;
		return consumed;
	}
	if (take == want) {
		if (svc_sock_final_rec(svsk)) {
			ctx->complete = true;
			desc->count = 0;
		} else {
			svc_tcp_fragment_received(svsk);
			if (++ctx->frags >= SVC_TCP_MAX_FRAGS)
				desc->count = 0;
		}
	}

	return consumed;

fault:
	desc->error = error;
	desc->count = 0;
	return consumed;
}

/*
 * A transport whose receive buffer is held measures what it is held to
 * from the skbs it reads: see svc_tcp_rx_geometry_account().
 */
static int svc_tcp_recv_actor(read_descriptor_t *desc, struct sk_buff *skb,
			      unsigned int offset, size_t len)
{
	struct svc_tcp_recv_ctx *ctx = desc->arg.data;
	struct svc_sock *svsk =
		container_of(ctx->rqstp->rq_xprt, struct svc_sock, sk_xprt);
	int consumed = svc_tcp_recv_skb(desc, skb, offset, len);

	if (consumed > 0 && (svsk->sk_rx_held || svsk->sk_rx_hold_pending))
		svc_tcp_rx_geometry_account(svsk, skb->truesize, skb->len,
					    consumed);
	return consumed;
}

#if IS_ENABLED(CONFIG_KUNIT)
int svc_tcp_recv_actor_kunit(struct svc_rqst *rqstp, unsigned int *frags,
			     bool *complete, read_descriptor_t *desc,
			     struct sk_buff *skb, unsigned int offset,
			     size_t len)
{
	struct svc_tcp_recv_ctx ctx = {
		.rqstp = rqstp,
		.frags = *frags,
		.complete = *complete,
	};
	void *arg = desc->arg.data;
	int ret;

	desc->arg.data = &ctx;
	ret = svc_tcp_recv_actor(desc, skb, offset, len);
	desc->arg.data = arg;
	*frags = ctx.frags;
	*complete = ctx.complete;
	return ret;
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_recv_actor_kunit);
#endif

static bool svc_tcp_at_urg_mark(struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);

	return tp->urg_data && tp->urg_seq == tp->copied_seq;
}

/*
 * Called after a read, with the socket locked.  A record that is being
 * loaned costs no copy to take up in pieces, only a pass for each: a wake,
 * hand-offs of the record and an ACK.  While such a record is incomplete
 * and the read took all that was queued, svc_data_ready() wakes no thread
 * until what the record lacks has arrived, or half of the receive window
 * still open, whichever is less; otherwise it wakes one for any data.  The
 * peer can send no more than that window before the next read opens it
 * again: waiting for more would leave it idle until the window had all
 * but closed, and taking up half keeps the other half arriving meanwhile.
 * Segments that arrived during the read are on the backlog and meet the
 * new mark when the lock is released.  A TLS transport is left alone: its
 * records are not the bytes TCP counts.
 */
VISIBLE_IF_KUNIT void svc_tcp_rx_set_want(struct svc_sock *svsk,
					  const struct svc_tcp_rx_state *state,
					  bool drained)
{
	u32 want = 0;

	if (drained && state && state->cr.credit &&
	    svsk->sk_tcplen >= sizeof(rpc_fraghdr) &&
	    !test_bit(XPT_TLS_SESSION, &svsk->sk_xprt.xpt_flags)) {
		u32 got = svsk->sk_tcplen - sizeof(rpc_fraghdr);
		u32 reclen = svc_sock_reclen(svsk);

		if (reclen > got)
			want = min_t(u32, reclen - got,
				     tcp_receive_window(tcp_sk(svsk->sk_sk)) >> 1);
	}
	if (READ_ONCE(svsk->sk_rx_want) != want)
		WRITE_ONCE(svsk->sk_rx_want, want);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_set_want);

VISIBLE_IF_KUNIT void svc_tcp_rx_hold_buffer(struct svc_sock *svsk);

/**
 * svc_tcp_recvfrom - Receive data from a TCP socket
 * @rqstp: request structure into which to receive an RPC Call
 *
 * Called in a loop when XPT_DATA has been set.
 *
 * Context: Process context. Takes and releases the socket lock.
 *
 * Returns:
 *   On success, the number of bytes in a received RPC Call, or
 *   %0 if a complete RPC Call message was not ready to return
 *
 * The zero return case handles partial receives and callback Replies.
 * The state of a partial receive is preserved in the svc_sock for
 * the next call to svc_tcp_recvfrom.
 */
VISIBLE_IF_KUNIT int svc_tcp_recvfrom(struct svc_rqst *rqstp)
{
	struct svc_sock	*svsk =
		container_of(rqstp->rq_xprt, struct svc_sock, sk_xprt);
	struct svc_serv	*serv = svsk->sk_xprt.xpt_server;
	struct svc_tcp_recv_ctx ctx = {
		.rqstp		= rqstp,
	};
	read_descriptor_t desc = {
		.arg.data	= &ctx,
		.count		= serv->sv_max_mesg + sizeof(rpc_fraghdr),
	};
	struct socket *sock = svsk->sk_sock;
	ssize_t len;
	__be32 *p;
	__be32 calldir;

	clear_bit(XPT_DATA, &svsk->sk_xprt.xpt_flags);
	len = svc_tcp_restore_pages(svsk, rqstp);
	if (len < 0)
		goto err_nuts;

again:
	lock_sock(sock->sk);
	len = sock->ops->read_sock(sock->sk, &desc, svc_tcp_recv_actor);
	/* ->read_sock stops at urgent data and consumes none of it.
	 * Only recvmsg() clears the condition, and this path calls
	 * none, so every later read stops at the same octet. An RPC
	 * stream carries no urgent data, so close the connection.
	 *
	 * The read that first reaches the mark consumes the octets
	 * ahead of it, so the stop does not show up as a zero len. A
	 * record completed ahead of the mark is returned first. The
	 * XPT_DATA set below brings the next call back here with
	 * nothing left to consume.
	 */
	if (!ctx.complete && svc_tcp_at_urg_mark(sock->sk))
		desc.error = -EPROTO;
	svc_tcp_rx_set_want(svsk, rqstp->rq_tcp_rx,
			    !ctx.complete && !desc.error && desc.count);
	release_sock(sock->sk);
	if (svsk->sk_rx_hold_pending)
		svc_tcp_rx_hold_buffer(svsk);

	/* ->read_sock returns the octets consumed before an actor
	 * failure, so a positive len can accompany desc.error.
	 */
	if (desc.error < 0) {
		len = desc.error;
		goto err_nuts;
	}
	if (len >= 0)
		trace_svcsock_tcp_recv(&svsk->sk_xprt, len);

	if (!ctx.complete) {
		if (!desc.count) {
			/* Waiting for credit: re-arming here would spin
			 * through the threads until credit returns.
			 */
			if (!ctx.credit_wait)
				set_bit(XPT_DATA, &svsk->sk_xprt.xpt_flags);
			goto err_incomplete;
		}
		/* A loaned record waits for its rest with XPT_DATA clear,
		 * and svc_data_ready() sets it again only once enough has
		 * arrived. If that happened by the time the socket lock
		 * was released, read on here rather than park the record
		 * and wake another thread to take it up.
		 */
		if (len >= 0 && READ_ONCE(svsk->sk_rx_want) &&
		    test_and_clear_bit(XPT_DATA, &svsk->sk_xprt.xpt_flags))
			goto again;
		/* A zero return leaves no record at the head to classify.
		 * -EINVAL means a control record sits there. Screen the
		 * other errors out first, because a probe calls
		 * sock_error(), whose xchg clears sk->sk_err as it reads.
		 */
		if (len <= 0 && len != -EINVAL)
			goto err_incomplete;

		len = svc_tcp_recv_ctrl_record(svsk);
		goto err_incomplete;
	}
	if (svsk->sk_datalen < 8)
		goto err_nuts;

	svc_tcp_flush_pages(svsk, rqstp);
	len = svc_tcp_rx_publish(svsk, rqstp);
	if (len < 0)
		goto err_nuts;

	rqstp->rq_prot	      = IPPROTO_TCP;
	assign_bit(RQ_LOCAL, &rqstp->rq_flags,
		   test_bit(XPT_LOCAL, &svsk->sk_xprt.xpt_flags));

	/* Completing one message stops ->read_sock with whatever
	 * follows still queued, and no path from here re-arms XPT_DATA.
	 * The queued message would wait for unrelated traffic.
	 */
	set_bit(XPT_DATA, &svsk->sk_xprt.xpt_flags);

	p = (__be32 *)rqstp->rq_arg.head[0].iov_base;
	calldir = p[1];
	if (calldir)
		len = receive_cb_reply(svsk, rqstp);

	/* Reset TCP read info */
	svc_tcp_recv_record_done(svsk);

	if (len < 0)
		goto error;

	svc_xprt_copy_addrs(rqstp, &svsk->sk_xprt);
	if (serv->sv_stats)
		serv->sv_stats->nettcpcnt++;

	svc_sock_secure_port(rqstp);
	svc_xprt_received(rqstp->rq_xprt);
	return rqstp->rq_arg.len;

err_incomplete:
	if (svc_tcp_save_pages(svsk, rqstp)) {
		len = -EINVAL;
		svc_tcp_rx_abort_active(svsk, rqstp, SVC_TCP_RX_ERROR,
					SVC_TCP_RX_REASON_INVARIANT, false);
		goto err_delete;
	}
	if (len < 0 && len != -EAGAIN)
		goto err_delete;
	if (svsk->sk_tcplen >= sizeof(rpc_fraghdr))
		trace_svcsock_tcp_recv_short(&svsk->sk_xprt,
				svc_sock_reclen(svsk),
				svsk->sk_tcplen - sizeof(rpc_fraghdr));
	else
		trace_svcsock_tcp_recv_eagain(&svsk->sk_xprt, 0);
	goto err_noclose;
error:
	trace_svcsock_tcp_recv_eagain(&svsk->sk_xprt, 0);
	goto err_noclose;
err_nuts:
	if (svc_tcp_rx_abort_active(svsk, rqstp, SVC_TCP_RX_ERROR,
				    len == -EREMOTEIO ?
					SVC_TCP_RX_REASON_NET_IOV :
				    len == -EFAULT ?
					SVC_TCP_RX_REASON_COPY_FAULT :
					SVC_TCP_RX_REASON_INVARIANT,
				    svsk->sk_datalen == 0))
		len = -EINVAL;
	/* svc_tcp_save_pages() has not run, so svsk->sk_pages[] holds
	 * no record. A non-zero sk_datalen makes the teardown-time
	 * svc_tcp_clear_pages() walk empty slots and WARN.
	 */
	svsk->sk_datalen = 0;
err_delete:
	trace_svcsock_tcp_recv_err(&svsk->sk_xprt, len);
	svc_xprt_deferred_close(&svsk->sk_xprt);
err_noclose:
	svc_xprt_received(rqstp->rq_xprt);
	return 0;	/* record not complete */
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_recvfrom);

/*
 * Fill @skb with region pages.  A fresh skb takes them without page
 * references: the reply's ubuf_info holds them until TCP frees its last
 * socket buffer (as io_uring does for registered buffers).  An skb that
 * already carries the reply's spliced header takes them with references,
 * so header and payload share one segment; the completion then waits for
 * those references before returning the pages.
 */
static int svc_rx_region_sg_from_iter(struct sk_buff *skb,
				      struct iov_iter *from, size_t length)
{
	struct skb_shared_info *shinfo = skb_shinfo(skb);
	int frag = shinfo->nr_frags;
	unsigned long truesize = 0;
	ssize_t copied = 0;
	bool refs = frag && !skb_zcopy_managed(skb);
	struct bvec_iter bi;
	int ret = 0;

	if (!frag)
		shinfo->flags |= SKBFL_MANAGED_FRAG_REFS;

	bi.bi_size = min(from->count, length);
	bi.bi_offset = from->iov_offset;
	bi.bi_idx = 0;

	while (bi.bi_size && frag < MAX_SKB_FRAGS) {
		struct bio_vec v = mp_bvec_iter_bvec(from->bvec, bi);

		copied += v.bv_len;
		truesize += PAGE_ALIGN(v.bv_len + v.bv_offset);
		if (refs)
			get_page(v.bv_page);
		__skb_fill_page_desc_noacc(shinfo, frag++, v.bv_page,
					   v.bv_offset, v.bv_len);
		bvec_iter_advance_single(from->bvec, &bi, v.bv_len);
	}
	if (bi.bi_size)
		ret = -EMSGSIZE;

	shinfo->nr_frags = frag;
	from->bvec += bi.bi_idx;
	from->nr_segs -= bi.bi_idx;
	from->count -= copied;
	from->iov_offset = bi.bi_offset;

	skb->data_len += copied;
	skb->len += copied;
	skb->truesize += truesize;
	return ret;
}

/*
 * Does bvec @k of the reply (as svc_tcp_sendmsg() builds it: record
 * marker, head, one entry per rq_res page, tail) carry a region page?
 * Decided by the reply slot's bit in rq_rx_region_map, never by address.
 */
VISIBLE_IF_KUNIT bool svc_rx_region_send_bvec(const struct svc_rqst *rqstp,
					      unsigned int nvecs,
					      unsigned int k)
{
	const struct xdr_buf *xdr = &rqstp->rq_res;
	unsigned int first = 1 + !!xdr->head[0].iov_len;
	unsigned int npages = nvecs - first - !!xdr->tail[0].iov_len;

	return k >= first && k < first + npages &&
	       test_bit(xdr->pages - rqstp->rq_respages + k - first,
			rqstp->rq_rx_region_map);
}
EXPORT_SYMBOL_IF_KUNIT(svc_rx_region_send_bvec);

/*
 * Send a reply carrying region pages: runs of the page bvecs whose reply
 * slots hold region pages go zero-copy under the reply's ubuf_info, all
 * else (record marker, head, tail, other pages) is spliced as usual, all
 * under xpt_mutex.  The region pages then belong to TCP, whatever was
 * sent.
 */
static int svc_tcp_sendmsg_rx_region(struct svc_sock *svsk,
				     struct svc_rqst *rqstp,
				     unsigned int nvecs)
{
	struct svc_rx_region_tx *tx = rqstp->rq_rx_region_tx;
	struct bio_vec *bv = svsk->sk_bvec;
	unsigned int i = 0, j;
	int sent = 0, ret = 0;

#define svc_rx_region_bvec(k) svc_rx_region_send_bvec(rqstp, nvecs, (k))
	while (i < nvecs) {
		bool region = svc_rx_region_bvec(i);
		struct msghdr msg = {};
		size_t bytes = 0;

		for (j = i; j < nvecs && svc_rx_region_bvec(j) == region; j++)
			bytes += bv[j].bv_len;
		msg.msg_flags = region ? MSG_ZEROCOPY : MSG_SPLICE_PAGES;
		if (j < nvecs)
			msg.msg_flags |= MSG_MORE;
		if (region) {
			msg.msg_ubuf = &tx->ubuf;
			msg.sg_from_iter = svc_rx_region_sg_from_iter;
		}
		iov_iter_bvec(&msg.msg_iter, ITER_SOURCE, bv + i, j - i, bytes);
		ret = sock_sendmsg(svsk->sk_sock, &msg);
		if (ret < 0)
			break;
		sent += ret;
		if (ret != bytes)
			break;
		i = j;
	}
#undef svc_rx_region_bvec
	svc_rx_region_handoff(rqstp);
	return ret < 0 && !sent ? ret : sent;
}

/*
 * MSG_SPLICE_PAGES is used exclusively to reduce the number of
 * copy operations in this path. Therefore the caller must ensure
 * that the pages backing @xdr are unchanging.
 */
static int svc_tcp_sendmsg(struct svc_sock *svsk, struct svc_rqst *rqstp,
			   rpc_fraghdr marker)
{
	struct msghdr msg = {
		.msg_flags	= MSG_SPLICE_PAGES,
	};
	int count;
	void *buf;
	int ret;

	/* The stream record marker is copied into a temporary page
	 * fragment buffer so that it can be included in sk_bvec.
	 */
	buf = page_frag_alloc(&svsk->sk_frag_cache, sizeof(marker),
			      GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	memcpy(buf, &marker, sizeof(marker));
	bvec_set_virt(svsk->sk_bvec, buf, sizeof(marker));

	count = xdr_buf_to_bvec(svsk->sk_bvec + 1, rqstp->rq_maxpages,
				&rqstp->rq_res);
	if (count < 0) {
		ret = count;
		goto out;
	}

	if (svc_rqst_rx_region_used(rqstp)) {
		ret = svc_tcp_sendmsg_rx_region(svsk, rqstp, 1 + count);
		goto out;
	}
	iov_iter_bvec(&msg.msg_iter, ITER_SOURCE, svsk->sk_bvec,
		      1 + count, sizeof(marker) + rqstp->rq_res.len);
	ret = sock_sendmsg(svsk->sk_sock, &msg);
out:
	page_frag_free(buf);
	return ret;
}

/**
 * svc_tcp_sendto - Send out a reply on a TCP socket
 * @rqstp: completed svc_rqst
 *
 * xpt_mutex ensures @rqstp's whole message is written to the socket
 * without interruption.
 *
 * Returns the number of bytes sent, or a negative errno.
 */
static int svc_tcp_sendto(struct svc_rqst *rqstp)
{
	struct svc_xprt *xprt = rqstp->rq_xprt;
	struct svc_sock	*svsk = container_of(xprt, struct svc_sock, sk_xprt);
	struct xdr_buf *xdr = &rqstp->rq_res;
	rpc_fraghdr marker = cpu_to_be32(RPC_LAST_STREAM_FRAGMENT |
					 (u32)xdr->len);
	int sent;

	svc_tcp_release_ctxt(xprt, rqstp->rq_xprt_ctxt);
	rqstp->rq_xprt_ctxt = NULL;

	mutex_lock(&xprt->xpt_mutex);
	if (svc_xprt_is_dead(xprt))
		goto out_notconn;
	sent = svc_tcp_sendmsg(svsk, rqstp, marker);
	trace_svcsock_tcp_send(xprt, sent);
	if (sent < 0 || sent != (xdr->len + sizeof(marker)))
		goto out_close;
	mutex_unlock(&xprt->xpt_mutex);
	return sent;

out_notconn:
	mutex_unlock(&xprt->xpt_mutex);
	return -ENOTCONN;
out_close:
	pr_notice("rpc-srv/tcp: %s: %s %d when sending %zu bytes - shutting down socket\n",
		  xprt->xpt_server->sv_name,
		  (sent < 0) ? "got error" : "sent",
		  sent, xdr->len + sizeof(marker));
	svc_xprt_deferred_close(xprt);
	mutex_unlock(&xprt->xpt_mutex);
	return -EAGAIN;
}

static struct svc_xprt *svc_tcp_create(struct svc_serv *serv,
				       struct net *net,
				       struct sockaddr *sa, int salen,
				       int flags)
{
	return svc_create_socket(serv, IPPROTO_TCP, net, sa, salen, flags);
}

static const struct svc_xprt_ops svc_tcp_ops = {
	.xpo_create = svc_tcp_create,
	.xpo_recvfrom = svc_tcp_recvfrom,
	.xpo_sendto = svc_tcp_sendto,
	.xpo_result_payload = svc_sock_result_payload,
	.xpo_release_ctxt = svc_tcp_release_ctxt,
	.xpo_release_payload = svc_tcp_release_payload,
	.xpo_bounce_payload = svc_tcp_bounce_payload,
	.xpo_bounce_done = svc_tcp_rx_bounce_done,
	.xpo_rx_region_read = svc_tcp_rx_region_read,
	.xpo_rx_region_unconfine = svc_tcp_rx_region_unconfine_op,
	.xpo_rx_region_park_free = svc_tcp_rx_region_park_free,
	.xpo_detach = svc_tcp_sock_detach,
	.xpo_free = svc_sock_free,
	.xpo_has_wspace = svc_tcp_has_wspace,
	.xpo_accept = svc_tcp_accept,
	.xpo_kill_temp_xprt = svc_tcp_kill_temp_xprt,
	.xpo_handshake = svc_tcp_handshake,
};

static struct svc_xprt_class svc_tcp_class = {
	.xcl_name = "tcp",
	.xcl_owner = THIS_MODULE,
	.xcl_ops = &svc_tcp_ops,
	.xcl_max_payload = RPCSVC_MAXPAYLOAD_TCP,
	.xcl_ident = XPRT_TRANSPORT_TCP,
};

/*
 * The default budget and the read pool go on the region's list too, so a
 * region free kicks their waiters; with no memory for that they keep their
 * static stocks and are kicked only by their own returns.
 */
static bool svc_rx_budget_default_listed;

static bool svc_tcp_rx_budget_list(struct svc_tcp_rx_budget *c, bool read)
{
	struct rx_loan_stock __percpu *stock = c->lb->stock;

	if (!rx_loan_budget_init(c->lb, read))
		return true;
	c->lb->stock = stock;
	return false;
}

void svc_init_xprt_sock(void)
{
	svc_rx_region_init();
	svc_rx_budget_default_listed =
		svc_tcp_rx_budget_list(&svc_rx_budget_default, false);
	/* The read pool is the initial namespace's rx_loan READ budget. */
	svc_rx_budget_read.lb = &rx_loan_net(&init_net)->read;
	WRITE_ONCE(svc_rx_budget_read.lb->owner, &svc_rx_budget_read);
	svc_tcp_rx_budget_init();
	svc_reg_xprt_class(&svc_tcp_class);
	svc_reg_xprt_class(&svc_udp_class);
}

void svc_cleanup_xprt_sock(void)
{
	svc_unreg_xprt_class(&svc_tcp_class);
	svc_unreg_xprt_class(&svc_udp_class);
	/* Dropped-park kicks of the budgets that are not per namespace. */
	if (svc_rx_budget_default_listed)
		rx_loan_budget_destroy(svc_rx_budget_default.lb);
	else
		flush_work(&svc_rx_budget_default.lb->kick_work);
	flush_work(&svc_rx_budget_read.lb->kick_work);
	WRITE_ONCE(svc_rx_budget_read.lb->owner, NULL);
	svc_rx_region_cleanup();
}

/*
 * A transport's receive buffer is locked, and counted in the memory its
 * skbs occupy, @factor bytes for each 100 of payload
 * (svc_tcp_rx_hold_factor()).  Half of the payload it can hold is the most
 * its window is opened to.
 *
 * The window is what the client may have on the way, and the other half is
 * for what has arrived and not been read: a thread is still copying the
 * record before, or waiting for credit.  With a window of all the buffer
 * every byte not yet read comes off what the client may send, and it is
 * held up whenever the reader is a little behind; and all the transports'
 * windows together arrive as one burst that the device's rings have to
 * take.
 *
 * TCP converts free receive memory into window at a payload/truesize ratio
 * it learns from full-size segments only, and would open the window to all
 * the buffer holds.  Cap the ratio at half of what the transport's skbs
 * were measured to occupy, in TCP_RMEM_TO_WIN_SCALE units, so that the
 * window also closes by half of what is left as the buffer fills.
 */
VISIBLE_IF_KUNIT u8 svc_tcp_rx_scaling_ratio(unsigned int factor)
{
	return div64_u64((u64)100 << TCP_RMEM_TO_WIN_SCALE,
			 (u64)max(factor, 100U) * 2);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_scaling_ratio);

/*
 * The most a transport's window is opened to: what TCP makes of the whole
 * of @rcvbuf at svc_tcp_rx_scaling_ratio(), so that the clamp is never
 * more than TCP would offer of an empty buffer.
 */
VISIBLE_IF_KUNIT int svc_tcp_rx_window_clamp(int rcvbuf, unsigned int factor)
{
	return ((u64)rcvbuf * svc_tcp_rx_scaling_ratio(factor)) >>
	       TCP_RMEM_TO_WIN_SCALE;
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_window_clamp);

/*
 * Bytes of payload a transport reads between two samples of what its skbs
 * occupy.  Long enough to hold several records, so that a record's short
 * last segment, which takes a whole receive buffer, weighs in a sample as
 * it does in the receive queue.
 */
#define SVC_TCP_RX_GEOMETRY_WINDOW	(4U << 20)
/*
 * An skb with less payload than this is not measured.  A call, a reply's
 * acknowledgment or a small request has a receive buffer to itself, a
 * hundred bytes in several thousand, and a transport that reads a few of
 * those before any data would be held to a window of less than a segment,
 * which TCP does not advertise at all: the first connection of a freshly
 * loaded server stopped that way.  A record's short last segment is not
 * lost by this, since GRO has it in an skb with the full ones before it.
 */
#define SVC_TCP_RX_GEOMETRY_MIN_SKB	1024U

static unsigned int svc_tcp_rx_geometry_sample(u32 mem, u32 pay)
{
	return clamp_t(u64, DIV_ROUND_UP_ULL((u64)mem * 100, pay), 100,
		       SVC_TCP_RX_CREDIT_FACTOR_MAX);
}

/*
 * @svsk read @used bytes of an skb of @skb_len bytes that occupies
 * @truesize.  TCP counts a receive buffer in truesize and a window in
 * payload, and what one is to the other is a property of how the device
 * cuts its pages into receive buffers and of the segments' sizes, so it is
 * measured: the memory and the payload of everything the transport reads
 * are summed, and every SVC_TCP_RX_GEOMETRY_WINDOW bytes their ratio is a
 * sample.  The transport's receive buffer is held to the higher of its
 * last two (svc_tcp_rx_hold_factor()).  Only the thread reading the
 * transport gets here.
 */
VISIBLE_IF_KUNIT void svc_tcp_rx_geometry_account(struct svc_sock *svsk,
						  unsigned int truesize,
						  unsigned int skb_len,
						  unsigned int used)
{
	if (skb_len < SVC_TCP_RX_GEOMETRY_MIN_SKB || !used)
		return;
	svsk->sk_rx_geo_mem += div_u64((u64)truesize * min(used, skb_len),
				       skb_len);
	svsk->sk_rx_geo_pay += used;
	if (svsk->sk_rx_geo_pay < SVC_TCP_RX_GEOMETRY_WINDOW)
		return;
	svsk->sk_rx_geo_factor[1] = svsk->sk_rx_geo_factor[0];
	svsk->sk_rx_geo_factor[0] =
		svc_tcp_rx_geometry_sample(svsk->sk_rx_geo_mem,
					   svsk->sk_rx_geo_pay);
	svsk->sk_rx_geo_mem = 0;
	svsk->sk_rx_geo_pay = 0;
	WRITE_ONCE(svc_tcp_rx_budget_for(svsk)->lb->hold_factor,
		   max(svsk->sk_rx_geo_factor[0], svsk->sk_rx_geo_factor[1]));
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_geometry_account);

/*
 * What @svsk's receive buffer is held to, as the bytes its skbs occupy for
 * each 100 of payload: the higher of the transport's last two samples,
 * else, until it has one, what another transport of its budget measured,
 * else the factor this code had before it measured.  What a transport has
 * read of its first window is not used: a few skbs are not a sample.
 */
VISIBLE_IF_KUNIT unsigned int svc_tcp_rx_hold_factor(struct svc_sock *svsk)
{
	unsigned int factor = svc_tcp_rx_fixed_factor();

	if (factor)
		return factor;
	factor = max(svsk->sk_rx_geo_factor[0], svsk->sk_rx_geo_factor[1]);
	if (factor)
		return factor;
	return READ_ONCE(svc_tcp_rx_budget_for(svsk)->lb->hold_factor) ?:
	       SVC_TCP_RX_CREDIT_FACTOR_UNLEARNED;
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_hold_factor);

/*
 * A transport whose buffer is held to one figure and has since measured
 * another is held again, once the two are an eighth apart.
 */
static bool svc_tcp_rx_hold_stale(struct svc_sock *svsk)
{
	unsigned int held = svsk->sk_rx_hold_factor;
	unsigned int now = svc_tcp_rx_hold_factor(svsk);

	return abs_diff(now, held) * 8 > held;
}

/*
 * A transport that receives region pages: lock its receive buffer to
 * svc_tcp_rx_rcvbuf, cap the payload/truesize ratio TCP turns free buffer
 * into window with at the region's geometry, and clamp the window, so its
 * unread data stays its share of the region.  The ceiling also keeps TCP
 * from raising the clamp again when it re-learns its ratio.  Transports
 * that never receive a region page keep TCP's own buffer.  TCP sizes a
 * connection's first window from its listener when the SYN arrives and
 * never shrinks a window it has offered, so listeners are held too: a
 * connection that opened with the listener's larger window would overflow
 * the buffer it is held to with its first burst.  Called with the socket
 * unlocked: each call takes the socket lock itself.
 */
VISIBLE_IF_KUNIT void svc_tcp_rx_hold_buffer(struct svc_sock *svsk)
{
	unsigned int bytes = svc_tcp_rx_rcvbuf_effective();
	unsigned int factor = svc_tcp_rx_hold_factor(svsk);
	struct sock *sk = svsk->sk_sk;
	int rcvbuf, clamp;
	bool again;
	u32 old_clamp;
	u8 ratio, old_ratio;

	svsk->sk_rx_hold_pending = false;
	if (!bytes || svsk->sk_rx_held)
		return;
	again = svsk->sk_rx_hold_factor != 0;
	svsk->sk_rx_hold_factor = factor;
	rcvbuf = max_t(int, bytes, SOCK_MIN_RCVBUF);
	/*
	 * Never under two segments: TCP advertises no window smaller than
	 * one, and the sender then waits on a receiver that waits on it.
	 */
	clamp = max_t(int, svc_tcp_rx_window_clamp(rcvbuf, factor),
		      2 * READ_ONCE(tcp_sk(sk)->advmss));
	ratio = svc_tcp_rx_scaling_ratio(factor);

	old_ratio = READ_ONCE(tcp_sk(sk)->scaling_ratio_max);
	old_clamp = READ_ONCE(tcp_sk(sk)->window_clamp);

	/* Unread data is this transport's share of the region. */
	sock_set_rcvbuf(sk, rcvbuf / 2);	/* doubled on the way in */
	tcp_sock_set_scaling_ratio_max(sk, ratio);
	if (tcp_sock_set_window_clamp(sk, clamp))
		pr_warn_once("svc: TCP receive window clamp failed\n");
	else if (again)
		tcp_sock_raise_rcv_window(sk, old_ratio, old_clamp);
	svsk->sk_rx_held = true;
	trace_svcsock_tcp_rx_hold(&svsk->sk_xprt, factor, rcvbuf, clamp);
}
EXPORT_SYMBOL_IF_KUNIT(svc_tcp_rx_hold_buffer);

static void svc_tcp_init(struct svc_sock *svsk, struct svc_serv *serv)
{
	struct sock	*sk = svsk->sk_sk;

	svc_xprt_init(sock_net(svsk->sk_sock->sk), &svc_tcp_class,
		      &svsk->sk_xprt, serv);
	set_bit(XPT_CACHE_AUTH, &svsk->sk_xprt.xpt_flags);
	set_bit(XPT_CONG_CTRL, &svsk->sk_xprt.xpt_flags);
	if (sk->sk_state == TCP_LISTEN) {
		strcpy(svsk->sk_xprt.xpt_remotebuf, "listener");
		set_bit(XPT_LISTENER, &svsk->sk_xprt.xpt_flags);
		set_bit(XPT_RPCB_UNREG, &svsk->sk_xprt.xpt_flags);
		sk->sk_data_ready = svc_tcp_listen_data_ready;
		set_bit(XPT_CONN, &svsk->sk_xprt.xpt_flags);
		/* Connections take their first window from the listener. */
		svc_tcp_rx_hold_buffer(svsk);
	} else {
		sk->sk_state_change = svc_tcp_state_change;
		sk->sk_data_ready = svc_data_ready;
		sk->sk_write_space = svc_write_space;

		svsk->sk_marker = xdr_zero;
		svsk->sk_tcplen = 0;
		svsk->sk_datalen = 0;
		memset(&svsk->sk_pages[0], 0,
		       svsk->sk_maxpages * sizeof(struct page *));

		tcp_sock_set_nodelay(sk);
		/*
		 * A peer that stops acknowledging must not keep its share
		 * (Q-C: stock NFSD sets no user timeout; 0 restores that).
		 */
		if (READ_ONCE(svc_tcp_rx_region_reads) &&
		    READ_ONCE(svc_tcp_rx_region_user_timeout))
			tcp_sock_set_user_timeout(sk,
				READ_ONCE(svc_tcp_rx_region_user_timeout));

		set_bit(XPT_DATA, &svsk->sk_xprt.xpt_flags);
		switch (sk->sk_state) {
		case TCP_SYN_RECV:
		case TCP_ESTABLISHED:
			break;
		default:
			svc_xprt_deferred_close(&svsk->sk_xprt);
		}
	}
}

void svc_sock_update_bufs(struct svc_serv *serv)
{
	/*
	 * The number of server threads has changed. Update
	 * rcvbuf and sndbuf accordingly on all sockets
	 */
	struct svc_sock *svsk;

	spin_lock_bh(&serv->sv_lock);
	list_for_each_entry(svsk, &serv->sv_permsocks, sk_xprt.xpt_list)
		set_bit(XPT_CHNGBUF, &svsk->sk_xprt.xpt_flags);
	spin_unlock_bh(&serv->sv_lock);
}

static int svc_sock_sendpages(struct svc_serv *serv, struct socket *sock, int flags)
{
	switch (sock->type) {
	case SOCK_STREAM:
		/* +1 for TCP record marker */
		if (flags & SVC_SOCK_TEMPORARY)
			return svc_serv_maxpages(serv) + 1;
		return 0;
	case SOCK_DGRAM:
		return SUNRPC_MAX_UDP_SENDPAGES;
	}
	return -EINVAL;
}

/*
 * Initialize socket for RPC use and create svc_sock struct
 */
static struct svc_sock *svc_setup_socket(struct svc_serv *serv,
						struct socket *sock,
						int flags)
{
	struct svc_sock	*svsk;
	struct sock	*inet;
	int		pmap_register = !(flags & SVC_SOCK_ANONYMOUS);
	int		sendpages;
	unsigned long	pages;

	sendpages = svc_sock_sendpages(serv, sock, flags);
	if (sendpages < 0)
		return ERR_PTR(sendpages);

	pages = svc_serv_maxpages(serv);
	svsk = kzalloc_flex(*svsk, sk_pages, pages);
	if (svsk) {
		svc_tcp_rx_wait_init(svsk);
		spin_lock_init(&svsk->sk_rx_region_lock);
		INIT_LIST_HEAD(&svsk->sk_rx_region_waiters);
	}
	if (!svsk)
		return ERR_PTR(-ENOMEM);

	if (sendpages) {
		svsk->sk_bvec = kzalloc_objs(*svsk->sk_bvec, sendpages);
		if (!svsk->sk_bvec) {
			kfree(svsk);
			return ERR_PTR(-ENOMEM);
		}
	}

	svsk->sk_maxpages = pages;

	inet = sock->sk;
	if (sock->type == SOCK_STREAM && inet->sk_state != TCP_LISTEN) {
		svsk->sk_rx = svc_tcp_rx_state_alloc_entries(svc_serv_maxbvecs(serv),
							     pages, GFP_KERNEL,
							     NUMA_NO_NODE);
		if (!svsk->sk_rx) {
			kfree(svsk->sk_bvec);
			kfree(svsk);
			return ERR_PTR(-ENOMEM);
		}
	}

	if (pmap_register) {
		int err;

		err = svc_register(serv, sock_net(sock->sk), inet->sk_family,
				     inet->sk_protocol,
				     ntohs(inet_sk(inet)->inet_sport));
		if (err < 0 && !(flags & SVC_SOCK_RPCBIND_NOERR)) {
			svc_tcp_rx_state_free(svsk->sk_rx);
			kfree(svsk->sk_bvec);
			kfree(svsk);
			return ERR_PTR(err);
		}
	}

	svsk->sk_sock = sock;
	svsk->sk_sk = inet;
	svsk->sk_ostate = inet->sk_state_change;
	svsk->sk_odata = inet->sk_data_ready;
	svsk->sk_owspace = inet->sk_write_space;
	/*
	 * This barrier is necessary in order to prevent race condition
	 * with svc_data_ready(), svc_tcp_listen_data_ready(), and others
	 * when calling callbacks above.
	 */
	wmb();
	inet->sk_user_data = svsk;

	/* Initialize the socket */
	if (sock->type == SOCK_DGRAM)
		svc_udp_init(svsk, serv);
	else
		svc_tcp_init(svsk, serv);

	trace_svcsock_new(svsk, sock);
	return svsk;
}

/**
 * svc_addsock - add a listener socket to an RPC service
 * @serv: pointer to RPC service to which to add a new listener
 * @net: caller's network namespace
 * @fd: file descriptor of the new listener
 * @name_return: pointer to buffer to fill in with name of listener
 * @len: size of the buffer
 * @flags: flags argument for svc_setup_socket()
 * @cred: credential
 *
 * Fills in socket name and returns positive length of name if successful.
 * Name is terminated with '\n'.  On error, returns a negative errno
 * value.
 */
int svc_addsock(struct svc_serv *serv, struct net *net, const int fd,
		char *name_return, const size_t len, int flags,
		const struct cred *cred)
{
	int err = 0;
	struct socket *so = sockfd_lookup(fd, &err);
	struct svc_sock *svsk = NULL;
	struct sockaddr_storage addr;
	struct sockaddr *sin = (struct sockaddr *)&addr;
	int salen;

	if (!so)
		return err;
	err = -EINVAL;
	if (sock_net(so->sk) != net)
		goto out;
	err = -EAFNOSUPPORT;
	if ((so->sk->sk_family != PF_INET) && (so->sk->sk_family != PF_INET6))
		goto out;
	err =  -EPROTONOSUPPORT;
	if (so->sk->sk_protocol != IPPROTO_TCP &&
	    so->sk->sk_protocol != IPPROTO_UDP)
		goto out;
	err = -EISCONN;
	if (so->state > SS_UNCONNECTED)
		goto out;
	err = -EBUSY;
	if (so->sk->sk_user_data)
		goto out;
	err = -ENOENT;
	if (!try_module_get(THIS_MODULE))
		goto out;
	svsk = svc_setup_socket(serv, so, flags);
	if (IS_ERR(svsk)) {
		module_put(THIS_MODULE);
		err = PTR_ERR(svsk);
		goto out;
	}
	salen = kernel_getsockname(svsk->sk_sock, sin);
	if (salen >= 0)
		svc_xprt_set_local(&svsk->sk_xprt, sin, salen);
	svsk->sk_xprt.xpt_cred = get_cred(cred);
	svc_add_new_perm_xprt(serv, &svsk->sk_xprt);
	return svc_one_sock_name(svsk, name_return, len);
out:
	sockfd_put(so);
	return err;
}
EXPORT_SYMBOL_GPL(svc_addsock);

/*
 * Create socket for RPC service.
 */
static struct svc_xprt *svc_create_socket(struct svc_serv *serv,
					  int protocol,
					  struct net *net,
					  struct sockaddr *sin, int len,
					  int flags)
{
	struct svc_sock	*svsk;
	struct socket	*sock;
	int		error;
	int		type;
	struct sockaddr_storage addr;
	struct sockaddr *newsin = (struct sockaddr *)&addr;
	int		newlen;
	int		family;

	if (protocol != IPPROTO_UDP && protocol != IPPROTO_TCP) {
		printk(KERN_WARNING "svc: only UDP and TCP "
				"sockets supported\n");
		return ERR_PTR(-EINVAL);
	}

	type = (protocol == IPPROTO_UDP)? SOCK_DGRAM : SOCK_STREAM;
	switch (sin->sa_family) {
	case AF_INET6:
		family = PF_INET6;
		break;
	case AF_INET:
		family = PF_INET;
		break;
	default:
		return ERR_PTR(-EINVAL);
	}

	error = __sock_create(net, family, type, protocol, &sock, 1);
	if (error < 0)
		return ERR_PTR(error);

	svc_reclassify_socket(sock);

	/*
	 * If this is an PF_INET6 listener, we want to avoid
	 * getting requests from IPv4 remotes.  Those should
	 * be shunted to a PF_INET listener via rpcbind.
	 */
	if (family == PF_INET6)
		ip6_sock_set_v6only(sock->sk);
	if (type == SOCK_STREAM)
		sock->sk->sk_reuse = SK_CAN_REUSE; /* allow address reuse */
	error = kernel_bind(sock, (struct sockaddr_unsized *)sin, len);
	if (error < 0)
		goto bummer;

	error = kernel_getsockname(sock, newsin);
	if (error < 0)
		goto bummer;
	newlen = error;

	if (protocol == IPPROTO_TCP) {
		sk_net_refcnt_upgrade(sock->sk);
		if ((error = kernel_listen(sock, SOMAXCONN)) < 0)
			goto bummer;
	}

	svsk = svc_setup_socket(serv, sock, flags);
	if (IS_ERR(svsk)) {
		error = PTR_ERR(svsk);
		goto bummer;
	}
	svc_xprt_set_local(&svsk->sk_xprt, newsin, newlen);
	return (struct svc_xprt *)svsk;
bummer:
	sock_release(sock);
	return ERR_PTR(error);
}

/*
 * Detach the svc_sock from the socket so that no
 * more callbacks occur.
 */
static void svc_sock_detach(struct svc_xprt *xprt)
{
	struct svc_sock *svsk = container_of(xprt, struct svc_sock, sk_xprt);
	struct sock *sk = svsk->sk_sk;

	/* put back the old socket callbacks */
	lock_sock(sk);
	sk->sk_state_change = svsk->sk_ostate;
	sk->sk_data_ready = svsk->sk_odata;
	sk->sk_write_space = svsk->sk_owspace;
	sk->sk_user_data = NULL;
	release_sock(sk);
}

/*
 * Disconnect the socket, and reset the callbacks
 */
static void svc_tcp_sock_detach(struct svc_xprt *xprt)
{
	struct svc_sock *svsk = container_of(xprt, struct svc_sock, sk_xprt);

	tls_handshake_close(svsk->sk_sock);

	svc_sock_detach(xprt);

	if (!test_bit(XPT_LISTENER, &xprt->xpt_flags)) {
		svc_tcp_rx_credit_forget(svsk);
		svc_rx_region_conn_drain(svsk);
		svc_tcp_clear_pages(svsk);
		kernel_sock_shutdown(svsk->sk_sock, SHUT_RDWR);
	}
}

/*
 * Free the svc_sock's socket resources and the svc_sock itself.
 */
static void svc_sock_free(struct svc_xprt *xprt)
{
	struct svc_sock *svsk = container_of(xprt, struct svc_sock, sk_xprt);
	struct socket *sock = svsk->sk_sock;

	trace_svcsock_free(svsk, sock);

	tls_handshake_cancel(sock->sk);
	if (sock->file)
		sockfd_put(sock);
	else
		sock_release(sock);

	page_frag_cache_drain(&svsk->sk_frag_cache);
	svc_tcp_rx_state_free(svsk->sk_rx);
	kfree(svsk->sk_bvec);
	kfree(svsk);
}
