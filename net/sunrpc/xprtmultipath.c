// SPDX-License-Identifier: GPL-2.0
/*
 * Multipath support for RPC
 *
 * Copyright (c) 2015, 2016, Primary Data, Inc. All rights reserved.
 *
 * Trond Myklebust <trond.myklebust@primarydata.com>
 *
 */
#include <linux/atomic.h>
#include <linux/types.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/rcupdate.h>
#include <linux/rculist.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/sunrpc/xprt.h>
#include <linux/sunrpc/addr.h>
#include <linux/sunrpc/xprtmultipath.h>
#include <linux/inetdevice.h>
#include <linux/moduleparam.h>
#include <linux/topology.h>
#include <net/inet_dscp.h>
#include <net/route.h>

#include "sysfs.h"

typedef struct rpc_xprt *(*xprt_switch_find_xprt_t)(struct rpc_xprt_switch *xps,
		const struct rpc_xprt *cur);

static const struct rpc_xprt_iter_ops rpc_xprt_iter_singular;
static const struct rpc_xprt_iter_ops rpc_xprt_iter_percpu;
static const struct rpc_xprt_iter_ops rpc_xprt_iter_listall;
static const struct rpc_xprt_iter_ops rpc_xprt_iter_listoffline;

static void xprt_switch_add_xprt_locked(struct rpc_xprt_switch *xps,
		struct rpc_xprt *xprt)
{
	if (unlikely(xprt_get(xprt) == NULL))
		return;
	list_add_tail_rcu(&xprt->xprt_switch, &xps->xps_xprt_list);
	smp_wmb();
	if (xps->xps_nxprts == 0)
		xps->xps_net = xprt->xprt_net;
	xps->xps_nxprts++;
	xps->xps_nactive++;
	rpc_xprt_switch_update_cpu_map_locked(xps);
}

/**
 * rpc_xprt_switch_add_xprt - Add a new rpc_xprt to an rpc_xprt_switch
 * @xps: pointer to struct rpc_xprt_switch
 * @xprt: pointer to struct rpc_xprt
 *
 * Adds xprt to the end of the list of struct rpc_xprt in xps.
 */
void rpc_xprt_switch_add_xprt(struct rpc_xprt_switch *xps,
		struct rpc_xprt *xprt)
{
	if (xprt == NULL)
		return;
	rpc_xprt_numa_assign(xprt);
	spin_lock(&xps->xps_lock);
	if (xps->xps_net == xprt->xprt_net || xps->xps_net == NULL)
		xprt_switch_add_xprt_locked(xps, xprt);
	spin_unlock(&xps->xps_lock);
	rpc_sysfs_xprt_setup(xps, xprt, GFP_KERNEL);
}

static void xprt_switch_remove_xprt_locked(struct rpc_xprt_switch *xps,
		struct rpc_xprt *xprt, bool offline)
{
	if (unlikely(xprt == NULL))
		return;
	if (!test_bit(XPRT_OFFLINE, &xprt->state) && offline)
		xps->xps_nactive--;
	xps->xps_nxprts--;
	if (xps->xps_nxprts == 0)
		xps->xps_net = NULL;
	smp_wmb();
	list_del_rcu(&xprt->xprt_switch);
	WRITE_ONCE(xprt->xprt_home_cpu, -1);
	rpc_xprt_switch_update_cpu_map_locked(xps);
}

/**
 * rpc_xprt_switch_remove_xprt - Removes an rpc_xprt from a rpc_xprt_switch
 * @xps: pointer to struct rpc_xprt_switch
 * @xprt: pointer to struct rpc_xprt
 * @offline: indicates if the xprt that's being removed is in an offline state
 *
 * Removes xprt from the list of struct rpc_xprt in xps.
 */
void rpc_xprt_switch_remove_xprt(struct rpc_xprt_switch *xps,
		struct rpc_xprt *xprt, bool offline)
{
	spin_lock(&xps->xps_lock);
	xprt_switch_remove_xprt_locked(xps, xprt, offline);
	spin_unlock(&xps->xps_lock);
	xprt_put(xprt);
}

/**
 * rpc_xprt_switch_get_main_xprt - Get the 'main' xprt for an xprt switch.
 * @xps: pointer to struct rpc_xprt_switch.
 */
struct rpc_xprt *rpc_xprt_switch_get_main_xprt(struct rpc_xprt_switch *xps)
{
	struct rpc_xprt_iter xpi;
	struct rpc_xprt *xprt;

	xprt_iter_init_listall(&xpi, xps);

	xprt = xprt_iter_get_next(&xpi);
	while (xprt && !xprt->main) {
		xprt_put(xprt);
		xprt = xprt_iter_get_next(&xpi);
	}

	xprt_iter_destroy(&xpi);
	return xprt;
}

static DEFINE_IDA(rpc_xprtswitch_ids);

void xprt_multipath_cleanup_ids(void)
{
	ida_destroy(&rpc_xprtswitch_ids);
}

static int xprt_switch_alloc_id(struct rpc_xprt_switch *xps, gfp_t gfp_flags)
{
	int id;

	id = ida_alloc(&rpc_xprtswitch_ids, gfp_flags);
	if (id < 0)
		return id;

	xps->xps_id = id;
	return 0;
}

static void xprt_switch_free_id(struct rpc_xprt_switch *xps)
{
	ida_free(&rpc_xprtswitch_ids, xps->xps_id);
}

/**
 * xprt_switch_alloc - Allocate a new struct rpc_xprt_switch
 * @xprt: pointer to struct rpc_xprt
 * @gfp_flags: allocation flags
 *
 * On success, returns an initialised struct rpc_xprt_switch, containing
 * the entry xprt. Returns NULL on failure.
 */
struct rpc_xprt_switch *xprt_switch_alloc(struct rpc_xprt *xprt,
		gfp_t gfp_flags)
{
	struct rpc_xprt_switch *xps;

	xps = kmalloc_obj(*xps, gfp_flags);
	if (xps != NULL) {
		spin_lock_init(&xps->xps_lock);
		kref_init(&xps->xps_kref);
		xprt_switch_alloc_id(xps, gfp_flags);
		xps->xps_nxprts = xps->xps_nactive = 0;
		xps->xps_net = NULL;
		INIT_LIST_HEAD(&xps->xps_xprt_list);
		xps->xps_iter_ops = &rpc_xprt_iter_singular;
		RCU_INIT_POINTER(xps->xps_cpu_map, NULL);
		rpc_sysfs_xprt_switch_setup(xps, xprt, gfp_flags);
		rpc_xprt_numa_assign(xprt);
		xprt_switch_add_xprt_locked(xps, xprt);
		xps->xps_nunique_destaddr_xprts = 1;
		rpc_sysfs_xprt_setup(xps, xprt, gfp_flags);
	}

	return xps;
}

static void xprt_switch_free_entries(struct rpc_xprt_switch *xps)
{
	spin_lock(&xps->xps_lock);
	while (!list_empty(&xps->xps_xprt_list)) {
		struct rpc_xprt *xprt;

		xprt = list_first_entry(&xps->xps_xprt_list,
				struct rpc_xprt, xprt_switch);
		xprt_switch_remove_xprt_locked(xps, xprt, true);
		spin_unlock(&xps->xps_lock);
		xprt_put(xprt);
		spin_lock(&xps->xps_lock);
	}
	spin_unlock(&xps->xps_lock);
}

static void xprt_switch_free(struct kref *kref)
{
	struct rpc_xprt_switch *xps = container_of(kref,
			struct rpc_xprt_switch, xps_kref);

	xprt_switch_free_entries(xps);
	kfree(rcu_dereference_protected(xps->xps_cpu_map, true));
	rpc_sysfs_xprt_switch_destroy(xps);
	xprt_switch_free_id(xps);
	kfree_rcu(xps, xps_rcu);
}

/**
 * xprt_switch_get - Return a reference to a rpc_xprt_switch
 * @xps: pointer to struct rpc_xprt_switch
 *
 * Returns a reference to xps unless the refcount is already zero.
 */
struct rpc_xprt_switch *xprt_switch_get(struct rpc_xprt_switch *xps)
{
	if (xps != NULL && kref_get_unless_zero(&xps->xps_kref))
		return xps;
	return NULL;
}

/**
 * xprt_switch_put - Release a reference to a rpc_xprt_switch
 * @xps: pointer to struct rpc_xprt_switch
 *
 * Release the reference to xps, and free it once the refcount is zero.
 */
void xprt_switch_put(struct rpc_xprt_switch *xps)
{
	if (xps != NULL)
		kref_put(&xps->xps_kref, xprt_switch_free);
}

struct rpc_xprt_cpu_map {
	struct rcu_head		rcu;
	struct rpc_xprt		*xprt[];	/* indexed by CPU; NULL: none */
};

/*
 * LAB ONLY switch.  Y: each transport gets a home core on the NUMA node of
 * the NIC its route leaves by, from a rotor per node shared by every
 * switch, so the transports to all servers cover every core of the node
 * before any core is home to two; a per-CPU switch is then mapped around
 * those homes.  N: the CPU-count split of rpc_xprt_switch_update_cpu_map_locked().
 */
static bool rpc_xprt_numa_map __read_mostly = true;
module_param_named(xprt_numa_map, rpc_xprt_numa_map, bool, 0644);
MODULE_PARM_DESC(xprt_numa_map,
		 "LAB ONLY: home nconnect transports on their NIC's NUMA node, one core each, staggered across servers (default Y)");

static atomic_t rpc_xprt_numa_rotor[MAX_NUMNODES];

/* The NUMA node of the NIC the route to @xprt's server leaves by */
static int rpc_xprt_route_node(struct rpc_xprt *xprt)
{
	const struct sockaddr_in *sin = (const struct sockaddr_in *)&xprt->addr;
	struct net_device *dev;
	struct rtable *rt;
	int node = NUMA_NO_NODE;

	if (xprt->addr.ss_family != AF_INET || !xprt->xprt_net)
		return NUMA_NO_NODE;
	rt = ip_route_output(xprt->xprt_net, sin->sin_addr.s_addr, 0,
			     inet_dsfield_to_dscp(0), 0, RT_SCOPE_UNIVERSE);
	if (IS_ERR(rt))
		return NUMA_NO_NODE;
	dev = rt->dst.dev;
	if (dev && (dev->flags & IFF_LOOPBACK)) {
		/* A server address of this host: the NIC that owns it */
		rcu_read_lock();
		dev = __ip_dev_find(xprt->xprt_net, sin->sin_addr.s_addr, false);
		if (dev)
			node = dev_to_node(&dev->dev);
		rcu_read_unlock();
	} else if (dev) {
		node = dev_to_node(&dev->dev);
	}
	ip_rt_put(rt);
	return node;
}

/* Position of @cpu among the online CPUs of @node, or -1 */
static int rpc_xprt_numa_pos(int cpu, int node)
{
	int c, pos = 0;

	if (cpu < 0 || node == NUMA_NO_NODE)
		return -1;
	for_each_cpu_and(c, cpumask_of_node(node), cpu_online_mask) {
		if (c == cpu)
			return pos;
		pos++;
	}
	return -1;
}

/**
 * rpc_xprt_numa_assign - Give @xprt a home core on its NIC's NUMA node
 * @xprt: transport, before it joins a switch and before it connects
 *
 * Process context.  Does nothing if @xprt already has a home, or if the
 * route to its server gives no node.
 */
void rpc_xprt_numa_assign(struct rpc_xprt *xprt)
{
	unsigned int cnt, pos, cpu;
	int node;

	if (!READ_ONCE(rpc_xprt_numa_map) || READ_ONCE(xprt->xprt_numa_home) >= 0)
		return;
	node = rpc_xprt_route_node(xprt);
	if (node == NUMA_NO_NODE || node < 0 || node >= MAX_NUMNODES)
		return;
	cnt = cpumask_weight_and(cpumask_of_node(node), cpu_online_mask);
	if (cnt == 0)
		return;
	pos = (unsigned int)atomic_fetch_inc(&rpc_xprt_numa_rotor[node]) % cnt;
	cpu = cpumask_nth_and(pos, cpumask_of_node(node), cpu_online_mask);
	if (cpu >= nr_cpu_ids)
		return;
	WRITE_ONCE(xprt->xprt_numa_node, node);
	WRITE_ONCE(xprt->xprt_numa_home, cpu);
}

/**
 * rpc_xprt_numa_comp_vector - The completion vector of @xprt's home core
 * @xprt: transport
 * @nvec: completion vectors of its device
 *
 * Assumes vector i interrupts on the i-th online CPU of the device's node,
 * as mlx5 spreads them and as the lab pins them.  Returns -1 without a home.
 */
int rpc_xprt_numa_comp_vector(const struct rpc_xprt *xprt, int nvec)
{
	int pos;

	if (!READ_ONCE(rpc_xprt_numa_map) || nvec <= 0)
		return -1;
	pos = rpc_xprt_numa_pos(READ_ONCE(xprt->xprt_numa_home),
				READ_ONCE(xprt->xprt_numa_node));
	return pos < 0 ? -1 : pos % nvec;
}
EXPORT_SYMBOL_GPL(rpc_xprt_numa_comp_vector);

/*
 * Map a per-CPU switch around its transports' homes.  On the transports'
 * NIC node each home core uses the transport homed on it and the other
 * cores are dealt round the transports in turn; the cores of any other
 * node are dealt round them the same way.  Returns false, leaving @map
 * alone, unless every active transport has a home on one node.
 */
static bool rpc_xprt_switch_numa_map_locked(struct rpc_xprt_cpu_map *map,
					    struct rpc_xprt **active,
					    unsigned int n)
{
	int node = READ_ONCE(active[0]->xprt_numa_node);
	unsigned int i, k;
	int cpu, other;

	if (!READ_ONCE(rpc_xprt_numa_map) || node == NUMA_NO_NODE)
		return false;
	for (i = 0; i < n; i++)
		if (READ_ONCE(active[i]->xprt_numa_node) != node ||
		    READ_ONCE(active[i]->xprt_numa_home) < 0)
			return false;

	for (i = 0; i < n; i++) {
		cpu = READ_ONCE(active[i]->xprt_numa_home);
		WRITE_ONCE(active[i]->xprt_home_cpu, cpu);
		if (!map->xprt[cpu])
			map->xprt[cpu] = active[i];
	}
	k = 0;
	for_each_cpu_and(cpu, cpumask_of_node(node), cpu_online_mask)
		if (!map->xprt[cpu])
			map->xprt[cpu] = active[k++ % n];
	for_each_node_with_cpus(other) {
		if (other == node)
			continue;
		k = 0;
		for_each_cpu_and(cpu, cpumask_of_node(other), cpu_online_mask)
			map->xprt[cpu] = active[k++ % n];
	}
	return true;
}

/**
 * rpc_xprt_switch_update_cpu_map_locked - Rebuild a per-CPU switch's CPU map
 * @xps: pointer to struct rpc_xprt_switch, with xps_lock held
 *
 * Gives each online CPU one active transport and each such transport a home
 * CPU, the way nvme-tcp gives each CPU an I/O queue.  Each NUMA node gets a
 * share of the transports in proportion to its online CPUs, so a CPU's
 * transport is homed on its own node.  CPUs that come online later, or a
 * failed allocation, have no entry and use the round-robin fallback.  A
 * switch with fewer than two active transports, or with transports to more
 * than one server address, gets no map and no home CPU.
 */
void rpc_xprt_switch_update_cpu_map_locked(struct rpc_xprt_switch *xps)
{
	struct rpc_xprt_cpu_map *map, *old;
	struct rpc_xprt **active, *xprt;
	unsigned int n = 0, seen = 0, total;
	bool multiaddr = false;
	int node, cpu;

	if (xps->xps_iter_ops != &rpc_xprt_iter_percpu)
		return;
	list_for_each_entry(xprt, &xps->xps_xprt_list, xprt_switch)
		WRITE_ONCE(xprt->xprt_home_cpu, -1);

	map = kzalloc(struct_size(map, xprt, nr_cpu_ids), GFP_ATOMIC);
	active = kmalloc_array(max(xps->xps_nactive, 1U), sizeof(*active),
			       GFP_ATOMIC);
	if (map && active)
		list_for_each_entry(xprt, &xps->xps_xprt_list, xprt_switch)
			if (!test_bit(XPRT_OFFLINE, &xprt->state) &&
			    n < xps->xps_nactive) {
				if (n && !rpc_cmp_addr(
					(struct sockaddr *)&xprt->addr,
					(struct sockaddr *)&active[0]->addr))
					multiaddr = true;
				active[n++] = xprt;
			}
	total = num_online_cpus();
	/*
	 * Steer only nconnect: one transport (failed trunking, a lone DS
	 * alias) has nothing to steer, and session trunking to several
	 * server addresses keeps spreading every CPU's load across them.
	 * Compare addresses only: nconnect clones of a port-0 (rpcbind)
	 * transport are added before their port is set.
	 */
	if (n < 2 || multiaddr) {
		kfree(map);
		map = NULL;
		goto publish;
	}
	if (rpc_xprt_switch_numa_map_locked(map, active, n))
		goto publish;
	for_each_node_with_cpus(node) {
		const struct cpumask *mask = cpumask_of_node(node);
		unsigned int cnt = cpumask_weight_and(mask, cpu_online_mask);
		unsigned int start, end, span, j = 0;

		if (cnt == 0)
			continue;
		start = n * seen / total;
		end = n * (seen + cnt) / total;
		span = end > start ? end - start : 1;
		for_each_cpu_and(cpu, mask, cpu_online_mask) {
			xprt = active[(start + j++ % span) % n];
			map->xprt[cpu] = xprt;
			if (READ_ONCE(xprt->xprt_home_cpu) < 0)
				WRITE_ONCE(xprt->xprt_home_cpu, cpu);
		}
		seen += cnt;
	}
publish:
	kfree(active);
	old = rcu_replace_pointer(xps->xps_cpu_map, map,
				  lockdep_is_held(&xps->xps_lock));
	if (old)
		kfree_rcu(old, rcu);
}

/**
 * rpc_xprt_switch_set_percpu - Set the per-CPU policy on rpc_xprt_switch
 * @xps: pointer to struct rpc_xprt_switch
 *
 * Each submitting CPU uses its own transport (see
 * rpc_xprt_switch_update_cpu_map_locked()), so CPUs do not share a cursor,
 * a send lock or a socket.
 */
void rpc_xprt_switch_set_percpu(struct rpc_xprt_switch *xps)
{
	spin_lock(&xps->xps_lock);
	if (xps->xps_iter_ops != &rpc_xprt_iter_percpu) {
		WRITE_ONCE(xps->xps_iter_ops, &rpc_xprt_iter_percpu);
		rpc_xprt_switch_update_cpu_map_locked(xps);
	}
	spin_unlock(&xps->xps_lock);
}

/**
 * rpc_xprt_switch_queuelen - Sum the queue lengths of a switch's transports
 * @xps: pointer to struct rpc_xprt_switch
 */
long rpc_xprt_switch_queuelen(struct rpc_xprt_switch *xps)
{
	struct rpc_xprt *xprt;
	long sum = 0;

	rcu_read_lock();
	list_for_each_entry_rcu(xprt, &xps->xps_xprt_list, xprt_switch)
		sum += atomic_long_read(&xprt->queuelen);
	rcu_read_unlock();
	return sum;
}

static
const struct rpc_xprt_iter_ops *xprt_iter_ops(const struct rpc_xprt_iter *xpi)
{
	if (xpi->xpi_ops != NULL)
		return xpi->xpi_ops;
	return rcu_dereference(xpi->xpi_xpswitch)->xps_iter_ops;
}

static
void xprt_iter_no_rewind(struct rpc_xprt_iter *xpi)
{
}

static
void xprt_iter_default_rewind(struct rpc_xprt_iter *xpi)
{
	WRITE_ONCE(xpi->xpi_cursor, NULL);
}

static
bool xprt_is_active(const struct rpc_xprt *xprt)
{
	return (kref_read(&xprt->kref) != 0 &&
		!test_bit(XPRT_OFFLINE, &xprt->state));
}

static
struct rpc_xprt *xprt_switch_find_first_entry(struct list_head *head)
{
	struct rpc_xprt *pos;

	list_for_each_entry_rcu(pos, head, xprt_switch) {
		if (xprt_is_active(pos))
			return pos;
	}
	return NULL;
}

static
struct rpc_xprt *xprt_switch_find_first_entry_offline(struct list_head *head)
{
	struct rpc_xprt *pos;

	list_for_each_entry_rcu(pos, head, xprt_switch) {
		if (!xprt_is_active(pos))
			return pos;
	}
	return NULL;
}

static
struct rpc_xprt *xprt_iter_first_entry(struct rpc_xprt_iter *xpi)
{
	struct rpc_xprt_switch *xps = rcu_dereference(xpi->xpi_xpswitch);

	if (xps == NULL)
		return NULL;
	return xprt_switch_find_first_entry(&xps->xps_xprt_list);
}

static
struct rpc_xprt *_xprt_switch_find_current_entry(struct list_head *head,
						 const struct rpc_xprt *cur,
						 bool find_active)
{
	struct rpc_xprt *pos;
	bool found = false;

	list_for_each_entry_rcu(pos, head, xprt_switch) {
		if (cur == pos)
			found = true;
		if (found && ((find_active && xprt_is_active(pos)) ||
			      (!find_active && !xprt_is_active(pos))))
			return pos;
	}
	return NULL;
}

static
struct rpc_xprt *xprt_switch_find_current_entry(struct list_head *head,
						const struct rpc_xprt *cur)
{
	return _xprt_switch_find_current_entry(head, cur, true);
}

static
struct rpc_xprt * _xprt_iter_current_entry(struct rpc_xprt_iter *xpi,
		struct rpc_xprt *first_entry(struct list_head *head),
		struct rpc_xprt *current_entry(struct list_head *head,
					       const struct rpc_xprt *cur))
{
	struct rpc_xprt_switch *xps = rcu_dereference(xpi->xpi_xpswitch);
	struct list_head *head;

	if (xps == NULL)
		return NULL;
	head = &xps->xps_xprt_list;
	if (xpi->xpi_cursor == NULL || xps->xps_nxprts < 2)
		return first_entry(head);
	return current_entry(head, xpi->xpi_cursor);
}

static
struct rpc_xprt *xprt_iter_current_entry(struct rpc_xprt_iter *xpi)
{
	return _xprt_iter_current_entry(xpi, xprt_switch_find_first_entry,
			xprt_switch_find_current_entry);
}

static
struct rpc_xprt *xprt_switch_find_current_entry_offline(struct list_head *head,
		const struct rpc_xprt *cur)
{
	return _xprt_switch_find_current_entry(head, cur, false);
}

static
struct rpc_xprt *xprt_iter_current_entry_offline(struct rpc_xprt_iter *xpi)
{
	return _xprt_iter_current_entry(xpi,
			xprt_switch_find_first_entry_offline,
			xprt_switch_find_current_entry_offline);
}

static
bool __rpc_xprt_switch_has_addr(struct rpc_xprt_switch *xps,
				const struct sockaddr *sap)
{
	struct list_head *head;
	struct rpc_xprt *pos;

	if (xps == NULL || sap == NULL)
		return false;

	head = &xps->xps_xprt_list;
	list_for_each_entry_rcu(pos, head, xprt_switch) {
		if (rpc_cmp_addr_port(sap, (struct sockaddr *)&pos->addr)) {
			pr_info("RPC:   addr %s already in xprt switch\n",
				pos->address_strings[RPC_DISPLAY_ADDR]);
			return true;
		}
	}
	return false;
}

bool rpc_xprt_switch_has_addr(struct rpc_xprt_switch *xps,
			      const struct sockaddr *sap)
{
	bool res;

	rcu_read_lock();
	res = __rpc_xprt_switch_has_addr(xps, sap);
	rcu_read_unlock();

	return res;
}

static
struct rpc_xprt *xprt_switch_find_next_entry(struct list_head *head,
		const struct rpc_xprt *cur, bool check_active)
{
	struct rpc_xprt *pos, *prev = NULL;
	bool found = false;

	list_for_each_entry_rcu(pos, head, xprt_switch) {
		if (cur == prev)
			found = true;
		/* for request to return active transports return only
		 * active, for request to return offline transports
		 * return only offline
		 */
		if (found && ((check_active && xprt_is_active(pos)) ||
			      (!check_active && !xprt_is_active(pos))))
			return pos;
		prev = pos;
	}
	return NULL;
}

static
struct rpc_xprt *xprt_switch_set_next_cursor(struct rpc_xprt_switch *xps,
		struct rpc_xprt **cursor,
		xprt_switch_find_xprt_t find_next)
{
	struct rpc_xprt *pos, *old;

	old = smp_load_acquire(cursor);
	pos = find_next(xps, old);
	smp_store_release(cursor, pos);
	return pos;
}

static
struct rpc_xprt *xprt_iter_next_entry_multiple(struct rpc_xprt_iter *xpi,
		xprt_switch_find_xprt_t find_next)
{
	struct rpc_xprt_switch *xps = rcu_dereference(xpi->xpi_xpswitch);

	if (xps == NULL)
		return NULL;
	return xprt_switch_set_next_cursor(xps, &xpi->xpi_cursor, find_next);
}

static
struct rpc_xprt *__xprt_switch_find_next_entry_roundrobin(struct list_head *head,
		const struct rpc_xprt *cur)
{
	struct rpc_xprt *ret;

	ret = xprt_switch_find_next_entry(head, cur, true);
	if (ret != NULL)
		return ret;
	return xprt_switch_find_first_entry(head);
}

static
struct rpc_xprt *xprt_switch_find_next_entry_roundrobin(struct rpc_xprt_switch *xps,
		const struct rpc_xprt *cur)
{
	return __xprt_switch_find_next_entry_roundrobin(&xps->xps_xprt_list, cur);
}

/*
 * A CPU that submits far more than its share (one busy job on a quiet
 * mount) spills to the round-robin choice once its own transport holds
 * this many tasks and that choice holds fewer than half as many.
 */
#define RPC_XPRT_PERCPU_SPILL	64

static
struct rpc_xprt *xprt_iter_next_entry_percpu(struct rpc_xprt_iter *xpi)
{
	struct rpc_xprt_switch *xps = rcu_dereference(xpi->xpi_xpswitch);
	struct rpc_xprt_cpu_map *map;
	struct rpc_xprt *xprt = NULL, *other;
	long len;

	if (xps == NULL)
		return NULL;
	map = rcu_dereference(xps->xps_cpu_map);
	if (map)
		xprt = map->xprt[raw_smp_processor_id()];
	if (!xprt || !xprt_is_active(xprt))
		return xprt_iter_next_entry_multiple(xpi,
				xprt_switch_find_next_entry_roundrobin);
	len = atomic_long_read(&xprt->queuelen);
	if (len < RPC_XPRT_PERCPU_SPILL)
		return xprt;
	other = xprt_iter_next_entry_multiple(xpi,
			xprt_switch_find_next_entry_roundrobin);
	if (other && 2 * atomic_long_read(&other->queuelen) < len)
		return other;
	return xprt;
}

static
struct rpc_xprt *xprt_switch_find_next_entry_all(struct rpc_xprt_switch *xps,
		const struct rpc_xprt *cur)
{
	return xprt_switch_find_next_entry(&xps->xps_xprt_list, cur, true);
}

static
struct rpc_xprt *xprt_switch_find_next_entry_offline(struct rpc_xprt_switch *xps,
		const struct rpc_xprt *cur)
{
	return xprt_switch_find_next_entry(&xps->xps_xprt_list, cur, false);
}

static
struct rpc_xprt *xprt_iter_next_entry_all(struct rpc_xprt_iter *xpi)
{
	return xprt_iter_next_entry_multiple(xpi,
			xprt_switch_find_next_entry_all);
}

static
struct rpc_xprt *xprt_iter_next_entry_offline(struct rpc_xprt_iter *xpi)
{
	return xprt_iter_next_entry_multiple(xpi,
			xprt_switch_find_next_entry_offline);
}

/*
 * xprt_iter_rewind - Resets the xprt iterator
 * @xpi: pointer to rpc_xprt_iter
 *
 * Resets xpi to ensure that it points to the first entry in the list
 * of transports.
 */
void xprt_iter_rewind(struct rpc_xprt_iter *xpi)
{
	rcu_read_lock();
	xprt_iter_ops(xpi)->xpi_rewind(xpi);
	rcu_read_unlock();
}

static void __xprt_iter_init(struct rpc_xprt_iter *xpi,
		struct rpc_xprt_switch *xps,
		const struct rpc_xprt_iter_ops *ops)
{
	rcu_assign_pointer(xpi->xpi_xpswitch, xprt_switch_get(xps));
	xpi->xpi_cursor = NULL;
	xpi->xpi_ops = ops;
}

/**
 * xprt_iter_init - Initialise an xprt iterator
 * @xpi: pointer to rpc_xprt_iter
 * @xps: pointer to rpc_xprt_switch
 *
 * Initialises the iterator to use the default iterator ops
 * as set in xps. This function is mainly intended for internal
 * use in the rpc_client.
 */
void xprt_iter_init(struct rpc_xprt_iter *xpi,
		struct rpc_xprt_switch *xps)
{
	__xprt_iter_init(xpi, xps, NULL);
}

/**
 * xprt_iter_init_listall - Initialise an xprt iterator
 * @xpi: pointer to rpc_xprt_iter
 * @xps: pointer to rpc_xprt_switch
 *
 * Initialises the iterator to iterate once through the entire list
 * of entries in xps.
 */
void xprt_iter_init_listall(struct rpc_xprt_iter *xpi,
		struct rpc_xprt_switch *xps)
{
	__xprt_iter_init(xpi, xps, &rpc_xprt_iter_listall);
}

void xprt_iter_init_listoffline(struct rpc_xprt_iter *xpi,
		struct rpc_xprt_switch *xps)
{
	__xprt_iter_init(xpi, xps, &rpc_xprt_iter_listoffline);
}

/**
 * xprt_iter_xchg_switch - Atomically swap out the rpc_xprt_switch
 * @xpi: pointer to rpc_xprt_iter
 * @newswitch: pointer to a new rpc_xprt_switch or NULL
 *
 * Swaps out the existing xpi->xpi_xpswitch with a new value.
 */
struct rpc_xprt_switch *xprt_iter_xchg_switch(struct rpc_xprt_iter *xpi,
		struct rpc_xprt_switch *newswitch)
{
	struct rpc_xprt_switch __rcu *oldswitch;

	/* Atomically swap out the old xpswitch */
	oldswitch = xchg(&xpi->xpi_xpswitch, RCU_INITIALIZER(newswitch));
	if (newswitch != NULL)
		xprt_iter_rewind(xpi);
	return rcu_dereference_protected(oldswitch, true);
}

/**
 * xprt_iter_destroy - Destroys the xprt iterator
 * @xpi: pointer to rpc_xprt_iter
 */
void xprt_iter_destroy(struct rpc_xprt_iter *xpi)
{
	xprt_switch_put(xprt_iter_xchg_switch(xpi, NULL));
}

/**
 * xprt_iter_xprt - Returns the rpc_xprt pointed to by the cursor
 * @xpi: pointer to rpc_xprt_iter
 *
 * Returns a pointer to the struct rpc_xprt that is currently
 * pointed to by the cursor.
 * Caller must be holding rcu_read_lock().
 */
struct rpc_xprt *xprt_iter_xprt(struct rpc_xprt_iter *xpi)
{
	WARN_ON_ONCE(!rcu_read_lock_held());
	return xprt_iter_ops(xpi)->xpi_xprt(xpi);
}

static
struct rpc_xprt *xprt_iter_get_helper(struct rpc_xprt_iter *xpi,
		struct rpc_xprt *(*fn)(struct rpc_xprt_iter *))
{
	struct rpc_xprt *ret;

	do {
		ret = fn(xpi);
		if (ret == NULL)
			break;
		ret = xprt_get(ret);
	} while (ret == NULL);
	return ret;
}

/**
 * xprt_iter_get_next - Returns the next rpc_xprt following the cursor
 * @xpi: pointer to rpc_xprt_iter
 *
 * Returns a reference to the struct rpc_xprt that immediately follows the
 * entry pointed to by the cursor.
 */
struct rpc_xprt *xprt_iter_get_next(struct rpc_xprt_iter *xpi)
{
	struct rpc_xprt *xprt;

	rcu_read_lock();
	xprt = xprt_iter_get_helper(xpi, xprt_iter_ops(xpi)->xpi_next);
	rcu_read_unlock();
	return xprt;
}

/* Policy for always returning the first entry in the rpc_xprt_switch */
static
const struct rpc_xprt_iter_ops rpc_xprt_iter_singular = {
	.xpi_rewind = xprt_iter_no_rewind,
	.xpi_xprt = xprt_iter_first_entry,
	.xpi_next = xprt_iter_first_entry,
};

/*
 * Policy for per-CPU selection of entries in the rpc_xprt_switch, falling
 * back to round-robin where the CPU has no active entry
 */
static
const struct rpc_xprt_iter_ops rpc_xprt_iter_percpu = {
	.xpi_rewind = xprt_iter_default_rewind,
	.xpi_xprt = xprt_iter_current_entry,
	.xpi_next = xprt_iter_next_entry_percpu,
};

/* Policy for once-through iteration of entries in the rpc_xprt_switch */
static
const struct rpc_xprt_iter_ops rpc_xprt_iter_listall = {
	.xpi_rewind = xprt_iter_default_rewind,
	.xpi_xprt = xprt_iter_current_entry,
	.xpi_next = xprt_iter_next_entry_all,
};

static
const struct rpc_xprt_iter_ops rpc_xprt_iter_listoffline = {
	.xpi_rewind = xprt_iter_default_rewind,
	.xpi_xprt = xprt_iter_current_entry_offline,
	.xpi_next = xprt_iter_next_entry_offline,
};
