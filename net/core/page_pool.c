/* SPDX-License-Identifier: GPL-2.0
 *
 * page_pool.c
 *	Author:	Jesper Dangaard Brouer <netoptimizer@brouer.com>
 *	Copyright (C) 2016 Red Hat, Inc.
 */

#include <linux/error-injection.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/debugfs.h>
#include <linux/moduleparam.h>
#include <linux/device.h>
#include <kunit/visibility.h>

#include <net/hotdata.h>
#include <net/netdev_lock.h>
#include <net/netdev_rx_queue.h>
#include <net/page_pool/cfg.h>
#include <net/page_pool/helpers.h>
#include <net/page_pool/memory_provider.h>
#include <net/xdp.h>

#include <linux/dma-direction.h>
#include <linux/dma-mapping.h>
#include <linux/page-flags.h>
#include <linux/mm.h> /* for put_page() */
#include <linux/poison.h>
#include <linux/ethtool.h>
#include <linux/netdevice.h>

#include <trace/events/page_pool.h>

#include "dev.h"
#include "mp_dmabuf_devmem.h"
#include "netmem_priv.h"
#include "page_pool_priv.h"

DEFINE_STATIC_KEY_FALSE(page_pool_mem_providers);

#define DEFER_TIME (msecs_to_jiffies(1000))
#define DEFER_WARN_INTERVAL (60 * HZ)

#define BIAS_MAX	(LONG_MAX >> 1)

/*
 * A pool's network device is named by its parent bus device (dev_name(), for
 * example a PCI address or "virtio0"), which exists before a driver creates
 * pools at probe and survives renames, or by its own name when it has no
 * parent.  A pool created without a netdev is named by its DMA device: the
 * same PCI address for a physical NIC, the interface itself for veth.
 *
 * Optional fixed receive-buffer region.  When rx_region_dev lists network
 * devices (comma-separated) and rx_region_pages is non-zero, every order-0
 * pool created for a listed device allocates only from one physically
 * contiguous region that all listed devices share, allocated when the first
 * such pool is created, and its pages return to the region instead of the
 * page allocator.  rx_region_read_pages splits the last pages of the region
 * off as a read partition for NFSD READ payloads; the listed devices' pools
 * use the rest (the write partition).  Without a split both directions share
 * the one partition.  Each partition has its own free list, lock and
 * statistics.  An empty partition fails the allocation: that is the stall.
 * A page another holder still references when it is returned leaves the
 * region for good and is counted as escaped; it is never replaced.
 *
 * The region outlives its pools, so that taking a port down and up, or
 * resizing its rings, finds the same memory again and needs no second
 * contiguous allocation.  It is given back only once no pool is bound to it,
 * no READ consumer has bound it (page_pool_rx_region_bind_reads(), which
 * holds it for good), and it no longer matches what is asked for:
 * rx_region_pages has changed, or rx_region_dev is empty.  That is checked
 * when the last pool bound to it is destroyed and again when the next pool
 * of a listed device is created, so a region is resized by taking every
 * listed device's pools away (the ports down), writing the new size, and
 * bringing them back.  A pool is not destroyed while any of its pages is
 * still out, a loaned one included, so by then every page is either back in
 * the region or escaped.
 */
#define RX_REGION_DEV_LEN	256
/*
 * Supported tunings: module parameters (page_pool.rx_region_* on the kernel
 * command line, /sys/module/page_pool/parameters/ at run time) and the same
 * settings in debugfs page_pool_rx_region_cfg/, both through one set of
 * kernel_param_ops, so either reads back what the other wrote.  A size
 * reads back as "<value> (auto)" or "<value> (manual)" and 0 returns it to
 * auto (page_pool_cfg_parse()).  Unless pages is set, the region's size is
 * computed when it is carved (rx_region_size_now()); pages reads back the
 * size the next carve uses, and debugfs page_pool_rx_region the carved
 * region, which never grows.  dev lists the network devices
 * (comma-separated, parent bus device, else interface name) whose page
 * pools take their pages from the region; pages, when set, replaces the
 * computed size; stack is what a bound pool keeps for reuse (0: the pool's
 * size).  read_pages
 * splits a read partition off; it changes only while no region exists,
 * since receive budgets are checked against the write partition it leaves,
 * and a split smaller than PAGE_POOL_RX_REGION_READ_MIN could never hold
 * one maximum READ and is refused.
 */
static char rx_region_dev[RX_REGION_DEV_LEN];
static unsigned int rx_region_pages;
static unsigned int rx_region_stack;
static unsigned int rx_region_read_pages;
/*
 * What the region is carved to hold, under rx_region_mutex: the pages the
 * registered receive-loan consumers ask for (page_pool_rx_region_demand()),
 * and the rings of the listed devices' pools that wait for it.
 */
static unsigned long rx_region_demand;
static unsigned long rx_region_nic;
static DEFINE_XARRAY(rx_region_waiting);
/* Of memory, the most the region takes: 1/RX_REGION_RAM_DIV. */
#define RX_REGION_RAM_DIV	64

/*
 * Each CPU caches a few of a partition's free pages, so taking and
 * returning pages rarely touches the shared free lists and their locks.  A
 * cache's own lock is uncontended except when a taker short of pages drains
 * every cache into the shards, so no free page is ever out of reach.  Lock
 * order: a CPU's cache, then one shard at a time.
 */
#define RX_REGION_PCP_MAX	64
#define RX_REGION_PCP_BATCH	32

struct rx_region_pcp {
	spinlock_t lock;
	unsigned int count;
	struct page *pages[RX_REGION_PCP_MAX];
};

/*
 * Behind the CPU caches a partition's free list is split into shards, each
 * with its own lock, so bulk transfers (a 1 MiB READ's 257 pages) from many
 * CPUs do not meet on one lock.  Each shard owns a contiguous pfn range and
 * a page always goes back to its owner, which therefore has room unless the
 * page is given back twice; a taker starts at its CPU's home shard and moves
 * on while it is short.
 */
#define RX_REGION_SHARDS_MAX	16
#define RX_REGION_SHARD_MIN	1024	/* pages: smaller partitions shard less */

/*
 * Every device queue's pool parks region pages in its ring and its alloc
 * cache, so with many queues full-size rings and caches would hold most of
 * the region.  A pool bound to the region keeps a ring of 64 entries,
 * refills 16 pages at a time and caches at most twice that; recycling
 * beyond these bounds returns pages to the region.
 */
#define RX_REGION_POOL_RING	64
#define RX_REGION_POOL_REFILL	(PP_ALLOC_CACHE_REFILL < 16 ? \
				 PP_ALLOC_CACHE_REFILL : 16)
#define RX_REGION_POOL_CACHE	(2 * RX_REGION_POOL_REFILL)

struct rx_region_shard {
	spinlock_t lock;
	unsigned long nfree;
	unsigned long cap;
	unsigned long min_free;		/* low-water mark */
	struct page **free;
} ____cacheline_aligned_in_smp;

/*
 * One partition of the region: per-CPU caches, pfn-range shards and
 * statistics.  A partition of no pages (the read partition of an unsplit
 * region) has no shards and no caches.
 */
struct page_pool_rx_region_part {
	unsigned long first_pfn;
	unsigned long nr;
	unsigned int nshards;
	unsigned long shard_pages;	/* pfns per shard (the last may own fewer) */
	unsigned long drained;		/* jiffies of the last drain for a refill */
	atomic_long_t alloc_fail;
	atomic_long_t escaped;
	struct rx_region_pcp __percpu *pcp;
	struct rx_region_shard shard[RX_REGION_SHARDS_MAX];
};

enum { RX_REGION_WRITE, RX_REGION_READ, RX_REGION_PARTS };

struct page_pool_rx_region {
	struct page_pool_rx_region_part part[RX_REGION_PARTS];
	unsigned long base_pfn;
	unsigned long nr;
	unsigned int pools;		/* bound to this region; rx_region_mutex */
	struct dentry *debugfs;
	char dev[RX_REGION_DEV_LEN];
};

static struct page_pool_rx_region *rx_region;
static DEFINE_MUTEX(rx_region_mutex);
/*
 * A READ consumer bound the region (page_pool_rx_region_bind_reads()): it is
 * held from then on as a bound pool holds it, and never given back.  Written
 * under rx_region_mutex; read locklessly once set.
 */
static bool rx_region_reads_bound;

/* Pages free in @p's CPU caches; lockless. */
static unsigned long rx_region_cached(const struct page_pool_rx_region_part *p)
{
	unsigned long cached = 0;
	int cpu;

	if (!p->pcp)
		return 0;
	for_each_possible_cpu(cpu)
		cached += READ_ONCE(per_cpu_ptr(p->pcp, cpu)->count);
	return cached;
}

/* Pages free in @p's shards; lockless. */
static unsigned long rx_region_shards_free(const struct page_pool_rx_region_part *p)
{
	unsigned long nfree = 0;
	unsigned int i;

	for (i = 0; i < p->nshards; i++)
		nfree += READ_ONCE(p->shard[i].nfree);
	return nfree;
}

/* The sum of @p's shards' low-water marks; lockless. */
static unsigned long rx_region_min_free(const struct page_pool_rx_region_part *p)
{
	unsigned long min_free = 0;
	unsigned int i;

	for (i = 0; i < p->nshards; i++)
		min_free += READ_ONCE(p->shard[i].min_free);
	return min_free;
}

/*
 * free counts the shards and the CPU caches; min_free sums the shards'
 * low-water marks (pages a cache holds count as taken from its shard).
 */
static void rx_region_part_show(struct seq_file *m, const char *prefix,
				struct page_pool_rx_region_part *p)
{
	unsigned long cached = rx_region_cached(p);

	seq_printf(m, "%sfree %lu\n%smin_free %lu\n%salloc_fail %lu\n%sescaped %lu\n%scached %lu\n%sshards %u\n",
		   prefix, rx_region_shards_free(p) + cached,
		   prefix, rx_region_min_free(p),
		   prefix, atomic_long_read(&p->alloc_fail),
		   prefix, atomic_long_read(&p->escaped),
		   prefix, cached, prefix, p->nshards);
}

/*
 * On from when the region is made until it is given back: no page is a
 * region page outside that.  Counted, so that a test that binds a pool to a
 * region of its own can hold it on too.
 */
DEFINE_STATIC_KEY_FALSE(page_pool_rx_region_key);
EXPORT_SYMBOL_GPL(page_pool_rx_region_key);

static int rx_region_show(struct seq_file *m, void *v)
{
	struct page_pool_rx_region *r = m->private;

	seq_printf(m, "dev %s\nphys_base 0x%llx\npages %lu\nread_pages %lu\n",
		   r->dev, (u64)r->base_pfn << PAGE_SHIFT, r->nr,
		   r->part[RX_REGION_READ].nr);
	rx_region_part_show(m, "", &r->part[RX_REGION_WRITE]);
	if (r->part[RX_REGION_READ].nr)
		rx_region_part_show(m, "read_", &r->part[RX_REGION_READ]);
	return 0;
}

static int rx_region_open(struct inode *inode, struct file *file)
{
	return single_open(file, rx_region_show, inode->i_private);
}

/* Restart each shard's low-water mark at its free count. */
static void rx_region_reset_min_free(struct page_pool_rx_region *r)
{
	unsigned long flags;
	unsigned int i, j;

	for (i = 0; i < RX_REGION_PARTS; i++) {
		struct page_pool_rx_region_part *p = &r->part[i];

		for (j = 0; j < p->nshards; j++) {
			struct rx_region_shard *sh = &p->shard[j];

			spin_lock_irqsave(&sh->lock, flags);
			WRITE_ONCE(sh->min_free, sh->nfree);
			spin_unlock_irqrestore(&sh->lock, flags);
		}
	}
}

/* Any write restarts each partition's low-water marks at its free counts. */
static ssize_t rx_region_write(struct file *file, const char __user *buf,
			       size_t len, loff_t *ppos)
{
	rx_region_reset_min_free(file_inode(file)->i_private);
	return len;
}

static const struct file_operations rx_region_fops = {
	.owner = THIS_MODULE,
	.open = rx_region_open,
	.read = seq_read,
	.write = rx_region_write,
	.llseek = seq_lseek,
	.release = single_release,
};

/* @dev is one of the comma-separated names in rx_region_dev. */
static bool page_pool_rx_region_listed(const char *dev)
{
	char list[RX_REGION_DEV_LEN], *cur = list, *name;

	strscpy(list, rx_region_dev, sizeof(list));
	while ((name = strsep(&cur, ",")) != NULL) {
		name = strim(name);
		if (*name && sysfs_streq(name, dev))
			return true;
	}
	return false;
}

/*
 * The region's size for rings of @nic pages and consumers asking @demand,
 * on a host of @ram pages: their sum rounded up to a power of two, at least
 * one maximum READ, at most a power of two within 1/RX_REGION_RAM_DIV of
 * memory.  0 while nothing asks for any.
 */
static unsigned long rx_region_size_for(unsigned long nic,
					unsigned long demand,
					unsigned long ram)
{
	unsigned long nr = nic + demand;
	unsigned long cap = rounddown_pow_of_two(max(ram / RX_REGION_RAM_DIV,
						     1UL));

	/* No region at all rather than one too small for the read floor. */
	if (!nr || cap < PAGE_POOL_RX_REGION_READ_MIN)
		return 0;
	nr = max(nr, (unsigned long)PAGE_POOL_RX_REGION_READ_MIN);
	return min(roundup_pow_of_two(nr), cap);
}

/* The size a region carved now would have; lockless, may be stale. */
static unsigned long rx_region_size_now(void)
{
	unsigned int pages = READ_ONCE(rx_region_pages);

	if (pages)
		return pages;
	return rx_region_size_for(READ_ONCE(rx_region_nic),
				  READ_ONCE(rx_region_demand),
				  totalram_pages());
}

/* The ring a pool's pages recycle through: its share of the NIC term. */
static unsigned long rx_region_pool_ring(const struct page_pool *pool)
{
	return pool->p.pool_size ?: 1024;
}

static struct lock_class_key rx_region_shard_keys[RX_REGION_SHARDS_MAX];

/*
 * Lay @p out over the @nr pages at @first_pfn: up to RX_REGION_SHARDS_MAX
 * shards (at most one per CPU, each of at least RX_REGION_SHARD_MIN pages
 * unless there is only one), each owning a slice of @free, and @pcp (NULL,
 * and unused, when @nr is 0) as the CPU caches, all empty.
 */
static void rx_region_part_init(struct page_pool_rx_region_part *p,
				unsigned long first_pfn, unsigned long nr,
				struct page **free,
				struct rx_region_pcp __percpu *pcp)
{
	unsigned long i, cap;
	int cpu;

	p->first_pfn = first_pfn;
	p->nr = nr;
	atomic_long_set(&p->alloc_fail, 0);
	atomic_long_set(&p->escaped, 0);
	p->drained = jiffies - 1;
	if (!nr) {
		p->nshards = 0;
		p->pcp = NULL;
		return;
	}
	p->pcp = pcp;
	for_each_possible_cpu(cpu) {
		struct rx_region_pcp *c = per_cpu_ptr(pcp, cpu);

		spin_lock_init(&c->lock);
		c->count = 0;
	}
	p->nshards = clamp_t(unsigned long, nr / RX_REGION_SHARD_MIN, 1,
			     min_t(unsigned int, RX_REGION_SHARDS_MAX,
				   nr_cpu_ids));
	cap = DIV_ROUND_UP(nr, p->nshards);
	p->shard_pages = cap;
	for (i = 0; i < p->nshards; i++) {
		struct rx_region_shard *sh = &p->shard[i];

		spin_lock_init(&sh->lock);
		/* rx_region_pop_exact() may hold several, in index order */
		lockdep_set_class(&sh->lock, &rx_region_shard_keys[i]);
		sh->free = free + i * cap;
		sh->cap = min(cap, nr - min(nr, i * cap));
		sh->nfree = sh->cap;	/* filled below, in pfn order */
		sh->min_free = sh->cap;
	}
	for (i = 0; i < nr; i++) {
		struct page *page = pfn_to_page(first_pfn + i);

		/*
		 * page_pool ORs its signature into pp_magic, which overlays
		 * page->lru.next.  Pages from the page allocator carry list
		 * poison there; a contiguous allocation may leave a live list
		 * pointer, so start every region page as a released pool page.
		 */
		page->pp_magic = 0;
		free[i] = page;
	}
}

/* The read split requested for a region of @nr pages; 0 if it leaves none. */
static unsigned long rx_region_read_split(unsigned long nr)
{
	unsigned long nr_read = READ_ONCE(rx_region_read_pages);

	return nr_read < nr ? nr_read : 0;
}

/*
 * The region once it exists, without rx_region_mutex: pairs with the
 * smp_store_release() in rx_region_publish(), so its partitions are seen
 * initialized.  A region no pool or READ consumer holds may be given back
 * (page_pool_rx_region_reap()), so a caller that holds neither reads it
 * under rcu_read_lock().
 */
static struct page_pool_rx_region *rx_region_published(void)
{
	/* pairs with smp_store_release() in rx_region_publish() */
	return smp_load_acquire(&rx_region);
}

/* An earlier allocation failed: a device-less consumer does not retry. */
static bool rx_region_failed;

/*
 * Lay the region @r out over the @nr pages at @base_pfn, @nr_read of them
 * split off for reads, with @pcp[RX_REGION_WRITE] and @pcp[RX_REGION_READ]
 * as the partitions' CPU caches (a read partition of no pages does not use
 * its own, which may be NULL), and publish it; under rx_region_mutex.
 */
static void rx_region_publish(struct page_pool_rx_region *r,
			      struct page **free,
			      struct rx_region_pcp __percpu *pcp[RX_REGION_PARTS],
			      unsigned long base_pfn, unsigned long nr,
			      unsigned long nr_read, const char *who)
{
	lockdep_assert_held(&rx_region_mutex);
	r->base_pfn = base_pfn;
	r->nr = nr;
	rx_region_part_init(&r->part[RX_REGION_WRITE], base_pfn, nr - nr_read,
			    free, pcp[RX_REGION_WRITE]);
	rx_region_part_init(&r->part[RX_REGION_READ], base_pfn + nr - nr_read,
			    nr_read, free + nr - nr_read, pcp[RX_REGION_READ]);
	strscpy(r->dev, rx_region_dev, RX_REGION_DEV_LEN);
	strim(r->dev);
	/* On before the region is seen; off once it is given back. */
	static_branch_inc(&page_pool_rx_region_key);
	/* Pairs with rx_region_published(). */
	smp_store_release(&rx_region, r);
	r->debugfs = debugfs_create_file("page_pool_rx_region", 0600, NULL, r,
					 &rx_region_fops);
	pr_info("page_pool: fixed rx region for %s: %lu pages (%lu read) at 0x%llx\n",
		who, nr, nr_read, (u64)base_pfn << PAGE_SHIFT);
}

/*
 * The partitions' CPU caches: always the write partition's, the read
 * partition's only when @nr_read splits one off.  False (none left
 * allocated) on failure.
 */
static bool rx_region_pcp_alloc(struct rx_region_pcp __percpu *pcp[RX_REGION_PARTS],
				unsigned long nr_read)
{
	pcp[RX_REGION_WRITE] = alloc_percpu(struct rx_region_pcp);
	pcp[RX_REGION_READ] = nr_read ? alloc_percpu(struct rx_region_pcp) :
					NULL;
	if (pcp[RX_REGION_WRITE] && (!nr_read || pcp[RX_REGION_READ]))
		return true;
	free_percpu(pcp[RX_REGION_WRITE]);
	free_percpu(pcp[RX_REGION_READ]);
	pcp[RX_REGION_WRITE] = NULL;
	pcp[RX_REGION_READ] = NULL;
	return false;
}

/* Allocate the region of @nr pages on @nid; under rx_region_mutex. */
static struct page_pool_rx_region *rx_region_create(unsigned long nr, int nid,
						    const char *who)
{
	struct rx_region_pcp __percpu *pcp[RX_REGION_PARTS] = {};
	unsigned long nr_read = rx_region_read_split(nr);
	struct page_pool_rx_region *r;
	struct page *base = NULL;
	struct page **free;

	lockdep_assert_held(&rx_region_mutex);
	if (READ_ONCE(rx_region_read_pages) && !nr_read)
		pr_warn("page_pool: rx_region_read_pages %u leaves no write partition of %lu pages; not split\n",
			READ_ONCE(rx_region_read_pages), nr);
	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r)
		goto fail;
	free = kvmalloc_array(nr, sizeof(*free), GFP_KERNEL);
	if (free && rx_region_pcp_alloc(pcp, nr_read)) {
#ifdef CONFIG_CONTIG_ALLOC
		base = alloc_contig_pages(nr, GFP_KERNEL, nid, NULL);
#endif
	}
	if (!base) {
		pr_warn("page_pool: no %lu-page fixed rx region for %s\n",
			nr, who);
		free_percpu(pcp[RX_REGION_WRITE]);
		free_percpu(pcp[RX_REGION_READ]);
		kvfree(free);
		kfree(r);
		goto fail;
	}
	rx_region_publish(r, free, pcp, page_to_pfn(base), nr, nr_read, who);
	return r;
fail:
	/* Pairs with READ_ONCE() in page_pool_rx_region_bind_reads(). */
	WRITE_ONCE(rx_region_failed, true);
	return NULL;
}

/*
 * Give the region back if nothing is bound to it and it is not the one asked
 * for any more.  Only the pages on the free lists are ours to free: an
 * escaped page went to the page allocator with its last reference.  Lockless
 * readers (page_pool_rx_region_size(), _read_pages()) hold rcu_read_lock(),
 * so the region is unpublished and a grace period passes before it is freed.
 */
static void page_pool_rx_region_reap(void)
{
	struct page_pool_rx_region *r = rx_region;
	unsigned long i, held = 0, escaped = 0;
	unsigned int s;
	int k, cpu;

	lockdep_assert_held(&rx_region_mutex);
	if (!r || r->pools || rx_region_reads_bound)
		return;
	if (rx_region_demand ||
	    (rx_region_dev[0] && rx_region_size_now() == r->nr))
		return;
	for (k = 0; k < RX_REGION_PARTS; k++) {
		escaped += atomic_long_read(&r->part[k].escaped);
		held += rx_region_shards_free(&r->part[k]) +
			rx_region_cached(&r->part[k]);
	}
	held += escaped;
	/* No pool, so no page can be out: keep the memory if that is wrong. */
	if (WARN_ON_ONCE(held != r->nr))
		return;
	WRITE_ONCE(rx_region, NULL);
	debugfs_remove(r->debugfs);
	synchronize_rcu();
	/* Free pages are in the shards and in the CPU caches. */
	for (k = 0; k < RX_REGION_PARTS; k++) {
		struct page_pool_rx_region_part *p = &r->part[k];

		for (s = 0; s < p->nshards; s++)
			for (i = 0; i < p->shard[s].nfree; i++)
				__free_page(p->shard[s].free[i]);
		if (!p->pcp)
			continue;
		for_each_possible_cpu(cpu) {
			struct rx_region_pcp *c = per_cpu_ptr(p->pcp, cpu);

			for (i = 0; i < c->count; i++)
				__free_page(c->pages[i]);
		}
	}
	pr_info("page_pool: fixed rx region for %s freed: %lu pages at 0x%llx, %lu escaped\n",
		r->dev, r->nr, (u64)r->base_pfn << PAGE_SHIFT, escaped);
	free_percpu(r->part[RX_REGION_WRITE].pcp);
	free_percpu(r->part[RX_REGION_READ].pcp);
	kvfree(r->part[RX_REGION_WRITE].shard[0].free);
	kfree(r);
	static_branch_dec(&page_pool_rx_region_key);
}

/* @pool is being destroyed: every page it took is back or has escaped. */
static void page_pool_rx_region_unbind(struct page_pool *pool)
{
	/*
	 * A carve attaches a waiting pool before it erases it, both under
	 * the mutex: with none left waiting, the attach is seen.
	 */
	if (xa_empty(&rx_region_waiting)) {
		smp_rmb();
		if (!READ_ONCE(pool->rx_region))
			return;
	}
	mutex_lock(&rx_region_mutex);
	if (xa_erase(&rx_region_waiting, (unsigned long)pool))
		WRITE_ONCE(rx_region_nic,
			   rx_region_nic - rx_region_pool_ring(pool));
	if (pool->rx_region && pool->rx_region == rx_region &&
	    !WARN_ON_ONCE(!rx_region->pools))
		rx_region->pools--;
	pool->rx_region = NULL;
	page_pool_rx_region_reap();
	mutex_unlock(&rx_region_mutex);
}

static struct page_pool_rx_region *
page_pool_rx_region_get(const struct page_pool *pool)
{
	const struct net_device *netdev = pool->slow.netdev;
	struct page_pool_rx_region *r;
	const char *dev;

	if (pool->p.order || !rx_region_dev[0])
		return NULL;
	if (netdev)
		dev = netdev->dev.parent ? dev_name(netdev->dev.parent) :
					   netdev->name;
	else if (pool->p.dev)
		dev = dev_name(pool->p.dev);
	else
		return NULL;
	if (!page_pool_rx_region_listed(dev))
		return NULL;
	mutex_lock(&rx_region_mutex);
	/* A region left over from another size goes before a new one is made. */
	page_pool_rx_region_reap();
	/*
	 * Every listed device shares the one region's write partition.  Its
	 * size is set (an override) or asked for (a registered consumer):
	 * carve it now.  Else the pool waits, its ring counted in the size,
	 * and the first consumer's carve attaches it.
	 */
	r = rx_region;
	if (!r && (READ_ONCE(rx_region_pages) || rx_region_demand) &&
	    !rx_region_failed)
		r = rx_region_create(rx_region_size_now() ?: 1,
				     pool->p.nid == NUMA_NO_NODE ?
				     numa_mem_id() : pool->p.nid, dev);
	if (r)
		r->pools++;
	else if (!xa_insert(&rx_region_waiting, (unsigned long)pool,
			    (void *)pool, GFP_KERNEL))
		WRITE_ONCE(rx_region_nic,
			   rx_region_nic + rx_region_pool_ring(pool));
	mutex_unlock(&rx_region_mutex);
	return r;
}

/*
 * Carve the region at its computed size and attach the listed devices'
 * pools that wait for it; under rx_region_mutex.  Attaching is the one
 * pointer store: the pool's next slow-path allocation (NAPI) takes region
 * pages, and its return path gives back by pfn, so the pages it holds from
 * before go to the page allocator.  An attached pool keeps its ring in
 * place of the region's stack: the stack is the pool's recycling state and
 * changes only while nothing recycles into it.
 */
static struct page_pool_rx_region *rx_region_carve_locked(int nid,
							  const char *who)
{
	unsigned long nr = rx_region_size_now();
	struct page_pool_rx_region *r;
	struct page_pool *pool;
	unsigned long idx;

	lockdep_assert_held(&rx_region_mutex);
	if (rx_region || rx_region_failed || !nr)
		return rx_region;
	r = rx_region_create(nr, nid, who);
	if (!r)
		return NULL;
	xa_for_each(&rx_region_waiting, idx, pool) {
		r->pools++;
		/* Pairs with READ_ONCE() in the allocation and return paths. */
		WRITE_ONCE(pool->rx_region, r);
		xa_erase(&rx_region_waiting, idx);
		WRITE_ONCE(rx_region_nic,
			   rx_region_nic - rx_region_pool_ring(pool));
	}
	return r;
}

/**
 * page_pool_rx_region_demand - a receive-loan consumer's floor comes or goes
 * @delta: pages the consumer asks the region to hold (negative: gives up)
 *
 * Demand only counts toward the size: the region is carved later, by
 * page_pool_rx_region_carve(), _bind_reads() or a listed device's pool,
 * so that a floor set after registration is the one it is carved for.  It
 * keeps that size until no consumer, pool or READ holds it.  A region that
 * exists is not resized: a late consumer shares what is there.
 *
 * Context: process context; may sleep.
 */
void page_pool_rx_region_demand(long delta)
{
	might_sleep();
	mutex_lock(&rx_region_mutex);
	WRITE_ONCE(rx_region_demand, rx_region_demand + delta);
	if (delta < 0)
		page_pool_rx_region_reap();
	mutex_unlock(&rx_region_mutex);
}
EXPORT_SYMBOL_GPL(page_pool_rx_region_demand);

/**
 * page_pool_rx_region_carve - carve the region for the pools that wait
 *
 * When a consumer asks for region pages and a listed device's pool waits
 * for the region, carve it at its computed size and attach the pools.
 * Without a waiting pool nothing could take region pages for a receive, so
 * nothing is carved (a READ carves through _bind_reads()).
 *
 * Context: process context; may sleep.
 */
void page_pool_rx_region_carve(void)
{
	if (!READ_ONCE(rx_region_demand) || !READ_ONCE(rx_region_nic) ||
	    READ_ONCE(rx_region_failed))
		return;
	might_sleep();
	mutex_lock(&rx_region_mutex);
	if (rx_region_demand && !xa_empty(&rx_region_waiting))
		rx_region_carve_locked(numa_mem_id(), "consumers");
	mutex_unlock(&rx_region_mutex);
}
EXPORT_SYMBOL_GPL(page_pool_rx_region_carve);

/*
 * Bind @pool to region @r (if any).  A bound pool recycles remotely released
 * pages through a stack in place of its ring: rx_region_stack entries when
 * set, else at most RX_REGION_POOL_RING.  Without the stack it keeps the
 * ring, shortened to RX_REGION_POOL_RING.
 */
static void page_pool_rx_region_bind(struct page_pool *pool,
				     struct page_pool_rx_region *r,
				     unsigned int ring_qsize)
{
	/*
	 * Without the stack the pool recycles through the ring.  A page that
	 * finds the stack full goes back to the region, to be mapped again
	 * when the pool next takes it, so rx_region_stack can make the stack
	 * larger than the pool's ring.
	 */
	unsigned int size = READ_ONCE(rx_region_stack) ?:
			    min_t(unsigned int, ring_qsize, RX_REGION_POOL_RING);

	pool->rx_region = r;
	if (!r)
		return;
	pool->rx_stack = kvcalloc(size, sizeof(*pool->rx_stack), GFP_KERNEL);
	if (pool->rx_stack) {
		pool->rx_stack_size = size;
		return;
	}
	if (pool->ring.size > RX_REGION_POOL_RING &&
	    ptr_ring_resize(&pool->ring, RX_REGION_POOL_RING, GFP_KERNEL,
			    NULL))
		pr_warn_once("page_pool: rx region pool keeps its full ring\n");
}

/**
 * page_pool_rx_region_bind_reads - make sure the region exists for reads
 *
 * A READ consumer has no listed device, so it creates the region itself,
 * when rx_region_pages is set and no earlier allocation failed (never
 * retried).  Devices listed in rx_region_dev later bind to it.  The region
 * is then placed on the calling thread's memory node (numa_mem_id()); a
 * device on another node that binds later reaches it across nodes, so a
 * READ consumer on a multi-node host should bind from a thread on the
 * devices' node, or list the devices so that they create it first.
 *
 * Once bound for reads the region is held for good, as a bound pool holds
 * it: it is never given back, so a change to rx_region_pages no longer
 * takes effect, and take_pages(), give_pages() and span() may use it
 * without a lock.
 *
 * Context: process context; may sleep (the first call allocates the region,
 * which may migrate pages).  Once the region is bound, or once an
 * allocation has failed, it returns without a lock.
 *
 * Return: true when a region exists.
 */
bool page_pool_rx_region_bind_reads(void)
{
	unsigned long nr = rx_region_size_now();
	bool bound;

	/* Pairs with smp_store_release() below. */
	if (smp_load_acquire(&rx_region_reads_bound))
		return true;
	if (!nr || READ_ONCE(rx_region_failed))
		return false;
	might_sleep();
	mutex_lock(&rx_region_mutex);
	rx_region_carve_locked(numa_mem_id(), "reads");
	bound = rx_region;
	if (bound)
		smp_store_release(&rx_region_reads_bound, true);
	mutex_unlock(&rx_region_mutex);
	return bound;
}
EXPORT_SYMBOL_GPL(page_pool_rx_region_bind_reads);

/**
 * page_pool_rx_region_size - pages in the fixed receive region
 *
 * Lockless, so a credit pool may read it under its own spinlock.
 *
 * Return: the region's size once it exists, else the size configured for it
 * (rx_region_pages); 0 when there is none.  Both partitions are inside it;
 * budgets for receive loans are checked against the write partition, the
 * size less page_pool_rx_region_read_pages().
 */
unsigned long page_pool_rx_region_size(void)
{
	struct page_pool_rx_region *r;
	unsigned long nr;

	rcu_read_lock();
	r = rx_region_published();
	nr = r ? r->nr : rx_region_size_now();
	rcu_read_unlock();
	return nr;
}
EXPORT_SYMBOL_GPL(page_pool_rx_region_size);

/**
 * page_pool_rx_region_read_pages - pages in the region's read partition
 *
 * Lockless, so a credit pool may read it under its own spinlock.
 *
 * Return: the read partition's size once the region exists, else the split
 * configured for it (rx_region_read_pages, when it leaves a write
 * partition); 0 when reads and writes share the region.  When not 0 it is
 * at least PAGE_POOL_RX_REGION_READ_MIN.
 */
unsigned long page_pool_rx_region_read_pages(void)
{
	struct page_pool_rx_region *r;
	unsigned long nr;

	rcu_read_lock();
	r = rx_region_published();
	nr = r ? r->part[RX_REGION_READ].nr :
		 rx_region_read_split(READ_ONCE(rx_region_pages));
	rcu_read_unlock();
	return nr;
}
EXPORT_SYMBOL_GPL(page_pool_rx_region_read_pages);

/* The shard this CPU takes from first. */
static unsigned int rx_region_home(const struct page_pool_rx_region_part *p)
{
	return (raw_smp_processor_id() * p->nshards) / nr_cpu_ids;
}

/*
 * Push @n pages of @pages, each onto the shard that owns its pfn: in array
 * order, so the last ends on top, or with @reverse last to first (undoing a
 * pop).  Pages from one shard in a row share one lock hold.  A page whose
 * owner is already full was given back twice: it warns and is counted
 * escaped, and is neither stored nor put (P1's double-give rule).
 */
static void rx_region_push(struct page_pool_rx_region_part *p,
			   struct page **pages, unsigned int n, bool reverse)
{
	struct rx_region_shard *held = NULL;
	unsigned long flags = 0;
	unsigned int k;

	for (k = 0; k < n; k++) {
		struct page *page = pages[reverse ? n - 1 - k : k];
		struct rx_region_shard *sh =
			&p->shard[(page_to_pfn(page) - p->first_pfn) /
				  p->shard_pages];

		if (sh != held) {
			if (held)
				spin_unlock_irqrestore(&held->lock, flags);
			spin_lock_irqsave(&sh->lock, flags);
			held = sh;
		}
		if (WARN_ON_ONCE(sh->nfree >= sh->cap)) {
			atomic_long_inc(&p->escaped);
			continue;
		}
		sh->free[sh->nfree] = page;
		WRITE_ONCE(sh->nfree, sh->nfree + 1);
	}
	if (held)
		spin_unlock_irqrestore(&held->lock, flags);
}

/* This CPU's home shard is down to an eighth of its pages. */
static bool rx_region_home_low(const struct page_pool_rx_region_part *p)
{
	const struct rx_region_shard *sh = &p->shard[rx_region_home(p)];

	return READ_ONCE(sh->nfree) < sh->cap / 8;
}

/* Plenty free in the shards: worth refilling a CPU cache from them. */
static bool rx_region_plenty(const struct page_pool_rx_region_part *p)
{
	return rx_region_shards_free(p) > p->nr / 8;
}

/*
 * Pop pages into @dst[@at..@n), home shard first, then round robin; return
 * the new @at.  Each shard's low-water mark follows its pops, except that an
 * all-or-nothing take (@exact) marks the shards it empties only once it has
 * all it wants, so a take that fails and is undone leaves no false low.
 */
static unsigned int rx_region_pop(struct page_pool_rx_region_part *p,
				  struct page **dst, unsigned int at,
				  unsigned int n, bool exact)
{
	unsigned int s = rx_region_home(p), tries;
	unsigned long flags, emptied = 0;

	for (tries = 0; at < n && tries < p->nshards; tries++) {
		struct rx_region_shard *sh = &p->shard[s];

		if (READ_ONCE(sh->nfree)) {
			unsigned long nfree;

			spin_lock_irqsave(&sh->lock, flags);
			nfree = sh->nfree;
			while (at < n && nfree)
				dst[at++] = sh->free[--nfree];
			WRITE_ONCE(sh->nfree, nfree);
			if (exact && at < n)
				__set_bit(s, &emptied);
			else if (nfree < sh->min_free)
				WRITE_ONCE(sh->min_free, nfree);
			spin_unlock_irqrestore(&sh->lock, flags);
		}
		s = s + 1 == p->nshards ? 0 : s + 1;
	}
	if (at == n)
		for_each_set_bit(s, &emptied, RX_REGION_SHARDS_MAX)
			WRITE_ONCE(p->shard[s].min_free, 0);
	return at;
}

/*
 * Return a partition's page to this CPU's cache, spilling a batch to the
 * shards when the cache is full, or everything when the home shard is low;
 * false when it is not ours or another holder has it (it then escapes the
 * region, counted here, and the caller puts it).
 */
static bool page_pool_rx_region_put(struct page_pool_rx_region_part *p,
				    struct page *page)
{
	unsigned long pfn = page_to_pfn(page), flags;
	struct rx_region_pcp *c;

	if (pfn - p->first_pfn >= p->nr)
		return false;
	if (page_ref_count(page) != 1) {
		atomic_long_inc(&p->escaped);
		return false;
	}
	migrate_disable();
	c = this_cpu_ptr(p->pcp);
	spin_lock_irqsave(&c->lock, flags);
	if (c->count == RX_REGION_PCP_MAX) {
		unsigned int keep = rx_region_home_low(p) ? 0 :
				    RX_REGION_PCP_MAX - RX_REGION_PCP_BATCH;

		rx_region_push(p, c->pages + keep, c->count - keep, false);
		WRITE_ONCE(c->count, keep);
	}
	c->pages[c->count] = page;
	WRITE_ONCE(c->count, c->count + 1);
	spin_unlock_irqrestore(&c->lock, flags);
	migrate_enable();
	return true;
}

/*
 * Move every CPU's cached pages to their shards: a taker is short.  A cache
 * found empty is skipped unlocked; a page put there meanwhile is a page
 * freed after the shortage began.
 */
static void rx_region_drain(struct page_pool_rx_region_part *p)
{
	unsigned long flags;
	int cpu;

	for_each_possible_cpu(cpu) {
		struct rx_region_pcp *c = per_cpu_ptr(p->pcp, cpu);

		if (!READ_ONCE(c->count))
			continue;
		spin_lock_irqsave(&c->lock, flags);
		rx_region_push(p, c->pages, c->count, false);
		WRITE_ONCE(c->count, 0);
		spin_unlock_irqrestore(&c->lock, flags);
	}
}

/* Pages @p can still lend: its size less its escaped pages. */
static unsigned long rx_region_part_capacity(struct page_pool_rx_region_part *p)
{
	unsigned long escaped = atomic_long_read(&p->escaped);

	return escaped < p->nr ? p->nr - escaped : 0;
}

/* Pop up to @n - @at of @sh's free pages into @dst[@at..), @sh->lock held. */
static unsigned int rx_region_pop_locked(struct rx_region_shard *sh,
					 struct page **dst, unsigned int at,
					 unsigned int n)
{
	unsigned long nfree = sh->nfree;

	while (at < n && nfree)
		dst[at++] = sh->free[--nfree];
	WRITE_ONCE(sh->nfree, nfree);
	if (nfree < sh->min_free)
		WRITE_ONCE(sh->min_free, nfree);
	return at;
}

/*
 * All @n pages, more than a batch, straight from the shards into @dst, or
 * none with @dst untouched: from this CPU's home shard alone when it holds
 * @n, else from the shards in index order once those locked hold @n
 * between them.  Each shard lock has its own lockdep class, so holding
 * several, always in index order, is not reported as recursion.
 */
static bool rx_region_pop_exact(struct page_pool_rx_region_part *p,
				struct page **dst, unsigned int n)
{
	struct rx_region_shard *sh = &p->shard[rx_region_home(p)];
	unsigned int locked, k, at = 0;
	unsigned long flags, have = 0;

	if (READ_ONCE(sh->nfree) >= n) {
		spin_lock_irqsave(&sh->lock, flags);
		if (sh->nfree >= n) {
			rx_region_pop_locked(sh, dst, 0, n);
			spin_unlock_irqrestore(&sh->lock, flags);
			return true;
		}
		spin_unlock_irqrestore(&sh->lock, flags);
	}
	local_irq_save(flags);
	for (locked = 0; locked < p->nshards && have < n; locked++) {
		spin_lock(&p->shard[locked].lock);
		have += p->shard[locked].nfree;
	}
	if (have >= n)
		for (k = 0; k < locked; k++)
			at = rx_region_pop_locked(&p->shard[k], dst, at, n);
	for (k = locked; k-- > 0;)
		spin_unlock(&p->shard[k].lock);
	local_irq_restore(flags);
	return have >= n;
}

/*
 * One attempt at @n pages: a batch or less from this CPU's cache, then the
 * shards (refilling the cache by a batch while the shards have plenty),
 * more than a batch from the shards alone.  @exact takes all or none and
 * writes @dst only with what it takes: a batch or less is staged on the
 * stack, more is popped only when the shards hold it.
 */
static unsigned int rx_region_get_once(struct page_pool_rx_region_part *p,
				       struct page **dst, unsigned int n,
				       bool exact)
{
	struct page *stage[RX_REGION_PCP_BATCH], **out = dst;
	struct rx_region_pcp *c;
	unsigned long flags;
	unsigned int i = 0;

	if (n > RX_REGION_PCP_BATCH) {
		if (exact)
			return rx_region_pop_exact(p, dst, n) ? n : 0;
		i = rx_region_pop(p, dst, 0, n, false);
	} else {
		if (exact)
			out = stage;
		migrate_disable();
		c = this_cpu_ptr(p->pcp);
		spin_lock_irqsave(&c->lock, flags);
		while (i < n && c->count) {
			out[i++] = c->pages[c->count - 1];
			WRITE_ONCE(c->count, c->count - 1);
		}
		if (i < n) {
			i = rx_region_pop(p, out, i, n, exact);
			/*
			 * Not when pages run low: cached pages are free yet
			 * out of other takers' reach until a drain.
			 */
			if (i == n && rx_region_plenty(p))
				WRITE_ONCE(c->count,
					   rx_region_pop(p, c->pages, 0,
							 RX_REGION_PCP_BATCH,
							 false));
		}
		spin_unlock_irqrestore(&c->lock, flags);
		migrate_enable();
		if (exact && i < n) {
			rx_region_push(p, stage, i, true);
			return 0;
		}
		if (exact)
			memcpy(dst, stage, n * sizeof(*dst));
	}
	return i;
}

/*
 * Take up to @n pages into @dst: @exact, all or none; else as many as are
 * free.  A taker that comes up short drains every CPU's cache into the
 * shards and tries once more, so no free page is out of its reach; a
 * device refill (not @exact) that finds nothing drains at most once a
 * jiffy, or at exhaustion every poll would lock every CPU's cache.
 */
static unsigned int rx_region_get(struct page_pool_rx_region_part *p,
				  struct page **dst, unsigned int n,
				  bool exact)
{
	unsigned long now;
	unsigned int got;

	if (exact && n > rx_region_part_capacity(p))
		return 0;
	got = rx_region_get_once(p, dst, n, exact);
	if (got == n || (got && !exact))
		return got;
	if (!exact) {
		now = jiffies;
		if (READ_ONCE(p->drained) == now)
			return got;
		WRITE_ONCE(p->drained, now);
	}
	rx_region_drain(p);
	return rx_region_get_once(p, dst, n, exact);
}

/*
 * Return @n pages of @p: fewer than a batch through this CPU's cache, a
 * batch or more straight to their shards.  A page another holder still
 * references escapes: counted and put.  @pages is not modified.
 */
static void rx_region_give(struct page_pool_rx_region_part *p,
			   struct page **pages, unsigned int n)
{
	unsigned int i, start = 0;

	if (n < RX_REGION_PCP_BATCH) {
		for (i = 0; i < n; i++)
			if (!page_pool_rx_region_put(p, pages[i]))
				put_page(pages[i]);
		return;
	}
	for (i = 0; i < n; i++) {
		if (likely(page_ref_count(pages[i]) == 1))
			continue;
		rx_region_push(p, pages + start, i - start, false);
		atomic_long_inc(&p->escaped);
		put_page(pages[i]);
		start = i + 1;
	}
	rx_region_push(p, pages + start, n - start, false);
}

/* The partition a consumer outside the pools draws on. */
static struct page_pool_rx_region_part *
rx_region_part(struct page_pool_rx_region *r, bool read)
{
	if (read && r->part[RX_REGION_READ].nr)
		return &r->part[RX_REGION_READ];
	return &r->part[RX_REGION_WRITE];
}

/**
 * page_pool_rx_region_take_pages - take region pages outside the pools
 * @read: from the read partition when the region is split
 * @pages: filled with the pages taken
 * @n: pages wanted
 *
 * All or nothing: on failure @pages is untouched, so a caller may take
 * into an array that still describes something else.  Up to
 * RX_REGION_PCP_BATCH (32) pages come from this CPU's cache, most recently
 * freed first, then from the shards; more come from the shards alone: this
 * CPU's home shard when it holds them all, else the shards in index order.
 * A take short of pages drains every CPU's cache and tries once more
 * before it fails.
 * Each page has refcount 1 and pp_magic 0.  Too few free pages is not an
 * allocation failure here, so alloc_fail is not counted.  What a failed take means depends on @n:
 * within page_pool_rx_region_capacity(), the pages are lent out and the
 * caller may wait for earlier takes to be given back; beyond it (a request
 * larger than the partition, or one that escaped pages have put out of
 * reach) it can never succeed, and the caller serves the request from
 * ordinary pages instead of waiting.  A split read partition is created
 * holding one maximum READ (PAGE_POOL_RX_REGION_READ_MIN); only escaped
 * pages can take that away.
 *
 * The caller has bound the region (page_pool_rx_region_bind_reads()).
 *
 * Return: @n, or 0 when there is no region or too few free pages.
 */
unsigned int page_pool_rx_region_take_pages(bool read, struct page **pages,
					    unsigned int n)
{
	struct page_pool_rx_region *r = rx_region_published();

	if (!r || !n)
		return 0;
	return rx_region_get(rx_region_part(r, read), pages, n, true);
}
EXPORT_SYMBOL_GPL(page_pool_rx_region_take_pages);

/**
 * page_pool_rx_region_give_pages - return pages taken by take_pages
 * @pages: the pages
 * @n: how many
 *
 * Each page goes back to the partition its address lies in, whichever
 * partition it was taken from, and there to the shard that owns it: a run
 * of RX_REGION_PCP_BATCH (32) or more pages of one partition directly, a
 * shorter run through this CPU's cache.  Give a page back only once every other
 * reference is gone: a page sent with MSG_SPLICE_PAGES stays referenced by
 * its skbs until the data is acknowledged, so a sender gives it back on TX
 * completion, not when the send call returns.  A page another holder still
 * references leaves the region for good: it is counted escaped (per
 * partition, in debugfs), this reference is put, and the holder's final put
 * frees it to the page allocator.  Escape is the fault path: the partition
 * is one page smaller from then on, page_pool_rx_region_capacity() says so,
 * and a take beyond it fails for good, so the caller serves that request
 * from ordinary pages (see page_pool_rx_region_take_pages()).  A page given
 * back twice is found when it reaches its owner shard already full (at once
 * in a bulk run, else when its CPU cache spills or is drained): it warns and
 * is counted escaped without being put again.  A page from outside the region warns
 * and is put.  Process or BH context.
 */
void page_pool_rx_region_give_pages(struct page **pages, unsigned int n)
{
	struct page_pool_rx_region *r = rx_region_published();
	struct page_pool_rx_region_part *p = NULL, *q;
	unsigned int i, start = 0;

	/* Each run of pages from one partition goes back in one call. */
	for (i = 0; i < n; i++) {
		unsigned long off;

		off = r ? page_to_pfn(pages[i]) - r->base_pfn : ULONG_MAX;
		if (WARN_ON_ONCE(!r || off >= r->nr)) {
			if (p)
				rx_region_give(p, pages + start, i - start);
			p = NULL;
			put_page(pages[i]);
			continue;
		}
		q = &r->part[off < r->part[RX_REGION_WRITE].nr ?
			     RX_REGION_WRITE : RX_REGION_READ];
		if (q != p) {
			if (p)
				rx_region_give(p, pages + start, i - start);
			p = q;
			start = i;
		}
	}
	if (p)
		rx_region_give(p, pages + start, n - start);
}
EXPORT_SYMBOL_GPL(page_pool_rx_region_give_pages);

/**
 * page_pool_rx_region_span - where a consumer's partition lies
 * @read: as for page_pool_rx_region_take_pages()
 * @base_pfn: set to the partition's first pfn when there is a region
 *
 * Return: the partition's size in pages, escaped ones included, so a
 * consumer may index per-page state by pfn - @base_pfn; 0 (@base_pfn
 * untouched) when there is no region.  page_pool_rx_region_capacity() is
 * how many of them can still be taken.
 */
unsigned long page_pool_rx_region_span(bool read, unsigned long *base_pfn)
{
	struct page_pool_rx_region *r = rx_region_published();
	struct page_pool_rx_region_part *p;

	if (!r)
		return 0;
	p = rx_region_part(r, read);
	*base_pfn = p->first_pfn;
	return p->nr;
}
EXPORT_SYMBOL_GPL(page_pool_rx_region_span);

/**
 * page_pool_rx_region_capacity - pages a consumer's partition can still lend
 * @read: as for page_pool_rx_region_take_pages()
 *
 * The partition's size less its escaped pages: the largest take that can
 * succeed once every earlier take is given back.  Lockless, so it may lag
 * an escape in progress; a caller compares a request against it before
 * deciding to wait for pages rather than serve the request unconfined.
 *
 * Return: the usable size in pages; 0 when there is no region.
 */
unsigned long page_pool_rx_region_capacity(bool read)
{
	struct page_pool_rx_region *r = rx_region_published();

	if (!r)
		return 0;
	return rx_region_part_capacity(rx_region_part(r, read));
}
EXPORT_SYMBOL_GPL(page_pool_rx_region_capacity);

#if IS_ENABLED(CONFIG_KUNIT)
static int rx_region_read_pages_dbg_set(void *data, u64 val);

/* Set rx_region_read_pages as its debugfs file read_pages does. */
int page_pool_rx_region_kunit_read_param(u64 val)
{
	return rx_region_read_pages_dbg_set(NULL, val);
}
EXPORT_SYMBOL_IF_KUNIT(page_pool_rx_region_kunit_read_param);

/* The size formula, on inputs the test chooses. */
unsigned long page_pool_rx_region_kunit_size_for(unsigned long nic,
						 unsigned long demand,
						 unsigned long ram)
{
	return rx_region_size_for(nic, demand, ram);
}
EXPORT_SYMBOL_IF_KUNIT(page_pool_rx_region_kunit_size_for);

/* The size a carve now would compute, on a host of @ram pages. */
unsigned long page_pool_rx_region_kunit_carve_size(unsigned long ram)
{
	return rx_region_size_for(READ_ONCE(rx_region_nic),
				  READ_ONCE(rx_region_demand), ram);
}
EXPORT_SYMBOL_IF_KUNIT(page_pool_rx_region_kunit_carve_size);

/* The overrides and the failure flag, for a test to restore later. */
void page_pool_rx_region_kunit_params(unsigned int *pages,
				      unsigned int *read_pages, bool *failed)
{
	mutex_lock(&rx_region_mutex);
	*pages = READ_ONCE(rx_region_pages);
	*read_pages = READ_ONCE(rx_region_read_pages);
	*failed = READ_ONCE(rx_region_failed);
	mutex_unlock(&rx_region_mutex);
}
EXPORT_SYMBOL_IF_KUNIT(page_pool_rx_region_kunit_params);

/* Restore the parameters and the failure flag outright. */
void page_pool_rx_region_kunit_config(unsigned int pages,
				      unsigned int read_pages, bool failed)
{
	mutex_lock(&rx_region_mutex);
	WRITE_ONCE(rx_region_pages, pages);
	WRITE_ONCE(rx_region_read_pages, read_pages);
	WRITE_ONCE(rx_region_failed, failed);
	mutex_unlock(&rx_region_mutex);
}
EXPORT_SYMBOL_IF_KUNIT(page_pool_rx_region_kunit_config);

/*
 * Publish a region over @nr pages the test owns at @base (each with
 * refcount 1), @nr_read of them split off for reads; -EBUSY if one exists.
 * The test holds it as a bound pool would (one count in r->pools), so the
 * unbind of the last test pool never reaps pages the test owns; only
 * page_pool_rx_region_kunit_remove() lets it go.
 */
int page_pool_rx_region_kunit_install(struct page *base, unsigned long nr,
				      unsigned long nr_read)
{
	struct page_pool_rx_region *r = kzalloc(sizeof(*r), GFP_KERNEL);
	struct page **free = kvmalloc_array(nr, sizeof(*free), GFP_KERNEL);
	struct rx_region_pcp __percpu *pcp[RX_REGION_PARTS] = {};
	int ret = !r || !free ? -ENOMEM : nr_read < nr ? 0 : -EINVAL;

	if (!ret && !rx_region_pcp_alloc(pcp, nr_read))
		ret = -ENOMEM;
	mutex_lock(&rx_region_mutex);
	if (!ret && rx_region)
		ret = -EBUSY;
	if (!ret) {
		rx_region_publish(r, free, pcp, page_to_pfn(base), nr, nr_read,
				  "kunit");
		r->pools = 1;
	}
	mutex_unlock(&rx_region_mutex);
	if (ret) {
		free_percpu(pcp[RX_REGION_WRITE]);
		free_percpu(pcp[RX_REGION_READ]);
		kvfree(free);
		kfree(r);
	}
	return ret;
}
EXPORT_SYMBOL_IF_KUNIT(page_pool_rx_region_kunit_install);

/*
 * Unpublish the region; free its pages too when bind_reads() allocated them
 * (@contig).  No pool or consumer may hold it.
 */
void page_pool_rx_region_kunit_remove(bool contig)
{
	struct page_pool_rx_region *r;

	mutex_lock(&rx_region_mutex);
	r = rx_region;
	WRITE_ONCE(rx_region, NULL);
	WRITE_ONCE(rx_region_reads_bound, false);
	mutex_unlock(&rx_region_mutex);
	if (!r)
		return;
	debugfs_remove(r->debugfs);
	synchronize_rcu();
#ifdef CONFIG_CONTIG_ALLOC
	if (contig)
		free_contig_range(r->base_pfn, r->nr);
#endif
	free_percpu(r->part[RX_REGION_WRITE].pcp);
	free_percpu(r->part[RX_REGION_READ].pcp);
	kvfree(r->part[RX_REGION_WRITE].shard[0].free);
	kfree(r);
	static_branch_dec(&page_pool_rx_region_key);
}
EXPORT_SYMBOL_IF_KUNIT(page_pool_rx_region_kunit_remove);

/*
 * A partition's free (shards and caches), min_free (the sum of the shards'
 * low-water marks), alloc_fail and escaped counts, as debugfs shows them.
 */
void page_pool_rx_region_kunit_part(bool read, unsigned long out[4])
{
	struct page_pool_rx_region *r;
	struct page_pool_rx_region_part *p;

	memset(out, 0, 4 * sizeof(*out));
	rcu_read_lock();
	r = rx_region_published();
	if (!r) {
		rcu_read_unlock();
		return;
	}
	p = &r->part[read ? RX_REGION_READ : RX_REGION_WRITE];
	out[0] = rx_region_shards_free(p) + rx_region_cached(p);
	out[1] = rx_region_min_free(p);
	out[2] = atomic_long_read(&p->alloc_fail);
	out[3] = atomic_long_read(&p->escaped);
	rcu_read_unlock();
}
EXPORT_SYMBOL_IF_KUNIT(page_pool_rx_region_kunit_part);

/*
 * With @i >= 0, the free pages in shard @i of a partition: its capacity in
 * @cap, how many it holds that another shard owns in @foreign.  With @i < 0,
 * the partition's: shards plus caches, the cached pages in @cached, the
 * fullest CPU cache in @maxcache.  Either way the shard count in @nshards
 * (0 for a partition of no pages; then everything is 0).
 */
unsigned long page_pool_rx_region_kunit_shards(bool read, int i,
					       unsigned long *cap,
					       unsigned int *foreign,
					       unsigned long *cached,
					       unsigned int *maxcache,
					       unsigned int *nshards)
{
	struct page_pool_rx_region *r = rx_region_published();
	struct page_pool_rx_region_part *p;
	unsigned long n, flags;
	int cpu;

	*cap = 0;
	*foreign = 0;
	*cached = 0;
	*maxcache = 0;
	*nshards = 0;
	if (!r)
		return 0;
	p = &r->part[read ? RX_REGION_READ : RX_REGION_WRITE];
	*nshards = p->nshards;
	if (i >= (int)p->nshards)
		return 0;
	if (i >= 0) {
		struct rx_region_shard *sh = &p->shard[i];
		unsigned long k;

		spin_lock_irqsave(&sh->lock, flags);
		*cap = sh->cap;
		for (k = 0; k < sh->nfree; k++)
			if ((page_to_pfn(sh->free[k]) - p->first_pfn) /
			    p->shard_pages != (unsigned long)i)
				(*foreign)++;
		n = sh->nfree;
		spin_unlock_irqrestore(&sh->lock, flags);
		return n;
	}
	if (p->pcp)
		for_each_possible_cpu(cpu) {
			unsigned int c = READ_ONCE(per_cpu_ptr(p->pcp, cpu)->count);

			*cached += c;
			*maxcache = max(*maxcache, c);
		}
	return rx_region_shards_free(p) + *cached;
}
EXPORT_SYMBOL_IF_KUNIT(page_pool_rx_region_kunit_shards);

/*
 * Take @n pages as take_pages() does (@exact), or as a device refill does
 * (not @exact: as many as are free, draining at most once a jiffy).
 */
unsigned int page_pool_rx_region_kunit_take(bool read, struct page **pages,
					    unsigned int n, bool exact)
{
	struct page_pool_rx_region *r = rx_region_published();

	if (!r || !n)
		return 0;
	return rx_region_get(rx_region_part(r, read), pages, n, exact);
}
EXPORT_SYMBOL_IF_KUNIT(page_pool_rx_region_kunit_take);

/* Drain every CPU's cache of a partition into its shards. */
void page_pool_rx_region_kunit_drain(bool read)
{
	struct page_pool_rx_region *r = rx_region_published();
	struct page_pool_rx_region_part *p;

	if (!r)
		return;
	p = &r->part[read ? RX_REGION_READ : RX_REGION_WRITE];
	if (p->pcp)
		rx_region_drain(p);
}
EXPORT_SYMBOL_IF_KUNIT(page_pool_rx_region_kunit_drain);

/* Restart the low-water marks, as a write to the debugfs file does. */
void page_pool_rx_region_kunit_reset_min_free(void)
{
	struct page_pool_rx_region *r = rx_region_published();

	if (r)
		rx_region_reset_min_free(r);
}
EXPORT_SYMBOL_IF_KUNIT(page_pool_rx_region_kunit_reset_min_free);

/*
 * A pool bound to the published region as a listed device's pool is, but
 * with no device and no DMA mapping; ERR_PTR(-ENODEV) without a region.
 * The caller destroys it with page_pool_destroy() before the region goes.
 */
struct page_pool *page_pool_rx_region_kunit_pool(void)
{
	struct page_pool_params params = {
		.order = 0,
		.pool_size = 1024,
		.nid = NUMA_NO_NODE,
	};
	struct page_pool_rx_region *r = rx_region_published();
	struct page_pool *pool;

	if (!r)
		return ERR_PTR(-ENODEV);
	pool = page_pool_create(&params);
	if (IS_ERR(pool))
		return pool;
	/* Counted as page_pool_rx_region_get() counts it; unbind uncounts. */
	mutex_lock(&rx_region_mutex);
	r->pools++;
	mutex_unlock(&rx_region_mutex);
	page_pool_rx_region_bind(pool, r, pool->p.pool_size);
	return pool;
}
EXPORT_SYMBOL_IF_KUNIT(page_pool_rx_region_kunit_pool);

static bool page_pool_recycle_in_cache(netmem_ref netmem,
				       struct page_pool *pool);

/*
 * Recycle @page (one of @pool's, refcount 1) straight into the pool's alloc
 * cache, as a direct (NAPI) return does; false, page untouched, when full.
 */
bool page_pool_rx_region_kunit_recycle(struct page_pool *pool,
				       struct page *page)
{
	bool ok;

	local_bh_disable();
	ok = page_pool_recycle_in_cache(page_to_netmem(page), pool);
	local_bh_enable();
	return ok;
}
EXPORT_SYMBOL_IF_KUNIT(page_pool_rx_region_kunit_recycle);

/*
 * Free an skb holding @page (one of @pool's, refcount 1) as a receiver on
 * another CPU frees it: onto the defer list of the CPU that "allocated" it,
 * the first other online CPU, which no device feeds. False, page untouched,
 * with no other CPU online.
 */
bool page_pool_rx_region_kunit_defer(struct page_pool *pool,
				     struct page *page)
{
	struct sk_buff *skb;
	int cpu, me;

	skb = alloc_skb(0, GFP_KERNEL);
	if (!skb)
		return false;
	local_bh_disable();
	me = smp_processor_id();
	cpu = cpumask_next_wrap(me, cpu_online_mask);
	if (cpu == me || cpu >= nr_cpu_ids) {
		local_bh_enable();
		kfree_skb(skb);
		return false;
	}
	skb_add_rx_frag(skb, 0, page, 0, PAGE_SIZE, PAGE_SIZE);
	skb_mark_for_recycle(skb);
	skb->alloc_cpu = cpu;
	skb_attempt_defer_free(skb);
	local_bh_enable();
	return true;
}
EXPORT_SYMBOL_IF_KUNIT(page_pool_rx_region_kunit_defer);
#endif


#ifdef CONFIG_PAGE_POOL_STATS
static DEFINE_PER_CPU(struct page_pool_recycle_stats, pp_system_recycle_stats);

/* alloc_stat_inc is intended to be used in softirq context */
#define alloc_stat_inc(pool, __stat)	(pool->alloc_stats.__stat++)
/* recycle_stat_inc is safe to use when preemption is possible. */
#define recycle_stat_inc(pool, __stat)							\
	do {										\
		struct page_pool_recycle_stats __percpu *s = pool->recycle_stats;	\
		this_cpu_inc(s->__stat);						\
	} while (0)

#define recycle_stat_add(pool, __stat, val)						\
	do {										\
		struct page_pool_recycle_stats __percpu *s = pool->recycle_stats;	\
		this_cpu_add(s->__stat, val);						\
	} while (0)

static const char pp_stats[][ETH_GSTRING_LEN] = {
	"rx_pp_alloc_fast",
	"rx_pp_alloc_slow",
	"rx_pp_alloc_slow_ho",
	"rx_pp_alloc_empty",
	"rx_pp_alloc_refill",
	"rx_pp_alloc_waive",
	"rx_pp_recycle_cached",
	"rx_pp_recycle_cache_full",
	"rx_pp_recycle_ring",
	"rx_pp_recycle_ring_full",
	"rx_pp_recycle_released_ref",
};

/**
 * page_pool_get_stats() - fetch page pool stats
 * @pool:	pool from which page was allocated
 * @stats:	struct page_pool_stats to fill in
 *
 * Deprecated driver API for querying stats. Page pool stats can be queried
 * via netdev Netlink.
 *
 * Retrieve statistics about the page_pool. This API is only available
 * if the kernel has been configured with ``CONFIG_PAGE_POOL_STATS=y``.
 * A pointer to a caller allocated struct page_pool_stats structure
 * is passed to this API which is filled in. The caller can then report
 * those stats to the user (perhaps via ethtool, debugfs, etc.).
 */
void page_pool_get_stats(const struct page_pool *pool,
			 struct page_pool_stats *stats)
{
	int cpu = 0;

	/* The caller is responsible to initialize stats. */
	stats->alloc_stats.fast += pool->alloc_stats.fast;
	stats->alloc_stats.slow += pool->alloc_stats.slow;
	stats->alloc_stats.slow_high_order += pool->alloc_stats.slow_high_order;
	stats->alloc_stats.empty += pool->alloc_stats.empty;
	stats->alloc_stats.refill += pool->alloc_stats.refill;
	stats->alloc_stats.waive += pool->alloc_stats.waive;

	for_each_possible_cpu(cpu) {
		const struct page_pool_recycle_stats *pcpu =
			per_cpu_ptr(pool->recycle_stats, cpu);

		stats->recycle_stats.cached += pcpu->cached;
		stats->recycle_stats.cache_full += pcpu->cache_full;
		stats->recycle_stats.ring += pcpu->ring;
		stats->recycle_stats.ring_full += pcpu->ring_full;
		stats->recycle_stats.released_refcnt += pcpu->released_refcnt;
	}
}
EXPORT_SYMBOL(page_pool_get_stats);

u8 *page_pool_ethtool_stats_get_strings(u8 *data)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(pp_stats); i++) {
		memcpy(data, pp_stats[i], ETH_GSTRING_LEN);
		data += ETH_GSTRING_LEN;
	}

	return data;
}
EXPORT_SYMBOL(page_pool_ethtool_stats_get_strings);

int page_pool_ethtool_stats_get_count(void)
{
	return ARRAY_SIZE(pp_stats);
}
EXPORT_SYMBOL(page_pool_ethtool_stats_get_count);

u64 *page_pool_ethtool_stats_get(u64 *data, const void *stats)
{
	const struct page_pool_stats *pool_stats = stats;

	*data++ = pool_stats->alloc_stats.fast;
	*data++ = pool_stats->alloc_stats.slow;
	*data++ = pool_stats->alloc_stats.slow_high_order;
	*data++ = pool_stats->alloc_stats.empty;
	*data++ = pool_stats->alloc_stats.refill;
	*data++ = pool_stats->alloc_stats.waive;
	*data++ = pool_stats->recycle_stats.cached;
	*data++ = pool_stats->recycle_stats.cache_full;
	*data++ = pool_stats->recycle_stats.ring;
	*data++ = pool_stats->recycle_stats.ring_full;
	*data++ = pool_stats->recycle_stats.released_refcnt;

	return data;
}
EXPORT_SYMBOL(page_pool_ethtool_stats_get);

#else
#define alloc_stat_inc(...)	do { } while (0)
#define recycle_stat_inc(...)	do { } while (0)
#define recycle_stat_add(...)	do { } while (0)
#endif

static bool page_pool_producer_lock(struct page_pool *pool)
	__acquires(&pool->ring.producer_lock)
{
	bool in_softirq = in_softirq();

	if (in_softirq)
		spin_lock(&pool->ring.producer_lock);
	else
		spin_lock_bh(&pool->ring.producer_lock);

	return in_softirq;
}

static void page_pool_producer_unlock(struct page_pool *pool,
				      bool in_softirq)
	__releases(&pool->ring.producer_lock)
{
	if (in_softirq)
		spin_unlock(&pool->ring.producer_lock);
	else
		spin_unlock_bh(&pool->ring.producer_lock);
}

static void page_pool_struct_check(void)
{
	CACHELINE_ASSERT_GROUP_MEMBER(struct page_pool, frag, frag_users);
	CACHELINE_ASSERT_GROUP_MEMBER(struct page_pool, frag, frag_page);
	CACHELINE_ASSERT_GROUP_MEMBER(struct page_pool, frag, frag_offset);
	CACHELINE_ASSERT_GROUP_SIZE(struct page_pool, frag,
				    PAGE_POOL_FRAG_GROUP_ALIGN);
}

static int page_pool_init(struct page_pool *pool,
			  const struct page_pool_params *params,
			  int cpuid)
{
	unsigned int ring_qsize = 1024; /* Default */
	struct netdev_rx_queue *rxq;
	int err;

	page_pool_struct_check();

	memcpy(&pool->p, &params->fast, sizeof(pool->p));
	memcpy(&pool->slow, &params->slow, sizeof(pool->slow));

	pool->cpuid = cpuid;
	pool->dma_sync_for_cpu = true;

	/* Validate only known flags were used */
	if (pool->slow.flags & ~PP_FLAG_ALL)
		return -EINVAL;

	if (pool->p.pool_size)
		ring_qsize = min(pool->p.pool_size, 16384);

	/* DMA direction is either DMA_FROM_DEVICE or DMA_BIDIRECTIONAL.
	 * DMA_BIDIRECTIONAL is for allowing page used for DMA sending,
	 * which is the XDP_TX use-case.
	 */
	if (pool->slow.flags & PP_FLAG_DMA_MAP) {
		if ((pool->p.dma_dir != DMA_FROM_DEVICE) &&
		    (pool->p.dma_dir != DMA_BIDIRECTIONAL))
			return -EINVAL;

		pool->dma_map = true;
	}

	if (pool->slow.flags & PP_FLAG_DMA_SYNC_DEV) {
		/* In order to request DMA-sync-for-device the page
		 * needs to be mapped
		 */
		if (!(pool->slow.flags & PP_FLAG_DMA_MAP))
			return -EINVAL;

		if (!pool->p.max_len)
			return -EINVAL;

		pool->dma_sync = true;

		/* pool->p.offset has to be set according to the address
		 * offset used by the DMA engine to start copying rx data
		 */
	}

	pool->has_init_callback = !!pool->slow.init_callback;

#ifdef CONFIG_PAGE_POOL_STATS
	if (!(pool->slow.flags & PP_FLAG_SYSTEM_POOL)) {
		pool->recycle_stats = alloc_percpu(struct page_pool_recycle_stats);
		if (!pool->recycle_stats)
			return -ENOMEM;
	} else {
		/* For system page pool instance we use a singular stats object
		 * instead of allocating a separate percpu variable for each
		 * (also percpu) page pool instance.
		 */
		pool->recycle_stats = &pp_system_recycle_stats;
		pool->system = true;
	}
#endif

	if (ptr_ring_init(&pool->ring, ring_qsize, GFP_KERNEL) < 0) {
#ifdef CONFIG_PAGE_POOL_STATS
		if (!pool->system)
			free_percpu(pool->recycle_stats);
#endif
		return -ENOMEM;
	}

	atomic_set(&pool->pages_state_release_cnt, 0);

	/* Driver calling page_pool_create() also call page_pool_destroy() */
	refcount_set(&pool->user_cnt, 1);

	xa_init_flags(&pool->dma_mapped, XA_FLAGS_ALLOC1);

	if (pool->slow.flags & PP_FLAG_ALLOW_UNREADABLE_NETMEM) {
		netdev_assert_locked(pool->slow.netdev);
		rxq = __netif_get_rx_queue(pool->slow.netdev,
					   pool->slow.queue_idx);
		pool->mp_priv = rxq->mp_params.mp_priv;
		pool->mp_ops = rxq->mp_params.mp_ops;
	}

	if (pool->mp_ops) {
		if (!pool->dma_map || !pool->dma_sync) {
			err = -EOPNOTSUPP;
			goto free_ptr_ring;
		}

		if (WARN_ON(!is_kernel_rodata((unsigned long)pool->mp_ops))) {
			err = -EFAULT;
			goto free_ptr_ring;
		}

		err = pool->mp_ops->init(pool);
		if (err) {
			pr_warn("%s() mem-provider init failed %d\n", __func__,
				err);
			goto free_ptr_ring;
		}

		static_branch_inc(&page_pool_mem_providers);
	} else if (pool->p.order > MAX_PAGE_ORDER) {
		err = -EINVAL;
		goto free_ptr_ring;
	} else {
		page_pool_rx_region_bind(pool, page_pool_rx_region_get(pool),
					 ring_qsize);
	}

	return 0;

free_ptr_ring:
	ptr_ring_cleanup(&pool->ring, NULL);
	xa_destroy(&pool->dma_mapped);
#ifdef CONFIG_PAGE_POOL_STATS
	if (!pool->system)
		free_percpu(pool->recycle_stats);
#endif
	return err;
}

static void page_pool_uninit(struct page_pool *pool)
{
	ptr_ring_cleanup(&pool->ring, NULL);
	kvfree(pool->rx_stack);
	xa_destroy(&pool->dma_mapped);

#ifdef CONFIG_PAGE_POOL_STATS
	if (!pool->system)
		free_percpu(pool->recycle_stats);
#endif

	if (pool->mp_ops) {
		pool->mp_ops->destroy(pool);
		static_branch_dec(&page_pool_mem_providers);
	}
}

/**
 * page_pool_create_percpu() - create a page pool for a given cpu.
 * @params: parameters, see struct page_pool_params
 * @cpuid: cpu identifier
 */
struct page_pool *
page_pool_create_percpu(const struct page_pool_params *params, int cpuid)
{
	struct page_pool *pool;
	int err;

	pool = kzalloc_node(sizeof(*pool), GFP_KERNEL, params->nid);
	if (!pool)
		return ERR_PTR(-ENOMEM);

	err = page_pool_init(pool, params, cpuid);
	if (err < 0)
		goto err_free;

	err = page_pool_list(pool);
	if (err)
		goto err_uninit;

	return pool;

err_uninit:
	page_pool_uninit(pool);
	page_pool_rx_region_unbind(pool);
err_free:
	pr_warn("%s() gave up with errno %d\n", __func__, err);
	kfree(pool);
	return ERR_PTR(err);
}
EXPORT_SYMBOL(page_pool_create_percpu);

/**
 * page_pool_create() - create a page pool
 * @params: parameters, see struct page_pool_params
 */
struct page_pool *page_pool_create(const struct page_pool_params *params)
{
	return page_pool_create_percpu(params, -1);
}
EXPORT_SYMBOL(page_pool_create);

static void page_pool_return_netmem(struct page_pool *pool, netmem_ref netmem);

/*
 * A pool bound to the fixed receive region keeps its recycled pages on a
 * stack, not in the ring: a ring hands back the page that has waited
 * longest, and a receive buffer is worth most while the cache still holds
 * it.  The producer lock covers both ends; the consumer takes it once for
 * each refill of the alloc cache.
 */
/*
 * Every pool goes through the stack's and the region's branches; while
 * there is no region, no pool is bound to one or has a stack, and each is
 * a patched-out jump.
 */
static bool page_pool_has_rx_stack(const struct page_pool *pool)
{
	return page_pool_rx_region_exists() && pool->rx_stack;
}

static bool page_pool_rx_stack_push(struct page_pool *pool, netmem_ref netmem)
{
	lockdep_assert_held(&pool->ring.producer_lock);

	if (pool->rx_stack_n == pool->rx_stack_size)
		return false;
	pool->rx_stack[pool->rx_stack_n++] = netmem;
	return true;
}

static noinline netmem_ref page_pool_rx_stack_refill(struct page_pool *pool)
{
	unsigned int n;
	bool in_softirq;

	/* Quicker fallback, avoid the lock when the stack is empty */
	if (!READ_ONCE(pool->rx_stack_n)) {
		alloc_stat_inc(pool, empty);
		return 0;
	}

	in_softirq = page_pool_producer_lock(pool);
	/* Only region-bound pools have a stack: refill by their bound. */
	n = min_t(unsigned int, pool->rx_stack_n,
		  pool->alloc.count < RX_REGION_POOL_REFILL ?
		  RX_REGION_POOL_REFILL - pool->alloc.count : 0);
	/* In stack order, so the alloc cache hands out the newest first. */
	memcpy(&pool->alloc.cache[pool->alloc.count],
	       &pool->rx_stack[pool->rx_stack_n - n],
	       n * sizeof(*pool->rx_stack));
	WRITE_ONCE(pool->rx_stack_n, pool->rx_stack_n - n);
	pool->alloc.count += n;
	page_pool_producer_unlock(pool, in_softirq);

	if (unlikely(!pool->alloc.count)) {
		alloc_stat_inc(pool, empty);
		return 0;
	}
	alloc_stat_inc(pool, refill);
	return pool->alloc.cache[--pool->alloc.count];
}

static noinline netmem_ref page_pool_refill_alloc_cache(struct page_pool *pool)
{
	struct ptr_ring *r = &pool->ring;
	netmem_ref netmem;
	int pref_nid; /* preferred NUMA node */

	if (page_pool_has_rx_stack(pool))
		return page_pool_rx_stack_refill(pool);

	/* Quicker fallback, avoid locks when ring is empty */
	if (__ptr_ring_empty(r)) {
		alloc_stat_inc(pool, empty);
		return 0;
	}

	/* Softirq guarantee CPU and thus NUMA node is stable. This,
	 * assumes CPU refilling driver RX-ring will also run RX-NAPI.
	 */
#ifdef CONFIG_NUMA
	pref_nid = (pool->p.nid == NUMA_NO_NODE) ? numa_mem_id() : pool->p.nid;
#else
	/* Ignore pool->p.nid setting if !CONFIG_NUMA, helps compiler */
	pref_nid = numa_mem_id(); /* will be zero like page_to_nid() */
#endif

	/* Refill alloc array, but only if NUMA match */
	do {
		netmem = (__force netmem_ref)__ptr_ring_consume(r);
		if (unlikely(!netmem))
			break;

		if (likely(netmem_is_pref_nid(netmem, pref_nid))) {
			pool->alloc.cache[pool->alloc.count++] = netmem;
		} else {
			/* NUMA mismatch;
			 * (1) release 1 page to page-allocator and
			 * (2) break out to fallthrough to alloc_pages_node.
			 * This limit stress on page buddy alloactor.
			 */
			page_pool_return_netmem(pool, netmem);
			alloc_stat_inc(pool, waive);
			netmem = 0;
			break;
		}
	} while (pool->alloc.count < PP_ALLOC_CACHE_REFILL);

	/* Return last page */
	if (likely(pool->alloc.count > 0)) {
		netmem = pool->alloc.cache[--pool->alloc.count];
		alloc_stat_inc(pool, refill);
	}

	return netmem;
}

/* fast path */
static netmem_ref __page_pool_get_cached(struct page_pool *pool)
{
	netmem_ref netmem;

	/* Caller MUST guarantee safe non-concurrent access, e.g. softirq */
	if (likely(pool->alloc.count)) {
		/* Fast-path */
		netmem = pool->alloc.cache[--pool->alloc.count];
		alloc_stat_inc(pool, fast);
	} else {
		netmem = page_pool_refill_alloc_cache(pool);
	}

	return netmem;
}

static void __page_pool_dma_sync_for_device(const struct page_pool *pool,
					    netmem_ref netmem,
					    u32 dma_sync_size)
{
#if defined(CONFIG_HAS_DMA) && defined(CONFIG_DMA_NEED_SYNC)
	dma_addr_t dma_addr = page_pool_get_dma_addr_netmem(netmem);

	dma_sync_size = min(dma_sync_size, pool->p.max_len);
	__dma_sync_single_for_device(pool->p.dev, dma_addr + pool->p.offset,
				     dma_sync_size, pool->p.dma_dir);
#endif
}

static __always_inline void
page_pool_dma_sync_for_device(const struct page_pool *pool,
			      netmem_ref netmem,
			      u32 dma_sync_size)
{
	if (pool->dma_sync && dma_dev_need_sync(pool->p.dev)) {
		rcu_read_lock();
		/* re-check under rcu_read_lock() to sync with page_pool_scrub() */
		if (pool->dma_sync)
			__page_pool_dma_sync_for_device(pool, netmem,
							dma_sync_size);
		rcu_read_unlock();
	}
}

static int page_pool_register_dma_index(struct page_pool *pool,
					netmem_ref netmem, gfp_t gfp)
{
	int err = 0;
	u32 id;

	if (unlikely(!PP_DMA_INDEX_BITS))
		goto out;

	/*
	 * Drivers request GFP flags according to both the current context and
	 * the device constraints, but the XArray entry itself is by no mean
	 * used by the device, so remove zone/policy flags.
	 */
	gfp &= ~(__GFP_DMA | __GFP_DMA32 | __GFP_HIGHMEM | __GFP_COMP);

	if (in_softirq())
		err = xa_alloc(&pool->dma_mapped, &id, netmem_to_page(netmem),
			       PP_DMA_INDEX_LIMIT, gfp);
	else
		err = xa_alloc_bh(&pool->dma_mapped, &id, netmem_to_page(netmem),
				  PP_DMA_INDEX_LIMIT, gfp);
	if (err) {
		WARN_ONCE(err != -ENOMEM, "couldn't track DMA mapping, please report to netdev@");
		goto out;
	}

	netmem_set_dma_index(netmem, id);
out:
	return err;
}

static void __page_pool_unmap_netmem_dma(struct page_pool *pool,
					 netmem_ref netmem)
{
	struct page *old, *page = netmem_to_page(netmem);
	unsigned long id;
	dma_addr_t dma;

	if (!pool->dma_map)
		return;

	/* Cache dma_addr before xa_cmpxchg. The scrub path holds no page ref;
	 * the unref path calls put_page() regardless of cmpxchg outcome, so
	 * after the cmpxchg we cannot safely touch netmem fields.
	 */
	dma = page_pool_get_dma_addr_netmem(netmem);

	if (likely(PP_DMA_INDEX_BITS)) {
		id = netmem_get_dma_index(netmem);
		if (!id)
			return;

		if (in_softirq())
			old = xa_cmpxchg(&pool->dma_mapped,
					 id, page, NULL, 0);
		else
			old = xa_cmpxchg_bh(&pool->dma_mapped,
					    id, page, NULL, 0);
		if (old != page)
			return;
	}

	dma_unmap_page_attrs(pool->p.dev, dma,
			     PAGE_SIZE << pool->p.order, pool->p.dma_dir,
			     DMA_ATTR_SKIP_CPU_SYNC | DMA_ATTR_WEAK_ORDERING);
}

static bool page_pool_dma_map(struct page_pool *pool, netmem_ref netmem, gfp_t gfp)
{
	dma_addr_t dma;
	int err;

	/* Setup DMA mapping: use 'struct page' area for storing DMA-addr
	 * since dma_addr_t can be either 32 or 64 bits and does not always fit
	 * into page private data (i.e 32bit cpu with 64bit DMA caps)
	 * This mapping is kept for lifetime of page, until leaving pool.
	 */
	dma = dma_map_page_attrs(pool->p.dev, netmem_to_page(netmem), 0,
				 (PAGE_SIZE << pool->p.order), pool->p.dma_dir,
				 DMA_ATTR_SKIP_CPU_SYNC |
					 DMA_ATTR_WEAK_ORDERING);
	if (dma_mapping_error(pool->p.dev, dma))
		return false;

	if (page_pool_set_dma_addr_netmem(netmem, dma)) {
		WARN_ONCE(1, "unexpected DMA address, please report to netdev@");
		goto unmap_failed;
	}

	err = page_pool_register_dma_index(pool, netmem, gfp);
	if (err)
		goto unset_failed;

	page_pool_dma_sync_for_device(pool, netmem, pool->p.max_len);

	return true;

unset_failed:
	page_pool_set_dma_addr_netmem(netmem, 0);
unmap_failed:
	dma_unmap_page_attrs(pool->p.dev, dma,
			     PAGE_SIZE << pool->p.order, pool->p.dma_dir,
			     DMA_ATTR_SKIP_CPU_SYNC | DMA_ATTR_WEAK_ORDERING);
	return false;
}

static struct page *__page_pool_alloc_page_order(struct page_pool *pool,
						 gfp_t gfp)
{
	struct page *page;

	gfp |= __GFP_COMP;
	page = alloc_pages_node(pool->p.nid, gfp, pool->p.order);
	if (unlikely(!page))
		return NULL;

	if (pool->dma_map && unlikely(!page_pool_dma_map(pool, page_to_netmem(page), gfp))) {
		put_page(page);
		return NULL;
	}

	alloc_stat_inc(pool, slow_high_order);
	page_pool_set_pp_info(pool, page_to_netmem(page));

	/* Track how many pages are held 'in-flight' */
	pool->pages_state_hold_cnt++;
	trace_page_pool_state_hold(pool, page_to_netmem(page),
				   pool->pages_state_hold_cnt);
	return page;
}

/* Slow path for a pool bound to the fixed receive region. */
static noinline netmem_ref page_pool_rx_region_alloc(struct page_pool *pool,
						     gfp_t gfp)
{
	/* Every listed device's pools draw on the write partition. */
	struct page_pool_rx_region_part *r =
		&READ_ONCE(pool->rx_region)->part[RX_REGION_WRITE];
	struct page *pages[RX_REGION_POOL_REFILL];
	netmem_ref netmem;
	int i, n;

	if (unlikely(pool->alloc.count > 0))
		return pool->alloc.cache[--pool->alloc.count];

	n = rx_region_get(r, pages, RX_REGION_POOL_REFILL, false);
	if (!n)
		atomic_long_inc(&r->alloc_fail);

	for (i = 0; i < n; i++) {
		netmem = page_to_netmem(pages[i]);
		if (pool->dma_map &&
		    unlikely(!page_pool_dma_map(pool, netmem, gfp))) {
			page_pool_rx_region_put(r, pages[i]);
			continue;
		}
		page_pool_set_pp_info(pool, netmem);
		pool->alloc.cache[pool->alloc.count++] = netmem;
		pool->pages_state_hold_cnt++;
		trace_page_pool_state_hold(pool, netmem,
					   pool->pages_state_hold_cnt);
	}
	if (likely(pool->alloc.count > 0)) {
		alloc_stat_inc(pool, slow);
		return pool->alloc.cache[--pool->alloc.count];
	}
	return 0;
}

/* slow path */
static noinline netmem_ref __page_pool_alloc_netmems_slow(struct page_pool *pool,
							  gfp_t gfp)
{
	const int bulk = PP_ALLOC_CACHE_REFILL;
	unsigned int pp_order = pool->p.order;
	bool dma_map = pool->dma_map;
	netmem_ref netmem;
	int i, nr_pages;

	/* Unconditionally set NOWARN if allocating from NAPI.
	 * Drivers forget to set it, and OOM reports on packet Rx are useless.
	 */
	if ((gfp & GFP_ATOMIC) == GFP_ATOMIC)
		gfp |= __GFP_NOWARN;

	/* Don't support bulk alloc for high-order pages */
	if (unlikely(pp_order))
		return page_to_netmem(__page_pool_alloc_page_order(pool, gfp));

	/* Unnecessary as alloc cache is empty, but guarantees zero count */
	if (unlikely(pool->alloc.count > 0))
		return pool->alloc.cache[--pool->alloc.count];

	/* Mark empty alloc.cache slots "empty" for alloc_pages_bulk */
	memset(&pool->alloc.cache, 0, sizeof(void *) * bulk);

	nr_pages = alloc_pages_bulk_node(gfp, pool->p.nid, bulk,
					 (struct page **)pool->alloc.cache);
	if (unlikely(!nr_pages))
		return 0;

	/* Pages have been filled into alloc.cache array, but count is zero and
	 * page element have not been (possibly) DMA mapped.
	 */
	for (i = 0; i < nr_pages; i++) {
		netmem = pool->alloc.cache[i];
		if (dma_map && unlikely(!page_pool_dma_map(pool, netmem, gfp))) {
			put_page(netmem_to_page(netmem));
			continue;
		}

		page_pool_set_pp_info(pool, netmem);
		pool->alloc.cache[pool->alloc.count++] = netmem;
		/* Track how many pages are held 'in-flight' */
		pool->pages_state_hold_cnt++;
		trace_page_pool_state_hold(pool, netmem,
					   pool->pages_state_hold_cnt);
	}

	/* Return last page */
	if (likely(pool->alloc.count > 0)) {
		netmem = pool->alloc.cache[--pool->alloc.count];
		alloc_stat_inc(pool, slow);
	} else {
		netmem = 0;
	}

	/* When page just alloc'ed is should/must have refcnt 1. */
	return netmem;
}

/* For using page_pool replace: alloc_pages() API calls, but provide
 * synchronization guarantee for allocation side.
 */
netmem_ref page_pool_alloc_netmems(struct page_pool *pool, gfp_t gfp)
{
	netmem_ref netmem;

	/* Fast-path: Get a page from cache */
	netmem = __page_pool_get_cached(pool);
	if (netmem)
		return netmem;

	/* Slow-path: cache empty, do real allocation */
	if (static_branch_unlikely(&page_pool_mem_providers) && pool->mp_ops)
		netmem = pool->mp_ops->alloc_netmems(pool, gfp);
	else if (page_pool_rx_region_exists() && READ_ONCE(pool->rx_region))
		netmem = page_pool_rx_region_alloc(pool, gfp);
	else
		netmem = __page_pool_alloc_netmems_slow(pool, gfp);
	return netmem;
}
EXPORT_SYMBOL(page_pool_alloc_netmems);
ALLOW_ERROR_INJECTION(page_pool_alloc_netmems, NULL);

struct page *page_pool_alloc_pages(struct page_pool *pool, gfp_t gfp)
{
	return netmem_to_page(page_pool_alloc_netmems(pool, gfp));
}
EXPORT_SYMBOL(page_pool_alloc_pages);

/* Calculate distance between two u32 values, valid if distance is below 2^(31)
 *  https://en.wikipedia.org/wiki/Serial_number_arithmetic#General_Solution
 */
#define _distance(a, b)	(s32)((a) - (b))

s32 page_pool_inflight(const struct page_pool *pool, bool strict)
{
	u32 release_cnt = atomic_read(&pool->pages_state_release_cnt);
	u32 hold_cnt = READ_ONCE(pool->pages_state_hold_cnt);
	s32 inflight;

	inflight = _distance(hold_cnt, release_cnt);

	if (strict) {
		trace_page_pool_release(pool, inflight, hold_cnt, release_cnt);
		WARN(inflight < 0, "Negative(%d) inflight packet-pages",
		     inflight);
	} else {
		inflight = max(0, inflight);
	}

	return inflight;
}

void page_pool_set_pp_info(struct page_pool *pool, netmem_ref netmem)
{
	netmem_set_pp(netmem, pool);
	netmem_or_pp_magic(netmem, PP_SIGNATURE);

	/* Ensuring all pages have been split into one fragment initially:
	 * page_pool_set_pp_info() is only called once for every page when it
	 * is allocated from the page allocator and page_pool_fragment_page()
	 * is dirtying the same cache line as the page->pp_magic above, so
	 * the overhead is negligible.
	 */
	page_pool_fragment_netmem(netmem, 1);
	if (pool->has_init_callback)
		pool->slow.init_callback(netmem, pool->slow.init_arg);
}

void page_pool_clear_pp_info(netmem_ref netmem)
{
	netmem_clear_pp_magic(netmem);
	netmem_set_pp(netmem, NULL);
}

static __always_inline void __page_pool_release_netmem_dma(struct page_pool *pool,
							   netmem_ref netmem)
{
	/* Caller must hold a page ref: __page_pool_unmap_netmem_dma() is
	 * safe without a ref, but the field clears below require it.
	 */
	if (!pool->dma_map)
		return;

	__page_pool_unmap_netmem_dma(pool, netmem);
	page_pool_set_dma_addr_netmem(netmem, 0);
	if (likely(PP_DMA_INDEX_BITS))
		netmem_set_dma_index(netmem, 0);
}

/* Disconnects a page (from a page_pool).  API users can have a need
 * to disconnect a page (from a page_pool), to allow it to be used as
 * a regular page (that will eventually be returned to the normal
 * page-allocator via put_page).
 */
static void page_pool_return_netmem(struct page_pool *pool, netmem_ref netmem)
{
	/* Read before the release count: the pool may go once it is up. */
	struct page_pool_rx_region *region =
		page_pool_rx_region_exists() ? READ_ONCE(pool->rx_region) : NULL;
	int count;
	bool put;

	put = true;
	if (static_branch_unlikely(&page_pool_mem_providers) && pool->mp_ops)
		put = pool->mp_ops->release_netmem(pool, netmem);
	else
		__page_pool_release_netmem_dma(pool, netmem);

	/* This may be the last page returned, releasing the pool, so
	 * it is not safe to reference pool afterwards.
	 */
	count = atomic_inc_return_relaxed(&pool->pages_state_release_cnt);
	trace_page_pool_state_release(pool, netmem, count);

	if (put) {
		page_pool_clear_pp_info(netmem);
		if (likely(!region) ||
		    !page_pool_rx_region_put(&region->part[RX_REGION_WRITE],
					     netmem_to_page(netmem)))
			put_page(netmem_to_page(netmem));
	}
	/* An optimization would be to call __free_pages(page, pool->p.order)
	 * knowing page is not part of page-cache (thus avoiding a
	 * __page_cache_release() call).
	 */
}

static bool page_pool_recycle_in_ring(struct page_pool *pool, netmem_ref netmem)
{
	bool in_softirq, ret;

	/* BH protection not needed if current is softirq */
	in_softirq = page_pool_producer_lock(pool);
	if (page_pool_has_rx_stack(pool))
		ret = page_pool_rx_stack_push(pool, netmem);
	else
		ret = !__ptr_ring_produce(&pool->ring, (__force void *)netmem);
	if (ret)
		recycle_stat_inc(pool, ring);
	page_pool_producer_unlock(pool, in_softirq);

	return ret;
}

/* Only allow direct recycling in special circumstances, into the
 * alloc side cache.  E.g. during RX-NAPI processing for XDP_DROP use-case.
 *
 * Caller must provide appropriate safe context.
 */
static bool page_pool_recycle_in_cache(netmem_ref netmem,
				       struct page_pool *pool)
{
	if (unlikely(pool->alloc.count == PP_ALLOC_CACHE_SIZE ||
		     (READ_ONCE(pool->rx_region) &&
		      pool->alloc.count >= RX_REGION_POOL_CACHE))) {
		recycle_stat_inc(pool, cache_full);
		return false;
	}

	/* Caller MUST have verified/know (page_ref_count(page) == 1) */
	pool->alloc.cache[pool->alloc.count++] = netmem;
	recycle_stat_inc(pool, cached);
	return true;
}

static bool __page_pool_page_can_be_recycled(netmem_ref netmem)
{
	return netmem_is_net_iov(netmem) ||
	       (page_ref_count(netmem_to_page(netmem)) == 1 &&
		!page_is_pfmemalloc(netmem_to_page(netmem)));
}

/* If the page refcnt == 1, this will try to recycle the page.
 * If pool->dma_sync is set, we'll try to sync the DMA area for
 * the configured size min(dma_sync_size, pool->max_len).
 * If the page refcnt != 1, then the page will be returned to memory
 * subsystem.
 */
static __always_inline netmem_ref
__page_pool_put_page(struct page_pool *pool, netmem_ref netmem,
		     unsigned int dma_sync_size, bool allow_direct)
{
	lockdep_assert_no_hardirq();

	/* This allocator is optimized for the XDP mode that uses
	 * one-frame-per-page, but have fallbacks that act like the
	 * regular page allocator APIs.
	 *
	 * refcnt == 1 means page_pool owns page, and can recycle it.
	 *
	 * page is NOT reusable when allocated when system is under
	 * some pressure. (page_is_pfmemalloc)
	 */
	if (likely(__page_pool_page_can_be_recycled(netmem))) {
		/* Read barrier done in page_ref_count / READ_ONCE */

		page_pool_dma_sync_for_device(pool, netmem, dma_sync_size);

		if (allow_direct && page_pool_recycle_in_cache(netmem, pool))
			return 0;

		/* Page found as candidate for recycling */
		return netmem;
	}

	/* Fallback/non-XDP mode: API user have elevated refcnt.
	 *
	 * Many drivers split up the page into fragments, and some
	 * want to keep doing this to save memory and do refcnt based
	 * recycling. Support this use case too, to ease drivers
	 * switching between XDP/non-XDP.
	 *
	 * In-case page_pool maintains the DMA mapping, API user must
	 * call page_pool_put_page once.  In this elevated refcnt
	 * case, the DMA is unmapped/released, as driver is likely
	 * doing refcnt based recycle tricks, meaning another process
	 * will be invoking put_page.
	 */
	recycle_stat_inc(pool, released_refcnt);
	page_pool_return_netmem(pool, netmem);

	return 0;
}

static bool page_pool_napi_local(const struct page_pool *pool)
{
	const struct napi_struct *napi;
	u32 cpuid;

	/* On PREEMPT_RT the softirq can be preempted by the consumer */
	if (IS_ENABLED(CONFIG_PREEMPT_RT))
		return false;

	if (unlikely(!in_softirq()))
		return false;

	/* Allow direct recycle if we have reasons to believe that we are
	 * in the same context as the consumer would run, so there's
	 * no possible race.
	 * __page_pool_put_page() makes sure we're not in hardirq context
	 * and interrupts are enabled prior to accessing the cache.
	 */
	cpuid = smp_processor_id();
	if (READ_ONCE(pool->cpuid) == cpuid)
		return true;

	napi = READ_ONCE(pool->p.napi);

	return napi && READ_ONCE(napi->list_owner) == cpuid;
}

void page_pool_put_unrefed_netmem(struct page_pool *pool, netmem_ref netmem,
				  unsigned int dma_sync_size, bool allow_direct)
{
	if (!allow_direct)
		allow_direct = page_pool_napi_local(pool);

	netmem = __page_pool_put_page(pool, netmem, dma_sync_size,
				      allow_direct);
	if (netmem && !page_pool_recycle_in_ring(pool, netmem)) {
		/* Cache full, fallback to free pages */
		recycle_stat_inc(pool, ring_full);
		page_pool_return_netmem(pool, netmem);
	}
}
EXPORT_SYMBOL(page_pool_put_unrefed_netmem);

void page_pool_put_unrefed_page(struct page_pool *pool, struct page *page,
				unsigned int dma_sync_size, bool allow_direct)
{
	page_pool_put_unrefed_netmem(pool, page_to_netmem(page), dma_sync_size,
				     allow_direct);
}
EXPORT_SYMBOL(page_pool_put_unrefed_page);

static void page_pool_recycle_ring_bulk(struct page_pool *pool,
					netmem_ref *bulk,
					u32 bulk_len)
{
	bool in_softirq;
	u32 i;

	/* Bulk produce into ptr_ring page_pool cache */
	in_softirq = page_pool_producer_lock(pool);

	for (i = 0; i < bulk_len; i++) {
		if (page_pool_has_rx_stack(pool) ?
		    !page_pool_rx_stack_push(pool, bulk[i]) :
		    __ptr_ring_produce(&pool->ring, (__force void *)bulk[i])) {
			/* ring full */
			recycle_stat_inc(pool, ring_full);
			break;
		}
	}

	recycle_stat_add(pool, ring, i);
	page_pool_producer_unlock(pool, in_softirq);

	/* Hopefully all pages were returned into ptr_ring */
	if (likely(i == bulk_len))
		return;

	/*
	 * ptr_ring cache is full, free remaining pages outside producer lock
	 * since put_page() with refcnt == 1 can be an expensive operation.
	 */
	for (; i < bulk_len; i++)
		page_pool_return_netmem(pool, bulk[i]);
}

/**
 * page_pool_put_netmem_bulk() - release references on multiple netmems
 * @data:	array holding netmem references
 * @count:	number of entries in @data
 *
 * Tries to refill a number of netmems into the ptr_ring cache holding ptr_ring
 * producer lock. If the ptr_ring is full, page_pool_put_netmem_bulk()
 * will release leftover netmems to the memory provider.
 * page_pool_put_netmem_bulk() is suitable to be run inside the driver NAPI tx
 * completion loop for the XDP_REDIRECT use case.
 *
 * Please note the caller must not use data area after running
 * page_pool_put_netmem_bulk(), as this function overwrites it.
 */
void page_pool_put_netmem_bulk(netmem_ref *data, u32 count)
{
	u32 bulk_len = 0;

	for (u32 i = 0; i < count; i++) {
		netmem_ref netmem = netmem_compound_head(data[i]);

		if (page_pool_unref_and_test(netmem))
			data[bulk_len++] = netmem;
	}

	count = bulk_len;
	while (count) {
		netmem_ref bulk[XDP_BULK_QUEUE_SIZE];
		struct page_pool *pool = NULL;
		bool allow_direct;
		u32 foreign = 0;

		bulk_len = 0;

		for (u32 i = 0; i < count; i++) {
			struct page_pool *netmem_pp;
			netmem_ref netmem = data[i];

			netmem_pp = netmem_get_pp(netmem);
			if (unlikely(!pool)) {
				pool = netmem_pp;
				allow_direct = page_pool_napi_local(pool);
			} else if (netmem_pp != pool) {
				/*
				 * If the netmem belongs to a different
				 * page_pool, save it for another round.
				 */
				data[foreign++] = netmem;
				continue;
			}

			netmem = __page_pool_put_page(pool, netmem, -1,
						      allow_direct);
			/* Approved for bulk recycling in ptr_ring cache */
			if (netmem)
				bulk[bulk_len++] = netmem;
		}

		if (bulk_len)
			page_pool_recycle_ring_bulk(pool, bulk, bulk_len);

		count = foreign;
	}
}
EXPORT_SYMBOL(page_pool_put_netmem_bulk);

static netmem_ref page_pool_drain_frag(struct page_pool *pool,
				       netmem_ref netmem)
{
	long drain_count = BIAS_MAX - pool->frag_users;

	/* Some user is still using the page frag */
	if (likely(page_pool_unref_netmem(netmem, drain_count)))
		return 0;

	if (__page_pool_page_can_be_recycled(netmem)) {
		page_pool_dma_sync_for_device(pool, netmem, -1);
		return netmem;
	}

	page_pool_return_netmem(pool, netmem);
	return 0;
}

static void page_pool_free_frag(struct page_pool *pool)
{
	long drain_count = BIAS_MAX - pool->frag_users;
	netmem_ref netmem = pool->frag_page;

	pool->frag_page = 0;

	if (!netmem || page_pool_unref_netmem(netmem, drain_count))
		return;

	page_pool_return_netmem(pool, netmem);
}

netmem_ref page_pool_alloc_frag_netmem(struct page_pool *pool,
				       unsigned int *offset, unsigned int size,
				       gfp_t gfp)
{
	unsigned int max_size = PAGE_SIZE << pool->p.order;
	netmem_ref netmem = pool->frag_page;

	if (WARN_ON(size > max_size))
		return 0;

	size = ALIGN(size, max_t(unsigned int, dma_get_cache_alignment(),
				 __alignof__(struct skb_shared_info)));
	*offset = pool->frag_offset;

	if (netmem && *offset + size > max_size) {
		netmem = page_pool_drain_frag(pool, netmem);
		if (netmem) {
			recycle_stat_inc(pool, cached);
			alloc_stat_inc(pool, fast);
			goto frag_reset;
		}
	}

	if (!netmem) {
		netmem = page_pool_alloc_netmems(pool, gfp);
		if (unlikely(!netmem)) {
			pool->frag_page = 0;
			return 0;
		}

		pool->frag_page = netmem;

frag_reset:
		pool->frag_users = 1;
		*offset = 0;
		pool->frag_offset = size;
		page_pool_fragment_netmem(netmem, BIAS_MAX);
		return netmem;
	}

	pool->frag_users++;
	pool->frag_offset = *offset + size;
	return netmem;
}
EXPORT_SYMBOL(page_pool_alloc_frag_netmem);

struct page *page_pool_alloc_frag(struct page_pool *pool, unsigned int *offset,
				  unsigned int size, gfp_t gfp)
{
	return netmem_to_page(page_pool_alloc_frag_netmem(pool, offset, size,
							  gfp));
}
EXPORT_SYMBOL(page_pool_alloc_frag);

static void page_pool_empty_ring(struct page_pool *pool)
{
	netmem_ref netmem;

	/* Empty the stack of a pool bound to the fixed receive region */
	while (page_pool_has_rx_stack(pool)) {
		bool in_softirq = page_pool_producer_lock(pool);

		netmem = pool->rx_stack_n ?
			pool->rx_stack[--pool->rx_stack_n] : 0;
		page_pool_producer_unlock(pool, in_softirq);
		if (!netmem)
			break;
		page_pool_return_netmem(pool, netmem);
	}

	/* Empty recycle ring */
	while ((netmem = (__force netmem_ref)ptr_ring_consume_bh(&pool->ring))) {
		/* Verify the refcnt invariant of cached pages */
		if (!(netmem_ref_count(netmem) == 1))
			pr_crit("%s() page_pool refcnt %d violation\n",
				__func__, netmem_ref_count(netmem));

		page_pool_return_netmem(pool, netmem);
	}
}

static void __page_pool_destroy(struct page_pool *pool)
{
	if (pool->disconnect)
		pool->disconnect(pool);

	page_pool_unlist(pool);
	page_pool_uninit(pool);
	page_pool_rx_region_unbind(pool);

	kfree(pool);
}

static void page_pool_empty_alloc_cache_once(struct page_pool *pool)
{
	netmem_ref netmem;

	if (pool->destroy_cnt)
		return;

	/* Empty alloc cache, assume caller made sure this is
	 * no-longer in use, and page_pool_alloc_pages() cannot be
	 * call concurrently.
	 */
	while (pool->alloc.count) {
		netmem = pool->alloc.cache[--pool->alloc.count];
		page_pool_return_netmem(pool, netmem);
	}
}

static void page_pool_scrub(struct page_pool *pool)
{
	unsigned long id;
	void *ptr;

	page_pool_empty_alloc_cache_once(pool);
	if (!pool->destroy_cnt++ && pool->dma_map) {
		if (pool->dma_sync) {
			/* Disable page_pool_dma_sync_for_device() */
			pool->dma_sync = false;

			/* Make sure all concurrent returns that may see the old
			 * value of dma_sync (and thus perform a sync) have
			 * finished before doing the unmapping below. Skip the
			 * wait if the device doesn't actually need syncing, or
			 * if there are no outstanding mapped pages.
			 */
			if (dma_dev_need_sync(pool->p.dev) &&
			    !xa_empty(&pool->dma_mapped))
				synchronize_net();
		}

		/* No page ref, dma-unmap only. */
		xa_for_each(&pool->dma_mapped, id, ptr)
			__page_pool_unmap_netmem_dma(pool, page_to_netmem((struct page *)ptr));
	}

	/* No more consumers should exist, but producers could still
	 * be in-flight.
	 */
	page_pool_empty_ring(pool);
}

static int page_pool_release(struct page_pool *pool)
{
	bool in_softirq;
	int inflight;

	page_pool_scrub(pool);
	inflight = page_pool_inflight(pool, true);
	/* Acquire producer lock to make sure producers have exited. */
	in_softirq = page_pool_producer_lock(pool);
	page_pool_producer_unlock(pool, in_softirq);
	if (!inflight)
		__page_pool_destroy(pool);

	return inflight;
}

/*
 * Free the skbs every CPU deferred to every other: a list is otherwise
 * freed only by its CPU's receive processing, or once half full.
 */
static void page_pool_skb_defer_flush(void)
{
	int cpu, node;

	for_each_possible_cpu(cpu)
		for_each_node(node)
			skb_defer_node_flush(per_cpu_ptr(net_hotdata.skb_defer_nodes,
							 cpu) + node);
}

static void page_pool_release_retry(struct work_struct *wq)
{
	struct delayed_work *dwq = to_delayed_work(wq);
	struct page_pool *pool = container_of(dwq, typeof(*pool), release_dw);
	void *netdev;
	int inflight;

	inflight = page_pool_release(pool);
	/* In rare cases, a driver bug may cause inflight to go negative.
	 * Don't reschedule release if inflight is 0 or negative.
	 * - If 0, the page_pool has been destroyed
	 * - if negative, we will never recover
	 * in both cases no reschedule is necessary.
	 */
	if (inflight <= 0)
		return;

	/*
	 * A region pool's pages can sit in skbs on the defer list of a CPU
	 * that no longer receives (its device went away) and so never frees
	 * them: free every such list here, then look again.
	 */
	if (READ_ONCE(pool->rx_region)) {
		page_pool_skb_defer_flush();
		inflight = page_pool_release(pool);
		if (inflight <= 0)
			return;
	}

	/* Periodic warning for page pools the user can't see */
	netdev = READ_ONCE(pool->slow.netdev);
	if (time_after_eq(jiffies, pool->defer_warn) &&
	    (!netdev || netdev == NET_PTR_POISON)) {
		int sec = (s32)((u32)jiffies - (u32)pool->defer_start) / HZ;

		pr_warn("%s() stalled pool shutdown: id %u, %d inflight %d sec\n",
			__func__, pool->user.id, inflight, sec);
		pool->defer_warn = jiffies + DEFER_WARN_INTERVAL;
	}

	/* Still not ready to be disconnected, retry later */
	schedule_delayed_work(&pool->release_dw, DEFER_TIME);
}

void page_pool_use_xdp_mem(struct page_pool *pool, void (*disconnect)(void *),
			   const struct xdp_mem_info *mem)
{
	refcount_inc(&pool->user_cnt);
	pool->disconnect = disconnect;
	pool->xdp_mem_id = mem->id;
}

/**
 * page_pool_enable_direct_recycling() - mark page pool as owned by NAPI
 * @pool: page pool to modify
 * @napi: NAPI instance to associate the page pool with
 *
 * Associate a page pool with a NAPI instance for lockless page recycling.
 * This is useful when a new page pool has to be added to a NAPI instance
 * without disabling that NAPI instance, to mark the point at which control
 * path "hands over" the page pool to the NAPI instance. In most cases driver
 * can simply set the @napi field in struct page_pool_params, and does not
 * have to call this helper.
 *
 * The function is idempotent, but does not implement any refcounting.
 * Single page_pool_disable_direct_recycling() will disable recycling,
 * no matter how many times enable was called.
 */
void page_pool_enable_direct_recycling(struct page_pool *pool,
				       struct napi_struct *napi)
{
	if (READ_ONCE(pool->p.napi) == napi)
		return;
	WARN_ON(!napi || pool->p.napi);

	mutex_lock(&page_pools_lock);
	WRITE_ONCE(pool->p.napi, napi);
	mutex_unlock(&page_pools_lock);
}
EXPORT_SYMBOL(page_pool_enable_direct_recycling);

void page_pool_disable_direct_recycling(struct page_pool *pool)
{
	/* Disable direct recycling based on pool->cpuid.
	 * Paired with READ_ONCE() in page_pool_napi_local().
	 */
	WRITE_ONCE(pool->cpuid, -1);

	if (!pool->p.napi)
		return;

	napi_assert_will_not_race(pool->p.napi);

	mutex_lock(&page_pools_lock);
	WRITE_ONCE(pool->p.napi, NULL);
	mutex_unlock(&page_pools_lock);
}
EXPORT_SYMBOL(page_pool_disable_direct_recycling);

void page_pool_destroy(struct page_pool *pool)
{
	if (!pool)
		return;

	if (!page_pool_put(pool))
		return;

	page_pool_disable_direct_recycling(pool);
	page_pool_free_frag(pool);

	if (!page_pool_release(pool))
		return;

	page_pool_detached(pool);
	pool->defer_start = jiffies;
	pool->defer_warn  = jiffies + DEFER_WARN_INTERVAL;

	INIT_DELAYED_WORK(&pool->release_dw, page_pool_release_retry);
	schedule_delayed_work(&pool->release_dw, DEFER_TIME);
}
EXPORT_SYMBOL(page_pool_destroy);

/* Caller must provide appropriate safe context, e.g. NAPI. */
void page_pool_update_nid(struct page_pool *pool, int new_nid)
{
	netmem_ref netmem;

	trace_page_pool_update_nid(pool, new_nid);
	pool->p.nid = new_nid;

	/* Flush pool alloc cache, as refill will check NUMA node */
	while (pool->alloc.count) {
		netmem = pool->alloc.cache[--pool->alloc.count];
		page_pool_return_netmem(pool, netmem);
	}
}
EXPORT_SYMBOL(page_pool_update_nid);

bool net_mp_niov_set_dma_addr(struct net_iov *niov, dma_addr_t addr)
{
	return page_pool_set_dma_addr_netmem(net_iov_to_netmem(niov), addr);
}

/* Associate a niov with a page pool. Should follow with a matching
 * net_mp_niov_clear_page_pool()
 */
void net_mp_niov_set_page_pool(struct page_pool *pool, struct net_iov *niov)
{
	netmem_ref netmem = net_iov_to_netmem(niov);

	page_pool_set_pp_info(pool, netmem);

	pool->pages_state_hold_cnt++;
	trace_page_pool_state_hold(pool, netmem, pool->pages_state_hold_cnt);
}

/* Disassociate a niov from a page pool. Should only be used in the
 * ->release_netmem() path.
 */
void net_mp_niov_clear_page_pool(struct net_iov *niov)
{
	netmem_ref netmem = net_iov_to_netmem(niov);

	page_pool_clear_pp_info(netmem);
}

/*
 * The region's settings.  Each is a module parameter and a file in debugfs
 * page_pool_rx_region_cfg/ driven by the same kernel_param_ops, so both
 * paths change one variable and read back the same line.
 */
static int rx_region_dev_param_set(const char *val,
				   const struct kernel_param *kp)
{
	char buf[RX_REGION_DEV_LEN];

	if (strscpy(buf, val, sizeof(buf)) < 0)
		return -EINVAL;
	mutex_lock(&rx_region_mutex);
	strscpy(rx_region_dev, strim(buf), sizeof(rx_region_dev));
	mutex_unlock(&rx_region_mutex);
	return 0;
}

static int rx_region_dev_param_get(char *buffer, const struct kernel_param *kp)
{
	int len;

	mutex_lock(&rx_region_mutex);
	len = scnprintf(buffer, PAGE_SIZE, "%s\n", rx_region_dev);
	mutex_unlock(&rx_region_mutex);
	return len;
}

static const struct kernel_param_ops rx_region_dev_ops = {
	.set = rx_region_dev_param_set,
	.get = rx_region_dev_param_get,
};
module_param_cb(rx_region_dev, &rx_region_dev_ops, NULL, 0644);
MODULE_PARM_DESC(rx_region_dev,
		 "Network devices, comma-separated (parent bus device, else interface name), whose page pools share the fixed receive region; read when a pool is created");

/* 0 is auto: the size a carve computes (rx_region_size_now()). */
static int rx_region_pages_param_set(const char *val,
				     const struct kernel_param *kp)
{
	unsigned int pages;
	bool manual;
	int ret = page_pool_cfg_parse(val, &pages, &manual);

	if (ret)
		return ret;
	mutex_lock(&rx_region_mutex);
	WRITE_ONCE(rx_region_pages, pages);
	mutex_unlock(&rx_region_mutex);
	return 0;
}

/*
 * The size the next carve uses (rx_region_size_now()), not the carved
 * region's: a region is never resized, so a change applies at the next
 * carve; debugfs page_pool_rx_region shows the carved region.
 */
static int rx_region_pages_param_get(char *buffer,
				     const struct kernel_param *kp)
{
	return page_pool_cfg_format(buffer, rx_region_size_now(),
				    READ_ONCE(rx_region_pages) != 0);
}

static const struct kernel_param_ops rx_region_pages_ops = {
	.set = rx_region_pages_param_set,
	.get = rx_region_pages_param_get,
};
module_param_cb(rx_region_pages, &rx_region_pages_ops, NULL, 0644);
MODULE_PARM_DESC(rx_region_pages,
		 "Pages in the fixed receive region (0: auto, computed when carved); reads \"<pages> (auto|manual)\": the size the next carve uses, while debugfs page_pool_rx_region shows the carved region; a change takes effect once no pool is bound to the region");

static int rx_region_stack_param_set(const char *val,
				     const struct kernel_param *kp)
{
	unsigned int stack;
	bool manual;
	int ret = page_pool_cfg_parse(val, &stack, &manual);

	if (!ret)
		WRITE_ONCE(rx_region_stack, stack);
	return ret;
}

static int rx_region_stack_param_get(char *buffer,
				     const struct kernel_param *kp)
{
	unsigned int stack = READ_ONCE(rx_region_stack);

	return page_pool_cfg_format(buffer, stack, stack != 0);
}

static const struct kernel_param_ops rx_region_stack_ops = {
	.set = rx_region_stack_param_set,
	.get = rx_region_stack_param_get,
};
module_param_cb(rx_region_stack, &rx_region_stack_ops, NULL, 0644);
MODULE_PARM_DESC(rx_region_stack,
		 "Pages a pool bound to the fixed receive region keeps for reuse (0: auto, each pool's size, read back as \"0 (auto)\"); read when a pool is created");

/*
 * Only while no region exists: budgets are checked against the split.
 * Setting the value already set is no change, and succeeds either way.
 */
static int rx_region_read_pages_dbg_set(void *data, u64 val)
{
	int ret = 0;

	if (val > UINT_MAX || (val && val < PAGE_POOL_RX_REGION_READ_MIN))
		return -EINVAL;
	mutex_lock(&rx_region_mutex);
	if (rx_region)
		ret = val == READ_ONCE(rx_region_read_pages) ? 0 : -EBUSY;
	else
		WRITE_ONCE(rx_region_read_pages, val);
	mutex_unlock(&rx_region_mutex);
	return ret;
}

static int rx_region_read_pages_param_set(const char *val,
					  const struct kernel_param *kp)
{
	unsigned int pages;
	bool manual;
	int ret = page_pool_cfg_parse(val, &pages, &manual);

	if (ret)
		return ret;
	/* the line read back from a carved region writes back as no change */
	if (page_pool_rx_region_exists() &&
	    pages == page_pool_rx_region_read_pages() &&
	    manual == (READ_ONCE(rx_region_read_pages) != 0))
		return 0;
	return rx_region_read_pages_dbg_set(NULL, pages);
}

/* The read partition once carved, else the split requested. */
static int rx_region_read_pages_param_get(char *buffer,
					  const struct kernel_param *kp)
{
	unsigned long nr = page_pool_rx_region_exists() ?
			   page_pool_rx_region_read_pages() :
			   READ_ONCE(rx_region_read_pages);

	return page_pool_cfg_format(buffer, nr,
				    READ_ONCE(rx_region_read_pages) != 0);
}

static const struct kernel_param_ops rx_region_read_pages_ops = {
	.set = rx_region_read_pages_param_set,
	.get = rx_region_read_pages_param_get,
};
module_param_cb(rx_region_read_pages, &rx_region_read_pages_ops, NULL, 0644);
MODULE_PARM_DESC(rx_region_read_pages,
		 "Pages of the fixed receive region split off for reads (0: auto, reads and writes share it; else at least one maximum READ); -EBUSY while the region exists");

static ssize_t rx_region_cfg_read(struct file *file, char __user *ubuf,
				  size_t count, loff_t *ppos)
{
	struct kernel_param kp = { .ops = file->private_data };
	char *buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	ssize_t ret;

	if (!buf)
		return -ENOMEM;
	ret = kp.ops->get(buf, &kp);
	if (ret >= 0)
		ret = simple_read_from_buffer(ubuf, count, ppos, buf, ret);
	kfree(buf);
	return ret;
}

static ssize_t rx_region_cfg_write(struct file *file, const char __user *ubuf,
				   size_t count, loff_t *ppos)
{
	struct kernel_param kp = { .ops = file->private_data };
	char buf[RX_REGION_DEV_LEN];
	int ret;

	if (*ppos || count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = 0;
	ret = kp.ops->set(buf, &kp);
	return ret ?: count;
}

static const struct file_operations rx_region_cfg_fops = {
	.open = simple_open,
	.read = rx_region_cfg_read,
	.write = rx_region_cfg_write,
	.llseek = default_llseek,
};

static int rx_region_size_get(void *data, u64 *val)
{
	*val = rx_region_size_now();
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(rx_region_size_fops, rx_region_size_get, NULL,
			 "%llu\n");

static void __init rx_region_cfg_file(const char *name, struct dentry *d,
				      const struct kernel_param_ops *ops)
{
	debugfs_create_file(name, 0600, d, (void *)ops, &rx_region_cfg_fops);
}

static int __init page_pool_rx_region_debugfs_init(void)
{
	struct dentry *d = debugfs_create_dir("page_pool_rx_region_cfg", NULL);

	rx_region_cfg_file("dev", d, &rx_region_dev_ops);
	rx_region_cfg_file("pages", d, &rx_region_pages_ops);
	rx_region_cfg_file("stack", d, &rx_region_stack_ops);
	rx_region_cfg_file("read_pages", d, &rx_region_read_pages_ops);
	debugfs_create_file_unsafe("size", 0400, d, NULL,
				   &rx_region_size_fops);
	debugfs_create_ulong("demand", 0400, d, &rx_region_demand);
	debugfs_create_ulong("nic", 0400, d, &rx_region_nic);
	return 0;
}
late_initcall(page_pool_rx_region_debugfs_init);

#if IS_ENABLED(CONFIG_KUNIT)
/*
 * Through the ops both the parameter and the debugfs file use: set @name
 * to @val unless NULL, then read it back into @buf (PAGE_SIZE) unless NULL.
 */
int page_pool_rx_region_kunit_param(const char *name, const char *val,
				    char *buf)
{
	struct kernel_param kp = {};
	int ret;

	if (!strcmp(name, "dev"))
		kp.ops = &rx_region_dev_ops;
	else if (!strcmp(name, "pages"))
		kp.ops = &rx_region_pages_ops;
	else if (!strcmp(name, "stack"))
		kp.ops = &rx_region_stack_ops;
	else if (!strcmp(name, "read_pages"))
		kp.ops = &rx_region_read_pages_ops;
	else
		return -ENOENT;
	if (val) {
		ret = kp.ops->set(val, &kp);
		if (ret)
			return ret;
	}
	return buf ? kp.ops->get(buf, &kp) : 0;
}
EXPORT_SYMBOL_IF_KUNIT(page_pool_rx_region_kunit_param);
#endif
