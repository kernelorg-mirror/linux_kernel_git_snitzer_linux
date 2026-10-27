// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the optimized LOCALIO completion CPU choice.
 *
 * A read or write completion is queued to a CPU of the completing CPU's
 * node that is online and a workqueue and domain housekeeping CPU.  The
 * completing CPU is usually the submitter's, so it is taken only when it
 * is the node's only such CPU.
 */
#include <kunit/test.h>
#include <linux/cpumask.h>
#include <linux/module.h>

#include "localio_cpu.h"

struct localio_cpu_masks {
	struct cpumask *node;
	struct cpumask *online;
	struct cpumask *hk_wq;
	struct cpumask *hk_domain;
};

static struct cpumask *localio_cpu_mask(struct kunit *test, unsigned int nr)
{
	struct cpumask *mask = kunit_kzalloc(test, cpumask_size(), GFP_KERNEL);
	unsigned int cpu;

	KUNIT_ASSERT_NOT_NULL(test, mask);
	for (cpu = 0; cpu < nr; cpu++)
		cpumask_set_cpu(cpu, mask);
	return mask;
}

/* A four-CPU node, every CPU eligible. */
static void localio_cpu_init(struct kunit *test, struct localio_cpu_masks *m)
{
	if (nr_cpu_ids < 4)
		kunit_skip(test, "needs four possible CPUs");
	m->node = localio_cpu_mask(test, 4);
	m->online = localio_cpu_mask(test, 4);
	m->hk_wq = localio_cpu_mask(test, 4);
	m->hk_domain = localio_cpu_mask(test, 4);
}

static unsigned int localio_cpu_pick(struct localio_cpu_masks *m,
				     unsigned int origin)
{
	return nfs_local_completion_cpu(origin, m->node, m->online, m->hk_wq,
					m->hk_domain);
}

/* The next CPU of the node, not the submitter's. */
static void localio_cpu_next_test(struct kunit *test)
{
	struct localio_cpu_masks m;

	localio_cpu_init(test, &m);
	KUNIT_EXPECT_EQ(test, localio_cpu_pick(&m, 0), 1);
	KUNIT_EXPECT_EQ(test, localio_cpu_pick(&m, 2), 3);
}

/* The scan wraps to the start of the node. */
static void localio_cpu_wrap_test(struct kunit *test)
{
	struct localio_cpu_masks m;

	localio_cpu_init(test, &m);
	KUNIT_EXPECT_EQ(test, localio_cpu_pick(&m, 3), 0);
	cpumask_clear_cpu(0, m.node);
	KUNIT_EXPECT_EQ(test, localio_cpu_pick(&m, 3), 1);
}

/* An offline or non-housekeeping CPU is skipped. */
static void localio_cpu_ineligible_test(struct kunit *test)
{
	struct localio_cpu_masks m;

	localio_cpu_init(test, &m);
	cpumask_clear_cpu(1, m.online);
	KUNIT_EXPECT_EQ(test, localio_cpu_pick(&m, 0), 2);
	cpumask_clear_cpu(2, m.hk_wq);
	KUNIT_EXPECT_EQ(test, localio_cpu_pick(&m, 0), 3);
	cpumask_clear_cpu(3, m.hk_domain);
	KUNIT_EXPECT_EQ(test, localio_cpu_pick(&m, 0), 0);
}

/* A CPU outside the node is never chosen. */
static void localio_cpu_node_test(struct kunit *test)
{
	struct localio_cpu_masks m;

	localio_cpu_init(test, &m);
	cpumask_clear(m.node);
	cpumask_set_cpu(1, m.node);
	cpumask_set_cpu(3, m.node);
	KUNIT_EXPECT_EQ(test, localio_cpu_pick(&m, 1), 3);
	KUNIT_EXPECT_EQ(test, localio_cpu_pick(&m, 3), 1);
}

/* A node whose only eligible CPU is the origin keeps the origin. */
static void localio_cpu_origin_only_test(struct kunit *test)
{
	struct localio_cpu_masks m;

	localio_cpu_init(test, &m);
	cpumask_clear(m.node);
	cpumask_set_cpu(2, m.node);
	KUNIT_EXPECT_EQ(test, localio_cpu_pick(&m, 2), 2);

	localio_cpu_init(test, &m);
	cpumask_clear_cpu(0, m.hk_wq);
	cpumask_clear_cpu(1, m.online);
	cpumask_clear_cpu(3, m.hk_domain);
	KUNIT_EXPECT_EQ(test, localio_cpu_pick(&m, 2), 2);
}

/* With no eligible CPU the caller falls back to nfsiod. */
static void localio_cpu_none_test(struct kunit *test)
{
	struct localio_cpu_masks m;

	localio_cpu_init(test, &m);
	cpumask_clear(m.hk_wq);
	KUNIT_EXPECT_EQ(test, localio_cpu_pick(&m, 1), nr_cpu_ids);
	cpumask_setall(m.hk_wq);
	cpumask_clear(m.node);
	KUNIT_EXPECT_EQ(test, localio_cpu_pick(&m, 1), nr_cpu_ids);
}

/* The last CPU id wraps the start to CPU 0. */
static void localio_cpu_last_id_test(struct kunit *test)
{
	struct localio_cpu_masks m;
	unsigned int last;

	localio_cpu_init(test, &m);
	last = nr_cpu_ids - 1;
	cpumask_set_cpu(last, m.node);
	cpumask_set_cpu(last, m.online);
	cpumask_set_cpu(last, m.hk_wq);
	cpumask_set_cpu(last, m.hk_domain);
	KUNIT_EXPECT_EQ(test, localio_cpu_pick(&m, last), 0);
}

static struct kunit_case localio_cpu_test_cases[] = {
	KUNIT_CASE(localio_cpu_next_test),
	KUNIT_CASE(localio_cpu_wrap_test),
	KUNIT_CASE(localio_cpu_ineligible_test),
	KUNIT_CASE(localio_cpu_node_test),
	KUNIT_CASE(localio_cpu_origin_only_test),
	KUNIT_CASE(localio_cpu_none_test),
	KUNIT_CASE(localio_cpu_last_id_test),
	{}
};

static struct kunit_suite localio_cpu_test_suite = {
	.name = "nfs-localio-completion-cpu",
	.test_cases = localio_cpu_test_cases,
};

kunit_test_suite(localio_cpu_test_suite);

MODULE_DESCRIPTION("KUnit tests for the LOCALIO completion CPU choice");
MODULE_LICENSE("GPL");
