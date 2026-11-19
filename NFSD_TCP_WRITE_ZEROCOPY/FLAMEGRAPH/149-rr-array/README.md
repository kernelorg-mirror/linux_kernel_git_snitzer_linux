# Round 149: the round-robin array on its own

Mike, 19 November: "run the A/B with commit f5c0543b9f377".  Round 146's
array (the switch's transports in an RCU array, the round-robin cursor's
place found there instead of by walking the list, the same choice) was
only ever measured under the less loaded of two, which was dropped for
its WRITE passes.  This is its own A/B, with the passes that decide
whether it stays.

`ab-array.sh`, after round 148: B (the client's `updates/` stack, round
145's) against P (`/root/zc-ckern/RR`, built from `f5c0543b9f377`),
4 KiB random READs at iodepth 96, B P B P B P (t2-r1191 to r1196), then
the WRITE and READ passes on P (r1197, r1198).
