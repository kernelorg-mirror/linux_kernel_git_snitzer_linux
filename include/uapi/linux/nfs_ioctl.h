/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Hammerspace NFS attrs ioctl userspace api.
 *
 * Copyright (C) Richard Sharpe Hammerspace, 2017-2019
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _UAPI_LINUX_NFS_IOCTL_H
#define _UAPI_LINUX_NFS_IOCTL_H

#include <linux/types.h>
#include <asm/byteorder.h>
#include <linux/time.h>

/* NFS ioctls */
#define NFS_IOC_FILE_STATX_GET	_IOR('N', 2, struct nfs_ioctl_nfs4_statx)
#define NFS_IOC_FILE_STATX_SET	_IOW('N', 3, struct nfs_ioctl_nfs4_statx)

#define NFS_IOC_FILE_ACCESS_GET	_IOR('N', 4, struct nfs_ioctl_nfs4_access)

/* Options for struct nfs_ioctl_nfs4_statx */
#define NFS_FA_OPTIONS_SYNC_AS_STAT			0x0000
#define NFS_FA_OPTIONS_FORCE_SYNC			0x2000 /* See statx */
#define NFS_FA_OPTIONS_DONT_SYNC			0x4000 /* See statx */

#define NFS_FA_VALID_TIME_CREATE			0x00001UL
#define NFS_FA_VALID_TIME_BACKUP			0x00002UL
#define NFS_FA_VALID_ARCHIVE				0x00004UL
#define NFS_FA_VALID_HIDDEN				0x00008UL
#define NFS_FA_VALID_SYSTEM				0x00010UL
#define NFS_FA_VALID_OWNER				0x00020UL
#define NFS_FA_VALID_OWNER_GROUP			0x00040UL
#define NFS_FA_VALID_ATIME				0x00080UL
#define NFS_FA_VALID_MTIME				0x00100UL
#define NFS_FA_VALID_CTIME				0x00200UL
#define NFS_FA_VALID_OFFLINE				0x00400UL
#define NFS_FA_VALID_MODE				0x00800UL
#define NFS_FA_VALID_NLINK				0x01000UL
#define NFS_FA_VALID_BLKSIZE				0x02000UL
#define NFS_FA_VALID_INO				0x04000UL
#define NFS_FA_VALID_DEV				0x08000UL
#define NFS_FA_VALID_RDEV				0x10000UL
#define NFS_FA_VALID_SIZE				0x20000UL
#define NFS_FA_VALID_BLOCKS				0x40000UL
#define NFS_FA_VALID_UNCACHEABLE			0x80000UL

#define NFS_FA_VALID_ALL_ATTR_0 (NFS_FA_VALID_TIME_CREATE | \
		NFS_FA_VALID_TIME_BACKUP | \
		NFS_FA_VALID_ARCHIVE | \
		NFS_FA_VALID_HIDDEN | \
		NFS_FA_VALID_SYSTEM | \
		NFS_FA_VALID_OWNER | \
		NFS_FA_VALID_OWNER_GROUP | \
		NFS_FA_VALID_ATIME | \
		NFS_FA_VALID_MTIME | \
		NFS_FA_VALID_CTIME | \
		NFS_FA_VALID_OFFLINE | \
		NFS_FA_VALID_MODE | \
		NFS_FA_VALID_NLINK | \
		NFS_FA_VALID_BLKSIZE | \
		NFS_FA_VALID_INO | \
		NFS_FA_VALID_DEV | \
		NFS_FA_VALID_RDEV | \
		NFS_FA_VALID_SIZE | \
		NFS_FA_VALID_BLOCKS | \
		NFS_FA_VALID_UNCACHEABLE)

#define NFS_FA_FLAG_ARCHIVE				(1UL << 0)
#define NFS_FA_FLAG_HIDDEN				(1UL << 1)
#define NFS_FA_FLAG_SYSTEM				(1UL << 2)
#define NFS_FA_FLAG_OFFLINE				(1UL << 3)
#define NFS_FA_FLAG_UNCACHEABLE				(1UL << 4)

struct nfs_ioctl_timespec {
	__s64		tv_sec;
	__s64		tv_nsec;
};

struct nfs_ioctl_nfs4_statx {
	__s32		real_fd;		/* real FD; -1 == use current file */
	__u32		fa_options;

	__u64		fa_request[2];		/* Attributes to retrieve */
	__u64		fa_valid[2];		/* Attributes set */

	struct nfs_ioctl_timespec fa_time_backup;/* Backup time */
	struct nfs_ioctl_timespec fa_btime;     /* Birth time */
	__u64		fa_flags;		/* Flag attributes */
	/* Ordinary attributes follow */
	struct nfs_ioctl_timespec fa_atime;	/* Access time */
	struct nfs_ioctl_timespec fa_mtime;	/* Modify time */
	struct nfs_ioctl_timespec fa_ctime;	/* Change time */
	__u32		fa_owner_uid;		/* Owner User ID */
	__u32		fa_group_gid;		/* Primary Group ID */
	__u32		fa_mode;		/* Mode */
	__u32		fa_nlink;
	__u32		fa_blksize;
	__u32		fa_spare;		/* Alignment */
	__u64		fa_ino;
	__u32		fa_dev;
	__u32		fa_rdev;
	__s64		fa_size;
	__s64		fa_blocks;
	__u64		fa_padding[4];
};

struct nfs_ioctl_nfs4_access {
	/* input */
	__u64		ac_flags;		/* operation flags */
	/* output */
	__u64		ac_mask;		/* NFS raw ACCESS reply mask */
};

#define NFS_AC_FLAG_EACCESS (1UL << 0)

#endif /* _UAPI_LINUX_NFS_IOCTL_H */
