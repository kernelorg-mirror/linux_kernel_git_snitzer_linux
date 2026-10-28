// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for nfsd's run-to-completion NFSv3 READ: nfsd_rtc_read(),
 * nfsd_rtc_read_file() (the fast path after the file-cache lookup),
 * nfsd3_rtc_complete() and nfsd_rtc_dispatch_finish().
 *
 * The file is an anonymous inode on an internal mount whose read_iter
 * finishes synchronously, returns -EIOCBQUEUED or returns -EAGAIN for an
 * IOCB_NOWAIT read.  A hit's READ3res is encoded with the real
 * nfs3svc_encode_readres() and compared byte for byte with the thread
 * path's (nfsd_iter_read() with io_cache_read direct) for the same READ.
 *
 * WRITE: the file's write_iter copies into a buffer, finishing synchronously,
 * returning -EIOCBQUEUED or returning -EAGAIN for IOCB_NOWAIT.  A hit's
 * WRITE3res (nfs3svc_encode_writeres()) is compared byte for byte with the
 * thread path's (nfsd_vfs_write() with io_cache_write direct).
 *
 * Either file can also fail an IOCB_NOWAIT call with a chosen error, or
 * (write) stop short, to show that every outcome of the NOWAIT attempt
 * other than a whole transfer is a counted miss with no reply and no
 * verifier reset.  The superblock can be frozen (sb_start_write_trylock()
 * failing on the work item).  The DRC case uses real nfsd_cache_lookup()
 * entries in init_net's reply cache, set up and torn down by the test.
 *
 * The suite is named under "sunrpc" so that the one sunrpc* KUnit boot
 * that covers the transport half of the fast path runs it too.
 */
#include <kunit/test.h>

#include <linux/file.h>
#include <linux/fs/super.h>
#include <linux/hash.h>
#include <linux/in.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/highmem.h>
#include <linux/magic.h>
#include <linux/mount.h>
#include <linux/pseudo_fs.h>
#include <linux/nfs3.h>
#include <linux/sunrpc/svc.h>
#include <linux/sunrpc/svc_xprt.h>
#include <linux/sunrpc/xdr.h>
#include <linux/uio.h>

#include "cache.h"
#include "nfsd.h"
#include "netns.h"
#include "vfs.h"
#include "xdr3.h"
#include "filecache.h"
#include "nfserr.h"

enum { RTCN_SYNC, RTCN_ASYNC, RTCN_EAGAIN };

#define RTCN_NFS_PROGRAM	100003
#define RTCN_SIZE	(3 * PAGE_SIZE + 100)
#define RTCN_ALIGN	512
#define RTCN_PAGES	8
/* status, post_op_attr (absent), count, eof, opaque length */
#define RTCN_HDR	(5 * XDR_UNIT)

struct rtcn {
	struct kunit		*test;
	struct vfsmount		*mnt;
	struct file		*file;
	u8			*data;
	int			mode;
	unsigned int		calls;
	int			flags;		/* ki_flags of the last read */
	struct kiocb		*pending;
	long			res;		/* what the async read returns */
	struct svc_serv		*serv;
	struct svc_rqst		*rq[2];		/* 0: thread path, 1: fast path */
	struct svc_xprt		xprt;
	struct nfsd_file	nf;
	struct svc_fh		fh;
	u64			saved_io_cache_read;
	u8			*reply[2];
	unsigned int		reply_len[2];
	u64			saved_io_cache_write;
	u8			*wdata;		/* what writes stored */
	struct page		*wpages[RTCN_PAGES];
	struct xdr_buf		payload;
	struct svc_export	*exp;
	int			nowait_err;	/* IOCB_NOWAIT I/O fails so */
	size_t			nowait_short;	/* IOCB_NOWAIT write stops here */
};

static ssize_t rtcn_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct rtcn *t = iocb->ki_filp->private_data;
	loff_t pos = iocb->ki_pos;
	size_t n = 0;

	t->calls++;
	t->flags = iocb->ki_flags;
	if (t->mode == RTCN_EAGAIN && (iocb->ki_flags & IOCB_NOWAIT))
		return -EAGAIN;
	if (t->nowait_err && (iocb->ki_flags & IOCB_NOWAIT))
		return t->nowait_err;
	if (pos < RTCN_SIZE)
		n = copy_to_iter(t->data + pos,
				 min_t(size_t, iov_iter_count(to),
				       RTCN_SIZE - pos), to);
	iocb->ki_pos += n;
	if (t->mode == RTCN_ASYNC && !is_sync_kiocb(iocb)) {
		t->pending = iocb;
		t->res = n;
		return -EIOCBQUEUED;
	}
	return n;
}

static ssize_t rtcn_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct rtcn *t = iocb->ki_filp->private_data;
	loff_t pos = iocb->ki_pos;
	size_t n;

	t->calls++;
	t->flags = iocb->ki_flags;
	if (t->mode == RTCN_EAGAIN && (iocb->ki_flags & IOCB_NOWAIT))
		return -EAGAIN;
	if (t->nowait_err && (iocb->ki_flags & IOCB_NOWAIT))
		return t->nowait_err;
	if (pos >= RTCN_SIZE)
		return -EFBIG;
	n = min_t(size_t, iov_iter_count(from), RTCN_SIZE - pos);
	if (t->nowait_short && (iocb->ki_flags & IOCB_NOWAIT))
		n = min(n, t->nowait_short);
	n = copy_from_iter(t->wdata + pos, n, from);
	iocb->ki_pos += n;
	if (t->mode == RTCN_ASYNC && !is_sync_kiocb(iocb)) {
		t->pending = iocb;
		t->res = n;
		return -EIOCBQUEUED;
	}
	return n;
}

static const struct file_operations rtcn_fops = {
	.read_iter	= rtcn_read_iter,
	.write_iter	= rtcn_write_iter,
};

static int rtcn_init_fs_context(struct fs_context *fc)
{
	return init_pseudo(fc, ANON_INODE_FS_MAGIC) ? 0 : -ENOMEM;
}

static struct file_system_type rtcn_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "nfsd_rtc_kunit",
	.init_fs_context = rtcn_init_fs_context,
	.kill_sb	= kill_anon_super,
};

static int rtcn_result_payload(struct svc_rqst *rqstp, unsigned int offset,
			       unsigned int length)
{
	return 0;
}

static const struct svc_xprt_ops rtcn_xprt_ops = {
	.xpo_result_payload	= rtcn_result_payload,
};

static const struct svc_version *rtcn_versions[4] = {
	[3] = &nfsd_version3,
};

static struct svc_program rtcn_program = {
	.pg_prog	= RTCN_NFS_PROGRAM,
	.pg_lovers	= 3,
	.pg_hivers	= 3,
	.pg_nvers	= ARRAY_SIZE(rtcn_versions),
	.pg_vers	= rtcn_versions,
	.pg_name	= "nfsd-rtc-kunit",
	.pg_class	= "nfsd",
};

static void rtcn_counts(unsigned long *hits, unsigned long *acq)
{
	nfsd_file_kunit_counts(hits, acq);
}

static struct svc_rqst *rtcn_rqst(struct kunit *test, struct rtcn *t)
{
	struct nfsd_thread_local_info *ntli;
	struct nfsd_rtc_ctx *c;
	struct svc_rqst *r;
	unsigned int i;

	r = svc_rqst_alloc(t->serv, &t->serv->sv_pools[0], NUMA_NO_NODE);
	KUNIT_ASSERT_NOT_NULL(test, r);
	KUNIT_ASSERT_GT(test, r->rq_maxpages, RTCN_PAGES);
	for (i = 0; i < RTCN_PAGES; i++) {
		r->rq_respages[i] = alloc_page(GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, r->rq_respages[i]);
	}
	r->rq_page_end = &r->rq_respages[RTCN_PAGES];
	ntli = kunit_kzalloc(test, sizeof(*ntli), GFP_KERNEL);
	c = kunit_kzalloc(test, sizeof(*c), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ntli);
	KUNIT_ASSERT_NOT_NULL(test, c);
	ntli->ntli_cachetype = RC_NOCACHE;
	c->rqstp = r;
	r->rq_private = ntli;
	r->rq_rtc_priv = c;
	r->rq_xprt = &t->xprt;
	r->rq_prog = RTCN_NFS_PROGRAM;
	r->rq_vers = 3;
	r->rq_proc = NFS3PROC_READ;
	r->rq_procinfo = &nfsd_version3.vs_proc[NFS3PROC_READ];
	return r;
}

static int rtcn_init(struct kunit *test)
{
	struct rtcn *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	struct inode *inode;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, t);
	t->test = test;
	test->priv = t;
	t->data = kunit_kmalloc(test, RTCN_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t->data);
	for (i = 0; i < RTCN_SIZE; i++)
		t->data[i] = (u8)(i * 31U + 7U);

	t->mnt = kern_mount(&rtcn_fs_type);
	KUNIT_ASSERT_FALSE(test, IS_ERR(t->mnt));
	inode = alloc_anon_inode(t->mnt->mnt_sb);
	KUNIT_ASSERT_FALSE(test, IS_ERR(inode));
	inode->i_mode = S_IFREG | 0644;
	i_size_write(inode, RTCN_SIZE);
	t->file = alloc_file_pseudo(inode, t->mnt, "nfsd-rtc-kunit", O_RDWR,
				    &rtcn_fops);
	KUNIT_ASSERT_FALSE(test, IS_ERR(t->file));
	t->file->private_data = t;
	t->file->f_mode |= FMODE_NOWAIT;

	/* What nfsd_file_lookup_cached() returns: a constructed file. */
	refcount_set(&t->nf.nf_ref, 1);
	t->nf.nf_file = t->file;
	t->nf.nf_inode = inode;
	t->nf.nf_net = &init_net;
	t->nf.nf_may = NFSD_MAY_READ;
	t->nf.nf_dio_mem_align = RTCN_ALIGN;
	t->nf.nf_dio_offset_align = RTCN_ALIGN;
	t->nf.nf_dio_read_offset_align = RTCN_ALIGN;

	/* No wcc: post_op_attr is "absent" on both paths. */
	fh_init(&t->fh, NFS3_FHSIZE);
	t->fh.fh_dentry = t->file->f_path.dentry;
	t->fh.fh_no_wcc = true;

	t->serv = svc_create(&rtcn_program, 65536, NULL);
	KUNIT_ASSERT_NOT_NULL(test, t->serv);
	t->xprt.xpt_ops = &rtcn_xprt_ops;
	t->xprt.xpt_net = &init_net;
	t->rq[0] = rtcn_rqst(test, t);
	t->rq[1] = rtcn_rqst(test, t);

	t->saved_io_cache_read = nfsd_io_cache_read;
	nfsd_io_cache_read = NFSD_IO_DIRECT;

	/* WRITE: a sync export, a page-aligned payload, a write buffer. */
	t->wdata = kunit_kzalloc(test, RTCN_SIZE, GFP_KERNEL);
	t->exp = kunit_kzalloc(test, sizeof(*t->exp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t->wdata);
	KUNIT_ASSERT_NOT_NULL(test, t->exp);
	/*
	 * WRITE3res wcc_data: a saved post-op attr, so both paths encode
	 * the same bytes without fh_getattr() (this fs has no export ops).
	 */
	t->exp->ex_path = t->file->f_path;
	KUNIT_ASSERT_EQ(test, vfs_getattr(&t->file->f_path,
					  &t->fh.fh_post_attr,
					  STATX_BASIC_STATS,
					  AT_STATX_SYNC_AS_STAT), 0);
	for (i = 0; i < RTCN_PAGES; i++) {
		unsigned int j;
		u8 *a;

		t->wpages[i] = alloc_page(GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, t->wpages[i]);
		a = page_address(t->wpages[i]);
		for (j = 0; j < PAGE_SIZE; j++)
			a[j] = (u8)(i * 13U + j * 5U + 3U);
	}
	t->saved_io_cache_write = nfsd_io_cache_write;
	nfsd_io_cache_write = NFSD_IO_DIRECT;
	return 0;
}

static void rtcn_exit(struct kunit *test)
{
	struct rtcn *t = test->priv;
	unsigned int i;

	if (!t)
		return;
	nfsd_io_cache_read = t->saved_io_cache_read;
	if (t->wdata)
		nfsd_io_cache_write = t->saved_io_cache_write;
	for (i = 0; i < RTCN_PAGES; i++)
		if (t->wpages[i])
			__free_page(t->wpages[i]);
	for (i = 0; i < 2; i++) {
		if (!t->rq[i])
			continue;
		t->rq[i]->rq_xprt = NULL;
		t->rq[i]->rq_private = NULL;
		t->rq[i]->rq_rtc_priv = NULL;
		svc_rqst_free(t->rq[i]);
	}
	if (t->serv)
		svc_destroy(&t->serv);
	if (!IS_ERR_OR_NULL(t->file))
		fput(t->file);
	if (!IS_ERR_OR_NULL(t->mnt)) {
		flush_delayed_fput();
		kern_unmount(t->mnt);
	}
}

/* svc_process()'s reply setup, then nfsd3_proc_read() up to the read. */
static struct nfsd3_readres *rtcn_prep(struct rtcn *t, unsigned int i,
				       bool try, u32 count)
{
	struct svc_rqst *r = t->rq[i];
	struct nfsd3_readres *resp = r->rq_resp;
	struct kvec *resv = &r->rq_res.head[0];

	clear_bit(RQ_RTC_TRY, &r->rq_flags);
	clear_bit(RQ_RTC_ASYNC, &r->rq_flags);
	clear_bit(RQ_RTC_MISS, &r->rq_flags);
	r->rq_rtc_reason = 0;
	r->rq_next_page = &r->rq_respages[1];
	resv->iov_base = page_address(r->rq_respages[0]);
	resv->iov_len = 0;
	r->rq_res.pages = r->rq_next_page;
	r->rq_res.len = 0;
	r->rq_res.page_base = 0;
	r->rq_res.page_len = 0;
	r->rq_res.buflen = PAGE_SIZE;
	r->rq_res.tail[0].iov_base = NULL;
	r->rq_res.tail[0].iov_len = 0;
	svcxdr_init_encode(r);
	memset(resp, 0, sizeof(*resp));
	resp->pages = r->rq_next_page;
	resp->count = count;
	resp->fh = t->fh;	/* fh_copy() would warn: fh_dentry is set */
	if (try)
		set_bit(RQ_RTC_TRY, &r->rq_flags);
	t->calls = 0;
	t->flags = 0;
	t->pending = NULL;
	return resp;
}

static void rtcn_capture(struct rtcn *t, unsigned int i)
{
	struct xdr_buf *buf = &t->rq[i]->rq_res;

	t->reply_len[i] = buf->len;
	t->reply[i] = kunit_kzalloc(t->test, buf->len + 1, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(t->test, t->reply[i]);
	KUNIT_ASSERT_EQ(t->test,
			read_bytes_from_xdr_buf(buf, 0, t->reply[i], buf->len), 0);
}

/* The thread path's READ3res for the same READ, into reply[0]. */
static void rtcn_thread_ref(struct rtcn *t, loff_t offset, u32 count)
{
	struct nfsd3_readres *resp = rtcn_prep(t, 0, false, count);

	resp->status = nfsd_iter_read(t->rq[0], &resp->fh, &t->nf, offset,
				      &resp->count, 0, &resp->eof);
	KUNIT_ASSERT_EQ(t->test, resp->status, nfs_ok);
	KUNIT_EXPECT_EQ(t->test, t->calls, 1);
	KUNIT_EXPECT_TRUE(t->test, t->flags & IOCB_DIRECT);
	KUNIT_ASSERT_TRUE(t->test, nfs3svc_encode_readres(t->rq[0],
					&t->rq[0]->rq_res_stream));
	rtcn_capture(t, 0);
}

/* reply[1] is the thread path's reply and carries the file's bytes. */
static void rtcn_expect_reply(struct rtcn *t, loff_t offset, u32 count,
			      bool eof)
{
	struct kunit *test = t->test;
	__be32 *p;

	KUNIT_ASSERT_EQ(test, t->reply_len[0], t->reply_len[1]);
	KUNIT_EXPECT_EQ(test, memcmp(t->reply[0], t->reply[1], t->reply_len[0]), 0);
	KUNIT_ASSERT_GE(test, t->reply_len[1], RTCN_HDR + count);
	p = (__be32 *)t->reply[1];
	KUNIT_EXPECT_EQ(test, be32_to_cpu(p[0]), 0);		/* NFS3_OK */
	KUNIT_EXPECT_EQ(test, be32_to_cpu(p[1]), 0);		/* no attrs */
	KUNIT_EXPECT_EQ(test, be32_to_cpu(p[2]), count);
	KUNIT_EXPECT_EQ(test, be32_to_cpu(p[3]), eof ? 1 : 0);
	KUNIT_EXPECT_EQ(test, be32_to_cpu(p[4]), count);
	KUNIT_EXPECT_EQ(test, memcmp(t->reply[1] + RTCN_HDR, t->data + offset,
				     count), 0);
}

/* What nfsd_file_lookup_cached() hands nfsd_rtc_read_file(): a reference. */
static struct nfsd_file *rtcn_get(struct rtcn *t)
{
	refcount_inc(&t->nf.nf_ref);
	return &t->nf;
}

/* Every fast-path outcome puts the lookup's reference exactly once. */
static void rtcn_expect_ref_back(struct rtcn *t)
{
	KUNIT_EXPECT_EQ(t->test, refcount_read(&t->nf.nf_ref), 1);
}

/* A synchronous hit: the thread path's reply, counted once. */
static void nfsd_rtc_sync_hit(struct kunit *test)
{
	struct rtcn *t = test->priv;
	/* Unaligned start and a short read at EOF: pad and eof are mapped. */
	loff_t offset = PAGE_SIZE + 1000;
	u32 want = 8192, count = RTCN_SIZE - offset;
	unsigned long h0, a0, h1, a1;
	struct nfsd3_readres *resp;
	struct svc_rqst *r = t->rq[1];

	rtcn_thread_ref(t, offset, want);
	resp = rtcn_prep(t, 1, true, want);
	rtcn_counts(&h0, &a0);
	resp->status = nfsd_rtc_read_file(r, &resp->fh, rtcn_get(t), offset,
					  &resp->count, &resp->eof);
	rtcn_counts(&h1, &a1);
	KUNIT_EXPECT_FALSE(test, test_bit(RQ_RTC_ASYNC, &r->rq_flags));
	KUNIT_EXPECT_FALSE(test, test_bit(RQ_RTC_MISS, &r->rq_flags));
	KUNIT_EXPECT_EQ(test, resp->status, nfs_ok);
	KUNIT_EXPECT_EQ(test, t->calls, 1);
	KUNIT_EXPECT_EQ(test, t->flags & (IOCB_DIRECT | IOCB_NOWAIT),
			IOCB_DIRECT | IOCB_NOWAIT);
	KUNIT_EXPECT_EQ(test, h1 - h0, 1);
	KUNIT_EXPECT_EQ(test, a1 - a0, 1);
	rtcn_expect_ref_back(t);
	KUNIT_ASSERT_TRUE(test, nfs3svc_encode_readres(r, &r->rq_res_stream));
	rtcn_capture(t, 1);
	rtcn_expect_reply(t, offset, count, true);
}

/*
 * An async hit: no count until nfsd3_rtc_complete(), which encodes the
 * thread path's reply through nfsd_rtc_dispatch_finish().  The test
 * records the result as nfsd_rtc_read_complete() does; its queueing half,
 * svc_rtc_complete(), is covered by the sunrpc rtc e2e cases.
 */
static void nfsd_rtc_async_complete(struct kunit *test)
{
	struct rtcn *t = test->priv;
	loff_t offset = RTCN_ALIGN;
	u32 count = 2 * PAGE_SIZE;
	unsigned long h0, a0, h1, a1;
	struct nfsd3_readres *resp;
	struct svc_rqst *r = t->rq[1];
	struct nfsd_rtc_ctx *c = r->rq_rtc_priv;

	rtcn_thread_ref(t, offset, count);
	t->mode = RTCN_ASYNC;
	resp = rtcn_prep(t, 1, true, count);
	rtcn_counts(&h0, &a0);
	nfsd_rtc_read_file(r, &resp->fh, rtcn_get(t), offset, &resp->count,
			   &resp->eof);
	KUNIT_ASSERT_TRUE(test, test_bit(RQ_RTC_ASYNC, &r->rq_flags));
	KUNIT_EXPECT_FALSE(test, test_bit(RQ_RTC_MISS, &r->rq_flags));
	KUNIT_ASSERT_PTR_EQ(test, t->pending, &c->kiocb);
	KUNIT_EXPECT_NOT_NULL(test, c->kiocb.ki_complete);
	KUNIT_EXPECT_PTR_EQ(test, c->nf, &t->nf);
	/* The reference stays with the in-flight read. */
	KUNIT_EXPECT_EQ(test, refcount_read(&t->nf.nf_ref), 2);
	rtcn_counts(&h1, &a1);
	KUNIT_EXPECT_EQ(test, h1 - h0, 0);
	KUNIT_EXPECT_EQ(test, a1 - a0, 0);

	c->res = t->res;
	KUNIT_EXPECT_EQ(test, nfsd3_rtc_ops.complete(r), 1);
	rtcn_counts(&h1, &a1);
	KUNIT_EXPECT_EQ(test, h1 - h0, 1);
	KUNIT_EXPECT_EQ(test, a1 - a0, 1);
	KUNIT_EXPECT_NULL(test, c->nf);
	rtcn_expect_ref_back(t);
	rtcn_capture(t, 1);
	rtcn_expect_reply(t, offset, count, false);
}

/* The real lookup: fh_verify() rejects the handle, a thread reruns it. */
static void nfsd_rtc_file_miss(struct kunit *test)
{
	struct rtcn *t = test->priv;
	unsigned long h0, a0, h1, a1;
	struct nfsd3_readres *resp;
	struct svc_rqst *r = t->rq[1];
	struct svc_fh fh;

	resp = rtcn_prep(t, 1, true, PAGE_SIZE);
	fh_init(&fh, NFS3_FHSIZE);
	fh.fh_handle.fh_size = 8;
	fh.fh_handle.fh_version = 0;		/* nfserr_badhandle */
	rtcn_counts(&h0, &a0);
	KUNIT_EXPECT_EQ(test, nfsd_rtc_read(r, &fh, 0, &resp->count,
					    &resp->eof), nfs_ok);
	rtcn_counts(&h1, &a1);
	KUNIT_EXPECT_TRUE(test, test_bit(RQ_RTC_MISS, &r->rq_flags));
	KUNIT_EXPECT_FALSE(test, test_bit(RQ_RTC_ASYNC, &r->rq_flags));
	KUNIT_EXPECT_EQ(test, r->rq_rtc_reason, SVC_RTC_FB_FILE_MISS);
	KUNIT_EXPECT_EQ(test, t->calls, 0);
	KUNIT_EXPECT_EQ(test, h1 - h0, 0);
	KUNIT_EXPECT_EQ(test, a1 - a0, 0);
	rtcn_expect_ref_back(t);
}

/* What the thread path would not read direct (or with NOWAIT) is a miss. */
static void nfsd_rtc_unaligned(struct kunit *test)
{
	struct rtcn *t = test->priv;
	unsigned long h0, a0, h1, a1;
	struct nfsd3_readres *resp;
	struct svc_rqst *r = t->rq[1];
	unsigned int i;

	for (i = 0; i < 3; i++) {
		u32 count = i == 1 ? RTCN_ALIGN - 1 : PAGE_SIZE;

		t->nf.nf_dio_read_offset_align = i == 0 ? 0 : RTCN_ALIGN;
		if (i == 2)
			t->file->f_mode &= ~FMODE_NOWAIT;
		resp = rtcn_prep(t, 1, true, count);
		rtcn_counts(&h0, &a0);
		nfsd_rtc_read_file(r, &resp->fh, rtcn_get(t), 0, &resp->count,
				   &resp->eof);
		rtcn_counts(&h1, &a1);
		KUNIT_EXPECT_TRUE(test, test_bit(RQ_RTC_MISS, &r->rq_flags));
		KUNIT_EXPECT_FALSE(test, test_bit(RQ_RTC_ASYNC, &r->rq_flags));
		KUNIT_EXPECT_EQ(test, r->rq_rtc_reason, SVC_RTC_FB_UNALIGNED);
		KUNIT_EXPECT_EQ(test, t->calls, 0);
		KUNIT_EXPECT_EQ(test, h1 - h0, 0);
		KUNIT_EXPECT_EQ(test, a1 - a0, 0);
		rtcn_expect_ref_back(t);
	}
	t->file->f_mode |= FMODE_NOWAIT;
	t->nf.nf_dio_read_offset_align = RTCN_ALIGN;
}

/* IOCB_NOWAIT submit says -EAGAIN: a miss, nothing counted, ref put. */
static void nfsd_rtc_eagain_submit(struct kunit *test)
{
	struct rtcn *t = test->priv;
	unsigned long h0, a0, h1, a1;
	struct nfsd3_readres *resp;
	struct svc_rqst *r = t->rq[1];
	struct nfsd_rtc_ctx *c = r->rq_rtc_priv;

	t->mode = RTCN_EAGAIN;
	resp = rtcn_prep(t, 1, true, PAGE_SIZE);
	rtcn_counts(&h0, &a0);
	nfsd_rtc_read_file(r, &resp->fh, rtcn_get(t), 0, &resp->count,
			   &resp->eof);
	rtcn_counts(&h1, &a1);
	KUNIT_EXPECT_TRUE(test, test_bit(RQ_RTC_MISS, &r->rq_flags));
	KUNIT_EXPECT_FALSE(test, test_bit(RQ_RTC_ASYNC, &r->rq_flags));
	KUNIT_EXPECT_EQ(test, r->rq_rtc_reason, SVC_RTC_FB_EAGAIN_SUBMIT);
	KUNIT_EXPECT_EQ(test, t->calls, 1);
	KUNIT_EXPECT_NULL(test, c->nf);
	KUNIT_EXPECT_EQ(test, h1 - h0, 0);
	KUNIT_EXPECT_EQ(test, a1 - a0, 0);
	rtcn_expect_ref_back(t);
	/* The thread rerun (no IOCB_NOWAIT) serves the same READ. */
	rtcn_thread_ref(t, 0, PAGE_SIZE);
}

/* The async read completes -EAGAIN: abandoned, nothing encoded or counted. */
static void nfsd_rtc_eagain_complete(struct kunit *test)
{
	struct rtcn *t = test->priv;
	unsigned long h0, a0, h1, a1;
	struct nfsd3_readres *resp;
	struct svc_rqst *r = t->rq[1];
	struct nfsd_rtc_ctx *c = r->rq_rtc_priv;

	t->mode = RTCN_ASYNC;
	resp = rtcn_prep(t, 1, true, PAGE_SIZE);
	rtcn_counts(&h0, &a0);
	nfsd_rtc_read_file(r, &resp->fh, rtcn_get(t), 0, &resp->count,
			   &resp->eof);
	KUNIT_ASSERT_TRUE(test, test_bit(RQ_RTC_ASYNC, &r->rq_flags));
	c->res = -EAGAIN;
	KUNIT_EXPECT_EQ(test, nfsd3_rtc_ops.complete(r), -EAGAIN);
	rtcn_counts(&h1, &a1);
	KUNIT_EXPECT_EQ(test, h1 - h0, 0);
	KUNIT_EXPECT_EQ(test, a1 - a0, 0);
	KUNIT_EXPECT_NULL(test, c->nf);
	KUNIT_EXPECT_EQ(test, r->rq_res.len, 0);
	rtcn_expect_ref_back(t);
}

/* svc_process()'s reply setup, then nfsd3_proc_write() up to the write. */
static struct nfsd3_writeres *rtcn_wprep(struct rtcn *t, unsigned int i,
					 bool try, u32 count, int stable)
{
	struct svc_rqst *r = t->rq[i];
	struct nfsd3_writeres *resp;

	r->rq_proc = NFS3PROC_WRITE;
	r->rq_procinfo = &nfsd_version3.vs_proc[NFS3PROC_WRITE];
	rtcn_prep(t, i, try, 0);
	resp = r->rq_resp;
	memset(resp, 0, sizeof(*resp));
	resp->fh = t->fh;
	resp->fh.fh_export = t->exp;
	resp->fh.fh_post_saved = true;
	resp->committed = stable;
	memset(&t->payload, 0, sizeof(t->payload));
	t->payload.pages = t->wpages;
	t->payload.page_len = count;
	t->payload.len = count;
	t->payload.buflen = count;
	memset(t->wdata, 0, RTCN_SIZE);
	return resp;
}

/* nfsd3_iocb_flags() */
static int rtcn_iocb_flags(int stable)
{
	return stable == NFS_FILE_SYNC ? IOCB_DSYNC | IOCB_SYNC :
	       stable == NFS_DATA_SYNC ? IOCB_DSYNC : 0;
}

/* The thread path's WRITE3res for the same WRITE, into reply[0]. */
static void rtcn_wthread_ref(struct rtcn *t, loff_t offset, u32 count,
			     int stable)
{
	struct nfsd3_writeres *resp = rtcn_wprep(t, 0, false, count, stable);
	unsigned long cnt = count;

	resp->status = nfsd_vfs_write(t->rq[0], &resp->fh, &t->nf, offset,
				      &t->payload, &cnt,
				      rtcn_iocb_flags(stable), resp->verf);
	resp->count = cnt;
	KUNIT_ASSERT_EQ(t->test, resp->status, nfs_ok);
	KUNIT_EXPECT_EQ(t->test, t->calls, 1);
	KUNIT_EXPECT_TRUE(t->test, t->flags & IOCB_DIRECT);
	KUNIT_ASSERT_TRUE(t->test, nfs3svc_encode_writeres(t->rq[0],
					&t->rq[0]->rq_res_stream));
	rtcn_capture(t, 0);
}

/* reply[1] is the thread path's reply; the file holds the payload. */
static void rtcn_wexpect_reply(struct rtcn *t, loff_t offset, u32 count,
			       int stable)
{
	struct kunit *test = t->test;
	unsigned int k;
	__be32 *p;

	KUNIT_ASSERT_EQ(test, t->reply_len[0], t->reply_len[1]);
	KUNIT_EXPECT_EQ(test, memcmp(t->reply[0], t->reply[1], t->reply_len[0]), 0);
	/* status, wcc_data, count, committed, verf */
	KUNIT_ASSERT_GE(test, t->reply_len[1], 7 * XDR_UNIT);
	p = (__be32 *)t->reply[1];
	KUNIT_EXPECT_EQ(test, be32_to_cpu(p[0]), 0);
	p = (__be32 *)(t->reply[1] + t->reply_len[1]) - 4;
	KUNIT_EXPECT_EQ(test, be32_to_cpu(p[0]), count);
	KUNIT_EXPECT_EQ(test, be32_to_cpu(p[1]), stable);
	for (k = 0; k < count / PAGE_SIZE; k++)
		KUNIT_EXPECT_EQ(test, memcmp(t->wdata + offset + k * PAGE_SIZE,
					     page_address(t->wpages[k]),
					     PAGE_SIZE), 0);
}

/* A synchronous WRITE hit, UNSTABLE: the thread path's reply, counted once. */
static void nfsd_rtc_write_sync_hit(struct kunit *test)
{
	struct rtcn *t = test->priv;
	u32 count = 2 * PAGE_SIZE;
	unsigned long h0, a0, h1, a1, cnt = count;
	struct nfsd3_writeres *resp;
	struct svc_rqst *r = t->rq[1];

	rtcn_wthread_ref(t, 0, count, NFS_UNSTABLE);
	resp = rtcn_wprep(t, 1, true, count, NFS_UNSTABLE);
	rtcn_counts(&h0, &a0);
	resp->status = nfsd_rtc_write_file(r, &resp->fh, rtcn_get(t), 0,
					   &t->payload, &cnt, 0, resp->verf);
	resp->count = cnt;
	rtcn_counts(&h1, &a1);
	KUNIT_EXPECT_FALSE(test, test_bit(RQ_RTC_ASYNC, &r->rq_flags));
	KUNIT_EXPECT_FALSE(test, test_bit(RQ_RTC_MISS, &r->rq_flags));
	KUNIT_EXPECT_EQ(test, resp->status, nfs_ok);
	KUNIT_EXPECT_EQ(test, t->calls, 1);
	KUNIT_EXPECT_EQ(test, t->flags & (IOCB_DIRECT | IOCB_NOWAIT | IOCB_DSYNC),
			IOCB_DIRECT | IOCB_NOWAIT);
	KUNIT_EXPECT_EQ(test, h1 - h0, 1);
	KUNIT_EXPECT_EQ(test, a1 - a0, 1);
	rtcn_expect_ref_back(t);
	KUNIT_ASSERT_TRUE(test, nfs3svc_encode_writeres(r, &r->rq_res_stream));
	rtcn_capture(t, 1);
	rtcn_wexpect_reply(t, 0, count, NFS_UNSTABLE);
}

/* An async WRITE hit, finished by nfsd3_rtc_complete() on the work item. */
static void rtcn_write_async(struct kunit *test, int stable)
{
	struct rtcn *t = test->priv;
	u32 count = 2 * PAGE_SIZE;
	loff_t offset = PAGE_SIZE;
	unsigned long h0, a0, h1, a1, cnt = count;
	struct nfsd3_writeres *resp;
	struct svc_rqst *r = t->rq[1];
	struct nfsd_rtc_ctx *c = r->rq_rtc_priv;

	rtcn_wthread_ref(t, offset, count, stable);
	t->mode = RTCN_ASYNC;
	resp = rtcn_wprep(t, 1, true, count, stable);
	rtcn_counts(&h0, &a0);
	nfsd_rtc_write_file(r, &resp->fh, rtcn_get(t), offset, &t->payload,
			    &cnt, rtcn_iocb_flags(stable), resp->verf);
	KUNIT_ASSERT_TRUE(test, test_bit(RQ_RTC_ASYNC, &r->rq_flags));
	KUNIT_EXPECT_FALSE(test, test_bit(RQ_RTC_MISS, &r->rq_flags));
	KUNIT_ASSERT_PTR_EQ(test, t->pending, &c->kiocb);
	/* The client's stability rides on the one direct NOWAIT kiocb. */
	KUNIT_EXPECT_EQ(test, t->flags & (IOCB_DIRECT | IOCB_NOWAIT |
					  IOCB_DSYNC | IOCB_SYNC),
			IOCB_DIRECT | IOCB_NOWAIT | rtcn_iocb_flags(stable));
	KUNIT_EXPECT_EQ(test, refcount_read(&t->nf.nf_ref), 2);
	rtcn_counts(&h1, &a1);
	KUNIT_EXPECT_EQ(test, h1 - h0, 0);

	c->res = t->res;
	KUNIT_EXPECT_EQ(test, nfsd3_rtc_ops.complete(r), 1);
	rtcn_counts(&h1, &a1);
	KUNIT_EXPECT_EQ(test, h1 - h0, 1);
	KUNIT_EXPECT_EQ(test, a1 - a0, 1);
	KUNIT_EXPECT_NULL(test, c->nf);
	rtcn_expect_ref_back(t);
	rtcn_capture(t, 1);
	rtcn_wexpect_reply(t, offset, count, stable);
}

static void nfsd_rtc_write_async_complete(struct kunit *test)
{
	rtcn_write_async(test, NFS_UNSTABLE);
}

/* DATA_SYNC and FILE_SYNC: IOCB_DSYNC (and IOCB_SYNC) on the async kiocb. */
static void nfsd_rtc_write_stable(struct kunit *test)
{
	rtcn_write_async(test, NFS_DATA_SYNC);
	rtcn_write_async(test, NFS_FILE_SYNC);
}

/* IOCB_NOWAIT write submit says -EAGAIN: a miss, nothing written or counted. */
static void nfsd_rtc_write_eagain_submit(struct kunit *test)
{
	struct rtcn *t = test->priv;
	unsigned long h0, a0, h1, a1, cnt = PAGE_SIZE;
	struct nfsd3_writeres *resp;
	struct svc_rqst *r = t->rq[1];
	struct nfsd_rtc_ctx *c = r->rq_rtc_priv;

	t->mode = RTCN_EAGAIN;
	resp = rtcn_wprep(t, 1, true, PAGE_SIZE, NFS_FILE_SYNC);
	rtcn_counts(&h0, &a0);
	nfsd_rtc_write_file(r, &resp->fh, rtcn_get(t), 0, &t->payload, &cnt,
			    rtcn_iocb_flags(NFS_FILE_SYNC), resp->verf);
	rtcn_counts(&h1, &a1);
	KUNIT_EXPECT_TRUE(test, test_bit(RQ_RTC_MISS, &r->rq_flags));
	KUNIT_EXPECT_FALSE(test, test_bit(RQ_RTC_ASYNC, &r->rq_flags));
	KUNIT_EXPECT_EQ(test, r->rq_rtc_reason, SVC_RTC_FB_EAGAIN_SUBMIT);
	KUNIT_EXPECT_EQ(test, t->calls, 1);
	KUNIT_EXPECT_NULL(test, c->nf);
	KUNIT_EXPECT_EQ(test, h1 - h0, 0);
	KUNIT_EXPECT_EQ(test, a1 - a0, 0);
	KUNIT_EXPECT_NULL(test, memchr_inv(t->wdata, 0, RTCN_SIZE));
	rtcn_expect_ref_back(t);
	/* The thread rerun (no IOCB_NOWAIT) is the first write. */
	rtcn_wthread_ref(t, 0, PAGE_SIZE, NFS_FILE_SYNC);
}

/* The async write completes -EAGAIN: abandoned, nothing encoded or counted. */
static void nfsd_rtc_write_eagain_complete(struct kunit *test)
{
	struct rtcn *t = test->priv;
	unsigned long h0, a0, h1, a1, cnt = PAGE_SIZE;
	struct nfsd3_writeres *resp;
	struct svc_rqst *r = t->rq[1];
	struct nfsd_rtc_ctx *c = r->rq_rtc_priv;

	t->mode = RTCN_ASYNC;
	resp = rtcn_wprep(t, 1, true, PAGE_SIZE, NFS_UNSTABLE);
	rtcn_counts(&h0, &a0);
	nfsd_rtc_write_file(r, &resp->fh, rtcn_get(t), 0, &t->payload, &cnt,
			    0, resp->verf);
	KUNIT_ASSERT_TRUE(test, test_bit(RQ_RTC_ASYNC, &r->rq_flags));
	c->res = -EAGAIN;
	KUNIT_EXPECT_EQ(test, nfsd3_rtc_ops.complete(r), -EAGAIN);
	rtcn_counts(&h1, &a1);
	KUNIT_EXPECT_EQ(test, h1 - h0, 0);
	KUNIT_EXPECT_EQ(test, a1 - a0, 0);
	KUNIT_EXPECT_NULL(test, c->nf);
	KUNIT_EXPECT_EQ(test, r->rq_res.len, 0);
	rtcn_expect_ref_back(t);
}

/* A DRC call header: what nfsd_cache_lookup() checksums and keys on. */
static void rtcn_drc_call(struct rtcn *t, struct svc_rqst *r, u32 xid,
			  bool try)
{
	struct sockaddr_in *sin = (struct sockaddr_in *)&r->rq_addr;

	rtcn_wprep(t, 1, try, PAGE_SIZE, NFS_UNSTABLE);
	r->rq_xid = cpu_to_be32(xid);
	r->rq_prot = IPPROTO_TCP;
	memset(&r->rq_addr, 0, sizeof(r->rq_addr));
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sin->sin_port = htons(700);
	r->rq_addrlen = sizeof(*sin);
}

/* A short encoded reply for nfsd_cache_update(): returns its statp. */
static __be32 *rtcn_drc_reply(struct svc_rqst *r)
{
	struct kvec *resv = &r->rq_res.head[0];
	__be32 *p = resv->iov_base;

	p[0] = 0;
	p[1] = cpu_to_be32(1);
	p[2] = cpu_to_be32(2);
	p[3] = cpu_to_be32(3);
	resv->iov_len = 4 * XDR_UNIT;
	return p;
}

/* The next xid after @xid whose DRC bucket is @xid's. */
static u32 rtcn_drc_same_bucket(struct nfsd_net *nn, u32 xid)
{
	u32 want = hash_32((__force u32)cpu_to_be32(xid), nn->maskbits);
	u32 x = xid + 1;

	while (hash_32((__force u32)cpu_to_be32(x), nn->maskbits) != want)
		x++;
	return x;
}

/*
 * DRC, with real nfsd_cache_lookup() entries on init_net: an abandoned
 * WRITE keeps its RC_INPROG entry for the thread rerun, a retransmission
 * meanwhile is dropped, pruning skips the kept entry while it is handed
 * off, the rerun's nfsd_cache_update() caches the reply exactly once, and
 * a hand-off no thread runs (svc_rtc_handoff_drain()) frees it exactly
 * once.  GFP_NOWAIT failures on the work item: no entry at lookup is a
 * counted no_mem miss; no reply buffer at update leaves the reply
 * uncached, as on a thread.
 *
 * The rerun's use of the kept entry inside nfsd_dispatch() (rp taken from
 * drc_rp, no second lookup) is not driven here: that needs a fully
 * decoded NFSv3 WRITE call in rq_arg.  The rerun is modelled by the
 * nfsd_cache_update() that nfsd_dispatch() then makes on that entry.
 */
static void nfsd_rtc_write_drc(struct kunit *test)
{
	struct rtcn *t = test->priv;
	struct svc_rqst *r = t->rq[1];
	struct nfsd_rtc_ctx *c = r->rq_rtc_priv;
	struct nfsd_thread_local_info *ntli = r->rq_private;
	struct nfsd_net *nn = nfsd_cache_kunit_nn(&init_net);
	struct nfsd_cacherep *a = NULL, *b = NULL, *cc = NULL, *d = NULL;
	struct nfsd_cacherep *rp;
	u8 *arg;
	u32 xa = 0x5000, xb, xc;
	int n;

	if (nn->drc_hashtbl)
		kunit_skip(test, "init_net DRC already in use");
	arg = kunit_kzalloc(test, 64, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, arg);
	memset(arg, 0x5a, 64);
	r->rq_arg.head[0].iov_base = arg;
	r->rq_arg.head[0].iov_len = 64;
	r->rq_arg.page_len = 0;
	r->rq_arg.tail[0].iov_len = 0;
	r->rq_arg.len = 64;
	KUNIT_ASSERT_EQ(test, nfsd_reply_cache_init(nn), 0);
	nn->max_drc_entries = 0;	/* every entry past its turn prunes */
	ntli->ntli_cachetype = RC_REPLBUFF;

	/* The rtc attempt (RQ_RTC_TRY) inserts A in progress. */
	rtcn_drc_call(t, r, xa, true);
	KUNIT_EXPECT_EQ(test, nfsd_cache_lookup(r, 0, 64, &a), RC_DOIT);
	KUNIT_ASSERT_NOT_NULL(test, a);
	KUNIT_EXPECT_EQ(test, a->c_state, RC_INPROG);
	KUNIT_EXPECT_EQ(test, atomic_read(&nn->num_drc_entries), 1);
	/* The NOWAIT write misses at completion: A is kept for the rerun. */
	c->rp = a;
	KUNIT_EXPECT_EQ(test, nfsd_rtc_dispatch_finish(r, true), 0);
	KUNIT_EXPECT_PTR_EQ(test, c->drc_rp, a);
	KUNIT_EXPECT_NULL(test, c->rp);
	KUNIT_EXPECT_EQ(test, r->rq_res.len, 0);
	set_bit(RQ_RTC_MISS, &r->rq_flags);
	nfsd3_rtc_ops.rqst_done(r);	/* a MISS hands off: A stays */
	KUNIT_EXPECT_PTR_EQ(test, c->drc_rp, a);

	/* A retransmission while A is handed off is dropped, as for a thread. */
	rtcn_drc_call(t, r, xa, false);
	rp = NULL;
	KUNIT_EXPECT_EQ(test, nfsd_cache_lookup(r, 0, 64, &rp), RC_DROPIT);
	KUNIT_EXPECT_NULL(test, rp);

	/* B, same bucket, done; C's lookup prunes B but skips in-progress A. */
	xb = rtcn_drc_same_bucket(nn, xa);
	xc = rtcn_drc_same_bucket(nn, xb);
	rtcn_drc_call(t, r, xb, false);
	KUNIT_EXPECT_EQ(test, nfsd_cache_lookup(r, 0, 64, &b), RC_DOIT);
	KUNIT_ASSERT_NOT_NULL(test, b);
	nfsd_cache_update(r, b, RC_REPLBUFF, rtcn_drc_reply(r));
	KUNIT_EXPECT_EQ(test, atomic_read(&nn->num_drc_entries), 2);
	rtcn_drc_call(t, r, xc, false);
	KUNIT_EXPECT_EQ(test, nfsd_cache_lookup(r, 0, 64, &cc), RC_DOIT);
	KUNIT_ASSERT_NOT_NULL(test, cc);
	KUNIT_EXPECT_EQ(test, atomic_read(&nn->num_drc_entries), 2);
	KUNIT_EXPECT_EQ(test, a->c_state, RC_INPROG);
	rtcn_drc_call(t, r, xa, false);
	rp = NULL;
	KUNIT_EXPECT_EQ(test, nfsd_cache_lookup(r, 0, 64, &rp), RC_DROPIT);

	/* The thread rerun answers on A: one cached reply, then replayed. */
	rtcn_drc_call(t, r, xa, false);
	rp = c->drc_rp;
	c->drc_rp = NULL;
	nfsd_cache_update(r, rp, RC_REPLBUFF, rtcn_drc_reply(r));
	KUNIT_EXPECT_EQ(test, a->c_state, RC_DONE);
	KUNIT_EXPECT_EQ(test, a->c_type, RC_REPLBUFF);
	rtcn_drc_call(t, r, xa, false);
	rp = NULL;
	KUNIT_EXPECT_EQ(test, nfsd_cache_lookup(r, 0, 64, &rp), RC_REPLY);
	nfsd_cache_update(r, cc, RC_NOCACHE, NULL);
	KUNIT_EXPECT_EQ(test, atomic_read(&nn->num_drc_entries), 1);

	/* A hand-off no thread runs: the drain's rqst_done frees D once. */
	rtcn_drc_call(t, r, xc + 1, true);
	KUNIT_EXPECT_EQ(test, nfsd_cache_lookup(r, 0, 64, &d), RC_DOIT);
	KUNIT_ASSERT_NOT_NULL(test, d);
	/* (D's insert may prune done A if they share a bucket.) */
	n = atomic_read(&nn->num_drc_entries);
	c->rp = d;
	KUNIT_EXPECT_EQ(test, nfsd_rtc_dispatch_finish(r, true), 0);
	KUNIT_EXPECT_PTR_EQ(test, c->drc_rp, d);
	clear_bit(RQ_RTC_MISS, &r->rq_flags);	/* svc_rtc_handoff_drain() */
	nfsd3_rtc_ops.rqst_done(r);
	KUNIT_EXPECT_NULL(test, c->drc_rp);
	KUNIT_EXPECT_EQ(test, atomic_read(&nn->num_drc_entries), n - 1);
	nfsd3_rtc_ops.rqst_done(r);
	KUNIT_EXPECT_EQ(test, atomic_read(&nn->num_drc_entries), n - 1);
	n--;

	/* No entry under GFP_NOWAIT: a counted miss, nothing inserted. */
	nfsd_cache_kunit_no_mem = true;
	rtcn_drc_call(t, r, xc + 2, true);
	rp = NULL;
	KUNIT_EXPECT_EQ(test, nfsd_cache_lookup(r, 0, 64, &rp), RC_DROPIT);
	KUNIT_EXPECT_NULL(test, rp);
	KUNIT_EXPECT_TRUE(test, test_bit(RQ_RTC_MISS, &r->rq_flags));
	KUNIT_EXPECT_EQ(test, r->rq_rtc_reason, SVC_RTC_FB_NO_MEM);
	KUNIT_EXPECT_EQ(test, atomic_read(&nn->num_drc_entries), n);
	/* The thread rerun allocates GFP_KERNEL: not injected. */
	rtcn_drc_call(t, r, xc + 2, false);
	KUNIT_EXPECT_EQ(test, nfsd_cache_lookup(r, 0, 64, &rp), RC_DOIT);
	KUNIT_ASSERT_NOT_NULL(test, rp);
	n = atomic_read(&nn->num_drc_entries);
	/* No reply buffer under GFP_NOWAIT: the reply is not cached. */
	set_bit(RQ_RTC_TRY, &r->rq_flags);
	nfsd_cache_update(r, rp, RC_REPLBUFF, rtcn_drc_reply(r));
	KUNIT_EXPECT_EQ(test, atomic_read(&nn->num_drc_entries), n - 1);
	nfsd_cache_kunit_no_mem = false;

	nfsd_reply_cache_shutdown(nn);
	ntli->ntli_cachetype = RC_NOCACHE;
	r->rq_arg.head[0].iov_base = NULL;
	r->rq_arg.head[0].iov_len = 0;
	r->rq_arg.len = 0;
	clear_bit(RQ_RTC_TRY, &r->rq_flags);
}

/* What nfsd_vfs_write() would do differently is a miss, nothing written. */
static void nfsd_rtc_write_unaligned(struct kunit *test)
{
	struct rtcn *t = test->priv;
	struct svc_rqst *r = t->rq[1];
	unsigned int i;

	for (i = 0; i < 4; i++) {
		unsigned long cnt = i == 1 ? RTCN_ALIGN - 1 : PAGE_SIZE;
		loff_t offset = i == 0 ? 100 : 0;
		struct nfsd3_writeres *resp;

		if (i == 2)
			t->file->f_mode &= ~FMODE_NOWAIT;
		resp = rtcn_wprep(t, 1, true, cnt, NFS_UNSTABLE);
		if (i == 3)
			resp->fh.fh_use_wgather = true;
		nfsd_rtc_write_file(r, &resp->fh, rtcn_get(t), offset,
				    &t->payload, &cnt, 0, resp->verf);
		KUNIT_EXPECT_TRUE(test, test_bit(RQ_RTC_MISS, &r->rq_flags));
		KUNIT_EXPECT_FALSE(test, test_bit(RQ_RTC_ASYNC, &r->rq_flags));
		KUNIT_EXPECT_EQ(test, r->rq_rtc_reason, i == 3 ?
				SVC_RTC_FB_WRITE_MODE : SVC_RTC_FB_UNALIGNED);
		KUNIT_EXPECT_EQ(test, t->calls, 0);
		rtcn_expect_ref_back(t);
	}
	t->file->f_mode |= FMODE_NOWAIT;
	/* io_cache_write not direct: nfsd_rtc_write() declines before lookup. */
	nfsd_io_cache_write = NFSD_IO_BUFFERED;
	{
		unsigned long cnt = PAGE_SIZE;

		rtcn_wprep(t, 1, true, cnt, NFS_UNSTABLE);
		nfsd_rtc_write(r, &t->fh, 0, &t->payload, &cnt, 0, NULL);
		KUNIT_EXPECT_EQ(test, r->rq_rtc_reason, SVC_RTC_FB_WRITE_MODE);
	}
	nfsd_io_cache_write = NFSD_IO_DIRECT;
}

/*
 * Any error of the NOWAIT read, at submit or completion, is a counted
 * miss; -EOPNOTSUPP (a !bdev_nowait() stack) and -EIO stand for the
 * errors that exist only because of IOCB_NOWAIT.  The rerun owns them.
 */
static void nfsd_rtc_read_nowait_errors(struct kunit *test)
{
	static const int errs[] = { -EOPNOTSUPP, -EIO };
	struct rtcn *t = test->priv;
	struct svc_rqst *r = t->rq[1];
	struct nfsd_rtc_ctx *c = r->rq_rtc_priv;
	unsigned long h0, a0, h1, a1;
	struct nfsd3_readres *resp;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(errs); i++) {
		t->mode = RTCN_SYNC;
		t->nowait_err = errs[i];
		resp = rtcn_prep(t, 1, true, PAGE_SIZE);
		rtcn_counts(&h0, &a0);
		nfsd_rtc_read_file(r, &resp->fh, rtcn_get(t), 0, &resp->count,
				   &resp->eof);
		rtcn_counts(&h1, &a1);
		KUNIT_EXPECT_TRUE(test, test_bit(RQ_RTC_MISS, &r->rq_flags));
		KUNIT_EXPECT_FALSE(test, test_bit(RQ_RTC_ASYNC, &r->rq_flags));
		KUNIT_EXPECT_EQ(test, r->rq_rtc_reason, SVC_RTC_FB_NOWAIT_ERR);
		KUNIT_EXPECT_EQ(test, t->calls, 1);
		KUNIT_EXPECT_NULL(test, c->nf);
		KUNIT_EXPECT_EQ(test, h1 - h0, 0);
		KUNIT_EXPECT_EQ(test, a1 - a0, 0);
		KUNIT_EXPECT_EQ(test, r->rq_res.len, 0);
		rtcn_expect_ref_back(t);
		/* The thread rerun (no IOCB_NOWAIT) serves the same READ. */
		rtcn_thread_ref(t, 0, PAGE_SIZE);

		t->nowait_err = 0;
		t->mode = RTCN_ASYNC;
		resp = rtcn_prep(t, 1, true, PAGE_SIZE);
		rtcn_counts(&h0, &a0);
		nfsd_rtc_read_file(r, &resp->fh, rtcn_get(t), 0, &resp->count,
				   &resp->eof);
		KUNIT_ASSERT_TRUE(test, test_bit(RQ_RTC_ASYNC, &r->rq_flags));
		c->res = errs[i];
		KUNIT_EXPECT_EQ(test, nfsd3_rtc_ops.complete(r), -EAGAIN);
		rtcn_counts(&h1, &a1);
		KUNIT_EXPECT_TRUE(test, test_bit(RQ_RTC_MISS, &r->rq_flags));
		KUNIT_EXPECT_EQ(test, r->rq_rtc_reason, SVC_RTC_FB_NOWAIT_ERR);
		KUNIT_EXPECT_EQ(test, h1 - h0, 0);
		KUNIT_EXPECT_EQ(test, a1 - a0, 0);
		KUNIT_EXPECT_NULL(test, c->nf);
		KUNIT_EXPECT_EQ(test, r->rq_res.len, 0);
		rtcn_expect_ref_back(t);
	}
	t->mode = RTCN_SYNC;
}

/* The thread path's WRITE3res for (0, PAGE_SIZE, UNSTABLE), kept. */
static u8 *rtcn_wbaseline(struct rtcn *t, unsigned int *len)
{
	u8 *base;

	rtcn_wthread_ref(t, 0, PAGE_SIZE, NFS_UNSTABLE);
	base = kunit_kmalloc(t->test, t->reply_len[0], GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(t->test, base);
	memcpy(base, t->reply[0], t->reply_len[0]);
	*len = t->reply_len[0];
	return base;
}

/* The thread rerun's reply is the baseline's: no verifier reset. */
static void rtcn_wexpect_rerun(struct rtcn *t, const u8 *base,
			       unsigned int len)
{
	rtcn_wthread_ref(t, 0, PAGE_SIZE, NFS_UNSTABLE);
	KUNIT_ASSERT_EQ(t->test, t->reply_len[0], len);
	KUNIT_EXPECT_EQ(t->test, memcmp(t->reply[0], base, len), 0);
}

/*
 * The NOWAIT write submit fails in a way that is not -EAGAIN, or writes
 * short: a counted nowait_err miss with no reply, no count, no verifier
 * reset; the rerun's reply is the thread path's.
 */
static void nfsd_rtc_write_nowait_err_submit(struct kunit *test)
{
	static const int errs[] = { -EOPNOTSUPP, -EINVAL, -EIO, 0 };
	struct rtcn *t = test->priv;
	struct svc_rqst *r = t->rq[1];
	struct nfsd_rtc_ctx *c = r->rq_rtc_priv;
	unsigned long h0, a0, h1, a1, cnt;
	struct nfsd3_writeres *resp;
	unsigned int i, len;
	u8 *base = rtcn_wbaseline(t, &len);

	for (i = 0; i < ARRAY_SIZE(errs); i++) {
		t->nowait_err = errs[i];
		t->nowait_short = errs[i] ? 0 : PAGE_SIZE / 2;
		cnt = PAGE_SIZE;
		resp = rtcn_wprep(t, 1, true, PAGE_SIZE, NFS_UNSTABLE);
		rtcn_counts(&h0, &a0);
		KUNIT_EXPECT_EQ(test, nfsd_rtc_write_file(r, &resp->fh,
				rtcn_get(t), 0, &t->payload, &cnt, 0,
				resp->verf), nfs_ok);
		rtcn_counts(&h1, &a1);
		KUNIT_EXPECT_TRUE(test, test_bit(RQ_RTC_MISS, &r->rq_flags));
		KUNIT_EXPECT_FALSE(test, test_bit(RQ_RTC_ASYNC, &r->rq_flags));
		KUNIT_EXPECT_EQ(test, r->rq_rtc_reason, SVC_RTC_FB_NOWAIT_ERR);
		KUNIT_EXPECT_EQ(test, t->calls, 1);
		KUNIT_EXPECT_NULL(test, c->nf);
		KUNIT_EXPECT_EQ(test, h1 - h0, 0);
		KUNIT_EXPECT_EQ(test, a1 - a0, 0);
		KUNIT_EXPECT_EQ(test, r->rq_res.len, 0);
		KUNIT_EXPECT_NULL(test, memchr_inv(t->wdata + t->nowait_short,
						   0, RTCN_SIZE - t->nowait_short));
		rtcn_expect_ref_back(t);
		t->nowait_err = 0;
		t->nowait_short = 0;
		rtcn_wexpect_rerun(t, base, len);
	}
}

/* As above, the async write completing so: abandoned, nothing encoded. */
static void nfsd_rtc_write_nowait_err_complete(struct kunit *test)
{
	static const long res[] = { -EOPNOTSUPP, -EINVAL, -EIO, PAGE_SIZE - 1 };
	struct rtcn *t = test->priv;
	struct svc_rqst *r = t->rq[1];
	struct nfsd_rtc_ctx *c = r->rq_rtc_priv;
	unsigned long h0, a0, h1, a1, cnt;
	struct nfsd3_writeres *resp;
	unsigned int i, len;
	u8 *base = rtcn_wbaseline(t, &len);

	for (i = 0; i < ARRAY_SIZE(res); i++) {
		t->mode = RTCN_ASYNC;
		cnt = PAGE_SIZE;
		resp = rtcn_wprep(t, 1, true, PAGE_SIZE, NFS_UNSTABLE);
		rtcn_counts(&h0, &a0);
		nfsd_rtc_write_file(r, &resp->fh, rtcn_get(t), 0, &t->payload,
				    &cnt, 0, resp->verf);
		KUNIT_ASSERT_TRUE(test, test_bit(RQ_RTC_ASYNC, &r->rq_flags));
		c->res = res[i];
		KUNIT_EXPECT_EQ(test, nfsd3_rtc_ops.complete(r), -EAGAIN);
		rtcn_counts(&h1, &a1);
		KUNIT_EXPECT_TRUE(test, test_bit(RQ_RTC_MISS, &r->rq_flags));
		KUNIT_EXPECT_EQ(test, r->rq_rtc_reason, SVC_RTC_FB_NOWAIT_ERR);
		KUNIT_EXPECT_EQ(test, h1 - h0, 0);
		KUNIT_EXPECT_EQ(test, a1 - a0, 0);
		KUNIT_EXPECT_NULL(test, c->nf);
		KUNIT_EXPECT_EQ(test, r->rq_res.len, 0);
		rtcn_expect_ref_back(t);
		t->mode = RTCN_SYNC;
		rtcn_wexpect_rerun(t, base, len);
	}
}

/*
 * A frozen superblock: sb_start_write_trylock() fails on the work item,
 * a counted sb_frozen miss before ->write_iter (no sleep in
 * sb_start_write() on the connection's work item).
 */
static void nfsd_rtc_write_frozen(struct kunit *test)
{
	struct rtcn *t = test->priv;
	struct super_block *sb = file_inode(t->file)->i_sb;
	struct svc_rqst *r = t->rq[1];
	struct nfsd_rtc_ctx *c = r->rq_rtc_priv;
	unsigned long h0, a0, h1, a1, cnt = PAGE_SIZE;
	struct nfsd3_writeres *resp;

	KUNIT_ASSERT_EQ(test, freeze_super(sb, FREEZE_HOLDER_KERNEL, NULL), 0);
	resp = rtcn_wprep(t, 1, true, PAGE_SIZE, NFS_FILE_SYNC);
	rtcn_counts(&h0, &a0);
	nfsd_rtc_write_file(r, &resp->fh, rtcn_get(t), 0, &t->payload, &cnt,
			    rtcn_iocb_flags(NFS_FILE_SYNC), resp->verf);
	rtcn_counts(&h1, &a1);
	KUNIT_EXPECT_EQ(test, thaw_super(sb, FREEZE_HOLDER_KERNEL, NULL), 0);
	KUNIT_EXPECT_TRUE(test, test_bit(RQ_RTC_MISS, &r->rq_flags));
	KUNIT_EXPECT_FALSE(test, test_bit(RQ_RTC_ASYNC, &r->rq_flags));
	KUNIT_EXPECT_EQ(test, r->rq_rtc_reason, SVC_RTC_FB_SB_FROZEN);
	KUNIT_EXPECT_EQ(test, t->calls, 0);
	KUNIT_EXPECT_NULL(test, c->nf);
	KUNIT_EXPECT_EQ(test, h1 - h0, 0);
	KUNIT_EXPECT_EQ(test, a1 - a0, 0);
	KUNIT_EXPECT_NULL(test, memchr_inv(t->wdata, 0, RTCN_SIZE));
	rtcn_expect_ref_back(t);
	/* Thawed, the thread rerun is the first write. */
	rtcn_wthread_ref(t, 0, PAGE_SIZE, NFS_FILE_SYNC);
}

/*
 * The WRITE declines not covered above: a local (loopback) client, a
 * file-cache miss, O_APPEND, and a payload the split would bounce.
 */
static void nfsd_rtc_write_fallbacks(struct kunit *test)
{
	struct rtcn *t = test->priv;
	struct svc_rqst *r = t->rq[1];
	unsigned long h0, a0, h1, a1, cnt;
	struct nfsd3_writeres *resp;
	struct svc_fh fh;

	cnt = PAGE_SIZE;
	rtcn_wprep(t, 1, true, PAGE_SIZE, NFS_UNSTABLE);
	set_bit(RQ_LOCAL, &r->rq_flags);
	KUNIT_EXPECT_EQ(test, nfsd_rtc_write(r, &t->fh, 0, &t->payload, &cnt,
					     0, NULL), nfs_ok);
	clear_bit(RQ_LOCAL, &r->rq_flags);
	KUNIT_EXPECT_EQ(test, r->rq_rtc_reason, SVC_RTC_FB_WRITE_MODE);
	KUNIT_EXPECT_EQ(test, t->calls, 0);

	cnt = PAGE_SIZE;
	rtcn_wprep(t, 1, true, PAGE_SIZE, NFS_UNSTABLE);
	fh_init(&fh, NFS3_FHSIZE);
	fh.fh_handle.fh_size = 8;
	fh.fh_handle.fh_version = 0;		/* nfserr_badhandle */
	rtcn_counts(&h0, &a0);
	KUNIT_EXPECT_EQ(test, nfsd_rtc_write(r, &fh, 0, &t->payload, &cnt, 0,
					     NULL), nfs_ok);
	rtcn_counts(&h1, &a1);
	KUNIT_EXPECT_TRUE(test, test_bit(RQ_RTC_MISS, &r->rq_flags));
	KUNIT_EXPECT_EQ(test, r->rq_rtc_reason, SVC_RTC_FB_FILE_MISS);
	KUNIT_EXPECT_EQ(test, t->calls, 0);
	KUNIT_EXPECT_EQ(test, h1 - h0, 0);
	KUNIT_EXPECT_EQ(test, a1 - a0, 0);
	rtcn_expect_ref_back(t);

	t->file->f_flags |= O_APPEND;
	cnt = PAGE_SIZE;
	resp = rtcn_wprep(t, 1, true, PAGE_SIZE, NFS_UNSTABLE);
	nfsd_rtc_write_file(r, &resp->fh, rtcn_get(t), 0, &t->payload, &cnt,
			    0, resp->verf);
	t->file->f_flags &= ~O_APPEND;
	KUNIT_EXPECT_EQ(test, r->rq_rtc_reason, SVC_RTC_FB_UNALIGNED);
	KUNIT_EXPECT_EQ(test, t->calls, 0);
	rtcn_expect_ref_back(t);

	/* Memory-misaligned payload: nfsd_vfs_write() would bounce it. */
	cnt = PAGE_SIZE;
	resp = rtcn_wprep(t, 1, true, PAGE_SIZE, NFS_UNSTABLE);
	t->payload.page_base = 100;
	nfsd_rtc_write_file(r, &resp->fh, rtcn_get(t), 0, &t->payload, &cnt,
			    0, resp->verf);
	t->payload.page_base = 0;
	KUNIT_EXPECT_TRUE(test, test_bit(RQ_RTC_MISS, &r->rq_flags));
	KUNIT_EXPECT_EQ(test, r->rq_rtc_reason, SVC_RTC_FB_UNALIGNED);
	KUNIT_EXPECT_EQ(test, t->calls, 0);
	rtcn_expect_ref_back(t);
}

/*
 * An async (non-EX_ISSYNC) export: FILE_SYNC is not honoured, as in
 * nfsd_vfs_write(); a sync hit with no IOCB_DSYNC, the thread's reply.
 */
static void nfsd_rtc_write_async_export(struct kunit *test)
{
	struct rtcn *t = test->priv;
	struct svc_rqst *r = t->rq[1];
	unsigned long cnt = PAGE_SIZE;
	struct nfsd3_writeres *resp;
	int saved = t->exp->ex_flags;

	t->exp->ex_flags |= NFSEXP_ASYNC;
	rtcn_wthread_ref(t, 0, PAGE_SIZE, NFS_FILE_SYNC);
	KUNIT_EXPECT_FALSE(test, t->flags & IOCB_DSYNC);
	resp = rtcn_wprep(t, 1, true, PAGE_SIZE, NFS_FILE_SYNC);
	resp->status = nfsd_rtc_write_file(r, &resp->fh, rtcn_get(t), 0,
					   &t->payload, &cnt,
					   rtcn_iocb_flags(NFS_FILE_SYNC),
					   resp->verf);
	resp->count = cnt;
	t->exp->ex_flags = saved;
	KUNIT_EXPECT_FALSE(test, test_bit(RQ_RTC_MISS, &r->rq_flags));
	KUNIT_EXPECT_FALSE(test, test_bit(RQ_RTC_ASYNC, &r->rq_flags));
	KUNIT_EXPECT_EQ(test, resp->status, nfs_ok);
	KUNIT_EXPECT_EQ(test, t->flags & (IOCB_DIRECT | IOCB_NOWAIT | IOCB_DSYNC),
			IOCB_DIRECT | IOCB_NOWAIT);
	rtcn_expect_ref_back(t);
	KUNIT_ASSERT_TRUE(test, nfs3svc_encode_writeres(r, &r->rq_res_stream));
	rtcn_capture(t, 1);
	rtcn_wexpect_reply(t, 0, PAGE_SIZE, NFS_FILE_SYNC);
}

static struct kunit_case nfsd_rtc_cases[] = {
	KUNIT_CASE(nfsd_rtc_sync_hit),
	KUNIT_CASE(nfsd_rtc_async_complete),
	KUNIT_CASE(nfsd_rtc_file_miss),
	KUNIT_CASE(nfsd_rtc_unaligned),
	KUNIT_CASE(nfsd_rtc_eagain_submit),
	KUNIT_CASE(nfsd_rtc_eagain_complete),
	KUNIT_CASE(nfsd_rtc_write_sync_hit),
	KUNIT_CASE(nfsd_rtc_write_async_complete),
	KUNIT_CASE(nfsd_rtc_write_stable),
	KUNIT_CASE(nfsd_rtc_write_eagain_submit),
	KUNIT_CASE(nfsd_rtc_write_eagain_complete),
	KUNIT_CASE(nfsd_rtc_write_drc),
	KUNIT_CASE(nfsd_rtc_write_unaligned),
	KUNIT_CASE(nfsd_rtc_read_nowait_errors),
	KUNIT_CASE(nfsd_rtc_write_nowait_err_submit),
	KUNIT_CASE(nfsd_rtc_write_nowait_err_complete),
	KUNIT_CASE(nfsd_rtc_write_frozen),
	KUNIT_CASE(nfsd_rtc_write_fallbacks),
	KUNIT_CASE(nfsd_rtc_write_async_export),
	{}
};

static struct kunit_suite nfsd_rtc_suite = {
	.name		= "sunrpc-nfsd-rtc",
	.init		= rtcn_init,
	.exit		= rtcn_exit,
	.test_cases	= nfsd_rtc_cases,
};

kunit_test_suite(nfsd_rtc_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_DESCRIPTION("KUnit tests for the nfsd NFSv3 READ/WRITE run-to-completion fast path");
MODULE_LICENSE("GPL");
