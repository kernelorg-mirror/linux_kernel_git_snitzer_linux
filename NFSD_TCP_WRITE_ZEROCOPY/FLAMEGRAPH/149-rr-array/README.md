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

## The runs (09:40 to 10:04): kept

| Pair | B IOPS | P IOPS | B client us | P client us | B server us | P server us |
|---|---|---|---|---|---|---|
| t2-r1191 / r1192 | 3.55 M | 3.43 M | 11.36 | 11.34 | 15.86 | 15.67 |
| t2-r1193 / r1194 | 3.67 M | 3.54 M | 11.25 | 11.25 | 15.52 | 16.27 |
| t2-r1195 / r1196 | 3.60 M | 3.51 M | 11.20 | 11.35 | 15.82 | 16.23 |
| mean | 3.61 M | 3.49 M | 11.27 | 11.31 | 15.73 | 16.06 |

- The client's CPU a READ the same; P 3% lower in IOPS in each pair and
  0.3 us more of the server's CPU a READ, within a run's spread and not
  explained (the choice is meant to be round-robin's).
- WRITE pass t2-r1197 **77.86 GB/s PASS**, no drive stepped down, no
  new kernel warning; READ pass t2-r1198 **81.83 GB/s PASS**.

**Kept** (Mike: "just keep it, in general its a small win"), and **in
the client's `updates/` since 19 November** (Mike: "yes, put it in the
client's updates/"): the 17 modules built from `f5c0543b9f377` (sunrpc
`1EFC577AC45E5EEF6EC1592`), `depmod -a`; the stack before
(`bdf70175fc522`, round 145's) kept in the client's
`/root/zc-ckern/updates-bdf70175fc522/`.  From here B is this stack.
