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
 * it.  This is that logic, once, for both callers.
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
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
	seg->boundary = false;
	seg->edges = false;
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
		size_t len = bvec->bv_len;

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
 * nfs_dio_boundary_claim - claim the page a boundary segment shares
 * @file: the file being written
 * @pos: any byte offset within the page
 *
 * The page holding a misaligned prefix or suffix is shared by exactly two
 * writes, the one ending in it and the one starting in it, which may arrive
 * in either order, from different clients, at the same time.  Put an empty
 * folio there if there is not one already: it is inserted unmarked, so
 * neither writer's DONTCACHE write marks it (a DONTCACHE write only marks a
 * folio it allocated itself) and it never enters WB_DONTCACHE_DIRTY, which
 * is what would otherwise arm the DONTCACHE writeback kick and have the
 * flusher write the page back, and drop it, between the two writes.
 *
 * The add is atomic, so of two concurrent partners exactly one gets the
 * folio.  If it cannot be allocated the segment is an ordinary DONTCACHE
 * write and the page is dropped after writeback as before.
 *
 * Return: true if this write is the second of the two and must call
 * nfs_dio_boundary_complete() once its segment has been written.
 */
bool nfs_dio_boundary_claim(struct file *file, loff_t pos)
{
	struct address_space *mapping = file->f_mapping;
	pgoff_t index = pos >> PAGE_SHIFT;
	gfp_t gfp = mapping_gfp_mask(mapping);
	struct folio *folio;
	int err;

	folio = filemap_alloc_folio(gfp, 0, NULL);
	if (!folio)
		return false;
	err = filemap_add_folio(mapping, folio, index, gfp);
	if (!err) {
		/* First writer: the page is in place, waiting for the partner. */
		folio_unlock(folio);
		folio_put(folio);
		return false;
	}
	folio_put(folio);
	return err == -EEXIST;
}
EXPORT_SYMBOL_GPL(nfs_dio_boundary_claim);

/**
 * nfs_dio_boundary_complete - mark a shared page both writers have written
 * @file: the file being written
 * @pos: any byte offset within the page
 *
 * The second writer's data is in the page: mark it so the next writeback
 * that cleans it drops it.  This must follow the write, because a clean
 * marked folio is dropped by whatever writeback completes next, and it uses
 * folio_set_dropbehind() rather than an accounted setter, because counting a
 * folio marked while dirty arms the DONTCACHE writeback kick and the page is
 * then written back, and dropped, before its partner has written it.
 */
void nfs_dio_boundary_complete(struct file *file, loff_t pos)
{
	struct address_space *mapping = file->f_mapping;
	struct folio *folio;

	folio = __filemap_get_folio(mapping, pos >> PAGE_SHIFT, FGP_DONTCACHE, 0);
	if (IS_ERR(folio))
		return;
	folio_set_dropbehind(folio);
	folio_put(folio);
}
EXPORT_SYMBOL_GPL(nfs_dio_boundary_complete);

/**
 * nfs_dio_seg_hold - claim the pages a segment shares, right before writing it
 * @file: the file being written
 * @seg: the segment about to be written
 * @pos: file offset the segment starts at
 * @len: bytes the segment will write
 * @hold: filled in for nfs_dio_seg_release()
 *
 * Claim the boundary page immediately before writing it, not when the write
 * is split.  Nothing is held across the write: the claim leaves the page in
 * the page cache unmarked, and claiming it earlier would give the partner
 * write the time a direct middle takes to complete the page and have it
 * dropped before this segment writes it.  A whole-write buffered segment
 * shares its first page with the previous write if it does not start on a
 * page boundary, and its last page with the next one if it does not end on
 * one; a page it covers entirely is its own.
 */
void nfs_dio_seg_hold(struct file *file, const struct nfs_dio_seg *seg,
		      loff_t pos, size_t len, struct nfs_dio_seg_hold *hold)
{
	hold->first = pos;
	hold->last = pos + len - 1;
	hold->complete_first = false;
	hold->complete_last = false;
	if (seg->boundary) {
		hold->complete_first = nfs_dio_boundary_claim(file, hold->first);
	} else if (seg->edges) {
		if (hold->first & ~PAGE_MASK)
			hold->complete_first =
				nfs_dio_boundary_claim(file, hold->first);
		if (((hold->last + 1) & ~PAGE_MASK) &&
		    (hold->last >> PAGE_SHIFT) != (hold->first >> PAGE_SHIFT))
			hold->complete_last =
				nfs_dio_boundary_claim(file, hold->last);
	}
}
EXPORT_SYMBOL_GPL(nfs_dio_seg_hold);

/**
 * nfs_dio_seg_release - mark the pages a written segment shares
 * @file: the file being written
 * @hold: what nfs_dio_seg_hold() took
 *
 * Call once the segment's write has completed.  Pages this write was the
 * second of the two partners to write are marked for dropping.
 */
void nfs_dio_seg_release(struct file *file, const struct nfs_dio_seg_hold *hold)
{
	if (hold->complete_first)
		nfs_dio_boundary_complete(file, hold->first);
	if (hold->complete_last)
		nfs_dio_boundary_complete(file, hold->last);
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
 * For a read, no segment is DONTCACHE and none holds a page: the split is
 * the same, the treatment of the buffered segments is not.
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
	unsigned int disposition;
	unsigned int nsegs = 0;

	if (direction == ITER_SOURCE &&
	    file->f_op->fop_flags & FOP_DONTCACHE)
		dontcache_flags = IOCB_DONTCACHE;
	/* Buffered segments follow the policy; the direct middle does not. */
	if (policy->dontcache)
		buffered_flags = dontcache_flags;

	/*
	 * If the file system doesn't advertise any alignment requirements,
	 * don't try to issue direct I/O at all.
	 */
	if (unlikely(!mem_align || !offset_align)) {
		disposition = NFS_DIO_NO_ALIGN;
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

	prefix_end = round_up(pos, offset_align);
	orig_end = pos + total;
	middle_end = round_down(orig_end, offset_align);

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
	     middle < PAGE_SIZE * policy->min_middle_pages)) {
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
		split->segs[nsegs++].boundary = !!buffered_flags;
	}

	nfs_dio_seg_init(&split->segs[nsegs], direction, bvec, nvecs, total,
			 prefix, middle, iocb_flags);

	/*
	 * If the memory is not aligned, direct I/O is impossible for the
	 * middle, so issue the entire payload as a single buffered segment:
	 * splitting would only turn one buffered I/O into three.
	 */
	if (!nfs_dio_iter_aligned(&split->segs[nsegs].iter, mem_align,
				  offset_align)) {
		disposition = NFS_DIO_MEM_MISALIGNED;
		goto no_dio;
	}
	/*
	 * Also mark the direct middle DONTCACHE: the file system may fall
	 * back to buffered I/O on its own (e.g. XFS on -ENOTBLK when it
	 * cannot invalidate page cache that a concurrent buffered prefix or
	 * suffix of an adjacent write just dirtied), and it reuses this kiocb
	 * to do so.  On the direct path itself the flag is inert.
	 */
	split->segs[nsegs++].flags |= IOCB_DIRECT | dontcache_flags;
	disposition = NFS_DIO_DIRECT;

	if (suffix) {
		nfs_dio_seg_init(&split->segs[nsegs], direction, bvec, nvecs,
				 total, prefix + middle, suffix, iocb_flags);
		split->segs[nsegs].flags |= buffered_flags;
		split->segs[nsegs++].boundary = !!buffered_flags;
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
	split->segs[0].edges = !!buffered_flags;
	nsegs = 1;
out:
	split->nsegs = nsegs;
	split->disposition = disposition |
			     (buffered_flags ? NFS_DIO_DONTCACHE : 0);
	split->prefix = prefix;
	split->middle = middle;
	split->suffix = suffix;
}
EXPORT_SYMBOL_GPL(nfs_dio_split);

MODULE_DESCRIPTION("Direct I/O helpers shared by NFSD and NFS LOCALIO");
MODULE_LICENSE("GPL");
