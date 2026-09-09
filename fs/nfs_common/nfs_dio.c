// SPDX-License-Identifier: GPL-2.0
/*
 * Direct I/O helpers shared by NFSD and the NFS client's LOCALIO path.
 *
 * Both hand a write they received as a page vector to a local file system.
 * Where the file system advertises direct I/O alignment, the aligned middle
 * of a misaligned write goes to the device as O_DIRECT and only the
 * misaligned start and end are buffered.  Those buffered pages may be marked
 * DONTCACHE so they are dropped once written back, but each is shared with
 * the neighbouring write and must not be dropped before both have written
 * it.  The page cache keeps it for the write that completes it; that write
 * marks it here once complete.  This is that logic, once, for both callers.
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/backing-dev.h>
#include <linux/bvec.h>
#include <linux/uio.h>
#include <linux/nfs_dio.h>

static void
nfs_dio_seg_init(struct nfs_dio_seg *seg, unsigned int direction,
		 struct bio_vec *bvec, unsigned int nvecs, unsigned long total,
		 size_t start, size_t len, int iocb_flags)
{
	iov_iter_bvec(&seg->iter, direction, bvec, nvecs, total);
	if (start)
		iov_iter_advance(&seg->iter, start);
	iov_iter_truncate(&seg->iter, len);
	seg->flags = iocb_flags;
	seg->claim = false;
}

/*
 * A vector built from a copied RPC receive buffer or from a pinned O_DIRECT
 * user buffer is contiguous after its first entry, so only its ends can be
 * misaligned.  A vector of loaned receive pages is not one contiguous
 * page-tiled run: a fragment may end mid-page, or its successor may start at
 * a non-zero page offset.  Two properties must hold before such an iterator
 * may be issued as direct I/O:
 *
 * - every fragment's offset and length must satisfy the device's memory
 *   alignment (the iov_iter_alignment() contract vs stx_dio_mem_align);
 * - every interior discontinuity must land on an @offset_align multiple
 *   (stx_dio_offset_align is never smaller than the logical block size).
 *   On a queue with a virtual boundary (e.g. NVMe) each discontinuity
 *   forces a bio split at that byte position, and bio_split_io_at() only
 *   splits on a logical-block boundary: a discontinuity at a payload
 *   position that is not a logical-block multiple strands a sub-sector
 *   residue with no valid split point, and the submission would fail with
 *   -EINVAL after any preceding split already wrote.
 *
 * One walk checks both: the alignment test must visit every fragment
 * anyway, so the discontinuity test rides the same pass on data already in
 * hand instead of a second O(nvecs) traversal, and the first violation of
 * either property returns early.  A contiguous page-tiled payload has no
 * interior discontinuities and is admitted on memory alignment alone.  The
 * iterator's total length must also be an @offset_align multiple.
 *
 * Returns NFS_DIO_DIRECT if the iterator may be issued as direct I/O, else
 * which of the two properties it breaks, for the caller's tracepoint.
 */
static enum nfs_dio_disposition
nfs_dio_iter_aligned_and_splittable(const struct iov_iter *iter,
				    u32 mem_align, u32 offset_align)
{
	const struct bio_vec *bvec = iter->bvec;
	size_t skip = iter->iov_offset;
	size_t left = iter->count;
	loff_t pos = 0;

	if (left & (offset_align - 1))
		return NFS_DIO_MEM_MISALIGNED;
	while (left) {
		unsigned int off = bvec->bv_offset + skip;
		size_t len = min_t(size_t, bvec->bv_len - skip, left);

		if ((off | len) & (mem_align - 1))
			return NFS_DIO_MEM_MISALIGNED;
		pos += len;
		left -= len;
		if (left &&
		    (((off + len) | (bvec + 1)->bv_offset) & ~PAGE_MASK) &&
		    (pos & (offset_align - 1)))
			return NFS_DIO_GAP_MISALIGNED;
		bvec++;
		skip = 0;
	}

	return NFS_DIO_DIRECT;
}

/**
 * nfs_dio_seg_hold - note the units a segment shares, right before writing it
 * @file: the file being written
 * @seg: the segment about to be written
 * @pos: file offset the segment starts at
 * @len: bytes the segment will write
 * @hold: filled in for nfs_dio_seg_release()
 *
 * The unit two writes share is the smallest folio the mapping allows: a
 * page, or a block where the block is larger than a page.  A segment shares
 * its first unit with the previous write if it does not start on a unit
 * boundary, and its last unit with the next one if it does not end on one; a
 * unit it covers entirely is its own.  A segment within one unit may share it
 * with both.  Such a unit is shared by exactly two writes, which may arrive
 * in either order, from different clients, at the same time.  The page cache
 * keeps it, unmarked, for the write that completes it; the segment is issued
 * IOCB_DONTCACHE_DEFER so that this write, not the page cache, marks it, and
 * only once the whole write, its sync included, is complete
 * (nfs_dio_seg_release()).
 *
 * Ask whether the neighbour has written its part immediately before writing
 * the unit, not when the write is split: asking earlier would give the
 * neighbour the time a direct middle takes to write its part and find this
 * write's part missing.  Nothing is held across the write.
 */
void nfs_dio_seg_hold(struct file *file, const struct nfs_dio_seg *seg,
		      loff_t pos, size_t len, struct nfs_dio_seg_hold *hold)
{
	struct address_space *mapping = file->f_mapping;
	size_t unit, last_len;
	loff_t mask, last_unit;
	bool one_unit, head, tail;

	hold->first = pos;
	hold->last = pos + len - 1;
	hold->complete_first = false;
	hold->complete_last = false;
	if (!seg->claim || !len)
		return;

	unit = mapping_min_folio_nrbytes(mapping);
	mask = ~(loff_t)(unit - 1);
	one_unit = (hold->first & mask) == (hold->last & mask);
	head = hold->first & (unit - 1);
	tail = (hold->last + 1) & (unit - 1);
	last_unit = hold->last & mask;
	last_len = (hold->last & (unit - 1)) + 1;

	hold->first_len = min_t(size_t, len,
				unit - (hold->first & (unit - 1)));
	if (head || (one_unit && tail))
		hold->complete_first =
			filemap_dontcache_edge_completes(mapping, hold->first,
							 hold->first_len);
	if (!one_unit && tail)
		hold->complete_last =
			filemap_dontcache_edge_completes(mapping, last_unit,
							 last_len);
}
EXPORT_SYMBOL_GPL(nfs_dio_seg_hold);

/**
 * nfs_dio_seg_release - mark the units a written segment shares
 * @file: the file being written
 * @hold: what nfs_dio_seg_hold() took
 * @written: bytes the segment's write wrote
 *
 * Call once the whole write the segment belongs to is complete, including
 * its fsync if it has one: a unit marked while still dirty is counted as
 * DONTCACHE dirty and wakes the DONTCACHE writeback kick.  Units this write
 * was the second of the two partners to write are marked for dropping.  A
 * short write that stopped inside the segment's first unit has not written
 * its part of that edge, and one that did not reach the last unit has not
 * written its part of that one; such an edge is left alone.
 */
void nfs_dio_seg_release(struct file *file, const struct nfs_dio_seg_hold *hold,
			 size_t written)
{
	struct address_space *mapping = file->f_mapping;

	if (hold->complete_first && written >= hold->first_len)
		filemap_dontcache_edge_complete(mapping, hold->first);
	if (hold->complete_last && written == hold->last - hold->first + 1)
		filemap_dontcache_edge_complete(mapping, hold->last);
}
EXPORT_SYMBOL_GPL(nfs_dio_seg_release);

/**
 * nfs_dio_split - split a write into direct and buffered segments
 * @file: the file being written
 * @policy: the alignments and the caller's choices
 * @direction: ITER_SOURCE for a write, ITER_DEST for a read
 * @bvec: the payload
 * @nvecs: entries in @bvec
 * @pos: file offset of the first byte
 * @total: bytes in the payload
 * @iocb_flags: the kiocb's flags, inherited by every segment
 * @split: the result
 *
 * The aligned middle of the payload is one direct segment; a misaligned
 * start and end are buffered.  Whenever direct I/O cannot be used at all,
 * the whole payload is one buffered segment, which carries IOCB_DONTCACHE
 * when the policy asks for it and the file system supports it, so its pages
 * are dropped once written back, and is an ordinary cached I/O otherwise.
 *
 * The direct middle of a write invalidates the page cache over its range,
 * so it must not share a folio with a buffered segment.  An end that is
 * already an @offset_align multiple needs no buffered segment.  A misaligned
 * end is cut at a multiple of the larger of @offset_align and the smallest
 * folio the mapping allows.  The prefix and suffix of a write hold the pages
 * they share.
 *
 * A direct read invalidates nothing, so a read is cut at @offset_align
 * multiples, no segment is DONTCACHE and none holds a page.
 */
void nfs_dio_split(struct file *file, const struct nfs_dio_policy *policy,
		   unsigned int direction, struct bio_vec *bvec,
		   unsigned int nvecs, loff_t pos, unsigned long total,
		   int iocb_flags, struct nfs_dio_split *split)
{
	u32 offset_align = policy->offset_align;
	u32 mem_align = policy->mem_align;
	loff_t prefix_end, orig_end, middle_end;
	size_t prefix = 0, middle = 0, suffix = 0;
	unsigned int dontcache_flags = 0;
	unsigned int buffered_flags = 0;
	unsigned int edge_flags;
	u32 mem_offset = nvecs ? bvec->bv_offset : 0;
	enum nfs_dio_disposition disposition;
	unsigned int nsegs = 0;
	unsigned long cut;

	if (direction == ITER_SOURCE &&
	    file->f_op->fop_flags & FOP_DONTCACHE)
		dontcache_flags = IOCB_DONTCACHE;
	/*
	 * Buffered segments follow the policy; the direct middle does not.
	 * Their shared pages are completed here (IOCB_DONTCACHE_DEFER), after
	 * the write's sync, rather than by the page cache while still dirty.
	 */
	if (policy->dontcache && dontcache_flags)
		buffered_flags = dontcache_flags | IOCB_DONTCACHE_DEFER;
	/*
	 * A misaligned end of a write needs no serialisation beyond the folio
	 * lock: a file system may write it under a shared inode lock where it
	 * can, e.g. where it fits in one folio (IOCB_NOSERIAL).
	 */
	edge_flags = buffered_flags;
	if (direction == ITER_SOURCE)
		edge_flags |= IOCB_NOSERIAL;

	/*
	 * If the file system doesn't advertise any alignment requirements,
	 * don't try to issue direct I/O at all.
	 */
	if (unlikely(!mem_align || !offset_align)) {
		disposition = NFS_DIO_NO_ALIGNMENT;
		goto no_dio;
	}

	/*
	 * If the I/O is smaller than the larger of the memory and logical
	 * offset alignment, no part of it can be direct I/O.
	 */
	if (unlikely(total < max(offset_align, mem_align))) {
		disposition = NFS_DIO_TOO_SMALL;
		goto no_dio;
	}

	orig_end = pos + total;

	/*
	 * The direct middle invalidates the page cache over its range. A
	 * middle that starts or stops inside the folio of a buffered end
	 * drops that folio, and the neighboring WRITE reads the folio back.
	 * A direct read invalidates nothing and is cut at the offset
	 * alignment.
	 */
	cut = offset_align;
	if (direction == ITER_SOURCE)
		cut = max_t(unsigned long, offset_align,
			    mapping_min_folio_nrbytes(file->f_mapping));
	/* round_up(pos, cut) can wrap at OFFSET_MAX, so take a distance. */
	prefix_end = pos;
	if (!IS_ALIGNED(pos, offset_align))
		prefix_end += min_t(loff_t, cut - (pos & (cut - 1)), total);
	middle_end = orig_end;
	if (!IS_ALIGNED(orig_end, offset_align))
		middle_end = max(round_down(orig_end, cut), prefix_end);

	prefix = prefix_end - pos;
	middle = middle_end - prefix_end;
	suffix = orig_end - middle_end;

	/*
	 * If there is no aligned middle section, or the aligned part is too
	 * small to be worth the split, issue a single buffered I/O instead of
	 * splitting up the write.
	 */
	if (!middle ||
	    ((prefix || suffix) &&
	     middle < policy->min_middle_bytes)) {
		disposition = NFS_DIO_NO_MIDDLE;
		goto no_dio;
	}

	/*
	 * The prefix and suffix are buffered I/O by definition.  Each shares
	 * its page with the neighbouring write; see nfs_dio_seg_hold(), which
	 * the caller uses right before issuing each of them, for how the page
	 * is held for the partner and dropped once both have written it.
	 * Without DONTCACHE both are plain cached I/Os and nothing is held or
	 * dropped: the pages stay until reclaim.
	 */
	if (prefix) {
		nfs_dio_seg_init(&split->segs[nsegs], direction, bvec, nvecs,
				 total, 0, prefix, iocb_flags);
		split->segs[nsegs].flags |= edge_flags;
		split->segs[nsegs++].claim = !!buffered_flags;
	}

	nfs_dio_seg_init(&split->segs[nsegs], direction, bvec, nvecs, total,
			 prefix, middle, iocb_flags);

	/*
	 * If the memory is not aligned -- or the block stack could be forced
	 * to split the payload at an interior discontinuity that is not
	 * logical-block aligned -- direct I/O is impossible for the middle,
	 * so issue the entire payload as a single buffered segment: splitting
	 * would only turn one buffered I/O into three.
	 */
	mem_offset = split->segs[nsegs].iter.bvec->bv_offset +
		     split->segs[nsegs].iter.iov_offset;
	disposition = nfs_dio_iter_aligned_and_splittable(
				&split->segs[nsegs].iter, mem_align,
				offset_align);
	if (disposition != NFS_DIO_DIRECT)
		goto no_dio;
	/*
	 * IOCB_DONTCACHE matters only if the file system falls back to
	 * buffered I/O (-ENOTBLK).
	 */
	split->segs[nsegs++].flags |= IOCB_DIRECT | dontcache_flags;
	disposition = NFS_DIO_DIRECT;

	if (suffix) {
		nfs_dio_seg_init(&split->segs[nsegs], direction, bvec, nvecs,
				 total, prefix + middle, suffix, iocb_flags);
		split->segs[nsegs].flags |= edge_flags;
		split->segs[nsegs++].claim = !!buffered_flags;
	}
	goto out;

no_dio:
	/*
	 * No direct I/O possible: pack into a single buffered segment.  Where
	 * it does not start or end on a page boundary, its first and last
	 * pages are shared with the neighbouring writes like a prefix or
	 * suffix and are held the same way (nfs_dio_seg_hold()).
	 */
	nfs_dio_seg_init(&split->segs[0], direction, bvec, nvecs, total, 0,
			 total, iocb_flags);
	split->segs[0].flags |= buffered_flags;
	split->segs[0].claim = !!buffered_flags;
	nsegs = 1;
out:
	split->nsegs = nsegs;
	split->disposition = disposition;
	split->dontcache = !!buffered_flags;
	split->prefix = prefix;
	split->middle = middle;
	split->suffix = suffix;
	split->mem_offset = mem_offset;
}
EXPORT_SYMBOL_GPL(nfs_dio_split);

MODULE_DESCRIPTION("Direct I/O helpers shared by NFSD and NFS LOCALIO");
MODULE_LICENSE("GPL");
