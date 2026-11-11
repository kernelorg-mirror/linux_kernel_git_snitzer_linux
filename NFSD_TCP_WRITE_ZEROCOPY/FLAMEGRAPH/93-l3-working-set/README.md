# Round 93: the region's working set against the L3 on tardis2

**The ask (Mike, 11 November):** "focus on what the actual L3 size usage
is relative to 'The pages that have to stay in the L3 are the ones the
rings and nfsd are touching at the moment'. Is it staying bounded and
nfsd/drives keeping up to stay reasonably close to L3 containing that
bounded working set?"  Then: "yes, run the six-run L3 measurement".

## What is known before it

- **The peak is not the working set.** Round 92's region peak, 11,757
  pages at `rx_refill_mid` 16 (about 184 MiB, 5.7 times the 32 MiB L3),
  is a burst's maximum, under the loan budget's 16,262 pages (128
  WRITEs, about 254 MiB).  The region was back to 1,292 pages held at
  the end of `t2-r477`.
- **The drives are not the limit.** In `t2-r477` each of the eight wrote
  9.0 to 9.1 GB/s at 92 to 93% busy, about 72 GB/s together, the run's
  rate; but busy is the share of time a drive has an I/O outstanding,
  not of what it can take, and all eight take 81.6 GB/s from local fio
  with no network (round 56).  nfsd's CPUs were 15% busy.  What holds
  the rate at 72 is not shown here; round 56 found it follows the
  connections.
- **The last look at the L3 was round 57** (build 514, 3 to 4 November,
  the settled two-port setting): 49 MiB of the region in use in the
  middle of a run (`pages - free`, which counts CPU caches and device
  slots as in use, so an overstatement) and 1.29 bytes through memory
  for each byte of payload, against about 0.1 if the L3 held the
  payload until the drive read it and about 2 if none of it stayed.
- At rest (rings of 256) the region holds 1,152 pages, 18 MiB, mostly
  the rings' posted buffers.  At 72 GB/s the L3 holds about 0.47 ms of
  payload.

## The runs

[run479.sh](run479.sh), six two-port runs on the settled setting,
`rx_refill_mid` 16 (as `EXP_XEU_rx_refill_mid`), from `t2-r479`,
`WARM=10 RUNTIME=40`, the held mark on, no perf on the cores and no
trace.  Beside each:

- `REGION_SAMPLE=1`: `regionsample.py` samples the pages held every
  20 ms on CPU 63 (a page held is on no shard, CPU cache or slot; the
  rings' posted buffers count).
- Over twoport's 10 s window, the mesh: `arm_cmn/hnf_mc_reqs/` (requests
  to the memory controller, a 64-byte line each) and
  `arm_cmn/hnf_cache_fill/`.

[l3read.py](l3read.py) gives, over the window: the pages held (median,
90th and 99th percentile, most) and the median as a share of the L3; a
held page's time at the median (the median held over the payload's
rate); the bytes through memory a byte of payload and the fills a
payload line.

**All six at 16 (Mike, 11 November: "16 is the settled setting. Seems
like mid 0 is a waste of time"):** the runner's default was 16, 0, 0,
16, 16, 0; it was stopped after `t2-r479` (16), before the first run at
0, and started again with `N0=480 ARMS="16 16 16 16 16"`.

## What it found (t2-r479 to r484)

[run479.out](run479.out).  Every run at 0 ring drops, flows placed
12/12, no drive stepped down, no stall, no new kernel warning; the host
was left at the settled setting (`rx_refill_mid` 16).

| | mean | range over the six |
|---|---|---|
| Rate (fio), GB/s | 71.66 | 71.15–72.06 |
| Pages held, median | 3,595 (56 MiB, **1.76 L3**) | 3,567–3,617 |
| 90th percentile | 4,112 (64 MiB, 2.0 L3) | 4,030–4,173 |
| 99th percentile | 6,910 (108 MiB, 3.4 L3) | 6,154–7,324 |
| Most sampled | 8,359 | 6,961–10,109 |
| The kernel's exact peak | 11,436 | 8,707–12,641 |
| A page held, at the median | 0.82 ms | 0.81–0.82 |
| To memory, GB/s | 120.9 | 118.2–123.1 |
| Bytes through memory a byte of payload | **1.69** | 1.66–1.71 |
| L3 fills a payload line | 1.13 | 1.12–1.13 |
| Loans refused (of about 3.4 M admitted) | 0 to 687 a run | |

- **Bounded, and steady.**  The median is the same in every run to 1.4%
  (3,567 to 3,617 pages), the 90th percentile to 3.5%.  The region is
  at 64 MiB or under 90% of the time, at 108 MiB or under 99%; the
  peak, 8,707 to 12,641, is bursts, which touch the budget (16,262
  pages) at times (up to 687 refusals a run).  The drives write 8.8 to
  9.1 GB/s each, under the 10 or so each takes, and keep up: nothing
  grows over a run.
- **Not inside the L3.**  The working set at the median is 1.76 times
  the 32 MiB L3, and a page is held 0.82 ms at the median against the
  0.47 ms of payload the L3 holds at this rate, with nothing else in it.
- **So most of the payload goes through DRAM.**  1.69 bytes through
  memory a byte, against about 0.1 if the L3 held the payload until the
  drive read it and about 2 if none of it stayed: about 120 GB/s of the
  DIMMs' 166.  The fills, 1.13 a payload line, say the card's writes are
  filled into the L3 (one a line); with each line evicted written back
  and read again by its drive (two requests), about four lines in five
  leave the L3 before the drive reads them.  Not measured: how much of
  the L3 the rest of the server (nfsd, XFS, the rings' own state) takes
  beside the payload.
- **Against round 57** (build 514, 67.4 GB/s): 1.29 bytes a byte then,
  1.69 now, at 4.3 GB/s more.  Round 57's 49 MiB in use was `pages -
  free`, which counts more than the held count here, so the working set
  has grown since as well.  Which change since (the rate, stack 96, the
  mid-poll refill, the held mark's own cost) moved it is not separated.
- **Where the held pages sit is not measured here.**  Round 56 put most
  of them on the receive side (a record's transfer on its connection,
  the socket, the ring) and about a tenth at the drive.  The rings'
  posted buffers are 8 MiB of the 56.
