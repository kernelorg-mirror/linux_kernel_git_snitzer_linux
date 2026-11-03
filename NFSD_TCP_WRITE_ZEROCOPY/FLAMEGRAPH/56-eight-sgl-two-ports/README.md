# Round 56: eight SGL drives over both ports

tardis2, build 506, 3 November. All eight data drives are DPRP5s with SGLs, four a port, and the region is on both xeu devices. Loan mode, 1 MiB WRITEs, 128 WRITEs of budget (round 50's 64 for each four drives), `WARM=10 RUNTIME=30`, after an fstrim and 120 s of rest. `settled.sh` is not changed by any of this. Round 55 waits (Mike).

Logs: `$L/t2-r143` to `t2-r146`.

| held buffer, rings | GB/s | window | client held by the window | drops a / b | client retransmits | region peak |
|---|---|---|---|---|---|---|
| 384 KiB, 128 (tardis1's two-port setting) | 61.93, 60.95 | 178 KiB | 98.6% | ring 42 / 30 | 75 | 9,881 |
| 448 KiB, 128 | 52.69 | 202 KiB | 96.2% | ring 24 / 21 | 49 | 6,745 |
| 512 KiB, 128 | 70.75, 69.39 | 244 KiB | 73.3% | ring 5,993 / 5,814 | 12,458 | 12,648 |
| 640 KiB, 128 | 67.58 | 305 KiB | 56.3% | ring 8,207 / 8,280 | 17,177 | 13,320 |
| 750 KiB, 256 | 72.18 | 357 KiB | 80.1% | FIFO 7,836 / 6,196 | 14,637 | 11,920 |

(First figure's run for the columns after GB/s. 750 KiB's second run was stopped: Mike ruled the size out.)

- **The configuration is tardis1's.** Both ports carry load, the interrupts, nfsd and XPS are as `settled.sh` holds them, and `audit` passes. tardis1's 384 KiB was sized for copy mode, where memory is the limit.
- **The drives are not the limit.** All eight from local fio with no network: 81.6 GB/s. `twoport.sh`'s DRIVE-LIMITED is only 90% busy; rate over busy gives each drive about 9.6 to 10.
- **Memory, from `memcost-log.sh`, the first taken on two ports:**

| | wire GB/s | requests to memory, GB/s | bytes for each byte of payload | region in use, middle |
|---|---|---|---|---|
| 384 KiB | 61.9 | 73 | 1.19 | 3004 pages, 47 MiB |
| 512 KiB | 70.9 | 111 | 1.57 | 4403 pages, 69 MiB |
| local fio | - | 125 | 1.53 (fio writes its own buffers) | - |

Copy on tardis1 took 2.17 bytes a byte, about 131 GB/s at 60.4. So at the same rate the loan takes about 44% less memory traffic. From 384 to 512 KiB the extra 9 GB/s costs 38 GB/s of memory, more than 4 bytes a byte: the region's pages in use leave the 32 MiB L3.

- **The rings:** a ring of 128 holds about 990 KB and has four connections, so a window of about 247 KiB fills it. 512 KiB's window is 244 KiB and that is where ring drops begin. Rings of 256 moved the loss to the FIFO at 750 KiB.
- **Not explained:** 448 KiB's dip. tardis1 at 4 x 128 shows the same shape (61.02 at 384, 49.15 at 448).
- **Not known:** PCIe MaxPayload reads 128 bytes on the card and the drives checked; not in `settled.sh` or the docs searched, and tardis1 is down.

## Rings of 256 and the ceiling (run147, run148)

One run a cell, except where two figures are given.

| held buffer, rings | GB/s | window | client held by the window | drops a / b | client retransmits | region peak | to memory, GB/s | bytes a byte |
|---|---|---|---|---|---|---|---|---|
| 448 KiB, 128 | 52.69, 59.17 | 202 KiB | 96% | ring 24 / 21, 310 / 274 | 49, 595 | 6,745, 8,926 | | |
| 480 KiB, 128 | 69.69 | 229 KiB | 80.1% | ring 5,069 / 5,058 | 10,568 | 11,436 | | |
| **512 KiB, 256** | **73.27** | 241 KiB | 98.0% | **none** | **5** | 12,316 | not taken | |
| 576 KiB, 256 | 73.24 | 270 KiB | 95.8% | FIFO 284 / 286 | 598 | 12,290 | 140 | 1.90 |
| 640 KiB, 256 | 73.57 | 279 KiB | 90.9% | FIFO 4,610 / 4,318 | 9,300 | 11,924 | 140 | 1.89 |

- **512 KiB at rings of 256 is the fastest setting found, and is not to be settled:** 73.27 GB/s with nothing lost, against 61.93 at tardis1's, but at 68 to 70 MiB of the region in use and 1.84 bytes a byte against 47 MiB and 1.19 (Mike, 3 November: the lowest footprint at the best rate). Its cost: nfsd 322.6k and the rings 164.9k cycles a WRITE, the highest here.
- **448 KiB is unsteady, not a dip:** 52.69 and 59.17 at one setting. Rings of 128 go from about 300 drops at 448 KiB to about 5,000 at 480, and the rate from about 59 to about 70. Why it jumps is not explained.
- **About 73 GB/s is a ceiling at rings of 256:** the rate is flat from 512 to 640 KiB, memory requests are 2.18e9/s (about 140 GB/s) at both sizes taken, and only the FIFO drops grow. That fits the card being unable to get frames into memory, but it is not shown: tardis2's DIMM layout is not recorded, so its limit is not known.
- **Rings of 256 cost the loan most of its memory saving:** 1.19 bytes a byte at 384 KiB and rings of 128, 1.57 at 512 and 128, 1.90 at 576 and 256. At 1.90, 80 GB/s needs about 152 GB/s of memory; at 1.19, about 95.

## Eight rings of 64 at 512 KiB (run149, Mike)

Two runs. Each ring's Rx and Tx share one of the port's eight CPUs; tardis1's 8 x 64 runs put Tx on nfsd's CPUs instead.

| | GB/s | ring drops a / b | client retransmits | client held by the window | region in use, middle | to memory, GB/s | bytes a byte | rings cycles a WRITE, all sixteen CPUs |
|---|---|---|---|---|---|---|---|---|
| run 1 | 57.41 | 6,107 / 6,340 | 12,987 | 42.5% | 54 MiB | 64 | 1.11 | 222.5k |
| run 2 | 58.32 | 7,643 / 6,999 | 15,246 | 46.3% | 54 MiB | 64 | 1.10 | 222.2k |

It loses as many frames as four of 128 and runs about 12 GB/s under it, with 1.10 bytes a byte, the least measured. Not the way to 80. A ring of 64 takes two connections, so each has the same ring space as four of 128.

## The budget at 512 KiB and rings of 256 (run150, Mike)

One run a budget, the memory traffic beside each.

| budget, WRITEs | GB/s | drops | client retransmits | region peak | in use, middle | to memory, GB/s | bytes a byte | loans refused |
|---|---|---|---|---|---|---|---|---|
| 128 | 72.73 | none | 9 | 12,312 | 70 MiB | 134 | 1.84 | 993 |
| 96 | 73.37 | none | 8 | 10,170 | 65 MiB | 137 | 1.87 | 18,587 |
| 64 | 72.26 | none | 2 | 7,417 | 68 MiB | 134 | 1.85 | 160,893 |
| 48 | 71.79 | none | 9 | 6,307 | 69 MiB | 128 | 1.78 | 1,970,803 |

The budget sets the peak and costs almost no rate down to 48 WRITEs (about 0.7 refusals a WRITE). It does not move the pages in use in the middle of a run or the bytes a byte: what fills the L3 at rings of 256 is not the loans the budget bounds. The peak is the budget's; the region's use in the middle of a run is the measure against the L3, and on it rings of 256 are about 21 MiB over the settled setting at any budget. 128 against run147's 73.27 shows the setting is steady.

## Budget 64 at the settled setting (run151, Mike)

384 KiB and rings of 128, two runs at budget 64 against budget 128 (run143's b1 and run146's m393216; run143 had no memory traffic).

| budget | GB/s | port a / b, fio | ring drops a / b | client retransmits | region peak | in use, middle | bytes a byte | loans refused |
|---|---|---|---|---|---|---|---|---|
| 128 | 61.93 | 31.29 / 30.65 | 42 / 30 | 75 | 9,881 | | | 0 |
| 128 | 60.95 | 31.98 / 28.97 | 10 / 15 | 29 | 4,084 (memcost's) | 47 MiB | 1.19 | |
| 64 | 63.99 | 30.02 / 33.98 | 6 / 40 | 50 | 6,822 | 58 MiB | 1.33 | 48,552 |
| 64 | 60.23 | 32.76 / 27.47 | 22 / 38 | 70 | 6,850 | 47 MiB | 1.33 | 30,813 |

The rate is the same (62.1 against 61.4 on average). The peak falls about 3,000 pages (30%). The middle is 47 to 58 MiB against 47, and the bytes a byte 1.33 against one figure of 1.19 at 128, so it is not shown to help the L3 and may not. At this setting one port ran 4 to 6 GB/s under the other in three of the four runs, which port it was changing; not looked into.

## Where a page of the region spends its time (run153, `pagelife.bt`)

`xeu-rig/pagelife.bt` follows one page in eight of the region through a cycle, from the pool handing it to the driver to its next hand-out, 10 s inside a run's window; `pagelife-read.py` gives each stage's mean time and, by Little's law, the pages it holds on average. Its total matches the region's use in the middle of an untraced run (about 3,000 pages at 384 KiB). The trace slows the server (51.96 GB/s against about 61, 65.52 against about 73), so the times are somewhat long; the shares are what to read. The WRITE is joined by the thread that releases the page: a record is often borrowed by one thread and written by another. (run152 joined it by the borrowing thread and is not to be read.)

| stage | 384 KiB, 128: mean | pages | share | 512 KiB, 256: mean | pages | share |
|---|---|---|---|---|---|---|
| 1 posted .. borrowed (the ring, then the socket) | 329 us | 603 | 19% | 326 us | 956 | 23% |
| 2 borrowed .. the WRITE issued (the rest of the record) | 572 us | 969 | 31% | 325 us | 878 | 22% |
| 3 the WRITE (the drive) | 160 us | 272 | 9% | 203 us | 549 | 13% |
| 4 the WRITE returned .. nfsd's put | 17 us | 29 | 1% | 19 us | 51 | 1% |
| 5 nfsd's put .. posted again | 584 us | 989 | 31% | 467 us | 1,263 | 31% |
| borrowed, no put seen (borrow .. posted again) | 1,282 us | 182 | 6% | 971 us | 225 | 6% |
| not loaned | 1,388 us | 102 | 3% | 1,179 us | 150 | 4% |
| all | | 3,145 | | | 4,071 | |

- **The drive holds 9 to 13% of the pages.** The rest is the receive side and the way back.
- **The way back is the largest:** about 31% at both settings, a page waiting from nfsd's put to its next hand-out (the skb holding its other buffer, the pool, idle). Its mean is two to four times its median, so a minority of pages hold most of it.
- **The record holds the next most:** pages borrowed early in a 1 MiB record wait for its last frames; at 384 KiB 572 us on average, at 512 KiB 325.
- **What the faster setting adds:** the ring (+353 pages, the deeper rings' posted pages), the drive (+277, its latency 160 to 203 us), the way back (+274). Each page's cycle is shorter (about 1.3 ms against 1.7), but more pages turn.

## Where it stands

The settled two-port setting, 384 KiB and four rings of 128, stays: the lowest footprint that loses nothing (47 MiB in use, 1.19 bytes a byte, about 61 GB/s). Every setting that carries more holds more of the region, in the middle of a run as well as at its peak. 80 GB/s within the L3 needs less of the region for each byte of window, which is a change to the code, not to the setting.

`settled.sh` still holds 384 KiB and rings of 128 for two ports.
