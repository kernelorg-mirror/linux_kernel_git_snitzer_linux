# Round 101: build 531 with PSI off (`psi=0`), the boot plan

14 November, tardis2, build 531 (`7.3.3-16.hs.531.canary`), the DRC
commits' `nfsd.ko` in `updates/` (round 100).  A plan only: nothing is
installed and no boot is set.  The reboot is Mike's (AGENTS.md: a
reboot of tardis2 risks a port).

## Why

PSI (pressure stall information) accounts every context switch:
`psi_task_switch()` reads the clock and updates each of the task's
groups, under the run queue's lock with interrupts off.  So a profile
without pseudo-NMI charges it to `finish_task_switch` at irq enable,
and round 100's shows no `psi_*` frame at all.  Round 61, with
pseudo-NMI, found PSI whole at **3.24k cycles a WRITE (1.2% of nfsd's
CPUs)** at 9.96 context switches a WRITE.  Build 531 at the settled
setting switches 7.12 times a WRITE (`t2-r610`: 1,062,664 a second
for 149,246 WRITEs), so PSI should be about **2.3k cycles a WRITE,
1.4% of nfsd's 163k**.  The rate will not move: the drives hold it
(round 100).

`CONFIG_PSI=y` and not `CONFIG_PSI_DEFAULT_DISABLED`, so it is on.
`psi=0` disables the `psi_disabled` static key at boot, and every
hook is then a patched-out branch.  At runtime there is no such
switch: `0` in the root's `cgroup.pressure` sets only
`psi_system.enabled`, and `psi_group_change()` still reads the clock,
takes the group's seqcount and counts the tasks for a disabled group
(`kernel/sched/psi.c`), so it would measure part of PSI, not `psi=0`.

What turns off with it: `/proc/pressure/{cpu,io,memory}` and every
cgroup's pressure files.  Nothing on tardis2 reads them
(`systemd-oomd` is not installed, nothing in the rig does).  Whether
Hammerspace's product can run without PSI is Hammerspace's to say;
this round only measures the cost.

## What it is compared with

Round 100's runs on this kernel and module, at the settled setting:
counted `t2-r604`, `r606`, `r610`, `r612` (162.5, 163.2, 163.0,
163.0k nfsd cycles a WRITE, mean 162.9k; GB/s 78.01 to 78.06) and the
call graph `t2-r611` (`finish_task_switch` 16.0k).  Two runs of the
same module have differed by 2.1k (round 100's A/B), so a 2.3k change
is near the spread of one run: it is judged on four counted runs
against four, and on `finish_task_switch`'s self cycles in the call
graph, where PSI's cost lands.

## The boot

The same kernel and initrd, one option more, as a one-shot entry, so
the boot after it is plain 531 again with nothing to undo:

    R=7.3.3-16.hs.531.canary.el10.aarch64+16k
    printf '%s\n' 'title      531 with psi=0 (one-shot test)' "version    $R" \
      'sort-key   ofp-rocky-zz-psi0' \
      'options    console=ttyAMA0 rodata=noalias crashkernel=1G psi=0' \
      "linux      /ofp-rocky/$R/linux" "initrd     /ofp-rocky/$R/initrd" \
      > /boot/loader/entries/ofp-rocky-531-psi0.conf
    bootctl list | grep -A4 531-psi0     # the entry, its options
    bootctl set-oneshot ofp-rocky-531-psi0.conf
    bootctl status | grep -E 'Default|OneShot'   # 531's own, and the psi0 entry

Then a reboot.  `updates/nfsd.ko` (the DRC commits) is kept: it is the
same kernel, so it loads again.  If a port comes up dead and only a
power cycle brings it back, set the one-shot again first
(`PORT-DEAD-AFTER-BOOT.md`; a power cycle uses it up).

## After it boots

1. `grep -o 'psi=0' /proc/cmdline` prints `psi=0` and
   `/proc/pressure` is gone.  If not, the boot fell back to 531's own
   entry: say so and stop.
2. `cat /sys/module/nfsd/srcversion` reads `603083CAF5F5B5E8DE898AC`.
3. `xeu-rig/bringup.sh` (kdump armed first: `kexec_crash_loaded` 1),
   ping both ports, mount the eight drives (`noatime,lazytime`), then
   `xeu-rig/settled.sh apply` ending in "the settled setting".
4. Commit what the boot showed (every boot's outcome, good or not, as
   `PORT-DEAD-AFTER-BOOT.md` asks) before any run.
5. Root needs over 2 GB free (`run601.sh` stops below it; round 100's
   first start did).
6. The runs, with round 100's script:
   `RUNS="613:C 614:G 615:C 616:C 617:C" ../100-build531-baseline/run601.sh`,
   about 30 minutes.
7. Read it as round 100 did: `76-canary-baseline/read65.py` over
   `t2-r613 r615 r616 r617` beside `t2-r604 r606 r610 r612`,
   `top.py` and `groups.py` likewise, and `77-loan-walk/under.py
   t2-r614 finish_task_switch.isra.0` against `t2-r611`.

## The boot after it

The one-shot is used up, so the next reboot is plain 531.  Four
counted runs there (`RUNS="618:C 619:C 620:C 621:C"`) tell a change
between boots from PSI's: they should read round 100's 162.9k.  This
needs a second reboot, Mike's to decide; it is worth it only if step 7
shows a change near the spread.

## What decides it

PSI is worth about 2.3k cycles a WRITE if round 61's share holds.  A
saving of that size frees about 1.4% of nfsd's CPUs at the same rate.
Whether to keep `psi=0` on tardis2 is Mike's; whether the product can
go without it is Hammerspace's.

## 15 November: the boot is on, with a new baseline first

Mike, 15 November: "let's reboot with psi=0 using the current kernel as
test kernel, in parallel I'll build the kernel with all the latest
commits using gitlab".

Since the plan was written, `updates/` has gained a `sunrpc.ko`
(`B1AE4F3F0262A3E5AD387FA`: `278b74ad8a0d9`, `2e2811d21f8c4`,
`fe25e474fa3de`, FLAMEGRAPH round 104) and the settled setting has
gained `rtc_read` Y and `svc_rtc_cpus` 8-55, which change how WRITEs
reach nfsd.  Round 100's runs are not this setting's, so (Mike:
"Baseline first") four counted runs and two two-port reads are taken
on this boot, PSI on, before the reboot, and the same after it:

- PSI on, this boot: `RUNS="692:C 693:C 694:C 695:C"
  ../100-build531-baseline/run601.sh`, then `PROBE=none
  RUNS="696:read 697:read" ../104-rx-cpus/run645.sh`.
- PSI off, the `psi=0` boot: `RUNS="698:C 699:G 700:C 701:C 702:C"`
  and `RUNS="703:read 704:read"`, the same scripts.

Read as step 7 says, with r692 to r695 in place of r604 to r612, and
the call graph r699 against... the one call graph on this boot is
round 100's r611 (before the new sunrpc), so `finish_task_switch` is
read against it with that caveat.  Both `updates/` modules are kept
for the boot: it is the same kernel.
