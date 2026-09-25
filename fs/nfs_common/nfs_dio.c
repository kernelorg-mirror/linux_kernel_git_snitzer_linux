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
 * Is every bio_vec of @iter aligned to @mem_align in memory, and its total
 * length to @len_align?  A vector built from RPC receive buffers or from a
 * pinned O_DIRECT user buffer is contiguous after its first entry, so the
 * loop rarely has more than one entry to reject; it is kept for the vectors
 * that are not.
 */
static bool
nfs_dio_iter_aligned(const struct iov_iter *iter, u32 mem_align, u32 len_align)
{
	const struct bio_vec *bvec = iter->bvec;
	size_t skip = iter->iov_offset;
	size_t size = iter->count;

	if (size & (len_align - 1))
		return false;
	do {
		size_t len = bvec->bv_len - skip;

		if (len > size)
			len = size;
		if ((unsigned long)(bvec->bv_offset + skip) & (mem_align - 1))
			return false;
		bvec++;
		size -= len;
		skip = 0;
	} while (size);

	return true;
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

	hold->first = pos;
	hold->last = pos + len - 1;
	hold->complete_first = false;
	hold->complete_last = false;
	if (seg->claim && len) {
		size_t unit = mapping_min_folio_nrpages(mapping) << PAGE_SHIFT;
		loff_t mask = ~(loff_t)(unit - 1);
		bool one_unit = (hold->first & mask) == (hold->last & mask);
		bool head = hold->first & (unit - 1);
		bool tail = (hold->last + 1) & (unit - 1);

		hold->first_len = min_t(size_t, len,
					unit - (hold->first & (unit - 1)));
		if (head || (one_unit && tail))
			hold->complete_first =
				filemap_dontcache_edge_completes(mapping, hold->first);
		if (!one_unit && tail)
			hold->complete_last =
				filemap_dontcache_edge_completes(mapping, hold->last);
	}
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
 * so it must not share a page with a buffered segment.  An end that is
 * already an @offset_align multiple needs no buffered segment; a misaligned
 * end is cut at a multiple of the larger of @offset_align and the page
 * size.  The prefix and suffix of a write hold the pages they share.
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
	u32 mem_offset = nvecs ? bvec->bv_offset : 0;
	enum nfs_dio_disposition disposition;
	unsigned int nsegs = 0;
	u32 cut;

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
	 * The direct middle invalidates the page cache over its range, so it
	 * must not share a folio with a buffered end.  An aligned end needs no
	 * buffered segment; a misaligned one is cut on a boundary of the
	 * smallest folio the mapping allows, a page or, where the block is
	 * larger, a block, even where the offset alignment is finer.  A
	 * direct read invalidates nothing and is cut at the offset alignment.
	 * A write may end at OFFSET_MAX, so the prefix is sized as a distance
	 * rather than by rounding @pos up, which could wrap.
	 */
	cut = offset_align;
	if (direction == ITER_SOURCE)
		cut = max3(offset_align, PAGE_SIZE,
			   (mapping_min_folio_nrpages(file->f_mapping) <<
			    PAGE_SHIFT));
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
		split->segs[nsegs].flags |= buffered_flags;
		split->segs[nsegs++].claim = !!buffered_flags;
	}

	nfs_dio_seg_init(&split->segs[nsegs], direction, bvec, nvecs, total,
			 prefix, middle, iocb_flags);

	/*
	 * If the memory is not aligned, direct I/O is impossible for the
	 * middle, so issue the entire payload as a single buffered segment:
	 * splitting would only turn one buffered I/O into three.
	 */
	mem_offset = split->segs[nsegs].iter.bvec->bv_offset +
		     split->segs[nsegs].iter.iov_offset;
	if (!nfs_dio_iter_aligned(&split->segs[nsegs].iter, mem_align,
				  offset_align)) {
		disposition = NFS_DIO_MEM_MISALIGNED;
		goto no_dio;
	}
	/*
	 * IOCB_DONTCACHE matters only if the file system falls back to
	 * buffered I/O (-ENOTBLK).
	 */
	split->segs[nsegs++].flags |= IOCB_DIRECT | dontcache_flags;
	disposition = NFS_DIO_DIRECT;

	if (suffix) {
		nfs_dio_seg_init(&split->segs[nsegs], direction, bvec, nvecs,
				 total, prefix + middle, suffix, iocb_flags);
		split->segs[nsegs].flags |= buffered_flags;
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
