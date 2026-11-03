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
