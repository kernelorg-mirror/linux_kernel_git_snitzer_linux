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
