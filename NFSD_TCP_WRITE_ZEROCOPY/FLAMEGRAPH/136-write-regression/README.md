# Round 136: the WRITE pass from 77.8 to 75 GB/s

Mike, 18 November: "I think next round should be to revisit the WRITE
drop from 78 to 75 GB/s.  That is a big red flag that cannot go
unanswered.  When the current queued work is completed, I can reboot
the client into the previous kernel that enjoyed 78 GB/s and then we
can have the fun of figuring out why the latest tip regresses WRITE
from 78 to 75 GB/s".  `WRITE.md`, TODO 1, has the client commits
between build 469 (77.73 to 77.91, rounds 106 and 107) and build 541
(74.85 to 75.21) and the order of attack.

The server runs the tip's own svc stack since 17:24 (round 135, "After").

1. **Before the reboot:** `write-check.sh 1076` on build 541's own
   client stack against that server: the figure the reboot is set
   against.
2. **On build 469 (Mike reboots the client):** the boot's checks (both
   ports, `settled.sh audit`, reported as found), two WRITE passes: if
   469 still writes about 77.8 against today's server the regression is
   the client's alone.  Then a WRITE pass with perf on both hosts (data
   in /dev/shm, `-N`), the server sockets' Send-Q and the client's
   per-transport queues.
3. **Back on build 541:** the same profile beside it; the A/Bs with no
   rebuild (`CLI_SCOPE`, `PIN=0`, nconnect 12 and 24); then the client
   commits out one at a time with cbuild/cswap (the splice
   `1b9af79d9f16f` first, then the per-CPU transport map
   `5555163d661a0`, then rpciod per-CPU `ccaaf769083fe`).

## Before the reboot (18 November, 17:25)

| Run | Client | Server | WRITE GB/s | Result |
|---|---|---|---|---|
| t2-r1070 (round 134) | build 541, round 132's P stack | round 129's stack | 75.21 | PASS |
| **t2-r1076** | **build 541's own stack** | **the tip's stack** | **74.60** | PASS, at the mark; no drive stepped down, no new kernel warning |
| t2-r858, r868 to r871 (rounds 106, 107) | build 469 | build 534's stack then | 77.73 to 77.91 | |

The client has no NFS mounted and is ready for Mike's reboot into
build 469.
