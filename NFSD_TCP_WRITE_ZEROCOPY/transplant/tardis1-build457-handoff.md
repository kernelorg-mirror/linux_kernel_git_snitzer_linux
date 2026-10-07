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
of the KUnit tests); regroup before handing back. `regroup-backup-kunit`
is the backup branch from the one regroup that session did; delete it
when Mike is satisfied. Nothing has been carried to the HS twin.

## 0. Armed: the first thing to do on the next boot

**Before anything else on the next boot, run the two KUnit cases that
were added for the two defects, and record the result here.** Neither
had run on a booted kernel when this was written (section 8 has what was
run on build 457 with rebuilt modules).

1. `uname -r` and `cat /sys/module/sunrpc/srcversion`. Build 457's
   shipped sunrpc is `9611113753DDA35599A6AE2` and has neither fix.
2. Ask Mike before the run (the host is a live server; the KUnit stage
   stops `nfs-server` for about a minute and starts it again).
3. If the booted kernel was built from a tree with all four commits:

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
  page cut; not confirmed.
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

### 8.2 Loading them, and what ran

Not done when this was written; the next commit of this file records it.

