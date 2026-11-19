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
R8, t2-r1202 to r1207, and 1 s of bpftrace on the second P run counting
xeu's polls.  Then on P the WRITE pass (r1208) and READ pass (r1209), at
8 rings of 64 (the passes' settled 4 of 128 posted), and Release 17
back at its `num_rings=4`.

The rig: `settled.sh` holds `tx_in_rx_napi` N (not held on an xeu
without it), audits more than four rings at ring-irqs.sh's layout, and
takes `EXP_XEU_RELEASE` for an xeu not installed from its RPM;
`port-setup.sh` takes `EXP_RINGS`.
