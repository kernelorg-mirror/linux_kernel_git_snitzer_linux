/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Direct I/O helpers shared by NFSD and the NFS client's LOCALIO path:
 * splitting a misaligned write into direct and buffered segments, and
 * holding the page a buffered boundary segment shares with its neighbour.
 */
#ifndef _LINUX_NFS_DIO_H
#define _LINUX_NFS_DIO_H

#include <linux/limits.h>
#include <linux/log2.h>
#include <linux/types.h>
#include <linux/uio.h>
#include <asm/page.h>

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

/* device takes discontiguous memory segments anywhere */
#define NFS_DIO_SEG_BOUNDARY_NONE	U32_MAX

/* What the caller knows about the file, and wants from the split. */
struct nfs_dio_policy {
	u32		mem_align;	/* alignment the device needs of payload memory */
	u32		offset_align;	/* alignment it needs of file offsets and lengths */
	/*
	 * Where the device lets payload memory be discontiguous: 0 if not
	 * known (a page-sized boundary is assumed), NFS_DIO_SEG_BOUNDARY_NONE
	 * if anywhere, else the power of two every gap between memory
	 * segments must fall on.
	 */
	u32		seg_boundary;
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

/*
 * Translate a statx-style report (@reported: STATX_DIO_SEG_BOUNDARY was in
 * the result mask; @boundary: dio_seg_boundary) into the policy encoding,
 * so every producer maps an unreported or unusable value to the
 * page-sized default the same way.
 */
static inline u32 nfs_dio_seg_boundary(bool reported, u32 boundary)
{
	if (!reported)
		return 0;
	if (!boundary)
		return NFS_DIO_SEG_BOUNDARY_NONE;
	return is_power_of_2(boundary) ? boundary : 0;
}

/*
 * The boundary interior memory joints are held to for a policy
 * @seg_boundary: 0 if none (NFS_DIO_SEG_BOUNDARY_NONE), else the reported
 * boundary, or PAGE_SIZE when it is not known.  The split and the callers'
 * tracepoints both use this, so a trace shows what the split enforces.
 */
static inline u32 nfs_dio_joint_boundary(u32 seg_boundary)
{
	if (seg_boundary == NFS_DIO_SEG_BOUNDARY_NONE)
		return 0;
	return seg_boundary ?: PAGE_SIZE;
}

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
