# Round 146: the round-robin choice without the list walk

Mike, 19 November: "yes, do the round-robin walk as round 146", from
round 144's perf c2c: the round-robin choice took 7.6% of the client's
HITM loads (`xprt_switch_find_next_entry` 3.4%,
`xprt_iter_next_entry_roundrobin` 2.6%, `xprt_iter_get_next` 1.6%),
walking the switch's list from its head to the cursor and reading each
transport's line, which every task writes.

`f5c0543b9f377` "SUNRPC: Find the round-robin cursor's place without
walking the list" (W=1 clean on the client): the switch keeps its
transports in list order in an RCU array, rebuilt under `xps_lock` at
each change to the list; the choice finds the cursor in it and reads
only the transports it may choose.  The same choice as before; with no
array (empty, or its allocation failed) the list walk as before.  The
cursor itself, `cl_xpi`, is still written by every choice.

`ab-rr.sh`: B (the client's `updates/` stack, round 145's) against P,
4 KiB random READs at iodepth 96, B P B P B P (t2-r1173 to r1178),
round 144's perf c2c on P (r1179), then the WRITE and READ passes on P
(r1180, r1181).

## The less loaded of two, in place of round-robin

Mike, 19 November, while the A/B above ran its first B (t2-r1173):
"round robin is a shit algorithm, try using metric such as least
loaded", then "yes, if you have queue length, use it".  The chain was
stopped after t2-r1173 (its script killed, the run left to finish), and
P is now the tip with `031dead35013f` "SUNRPC: Give each new task the
less loaded of two transports" on `f5c0543b9f377`: each task gets the
one with the smaller `queuelen` (the tasks bound to it) of two active
transports picked at random from the switch's array.  Comparing all
twelve would read every transport's `queuelen`, a line every task
writes, twelve HITMs a choice; two choices read two and keep the most
loaded transport near the average.  The shared cursor is no longer
written.  W=1 clean on the client (`/root/zc-ckern/LL`, sunrpc
`0914657C2DBA58D0D84B792`).

`ab-rr.sh <PID>`: waits for t2-r1173's step, then P B P B P (t2-r1174
to r1178), perf c2c on P (r1179), the WRITE and READ passes on P
(r1180, r1181).

**t2-r1180, P's WRITE pass: 75.76 GB/s, RERUN** (port a 37.09 GB/s,
p99 228 ms; port b 38.67, 98 ms; round 145's t2-r1171 39.05 / 38.92 at
81 / 84 ms).  `write-again.sh`, after round 147's run: P's second WRITE
pass (t2-r1183), then B's (t2-r1184) on drives in the same state.

## The runs (08:53 to 09:34), and the less loaded of two dropped

| Run | Arm | 4 KiB IOPS | Port a / b (M) | p99 a / b (ms) | Client us a READ |
|---|---|---|---|---|---|
| t2-r1173 | B | 3.49 M | 1.68 / 1.81 | 4.55 / 4.05 | 12.65 (files written first, the chain stopped and restarted round it) |
| t2-r1174 | P | 3.62 M | 1.85 / 1.76 | 3.23 / 3.26 | 11.27 |
| t2-r1175 | B | 3.32 M | 1.73 / 1.59 | 5.08 / 6.72 | 11.57 |
| t2-r1176 | P | 3.60 M | 1.86 / 1.74 | 3.10 / 3.36 | 11.07 |
| t2-r1177 | B | 3.62 M | 1.86 / 1.76 | 3.13 / 3.36 | 11.27 |
| t2-r1178 | P | 3.35 M | 1.82 / 1.53 | 3.59 / 18.22 | 11.46 |

- 4 KiB READs: the same on both arms with even drives (3.60 to 3.62 M);
  the lows on each are a port's drives.  Identical READs are spread
  evenly by round-robin already, so there is nothing for a load metric
  to correct.
- perf c2c on P (t2-r1179, resolved against the running kernel's
  symbols): the choice's HITMs 2,220 (5.0%) against round-robin's 3,547
  (7.6%), cross-socket HITMs 322 against 670.  Two picks still read two
  `queuelen` lines every task writes.
- READ pass t2-r1181 **81.87 GB/s PASS**.
- **WRITE pass below the mark twice**: t2-r1180 **75.76 GB/s** (port a
  37.09 at p99 228 ms), t2-r1183 **77.45** (38.73 / 38.72 at 86.5 ms).
  B's WRITE pass on the same drives right after, t2-r1184: **77.82
  PASS** (38.87 / 38.95 at p99 81 / 82 ms).  A regression: round-robin hands identical 1 MiB WRITEs to the
  connections in strict turn, and two random picks only even them out
  on average, so some run deeper and the WRITEs wait on the deepest.

**`031dead35013f` dropped from the history** (Mike: "rewrite history
to just not include the code change"): the docs commits after it
replayed on `10fcb1c620fff`, the code that of `f5c0543b9f377` (the
array, under it, stays: the same round-robin choice found without the
walk), the tip before kept as `kernel-7.3/canary.pre-drop-leastloaded`,
the patch `patches/dropped/0028`.  The array has not been measured on
its own; it was only ever under the less loaded of two.

The lesson: **a balancing rule for nconnect is judged on WRITEs**, where
a connection that runs deep holds up the whole stream; strict turns are
hard to beat for tasks of one size, and a load metric earns its place
only where sizes or connections differ.
