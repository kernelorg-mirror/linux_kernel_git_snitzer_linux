// SPDX-License-Identifier: GPL-2.0
/*
 * Receive page loans: the loan table and the receive-region page walk
 * shared by the consumers that hold the pages of received skbs
 * (include/net/rx_loan.h).
 */

#include <kunit/visibility.h>
#include <linux/bitmap.h>
#include <linux/export.h>
#include <linux/minmax.h>
#include <linux/percpu.h>
#include <linux/prefetch.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <net/page_pool/helpers.h>
#include <net/rx_loan.h>
#include <net/xdp.h>

/*
 * Region pages bytes [off, off + len) of @page add to a count whose last
 * page was *@prev.  A receive page that holds several frames is handed over
 * one fragment at a time: only the first of them adds the page.
 */
static u32 rx_loan_region_span(struct page *page, u32 off, u32 len,
			       struct page **prev)
{
	struct page *first = page + (off >> PAGE_SHIFT);
	u32 pages = DIV_ROUND_UP(offset_in_page(off) + len, PAGE_SIZE);
	struct page *last = first + pages - 1;

	if (!page_pool_page_in_rx_region(page))
		return 0;
	if (first == *prev)
		pages--;
	*prev = last;
	return pages;
}

/*
 * Upper bound on the region pages bytes [first, last) of @skb, whose first
 * byte is at @base, add to a count whose last page was *@prev: only pages
 * from a pool bound to the fixed receive region count, and a page counts
 * once however many consecutive fragments lie in it.
 */
u32 rx_loan_skb_region_pages(const struct sk_buff *skb, u32 base, u32 first,
			     u32 last, struct page **prev)
{
	const struct sk_buff *frag_skb;
	u32 cursor = base, pages = 0, lo, hi;
	unsigned int index;

	/* Without the region there is nothing to find: walk no skb. */
	if (!page_pool_rx_region_exists())
		return 0;
	if (max(first, base) >= min(last, base + skb->len))
		return 0;
	if (skb_headlen(skb)) {
		lo = max(first, cursor);
		hi = min(last, cursor + skb_headlen(skb));
		if (lo < hi) {
			const unsigned char *data = skb->data + lo - cursor;

			pages += rx_loan_region_span(virt_to_page(data),
						     offset_in_page(data),
						     hi - lo, prev);
		}
		cursor += skb_headlen(skb);
	}
	for (index = 0; index < skb_shinfo(skb)->nr_frags; index++) {
		const skb_frag_t *frag = &skb_shinfo(skb)->frags[index];
		struct page *page = skb_frag_page(frag);

		lo = max(first, cursor);
		hi = min(last, cursor + skb_frag_size(frag));
		if (lo < hi && page)
			pages += rx_loan_region_span(page,
					skb_frag_off(frag) + lo - cursor,
					hi - lo, prev);
		cursor += skb_frag_size(frag);
	}
	skb_walk_frags(skb, frag_skb) {
		pages += rx_loan_skb_region_pages(frag_skb, cursor, first, last,
						  prev);
		cursor += frag_skb->len;
	}
	return pages;
}
EXPORT_SYMBOL_GPL(rx_loan_skb_region_pages);

/*
 * Fetch, for writing, the struct page of each page that bytes [first,
 * last) of @skb lie in.  A loaned record examines each page's struct page
 * (is it the region's, a page pool's), takes a reference on it, and drops
 * the skb's when the skb is freed: those are this CPU's first touches of
 * lines the ring's CPU wrote last.  Fetched together they overlap, and
 * fetched for writing the reference count's atomics then find the line
 * here.
 */
void rx_loan_prefetch_pages(const struct sk_buff *skb, u32 base, u32 first,
			    u32 last)
{
	const struct sk_buff *frag_skb;
	u32 cursor = base;
	unsigned int index;

	if (max(first, base) >= min(last, base + skb->len))
		return;
	if (skb_headlen(skb)) {
		if (max(first, cursor) < min(last, cursor + skb_headlen(skb)))
			prefetchw(virt_to_page(skb->data));
		cursor += skb_headlen(skb);
	}
	for (index = 0; index < skb_shinfo(skb)->nr_frags; index++) {
		const skb_frag_t *frag = &skb_shinfo(skb)->frags[index];
		struct page *page = skb_frag_page(frag);

		if (page && max(first, cursor) <
			    min(last, cursor + skb_frag_size(frag)))
			prefetchw(page + (skb_frag_off(frag) >> PAGE_SHIFT));
		cursor += skb_frag_size(frag);
	}
	skb_walk_frags(skb, frag_skb) {
		rx_loan_prefetch_pages(frag_skb, cursor, first, last);
		cursor += frag_skb->len;
	}
}
EXPORT_SYMBOL_GPL(rx_loan_prefetch_pages);

/* A table of @entries loan entries, each a bvec and a bit in each map. */
int rx_loan_table_alloc(struct rx_loan_table *t, unsigned long entries,
			gfp_t gfp, int node)
{
	if (!entries || entries > U32_MAX)
		return -EINVAL;
	t->bvec = kcalloc_node(entries, sizeof(*t->bvec), gfp, node);
	if (!t->bvec)
		return -ENOMEM;
	t->borrowed = bitmap_zalloc_node(entries, gfp, node);
	if (!t->borrowed)
		goto out_free_bvec;
	t->page_pool = bitmap_zalloc_node(entries, gfp, node);
	if (!t->page_pool)
		goto out_free_borrowed;
	t->capacity = entries;
	return 0;

out_free_borrowed:
	bitmap_free(t->borrowed);
out_free_bvec:
	kfree(t->bvec);
	t->bvec = NULL;
	t->borrowed = NULL;
	t->page_pool = NULL;
	return -ENOMEM;
}
EXPORT_SYMBOL_GPL(rx_loan_table_alloc);

void rx_loan_table_free(struct rx_loan_table *t)
{
	bitmap_free(t->page_pool);
	bitmap_free(t->borrowed);
	kfree(t->bvec);
}
EXPORT_SYMBOL_GPL(rx_loan_table_free);

/* The counts agree and every borrowed entry holds a page. */
bool rx_loan_table_refs_valid(const struct rx_loan_table *t)
{
	unsigned int index;

	if (!rx_loan_table_counts_valid(t))
		return false;
	if (!t->count)
		return !t->borrowed_count && !t->borrowed_bytes &&
			!t->page_pool_bytes;
	if (bitmap_weight(t->borrowed, t->count) != t->borrowed_count ||
	    !bitmap_subset(t->page_pool, t->borrowed, t->count))
		return false;
	for (index = 0; index < t->count; index++)
		if (test_bit(index, t->borrowed) &&
		    (!t->bvec[index].bv_page ||
		     !t->bvec[index].bv_len ||
		     t->bvec[index].bv_offset >= PAGE_SIZE ||
		     t->bvec[index].bv_len >
			PAGE_SIZE - t->bvec[index].bv_offset))
			return false;
	return true;
}
EXPORT_SYMBOL_GPL(rx_loan_table_refs_valid);

/*
 * Page-pool pages of one record returned to their pool at a time.
 * page_pool_put_netmem_bulk() gathers the pages it recycles into an
 * on-stack array of XDP_BULK_QUEUE_SIZE entries and does not check
 * its bound, so a batch must not be larger.
 */
#define RX_LOAN_PUT_BATCH	XDP_BULK_QUEUE_SIZE

/*
 * Return every borrowed entry of @t, whose references the caller checked
 * with rx_loan_table_refs_valid(): its bytes stay counted.  -EINVAL if
 * the table does not come out empty.
 */
int rx_loan_table_release(struct rx_loan_table *t)
{
	netmem_ref batch[RX_LOAN_PUT_BATCH];
	unsigned int index, nr = 0;

	/* page_pool_put_netmem_bulk() does not check its array's bound. */
	BUILD_BUG_ON(RX_LOAN_PUT_BATCH > XDP_BULK_QUEUE_SIZE);
	/* the lockless fast-path fields share no line with the lock */
	BUILD_BUG_ON(IS_ENABLED(CONFIG_SMP) &&
		     offsetofend(struct rx_loan_budget, stock) >
		     offsetof(struct rx_loan_budget, lock));

	for (index = 0; index < t->count; index++) {
		if (!test_bit(index, t->borrowed))
			continue;
		/*
		 * A record's page-pool pages go back in batches: the pool's
		 * producer lock is taken for each batch and not each page.
		 */
		if (IS_ENABLED(CONFIG_PAGE_POOL) &&
		    test_bit(index, t->page_pool)) {
			batch[nr++] = page_to_netmem(t->bvec[index].bv_page);
			if (nr == ARRAY_SIZE(batch)) {
				page_pool_put_netmem_bulk(batch, nr);
				nr = 0;
			}
		} else {
			rx_loan_put_borrowed(t->bvec[index].bv_page, false);
		}
		__clear_bit(index, t->borrowed);
		__clear_bit(index, t->page_pool);
		t->refs_released++;
		t->borrowed_count--;
	}
	if (nr)
		page_pool_put_netmem_bulk(batch, nr);
	if (WARN_ON_ONCE(t->borrowed_count ||
			 (t->count &&
			  (!bitmap_empty(t->borrowed, t->count) ||
			   !bitmap_empty(t->page_pool, t->count))) ||
			 t->refs_released != t->refs_acquired))
		return -EINVAL;
	return 0;
}
EXPORT_SYMBOL_GPL(rx_loan_table_release);

/* Units that copied bytes rather than lending them all, and those bytes. */
struct rx_loan_copies {
	u64	records;
	u64	bytes;
};

static DEFINE_PER_CPU(struct rx_loan_copies, rx_loan_copies);

/* Sum the copies counter over every CPU. */
void rx_loan_copies_read(u64 *records, u64 *bytes)
{
	int cpu;

	*records = 0;
	*bytes = 0;
	for_each_possible_cpu(cpu) {
		const struct rx_loan_copies *c = per_cpu_ptr(&rx_loan_copies,
							     cpu);

		*records += READ_ONCE(c->records);
		*bytes += READ_ONCE(c->bytes);
	}
}
EXPORT_SYMBOL_GPL(rx_loan_copies_read);

/*
 * Clear @t's counts for its next record and keep its arrays.  Append sets
 * only [0, count); terminal owners clear it before reset.  A unit that
 * copied bytes counts once in the copies counter here.
 */
void rx_loan_table_reset(struct rx_loan_table *t)
{
	unsigned int capacity = t->capacity;
	struct bio_vec *bvec = t->bvec;
	unsigned long *borrowed = t->borrowed;
	unsigned long *page_pool = t->page_pool;

	if (t->copied_bytes) {
		this_cpu_inc(rx_loan_copies.records);
		this_cpu_add(rx_loan_copies.bytes, t->copied_bytes);
	}
	memset(t, 0, sizeof(*t));
	t->capacity = capacity;
	t->bvec = bvec;
	t->borrowed = borrowed;
	t->page_pool = page_pool;
}
EXPORT_SYMBOL_GPL(rx_loan_table_reset);

/* READs holding @lb's credit. */
long rx_loan_read_inflight(struct rx_loan_budget *lb)
{
	long n = 0;
	int cpu;

	for_each_possible_cpu(cpu)
		n += READ_ONCE(per_cpu_ptr(lb->stock, cpu)->read_inflight);
	return n;
}
EXPORT_SYMBOL_GPL(rx_loan_read_inflight);

/* Receive records holding credit from @lb's stocks. */
long rx_loan_stocked_records(struct rx_loan_budget *lb)
{
	long n = 0;
	int cpu;

	for_each_possible_cpu(cpu)
		n += READ_ONCE(per_cpu_ptr(lb->stock, cpu)->records);
	return n;
}
EXPORT_SYMBOL_GPL(rx_loan_stocked_records);

/*
 * Learn @nrec records whose highest sample was @peak, lb->lock held.  The
 * records released without the lock arrive here in batches: the estimate
 * is a maximum, so a batch teaches it what its records one by one would
 * have.  Every window boundary a batch crosses still ends a window, at the
 * same record count as one by one; where the batch's records run on past
 * a boundary, its peak is counted in the window they run on into, so a
 * boundary within a batch can keep the estimate up longer, never bring it
 * down sooner.
 */
void rx_loan_cost_fold_locked(struct rx_loan_budget *lb,
			      unsigned int peak, unsigned int nrec)
{
	unsigned int factor = lb->cost_factor;

	if (!peak || !nrec)
		return;
	lb->cost_peak = max(lb->cost_peak, peak);
	factor = max(factor, peak);
	lb->cost_records += nrec;
	while (lb->cost_records >= RX_LOAN_COST_WINDOW) {
		factor = max(lb->cost_peak, lb->cost_last_peak);
		lb->cost_last_peak = lb->cost_peak;
		lb->cost_records -= RX_LOAN_COST_WINDOW;
		lb->cost_peak = lb->cost_records ? peak : 0;
	}
	WRITE_ONCE(lb->cost_factor, factor);
}
EXPORT_SYMBOL_GPL(rx_loan_cost_fold_locked);

/*
 * Fold the samples of records released to @st without the lock into
 * @lb's estimate, lb->lock held.  The estimate is the highest sample, so
 * a batch's highest sample and its count are all it needs.
 */
void rx_loan_cost_fold_cpu_locked(struct rx_loan_budget *lb,
				  struct rx_loan_stock *st)
{
	u64 v = atomic64_xchg(&st->cost_sample, 0);

	if (v)
		rx_loan_cost_fold_locked(lb, (u32)v, v >> 32);
}
EXPORT_SYMBOL_GPL(rx_loan_cost_fold_cpu_locked);

/*
 * Stop stocking and return every CPU's stocked credit to @lb's avail,
 * lb->lock held.  Pairs with rx_loan_stock_put(): a put either sees
 * contended set or has added its credit before this drain takes it.
 * Stocking resumes at a kick that finds no waiter.  A stocked record's
 * credit lent to a WRITE bounce is not in a stock: its record stays
 * counted until svc_tcp_rx_bounce_done() returns it.
 */
void rx_loan_stock_drain_locked(struct rx_loan_budget *lb)
{
	int cpu;

	WRITE_ONCE(lb->contended, true);
	/* Pairs with rx_loan_stock_put(): set contended, then take stocks. */
	smp_mb();
	for_each_possible_cpu(cpu) {
		struct rx_loan_stock *st = per_cpu_ptr(lb->stock, cpu);

		lb->avail += atomic_long_xchg(&st->pages, 0);
		rx_loan_cost_fold_cpu_locked(lb, st);
	}
}
EXPORT_SYMBOL_GPL(rx_loan_stock_drain_locked);

/*
 * Stock this CPU with a batch while credit is plentiful and nobody waits,
 * lb->lock held.  The reserve is never stocked.  Returns the credit stocked.
 */
long rx_loan_stock_refill_locked(struct rx_loan_budget *lb)
{
	atomic_long_t *pages = &raw_cpu_ptr(lb->stock)->pages;

	rx_loan_cost_fold_cpu_locked(lb, raw_cpu_ptr(lb->stock));
	if (lb->contended || !lb->budget ||
	    lb->avail < 2 * RX_LOAN_STOCK_BATCH + (long)lb->reserve_size ||
	    atomic_long_read(pages) >= RX_LOAN_STOCK_BATCH)
		return 0;
	lb->avail -= RX_LOAN_STOCK_BATCH;
	atomic_long_add(RX_LOAN_STOCK_BATCH, pages);
	return RX_LOAN_STOCK_BATCH;
}
EXPORT_SYMBOL_GPL(rx_loan_stock_refill_locked);

void rx_loan_cost_learn_locked(struct rx_loan_budget *lb, unsigned int sample)
{
	if (lb->cost_warm < RX_LOAN_COST_WARMUP)
		WRITE_ONCE(lb->cost_warm, lb->cost_warm + 1);
	rx_loan_cost_fold_locked(lb, sample, 1);
}
EXPORT_SYMBOL_GPL(rx_loan_cost_learn_locked);

/*
 * Apply @budget and @reserve at once.  Records in flight, and WRITE bounces
 * holding credit (@lent), keep the credit they hold and return it to the new
 * configuration when they end, so with either outstanding the free counts
 * move by the change (and may go negative until they end); with none, they
 * are set outright.  True if anything changed: the caller must then wake the
 * oldest waiter, as nothing else may.
 */
static bool rx_loan_budget_apply_locked(struct rx_loan_budget *lb,
					unsigned int budget,
					unsigned int reserve)
{
	reserve = min(reserve, budget);

	if (budget == lb->budget && reserve == lb->reserve_size)
		return false;
	/*
	 * Stocks hold the old configuration's credit: back to avail first.
	 * Holders of stocked credit count as in flight, or a reset would
	 * forget credit they return later and over-credit the budget.
	 */
	rx_loan_stock_drain_locked(lb);
	if (lb->inflight_count || lb->lent || rx_loan_read_inflight(lb) ||
	    rx_loan_stocked_records(lb)) {
		lb->avail += (long)(budget - reserve) -
			    (long)(lb->budget - lb->reserve_size);
		lb->reserve += (long)reserve - (long)lb->reserve_size;
	} else {
		lb->avail = budget - reserve;
		lb->reserve = reserve;
	}
	lb->budget = budget;
	lb->reserve_size = reserve;
	return true;
}

#if IS_ENABLED(CONFIG_KUNIT)
/* Stand-ins for a region's geometry, so KUnit needs no device. */
static unsigned long rx_loan_kunit_region_pages;
static unsigned long rx_loan_kunit_region_read;

/*
 * Stand in for a region of @pages pages with @read_pages split off for
 * reads (0, 0: the real region, if any).  Nothing is applied: a read pool
 * picks it up at its next locked op.
 */
void rx_loan_kunit_region(unsigned long pages, unsigned long read_pages)
{
	WRITE_ONCE(rx_loan_kunit_region_read, read_pages);
	WRITE_ONCE(rx_loan_kunit_region_pages, pages);
}
EXPORT_SYMBOL_IF_KUNIT(rx_loan_kunit_region);
#endif

unsigned long rx_loan_region_size(void)
{
#if IS_ENABLED(CONFIG_KUNIT)
	if (READ_ONCE(rx_loan_kunit_region_pages))
		return READ_ONCE(rx_loan_kunit_region_pages);
#endif
	return page_pool_rx_region_size();
}
EXPORT_SYMBOL_GPL(rx_loan_region_size);

/* Lockless: called under a budget's spinlock. */
unsigned long rx_loan_region_read_pages(void)
{
#if IS_ENABLED(CONFIG_KUNIT)
	if (READ_ONCE(rx_loan_kunit_region_pages))
		return READ_ONCE(rx_loan_kunit_region_read);
#endif
	return page_pool_rx_region_read_pages();
}
EXPORT_SYMBOL_GPL(rx_loan_region_read_pages);

/*
 * A read pool's size: the read partition when split, else what the
 * configured budgets (@used pages) leave of the shared region, for the
 * READs of transports whose budget is 0.
 */
unsigned int rx_loan_read_pool_pages(unsigned long used)
{
	unsigned int pages = rx_loan_region_read_pages();

	if (!pages) {
		unsigned long region = rx_loan_region_size();

		pages = region > used ? min(region - used,
					    (unsigned long)UINT_MAX) : 0;
	}
	return pages;
}
EXPORT_SYMBOL_GPL(rx_loan_read_pool_pages);

/*
 * Apply the configuration requested of @lb (and, for a read pool, the
 * region's geometry as it is now): true if it changed.
 */
static bool rx_loan_budget_configure_locked(struct rx_loan_budget *lb)
{
	unsigned long pages = READ_ONCE(lb->cfg_pages);
	unsigned int budget, reserve = READ_ONCE(lb->cfg_reserve);

	if (lb->read) {
		budget = rx_loan_read_pool_pages(pages);
		reserve = 0;
	} else {
		budget = pages;
		if (budget && READ_ONCE(lb->cfg_off))
			budget = 0;
	}
	return rx_loan_budget_apply_locked(lb, budget, reserve);
}

/*
 * For a consumer returning credit under @lb's lock outside rx_loan_return()
 * (sunrpc's WRITE bounces): apply the requested configuration first.
 */
bool rx_loan_budget_update_locked(struct rx_loan_budget *lb)
{
	return rx_loan_budget_configure_locked(lb);
}
EXPORT_SYMBOL_GPL(rx_loan_budget_update_locked);

/*
 * Queue @w behind every waiter with an older ticket.  @need is the credit
 * it was refused, or 0 for a record paused in a charge, which any credit
 * moves on.
 */
static void rx_loan_wait_locked(struct rx_loan_budget *lb,
				struct rx_loan_waiter *w, u64 ticket, u32 need)
{
	struct rx_loan_waiter *pos;

	w->need = need;
	if (!list_empty(&w->node))
		return;
	w->ticket = ticket;
	w->budget = lb;
	list_for_each_entry(pos, &lb->waiters, node) {
		if (pos->ticket > ticket) {
			list_add_tail(&w->node, &pos->node);
			return;
		}
	}
	list_add_tail(&w->node, &lb->waiters);
}

/* @w no longer waits. */
static void rx_loan_unwait_locked(struct rx_loan_waiter *w)
{
	if (!list_empty(&w->node))
		list_del_init(&w->node);
	w->ticket = 0;
	w->need = 0;
	w->budget = NULL;
}

static void rx_loan_link_locked(struct rx_loan_budget *lb,
				struct rx_loan_credit *cr)
{
	struct rx_loan_credit *pos;

	list_for_each_entry(pos, &lb->inflight, node) {
		if (pos->ticket > cr->ticket) {
			list_add_tail(&cr->node, &pos->node);
			goto linked;
		}
	}
	list_add_tail(&cr->node, &lb->inflight);
linked:
	cr->linked = true;
	cr->budget = lb;
	lb->inflight_count++;
}

/*
 * Exact credit (a READ).  An exact request takes exact pages, not a
 * receive estimate, and joins the budget's one FIFO: a parked request
 * takes its ticket from next_ticket as a waiting transport does, and
 * returned credit goes to the oldest waiter of either kind.
 */

/*
 * Credit for the exact request @w describes, lb->lock held.  Returns 0,
 * recording it in @w, or -EAGAIN when credit is short and the request must
 * park.  As for a receive record, the oldest request with nothing in
 * flight always starts: it takes avail, then the reserve, and the rest is
 * overrun (progress over accounting; it is never returned).  With no
 * budget a request is unmetered: the free pages alone bound it.
 */
static int rx_loan_grant_locked(struct rx_loan_budget *lb,
				struct rx_loan_waiter *w)
{
	long n = w->need, from_avail = n, from_reserve = 0, over = 0;

	if (lb->budget && lb->avail < n && !lb->contended)
		rx_loan_stock_drain_locked(lb);	/* stocked credit first */
	if (!lb->budget) {
		from_avail = 0;
	} else if (lb->avail < n) {
		if (!list_empty(&lb->inflight) || rx_loan_read_inflight(lb) ||
		    rx_loan_stocked_records(lb))
			return -EAGAIN;
		from_avail = max(lb->avail, 0L);
		from_reserve = min(n - from_avail, max(lb->reserve, 0L));
		over = n - from_avail - from_reserve;
		if (over)
			lb->overrun++;
	}
	lb->avail -= from_avail;
	lb->reserve -= from_reserve;
	w->credit = from_avail;
	w->reserve = from_reserve;
	w->over = over;
	w->budget = lb;
	this_cpu_inc(lb->stock->read_inflight);
	rx_loan_stock_refill_locked(lb);
	return 0;
}

/*
 * Pass returned credit on, lb->lock held: exact requests at the head of
 * the FIFO are granted credit, pinned and moved to @granted, for the caller
 * to wake with rx_loan_wake_granted() once lb->lock is dropped; then the
 * oldest transport, now the head, is pinned for rx_loan_wake(), unless it
 * is @running.  A request that cannot be granted yet blocks everything
 * younger.
 *
 * While a record is in flight, admission takes only from avail, so a
 * waiter refused for want of credit is woken only when avail covers what
 * it asked for: woken sooner, it is refused again, and a thread has
 * visited its transport for nothing.  Every release tests this again, and
 * with nothing in flight the reserve admits the oldest whatever avail is,
 * so no waiter is left behind.  A waiter's pin may decline the wake.
 */
struct rx_loan_waiter *rx_loan_kick_locked(struct rx_loan_budget *lb,
					   struct rx_loan_waiter *running,
					   struct list_head *granted)
{
	struct rx_loan_waiter *head;

	while ((head = list_first_entry_or_null(&lb->waiters,
						struct rx_loan_waiter, node)) &&
	       head->exact) {
		if (rx_loan_grant_locked(lb, head))
			return NULL;
		lb->read_granted++;
		head->ops->pin(head);
		list_move_tail(&head->node, granted);
	}
	/* Nobody waits (and any change is applied): stock again. */
	if (!head) {
		WRITE_ONCE(lb->contended, false);
		return NULL;
	}
	if (head == running)
		return NULL;
	if (lb->budget && lb->avail + lb->reserve <= 0)
		return NULL;
	if (lb->budget && !list_empty(&lb->inflight) &&
	    lb->avail < (long)head->need)
		return NULL;
	return head->ops->pin(head) ? head : NULL;
}
EXPORT_SYMBOL_GPL(rx_loan_kick_locked);

/* Wake the exact requests a kick granted; lb->lock not held. */
void rx_loan_wake_granted(struct list_head *granted)
{
	struct rx_loan_waiter *w, *next;

	list_for_each_entry_safe(w, next, granted, node) {
		list_del_init(&w->node);
		/* may free @w */
		w->ops->wake(w);
	}
}
EXPORT_SYMBOL_GPL(rx_loan_wake_granted);

/* @lb's kick_work: a dropped grant's credit is passed on from here. */
void rx_loan_kick_fn(struct work_struct *work)
{
	struct rx_loan_budget *lb = container_of(work, struct rx_loan_budget,
						 kick_work);
	struct rx_loan_waiter *wake;
	LIST_HEAD(granted);

	spin_lock_bh(&lb->lock);
	rx_loan_budget_configure_locked(lb);
	wake = rx_loan_kick_locked(lb, NULL, &granted);
	spin_unlock_bh(&lb->lock);
	rx_loan_wake(wake);
	rx_loan_wake_granted(&granted);
}
EXPORT_SYMBOL_GPL(rx_loan_kick_fn);

/*
 * Admit a record whose credit @cr is to hold, at a charge of @cost, for the
 * transport whose waiter is @w.  @warm: admission may take stocked credit
 * (rx_loan_cost_warm(), or a fixed factor).  False: @w waits, and is woken,
 * oldest first, when credit returns.
 */
bool rx_loan_admit(struct rx_loan_budget *lb, struct rx_loan_waiter *w,
		   struct rx_loan_credit *cr, u32 cost, bool warm)
{
	struct rx_loan_waiter *head, *wake = NULL, *undo_wake = NULL;
	u32 from_avail, from_reserve;
	bool changed, tried = false;
	LIST_HEAD(granted);

	if (WARN_ON_ONCE(cr->linked))
		return true;
	/*
	 * A holder is counted before it takes stocked credit and uncounted
	 * only once the credit is back, so a change of configuration that
	 * finds no holders (rx_loan_budget_configure_locked) misses none.
	 */
	if (cost <= RX_LOAN_STOCK_MAX && READ_ONCE(lb->budget) &&
	    !READ_ONCE(lb->contended) && list_empty(&w->node) && warm) {
		tried = true;
		this_cpu_inc(lb->stock->records);
		if (rx_loan_stock_take(lb, cost)) {
			/* This CPU's stock: nobody waits, no order to keep. */
			cr->credit = cost;
			cr->reserve = 0;
			cr->over = 0;
			cr->ticket = 0;
			cr->budget = lb;
			cr->stocked = true;
			cr->linked = true;
			this_cpu_inc(lb->stock->admitted);
			return true;
		}
	}
	spin_lock_bh(&lb->lock);
	if (tried) {
		/*
		 * The count held while trying may have kept the oldest waiter
		 * off the reserve: uncount under the lock and let it retry.
		 */
		this_cpu_dec(lb->stock->records);
		undo_wake = rx_loan_kick_locked(lb, NULL, &granted);
	}
	changed = rx_loan_budget_configure_locked(lb);
	if (!lb->budget) {
		/* Disable drains existing waiters through the same FIFO wake chain. */
		rx_loan_unwait_locked(w);
		wake = rx_loan_kick_locked(lb, NULL, &granted);
		goto out;
	}
	head = list_first_entry_or_null(&lb->waiters, struct rx_loan_waiter,
					node);
	if (head && head != w)
		goto deny;
	if (lb->avail < cost && !lb->contended)
		rx_loan_stock_drain_locked(lb);	/* stocked credit first */
	if (list_empty(&lb->inflight) && !rx_loan_stocked_records(lb)) {
		/* The oldest record may use the reserve: it always starts. */
		from_avail = min_t(long, cost, max(lb->avail, 0L));
		from_reserve = min_t(long, cost - from_avail,
				     max(lb->reserve, 0L));
	} else if (lb->avail >= cost) {
		from_avail = cost;
		from_reserve = 0;
	} else {
		goto deny;
	}
	lb->avail -= from_avail;
	lb->reserve -= from_reserve;
	cr->credit = from_avail + from_reserve;
	cr->reserve = from_reserve;
	cr->over = 0;
	cr->ticket = w->ticket ?: lb->next_ticket++;
	cr->stocked = false;
	rx_loan_unwait_locked(w);
	rx_loan_link_locked(lb, cr);
	lb->admitted++;
	if (!from_reserve)
		rx_loan_stock_refill_locked(lb);
	wake = rx_loan_kick_locked(lb, NULL, &granted);
out:
	spin_unlock_bh(&lb->lock);
	rx_loan_wake(undo_wake);
	rx_loan_wake(wake);
	rx_loan_wake_granted(&granted);
	return true;

deny:
	rx_loan_wait_locked(lb, w, w->ticket ?: lb->next_ticket++, cost);
	lb->denied++;
	if (changed)
		wake = rx_loan_kick_locked(lb, w, &granted);
	spin_unlock_bh(&lb->lock);
	rx_loan_wake(undo_wake);
	rx_loan_wake(wake);
	rx_loan_wake_granted(&granted);
	return false;
}
EXPORT_SYMBOL_GPL(rx_loan_admit);

/*
 * The slow path of rx_loan_charge(): an admitted record's credit @cr must
 * grow to @held pages.  False: pause the record; @w is woken when credit
 * returns.
 */
bool __rx_loan_charge(struct rx_loan_waiter *w, struct rx_loan_credit *cr,
		      u32 held)
{
	struct rx_loan_budget *lb = cr->budget;
	struct rx_loan_waiter *wake = NULL;
	LIST_HEAD(granted);
	bool oldest, changed;
	long want, take;

	want = held - cr->credit;
	if (cr->stocked && rx_loan_stock_take(lb, want)) {
		cr->credit += want;
		return true;
	}
	spin_lock_bh(&lb->lock);
	if (cr->stocked) {
		/* Out of stock: an ordinary record from now on, the youngest. */
		cr->ticket = lb->next_ticket++;
		cr->stocked = false;
		rx_loan_link_locked(lb, cr);
		this_cpu_dec(lb->stock->records);
	}
	changed = rx_loan_budget_configure_locked(lb);
	if (!lb->budget) {
		/* Admission was turned off: records in flight finish unthrottled. */
		cr->credit += want;
		cr->over += want;
		want = 0;
	}
	if (want && lb->avail < want && !lb->contended)
		rx_loan_stock_drain_locked(lb);	/* stocked credit first */
	oldest = list_first_entry(&lb->inflight, struct rx_loan_credit,
				  node) == cr;
	take = min(want, max(lb->avail, 0L));
	lb->avail -= take;
	cr->credit += take;
	want -= take;
	if (want && oldest) {
		take = min(want, max(lb->reserve, 0L));
		lb->reserve -= take;
		cr->credit += take;
		cr->reserve += take;
		want -= take;
		if (want) {
			/* Beyond the budget: progress over accounting. */
			cr->credit += want;
			cr->over += want;
			lb->overrun++;
			want = 0;
		}
	}
	if (want) {
		rx_loan_wait_locked(lb, w, cr->ticket, 0);
		lb->paused++;
	} else {
		rx_loan_unwait_locked(w);
	}
	if (changed)
		wake = rx_loan_kick_locked(lb, w, &granted);
	spin_unlock_bh(&lb->lock);
	rx_loan_wake(wake);
	rx_loan_wake_granted(&granted);
	return !want;
}
EXPORT_SYMBOL_GPL(__rx_loan_charge);

/*
 * Return a record's credit @cr, keeping @lend units of it lent on (to a
 * direct-write bounce; never new credit), described in @out: its ordinary
 * credit first, then its reserve; credit past the budget (over) is not
 * converted.  The conversion shares the section that returns the rest, so
 * a bounce costs the budget's lock no more than before.  The caller
 * checked that @lend is covered.  @sample is the record's cost sample
 * (rx_loan_cost_learn_locked()).
 */
void rx_loan_return(struct rx_loan_credit *cr, unsigned int sample, u32 lend,
		    struct rx_loan_lend *out)
{
	struct rx_loan_budget *lb;
	struct rx_loan_waiter *wake;
	LIST_HEAD(granted);

	if (!cr->linked)
		return;
	lb = cr->budget;
	if (cr->stocked) {
		/*
		 * To this CPU's stock of the budget it came from, but for
		 * @lend: a stocked record's credit is all ordinary.  The
		 * record stays counted while a bounce holds its credit.
		 */
		long back;

		WARN_ON_ONCE(lend > cr->credit);
		rx_loan_cost_stash(lb, sample);
		back = rx_loan_stock_put(lb, cr->credit - lend);
		if (lend) {
			this_cpu_add(lb->stock->lent, lend);
			out->budget = lb;
			out->credit = lend;
			out->reserve = 0;
			out->stocked = true;
		}
		cr->credit = 0;
		cr->linked = false;
		cr->stocked = false;
		cr->budget = NULL;
		if (!back) {
			/* Uncounted only once the credit is back: see admit. */
			if (!lend)
				this_cpu_dec(lb->stock->records);
			return;
		}
		spin_lock_bh(&lb->lock);
		lb->avail += back;
		rx_loan_cost_fold_cpu_locked(lb, raw_cpu_ptr(lb->stock));
		if (!lend)
			this_cpu_dec(lb->stock->records);
		goto returned;
	}
	spin_lock_bh(&lb->lock);
	if (lend) {
		u32 normal = min(lend, cr->credit - cr->reserve - cr->over);
		u32 reserve = min(lend - normal, cr->reserve);

		WARN_ON_ONCE(normal + reserve != lend);
		cr->credit -= normal + reserve;
		cr->reserve -= reserve;
		lb->lent += normal + reserve;
		out->budget = lb;
		out->credit = normal;
		out->reserve = reserve;
	}
	list_del(&cr->node);
	lb->inflight_count--;
	lb->reserve += cr->reserve;
	lb->avail += cr->credit - cr->reserve - cr->over;
	rx_loan_cost_learn_locked(lb, sample);
	cr->credit = 0;
	cr->reserve = 0;
	cr->over = 0;
	cr->linked = false;
	cr->budget = NULL;
returned:
	rx_loan_budget_configure_locked(lb);
	wake = rx_loan_kick_locked(lb, NULL, &granted);
	spin_unlock_bh(&lb->lock);
	rx_loan_wake(wake);
	rx_loan_wake_granted(&granted);
}
EXPORT_SYMBOL_GPL(rx_loan_return);

/*
 * A record whose loaned pages could not be accounted for is abandoned, its
 * pages possibly still held: keep its credit spent, but take it out of the
 * in-flight order so the next record can be the oldest and draw the reserve.
 */
void rx_loan_orphan(struct rx_loan_credit *cr)
{
	struct rx_loan_budget *lb;
	struct rx_loan_waiter *wake;
	LIST_HEAD(granted);

	if (!cr->linked)
		return;
	lb = cr->budget;
	spin_lock_bh(&lb->lock);
	if (cr->stocked) {
		/* Not on the in-flight list: only its holder count goes. */
		this_cpu_dec(lb->stock->records);
		cr->stocked = false;
	} else {
		list_del(&cr->node);
		lb->inflight_count--;
	}
	lb->orphaned += cr->credit - cr->over;
	cr->credit = 0;
	cr->reserve = 0;
	cr->over = 0;
	cr->linked = false;
	cr->budget = NULL;
	wake = rx_loan_kick_locked(lb, NULL, &granted);
	spin_unlock_bh(&lb->lock);
	rx_loan_wake(wake);
	rx_loan_wake_granted(&granted);
}
EXPORT_SYMBOL_GPL(rx_loan_orphan);

/* A transport going away stops waiting. */
void rx_loan_unwait(struct rx_loan_waiter *w)
{
	struct rx_loan_budget *lb = READ_ONCE(w->budget);
	struct rx_loan_waiter *wake;
	LIST_HEAD(granted);

	if (!lb)
		return;
	spin_lock_bh(&lb->lock);
	rx_loan_unwait_locked(w);
	wake = rx_loan_kick_locked(lb, NULL, &granted);
	spin_unlock_bh(&lb->lock);
	rx_loan_wake(wake);
	rx_loan_wake_granted(&granted);
}
EXPORT_SYMBOL_GPL(rx_loan_unwait);

/*
 * Exact credit for @w->need pages now, from this CPU's stock or under the
 * lock: 0, recorded in @w, or -EAGAIN when credit is short or anyone
 * waits, for the caller to rx_loan_park() @w (or a copy of it).
 */
int rx_loan_grant(struct rx_loan_budget *lb, struct rx_loan_waiter *w)
{
	struct rx_loan_waiter *wake = NULL, *undo_wake = NULL;
	bool changed, tried = false;
	LIST_HEAD(granted);
	int ret;

	if (w->need <= RX_LOAN_STOCK_MAX && READ_ONCE(lb->budget) &&
	    !READ_ONCE(lb->contended)) {
		tried = true;
		/* Counted first: see rx_loan_admit(). */
		this_cpu_inc(lb->stock->read_inflight);
		if (rx_loan_stock_take(lb, w->need)) {
			/* This CPU's stock: no waiters, no FIFO to respect. */
			w->credit = w->need;
			w->reserve = 0;
			w->over = 0;
			w->budget = lb;
			this_cpu_inc(lb->stock->read_admitted);
			return 0;
		}
	}
	spin_lock_bh(&lb->lock);
	if (tried) {
		this_cpu_dec(lb->stock->read_inflight);
		undo_wake = rx_loan_kick_locked(lb, NULL, &granted);
	}
	changed = rx_loan_budget_configure_locked(lb);
	if (!list_empty(&lb->waiters))
		ret = -EAGAIN;
	else
		ret = rx_loan_grant_locked(lb, w);
	if (!ret)
		this_cpu_inc(lb->stock->read_admitted);
	if (changed)
		wake = rx_loan_kick_locked(lb, NULL, &granted);
	spin_unlock_bh(&lb->lock);
	rx_loan_wake(undo_wake);
	rx_loan_wake(wake);
	rx_loan_wake_granted(&granted);
	return ret;
}
EXPORT_SYMBOL_GPL(rx_loan_grant);

/*
 * Park the exact request @w on @lb until it is granted: its pin and wake
 * run then.  It is linked under the same lock hold that re-checks credit,
 * so a return that raced the first attempt grants it at once.  @w may be
 * freed before this returns.
 */
void rx_loan_park(struct rx_loan_budget *lb, struct rx_loan_waiter *w)
{
	struct rx_loan_waiter *wake;
	LIST_HEAD(granted);

	w->exact = true;
	spin_lock_bh(&lb->lock);
	w->ticket = lb->next_ticket++;
	list_add_tail(&w->node, &lb->waiters);
	lb->read_parked++;
	wake = rx_loan_kick_locked(lb, NULL, &granted);
	spin_unlock_bh(&lb->lock);
	rx_loan_wake(wake);
	rx_loan_wake_granted(&granted);
}
EXPORT_SYMBOL_GPL(rx_loan_park);

/* Return an exact grant's credit (not its overrun) and pass it on. */
void rx_loan_grant_return(struct rx_loan_budget *lb, long credit, long reserve)
{
	struct rx_loan_waiter *wake;
	LIST_HEAD(granted);

	this_cpu_inc(lb->stock->read_completed);
	/* Credit from avail goes to this CPU's stock while nobody waits. */
	if (!reserve && credit) {
		credit = rx_loan_stock_put(lb, credit);
		if (!credit) {
			/* Uncounted only once the credit is back: see admit. */
			this_cpu_dec(lb->stock->read_inflight);
			return;
		}
	}
	spin_lock_bh(&lb->lock);
	lb->avail += credit;
	lb->reserve += reserve;
	this_cpu_dec(lb->stock->read_inflight);
	rx_loan_budget_configure_locked(lb);
	wake = rx_loan_kick_locked(lb, NULL, &granted);
	spin_unlock_bh(&lb->lock);
	rx_loan_wake(wake);
	rx_loan_wake_granted(&granted);
}
EXPORT_SYMBOL_GPL(rx_loan_grant_return);

/*
 * Return the credit of a request granted while parked and dropped before
 * its wake ran.  The drop can run inside a grant chain (the wake of a dead
 * transport's request), and passing credit on from there would recurse, so
 * the kick runs from @lb's kick_work, which the owner flushes.
 */
void rx_loan_grant_drop(struct rx_loan_budget *lb, long credit, long reserve)
{
	spin_lock_bh(&lb->lock);
	lb->avail += credit;
	lb->reserve += reserve;
	this_cpu_dec(lb->stock->read_inflight);
	spin_unlock_bh(&lb->lock);
	this_cpu_inc(lb->stock->read_completed);
	schedule_work(&lb->kick_work);
}
EXPORT_SYMBOL_GPL(rx_loan_grant_drop);

/* Request @pages and @reserve of @lb and apply them now, waking as they allow. */
/* Apply the configuration last staged on @lb now, and wake who it admits. */
void rx_loan_budget_reconfigure(struct rx_loan_budget *lb)
{
	struct rx_loan_waiter *wake = NULL;
	LIST_HEAD(granted);

	spin_lock_bh(&lb->lock);
	if (rx_loan_budget_configure_locked(lb))
		wake = rx_loan_kick_locked(lb, NULL, &granted);
	spin_unlock_bh(&lb->lock);
	rx_loan_wake(wake);
	rx_loan_wake_granted(&granted);
}
EXPORT_SYMBOL_GPL(rx_loan_budget_reconfigure);

void rx_loan_budget_request(struct rx_loan_budget *lb, unsigned long pages,
			    unsigned int reserve, bool off)
{
	spin_lock_bh(&lb->lock);
	rx_loan_budget_stage_locked(lb, pages, reserve, off);
	spin_unlock_bh(&lb->lock);
	rx_loan_budget_reconfigure(lb);
}
EXPORT_SYMBOL_GPL(rx_loan_budget_request);
