# Round 134: block accounting off on the data drives

Mike, 18 November: "run the iostats=0 A/B, quick".  Round 133's
profile put 0.34 us of the server's CPU a READ in reading the counter
(`arch_counter_get_cntpct`), 0.12 of it for block accounting
(`blk_account_io_start`, `blk_mq_start_request`), with the accounting's
own work beside it.  `queue/iostats` is 1 on the eight data drives and
`settled.sh` does not hold it.

`ab-iostats.sh 1066:1 1067:0 1068:1 1069:0`: the client on round 132's
P stack (as t2-r1063, 3.50 M), iodepth 96, iostats set on nvme0n1 to
nvme7n1 for the run and back to 1 after each.  The client goes back to
build 541's stack after.

## The passes (Mike: "run a READ and WRITE pass and record result with a commit", and "I've taken to skip the READ and WRITE pass verification steps but they are important")

After the A/B, with the client on round 132's P stack (so that the
client's change is under them, and the server on round 129's stack,
so that rounds 129 to 134 are): `write-check.sh 1070`, then
`read-check.sh 1071`, each through `step.sh`; the client back on build
541's stack after.

## Result (18 November, 15:55 to 17:05)

| Run | iostats | M IOPS (ports) | p99 a; b (ms) | Server busy, us a READ | Client busy (busiest) | Client us a READ |
|---|---|---|---|---|---|---|
| t2-r1066 | 1 | 3.53 (1.84 / 1.69) | 3.65; 4.18 | 87%, 15.77 | 83% (90%) | 11.29 |
| t2-r1067 | 0 | 3.52 (1.86 / 1.66) | 3.56; 4.01 | 86%, 15.64 | 83% (91%) | 11.32 |
| t2-r1068 | 1 | 3.52 (1.83 / 1.69) | 3.59; 3.72 | 86%, 15.64 | 82% (88%) | 11.18 |
| t2-r1069 | 0 | 3.52 (1.84 / 1.68) | 3.62; 3.92 | 86%, 15.64 | 83% | 11.32 |

iostats 1: 15.705 us of the server's CPU a READ; iostats 0: 15.64.
The two runs at 1 differ by as much on their own (0.13 us), and IOPS
are the same at 3.52 to 3.53 M: **block accounting off saves nothing
measurable**, whatever round 133's profile gave it (0.12 us of counter
reads).  Not proposed for `settled.sh`; iostats is 1 on all eight
drives again.  No new kernel warning.

**The passes**, the first since round 128 (Mike: "I've taken to skip
the READ and WRITE pass verification steps but they are important"),
with the server on round 129's stack (rounds 129 to 134's server code,
the partial wait's switch at Y) and the client on round 132's P stack
(rounds 131 and 132's client code):

| Run | Pass | GB/s | Mark | Result |
|---|---|---|---|---|
| t2-r1070 | WRITE | 75.21 | 74.6 | **PASS**, no drive stepped down, no new kernel warning |
| t2-r1071 | READ | 78.93 | 71.5 | **PASS**, no new kernel warning |

The WRITE pass is build 541's client's 75 (t2-r993: 75.20); the 77.8
of build 469 is round 136's (Mike: "next round should be to revisit
the WRITE drop from 78 to 75 GB/s").
