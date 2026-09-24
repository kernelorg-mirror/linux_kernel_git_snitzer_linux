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
	NFS_DIO_NO_ALIGN,	/* fs advertises no alignment: one segment */
	NFS_DIO_TOO_SMALL,	/* len < max(offset_align, mem_align): one segment */
	NFS_DIO_NO_MIDDLE,	/* no or tiny aligned middle: one segment */

	/* ORed in: the write's buffered segments carry IOCB_DONTCACHE */
	NFS_DIO_DONTCACHE = 0x80,
};

/* What the caller knows about the file, and wants from the split. */
struct nfs_dio_policy {
	u32		mem_align;	/* alignment the device needs of payload memory */
	u32		offset_align;	/* alignment it needs of file offsets and lengths */
	/*
	 * Do not split for a direct middle smaller than this many pages:
	 * three I/Os for a write that small cost more than one buffered one.
	 */
	unsigned int	min_middle_pages;
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
	bool		boundary;	/* prefix or suffix of a split */
	bool		edges;		/* buffered fallback: partial end pages */
};

struct nfs_dio_split {
	struct nfs_dio_seg	segs[NFS_DIO_MAX_SEGS];
	unsigned int		nsegs;
	/* NFS_DIO_DONTCACHE is ORed in when the buffered segments carry it */
	unsigned int		disposition;
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

void nfs_dio_split(struct file *file, const struct nfs_dio_policy *policy,
		   unsigned int direction, struct bio_vec *bvec,
		   unsigned int nvecs, loff_t pos, unsigned long total,
		   int iocb_flags, struct nfs_dio_split *split);
bool nfs_dio_boundary_claim(struct file *file, loff_t pos);
void nfs_dio_boundary_complete(struct file *file, loff_t pos);
void nfs_dio_seg_hold(struct file *file, const struct nfs_dio_seg *seg,
		      loff_t pos, size_t len, struct nfs_dio_seg_hold *hold);
void nfs_dio_seg_release(struct file *file, const struct nfs_dio_seg_hold *hold);

#endif /* _LINUX_NFS_DIO_H */
