// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the receive page-loan helper (net/core/rx_loan.c).
 * The consumers' own suites (net/sunrpc/svcsock_kunit.c) test it as they
 * drive it; these need no consumer.
 */

#include <kunit/test.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/skbuff.h>
#include <linux/workqueue.h>
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
	KUNIT_ASSERT_NOT_NULL(test, lb->stock);
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
	KUNIT_ASSERT_TRUE(test, rx_loan_admit(lb, &a.w, &a.cr, 12, false));
	KUNIT_ASSERT_FALSE(test, rx_loan_admit(lb, &b.w, &b.cr, 8, false));
	KUNIT_EXPECT_TRUE(test, rx_loan_waiting(&b.w));
	KUNIT_EXPECT_EQ(test, rx_loan_grant(lb, &p.w), -EAGAIN);
	rx_loan_park(lb, &p.w);
	KUNIT_EXPECT_EQ(test, lb->read_parked, 1ULL);
	KUNIT_EXPECT_GT(test, p.w.ticket, b.w.ticket);
	KUNIT_EXPECT_EQ(test, b.woken + p.woken, 0);

	/* A return wakes the oldest, the transport; the READ stays behind. */
	rx_loan_kunit_return(&a);
	KUNIT_EXPECT_EQ(test, b.woken, 1);
	KUNIT_EXPECT_EQ(test, p.woken, 0);

	/* The transport's admission passes the head on to the READ. */
	KUNIT_ASSERT_TRUE(test, rx_loan_admit(lb, &b.w, &b.cr, 8, false));
	KUNIT_EXPECT_FALSE(test, rx_loan_waiting(&b.w));
	KUNIT_EXPECT_EQ(test, p.pinned, 1);
	KUNIT_EXPECT_EQ(test, p.woken, 1);
	KUNIT_EXPECT_EQ(test, p.w.credit, 1U);
	KUNIT_EXPECT_LT(test, b.order, p.order);
	KUNIT_EXPECT_EQ(test, lb->read_granted, 1ULL);
	KUNIT_EXPECT_TRUE(test, list_empty(&lb->waiters));

	rx_loan_grant_return(lb, p.w.credit, p.w.reserve);
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
	KUNIT_ASSERT_TRUE(test, rx_loan_admit(lb, &a.w, &a.cr, 14, false));
	KUNIT_EXPECT_EQ(test, a.cr.credit, 14U);
	KUNIT_EXPECT_EQ(test, a.cr.reserve, 2U);
	KUNIT_EXPECT_EQ(test, lb->reserve, 2L);
	KUNIT_ASSERT_FALSE(test, rx_loan_admit(lb, &b.w, &b.cr, 1, false));

	/* The oldest grows from the reserve, then past the budget. */
	KUNIT_EXPECT_TRUE(test, rx_loan_charge(&a.w, &a.cr, 16));
	KUNIT_EXPECT_EQ(test, a.cr.reserve, 4U);
	KUNIT_EXPECT_TRUE(test, rx_loan_charge(&a.w, &a.cr, 18));
	KUNIT_EXPECT_EQ(test, a.cr.over, 2U);
	KUNIT_EXPECT_EQ(test, lb->overrun, 1ULL);

	rx_loan_kunit_return(&a);
	KUNIT_EXPECT_EQ(test, b.woken, 1);
	KUNIT_EXPECT_EQ(test, lb->reserve, 4L);
	KUNIT_ASSERT_TRUE(test, rx_loan_admit(lb, &b.w, &b.cr, 1, false));
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
	KUNIT_ASSERT_TRUE(test, rx_loan_admit(lb, &a.w, &a.cr, 10, false));
	KUNIT_ASSERT_TRUE(test, rx_loan_admit(lb, &c.w, &c.cr, 4, false));
	KUNIT_ASSERT_FALSE(test, rx_loan_admit(lb, &b.w, &b.cr, 7, false));
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

/* An orphan stays spent but leaves the in-flight order. */
static void rx_loan_orphan_test(struct kunit *test)
{
	struct rx_loan_budget *lb = rx_loan_kunit_budget(test, 16, 4);
	struct rx_loan_kunit_waiter a, b;

	rx_loan_kunit_waiter_init(&a, false, 0);
	rx_loan_kunit_waiter_init(&b, false, 0);
	KUNIT_ASSERT_TRUE(test, rx_loan_admit(lb, &a.w, &a.cr, 12, false));
	KUNIT_ASSERT_FALSE(test, rx_loan_admit(lb, &b.w, &b.cr, 1, false));
	rx_loan_orphan(&a.cr);
	KUNIT_EXPECT_FALSE(test, a.cr.linked);
	KUNIT_EXPECT_EQ(test, lb->orphaned, 12ULL);
	KUNIT_EXPECT_EQ(test, lb->inflight_count, 0U);
	KUNIT_EXPECT_EQ(test, b.woken, 1);

	/* Nothing in flight: the next record is the oldest, on the reserve. */
	KUNIT_ASSERT_TRUE(test, rx_loan_admit(lb, &b.w, &b.cr, 1, false));
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
	KUNIT_ASSERT_EQ(test, rx_loan_grant(lb, &g.w), 0);
	KUNIT_EXPECT_EQ(test, g.w.credit, 16U);
	KUNIT_EXPECT_EQ(test, rx_loan_grant(lb, &p.w), -EAGAIN);
	rx_loan_park(lb, &p.w);
	KUNIT_EXPECT_EQ(test, p.woken, 0);

	rx_loan_grant_drop(lb, g.w.credit, g.w.reserve);
	flush_work(&lb->kick_work);
	KUNIT_EXPECT_EQ(test, p.woken, 1);
	KUNIT_EXPECT_EQ(test, p.w.credit, 1U);
	KUNIT_EXPECT_EQ(test, lb->read_granted, 1ULL);
	rx_loan_grant_return(lb, p.w.credit, p.w.reserve);
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
	KUNIT_ASSERT_TRUE(test, rx_loan_admit(lb, &a.w, &a.cr, 10, false));
	KUNIT_ASSERT_FALSE(test, rx_loan_admit(lb, &b.w, &b.cr, 20, false));

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

	KUNIT_ASSERT_TRUE(test, rx_loan_admit(lb, &b.w, &b.cr, 20, false));
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
	KUNIT_ASSERT_TRUE(test, rx_loan_admit(lb, &a.w, &a.cr, 4, false));
	KUNIT_EXPECT_EQ(test, lb->budget, 24U);
	KUNIT_EXPECT_EQ(test, lb->avail, 20L);
	rx_loan_kunit_return(&a);

	/* Unsplit, a read pool has what the configured budgets leave. */
	rx_loan_kunit_region(48, 0);
	KUNIT_EXPECT_EQ(test, rx_loan_read_pool_pages(40), 8U);
	KUNIT_EXPECT_EQ(test, rx_loan_read_pool_pages(48), 0U);
}

static struct kunit_case rx_loan_test_cases[] = {
	KUNIT_CASE(rx_loan_table_alloc_test),
	KUNIT_CASE(rx_loan_table_append_test),
	KUNIT_CASE(rx_loan_table_release_test),
	KUNIT_CASE(rx_loan_table_return_entry_test),
	KUNIT_CASE(rx_loan_skb_region_pages_test),
	KUNIT_CASE(rx_loan_fifo_test),
	KUNIT_CASE(rx_loan_reserve_oldest_test),
	KUNIT_CASE(rx_loan_wake_need_test),
	KUNIT_CASE(rx_loan_orphan_test),
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
