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
- **The drives set the rate.** In `t2-r477` each of the eight wrote 9.0
  to 9.1 GB/s at 92 to 93% busy, about 72 GB/s together, the run's rate;
  nfsd's CPUs were 15% busy.  A WRITE's payload waits in the region for
  its drive.
- **The last look at the L3 was round 57** (build 514, 3 to 4 November,
  the settled two-port setting): 49 MiB of the region in use in the
  middle of a run (`pages - free`, which counts CPU caches and device
  slots as in use, so an overstatement) and 1.29 bytes through memory
  for each byte of payload, against about 0.1 if the L3 held the
  payload until the drive read it and about 2 if none of it stayed.
- At rest (rings of 256) the region holds 1,152 pages, 18 MiB, mostly
  the rings' posted buffers.  At 72 GB/s the L3 holds about 0.47 ms of
  payload.

## The runs (planned)

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
