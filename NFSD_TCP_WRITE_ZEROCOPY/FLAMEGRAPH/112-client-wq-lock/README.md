# Round 112: the client's workqueue lock at 2.5 M IOPS

Mike, 18 November: "profile the client's workqueue lock at 2.5 M IOPS".

## Why

Round 111's profiles (t2-r963, t2-r964) put about 10% of the client's
samples in spin locks (`_raw_spin_lock` 5.75%,
`native_queued_spin_lock_slowpath` 3.89%), most of them under
`queue_work_on` (6.16% with children), the lock of a workqueue pool as
the client queues work for rpciod and xprtiod.  Round 108 found that lock
the limit on build 469 (63.5% of the client's cycles); build 541's
canary-WIP commits cut it, and at 2.5 M IOPS it is back.  This round
finds which pool's lock, who takes it and how long they wait.

## The run

On the server at the tip's stack (`/var/tmp/svcstack-541-kunit`),
both borrows held by `settled-iops.sh`; the client on build 541,
`cache_shard`.  `prof-wq.sh 965`: one 4 KiB READ run, RUNTIME=35, and on
the client from the load's second 8, one after another: `perf record -a
-g` (6 s), `perf lock contention -a -b -v` (6 s, by caller with stacks),
`perf lock contention -a -b -l` (6 s, by lock).  Its rate is taken
under the probes.

## Results (t2-r965, 18 November, 02:47)

**2.52 M IOPS (1.26 / 1.26 M) under the probes**, 1.21 / 4.42 ms, server
71% busy (busiest 94%), client 71% (busiest 81%), no new kernel warning.
Reports in `/var/tmp/zc-run-logs/t2-r965/` (`cli-lock-callers.txt`,
`cli-lock-addr.txt`, `cli-self.txt`, `cli-children.txt`, `cli-wq.txt`).
(`cli-spin-callers.txt` came out empty: the caller report's options
matched nothing; the lock tracer below answers it.)

### Which lock: six unbound workqueue pools

`perf lock contention -l` puts almost all of it on six spinlocks in
`kmalloc-1k` objects, about 0.93 M contentions and 1.25 s of waiting each
in the 6 s, the rest far behind (`kmem_cache_node` 0.08 M, the slab).
A bound pool is static per-CPU data, not slab; six `kmalloc-1k` locks are
six unbound `worker_pool`s, cache_shard's six shards of 8 CPUs.

### Who takes it

| Caller (`perf lock contention -v`) | Contended | Waited | Avg |
|---|---|---|---|
| `worker_thread` (`kernel/workqueue.c:3545`), a worker between items | 2.39 M | 3.47 s | 1.45 us |
| `queue_work_on` from `__rpc_execute` (the task's release) | 1.51 M | 2.39 s | 1.58 us |
| `kthread` (a worker's loop) | 0.33 M | 0.42 s | 1.27 us |
| `__slab_free` from RCU's `mempool_free` | 0.10 M | 0.23 s | 2.24 us |
| `try_to_wake_up` from `__queue_work` and `process_one_work` | 0.16 M | 0.16 s | |
| `queue_work_on` from `tcp_data_queue` (xprtiod's receive work) | 0.016 M | 0.02 s | |

About 6.3 s of waiting in 6 s on 48 CPUs, 2.2% of the client; in
samples, `_raw_spin_lock` 5.73% and `native_queued_spin_lock_slowpath`
4.18% (self), `queue_work_on` 6.17% with children.

### Whose work: nfsiod's release of every READ

In build 541 (`4c8aa21d29987`) rpciod is per-CPU (`WQ_PERCPU`, the
canary-WIP change) and xprtiod and nfsiod unbound.  The requeue in
`__rpc_execute()` is the task's release: `rpc_release_task()` ->
`rpc_final_put_task(task, task->tk_workqueue)` queues
`rpc_async_release`, and an NFS READ's `tk_workqueue` is
`nfsiod_workqueue` (`fs/nfs/pagelist.c:856`).  So every 4 KiB READ queues
one small item to nfsiod (`rpc_async_release` 3.76% with children,
`nfs_direct_read_completion` 3.32% in it), about 15 M in the 6 s.  nfsiod
is `WQ_UNBOUND` without `WQ_SYSFS` (so it is not in `cli-wq.txt`), and
unbound workqueues with the same attributes share pools: nfsiod's
releases and xprtiod's receive work run on the same six pools, and the
workers fetching those short items are most of the waiting
(`worker_thread`).  Round 109 named xprtiod's pool lock; on build 541
with rpciod per-CPU it is nfsiod's release that fills it.

### What could remove it (not started)

1. **A/B of the client's scope at `cpu` (or `smt`) against cache_shard**,
   no build: `CLI_SCOPE` sets `default_affinity_scope`, which nfsiod and
   xprtiod take; at `cpu`, one pool a CPU, each lock taken by one CPU's
   queuers and workers.  Round 109 tried numa and cache_shard only.
2. **nfsiod per-CPU** (`WQ_PERCPU`), as build 541 made rpciod: the
   release queued on the CPU that ran the task, on that CPU's own pool.
3. **Release a READ's task inline on rpciod**, saving the item and the
   pool round trip altogether; `rpc_release` runs on nfsiod so that it
   may sleep, so this needs care for the procedures where it does.

## The A/B: the client's scope at cpu against cache_shard (plan)

Mike, 18 November: "A/B option 1 and then maybe we'll do option 2 if
option 1 goes well".  Writing `default_affinity_scope` reapplies it to
every unbound workqueue already made, nfsiod and xprtiod among them: at
`cpu` each has a pool a CPU.  Three runs a side in turns with round
109's `ab-scope.sh` (it sets `CLI_SCOPE` for each run and checks the
run's geometry line), at the IOPS setting, both borrows held:

    cd xeu-rig; ./step.sh /var/tmp/zc-run-logs/r112 scope ./settled-iops.sh run \
        ../FLAMEGRAPH/109-iops/ab-scope.sh 966:cpu 967:cache_shard 968:cpu 969:cache_shard 970:cpu 971:cache_shard

## The A/B's results (18 November, 03:16 to 03:35)

Every run's geometry line confirms its scope; no new kernel warning;
the host back at the settled setting after.

| Run | Scope | M IOPS (ports) | ms mean / p99 a; b | Client busy (busiest) | Server busy (busiest) |
|---|---|---|---|---|---|
| t2-r966 | cpu | 2.54 (1.25 / 1.29) | 1.23 / 4.69; 1.18 / 4.36 | 67% (78%) | 72% (96%) |
| t2-r967 | cache_shard | 2.52 (1.24 / 1.28) | 1.24 / 4.62; 1.20 / 4.55 | 70% (80%) | 71% (93%) |
| t2-r968 | cpu | 2.54 (1.27 / 1.27) | 1.21 / 4.88; 1.20 / 4.95 | 68% (76%) | 72% (98%) |
| t2-r969 | cache_shard | 2.54 (1.26 / 1.28) | 1.22 / 4.69; 1.20 / 4.55 | 71% (82%) | 72% (95%) |
| t2-r970 | cpu | 2.55 (1.27 / 1.28) | 1.20 / 4.82; 1.20 / 4.75 | 68% (77%) | 72% (96%) |
| t2-r971 | cache_shard | 2.53 (1.27 / 1.26) | 1.21 / 4.55; 1.21 / 4.55 | 71% (77%) | 71% (95%) |

- **Rate: cpu 2.543 M, cache_shard 2.530 M on the mean, 0.5%**: within
  the spread (cache_shard's own runs span 2.52 to 2.54).  No gain in rate.
- **Client CPU: 67 to 68% at cpu against 70 to 71%, 3 points less in
  every pair**, the busiest CPU 76 to 78% against 77 to 82%.  The pool
  lock's waiting goes, as round 112's profile measured it (about 2%).
- **p99 0.1 to 0.3 ms longer at cpu** (4.69 to 4.95 against 4.55 to
  4.69): a release or receive item waits for its own CPU's worker, with
  no other CPU of the shard to take it.
- **The server's busiest work CPU reads 95 to 98% in every run**: the
  client's lock no longer sets the rate; the server does.  A client
  change that only frees client CPU (option 2, nfsiod per-CPU) would
  likely show the same: less client CPU, the same rate.
