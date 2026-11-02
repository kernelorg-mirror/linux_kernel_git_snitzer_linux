// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for nvmet-tcp's receive of WRITE data into rx_loan region
 * loans and its sends of READ data from them.  Included from tcp.c so that the static helpers are reachable.
 */
#include <kunit/test.h>

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");

struct nvmet_tcp_kunit {
	struct nvmet_tcp_queue	*queue;
	struct page		*page;
};

static struct nvmet_tcp_cmd *nvmet_tcp_kunit_cmd(struct kunit *test,
						 struct nvmet_tcp_queue *queue,
						 u32 bytes)
{
	struct nvmet_tcp_cmd *cmd = kunit_kzalloc(test, sizeof(*cmd),
						  GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, cmd);
	cmd->queue = queue;
	cmd->req.transfer_len = bytes;
	cmd->pdu_len = bytes;
	KUNIT_ASSERT_EQ(test, rx_loan_table_alloc(&cmd->lt,
						  nvmet_tcp_rx_entries(bytes),
						  GFP_KERNEL, NUMA_NO_NODE), 0);
	cmd->flags = NVMET_TCP_F_RX_LOAN;
	return cmd;
}

static int nvmet_tcp_kunit_init(struct kunit *test)
{
	struct nvmet_tcp_kunit *k = kunit_kzalloc(test, sizeof(*k), GFP_KERNEL);

	if (!k)
		return -ENOMEM;
	k->queue = kunit_kzalloc(test, sizeof(*k->queue), GFP_KERNEL);
	k->page = alloc_page(GFP_KERNEL);
	if (!k->queue || !k->page)
		return -ENOMEM;
	INIT_LIST_HEAD(&k->queue->rx_withheld);
	init_llist_head(&k->queue->rx_done);
	INIT_LIST_HEAD(&k->queue->rx_wait.node);
	k->queue->rx_wait.ops = &nvmet_tcp_rx_waiter_ops;
	memset(page_address(k->page), 0x5a, PAGE_SIZE);
	test->priv = k;
	return 0;
}

static void nvmet_tcp_kunit_exit(struct kunit *test)
{
	struct nvmet_tcp_kunit *k = test->priv;

	if (k && k->page)
		__free_page(k->page);
}

/* An skb of two page frags, 1448 + 1552 bytes, as a NIC would build. */
static struct sk_buff *nvmet_tcp_kunit_skb(struct kunit *test,
					   struct page *page)
{
	struct sk_buff *skb = alloc_skb(0, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, skb);
	get_page(page);
	skb_fill_page_desc(skb, 0, page, 0, 1448);
	get_page(page);
	skb_fill_page_desc(skb, 1, page, 1448, 1552);
	skb->len = 3000;
	skb->data_len = 3000;
	skb->truesize += PAGE_SIZE;
	return skb;
}

static int nvmet_tcp_kunit_actor(struct nvmet_tcp_cmd *cmd,
				 struct sk_buff *skb, int *stop)
{
	struct nvmet_tcp_rx_desc rd = { .cmd = cmd };
	read_descriptor_t desc = {
		.arg.data = &rd,
		.count = cmd->pdu_len - cmd->pdu_recv,
	};
	int ret = nvmet_tcp_rx_actor(&desc, skb, 0, skb->len);

	*stop = rd.stop;
	return ret;
}

static void nvmet_tcp_kunit_reserve_rule(struct kunit *test)
{
	u32 unit;

	/* Unlearned: a reserve of 66 entries fits a 64 KiB unit. */
	unit = nvmet_tcp_rx_unit_max(66, 0);
	KUNIT_EXPECT_EQ(test, unit, (u32)SZ_64K);
	KUNIT_EXPECT_LE(test, nvmet_tcp_rx_cost(unit, 0, UINT_MAX), 66U);

	/* Learned at 150 region pages per 100: 42 pages for 64. */
	unit = nvmet_tcp_rx_unit_max(64, 150);
	KUNIT_EXPECT_EQ(test, unit, (u32)(42 * PAGE_SIZE));
	KUNIT_EXPECT_LE(test, nvmet_tcp_rx_cost(unit, 150, UINT_MAX), 64U);

	/* Clamped to [PAGE_SIZE, NVMET_TCP_MAXH2CDATA]. */
	KUNIT_EXPECT_EQ(test, nvmet_tcp_rx_unit_max(0, 0), (u32)PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, nvmet_tcp_rx_unit_max(1U << 20, 0),
			(u32)NVMET_TCP_MAXH2CDATA);
	/* Never more than a table holds. */
	KUNIT_EXPECT_EQ(test, nvmet_tcp_rx_cost(SZ_1M, 0, 7), 7U);
}

static void nvmet_tcp_kunit_actor_borrows(struct kunit *test)
{
	struct nvmet_tcp_kunit *k = test->priv;
	struct nvmet_tcp_cmd *cmd = nvmet_tcp_kunit_cmd(test, k->queue, 3000);
	struct sk_buff *skb = nvmet_tcp_kunit_skb(test, k->page);
	int stop;

	KUNIT_EXPECT_EQ(test, nvmet_tcp_kunit_actor(cmd, skb, &stop), 3000);
	KUNIT_EXPECT_EQ(test, stop, NVMET_TCP_RX_GO);
	KUNIT_EXPECT_EQ(test, cmd->pdu_recv, 3000U);
	KUNIT_EXPECT_EQ(test, cmd->rbytes_done, 3000U);
	KUNIT_EXPECT_GE(test, cmd->lt.count, 1U);
	kfree_skb(skb);
	/* The table holds the page beyond the skb's life. */
	KUNIT_EXPECT_GT(test, page_ref_count(k->page), 1);
	nvmet_tcp_free_cmd_buffers(cmd);
	KUNIT_EXPECT_EQ(test, page_ref_count(k->page), 1);
	KUNIT_EXPECT_FALSE(test, cmd->flags & NVMET_TCP_F_RX_LOAN);
}

/* A locked head's bytes go to the bounce page; the table takes them. */
static void nvmet_tcp_kunit_locked_head_bounced(struct kunit *test)
{
	struct nvmet_tcp_kunit *k = test->priv;
	struct nvmet_tcp_cmd *cmd = nvmet_tcp_kunit_cmd(test, k->queue, 3000);
	struct sk_buff *skb = alloc_skb(3000, GFP_KERNEL);
	int stop;

	KUNIT_ASSERT_NOT_NULL(test, skb);
	memset(skb_put(skb, 3000), 0xa5, 3000);
	KUNIT_EXPECT_EQ(test, nvmet_tcp_kunit_actor(cmd, skb, &stop), 3000);
	KUNIT_EXPECT_EQ(test, stop, NVMET_TCP_RX_GO);
	KUNIT_EXPECT_EQ(test, cmd->lt.count, 1U);
	KUNIT_EXPECT_EQ(test, cmd->lt.copied_bytes, 3000U);
	KUNIT_EXPECT_EQ(test, cmd->lt.borrowed_count, 0U);
	KUNIT_EXPECT_NOT_NULL(test, cmd->rx_bounce);
	kfree_skb(skb);
	nvmet_tcp_free_cmd_buffers(cmd);
	KUNIT_EXPECT_NULL(test, cmd->rx_bounce);
}

/*
 * A small locked head (as virtio-net and GRO build it) before page frags:
 * its bytes are bounced, the frags still lent, in data order.
 */
static void nvmet_tcp_kunit_locked_head_mixed(struct kunit *test)
{
	struct nvmet_tcp_kunit *k = test->priv;
	struct nvmet_tcp_cmd *cmd = nvmet_tcp_kunit_cmd(test, k->queue, 3000);
	struct sk_buff *skb = alloc_skb(56, GFP_KERNEL);
	u8 buf[2];
	int stop;

	KUNIT_ASSERT_NOT_NULL(test, skb);
	memset(skb_put(skb, 56), 0xa5, 56);
	get_page(k->page);
	skb_fill_page_desc(skb, 0, k->page, 0, 2944);
	skb->len += 2944;
	skb->data_len = 2944;
	skb->truesize += PAGE_SIZE;
	KUNIT_EXPECT_EQ(test, nvmet_tcp_kunit_actor(cmd, skb, &stop), 3000);
	KUNIT_EXPECT_EQ(test, stop, NVMET_TCP_RX_GO);
	KUNIT_EXPECT_EQ(test, cmd->lt.copied_bytes, 56U);
	KUNIT_EXPECT_EQ(test, cmd->lt.borrowed_bytes, 2944U);
	kfree_skb(skb);
	/* The frag's page stays lent beyond the skb's life. */
	KUNIT_EXPECT_GT(test, page_ref_count(k->page), 1);
	KUNIT_ASSERT_EQ(test, nvmet_tcp_rx_to_sgl(cmd), 0);
	KUNIT_EXPECT_EQ(test, page_ref_count(k->page), 1);
	KUNIT_EXPECT_NULL(test, cmd->rx_bounce);
	KUNIT_ASSERT_NOT_NULL(test, cmd->req.sg);
	sg_pcopy_to_buffer(cmd->req.sg, cmd->req.sg_cnt, buf, 2, 55);
	KUNIT_EXPECT_EQ(test, buf[0], 0xa5);
	KUNIT_EXPECT_EQ(test, buf[1], 0x5a);
	nvmet_tcp_free_cmd_buffers(cmd);
}

/* A locked head and page frags, @fill in the head, as virtio-net builds it. */
static struct sk_buff *nvmet_tcp_kunit_head_skb(struct kunit *test,
						struct page *page, u8 fill)
{
	struct sk_buff *skb = alloc_skb(56, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, skb);
	memset(skb_put(skb, 56), fill, 56);
	get_page(page);
	skb_fill_page_desc(skb, 0, page, 0, 2944);
	skb->len += 2944;
	skb->data_len = 2944;
	skb->truesize += PAGE_SIZE;
	return skb;
}

/*
 * Two such skbs into one command: the second head goes after the first in
 * the bounce page, and each lands at its own place in the data.
 */
static void nvmet_tcp_kunit_locked_head_two_skbs(struct kunit *test)
{
	struct nvmet_tcp_kunit *k = test->priv;
	struct nvmet_tcp_cmd *cmd = nvmet_tcp_kunit_cmd(test, k->queue, 6000);
	struct sk_buff *skb;
	u8 buf[2];
	int stop;

	skb = nvmet_tcp_kunit_head_skb(test, k->page, 0xa5);
	KUNIT_EXPECT_EQ(test, nvmet_tcp_kunit_actor(cmd, skb, &stop), 3000);
	KUNIT_EXPECT_EQ(test, stop, NVMET_TCP_RX_GO);
	kfree_skb(skb);
	skb = nvmet_tcp_kunit_head_skb(test, k->page, 0xc3);
	KUNIT_EXPECT_EQ(test, nvmet_tcp_kunit_actor(cmd, skb, &stop), 3000);
	KUNIT_EXPECT_EQ(test, stop, NVMET_TCP_RX_GO);
	kfree_skb(skb);
	KUNIT_EXPECT_EQ(test, cmd->rx_bounce_used, 112U);
	KUNIT_EXPECT_EQ(test, cmd->lt.copied_bytes, 112U);
	KUNIT_EXPECT_EQ(test, cmd->lt.borrowed_bytes, 5888U);
	KUNIT_ASSERT_EQ(test, nvmet_tcp_rx_to_sgl(cmd), 0);
	KUNIT_EXPECT_EQ(test, page_ref_count(k->page), 1);
	KUNIT_EXPECT_NULL(test, cmd->rx_bounce);
	KUNIT_ASSERT_NOT_NULL(test, cmd->req.sg);
	sg_pcopy_to_buffer(cmd->req.sg, cmd->req.sg_cnt, buf, 2, 0);
	KUNIT_EXPECT_EQ(test, buf[0], 0xa5);
	sg_pcopy_to_buffer(cmd->req.sg, cmd->req.sg_cnt, buf, 2, 2999);
	KUNIT_EXPECT_EQ(test, buf[0], 0x5a);
	KUNIT_EXPECT_EQ(test, buf[1], 0xc3);
	sg_pcopy_to_buffer(cmd->req.sg, cmd->req.sg_cnt, buf, 2, 3055);
	KUNIT_EXPECT_EQ(test, buf[0], 0xc3);
	KUNIT_EXPECT_EQ(test, buf[1], 0x5a);
	nvmet_tcp_free_cmd_buffers(cmd);
}

/* A locked head past the bounce page's room still copies the record. */
static void nvmet_tcp_kunit_locked_head_copies(struct kunit *test)
{
	struct nvmet_tcp_kunit *k = test->priv;
	struct nvmet_tcp_cmd *cmd = nvmet_tcp_kunit_cmd(test, k->queue,
							PAGE_SIZE + 1);
	struct sk_buff *skb = alloc_skb(PAGE_SIZE + 1, GFP_KERNEL);
	int stop;

	KUNIT_ASSERT_NOT_NULL(test, skb);
	skb_put(skb, PAGE_SIZE + 1);
	KUNIT_EXPECT_EQ(test, nvmet_tcp_kunit_actor(cmd, skb, &stop), 0);
	KUNIT_EXPECT_EQ(test, stop, NVMET_TCP_RX_COPY);
	KUNIT_EXPECT_EQ(test, cmd->lt.count, 0U);
	kfree_skb(skb);
	nvmet_tcp_free_cmd_buffers(cmd);
	KUNIT_EXPECT_NULL(test, cmd->rx_bounce);
}

/*
 * nvmet-tcp's own copies counter: allocated at the consumer's first
 * registration and kept across registrations.  If no queue has registered,
 * register and unregister at once under the queues' lock, so the counter
 * exists while kNFSd's share of the region, and what any queue sees, stay
 * as they were.  For the moment between the two nvmet-tcp counts as a
 * consumer, so a region carved in it would be sized with its floor: a test
 * kernel's first call only.
 */
static void nvmet_tcp_kunit_own_copies(struct kunit *test, u64 *records,
				       u64 *bytes)
{
	mutex_lock(&nvmet_tcp_rx_consumer_lock);
	if (!nvmet_tcp_rx_consumer_on && !nvmet_tcp_rx_consumer.copies) {
		rx_loan_consumer_register(&nvmet_tcp_rx_consumer,
					  rx_loan_net(&init_net), "nvmet-tcp",
					  NVMET_TCP_RX_FLOOR);
		rx_loan_consumer_unregister(&nvmet_tcp_rx_consumer);
	}
	mutex_unlock(&nvmet_tcp_rx_consumer_lock);
	KUNIT_ASSERT_TRUE(test, nvmet_tcp_rx_consumer.copies != NULL);
	rx_loan_consumer_copies_read(&nvmet_tcp_rx_consumer, records, bytes);
}

static void nvmet_tcp_kunit_copy_counted(struct kunit *test)
{
	struct nvmet_tcp_kunit *k = test->priv;
	struct nvmet_tcp_cmd *cmd = nvmet_tcp_kunit_cmd(test, k->queue, 3000);
	struct sk_buff *skb = nvmet_tcp_kunit_skb(test, k->page);
	u64 rec0, bytes0, rec1, bytes1, own0, obytes0, own1, obytes1;
	u8 buf[64];
	int stop;

	KUNIT_ASSERT_EQ(test, nvmet_tcp_kunit_actor(cmd, skb, &stop), 3000);
	kfree_skb(skb);
	rx_loan_copies_read(&rec0, &bytes0);
	nvmet_tcp_kunit_own_copies(test, &own0, &obytes0);
	KUNIT_ASSERT_EQ(test, nvmet_tcp_rx_to_sgl(cmd), 0);
	rx_loan_copies_read(&rec1, &bytes1);
	nvmet_tcp_kunit_own_copies(test, &own1, &obytes1);
	KUNIT_EXPECT_GE(test, rec1 - rec0, 1ULL);
	KUNIT_EXPECT_GE(test, bytes1 - bytes0, 3000ULL);
	/* Its own counter moves too (a live target may add its own). */
	KUNIT_EXPECT_GE(test, own1 - own0, 1ULL);
	KUNIT_EXPECT_GE(test, obytes1 - obytes0, 3000ULL);
	KUNIT_EXPECT_FALSE(test, cmd->flags & NVMET_TCP_F_RX_LOAN);
	/* The loans went back at the copy, not at the free. */
	KUNIT_EXPECT_EQ(test, page_ref_count(k->page), 1);
	KUNIT_ASSERT_NOT_NULL(test, cmd->req.sg);
	sg_pcopy_to_buffer(cmd->req.sg, cmd->req.sg_cnt, buf, sizeof(buf),
			   2900);
	KUNIT_EXPECT_EQ(test, buf[0], 0x5a);
	KUNIT_EXPECT_EQ(test, buf[sizeof(buf) - 1], 0x5a);
	nvmet_tcp_free_cmd_buffers(cmd);
}

static void nvmet_tcp_kunit_noop_work(struct work_struct *w)
{
}

static void nvmet_tcp_kunit_data_done_once(struct kunit *test)
{
	struct nvmet_tcp_kunit *k = test->priv;
	struct nvmet_tcp_queue *queue = k->queue;
	struct nvmet_tcp_cmd *cmd = nvmet_tcp_kunit_cmd(test, queue, 3000);
	struct sk_buff *skb = nvmet_tcp_kunit_skb(test, k->page);
	int stop;

	/* Inline: the first call returns everything, the second nothing. */
	KUNIT_ASSERT_EQ(test, nvmet_tcp_kunit_actor(cmd, skb, &stop), 3000);
	kfree_skb(skb);
	nvmet_tcp_data_done(&cmd->req);
	KUNIT_EXPECT_EQ(test, page_ref_count(k->page), 1);
	nvmet_tcp_data_done(&cmd->req);
	KUNIT_EXPECT_EQ(test, page_ref_count(k->page), 1);
	nvmet_tcp_free_cmd_buffers(cmd);
	KUNIT_EXPECT_EQ(test, page_ref_count(k->page), 1);

	/* Hard-irq-like: deferred to io_work, drained at the free. */
	KUNIT_ASSERT_EQ(test, sock_create_kern(&init_net, PF_INET, SOCK_STREAM,
					       IPPROTO_TCP, &queue->sock), 0);
	/* Never received, so no incoming CPU yet: give queue_cpu() one. */
	queue->sock->sk->sk_incoming_cpu = raw_smp_processor_id();
	INIT_WORK(&queue->io_work, nvmet_tcp_kunit_noop_work);
	cmd = nvmet_tcp_kunit_cmd(test, queue, 3000);
	skb = nvmet_tcp_kunit_skb(test, k->page);
	KUNIT_ASSERT_EQ(test, nvmet_tcp_kunit_actor(cmd, skb, &stop), 3000);
	kfree_skb(skb);
	local_irq_disable();
	nvmet_tcp_data_done(&cmd->req);
	nvmet_tcp_data_done(&cmd->req);
	local_irq_enable();
	KUNIT_EXPECT_TRUE(test, cmd->flags & NVMET_TCP_F_RX_DEFER);
	KUNIT_EXPECT_FALSE(test, llist_empty(&queue->rx_done));
	KUNIT_EXPECT_GT(test, page_ref_count(k->page), 1);
	flush_work(&queue->io_work);
	nvmet_tcp_free_cmd_buffers(cmd);
	KUNIT_EXPECT_TRUE(test, llist_empty(&queue->rx_done));
	KUNIT_EXPECT_EQ(test, page_ref_count(k->page), 1);
	sock_release(queue->sock);
	queue->sock = NULL;
}

static void nvmet_tcp_kunit_queue_down(void *data)
{
	struct nvmet_tcp_queue *queue = data;

	flush_work(&queue->io_work);
	sock_release(queue->sock);
	queue->sock = NULL;
}

/* A socket for queue_cpu(), an io_work that does nothing, a kref to pin. */
static void nvmet_tcp_kunit_queue_live(struct kunit *test,
				       struct nvmet_tcp_queue *queue)
{
	int ret;

	KUNIT_ASSERT_EQ(test, sock_create_kern(&init_net, PF_INET, SOCK_STREAM,
					       IPPROTO_TCP, &queue->sock), 0);
	/* Never received, so no incoming CPU yet: give queue_cpu() one. */
	queue->sock->sk->sk_incoming_cpu = raw_smp_processor_id();
	INIT_WORK(&queue->io_work, nvmet_tcp_kunit_noop_work);
	kref_init(&queue->kref);
	ret = kunit_add_action_or_reset(test, nvmet_tcp_kunit_queue_down, queue);
	KUNIT_ASSERT_EQ(test, ret, 0);
}

static void nvmet_tcp_kunit_budget_free(void *data)
{
	struct rx_loan_budget *lb = data;

	flush_work(&lb->kick_work);
	free_percpu(lb->stock);
}

static void nvmet_tcp_kunit_region_reset(void *data)
{
	rx_loan_kunit_region(0, 0);
}

/*
 * A budget of @budget pages, @reserve of them reserve, below stocking (as
 * the helper's own tests build one), over a stand-in region.
 */
static struct rx_loan_budget *nvmet_tcp_kunit_budget(struct kunit *test,
						     unsigned int budget,
						     unsigned int reserve)
{
	struct rx_loan_budget *lb = kunit_kzalloc(test, sizeof(*lb),
						  GFP_KERNEL);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, lb);
	lb->stock = alloc_percpu(struct rx_loan_stock);
	KUNIT_ASSERT_TRUE(test, lb->stock != NULL);
	ret = kunit_add_action_or_reset(test, nvmet_tcp_kunit_budget_free, lb);
	KUNIT_ASSERT_EQ(test, ret, 0);
	spin_lock_init(&lb->lock);
	lb->next_ticket = 1;
	INIT_LIST_HEAD(&lb->inflight);
	INIT_LIST_HEAD(&lb->waiters);
	INIT_WORK(&lb->kick_work, rx_loan_kick_fn);
	rx_loan_budget_request(lb, budget, reserve, false);
	spin_lock_bh(&lb->lock);
	lb->contended = false;
	spin_unlock_bh(&lb->lock);
	ret = kunit_add_action_or_reset(test, nvmet_tcp_kunit_region_reset, NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);
	rx_loan_kunit_region(1UL << 16, 0);
	return lb;
}

static long nvmet_tcp_kunit_avail(struct rx_loan_budget *lb)
{
	long avail;

	spin_lock_bh(&lb->lock);
	rx_loan_stock_drain_locked(lb);
	avail = lb->avail;
	spin_unlock_bh(&lb->lock);
	return avail;
}

static void nvmet_tcp_kunit_withhold(struct kunit *test)
{
	struct nvmet_tcp_kunit *k = test->priv;
	struct nvmet_tcp_queue *queue = k->queue;
	struct nvmet_tcp_cmd *a = nvmet_tcp_kunit_cmd(test, queue, 4096);
	struct nvmet_tcp_cmd *b = nvmet_tcp_kunit_cmd(test, queue, 4096);
	struct rx_loan_budget *off = kunit_kzalloc(test, sizeof(*off),
						   GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, off);
	nvmet_tcp_kunit_queue_live(test, queue);
	queue->rx_budget = off;	/* budget 0: turned off */

	/* a holds the unit: b's R2T is withheld, not sent. */
	queue->rx_unit = a;
	a->r2t_end = 4096;
	KUNIT_EXPECT_FALSE(test, nvmet_tcp_rx_r2t_ok(b));
	KUNIT_EXPECT_PTR_EQ(test, list_first_entry(&queue->rx_withheld,
						   struct nvmet_tcp_cmd, entry),
			    b);
	KUNIT_EXPECT_NULL(test, nvmet_tcp_rx_next_withheld(queue));

	/* a's unit is in and it needs no more: the unit is free. */
	a->rbytes_done = 4096;
	nvmet_tcp_rx_unit_done(a);
	KUNIT_EXPECT_NULL(test, queue->rx_unit);

	/* b goes out on the stock path, the budget being off. */
	KUNIT_EXPECT_PTR_EQ(test, nvmet_tcp_rx_next_withheld(queue), b);
	KUNIT_EXPECT_TRUE(test, list_empty(&queue->rx_withheld));
	KUNIT_EXPECT_FALSE(test, b->flags & NVMET_TCP_F_RX_LOAN);
	KUNIT_EXPECT_NOT_NULL(test, b->req.sg);
	nvmet_tcp_free_cmd_buffers(a);
	nvmet_tcp_free_cmd_buffers(b);
}

/* Admission denied then resumed; a's credit comes back exactly once. */
static void nvmet_tcp_kunit_deny_resume(struct kunit *test)
{
	struct nvmet_tcp_kunit *k = test->priv;
	struct nvmet_tcp_queue *queue = k->queue;
	struct nvmet_tcp_cmd *a, *b;
	struct rx_loan_budget *lb;
	long avail;

	nvmet_tcp_kunit_queue_live(test, queue);
	lb = nvmet_tcp_kunit_budget(test, 72, 66);	/* 6 pages avail */
	queue->rx_budget = lb;
	a = nvmet_tcp_kunit_cmd(test, queue, 3000);	/* cost 5 */
	b = nvmet_tcp_kunit_cmd(test, queue, 4096);	/* cost 6 */

	/* a capsule that may wait is admitted at its cost. */
	KUNIT_ASSERT_EQ(test, nvmet_tcp_rx_admit_capsule(a), 0);
	KUNIT_EXPECT_TRUE(test, a->cr.linked);
	KUNIT_EXPECT_PTR_EQ(test, queue->rx_unit, a);
	a->rbytes_done = 3000;
	KUNIT_EXPECT_FALSE(test, nvmet_tcp_rx_unit_done(a));
	KUNIT_EXPECT_NULL(test, queue->rx_unit);

	/* b's unit costs more than is left: withheld, its waiter queued. */
	KUNIT_EXPECT_FALSE(test, nvmet_tcp_rx_r2t_ok(b));
	KUNIT_EXPECT_TRUE(test, rx_loan_waiting(&queue->rx_wait));
	KUNIT_EXPECT_FALSE(test, b->cr.linked);
	KUNIT_EXPECT_NULL(test, nvmet_tcp_rx_next_withheld(queue));

	/* The backend is done with a: its credit comes back, once. */
	nvmet_tcp_data_done(&a->req);
	KUNIT_EXPECT_FALSE(test, a->cr.linked);
	flush_work(&lb->kick_work);
	flush_work(&queue->io_work);
	avail = nvmet_tcp_kunit_avail(lb);
	nvmet_tcp_data_done(&a->req);
	nvmet_tcp_free_cmd_buffers(a);
	KUNIT_EXPECT_EQ(test, nvmet_tcp_kunit_avail(lb), avail);

	/* Woken, b is admitted and its R2T goes out. */
	KUNIT_EXPECT_TRUE(test, READ_ONCE(queue->rx_kicked));
	KUNIT_EXPECT_PTR_EQ(test, nvmet_tcp_rx_next_withheld(queue), b);
	KUNIT_EXPECT_TRUE(test, b->cr.linked);
	KUNIT_EXPECT_EQ(test, b->r2t_end, 4096U);
	KUNIT_EXPECT_PTR_EQ(test, queue->rx_unit, b);
	KUNIT_EXPECT_TRUE(test, list_empty(&queue->rx_withheld));
	nvmet_tcp_free_cmd_buffers(b);
	KUNIT_EXPECT_NULL(test, queue->rx_unit);
	KUNIT_EXPECT_FALSE(test, b->cr.linked);
}

/* A command keeps the unit across its R2Ts; a younger one cannot take it. */
static void nvmet_tcp_kunit_unit_kept(struct kunit *test)
{
	struct nvmet_tcp_kunit *k = test->priv;
	struct nvmet_tcp_queue *queue = k->queue;
	struct nvmet_tcp_cmd *a, *b;

	nvmet_tcp_kunit_queue_live(test, queue);
	queue->rx_budget = nvmet_tcp_kunit_budget(test, 100, 6); /* 4 KiB unit */
	a = nvmet_tcp_kunit_cmd(test, queue, 3 * 4096);
	b = nvmet_tcp_kunit_cmd(test, queue, 4096);

	KUNIT_ASSERT_TRUE(test, nvmet_tcp_rx_r2t_ok(a));
	KUNIT_EXPECT_EQ(test, a->r2t_end, 4096U);
	KUNIT_EXPECT_FALSE(test, nvmet_tcp_rx_r2t_ok(b));
	KUNIT_EXPECT_FALSE(test, b->cr.linked);

	/* Unit 1 is in: a solicits more and keeps the unit. */
	a->rbytes_done = 4096;
	KUNIT_EXPECT_FALSE(test, nvmet_tcp_rx_unit_done(a));
	KUNIT_EXPECT_PTR_EQ(test, queue->rx_unit, a);
	KUNIT_EXPECT_PTR_EQ(test, llist_del_all(&queue->resp_list), &a->lentry);
	WRITE_ONCE(queue->rx_kicked, true);
	KUNIT_EXPECT_NULL(test, nvmet_tcp_rx_next_withheld(queue));
	KUNIT_EXPECT_TRUE(test, nvmet_tcp_rx_r2t_ok(a));
	KUNIT_EXPECT_EQ(test, a->r2t_end, 2 * 4096U);

	a->rbytes_done = 2 * 4096;
	KUNIT_EXPECT_FALSE(test, nvmet_tcp_rx_unit_done(a));
	KUNIT_EXPECT_PTR_EQ(test, llist_del_all(&queue->resp_list), &a->lentry);
	KUNIT_EXPECT_TRUE(test, nvmet_tcp_rx_r2t_ok(a));
	KUNIT_EXPECT_EQ(test, a->r2t_end, 3 * 4096U);

	/* All of a is in: the unit passes to b, the oldest withheld. */
	a->rbytes_done = 3 * 4096;
	KUNIT_EXPECT_FALSE(test, nvmet_tcp_rx_unit_done(a));
	KUNIT_EXPECT_NULL(test, queue->rx_unit);
	KUNIT_EXPECT_TRUE(test, llist_empty(&queue->resp_list));
	KUNIT_EXPECT_PTR_EQ(test, nvmet_tcp_rx_next_withheld(queue), b);
	KUNIT_EXPECT_PTR_EQ(test, queue->rx_unit, b);
	nvmet_tcp_free_cmd_buffers(a);
	nvmet_tcp_free_cmd_buffers(b);
}

/*
 * A capsule pipelined after R2T #1 of another command must not wait for
 * credit that command holds: it is copied, and the waiter is left alone.
 */
static void nvmet_tcp_kunit_capsule_no_wait(struct kunit *test)
{
	struct nvmet_tcp_kunit *k = test->priv;
	struct nvmet_tcp_queue *queue = k->queue;
	struct nvmet_tcp_cmd *a, *c;

	nvmet_tcp_kunit_queue_live(test, queue);
	queue->rx_budget = nvmet_tcp_kunit_budget(test, 12, 6);
	a = nvmet_tcp_kunit_cmd(test, queue, 3 * 4096);
	c = nvmet_tcp_kunit_cmd(test, queue, 3000);

	KUNIT_ASSERT_TRUE(test, nvmet_tcp_rx_r2t_ok(a));
	KUNIT_EXPECT_EQ(test, nvmet_tcp_rx_admit_capsule(c), 1);
	KUNIT_EXPECT_FALSE(test, rx_loan_waiting(&queue->rx_wait));
	KUNIT_EXPECT_FALSE(test, c->flags & NVMET_TCP_F_RX_LOAN);
	KUNIT_EXPECT_NOT_NULL(test, c->req.sg);
	KUNIT_EXPECT_EQ(test, iov_iter_count(&c->recv_msg.msg_iter), 3000UL);
	KUNIT_EXPECT_PTR_EQ(test, queue->rx_unit, a);
	nvmet_tcp_free_cmd_buffers(c);
	nvmet_tcp_free_cmd_buffers(a);
}

static struct nvmet_tcp_queue *nvmet_tcp_kunit_queue(struct kunit *test)
{
	struct nvmet_tcp_queue *queue = kunit_kzalloc(test, sizeof(*queue),
						      GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, queue);
	INIT_LIST_HEAD(&queue->rx_withheld);
	init_llist_head(&queue->rx_done);
	INIT_LIST_HEAD(&queue->rx_wait.node);
	queue->rx_wait.ops = &nvmet_tcp_rx_waiter_ops;
	nvmet_tcp_kunit_queue_live(test, queue);
	return queue;
}

/*
 * Release, in release_queue_work's order, while the queue waits for a
 * withheld R2T and a request is still outstanding: the waiter leaves the
 * FIFO at once, nothing re-queues it, and a younger waiter on another
 * queue is woken by the outstanding request's return.
 */
static void nvmet_tcp_kunit_teardown_waiter(struct kunit *test)
{
	struct nvmet_tcp_kunit *k = test->priv;
	struct nvmet_tcp_queue *queue = k->queue, *q2;
	struct nvmet_tcp_cmd *a, *b, *d;
	struct rx_loan_budget *lb;

	nvmet_tcp_kunit_queue_live(test, queue);
	q2 = nvmet_tcp_kunit_queue(test);
	lb = nvmet_tcp_kunit_budget(test, 72, 66);
	queue->rx_budget = lb;
	q2->rx_budget = lb;
	a = nvmet_tcp_kunit_cmd(test, queue, 3000);
	b = nvmet_tcp_kunit_cmd(test, queue, 4096);
	d = nvmet_tcp_kunit_cmd(test, q2, 4096);
	KUNIT_ASSERT_EQ(test, nvmet_tcp_rx_admit_capsule(a), 0);
	a->rbytes_done = 3000;
	KUNIT_EXPECT_FALSE(test, nvmet_tcp_rx_unit_done(a));
	KUNIT_ASSERT_FALSE(test, nvmet_tcp_rx_r2t_ok(b));
	KUNIT_ASSERT_TRUE(test, rx_loan_waiting(&queue->rx_wait));
	KUNIT_ASSERT_FALSE(test, nvmet_tcp_rx_r2t_ok(d));
	KUNIT_ASSERT_TRUE(test, rx_loan_waiting(&q2->rx_wait));

	/* Disconnecting: io_work's next look takes the waiter off. */
	WRITE_ONCE(queue->state, NVMET_TCP_Q_DISCONNECTING);
	KUNIT_EXPECT_NULL(test, nvmet_tcp_rx_next_withheld(queue));
	KUNIT_EXPECT_FALSE(test, rx_loan_waiting(&queue->rx_wait));

	/* release_queue_work: the kref is at zero, receive stopped. */
	refcount_set(&queue->kref.refcount, 0);
	WRITE_ONCE(queue->rcv_state, NVMET_TCP_RECV_ERR);
	rx_loan_unwait(&queue->rx_wait);
	WRITE_ONCE(queue->rx_kicked, true);
	KUNIT_EXPECT_NULL(test, nvmet_tcp_rx_next_withheld(queue));
	KUNIT_EXPECT_EQ(test, nvmet_tcp_rx_admit_capsule(b), -EAGAIN);
	KUNIT_EXPECT_FALSE(test, rx_loan_waiting(&queue->rx_wait));
	KUNIT_EXPECT_FALSE(test, b->cr.linked);

	/* a completes in nvmet_sq_destroy(): the younger waiter goes on. */
	nvmet_tcp_data_done(&a->req);
	flush_work(&lb->kick_work);
	flush_work(&q2->io_work);
	KUNIT_EXPECT_TRUE(test, READ_ONCE(q2->rx_kicked));
	KUNIT_EXPECT_PTR_EQ(test, nvmet_tcp_rx_next_withheld(q2), d);
	KUNIT_EXPECT_TRUE(test, d->cr.linked);
	list_del_init(&b->entry);
	nvmet_tcp_free_cmd_buffers(a);
	nvmet_tcp_free_cmd_buffers(b);
	nvmet_tcp_free_cmd_buffers(d);
	KUNIT_EXPECT_TRUE(test, list_empty(&lb->waiters));
}

/*
 * No memory to copy into mid-PDU: the loans and credit go back, the rest
 * of the PDU is discarded, and the command keeps the unit (so the rest of
 * its R2T is accepted and discarded) and is not executed.
 */
static void nvmet_tcp_kunit_sink(struct kunit *test)
{
	struct nvmet_tcp_kunit *k = test->priv;
	struct nvmet_tcp_queue *queue = k->queue;
	struct nvmet_tcp_cmd *a;

	nvmet_tcp_kunit_queue_live(test, queue);
	queue->rx_budget = nvmet_tcp_kunit_budget(test, 100, 6); /* 4 KiB unit */
	a = nvmet_tcp_kunit_cmd(test, queue, 3 * 4096);
	KUNIT_ASSERT_TRUE(test, nvmet_tcp_rx_r2t_ok(a));
	KUNIT_ASSERT_TRUE(test, a->cr.linked);

	a->pdu_len = 4096;
	a->pdu_recv = 1000;
	a->rbytes_done = 1000;
	nvmet_tcp_rx_sink(a);
	KUNIT_EXPECT_FALSE(test, a->flags & NVMET_TCP_F_RX_LOAN);
	KUNIT_EXPECT_TRUE(test, a->flags & NVMET_TCP_F_RX_SINK);
	KUNIT_EXPECT_FALSE(test, a->cr.linked);
	KUNIT_EXPECT_EQ(test, iov_iter_count(&a->recv_msg.msg_iter), 3096UL);
	KUNIT_EXPECT_PTR_EQ(test, queue->rx_unit, a);

	/* Short of r2t_end: not executed, still sinking, still the owner. */
	a->pdu_recv = 2048;
	a->rbytes_done = 2048;
	KUNIT_EXPECT_TRUE(test, nvmet_tcp_rx_unit_done(a));
	KUNIT_EXPECT_TRUE(test, a->flags & NVMET_TCP_F_RX_SINK);
	KUNIT_EXPECT_PTR_EQ(test, queue->rx_unit, a);
	nvmet_tcp_free_cmd_buffers(a);
	KUNIT_EXPECT_NULL(test, queue->rx_unit);
}

/*
 * A write needing more entries than the bound is copied whole by the stock
 * path, decided up front and counted, with nothing borrowed; so is a capsule
 * larger than a unit.  Bytes past a short table's entries are counted.
 */
static void nvmet_tcp_kunit_table_cap(struct kunit *test)
{
	struct nvmet_tcp_kunit *k = test->priv;
	struct nvmet_tcp_queue *queue = k->queue;
	struct nvme_command *nc = kunit_kzalloc(test, sizeof(*nc), GFP_KERNEL);
	struct nvmet_tcp_cmd *cmd = kunit_kzalloc(test, sizeof(*cmd),
						  GFP_KERNEL);
	u64 rec0, bytes0, rec1, bytes1;

	KUNIT_ASSERT_NOT_NULL(test, nc);
	KUNIT_ASSERT_NOT_NULL(test, cmd);
	queue->rx_budget = nvmet_tcp_kunit_budget(test, 100, 66);
	queue->rx_region = true;
	nc->rw.opcode = nvme_cmd_write;
	cmd->queue = queue;
	cmd->req.cmd = nc;
	cmd->req.data_bvec_ok = true;

	/* The host default max_sectors_kb is borrowed whole. */
	KUNIT_ASSERT_TRUE(test, nvmet_tcp_rx_want(cmd, SZ_1M + SZ_256K));
	KUNIT_EXPECT_EQ(test, cmd->lt.capacity,
			nvmet_tcp_rx_entries(SZ_1M + SZ_256K));
	nvmet_tcp_free_cmd_buffers(cmd);
	KUNIT_EXPECT_FALSE(test, cmd->flags & NVMET_TCP_F_RX_LOAN);

	/* Past the cap: no table, nothing borrowed, the whole write counted. */
	rx_loan_copies_read(&rec0, &bytes0);
	KUNIT_EXPECT_FALSE(test, nvmet_tcp_rx_want(cmd, SZ_4M));
	rx_loan_copies_read(&rec1, &bytes1);
	KUNIT_EXPECT_FALSE(test, cmd->flags & NVMET_TCP_F_RX_LOAN);
	KUNIT_EXPECT_NULL(test, cmd->lt.bvec);
	KUNIT_EXPECT_EQ(test, cmd->lt.borrowed_bytes, 0U);
	KUNIT_EXPECT_EQ(test, rec1 - rec0, 1ULL);
	KUNIT_EXPECT_EQ(test, bytes1 - bytes0, (u64)SZ_4M);

	/* A capsule larger than a unit: the same, before any allocation. */
	queue->rx_budget = nvmet_tcp_kunit_budget(test, 100, 6); /* 4 KiB unit */
	cmd->pdu_len = 8192;
	rx_loan_copies_read(&rec0, &bytes0);
	KUNIT_EXPECT_FALSE(test, nvmet_tcp_rx_want(cmd, 8192));
	rx_loan_copies_read(&rec1, &bytes1);
	KUNIT_EXPECT_NULL(test, cmd->lt.bvec);
	KUNIT_EXPECT_EQ(test, rec1 - rec0, 1ULL);
	KUNIT_EXPECT_EQ(test, bytes1 - bytes0, 8192ULL);

	/* A table of one entry: the copy is counted in full all the same. */
	cmd = nvmet_tcp_kunit_cmd(test, queue, SZ_64K);
	rx_loan_table_free(&cmd->lt);
	KUNIT_ASSERT_EQ(test, rx_loan_table_alloc(&cmd->lt, 1, GFP_KERNEL,
						  NUMA_NO_NODE), 0);
	rx_loan_copies_read(&rec0, &bytes0);
	KUNIT_ASSERT_EQ(test, nvmet_tcp_rx_to_sgl(cmd), 0);
	rx_loan_copies_read(&rec1, &bytes1);
	KUNIT_EXPECT_EQ(test, rec1 - rec0, 1ULL);
	KUNIT_EXPECT_EQ(test, bytes1 - bytes0, (u64)SZ_64K);
	nvmet_tcp_free_cmd_buffers(cmd);
}

/* In-capsule data larger than a unit is copied at admission, and counted. */
static void nvmet_tcp_kunit_capsule_over_unit(struct kunit *test)
{
	struct nvmet_tcp_kunit *k = test->priv;
	struct nvmet_tcp_queue *queue = k->queue;
	u64 rec0, bytes0, rec1, bytes1;
	struct nvmet_tcp_cmd *c;

	nvmet_tcp_kunit_queue_live(test, queue);
	queue->rx_budget = nvmet_tcp_kunit_budget(test, 100, 6); /* 4 KiB unit */
	c = nvmet_tcp_kunit_cmd(test, queue, 8192);
	rx_loan_copies_read(&rec0, &bytes0);
	KUNIT_EXPECT_EQ(test, nvmet_tcp_rx_admit_capsule(c), 1);
	rx_loan_copies_read(&rec1, &bytes1);
	KUNIT_EXPECT_FALSE(test, c->flags & NVMET_TCP_F_RX_LOAN);
	KUNIT_EXPECT_FALSE(test, c->cr.linked);
	KUNIT_EXPECT_NULL(test, queue->rx_unit);
	KUNIT_EXPECT_EQ(test, rec1 - rec0, 1ULL);
	KUNIT_EXPECT_EQ(test, bytes1 - bytes0, 8192ULL);
	nvmet_tcp_free_cmd_buffers(c);
}

/*
 * A linked capsule with table room, not the oldest record, whose charge
 * fails: it copies, leaves no waiter queued, its credit (with what the
 * failed charge took) comes back at the free, and a younger waiter on
 * another queue is woken.
 */
static void nvmet_tcp_kunit_charge_fail_copies(struct kunit *test)
{
	struct nvmet_tcp_kunit *k = test->priv;
	struct nvmet_tcp_queue *queue = k->queue, *q2;
	struct nvmet_tcp_cmd *a, *c, *d;
	struct rx_loan_budget *lb;
	struct sk_buff *skb;
	long avail0;
	int stop;

	nvmet_tcp_kunit_queue_live(test, queue);
	q2 = nvmet_tcp_kunit_queue(test);
	lb = nvmet_tcp_kunit_budget(test, 100, 66);	/* 34 pages avail */
	queue->rx_budget = lb;
	q2->rx_budget = lb;
	avail0 = nvmet_tcp_kunit_avail(lb);

	/* a owns the unit and is the oldest record. */
	a = nvmet_tcp_kunit_cmd(test, queue, 3 * 4096);	/* cost 14 */
	KUNIT_ASSERT_TRUE(test, nvmet_tcp_rx_r2t_ok(a));
	/* c is admitted behind it at its table's cost, without waiting. */
	c = nvmet_tcp_kunit_cmd(test, queue, 3000);	/* cost 5 */
	KUNIT_ASSERT_EQ(test, nvmet_tcp_rx_admit_capsule(c), 0);
	KUNIT_ASSERT_TRUE(test, c->cr.linked);
	/* d, on another queue, costs more than is left: it waits. */
	d = nvmet_tcp_kunit_cmd(test, q2, 16384);	/* cost 18 */
	KUNIT_ASSERT_FALSE(test, nvmet_tcp_rx_r2t_ok(d));
	KUNIT_ASSERT_TRUE(test, rx_loan_waiting(&q2->rx_wait));

	/* c's next skb would hold more region pages than avail has. */
	c->lt.region_count = c->cr.credit + nvmet_tcp_kunit_avail(lb) + 4;
	skb = nvmet_tcp_kunit_skb(test, k->page);
	KUNIT_EXPECT_EQ(test, nvmet_tcp_kunit_actor(c, skb, &stop), 0);
	kfree_skb(skb);
	KUNIT_EXPECT_EQ(test, stop, NVMET_TCP_RX_COPY);
	KUNIT_EXPECT_EQ(test, c->lt.count, 0U);
	KUNIT_EXPECT_FALSE(test, rx_loan_waiting(&queue->rx_wait));
	KUNIT_EXPECT_TRUE(test, rx_loan_waiting(&q2->rx_wait));

	/* c's free returns its credit: d, younger, is woken and admitted. */
	c->lt.region_count = 0;
	nvmet_tcp_free_cmd_buffers(c);
	KUNIT_EXPECT_FALSE(test, c->cr.linked);
	flush_work(&lb->kick_work);
	flush_work(&q2->io_work);
	KUNIT_EXPECT_TRUE(test, READ_ONCE(q2->rx_kicked));
	KUNIT_EXPECT_PTR_EQ(test, nvmet_tcp_rx_next_withheld(q2), d);
	KUNIT_EXPECT_TRUE(test, d->cr.linked);
	nvmet_tcp_free_cmd_buffers(d);
	nvmet_tcp_free_cmd_buffers(a);
	KUNIT_EXPECT_EQ(test, nvmet_tcp_kunit_avail(lb), avail0);
	KUNIT_EXPECT_TRUE(test, list_empty(&lb->waiters));
	KUNIT_EXPECT_EQ(test, page_ref_count(k->page), 1);
}

/*
 * A linked capsule that does not own the unit, offered more than its table
 * holds: it copies, never charges past its credit, never waits.
 */
static void nvmet_tcp_kunit_overshoot_copies(struct kunit *test)
{
	struct nvmet_tcp_kunit *k = test->priv;
	struct nvmet_tcp_queue *queue = k->queue;
	struct nvmet_tcp_cmd *a, *c;
	struct sk_buff *skb;
	int stop;

	nvmet_tcp_kunit_queue_live(test, queue);
	queue->rx_budget = nvmet_tcp_kunit_budget(test, 100, 6);
	a = nvmet_tcp_kunit_cmd(test, queue, 3 * 4096);
	KUNIT_ASSERT_TRUE(test, nvmet_tcp_rx_r2t_ok(a));

	c = kunit_kzalloc(test, sizeof(*c), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, c);
	c->queue = queue;
	c->req.transfer_len = 3000;
	c->pdu_len = 3000;
	KUNIT_ASSERT_EQ(test, rx_loan_table_alloc(&c->lt, 1, GFP_KERNEL,
						  NUMA_NO_NODE), 0);
	c->flags = NVMET_TCP_F_RX_LOAN;
	KUNIT_ASSERT_EQ(test, nvmet_tcp_rx_admit_capsule(c), 0);
	KUNIT_ASSERT_TRUE(test, c->cr.linked);
	KUNIT_EXPECT_PTR_EQ(test, queue->rx_unit, a);

	/* Two frags, two entries, a table of one. */
	skb = nvmet_tcp_kunit_skb(test, k->page);
	KUNIT_EXPECT_EQ(test, nvmet_tcp_kunit_actor(c, skb, &stop), 0);
	kfree_skb(skb);
	KUNIT_EXPECT_EQ(test, stop, NVMET_TCP_RX_COPY);
	KUNIT_EXPECT_EQ(test, c->lt.count, 0U);
	KUNIT_EXPECT_FALSE(test, rx_loan_waiting(&queue->rx_wait));
	nvmet_tcp_free_cmd_buffers(c);
	nvmet_tcp_free_cmd_buffers(a);
	KUNIT_EXPECT_EQ(test, page_ref_count(k->page), 1);
}

/*
 * A request that cannot take the loan's layout (here, an sgl-only command)
 * has its loans tiled into the stock sgl at the finish: a counted copy, to
 * nvmet-tcp and the host-wide total, the loans back before the free.
 */
static void nvmet_tcp_kunit_finish_tiles(struct kunit *test)
{
	struct nvmet_tcp_kunit *k = test->priv;
	struct nvmet_tcp_cmd *cmd = nvmet_tcp_kunit_cmd(test, k->queue, 6000);
	u64 rec0, bytes0, rec1, bytes1, own0, obytes0, own1, obytes1;
	struct sk_buff *skb;
	u8 buf[64];
	int stop;

	/* Two skbs: the second's bytes cross into the sgl's second page. */
	memset(page_address(k->page) + 1448, 0xa5, 1552);
	skb = nvmet_tcp_kunit_skb(test, k->page);
	KUNIT_ASSERT_EQ(test, nvmet_tcp_kunit_actor(cmd, skb, &stop), 3000);
	kfree_skb(skb);
	skb = nvmet_tcp_kunit_skb(test, k->page);
	KUNIT_ASSERT_EQ(test, nvmet_tcp_kunit_actor(cmd, skb, &stop), 3000);
	kfree_skb(skb);
	KUNIT_ASSERT_EQ(test, cmd->lt.body_bytes, 6000U);
	cmd->req.data_bvec_ok = false;

	rx_loan_copies_read(&rec0, &bytes0);
	nvmet_tcp_kunit_own_copies(test, &own0, &obytes0);
	KUNIT_ASSERT_EQ(test, nvmet_tcp_rx_finish(cmd), 0);
	rx_loan_copies_read(&rec1, &bytes1);
	nvmet_tcp_kunit_own_copies(test, &own1, &obytes1);
	KUNIT_EXPECT_GE(test, rec1 - rec0, 1ULL);
	KUNIT_EXPECT_GE(test, bytes1 - bytes0, 6000ULL);
	KUNIT_EXPECT_GE(test, own1 - own0, 1ULL);
	KUNIT_EXPECT_GE(test, obytes1 - obytes0, 6000ULL);
	KUNIT_EXPECT_NULL(test, cmd->req.data_bvec);
	KUNIT_EXPECT_FALSE(test, cmd->flags & NVMET_TCP_F_RX_LOAN);
	KUNIT_EXPECT_EQ(test, page_ref_count(k->page), 1);
	KUNIT_ASSERT_NOT_NULL(test, cmd->req.sg);
	/* Page tiled: byte 4500 is the second skb's 1500th, of 0xa5. */
	sg_pcopy_to_buffer(cmd->req.sg, cmd->req.sg_cnt, buf, 1, 1440);
	KUNIT_EXPECT_EQ(test, buf[0], 0x5a);
	sg_pcopy_to_buffer(cmd->req.sg, cmd->req.sg_cnt, buf, 1, 1448);
	KUNIT_EXPECT_EQ(test, buf[0], 0xa5);
	sg_pcopy_to_buffer(cmd->req.sg, cmd->req.sg_cnt, buf, 1, 4500);
	KUNIT_EXPECT_EQ(test, buf[0], 0xa5);
	sg_pcopy_to_buffer(cmd->req.sg, cmd->req.sg_cnt, buf, 1, 3100);
	KUNIT_EXPECT_EQ(test, buf[0], 0x5a);
	nvmet_tcp_free_cmd_buffers(cmd);
}

/*
 * A queue's hold: not marked nor measured until region pages are seen;
 * then each skb read is measured, a full window makes a sample that the
 * budget learns, and a held factor an eighth off the measured one is
 * marked to be held again.
 */
static void nvmet_tcp_kunit_hold_account(struct kunit *test)
{
	struct nvmet_tcp_kunit *k = test->priv;
	struct nvmet_tcp_queue *queue = k->queue;
	struct rx_loan_hold *h = &queue->rx_hold;
	struct sk_buff *skb = nvmet_tcp_kunit_skb(test, k->page);
	unsigned int want, i;

	queue->rx_budget = nvmet_tcp_kunit_budget(test, 100, 66);
	want = clamp_t(unsigned int, DIV_ROUND_UP(skb->truesize * 100, 3000),
		       100, RX_LOAN_HOLD_FACTOR_MAX);

	/* No region pages: nothing marked, nothing measured. */
	nvmet_tcp_rx_hold_account(queue, skb, 3000, false);
	KUNIT_EXPECT_FALSE(test, h->pending);
	KUNIT_EXPECT_EQ(test, h->geo_pay, 0U);

	/* Region pages: marked, and measured from this skb on. */
	nvmet_tcp_rx_hold_account(queue, skb, 3000, true);
	KUNIT_EXPECT_TRUE(test, h->pending);
	KUNIT_EXPECT_EQ(test, h->geo_pay, 3000U);
	KUNIT_EXPECT_EQ(test, rx_loan_hold_measured(h), 0U);
	KUNIT_EXPECT_EQ(test, rx_loan_hold_factor(h, queue->rx_budget),
			RX_LOAN_HOLD_FACTOR_UNLEARNED);

	for (i = 1; i < DIV_ROUND_UP(4U << 20, 3000); i++)
		nvmet_tcp_rx_hold_account(queue, skb, 3000, false);
	KUNIT_EXPECT_EQ(test, rx_loan_hold_measured(h), want);
	KUNIT_EXPECT_EQ(test, READ_ONCE(queue->rx_budget->hold_factor), want);
	KUNIT_EXPECT_EQ(test, h->geo_pay, 0U);

	/* Held at a factor well off the measured one: marked again. */
	h->pending = false;
	h->held = true;
	h->factor = want * 2;
	nvmet_tcp_rx_hold_account(queue, skb, 3000, false);
	KUNIT_EXPECT_FALSE(test, h->held);
	KUNIT_EXPECT_TRUE(test, h->pending);
	kfree_skb(skb);
}

/* Applying a queue's hold locks its socket's receive buffer, once. */
static void nvmet_tcp_kunit_hold_apply(struct kunit *test)
{
	struct nvmet_tcp_kunit *k = test->priv;
	struct nvmet_tcp_queue *queue = k->queue;
	struct rx_loan_hold *h = &queue->rx_hold;
	struct sock *sk;

	nvmet_tcp_kunit_queue_live(test, queue);
	queue->rx_budget = nvmet_tcp_kunit_budget(test, 100, 66);
	sk = queue->sock->sk;
	h->pending = true;
	nvmet_tcp_rx_hold_apply(h, sk, queue->rx_budget);
	KUNIT_EXPECT_TRUE(test, h->held);
	KUNIT_EXPECT_FALSE(test, h->pending);
	KUNIT_EXPECT_EQ(test, h->factor, RX_LOAN_HOLD_FACTOR_UNLEARNED);
	KUNIT_EXPECT_TRUE(test, sk->sk_userlocks & SOCK_RCVBUF_LOCK);
	KUNIT_EXPECT_EQ(test, READ_ONCE(sk->sk_rcvbuf), NVMET_TCP_RX_RCVBUF);
	KUNIT_EXPECT_EQ(test, READ_ONCE(tcp_sk(sk)->scaling_ratio_max),
			rx_loan_hold_scaling_ratio(RX_LOAN_HOLD_FACTOR_UNLEARNED));

	/* Held already: nothing done. */
	WRITE_ONCE(sk->sk_rcvbuf, SZ_64K);
	nvmet_tcp_rx_hold_apply(h, sk, queue->rx_budget);
	KUNIT_EXPECT_EQ(test, READ_ONCE(sk->sk_rcvbuf), SZ_64K);
}

/*
 * A queue that will never lend (here: a data digest) gives back the hold
 * its socket inherited from a held listener; an unheld socket is left as
 * it is.
 */
static void nvmet_tcp_kunit_hold_release(struct kunit *test)
{
	struct nvmet_tcp_kunit *k = test->priv;
	struct nvmet_tcp_queue *queue = k->queue;
	struct rx_loan_hold *h = &queue->rx_hold;
	struct sock *sk;
	int rmem;

	nvmet_tcp_kunit_queue_live(test, queue);
	sk = queue->sock->sk;
	rmem = READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_rmem[1]);
	queue->rx_budget = nvmet_tcp_kunit_budget(test, 100, 66);
	nvmet_tcp_rx_hold_apply(h, sk, queue->rx_budget);
	KUNIT_ASSERT_TRUE(test, sk->sk_userlocks & SOCK_RCVBUF_LOCK);
	queue->rx_budget = NULL;
	queue->rx_region = false;
	queue->data_digest = true;
	nvmet_tcp_rx_setup(queue);
	KUNIT_EXPECT_FALSE(test, queue->rx_region);
	KUNIT_EXPECT_FALSE(test, sk->sk_userlocks & SOCK_RCVBUF_LOCK);
	KUNIT_EXPECT_EQ(test, READ_ONCE(tcp_sk(sk)->scaling_ratio_max), 0);
	KUNIT_EXPECT_GE(test, READ_ONCE(sk->sk_rcvbuf),
			max_t(int, NVMET_TCP_RX_RCVBUF, rmem));
	/* The clamp follows the buffer again. */
	KUNIT_EXPECT_GE(test, READ_ONCE(tcp_sk(sk)->window_clamp),
			(u32)tcp_win_from_space(sk, READ_ONCE(sk->sk_rcvbuf)));

	/* Not locked, at tcp_rmem[1] or over: nothing done. */
	WRITE_ONCE(sk->sk_rcvbuf, 2 * rmem);
	rx_loan_hold_release(sk);
	KUNIT_EXPECT_EQ(test, READ_ONCE(sk->sk_rcvbuf), 2 * rmem);
	/* Not locked, under it (a held listener's buffer): raised. */
	WRITE_ONCE(sk->sk_rcvbuf, SZ_64K);
	rx_loan_hold_release(sk);
	KUNIT_EXPECT_EQ(test, READ_ONCE(sk->sk_rcvbuf), max(rmem, SZ_64K));
	queue->data_digest = false;
}

static void nvmet_tcp_kunit_sock_release(void *data)
{
	sock_release(data);
}

/*
 * A held listener sets only its buffer and ratio, so SYNs keep a stock
 * window scale; released, it is stock again.
 */
static void nvmet_tcp_kunit_listener_release(struct kunit *test)
{
	struct sockaddr_in sin = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_ANY),
	};
	struct rx_loan_hold h = {};
	struct socket *sock;
	struct sock *sk;

	KUNIT_ASSERT_EQ(test, sock_create_kern(&init_net, PF_INET, SOCK_STREAM,
					       IPPROTO_TCP, &sock), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			nvmet_tcp_kunit_sock_release, sock), 0);
	sk = sock->sk;
	KUNIT_ASSERT_EQ(test, kernel_bind(sock, (struct sockaddr_unsized *)&sin,
					  sizeof(sin)), 0);
	KUNIT_ASSERT_EQ(test, kernel_listen(sock, 1), 0);
	KUNIT_EXPECT_EQ(test, rx_loan_hold_listen(&h, sk, NVMET_TCP_RX_RCVBUF,
						  150), 0);
	KUNIT_EXPECT_TRUE(test, h.held);
	KUNIT_EXPECT_EQ(test, READ_ONCE(sk->sk_rcvbuf), NVMET_TCP_RX_RCVBUF);
	KUNIT_EXPECT_EQ(test, tcp_sk(sk)->scaling_ratio,
			rx_loan_hold_scaling_ratio(150));
	KUNIT_EXPECT_FALSE(test, sk->sk_userlocks & SOCK_RCVBUF_LOCK);
	KUNIT_EXPECT_EQ(test, READ_ONCE(tcp_sk(sk)->window_clamp), 0);
	KUNIT_EXPECT_EQ(test, READ_ONCE(tcp_sk(sk)->scaling_ratio_max), 0);
	KUNIT_EXPECT_EQ(test, rx_loan_hold_listen(&h, sk, NVMET_TCP_RX_RCVBUF,
						  150), -EALREADY);

	rx_loan_hold_release(sk);
	KUNIT_EXPECT_FALSE(test, sk->sk_userlocks & SOCK_RCVBUF_LOCK);
	KUNIT_EXPECT_EQ(test, READ_ONCE(tcp_sk(sk)->window_clamp), 0);
	KUNIT_EXPECT_EQ(test, READ_ONCE(tcp_sk(sk)->scaling_ratio_max), 0);
	KUNIT_EXPECT_EQ(test, tcp_sk(sk)->scaling_ratio,
			TCP_DEFAULT_SCALING_RATIO);
	KUNIT_EXPECT_EQ(test, READ_ONCE(sk->sk_rcvbuf),
			READ_ONCE(init_net.ipv4.sysctl_tcp_rmem[1]));
}

struct nvmet_tcp_kunit_knfsd {
	struct rx_loan_budget	*lb;
	unsigned long		pages;
	unsigned int		reserve;
	bool			off;
};

static void nvmet_tcp_kunit_knfsd_restore(void *data)
{
	struct nvmet_tcp_kunit_knfsd *k = data;

	rx_loan_budget_request(k->lb, k->pages, k->reserve, k->off);
}

static void nvmet_tcp_kunit_budget_destroy(void *data)
{
	rx_loan_budget_destroy(data);
}

/*
 * nvmet-tcp's write budget is its own: kNFSd's turned off leaves nvmet-tcp
 * the region's write share, and nvmet-tcp's budget changes nothing of
 * kNFSd's.  The module's budget is set up the same way.
 */
static void nvmet_tcp_kunit_own_budget(struct kunit *test)
{
	struct rx_loan_budget *knfsd = &rx_loan_net(&init_net)->write;
	bool cfg_default = READ_ONCE(knfsd->cfg_default);
	struct nvmet_tcp_kunit_knfsd *k;
	struct rx_loan_budget *lb;
	int ret;

	k = kunit_kzalloc(test, sizeof(*k), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, k);
	lb = kunit_kzalloc(test, sizeof(*lb), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, lb);
	k->lb = knfsd;
	k->pages = READ_ONCE(knfsd->cfg_pages);
	k->reserve = READ_ONCE(knfsd->cfg_reserve);
	k->off = READ_ONCE(knfsd->cfg_off);
	/* Actions run last first: kNFSd's restore after the region's. */
	ret = kunit_add_action_or_reset(test, nvmet_tcp_kunit_knfsd_restore, k);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = kunit_add_action_or_reset(test, nvmet_tcp_kunit_region_reset, NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);
	rx_loan_kunit_region(1UL << 16, 1UL << 12);
	KUNIT_ASSERT_EQ(test, nvmet_tcp_rx_budget_init(lb), 0);
	ret = kunit_add_action_or_reset(test, nvmet_tcp_kunit_budget_destroy, lb);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, READ_ONCE(lb->cfg_default));
	KUNIT_EXPECT_TRUE(test, lb->write_capped);
	KUNIT_EXPECT_FALSE(test, lb->read);

	rx_loan_budget_request(knfsd, k->pages, k->reserve, true);
	KUNIT_EXPECT_EQ(test, READ_ONCE(knfsd->budget), 0U);
	rx_loan_budget_reconfigure(lb);
	KUNIT_EXPECT_EQ(test, READ_ONCE(lb->budget),
			(unsigned int)((1UL << 16) - (1UL << 12)));
	KUNIT_EXPECT_EQ(test, READ_ONCE(knfsd->cfg_default), cfg_default);
	KUNIT_EXPECT_FALSE(test, knfsd->write_capped);

	/* The module's queues draw on a budget set up the same way. */
	KUNIT_EXPECT_TRUE(test, READ_ONCE(nvmet_tcp_rx_budget.cfg_default));
	KUNIT_EXPECT_TRUE(test, nvmet_tcp_rx_budget.write_capped);
	KUNIT_EXPECT_FALSE(test, nvmet_tcp_rx_budget.read);
	KUNIT_EXPECT_TRUE(test, READ_ONCE(nvmet_tcp_rx_budget.stock) != NULL);
}

/* A stand-in READ partition of @n pages, gone (pages freed) at test end. */
struct nvmet_tcp_kunit_tx_region {
	struct page	**pages;
	unsigned int	n;
};

static void nvmet_tcp_kunit_tx_region_free(void *data)
{
	struct nvmet_tcp_kunit_tx_region *r = data;
	unsigned int i;

	rx_loan_tx_flush();
	rx_loan_kunit_tx_region(NULL, 0);
	for (i = 0; i < r->n; i++)
		if (r->pages[i])
			__free_page(r->pages[i]);
}

static void nvmet_tcp_kunit_tx_region(struct kunit *test, unsigned int n)
{
	struct nvmet_tcp_kunit_tx_region *r;
	unsigned int i;
	int ret;

	r = kunit_kzalloc(test, sizeof(*r), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, r);
	r->pages = kunit_kcalloc(test, n, sizeof(*r->pages), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, r->pages);
	r->n = n;
	ret = kunit_add_action_or_reset(test, nvmet_tcp_kunit_tx_region_free,
					r);
	KUNIT_ASSERT_EQ(test, ret, 0);
	for (i = 0; i < n; i++) {
		r->pages[i] = alloc_page(GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, r->pages[i]);
	}
	rx_loan_kunit_tx_region(r->pages, n);
}

/*
 * A READ budget over the stand-in region, sized as admission sizes it (the
 * region less the write budgets) before the test reads its credit.
 */
static struct rx_loan_budget *nvmet_tcp_kunit_read_budget(struct kunit *test)
{
	struct rx_loan_budget *lb = nvmet_tcp_kunit_budget(test, 64, 0);

	lb->read = true;
	spin_lock_bh(&lb->lock);
	rx_loan_budget_update_locked(lb);
	spin_unlock_bh(&lb->lock);
	return lb;
}

/* A READ of @bytes on a queue that lends, to a namespace taking bvecs. */
static struct nvmet_tcp_cmd *nvmet_tcp_kunit_read(struct kunit *test,
						  u32 bytes)
{
	struct nvmet_tcp_cmd *cmd = kunit_kzalloc(test, sizeof(*cmd),
						  GFP_KERNEL);
	struct nvme_command *sqe = kunit_kzalloc(test, sizeof(*sqe),
						 GFP_KERNEL);
	struct nvmet_ns *ns = kunit_kzalloc(test, sizeof(*ns), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, cmd);
	KUNIT_ASSERT_NOT_NULL(test, sqe);
	KUNIT_ASSERT_NOT_NULL(test, ns);
	cmd->queue = nvmet_tcp_kunit_queue(test);
	cmd->queue->rx_region = true;
	sqe->common.opcode = nvme_cmd_read;
	ns->data_bvec_ok = true;
	cmd->req.cmd = sqe;
	cmd->req.ns = ns;
	cmd->req.data_bvec_ok = true;
	cmd->req.transfer_len = bytes;
	return cmd;
}

static void nvmet_tcp_kunit_tx_delta(struct nvmet_tcp_tx_stats *base,
				     struct nvmet_tcp_tx_stats *d)
{
	nvmet_tcp_tx_sum(d);
	d->confined -= base->confined;
	d->unconfined -= base->unconfined;
	d->sends -= base->sends;
	d->copies -= base->copies;
	d->copy_pages -= base->copy_pages;
}

/*
 * A READ is served from READ-partition pages under exact credit; TCP's
 * references outlive the command's, and the pages and credit come back
 * only with the last of them.
 */
static void nvmet_tcp_kunit_tx_confined(struct kunit *test)
{
	struct rx_loan_budget *lb = nvmet_tcp_kunit_read_budget(test);
	u32 bytes = 3 * PAGE_SIZE + 100;
	struct nvmet_tcp_tx_stats base, d;
	struct nvmet_tcp_cmd *cmd;
	struct ubuf_info *ubuf;
	long avail;

	nvmet_tcp_kunit_tx_region(test, 8);
	cmd = nvmet_tcp_kunit_read(test, bytes);
	avail = nvmet_tcp_kunit_avail(lb);
	nvmet_tcp_tx_sum(&base);

	KUNIT_ASSERT_TRUE(test, nvmet_tcp_tx_want(cmd, bytes, lb));
	KUNIT_EXPECT_TRUE(test, cmd->flags & NVMET_TCP_F_TX_LOAN);
	KUNIT_ASSERT_NOT_NULL(test, cmd->tx);
	KUNIT_EXPECT_NULL(test, cmd->req.sg);
	KUNIT_EXPECT_NULL(test, cmd->cur_sg);
	KUNIT_EXPECT_PTR_EQ(test, cmd->req.data_bvec, cmd->iov);
	KUNIT_EXPECT_EQ(test, cmd->req.data_bvec_cnt, 4U);
	KUNIT_EXPECT_EQ(test, cmd->req.data_bvec[3].bv_len, 100U);
	KUNIT_EXPECT_EQ(test, rx_loan_kunit_tx_free(), 4U);
	KUNIT_EXPECT_EQ(test, nvmet_tcp_kunit_avail(lb), avail - 4);
	nvmet_tcp_kunit_tx_delta(&base, &d);
	KUNIT_EXPECT_EQ(test, d.confined, 1ULL);
	KUNIT_EXPECT_EQ(test, d.unconfined, 0ULL);
	KUNIT_EXPECT_EQ(test, d.sends, 1L);

	/* Sent in two skbs, then the command is done. */
	ubuf = &cmd->tx->tx.ubuf;
	refcount_add(2, &ubuf->refcnt);
	nvmet_tcp_free_cmd_buffers(cmd);
	KUNIT_EXPECT_FALSE(test, cmd->flags & NVMET_TCP_F_TX_LOAN);
	KUNIT_EXPECT_NULL(test, cmd->tx);
	KUNIT_EXPECT_NULL(test, cmd->iov);
	rx_loan_tx_flush();
	KUNIT_EXPECT_EQ(test, rx_loan_kunit_tx_free(), 4U);

	ubuf->ops->complete(NULL, ubuf, true);
	rx_loan_tx_flush();
	KUNIT_EXPECT_EQ(test, rx_loan_kunit_tx_free(), 4U);
	ubuf->ops->complete(NULL, ubuf, true);
	rx_loan_tx_flush();
	KUNIT_EXPECT_EQ(test, rx_loan_kunit_tx_free(), 8U);
	KUNIT_EXPECT_EQ(test, nvmet_tcp_kunit_avail(lb), avail);
	nvmet_tcp_kunit_tx_delta(&base, &d);
	KUNIT_EXPECT_EQ(test, d.sends, 0L);
}

/* A command freed before its data went out returns at once. */
static void nvmet_tcp_kunit_tx_unsent(struct kunit *test)
{
	struct rx_loan_budget *lb = nvmet_tcp_kunit_read_budget(test);
	struct nvmet_tcp_cmd *cmd;
	long avail;

	nvmet_tcp_kunit_tx_region(test, 4);
	cmd = nvmet_tcp_kunit_read(test, 2 * PAGE_SIZE);
	avail = nvmet_tcp_kunit_avail(lb);
	KUNIT_ASSERT_TRUE(test, nvmet_tcp_tx_want(cmd, 2 * PAGE_SIZE, lb));
	KUNIT_EXPECT_EQ(test, rx_loan_kunit_tx_free(), 2U);
	nvmet_tcp_free_cmd_buffers(cmd);
	rx_loan_tx_flush();
	KUNIT_EXPECT_EQ(test, rx_loan_kunit_tx_free(), 4U);
	KUNIT_EXPECT_EQ(test, nvmet_tcp_kunit_avail(lb), avail);
}

/*
 * Pages short: the stock sgl, the credit returned at once; a READ larger
 * than the partition never asks for credit.  Neither waits.
 */
static void nvmet_tcp_kunit_tx_unconfined(struct kunit *test)
{
	struct rx_loan_budget *lb = nvmet_tcp_kunit_read_budget(test);
	struct nvmet_tcp_cmd *first, *cmd, *big;
	struct nvmet_tcp_tx_stats base, d;
	long avail;

	nvmet_tcp_kunit_tx_region(test, 4);
	first = nvmet_tcp_kunit_read(test, 3 * PAGE_SIZE);
	cmd = nvmet_tcp_kunit_read(test, 2 * PAGE_SIZE);
	big = nvmet_tcp_kunit_read(test, 5 * PAGE_SIZE);
	avail = nvmet_tcp_kunit_avail(lb);
	nvmet_tcp_tx_sum(&base);

	KUNIT_ASSERT_TRUE(test, nvmet_tcp_tx_want(first, 3 * PAGE_SIZE, lb));
	KUNIT_EXPECT_FALSE(test, nvmet_tcp_tx_want(cmd, 2 * PAGE_SIZE, lb));
	KUNIT_EXPECT_FALSE(test, cmd->flags & NVMET_TCP_F_TX_LOAN);
	KUNIT_EXPECT_NULL(test, cmd->iov);
	KUNIT_EXPECT_NULL(test, cmd->req.data_bvec);
	KUNIT_EXPECT_EQ(test, rx_loan_kunit_tx_free(), 1U);
	KUNIT_EXPECT_EQ(test, nvmet_tcp_kunit_avail(lb), avail - 3);

	KUNIT_EXPECT_FALSE(test, nvmet_tcp_tx_want(big, 5 * PAGE_SIZE, lb));
	KUNIT_EXPECT_EQ(test, nvmet_tcp_kunit_avail(lb), avail - 3);
	nvmet_tcp_kunit_tx_delta(&base, &d);
	KUNIT_EXPECT_EQ(test, d.confined, 1ULL);
	KUNIT_EXPECT_EQ(test, d.unconfined, 2ULL);

	nvmet_tcp_free_cmd_buffers(first);
	rx_loan_tx_flush();
	KUNIT_EXPECT_EQ(test, rx_loan_kunit_tx_free(), 4U);
	KUNIT_EXPECT_EQ(test, nvmet_tcp_kunit_avail(lb), avail);
}

/*
 * The backend refuses the bvec (a misaligned length): the send is
 * undone, pages and credit back, and the READ takes the stock sgl.
 */
static void nvmet_tcp_kunit_tx_bvec_refused(struct kunit *test)
{
	struct rx_loan_budget *lb = nvmet_tcp_kunit_read_budget(test);
	struct nvmet_tcp_tx_stats base, d;
	struct nvmet_tcp_cmd *cmd;
	long avail;

	nvmet_tcp_kunit_tx_region(test, 4);
	cmd = nvmet_tcp_kunit_read(test, PAGE_SIZE + 4);
	cmd->req.ns->data_bvec_len_mask = 511;
	avail = nvmet_tcp_kunit_avail(lb);
	nvmet_tcp_tx_sum(&base);
	KUNIT_EXPECT_FALSE(test, nvmet_tcp_tx_want(cmd, PAGE_SIZE + 4, lb));
	KUNIT_EXPECT_FALSE(test, cmd->flags & NVMET_TCP_F_TX_LOAN);
	KUNIT_EXPECT_NULL(test, cmd->iov);
	KUNIT_EXPECT_NULL(test, cmd->tx);
	rx_loan_tx_flush();
	KUNIT_EXPECT_EQ(test, rx_loan_kunit_tx_free(), 4U);
	KUNIT_EXPECT_EQ(test, nvmet_tcp_kunit_avail(lb), avail);
	nvmet_tcp_kunit_tx_delta(&base, &d);
	KUNIT_EXPECT_EQ(test, d.unconfined, 1ULL);
	KUNIT_EXPECT_EQ(test, d.sends, 0L);
}

/*
 * A send whose pages TCP copied (zerocopy_success false on one of its
 * skbs) is counted with its pages at release; one zero copy is not.
 */
static void nvmet_tcp_kunit_tx_copied(struct kunit *test)
{
	struct rx_loan_budget *lb = nvmet_tcp_kunit_read_budget(test);
	struct nvmet_tcp_cmd *zc, *cp;
	struct nvmet_tcp_tx_stats base, d;
	struct ubuf_info *ubuf;

	nvmet_tcp_kunit_tx_region(test, 8);
	zc = nvmet_tcp_kunit_read(test, 2 * PAGE_SIZE);
	cp = nvmet_tcp_kunit_read(test, 3 * PAGE_SIZE);
	nvmet_tcp_tx_sum(&base);
	KUNIT_ASSERT_TRUE(test, nvmet_tcp_tx_want(zc, 2 * PAGE_SIZE, lb));
	KUNIT_ASSERT_TRUE(test, nvmet_tcp_tx_want(cp, 3 * PAGE_SIZE, lb));

	ubuf = &zc->tx->tx.ubuf;
	refcount_inc(&ubuf->refcnt);
	nvmet_tcp_free_cmd_buffers(zc);
	ubuf->ops->complete(NULL, ubuf, true);

	/* Two skbs: the first copied, the last zero copy. */
	ubuf = &cp->tx->tx.ubuf;
	refcount_add(2, &ubuf->refcnt);
	nvmet_tcp_free_cmd_buffers(cp);
	ubuf->ops->complete(NULL, ubuf, false);
	ubuf->ops->complete(NULL, ubuf, true);
	rx_loan_tx_flush();
	KUNIT_EXPECT_EQ(test, rx_loan_kunit_tx_free(), 8U);
	nvmet_tcp_kunit_tx_delta(&base, &d);
	KUNIT_EXPECT_EQ(test, d.sends, 0L);
	KUNIT_EXPECT_EQ(test, d.copies, 1ULL);
	KUNIT_EXPECT_EQ(test, d.copy_pages, 3ULL);
}

/* An skb that carries @ubuf's send, as TCP's first zero-copy append. */
static struct sk_buff *nvmet_tcp_kunit_tx_skb(struct kunit *test,
					      struct ubuf_info *ubuf)
{
	struct sk_buff *skb = alloc_skb(0, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, skb);
	KUNIT_ASSERT_NOT_NULL(test, ubuf->ops->link_skb);
	KUNIT_ASSERT_EQ(test, ubuf->ops->link_skb(skb, ubuf), 0);
	KUNIT_ASSERT_PTR_EQ(test, skb_zcopy(skb), ubuf);
	return skb;
}

/* Chaining is built in only with CONFIG_RX_LOAN_TX_LINK. */
#define NVMET_TCP_KUNIT_NEED_TX_LINK(test)				\
	do {								\
		if (!IS_ENABLED(CONFIG_RX_LOAN_TX_LINK))		\
			kunit_skip(test, "CONFIG_RX_LOAN_TX_LINK=n");	\
	} while (0)

/*
 * Consecutive READs share skbs (->link_skb): each READ's pages and credit
 * come back only when the last skb carrying any of the chain is freed,
 * whichever skb that is, and not with the command.
 */
static void nvmet_tcp_kunit_tx_linked(struct kunit *test)
{
	struct rx_loan_budget *lb = nvmet_tcp_kunit_read_budget(test);
	struct nvmet_tcp_cmd *a, *b;
	struct ubuf_info *ua, *ub;
	struct sk_buff *s1, *s2;
	long avail;
	int order;

	NVMET_TCP_KUNIT_NEED_TX_LINK(test);
	nvmet_tcp_kunit_tx_region(test, 8);
	avail = nvmet_tcp_kunit_avail(lb);
	for (order = 0; order < 2; order++) {
		a = nvmet_tcp_kunit_read(test, 2 * PAGE_SIZE);
		b = nvmet_tcp_kunit_read(test, 2 * PAGE_SIZE);
		KUNIT_ASSERT_TRUE(test, nvmet_tcp_tx_want(a, 2 * PAGE_SIZE, lb));
		KUNIT_ASSERT_TRUE(test, nvmet_tcp_tx_want(b, 2 * PAGE_SIZE, lb));
		ua = &a->tx->tx.ubuf;
		ub = &b->tx->tx.ubuf;

		/* s1 carries a's data then b's head; s2 the rest of b's. */
		s1 = nvmet_tcp_kunit_tx_skb(test, ua);
		KUNIT_ASSERT_EQ(test, ub->ops->link_skb(s1, ub), 0);
		KUNIT_EXPECT_PTR_EQ(test, skb_zcopy(s1), ua);
		KUNIT_EXPECT_EQ(test, ua->ops->link_skb(s1, ua), 0);
		s2 = nvmet_tcp_kunit_tx_skb(test, ub);

		nvmet_tcp_free_cmd_buffers(a);
		nvmet_tcp_free_cmd_buffers(b);
		rx_loan_tx_flush();
		KUNIT_EXPECT_EQ(test, rx_loan_kunit_tx_free(), 4U);
		KUNIT_EXPECT_EQ(test, nvmet_tcp_kunit_avail(lb), avail - 4);

		kfree_skb(order ? s2 : s1);
		rx_loan_tx_flush();
		KUNIT_EXPECT_EQ(test, rx_loan_kunit_tx_free(), 4U);
		KUNIT_EXPECT_EQ(test, nvmet_tcp_kunit_avail(lb), avail - 4);

		kfree_skb(order ? s1 : s2);
		rx_loan_tx_flush();
		KUNIT_EXPECT_EQ(test, rx_loan_kunit_tx_free(), 8U);
		KUNIT_EXPECT_EQ(test, nvmet_tcp_kunit_avail(lb), avail);
	}
}

static void nvmet_tcp_kunit_other_complete(struct sk_buff *skb,
					   struct ubuf_info *uarg,
					   bool zerocopy_success)
{
	if (refcount_dec_and_test(&uarg->refcnt))
		return;
}

static const struct ubuf_info_ops nvmet_tcp_kunit_other_ops = {
	.complete = nvmet_tcp_kunit_other_complete,
};

/*
 * A READ starts its own skb behind another provider's send, behind a send
 * whose chain is full, and when it is in a chain already; each refused READ
 * comes back on its own.
 */
static void nvmet_tcp_kunit_tx_link_refused(struct kunit *test)
{
	struct rx_loan_budget *lb = nvmet_tcp_kunit_read_budget(test);
	struct nvmet_tcp_cmd *cmd[RX_LOAN_KUNIT_TX_CHAIN_MAX + 2];
	struct ubuf_info other = {
		.ops = &nvmet_tcp_kunit_other_ops,
		.flags = SKBFL_ZEROCOPY_FRAG,
		.refcnt = REFCOUNT_INIT(1),
	};
	struct sk_buff *s1, *s2, *s3;
	struct ubuf_info *u0, *u1;
	unsigned int i, n = ARRAY_SIZE(cmd);
	long avail;

	NVMET_TCP_KUNIT_NEED_TX_LINK(test);
	nvmet_tcp_kunit_tx_region(test, n);
	avail = nvmet_tcp_kunit_avail(lb);
	for (i = 0; i < n; i++) {
		cmd[i] = nvmet_tcp_kunit_read(test, PAGE_SIZE);
		KUNIT_ASSERT_TRUE(test, nvmet_tcp_tx_want(cmd[i], PAGE_SIZE,
							  lb));
	}
	u0 = &cmd[0]->tx->tx.ubuf;

	/* Another provider's skb. */
	s1 = alloc_skb(0, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, s1);
	skb_zcopy_init(s1, &other);
	KUNIT_EXPECT_EQ(test, u0->ops->link_skb(s1, u0), -EEXIST);
	KUNIT_EXPECT_PTR_EQ(test, skb_zcopy(s1), &other);
	kfree_skb(s1);
	KUNIT_EXPECT_EQ(test, refcount_read(&other.refcnt), 0U);

	/* The chain fills at its cap; the next READ is refused. */
	s1 = nvmet_tcp_kunit_tx_skb(test, u0);
	for (i = 1; i < RX_LOAN_KUNIT_TX_CHAIN_MAX; i++) {
		struct ubuf_info *u = &cmd[i]->tx->tx.ubuf;

		KUNIT_EXPECT_EQ(test, u->ops->link_skb(s1, u), 0);
	}
	u1 = &cmd[RX_LOAN_KUNIT_TX_CHAIN_MAX]->tx->tx.ubuf;
	KUNIT_EXPECT_EQ(test, u1->ops->link_skb(s1, u1), -EEXIST);
	/* A new skb behind a member of a full chain is refused too. */
	s2 = nvmet_tcp_kunit_tx_skb(test, &cmd[5]->tx->tx.ubuf);
	KUNIT_EXPECT_EQ(test, u1->ops->link_skb(s2, u1), -EEXIST);

	/* A send in a chain does not join another. */
	s3 = nvmet_tcp_kunit_tx_skb(test, u1);
	u0 = &cmd[RX_LOAN_KUNIT_TX_CHAIN_MAX + 1]->tx->tx.ubuf;
	KUNIT_EXPECT_EQ(test, u0->ops->link_skb(s3, u0), 0);
	KUNIT_EXPECT_EQ(test, cmd[2]->tx->tx.ubuf.ops->link_skb(s3,
				&cmd[2]->tx->tx.ubuf), -EEXIST);

	for (i = 0; i < n; i++)
		nvmet_tcp_free_cmd_buffers(cmd[i]);
	kfree_skb(s3);
	rx_loan_tx_flush();
	/* The refused READs' own chain is back; the full chain is not. */
	KUNIT_EXPECT_EQ(test, rx_loan_kunit_tx_free(), 2U);
	kfree_skb(s1);
	rx_loan_tx_flush();
	KUNIT_EXPECT_EQ(test, rx_loan_kunit_tx_free(), 2U);
	kfree_skb(s2);
	rx_loan_tx_flush();
	KUNIT_EXPECT_EQ(test, rx_loan_kunit_tx_free(), n);
	KUNIT_EXPECT_EQ(test, nvmet_tcp_kunit_avail(lb), avail);
}

/* One copied skb of a chain counts every READ of it as copied. */
static void nvmet_tcp_kunit_tx_link_copied(struct kunit *test)
{
	struct rx_loan_budget *lb = nvmet_tcp_kunit_read_budget(test);
	struct nvmet_tcp_tx_stats base, d;
	struct nvmet_tcp_cmd *a, *b;
	struct ubuf_info *ua, *ub;
	struct sk_buff *s1;

	NVMET_TCP_KUNIT_NEED_TX_LINK(test);
	nvmet_tcp_kunit_tx_region(test, 8);
	a = nvmet_tcp_kunit_read(test, 2 * PAGE_SIZE);
	b = nvmet_tcp_kunit_read(test, 3 * PAGE_SIZE);
	nvmet_tcp_tx_sum(&base);
	KUNIT_ASSERT_TRUE(test, nvmet_tcp_tx_want(a, 2 * PAGE_SIZE, lb));
	KUNIT_ASSERT_TRUE(test, nvmet_tcp_tx_want(b, 3 * PAGE_SIZE, lb));
	ua = &a->tx->tx.ubuf;
	ub = &b->tx->tx.ubuf;
	s1 = nvmet_tcp_kunit_tx_skb(test, ua);
	KUNIT_ASSERT_EQ(test, ub->ops->link_skb(s1, ub), 0);
	nvmet_tcp_free_cmd_buffers(a);
	nvmet_tcp_free_cmd_buffers(b);
	/* TCP copied s1's frags (skb_copy_ubufs()), then freed it. */
	skb_zcopy_clear(s1, false);
	kfree_skb(s1);
	rx_loan_tx_flush();
	KUNIT_EXPECT_EQ(test, rx_loan_kunit_tx_free(), 8U);
	nvmet_tcp_kunit_tx_delta(&base, &d);
	KUNIT_EXPECT_EQ(test, d.sends, 0L);
	KUNIT_EXPECT_EQ(test, d.copies, 2ULL);
	KUNIT_EXPECT_EQ(test, d.copy_pages, 5ULL);
}

/*
 * Up to NVMET_TCP_TX_INLINE pages the bvecs are the command's own; a
 * larger READ allocates them, and both come back whole.
 */
static void nvmet_tcp_kunit_tx_inline(struct kunit *test)
{
	struct rx_loan_budget *lb = nvmet_tcp_kunit_read_budget(test);
	u32 small = NVMET_TCP_TX_INLINE * PAGE_SIZE;
	u32 large = (NVMET_TCP_TX_INLINE + 4) * PAGE_SIZE;
	struct nvmet_tcp_cmd *cmd, *big;
	long avail;

	nvmet_tcp_kunit_tx_region(test, 2 * NVMET_TCP_TX_INLINE + 4);
	cmd = nvmet_tcp_kunit_read(test, small);
	big = nvmet_tcp_kunit_read(test, large);
	avail = nvmet_tcp_kunit_avail(lb);

	KUNIT_ASSERT_TRUE(test, nvmet_tcp_tx_want(cmd, small, lb));
	KUNIT_EXPECT_PTR_EQ(test, cmd->iov, &cmd->tx_bv[0]);
	KUNIT_EXPECT_EQ(test, cmd->req.data_bvec_cnt, NVMET_TCP_TX_INLINE);
	KUNIT_ASSERT_TRUE(test, nvmet_tcp_tx_want(big, large, lb));
	KUNIT_EXPECT_PTR_NE(test, big->iov, &big->tx_bv[0]);
	KUNIT_EXPECT_EQ(test, big->req.data_bvec_cnt, NVMET_TCP_TX_INLINE + 4);
	KUNIT_EXPECT_EQ(test, rx_loan_kunit_tx_free(), 0U);

	nvmet_tcp_free_cmd_buffers(cmd);
	nvmet_tcp_free_cmd_buffers(big);
	KUNIT_EXPECT_NULL(test, cmd->iov);
	KUNIT_EXPECT_NULL(test, big->iov);
	rx_loan_tx_flush();
	KUNIT_EXPECT_EQ(test, rx_loan_kunit_tx_free(),
			2 * NVMET_TCP_TX_INLINE + 4);
	KUNIT_EXPECT_EQ(test, nvmet_tcp_kunit_avail(lb), avail);
}

/* Not a READ the region serves: no pages, no credit, nothing counted. */
static void nvmet_tcp_kunit_tx_ineligible(struct kunit *test)
{
	struct rx_loan_budget *lb = nvmet_tcp_kunit_read_budget(test);
	struct nvmet_tcp_cmd *write, *stock, *sgl;
	struct nvmet_tcp_tx_stats base, d;

	nvmet_tcp_kunit_tx_region(test, 4);
	write = nvmet_tcp_kunit_read(test, PAGE_SIZE);
	write->req.cmd->common.opcode = nvme_cmd_write;
	stock = nvmet_tcp_kunit_read(test, PAGE_SIZE);
	stock->queue->rx_region = false;
	sgl = nvmet_tcp_kunit_read(test, PAGE_SIZE);
	sgl->req.data_bvec_ok = false;
	nvmet_tcp_tx_sum(&base);
	KUNIT_EXPECT_FALSE(test, nvmet_tcp_tx_want(write, PAGE_SIZE, lb));
	KUNIT_EXPECT_FALSE(test, nvmet_tcp_tx_want(stock, PAGE_SIZE, lb));
	KUNIT_EXPECT_FALSE(test, nvmet_tcp_tx_want(sgl, PAGE_SIZE, lb));
	KUNIT_EXPECT_EQ(test, rx_loan_kunit_tx_free(), 4U);
	nvmet_tcp_kunit_tx_delta(&base, &d);
	KUNIT_EXPECT_EQ(test, d.confined, 0ULL);
	KUNIT_EXPECT_EQ(test, d.unconfined, 0ULL);
}

static struct kunit_case nvmet_tcp_rx_loan_cases[] = {
	KUNIT_CASE(nvmet_tcp_kunit_reserve_rule),
	KUNIT_CASE(nvmet_tcp_kunit_actor_borrows),
	KUNIT_CASE(nvmet_tcp_kunit_locked_head_bounced),
	KUNIT_CASE(nvmet_tcp_kunit_locked_head_mixed),
	KUNIT_CASE(nvmet_tcp_kunit_locked_head_two_skbs),
	KUNIT_CASE(nvmet_tcp_kunit_locked_head_copies),
	KUNIT_CASE(nvmet_tcp_kunit_copy_counted),
	KUNIT_CASE(nvmet_tcp_kunit_data_done_once),
	KUNIT_CASE(nvmet_tcp_kunit_withhold),
	KUNIT_CASE(nvmet_tcp_kunit_deny_resume),
	KUNIT_CASE(nvmet_tcp_kunit_unit_kept),
	KUNIT_CASE(nvmet_tcp_kunit_capsule_no_wait),
	KUNIT_CASE(nvmet_tcp_kunit_teardown_waiter),
	KUNIT_CASE(nvmet_tcp_kunit_sink),
	KUNIT_CASE(nvmet_tcp_kunit_table_cap),
	KUNIT_CASE(nvmet_tcp_kunit_overshoot_copies),
	KUNIT_CASE(nvmet_tcp_kunit_capsule_over_unit),
	KUNIT_CASE(nvmet_tcp_kunit_charge_fail_copies),
	KUNIT_CASE(nvmet_tcp_kunit_finish_tiles),
	KUNIT_CASE(nvmet_tcp_kunit_hold_account),
	KUNIT_CASE(nvmet_tcp_kunit_hold_apply),
	KUNIT_CASE(nvmet_tcp_kunit_hold_release),
	KUNIT_CASE(nvmet_tcp_kunit_listener_release),
	KUNIT_CASE(nvmet_tcp_kunit_own_budget),
	KUNIT_CASE(nvmet_tcp_kunit_tx_confined),
	KUNIT_CASE(nvmet_tcp_kunit_tx_unsent),
	KUNIT_CASE(nvmet_tcp_kunit_tx_unconfined),
	KUNIT_CASE(nvmet_tcp_kunit_tx_bvec_refused),
	KUNIT_CASE(nvmet_tcp_kunit_tx_ineligible),
	KUNIT_CASE(nvmet_tcp_kunit_tx_copied),
	KUNIT_CASE(nvmet_tcp_kunit_tx_inline),
	KUNIT_CASE(nvmet_tcp_kunit_tx_linked),
	KUNIT_CASE(nvmet_tcp_kunit_tx_link_refused),
	KUNIT_CASE(nvmet_tcp_kunit_tx_link_copied),
	{}
};

static struct kunit_suite nvmet_tcp_rx_loan_suite = {
	.name		= "nvmet-tcp-rx-loan",
	.init		= nvmet_tcp_kunit_init,
	.exit		= nvmet_tcp_kunit_exit,
	.test_cases	= nvmet_tcp_rx_loan_cases,
};

kunit_test_suite(nvmet_tcp_rx_loan_suite);
