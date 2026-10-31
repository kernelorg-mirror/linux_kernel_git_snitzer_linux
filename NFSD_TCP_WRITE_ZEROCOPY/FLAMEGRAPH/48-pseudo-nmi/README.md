# Round 48: a profile that sees hard interrupts (pseudo-NMI)

**The ask (Mike, 31 October):** without pseudo-NMI, perf cannot sample with interrupts off: a sample lands where they come back on. Round 47 timed about 20 us a WRITE of hard interrupts on nfsd's CPUs (NVMe 13, IPIs 6.6) that no profile has ranked. This round takes a profile with `irqchip.gicv3_pseudo_nmi=1`, a look and not a change to keep: pseudo-NMI adds its own cost to masking interrupts, so take shares from this boot and run figures from a boot without it.

**The boot (31 October, Mike rebooting).** Added on tardis2 to `/etc/kernel/cmdline`, for kernels installed from now on, and to build 505's own entry, `/boot/loader/entries/ofp-rocky-7.3.0-7.hs.505.loanpages_pool.el10.aarch64+16k.conf`, since the file is read only at install. The two as they were are kept here (`boot-entry-505-before.conf`, `etc-kernel-cmdline-before`); `settled.sh` does not audit the parameter.

**After the boot:**

1. Check: `grep gicv3_pseudo_nmi /proc/cmdline`, and dmesg for `Pseudo-NMI` being used (`GICv3: Pseudo-NMIs enabled`).
2. `for n in 0 1 2 3 4 5 6 7; do mount -o noatime,lazytime /dev/nvme${n}n1 /hs/nvme$n; done`, `systemctl start nfs-server`, `./bringup.sh 32768 256`, `./settled.sh apply`, which must end in "the settled setting". Check that no NFS client is connected first.
3. `./flame.sh $L/t2-fl48 fl48 64`, the same run as rounds 46 and 47's, and read `top-nfsd.txt` and `top-rings.txt` against `../47-write-path/fl47/`. `_raw_spin_unlock_irqrestore` should fall away and `nvme_irq`, the completion path and the IPI's handlers appear. If the cycles a WRITE differ much from fl47's 292.2k, say so: it is pseudo-NMI's own cost.
4. `xeu` has no symbols in `perf script` (`[[xeu]]`), so read its frames with the build's module.

**To take it out:** take the parameter off both files and reboot.

## The run

tardis2, build 505, sunrpc `666175BED7A343E376C1742`, one-port loan64 at the settled setting; `GICv3: Pseudo-NMIs enabled using relaxed ICC_PMR_EL1 synchronisation`, and the PMU `using NMIs`. Copy was not run. [fl48/](fl48/) has the reports and flamegraphs, [irqs.py](irqs.py) the breakdown by interrupt; `perf.data` is in `$L/t2-fl48/fl48-flame/`. All eight drives were found at boot.

| | GB/s | ring cycles (instructions) a WRITE | nfsd cycles (instructions) a WRITE | peak | stepped down, stalled |
|---|---|---|---|---|---|
| fl47, no pseudo-NMI | 40.49 | 128.8k (283.5k) | 292.2k (306.6k) | | no |
| fl48, pseudo-NMI | 40.40 | 129.7k (282.6k) | **297.2k (305.8k)** | 6095 | no |

**Pseudo-NMI's own cost, from one profiled run a side: nfsd +5.0k cycles a WRITE (+1.7%), the rings +0.9k (+0.7%), instructions the same.** Loans as usual: admitted 1538656, denied 1543445, overrun 0. Two `perf: interrupt took too long` lines lowered `perf_event_max_sample_rate` to 63800, far above the 999 Hz used.

**Read this round in cycles, not in `top-*.txt`'s microseconds.** Those are shares of mpstat's busy time, which has no IRQ accounting and is tick-sampled, and 12.9% of the stacks on nfsd's CPUs are now interrupts of the idle task. Below, stacks' weights are divided by the WRITEs in the 8 s profile (308.4k); they add to 296k cycles a WRITE against 297.2k counted, and fl47's to 289.6k against 292.2k. The clock measures 2.0 GHz.

## What the profile saw that the others could not

**`_raw_spin_unlock_irqrestore` fell away as round 46 predicted:** 9.89k to 0.80k cycles a WRITE, `_raw_spin_unlock_irq` 2.06k to 0.14k, and `finish_task_switch` 7.07k to 0.37k on threads, plus about 15k on the idle task. The handlers appear, and about 20k cycles a WRITE of frames that were never in a profile: `__page_pool_rx_region_put`, `update_sg_lb_stats`, `flush_workqueue_prep_pwqs`, `__smp_call_single_queue`, the scheduler's accounting.

**The cycles of nfsd's CPUs, a WRITE (fl48):**

| | k cycles | share |
|---|---|---|
| threads | 199.9 | 67% |
| hard interrupts (`gic_handle_irq`) | 27.8 | 9.4% |
| the idle task with no interrupt | 69.2 | 23% |
| of it `cpu_do_idle`, the wait itself | 52.7 | |

**Hard interrupts are 27.8k cycles a WRITE, about 13.9 us**, against round 47's 20.6 us timed with its probes inside the path. 96% of them land on an idle CPU.

| interrupt | k cycles a WRITE | a WRITE (round 47) | k cycles each |
|---|---|---|---|
| `nvme_irq` | 11.41 | 4.07 | 2.8 |
| `ipi_handler`, nearly all `sched_ttwu_pending` (4.92) | 6.50 | 3.94 | 1.65 |
| the timer | 2.15 | 0.12 | |
| the GIC's entry, lookup and EOI, not under a handler | 7.73 | about 8.1 | about 0.95 |

- **Why they were invisible, beyond the masking:** in earlier profiles a sample in an interrupt of the idle task landed after it, on an idle stack, and `flame.sh` drops idle stacks. fl47's idle stacks are 99k cycles, fl48's 69k with 28k of interrupts. So every earlier nfsd ranking left these out altogether, rather than misplacing them.
- **`nvme_irq` is 11.41k:** `blk_mq_end_request_batch` about 8.5k, of it `iomap_dio_bio_end_io` 3.2k, nearly all the wake of the thread in `blk_io_schedule`; `nvme_poll_cq` 1.3k; `nvme_unmap_data` 1.0k.
- **Pseudo-NMI still has a blind spot.** 76% of `__gic_handle_irq_from_irqson`'s self cost is the instruction after `msr DAIFClr, #0x3`, where the handler first opens to the PMU's NMI, so what is charged there is exception entry and the acknowledge, taken with everything masked. It is part of the 7.73k. How much is the read of the interrupt number and how much is pseudo-NMI's masking is not separated.

**Sleeping and waking, read whole, is the largest single thing on nfsd's CPUs:** `__schedule` 23.1k, `try_to_wake_up` 8.1k, the IPIs that deliver wakes 6.5k: about 37.7k cycles a WRITE, 13% of the 297.2k, at 11.4 context switches a WRITE. Round 46 had it at 1.91 + 2.97 us. Of it, not ranked by any round: `sched_balance_newidle` 3.81k, a CPU going idle looking for work to pull, and `psi_task_switch` 2.02k.

**Other paths with interrupts off, now in their place:**

| path | k cycles a WRITE, fl47 to fl48 | |
|---|---|---|
| `page_pool_return_netmem_bulk` | 0.63 to 2.98 | the region's return under its lock, of it `__page_pool_rx_region_put` 1.98 self |
| `page_pool_recycle_ring_bulk` | 3.92 to 4.23 | the same path whole: the cost did not move, only where it shows |
| `dma_pool_alloc` + `dma_pool_free` | 0.04 to 1.52 | NVMe's SGL lists, under the pool's lock, on both the submit and the completion |
| `__flush_workqueue` under `xlog_cil_push_now` | 1.89 to 2.34 | a COMMIT's log force; `nfsd3_proc_commit` is 8.53 in all |

## The rings

129.7k cycles a WRITE, and the picture is the one known: `net_rx_action` 106.0k, `gro_receive_skb` 66.3k, `tcp_v4_rcv` 25.4k. `handle_softirqs` self fell from 2.07 to 0.23 us, the samples that were waiting for it to turn interrupts on. Hard interrupts are 5.88k (4.5%): the timer 3.22k and the GIC's entry 2.65k, nearly all on an idle ring CPU; `xeu`'s own interrupt handler is about 0.01k.

**The timer on the rings is 3.22k cycles a WRITE, and `hrtimer_start` another 1.98k**, about 4% of the rings. The callbacks are small (`napi_watchdog` 0.12k, the tick 0.07k); the time is in `hrtimer_interrupt` programming the next event and reading the clock. This fits the deferral's timer (`DEFER` 2/10000, round 44), whose saving is net of it, but expiries were not counted, so that it is the deferral's is a reading.

**`xeu`, step 4:** perf left the frames unnamed, so the sampled addresses were resolved through `/proc/kallsyms` (no reload since the run; `perf.data`'s addresses fall in the live module). 15.29k cycles a WRITE on the rings, 11.8%: `xeu_rx_poll` 11.01k (72%), `xeu_rx_refill` 3.73k (24%), and 0.47k charged to `xeu_driver_exit`, kallsyms' nearest symbol to a function it does not name. Nothing else over 0.03k.

## Where it stands

A look; nothing changed, nothing is settled. **The boot parameter is still on**, in `/etc/kernel/cmdline` and build 505's entry: run figures want a boot without it. What to take up from here is Mike's.
