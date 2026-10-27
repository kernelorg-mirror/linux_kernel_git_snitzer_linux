/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Completion CPU choice for optimized LOCALIO completion.
 */
#ifndef _FS_NFS_LOCALIO_CPU_H
#define _FS_NFS_LOCALIO_CPU_H

#include <linux/cpumask.h>

/**
 * nfs_local_completion_cpu - pick the CPU that finishes a LOCALIO I/O
 * @origin: the CPU the backing I/O completed on
 * @node: the CPUs of @origin's NUMA node
 * @online: online CPUs
 * @hk_wq: workqueue housekeeping CPUs
 * @hk_domain: domain housekeeping CPUs
 *
 * With per-CPU block queues @origin is usually the submitter's CPU, and
 * finishing there can preempt the submitter.  Scan @node from the CPU after
 * @origin, wrapping, for one that is in @online, @hk_wq and @hk_domain.
 * @origin is taken only when it is the only such CPU.
 *
 * Return: the chosen CPU, or nr_cpu_ids when no CPU of @node qualifies.
 */
static inline unsigned int
nfs_local_completion_cpu(unsigned int origin, const struct cpumask *node,
			 const struct cpumask *online,
			 const struct cpumask *hk_wq,
			 const struct cpumask *hk_domain)
{
	unsigned int origin_fallback = nr_cpu_ids;
	unsigned int start = origin + 1;
	unsigned int cpu;

	if (start >= nr_cpu_ids)
		start = 0;
	for_each_cpu_wrap(cpu, node, start) {
		if (!cpumask_test_cpu(cpu, online) ||
		    !cpumask_test_cpu(cpu, hk_wq) ||
		    !cpumask_test_cpu(cpu, hk_domain))
			continue;
		if (cpu != origin)
			return cpu;
		origin_fallback = cpu;
	}
	return origin_fallback;
}

#endif /* _FS_NFS_LOCALIO_CPU_H */
