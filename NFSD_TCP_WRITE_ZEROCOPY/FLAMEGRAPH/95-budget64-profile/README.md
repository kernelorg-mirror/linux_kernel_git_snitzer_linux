# Round 95: the profile at the budget of 64, for the next round's focus

**The ask (Mike, 12 November):** "keep the changes, but put the window
back to 0 and settle the budget at 64.. then re-run 3 runs and do a
profile to see what should be focus of the next round".

## The setting

- The window of loaned WRITEs per export (round 94, `05844df27a1f`,
  `db48abad7ef5`, `9e4eb880bb91`) stays in the tree and in the loaded
  stack (sunrpc `FC78BAE8A6C2B0A92A1366A`), at
  `svc_tcp_rx_target_window` 0: off.
- **The two-port loan budget is 64 WRITEs** (`xeu-rig/common.sh`,
  `TWO_WRITES`), from 128: round 94's sweep (`t2-r490..r493`) found 64
  halves the region's peak (7,362 pages against 11,511) with the rate,
  the median held and the bytes through memory unchanged.  The runners
  pass `WRITES=$TWO_WRITES`.

## The runs

[run501.sh](run501.sh), round 88's `run367.sh` at this setting: three
two-port loan runs, `t2-r501` counted (cycles, instructions, L1D and
L2D refills on nfsd's CPUs 8-55 for 8 s of the window), `t2-r502`
cycles with call graphs, `t2-r503` the held mark for the exact peak;
the region sampled beside each.  Read with round 76's `read65.py`,
`top.py`, `groups.py`, `symdiff.py` (against round 88's `t2-r367`,
`t2-r368`, the same stack's predecessor at 128) and round 77's
`callers.py`, `under.py`, round 88's `schedwhy.py`.

## Result: round 88's profile; the rate is the window over the RTT

[run501.out](run501.out).  Every run clean: no new kernel warning, no
drive stepped down, retransmits 34 to 35.

| run | kind | GB/s | WRITEs/s | nfsd cycles a WRITE | woken/W | held, median (L3) | p99 | peak |
|---|---|---|---|---|---|---|---|---|
| `t2-r501` | counted | 71.59 | 68,342 | 248,133 | 4.26 | 3,593 (1.75) | 6,279 | 6,779 (sampled) |
| `t2-r502` | call graph | 71.20 | 67,817 | 243,439 | | 3,555 (1.74) | 6,277 | 6,798 (sampled) |
| `t2-r503` | held mark | 72.19 | 68,779 | | | 3,666 (1.79) | 6,210 | **7,052 (exact)** |
| round 88, `t2-r367`, `r368` (budget 128) | counted | 69.89, 70.63 | | 242,898, 244,808 | 5.81, 5.71 | | | 11,919 (exact, `r370`) |

- **The budget at 64 does what the sweep said:** the exact peak 7,052
  pages against 11,919, the rate and the median held (56 MiB, 1.75 L3)
  as before.
- **The profile is round 88's to within 2%**
  ([read65-r501.txt](read65-r501.txt),
  [groups-r501.txt](groups-r501.txt) against
  `../88-build522-baseline/groups-522.txt`,
  [top-r501.txt](top-r501.txt),
  [symdiff-r501-88.txt](symdiff-r501-88.txt)):
  248k cycles a WRITE against 243 to 245k, every area within 1.5k.  Of
  the 4k more, 2.4k is `[unknown]`: xeu's symbols, unresolved since its
  reload moved it (the kallsyms are saved; `ringcost.py`'s way resolves
  them), not a cost.  The window's check with the window off is 195
  cycles a WRITE (`svc_tcp_rx_target_admit`), 0.08%; a static key would
  take it to nothing.
- **Where the cycles are** ([callers-r502.txt](callers-r502.txt),
  [under-r502-release.txt](under-r502-release.txt),
  [under-r502-switch.txt](under-r502-switch.txt),
  [under-r502-blk.txt](under-r502-blk.txt),
  [schedwhy-r502.txt](schedwhy-r502.txt)), k cycles a WRITE of 243:
  the thread switch 24.0 (17.3 of it the idle task switching to a woken
  thread), block submit 21.9 (`blk_add_rq_to_plug` 11.3), the receive's
  loan bookkeeping about 22, the release 12.2 (`rx_loan_put_run` 7.4;
  14.0 in round 88, traced in round 89, the stack of 128 that would take
  9k of it not kept in round 90 for its footprint), `svc_send` 34.0
  inclusive.  Nothing new; nfsd's CPUs are 15% busy, and none of it
  sets the rate.

**What sets the rate.**  The client's 24 connections are
`rwnd_limited` 99.4 to 99.5% of the window (`t2-r501`'s
`pmu/ss-client.txt`): the client always has data and is waiting for
the server's receive window.  The server advertises 243,712 bytes a
connection (`rcv_ssthresh`, 0.465 of the 512 KiB held buffer; `snd_wnd`
on the client the same on 16 of 24, 187 to 217 KiB on the rest), and
the round trip the client sees is 85 to 93 us smoothed against a floor
of 12 to 14 us (`minrtt`): 24 connections times 244 KiB over 90 us is
65 GB/s, over 80 us 73.  So the rate is the window over the round
trip, as a window-limited TCP flow's is, and three quarters of that
round trip is not the wire.  It is the server's: the time a segment
waits in the ring and the NAPI deferral (2 interrupts or 10 us), in
the socket until nfsd reads it (`sockq.py`'s 1.5 MiB over 24
connections at 3 GB/s each is about 20 us), the read itself in 5.6
passes a record with a thread wake for each (round 63;
`finish_task_switch` is the largest symbol in the profile for it), and
the ACK that carries
the opened window back through the transmit path.  Nothing in rounds
56 to 94 measured that path's latency.

## The focus suggested for round 96: the server's share of the RTT

Every held-page row of round 94 is the rate times a time, and the
rate itself is the window over a round trip the server mostly makes.
Shortening the server's latency is the one lever that moves both ways
at once: at the same window and connections the rate rises, or at the
same rate the window can fall and with it the rings, the assembly and
the peak.  Round 56 tried the other two levers of the product and
paid for each: more connections cost footprint, a larger held buffer
cost ring drops.  The per-drive window (round 94) could not shorten
the drive's queue without starving the drives because the burst had
to wait a whole round trip plus the assembly on the client.

**First the measurement, no code:** in a settled run's window, per
connection, the time from a segment's arrival (`xeu`'s NAPI poll) to
nfsd's read of it, from the read to the ACK that opens the window
(`tcp:tcp_probe` gives `snd_wnd`, `rcv_wnd` and the sequence at each
segment, both ends), the passes a record and the wake latency of each
(`sched:sched_wakeup` to `sched_switch` for nfsd's threads), and the
transmit path's time for the ACK.  Then the levers in order of what
the measurement shows: the NAPI deferral and `gro_flush_timeout` (set
for the rings' CPU cost in rounds 30 to 32, never for latency), the
`want_eighths` mark (6: a thread waits for six eighths of a window
before it reads, which cuts passes and adds latency by design), round
63's options for fewer passes a record, and the wake path (17k cycles
a WRITE in the idle task's switch).  Each reports the round trip, the
rate, the held median and the peak together.
