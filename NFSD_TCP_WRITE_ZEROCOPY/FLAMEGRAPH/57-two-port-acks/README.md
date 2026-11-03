# Round 57: the ACKs nfsd's reads send, at tardis2's two-port setting

**The baseline (Mike, 3 November):** four loan runs, latency and a profile
at the setting round 56 settled: both ports, 12 connections a port, a
512 KiB held buffer, rings of 128, budget 128 WRITEs, region 32768 pages,
`rx_region_stack` 64, `svc_tcp_rx_want_eighths` 6. Build 506
(`7.3.0-10.hs.506.loanpages_pool.el10.aarch64+16k`), its own nfsd
(`3F0C341E86E266D5FDF2ADF`) and sunrpc (`5DDCC44639FC73AF533513F`), xeu
`CA0931345E46C929B11D01F`. Eight DapuStor SGL drives, four a port. The
drivers are round 56's [run165.sh](../56-eight-sgl-two-ports/run165.sh) and
[run166.sh](../56-eight-sgl-two-ports/run166.sh), the runs
`/var/tmp/zc-run-logs/t2-r165/` and `t2-r166/`. Copy mode was not run.

Figures from before tardis2's four added drives and second port (rounds 55
and earlier) are not comparable.

## Loan runs

| run | GB/s, both ports | rings / nfsd cycles a WRITE | nfsd instructions a WRITE | in use at the middle | bytes through memory a byte | ring drops a / b |
|---|---|---|---|---|---|---|
| r165-1 | 70.19 | 144.6k / 279.0k | 273.3k | 52 MiB | 1.39 | 92 / 149 |
| r165-2 | 64.83 | 145.0k / 269.1k | 273.9k | 43 MiB | 1.22 | 833 / 7 |
| r165-3 | 67.24 | 144.4k / 271.9k | 273.8k | 53 MiB | 1.27 | 57 / 17 |
| r165-4 | 67.41 | 144.2k / 272.2k | 273.2k | 49 MiB | 1.28 | 24 / 56 |
| **mean** | **67.42** | **144.6k / 273.1k** | **273.6k** | **49 MiB** | **1.29** | |

- Every run untraced and settled, no drive stepped down, no stall, no FIFO
  drop, `alloc_fail` 0. A WRITE takes 4.12 NVMe interrupts and about 4.3
  IPIs.
- The rate is the sum of the two ports. Runs differ by up to 5 GB/s; one
  run's rate is not to be leaned on.
- nfsd's cycles and instructions a WRITE are below round 50's one-port
  290.2k and 300.1k. Why was not looked at.

## Latency

One port, iodepth 1, untraced (`lat.sh`):

| run | GB/s | p50 | p90 | p99 | p99.9 | mean | max, ms |
|---|---|---|---|---|---|---|---|
| l1 | 8.45 | 0.424 | 0.469 | 0.545 | 0.668 | 0.431 | 0.855 |
| l2 | 8.07 | 0.432 | 0.510 | 0.578 | 0.651 | 0.444 | 0.768 |
| round 50 | 7.72, 8.24 | 0.453, 0.436 | 0.537, 0.481 | 0.594, 0.553 | | 0.469, 0.443 | |

Within round 50's range.

## The profile (fl56)

`WHICH=ab ./flame.sh`, both ports, 68,358 WRITEs a second in the profile
([cost.txt](cost.txt), [top-nfsd.txt](top-nfsd.txt),
[top-rings.txt](top-rings.txt)). The profiled run carried 71.68 GB/s in all:
36.43 on port a and 35.26 on port b, 1.5 above the best untraced run, inside
the runs' spread. The microseconds a WRITE below do not depend on the rate.

- **Rings busy 34.4 µs a WRITE, nfsd 98.5.**
- nfsd's time, inclusive, µs a WRITE:

| frame | µs | |
|---|---|---|
| `svc_tcp_recvfrom` | 32.8 | the read |
| of it `tcp_cleanup_rbuf` → `tcp_send_ack` | **9.1** | the ACKs the read sends |
| of it `svc_tcp_recv_actor` | 11.0 | the loan's walk, borrow and append |
| `nfsd_direct_write` | 20.4 | XFS, iomap, the block layer and NVMe (`nvme_prep_rq` 5.8) |
| `svc_tcp_sendto` | 13.9 | the reply 7.2, the release of the loan 5.8 |
| of the release, `page_pool_recycle_ring_bulk` | 3.0 | the pool's producer lock with interrupts off (round 45) |
| `svc_recv`'s sleep and wakes | about 5 | `svc_thread_wait_for_work` 2.4, `svc_pool_wake_idle_thread` 2.2 |
| `fh_verify` | 3.3 | round 54, dropped |
| `xfs_log_force_seq` | 2.7 | |
| `nfsd_cache_lookup` | 2.3 | the v3 reply cache, kept for flexfiles (round 12) |

- The largest self frames on nfsd: `_raw_spin_unlock_irqrestore` 5.5 (the
  pool's producer lock 2.1, thread wakes 1.6, NVMe SGL setup 0.5) and
  `finish_task_switch` 3.2 (the idle sleep 1.7, waiting for the drive 0.8).

**TCP sends about 7.05 segments a WRITE** (the server's `OutSegs` over 1.8 s
of run r165-1, about 120,500 WRITEs): one reply and about six ACKs. At one
port and 384 KiB, round 25 counted 5.58 at want_eighths 6. So each ACK costs
nfsd about 1.5 µs.

## Where a page spends its time (run166)

`pagelife.bt` for 10 s in one run on both ports at the settled setting
([pagelife.txt](pagelife.txt), [pagelife-read.txt](pagelife-read.txt)). The
trace slowed the run to 57.37 GB/s. Pages held are Little's law over one
page in eight, times eight:

| stage | pages held | mean | median |
|---|---|---|---|
| 1 ring and socket | 585 | 262 µs | 128 to 256 |
| 2 record, first borrow to the WRITE | 726 | 352 µs | 128 to 256 |
| 3 drive | 341 | 165 µs | 128 to 256 |
| 4 to the release | 47 | 23 µs | 16 to 32 |
| 5a to repost, the other buffer held at the put | 179 | 1,607 µs | 256 to 512 |
| 5b to repost, page free at the put | 751 | 385 µs | 64 to 128 |
| of 5b, in the pool's stack (70% of 5b's cycles) | 180 | 133 µs | 64 to 128 |
| of 5b, free in the region (15%) | 377 | 1,301 µs | 128 to 256 |
| of 5b, in the alloc cache | 45 | 27 µs | 16 to 32 |
| 2 to 5, no put seen | 159 | 906 µs | |
| 0 not loaned | 86 | 956 µs | 512 to 1024 |
| **all** | **about 3,620** | | |

- The receive side (stages 1 and 2) holds 36% of the pages in use, the way
  back (5a and 5b) 26%, the drive 9%.
- The pages that go back into the region are 15% of those free at their put
  but hold half of 5b: about 1.3 ms each on the region's LIFO free lists
  before the pool takes them again. Changing that is page_pool, in the
  kernel.

## The target: the ACKs nfsd's reads send

9.1 µs a WRITE, 9% of nfsd, about six ACKs a WRITE: the largest thing on
nfsd that sunrpc controls. The rest of the top of the profile is the kernel's
(the write below nfsd, round 55's SGL walk; page_pool's producer lock),
already worked (the loan's walk, borrow and release in rounds 22 to 24, 36,
52 and 53; the wakes in rounds 17, 18 and 40) or kept on purpose (the reply
cache).

Round 25 left two things about these ACKs open: want_eighths 6 at two ports,
not measured; and the short pass at a record's start that loan credit refuses
(about 0.57 ACKs a WRITE then). The refusal itself is not to be changed
(round 26, Mike), only what its pass costs.

**First, the trace** ([run167.sh](run167.sh)): one loan run on both ports at
the settled setting with round 25's [acks.bt](../25-later-wake/acks.bt) for
10 s inside the window, then its [small.bt](../25-later-wake/small.bt) for
10 s, and the server's TCP counters around the first. It says which rule sends
each ACK, after which read, and how many passes a record takes, against round
25's 4.8 ACKs and about four passes a WRITE. Needs no rebuild and no reload;
the A/B after it decides whether a change is sunrpc's.
