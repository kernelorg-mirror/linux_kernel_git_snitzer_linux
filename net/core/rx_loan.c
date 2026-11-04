// SPDX-License-Identifier: GPL-2.0
/*
 * Receive page loans: the loan table and the receive-region page walk
 * shared by the consumers that hold the pages of received skbs
 * (include/net/rx_loan.h).
 */

#include <kunit/visibility.h>
#include <linux/bitmap.h>
#include <linux/export.h>
#include <linux/highmem.h>
#include <linux/math64.h>
#include <linux/minmax.h>
#include <linux/mutex.h>
#include <linux/percpu.h>
#include <linux/prefetch.h>
#include <linux/rculist.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <net/net_namespace.h>
#include <net/netns/generic.h>
#include <net/page_pool/helpers.h>
#include <net/rx_loan.h>
#include <net/sock.h>
#include <net/tcp.h>
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

/*
 * The counts agree and every borrowed entry holds a page; with @checked,
 * whoever built the table checked its entries, and only the counts and the
 * bitmaps are checked again.
 */
static bool __rx_loan_table_refs_valid(const struct rx_loan_table *t,
				       bool checked)
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
	if (checked)
		return true;
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

/* The counts agree and every borrowed entry holds a page. */
bool rx_loan_table_refs_valid(const struct rx_loan_table *t)
{
	return __rx_loan_table_refs_valid(t, false);
}
EXPORT_SYMBOL_GPL(rx_loan_table_refs_valid);

/*
 * rx_loan_table_refs_valid() for a table whose entries were checked when it
 * was published and have not changed since: the counts and bitmaps only.
 */
bool rx_loan_table_refs_valid_checked(const struct rx_loan_table *t)
{
	return __rx_loan_table_refs_valid(t, true);
}
EXPORT_SYMBOL_GPL(rx_loan_table_refs_valid_checked);

/*
 * Page-pool pages of one record returned to their pool at a time.
 * page_pool_put_netmem_bulk() gathers the pages it recycles into an
 * on-stack array of XDP_BULK_QUEUE_SIZE entries and does not check
 * its bound, so a batch must not be larger.
 */
#define RX_LOAN_PUT_BATCH	XDP_BULK_QUEUE_SIZE

/*
 * Return @refs references to page-pool @netmem, held by entries side by
 * side.  A driver that cuts a page into several receive buffers puts them
 * in an skb in turn, so a record often holds two or more of one page's
 * buffers next to each other, and often every reference to the page.
 * Then page_pool_unref_netmem() drops them all with a read of the page's
 * count, where one at a time the first drops took an atomic on a line
 * other CPUs write.  Its last user holds the page at a count of 1, and
 * the batch returns it by dropping that.
 */
static void rx_loan_put_run(netmem_ref *batch, unsigned int *nr,
			    netmem_ref netmem, long refs)
{
	if (refs > 1 && page_pool_unref_netmem(netmem, refs))
		return;
	batch[(*nr)++] = netmem;
	if (*nr == RX_LOAN_PUT_BATCH) {
		page_pool_put_netmem_bulk(batch, *nr);
		*nr = 0;
	}
}

/*
 * A prefetch is a hint the CPU may drop, and when the release's misses are
 * many, it drops some: a page whose struct page is fetched only a batch
 * ahead still misses at its drop.  So each is fetched a second time.  On
 * arm64 the second request goes ahead of the first, into the L2 only and
 * two batches ahead, and the prefetch a batch ahead finds it there.  Where
 * there is no such prefetch, the page is prefetched again a few entries
 * before its drop.
 */
#ifdef CONFIG_ARM64
#define RX_LOAN_PREFETCH_AGAIN	(2 * RX_LOAN_PUT_BATCH)

static __always_inline void rx_loan_prefetch_again(const void *p)
{
	asm volatile("prfm pstl2keep, %a0" : : "p" (p));
}
#else
#define RX_LOAN_PREFETCH_AGAIN	4

static __always_inline void rx_loan_prefetch_again(const void *p)
{
	prefetchw(p);
}
#endif

/*
 * Return every borrowed entry of @t, whose references the caller checked
 * with rx_loan_table_refs_valid(): its bytes stay counted.  -EINVAL if
 * the table does not come out empty.
 */
int rx_loan_table_release(struct rx_loan_table *t)
{
	netmem_ref batch[RX_LOAN_PUT_BATCH];
	unsigned int index, nr = 0;
	netmem_ref run = 0;
	long run_refs = 0;

	/* page_pool_put_netmem_bulk() does not check its array's bound. */
	BUILD_BUG_ON(RX_LOAN_PUT_BATCH > XDP_BULK_QUEUE_SIZE);
	/* the lockless fast-path fields share no line with the lock */
	BUILD_BUG_ON(IS_ENABLED(CONFIG_SMP) &&
		     offsetofend(struct rx_loan_budget, stock) >
		     offsetof(struct rx_loan_budget, lock));

	/*
	 * Each borrowed page's struct page has left this CPU's caches while
	 * the record was written out, and is read and written again to drop
	 * its references.  Fetch each a batch of entries ahead of its drop,
	 * so that a batch's misses are in flight together and not taken one
	 * page after another, and again as rx_loan_prefetch_again() says.
	 */
	for (index = 0; index < min(t->count, RX_LOAN_PUT_BATCH); index++)
		prefetchw(t->bvec[index].bv_page);
	for (; index < min(t->count, RX_LOAN_PREFETCH_AGAIN); index++)
		rx_loan_prefetch_again(t->bvec[index].bv_page);
	for (index = 0; index < t->count; index++) {
		if (index + RX_LOAN_PREFETCH_AGAIN < t->count)
			rx_loan_prefetch_again(t->bvec[index +
					RX_LOAN_PREFETCH_AGAIN].bv_page);
		if (index + RX_LOAN_PUT_BATCH < t->count)
			prefetchw(t->bvec[index + RX_LOAN_PUT_BATCH].bv_page);
		if (!test_bit(index, t->borrowed))
			continue;
		/*
		 * A record's page-pool pages go back in batches: the pool's
		 * producer lock is taken for each batch and not each page.
		 */
		if (IS_ENABLED(CONFIG_PAGE_POOL) &&
		    test_bit(index, t->page_pool)) {
			struct page *page = t->bvec[index].bv_page;
			netmem_ref netmem;

			netmem = netmem_compound_head(page_to_netmem(page));
			if (run_refs && netmem == run) {
				run_refs++;
			} else {
				if (run_refs)
					rx_loan_put_run(batch, &nr, run,
							run_refs);
				run = netmem;
				run_refs = 1;
			}
		} else {
			rx_loan_put_borrowed(t->bvec[index].bv_page, false);
		}
		__clear_bit(index, t->borrowed);
		__clear_bit(index, t->page_pool);
		t->refs_released++;
		t->borrowed_count--;
	}
	if (run_refs)
		rx_loan_put_run(batch, &nr, run, run_refs);
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

/* Every consumer's copies, and those of units counted to none. */
static DEFINE_PER_CPU(struct rx_loan_copies, rx_loan_copies);

static void rx_loan_copies_sum(const struct rx_loan_copies __percpu *pc,
			       u64 *records, u64 *bytes)
{
	int cpu;

	*records = 0;
	*bytes = 0;
	if (!pc)
		return;
	for_each_possible_cpu(cpu) {
		const struct rx_loan_copies *c = per_cpu_ptr(pc, cpu);

		*records += READ_ONCE(c->records);
		*bytes += READ_ONCE(c->bytes);
	}
}

/* Sum the host-wide copies counter over every CPU. */
void rx_loan_copies_read(u64 *records, u64 *bytes)
{
	rx_loan_copies_sum(&rx_loan_copies, records, bytes);
}
EXPORT_SYMBOL_GPL(rx_loan_copies_read);

/* Sum @c's own copies over every CPU: 0 before it first registered. */
void rx_loan_consumer_copies_read(const struct rx_loan_consumer *c,
				  u64 *records, u64 *bytes)
{
	rx_loan_copies_sum(c->copies, records, bytes);
}
EXPORT_SYMBOL_GPL(rx_loan_consumer_copies_read);

/* A unit copied @bytes: host-wide, and to @c's own counter if any. */
static void rx_loan_copies_count(struct rx_loan_consumer *c, u32 bytes)
{
	struct rx_loan_copies __percpu *own = c ? READ_ONCE(c->copies) : NULL;

	this_cpu_inc(rx_loan_copies.records);
	this_cpu_add(rx_loan_copies.bytes, bytes);
	if (own) {
		this_cpu_inc(own->records);
		this_cpu_add(own->bytes, bytes);
	}
}

/*
 * Clear @t's counts for its next record and keep its arrays.  Append sets
 * only [0, count); terminal owners clear it before reset.  A unit that
 * copied bytes counts once in the copies counter here, and in @c's own if
 * @c is not NULL.
 */
void rx_loan_table_reset_consumer(struct rx_loan_table *t,
				  struct rx_loan_consumer *c)
{
	unsigned int capacity = t->capacity;
	struct bio_vec *bvec = t->bvec;
	unsigned long *borrowed = t->borrowed;
	unsigned long *page_pool = t->page_pool;

	if (t->copied_bytes)
		rx_loan_copies_count(c, t->copied_bytes);
	memset(t, 0, sizeof(*t));
	t->capacity = capacity;
	t->bvec = bvec;
	t->borrowed = borrowed;
	t->page_pool = page_pool;
}
EXPORT_SYMBOL_GPL(rx_loan_table_reset_consumer);

/* As rx_loan_table_reset_consumer(), its copies counted to no consumer. */
void rx_loan_table_reset(struct rx_loan_table *t)
{
	rx_loan_table_reset_consumer(t, NULL);
}
EXPORT_SYMBOL_GPL(rx_loan_table_reset);

/* Copy @len bytes at @src to byte @dest_offset of @arena (or @region). */
static void rx_loan_tile_copy(struct page **arena,
			      const struct bio_vec *region, u32 dest_offset,
			      const struct bio_vec *src, u32 src_offset,
			      u32 len)
{
	while (len) {
		u32 index = dest_offset >> PAGE_SHIFT;
		u32 in_page = offset_in_page(dest_offset);
		u32 n = min_t(u32, len, PAGE_SIZE - in_page);
		struct page *page = region ? region[index].bv_page :
					     arena[index];

		memcpy_page(page, in_page, src->bv_page,
			    src->bv_offset + src_offset, n);
		dest_offset += n;
		src_offset += n;
		len -= n;
	}
}

/**
 * rx_loan_tile_take - take pages of the region's write partition to tile in
 * @out: receives the pages, in @out[].bv_page
 * @n: how many
 *
 * All or nothing, without a second array: page_pool_rx_region_take_pages()
 * packs the page pointers at the start of @out, and they are spread out
 * from the last, so no pointer is overwritten before it is read.  The
 * caller holds the region credit they stand for.
 *
 * Return: true if @out has @n pages.
 */
bool rx_loan_tile_take(struct bio_vec *out, unsigned int n)
{
	struct page **pages = (struct page **)out;
	unsigned int i;

	BUILD_BUG_ON(sizeof(*pages) > sizeof(*out));
	if (page_pool_rx_region_take_pages(false, pages, n) != n)
		return false;
	for (i = n; i-- > 0;)
		out[i].bv_page = pages[i];
	return true;
}
EXPORT_SYMBOL_GPL(rx_loan_tile_take);

/**
 * rx_loan_tile_give - give back what rx_loan_tile_take() took
 * @bvec: the pages, in @bvec[].bv_page; clobbered (packs them at its start)
 * @n: how many
 */
void rx_loan_tile_give(struct bio_vec *bvec, unsigned int n)
{
	struct page **pages = (struct page **)bvec;
	unsigned int i;

	for (i = 0; i < n; i++)
		pages[i] = bvec[i].bv_page;
	page_pool_rx_region_give_pages(pages, n);
}
EXPORT_SYMBOL_GPL(rx_loan_tile_give);

/**
 * rx_loan_tile - lay a loaned payload out page-tiled
 * @t: the record's loan table, its references checked
 *     (rx_loan_table_refs_valid())
 * @base: the record-body offset of @t's first entry
 * @start: the payload's record-body offset
 * @len: the payload's length, not 0, within @t
 * @arena: the consumer's pages by record-body offset, when @region is NULL
 * @region: pages to tile into from in-page offset @align
 *	    (rx_loan_tile_take()), or NULL
 * @align: in-page offset of the payload's first byte in @region
 * @out: receives the page-tiled payload, room for every page it spans
 * @c: the consumer the copy counts to as one unit, or NULL: not counted
 *
 * Whole pages joined only at page boundaries, as direct I/O needs.  With
 * @region, every byte of the payload is copied there, from @align.
 * Without, the payload is laid out in @arena at its record-body offsets,
 * where @t's copied entries already are, and only its borrowed bytes are
 * copied.  The caller has checked that the layout fits @out and the pages.
 * Each borrowed entry whose bytes the copy consumed is returned as it
 * passes, its bytes still counted; the rest stay loaned.  Nothing waits.
 *
 * Return: entries in @out.
 */
unsigned int rx_loan_tile(struct rx_loan_table *t, u32 base, u32 start,
			  u32 len, struct page **arena,
			  const struct bio_vec *region, u32 align,
			  struct bio_vec *out, struct rx_loan_consumer *c)
{
	u32 index, b, n, end = start + len, copied = 0;

	for (b = base, index = 0; index < t->count && b < end; index++) {
		struct bio_vec *bvec = &t->bvec[index];
		u32 lo = max(b, start), hi = min(b + bvec->bv_len, end);
		bool borrowed = test_bit(index, t->borrowed);

		if (lo < hi && (region || borrowed)) {
			rx_loan_tile_copy(arena, region,
					  region ? lo - start + align : lo,
					  bvec, lo - b, hi - lo);
			copied += hi - lo;
		}
		/* Nothing reads a consumed loaned page again: return it now. */
		if (borrowed && lo < hi && b + bvec->bv_len <= end)
			rx_loan_table_return_entry(t, index);
		b += bvec->bv_len;
	}

	/* @b walks the destination: region from @align, arena by body offset. */
	for (b = region ? align : start, n = 0, end = b + len; b < end; n++) {
		u32 seg = min_t(u32, end - b, PAGE_SIZE - offset_in_page(b));
		struct page *page = region ? region[n].bv_page :
					     arena[b >> PAGE_SHIFT];

		bvec_set_page(&out[n], page, seg, offset_in_page(b));
		b += seg;
	}
	if (c && copied)
		rx_loan_copies_count(c, copied);
	return n;
}
EXPORT_SYMBOL_GPL(rx_loan_tile);

/*
 * A socket's receive buffer is locked, and counted in the memory its skbs
 * occupy, @factor bytes for each 100 of payload (rx_loan_hold_factor()).
 * Half of the payload it can hold is the most its window is opened to.
 *
 * The window is what the peer may have on the way, and the other half is
 * for what has arrived and not been read: a thread is still copying the
 * record before, or waiting for credit.  With a window of all the buffer
 * every byte not yet read comes off what the peer may send, and it is
 * held up whenever the reader is a little behind; and all the sockets'
 * windows together arrive as one burst that the device's rings have to
 * take.
 *
 * TCP converts free receive memory into window at a payload/truesize ratio
 * it learns from full-size segments only, and would open the window to all
 * the buffer holds.  Cap the ratio at half of what the socket's skbs were
 * measured to occupy, in TCP_RMEM_TO_WIN_SCALE units, so that the window
 * also closes by half of what is left as the buffer fills.
 */
u8 rx_loan_hold_scaling_ratio(unsigned int factor)
{
	return div64_u64((u64)100 << TCP_RMEM_TO_WIN_SCALE,
			 (u64)max(factor, 100U) * 2);
}
EXPORT_SYMBOL_GPL(rx_loan_hold_scaling_ratio);

/*
 * The most a socket's window is opened to: what TCP makes of the whole of
 * @rcvbuf at rx_loan_hold_scaling_ratio(), so that the clamp is never more
 * than TCP would offer of an empty buffer.
 */
int rx_loan_hold_window_clamp(int rcvbuf, unsigned int factor)
{
	return ((u64)rcvbuf * rx_loan_hold_scaling_ratio(factor)) >>
	       TCP_RMEM_TO_WIN_SCALE;
}
EXPORT_SYMBOL_GPL(rx_loan_hold_window_clamp);

/*
 * Bytes of payload a socket reads between two samples of what its skbs
 * occupy.  Long enough to hold several records, so that a record's short
 * last segment, which takes a whole receive buffer, weighs in a sample as
 * it does in the receive queue.
 */
#define RX_LOAN_HOLD_WINDOW	(4U << 20)
/*
 * An skb with less payload than this is not measured.  A call, a reply's
 * acknowledgment or a small request has a receive buffer to itself, a
 * hundred bytes in several thousand, and a socket that reads a few of
 * those before any data would be held to a window of less than a segment,
 * which TCP does not advertise at all: the first connection of a freshly
 * loaded server stopped that way.  A record's short last segment is not
 * lost by this, since GRO has it in an skb with the full ones before it.
 */
#define RX_LOAN_HOLD_MIN_SKB	1024U

static unsigned int rx_loan_hold_sample(u32 mem, u32 pay)
{
	return clamp_t(u64, DIV_ROUND_UP_ULL((u64)mem * 100, pay), 100,
		       RX_LOAN_HOLD_FACTOR_MAX);
}

/**
 * rx_loan_hold_account - measure what a socket's skbs occupy
 * @h: the socket's hold
 * @truesize: what the skb occupies
 * @skb_len: its payload
 * @used: what of it was read
 *
 * TCP counts a receive buffer in truesize and a window in payload, and what
 * one is to the other is a property of how the device cuts its pages into
 * receive buffers and of the segments' sizes, so it is measured: the memory
 * and the payload of everything the socket reads are summed, and every
 * RX_LOAN_HOLD_WINDOW bytes their ratio is a sample.  The receive buffer
 * is held to the higher of the last two (rx_loan_hold_measured()).  Only
 * the thread reading the socket gets here.
 *
 * Return: true if a sample was taken, which the caller may publish as its
 * budget's hold_factor.
 */
bool rx_loan_hold_account(struct rx_loan_hold *h, unsigned int truesize,
			  unsigned int skb_len, unsigned int used)
{
	if (skb_len < RX_LOAN_HOLD_MIN_SKB || !used)
		return false;
	h->geo_mem += div_u64((u64)truesize * min(used, skb_len), skb_len);
	h->geo_pay += used;
	if (h->geo_pay < RX_LOAN_HOLD_WINDOW)
		return false;
	h->geo_factor[1] = h->geo_factor[0];
	h->geo_factor[0] = rx_loan_hold_sample(h->geo_mem, h->geo_pay);
	h->geo_mem = 0;
	h->geo_pay = 0;
	return true;
}
EXPORT_SYMBOL_GPL(rx_loan_hold_account);

/**
 * rx_loan_hold_factor - what a socket's receive buffer is held to
 * @h: the socket's hold
 * @lb: the budget it draws on, or NULL
 *
 * The bytes its skbs occupy for each 100 of payload: the higher of its last
 * two samples, else, until it has one, what another socket of @lb measured
 * (@lb->hold_factor), else RX_LOAN_HOLD_FACTOR_UNLEARNED.  What a socket
 * has read of its first window is not used: a few skbs are not a sample.
 */
unsigned int rx_loan_hold_factor(const struct rx_loan_hold *h,
				 const struct rx_loan_budget *lb)
{
	unsigned int factor = rx_loan_hold_measured(h);

	if (factor)
		return factor;
	return (lb ? READ_ONCE(lb->hold_factor) : 0) ?:
	       RX_LOAN_HOLD_FACTOR_UNLEARNED;
}
EXPORT_SYMBOL_GPL(rx_loan_hold_factor);

#if IS_ENABLED(CONFIG_INET)
/*
 * A socket held again at a lower factor (rx_loan_hold_stale()) gets a
 * higher ratio ceiling and window clamp, but neither lifts what the first
 * hold held down: TCP lowers its ratio to a lower ceiling at once and
 * raises it only when it next measures a segment of a new size, and a
 * higher clamp raises rcv_ssthresh only as far as the window TCP already
 * offers.  The window would stay where the first hold left it for the
 * socket's life, so that a socket held before its budget had learned its
 * factor never had the window of those held after.  Lift both: the ratio
 * to the new ceiling where the old one held it, rcv_ssthresh by what the
 * clamp rose.  TCP still offers no more window than the free buffer
 * allows.
 */
static void rx_loan_hold_raise(struct sock *sk, u8 old_ratio, u32 old_clamp)
{
	struct tcp_sock *tp = tcp_sk(sk);

	lock_sock(sk);
	if (tp->scaling_ratio == old_ratio && tp->scaling_ratio_max > old_ratio)
		tp->scaling_ratio = tp->scaling_ratio_max;
	if (tp->window_clamp > old_clamp)
		tp->rcv_ssthresh = min(tp->window_clamp, tp->rcv_ssthresh +
				       (tp->window_clamp - old_clamp));
	release_sock(sk);
}

/**
 * rx_loan_hold_apply - hold a TCP socket's receive buffer
 * @h: the socket's hold; its pending request is cleared
 * @sk: the socket, unlocked: each step takes the socket lock itself
 * @bytes: the receive buffer to hold it to, or 0: not held
 * @factor: what its skbs occupy for each 100 of payload
 *	    (rx_loan_hold_factor())
 * @rcvbuf: receives the receive buffer it was held to
 * @clamp: receives the window clamp
 *
 * Lock the receive buffer to @bytes, cap the payload/truesize ratio TCP
 * turns free buffer into window with at @factor, and clamp the window, so
 * its unread data stays its share of the region.  The ceiling also keeps
 * TCP from raising the clamp again when it re-learns its ratio.  Sockets
 * that never receive a region page keep TCP's own buffer
 * (rx_loan_hold_release()).  TCP sizes a
 * connection's first window from its listener when the SYN arrives and
 * never shrinks a window it has offered, so a listener's buffer and ratio
 * are held too (rx_loan_hold_listen(), no lock or clamp): a connection that
 * opened with the listener's larger window would overflow the buffer it is
 * held to with its first burst.
 *
 * Return: 0 once held; -EALREADY, nothing done, when @bytes is 0 or @h is
 * already held; else the window clamp's error, @h held all the same.
 */
int rx_loan_hold_apply(struct rx_loan_hold *h, struct sock *sk,
		       unsigned int bytes, unsigned int factor, int *rcvbuf,
		       int *clamp)
{
	u32 old_clamp;
	u8 ratio, old_ratio;
	bool again;
	int err;

	h->pending = false;
	if (!bytes || h->held)
		return -EALREADY;
	again = h->factor != 0;
	h->factor = factor;
	*rcvbuf = max_t(int, bytes, SOCK_MIN_RCVBUF);
	/*
	 * Never under two segments: TCP advertises no window smaller than
	 * one, and the sender then waits on a receiver that waits on it.
	 */
	*clamp = max_t(int, rx_loan_hold_window_clamp(*rcvbuf, factor),
		       2 * READ_ONCE(tcp_sk(sk)->advmss));
	ratio = rx_loan_hold_scaling_ratio(factor);

	old_ratio = READ_ONCE(tcp_sk(sk)->scaling_ratio_max);
	old_clamp = READ_ONCE(tcp_sk(sk)->window_clamp);

	/* Unread data is this socket's share of the region. */
	sock_set_rcvbuf(sk, *rcvbuf / 2);	/* doubled on the way in */
	tcp_sock_set_scaling_ratio_max(sk, ratio);
	err = tcp_sock_set_window_clamp(sk, *clamp);
	if (!err && again)
		rx_loan_hold_raise(sk, old_ratio, old_clamp);
	h->held = true;
	return err;
}
EXPORT_SYMBOL_GPL(rx_loan_hold_apply);

/**
 * rx_loan_hold_listen - hold the first window of a listener's connections
 * @h: the listener's hold
 * @sk: the listener, unlocked
 * @bytes: the receive buffer its connections open with, or 0: not held
 * @factor: what skbs occupy for each 100 of payload (rx_loan_hold_factor())
 *
 * TCP sizes a connection's first window from its listener's receive
 * buffer and ratio when the SYN arrives, and never shrinks a window it has
 * offered.  Set only those two: a lock or a window clamp on the listener
 * would also lower the window scale every connection negotiates, for its
 * whole life, lending or not.  Connections inherit the buffer unlocked;
 * hold each that may lend with rx_loan_hold_apply() before reading it, and
 * give the rest back theirs with rx_loan_hold_release().
 *
 * Return: 0 once held; -EALREADY, nothing done, when @bytes is 0 or @h is
 * already held.
 */
int rx_loan_hold_listen(struct rx_loan_hold *h, struct sock *sk,
			unsigned int bytes, unsigned int factor)
{
	u8 ratio = rx_loan_hold_scaling_ratio(factor);

	if (!bytes || h->held)
		return -EALREADY;
	lock_sock(sk);
	WRITE_ONCE(sk->sk_rcvbuf, max_t(int, bytes, SOCK_MIN_RCVBUF));
	if (ratio < tcp_sk(sk)->scaling_ratio)
		tcp_sk(sk)->scaling_ratio = ratio;
	release_sock(sk);
	h->factor = factor;
	/* callers may read a listener's hold locklessly */
	WRITE_ONCE(h->held, true);
	return 0;
}
EXPORT_SYMBOL_GPL(rx_loan_hold_listen);

/**
 * rx_loan_hold_release - give a socket back TCP's own receive buffer
 * @sk: a connection that will never receive region pages, or a listener
 *	whose connections no longer would; unlocked
 *
 * A connection that will not lend must not keep a hold, applied
 * (rx_loan_hold_apply()) or inherited from a held listener
 * (rx_loan_hold_listen()): unlock its buffer so TCP autotunes it again
 * from at least tcp_rmem[1], drop the ratio ceiling, and let the window
 * clamp follow the buffer.  A listener goes back to stock: tcp_rmem[1], the
 * default ratio and no window clamp.  A connection neither locked nor
 * under tcp_rmem[1] is left as it is.
 */
void rx_loan_hold_release(struct sock *sk)
{
	int rmem = READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_rmem[1]);
	struct tcp_sock *tp = tcp_sk(sk);
	u32 old_clamp;
	u8 old_ratio;
	int clamp;

	lock_sock(sk);
	if (sk->sk_state == TCP_LISTEN) {
		/* tcp_set_window_clamp() refuses 0 but on a closed socket. */
		sk->sk_userlocks &= ~SOCK_RCVBUF_LOCK;
		WRITE_ONCE(sk->sk_rcvbuf, rmem);
		WRITE_ONCE(tp->scaling_ratio_max, 0);
		tcp_scaling_ratio_init(sk);
		WRITE_ONCE(tp->window_clamp, 0);
		release_sock(sk);
		return;
	}
	if (!(sk->sk_userlocks & SOCK_RCVBUF_LOCK) && sk->sk_rcvbuf >= rmem) {
		release_sock(sk);
		return;
	}
	sk->sk_userlocks &= ~SOCK_RCVBUF_LOCK;
	WRITE_ONCE(sk->sk_rcvbuf, max_t(int, sk->sk_rcvbuf, rmem));
	old_ratio = tp->scaling_ratio_max;
	old_clamp = tp->window_clamp;
	tp->scaling_ratio_max = 0;
	clamp = tcp_win_from_space(sk, sk->sk_rcvbuf);
	release_sock(sk);

	if ((u32)clamp > old_clamp && !tcp_sock_set_window_clamp(sk, clamp))
		rx_loan_hold_raise(sk, old_ratio, old_clamp);
}
EXPORT_SYMBOL_GPL(rx_loan_hold_release);
#endif /* CONFIG_INET */

/*
 * The region's credit outstanding across every budget of every namespace
 * and every consumer, write and READ: the hard cap on them all together.
 * A budget charges it when credit goes to a record or a READ, from its
 * avail and reserve or from a CPU's stock (one atomic per record on the
 * per-CPU fast path: rx_loan_stock_take()), and refunds it when the credit
 * comes back.  Stocked credit is the budget's, not the region's, so a stock
 * larger than what the region has left takes nothing past it; a refusal
 * drains the stocks once (@draining) and every return after it kicks the
 * budgets (@waiting).  With no region it counts, and refuses nothing.
 * Write credit, every budget's but the read pools', is also counted in
 * @out_write, whatever namespace or consumer it belongs to.  A
 * write_capped budget (nvmet-tcp's, sized to the write share beside
 * kNFSd's) is refused past the write partition (the region less its read
 * split) by that count, so it cannot take the read partition; kNFSd's
 * budgets are capped by @out alone, as before there was a second.
 */
static struct {
	atomic_long_t	out;
	atomic_long_t	out_write;	/* of @out, write budgets' credit */
	atomic_long_t	orphaned;	/* kept spent: abandoned records */
	atomic_long_t	denied;
	atomic_t	waiting;	/* refused since the last return */
	atomic_t	draining;	/* stocks drained for a refusal */
} rx_loan_region ____cacheline_aligned_in_smp;

/*
 * Registered consumers and the budgets a region return kicks: written on
 * the control path only (a consumer's start and stop, a namespace's),
 * never on admission, so a mutex.  The budgets are walked under RCU.
 */
static DEFINE_MUTEX(rx_loan_consumers_lock);
static LIST_HEAD(rx_loan_consumers);
static LIST_HEAD(rx_loan_budgets);
static unsigned int rx_loan_nr_consumers;

static void rx_loan_region_kick_all(void)
{
	struct rx_loan_budget *lb;

	rcu_read_lock();
	list_for_each_entry_rcu(lb, &rx_loan_budgets, region_node)
		schedule_work(&lb->kick_work);
	rcu_read_unlock();
}

/*
 * Charge @n pages to the region: false, with nothing charged, if that
 * would pass it, unless @force (a budget turned off: counted, not capped).
 * With no region size the charge is counted, not capped.  @carved, if not
 * NULL: whether a region was in place to charge, not only a size for one.
 * No undo of the total: a refusal never makes another admission see a
 * total it passed.  A write_capped budget's write count is undone when the
 * total refuses it, so another such admission may briefly see it.
 */
static bool rx_loan_region_carved(void);

/* Add @n to @ctr unless that passes @cap; see rx_loan_region_get(). */
static bool rx_loan_region_add(atomic_long_t *ctr, long n, long cap)
{
	long v = atomic_long_read(ctr);

	do {
		if (v + n > cap) {
			/*
			 * Pairs with rx_loan_region_put(): either it sees
			 * waiting, or this sees what it returned.
			 */
			atomic_set(&rx_loan_region.waiting, 1);
			smp_mb__after_atomic();
			v = atomic_long_read(ctr);
			if (v + n <= cap)
				continue;
			atomic_long_inc(&rx_loan_region.denied);
			/* Idle stocks may hold what is short: drain them once. */
			if (!atomic_xchg(&rx_loan_region.draining, 1))
				rx_loan_region_kick_all();
			return false;
		}
	} while (!atomic_long_try_cmpxchg(ctr, &v, v + n));
	return true;
}

static bool rx_loan_region_get(long n, const struct rx_loan_budget *lb,
			       bool force, bool *carved)
{
	unsigned long size, read_pages;
	bool read = lb->read;

	if (n <= 0)
		return true;
	size = rx_loan_region_size();
	if (carved)
		*carved = rx_loan_region_carved();
	if (!size || force) {
		atomic_long_add(n, &rx_loan_region.out);
		if (!read)
			atomic_long_add(n, &rx_loan_region.out_write);
		return true;
	}
	if (lb->write_capped) {
		read_pages = rx_loan_region_read_pages();
		if (!rx_loan_region_add(&rx_loan_region.out_write, n,
					size > read_pages ? size - read_pages : 0))
			return false;
	}
	if (!rx_loan_region_add(&rx_loan_region.out, n, size)) {
		/*
		 * Back the write count off with no kick: nothing was
		 * returned, and the refusal has just set @waiting for the
		 * next real return to kick the budgets.
		 */
		if (lb->write_capped)
			atomic_long_sub(n, &rx_loan_region.out_write);
		return false;
	}
	/* Not capped (kNFSd's): counted once the total has passed it. */
	if (!read && !lb->write_capped)
		atomic_long_add(n, &rx_loan_region.out_write);
	if (atomic_read(&rx_loan_region.draining))
		atomic_set(&rx_loan_region.draining, 0);
	return true;
}

/*
 * Refund @pages to the region, any context.  After a refusal, kick every
 * budget, whose waiters may be short of the region rather than their own
 * credit.
 */
/*
 * Charge @pages to the region for a stocked take of @lb's credit; false if
 * it has not got them.
 */
bool rx_loan_region_take(long pages, const struct rx_loan_budget *lb,
			 bool *carved)
{
	return rx_loan_region_get(pages, lb, false, carved);
}
EXPORT_SYMBOL_GPL(rx_loan_region_take);

static void rx_loan_region_returned(void)
{
	smp_mb__after_atomic();
	if (atomic_read(&rx_loan_region.waiting) &&
	    atomic_xchg(&rx_loan_region.waiting, 0))
		rx_loan_region_kick_all();
}

void rx_loan_region_put(long pages, bool read)
{
	if (pages <= 0)
		return;
	if (!read)
		atomic_long_sub(pages, &rx_loan_region.out_write);
	atomic_long_sub(pages, &rx_loan_region.out);
	rx_loan_region_returned();
}
EXPORT_SYMBOL_GPL(rx_loan_region_put);

long rx_loan_region_out(void)
{
	return atomic_long_read(&rx_loan_region.out);
}
EXPORT_SYMBOL_GPL(rx_loan_region_out);

/* Of rx_loan_region_out(), the write budgets' credit (for KUnit). */
long rx_loan_region_write_out(void)
{
	return atomic_long_read(&rx_loan_region.out_write);
}
EXPORT_SYMBOL_IF_KUNIT(rx_loan_region_write_out);

long rx_loan_region_orphaned(void)
{
	return atomic_long_read(&rx_loan_region.orphaned);
}
EXPORT_SYMBOL_GPL(rx_loan_region_orphaned);

/**
 * rx_loan_consumer_share - the most of the region @c may hold
 * @c: a consumer
 *
 * The region's pages over the consumers registered, region-wide; 0 (no
 * cap) with no region, or for a consumer not registered.  Lockless.
 */
unsigned long rx_loan_consumer_share(const struct rx_loan_consumer *c)
{
	unsigned int n = READ_ONCE(rx_loan_nr_consumers);

	if (!READ_ONCE(c->registered) || !n)
		return 0;
	return rx_loan_region_size() / n;
}
EXPORT_SYMBOL_GPL(rx_loan_consumer_share);

/*
 * Charge @n pages to @c's count of @read or write credit: false, with
 * nothing charged, if that would pass its share, unless @force (the oldest
 * record, which always starts; counted in overrun_share).
 */
static bool rx_loan_consumer_get(struct rx_loan_consumer *c, bool read,
				 long n, bool force)
{
	atomic_long_t *out;
	unsigned long share;
	long v;

	if (!c || n <= 0)
		return true;
	out = read ? &c->out_read : &c->out_write;
	share = rx_loan_consumer_share(c);
	v = atomic_long_read(out);
	do {
		if (share && v + n > (long)share) {
			if (!force) {
				atomic_long_inc(&c->share_denied);
				return false;
			}
			atomic_long_inc(&c->overrun_share);
			atomic_long_add(n, out);
			return true;
		}
	} while (!atomic_long_try_cmpxchg(out, &v, v + n));
	return true;
}

/**
 * rx_loan_consumer_put - refund credit charged to a consumer's share
 * @c: the consumer charged, or NULL
 * @read: READ (exact) credit, else write
 * @n: pages
 *
 * For credit returned outside rx_loan_return() and the grant returns:
 * what a record lent on (struct rx_loan_lend) when its borrower is done.
 * The caller then passes the credit on through the budget, whose kick
 * brings back any request waiting for @c to come under its share.
 */
void rx_loan_consumer_put(struct rx_loan_consumer *c, bool read, long n)
{
	if (c && n > 0)
		atomic_long_sub(n, read ? &c->out_read : &c->out_write);
}
EXPORT_SYMBOL_GPL(rx_loan_consumer_put);

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

/* For stats: lockless, so a racing CPU's counts may be off by its last op. */
void rx_loan_stock_sum(struct rx_loan_budget *lb,
		       struct rx_loan_stock_sum *sum)
{
	int cpu;

	memset(sum, 0, sizeof(*sum));
	for_each_possible_cpu(cpu) {
		struct rx_loan_stock *st = per_cpu_ptr(lb->stock, cpu);

		sum->pages += atomic_long_read(&st->pages);
		sum->records += READ_ONCE(st->records);
		sum->read_inflight += READ_ONCE(st->read_inflight);
		sum->lent += READ_ONCE(st->lent);
		sum->admitted += READ_ONCE(st->admitted);
		sum->read_admitted += READ_ONCE(st->read_admitted);
		sum->read_completed += READ_ONCE(st->read_completed);
	}
}
EXPORT_SYMBOL_GPL(rx_loan_stock_sum);

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
	long back = 0;
	int cpu;

	WRITE_ONCE(lb->contended, true);
	/* Pairs with rx_loan_stock_put(): set contended, then take stocks. */
	smp_mb();
	for_each_possible_cpu(cpu) {
		struct rx_loan_stock *st = per_cpu_ptr(lb->stock, cpu);

		back += atomic_long_xchg(&st->pages, 0);
		rx_loan_cost_fold_cpu_locked(lb, st);
	}
	lb->avail += back;
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
	/* Uncharged: the region is charged as a record takes it. */
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
 * A test's region starts with nothing out or orphaned: what was before it
 * is set aside while the region is in place (@aside), and is all there is
 * again when it goes, whatever the test left spent, but for credit charged
 * before the region and given back under it, which comes off what was set
 * aside: the total, less what stays orphaned, is then below nothing.
 */
void rx_loan_kunit_region_set_aside(bool aside)
{
	static long real_out, real_out_write, real_orphaned;
	long early;

	if (aside) {
		real_out = atomic_long_xchg(&rx_loan_region.out, 0);
		real_out_write = atomic_long_xchg(&rx_loan_region.out_write, 0);
		real_orphaned = atomic_long_xchg(&rx_loan_region.orphaned, 0);
	} else {
		early = atomic_long_read(&rx_loan_region.out) -
			atomic_long_read(&rx_loan_region.orphaned);
		atomic_long_set(&rx_loan_region.out,
				real_out + min(early, 0L));
		/* Orphans are write records: the same holds of write credit. */
		early = atomic_long_read(&rx_loan_region.out_write) -
			atomic_long_read(&rx_loan_region.orphaned);
		atomic_long_set(&rx_loan_region.out_write,
				max(real_out_write + min(early, 0L), 0L));
		atomic_long_set(&rx_loan_region.orphaned, real_orphaned);
	}
	atomic_set(&rx_loan_region.waiting, 0);
	atomic_set(&rx_loan_region.draining, 0);
}
EXPORT_SYMBOL_IF_KUNIT(rx_loan_kunit_region_set_aside);

/*
 * Stand in for a region of @pages pages with @read_pages split off for
 * reads (0, 0: the real region, if any).  Nothing is applied: a read pool
 * picks it up at its next locked op.  Like a test's own region, a stand-in
 * starts with nothing out (rx_loan_kunit_region_set_aside()).
 */
void rx_loan_kunit_region(unsigned long pages, unsigned long read_pages)
{
	bool was = READ_ONCE(rx_loan_kunit_region_pages);

	WRITE_ONCE(rx_loan_kunit_region_read, read_pages);
	WRITE_ONCE(rx_loan_kunit_region_pages, pages);
	if (!pages != !was) {
		rx_loan_kunit_region_set_aside(pages);
		return;
	}
	atomic_set(&rx_loan_region.waiting, 0);
	atomic_set(&rx_loan_region.draining, 0);
}
EXPORT_SYMBOL_IF_KUNIT(rx_loan_kunit_region);

/* How many times the region has refused a charge. */
long rx_loan_kunit_region_denied(void)
{
	return atomic_long_read(&rx_loan_region.denied);
}
EXPORT_SYMBOL_IF_KUNIT(rx_loan_kunit_region_denied);
#endif

/*
 * Whether the region is in place (or a test's stand-in for it).  Before it
 * is carved, and after a carve failed, it has a size but no pages: what is
 * charged then holds no region page.
 */
static bool rx_loan_region_carved(void)
{
#if IS_ENABLED(CONFIG_KUNIT)
	if (READ_ONCE(rx_loan_kunit_region_pages))
		return true;
#endif
	return page_pool_rx_region_exists();
}

unsigned long rx_loan_region_size(void)
{
#if IS_ENABLED(CONFIG_KUNIT)
	if (READ_ONCE(rx_loan_kunit_region_pages))
		return READ_ONCE(rx_loan_kunit_region_pages);
#endif
	return page_pool_rx_region_size();
}
EXPORT_SYMBOL_GPL(rx_loan_region_size);

/**
 * rx_loan_consumer_register - a consumer of region loans starts
 * @c: the consumer's record, held until rx_loan_consumer_unregister()
 * @ln: the namespace whose budgets it draws on, or NULL
 * @name: for its statistics
 * @floor: pages it asks the region to hold, when it is carved
 *
 * Registering only counts @floor in what the region is to hold: it is not
 * carved here, as a server registers when it starts, before it starts the
 * threads its floor follows (rx_loan_consumer_set_floor()).  It
 * is carved at the first READ (page_pool_rx_region_bind_reads()), when a
 * listed device's pool is attached, or at a consumer's first connection
 * (rx_loan_region_carve()), whichever comes first.  Every consumer's share is then the region over the consumers: a late
 * consumer gets its share of the region that exists, and the others'
 * shrink to theirs; credit they hold beyond it is not taken back, they
 * wait until it drains under their share.
 *
 * @c's counts of credit out start zeroed (the caller's to clear) and are
 * not reset here: what it still had out when it last unregistered keeps
 * returning to them.  Its copies counter is allocated at the first
 * registration and freed by rx_loan_consumer_destroy().
 *
 * Context: process context; may sleep.
 * Return: 0.
 */
int rx_loan_consumer_register(struct rx_loan_consumer *c,
			      struct rx_loan_net *ln, const char *name,
			      unsigned long floor)
{
	/*
	 * Its own copies counter, kept across registrations.  Without one
	 * its copies count in the host-wide total only.
	 */
	if (!c->copies)
		WRITE_ONCE(c->copies, alloc_percpu(struct rx_loan_copies));
	c->name = name;
	c->floor = floor;
	c->ln = ln;
	atomic_long_set(&c->overrun_share, 0);
	atomic_long_set(&c->share_denied, 0);
	mutex_lock(&rx_loan_consumers_lock);
	page_pool_rx_region_demand(floor);
	list_add_tail(&c->node, &rx_loan_consumers);
	if (ln)
		ln->nr_consumers++;
	WRITE_ONCE(rx_loan_nr_consumers, rx_loan_nr_consumers + 1);
	WRITE_ONCE(c->registered, true);
	mutex_unlock(&rx_loan_consumers_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(rx_loan_consumer_register);

/**
 * rx_loan_consumer_unregister - a consumer of region loans stops
 * @c: as registered
 *
 * Its transports are gone, but credit they lent on (a bounce, a transmit)
 * may still be out: it returns to @c's counts as it lands, so they are
 * kept, and @c must outlive it.  Credit of records abandoned by
 * rx_loan_orphan() under a region stays spent there, and was counted as
 * orphaned then.
 *
 * Context: process context; may sleep.
 */
void rx_loan_consumer_unregister(struct rx_loan_consumer *c)
{
	mutex_lock(&rx_loan_consumers_lock);
	list_del(&c->node);
	if (c->ln)
		c->ln->nr_consumers--;
	WRITE_ONCE(rx_loan_nr_consumers, rx_loan_nr_consumers - 1);
	WRITE_ONCE(c->registered, false);
	page_pool_rx_region_demand(-(long)c->floor);
	mutex_unlock(&rx_loan_consumers_lock);
}
EXPORT_SYMBOL_GPL(rx_loan_consumer_unregister);

/**
 * rx_loan_consumer_destroy - free what registering @c allocated
 * @c: unregistered, nothing left to count to it
 *
 * Context: process context.
 */
void rx_loan_consumer_destroy(struct rx_loan_consumer *c)
{
	WARN_ON_ONCE(READ_ONCE(c->registered));
	free_percpu(c->copies);
	c->copies = NULL;
}
EXPORT_SYMBOL_GPL(rx_loan_consumer_destroy);

/**
 * rx_loan_consumer_set_floor - a consumer's floor follows its size
 * @c: a consumer, registered or not
 * @floor: the pages it now asks the region to hold
 *
 * A server's floor follows its thread count, which it sets after it
 * registers.  Before the region is carved the new floor is what it is
 * carved for; once carved the region does not change, and the floor no
 * longer matters to it (shares are the region over the consumers).
 *
 * Context: process context; may sleep.  Control path only.
 */
void rx_loan_consumer_set_floor(struct rx_loan_consumer *c,
				unsigned long floor)
{
	long delta;

	mutex_lock(&rx_loan_consumers_lock);
	delta = (long)floor - (long)c->floor;
	c->floor = floor;
	if (c->registered && delta)
		page_pool_rx_region_demand(delta);
	mutex_unlock(&rx_loan_consumers_lock);
}
EXPORT_SYMBOL_GPL(rx_loan_consumer_set_floor);

/**
 * rx_loan_region_carve - carve the region for a consumer's first admission
 *
 * Called before a consumer admits its first region-backed record (sunrpc:
 * a new connection): carves the region at the size the registered floors
 * and the waiting devices' rings ask for, when a listed device's pool
 * waits for it.  Lockless once the region exists or nothing waits.
 *
 * Context: process context; may sleep.
 */
void rx_loan_region_carve(void)
{
	if (page_pool_rx_region_exists())
		return;
	page_pool_rx_region_carve();
}
EXPORT_SYMBOL_GPL(rx_loan_region_carve);

/**
 * rx_loan_region_write_default - a namespace's write budget by default
 *
 * The region's write share, its pages less the read split, once a region
 * exists; 0 before.  Lockless: called under a budget's spinlock.
 */
unsigned long rx_loan_region_write_default(void)
{
	unsigned long size, read;

#if IS_ENABLED(CONFIG_KUNIT)
	if (!READ_ONCE(rx_loan_kunit_region_pages))
#endif
		if (!page_pool_rx_region_exists())
			return 0;
	size = rx_loan_region_size();
	read = rx_loan_region_read_pages();
	return size > read ? size - read : 0;
}
EXPORT_SYMBOL_GPL(rx_loan_region_write_default);

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
		if (!budget && READ_ONCE(lb->cfg_default))
			budget = min_t(unsigned long,
				       rx_loan_region_write_default(), UINT_MAX);
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
	/* From the newest: a waiter refused for the first time goes last. */
	list_for_each_entry_reverse(pos, &lb->waiters, node) {
		if (pos->ticket <= ticket) {
			list_add(&w->node, &pos->node);
			return;
		}
	}
	list_add(&w->node, &lb->waiters);
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

	/*
	 * From the newest: a record is admitted only as the oldest waiter or
	 * with nobody waiting, so its ticket is nearly always the newest, and
	 * walking from the oldest would touch every record in flight.
	 */
	list_for_each_entry_reverse(pos, &lb->inflight, node) {
		if (pos->ticket <= cr->ticket) {
			list_add(&cr->node, &pos->node);
			goto linked;
		}
	}
	list_add(&cr->node, &lb->inflight);
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
static bool rx_loan_budget_idle(struct rx_loan_budget *lb)
{
	return list_empty(&lb->inflight) && !rx_loan_read_inflight(lb) &&
	       !rx_loan_stocked_records(lb);
}

/*
 * @w's consumer is past its share for what @w asks, lb->lock held.  Such a
 * request waits for its consumer, not for the namespace: it keeps its
 * place in @lb's FIFO but never heads it, so other consumers are admitted
 * past it, and its consumer's own returns (each of which kicks @lb) bring
 * it back.  With nothing in flight on @lb it heads as any request does,
 * and as the oldest may pass its share.
 */
static bool rx_loan_share_waits(struct rx_loan_budget *lb,
				const struct rx_loan_waiter *w)
{
	const struct rx_loan_consumer *c = w->consumer;
	unsigned long share;

	/*
	 * Only a waiter its share alone refused: one the region or the
	 * budget refused waits for any return, whoever makes it, and the
	 * oldest record, which may pass its share, is never refused by it.
	 * Re-evaluated at every kick: within its share again, it heads.
	 */
	if (!c || !w->share)
		return false;
	share = rx_loan_consumer_share(c);
	return share && atomic_long_read(lb->read ? &c->out_read :
					 &c->out_write) + w->need > (long)share;
}

/* The oldest waiter whose consumer is within its share, lb->lock held. */
static struct rx_loan_waiter *rx_loan_head_locked(struct rx_loan_budget *lb)
{
	struct rx_loan_waiter *pos;
	bool idle;

	if (list_empty(&lb->waiters))
		return NULL;
	idle = rx_loan_budget_idle(lb);
	list_for_each_entry(pos, &lb->waiters, node)
		if (idle || !rx_loan_share_waits(lb, pos))
			return pos;
	return NULL;
}

/*
 * The consumer's share and the region's total, for exact credit of @n
 * pages, @over of them past the budget, lb->lock held: 0, with @n less
 * @over charged to both, or -EAGAIN.  The region must hold all @n, the
 * overrun included; the overrun is not charged, as it is never returned.
 * The share yields to a request with nothing else in flight (@idle).
 */
static int rx_loan_exact_limits_locked(struct rx_loan_budget *lb,
				       struct rx_loan_waiter *w, long n,
				       long over, bool idle)
{
	struct rx_loan_consumer *c = w->consumer;

	if (!rx_loan_consumer_get(c, lb->read, n - over, false)) {
		if (!idle && !rx_loan_budget_idle(lb)) {
			w->share = true;
			goto refuse;
		}
		rx_loan_consumer_get(c, lb->read, n - over, true);
	}
	if (!rx_loan_region_get(n, lb, false, NULL)) {
		rx_loan_consumer_put(c, lb->read, n - over);
		goto refuse;
	}
	if (over) {
		if (!lb->read)
			atomic_long_sub(over, &rx_loan_region.out_write);
		atomic_long_sub(over, &rx_loan_region.out);
	}
	return 0;
refuse:
	/* Returns to stocks must kick now: stop stocking. */
	if (!lb->contended)
		rx_loan_stock_drain_locked(lb);
	return -EAGAIN;
}

static int rx_loan_grant_locked(struct rx_loan_budget *lb,
				struct rx_loan_waiter *w)
{
	long n = w->need, from_avail = n, from_reserve = 0, over = 0;
	bool idle = false;

	w->share = false;	/* unless the share refuses it below */

	if (lb->budget && lb->avail < n && !lb->contended)
		rx_loan_stock_drain_locked(lb);	/* stocked credit first */
	if (!lb->budget) {
		from_avail = 0;
	} else if (lb->avail < n) {
		if (!list_empty(&lb->inflight) || rx_loan_read_inflight(lb) ||
		    rx_loan_stocked_records(lb))
			return -EAGAIN;
		idle = true;
		from_avail = max(lb->avail, 0L);
		from_reserve = min(n - from_avail, max(lb->reserve, 0L));
		over = n - from_avail - from_reserve;
	}
	if (lb->budget &&
	    rx_loan_exact_limits_locked(lb, w, n, over, idle))
		return -EAGAIN;
	if (over)
		lb->overrun++;
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
 * younger.  A request whose consumer is past its share is not a head
 * (rx_loan_share_waits()): it is passed over until its consumer returns.
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

	while ((head = rx_loan_head_locked(lb)) && head->exact) {
		if (rx_loan_grant_locked(lb, head))
			return NULL;
		lb->read_granted++;
		head->ops->pin(head);
		list_move_tail(&head->node, granted);
	}
	if (!head) {
		/* Nobody waits (and any change is applied): stock again. */
		if (list_empty(&lb->waiters))
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

/*
 * Take @n pages of this CPU's stock of @lb for consumer @c, within its
 * share; rx_loan_stock_take() charges them to the region (@carved: see
 * rx_loan_region_get()).
 */
static bool rx_loan_stock_take_charged(struct rx_loan_budget *lb,
				       struct rx_loan_consumer *c, long n,
				       bool *carved)
{
	if (!rx_loan_consumer_get(c, lb->read, n, false))
		return false;
	if (__rx_loan_stock_take(lb, n, carved))
		return true;
	rx_loan_consumer_put(c, lb->read, n);
	return false;
}

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
	/* The region refused someone: idle stocks give back what they hold. */
	if (atomic_read(&rx_loan_region.draining) && !lb->contended)
		rx_loan_stock_drain_locked(lb);
	rx_loan_budget_configure_locked(lb);
	wake = rx_loan_kick_locked(lb, NULL, &granted);
	spin_unlock_bh(&lb->lock);
	rx_loan_wake(wake);
	rx_loan_wake_granted(&granted);
}
EXPORT_SYMBOL_GPL(rx_loan_kick_fn);

/*
 * Admit a record whose credit @cr is to hold, at a charge of @cost, for the
 * transport whose waiter is @w, of consumer @c (NULL: no share).  @warm:
 * admission may take stocked credit (rx_loan_cost_warm(), or a fixed
 * factor).  False: @w waits, and is woken, oldest first, when credit
 * returns.  The credit is charged to @c's share and to the region's total,
 * one atomic each per record; the oldest record may pass the share, never
 * the region.
 */
bool rx_loan_admit(struct rx_loan_budget *lb, struct rx_loan_consumer *c,
		   struct rx_loan_waiter *w, struct rx_loan_credit *cr,
		   u32 cost, bool warm)
{
	struct rx_loan_waiter *head, *wake = NULL, *undo_wake = NULL;
	u32 from_avail, from_reserve;
	bool changed, oldest, tried = false, share_only = false, carved = false;
	LIST_HEAD(granted);

	if (WARN_ON_ONCE(cr->linked))
		return true;
	cr->consumer = c;
	w->consumer = c;	/* for rx_loan_share_waits() */
	/*
	 * A holder is counted before it takes stocked credit and uncounted
	 * only once the credit is back, so a change of configuration that
	 * finds no holders (rx_loan_budget_configure_locked) misses none.
	 */
	if (cost <= RX_LOAN_STOCK_MAX && READ_ONCE(lb->budget) &&
	    !READ_ONCE(lb->contended) && list_empty(&w->node) && warm) {
		tried = true;
		this_cpu_inc(lb->stock->records);
		if (rx_loan_stock_take_charged(lb, c, cost, &carved)) {
			/* This CPU's stock: nobody waits, no order to keep. */
			cr->credit = cost;
			cr->region = carved ? cost : 0;
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
	head = rx_loan_head_locked(lb);
	if (head && head != w)
		goto deny;
	if (lb->avail < cost && !lb->contended)
		rx_loan_stock_drain_locked(lb);	/* stocked credit first */
	oldest = list_empty(&lb->inflight) && !rx_loan_stocked_records(lb);
	if (oldest) {
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
	/* The oldest may pass its share (progress), never the region. */
	if (!rx_loan_consumer_get(c, lb->read, from_avail + from_reserve,
				  oldest)) {
		share_only = true;	/* never the oldest: it may pass */
		goto deny_limit;
	}
	if (!rx_loan_region_get(from_avail + from_reserve, lb, false, &carved)) {
		rx_loan_consumer_put(c, lb->read, from_avail + from_reserve);
		goto deny_limit;
	}
	lb->avail -= from_avail;
	lb->reserve -= from_reserve;
	cr->credit = from_avail + from_reserve;
	cr->region = carved ? cr->credit : 0;
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

deny_limit:
	/* Returns to stocks must kick now: stop stocking. */
	if (!lb->contended)
		rx_loan_stock_drain_locked(lb);
deny:
	w->share = share_only;
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
 * returns.  The growth is charged to the consumer's share (the oldest
 * record may pass it) and to the region's total, which nothing passes.
 */
bool __rx_loan_charge(struct rx_loan_waiter *w, struct rx_loan_credit *cr,
		      u32 held)
{
	struct rx_loan_budget *lb = cr->budget;
	struct rx_loan_consumer *c = cr->consumer;
	struct rx_loan_waiter *wake = NULL;
	LIST_HEAD(granted);
	bool oldest, changed, limited = false, share_only = false;
	bool carved = false;
	long want, take, asked;

	want = held - cr->credit;
	if (cr->stocked && rx_loan_stock_take_charged(lb, c, want, &carved)) {
		cr->credit += want;
		if (carved)
			cr->region += want;
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
		rx_loan_consumer_get(c, lb->read, want, true);
		rx_loan_region_get(want, lb, true, &carved);
		cr->credit += want;
		cr->over += want;
		if (carved)
			cr->region += want;
		want = 0;
	}
	if (want && lb->avail < want && !lb->contended)
		rx_loan_stock_drain_locked(lb);	/* stocked credit first */
	oldest = list_first_entry(&lb->inflight, struct rx_loan_credit,
				  node) == cr;
	if (want) {
		limited = !rx_loan_consumer_get(c, lb->read, want, oldest);
		share_only = limited;	/* never the oldest: it may pass */
		if (!limited && !rx_loan_region_get(want, lb, false, &carved)) {
			rx_loan_consumer_put(c, lb->read, want);
			limited = true;
		}
		/* Returns to stocks must kick now: stop stocking. */
		if (limited && !lb->contended)
			rx_loan_stock_drain_locked(lb);
	}
	if (want && !limited) {
		asked = want;
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
			/* Not taken: give back what was charged for it. */
			rx_loan_consumer_put(c, lb->read, want);
			rx_loan_region_put(want, lb->read);
		}
		if (carved)
			cr->region += asked - want;
	}
	if (want) {
		w->consumer = c;
		w->share = share_only;
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
 * (rx_loan_cost_learn_locked()).  The consumer's share gets all of it back
 * at once, the lent part included; the region gets back what returns to
 * the budget, and the lent part when the bounce returns it
 * (rx_loan_region_put()).
 */
void rx_loan_return(struct rx_loan_credit *cr, unsigned int sample, u32 lend,
		    struct rx_loan_lend *out)
{
	struct rx_loan_consumer *c = cr->consumer;
	struct rx_loan_budget *lb;
	struct rx_loan_waiter *wake;
	LIST_HEAD(granted);
	long refund;

	if (!cr->linked)
		return;
	lb = cr->budget;
	/* The lent part stays charged until the borrower returns it. */
	rx_loan_consumer_put(c, lb->read, (long)cr->credit - lend);
	cr->consumer = NULL;
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
			out->consumer = c;
			out->credit = lend;
			out->reserve = 0;
			out->stocked = true;
		}
		cr->credit = 0;
		cr->region = 0;
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
		refund = 0;	/* rx_loan_stock_put() refunded it */
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
		out->consumer = c;
		out->credit = normal;
		out->reserve = reserve;
	}
	list_del(&cr->node);
	lb->inflight_count--;
	lb->reserve += cr->reserve;
	lb->avail += cr->credit - cr->reserve - cr->over;
	refund = cr->credit;
	rx_loan_cost_learn_locked(lb, sample);
	cr->credit = 0;
	cr->region = 0;
	cr->reserve = 0;
	cr->over = 0;
	cr->linked = false;
	cr->budget = NULL;
returned:
	rx_loan_budget_configure_locked(lb);
	wake = rx_loan_kick_locked(lb, NULL, &granted);
	spin_unlock_bh(&lb->lock);
	rx_loan_region_put(refund, lb->read);
	rx_loan_wake(wake);
	rx_loan_wake_granted(&granted);
}
EXPORT_SYMBOL_GPL(rx_loan_return);

/*
 * A record whose loaned pages could not be accounted for is abandoned, @held
 * of its pages possibly still out: keep that much of its credit spent, but
 * take it out of the in-flight order so the next record can be the oldest
 * and draw the reserve.  The rest came back with its pages and is returned
 * as rx_loan_return() would: over first, then reserve, then ordinary.  Of
 * the kept part, only what was charged while a region was in place can hold
 * region pages: that stays spent in its consumer's share and in the
 * region's total too.  The rest, charged before the region was carved (or
 * after its carve failed), holds no region page: it goes back to both, or
 * the region, carved later, would keep it spent for good.  So does all of
 * it if the region is no longer in place (only a test's stand-in is ever
 * taken away).
 */
void rx_loan_orphan(struct rx_loan_credit *cr, u32 held)
{
	struct rx_loan_budget *lb;
	struct rx_loan_waiter *wake;
	u32 keep, back, over, reserve;
	bool stocked = cr->stocked;
	long kept, refund, spill = 0;
	LIST_HEAD(granted);

	if (!cr->linked)
		return;
	lb = cr->budget;
	keep = min(held, cr->credit);
	back = cr->credit - keep;
	over = min(back, cr->over);
	reserve = min(back - over, cr->reserve);
	kept = rx_loan_region_carved() ? min_t(long, keep, cr->region) : 0;
	refund = cr->credit - kept;
	rx_loan_consumer_put(cr->consumer, lb->read, refund);
	/* A stocked record's credit is all ordinary: to the stock, as returned. */
	if (stocked && back) {
		spill = rx_loan_stock_put(lb, back);
		refund -= back;	/* rx_loan_stock_put() refunded it */
	}
	spin_lock_bh(&lb->lock);
	if (stocked) {
		/* Not on the in-flight list: only its holder count goes. */
		this_cpu_dec(lb->stock->records);
		cr->stocked = false;
		lb->avail += spill;
	} else {
		list_del(&cr->node);
		lb->inflight_count--;
		lb->reserve += reserve;
		lb->avail += back - over - reserve;
	}
	lb->orphaned += keep - min(keep, cr->over - over);
	atomic_long_add(kept, &rx_loan_region.orphaned);
	cr->credit = 0;
	cr->region = 0;
	cr->reserve = 0;
	cr->over = 0;
	cr->linked = false;
	cr->budget = NULL;
	cr->consumer = NULL;
	wake = rx_loan_kick_locked(lb, NULL, &granted);
	spin_unlock_bh(&lb->lock);
	rx_loan_region_put(refund, lb->read);
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
 * Exact credit for @w->need pages now, for consumer @c (NULL: no share),
 * from this CPU's stock or under the lock: 0, recorded in @w, or -EAGAIN
 * when credit, @c's share or the region is short or anyone waits, for the
 * caller to rx_loan_park() @w (or a copy of it).
 */
int rx_loan_grant(struct rx_loan_budget *lb, struct rx_loan_consumer *c,
		  struct rx_loan_waiter *w)
{
	struct rx_loan_waiter *wake = NULL, *undo_wake = NULL;
	bool changed, tried = false;
	LIST_HEAD(granted);
	int ret;

	w->consumer = c;
	if (w->need <= RX_LOAN_STOCK_MAX && READ_ONCE(lb->budget) &&
	    !READ_ONCE(lb->contended)) {
		tried = true;
		/* Counted first: see rx_loan_admit(). */
		this_cpu_inc(lb->stock->read_inflight);
		if (rx_loan_stock_take_charged(lb, c, w->need, NULL)) {
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
	if (rx_loan_head_locked(lb))
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
 * Park the exact request @w of consumer @c on @lb until it is granted: its
 * pin and wake run then.  It is linked under the same lock hold that
 * re-checks credit, so a return that raced the first attempt grants it at
 * once.  @w may be freed before this returns.
 */
void rx_loan_park(struct rx_loan_budget *lb, struct rx_loan_consumer *c,
		  struct rx_loan_waiter *w)
{
	struct rx_loan_waiter *wake;
	LIST_HEAD(granted);

	w->exact = true;
	w->consumer = c;
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

/*
 * Return an exact grant's credit (not its overrun) and pass it on: to @c's
 * share and, as it leaves the stock, to the region's total.
 */
void rx_loan_grant_return(struct rx_loan_budget *lb, struct rx_loan_consumer *c,
			  long credit, long reserve)
{
	struct rx_loan_waiter *wake;
	LIST_HEAD(granted);
	long refund = credit + reserve;

	rx_loan_consumer_put(c, lb->read, credit + reserve);
	this_cpu_inc(lb->stock->read_completed);
	/* Credit from avail goes to this CPU's stock while nobody waits. */
	if (!reserve && credit) {
		credit = rx_loan_stock_put(lb, credit);
		refund = 0;	/* rx_loan_stock_put() refunded it */
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
	rx_loan_region_put(refund, lb->read);
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
void rx_loan_grant_drop(struct rx_loan_budget *lb, struct rx_loan_consumer *c,
			long credit, long reserve)
{
	rx_loan_consumer_put(c, lb->read, credit + reserve);
	spin_lock_bh(&lb->lock);
	lb->avail += credit;
	lb->reserve += reserve;
	this_cpu_dec(lb->stock->read_inflight);
	spin_unlock_bh(&lb->lock);
	/* No kick from the region here either: kick_work passes it on. */
	if (!lb->read)
		atomic_long_sub(credit + reserve, &rx_loan_region.out_write);
	atomic_long_sub(credit + reserve, &rx_loan_region.out);
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

/**
 * rx_loan_budget_init - set up a budget the region's returns kick
 * @lb: zeroed
 * @read: the READ (exact) budget, sized to the full read pool
 *
 * Context: process context; may sleep.
 * Return: 0 or -ENOMEM.
 */
int rx_loan_budget_init(struct rx_loan_budget *lb, bool read)
{
	struct rx_loan_stock __percpu *stock;

	stock = alloc_percpu(struct rx_loan_stock);
	if (!stock)
		return -ENOMEM;
	lb->read = read;
	spin_lock_init(&lb->lock);
	lb->next_ticket = 1;
	INIT_LIST_HEAD(&lb->inflight);
	INIT_LIST_HEAD(&lb->waiters);
	INIT_WORK(&lb->kick_work, rx_loan_kick_fn);
	/* A reader that sees @stock may take the lock and walk the lists. */
	smp_store_release(&lb->stock, stock);
	mutex_lock(&rx_loan_consumers_lock);
	list_add_tail_rcu(&lb->region_node, &rx_loan_budgets);
	mutex_unlock(&rx_loan_consumers_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(rx_loan_budget_init);

/*
 * Tear down @lb, nothing in flight: its stocks go back, and with them
 * their charge to the region.  @stock is cleared under the lock, so a
 * reader that takes the lock and still sees it may sum it.
 */
void rx_loan_budget_destroy(struct rx_loan_budget *lb)
{
	struct rx_loan_stock __percpu *stock;

	if (!lb->stock)
		return;
	mutex_lock(&rx_loan_consumers_lock);
	list_del_rcu(&lb->region_node);
	mutex_unlock(&rx_loan_consumers_lock);
	synchronize_rcu();
	flush_work(&lb->kick_work);
	spin_lock_bh(&lb->lock);
	WARN_ON_ONCE(!list_empty(&lb->inflight) ||
		     !list_empty(&lb->waiters) || lb->lent);
	rx_loan_stock_drain_locked(lb);
	stock = lb->stock;
	WRITE_ONCE(lb->stock, NULL);
	spin_unlock_bh(&lb->lock);
	free_percpu(stock);
}
EXPORT_SYMBOL_GPL(rx_loan_budget_destroy);

/*
 * Request @pages and @reserve of @ln's write budget (@off: turned off),
 * and size its READ budget to what that leaves of the read pool.
 */
void rx_loan_net_write_request(struct rx_loan_net *ln, unsigned long pages,
			       unsigned int reserve, bool off)
{
	rx_loan_budget_request(&ln->write, pages, reserve, off);
	rx_loan_budget_request(&ln->read, off ? 0 : pages, 0, false);
}
EXPORT_SYMBOL_GPL(rx_loan_net_write_request);

/* A namespace's budgets, or a stand-in's (KUnit). */
int rx_loan_net_state_init(struct rx_loan_net *ln)
{
	int ret;

	ret = rx_loan_budget_init(&ln->write, false);
	if (ret)
		return ret;
	ret = rx_loan_budget_init(&ln->read, true);
	if (ret) {
		rx_loan_budget_destroy(&ln->write);
		return ret;
	}
	rx_loan_net_write_request(ln, 0, 0, false);
	return 0;
}
EXPORT_SYMBOL_GPL(rx_loan_net_state_init);

void rx_loan_net_state_exit(struct rx_loan_net *ln)
{
	WARN_ON_ONCE(ln->nr_consumers);
	rx_loan_budget_destroy(&ln->read);
	rx_loan_budget_destroy(&ln->write);
}
EXPORT_SYMBOL_GPL(rx_loan_net_state_exit);

static unsigned int rx_loan_net_id __read_mostly;

struct rx_loan_net *rx_loan_net(const struct net *net)
{
	return net_generic(net, rx_loan_net_id);
}
EXPORT_SYMBOL_GPL(rx_loan_net);

static int __net_init rx_loan_net_init(struct net *net)
{
	struct rx_loan_net *ln = rx_loan_net(net);
	int ret = rx_loan_net_state_init(ln);

	/* Until configured, the write budget is the region's write share. */
	if (!ret)
		WRITE_ONCE(ln->write.cfg_default, true);
	return ret;
}

static void __net_exit rx_loan_net_exit(struct net *net)
{
	rx_loan_net_state_exit(rx_loan_net(net));
}

static struct pernet_operations rx_loan_net_ops = {
	.init	= rx_loan_net_init,
	.exit	= rx_loan_net_exit,
	.id	= &rx_loan_net_id,
	.size	= sizeof(struct rx_loan_net),
};

static int __init rx_loan_init(void)
{
	return register_pernet_subsys(&rx_loan_net_ops);
}
subsys_initcall(rx_loan_init);
