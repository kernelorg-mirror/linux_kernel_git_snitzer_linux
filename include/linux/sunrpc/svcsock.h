/* SPDX-License-Identifier: GPL-2.0 */
/*
 * linux/include/linux/sunrpc/svcsock.h
 *
 * RPC server socket I/O.
 *
 * Copyright (C) 1995, 1996 Olaf Kirch <okir@monad.swb.de>
 */

#ifndef SUNRPC_SVCSOCK_H
#define SUNRPC_SVCSOCK_H

#include <linux/sunrpc/svc.h>
#include <linux/sunrpc/svc_xprt.h>

struct svc_tcp_rx_budget;

enum svc_tcp_rx_mode {
	SVC_TCP_RX_EMPTY,
	SVC_TCP_RX_PREFIX,
	SVC_TCP_RX_MIXED,
	SVC_TCP_RX_COPY,
	SVC_TCP_RX_PUBLISHED,
};

enum svc_tcp_rx_action {
	SVC_TCP_RX_CLASSIFY,
	SVC_TCP_RX_MATERIALIZE,
	SVC_TCP_RX_PUBLISH,
	SVC_TCP_RX_ERROR,
	SVC_TCP_RX_RELEASE,
	SVC_TCP_RX_ABORT,
	SVC_TCP_RX_PAYLOAD_RELEASE,
	SVC_TCP_RX_BOUNCE,
};

enum svc_tcp_rx_reason {
	SVC_TCP_RX_REASON_NONE,
	SVC_TCP_RX_REASON_SHORT_PREFIX,
	SVC_TCP_RX_REASON_TLS,
	SVC_TCP_RX_REASON_DIRECTION,
	SVC_TCP_RX_REASON_RPC_VERSION,
	SVC_TCP_RX_REASON_PROGRAM,
	SVC_TCP_RX_REASON_VERSION,
	SVC_TCP_RX_REASON_PROCEDURE,
	SVC_TCP_RX_REASON_AUTH,
	SVC_TCP_RX_REASON_LOCKED_HEAD,
	SVC_TCP_RX_REASON_MANAGED_FRAGS,
	SVC_TCP_RX_REASON_CAPACITY,
	SVC_TCP_RX_REASON_UNREADABLE,
	SVC_TCP_RX_REASON_NET_IOV,
	SVC_TCP_RX_REASON_NULL_PAGE,
	SVC_TCP_RX_REASON_COPY_FAULT,
	SVC_TCP_RX_REASON_INVARIANT,
	SVC_TCP_RX_REASON_SMALL,
	SVC_TCP_RX_REASON_NO_BUDGET,
};

/* Records a budget learns its charge over: see svc_tcp_rx_cost_learn_locked(). */
#define SVC_TCP_RX_COST_WINDOW	1024
/*
 * Records a budget releases under its lock, learning their cost, before
 * admission takes its per-CPU stocks: one stock's worth of pages.
 */
#define SVC_TCP_RX_COST_WARMUP	64

/*
 * Receive descriptors and their page references move together by exchanging
 * this preallocated object between a request and a connected TCP socket.
 */
struct svc_tcp_rx_state {
	struct bio_vec		*bvec;
	unsigned long		*borrowed;
	unsigned long		*page_pool;
	struct svc_rqst		*rqstp;
	u32			capacity;
	u32			count;
	u32			body_bytes;
	u32			fixed_bytes;
	u32			borrowed_bytes;
	u32			copied_bytes;

	u32			materialized_bytes;
	u32			page_pool_bytes;
	u32			refs_acquired;
	u32			refs_released;
	u32			borrowed_count;
	u32			xid;
	u64			record_seq;
	u8			mode;
	u8			reason;
	bool			terminal;
	/* pages in the copy arena (rq_pages); @capacity counts bvecs */
	u32			arena_pages;
	/* region credit held by this record (svc_tcp_rx_credit_*) */
	u64			credit_ticket;
	u32			credit;
	u32			credit_reserve;
	u32			credit_over;
	/*
	 * region pages loaned, an upper bound on those held: a page counts
	 * again only after a loan of another one (@region_last is the last)
	 */
	u32			region_count;
	struct page		*region_last;
	bool			credit_linked;
	/* admitted from a CPU's stock: off the budget's in-flight list */
	bool			credit_stocked;
	struct list_head	credit_node;
	/* the budget this record's credit came from, while linked */
	struct svc_tcp_rx_budget *credit_budget;
	/*
	 * region pages and credit lent to a WRITE bounce
	 * (svc_rqst_bounce_payload), and the budget the credit returns to
	 */
	struct svc_tcp_rx_budget *bounce_budget;
	u32			bounce_pages;
	u32			bounce_credit;
	u32			bounce_reserve;
	/* lent from a stocked record: done returns it to a CPU's stock */
	bool			bounce_stocked;
} ____cacheline_aligned;

/*
 * RPC server socket.
 */
struct svc_sock {
	struct svc_xprt		sk_xprt;
	struct socket *		sk_sock;	/* berkeley socket layer */
	struct sock *		sk_sk;		/* INET layer */

	/* We keep the old state_change and data_ready CB's here */
	void			(*sk_ostate)(struct sock *);
	void			(*sk_odata)(struct sock *);
	void			(*sk_owspace)(struct sock *);

	/* For sends (protected by xpt_mutex) */
	struct bio_vec		*sk_bvec;

	/* private TCP part */
	/* On-the-wire fragment header: */
	__be32			sk_marker;
	/* As we receive a record, this includes the length received so
	 * far (including the fragment header): */
	u32			sk_tcplen;
	/* Total length of the data (not including fragment headers)
	 * received so far in the fragments making up this rpc: */
	u32			sk_datalen;
	u64			sk_rx_sequence;
	struct svc_tcp_rx_state	*sk_rx;
	/* waiting for region credit, ordered by sk_credit_ticket */
	struct list_head	sk_credit_wait;
	u64			sk_credit_ticket;
	/* credit a refused admission asked for; 0 while paused in a charge */
	u32			sk_credit_need;
	/* the budget whose waiters @sk_credit_wait is on */
	struct svc_tcp_rx_budget *sk_budget;
	/*
	 * What an incomplete loaned record still lacks after a read that
	 * drained the socket, or 0: svc_data_ready() wakes no thread for
	 * less (svc_tcp_rx_set_want())
	 */
	u32			sk_rx_want;
	/* region pages held by this connection's confined READ replies */
	atomic_t		sk_rx_region_inflight;
	bool			sk_rx_region_stalled;	/* at its share */
	/* READs parked until the connection's share has room */
	spinlock_t		sk_rx_region_lock;
	struct list_head	sk_rx_region_waiters;
	/* region pages seen: hold the receive buffer (set by the actor) */
	bool			sk_rx_hold_pending;
	/* receive buffer, window clamp and scaling ratio held */
	bool			sk_rx_held;
	/* what they were held to (svc_tcp_rx_hold_factor()) */
	unsigned int		sk_rx_hold_factor;
	/*
	 * What the skbs read since the last sample occupy and carry, and
	 * the last two samples, newest first, as bytes occupied for each
	 * 100 of payload (svc_tcp_rx_geometry_account())
	 */
	u32			sk_rx_geo_mem;
	u32			sk_rx_geo_pay;
	unsigned int		sk_rx_geo_factor[2];

	struct page_frag_cache  sk_frag_cache;

	struct completion	sk_handshake_done;

	/* received data */
	unsigned int		sk_spare_pages;	/* left by a restore, for a save */
	unsigned long		sk_maxpages;
	struct page *		sk_pages[] __counted_by(sk_maxpages);
};

static inline u32 svc_sock_reclen(struct svc_sock *svsk)
{
	return be32_to_cpu(svsk->sk_marker) & RPC_FRAGMENT_SIZE_MASK;
}

static inline u32 svc_sock_final_rec(struct svc_sock *svsk)
{
	return be32_to_cpu(svsk->sk_marker) & RPC_LAST_STREAM_FRAGMENT;
}

/*
 * Function prototypes.
 */
int		svc_recv(struct svc_rqst *rqstp, long timeo);
void		svc_send(struct svc_rqst *rqstp);
int		svc_addsock(struct svc_serv *serv, struct net *net,
			    const int fd, char *name_return, const size_t len,
			    const struct cred *cred);
void		svc_init_xprt_sock(void);
void		svc_cleanup_xprt_sock(void);
struct svc_tcp_rx_state *svc_tcp_rx_state_alloc(unsigned long capacity,
						gfp_t gfp, int node);
struct svc_tcp_rx_state *svc_tcp_rx_state_alloc_entries(unsigned long entries,
							unsigned long capacity,
							gfp_t gfp, int node);
void svc_tcp_rx_state_free(struct svc_tcp_rx_state *state);
struct svc_tcp_rx_budget *svc_tcp_rx_budget_read_for(struct svc_sock *svsk);

#if IS_ENABLED(CONFIG_KUNIT)
extern unsigned int svc_tcp_rx_copy_below;

enum svc_tcp_rx_kunit_fault {
	SVC_TCP_RX_KUNIT_FAULT_NONE,
	SVC_TCP_RX_KUNIT_FAULT_APPEND,
	SVC_TCP_RX_KUNIT_FAULT_MATERIALIZE,
};

int svc_tcp_rx_append_borrowed(struct svc_tcp_rx_state *state,
			       struct page *page, unsigned int offset,
			       unsigned int len, bool page_pool);
int svc_tcp_rx_append_copied(struct svc_tcp_rx_state *state,
			     struct page *page, unsigned int offset,
			     unsigned int len);
int svc_tcp_rx_materialize(struct svc_tcp_rx_state *state,
			   struct page **pages);
int svc_tcp_rx_release_refs(struct svc_tcp_rx_state *state);
void svc_tcp_rx_reset(struct svc_tcp_rx_state *state);
int svc_tcp_rx_exchange(struct svc_tcp_rx_state **active,
			struct svc_tcp_rx_state **empty);
int svc_tcp_rx_abort(struct svc_sock *svsk, struct svc_rqst *rqstp,
		     struct svc_tcp_rx_state *state,
		     enum svc_tcp_rx_action action,
		     enum svc_tcp_rx_reason reason);
void svc_tcp_release_ctxt(struct svc_xprt *xprt, void *ctxt);
int svc_tcp_rx_publish(struct svc_sock *svsk, struct svc_rqst *rqstp);
int svc_tcp_recv_actor_kunit(struct svc_rqst *rqstp, unsigned int *frags,
			     bool *complete, read_descriptor_t *desc,
			     struct sk_buff *skb, unsigned int offset,
			     size_t len);
int svc_tcp_restore_pages(struct svc_sock *svsk, struct svc_rqst *rqstp);
int svc_tcp_save_pages(struct svc_sock *svsk, struct svc_rqst *rqstp);
void svc_tcp_clear_pages(struct svc_sock *svsk);
int svc_tcp_recvfrom(struct svc_rqst *rqstp);
void svc_tcp_recv_record_done(struct svc_sock *svsk);
void svc_tcp_rx_kunit_fault_set(enum svc_tcp_rx_kunit_fault fault,
				unsigned int skip);
void svc_tcp_rx_kunit_fault_observed(u32 *acquired, u32 *released);
struct kernel_param;
void svc_tcp_rx_budget_kunit_configure(unsigned int pages,
				       unsigned int reserve,
				       unsigned int factor);
void svc_tcp_rx_budget_kunit_rcvbuf(unsigned int bytes);
void svc_tcp_rx_budget_kunit_default(unsigned int pages, unsigned int reserve);
int svc_tcp_rx_budget_kunit_set(unsigned int pages);
void svc_tcp_rx_kunit_save(void);
void svc_tcp_rx_kunit_restore(void);
unsigned long svc_tcp_spare_kunit_set_max(unsigned long bytes);
long svc_tcp_spare_kunit_pages(void);
void svc_tcp_rx_budget_kunit_region(unsigned long pages,
				    unsigned long read_pages);
struct svc_tcp_rx_budget *svc_tcp_rx_budget_kunit_read(void);
void svc_tcp_rx_budget_kunit_init(void);
bool svc_tcp_rx_budget_on(struct svc_tcp_rx_budget *c);
int svc_tcp_rx_budget_kunit_stats(struct svc_tcp_rx_budget *c, char *buf,
				  size_t size);
long svc_tcp_rx_budget_default_avail(void);
bool svc_tcp_rx_credit_admit(struct svc_sock *svsk,
			     struct svc_tcp_rx_state *state, u32 reclen);
bool svc_tcp_rx_credit_charge(struct svc_sock *svsk,
			      struct svc_tcp_rx_state *state, u32 need);
void svc_tcp_rx_credit_release(struct svc_tcp_rx_state *state);
void svc_tcp_rx_credit_forget(struct svc_sock *svsk);
bool svc_tcp_rx_budget_copy(struct svc_sock *svsk);
long svc_tcp_rx_budget_avail(void);
unsigned int svc_tcp_rx_budget_cost_factor(void);
long svc_tcp_rx_budget_reserve_avail(void);
void svc_tcp_rx_release_payload(struct svc_sock *svsk, struct svc_rqst *rqstp);
int svc_tcp_rx_bounce(struct svc_sock *svsk, struct svc_rqst *rqstp,
		      const struct xdr_buf *payload, unsigned int align,
		      unsigned int mem_align, struct bio_vec *out,
		      unsigned int out_size);
void svc_tcp_rx_bounce_done(struct svc_rqst *rqstp, struct bio_vec *bvec,
			    unsigned int nvecs);
void svc_data_ready(struct sock *sk);
u32 svc_tcp_rx_skb_region_pages(const struct sk_buff *skb, u32 base,
				u32 first, u32 last);
int svc_tcp_rx_window_clamp(int rcvbuf, unsigned int factor);
void svc_tcp_rx_geometry_account(struct svc_sock *svsk, unsigned int truesize,
				 unsigned int skb_len, unsigned int used);
unsigned int svc_tcp_rx_hold_factor(struct svc_sock *svsk);
struct svc_rx_region_park;
struct ubuf_info;
int svc_tcp_rx_region_read(struct svc_rqst *rqstp, unsigned int pages);
void svc_tcp_rx_region_unconfine(struct svc_rqst *rqstp);
void svc_tcp_rx_region_park_free(struct svc_rx_region_park *park);
void svc_tcp_rx_region_unconfine_op(struct svc_rqst *rqstp, bool served);
void svc_rx_region_share_stall(struct svc_sock *svsk, unsigned int share,
			       unsigned int held, __be32 xid);
void svc_tcp_rx_region_kunit_read_region(struct page **pages, unsigned int n);
unsigned int svc_tcp_rx_region_kunit_read_free(void);
struct ubuf_info *svc_tcp_rx_region_kunit_send(struct svc_rqst *rqstp,
					       unsigned int skbs);
void svc_tcp_rx_region_kunit_flush(void);
void svc_tcp_rx_region_kunit_reads(bool on);
int svc_tcp_rx_region_kunit_has_wspace(struct svc_sock *svsk);
void svc_tcp_rx_region_kunit_escape(unsigned int n);
void svc_tcp_rx_region_kunit_conn_drain(struct svc_sock *svsk);
void svc_tcp_rx_region_kunit_age(struct ubuf_info *ubuf);
void svc_tcp_rx_region_kunit_drain_waiters(void);
enum svc_tcp_rx_kunit_stats {
	SVC_TCP_RX_STATS_READ,		/* the budget READs on a transport draw on */
	SVC_TCP_RX_STATS_INIT,		/* the initial namespace's */
	SVC_TCP_RX_STATS_DEFAULT,
	SVC_TCP_RX_STATS_POOL,		/* the global read pool */
};
struct svc_tcp_rx_budget *
svc_tcp_rx_region_kunit_budget(struct svc_sock *svsk,
			       enum svc_tcp_rx_kunit_stats which);
bool svc_rx_region_send_bvec(const struct svc_rqst *rqstp, unsigned int nvecs,
			     unsigned int k);
u8 svc_tcp_rx_scaling_ratio(unsigned int factor);
void svc_tcp_rx_hold_buffer(struct svc_sock *svsk);
void svc_tcp_rx_set_want(struct svc_sock *svsk,
			 const struct svc_tcp_rx_state *state, bool drained);
#endif

/*
 * svc_makesock socket characteristics
 */
#define SVC_SOCK_DEFAULTS	(0U)
#define SVC_SOCK_ANONYMOUS	(1U << 0)	/* don't register with pmap */
#define SVC_SOCK_TEMPORARY	(1U << 1)	/* flag socket as temporary */

#endif /* SUNRPC_SVCSOCK_H */
