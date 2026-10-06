# tardis1 (64 KiB): the first boot-1 run, what it found and what was changed

Working notes for the 64 KiB host, written 2026-10-05. tardis1 has no Claude
memory that carries over between sessions, so **this file is the record of
where the work stands** until a kernel with the changes below has been run.
A new session on tardis1 should read this after `AGENTS.md` and
`transplant/README.md`.

Nothing here has been folded into `README.md`, `RESULTS.md` or
`results/64KiB.tsv`. That is deliberate: the run was on a kernel whose loans
were throttled by a defect (section 3), so its figures are a "before", not
the 64 KiB record. Section 9 says what gets written where once the fixed
kernel has run.

## 1. Where things stand

| | |
|---|---|
| host | `tardis1.perf.hammer.space`, aarch64, 64 KiB pages, 64 CPUs, 63 GiB |
| kernel run | `7.1.13-19.hs.454.loanpages_pool.el9.aarch64+64k`, packaged RPMs, built by Mike from `42c4c8306b98`; sunrpc srcversion `4882A60124EF5C26B2AF247`, nfsd `4E439F2B24A4A7C3A8521DE` |
| same code as | the 4 KiB host's run (`26e799b10e8e`): the two commits differ only in docs and harness files, and sunrpc's srcversion matches |
| plan | boot 1 complete (2026-10-05 22:12 to 23:08, 56 minutes). **Boot 2 not run, on purpose** |
| branch tip | `d063737e2ed3` pushed by Mike 2026-10-05 (section 7); only later edits to this file come after it |
| new kernel | `7.1.13-19.hs.455.loanpages_pool.el9.aarch64+64k`, installed and booted 2026-10-06; sunrpc srcversion `9611113753DDA35599A6AE2` (differs from 454's, as it must), nfsd unchanged |
| build 455 | **the whole plan is complete**: boot 1 (2026-10-06 11:11 to 12:03, **section 10**) and boot 2 (12:38 to 12:43, **section 11**), every stage PASS or DONE, `analyze.py` OVERALL PASS on both kinds of storage |
| waiting on | Mike: a kernel built with the page-cut change (**section 10.3**; committed 2026-10-06, not built), reconciled with the simplified version of that change and the `-20` baseline. Then the LOCALIO stages again on it, and only then the 64 KiB record. The 16-connection gap (10.2) is still open |

Short version of the findings:

1. **A code defect, fixed, not yet built:** a loaned WRITE was charged
   against the region budget per skb fragment, not per region page. At
   64 KiB that is about seven times too much, and it throttled every loan
   (section 3).
2. **A test ceiling that does not hold for records smaller than a page,**
   changed: the LOCALIO boundary test's DONTCACHE kick-round check
   (section 4).
3. **One KUnit skip that fails the stage** and is structural at 64 KiB;
   the test was rewritten afterwards to run there, not yet built
   (section 5).
4. **One harness fix:** `preflight.sh` could not find a packaged kernel's
   config on this host (section 6).
5. **Safety held throughout:** with a budget set, no cell dropped a frame
   or failed an allocation, and every fio verify passed. But the region was
   never more than about a quarter full, so the throttle has not been
   tested under real pressure at 64 KiB yet (section 3.5).

## 2. What was run, and each stage's result

Results: `/var/tmp/zc-testplan-7.1.13-19.hs.454.loanpages_pool.el9.aarch64+64k/`
(500 MB). Run log: `/var/tmp/zc-run-logs/boot1.log`.

| stage | result | read as |
|---|---|---|
| `preflight` | PASS | after the config fallback (section 6) and `host-bootstrap.sh` |
| `budget-D`, `budget-D-sgl` | DONE | clean: fio 0, no drops, no allocation failures; overrun non-zero (section 3) |
| `kunit` | **FAIL** | 63 pass, 0 fail, 1 skip; the skip fails the stage (section 5) |
| `bvecrepro` | PASS | first real run against a packaged kernel's `-devel` files; `MATCH (all 1048576 bytes correct)` |
| `rig-cmp`, `loan-assert`, `killswitch`, `system-correctness` | PASS | |
| `zc-down`, `sgl-up`, `sgl-down` | DONE | |
| `sgl-brd`, `sgl-tcp`, `sgl-loop` | DONE | no `dd` failures, no compare mismatches (section 8.1) |
| `localio-tcp`, `localio-loop` | DONE | **boundary test exited 1** in the plan's run; exit 0 after the test change (section 4). short-read and stable exit 0 both times |
| `budget-A`, `-TLP`, `-X`, `-N` (nvme-loop) | DONE | clean by the plan's rule; loans throttled (section 3) |
| `budget-A-sgl`, `-TLP-sgl`, `-X-sgl`, `-N-sgl` (NVMe/TCP) | DONE | the same |
| `budget-B`, `budget-B-sgl`, `analyze-AB`, `analyze-AB-sgl`, `summary` | not run | boot 2, skipped on this kernel |

"Clean by the plan's rule" means: every cell's fio exit status is 0, and
with a budget set there are no drops and no allocation failures. Cells that
are meant to drop did, as on the other hosts:

- `floor-refused` (phase A): 543 drops on nvme-loop, 1938 on NVMe/TCP, the
  region full at 555 pages. The 16 KiB and 4 KiB records show drops in this
  cell too.
- `x-off-RN` (phase X, two clients, no budget): 4184 drops on nvme-loop,
  4454 on NVMe/TCP, region full at 555. This is the case the budget exists
  to prevent.
- `n-exit` (phase N): fio exit status 4. The cell stops nfsd under load;
  both other hosts' records show 4.

Kernel log during the run: no WARNING, BUG, stall or allocation failure.
Only these, none from the series:

- `operation not supported error ... WRITE_ZEROES` / `DISCARD` on the
  nvme-loop test device (`nvme9c9n1`);
- `NFSD: Unable to initialize client recovery tracking! (-110)` /
  `Is nfsdcld running?` when nfsd starts in a test namespace.

Timing: boot 1 takes 56 minutes here (50 on the 4-CPU hosts). The quick
stages take seconds; phase A is 11 to 17 minutes per kind of storage.

## 3. The budget charged fragments, not pages (code, fixed in `c84e1147bde7`)

### 3.1 What was seen

On files that already have their size, a loan was far slower than the copy
beside it, on both kinds of storage. On the 16 KiB and 4 KiB hosts these
pairs are level.

| cell (phase A) | storage | MB/s | peak region pages | budget | paused | overrun | written |
|---|---|---|---|---|---|---|---|
| `seq-sized-copy` | nvme-loop | 2156 | 8 | 0 | 0 | 0 | direct |
| `seq-sized-on` | nvme-loop | 1000 | 80 | 475 | 758 | 0 | dontcache |
| `mixed-sized-copy` | nvme-loop | 2974 | 8 | 0 | 0 | 0 | direct |
| `mixed-sized-on` | nvme-loop | 307 | 132 | 419 | 2274 | 835 | dontcache |
| `seq-sized-copy` | NVMe/TCP | 2191 | 8 | 0 | 0 | 0 | direct |
| `seq-sized-on` | NVMe/TCP | 983 | 80 | 479 | 779 | 0 | direct |
| `mixed-sized-copy` | NVMe/TCP | 2664 | 8 | 0 | 0 | 0 | direct |
| `mixed-sized-on` | NVMe/TCP | 320 | 124 | 423 | 2015 | 592 | direct |

For comparison, 16 KiB `seq-sized-on` on nvme-loop: 1802 MB/s against 1832
for its copy, 966 of 1391 budget pages held, 0 pauses.

The same shows in every loan cell of every phase: peak region use of 80
pages on one connection and 124 to 148 on sixteen, a quarter of the budget
or less, with pauses and overruns in the thousands (over 110000 pauses in
the 16-connection `step` cells; `step-on-n16` 750 to 895 MB/s against 1836
to 1911 for its copy). At the end of boot 1 the counters stood at
admitted 1086989, denied 1307285, paused 1051126, overrun 1537572.

The growing-file pairs hide it: the copy there is held to about 440 MB/s by
something else, so a throttled loan at 1002 MB/s (`seq-on`, nvme-loop) looks
faster.

### 3.2 The cause

- The budget is in pages. A WRITE is admitted at a cost of
  `pages * factor / 100 + 1`: 25 for a 1 MiB WRITE at 64 KiB (16 pages,
  factor 150).
- As each skb arrives the record is charged for what it holds:
  `held = state->region_count + need`, where `need` comes from
  `svc_tcp_rx_skb_region_pages()`. That function counted **at least one
  per skb fragment** in the region, and `region_count` counted one per
  loan entry. Its own comment said "region loan entries".
- Entries and pages agree only while a frame has a receive page to itself,
  which is so at 16 KiB with MTU 9000. At 64 KiB veth packs about seven
  9000-byte frames into a page: a 1 MiB WRITE is about 117 fragments on
  about 17 pages.
- 475 / 117 is about four WRITEs. Four WRITEs hold about 80 pages, which is
  the peak `used_pages` of every one-connection loan cell.

### 3.3 Ruling out the write path

On nvme-loop a loan is written through the page cache (dontcache) and a
copy direct, so the write path was a candidate. On NVMe/TCP with
`RX_IP_ALIGN=2` the loans are written direct, exactly like the copies
(`wr_direct` equals the WRITE count in every loan cell), and the gap, the
pauses and the 80-page peak are unchanged. The throttle is the cause.

### 3.4 The fix

`c84e1147bde7` "SUNRPC: Charge a TCP receive loan for the region pages it
holds, not its fragments" (`net/sunrpc/svcsock.c`,
`include/linux/sunrpc/svcsock.h`):

- a fragment in the same region page as the one before it adds nothing
  (`svc_tcp_rx_region_span()`);
- across skbs too: the charge for a new skb starts from the last region
  page the record loaned, kept in a new `state->region_last`
  (`svc_tcp_rx_skb_region_pages_after()`);
- `region_count` in `svc_tcp_rx_append()` follows the same rule, so what a
  record is charged still covers what it is counted as holding. (Dropping
  elements from a sequence never adds runs, so the appended fragments can
  not count more than the skb's did.)
- it stays an **upper bound**: a page seen again after a different page
  counts again;
- `region_last` is cleared in `svc_tcp_rx_retract_last()` and in
  materialize, so a page whose loan was dropped counts if loaned again;
- `svc_tcp_rx_skb_region_pages()` keeps its signature (it is the KUnit
  entry point and the test for "does this skb carry region pages");
- where a frame has a page to itself (16 KiB) or spans pages of its own
  (4 KiB) the count is what it was. **The other hosts' results should not
  move, but that is an expectation, not a measurement.**

`19a1bb8479e4` adds the KUnit cases: two fragments of one region page in an
skb count once (whole skb and a byte range covering both), and a second
loan of the page a record holds last leaves `region_count` unchanged.

`slipstream/BUDGET_SCOPE.md` 4.2 has a dated bullet (in `1e38df3a02fe`).

**State: `checkpatch --strict` clean. Not compiled, not run.** Nothing is
built on tardis1.

### 3.5 What the fixed kernel has to show

- **Sized pairs level**, as on the other hosts: `seq-sized-on` near its
  copy, `mixed-sized-on` near its copy, on both kinds of storage.
- **Loans holding much more of the region.** With pages counted properly a
  record's charge is about 17 against a credit of 25, so loans should run
  up to the budget (475 of 555 on one connection). This is the **first real
  test at 64 KiB of the reserve (30) and the credit factor (150)**, which
  `transplant/README.md` notes was "computed and never measured". On build
  454 the budget refused about seven times too early, so "no drops with a
  budget set" held trivially.
- **Overruns.** Most should disappear: they came from the oldest record
  being charged for fragments past its 25-page credit. If they persist at
  similar rates with correct counting, the factor is too small for how
  veth packs frames at 64 KiB, and that is the next thing to look at.
- **Still no drops and no allocation failures with a budget set**, now
  with the region genuinely in use.
- The new KUnit cases pass (they were added to existing tests, so the
  suite's total stays 41; with section 5's change it should be 41 pass,
  0 skip).

### 3.6 Open on this subject

- The charge dedupes only *consecutive* fragments in a page. If a driver
  hands one connection fragments of a page interleaved with another page's
  (the Xsight card may, veth did not appear to), the count over-charges
  again, safely. Worth a look at `region_count` against pages really held
  on the card, via the `svcsock_tcp_rx_lifetime` tracepoint.
- `mtu1500-on` at 64 KiB shows `wr_direct` 512 and 16 region pages; at
  16 KiB the same cell is also `wr_direct` 512. Not examined further here.

## 4. LOCALIO boundary test: kick rounds for records smaller than a page (test, changed in `573d3938239b`)

### 4.1 What was seen

`localio-boundary-test.sh` exited 1 in both LOCALIO stages, only for the
raw 6000-byte record:

| storage | WRITEs | kick rounds | ceiling (WRITEs/20 + 100) | per WRITE |
|---|---|---|---|---|
| NVMe/TCP | 342475 | 20012 | 17223 | 0.058 |
| nvme-loop | 357912 | 37907 | 17995 | 0.106 |

The test passed on the 4 KiB and 16 KiB hosts. short-read and stable
exited 0, and no file page stayed resident (0.0%) for any record size.

### 4.2 The cause

- The kept-page code (`fs/nfs_common/nfs_dio.c`,
  `nfs_dio_boundary_claim()` / `_complete()`) is written for a page shared
  by **exactly two** writes: the first leaves it unmarked, the second marks
  it for dropping once its own write is complete.
- At 64 KiB about eleven 6000-byte writes share a page (10.9 measured), and
  the eight iorhard ranks write adjacent records (8 x 6000 = 48000 bytes),
  so most of them are in the same page at once. Every write after the
  first counts as "the second" and marks the page; with neighbours writing,
  it is often still dirty with someone else's data, which counts it as
  DONTCACHE dirty and wakes the kick.
- From the saved traces: the share of WRITEs that came within 0.5 ms of
  another task's WRITE to the same page was 41% on NVMe/TCP and 76% on
  nvme-loop, and the rounds follow (0.058 and 0.106 per WRITE).
- The test already declined to judge **reads** for such a record ("more
  than two writers share one") but still judged kick rounds.

### 4.3 The control (2026-10-05, after boot 1, build 454)

6000-byte records only, same rig as the stage, varying the ranks. Output:
`/var/tmp/zc-localio-control-20261005/` (833 MB of traces).

| ranks | NVMe/TCP rounds (per WRITE) | nvme-loop rounds (per WRITE) | reads/WRITE tcp, loop |
|---|---|---|---|
| 1 | 0 (0.000) | 0 (0.000) | 0.457, 0.457 |
| 2 | 72810 (0.460) | 48967 (0.174) | 0.447, 0.516 |
| 8 | 30340 (0.088) | 34934 (0.098) | 0.730, 0.725 |

- **One writer gives no rounds at all.** The rounds come from writers
  sharing a page at the same time, not from the kept-page code on its own.
- **It is not proportional to the number of writers:** two ranks give more
  rounds than eight. Unexplained. A guess, unchecked: with eight the
  flusher is already busy and one round covers many pages.
- The 8-rank figure varies run to run: 20012, 21517, 30340 on NVMe/TCP and
  37907, 36295, 34934 on nvme-loop.

### 4.4 The change

`573d3938239b`: for a record smaller than a page the test reports kick
rounds and does not judge them, as it already does that record's reads.
The line reads "reads and kick rounds not judged: record smaller than a
page, more than two writers share one". Records of a page or more are
judged as before.

- A ceiling scaled by writers per page was **not** used: at 64 KiB it
  would be near 0.55 rounds per WRITE, inside the 0.10 to 0.54 the test
  measured for the regression it exists to catch (pages marked while
  dirty).
- 4 KiB: no change with the default records (both larger than a page).
- 16 KiB: the raw 6000-byte row, which was judged and passed there, is now
  report-only.
- `slipstream/NFSD_LOCALIO_DIRECT_SPLIT.md` section 6 states the exception.
- The commit message says the one-rank control "has not been run". It has
  now (4.3); the message is stale on that point and can be corrected when
  the history is next folded.

Re-run through the plan with the changed test: `localio-tcp` and
`localio-loop` both **boundary exit 0, short-read exit 0, stable exit 0**.

### 4.5 Open: read-back for sub-page records at 64 KiB (behaviour, not a test matter)

Even with one writer, 6000-byte records read back 0.457 pages per WRITE,
and 47008-byte records (also smaller than a 64 KiB page) 0.355 to 0.391.
With many sub-page writes to a page, the page is dropped after the second
write and read back for a later one. On 4 KiB and 16 KiB the comparable
figures are 0.000 to 0.009 for records the kept-page code covers.

This is a cost of misaligned records smaller than a page through LOCALIO
at 64 KiB. It is not a failure of anything the plan checks, and nothing
was changed for it. Whether it matters depends on whether tardis1's real
workload writes such records. `NFSD_LOCALIO_DIRECT_SPLIT.md` section 5
("64 KiB, from the code, not run") is the place to compare against.

## 5. KUnit: `svcsock_rx_capacity_plus_one_test` skips at 64 KiB (test rewritten, not yet built)

- All four suites ran, 0 failures: `sunrpc-xdr-bvec` 7/7,
  `sunrpc-svcsock-rx` 40 pass + 1 skip of 41, `nfsd-receive-bvec` 6/6,
  `nfsd4-receive-bvec` 10/10.
- The skip: `svcsock_rx_capacity_plus_one_test # SKIP needs 33 fragments
  in one skb, MAX_SKB_FRAGS is 17`.
- `run-kunit.sh` fails on any skip (`UNEXPECTED SKIP on 65536-byte
  pages`), so the stage is FAIL, every time it runs on this host.
- `transplant/README.md` anticipated it: "At 64 KiB, if
  `capacity_plus_one` skips, check whether capacity + 1 > MAX_SKB_FRAGS
  before calling it a defect." It is: 33 > 17. The test cannot be built on
  this kernel.
- **Changed after the run (2026-10-05, Mike's choice): the test now runs
  at 64 KiB.** It spreads its capacity + 1 loaned fragments over as many
  skbs as `MAX_SKB_FRAGS` requires (two here: 17 and 16) and feeds them to
  the actor in turn, so the table fills in a later skb with the earlier
  skb's loans already taken. The `kunit_skip()` is gone. Where the table
  fits in one skb (4 KiB, 16 KiB) it builds one skb, as before. The
  expectations are unchanged: copy mode, reason "capacity", every loan
  released, bytes intact.
- **Not compiled, not run.** It needs the new kernel. The thing to watch
  is whether falling back in the second skb really does release and copy
  the first skb's loans as the one-skb case does: the test now asks a
  question at 64 KiB that no test asked before, so a failure here may be
  the code and not the test.
- On the new kernel expect `sunrpc-svcsock-rx` **41 pass, 0 skip** and the
  `kunit` stage PASS. `run-kunit.sh` is unchanged.

## 6. Host setup: what was done to tardis1, and its traps

**Done to the host:**

- `host-bootstrap.sh` installed `fio`, `clang`, `bc`, `dwarves`, `libaio`
  and their dependencies (21 packages, among them `gcc-c++`,
  `libstdc++-devel`, `librados2`, `librbd1`, `librdmacm`, `boost-*`, the
  `fio-engine-*` set), and wrote `/etc/nfs.conf.d/zc-harness.conf`.
- Nothing else persistent. The harness restores the settings it changes.

**Harness fix, `1e38df3a02fe`:** tardis1 has **no `/boot/config-*`** at
all. A packaged kernel's config is `/lib/modules/<release>/config`.
`preflight.sh` said "no config to check"; it now falls back to that file.

**Paths on this host** (not the `/root/kernel/...` the README assumes):

- the tree: `/home/serviceadmin/snitm/kernel/linux`
- kernel-rpmtree: `/home/serviceadmin/snitm/kernel/kernel-rpmtree` (`main`
  at `e3a9e0b`; nothing in the tardis1 path uses it)

**Traps:**

- **It is a live server.** nfsd runs 128 threads exporting `/hs/nvme0` to
  `/hs/nvme4`, and `pl-105.perf.hammer.space` (192.168.0.105) held a
  confirmed NFSv4.2 client with five connections throughout. The plan
  stops and starts nfsd in the KUnit stage and reloads veth. Mike said to
  start anyway on 2026-10-05; **ask again before the next run.**
- **Remote git:** a plain `git fetch origin` fails with
  `Permission denied (publickey)`. Mike says to use
  `ssh -i /root/.ssh/id_ed25519_snitzer -o IdentitiesOnly=yes`
  (as `GIT_SSH_COMMAND`). That failed too from a non-interactive shell:
  the server accepts the key and authentication then fails with no prompt,
  which looks like a passphrase. So nothing was fetched; Mike confirmed
  both trees were current. Never push from here.
- **Start the plan from somewhere other than `transplant/`.** fio writes
  `local-*-verify.state` files into the current directory. Six are sitting
  untracked in `transplant/` from this run, with an untracked `statx-dio`
  binary in `NFSD_TCP_WRITE_ZEROCOPY/`. They were left alone (never remove
  untracked files); ask Mike before clearing them.
- **`run-testplan.sh --redo STAGE` re-runs the stage itself** and then
  carries on with the plan (it also re-ran `preflight` and `kunit`,
  because `kunit` is FAIL and so not done). Do not follow it with
  `--only`: that runs the stage a second time. `status.tsv` has duplicate
  rows for `preflight`, `kunit`, `localio-tcp` and `localio-loop` for this
  reason; the last row for each is the current one.
- **`region_budget_stats` reads `budget 10` after a KUnit run** on a boot
  that already has the region (seen after the re-runs: `budget 10
  reserve_size 0 ... orphaned 40`, up from `orphaned 20`). The region is
  bound to the test veth devices, so it does not touch the real exports,
  and the host is to be rebooted into the new kernel anyway. Not chased.
- **Installed kernels:** 64 KiB RPM sets for `7.1.13-14.hs.441`,
  `7.1.13-19.hs.452` and `7.1.13-19.hs.454` (each with
  `-modules-internal`, `-devel` and `kmod-64k-xsight`) are installed. A new
  build number will tell the new kernel apart; `preflight.sh` cannot tell
  whether a running kernel has `c84e1147bde7` (section 9).

## 7. Commits made on tardis1

On `kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY`, on top of `42c4c8306b98`.
Mike pushed all of these on 2026-10-05, through `d063737e2ed3`, and is
building the new kernel from that commit: it has the charge fix and the
rewritten capacity + 1 test.
The KUnit commit was moved ahead of this file's commits to keep the
grouping; the branch before that move is
`kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY.pre-regroup-20261005` (same
tree).
`check-grouping.sh`: `grouping OK: code, then KUnit, then docs`.

| kind | commit | subject |
|---|---|---|
| code | `c84e1147bde7` | SUNRPC: Charge a TCP receive loan for the region pages it holds, not its fragments |
| KUnit | `19a1bb8479e4` | SUNRPC: test that region credit counts a page once in the svcsock KUnit suite |
| docs | `1e38df3a02fe` | NFSD_TCP_WRITE_ZEROCOPY: record the page charge found at 64 KiB, and let preflight find a packaged kernel's config |
| docs | `573d3938239b` | NFSD_TCP_WRITE_ZEROCOPY: do not judge DONTCACHE kick rounds for a record smaller than a page |
| KUnit | `86b745caa01c` | SUNRPC: run the capacity + 1 svcsock KUnit test where the table exceeds one skb |
| docs | (this file, two commits) | |

The code commit carries `Fixes: 49f73a653f81 ("SUNRPC: Credit only the
region pages a TCP receive loan holds")`. All end with
`Assisted-by: Claude:claude-opus-5-5` and Mike's `Signed-off-by`.

Per `AGENTS.md`, a change on this branch must be carried to the HS twin.
That has not been done for these.

## 8. Other observations, recorded so they are not rediscovered

### 8.1 SGL rig dispositions at 64 KiB

| rig | device | WRITE segments |
|---|---|---|
| `sgl-brd` | `/dev/ram0`, no boundary, memory alignment 512 | 8 direct, 152 `mem_misaligned` |
| `sgl-tcp` | NVMe/TCP, no boundary, memory alignment 4 | 160 direct |
| `sgl-loop` | nvme-loop, boundary 4096 | 160 direct |

Each: `dd_fails=0 cmp_mismatches=0`, loans `borrowed=167800320
copied=54760`. Not yet set beside the other hosts' dispositions.

### 8.2 Things confirmed at 64 KiB

- **`rx_ip_align=2` gives aligned loans:** on NVMe/TCP every loan cell is
  written direct (`wr_direct` = WRITE count).
- **On nvme-loop a loan is written dontcache and a copy direct,** as on
  the other hosts.
- **Budget geometry as the README predicted:** 1 MiB = 16 pages, factor
  150, record cost 25, reserve 30, region default 555 pages (35 MiB).
- **The phase N sum check works:** `init 270 + B 338 over 555 refused
  (... No space left on device), B stays 0`.
- **`bvecrepro` builds and loads against `kernel-64k-devel`:** the
  packaged kernel does not refuse the unsigned test module.

### 8.3 Not looked at

- Throughput of the growing-file copies (about 440 MB/s where the sized
  ones reach 2100 to 2900) and why; it is what makes a throttled loan look
  good in the growing pairs.
- Any comparison of the phase D, TLP, X and N tables with the other page
  sizes. Pointless until the loans are not throttled.

## 9. What to do next

**When Mike has built the kernel from `d063737e2ed3`** and installed it with
`kernel-64k-modules-internal`, `kernel-64k-devel` and `kmod-64k-xsight`:

1. Ask Mike whether the window is quiet (`pl-105` is a live client), and
   which commit the build is from. Confirm with `uname -r` and
   `cat /sys/module/sunrpc/srcversion`: it must **differ** from
   `4882A60124EF5C26B2AF247`. `preflight.sh` cannot tell a kernel with the
   charge fix from one without.
2. Move the old results aside or clear them only on Mike's word (the rule
   is "keep only the last full run", and these are in
   `/var/tmp/zc-testplan-7.1.13-19.hs.454...`, `/var/tmp/zc-run-logs`,
   `/var/tmp/zc-localio-control-20261005`). A new release name gives the
   new run its own results directory in any case.
3. From a directory other than `transplant/`: `preflight.sh`, then
   `run-testplan.sh`; reboot when it asks; `run-testplan.sh` again.
4. Read every cell, then check section 3.5's list first: sized pairs
   level, region use up near the budget, overruns, no drops with a budget.
5. Expect `kunit` PASS with no skip (section 5: the rewritten
   `capacity_plus_one` test runs for the first time), and both LOCALIO
   stages with boundary exit 0.
6. Only then write the 64 KiB record: `results-table.py extract ... >
   results/64KiB.tsv`, regenerate `RESULTS.md`, add the "64 KiB host"
   account to `transplant/README.md`, update its "Where things stand"
   table, and fold what is still true from this file into those. This
   file can then go, or shrink to what it still explains.
7. `check-grouping.sh` before handing back. Do not push.

**If the fixed kernel shows something else,** the build-454 results are the
"before" to compare with, cell for cell; that is the reason they are kept.

**Decisions waiting on Mike:**

- whether the sub-page read-back through LOCALIO matters (section 4.5);
- carrying the four commits to the HS twin (section 7);
- clearing the untracked files and the old results (sections 6 and 9).

## 10. Build 455, boot 1 (2026-10-06): the charge fix measured

Kernel `7.1.13-19.hs.455.loanpages_pool.el9.aarch64+64k`, packaged RPMs
(with `-modules-internal` and `-devel`), built by Mike and said to be from
`d063737e2ed3`; sunrpc `9611113753DDA35599A6AE2`, nfsd
`4E439F2B24A4A7C3A8521DE`. `preflight.sh`: PASS on a fresh boot. Results:
`/var/tmp/zc-testplan-7.1.13-19.hs.455.loanpages_pool.el9.aarch64+64k/`.
Run log: `/var/tmp/zc-run-logs/boot1-455.log`. The plan was started from
`/var/tmp/zc-run-logs/cwd-455`, where fio left its six state files.
`pl-105` was connected (seven connections) when the run started; Mike asked
for the run.

Nothing here is in `README.md`, `RESULTS.md` or `results/64KiB.tsv` yet:
boot 2 has not run.

### 10.1 Stages

Every stage PASS or DONE, 52 minutes. No fio verify failure, no cell
killed by its timeout.

- **KUnit: PASS, no skip.** 7/7, **41/41**, 6/6, 10/10.
  `svcsock_rx_capacity_plus_one_test` ran for the first time at 64 KiB and
  passed (section 5's question: the second skb's fallback does release and
  copy the first skb's loans).
- **bvecrepro** MATCH; **rig-cmp** PASS; **loan-assert** 50 of 50
  connections fully direct; **killswitch** validated; **system-correctness**
  fails=0, direct=500, vector=0.
- **SGL rigs,** `dd_fails=0 cmp_mismatches=0` each: brd 10 direct and 150
  `mem_misaligned`; NVMe/TCP 160 direct; nvme-loop 152 direct and 8
  `mem_misaligned`. (Build 454: 8/152, 160, 160/0.)
- **LOCALIO, both rigs:** boundary exit 0, short-read exit 0, stable exit
  0. Reads per WRITE 0.417 and 0.299 at 47008, 0.007 and 0.008 at 96000,
  0.738 and 0.707 at 6000 (tcp, loop); 0.0% resident throughout.

### 10.2 Section 3.5's list

| pair (phase A) | conn | nvme-loop: copy / loan MB/s | NVMe/TCP: copy / loan MB/s | peak region pages, loan (loop, tcp) | paused (loop, tcp) |
|---|---|---|---|---|---|
| `seq-sized` | 1 | 2130 / 2130 | 2191 / 2165 | 180, 188 | 0, 0 |
| `step` | 1 | 1320 / 1319 | 1320 / 1317 | 318, 317 | 0, 0 |
| `mixed-sized` | 16 | 2684 / 1104 | 2886 / 1106 | 272, 264 | 1057, 958 |
| `step-n16` | 16 | 1902 / 1259 | 1841 / 1007 | 279, 280 | 98355, 96370 |
| `seq` (new file) | 1 | 439 / 2114 | 401 / 394 | 180, 316 | 0, 0 |
| `mixed` (new files) | 16 | 440 / 1160 | 446 / 805 | 276, 320 | 1044, 992 |

- **Overruns are gone:** 0 in every cell of every phase on both kinds of
  storage (build 454: 1537572 by the end of boot 1). The init counter
  reads `overrun 4` at the end; no cell's table shows where those came
  from.
- **One connection: copy and loan are level, and nothing pauses.** On
  build 454 `seq-sized-on` ran at 1000 and 983 MB/s with 758 and 779
  pauses.
- **Sixteen connections: the loan is still well behind its copy, and
  still pauses.** `mixed-sized-on` is at 41% and 38% of its copy (build
  454: 10% and 12%), `step-on-n16` at 66% and 55%. About 1000 pauses per
  2785 WRITEs in every 16-connection `mixed` loan cell, and about 96000 to
  137000 in the `step` ones. So the "sized pairs level" expectation holds
  for one connection only.
- **The region is not used up to the budget.** Peak use is 180 to 318
  pages on one connection (budget 471 to 479) and 256 to 360 on sixteen
  (budget 415 to 423), up from 80 and 124 to 148 on build 454.
- **No drops and no allocation failures in any cell with a budget set**
  and the receive buffer held. The cells meant to drop did: `floor-refused`
  596 and 1902, `x-off-RN` 4087 and 4131. `n-exit` fio exit 4, as intended.
  `region-final.txt`'s `alloc_fail` (4683, 10716) is exactly those cells'
  sum.
- **How WRITEs were issued:** on nvme-loop every loan is dontcache and
  every copy direct; on NVMe/TCP every loan is direct (`wr_direct` equals
  the admitted count in every loan cell). `mtu1500-on` is 512 direct on
  both, 16 region pages.

A reading of the 16-connection gap, not checked: the peak of about 272
pages is sixteen records of 17 pages, and a budget of 423 less the reserve
of 30 admits fifteen records at a cost of 25. So the budget is doing what
it is set to do, one record or so per connection, and the copy beside it
has no such limit on this 64-CPU host. Whether that is the factor (150,
25 charged for 17 held), the reserve, or simply the price of a 555-page
region with sixteen connections is the next thing to look at. Phase D and
X show the same shape (`d-on-N` 1151 and 854, `x-on-R` 1075 and 838, each
with about 1000 pauses).

### 10.3 New in the kernel log

Once, at 11:17:25, as `localio-loop`'s boundary test began its first
record size (47008):

```
Page cache invalidation failure on direct I/O.  Possible data corruption due to collision with buffered I/O!
File: /export/t4/liob.ior PID: 452 Comm: kworker/54:1
```

**Examined 2026-10-06, from the saved trace** (`localio-loop/.../boundary/rs47008.trace`);
the rig is gone, so nothing was re-run. It is a hole in the split rule, and
it **failed a write**:

- The message prints up to ten times a day, so one line is one event.
- **What collided.** Record 6655 (rank 7) ends on a 4096-byte multiple,
  16384 bytes into page 4774. That end "needs no buffered segment", so its
  middle was written direct into the first 16384 bytes of the page. Record
  6656 (rank 0) starts there and ends misaligned in the same page; it
  holds no whole page, so it has no middle and was written buffered into
  the rest of that page, 0.19 ms later. The direct write completed with
  the page dirty, could not invalidate it, logged the message and set
  `-EIO` in the mapping's `wb_err`.
- **What it cost.** Record 6656's write is synced by
  `nfs_local_write_sync()` (`vfs_fsync_range()`), which reported that
  `-EIO`. Rank 0's `pwrite()` failed and it stopped after 833 records, at
  exactly that record; the other seven wrote 5710 each. That is why this
  run wrote 40802 records (1829 MiB) where NVMe/TCP wrote 45680 (2047
  MiB).
- **The test did not notice:** it sends `iorhard`'s stderr to `/dev/null`,
  `iorhard` exits 0 after a failed `pwrite()`, and nothing compares the
  record count. "boundary exit 0" was wrong for this run.
- **The rule that allows it** is in `nfs_dio_split()`: "an end that is
  already an `offset_align` multiple needs no buffered segment, and the
  middle starts or stops right there, mid-page or not". That is safe only
  if the neighbouring write is direct in that page too. The neighbour is
  buffered there whenever it has no direct middle: it holds no whole page
  (a record smaller than a page), its middle is under
  `direct_misaligned_min_bytes`, or its memory is misaligned.
- **How often.** In this trace 102 pages were written by one record's
  direct middle and another record's buffered segment, 31 of those pairs
  within 2 ms of each other; one lost the race. NVMe/TCP's trace has 178
  such pages and 51 pairs within 2 ms, and none lost it. The 96000-byte
  record has none on either rig (both neighbours are direct).
- **Not only 64 KiB, not only LOCALIO.** The same split serves
  `nfsd_vfs_write()`, which checks `wb_err` after its write. The geometry
  needs a page larger than `offset_align` and a neighbour without a
  middle; 64 KiB pages with records under a page are simply where the
  test's sizes reach it. Whether the 4 KiB and 16 KiB hosts' record sizes
  can reach it was worked out by hand only for 16 KiB (47008 and 6000: no).
- **Data:** not checked, the files are gone. From the code, the direct
  bytes are on disk and XFS writes back only the blocks the buffered write
  dirtied, so the exposure looks like the `-EIO` and a page cache copy
  that may be stale until it is dropped; that is a reading, not a test.
- **Changed 2026-10-06, Mike's decision:** the middle of a write is now
  whole pages only; every end that is not on a page boundary is cut at
  one, aligned or not (`nfs_dio_split()`, with its KUnit cases and
  `nfsd-io-modes.rst`). The cost: a write that is block-aligned but not
  page-aligned is no longer direct throughout, and one that holds no
  whole page (anything under 64 KiB here) is buffered.
  `slipstream/NFSD_LOCALIO_DIRECT_SPLIT.md` section 7 has the account.
- **The test is fixed:** `iorhard` names a failed write and exits 1, and
  the boundary test fails the record size on that or on the kernel
  message.
- **How far it is verified:** compiled against build 455's `-devel`
  files, and the split KUnit test passes here at 64 KiB from a test-only
  module with a private copy of the new split. **Not built into a kernel,
  not booted, the boundary test not re-run.** That needs build 456.
- **Done to the host for that check:** `kunit` was reloaded with
  `enable=1`, the test module loaded and removed, then `kunit` (disabled)
  and `nfsd4_bvec_kunit` loaded again as `/etc/modules-load.d/nfs.conf`
  has them. nfsd was not stopped. The kernel is now tainted N (a KUnit
  test ran) until the next boot. **`dmesg -c` was run by mistake at
  13:14 and cleared the ring buffer;** the journal (`journalctl -k -b`)
  still has this boot's messages.

Otherwise the log has only the two messages section 2 names, and no
WARNING, BUG, stall or allocation failure.

### 10.4 Other things to set beside the other hosts

- `early-release-m1-d10`: 256 admitted, `wr_direct`, `wr_dontcache` and
  `wr_buffered` all 0, on both kinds of storage. Not compared.
- `step-on-n16` draws 6633 to 7770 retransmits and as many loss probes,
  none with quick acknowledgements or probes off: the spurious tail-loss
  probes of the TLP study.
- `seq-on` on NVMe/TCP held 316 pages where `seq-sized-on` held 188.

## 11. Build 455, boot 2 (2026-10-06): phase B and the analysis

Fresh boot of the same kernel (sunrpc `9611113753DDA35599A6AE2`), 12:38 to
12:43, five minutes. Run log: `/var/tmp/zc-run-logs/boot2-455.log`, started
from `/var/tmp/zc-run-logs/cwd-455`. One nfsd client was connected when it
started; Mike asked for the run.

- **Stages:** `preflight` PASS (fresh boot), `budget-B` DONE, `budget-B-sgl`
  DONE, `analyze-AB` PASS, `analyze-AB-sgl` PASS, `summary` DONE. The plan
  is complete for build 455.
- **`analyze.py`: OVERALL PASS** for nvme-loop and for NVMe/TCP, every
  judged cell of phases A and B PASS.
- **Phase B** (region at its configured minimum, 114 pages; budget 30,
  equal to the reserve; `edge-below-n1` budget 46):

| cell | conn | nvme-loop MB/s | NVMe/TCP MB/s | peak region pages (loop, tcp) | denied (loop, tcp) |
|---|---|---|---|---|---|
| `edge-at-n1` | 1 | 370 | 336 | 24, 28 | 319, 341 |
| `edge-at-n4` | 4 | 349 | 325 | 36, 36 | 516, 522 |
| `edge-at-mixed-n4` | 4 | 146 | 140 | 36, 36 | 5597, 5592 |
| `edge-at-step` | 1 | 896 | 759 | 28, 28 | 103113, 89059 |
| `edge-below-n1` | 1 | 362 | 337 | 28, 24 | 270, 274 |
| `edge-off-n1` (no receive buffer held) | 1 | 441 | 403 | 8, 8 | 0, 0 |
| `edge-copy-n1` | 1 | 434 | 398 | 4, 4 | 0, 0 |

- **Every cell:** fio exit 0, no drops, no allocation failures, 0 paused,
  0 overrun, nothing collapsed. Loans are written dontcache on nvme-loop
  and direct on NVMe/TCP, every one (`admitted` equals the count); the two
  copy cells are direct.
- `edge-at-step` retransmits 925 and 1071 times, with as many loss probes
  and duplicate acknowledgements and no timeout. The 16 KiB and 4 KiB
  records show the same in this cell (617 to 1193).
- Peak region use is 1.5 to 2.25 MiB, where 16 KiB shows 2.25 to 2.5 MiB
  and 4 KiB 1.6 to 1.9 MiB.
- **`edge-off-n1` did not drop here** (nor at 16 KiB); at 4 KiB it drops
  482 and 1389 frames. It held 8 pages, 0.5 MiB, against 3.25 to 4 MiB on
  the other hosts. Not examined.
- **Kernel log, boot 2:** only the `DISCARD` "operation not supported"
  lines on the nvme-loop test device, once per stage. No WARNING, BUG,
  stall or allocation failure, and no repeat of 10.3's invalidation
  message (no LOCALIO stage runs on boot 2).

Not done: `results/64KiB.tsv`, `RESULTS.md` and the README's 64 KiB
account (section 9 step 6). Sections 10.2 (the 16-connection gap) and 10.3
(the invalidation message) are still open, and 10.3 says to understand
that message first; it now is (10.3), and it is a defect.
