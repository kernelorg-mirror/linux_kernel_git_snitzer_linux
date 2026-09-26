// SPDX-License-Identifier: GPL-2.0
/*
 * bvecrepro: reproduce the loaned-bvec direct-write tail-drop without NFS.
 *
 * Builds a 1 MiB REQ_OP_WRITE bio whose bvec table is an external array
 * (the bio_iov_bvec_set() shape iomap uses for ITER_BVEC direct I/O):
 *   bv0 = (off0, PAGE_SIZE - off0), N-1 x (0, PAGE_SIZE), (0, off0)
 * (N = 1 MiB / PAGE_SIZE full pages: 64 bvecs + spill on 16K, 256 + spill on 4K)
 * submits it raw to the target device, reads the range back, and compares
 * against the stamped pattern (each u32 = its own payload offset).
 *
 * Gapped mode (frag=N, N > 0): the payload is instead cut into N-byte
 * fragments, one per page, each starting at page offset frag_off, the last
 * one taking the remainder -- the geometry of a loaned receive, one fragment
 * per TCP segment.  Every joint is then a gap in memory.  A device with no
 * virtual boundary takes it as long as every fragment meets its DMA
 * alignment; a device with one must split at each gap on a logical-block
 * multiple, which fails with -EINVAL when a gap is not on one.
 */
#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/highmem.h>
#include <linux/vmalloc.h>

static char *dev = "/dev/nvme0n1";
module_param(dev, charp, 0444);
static uint off0 = 684;
module_param(off0, uint, 0444);
static ulong start_sector = 4096;
module_param(start_sector, ulong, 0444);
static uint frag;
module_param(frag, uint, 0444);
static uint frag_off;
module_param(frag_off, uint, 0444);

#define TOTAL (1024 * 1024)

static int __init bvecrepro_init(void)
{
	unsigned int nfull = TOTAL / PAGE_SIZE;
	unsigned int npages = nfull + 1;	/* 65 on 16K, 257 on 4K */
	struct page **pages, **rpages;
	struct bio_vec *bvecs, *rtable;
	struct file *bdev_file;
	struct block_device *bdev;
	unsigned int i, nvecs, p;
	struct bio bio;
	int ret;

	if (frag) {
		if (frag % 4 || frag_off + frag > PAGE_SIZE)
			return -EINVAL;
		npages = DIV_ROUND_UP(TOTAL, frag);
	} else if (off0 == 0) {
		npages = nfull;
	}

	bdev_file = bdev_file_open_by_path(dev, BLK_OPEN_READ | BLK_OPEN_WRITE,
					   THIS_MODULE, NULL);
	if (IS_ERR(bdev_file))
		return PTR_ERR(bdev_file);
	bdev = file_bdev(bdev_file);

	pages = kcalloc(npages, sizeof(*pages), GFP_KERNEL);
	rpages = kcalloc(TOTAL / PAGE_SIZE, sizeof(*rpages), GFP_KERNEL);
	bvecs = kcalloc(npages, sizeof(*bvecs), GFP_KERNEL);
	rtable = kcalloc(TOTAL / PAGE_SIZE, sizeof(*rtable), GFP_KERNEL);
	if (!pages || !rpages || !bvecs || !rtable) {
		ret = -ENOMEM;
		goto out;
	}
	for (i = 0; i < npages; i++) {
		pages[i] = alloc_page(GFP_KERNEL);
		if (!pages[i]) {
			ret = -ENOMEM;
			goto out;
		}
	}
	for (i = 0; i < TOTAL / PAGE_SIZE; i++) {
		rpages[i] = alloc_page(GFP_KERNEL);
		if (!rpages[i]) {
			ret = -ENOMEM;
			goto out;
		}
	}

	/* bvec geometry */
	nvecs = 0;
	if (frag) {
		for (i = 0, p = 0; p < TOTAL; i++, p += frag)
			bvec_set_page(&bvecs[nvecs++], pages[i],
				      min_t(unsigned int, frag, TOTAL - p),
				      frag_off);
	} else if (off0) {
		bvec_set_page(&bvecs[nvecs++], pages[0], PAGE_SIZE - off0, off0);
		for (i = 1; i < nfull; i++)
			bvec_set_page(&bvecs[nvecs++], pages[i], PAGE_SIZE, 0);
		bvec_set_page(&bvecs[nvecs++], pages[nfull], off0, 0);
	} else {
		for (i = 0; i < nfull; i++)
			bvec_set_page(&bvecs[nvecs++], pages[i], PAGE_SIZE, 0);
	}

	/* stamp: each u32 in payload order = its own payload byte offset */
	p = 0;
	for (i = 0; i < nvecs; i++) {
		u32 *base = page_address(bvecs[i].bv_page);
		unsigned int o;

		for (o = 0; o < bvecs[i].bv_len; o += 4)
			base[(bvecs[i].bv_offset + o) / 4] = p + o;
		p += bvecs[i].bv_len;
	}

	/* write: external bvec table, the bio_iov_bvec_set() shape */
	bio_init(&bio, bdev, NULL, 0, REQ_OP_WRITE | REQ_SYNC);
	bio.bi_iter.bi_sector = start_sector;
	bio.bi_io_vec = bvecs;
	bio.bi_iter.bi_idx = 0;
	bio.bi_iter.bi_bvec_done = 0;
	bio.bi_iter.bi_size = TOTAL;
	bio_set_flag(&bio, BIO_CLONED);
	ret = submit_bio_wait(&bio);
	pr_info("bvecrepro: WRITE dev=%s off0=%u frag=%u frag_off=%u nvecs=%u status=%d\n",
		dev, off0, frag, frag_off, nvecs, ret);
	if (ret)
		goto out;

	/* read back with a clean page-aligned bio */
	bio_init(&bio, bdev, rtable, TOTAL / PAGE_SIZE, REQ_OP_READ);
	bio.bi_iter.bi_sector = start_sector;
	for (i = 0; i < TOTAL / PAGE_SIZE; i++)
		if (bio_add_page(&bio, rpages[i], PAGE_SIZE, 0) != PAGE_SIZE) {
			ret = -EIO; goto out;
		}
	ret = submit_bio_wait(&bio);
	pr_info("bvecrepro: READ status=%d\n", ret);
	if (ret)
		goto out;

	/* compare */
	{
		unsigned int onset = TOTAL, bad = 0;
		s64 shift = 0;

		for (p = 0; p < TOTAL; p += 4) {
			u32 *base = page_address(rpages[p / PAGE_SIZE]);
			u32 got = base[(p % PAGE_SIZE) / 4];

			if (got != p) {
				if (onset == TOTAL) {
					onset = p;
					shift = (s64)got - p;
				}
				bad += 4;
			}
		}
		if (onset == TOTAL)
			pr_info("bvecrepro: MATCH (all %u bytes correct)\n", TOTAL);
		else
			pr_info("bvecrepro: CORRUPT onset=%u onset%%%lu=%lu first-shift=%lld bad-bytes=%u\n",
				onset, PAGE_SIZE, (ulong)onset % PAGE_SIZE,
				shift, bad);
	}
	ret = 0;
out:
	if (pages)
		for (i = 0; i < npages; i++)
			if (pages[i])
				__free_page(pages[i]);
	if (rpages)
		for (i = 0; i < TOTAL / PAGE_SIZE; i++)
			if (rpages[i])
				__free_page(rpages[i]);
	kfree(pages);
	kfree(rpages);
	kfree(bvecs);
	kfree(rtable);
	fput(bdev_file);
	return ret ? ret : -EAGAIN;	/* never stay loaded; result is in dmesg */
}
module_init(bvecrepro_init);
MODULE_DESCRIPTION("loaned-bvec direct-write tail-drop reproducer");
MODULE_LICENSE("GPL");
