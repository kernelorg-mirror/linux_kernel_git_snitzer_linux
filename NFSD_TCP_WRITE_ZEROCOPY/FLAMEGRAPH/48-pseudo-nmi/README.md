# Round 48: a profile that sees hard interrupts (pseudo-NMI)

**The ask (Mike, 31 October):** without pseudo-NMI, perf cannot sample with interrupts off: a sample lands where they come back on. Round 47 timed about 20 us a WRITE of hard interrupts on nfsd's CPUs (NVMe 13, IPIs 6.6) that no profile has ranked. This round takes a profile with `irqchip.gicv3_pseudo_nmi=1`, a look and not a change to keep: pseudo-NMI adds its own cost to masking interrupts, so take shares from this boot and run figures from a boot without it.

**The boot (31 October, Mike rebooting).** Added on tardis2 to `/etc/kernel/cmdline`, for kernels installed from now on, and to build 505's own entry, `/boot/loader/entries/ofp-rocky-7.3.0-7.hs.505.loanpages_pool.el10.aarch64+16k.conf`, since the file is read only at install. The two as they were are kept here (`boot-entry-505-before.conf`, `etc-kernel-cmdline-before`); `settled.sh` does not audit the parameter.

**After the boot:**

1. Check: `grep gicv3_pseudo_nmi /proc/cmdline`, and dmesg for `Pseudo-NMI` being used (`GICv3: Pseudo-NMIs enabled`).
2. `for n in 0 1 2 3 4 5 6 7; do mount -o noatime,lazytime /dev/nvme${n}n1 /hs/nvme$n; done`, `systemctl start nfs-server`, `./bringup.sh 32768 256`, `./settled.sh apply`, which must end in "the settled setting". Check that no NFS client is connected first.
3. `./flame.sh $L/t2-fl48 fl48 64`, the same run as rounds 46 and 47's, and read `top-nfsd.txt` and `top-rings.txt` against `../47-write-path/fl47/`. `_raw_spin_unlock_irqrestore` should fall away and `nvme_irq`, the completion path and the IPI's handlers appear. If the cycles a WRITE differ much from fl47's 292.2k, say so: it is pseudo-NMI's own cost.
4. `xeu` has no symbols in `perf script` (`[[xeu]]`), so read its frames with the build's module.

**To take it out:** take the parameter off both files and reboot.
