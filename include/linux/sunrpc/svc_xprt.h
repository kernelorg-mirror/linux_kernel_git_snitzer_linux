/* SPDX-License-Identifier: GPL-2.0 */
/*
 * linux/include/linux/sunrpc/svc_xprt.h
 *
 * RPC server transport I/O
 */

#ifndef SUNRPC_SVC_XPRT_H
#define SUNRPC_SVC_XPRT_H

#include <linux/sunrpc/svc.h>
#include <linux/llist.h>
#include <linux/workqueue.h>

struct module;

struct svc_xprt_ops {
	struct svc_xprt	*(*xpo_create)(struct svc_serv *,
				       struct net *net,
				       struct sockaddr *, int,
				       int);
	struct svc_xprt	*(*xpo_accept)(struct svc_xprt *);
	int		(*xpo_has_wspace)(struct svc_xprt *);
	int		(*xpo_recvfrom)(struct svc_rqst *);
	int		(*xpo_sendto)(struct svc_rqst *);
	/* xpo_sendto() for a caller that holds xpt_mutex (rtc send FIFO) */
	int		(*xpo_sendto_locked)(struct svc_rqst *);
	int		(*xpo_result_payload)(struct svc_rqst *, unsigned int,
					      unsigned int);
	void		(*xpo_release_ctxt)(struct svc_xprt *xprt, void *ctxt);
	void		(*xpo_release_payload)(struct svc_rqst *rqstp);
	int		(*xpo_bounce_payload)(struct svc_rqst *rqstp,
					      const struct xdr_buf *payload,
					      unsigned int align,
					      unsigned int mem_align,
					      struct bio_vec *bvec,
					      unsigned int bvec_size);
	void		(*xpo_bounce_done)(struct svc_rqst *rqstp,
					   struct bio_vec *bvec,
					   unsigned int nvecs);
	int		(*xpo_rx_region_read)(struct svc_rqst *rqstp,
					      unsigned int pages);
	void		(*xpo_rx_region_unconfine)(struct svc_rqst *rqstp,
						   bool served);
	void		(*xpo_rx_region_park_free)(struct svc_rx_region_park *park);
	void		(*xpo_detach)(struct svc_xprt *);
	void		(*xpo_free)(struct svc_xprt *);
	void		(*xpo_kill_temp_xprt)(struct svc_xprt *);
	void		(*xpo_handshake)(struct svc_xprt *xprt);
	/*
	 * Read the next call's program, version and procedure without
	 * consuming it.  Called with XPT_BUSY held; must not sleep.
	 * Returns 0, or -EAGAIN when the header is not at hand.
	 */
	int		(*xpo_peek_call)(struct svc_xprt *xprt, u32 *prog,
					   u32 *vers, u32 *proc, u32 *flavor);
};

struct svc_xprt_class {
	const char		*xcl_name;
	struct module		*xcl_owner;
	const struct svc_xprt_ops *xcl_ops;
	struct list_head	xcl_list;
	u32			xcl_max_payload;
	int			xcl_ident;
	u32			xcl_flags;
/* Set only on classes whose xpo_has_wspace() reads xpt_reserved */
#define SVC_XPRT_FLAG_WSPACE_RESERVE	BIT(0)
};

/*
 * This is embedded in an object that wants a callback before deleting
 * an xprt; intended for use by NFSv4.1, which needs to know when a
 * client's tcp connection (and hence possibly a backchannel) goes away.
 */
struct svc_xpt_user {
	struct list_head list;
	void (*callback)(struct svc_xpt_user *);
};

struct svc_xprt {
	struct svc_xprt_class	*xpt_class;
	const struct svc_xprt_ops *xpt_ops;
	struct kref		xpt_ref;
	ktime_t			xpt_qtime;
	struct list_head	xpt_list;
	struct lwq_node		xpt_ready;
	unsigned long		xpt_flags;

	struct svc_serv		*xpt_server;	/* service for transport */
	atomic_t		xpt_reserved;	/* outq space rsvd, UDP only */
	atomic_t		xpt_nr_rqsts;	/* Number of requests */
	struct mutex		xpt_mutex;	/* to serialize sending data */
	spinlock_t		xpt_lock;	/* protects sk_deferred
						 * and xpt_auth_cache */
	void			*xpt_auth_cache;/* auth cache */
	struct list_head	xpt_deferred;	/* deferred requests that need
						 * to be revisted */
	struct sockaddr_storage	xpt_local;	/* local address */
	size_t			xpt_locallen;	/* length of address */
	struct sockaddr_storage	xpt_remote;	/* remote peer's address */
	size_t			xpt_remotelen;	/* length of address */
	char			xpt_remotebuf[INET6_ADDRSTRLEN + 10];
	struct list_head	xpt_users;	/* callbacks on free */

	struct net		*xpt_net;
	netns_tracker		ns_tracker;
	const struct cred	*xpt_cred;
	struct rpc_xprt		*xpt_bc_xprt;	/* NFSv4.1 backchannel */
	struct rpc_xprt_switch	*xpt_bc_xps;	/* NFSv4.1 backchannel */
	struct svc_rtc_conn	*xpt_rtc;	/* run-to-completion work item */
};

/*
 * Run-to-completion (rtc) offer hook.
 *
 * An upper layer (nfsd) may register one hook per svc_serv.  While it is
 * registered, each connected TCP transport gets a per-connection work item
 * that runs on the CPU that received its data.  svc_xprt_enqueue() queues
 * that work item instead of waking a thread; the work item offers the
 * transport to the hook and, when the hook declines, hands the transport to
 * a thread exactly as svc_xprt_enqueue() would have.  A call the hook
 * serves is received, decoded and submitted (lookup-only, IOCB_NOWAIT) on
 * the work item; its reply is encoded on the work item after
 * svc_rtc_complete() and sent through the per-connection FIFO, one
 * xpt_mutex hold per batch.  A lookup miss or -EAGAIN hands the received
 * call to an nfsd thread (svc_pool.sp_rtc_rqsts), which reruns it.
 */
enum svc_rtc_reason {
	SVC_RTC_FB_NO_HOOK,		/* no hook registered */
	SVC_RTC_FB_XPRT_STATE,		/* close, conn, handshake, deferred */
	SVC_RTC_FB_STOPPING,		/* transport being torn down */
	SVC_RTC_FB_DECLINED,		/* hook declined the transport */
	SVC_RTC_FB_NO_PEEK,		/* call header not peekable (partial, TLS) */
	SVC_RTC_FB_NOT_FAST,		/* hook does not serve this call */
	SVC_RTC_FB_POOL_EMPTY,		/* every pool rqst is in use */
	SVC_RTC_FB_NO_SLOT,		/* per-connection request limit */
	SVC_RTC_FB_NO_MEM,		/* receive pages not allocated (GFP_NOWAIT) */
	SVC_RTC_FB_AUTH,		/* peeked cred flavor not the hook's */
	SVC_RTC_FB_MISS,		/* cache lookup would upcall or wait */
	SVC_RTC_FB_FILE_MISS,		/* no open file in the file cache */
	SVC_RTC_FB_UNALIGNED,		/* not a direct-I/O-able READ */
	SVC_RTC_FB_EAGAIN_SUBMIT,	/* IOCB_NOWAIT submit said -EAGAIN */
	SVC_RTC_FB_EAGAIN_COMPLETE,	/* async completion said -EAGAIN */
	SVC_RTC_FB_REGION_PARK,		/* READ payload region would park */
	SVC_RTC_FB_WRITE_MODE,		/* WRITE the thread path does differently */
	SVC_RTC_NR_REASONS
};

enum svc_rtc_verdict {
	SVC_RTC_THREAD,			/* hand the transport to a thread */
	SVC_RTC_IDLE,			/* nothing more to do */
	SVC_RTC_REQUEUE,		/* budget used up: run again */
	SVC_RTC_FAST,			/* serve the next call on a work item */
};

struct svc_rtc_ops {
	/* RPC auth flavor the hook serves; others go to a thread unreceived */
	u32	flavor;
	/*
	 * Called on the work item with XPT_BUSY held and nothing read from
	 * the transport.  Returns true to take the next record on the fast
	 * path.  Must not sleep and must have no side effect when it
	 * returns false.
	 */
	bool	(*want)(struct svc_xprt *xprt);
	/*
	 * Called after want() with the peeked call header.  Returns true
	 * to receive and process the call on the work item.  Must not sleep.
	 */
	bool	(*want_call)(struct svc_xprt *xprt, u32 prog, u32 vers,
			     u32 proc);
	/* Set up / tear down a pool rqst's upper-layer state (may sleep). */
	int	(*rqst_init)(struct svc_rqst *rqstp);
	void	(*rqst_free)(struct svc_rqst *rqstp);
	/* After processing on a work item or thread, before release. */
	void	(*rqst_done)(struct svc_rqst *rqstp);
	/*
	 * On the work item, after svc_rtc_complete(): finish an
	 * RQ_RTC_ASYNC call.  Returns the dispatch result (1 send, 0 drop),
	 * or -EAGAIN to hand the call to a thread.
	 */
	int	(*complete)(struct svc_rqst *rqstp);
};

struct svc_rtc_send;
typedef void (*svc_rtc_send_fn)(struct svc_xprt *xprt,
				struct svc_rtc_send *send, bool dead);

/*
 * One queued reply.  @fn sends it with xpt_mutex held, checking
 * svc_xprt_is_dead() under the mutex as svc_tcp_sendto() does, or, when
 * @dead is true, releases it without sending (no mutex held).
 */
struct svc_rtc_send {
	struct llist_node	node;
	svc_rtc_send_fn		fn;
};

#define SVC_RTC_SEND_BUDGET	8
#define SVC_RTC_IO_WORK_BUDGET	64

struct svc_rtc_conn {
	struct work_struct	work;
	struct svc_xprt		*xprt;
	int			cpu;	/* CPU of the latest receive kick */
	unsigned long		flags;
	struct llist_head	sendq;
	/* FIFO replies taken from @sendq not yet sent; work item only */
	struct llist_node	*pending;
	/* async calls completed (svc_rtc_complete(), any context) */
	struct llist_head	done;
};

/* Pool rqsts per service: at most one per nfsd thread, and this many. */
#define SVC_RTC_POOL_CAP	64

struct svc_rtc_rqst {
	struct lwq_node		qnode;	/* pool free list or sp_rtc_rqsts */
	struct llist_node	node;	/* conn->done */
	struct svc_rtc_send	send;	/* reply on conn->sendq */
	struct svc_rtc_conn	*conn;	/* while in use on a connection */
	struct svc_rqst		*rqstp;
	struct list_head	all;
	bool			queued;	/* reply on the send FIFO */
};

struct svc_rtc_pool {
	const struct svc_rtc_ops *ops;
	struct lwq		free;
	struct list_head	all;
	unsigned int		nr;
};

/* svc_rtc_conn.flags */
enum {
	SVC_RTC_RECV,			/* work item owns XPT_BUSY */
	SVC_RTC_STOPPING,		/* svc_rtc_xprt_stop() ran */
};

struct svc_rtc_stats {
	u64	diverted;
	u64	runs;
	u64	sends;
	u64	send_batches;
	u64	budget_exhausted;
	u64	read_offered;	/* calls the hook wants (READ) */
	u64	fast_completed;	/* replies sent from a work item */
	u64	async_submitted;
	u64	recv_empty;
	u64	fallback[SVC_RTC_NR_REASONS];
};

int	svc_rtc_register(struct svc_serv *serv, const struct svc_rtc_ops *ops);
void	svc_rtc_unregister(struct svc_serv *serv);
void	svc_rtc_queue_send(struct svc_xprt *xprt, struct svc_rtc_send *send);
void	svc_rtc_stats_sum(struct svc_rtc_stats *sum);
void	svc_rtc_serv_destroy(struct svc_serv *serv);
void	svc_rtc_complete(struct svc_rqst *rqstp);
#if IS_ENABLED(CONFIG_SUNRPC_SVC_RTC_KUNIT_TEST)
extern void (*svc_rtc_kunit_published)(struct svc_xprt *xprt);
extern bool svc_rtc_kunit_no_mem;	/* KUnit: fail the receive pages */
extern bool svc_rtc_kunit_no_slot;	/* KUnit: fail the request slot */
#endif
void	svc_rtc_pool_resize(struct svc_serv *serv);
struct seq_file;
void	svc_rtc_stats_show(struct seq_file *m);
#if IS_ENABLED(CONFIG_KUNIT)
int	svc_rtc_serv_prepare(struct svc_serv *serv);
int	svc_rtc_xprt_attach(struct svc_xprt *xprt, gfp_t gfp);
void	svc_rtc_xprt_stop(struct svc_xprt *xprt);
void	svc_rtc_xprt_detach(struct svc_xprt *xprt);
bool	svc_rtc_should_divert(struct svc_xprt *xprt);
enum svc_rtc_verdict svc_rtc_recv_step(struct svc_xprt *xprt,
				       enum svc_rtc_reason *reason,
				       const struct svc_rtc_ops **opsp);
bool	svc_rtc_thread_serve(struct svc_rqst *rqstp, struct svc_xprt *xprt);
unsigned int svc_rtc_drain_sends(struct svc_rtc_conn *conn, bool dead);
int	svc_rtc_pool_set(struct svc_serv *serv, unsigned int nr);
bool	svc_rtc_thread_run(struct svc_rqst *thr);
bool	svc_rtc_thread_steal(struct svc_rqst *thr);
#endif

/* Abandon the fast path for @rqstp: a thread reruns the call. */
static inline void svc_rtc_miss(struct svc_rqst *rqstp,
				enum svc_rtc_reason reason)
{
	if (!test_and_set_bit(RQ_RTC_MISS, &rqstp->rq_flags))
		rqstp->rq_rtc_reason = reason;
}

/* flag bits for xpt_flags */
enum {
	XPT_BUSY,		/* enqueued/receiving */
	XPT_CONN,		/* conn pending */
	XPT_CLOSE,		/* dead or dying */
	XPT_DATA,		/* data pending */
	XPT_TEMP,		/* connected transport */
	XPT_DEAD,		/* transport closed */
	XPT_CHNGBUF,		/* need to change snd/rcv buf sizes */
	XPT_DEFERRED,		/* deferred request pending */
	XPT_OLD,		/* used for xprt aging mark+sweep */
	XPT_LISTENER,		/* listening endpoint */
	XPT_CACHE_AUTH,		/* cache auth info */
	XPT_LOCAL,		/* connection from loopback interface */
	XPT_KILL_TEMP,		/* call xpo_kill_temp_xprt before closing */
	XPT_CONG_CTRL,		/* has congestion control */
	XPT_HANDSHAKE,		/* xprt requests a handshake */
	XPT_TLS_SESSION,	/* transport-layer security established */
	XPT_PEER_AUTH,		/* peer has been authenticated */
	XPT_PEER_VALID,		/* peer has presented a filehandle that
				 * it has access to.  It is NOT counted
				 * in ->sv_tmpcnt.
				 */
	XPT_RPCB_UNREG,		/* transport that needs unregistering
				 * with rpcbind (TCP, UDP) on destroy
				 */
};

/*
 * Maximum number of "tmp" connections - those without XPT_PEER_VALID -
 * permitted on any service.
 */
#define XPT_MAX_TMP_CONN	64

static inline void svc_xprt_set_valid(struct svc_xprt *xpt)
{
	if (test_bit(XPT_TEMP, &xpt->xpt_flags) &&
	    !test_and_set_bit(XPT_PEER_VALID, &xpt->xpt_flags)) {
		struct svc_serv *serv = xpt->xpt_server;

		spin_lock(&serv->sv_lock);
		serv->sv_tmpcnt -= 1;
		spin_unlock(&serv->sv_lock);
	}
}

static inline void unregister_xpt_user(struct svc_xprt *xpt, struct svc_xpt_user *u)
{
	spin_lock(&xpt->xpt_lock);
	list_del_init(&u->list);
	spin_unlock(&xpt->xpt_lock);
}

static inline int register_xpt_user(struct svc_xprt *xpt, struct svc_xpt_user *u)
{
	spin_lock(&xpt->xpt_lock);
	if (test_bit(XPT_CLOSE, &xpt->xpt_flags)) {
		/*
		 * The connection is about to be deleted soon (or,
		 * worse, may already be deleted--in which case we've
		 * already notified the xpt_users).
		 */
		spin_unlock(&xpt->xpt_lock);
		return -ENOTCONN;
	}
	list_add(&u->list, &xpt->xpt_users);
	spin_unlock(&xpt->xpt_lock);
	return 0;
}

static inline bool svc_xprt_is_dead(const struct svc_xprt *xprt)
{
	return (test_bit(XPT_DEAD, &xprt->xpt_flags) != 0) ||
		(test_bit(XPT_CLOSE, &xprt->xpt_flags) != 0);
}

int	svc_reg_xprt_class(struct svc_xprt_class *);
void	svc_unreg_xprt_class(struct svc_xprt_class *);
void	svc_xprt_init(struct net *, struct svc_xprt_class *, struct svc_xprt *,
		      struct svc_serv *);
int	svc_xprt_create_from_sa(struct svc_serv *serv, const char *xprt_name,
				struct net *net, struct sockaddr *sap,
				int flags, const struct cred *cred);
int	svc_xprt_create(struct svc_serv *serv, const char *xprt_name,
			struct net *net, const int family,
			const unsigned short port, int flags,
			const struct cred *cred);
void	svc_xprt_destroy_all(struct svc_serv *serv, struct net *net,
			     bool unregister);
void	svc_xprt_received(struct svc_xprt *xprt);
void	svc_xprt_enqueue(struct svc_xprt *xprt);
void	svc_xprt_put(struct svc_xprt *xprt);
void	svc_xprt_copy_addrs(struct svc_rqst *rqstp, struct svc_xprt *xprt);
void	svc_xprt_close(struct svc_xprt *xprt);
int	svc_port_is_privileged(struct sockaddr *sin);
int	svc_print_xprts(char *buf, int maxlen);
struct svc_xprt *svc_find_listener(struct svc_serv *serv, const char *xcl_name,
				   struct net *net, const struct sockaddr *sa);
struct	svc_xprt *svc_find_xprt(struct svc_serv *serv, const char *xcl_name,
			struct net *net, const sa_family_t af,
			const unsigned short port);
int	svc_xprt_names(struct svc_serv *serv, char *buf, const int buflen);
void	svc_add_new_perm_xprt(struct svc_serv *serv, struct svc_xprt *xprt);
void	svc_age_temp_xprts_now(struct svc_serv *, struct sockaddr *);
void	svc_xprt_deferred_close(struct svc_xprt *xprt);
struct svc_deferred_req *svc_defer_rx_region(struct svc_rqst *rqstp);
void	svc_revisit_rx_region(struct svc_deferred_req *dr);

static inline void svc_xprt_get(struct svc_xprt *xprt)
{
	kref_get(&xprt->xpt_ref);
}
static inline void svc_xprt_set_local(struct svc_xprt *xprt,
				      const struct sockaddr *sa,
				      const size_t salen)
{
	memcpy(&xprt->xpt_local, sa, salen);
	xprt->xpt_locallen = salen;
}
static inline void svc_xprt_set_remote(struct svc_xprt *xprt,
				       const struct sockaddr *sa,
				       const size_t salen)
{
	memcpy(&xprt->xpt_remote, sa, salen);
	xprt->xpt_remotelen = salen;
	snprintf(xprt->xpt_remotebuf, sizeof(xprt->xpt_remotebuf) - 1,
		 "%pISpc", sa);
}

static inline unsigned short svc_addr_port(const struct sockaddr *sa)
{
	const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
	const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)sa;

	switch (sa->sa_family) {
	case AF_INET:
		return ntohs(sin->sin_port);
	case AF_INET6:
		return ntohs(sin6->sin6_port);
	}

	return 0;
}

static inline size_t svc_addr_len(const struct sockaddr *sa)
{
	switch (sa->sa_family) {
	case AF_INET:
		return sizeof(struct sockaddr_in);
	case AF_INET6:
		return sizeof(struct sockaddr_in6);
	}
	BUG();
}

static inline unsigned short svc_xprt_local_port(const struct svc_xprt *xprt)
{
	return svc_addr_port((const struct sockaddr *)&xprt->xpt_local);
}

static inline unsigned short svc_xprt_remote_port(const struct svc_xprt *xprt)
{
	return svc_addr_port((const struct sockaddr *)&xprt->xpt_remote);
}

static inline char *__svc_print_addr(const struct sockaddr *addr,
				     char *buf, const size_t len)
{
	const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
	const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)addr;

	switch (addr->sa_family) {
	case AF_INET:
		snprintf(buf, len, "%pI4, port=%u", &sin->sin_addr,
			ntohs(sin->sin_port));
		break;

	case AF_INET6:
		snprintf(buf, len, "%pI6, port=%u",
			 &sin6->sin6_addr,
			ntohs(sin6->sin6_port));
		break;

	default:
		snprintf(buf, len, "unknown address type: %d", addr->sa_family);
		break;
	}

	return buf;
}
#endif /* SUNRPC_SVC_XPRT_H */
