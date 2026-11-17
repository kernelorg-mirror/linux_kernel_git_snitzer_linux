# FLAMEGRAPH round 108: 4 KiB random READ IOPS

Mike, 17 November: "we need to do some 4K random READ IOPS runs,
starting with how fast all 8 drives can do it locally, using the
io_uring method is perfectly fine".  Rounds 104 to 107 measured
bandwidth; this round measures operations a second, from the drives
alone first, then over NFS.

## The drives alone

tardis2's eight data drives are DapuStor DPRP51016TTL15T3000 (PCIe 5,
15.36 TB, firmware FE003208), each with its own XFS; the server is 64
Neoverse-N2 cores on one node.  The NVMe driver has no poll queues
(`poll_queues` 0; the boot drive is NVMe too, so the driver is not
reloaded for them), so completions are interrupts.

`local.sh <label> <jobs a drive>:<iodepth> ...`: fio on tardis2 itself,
each drive's kept `f-*` files (about 2 GB a drive, written whole by
the fill: the raw device would read trimmed blocks, which a drive
answers without its flash), 4 KiB random reads, O_DIRECT, io_uring with
registered buffers and files, ramp 3 s and 20 s counted, mpstat for 10
s in the middle.  About to run through `xeu-rig/step.sh
/var/tmp/zc-run-logs/r108 local`: `local.sh sweep 1:32 2:64 4:64 8:32`.

### The sweep (`/var/tmp/zc-run-logs/r108-sweep`)

| jobs a drive : iodepth | jobs | M IOPS | GB/s | a drive, M | completion mean / p99, us | CPUs busy (busiest) |
|---|---|---|---|---|---|---|
| 1 : 32 | 8 | 2.99 | 12.2 | 0.37 to 0.39 | 83 / 118 | 15% (100%) |
| 2 : 64 | 16 | 6.22 | 25.5 | 0.66 to 0.83 | 163 / 232 | 28% (100%) |
| 4 : 64 | 32 | 12.59 | 51.6 | 1.55 to 1.62 | 160 / 234 | 52% (100%) |
| **8 : 32** | **64** | **16.05** | **65.7** | 1.81 to 2.21 | 126 / 334 | 72% (88%) |

- Up to 32 jobs a fio thread is its core's: about 390 K IOPS a job,
  the rate doubling with the jobs.
- At 64, a job to a CPU, 27% more and not twice: no CPU full, but at
  72% busy with every core running a job the CPUs are close too, and
  the drives spread from 1.81 to 2.21 M.  16 M IOPS is a floor on the
  eight drives, not their ceiling: the server's CPUs and the drives run
  out about together.
- No NVMe error or kernel warning.  (A "FAT-fs (nvme8n1p1): Volume was
  not properly unmounted" in the kernel log about 10:56 is the boot
  drive's EFI partition, most likely marked dirty by the morning's
  crash; not these runs.)

## Next (Mike: "run the 8:64 local point, then the NFS 4K runs")

`local.sh deep 8:64`, then over NFS `nfs4k.sh <n>:<iodepth>[:<nconnect>]`:
twoport.sh on both ports at the settled setting, fio on the client 16
jobs a port reading the same kept files with 4 KiB random reads on
io_uring with registered buffers, WARM=5 RUNTIME=20, mpstat on both
ends in the load.  `twoport.sh`'s fill now always says `iodepth=16`
(its default, so the fill's checksum is unchanged and the kept files
hold at any IODEPTH).  About to run through `xeu-rig/step.sh
/var/tmp/zc-run-logs/r108`: `local.sh deep 8:64`, then `nfs4k.sh
872:16 873:32 874:64`, 512 to 2048 READs outstanding a port.

### The 8:64 point and 4 KiB over NFS

| run | outstanding | M IOPS | completion mean / p99 | server busy (busiest) | client busy (busiest) |
|---|---|---|---|---|---|
| local 8 : 32 | 256 a drive | 16.05 | 126 / 334 us | 72% (88%) | |
| **local 8 : 64** | 512 a drive | **18.78** (2.29 to 2.39 a drive) | 215 / 659 us | 93% (99%) | |
| t2-r872, NFS, iodepth 16 | 256 a port | 0.98 (0.49 / 0.50) | 0.52 / 1.32 ms | 33% (100%) | 85% (89%) |
| t2-r873, NFS, iodepth 32 | 512 a port | 0.98 (0.49 / 0.49) | 1.05 / 2.41 ms | 34% (93%) | 85% (86%) |
| t2-r874, NFS, iodepth 64 | 1024 a port | 0.95 (0.47 / 0.48) | 2.17 / 4.69 ms | 30% (90%) | 89% (92%) |

- **Locally the server's 64 cores are the limit at 18.78 M IOPS**,
  93% busy, the drives even at 2.3 M each.
- **Over NFS about 1 M IOPS, 5% of that, at any depth**: twice the
  queue is twice the latency and no more IOPS, a fixed service rate.
  The client is 85 to 89% busy, evenly; the server about a third,
  though one of its CPUs is near full (32 connections, each one's READ
  work on one CPU).  Every run kept its files, new kernel warnings 0.
- Next: where a 4 KiB READ's time goes, a profile of both ends at
  iodepth 16.

## David Flynn's IOPS setting (settled-iops.sh)

Mike, 17 November: "run the NFS 4K runs with settled-iops.sh" (David
Flynn's `nfsv3-max-iops-agent-instructions-20261117.md`, 3.59 M IOPS
from one client on AWS).  `xeu-rig/settled-iops.sh` holds settled.sh
with 768 nfsd threads and his 256 MiB socket buffers, and its `run`
sets his load: each filesystem from its own server address
(`SRV_ALIAS=1`, eight rpc_clnts at nconnect 16, 128 connections), 24
jobs a port (one a client CPU) at iodepth 32.  The client is still
build 469, without his patch 27 (the rpc_clnt shards, `7cc54f28c69fc`
here); Mike is building the canary-WIP tip for it.  Two more 512 MiB
files a drive (`f-<port>.4` and `.5`, copied from `.0` and `.1` with
O_DIRECT) give the six jobs a filesystem their files.

    cd xeu-rig; ./step.sh /var/tmp/zc-run-logs/r108 iops ./settled-iops.sh run \
        ../FLAMEGRAPH/108-4k-randread/nfs4k.sh 875:32 876:32 877:32 878:16 879:64

Three at his iodepth 32, as he asks, then 16 and 64.  Runs are 20 s
(his are 240), so they set against t2-r872 to r874 above.

| Run, settled-iops.sh | in flight | M IOPS (a / b) | completion mean / p99 | server busy (busiest) | client busy (busiest) |
|---|---|---|---|---|---|
| t2-r875, iodepth 32 | 768 a port | 1.06 (0.52 / 0.53) | 1.46 / 4.15 ms | 48% (64%) | 94% (96%) |
| t2-r876, iodepth 32 | 768 a port | 1.05 (0.52 / 0.53) | 1.46 / 4.23 ms | 49% (66%) | 95% (97%) |
| t2-r877, iodepth 32 | 768 a port | 1.05 (0.52 / 0.53) | 1.46 / 4.18 ms | 48% (63%) | 94% (96%) |
| t2-r878, iodepth 16 | 384 a port | 1.00 (0.50 / 0.50) | 0.76 / 2.11 ms | 43% (57%) | 84% (87%) |
| t2-r879, iodepth 64 | 1536 a port | 0.94 (0.47 / 0.48) | 3.28 / 10.03 ms | 34% (58%) | 100% (100%) |

- **At his iodepth 32: 1.05 M IOPS median, 1.05 minimum**, 7% over
  the settled setting's 0.98 (t2-r873), and 29% of the 3.59 M he
  measured.  The wire is not near: 1 M 4 KiB READs are about 35
  Gbit/s on two 400 Gbit ports.
- **The client is the limit**: 94 to 95% busy at depth 32 and 100% at
  64, where the rate falls to 0.94.  The server is half idle, and the
  one CPU near full in t2-r872 to r874 is gone: its busiest is 57 to
  66% with the work over 128 connections.
- Eight rpc_clnts in place of two did not lift the client: its cost
  is each RPC's, which his patch 27 and the canary-WIP tip Mike is
  building for the client are to answer.  Every run kept its files,
  new kernel warnings 0; settled.sh's setting is back after.

## The profile of both ends at iodepth 32

Mike, 17 November: "profile both ends at iodepth 32 while I build the
client kernel".  `prof4k.sh` is one nfs4k.sh run under settled-iops.sh
with 8 s of `perf record -a -g` at 999 Hz on both ends at once from
the load's second 8, and the client's cycles and instructions a
socket:

    cd xeu-rig; ./step.sh /var/tmp/zc-run-logs/r108 prof4k \
        ../FLAMEGRAPH/108-4k-randread/prof4k.sh 880

t2-r880: 1.05 M IOPS (0.52 / 0.53), 1.46 / 4.18 ms, server 41% busy
(busiest 55%), client 95% (96%), new kernel warnings 0: the same as
t2-r875 to r877 with both ends recorded.

- **The client spends 63.5% of its cycles in
  `native_queued_spin_lock_slowpath`, on the workqueue's pool lock,
  not the rpc_clnt's `cl_lock`.**  `__queue_work` is 32.9% with its
  children and `process_one_work` itself (the worker taking the next
  item) most of the rest: every RPC step is queued on rpciod
  (`rpc_wake_up_queued_task` 15.5%, `rpc_async_schedule` 14.0%) and
  every receive on xprtiod (`xs_stream_data_receive_workfn` 18.9%).
  Both are unbound on build 469 with `affinity_scope` numa, so the 24
  CPUs of a socket share one pool and its lock (`kworker/u217` and
  `u218`).  `call_reserveresult` and `rpc_task_release_client`, where
  patch 27's `cl_lock` is taken, do not show.
- **The canary-WIP tip has David's fixes for this**, all ancestors of
  this branch: `ccaaf769083fe` (rpciod a per-CPU workqueue),
  `50eee8803afc5` (NFS pgio runs the first RPC leg in the caller, no
  queue), `ba5642ee4cac2` (a sent task to pending without a requeue),
  `5555163d661a0` (each CPU its own transport under nconnect), as well
  as patch 27 (`7cc54f28c69fc`).  xprtiod stays unbound there.
- **The server is not the limit**: 32% of its samples idle, nfsd
  16%; the largest self costs are the scheduler's switch (7%) and
  softirqs (5%), then `sunrpc_cache_find_rcu` and
  `svcauth_unix_set_client` at 1% each.

## The client on the canary-WIP tip (build 541)

17 November, 19:45: the client booted `7.3.3-16.hs.541.canary.el9.x86_64`,
Mike's build of the canary-WIP tip.  Read on it, nothing changed:
rpciod `per_cpu` 1, xprtiod unbound (`affinity_scope` default,
cache_shard), `sunrpc.xprt_numa_map` Y, `nfs.nowait_writable` N, both
ports at MTU 9000 and answering.  The commits of `IOPS.md`'s table are
in it: `INLINE_OK` in the sunrpc task flags' format,
`rpc_sleep_move_timeout`, `rpc_xprt_switch_update_cpu_map_locked`,
`rpc_clnt_alloc_task_shards`, `rpc_xprt_numa_assign` in its symbols.
The server is as it was, at the settled setting.  The plan, as the
handoff in `IOPS.md` has it:

    cd xeu-rig; ./step.sh /var/tmp/zc-run-logs/r108 iops-wip ./settled-iops.sh run \
        ../FLAMEGRAPH/108-4k-randread/nfs4k.sh 881:32 882:32 883:32
    ./step.sh /var/tmp/zc-run-logs/r108 prof4k-wip ../FLAMEGRAPH/108-4k-randread/prof4k.sh 884

t2-r881 and r882 did not run: twoport.sh refused, "the client's rpciod
and xprtiod not made strict".  On build 541 rpciod is a per-CPU
workqueue (`ccaaf769083fe`) and has no `affinity_strict`, which
twoport.sh (round 91) required of both.  A per-CPU workqueue runs each
item on the CPU that queued it, stricter than `affinity_strict`, so
twoport.sh now leaves a workqueue with `per_cpu` 1 as it is, holds
xprtiod (still unbound) strict as before, and its geometry line reads
"per-cpu/1".  Run again from t2-r883, settled.sh's setting put back
between:

    cd xeu-rig; ./step.sh /var/tmp/zc-run-logs/r108 iops-wip2 ./settled-iops.sh run \
        ../FLAMEGRAPH/108-4k-randread/nfs4k.sh 883:32 884:32 885:32
    ./step.sh /var/tmp/zc-run-logs/r108 prof4k-wip ../FLAMEGRAPH/108-4k-randread/prof4k.sh 886

During a run twoport.sh sets the client's workqueue
`default_affinity_scope` to numa (`CLI_SCOPE`, since 7 November) and
puts it back after; on build 541 the default is cache_shard, so
xprtiod runs at numa as on build 469.
