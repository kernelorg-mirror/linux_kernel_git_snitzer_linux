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
