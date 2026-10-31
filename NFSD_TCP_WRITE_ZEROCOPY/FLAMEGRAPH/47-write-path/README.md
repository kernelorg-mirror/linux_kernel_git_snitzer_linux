# Round 47: the write below nfsd

**The ask (Mike, 31 October):** take up the write path below nfsd, the largest path no round has taken up (round 46: 19.1 us of nfsd's 94.1 a WRITE through XFS, iomap, the block layer and NVMe).

tardis2, build 505, one-port loan64 at the settled setting, sunrpc `666175BED7A343E376C1742`. Copy was not run.

## What a WRITE does below nfsd (run120, [write.bt](write.bt), [r120/](r120/))

From round 46's profile, the tree under `xfs_file_write_iter`, with callees, us a WRITE: `file_update_time_flags` 3.16 (an XFS transaction, `xfs_trans_commit` 1.78); `iomap_dio_rw` 15.22, of it `blk_mq_submit_bio` 8.67 (`nvme_map_data` about 5, `bio_split_rw` 1.85, `__blk_mq_alloc_requests` 0.72), `blk_io_schedule` 1.42, the iomap lookup 1.12.

Counted, one loan64 run, all probes in one 5 s window:

| a WRITE | |
|---|---|
| `blk_mq_submit_bio`, `blk_mq_start_request` | 4.07; 256 KiB and 32 segments of 8 KiB each. `max_hw_sectors_kb` is 256 (MDTS 6), so the drive sets this |
| `nvme_queue_rqs` | 4.00: each request flushes the plug on its own, `blk_add_rq_to_plug()` flushing at each of 128 KiB or more |
| `nvme_irq` | 4.07, all on nfsd's CPUs (a queue a CPU) |
| `xfs_vn_update_time`, `__xfs_trans_commit` | **0.75**, 29k a second: 16 files at CONFIG_HZ=100 would allow 1600 |
| hard interrupts on nfsd's CPUs, timed | 8.2, 20.6 us: `nvme` 4.07 and 13.05 us (3.2 each), IPIs 3.94 and 6.61 us (1.7 each, the wakes), the timer 0.12 and 0.91 us |

**Hard interrupts are the largest thing below nfsd and perf cannot show them:** without pseudo-NMI a sample taken in one lands after it. The times are with `write.bt`'s own probes inside the NVMe path, so they read high.

**Why the timestamps.** XFS is `FS_MGTIME`. `xfs_vn_getattr()` calls `fill_mg_cmtime()`, which marks the ctime queried; each WRITE's post-op attributes do that, and the next WRITE's stamp must then differ, which fits 0.75 a WRITE.

How the counts were got right: `interval:s:3` fires every 3 s and bpftrace detaches probes one at a time, so the first two tries had unequal windows; `write.bt` now latches its window, and every per-WRITE figure here comes from a run in which `write` counts 5 s of WRITEs exactly (193k).

## `queue/nomerges` 2: no gain (run121)

With nomerges set `blk_add_rq_to_plug()` does not flush at 128 KiB, and these requests never merge. A B B A B A, untraced, [run121.sh](run121.sh), [run121.out](run121.out).

| | GB/s | ring cycles (instructions) a WRITE | nfsd cycles (instructions) a WRITE | peak | mean in use |
|---|---|---|---|---|---|
| A, 0 | 40.43 to 40.51 | 128.5k (283.4k) | 292.1k (301.6k), 291.7 to 292.4 | 5916 to 6162 | 4861 |
| B, 2 | 40.41 to 40.53 | 128.7k (283.4k) | 291.6k (300.9k), 291.2 to 292.1 | 5864 to 6223 | 4902 |

The change works and buys nothing: counted (r120-3), `nvme_queue_rqs` falls from 4.00 to 1.00 a WRITE, but `nvme_irq` stays at 4.07 and its time at 13.1 us. The drive completes the four apart, so a shared doorbell does not share an interrupt. **Not kept.**

## `lazytime`: kept (run122)

A B B A B A, untraced, [run122.sh](run122.sh), [run122.out](run122.out).

| | GB/s | ring cycles (instructions) a WRITE | nfsd cycles (instructions) a WRITE | peak | mean in use |
|---|---|---|---|---|---|
| A, not lazytime | 40.14 to 40.52 | 128.5k (283.2k) | 292.0k (301.1k), 292.0 to 292.1 | 5931 to 6355 | 4769 |
| B, lazytime | 40.50 to 40.53 | 128.6k (283.4k) | **287.8k (300.4k)**, 287.7 to 287.9 | 5866 to 6246 | 4931 |

**nfsd −4.3k cycles a WRITE (−1.5%), the two arms well apart; the rings and the peak the same.** A's 40.14 GB/s (run 6) had every drive at 562 GB, no step-down and no stall, and its cycles match; it and its mean in use of 4393 are a dip in the window. Without it A's mean is 4957. The latency at iodepth 1 was not run.

**Kept (Mike, 31 October):** `settled.sh` holds `LAZYTIME=lazytime`, remounts each mounted data drive in `apply` and audits it; `EXP_LAZYTIME=nolazytime` holds a run without it. What it trades: a timestamp not yet logged can be lost in a crash, the data cannot; a COMMIT's fsync logs it. Not yet weighed for Hammerspace's product.

**Why cycles and not instructions** (r120-4 counted, [fl47/](fl47/) the profile, [run123.sh](run123.sh) `instructions` sampled, one run a side):

| a WRITE | not lazytime | lazytime |
|---|---|---|
| `__xfs_trans_commit` calls | 0.75 | 0.12, nearly all `xfs_vn_sync_lazytime` at a COMMIT (0.11) |
| `xfs_vn_update_time`, us | 3.00 | 0.29 |
| `xfs_vn_update_time`, k instructions | 2.28 | 0.25 |
| `nfsd3_proc_commit`, us | 2.86 | 3.71 |
| `nfsd3_proc_commit`, k instructions | 1.62 | 2.16 |

- **The transactions were few instructions and many cycles:** about 2.3k instructions for about 3 us, near 9k cycles at the profile's rate, an IPC of about 0.25 against about 1 for nfsd as a whole. So the saving is in cycles: the first hypothesis, stalls on the inode's and the log's shared lines, is what fits. Which lines is not measured.
- **The work did not move to the COMMIT:** only 0.5k instructions came back there.
- **What is left is small and at the sleep:** a net saving of about 1.5k instructions is reduced by about 1.2k more in `finish_task_switch` and 0.4k in idle entry. Those are from one run a side; total sampled instructions are the same in both (245.6k and 245.5k).

## NVMe Interrupt Coalescing: not kept (run124)

Feature 8 at `0x0103`, 4 completions or 100 us, not saved on the drive; off is 0. [run124.sh](run124.sh), [run124.out](run124.out). At saturation, with `lazytime`, A B B A B A untraced:

| | GB/s | ring cycles (instructions) a WRITE | nfsd cycles (instructions) a WRITE | peak | mean in use |
|---|---|---|---|---|---|
| A, off | 40.28 to 40.46 | 128.9k (283.6k) | 287.8k (300.7k), 286.8 to 288.4 | 5878 to 6177 | 4699 |
| B, `0x0103` | 40.45 to 40.57 | 128.3k (283.4k) | **281.1k (294.8k)**, 280.8 to 281.2 | 5944 to 6199 | 4991 |

nfsd −6.7k cycles (−2.3%), −5.9k instructions. Counted (r124-7): `nvme_irq` 4.08 to 1.12 a WRITE, its time 13.2 to 8.9 us, IPIs the same.

**At iodepth 1 the drive holds completions, far past what was set** (`lat.sh`, O A B B A O, O being `lazytime` off; then 64 KiB, A B B A):

| | run | GB/s | p50, ms | p99, ms | mean, ms |
|---|---|---|---|---|---|
| 1 MiB, off | 2, 5 | 8.27, 8.47 | 0.424, 0.424 | 0.553, 0.553 | 0.436, 0.431 |
| 1 MiB, `0x0103` | 3, 4 | 6.63, 6.72 | **0.561, 0.561** | 0.676, 0.684 | 0.567, 0.563 |
| 64 KiB, off | 1, 4 | 2.40, 2.45 | 0.099, 0.098 | 0.130, 0.121 | 0.102, 0.101 |
| 64 KiB, `0x0103` | 2, 3 | 0.68, 0.68 | **0.379, 0.379** | 0.387, 0.391 | 0.379, 0.379 |

- **+137 us at 1 MiB (32%), +280 us at 64 KiB (3.8 times), the rate at iodepth 1 down 20% and 72%.** `lat.bt` puts all of it in the write: `nfsd_vfs_write` moves from the 128 to 256 us bucket to 256 to 512; wakes, queueing and the reply stay.
- **The drive does not do what the setting says:** a lone completion waits about 280 us, not 100, and a WRITE's four requests do not end the wait when the fourth arrives. 100 us is the shortest time there is (TIME is in units of it, and 0 is no aggregation), so a setting with a smaller hold is not available. Only this one value was tried.
- **Not kept (Mike's condition: keep it if the latency holds; it does not).** `settled.sh` is unchanged and all four drives are back at 0.

**`lazytime`'s latency, the check owed from run122:** lazytime (runs 2, 5) against not (1, 6): p50 0.424 against 0.428, p99 0.553 against 0.578 and 0.569, mean 0.436 and 0.431 against 0.441 and 0.442. A little better; it holds on both yardsticks.

## Where it stands

Kept: `lazytime`, nfsd −4.3k cycles a WRITE (−1.5%) and a little less latency. Not kept: `nomerges` 2 and interrupt coalescing.

The NVMe hard interrupts are still there, about 13 us a WRITE as timed, one a request. The drive's coalescing cannot take them without the latency, so anything further needs kernel or firmware work, and none has been tried. What is left of `nvme_map_data`, about 5 us, follows from 128 pieces of 8 KiB a WRITE, and `bio_split_rw`, 1.85 us, from the drive's 256 KiB limit.
