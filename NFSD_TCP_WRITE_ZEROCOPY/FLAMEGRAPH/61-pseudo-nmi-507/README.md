# Round 61: build 507 at the two-port setting, seen with pseudo-NMI

**The plan (4 November, after round 60 was dropped):** round 57's
targets are used up (rounds 55, 58 and 59), the wakes are closed (round
40), the region's lock can go no further inside the footprint, and round
60 found sunrpc's own loan frames worth under 1% more. Every profile since
round 48 was taken without pseudo-NMI, so time with interrupts off is
charged to where they come back on (`_raw_spin_unlock_irqrestore`,
`finish_task_switch`), and interrupts taken on an idle CPU are dropped
with the idle stacks. Round 48 saw them once, on build 505 on one port:
hard interrupts 27.8k cycles a WRITE on nfsd's CPUs (`nvme_irq` 11.4k,
the IPIs 6.5k), sleeping and waking about 37.7k, and `dma_pool_alloc` and
`dma_pool_free` 1.5k. Build 507 differs: two ports, eight SGL drives,
rounds 51 to 58's changes. This round takes the same look on it, to rank
what is left before choosing a change. It is a look, not a change to keep:
pseudo-NMI costs about 1.7% of nfsd's cycles of its own (round 48), so take
the shares from this boot and run figures from a boot without it.

**The baseline without pseudo-NMI:** round 55's B profile on the same
kernel, `$L/t2-r172/fl-blk-flame/` (67.31 GB/s, 64344 WRITEs a second,
nfsd busy 88.79 us a WRITE, rings 31.89), and its counted runs in
[../55-nvme-sgl-walk/run172.out](../55-nvme-sgl-walk/run172.out).

## The boot (Mike)

Add `irqchip.gicv3_pseudo_nmi=1` to build 507's own entry,
`/boot/loader/entries/ofp-rocky-7.3.0-10.hs.507.loanpages_pool.el10.aarch64+16k.conf`
(its `options` line), and to `/etc/kernel/cmdline` too only if the next
kernel installed is also to boot with it. The two files as they are now
are here: [boot-entry-507-before.conf](boot-entry-507-before.conf) and
[etc-kernel-cmdline-before](etc-kernel-cmdline-before). `settled.sh` does
not audit the parameter. Then reboot.

**Done (4 November):** the parameter is on build 507's entry's `options`
line (`grubby --info=DEFAULT` shows it); `/etc/kernel/cmdline` is
unchanged, so a kernel installed later boots without it. Mike reboots.

## After the boot

1. `grep gicv3_pseudo_nmi /proc/cmdline`, dmesg shows
   `GICv3: Pseudo-NMIs enabled`, and the PMU is `using NMIs`.
2. The first ten minutes as on any boot (`../../xeu-rig/README.md`,
   section 1): kdump armed first (`./bringup.sh 32768 256`, which also
   mounts `/hs/nvme0` to `7`), `./settled.sh apply` ending in "the settled
   setting", nothing in `updates/` (`modinfo -n sunrpc nfsd`: `kernel/`;
   sunrpc `2A512644C685341146A3A9B`, nfsd `BB6D1949646AB051A8D99CC`), dmesg
   with no `WARNING:`, `BUG:` or `Oops`. No NFS client connected.
3. From `xeu-rig/` (`ab-switch.sh` refuses a pseudo-NMI boot, so by
   hand): the profile, the same run as ab-switch's B profile (two ports,
   budget 128), `WHICH=ab ./flame.sh $L/t2-fl61 fl61 128`. Then one
   counted loan run beside it, after `fstrim` of `/hs/nvme0` to `7` and
   120 s of rest, as ab-switch's `run()`: `env WARM=10 RUNTIME=30
   WRITES=128 ./twoport.sh $L/t2-r178 ab`, and `./cost.sh $L/t2-r178` for
   cycles and instructions a WRITE. If nfsd's cycles a WRITE differ from
   build 507's (about 271k) by much more than round 48's 1.7%, say so.
4. Read it in cycles a WRITE, as round 48 did (stack weights divided by the
   profile's WRITEs), not in `top-*.txt`'s microseconds. Against fl-blk:
   threads, hard interrupts (`gic_handle_irq`, by handler: `nvme_irq`, the
   IPIs, the timer, the GIC's own entry), and the idle task. Then sleeping
   and waking whole, the paths with interrupts off in their place (the
   region's return under its lock, `dma_pool_*` for the SGL lists), and on
   the rings the timer and `hrtimer_start` beside the deferral.
   [../48-pseudo-nmi/irqs.py](../48-pseudo-nmi/irqs.py) does the
   breakdown by interrupt.
5. The next target is chosen from that ranking (Mike).

**Found after the boot (4 November):** `/proc/cmdline` has
`irqchip.gicv3_pseudo_nmi=1`; dmesg `GICv3: Pseudo-NMIs enabled using
relaxed ICC_PMR_EL1 synchronisation`, the PMU `using NMIs`, and with it
`NMI watchdog: Enabled. Permanently consumes one hw-PMU counter` (6 of 7
left to perf; `kernel.nmi_watchdog` 1, left as it is). kdump was loaded at
boot (`kexec_crash_loaded` 1, the kernel's copy in `/var/lib/kdump/boot/`
from the boot before); `bringup.sh 32768 256` mounted `/hs/nvme0` to `7`,
started nfs-server and mapped both ports into the region, and `settled.sh
apply` ended in "the settled setting". srcversions as before: sunrpc
`2A512644C685341146A3A9B`, nfsd `BB6D1949646AB051A8D99CC`, xeu
`CA0931345E46C929B11D01F`; `updates/` does not exist. No NFS connection,
no WARNING, BUG or Oops.

**To take it out:** put the `options` line back as in
`boot-entry-507-before.conf` and reboot. Every A/B after this round runs
on a boot without the parameter.

## The runs

**fl61, the profile (4 November):** `WHICH=ab ./flame.sh $L/t2-fl61 fl61
128`, [run-fl61.out](run-fl61.out): 66.18 GB/s on both ports, 63173 WRITEs
a second in the profile, nfsd busy 88.35 us a WRITE, rings 29.40 (fl-blk:
67.31, 64344, 88.79, 31.89). Loans admitted 2503728, denied 344, overrun
0; the setting held at the end. Three `perf: interrupt took too long`
lines lowered `perf_event_max_sample_rate` to 51000, far above the 999 Hz
used. `perf.data` is in `$L/t2-fl61/fl61-flame/`.

**t2-r178, the counted run (4 November):** after `fstrim` of the eight and
120 s of rest, `env WARM=10 RUNTIME=30 WRITES=128 ./twoport.sh $L/t2-r178
ab`: **71.21 GB/s** on both ports (67806 WRITEs a second in the window), no
drive stepped down, none low in the warm-up, no stall; port b's drives
8.8 to 8.9 GB/s each, the busiest 90% busy, so twoport.sh marks it
`DRIVE-LIMITED`. Loans admitted 2700464, denied 84, overrun 0; client
retransmits 562; the region's pages in use at most 12124 (blk-2-B's
11152). `cost.sh`, port a's rings and nfsd's CPUs, counted:

| | GB/s | ring cycles (instructions) a WRITE | nfsd cycles (instructions) a WRITE |
|---|---|---|---|
| blk-1-A, no pseudo-NMI | 70.54 | 73.9k (144.6k) | 279.6k (278.2k) |
| blk-2-B | 70.01 | 72.8k (143.4k) | 275.9k (272.7k) |
| blk-3-B | 67.63 | 74.0k (146.8k) | 270.0k (272.8k) |
| blk-4-A | 65.63 | 73.7k (145.4k) | 269.5k (278.7k) |
| **t2-r178, pseudo-NMI** | **71.21** | **74.7k (144.0k)** | **285.1k (269.2k)** |

**Pseudo-NMI costs more here than round 48 found: nfsd +11.4k cycles a
WRITE against the four runs' mean of 273.8k (+4.1%; +5.5k, +2.0%, against
the highest of them), where round 48 had +5.0k (+1.7%); the rings +0.9k
(+1.2%).** Instructions are no more than theirs (269.2k against 268.9k to
278.7k), so it is time with the same work, as in round 48. One run a side;
the rate is the highest of the five, so it is not a slower run's idle
cost. (`cost.sh`'s ring is port a's four CPUs; the ab-switch lines' 144k
are both ports' eight.)

## The profile read in cycles a WRITE (step 4)

Stack weights divided by the WRITEs in the 8 s profile (fl61 505.4k, fl-blk
514.8k), idle stacks kept, by [rank.py](rank.py) (`perf script --cpu` into
`stackcollapse-perf.pl`, with nothing dropped); the reports are in
[fl61/](fl61/). An interrupt is `el1_interrupt` on the stack; the softirqs
it runs on its way out (`irq_exit_rcu`) come after `gic_handle_irq` has
returned and are counted apart from it. fl61's stacks add to 276.8k cycles
a WRITE on nfsd's CPUs, fl-blk's to 273.9k (it ran at 67.31 GB/s, fl61 at
66.18; the counted run is the one above).

**nfsd's CPUs (8 to 55), k cycles a WRITE:**

| | fl-blk, no pseudo-NMI | fl61 | fl48 (build 505, one port) |
|---|---|---|---|
| threads | 188.6 | **195.5** (70.6%) | 199.9 |
| hard interrupts | (hidden) | **25.1** (9.1%) | 27.8 |
| softirqs on an interrupt's exit | 0.5 | 0.9 | |
| the idle task, no interrupt | 84.9 | **55.3**, the wait itself 44.4 | 69.2, the wait 52.7 |

**Hard interrupts are 25.1k cycles a WRITE (about 12.6 us at 2.0 GHz), 98%
of them on an idle CPU**; round 48 had 27.8k:

| interrupt | fl61 | fl48 | what is in it (fl61) |
|---|---|---|---|
| `nvme_irq` | 10.45 | 11.41 | `blk_mq_end_request_batch` 8.29, of it `iomap_dio_bio_end_io` 2.91 (the wake of the thread); `nvme_poll_cq` 0.98, `nvme_unmap_data` 0.80, `dma_pool_free` 0.40 |
| the GIC's entry, not under a handler | 7.73 | 7.73 | `__gic_handle_irq_from_irqson` self 2.69: the blind spot of round 48, exception entry and acknowledge |
| IPIs | 5.72 | 6.50 | `sched_ttwu_pending` 4.43 |
| the timer | 1.23 | 2.15 | `hrtimer_interrupt` 1.16 |

**Sleeping and waking, whole: about 34.6k cycles a WRITE (12.5% of
nfsd's CPUs)**: `__schedule` 21.83k, `try_to_wake_up` 7.06k (2.90 of it in
interrupts), the IPIs 5.72k; round 48 had 37.7k. 9.96 context switches and
3.42 threads woken a WRITE in t2-r178 (blk-2-B 10.66 and 3.65). Of it, as in
round 48: `sched_balance_newidle` 3.65k, a CPU going idle looking for work
to pull, and `psi_task_switch` 2.11k.

**Found here, not ranked before: reading the clock is 6.13k cycles a
WRITE self (`arch_counter_get_cntpct`; fl-blk showed 1.18k, the rest
under interrupts off), of it PSI 1.55k, the run queue's clock 1.04k,
`sched_clock` 0.49k, nohz's tick on idle entry 0.76k, cpuidle 0.75k, TCP's
`tcp_mstamp_refresh` 0.54k, block's accounting 0.50k.** PSI whole
(every `psi_*` frame) is **3.24k cycles a WRITE, 1.2%** of nfsd's CPUs,
all of it with interrupts off and so missing from every profile before.
`/proc/pressure` is there: PSI is on in this kernel.

**The paths with interrupts off, in their place:**

| path | fl-blk to fl61, k cycles a WRITE | |
|---|---|---|
| `_raw_spin_unlock_irqrestore` | 10.16 to 1.03 | fell away, as in round 48; on the rings 7.15 to 0.98 |
| `_raw_spin_unlock_irq` | 2.20 to 0.14 | |
| `page_pool_return_netmem_bulk` | 0.39 to 3.42 | the region's return under its lock, `__page_pool_rx_region_put` 2.55 self (fl48 2.98 and 1.98, before round 58) |
| `page_pool_recycle_ring_bulk` | 5.29 to 4.70 | the same path whole (fl48 4.23) |
| `dma_pool_alloc` + `dma_pool_free` | 0.05 to 1.25 | NVMe's SGL lists, their lock with interrupts off 0.71 of it (fl48 1.52) |
| `blk_rq_dma_map_iter_next` | 6.74 to 6.63 | self 4.95; round 55's inlining took `blk_map_iter_next` into it |
| `__flush_workqueue` | 1.86 to 2.36 | a COMMIT's log force; `nfsd3_proc_commit` 8.49 in all |

**What is left on nfsd's threads, by self (k cycles a WRITE):**
`_raw_spin_lock` 6.98, of it 2.89 the qdisc's lock in `__dev_xmit_skb` for
the replies (fl-blk 2.94, so seen before); `arch_counter_get_cntpct` 6.13;
`blk_rq_dma_map_iter_next` 4.95; sunrpc's loan walk
(`svc_tcp_rx_append_ref` 4.57, `svc_tcp_rx_borrow_segment` 3.84,
`svc_tcp_rx_release_refs` 2.89, `svc_tcp_rx_walk_skb` 2.50,
`svc_tcp_rx_prefetch_pages` 2.27, `svc_tcp_rx_region_span` 1.54, about
17.6 together, round 60's ground); `__page_pool_rx_region_put` 2.55;
`napi_pp_put_page` 2.43. `queued_spin_lock_slowpath` is 1.69, the socket's
lock 0.46 and the pool's queue 0.36 of it. Inclusive: `svc_tcp_recvfrom`
62.1k, `nfsd3_proc_write` 51.3k, `svc_tcp_sendto` 26.1k, `tcp_send_ack`
from `tcp_cleanup_rbuf` 16.8k.

**The rings (both ports' eight CPUs), k cycles a WRITE:** 147.1 in all
(fl-blk 144.2): the softirqs 114.9 (`net_rx_action` 112.6,
`gro_receive_skb` 70.3, `tcp_v4_rcv` 29.7), hard interrupts 8.4 (the timer
4.79, of it `hrtimer_interrupt` 4.13; the GIC's entry 3.55; `xeu`'s own
0.07), the idle task's wait 21.0. `hrtimer_start_range_ns` is another
2.83k: with the timer about 7.6k, 5% of the rings, as round 48 found
(3.22 + 1.98 on one port's four CPUs) and with the deferral (`DEFER`
2/10000) still the likely owner; expiries still not counted.

## The ranking, for the next target (step 5, Mike's)

What this boot shows that is left on nfsd's CPUs, largest first, in k
cycles a WRITE (of 276.8):

1. **Sleeping and waking, 34.6** (the IPIs 5.7 and `nvme_irq`'s wakes in
   it). Known since round 46; round 40 closed the wakes that could go.
2. **`nvme_irq`, 10.5**, nearly all `blk_mq_end_request_batch`. Fewer
   interrupts a WRITE (coalescing on the drives, or polled queues for
   nfsd's direct writes) is the lever; neither has been tried.
3. **The GIC's entry, 7.7**, a fixed cost of each interrupt; it falls
   only with fewer interrupts (2 and the IPIs of 1).
4. **The timer on the rings, about 7.6** (and 1.2 on nfsd's CPUs): to
   count the deferral's expiries before anything else.
5. **Reading the clock, 6.1, and PSI, 3.2**, overlapping (1.55 of the
   clock is PSI's). PSI could be measured off with `psi=0` on the command
   line on a boot without pseudo-NMI: a host setting, not a change to the
   series, and Hammerspace's own setting would decide whether it can go.
6. **The region's return under its lock, 3.4**, after round 58; the lock
   can go no further inside the footprint (the plan above).
7. **NVMe's SGL lists in `dma_pool`, 1.25**, under the pool's lock.

Pseudo-NMI itself costs 4% of nfsd's cycles on this boot (above), so these
are shares; a change is to be measured on a boot without it.
