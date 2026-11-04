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
