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
