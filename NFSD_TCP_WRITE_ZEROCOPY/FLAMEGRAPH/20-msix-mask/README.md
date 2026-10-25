# Round 20: hold a ring's interrupt off at the card, not through the ITS

**What was profiled:** fl20-loan64, build 499 at the settled setting (`drc_max_entries` 4096), kmod-xsight 2.0.1-8.
- The rings cost 62.85 µs a WRITE. The most costly frame was `_raw_spin_unlock_irqrestore`, 11.51 µs a WRITE, of which 10.09 is under `__napi_poll;xeu_rx_poll;enable_irq`. That is skid: the time spent with interrupts off while the irq core re-arms the line, with no pseudo-NMI to sample it.
- **Why the re-arm costs that much.** xeu's handler calls `disable_irq_nosync()` and its poll calls `enable_irq()` when `napi_complete_done()` says the poll is over. The disable is lazy, so an interrupt that comes while the ring polls is masked then: on ITS-PCI-MSIX that is an LPI config change with an ITS INV and SYNC. `enable_irq()` unmasks it the same way and resends the held interrupt with a third command (`its_irq_retrigger`), each under the descriptor's lock with interrupts off.
- Counted with [`rearm.bt`](rearm.bt) (run by [`r20count.sh`](r20count.sh)) on one run, for port a's four Rx vectors, per WRITE: 5.11 `enable_irq`, 8.62 hard interrupts, 3.51 ITS masks, 3.53 unmasks and 3.53 resends ([cost.txt](cost.txt)). That is about 10.6 ITS commands a WRITE.
- **What was ruled out.** The E1's firmware interface has no interrupt moderation or mask register for the driver. Masking eagerly (`IRQ_DISABLE_UNLAZY`) would still cost an ITS mask and unmask each poll. The kernel's deferral (`napi_defer_hard_irqs`, `gro_flush_timeout`) hangs xeu and is not set on this port. `ethtool -C` is not supported.

**The change:** kmod-xsight 2.0.1-9, "xeu: hold a ring's interrupt off at the card while it polls" (kernel-rpmtree `2013f7e`, Patch9, on Release 8's affinity fix). Mike agreed to change Xsight's driver for this round.
- A new parameter, `xeu.irq_msix_mask` (default N), read when a port opens.
- With it set, a ring whose interrupt is an MSI-X entry the kernel can mask records its entry's vector control word when the irq is requested. It then sets the entry's mask bit where it called `disable_irq_nosync()` and clears it where it called `enable_irq()`, in both the Rx and Tx polls. That is one posted MMIO write each way.
- An interrupt that comes while the entry is masked is held pending by the card and sent on the unmask, so nothing is resent and the ITS is not touched.
- The kernel's cached copy of the control word is not changed. It still unmasks the entry when the irq is requested and masks it when it is freed.
- `settled.sh` holds it at N until this round is decided. `apply` writes the value a run asks for (`EXP_XEU_irq_msix_mask`) and opens the ports again for it, as for `rx_region_stack`. A value written by hand without an open is not what the rings run, and the audit cannot tell.

**Runs 2026-10-25 on tardis2**, build 499 (the tip, `bca1a2f143eb`), on the kernel's own sunrpc (`EFD53C53F61C1F0CDF37D89`) and nfsd (`3F0C341E86E266D5FDF2ADF`), xeu 2.0.1-9 (`E6525B5F7DA25C216AE0DCD`). The driver is `/var/tmp/zc-run-logs/t2-tipkernel/run40.sh`, the runs `/var/tmp/zc-run-logs/t2-r40/`.
- A is N, the settled setting; B is Y. Before every run, on both sides, the other side's value was written and `settled.sh apply` put this side's in place and opened port a again.
- xeu was reloaded once for 2.0.1-9, with no NFS client connected. After the reload `bringup.sh` stops because the region already exists this boot; port a was set up with `port-setup.sh enp161s0f0 0` and settled.

## Outcome: the rings 10.0 µs a WRITE cheaper (-17%); nothing else moved

| run | irq_msix_mask | GB/s | ring µs a WRITE | nfsd µs a WRITE | Rx interrupts a WRITE | Tx interrupts a WRITE | region peak in use, pages |
|---|---|---|---|---|---|---|---|
| r40-1-loan64-A | N | 40.50 | 61.2 | 110.6 | 4.66 | 5.62 | 6569 |
| r40-2-loan64-B | Y | 40.51 | 49.0 | 111.0 | 9.23 | 5.85 | 6462 |
| r40-3-loan64-A | N | 40.49 | 59.0 | 112.0 | 4.69 | 5.77 | 6332 |
| r40-4-loan64-B | Y | 40.19 | 51.6 | 111.1 | 8.89 | 5.51 | 6481 |
| r40-5-loan64-A | N | 40.53 | 60.2 | 111.1 | 4.58 | 5.83 | 6494 |
| r40-6-loan64-B | Y | 40.51 | 50.7 | 112.6 | 9.27 | 5.67 | 6412 |
| r40-7-loan64-A | N | 40.43 | 59.4 | 110.3 | 4.66 | 5.91 | 6410 |
| r40-8-loan64-B | Y | 40.46 | 48.6 | 111.9 | 9.28 | 5.64 | 6369 |

Every run was untraced, settled and DRIVE-LIMITED, with no drive stepped down ([cost.txt](cost.txt); the interrupts are port a's, from `/proc/interrupts` around each run).

| | rings µs a WRITE | nfsd µs a WRITE | region peak, pages |
|---|---|---|---|
| A, 4 runs | 59.95 ± 0.84 | 111.00 ± 0.64 | about 6450 |
| B, 4 runs | 49.98 ± 1.23 | 111.65 ± 0.65 | about 6430 |
| B - A | **-9.97 ± 0.74** | +0.65 ± 0.46 | the same |

- **The rings:** every B run (48.6 to 51.6) is below every A run (59.0 to 61.2). The saving is the whole of what `enable_irq` cost in round 20's profile.
- **The region:** no change, so the rule against growing its footprint is met. The change holds no page longer.
- **Not changed:** nfsd, the rate (40.2 to 40.5 GB/s), the hand-offs (5.4 to 5.6 enqueues and 4.3 to 4.4 wakes a WRITE), refused loans, the Tx interrupts, latency (below).
- **The ring takes twice as many interrupts:** 9.2 a WRITE against 4.65. The card sends the next interrupt as soon as the entry is unmasked, instead of after the ITS has unmasked the line, so polls are more and smaller. Each costs much less than an ITS round trip: `handle_softirqs` self goes from 1.14 to 2.94 µs a WRITE, a part of the saving given back.
- The count run with Y (`msixY`): 0 `enable_irq`, 4 ITS masks and 4 unmasks in the whole run (the port's open and close), no resends, 11.4 hard interrupts a WRITE.

## The profile

fl40-loan64 with Y, against fl20-loan64 with N (the same kernel and setting, kmod 2.0.1-8, whose N path is the same code), µs a WRITE ([top-rings.txt](top-rings.txt), [top-nfsd.txt](top-nfsd.txt)):

| | fl20 (N) | fl40 (Y) |
|---|---|---|
| the rings, busy | 62.9 | 51.6 |
| every stack through `enable_irq` | 16.5% (10.4) | 0 |
| `_raw_spin_unlock_irqrestore` | 11.51 | 1.35 |
| `handle_softirqs` | 1.14 | 2.94 |
| `eth_type_trans` | 5.04 | 4.34 |
| `page_pool_alloc_frag_netmem` | 4.18 | 3.91 |
| nfsd, busy | 113.7 | 112.4 |

- The next frames on the rings are the receive itself: `eth_type_trans` (the first touch of each frame's header), the page pool, `xeu_rx_poll` and GRO.

## Latency

[latency.txt](latency.txt), untraced runs:

| | p50 | p90 | p99 | mean |
|---|---|---|---|---|
| B1, Y | 0.424 ms | 0.494 | 0.561 | 0.432 |
| A1, N | 0.416 | 0.489 | 0.569 | 0.427 |
| B2, Y | 0.420 | 0.494 | 0.586 | 0.431 |
| A2, N | 0.444 | 0.528 | 0.618 | 0.458 |

- No difference: the interrupt comes back sooner, but a WRITE's latency is the drive's.

## Not yet run

- Copy mode on two ports with Y. tardis2 runs one port; Y's saving is per poll, so it should not be lost in copy mode, but that is not measured.
- Whether `irq_msix_mask` should become xeu's only way (and the parameter go) is for Xsight, with this round as the case.

## Decision

- Mike's to make: whether `settled.sh` holds `irq_msix_mask` at Y.
