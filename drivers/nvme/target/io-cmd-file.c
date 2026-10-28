// SPDX-License-Identifier: GPL-2.0
/*
 * NVMe Over Fabrics Target File I/O commands implementation.
 * Copyright (c) 2017-2018 Western Digital Corporation or its
 * affiliates.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/uio.h>
#include <linux/falloc.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include "nvmet.h"

#define NVMET_MIN_MPOOL_OBJ		16

void nvmet_file_ns_revalidate(struct nvmet_ns *ns)
{
	ns->size = i_size_read(ns->file->f_mapping->host);
}

void nvmet_file_ns_disable(struct nvmet_ns *ns)
{
	if (ns->file) {
		if (ns->buffered_io)
			flush_workqueue(buffered_io_wq);
		mempool_destroy(ns->bvec_pool);
		ns->bvec_pool = NULL;
		fput(ns->file);
		ns->file = NULL;
	}
}

/*
 * Which lent bvecs (nvmet_req_set_data_bvec()) this file takes, and
 * whether a direct read goes IOCB_NOWAIT first.  Buffered I/O copies, so
 * it takes any bvec; direct I/O takes one whose segments meet the file
 * system's direct I/O alignment (STATX_DIOALIGN; a segment's length to
 * both its offset and its memory alignment), and none when it does not
 * say.  A direct read goes IOCB_NOWAIT first only where the block device
 * can refuse rather than block, and not where iomap always refuses one
 * (stable-write bounce, encryption): there it would only cost a workqueue
 * hop.  ext4 can still block a NOWAIT read updating atime.
 */
static void nvmet_file_ns_set_data_bvec(struct nvmet_ns *ns)
{
	struct inode *inode = file_inode(ns->file);
	struct block_device *bdev = inode->i_sb->s_bdev;
	struct kstat stat = {};

	ns->data_bvec_ok = false;
	ns->data_bvec_addr_mask = 0;
	ns->data_bvec_len_mask = 0;
	ns->file_nowait_read = false;

	if (ns->buffered_io) {
		ns->data_bvec_ok = true;
		return;
	}

	if (!vfs_getattr_nosec(&ns->file->f_path, &stat, STATX_DIOALIGN,
			       AT_STATX_SYNC_AS_STAT) &&
	    (stat.result_mask & STATX_DIOALIGN) &&
	    is_power_of_2(stat.dio_mem_align) &&
	    is_power_of_2(stat.dio_offset_align)) {
		ns->data_bvec_ok = true;
		ns->data_bvec_addr_mask = stat.dio_mem_align - 1;
		ns->data_bvec_len_mask = (stat.dio_offset_align - 1) |
					 (stat.dio_mem_align - 1);
	}

	ns->file_nowait_read = (ns->file->f_mode & FMODE_NOWAIT) &&
		bdev && bdev_nowait(bdev) &&
		!mapping_stable_writes(ns->file->f_mapping) &&
		!IS_ENCRYPTED(inode);
}

int nvmet_file_ns_enable(struct nvmet_ns *ns)
{
	int flags = O_RDWR | O_LARGEFILE;
	int ret = 0;

	if (!ns->buffered_io)
		flags |= O_DIRECT;

	ns->file = filp_open(ns->device_path, flags, 0);
	if (IS_ERR(ns->file)) {
		ret = PTR_ERR(ns->file);
		pr_err("failed to open file %s: (%d)\n",
			ns->device_path, ret);
		ns->file = NULL;
		return ret;
	}

	nvmet_file_ns_revalidate(ns);
	nvmet_file_ns_set_data_bvec(ns);

	/*
	 * i_blkbits can be greater than the universally accepted upper bound,
	 * so make sure we export a sane namespace lba_shift.
	 */
	ns->blksize_shift = min_t(u8,
			file_inode(ns->file)->i_blkbits, 12);

	ns->bvec_pool = mempool_create(NVMET_MIN_MPOOL_OBJ, mempool_alloc_slab,
			mempool_free_slab, nvmet_bvec_cache);

	if (!ns->bvec_pool) {
		ret = -ENOMEM;
		goto err;
	}

	return ret;
err:
	fput(ns->file);
	ns->file = NULL;
	ns->size = 0;
	ns->blksize_shift = 0;
	return ret;
}

static ssize_t nvmet_file_submit_bvec(struct nvmet_req *req, loff_t pos,
		unsigned long nr_segs, size_t count, int ki_flags)
{
	struct kiocb *iocb = &req->f.iocb;
	ssize_t (*call_iter)(struct kiocb *iocb, struct iov_iter *iter);
	struct iov_iter iter;
	int rw;

	if (req->cmd->rw.opcode == nvme_cmd_write) {
		if (req->cmd->rw.control & cpu_to_le16(NVME_RW_FUA))
			ki_flags |= IOCB_DSYNC;
		call_iter = req->ns->file->f_op->write_iter;
		rw = ITER_SOURCE;
	} else {
		call_iter = req->ns->file->f_op->read_iter;
		rw = ITER_DEST;
	}

	iov_iter_bvec(&iter, rw, req->f.bvec, nr_segs, count);

	iocb->ki_pos = pos;
	iocb->ki_flags = ki_flags | iocb->ki_filp->f_iocb_flags;

	return call_iter(iocb, &iter);
}

static void nvmet_file_submit_buffered_io(struct nvmet_req *req);

static void nvmet_file_io_done(struct kiocb *iocb, long ret)
{
	struct nvmet_req *req = container_of(iocb, struct nvmet_req, f.iocb);
	u16 status = NVME_SC_SUCCESS;

	/*
	 * An I/O submitted IOCB_NOWAIT can be refused after it started: a
	 * direct read's REQ_NOWAIT bio fails -EAGAIN (or -EOPNOTSUPP, from
	 * submit_bio_noacct()), and a direct or buffered read, or a buffered
	 * write, comes back short when a later part of its range would have
	 * blocked.  Do it all again, on the same bvec, from the workqueue,
	 * where it may block: rereading, or rewriting the same bytes through
	 * the page cache, is safe, as far as any two overlapping writes in
	 * flight are ordered at all.  (Direct writes are never submitted
	 * IOCB_NOWAIT; see nvmet_file_execute_rw().)  The retry keeps
	 * f.iocb.ki_filp, which a submitter may still read as it unwinds.
	 */
	if (unlikely(req->f.nowait) &&
	    (ret == -EAGAIN || ret == -EOPNOTSUPP ||
	     (ret >= 0 && ret < req->transfer_len))) {
		req->f.nowait = false;
		nvmet_file_submit_buffered_io(req);
		return;
	}

	if (req->f.bvec != req->inline_bvec && req->f.bvec != req->data_bvec) {
		if (likely(req->f.mpool_alloc == false))
			kfree(req->f.bvec);
		else
			mempool_free(req->f.bvec, req->ns->bvec_pool);
	}

	if (unlikely(ret != req->transfer_len)) {
		/* short or nothing (the file shrank): never success */
		if (ret >= 0)
			ret = -EIO;
		status = errno_to_nvme_status(req, ret);
	}
	nvmet_req_data_done(req);
	nvmet_req_complete(req, status);
}

static bool nvmet_file_execute_io(struct nvmet_req *req, int ki_flags)
{
	ssize_t nr_bvec = nvmet_req_data_segs(req);
	unsigned long bv_cnt = 0;
	bool is_sync = false;
	size_t len = 0, total_len = 0;
	ssize_t ret = 0;
	loff_t pos;
	int i;
	struct scatterlist *sg;

	if (req->f.mpool_alloc && nr_bvec > NVMET_MAX_MPOOL_BVEC)
		is_sync = true;

	pos = le64_to_cpu(req->cmd->rw.slba) << req->ns->blksize_shift;
	if (unlikely(pos + req->transfer_len > req->ns->size)) {
		/* through nvmet_file_io_done(), which frees f.bvec */
		ret = -ENOSPC;
		goto complete;
	}

	/*
	 * Clear all but ki_filp: on a retry (nvmet_file_io_done()) the
	 * submitter of the refused I/O may still be reading it (ext4's
	 * file_accessed()), and it is the same file anyway.
	 */
	BUILD_BUG_ON(offsetof(struct kiocb, ki_filp) != 0);
	req->f.iocb.ki_filp = req->ns->file;
	memset((char *)&req->f.iocb + sizeof(req->f.iocb.ki_filp), 0,
	       sizeof(struct kiocb) - sizeof(req->f.iocb.ki_filp));
	if (req->data_bvec) {
		/* the transport's own bvec: no copy of it to make */
		bv_cnt = req->data_bvec_cnt;
		total_len = req->transfer_len;
		goto submit;
	}
	for_each_sg(req->sg, sg, req->sg_cnt, i) {
		bvec_set_page(&req->f.bvec[bv_cnt], sg_page(sg), sg->length,
			      sg->offset);
		len += req->f.bvec[bv_cnt].bv_len;
		total_len += req->f.bvec[bv_cnt].bv_len;
		bv_cnt++;

		WARN_ON_ONCE((nr_bvec - 1) < 0);

		if (unlikely(is_sync) &&
		    (nr_bvec - 1 == 0 || bv_cnt == NVMET_MAX_MPOOL_BVEC)) {
			ret = nvmet_file_submit_bvec(req, pos, bv_cnt, len, 0);
			if (ret != len) {
				if (ret >= 0)
					ret = -EIO;
				goto complete;
			}

			pos += len;
			bv_cnt = 0;
			len = 0;
		}
		nr_bvec--;
	}

	if (WARN_ON_ONCE(total_len != req->transfer_len)) {
		ret = -EIO;
		goto complete;
	}

	if (unlikely(is_sync)) {
		ret = total_len;
		goto complete;
	}

submit:
	/*
	 * A NULL ki_complete asks for synchronous execution, which we want
	 * for the buffered IOCB_NOWAIT case.  A direct IOCB_NOWAIT is
	 * asynchronous, like any direct I/O.
	 */
	if (!(ki_flags & IOCB_NOWAIT) || !req->ns->buffered_io)
		req->f.iocb.ki_complete = nvmet_file_io_done;

	ret = nvmet_file_submit_bvec(req, pos, bv_cnt, total_len, ki_flags);

	switch (ret) {
	case -EIOCBQUEUED:
		return true;
	case -EAGAIN:
		if (WARN_ON_ONCE(!(ki_flags & IOCB_NOWAIT)))
			goto complete;
		return false;
	case -EOPNOTSUPP:
		/*
		 * For file systems returning error -EOPNOTSUPP, handle
		 * IOCB_NOWAIT error case separately and retry without
		 * IOCB_NOWAIT.
		 */
		if ((ki_flags & IOCB_NOWAIT))
			return false;
		break;
	}

complete:
	nvmet_file_io_done(&req->f.iocb, ret);
	return true;
}

static void nvmet_file_buffered_io_work(struct work_struct *w)
{
	struct nvmet_req *req = container_of(w, struct nvmet_req, f.work);

	nvmet_file_execute_io(req, 0);
}

static void nvmet_file_submit_buffered_io(struct nvmet_req *req)
{
	INIT_WORK(&req->f.work, nvmet_file_buffered_io_work);
	queue_work(buffered_io_wq, &req->f.work);
}

static void nvmet_file_execute_rw(struct nvmet_req *req)
{
	ssize_t nr_bvec = nvmet_req_data_segs(req);

	if (!nvmet_check_transfer_len(req, nvmet_rw_data_len(req)))
		return;

	if (!nr_bvec) {
		nvmet_req_complete(req, 0);
		return;
	}

	req->f.nowait = false;
	if (req->data_bvec)
		req->f.bvec = req->data_bvec;
	else if (nr_bvec > NVMET_MAX_INLINE_BIOVEC)
		req->f.bvec = kmalloc_objs(struct bio_vec, nr_bvec);
	else
		req->f.bvec = req->inline_bvec;

	if (unlikely(!req->f.bvec)) {
		/* fallback under memory pressure; never with data_bvec */
		req->f.bvec = mempool_alloc(req->ns->bvec_pool, GFP_KERNEL);
		req->f.mpool_alloc = true;
	} else
		req->f.mpool_alloc = false;

	if (req->ns->buffered_io) {
		/*
		 * A buffered IOCB_NOWAIT write fails -EINVAL where the file
		 * system does not take one (generic_write_checks_count()).
		 */
		req->f.nowait = true;
		if (likely(!req->f.mpool_alloc) &&
		    (req->ns->file->f_mode & FMODE_NOWAIT) &&
		    (req->cmd->rw.opcode == nvme_cmd_read ||
		     (req->ns->file->f_op->fop_flags & FOP_BUFFER_WASYNC)) &&
		    nvmet_file_execute_io(req, IOCB_NOWAIT))
			return;
		req->f.nowait = false;
		nvmet_file_submit_buffered_io(req);
	} else if (likely(!req->f.mpool_alloc) &&
		   req->cmd->rw.opcode == nvme_cmd_read &&
		   req->ns->file_nowait_read) {
		/*
		 * A direct read that would block (a lock, a full queue) must
		 * not block the transport's I/O context: submit it
		 * IOCB_NOWAIT first and run what refuses from the workqueue.
		 * Direct writes are submitted as before: one refused after a
		 * part of it was written fails -EIO (iomap_dio_complete()), so
		 * it cannot be retried.
		 */
		req->f.nowait = true;
		if (!nvmet_file_execute_io(req, IOCB_NOWAIT)) {
			req->f.nowait = false;
			nvmet_file_submit_buffered_io(req);
		}
	} else
		nvmet_file_execute_io(req, 0);
}

u16 nvmet_file_flush(struct nvmet_req *req)
{
	return errno_to_nvme_status(req, vfs_fsync(req->ns->file, 1));
}

static void nvmet_file_flush_work(struct work_struct *w)
{
	struct nvmet_req *req = container_of(w, struct nvmet_req, f.work);

	nvmet_req_complete(req, nvmet_file_flush(req));
}

static void nvmet_file_execute_flush(struct nvmet_req *req)
{
	if (!nvmet_check_transfer_len(req, 0))
		return;
	INIT_WORK(&req->f.work, nvmet_file_flush_work);
	queue_work(nvmet_wq, &req->f.work);
}

static void nvmet_file_execute_discard(struct nvmet_req *req)
{
	int mode = FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE;
	struct nvme_dsm_range range;
	loff_t offset, len;
	u16 status = 0;
	int ret;
	int i;

	for (i = 0; i <= le32_to_cpu(req->cmd->dsm.nr); i++) {
		status = nvmet_copy_from_sgl(req, i * sizeof(range), &range,
					sizeof(range));
		if (status)
			break;

		offset = le64_to_cpu(range.slba) << req->ns->blksize_shift;
		len = le32_to_cpu(range.nlb);
		len <<= req->ns->blksize_shift;
		if (offset + len > req->ns->size) {
			req->error_slba = le64_to_cpu(range.slba);
			status = errno_to_nvme_status(req, -ENOSPC);
			break;
		}

		ret = vfs_fallocate(req->ns->file, mode, offset, len);
		if (ret && ret != -EOPNOTSUPP) {
			req->error_slba = le64_to_cpu(range.slba);
			status = errno_to_nvme_status(req, ret);
			break;
		}
	}

	nvmet_req_complete(req, status);
}

static void nvmet_file_dsm_work(struct work_struct *w)
{
	struct nvmet_req *req = container_of(w, struct nvmet_req, f.work);

	switch (le32_to_cpu(req->cmd->dsm.attributes)) {
	case NVME_DSMGMT_AD:
		nvmet_file_execute_discard(req);
		return;
	case NVME_DSMGMT_IDR:
	case NVME_DSMGMT_IDW:
	default:
		/* Not supported yet */
		nvmet_req_complete(req, 0);
		return;
	}
}

static void nvmet_file_execute_dsm(struct nvmet_req *req)
{
	if (!nvmet_check_data_len_lte(req, nvmet_dsm_len(req)))
		return;
	INIT_WORK(&req->f.work, nvmet_file_dsm_work);
	queue_work(nvmet_wq, &req->f.work);
}

static void nvmet_file_write_zeroes_work(struct work_struct *w)
{
	struct nvmet_req *req = container_of(w, struct nvmet_req, f.work);
	struct nvme_write_zeroes_cmd *write_zeroes = &req->cmd->write_zeroes;
	int mode = FALLOC_FL_ZERO_RANGE | FALLOC_FL_KEEP_SIZE;
	loff_t offset;
	loff_t len;
	int ret;

	offset = le64_to_cpu(write_zeroes->slba) << req->ns->blksize_shift;
	len = (((sector_t)le16_to_cpu(write_zeroes->length) + 1) <<
			req->ns->blksize_shift);

	if (unlikely(offset + len > req->ns->size)) {
		nvmet_req_complete(req, errno_to_nvme_status(req, -ENOSPC));
		return;
	}

	ret = vfs_fallocate(req->ns->file, mode, offset, len);
	nvmet_req_complete(req, ret < 0 ? errno_to_nvme_status(req, ret) : 0);
}

static void nvmet_file_execute_write_zeroes(struct nvmet_req *req)
{
	if (!nvmet_check_transfer_len(req, 0))
		return;
	INIT_WORK(&req->f.work, nvmet_file_write_zeroes_work);
	queue_work(nvmet_wq, &req->f.work);
}

u16 nvmet_file_parse_io_cmd(struct nvmet_req *req)
{
	switch (req->cmd->common.opcode) {
	case nvme_cmd_read:
	case nvme_cmd_write:
		req->execute = nvmet_file_execute_rw;
		req->data_bvec_ok = req->ns->data_bvec_ok;
		return 0;
	case nvme_cmd_flush:
		req->execute = nvmet_file_execute_flush;
		return 0;
	case nvme_cmd_dsm:
		req->execute = nvmet_file_execute_dsm;
		return 0;
	case nvme_cmd_write_zeroes:
		req->execute = nvmet_file_execute_write_zeroes;
		return 0;
	default:
		return nvmet_report_invalid_opcode(req);
	}
}
