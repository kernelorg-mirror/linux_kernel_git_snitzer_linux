# Round 92: the receive refill path on tardis2

tardis2, build 522 (7.3.3-14.hs.522.canary, 16 KiB), loan mode on two
ports, the settled setting (the pool's stack at 96; the client's
`rpciod` and `xprtiod` strict, round 91).

**Why.**  Round 91 found the server's ring tail drops a steady trickle at
every time scale it could sample, down to 10 ms: in a fifth to a quarter
of 10 ms intervals, a few frames at a time, following neither the frames
arriving nor the ring CPUs' NAPI polls at full budget, and rising with
the rate.  So a ring runs out of posted buffers for a few frames at a
time, under 10 ms.  Mike (10 November): the next area is the refill path.

**The driver, as tardis2 runs it.**  The source is
`kmod-xsight-2.0.1.tar.gz` with kernel-rpmtree's `xsight/` patches 0 to 13
applied as the spec's `%autosetup -p0` does; its module parameters are
exactly the loaded `xeu.ko`'s.  Live: `rx_refill_thresh` 8,
`rx_buf_size` 8192 (two buffers a 16 KiB page), rings of 128 under load
(`twoport.sh`'s two-port `ENTRIES=128`; 256 at rest, which is what a
first look read).  So a ring has 1 MiB posted, about 110 us at the 9
GB/s each of a port's four rings takes.

- A ring is refilled only at the end of `xeu_rx_poll()`, once at least
  `rx_refill_thresh` slots are empty (`xeu_rx_refill()`, buffers from
  the ring's page pool, which the receive region backs).
- A refill that leaves the ring short arms a timer to poll again in
  `XEU_RX_REFILL_RETRY_USECS`, 1000 us: about nine ring lifetimes.
- A failed allocation prints "Failed to allocate Rx page from pool"
  (rate limited).  There has been none since this boot.
- Reading the ring counters (`ethtool -S`, `ndo_get_stats64`) waits on
  the card's mailbox in `udelay(100)` steps under `spin_lock_bh`, about
  1.4 ms a port: round 91's 10 ms sampling was not free.

**How (`refill.bt`, `run433.sh`).**  No BTF for `xeu`, and the refill is
the compiler's `xeu_rx_refill.isra.0(rxr, n)`, which returns nothing.  The
ring's size (u16 at `rxr+70`), producer index (`+10`) and consumer index
(`+14`) are read where `xeu_rx_poll()`'s code reads them in the loaded
module.  Per ring: at each refill the buffers still posted (size less
`n`), how often fewer than 32 or none were left, and what each refill
posted (the producer index at the next refill less this one's) against
what it asked for.  Per NAPI: the polls, frames, polls at full budget,
the gap before a full-budget poll (the time the card had the ring alone)
and each poll's length.  Then the retry timer, NULL returns of
`page_pool_alloc_pages`, the page pool's slow path and the region's
allocations.  T U T, `t2-r433..r435`: 10 s traced from the start of
twoport's window, against an untraced run, for what the trace costs.

## Result (`t2-r433..r435`, `refread-r433-r435.txt`)

| run | | GB/s | twoport's ring drops (10 s window) |
|---|---|---|---|
| t2-r433 | traced | 67.22 | 10,780 |
| t2-r434 | untraced | 71.84 | 2,100 |
| t2-r435 | traced | 67.11 | 10,862 |

- **The trace is far too heavy:** 4.6 GB/s less and five times the
  drops.  Its probes on every poll (about 290,000 a second, plus the
  tracepoint) cost the ring CPUs that much, so its timings (gaps, poll
  lengths) are of a disturbed system and are not used.
- **Seen from the driver, no ring came near running out at a refill:**
  of 2.42 million refills in 10 s, every one found at least 48 of the
  ring's 128 buffers still posted (most 64 to 128).  The driver's count
  of posted buffers includes those the card has already filled and the
  poll has not reached, so this does not say the card had free buffers.
- **No allocation failed:** no NULL from `page_pool_alloc_pages`, no
  slow path; the region gave 426,000 pages to the pools' stacks.
- **But the retry timer fired about 1,326 times in 10 s**, about 133 a
  second, and only a refill that leaves 8 or more slots empty arms it,
  where the trace's own measure (the producer index at a ring's next
  refill) found no refill short.  One of the two is wrong.

**Next (`short.bt`, `t2-r436`):** a light trace, no probe on the polls:
at `xeu_rx_refill_retry_if_short()`, right after each refill, how many
slots the ring still has empty, and the timer.

## Result (`short.bt`, `t2-r436`)

71.59 GB/s, ring drops 1,788 in twoport's window: the light trace costs
nothing.  In 10 s, 2.68 million refills asked for 99.3 million buffers.

- **No refill comes up short.**  Right after each of 2.58 million
  refills (`xeu_rx_refill_retry_if_short()`), the ring had fewer than 8
  empty slots: every refill posted what it asked for.
- **The retry timer's 1,409 firings are a wrap of the ring's indexes.**
  `xeu_rx_refill_retry_if_short()` is the only caller of `mod_timer` on
  the refill timer, and in the loaded module it computes the empty slots
  as `cons + size - prod` in 32 bits, not masked to the u16 the indexes
  are:

  ```
  f458:	add	w1, w1, w3	// cons + size
  f45c:	sub	w1, w1, w5	// - prod, not masked to u16
  f474:	cmp	w1, w2		// >= rx_refill_thresh: arm the timer
  ```

  So whenever a ring's producer index has wrapped past 65535 and its
  consumer index has not yet, the count is near 65536 and the timer is
  armed though the ring is full.  A ring posts about 1.12 million
  buffers a second (89.5 million in 10 s over eight rings), so it wraps
  every 58 ms: about 17 a second a ring, 137 a second over eight, as the
  timer fires (133 to 141 a second).  `xeu_rx_poll()`'s own count is
  masked (`and w1, w1, #0xffff`), so the refills are right.  The bug is
  in kernel-rpmtree's `xeu-retry-an-RX-refill-that-came-up-short.patch`
  (the C expression `ring->size - (ring->sr_prod_idx -
  ring->sr_cons_idx)` is computed in int); it is harmless (the timer
  only schedules the ring's NAPI, normally running already) and a cast
  to u16 fixes it.

**So the refill path does not explain the drops.**  Allocation never
fails, no refill is short, and at every refill the driver still counts
at least 48 of the ring's 128 buffers posted.  Those include buffers the
card has filled and the poll has not yet reached, so what is left is the
card's side: how many of the posted buffers are still free when frames
arrive, which is a question of how long the card's completions wait for
a poll (a ring of 128 is about 110 us at 9 GB/s).

## The wrap fixed in kernel-rpmtree (11 November, Mike: "fix the wrap")

kernel-rpmtree `cd60c0b` (pushed; `7cbd88c` before) "xsight: count an xeu ring's empty slots in a u16
at the refill retry", Release 14: `u32 rx_fill_cnt` becomes `u16` in
the retry patch (Patch3), as the poll's own count is, and the threshold
patch (Patch4) after it carries the new context.  All fourteen patches
apply to the tarball in spec order, and the only difference from the
source of the loaded module is that line; xeu builds against this kernel
with no warning of its own.  It is not loaded: no reload of `xeu` on
this boot.  It changes no figure here (the timer only kicks a NAPI that
is already running) and takes effect with the next package.

## Step 2: how long a ring that is behind waits for its refill

Mike, 11 November: do 2 (a light look at how long the card's
completions wait for a poll), then 3 (rings of 256 against 128) is in
play.

What sets it.  The MTU is 7792, so a frame takes one 8 KiB buffer; a
poll takes at most 64 frames (`XEU_NAPI_POLL_WEIGHT`), half a ring of
128, and refills only at its end.  Each ring has a CPU of its own (port
a's on 0, 2, 4 and 6, port b's on 56, 58, 60 and 62; transmit on the odd
CPUs), and both ports defer at 2/10000: a poll that empties the ring
leaves it masked and the next poll comes from a 10 us timer.  A ring that
is behind (a poll took its whole budget, so more was waiting) is polled
again at once, and the card then has, until that poll ends, what was
posted less what was waiting: at most 64 buffers.  So the figure that
decides a drop is how long a full poll takes against how fast frames come
to its ring.

`napigap.bt` gets it from the `napi:napi_poll` tracepoint alone, the one
round 91's 10 ms sampling used at no measurable cost: per port, the gap
from each poll's end to the next of the same NAPI, by whether the one
before took its whole budget, some or none; the length of each run of full
polls, and the frames and time inside them; the frames a poll took; and
what a poll took after a wait of 64 us or more that did not follow a full
one.  `run433.sh` with `BT=napigap`, T U T from t2-r437, keeps
`/proc/net/softnet_stat` before and after each run as well (a squeeze of
NET_RX by its budget is its third column).

### What it found (t2-r437 to r439)

```
t2-r437 T | 71.58 GB/s | ring drops 1856 | retransmits 1996
t2-r438 U | 71.66 GB/s | ring drops 1929 | retransmits 2060
t2-r439 T | 71.23 GB/s | ring drops 1525 | retransmits 1613
```

The trace costs nothing (the untraced run between is at the same rate
and drops).  The reading is in `gapread-r437-r439.txt`; the two traced
runs agree, and the two ports do too.  Per port, in 10 s:

- **About 2 million polls, 10% of them full** (64 frames: more was
  waiting).  Runs of full polls are short: median 2, the 99th percentile
  4.
- **Inside such runs a ring is drained at 1.87 frames a us**, 34 us a
  full poll, 14.6 GB/s a ring.  A ring's average is about 9 GB/s, so a
  ring is behind only in bursts.
- **The polls are not late.**  After a full poll the next follows at once
  (gaps of 64 us or more 0.03 to 0.09%, the longest 93 to 320 us); after
  a partial one the deferral's timer brings the next (64 us or more 0.01
  to 0.08%).  Only after an empty poll, with the ring idle and unmasked,
  are gaps of 32 to 64 us common (about 22%) and of 64 us or more 1.7 to
  2%: the ring had nothing to take.
- **What a poll finds after such a wait shows how frames come:** the
  frames taken peak at 24 to 32 (a client TSO send of about 256 KiB at
  7792 a frame), and 7 to 10% of those polls are full.
- **NET_RX is never squeezed** (0 to 6 in 10 s a CPU) and the backlog
  drops nothing (`softnet_stat`).

So a completion does not wait for a poll that is late: the card is
outrunning a poll that is on time.  Frames come to a ring in bursts of
TSO sends at line rate, several connections to a ring (twelve a port on
four rings), and a full poll drains 64 frames in 34 us while the card
has at most 64 buffers free until it ends.  A burst of more than the
ring holds, at more than 1.87 frames a us, is dropped at the tail.  Two
levers: a ring that holds a longer burst (step 3, rings of 256) or a
poll that drains faster, or refills before its end.

## Step 3: rings of 256 against 128 (planned)

`run440.sh`: twoport's 128 against `ENTRIES=256` (a run's experiment,
which `settled.sh` audits), 128 256 256 128 128 256 from t2-r440, the
held mark on in every run for the region's exact peak, since a ring of
256 posts 128 more buffers (1 MiB) a ring: up to 8 MiB more of the
region over eight rings, against the footprint rule.  What counts:
drops, retransmits and rate against the peak.

### What it found (t2-r440 to r445)

```
t2-r440 ring 128 | 62.91 GB/s | ring drops 7748 | retransmits 7975 | peak  8643   (not counted)
t2-r441 ring 256 | 70.36 GB/s | ring drops    0 | retransmits   11 | peak 10912
t2-r442 ring 256 | 71.20 GB/s | ring drops    0 | retransmits   28 | peak 13239
t2-r443 ring 128 | 70.89 GB/s | ring drops 1221 | retransmits 1300 | peak 10138
t2-r444 ring 128 | 71.68 GB/s | ring drops 1813 | retransmits 1920 | peak 11878
t2-r445 ring 256 | 70.68 GB/s | ring drops    0 | retransmits   27 | peak 12456
```

t2-r440 does not count: twoport placed none of port a's flows ("No
flows in place-flows-a.txt"), so RSS left ring 0 with none of the
traffic and rings 2 and 3 with 44 and 42%, and port a dropped 7743 and
carried 26.3 GB/s.  Every other run placed its flows (rings at 24 to 26%
each).  The settled setting was back at rest after the last run.

- **Rings of 256 drop nothing**: 0 in all three runs, against 1221 and
  1813 at 128, and the client's retransmits fall from 1300 to 1920 to 11
  to 28.  That confirms step 2: the drops are bursts larger than the
  ring holds.
- **The rate does not rise**: 70.75 GB/s on average at 256 against 71.29
  at 128 here (and 71.2 to 71.8 at 128 in t2-r434 to r439).  The
  retransmits cost nothing measurable, and 256 may be 0.5 to 0.8 GB/s
  lower.
- **The region's peak rises**: 12,202 pages on average at 256 (10,912
  to 13,239) against 11,008 at 128 (10,138 and 11,878), about 1,200
  pages (11%, 19 MiB) more.  That is more than the 512 pages (8 MiB) the
  rings' extra buffers alone would hold, but the spread between runs is
  as large.

Under the footprint rule, rings of 256 are not kept as they are: they
remove the drops but raise the peak and do not raise the rate.  It is
Mike's call.  What would remove the drops without growing the region is
step 2's other lever: a poll that refills before its end, so that a
ring behind never has only 64 buffers free for a whole 34 us.

## The mid-poll refill (11 November, Mike: "write the mid-poll refill patch for xeu")

kernel-rpmtree `19c4c5d` "xsight: refill xeu's RX rings in the middle of
a poll", Patch14, Release 15: `rx_refill_mid` (default 0, read once a
poll).  Set, the poll also refills its ring between frames as soon as
that many slots are empty, after `sr_cons_idx` has moved past the frame
just taken, so only slots the poll is done with are posted again.  The
end-of-poll refill and its retry are unchanged, and the ring holds no
more buffers than it did: the region's footprint is bounded as at 128,
though the ring may run fuller on average.  Each refill is a doorbell
write, so a low setting costs the rings some.

All fifteen patches apply to the tarball in spec order, and xeu builds
against this kernel with no warning of its own.  It is not loaded (no
reload of `xeu` on this boot).  The A/B it wants on the next boot with
the package: twoport's rings of 128 with `rx_refill_mid` 0 against 16
and 32 (16 posts a quarter of a full poll's buffers back mid-poll, 32
half), for ring drops, retransmits, the rings' cycles and the region's
peak, with rings of 256 beside them for what the drops cost.

## Release 15 loaded (11 November, Mike: "add the new xeu driver to updates and reload the xeu interfaces")

Built from the tarball with the fifteen patches against this kernel; its
`xsl_core` came out identical to the loaded one (srcversion
5D07B4F4E11C2A6F73046EE), so only `xeu` was reloaded.  The new `xeu.ko`
(srcversion C3071685F0211EF8EBA2A7F, against Release 13's
CA0931345E46C929B11D01F) is in `/lib/modules/$(uname -r)/updates/` beside
the `sunrpc.ko` already there (10 November, the one loaded), after `depmod -a`; the Release 13 rpm is still
installed, its module in `extra/xsight/` shadowed.  With the client free
and the setting audited at rest: `rmmod xeu; modprobe xeu` (02:11:24),
then `port-setup.sh` for each port and `settled.sh apply`, as on tardis1
(`xeu-rig/XSIGHT-FOLLOWUP.md`, X4's reload).

As on 7 November, the ports came back with the driver's default eight
rings, which ran the region dry for the 30 s until `port-setup.sh` cut
each to four (`alloc_fail` 13, "Failed to allocate Rx page from pool" a
few times a second on both ports).  Once both were set up the region had
31,664 pages free and four pools a port, and nothing has failed since.
After `settled.sh apply`: "the settled setting" at rest, both ports
answer the client, the rings' interrupts on 0 to 7 and 56 to 63 as
before, `rx_refill_mid` 0, kdump armed.  The record is in
`/var/tmp/zc-run-logs/xeu-r15-reload/`.

A note on step 2's "a frame to a buffer": xeu says at every open that a
frame of MTU 7792 can span two buffers of 8192 with `rx_ip_align` 2.
Round 92's first trace asked 99.3 million buffers in 10 s for about 92
million frames, so about one frame in thirteen takes two, and a full poll
of 64 frames takes about 69 of a ring's 128 buffers, not 64.

## The mid-poll refill's A/B (planned; Mike, 11 November: "run the rx_refill_mid A/B")

`settled.sh` now holds `rx_refill_mid` at 0 in its xeu table (what
Release 15 loads with, so the setting is unchanged; an xeu without the
parameter, tardis1's, is not held to it), writes it at `apply` and takes
a run's value as `EXP_XEU_rx_refill_mid`.  `run446.sh` from t2-r446:
rings of 128 at `rx_refill_mid` 0, 16 and 32, three runs each in a
rotated order, and rings of 256 at 0 twice beside them, the held mark on
in every run.  What counts: ring drops and retransmits, the rate, the
region's exact peak, and the ring CPUs' busy share (the doorbell writes'
cost).  A run in which twoport placed no flows on a port does not count
(t2-r440); the line says how many it placed.

### What it found (t2-r446 to r456)

| arm | runs | GB/s (mean) | ring drops | retransmits | region peak (mean, range) | ring CPUs' softirq |
|---|---|---|---|---|---|---|
| 128, mid 0 | r446 r451 r456 | 71.54 (71.19 to 72.11) | 1,479 to 2,269 | 1,547 to 2,360 | 11,528 (10,974 to 12,599) | 20.4, 25.4, 29.3% |
| 128, mid 16 | r447 r452 r454 | 71.68 (71.61 to 71.78) | 0 | 26 to 44 | 12,198 (11,558 to 12,573) | 23.2, 27.5, 28.9% |
| 128, mid 32 | r448 r450 r455 | 71.53 (71.35 to 71.72) | 0 | 25 to 26 | 11,422 (9,561 to 12,995) | 27.8, 26.4, 28.4% |
| 256, mid 0 | r449 r453 | 70.53 (70.26, 70.79) | 0 | 26, 47 | 10,339 (9,455, 11,222) | 27.5, 28.6% |

Every run placed its twelve flows a port, no kernel warning, no
allocation failure; the setting at rest afterwards, `rx_refill_mid` 0.
The softirq share is the mean of the eight receive CPUs' `%soft` in
`srv-mpstat.txt`, in run order within each arm.

- **The mid-poll refill takes the drops away on rings of 128**: none in
  any of six runs at 16 or 32, against 1,479 to 2,269 at 0, and the
  client's retransmits from about 1,900 to 25 to 44, as low as rings of
  256.
- **At no cost in rate**: 71.68 at 16 and 71.53 at 32 against 71.54 at
  0; rings of 256 are again about 1 GB/s lower (70.53).
- **The region's peak is not raised at 32**: 11,422 against 11,528 at 0.
  At 16 it is 670 higher, inside a spread of 1,600 to 3,400 between runs
  of one arm, so not shown either way.
- **The rings' cost is not seen**: the receive CPUs' softirq share rose
  through the session in every arm (20 to 29% from the first run to the
  last), and within that drift the arms do not separate; the extra
  doorbells cost at most a point or two.

So `rx_refill_mid` 32 does what rings of 256 did for the drops without
their lower rate or the region they hold.  Keeping it is a change to
`settled.sh` (its xeu table, 0 now), Mike's call.

## Where the mid-poll refill stops working (planned; Mike, 11 November: "run 48 and 64 to find the edge")

16 and 32 both dropped nothing, so the choice between them waits on
where drops come back.  `run446.sh` with `ARMS="128:48 128:64 128:32
128:64 128:48 128:32 128:48 128:64"` from t2-r457: three runs each at 48
and 64, two at 32 to tie the set to the last one.  At 64 a full poll of
64 frames refills about once, near its end, so it should be close to 0.
