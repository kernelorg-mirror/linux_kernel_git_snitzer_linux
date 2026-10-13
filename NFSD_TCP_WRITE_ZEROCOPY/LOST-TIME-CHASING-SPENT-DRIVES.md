# Lost time chasing spent drives

Written at the end of 2026-10-12 on tardis1, to be gone through on
2026-10-13 (Mike). For most of that afternoon and evening the eight
data drives were in a slow state from sustained writing, and the
results they produced were read as results about the server. This
lists what the published artifact still says that is stale, and every
conclusion and change made that day, sorted by how far the drives
could have skewed it. Nothing here has been acted on: the artifact
was reviewed and not changed.

The artifact is "NFSD Receive Region Budget",
https://claude.ai/artifact/3UQkMVoWW8p9A5ZASPnm3g, source
`artifacts/region-budget.html`. The day's accounts are in
`XEU_CONVERGENCE.md` from "No cache flush to the drives, and two
ports at 58.5 GB/s" to "The two things that pull a two-port run
down". Every two-port run is a line of
`xeu-rig/results-twoport-464.tsv`.

## What the drives were doing

The drives are eight WD_BLACK SN8100 2 TB (firmware 830ZRR02), a
consumer model. Seen once each drive had its own filesystem and was
sampled each second:

- Rested, a drive writes 10.7 GB/s. Under sustained writing it steps
  down to about 4.5 GB/s at 90 to 98% busy and stays there, and in
  the worst runs steps again to about 3.5 and 2.5.
- Two ports at 58 GB/s need 7.25 GB/s from every drive. One port on
  four drives at 44 needs 11.
- Each two-port run writes over a terabyte to each drive. Batches ran
  back to back for hours with no rest.
- Seven of the eight drives did it, a different set each run. Of 14
  runs with per-drive figures, the count of runs each was slow:
  nvme0 4, nvme1 0, nvme2 5, nvme3 6, nvme4 6, nvme5 6, nvme6 1,
  nvme7 3.
- Under raid0 a stripe ran at its slowest member's pace, so one spent
  drive looked like a slow port, and which drive it was could not be
  seen.
- Wear went from 6 to 13% used in the morning to 9 to 17% by night.

Not yet measured: how much a rested drive absorbs before it steps
down, and how long a rest restores it. Those are the first job on
13 October.

## When the results were probably sound, and when not

| period, 12 October | two-port totals, GB/s | state of the drives |
|---|---|---|
| morning, client moved to build 469 | 48.1 to 52.1 | flushing on every COMMIT; some rest between batches |
| about 10:50 to 13:30, flushes stopped, warm-up added | 45.0 to 58.5 | mixed: 58.5, 57.8, 58.2 and 57.4 came after pauses; 45.0 and 51.5 had a slow stripe |
| about 14:00 to 17:10, thread split, wakeup changes | 48.9 to 55.3 | degrading; first stripe down to 3.6 GB/s a drive in one-port runs |
| 17:20 to 18:40, both stripes formatted, rings test | 47.0 to 55.6 | freshly formatted, then spent again within runs |
| 18:45 to 21:45, no raid0, sampled and watched runs | 37.4 to 52.6 | spent in nearly every run |

The best two-port figure, 58.5 GB/s, and its neighbours at 57 to 58
are from the second period and had the drives 71 to 75% busy, so
they stand. Everything measured after about 14:00 was compared
against a moving floor.

## Stale in the artifact

Reviewed from the headline to the end of the two-port section. The
sections below that are dated accounts of earlier days and were not
re-read.

### Wrong as of the end of the day

Storage (raid0 is gone and all eight drives were formatted):

- The two-port settings table: "a stripe of four NVMe drives for
  each port, MD raid0, XFS ... assembled by hand after a boot". It is
  a separate XFS on each drive, four to a port.
- The timeline's 12 October row: "each onto its own stripe of four
  drives".
- The two-port section's setup paragraph: "the eight data drives
  became two stripes of four", with nothing about their being split
  later.
- The harness note: a run "records what each stripe's drives wrote".
- The one-port settings table: "six NVMe drives, MD raid0, XFS".
  True of the 44.6 GB/s run; that configuration no longer exists and
  the page does not say so.

Run counts and ranges for two ports:

- "45.0 to 58.5 across runs" in the top tile, "seven runs, 45.0 to
  58.5" in the best-settings table and the "7" in the flush table
  stop at early afternoon. There are about 60 two-port runs with no
  flush sent, from 37.4 to 58.5.

The drives:

- "after about three minutes of rest they were back at 11" and "it
  looks like the drives' fast write cache running out, which is not
  proven". The evening's data is stronger and says more: the steps
  to 4.5, 3.5 and 2.5 GB/s, seven of eight drives, and that this is
  what pulled the afternoon down.
- "One port is limited by its four drives" does not say two ports
  are limited by them too once the drives are spent.

Rings and CPUs:

- "the ring CPUs 47 to 50%" in the 58.5 run is an average over a
  port's eight. The four receive CPUs are 80 to 86% in softirq and
  the four transmit CPUs about 10%.
- "Ring interrupts ... set by hand, per port" and "the second port's
  settings are set by hand": the harness places the interrupts at
  every run and after every ring change.

Open items since answered:

- Under the one-port table: two threads copying "to be tried again
  with more than one port". Tried on two ports; not a gain.
- Same list: what the linger is worth "not measured". 0 against 50
  was compared on two ports.
- "The second port ran 2 to 4 GB/s under the first ... not looked
  into". Later the first port was usually behind, and the cause is
  partly known: the drives, and one captured stall.

### Missing

- Eight receive rings on CPUs of their own: tested, not a gain. The
  page lists only "eight rings" among 11 October's.
- The removal of raid0.
- The captured 25 s in which one port went silent with the client's
  retransmission timers firing and nothing dropped at the server.
- The one-port check of the 128 KiB mark, which the drives limited
  and which settled nothing; the page shows the setting as pinned
  without it.
- The evening's accounts, in the closing list of where it is written
  up.

### Dated labels that now mislead

- The one-port section's eyebrow says "as of 11 October" and now
  holds two settings pinned on 12 October and the two-port table.
- Its text says `sunrpc` "came up with all three values"; the file
  holds five.
- The two-port section is "Newest ... 11 and 12 October" and nothing
  marks that its figures stop at mid-afternoon.

### Still correct

The headline and both best figures (44.6 and 58.5), the receive
memory figures, the plain-TCP table, the explanation of the flushes,
the copy test, the wakeup counts, the thread-split figures as
measured, the receive-memory table (labelled as measured with the
drives flushing) and both `modprobe.d` lines.

## Conclusions and changes of the day, by how much the drives could have skewed them

### Do not depend on the drives

- **Plain TCP, no NFS and no drives:** 49.8 and 99.4 GB/s with
  receivers that discard, 49.8 and 89.9 with receivers that copy.
- **The copy test** (`TARDIS1_COPY_PERFORMANCE.md`): 62 to 68 GB/s
  memory to memory by any method, reached with 8 CPUs; a copy that
  does not fetch its destination gains nothing.
- **What `xeu` does with ring interrupts:** every port's on CPUs 0
  to 7 at each open, from the driver's source.
- **The wakeup counts.** `svc_tcp_rx_keep` and `svc_tcp_rx_lowat`
  were judged by threads woken and context switches a second at a
  sampled WRITE rate near 56 thousand a second in every run, on and
  off by turns: 1.15 million wakeups to 0.40 million. The counts
  hold. That the rate is "level" was read off noisy totals and is
  weaker.
- **Who wakes whom**, and the split of the cycles in the unlock and
  the switch.
- **The flushes.** A COMMIT for every 8 MiB flushing every drive of
  a stripe is a count (3,550 a second to each drive), and the
  `async` runs showed its cost. The size of the gain (about 4 GB/s
  on two ports, 6 on one) is from few runs.

### Compared within one batch, so the direction is probably right, but on a moving floor

- **nfsd's threads split by port:** 55.3 and 53.8 against 54.1 and
  54.4, and 57.4 for the standing four pools from a later run. "Not
  a gain" rests on two runs each; "two pools by port cost 3 GB/s
  against four hashed" rests on one run against two, hours apart.
- **Eight receive rings:** 52.9 and 52.2 against 54.3 and 55.3, by
  turns. That the receive CPUs fell from 83% to 52% busy without the
  rate rising is solid. Rings of 64 dropped frames (1,234 to 6,033
  at a port), which is its own handicap, and eight rings of 128
  (47.0) took the receive memory past the cache: neither is a fair
  test of eight rings at the same memory and no drops.
- **The client on build 469:** "changes little" from three runs of
  each setting against the day before, with the drives flushing.

### Read wrong at the time, or not settled

- **"The first port stalls" and "the second port runs behind".**
  From about 15:00 the first port's lower rate was put down to an
  unexplained stall. In most of those runs it was a spent drive in
  its stripe. Only `fmt-two-2` (16.7 s worst latency, drives 46%
  busy) and `stall-two-3` (25 s silent, sampled) are the other
  thing.
- **Formatting the drives "did not bring it back".** The runs after
  the format wrote each drive back into its slow state within a run
  or two. What a format does for a spent drive was not measured.
- **Removing raid0 "is not a gain in rate":** 52.6, 50.9 and 50.4
  against 55.6, 51.7 and 50.8, both on drives being spent as they
  ran. Not a comparison of the two layouts.
- **One port with no flush sent:** 42.2 at best of six from 18.1.
  All drive-limited. What one port carries on four rested drives
  with no flush is not known; 43.4 with the export `async` is the
  nearest.
- **The 128 KiB mark on one port:** six runs, five drive-limited.
  Shows that nothing stalls with it and that the linger already
  holds one port to about two wakeups a WRITE. Says nothing of rate.
  It was pinned on that.
- **The drive "slow start" of the morning** (19 to 35 s at 0 to 5
  GB/s after an idle) and the autonomous power states. Never seen
  with them off, not reproduced with them on in the two tries made.
  They were turned off and a udev rule keeps them off; whether that
  matters is not shown. It may be the same slow state seen from the
  other end.
- **The early-afternoon decline from 57 to 54** was noted as "runs
  reach the rate and do not hold it" and left. It was the drives.
- **The multi-second write latencies at the end of `catch-two-3`
  and `-7`** (12 to 26 s) came with the drives' second and third
  steps down; why a write waits that long behind a drive at 2.5
  GB/s was not looked into.

### One real thing found that is not the drives

In `stall-two-3` the first port received 0 to 4 GB/s for 25 s and
its drives wrote nothing, while the second port carried 24 to 39
GB/s. The server's port dropped nothing. The client's `TCPTimeouts`
rose by 10 to 38 a second with 0 to 24 segments retransmitted. The
client's transmit path for that port is suspected and not shown.
Ten watched runs after it did not catch another.

## What was changed on the host and in the tree that day

Kept, and to be looked at again in the light of the above:

| change | where | basis |
|---|---|---|
| Linux told the drives have no volatile cache (`write through`) | udev rule, `drive-cache.sh`, the harness | Mike's instruction; the flush count |
| Drives' autonomous power states off | same udev rule and script | never seen slow with them off; not shown to be the cause |
| `svc_tcp_rx_keep=1` | `/etc/modprobe.d/sunrpc.conf`; code `ddec5cbfb681` | wakeup counts; Mike: keep it |
| `svc_tcp_rx_lowat=131072` | `/etc/modprobe.d/sunrpc.conf`; code `3feba363b9fb` | wakeup counts on two ports; one port inconclusive |
| A 40 s warm-up fio does not count, and each run's drive figures | `twoport.sh` | to keep a slow start out of the figure; it does not keep a slow state out |
| All eight drives formatted, twice for the first four | `nvme format --lbaf=1 --ses=1` | Mike's instruction; `nvme4n1` went from 512-byte to 4 KiB |
| No raid0: a filesystem on each drive, four to a port | `/hs/nvme0` to `/hs/nvme7`; `twoport.sh` | Mike's instruction; each drive can now be seen |
| Ring interrupts placed again after a ring change | `twoport.sh`, `iperf2port.sh` | a real fault: both ports' rings had shared CPUs 0 to 7 in one plain-TCP run |
| `swap-sunrpc.sh` refuses while NFS connections are up | the script | it was run under a live run and stopped nfsd |

Tried and not kept: `pools_by_local` (the patch is
`/var/tmp/zc-run-logs/pools-by-local.patch`; the code is not in the
tree), `TXIRQ_A`/`TXIRQ_B` for receive rings on their own CPUs (the
option stays in `twoport.sh`, unused).

## To settle on 13 October, in order

1. The drives, locally and with no NFS: what a rested drive takes
   at 7 GB/s and at 11 before it steps down, and how long a rest
   restores it. `/var/tmp/zc-run-logs/drvprobe.sh <tag> <dir> <secs>`
   is the probe.
2. From that, a run length and a pause that keep a run inside the
   drives' fast state, and the harness changed to refuse or mark a
   run whose drives stepped down.
3. Then again, on rested drives: two ports at the pinned setting for
   a baseline; the 128 KiB mark off and on for rate, on one port and
   two; eight rings of 64 with enough depth not to drop, if still of
   interest; raid0 against separate filesystems only if it matters.
4. The artifact, corrected from the list above once the figures
   under it are trusted.
5. The silent port: the watchers are `xeu-rig/stall-watch-server.sh`
   and `stall-watch-client.sh`.
