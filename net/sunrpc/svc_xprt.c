// SPDX-License-Identifier: GPL-2.0-only
/*
 * linux/net/sunrpc/svc_xprt.c
 *
 * Author: Tom Tucker <tom@opengridcomputing.com>
 */

#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/clock.h>
#include <kunit/visibility.h>
#include <linux/errno.h>
#include <linux/freezer.h>
#include <linux/slab.h>
#include <net/sock.h>
#include <linux/sunrpc/addr.h>
#include <linux/sunrpc/stats.h>
#include <linux/sunrpc/svc_xprt.h>
#include <linux/sunrpc/svcsock.h>
#include <linux/sunrpc/xprt.h>
#include <linux/sunrpc/bc_xprt.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <trace/events/sunrpc.h>

#define RPCDBG_FACILITY	RPCDBG_SVCXPRT

static unsigned int svc_rpc_per_connection_limit __read_mostly;
module_param(svc_rpc_per_connection_limit, uint, 0644);


static struct svc_deferred_req *svc_deferred_dequeue(struct svc_xprt *xprt);
static int svc_deferred_recv(struct svc_rqst *rqstp);
static struct cache_deferred_req *svc_defer(struct cache_req *req);
static void svc_age_temp_xprts(struct timer_list *t);
static void svc_delete_xprt(struct svc_xprt *xprt);

/* apparently the "standard" is that clients close
 * idle connections after 5 minutes, servers after
 * 6 minutes
 *   http://nfsv4bat.org/Documents/ConnectAThon/1996/nfstcp.pdf
 */
static int svc_conn_age_period = 6*60;

/* List of registered transport classes */
static DEFINE_SPINLOCK(svc_xprt_class_lock);
static LIST_HEAD(svc_xprt_class_list);

/* SMP locking strategy:
 *
 *	svc_serv->sv_lock protects sv_tempsocks, sv_permsocks, sv_tmpcnt.
 *	when both need to be taken (rare), svc_serv->sv_lock is first.
 *	The "service mutex" protects svc_serv->sv_nrthread.
 *	svc_sock->sk_lock protects the svc_sock->sk_deferred list
 *             and the ->sk_info_authunix cache.
 *
 *	The XPT_BUSY bit in xprt->xpt_flags prevents a transport being
 *	enqueued multiply. During normal transport processing this bit
 *	is set by svc_xprt_enqueue and cleared by svc_xprt_received.
 *	Providers should not manipulate this bit directly.
 *
 *	Some flags can be set to certain values at any time
 *	providing that certain rules are followed:
 *
 *	XPT_CONN, XPT_DATA:
 *		- Can be set or cleared at any time.
 *		- After a set, svc_xprt_enqueue must be called to enqueue
 *		  the transport for processing.
 *		- After clearing XPT_CONN, the transport must be
 *		  accepted. If this succeeds, the bit must be set again.
 *		- xpo_recvfrom decides when XPT_DATA is cleared; see
 *		  svc_xprt_received.
 *	XPT_CLOSE:
 *		- Can set at any time. It is never cleared.
 *      XPT_DEAD:
 *		- Can only be set while XPT_BUSY is held which ensures
 *		  that no other thread will be using the transport or will
 *		  try to set XPT_DEAD.
 */

/**
 * svc_reg_xprt_class - Register a server-side RPC transport class
 * @xcl: New transport class to be registered
 *
 * Returns zero on success; otherwise a negative errno is returned.
 */
int svc_reg_xprt_class(struct svc_xprt_class *xcl)
{
	struct svc_xprt_class *cl;
	int res = -EEXIST;

	INIT_LIST_HEAD(&xcl->xcl_list);
	spin_lock(&svc_xprt_class_lock);
	/* Make sure there isn't already a class with the same name */
	list_for_each_entry(cl, &svc_xprt_class_list, xcl_list) {
		if (strcmp(xcl->xcl_name, cl->xcl_name) == 0)
			goto out;
	}
	list_add_tail(&xcl->xcl_list, &svc_xprt_class_list);
	res = 0;
out:
	spin_unlock(&svc_xprt_class_lock);
	return res;
}
EXPORT_SYMBOL_GPL(svc_reg_xprt_class);

/**
 * svc_unreg_xprt_class - Unregister a server-side RPC transport class
 * @xcl: Transport class to be unregistered
 *
 */
void svc_unreg_xprt_class(struct svc_xprt_class *xcl)
{
	spin_lock(&svc_xprt_class_lock);
	list_del_init(&xcl->xcl_list);
	spin_unlock(&svc_xprt_class_lock);
}
EXPORT_SYMBOL_GPL(svc_unreg_xprt_class);

/**
 * svc_print_xprts - Format the transport list for printing
 * @buf: target buffer for formatted address
 * @maxlen: length of target buffer
 *
 * Fills in @buf with a string containing a list of transport names, each name
 * terminated with '\n'. If the buffer is too small, some entries may be
 * missing, but it is guaranteed that all lines in the output buffer are
 * complete.
 *
 * Returns positive length of the filled-in string.
 */
int svc_print_xprts(char *buf, int maxlen)
{
	struct svc_xprt_class *xcl;
	char tmpstr[80];
	int len = 0;
	buf[0] = '\0';

	spin_lock(&svc_xprt_class_lock);
	list_for_each_entry(xcl, &svc_xprt_class_list, xcl_list) {
		int slen;

		slen = snprintf(tmpstr, sizeof(tmpstr), "%s %d\n",
				xcl->xcl_name, xcl->xcl_max_payload);
		if (slen >= sizeof(tmpstr) || len + slen >= maxlen)
			break;
		len += slen;
		strcat(buf, tmpstr);
	}
	spin_unlock(&svc_xprt_class_lock);

	return len;
}

/**
 * svc_xprt_deferred_close - Close a transport
 * @xprt: transport instance
 *
 * Used in contexts that need to defer the work of shutting down
 * the transport to an nfsd thread.
 */
void svc_xprt_deferred_close(struct svc_xprt *xprt)
{
	trace_svc_xprt_close(xprt);
	if (!test_and_set_bit(XPT_CLOSE, &xprt->xpt_flags))
		svc_xprt_enqueue(xprt);
}
EXPORT_SYMBOL_GPL(svc_xprt_deferred_close);

static void svc_xprt_free(struct kref *kref)
{
	struct svc_xprt *xprt =
		container_of(kref, struct svc_xprt, xpt_ref);
	struct module *owner = xprt->xpt_class->xcl_owner;
	if (test_bit(XPT_CACHE_AUTH, &xprt->xpt_flags))
		svcauth_unix_info_release(xprt);
	put_cred(xprt->xpt_cred);
	put_net_track(xprt->xpt_net, &xprt->ns_tracker);
	/* See comment on corresponding get in xs_setup_bc_tcp(): */
	if (xprt->xpt_bc_xprt)
		xprt_put(xprt->xpt_bc_xprt);
	if (xprt->xpt_bc_xps)
		xprt_switch_put(xprt->xpt_bc_xps);
	trace_svc_xprt_free(xprt);
	xprt->xpt_ops->xpo_free(xprt);
	module_put(owner);
}

void svc_xprt_put(struct svc_xprt *xprt)
{
	kref_put(&xprt->xpt_ref, svc_xprt_free);
}
EXPORT_SYMBOL_GPL(svc_xprt_put);

/*
 * Called by transport drivers to initialize the transport independent
 * portion of the transport instance.
 */
void svc_xprt_init(struct net *net, struct svc_xprt_class *xcl,
		   struct svc_xprt *xprt, struct svc_serv *serv)
{
	memset(xprt, 0, sizeof(*xprt));
	xprt->xpt_class = xcl;
	xprt->xpt_ops = xcl->xcl_ops;
	kref_init(&xprt->xpt_ref);
	xprt->xpt_server = serv;
	INIT_LIST_HEAD(&xprt->xpt_list);
	INIT_LIST_HEAD(&xprt->xpt_deferred);
	INIT_LIST_HEAD(&xprt->xpt_users);
	mutex_init(&xprt->xpt_mutex);
	spin_lock_init(&xprt->xpt_lock);
	set_bit(XPT_BUSY, &xprt->xpt_flags);
	xprt->xpt_net = get_net_track(net, &xprt->ns_tracker, GFP_ATOMIC);
	strcpy(xprt->xpt_remotebuf, "uninitialized");
}
EXPORT_SYMBOL_GPL(svc_xprt_init);

/**
 * svc_xprt_received - start next receiver thread
 * @xprt: controlling transport
 *
 * The caller must hold the XPT_BUSY bit and must
 * not thereafter touch transport data.
 *
 * Note: xpo_recvfrom decides when to clear XPT_DATA. A transport may
 * leave the bit set until a read attempt finds no (or insufficient)
 * data, or clear it as soon as it consumes the last queued receive.
 */
void svc_xprt_received(struct svc_xprt *xprt)
{
	if (!test_bit(XPT_BUSY, &xprt->xpt_flags)) {
		WARN_ONCE(1, "xprt=0x%p already busy!", xprt);
		return;
	}

	/* As soon as we clear busy, the xprt could be closed and
	 * 'put', so we need a reference to call svc_xprt_enqueue with:
	 */
	svc_xprt_get(xprt);
	smp_mb__before_atomic();
	clear_bit(XPT_BUSY, &xprt->xpt_flags);
	/*
	 * Order the clear before the readiness checks below.  A waker that
	 * changes a condition xpo_has_wspace() reads, then (after a full
	 * barrier, e.g. the xchg() in svc_rx_region_share_unstall()) finds
	 * XPT_BUSY set and does not enqueue, relies on this enqueue.
	 */
	smp_mb__after_atomic();

	/*
	 * Skip the enqueue when no actionable flags are set.
	 * Each producer both sets its flag (XPT_DATA, XPT_CLOSE,
	 * etc.) and calls svc_xprt_enqueue(); if a set_bit races
	 * with this check, the producer's own enqueue observes
	 * !XPT_BUSY and dispatches the transport.
	 */
	if (READ_ONCE(xprt->xpt_flags) &
	    (BIT(XPT_CONN) | BIT(XPT_CLOSE) | BIT(XPT_HANDSHAKE) |
	     BIT(XPT_DATA) | BIT(XPT_DEFERRED)))
		svc_xprt_enqueue(xprt);

	svc_xprt_put(xprt);
}
EXPORT_SYMBOL_GPL(svc_xprt_received);

void svc_add_new_perm_xprt(struct svc_serv *serv, struct svc_xprt *new)
{
	clear_bit(XPT_TEMP, &new->xpt_flags);
	spin_lock_bh(&serv->sv_lock);
	list_add(&new->xpt_list, &serv->sv_permsocks);
	spin_unlock_bh(&serv->sv_lock);
	svc_xprt_received(new);
}

static int _svc_xprt_create(struct svc_serv *serv, const char *xprt_name,
			    struct net *net, struct sockaddr *sap,
			    size_t len, int flags, const struct cred *cred)
{
	struct svc_xprt_class *xcl;

	spin_lock(&svc_xprt_class_lock);
	list_for_each_entry(xcl, &svc_xprt_class_list, xcl_list) {
		struct svc_xprt *newxprt;
		unsigned short newport;

		if (strcmp(xprt_name, xcl->xcl_name))
			continue;

		if (!try_module_get(xcl->xcl_owner))
			goto err;

		spin_unlock(&svc_xprt_class_lock);
		newxprt = xcl->xcl_ops->xpo_create(serv, net, sap, len, flags);
		if (IS_ERR(newxprt)) {
			trace_svc_xprt_create_err(serv->sv_programs->pg_name,
						  xcl->xcl_name, sap, len,
						  newxprt);
			module_put(xcl->xcl_owner);
			return PTR_ERR(newxprt);
		}
		newxprt->xpt_cred = get_cred(cred);
		svc_add_new_perm_xprt(serv, newxprt);
		newport = svc_xprt_local_port(newxprt);
		return newport;
	}
 err:
	spin_unlock(&svc_xprt_class_lock);
	/* This errno is exposed to user space.  Provide a reasonable
	 * perror msg for a bad transport. */
	return -EPROTONOSUPPORT;
}

/**
 * svc_xprt_create_from_sa - Add a new listener to @serv from socket address
 * @serv: target RPC service
 * @xprt_name: transport class name
 * @net: network namespace
 * @sap: socket address pointer
 * @flags: SVC_SOCK flags
 * @cred: credential to bind to this transport
 *
 * Return local xprt port on success or %-EPROTONOSUPPORT on failure
 */
int svc_xprt_create_from_sa(struct svc_serv *serv, const char *xprt_name,
			    struct net *net, struct sockaddr *sap,
			    int flags, const struct cred *cred)
{
	size_t len;
	int err;

	switch (sap->sa_family) {
	case AF_INET:
		len = sizeof(struct sockaddr_in);
		break;
#if IS_ENABLED(CONFIG_IPV6)
	case AF_INET6:
		len = sizeof(struct sockaddr_in6);
		break;
#endif
	default:
		return -EAFNOSUPPORT;
	}

	err = _svc_xprt_create(serv, xprt_name, net, sap, len, flags, cred);
	if (err == -EPROTONOSUPPORT) {
		request_module("svc%s", xprt_name);
		err = _svc_xprt_create(serv, xprt_name, net, sap, len, flags,
				       cred);
	}

	return err;
}
EXPORT_SYMBOL_GPL(svc_xprt_create_from_sa);

/**
 * svc_xprt_create - Add a new listener to @serv
 * @serv: target RPC service
 * @xprt_name: transport class name
 * @net: network namespace
 * @family: network address family
 * @port: listener port
 * @flags: SVC_SOCK flags
 * @cred: credential to bind to this transport
 *
 * Return local xprt port on success or %-EPROTONOSUPPORT on failure
 */
int svc_xprt_create(struct svc_serv *serv, const char *xprt_name,
		    struct net *net, const int family,
		    const unsigned short port, int flags,
		    const struct cred *cred)
{
	struct sockaddr_in sin = {
		.sin_family		= AF_INET,
		.sin_addr.s_addr	= htonl(INADDR_ANY),
		.sin_port		= htons(port),
	};
#if IS_ENABLED(CONFIG_IPV6)
	struct sockaddr_in6 sin6 = {
		.sin6_family		= AF_INET6,
		.sin6_addr		= IN6ADDR_ANY_INIT,
		.sin6_port		= htons(port),
	};
#endif
	struct sockaddr *sap;

	switch (family) {
	case PF_INET:
		sap = (struct sockaddr *)&sin;
		break;
#if IS_ENABLED(CONFIG_IPV6)
	case PF_INET6:
		sap = (struct sockaddr *)&sin6;
		break;
#endif
	default:
		return -EAFNOSUPPORT;
	}

	return svc_xprt_create_from_sa(serv, xprt_name, net, sap, flags, cred);
}
EXPORT_SYMBOL_GPL(svc_xprt_create);

/*
 * Copy the local and remote xprt addresses to the rqstp structure
 */
void svc_xprt_copy_addrs(struct svc_rqst *rqstp, struct svc_xprt *xprt)
{
	memcpy(&rqstp->rq_addr, &xprt->xpt_remote, xprt->xpt_remotelen);
	rqstp->rq_addrlen = xprt->xpt_remotelen;

	/*
	 * Destination address in request is needed for binding the
	 * source address in RPC replies/callbacks later.
	 */
	memcpy(&rqstp->rq_daddr, &xprt->xpt_local, xprt->xpt_locallen);
	rqstp->rq_daddrlen = xprt->xpt_locallen;
}
EXPORT_SYMBOL_GPL(svc_xprt_copy_addrs);

/**
 * svc_print_addr - Format rq_addr field for printing
 * @rqstp: svc_rqst struct containing address to print
 * @buf: target buffer for formatted address
 * @len: length of target buffer
 *
 */
char *svc_print_addr(struct svc_rqst *rqstp, char *buf, size_t len)
{
	return __svc_print_addr(svc_addr(rqstp), buf, len);
}
EXPORT_SYMBOL_GPL(svc_print_addr);

static bool svc_xprt_slots_in_range(struct svc_xprt *xprt)
{
	unsigned int limit = svc_rpc_per_connection_limit;
	int nrqsts = atomic_read(&xprt->xpt_nr_rqsts);

	return limit == 0 || (nrqsts >= 0 && nrqsts < limit);
}

static bool svc_xprt_reserve_slot(struct svc_rqst *rqstp, struct svc_xprt *xprt)
{
	if (!test_bit(RQ_DATA, &rqstp->rq_flags)) {
		if (!svc_xprt_slots_in_range(xprt))
			return false;
		atomic_inc(&xprt->xpt_nr_rqsts);
		set_bit(RQ_DATA, &rqstp->rq_flags);
	}
	return true;
}

/*
 * After a caller releases write-space or a request slot,
 * re-enqueue the transport only when there is pending
 * work that a thread could act on.  The smp_mb() pairs
 * with the smp_rmb() in svc_xprt_ready() and orders the
 * preceding counter update before the flags read so a
 * concurrent set_bit(XPT_DATA) is visible here.
 *
 * When the transport is BUSY, the thread holding it will
 * call svc_xprt_received() upon completion, which checks
 * for pending work and re-enqueues as needed.
 */
static void svc_xprt_resource_released(struct svc_xprt *xprt)
{
	unsigned long xpt_flags;

	smp_mb();
	xpt_flags = READ_ONCE(xprt->xpt_flags);
	if (xpt_flags & (BIT(XPT_DATA) | BIT(XPT_DEFERRED)) &&
	    !(xpt_flags & BIT(XPT_BUSY)))
		svc_xprt_enqueue(xprt);
}

static void svc_xprt_release_slot(struct svc_rqst *rqstp)
{
	struct svc_xprt	*xprt = rqstp->rq_xprt;
	if (test_and_clear_bit(RQ_DATA, &rqstp->rq_flags)) {
		atomic_dec(&xprt->xpt_nr_rqsts);
		svc_xprt_resource_released(xprt);
	}
}

static bool svc_xprt_ready(struct svc_xprt *xprt)
{
	unsigned long xpt_flags;

	/*
	 * If another cpu has recently updated xpt_flags,
	 * sk_sock->flags, xpt_reserved (UDP only), or xpt_nr_rqsts,
	 * we need to know about it; otherwise it's possible that both
	 * that cpu and this one could call svc_xprt_enqueue() without
	 * either svc_xprt_enqueue() recognizing that the conditions
	 * below are satisfied, and we could stall indefinitely:
	 */
	smp_rmb();
	xpt_flags = READ_ONCE(xprt->xpt_flags);

	trace_svc_xprt_enqueue(xprt, xpt_flags);
	if (xpt_flags & BIT(XPT_BUSY))
		return false;
	if (xpt_flags & (BIT(XPT_CONN) | BIT(XPT_CLOSE) | BIT(XPT_HANDSHAKE)))
		return true;
	if (xpt_flags & (BIT(XPT_DATA) | BIT(XPT_DEFERRED))) {
		if (xprt->xpt_ops->xpo_has_wspace(xprt) &&
		    svc_xprt_slots_in_range(xprt))
			return true;
		trace_svc_xprt_no_write_space(xprt);
		return false;
	}
	return false;
}

/*
 * Sibling pools.  With one pool per cpu, receive steering can put the work
 * of a few connections on a few pools while the threads of the other pools
 * sleep.  svc_pool_ext_layout() chains the pools of a node that have
 * threads into a ring; each pool without threads hangs off the nearest
 * ring member, which serves it.  Two bounded mechanisms use it, both
 * starting with the SVC_POOL_STEAL_SPAN ring members on either side of a
 * pool:
 *
 *  - an enqueue that cannot claim an idle thread of its own pool (none is
 *    idle, or the first idle one is already claimed for earlier work)
 *    claims one of the nearest forward ring members that has one;
 *  - a thread whose own pool is empty takes a transport from the pools it
 *    serves, then from the nearest non-empty backward ring member.
 *
 * When the pool can reach other nodes (far), the wake goes on to its
 * node's idle map (svc_pool_idle_add()), which names the pools whose
 * idle list is not empty, and tries those nearest first; the steal walk
 * goes on over the rest of its node's ring, at most once per idle period
 * (svc_recv()).  Only when the node has no idle thread to claim, or no
 * work, do they go to the other nodes, one node at a time along far.
 * There they look at the pools serving the places near the relative
 * places of their own block (svc_pool_far_ranges()); that relation is
 * symmetric, so the walks still mirror on nodes of any size and with
 * sparse threads, and a steal leaves a remote queue whose own pool has an idle
 * thread.  So all pools with threads can serve arrivals that land on one
 * node, while a transport crosses nodes only when its own node has no
 * idle thread.
 *
 * The two walks mirror, so the thread a sibling wake claims looks at the
 * queue that needed it; a claimed thread that finds nothing just goes
 * idle again, and the transport stays queued for its own pool.  A
 * transport is on one queue at a time (XPT_BUSY) and lwq_dequeue() hands
 * it to one thread, so a steal can never process a transport twice.
 * pernode pools of different nodes are never in one ring and have no far
 * pool, so pernode behaves as before.
 */
#define SVC_POOL_STEAL_SPAN	8U

/*
 * The idle maps.  Each node's run of pools has a bitmap, from a cache
 * line of its own, with one bit per pool (ext->place), set while that
 * pool's idle list is not empty.  Only the list's transitions write it:
 * svc_pool_idle_add() when it makes the list non-empty, and
 * svc_pool_idle_del() when it leaves it empty.  An enqueue or a claim
 * never writes it, so a saturated node, whose idle lists are empty, has
 * a clean, empty map that its wakes read in place of every idle list.
 *
 * The map is a hint, and a stale bit costs work, never a transport.  A
 * set bit whose pool has no idle thread to claim (the first one is
 * claimed, or the list just emptied) costs the waker one failed
 * svc_pool_wake_idle_thread(), which only reads that list, and perhaps a
 * cross-node wake its node did not need.  A clear bit whose pool has an
 * idle thread lets the waker pass that pool by as if its threads were
 * busy.  Neither loses or doubles a transport: the transport stays on
 * the queue it was put on, which that pool's own threads and the pools
 * serving it look at before they sleep, and XPT_BUSY and lwq_dequeue()
 * still give it to one thread.
 *
 * Nor does the map open a lost wakeup between a thread going idle and a
 * racing enqueue.  The thread adds itself to the idle list (llist_add(),
 * a full barrier), sets its bit if the list was empty, and orders that
 * store before the queue loads of svc_thread_sibling_work() with
 * smp_mb__after_atomic().  The enqueue stores its transport with
 * lwq_enqueue() (a cmpxchg) before it loads the idle lists and the map.
 * So either the waker sees the thread, on its list or in the map, or the
 * thread's walk, which covers its whole node whenever the waker reads
 * the map, sees the transport, as before the map.  A thread that joins a
 * non-empty list relies on the bit its predecessor set.  The clearing
 * side is the delicate one: svc_pool_idle_del() clears the bit only when
 * its removal left the list empty, then, after smp_mb__after_atomic(),
 * looks at the list again.  A thread added meanwhile is either seen
 * there or saw the clear bit and set it itself.  When it is seen and the
 * bit is still clear, a waker may have read the map between the clear
 * and now, so svc_pool_idle_del() sets the bit and claims the first idle
 * thread; that thread walks its node again (svc_recv()) and takes what
 * the waker could not give it.
 */

/**
 * svc_pool_idle_add - put an idle thread on its pool's idle list
 * @ext: the pool's svc_pool_ext
 * @pool: the thread's pool
 * @rqstp: the thread
 *
 * The caller then loads the queues it may take work from.
 */
VISIBLE_IF_KUNIT void
svc_pool_idle_add(struct svc_pool_ext *ext, struct svc_pool *pool,
		  struct svc_rqst *rqstp)
{
	unsigned long *map;

	if (!llist_add(&rqstp->rq_idle, &pool->sp_idle_threads))
		return;
	map = READ_ONCE(ext->idle);
	if (map) {
		set_bit(READ_ONCE(ext->place), map);
		/* the bit before the caller's queue loads */
		smp_mb__after_atomic();
	}
}
EXPORT_SYMBOL_IF_KUNIT(svc_pool_idle_add);

/**
 * svc_pool_idle_del - take an idle thread off its pool's idle list
 * @ext: the pool's svc_pool_ext
 * @pool: the thread's pool
 * @rqstp: the thread
 *
 * Only the first thread on the list can leave it.
 *
 * Return: true if @rqstp was first and is now off the list.
 */
VISIBLE_IF_KUNIT bool
svc_pool_idle_del(struct svc_pool_ext *ext, struct svc_pool *pool,
		  struct svc_rqst *rqstp)
{
	unsigned long *map;
	unsigned int place;

	if (!llist_del_first_this(&pool->sp_idle_threads, &rqstp->rq_idle))
		return false;
	map = READ_ONCE(ext->idle);
	if (!map || READ_ONCE(pool->sp_idle_threads.first))
		return true;
	place = READ_ONCE(ext->place);
	clear_bit(place, map);
	/* the clear before the second look at the list */
	smp_mb__after_atomic();
	if (READ_ONCE(pool->sp_idle_threads.first) &&
	    !test_and_set_bit(place, map))
		svc_pool_wake_idle_thread(pool);
	return true;
}
EXPORT_SYMBOL_IF_KUNIT(svc_pool_idle_del);

/*
 * Does @pool share its run with other pools, or reach other nodes?  An
 * unpooled service has one pool; otherwise this reads only @pool's own
 * ext, not the global map.
 */
static bool svc_pool_has_siblings(const struct svc_serv *serv,
				  const struct svc_pool *pool)
{
	const struct svc_pool_ext *ext = &serv->sv_pool_ext[pool->sp_id];

	return serv->sv_is_pooled &&
	       ((READ_ONCE(ext->range) & 0xffff) > 1 ||
		READ_ONCE(ext->far) != pool->sp_id);
}

/*
 * The far pool of the node after @peer's, or UINT_MAX when that is back
 * on @self's node (the run of @self).
 */
static unsigned int svc_pool_next_far(const struct svc_pool_ext *ext,
				      unsigned int self, unsigned int peer)
{
	unsigned int far = READ_ONCE(ext[peer].far);

	if (READ_ONCE(ext[far].range) == READ_ONCE(ext[self].range))
		return UINT_MAX;
	return far;
}

/**
 * svc_pool_far_window - the places of another node a pool looks at
 * @j: the pool's place in its own node's run
 * @n: the number of pools in that run
 * @m: the number of pools in the other node's run
 * @lo: set to the first place in the other run
 * @hi: set to the last place, below @lo when there is none
 *
 * Place i of the other run is in the window when the centres of places j
 * and i, as fractions of their runs, are less than t / (2 * n * m) apart,
 * with t = max(SVC_POOL_STEAL_SPAN * min(n, m), max(n, m) + 1).  That
 * relation is the same with the two runs swapped, so a cross-node wake
 * claims a thread only where that thread's steal walk looks back at the
 * waking pool.  A window is never empty, and has at most
 * SVC_POOL_STEAL_SPAN places while the runs differ in size by less than
 * that factor (else about the ratio of their sizes).  The walks look at
 * the pool serving each place, so a window of threadless places still
 * reaches the threads that serve them.
 */
VISIBLE_IF_KUNIT void svc_pool_far_window(unsigned int j, unsigned int n, unsigned int m,
					 unsigned int *lo, unsigned int *hi)
{
	unsigned long c, t, q;

	/* (2 * j + 1) * m + t must not overflow */
	BUILD_BUG_ON(BITS_PER_LONG < 64 && NR_CPUS > 0x7fff);

	*lo = 1;
	*hi = 0;
	if (!n || !m || j >= n)
		return;
	c = (2UL * j + 1) * m;
	t = max(SVC_POOL_STEAL_SPAN * min(n, m), max(n, m) + 1);
	/* the first i with (2i + 1) n > c - t, the last with < c + t */
	q = c + 1 > t + n ? (c - t + n) / n : 1;
	*lo = q / 2;
	q = (c + t - 1) / n;
	*hi = min_t(unsigned long, (q - 1) / 2, m - 1);
}
EXPORT_SYMBOL_IF_KUNIT(svc_pool_far_window);

/*
 * The places of another node's run of @fcount pools that the cross-node
 * walks of @self look at: the svc_pool_far_window() places of every
 * place of @self's block (itself and the threadless pools it serves),
 * as at most two ranges [lo, hi], two when the block wraps within its
 * run.  The windows grow with the place, so a segment of the block gives
 * one range.  A remote pool so takes part when one of its own block's
 * places is in one of those windows; the place relation is symmetric,
 * so this relation of pools is too, which the walks look at through
 * serve: a pool whose wake can claim a remote thread is a pool that
 * thread's steal walk looks at, also when threads are sparse.
 */
static unsigned int svc_pool_far_ranges(const struct svc_pool_ext *ext,
					unsigned int self, unsigned int range,
					unsigned int fcount, unsigned int *lo,
					unsigned int *hi)
{
	unsigned int gap = READ_ONCE(ext[self].gap);
	unsigned int count = range & 0xffff, s = self - (range >> 16);
	unsigned int back = min(gap >> 16, count - 1);
	unsigned int end = s + min(gap & 0xffff, count - 1);
	unsigned int seg[2][2], nr, k, x;

	if (back > s) {
		seg[0][0] = s + count - back;
		seg[0][1] = count - 1;
		seg[1][0] = 0;
		seg[1][1] = min(end, count - 1);
		nr = 2;
	} else if (end >= count) {
		seg[0][0] = s - back;
		seg[0][1] = count - 1;
		seg[1][0] = 0;
		seg[1][1] = end - count;
		nr = 2;
	} else {
		seg[0][0] = s - back;
		seg[0][1] = end;
		nr = 1;
	}
	for (k = 0; k < nr; k++) {
		svc_pool_far_window(seg[k][0], count, fcount, &lo[k], &x);
		svc_pool_far_window(seg[k][1], count, fcount, &x, &hi[k]);
	}
	return nr;
}
/*
 * How many ring members of its own node the steal walk of @self looks
 * at: SVC_POOL_STEAL_SPAN, or with @full the whole run when @self
 * reaches other nodes, so that nothing crosses nodes while its own node
 * has work.
 */
static unsigned int svc_pool_local_span(const struct svc_pool_ext *ext,
					unsigned int self, unsigned int range,
					bool full)
{
	if (!full || READ_ONCE(ext[self].far) == self)
		return SVC_POOL_STEAL_SPAN;
	return range & 0xffff;
}

/* The next and previous pool of @i's run, wrapping within the run */
static unsigned int svc_pool_run_next(const struct svc_pool_ext *ext,
				      unsigned int i)
{
	unsigned int range = READ_ONCE(ext[i].range);
	unsigned int first = range >> 16, count = range & 0xffff;

	if (!count)
		return i;
	return i + 1 < first + count ? i + 1 : first;
}

static unsigned int svc_pool_run_prev(const struct svc_pool_ext *ext,
				      unsigned int i)
{
	unsigned int range = READ_ONCE(ext[i].range);
	unsigned int first = range >> 16, count = range & 0xffff;

	if (!count)
		return i;
	return i > first ? i - 1 : first + count - 1;
}

/*
 * Claim an idle thread of a pool that the idle map of @self's node names,
 * nearest first going forward from @after and wrapping, each pool once.
 * @first and @count are the node's run.  Reads the map, and only the
 * idle lists of the pools it names.  Returns the pool claimed, or
 * UINT_MAX.
 */
static unsigned int svc_pool_idle_claim(struct svc_pool *pools,
					const struct svc_pool_ext *ext,
					unsigned int self, unsigned int first,
					unsigned int count, unsigned int after)
{
	const unsigned long *map = READ_ONCE(ext[self].idle);
	unsigned int bit, start;

	if (!map || !count)
		return UINT_MAX;
	start = (after - first + 1) % count;
	for_each_set_bit_wrap(bit, map, count, start) {
		if (first + bit == self)
			continue;
		if (svc_pool_wake_idle_thread(&pools[first + bit]))
			return first + bit;
	}
	return UINT_MAX;
}

/**
 * svc_pool_wake_sibling - claim an idle thread of a sibling pool
 * @pools: the service's pools
 * @ext: their per-pool state
 * @self: the pool whose work needs a thread
 *
 * Called when svc_pool_wake_idle_thread(@self) claimed nothing.  Walks
 * forward over up to SVC_POOL_STEAL_SPAN pools with threads in @self's
 * run, nearest first.  When @self reaches other nodes, it then tries the
 * pools its node's idle map names, nearest first from where the walk
 * stopped (svc_pool_idle_claim()), and only when none of those gave a
 * thread, each other node in turn: the pools serving the places that
 * svc_pool_far_ranges() names, if that node's map names them.  A
 * saturated node so reads eight idle lists and its map, as many lines as
 * without the map.  The claimed thread finds the work through
 * svc_xprt_steal().
 *
 * Returns the pool whose thread was claimed, or @self when none was.
 */
VISIBLE_IF_KUNIT unsigned int
svc_pool_wake_sibling(struct svc_pool *pools, struct svc_pool_ext *ext,
		      unsigned int self)
{
	unsigned int range = READ_ONCE(ext[self].range);
	unsigned int first = range >> 16, count = range & 0xffff;
	unsigned int peer = self, far = self;
	unsigned int d, n, i, r, nr, lo[2], hi[2], frange, fbase, last;
	const unsigned long *fmap;

	for (d = 0; d < SVC_POOL_STEAL_SPAN; d++) {
		peer = READ_ONCE(ext[peer].next);
		if (peer == self)
			break;
		if (svc_pool_wake_idle_thread(&pools[peer]))
			goto claimed;
	}
	if (READ_ONCE(ext[self].far) == self)
		return self;
	/* the rest of the node, by its idle map, unless the walk saw it all */
	if (peer != self) {
		peer = svc_pool_idle_claim(pools, ext, self, first, count, peer);
		if (peer != UINT_MAX)
			goto claimed;
	}
	/* no idle thread on this node: the other nodes, one at a time */
	for (n = 0; n < MAX_NUMNODES; n++) {
		far = svc_pool_next_far(ext, self, far);
		if (far == UINT_MAX)
			break;
		frange = READ_ONCE(ext[far].range);
		fbase = frange >> 16;
		fmap = READ_ONCE(ext[far].idle);
		nr = svc_pool_far_ranges(ext, self, range, frange & 0xffff,
					 lo, hi);
		for (r = 0; r < nr; r++) {
			for (i = lo[r], last = UINT_MAX; i <= hi[r]; i++) {
				peer = READ_ONCE(ext[fbase + i].serve);
				if (peer == last)
					continue;
				last = peer;
				if (fmap && !test_bit(peer - fbase, fmap))
					continue;
				if (svc_pool_wake_idle_thread(&pools[peer]))
					goto claimed;
			}
		}
	}
	return self;
claimed:
	percpu_counter_inc(&ext[self].sibling_wakes);
	return peer;
}
EXPORT_SYMBOL_IF_KUNIT(svc_pool_wake_sibling);

/* With @xprtp, dequeue a transport of @pool into it; else report work */
static bool svc_pool_steal_one(struct svc_pool *pool, struct svc_xprt **xprtp)
{
	if (lwq_empty(&pool->sp_xprts))
		return false;
	if (!xprtp)
		return true;
	*xprtp = lwq_dequeue(&pool->sp_xprts, struct svc_xprt, xpt_ready);
	if (!*xprtp)
		return false;
	svc_xprt_get(*xprtp);
	return true;
}

/*
 * Walk the pools a thread of @self may take work from: first the pools
 * without threads that @self serves (its block, before and after it),
 * then the pools with threads backward along the ring, nearest first: up
 * to SVC_POOL_STEAL_SPAN of them.  With @full, the whole of its node's
 * ring when @self reaches other nodes, and only then the pools serving
 * the places of each other node in turn that svc_pool_far_ranges()
 * names, leaving a pool whose idle map bit says its own idle thread
 * serves it.  With @xprtp, dequeue the first transport found into it;
 * without, only report a pool with work.
 */
static struct svc_pool *
svc_pool_steal_scan(struct svc_pool *pools, const struct svc_pool_ext *ext,
		    unsigned int self, struct svc_xprt **xprtp, bool full)
{
	unsigned int gap = READ_ONCE(ext[self].gap);
	unsigned int back = gap >> 16, block = back + (gap & 0xffff);
	unsigned int range = READ_ONCE(ext[self].range);
	unsigned int span = svc_pool_local_span(ext, self, range, full);
	unsigned int peer = self, far = self;
	unsigned int k, n, i, r, nr, lo[2], hi[2], frange, fbase, last;
	const unsigned long *fmap;

	for (k = 0; k < block + span; k++) {
		if (k < back)
			peer = svc_pool_run_prev(ext, peer);
		else if (k < block)
			peer = svc_pool_run_next(ext, k == back ? self : peer);
		else
			peer = READ_ONCE(ext[k == block ? self : peer].prev);
		if (peer == self)
			break;
		if (svc_pool_steal_one(&pools[peer], xprtp))
			return &pools[peer];
	}
	for (n = 0; full && n < MAX_NUMNODES; n++) {
		far = svc_pool_next_far(ext, self, far);
		if (far == UINT_MAX)
			break;
		frange = READ_ONCE(ext[far].range);
		fbase = frange >> 16;
		fmap = READ_ONCE(ext[far].idle);
		nr = svc_pool_far_ranges(ext, self, range, frange & 0xffff,
					 lo, hi);
		for (r = 0; r < nr; r++) {
			for (i = lo[r], last = UINT_MAX; i <= hi[r]; i++) {
				peer = READ_ONCE(ext[fbase + i].serve);
				if (peer == last)
					continue;
				last = peer;
				if (fmap && test_bit(peer - fbase, fmap))
					continue;
				if (svc_pool_steal_one(&pools[peer], xprtp))
					return &pools[peer];
			}
		}
	}
	return NULL;
}

/**
 * svc_pool_steal_source - find a pool with work a thread of @self may take
 * @pools: the service's pools
 * @ext: their per-pool state
 * @self: the pool of the looking thread
 *
 * Returns the nearest such pool with queued transports, or NULL.
 */
VISIBLE_IF_KUNIT struct svc_pool *
svc_pool_steal_source(struct svc_pool *pools, const struct svc_pool_ext *ext,
		      unsigned int self)
{
	return svc_pool_steal_scan(pools, ext, self, NULL, true);
}
EXPORT_SYMBOL_IF_KUNIT(svc_pool_steal_source);

/**
 * svc_xprt_steal - take a transport from a sibling pool
 * @pools: the service's pools
 * @ext: their per-pool state
 * @self: the pool of the taking thread
 * @from: set to the pool the transport came from
 *
 * Returns a referenced transport, or NULL.
 */
VISIBLE_IF_KUNIT struct svc_xprt *
svc_xprt_steal(struct svc_pool *pools, const struct svc_pool_ext *ext,
	       unsigned int self, unsigned int *from)
{
	struct svc_xprt *xprt = NULL;
	struct svc_pool *pool;

	pool = svc_pool_steal_scan(pools, ext, self, &xprt, true);
	if (pool)
		*from = pool->sp_id;
	return xprt;
}
EXPORT_SYMBOL_IF_KUNIT(svc_xprt_steal);

/*
 * Queue @xprt on @pool, then claim an idle thread of @pool for it, or,
 * when that fails, an idle thread of a sibling pool, which steals it.
 */
VISIBLE_IF_KUNIT void svc_pool_enqueue_xprt(struct svc_serv *serv,
					    struct svc_pool *pool,
					    struct svc_xprt *xprt)
{
	percpu_counter_inc(&pool->sp_sockets_queued);
	/* Only trace_svc_xprt_dequeue() reads the enqueue time. */
	if (trace_svc_xprt_dequeue_enabled())
		xprt->xpt_qtime = ktime_get();
	lwq_enqueue(&xprt->xpt_ready, &pool->sp_xprts);

	if (!svc_pool_wake_idle_thread(pool) &&
	    svc_pool_has_siblings(serv, pool))
		svc_pool_wake_sibling(serv->sv_pools, serv->sv_pool_ext,
				      pool->sp_id);
}
EXPORT_SYMBOL_IF_KUNIT(svc_pool_enqueue_xprt);

/**
 * svc_xprt_enqueue - Queue a transport on an idle nfsd thread
 * @xprt: transport with data pending
 *
 */
void svc_xprt_enqueue(struct svc_xprt *xprt)
{
	struct svc_serv *serv;
	struct svc_pool *pool;

	if (!svc_xprt_ready(xprt))
		return;

	/* Mark transport as busy. It will remain in this state until
	 * the provider calls svc_xprt_received. We update XPT_BUSY
	 * atomically because it also guards against trying to enqueue
	 * the transport twice.
	 */
	if (test_and_set_bit(XPT_BUSY, &xprt->xpt_flags))
		return;

	serv = xprt->xpt_server;
	pool = svc_pool_for_cpu(serv);
	svc_pool_enqueue_xprt(serv, pool, xprt);
}
EXPORT_SYMBOL_GPL(svc_xprt_enqueue);

/*
 * Dequeue the first transport, if there is one.
 */
static struct svc_xprt *svc_xprt_dequeue(struct svc_pool *pool)
{
	struct svc_xprt	*xprt = NULL;

	xprt = lwq_dequeue(&pool->sp_xprts, struct svc_xprt, xpt_ready);
	if (xprt)
		svc_xprt_get(xprt);
	return xprt;
}

/**
 * svc_reserve - change the space reserved for the reply to a request.
 * @rqstp:  The request in question
 * @space: new max space to reserve
 *
 * Each request reserves some space on the output queue of the transport
 * to make sure the reply fits.  This function reduces that reserved
 * space to be the amount of space used already, plus @space.
 *
 * The transport's reservation is tracked only on classes that set
 * SVC_XPRT_FLAG_WSPACE_RESERVE.  On the others, only @rqstp's
 * reservation is updated.
 *
 */
void svc_reserve(struct svc_rqst *rqstp, int space)
{
	struct svc_xprt *xprt = rqstp->rq_xprt;

	space += rqstp->rq_res.head[0].iov_len;

	if (xprt && space < rqstp->rq_reserved) {
		if (xprt->xpt_class->xcl_flags & SVC_XPRT_FLAG_WSPACE_RESERVE) {
			atomic_sub((rqstp->rq_reserved - space),
				   &xprt->xpt_reserved);
			svc_xprt_resource_released(xprt);
		}
		rqstp->rq_reserved = space;
	}
}
EXPORT_SYMBOL_GPL(svc_reserve);

static void free_deferred(struct svc_xprt *xprt, struct svc_deferred_req *dr)
{
	if (!dr)
		return;

	xprt->xpt_ops->xpo_release_ctxt(xprt, dr->xprt_ctxt);
	if (dr->rx_region.parked && xprt->xpt_ops->xpo_rx_region_park_free)
		xprt->xpt_ops->xpo_rx_region_park_free(&dr->rx_region);
	kfree(dr);
}

static void svc_xprt_release(struct svc_rqst *rqstp)
{
	struct svc_xprt	*xprt = rqstp->rq_xprt;

	xprt->xpt_ops->xpo_release_ctxt(xprt, rqstp->rq_xprt_ctxt);
	rqstp->rq_xprt_ctxt = NULL;

	free_deferred(xprt, rqstp->rq_deferred);
	rqstp->rq_deferred = NULL;

	/* An unsent confined reply returns its pages; a sent one left NULLs. */
	svc_rqst_rx_region_unconfine(rqstp, false);
	svc_rqst_release_pages(rqstp);
	svc_rqst_rx_region_restore(rqstp);
	rqstp->rq_res.page_len = 0;
	rqstp->rq_res.page_base = 0;

	/* Reset response buffer and release
	 * the reservation.
	 * But first, check that enough space was reserved
	 * for the reply, otherwise we have a bug!
	 */
	if ((rqstp->rq_res.len) >  rqstp->rq_reserved)
		printk(KERN_ERR "RPC request reserved %d but used %d\n",
		       rqstp->rq_reserved,
		       rqstp->rq_res.len);

	rqstp->rq_res.head[0].iov_len = 0;
	svc_reserve(rqstp, 0);
	svc_xprt_release_slot(rqstp);
	rqstp->rq_xprt = NULL;
	svc_xprt_put(xprt);
}

/**
 * svc_wake_up - Wake up a service thread for non-transport work
 * @serv: RPC service
 *
 * Some svc_serv's will have occasional work to do, even when a xprt is not
 * waiting to be serviced. This function is there to "kick" a task in one of
 * those services so that it can wake up and do that work. Note that we only
 * bother with one pool as we don't need to wake up more than one thread for
 * this purpose: pool 0, or the pool that serves it when pool 0 has no
 * threads (see svc_pool_ext_layout()).
 */
void svc_wake_up(struct svc_serv *serv)
{
	unsigned int pidx = serv->sv_is_pooled ?
			    READ_ONCE(serv->sv_pool_ext[0].serve) : 0;
	struct svc_pool *pool = &serv->sv_pools[pidx];

	set_bit(SP_TASK_PENDING, &pool->sp_flags);
	svc_pool_wake_idle_thread(pool);
}
EXPORT_SYMBOL_GPL(svc_wake_up);

int svc_port_is_privileged(struct sockaddr *sin)
{
	switch (sin->sa_family) {
	case AF_INET:
		return ntohs(((struct sockaddr_in *)sin)->sin_port)
			< PROT_SOCK;
	case AF_INET6:
		return ntohs(((struct sockaddr_in6 *)sin)->sin6_port)
			< PROT_SOCK;
	default:
		return 0;
	}
}

/*
 * Make sure that we don't have too many connections that have not yet
 * demonstrated that they have access to the NFS server. If we have,
 * something must be dropped. It's not clear what will happen if we allow
 * "too many" connections, but when dealing with network-facing software,
 * we have to code defensively. Here we do that by imposing hard limits.
 *
 * There's no point in trying to do random drop here for DoS
 * prevention. The NFS clients does 1 reconnect in 15 seconds. An
 * attacker can easily beat that.
 *
 * The only somewhat efficient mechanism would be if drop old
 * connections from the same IP first. But right now we don't even
 * record the client IP in svc_sock.
 */
static void svc_check_conn_limits(struct svc_serv *serv)
{
	if (serv->sv_tmpcnt > XPT_MAX_TMP_CONN) {
		struct svc_xprt *xprt = NULL, *xprti;
		spin_lock_bh(&serv->sv_lock);
		if (!list_empty(&serv->sv_tempsocks)) {
			/*
			 * Always select the oldest connection. It's not fair,
			 * but nor is life.
			 */
			list_for_each_entry_reverse(xprti, &serv->sv_tempsocks,
						    xpt_list) {
				if (!test_bit(XPT_PEER_VALID, &xprti->xpt_flags)) {
					xprt = xprti;
					set_bit(XPT_CLOSE, &xprt->xpt_flags);
					svc_xprt_get(xprt);
					break;
				}
			}
		}
		spin_unlock_bh(&serv->sv_lock);

		if (xprt) {
			svc_xprt_enqueue(xprt);
			svc_xprt_put(xprt);
		}
	}
}

static bool svc_fill_pages(struct svc_rqst *rqstp, struct page **pages,
			   unsigned long npages)
{
	unsigned long filled, ret;

	for (filled = 0; filled < npages; filled = ret) {
		ret = alloc_pages_bulk(GFP_KERNEL, npages, pages);
		if (ret > filled)
			/* Made progress, don't sleep yet */
			continue;

		set_current_state(TASK_IDLE);
		if (svc_thread_should_stop(rqstp)) {
			set_current_state(TASK_RUNNING);
			return false;
		}
		trace_svc_alloc_arg_err(npages, ret);
		memalloc_retry_wait(GFP_KERNEL);
	}
	return true;
}

static bool svc_alloc_arg(struct svc_rqst *rqstp)
{
	struct xdr_buf *arg = &rqstp->rq_arg;
	unsigned long pages, nfree;

	pages = rqstp->rq_maxpages;

	nfree = rqstp->rq_pages_nfree;
	if (nfree) {
		if (!svc_fill_pages(rqstp, rqstp->rq_pages, nfree))
			return false;
		rqstp->rq_pages_nfree = 0;
	}

	if (WARN_ON_ONCE(rqstp->rq_next_page < rqstp->rq_respages))
		return false;
	nfree = rqstp->rq_next_page - rqstp->rq_respages;
	if (nfree) {
		if (!svc_fill_pages(rqstp, rqstp->rq_respages, nfree))
			return false;
	}

	rqstp->rq_next_page = rqstp->rq_respages;
	rqstp->rq_page_end = &rqstp->rq_respages[pages];
	/* svc_rqst_replace_page() dereferences *rq_next_page even
	 * at rq_page_end; NULL prevents releasing a garbage page.
	 */
	rqstp->rq_page_end[0] = NULL;

	/* Make arg->head point to first page and arg->pages point to rest */
	arg->head[0].iov_base = page_address(rqstp->rq_pages[0]);
	arg->head[0].iov_len = PAGE_SIZE;
	arg->pages = rqstp->rq_pages + 1;
	arg->page_base = 0;
	/* save at least one page for response */
	arg->page_len = (pages-2)*PAGE_SIZE;
	arg->len = (pages-1)*PAGE_SIZE;
	arg->tail[0].iov_len = 0;

	rqstp->rq_xid = xdr_zero;
	return true;
}

/*
 * One idle period's steal walks.  The walk over the whole node runs at
 * most once per period: before the thread sleeps (svc_thread_sibling_work()),
 * or after it was claimed or slept, when the work may have come anywhere
 * on the node; the walks after it in the same period try the pool it
 * found, then only the SVC_POOL_STEAL_SPAN nearest ring members.
 */
struct svc_steal_cycle {
	struct svc_pool		*found;		/* where the full walk saw work */
	bool			walked;		/* the full walk has run */
};

/* Is there work this thread may take from a sibling pool? */
static bool svc_thread_sibling_work(struct svc_rqst *rqstp,
				    struct svc_steal_cycle *cyc)
{
	struct svc_serv *serv = rqstp->rq_server;

	if (!svc_pool_has_siblings(serv, rqstp->rq_pool))
		return false;
	/* transports, in one pass */
	cyc->walked = true;
	cyc->found = svc_pool_steal_source(serv->sv_pools, serv->sv_pool_ext,
					   rqstp->rq_pool->sp_id);
	return cyc->found;
}

static struct svc_xprt *svc_thread_steal(struct svc_rqst *rqstp,
					 struct svc_pool **from,
					 struct svc_steal_cycle *cyc)
{
	struct svc_serv *serv = rqstp->rq_server;
	unsigned int self = rqstp->rq_pool->sp_id;
	struct svc_xprt *xprt = NULL;
	struct svc_pool *pool = cyc->found;
	unsigned int id;

	if (!svc_pool_has_siblings(serv, rqstp->rq_pool))
		return NULL;
	if (!pool || !svc_pool_steal_one(pool, &xprt)) {
		if (cyc->walked) {
			pool = svc_pool_steal_scan(serv->sv_pools,
						   serv->sv_pool_ext, self,
						   &xprt, false);
		} else {
			xprt = svc_xprt_steal(serv->sv_pools, serv->sv_pool_ext,
					      self, &id);
			pool = xprt ? &serv->sv_pools[id] : NULL;
		}
	}
	cyc->walked = true;
	if (xprt) {
		percpu_counter_inc(&serv->sv_pool_ext[self].steals);
		*from = pool;
	}
	return xprt;
}

static bool
svc_thread_should_sleep(struct svc_rqst *rqstp)
{
	struct svc_pool		*pool = rqstp->rq_pool;

	/* did someone call svc_wake_up? */
	if (test_bit(SP_TASK_PENDING, &pool->sp_flags))
		return false;

	/* was a socket queued? */
	if (!lwq_empty(&pool->sp_xprts))
		return false;

	/* are we shutting down? */
	if (svc_thread_should_stop(rqstp))
		return false;

#if defined(CONFIG_SUNRPC_BACKCHANNEL)
	if (svc_is_backchannel(rqstp)) {
		if (!lwq_empty(&rqstp->rq_server->sv_cb_list))
			return false;
	}
#endif

	return true;
}

/*
 * As svc_thread_should_sleep(), and the thread was not claimed for work
 * (RQ_WAKING), possibly a sibling pool's, since it went idle.
 */
static bool svc_thread_may_sleep(struct svc_rqst *rqstp)
{
	return svc_thread_should_sleep(rqstp) &&
	       !test_bit(RQ_WAKING, &rqstp->rq_flags);
}

/**
 * svc_thread_idle_spin - poll for work before sleeping
 * @rqstp: an idle thread, already on its pool's idle list
 * @max_us: the longest spin, SVC_IDLE_SPIN_US
 *
 * The thread stays running, so a waker that finds it first on the idle
 * list claims it (RQ_WAKING) without waking it.  Each iteration checks
 * only the thread's own pool and its own RQ_WAKING bit; sibling queues
 * were checked once before the spin.  Returns true when work turned up.
 */
VISIBLE_IF_KUNIT bool svc_thread_idle_spin(struct svc_rqst *rqstp,
					   unsigned int max_us)
{
	unsigned int budget = svc_idle_spin_budget(rqstp->rq_spin_ns, max_us);
	unsigned int cap = max_us * NSEC_PER_USEC;
	struct svc_pool *pool = rqstp->rq_pool;
	struct svc_pool_ext *ext;
	u64 start;

	if (!budget)
		return false;
	start = local_clock();
	do {
		cpu_relax();
		if (!svc_thread_may_sleep(rqstp)) {
			ext = &rqstp->rq_server->sv_pool_ext[pool->sp_id];
			rqstp->rq_spin_ns = svc_idle_spin_grow(budget, cap);
			percpu_counter_inc(&ext->spin_hits);
			return true;
		}
	} while (!need_resched() && local_clock() - start < budget);
	if (!need_resched())
		rqstp->rq_spin_ns = svc_idle_spin_shrink(budget);
	return false;
}
EXPORT_SYMBOL_IF_KUNIT(svc_thread_idle_spin);

/* Claim another thread for the work a claim on this one stood for */
VISIBLE_IF_KUNIT void svc_thread_pass_claim(struct svc_rqst *rqstp)
{
	struct svc_serv *serv = rqstp->rq_server;
	struct svc_pool *pool = rqstp->rq_pool;

	if (!svc_pool_wake_idle_thread(pool) &&
	    svc_pool_has_siblings(serv, pool))
		svc_pool_wake_sibling(serv->sv_pools, serv->sv_pool_ext,
				      pool->sp_id);
}
EXPORT_SYMBOL_IF_KUNIT(svc_thread_pass_claim);

static bool svc_schedule_timeout(long timeo)
{
	return schedule_timeout(timeo ? timeo : MAX_SCHEDULE_TIMEOUT) == 0;
}

static bool svc_thread_wait_for_work(struct svc_rqst *rqstp, long timeo,
				     struct svc_steal_cycle *cyc)
{
	struct svc_pool *pool = rqstp->rq_pool;
	struct svc_pool_ext *ext = &rqstp->rq_server->sv_pool_ext[pool->sp_id];
	bool did_timeout = false;

	if (svc_thread_should_sleep(rqstp)) {
		const unsigned int spin_us = SVC_IDLE_SPIN_US;

		/* Without a spin, the task state is set first, so that a
		 * waker that claims this thread once it is on the idle list
		 * either sees it sleeping or is seen by the checks below.
		 */
		if (likely(!spin_us))
			set_current_state(TASK_IDLE | TASK_FREEZABLE);
		/* Ordered before the llist_add() that makes this thread
		 * visible to svc_pool_wake_idle_thread(), the only setter.
		 */
		clear_bit(RQ_WAKING, &rqstp->rq_flags);
		svc_pool_idle_add(ext, pool, rqstp);
		/* Once per idle period, after svc_pool_idle_add(): an
		 * enqueue on a sibling pool either sees this thread on the
		 * idle list or in the idle map and claims it, or its work
		 * is seen here (see svc_pool_idle_add()).
		 */
		if (svc_thread_sibling_work(rqstp, cyc))
			goto dequeue_self;
		if (spin_us) {
			/* On the idle list but running: a waker claims this
			 * thread without waking it.  The set_current_state()
			 * after the spin orders the state before the
			 * recheck below, so a waker that saw this thread
			 * running set RQ_WAKING where the recheck sees it.
			 */
			if (svc_thread_idle_spin(rqstp, spin_us))
				goto dequeue_self;
			set_current_state(TASK_IDLE | TASK_FREEZABLE);
		}
		if (likely(svc_thread_may_sleep(rqstp))) {
			unsigned int cap = spin_us * NSEC_PER_USEC;
			u64 slept = spin_us ? local_clock() : 0;

			did_timeout = svc_schedule_timeout(timeo);
			/* work that came while it slept may be anywhere */
			cyc->walked = false;
			cyc->found = NULL;
			/* A spin of the full budget would have found the
			 * work that ended this sleep.
			 */
			if (spin_us && !did_timeout &&
			    local_clock() - slept <= cap)
				rqstp->rq_spin_ns =
					svc_idle_spin_grow(rqstp->rq_spin_ns, cap);
		}
dequeue_self:
		/* a claim stands for work anywhere on the node */
		if (test_bit(RQ_WAKING, &rqstp->rq_flags)) {
			cyc->walked = false;
			cyc->found = NULL;
		}

		while (!svc_pool_idle_del(ext, pool, rqstp)) {
			/* Work just became available.  This thread can only
			 * handle it after removing rqstp from the idle
			 * list. If that attempt failed, some other thread
			 * must have queued itself after finding no
			 * work to do, so that thread has taken responsibly
			 * for this new work.  A claim (RQ_WAKING) that
			 * reached this thread while it was first on the
			 * list is passed on, as no waker will look at this
			 * thread again before it is first again.  Then this
			 * thread can safely sleep until woken again; each
			 * sleep follows set_current_state() and a test of
			 * RQ_WAKING, so a later claim is not lost.
			 */
			if (test_and_clear_bit(RQ_WAKING, &rqstp->rq_flags))
				svc_thread_pass_claim(rqstp);
			else
				did_timeout = svc_schedule_timeout(timeo);
			cyc->walked = false;
			cyc->found = NULL;
			set_current_state(TASK_IDLE | TASK_FREEZABLE);
		}
		__set_current_state(TASK_RUNNING);
	} else {
		cond_resched();
	}
	try_to_freeze();
	return did_timeout;
}

static void svc_add_new_temp_xprt(struct svc_serv *serv, struct svc_xprt *newxpt)
{
	spin_lock_bh(&serv->sv_lock);
	set_bit(XPT_TEMP, &newxpt->xpt_flags);
	list_add(&newxpt->xpt_list, &serv->sv_tempsocks);
	serv->sv_tmpcnt++;
	if (serv->sv_temptimer.function == NULL) {
		/* setup timer to age temp transports */
		serv->sv_temptimer.function = svc_age_temp_xprts;
		mod_timer(&serv->sv_temptimer,
			  jiffies + svc_conn_age_period * HZ);
	}
	spin_unlock_bh(&serv->sv_lock);
	svc_xprt_received(newxpt);
}

static void svc_handle_xprt(struct svc_rqst *rqstp, struct svc_xprt *xprt)
{
	struct svc_serv *serv = rqstp->rq_server;
	int len = 0;

	if (test_bit(XPT_CLOSE, &xprt->xpt_flags)) {
		if (test_and_clear_bit(XPT_KILL_TEMP, &xprt->xpt_flags))
			xprt->xpt_ops->xpo_kill_temp_xprt(xprt);
		svc_delete_xprt(xprt);
		/* Leave XPT_BUSY set on the dead xprt: */
		goto out;
	}
	if (test_bit(XPT_LISTENER, &xprt->xpt_flags)) {
		struct svc_xprt *newxpt;
		/*
		 * We know this module_get will succeed because the
		 * listener holds a reference too
		 */
		__module_get(xprt->xpt_class->xcl_owner);
		svc_check_conn_limits(xprt->xpt_server);
		newxpt = xprt->xpt_ops->xpo_accept(xprt);
		if (newxpt) {
			newxpt->xpt_cred = get_cred(xprt->xpt_cred);
			trace_svc_xprt_accept(newxpt, serv->sv_name);
			svc_add_new_temp_xprt(serv, newxpt);
		} else {
			module_put(xprt->xpt_class->xcl_owner);
		}
		svc_xprt_received(xprt);
	} else if (test_bit(XPT_HANDSHAKE, &xprt->xpt_flags)) {
		xprt->xpt_ops->xpo_handshake(xprt);
		svc_xprt_received(xprt);
	} else if (svc_xprt_reserve_slot(rqstp, xprt)) {
		/* XPT_DATA|XPT_DEFERRED case: */
		rqstp->rq_deferred = svc_deferred_dequeue(xprt);
		if (rqstp->rq_deferred)
			len = svc_deferred_recv(rqstp);
		else
			len = xprt->xpt_ops->xpo_recvfrom(rqstp);
		rqstp->rq_reserved = serv->sv_max_mesg;
		if (xprt->xpt_class->xcl_flags & SVC_XPRT_FLAG_WSPACE_RESERVE)
			atomic_add(rqstp->rq_reserved, &xprt->xpt_reserved);
		if (len <= 0)
			goto out;

		trace_svc_xdr_recvfrom(&rqstp->rq_arg);

		clear_bit(XPT_OLD, &xprt->xpt_flags);

		rqstp->rq_chandle.defer = svc_defer;

		if (serv->sv_stats)
			serv->sv_stats->netcnt++;
		percpu_counter_inc(&rqstp->rq_pool->sp_messages_arrived);
		rqstp->rq_stime = ktime_get();
		svc_process(rqstp);
	} else
		svc_xprt_received(xprt);

out:
	rqstp->rq_res.len = 0;
	svc_xprt_release(rqstp);
}

/*
 * More work is pending in @pool after this thread dequeued some: claim
 * another thread for it, in @pool or else in a sibling pool.  @pool is
 * this thread's own pool, or the pool it stole from.
 */
static void svc_thread_wake_next(struct svc_rqst *rqstp,
				 struct svc_pool *pool)
{
	struct svc_serv *serv = rqstp->rq_server;

	if (pool == rqstp->rq_pool ? svc_thread_should_sleep(rqstp) :
				     lwq_empty(&pool->sp_xprts))
		return;
	if (!svc_pool_wake_idle_thread(pool) &&
	    svc_pool_has_siblings(serv, pool))
		svc_pool_wake_sibling(serv->sv_pools, serv->sv_pool_ext,
				      pool->sp_id);
}

/**
 * svc_recv - Receive and process the next request on any transport
 * @rqstp: an idle RPC service thread
 * @timeo: timeout (in jiffies) (0 means infinite timeout)
 *
 * This code is carefully organised not to touch any cachelines in
 * the shared svc_serv structure, only cachelines in the local
 * svc_pool.
 *
 * If the timeout is 0, then the sleep will never time out.
 *
 * Returns -ETIMEDOUT if idle for an extended period
 *         -EBUSY if there is more work to do than available threads
 *         0 otherwise.
 */
int svc_recv(struct svc_rqst *rqstp, long timeo)
{
	struct svc_steal_cycle cyc = {};
	struct svc_pool *pool = rqstp->rq_pool;
	struct svc_pool *from;
	bool did_timeout;
	int ret = 0;

	if (!svc_alloc_arg(rqstp))
		return ret;

	did_timeout = svc_thread_wait_for_work(rqstp, timeo, &cyc);

	if (did_timeout && svc_thread_should_sleep(rqstp) &&
	    pool->sp_nrthrmin && pool->sp_nrthreads > pool->sp_nrthrmin)
		ret = -ETIMEDOUT;

	clear_bit(SP_TASK_PENDING, &pool->sp_flags);

	if (svc_thread_should_stop(rqstp)) {
		if (!svc_thread_should_sleep(rqstp))
			svc_pool_wake_idle_thread(pool);
		return ret;
	}

	from = pool;
	rqstp->rq_xprt = svc_xprt_dequeue(pool);
	if (!rqstp->rq_xprt)
		rqstp->rq_xprt = svc_thread_steal(rqstp, &from, &cyc);
	if (rqstp->rq_xprt) {
		struct svc_xprt *xprt = rqstp->rq_xprt;

		svc_thread_wake_next(rqstp, from);
		/* Normally we will wait up to 5 seconds for any required
		 * cache information to be provided.  When there are no
		 * idle threads, we reduce the wait time.
		 */
		if (pool->sp_idle_threads.first) {
			rqstp->rq_chandle.thread_wait = 5 * HZ;
		} else {
			rqstp->rq_chandle.thread_wait = 1 * HZ;
			/*
			 * No idle threads: signal -EBUSY so the caller
			 * can consider spawning another thread. Use
			 * SP_TASK_STARTING to limit this signal to one
			 * thread at a time; the caller clears this flag
			 * after starting a new thread.
			 */
			if (!did_timeout && timeo &&
			    !test_and_set_bit(SP_TASK_STARTING,
					      &pool->sp_flags))
				ret = -EBUSY;
		}

		trace_svc_xprt_dequeue(rqstp);
		svc_handle_xprt(rqstp, xprt);
	}

#if defined(CONFIG_SUNRPC_BACKCHANNEL)
	if (svc_is_backchannel(rqstp)) {
		struct svc_serv *serv = rqstp->rq_server;
		struct rpc_rqst *req;

		req = lwq_dequeue(&serv->sv_cb_list,
				  struct rpc_rqst, rq_bc_list);
		if (req) {
			svc_thread_wake_next(rqstp, pool);
			svc_process_bc(req, rqstp);
		}
	}
#endif
	return ret;
}
EXPORT_SYMBOL_GPL(svc_recv);

/**
 * svc_send - Return reply to client
 * @rqstp: RPC transaction context
 *
 */
void svc_send(struct svc_rqst *rqstp)
{
	struct svc_xprt	*xprt;
	struct xdr_buf	*xb;
	int status;

	xprt = rqstp->rq_xprt;

	/* calculate over-all length */
	xb = &rqstp->rq_res;
	xb->len = xb->head[0].iov_len +
		xb->page_len +
		xb->tail[0].iov_len;
	trace_svc_xdr_sendto(rqstp->rq_xid, xb);
	trace_svc_stats_latency(rqstp);

	status = xprt->xpt_ops->xpo_sendto(rqstp);

	trace_svc_send(rqstp, status);
}

/*
 * Timer function to close old temporary transports, using
 * a mark-and-sweep algorithm.
 */
static void svc_age_temp_xprts(struct timer_list *t)
{
	struct svc_serv *serv = timer_container_of(serv, t, sv_temptimer);
	struct svc_xprt *xprt;
	struct list_head *le, *next;

	dprintk("svc_age_temp_xprts\n");

	if (!spin_trylock_bh(&serv->sv_lock)) {
		/* busy, try again 1 sec later */
		dprintk("svc_age_temp_xprts: busy\n");
		mod_timer(&serv->sv_temptimer, jiffies + HZ);
		return;
	}

	list_for_each_safe(le, next, &serv->sv_tempsocks) {
		xprt = list_entry(le, struct svc_xprt, xpt_list);

		/* First time through, just mark it OLD. Second time
		 * through, close it. */
		if (!test_and_set_bit(XPT_OLD, &xprt->xpt_flags))
			continue;
		if (kref_read(&xprt->xpt_ref) > 1 ||
		    test_bit(XPT_BUSY, &xprt->xpt_flags))
			continue;
		list_del_init(le);
		set_bit(XPT_CLOSE, &xprt->xpt_flags);
		dprintk("queuing xprt %p for closing\n", xprt);

		/* a thread will dequeue and close it soon */
		svc_xprt_enqueue(xprt);
	}
	spin_unlock_bh(&serv->sv_lock);

	mod_timer(&serv->sv_temptimer, jiffies + svc_conn_age_period * HZ);
}

/* Close temporary transports whose xpt_local matches server_addr immediately
 * instead of waiting for them to be picked up by the timer.
 *
 * This is meant to be called from a notifier_block that runs when an ip
 * address is deleted.
 */
void svc_age_temp_xprts_now(struct svc_serv *serv, struct sockaddr *server_addr)
{
	struct svc_xprt *xprt;
	struct list_head *le, *next;
	LIST_HEAD(to_be_closed);

	spin_lock_bh(&serv->sv_lock);
	list_for_each_safe(le, next, &serv->sv_tempsocks) {
		xprt = list_entry(le, struct svc_xprt, xpt_list);
		if (rpc_cmp_addr(server_addr, (struct sockaddr *)
				&xprt->xpt_local)) {
			dprintk("svc_age_temp_xprts_now: found %p\n", xprt);
			list_move(le, &to_be_closed);
		}
	}
	spin_unlock_bh(&serv->sv_lock);

	while (!list_empty(&to_be_closed)) {
		le = to_be_closed.next;
		list_del_init(le);
		xprt = list_entry(le, struct svc_xprt, xpt_list);
		set_bit(XPT_CLOSE, &xprt->xpt_flags);
		set_bit(XPT_KILL_TEMP, &xprt->xpt_flags);
		dprintk("svc_age_temp_xprts_now: queuing xprt %p for closing\n",
				xprt);
		svc_xprt_enqueue(xprt);
	}
}
EXPORT_SYMBOL_GPL(svc_age_temp_xprts_now);

static void call_xpt_users(struct svc_xprt *xprt)
{
	struct svc_xpt_user *u;

	spin_lock(&xprt->xpt_lock);
	while (!list_empty(&xprt->xpt_users)) {
		u = list_first_entry(&xprt->xpt_users, struct svc_xpt_user, list);
		list_del_init(&u->list);
		u->callback(u);
	}
	spin_unlock(&xprt->xpt_lock);
}

/*
 * If rpcbind stops answering, every listener still to be destroyed would
 * only wait out the same timeout again. Drop the flag on the rest of this
 * teardown, which is every listener already marked for close.
 */
static void svc_xprt_clear_rpcb_unreg(struct svc_serv *serv, struct net *net)
{
	struct svc_xprt *xprt;

	spin_lock_bh(&serv->sv_lock);
	list_for_each_entry(xprt, &serv->sv_permsocks, xpt_list)
		if (xprt->xpt_net == net &&
		    test_bit(XPT_CLOSE, &xprt->xpt_flags))
			clear_bit(XPT_RPCB_UNREG, &xprt->xpt_flags);
	spin_unlock_bh(&serv->sv_lock);
}

/*
 * Remove a dead transport
 */
static void svc_delete_xprt(struct svc_xprt *xprt)
{
	struct svc_serv	*serv = xprt->xpt_server;
	struct svc_deferred_req *dr;

	/* unregister with rpcbind for when transport type is TCP or UDP.
	 */
	if (test_bit(XPT_RPCB_UNREG, &xprt->xpt_flags)) {
		struct svc_sock *svsk = container_of(xprt, struct svc_sock,
						     sk_xprt);
		struct socket *sock = svsk->sk_sock;
		unsigned int failures = svc_rpcb_failure_count(serv);

		if (svc_register(serv, xprt->xpt_net, sock->sk->sk_family,
				 sock->sk->sk_protocol, 0) < 0)
			pr_warn("failed to unregister %s with rpcbind\n",
				xprt->xpt_class->xcl_name);

		if (svc_rpcb_failure_count(serv) != failures)
			svc_xprt_clear_rpcb_unreg(serv, xprt->xpt_net);
	}

	if (test_and_set_bit(XPT_DEAD, &xprt->xpt_flags))
		return;

	trace_svc_xprt_detach(xprt);
	xprt->xpt_ops->xpo_detach(xprt);
	if (xprt->xpt_bc_xprt)
		xprt->xpt_bc_xprt->ops->close(xprt->xpt_bc_xprt);

	spin_lock_bh(&serv->sv_lock);
	list_del_init(&xprt->xpt_list);
	if (test_bit(XPT_TEMP, &xprt->xpt_flags) &&
	    !test_bit(XPT_PEER_VALID, &xprt->xpt_flags))
		serv->sv_tmpcnt--;
	spin_unlock_bh(&serv->sv_lock);

	while ((dr = svc_deferred_dequeue(xprt)) != NULL)
		free_deferred(xprt, dr);

	call_xpt_users(xprt);
	svc_xprt_put(xprt);
}

/**
 * svc_xprt_close - Close a client connection
 * @xprt: transport to disconnect
 *
 */
void svc_xprt_close(struct svc_xprt *xprt)
{
	trace_svc_xprt_close(xprt);
	set_bit(XPT_CLOSE, &xprt->xpt_flags);
	if (test_and_set_bit(XPT_BUSY, &xprt->xpt_flags))
		/* someone else will have to effect the close */
		return;
	/*
	 * We expect svc_close_xprt() to work even when no threads are
	 * running (e.g., while configuring the server before starting
	 * any threads), so if the transport isn't busy, we delete
	 * it ourself:
	 */
	svc_delete_xprt(xprt);
}
EXPORT_SYMBOL_GPL(svc_xprt_close);

static int svc_close_list(struct svc_serv *serv, struct list_head *xprt_list, struct net *net)
{
	struct svc_xprt *xprt;
	int ret = 0;

	spin_lock_bh(&serv->sv_lock);
	list_for_each_entry(xprt, xprt_list, xpt_list) {
		if (xprt->xpt_net != net)
			continue;
		ret++;
		set_bit(XPT_CLOSE, &xprt->xpt_flags);
		svc_xprt_enqueue(xprt);
	}
	spin_unlock_bh(&serv->sv_lock);
	return ret;
}

static void svc_clean_up_xprts(struct svc_serv *serv, struct net *net)
{
	struct svc_xprt *xprt;
	int i;

	for (i = 0; i < svc_serv_nrpools(serv); i++) {
		struct svc_pool *pool = &serv->sv_pools[i];
		struct llist_node *q, **t1, *t2;

		q = lwq_dequeue_all(&pool->sp_xprts);
		lwq_for_each_safe(xprt, t1, t2, &q, xpt_ready) {
			if (xprt->xpt_net == net) {
				set_bit(XPT_CLOSE, &xprt->xpt_flags);
				svc_delete_xprt(xprt);
				xprt = NULL;
			}
		}

		if (q)
			lwq_enqueue_batch(q, &pool->sp_xprts);
	}
}

/**
 * svc_xprt_destroy_all - Destroy transports associated with @serv
 * @serv: RPC service to be shut down
 * @net: target network namespace
 * @unregister: true if it is OK to unregister the destroyed xprts
 *
 * Server threads may still be running (especially in the case where the
 * service is still running in other network namespaces).
 *
 * So we shut down sockets the same way we would on a running server, by
 * setting XPT_CLOSE, enqueuing, and letting a thread pick it up to do
 * the close.  In the case there are no such other threads,
 * threads running, svc_clean_up_xprts() does a simple version of a
 * server's main event loop, and in the case where there are other
 * threads, we may need to wait a little while and then check again to
 * see if they're done.
 */
void svc_xprt_destroy_all(struct svc_serv *serv, struct net *net,
			  bool unregister)
{
	int delay = 0;

	while (svc_close_list(serv, &serv->sv_permsocks, net) +
	       svc_close_list(serv, &serv->sv_tempsocks, net)) {

		svc_clean_up_xprts(serv, net);
		msleep(delay++);
	}

	if (unregister && !serv->sv_no_rpcbind)
		svc_rpcb_cleanup(serv, net);
}
EXPORT_SYMBOL_GPL(svc_xprt_destroy_all);

/*
 * Handle defer and revisit of requests
 */

static void svc_revisit(struct cache_deferred_req *dreq, int too_many)
{
	struct svc_deferred_req *dr =
		container_of(dreq, struct svc_deferred_req, handle);
	struct svc_xprt *xprt = dr->xprt;

	spin_lock(&xprt->xpt_lock);
	set_bit(XPT_DEFERRED, &xprt->xpt_flags);
	if (too_many || test_bit(XPT_DEAD, &xprt->xpt_flags)) {
		spin_unlock(&xprt->xpt_lock);
		trace_svc_defer_drop(dr);
		free_deferred(xprt, dr);
		svc_xprt_put(xprt);
		return;
	}
	dr->xprt = NULL;
	list_add(&dr->handle.recent, &xprt->xpt_deferred);
	spin_unlock(&xprt->xpt_lock);
	/*
	 * Order the list_add() above before the XPT_BUSY load in
	 * svc_xprt_enqueue().  spin_unlock() is only a release and does not
	 * order a later load.  svc_tcp_has_wspace() reads xpt_deferred
	 * locklessly; this pairs with the smp_mb__after_atomic() in
	 * svc_xprt_received(), so either that path sees the list entry or
	 * this waker sees XPT_BUSY clear and enqueues.
	 */
	smp_mb();
	trace_svc_defer_queue(dr);
	svc_xprt_enqueue(xprt);
	svc_xprt_put(xprt);
}

/*
 * Save the request off for later processing. Only a Call that fits
 * entirely in rq_arg.head[0] can be deferred.
 */
static struct svc_deferred_req *__svc_defer(struct svc_rqst *rqstp);

static struct cache_deferred_req *svc_defer(struct cache_req *req)
{
	struct svc_rqst *rqstp = container_of(req, struct svc_rqst, rq_chandle);
	struct svc_deferred_req *dr;

	if (!test_bit(RQ_USEDEFERRAL, &rqstp->rq_flags))
		return NULL;
	dr = __svc_defer(rqstp);
	return dr ? &dr->handle : NULL;
}

/**
 * svc_defer_rx_region - Park a request until region pages are free
 * @rqstp: RPC transaction context, its call decoded and nothing run yet
 *
 * Unlike cache deferral this ignores RQ_USEDEFERRAL, which NFSv4 decoding
 * clears: no operation has run, so replaying the call is safe.  The parked
 * request is never dropped as "too many".  Its RQ_SECURE is saved at the
 * first park and restored on every replay.  On success the request is
 * marked RQ_DROPME and the caller sends no reply; svc_revisit_rx_region()
 * replays it.
 *
 * Return: the parked request, or NULL if it cannot be parked.
 */
struct svc_deferred_req *svc_defer_rx_region(struct svc_rqst *rqstp)
{
	return __svc_defer(rqstp);
}
EXPORT_SYMBOL_GPL(svc_defer_rx_region);

/**
 * svc_revisit_rx_region - Replay a request parked by svc_defer_rx_region()
 * @dr: the parked request
 */
void svc_revisit_rx_region(struct svc_deferred_req *dr)
{
	svc_revisit(&dr->handle, 0);
}
EXPORT_SYMBOL_GPL(svc_revisit_rx_region);

static struct svc_deferred_req *__svc_defer(struct svc_rqst *rqstp)
{
	struct svc_deferred_req *dr;

	if (rqstp->rq_arg.page_len)
		return NULL; /* if more than a page, give up FIXME */
	if (rqstp->rq_deferred) {
		dr = rqstp->rq_deferred;
		rqstp->rq_deferred = NULL;
	} else {
		size_t size;

		if (rqstp->rq_arg.len > rqstp->rq_arg.head[0].iov_len)
			return NULL;

		/* FIXME maybe discard if size too large */
		size = sizeof(struct svc_deferred_req) + rqstp->rq_arg.len;
		dr = kmalloc(size, GFP_KERNEL);
		if (dr == NULL)
			return NULL;

		dr->rx_region.parked = false;
		dr->handle.owner = rqstp->rq_server;
		dr->prot = rqstp->rq_prot;
		memcpy(&dr->addr, &rqstp->rq_addr, rqstp->rq_addrlen);
		dr->addrlen = rqstp->rq_addrlen;
		dr->daddr = rqstp->rq_daddr;
		dr->secure = test_bit(RQ_SECURE, &rqstp->rq_flags);
		dr->argslen = rqstp->rq_arg.len >> 2;

		memcpy(dr->args, rqstp->rq_arg.head[0].iov_base,
		       dr->argslen << 2);
	}
	dr->xprt_ctxt = rqstp->rq_xprt_ctxt;
	rqstp->rq_xprt_ctxt = NULL;
	trace_svc_defer(rqstp);
	svc_xprt_get(rqstp->rq_xprt);
	dr->xprt = rqstp->rq_xprt;
	set_bit(RQ_DROPME, &rqstp->rq_flags);

	dr->handle.revisit = svc_revisit;
	return dr;
}

/*
 * recv data from a deferred request into an active one
 */
static noinline int svc_deferred_recv(struct svc_rqst *rqstp)
{
	struct svc_deferred_req *dr = rqstp->rq_deferred;

	trace_svc_defer_recv(dr);

	rqstp->rq_arg.head[0].iov_base = dr->args;
	rqstp->rq_arg.head[0].iov_len = dr->argslen << 2;
	rqstp->rq_arg.page_len = 0;
	rqstp->rq_arg.len     = dr->argslen << 2;
	rqstp->rq_prot        = dr->prot;
	memcpy(&rqstp->rq_addr, &dr->addr, dr->addrlen);
	rqstp->rq_addrlen     = dr->addrlen;
	rqstp->rq_daddr       = dr->daddr;
	rqstp->rq_xprt_ctxt   = dr->xprt_ctxt;
	/*
	 * ->xpo_recvfrom() is bypassed for a deferred request, so the
	 * replaying thread's RQ_SECURE is stale from whatever request
	 * it handled last. Restore the value computed at receive time.
	 */
	assign_bit(RQ_SECURE, &rqstp->rq_flags, dr->secure);

	dr->xprt_ctxt = NULL;
	svc_xprt_received(rqstp->rq_xprt);
	return dr->argslen << 2;
}


static struct svc_deferred_req *svc_deferred_dequeue(struct svc_xprt *xprt)
{
	struct svc_deferred_req *dr = NULL;

	if (!test_bit(XPT_DEFERRED, &xprt->xpt_flags))
		return NULL;
	spin_lock(&xprt->xpt_lock);
	if (!list_empty(&xprt->xpt_deferred)) {
		dr = list_entry(xprt->xpt_deferred.next,
				struct svc_deferred_req,
				handle.recent);
		list_del_init(&dr->handle.recent);
	} else
		clear_bit(XPT_DEFERRED, &xprt->xpt_flags);
	spin_unlock(&xprt->xpt_lock);
	return dr;
}

/**
 * svc_find_listener - find an RPC transport instance
 * @serv: pointer to svc_serv to search
 * @xcl_name: C string containing transport's class name
 * @net: owner net pointer
 * @sa: sockaddr containing address
 *
 * Return the transport instance pointer for the endpoint accepting
 * connections/peer traffic from the specified transport class,
 * and matching sockaddr.
 */
struct svc_xprt *svc_find_listener(struct svc_serv *serv, const char *xcl_name,
				   struct net *net, const struct sockaddr *sa)
{
	struct svc_xprt *xprt;
	struct svc_xprt *found = NULL;

	spin_lock_bh(&serv->sv_lock);
	list_for_each_entry(xprt, &serv->sv_permsocks, xpt_list) {
		if (xprt->xpt_net != net)
			continue;
		if (strcmp(xprt->xpt_class->xcl_name, xcl_name))
			continue;
		if (!rpc_cmp_addr_port(sa, (struct sockaddr *)&xprt->xpt_local))
			continue;
		found = xprt;
		svc_xprt_get(xprt);
		break;
	}
	spin_unlock_bh(&serv->sv_lock);
	return found;
}
EXPORT_SYMBOL_GPL(svc_find_listener);

/**
 * svc_find_xprt - find an RPC transport instance
 * @serv: pointer to svc_serv to search
 * @xcl_name: C string containing transport's class name
 * @net: owner net pointer
 * @af: Address family of transport's local address
 * @port: transport's IP port number
 *
 * Return the transport instance pointer for the endpoint accepting
 * connections/peer traffic from the specified transport class,
 * address family and port.
 *
 * Specifying 0 for the address family or port is effectively a
 * wild-card, and will result in matching the first transport in the
 * service's list that has a matching class name.
 */
struct svc_xprt *svc_find_xprt(struct svc_serv *serv, const char *xcl_name,
			       struct net *net, const sa_family_t af,
			       const unsigned short port)
{
	struct svc_xprt *xprt;
	struct svc_xprt *found = NULL;

	/* Sanity check the args */
	if (serv == NULL || xcl_name == NULL)
		return found;

	spin_lock_bh(&serv->sv_lock);
	list_for_each_entry(xprt, &serv->sv_permsocks, xpt_list) {
		if (xprt->xpt_net != net)
			continue;
		if (strcmp(xprt->xpt_class->xcl_name, xcl_name))
			continue;
		if (af != AF_UNSPEC && af != xprt->xpt_local.ss_family)
			continue;
		if (port != 0 && port != svc_xprt_local_port(xprt))
			continue;
		found = xprt;
		svc_xprt_get(xprt);
		break;
	}
	spin_unlock_bh(&serv->sv_lock);
	return found;
}
EXPORT_SYMBOL_GPL(svc_find_xprt);

static int svc_one_xprt_name(const struct svc_xprt *xprt,
			     char *pos, int remaining)
{
	int len;

	len = snprintf(pos, remaining, "%s %u\n",
			xprt->xpt_class->xcl_name,
			svc_xprt_local_port(xprt));
	if (len >= remaining)
		return -ENAMETOOLONG;
	return len;
}

/**
 * svc_xprt_names - format a buffer with a list of transport names
 * @serv: pointer to an RPC service
 * @buf: pointer to a buffer to be filled in
 * @buflen: length of buffer to be filled in
 *
 * Fills in @buf with a string containing a list of transport names,
 * each name terminated with '\n'.
 *
 * Returns positive length of the filled-in string on success; otherwise
 * a negative errno value is returned if an error occurs.
 */
int svc_xprt_names(struct svc_serv *serv, char *buf, const int buflen)
{
	struct svc_xprt *xprt;
	int len, totlen;
	char *pos;

	/* Sanity check args */
	if (!serv)
		return 0;

	spin_lock_bh(&serv->sv_lock);

	pos = buf;
	totlen = 0;
	list_for_each_entry(xprt, &serv->sv_permsocks, xpt_list) {
		len = svc_one_xprt_name(xprt, pos, buflen - totlen);
		if (len < 0) {
			*buf = '\0';
			totlen = len;
		}
		if (len <= 0)
			break;

		pos += len;
		totlen += len;
	}

	spin_unlock_bh(&serv->sv_lock);
	return totlen;
}
EXPORT_SYMBOL_GPL(svc_xprt_names);

/*----------------------------------------------------------------------------*/

static void *svc_pool_stats_start(struct seq_file *m, loff_t *pos)
{
	unsigned int pidx = (unsigned int)*pos;
	struct svc_info *si = m->private;

	dprintk("svc_pool_stats_start, *pidx=%u\n", pidx);

	mutex_lock(si->mutex);

	if (!pidx)
		return SEQ_START_TOKEN;
	if (!si->serv)
		return NULL;
	return pidx > svc_serv_nrpools(si->serv) ? NULL
		: &si->serv->sv_pools[pidx - 1];
}

static void *svc_pool_stats_next(struct seq_file *m, void *p, loff_t *pos)
{
	struct svc_pool *pool = p;
	struct svc_info *si = m->private;
	struct svc_serv *serv = si->serv;

	dprintk("svc_pool_stats_next, *pos=%llu\n", *pos);

	if (!serv) {
		pool = NULL;
	} else if (p == SEQ_START_TOKEN) {
		pool = &serv->sv_pools[0];
	} else {
		unsigned int pidx = (pool - &serv->sv_pools[0]);
		if (pidx < svc_serv_nrpools(serv) - 1)
			pool = &serv->sv_pools[pidx+1];
		else
			pool = NULL;
	}
	++*pos;
	return pool;
}

static void svc_pool_stats_stop(struct seq_file *m, void *p)
{
	struct svc_info *si = m->private;

	mutex_unlock(si->mutex);
}

static int svc_pool_stats_show(struct seq_file *m, void *p)
{
	struct svc_info *si = m->private;
	struct svc_pool *pool = p;
	struct svc_pool_ext *ext;

	if (p == SEQ_START_TOKEN) {
		seq_puts(m, "# pool packets-arrived sockets-enqueued threads-woken threads-timedout sibling-wakes steals spin-hits\n");
		return 0;
	}

	ext = &si->serv->sv_pool_ext[pool->sp_id];
	seq_printf(m, "%u %llu %llu %llu 0 %llu %llu %llu\n",
		   pool->sp_id,
		   percpu_counter_sum_positive(&pool->sp_messages_arrived),
		   percpu_counter_sum_positive(&pool->sp_sockets_queued),
		   percpu_counter_sum_positive(&pool->sp_threads_woken),
		   percpu_counter_sum_positive(&ext->sibling_wakes),
		   percpu_counter_sum_positive(&ext->steals),
		   percpu_counter_sum_positive(&ext->spin_hits));

	return 0;
}

static const struct seq_operations svc_pool_stats_seq_ops = {
	.start	= svc_pool_stats_start,
	.next	= svc_pool_stats_next,
	.stop	= svc_pool_stats_stop,
	.show	= svc_pool_stats_show,
};

int svc_pool_stats_open(struct svc_info *info, struct file *file)
{
	struct seq_file *seq;
	int err;

	err = seq_open(file, &svc_pool_stats_seq_ops);
	if (err)
		return err;
	seq = file->private_data;
	seq->private = info;

	return 0;
}
EXPORT_SYMBOL(svc_pool_stats_open);

/*----------------------------------------------------------------------------*/
