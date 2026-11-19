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
