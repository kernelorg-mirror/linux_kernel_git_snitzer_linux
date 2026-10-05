# Transplanting the page-loan test harness to another host

Everything needed to take branch `kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY`
to a new aarch64 test host, build and boot its kernel there, and run the
whole test plan, with no file from the original 4 KiB development VM.

Two transplants use it:

1. **The 16 KiB host (dry run).** A host whose CPU implements the 16 KiB
   granule. The kernel is built **on this host** from the host's own
   known-good `.config`, plus the options this work needs.
2. **tardis1 (64 KiB, Xsight E1).** Nothing is compiled on tardis1. Its
   kernel is the **packaged** full 64 KiB build from this branch's
   `redhat/configs`, installed from RPMs. Only the harness, a plain clone of
   the branch, runs there.

The 16 KiB run proves the procedure and the harness at a page size larger
than 4 KiB. tardis1 then repeats it with an install in place of the build.

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

## The 16 KiB host

Prerequisites, from Mike:
- a clone of this branch at `/root/kernel/linux`;
- kernel-rpmtree at `/root/kernel/kernel-rpmtree`;
- the host's known-good 16 KiB `.config`;
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
```

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
- the Xsight NIC driver built against those RPMs
  (`build-kernel-in-ubi.sh --kernel-rpms … --kmod-dir xsight=<ver>`, giving
  `kmod-64k-xsight`), with the `rx_ip_align` patch if that experiment is
  wanted (`../xeu-SGL-compat-receive-placement/`)

Then on tardis1, with a clone of the branch (nothing in it is built):

```sh
T=NFSD_TCP_WRITE_ZEROCOPY/transplant
$T/host-bootstrap.sh
$T/preflight.sh        # no manifest: build identity skipped; config from /boot
$T/run-testplan.sh     # bvecrepro is SKIPped (needs a built tree); all else runs
```

## The test plan (`run-testplan.sh`)

| boot | stage | what | judged by |
|---|---|---|---|
| any | `preflight` | `preflight.sh` | PASS/FAIL |
| 1 (fresh) | `budget-D` | `PHASE=D run-budget.sh`: two devices share one region and budget. It must run before anything creates the region. | tables vs D-s4 / D4 (`../slipstream/BUDGET_SCOPE.md` 8.1, 8.6) |
| 1 | `kunit` | `run-kunit.sh`: all four suites | PASS/FAIL; skip rule below |
| 1 | `bvecrepro` | `run-bvecrepro.sh`, which needs a tree built for the running kernel | PASS/FAIL, or SKIP on a packaged kernel |
| 1 | `rig-cmp`, `loan-assert`, `killswitch`, `system-correctness` | the zc rig runtime checks | PASS/FAIL (each exits with its failure count) |
| 1 | `zc-down`, `sgl-up`, `sgl-brd/tcp/loop`, `sgl-down` | NVMe SGL rigs (`../NVME_SGL_SUPPORT_PROJECT.md` 6) | DONE; compare dispositions |
| 1 | `localio-tcp`, `localio-loop` | `run-sgl-localio-ab.sh` (LOCALIO boundary, short-read and stable tests) | DONE; each test prints its own exit status |
| 1 | `budget-A`, `budget-TLP`, `budget-X`, `budget-N` | `run-budget.sh` phases on the same region, on storage that needs whole pages (nvme-loop) | tables vs the 4 KiB results (`../slipstream/README.md` section 8, `BUDGET_SCOPE.md` 8) |
| 1 | `budget-A-sgl`, `budget-X-sgl` | phases A and X again with `STORAGE=sgl`: storage that takes arbitrary pieces (NVMe/TCP), veth placing the payload on a 4-byte boundary (`RX_IP_ALIGN=2`) | tables beside the nvme-loop ones; `wr_direct` |
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

## Validated on 4 KiB (2026-10-03)

The runner and every changed script were run on the original 4 KiB VM
(kernel `7.1.13-19.hs.451` #38, modules = tip). `run-testplan.sh --only`
covered each stage that needs no fresh boot, with results in
`/var/tmp/zc-testplan-4k-validate/`:

- **PASS:** bvecrepro, rig-cmp, loan-assert, killswitch, system-correctness.
- **DONE:** zc-down (left nothing behind), sgl-up, sgl-brd/tcp/loop, sgl-down.
- **LOCALIO tcp and loop:** DONE. The boundary, short-read and stable tests
  all exit 0. At record size 47008 the result was 0.004 reads per WRITE on
  tcp and 0.007 on loop; at 6000 it was 0.000; 0.0% resident.
- **budget-X:** DONE, with derived budgets identical to the constants
  (budget 2698, reserve 390, tight 390). Its cells match X-s4 and X3.
- **kunit:** PASS, run separately, 7/7, 38 + 1 expected skip of 39, 6/6,
  10/10.
- **The runner's gating:** it stops at budget-D on a boot that is no longer
  fresh.

**Full plan, boot 1** (2026-10-04, fresh boot of #38, `run-testplan.sh --force
--results /var/tmp/zc-testplan-4k-fresh`; `--force` because the VM keeps its
own nfs.conf, the preflight's one failure):

- **budget-D** ran through the fresh-boot gate and matches D4 cell for cell
  (0 allocation failures with a budget set, 2785 / 5570 admitted).
- **PASS:** kunit, bvecrepro, rig-cmp, loan-assert, killswitch,
  system-correctness.
- **DONE:** zc-down, sgl-up/brd/tcp/loop/down.
- **LOCALIO tcp and loop:** all tests exit 0; 0.006 reads per WRITE at
  47008, 0.000 at 6000, 0.0% resident.
- **budget-A:** cells with a budget have 0 drops. It wrote `parked.txt`
  (257).
- **budget-TLP and budget-X:** X matches X3.
- **budget-N:** every cell passes, including n-reconf with fio rc 0 at 103
  MB/s, the sum check and n-exit.
- **The runner then stopped at budget-B** with "reboot, then run again"
  (exit 3), as designed.

**Full plan, boot 2** (fresh boot, same command):

- **budget-B** ran at the configured minimum, 967 pages (A's 257 parked +
  256 + 390 + 64). Every edge cell admitted with 0 drops and 0 allocation
  failures, the region peaking at 416-480 pages.
- **analyze-AB:** David's `analyze.py` gives **OVERALL PASS** on A and B.
- **summary:** DONE.

The whole plan has now run on 4 KiB, both fresh-boot stages included.
One runner fix came out of it: phases TLP, X and D re-measure the parked
pages into the same `parked.txt`, so budget-B now reads phase A's own value,
kept as `parked-A.txt`. This run used A's logged 257.

**`build-test-kernel.sh` trial** (2026-10-04, `--no-install`, in a scratch
worktree at `cf9bc64125da`, base = the hs.448 production config):
- **Merge:** `merge-config.sh` added exactly the seven options the base
  lacked (four KUnit suites, veth, null_blk, dm-delay). The resolved config
  passes `check-config.sh` and is identical to the qualified hs.451 KUnit
  config except `CONFIG_LOCALVERSION`, which `build-kernel-on-host.sh` sets
  itself.
- **Build:** `build-kernel-on-host.sh --build-number 451` named the kernel
  `7.1.13-19.hs.451.el9.aarch64+` and built it. The worktree had just been
  built with the same config, so make had nothing to do.
- **Manifest:** written; sunrpc srcversion `CCD0706E`, the qualified build's.
- **Not exercised:** a long compile from a different config, and the
  install, boot-entry and `/boot/config` steps. The 16 KiB host is their
  first real run.

**Bisect walk of the TCP ratio-hook section**
(`17e45531ee8a..cf9bc64125da`, hs.451 KUnit config with
`CONFIG_WERROR=y`): 9/9 clean (4 built, 5 docs skipped). Sparse against a
cold whole-tree baseline finds nothing the branch introduced. The last two runs of D and B on this VM are D4 and phase B
(BUDGET_SCOPE 8.6, slipstream README).

## 16 KiB host (2026-10-04)

The kernel was built on this host, the whole test plan was run, and five
problems were found and fixed on the way. This section is arranged by what
a reader needs to know, not by the order things happened; the order is
listed near the end.

### In short

- **The whole test plan passes at 16 KiB on one kernel**, both boots: no
  data verification failure, no hang, no kernel warning.
- **Four kernel fixes were needed first:** where a misaligned write is cut
  into direct and buffered parts; the unit of the knob that decides whether
  to cut at all; what happens to a fixed receive region nobody set a limit
  for; and a data-losing bug in null_blk, the test storage.
- **What the results mean depends on what the storage can do.** There are
  two cases, set out below, and the right configuration differs between
  them. Most of the budget harness runs on only one of the two.
- **Only 16 KiB was run.** Nothing here was run at 4 KiB or 64 KiB.

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
  and on this test rig alike; see "NVMe/TCP storage in the budget harness"
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

Kernel `7.1.13-19.hs.451.el9.aarch64+` build #12, built from the branch tip
that carries the four fixes; sunrpc `3954E9CB`, nfsd `4E439F2B`, nfs_dio
`A03FA30E`, null_blk `DC3E1848`. Results in
`/var/tmp/zc-testplan-16k-run3/`. Every cell's fio exit status, run time and
verification output was read, not only the stage's status.

**Storage that needs whole pages** (the budget harness, `run-budget.sh`,
whose device is nvme-loop). Region of 1519 pages, 24 MiB.

The intended setup, budget 0 with the receive buffer held:

| cell | connections | MB/s | packets dropped | most region pages in use |
|---|---|---|---|---|
| `step-rcvbuf-n16` (storage stalls 10 ms mid-run) | 16 | 1496 | 0 | 112 |
| `mixed-rcvbuf` (mixed write sizes) | 16 | 89 | 0 | 128 |

- All their WRITEs were copied and written direct: 170568 of 170568 in a
  traced re-run.
- The region is barely touched: about 2 MiB for 16 connections. A re-run
  on a region of only 306 pages (4.8 MiB) gave 1472 and 93 MB/s, no drops,
  the same 112 pages.

The same setup without the receive buffer held:

| cell | connections | MB/s | packets dropped | most region pages in use |
|---|---|---|---|---|
| `step-off` | 1 | 1398 | 0 | 1446 |
| `seq-off` | 1 | 57 (explained below) | 0 | 258 |
| `mixed-off` | 16 | 83 | 8718 | 1519, all of it |
| `x-off-R` | 16 | 83 | 8390 | 1519, all of it |

- With several connections the region fills with data nfsd has not read
  yet and the card drops packets. The data still verifies and nothing
  hangs. The receive buffer has to be held.

With a budget set, so that WRITEs are loaned:

| cell | connections | MB/s | packets dropped | most region pages in use |
|---|---|---|---|---|
| `step-on-n16` | 16 | 1527-1591 | 0 | about 1010 |
| `mixed-on` | 16 | 114-117 | 0 | about 1050 |
| `seq-on` | 1 | 416 | 0 | 967 |

- Every loaned WRITE here was written buffered (512 of 512 traced), as the
  storage requires. So these cells show that the budget does its job:
  nothing dropped, nothing waiting for ever, the region never exhausted.
  They do not show loans paying off, because on this storage they cannot.
- They hold about ten times as much of the region as the intended setup.

**Storage that takes arbitrary pieces** (NVMe over TCP).

- Only the plan's SGL stage runs on it, with no region and no budget: 10
  connections of 16 MiB, loans on, 0 compare mismatches, 124 of 160 WRITEs
  direct.
- **The budget has never been tested on this kind of storage**, which is
  where it matters most: loans that stay direct and hold region pages for
  as long as the device takes. The budget harness needs an NVMe/TCP variant
  for that.

**The budget's own behaviour**, whatever the storage. All on nvme-loop:

- Two devices sharing one region (`budget-D`, run here for the first time
  at 16 KiB): no allocation failures; the two clients get 57 MB/s each.
- A separate budget per network namespace, a default budget, changing a
  budget under load, and a namespace going away (`budget-N`): each WRITE is
  counted against the right budget; `n-reconf` completes with fio rc 0;
  the region is whole afterwards. `n-exit` ends with fio rc 4, as intended:
  it deletes the server.
- A client whose packets do not come from the region is never limited by
  the budget (`budget-X`): all six of its cells verify.
- The smallest workable region (`budget-B`, on its own fresh boot): 306
  pages; with the budget equal to one WRITE's worth, WRITEs proceed one at
  a time, 52 MB/s, nothing dropped, the region never more than 160 pages
  in use. David's `analyze.py` gives OVERALL PASS on phases A and B.

### The rest of the plan

- **KUnit:** 7/7, 41/41, 6/6, 10/10, no skips.
- **PASS:** bvecrepro; rig-cmp (30 x 16 MiB, 0 mismatches); loan-assert
  (50 of 50 connections fully direct); killswitch; system-correctness.
- **SGL rigs:** 0 compare mismatches on brd, NVMe/TCP and nvme-loop.
- **LOCALIO:** the boundary test exits 0 on both rigs (0.003 reads per
  WRITE for 47008-byte records); the short-read and stable tests exit 0.

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
128 KiB on 64 KiB. Replaced by `direct_misaligned_min_bytes`, default 8192.
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

### Why one copy cell was slow

In the run above `seq-off`, which writes a new file in order over one
connection, ran at 57 MB/s where the same cell with loans ran at 416.
That was not the cost of a copy. Two things were:

- **The test storage was ten times slower than asked.** dm-delay kept the
  time a write is due in timer ticks, and this kernel ticks at 100 Hz, so
  the 1 ms the harness asks for became 10 ms. Fixed on 2026-10-05 ("dm-delay:
  do not round a short delay up to the next timer tick"): a 1 ms delay now
  measures 1.16 ms.
- **XFS makes direct writes to a growing file wait for each other.** On
  this storage a copied WRITE is written direct. Eight nfsd threads write
  the file's WRITEs out of order, and XFS makes a direct write that starts
  beyond the current end of the file wait for every direct write in
  progress. So the eight queue behind one another, and each pays the
  storage delay in turn. This is real and would affect any server writing
  a new file direct from several threads; the slow storage multiplied it.
  `../on-die-receive-copy/SPEC.md` section 3.4 is corrected for it.

With the delay fixed, on the phase B region (306 pages), every write
verified:

| storage | cell | written | MB/s before | MB/s after |
|---|---|---|---|---|
| nvme-loop | seq-off (copy) | direct | 55 | 355 |
| nvme-loop | seq-on (loan) | buffered | 52 | 320 |
| NVMe/TCP, payload placed | seq-off (copy) | direct | 53 | 350 |
| NVMe/TCP, payload placed | seq-on (loan) | direct | 52 | 315 |

- **Every throughput figure recorded before this fix, in any cell with a
  storage delay under 10 ms, was set by the tick and not by the code under
  test.** That includes the 16 KiB tables above and the 4 KiB results.
  Their drop, admission and region-use columns stand; their MB/s columns
  do not compare anything.
- These four cells are one run each. They are enough to show the delay is
  honest, not to rank copy against loan.

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

### Order of events

1. Build; first boot: panic in KUnit (problem 1). Second boot: the same,
   from another stage that loads the same suite.
2. Third boot, a first pass through the plan: LOCALIO boundary test fails
   (problem 2); `budget-D` silently not run (problem 6).
3. Problems 2, 3 and 7 fixed; kernel rebuilt; second pass: a cell hangs
   (problem 4), and fio verification failures are found in this pass and,
   looking back, in the first (problem 5).
4. Problems 4, 5 and 6 fixed; kernel rebuilt (build #12); third pass, both
   boots: clean. That pass is what "What was measured" reports.

### Not done

- **The whole plan with the NVMe/TCP stages.** Only the cells listed below
  have run: loans written direct under a budget in four cells, none of
  phase X, and phase A not on its own region.
- **The veth and dm-delay changes on a freshly booted kernel.** Each was
  built and loaded as a module into the running kernel. The kernel has
  been rebuilt and installed with both, but not booted.
- **Anything at 4 KiB or 64 KiB** with these fixes. Problems 2 and 4 change
  behaviour at 4 KiB in some configurations.
- **Throughput results with the storage delay fixed.** Only the four
  cells in "Why one copy cell was slow" have run since. Every MB/s figure
  before that needs re-measuring.
- **A KUnit pair** to stand where the old "no budget" cells stood: several
  half-received WRITEs that together exceed a small region must hold
  nothing at a budget of 0, and never more than the budget with one.
- **Unexplained:** why the cell of problem 4 only slowed down on 4 KiB, and
  why it came free after nine minutes in the first pass here.
- **For 64 KiB, by reading the code only:** a misaligned write smaller than
  a page has no whole page in it, so it is written buffered whatever
  problem 3's knob says.
- **Sparse** has not run on any of today's commits; the host's version is
  refused by the kernel's Makefile.

### NVMe/TCP storage in the budget harness (2026-10-05)

`run-budget.sh` takes `STORAGE=prp` (nvme-loop, as before) or `STORAGE=sgl`
(NVMe/TCP to 127.0.0.1, same null_blk and delay device behind it), and the
plan repeats phases A, X and B on the second, so each page size gets both
sets of results.

**What it found first: on NVMe/TCP, loaned WRITEs were not written
direct.** NVMe wants memory aligned to 4 bytes. veth with XDP puts every
frame at a 256-byte headroom, so after the 14-byte Ethernet header the TCP
payload is 2 bytes off a 4-byte boundary. nfsd sees that and falls back to
a buffered write. This is the same misplacement the Xsight card has, which
`xeu`'s `rx_ip_align=2` exists to fix, **reproduced here with no Xsight
hardware**.

**So veth was given the same setting**, for testing only ("veth: add
rx_ip_align, extra headroom for frames copied for XDP"). `run-budget.sh`
sets it through `RX_IP_ALIGN`, which defaults to 2 with `STORAGE=sgl` and 0
otherwise, and records it in `rig.txt`. With it, every loaned WRITE on
NVMe/TCP is written direct. `RX_IP_ALIGN=0 STORAGE=sgl` shows the problem
again. Details: `../xeu-SGL-compat-receive-placement/README.md`.

Measured on the 16 KiB host, region 306 pages, MTU 9000, 1 MiB WRITEs,
every write verified by fio. The kernel is as in "State of the branch" plus
the veth module rebuilt with the parameter.

| storage | placement | cell | budget | MB/s | drops | peak pages | admitted | direct | buffered, pages dropped |
|---|---|---|---|---|---|---|---|---|---|
| nvme-loop | 0 | seq-off (copy) | 0 | 55 | 0 | 256 | 0 | 512 | 0 |
| nvme-loop | 0 | seq-on (loan) | 146 | 52 | 0 | 144 | 512 | 0 | 512 |
| NVMe/TCP | 0 | seq-off (copy) | 0 | 53 | 0 | 240 | 0 | 512 | 0 |
| NVMe/TCP | 0 | seq-on (loan) | 146 | 52 | 0 | 144 | 512 | 0 | 512 |
| NVMe/TCP | 2 | seq-off (copy) | 0 | 53 | 754 | 306 | 0 | 512 | 0 |
| NVMe/TCP | 2 | seq-on (loan) | 146 | 52 | 0 | 144 | 512 | **512** | 0 |
| NVMe/TCP | 0 | edge-at-n1 | 130 | 52 | 0 | 144 | 256 | 0 | 256 |
| NVMe/TCP | 0 | edge-at-step | 130 | 676 | 0 | 144 | 77344 | 0 | 77344 |
| NVMe/TCP | 0 | edge-below-n1 | 194 | 52 | 0 | 144 | 256 | 0 | 256 |
| NVMe/TCP | 2 | edge-at-n1 | 130 | 52 | 0 | 144 | 256 | **256** | 0 |
| NVMe/TCP | 2 | edge-at-step | 130 | 618 | 0 | 144 | 70683 | **70683** | 0 |
| NVMe/TCP | 2 | edge-below-n1 | 194 | 52 | 0 | 144 | 256 | **256** | 0 |

- The seq cells are two cells of phase A run by hand on the phase B
  region. The edge cells are phase B cells through the plan's
  `budget-B-sgl` stage.
- **With the payload placed, the budget holds while loans are written
  direct**: no drops, no allocation failures, and the same 144-page peak as
  when the loans were going through the page cache. This is the first run
  of the configuration the second kind of storage is meant to use.
- **The seq-off cell that dropped 754 packets** is a copy cell with the
  receive buffer not held, the third row of the table in "Two kinds of
  storage". The warm-up cell, the same configuration, dropped packets in
  two of four runs (291 and 247) whatever the placement. Both ran on a
  region much smaller than phase A is meant to use. It is not caused by the
  placement, but it has not been looked at further.
- Throughput in the 52 MB/s cells is set by the test storage's delay, so
  these runs say nothing about what the direct write is worth.

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
- Nothing is pushed; the history before the tidy-up is kept on
  `kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY.pre-cleanup-2026-10-04`.
- **With the KUnit suites enabled, the series does not build from "NFSD:
  count the misaligned-WRITE split gate in bytes, not pages" until "NFSD:
  give the KUnit split test the byte-counted gate"**, nine commits later at
  the head of the first KUnit section. The code commit renames a field the
  existing split test sets, and code comes before KUnit. That goes away
  when the pair is folded into the baseline, where the code and the test
  each go into the commit that introduced them. Without the KUnit suites
  every commit builds.
- `check-grouping.sh` says `grouping OK` when given the last commit of
  that first KUnit section as its base.
- **The budget's names were settled after the 16 KiB run above.** That
  run used the tunables under their earlier names (`svc_tcp_rx_credit_pages`
  and so on). Since the rename: the four KUnit suites pass, `preflight.sh`
  passes, and one loaned and one copied cell of `run-budget.sh` ran clean
  with `analyze.py` passing. The whole plan has not been re-run.
- **Not re-run on the tidied history:** the per-commit bisect walk with
  `CONFIG_WERROR=y`, and sparse. The source is the tree that was tested
  above, less one empty lock/unlock pair in a KUnit-only helper.

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
overruns mean it is low. At 16 KiB there were none (2026-10-04), so 190 and
170 are not low; 64 KiB is unmeasured.

**Results that differ from the 4 KiB record.** The 16 KiB column is what
was measured on 2026-10-04; 64 KiB is expectation only.

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
- **Record results** in `NFSD_TCP_WRITE_ZEROCOPY/` (`TESTING.md`,
  `slipstream/BUDGET_SCOPE.md`) as docs commits.
- **Do not build anything Mike has not asked for.** On tardis1, nothing is
  built at all.
