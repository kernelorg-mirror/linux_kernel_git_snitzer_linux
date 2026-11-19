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
