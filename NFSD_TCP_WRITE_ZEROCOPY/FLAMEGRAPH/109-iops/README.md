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
