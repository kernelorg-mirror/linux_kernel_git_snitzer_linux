// SPDX-License-Identifier: GPL-2.0-only
/*
 * linux/net/sunrpc/svc.c
 *
 * High-level RPC service routines
 *
 * Copyright (C) 1995, 1996 Olaf Kirch <okir@monad.swb.de>
 *
 * Multiple threads pools and NUMAisation
 * Copyright (c) 2006 Silicon Graphics, Inc.
 * by Greg Banks <gnb@melbourne.sgi.com>
 */

#include <linux/linkage.h>
#include <linux/sched/signal.h>
#include <linux/errno.h>
#include <linux/net.h>
#include <linux/in.h>
#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <kunit/visibility.h>

#include <linux/sunrpc/types.h>
#include <linux/sunrpc/xdr.h>
#include <linux/sunrpc/stats.h>
#include <linux/sunrpc/svcsock.h>
#include <linux/sunrpc/clnt.h>
#include <linux/sunrpc/bc_xprt.h>

#include <trace/events/sunrpc.h>

#include "fail.h"
#include "sunrpc.h"

#define RPCDBG_FACILITY	RPCDBG_SVCDSP

static void svc_unregister(const struct svc_serv *serv, struct net *net);

/*
 * Structure for mapping nodes to pools and vice versa.
 * Setup once during sunrpc initialisation.
 */

#define SVC_POOL_DEFAULT	SVC_POOL_AUTO

/*
 * Mode for mapping cpus to pools.
 */
enum {
	SVC_POOL_AUTO = -1,	/* choose one of the others */
	SVC_POOL_GLOBAL,	/* no mapping, just a single global pool
				 * (legacy & UP mode) */
	SVC_POOL_PERCPU,	/* one pool per cpu */
	SVC_POOL_PERNODE	/* one pool per numa node */
};

struct svc_pool_map {
	int count;			/* How many svc_servs use us */
	int mode;			/* Note: int not enum to avoid
					 * warnings about "enumeration value
					 * not handled in switch" */
	unsigned int npools;
	unsigned int *pool_to;		/* maps pool id to cpu or node */
	unsigned int *to_pool;		/* maps cpu or node to pool id */
};

static struct svc_pool_map svc_pool_map = {
	.mode = SVC_POOL_DEFAULT
};

static DEFINE_MUTEX(svc_pool_map_mutex);/* protects svc_pool_map.count only */

static int
__param_set_pool_mode(const char *val, struct svc_pool_map *m)
{
	int err, mode;

	mutex_lock(&svc_pool_map_mutex);

	err = 0;
	if (!strncmp(val, "auto", 4))
		mode = SVC_POOL_AUTO;
	else if (!strncmp(val, "global", 6))
		mode = SVC_POOL_GLOBAL;
	else if (!strncmp(val, "percpu", 6))
		mode = SVC_POOL_PERCPU;
	else if (!strncmp(val, "pernode", 7))
		mode = SVC_POOL_PERNODE;
	else
		err = -EINVAL;

	if (err)
		goto out;

	if (m->count == 0)
		m->mode = mode;
	else if (mode != m->mode)
		err = -EBUSY;
out:
	mutex_unlock(&svc_pool_map_mutex);
	return err;
}

static int
param_set_pool_mode(const char *val, const struct kernel_param *kp)
{
	struct svc_pool_map *m = kp->arg;

	return __param_set_pool_mode(val, m);
}

int sunrpc_set_pool_mode(const char *val)
{
	return __param_set_pool_mode(val, &svc_pool_map);
}
EXPORT_SYMBOL(sunrpc_set_pool_mode);

/**
 * sunrpc_get_pool_mode - get the current pool_mode for the host
 * @buf: where to write the current pool_mode
 * @size: size of @buf
 *
 * Grab the current pool_mode from the svc_pool_map and write
 * the resulting string to @buf. Returns the number of characters
 * written to @buf (a'la snprintf()).
 */
int
sunrpc_get_pool_mode(char *buf, size_t size)
{
	struct svc_pool_map *m = &svc_pool_map;

	switch (m->mode)
	{
	case SVC_POOL_AUTO:
		return snprintf(buf, size, "auto");
	case SVC_POOL_GLOBAL:
		return snprintf(buf, size, "global");
	case SVC_POOL_PERCPU:
		return snprintf(buf, size, "percpu");
	case SVC_POOL_PERNODE:
		return snprintf(buf, size, "pernode");
	default:
		return snprintf(buf, size, "%d", m->mode);
	}
}
EXPORT_SYMBOL(sunrpc_get_pool_mode);

static int
param_get_pool_mode(char *buf, const struct kernel_param *kp)
{
	char str[16];
	int len;

	len = sunrpc_get_pool_mode(str, ARRAY_SIZE(str));

	/* Ensure we have room for newline and NUL */
	len = min_t(int, len, ARRAY_SIZE(str) - 2);

	/* tack on the newline */
	str[len] = '\n';
	str[len + 1] = '\0';

	return sysfs_emit(buf, "%s", str);
}

module_param_call(pool_mode, param_set_pool_mode, param_get_pool_mode,
		  &svc_pool_map, 0644);

/*
 * "auto" picks one pool per cpu on every topology: a node-wide pool
 * serialises every wakeup of the node's threads on that pool's queue.
 * Idle threads of sibling pools pick up a busy pool's backlog (see
 * svc_pool_wake_sibling()).
 */
static int
svc_pool_map_choose_mode(void)
{
	return SVC_POOL_PERCPU;
}

/*
 * Allocate the to_pool[] and pool_to[] arrays.
 * Returns 0 on success or an errno.
 */
static int
svc_pool_map_alloc_arrays(struct svc_pool_map *m, unsigned int maxpools)
{
	m->to_pool = kcalloc(maxpools, sizeof(unsigned int), GFP_KERNEL);
	if (!m->to_pool)
		goto fail;
	m->pool_to = kcalloc(maxpools, sizeof(unsigned int), GFP_KERNEL);
	if (!m->pool_to)
		goto fail_free;

	return 0;

fail_free:
	kfree(m->to_pool);
	m->to_pool = NULL;
fail:
	return -ENOMEM;
}

/*
 * Initialise the pool map for one pool per NUMA node.
 * Returns number of pools or <0 on error.
 */
static int
svc_pool_map_init_pernode(struct svc_pool_map *m)
{
	unsigned int maxpools = nr_node_ids;
	unsigned int pidx = 0;
	unsigned int node;
	int err;

	err = svc_pool_map_alloc_arrays(m, maxpools);
	if (err)
		return err;

	for_each_node_with_cpus(node) {
		/* some architectures (e.g. SN2) have cpuless nodes */
		BUG_ON(pidx > maxpools);
		m->to_pool[node] = pidx;
		m->pool_to[pidx] = node;
		pidx++;
	}
	/* nodes brought online later all get mapped to pool0, sorry */

	return pidx;
}

static unsigned int
svc_pool_cpu_node(unsigned int cpu,
		  int (*node_of)(unsigned int cpu, const void *arg),
		  const void *arg, unsigned int nr_nodes)
{
	int n = node_of(cpu, arg);

	return n < 0 || n >= (int)nr_nodes ? 0 : n;
}

/**
 * svc_pool_map_fill_percpu - number one pool per cpu, node by node
 * @to_pool: cpu to pool id, filled for every cpu in @possible
 * @pool_to: pool id to cpu, filled for every pool
 * @cpus: the cpus that get a pool (the online cpus)
 * @possible: every cpu that can come online
 * @node_of: the node of a cpu
 * @arg: passed to @node_of
 * @nr_nodes: node ids are below this; others count as node 0
 *
 * Pools of one node are numbered consecutively, so each node's pools
 * form one run of pool ids, which the sibling wake and steal look in
 * before they look at other nodes.
 * A cpu of @possible that has no pool yet (it comes online later)
 * queues on the first pool of its own node, or on pool 0 when its node
 * has no pool.
 *
 * Returns the number of pools.
 */
VISIBLE_IF_KUNIT unsigned int
svc_pool_map_fill_percpu(unsigned int *to_pool, unsigned int *pool_to,
			 const struct cpumask *cpus,
			 const struct cpumask *possible,
			 int (*node_of)(unsigned int cpu, const void *arg),
			 const void *arg, unsigned int nr_nodes)
{
	unsigned int pidx = 0;
	unsigned int node, cpu;

	for (node = 0; node < nr_nodes; node++) {
		unsigned int first = pidx;

		for_each_cpu(cpu, cpus) {
			if (svc_pool_cpu_node(cpu, node_of, arg,
					      nr_nodes) != node)
				continue;
			to_pool[cpu] = pidx;
			pool_to[pidx] = cpu;
			pidx++;
		}
		for_each_cpu_andnot(cpu, possible, cpus) {
			if (svc_pool_cpu_node(cpu, node_of, arg,
					      nr_nodes) == node)
				to_pool[cpu] = pidx > first ? first : 0;
		}
	}
	return pidx;
}
EXPORT_SYMBOL_IF_KUNIT(svc_pool_map_fill_percpu);

static int svc_cpu_node(unsigned int cpu, const void *arg)
{
	return cpu_to_node(cpu);
}

/*
 * Initialise the pool map for SVC_POOL_PERCPU mode.
 * Returns number of pools or <0 on error.
 */
static int
svc_pool_map_init_percpu(struct svc_pool_map *m)
{
	int err;

	err = svc_pool_map_alloc_arrays(m, nr_cpu_ids);
	if (err)
		return err;
	return svc_pool_map_fill_percpu(m->to_pool, m->pool_to,
					cpu_online_mask, cpu_possible_mask,
					svc_cpu_node, NULL, nr_node_ids);
}


/*
 * Add a reference to the global map of nodes to pools (and
 * vice versa) if pools are in use.
 * Initialise the map if we're the first user.
 * Returns the number of pools, or 0 on failure.
 */
static unsigned int
svc_pool_map_get(void)
{
	struct svc_pool_map *m = &svc_pool_map;
	int npools = -1;

	mutex_lock(&svc_pool_map_mutex);
	if (m->count++) {
		mutex_unlock(&svc_pool_map_mutex);
		return m->npools;
	}

	if (m->mode == SVC_POOL_AUTO)
		m->mode = svc_pool_map_choose_mode();

	switch (m->mode) {
	case SVC_POOL_PERCPU:
		npools = svc_pool_map_init_percpu(m);
		break;
	case SVC_POOL_PERNODE:
		npools = svc_pool_map_init_pernode(m);
		break;
	}

	if (npools <= 0) {
		/* default, or memory allocation failure */
		npools = 1;
		m->mode = SVC_POOL_GLOBAL;
	}
	m->npools = npools;
	mutex_unlock(&svc_pool_map_mutex);
	return npools;
}

/*
 * Drop a reference to the global map of nodes to pools.
 * When the last reference is dropped, the map data is
 * freed; this allows the sysadmin to change the pool.
 */
static void
svc_pool_map_put(void)
{
	struct svc_pool_map *m = &svc_pool_map;

	mutex_lock(&svc_pool_map_mutex);
	if (!--m->count) {
		kfree(m->to_pool);
		m->to_pool = NULL;
		kfree(m->pool_to);
		m->pool_to = NULL;
		m->npools = 0;
	}
	mutex_unlock(&svc_pool_map_mutex);
}

static int svc_pool_map_get_node(unsigned int pidx)
{
	const struct svc_pool_map *m = &svc_pool_map;

	if (m->count) {
		if (m->mode == SVC_POOL_PERCPU)
			return cpu_to_node(m->pool_to[pidx]);
		if (m->mode == SVC_POOL_PERNODE)
			return m->pool_to[pidx];
	}
	return NUMA_NO_NODE;
}

/* The NUMA node of @pool's threads (NUMA_NO_NODE if not per-node). */
int svc_pool_node(struct svc_serv *serv, struct svc_pool *pool)
{
	/* The pool map describes only pooled services. */
	if (!serv->sv_is_pooled)
		return NUMA_NO_NODE;
	return svc_pool_map_get_node(pool->sp_id);
}

/* An rtc attempt that set RQ_RTC_COUNT ended on its work item: count it. */
void svc_rtc_count_proc(struct svc_rqst *rqstp)
{
	if (test_and_clear_bit(RQ_RTC_COUNT, &rqstp->rq_flags))
		this_cpu_inc(rqstp->rq_server->sv_stats->vs_count
				[rqstp->rq_vers][rqstp->rq_proc]);
}

/*
 * Set the given thread's cpus_allowed mask so that it
 * will only run on cpus in the given pool.
 */
static inline void
svc_pool_map_set_cpumask(struct task_struct *task, unsigned int pidx)
{
	struct svc_pool_map *m = &svc_pool_map;
	unsigned int id = m->pool_to[pidx];	/* a cpu or a node */

	/*
	 * The caller checks for more than one pool, which
	 * implies that we've been initialized.
	 */
	WARN_ON_ONCE(m->count == 0);
	if (m->count == 0)
		return;

	switch (m->mode) {
	case SVC_POOL_PERCPU:
		/*
		 * kthread() resets a plain mask to the housekeeping cpus
		 * when the thread first runs; a preferred affinity
		 * survives that.
		 */
		kthread_affine_preferred(task, cpumask_of(id));
		break;
	case SVC_POOL_PERNODE:
		set_cpus_allowed_ptr(task, cpumask_of_node(id));
		break;
	}
}

/**
 * svc_serv_nrpools - number of thread pools backing a service
 * @serv: An RPC service
 *
 * Pooled services all share the global svc_pool_map, so their pool count
 * is svc_pool_map.npools. Unpooled services have a single pool. Reading
 * npools without svc_pool_map_mutex is safe: a pooled service holds a map
 * reference for its whole lifetime, so npools is stable once set.
 *
 * Return value:
 *   The number of pools in @serv
 */
unsigned int svc_serv_nrpools(const struct svc_serv *serv)
{
	return serv->sv_is_pooled ? svc_pool_map.npools : 1;
}
EXPORT_SYMBOL_GPL(svc_serv_nrpools);

/**
 * svc_pool_for_cpu_on - Select the pool that serves @cpu
 * @serv: An RPC service
 * @cpu: an online CPU
 *
 * Use @cpu and the svc_pool_map to select the svc thread pool to use.
 * Once initialized, the svc_pool_map does not change.
 *
 * Return value:
 *   A pointer to an svc_pool
 */
struct svc_pool *svc_pool_for_cpu_on(struct svc_serv *serv, unsigned int cpu)
{
	unsigned int nrpools = svc_serv_nrpools(serv);
	struct svc_pool_map *m = &svc_pool_map;
	unsigned int pidx, i;

	if (nrpools <= 1)
		return serv->sv_pools;

	/*
	 * It's possible to have a pool with no threads. Userland can just set
	 * things up this way directly. Also, when threads are autodistributed
	 * they are spread evenly across the pools, but when there are fewer
	 * threads than pools some pools can end up with none.
	 *
	 * A transport enqueued on a threadless pool goes to the pool of the
	 * same node that serves it (svc_pool_ext_layout()), and from there,
	 * should that one have lost its threads too, to the next populated
	 * pool, trading NUMA locality for a guarantee that the transport is
	 * serviced.
	 */
	if (m->mode == SVC_POOL_PERCPU)
		pidx = m->to_pool[cpu];
	else
		pidx = m->to_pool[cpu_to_node(cpu)];
	if (likely(data_race(serv->sv_pools[pidx].sp_nrthreads)))
		return &serv->sv_pools[pidx];
	pidx = READ_ONCE(serv->sv_pool_ext[pidx].serve);
	for (i = 0; i < nrpools; i++) {
		struct svc_pool *pool = &serv->sv_pools[pidx];

		/* This is set under the service mutex and rarely ever
		 * changes. A data race here is harmless.
		 */
		if (data_race(pool->sp_nrthreads))
			return pool;

		if (++pidx >= nrpools)
			pidx = 0;
	}

	/* No pool has any threads; nothing can service the transport. */
	return &serv->sv_pools[pidx];
}

/**
 * svc_pool_for_cpu - Select pool to run a thread on this cpu
 * @serv: An RPC service
 *
 * Return value:
 *   A pointer to the svc_pool that serves the active CPU
 */
struct svc_pool *svc_pool_for_cpu(struct svc_serv *serv)
{
	return svc_pool_for_cpu_on(serv, raw_smp_processor_id());
}

static int svc_rpcb_setup(struct svc_serv *serv, struct net *net)
{
	int err;

	err = rpcb_create_local(net);
	if (err)
		return err;

	/* Remove any stale portmap registrations */
	svc_unregister(serv, net);
	return 0;
}

void svc_rpcb_cleanup(struct svc_serv *serv, struct net *net)
{
	svc_unregister(serv, net);
	rpcb_put_local(net);
}

static int svc_uses_rpcbind(struct svc_serv *serv)
{
	unsigned int		p, i;

	for (p = 0; p < serv->sv_nprogs; p++) {
		struct svc_program *progp = &serv->sv_programs[p];

		for (i = 0; i < progp->pg_nvers; i++) {
			if (progp->pg_vers[i] == NULL)
				continue;
			if (!progp->pg_vers[i]->vs_hidden)
				return 1;
		}
	}

	return 0;
}

int svc_bind(struct svc_serv *serv, struct net *net)
{
	if (serv->sv_no_rpcbind)
		return 0;
	if (!svc_uses_rpcbind(serv))
		return 0;
	return svc_rpcb_setup(serv, net);
}
EXPORT_SYMBOL_GPL(svc_bind);

#if defined(CONFIG_SUNRPC_BACKCHANNEL)
static void
__svc_init_bc(struct svc_serv *serv)
{
	lwq_init(&serv->sv_cb_list);
}
#else
static void
__svc_init_bc(struct svc_serv *serv)
{
}
#endif

static int svc_pool_init_counters(struct svc_pool *pool)
{
	int err;

	err = percpu_counter_init(&pool->sp_messages_arrived, 0, GFP_KERNEL);
	if (err)
		return err;
	err = percpu_counter_init(&pool->sp_sockets_queued, 0, GFP_KERNEL);
	if (err)
		goto err_sockets;
	err = percpu_counter_init(&pool->sp_threads_woken, 0, GFP_KERNEL);
	if (err)
		goto err_threads;
	return 0;

err_threads:
	percpu_counter_destroy(&pool->sp_sockets_queued);
err_sockets:
	percpu_counter_destroy(&pool->sp_messages_arrived);
	return err;
}

static void svc_pool_destroy_counters(struct svc_pool *pool)
{
	percpu_counter_destroy(&pool->sp_messages_arrived);
	percpu_counter_destroy(&pool->sp_sockets_queued);
	percpu_counter_destroy(&pool->sp_threads_woken);
}

static int svc_pool_ext_init(struct svc_pool_ext *ext)
{
	int err;

	ext->node = NUMA_NO_NODE;
	err = percpu_counter_init(&ext->sibling_wakes, 0, GFP_KERNEL);
	if (err)
		return err;
	err = percpu_counter_init(&ext->steals, 0, GFP_KERNEL);
	if (err)
		goto err_steals;
	err = percpu_counter_init(&ext->spin_hits, 0, GFP_KERNEL);
	if (err)
		goto err_spin;
	return 0;

err_spin:
	percpu_counter_destroy(&ext->steals);
err_steals:
	percpu_counter_destroy(&ext->sibling_wakes);
	return err;
}

static void svc_pool_ext_destroy(struct svc_pool_ext *ext)
{
	percpu_counter_destroy(&ext->sibling_wakes);
	percpu_counter_destroy(&ext->steals);
	percpu_counter_destroy(&ext->spin_hits);
}

static bool svc_pool_has_threads(const struct svc_pool *pool)
{
	return READ_ONCE(pool->sp_nrthrmax);
}

/* Pool @d places after (@d > 0) or before @i, in the run @first + @count */
static unsigned int svc_pool_run_step(unsigned int first, unsigned int count,
				      unsigned int i, int d)
{
	return first + (i - first + count + d) % count;
}

/* Lay out one run of @count pools from @first; see svc_pool_ext_layout() */
static void svc_pool_ext_layout_run(const struct svc_pool *pools,
				    struct svc_pool_ext *ext,
				    unsigned int first, unsigned int count)
{
	unsigned int range = first << 16 | count;
	unsigned int cur = UINT_MAX;
	unsigned int i, j, d, back, fwd;

	/* the last pool with threads, which comes before the first one */
	for (j = count; j-- > 0 && cur == UINT_MAX;)
		if (svc_pool_has_threads(&pools[first + j]))
			cur = first + j;

	/* range, and the ring of pools with threads, via prev and next */
	for (j = 0; j < count; j++) {
		i = first + j;
		WRITE_ONCE(ext[i].range, range);
		if (cur == UINT_MAX || !svc_pool_has_threads(&pools[i])) {
			/* serve and next of a served pool are set below */
			if (cur == UINT_MAX) {
				WRITE_ONCE(ext[i].serve, i);
				WRITE_ONCE(ext[i].next, i);
			}
			WRITE_ONCE(ext[i].prev, i);
			WRITE_ONCE(ext[i].gap, 0);
			continue;
		}
		WRITE_ONCE(ext[i].prev, cur);
		WRITE_ONCE(ext[cur].next, i);
		cur = i;
	}
	if (cur == UINT_MAX)
		return;

	/*
	 * serve and gap: a pool without threads is served by the nearest
	 * pool with threads, the next one on a tie.  So each of those serves
	 * the nearer half of the threadless pools on either side.
	 */
	for (j = 0; j < count; j++) {
		i = first + j;
		if (!svc_pool_has_threads(&pools[i]))
			continue;
		back = (i - ext[i].prev + count - 1) % count;
		fwd = (ext[i].next - i + count - 1) % count;
		back = (back + 1) / 2;
		fwd /= 2;
		WRITE_ONCE(ext[i].serve, i);
		WRITE_ONCE(ext[i].gap, back << 16 | fwd);
		for (d = 1; d <= back + fwd; d++) {
			unsigned int k = d <= back ?
				svc_pool_run_step(first, count, i, -(int)d) :
				svc_pool_run_step(first, count, i, d - back);

			WRITE_ONCE(ext[k].serve, i);
			WRITE_ONCE(ext[k].next, i);
		}
	}
}

/**
 * svc_pool_ext_layout - recompute which pools serve which
 * @pools: a service's pools, with their sp_nrthrmax set
 * @ext: their svc_pool_ext, with node set
 * @npools: number of pools
 *
 * Each node's pools are one run of consecutive pool ids (per-cpu pools
 * are numbered node by node).  Within its run, a pool without threads is
 * served by the nearest pool that has threads, the next one on a tie: its
 * enqueues go there, and that pool's threads look at its queue before
 * they sleep.  Pools with threads are chained both ways (prev, next), so
 * svc_pool_wake_sibling() and the steal walk step from one to the next.
 * When some node has no threads at all, every pool is put in one run, so
 * that node is served from the others.
 *
 * Otherwise each pool's far is the first pool of the next node's run (by
 * node order, wrapping); following far from node to node reaches every
 * node.  The wake and the steal walk go there only when their own node
 * has no idle thread or no work at all, and look at the pools near their
 * own relative place (svc_pool_far_window()).  far is the pool itself
 * when there is one run, or when every run is a single pool (pernode), so
 * those never reach across nodes.
 *
 * Each node's run of pools (by node, whatever the runs above) also has
 * an idle map in the service's idle lines, from a line of its own, with
 * bit place of each pool (svc_pool_idle_add()).  Node runs change only
 * when the pools' nodes are set, before the service has threads, so no
 * bit moves while it may be set.  Without lines (a test rig) idle is
 * NULL and the maps are not kept.
 *
 * Called whenever a pool's thread count changes, under the caller's
 * service mutex.  It allocates nothing and takes no lock.
 */
VISIBLE_IF_KUNIT void svc_pool_ext_layout(const struct svc_pool *pools,
					  struct svc_pool_ext *ext,
					  unsigned int npools)
{
	unsigned int first, end, next, j, line = 0;
	bool whole = false, multi = false;

	/* range packs a pool id and a pool count into 16 bits each */
	BUILD_BUG_ON(NR_CPUS > 0xffff || MAX_NUMNODES > 0xffff);

	/* a node run of c pools takes at most c lines: npools suffice */
	for (first = 0; first < npools; first = end) {
		for (end = first + 1; end < npools &&
		     ext[end].node == ext[first].node; end++)
			;
		for (j = first; j < end; j++) {
			WRITE_ONCE(ext[j].place, j - first);
			WRITE_ONCE(ext[j].idle,
				   ext[0].lines ? ext[0].lines[line].map : NULL);
		}
		line += DIV_ROUND_UP(end - first, SVC_POOL_IDLE_LINE_BITS);
	}

	for (first = 0; first < npools && !whole; first = end) {
		bool served = false;

		for (end = first; end < npools &&
		     ext[end].node == ext[first].node; end++)
			served |= svc_pool_has_threads(&pools[end]);
		whole = !served;
	}
	for (first = 0; first < npools; first = end) {
		end = first + 1;
		if (whole)
			end = npools;
		while (end < npools && ext[end].node == ext[first].node)
			end++;
		svc_pool_ext_layout_run(pools, ext, first, end - first);
		multi |= end - first > 1;
	}
	for (first = 0; first < npools; first = end) {
		for (end = first + 1; end < npools &&
		     ext[end].node == ext[first].node; end++)
			;
		next = end < npools ? end : 0;
		for (j = first; j < end; j++)
			WRITE_ONCE(ext[j].far,
				   whole || !multi || next == first ? j : next);
	}
}
EXPORT_SYMBOL_IF_KUNIT(svc_pool_ext_layout);

/*
 * Create an RPC service
 */
static struct svc_serv *
__svc_create(struct svc_program *prog, int nprogs, struct svc_stat *stats,
	     unsigned int bufsize, int npools, int (*threadfn)(void *data))
{
	struct svc_serv	*serv;
	unsigned int vers;
	unsigned int xdrsize;
	unsigned int i;

	if (!(serv = kzalloc_obj(*serv)))
		return NULL;
	serv->sv_name      = prog->pg_name;
	serv->sv_programs  = prog;
	serv->sv_nprogs    = nprogs;
	serv->sv_stats     = stats;
	if (bufsize > RPCSVC_MAXPAYLOAD)
		bufsize = RPCSVC_MAXPAYLOAD;
	serv->sv_max_payload = bufsize? bufsize : 4096;
	serv->sv_max_mesg  = roundup(serv->sv_max_payload + PAGE_SIZE, PAGE_SIZE);
	serv->sv_threadfn = threadfn;
	xdrsize = 0;
	for (i = 0; i < nprogs; i++) {
		struct svc_program *progp = &prog[i];

		progp->pg_lovers = progp->pg_nvers-1;
		for (vers = 0; vers < progp->pg_nvers ; vers++)
			if (progp->pg_vers[vers]) {
				progp->pg_hivers = vers;
				if (progp->pg_lovers > vers)
					progp->pg_lovers = vers;
				if (progp->pg_vers[vers]->vs_xdrsize > xdrsize)
					xdrsize = progp->pg_vers[vers]->vs_xdrsize;
			}
	}
	serv->sv_xdrsize   = xdrsize;
	INIT_LIST_HEAD(&serv->sv_tempsocks);
	INIT_LIST_HEAD(&serv->sv_permsocks);
	timer_setup(&serv->sv_temptimer, NULL, 0);
	spin_lock_init(&serv->sv_lock);

	__svc_init_bc(serv);

	serv->sv_pools = kzalloc_objs(struct svc_pool, npools);
	if (!serv->sv_pools) {
		kfree(serv);
		return NULL;
	}
	serv->sv_pool_ext = kzalloc_objs(struct svc_pool_ext, npools);
	if (!serv->sv_pool_ext) {
		kfree(serv->sv_pools);
		kfree(serv);
		return NULL;
	}
	/* a power of two, so kmalloc aligns it to a cache line at least */
	serv->sv_pool_ext[0].lines =
		kzalloc(roundup_pow_of_two(npools *
					   sizeof(struct svc_pool_idle_line)),
			GFP_KERNEL);
	if (!serv->sv_pool_ext[0].lines) {
		kfree(serv->sv_pool_ext);
		kfree(serv->sv_pools);
		kfree(serv);
		return NULL;
	}

	for (i = 0; i < npools; i++) {
		struct svc_pool *pool = &serv->sv_pools[i];

		dprintk("svc: initialising pool %u for %s\n",
				i, serv->sv_name);

		pool->sp_id = i;
		lwq_init(&pool->sp_xprts);
		lwq_init(&pool->sp_rtc_rqsts);
		INIT_LIST_HEAD(&pool->sp_all_threads);
		init_llist_head(&pool->sp_idle_threads);

		if (svc_pool_init_counters(pool))
			goto out_err;
		if (svc_pool_ext_init(&serv->sv_pool_ext[i])) {
			svc_pool_destroy_counters(pool);
			goto out_err;
		}
		serv->sv_pool_ext[i].lines = serv->sv_pool_ext[0].lines;
	}
	svc_pool_ext_layout(serv->sv_pools, serv->sv_pool_ext, npools);

	return serv;

out_err:
	while (i--) {
		svc_pool_ext_destroy(&serv->sv_pool_ext[i]);
		svc_pool_destroy_counters(&serv->sv_pools[i]);
	}
	kfree(serv->sv_pool_ext[0].lines);
	kfree(serv->sv_pool_ext);
	kfree(serv->sv_pools);
	kfree(serv);
	return NULL;
}

/**
 * svc_create - Create an RPC service
 * @prog: the RPC program the new service will handle
 * @bufsize: maximum message size for @prog
 * @threadfn: a function to service RPC requests for @prog
 *
 * Returns an instantiated struct svc_serv object or NULL.
 */
struct svc_serv *svc_create(struct svc_program *prog, unsigned int bufsize,
			    int (*threadfn)(void *data))
{
	return __svc_create(prog, 1, NULL, bufsize, 1, threadfn);
}
EXPORT_SYMBOL_GPL(svc_create);

/**
 * svc_create_pooled - Create an RPC service with pooled threads
 * @prog:  Array of RPC programs the new service will handle
 * @nprogs: Number of programs in the array
 * @stats: the stats struct if desired
 * @bufsize: maximum message size for @prog
 * @threadfn: a function to service RPC requests for @prog
 *
 * Returns an instantiated struct svc_serv object or NULL.
 */
struct svc_serv *svc_create_pooled(struct svc_program *prog,
				   unsigned int nprogs,
				   struct svc_stat *stats,
				   unsigned int bufsize,
				   int (*threadfn)(void *data))
{
	struct svc_serv *serv;
	unsigned int npools = svc_pool_map_get();
	unsigned int i;

	if (!npools)
		return NULL;

	serv = __svc_create(prog, nprogs, stats, bufsize, npools, threadfn);
	if (!serv)
		goto out_err;
	serv->sv_is_pooled = true;
	for (i = 0; i < npools; i++)
		serv->sv_pool_ext[i].node = svc_pool_map_get_node(i);
	svc_pool_ext_layout(serv->sv_pools, serv->sv_pool_ext, npools);
	return serv;
out_err:
	svc_pool_map_put();
	return NULL;
}
EXPORT_SYMBOL_GPL(svc_create_pooled);

/*
 * Destroy an RPC service. Should be called with appropriate locking to
 * protect sv_permsocks and sv_tempsocks.
 */
void
svc_destroy(struct svc_serv **servp)
{
	struct svc_serv *serv = *servp;
	unsigned int i;

	*servp = NULL;

	dprintk("svc: svc_destroy(%s)\n", serv->sv_programs->pg_name);
	timer_shutdown_sync(&serv->sv_temptimer);

	/*
	 * Remaining transports at this point are not expected.
	 */
	WARN_ONCE(!list_empty(&serv->sv_permsocks),
		  "SVC: permsocks remain for %s\n", serv->sv_programs->pg_name);
	WARN_ONCE(!list_empty(&serv->sv_tempsocks),
		  "SVC: tempsocks remain for %s\n", serv->sv_programs->pg_name);

	cache_clean_deferred(serv);
	svc_rtc_serv_destroy(serv);

	for (i = 0; i < svc_serv_nrpools(serv); i++) {
		struct svc_pool *pool = &serv->sv_pools[i];

		svc_pool_destroy_counters(pool);
		svc_pool_ext_destroy(&serv->sv_pool_ext[i]);
	}

	if (serv->sv_is_pooled)
		svc_pool_map_put();

	kfree(serv->sv_pool_ext[0].lines);
	kfree(serv->sv_pool_ext);
	kfree(serv->sv_pools);
	kfree(serv);
}
EXPORT_SYMBOL_GPL(svc_destroy);

static bool
svc_init_buffer(struct svc_rqst *rqstp, const struct svc_serv *serv, int node)
{
	rqstp->rq_maxpages = svc_serv_maxpages(serv);

	/* +1 for a NULL sentinel readable by nfsd_splice_actor() */
	rqstp->rq_pages = kcalloc_node(rqstp->rq_maxpages + 1,
				       sizeof(struct page *),
				       GFP_KERNEL, node);
	if (!rqstp->rq_pages)
		return false;

	/* +1 for a NULL sentinel at rq_page_end (see svc_rqst_replace_page) */
	rqstp->rq_respages = kcalloc_node(rqstp->rq_maxpages + 1,
					  sizeof(struct page *),
					  GFP_KERNEL, node);
	if (!rqstp->rq_respages) {
		kfree(rqstp->rq_pages);
		rqstp->rq_pages = NULL;
		return false;
	}

	rqstp->rq_pages_nfree = rqstp->rq_maxpages;
	rqstp->rq_next_page = rqstp->rq_respages + rqstp->rq_maxpages;
	return true;
}

/*
 * Release an RPC server buffer
 */
static void
svc_release_buffer(struct svc_rqst *rqstp)
{
	unsigned long i;

	if (rqstp->rq_pages) {
		for (i = 0; i < rqstp->rq_maxpages; i++)
			if (rqstp->rq_pages[i])
				put_page(rqstp->rq_pages[i]);
		kfree(rqstp->rq_pages);
	}

	if (rqstp->rq_respages) {
		for (i = 0; i < rqstp->rq_maxpages; i++)
			if (rqstp->rq_respages[i])
				put_page(rqstp->rq_respages[i]);
		kfree(rqstp->rq_respages);
	}
}

static void svc_rqst_free_rcu(struct rcu_head *head)
{
	struct svc_rqst *rqstp = container_of(head, struct svc_rqst, rq_rcu_head);

	kfree(rqstp->rq_resp);
	kfree(rqstp->rq_argp);
	kfree(rqstp);
}

/**
 * svc_rqst_free - Free an svc_rqst from svc_rqst_alloc()
 * @rqstp: request to free
 */
void
svc_rqst_free(struct svc_rqst *rqstp)
{
	folio_batch_release(&rqstp->rq_fbatch);
	svc_tcp_rx_state_free(rqstp->rq_tcp_rx);
	kfree(rqstp->rq_bvec);
	kfree(rqstp->rq_rx_region_pages);
	kfree(rqstp->rq_rx_region_saved);
	bitmap_free(rqstp->rq_rx_region_map);
	svc_release_buffer(rqstp);
	if (rqstp->rq_scratch_folio)
		folio_put(rqstp->rq_scratch_folio);
	kfree(rqstp->rq_auth_data);
	call_rcu(&rqstp->rq_rcu_head, svc_rqst_free_rcu);
}
EXPORT_SYMBOL_IF_KUNIT(svc_rqst_free);

/**
 * svc_rqst_alloc - Allocate an svc_rqst and its buffers
 * @serv: RPC service
 * @pool: pool the request counts against
 * @node: NUMA node for the allocations
 *
 * The request is not a thread: no thread accounting is done.  Free it
 * with svc_rqst_free().
 */
struct svc_rqst *
svc_rqst_alloc(struct svc_serv *serv, struct svc_pool *pool, int node)
{
	struct svc_rqst	*rqstp;

	rqstp = kzalloc_node(sizeof(*rqstp), GFP_KERNEL, node);
	if (!rqstp)
		return rqstp;

	folio_batch_init(&rqstp->rq_fbatch);

	rqstp->rq_server = serv;
	rqstp->rq_pool = pool;

	rqstp->rq_scratch_folio = __folio_alloc_node(GFP_KERNEL, 0,
						     node == NUMA_NO_NODE ?
						     numa_mem_id() : node);
	if (!rqstp->rq_scratch_folio)
		goto out_enomem;

	rqstp->rq_argp = kmalloc_node(serv->sv_xdrsize, GFP_KERNEL, node);
	if (!rqstp->rq_argp)
		goto out_enomem;

	rqstp->rq_resp = kmalloc_node(serv->sv_xdrsize, GFP_KERNEL, node);
	if (!rqstp->rq_resp)
		goto out_enomem;

	if (!svc_init_buffer(rqstp, serv, node))
		goto out_enomem;

	rqstp->rq_maxbvecs = svc_serv_maxbvecs(serv);
	rqstp->rq_bvec = kcalloc_node(rqstp->rq_maxbvecs,
				      sizeof(struct bio_vec),
				      GFP_KERNEL, node);
	if (!rqstp->rq_bvec)
		goto out_enomem;

	rqstp->rq_tcp_rx = svc_tcp_rx_state_alloc_entries(rqstp->rq_maxbvecs,
							  rqstp->rq_maxpages,
							  GFP_KERNEL, node);
	if (!rqstp->rq_tcp_rx)
		goto out_enomem;

	rqstp->rq_rx_region_pages = kcalloc_node(rqstp->rq_maxpages,
						 sizeof(struct page *),
						 GFP_KERNEL, node);
	rqstp->rq_rx_region_saved = kcalloc_node(rqstp->rq_maxpages,
						 sizeof(struct page *),
						 GFP_KERNEL, node);
	rqstp->rq_rx_region_map = bitmap_zalloc_node(rqstp->rq_maxpages,
						     GFP_KERNEL, node);
	if (!rqstp->rq_rx_region_pages || !rqstp->rq_rx_region_saved ||
	    !rqstp->rq_rx_region_map)
		goto out_enomem;

	rqstp->rq_err = -EAGAIN; /* No error yet */
	return rqstp;

out_enomem:
	svc_rqst_free(rqstp);
	return NULL;
}
EXPORT_SYMBOL_IF_KUNIT(svc_rqst_alloc);

static struct svc_rqst *
svc_prepare_thread(struct svc_serv *serv, struct svc_pool *pool, int node)
{
	struct svc_rqst	*rqstp;

	rqstp = svc_rqst_alloc(serv, pool, node);
	if (!rqstp)
		return NULL;

	serv->sv_nrthreads += 1;
	pool->sp_nrthreads += 1;

	/* Protected by whatever lock the service uses when calling
	 * svc_set_num_threads()
	 */
	list_add_rcu(&rqstp->rq_all, &pool->sp_all_threads);

	return rqstp;
}

/**
 * svc_pool_wake_idle_thread - Awaken an idle thread in @pool
 * @pool: service thread pool
 *
 * Can be called from soft IRQ or process context. Finding an idle
 * service thread and marking it BUSY is atomic with respect to
 * other calls to svc_pool_wake_idle_thread().
 *
 * The first idle thread is claimed with RQ_WAKING, once per idle period:
 * the thread clears the bit each time it goes idle.  A claimed thread
 * looks for work in its own pool and its steal window before it sleeps
 * again, so each claim stands for one piece of work.
 *
 * Returns true when this call claimed a thread.  Returns false when
 * @pool has no idle thread, or its first idle thread is already claimed
 * for earlier work; the caller then looks to a sibling pool.
 */
bool svc_pool_wake_idle_thread(struct svc_pool *pool)
{
	struct svc_rqst	*rqstp;
	struct llist_node *ln;

	rcu_read_lock();
	ln = READ_ONCE(pool->sp_idle_threads.first);
	if (ln) {
		rqstp = llist_entry(ln, struct svc_rqst, rq_idle);
		/*
		 * Only trace_svc_xprt_dequeue() reads this. The store is to
		 * another thread's svc_rqst, made even when that thread is
		 * already running, so it is skipped unless the event is on.
		 */
		if (trace_svc_xprt_dequeue_enabled())
			WRITE_ONCE(rqstp->rq_qtime, ktime_get());
		/* A woken thread stays first on the idle list until it
		 * runs.  A running thread on the list is spinning or about
		 * to sleep: it rechecks its queue and RQ_WAKING after
		 * setting its task state, and the test_and_set_bit() here
		 * orders the work queued by the caller before the
		 * task_is_running() read, so it needs no wakeup.
		 */
		if (!test_bit(RQ_WAKING, &rqstp->rq_flags) &&
		    !test_and_set_bit(RQ_WAKING, &rqstp->rq_flags)) {
			if (!task_is_running(rqstp->rq_task)) {
				wake_up_process(rqstp->rq_task);
				trace_svc_pool_thread_wake(pool,
							   rqstp->rq_task->pid);
				percpu_counter_inc(&pool->sp_threads_woken);
			} else {
				trace_svc_pool_thread_running(pool,
							rqstp->rq_task->pid);
			}
			rcu_read_unlock();
			return true;
		}
		trace_svc_pool_thread_running(pool, rqstp->rq_task->pid);
		rcu_read_unlock();
		return false;
	}
	rcu_read_unlock();
	trace_svc_pool_thread_noidle(pool, 0);
	return false;
}
EXPORT_SYMBOL_GPL(svc_pool_wake_idle_thread);

/**
 * svc_pool_hand_idle_thread - give a transport to an idle thread of @pool
 * @pool: service thread pool
 * @xprt: a transport whose XPT_BUSY the caller holds
 *
 * Claims the first idle thread as svc_pool_wake_idle_thread() does, and
 * in the same atomic step marks the claim as bringing a transport
 * (RQ_HANDOFF), so the thread takes @xprt from rq_handoff and not off
 * sp_xprts, whose lock it does not take.  The thread waits for the store
 * of @xprt that follows the claim (svc_thread_take_handoff()).
 *
 * Return: true if the thread has @xprt.  false leaves @xprt with the
 * caller, which queues it; the thread a false return claimed, if any,
 * then stands for that queued work.
 */
bool svc_pool_hand_idle_thread(struct svc_pool *pool, struct svc_xprt *xprt)
{
	struct svc_rqst	*rqstp;
	struct llist_node *ln;
	unsigned long flags;
	bool handed = false;

	rcu_read_lock();
	ln = READ_ONCE(pool->sp_idle_threads.first);
	if (!ln)
		goto out;
	rqstp = llist_entry(ln, struct svc_rqst, rq_idle);
	flags = READ_ONCE(rqstp->rq_flags);
	/* the thread waits for the store after the claim: no preemption
	 * between them
	 */
	preempt_disable();
	do {
		if (flags & (BIT(RQ_WAKING) | BIT(RQ_HANDOFF))) {
			preempt_enable();
			goto out;
		}
	} while (!try_cmpxchg(&rqstp->rq_flags, &flags,
			      flags | BIT(RQ_WAKING) | BIT(RQ_HANDOFF)));
	/* Only a claim's owner stores here, and the thread empties the
	 * slot before it can be claimed again; a full slot is a transport
	 * the thread has not taken yet, so this one is queued instead.
	 */
	handed = !cmpxchg_release(&rqstp->rq_handoff, NULL, xprt);
	preempt_enable();
	/* The cmpxchg() of the claim orders it before the task_is_running()
	 * read, as test_and_set_bit() does in svc_pool_wake_idle_thread().
	 */
	if (!task_is_running(rqstp->rq_task)) {
		wake_up_process(rqstp->rq_task);
		trace_svc_pool_thread_wake(pool, rqstp->rq_task->pid);
		percpu_counter_inc(&pool->sp_threads_woken);
	} else {
		trace_svc_pool_thread_running(pool, rqstp->rq_task->pid);
	}
out:
	rcu_read_unlock();
	return handed;
}

/**
 * svc_new_thread - spawn a new thread in the given pool
 * @serv: the serv to which the pool belongs
 * @pool: pool in which thread should be spawned
 *
 * Create a new thread inside @pool, which is a part of @serv.
 * Caller must hold the service mutex.
 *
 * Returns 0 on success, or -errno on failure.
 */
int svc_new_thread(struct svc_serv *serv, struct svc_pool *pool)
{
	struct svc_rqst	*rqstp;
	struct task_struct *task;
	int node;
	int err = 0;

	/*
	 * Only pooled services hold a reference to the pool map, so only they
	 * may consult it. Unpooled services (e.g. lockd, the NFS callback)
	 * leave placement to the allocator.
	 */
	if (serv->sv_is_pooled)
		node = svc_pool_map_get_node(pool->sp_id);
	else
		node = NUMA_NO_NODE;

	rqstp = svc_prepare_thread(serv, pool, node);
	if (!rqstp)
		return -ENOMEM;
	task = kthread_create_on_node(serv->sv_threadfn, rqstp,
				      node, "%s", serv->sv_name);
	if (IS_ERR(task)) {
		err = PTR_ERR(task);
		goto out;
	}

	rqstp->rq_task = task;
	if (svc_serv_nrpools(serv) > 1)
		svc_pool_map_set_cpumask(task, pool->sp_id);

	svc_sock_update_bufs(serv);
	wake_up_process(task);

	/* Wait for the thread to signal initialization status */
	wait_var_event(&rqstp->rq_err, rqstp->rq_err != -EAGAIN);
	err = rqstp->rq_err;
out:
	if (err)
		svc_exit_thread(rqstp);
	return err;
}
EXPORT_SYMBOL_GPL(svc_new_thread);

static int
svc_start_kthreads(struct svc_serv *serv, struct svc_pool *pool, int nrservs)
{
	int err = 0;

	while (!err && nrservs--)
		err = svc_new_thread(serv, pool);

	return err;
}

static int
svc_stop_kthreads(struct svc_serv *serv, struct svc_pool *pool, int nrservs)
{
	do {
		set_bit(SP_VICTIM_REMAINS, &pool->sp_flags);
		set_bit(SP_NEED_VICTIM, &pool->sp_flags);
		svc_pool_wake_idle_thread(pool);
		wait_on_bit(&pool->sp_flags, SP_VICTIM_REMAINS, TASK_IDLE);
		nrservs++;
	} while (nrservs < 0);
	return 0;
}

/**
 * svc_set_pool_threads - adjust number of threads per pool
 * @serv: RPC service to adjust
 * @pool: Specific pool from which to choose threads
 * @min_threads: min number of threads to run in @pool
 * @max_threads: max number of threads in @pool (0 means kill all threads)
 *
 * Create or destroy threads in @pool to bring it into an acceptable range
 * between @min_threads and @max_threads.
 *
 * If @min_threads is 0 or larger than @max_threads, then it is ignored and
 * the pool will be set to run a static @max_threads number of threads.
 *
 * Caller must ensure mutual exclusion between this and server startup or
 * shutdown.
 *
 * Returns zero on success or a negative errno if an error occurred while
 * starting a thread.
 */
int
svc_set_pool_threads(struct svc_serv *serv, struct svc_pool *pool,
		     unsigned int min_threads, unsigned int max_threads)
{
	int delta;

	if (!pool)
		return -EINVAL;

	/* clamp min threads to the max */
	if (min_threads > max_threads)
		min_threads = max_threads;

	pool->sp_nrthrmin = min_threads;
	pool->sp_nrthrmax = max_threads;
	/* pools left without threads are served by one that has them */
	svc_pool_ext_layout(serv->sv_pools, serv->sv_pool_ext,
			    svc_serv_nrpools(serv));
	/* the receive region's floor follows the threads */
	svc_tcp_rx_serv_threads(serv);

	/*
	 * When min_threads is set, then only change the number of
	 * threads to bring it within an acceptable range.
	 */
	if (min_threads) {
		if (pool->sp_nrthreads > max_threads)
			delta = max_threads;
		else if (pool->sp_nrthreads < min_threads)
			delta = min_threads;
		else
			return 0;
	} else {
		delta = max_threads;
	}

	delta -= pool->sp_nrthreads;
	if (delta > 0)
		return svc_start_kthreads(serv, pool, delta);
	if (delta < 0) {
		int err = svc_stop_kthreads(serv, pool, delta);

		/* svc_pool_siblings=N: what it still holds goes to its server */
		svc_pool_rehome(serv, pool);
		return err;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(svc_set_pool_threads);

/* One past the last pool of the node that @first is on */
static unsigned int svc_pool_node_end(const struct svc_pool_ext *ext,
				      unsigned int npools, unsigned int first)
{
	unsigned int end = first + 1;

	while (end < npools && ext[end].node == ext[first].node)
		end++;
	return end;
}

/* A node's threads before the remainder: its share by pool count */
static unsigned int svc_pool_node_base(unsigned int total, unsigned int count,
				       unsigned int npools)
{
	return div_u64((u64)total * count, npools);
}

/**
 * svc_pool_split_init - start splitting threads over a service's pools
 * @s: cursor to set up
 * @ext: the pools' svc_pool_ext, with node set
 * @npools: number of pools
 * @total: threads to split
 *
 * Each node (a run of consecutive pools with the same node) gets @total
 * threads in proportion to its pool count, with the remainder one each
 * on the lowest nodes; svc_pool_split_next() spreads that evenly over
 * the node's pools.
 */
VISIBLE_IF_KUNIT void svc_pool_split_init(struct svc_pool_split *s,
					  const struct svc_pool_ext *ext,
					  unsigned int npools,
					  unsigned int total)
{
	unsigned int first, end, sum = 0;

	for (first = 0; first < npools; first = end) {
		end = svc_pool_node_end(ext, npools, first);
		sum += svc_pool_node_base(total, end - first, npools);
	}
	*s = (struct svc_pool_split){
		.total = total, .npools = npools, .rem = total - sum,
	};
}
EXPORT_SYMBOL_IF_KUNIT(svc_pool_split_init);

/**
 * svc_pool_split_next - the threads of the next pool
 * @s: cursor from svc_pool_split_init()
 * @ext: the same svc_pool_ext
 * @i: the pool; call for pools 0, 1, ... in order
 *
 * Returns pool @i's share of its node's threads.
 */
VISIBLE_IF_KUNIT unsigned int
svc_pool_split_next(struct svc_pool_split *s, const struct svc_pool_ext *ext,
		    unsigned int i)
{
	if (i >= s->end) {
		s->first = s->end;
		s->end = svc_pool_node_end(ext, s->npools, s->first);
		s->threads = svc_pool_node_base(s->total, s->end - s->first,
						s->npools) +
			     (s->node++ < s->rem);
	}
	return svc_pool_share(s->threads, i - s->first, s->end - s->first);
}
EXPORT_SYMBOL_IF_KUNIT(svc_pool_split_next);

/**
 * svc_set_num_threads - adjust number of threads in serv
 * @serv: RPC service to adjust
 * @min_threads: min number of threads to run, split over the pools
 * @nrservs: New number of threads for @serv (0 means kill all threads)
 *
 * Create or destroy threads in @serv to bring it to exactly @nrservs.
 * svc_pool_split_init() gives each node a share of them by its pool
 * count, with the remainder on the lowest nodes, and svc_pool_share()
 * spreads a node's share evenly over its pools.  With fewer threads than
 * pools some pools get none; svc_pool_ext_layout() has each of those
 * served by the nearest pool of its node that has threads, so each such
 * pool serves a short block of its neighbours, or by the pools of other
 * nodes when its node got no thread at all.  Pool 0 may be one of them:
 * svc_wake_up() kicks the pool that serves it.
 *
 * @min_threads is split the same way over the pools that get threads,
 * with at least one each, so the minimum for @serv is @min_threads, or
 * the number of pools with threads when that is larger, and never more
 * than @nrservs.
 *
 * Caller must ensure mutual exclusion between this and server startup or
 * shutdown.
 *
 * Returns zero on success or a negative errno if an error occurred while
 * starting a thread. On failure, some pools may have already been
 * adjusted; the caller is responsible for recovery.
 */
int
svc_set_num_threads(struct svc_serv *serv, unsigned int min_threads,
		    unsigned int nrservs)
{
	unsigned int nrpools = svc_serv_nrpools(serv);
	unsigned int served = min(nrservs, nrpools);
	struct svc_pool_split split;
	unsigned int i, rank = 0;
	int err = 0;

	svc_pool_split_init(&split, serv->sv_pool_ext, nrpools, nrservs);
	for (i = 0; i < nrpools; ++i) {
		unsigned int threads = svc_pool_split_next(&split,
							   serv->sv_pool_ext,
							   i);
		unsigned int min = 0;

		if (threads && min_threads)
			min = max(1U, svc_pool_share(min_threads, rank++,
						     served));
		err = svc_set_pool_threads(serv, &serv->sv_pools[i], min,
					   threads);
		if (err)
			break;
	}
	svc_rtc_pool_resize(serv);
	return err;
}
EXPORT_SYMBOL_GPL(svc_set_num_threads);

/**
 * svc_serv_maxthreads - report a service's configured thread ceiling
 * @serv: RPC service to query
 *
 * A pooled service sizes its threads dynamically, so the number of
 * threads running at any moment tracks recent load rather than the
 * service's capacity. The per-pool maximum is the stable figure a
 * consumer should size against.
 *
 * The caller must keep @serv valid for the duration of the call.
 *
 * Return: the sum of every pool's maximum thread count.
 */
unsigned int svc_serv_maxthreads(const struct svc_serv *serv)
{
	unsigned int i, max = 0;

	for (i = 0; i < svc_serv_nrpools(serv); i++)
		max += data_race(serv->sv_pools[i].sp_nrthrmax);
	return max;
}
EXPORT_SYMBOL_GPL(svc_serv_maxthreads);

/**
 * svc_rqst_replace_page - Replace one page in rq_respages[]
 * @rqstp: svc_rqst with pages to replace
 * @page: replacement page
 *
 * When replacing a page in rq_respages, batch the release of the
 * replaced pages to avoid hammering the page allocator.
 *
 * Return values:
 *   %true: page replaced
 *   %false: array bounds checking failed
 */
bool svc_rqst_replace_page(struct svc_rqst *rqstp, struct page *page)
{
	struct page **begin = rqstp->rq_respages;
	struct page **end = rqstp->rq_page_end;

	if (unlikely(rqstp->rq_next_page < begin || rqstp->rq_next_page > end)) {
		trace_svc_replace_page_err(rqstp);
		return false;
	}

	if (*rqstp->rq_next_page)
		svc_rqst_page_release(rqstp, *rqstp->rq_next_page);

	get_page(page);
	*(rqstp->rq_next_page++) = page;
	return true;
}
EXPORT_SYMBOL_GPL(svc_rqst_replace_page);

/**
 * svc_rqst_release_pages - Release Reply buffer pages
 * @rqstp: RPC transaction context
 *
 * Release response pages in the range [rq_respages, rq_next_page).
 * NULL entries in this range are skipped, allowing transports to
 * transfer pages to a send context before this function runs.
 */
void svc_rqst_release_pages(struct svc_rqst *rqstp)
{
	struct page **pp;

	for (pp = rqstp->rq_respages; pp < rqstp->rq_next_page; pp++) {
		if (*pp) {
			if (!folio_batch_add(&rqstp->rq_fbatch,
					     page_folio(*pp)))
				__folio_batch_release(&rqstp->rq_fbatch);
			*pp = NULL;
		}
	}
	if (rqstp->rq_fbatch.nr)
		__folio_batch_release(&rqstp->rq_fbatch);
}

/**
 * svc_rqst_release_payload - Return a request's received payload early
 * @rqstp: RPC transaction context
 *
 * The caller has consumed the argument payload (for example, copied a
 * WRITE into the page cache) and will not read it again. A transport that
 * loans its receive buffers returns them now instead of when the RPC
 * completes; other transports do nothing.
 */
void svc_rqst_release_payload(struct svc_rqst *rqstp)
{
	struct svc_xprt *xprt = rqstp->rq_xprt;

	if (xprt && xprt->xpt_ops->xpo_release_payload)
		xprt->xpt_ops->xpo_release_payload(rqstp);
}
EXPORT_SYMBOL_GPL(svc_rqst_release_payload);

/**
 * svc_rqst_bounce_payload - Lay a loaned payload out page-tiled
 * @rqstp: RPC transaction context
 * @payload: the argument payload, a view of the request's arguments
 * @align: in-page offset for the first byte (the file position's), so a
 *	   direct write's aligned middle starts memory-aligned
 * @mem_align: the memory alignment, against the file position, that the
 *	       copy's start and every page joint must meet
 * @bvec: receives the page-tiled payload
 * @bvec_size: entries in @bvec
 *
 * For a direct WRITE whose loaned fragments the device cannot take: the
 * transport copies the payload once into whole pages (its fixed receive
 * region when the record holds region credit), returns the loan, and
 * describes the copy in @bvec.  Never waits.  Returns the number of
 * entries, or a negative errno when the payload is not loaned or cannot be
 * bounced; the payload is then untouched.  A positive return must be
 * paired with svc_rqst_bounce_done() once the device has the data.
 */
int svc_rqst_bounce_payload(struct svc_rqst *rqstp,
			    const struct xdr_buf *payload, unsigned int align,
			    unsigned int mem_align, struct bio_vec *bvec,
			    unsigned int bvec_size)
{
	struct svc_xprt *xprt = rqstp->rq_xprt;

	if (!xprt || !xprt->xpt_ops->xpo_bounce_payload)
		return -EOPNOTSUPP;
	return xprt->xpt_ops->xpo_bounce_payload(rqstp, payload, align,
						 mem_align, bvec, bvec_size);
}
EXPORT_SYMBOL_GPL(svc_rqst_bounce_payload);

/**
 * svc_rqst_bounce_done - Return a bounce once the device has the data
 * @rqstp: RPC transaction context
 * @bvec: the entries svc_rqst_bounce_payload() filled; clobbered
 * @nvecs: its return value
 */
void svc_rqst_bounce_done(struct svc_rqst *rqstp, struct bio_vec *bvec,
			  unsigned int nvecs)
{
	struct svc_xprt *xprt = rqstp->rq_xprt;

	if (xprt && xprt->xpt_ops->xpo_bounce_done)
		xprt->xpt_ops->xpo_bounce_done(rqstp, bvec, nvecs);
}
EXPORT_SYMBOL_GPL(svc_rqst_bounce_done);

/**
 * svc_rqst_rx_region_read - Confine a READ payload to region pages
 * @rqstp: RPC transaction context, its call decoded
 * @pages: payload pages the reply will carry
 *
 * Called before the duplicate-reply cache and before any operation runs.
 * The transport either installs region pages as the reply's payload pages
 * (svc_rqst_rx_region_held()), leaves the request unconfined, or parks it
 * until pages are free.
 *
 * Return values:
 *   %0: process the request
 *   %-EAGAIN: parked; do not process or reply, the transport replays it
 */
int svc_rqst_rx_region_read(struct svc_rqst *rqstp, unsigned int pages)
{
	struct svc_xprt *xprt = rqstp->rq_xprt;

	if (!pages || !xprt || !xprt->xpt_ops->xpo_rx_region_read)
		return 0;
	return xprt->xpt_ops->xpo_rx_region_read(rqstp, pages);
}
EXPORT_SYMBOL_GPL(svc_rqst_rx_region_read);

/**
 * svc_rqst_rx_region_unconfine - Serve a confined READ from ordinary pages
 * @rqstp: RPC transaction context
 * @served: the READ is served from ordinary pages (counted as unconfined);
 *	false for a reply that is never sent
 *
 * Returns the region pages and credit before they are used (for example
 * a file without direct I/O) and puts the request's own pages back.
 */
void svc_rqst_rx_region_unconfine(struct svc_rqst *rqstp, bool served)
{
	struct svc_xprt *xprt = rqstp->rq_xprt;

	if (svc_rqst_rx_region_held(rqstp) &&
	    xprt->xpt_ops->xpo_rx_region_unconfine)
		xprt->xpt_ops->xpo_rx_region_unconfine(rqstp, served);
}
EXPORT_SYMBOL_GPL(svc_rqst_rx_region_unconfine);

/**
 * svc_rqst_rx_region_fill - Read a READ's payload into region pages
 * @rqstp: RPC transaction context holding region pages
 * @n: pages the READ consumes from rq_next_page on
 *
 * Swaps the next @n reply page slots for region pages, saving the pages
 * they held.  Each READ fills its own slots when it runs, so slots other
 * operations already encoded into are never touched.
 *
 * Return: false when too few region pages are left; the READ then uses
 * the request's own pages.
 */
bool svc_rqst_rx_region_fill(struct svc_rqst *rqstp, unsigned int n)
{
	unsigned int slot = rqstp->rq_next_page - rqstp->rq_respages;
	unsigned int i;

	if (!svc_rqst_rx_region_held(rqstp) ||
	    rqstp->rq_rx_region_used + n > rqstp->rq_rx_region_count ||
	    n > rqstp->rq_page_end - rqstp->rq_next_page)
		return false;
	for (i = 0; i < n; i++, slot++) {
		rqstp->rq_rx_region_saved[slot] = rqstp->rq_respages[slot];
		rqstp->rq_respages[slot] =
			rqstp->rq_rx_region_pages[rqstp->rq_rx_region_used++];
		__set_bit(slot, rqstp->rq_rx_region_map);
	}
	return true;
}
EXPORT_SYMBOL_GPL(svc_rqst_rx_region_fill);

/**
 * svc_rqst_rx_region_uninstall - Empty the reply slots holding region pages
 * @rqstp: RPC transaction context
 * @restore: put the saved pages back now (else after the reply is released)
 */
void svc_rqst_rx_region_uninstall(struct svc_rqst *rqstp, bool restore)
{
	unsigned int slot;

	for_each_set_bit(slot, rqstp->rq_rx_region_map, rqstp->rq_maxpages)
		rqstp->rq_respages[slot] = NULL;
	if (restore)
		svc_rqst_rx_region_restore(rqstp);
}
EXPORT_SYMBOL_GPL(svc_rqst_rx_region_uninstall);

/**
 * svc_rqst_rx_region_restore - Put displaced reply pages back after a send
 * @rqstp: RPC transaction context
 *
 * The send handed the region pages to TCP and left their slots empty;
 * refilling them with the pages they displaced spares the allocator.
 */
void svc_rqst_rx_region_restore(struct svc_rqst *rqstp)
{
	unsigned int slot;

	for_each_set_bit(slot, rqstp->rq_rx_region_map, rqstp->rq_maxpages) {
		if (rqstp->rq_respages[slot])
			put_page(rqstp->rq_rx_region_saved[slot]);
		else
			rqstp->rq_respages[slot] =
				rqstp->rq_rx_region_saved[slot];
	}
	bitmap_zero(rqstp->rq_rx_region_map, rqstp->rq_maxpages);
}
EXPORT_SYMBOL_GPL(svc_rqst_rx_region_restore);

/**
 * svc_exit_thread - finalise the termination of a sunrpc server thread
 * @rqstp: the svc_rqst which represents the thread.
 *
 * When a thread started with svc_new_thread() exits it must call
 * svc_exit_thread() as its last act.  This must be done with the
 * service mutex held.  Normally this is held by a DIFFERENT thread, the
 * one that is calling svc_set_num_threads() and which will wait for
 * SP_VICTIM_REMAINS to be cleared before dropping the mutex.  If the
 * thread exits for any reason other than svc_thread_should_stop()
 * returning %true (which indicated that svc_set_num_threads() is
 * waiting for it to exit), then it must take the service mutex itself,
 * which can only safely be done using mutex_try_lock().
 */
void
svc_exit_thread(struct svc_rqst *rqstp)
{
	struct svc_serv	*serv = rqstp->rq_server;
	struct svc_pool	*pool = rqstp->rq_pool;

	svc_thread_return_handoff(rqstp);
	list_del_rcu(&rqstp->rq_all);

	pool->sp_nrthreads -= 1;
	serv->sv_nrthreads -= 1;
	svc_sock_update_bufs(serv);

	svc_rqst_free(rqstp);

	clear_and_wake_up_bit(SP_VICTIM_REMAINS, &pool->sp_flags);
}
EXPORT_SYMBOL_GPL(svc_exit_thread);

/*
 * Register an "inet" protocol family netid with the local
 * rpcbind daemon via an rpcbind v4 SET request.
 *
 * No netconfig infrastructure is available in the kernel, so
 * we map IP_ protocol numbers to netids by hand.
 *
 * Returns zero on success; a negative errno value is returned
 * if any error occurs.
 */
static int __svc_rpcb_register4(struct net *net, const u32 program,
				const u32 version,
				const unsigned short protocol,
				const unsigned short port)
{
	const struct sockaddr_in sin = {
		.sin_family		= AF_INET,
		.sin_addr.s_addr	= htonl(INADDR_ANY),
		.sin_port		= htons(port),
	};
	const char *netid;
	int error;

	switch (protocol) {
	case IPPROTO_UDP:
		netid = RPCBIND_NETID_UDP;
		break;
	case IPPROTO_TCP:
		netid = RPCBIND_NETID_TCP;
		break;
	default:
		return -ENOPROTOOPT;
	}

	error = rpcb_v4_register(net, program, version,
					(const struct sockaddr *)&sin, netid);

	/*
	 * User space didn't support rpcbind v4, so retry this
	 * registration request with the legacy rpcbind v2 protocol.
	 */
	if (error == -EPROTONOSUPPORT)
		error = rpcb_register(net, program, version, protocol, port);

	return error;
}

#if IS_ENABLED(CONFIG_IPV6)
/*
 * Register an "inet6" protocol family netid with the local
 * rpcbind daemon via an rpcbind v4 SET request.
 *
 * No netconfig infrastructure is available in the kernel, so
 * we map IP_ protocol numbers to netids by hand.
 *
 * Returns zero on success; a negative errno value is returned
 * if any error occurs.
 */
static int __svc_rpcb_register6(struct net *net, const u32 program,
				const u32 version,
				const unsigned short protocol,
				const unsigned short port)
{
	const struct sockaddr_in6 sin6 = {
		.sin6_family		= AF_INET6,
		.sin6_addr		= IN6ADDR_ANY_INIT,
		.sin6_port		= htons(port),
	};
	const char *netid;
	int error;

	switch (protocol) {
	case IPPROTO_UDP:
		netid = RPCBIND_NETID_UDP6;
		break;
	case IPPROTO_TCP:
		netid = RPCBIND_NETID_TCP6;
		break;
	default:
		return -ENOPROTOOPT;
	}

	error = rpcb_v4_register(net, program, version,
					(const struct sockaddr *)&sin6, netid);

	/*
	 * User space didn't support rpcbind version 4, so we won't
	 * use a PF_INET6 listener.
	 */
	if (error == -EPROTONOSUPPORT)
		error = -EAFNOSUPPORT;

	return error;
}
#endif	/* IS_ENABLED(CONFIG_IPV6) */

/*
 * Register a kernel RPC service via rpcbind version 4.
 *
 * Returns zero on success; a negative errno value is returned
 * if any error occurs.
 */
static int __svc_register(struct net *net, const char *progname,
			  const u32 program, const u32 version,
			  const int family,
			  const unsigned short protocol,
			  const unsigned short port)
{
	int error = -EAFNOSUPPORT;

	switch (family) {
	case PF_INET:
		error = __svc_rpcb_register4(net, program, version,
						protocol, port);
		break;
#if IS_ENABLED(CONFIG_IPV6)
	case PF_INET6:
		error = __svc_rpcb_register6(net, program, version,
						protocol, port);
#endif
	}

	trace_svc_register(progname, version, family, protocol, port, error);
	return error;
}

static
int svc_rpcbind_set_version(struct net *net,
			    const struct svc_program *progp,
			    u32 version, int family,
			    unsigned short proto,
			    unsigned short port)
{
	return __svc_register(net, progp->pg_name, progp->pg_prog,
				version, family, proto, port);

}

int svc_generic_rpcbind_set(struct net *net,
			    const struct svc_program *progp,
			    u32 version, int family,
			    unsigned short proto,
			    unsigned short port)
{
	const struct svc_version *vers = progp->pg_vers[version];
	int error;

	if (vers == NULL)
		return 0;

	if (vers->vs_hidden) {
		trace_svc_noregister(progp->pg_name, version, proto,
				     port, family, 0);
		return 0;
	}

	/*
	 * Don't register a UDP port if we need congestion
	 * control.
	 */
	if (vers->vs_need_cong_ctrl && proto == IPPROTO_UDP)
		return 0;

	error = svc_rpcbind_set_version(net, progp, version,
					family, proto, port);

	/* -EIO means no answer, not a refusal, so vs_rpcb_optnl must keep it. */
	if (error == -EIO)
		return error;

	return (vers->vs_rpcb_optnl) ? 0 : error;
}
EXPORT_SYMBOL_GPL(svc_generic_rpcbind_set);

/**
 * svc_rpcb_failure_count - local rpcbind calls for @serv that got no answer
 * @serv: RPC service to query
 *
 * svc_register() adds one for each of its calls that got no answer. A reply
 * that refuses one entry does not count, because rpcbind answered and the
 * next entry may still succeed.
 *
 * The count is kept per serv rather than per net. The local rpcbind client
 * is per-net and lockd shares it, but a count that another service can move
 * says nothing about this serv's own calls.
 *
 * This is for callers that cannot see the svc_register() return, because a
 * transport class sits in between. Such a caller reads the count before it
 * starts and compares as it goes, so there is no state to reset between
 * operations. The count never resets, and callers must not attach meaning
 * to the value itself.
 *
 * Return: the number of unanswered calls since this serv was created.
 */
unsigned int svc_rpcb_failure_count(struct svc_serv *serv)
{
	return atomic_read(&serv->sv_rpcb_failures);
}
EXPORT_SYMBOL_GPL(svc_rpcb_failure_count);

/**
 * svc_register - register an RPC service with the local portmapper
 * @serv: svc_serv struct for the service to register
 * @net: net namespace for the service to register
 * @family: protocol family of service's listener socket
 * @proto: transport protocol number to advertise
 * @port: port to advertise
 *
 * Service is registered for any address in the passed-in protocol family
 */
int svc_register(struct svc_serv *serv, struct net *net,
		 const int family, const unsigned short proto,
		 const unsigned short port)
{
	bool			noanswer = false;
	unsigned int		p, i;
	int			error = 0;

	WARN_ON_ONCE(proto == 0 && port == 0);
	if (proto == 0 && port == 0)
		return -EINVAL;

	if (serv->sv_no_rpcbind)
		return 0;

	for (p = 0; p < serv->sv_nprogs; p++) {
		struct svc_program *progp = &serv->sv_programs[p];

		for (i = 0; i < progp->pg_nvers; i++) {
			const struct svc_version *vers = progp->pg_vers[i];
			int ret;

			ret = progp->pg_rpcbind_set(net, progp, i,
					family, proto, port);
			if (ret == -EIO) {
				noanswer = true;
				if (vers && vers->vs_rpcb_optnl)
					ret = 0;
			}
			if (ret < 0) {
				printk(KERN_WARNING "svc: failed to register "
					"%sv%u RPC service (errno %d).\n",
					progp->pg_name, i, -ret);
				if (!error)
					error = ret;
				break;
			}
			if (noanswer)
				break;
		}

		/* Give up on trying to register anything if it didn't respond */
		if (noanswer)
			break;
	}

	if (noanswer)
		atomic_inc(&serv->sv_rpcb_failures);

	return error;
}

/*
 * If user space is running rpcbind, it should take the v4 UNSET
 * and clear everything for this [program, version].  If user space
 * is running portmap, it will reject the v4 UNSET, but won't have
 * any "inet6" entries anyway.  So a PMAP_UNSET should be sufficient
 * in this case to clear all existing entries for [program, version].
 */
static int __svc_unregister(struct net *net, const u32 program, const u32 version,
			    const char *progname)
{
	int error;

	error = rpcb_v4_register(net, program, version, NULL, "");

	/*
	 * User space didn't support rpcbind v4, so retry this
	 * request with the legacy rpcbind v2 protocol.
	 */
	if (error == -EPROTONOSUPPORT)
		error = rpcb_register(net, program, version, 0, 0);

	trace_svc_unregister(progname, version, error);
	return error;
}

/*
 * All netids, bind addresses and ports registered for [program, version]
 * are removed from the local rpcbind database (if the service is not
 * hidden) to make way for a new instance of the service.
 *
 * The result of unregistration is reported via dprintk for those who want
 * verification of the result, but is otherwise not important.
 */
static void svc_unregister(const struct svc_serv *serv, struct net *net)
{
	struct sighand_struct *sighand;
	unsigned long flags;
	unsigned int p, i;

	clear_thread_flag(TIF_SIGPENDING);

	for (p = 0; p < serv->sv_nprogs; p++) {
		struct svc_program *progp = &serv->sv_programs[p];

		for (i = 0; i < progp->pg_nvers; i++) {
			if (progp->pg_vers[i] == NULL)
				continue;
			if (progp->pg_vers[i]->vs_hidden)
				continue;
			if (__svc_unregister(net, progp->pg_prog, i,
					     progp->pg_name) == -EIO)
				goto out;
		}
	}

out:
	rcu_read_lock();
	sighand = rcu_dereference(current->sighand);
	spin_lock_irqsave(&sighand->siglock, flags);
	recalc_sigpending();
	spin_unlock_irqrestore(&sighand->siglock, flags);
	rcu_read_unlock();
}

/*
 * dprintk the given error with the address of the client that caused it.
 */
#if IS_ENABLED(CONFIG_SUNRPC_DEBUG)
static __printf(2, 3)
void svc_printk(struct svc_rqst *rqstp, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;
	char 	buf[RPC_MAX_ADDRBUFLEN];

	va_start(args, fmt);

	vaf.fmt = fmt;
	vaf.va = &args;

	dprintk("svc: %s: %pV", svc_print_addr(rqstp, buf, sizeof(buf)), &vaf);

	va_end(args);
}
#else
static __printf(2,3) void svc_printk(struct svc_rqst *rqstp, const char *fmt, ...) {}
#endif

__be32 svc_proc_lookup(const struct svc_program *progp, u32 version,
		       u32 procedure, const struct svc_version **verspp,
		       const struct svc_procedure **procpp)
{
	const struct svc_version *versp;

	*verspp = NULL;
	*procpp = NULL;
	if (version >= progp->pg_nvers)
		return rpc_prog_mismatch;
	versp = progp->pg_vers[version];
	if (!versp)
		return rpc_prog_mismatch;
	*verspp = versp;
	if (procedure >= versp->vs_nproc)
		return rpc_proc_unavail;
	*procpp = &versp->vs_proc[procedure];
	return rpc_success;
}
EXPORT_SYMBOL_IF_KUNIT(svc_proc_lookup);

__be32
svc_generic_init_request(struct svc_rqst *rqstp,
		const struct svc_program *progp,
		struct svc_process_info *ret)
{
	const struct svc_procedure *procp;
	const struct svc_version *versp;
	__be32 status;

	status = svc_proc_lookup(progp, rqstp->rq_vers, rqstp->rq_proc,
				 &versp, &procp);
	if (status == rpc_prog_mismatch)
		goto err_bad_vers;

	/*
	 * Some protocol versions (namely NFSv4) require some form of
	 * congestion control.  (See RFC 7530 section 3.1 paragraph 2)
	 * In other words, UDP is not allowed. We mark those when setting
	 * up the svc_xprt, and verify that here.
	 *
	 * The spec is not very clear about what error should be returned
	 * when someone tries to access a server that is listening on UDP
	 * for lower versions. RPC_PROG_MISMATCH seems to be the closest
	 * fit.
	 */
	if (versp->vs_need_cong_ctrl && rqstp->rq_xprt &&
	    !test_bit(XPT_CONG_CTRL, &rqstp->rq_xprt->xpt_flags))
		goto err_bad_vers;

	if (status == rpc_proc_unavail)
		goto err_bad_proc;
	rqstp->rq_procinfo = procp;

	/* Initialize storage for argp and resp */
	memset(rqstp->rq_argp, 0, procp->pc_argzero);
	memset(rqstp->rq_resp, 0, procp->pc_ressize);

	/* Bump per-net per-procedure stats counter */
	if (rqstp->rq_server->sv_stats &&
	    rqstp->rq_server->sv_stats->program == progp &&
	    rqstp->rq_server->sv_stats->vs_count &&
	    rqstp->rq_server->sv_stats->vs_count[rqstp->rq_vers]) {
		/* An rtc attempt may be handed to a thread: count at the end. */
		if (test_bit(RQ_RTC_TRY, &rqstp->rq_flags))
			set_bit(RQ_RTC_COUNT, &rqstp->rq_flags);
		else
			this_cpu_inc(rqstp->rq_server->sv_stats->vs_count
					[rqstp->rq_vers][rqstp->rq_proc]);
	}

	ret->dispatch = versp->vs_dispatch;
	return rpc_success;
err_bad_vers:
	ret->mismatch.lovers = progp->pg_lovers;
	ret->mismatch.hivers = progp->pg_hivers;
	return rpc_prog_mismatch;
err_bad_proc:
	return rpc_proc_unavail;
}
EXPORT_SYMBOL_GPL(svc_generic_init_request);

/**
 * svc_stat_alloc_counts - allocate per-netns per-version call count arrays
 * @statp: svc_stat whose vs_count arrays should be allocated
 *
 * statp->program must be set before calling this.
 *
 * Returns zero on success, or a negative errno otherwise.
 */
int svc_stat_alloc_counts(struct svc_stat *statp)
{
	struct svc_program *prog = statp->program;
	unsigned int i;

	statp->vs_count = kcalloc(prog->pg_nvers,
				  sizeof(unsigned long __percpu *),
				  GFP_KERNEL);
	if (!statp->vs_count)
		return -ENOMEM;

	for (i = 0; i < prog->pg_nvers; i++) {
		if (!prog->pg_vers[i])
			continue;
		statp->vs_count[i] = __alloc_percpu(prog->pg_vers[i]->vs_nproc *
					    sizeof(unsigned long),
					    sizeof(unsigned long));
		if (!statp->vs_count[i])
			goto err;
	}
	return 0;
err:
	svc_stat_free_counts(statp);
	return -ENOMEM;
}
EXPORT_SYMBOL_GPL(svc_stat_alloc_counts);

/**
 * svc_stat_free_counts - free per-netns per-version call count arrays
 * @statp: svc_stat whose vs_count arrays should be freed
 */
void svc_stat_free_counts(struct svc_stat *statp)
{
	struct svc_program *prog = statp->program;
	unsigned int i;

	if (!statp->vs_count)
		return;

	for (i = 0; i < prog->pg_nvers; i++)
		free_percpu(statp->vs_count[i]);
	kfree(statp->vs_count);
	statp->vs_count = NULL;
}
EXPORT_SYMBOL_GPL(svc_stat_free_counts);

/*
 * Common routine for processing the RPC request.
 */
/*
 * The part of svc_process_common() after the program's dispatch: shared
 * with svc_rtc_process_resume(), which runs it after an async completion.
 */
static int svc_process_dispatched(struct svc_rqst *rqstp, int rc,
				  unsigned int aoffset)
{
	const struct svc_procedure *procp = rqstp->rq_procinfo;
	struct xdr_stream *xdr = &rqstp->rq_res_stream;
	struct svc_serv *serv = rqstp->rq_server;

	xdr_finish_decode(xdr);

	/* rtc: a thread reruns the call; nothing may be sent for it. */
	if (test_bit(RQ_RTC_MISS, &rqstp->rq_flags))
		goto dropit;
	if (!rc)
		goto dropit;
	if (rqstp->rq_auth_stat != rpc_auth_ok)
		goto err_bad_auth;

	if (*rqstp->rq_accept_statp != rpc_success)
		xdr_truncate_encode(xdr, aoffset);

	if (procp->pc_encode == NULL)
		goto dropit;

sendit:
	if (svc_authorise(rqstp))
		goto close_xprt;
	return 1;		/* Caller can now send it */

dropit:
	svc_authorise(rqstp);	/* doesn't hurt to call this twice */
	return 0;

close_xprt:
	if (test_bit(RQ_RTC_MISS, &rqstp->rq_flags))
		return 0;
	if (rqstp->rq_xprt && test_bit(XPT_TEMP, &rqstp->rq_xprt->xpt_flags))
		svc_xprt_close(rqstp->rq_xprt);
	return 0;

err_bad_auth:
	if (serv->sv_stats)
		serv->sv_stats->rpcbadauth++;
	/* Restore write pointer to location of reply status: */
	xdr_truncate_encode(xdr, XDR_UNIT * 2);
	xdr_stream_encode_u32(xdr, RPC_MSG_DENIED);
	xdr_stream_encode_u32(xdr, RPC_AUTH_ERROR);
	xdr_stream_encode_be32(xdr, rqstp->rq_auth_stat);
	goto sendit;
}

static int
svc_process_common(struct svc_rqst *rqstp)
{
	struct xdr_stream	*xdr = &rqstp->rq_res_stream;
	struct svc_program	*progp = NULL;
	const struct svc_procedure *procp = NULL;
	struct svc_serv		*serv = rqstp->rq_server;
	struct svc_process_info process;
	enum svc_auth_status	auth_res;
	unsigned int		aoffset;
	int			pr, rc;
	__be32			*p;

	/* Reset the accept_stat for the RPC */
	rqstp->rq_accept_statp = NULL;

	/* Will be turned off only when NFSv4 Sessions are used */
	set_bit(RQ_USEDEFERRAL, &rqstp->rq_flags);
	clear_bit(RQ_DROPME, &rqstp->rq_flags);

	/* Construct the first words of the reply: */
	svcxdr_init_encode(rqstp);
	xdr_stream_encode_be32(xdr, rqstp->rq_xid);
	xdr_stream_encode_be32(xdr, rpc_reply);

	p = xdr_inline_decode(&rqstp->rq_arg_stream, XDR_UNIT * 4);
	if (unlikely(!p))
		goto err_short_len;
	if (*p++ != cpu_to_be32(RPC_VERSION))
		goto err_bad_rpc;

	xdr_stream_encode_be32(xdr, rpc_msg_accepted);

	rqstp->rq_prog = be32_to_cpup(p++);
	rqstp->rq_vers = be32_to_cpup(p++);
	rqstp->rq_proc = be32_to_cpup(p);

	for (pr = 0; pr < serv->sv_nprogs; pr++)
		if (rqstp->rq_prog == serv->sv_programs[pr].pg_prog)
			progp = &serv->sv_programs[pr];

	/*
	 * Decode auth data, and add verifier to reply buffer.
	 * We do this before anything else in order to get a decent
	 * auth verifier.
	 */
	auth_res = svc_authenticate(rqstp);
	/* Also give the program a chance to reject this call: */
	if (auth_res == SVC_OK && progp)
		auth_res = progp->pg_authenticate(rqstp);
	trace_svc_authenticate(rqstp, auth_res);
	switch (auth_res) {
	case SVC_OK:
		break;
	case SVC_GARBAGE:
		rqstp->rq_auth_stat = rpc_autherr_badcred;
		goto err_bad_auth;
	case SVC_DENIED:
		goto err_bad_auth;
	case SVC_CLOSE:
		goto close;
	case SVC_DROP:
		goto dropit;
	case SVC_COMPLETE:
		goto sendit;
	default:
		pr_warn_once("Unexpected svc_auth_status (%d)\n", auth_res);
		rqstp->rq_auth_stat = rpc_autherr_failed;
		goto err_bad_auth;
	}

	if (progp == NULL)
		goto err_bad_prog;

	switch (progp->pg_init_request(rqstp, progp, &process)) {
	case rpc_success:
		break;
	case rpc_prog_unavail:
		goto err_bad_prog;
	case rpc_prog_mismatch:
		goto err_bad_vers;
	case rpc_proc_unavail:
		goto err_bad_proc;
	}

	procp = rqstp->rq_procinfo;
	/* Should this check go into the dispatcher? */
	if (!procp || !procp->pc_func)
		goto err_bad_proc;

	/* Syntactic check complete */
	if (serv->sv_stats)
		/* data_race: best-effort aggregate statistics counter. */
		data_race(serv->sv_stats->rpccnt++);
	trace_svc_process(rqstp, progp->pg_name);

	aoffset = xdr_stream_pos(xdr);

	/* un-reserve some of the out-queue now that we have a
	 * better idea of reply size
	 */
	if (procp->pc_xdrressize)
		svc_reserve_auth(rqstp, procp->pc_xdrressize<<2);

	/* Call the function that processes the request. */
	rc = process.dispatch(rqstp);
	if (test_bit(RQ_RTC_ASYNC, &rqstp->rq_flags)) {
		/* svc_rtc_process_resume() finishes it after completion. */
		rqstp->rq_rtc_aoffset = aoffset;
		return 2;
	}
	return svc_process_dispatched(rqstp, rc, aoffset);

 sendit:
	if (svc_authorise(rqstp))
		goto close_xprt;
	return 1;		/* Caller can now send it */

 dropit:
	svc_authorise(rqstp);	/* doesn't hurt to call this twice */
	dprintk("svc: svc_process dropit\n");
	return 0;

 close:
	svc_authorise(rqstp);
close_xprt:
	/* rtc: a lookup miss goes to a thread; it is no reason to close. */
	if (test_bit(RQ_RTC_MISS, &rqstp->rq_flags))
		return 0;
	if (rqstp->rq_xprt && test_bit(XPT_TEMP, &rqstp->rq_xprt->xpt_flags))
		svc_xprt_close(rqstp->rq_xprt);
	dprintk("svc: svc_process close\n");
	return 0;

err_short_len:
	svc_printk(rqstp, "short len %u, dropping request\n",
		   rqstp->rq_arg.len);
	goto close_xprt;

err_bad_rpc:
	if (serv->sv_stats)
		serv->sv_stats->rpcbadfmt++;
	xdr_stream_encode_u32(xdr, RPC_MSG_DENIED);
	xdr_stream_encode_u32(xdr, RPC_MISMATCH);
	/* Only RPCv2 supported */
	xdr_stream_encode_u32(xdr, RPC_VERSION);
	xdr_stream_encode_u32(xdr, RPC_VERSION);
	return 1;	/* don't wrap */

err_bad_auth:
	dprintk("svc: authentication failed (%d)\n",
		be32_to_cpu(rqstp->rq_auth_stat));
	if (serv->sv_stats)
		serv->sv_stats->rpcbadauth++;
	/* Restore write pointer to location of reply status: */
	xdr_truncate_encode(xdr, XDR_UNIT * 2);
	xdr_stream_encode_u32(xdr, RPC_MSG_DENIED);
	xdr_stream_encode_u32(xdr, RPC_AUTH_ERROR);
	xdr_stream_encode_be32(xdr, rqstp->rq_auth_stat);
	goto sendit;

err_bad_prog:
	dprintk("svc: unknown program %d\n", rqstp->rq_prog);
	if (serv->sv_stats)
		serv->sv_stats->rpcbadfmt++;
	*rqstp->rq_accept_statp = rpc_prog_unavail;
	goto sendit;

err_bad_vers:
	svc_printk(rqstp, "unknown version (%d for prog %d, %s)\n",
		       rqstp->rq_vers, rqstp->rq_prog, progp->pg_name);

	if (serv->sv_stats)
		serv->sv_stats->rpcbadfmt++;
	*rqstp->rq_accept_statp = rpc_prog_mismatch;

	/*
	 * svc_authenticate() has already added the verifier and
	 * advanced the stream just past rq_accept_statp.
	 */
	xdr_stream_encode_u32(xdr, process.mismatch.lovers);
	xdr_stream_encode_u32(xdr, process.mismatch.hivers);
	goto sendit;

err_bad_proc:
	svc_printk(rqstp, "unknown procedure (%d)\n", rqstp->rq_proc);

	if (serv->sv_stats)
		serv->sv_stats->rpcbadfmt++;
	*rqstp->rq_accept_statp = rpc_proc_unavail;
	goto sendit;
}

/*
 * Drop request
 */
static void svc_drop(struct svc_rqst *rqstp)
{
	trace_svc_drop(rqstp);
}

static void svc_release_rqst(struct svc_rqst *rqstp)
{
	const struct svc_procedure *procp = rqstp->rq_procinfo;

	if (procp && procp->pc_release)
		procp->pc_release(rqstp);

	/*
	 * A subsequent svc_release_rqst() on this rqstp must not
	 * re-invoke pc_release against released state.
	 */
	rqstp->rq_procinfo = NULL;
}

/**
 * svc_process - Execute one RPC transaction
 * @rqstp: RPC transaction context
 *
 */
void svc_process(struct svc_rqst *rqstp)
{
	struct kvec		*resv = &rqstp->rq_res.head[0];
	__be32 *p;

#if IS_ENABLED(CONFIG_FAIL_SUNRPC)
	if (!fail_sunrpc.ignore_server_disconnect &&
	    should_fail(&fail_sunrpc.attr, 1))
		svc_xprt_deferred_close(rqstp->rq_xprt);
#endif

	/* Discard a stale release hook from a previous RPC. */
	rqstp->rq_procinfo = NULL;

	/*
	 * Setup response xdr_buf.
	 * Initially it has just one page
	 */
	rqstp->rq_next_page = &rqstp->rq_respages[1];
	resv->iov_base = page_address(rqstp->rq_respages[0]);
	resv->iov_len = 0;
	rqstp->rq_res.pages = rqstp->rq_next_page;
	rqstp->rq_res.len = 0;
	rqstp->rq_res.page_base = 0;
	rqstp->rq_res.page_len = 0;
	rqstp->rq_res.buflen = PAGE_SIZE;
	rqstp->rq_res.tail[0].iov_base = NULL;
	rqstp->rq_res.tail[0].iov_len = 0;

	svcxdr_init_decode(rqstp);
	p = xdr_inline_decode(&rqstp->rq_arg_stream, XDR_UNIT * 2);
	if (unlikely(!p))
		goto out_drop;
	rqstp->rq_xid = *p++;
	if (unlikely(*p != rpc_call))
		goto out_baddir;

	switch (svc_process_common(rqstp)) {
	case 0:
		svc_release_rqst(rqstp);
		goto out_drop;
	case 2:
		return;	/* rtc async: svc_rtc_process_resume() sends it */
	}
	svc_send(rqstp);
	svc_release_rqst(rqstp);
	return;

out_baddir:
	svc_printk(rqstp, "bad direction 0x%08x, dropping request\n",
		   be32_to_cpu(*p));
	if (rqstp->rq_server->sv_stats)
		rqstp->rq_server->sv_stats->rpcbadfmt++;
out_drop:
	svc_drop(rqstp);
}

/**
 * svc_rtc_process_resume - Finish an RQ_RTC_ASYNC call after completion
 * @rqstp: rqst whose svc_process() returned with RQ_RTC_ASYNC set
 * @rc: the program's dispatch result once its completion ran
 *
 * Runs the part of svc_process() the async call skipped: the reply goes
 * through svc_send() exactly as on the synchronous path.
 */
void svc_rtc_process_resume(struct svc_rqst *rqstp, int rc)
{
	clear_bit(RQ_RTC_ASYNC, &rqstp->rq_flags);
	if (svc_process_dispatched(rqstp, rc, rqstp->rq_rtc_aoffset))
		svc_send(rqstp);
	else
		svc_drop(rqstp);
	svc_release_rqst(rqstp);
}

/**
 * svc_rtc_process_abandon - Undo an RQ_RTC_ASYNC call for a thread rerun
 * @rqstp: rqst whose svc_process() returned with RQ_RTC_ASYNC set
 */
void svc_rtc_process_abandon(struct svc_rqst *rqstp)
{
	clear_bit(RQ_RTC_ASYNC, &rqstp->rq_flags);
	svc_authorise(rqstp);
	svc_release_rqst(rqstp);
	svc_drop(rqstp);
}

#if defined(CONFIG_SUNRPC_BACKCHANNEL)
/**
 * svc_process_bc - process a reverse-direction RPC request
 * @req: RPC request to be used for client-side processing
 * @rqstp: server-side execution context
 *
 */
void svc_process_bc(struct rpc_rqst *req, struct svc_rqst *rqstp)
{
	struct rpc_timeout timeout = {
		.to_increment		= 0,
	};
	struct rpc_task *task;
	int proc_error;

	/* Build the svc_rqst used by the common processing routine */
	rqstp->rq_procinfo = NULL;
	rqstp->rq_xid = req->rq_xid;
	rqstp->rq_prot = req->rq_xprt->prot;
	rqstp->rq_bc_net = req->rq_xprt->xprt_net;

	rqstp->rq_addrlen = sizeof(req->rq_xprt->addr);
	memcpy(&rqstp->rq_addr, &req->rq_xprt->addr, rqstp->rq_addrlen);
	memcpy(&rqstp->rq_arg, &req->rq_rcv_buf, sizeof(rqstp->rq_arg));
	memcpy(&rqstp->rq_res, &req->rq_snd_buf, sizeof(rqstp->rq_res));

	/* Adjust the argument buffer length */
	rqstp->rq_arg.len = req->rq_private_buf.len;
	if (rqstp->rq_arg.len <= rqstp->rq_arg.head[0].iov_len) {
		rqstp->rq_arg.head[0].iov_len = rqstp->rq_arg.len;
		rqstp->rq_arg.page_len = 0;
	} else if (rqstp->rq_arg.len <= rqstp->rq_arg.head[0].iov_len +
			rqstp->rq_arg.page_len)
		rqstp->rq_arg.page_len = rqstp->rq_arg.len -
			rqstp->rq_arg.head[0].iov_len;
	else
		rqstp->rq_arg.len = rqstp->rq_arg.head[0].iov_len +
			rqstp->rq_arg.page_len;

	/* Reset the response buffer */
	rqstp->rq_res.head[0].iov_len = 0;

	/*
	 * Skip the XID and calldir fields because they've already
	 * been processed by the caller.
	 */
	svcxdr_init_decode(rqstp);
	if (!xdr_inline_decode(&rqstp->rq_arg_stream, XDR_UNIT * 2))
		return;

	/* Parse and execute the bc call */
	proc_error = svc_process_common(rqstp);

	atomic_dec(&req->rq_xprt->bc_slot_count);
	if (!proc_error) {
		/* Processing error: drop the request */
		xprt_free_bc_request(req);
		svc_release_rqst(rqstp);
		return;
	}
	/* Finally, send the reply synchronously */
	if (rqstp->bc_to_initval > 0) {
		timeout.to_initval = rqstp->bc_to_initval;
		timeout.to_retries = rqstp->bc_to_retries;
	} else {
		timeout.to_initval = req->rq_xprt->timeout->to_initval;
		timeout.to_retries = req->rq_xprt->timeout->to_retries;
	}
	timeout.to_maxval = timeout.to_initval;
	memcpy(&req->rq_snd_buf, &rqstp->rq_res, sizeof(req->rq_snd_buf));
	task = rpc_run_bc_task(req, &timeout);
	svc_release_rqst(rqstp);

	if (IS_ERR(task))
		return;

	WARN_ON_ONCE(atomic_read(&task->tk_count) != 1);
	rpc_put_task(task);
}
#endif /* CONFIG_SUNRPC_BACKCHANNEL */

/**
 * svc_max_payload - Return transport-specific limit on the RPC payload
 * @rqstp: RPC transaction context
 *
 * Returns the maximum number of payload bytes the current transport
 * allows.
 */
u32 svc_max_payload(const struct svc_rqst *rqstp)
{
	u32 max = rqstp->rq_xprt->xpt_class->xcl_max_payload;

	if (rqstp->rq_server->sv_max_payload < max)
		max = rqstp->rq_server->sv_max_payload;
	return max;
}
EXPORT_SYMBOL_GPL(svc_max_payload);

/**
 * svc_proc_name - Return RPC procedure name in string form
 * @rqstp: svc_rqst to operate on
 *
 * Return value:
 *   Pointer to a NUL-terminated string
 */
const char *svc_proc_name(const struct svc_rqst *rqstp)
{
	if (rqstp && rqstp->rq_procinfo)
		return rqstp->rq_procinfo->pc_name;
	return "unknown";
}


/**
 * svc_encode_result_payload - mark a range of bytes as a result payload
 * @rqstp: svc_rqst to operate on
 * @offset: payload's byte offset in rqstp->rq_res
 * @length: size of payload, in bytes
 *
 * Returns zero on success, or a negative errno if a permanent
 * error occurred.
 */
int svc_encode_result_payload(struct svc_rqst *rqstp, unsigned int offset,
			      unsigned int length)
{
	return rqstp->rq_xprt->xpt_ops->xpo_result_payload(rqstp, offset,
							   length);
}
EXPORT_SYMBOL_GPL(svc_encode_result_payload);

/**
 * svc_fill_symlink_pathname - Construct pathname argument for VFS symlink call
 * @rqstp: svc_rqst to operate on
 * @first: buffer containing first section of pathname
 * @p: buffer containing remaining section of pathname
 * @total: total length of the pathname argument
 *
 * The VFS symlink API demands a NUL-terminated pathname in mapped memory.
 * Returns pointer to a NUL-terminated string, or an ERR_PTR. Caller must free
 * the returned string.
 */
char *svc_fill_symlink_pathname(struct svc_rqst *rqstp, struct kvec *first,
				void *p, size_t total)
{
	size_t len, remaining;
	char *result, *dst;

	result = kmalloc(total + 1, GFP_KERNEL);
	if (!result)
		return ERR_PTR(-ESERVERFAULT);

	dst = result;
	remaining = total;

	len = min_t(size_t, total, first->iov_len);
	if (len) {
		memcpy(dst, first->iov_base, len);
		dst += len;
		remaining -= len;
	}

	if (remaining) {
		len = min_t(size_t, remaining, PAGE_SIZE);
		memcpy(dst, p, len);
		dst += len;
	}

	*dst = '\0';

	/* Sanity check: Linux doesn't allow the pathname argument to
	 * contain a NUL byte.
	 */
	if (strlen(result) != total) {
		kfree(result);
		return ERR_PTR(-EINVAL);
	}
	return result;
}
EXPORT_SYMBOL_GPL(svc_fill_symlink_pathname);
