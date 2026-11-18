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

## The before profile and trace (Mike: "have you collected perf data
from both the client and server while running the tip on the server?
as the comprehensive 'before' profile and trace data")

`prof-write.sh 1077`: one WRITE pass with perf on both hosts (499 Hz,
5 s, data in each host's /dev/shm, `-N`), mpstat on both, the client's
memory controllers (round 107's shape), `ss -tni` on both hosts' ports
and the client's per-transport RPC queues three times in the load, and
nstat deltas on both.  The same script runs on build 469 after the
reboot.

### t2-r1077: build 541's client, the tip's server (17:51 to 17:54)

PASS, 74.79 GB/s (37.14 / 37.65), no drive stepped down, no new kernel
warning; fio's completion 57 ms mean, 156 ms p99.  Raw perf data:
`/dev/shm/zc-p136-1077.data` (server), `/dev/shm/zc-cli136-1077.data`
(client).

- **CPU** (mpstat, seconds 4 to 9, iowait as idle): client 43% / 42%
  (node 0 / node 1), server 25% (work CPUs 8-55 27%).  Neither host is
  CPU-bound.
- **Client memory**: 158.9 GB/s in the 5 s, 2.13 bytes a byte written
  (round 107 on build 469: 2.17).
- **Client profile**: `cpu_idle_poll` 56.6% (idle=poll); fio 15.7% of
  samples, `get_io_u` alone 7.25% (fio's own); then
  `__list_del_entry_valid_or_report` 3.23%, `nfs_page_create` 1.48%,
  `nfs_scan_commit_list` 1.35%, `nfs_clear_request` 1.14%,
  `try_grab_folio_fast` 1.10%, spin lock 1.0 + slowpath 0.83%,
  `mutex_spin_on_owner` 0.82%.  rpciod and xprtiod workers each under
  0.5%.
- **Server profile**: idle 51%, nfsd 47%; softirq 5.6%, the ring's
  receive (`xeu_rx_poll` 1.9%), the loan's table 0.8%.
- **Sockets** (`ss -tni`, seconds 6, 9, 12): the client's 32 sockets
  hold 50 to 53 MB unsent (1.6 MB each), cwnd 41 to 47 segments of 7792
  bytes (about 350 KB); the server's hold nothing unsent and 4.5 to 5.2
  MB unread.  Client bytes retransmitted from second 6 to 12: 1.19 MB.
  The server's rings dropped 85 and 68 frames in the 10 s window.
- **The client's RPC queues** (`xprt_info`): 32 TCP transports, 16 a
  port; 4,042 to 4,398 RPCs waiting to send (up to 315 on one
  transport), 147 to 161 sent and waiting for a reply, none in the
  backlog: about 5 in flight a transport, 130 queued behind them.
- nstat: this run's files have the counters since boot (`nstat -a`, a
  mistake); `prof-write.sh` now writes the deltas since second 4.

## Build 469's client (Mike rebooted it at about 18:28)

The one-shot entry was set with `grub2-reboot`; this grub saves the
entry it boots, so `saved_entry` is now 469 too, and goes back to 541
with `grub2-set-default` when the round wants it.

- `uname -r` 7.3.0-0.rc7.2.hs.469.el9.x86_64, the same command line as
  541's; no NFS mounted; both ports answer (0.049 and 0.039 ms).
- **The two client patches** (Mike: "469 should have the 2
  patches/client/patches applied via updates/ please verify"): sunrpc
  is loaded from `updates/sunrpc.ko`, srcversion
  `AB674CEE87351D814F68CEE`, the build in the client's
  `/root/zc-csunrpc/C`, whose source has both: 0001's `WQ_SYSFS` on
  rpciod and xprtiod (`sched.c` 1333, 1337; both in
  `/sys/devices/virtual/workqueue/`) and 0002's splice (`socklib.c`'s
  splice flag through `xprt_send_pagedata()`, `xprtsock.c` 1146 setting
  `MSG_SPLICE_PAGES`).
- **The client audit**: two BAD, both what build 469 is: the kernel,
  and `rpciod/per_cpu` 0 (per-CPU rpciod, `ccaaf769083fe`, is one of
  the commits it lacks).  `settled.sh` takes `EXP_CLI_WQ` now, as it
  took `EXP_CLI_KERNEL`, and the runs on 469 are held with
  `EXP_CLI_KERNEL=7.3.0-0.rc7.2.hs.469.el9.x86_64
  EXP_CLI_WQ="rpciod/per_cpu=0"`: with them the audit reads the
  settled setting.

Then `write-check.sh 1078` and `prof-write.sh 1079`.
