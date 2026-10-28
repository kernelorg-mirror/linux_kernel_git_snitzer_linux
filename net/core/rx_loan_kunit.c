// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the receive page-loan helper (net/core/rx_loan.c).
 * The consumers' own suites (net/sunrpc/svcsock_kunit.c) test it as they
 * drive it; these need no consumer.
 */

#include <kunit/test.h>
#include <linux/completion.h>
#include <linux/gfp.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/skbuff.h>
#include <linux/workqueue.h>
#include <net/page_pool/cfg.h>
#include <net/page_pool/helpers.h>
#include <net/rx_loan.h>

static void rx_loan_kunit_table_free(void *data)
{
	rx_loan_table_free(data);
}

static struct rx_loan_table *rx_loan_kunit_table(struct kunit *test,
						 unsigned long entries)
{
	struct rx_loan_table *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, t);
	KUNIT_ASSERT_EQ(test, rx_loan_table_alloc(t, entries, GFP_KERNEL,
						  NUMA_NO_NODE), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			rx_loan_kunit_table_free, t), 0);
	return t;
}

static void rx_loan_kunit_page_free(void *data)
{
	__free_page(data);
}

static struct page *rx_loan_kunit_page(struct kunit *test)
{
	struct page *page = alloc_page(GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, page);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			rx_loan_kunit_page_free, page), 0);
	return page;
}

/* A table takes 1 to U32_MAX entries and starts empty. */
static void rx_loan_table_alloc_test(struct kunit *test)
{
	struct rx_loan_table bad = {};
	struct rx_loan_table *t = rx_loan_kunit_table(test, 3);

	KUNIT_EXPECT_EQ(test, rx_loan_table_alloc(&bad, 0, GFP_KERNEL,
						  NUMA_NO_NODE), -EINVAL);
	KUNIT_EXPECT_NULL(test, bad.bvec);
	KUNIT_EXPECT_EQ(test, t->capacity, 3U);
	KUNIT_EXPECT_EQ(test, t->count, 0U);
	KUNIT_EXPECT_TRUE(test, rx_loan_table_counts_valid(t));
	KUNIT_EXPECT_TRUE(test, rx_loan_table_refs_valid(t));
}

/*
 * Bytes that continue the last entry's page coalesce into it; others take
 * an entry until the table is full.  Invalid ranges take nothing.
 */
static void rx_loan_table_append_test(struct kunit *test)
{
	struct rx_loan_table *t = rx_loan_kunit_table(test, 2);
	struct page *a = rx_loan_kunit_page(test);
	struct page *b = rx_loan_kunit_page(test);
	struct page *c = rx_loan_kunit_page(test);

	KUNIT_EXPECT_EQ(test, rx_loan_table_append(t, NULL, 0, 8, false,
						   false), -EINVAL);
	KUNIT_EXPECT_EQ(test, rx_loan_table_append(t, a, 0, 0, false, false),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, rx_loan_table_append(t, a, PAGE_SIZE, 1, false,
						   false), -EINVAL);
	KUNIT_EXPECT_EQ(test, rx_loan_table_append(t, a, 8, PAGE_SIZE, false,
						   false), -EINVAL);
	/* A page-pool entry is always borrowed. */
	KUNIT_EXPECT_EQ(test, rx_loan_table_append(t, a, 0, 8, false, true),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, t->count, 0U);

	KUNIT_ASSERT_EQ(test, rx_loan_table_append(t, a, 0, 100, false, false),
			0);
	KUNIT_ASSERT_EQ(test, rx_loan_table_append(t, a, 100, 28, false,
						   false), 0);
	KUNIT_EXPECT_EQ(test, t->count, 1U);
	KUNIT_EXPECT_EQ(test, t->bvec[0].bv_len, 128U);
	/* Not contiguous with the last entry: a new one. */
	KUNIT_ASSERT_EQ(test, rx_loan_table_append(t, b, 0, 64, false, false),
			0);
	KUNIT_EXPECT_EQ(test, rx_loan_table_append(t, c, 0, 64, false, false),
			-ENOSPC);
	KUNIT_EXPECT_EQ(test, t->count, 2U);
	KUNIT_EXPECT_EQ(test, t->body_bytes, 192U);
	KUNIT_EXPECT_EQ(test, t->copied_bytes, 192U);
	KUNIT_EXPECT_EQ(test, t->borrowed_bytes, 0U);
	KUNIT_EXPECT_EQ(test, t->borrowed_count, 0U);
	KUNIT_EXPECT_TRUE(test, rx_loan_table_refs_valid(t));
}

/*
 * A borrowed entry holds a page reference until the table is released;
 * a copied one holds none.  Reset then clears the counts and keeps the
 * arrays for the next record.
 */
static void rx_loan_table_release_test(struct kunit *test)
{
	struct rx_loan_table *t = rx_loan_kunit_table(test, 4);
	struct page *a = rx_loan_kunit_page(test);
	struct page *b = rx_loan_kunit_page(test);
	struct bio_vec *bvec = t->bvec;
	int a_refs = page_ref_count(a);
	int b_refs = page_ref_count(b);

	KUNIT_ASSERT_EQ(test, rx_loan_table_append(t, a, 0, 64, true, false),
			0);
	KUNIT_ASSERT_EQ(test, rx_loan_table_append(t, b, 0, 64, false, false),
			0);
	/* Borrowed and copied bytes of one page do not coalesce. */
	KUNIT_ASSERT_EQ(test, rx_loan_table_append(t, a, 64, 64, false, false),
			0);
	KUNIT_EXPECT_EQ(test, t->count, 3U);
	KUNIT_EXPECT_EQ(test, page_ref_count(a), a_refs + 1);
	KUNIT_EXPECT_EQ(test, page_ref_count(b), b_refs);
	KUNIT_EXPECT_EQ(test, t->borrowed_count, 1U);
	KUNIT_EXPECT_EQ(test, t->borrowed_bytes, 64U);
	KUNIT_EXPECT_EQ(test, t->copied_bytes, 128U);
	/* Not a page pool's page, not a region page. */
	KUNIT_EXPECT_EQ(test, t->region_count, 0U);
	KUNIT_ASSERT_TRUE(test, rx_loan_table_refs_valid(t));

	KUNIT_EXPECT_EQ(test, rx_loan_table_release(t), 0);
	KUNIT_EXPECT_EQ(test, page_ref_count(a), a_refs);
	KUNIT_EXPECT_EQ(test, t->borrowed_count, 0U);
	KUNIT_EXPECT_EQ(test, t->refs_released, t->refs_acquired);
	KUNIT_EXPECT_EQ(test, t->body_bytes, 192U);

	rx_loan_table_reset(t);
	KUNIT_EXPECT_PTR_EQ(test, t->bvec, bvec);
	KUNIT_EXPECT_EQ(test, t->capacity, 4U);
	KUNIT_EXPECT_EQ(test, t->count, 0U);
	KUNIT_EXPECT_EQ(test, t->body_bytes, 0U);
	KUNIT_EXPECT_EQ(test, t->refs_acquired, 0U);
}

/* Returning one entry drops its reference and keeps its bytes counted. */
static void rx_loan_table_return_entry_test(struct kunit *test)
{
	struct rx_loan_table *t = rx_loan_kunit_table(test, 2);
	struct page *a = rx_loan_kunit_page(test);
	int a_refs = page_ref_count(a);

	KUNIT_ASSERT_EQ(test, rx_loan_table_append(t, a, 0, 64, true, false),
			0);
	rx_loan_table_return_entry(t, 0);
	KUNIT_EXPECT_EQ(test, page_ref_count(a), a_refs);
	KUNIT_EXPECT_EQ(test, t->borrowed_count, 0U);
	KUNIT_EXPECT_EQ(test, t->refs_released, 1U);
	KUNIT_EXPECT_EQ(test, t->borrowed_bytes, 64U);
	KUNIT_EXPECT_FALSE(test, test_bit(0, t->borrowed));
}

/* Pages from no pool bound to the region count nothing. */
static void rx_loan_skb_region_pages_test(struct kunit *test)
{
	struct sk_buff *skb = alloc_skb(256, GFP_KERNEL);
	struct page *prev = NULL;

	KUNIT_ASSERT_NOT_NULL(test, skb);
	skb_put_zero(skb, 200);
	KUNIT_EXPECT_EQ(test, rx_loan_skb_region_pages(skb, 0, 0, skb->len,
						       &prev), 0U);
	KUNIT_EXPECT_EQ(test, rx_loan_skb_region_pages(skb, 0, 300, 400,
						       &prev), 0U);
	rx_loan_prefetch_pages(skb, 0, 0, skb->len);
	kfree_skb(skb);
}

struct rx_loan_kunit_waiter {
	struct rx_loan_waiter	w;
	struct rx_loan_credit	cr;
	int			pinned;
	int			woken;
	int			order;
};

static int rx_loan_kunit_seq;

static bool rx_loan_kunit_pin(struct rx_loan_waiter *w)
{
	container_of(w, struct rx_loan_kunit_waiter, w)->pinned++;
	return true;
}

static void rx_loan_kunit_wake(struct rx_loan_waiter *w)
{
	struct rx_loan_kunit_waiter *kw =
		container_of(w, struct rx_loan_kunit_waiter, w);

	kw->woken++;
	kw->order = ++rx_loan_kunit_seq;
}

static const struct rx_loan_waiter_ops rx_loan_kunit_waiter_ops = {
	.pin	= rx_loan_kunit_pin,
	.wake	= rx_loan_kunit_wake,
};

static void rx_loan_kunit_budget_free(void *data)
{
	struct rx_loan_budget *lb = data;

	flush_work(&lb->kick_work);
	free_percpu(lb->stock);
}

/* A budget of @budget pages, @reserve of them reserve, below stocking. */
static struct rx_loan_budget *rx_loan_kunit_budget(struct kunit *test,
						   unsigned int budget,
						   unsigned int reserve)
{
	struct rx_loan_budget *lb = kunit_kzalloc(test, sizeof(*lb),
						  GFP_KERNEL);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, lb);
	lb->stock = alloc_percpu(struct rx_loan_stock);
	KUNIT_ASSERT_TRUE(test, lb->stock != NULL);
	ret = kunit_add_action_or_reset(test, rx_loan_kunit_budget_free, lb);
	KUNIT_ASSERT_EQ(test, ret, 0);
	spin_lock_init(&lb->lock);
	lb->next_ticket = 1;
	INIT_LIST_HEAD(&lb->inflight);
	INIT_LIST_HEAD(&lb->waiters);
	INIT_WORK(&lb->kick_work, rx_loan_kick_fn);
	rx_loan_budget_request(lb, budget, reserve, false);
	spin_lock_bh(&lb->lock);
	lb->contended = false;
	spin_unlock_bh(&lb->lock);
	return lb;
}

static void rx_loan_kunit_waiter_init(struct rx_loan_kunit_waiter *kw,
				      bool exact, u32 need)
{
	memset(kw, 0, sizeof(*kw));
	INIT_LIST_HEAD(&kw->w.node);
	kw->w.ops = &rx_loan_kunit_waiter_ops;
	kw->w.exact = exact;
	kw->w.need = need;
}

/* avail with every stock drained back to it */
static long rx_loan_kunit_avail(struct rx_loan_budget *lb)
{
	long avail;

	spin_lock_bh(&lb->lock);
	rx_loan_stock_drain_locked(lb);
	avail = lb->avail;
	spin_unlock_bh(&lb->lock);
	return avail;
}

static void rx_loan_kunit_return(struct rx_loan_kunit_waiter *kw)
{
	struct rx_loan_lend out = {};

	rx_loan_return(&kw->cr, 0, 0, &out);
	KUNIT_EXPECT_FALSE(current->kunit_test, kw->cr.linked);
}

static void rx_loan_kunit_region_reset(void *data)
{
	rx_loan_kunit_region(0, 0);
}

/*
 * Stand in for a region of @pages pages, @read_pages of them for reads,
 * until the test ends, failed or not: a stand-in left in place would size
 * the next test's region, and a running server's.  It starts with nothing
 * out (rx_loan_kunit_region()).
 */
static void rx_loan_kunit_standin(struct kunit *test, unsigned long pages,
				  unsigned long read_pages)
{
	int ret = kunit_add_action_or_reset(test, rx_loan_kunit_region_reset,
					    NULL);

	KUNIT_ASSERT_EQ(test, ret, 0);
	rx_loan_kunit_region(pages, read_pages);
}

static void rx_loan_kunit_consumer_stop(void *data)
{
	struct rx_loan_consumer *c = data;

	if (READ_ONCE(c->registered))
		rx_loan_consumer_unregister(c);
}

/*
 * A consumer registered until the test ends, failed or not, if the test
 * does not unregister it first: the consumers' list is global, a running
 * server's consumer is on it, and a record left on it outlives the test.
 */
static struct rx_loan_consumer *rx_loan_kunit_consumer(struct kunit *test,
							const char *name)
{
	struct rx_loan_consumer *c = kunit_kzalloc(test, sizeof(*c),
						   GFP_KERNEL);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, c);
	KUNIT_ASSERT_EQ(test, rx_loan_consumer_register(c, NULL, name, 0), 0);
	ret = kunit_add_action_or_reset(test, rx_loan_kunit_consumer_stop, c);
	KUNIT_ASSERT_EQ(test, ret, 0);
	return c;
}

#define RX_LOAN_KUNIT_PROBE	(1UL << 20)	/* pages: a share each */

/*
 * How many consumers are registered besides the test's own, a running
 * server's for one: every share is the region over all of them.  With the
 * test's stand-in of @pages in place, before it registers its own.
 */
static unsigned long rx_loan_kunit_others(struct kunit *test,
					  unsigned long pages)
{
	struct rx_loan_consumer *p = rx_loan_kunit_consumer(test, "probe");
	unsigned long share;

	rx_loan_kunit_region(RX_LOAN_KUNIT_PROBE, 0);
	share = rx_loan_consumer_share(p);
	rx_loan_consumer_unregister(p);
	rx_loan_kunit_region(pages, 0);
	KUNIT_ASSERT_GT(test, share, 0UL);
	return RX_LOAN_KUNIT_PROBE / share - 1;
}

static void rx_loan_kunit_net_exit(void *data)
{
	rx_loan_net_state_exit(data);
}

/*
 * A stand-in namespace's budgets, on the region's list of budgets until
 * the test ends, failed or not.
 */
static struct rx_loan_net *rx_loan_kunit_net(struct kunit *test)
{
	struct rx_loan_net *ln = kunit_kzalloc(test, sizeof(*ln), GFP_KERNEL);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, ln);
	KUNIT_ASSERT_EQ(test, rx_loan_net_state_init(ln), 0);
	ret = kunit_add_action_or_reset(test, rx_loan_kunit_net_exit, ln);
	KUNIT_ASSERT_EQ(test, ret, 0);
	return ln;
}

/*
 * One FIFO: an exact request parked behind a refused transport is granted
 * only after that transport is admitted, then woken in ticket order.
 */
static void rx_loan_fifo_test(struct kunit *test)
{
	struct rx_loan_budget *lb = rx_loan_kunit_budget(test, 16, 0);
	struct rx_loan_kunit_waiter a, b, p;

	rx_loan_kunit_waiter_init(&a, false, 0);
	rx_loan_kunit_waiter_init(&b, false, 0);
	rx_loan_kunit_waiter_init(&p, true, 1);
	KUNIT_ASSERT_TRUE(test, rx_loan_admit(lb, NULL, &a.w, &a.cr, 12, false));
	KUNIT_ASSERT_FALSE(test, rx_loan_admit(lb, NULL, &b.w, &b.cr, 8, false));
	KUNIT_EXPECT_TRUE(test, rx_loan_waiting(&b.w));
	KUNIT_EXPECT_EQ(test, rx_loan_grant(lb, NULL, &p.w), -EAGAIN);
	rx_loan_park(lb, NULL, &p.w);
	KUNIT_EXPECT_EQ(test, lb->read_parked, 1ULL);
	KUNIT_EXPECT_GT(test, p.w.ticket, b.w.ticket);
	KUNIT_EXPECT_EQ(test, b.woken + p.woken, 0);

	/* A return wakes the oldest, the transport; the READ stays behind. */
	rx_loan_kunit_return(&a);
	KUNIT_EXPECT_EQ(test, b.woken, 1);
	KUNIT_EXPECT_EQ(test, p.woken, 0);

	/* The transport's admission passes the head on to the READ. */
	KUNIT_ASSERT_TRUE(test, rx_loan_admit(lb, NULL, &b.w, &b.cr, 8, false));
	KUNIT_EXPECT_FALSE(test, rx_loan_waiting(&b.w));
	KUNIT_EXPECT_EQ(test, p.pinned, 1);
	KUNIT_EXPECT_EQ(test, p.woken, 1);
	KUNIT_EXPECT_EQ(test, p.w.credit, 1U);
	KUNIT_EXPECT_LT(test, b.order, p.order);
	KUNIT_EXPECT_EQ(test, lb->read_granted, 1ULL);
	KUNIT_EXPECT_TRUE(test, list_empty(&lb->waiters));

	rx_loan_grant_return(lb, NULL, p.w.credit, p.w.reserve);
	rx_loan_kunit_return(&b);
	KUNIT_EXPECT_EQ(test, rx_loan_kunit_avail(lb), 16L);
	KUNIT_EXPECT_EQ(test, lb->inflight_count, 0U);
}

/* Only the oldest record draws the reserve, then overruns. */
static void rx_loan_reserve_oldest_test(struct kunit *test)
{
	struct rx_loan_budget *lb = rx_loan_kunit_budget(test, 16, 4);
	struct rx_loan_kunit_waiter a, b;

	rx_loan_kunit_waiter_init(&a, false, 0);
	rx_loan_kunit_waiter_init(&b, false, 0);
	KUNIT_ASSERT_TRUE(test, rx_loan_admit(lb, NULL, &a.w, &a.cr, 14, false));
	KUNIT_EXPECT_EQ(test, a.cr.credit, 14U);
	KUNIT_EXPECT_EQ(test, a.cr.reserve, 2U);
	KUNIT_EXPECT_EQ(test, lb->reserve, 2L);
	KUNIT_ASSERT_FALSE(test, rx_loan_admit(lb, NULL, &b.w, &b.cr, 1, false));

	/* The oldest grows from the reserve, then past the budget. */
	KUNIT_EXPECT_TRUE(test, rx_loan_charge(&a.w, &a.cr, 16));
	KUNIT_EXPECT_EQ(test, a.cr.reserve, 4U);
	KUNIT_EXPECT_TRUE(test, rx_loan_charge(&a.w, &a.cr, 18));
	KUNIT_EXPECT_EQ(test, a.cr.over, 2U);
	KUNIT_EXPECT_EQ(test, lb->overrun, 1ULL);

	rx_loan_kunit_return(&a);
	KUNIT_EXPECT_EQ(test, b.woken, 1);
	KUNIT_EXPECT_EQ(test, lb->reserve, 4L);
	KUNIT_ASSERT_TRUE(test, rx_loan_admit(lb, NULL, &b.w, &b.cr, 1, false));
	KUNIT_EXPECT_EQ(test, b.cr.reserve, 0U);
	rx_loan_kunit_return(&b);
	KUNIT_EXPECT_EQ(test, rx_loan_kunit_avail(lb), 12L);
}

/* While a record is in flight, a waiter is woken only when avail covers it. */
static void rx_loan_wake_need_test(struct kunit *test)
{
	struct rx_loan_budget *lb = rx_loan_kunit_budget(test, 16, 0);
	struct rx_loan_kunit_waiter a, b, c;

	rx_loan_kunit_waiter_init(&a, false, 0);
	rx_loan_kunit_waiter_init(&b, false, 0);
	rx_loan_kunit_waiter_init(&c, false, 0);
	KUNIT_ASSERT_TRUE(test, rx_loan_admit(lb, NULL, &a.w, &a.cr, 10, false));
	KUNIT_ASSERT_TRUE(test, rx_loan_admit(lb, NULL, &c.w, &c.cr, 4, false));
	KUNIT_ASSERT_FALSE(test, rx_loan_admit(lb, NULL, &b.w, &b.cr, 7, false));
	KUNIT_EXPECT_EQ(test, b.w.need, 7U);

	rx_loan_kunit_return(&c);	/* avail 6 < 7 */
	KUNIT_EXPECT_EQ(test, b.pinned + b.woken, 0);
	rx_loan_kunit_return(&a);	/* avail 16 */
	KUNIT_EXPECT_EQ(test, b.woken, 1);

	rx_loan_unwait(&b.w);
	KUNIT_EXPECT_FALSE(test, rx_loan_waiting(&b.w));
	KUNIT_EXPECT_NULL(test, b.w.budget);
	KUNIT_EXPECT_EQ(test, rx_loan_kunit_avail(lb), 16L);
}

/*
 * Only credit charged while a region was in place can hold its pages: a
 * record charged before the region is carved and grown after it keeps only
 * the growth spent when it is abandoned.  The rest goes back to its
 * consumer's share and to the region's total, here below the stand-in's
 * nothing: it was charged before it.
 */
static void rx_loan_orphan_carved_test(struct kunit *test)
{
	long out = rx_loan_region_out(), orphaned = rx_loan_region_orphaned();
	struct rx_loan_consumer *c;
	struct rx_loan_budget *lb;
	struct rx_loan_kunit_waiter a;
	long base;

	if (page_pool_rx_region_exists() || rx_loan_region_size())
		kunit_skip(test, "a region or a region size already exists");
	lb = rx_loan_kunit_budget(test, 256, 0);
	c = rx_loan_kunit_consumer(test, "c");
	rx_loan_kunit_waiter_init(&a, false, 0);
	KUNIT_ASSERT_TRUE(test, rx_loan_admit(lb, c, &a.w, &a.cr, 12, false));
	KUNIT_EXPECT_EQ(test, a.cr.region, 0U);
	KUNIT_EXPECT_EQ(test, rx_loan_region_out(), out + 12);

	rx_loan_kunit_standin(test, 1024, 0);
	base = rx_loan_region_out();
	KUNIT_EXPECT_TRUE(test, rx_loan_charge(&a.w, &a.cr, 20));
	KUNIT_EXPECT_EQ(test, a.cr.credit, 20U);
	KUNIT_EXPECT_EQ(test, a.cr.region, 8U);
	KUNIT_EXPECT_EQ(test, rx_loan_region_out(), base + 8);
	rx_loan_orphan(&a.cr, U32_MAX);
	KUNIT_EXPECT_FALSE(test, a.cr.linked);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&c->out_write), 8L);
	KUNIT_EXPECT_EQ(test, rx_loan_region_orphaned(), 8L);
	KUNIT_EXPECT_EQ(test, rx_loan_region_out(), base - 4);
	KUNIT_EXPECT_EQ(test, lb->orphaned, 20ULL);
	rx_loan_consumer_unregister(c);
	rx_loan_kunit_region(0, 0);
	KUNIT_EXPECT_EQ(test, rx_loan_region_out(), out);
	KUNIT_EXPECT_EQ(test, rx_loan_region_orphaned(), orphaned);
}

/*
 * A region configured but not yet carved, or whose carve failed, has a size
 * that caps charges but no pages: a record charged then holds none of its
 * pages, so an orphan gives all its credit back to its consumer's share and
 * to the region's total.
 */
static void rx_loan_orphan_uncarved_test(struct kunit *test)
{
	long out = rx_loan_region_out(), orphaned = rx_loan_region_orphaned();
	unsigned int pages, read_pages;
	struct rx_loan_consumer *c;
	struct rx_loan_kunit_waiter a;
	struct rx_loan_budget *lb;
	bool failed;
	int i;

	if (page_pool_rx_region_exists() || rx_loan_region_size())
		kunit_skip(test, "a region or a region size already exists");
	page_pool_rx_region_kunit_params(&pages, &read_pages, &failed);
	lb = rx_loan_kunit_budget(test, 256, 0);
	c = rx_loan_kunit_consumer(test, "c");
	for (i = 0; i < 2; i++) {
		/* Configured, then configured with its carve failed. */
		page_pool_rx_region_kunit_config(1024, 0, i);
		KUNIT_EXPECT_NE(test, rx_loan_region_size(), 0UL);
		rx_loan_kunit_waiter_init(&a, false, 0);
		KUNIT_ASSERT_TRUE(test, rx_loan_admit(lb, c, &a.w, &a.cr, 12,
						      false));
		KUNIT_EXPECT_TRUE(test, rx_loan_charge(&a.w, &a.cr, 20));
		KUNIT_EXPECT_EQ(test, a.cr.credit, 20U);
		KUNIT_EXPECT_EQ(test, a.cr.region, 0U);
		KUNIT_EXPECT_EQ(test, rx_loan_region_out(), out + 20);
		rx_loan_orphan(&a.cr, U32_MAX);
		KUNIT_EXPECT_FALSE(test, a.cr.linked);
		KUNIT_EXPECT_EQ(test, atomic_long_read(&c->out_write), 0L);
		KUNIT_EXPECT_EQ(test, rx_loan_region_out(), out);
		KUNIT_EXPECT_EQ(test, rx_loan_region_orphaned(), orphaned);
	}
	KUNIT_EXPECT_FALSE(test, page_pool_rx_region_exists());
	KUNIT_EXPECT_EQ(test, lb->orphaned, 40ULL);
	rx_loan_consumer_unregister(c);
	page_pool_rx_region_kunit_config(pages, read_pages, failed);
}

/* An orphan stays spent but leaves the in-flight order. */
static void rx_loan_orphan_test(struct kunit *test)
{
	struct rx_loan_budget *lb = rx_loan_kunit_budget(test, 16, 4);
	struct rx_loan_kunit_waiter a, b;

	rx_loan_kunit_waiter_init(&a, false, 0);
	rx_loan_kunit_waiter_init(&b, false, 0);
	KUNIT_ASSERT_TRUE(test, rx_loan_admit(lb, NULL, &a.w, &a.cr, 12, false));
	KUNIT_ASSERT_FALSE(test, rx_loan_admit(lb, NULL, &b.w, &b.cr, 1, false));
	rx_loan_orphan(&a.cr, U32_MAX);
	KUNIT_EXPECT_FALSE(test, a.cr.linked);
	KUNIT_EXPECT_EQ(test, lb->orphaned, 12ULL);
	KUNIT_EXPECT_EQ(test, lb->inflight_count, 0U);
	KUNIT_EXPECT_EQ(test, b.woken, 1);

	/* Nothing in flight: the next record is the oldest, on the reserve. */
	KUNIT_ASSERT_TRUE(test, rx_loan_admit(lb, NULL, &b.w, &b.cr, 1, false));
	KUNIT_EXPECT_EQ(test, b.cr.reserve, 1U);
	rx_loan_kunit_return(&b);
	KUNIT_EXPECT_EQ(test, rx_loan_kunit_avail(lb), 0L);
	KUNIT_EXPECT_EQ(test, lb->reserve, 4L);
}

/* A dropped grant's credit is passed on from the kick work. */
static void rx_loan_grant_drop_test(struct kunit *test)
{
	struct rx_loan_budget *lb = rx_loan_kunit_budget(test, 16, 0);
	struct rx_loan_kunit_waiter g, p;

	rx_loan_kunit_waiter_init(&g, true, 16);
	rx_loan_kunit_waiter_init(&p, true, 1);
	KUNIT_ASSERT_EQ(test, rx_loan_grant(lb, NULL, &g.w), 0);
	KUNIT_EXPECT_EQ(test, g.w.credit, 16U);
	KUNIT_EXPECT_EQ(test, rx_loan_grant(lb, NULL, &p.w), -EAGAIN);
	rx_loan_park(lb, NULL, &p.w);
	KUNIT_EXPECT_EQ(test, p.woken, 0);

	rx_loan_grant_drop(lb, NULL, g.w.credit, g.w.reserve);
	flush_work(&lb->kick_work);
	KUNIT_EXPECT_EQ(test, p.woken, 1);
	KUNIT_EXPECT_EQ(test, p.w.credit, 1U);
	KUNIT_EXPECT_EQ(test, lb->read_granted, 1ULL);
	rx_loan_grant_return(lb, NULL, p.w.credit, p.w.reserve);
	KUNIT_EXPECT_EQ(test, rx_loan_read_inflight(lb), 0L);
	KUNIT_EXPECT_EQ(test, rx_loan_kunit_avail(lb), 16L);
}

/* A change of configuration with a record in flight wakes a waiter. */
static void rx_loan_reconfigure_test(struct kunit *test)
{
	struct rx_loan_budget *lb = rx_loan_kunit_budget(test, 16, 0);
	struct rx_loan_kunit_waiter a, b;

	rx_loan_kunit_waiter_init(&a, false, 0);
	rx_loan_kunit_waiter_init(&b, false, 0);
	KUNIT_ASSERT_TRUE(test, rx_loan_admit(lb, NULL, &a.w, &a.cr, 10, false));
	KUNIT_ASSERT_FALSE(test, rx_loan_admit(lb, NULL, &b.w, &b.cr, 20, false));

	/* Requested, not applied: the kick's locked op applies it. */
	spin_lock_bh(&lb->lock);
	rx_loan_budget_stage_locked(lb, 32, 0, false);
	spin_unlock_bh(&lb->lock);
	KUNIT_EXPECT_EQ(test, lb->budget, 16U);
	schedule_work(&lb->kick_work);
	flush_work(&lb->kick_work);
	KUNIT_EXPECT_EQ(test, lb->budget, 32U);
	KUNIT_EXPECT_EQ(test, lb->avail, 22L);
	KUNIT_EXPECT_EQ(test, b.woken, 1);

	KUNIT_ASSERT_TRUE(test, rx_loan_admit(lb, NULL, &b.w, &b.cr, 20, false));
	rx_loan_kunit_return(&a);
	rx_loan_kunit_return(&b);
	KUNIT_EXPECT_EQ(test, rx_loan_kunit_avail(lb), 32L);
}

/* A CPU's stock: refilled from avail, taken, trimmed, drained back. */
static void rx_loan_stock_test(struct kunit *test)
{
	struct rx_loan_budget *lb = rx_loan_kunit_budget(test, 256, 0);

	migrate_disable();
	spin_lock_bh(&lb->lock);
	KUNIT_EXPECT_EQ(test, rx_loan_stock_refill_locked(lb),
			(long)RX_LOAN_STOCK_BATCH);
	KUNIT_EXPECT_EQ(test, rx_loan_stock_refill_locked(lb), 0L);
	spin_unlock_bh(&lb->lock);
	KUNIT_EXPECT_EQ(test, lb->avail, 256L - RX_LOAN_STOCK_BATCH);
	KUNIT_EXPECT_TRUE(test, rx_loan_stock_take(lb, 10));
	KUNIT_EXPECT_FALSE(test, rx_loan_stock_take(lb, RX_LOAN_STOCK_BATCH));
	KUNIT_EXPECT_EQ(test, rx_loan_stock_put(lb, 10), 0L);
	/* Past RX_LOAN_STOCK_MAX the stock is trimmed to a batch. */
	KUNIT_EXPECT_EQ(test, rx_loan_stock_put(lb, RX_LOAN_STOCK_MAX),
			(long)RX_LOAN_STOCK_MAX);
	migrate_enable();

	KUNIT_EXPECT_EQ(test, rx_loan_kunit_avail(lb), 256L);
	/* Drained, the budget is contended: no stock until a kick clears it. */
	KUNIT_EXPECT_FALSE(test, rx_loan_stock_take(lb, 1));
	KUNIT_EXPECT_EQ(test, rx_loan_stock_put(lb, 5), 5L);
}

/* The charge is the peak of this window and the last. */
static void rx_loan_cost_window_test(struct kunit *test)
{
	struct rx_loan_budget *lb = rx_loan_kunit_budget(test, 16, 0);
	int i;

	spin_lock_bh(&lb->lock);
	rx_loan_cost_learn_locked(lb, 150);
	for (i = 1; i < RX_LOAN_COST_WINDOW; i++)
		rx_loan_cost_learn_locked(lb, 120);
	KUNIT_EXPECT_EQ(test, lb->cost_factor, 150U);
	for (i = 0; i < RX_LOAN_COST_WINDOW; i++)
		rx_loan_cost_learn_locked(lb, 110);
	KUNIT_EXPECT_EQ(test, lb->cost_factor, 150U);
	for (i = 0; i < RX_LOAN_COST_WINDOW; i++)
		rx_loan_cost_learn_locked(lb, 105);
	KUNIT_EXPECT_EQ(test, lb->cost_factor, 110U);
	spin_unlock_bh(&lb->lock);
	KUNIT_EXPECT_TRUE(test, rx_loan_cost_warm(lb));

	/* Stashed samples fold in as one peak over their count. */
	migrate_disable();
	rx_loan_cost_stash(lb, 200);
	rx_loan_cost_stash(lb, 180);
	spin_lock_bh(&lb->lock);
	rx_loan_cost_fold_cpu_locked(lb, raw_cpu_ptr(lb->stock));
	spin_unlock_bh(&lb->lock);
	migrate_enable();
	KUNIT_EXPECT_EQ(test, lb->cost_factor, 200U);
	KUNIT_EXPECT_EQ(test, lb->cost_records, 2U);
}

/* A unit that copied counts once, with its bytes; one that only lent, not. */
static void rx_loan_copies_test(struct kunit *test)
{
	struct rx_loan_table *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	u64 records, bytes, records2, bytes2;

	KUNIT_ASSERT_NOT_NULL(test, t);
	rx_loan_copies_read(&records, &bytes);
	t->copied_bytes = 300;
	t->borrowed_bytes = 4096;
	rx_loan_table_reset(t);
	KUNIT_EXPECT_EQ(test, t->copied_bytes, 0U);
	rx_loan_table_reset(t);
	t->borrowed_bytes = 4096;
	rx_loan_table_reset(t);
	rx_loan_copies_read(&records2, &bytes2);
	KUNIT_EXPECT_EQ(test, records2 - records, 1ULL);
	KUNIT_EXPECT_EQ(test, bytes2 - bytes, 300ULL);
}

/* A read pool follows the region's read size at its next locked op. */
static void rx_loan_region_read_pickup_test(struct kunit *test)
{
	struct rx_loan_budget *lb = rx_loan_kunit_budget(test, 0, 0);
	struct rx_loan_kunit_waiter a;
	int ret;

	ret = kunit_add_action_or_reset(test, rx_loan_kunit_region_reset, NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);
	lb->read = true;
	rx_loan_kunit_region(64, 16);
	rx_loan_budget_request(lb, 0, 0, false);
	KUNIT_EXPECT_EQ(test, lb->budget, 16U);

	/* Resized with no request: admission's locked op applies it. */
	rx_loan_kunit_region(64, 24);
	rx_loan_kunit_waiter_init(&a, false, 0);
	KUNIT_ASSERT_TRUE(test, rx_loan_admit(lb, NULL, &a.w, &a.cr, 4, false));
	KUNIT_EXPECT_EQ(test, lb->budget, 24U);
	KUNIT_EXPECT_EQ(test, lb->avail, 20L);
	rx_loan_kunit_return(&a);

	/* Unsplit, a read pool has what the configured budgets leave. */
	rx_loan_kunit_region(48, 0);
	KUNIT_EXPECT_EQ(test, rx_loan_read_pool_pages(40), 8U);
	KUNIT_EXPECT_EQ(test, rx_loan_read_pool_pages(48), 0U);
}

/*
 * The computed size on the qualified shapes: kNFSd's floor is two
 * double-buffered records of one maximum payload (1 MiB) per thread,
 * nrthreads * 4 * (payload pages + 1).
 */
static void rx_loan_region_size_for_test(struct kunit *test)
{
	unsigned long big = 1UL << 30;	/* pages: no memory cap */
	unsigned long payload = (1UL << 20) >> PAGE_SHIFT;

	/* Nothing asks: no region. */
	KUNIT_EXPECT_EQ(test, page_pool_rx_region_kunit_size_for(0, 0, big), 0UL);
	/* A READ server whose NIC has no page_pool, 192 threads. */
	if (PAGE_SIZE == 4096)
		KUNIT_EXPECT_EQ(test,
				page_pool_rx_region_kunit_size_for(0,
					192 * 4 * (payload + 1), big),
				262144UL);
	/* veth rig: one queue of 256 and 8 threads. */
	KUNIT_EXPECT_EQ(test,
			page_pool_rx_region_kunit_size_for(256,
				8 * 4 * (payload + 1), big),
			roundup_pow_of_two(256 + 8 * 4 * (payload + 1)));
	/* At least one maximum READ. */
	KUNIT_EXPECT_EQ(test, page_pool_rx_region_kunit_size_for(1, 0, big),
			roundup_pow_of_two(PAGE_POOL_RX_REGION_READ_MIN));
	/* At most a power of two within a 64th of memory. */
	KUNIT_EXPECT_EQ(test,
			page_pool_rx_region_kunit_size_for(0, 1UL << 20,
							   3000 * 64),
			2048UL);
	/* None where that is less than one maximum READ. */
	KUNIT_EXPECT_EQ(test,
			page_pool_rx_region_kunit_size_for(0, 1UL << 20,
							   100 * 64),
			0UL);
}

/*
 * Shares split the region that exists among every consumer, @o others
 * among them; a late consumer gets its part.
 */
static void rx_loan_consumer_share_test(struct kunit *test)
{
	struct rx_loan_consumer *a, *b;
	unsigned long o;

	rx_loan_kunit_standin(test, 1024, 0);
	o = rx_loan_kunit_others(test, 1024);
	/* Floors of 0: the stand-in region is the one shared. */
	a = rx_loan_kunit_consumer(test, "a");
	KUNIT_EXPECT_EQ(test, rx_loan_consumer_share(a), 1024 / (o + 1));
	b = rx_loan_kunit_consumer(test, "b");
	KUNIT_EXPECT_EQ(test, rx_loan_consumer_share(a), 1024 / (o + 2));
	KUNIT_EXPECT_EQ(test, rx_loan_consumer_share(b), 1024 / (o + 2));
	rx_loan_consumer_unregister(a);
	KUNIT_EXPECT_EQ(test, rx_loan_consumer_share(a), 0UL);
	KUNIT_EXPECT_EQ(test, rx_loan_consumer_share(b), 1024 / (o + 1));
	rx_loan_consumer_unregister(b);
	rx_loan_kunit_region(0, 0);
}

/*
 * What the region holds out past @base, what it had out when the test
 * began: all of @lb's credit not in its avail or its stocks (stocked
 * credit is charged as a record takes it).
 */
static void rx_loan_kunit_expect_out(struct kunit *test,
				     struct rx_loan_budget *lb, long size,
				     long base)
{
	long avail;
	int cpu;

	spin_lock_bh(&lb->lock);
	avail = lb->avail + lb->reserve;
	for_each_possible_cpu(cpu)
		avail += atomic_long_read(&per_cpu_ptr(lb->stock, cpu)->pages);
	spin_unlock_bh(&lb->lock);
	KUNIT_EXPECT_EQ(test, rx_loan_region_out() - base, size - avail);
}

struct rx_loan_kunit_racer {
	struct rx_loan_budget *lb;
	struct rx_loan_consumer *c, *other;
	unsigned long region;
	atomic_t *bad;
	struct completion done;
	u32 seed;
};

static int rx_loan_kunit_race_fn(void *data)
{
	struct rx_loan_kunit_racer *r = data;
	unsigned long share = rx_loan_consumer_share(r->c);
	struct rx_loan_kunit_waiter kw;
	int i;

	for (i = 0; i < 3000; i++) {
		u32 cost;

		r->seed = r->seed * 1103515245 + 12345;
		cost = 1 + (r->seed >> 16) % 32;
		rx_loan_kunit_waiter_init(&kw, false, 0);
		if (rx_loan_admit(r->lb, r->c, &kw.w, &kw.cr, cost, true)) {
			struct rx_loan_lend out = {};

			if (rx_loan_region_out() > (long)r->region ||
			    atomic_long_read(&r->c->out_write) > (long)share)
				atomic_inc(r->bad);
			rx_loan_return(&kw.cr, 0, 0, &out);
		} else {
			rx_loan_unwait(&kw.w);
		}
		if (rx_loan_region_out() > (long)r->region ||
		    atomic_long_read(&r->c->out_write) +
		    atomic_long_read(&r->other->out_write) >
		    (long)r->region)
			atomic_inc(r->bad);
		cond_resched();
	}
	complete(&r->done);
	return 0;
}

/*
 * Two consumers (a sunrpc-like one and an nvmet stand-in) admitting at
 * once from two threads: the region's total and each share hold after
 * every operation.
 */
static void rx_loan_region_total_concurrent_test(struct kunit *test)
{
	struct rx_loan_budget *lb;
	struct rx_loan_consumer *a, *b;
	struct rx_loan_kunit_racer r[2];
	struct task_struct *t;
	atomic_t bad = ATOMIC_INIT(0);
	long base;
	int i;

	rx_loan_kunit_standin(test, 256, 0);
	base = rx_loan_region_out();
	lb = rx_loan_kunit_budget(test, 256, 0);
	a = rx_loan_kunit_consumer(test, "sunrpc");
	b = rx_loan_kunit_consumer(test, "nvmet");
	for (i = 0; i < 2; i++) {
		r[i].lb = lb;
		r[i].c = i ? b : a;
		r[i].other = i ? a : b;
		r[i].region = 256;
		r[i].bad = &bad;
		r[i].seed = 7 + i;
		init_completion(&r[i].done);
		t = kthread_run(rx_loan_kunit_race_fn, &r[i], "rx_loan_race%d",
				i);
		KUNIT_ASSERT_FALSE(test, IS_ERR(t));
	}
	for (i = 0; i < 2; i++)
		wait_for_completion(&r[i].done);
	KUNIT_EXPECT_EQ(test, atomic_read(&bad), 0);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&a->out_write), 0L);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&b->out_write), 0L);
	KUNIT_EXPECT_EQ(test, rx_loan_kunit_avail(lb), 256L);
	KUNIT_EXPECT_EQ(test, rx_loan_region_out(), base);
	rx_loan_consumer_unregister(b);
	rx_loan_consumer_unregister(a);
	rx_loan_kunit_region(0, 0);
}

#define RX_LOAN_KUNIT_HELD	16	/* records and grants a racer holds */
#define RX_LOAN_KUNIT_COST	8	/* the most one asks: stockable */

struct rx_loan_kunit_binder {
	struct rx_loan_budget *lb[2];	/* admits from [0], grants from [1] */
	struct rx_loan_consumer *c[2];	/* [0] is this racer's */
	long region;
	long max_region;		/* the most seen out of the region */
	long max_out[2];		/* and of each consumer, as c[] */
	atomic_t *bad;
	struct completion done;
	u32 seed;
	struct rx_loan_kunit_waiter rec[RX_LOAN_KUNIT_HELD];
	struct rx_loan_kunit_waiter grant[RX_LOAN_KUNIT_HELD];
};

/*
 * The region's total, and each consumer within its share but for one
 * oldest overrun on each budget it draws on.
 */
static void rx_loan_kunit_bind_check(struct rx_loan_kunit_binder *r)
{
	long v = rx_loan_region_out();
	int i;

	r->max_region = max(r->max_region, v);
	if (v > r->region)
		atomic_inc(r->bad);
	for (i = 0; i < 2; i++) {
		v = atomic_long_read(&r->c[i]->out_write);
		r->max_out[i] = max(r->max_out[i], v);
		if (v > (long)rx_loan_consumer_share(r->c[i]) +
		    2 * RX_LOAN_KUNIT_COST)
			atomic_inc(r->bad);
	}
}

/*
 * A racer's waiters decline every wake: their slots are reused at once,
 * and a wake from the other racer must not land in a reused one.  The
 * racer retries on its own.
 */
static bool rx_loan_kunit_bind_pin(struct rx_loan_waiter *w)
{
	return false;
}

static const struct rx_loan_waiter_ops rx_loan_kunit_bind_ops = {
	.pin	= rx_loan_kunit_bind_pin,
	.wake	= rx_loan_kunit_wake,
};

static int rx_loan_kunit_bind_fn(void *data)
{
	struct rx_loan_kunit_binder *r = data;
	struct rx_loan_consumer *c = r->c[0];
	int i;

	for (i = 0; i < 4000; i++) {
		int slot = i % RX_LOAN_KUNIT_HELD;
		struct rx_loan_kunit_waiter *kr = &r->rec[slot];
		struct rx_loan_kunit_waiter *kg = &r->grant[slot];
		struct rx_loan_lend out = {};
		u32 cost;

		r->seed = r->seed * 1103515245 + 12345;
		cost = 1 + (r->seed >> 16) % RX_LOAN_KUNIT_COST;
		/* A stocked admit, or the return of the record in its slot. */
		if (kr->cr.linked) {
			rx_loan_return(&kr->cr, 0, 0, &out);
		} else {
			rx_loan_kunit_waiter_init(kr, false, 0);
			kr->w.ops = &rx_loan_kunit_bind_ops;
			if (!rx_loan_admit(r->lb[0], c, &kr->w, &kr->cr, cost,
					   true))
				rx_loan_unwait(&kr->w);
		}
		rx_loan_kunit_bind_check(r);
		/* A grant, or the return of the grant in its slot. */
		if (kg->w.budget) {
			rx_loan_grant_return(r->lb[1], c, kg->w.credit,
					     kg->w.reserve);
			kg->w.budget = NULL;
		} else {
			rx_loan_kunit_waiter_init(kg, true, cost);
			kg->w.ops = &rx_loan_kunit_bind_ops;
			if (rx_loan_grant(r->lb[1], c, &kg->w))
				kg->w.budget = NULL;
		}
		rx_loan_kunit_bind_check(r);
		cond_resched();
	}
	for (i = 0; i < RX_LOAN_KUNIT_HELD; i++) {
		struct rx_loan_lend out = {};

		if (r->rec[i].cr.linked)
			rx_loan_return(&r->rec[i].cr, 0, 0, &out);
		if (r->grant[i].w.budget)
			rx_loan_grant_return(r->lb[1], c, r->grant[i].w.credit,
					     r->grant[i].w.reserve);
		rx_loan_kunit_bind_check(r);
	}
	complete(&r->done);
	return 0;
}

/*
 * Two consumers on two threads, each taking stocked admits from one budget
 * and grants from another.  The region is a share of 64 for each consumer,
 * the @o others too, so 128 for the two alone.  Each racer would hold up
 * to 256 pages, twice that and four times its share, and the two budgets
 * (1.5 times the region each) together are three times the region, so the
 * region binds and refuses: its total and each consumer's share hold after
 * every operation, and everything comes back.
 */
static void rx_loan_region_binds_concurrent_test(struct kunit *test)
{
	struct rx_loan_consumer *a, *b;
	struct rx_loan_kunit_binder *r;
	struct rx_loan_budget *lb[2];
	atomic_t bad = ATOMIC_INIT(0);
	struct task_struct *t;
	long denied, share, region, base;
	unsigned long o;
	int i;

	r = kunit_kcalloc(test, 2, sizeof(*r), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, r);
	rx_loan_kunit_standin(test, 128, 0);
	o = rx_loan_kunit_others(test, 128);
	region = 64 * (o + 2);
	rx_loan_kunit_region(region, 0);
	base = rx_loan_region_out();
	denied = rx_loan_kunit_region_denied();
	lb[0] = rx_loan_kunit_budget(test, region * 3 / 2, 0);
	lb[1] = rx_loan_kunit_budget(test, region * 3 / 2, 0);
	a = rx_loan_kunit_consumer(test, "sunrpc");
	b = rx_loan_kunit_consumer(test, "nvmet");
	share = rx_loan_consumer_share(a);
	KUNIT_ASSERT_EQ(test, share, 64L);
	/*
	 * The shares sum to the region, so only the oldest's overrun passes
	 * it: A's oldest record takes all but 28 pages of it (past its
	 * share), and B's grant of 40, within B's share, must be refused by
	 * the region.
	 */
	rx_loan_kunit_waiter_init(&r[0].rec[0], false, 0);
	KUNIT_ASSERT_TRUE(test, rx_loan_admit(lb[0], a, &r[0].rec[0].w,
					      &r[0].rec[0].cr,
					      region - base - 28, false));
	rx_loan_kunit_waiter_init(&r[1].grant[0], true, 40);
	KUNIT_EXPECT_EQ(test, rx_loan_grant(lb[1], b, &r[1].grant[0].w),
			-EAGAIN);
	KUNIT_EXPECT_GT(test, rx_loan_kunit_region_denied(), denied);
	KUNIT_EXPECT_LE(test, rx_loan_region_out(), region);
	{
		struct rx_loan_lend out = {};

		rx_loan_return(&r[0].rec[0].cr, 0, 0, &out);
	}
	KUNIT_ASSERT_EQ(test, rx_loan_region_out(), base);
	memset(r, 0, 2 * sizeof(*r));
	for (i = 0; i < 2; i++) {
		r[i].lb[0] = lb[0];
		r[i].lb[1] = lb[1];
		r[i].c[0] = i ? b : a;
		r[i].c[1] = i ? a : b;
		r[i].region = region;
		r[i].bad = &bad;
		r[i].seed = 11 + i;
		init_completion(&r[i].done);
		t = kthread_run(rx_loan_kunit_bind_fn, &r[i], "rx_loan_bind%d",
				i);
		KUNIT_ASSERT_FALSE(test, IS_ERR(t));
	}
	for (i = 0; i < 2; i++)
		wait_for_completion(&r[i].done);
	flush_work(&lb[0]->kick_work);
	flush_work(&lb[1]->kick_work);
	KUNIT_EXPECT_EQ(test, atomic_read(&bad), 0);
	/* The region was asked for more than it had, and said no. */
	KUNIT_EXPECT_GT(test, rx_loan_kunit_region_denied(), denied);
	/* Its total and each share held throughout, as each racer saw. */
	for (i = 0; i < 2; i++) {
		KUNIT_EXPECT_LE(test, r[i].max_region, region);
		KUNIT_EXPECT_LE(test, r[i].max_out[0],
				share + 2 * RX_LOAN_KUNIT_COST);
		KUNIT_EXPECT_LE(test, r[i].max_out[1],
				share + 2 * RX_LOAN_KUNIT_COST);
	}
	KUNIT_EXPECT_EQ(test, atomic_long_read(&a->out_write), 0L);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&b->out_write), 0L);
	KUNIT_EXPECT_EQ(test, rx_loan_kunit_avail(lb[0]), region * 3 / 2);
	KUNIT_EXPECT_EQ(test, rx_loan_kunit_avail(lb[1]), region * 3 / 2);
	KUNIT_EXPECT_EQ(test, rx_loan_region_out(), base);
	rx_loan_consumer_unregister(b);
	rx_loan_consumer_unregister(a);
	rx_loan_kunit_region(0, 0);
}

/*
 * A late consumer gets its share; the first's credit is not revoked.  The
 * region is a share of 32 for each consumer, the @o others too; alone, A
 * takes as many records of 12 as its larger share holds, up to four, which
 * passes 32, and the budget is 16 more than that.
 */
static void rx_loan_late_consumer_test(struct kunit *test)
{
	struct rx_loan_budget *lb;
	struct rx_loan_consumer *a, *b;
	struct rx_loan_kunit_waiter ka[5], kb[2];
	long region, budget, base;
	unsigned long o;
	int i, n;

	rx_loan_kunit_standin(test, 64, 0);
	o = rx_loan_kunit_others(test, 64);
	region = 32 * (o + 2);
	rx_loan_kunit_region(region, 0);
	base = rx_loan_region_out();
	a = rx_loan_kunit_consumer(test, "a");
	n = min_t(unsigned long, 4, rx_loan_consumer_share(a) / 12);
	KUNIT_ASSERT_GT(test, 12 * n, 32);
	budget = 12 * n + 16;
	lb = rx_loan_kunit_budget(test, budget, 0);
	for (i = 0; i < n; i++) {
		rx_loan_kunit_waiter_init(&ka[i], false, 0);
		KUNIT_ASSERT_TRUE(test, rx_loan_admit(lb, a, &ka[i].w,
						      &ka[i].cr, 12, false));
	}
	b = rx_loan_kunit_consumer(test, "b");
	KUNIT_EXPECT_EQ(test, rx_loan_consumer_share(a), 32UL);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&a->out_write), 12L * n);
	/* Past its new share, not the oldest: refused, nothing revoked. */
	rx_loan_kunit_waiter_init(&ka[4], false, 0);
	KUNIT_EXPECT_FALSE(test, rx_loan_admit(lb, a, &ka[4].w, &ka[4].cr,
					       12, false));
	KUNIT_EXPECT_EQ(test, atomic_long_read(&a->share_denied), 1L);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&a->out_write), 12L * n);
	/*
	 * B gets its share of what the budget has left: A's refused record
	 * waits for A, queued, without heading the FIFO in B's way.
	 */
	rx_loan_kunit_waiter_init(&kb[0], false, 0);
	KUNIT_EXPECT_TRUE(test, rx_loan_admit(lb, b, &kb[0].w, &kb[0].cr,
					      12, false));
	KUNIT_EXPECT_TRUE(test, rx_loan_waiting(&ka[4].w));
	rx_loan_kunit_waiter_init(&kb[1], false, 0);
	KUNIT_EXPECT_FALSE(test, rx_loan_admit(lb, b, &kb[1].w, &kb[1].cr,
					       12, false));
	KUNIT_EXPECT_EQ(test, atomic_long_read(&b->share_denied), 0L);
	KUNIT_EXPECT_LE(test, rx_loan_region_out(), region);
	rx_loan_unwait(&kb[1].w);
	rx_loan_kunit_return(&kb[0]);
	/* B's return does not bring A under its share: A's record waits. */
	KUNIT_EXPECT_TRUE(test, rx_loan_waiting(&ka[4].w));
	KUNIT_EXPECT_EQ(test, ka[4].woken, 0);
	for (i = 0; i < n; i++)
		rx_loan_kunit_return(&ka[i]);
	/* A's own returns do. */
	KUNIT_EXPECT_GT(test, ka[4].woken, 0);
	rx_loan_unwait(&ka[4].w);
	KUNIT_EXPECT_EQ(test, rx_loan_kunit_avail(lb), budget);
	KUNIT_EXPECT_EQ(test, rx_loan_region_out(), base);
	rx_loan_consumer_unregister(b);
	rx_loan_consumer_unregister(a);
	rx_loan_kunit_region(0, 0);
}

/* A stocked take charges the region; its return refunds it, exactly. */
static void rx_loan_stock_region_charge_test(struct kunit *test)
{
	struct rx_loan_budget *lb;
	struct rx_loan_kunit_waiter kw;
	long base;

	rx_loan_kunit_standin(test, 1024, 0);
	base = rx_loan_region_out();
	lb = rx_loan_kunit_budget(test, 256, 0);
	rx_loan_kunit_waiter_init(&kw, false, 0);
	KUNIT_ASSERT_TRUE(test, rx_loan_admit(lb, NULL, &kw.w, &kw.cr, 4,
					      true));
	rx_loan_kunit_expect_out(test, lb, 256, base);
	rx_loan_kunit_return(&kw);
	rx_loan_kunit_expect_out(test, lb, 256, base);
	KUNIT_EXPECT_EQ(test, rx_loan_kunit_avail(lb), 256L);
	KUNIT_EXPECT_EQ(test, rx_loan_region_out(), base);
	rx_loan_kunit_region(0, 0);
}

/*
 * A record orphaned under a region keeps spent only what its pages still
 * out can hold: what came back with its pages returns to its consumer's
 * share, the budget and the region, as a return would.
 */
static void rx_loan_orphan_region_refund_test(struct kunit *test)
{
	struct rx_loan_consumer *c;
	struct rx_loan_budget *lb;
	struct rx_loan_kunit_waiter a, s;
	long base, orphaned;

	rx_loan_kunit_standin(test, 1024, 0);
	base = rx_loan_region_out();
	orphaned = rx_loan_region_orphaned();
	lb = rx_loan_kunit_budget(test, 256, 0);
	c = rx_loan_kunit_consumer(test, "c");
	rx_loan_kunit_waiter_init(&a, false, 0);
	KUNIT_ASSERT_TRUE(test, rx_loan_admit(lb, c, &a.w, &a.cr, 12, false));
	KUNIT_EXPECT_EQ(test, atomic_long_read(&c->out_write), 12L);

	/* Four pages still out: those four stay spent, eight come back. */
	rx_loan_orphan(&a.cr, 4);
	KUNIT_EXPECT_FALSE(test, a.cr.linked);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&c->out_write), 4L);
	KUNIT_EXPECT_EQ(test, rx_loan_region_orphaned(), orphaned + 4);
	KUNIT_EXPECT_EQ(test, lb->orphaned, 4ULL);
	KUNIT_EXPECT_EQ(test, lb->inflight_count, 0U);
	KUNIT_EXPECT_EQ(test, rx_loan_kunit_avail(lb), 252L);
	rx_loan_kunit_expect_out(test, lb, 256, base);

	/* A stocked record whose pages all came back keeps nothing. */
	rx_loan_kunit_waiter_init(&s, false, 0);
	migrate_disable();
	spin_lock_bh(&lb->lock);
	/* Nobody waits: stock again, as a kick would. */
	lb->contended = false;
	KUNIT_EXPECT_EQ(test, rx_loan_stock_refill_locked(lb),
			(long)RX_LOAN_STOCK_BATCH);
	spin_unlock_bh(&lb->lock);
	KUNIT_EXPECT_TRUE(test, rx_loan_admit(lb, c, &s.w, &s.cr, 4, true));
	KUNIT_EXPECT_TRUE(test, s.cr.stocked);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&c->out_write), 8L);
	rx_loan_orphan(&s.cr, 0);
	migrate_enable();
	KUNIT_EXPECT_EQ(test, atomic_long_read(&c->out_write), 4L);
	KUNIT_EXPECT_EQ(test, rx_loan_region_orphaned(), orphaned + 4);
	KUNIT_EXPECT_EQ(test, lb->orphaned, 4ULL);
	KUNIT_EXPECT_EQ(test, rx_loan_stocked_records(lb), 0L);
	rx_loan_kunit_expect_out(test, lb, 256, base);
	rx_loan_consumer_unregister(c);
	rx_loan_kunit_region(0, 0);
}

/* Grants, parked grants and their returns are charged and refunded. */
static void rx_loan_grant_region_test(struct kunit *test)
{
	struct rx_loan_budget *lb;
	struct rx_loan_kunit_waiter g, p;
	long base;

	rx_loan_kunit_standin(test, 1024, 0);
	base = rx_loan_region_out();
	lb = rx_loan_kunit_budget(test, 256, 0);
	rx_loan_kunit_waiter_init(&g, true, 16);
	KUNIT_ASSERT_EQ(test, rx_loan_grant(lb, NULL, &g.w), 0);
	rx_loan_kunit_expect_out(test, lb, 256, base);
	rx_loan_kunit_waiter_init(&p, true, 24);
	rx_loan_park(lb, NULL, &p.w);
	KUNIT_EXPECT_EQ(test, p.woken, 1);
	KUNIT_EXPECT_EQ(test, p.w.credit, 24L);
	rx_loan_kunit_expect_out(test, lb, 256, base);
	rx_loan_grant_return(lb, NULL, g.w.credit, g.w.reserve);
	rx_loan_kunit_expect_out(test, lb, 256, base);
	rx_loan_grant_drop(lb, NULL, p.w.credit, p.w.reserve);
	flush_work(&lb->kick_work);
	rx_loan_kunit_expect_out(test, lb, 256, base);
	KUNIT_EXPECT_EQ(test, rx_loan_kunit_avail(lb), 256L);
	KUNIT_EXPECT_EQ(test, rx_loan_region_out(), base);
	rx_loan_kunit_region(0, 0);
}

/*
 * Progress: the oldest record may pass its share, never the region; it
 * pauses on the region and resumes when another consumer returns.  Each
 * one's first record is its oldest, so other consumers' smaller shares
 * change none of this.
 */
static void rx_loan_progress_region_test(struct kunit *test)
{
	struct rx_loan_budget *lb;
	struct rx_loan_consumer *a, *b;
	struct rx_loan_kunit_waiter ka, kb;
	long base;

	rx_loan_kunit_standin(test, 24, 0);
	base = rx_loan_region_out();
	lb = rx_loan_kunit_budget(test, 32, 0);
	a = rx_loan_kunit_consumer(test, "a");
	b = rx_loan_kunit_consumer(test, "b");
	rx_loan_kunit_waiter_init(&ka, false, 0);
	KUNIT_ASSERT_TRUE(test, rx_loan_admit(lb, a, &ka.w, &ka.cr, 16,
					      false));
	KUNIT_EXPECT_EQ(test, atomic_long_read(&a->overrun_share), 1L);
	rx_loan_kunit_waiter_init(&kb, false, 0);
	KUNIT_ASSERT_TRUE(test, rx_loan_admit(lb, b, &kb.w, &kb.cr, 6,
					      false));
	KUNIT_EXPECT_FALSE(test, rx_loan_charge(&ka.w, &ka.cr, 20));
	KUNIT_EXPECT_EQ(test, rx_loan_region_out(), base + 22);
	rx_loan_kunit_return(&kb);
	KUNIT_EXPECT_EQ(test, ka.woken, 1);
	KUNIT_EXPECT_TRUE(test, rx_loan_charge(&ka.w, &ka.cr, 20));
	KUNIT_EXPECT_EQ(test, rx_loan_region_out(), base + 20);
	rx_loan_kunit_return(&ka);
	KUNIT_EXPECT_EQ(test, rx_loan_region_out(), base);
	rx_loan_consumer_unregister(b);
	rx_loan_consumer_unregister(a);
	rx_loan_kunit_region(0, 0);
}

/*
 * Two namespaces' budgets share one region total: one refused for the
 * region is kicked when the other's credit returns.
 */
static void rx_loan_netns_budget_test(struct kunit *test)
{
	struct rx_loan_net *n1, *n2;
	struct rx_loan_kunit_waiter k1, k2;
	long base;

	rx_loan_kunit_standin(test, 64, 0);
	base = rx_loan_region_out();
	n1 = rx_loan_kunit_net(test);
	n2 = rx_loan_kunit_net(test);
	rx_loan_net_write_request(n1, 48, 0, false);
	rx_loan_net_write_request(n2, 48, 0, false);
	rx_loan_kunit_waiter_init(&k1, false, 0);
	KUNIT_ASSERT_TRUE(test, rx_loan_admit(&n1->write, NULL, &k1.w,
					      &k1.cr, 40, false));
	rx_loan_kunit_waiter_init(&k2, false, 0);
	KUNIT_EXPECT_FALSE(test, rx_loan_admit(&n2->write, NULL, &k2.w,
					       &k2.cr, 40, false));
	KUNIT_EXPECT_EQ(test, rx_loan_region_out(), base + 40);
	rx_loan_kunit_return(&k1);
	flush_work(&n2->write.kick_work);
	KUNIT_EXPECT_EQ(test, k2.woken, 1);
	KUNIT_EXPECT_TRUE(test, rx_loan_admit(&n2->write, NULL, &k2.w,
					      &k2.cr, 40, false));
	rx_loan_kunit_return(&k2);
	rx_loan_net_state_exit(n2);
	rx_loan_net_state_exit(n1);
	KUNIT_EXPECT_EQ(test, rx_loan_region_out(), base);
	rx_loan_kunit_region(0, 0);
}

/*
 * The region's cases with another consumer registered first, as a running
 * server's is when the suite loads after nfsd starts: shares are over
 * every consumer, and each case cleans up what it registers whether it
 * passes or not.
 */
static void rx_loan_other_consumer_test(struct kunit *test)
{
	unsigned long o;

	rx_loan_kunit_standin(test, 1024, 0);
	o = rx_loan_kunit_others(test, 1024);
	rx_loan_kunit_consumer(test, "live");
	KUNIT_EXPECT_EQ(test, rx_loan_kunit_others(test, 1024), o + 1);
	rx_loan_kunit_region(0, 0);
	rx_loan_consumer_share_test(test);
	rx_loan_region_binds_concurrent_test(test);
	rx_loan_late_consumer_test(test);
	rx_loan_progress_region_test(test);
	rx_loan_netns_budget_test(test);
}

static void rx_loan_cfg_parse_expect(struct kunit *test, const char *val,
				     unsigned int want, bool want_manual)
{
	unsigned int out = ~0U;
	bool manual = !want_manual;

	KUNIT_EXPECT_EQ_MSG(test, page_pool_cfg_parse(val, &out, &manual), 0,
			    "%s", val);
	KUNIT_EXPECT_EQ_MSG(test, out, want, "%s", val);
	KUNIT_EXPECT_EQ_MSG(test, manual, want_manual, "%s", val);
}

/* The region's settings as a case found them, put back however it ends. */
struct rx_loan_kunit_region_saved {
	unsigned int pages, read_pages;
	bool failed;
	char *stack;
};

static void rx_loan_kunit_region_params_restore(void *data)
{
	struct rx_loan_kunit_region_saved *saved = data;

	page_pool_rx_region_kunit_config(saved->pages, saved->read_pages,
					 saved->failed);
	page_pool_rx_region_kunit_param("stack", saved->stack, NULL);
}

#define RX_LOAN_KUNIT_CARVED_ORDER	4

static void rx_loan_kunit_carved_free(struct page *base)
{
	unsigned long i;

	for (i = 0; i < (1UL << RX_LOAN_KUNIT_CARVED_ORDER); i++)
		__free_page(base + i);
}

/* A KUnit region over pages of its own, as a carve would publish. */
static void rx_loan_kunit_carved_remove(void *data)
{
	page_pool_rx_region_kunit_remove(false);
	rx_loan_kunit_carved_free(data);
}

/*
 * The region's settings through the ops their module parameters and
 * debugfs files share: 0 is auto (the computed size), N is manual N, and
 * back to 0 is auto again; a line read back parses to what it says.
 * read_pages cannot move once a region is carved, but writing back what
 * it reads is no change and succeeds; pages then still reads the size the
 * next carve uses.
 */
static void rx_loan_region_param_test(struct kunit *test)
{
	struct rx_loan_kunit_region_saved *saved;
	char *buf = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	char *line = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	unsigned int out, read_pages;
	struct page *base;
	char want[48];
	bool manual;
	int ret;

	saved = kunit_kzalloc(test, sizeof(*saved), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, saved);
	saved->stack = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, saved->stack);
	KUNIT_ASSERT_NOT_NULL(test, buf);
	KUNIT_ASSERT_NOT_NULL(test, line);
	page_pool_rx_region_kunit_params(&saved->pages, &saved->read_pages,
					 &saved->failed);
	KUNIT_ASSERT_GT(test, page_pool_rx_region_kunit_param("stack", NULL,
							      saved->stack), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			rx_loan_kunit_region_params_restore, saved), 0);

	rx_loan_cfg_parse_expect(test, "0", 0, false);
	rx_loan_cfg_parse_expect(test, "auto\n", 0, false);
	rx_loan_cfg_parse_expect(test, "7", 7, true);
	rx_loan_cfg_parse_expect(test, "12 (auto)\n", 0, false);
	rx_loan_cfg_parse_expect(test, "12 (manual)\n", 12, true);
	rx_loan_cfg_parse_expect(test, "0 (manual)", 0, true);
	rx_loan_cfg_parse_expect(test, "off", 0, true);
	KUNIT_EXPECT_EQ(test, page_pool_cfg_parse("7 (later)", &out, &manual),
			-EINVAL);
	KUNIT_EXPECT_NE(test, page_pool_cfg_parse("seven", &out, &manual), 0);

	KUNIT_ASSERT_GT(test, page_pool_rx_region_kunit_param("pages", "0", buf), 0);
	snprintf(want, sizeof(want), "%lu (auto)\n",
		 page_pool_rx_region_kunit_carve_size(totalram_pages()));
	KUNIT_EXPECT_STREQ(test, buf, want);
	KUNIT_ASSERT_GT(test, page_pool_rx_region_kunit_param("pages", "4096", buf), 0);
	KUNIT_EXPECT_STREQ(test, buf, "4096 (manual)\n");
	KUNIT_ASSERT_GT(test, page_pool_rx_region_kunit_param("pages", "0", buf), 0);
	KUNIT_EXPECT_STREQ(test, buf, want);
	KUNIT_ASSERT_GT(test, page_pool_rx_region_kunit_param("pages", "4096 (manual)", buf), 0);
	KUNIT_EXPECT_STREQ(test, buf, "4096 (manual)\n");
	KUNIT_ASSERT_GT(test, page_pool_rx_region_kunit_param("pages", "4096 (auto)", buf), 0);
	KUNIT_EXPECT_STREQ(test, buf, want);

	KUNIT_ASSERT_GT(test, page_pool_rx_region_kunit_param("stack", "16", buf), 0);
	KUNIT_EXPECT_STREQ(test, buf, "16 (manual)\n");
	KUNIT_ASSERT_GT(test, page_pool_rx_region_kunit_param("stack", "auto", buf), 0);
	KUNIT_EXPECT_STREQ(test, buf, "0 (auto)\n");

	/* a split too small for one maximum READ is refused */
	snprintf(want, sizeof(want), "%lu", PAGE_POOL_RX_REGION_READ_MIN - 1);
	KUNIT_EXPECT_EQ(test, page_pool_rx_region_kunit_param("read_pages", want, NULL),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, page_pool_rx_region_kunit_param("nonesuch", "0", NULL),
			-ENOENT);

	/* a carved region: read_pages is fixed, a write-back is no change */
	base = alloc_pages(GFP_KERNEL, RX_LOAN_KUNIT_CARVED_ORDER);
	KUNIT_ASSERT_NOT_NULL(test, base);
	split_page(base, RX_LOAN_KUNIT_CARVED_ORDER);
	ret = page_pool_rx_region_kunit_install(base,
			1UL << RX_LOAN_KUNIT_CARVED_ORDER, 0);
	if (ret) {	/* -EBUSY: a region is carved already; use it */
		rx_loan_kunit_carved_free(base);
		KUNIT_ASSERT_EQ(test, ret, -EBUSY);
	} else {
		KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
				rx_loan_kunit_carved_remove, base), 0);
	}
	KUNIT_ASSERT_GT(test, page_pool_rx_region_kunit_param("read_pages", NULL, line), 0);
	KUNIT_EXPECT_GT(test, page_pool_rx_region_kunit_param("read_pages", line, buf), 0);
	KUNIT_EXPECT_STREQ(test, buf, line);
	page_pool_rx_region_kunit_params(&out, &read_pages, &manual);
	snprintf(want, sizeof(want), "%lu",
		 PAGE_POOL_RX_REGION_READ_MIN + (read_pages == PAGE_POOL_RX_REGION_READ_MIN));
	KUNIT_EXPECT_EQ(test, page_pool_rx_region_kunit_param("read_pages", want, buf),
			-EBUSY);
	KUNIT_ASSERT_GT(test, page_pool_rx_region_kunit_param("read_pages", NULL, buf), 0);
	KUNIT_EXPECT_STREQ(test, buf, line);
	/* the setting is the next carve's size, not the carved region's */
	KUNIT_ASSERT_GT(test, page_pool_rx_region_kunit_param("pages", "4096", buf), 0);
	KUNIT_EXPECT_STREQ(test, buf, "4096 (manual)\n");
}

static struct kunit_case rx_loan_test_cases[] = {
	KUNIT_CASE(rx_loan_region_param_test),
	KUNIT_CASE(rx_loan_region_size_for_test),
	KUNIT_CASE(rx_loan_consumer_share_test),
	KUNIT_CASE(rx_loan_other_consumer_test),
	KUNIT_CASE(rx_loan_region_total_concurrent_test),
	KUNIT_CASE(rx_loan_region_binds_concurrent_test),
	KUNIT_CASE(rx_loan_late_consumer_test),
	KUNIT_CASE(rx_loan_stock_region_charge_test),
	KUNIT_CASE(rx_loan_orphan_region_refund_test),
	KUNIT_CASE(rx_loan_grant_region_test),
	KUNIT_CASE(rx_loan_progress_region_test),
	KUNIT_CASE(rx_loan_netns_budget_test),
	KUNIT_CASE(rx_loan_table_alloc_test),
	KUNIT_CASE(rx_loan_table_append_test),
	KUNIT_CASE(rx_loan_table_release_test),
	KUNIT_CASE(rx_loan_table_return_entry_test),
	KUNIT_CASE(rx_loan_skb_region_pages_test),
	KUNIT_CASE(rx_loan_fifo_test),
	KUNIT_CASE(rx_loan_reserve_oldest_test),
	KUNIT_CASE(rx_loan_wake_need_test),
	KUNIT_CASE(rx_loan_orphan_test),
	KUNIT_CASE(rx_loan_orphan_carved_test),
	KUNIT_CASE(rx_loan_orphan_uncarved_test),
	KUNIT_CASE(rx_loan_grant_drop_test),
	KUNIT_CASE(rx_loan_reconfigure_test),
	KUNIT_CASE(rx_loan_stock_test),
	KUNIT_CASE(rx_loan_cost_window_test),
	KUNIT_CASE(rx_loan_copies_test),
	KUNIT_CASE(rx_loan_region_read_pickup_test),
	{}
};

static struct kunit_suite rx_loan_test_suite = {
	.name = "rx_loan",
	.test_cases = rx_loan_test_cases,
};

kunit_test_suite(rx_loan_test_suite);

MODULE_DESCRIPTION("KUnit tests for the rx page-loan helper");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
