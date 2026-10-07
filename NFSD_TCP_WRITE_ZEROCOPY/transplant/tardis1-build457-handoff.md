# tardis1 (64 KiB), build 457: handoff after the waiter-list hang

Written 2026-10-06, about 21:15, at the end of the session that ran build
457's boot 1. tardis1 has no Claude memory between sessions: **this file is
where a new session starts.** Read `AGENTS.md`, then
`tardis1-boot1-testing.md` (builds 454 and 455), then this.

That session committed two things, neither pushed: the fix for the hang
(`3ec476031fd9`, section 4) and this file. The fix was moved to the end
of the code section, so **every commit after it has a new SHA**. The
backup branch made for that move was deleted the same day, at Mike's
request.

**Updated 2026-10-06, about 22:00, by the session after it**, which
added four commits, none pushed (SHAs as of that session; a regroup moves
all but the first two):

| commit | what | where |
|---|---|---|
| `3ec476031fd9` | the hang fix (section 4) | end of the code section |
| `1fcefc552397` | its KUnit case, `svcsock_rx_credit_collapsed_unwaits_test` | end of the KUnit tests, before the configs commit |
| `7abb45f322f7` | the materialize fix (section 5): merge pages marked in a third bitmap | **at the tip, after the docs** |
| `377d00eb185b` | its KUnit case, `svcsock_rx_merge_then_materialize_test` | **at the tip, after the docs** |

The last two were left at the tip at Mike's request, so
**`check-grouping.sh v7.1.13-20` reports `grouping BROKEN` until they are
regrouped** (the fix to the end of the code section, the test to the end
of the KUnit tests); regroup before handing back. **Five more commits
followed at the tip the same night (section 9: the write split is cut
per end again) and two more after midnight (section 10: writes beyond
EOF are not issued direct), and they need the same regroup.** `regroup-backup-kunit`
is the backup branch from the one regroup that session did; delete it
when Mike is satisfied. Nothing has been carried to the HS twin.

**Pushed by Mike on 2026-10-07, to two work-in-progress branches, not to
the branch itself.** Where this file says "not pushed" it means
`origin/kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY`, which is still at
`e137fc262e46`, build 457's tip from before the regroups (so the local
branch reads as far ahead of and behind it; that is rewritten history,
nothing is lost):

| remote branch | at | holds |
|---|---|---|
| `origin/kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY-WIP` | `a1c37cac4922` | everything through section 9 |
| `origin/kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY-WIP2` | `767f20751a4d` | that plus section 10 (the XFS work and the beyond-EOF rule) |

Neither is regrouped (`check-grouping.sh v7.1.13-20` reports `grouping
BROKEN` for both), and nothing is on the HS twin. A regroup rewrites
the SHAs this file names from `7abb45f322f7` on.

## 0. Armed: the first thing to do on the next boot

**Before anything else on the next boot, run the two KUnit cases that
were added for the two defects, and record the result here.** Neither
had run on a booted kernel when this was written (section 8 has what was
run on build 457 with rebuilt modules).

1. `uname -r` and `cat /sys/module/sunrpc/srcversion`. Build 457's
   shipped sunrpc is `9611113753DDA35599A6AE2` and has neither fix.
2. Ask Mike before the run (the host is a live server; the KUnit stage
   stops `nfs-server` for about a minute and starts it again).
3. If the booted kernel was built from a tree with all four commits
   (and section 9's five; `run-kunit.sh` runs their suite too, and
   section 9.6 has the runtime check that belongs to them):

   ```
   cd /var/tmp/zc-run-logs && systemctl stop nfs-server && \
     bash ~/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/run-kunit.sh /var/tmp/zc-run-logs/kunit-armed-$(uname -r).log; \
     systemctl start nfs-server
   grep -E 'svcsock_rx_(credit_collapsed_unwaits|merge_then_materialize)_test|# sunrpc-svcsock-rx: pass' \
     /var/tmp/zc-run-logs/kunit-armed-$(uname -r).log
   ```

   Wanted: `ok` for both cases, `== KUNIT PASS`, and no
   `svc_tcp_rx_materialize` warning in `dmesg`. If either case is
   **missing** from the log the kernel was not built from this tree's
   tip: say so, do not report a pass.
4. If the booted kernel is build 457 again (or any kernel without the
   fixes), the shipped modules have neither the fixes nor the cases:
   follow section 8 to load the rebuilt modules, then step 3 with the
   rebuilt KUnit modules as section 8 says.
5. Then the runtime checks that KUnit cannot give: the loop of section
   3.3 under `credit.bt` until a `COLLAPSE ... waiting=1` line is
   followed by the cell finishing, and `budget-A-sgl`'s `collapse-off`
   cell with no warning.

## 1. Where things stand

| | |
|---|---|
| kernel | `7.1.13-20.hs.457.loanpages_pool.el9.aarch64+64k`, packaged RPMs, booted 2026-10-06 19:23. sunrpc srcversion `9611113753DDA35599A6AE2` (same as build 455), nfsd `A1443B45D205F18D790B96C` (new). `direct_misaligned_min_pages` exists and reads 1, so it is from the branch as rebased onto `v7.1.13-20` |
| branch | `kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY`. Build 457 was run with the tip at `e137fc262e46`; that commit is now `f99a4fbe8c90` (same content, after the regroup), with this file's commit on top |
| plan | boot 1 **incomplete and stopped by hand**. Boot 2 not run |
| found | a hang in region-credit admission, **root cause confirmed** (section 3), fixed (section 4); a `svc_tcp_rx_materialize` warning, cause read from the code (section 5), fixed. Both fixes have a KUnit case; see sections 0 and 8 for what has run |
| installed this session | `drgn` 0.0.33 and `kernel-64k-debuginfo` / `kernel-debuginfo-common` for build 457 (Mike installed the debuginfo) |
| not a fresh boot | the receive region exists; `budget-D` and `budget-B` need a reboot |

## 2. Build 457, boot 1: what ran

Results: `/var/tmp/zc-testplan-7.1.13-20.hs.457.loanpages_pool.el9.aarch64+64k/`.
Run log: `/var/tmp/zc-run-logs/boot1-457.log`, started from
`/var/tmp/zc-run-logs/cwd-457`. `preflight.sh`: PASS on a fresh boot
(`/var/tmp/zc-run-logs/preflight-457.log`). Two nfsd clients were connected
when the plan started; Mike asked for the run.

| stage | result |
|---|---|
| `preflight`, `kunit`, `bvecrepro`, `rig-cmp`, `loan-assert`, `killswitch`, `system-correctness` | PASS |
| `budget-D`, `budget-D-sgl`, `zc-down`, `sgl-up`, `sgl-brd`, `sgl-tcp`, `sgl-loop`, `sgl-down`, `localio-tcp`, `localio-loop`, `budget-A`, `budget-TLP` | DONE |
| `budget-X` | **FAIL**: `x-tight-RN` hung, `R_rc 124` (section 3) |
| `budget-N`, `budget-A-sgl` | DONE (ran after the hung cell was killed by its timeout) |
| `budget-TLP-sgl` | **killed by hand part-way**; its directory is partial |
| `budget-X-sgl`, `budget-N-sgl`, all of boot 2 | not run |

**Only the stage results were read.** Not read: the KUnit counts, the
LOCALIO boundary output (the first run with the page cut and the fixed
test; section 10.3 of `tardis1-boot1-testing.md` says what to look for),
the SGL dispositions, and the phase D, A, TLP, N and A-sgl cells.

Seen in passing, not checked, to set beside build 455:

- **Growing-file copies are four to five times faster.** Phase X,
  nvme-loop: `x-off-N` 2085 MB/s (455: 436), `x-off-R` 1842 (441),
  `x-on-RN` 797 / 2152 (390 / 415). Loans did not move: `x-on-R` 1050
  (1075), `x-tight-R` 146 (146). `x-off-RN` drops 544 (4087).
- **The 16-connection `mixed` copies are no longer all direct:**
  `mixed-copy` and `mixed-sized-copy` show `wr_direct` 1407 and
  `wr_dontcache` 3938; on build 455 every copy was direct. Probably the
  page cut; not confirmed. **Confirmed the same night, and it is the
  same thing as the bullet above: section 9.7.**
- A comparison script for the three builds' cells is at
  `/tmp/claude-0/-home-serviceadmin-snitm-kernel-linux/7faf2fbc-bbfe-45e9-93b4-8229c848f82f/scratchpad/cmp.py`
  (`cmp.py A X` etc.); it may not survive, and is 40 lines to rewrite.

## 3. The hang: a waiting transport that reads on without admission (confirmed)

### 3.1 What happens

In `net/sunrpc/svcsock.c`:

1. A transport is denied in `svc_tcp_rx_credit_admit()` at a record's first
   skb that carries region pages, and is put on the budget's waiter list
   (`sk_credit_wait`, ordered by ticket).
2. While it waits, TCP collapses its receive queue (`tcp_collapse()`, when
   `sk_rmem_alloc` exceeds `sk_rcvbuf`). The queued data is now in ordinary
   pages.
3. It is woken as the oldest waiter. In `svc_tcp_recv_actor()` the credit
   block computes `need == 0` for that skb, so neither
   `svc_tcp_rx_credit_admit()` nor `svc_tcp_rx_credit_charge()` is called,
   and the record is read and completed.
4. **Nothing takes it off the waiter list.** Only its own admission, a
   successful charge, `svc_tcp_rx_credit_forget()` (teardown, or budget 0)
   do that.
5. It drains its queue and stops. `svc_data_ready()` returns early for a
   transport on the waiter list (`svc_tcp_rx_credit_waiting()`), so new
   data does not enqueue it.
6. Every other transport is denied (`head && head != svsk`), a denial does
   not wake the head, and with nothing in flight no release ever comes.

Not specific to 64 KiB, to this build or to the tight budget: build 455
has the same sunrpc code. The tight cells make it likely because sockets
wait with a full receive buffer.

### 3.2 Evidence

- **Original** (plan run, `budget-X` on nvme-loop, cell `x-tight-RN`,
  from about 19:57): `budget 30 reserve_size 30 avail 0 reserve 30
  inflight 0 waiters 14`, `admitted` static, all 128 nfsd threads idle,
  76 of 555 region pages held. Server `TCPRcvCollapsed` was 0 at 19:56:11
  and 2 during the hang. It crawled forward (64 more admissions) and was
  killed at its timeout at 20:17. What let it crawl is **not explained**.
  Saved: `/var/tmp/zc-run-logs/hang-457-x-tight-RN/`.
- **Reproduced** at 21:04:38, repetition 34 of `x-tight-RN` alone, under
  bpftrace, then inspected with drgn. In
  `/var/tmp/zc-run-logs/hang-457-repro/`:
  - `trace2-at-hang.out`: the last 40 lines for
    `sv=0xffff00832d720000` show DENY (ticket 1528632, reclen 4232), then
    `COLLAPSE ... waiting=1 rmem=131840 rcvbuf=131072`, then WAKE, then
    `RECVRET ret=4232 ... waiting=1` with no ADMIT, then silence.
  - `drgn-hang2.txt`: the pool (`inflight=0`, `reserve=30`) and its five
    waiters. The head is that transport, between records (`tcplen=0`)
    with a 528-byte request unread; the other four are stopped at
    `tcplen=32 datalen=28`.
- **Rate:** one collapse in 34 repetitions of the cell; none in 14 full
  phase X runs before that. Every collapse seen on a waiting transport
  hung.

### 3.3 Tools left for the next session

All in `/var/tmp/zc-run-logs/hang-457-repro/`:

| file | use |
|---|---|
| `credit.bt` | `bpftrace credit.bt > trace.out`: every admit, deny (with the head waiter's state), pause, release, wake, forget, what a woken transport's `svc_tcp_recvfrom()` returned, and `tcp_collapse()` on port 2049 with whether the transport was waiting |
| `waiters.py` | `drgn waiters.py`: each budget, its records in flight, each waiter and the skbs in its receive queue |
| `run-budget-cells.sh` | a copy of `run-budget.sh` in which phase X honours `CELLS=` and repeats `x-tight-RN` `REPS` times. To reproduce: `CELLS=x-tight-RN REPS=60 STORAGE=prp PHASE=X TMO=6000 bash ./run-budget-cells.sh <outdir>`. About 9 seconds a repetition; a hang shows as `inflight 0` with waiters in `/proc/sys/net/sunrpc/region_budget_stats` |

Stop a hung run with `pkill -KILL -x fio`, then SIGTERM (not SIGKILL) to
the script so its cleanup runs.

## 4. Task 1: fix the hang (committed as `3ec476031fd9`, compiled, not run)

**Committed, not pushed:** `3ec476031fd9` "SUNRPC: Take a transport that
reads on without admission off the credit wait list", nine lines in
`net/sunrpc/svcsock.c`, the last commit of the code section.
`check-grouping.sh v7.1.13-20`: `grouping OK`. The final tree compiles
with `W=1` and no warnings against build 457's config less module
versions, BTF and signing (`make O=/var/tmp/zc-build-check
net/sunrpc/svcsock.o`; that directory can be reused or deleted). Not
built into a kernel, not run.

**Its `Fixes:` tag is `7c0a0bbe7ab2`** ("SUNRPC: Credit only the region
pages a TCP receive loan holds"), from reading the diffs:

- `7ab514bb557c` (region credit admission) asked for admission at the
  start of every record, so a woken transport always asked again and
  could not be left on the list this way.
- `7c0a0bbe7ab2` moved admission to a record's first skb that carries a
  region page. From there a waiter whose skb no longer carries one
  passes without asking and stays listed; as the oldest waiter it blocks
  the rest until its own next region record, if one comes.
- `e134479afcd2` (do not re-queue a waiting transport) made it worse:
  the listed transport itself no longer runs on new data. It did not
  introduce the defect. (The commit first cited this one, picked from
  its subject without reading it; corrected before the regroup.)

What the change does, for Mike to agree or redirect:

- In `svc_tcp_recv_actor()`, after the credit block inside `if (take)`:
  if the record was not paused and the transport is still on the waiter
  list, it has read past where it was denied without admission, so call
  `svc_tcp_rx_credit_forget(svsk)`. That unlinks it and wakes the next
  waiter, which restarts the chain. `svc_tcp_rx_credit_waiting()` is the
  lockless test; the actor is the only thread that links or unlinks its
  own transport.
- This also covers a record already admitted that paused in
  `svc_tcp_rx_credit_charge()` and whose next skb was collapsed: it stays
  in flight and its release wakes the head as usual.
- To decide: whether a transport that reads on this way should keep its
  ticket (it should not need one), and whether `svc_data_ready()`'s early
  return wants a second look given that a listed transport can have work
  that is not a region record.

Then:

- **KUnit: written, `1fcefc552397`**
  (`svcsock_rx_credit_collapsed_unwaits_test` in `sunrpc-svcsock-rx`).
  Through the receive actor: budget 10 all held by one record; the
  fixture transport is refused at a region page-pool fragment after its
  28-byte prefix; a second transport queues behind it; the record in
  flight ends with wakes suppressed, leaving two waiters and nothing in
  flight; the rest of the record then comes from a plain-page skb.
  Expects the record complete with no credit drawn, the transport off
  the list (ticket and `sk_budget` cleared), the second transport woken
  and then admitted.
- **Compile check.** Nothing is built on tardis1; build 455's notes
  compiled changed files against the `-devel` config less module
  versions, BTF and signing.
- **Verify on a kernel Mike builds:** the loop in 3.3 with `credit.bt`
  attached, until a `COLLAPSE ... waiting=1` line is followed by the cell
  finishing.
- **Docs:** `slipstream/BUDGET_SCOPE.md` for the mechanism,
  `check-grouping.sh v7.1.13-20` before handing back, and the HS twin
  (`AGENTS.md`: a change on one branch must be carried to the other).

**Related, to look at with the fix:** the receive-buffer hold
(`svc_tcp_rx_hold_buffer()`, `svc_tcp_rx_window_clamp()`) exists so a
waiting transport's queue stays inside its buffer and is not collapsed.
Here it was 768 bytes over (131840 against 131072). A collapse also turns
a loan into a copy. Whether the clamp's margin is short at 64 KiB, or the
collapse is to be expected and only the list handling is wrong, is open.

## 5. Task 2: the `svc_tcp_rx_materialize` warning (fixed as `7abb45f322f7`, not reproduced at runtime)

**Fixed, Mike's choice of the two below: a third bitmap.**
`7abb45f322f7` "SUNRPC: Count a merge page's bytes once when a TCP
receive record is materialized", `Fixes: 1ecfe1f42fc6`. `struct
svc_tcp_rx_state` gains `merged` beside `borrowed` and `page_pool`
(**the struct's layout changes**, so sunrpc, nfsd and the KUnit modules
must be built together). The bit is set when
`svc_tcp_rx_copy_segment_merged()` enters its page; materialize moves no
bytes for a marked entry (`borrowed_bytes`, `copied_bytes` and
`materialized_bytes` are left alone) but still drops the reference;
`svc_tcp_rx_retract_last()` gives a marked entry's bytes back to
`copied_bytes` (by reading, it cannot meet one today); the mark is
cleared wherever the borrowed one is, and checked empty with it.

**KUnit: `377d00eb185b`** (`svcsock_rx_merge_then_materialize_test`): a
locked-head skb with the prefix and 40 body bytes (merged), then an skb
with the last 64 bytes on a region page and no budget anywhere, so the
actor's "No budget to draw on" branch materializes the record with the
merge page in its table. No collapse needed. Expects no error, the
record complete as a copy with reason `NO_BUDGET`, nothing borrowed,
`copied_bytes` 104, references balanced and the body intact.

What follows is the examination that led there.

Once, 2026-10-06 20:29:42, in the last seconds of `budget-A-sgl`
(NVMe/TCP; the stage is recorded DONE at 20:29:49):

```
WARNING: net/sunrpc/svcsock.c:1444 at svc_tcp_rx_materialize+0x104/0x2d8 [sunrpc], CPU#25: nfsd/8573
 svc_tcp_rx_materialize
 svc_tcp_recv_actor+0x594/0x720
 __tcp_read_sock
 tcp_read_sock
 svc_tcp_recvfrom+0xb0/0x728
```

- Line 1444 is the `WARN_ON_ONCE` at the end of materialize: after
  copying every borrowed fragment, `body_offset != state->body_bytes`, or
  a borrowed or page-pool count is left over, or references acquired and
  released differ.
- It is `WARN_ON_ONCE`: it will not print again this boot, whatever
  happens.
- The full trace with registers is in
  `/var/tmp/zc-run-logs/hang-457-x-tight-RN/kern-after.log` and in
  `journalctl -k -b`.

**Examined afterwards, from the code and the debuginfo; not reproduced:**

- **The caller** (`svc_tcp_recv_actor+0x594`, by `addr2line` on
  `sunrpc.ko.debug`) is the "No budget to draw on: copy, do not loan"
  call in the actor's credit block.
- **The cell** was `collapse-off` (budget 0, no receive buffer held),
  which finished at 20:29:42 and recorded 7 collapses.
- **The likely cause is a merge page counted twice.** The merge function
  (the one that sets `state->merge_fill`) appends its page with
  `svc_tcp_rx_append(..., borrowed = true, ...)`, which adds the bytes to
  `borrowed_bytes`, and then moves them out again: `borrowed_bytes -=
  tail + len; copied_bytes += tail + len` ("the page holds copies; keep
  the byte accounting honest"). The page's bit stays set in
  `state->borrowed`. `svc_tcp_rx_materialize()` then does `borrowed_bytes
  -= bvec->bv_len` for every bvec with that bit, so a merge page's bytes
  come off a second time, `borrowed_bytes` wraps below zero, and the
  closing `WARN_ON_ONCE` sees it non-zero and returns `-EINVAL`, which
  faults the receive. `copied_bytes` is counted twice the same way.
- **Consistent with the registers:** `x0` is `0xfffbb110`, a small
  negative number as a `u32`.
- **Why here:** in `collapse-off` a record's first skbs can be collapsed
  copies, which are short non-region segments and get merged; the next
  skb carries region pages, the budget is 0, and the record is
  materialized with a merge page in its table. Any materialize after a
  merge (the capacity fallbacks too) should trip the same check; that is
  a reading, not a test.
- **Not fixed.** `materialize` cannot tell a merge page from a loaned
  one: there is no per-entry mark. Either mark merge pages (a third
  bitmap) or stop moving the bytes at merge time; Mike's choice. A KUnit
  case (merge, then materialize) would show it without a collapse.
- **Related to section 3 only by the trigger:** both need a collapse
  under a record in progress. The code paths are separate.

## 6. Task 3: finish build 457's record

Only on a kernel with the fix, since `x-tight-RN` can hang any run:

1. Read every stage of this boot 1 that did run (section 2's "not read"
   list), in particular both LOCALIO stages.
2. Re-run the plan from `budget-X` (`--redo budget-X` re-runs it and
   carries on; do not follow with `--only`), reboot, boot 2.
3. Then the 64 KiB record, as `tardis1-boot1-testing.md` section 9 step 6
   says. The 16-connection gap (its section 10.2) is still open.

## 7. State of the host

- **It is a live server**; ask Mike before any run. Start the plan from a
  directory other than `transplant/`.
- The rig is down, nfsd is active, bpftrace stopped, budgets all 0.
- **Since 21:57 the NFS and sunrpc modules are the rebuilt, fixed ones**
  (section 8.2), not the shipped ones; since 23:08 `nfs_localio` is
  section 9's build (`/var/tmp/zc-mod-457fix/perend/`), and **since
  00:38 on 2026-10-07 `nfs_dio`, `nfs` (with its five client modules)
  and `nfsd` are section 10's** (`/var/tmp/zc-mod-457fix/eof/`), which
  has everything of section 9 plus the beyond-EOF rule.
- **The debugfs gate is `direct_misaligned_min_bytes` (8192) on the
  loaded nfsd**, not `direct_misaligned_min_pages`; a reboot of build 457
  brings the page-counted one back.
- NFS service was stopped six times on 2026-10-06 between 21:57 and
  23:40 (four module swaps, one of which took two attempts and one
  about 75 seconds, and two KUnit runs), and once more at 00:37 on
  2026-10-07 (9 seconds). pl-105 reconnected each time.
- **Two settings were not restored**, because harness runs were killed
  with SIGKILL early in the triage: `page_pool.rx_region_dev` reads
  `rxsrv,rxsrv2` and `/sys/kernel/tracing/tracing_on` reads 0. Their
  values at boot were not recorded; a reboot clears both.
- `nfsd` `io_cache_write` reads 2 and `tcp_rmem` `65536 131072 33554432`;
  not compared with boot values.
- The kernel is tainted (KUnit ran, the warning in section 5).
- An untracked `NFSD_TCP_WRITE_ZEROCOPY/statx-dio` binary was back
  (a harness build product); it was gone by the next session. Delete it
  before committing if it returns.
- A mistake to avoid: `pkill -f run-testplan.sh` from a shell whose own
  command line contains that string kills the shell too.
- Results directories for builds 454 and 455 are still in `/var/tmp`.

## 8. The fixes on build 457's running kernel, with rebuilt modules

Mike asked for the fixed modules to be built against build 457's
`kernel-64k-devel` and loaded into the running kernel, rather than wait
for a kernel build. Everything is in `/var/tmp/zc-mod-457fix/`.

### 8.1 How they are built (done, about 30 seconds in all)

The struct change means the changed header must win over the devel
tree's copy, and with `CONFIG_MODVERSIONS` (gendwarfksyms) it changes
the CRC of 216 of sunrpc's 327 exported symbols, so **every module that
imports from sunrpc is rebuilt**, not only sunrpc and nfsd:

```
KB=/lib/modules/$(uname -r)/build; W=/var/tmp/zc-mod-457fix; S=~/snitm/kernel/linux
mkdir -p $W/overlay/include/linux/sunrpc
cp $S/include/linux/sunrpc/svcsock.h $W/overlay/include/linux/sunrpc/
make -j48 -C $KB M=$S/net/sunrpc MO=$W/sunrpc CC="gcc -I$W/overlay/include" modules
ex=$W/sunrpc/Module.symvers
for d in fs/nfs_common fs/lockd fs/nfs fs/nfsd; do n=$(basename $d)
  make -j48 -C $KB M=$S/$d MO=$W/$n CC="gcc -I$W/overlay/include" \
       KBUILD_EXTRA_SYMBOLS="$ex" modules; ex="$ex $W/$n/Module.symvers"; done
```

- `CC="gcc -I..."` puts the overlay ahead of the devel tree's `include/`;
  `svcsock.h` is the only header that differs from the devel tree
  (checked with `diff -rq` over the sunrpc, nfs, lockd and page_pool
  headers). `MO=` keeps the objects out of the source tree.
- 20 modules: `sunrpc`, `auth_rpcgss`, `rpcsec_gss_krb5`, `rpcrdma`,
  `grace`, `nfs_acl`, `nfs_dio`, `nfs_localio`, `lockd`, `nfs`, `nfsv3`,
  `nfsv4`, the three layout drivers, `nfsd`, and the four KUnit modules.
- They are unsigned and have no BTF ("Skipping BTF generation ... due to
  unavailability of vmlinux"); the kernel does not enforce signatures
  (`sig_enforce` N, lockdown none). The compiler is gcc 11.5.0-14, the
  kernel's 11.5.0-15.
- **The method was checked against the shipped module:** the same build
  of the tree with both fixes reversed (`$W/refsrc`, a detached worktree;
  remove it with `git worktree remove`) gives all 327 CRCs equal to the
  devel `Module.symvers` and all 3116 functions of `sunrpc.ko` the same
  size as the shipped one. Its srcversion differs from the shipped
  `9611113753DDA35599A6AE2` (`6B3F8B7A0EF7110D220E1BE`); not explained.
- The fixed sunrpc's srcversion is `4BB9ADB459E9D7ED8831736`.

### 8.2 Loading them (done 2026-10-06 21:57, NFS service down about 2 minutes)

**The rebuilt modules are what is loaded now**, until the next boot
(nothing was installed under `/lib/modules`; a reboot of build 457 comes
back with the shipped, unfixed modules). `cat /sys/module/sunrpc/srcversion`
tells which: `4BB9ADB459E9D7ED8831736` rebuilt, `9611113753DDA35599A6AE2`
shipped.

```
systemctl stop nfs-server nfs-mountd nfs-idmapd nfsdcld rpc-statd rpc-gssd gssproxy
umount /proc/fs/nfsd; umount /var/lib/nfs/rpc_pipefs
for m in nfsd rpcsec_gss_krb5 auth_rpcgss rpcrdma blocklayoutdriver nfs_layout_flexfiles \
         nfs_layout_nfsv41_files nfsv4 nfsv3 nfs nfs_acl lockd grace nfs_localio nfs_dio sunrpc; do rmmod $m; done
cd /var/tmp/zc-mod-457fix
for m in sunrpc/sunrpc nfs_common/grace nfs_common/nfs_dio nfs_common/nfs_localio nfs_common/nfs_acl \
         lockd/lockd sunrpc/auth_gss/auth_rpcgss sunrpc/auth_gss/rpcsec_gss_krb5 sunrpc/xprtrdma/rpcrdma \
         nfs/nfs nfs/nfsv3 nfs/nfsv4 nfs/filelayout/nfs_layout_nfsv41_files \
         nfs/flexfilelayout/nfs_layout_flexfiles nfs/blocklayout/blocklayoutdriver nfsd/nfsd; do insmod $m.ko; done
systemctl start var-lib-nfs-rpc_pipefs.mount nfsdcld nfs-idmapd      # before nfs-server, see below
echo 1 > /sys/kernel/debug/nfsd/disable-splice-read
echo 2 > /sys/kernel/debug/nfsd/io_cache_read; echo 2 > /sys/kernel/debug/nfsd/io_cache_write
sysctl -w fs.nfs.nlm_grace_period=10
systemctl start nfs-server
```

Traps, all met once:

- **`systemctl start nfs-server` does not bring `rpc_pipefs` back** after
  it was unmounted by hand: `nfsdcld` stayed down, `nfs-idmapd` failed,
  and nfsd started with "Unable to initialize client recovery tracking!
  (-110)". It ran like that for about a minute before the mount and the
  two daemons were started and nfsd restarted. The one connected client
  (pl-105, NFSv4.2) was confirmed again afterwards.
- **A reload resets what was set at runtime:** nfsd's debugfs
  (`disable-splice-read` 1, `io_cache_read` 2, `io_cache_write` 2 before;
  0, 0, 0 after) and `fs.nfs.nlm_grace_period` (10 before). Restored by
  hand as above; the before and after values are in
  `/var/tmp/zc-mod-457fix/state/`. Every other sunrpc, nfsd, nfs and lockd
  parameter and sysctl compared equal.
- After a KUnit run `region_budget_stats` shows the last test's budget
  (`budget 10`) until the pool is next used; the requested budget is 0.
- The rebuilt sunrpc has no BTF, so `credit.bt` (fentry on sunrpc
  functions) does not attach. `/var/tmp/zc-run-logs/verify-457fix/credit-nobtf.bt`
  is the same trace with kprobes and `struct svc_sock` offsets (600, 616,
  624 for `sk_credit_wait`, `sk_credit_ticket`, `sk_budget` in this
  build; re-derive with `pahole -C svc_sock sunrpc.ko` for any other).

### 8.3 What ran on the rebuilt modules

Logs in `/var/tmp/zc-run-logs/` and `/var/tmp/zc-run-logs/verify-457fix/`.

| what | result |
|---|---|
| KUnit, four suites (`/var/tmp/zc-mod-457fix/run-kunit-rebuilt.sh`, which is `run-kunit.sh` loading the rebuilt test modules by path; log `kunit-457fix-rebuilt.log`) | **PASS**: `sunrpc-svcsock-rx` 43/43 with `ok 23 svcsock_rx_credit_collapsed_unwaits_test` and `ok 32 svcsock_rx_merge_then_materialize_test`; xdr 7/7, nfsd 6/6, nfsd4 10/10 |
| the hang: `CELLS=x-tight-RN REPS=80 STORAGE=prp PHASE=X TMO=300 run-budget-cells.sh`, traced (`run-hang1.log`, `trace-hang.out`) | **80 of 80 completed**, `R_rc 0` and `N_rc 0` every time (R 137-141 MB/s, N about 2140), none killed by its timeout. Unfixed, the cell hung at repetition 34 |
| the collapse the fix is for, in that run | **seen once and survived**: `COLLAPSE ... sv=0xffff008079b5e800 tk=128100 waiting=1 rmem=131328 rcvbuf=131072`, then `WAKE` of that transport, then `FORGET ... waiting=1 from=svc_tcp_recv_actor` 8 us later, then it is woken again with tickets 128118, 128140 and the run goes on. That is the only `FORGET` of a listed transport in the trace. Budget at the end: `inflight 0 waiters 0` |
| materialize: `CELLS=collapse-off STORAGE=sgl PHASE=A run-budget.sh`, five times, traced (`run-collapse-off-N.log`, `trace-collapse-off.out`) | `fio_rc 0` five times; **14565 materializes from the receive actor, none returned an error**, no warning in the log. The fifth run recorded 6 collapses (one on a server socket in the trace) |

What this does and does not show:

- **The hang fix is verified at runtime**: the event that hung build 457
  happened and the cell finished.
- **The materialize fix is verified by KUnit only.** The runtime runs did
  not trip the check, but nothing shows that a record holding a merge
  page was materialized in them (the merge is not traced), and unfixed
  the warning was seen once in one run. The KUnit case drives exactly
  that sequence. The sunrpc module was reloaded, so its `WARN_ON_ONCE`
  was armed again for these runs.
- **Neither KUnit case was shown to fail on unfixed code.** The
  reference build in 8.1 could do it (its `svcsock_kunit.ko` has both
  cases, its `sunrpc.ko` neither fix) with one more module swap; not
  done.
- The clamp question of section 4 is still open: the collapse again came
  with the queue just over the buffer (131328 against 131072).

The kernel is tainted by the unsigned modules as well as by KUnit. The
rig is down, budgets are 0, bpftrace is stopped.

## 9. 2026-10-06 night: the write split is cut per end again

Mike, after sections 0 to 8 were done: the page cut of
`slipstream/NFSD_LOCALIO_DIRECT_SPLIT.md` section 7 costs too much (a
write aligned to the device but not to a page is buffered), so go back
to cutting at the direct I/O alignment, reproduce the failure that
motivated the page cut, and fix that failure instead. **That design
record has not been updated; this section is the only account.**

### 9.1 The commits (at the tip, not pushed, not regrouped, not on the HS twin)

In this order, so that no commit has the cut without its fix:

| commit | what |
|---|---|
| `62286e633852` | NFS/localio: complete a direct write that shares a page before it returns |
| `08d80dc89a93` | nfs_common: leave an aligned end of a split write where it is |
| `11e038c05ac5` | NFSD: count the misaligned-WRITE split gate in bytes, not pages |
| `7b96742f6f12` | NFSD: give the KUnit split test the byte-counted gate |
| `b494cd4d0711` | NFSD: test the per-end cut of a split write in the KUnit suite |

- **The cut and the gate are Mike's**, taken from the state before the
  page cut: `git diff v7.1.13-20 cb4917369ce9`, for `fs/nfs_common/nfs_dio.c`
  byte for byte, with `nfsd-io-modes.rst` and the gate rename from the
  same diff. Two conflicts with this branch were resolved by keeping its
  `seg_boundary` policy field. (A first attempt that cut writes at the
  offset alignment exactly as reads are was rejected by Mike and never
  run.)
- The KUnit suites do not build between `11e038c05ac5` and
  `7b96742f6f12`: the code renames the policy field the test sets.

### 9.2 The cut

For a write, each end on its own: an end that is an `offset_align`
multiple has no buffered segment and the middle starts or stops there,
mid-page or not; a misaligned end is cut at a multiple of the larger of
`offset_align` and the page size. Reads are cut at `offset_align`.
Before the gate:

| ends | direct middle |
|---|---|
| both aligned | the whole write |
| one aligned | from the aligned end to the last page boundary before the misaligned end; none if no page boundary lies inside the write |
| both misaligned | the whole pages between the cuts; none if the write holds no whole page |

**So "a misaligned write that holds no whole page is one buffered
segment" still holds for a write with two misaligned ends**, and Mike
asked about exactly that. iorhard's records are almost all of that
kind, by arithmetic: records are back to back, 47008 = 32 x 1469, so a
record boundary is a 4096 multiple every 128 records and 713 of 45680
records have one aligned end (6000 = 16 x 375: every 256, 2797 of
357912). With a page boundary also needed inside the record, **490 and
174 writes a run have a direct middle**, which the traces match
exactly. Cutting a misaligned end at the offset alignment too would
lift this; it was discussed and **not decided**. It would put a
write's own buffered end and its own direct middle in one page (issued
one after the other, so by reading the code safe), and bring back the
read-back the page cut was first made for (0.988 reads per WRITE at
16 KiB).

### 9.3 The failure, reproduced

`run-sgl-localio-ab.sh loop` (LOCALIO, XFS on nvme-loop, 4096-byte
alignment, eight writers), the per-end cut, the gate at 0 so that small
middles are issued. In `/var/tmp/zc-run-logs/diocut-perend/`:

- **Six of six runs failed a write with `EIO`** at 6000-byte records
  (one of six at 47008), 17 failed invalidations in all.
- `inval.bt` there traces every failed `invalidate_inode_pages2_range()`
  with its page range and caller. **Use it, not the kernel message:**
  the message is limited to ten a day and had printed ten by 22:50.
- Every failure came from `kiocb_invalidate_post_direct_write()` <-
  `iomap_dio_complete()` <- `iomap_dio_complete_work()`, in a kworker.
- The first one, page 2039: record 22271 (offset 133626000) was split
  as a 1904-byte buffered prefix and a 4096-byte direct middle with no
  suffix; three neighbours were buffered in that page within a
  millisecond (`loop/boundary/rs6000.trace`).

### 9.4 The cause and the fix

- A direct write invalidates the page cache before it is submitted
  (failure there falls back to buffered I/O, which is safe) and again
  after it completes (failure there sets `-EIO` in `wb_err`).
- For a synchronous kiocb XFS holds `IOLOCK_SHARED` across
  `iomap_dio_rw()`, completion included, and a buffered write takes it
  exclusive. For an asynchronous one the lock is dropped at
  `-EIOCBQUEUED` and completion runs with nothing held.
- **LOCALIO used asynchronous completion only for a direct segment that
  is the last of its write**, which under the per-end cut is exactly a
  write ending on an aligned boundary. NFSD's kiocbs are always
  synchronous.
- **The fix** (`62286e633852`): `nfs_dio_write_shares_page(pos, len)` in
  `include/linux/nfs_dio.h` is true for a direct write that starts or
  stops inside a page; `nfs_local_call_write()` uses AIO completion only
  when it is false. Where the offset alignment is a page-size multiple
  it is never true and nothing changes.
- Considered and not taken: retrying the invalidation after a flush in
  the generic code (narrows the race, does not close it, needs upstream
  agreement); having the buffered segment wait for direct I/O in flight
  (a new asynchronous direct write can start after the wait).

### 9.5 What was run with the fix (all on build 457, 64 KiB, rebuilt modules)

| what | result |
|---|---|
| LOCALIO, 6000-byte records, gate 0, twelve 15 s runs on one rig (`focus/`, `inval-focus.out`) | 12 of 12: every write succeeded, **0 failed invalidations**, 357912 WRITEs each |
| LOCALIO, 6000 and 47008, gate 0, the full script (`final0-1`, `final0-2`; and `fixA1..5` on an equivalent cut with the page-counted gate at 0) | 7 of 7 clean |
| LOCALIO, 6000 and 47008, default 8192-byte gate (`final1..5`) | 5 of 5 clean; at this gate the 6000-byte record's 4096-byte middle is not issued |
| NFSD, `io_cache_write` 2 (direct), LOCALIO off, gate 0: 6000 x 4 runs, 47008 x 1 (`nfsd-boundary.sh`, `nfsd-gate0/`) | every write succeeded, 0 failed invalidations; 174 and 490 writes a run `disposition=direct`. **First run of the NFSD path with this cut.** The script's `with_direct_middle=0` is its own parsing error; count `disposition=direct` |
| KUnit, four suites, before the three cases were reworked (`kunit-perend.log`) | `nfsd-receive-bvec` 5/6: the three cases that described the page cut; xdr 7/7, svcsock 43/43, nfsd4 10/10 |
| KUnit `nfsd-receive-bvec` alone, after (`b494cd4d0711`) | 6/6 |

Read-back is unchanged by any of this: about 0.70 READ bios per WRITE
at 6000 bytes and 0.35 to 0.40 at 47008, LOCALIO. It is a cost, not a
failure, and was left alone on purpose.

### 9.6 Not done, not known

- **The four KUnit suites have not been run together on the committed
  tree**; `nfsd-receive-bvec` was run alone after its cases changed.
- **Only XFS.** The fix relies on the file system keeping buffered
  writes out for the length of a synchronous direct write. ext4 and
  others were not read or run.
- **Cost not measured:** a workload of writes aligned to the device but
  not to a page now occupies its LOCALIO worker until each completes.
- **Only 64 KiB.** 16 KiB, and 4 KiB over 512-byte blocks, reach the
  same geometry and were not run.
- **Docs owed:** `slipstream/NFSD_LOCALIO_DIRECT_SPLIT.md` (its sections
  7 and 1 now describe a cut the code no longer has),
  `localio-boundary-test.sh`'s comments on the cut and the gate (its
  `gate_bytes()` already reads either knob), `AGENTS.md`'s line that
  `v7.1.13-20` carries the page cut and the page-counted gate.
- The runtime check to repeat on a built kernel: `inval.bt` attached,
  `echo 0 > /sys/kernel/debug/nfsd/direct_misaligned_min_bytes`,
  `BOUNDARY_RS="6000 6000 6000 6000" bash focused.sh loop <outdir>`
  (`focused.sh` in the run directory is `run-sgl-localio-ab.sh` without
  its short-read and stable tests), then the gate back to 8192. Wanted:
  no `INVAL-FAIL` line and no failed write.

### 9.7 The 16-connection `mixed` copies: all direct again, and slow again on growing files

Section 2 noted two things about build 457 "in passing": growing-file
copies four to five times faster, and the `mixed` copies no longer all
direct. **They are one effect, and it was the page cut.** Phase A's
copy cells re-run on the loaded per-end cut (default 8192-byte gate),
`CELLS="mixed-off mixed-copy mixed-sized-copy mixed-on" STORAGE=<prp|sgl>
PHASE=A run-budget.sh`, one run each, in
`/var/tmp/zc-run-logs/diocut-perend/mixed-prp` and `mixed-sgl`. MB/s,
then segments direct + DONTCACHE:

| cell | storage | build 455 | build 457 (page cut) | per-end cut |
|---|---|---|---|---|
| `mixed-copy` | nvme-loop | 440, 2785 + 0 | 2105, 1407 + 3938 | 446, 2785 + 0 |
| `mixed-copy` | NVMe/TCP | 446, 2785 + 0 | 2223, 1407 + 3938 | 447, 2785 + 0 |
| `mixed-off` | nvme-loop | 438, 2785 + 0 | 1956, 1407 + 3938 | 444, 2785 + 0 |
| `mixed-off` | NVMe/TCP | 447, 2785 + 0 | 1907, 1407 + 3938 | 446, 2785 + 0 |
| `mixed-sized-copy` | nvme-loop | 2684, 2785 + 0 | 2391, 1407 + 3938 | 2550, 2785 + 0 |
| `mixed-sized-copy` | NVMe/TCP | 2886, 2785 + 0 | 2413, 1407 + 3938 | 2811, 2785 + 0 |
| `mixed-on` (loans) | nvme-loop | 1160, 0 + 2785 | 1098, 0 + 2785 | 1156, 0 + 2785 |
| `mixed-on` (loans) | NVMe/TCP | 805, 2785 + 0 | 1089, 1407 + 3938 | 872, 2785 + 0 |

- **Every copy is one direct segment again**, as on build 455, on both
  kinds of storage. The page cut had made these writes (aligned to the
  device, not to a 64 KiB page) partly or wholly buffered.
- **The growing-file copies (`mixed-copy`, `mixed-off`) are back to
  about 445 MB/s from about 2000.** The speed on build 457 was those
  writes not being direct, not a gain. The same workload on files that
  already have their size (`mixed-sized-copy`) is all direct at 2550 to
  2811, a little above the page cut's 2391 to 2413.
- That direct writes to a growing file are slow is the series' earlier
  finding (XFS makes a direct write that starts beyond end-of-file wait
  for the direct writes in progress; `PREALLOC` in the harness exists
  for it). **Not re-checked that night**; the sized twin being fast is
  consistent with it.
- Loans on NVMe/TCP (`mixed-on`) are all direct again too, 872 against
  1089 under the page cut and 805 on build 455.
- No write failed, nothing in the kernel log. **Single runs.**

So the per-end cut gives back direct I/O for these writes and, with it,
the growing-file cost the page cut had hidden. Whether that cost is
acceptable, or wants its own answer, is Mike's to decide; nothing was
changed for it.

### 9.8 Which cut serves copy mode (assessment, 2026-10-07; Mike has not ruled on it)

Mike's goal for the change: let copy mode (budget 0, region records
copied) issue O_DIRECT more often than not. The assessment given, from
this section's runs and the code:

- **A copied record's memory is not the obstacle**: it lands in the
  request's own pages. Whether it goes direct is decided by the file
  offsets of its two ends.
- **Most client WRITEs have both ends on a 4096 multiple** (page cache
  writeback from a 4 KiB-page client, aligned O_DIRECT; a file's tail
  is the exception). That is general Linux client behaviour, **not
  measured here** beyond the fio cells of 9.7.
- **The per-end cut (committed) makes exactly those writes direct
  throughout** at any server page size: 9.7's copy cells, 2785 direct
  segments and nothing buffered.

On 64 KiB pages:

| scheme | write aligned to the device, not to a page | write with misaligned ends | cost |
|---|---|---|---|
| page cut (`v7.1.13-20`) | buffered outside whole pages (1407 direct + 3938 DONTCACHE in the copy cells) | direct only for whole pages | loses direct I/O |
| per-end (committed) | all direct | direct only for whole pages, or up to an aligned end | needed 9.4's fix; verified |
| offset alignment at every end | all direct | a direct middle almost always | a write's middle shares a page with its own buffered ends; 0.988 reads per WRITE at 16 KiB under the original code; **untested on this code** |

**Recommended: keep the per-end cut.** The third scheme only adds
direct I/O for writes with two misaligned ends (iorhard's records),
which is not what copy mode mostly sees.

Two limits on the benefit, whatever the cut:

- **Growing files.** Direct is not faster there (9.7: about 445 MB/s
  all direct against about 2000 partly buffered; 2550 to 2811 on files
  that already have their size). "Direct more often" buys a flat page
  cache in copy mode, and throughput only where files are preallocated
  or overwritten. **This is the next thing to decide.** Section 10 is
  what was then done about it.
- **Loans.** On storage that needs whole pages a loaned write cannot go
  direct under any cut; on storage that takes arbitrary pieces loans
  were all direct again under the per-end cut.

### 9.9 How this build was made and loaded

As section 8.1, with `include/linux/nfs_dio.h` added to the overlay,
into `/var/tmp/zc-mod-457fix/perend/` (`fs/nfs_common`, `fs/nfs`,
`fs/nfsd`; `fs/lockd` and `net/sunrpc` are section 8's). Loaded srcversions:
`nfs_dio` `CFF9C7DDC678CC81302DF48`, `nfs_localio`
`072AB9D318501946DB7FAA4`, `nfs` `4A6413247179A1E8A8DA9D9`, `nfsd`
`2C5C663D4C9E884235B972A` (nfsd's is of the build before the KUnit
cases changed; nfsd itself did not change).

- **Swapping `nfs_dio` needs `nfsd` and `nfs` unloaded, and the gate
  rename changes the version of `nfs_localio`'s `nfs_to` as well**, so
  `nfs_localio` goes with them. Missing that left NFS down for about 75
  seconds with "disagrees about version of symbol" for every import.
- `nfsd` does not unload while `nfsdcld` runs: stop `nfsdcld`,
  `nfs-idmapd` and `nfs-mountd` with `nfs-server`, start the first two
  again before it. `sunrpc` can stay, so `rpc_pipefs` stays mounted.
- Swapping only the client side (`nfs`, `nfsv3`, `nfsv4`, the three
  layout drivers) needs no server stop when nothing is mounted.
- Other directories there (`exp-perend`, `exp-nfs-sync`) are earlier
  builds of the same work and can go.

## 10. 2026-10-07, after midnight: direct writes to a growing file on XFS

Mike asked for the growing-file cost of sections 9.7 and 9.8 to be
confronted: is it XFS's design, and can it be fixed.
**`slipstream/XFS_BEYOND_EOF_DIRECT.md` is the full account**
(measurements, the XFS code, upstream references, what was ruled out);
this is the summary and the state.

### 10.1 What was found

- Traced at nfsd's call into XFS (`/var/tmp/zc-run-logs/xfs-eof/eof.bt`),
  phase A's 16-connection copy cells on nvme-loop: on files that grow
  (`mixed-copy`, 442 MB/s) a direct WRITE waits 95 ms on average before
  its I/O starts and the I/O takes 3.3 ms; on the same files given
  their size (`mixed-sized-copy`, 2804 MB/s) it waits 5 us.
- **It is XFS's design.** A write that starts beyond EOF takes the
  IOLOCK exclusive and waits for every direct write in flight, so that
  it can zero from a file size that cannot move
  (`xfs_file_write_zero_eof()`). nfsd's direct writes hold the lock
  shared across their I/O, so writes beyond EOF run one at a time,
  storage time included. 550 of 2913 WRITEs zeroed beyond EOF.
- **Not fixable in XFS by this project.** The wait was added upstream on
  purpose; skipping it needs proof that nothing in the gap needs
  zeroing, and from the code a zeroing pass without it can meet a
  write whose blocks are converted but whose size is not yet raised.
  A reading, not a test; nothing was tried in XFS.

### 10.2 What was done (committed at the tip, **not decided**)

| commit | what |
|---|---|
| `7809bfb4c8e9` | nfs_common: do not issue a write that starts beyond EOF as direct I/O |
| `aeed1c15a2ac` | NFSD: give the KUnit split test's file a size |

`nfs_dio_split()` issues a write that starts beyond the file's size
(read unlocked, a hint) as one buffered segment, DONTCACHE by policy: a
new disposition `NFS_DIO_BEYOND_EOF`, `beyond_eof` in
`nfsd_write_dio_split`. NFSD and LOCALIO both get it. The KUnit split
test dereferences a file with no inode between the two commits.

Measured with it loaded, the same two cells, one run each
(`/var/tmp/zc-run-logs/xfs-eof/proto-*`):

| | growing, before | growing, with the rule | sized, before | sized, with the rule |
|---|---|---|---|---|
| throughput | 442 MB/s | **2355 MB/s** | 2804 MB/s | 2526 MB/s |
| WRITEs issued direct | 2913 | **668 (23%)** | 2913 | 2912 |
| WRITEs buffered as beyond EOF | 0 | 2245 (77%) | 0 | 1 |

- The growing-file copy is fast again, and **most of its WRITEs are no
  longer direct**: with no wait, the size is read at once and most
  out-of-order writes are still ahead of it. The buffered ones are
  DONTCACHE, so the page cache stays flat.
- Files that have their size are unaffected.
- `fio_rc 0`, fio's verification passed, nothing in the kernel log.
  `nfsd-receive-bvec` 6/6 on this build, run alone.

### 10.3 Open

- **Whether the rule stays is Mike's decision**, put to him as: keep it
  and add its KUnit case; drop it (revert `7809bfb4c8e9`) and accept
  slow direct writes to a growing file; or make it a policy knob beside
  `direct_misaligned_dontcache`, suggested default on. It restores the
  throughput and gives up direct I/O for files that grow out of order,
  which is against the aim of 9.8 for exactly those files.
- No KUnit case for `beyond_eof`. The other three suites were not run
  on this build. LOCALIO was not run with the rule. Only XFS, only
  64 KiB, only nvme-loop, single runs.
- `nfsd-io-modes.rst` does not describe the rule or the new
  disposition.
- The swap for this build: as section 9.9, but `nfs_localio`'s exports
  did not change (checked against the build's `Module.symvers`), so
  only `nfs_dio`, the six client modules and `nfsd` were swapped; done
  in the order of 9.9 it took 9 seconds.


## 11. Build 458, boot 1 (2026-10-07): complete, every stage PASS or DONE

**A new session after the reboot starts here: boot 2 is what is left.**
Run `preflight.sh`, then `run-testplan.sh` (from
`/var/tmp/zc-run-logs/cwd-458`, logging to
`/var/tmp/zc-run-logs/boot2-458.log`); it runs `budget-B`,
`budget-B-sgl`, `analyze-AB`, `analyze-AB-sgl` and `summary`. Mike's
plan for the cycle: if boot 1 and boot 2 both pass, update the claude.ai
artifact ("NFSD Receive Region Budget",
`https://claude.ai/artifact/3UQkMVoWW8p9A5ZASPnm3g`, is the most recently
updated one; confirm with Mike which he means).

Kernel `7.1.13-20.hs.458.loanpages_pool.el9.aarch64+64k`, packaged RPMs,
booted 2026-10-07 05:43; sunrpc `299EF7D944C6D4A8DA14765`, nfsd
`5575F68355368469250681E`. **Built from the `-WIP` branch on purpose
(Mike):** it has the hang fix, the materialize fix and section 9's
per-end cut (`direct_misaligned_min_bytes`, 8192), and **not** section
10's beyond-EOF rule (no `beyond_eof` disposition in `nfsd.ko`).
`preflight.sh` PASS on the fresh boot
(`/var/tmp/zc-run-logs/preflight-458.log`). Results:
`/var/tmp/zc-testplan-7.1.13-20.hs.458.loanpages_pool.el9.aarch64+64k/`.
Run log `/var/tmp/zc-run-logs/boot1-458.log`, 05:48 to 06:41, 53
minutes. One nfsd client (pl-105) was connected; Mike asked for the run.

- **Section 0's armed check: done by the plan's `kunit` stage, on the
  shipped modules.** `ok 23 svcsock_rx_credit_collapsed_unwaits_test`,
  `ok 32 svcsock_rx_merge_then_materialize_test`; 7/7, 43/43, 6/6,
  10/10, no skip, `== KUNIT PASS`.
- `bvecrepro`, `rig-cmp`, `loan-assert`, `killswitch`,
  `system-correctness` PASS. SGL rigs `dd_fails=0 cmp_mismatches=0`
  each.
- **LOCALIO, both rigs, the first plan run with the per-end cut and the
  fixed test:** boundary, short-read and stable exit 0; **every writer
  finished** (45680, 22368 and 357912 WRITEs at 47008, 96000 and 6000
  on both; build 455 lost a write on nvme-loop and stopped at 40802);
  reads per WRITE 0.418 / 0.003 / 0.766 on NVMe/TCP and 0.334 / 0.005 /
  0.726 on nvme-loop; 0.0% resident.
- **`x-tight-RN` completed on both kinds of storage** (`R_rc 0`, `N_rc
  0`, 2785 admitted; R 128 and 129 MB/s). One pass each: the collapse
  that hung build 457 came about once in 34 repetitions, so this is not
  by itself the runtime proof (section 8.3 is).
- **No `svc_tcp_rx_materialize` warning.** `collapse-off` recorded 0
  collapses on both, so the sequence that warned on build 457 probably
  did not occur.
- **Budget phases:** every cell fio exit 0 (`n-exit` 4, as intended); no
  drops and no allocation failures with a budget set; overrun 0 in
  every cell (the init counter ends at `overrun 4`, as on build 455).
  Cells meant to drop did: `floor-refused` 484 and 1729, `x-off-RN`
  4137 and 3914.
- **Every copy is one direct segment again** on both kinds of storage,
  and on NVMe/TCP every loan too. Growing-file copies are at about 440
  MB/s (`seq-copy` 438 and 399, `mixed-copy` 440 and 447), as section
  9.7 says to expect without the beyond-EOF rule; sized ones 2174 to
  2594.
- One connection: loan and copy level (`seq-sized-on` 2191 / 2174 and
  2165 / 2174), 0 pauses. **Sixteen connections: the gap of
  `tardis1-boot1-testing.md` 10.2 is unchanged** (`mixed-sized-on` 1236
  / 2594 and 1142 / 2521, about 950 pauses; `step-on-n16` 1229 / 1912
  and 1009 / 1846).
- **Kernel log:** no WARNING, BUG, stall, allocation failure or
  invalidation failure.

Not read cell by cell: phases D, TLP and N beyond their exit statuses
and the `n-sum` refusal (`init 270 + B 338 over 555 refused`). Nothing
is in `RESULTS.md` or `results/64KiB.tsv` yet. fio's six state files
are in `cwd-458`; the untracked `statx-dio` came back and was deleted.

## 12. Build 458, boot 2 (2026-10-07): the plan is complete for build 458

**A new session starts here: nothing of the plan is left to run.** What
is left is the 64 KiB record and Mike's decisions (below).

Fresh boot of the same kernel (sunrpc `299EF7D944C6D4A8DA14765`, nfsd
`5575F68355368469250681E`), 09:22 to 09:27, five minutes. `preflight.sh`
PASS on the fresh boot (`/var/tmp/zc-run-logs/preflight-458-boot2.log`).
Run log `/var/tmp/zc-run-logs/boot2-458.log`, started from
`/var/tmp/zc-run-logs/cwd-458`. No nfsd client was connected; Mike asked
for the run.

- **Stages:** `preflight` PASS, `budget-B` DONE, `budget-B-sgl` DONE,
  `analyze-AB` PASS, `analyze-AB-sgl` PASS, `summary` DONE.
- **`analyze.py`: OVERALL PASS** for nvme-loop and for NVMe/TCP, every
  judged cell of phases A and B PASS.
- **Phase B** (region at its configured minimum, 114 pages; budget 30,
  equal to the reserve; `edge-below-n1` budget 46):

| cell | conn | nvme-loop MB/s | NVMe/TCP MB/s | peak region pages (loop, tcp) | denied (loop, tcp) |
|---|---|---|---|---|---|
| `edge-at-n1` | 1 | 363 | 332 | 24, 24 | 346, 288 |
| `edge-at-n4` | 4 | 352 | 328 | 36, 36 | 517, 512 |
| `edge-at-mixed-n4` | 4 | 146 | 144 | 36, 36 | 5583, 5606 |
| `edge-at-step` | 1 | 896 | 764 | 28, 28 | 103593, 88737 |
| `edge-below-n1` | 1 | 362 | 340 | 28, 24 | 353, 282 |
| `edge-off-n1` (no receive buffer held) | 1 | 440 | 403 | 8, 4 | 0, 0 |
| `edge-copy-n1` | 1 | 438 | 398 | 4, 8 | 0, 0 |

- **Every cell:** fio exit 0, no drops, no allocation failures, 0 paused,
  0 overrun, 0 collapsed. Loans are written dontcache on nvme-loop and
  direct on NVMe/TCP, every one (the count equals `admitted`); the two
  copy cells are direct.
- **Within a few percent of build 455's boot 2**
  (`tardis1-boot1-testing.md` section 11) in every cell: the largest
  move is `edge-at-n1` on nvme-loop, 370 to 363.
- `edge-at-step` retransmits 1359 and 1049 times with as many loss
  probes and duplicate acknowledgements and no timeout (build 455: 925
  and 1071).
- **Kernel log, boot 2:** only the `DISCARD` "operation not supported"
  line on the nvme-loop test device, once per stage. No WARNING, BUG,
  stall, allocation failure, invalidation failure or
  `svc_tcp_rx_materialize` warning.
- The summary stage's `check-grouping.sh` reports `grouping BROKEN`, as
  expected: section 10's two commits sit after docs commits.
- After the run: rig down, nfsd active, budget 0, `inflight 0 waiters 0`.

**Still to do, none of it started:**

- the artifact Mike named for this cycle (section 11). **The 64 KiB
  record is written** (2026-10-07): `results/64KiB.tsv`, `RESULTS.md`
  and "64 KiB host" in `README.md`, which also has what phases D, TLP
  and N showed;
- the regroup, the HS twin, and Mike's decision on the beyond-EOF rule
  (section 10.3), which build 458 does not have;
- the 16-connection gap (`tardis1-boot1-testing.md` 10.2), unchanged.

## 13. 2026-10-07: the 16-connection gap, what is known and the experiment armed

**A new session after the reboot starts here.** Mike rebooted for this
experiment; the region must be phase A's 555 pages, so run nothing of
phase B or D first.

Mike's question was whether the gap is XFS's beyond-EOF wait. **It is
not:** the `-sized-` cells `fallocate` their files on the server before
the client writes, the step cells overwrite a 1 GiB file, and on
nvme-loop the loan is not direct I/O and shows the same gap.

What points at the charge per WRITE instead (existing data, no new run):

- `/var/tmp/zc-run-logs/hang-457-repro/trace1.out` (build 457, 16
  connections, mixed sizes, `credit.bt`): a 1 MiB record is admitted
  with 27 pages of credit; 36% of 50400 such records then paused in
  `svc_tcp_rx_credit_charge()` wanting more, median 30, 90th percentile
  38, maximum 54. 256 KiB records: admitted with 9, 12% paused, up to 22.
- Only the 16-connection loan cells at 64 KiB pause; one connection and
  every 4 KiB and 16 KiB cell show 0.
- In `step-on-n16`'s 10 ms stall the loan runs at 48 MB/s (about one
  WRITE per 20 ms); its copy and the one-connection loan both hold 705.

**Reading, unconfirmed:** veth packs about seven frames to a 64 KiB
page, and with sixteen connections interleaved they belong to different
connections, so a record spans more region pages than the factor of 150
allows for and stalls mid-receive holding its credit.

**The experiment:** `mixed-sized-copy` and `mixed-sized-on` (and
`step-copy-n16` / `step-on-n16` if time allows), `STORAGE=prp PHASE=A`
with `CELLS=`, under `credit.bt`, at credit factors 150, 250 and 350.
`FACTOR` comes from `F9` in `run-budget.sh` (check how to override it;
the budget and the reserve are derived from it). Wanted: pauses gone and
the loan at its copy's rate at some factor. If so the 64 KiB factor is
too low for several connections, and the fix is in how it is computed.

The artifact was reworded the same day to say the gap is under
investigation and is not XFS; its source is kept at
`/var/tmp/zc-run-logs/artifact/region-budget.html` (publish to the same
URL). `RESULTS.md` and the README's "64 KiB host" still give the older
reading ("the budget is doing what it is set to do"); correct them when
the experiment has run. Mike, 2026-10-07: no regroup until he asks.

## 14. 2026-10-07: the credit factor experiment, run

Build 458, fresh boot, phase A's 555-page region (4 parked), `STORAGE=prp`.
`run-budget.sh` takes `FACTOR9=<n>` for the MTU 9000 factor; give `SIZE=`
with it, because the default region size follows the factor. Everything is
in `/var/tmp/zc-run-logs/credit-factor/` (`run.sh`, `f150` `f250` `f350`
under `credit.bt`, `d0-f150` `d0-f350` untraced with `FORCE_DELAY=0`,
`pauses.py` for the traces). All runs rc 0, fio rc 0, kernel log clean.

The budget is 423 pages at every factor, so a higher factor admits fewer
1 MiB records at once: 15 at 150 (27 pages each), 9 at 250 (44), 6 at 350
(61).

| factor | `mixed-sized-copy` | `mixed-sized-on` | paused / denied | `step-copy-n16` | `step-on-n16` | paused |
|---|---|---|---|---|---|---|
| 150 | 2958 | 1060 | 888 / 2797 | 1928 | 1183 | 100992 |
| 250 | 2789 | 1532 | 50 / 3416 | 1833 | 1582 | 31370 |
| 350 | 2232 | 1165 | 0 / 3635 | 1715 | 1560 | 0 |

The step cells by phase (MB/s; no delay, the 10 ms stall, no delay):

| factor | copy | loan |
|---|---|---|
| 150 | 2532 / 719 / 2526 | 1740 / 50 / 1740 |
| 250 | 2394 / 709 / 2394 | 2318 / 80 / 2323 |
| 350 | 2221 / 699 / 2224 | 2202 / 235 / 2208 |

The mixed pair with no storage delay (`FORCE_DELAY=0`, untraced):

| factor | copy | loan | paused / denied |
|---|---|---|---|
| 150 | 2991 | 1538 | 1020 / 1904 |
| 350 | 2651 | 2474 | 0 / 5051 |

**What it shows:**

- **The charge is too low at 150 for sixteen connections.** 59% of 1 MiB
  records pause mid-receive at 150, 16% at 250, none at 350. With the
  pauses gone and storage that does not wait, the loan is at its copy's
  rate: 2202 against 2221 (step) and 2474 against 2651 (mixed).
- **On this region a higher factor does not close the gap when storage
  waits.** Six records at once is then the limit: `mixed-sized-on` at
  1 ms is 1165 against 2232, and the stall holds 235 against 699. The
  stall at 150 is worse than its 15 records explain (50 MB/s): paused
  records keep their credit while they wait for more.
- **The factor costs the copy too**, 2958 to 2232 on the mixed cell: it
  also sets the held buffer's window clamp
  (`svc_tcp_rx_window_clamp()`).
- `step-on-n16` retransmits more as the factor rises (6969, 8036, 14018,
  all loss probes, no timeout).

**Not yet shown:** that 350 with a budget for 15 records matches the copy
when storage waits. That needs the region the factor derives (870 pages),
so a fresh boot: `FACTOR9=350` without `SIZE=`.

`credit.bt` has never printed RELEASE on these builds: GCC split the body
into `svc_tcp_rx_credit_release.part.0` and the callers in `svcsock.c`
call that. A kprobe on it counts one release per admission (2235 of each
in 10 s). **Fixed** in `/var/tmp/zc-run-logs/credit-factor/credit.bt`
(a kprobe on the part, reading `arg0`; skipped on a build without it);
the copy in `hang-457-repro/` is as it was. `pauses.py` now also gives
the credit a record held at release. `mixed-sized-on` again under it
(`rel-f150`, `rel-f350`), 1 MiB records:

| factor | admitted with | held at release: median / 90% / 99% / most |
|---|---|---|
| 150 | 27 | 39 / 45 / 51 / 57 |
| 350 | 61 | 61 / 61 / 61 / 62 |

So a 1 MiB record on sixteen connections holds up to 57 region pages for
its 17 of payload, which is a factor of about 340; 250 (44 pages) covers
fewer than nine records in ten.

`RESULTS.md`, the README's "64 KiB host" and the artifact were corrected
the same day with these results.

## 15. 2026-10-07: factor 350 on the region it derives (870 pages)

Build 458, a fresh boot, `FACTOR9=350` without `SIZE=`, `STORAGE=prp
PHASE=A`: the region is 870 pages (8 parked), the budget 734 and the
reserve 60, so twelve 1 MiB records at once at 61 pages each (section 14
expected fifteen; the 555-page region admitted six). Everything is in
`/var/tmp/zc-run-logs/credit-factor-870/` (`run.sh`, `driver.log`, `f350`
under section 14's fixed `credit.bt`, `d0-f350` untraced with
`FORCE_DELAY=0`). Both runs rc 0, fio rc 0, no drops or allocation
failures, kernel log clean.

| cell (MB/s) | copy | loan | paused / denied | on 555 pages: copy / loan |
|---|---|---|---|---|
| `mixed-sized`, 1 ms | 2581 | 1766 | 1 / 3320 | 2232 / 1165 |
| `mixed-sized`, no delay | 2619 | 2625 | 3 / 4730 | 2651 / 2474 |
| `step-n16`, whole run | 1727 | 1665 | 500 / 234719 | 1715 / 1560 |

The step cells by phase (no delay, the 10 ms stall, no delay), read from
fio's 500 ms bandwidth log and so approximate: copy 2240 / 704 / 2240,
loan 2245 / 485 / 2250.

**What it shows:**

- **With storage that does not wait the loan is level with its copy**
  (2625 against 2619, and the step cells outside the stall).
- **When storage waits the loan is at about two thirds of its copy**,
  68% in the 1 ms mixed cell and 69% in the stall (52% and 34% on 555
  pages). Twelve records at once is still a limit the copy does not
  have.
- **Pauses are nearly gone, not gone:** 481 of 190507 1 MiB records in
  `step-on-n16` (0.25%, wanting 65 or 66 pages), none of the smaller
  records. On 555 pages 350 showed none.
- **Credit a 1 MiB record held at release:** median 61, 90% 61, 99% 63,
  most 73 (62 on 555 pages). With twelve in flight 61 is not quite the
  ceiling.
- `step-on-n16` retransmits 5594 times (14018 on 555 pages), all loss
  probes, no timeout. The copy's rate is not hurt by the larger region
  (`mixed-sized-copy` 2581, 2232 on 555).

**Not settled:** how the factor and the budget should be computed for
64 KiB. A budget for fifteen or sixteen records at 350 has not been run.

`RESULTS.md`, the README's "64 KiB host" and the artifact (source
`/var/tmp/zc-run-logs/artifact/region-budget.html`, version 11) carry
these results.

## 16. 2026-10-07: the 16-record run armed, and the region's upper bound on tardis1

**A new session after the reboot starts here.** Mike rebooted for this
run; the region must be 1080 pages, so run nothing else first.

**The run:** `/var/tmp/zc-run-logs/credit-factor-b16/run.sh` (it refuses
to start if a region of another size exists). Factor 350 charges a 1 MiB
record 61 pages, so sixteen at once need a budget of 16 x 61 = 976;
`run-budget.sh` sets the budget to region - parked - 16 x 2 - 64, so
with 8 parked the region is `SIZE=1080` (67.5 MiB). Same cells as
section 15: the mixed and step pairs under `credit.bt`, then the mixed
pair untraced with `FORCE_DELAY=0`. The region is one contiguous
allocation and 67.5 MiB is the largest tried here; if it fails the
harness says so and stops.

**What it decides:** with sixteen connections and twelve records
admitted at once (section 15) four connections always wait on the
budget. If the loan matches its copy with sixteen admitted, the
remaining gap is the budget's size; if it still lags, something else
costs the loan. Mike, 2026-10-07: this is the closing run of the factor
line; after it the focus is sizing the region, so that the absolute
upper bound on tardis1 is known.

**tardis1's caches** (`lscpu`, build 458; 64 Neoverse-N2 cores, one
socket): L2 512 KiB per core, **L3 32 MiB shared by all 64 cores**,
which is 512 pages of 64 KiB. This is what the kernel reports from
firmware tables; `on-die-receive-copy/SPEC.md` lists the system cache
size as unverified, so confirm it against the SoC's documentation.

- Every region tried is larger than the L3: 555 pages (34.7 MiB, the
  default), 870 (54 MiB), 1080 (67.5 MiB).
- What counts is the pages in use, not the region's size: the region's
  free list returns the most recently freed page first
  (`net/core/page_pool.c`), so pages never needed are never touched. A
  copy holds 0.5 to 1.5 MiB whatever the region's size; a loan 11 to 20
  MiB as configured; sixteen loans at 61 pages each are 61 MiB and
  cannot fit.
- Fitting in the L3 is only an upper bound. Whether received data stays
  there under load is the hardware measurement the SPEC plans; veth
  cannot show it.

**After the run:**

1. Take the peak pages in use (`used_pages`) from it, beside those for
   six records at once (555 pages) and twelve (870).
2. Work out the largest budget whose pages in use stay under a chosen
   share of 32 MiB, per connection count. Mike picks the share; half of
   the L3 is a starting point.
3. Record that as tardis1's bound: the budget, the connections it
   supports, and the cases that must copy.

Mike, 2026-10-07: leave the HS twin branch alone until he says.
