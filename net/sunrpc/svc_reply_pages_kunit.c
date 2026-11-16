// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the recycling of sent TCP replies' pages
 * (svc_reply_pages.c): which replies a descriptor takes, and that its
 * completion hands the pages to the sending CPU's cache, from which a fill
 * takes them, with a full cache freeing them and a still-referenced page let
 * go.  The completion is driven by dropping the sender's reference, as when
 * no skb took the pages.  The tests set svc_tcp_reply_recycle, which is
 * global: they empty the caches before and after each case and put the old
 * value back.
 */

#include <kunit/test.h>
#include <linux/bvec.h>
#include <linux/gfp.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/sunrpc/svc.h>

#include "svc_reply_pages.h"

/* A reply of a head page and 16 payload pages: at least 64 KiB of payload. */
#define RP_PAGES	17

struct rp_test {
	struct svc_rqst		*rqstp;
	struct page		**slots;	/* rq_respages */
	struct page		*orig[RP_PAGES];
	struct bio_vec		bvec[RP_PAGES];
	unsigned int		old;
};

static int rp_init(struct kunit *test)
{
	struct rp_test *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, t);
	t->rqstp = kunit_kzalloc(test, sizeof(*t->rqstp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t->rqstp);
	t->slots = kunit_kcalloc(test, RP_PAGES + 1, sizeof(*t->slots),
				 GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t->slots);
	for (i = 0; i < RP_PAGES; i++) {
		t->slots[i] = alloc_page(GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, t->slots[i]);
		t->orig[i] = t->slots[i];
		bvec_set_page(&t->bvec[i], t->slots[i], PAGE_SIZE, 0);
	}
	t->rqstp->rq_respages = t->slots;
	t->rqstp->rq_next_page = t->slots + RP_PAGES;
	t->rqstp->rq_page_end = t->slots + RP_PAGES;
	t->rqstp->rq_res.page_len = (RP_PAGES - 1) * PAGE_SIZE;
	t->old = svc_reply_kunit_set(0);	/* empty every cache */
	test->priv = t;
	return 0;
}

static void rp_exit(struct kunit *test)
{
	struct rp_test *t = test->priv;
	unsigned int i;

	svc_reply_kunit_set(0);
	for (i = 0; i <= RP_PAGES; i++)
		if (t->slots[i])
			put_page(t->slots[i]);
	svc_reply_kunit_set(t->old);
}

/* Take the reply and complete its send on this CPU; migration disabled. */
static struct svc_reply_tx *rp_take(struct kunit *test, struct rp_test *t)
{
	struct svc_reply_tx *tx;
	unsigned int i;

	tx = svc_reply_tx_take(t->rqstp, t->bvec, RP_PAGES);
	KUNIT_ASSERT_NOT_NULL(test, tx);
	KUNIT_EXPECT_EQ(test, tx->n, RP_PAGES);
	KUNIT_EXPECT_EQ(test, tx->cpu, smp_processor_id());
	for (i = 0; i < RP_PAGES; i++)
		KUNIT_EXPECT_NULL(test, t->slots[i]);
	return tx;
}

/* Recycling off: the reply is sent as before, its pages left in the rqst. */
static void rp_off(struct kunit *test)
{
	struct rp_test *t = test->priv;
	unsigned int i;

	KUNIT_EXPECT_NULL(test, svc_reply_tx_take(t->rqstp, t->bvec,
						  RP_PAGES));
	for (i = 0; i < RP_PAGES; i++)
		KUNIT_EXPECT_PTR_EQ(test, t->slots[i], t->orig[i]);
}

/* A payload under 64 KiB is not recycled. */
static void rp_small(struct kunit *test)
{
	struct rp_test *t = test->priv;

	svc_reply_kunit_set(1024);
	t->rqstp->rq_res.page_len = 64 * 1024 - 1;
	KUNIT_EXPECT_NULL(test, svc_reply_tx_take(t->rqstp, t->bvec,
						  RP_PAGES));
	KUNIT_EXPECT_PTR_EQ(test, t->slots[0], t->orig[0]);
}

/* A reply sent from a page that is not the rqst's own keeps its pages. */
static void rp_foreign(struct kunit *test)
{
	struct rp_test *t = test->priv;
	struct page *other = alloc_page(GFP_KERNEL);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, other);
	svc_reply_kunit_set(1024);
	bvec_set_page(&t->bvec[5], other, PAGE_SIZE, 0);
	KUNIT_EXPECT_NULL(test, svc_reply_tx_take(t->rqstp, t->bvec,
						  RP_PAGES));
	for (i = 0; i < RP_PAGES; i++)
		KUNIT_EXPECT_PTR_EQ(test, t->slots[i], t->orig[i]);
	put_page(other);
}

/* A completed send's pages fill the next rqst on the same CPU. */
static void rp_reuse(struct kunit *test)
{
	struct rp_test *t = test->priv;
	struct svc_reply_kunit_stats s0, s1;
	struct svc_reply_tx *tx;
	unsigned int i, j, found;
	int cpu;

	svc_reply_kunit_set(1024);
	migrate_disable();
	cpu = smp_processor_id();
	svc_reply_kunit_stats(&s0);
	tx = rp_take(test, t);
	net_zcopy_put(&tx->ubuf);	/* the last reference: completes */
	KUNIT_EXPECT_EQ(test, svc_reply_kunit_cached(cpu), RP_PAGES);
	svc_reply_pages_fill(t->slots, RP_PAGES);
	KUNIT_EXPECT_EQ(test, svc_reply_kunit_cached(cpu), 0);
	svc_reply_kunit_stats(&s1);
	migrate_enable();

	KUNIT_EXPECT_EQ(test, s1.sends - s0.sends, 1);
	KUNIT_EXPECT_EQ(test, s1.reused - s0.reused, RP_PAGES);
	KUNIT_EXPECT_EQ(test, s1.busy - s0.busy, 0);
	KUNIT_EXPECT_EQ(test, s1.live - s0.live, 0);
	/* The same pages, in some order. */
	for (i = 0, found = 0; i < RP_PAGES; i++)
		for (j = 0; j < RP_PAGES; j++)
			found += t->slots[i] == t->orig[j];
	KUNIT_EXPECT_EQ(test, found, RP_PAGES);
}

/* A page still referenced elsewhere is let go, not reused. */
static void rp_busy(struct kunit *test)
{
	struct rp_test *t = test->priv;
	struct svc_reply_kunit_stats s0, s1;
	struct page *held = t->orig[3];
	struct svc_reply_tx *tx;
	unsigned int i, empty = 0;

	svc_reply_kunit_set(1024);
	migrate_disable();
	svc_reply_kunit_stats(&s0);
	tx = rp_take(test, t);
	get_page(held);
	net_zcopy_put(&tx->ubuf);
	svc_reply_pages_fill(t->slots, RP_PAGES);
	svc_reply_kunit_stats(&s1);
	migrate_enable();

	KUNIT_EXPECT_EQ(test, s1.busy - s0.busy, 1);
	KUNIT_EXPECT_EQ(test, s1.reused - s0.reused, RP_PAGES - 1);
	for (i = 0; i < RP_PAGES; i++) {
		empty += !t->slots[i];
		KUNIT_EXPECT_PTR_NE(test, t->slots[i], held);
	}
	KUNIT_EXPECT_EQ(test, empty, 1);
	KUNIT_EXPECT_EQ(test, page_ref_count(held), 1);
	put_page(held);
}

/* A cache with no room frees a completed send's pages. */
static void rp_full(struct kunit *test)
{
	struct rp_test *t = test->priv;
	struct svc_reply_kunit_stats s0, s1;
	struct svc_reply_tx *tx;
	int cpu;

	svc_reply_kunit_set(RP_PAGES - 1);
	migrate_disable();
	cpu = smp_processor_id();
	svc_reply_kunit_stats(&s0);
	tx = rp_take(test, t);
	net_zcopy_put(&tx->ubuf);
	KUNIT_EXPECT_EQ(test, svc_reply_kunit_cached(cpu), 0);
	svc_reply_kunit_stats(&s1);
	migrate_enable();

	KUNIT_EXPECT_EQ(test, s1.over - s0.over, RP_PAGES);
	KUNIT_EXPECT_EQ(test, s1.live - s0.live, 0);
}

/* Setting the limit to 0 frees what the caches keep. */
static void rp_trim(struct kunit *test)
{
	struct rp_test *t = test->priv;
	struct svc_reply_tx *tx;
	int cpu;

	svc_reply_kunit_set(1024);
	migrate_disable();
	cpu = smp_processor_id();
	tx = rp_take(test, t);
	net_zcopy_put(&tx->ubuf);
	KUNIT_EXPECT_EQ(test, svc_reply_kunit_cached(cpu), RP_PAGES);
	svc_reply_kunit_set(0);
	KUNIT_EXPECT_EQ(test, svc_reply_kunit_cached(cpu), 0);
	svc_reply_pages_fill(t->slots, RP_PAGES);
	migrate_enable();
	KUNIT_EXPECT_NULL(test, t->slots[0]);
}

static struct kunit_case rp_cases[] = {
	KUNIT_CASE(rp_off),
	KUNIT_CASE(rp_small),
	KUNIT_CASE(rp_foreign),
	KUNIT_CASE(rp_reuse),
	KUNIT_CASE(rp_busy),
	KUNIT_CASE(rp_full),
	KUNIT_CASE(rp_trim),
	{}
};

static struct kunit_suite rp_suite = {
	.name		= "sunrpc-reply-pages",
	.init		= rp_init,
	.exit		= rp_exit,
	.test_cases	= rp_cases,
};
kunit_test_suite(rp_suite);

MODULE_DESCRIPTION("KUnit tests for the recycling of sent SUNRPC TCP replies' pages");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
