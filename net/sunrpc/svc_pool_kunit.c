// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for SUNRPC svc thread pools: the per-cpu pool map, the
 * pool layout that serves pools without threads, the sibling wake and
 * steal, the thread split, and the adaptive idle spin.
 */

#include <kunit/test.h>

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/cpumask.h>
#include <linux/kref.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sunrpc/svc.h>
#include <linux/sunrpc/svc_xprt.h>

#define SVC_POOL_TEST_CPUS	16

struct svc_pool_test_topo {
	unsigned int nodes;
	unsigned int ncpus;
	bool interleave;	/* node = cpu % nodes, else contiguous blocks */
};

static int svc_pool_test_node_of(unsigned int cpu, const void *arg)
{
	const struct svc_pool_test_topo *t = arg;

	if (t->interleave)
		return cpu % t->nodes;
	return cpu * t->nodes / t->ncpus;
}

struct svc_pool_test_map {
	unsigned int *to_pool, *pool_to, npools;
	struct cpumask *cpus, *possible;
};

/* Fill a map for @topo with the cpus from @online up offline */
static struct svc_pool_test_map *
svc_pool_test_fill(struct kunit *test, const struct svc_pool_test_topo *topo,
		   unsigned int online)
{
	struct svc_pool_test_map *m;
	unsigned int cpu;

	m = kunit_kzalloc(test, sizeof(*m), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, m);
	m->cpus = kunit_kzalloc(test, cpumask_size(), GFP_KERNEL);
	m->possible = kunit_kzalloc(test, cpumask_size(), GFP_KERNEL);
	m->to_pool = kunit_kcalloc(test, nr_cpu_ids, sizeof(*m->to_pool),
				   GFP_KERNEL);
	m->pool_to = kunit_kcalloc(test, nr_cpu_ids, sizeof(*m->pool_to),
				   GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, m->cpus);
	KUNIT_ASSERT_NOT_NULL(test, m->possible);
	KUNIT_ASSERT_NOT_NULL(test, m->to_pool);
	KUNIT_ASSERT_NOT_NULL(test, m->pool_to);
	for (cpu = 0; cpu < topo->ncpus; cpu++) {
		cpumask_set_cpu(cpu, m->possible);
		if (cpu < online)
			cpumask_set_cpu(cpu, m->cpus);
	}
	/* poison: every possible cpu must be given a pool */
	for (cpu = 0; cpu < nr_cpu_ids; cpu++)
		m->to_pool[cpu] = UINT_MAX;
	m->npools = svc_pool_map_fill_percpu(m->to_pool, m->pool_to, m->cpus,
					     m->possible,
					     svc_pool_test_node_of, topo,
					     topo->nodes);
	return m;
}

static void svc_pool_test_map(struct kunit *test,
			      const struct svc_pool_test_topo *topo)
{
	struct svc_pool_test_map *m = svc_pool_test_fill(test, topo,
							 topo->ncpus);
	unsigned int cpu, pidx;
	unsigned long *seen;

	seen = kunit_kcalloc(test, BITS_TO_LONGS(SVC_POOL_TEST_CPUS),
			     sizeof(*seen), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, seen);
	KUNIT_EXPECT_EQ(test, m->npools, topo->ncpus);

	/* every cpu has its own pool, and the maps are inverse */
	for_each_cpu(cpu, m->cpus) {
		pidx = m->to_pool[cpu];
		KUNIT_ASSERT_LT(test, pidx, m->npools);
		KUNIT_EXPECT_EQ(test, m->pool_to[pidx], cpu);
		KUNIT_EXPECT_FALSE(test, test_and_set_bit(pidx, seen));
	}

	/* pools of a node are consecutive, in node order */
	for (pidx = 1; pidx < m->npools; pidx++)
		KUNIT_EXPECT_LE(test,
				svc_pool_test_node_of(m->pool_to[pidx - 1], topo),
				svc_pool_test_node_of(m->pool_to[pidx], topo));
}

static unsigned int svc_pool_test_ncpus(void)
{
	return min_t(unsigned int, SVC_POOL_TEST_CPUS, nr_cpu_ids);
}

static void svc_pool_map_one_node_test(struct kunit *test)
{
	const struct svc_pool_test_topo topo = {
		.nodes = 1, .ncpus = svc_pool_test_ncpus(),
	};

	svc_pool_test_map(test, &topo);
}

static void svc_pool_map_two_node_test(struct kunit *test)
{
	const struct svc_pool_test_topo blocks = {
		.nodes = 2, .ncpus = svc_pool_test_ncpus(),
	};
	const struct svc_pool_test_topo interleaved = {
		.nodes = 2, .ncpus = svc_pool_test_ncpus(), .interleave = true,
	};

	if (blocks.ncpus < 2)
		kunit_skip(test, "needs 2 cpu ids");
	svc_pool_test_map(test, &blocks);
	svc_pool_test_map(test, &interleaved);
}

/* A cpu that comes online after the map is built uses its own node's pool */
static void svc_pool_map_hotplug_test(struct kunit *test)
{
	const struct svc_pool_test_topo one = { .nodes = 1, .ncpus = 4 };
	const struct svc_pool_test_topo two = { .nodes = 2, .ncpus = 4 };
	struct svc_pool_test_map *m;

	if (nr_cpu_ids < 4)
		kunit_skip(test, "needs 4 cpu ids");

	m = svc_pool_test_fill(test, &one, 3);
	KUNIT_EXPECT_EQ(test, m->npools, 3U);
	KUNIT_EXPECT_EQ(test, m->to_pool[3], 0U);

	/* node 1 is cpus 2-3: cpu 3 goes to node 1's first pool, not 0 */
	m = svc_pool_test_fill(test, &two, 3);
	KUNIT_EXPECT_EQ(test, m->npools, 3U);
	KUNIT_EXPECT_EQ(test, m->to_pool[2], 2U);
	KUNIT_EXPECT_EQ(test, m->to_pool[3], 2U);

	/* node 1 has no pool at all: pool 0 */
	m = svc_pool_test_fill(test, &two, 2);
	KUNIT_EXPECT_EQ(test, m->npools, 2U);
	KUNIT_EXPECT_EQ(test, m->to_pool[2], 0U);
	KUNIT_EXPECT_EQ(test, m->to_pool[3], 0U);
}

struct svc_pool_test_rig {
	unsigned int n;
	struct svc_pool *pools;
	struct svc_pool_ext *ext;
};

static void svc_pool_test_rig_free(void *arg)
{
	struct svc_pool_test_rig *rig = arg;
	unsigned int i;

	for (i = 0; i < rig->n; i++) {
		percpu_counter_destroy(&rig->pools[i].sp_sockets_queued);
		percpu_counter_destroy(&rig->ext[i].sibling_wakes);
		percpu_counter_destroy(&rig->ext[i].steals);
		percpu_counter_destroy(&rig->ext[i].spin_hits);
	}
}

/*
 * @n pools over @nodes nodes in consecutive runs, each pool with
 * sp_nrthrmax 1 (pass threads to give other counts, or NULL).
 */
static struct svc_pool_test_rig *
svc_pool_test_rig(struct kunit *test, unsigned int n, unsigned int nodes,
		  const unsigned int *threads)
{
	struct svc_pool_idle_line *lines;
	struct svc_pool_test_rig *rig;
	unsigned int i;

	rig = kunit_kzalloc(test, sizeof(*rig), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, rig);
	rig->pools = kunit_kcalloc(test, n, sizeof(*rig->pools), GFP_KERNEL);
	rig->ext = kunit_kcalloc(test, n, sizeof(*rig->ext), GFP_KERNEL);
	lines = kunit_kzalloc(test, roundup_pow_of_two(n * sizeof(*lines)),
			      GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, rig->pools);
	KUNIT_ASSERT_NOT_NULL(test, rig->ext);
	KUNIT_ASSERT_NOT_NULL(test, lines);
	for (i = 0; i < n; i++) {
		rig->pools[i].sp_id = i;
		rig->pools[i].sp_nrthrmax = threads ? threads[i] : 1;
		lwq_init(&rig->pools[i].sp_xprts);
		init_llist_head(&rig->pools[i].sp_idle_threads);
		rig->ext[i].node = i * nodes / n;
		rig->ext[i].lines = lines;
		KUNIT_ASSERT_EQ(test, percpu_counter_init(
				&rig->pools[i].sp_sockets_queued, 0, GFP_KERNEL), 0);
		KUNIT_ASSERT_EQ(test, percpu_counter_init(
				&rig->ext[i].sibling_wakes, 0, GFP_KERNEL), 0);
		KUNIT_ASSERT_EQ(test, percpu_counter_init(
				&rig->ext[i].steals, 0, GFP_KERNEL), 0);
		KUNIT_ASSERT_EQ(test, percpu_counter_init(
				&rig->ext[i].spin_hits, 0, GFP_KERNEL), 0);
		rig->n = i + 1;
	}
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
				svc_pool_test_rig_free, rig), 0);
	svc_pool_ext_layout(rig->pools, rig->ext, n);
	return rig;
}

/* Put an idle, running thread (the test task) on pool @i's idle list */
static struct svc_rqst *svc_pool_test_idle(struct kunit *test,
					   struct svc_pool_test_rig *rig,
					   unsigned int i)
{
	struct svc_rqst *rqstp = kunit_kzalloc(test, sizeof(*rqstp),
					       GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, rqstp);
	rqstp->rq_task = current;
	rqstp->rq_pool = &rig->pools[i];
	svc_pool_idle_add(&rig->ext[i], &rig->pools[i], rqstp);
	return rqstp;
}

static struct svc_xprt *svc_pool_test_xprts(struct kunit *test,
					    struct svc_pool_test_rig *rig,
					    unsigned int pool, unsigned int nx)
{
	struct svc_xprt *x = kunit_kcalloc(test, nx, sizeof(*x), GFP_KERNEL);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, x);
	for (i = 0; i < nx; i++) {
		kref_init(&x[i].xpt_ref);
		lwq_enqueue(&x[i].xpt_ready, &rig->pools[pool].sp_xprts);
	}
	return x;
}

static struct svc_serv *svc_pool_test_serv(struct kunit *test,
					   struct svc_pool_test_rig *rig)
{
	struct svc_serv *serv = kunit_kzalloc(test, sizeof(*serv), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, serv);
	serv->sv_is_pooled = true;
	serv->sv_pools = rig->pools;
	serv->sv_pool_ext = rig->ext;
	return serv;
}

/* N1: a pool's idle bit follows its idle list going non-empty and empty */
static void svc_pool_idle_map_test(struct kunit *test)
{
	struct svc_pool_test_rig *rig = svc_pool_test_rig(test, 24, 2, NULL);
	struct svc_pool_ext *e = &rig->ext[14];
	struct svc_pool *pool = &rig->pools[14];
	struct svc_rqst *a, *b;

	/* one map per node, each from a line of its own */
	KUNIT_EXPECT_EQ(test, e->place, 2U);
	KUNIT_EXPECT_PTR_EQ(test, e->idle, rig->ext[12].idle);
	KUNIT_EXPECT_PTR_NE(test, e->idle, rig->ext[11].idle);
	KUNIT_EXPECT_EQ(test, (unsigned long)e->idle % SMP_CACHE_BYTES, 0UL);
	KUNIT_EXPECT_FALSE(test, test_bit(2, e->idle));

	a = svc_pool_test_idle(test, rig, 14);
	KUNIT_EXPECT_TRUE(test, test_bit(2, e->idle));
	b = svc_pool_test_idle(test, rig, 14);
	/* a claim does not write the map */
	KUNIT_EXPECT_TRUE(test, svc_pool_wake_idle_thread(pool));
	KUNIT_EXPECT_TRUE(test, test_bit(2, e->idle));
	/* only the first thread leaves; the list stays non-empty */
	KUNIT_EXPECT_FALSE(test, svc_pool_idle_del(e, pool, a));
	KUNIT_EXPECT_TRUE(test, svc_pool_idle_del(e, pool, b));
	KUNIT_EXPECT_TRUE(test, test_bit(2, e->idle));
	KUNIT_EXPECT_TRUE(test, svc_pool_idle_del(e, pool, a));
	KUNIT_EXPECT_FALSE(test, test_bit(2, e->idle));
	KUNIT_EXPECT_EQ(test, bitmap_weight(e->idle, 12), 0U);
	KUNIT_EXPECT_EQ(test, bitmap_weight(rig->ext[0].idle, 12), 0U);
}

/* N1: a saturated node's wake reads its idle map, not every idle list */
static void svc_pool_saturated_no_walk_test(struct kunit *test)
{
	struct svc_pool_test_rig *rig = svc_pool_test_rig(test, 32, 2, NULL);
	struct svc_rqst *hid[2];

	/*
	 * Idle threads with clear bits, beyond the eight-pool window and in
	 * the remote window: the wake must not look at their idle lists.
	 */
	hid[0] = svc_pool_test_idle(test, rig, 12);
	hid[1] = svc_pool_test_idle(test, rig, 17);
	clear_bit(12, rig->ext[12].idle);
	clear_bit(1, rig->ext[17].idle);
	KUNIT_EXPECT_EQ(test, svc_pool_wake_sibling(rig->pools, rig->ext, 0),
			0U);
	KUNIT_EXPECT_FALSE(test, test_bit(RQ_WAKING, &hid[0]->rq_flags));
	KUNIT_EXPECT_FALSE(test, test_bit(RQ_WAKING, &hid[1]->rq_flags));
	KUNIT_EXPECT_EQ(test,
			percpu_counter_sum(&rig->ext[0].sibling_wakes), 0);
	/* with its bit set, the thread beyond the window is found */
	set_bit(12, rig->ext[12].idle);
	KUNIT_EXPECT_EQ(test, svc_pool_wake_sibling(rig->pools, rig->ext, 0),
			12U);
	KUNIT_EXPECT_TRUE(test, test_bit(RQ_WAKING, &hid[0]->rq_flags));
}

/* N1: past the window, the idle map is tried nearest first, wrapping */
static void svc_pool_idle_map_wrap_test(struct kunit *test)
{
	struct svc_pool_test_rig *rig = svc_pool_test_rig(test, 32, 2, NULL);
	struct svc_rqst *r6, *r9;

	/* from 12 the window is 13-15 and 0-4; 6 comes before 9 */
	r9 = svc_pool_test_idle(test, rig, 9);
	r6 = svc_pool_test_idle(test, rig, 6);
	KUNIT_EXPECT_EQ(test, svc_pool_wake_sibling(rig->pools, rig->ext, 12),
			6U);
	KUNIT_EXPECT_TRUE(test, test_bit(RQ_WAKING, &r6->rq_flags));
	KUNIT_EXPECT_EQ(test, svc_pool_wake_sibling(rig->pools, rig->ext, 12),
			9U);
	KUNIT_EXPECT_TRUE(test, test_bit(RQ_WAKING, &r9->rq_flags));
	/* both claimed, none on the other node: nothing to wake */
	KUNIT_EXPECT_EQ(test, svc_pool_wake_sibling(rig->pools, rig->ext, 12),
			12U);
}

/*
 * N2: with sparse threads (node 0 at places 5 and 11, node 1 at 11) the
 * far windows, mapped through serve, reach the remote thread both ways.
 */
static void svc_pool_sparse_far_test(struct kunit *test)
{
	unsigned int threads[24] = { [5] = 1, [11] = 1, [23] = 1 };
	struct svc_pool_test_rig *rig = svc_pool_test_rig(test, 24, 2, threads);
	struct svc_xprt *x, *got;
	struct svc_rqst *r;
	unsigned int from = UINT_MAX;

	r = svc_pool_test_idle(test, rig, 23);
	KUNIT_EXPECT_EQ(test, svc_pool_wake_sibling(rig->pools, rig->ext, 5),
			23U);
	KUNIT_EXPECT_TRUE(test, test_bit(RQ_WAKING, &r->rq_flags));
	KUNIT_ASSERT_TRUE(test, svc_pool_idle_del(&rig->ext[23],
						  &rig->pools[23], r));

	/* and pool 23's thread takes pool 5's backlog */
	x = svc_pool_test_xprts(test, rig, 5, 2);
	got = svc_xprt_steal(rig->pools, rig->ext, 23, &from);
	KUNIT_ASSERT_NOT_NULL(test, got);
	KUNIT_EXPECT_EQ(test, from, 5U);
	KUNIT_EXPECT_LT(test, (unsigned long)(got - x), 2UL);
}

/* M1: a backlog behind one busy thread is taken by sibling pools */
static void svc_pool_wake_sibling_test(struct kunit *test)
{
	struct svc_pool_test_rig *rig = svc_pool_test_rig(test, 12, 1, NULL);
	struct svc_rqst *r[4];
	struct svc_xprt *x, *got[4];
	unsigned int i, from;

	/* pool 0's thread is busy; pools 1-3 each have a sleeping one */
	for (i = 1; i < 4; i++)
		r[i] = svc_pool_test_idle(test, rig, i);
	x = svc_pool_test_xprts(test, rig, 0, 3);
	for (i = 1; i < 4; i++) {
		KUNIT_EXPECT_FALSE(test, svc_pool_wake_idle_thread(&rig->pools[0]));
		KUNIT_EXPECT_EQ(test, svc_pool_wake_sibling(rig->pools, rig->ext,
							    0), i);
		KUNIT_EXPECT_TRUE(test, test_bit(RQ_WAKING, &r[i]->rq_flags));
	}
	/* every sibling is claimed: nothing more to wake */
	KUNIT_EXPECT_EQ(test, svc_pool_wake_sibling(rig->pools, rig->ext, 0),
			0U);
	KUNIT_EXPECT_EQ(test,
			percpu_counter_sum(&rig->ext[0].sibling_wakes), 3);

	/* the claimed threads find pool 0's transports, one each */
	for (i = 1; i < 4; i++) {
		from = UINT_MAX;
		got[i] = svc_xprt_steal(rig->pools, rig->ext, i, &from);
		KUNIT_ASSERT_NOT_NULL(test, got[i]);
		KUNIT_EXPECT_EQ(test, from, 0U);
		KUNIT_EXPECT_LT(test, (unsigned long)(got[i] - x), 3UL);
	}
	KUNIT_EXPECT_PTR_NE(test, got[1], got[2]);
	KUNIT_EXPECT_PTR_NE(test, got[1], got[3]);
	KUNIT_EXPECT_PTR_NE(test, got[2], got[3]);
	KUNIT_EXPECT_TRUE(test, lwq_empty(&rig->pools[0].sp_xprts));
}

/* M1: the local idle thread is already claimed, so a sibling is woken */
static void svc_pool_wake_claimed_test(struct kunit *test)
{
	struct svc_pool_test_rig *rig = svc_pool_test_rig(test, 12, 1, NULL);
	struct svc_rqst *local, *sib;

	sib = svc_pool_test_idle(test, rig, 4);
	local = svc_pool_test_idle(test, rig, 0);
	KUNIT_EXPECT_TRUE(test, svc_pool_wake_idle_thread(&rig->pools[0]));
	KUNIT_EXPECT_TRUE(test, test_bit(RQ_WAKING, &local->rq_flags));
	KUNIT_EXPECT_FALSE(test, svc_pool_wake_idle_thread(&rig->pools[0]));
	KUNIT_EXPECT_EQ(test, svc_pool_wake_sibling(rig->pools, rig->ext, 0),
			4U);
	KUNIT_EXPECT_TRUE(test, test_bit(RQ_WAKING, &sib->rq_flags));
	/* pool 4 is in pool 0's window, and pool 0 in pool 4's */
	svc_pool_test_xprts(test, rig, 0, 1);
	KUNIT_EXPECT_PTR_EQ(test, svc_pool_steal_source(rig->pools, rig->ext,
							4), &rig->pools[0]);
	/* pool 9 is more than SVC_POOL_STEAL_SPAN pools on */
	KUNIT_EXPECT_NULL(test, svc_pool_steal_source(rig->pools, rig->ext,
						      9));
}

/* M2: the window wraps within the node, for the last pool of each node */
static void svc_pool_node_wrap_test(struct kunit *test)
{
	struct svc_pool_test_rig *rig = svc_pool_test_rig(test, 12, 2, NULL);

	KUNIT_EXPECT_EQ(test, READ_ONCE(rig->ext[5].range), 0U << 16 | 6);
	KUNIT_EXPECT_EQ(test, READ_ONCE(rig->ext[11].range), 6U << 16 | 6);

	svc_pool_test_idle(test, rig, 0);
	svc_pool_test_idle(test, rig, 6);
	KUNIT_EXPECT_EQ(test, svc_pool_wake_sibling(rig->pools, rig->ext, 5),
			0U);
	KUNIT_EXPECT_EQ(test, svc_pool_wake_sibling(rig->pools, rig->ext, 11),
			6U);
	/* both claimed now: no idle thread on either node */
	KUNIT_EXPECT_EQ(test, svc_pool_wake_sibling(rig->pools, rig->ext, 5),
			5U);
	KUNIT_EXPECT_EQ(test, svc_pool_wake_sibling(rig->pools, rig->ext, 11),
			11U);

	svc_pool_test_xprts(test, rig, 5, 1);
	KUNIT_EXPECT_PTR_EQ(test, svc_pool_steal_source(rig->pools, rig->ext,
							0), &rig->pools[5]);
	/* pool 9's node has no work, so it looks near its place in node 0 */
	KUNIT_EXPECT_PTR_EQ(test, svc_pool_steal_source(rig->pools, rig->ext,
							9), &rig->pools[5]);
	svc_pool_test_xprts(test, rig, 11, 1);
	KUNIT_EXPECT_PTR_EQ(test, svc_pool_steal_source(rig->pools, rig->ext,
							6), &rig->pools[11]);
	KUNIT_EXPECT_PTR_EQ(test, svc_pool_steal_source(rig->pools, rig->ext,
							0), &rig->pools[5]);
}

/*
 * Arrivals on one node reach the other node's threads, but only when the
 * arrival node has no idle thread or no work nearby.
 */
static void svc_pool_cross_node_test(struct kunit *test)
{
	struct svc_pool_test_rig *rig = svc_pool_test_rig(test, 12, 2, NULL);
	struct svc_rqst *near, *remote;
	struct svc_xprt *x, *got;
	unsigned int from = UINT_MAX;

	/* each pool's far pool is the first pool of the other node */
	KUNIT_EXPECT_EQ(test, READ_ONCE(rig->ext[0].far), 6U);
	KUNIT_EXPECT_EQ(test, READ_ONCE(rig->ext[2].far), 6U);
	KUNIT_EXPECT_EQ(test, READ_ONCE(rig->ext[11].far), 0U);

	remote = svc_pool_test_idle(test, rig, 1);
	near = svc_pool_test_idle(test, rig, 7);
	x = svc_pool_test_xprts(test, rig, 8, 2);
	/* the idle thread on pool 8's own node is preferred */
	KUNIT_EXPECT_EQ(test, svc_pool_wake_sibling(rig->pools, rig->ext, 8),
			7U);
	KUNIT_EXPECT_TRUE(test, test_bit(RQ_WAKING, &near->rq_flags));
	KUNIT_EXPECT_FALSE(test, test_bit(RQ_WAKING, &remote->rq_flags));
	/* that one claimed, a thread of the other node is */
	KUNIT_EXPECT_EQ(test, svc_pool_wake_sibling(rig->pools, rig->ext, 8),
			1U);
	KUNIT_EXPECT_TRUE(test, test_bit(RQ_WAKING, &remote->rq_flags));
	KUNIT_EXPECT_EQ(test,
			percpu_counter_sum(&rig->ext[8].sibling_wakes), 2);
	KUNIT_EXPECT_EQ(test, svc_pool_wake_sibling(rig->pools, rig->ext, 8),
			8U);

	/* the node-0 thread finds pool 8's work near its place in node 1 */
	got = svc_xprt_steal(rig->pools, rig->ext, 1, &from);
	KUNIT_ASSERT_NOT_NULL(test, got);
	KUNIT_EXPECT_EQ(test, from, 8U);
	KUNIT_EXPECT_LT(test, (unsigned long)(got - x), 2UL);
	/* work on its own node comes first */
	svc_pool_test_xprts(test, rig, 4, 1);
	KUNIT_EXPECT_PTR_EQ(test, svc_pool_steal_source(rig->pools, rig->ext,
							1), &rig->pools[4]);
}

/* pernode pools (one per node) have no far pool and never cross */
static void svc_pool_pernode_no_cross_test(struct kunit *test)
{
	struct svc_pool_test_rig *rig = svc_pool_test_rig(test, 2, 2, NULL);

	KUNIT_EXPECT_EQ(test, READ_ONCE(rig->ext[0].far), 0U);
	KUNIT_EXPECT_EQ(test, READ_ONCE(rig->ext[1].far), 1U);
	svc_pool_test_idle(test, rig, 1);
	svc_pool_test_xprts(test, rig, 0, 1);
	KUNIT_EXPECT_EQ(test, svc_pool_wake_sibling(rig->pools, rig->ext, 0),
			0U);
	KUNIT_EXPECT_NULL(test, svc_pool_steal_source(rig->pools, rig->ext, 1));
}

/* Give the rig's pools the nodes @nodes and lay them out again */
static void svc_pool_test_renode(struct svc_pool_test_rig *rig,
				 const unsigned int *nodes)
{
	unsigned int i;

	for (i = 0; i < rig->n; i++)
		rig->ext[i].node = nodes[i];
	svc_pool_ext_layout(rig->pools, rig->ext, rig->n);
}

/* The cross-node window is symmetric, bounded and never empty */
static void svc_pool_far_window_test(struct kunit *test)
{
	unsigned int n, m, j, i, lo, hi, lo2, hi2, lim;

	for (n = 1; n <= 20; n++) {
		for (m = 1; m <= 20; m++) {
			/* SVC_POOL_STEAL_SPAN, or the ratio of the sizes */
			lim = max(8U, DIV_ROUND_UP(max(n, m) + 1, min(n, m)));
			for (j = 0; j < n; j++) {
				svc_pool_far_window(j, n, m, &lo, &hi);
				KUNIT_EXPECT_LE(test, lo, hi);
				KUNIT_EXPECT_LT(test, hi, m);
				KUNIT_EXPECT_LE(test, hi - lo + 1, lim);
				for (i = 0; i < m; i++) {
					svc_pool_far_window(i, m, n, &lo2, &hi2);
					KUNIT_EXPECT_EQ(test, i >= lo && i <= hi,
							j >= lo2 && j <= hi2);
				}
			}
		}
	}
	svc_pool_far_window(0, 0, 4, &lo, &hi);
	KUNIT_EXPECT_GT(test, lo, hi);
	svc_pool_far_window(3, 3, 4, &lo, &hi);
	KUNIT_EXPECT_GT(test, lo, hi);
}

/*
 * Three nodes of 2, 9 and 3 pools: every claim a node-1 wake makes on
 * another node is found by the claimed thread's steal, whichever node
 * the far chain reaches it on.
 */
static void svc_pool_three_node_test(struct kunit *test)
{
	static const unsigned int nodes[14] = {
		0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2,
	};
	struct svc_pool_test_rig *rig;
	struct svc_xprt *x;
	unsigned int s, p, i, from, pass;

	for (pass = 0; pass < 2; pass++) {
		for (s = 2; s <= 10; s++) {
			rig = svc_pool_test_rig(test, 14, 1, NULL);
			svc_pool_test_renode(rig, nodes);
			/* idle threads on node 2, then on node 0 only */
			for (i = 0; i < 14; i++)
				if (nodes[i] == (pass ? 0 : 2))
					svc_pool_test_idle(test, rig, i);
			x = svc_pool_test_xprts(test, rig, s, 1);
			p = svc_pool_wake_sibling(rig->pools, rig->ext, s);
			KUNIT_EXPECT_EQ(test, nodes[p], pass ? 0U : 2U);
			from = UINT_MAX;
			KUNIT_EXPECT_PTR_EQ(test, svc_xprt_steal(rig->pools,
					    rig->ext, p, &from), x);
			KUNIT_EXPECT_EQ(test, from, s);
		}
	}
	KUNIT_EXPECT_EQ(test, READ_ONCE(rig->ext[1].far), 2U);
	KUNIT_EXPECT_EQ(test, READ_ONCE(rig->ext[10].far), 11U);
	KUNIT_EXPECT_EQ(test, READ_ONCE(rig->ext[13].far), 0U);
	KUNIT_EXPECT_EQ(test, READ_ONCE(rig->ext[5].range), 2U << 16 | 9);
	/* no idle thread on any node: the far chain ends, nothing claimed */
	rig = svc_pool_test_rig(test, 14, 1, NULL);
	svc_pool_test_renode(rig, nodes);
	KUNIT_EXPECT_EQ(test, svc_pool_wake_sibling(rig->pools, rig->ext, 6),
			6U);
	KUNIT_EXPECT_NULL(test, svc_pool_steal_source(rig->pools, rig->ext,
						      12));
}

/* A node with no threads makes one run of all pools: far is the pool itself */
static void svc_pool_whole_no_far_test(struct kunit *test)
{
	static const unsigned int threads[12] = { 1, 1, 1, 1, 1, 1 };
	struct svc_pool_test_rig *rig = svc_pool_test_rig(test, 12, 2,
							  threads);
	struct svc_serv *serv = svc_pool_test_serv(test, rig);
	struct svc_xprt *x = kunit_kzalloc(test, sizeof(*x), GFP_KERNEL);
	struct svc_rqst *sib;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, x);
	kref_init(&x->xpt_ref);
	for (i = 0; i < 12; i++) {
		KUNIT_EXPECT_EQ(test, READ_ONCE(rig->ext[i].far), i);
		KUNIT_EXPECT_EQ(test, READ_ONCE(rig->ext[i].range),
				0U << 16 | 12);
	}
	/* the one run keeps the eight-pool window */
	sib = svc_pool_test_idle(test, rig, 5);
	svc_pool_enqueue_xprt(serv, &rig->pools[0], x);
	KUNIT_EXPECT_TRUE(test, test_bit(RQ_WAKING, &sib->rq_flags));
	KUNIT_EXPECT_EQ(test,
			percpu_counter_sum(&rig->ext[0].sibling_wakes), 1);
}

/* An enqueue on a one-pool node reaches the threads of a larger node */
static void svc_pool_enqueue_single_node_test(struct kunit *test)
{
	static const unsigned int nodes[4] = { 0, 1, 1, 1 };
	struct svc_pool_test_rig *rig = svc_pool_test_rig(test, 4, 1, NULL);
	struct svc_serv *serv = svc_pool_test_serv(test, rig);
	struct svc_xprt *x = kunit_kzalloc(test, sizeof(*x), GFP_KERNEL);
	struct svc_rqst *remote;
	unsigned int from = UINT_MAX;

	KUNIT_ASSERT_NOT_NULL(test, x);
	kref_init(&x->xpt_ref);
	svc_pool_test_renode(rig, nodes);
	/* one pool in its run, but a far pool: svc_pool_has_siblings() */
	KUNIT_EXPECT_EQ(test, READ_ONCE(rig->ext[0].range), 0U << 16 | 1);
	KUNIT_EXPECT_EQ(test, READ_ONCE(rig->ext[0].far), 1U);
	remote = svc_pool_test_idle(test, rig, 2);
	svc_pool_enqueue_xprt(serv, &rig->pools[0], x);
	KUNIT_EXPECT_TRUE(test, test_bit(RQ_WAKING, &remote->rq_flags));
	KUNIT_EXPECT_EQ(test,
			percpu_counter_sum(&rig->ext[0].sibling_wakes), 1);
	KUNIT_EXPECT_PTR_EQ(test, svc_xprt_steal(rig->pools, rig->ext, 2,
						 &from), x);
	KUNIT_EXPECT_EQ(test, from, 0U);
}

/*
 * No cross-node wake while any pool of the node has an idle thread, even
 * beyond the eight nearest; a remote steal leaves a queue whose own pool
 * has an unclaimed idle thread.
 */
static void svc_pool_no_cross_beyond_window_test(struct kunit *test)
{
	struct svc_pool_test_rig *rig = svc_pool_test_rig(test, 24, 2, NULL);
	struct svc_serv *serv = svc_pool_test_serv(test, rig);
	struct svc_xprt *x = kunit_kcalloc(test, 2, sizeof(*x), GFP_KERNEL);
	struct svc_rqst *near, *remote;
	unsigned int from = UINT_MAX;

	KUNIT_ASSERT_NOT_NULL(test, x);
	kref_init(&x[0].xpt_ref);
	kref_init(&x[1].xpt_ref);
	/* node 0 is pools 0-11: pool 10 is ten pools on from pool 0 */
	near = svc_pool_test_idle(test, rig, 10);
	remote = svc_pool_test_idle(test, rig, 12);
	svc_pool_enqueue_xprt(serv, &rig->pools[0], &x[0]);
	KUNIT_EXPECT_TRUE(test, test_bit(RQ_WAKING, &near->rq_flags));
	KUNIT_EXPECT_FALSE(test, test_bit(RQ_WAKING, &remote->rq_flags));
	/* pool 10's thread takes pool 0's work before other nodes' work */
	svc_pool_test_xprts(test, rig, 13, 1);
	KUNIT_EXPECT_PTR_EQ(test, svc_xprt_steal(rig->pools, rig->ext, 10,
						 &from), &x[0]);
	KUNIT_EXPECT_EQ(test, from, 0U);
	/* with that thread claimed, the other node's thread is */
	svc_pool_enqueue_xprt(serv, &rig->pools[0], &x[1]);
	KUNIT_EXPECT_TRUE(test, test_bit(RQ_WAKING, &remote->rq_flags));
	/* pool 12's own thread is claimed, pool 13's work has none: taken */
	KUNIT_EXPECT_PTR_EQ(test, svc_pool_steal_source(rig->pools, rig->ext,
							3), &rig->pools[0]);
	lwq_dequeue(&rig->pools[0].sp_xprts, struct svc_xprt, xpt_ready);
	KUNIT_EXPECT_PTR_EQ(test, svc_pool_steal_source(rig->pools, rig->ext,
							3), &rig->pools[13]);
	/* an unclaimed idle thread of pool 13 serves it: left alone */
	svc_pool_test_idle(test, rig, 13);
	KUNIT_EXPECT_NULL(test, svc_pool_steal_source(rig->pools, rig->ext, 3));
}

static void svc_pool_share_test(struct kunit *test)
{
	static const unsigned int totals[] = { 0, 1, 8, 191, 192, 200 };
	unsigned int t, i, sum, s;

	for (t = 0; t < ARRAY_SIZE(totals); t++) {
		for (sum = 0, i = 0; i < 192; i++) {
			s = svc_pool_share(totals[t], i, 192);
			KUNIT_EXPECT_LE(test, s, DIV_ROUND_UP(totals[t], 192));
			KUNIT_EXPECT_GE(test, s, totals[t] / 192);
			sum += s;
		}
		KUNIT_EXPECT_EQ(test, sum, totals[t]);
	}
}

/*
 * @n pools over @nodes nodes with @total threads placed as
 * svc_set_num_threads() places them: svc_pool_split_init() and
 * svc_pool_split_next(), then svc_pool_ext_layout().
 */
static struct svc_pool_test_rig *
svc_pool_test_split_rig(struct kunit *test, unsigned int n,
			unsigned int nodes, unsigned int total)
{
	struct svc_pool_test_rig *rig = svc_pool_test_rig(test, n, nodes, NULL);
	struct svc_pool_split split;
	unsigned int i;

	svc_pool_split_init(&split, rig->ext, n, total);
	for (i = 0; i < n; i++)
		rig->pools[i].sp_nrthrmax = svc_pool_split_next(&split,
								rig->ext, i);
	svc_pool_ext_layout(rig->pools, rig->ext, n);
	return rig;
}

/* Distance from @a to @b around @a's run */
static unsigned int svc_pool_test_dist(const struct svc_pool_ext *ext,
				       unsigned int a, unsigned int b)
{
	unsigned int count = READ_ONCE(ext[a].range) & 0xffff;
	unsigned int d = a > b ? a - b : b - a;

	return min(d, count - d);
}

/*
 * Check a split layout of @total threads: the counts add up, no pool has
 * two threads while another has none, every pool is served by the nearest
 * pool of its run that has threads, and that pool's steal walk reaches
 * it.  Returns the most threadless pools that one pool serves.
 */
static unsigned int svc_pool_test_check_split(struct kunit *test,
					      struct svc_pool_test_rig *rig,
					      unsigned int total)
{
	unsigned int n = rig->n, i, j, sum = 0, most = 0, from, *served;
	struct svc_xprt *x;

	served = kunit_kcalloc(test, n, sizeof(*served), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, served);
	for (i = 0; i < n; i++) {
		sum += rig->pools[i].sp_nrthrmax;
		if (total < n)
			KUNIT_EXPECT_LE(test, rig->pools[i].sp_nrthrmax, 1U);
	}
	KUNIT_EXPECT_EQ(test, sum, total);
	for (i = 0; i < n; i++) {
		unsigned int s = READ_ONCE(rig->ext[i].serve);
		unsigned int range = READ_ONCE(rig->ext[i].range);

		KUNIT_ASSERT_LT(test, s, n);
		KUNIT_EXPECT_GT(test, rig->pools[s].sp_nrthrmax, 0U);
		for (j = range >> 16; j < (range >> 16) + (range & 0xffff); j++)
			if (rig->pools[j].sp_nrthrmax)
				KUNIT_EXPECT_LE(test,
					svc_pool_test_dist(rig->ext, i, s),
					svc_pool_test_dist(rig->ext, i, j));
		if (s == i)
			continue;
		served[s]++;
		x = svc_pool_test_xprts(test, rig, i, 1);
		from = UINT_MAX;
		KUNIT_EXPECT_PTR_EQ(test, svc_xprt_steal(rig->pools, rig->ext,
							 s, &from), x);
		KUNIT_EXPECT_EQ(test, from, i);
	}
	for (i = 0; i < n; i++)
		most = max(most, served[i]);
	return most;
}

/*
 * Fewer threads than pools, placed by the real split: threads go to the
 * nodes by pool count and spread evenly within each, so every thread
 * serves a short block of its own node.
 */
static void svc_pool_split_test(struct kunit *test)
{
	struct svc_pool_test_rig *rig;
	unsigned int i, node[2];

	/* 64 pools, 16 threads, one node: each serves 3 pools */
	rig = svc_pool_test_split_rig(test, 64, 1, 16);
	KUNIT_EXPECT_EQ(test, svc_pool_test_check_split(test, rig, 16), 3U);

	/* 2 x 64 pools, 16 threads: 8 per node, each serving 7 of its node */
	rig = svc_pool_test_split_rig(test, 128, 2, 16);
	KUNIT_EXPECT_EQ(test, svc_pool_test_check_split(test, rig, 16), 7U);
	node[0] = node[1] = 0;
	for (i = 0; i < 128; i++) {
		node[rig->ext[i].node] += rig->pools[i].sp_nrthrmax;
		KUNIT_EXPECT_EQ(test,
			rig->ext[READ_ONCE(rig->ext[i].serve)].node,
			rig->ext[i].node);
	}
	KUNIT_EXPECT_EQ(test, node[0], 8U);
	KUNIT_EXPECT_EQ(test, node[1], 8U);

	/* 2 x 64 pools, 1 thread: it goes to node 0, which serves node 1 */
	rig = svc_pool_test_split_rig(test, 128, 2, 1);
	svc_pool_test_check_split(test, rig, 1);
	KUNIT_EXPECT_EQ(test, rig->pools[63].sp_nrthrmax, 1U);
	KUNIT_EXPECT_EQ(test, READ_ONCE(rig->ext[127].range), 128U);
	for (i = 64; i < 128; i++)
		KUNIT_EXPECT_EQ(test, READ_ONCE(rig->ext[i].serve), 63U);
}

/*
 * M3: 8 threads on 192 per-cpu pools over 2 nodes, as svc_set_num_threads()
 * splits them: 4 per node, at the end of every 24 pools; threadless pools
 * are served by the nearest of those in their node.
 */
static void svc_pool_threadless_test(struct kunit *test)
{
	static const unsigned int t[] = { 0, 22, 24, 35, 72, 96, 190 };
	static const unsigned int want[] = { 95, 23, 23, 47, 71, 191, 191 };
	const unsigned int n = 192;
	struct svc_pool_test_rig *rig;
	unsigned int i, served[2] = { 0, 0 }, from;
	struct svc_xprt *x;

	rig = svc_pool_test_split_rig(test, n, 2, 8);
	for (i = 0; i < n; i++) {
		KUNIT_EXPECT_EQ(test, rig->pools[i].sp_nrthrmax,
				(i + 1) % 24 == 0 ? 1U : 0U);
		served[rig->ext[i].node] += rig->pools[i].sp_nrthrmax;
		KUNIT_EXPECT_EQ(test,
			rig->ext[READ_ONCE(rig->ext[i].serve)].node,
			rig->ext[i].node);
	}
	KUNIT_EXPECT_EQ(test, served[0], 4U);
	KUNIT_EXPECT_EQ(test, served[1], 4U);
	KUNIT_EXPECT_EQ(test, svc_pool_test_check_split(test, rig, 8), 23U);

	for (i = 0; i < ARRAY_SIZE(t); i++) {
		KUNIT_EXPECT_EQ(test, READ_ONCE(rig->ext[t[i]].serve), want[i]);
		x = svc_pool_test_xprts(test, rig, t[i], 1);
		from = UINT_MAX;
		KUNIT_EXPECT_PTR_EQ(test, svc_xprt_steal(rig->pools, rig->ext,
							 want[i], &from), x);
		KUNIT_EXPECT_EQ(test, from, t[i]);
	}

	/* one thread for two nodes: it is on node 0, which serves node 1 */
	rig = svc_pool_test_split_rig(test, n, 2, 1);
	KUNIT_EXPECT_EQ(test, READ_ONCE(rig->ext[0].serve), 95U);
	KUNIT_EXPECT_EQ(test, READ_ONCE(rig->ext[191].serve), 95U);
	KUNIT_EXPECT_EQ(test, READ_ONCE(rig->ext[0].range), (unsigned int)n);
	x = svc_pool_test_xprts(test, rig, 191, 1);
	KUNIT_EXPECT_PTR_EQ(test, svc_xprt_steal(rig->pools, rig->ext, 95,
						 &from), x);
	KUNIT_EXPECT_EQ(test, from, 191U);
}

/* svc_wake_up() reaches a thread when pool 0 has no threads */
static void svc_wake_up_threadless_test(struct kunit *test)
{
	/* one thread on 4 pools: the split puts it on pool 3 */
	struct svc_pool_test_rig *rig = svc_pool_test_split_rig(test, 4, 1, 1);
	struct svc_serv *serv = svc_pool_test_serv(test, rig);
	struct svc_rqst *rqstp;

	KUNIT_EXPECT_EQ(test, rig->pools[0].sp_nrthrmax, 0U);
	KUNIT_EXPECT_EQ(test, READ_ONCE(rig->ext[0].serve), 3U);
	rqstp = svc_pool_test_idle(test, rig, 3);
	rqstp->rq_server = serv;
	svc_wake_up(serv);
	KUNIT_EXPECT_TRUE(test, test_bit(SP_TASK_PENDING,
					 &rig->pools[3].sp_flags));
	KUNIT_EXPECT_FALSE(test, test_bit(SP_TASK_PENDING,
					  &rig->pools[0].sp_flags));
	KUNIT_EXPECT_TRUE(test, test_bit(RQ_WAKING, &rqstp->rq_flags));
	/* the thread does not sleep on the pending task */
	clear_bit(RQ_WAKING, &rqstp->rq_flags);
	rqstp->rq_spin_ns = 1000;
	KUNIT_EXPECT_TRUE(test, svc_thread_idle_spin(rqstp, 10));

	/* pool 0 with a thread: the flag stays on pool 0 */
	rig = svc_pool_test_rig(test, 4, 1, NULL);
	serv = svc_pool_test_serv(test, rig);
	svc_wake_up(serv);
	KUNIT_EXPECT_TRUE(test, test_bit(SP_TASK_PENDING,
					 &rig->pools[0].sp_flags));
}

/* A claimed thread that cannot leave the idle list passes its claim on */
static void svc_thread_pass_claim_test(struct kunit *test)
{
	struct svc_pool_test_rig *rig = svc_pool_test_rig(test, 12, 1, NULL);
	struct svc_serv *serv = svc_pool_test_serv(test, rig);
	struct svc_rqst *old, *new, *sib;

	sib = svc_pool_test_idle(test, rig, 1);
	old = svc_pool_test_idle(test, rig, 0);
	old->rq_server = serv;
	KUNIT_EXPECT_TRUE(test, svc_pool_wake_idle_thread(&rig->pools[0]));
	/* a newer idle thread is now first, so @old cannot remove itself */
	new = svc_pool_test_idle(test, rig, 0);
	KUNIT_EXPECT_FALSE(test, llist_del_first_this(
				&rig->pools[0].sp_idle_threads, &old->rq_idle));
	KUNIT_EXPECT_TRUE(test, test_and_clear_bit(RQ_WAKING, &old->rq_flags));
	svc_thread_pass_claim(old);
	KUNIT_EXPECT_TRUE(test, test_bit(RQ_WAKING, &new->rq_flags));
	KUNIT_EXPECT_FALSE(test, test_bit(RQ_WAKING, &sib->rq_flags));
	/* the first idle thread is claimed: the claim goes to a sibling */
	svc_thread_pass_claim(old);
	KUNIT_EXPECT_TRUE(test, test_bit(RQ_WAKING, &sib->rq_flags));
	KUNIT_EXPECT_EQ(test,
			percpu_counter_sum(&rig->ext[0].sibling_wakes), 1);
}

/* The enqueue path: local claim first, else a sibling that steals */
static void svc_pool_enqueue_sibling_test(struct kunit *test)
{
	struct svc_pool_test_rig *rig = svc_pool_test_rig(test, 12, 1, NULL);
	struct svc_serv *serv = svc_pool_test_serv(test, rig);
	struct svc_xprt *x = kunit_kcalloc(test, 2, sizeof(*x), GFP_KERNEL);
	struct svc_rqst *sib, *local;
	unsigned int from = UINT_MAX;

	KUNIT_ASSERT_NOT_NULL(test, x);
	kref_init(&x[0].xpt_ref);
	kref_init(&x[1].xpt_ref);
	/* pool 0's only thread is busy; pool 2 has an idle one */
	sib = svc_pool_test_idle(test, rig, 2);
	svc_pool_enqueue_xprt(serv, &rig->pools[0], &x[0]);
	KUNIT_EXPECT_TRUE(test, test_bit(RQ_WAKING, &sib->rq_flags));
	KUNIT_EXPECT_EQ(test,
			percpu_counter_sum(&rig->ext[0].sibling_wakes), 1);
	KUNIT_EXPECT_EQ(test,
			percpu_counter_sum(&rig->pools[0].sp_sockets_queued), 1);
	KUNIT_EXPECT_PTR_EQ(test, svc_xprt_steal(rig->pools, rig->ext, 2,
						 &from), &x[0]);
	KUNIT_EXPECT_EQ(test, from, 0U);

	/* an unclaimed local idle thread is claimed, no sibling is woken */
	local = svc_pool_test_idle(test, rig, 5);
	svc_pool_enqueue_xprt(serv, &rig->pools[5], &x[1]);
	KUNIT_EXPECT_TRUE(test, test_bit(RQ_WAKING, &local->rq_flags));
	KUNIT_EXPECT_EQ(test,
			percpu_counter_sum(&rig->ext[5].sibling_wakes), 0);
}

static void svc_pool_steal_test(struct kunit *test)
{
	struct svc_pool_test_rig *rig = svc_pool_test_rig(test, 12, 1, NULL);
	const unsigned int nx = 5;
	struct svc_xprt *x, *got;
	unsigned int i, total = 0, from, taken[5] = { 0 };

	/* nothing queued anywhere */
	KUNIT_EXPECT_NULL(test, svc_pool_steal_source(rig->pools, rig->ext, 3));
	KUNIT_EXPECT_NULL(test, svc_xprt_steal(rig->pools, rig->ext, 3, &from));

	x = svc_pool_test_xprts(test, rig, 0, nx);

	/* pool 1 looks backward at pool 0; a pool never at itself */
	KUNIT_EXPECT_PTR_EQ(test, svc_pool_steal_source(rig->pools, rig->ext,
							1), &rig->pools[0]);
	KUNIT_EXPECT_NULL(test, svc_pool_steal_source(rig->pools, rig->ext, 0));
	KUNIT_EXPECT_NULL(test, svc_pool_steal_source(rig->pools, rig->ext, 11));
	/* the window wraps: pool 8 is eight pools forward of 0 */
	KUNIT_EXPECT_PTR_EQ(test, svc_pool_steal_source(rig->pools, rig->ext,
							8), &rig->pools[0]);
	KUNIT_EXPECT_NULL(test, svc_pool_steal_source(rig->pools, rig->ext, 9));

	/* thieves in pools 1..4 drain pool 0: each transport exactly once */
	for (i = 0; i < 2 * nx; i++) {
		got = svc_xprt_steal(rig->pools, rig->ext, 1 + i % 4, &from);
		if (!got)
			continue;
		taken[got - x]++;
		KUNIT_EXPECT_EQ(test, kref_read(&got->xpt_ref), 2U);
		total++;
	}
	KUNIT_EXPECT_EQ(test, total, nx);
	for (i = 0; i < nx; i++)
		KUNIT_EXPECT_EQ(test, taken[i], 1U);
	KUNIT_EXPECT_TRUE(test, lwq_empty(&rig->pools[0].sp_xprts));
}

#define SVC_POOL_TEST_THIEVES	4
#define SVC_POOL_TEST_XPRTS	256

struct svc_pool_test_race {
	struct svc_pool_test_rig *rig;
	struct svc_xprt *x;
	atomic_t taken[SVC_POOL_TEST_XPRTS];
	atomic_t total;
	struct completion done[SVC_POOL_TEST_THIEVES];
	unsigned int pool[SVC_POOL_TEST_THIEVES];
};

struct svc_pool_test_thief {
	struct svc_pool_test_race *race;
	unsigned int idx;
};

static int svc_pool_test_thief_fn(void *arg)
{
	struct svc_pool_test_thief *t = arg;
	struct svc_pool_test_race *r = t->race;
	struct svc_xprt *got;
	unsigned int from;

	while ((got = svc_xprt_steal(r->rig->pools, r->rig->ext,
				     r->pool[t->idx], &from))) {
		atomic_inc(&r->taken[got - r->x]);
		atomic_inc(&r->total);
		cond_resched();
	}
	complete(&r->done[t->idx]);
	return 0;
}

/* Concurrent thieves never take one transport twice */
static void svc_pool_steal_race_test(struct kunit *test)
{
	struct svc_pool_test_thief *t;
	struct svc_pool_test_race *r;
	struct task_struct *task[SVC_POOL_TEST_THIEVES];
	unsigned int i;

	r = kunit_kzalloc(test, sizeof(*r), GFP_KERNEL);
	t = kunit_kcalloc(test, SVC_POOL_TEST_THIEVES, sizeof(*t), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, r);
	KUNIT_ASSERT_NOT_NULL(test, t);
	r->rig = svc_pool_test_rig(test, 12, 1, NULL);
	r->x = svc_pool_test_xprts(test, r->rig, 0, SVC_POOL_TEST_XPRTS);
	for (i = 0; i < SVC_POOL_TEST_THIEVES; i++) {
		init_completion(&r->done[i]);
		r->pool[i] = 1 + i;
		t[i].race = r;
		t[i].idx = i;
		task[i] = kthread_create(svc_pool_test_thief_fn, &t[i],
					 "svc-thief/%u", i);
		KUNIT_ASSERT_FALSE(test, IS_ERR(task[i]));
	}
	for (i = 0; i < SVC_POOL_TEST_THIEVES; i++)
		wake_up_process(task[i]);
	for (i = 0; i < SVC_POOL_TEST_THIEVES; i++)
		KUNIT_ASSERT_GT(test, wait_for_completion_timeout(&r->done[i],
								  30 * HZ), 0UL);
	KUNIT_EXPECT_EQ(test, atomic_read(&r->total), SVC_POOL_TEST_XPRTS);
	for (i = 0; i < SVC_POOL_TEST_XPRTS; i++)
		KUNIT_EXPECT_EQ(test, atomic_read(&r->taken[i]), 1);
}

static void svc_idle_spin_arith_test(struct kunit *test)
{
	const unsigned int max = 10 * NSEC_PER_USEC;
	unsigned int ns = 0, steps = 0;

	/* growth starts at SVC_IDLE_SPIN_START_NS and doubles to the cap */
	ns = svc_idle_spin_grow(ns, max);
	KUNIT_EXPECT_EQ(test, ns, SVC_IDLE_SPIN_START_NS);
	KUNIT_EXPECT_EQ(test, svc_idle_spin_grow(ns, max), 2 * ns);
	while (ns < max && steps++ < 32)
		ns = svc_idle_spin_grow(ns, max);
	KUNIT_EXPECT_EQ(test, ns, max);
	KUNIT_EXPECT_EQ(test, svc_idle_spin_grow(ns, max), max);
	KUNIT_EXPECT_EQ(test, svc_idle_spin_grow(0, 500), 500U);

	/* decay halves and reaches 0 below the floor */
	KUNIT_EXPECT_EQ(test, svc_idle_spin_shrink(max), max / 2);
	KUNIT_EXPECT_EQ(test, svc_idle_spin_shrink(2 * SVC_IDLE_SPIN_FLOOR_NS),
			SVC_IDLE_SPIN_FLOOR_NS);
	KUNIT_EXPECT_EQ(test, svc_idle_spin_shrink(2 * SVC_IDLE_SPIN_FLOOR_NS - 1),
			0U);
	steps = 0;
	while (ns && steps++ < 32)
		ns = svc_idle_spin_shrink(ns);
	KUNIT_EXPECT_EQ(test, ns, 0U);
	KUNIT_EXPECT_LE(test, steps, 7U);
	KUNIT_EXPECT_EQ(test, svc_idle_spin_shrink(0), 0U);

	/* the budget never exceeds the longest spin */
	KUNIT_EXPECT_EQ(test, svc_idle_spin_budget(50000, 10), max);
	KUNIT_EXPECT_EQ(test, svc_idle_spin_budget(3000, 10), 3000U);
	KUNIT_EXPECT_EQ(test,
			svc_idle_spin_budget(UINT_MAX, SVC_IDLE_SPIN_US_MAX),
			SVC_IDLE_SPIN_US_MAX * (unsigned int)NSEC_PER_USEC);
}

static void svc_idle_spin_off_test(struct kunit *test)
{
	struct svc_rqst *rqstp;

	KUNIT_EXPECT_EQ(test, svc_idle_spin_budget(5000, 0), 0U);

	/* with a longest spin of 0 the spin returns before touching the pool */
	rqstp = kunit_kzalloc(test, sizeof(*rqstp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, rqstp);
	rqstp->rq_spin_ns = 5000;
	KUNIT_EXPECT_FALSE(test, svc_thread_idle_spin(rqstp, 0));
	KUNIT_EXPECT_EQ(test, rqstp->rq_spin_ns, 5000U);

	/* a zero budget also stays out, whatever the longest spin */
	rqstp->rq_spin_ns = 0;
	KUNIT_EXPECT_FALSE(test, svc_thread_idle_spin(rqstp, 10));

	/* the longest spin is capped */
	KUNIT_EXPECT_EQ(test, SVC_IDLE_SPIN_US_MAX, 50U);

	/* the spin is fixed off; there is no parameter */
	KUNIT_EXPECT_EQ(test, SVC_IDLE_SPIN_US, 0U);
	kunit_info(test, "SVC_IDLE_SPIN_US=%u", SVC_IDLE_SPIN_US);
}


/* A spin that finds work, or a claim, is a hit and grows the budget */
static void svc_idle_spin_hit_test(struct kunit *test)
{
	struct svc_pool_test_rig *rig = svc_pool_test_rig(test, 2, 1, NULL);
	struct svc_serv *serv;
	struct svc_rqst *rqstp;

	serv = kunit_kzalloc(test, sizeof(*serv), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, serv);
	serv->sv_pools = rig->pools;
	serv->sv_pool_ext = rig->ext;
	rqstp = svc_pool_test_idle(test, rig, 0);
	rqstp->rq_server = serv;

	/* work on the own pool */
	svc_pool_test_xprts(test, rig, 0, 1);
	rqstp->rq_spin_ns = 1000;
	KUNIT_EXPECT_TRUE(test, svc_thread_idle_spin(rqstp, 10));
	KUNIT_EXPECT_EQ(test, rqstp->rq_spin_ns, 2000U);
	KUNIT_EXPECT_EQ(test, percpu_counter_sum(&rig->ext[0].spin_hits), 1);

	/* claimed for a sibling's work: RQ_WAKING ends the spin */
	lwq_dequeue(&rig->pools[0].sp_xprts, struct svc_xprt, xpt_ready);
	set_bit(RQ_WAKING, &rqstp->rq_flags);
	KUNIT_EXPECT_TRUE(test, svc_thread_idle_spin(rqstp, 1));
	KUNIT_EXPECT_EQ(test, rqstp->rq_spin_ns, 1000U);
	KUNIT_EXPECT_EQ(test, percpu_counter_sum(&rig->ext[0].spin_hits), 2);

	/* nothing: a miss halves the budget */
	clear_bit(RQ_WAKING, &rqstp->rq_flags);
	KUNIT_EXPECT_FALSE(test, svc_thread_idle_spin(rqstp, 10));
	if (!need_resched())
		KUNIT_EXPECT_EQ(test, rqstp->rq_spin_ns, 500U);
	KUNIT_EXPECT_EQ(test, percpu_counter_sum(&rig->ext[0].spin_hits), 2);
}

static struct kunit_case svc_pool_test_cases[] = {
	KUNIT_CASE(svc_pool_map_one_node_test),
	KUNIT_CASE(svc_pool_map_two_node_test),
	KUNIT_CASE(svc_pool_map_hotplug_test),
	KUNIT_CASE(svc_pool_idle_map_test),
	KUNIT_CASE(svc_pool_saturated_no_walk_test),
	KUNIT_CASE(svc_pool_idle_map_wrap_test),
	KUNIT_CASE(svc_pool_sparse_far_test),
	KUNIT_CASE(svc_pool_wake_sibling_test),
	KUNIT_CASE(svc_pool_wake_claimed_test),
	KUNIT_CASE(svc_pool_node_wrap_test),
	KUNIT_CASE(svc_pool_cross_node_test),
	KUNIT_CASE(svc_pool_pernode_no_cross_test),
	KUNIT_CASE(svc_pool_far_window_test),
	KUNIT_CASE(svc_pool_three_node_test),
	KUNIT_CASE(svc_pool_whole_no_far_test),
	KUNIT_CASE(svc_pool_enqueue_single_node_test),
	KUNIT_CASE(svc_pool_no_cross_beyond_window_test),
	KUNIT_CASE(svc_pool_share_test),
	KUNIT_CASE(svc_pool_threadless_test),
	KUNIT_CASE(svc_pool_split_test),
	KUNIT_CASE(svc_wake_up_threadless_test),
	KUNIT_CASE(svc_thread_pass_claim_test),
	KUNIT_CASE(svc_pool_enqueue_sibling_test),
	KUNIT_CASE(svc_pool_steal_test),
	KUNIT_CASE(svc_pool_steal_race_test),
	KUNIT_CASE(svc_idle_spin_arith_test),
	KUNIT_CASE(svc_idle_spin_hit_test),
	KUNIT_CASE(svc_idle_spin_off_test),
	{}
};

static struct kunit_suite svc_pool_test_suite = {
	.name = "sunrpc-svc-pool",
	.test_cases = svc_pool_test_cases,
};

kunit_test_suite(svc_pool_test_suite);

MODULE_DESCRIPTION("KUnit tests for SUNRPC svc thread pools");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_LICENSE("GPL");
