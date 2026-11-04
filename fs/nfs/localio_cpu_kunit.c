// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the optimized LOCALIO completion CPU choice.
 *
 * A read or write completion is queued to a CPU of the completing CPU's
 * node that is online and a workqueue and domain housekeeping CPU.  The
 * completing CPU is usually the submitter's, so it is taken only when it
 * is the node's only such CPU.
 *
 * Also the classification of the data copies a LOCALIO O_DIRECT I/O
 * makes, from how nfs_dio_split() splits it.
 */
#include <kunit/test.h>
#include <linux/cpumask.h>
#include <linux/bvec.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/nfs_dio.h>
#include <linux/nfs_fs.h>
#include <linux/nfs_page.h>
#include <linux/shmem_fs.h>
#include <linux/uio.h>

#include "internal.h"
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

/*
 * Copy classification.  Each case splits a payload with nfs_dio_split(),
 * as nfs_local_iters_init() does, and accounts every segment as fully
 * transferred, the way nfs_local_count_seg() does at its completion.
 * Offsets and lengths scale with PAGE_SIZE, since a write's buffered ends
 * are cut on page boundaries.
 */
struct localio_copy_case {
	unsigned int	rw;		/* ITER_SOURCE or ITER_DEST */
	u32		mem_align;
	u32		offset_align;
	u32		seg_boundary;
	unsigned int	min_middle_bytes;
	bool		stable_writes;
	loff_t		pos;
	unsigned int	bv_offset;
	size_t		len;
};

/* What a fully transferred I/O adds to the counters. */
struct localio_copy_count {
	size_t		direct;
	size_t		copied[NFS_LOCAL_COPY_NR];
	unsigned int	nsegs;
};

static void localio_copy_fput(void *data)
{
	fput(data);
}

/*
 * A real file, so that nfs_dio_split() finds the mapping it reads for a
 * write's cut: an unnamed shmem file, which does no DONTCACHE and whose
 * smallest folio is a page.
 */
static struct file *localio_copy_file(struct kunit *test)
{
	struct file *file;

	file = shmem_kernel_file_setup("localio_cpu_kunit", 0, EMPTY_VMA_FLAGS);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, file);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, localio_copy_fput, file),
			0);
	if (mapping_min_folio_nrbytes(file->f_mapping) != PAGE_SIZE)
		kunit_skip(test, "smallest folio is not a page");
	return file;
}

static void localio_copy_split(struct kunit *test,
			       const struct localio_copy_case *c,
			       struct bio_vec *bvec, unsigned int nvecs,
			       struct localio_copy_count *count)
{
	struct nfs_dio_policy policy = {
		.mem_align = c->mem_align,
		.offset_align = c->offset_align,
		.seg_boundary = c->seg_boundary,
		.min_middle_bytes = c->min_middle_bytes,
	};
	struct file *file = localio_copy_file(test);
	struct nfs_dio_split split;
	size_t len = 0;
	unsigned int i;

	nfs_dio_split(file, &policy, c->rw, bvec, nvecs, c->pos, c->len, 0,
		      &split);
	memset(count, 0, sizeof(*count));
	count->nsegs = split.nsegs;
	for (i = 0; i < split.nsegs; i++) {
		const struct nfs_dio_seg *seg = &split.segs[i];
		size_t bytes = iov_iter_count(&seg->iter);
		enum nfs_local_copy_cause cause;

		cause = nfs_local_seg_copy_cause(&split, seg, c->stable_writes);
		if (cause == NFS_LOCAL_COPY_NONE)
			count->direct += bytes;
		else
			count->copied[cause] += bytes;
		len += bytes;
	}
	KUNIT_EXPECT_EQ(test, len, c->len);
}

static void localio_copy_one(struct kunit *test,
			     const struct localio_copy_case *c,
			     struct localio_copy_count *count)
{
	struct bio_vec bvec;

	bvec_set_page(&bvec, NULL, c->len, c->bv_offset);
	localio_copy_split(test, c, &bvec, 1, count);
}

/* Expect @c to copy @copied bytes for @cause and send the rest direct. */
static void localio_copy_expect(struct kunit *test,
				const struct localio_copy_case *c,
				enum nfs_local_copy_cause cause, size_t copied,
				unsigned int nsegs)
{
	struct localio_copy_count count;
	unsigned int i;

	localio_copy_one(test, c, &count);
	KUNIT_EXPECT_EQ(test, count.nsegs, nsegs);
	KUNIT_EXPECT_EQ(test, count.direct, c->len - copied);
	for (i = NFS_LOCAL_COPY_NONE + 1; i < NFS_LOCAL_COPY_NR; i++)
		KUNIT_EXPECT_EQ_MSG(test, count.copied[i],
				    i == cause ? copied : 0, "cause %u", i);
}

#define LOCALIO_COPY_CASE(dir)						\
	.rw = (dir), .mem_align = 4, .offset_align = PAGE_SIZE,		\
	.seg_boundary = NFS_DIO_SEG_BOUNDARY_NONE

/* An aligned direct write or read copies nothing. */
static void localio_copy_direct_test(struct kunit *test)
{
	struct localio_copy_case c = {
		LOCALIO_COPY_CASE(ITER_SOURCE),
		.pos = PAGE_SIZE, .len = 4 * PAGE_SIZE,
	};

	localio_copy_expect(test, &c, NFS_LOCAL_COPY_NONE, 0, 1);
	c.rw = ITER_DEST;
	localio_copy_expect(test, &c, NFS_LOCAL_COPY_NONE, 0, 1);
}

/* A direct segment the backing fs bounces is copied. */
static void localio_copy_stable_writes_test(struct kunit *test)
{
	struct localio_copy_case c = {
		LOCALIO_COPY_CASE(ITER_SOURCE),
		.stable_writes = true,
		.pos = 0, .len = 4 * PAGE_SIZE,
	};
	struct localio_copy_count count;

	localio_copy_expect(test, &c, NFS_LOCAL_COPY_STABLE_WRITES,
			    4 * PAGE_SIZE, 1);

	/* Its buffered tails keep their own cause. */
	c.pos = 1000;
	c.len = 3 * PAGE_SIZE;
	localio_copy_one(test, &c, &count);
	KUNIT_EXPECT_EQ(test, count.direct, 0);
	KUNIT_EXPECT_EQ(test, count.copied[NFS_LOCAL_COPY_MERGE_TAIL],
			PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, count.copied[NFS_LOCAL_COPY_STABLE_WRITES],
			2 * PAGE_SIZE);

	/* A buffered I/O is not a stable_writes copy. */
	c.pos = 0;
	c.len = 100;
	localio_copy_expect(test, &c, NFS_LOCAL_COPY_SMALL, 100, 1);
}

/* A direct split copies its buffered prefix and suffix. */
static void localio_copy_merge_tail_test(struct kunit *test)
{
	struct localio_copy_case c = {
		LOCALIO_COPY_CASE(ITER_SOURCE),
		.pos = 1000, .len = 3 * PAGE_SIZE,
	};

	/* prefix PAGE_SIZE - 1000, middle 2 pages, suffix 1000 */
	localio_copy_expect(test, &c, NFS_LOCAL_COPY_MERGE_TAIL, PAGE_SIZE, 3);

	/* prefix only */
	c.len = 3 * PAGE_SIZE - 1000;
	localio_copy_expect(test, &c, NFS_LOCAL_COPY_MERGE_TAIL,
			    PAGE_SIZE - 1000, 2);

	/* suffix only */
	c.pos = 0;
	c.len = 2 * PAGE_SIZE + 1000;
	localio_copy_expect(test, &c, NFS_LOCAL_COPY_MERGE_TAIL, 1000, 2);
}

/* A write shorter than the alignment is all buffered. */
static void localio_copy_small_test(struct kunit *test)
{
	struct localio_copy_case c = {
		LOCALIO_COPY_CASE(ITER_SOURCE),
		.pos = 0, .len = 100,
	};

	localio_copy_expect(test, &c, NFS_LOCAL_COPY_SMALL, 100, 1);
}

/* No advertised alignment: all buffered. */
static void localio_copy_no_alignment_test(struct kunit *test)
{
	struct localio_copy_case c = {
		LOCALIO_COPY_CASE(ITER_SOURCE),
		.pos = 0, .len = 4 * PAGE_SIZE,
	};

	c.mem_align = 0;

	localio_copy_expect(test, &c, NFS_LOCAL_COPY_NO_ALIGNMENT,
			    4 * PAGE_SIZE, 1);
}

/* No aligned middle, or one below min_middle_bytes: all buffered. */
static void localio_copy_no_middle_test(struct kunit *test)
{
	struct localio_copy_case c = {
		LOCALIO_COPY_CASE(ITER_SOURCE),
		.pos = 1000, .len = PAGE_SIZE,
	};

	localio_copy_expect(test, &c, NFS_LOCAL_COPY_NO_MIDDLE, PAGE_SIZE, 1);

	/* a two-page middle, under a four-page minimum */
	c.len = 3 * PAGE_SIZE;
	c.min_middle_bytes = 4 * PAGE_SIZE;
	localio_copy_expect(test, &c, NFS_LOCAL_COPY_NO_MIDDLE,
			    3 * PAGE_SIZE, 1);
}

/* Misaligned payload memory: all buffered. */
static void localio_copy_mem_misaligned_test(struct kunit *test)
{
	struct localio_copy_case c = {
		LOCALIO_COPY_CASE(ITER_SOURCE),
		.pos = 0, .bv_offset = 1, .len = 2 * PAGE_SIZE,
	};

	c.mem_align = 512;

	localio_copy_expect(test, &c, NFS_LOCAL_COPY_MEM_MISALIGNED,
			    2 * PAGE_SIZE, 1);
}

/*
 * A fragment gap off the device's segment boundary (a PRP-only device)
 * that is not logical-block aligned: all buffered.
 */
static void localio_copy_prp_gap_test(struct kunit *test)
{
	struct localio_copy_case c = {
		LOCALIO_COPY_CASE(ITER_SOURCE),
		.pos = 0, .len = 2 * PAGE_SIZE,
	};
	struct localio_copy_count count;
	struct bio_vec bvec[2];

	c.seg_boundary = PAGE_SIZE;
	bvec_set_page(&bvec[0], NULL, PAGE_SIZE / 2, 0);
	bvec_set_page(&bvec[1], NULL, PAGE_SIZE + PAGE_SIZE / 2, 0);
	localio_copy_split(test, &c, bvec, 2, &count);
	KUNIT_EXPECT_EQ(test, count.direct, 0);
	KUNIT_EXPECT_EQ(test, count.copied[NFS_LOCAL_COPY_PRP_GAP],
			2 * PAGE_SIZE);

	/* The same gap is fine on a device that takes gaps anywhere. */
	c.seg_boundary = NFS_DIO_SEG_BOUNDARY_NONE;
	localio_copy_split(test, &c, bvec, 2, &count);
	KUNIT_EXPECT_EQ(test, count.direct, 2 * PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, count.copied[NFS_LOCAL_COPY_PRP_GAP], 0);
}

/* Reads are classified the same way. */
static void localio_copy_read_test(struct kunit *test)
{
	struct localio_copy_case c = {
		LOCALIO_COPY_CASE(ITER_DEST),
		.pos = 1000, .len = 3 * PAGE_SIZE,
	};

	localio_copy_expect(test, &c, NFS_LOCAL_COPY_MERGE_TAIL, PAGE_SIZE, 3);

	c.pos = 0;
	c.len = 100;
	localio_copy_expect(test, &c, NFS_LOCAL_COPY_SMALL, 100, 1);

	c.len = 2 * PAGE_SIZE;
	c.bv_offset = 1;
	c.mem_align = 512;
	localio_copy_expect(test, &c, NFS_LOCAL_COPY_MEM_MISALIGNED,
			    2 * PAGE_SIZE, 1);
}

/*
 * A read's misaligned suffix is cut at the offset alignment, a write's on
 * a page boundary, so a read copies less of the same tail.
 */
static void localio_copy_read_suffix_test(struct kunit *test)
{
	struct localio_copy_case c = {
		LOCALIO_COPY_CASE(ITER_DEST),
		.pos = 0, .len = 2 * PAGE_SIZE + 700,
	};

	c.offset_align = 512;

	localio_copy_expect(test, &c, NFS_LOCAL_COPY_MERGE_TAIL, 700 - 512, 2);
	c.rw = ITER_SOURCE;
	localio_copy_expect(test, &c, NFS_LOCAL_COPY_MERGE_TAIL, 700, 2);
}

/*
 * Fragmented kernel bvecs.  A direct write of loaned receive pages reaches
 * LOCALIO as one header whose requests do not tile their pages; each case
 * builds those requests, describes them with nfs_local_bvec_from_reqs() as
 * nfs_local_iters_init() does, and splits the result.  Page and half-page
 * extents are written in PAGE_SIZE; the 512-byte alignment and the short
 * sub-page offsets and lengths stay fixed, so each joint keeps its meaning
 * on 4, 16 and 64 KiB pages.
 */
#define HALF_PAGE	(PAGE_SIZE / 2)

struct localio_frag {
	unsigned int	page;		/* which fake page */
	unsigned int	offset;
	unsigned int	len;
};

static unsigned int localio_frags_bvec(struct kunit *test,
				       const struct localio_frag *frags,
				       unsigned int nfrags, size_t skip,
				       size_t *len, struct bio_vec *bvec)
{
	struct page *pages = kunit_kcalloc(test, 8, sizeof(*pages),
					   GFP_KERNEL);
	struct nfs_page *reqs = kunit_kcalloc(test, nfrags, sizeof(*reqs),
					      GFP_KERNEL);
	LIST_HEAD(head);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, pages);
	KUNIT_ASSERT_NOT_NULL(test, reqs);
	for (i = 0; i < nfrags; i++) {
		reqs[i].wb_page = &pages[frags[i].page];
		reqs[i].wb_pgbase = frags[i].offset;
		reqs[i].wb_bytes = frags[i].len;
		list_add_tail(&reqs[i].wb_list, &head);
	}
	return nfs_local_bvec_from_reqs(bvec, nfrags, &head, skip, len);
}

/* Split @frags under @c and expect @copied bytes for @cause. */
static void localio_frags_expect(struct kunit *test,
				 struct localio_copy_case *c,
				 const struct localio_frag *frags,
				 unsigned int nfrags,
				 enum nfs_local_copy_cause cause,
				 size_t copied, unsigned int nsegs)
{
	struct bio_vec bvec[8];
	struct localio_copy_count count;
	size_t len = SIZE_MAX;
	unsigned int nvecs, i;

	KUNIT_ASSERT_LE(test, nfrags, ARRAY_SIZE(bvec));
	nvecs = localio_frags_bvec(test, frags, nfrags, 0, &len, bvec);
	c->len = len;
	localio_copy_split(test, c, bvec, nvecs, &count);
	KUNIT_EXPECT_EQ(test, count.nsegs, nsegs);
	KUNIT_EXPECT_EQ(test, count.direct, c->len - copied);
	for (i = NFS_LOCAL_COPY_NONE + 1; i < NFS_LOCAL_COPY_NR; i++)
		KUNIT_EXPECT_EQ_MSG(test, count.copied[i],
				    i == cause ? copied : 0, "cause %u", i);
}

#define LOCALIO_FRAG_CASE						\
	.rw = ITER_SOURCE, .mem_align = 4, .offset_align = 512

/*
 * Sub-page fragments whose ends and joints meet the policy go direct as one
 * segment and copy nothing.  An unknown segment boundary is taken to be a
 * page, so every mid-page joint must fall on a 512-byte file offset.
 */
static void localio_frag_direct_test(struct kunit *test)
{
	static const struct localio_frag frags[] = {
		{ 0, 0, HALF_PAGE }, { 1, 0, HALF_PAGE },
		{ 2, 512, PAGE_SIZE - 512 }, { 3, 0, PAGE_SIZE / 4 },
	};
	struct localio_copy_case c = {
		LOCALIO_FRAG_CASE, .pos = 2 * PAGE_SIZE,
	};
	struct bio_vec bvec[4];
	size_t len = SIZE_MAX;

	KUNIT_EXPECT_EQ(test, localio_frags_bvec(test, frags, 4, 0, &len, bvec),
			4);
	KUNIT_EXPECT_EQ(test, len, 2 * PAGE_SIZE + PAGE_SIZE / 4 - 512);
	localio_frags_expect(test, &c, frags, 4, NFS_LOCAL_COPY_NONE, 0, 1);

	/* A device that takes gaps anywhere admits them too. */
	c.seg_boundary = NFS_DIO_SEG_BOUNDARY_NONE;
	localio_frags_expect(test, &c, frags, 4, NFS_LOCAL_COPY_NONE, 0, 1);
}

/* Adjacent requests in one page are one fragment, not a false joint. */
static void localio_frag_merge_test(struct kunit *test)
{
	static const struct localio_frag frags[] = {
		{ 0, 0, 1000 }, { 0, 1000, PAGE_SIZE - 1000 },
		{ 1, 0, PAGE_SIZE },
	};
	struct localio_copy_case c = { LOCALIO_FRAG_CASE, .pos = 0 };
	struct bio_vec bvec[3];
	size_t len = SIZE_MAX;

	c.offset_align = PAGE_SIZE;

	KUNIT_EXPECT_EQ(test, localio_frags_bvec(test, frags, 3, 0, &len, bvec),
			2);
	KUNIT_EXPECT_EQ(test, bvec[0].bv_len, PAGE_SIZE);
	localio_frags_expect(test, &c, frags, 3, NFS_LOCAL_COPY_NONE, 0, 1);
}

/* A fragment off the memory alignment makes the whole write a counted copy. */
static void localio_frag_mem_misaligned_test(struct kunit *test)
{
	static const struct localio_frag frags[] = {
		{ 0, 0, HALF_PAGE }, { 1, 2, HALF_PAGE },
		{ 2, 0, PAGE_SIZE },
	};
	struct localio_copy_case c = { LOCALIO_FRAG_CASE, .pos = 0 };

	localio_frags_expect(test, &c, frags, 3,
			     NFS_LOCAL_COPY_MEM_MISALIGNED, 2 * PAGE_SIZE, 1);
}

/*
 * A mid-page joint off the offset alignment would let the block layer split
 * the write off a logical block, so the write is a counted copy; with no
 * segment boundary the same fragments go direct.
 */
static void localio_frag_gap_test(struct kunit *test)
{
	static const struct localio_frag frags[] = {
		{ 0, 0, 1500 }, { 1, 0, PAGE_SIZE }, { 2, 0, PAGE_SIZE - 1500 },
	};
	struct localio_copy_case c = { LOCALIO_FRAG_CASE, .pos = 0 };

	localio_frags_expect(test, &c, frags, 3, NFS_LOCAL_COPY_PRP_GAP,
			     2 * PAGE_SIZE, 1);
	c.seg_boundary = NFS_DIO_SEG_BOUNDARY_NONE;
	localio_frags_expect(test, &c, frags, 3, NFS_LOCAL_COPY_NONE, 0, 1);
}

/*
 * A misaligned file range over fragments: the page-aligned middle goes
 * direct and only the buffered ends are copied, whatever the fragments
 * outside the middle look like.
 */
static void localio_frag_mixed_test(struct kunit *test)
{
	static const struct localio_frag frags[] = {
		{ 0, 1000, PAGE_SIZE - 1000 }, { 1, 0, PAGE_SIZE },
		{ 2, 0, PAGE_SIZE }, { 3, 0, 1000 },
	};
	static const struct localio_frag bad_middle[] = {
		{ 0, 1000, PAGE_SIZE - 1000 }, { 1, 0, 2000 },
		{ 2, 0, PAGE_SIZE - 1904 }, { 3, 0, PAGE_SIZE - 96 },
		{ 4, 0, 1000 },
	};
	struct localio_copy_case c = { LOCALIO_FRAG_CASE, .pos = 1000 };

	/* prefix PAGE_SIZE - 1000, middle 2 pages, suffix 1000 */
	localio_frags_expect(test, &c, frags, 4, NFS_LOCAL_COPY_MERGE_TAIL,
			     PAGE_SIZE, 3);

	/* A gap inside the middle makes the whole write one counted copy. */
	localio_frags_expect(test, &c, bad_middle, 5, NFS_LOCAL_COPY_PRP_GAP,
			     3 * PAGE_SIZE, 1);
}

/* A restarted write describes only what is left of its fragments. */
static void localio_frag_restart_test(struct kunit *test)
{
	static const struct localio_frag frags[] = {
		{ 0, 0, HALF_PAGE }, { 1, 0, HALF_PAGE },
		{ 2, 0, PAGE_SIZE / 4 }, { 3, 512, 512 },
	};
	struct bio_vec bvec[4];
	size_t len = PAGE_SIZE / 4;

	/* Resume a quarter page into the second fragment. */
	KUNIT_EXPECT_EQ(test, localio_frags_bvec(test, frags, 4,
						 3 * PAGE_SIZE / 4, &len,
						 bvec), 1);
	KUNIT_EXPECT_EQ(test, len, PAGE_SIZE / 4);
	KUNIT_EXPECT_EQ(test, bvec[0].bv_offset, PAGE_SIZE / 4);
	KUNIT_EXPECT_EQ(test, bvec[0].bv_len, PAGE_SIZE / 4);

	/* Resume 404 bytes into the third. */
	len = SIZE_MAX;
	KUNIT_EXPECT_EQ(test, localio_frags_bvec(test, frags, 4,
						 PAGE_SIZE + 404, &len,
						 bvec), 2);
	KUNIT_EXPECT_EQ(test, len, PAGE_SIZE / 4 - 404 + 512);
	KUNIT_EXPECT_EQ(test, bvec[0].bv_offset, 404);
	KUNIT_EXPECT_EQ(test, bvec[1].bv_offset, 512);
}

/* A request of a direct write: @len bytes at file offset @pos. */
struct localio_req {
	unsigned int	page;		/* which fake page */
	unsigned int	offset;
	unsigned int	len;
	loff_t		pos;
};

static struct nfs_page *localio_reqs(struct kunit *test,
				     const struct localio_req *r,
				     unsigned int n, struct list_head *head)
{
	struct page *pages = kunit_kcalloc(test, 8, sizeof(*pages),
					   GFP_KERNEL);
	struct nfs_page *reqs = kunit_kcalloc(test, n, sizeof(*reqs),
					      GFP_KERNEL);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, pages);
	KUNIT_ASSERT_NOT_NULL(test, reqs);
	for (i = 0; i < n; i++) {
		reqs[i].wb_page = &pages[r[i].page];
		reqs[i].wb_pgbase = r[i].offset;
		reqs[i].wb_bytes = r[i].len;
		reqs[i].wb_index = r[i].pos >> PAGE_SHIFT;
		reqs[i].wb_offset = r[i].pos & ~PAGE_MASK;
		list_add_tail(&reqs[i].wb_list, head);
	}
	return reqs;
}

static size_t localio_list_bytes(const struct list_head *head)
{
	const struct nfs_page *req;
	size_t bytes = 0;

	list_for_each_entry(req, head, wb_list)
		bytes += req->wb_bytes;
	return bytes;
}

/*
 * The RPC fallback issues a fragmented list run by run: each run must carry
 * exactly its own requests, in order, and its own bytes, and the rest must
 * keep every later request.
 */
static void localio_pgio_cut_run_test(struct kunit *test)
{
	static const struct localio_req r[] = {
		/* ends mid-page */
		{ 0, 0, HALF_PAGE, 0 },
		/* starts a page */
		{ 1, 0, PAGE_SIZE, HALF_PAGE },
		/* contiguous with the last */
		{ 2, 0, PAGE_SIZE, HALF_PAGE + PAGE_SIZE },
		/* starts mid-page */
		{ 3, 512, 512, HALF_PAGE + 2 * PAGE_SIZE },
		/* follows it in the page */
		{ 3, 1024, 1536, HALF_PAGE + 2 * PAGE_SIZE + 512 },
	};
	static const unsigned int runs[][2] = { { 0, 1 }, { 1, 3 }, { 3, 5 } };
	static const size_t bytes[] = { HALF_PAGE, 2 * PAGE_SIZE, 2048 };
	struct nfs_page *reqs;
	LIST_HEAD(list);
	unsigned int i, k;

	reqs = localio_reqs(test, r, ARRAY_SIZE(r), &list);
	for (k = 0; k < ARRAY_SIZE(runs); k++) {
		LIST_HEAD(rest);
		struct nfs_page *req;

		KUNIT_ASSERT_FALSE(test, list_empty(&list));
		KUNIT_EXPECT_EQ(test, nfs_pgio_cut_run(&list, &rest), bytes[k]);
		KUNIT_EXPECT_EQ(test, localio_list_bytes(&list), bytes[k]);
		i = runs[k][0];
		list_for_each_entry(req, &list, wb_list)
			KUNIT_EXPECT_PTR_EQ(test, req, &reqs[i++]);
		KUNIT_EXPECT_EQ(test, i, runs[k][1]);
		list_for_each_entry(req, &rest, wb_list)
			KUNIT_EXPECT_PTR_EQ(test, req, &reqs[i++]);
		KUNIT_EXPECT_EQ(test, i, ARRAY_SIZE(r));
		INIT_LIST_HEAD(&list);
		list_splice_init(&rest, &list);
	}
	KUNIT_EXPECT_TRUE(test, list_empty(&list));
}

/* Only page-based requests adjacent in the file join; the count is capped. */
static void localio_frag_join_gate_test(struct kunit *test)
{
	static const struct localio_req r[] = {
		{ 0, 0, HALF_PAGE, 0 },
		/* adjacent, other page */
		{ 1, 0, PAGE_SIZE, HALF_PAGE },
		/* hole in the file */
		{ 2, 512, 512, 2 * PAGE_SIZE },
	};
	struct nfs_page *reqs;
	LIST_HEAD(list);

	reqs = localio_reqs(test, r, ARRAY_SIZE(r), &list);
	KUNIT_EXPECT_FALSE(test, nfs_page_is_contiguous(&reqs[0], &reqs[1]));
	KUNIT_EXPECT_TRUE(test, nfs_page_frag_adjacent(&reqs[0], &reqs[1]));
	KUNIT_EXPECT_FALSE(test, nfs_page_frag_adjacent(&reqs[1], &reqs[2]));
	set_bit(PG_FOLIO, &reqs[1].wb_flags);
	KUNIT_EXPECT_FALSE(test, nfs_page_frag_adjacent(&reqs[0], &reqs[1]));
	clear_bit(PG_FOLIO, &reqs[1].wb_flags);

	KUNIT_EXPECT_TRUE(test, nfs_pgio_frag_may_add(false, UINT_MAX));
	KUNIT_EXPECT_TRUE(test, nfs_pgio_frag_may_add(true, 0));
	KUNIT_EXPECT_TRUE(test, nfs_pgio_frag_may_add(true,
					NFS_PGIO_FRAG_MAX_REQS - 1));
	KUNIT_EXPECT_FALSE(test, nfs_pgio_frag_may_add(true,
					NFS_PGIO_FRAG_MAX_REQS));
	/* The page array this allows is no larger than an ordinary one's. */
	KUNIT_EXPECT_LE(test, NFS_PGIO_FRAG_MAX_REQS * sizeof(struct page *),
			PAGE_SIZE);
}

/*
 * The whole request is admitted or refused before pg_test sees it, so a full
 * fragmented header refuses even a request contiguous with its last one.
 */
static void localio_frag_admit_test(struct kunit *test)
{
	static const struct localio_req r[] = {
		{ 0, 0, HALF_PAGE, 0 },
		/* contiguous */
		{ 0, HALF_PAGE, HALF_PAGE, HALF_PAGE },
		/* adjacent fragment */
		{ 1, 512, 512, PAGE_SIZE },
		/* hole in the file */
		{ 2, 0, 512, 2 * PAGE_SIZE },
	};
	const unsigned int max = NFS_PGIO_FRAG_MAX_REQS;
	struct nfs_page *reqs;
	LIST_HEAD(list);
	bool frag;

	reqs = localio_reqs(test, r, ARRAY_SIZE(r), &list);

	frag = false;
	KUNIT_EXPECT_TRUE(test, nfs_pgio_frag_admit(&reqs[0], &reqs[1], false,
						    false, UINT_MAX, &frag));
	KUNIT_EXPECT_FALSE(test, frag);
	KUNIT_EXPECT_TRUE(test, nfs_pgio_frag_admit(&reqs[0], &reqs[1], true,
						    true, max - 1, &frag));
	KUNIT_EXPECT_FALSE(test, nfs_pgio_frag_admit(&reqs[0], &reqs[1], true,
						     true, max, &frag));
	KUNIT_EXPECT_FALSE(test, frag);

	KUNIT_EXPECT_FALSE(test, nfs_pgio_frag_admit(&reqs[1], &reqs[2], false,
						     false, 0, &frag));
	KUNIT_EXPECT_FALSE(test, frag);
	KUNIT_EXPECT_TRUE(test, nfs_pgio_frag_admit(&reqs[1], &reqs[2], true,
						    false, max - 1, &frag));
	KUNIT_EXPECT_TRUE(test, frag);
	frag = false;
	KUNIT_EXPECT_FALSE(test, nfs_pgio_frag_admit(&reqs[1], &reqs[2], true,
						     false, max, &frag));
	frag = false;
	KUNIT_EXPECT_FALSE(test, nfs_pgio_frag_admit(&reqs[2], &reqs[3], true,
						     false, 0, &frag));
	KUNIT_EXPECT_FALSE(test, frag);
}

/* A restarted write's skip comes from the header, past its first request. */
static void localio_frag_skip_test(struct kunit *test)
{
	static const struct localio_req r[] = {
		{ 0, 0, HALF_PAGE, 2 * PAGE_SIZE },
		{ 1, 0, HALF_PAGE, 2 * PAGE_SIZE + HALF_PAGE },
		{ 2, 0, PAGE_SIZE / 4, 3 * PAGE_SIZE },
	};
	const size_t total = PAGE_SIZE + PAGE_SIZE / 4;
	const size_t done = HALF_PAGE + 452;	/* a short write's bytes */
	struct nfs_pgio_header *hdr = kunit_kzalloc(test, sizeof(*hdr),
						    GFP_KERNEL);
	struct bio_vec bvec[3];
	struct nfs_page *reqs;
	size_t len;

	KUNIT_ASSERT_NOT_NULL(test, hdr);
	INIT_LIST_HEAD(&hdr->pages);
	reqs = localio_reqs(test, r, ARRAY_SIZE(r), &hdr->pages);
	hdr->req = &reqs[0];
	hdr->args.offset = 2 * PAGE_SIZE;
	KUNIT_EXPECT_EQ(test, nfs_local_frag_skip(hdr), 0);

	hdr->args.offset = 2 * PAGE_SIZE + done;
	hdr->args.count = total - done;
	KUNIT_EXPECT_EQ(test, nfs_local_frag_skip(hdr), done);
	len = hdr->args.count;
	KUNIT_EXPECT_EQ(test, nfs_local_bvec_from_reqs(bvec, 3, &hdr->pages,
						nfs_local_frag_skip(hdr),
						&len), 2);
	KUNIT_EXPECT_EQ(test, len, hdr->args.count);
	KUNIT_EXPECT_PTR_EQ(test, bvec[0].bv_page, reqs[1].wb_page);
	KUNIT_EXPECT_EQ(test, bvec[0].bv_offset, 452);
	KUNIT_EXPECT_EQ(test, bvec[0].bv_len, HALF_PAGE - 452);
	KUNIT_EXPECT_EQ(test, bvec[1].bv_len, PAGE_SIZE / 4);
}

#undef HALF_PAGE

static struct kunit_case localio_copy_test_cases[] = {
	KUNIT_CASE(localio_copy_direct_test),
	KUNIT_CASE(localio_copy_stable_writes_test),
	KUNIT_CASE(localio_copy_merge_tail_test),
	KUNIT_CASE(localio_copy_small_test),
	KUNIT_CASE(localio_copy_no_alignment_test),
	KUNIT_CASE(localio_copy_no_middle_test),
	KUNIT_CASE(localio_copy_mem_misaligned_test),
	KUNIT_CASE(localio_copy_prp_gap_test),
	KUNIT_CASE(localio_copy_read_test),
	KUNIT_CASE(localio_copy_read_suffix_test),
	KUNIT_CASE(localio_frag_direct_test),
	KUNIT_CASE(localio_frag_merge_test),
	KUNIT_CASE(localio_frag_mem_misaligned_test),
	KUNIT_CASE(localio_frag_gap_test),
	KUNIT_CASE(localio_frag_mixed_test),
	KUNIT_CASE(localio_frag_restart_test),
	KUNIT_CASE(localio_pgio_cut_run_test),
	KUNIT_CASE(localio_frag_join_gate_test),
	KUNIT_CASE(localio_frag_admit_test),
	KUNIT_CASE(localio_frag_skip_test),
	{}
};

static struct kunit_suite localio_copy_test_suite = {
	.name = "nfs-localio-copies",
	.test_cases = localio_copy_test_cases,
};

kunit_test_suites(&localio_cpu_test_suite, &localio_copy_test_suite);

MODULE_DESCRIPTION("KUnit tests for the LOCALIO completion CPU choice and copy accounting");
MODULE_LICENSE("GPL");
