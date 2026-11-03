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

- **512 KiB at rings of 256 is the best setting found:** 73.27 GB/s with nothing lost, against 61.93 at tardis1's. Its cost: nfsd 322.6k and the rings 164.9k cycles a WRITE, the highest here.
- **448 KiB is unsteady, not a dip:** 52.69 and 59.17 at one setting. Rings of 128 go from about 300 drops at 448 KiB to about 5,000 at 480, and the rate from about 59 to about 70. Why it jumps is not explained.
- **About 73 GB/s is a ceiling at rings of 256:** the rate is flat from 512 to 640 KiB, memory requests are 2.18e9/s (about 140 GB/s) at both sizes taken, and only the FIFO drops grow. That fits the card being unable to get frames into memory, but it is not shown: tardis2's DIMM layout is not recorded, so its limit is not known.
- **Rings of 256 cost the loan most of its memory saving:** 1.19 bytes a byte at 384 KiB and rings of 128, 1.57 at 512 and 128, 1.90 at 576 and 256. At 1.90, 80 GB/s needs about 152 GB/s of memory; at 1.19, about 95.

`settled.sh` still holds 384 KiB and rings of 128 for two ports.
