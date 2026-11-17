// SPDX-License-Identifier: GPL-2.0
/*
 * Recycling of the pages a TCP reply was sent from.
 *
 * A large reply's pages are filled on one CPU (the work item or thread that
 * ran the call, in svc_alloc_arg()) and freed on another (the CPU whose
 * softirq takes the ACK that releases the last skb).  The pages then always
 * cross between CPUs: the sending CPU's per-CPU free lists are empty when it
 * fills, so every fill takes the zone lock, and the ACKing CPU's lists
 * overflow, so every free drains under it again.  With
 * svc_tcp_reply_recycle set, svc_tcp_sendmsg() sends such a reply's pages
 * zero copy under a descriptor of its own (struct svc_reply_tx, holding the
 * rqst's references to the pages), and the descriptor's completion hands the
 * pages back to the sending CPU's cache, from which svc_alloc_arg() takes them
 * before it calls the page allocator.  The ACKing CPU does one llist_add() a
 * reply instead of a page free a page, and nothing moves to the sending CPU's
 * softirq.
 *
 * The skbs hold no page references (SKBFL_MANAGED_FRAG_REFS, through
 * rx_loan_tx_sg_from_iter()): the descriptor's reference keeps the pages
 * until TCP frees the last skb that carries any of them, which is when the
 * completion runs.  A page someone else still references when it is taken
 * from the cache (an skb whose frags were downgraded to references) is let go
 * instead of reused.
 *
 * A CPU's cache holds at most svc_tcp_reply_recycle pages; a completion that
 * finds its CPU's cache full frees the pages as before.  A shrinker empties
 * the caches under memory pressure, and setting the limit to 0 empties them.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/percpu.h>
#include <linux/llist.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/shrinker.h>
#include <linux/skbuff.h>
#include <linux/sunrpc/svc.h>
#include <net/rx_loan.h>
#include <linux/delay.h>
#include <kunit/visibility.h>

#include "svc_reply_pages.h"

/* A reply's payload of at least this many bytes is recycled. */
#define SVC_REPLY_RECYCLE_MIN	(64 * 1024)
/* Empty descriptors a CPU keeps for its next sends. */
#define SVC_REPLY_SPARE_MAX	64
/* Replies one chain of shared skbs holds, the head among them. */
#define SVC_REPLY_CHAIN_MAX	32

struct svc_reply_cache {
	spinlock_t		lock;	/* every consumer: fill, send, trim */
	struct llist_head	ready;	/* completed sends, their pages kept */
	struct svc_reply_tx	*cur;	/* pages taken from its end */
	struct svc_reply_tx	*spare;	/* empty descriptors, by ->next */
	unsigned int		nspare;
	/* Pages in ready and cur: added by any CPU's completions. */
	atomic_t		pages ____cacheline_aligned_in_smp;
};

/* Counters, per CPU: some are counted in a completion's softirq. */
struct svc_reply_stats {
	unsigned long	sends;		/* replies sent under a descriptor */
	unsigned long	sent_pages;	/* pages those replies handed over */
	unsigned long	reused;		/* pages a fill took from a cache */
	unsigned long	busy;		/* taken from a cache still referenced */
	unsigned long	over;		/* pages freed by a full cache */
	unsigned long	trimmed;	/* pages freed by trim or the shrinker */
	unsigned long	no_desc;	/* replies spliced for want of one */
	unsigned long	copied;		/* sends TCP copied, not zero copy */
	long		live;		/* descriptors made less completed */
};

static DEFINE_PER_CPU(struct svc_reply_cache, svc_reply_cache);
static DEFINE_PER_CPU(struct svc_reply_stats, svc_reply_stats);
static unsigned int svc_reply_recycle __read_mostly;
static struct shrinker *svc_reply_shrinker;

#define svc_reply_stat_add(f, n)	this_cpu_add(svc_reply_stats.f, (n))
#define svc_reply_stat_inc(f)		this_cpu_inc(svc_reply_stats.f)

static void svc_reply_tx_complete(struct sk_buff *skb, struct ubuf_info *uarg,
				  bool zerocopy_success);
static int svc_reply_tx_link_skb(struct sk_buff *skb, struct ubuf_info *uarg);

static const struct ubuf_info_ops svc_reply_tx_ops = {
	.complete	= svc_reply_tx_complete,
	.link_skb	= svc_reply_tx_link_skb,
};

static void svc_reply_put_pages(struct page **pages, unsigned int n)
{
	while (n--)
		put_page(pages[n]);
}

/* Under @c's lock. */
static void svc_reply_spare_put(struct svc_reply_cache *c,
				struct svc_reply_tx *tx)
{
	if (c->nspare >= SVC_REPLY_SPARE_MAX) {
		kfree(tx);
		return;
	}
	tx->next = c->spare;
	c->spare = tx;
	c->nspare++;
}

/* Under @c's lock: free what @c keeps, and return its pages so freed. */
static unsigned long svc_reply_cache_empty(struct svc_reply_cache *c)
{
	struct llist_node *node = llist_del_all(&c->ready);
	struct svc_reply_tx *tx, *next;
	unsigned long freed = 0;

	if (c->cur) {
		freed += c->cur->n;
		svc_reply_put_pages(c->cur->pages, c->cur->n);
		kfree(c->cur);
		c->cur = NULL;
	}
	llist_for_each_entry_safe(tx, next, node, node) {
		freed += tx->n;
		svc_reply_put_pages(tx->pages, tx->n);
		kfree(tx);
	}
	atomic_sub(freed, &c->pages);
	while ((tx = c->spare)) {
		c->spare = tx->next;
		kfree(tx);
	}
	c->nspare = 0;
	return freed;
}

static unsigned long svc_reply_trim_all(void)
{
	unsigned long freed = 0;
	int cpu;

	for_each_possible_cpu(cpu) {
		struct svc_reply_cache *c = per_cpu_ptr(&svc_reply_cache, cpu);

		spin_lock(&c->lock);
		freed += svc_reply_cache_empty(c);
		spin_unlock(&c->lock);
	}
	svc_reply_stat_add(trimmed, freed);
	return freed;
}

/* Is @page one svc_alloc_arg() could have filled, and only ours? */
static bool svc_reply_page_plain(struct page *page)
{
	return !page->mapping && !PageCompound(page) &&
	       !page_pool_page_is_pp(page);
}

/**
 * svc_reply_pages_fill - Fill empty page slots from this CPU's cache
 * @pages: an rqst's page array
 * @npages: its slots to fill; those not NULL are left as they are
 *
 * Takes pages completed replies handed back to this CPU, so that the page
 * allocator is called only for what the cache does not hold.
 */
void svc_reply_pages_fill(struct page **pages, unsigned long npages)
{
	struct svc_reply_cache *c;
	unsigned long i, taken = 0, reused = 0, busy = 0;
	struct llist_node *node;
	struct page *page;

	c = get_cpu_ptr(&svc_reply_cache);
	if (!atomic_read(&c->pages))
		goto out;
	spin_lock(&c->lock);
	for (i = 0; i < npages; i++) {
		if (pages[i])
			continue;
		for (;;) {
			while (!c->cur || !c->cur->n) {
				if (c->cur)
					svc_reply_spare_put(c, c->cur);
				node = llist_del_first(&c->ready);
				if (!node) {
					c->cur = NULL;
					goto unlock;
				}
				c->cur = llist_entry(node, struct svc_reply_tx,
						     node);
			}
			page = c->cur->pages[--c->cur->n];
			taken++;
			if (page_ref_count(page) == 1 &&
			    svc_reply_page_plain(page))
				break;
			/* Still someone else's too: let it go with them. */
			put_page(page);
			busy++;
		}
		pages[i] = page;
		reused++;
	}
unlock:
	atomic_sub(taken, &c->pages);
	spin_unlock(&c->lock);
	svc_reply_stat_add(reused, reused);
	svc_reply_stat_add(busy, busy);
out:
	put_cpu_ptr(&svc_reply_cache);
}

static struct svc_reply_tx *svc_reply_tx_get(unsigned int need)
{
	struct svc_reply_cache *c = get_cpu_ptr(&svc_reply_cache);
	struct svc_reply_tx *tx;

	spin_lock(&c->lock);
	tx = c->spare;
	if (tx) {
		c->spare = tx->next;
		c->nspare--;
	}
	spin_unlock(&c->lock);
	put_cpu_ptr(&svc_reply_cache);
	if (tx && tx->size < need) {
		kfree(tx);
		tx = NULL;
	}
	if (!tx) {
		/* A work item must not reclaim: nothing but a plain send lost. */
		tx = kmalloc(struct_size(tx, pages, need),
			     GFP_NOWAIT | __GFP_NOWARN);
		if (!tx)
			return NULL;
		tx->size = need;
	}
	return tx;
}

/* Is the page of @bv one of @tx's? */
static bool svc_reply_tx_has(const struct svc_reply_tx *tx,
			     const struct bio_vec *bv, unsigned int *hint)
{
	unsigned int i, k;

	for (i = 0; i < tx->n; i++) {
		k = (*hint + i) % tx->n;
		if (tx->pages[k] == bv->bv_page) {
			*hint = k;
			return true;
		}
	}
	return false;
}

/**
 * svc_reply_tx_take - Hand a reply's pages to a send descriptor
 * @rqstp: the reply's rqst
 * @bvec: the reply as it is to be sent, without its record marker
 * @nvecs: entries in @bvec
 *
 * The reply qualifies when recycling is on, its payload is large enough,
 * and every page @bvec names is one of the rqst's reply pages
 * [rq_respages, rq_next_page) as svc_alloc_arg() filled them.  Those pages
 * then move to the descriptor, their slots left NULL (so
 * svc_rqst_release_pages() skips them and svc_alloc_arg() fills them again),
 * with the rqst's references.
 *
 * Return: the descriptor, holding one reference the caller drops with
 * net_zcopy_put() once the send is queued; NULL to send as before.
 */
struct svc_reply_tx *svc_reply_tx_take(struct svc_rqst *rqstp,
				       const struct bio_vec *bvec,
				       unsigned int nvecs)
{
	unsigned int need = rqstp->rq_next_page - rqstp->rq_respages;
	unsigned int i, hint = 0;
	struct svc_reply_tx *tx;
	struct page **pp;

	if (!READ_ONCE(svc_reply_recycle) ||
	    rqstp->rq_res.page_len < SVC_REPLY_RECYCLE_MIN || !need ||
	    rqstp->rq_next_page > rqstp->rq_page_end)
		return NULL;
	tx = svc_reply_tx_get(need);
	if (!tx) {
		svc_reply_stat_inc(no_desc);
		return NULL;
	}
	tx->n = 0;
	for (pp = rqstp->rq_respages; pp < rqstp->rq_next_page; pp++) {
		if (!*pp)
			continue;
		if (!svc_reply_page_plain(*pp))
			goto keep;
		tx->pages[tx->n++] = *pp;
	}
	for (i = 0; i < nvecs; i++)
		if (!svc_reply_tx_has(tx, &bvec[i], &hint))
			goto keep;

	for (pp = rqstp->rq_respages; pp < rqstp->rq_next_page; pp++)
		*pp = NULL;
	tx->ubuf.ops = &svc_reply_tx_ops;
	tx->ubuf.flags = SKBFL_ZEROCOPY_FRAG | SKBFL_DONT_ORPHAN;
	refcount_set(&tx->ubuf.refcnt, 1);
	tx->copied = false;
	tx->chain_head = NULL;
	tx->chain_next = NULL;
	tx->chain_nr = 0;
	tx->cpu = raw_smp_processor_id();
	svc_reply_stat_inc(sends);
	svc_reply_stat_add(sent_pages, tx->n);
	svc_reply_stat_inc(live);
	return tx;
keep:
	/* Not all ours to recycle: the rqst keeps them, the send splices. */
	tx->n = 0;
	kfree(tx);
	return NULL;
}

/* A send TCP is done with: its pages to its CPU's cache, or freed. */
static void svc_reply_tx_done(struct svc_reply_tx *tx)
{
	unsigned int cap = READ_ONCE(svc_reply_recycle);
	struct svc_reply_cache *c;

	if (READ_ONCE(tx->copied))
		svc_reply_stat_inc(copied);
	svc_reply_stat_add(live, -1);
	c = per_cpu_ptr(&svc_reply_cache, tx->cpu);
	if (atomic_add_return(tx->n, &c->pages) > cap) {
		atomic_sub(tx->n, &c->pages);
		svc_reply_stat_add(over, tx->n);
		svc_reply_put_pages(tx->pages, tx->n);
		kfree(tx);
		return;
	}
	llist_add(&tx->node, &c->ready);
}

/*
 * ubuf_info_ops.complete: one call for each skb that carried the send and
 * one for the sender's reference.  At the last, a send linked behind a
 * chain's head drops the reference it holds on the head; a head releases
 * itself and every send linked behind it, all of whose own references are
 * gone by then.  In softirq or process context.
 */
static void svc_reply_tx_complete(struct sk_buff *skb, struct ubuf_info *uarg,
				  bool zerocopy_success)
{
	struct svc_reply_tx *tx = container_of(uarg, struct svc_reply_tx, ubuf);
	struct svc_reply_tx *next;

	if (!zerocopy_success)
		WRITE_ONCE(tx->copied, true);
	if (!refcount_dec_and_test(&uarg->refcnt))
		return;
	if (tx->chain_head) {
		svc_reply_tx_complete(skb, &tx->chain_head->ubuf,
				      zerocopy_success);
		return;
	}
	for (; tx; tx = next) {
		next = tx->chain_next;
		svc_reply_tx_done(tx);
	}
}

/*
 * ubuf_info_ops.link_skb, under the socket lock: let consecutive replies
 * share skbs, as io_uring's notifications do (io_link_skb()), so that TCP
 * builds the same full segments it does from spliced replies instead of
 * ending each reply's data in an skb of its own.  A reply whose data lands
 * on an skb that carries an earlier reply's joins that reply's chain and
 * takes a reference on the chain's head for itself; the skb keeps the
 * earlier reply's ubuf_info, and the whole chain is released once the last
 * skb carrying any of it is freed.  Only replies, and at most
 * SVC_REPLY_CHAIN_MAX in a chain: that bounds the pages one chain holds.
 */
static int svc_reply_tx_link_skb(struct sk_buff *skb, struct ubuf_info *uarg)
{
	struct svc_reply_tx *tx = container_of(uarg, struct svc_reply_tx, ubuf);
	struct ubuf_info *prev_uarg = skb_zcopy(skb);
	struct svc_reply_tx *prev, *head;

	if (!prev_uarg) {
		net_zcopy_get(uarg);
		skb_zcopy_init(skb, uarg);
		return 0;
	}
	if (prev_uarg == uarg)
		return 0;
	if (prev_uarg->ops != &svc_reply_tx_ops ||
	    skb_shinfo(skb)->nr_frags >= MAX_SKB_FRAGS)
		return -EEXIST;
	/* A send joins a chain once, before any send joins behind it. */
	if (tx->chain_head || tx->chain_next)
		return -EEXIST;
	prev = container_of(prev_uarg, struct svc_reply_tx, ubuf);
	head = prev->chain_head ?: prev;
	if (head->chain_nr + 1 >= SVC_REPLY_CHAIN_MAX)
		return -EEXIST;
	head->chain_nr++;
	tx->chain_head = head;
	tx->chain_next = head->chain_next;
	head->chain_next = tx;
	/* @head is held by @skb, through @prev: take this send's own. */
	net_zcopy_get(&head->ubuf);
	return 0;
}

static unsigned long svc_reply_shrink_count(struct shrinker *s,
					    struct shrink_control *sc)
{
	unsigned long n = 0;
	int cpu;

	for_each_possible_cpu(cpu)
		n += atomic_read(&per_cpu_ptr(&svc_reply_cache, cpu)->pages);
	return n ?: SHRINK_EMPTY;
}

static unsigned long svc_reply_shrink_scan(struct shrinker *s,
					   struct shrink_control *sc)
{
	return svc_reply_trim_all() ?: SHRINK_STOP;
}

static int svc_reply_recycle_set(const char *val,
				 const struct kernel_param *kp)
{
	unsigned int n;
	int ret = kstrtouint(val, 0, &n);

	if (ret)
		return ret;
	WRITE_ONCE(svc_reply_recycle, n);
	if (!n)
		svc_reply_trim_all();
	return 0;
}

static const struct kernel_param_ops svc_reply_recycle_ops = {
	.set	= svc_reply_recycle_set,
	.get	= param_get_uint,
};
module_param_cb(svc_tcp_reply_recycle, &svc_reply_recycle_ops,
		&svc_reply_recycle, 0644);
MODULE_PARM_DESC(svc_tcp_reply_recycle,
		 "Pages of sent TCP replies each CPU keeps to fill the next replies from (0: off, the pages are freed as before; setting 0 frees what the CPUs keep)");

static int svc_reply_stats_get(char *buf, const struct kernel_param *kp)
{
	struct svc_reply_stats s = {};
	unsigned long cached = 0;
	int cpu;

	for_each_possible_cpu(cpu) {
		const struct svc_reply_stats *p =
			per_cpu_ptr(&svc_reply_stats, cpu);

		s.sends += READ_ONCE(p->sends);
		s.sent_pages += READ_ONCE(p->sent_pages);
		s.reused += READ_ONCE(p->reused);
		s.busy += READ_ONCE(p->busy);
		s.over += READ_ONCE(p->over);
		s.trimmed += READ_ONCE(p->trimmed);
		s.no_desc += READ_ONCE(p->no_desc);
		s.copied += READ_ONCE(p->copied);
		s.live += READ_ONCE(p->live);
		cached += atomic_read(&per_cpu_ptr(&svc_reply_cache,
						   cpu)->pages);
	}
	return sysfs_emit(buf,
			  "sends %lu sent_pages %lu reused %lu busy %lu over %lu trimmed %lu no_desc %lu copied %lu live %ld cached %lu\n",
			  s.sends, s.sent_pages, s.reused, s.busy, s.over,
			  s.trimmed, s.no_desc, s.copied, s.live, cached);
}

static const struct kernel_param_ops svc_reply_stats_ops = {
	.get	= svc_reply_stats_get,
};
module_param_cb(svc_tcp_reply_recycle_stats, &svc_reply_stats_ops, NULL,
		0444);
MODULE_PARM_DESC(svc_tcp_reply_recycle_stats,
		 "Reply page recycling since the module loaded: replies and pages sent under a descriptor, pages reused, let go still referenced, freed by a full cache or a trim, replies spliced for want of a descriptor, sends TCP copied, descriptors not yet completed, pages cached");

static long svc_reply_live(void)
{
	long live = 0;
	int cpu;

	for_each_possible_cpu(cpu)
		live += READ_ONCE(per_cpu_ptr(&svc_reply_stats, cpu)->live);
	return live;
}

int __init svc_reply_pages_init(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct svc_reply_cache *c = per_cpu_ptr(&svc_reply_cache, cpu);

		spin_lock_init(&c->lock);
		init_llist_head(&c->ready);
	}
	svc_reply_shrinker = shrinker_alloc(0, "sunrpc-reply-pages");
	if (!svc_reply_shrinker)
		return -ENOMEM;
	svc_reply_shrinker->count_objects = svc_reply_shrink_count;
	svc_reply_shrinker->scan_objects = svc_reply_shrink_scan;
	shrinker_register(svc_reply_shrinker);
	return 0;
}

/*
 * Every server is gone, but a closed socket's skbs can outlive it (an
 * orphaned socket retransmits until it gives up): their completions call
 * into this module, so wait for the last of them.
 */
void svc_reply_pages_exit(void)
{
	unsigned long start = jiffies, next = start + 10 * HZ;
	long live;

	WRITE_ONCE(svc_reply_recycle, 0);
	while ((live = svc_reply_live()) > 0) {
		if (time_after(jiffies, next)) {
			pr_warn("sunrpc: %ld sent replies still held by TCP after %lus\n",
				live, (jiffies - start) / HZ);
			next = jiffies + 10 * HZ;
		}
		msleep(20);
	}
	shrinker_free(svc_reply_shrinker);
	svc_reply_trim_all();
}

#if IS_ENABLED(CONFIG_KUNIT)
EXPORT_SYMBOL_IF_KUNIT(svc_reply_pages_fill);
EXPORT_SYMBOL_IF_KUNIT(svc_reply_tx_take);

/* Set svc_tcp_reply_recycle as its parameter would; return the old value. */
unsigned int svc_reply_kunit_set(unsigned int pages)
{
	unsigned int old = READ_ONCE(svc_reply_recycle);

	WRITE_ONCE(svc_reply_recycle, pages);
	if (!pages)
		svc_reply_trim_all();
	return old;
}
EXPORT_SYMBOL_IF_KUNIT(svc_reply_kunit_set);

/* Pages @cpu's cache holds. */
unsigned int svc_reply_kunit_cached(int cpu)
{
	return atomic_read(&per_cpu_ptr(&svc_reply_cache, cpu)->pages);
}
EXPORT_SYMBOL_IF_KUNIT(svc_reply_kunit_cached);

/* The counters, summed over the CPUs. */
void svc_reply_kunit_stats(struct svc_reply_kunit_stats *k)
{
	int cpu;

	memset(k, 0, sizeof(*k));
	for_each_possible_cpu(cpu) {
		const struct svc_reply_stats *p =
			per_cpu_ptr(&svc_reply_stats, cpu);

		k->sends += READ_ONCE(p->sends);
		k->reused += READ_ONCE(p->reused);
		k->busy += READ_ONCE(p->busy);
		k->over += READ_ONCE(p->over);
		k->live += READ_ONCE(p->live);
	}
}
EXPORT_SYMBOL_IF_KUNIT(svc_reply_kunit_stats);
#endif
