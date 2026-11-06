// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * Copyright (c) 2015-2018 Oracle. All rights reserved.
 * Copyright (c) 2014 Open Grid Computing, Inc. All rights reserved.
 * Copyright (c) 2005-2007 Network Appliance, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the BSD-type
 * license below:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *      Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *
 *      Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following
 *      disclaimer in the documentation and/or other materials provided
 *      with the distribution.
 *
 *      Neither the name of the Network Appliance, Inc. nor the names of
 *      its contributors may be used to endorse or promote products
 *      derived from this software without specific prior written
 *      permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Author: Tom Tucker <tom@opengridcomputing.com>
 */

#include <linux/cpumask.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/topology.h>
#include <linux/workqueue.h>
#include <linux/export.h>

#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>
#include <rdma/rw.h>

#include <linux/sunrpc/addr.h>
#include <linux/sunrpc/debug.h>
#include <linux/sunrpc/svc_xprt.h>
#include <linux/sunrpc/svc_rdma.h>

#include "xprt_rdma.h"
#include <trace/events/rpcrdma.h>

#define RPCDBG_FACILITY	RPCDBG_SVCXPRT

static struct svcxprt_rdma *svc_rdma_create_xprt(struct svc_serv *serv,
						 struct net *net, int node);
static int svc_rdma_listen_handler(struct rdma_cm_id *cma_id,
				   struct rdma_cm_event *event);
static struct svc_xprt *svc_rdma_create(struct svc_serv *serv,
					struct net *net,
					struct sockaddr *sa, int salen,
					int flags);
static struct svc_xprt *svc_rdma_accept(struct svc_xprt *xprt);
static void svc_rdma_detach(struct svc_xprt *xprt);
static void svc_rdma_free(struct svc_xprt *xprt);
static int svc_rdma_has_wspace(struct svc_xprt *xprt);
static void svc_rdma_kill_temp_xprt(struct svc_xprt *);

/*
 * LAB ONLY, set on the boot line: both CQs of each accepted connection
 * share one completion vector, chosen so that a client's connections on
 * a device take distinct cores of its NUMA node and every core carries
 * the same number of connections.  With per-CPU nfsd pools, the work a
 * core's receive completions queue then goes to that core's own pool.
 */
static bool svc_rdma_cq_per_core __read_mostly = true;
module_param_named(svc_cq_per_core, svc_rdma_cq_per_core, bool, 0444);
MODULE_PARM_DESC(svc_cq_per_core,
		 "LAB ONLY: spread svcrdma CQs one per core of the device's NUMA node");

/*
 * One client's live connections on one device, per CPU.  The client is
 * its address with the port zeroed, as handle_connect_req() records it.
 */
struct svc_rdma_cq_peer {
	struct list_head	list;
	const struct ib_device	*dev;
	struct sockaddr_storage	addr;
	unsigned int		ref;		/* connections holding it */
	u16			load[];		/* nr_cpu_ids entries */
};

static DEFINE_SPINLOCK(svc_rdma_cq_lock);
static LIST_HEAD(svc_rdma_cq_peers);
static DEFINE_PER_CPU(unsigned int, svc_rdma_cq_load);	/* live connections */
static unsigned int svc_rdma_cq_rotor;

static const struct svc_xprt_ops svc_rdma_ops = {
	.xpo_create = svc_rdma_create,
	.xpo_recvfrom = svc_rdma_recvfrom,
	.xpo_sendto = svc_rdma_sendto,
	.xpo_result_payload = svc_rdma_result_payload,
	.xpo_release_ctxt = svc_rdma_release_ctxt,
	.xpo_detach = svc_rdma_detach,
	.xpo_free = svc_rdma_free,
	.xpo_has_wspace = svc_rdma_has_wspace,
	.xpo_accept = svc_rdma_accept,
	.xpo_kill_temp_xprt = svc_rdma_kill_temp_xprt,
};

struct svc_xprt_class svc_rdma_class = {
	.xcl_name = "rdma",
	.xcl_owner = THIS_MODULE,
	.xcl_ops = &svc_rdma_ops,
	.xcl_max_payload = RPCSVC_MAXPAYLOAD_RDMA,
	.xcl_ident = XPRT_TRANSPORT_RDMA,
};

/**
 * svc_rdma_xprt_deferred_close - Close an RDMA transport (deferred)
 * @rdma: transport to close
 */
void svc_rdma_xprt_deferred_close(struct svcxprt_rdma *rdma)
{
	svc_xprt_deferred_close(&rdma->sc_xprt);

	/* Release parked sc_sq_ticket_wait and sc_send_wait waiters.
	 * Once XPT_CLOSE is observed each returns -ENOTCONN.
	 */
	wake_up_all(&rdma->sc_sq_ticket_wait);
	wake_up_all(&rdma->sc_send_wait);
}

/* QP event handler */
static void qp_event_handler(struct ib_event *event, void *context)
{
	struct svc_xprt *xprt = context;
	struct svcxprt_rdma *rdma =
		container_of(xprt, struct svcxprt_rdma, sc_xprt);

	trace_svcrdma_qp_error(event, (struct sockaddr *)&xprt->xpt_remote);
	switch (event->event) {
	/* These are considered benign events */
	case IB_EVENT_PATH_MIG:
	case IB_EVENT_COMM_EST:
	case IB_EVENT_SQ_DRAINED:
	case IB_EVENT_QP_LAST_WQE_REACHED:
		break;

	/* These are considered fatal events */
	case IB_EVENT_PATH_MIG_ERR:
	case IB_EVENT_QP_FATAL:
	case IB_EVENT_QP_REQ_ERR:
	case IB_EVENT_QP_ACCESS_ERR:
	case IB_EVENT_DEVICE_FATAL:
	default:
		svc_rdma_xprt_deferred_close(rdma);
		break;
	}
}

static struct rdma_cm_id *
svc_rdma_create_listen_id(struct net *net, struct sockaddr *sap,
			  void *context)
{
	struct rdma_cm_id *listen_id;
	int ret;

	listen_id = rdma_create_id(net, svc_rdma_listen_handler, context,
				   RDMA_PS_TCP, IB_QPT_RC);
	if (IS_ERR(listen_id))
		return listen_id;

	/* Allow both IPv4 and IPv6 sockets to bind a single port
	 * at the same time.
	 */
#if IS_ENABLED(CONFIG_IPV6)
	ret = rdma_set_afonly(listen_id, 1);
	if (ret)
		goto out_destroy;
#endif
	ret = rdma_bind_addr(listen_id, sap);
	if (ret)
		goto out_destroy;

	ret = rdma_listen(listen_id, RPCRDMA_LISTEN_BACKLOG);
	if (ret)
		goto out_destroy;

	return listen_id;

out_destroy:
	rdma_destroy_id(listen_id);
	return ERR_PTR(ret);
}

static struct svcxprt_rdma *svc_rdma_create_xprt(struct svc_serv *serv,
						 struct net *net, int node)
{
	static struct lock_class_key svcrdma_rwctx_lock;
	static struct lock_class_key svcrdma_sctx_lock;
	static struct lock_class_key svcrdma_dto_lock;
	struct svcxprt_rdma *cma_xprt;

	cma_xprt = kzalloc_node(sizeof(*cma_xprt), GFP_KERNEL, node);
	if (!cma_xprt)
		return NULL;
	cma_xprt->sc_cq_cpu = -1;

	svc_xprt_init(net, &svc_rdma_class, &cma_xprt->sc_xprt, serv);
	INIT_LIST_HEAD(&cma_xprt->sc_accept_q);
	INIT_LIST_HEAD(&cma_xprt->sc_rq_dto_q);
	INIT_LIST_HEAD(&cma_xprt->sc_read_complete_q);
	init_llist_head(&cma_xprt->sc_send_ctxts);
	init_llist_head(&cma_xprt->sc_recv_ctxts);
	init_llist_head(&cma_xprt->sc_rw_ctxts);
	init_llist_head(&cma_xprt->sc_send_release_list);
	init_llist_head(&cma_xprt->sc_send_stranded_ctxts);
	init_llist_head(&cma_xprt->sc_recv_stranded_ctxts);
	init_waitqueue_head(&cma_xprt->sc_send_wait);
	init_waitqueue_head(&cma_xprt->sc_sq_ticket_wait);

	spin_lock_init(&cma_xprt->sc_lock);
	spin_lock_init(&cma_xprt->sc_rq_dto_lock);
	lockdep_set_class(&cma_xprt->sc_rq_dto_lock, &svcrdma_dto_lock);
	spin_lock_init(&cma_xprt->sc_send_lock);
	lockdep_set_class(&cma_xprt->sc_send_lock, &svcrdma_sctx_lock);
	spin_lock_init(&cma_xprt->sc_rw_ctxt_lock);
	lockdep_set_class(&cma_xprt->sc_rw_ctxt_lock, &svcrdma_rwctx_lock);

	/*
	 * Note that this implies that the underlying transport support
	 * has some form of congestion control (see RFC 7530 section 3.1
	 * paragraph 2). For now, we assume that all supported RDMA
	 * transports are suitable here.
	 */
	set_bit(XPT_CONG_CTRL, &cma_xprt->sc_xprt.xpt_flags);

	return cma_xprt;
}

static void
svc_rdma_parse_connect_private(struct svcxprt_rdma *newxprt,
			       struct rdma_conn_param *param)
{
	const struct rpcrdma_connect_private *pmsg = param->private_data;

	if (pmsg &&
	    pmsg->cp_magic == rpcrdma_cmp_magic &&
	    pmsg->cp_version == RPCRDMA_CMP_VERSION) {
		newxprt->sc_snd_w_inv = pmsg->cp_flags &
					RPCRDMA_CMP_F_SND_W_INV_OK;

		dprintk("svcrdma: client send_size %u, recv_size %u "
			"remote inv %ssupported\n",
			rpcrdma_decode_buffer_size(pmsg->cp_send_size),
			rpcrdma_decode_buffer_size(pmsg->cp_recv_size),
			newxprt->sc_snd_w_inv ? "" : "un");
	}
}

/*
 * This function handles the CONNECT_REQUEST event on a listening
 * endpoint. It is passed the cma_id for the _new_ connection. The context in
 * this cma_id is inherited from the listening cma_id and is the svc_xprt
 * structure for the listening endpoint.
 *
 * This function creates a new xprt for the new connection and enqueues it on
 * the accept queue for the listen xprt. When the listen thread is kicked, it
 * will call the recvfrom method on the listen xprt which will accept the new
 * connection.
 *
 * Return values:
 *     %0: Do not destroy @new_cma_id
 *     %1: Destroy @new_cma_id (allocation failure)
 */
static int handle_connect_req(struct rdma_cm_id *new_cma_id,
			      struct rdma_conn_param *param)
{
	struct svcxprt_rdma *listen_xprt = new_cma_id->context;
	struct svcxprt_rdma *newxprt;
	struct sockaddr *sa;

	newxprt = svc_rdma_create_xprt(listen_xprt->sc_xprt.xpt_server,
				       listen_xprt->sc_xprt.xpt_net,
				       ibdev_to_node(new_cma_id->device));
	if (!newxprt)
		return 1;
	newxprt->sc_cm_id = new_cma_id;
	new_cma_id->context = newxprt;
	svc_rdma_parse_connect_private(newxprt, param);

	/* Save client advertised inbound read limit for use later in accept. */
	newxprt->sc_ord = param->initiator_depth;

	sa = (struct sockaddr *)&newxprt->sc_cm_id->route.addr.dst_addr;
	newxprt->sc_xprt.xpt_remotelen = svc_addr_len(sa);
	memcpy(&newxprt->sc_xprt.xpt_remote, sa,
	       newxprt->sc_xprt.xpt_remotelen);
	snprintf(newxprt->sc_xprt.xpt_remotebuf,
		 sizeof(newxprt->sc_xprt.xpt_remotebuf) - 1, "%pISc", sa);

	/* The remote port is arbitrary and not under the control of the
	 * client ULP. Set it to a fixed value so that the DRC continues
	 * to be effective after a reconnect.
	 */
	rpc_set_port((struct sockaddr *)&newxprt->sc_xprt.xpt_remote, 0);

	sa = (struct sockaddr *)&newxprt->sc_cm_id->route.addr.src_addr;
	svc_xprt_set_local(&newxprt->sc_xprt, sa, svc_addr_len(sa));

	/*
	 * Enqueue the new transport on the accept queue of the listening
	 * transport
	 */
	spin_lock(&listen_xprt->sc_lock);
	list_add_tail(&newxprt->sc_accept_q, &listen_xprt->sc_accept_q);
	spin_unlock(&listen_xprt->sc_lock);

	set_bit(XPT_CONN, &listen_xprt->sc_xprt.xpt_flags);
	svc_xprt_enqueue(&listen_xprt->sc_xprt);
	return 0;
}

/**
 * svc_rdma_listen_handler - Handle CM events generated on a listening endpoint
 * @cma_id: the server's listener rdma_cm_id
 * @event: details of the event
 *
 * Return values:
 *     %0: Do not destroy @cma_id
 *     %1: Destroy @cma_id
 *
 * NB: There is never a DEVICE_REMOVAL event for INADDR_ANY listeners.
 */
static int svc_rdma_listen_handler(struct rdma_cm_id *cma_id,
				   struct rdma_cm_event *event)
{
	struct sockaddr *sap = (struct sockaddr *)&cma_id->route.addr.src_addr;
	struct svcxprt_rdma *cma_xprt = cma_id->context;
	struct svc_xprt *cma_rdma = &cma_xprt->sc_xprt;
	struct rdma_cm_id *listen_id;

	switch (event->event) {
	case RDMA_CM_EVENT_CONNECT_REQUEST:
		return handle_connect_req(cma_id, &event->param.conn);
	case RDMA_CM_EVENT_ADDR_CHANGE:
		listen_id = svc_rdma_create_listen_id(cma_rdma->xpt_net,
						      sap, cma_xprt);
		if (IS_ERR(listen_id)) {
			pr_err("Listener dead, address change failed for device %s\n",
				cma_id->device->name);
			cma_xprt->sc_cm_id = NULL;
		} else
			cma_xprt->sc_cm_id = listen_id;
		return 1;
	default:
		break;
	}
	return 0;
}

/**
 * svc_rdma_cma_handler - Handle CM events on client connections
 * @cma_id: the server's listener rdma_cm_id
 * @event: details of the event
 *
 * Return values:
 *     %0: Do not destroy @cma_id
 *     %1: Destroy @cma_id (never returned here)
 */
static int svc_rdma_cma_handler(struct rdma_cm_id *cma_id,
				struct rdma_cm_event *event)
{
	struct svcxprt_rdma *rdma = cma_id->context;
	struct svc_xprt *xprt = &rdma->sc_xprt;

	switch (event->event) {
	case RDMA_CM_EVENT_ESTABLISHED:
		clear_bit(RDMAXPRT_CONN_PENDING, &rdma->sc_flags);

		/* Handle any requests that were received while
		 * CONN_PENDING was set. */
		svc_xprt_enqueue(xprt);
		break;
	case RDMA_CM_EVENT_DISCONNECTED:
		svc_rdma_xprt_deferred_close(rdma);
		break;
	default:
		break;
	}
	return 0;
}

/*
 * Create a listening RDMA service endpoint.
 */
static struct svc_xprt *svc_rdma_create(struct svc_serv *serv,
					struct net *net,
					struct sockaddr *sa, int salen,
					int flags)
{
	struct rdma_cm_id *listen_id;
	struct svcxprt_rdma *cma_xprt;

	if (sa->sa_family != AF_INET && sa->sa_family != AF_INET6)
		return ERR_PTR(-EAFNOSUPPORT);
	cma_xprt = svc_rdma_create_xprt(serv, net, NUMA_NO_NODE);
	if (!cma_xprt)
		return ERR_PTR(-ENOMEM);
	set_bit(XPT_LISTENER, &cma_xprt->sc_xprt.xpt_flags);
	strcpy(cma_xprt->sc_xprt.xpt_remotebuf, "listener");

	listen_id = svc_rdma_create_listen_id(net, sa, cma_xprt);
	if (IS_ERR(listen_id)) {
		/* _svc_xprt_create() acquired one module reference and
		 * puts it on xpo_create failure.  svc_xprt_free() puts
		 * a second one when the kref drops to zero.  Take a
		 * compensating reference so both puts are balanced.
		 */
		__module_get(cma_xprt->sc_xprt.xpt_class->xcl_owner);
		svc_xprt_put(&cma_xprt->sc_xprt);
		return ERR_CAST(listen_id);
	}
	cma_xprt->sc_cm_id = listen_id;

	/*
	 * We need to use the address from the cm_id in case the
	 * caller specified 0 for the port number.
	 */
	sa = (struct sockaddr *)&cma_xprt->sc_cm_id->route.addr.src_addr;
	svc_xprt_set_local(&cma_xprt->sc_xprt, sa, salen);

	return &cma_xprt->sc_xprt;
}

static void svc_rdma_xprt_done(struct rpcrdma_notification *rn)
{
	struct svcxprt_rdma *rdma = container_of(rn, struct svcxprt_rdma,
						 sc_rn);
	struct rdma_cm_id *id = rdma->sc_cm_id;

	trace_svcrdma_device_removal(id);
	svc_xprt_close(&rdma->sc_xprt);
}

/* The client @sap's entry for @dev, with svc_rdma_cq_lock held */
static struct svc_rdma_cq_peer *
svc_rdma_cq_peer_find(const struct ib_device *dev, const struct sockaddr *sap)
{
	struct svc_rdma_cq_peer *peer;

	list_for_each_entry(peer, &svc_rdma_cq_peers, list)
		if (peer->dev == dev &&
		    rpc_cmp_addr((const struct sockaddr *)&peer->addr, sap))
			return peer;
	return NULL;
}

/**
 * svc_rdma_cq_vector - Choose the completion vector for a new connection
 * @rdma: transport being accepted
 * @dev: its device
 *
 * Takes the online CPU of @dev's NUMA node that carries the fewest of
 * this client's connections on @dev, then the fewest connections, ties
 * round robin, and returns the vector that interrupts on it.  That
 * assumes vector i interrupts on the i-th online CPU of the node, as
 * mlx5 spreads its vectors and as the lab pins them.  Returns -1, taking
 * no CPU, when the device gives no node or has one vector.  Process
 * context.
 */
static int svc_rdma_cq_vector(struct svcxprt_rdma *rdma,
			      struct ib_device *dev)
{
	const struct sockaddr *sap =
		(const struct sockaddr *)&rdma->sc_xprt.xpt_remote;
	unsigned int cnt, start, key, load, mine, pos = 0;
	unsigned int best_key = UINT_MAX, best_load = UINT_MAX;
	unsigned int best_mine = UINT_MAX;
	struct svc_rdma_cq_peer *peer, *new;
	int node, cpu, best_cpu = -1, best_pos = -1;
	const struct cpumask *mask;

	if (!READ_ONCE(svc_rdma_cq_per_core) || dev->num_comp_vectors < 2)
		return -1;
	node = ibdev_to_node(dev);
	if (node < 0 || node >= nr_node_ids)
		return -1;
	mask = cpumask_of_node(node);
	new = kzalloc(struct_size(new, load, nr_cpu_ids), GFP_KERNEL);

	spin_lock(&svc_rdma_cq_lock);
	cnt = cpumask_weight_and(mask, cpu_online_mask);
	if (!cnt)
		goto out_unlock;
	peer = svc_rdma_cq_peer_find(dev, sap);
	start = svc_rdma_cq_rotor++ % cnt;
	for_each_cpu_and(cpu, mask, cpu_online_mask) {
		mine = peer ? peer->load[cpu] : 0;
		load = per_cpu(svc_rdma_cq_load, cpu);
		key = (pos + cnt - start) % cnt;
		if (mine < best_mine ||
		    (mine == best_mine &&
		     (load < best_load ||
		      (load == best_load && key < best_key)))) {
			best_mine = mine;
			best_load = load;
			best_key = key;
			best_cpu = cpu;
			best_pos = pos;
		}
		pos++;
	}
	if (best_cpu < 0)
		goto out_unlock;
	if (!peer && new) {
		peer = new;
		new = NULL;
		peer->dev = dev;
		memcpy(&peer->addr, sap, rdma->sc_xprt.xpt_remotelen);
		list_add(&peer->list, &svc_rdma_cq_peers);
	}
	per_cpu(svc_rdma_cq_load, best_cpu)++;
	rdma->sc_cq_cpu = best_cpu;
	if (peer) {
		peer->load[best_cpu]++;
		peer->ref++;
		rdma->sc_cq_peer = peer;
	}
out_unlock:
	spin_unlock(&svc_rdma_cq_lock);
	kfree(new);
	return best_cpu < 0 ? -1 : best_pos % dev->num_comp_vectors;
}

/*
 * Give back what svc_rdma_cq_vector() took for @rdma.  Called when the
 * connection closes and again when it is freed; only the first call
 * finds anything to give back.
 */
static void svc_rdma_cq_vector_put(struct svcxprt_rdma *rdma)
{
	struct svc_rdma_cq_peer *peer, *dead = NULL;
	int cpu;

	if (READ_ONCE(rdma->sc_cq_cpu) < 0)
		return;
	spin_lock(&svc_rdma_cq_lock);
	cpu = rdma->sc_cq_cpu;
	peer = rdma->sc_cq_peer;
	if (cpu >= 0) {
		per_cpu(svc_rdma_cq_load, cpu)--;
		if (peer) {
			peer->load[cpu]--;
			if (!--peer->ref) {
				list_del(&peer->list);
				dead = peer;
			}
		}
	}
	WRITE_ONCE(rdma->sc_cq_cpu, -1);
	rdma->sc_cq_peer = NULL;
	spin_unlock(&svc_rdma_cq_lock);
	kfree(dead);
}

/*
 * Both CQs of @rdma on one vector: @vec, from svc_rdma_cq_vector(), or
 * any vector as before when @vec is -1.  If @vec cannot be used, give
 * its CPU back and keep the two CQs together on the vector the first
 * one gets.
 */
static int svc_rdma_alloc_cqs(struct svcxprt_rdma *rdma,
			      struct ib_device *dev, int vec, int rq_depth)
{
	if (vec >= 0) {
		rdma->sc_sq_cq = ib_alloc_cq(dev, rdma, rdma->sc_sq_depth, vec,
					     IB_POLL_WORKQUEUE);
		if (!IS_ERR(rdma->sc_sq_cq)) {
			rdma->sc_rq_cq = ib_alloc_cq(dev, rdma, rq_depth, vec,
						     IB_POLL_WORKQUEUE);
			if (!IS_ERR(rdma->sc_rq_cq))
				return 0;
			ib_free_cq(rdma->sc_sq_cq);
		}
		rdma->sc_sq_cq = NULL;
		rdma->sc_rq_cq = NULL;
		svc_rdma_cq_vector_put(rdma);
		rdma->sc_sq_cq = ib_alloc_cq_any(dev, rdma, rdma->sc_sq_depth,
						 IB_POLL_WORKQUEUE);
		if (IS_ERR(rdma->sc_sq_cq))
			return PTR_ERR(rdma->sc_sq_cq);
		rdma->sc_rq_cq = ib_alloc_cq(dev, rdma, rq_depth,
					     rdma->sc_sq_cq->comp_vector,
					     IB_POLL_WORKQUEUE);
		return PTR_ERR_OR_ZERO(rdma->sc_rq_cq);
	}
	/* As before: svc_cq_per_core=N, or a device that gives no node */
	rdma->sc_sq_cq = ib_alloc_cq_any(dev, rdma, rdma->sc_sq_depth,
					 IB_POLL_WORKQUEUE);
	if (IS_ERR(rdma->sc_sq_cq))
		return PTR_ERR(rdma->sc_sq_cq);
	rdma->sc_rq_cq = ib_alloc_cq_any(dev, rdma, rq_depth,
					 IB_POLL_WORKQUEUE);
	return PTR_ERR_OR_ZERO(rdma->sc_rq_cq);
}

/*
 * This is the xpo_recvfrom function for listening endpoints. Its
 * purpose is to accept incoming connections. The CMA callback handler
 * has already created a new transport and attached it to the new CMA
 * ID.
 *
 * There is a queue of pending connections hung on the listening
 * transport. This queue contains the new svc_xprt structure. This
 * function takes svc_xprt structures off the accept_q and completes
 * the connection.
 */
static struct svc_xprt *svc_rdma_accept(struct svc_xprt *xprt)
{
	unsigned int ctxts, rq_depth, maxpayload;
	struct svcxprt_rdma *listen_rdma;
	struct svcxprt_rdma *newxprt = NULL;
	struct rdma_conn_param conn_param;
	struct rpcrdma_connect_private pmsg;
	struct ib_qp_init_attr qp_attr;
	struct ib_device *dev;
	int ret = 0, vec;

	listen_rdma = container_of(xprt, struct svcxprt_rdma, sc_xprt);
	clear_bit(XPT_CONN, &xprt->xpt_flags);
	/* Get the next entry off the accept list */
	spin_lock(&listen_rdma->sc_lock);
	if (!list_empty(&listen_rdma->sc_accept_q)) {
		newxprt = list_entry(listen_rdma->sc_accept_q.next,
				     struct svcxprt_rdma, sc_accept_q);
		list_del_init(&newxprt->sc_accept_q);
	}
	if (!list_empty(&listen_rdma->sc_accept_q))
		set_bit(XPT_CONN, &listen_rdma->sc_xprt.xpt_flags);
	spin_unlock(&listen_rdma->sc_lock);
	if (!newxprt)
		return NULL;

	dev = newxprt->sc_cm_id->device;
	newxprt->sc_port_num = newxprt->sc_cm_id->port_num;

	if (rpcrdma_rn_register(dev, &newxprt->sc_rn, svc_rdma_xprt_done))
		goto errout;

	newxprt->sc_max_req_size = svcrdma_max_req_size;
	newxprt->sc_max_requests = svcrdma_max_requests;
	newxprt->sc_max_bc_requests = svcrdma_max_bc_requests;
	newxprt->sc_recv_batch = RPCRDMA_MAX_RECV_BATCH;

	/* Qualify the transport's resource defaults with the
	 * capabilities of this particular device.
	 */

	/* Transport header, head iovec, tail iovec */
	newxprt->sc_max_send_sges = 3;
	/* Add one SGE per page list entry */
	newxprt->sc_max_send_sges += (svcrdma_max_req_size / PAGE_SIZE) + 1;
	if (newxprt->sc_max_send_sges > dev->attrs.max_send_sge)
		newxprt->sc_max_send_sges = dev->attrs.max_send_sge;
	rq_depth = newxprt->sc_max_requests + newxprt->sc_max_bc_requests +
		   newxprt->sc_recv_batch + 1 /* drain */;
	if (rq_depth > dev->attrs.max_qp_wr) {
		rq_depth = dev->attrs.max_qp_wr;
		newxprt->sc_recv_batch = 1;
		newxprt->sc_max_requests = rq_depth - 2;
		newxprt->sc_max_bc_requests = 2;
	}

	newxprt->sc_fc_credits = cpu_to_be32(newxprt->sc_max_requests);

	/* Estimate the needed number of rdma_rw contexts. The maximum
	 * Read and Write chunks have one segment each. Each request
	 * can involve one Read chunk and either a Write chunk or Reply
	 * chunk; thus a factor of three.
	 */
	maxpayload = min(xprt->xpt_server->sv_max_payload,
			 RPCSVC_MAXPAYLOAD_RDMA);
	ctxts = newxprt->sc_max_requests * 3 *
		rdma_rw_mr_factor(dev, newxprt->sc_port_num,
				  maxpayload >> PAGE_SHIFT);

	newxprt->sc_sq_depth = rq_depth +
		rdma_rw_max_send_wr(dev, newxprt->sc_port_num, ctxts, 0);
	if (newxprt->sc_sq_depth > dev->attrs.max_qp_wr)
		newxprt->sc_sq_depth = dev->attrs.max_qp_wr;
	atomic_set(&newxprt->sc_sq_avail, newxprt->sc_sq_depth);
	atomic_set(&newxprt->sc_sq_ticket_head, 0);
	atomic_set(&newxprt->sc_sq_ticket_tail, 0);

	newxprt->sc_pd = ib_alloc_pd(dev, 0);
	if (IS_ERR(newxprt->sc_pd)) {
		trace_svcrdma_pd_err(newxprt, PTR_ERR(newxprt->sc_pd));
		goto errout;
	}
	vec = svc_rdma_cq_vector(newxprt, dev);
	if (svc_rdma_alloc_cqs(newxprt, dev, vec, rq_depth))
		goto errout;

	memset(&qp_attr, 0, sizeof qp_attr);
	qp_attr.event_handler = qp_event_handler;
	qp_attr.qp_context = &newxprt->sc_xprt;
	qp_attr.port_num = newxprt->sc_port_num;
	qp_attr.cap.max_rdma_ctxs = ctxts;
	qp_attr.cap.max_send_wr = newxprt->sc_sq_depth - ctxts;
	qp_attr.cap.max_recv_wr = rq_depth;
	qp_attr.cap.max_send_sge = newxprt->sc_max_send_sges;
	qp_attr.cap.max_recv_sge = 1;
	qp_attr.sq_sig_type = IB_SIGNAL_REQ_WR;
	qp_attr.qp_type = IB_QPT_RC;
	qp_attr.send_cq = newxprt->sc_sq_cq;
	qp_attr.recv_cq = newxprt->sc_rq_cq;
	dprintk("    cap.max_send_wr = %d, cap.max_recv_wr = %d\n",
		qp_attr.cap.max_send_wr, qp_attr.cap.max_recv_wr);
	dprintk("    cap.max_send_sge = %d, cap.max_recv_sge = %d\n",
		qp_attr.cap.max_send_sge, qp_attr.cap.max_recv_sge);
	dprintk("    send CQ depth = %u, recv CQ depth = %u\n",
		newxprt->sc_sq_depth, rq_depth);
	ret = rdma_create_qp(newxprt->sc_cm_id, newxprt->sc_pd, &qp_attr);
	if (ret) {
		trace_svcrdma_qp_err(newxprt, ret);
		goto errout;
	}
	newxprt->sc_max_send_sges = qp_attr.cap.max_send_sge;
	newxprt->sc_qp = newxprt->sc_cm_id->qp;

	if (!(dev->attrs.device_cap_flags & IB_DEVICE_MEM_MGT_EXTENSIONS))
		newxprt->sc_snd_w_inv = false;
	if (!rdma_protocol_iwarp(dev, newxprt->sc_port_num) &&
	    !rdma_ib_or_roce(dev, newxprt->sc_port_num)) {
		trace_svcrdma_fabric_err(newxprt, -EINVAL);
		goto errout;
	}

	if (!svc_rdma_post_recvs(newxprt))
		goto errout;

	/* Construct RDMA-CM private message */
	pmsg.cp_magic = rpcrdma_cmp_magic;
	pmsg.cp_version = RPCRDMA_CMP_VERSION;
	pmsg.cp_flags = 0;
	pmsg.cp_send_size = pmsg.cp_recv_size =
		rpcrdma_encode_buffer_size(newxprt->sc_max_req_size);

	/* Accept Connection */
	set_bit(RDMAXPRT_CONN_PENDING, &newxprt->sc_flags);
	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 0;
	conn_param.initiator_depth = min(newxprt->sc_ord, dev->attrs.max_qp_init_rd_atom);
	if (!conn_param.initiator_depth) {
		ret = -EINVAL;
		trace_svcrdma_initdepth_err(newxprt, ret);
		goto errout;
	}
	conn_param.private_data = &pmsg;
	conn_param.private_data_len = sizeof(pmsg);
	rdma_lock_handler(newxprt->sc_cm_id);
	newxprt->sc_cm_id->event_handler = svc_rdma_cma_handler;
	ret = rdma_accept(newxprt->sc_cm_id, &conn_param);
	rdma_unlock_handler(newxprt->sc_cm_id);
	if (ret) {
		trace_svcrdma_accept_err(newxprt, ret);
		goto errout;
	}

	if (IS_ENABLED(CONFIG_SUNRPC_DEBUG)) {
		struct sockaddr *sap;

		dprintk("svcrdma: new connection accepted on device %s:\n", dev->name);
		sap = (struct sockaddr *)&newxprt->sc_cm_id->route.addr.src_addr;
		dprintk("    local address   : %pIS:%u\n", sap, rpc_get_port(sap));
		sap = (struct sockaddr *)&newxprt->sc_cm_id->route.addr.dst_addr;
		dprintk("    remote address  : %pIS:%u\n", sap, rpc_get_port(sap));
		dprintk("    max_sge         : %u\n", newxprt->sc_max_send_sges);
		dprintk("    sq_depth        : %d\n", newxprt->sc_sq_depth);
		dprintk("    rdma_rw_ctxs    : %d\n", ctxts);
		dprintk("    max_requests    : %d\n", newxprt->sc_max_requests);
		dprintk("    ord             : %d\n", conn_param.initiator_depth);
		dprintk("    cq vectors      : %u/%u (cpu %d)\n",
			newxprt->sc_sq_cq->comp_vector,
			newxprt->sc_rq_cq->comp_vector, newxprt->sc_cq_cpu);
	}

	return &newxprt->sc_xprt;

 errout:
	/*
	 * Drop the kref_init birth reference. svc_xprt_free will
	 * dispatch xpo_free = svc_rdma_free, which tears down sc_qp,
	 * sc_sq_cq, sc_rq_cq, and sc_pd under existing IS_ERR/NULL
	 * guards, and sc_rn under the rn_done sentinel guard inside
	 * rpcrdma_rn_unregister.
	 *
	 * sc_cm_id is destroyed unconditionally by svc_rdma_free; that
	 * is safe here because sc_cm_id is non-NULL by caller invariant
	 * on every path that reaches this errout: handle_connect_req
	 * installs newxprt->sc_cm_id before queueing the new xprt for
	 * accept, and svc_rdma_accept has already dereferenced it above
	 * the first goto errout.
	 *
	 * svc_handle_xprt() drops its pre-acquired module reference when
	 * ->xpo_accept() returns NULL. Take a replacement reference before
	 * freeing @newxprt, because svc_xprt_free() drops the module
	 * reference associated with @newxprt.
	 */
	__module_get(newxprt->sc_xprt.xpt_class->xcl_owner);
	svc_xprt_put(&newxprt->sc_xprt);
	return NULL;
}

static void svc_rdma_detach(struct svc_xprt *xprt)
{
	struct svcxprt_rdma *rdma =
		container_of(xprt, struct svcxprt_rdma, sc_xprt);

	if (rdma->sc_cm_id)
		rdma_disconnect(rdma->sc_cm_id);

	/*
	 * Most close paths go through svc_rdma_xprt_deferred_close(),
	 * which wakes the SQ waitqueues. svc_xprt_close() reaches
	 * detach without that helper, so wake any threads parked in
	 * svc_rdma_sq_wait() here as well.
	 */
	wake_up_all(&rdma->sc_sq_ticket_wait);
	wake_up_all(&rdma->sc_send_wait);

	/* A closed connection no longer counts toward its core's load */
	svc_rdma_cq_vector_put(rdma);
}

/**
 * svc_rdma_free - Release class-specific transport resources
 * @xprt: Generic svc transport object
 */
static void svc_rdma_free(struct svc_xprt *xprt)
{
	struct svcxprt_rdma *rdma =
		container_of(xprt, struct svcxprt_rdma, sc_xprt);
	struct ib_device *device;

	might_sleep();

	if (!rdma->sc_cm_id)
		goto out_free;

	device = rdma->sc_cm_id->device;

	/* This blocks until the Completion Queues are empty */
	if (rdma->sc_qp && !IS_ERR(rdma->sc_qp))
		ib_drain_qp(rdma->sc_qp);
	svc_rdma_send_ctxts_drain(rdma);
	svc_rdma_send_ctxts_stranded_release(rdma);

	svc_rdma_flush_recv_queues(rdma);
	svc_rdma_recv_ctxts_stranded_release(rdma);

	svc_rdma_destroy_rw_ctxts(rdma);
	svc_rdma_send_ctxts_destroy(rdma);
	svc_rdma_recv_ctxts_destroy(rdma);

	/* Destroy the QP if present (not a listener) */
	if (rdma->sc_qp && !IS_ERR(rdma->sc_qp))
		ib_destroy_qp(rdma->sc_qp);

	if (rdma->sc_sq_cq && !IS_ERR(rdma->sc_sq_cq))
		ib_free_cq(rdma->sc_sq_cq);

	if (rdma->sc_rq_cq && !IS_ERR(rdma->sc_rq_cq))
		ib_free_cq(rdma->sc_rq_cq);
	svc_rdma_cq_vector_put(rdma);

	if (rdma->sc_pd && !IS_ERR(rdma->sc_pd))
		ib_dealloc_pd(rdma->sc_pd);

	if (!test_bit(XPT_LISTENER, &rdma->sc_xprt.xpt_flags))
		rpcrdma_rn_unregister(device, &rdma->sc_rn);

	/* Destroy the CM ID */
	rdma_destroy_id(rdma->sc_cm_id);

out_free:
	kfree(rdma);
}

static int svc_rdma_has_wspace(struct svc_xprt *xprt)
{
	struct svcxprt_rdma *rdma =
		container_of(xprt, struct svcxprt_rdma, sc_xprt);

	/*
	 * If there are already waiters on the SQ,
	 * return false.
	 */
	if (waitqueue_active(&rdma->sc_send_wait) ||
	    waitqueue_active(&rdma->sc_sq_ticket_wait))
		return 0;

	/* Otherwise return true. */
	return 1;
}

static void svc_rdma_kill_temp_xprt(struct svc_xprt *xprt)
{
}
