# Round 43: the receive region's size

**The ask (Mike, 29 October):** the results from running the region pool at different overall sizes, such as 32 MB, 64 MB and 100 MB.

**What there was before:** none at the current setting. The region has been 32768 pages (512 MiB at 16 KiB pages) since the bring-up. The other sizes in the record are from other boots and are not comparable: 2048 pages on the 64 KiB kernel (128 MiB, 7 October) and 6144 pages (96 MiB) on build 459 (8 October), in copy mode with other drives and builds. Loan64's peak in use is about 6350 pages (99 MiB, run102 and run103), and its budget is 8198 pages (128 MiB), so 32 and 64 MiB regions sit below what loan64 uses today.

## The sweep (r104)

[run104.sh](run104.sh): the region at 32768 pages (512 MiB), 6400 (100 MiB), 4096 (64 MiB) and 2048 (32 MiB), then 32768 again. At each size there is one copy run and one loan run whose budget fits the region with 1024 pages (16 MiB) left for the rings and the copy.

| region | pages | loan run | budget, pages |
|---|---|---|---|
| 512 MiB | 32768 | 64 WRITEs (loan64) | 8198 |
| 100 MiB | 6400 | 41 | 5300 |
| 64 MiB | 4096 | 23 | 3032 |
| 32 MiB | 2048 | 7 | 1016 |

All runs use round 41's kept xeu at the settled setting except for the region's size, and are counted and untraced (WARM=10 RUNTIME=30). Each line reports:

- the rate;
- ring and nfsd cycles a WRITE;
- the region's pages in use at most, against its size;
- allocations the region failed (the ring finding it empty);
- loans admitted and denied;
- the client's retransmits.

**The resize** follows page_pool's comment:

1. Write the new `rx_region_pages`.
2. Take port a down, so its pools go and the old region is given back.
3. Bring the port up, which makes the new region.

`settled.sh` holds the size to `EXP_REGION_PAGES` for the run, and now also checks the size of the region actually made (debugfs). Each resize:

- refuses to run with an NFS client connected;
- waits for the old region to go before the port comes up;
- stops if the new one is not made.

This is the first time the resize runs on the rig: "page_pool: Give the fixed receive region back when it is resized" was queued on 9 October when tardis1 crashed, and never run after. If 32768 contiguous pages cannot be had again at the end, the script stops at the size it could make, and a reboot puts the region back.

### What run104 found

[run104.out](run104.out). The first attempt was stopped at 05:50 with the region at 6400 pages ([run104-stopped.out](run104-stopped.out)); round 42's run105 put it back to 32768, and run104 was run again whole from 16:36.

- Every run placed its flows, no drive stepped down and no WARNING appeared.
- Each resize gave the old region back with 0 pages escaped.
- 32768 contiguous pages were had again at the end, so the resize works on the rig without a reboot.
- At the end the region was 32768 pages, with round 41's kept xeu and the settled setting.

Pages are 16 KiB: 64 pages are 1 MiB.

**Copy mode:**

| region | GB/s | peak in use, pages / MiB | ring alloc fails | ring cycles a WRITE | nfsd cycles a WRITE |
|---|---|---|---|---|---|
| 512 MiB (32768) | 40.80 | 1248 / 19.5 MiB | 0 | 147.1k | 667.5k |
| 100 MiB (6400) | 40.80 | 1200 / 18.8 MiB | 0 | 147.5k | 680.3k |
| 64 MiB (4096) | 40.80 | 1184 / 18.5 MiB | 0 | 146.4k | 672.5k |
| 32 MiB (2048) | 40.81 | 1168 / 18.3 MiB | 0 | 148.6k | 670.0k |
| 512 MiB again | 40.80 | 1184 / 18.5 MiB | 0 | 146.9k | 675.8k |

**Loan mode,** each with the budget that fits its region with 1024 pages (16 MiB) left:

| region | budget, pages / MiB (WRITEs) | GB/s | peak in use, pages / MiB | ring alloc fails | ring cycles a WRITE | nfsd cycles a WRITE | WRITEs/s | admitted | paused |
|---|---|---|---|---|---|---|---|---|---|
| 512 MiB | 8198 / 128.1 MiB (64) | 40.50 | 6306 / 98.5 MiB | 0 | 140.0k | 296.2k | 38.7k | 48.7% | 72 |
| 100 MiB | 5300 / 82.8 MiB (41) | 39.85 | 4779 / 74.7 MiB | 0 | 137.7k | 287.6k | 38.0k | 48.7% | 19 |
| 64 MiB | 3032 / 47.4 MiB (23) | 37.89 | 3436 / 53.7 MiB | 0 | 136.3k | 269.5k | 36.0k | 48.8% | 31 |
| 32 MiB | 1016 / 15.9 MiB (7) | 27.67 | **2048 / 32.0 MiB, all of it** | **3352** | 136.5k | 231.7k | 26.5k | 49.5% | 2 |
| 512 MiB again | 8198 / 128.1 MiB (64) | 40.49 | 6327 / 98.9 MiB | 0 | 139.7k | 296.2k | 38.6k | 48.8% | 73 |

("admitted" is the share of WRITEs the budget loaned, over the run; the rest are copied. "paused" is the budget's pauses over the run.)

- **Copy mode does not care about the region's size, down to 32 MiB.** Its peak is 18–20 MiB at every size, and its rate and cycles do not move.
- **Loan mode loses rate as its budget shrinks:** 1.6% at 100 MiB, 6.4% at 64 MiB and 32% at 32 MiB. The two runs at 512 MiB, first and last, agree to 0.01 GB/s and 21 pages, so the steps are not drift.
- **At 32 MiB the region ran out.** The budget left 1032 pages for the rings and the copied WRITEs, and copy mode alone needs 1168–1248. All 2048 pages were in use, and the ring failed 3352 allocations, each waiting for the refill timer. The 16 MiB margin is too small there. run109 ran it again with a budget of 720 pages, and that did not fit either (below).
- **Cycles a WRITE fall with the budget, nfsd's most (296k to 232k), while the rate falls too.** So the smaller regions are not short of CPU. The likeliest limit is the number of WRITEs the budget lets hold loans at once, at the same 49% admitted. This is a reading, not shown.
- **A loaned WRITE holds about 98 pages at peak, not 64:** 6306 pages at 512 MiB with up to 64 WRITEs on loan, beside the copied ones. twoport.sh charges 126 a WRITE.

## 32 MiB with a smaller budget (r109)

[run109.sh](run109.sh): the region at 2048 pages, one loan run with the budget at 720 pages (`BUDGET_PAGES`, new in twoport.sh, with the reserve that `WRITES=5` gives), then 32768 again and the settled setting. 720 is below the 800–880 pages that copy mode's peak leaves, with a margin for its scatter.

[run109.out](run109.out), at 19:06 to 19:11 on 29 October, after round 44:

| region | budget, pages / MiB (WRITEs) | GB/s | peak in use, pages / MiB | ring alloc fails | ring cycles a WRITE | nfsd cycles a WRITE | WRITEs/s | admitted | paused |
|---|---|---|---|---|---|---|---|---|---|
| 32 MiB (r104) | 1016 / 15.9 MiB (7) | 27.67 | 2048 / 32.0 MiB, all of it | 3352 | 136.5k | 231.7k | 26.5k | 49.5% | 2 |
| 32 MiB (r109) | 720 / 11.3 MiB (5) | **21.84** | **2048 / 32.0 MiB, all of it** | 6 | 137.1k | 227.3k | 20.8k | 49.6% | 0 |

- **720 pages does not fit.** The estimate behind it added the budget to copy mode's peak (about 1170 + 720 = 1890 pages). The region still filled, so under loans something besides the loaned WRITEs holds pages that copy mode does not: the likeliest are records waiting in the sockets' receive queues before nfsd takes them. This is a reading, not shown. The run records only the region's lowest point, not what held it.
- **The rate was flat and low the whole run, not in stretches.** The four drives carried 21.2 to 22.2 GB/s every second at 50 to 65% busy. The wire carried 22.05 GB/s with no FIFO or ring drops and 4 retransmits. The rings were 5% busy and nfsd 3%. The server was waiting, not working.
- **A smaller budget gave a lower rate and almost no allocation failures** (6 against 3352). These figures do not explain that.

Loan mode does not fit a 32 MiB region at a budget of 5 or 7 WRITEs, while copy mode runs there at 1170 pages. A budget that fits would first need a count of what holds the region's pages under loans.

