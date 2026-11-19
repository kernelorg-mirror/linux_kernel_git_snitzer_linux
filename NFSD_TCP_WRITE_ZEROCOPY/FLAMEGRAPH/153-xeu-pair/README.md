# Round 153: one NAPI context a ring pair, and eight smaller rings

Mike, 19 November: "but do start on 'merging TX completion into the RX
NAPI' ... to try that as yet another patch to the xeu driver", "and
just for IOPS config, maybe we try 8 rings that are smaller in size so
as not to expand overall pool region size?", then "yes, do the reloads
and run round 153".

**The patch:** kernel-rpmtree `081f549`, Patch16
`xeu-reap-a-TX-ring-s-completions-in-its-RX-ring-s-poll.patch`, kmod-xsight
Release 18: with `tx_in_rx_napi` Y (read when a port opens; needs
`irq_msix_mask`), RX ring i's poll reaps TX ring i's completions and TX
ring i's interrupt schedules that poll.  Built against build 541 with
no warning in `xeu_drv.c` at W=1; srcversion `D8F04765830566CCA26AE4B`
(Release 17 is `4FE2E7FF217385CDB5D4BB2`).

**The A/B, `ab-pair.sh`:** Release 18 put in `updates/` and loaded once
with `num_rings=8`; the arms only reopen the ports.

| arm | rings a port | `tx_in_rx_napi` | vectors |
|---|---|---|---|
| B | 4 x 256 | N | Rx-i on c0 + 2i, Tx-i on c0 + 2i + 1 (settled) |
| R8 | 8 x 128 | N | Rx-i and Tx-i on c0 + i |
| P | 8 x 128 | Y | Rx-i and Tx-i on c0 + i, one NAPI context |

R8 and P post the receive buffers B does.  Each run is round 151's
`packets.sh` (IOPS at iodepth 96 with the per-READ counts): B R8 P B P
R8, t2-r1203 to r1208 (t2-r1202, the first B, was refused by the
audit: `xeu_release` read rpm's "not owned" as a release; fixed), and 1 s of bpftrace on the second P run counting
xeu's polls.  Then Release 17 back at its `num_rings=4`.

**IOPS only (Mike, 19 November: "these additional tunings should only
be used for the IOPS pass at this point, so don't change READ and WRITE
passes.  So that implies testing IOPS requires focused xeu driver
reload").**  Release 18, 8 rings and `tx_in_rx_napi` are loaded only
for IOPS runs, by a reload of xeu, and taken out after; the WRITE and
READ passes stay on the settled Release 17 at 4 rings, which this
round does not change, so it runs no pass.

The rig: `settled.sh` holds `tx_in_rx_napi` N (not held on an xeu
without it), audits more than four rings at ring-irqs.sh's layout, and
takes `EXP_XEU_RELEASE` for an xeu not installed from its RPM;
`port-setup.sh` takes `EXP_RINGS`.

## The first runs, and the change of plan (19 November, evening)

| run | arm | IOPS | a / b M | p99 ms a / b | server us a READ | client us a READ |
|---|---|---|---|---|---|---|
| `t2-r1202` | B | refused by the audit (`xeu_release`, fixed) | | | | |
| `t2-r1203` | B | 3.36 M | 1.53 / 1.83 | 8.36 / 5.08 | 16.0 | 11.6 |
| `t2-r1204` | R8 | (below) | | | | |

t2-r1203's B was no control (Mike: "B is the control run?  meaning no
change, just using settled settings"): Release 18 loaded at
`num_rings=8` and cut to 4 rings, and port a carried 1.53 M against port
b's 1.83 M (Mike: "please fix the rings.. cannot have such imbalance").
In t2-r1204 (8 rings) every ring of both ports took 159K to 215K frames
a second, port a 1.54 M and port b 1.61 M: the 128 entries `ethtool -x`
shows are only part of the table the card uses (they name rings 0 to 3
only, and rings 4 to 7 receive all the same), so the table is not
written with `ethtool -X`.

`ab-pair2.sh` continues: P R8 P on this load (t2-r1205 to r1207), then
Release 17 back at `num_rings=4` and B twice on the plain settled
setting (t2-r1208, r1209).  `packets.sh` now prints each server ring's
frames a second, so every run shows the spread over rings and ports.

**And t2-r1204 is no fair R8 either** (3.55 M, 1.71 / 1.84 M, server
91% busy, 16.4 us a READ): `twoport.sh` gives every ring 128 entries for
a load and puts them back to 256 after, so the settled IOPS load has
4 x 128 a port, and R8 posted 8 x 128, twice the receive buffers.
`ab-pair2.sh` was stopped before its first run and now runs R8 P R8 P at
8 x 64 a port (t2-r1205 to r1208), then B twice on Release 17
(t2-r1209, r1210).

## Result: neither 8 rings nor one NAPI context a pair saves CPU

`ab-pair2.sh`, 19:25 to 19:50.  R8 and P at 8 x 64 a port on Release 18
(`num_rings=8`), B on Release 17 at the settled setting.  No new kernel
warning; no ring logged "tx_in_rx_napi not used".

| run | arm | IOPS | a / b M | p99 ms a / b | server busy | server us a READ | client us a READ | NET_RX ring CPUs | NET_RX nfsd CPUs |
|---|---|---|---|---|---|---|---|---|---|
| `t2-r1205` | R8 | 3.64 M | 1.78 / 1.86 | 3.10 / 3.13 | 90% | 15.82 | 11.21 | 0.541 | 0.455 |
| `t2-r1206` | P | 3.59 M | 1.76 / 1.83 | 3.33 / 3.29 | 89% | 15.87 | 11.23 | 0.345 | 0.459 |
| `t2-r1207` | R8 | 3.51 M | 1.77 / 1.74 | 4.75 / 6.13 | 85% | 15.50 | 11.21 | 0.595 | 0.499 |
| `t2-r1208` | P | 3.40 M | 1.53 / 1.87 | 6.59 / 4.62 | 83% | 15.62 | 11.44 | 0.384 | 0.466 |
| `t2-r1209` | B | 3.58 M | 1.85 / 1.73 | 3.06 / 3.26 | 88% | 15.73 | 11.26 | 0.340 | 0.465 |
| `t2-r1210` | B | 3.59 M | 1.84 / 1.75 | 3.13 / 3.16 | 89% | 15.87 | 11.10 | 0.352 | 0.485 |

Means: B 15.80 us a READ at 3.59 M, R8 15.66 at 3.58 M, P 15.75 at
3.50 M.  Every run is within 15.50 to 15.87: no arm differs from B by
more than runs of one arm differ from each other.

- **P works as designed:** in 1 s of bpftrace on t2-r1208, `xeu_rx_poll`
  ran 1.57 M times and `xeu_tx_poll` never; the TX interrupts ran the RX
  poll 255 K times.  It halves R8's ring-CPU softirqs (0.35 against
  0.54 to 0.60 a READ), back to B's, and saves no CPU: the polls it
  removes are cheap.  The RFS hop (0.46 to 0.50 a READ) is the same in
  every arm.
- **The rings and ports on B are even:** every ring 384K to 443K frames
  a second, the ports 1.84 / 1.75 M.  The two runs with port a behind
  (t2-r1203, 1.53 M; t2-r1208, 1.53 M with rings 2 and 5 at 121K) were
  on Release 18; in t2-r1208 the ring CPUs were 84 to 100% busy, about
  half of it nfsd's work spilled onto them (`svc_rtc_spill_cpus`
  0-7,56-63): round 154 tries no spill.
- So xeu Patch16 (`tx_in_rx_napi`) and 8 rings are not kept for IOPS;
  Release 17 is back at `num_rings=4`, the settled setting.

**Dropped (Mike, 19 November: "yes, drop Patch16 by rewriting
kernel-rpmtree history"):** kernel-rpmtree's `081f549` (never pushed) is
off `main`, which is back at `c638b62`, Release 17; the commit is kept
on `main.pre-drop-xeu-pair` and as `patches/dropped/0029`.
