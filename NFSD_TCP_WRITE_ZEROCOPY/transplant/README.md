# Transplanting the page-loan test harness to another host

Everything needed to take branch `kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY`
to an aarch64 test host, build or install its kernel there, run the whole
test plan, and bring back results that can be set beside another page
size's.

## Where things stand (2026-10-07)

| host | page size | kernel | state |
|---|---|---|---|
| the 16 KiB host | 16 KiB | built on the host | **complete**: the whole plan, both kinds of storage. `results/16KiB.tsv`; the account is "16 KiB host" below |
| the 4 KiB ARM host | 4 KiB | built on the host | **complete** (2026-10-05): the whole plan, both kinds of storage. `results/4KiB.tsv`; the account is "4 KiB host" below |
| tardis1 (Xsight E1) | 64 KiB | packaged RPMs, build 458 | **complete** (2026-10-07): the whole plan, both kinds of storage, on veth. `results/64KiB.tsv`; the account is "64 KiB host" below. Its kernel is later than the other two hosts' |

`RESULTS.md` is the side-by-side comparison, generated from one record per
host. A host's work is done when its record is committed there.

**What a run on the next host has to produce:**

1. `preflight.sh` says PREFLIGHT PASS.
2. `run-testplan.sh` completes: boot 1, reboot, boot 2. Every stage PASS or
   DONE; every cell read, not only the stage status.
3. `results-table.py extract <results dir> > results/<page>.tsv`, committed,
   and `RESULTS.md` regenerated (`RESULTS.md` says how).
4. An account of anything that differed, as a section of this file like
   "16 KiB host", kept to the last full run and the problems that explain a
   change in the tree.
5. `check-grouping.sh` says `grouping OK`.

**What is different since the last 4 KiB run** (2026-10-04), so that
nothing there is taken for granted:

- **Names.** The limit is the *budget* everywhere: `svc_tcp_rx_budget_pages`
  and friends, `net.sunrpc.region_budget_*`, `run-budget.sh`,
  `budget-*` stages. *Credit* is only what one WRITE draws against it
  (`svc_tcp_rx_credit_factor` keeps its name).
- **A budget of 0 copies, always**, even when no budget is set anywhere.
  The old "region, no budget" cells that loaned without limit are gone.
- **A write is cut at page boundaries, at every end that is not on one**
  (since 2026-10-06 a DIO-aligned end too:
  `slipstream/NFSD_LOCALIO_DIRECT_SPLIT.md` section 7), and the knob that
  decides whether to cut is in pages again since `v7.1.13-20`
  (`direct_misaligned_min_pages`, default 2 on 4 KiB pages and 1 on
  larger ones; it was `direct_misaligned_min_bytes` for the runs below). No
  effect expected at 4 KiB; that is to be confirmed, not assumed.
- **Two test devices are fixed in this kernel:** null_blk lost data with a
  block size below the page size (not reachable at 4 KiB with 4096-byte
  blocks), and dm-delay rounded short delays up to a timer tick. **The
  second changes every throughput figure**: at 100 Hz the harness's 1 ms
  storage delay was 10 ms.
- **The plan runs every budget phase on two kinds of storage**, nvme-loop
  and NVMe/TCP, and veth has a test-only `rx_ip_align` so that loans on
  the second are written direct.
- **Every loan cell has a twin that copies** (`-copy`), and two pairs
  write files that already have their size (`-sized-`).
- **`cells.tsv` says how each cell's WRITEs were issued** (`wr_direct`,
  `wr_dontcache`, `wr_buffered`), which needs `CONFIG_HIST_TRIGGERS`.
- **The plan fails a budget stage** on any fio verify failure or any cell
  killed by its timeout, and the analyze stages fail when a phase is
  missing.

**Things to look at on 4 KiB in particular:**

- **Does `rx_ip_align=2` give aligned loans?** At 4 KiB and MTU 9000 veth
  lays a frame out as a head and two page fragments, not one page. If
  `wr_direct` is 0 in the loan cells of the NVMe/TCP stages, that is why;
  record it. MTU 1500 should be aligned either way.
- **`mtu1500-on` may really loan** at 4 KiB (the loan table is larger
  relative to the fragment count), where at 16 KiB it is always copied.
- **The cell that hung at 16 KiB before the budget-0 fix only slowed down
  at 4 KiB.** Nobody knows why. It cannot happen now, but if anything
  stalls, look there first.
- **Timer tick.** Check `CONFIG_HZ`. The dm-delay fix makes the storage
  delay honest either way, but TCP's own timers differ with it.

## Starting the 4 KiB host's session

The 4 KiB host has an older clone, an older kernel and a Claude memory
that predate the changes above. "Run preflight.sh and then
run-testplan.sh" is not enough: it needs the new branch and a kernel built
from it first.

**The first thing to say to a new session there.** That host has not
pulled the branch that holds this file, so the session cannot be told to
read it from the working tree. This reads the prompt from the remote
before anything is reset:

```
In /root/kernel/linux: git fetch origin, then read the section "Starting the
4 KiB host's session" with
  git show origin/kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY:NFSD_TCP_WRITE_ZEROCOPY/transplant/README.md
and follow the prompt in it. Don't reset or pull until you've told me what local
commits, branches and untracked files this clone has.
```

Before sending it, change the path if the clone is elsewhere, and add
which file is that host's known-good 4 KiB config: the prompt below asks
for it without naming one.

**The prompt it then follows** (or paste this instead of the above):

```
Pull kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY (I force-pushed it; history was
rewritten, so reset to origin rather than merge, and do not remove untracked
files). If this clone has unpushed commits or local branches from the old
history, tell me what they are and ask before resetting.

Read AGENTS.md, then NFSD_TCP_WRITE_ZEROCOPY/transplant/README.md from the top:
"Where things stand", the build section, and the working rules at the end. Your
memory of this project is stale: the tunables, script and stage names changed
(credit -> budget), a budget of 0 now copies, and the plan now runs on two kinds
of storage.

Then:
1. Run host-bootstrap.sh, check the config, and build with build-test-kernel.sh
   from this host's known-good 4 KiB config. Tell me when to reboot.
2. After the reboot: preflight.sh, then run-testplan.sh. It stops and asks for a
   second reboot; after that, run it again to finish.
3. Read every cell, not just stage status. Then extract this host's record with
   results-table.py into transplant/results/4KiB.tsv, regenerate RESULTS.md, and
   write the 4 KiB account in the README, replacing the superseded 4 KiB section.
4. Commit as docs commits, run check-grouping.sh, and don't push.
```

What to expect:

- **Three stops for Mike:** a reboot after the build, a reboot part-way
  through the plan, and the push at the end. About an hour of run time on
  the first boot and a few minutes on the second.
- **`preflight.sh` fails on the old kernel**, by the tunables' names. That
  is the check that the build and reboot happened.
- **If the NVMe/TCP loan cells show their WRITEs written buffered**
  (`wr_direct` 0), that is the 4 KiB veth frame layout described under
  "Things to look at on 4 KiB". Record it; it is a finding, not a failure
  of the run.
- **Old results on that host** (`/var/tmp/zc-testplan-4k-*` and the like)
  stay until Mike says to clear them, as on the 16 KiB host.

## Starting tardis1's session

**First, the kernel, which is Mike's to do.** Nothing is built on tardis1.

- The packaged 64 KiB RPMs listed under "tardis1 (64 KiB)" below must be
  installed and booted, `kernel-64k-modules-internal` and
  `kernel-64k-devel` included, built from a tree that has everything up to
  at least `82d7a6d21207`.
- **Which branch they are built from matters.** Earlier tardis1 kernels
  came from the HS twin, `kernel-7.1/hs-7.1.13-19.NFSD_TCP_WRITE_ZEROCOPY`,
  and this work has not been carried to it. Build from this branch, or
  carry the work to the twin first.
- Run `check-config.sh` on the 64 KiB config before building:
  `CONFIG_HIST_TRIGGERS` joined the fragment after that config was last
  checked.
- `preflight.sh` fails on a kernel without the work (the tunables' names,
  veth's parameter), so the wrong kernel cannot slip through.

**Then the first thing to say to a new session there:**

```
In <path to the clone>: git fetch origin, then read the section "Starting the
4 KiB host's session" with
  git show origin/kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY:NFSD_TCP_WRITE_ZEROCOPY/transplant/README.md
and follow the prompt in it, with these changes for this host: this is tardis1,
64 KiB pages. The packaged 64 KiB kernel is already installed and booted; build
nothing here, so skip step 1 except host-bootstrap.sh. The record is
results/64KiB.tsv, and RESULTS.md has a 64 KiB row waiting in its first table.
Also read the sections "Starting tardis1's session" and "tardis1 (64 KiB)".
Don't reset or pull until you've told me what local commits, branches and
untracked files this clone has.
```

What is different about this host:

- **It is not a scratch VM.** The harness is written to touch nothing but
  its own exports, devices and namespaces, and restores the settings it
  changes. It does stop and start nfsd for the KUnit stage and reloads the
  veth module. If tardis1 is serving anything, say so in the message or
  pick a quiet window.
- **The plan runs on veth and test storage, not on the Xsight card or the
  real NVMe.** It answers whether the series behaves at 64 KiB. Whether the
  region stays in the CPU's cache and what a copy costs there are a
  separate exercise.
- **Two reboots**, as on the other hosts.

## What is in this directory

| file | does |
|---|---|
| `zerocopy.fragment` | every Kconfig option the series, its four KUnit suites and its rigs need |
| `check-config.sh CONFIG` | checks a finished config against the fragment (a build's `.config`, a `redhat/configs` flavor, `/boot/config-*`) |
| `merge-config.sh BASE OUT [TREE]` | merges into a host's known-good config only the options it lacks or has weaker, resolves with `olddefconfig`, re-checks, and names anything Kconfig dropped |
| `host-bootstrap.sh [--check]` | installs every command and package the build and the harness use on EL9 (enables CRB if needed), adds `/etc/nfs.conf.d/zc-harness.conf` (NFS 3, 4.0, 4.1, 4.2), mounts configfs, debugfs and tracefs |
| `build-test-kernel.sh --base-config FILE` | merge, then kernel-rpmtree's `build-kernel-on-host.sh --build-number 451`, then install, set as the default boot entry, and write a manifest (`/var/lib/zc-transplant/manifest`) |
| `preflight.sh` | after the boot: is this the kernel just built (manifest) or a packaged one; are config, modules, the series' interfaces, tools, memory and disk all present; is the boot still fresh |
| `run-testplan.sh` | the whole test plan, stage by stage, resumable across its reboot; results in `/var/tmp/zc-testplan-<release>/` |
| `zc-rig-down.sh` | teardown for the zc rig the zc runtime scripts leave up |
| `check-grouping.sh [BASE]` | enforces the series' history layout (code, KUnit, docs; no mixed commits; `Fixes:` tags on the branch); **required after committing on a test host** |
| `results-table.py extract DIR` / `compare FILES` | reduces a finished run to one record (`results/<page>.tsv`) and renders several side by side (`RESULTS.md`) |
| `RESULTS.md`, `results/` | the comparison across page sizes, and each host's record |

## A host that builds its own kernel (4 KiB, 16 KiB)

The 16 KiB host was done this way, and the 4 KiB ARM host is the same.

Prerequisites, from Mike:
- a clone of this branch at `/root/kernel/linux`;
- kernel-rpmtree at `/root/kernel/kernel-rpmtree`;
- the host's known-good `.config` for its page size;
- root.

```sh
git clone -b kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY \
    ssh://git@gitlab.lab.hammer.space:2222/engineering/linux.git /root/kernel/linux
cd /root/kernel/linux
T=NFSD_TCP_WRITE_ZEROCOPY/transplant

$T/host-bootstrap.sh                       # packages, nfs.conf drop-in, mounts
$T/check-config.sh /path/to/known-good.config     # what it lacks, before building
$T/build-test-kernel.sh --base-config /path/to/known-good.config
#   -> 7.1.13-19.hs.451.el9.aarch64+  (installed, default boot entry, manifest)
reboot
```

After the reboot, start a Claude Code session in `/root/kernel/linux` and hand
over. It reads `AGENTS.md`, which points here. Then:

```sh
NFSD_TCP_WRITE_ZEROCOPY/transplant/preflight.sh      # must say PREFLIGHT PASS
NFSD_TCP_WRITE_ZEROCOPY/transplant/run-testplan.sh   # boot 1; stops asking for a reboot
reboot
NFSD_TCP_WRITE_ZEROCOPY/transplant/run-testplan.sh   # boot 2: budget-B, analyze, summary

cd NFSD_TCP_WRITE_ZEROCOPY/transplant
./results-table.py extract /var/tmp/zc-testplan-$(uname -r) > results/4KiB.tsv   # this host's record
```

Things the 16 KiB build taught, which apply to any host:

- **A known-good config with no debug information fails the config
  check** (BTF is required). Turn it on in a copy and pass that.
- **`localmodconfig` to shorten the build** drops modules the harness
  needs and the config check does not list. "How the kernel was built",
  under "16 KiB host", says which and how they were restored.
- **To rebuild one module after a change, run the whole incremental
  build** (`make -j`, then copy the `.ko`), not `make path/to/module.ko`.
  The single-module form fails at modpost for a module that uses another
  module's symbols and leaves the old `.ko` in place, and it is easy to
  miss the error.
- **Allow about 7 minutes per build and an hour for the plan** (50
  minutes on boot 1, 6 on boot 2) on a 4-CPU VM.

The clone's branch tracks hs-gitlab's copy, which is what Mike pushed and
the base `check-grouping.sh` uses. `-b` only saves a checkout: a plain
clone followed by `git checkout kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY`
sets up the same tracking, after first writing out the default branch's
whole working tree.

`merge-config.sh` never edits the base config. It writes the resolved one
to `/var/lib/zc-transplant/config-<time>`, with a `.report` beside it listing
what it added. If Kconfig drops a required option because of an unmet
dependency, the build stops before compiling and names the option.

## tardis1 (64 KiB)

Kernel: the full packaged 64 KiB kernel from this branch's `redhat/configs`,
built wherever Mike builds RPMs (`build-kernel.sh --srpm-only` +
`build-kernel-in-ubi.sh --full`, build number 451). The branch's
`redhat/configs` already enable everything in `zerocopy.fragment`:
`check-config.sh` passes on `redhat/configs/kernel-7.1.13-aarch64-64k.config`
and `kernel-7.1.13-aarch64.config` (2026-10-03). The four KUnit suites
(`e2a0e59c2d09`) are among them. Install on tardis1:

- `kernel-64k`, `kernel-64k-core`, `kernel-64k-modules-core`, `kernel-64k-modules`
- `kernel-64k-modules-internal`, which holds the KUnit suites
  (`redhat/rhel_files/def_variants.yaml.rhel`: `.*kunit.*: modules-internal`)
- `kernel-64k-devel`, which the `bvecrepro` stage builds its small test
  module against (with `gcc` and `make`, which `host-bootstrap.sh`
  installs). Without it that one stage is skipped
- the Xsight NIC driver built against those RPMs
  (`build-kernel-in-ubi.sh --kernel-rpms … --kmod-dir xsight=<ver>`, giving
  `kmod-64k-xsight`), with the `rx_ip_align` patch if that experiment is
  wanted (`../xeu-SGL-compat-receive-placement/`)

The RPMs must be built from this branch at or after `82d7a6d21207`
(2026-10-05), which carries the null_blk and dm-delay fixes, veth's
`rx_ip_align`, the write-split fix and the budget names. `preflight.sh`
tells: it checks the tunables' names, veth's parameter and the histogram
triggers.

The 64 KiB config was checked against `zerocopy.fragment` on 2026-10-05
(kernel-rpmtree's `rpmbuild/configs/kernel-aarch64-64k-rhel.config`):

- **The five KUnit options are set, each `=m`:** `KUNIT`,
  `SUNRPC_XDR_KUNIT_TEST`, `SUNRPC_SVCSOCK_KUNIT_TEST`,
  `NFSD_BVEC_KUNIT_TEST`, `NFSD4_BVEC_KUNIT_TEST`.
- **That config also has `CONFIG_KUNIT_ALL_TESTS=m`**, which the 4 KiB
  and 16 KiB test kernels do not. It builds every KUnit suite in the tree,
  not only these four. The harness is unaffected, since `run-kunit.sh`
  loads its four suites by name, but the RPMs carry more modules.
- **The suites ship in `kernel-64k-modules-internal`**, not in the main
  modules package. The options being set does not help without that RPM
  installed: the KUnit stage would find no modules to load.
- **`CONFIG_HIST_TRIGGERS=y` is set**, which the harness's write-path
  counters need. `CONFIG_HIST_TRIGGERS_DEBUG` is off and is not needed;
  the other two hosts ran with it off.
- **`check-config.sh` reports four options missing from that file, and
  they are not:** `PAGE_POOL`, `NVME_CORE`, `NVME_FABRICS` and
  `CONTIG_ALLOC` have no prompt, so an unresolved RHEL-style config never
  lists them. Kconfig sets them at build time from options that are in the
  file (`BLK_DEV_NVME`, `NVME_TCP`, `NVME_TARGET_TCP`, `CMA`). The check
  that counts is on the built kernel's `/boot/config-<release>`, which
  `preflight.sh` reads.

Then on tardis1, with a clone of the branch (nothing in it is built):

```sh
T=NFSD_TCP_WRITE_ZEROCOPY/transplant
$T/host-bootstrap.sh
$T/preflight.sh        # no manifest: build identity skipped; config from /boot
$T/run-testplan.sh     # every stage runs; bvecrepro builds against kernel-64k-devel
```

`bvecrepro` loads a test module of the harness's own, so it needs the
running kernel's build files. On a host that built its kernel that is the
tree; on a packaged kernel it is `/lib/modules/<release>/build`, from the
`-devel` package. Until 2026-10-05 the plan only knew the first and
skipped the stage on a packaged kernel. `preflight.sh` now says which it
found. The packaged path has been run only on the 16 KiB host, where that
directory is the tree itself: on tardis1 it is the first real use, and a
kernel that refuses unsigned modules would fail it there.

What the plan does and does not use on tardis1:

- **It tests the kernel on veth, not on the Xsight card.** Every rig is
  local: veth with XDP for receive, null_blk behind nvme-loop or NVMe/TCP
  for storage. It answers "does the series behave at 64 KiB", and is
  comparable with the other two hosts for that reason.
- **The card and the real NVMe are a separate exercise**
  (`../xeu-SGL-compat-receive-placement/`, `../on-die-receive-copy/SPEC.md`
  phases 0a to 0c): whether a small region stays in the CPU's cache, what
  a copy costs there, and what the held receive buffer costs at 400G.
  None of the three test hosts' veth results speak to those.
- **Expect at 64 KiB:** a 1 MiB WRITE is 16 pages; the default region is
  555 pages (35 MiB); veth packs several frames to a page; the credit
  factor is 150, computed and never measured, so read `paused` and
  `overrun` in the cells with a budget. A misaligned write smaller than a
  page is written buffered.

## The test plan (`run-testplan.sh`)

| boot | stage | what | judged by |
|---|---|---|---|
| any | `preflight` | `preflight.sh` | PASS/FAIL |
| 1 (fresh) | `budget-D`, then `budget-D-sgl` | `PHASE=D run-budget.sh`: two devices share one region and budget. It must run before anything creates the region; the NVMe/TCP pass follows on the region it made. | tables vs D-s4 / D4 (`../slipstream/BUDGET_SCOPE.md` 8.1, 8.6) |
| 1 | `kunit` | `run-kunit.sh`: all four suites | PASS/FAIL; skip rule below |
| 1 | `bvecrepro` | `run-bvecrepro.sh`, which builds a test module against the tree, or against a packaged kernel's `-devel` files | PASS/FAIL, or SKIP with neither |
| 1 | `rig-cmp`, `loan-assert`, `killswitch`, `system-correctness` | the zc rig runtime checks | PASS/FAIL (each exits with its failure count) |
| 1 | `zc-down`, `sgl-up`, `sgl-brd/tcp/loop`, `sgl-down` | NVMe SGL rigs (`../NVME_SGL_SUPPORT_PROJECT.md` 6) | DONE; compare dispositions |
| 1 | `localio-tcp`, `localio-loop` | `run-sgl-localio-ab.sh` (LOCALIO boundary, short-read and stable tests) | DONE; each test prints its own exit status |
| 1 | `budget-A`, `budget-TLP`, `budget-X`, `budget-N` | `run-budget.sh` phases on the same region, on storage that needs whole pages (nvme-loop) | no drops or allocation failures with a budget set; `RESULTS.md` beside the other page sizes |
| 1 | `budget-A-sgl`, `budget-TLP-sgl`, `budget-X-sgl`, `budget-N-sgl` | the same phases again with `STORAGE=sgl`: storage that takes arbitrary pieces (NVMe/TCP), veth placing the payload on a 4-byte boundary (`RX_IP_ALIGN=2`) | tables beside the nvme-loop ones; `wr_direct` |
| 2 (fresh) | `budget-B` | the region at its configured minimum (uses A's `parked.txt`) | |
| 2 | `budget-B-sgl` | phase B again with `STORAGE=sgl`, on the region `budget-B` created | |
| 2 | `analyze-AB`, `analyze-AB-sgl` | David's `slipstream/tools/analyze.py` on A and B, for each storage | OVERALL PASS/FAIL |
| 2 | `summary` | `status.tsv` and every budget table | |

The nvme-loop stages write to `<results>/budget`, the NVMe/TCP ones to
`<results>/budget-sgl`. Each `stress-<phase>/rig.txt` records which storage
it was and the device's `virt_boundary_mask` (the harness refuses to run if
the device is not what `STORAGE` says). `cells.tsv` ends with `wr_direct`,
`wr_dontcache` and `wr_buffered`: how many segments nfsd issued as direct
writes, as buffered writes that drop their pages afterwards, and as plain
buffered writes. That is what tells the two kinds of storage apart, so read
it first.

A stage that fails is recorded and the plan continues. Re-running resumes
where it stopped. `--list` shows status, `--only STAGE` runs one stage, and
`--redo STAGE` forgets a result.

## 4 KiB host (2026-10-05)

The original development VM, as the 4 KiB host: aarch64, 8 CPUs, 11 GiB,
`CONFIG_HZ=100`. Kernel `7.1.13-19.hs.451.el9.aarch64+` #43, built here
by `build-test-kernel.sh` from `26e799b10e8e` with the VM's own config
(the one #42 was built from; it lacked nothing in `zerocopy.fragment`);
sunrpc `4882A601`. `preflight.sh`: PASS. Results:
`/var/tmp/zc-testplan-7.1.13-19.hs.451.el9.aarch64+`; the record is
`results/4KiB.tsv`.

### In short

- **The whole plan passes, on both kinds of storage, in two boots.** Every
  stage PASS or DONE; `analyze.py` OVERALL PASS for nvme-loop and for
  NVMe/TCP; no fio verify failure, hang or kernel warning in any cell.
- **The answers are the 16 KiB answers.** With a budget set and the
  receive buffer held nothing is dropped and the region never runs out.
  A loan is written direct on storage that takes arbitrary pieces and
  through the page cache on storage that needs whole pages; a copy is
  always written direct. Copy and loan are level in throughput in the
  sized pairs; a loan holds 8 to 10 MiB of the 16 MiB region, a copy 0.25
  to 1.8 MiB.
- **One harness fault, found here and fixed:** n-reconf no longer changed
  the budget under load, on either host (below).

### The questions 4 KiB was to answer

- **Does `rx_ip_align=2` give aligned loans at 4 KiB?** Yes. veth lays an
  MTU 9000 frame out as a head and two page fragments here, and still
  every loaned WRITE on NVMe/TCP was written direct: `seq-on` 512 of 512,
  `mixed-on` 2785 of 2785, and every other loan cell of phases A, TLP and
  B.
- **Does `mtu1500-on` loan at 4 KiB?** No. It is admitted (512 records)
  and written direct, as at 16 KiB, on both kinds of storage: it is copied.
- **The cell that slowed down at 4 KiB before the budget-0 fix** ran
  normally; nothing stalled.
- **Timer tick.** `CONFIG_HZ=100`. The dm-delay fix makes the 1 ms storage
  delay honest here, which is why every figure is about five times the
  older 4 KiB records'.

### What else was seen

- **Parked pages: 128** in every phase on both kinds of storage, so phase
  B's minimum region is 838 pages (128 + 256 + 390 + 64).
- **KUnit:** svcsock 40 + 1 skip of 41 (`svcsock_rx_arena_bound_test`, by
  design on 4 KiB), xdr 7/7, nfsd receive 6/6, nfsd4 receive 10/10.
- **zc rig and SGL rigs:** rig-cmp 457 direct, 0 vector; loan-assert 10 of
  10 connections fully direct; system-correctness 439 direct. SGL rigs,
  500 loaned WRITEs each: NVMe/TCP 160 of 160 direct, nvme-loop 156
  direct and 4 memory-misaligned, brd (512-byte memory alignment) 10
  direct and 150 memory-misaligned.
- **LOCALIO:** boundary, short-read and stable tests exit 0 on both
  transports; 0.002 (tcp) and 0.005 (loop) reads per WRITE at 47008,
  0.000 at 6000, 0.0% resident.
- **TLP:** `step-on-n16` on nvme-loop shows the spurious tail-loss probes
  the TLP study explained (7877 retransmits, 8871 probes, harmless); the
  quickack cell its usual 3 collapsed and 1 receive-queue drop. NVMe/TCP
  showed neither.
- **`collapse-off` on nvme-loop wrote 10 of 2785 WRITEs through the page
  cache**, where 16 KiB wrote all 2785 direct. That cell has no budget,
  so its region records are copied. TCP collapsed its receive queue 18
  times in that cell (16 on NVMe/TCP, which wrote everything direct), and
  a collapse rebuilds the data in ordinary kernel memory, outside the
  region, where the budget does not apply: such records can still be
  loaned, and their data is not in whole pages. The counts are
  consistent with that; the cell keeps no per-record trace, so it is not
  proven. fio verified the data.

### Problem found and fixed

**n-reconf no longer tested a budget change under load.** Its hook sets
the second namespace's budget to 400, 0 and 2000 at 6, 12 and 18 s, but
its load was sized in data (4 jobs of 768 MiB). Once dm-delay stopped
rounding the delay up to a tick, that load finished in about 4 s: at the
first change B's pool was idle with its whole count admitted. The 16 KiB
record shows the same (8352 admitted, as here), so its n-reconf did not
test it either. `0d3ab1e2c6f2` runs ten verified passes over the same
files (about 40 s here). Re-run on this boot (`budget-N` and
`budget-N-sgl`, `--only`), both kinds of storage show records in flight
and waiters at the first change (1 and 16), the waiters drained after the
change to 0, B's records drew on the default budget while its own was 0
(16729 to 17995 of them), fio exit 0. The 16 KiB host's n-reconf was
re-run the same way afterwards; its result is under "16 KiB host".

## 16 KiB host (2026-10-05)

The kernel was built on this host and the whole test plan run on it, on
both kinds of storage. Eleven problems were found and fixed on the way, in
the kernel, in two test devices and in the harness. This section gives the
results of the last full run, then the problems, since each explains a
change in the tree. Earlier runs on this host are not recorded: they were
of kernels and a harness that have since changed.

### In short

- **The whole test plan passes at 16 KiB**, every budget phase on storage that
  needs whole pages and on storage that takes arbitrary pieces: no data
  verification failure, no hang, no kernel warning.
- **The budget does its job on both.** With a budget set and the receive
  buffer held, no cell dropped a packet or ran the region out, whether its
  loans were written through the page cache or direct.
- **On storage that takes arbitrary pieces, every loaned WRITE was written
  direct**, once the test network device placed the payload on a 4-byte
  boundary. Without that, none were.
- **Copy and loan are level on this rig** once they are compared like
  for like: within a few percent of each other on both kinds of storage,
  with a copy holding 16 to 112 pages of the region and a loan about
  1000. This is a VM with memory for storage, so it says nothing about
  what a copy costs on hardware.
- **A direct write to a file that is growing is slow on XFS, loaned or
  copied**: about 350 MB/s where the same write to a file that already has
  its size runs at 1600 to 2100. That, not the loan, is why loans written
  direct looked five times slower than loans written through the page
  cache in the plan's new-file cells.
- **Only 16 KiB was run.** Nothing here was run at 4 KiB or 64 KiB with
  these changes.

### Words used here

| word | what it means |
|---|---|
| **loan** | nfsd hands the network card's own receive pages to the file system as the WRITE's data. nfsd copies nothing. The pages cannot go back to the card until the write is finished. |
| **copy** | nfsd copies the WRITE's data out of the receive pages into its own buffer, and the receive pages go straight back to the card. |
| **direct write** | the file system sends nfsd's memory to the device as it is (O_DIRECT). Nothing is copied into the page cache. |
| **buffered write** | the file system copies the data into the page cache and writes it out from there. |
| **region** | a fixed, small block of memory that named network devices must receive into (`page_pool.rx_region_dev`, `rx_region_pages`). It exists so that the place packets land can be small enough to stay in the CPU's cache, which makes copying out of it cheap. Being fixed, it can run out. |
| **budget** | a limit, in pages, on how much of the region loans may hold at once. A connection whose next WRITE does not fit waits, and TCP slows its sender. A budget of 0 means no loans from the region: its data is copied. Set with `svc_tcp_rx_budget_pages`, or per network namespace with `net.sunrpc.region_budget_pages`. In the code, what one WRITE draws against the budget is called its credit. |
| **held receive buffer** | each connection's TCP receive buffer fixed at 128 KiB rather than left to grow (`svc_tcp_rx_rcvbuf`). It limits how much data that nfsd has not read yet is sitting in region pages. |

### Two kinds of storage, two right answers

**1. Storage that needs its data in whole pages.** This is NVMe without
SGL support ("PRP only"), which is what tardis1 has. The test stand-in is
nvme-loop.

- A loaned WRITE reaches the file system as one piece per network packet,
  and each piece ends part-way through a page. This storage cannot take
  that as a direct write.
- nfsd therefore falls back to a buffered write, which copies the data into
  the page cache. The loan saved nfsd's copy and bought a different one,
  and held the receive pages longer while it did.
- **The right configuration is: a region, a budget of 0, and the receive
  buffer held.** nfsd copies each WRITE out of the region as it arrives,
  into its own buffer, which is laid out in whole pages and can be written
  direct. The region stays nearly empty.

**2. Storage that takes its data in arbitrary pieces.** This is NVMe with
SGL support. The test stand-in is NVMe over TCP.

- A loaned WRITE can be written direct as it stands: no copy anywhere.
- **But only if the card puts the data on a 4-byte boundary**, which NVMe
  requires of memory. By default it lands 2 bytes off, on the Xsight card
  and on this test rig alike; see problem 9
  below. On the card that is fixed by loading `xeu` with `rx_ip_align=2`,
  and the rig's veth now has the same setting. Each is a private driver
  parameter; the network stack has no general way to ask for it. One
  design for a setting the stack would own, with veth as its first user,
  is written up in `../xeu-SGL-compat-receive-placement/README.md`. It is
  not built, and Mike expects upstream would not take it.
- The price is that the receive pages are held until the device has
  finished the write. With a region, that is what can use it up.
- **The right configuration is: loans on, and if a region is used, a budget
  and the receive buffer held.**

What each combination does:

| | storage needs whole pages | storage takes arbitrary pieces |
|---|---|---|
| region, budget 0 (copy) | **the intended setup**: one copy, then a direct write | works, but gives up the copy-free path |
| region, budget set (loan) | works, but copies anyway, through the page cache, and holds region pages longer | **the intended setup**: no copy, direct write |
| region, either, receive buffer not held | unread data fills the region and the card drops packets | the same |

This two-way split leaves things out, and they matter:

- **Page size against block size.** Where a misaligned write is cut depends
  on both; that was one of today's bugs.
- **Client alignment.** A copied WRITE goes direct only if its file offset
  is a multiple of 4.
- **A third route** for whole-page storage: making the card place each
  packet's data at the start of a page with a packet size that is a
  multiple of 4 KiB, so loans go direct after all
  (`../on-die-receive-copy/SPEC.md`, "Z1"). Not tested anywhere.
- **Whether the region really stays in the CPU's cache** is a hardware
  question for tardis1. Nothing on this host can answer it.
- **LOCALIO** takes neither path: its data is the client's own pages.

### What was measured

Kernel `7.1.13-19.hs.451.el9.aarch64+` build #17, built from branch tip
`82d7a6d21207`; sunrpc `4882A601`, nfsd `4E439F2B`, nfs_dio `A03FA30E`,
null_blk `DC3E1848`, dm_delay `689119E5`, veth `7E6CC94B`. Results in
`/var/tmp/zc-testplan-16k-run4/`. Every cell's fio exit status and
verification output was read, not only each stage's status.

The storage is the same for both kinds but for the transport: a
memory-backed null_blk device behind dm-delay (1 ms per write unless a
cell says otherwise), exported over nvme-loop (**needs whole pages**) or
over NVMe/TCP with the payload placed (**takes arbitrary pieces**). Region
of 1519 pages (24 MiB), MTU 9000, 1 MiB WRITEs. "Written" is how nfsd
issued the cell's WRITEs, from the harness's counters; it was all one way
in every cell.

**Copy against loan, on both kinds of storage** (phase A, re-run after
the cells were made comparable; see problem 11). Each loan cell (`-on`:
budget set) has a twin that copies (`-copy`: budget 0) and is otherwise
the same, the held receive buffer included. The `-sized-` pairs write
files that already have their size.

| pair | conn | whole pages: copy / loan MB/s | loan written | arbitrary pieces: copy / loan MB/s | loan written | region pages: copy / loan |
|---|---|---|---|---|---|---|
| `seq-sized` | 1 | 1832 / 1802 | buffered | 1662 / 1612 | direct | 16 / 967 |
| `mixed-sized` | 16 | 792 / 799 | buffered | 790 / 781 | direct | 112 / 1060 |
| `step` (storage stalls mid-run) | 1 | 1194 / 1298 | buffered | 1085 / 1070 | direct | 32 / 970 |
| `step-n16` (the same) | 16 | 1439 / 1439 | buffered | 1207 / 1128 | direct | 112 / 1010 |
| `seq` (new file) | 1 | 414 / 1766 | buffered | 358 / 339 | direct | 16 / 965 |
| `mixed` (new files) | 16 | 656 / 746 | buffered | 624 / 603 | direct | 112 / 1060 |

- **Nothing dropped and the region never ran out** in any of these cells,
  on either kind of storage. A copy is always written direct.
- **On storage that takes arbitrary pieces every loaned WRITE went
  direct**: 512 of 512, 2785 of 2785, 122469 of 122469, 128922 of 128922.
- **Copy and loan are level on this rig.** Where the file has its size
  they are within 3% of each other in three of the four pairs on each
  kind of storage, and in the fourth (`step`, one connection) the loan
  leads by 9% on the first kind and trails by 1% on the second. Each cell
  is one run, so differences of this size are not findings.
- **What does differ is the region:** a copy holds 16 to 112 pages of it,
  a loan about 1000.
- **The two new-file pairs measure XFS, not copy against loan.** A direct
  write to a growing file waits (next part). On the first kind of storage
  that hits the copy and not the loan, which is buffered there: 414
  against 1766. On the second it hits both: 358 against 339.
- This is a VM with memory for storage. A copy here costs nothing like
  what it costs when the data has left the CPU's cache, so level here
  does not mean level on hardware.

Other cells of the phase:

- `seq-off`, `mixed-off`, `step-off` have no budget and no held receive
  buffer; they are under "What happens without the receive buffer held".
- `mixed-on-v42` (NFSv4.2, loan): 792 and 787 MB/s, as `mixed-sized-on`.
- `collapse-on` (loan, small socket memory): 766 and 624 MB/s, no drops.
- `mtu1500-on`: at MTU 1500 a 1 MiB WRITE has more pieces than a loan can
  hold, so every one of its 512 WRITEs was copied (traced:
  `reason=capacity`) and written direct on both kinds of storage. It
  tests that fallback, not a loan.

**Why a direct write to a new file is slow.** `seq-on` and `seq-off`
write a new 512 MiB file in order, sixteen 1 MiB WRITEs at a time, served
by eight nfsd threads. XFS makes a direct write that starts beyond the
current end of the file wait for every direct write in progress, so the
threads queue behind one another and each pays the storage's 1 ms in turn
(`../on-die-receive-copy/SPEC.md` section 3.4). Giving the file its size
first (`PREALLOC=1`, which the harness gained for this) removes the wait.
Two runs each with 2 GiB files, same kernel and region:

| storage | cell | written | new file: MB/s | file already sized: MB/s |
|---|---|---|---|---|
| needs whole pages | `seq-off` (copy) | direct | 396, 347 | 2158, 2160 |
| needs whole pages | `seq-on` (loan) | buffered | 1483, 1684 | 1790, 1796 |
| takes arbitrary pieces | `seq-off` (copy) | direct | 340, 350 | 1542, 2032 |
| takes arbitrary pieces | `seq-on` (loan) | direct | 335, 338 | 1571, 1642 |

- **Every direct write to the growing file runs at 335 to 396 MB/s**,
  whether it was loaned or copied and whichever storage it is on. Sized
  first, the same writes run four to six times faster. Buffered writes do
  not care.
- **With that out of the way a loan written direct is about a tenth
  slower than a loan written through the page cache** in this table (1571
  and 1642 against 1790 and 1796). "The remaining tenth" below takes that
  apart.
- **The copy cells in this table are `seq-off`, without the receive
  buffer held**, which the same part shows to be worth more than anything
  else here. The plan's cells have since been made comparable; the table
  at the head of this section is the result.
- **What it does say for a real server:** a workload that creates files
  with direct writes from several threads will meet this XFS wait,
  loaned or not. Whether tardis1's workload creates or overwrites decides
  how much it matters there.
- `step-on` is not helped by sizing the file (1038 and 1078 MB/s): it
  rewrites one 1 GiB file for two minutes, so only its first pass grows
  the file. Its direct loans still run about a fifth slower than buffered
  ones (1330); that cell stalls the storage for 20 seconds, which a
  direct write has to sit through.

**The remaining tenth.** It is two things, and neither is the loan.

- **About half is the transport.** The table above compares a direct
  loan on NVMe/TCP with a buffered loan on nvme-loop. A buffered loan on
  NVMe/TCP itself (payload not placed, so it falls back) runs at 1644 to
  1649 MB/s, not 1790: NVMe/TCP is the slower way to reach the same test
  device.
- **The rest, about 6%, is real and small.** Same storage, same file
  sized first, receive buffer held at 256 KiB, three runs each:

  | WRITE | MB/s |
  |---|---|
  | loaned, written through the page cache | 1694, 1714, 1719 |
  | loaned, written direct | 1601, 1615, 1583 |
  | copied (budget 0), written direct | 1568, 1646, 1724 |

- **What was established about the 6%:**
  - It needs a storage delay. With the delay set to 0 (`FORCE_DELAY=0`)
    the two loaned cells overlap: 1476, 1985, 1858 direct against 1637,
    1956, 2016 buffered.
  - It is not a shortage of nfsd threads (the same gap with 16 as with
    8), not the budget (no WRITE was refused in either), and not TCP
    (identical counters on both ends).
  - Every WRITE here is a stable one, so nfsd waits for the data and for
    the file system's log in both cases: 3.6 ms per WRITE buffered, 3.9
    direct, measured inside nfsd. The difference is in the one 1 MiB
    request to the device, which takes about 1.65 ms when issued direct
    and 1.46 ms when issued by writeback.
  - It is not the loan's many small pieces: a copied WRITE, which is
    whole pages, takes just as long when written direct.
  - Throughput follows the client's time per WRITE, sixteen in flight at
    10.4 ms direct against 9.9 ms buffered.
- **What was not established:** why the same request takes 0.2 ms longer
  issued direct than by writeback on this device, and why 0.3 ms more in
  the server becomes 0.5 ms at the client.
- **A larger effect turned up on the way: the held receive buffer limits
  one connection.** The same copied cell runs at 1295 to 1388 MB/s with
  the buffer held at 128 KiB, 1568 to 1724 at 256 KiB, and 1732 to 2307
  with it not held. Holding the buffer is what keeps the region from
  filling, and on a single connection it costs up to a third of the
  throughput on this rig. The plan's only copy cells then (`seq-off` and
  the like) ran with the buffer not held and its loan cells with it held,
  so their MB/s were never comparing copy with loan (problem 11).
- **Compared fairly, copy and loan are level here:** 1568 to 1724 copied
  against 1583 to 1615 loaned, both written direct, with the copy using
  16 region pages and the loan 968. That is not evidence about real
  hardware: this is a VM, the storage is memory behind a fixed delay, and
  a copy here costs nothing like what it costs when the data has left the
  CPU's cache.

**What happens without the receive buffer held** (budget 0, so WRITEs are
copied; the region fills with data nfsd has not read yet):

| cell | conn | whole pages: MB/s | drops | arbitrary pieces: MB/s | drops | peak region pages |
|---|---|---|---|---|---|---|
| `step-off` | 1 | 1482 | 0 | 1262 | 0 | 1463 / 1488 |
| `mixed-off` | 16 | 540 | 7196 | 566 | 7916 | 1519, all of it |
| `x-off-R` | 16 | 548 | 7471 | 447 | 7620 | 1519, all of it |

- With several connections the card drops packets. The data still
  verifies and nothing hangs. `floor-refused`, where a receive buffer too
  small to hold is refused, shows the same (531 and 875 drops). The
  receive buffer has to be held.

**The intended setup for storage that needs whole pages**: budget 0 with
the receive buffer held (phase TLP, nvme-loop):

| cell | conn | MB/s | drops | peak region pages | written |
|---|---|---|---|---|---|
| `step-rcvbuf-n16` | 16 | 1351 | 0 | 112 | direct, 153215 of 153215 |
| `mixed-rcvbuf` | 16 | 602 | 0 | 112 | direct, 2785 of 2785 |

- One copy, then a direct write, and the region barely touched: under
  2 MiB for 16 connections.
- Beside them, the same loads loaned on this storage (`step-on-n16`
  1364 MB/s, `mixed-on` 752) hold about 1010 and 1050 pages. So on this
  rig the intended setup costs nothing in the stalled-storage cell and
  about a fifth in the mixed one, for a tenth of the region.

**The smallest workable region** (phase B, each kind on the same fresh
boot): 306 pages, budget of one WRITE's worth.

| cell | conn | budget | whole pages: MB/s | arbitrary pieces: MB/s | drops | peak region pages |
|---|---|---|---|---|---|---|
| `edge-at-n1` | 1 | 130 | 285 | 279 | 0 | 144 |
| `edge-at-n4` | 4 | 130 | 292 | 270 | 0 | 160 |
| `edge-at-mixed-n4` | 4 | 130 | 124 | 124 | 0 | 160 |
| `edge-at-step` | 1 | 130 | 812 | 626 | 0 | 144 |
| `edge-below-n1` | 1 | 194 | 311 | 275 | 0 | 144 |
| `edge-copy-n1` (copy, buffer held) | 1 | 0 | 394 | 340 | 0 | 16 |
| `edge-off-n1` (copy, buffer not held) | 1 | 0 | 382 | 339 | 0 | 208 / 256 |

- WRITEs proceed one at a time, nothing is dropped, and the region is
  never more than 160 pages in use. Loans were written buffered on the
  first kind of storage and direct on the second, all of them.
- `edge-copy-n1` is `edge-at-n1`'s twin that copies. It is no slower than
  the loan held to one WRITE at a time, and uses 16 pages. These cells
  write a new file, so the copy's figure is the XFS wait's.
- David's `analyze.py` gives OVERALL PASS on phases A and B for each kind.

**The budget's own behaviour**, whatever the storage:

- **A client whose packets do not come from the region is never limited**
  (phase X, both kinds): running alone it is never admitted or refused,
  and with a budget set it keeps 480 to 550 MB/s whether or not the region
  client beside it is being held to 100 to 130 MB/s by a tight budget.
- **Two devices sharing one region** (phase D, both kinds): with a
  budget, no drops and no allocation failures; the two clients get 360
  and 355 MB/s on the first kind of storage, 313 and 315 on the second.
  Without one, 8618 and 7953 allocation failures.
- **A budget per network namespace** (phase N, both kinds): each WRITE is
  counted against the right budget, whether its own, the default or none
  (copied, 5570 of 5570); the region is whole afterwards. `n-exit` ends
  with fio rc 4, as intended: it deletes the server.
- **A budget changed under load** (`n-reconf`, both kinds; re-run on a
  fifth boot with the harness fix `0d3ab1e2c6f2`, which keeps the load
  running through the changes at 6, 12 and 18 s). Each change landed with
  the load running: 13922 and 17233 WRITEs already admitted at the first.
  At the change to 0, eight connections were waiting on each kind of
  storage; the cell went on, B's WRITEs drew on the default budget while
  its own was 0 (15404 and 22644 of them), and it ended with no waiters,
  nothing in flight, the pool whole and fio exit 0 with its data verified.
  The cell keeps two snapshots and no per-record trace, so that the eight
  were woken is inferred from the cell finishing, not observed one by one.
- **Tail-loss probes** (phase TLP, both kinds): no retransmission
  timeouts in any cell. The stalled-storage cell draws about 3000
  spurious retransmissions on either kind (3148 and 3013), which quick
  acknowledgements or turning probes off on the client remove, as on
  4 KiB.

The run took five boots of the same kernel, into the one results
directory (the fifth for the `n-reconf` re-run above): the plan's two, a third when phases D, TLP and N were added on
the second kind of storage, and a fourth when phases A and B were re-run
with the cells of problem 11.

### The rest of the plan

- **KUnit:** 7/7, 41/41, 6/6, 10/10, no skips.
- **PASS:** bvecrepro; rig-cmp (30 x 16 MiB, 0 mismatches); loan-assert
  (50 of 50 connections fully loaned); killswitch; system-correctness.
- **SGL rigs** (no region, no budget, payload not placed): 0 compare
  mismatches on brd, NVMe/TCP and nvme-loop.
- **LOCALIO:** the boundary test exits 0 on both rigs (0.002 and 0.004
  reads per WRITE for 47008-byte records); the short-read and stable tests
  exit 0.

### Problems found and fixed

**1. The host panicked twice in a KUnit test.** One svcsock test assumed a
table of three entries; the table is sized from the page size and has
twelve at 16 KiB, so the test took a path its fixture was not built for and
dereferenced NULL. A test bug only; the receive code was right. Fixed by
"SUNRPC: size the materialize-fault test from the loan table, not the page
count" and "SUNRPC: give the svcsock KUnit outer-receive fixture transport
ops", after which a wrong expectation fails the test instead of panicking.

**2. A misaligned write was cut in the wrong place.** nfsd and LOCALIO
write the aligned middle of a misaligned write direct and its two ends
buffered. That only helps if direct and buffered I/O never share a page.
The code cut the middle at the storage's alignment (4096), which is a page
boundary only when pages are 4 KiB. On larger pages each misaligned write
had to read back the pages it shares with its neighbours: 0.99 reads per
WRITE in the LOCALIO test, against 0.004 on 4 KiB. Fixed by "nfs_common:
split a write's direct middle at page boundaries": 0.003 afterwards. The
full account, with what is still open, is in
`../slipstream/NFSD_LOCALIO_DIRECT_SPLIT.md`.

**3. The knob that decides whether to cut was counted in pages.**
`direct_misaligned_num_pages`, default 2, meant 8 KiB on 4 KiB pages and
128 KiB on 64 KiB. Replaced by `direct_misaligned_min_bytes`, default 8192,
for the runs recorded here. **Since `v7.1.13-20` it is
`direct_misaligned_min_pages`**, default 2 on 4 KiB pages and 1 on larger
ones, and the byte-counted commit is gone from this branch.
**Mike is not convinced by this one and wants it revisited**; it leads the
TODO list in the document above.

**4. A region with no budget could hang the server.** With no budget set,
loans from the region had no limit. Sixteen connections each part-way
through a 1 MiB WRITE held all 1519 pages between them; the card could not
receive the rest of any of them, so none finished and none gave its pages
back. Seen as one harness cell (`step-rcvbuf-n16`) that never completed,
and confirmed by inspecting the stuck server: every region page was in one
of sixteen half-received WRITEs, none in a socket queue, no nfsd thread
running. The rule that a budget of 0 means copy then covered only the case
where some other budget was set; it now covers every case ("SUNRPC: Copy
region records when the budget drawn on is 0"), and a copy cannot get
stuck. This is also what makes "region, budget 0" the intended setup for
storage that needs whole pages.

**5. The test storage lost data.** fio reported blocks of zeroes in files
it had just written: the same seven cells in two runs, and an eighth once
problem 4's fix made it write direct. Only direct writes were affected. It was null_blk, the memory-backed test device under the budget
harness, not nfsd: it mis-tracks which blocks hold data after a write whose
pieces are not whole blocks, when its block size is smaller than the page.
It is an upstream bug and can be shown with one write from userspace
(`../null-blk-subpage-repro.py`). Fixed by "null_blk: keep each
memory-backed copy within one block". `../BIO-DRIVER-AUDIT.md` has the
detail.

**6. The harness hid problems 4 and 5.** The plan recorded every budget
stage as done whatever its cells did, and one script exited silently on a
plain clone so that `budget-D` never ran. The plan now fails a budget stage
if any cell fails fio's verification or has to be killed by its timeout.

**7. The LOCALIO boundary test judged larger pages by 4 KiB rules.** Its
small record (6000 bytes) is larger than a 4 KiB page and smaller than a
16 KiB one, which changes what the test can show. The record is now scaled
with the page size, and a record smaller than a page is reported, not
judged. Which of its two read limits applies now follows whether the
record can be cut, not its literal size.

**8. The test storage was ten times slower than asked.** The harness asks
dm-delay for a 1 ms storage delay. dm-delay kept the time a write is due in
timer ticks, and this kernel ticks at 100 Hz, so every write took 10 ms. A
copy cell that writes direct ran at 57 MB/s for that reason, and so did
every cell whose WRITEs waited on the storage. Fixed by "dm-delay: do not
round a short delay up to the next timer tick": a 1 ms delay now measures
1.16 ms. Looking into that cell also showed something real about XFS,
which `../on-die-receive-copy/SPEC.md` section 3.4 now records: a direct
write that starts beyond the end of a growing file waits for every direct
write in progress, so several nfsd threads creating one file direct queue
behind one another.

**9. On storage that takes arbitrary pieces, no loaned WRITE was written
direct.** The budget harness only ran on nvme-loop. Given an NVMe/TCP
variant (`STORAGE=sgl`), all 512 of 512 loaned WRITEs were still written
through the page cache. NVMe wants memory aligned to 4 bytes, and veth
with XDP puts every frame at a 256-byte headroom, so after the 14-byte
Ethernet header the TCP payload is 2 bytes off. This is the misplacement
the Xsight card has, which `xeu`'s `rx_ip_align=2` exists to fix,
**reproduced with no Xsight hardware**. veth was given the same setting,
for testing only ("veth: add rx_ip_align, extra headroom for frames copied
for XDP"); with it 512 of 512 went direct. The harness sets it through
`RX_IP_ALIGN` (2 with `STORAGE=sgl`, else 0). The reproducer, and one
design for a setting the network stack would own, are in
`../xeu-SGL-compat-receive-placement/README.md`.

**10. The harness could report a pass with nothing run.** David's
`analyze.py` says OVERALL PASS on an empty directory. The plan's analyze
stages now fail unless both phases' tables exist. `cells.tsv` also gained
three columns saying how nfsd issued each cell's WRITEs, which is what
problem 9 was found with.

**11. The plan had no copy cell that could be compared with a loan
cell.** Its `-off` cells have no budget and no held receive buffer; its
`-on` cells have both. Holding the buffer alone costs one connection up
to a third here, and a direct write to a new file waits on XFS, so the
two differed in three things at once. Every `-on` cell now has a `-copy`
twin (budget 0, the rest the same), two pairs also run on files that
already have their size (`SIZED=1`), and phase B has `edge-copy-n1`.

### How the kernel was built

- Mike's known-good 16 KiB config has no debug information, which the
  harness's config check requires for BTF. He chose to turn it on, then to
  cut the build down with `localmodconfig`.
- `localmodconfig` drops every module not loaded at the time, including
  ones the harness needs that its config check does not list (the loop
  device, pNFS, NFS ACLs, TLS, NVMe multipath). Every NFS, SUNRPC, NVMe,
  loop, TLS, zram and KUnit option was therefore restored from the full
  config. The three derived configs are in `/var/lib/zc-transplant/`.
- The result is 95 modules and builds in about 7 minutes at `-j4`.
- kernel-rpmtree's `--localmodconfig` cannot be used for this as it stands:
  it reduces after the harness options are merged and strips them.

### Not done

- **Anything at 4 KiB or 64 KiB** with these changes. Problems 2 and 4
  change behaviour at 4 KiB in some configurations, and problem 8 changes
  every throughput figure taken there.
- **Why a 1 MiB request takes 0.2 ms longer issued direct than by
  writeback** on the test device, which is the 6% left between direct and
  buffered loans; and the fifth in the stalled-storage cell.
- **What the held receive buffer costs with more than one connection, and
  on real hardware.** One connection on this rig loses up to a third.
- **A KUnit pair** to stand where the old "no budget" cells stood: several
  half-received WRITEs that together exceed a small region must hold
  nothing at a budget of 0, and never more than the budget with one.
- **Unexplained:** why the cell of problem 4 only slowed down on 4 KiB.
- **For 64 KiB, by reading the code only:** a misaligned write smaller than
  a page has no whole page in it, so it is written buffered whatever
  problem 3's knob says.
- **Sparse** has not run on any of these commits; the host's version is
  refused by the kernel's Makefile.

### State of the branch

The history from David's first Slipstream commit on was tidied on
2026-10-04, at Mike's request, so that it reads as the work is now
understood and not in the order it was discovered:

- **Four code commits lead the series, before David's first commit.**
  First two fixes to block-layer test devices that stand on their own:
  "null_blk: keep each memory-backed copy within one block" and "dm-delay:
  do not round a short delay up to the next timer tick" (2026-10-05). Then the
  write-split work: "nfs_common: split a write's direct middle at page
  boundaries" and "NFSD: count the misaligned-WRITE split gate in bytes,
  not pages". Those two correct code in the baseline, and sit there so
  Mike can fold them into it. Their two KUnit commits open the first KUnit
  section.
- **David's commits, and the fixes and tests made with them, come
  next, as he wrote them** but for one thing: the limit he introduced is
  named "budget" throughout (tunables, sysctls, the state structure), and
  "credit" is kept for what one WRITE draws against it. Each of his commits
  that this touched says so in a `[snitzer: ...]` note. His five code
  commits, Mike's three fixes, then the KUnit exposure and tests.
- **Mike's later work follows as one run**: 11 code commits (the last a
  test aid in veth, added 2026-10-05), then the
  KUnit commits (6 that expose internals, 8 tests), then the
  documentation. Two fixes to this work's own commits were folded into the
  commits they fixed (a transport that copies must stop waiting; a budget
  of 0 copies even when no budget is set anywhere, both now in "SUNRPC:
  Copy region records when the budget drawn on is 0"), and one into the
  commit that made it necessary (a budget change wakes waiters, now in
  "SUNRPC: Do not re-queue a transport that waits for region credit").
- **The documentation was regrouped by subject** and rewritten where its
  framing was wrong or it recorded dead ends.
- Mike pushed the tidied branch on 2026-10-05. The backup branches made
  on the 16 KiB host before the tidy-up and each later reordering were
  deleted that day at his word; the history before them is not kept.
- **With the KUnit suites enabled, the series does not build from "NFSD:
  count the misaligned-WRITE split gate in bytes, not pages" until "NFSD:
  give the KUnit split test the byte-counted gate"**, nine commits later at
  the head of the first KUnit section. The code commit renames a field the
  existing split test sets, and code comes before KUnit. That goes away
  when the pair is folded into the baseline, where the code and the test
  each go into the commit that introduced them. Without the KUnit suites
  every commit builds.
- `check-grouping.sh` says `grouping OK` against what Mike pushed (its
  default base). Over the whole series it needs the last commit of the
  first KUnit section as its base, because the series reads code, KUnit,
  code, KUnit, docs.
- **Not run on this history:** the per-commit bisect walk with
  `CONFIG_WERROR=y`, and sparse. The run recorded above is of the branch
  tip. The kernel was built at `82d7a6d21207`; everything after it on the
  branch is harness and documents.

## 64 KiB host, tardis1 (2026-10-07)

`tardis1.perf.hammer.space`: aarch64, 64 KiB pages, 64 CPUs, 63 GiB, a
live NFS server. Kernel `7.1.13-20.hs.458.loanpages_pool.el9.aarch64+64k`,
packaged RPMs built by Mike from the `-WIP` branch (`a1c37cac4922`);
sunrpc `299EF7D944C6D4A8DA14765`, nfsd `5575F68355368469250681E`.
`preflight.sh`: PASS on both boots. Results:
`/var/tmp/zc-testplan-7.1.13-20.hs.458.loanpages_pool.el9.aarch64+64k`;
the record is `results/64KiB.tsv`. Boot 1 took 53 minutes, boot 2 five.

This is the fourth kernel run here (builds 454, 455, 457, 458). Each of
the first three found a defect; only build 458's run is recorded.
`tardis1-boot1-testing.md` (builds 454 and 455) and
`tardis1-build457-handoff.md` (builds 457 and 458) are the working notes,
with the evidence for each defect below.

### In short

- **The whole plan passes at 64 KiB, on both kinds of storage, in two
  boots.** Every stage PASS or DONE; `analyze.py` OVERALL PASS for
  nvme-loop and for NVMe/TCP; no fio verify failure, hang or kernel
  warning in any cell.
- **The budget does its job.** With a budget set and the receive buffer
  held nothing is dropped and the region never runs out, and no cell
  overran its credit. The cells meant to drop did (`floor-refused` 484
  and 1729, `x-off-RN` 4137 and 3914).
- **A loan is written direct on storage that takes arbitrary pieces and
  through the page cache on storage that needs whole pages; a copy is
  always written direct**, one segment each.
- **One connection: copy and loan are level. Sixteen connections: the
  loan runs at about half to two thirds of its copy**, and pauses. This
  is the one result that differs from 4 KiB and 16 KiB. The credit
  factor of 150 is too low for sixteen connections; a higher one on the
  same region removes the pauses and not the whole gap (below).
- **Tested on veth, not on the Xsight card or the real NVMe.**

### What was measured

| pair (phase A) | conn | nvme-loop: copy / loan MB/s | NVMe/TCP: copy / loan MB/s | region MiB, loan (loop, tcp) | paused (loop, tcp) |
|---|---|---|---|---|---|
| `seq-sized` | 1 | 2174 / 2191 | 2174 / 2165 | 11.25, 11.75 | 0, 0 |
| `step` | 1 | 1315 / 1323 | 1303 / 1323 | 19.81, 19.81 | 0, 0 |
| `mixed-sized` | 16 | 2594 / 1236 | 2521 / 1142 | 17.00, 15.75 | 983, 940 |
| `step-n16` | 16 | 1912 / 1229 | 1846 / 1009 | 17.88, 16.88 | 100638, 94753 |
| `seq` (new file) | 1 | 438 / 2105 | 399 / 394 | 11.00, 19.75 | 0, 0 |
| `mixed` (new files) | 16 | 440 / 1156 | 447 / 847 | 16.75, 20.75 | 1077, 907 |

- **A copy holds 0.5 to 1.5 MiB of the 35 MiB region, a loan 11 to 20
  MiB** (32 to 57%), less of the region than at 16 KiB (64 to 70%).
- **The 16-connection gap.** Peak use of about 272 pages is sixteen
  records of 17 pages; a budget of 423 less the reserve of 30 admits
  fifteen records at a cost of 25. Phase TLP's 16-connection loan cells
  pause the same way. It is not XFS's beyond-EOF wait: these files have
  their size, and on nvme-loop the loan is not written direct.
- **The credit factor of 150 is too low for sixteen connections**
  (experiment of 2026-10-07; nvme-loop, the same region, `FACTOR9=` in
  `run-budget.sh`; `tardis1-build457-handoff.md` section 14). A 1 MiB
  record is admitted with 27 pages and 59% of them pause for more
  mid-receive, holding their credit; at 250 (44 pages) 16% pause, at 350
  (61 pages) none.

  | factor | `mixed-sized` at 1 ms: copy / loan | the same, no delay | `step-n16` no delay: copy / loan | in the 10 ms stall | records admitted at once |
  |---|---|---|---|---|---|
  | 150 | 2958 / 1060 | 2991 / 1538 | 2532 / 1740 | 719 / 50 | 15 |
  | 250 | 2789 / 1532 | not run | 2394 / 2318 | 709 / 80 | 9 |
  | 350 | 2232 / 1165 | 2651 / 2474 | 2221 / 2202 | 699 / 235 | 6 |
  | 350, its own 870-page region | 2581 / 1766 | 2619 / 2625 | about 2240 / 2245 | about 704 / 485 | 11 |
  | 350, a 1080-page region | 2261 / 1992 | 2581 / 2468 | about 2250 / 2205 | about 704 / 634 | 14 |
  | 350, a 1180-page region | 2544 / 1977 | 2587 / 2619 | about 2240 / 2220 | about 705 / 701 | 16 |

  - With the pauses gone and storage that does not wait, the loan is at
    its copy's rate.
  - When storage waits it is not: the budget is 423 pages at every
    factor, so a higher factor admits fewer records at once.
  - The factor slows the copy as well (2958 to 2232), because it also
    sets the held receive buffer's window clamp.
  - 350 on the region it derives (870 pages, a budget of 734; a fresh
    boot, handoff section 15) admits eleven records at once, not
    fifteen: the 60-page reserve admits nothing, so 674 pages do, at 61
    each. With no storage delay the loan is level with its copy.
    When storage waits it is at about two thirds of it, in the 1 ms
    mixed cell and in the 10 ms stall alike: eleven records at once is
    still a limit the copy does not have.
  - 350 on 1080 pages (a budget of 944; another boot, handoff section
    17) admits fourteen. It was sized for sixteen, from a sum that
    left out the reserve and took a 128 KiB receive buffer as 2 pages
    where it is 4. When storage waits the loan is at about nine tenths
    of its copy (88% at 1 ms, 90% in the stall); the 1 ms copy was
    itself lower on that boot (2261 against 2581). With no delay the
    loan is at 96%.
  - 350 on 1180 pages (a budget of 1044, 73.75 MiB; another boot,
    handoff section 19) admits sixteen. **The stall no longer costs the
    loan anything:** 99% of its copy in the stall and over the whole of
    `step-n16`, where denials fall from 95609 to 2 and retransmits from
    1568 to 3. With no delay the loan is level with its copy. **The
    1 ms mixed cell does not move:** the loan is 1977 (1992 with
    fourteen) and denials 3043 (3188); it is 78% of its copy and not
    88% because the copy was faster on this boot (2544 against 2261).
    Those denials have 13 to 53 records in flight (median 30), of all
    four record sizes: with a 1 ms storage delay a connection holds
    credit for more than one record at once, so sixteen 1 MiB records
    is not the limit in that cell. What would lift it is not shown.
  - **Pages in use at the peak** in the loan cells: 160 to 176 with six
    records at once (555 pages), 280 to 300 with eleven (870), 300 to
    348 with fourteen (1080), 312 to 408 with sixteen (1180). That is
    20 to 29 pages per record admitted against the 61 charged, and
    25.5 MiB at most, under the 32 MiB L3 the host reports. The copies
    hold 8 to 12 pages on every region.
  - There the factor is no longer quite enough: 481 of 190507 1 MiB
    records pause in `step-on-n16` (none on 555 pages), and a record
    holds up to 73 pages against the 61 it is admitted with.
- **New-file copies are at about 440 MB/s** where sized ones reach 2174
  to 2594: a direct write that starts beyond EOF waits on XFS
  (`../slipstream/XFS_BEYOND_EOF_DIRECT.md`). On nvme-loop the loan is
  written through the page cache and does not wait, which is why it
  looks five times faster in the `seq` pair.
- **Phase B** (region 114 pages, budget 30): `edge-at-n1` 363 and 332
  MB/s against `edge-copy-n1` 438 and 398, 1.5 to 2.25 MiB held, no
  pauses, no drops.
- **The `-off` cells do not drop here** except `x-off-RN`: `mixed-off`,
  `x-off-R`, `d-off-N` and `edge-off-n1` hold 0.25 to 3.25 MiB and drop
  nothing, where 4 KiB and 16 KiB fill the region and drop thousands.
  Not examined.

### The rest of the plan

- **KUnit:** xdr 7/7, svcsock 43/43, nfsd receive 6/6, nfsd4 receive
  10/10, no skip.
- **zc rig and SGL rigs:** bvecrepro MATCH; rig-cmp PASS; loan-assert 50
  of 50 connections fully direct; killswitch validated;
  system-correctness 517 direct, 0 vector. SGL rigs, `dd_fails=0
  cmp_mismatches=0` each: NVMe/TCP 160 of 160 direct, nvme-loop 156
  direct and 4 memory-misaligned, brd 9 direct and 151
  memory-misaligned.
- **LOCALIO, both rigs:** boundary, short-read and stable exit 0 and
  every writer finished. Reads per WRITE 0.418 / 0.003 / 0.766 on
  NVMe/TCP and 0.334 / 0.005 / 0.726 on nvme-loop at 47008, 96000 and
  6000 bytes; 0.0% resident. The two sizes under a page read back a
  third to three quarters of a page per WRITE (0.000 to 0.005 at 4 KiB):
  a cost of misaligned records smaller than a page, left alone.
- **Parked pages: 4** (8 in one phase).
- **Phase N:** `n-sum` refused as it should (`init 270 + B 338 over
  555`), `n-exit` fio exit 4 as intended. `n-reconf` copied 20 and 27
  records for want of a budget, where the other hosts show 0, and
  `n-own`, `n-default` and `n-copy` each show exactly 12.00 MiB held.
  Neither was examined.
- **Kernel log:** no WARNING, BUG, stall, allocation failure or
  invalidation failure on either boot (boot 1 as read at the time; the
  host keeps no journal across a reboot). Boot 2 shows only "operation
  not supported" for `DISCARD` on the nvme-loop test device.
- **TLP:** `step-on-n16` retransmits 6645 and 7714 times with no
  timeout; with quick acknowledgements or with probes off, 0.

### Problems found and fixed

Commits are named as on this branch on 2026-10-07; a regroup renames them.

1. **A loan was charged per skb fragment, not per region page** (build
   454). veth packs about seven MTU 9000 frames to a 64 KiB page, so a
   1 MiB WRITE was charged about 117 against a credit of 25 and every
   loan was throttled to a quarter of the budget. `dff7f2d4f04a`. Where
   a frame has a page to itself (16 KiB) or spans its own pages (4 KiB)
   the charge is what it was; that is from the code, not from a run.
2. **A waiting transport whose queued data TCP collapsed read on without
   admission and stayed at the head of the wait list**, stopping every
   other transport (build 457, `x-tight-RN`). Not specific to 64 KiB.
   `3ec476031fd9`; verified at runtime with rebuilt modules (80 of 80
   repetitions, the collapse seen once and survived) and by a KUnit
   case.
3. **Materializing a record that holds a merge page counted its bytes
   twice** and faulted the receive with a warning (build 457,
   `collapse-off`). `7abb45f322f7`; verified by its KUnit case only.
4. **A direct middle and a neighbour's buffered segment in one page
   failed a write with `-EIO`** through LOCALIO (build 455; the test did
   not notice). First answered by cutting every end at a page boundary
   (`v7.1.13-20`), which cost direct I/O for writes aligned to the
   device but not to a page; then by cutting per end again and having
   LOCALIO complete a direct write that shares a page before it returns
   (`62286e633852`, `08d80dc89a93`, `11e038c05ac5`). Only XFS was run.
5. **Tests and harness:** the capacity + 1 KUnit test spans skbs where
   the table exceeds `MAX_SKB_FRAGS` (`6294e82414e2`); the LOCALIO
   boundary test does not judge kick rounds for a record smaller than a
   page, and fails on a failed write or the kernel's invalidation
   message; `preflight.sh` finds a packaged kernel's config in
   `/lib/modules/<release>/config`.

### Not done

- **The 4 KiB and 16 KiB hosts have not been run with problems 1 to 4's
  changes.** Their columns in `RESULTS.md` are of `v7.1.13-19`.
- **The 16-connection gap:** the factor of 150 is shown to be too low.
  A factor of 350 on a larger region closes it, or nearly, when storage
  does not wait. In a 10 ms stall the loan is at about two thirds of
  its copy with eleven records admitted at once (870 pages), nine
  tenths with fourteen (1080) and level with sixteen (1180). With a
  1 ms delay on mixed record sizes it stays near 1980 MB/s at fourteen
  and at sixteen, 78 to 88% of a copy that varies between boots.
  How the factor and the budget should be computed for
  64 KiB is not settled.
- **The beyond-EOF rule** (`7809bfb4c8e9`: a write that starts beyond
  EOF is issued buffered) is committed and undecided, and build 458 does
  not have it. Measured once with rebuilt modules: `mixed-copy` 442 to
  2355 MB/s, with 77% of its WRITEs no longer direct.
- **The series is not regrouped** (`check-grouping.sh v7.1.13-20`:
  `grouping BROKEN`) and none of this is on the HS twin.
- **The Xsight card and the real NVMe.**

## What changes with the page size

The harness derives its page budgets from `getconf PAGESIZE`. At 4 KiB each
derived value is exactly the qualified constant. Run
`GEOMETRY=1 run-budget.sh` to print them, or `GEOMETRY=1 GEOMETRY_PS=65536`
for another page size:

| | 4 KiB | 16 KiB | 64 KiB |
|---|---|---|---|
| 1 MiB record | 256 pages | 64 | 16 |
| credit factor, MTU 9000 / 1500 | 150 / 280 (measured) | 190 / 170 | 150 / 150 |
| record cost, reserve | 385, 390 | 123, 130 | 25, 30 |
| default region | 4096 pages (16 MiB) | 1519 (24 MiB) | 555 (35 MiB) |
| phase N budgets | 2000 / 1600 / 1500 / 1000 / 400 | scaled by region / 4096 | same |

The factor for 16 and 64 KiB is computed from page_pool's frame placement.
Read `paused` and `overrun` in the cells with a budget: many pauses or
overruns mean it is low. At 16 KiB there were none (2026-10-05), so 190 and
170 are not low. At 64 KiB (2026-10-07) there were no overruns, but the 16-connection loan
cells pause, and 150 is low for them: at 350 they do not ("64 KiB host").

**Results that differ from the 4 KiB record.** The 16 KiB column is what
was measured on 2026-10-05. The 64 KiB expectations here held on
2026-10-07: no KUnit skip (the capacity + 1 test was rewritten to span
skbs), loan-assert 50 of 50, `vector` 0.

| check | 4 KiB | 16 / 64 KiB |
|---|---|---|
| KUnit `sunrpc-svcsock-rx` skips | 1 (`svcsock_rx_arena_bound_test`) | **none**. `run-kunit.sh` fails on any skip. At 64 KiB, if `capacity_plus_one` skips, check whether capacity + 1 > MAX_SKB_FRAGS before calling it a defect. |
| loan-assert bvecs / copied for a merge | 257 / 4096 | 65 / 16384; at 64 KiB 17 / 65536 |
| loan-assert fully-direct connections | 10 of 10 | 16 KiB: 50 of 50. The script tries 50 on larger pages and needs only one. |
| rig-cmp, system-correctness `vector` | 0 | 16 KiB: 0. A value above 0 would not be a regression (one whole page per merge) as long as the data compares. |
| `nvme-loop` over brd physical block | 4096 | the page size |
| veth MTU 9000 frame layout | head + 2 page frags | 16 KiB: one page per frame; 64 KiB: frames packed several to a page (so `../slipstream/README.md` section 7 finding 3, one frame per page, is not reproduced by veth at 64 KiB) |

## Working rules for the Claude session on the new host

The original host's Claude memory does not come along. These are its rules
for this tree, from Mike:

- **Never remove untracked files.** This includes `git clean`,
  `git stash -u`, `checkout -f`, `make clean` and `distclean`, worktree
  removal without an audit, and `tar -x` over the tree.
- **Never `git add -A`.** Stage explicit paths.
- **Never `git push`.** Mike pushes and force-pushes himself.
- **Never delete a branch without asking.**
- **Commits on `kernel-7.1.13/*`** end with `Assisted-by: Claude:<model-id>`
  and `Signed-off-by: Mike Snitzer <snitzer@kernel.org>`. Never use
  `Co-Authored-By:`.
- **Commit grouping in this series:**
  - Code commits come first and carry no KUnit references.
  - The KUnit section starts with the enablement commits, then the tests.
  - Docs (`NFSD_TCP_WRITE_ZEROCOPY/`, `AGENTS.md`) go at the end.
  - New work goes in as commits at the tip; a fix to an unpushed commit
    carries a `Fixes:` tag and is folded when Mike asks.
- **Required before handing work back from a test host** (end of a session,
  or before Mike pulls) whenever anything was committed on the 16 KiB host
  or tardis1. Mike requires this cleanup pass every time.
  **Suspended on tardis1 since 2026-10-07: Mike said not to worry about
  regrouping until he asks.** Until then do not run the check, report
  its result or offer a regroup; commit at the tip and stop.
  1. Run `transplant/check-grouping.sh`. Its base defaults to the branch's
     upstream, which is what Mike pushed. It checks code, then KUnit, then
     docs order; no mixed commits; every `Fixes:` tag on the branch; no
     `Co-Authored-By:`.
  2. If it fails, regroup:
     - make a backup branch `<branch>.pre-regroup-<date>` first;
     - `git rebase -i` with a scripted todo (`GIT_SEQUENCE_EDITOR`) that
       moves each code commit after the last code commit, each KUnit
       enablement after the last enablement, and each test after the last
       test;
     - check that the tree is identical (`git diff <backup> HEAD` is empty);
     - re-point every `Fixes:` tag the check lists as stale;
     - update docs that cite a moved SHA.
  3. Build-check every moved commit (`make W=1 C=1 net/sunrpc/`, or whatever
     directories the commits touch).
  4. Re-run `check-grouping.sh` until it says `grouping OK`. Report the
     backup branch and the new SHAs.
  `run-testplan.sh`'s summary stage runs the check too, so a broken layout
  shows in every test-plan result.
- **Label the code state first.** When reasoning about any result, first
  name the kernel, commit and modules it came from. Run the like-for-like
  control cell before attributing an effect.
- **Record results** as docs commits: the host's record in
  `transplant/results/`, the comparison in `transplant/RESULTS.md`, and
  the account in this file.
- **Keep only the last full run.** When a run is complete, results of
  earlier runs on that host come out of the documents and off the disk
  (`/var/tmp/zc-*`). Keep an earlier observation only where it explains a
  change to the code or the harness.
- **Read every cell.** A stage's DONE says the script finished. Check
  each cell's fio exit status and verify output, the kernel log, and how
  the WRITEs were issued before calling a stage clean.
- **Check that a build did what was asked** before measuring: the exit
  status of `make`, and the `.ko`'s date or symbols. A filter on its
  output can hide the error.
- **Words.** *Budget* is the configured limit; *credit* is what one WRITE
  draws against it. Describe results by what the storage can do ("needs
  whole pages", "takes arbitrary pieces") and by copy or loan, and say
  which kernel, commit and modules a result is from.
- **Compare like with like.** A copy cell and a loan cell are comparable
  only if they differ in the budget alone: same held receive buffer, same
  kind of file (growing or already sized), same storage.
- **Shell traps on these hosts:** `cp` and `mv` are aliased to ask before
  overwriting (use `\cp`, `\mv -f`); `pkill -f PATTERN` matches the shell
  running it.
- **Do not build anything Mike has not asked for.** On tardis1, nothing is
  built at all.
- **Before changing a copy, loan or storage path, read the design**:
  `../slipstream/README.md`, `../slipstream/BUDGET_SCOPE.md`,
  `../on-die-receive-copy/SPEC.md`, `../NVME_SGL_SUPPORT_PROJECT.md`. The
  intended setup differs by kind of storage, and a result means nothing
  until it is known which is under test.
