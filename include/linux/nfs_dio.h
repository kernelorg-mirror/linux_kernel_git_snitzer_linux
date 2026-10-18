/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Direct I/O helpers shared by NFSD and the NFS client's LOCALIO path:
 * splitting a misaligned write into direct and buffered segments, and
 * holding the page a buffered boundary segment shares with its neighbour.
 */
#ifndef _LINUX_NFS_DIO_H
#define _LINUX_NFS_DIO_H

#include <linux/types.h>
#include <linux/uio.h>

struct bio_vec;
struct file;

/* How a write was split; callers report these in their tracepoints. */
enum nfs_dio_disposition {
	NFS_DIO_DIRECT,		/* aligned middle uses direct I/O */
	NFS_DIO_MEM_MISALIGNED,	/* payload memory misaligned: one segment */
	NFS_DIO_NO_ALIGNMENT,	/* fs advertises no alignment: one segment */
	NFS_DIO_TOO_SMALL,	/* len < max(offset_align, mem_align): one segment */
	NFS_DIO_NO_MIDDLE,	/* no or tiny aligned middle: one segment */
};

/* What the caller knows about the file, and wants from the split. */
struct nfs_dio_policy {
	u32		mem_align;	/* alignment the device needs of payload memory */
	u32		offset_align;	/* alignment it needs of file offsets and lengths */
	/*
	 * Do not split for a direct middle smaller than this many bytes:
	 * three I/Os for a write that small cost more than one buffered one.
	 * Bytes, not pages, so the gate does not grow with the page size.
	 */
	unsigned int	min_middle_bytes;
	/*
	 * Buffered segments carry IOCB_DONTCACHE, when the file system
	 * supports it, and hold the page each shares with the neighbouring
	 * write; see nfs_dio_seg_hold().  Otherwise they are ordinary cached
	 * writes and their pages stay until reclaim.
	 */
	bool		dontcache;
};

#define NFS_DIO_MAX_SEGS	3

struct nfs_dio_seg {
	struct iov_iter	iter;
	int		flags;		/* IOCB_* flags for this segment */
	bool		claim;		/* hold the pages it shares, see nfs_dio_seg_hold() */
};

struct nfs_dio_split {
	struct nfs_dio_seg	segs[NFS_DIO_MAX_SEGS];
	unsigned int		nsegs;
	enum nfs_dio_disposition	disposition;
	/* the buffered segments carry IOCB_DONTCACHE */
	bool			dontcache;
	/* the three candidate segments' byte counts, for tracing */
	size_t			prefix, middle, suffix;
};

/*
 * The pages a segment holds for its neighbour across its write; filled in by
 * nfs_dio_seg_hold() before the write, consumed by nfs_dio_seg_release()
 * after it.
 */
struct nfs_dio_seg_hold {
	loff_t	first, last;
	bool	complete_first, complete_last;
};

/*
 * A direct write that starts or stops inside a page may share that page
 * with a buffered write: nfs_dio_split() leaves an end that is already an
 * offset_align multiple where it is, and the write on the other side of it
 * is buffered there whenever it has no direct middle.  Such a direct write
 * must complete before its ->write_iter() returns.  A file system keeps
 * buffered writes out for the length of a synchronous direct write, the
 * invalidation of the page cache after it included; it drops that
 * exclusion when it queues an asynchronous one, and a page a neighbour
 * dirties before the I/O completes cannot be invalidated: the direct write
 * succeeds and the file's next fsync reports -EIO.
 *
 * True if a direct write of @len bytes at @pos must not be issued for
 * asynchronous completion.  Where offset_align is a page-size multiple it
 * never is.
 */
static inline bool nfs_dio_write_shares_page(loff_t pos, size_t len)
{
	return ((pos | (pos + len)) & ~PAGE_MASK) != 0;
}

void nfs_dio_split(struct file *file, const struct nfs_dio_policy *policy,
		   unsigned int direction, struct bio_vec *bvec,
		   unsigned int nvecs, loff_t pos, unsigned long total,
		   int iocb_flags, struct nfs_dio_split *split);
void nfs_dio_seg_hold(struct file *file, const struct nfs_dio_seg *seg,
		      loff_t pos, size_t len, struct nfs_dio_seg_hold *hold);
void nfs_dio_seg_release(struct file *file, const struct nfs_dio_seg_hold *hold,
			 size_t written);

#endif /* _LINUX_NFS_DIO_H */
