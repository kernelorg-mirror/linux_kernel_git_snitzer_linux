// SPDX-License-Identifier: GPL-2.0
/*
 * statx-dio - print the direct I/O attributes statx(2) reports for a path.
 *
 * Build:  gcc -O2 -Wall -o statx-dio statx-dio.c
 * Usage:  statx-dio PATH
 *
 * Asks for STATX_DIOALIGN | STATX_DIO_READ_ALIGN | STATX_DIO_SEG_BOUNDARY
 * and prints stx_mask plus each direct I/O field whose bit came back set.
 * Exits 1 if STATX_DIO_SEG_BOUNDARY is not in stx_mask (the file system or
 * the kernel does not report it), 2 on a usage or statx error.
 *
 * Installed uapi headers that predate a field get a local fallback: the mask
 * bit is defined here and the field is read at its fixed offset in the
 * 256-byte struct statx.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/stat.h>

#ifndef STATX_DIO_READ_ALIGN
#define STATX_DIO_READ_ALIGN	0x00020000U
#endif
#ifndef STATX_DIO_SEG_BOUNDARY
#define STATX_DIO_SEG_BOUNDARY	0x00040000U
#endif

#define STX_DIO_READ_OFFSET_ALIGN_OFF	0xb4
#define STX_DIO_SEG_BOUNDARY_OFF	0xbc

static __u32 stx_u32_at(const struct statx *stx, size_t off)
{
	__u32 v;

	memcpy(&v, (const char *)stx + off, sizeof(v));
	return v;
}

int main(int argc, char **argv)
{
	unsigned int want = STATX_DIOALIGN | STATX_DIO_READ_ALIGN |
			    STATX_DIO_SEG_BOUNDARY;
	struct statx stx;

	if (argc != 2) {
		fprintf(stderr, "usage: %s PATH\n", argv[0]);
		return 2;
	}

	memset(&stx, 0, sizeof(stx));
	if (syscall(SYS_statx, AT_FDCWD, argv[1], 0, want, &stx) < 0) {
		fprintf(stderr, "statx %s: %s\n", argv[1], strerror(errno));
		return 2;
	}

	printf("path=%s stx_mask=0x%08x\n", argv[1], stx.stx_mask);
	if (stx.stx_mask & STATX_DIOALIGN)
		printf("dio_mem_align=%u dio_offset_align=%u\n",
		       stx.stx_dio_mem_align, stx.stx_dio_offset_align);
	if (stx.stx_mask & STATX_DIO_READ_ALIGN)
		printf("dio_read_offset_align=%u\n",
		       stx_u32_at(&stx, STX_DIO_READ_OFFSET_ALIGN_OFF));
	if (!(stx.stx_mask & STATX_DIO_SEG_BOUNDARY)) {
		printf("dio_seg_boundary: not reported\n");
		return 1;
	}
	printf("dio_seg_boundary=%u\n",
	       stx_u32_at(&stx, STX_DIO_SEG_BOUNDARY_OFF));
	return 0;
}
