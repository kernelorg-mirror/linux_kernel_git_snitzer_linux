/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Receive page loans: a consumer that parses records out of a TCP receive
 * queue holds the pages its skbs carry, instead of copying them, until the
 * record completes.  This is the transport-neutral part of that: the loan
 * table a record's pages are held in, the walk that counts the pages
 * of the fixed receive region a record holds, and the budget of those
 * pages that records draw credit from.
 */

#ifndef _NET_RX_LOAN_H
#define _NET_RX_LOAN_H

#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/cache.h>
#include <linux/bvec.h>
#include <linux/gfp_types.h>
#include <linux/mm.h>
#include <linux/list.h>
#include <linux/minmax.h>
#include <linux/overflow.h>
#include <linux/percpu.h>
#include <linux/skbuff_ref.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include <net/page_pool/helpers.h>

struct net;
struct rx_loan_consumer;
struct sk_buff;

/*
 * The pages of one record: one bvec per received fragment, each either
 * borrowed (a reference on the skb's page) or copied into a page the
 * consumer owns.
 */
struct rx_loan_table {
	struct bio_vec		*bvec;
	unsigned long		*borrowed;
	unsigned long		*page_pool;
	u32			capacity;
	u32			count;
	u32			body_bytes;
	u32			borrowed_bytes;
	u32			copied_bytes;
	u32			page_pool_bytes;
	u32			refs_acquired;
	u32			refs_released;
	u32			borrowed_count;
	/*
	 * region pages loaned, an upper bound on those held: a page counts
	 * again only after a loan of another one (@region_last is the last)
	 */
	u32			region_count;
	struct page		*region_last;
};

u32 rx_loan_skb_region_pages(const struct sk_buff *skb, u32 base, u32 first,
			     u32 last, struct page **prev);
void rx_loan_prefetch_pages(const struct sk_buff *skb, u32 base, u32 first,
			    u32 last);

int rx_loan_table_alloc(struct rx_loan_table *t, unsigned long entries,
			gfp_t gfp, int node);
void rx_loan_table_free(struct rx_loan_table *t);
bool rx_loan_table_refs_valid(const struct rx_loan_table *t);
int rx_loan_table_release(struct rx_loan_table *t);
void rx_loan_table_reset(struct rx_loan_table *t);
void rx_loan_copies_read(u64 *records, u64 *bytes);

/* The table's counts agree; checks no entry. */
static inline bool rx_loan_table_counts_valid(const struct rx_loan_table *t)
{
	return t->count <= t->capacity &&
	       t->refs_released <= t->refs_acquired &&
	       t->refs_acquired - t->refs_released == t->borrowed_count &&
	       t->borrowed_count <= t->count &&
	       t->page_pool_bytes <= t->borrowed_bytes;
}

static inline bool rx_loan_table_can_coalesce(const struct rx_loan_table *t,
					      struct page *page,
					      unsigned int offset,
					      bool borrowed, bool page_pool)
{
	const struct bio_vec *last;
	unsigned int index;

	if (!t->count)
		return false;
	index = t->count - 1;
	last = &t->bvec[index];
	return test_bit(index, t->borrowed) == borrowed &&
		test_bit(index, t->page_pool) == page_pool &&
		last->bv_page == page &&
		last->bv_offset + last->bv_len == offset;
}

/*
 * A borrowed page-pool page from a recycling skb is held the way the skb
 * holds it, by a page-pool reference, so releasing the loan recycles the
 * page into its pool.  A plain page reference would make the pool eject the
 * page when the skb is freed.  Every other borrowed page takes a page
 * reference.
 */
static inline void rx_loan_get_borrowed(struct page *page, bool page_pool)
{
#if IS_ENABLED(CONFIG_PAGE_POOL)
	if (page_pool) {
		page_pool_ref_netmem(netmem_compound_head(page_to_netmem(page)));
		return;
	}
#endif
	get_page(page);
}

static inline void rx_loan_put_borrowed(struct page *page, bool page_pool)
{
	skb_page_unref(page_to_netmem(page), page_pool);
}

/* Return loan entry @index; its bytes stay counted, the record ends next. */
static inline void rx_loan_table_return_entry(struct rx_loan_table *t,
					      unsigned int index)
{
	rx_loan_put_borrowed(t->bvec[index].bv_page,
			     test_bit(index, t->page_pool));
	__clear_bit(index, t->borrowed);
	__clear_bit(index, t->page_pool);
	t->refs_released++;
	t->borrowed_count--;
}

/* Can bytes [@offset, @offset + @len) of @page be appended to @t? */
static inline bool rx_loan_table_append_ok(const struct rx_loan_table *t,
					   struct page *page,
					   unsigned int offset,
					   unsigned int len, bool borrowed,
					   bool page_pool)
{
	u32 body_bytes;

	return page && len && (!page_pool || borrowed) &&
	       offset < PAGE_SIZE && len <= PAGE_SIZE - offset &&
	       !check_add_overflow(t->body_bytes, len, &body_bytes);
}

/* Append, once rx_loan_table_append_ok() said it may. */
static inline int __rx_loan_table_append(struct rx_loan_table *t,
					 struct page *page,
					 unsigned int offset,
					 unsigned int len, bool borrowed,
					 bool page_pool)
{
	struct bio_vec *bvec;

	if (rx_loan_table_can_coalesce(t, page, offset, borrowed, page_pool)) {
		bvec = &t->bvec[t->count - 1];
		bvec->bv_len += len;
	} else {
		if (t->count == t->capacity)
			return -ENOSPC;
		bvec = &t->bvec[t->count];
		if (borrowed)
			rx_loan_get_borrowed(page, page_pool);
		bvec_set_page(bvec, page, len, offset);
		if (borrowed) {
			__set_bit(t->count, t->borrowed);
			t->borrowed_count++;
			t->refs_acquired++;
			if (page_pool && page_pool_page_in_rx_region(page) &&
			    page != t->region_last) {
				t->region_count++;
				t->region_last = page;
			}
		}
		if (page_pool)
			__set_bit(t->count, t->page_pool);
		t->count++;
	}
	t->body_bytes += len;
	if (borrowed) {
		t->borrowed_bytes += len;
		if (page_pool)
			t->page_pool_bytes += len;
	} else {
		t->copied_bytes += len;
	}
	return 0;
}

static inline int rx_loan_table_append(struct rx_loan_table *t,
				       struct page *page, unsigned int offset,
				       unsigned int len, bool borrowed,
				       bool page_pool)
{
	if (!rx_loan_table_append_ok(t, page, offset, len, borrowed,
				     page_pool))
		return -EINVAL;
	return __rx_loan_table_append(t, page, offset, len, borrowed,
				      page_pool);
}

/*
 * Credit a CPU keeps while its budget has no waiters, so admitting a record
 * or a READ and returning its credit rarely take the budget's lock.
 * Stocked credit is still the budget's own: the stats count it as
 * available, and the first waiter or a change of configuration drains every
 * stock back under the lock.  Holders are counted per CPU, before they take
 * stocked credit and until it is back, so the budget's "anything in
 * flight" tests see them.  Credit always goes back to the budget it came
 * from (state->credit_budget, a READ's budget), never across namespaces.
 */
#define RX_LOAN_STOCK_MAX		64
#define RX_LOAN_STOCK_BATCH	32

struct rx_loan_stock {
	atomic_long_t		pages;
	/* receive records admitted from a stock: off the in-flight list */
	long			records;
	long			read_inflight;	/* READs admitted less returned here */
	long			lent;	/* stocked records' credit lent to bounces */
	u64			admitted;	/* records admitted from the stock */
	u64			read_admitted;
	u64			read_completed;	/* credit returned, however it ended */
	/*
	 * Cost samples of records released to this stock without the lock:
	 * the count in the high 32 bits, the highest sample in the low 32.
	 * Folded into the budget's estimate under its lock at the next
	 * refill, drain or spill (rx_loan_cost_fold_cpu_locked()).
	 */
	atomic64_t		cost_sample;
};

/*
 * A budget of receive pages that loaned records draw credit from.  Every
 * record's lookup of its budget reads the requested configuration, which
 * changes only with the configuration; admission and release write lock,
 * avail and what follows for every loaned record.  They are kept on
 * separate cache lines, so that a lookup does not take a line another CPU
 * has just written.  A consumer embeds the budget first in its own.
 */
struct rx_loan_budget {
	/* the read partition's pool */
	bool			read;
	/*
	 * Requested configuration, applied at the next locked op: pages and
	 * reserve; for the read pool, what the configured budgets take of a
	 * shared region.  cfg_off turns a budget off (sunrpc's receive clamp).
	 */
	bool			cfg_off;
	/*
	 * A namespace's write budget nobody configured: with cfg_pages 0 it
	 * takes the region's write share (rx_loan_region_write_default())
	 * once a region exists, rather than 0, which copies.
	 */
	bool			cfg_default;
	unsigned int		cfg_reserve;
	unsigned long		cfg_pages;
	/* the consumer's object this budget belongs to (read-mostly) */
	void			*owner;
	/*
	 * Read without the lock on the per-CPU fast path: on this
	 * read-mostly line, away from the lock and the counters written
	 * under it.
	 */
	bool			contended;	/* waiters or a drain: no stocking */
	struct rx_loan_stock __percpu *stock;
	spinlock_t		lock ____cacheline_aligned_in_smp;
	long			avail;
	long			reserve;
	unsigned int		budget;		/* configuration in force */
	unsigned int		reserve_size;
	unsigned int		inflight_count;
	u64			next_ticket;
	/*
	 * Region pages a record held per 100 pages of record, learned from
	 * the records that ended; 0 until one has.  It is the most any of
	 * the last RX_LOAN_COST_WINDOW records showed, and of this
	 * window's so far (rx_loan_cost_learn_locked()).
	 */
	unsigned int		cost_factor;
	/*
	 * What the skbs of one of its transports last occupied for each
	 * 100 bytes of payload: where a new transport's receive buffer is
	 * held until it has measured its own.  Written without the lock;
	 * any transport's figure will do.
	 */
	unsigned int		hold_factor;
	unsigned int		cost_peak;	/* of this window */
	unsigned int		cost_last_peak;	/* of the window before */
	unsigned int		cost_records;	/* in this window */
	/*
	 * Records released under the lock since nothing was learned, to
	 * RX_LOAN_COST_WARMUP: until then admission takes no stock, so
	 * the first records teach the estimate on the locked path.
	 */
	unsigned int		cost_warm;
	struct list_head	inflight;	/* admitted records, oldest first */
	/* waiting transports and parked exact requests, oldest ticket first */
	struct list_head	waiters;
	u64			admitted;
	u64			denied;
	u64			paused;
	u64			overrun;
	u64			orphaned;	/* pages kept spent by abandoned records */
	unsigned int		lent;		/* credit converted to WRITE bounces */
	/* READs in flight, admitted and completed: per CPU, in @stock */
	u64			read_parked;
	u64			read_granted;
	struct work_struct	kick_work;	/* rx_loan_kick_fn(); the owner flushes it */
	/* on the budgets a region return kicks (rx_loan_budget_init()) */
	struct list_head	region_node;
};

#define RX_LOAN_COST_WINDOW	1024
#define RX_LOAN_COST_WARMUP	64

struct rx_loan_waiter;

struct rx_loan_waiter_ops {
	/* lb->lock held: hold @w for its wake; false declines the wake */
	bool (*pin)(struct rx_loan_waiter *w);
	/* every lock dropped, the last touch of @w, which it may free */
	void (*wake)(struct rx_loan_waiter *w);
};

/*
 * A waiter on a budget's one FIFO: a transport refused admission or paused
 * in a charge, woken to retry, or a parked exact request (a READ), granted
 * its credit under the lock before it is woken.
 */
struct rx_loan_waiter {
	struct list_head	node;	/* on the budget's waiters */
	u64			ticket;
	u32			need;	/* 0: paused in a charge */
	bool			exact;	/* granted under the lock, not retried */
	/* refused by its consumer's share alone: rx_loan_share_waits() */
	bool			share;
	struct rx_loan_budget	*budget;	/* waited on, or granted by */
	/* an exact request's consumer (rx_loan_grant(), rx_loan_park()) */
	struct rx_loan_consumer	*consumer;
	const struct rx_loan_waiter_ops *ops;
	/* an exact grant: from avail, from the reserve, beyond the budget */
	u32			credit;
	u32			reserve;
	u32			over;
};

/* The credit an admitted record holds. */
struct rx_loan_credit {
	u64			ticket;
	u32			credit;
	u32			reserve;
	u32			over;
	/* of credit, what was charged while a region was in place */
	u32			region;
	bool			linked;
	/* admitted from a CPU's stock: off the budget's in-flight list */
	bool			stocked;
	struct list_head	node;
	/* the budget this credit came from, while linked */
	struct rx_loan_budget	*budget;
	struct rx_loan_consumer	*consumer;	/* charged, or NULL */
};

/* Credit a returning record lent on (rx_loan_return()). */
struct rx_loan_lend {
	struct rx_loan_budget	*budget;
	/* charged with the lent credit until the borrower returns it */
	struct rx_loan_consumer	*consumer;
	u32			credit;
	u32			reserve;
	bool			stocked;
};

bool rx_loan_budget_update_locked(struct rx_loan_budget *lb);
unsigned long rx_loan_region_size(void);
unsigned long rx_loan_region_read_pages(void);
unsigned int rx_loan_read_pool_pages(unsigned long used);
#if IS_ENABLED(CONFIG_KUNIT)
void rx_loan_kunit_region_set_aside(bool aside);
void rx_loan_kunit_region(unsigned long pages, unsigned long read_pages);
long rx_loan_kunit_region_denied(void);
#endif
/*
 * A user of region loans (sunrpc's server, nvmet-tcp's port): registered
 * while it serves, so that the region is carved for its floor and its share
 * of the region is set.  The share is the region's pages over the consumers
 * registered (rx_loan_consumer_share()); a late consumer gets its share of
 * the region that exists, which never grows, and the others' shrink to
 * theirs with nothing taken back.  Credit the consumer holds is counted in
 * @out_write and @out_read, one atomic per record or READ, and an admission
 * that would take either past the share waits, unless its record is the
 * budget's oldest (progress; counted in @overrun_share).  The region-wide
 * total (rx_loan_region_out()) is a hard cap on every budget of every
 * namespace together; nothing passes it.
 */
struct rx_loan_consumer {
	const char		*name;
	struct list_head	node;
	struct rx_loan_net	*ln;	/* its namespace's budgets, or NULL */
	unsigned long		floor;	/* pages asked of the region */
	bool			registered;
	atomic_long_t		out_write ____cacheline_aligned_in_smp;
	atomic_long_t		out_read;
	atomic_long_t		overrun_share;	/* oldest records past the share */
	atomic_long_t		share_denied;
};

/*
 * A namespace's receive-loan budgets, shared by every consumer in it: one
 * for receive records (write) and one for exact requests (READ), sized to
 * the full read pool.  Every namespace's budgets draw on the one region,
 * whose outstanding total is the hard cap.
 */
struct rx_loan_net {
	struct rx_loan_budget	write;
	struct rx_loan_budget	read;
	unsigned int		nr_consumers;	/* under the registration mutex */
};

struct rx_loan_net *rx_loan_net(const struct net *net);
int rx_loan_net_state_init(struct rx_loan_net *ln);
void rx_loan_net_state_exit(struct rx_loan_net *ln);
void rx_loan_net_write_request(struct rx_loan_net *ln, unsigned long pages,
			       unsigned int reserve, bool off);
int rx_loan_budget_init(struct rx_loan_budget *lb, bool read);
void rx_loan_budget_destroy(struct rx_loan_budget *lb);

int rx_loan_consumer_register(struct rx_loan_consumer *c,
			      struct rx_loan_net *ln, const char *name,
			      unsigned long floor);
void rx_loan_consumer_unregister(struct rx_loan_consumer *c);
void rx_loan_consumer_set_floor(struct rx_loan_consumer *c,
				unsigned long floor);
void rx_loan_region_carve(void);
unsigned long rx_loan_region_write_default(void);
unsigned long rx_loan_consumer_share(const struct rx_loan_consumer *c);

long rx_loan_region_out(void);
long rx_loan_region_orphaned(void);

void rx_loan_budget_request(struct rx_loan_budget *lb, unsigned long pages,
			    unsigned int reserve, bool off);
void rx_loan_budget_reconfigure(struct rx_loan_budget *lb);

/*
 * Request a configuration of @lb without applying it: its next locked op
 * does (rx_loan_budget_reconfigure() applies it now).  Under lb->lock, so
 * that a caller deriving the values from its sources under the lock
 * cannot overwrite a later request with an earlier one.
 */
static inline void rx_loan_budget_stage_locked(struct rx_loan_budget *lb,
					       unsigned long pages,
					       unsigned int reserve, bool off)
{
	lockdep_assert_held(&lb->lock);
	WRITE_ONCE(lb->cfg_pages, pages);
	WRITE_ONCE(lb->cfg_reserve, reserve);
	WRITE_ONCE(lb->cfg_off, off);
}
struct rx_loan_waiter *rx_loan_kick_locked(struct rx_loan_budget *lb,
					   struct rx_loan_waiter *running,
					   struct list_head *granted);
void rx_loan_wake_granted(struct list_head *granted);
void rx_loan_kick_fn(struct work_struct *work);
bool rx_loan_admit(struct rx_loan_budget *lb, struct rx_loan_consumer *c,
		   struct rx_loan_waiter *w, struct rx_loan_credit *cr,
		   u32 cost, bool warm);
bool __rx_loan_charge(struct rx_loan_waiter *w, struct rx_loan_credit *cr,
		      u32 held);
void rx_loan_return(struct rx_loan_credit *cr, unsigned int sample, u32 lend,
		    struct rx_loan_lend *out);
void rx_loan_orphan(struct rx_loan_credit *cr, u32 held);
void rx_loan_consumer_put(struct rx_loan_consumer *c, bool read, long n);
void rx_loan_unwait(struct rx_loan_waiter *w);
int rx_loan_grant(struct rx_loan_budget *lb, struct rx_loan_consumer *c,
		  struct rx_loan_waiter *w);
void rx_loan_park(struct rx_loan_budget *lb, struct rx_loan_consumer *c,
		  struct rx_loan_waiter *w);
void rx_loan_grant_return(struct rx_loan_budget *lb, struct rx_loan_consumer *c,
			  long credit, long reserve);
void rx_loan_grant_drop(struct rx_loan_budget *lb, struct rx_loan_consumer *c,
			long credit, long reserve);

/* Wake the waiter a kick pinned, if any; no lock held. */
static inline void rx_loan_wake(struct rx_loan_waiter *w)
{
	if (w)
		w->ops->wake(w);
}

/* @w waits for credit. */
static inline bool rx_loan_waiting(const struct rx_loan_waiter *w)
{
	return !list_empty_careful(&w->node);
}

/*
 * Before an admitted record consumes an skb: make sure its credit @cr covers
 * @held region pages.  False: pause the record; @w is woken when credit
 * returns.
 */
static inline bool rx_loan_charge(struct rx_loan_waiter *w,
				  struct rx_loan_credit *cr, u32 held)
{
	if (!cr->linked || held <= cr->credit)
		return true;
	return __rx_loan_charge(w, cr, held);
}

long rx_loan_read_inflight(struct rx_loan_budget *lb);
long rx_loan_stocked_records(struct rx_loan_budget *lb);
void rx_loan_cost_fold_locked(struct rx_loan_budget *lb, unsigned int peak,
			      unsigned int nrec);
void rx_loan_cost_fold_cpu_locked(struct rx_loan_budget *lb,
				  struct rx_loan_stock *st);
void rx_loan_cost_learn_locked(struct rx_loan_budget *lb, unsigned int sample);
void rx_loan_stock_drain_locked(struct rx_loan_budget *lb);
long rx_loan_stock_refill_locked(struct rx_loan_budget *lb);
bool rx_loan_region_take(long pages, bool *carved);
void rx_loan_region_put(long pages);

/*
 * Take @n pages of credit from this CPU's stock, with no lock.  The caller
 * has counted itself as a holder (records or read_inflight) first.  A stock
 * is budget credit, not region pages: what is taken is charged to the
 * region here, one atomic per record, and goes back to the stock if the
 * region has not got it.  @carved, if not NULL: whether a region was in
 * place to charge.
 */
static inline bool __rx_loan_stock_take(struct rx_loan_budget *lb, long n,
					bool *carved)
{
	atomic_long_t *pages = &raw_cpu_ptr(lb->stock)->pages;
	long v;

	if (READ_ONCE(lb->contended))
		return false;
	v = atomic_long_read(pages);
	do {
		if (v < n)
			return false;
	} while (!atomic_long_try_cmpxchg(pages, &v, v - n));
	if (likely(rx_loan_region_take(n, carved)))
		return true;
	atomic_long_add(n, pages);
	return false;
}

static inline bool rx_loan_stock_take(struct rx_loan_budget *lb, long n)
{
	return __rx_loan_stock_take(lb, n, NULL);
}

/*
 * Return @n pages of credit to this CPU's stock, and so @n pages to the
 * region, which charged them at rx_loan_stock_take().  Returns what must go
 * to @lb's avail under its lock instead (already refunded to the region):
 * all of it while @lb is contended, else what a full stock spills (all but
 * a batch).
 */
static inline long rx_loan_stock_put(struct rx_loan_budget *lb, long n)
{
	atomic_long_t *pages = &raw_cpu_ptr(lb->stock)->pages;
	long v, keep;

	rx_loan_region_put(n);
	if (READ_ONCE(lb->contended))
		return n;
	v = atomic_long_add_return(n, pages);
	/* Pairs with rx_loan_stock_drain_locked(): see there. */
	smp_mb__after_atomic();
	keep = READ_ONCE(lb->contended) ? 0 : RX_LOAN_STOCK_BATCH;
	if (!keep || v > RX_LOAN_STOCK_MAX) {
		v = atomic_long_read(pages);
		do {
			if (v <= keep)
				return 0;
		} while (!atomic_long_try_cmpxchg(pages, &v, keep));
		return v - keep;
	}
	return 0;
}

/*
 * A record admitted from a stock has ended and its credit went back to the
 * stock without the lock: keep its @sample on this CPU for the next fold.
 */
static inline void rx_loan_cost_stash(struct rx_loan_budget *lb,
				      unsigned int sample)
{
	atomic64_t *acc = &raw_cpu_ptr(lb->stock)->cost_sample;
	u64 v, n;

	if (!sample)
		return;
	v = atomic64_read(acc);
	do {
		n = ((v >> 32) + 1) << 32 | max_t(u32, (u32)v, sample);
		if ((v >> 32) == U32_MAX)
			n = v | max_t(u32, (u32)v, sample);
	} while (!atomic64_try_cmpxchg(acc, &v, n));
}

/*
 * Admission takes stocked credit only once @lb has learned from
 * RX_LOAN_COST_WARMUP records on the locked path, a stock's worth: the
 * stock then charges what those records showed, not all a record can hold,
 * and its own records keep teaching the estimate through the fold.
 */
static inline bool rx_loan_cost_warm(const struct rx_loan_budget *lb)
{
	return READ_ONCE(lb->cost_warm) >= RX_LOAN_COST_WARMUP;
}

#endif /* _NET_RX_LOAN_H */
