# Round 110: the export borrow with several entries a thread

Mike, 17 November: "I think we need to fix this borrowing so that it
isn't so fragile.. we need the ability to have multiple exports and
not have it be the basis for skipping such an important optimization",
then "build the borrow fix and A/B it at 0 vs 16".

## Why

The borrow (`e5df1b4f4106b`, `d7f9f04a9ded7`) kept one export a nfsd
thread or rtc pool rqst.  The rig exports eight filesystems, and a
thread serves whichever comes next, so the slot missed often (debugfs
`nfsd/export_borrow`, the tip's stack on build 541, client build 541):

| Runs | Shape | M IOPS | hits | fills | hit rate |
|---|---|---|---|---|---|
| t2-r915, r916 (WRITE, READ passes) | | | 7.0 M | 21.5 M | 24% |
| t2-r918 | iodepth 32, nconnect 16 | 2.02 | 28.0 M | 26.6 M | 51% |
| t2-r920 | iodepth 64, nconnect 12 | 2.50 | 41.3 M | 25.2 M | 62% |
| t2-r921 | iodepth 64, nconnect 12 | 2.48 | 39.7 M | 26.3 M | 60% |
| t2-r922 | iodepth 64, nconnect 12 | 2.51 | 40.4 M | 26.3 M | 61% |

Every miss dropped the slot's pair and looked the new one up, more
than no slot at all.  Round 109's best on 541's own stack, without the
borrow, was 2.39 M (t2-r909, r911, r913).

## The change

`bbcc6e8d1389d` "NFSD: Cache several exports in each thread's borrow
slot": up to `nfsd.export_borrow_slots` entries a slot (default 16, at
most 64, 0 disables), the least recently used replaced on a miss, a new
`evict` counter for fills that dropped a live entry.  Its suite,
`5420d7f0a7dc7`, adds `ebt_lru_evict`; it is compiled (W=1, against
541) and not run: no KUnit on tardis2.

Built with `build-svcstack.sh` from `5420d7f0a7dc7` into
`/var/tmp/svcstack-541-5420d7f0a7dc7/stack`: nfsd
`9E859F33630085ECFA66EC1`, sunrpc unchanged (`7A2D473DDE10AB1E01D4BB3`).

## The A/B

`ab-borrow.sh <n>:<slots> ...`: the parameter set, then one
`settled-iops.sh run` of `nfs4k.sh <n>` (iodepth 64, nconnect 12,
cache_shard), which restarts nfs-server on its way in so every slot of
the run is made with that value.  In turns, 16 first:

    cd xeu-rig; ../FLAMEGRAPH/110-export-borrow/ab-borrow.sh \
        923:16 924:0 925:16 926:0 927:16 928:0

### Results (17 November, 23:37 to 23:57)

Server build 541 with the stack from `5420d7f0a7dc7` (nfsd
`9E859F33630085ECFA66EC1`), client build 541, `settled-iops.sh`:
iodepth 64, nconnect 12, cache_shard.  Logs in
`/var/tmp/zc-run-logs/r110/`, the borrow counters' change for each run
in `/var/tmp/zc-run-logs/541/ab-borrow.log`.

| Run | slots | M IOPS (ports) | ms mean / p99 | Server busy (busiest) | Client busy (busiest) | hits | fills | evicts | skips |
|---|---|---|---|---|---|---|---|---|---|
| t2-r923 | 16 | 2.48 (1.25 / 1.23) | 1.23 / 4.75 | 72% (93%) | 70% (80%) | 66.06 M | 10,416 | 0 | 0 |
| t2-r924 | 0 | 2.47 (1.23 / 1.24) | 1.25 / 4.88 | 72% (94%) | 70% (78%) | 0 | 0 | 0 | 65.85 M |
| t2-r925 | 16 | 2.47 (1.24 / 1.23) | 1.24 / 4.62 | 72% (94%) | 70% (75%) | 65.77 M | 10,306 | 0 | 0 |
| t2-r926 | 0 | 2.47 (1.22 / 1.25) | 1.25 / 4.82 | 72% (95%) | 70% (80%) | 0 | 0 | 0 | 65.85 M |
| t2-r927 | 16 | 2.47 (1.23 / 1.24) | 1.25 / 4.49 | 72% (99%) | 70% (79%) | 65.82 M | 10,281 | 0 | 0 |
| t2-r928 | 0 | 2.48 (1.25 / 1.23) | 1.22 / 4.55 | 72% (95%) | 70% (77%) | 0 | 0 | 0 | 66.04 M |

ms mean / p99 is port a's.  No new kernel warnings in any run, no
bad_return, 48 drains a run (the restarts' cache flushes).

- **The fix does what it is for: 16 entries hit 99.98% of NFSv3 file
  handles** (about 10,300 fills a run, the first borrow of each of the
  eight exports in each slot, and no evictions), against 60 to 62% with
  one slot a thread (t2-r920 to r922).  0 disables it: every file
  handle is a skip.
- **It changes nothing measurable at this shape: 2.47 to 2.48 M IOPS
  on both sides**, the same latency, 72% of the server and 70% of the
  client.  The one-slot borrow on the same tip read 2.48 to 2.51
  (t2-r920 to r922), within the same spread.  nfsd's busiest work CPU
  is at 93 to 99% on both sides: the limit is elsewhere, and what the
  borrow saves is not on it.  A profile at 16 and at 0 (`prof-borrow.sh`,
  t2-r929 and r930) is to show what that limit is and what a hit costs.

### Profiles at 16 and 0 (18 November, 00:00 to 00:04)

`prof-borrow.sh 929 16`, then `930 0`; reports in
`/var/tmp/zc-run-logs/t2-r929/` and `t2-r930/` (`prof-srv-*.txt`), the
data in `/dev/shm/zc-p110-929.data` and `-930.data`.

**t2-r929's IOPS (1.51 M) are not a result: port b's transmit queue 0
hung.**  At 00:00:08 `xeu 0000:a4:00.0 enp164s0f0: NETDEV WATCHDOG:
CPU: 31: transmit queue 0 timed out 5720 ms`, then `Tx timeout`; at
00:00:12 port a's firmware mailbox timed out (`RM message [a1:00.0]
send timed out ... XSL_MSG_EU_RX_PORT_CNTRS_GET failed, error: 110`).
Just before, perf lowered its rate ("interrupt took too long"), and
sysstat-collect took 10 s of CPU.  The run's per-second sampler
stopped for 11 s (00:00:01 to :12) and the client's READs stalled from
about :06.  Both ports pinged after it, and t2-r930 ran at 2.46 M.  It
is the first transmit timeout in this tree's records, and in the
kernel journal since 1 November.  nfs4k.sh's "new kernel warnings 0"
missed it: its filter matches neither `NETDEV WATCHDOG` nor `Tx
timeout`.  The perf window (23:59:47 to :55) ended before the queue
stuck (from about 00:00:02), with the wire steady through it, so the
profile itself stands.

**What a hit costs, against the lookup it replaces** (share of all
server samples):

| | slots 16 (t2-r929) | slots 0 (t2-r930) |
|---|---|---|
| `fh_verify`, with children | 6.45% | 8.50% |
| `rqst_exp_find` (exp_find_key, exp_get_by_name, cache_check), with children | not called | 3.02% |
| `nfsd_exp_borrow`, self | 0.48% | 0.21% (the skip) |
| `nfsd_exp_ent_match`, self | 0.40% | not called |
| `fh_put`, with children | 0.72% | 0.88% |
| `sunrpc_cache_lookup_rcu`, with children | 1.20% (auth caches) | 3.43% |
| swapper (idle) | 18.34% | 17.98% |

- **A hit costs about 0.9% (borrow and scan) where the lookup cost
  3.0%, and fh_verify drops by 2.05 points.**  The complexity does
  not negate the hit rate, but half of a hit is the scan:
  `nfsd_exp_ent_match()` is an out-of-line call for each entry looked
  at, about 4.5 a hit with eight exports.  Checking the most recently
  used entry first and inlining the match would cut most of it.
- **The saving does not reach IOPS or idle time** (18.3% against
  18.0%, 2.46 to 2.48 M on both sides): the cycles fh_verify no longer
  takes are taken up elsewhere.

**What the server spends at 16** (with children, of all samples; the
48 work CPUs 8-55 evenly loaded, 1.77% of samples each):

- receive and transmit softirqs 23.6% (NAPI 16.3%);
- sending the replies 16.6% (`svc_tcp_send_record` to `tcp_sendmsg`,
  one 4 KiB reply a send);
- receiving the calls: `svc_xprt_recv_call` 12.0%, `svc_tcp_recvfrom`
  10.1%;
- the READ itself 11.7% (`nfsd_rtc_read_file`: XFS direct read 10.8%,
  block submit 5.2%);
- per-call cache lookups: `nfsd_file_lookup_cached` 7.4% (with
  `fh_verify` 6.5% in it, of which `exportfs_decode_fh_raw` 3.7%), and
  `svc_set_client` 4.1% (`svcauth_unix_set_client` 3.7%, 1.8% self:
  the ip_map and unix_gid caches, looked up and referenced on every
  call, the same shape as the export lookup the borrow removed);
- `_raw_spin_lock` 4.0% across callers.

Candidates for the next round, none of them started: the auth caches'
per-call lookup (`svcauth_unix_set_client`) borrowed per thread as the
export now is; the dentry decode under `nfsd_set_fh_dentry`; the reply
send's cost per 4 KiB.

## The entry used last first (18 November)

Mike: "do the MRU-first scan".  `f85de7d8a7d1e` "NFSD: Look at the
export borrowed last first": the slot keeps the index of the entry hit
or filled last and looks at it before scanning; the match is always
inline and compares the fsid type and first word before the rest.
debugfs counts the lookups found there as `mru`.  Its suite case is
`c3493f011b68e` (`ebt_mru_first`), compiled at W=1, not run.  Built
into `/var/tmp/svcstack-541-mru/stack` (nfsd `528EBC791C7D3FAEB133919`,
sunrpc unchanged; `nfsd_exp_ent_match` no longer a symbol).

The borrow fix (`bbcc6e8d1389d`) has had no WRITE or READ pass yet; both
are run on this build, which carries it.  The plan, in order:

    cd xeu-rig; STACK=/var/tmp/svcstack-541-mru/stack SUNRPC_KO=none \
        ./step.sh /var/tmp/zc-run-logs/541 swap-mru ./swap-sunrpc.sh
    ./settled.sh apply
    ./write-check.sh 931; ./read-check.sh 932
    ../FLAMEGRAPH/110-export-borrow/prof-borrow.sh 933 16

### The passes and the profile on it (18 November, 00:13 to 00:28)

Server build 541 with `/var/tmp/svcstack-541-mru/stack` (nfsd
`528EBC791C7D3FAEB133919`, `export_borrow_slots` 16), client build
541.  The borrow counters' change in each, from
`/var/tmp/zc-run-logs/541/mru.log`:

| Run | | GB/s or M IOPS | Result | borrow hits | at the MRU entry |
|---|---|---|---|---|---|
| t2-r931 | WRITE pass | 74.55 | RERUN | 5.09 M | 0.66 M (13%) |
| t2-r932 | READ pass (io_uring, rsize 256 KiB) | 79.22 | **PASS** | 18.42 M | 5.52 M (30%) |
| t2-r933 | IOPS profile at 16 | 2.49 | | 66.49 M | 40.43 M (61%) |
| t2-r934 | WRITE pass again | 74.98 | RERUN | | |

No drive stepped down and no new kernel warning in any (with the wider
filter of `c30fb8f5fdaab`); no evictions, no bad returns.

**The READ pass passes**: 79.22 GB/s, the drives 9.8 to 10.0 GB/s each
at 100% busy, against 79.65 on the tip's stack before the borrow fix
(t2-r916).  Against the frame: 76% of what the drives read with local
fio (104.58 GB/s) and 81% of what the ports send with iperf3 `-Z`
(98.31); the pass measures no memory or Send-Q.

**The WRITE pass is below 75.0 twice, a regression by the rule**
(AGENTS.md: not kept unless Mike decides).  74.55 and 74.98, the drives
9.2 to 9.4 GB/s each at 95 to 96% busy, ring drops 43 to 85, against:

| Server stack (client build 541) | WRITE passes |
|---|---|
| build 541's own | 74.85, 75.16 (t2-r905, r907, round 109) |
| the tip, one-slot borrow | 74.91, 75.25 (t2-r915, r917) |
| the tip, 16 entries and MRU-first | 74.55, 74.98 (t2-r931, r934) |

About 0.3 GB/s lower on the mean, inside the spread of the others; two
runs do not tell a cost of the borrow from the spread.  To tell: WRITE
passes on this build at `export_borrow_slots` 16 and 0 in turns, the
same modules and boot.  Mike's decision.

**What a hit costs now** (t2-r933 against t2-r929, share of all server
samples):

| | t2-r929 (scan from the first) | t2-r933 (MRU first, inline match) |
|---|---|---|
| `nfsd_exp_borrow`, self | 0.48% | 0.69% (the match inlined in it) |
| `nfsd_exp_ent_match`, self | 0.40% | inlined |
| a hit in all | 0.88% | **0.69%** |
| `fh_verify`, with children | 6.45% | 6.40% |
| swapper (idle) | 18.34% | 18.61% |

A hit costs about a fifth less.  61% of the IOPS run's hits were the
entry used last, 30% of the READ pass's, 13% of the WRITE pass's: the
WRITE pass's 16 jobs a port at iodepth 16 spread more exports over each
thread.  IOPS are unchanged (2.49 M against 2.47 to 2.48), as the A/B
said: the limit is not in the lookup.
