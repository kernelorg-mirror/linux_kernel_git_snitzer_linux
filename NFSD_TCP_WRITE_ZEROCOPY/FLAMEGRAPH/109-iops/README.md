# Round 109: 4 KiB READ IOPS on the canary-WIP client

The plan is "Round 109: what to chase next" in `../../IOPS.md`; the base
is t2-r883 to r887 (round 108): 1.81 M IOPS median at iodepth 32 under
`xeu-rig/settled-iops.sh`, client build 541, neither end saturated.

## 1. The server's pool_mode, percpu (what auto picks) against pernode

`ab-pool.sh` (Mike, 17 November): three runs a side in turns, each one
nfs4k.sh run at iodepth 32 under settled-iops.sh, `EXP_POOL_MODE`
percpu (64 pools of 12 threads) or pernode (one pool of 768,
settled.sh's).  David's servers ran auto, which on this sunrpc always
picks percpu (`svc_pool_map_choose_mode()`); percpu is set by name so
that the audit reads back what it set.  READs run in rtc work items, not on
nfsd's threads (2.5% of t2-r886's samples), so little is expected; it
settles what David Flynn's setup (auto) assumed.

    cd xeu-rig; ./step.sh /var/tmp/zc-run-logs/r109 abpool ../FLAMEGRAPH/109-iops/ab-pool.sh \
        888:percpu 889:pernode 890:percpu 891:pernode 892:percpu 893:pernode

| Run | pool_mode | M IOPS (a / b) | completion mean / p99 | server busy (busiest) | client busy (busiest) |
|---|---|---|---|---|---|
| t2-r888 | percpu (64 pools) | 1.91 (0.97 / 0.94) | 0.79 / 2.93 ms | 56% (95%) | 65% (75%) |
| t2-r889 | pernode (1 pool) | 1.85 (0.92 / 0.93) | 0.83 / 2.83 ms | 53% (90%) | 63% (70%) |
| t2-r890 | percpu | 1.93 (0.97 / 0.96) | 0.79 / 2.97 ms | 56% (97%) | 66% (74%) |
| t2-r891 | pernode | 1.90 (0.94 / 0.96) | 0.81 / 2.67 ms | 56% (84%) | 64% (69%) |
| t2-r892 | percpu | 1.96 (0.97 / 0.99) | 0.79 / 2.80 ms | 57% (92%) | 66% (72%) |
| t2-r893 | pernode | 1.89 (0.96 / 0.94) | 0.80 / 2.74 ms | 54% (88%) | 64% (71%) |

- **percpu: 1.93 M IOPS median, 1.91 minimum; pernode: 1.89 median,
  1.85 minimum.**  About 2% more on percpu, small, but every percpu run
  is above every pernode run (1.91 against at most 1.90).  The
  pernode runs here read 1.85 to 1.90, a little above t2-r883 to r887
  (1.81 to 1.86) at the same setting.
- percpu takes the server's busiest CPU higher (92 to 97% against 84
  to 90%) and its p99 a little longer (2.80 to 2.97 ms against 2.67 to
  2.83); the client is 1 to 2 points busier, as for the extra rate.
- READs run in rtc work items, so the pools reach them only through
  the nfsd threads' share of the work (2.5% of samples in t2-r886);
  2% is about what that leaves room for.
- Every run confirmed its mode in the audit (64 pools of 12 threads,
  or one of 768), new kernel warnings 0, settled.sh's setting (pernode)
  after each.  settled.sh is not changed: percpu cost WRITEs on builds
  512 and 514 (IOPS.md), so for IOPS it is an `EXP_POOL_MODE=percpu`
  run setting, and settling it would take a WRITE pass and Mike's word.
- **Decided (Mike, 17 November): pernode stays settled, and percpu is
  ruled out for now** ("just leave pernode settled.  Nice to have ruled
  out percpu at this time").  The rest of round 109 runs at pernode.

- **Then, for IOPS (Mike, 17 November, on 1.96 being percpu's): "for
  settled-iops.sh settle percpu".**  `settled-iops.sh` holds
  `EXP_POOL_MODE=percpu` with its other values; settled.sh stays
  pernode for the passes.  The rest of round 109 runs at percpu, with
  t2-r888, r890 and r892 its iodepth-32 base.

## 2. Depth on the new client: iodepth 16 and 64

Is the rate now set by the number in flight?  nfs4k.sh under
settled-iops.sh (percpu), two runs at each of 16 and 64 in turns,
iodepth 32 being t2-r888, r890 and r892.  t2-r894 was stopped in its
load when percpu came into settled-iops.sh (its directory is
`t2-r894-stopped-in-load`; settled.sh's setting put back after):

    cd xeu-rig; ./step.sh /var/tmp/zc-run-logs/r109 depth2 ./settled-iops.sh run \
        ../FLAMEGRAPH/108-4k-randread/nfs4k.sh 895:16 896:64 897:16 898:64

| Run (percpu) | iodepth | in flight | M IOPS (a / b) | completion mean / p99 | server busy (busiest) | client busy (busiest) |
|---|---|---|---|---|---|---|
| t2-r895 | 16 | 768 | 1.62 (0.81 / 0.82) | 0.47 / 1.65 ms | 48% (82%) | 53% (65%) |
| t2-r897 | 16 | 768 | 1.56 (0.78 / 0.78) | 0.49 / 1.66 ms | 45% (79%) | 50% (57%) |
| t2-r888, r890, r892 | 32 | 1,536 | 1.91 to 1.96, median 1.93 | 0.79 / 2.80 to 2.97 ms | 56 to 57% (92 to 97%) | 65 to 66% (72 to 75%) |
| t2-r896 | 64 | 3,072 | **2.21** (1.11 / 1.10) | 1.38 / 4.15 ms | 67% (98%) | 75% (80%) |
| t2-r898 | 64 | 3,072 | 2.19 (1.09 / 1.11) | 1.41 / 4.11 ms | 67% (98%) | 75% (86%) |

- **The rate still climbs with depth on the canary-WIP client, but
  less each time:** 1.59, 1.93 and 2.20 M IOPS at 16, 32 and 64
  (+21%, then +14%), while the mean READ takes 0.48, 0.80 and 1.40 ms.
  On build 469 depth 64 was below 32 (t2-r879), the client full.
- **At 64 the server's busiest CPU is full (98% in both runs)** while
  its mean is 67% and the client's 75%.  That CPU's connections set
  the pace: the next limit is how the work is spread over the server's
  CPUs, item 3 of the plan.
- New kernel warnings 0 in all four; settled.sh's setting after.

## 3. The server's spread: nconnect 12 and 18 at iodepth 64

Mike, 17 November: "run the nconnect 12 and 18 A/B at iodepth 64".  At
nconnect 16 the eight mounts make 128 connections on nfsd's 48 work
CPUs, 2.67 a CPU, and the busiest CPU is full (98%) while the mean is
67%.  nconnect 12 makes 96 (two a CPU), 18 makes 144 (three).  In
turns, with 16 again beside them (t2-r896 and r898 are its first two):

    cd xeu-rig; ./step.sh /var/tmp/zc-run-logs/r109 nconn ./settled-iops.sh run \
        ../FLAMEGRAPH/108-4k-randread/nfs4k.sh 899:64:12 900:64:18 901:64:16 902:64:12 903:64:18 904:64:16

| nconnect (connections) | runs | M IOPS | completion mean / p99 | server busy (busiest) | client busy |
|---|---|---|---|---|---|
| **12 (96)** | t2-r899, r902 | **2.31, 2.31** | 1.32 to 1.33 / 3.92 to 4.23 ms | 69 to 71% (100%) | 77% |
| 16 (128) | t2-r896, r898, r901, r904 | 2.19 to 2.21 | 1.38 to 1.41 / 4.08 to 4.15 ms | 67 to 68% (98 to 99%) | 75 to 76% |
| 18 (144) | t2-r900, r903 | 2.22, 2.21 | 1.38 / 4.11 to 4.15 ms | 68% (96%) | 75 to 76% |

- **nconnect 12 reads 5% more (2.31 against 2.20)**, two connections on
  each work CPU; 18 is no better than 16.  The busiest server CPU is
  still full at 12 (100%).  New kernel warnings 0 in all six.
- nfs-server restarted once in this A/B (21:06:17, into percpu) and
  once after it: a sequence under one settled-iops.sh run switches
  pool_mode on the way in and out only.

## nfsdcld spun all day, with NFSv3 alone in use

`nfsdcld` (the NFSv4 client tracking daemon, started by nfs-server
because nfsd had v4 on: `/proc/fs/nfsd/versions` "+3 +4 +4.1 +4.2")
had 10 h 49 min of CPU in 12 h 29 min, looping on
"cld_pipe_read_msg: pipe read failed: Broken pipe": 2.33 M journal
lines from 19:51 (the journal's oldest) to 21:06:17, about 480 a
second, which is also `systemd-journald`'s 1.6% in t2-r886.  It stopped
at the nfs-server restart of 21:06:17.  Every run from t2-r883 to r898,
both profiles included, had it; t2-r899 on did not.  Its cost to IOPS
did not show (nconnect 16: 2.19 and 2.21 with it, 2.20 and 2.21
without), but it took a CPU nfsd's work could have had, on any of the
64 (its affinity 0-63).  No IOPS run used NFSv4: every mount is
`vers=3` (t2-r903's eight, read on the client), and nfsd had no v4
client.  Mike, 17 November: "turn off NFSv4 and make it so that
nfsdcld never ever runs!"

Done at 21:30 (`settled.sh`, e6f31027d0367): `/etc/nfs.conf.d/zc-nfsv3-only.conf`
(`vers4=n`, `vers4.0` to `vers4.2` too) read at each nfs-server start,
`nfsdcld.service` masked, and the audit holds both ("nfsd NFSv4 off",
"nfsdcld masked, not running").  After apply's restart:
`/proc/fs/nfsd/versions` "+3 -4 -4.0 -4.1 -4.2", no nfsdcld, 512
threads, the settled setting.  A change to settled.sh takes the WRITE
and READ passes (AGENTS.md):

    cd xeu-rig; ./step.sh /var/tmp/zc-run-logs/r109 wpass ./write-check.sh 905
    ./step.sh /var/tmp/zc-run-logs/r109 rpass ./read-check.sh 906

**The WRITE pass: RERUN twice, 74.85 (t2-r905) and 75.16 GB/s
(t2-r907)**, against 77.73 to 77.91 on libaio before (round 107,
t2-r868 to r871).  No drive stepped down, new kernel warnings 0, both
ports even at about 37.5, the drives 9.2 to 9.5 GB/s each at 95 to 96%
busy (9.7 to 9.8 at 98% before), all 32 server sockets carrying 41 to
51 GB each (43 to 53 before), the client 39% busy (48% before).  Both
passes are the first with the client on build 541; the server's only
change is v3 only.  Mike, 17 November: "has to be client side
difference.. I upgraded that client kernel".  So NFSv4 stays off, and
the passes are not a regression of it; the WRITE mark on the build 541
client is Mike's to decide.  The READ pass follows:

    cd xeu-rig; ./step.sh /var/tmp/zc-run-logs/r109 rpass ./read-check.sh 906

**The READ pass: PASS at 80.02 GB/s (t2-r906)**, new kernel warnings 0,
both ports at about 40, the drives 9.9 to 10.1 GB/s each at 100%
(80.39 in t2-r867 on build 469): v3 only and the client's build 541
leave READs where they were.

## The client in the audit

Mike, 17 November: "add the client audit".  `settled.sh`'s client
audit (read only) read the client's sysctls, its ports' qdiscs and
pause frames.  It now also reads, against `CLI_KERNEL` and the rest
beside `CLI_DEV`: the client's kernel (build 541; `EXP_CLI_KERNEL` for
another), its command line (`idle=poll`, `processor.max_cstate=1`,
`intel_idle.max_cstate=0`, `iommu=off`, `selinux=0`), rpciod per-CPU,
xprtiod unbound and strict, `sunrpc.xprt_numa_map` Y,
`nfs.nowait_writable` N (or nfs not loaded), `kernel.io_uring_disabled`
0, both ports at MTU 9000; and at rest only (a run changes them and
puts them back) the workqueue scope cache_shard, irqbalance active and
no `SRV_ALIAS` address left on the server's ports.  `twoport.sh`'s
client line records the client's kernel in every run's geometry.
Nothing on either host is set by it, so the setting the passes ran on
(t2-r905 to r907) is unchanged.

## 4. What the server's busiest CPU does

Mike: "profile the hot CPU".  `prof-hot.sh` at the best of section 3,
iodepth 64 and nconnect 12:

    cd xeu-rig; ./step.sh /var/tmp/zc-run-logs/r109 hot ../FLAMEGRAPH/109-iops/prof-hot.sh 908 64 12

t2-r908: 2.31 M IOPS (1.16 / 1.16), 1.32 / 3.98 ms, server 70% busy
(busiest 93%), client 77%, new kernel warnings 0.

- **No one CPU is hot: nfsd's 48 work CPUs are 86.7% busy on average
  (82 to 93%), two rtc connections on each** (`placed` read 2 on every
  CPU at both ends of the window).  The busiest (48, 92.6%) and the
  median (21, 86.6%) look alike: 92% of the busiest under
  `svc_rtc_work`, the rest idle and softirq.
- **The per-request checks are about 10% of each:** `module_put` 2.5%
  and `try_module_get` 2.0% (the export's and the auth domain's module
  references), `svcauth_unix_set_client` 2.2%, `sunrpc_cache_find_rcu`
  1.8%, `refcount_dec_not_one` 1.4%, beside `_raw_spin_lock` 3.4%.
  That is what David's 18 and 19 (borrowing the export) take away.
- nfsdcld and systemd-journald took no samples.  About a fifth of each
  CPU's samples carry the command `kthreadd`, under the same
  `kthread` and `worker_thread` chain as the kworkers' (96% of the
  CPU's samples are under `kthread`): the rtc kworkers' samples under
  another name, not other work.
- So the server is near its limit across its work CPUs at 2.31 M, and
  the next server step is the per-READ cost, not the spread.

## 5. The client's workqueue scope: cache_shard against numa

Every run so far had the client's workqueue `default_affinity_scope`
at numa (`twoport.sh`'s `CLI_SCOPE`, since 7 November), not build 541's
own cache_shard: xprtiod, still unbound, has one pool a node at numa
and one a cache shard at cache_shard, and its pool lock was most of
the client's 13.4% in spin locks (t2-r887).  Mike, 17 November: "run
the cache_shard A/B".  Three runs a side in turns at iodepth 64 and
nconnect 12, under one settled-iops.sh run:

    cd xeu-rig; ./step.sh /var/tmp/zc-run-logs/r109 scope ./settled-iops.sh run \
        ../FLAMEGRAPH/109-iops/ab-scope.sh 909:cache_shard 910:numa 911:cache_shard 912:numa 913:cache_shard 914:numa
