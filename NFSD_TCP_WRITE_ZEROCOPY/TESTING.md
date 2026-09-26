# Testing the page-loan series

How to verify the NFSD TCP write-path zero-copy series: the KUnit suites (build,
run, expected results), the runtime validation that shows the copy elimination,
and the static QA gates. Distilled from the series' own qualification procedure
(David Flynn / codex docs: `RATIONALE.md`, `BACKPORT-NOTES.md`,
`CONFIG-DERIVATION.md`).

Two levels of testing, independent:

1. **Bisect-safety / build** — does every commit build clean. Covered in
   [`README.md`](README.md) → "Building & bisect testing" (base + every
   commit, `CONFIG_WERROR=y`, KUnit modules built). Use the provided
   `v7.1.8-3-aarch64-{4k,16k,64k}.config`.
2. **Functional** — the KUnit suites (below) and real NFS traffic (below).

## KUnit coverage map

The feature commits deliberately carry no KUnit references: the KUnit
infrastructure (the suites, the IF_KUNIT exports, the fault injection)
may never go upstream, so the production series must not be coupled to
it, even weakly. The mapping lives here and in the test commits -- which
name the code they drive -- never the other way around:

- **`sunrpc-xdr-bvec`** (`CONFIG_SUNRPC_XDR_KUNIT_TEST`) — the
  authoritative XDR bvec representation and decode paths ("SUNRPC:
  distinguish/decode from/poison authoritative XDR bvecs").
- **`sunrpc-svcsock-rx`** (`CONFIG_SUNRPC_SVCSOCK_KUNIT_TEST`) — the TCP
  receive page loan: classifier, skb walker, publisher, partial-state
  exchange, materialization, capacity release, duplicate cleanup,
  valid/invalid partial exchange, published-request/socket-partial
  independence, max-capacity count-zero cleanup, provenance reuse,
  locked-head merging, and the `svc_tcp_rx_loan_pages` kill-switch.
- **`nfsd-receive-bvec`** (`CONFIG_NFSD_BVEC_KUNIT_TEST`) — the NFSv3
  WRITE receive-bvec consumer: page-array vs authoritative-bvec decode
  parity, duplicate-reply-cache checksum gathering, and DIO segment
  carving — including the IOCB_DONTCACHE no-alignment fallback
  (regression cover for the clobber fix, now folded into the base's
  heuristic commit), the interior-discontinuity gate (fused with
  the memory-alignment test into one admission walk,
  nfs_dio_iter_aligned_and_splittable()), and the device's direct I/O
  segment boundary (NVMe SGL sub-project, `NVME_SGL_SUPPORT_PROJECT.md`
  section 5): no boundary, an explicit 4096, a boundary above the page
  size, and unknown (= page-sized), plus the statx-report translation
  (`nfsd_bvec_dio_seg_boundary_test`).
- **`nfsd4-receive-bvec`** (`CONFIG_NFSD4_BVEC_KUNIT_TEST`) — NFSv4
  COMPOUND receive bvecs, including session replay against a live nfsd.

## Next regression run (handoff, 2026-09-25)

State at hand-off: branch `kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY`
(`v7.1.13-14` + the series + the two merge-top-up commits, tip at or after
`3ac9fd90f192`) checked out in `/root/kernel/linux`; Mike built a kernel
from it and is rebooting into it. Nothing on this base has run yet —
compile checks only. Run in this order, recording each result here:

1. **Identify the kernel.** `uname -r`; confirm it was built from this
   branch and carries `1e48b4d03cb8` (srcversion of `sunrpc.ko`, `nfsd.ko`,
   `nfs_dio.ko`, `svcsock_kunit.ko` against the in-tree objects, as in the
   hs.160 qualification). Label every result below with that release.
2. **KUnit** (`run-kunit.sh`; needs nfs-server state for the NFSv4 suite as
   the script sets up): expect `sunrpc-xdr-bvec` 7/7, `sunrpc-svcsock-rx`
   **54/54** (53 + the new `svcsock_rx_back_to_back_locked_heads_test`),
   `nfsd-receive-bvec` **6/6** (its `dio_segments_test` now drives
   `nfs_dio_split()`; 6 with the NVMe SGL phase 1 commits, which add the
   segment-boundary rows and `nfsd_bvec_dio_seg_boundary_test`),
   `nfsd4-receive-bvec` 9/9. Run the suite on the 64K-page config too:
   segment-boundary rows 5 and 6 (`NVME_SGL_SUPPORT_PROJECT.md` section 5)
   differ only there.
3. **Prove the regression case bites.** Rebuild only
   `net/sunrpc/svcsock.o` + `sunrpc.ko` with `1e48b4d03cb8` reverted (scratch
   branch, never on the project branch), reload `sunrpc` and
   `svcsock_kunit`: the new case must fail at the byte comparison. Restore.
   (Alternative that needs no reboot or module swap: `kunit.py run
   --arch=arm64` in a clean worktree under qemu TCG — RHEL's
   `/usr/libexec/qemu-kvm` supports TCG and needs a `qemu-system-aarch64`
   wrapper on `PATH`.)
4. **Runtime harness** (needs `BLK_DEV_RAM=m` and `ZRAM=m`, which the
   2026-09-25 `.config` lacked): `run-bvecrepro.sh`, `run-rig-cmp.sh`
   (0 dd/cmp errors, 0 `bio_split_io_at()` rejections with
   `split_einval.bt`), `run-loan-assert.sh`, `run-killswitch.sh`,
   `run-system-correctness.sh`.
5. **Reproduce the collapse geometry on purpose** — the case every earlier
   run missed. Shrink the server's receive buffer (e.g. a small
   `net.ipv4.tcp_rmem` max) so TCP collapses the receive queue, confirm
   back-to-back `locked-head` records in `zcstat.sh` / the
   `svcsock_tcp_rx_lifetime` reasons, and run `run-rig-cmp.sh` with loans on:
   must be clean with the fix. If time allows, A/B against the fix-reverted
   `sunrpc.ko` from step 3 to show it corrupting — that is the end-to-end
   proof David's AI reported (corrupt with the bug, clean with the guard).
6. **LOCALIO A/B** (`../NFS_LOCALIO_DONTCACHE/`). On `v7.1.13-14` the
   DIRECT write split is the shared `nfs_dio_split()`, which this series
   changes (per-fragment length check, discontinuity gate), so LOCALIO
   writes now run the series' code. A = stock `v7.1.13-14`
   `nfs`/`nfs_localio`/`nfs_dio`/`nfsd` modules, B = the running kernel's
   (this branch); swap them as a set (`nfs_to` CRC) with
   `swap-localio-arm.sh`. Expect no behaviour change: LOCALIO's pinned user
   buffers are page-tiled after their first entry. Run
   `localio-port-ab.sh` (resident pages, reads/WRITE, flusher rounds),
   `localio-stable-test.sh` and `localio-short-read-test.sh` on both arms.
   Note `prepare-localio-arms.sh` is hard-wired to `TAG=v7.1.13-12` and the
   hs.436 release (arms lioA/lioB/lioC of the port itself); it needs its tag,
   release check and arm definitions updated for this A/B first.
7. **Per-commit bisect walk** — required on this boot (Mike, 2026-09-25):
   every commit from `v7.1.13-14` to the tip, `CONFIG_WERROR=y` with the
   four KUnit modules built, plus the sparse adjudication, via
   `run-bisect-walk.sh` run from a scratch copy held outside the tree
   (per-commit checkouts below the docs commit remove
   `NFSD_TCP_WRITE_ZEROCOPY/`). Seed its config from the running kernel's
   (`/lib/modules/$(uname -r)/build/.config`, or regenerate from the
   `v7.1.13-14` src.rpm) with `WERROR=y` and the four suites `=m`; the
   `v7.1.8-3-aarch64-*.config` seeds are stale for this base. The walk must
   cover `fs/nfs_common/` now that the gate lives there (the script's full
   `make C=1` does). Its defaults are stale — `BRANCH=kernel-7.1.8/main-5…`,
   `BASE=v7.1.8-5`, `WALK_CONFIG=…/v7.1.8-3-aarch64-4k.config` and
   `WORKDIR=/root/kernel/linux` — so run it as
   `BRANCH=kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY BASE=v7.1.13-14
   WALK_CONFIG=/abs/path/walk.config WORKDIR=<scratch worktree>`.
   Run it in a separate worktree so the main tree stays on the tip, and
   never while a runtime step is using the modules.

### Results — `7.1.13-14.hs.439.loanpages` (2026-09-25)

Kernel built by Mike from this branch at `3ac9fd90f192` (pre-fold; after the
2026-09-26 fold the identical tree is `1fe6949a3f28`, and the fix `1e48b4d03cb8`
named below is folded into `dd2e42c5f547`) (build #19,
16:26; `sunrpc.ko` relinked 16:35, after the fix was committed 16:30).

1. **Kernel identified.** All 11 relevant installed modules
   srcversion-match the in-tree objects (`sunrpc`, `svcsock_kunit`,
   `xdr_kunit`, `nfsd`, `nfsd_bvec_kunit`, `nfsd4_bvec_kunit`, `nfs_dio`,
   `nfs`, `nfs_localio`, `brd`, `zram`); the tree was clean, the source
   carries `1e48b4d03cb8`, and `svcsock_kunit.ko` contains the new case.
   `.config` now has `BLK_DEV_RAM=m`, `ZRAM=m`, `WERROR=y`.
2. **KUnit: all pass.** `sunrpc-xdr-bvec` 7/7, `sunrpc-svcsock-rx`
   **54/54** (`ok 8 svcsock_rx_back_to_back_locked_heads_test`),
   `nfsd-receive-bvec` 5/5, `nfsd4-receive-bvec` 9/9; no WARN/BUG.
3. **The regression case bites.** `sunrpc.ko` rebuilt with `1e48b4d03cb8`
   reverted in the working copy only, loaded alone (NFS stack and
   `rpc_pipefs` unloaded) with its matching `svcsock_kunit.ko`:
   **53/54**, only the new case failing, and failing as the bug predicts —
   `merge_fill` still set after the arena copy, `copied_bytes` /
   `borrowed_bytes` wrong (the borrowed bytes were copied, not loaned), no
   loan reference on the borrowed page, **the arena's fixed RPC header
   overwritten**, and **published payload bytes not matching the record**.
   No WARN/BUG/Oops: the stray top-up stays inside the fixture's own arena
   page and its reference is released, so the damage is data and
   accounting only, as in David's reports.  Installed `sunrpc` restored and
   re-verified; tree rebuilt so its objects srcversion-match again (an
   `M=` build hashes a different file set, as noted in
   `../NFS_LOCALIO_DONTCACHE/` §5).
4. **Runtime harness: all green.** `run-rig-cmp.sh` 30 × 16 MiB:
   `dd_fails=0 cmp_mismatches=0 direct=468`, ~1.0 GB borrowed vs 164 KB
   copied, and `split_einval.bt` saw **no `-EINVAL`** (10080 unsplit, 480
   clean splits at 2040 sectors — the hs.160 shape). `run-loan-assert.sh`
   10/10 connections fully loaned and fully direct (one locked head each).
   `run-killswitch.sh` off: 16 disabled / 0 borrowed, on: 16 loaned, the
   svcsock suite passes with the switch off. `run-system-correctness.sh`
   0 fails (424 direct). `bvecrepro` **8/8 MATCH**: brd `off0` 684/160/0,
   nvme-loop 684, zram (lzo-rle) 684/160/512/0.
5. **Receive-queue collapse: not reproducible on loopback.** With
   `tcp_rmem` max 64 KiB, `lo` MTU 1500 / 576 / 256, and forced TCP memory
   pressure (`tcp_mem` "1 2 <high>", 39 pressure entries),
   `TcpExtTCPRcvCollapsed` and `PruneCalled` stayed 0 and no record carried
   `reason=locked-head` beyond the first head: loopback coalesces the
   receive queue and the sender stays inside the window, so the queue is
   never pruned. The one collapse-configured `run-rig-cmp.sh` run (64 KiB
   `tcp_rmem`) was clean (30 × 16 MiB, 0 errors) but did not exercise the
   geometry. An end-to-end reproduction needs a real NIC under
   receive-buffer pressure (David's rig) or a fault-injection hook; the
   deterministic proof is steps 2–3. All knobs were restored.
6. **LOCALIO A/B: no behaviour change.** On the LOCALIO side the series
   changes only `fs/nfs_common/nfs_dio.c` (`nfs_dio.h` unchanged, import
   CRCs identical), so arm A = `v7.1.13-14`'s own `nfs_dio.c` built as
   `nfs_dio.ko` for this kernel and arm B = the running one; nothing else
   swapped, `/lib/modules` untouched, each arm's srcversion proven before
   testing. Driver: a `localio-port-ab.sh` clone (fresh rig per arm,
   LOCALIO on, v3, 8 ranks):

   | | dioA (stock `-14`) | dioB (this branch) |
   |---|---|---|
   | rs=47008 WRITEs / reads per WRITE / resident | 45680 / 0.004 / 0.0% | 45680 / 0.006 / 0.0% |
   | rs=6000 WRITEs / reads per WRITE / resident | 357912 / 0.000 / 0.0% | 357912 / 0.000 / 0.0% |
   | DONTCACHE flusher rounds | 0 | 0 |
   | short-read test | 5/5 PASS | 5/5 PASS |
   | stability test | FILE_SYNC reported, no COMMIT | same |

   0.004 vs 0.006 is the writer-concurrency residual of
   `../NFS_LOCALIO_DONTCACHE/` §5b (0.005 at the port's tip).
7. **Bisect walk: done 2026-09-26** on the next kernel,
   `7.1.13-14.hs.440.loanpages`, over the whole branch including the NVMe
   SGL phase 1: 51/51 clean, zero branch-introduced sparse findings
   (`NVME_SGL_SUPPORT_PROJECT.md` section 6 → "Results"). The first
   attempt, below, is kept as the record: **NOT RUN — blocked on disk space.** Launched from a
   scratch copy (`BRANCH=kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY
   BASE=v7.1.13-14`, cold worktree, running config minus debug info with
   `WERROR=y` and the four suites `=m`); the cold full build of step 1
   filled `/` (54 GB, ~4 GB free at the start) after 196 s and every later
   checkout failed, so its "3/38 clean" summary is an infrastructure failure,
   not a result. A full-tree cold walk needs more free space than the host
   has; still to do. (Also learned: the script's cold/warm test counts any
   `.o`, so a `make olddefconfig` in the walk worktree — which builds the
   kconfig host tools — marks the baseline warm; `make mrproper` it first.)

Not yet done: carry the merge top-up fix (now in `dd2e42c5f547`) + `1fe6949a3f28` to
`kernel-7.1/hs-7.1.13-12.NFSD_TCP_WRITE_ZEROCOPY` (or whichever HS base comes
next).

## Test coverage status (as of 2026-09-13)

**Update 2026-09-25 — `kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY` rebased
onto `v7.1.13-14`:** compile check only (tip and the four hand-resolved
commits, `CONFIG_WERROR=y`, running `hs.436` config + `NFS_COMMON_DIO=m`).
`dio_segments_test` now calls `nfs_dio_split()` directly with nfsd's default
policy (`min_middle_pages` 2, `dontcache` set) instead of
`nfsd_write_dio_iters_init()`, so the knob pin and the `svc_rqst`/`svc_fh`
stand-ins of the `-12` version are gone; expectations unchanged, and
`later-length-misaligned` is what exercises the strengthened per-fragment
length check. Runtime note: the gate now also sits on the NFS LOCALIO write
path, so re-run `../NFS_LOCALIO_DONTCACHE/localio-port-ab.sh` alongside this
project's harness when qualifying.

**Update 2026-09-24 — sibling branch `kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY`
on `kernel-7.1.13/main` (`v7.1.13-12`):** compile check only (tip,
`CONFIG_WERROR=y`, running `7.1.13-12.hs.436` config, `NFS_LOCALIO=y`, four
suites `=m`; `fs/nfs/`, `fs/nfsd/`, `net/sunrpc/`, `lib/kunit/`). On this base
`dio_segments_test` checks `boundary`/`edges` instead of head/tail page
indices and pins `nfsd_direct_misaligned_dontcache` (default Y) for its
DONTCACHE expectations; segment expectations are unchanged.

**Update 2026-09-18 — rebase onto `kernel-7.1/hs-7.1.13-5`
(`kernel-7.1/hs-7.1.13-5.NFSD_TCP_WRITE_ZEROCOPY`, 35 commits):**

- Everything below this update was measured on the `v7.1.8-5`-based
  `main-5` branch (hs.160). On the new base only a **compile check** has
  been done: the branch tip builds `CONFIG_WERROR=y`-clean from the running
  kernel's config (`/boot/config-7.1.13-3.hs.418.el9.aarch64`) with
  `KUNIT=m` and the four suites `=m` — no warnings in `fs/nfsd/`,
  `net/sunrpc/`, `lib/kunit/`. Not yet re-run here: the four KUnit suites,
  the per-commit bisect/sparse walk (`run-bisect-walk.sh` — the
  `v7.1.8-3-aarch64-*.config` seeds still need regenerating for this base),
  and the runtime harness (bvecrepro, rig-cmp, loan-assert, kill-switch,
  system-correctness).
- The one test-side change of the rebase is in `nfsd-receive-bvec`'s
  `dio_segments_test`: `nfsd_write_dio_iters_init()` on this base takes
  the request and file handle (its `trace_nfsd_write_dio_split()`
  dereferences both, so the test hands it zeroed `svc_rqst`/`svc_fh`
  objects) and reports the buffered prefix/suffix page indices, which the
  test now checks. Every segment-count/flag expectation is unchanged: the
  base's `NFSD_DONTCACHE_FALLBACK` write path demotes a memory-misaligned or
  unsplittable middle to one whole-WRITE buffered segment, exactly what the
  cases already encode.

**Update 2026-09-09 — the `main-5` walk and the hs.160 kernel:**

- **Full arm64 4K bisect/sparse walk of the final 43-commit series**
  (`run-bisect-walk.sh`, this host, respun `v7.1.8-5` base): **43/43
  WERROR-clean, sparse verdict zero branch-introduced findings** (cold
  whole-tree baseline of 2957 pre-existing findings; step 1 ~19 min,
  incremental steps seconds; the docs commit is the one SKIP-DOC).
  Walked after the section-contiguity reorder, the KUnit-decoupling
  audit, and the base respin that folded the DONTCACHE-clobber fix —
  so this walk covers the exact history intended for push. Note: the
  walk must run from a copy of the script held outside the tree
  (per-commit checkouts remove `NFSD_TCP_WRITE_ZEROCOPY/` below the
  docs commit).
- **Kernel `7.1.8-3.hs.160.el9.aarch64+` built from the branch tip**
  (`make -j8`, host config, LOCALVERSION bumped 159→160), modules
  installed, `make install` created the BLS entry and initramfs, and
  grubby default now points at hs.160. **The `7.1.8-3` prefix in the
  release string is stale**: hs.160 is built from the `main-5` branch
  on the respun **`v7.1.8-5`** base — only the config's
  version-string seed lags; the code is v7.1.8-5-based.
- **hs.160 runtime qualification DONE (2026-09-09, post-reboot)** — the
  full harness on the booted `7.1.8-3.hs.160.el9.aarch64+`
  (v7.1.8-5-based despite the stale `7.1.8-3` uname prefix), all 11
  relevant installed modules (sunrpc/nfsd/nfs/nfsv3/nfsv4/brd/zram +
  the four `*_kunit`) srcversion-verified against the tree's objects
  first; zero fault signatures in the journal across the whole session
  (only the expected out-of-tree/unsigned bvecrepro taints):
  - KUnit (`run-kunit.sh`): 7/7, 53/53, 5/5, 9/9 — **first live
    execution of the `discontinuity-*` dio_segments cases** (this run
    predates the fused-walk rework: its 13-geometry table still carried
    the since-retired `discontinuity-page-array-short-circuit` case and
    the `page_mode` plumbing).
  - `bvecrepro`: **10/10 rows MATCH** — brd `off0=684/160/0`, nvme-loop
    `off0=684/160/0`, zram `off0=684/160/512/0`.
  - `run-rig-cmp.sh` (loans Y) with `split_einval.bt` attached for the
    whole run: 30 × 16 MiB `O_DIRECT`, **0 dd failures, 0 `cmp`
    mismatches**, 453 direct / 27 vector, 0 LOCALIO, pre-`cmp`
    `fincore` flat (0 pages), 1 006 935 840 borrowed vs 178 920 copied
    — and the probe's verdict is the headline: **zero
    `bio_split_io_at()` rejections** (480 mid-bvec splits, every return
    a valid split point; on hs.159 with the -EINVAL fallback this same
    run drew 12 rejections). The admission-time discontinuity gate
    demotes those geometries before the block layer ever sees them.
  - `run-loan-assert.sh` (CONNS=50): **800/800 1 MiB receives fully
    loaned**, fails=0, 43/50 connections fully direct, locked-head 0–1
    per connection.
  - `run-killswitch.sh`: all three properties hold — 16/16
    `reason=disabled borrowed=0` while off, mid-connection flip
    re-loans the next writes (16/16), svcsock suite green with the
    host switch off, switch restored to Y.
  - `run-system-correctness.sh`: **all cases pass** (`fails=0`), both
    versions × both modes + 2×10 anchor loops; 432 direct / 312
    vector, 0 LOCALIO.
  This closes the runtime open item from the hs.160 build — and with
  it **the qualification of the `main-5` branch is complete**: the
  bisect/sparse walk item was already closed pre-reboot by the
  adjudicated 43/43 `main-5` walk above, which covers this exact
  history, so nothing remains open against the branch. Residual
  housekeeping only (not a test gap): the `v7.1.8-3-*.config` seeds
  are stale-named for the `v7.1.8-5` base (they still resolve
  correctly via olddefconfig).

**Update 2026-09-09 (later) — the fused admission walk:**

The DIO admission gate was reworked after the qualification above:
`nfsd_dio_iter_is_aligned_and_splittable()` replaces the
`nfsd_dio_iter_is_aligned()` + `nfsd_dio_iter_is_splittable()` pair,
checking per-fragment memory alignment and interior-discontinuity
placement in **one O(nvecs) pass** (previously two full walks per
direct-eligible segment — `iov_iter_alignment()` computes its complete
OR even after the mask is violated; the fused walk early-exits on the
first violation of either property). The `XDRBUF_PAGE_BVECS`-only
short-circuit commit was **dropped** along with its `page_mode`
plumbing through `nfsd_vfs_write()`/`nfsd_direct_write()`/
`nfsd_write_dio_iters_init()`: the gate now judges the iterator's
geometry alone, and a contiguous page-tiled (copied-arena) payload
passes without ever evaluating the offset_align test, so the
per-request discriminator became unnecessary. The
`discontinuity-page-array-short-circuit` KUnit case was retired with
it — its premise (trust the payload mode over the geometry) no longer
exists, and its `{4092, 4, 4096}` layout is now correctly demoted; the
dio_segments table is 12 geometries. Rewritten in place (gate commit
reworked, short-circuit commit dropped, KUnit/doc commits adapted;
`Fixes:` tags remapped).

**Re-qualified on the same booted hs.160 kernel** (module-only reload:
new `nfsd.ko` + `nfsd_bvec_kunit.ko`, srcversion-verified; sunrpc
unchanged by the delta), zero fault signatures:

- KUnit (`run-kunit.sh`): 7/7, 53/53, 5/5 (12-geometry dio table),
  9/9.
- `run-rig-cmp.sh` (loans Y) with `split_einval.bt` attached: 30 ×
  16 MiB `O_DIRECT`, **0 dd failures, 0 `cmp` mismatches**, 468
  direct / 12 vector, 0 LOCALIO, pre-`cmp` `fincore` flat, and
  **zero `bio_split_io_at()` rejections** (479 mid-bvec splits, all
  valid).
- `run-loan-assert.sh` (CONNS=10): 160/160 receives fully loaned,
  fails=0, 9/10 connections fully direct.
- `run-system-correctness.sh`: **all cases pass** (`fails=0`), both
  versions × both modes + anchor loops; 468 direct / 276 vector,
  0 LOCALIO.

~~Reopened by the rewrite: the bisect/sparse walk~~ — **re-run and
closed (2026-09-09, same day)**. The rewritten span was walked as
`BASE=<gate~2>`: step 1 is the *unchanged parent* of the gate commit,
built cold (884 s) so the whole-tree sparse baseline (2957 normalized
findings — identical count to the pre-rewrite walk) sits *below* the
first rewritten commit, and the gate commit itself is adjudicated
rather than absorbed into the baseline. Result: **28/28 clean, 0
failures** (24 code steps + 3 SKIP-DOC + the LOCALIO tip; the
rewritten gate, expose, consumers-test, and cover commits each
`sparse=0`–`3`, all blamed to baseline). In-tree, after `mrproper`
(the production build was restored from the saved hs.160 config
afterward, srcversions re-verified).

The raw verdict initially reported **one** branch-introduced finding
(`init/main.c` `envp_init`) — a **false positive of the adjudicator's
own normalizer**: the baseline log carried the identical finding
(`init/main.c:198:12`) behind interleaved progress output containing
`+` characters (`.+..+...+init/main.c:...`), which the dots-only
prefix strip left unmatched. `run-bisect-walk.sh`'s `norm()` now
strips `+` in the leading run too; re-adjudicating the same logs
under the fixed normalizer yields **zero branch-introduced findings**
(baseline 2957, steps-2+ union 39, all blamed). With that, the
combined `main-5` walk record stands: the pre-rewrite 43/43 covers
the unchanged prefix below the gate, this 28/28 covers the gate
forward.

**Final restructure (2026-09-09, after the walk):** the branch is now
**40 commits** at that point (43 as of 2026-09-13, the additions being
the xeu experiment, the zcstat hardening, and `run-rxpasses.sh`), tree
content verified byte-identical at every step:

- the standalone **LOCALIO fix moved to the very front** of the branch
  (LOCALIO is not this project's focus); sections run LOCALIO fix →
  features → KUnit → the **single folded docs commit at the tip** (all
  project-doc updates fold into it from now on);
- the two `dio_segments_test` fixes (backing `struct file`; misaligned-
  gate split expectation) **folded into "nfsd: exhaustively test
  receive bvec consumers"**, whose merged message covers both;
- the bug/kunit fixer's upstream reference **restored verbatim**:
  `Fixes: 85347718ab0d` is the *mainline* hash of the kunit-core
  backport's origin (see its own "(cherry picked from ...)" line) and
  is never to be rewritten — an interim remap to the in-series hash
  was an error. No `Fixes:` tag in the series now targets an
  in-series hash, so rebases carry no remap burden.

The walk above predates this reorder; per-step content differs only by
the LOCALIO client files present at the bottom of every step, so the
walk was not repeated.

**Done:**

- Bisect build walk: base + all 30 series commits, `CONFIG_WERROR=y`, **4K and
  64K** configs, KUnit modules built (README.md).
- All four KUnit suites **green on the Hammerspace host** (4K kernel
  `7.1.8-3.hs.159.el9.aarch64+`): `sunrpc-xdr-bvec` 7/7, `sunrpc-svcsock-rx`
  53/53, `nfsd-receive-bvec` 5/5 (9 DIO geometries), `nfsd4-receive-bvec` 9/9;
  zero fault signatures — after the three follow-on fixes below.
- Follow-on fix commits: `checkpatch` clean (remaining line-length warnings are
  verbatim quoted oops/KTAP output), `W=1` clean for `fs/nfsd`, tip builds
  WERROR-clean on the 4K config.
- All four KUnit suites **green on a 16K page-size guest** (16K kernel
  `7.1.8-3.hs.161.el9.aarch64+`, Apple Virtualization guest on a MacBook Pro
  testbed, 2026-09-04): `sunrpc-xdr-bvec` 7/7, `sunrpc-svcsock-rx` 53/53,
  `nfsd-receive-bvec` 5/5 (all 9 DIO geometries, including the
  page-size-scaled splits), `nfsd4-receive-bvec` 9/9 (session replay against
  a hand-started v4-only nfsd); zero fault signatures. First non-4K execution
  of the suites.
- Runtime validation **on the same 16K guest** (2026-09-05, brd-backed
  nvme-loop export, LOCALIO disabled): the receive loan is fully effective —
  16 MiB of page-aligned `O_DIRECT` writes borrowed 16 784 683 bytes and
  copied 65 (small control RPCs only; every WRITE receive `mode=published
  reason=none`, 65 bvecs per 1 MiB receive), and the page-cache footprint of
  written files is **flat (0 pages cached)** whether writes take the direct
  or the DONTCACHE-buffered path. The run also surfaced a **client-visible
  EINVAL on loaned direct writes** — see "16K runtime run and the loaned-DIO
  EINVAL finding" below.
- **Full 4K re-validation after the reboot onto the rebuilt kernel**
  (2026-09-05, `7.1.8-3.hs.159.el9.aarch64+` rebuilt from tree tip — brd fix
  + EINVAL fallback in; installed brd/nfsd/sunrpc srcversions verified
  against the tree's objects). All via the runner scripts in this directory:
  - KUnit: `sunrpc-xdr-bvec` 7/7, `sunrpc-svcsock-rx` 53/53,
    `nfsd-receive-bvec` 5/5, `nfsd4-receive-bvec` 9/9, zero fault
    signatures (`run-kunit.sh`).
  - `bvecrepro` matrix: **all four rows MATCH** on the patched brd —
    poison `off0=684`, legacy `off0=160`, aligned `off0=0` on `/dev/ram0`
    directly, and poison `off0=684` through nvme-loop (`run-bvecrepro.sh`).
  - The previously-missing **`cmp`-based runtime validation**
    (`run-rig-cmp.sh`): 30 fresh connections × 16 MiB `O_DIRECT`,
    **0 dd failures, 0 `cmp` mismatches**, 462 direct / 40 vector,
    0 LOCALIO hits, 1 007 112 492 bytes borrowed vs 2 268 copied. A fresh
    post-run write checked *before* any local read shows the write-path
    page-cache footprint flat (`fincore` 0 pages; the 16M the script
    itself reports is from its own local `cmp` reads, not the writes).
- **System-level functional & correctness suite** (2026-09-05, first run
  green): KUnit-decoupled production-stack coverage with model-file
  byte-verification — see "System-level functional & correctness
  coverage" below and `run-system-correctness.sh`.
- **Full 4K bisect walk of the entire branch** (2026-09-05): all 55 commits
  of `v7.1.8-3..kernel-7.1.8/main.NFSD_TCP_WRITE_ZEROCOPY` — the 30-commit
  series plus all 25 follow-ons through the re-validation commit — build
  **55/55 clean, zero failures** under `CONFIG_WERROR=y`. The walk re-applies
  the tip's `v7.1.8-3-aarch64-4k.config` through `olddefconfig` at every
  commit so the four KUnit test options stay `=m` from the moment their
  Kconfig symbols exist (a static `.config` would silently drop them to `n`
  mid-walk). Supersedes the earlier base+30 4K walk. **Re-verified after
  the 2026-09-05 branch restructuring** (MLDSA config + the five upstream
  fixes hoisted to the front, KUnit test commits regrouped in the
  loan-pages segment, project docs moved to the end): the full walk of
  the restructured branch is **64/64 clean, zero failures** — including
  the two conflict-resolved sunrpc Kconfig/Makefile intermediates the
  regrouping created. Future walks skip the doc-only commits at the end
  (`run-bisect-walk.sh` auto-detects them). Re-verified after every
  subsequent restructuring; **latest (post KUnit-boilerplate
  consolidation, with sparse active): 42/42 code commits build clean,
  zero failures, zero branch-introduced sparse findings.** That last
  clause was read off raw per-step counts, which cannot distinguish a new
  finding from a pre-existing one displaced by a commit inserting lines
  above it; the x86_64 walk below hit exactly that case. The conclusion is
  believed correct — every hit was blamed to base/upstream by hand — and
  the 2026-09-09 `main-5` walk (43/43, top of this file) subsequently ran
  under the adjudicator and settled it mechanically: zero
  branch-introduced findings against a cold whole-tree baseline.

- **Full x86_64 bisect walk of the entire branch** (2026-09-06): the first
  non-aarch64 execution of the walk, on `7.1.8-3.hs.297.el8.x86_64`
  (16-core, gcc 11.2.1, sparse v0.6.5-rc1) — **42/42 code commits build
  clean, 0 failures, zero branch-introduced sparse findings**, 36 doc-only
  commits auto-skipped, 60 min wall (mean 84 s/step; step 1's from-scratch
  build 979 s). This is the cross-architecture coverage that page-size
  permutations cannot give (arch-specific warning surface, type/format
  differences), and it found nothing new: the aarch64 result now has an
  independent confirmation on a different architecture and compiler.

  Two host conditions had to be corrected first, both of which would have
  produced a *silently* vacuous pass:

  - the booted x86_64 config had `# CONFIG_WERROR is not set`, so `rc==0`
    would not have meant "zero warnings". The walk config is that config
    with `CONFIG_WERROR=y` (which `olddefconfig` correctly propagates to
    `KVM_WERROR=y` and `LD_ORPHAN_WARN_LEVEL=error`); the runner now
    refuses to start without it.
  - el8 ships sparse 0.6.4, which fails kbuild's `__typeof_unqual__`
    validity probe and gets *silently disabled* — reporting `sparse=0`
    while checking nothing. Built git-master sparse (v0.6.5-rc1) and
    pre-flighted `checker-valid.sh`, as on aarch64.

  Config deviations from the booted kernel, both deliberate: BTF off
  (matching the aarch64 4K walk config — BTF is link-time and type-driven,
  so it has no bearing on warning or sparse coverage, and the tip's BTF is
  already qualified separately) and `LOCALVERSION="-walk"` so nothing can
  collide with the installed `hs.297`. The walk ran in a dedicated `git
  worktree`: kbuild refuses an `O=` build against a tree carrying an
  in-tree build, and `make mrproper` would have destroyed the artifacts of
  the installed kernel.

  **On the sparse result.** 15 of the 42 steps did emit sparse output; all
  of it was adjudicated to the step-1 whole-tree baseline (3786 normalized
  findings), leaving zero new. Taking the core commit "SUNRPC: loan
  eligible TCP receive pages" (`sparse=7`) as the worked example, all
  seven are pre-existing: the `__rcu` casts in `clnt.c`/`auth.c`, the
  `nfs4xdr.c` base-type assignment, plus `nfs4trace.h` "too long token
  expansion" and `bitsperlong.h` word-size errors that are sparse's own
  limits rather than code defects. They resurface only because those TUs
  get recompiled. Across all 42 steps the union of findings spans 21
  files, overwhelmingly KUnit/test TUs pulled in by the series'
  `lib/kunit` and `include/kunit` changes. The adjudicator was itself
  tested against an injected `net/sunrpc/svcsock.c` finding (caught) and
  against injected line-shifted and dot-prefixed pre-existing findings
  (correctly silent), so the zero is a verified zero and not an absence of
  looking.

- **Full x86_64 runtime & KUnit qualification** (2026-09-06), on
  `7.1.8-3.hs.297.el8.x86_64+` built in place from the tree and booted
  (all ten relevant installed modules srcversion-matched against the
  tree's objects before anything ran). The **first non-aarch64 execution
  of the KUnit suites** — until now they had run only on the 4K and 16K
  aarch64 kernels — and the first runtime validation on x86_64. Every
  runner in this directory, zero fault signatures in dmesg across the
  whole session:
  - KUnit (`run-kunit.sh`): `sunrpc-xdr-bvec` 7/7, `sunrpc-svcsock-rx`
    53/53, `nfsd-receive-bvec` 5/5, `nfsd4-receive-bvec` 9/9.
  - `bvecrepro` (`run-bvecrepro.sh`): all four documented rows MATCH.
    Extended beyond them this run: the nvme-loop `off0=160`/`off0=0`
    rows, and a **zram** set (`off0=684/160/512/0`) — all MATCH, which
    is the first empirical check of the zram fix at 4K, the page size
    where its `is_partial_io()` defect actually bit.
  - `run-rig-cmp.sh`: 30 fresh connections × 16 MiB `O_DIRECT`, **0 dd
    failures, 0 `cmp` mismatches**, 480 direct / 28 vector, 0 LOCALIO
    hits, 1 006 934 688 bytes borrowed vs 125 488 copied, and the
    pre-`cmp` `fincore` on the first file **flat (0 pages)**.
  - `run-loan-assert.sh`: after the bound fix below, 50 fresh
    connections — **800/800 1 MiB receives fully loaned**, 0 not loaned,
    18 locked-head receives, 785 direct / 46 vector, 40/50 connections
    fully direct, all data `cmp`-clean.
  - `run-system-correctness.sh`: **all cases pass** (`fails=0`), 2
    versions × 2 modes × the 9-case matrix incl. the 100-small-file
    churn, plus 2×10 fresh-connection anchor loops; 461 direct / 295
    vector, 0 LOCALIO.
  - `git diff --check` over all 80 commits of `v7.1.8-3..tip`: clean.

  Two findings, both in the harness rather than the kernel:

  - **`run-loan-assert.sh`'s locked-head byte ceiling was wrong.** Its
    `copied <= 4096` was an aarch64 observation, not the contract: a
    receive hits the locked-head case once per skb carrying a locked
    linear head, and `svc_tcp_rx_copy_segment_merged()` folds each
    occurrence into one whole page, so `copied` grows by up to
    `PAGE_SIZE` per event with no static ceiling. On x86_64 loopback it
    failed ~20% of connections on byte-correct data — up to **3 merge
    events in a single 1 MiB receive** (`copied=12288`). bpftrace
    settled it: **15 calls to `svc_tcp_rx_copy_segment_merged()` and 0
    to `svc_tcp_rx_copy_segment()`**, i.e. every copy took the merge
    path and none fell back to the three-way split. The assertion is now
    structural (bvec count, `materialized`, copy fraction) — see the
    loan-assertion section below.
  - **`run-bvecrepro.sh`'s nvme-loop row had never run.** It located the
    namespace with `nvme list | awk '/loop/'`, but nvme-cli 2.x prints
    Model `Linux` for an nvmet loop namespace with no "loop" in the row,
    so the row always reported itself skipped. It now finds the
    namespace through sysfs, as the other runners do; `K` also no longer
    hardcodes one checkout path.

  Two **host** conditions had to be corrected first, neither a kernel
  matter but both fatal to the rig on a fresh el8 host:

  - `/etc/nfs.conf` carried `[exports] rootdir=/root/snitm/git/HS`, so
    `exportfs` resolved `/export/zc` under that prefix and refused to
    export it. Overridden with an `/etc/nfs.conf.d/` drop-in
    (`rootdir=/`) rather than editing the host's file.
  - the same file had `vers4.2=n` and `vers4.0=n`, which the
    system-correctness suite's `vers=4.2` matrix needs; the drop-in
    enables 3, 4.0, 4.1 and 4.2.

  Not repeated this session: the bisect walk (done 2026-09-06, above)
  and with it the `W=1`/sparse gates, since re-running `W=1` in place
  would rebuild objects that currently match the installed kernel.

- **aarch64 4K re-validation of the updated harness** (2026-09-05, after
  the x86_64-driven runner fixes: structural loan-assert bound + the
  sysfs-based bvecrepro nvme-loop row). Booted `7.1.8-3.hs.159.el9.aarch64+`
  verified current (all commits since its build are doc/harness-only;
  brd/sunrpc/nfsd/nfs/zram srcversions match the tree's objects). Every
  runner, zero fault signatures across the whole session (journalctl-swept,
  since the runners' `dmesg -C` truncates the ring):
  - KUnit (`run-kunit.sh`): 7/7, 53/53, 5/5, 9/9.
  - `bvecrepro` (`run-bvecrepro.sh`): all four rows MATCH — and the
    nvme-loop row **demonstrably executed via the new sysfs discovery**
    on this host. Extended with the **zram set** (`off0=684/160/512/0`,
    all MATCH): first aarch64-4K empirical check of the zram fix,
    mirroring the x86_64 extension.
  - `run-rig-cmp.sh`: 30 × 16 MiB, **0 dd failures, 0 mismatches**,
    480 direct / 24 vector, 0 LOCALIO, pre-`cmp` `fincore` flat
    (0 pages), 1 006 925 984 borrowed vs 188 776 copied.
  - `run-loan-assert.sh` (CONNS=50): **800/800 1 MiB receives fully
    loaned**, fails=0, 40/50 connections fully direct — the structural
    bound holds on aarch64 with no false failures (locked-head 0–1 per
    connection here, vs up to 3 on x86_64).
  - `run-system-correctness.sh`: **all cases pass** (`fails=0`), both
    versions × both modes + 2×10 anchor loops; 427 direct / 329 vector,
    0 LOCALIO.
  Bisect/sparse walk deliberately not repeated (unchanged code since the
  adjudicated 2026-09-06 walks).

- **16K re-validation with the full harness** (2026-09-05, the 16K guest,
  `7.1.8-3.hs.161.el9.aarch64+`). The guest was brought current **in
  place, no reboot**: the delta between the installed build and the tree
  tip is module-only (the branch's only vmlinux-built files,
  `kernel/panic.c` and `lib/bug.c`, come from the two kunit-suppression
  commits already in the running kernel), so the tip was rebuilt with the
  production 16K config reusing buildnumber 161 (zero warnings),
  `modules_install`ed, and the stale modules reloaded — sunrpc (the
  locked-head merge), zram (the sub-page bvec fix), svcsock_kunit (the
  merged-contract lifetime test); brd/nfsd/nfs srcversions verified
  current against the tree's objects. Every runner, zero fault signatures
  (journalctl-swept; expected benign taints only: O for the out-of-tree
  bvecrepro, E because mrproper had destroyed the original module-signing
  key, N from KUnit):
  - KUnit (`run-kunit.sh`): 7/7, 53/53, 5/5, 9/9 — first 16K execution
    of the merged-contract svcsock lifetime test.
  - `bvecrepro` (`run-bvecrepro.sh`): all four rows MATCH (the nvme-loop
    row via the sysfs discovery); extended with the zram set
    (`off0=684/160/512/0`, all MATCH) — **first zram check at 16K page
    size**.
  - `run-rig-cmp.sh`: 30 × 16 MiB, **0 dd failures, 0 `cmp`
    mismatches**, 418 direct / 143 vector, 0 LOCALIO, pre-`cmp`
    `fincore` flat (0 pages), 1 006 433 136 borrowed vs 688 488 copied
    (higher copied than the 4K runs because each locked-head merge event
    costs one whole 16K page here; still < 0.07% of borrowed).
  - `run-loan-assert.sh` (CONNS=50): **800/800 1 MiB receives fully
    loaned**, fails=0, 20/50 connections fully direct; locked-head 0–1
    per connection, so the structural bound holds with margin (at 16K
    its `copied < body/16` clause admits at most 3 merged pages per
    1 MiB receive — worth remembering if a future host draws more).
  - `run-system-correctness.sh`: **all cases pass** (`fails=0`), both
    versions × both modes + 2×10 anchor loops; 391 direct / 344 vector,
    0 LOCALIO. First 16K execution of this suite.
  Bisect/sparse walk skipped by request (the 16K walk is dropped; the
  adjudicated 4K and x86_64 walks stand).

**Remaining (from existing coverage — not new tests):**

- KUnit suites on the **64K page-size kernel** — the 64K config builds, and
  the 16K run above now exercises the page-size-scaling paths at a non-4K
  `PAGE_SIZE` (16K stands in for 64K on the MacBook Pro testbeds: guests on
  Apple silicon, whose M-series cores implement only the 4K and 16K
  translation granules, so a 64K kernel's EFI stub refuses to boot there —
  kernel-rpmtree `JOURNAL.md`, 2026-09-04), but the suites have not executed
  at 64K itself. **64K testing depends on E1 hardware** (or any other host
  whose CPU implements the 64K granule, e.g. Ampere lab hosts) — both the
  KUnit suites and the runtime validation on the 64K kernel wait on that.
  16K config provenance: production base is kernel-rpmtree's derived
  `kernel-aarch64-16k-rhel.config` (the rhel flavor with
  `ARM64_16K_PAGES=y`; the redhat infra carries no 16k rhel flavor of its
  own, only fedora ones) — built and verified as `7.1.8-3.hs.161`.
  Qualification/KUnit config: `v7.1.8-3-aarch64-16k.config` (this
  directory), derived the same way from `v7.1.8-3-aarch64-4k.config`;
  used for the 16K KUnit builds (its bisect walk is dropped — next bullet).
- ~~Bisect walk on x86_64~~ **Done (2026-09-06)** — see the x86_64 walk
  entry under "Done" above. The 16K and 64K walk items are **dropped**
  (2026-09-05): page size on the same architecture and compiler varies
  only constants, so the completed full 4K walk plus the historical 64K
  base+30 walk already bracket the useful endpoints.
- ~~sparse~~ **Done (2026-09-05; adjudicated 2026-09-06):** sparse is
  folded into `run-bisect-walk.sh` (`make C=1` — every step checks exactly
  the TUs that commit recompiles; el8/el9's sparse 0.6.4 release fails
  kbuild's `__typeof_unqual__` validity probe and gets *silently
  disabled*, so the runner pre-flights `checker-valid.sh` and a git-master
  build, v0.6.5-rc1, is required). The runner no longer reports a raw
  per-step count but a verdict: step 1 builds from scratch and so
  sparse-checks the whole tree, and a finding is branch-introduced iff it
  appears at a later step and is not in that baseline, compared on
  (file, column, message) so that neither sparse's progress dots nor a
  line shift from an insertion above a pre-existing warning reads as new.
  Because that reasoning needs step 1 to have been cold, the runner probes
  for stale objects and reports UNSOUND rather than a confident zero if it
  was not. First full profile: **zero findings
  introduced by anything on this branch** — all hits are pre-existing
  base/upstream noise (the sunrpc `__rcu` casts, an nfs4xdr assignment
  from the base dir-events code, a zram params check upstream), blamed
  and verified.
- ~~The `DEBUG_INFO`/BTF rebuild~~ **Done (2026-09-05):** the tip built
  with `CONFIG_DEBUG_INFO_BTF=y` + `CONFIG_DEBUG_INFO_BTF_MODULES=y`
  (pahole v1.31, dwarves from el9), installed and **booted as
  `7.1.8-3.hs.159.el9.aarch64+`** — the fully faithful production
  configuration. BTF analysis on the live kernel (debug info stays in
  the tree: `/root/kernel/linux/vmlinux` + unstripped in-tree `.ko`;
  installed modules are stripped by design):
  - **Module BTF accepted end-to-end**: `/sys/kernel/btf/{vmlinux,
    sunrpc,nfsd,lockd,grace}` all present after module load, zero BTF
    mismatch/pahole complaints in dmesg, and `bpftool btf dump` parses
    the vmlinux and nfsd blobs completely.
  - **BTF ≡ DWARF** for every struct the series touches (`xdr_buf`,
    `svc_tcp_rx_state`, `svc_procedure`, `svc_rqst`, `svc_sock`),
    including the `pc_xdrressize:31`/`pc_xdr_bvec:1` bitfield split
    (both at bit offset 48:0/48:31, same word — `svc_procedure` stays
    64 bytes, one cacheline, zero growth).
  - **pahole layout review — the series introduces zero new holes**:
    `xdr_buf` 72→80 bytes with *no* holes (12 bytes of new members,
    4 absorbed by former tail padding); `svc_rqst` grows exactly the
    expected 24 bytes (2×8 embedded `xdr_buf`s + `rq_tcp_rx`) and its
    three 4-byte holes trail pre-existing base members (`rq_prot`,
    `rq_reserved`, `rq_err`) — `rq_tcp_rx` packs flush after
    `rq_bvec`; `svc_sock`'s lone 4-byte hole after `sk_datalen`
    predates the series (`sk_rx_sequence`/`sk_rx` add exactly 16
    bytes). New `svc_tcp_rx_state` is 128 bytes, `__aligned(64)`, its
    single 1-byte hole (before `merge_fill`) is subsumed by the 32
    bytes of alignment tail padding — repacking would not shrink it.
  - **BTF-based tooling ready**: the three `svc_tcp_rx_*` enums are in
    the sunrpc BTF and `svcsock_tcp_rx_lifetime` is registered in
    tracefs, so bpftrace/BPF probes can now be typed against the live
    kernel (the bisect configs keep BTF off; that only ever deviated
    from production on this axis).
  A per-commit walk under `DEBUG_INFO_BTF` is judged redundant: BTF
  encode is a link-time, type-driven pahole step, and the tip build
  covers the type superset of every intermediate commit.
- **Runtime validation** — **functional check done** on 4K (2026-09-04) and
  16K (2026-09-05, which also verified the flat page-cache footprint via
  `fincore`); see "Host-local rig" below. Not yet done: forcing the copy-path
  `reason` codes live (needs krb5 or `xprtsec=tls`). The **loaned-DIO EINVAL**
  is root-caused (2026-09-09: interior payload discontinuity, loan-only —
  see `NFSD-LOAN-PAGES-EINVAL-ROOT-CAUSE.md`) and **closed by the
  admission-time discontinuity gate** carried in-series since the
  `v7.1.8-5` rebase (the interim `nfsd_direct_write()` buffered fallback
  is gone), and the
  silent corruption found while validating it is **root-caused and fixed**
  (brd's per-segment `bi_sector` skew — see "The root cause" below); the
  4K `cmp`-based re-run with the patched brd is **done** (2026-09-05, see
  "Done" above); the audit of the other bio-based backing drivers is
  **complete** (see `BIO-DRIVER-AUDIT.md` — zram carried the same class
  and is fixed on main).
- **Performance comparison** (`io_cache_write` 0 vs 2, patched vs unpatched
  kernel) — belongs on tardis1-class hardware, not this VM.
- **Early-boot testing** — explicitly deferred for now.

## Fixes that must be sent upstream

**Resolved by the 2026-09-09 rebase onto `v7.1.8-5`** (branch
`kernel-7.1.8/main-5.NFSD_TCP_WRITE_ZEROCOPY`): the brd fix, the reworked
zram fix (with its two upstream zram prereqs), and the nfsd filecache
DIO-alignment fix are all **in the base tag** and no longer carried by
this branch. The DONTCACHE-clobber fix is folded into the base's
heuristic commit itself (2026-09-09 base respin), so the series no
longer carries it. The -EINVAL fallback is gone —
replaced by the in-series admission gate ("nfsd: require interior payload
discontinuities to be logical-block aligned"), ordered ahead of the loan
commits. The narrative below is the pre-rebase record.

Standalone fixes carried on this branch, independent of the page-loan
series. All were found by this project's qualification; none depend on
a series commit. **Four** of the five were posted 2026-09-08 as the
4-patch series drafted in `upstream-fixes/` (2026-09-06, unversioned
iteration dir) — **reduced to three 2026-09-09**: patch 4/4 (the -EINVAL
fallback) was withdrawn after Chuck Lever's review, see the table row and
`NFSD-LOAN-PAGES-EINVAL-ROOT-CAUSE.md`; **the zram patch was rebased for v7.3-rc2**
(2026-09-08): the original conflicted with upstream commits
`184bf187c45b` ("zram: switch to unsigned long indexing") and
`d1aba9859847` ("zram: drop unused bio parameter from write helpers"),
both zram-local. Branch
`kernel-7.1.8/main.NFSD_TCP_WRITE_ZEROCOPY-zram-fix` (off main) carries
revert-of-original → the two upstream prereqs (cherry-picked `-x` +
S-o-b; `184bf187c45b` adapted with a `[snitzer:]` note because this
tree lacks three other intermediate zram commits) → the re-landed fix
(`unsigned long index = pos >> PAGE_SHIFT` merges both changes).
Re-qualified on that branch: zram.ko builds warning-free with BTF, and
the `bvecrepro` zram set (`off0=684/160/512/0`) is **all MATCH** on the
live 4K host — re-run after the follow-up cleanup (Mike, 2026-09-08):
with `is_partial_io()` honest everywhere, `ZRAM_PARTIAL_IO` is
unconditionally defined, so the dead
`WARN_ON_ONCE(!IS_ENABLED(ZRAM_PARTIAL_IO))` guard in
`read_from_bdev()` and the define itself are dropped (mirrored on main
as a `fixup!` of the zram fix commit). The regenerated `0002` in
`upstream-fixes/` now `git apply --check`s clean on v7.3-rc2 (the
original demonstrably does not). The DONTCACHE-clobber fix
is **excluded
from the upstream posting** — verified against mainline
`06c5c97293e3`: the bug was introduced by the hch "NFSD_IO_DIRECT
heuristic for small IO" RFC (`eaa2a4f9efb7`, hs-carried,
Not-signed-off-by), so upstream does not have it. That fix is folded
into the base's heuristic commit since the 2026-09-09 respin and
travels with it if/when it goes up.

| commit | subject | upstream exposure |
|---|---|---|
| `1177569feea1` | brd: iterate the bio by byte position, not bi_sector | Silent corruption of any `ITER_BVEC` direct I/O with sub-sector segment lengths on brd-backed storage — stock `NFSD_IO_DIRECT`'s copied-arena geometry corrupts today. |
| `9d51eb25526f` | zram: handle sub-page bvec segments without corrupting data | Silent corruption on every 4K-page kernel: `is_partial_io()` hardwired false sends sub-page bvecs down the full-page fast path (ignores `bv_offset`/`bv_len`), plus the brd sector-cursor skew. |
| `21d6c8e1f43e` | nfsd: fetch direct I/O alignment for files handed to the filecache | The supplied-file acquire branch (NFSv4 OPEN+CREATE) leaves the nfsd_file's dio alignment fields zero, so every WRITE to a freshly created file refuses direct I/O for the file's cached lifetime. Trailers reformatted 2026-09-05 (Assisted-by + Signed-off-by, Fixes tag added). |
| `de7d14a5f406` | nfsd: don't clobber IOCB_DONTCACHE on the no-alignment write fallback | **Not upstream-exposed** (corrected 2026-09-06): the clobber comes from the hs-carried hch heuristic RFC `eaa2a4f9efb7`, not mainline — mainline's `no_dio` fallback never sets DONTCACHE at all. Dropped from the upstream posting. **Folded into the base's heuristic commit at the 2026-09-09 `v7.1.8-5` respin** — no longer carried in-series. |
| `d3276f647bdf` | nfsd: fall back to buffered I/O when a direct write gets -EINVAL | **Not upstream-exposed** (corrected 2026-09-09): the rejection needs a discontiguous payload, which upstream's copied rq_pages never produce — the failing geometry is loan-only. Patch 4/4 withdrawn on-list (Chuck NAK'd the -EINVAL retry; msgid `aqGL5dh49toktgBI@kernel.org`). Dropped from the branch in the 2026-09-09 `v7.1.8-5` rebase — replaced by the admission-time gate ("nfsd: require interior payload discontinuities to be logical-block aligned") ordered ahead of the loan commits, with KUnit coverage in `dio_segments_test`. See `NFSD-LOAN-PAGES-EINVAL-ROOT-CAUSE.md`. |

## The 2026-09-05 series restructuring

The branch was restructured in three steps, tree content byte-identical
throughout (every step verified with `git diff old-tip new-tip` empty):

1. **Zones.** The MLDSA config answer and the five standalone upstream
   fixes moved to the front of the branch; every internal
   `NFSD_TCP_WRITE_ZEROCOPY:` project-doc commit moved to the end. The
   series and its code follow-ons sit in the middle.
2. **Code-first, tests-after.** Within the page-loan feature half, the
   KUnit test commits regrouped after the feature code, followed by the
   dio_segments_test fixes and the standalone-KUnit-modules rework, with
   the CONFIG enablement commit closing the segment. One commit folded:
   "fix NFSDv4 KUnit test style" into "split NFSv4 receive bvec KUnit
   module" (no value split out).
3. **KUnit fully factored out of feature code (completed for all seven
   feature commits, including the NFSv4 COMPOUND accessors).** Three feature commits
   had carried KUnit material; each was split so the production half
   stands alone and the test half folded into the test commit it
   belongs with:
   - *accept immutable receive bvec requests* → its 225-line
     `nfsd_bvec_kunit.c` scaffolding + `NFSD_BVEC_KUNIT_TEST`
     Kconfig/Makefile wiring folded into *exhaustively test receive
     bvec consumers*, which now creates the test module (and carries
     the vfs.c/nfs3xdr.c test accessors — added by a test commit, not
     the feature).
   - *SUNRPC: loan eligible TCP receive pages* → its 355-line
     `svcsock_kunit.c` + wiring folded into *exhaustively test TCP
     receive page loans* (the whole 1539-line module now lands there).
   - *nfsd: accept receive bvecs for NFSv4 COMPOUND* → its 393-line
     NFSv4 test section folded into the nfsd4 module commit, which
     became a pure addition creating `nfsd4_bvec_kunit.c` outright
     (the v4 tests never pass through `nfsd_bvec_kunit.c` any more).

   Result: the six feature-code commits touch **zero** KUnit files,
   Kconfig test options, or Makefile test wiring — the production
   feature is complete and operational before any test code exists.
   This also dissolved the one ordering constraint the regrouping had
   hit (the COMPOUND commit's test cases needed the consumers-test
   helper signature); with the tests factored out, feature code has no
   dependency on test code in either direction.

4. **KUnit enablement boilerplate consolidated.** The visibility
   includes, `VISIBLE_IF_KUNIT` flips, `#if IS_ENABLED(CONFIG_KUNIT)`
   prototype blocks (svcsock.h, cache.h, nfsd.h, vfs.h, xdr4.h), the
   `nfsd3_procedure()`/`nfsd4_procedure()` test-only lookups,
   `nfsd4_kunit_savemem()`, and the compound decode/release exports all
   moved out of the feature commits into one Yamashita-authored commit
   ("SUNRPC/nfsd: expose receive-bvec internals for the KUnit suites")
   at the head of the KUnit block. The seven feature commits carry
   **zero KUnit references**: the production code stands alone and
   would function identically if every test commit were dropped.

The full 4K bisect walk was re-run clean after each step; after the final
KUnit factoring: **39/39 code commits build clean, zero failures** (26
doc-only commits auto-skipped).

---

## KUnit suites

Four suites cover the transport, the XDR layer, and both NFS consumers. They are
independently falsifiable — production correctness does not depend on them, and
production objects carry no test symbols.

| Suite | Covers | Qualified count |
|-------|--------|-----------------|
| `sunrpc-xdr-bvec` | authoritative-bvec XDR geometry validation and decode | 7/7 |
| `sunrpc-svcsock-rx` | the production `->read_sock` actor: classifier, skb walker, publisher, partial-state exchange, materializer, page-refcount unwind | 53/53 |
| `nfsd-receive-bvec` | NFSv3 WRITE consumer — differential matrix (page-array vs authoritative-bvec) over procedure selection, dup-cache checksum, DIO eligibility across aligned/misaligned/single/multi-page | 5/5 |
| `nfsd4-receive-bvec` | NFSv4 COMPOUND decoder + procedure loop + encoders + saved-memory owner + session-slot replay, over no-WRITE / WRITE-first/middle/last / two-WRITE / truncated / malformed compounds | 9/9 |

### Build a KUnit kernel

KUnit is a **separate configuration** from the production/bisect build (the
production config keeps `CONFIG_KUNIT=n`, because the series' `IS_ENABLED(CONFIG_
KUNIT)` accessors would otherwise compile into production objects).

The provided `v7.1.8-3-aarch64-{4k,16k,64k}.config` already enable the four test
options as modules (`NFSD_BVEC_KUNIT_TEST`, `NFSD4_BVEC_KUNIT_TEST`,
`SUNRPC_XDR_KUNIT_TEST`, `SUNRPC_SVCSOCK_KUNIT_TEST` = m, `KUNIT_ALL_TESTS=n`),
so a build from either produces the loadable test modules. (Note: because the
Hammerspace base builds `NFSD=m`, the NFSv3 suite is a standalone module and
needs `EXPORT_SYMBOL_IF_KUNIT(nfs3svc_decode_writeargs)` — carried in the series.
The series' own upstream qualification instead used `NFSD=y` with the NFSv3 test
built into `nfsd`.)

**All four suites build as standalone modules** (`xdr_kunit.ko`,
`svcsock_kunit.ko`, `nfsd_bvec_kunit.ko`, `nfsd4_bvec_kunit.ko`). The two SUNRPC
suites were originally linked *into* `sunrpc.ko`; main reworks them into
standalone modules (with `EXPORT_SYMBOL_IF_KUNIT` for the sunrpc internals they
exercise) so `sunrpc.ko` carries no `kunit` dependency. This matters for a
**shipped debug kernel**: with the in-module wiring, enabling the tests made
`sunrpc.ko`/`rpcrdma.ko` depend on `kunit.ko` (a `modules-internal` module),
which the distro module-to-package filter cannot place — the standalone-module
form keeps `sunrpc.ko` in its normal package while the four `*_kunit.ko` land in
`modules-internal` like every other KUnit module.

```sh
cp NFSD_TCP_WRITE_ZEROCOPY/v7.1.8-3-aarch64-4k.config .config
make ARCH=arm64 olddefconfig
make ARCH=arm64 -j"$(nproc)"
```

### Run the suites

Boot the kernel, load the modules, read KTAP from dmesg. `sunrpc-xdr-bvec` and
`sunrpc-svcsock-rx` run standalone. The two NFSD suites are independent
standalone modules — load each explicitly (`nfsd4_bvec_kunit` does **not**
pull in `nfsd_bvec_kunit`).

The **NFSv4 suite needs a running NFS server with NFSv4.0/4.1/4.2 enabled**
before the module loads — its session-replay case exchanges EXCHANGE_ID /
CREATE_SESSION / DESTROY_SESSION / DESTROY_CLIENTID through production paths, and
a live `nn->nfsd_serv` must exist:

```sh
# install/start a server (example; Fedora's nfs-utils starts nfsd with vers4.0=n)
dnf install -y nfs-utils
# enable minor versions 0/1/2, else the suite's v4.0 compounds get NFS4ERR_MINOR_VERS_MISMATCH
printf '[nfsd]\nvers4.0=y\nvers4.1=y\nvers4.2=y\n' > /etc/nfs.conf.d/pageloan-kunit.conf
systemctl start rpcbind nfs-server

modprobe nfsd_bvec_kunit           # nfsd-receive-bvec
modprobe nfsd4_bvec_kunit          # nfsd4-receive-bvec (needs the live server)
dmesg | grep -E 'KTAP|ok |not ok |# '   # read results

systemctl stop nfs-server rpcbind  # tear down afterward
```

Two gotchas hit during host qualification (2026-09-04):

- **`kunit` must be loaded with `enable=1`.** When a test module is loaded
  first, modprobe pulls `kunit` in with its default `enable=0` and every suite
  reports `kunit: disabled`; the parameter is not writable at runtime, and
  unloading the last test module lets the next autoload come back disabled
  again. Always `modprobe -r kunit; modprobe kunit enable=1` before a run.
  (Confusingly, `enable` looks nonexistent — it has 0 sysfs permissions, so
  it never appears under `/sys/module/kunit/parameters/` — but it is real.)
- **No nfs-utils needed for the NFSv4 suite** — the session-replay case only
  needs a live `nn->nfsd_serv`, which can be hand-started (v4-only, no
  rpcbind) via the nfsd filesystem:

  ```sh
  mount -t nfsd nfsd /proc/fs/nfsd
  echo "-2 -3 +4 +4.1 +4.2" > /proc/fs/nfsd/versions
  echo "tcp 2049" > /proc/fs/nfsd/portlist   # threads write fails with no listener
  echo 4 > /proc/fs/nfsd/threads
  # ... run the suite ...
  echo 0 > /proc/fs/nfsd/threads             # tear down
  ```

  The `nfsdcld`/client-recovery-tracking warning at start is harmless here.

### Expected results & the one known finding

Qualified result (author's guest run): `sunrpc-xdr-bvec` 7/7,
`sunrpc-svcsock-rx` 53/53, `nfsd-receive-bvec` 5/5, `nfsd4-receive-bvec` 9/9,
zero fault signatures.

Re-qualified on the Hammerspace v7.1.8 host (2026-09-04, 4K config) with the
same totals after three follow-on fixes (see `PROJECT.md` → "Follow-on
fixes"): the first host run **panicked** loading `nfsd_bvec_kunit` — the DIO
segments test passed a NULL `nf_file` into `nfsd_write_dio_iters_init()`,
which has dereferenced `nf_file->f_op->fop_flags` since the NFSD_IO_DIRECT
heuristic commit — and that panic had been masking a stale 3-way-split
expectation invalidated by the `nfsd_direct_misaligned_num_pages` gate. The
production DONTCACHE-clobber fix fell out of the same debugging. The DIO case
table now holds 9 geometries (was 6), still 5 tests / 5 passes.

Re-qualified again on a **16K page-size guest** (2026-09-04, `7.1.8-3.hs.161`,
Apple Virtualization guest on a MacBook Pro testbed): identical totals, zero
fault signatures — the first non-4K execution of the suites, covering the
page-size-scaled DIO split geometries at 16K.

Historical note (already fixed in the series): `nfsd4_q35_session_replay_test`
initially failed on the v7.1.8 base. Root cause is **base-specific, not a
receive-path or loan defect**: NFSD encodes the SEQUENCE result live on a replay
(RFC 8881 lets `highest_slotid`/`target_highest_slotid` track the session), and
v7.1.8 grows the session slot table unconditionally after the first request
(7.2 bounds growth by the nfsd thread ceiling), so a byte-compare of a replay
against the *first* reply only holds where the table cannot grow. The series'
last commit adapts the test to a replay-vs-replay comparison (the series' actual
property, independent of the base's slot-growth policy). The authoritative
decode path was never at fault.

## System-level functional & correctness coverage (KUnit-decoupled)

Work item added 2026-09-05: the KUnit suites are precise but tightly
coupled to their own infrastructure, and this project's history proves
why an independent layer matters — every pre-`cmp` qualification counted
tracepoints and missed real data corruption twice (the brd tail-drop, the
zram full-page clobber). This suite exercises the same coverage *areas*
through the production stack — real NFS traffic, real block devices —
verifying **correctness by byte-comparison against a local model file**
that receives the identical operation sequence (checked via NFS read-back
*and* directly on the export), across `vers={3,4.2}` x
`io_cache_write={0,2}`, LOCALIO off. Runner: `run-system-correctness.sh`
(this directory; brings up the brd/nvme-loop rig itself, leaves it up).

Derivation from the KUnit suites:

| KUnit suite / area | system-level analogue | KUnit-only residue |
|---|---|---|
| `sunrpc-xdr-bvec` — bvec geometry validation + decode | every write geometry lands byte-correct end-to-end: odd record sizes (999B, 3000B), odd offsets, sub-block RMW pokes, odd-increment appends | malformed/poisoned decode streams (a real client will not emit them) |
| `sunrpc-svcsock-rx` — classifier, skb walker, publisher, partial state, materializer, refcount unwind | fresh-connection anchor loops (TCP segmentation phase varies per connection — the geometry class that triggered the EINVAL and corruption findings), interleaved small-RPC churn (100 small files) between large WRITEs, large streams | exclusion-table reasons (tls/gss/reply — live forcing still needs krb5 or `xprtsec=tls`), refcount-unwind internals |
| `nfsd-receive-bvec` — v3 WRITE consumer, DIO eligibility matrix, dup-cache checksum | full matrix on a `vers=3` mount; DIO-vs-buffered decisions observed via `nfsd_write_direct`/`_vector` counts; mode 0 vs mode 2 byte-identical on identical inputs (direct path cross-checked against buffered) | dup-cache checksum internals (needs retransmit injection); the 9-geometry split table at page precision |
| `nfsd4-receive-bvec` — COMPOUND decode, procedure loop, session replay | full matrix on a `vers=4.2` mount (COMPOUNDs exercised naturally: OPEN/WRITE/COMMIT/CLOSE across the same geometries) | truncated/malformed compounds, session-slot replay byte-compare |

**First run green (2026-09-05, 4K host, patched brd):** all cases pass —
2 versions x 2 modes x the 9-case matrix (incl. 100-small-file churn) +
2x10 fresh-connection anchor loops; 425 direct / 327 vector writes
exercised, 0 LOCALIO hits, `fails=0`.

### 1 MiB loan assertion — `run-loan-assert.sh` (2026-09-05)

Focused NFSD-on-XFS check, factored out because no non-KUnit test asserted
it: **every 1 MiB WRITE receive must use loaned pages** — `action=publish`,
`mode=published`, `materialized=0`, and no copying except the transport's
*designed* `locked-head` fallback (bytes overlapping a locked skb linear
head are copied per segment; the paged frags still loan; the suite bounds
it at < 4 KiB). Per fresh connection: 16 x 1 MiB page-aligned `O_DIRECT`
writes, `cmp`-verified, per-receive assertions parsed from
`svcsock_tcp_rx_lifetime`, plus the requirement that at least one
connection completes fully `nfsd_write_direct`.

**First qualified run (4K host): PASS — all 160 large receives loaned.**
Two behaviors worth knowing, both benign for correctness (all data
`cmp`-clean):

- **`locked-head` is common on this rig:** 8/10 connections had exactly
  one receive (of 16) copy 53 bytes of locked skb linear head
  (`borrowed=1048695 copied=53 reason=locked-head`, 259 bvecs vs the
  usual 257). That IO loses its DIO path — 15 direct + 1 vector on
  those connections.

  **Pursued and rejected (2026-09-05): an NFSD-side head bounce.** A
  prototype added a `bounce_head` to `nfsd_write_dio_seg` — find a bvec
  boundary within the first page whose remainder passes the alignment
  gate, copy the head into a bounce page at issue time, keep
  `IOCB_DIRECT`. Instrumentation of the real geometry disproved it. The
  affected WRITE's payload iterator is (captured live):
  `bv[0]=(1636,2460)`, 127x `(0,4096)`, then `(0,1379) + (4039,53) +
  (1432,2664)` — the 53 locked bytes land **mid-payload** at an
  arbitrary copy-arena offset, splitting one page into three — then
  127x `(0,4096)` and tail `(0,1636)`. Two fatal properties: the break
  is mid-stream (no head cut reaches it), and the clean runs' page
  boundaries sit at `2460 + 4096k = 412 (mod 512)`, so when the block
  layer must split (259 bvecs exceed the queue's segment limit) **no
  512-aligned split boundary exists anywhere** — and no
  contiguous-prefix bounce can move interior page boundaries. The
  DONTCACHE fallback is the correct terminal behavior for this
  geometry at the NFSD layer.

  **The real fix lives in the transport's copy placement — implemented
  2026-09-05** ("SUNRPC: merge locked-head copies into a whole-page
  loan bvec", incremental on top of the series, not folded): a
  locked-head copy in MIXED mode retracts the trailing partial-page
  borrowed bytes, assembles them plus the locked bytes in a fresh page,
  and the following borrowed bytes top the page up
  (`state->merge_fill`) — one whole-page bvec replaces the three-way
  split. Qualified: `sunrpc-svcsock-rx` 53/53 (the mixed-geometry
  lifetime test adapted to the merged contract in its own commit),
  both NFSD suites green, system-correctness suite all green, and the
  loan assertion now shows a locked-head receive publishing **257
  clean bvecs with `copied=4096`** (the merged page) and its IO
  staying direct — **10/10 connections fully direct** in the first
  qualified run (vs 1–5/10 before; the anchor disturbance disappeared
  with the merge — it was downstream fallout of the same unmerged
  copies). The residual non-direct connections in later samples are
  the pre-existing mid-page-anchor class, where the loan holds and
  writes fall back buffered. `run-loan-assert.sh`'s locked-head bound
  was one page (`copied <= 4096`, the merged page) — **corrected
  2026-09-06** to a structural assertion, because a receive can hit the
  locked-head case once per skb with a locked linear head and each
  occurrence costs a whole merged page, so the byte ceiling has no basis
  (x86_64 saw 3 merge events in one 1 MiB receive). The suite now
  requires instead that the bvec count stay at the clean-loan count
  (body pages + 1 spill, so the three-way split is rejected outright),
  that nothing be materialized, and that `copied` stay a negligible
  fraction of the body.
- The TCP anchor phase still demotes whole connections' writes to the
  buffered path occasionally (loan intact), as documented under the 16K
  runtime findings — in this sample 1/10 connections was fully direct
  end-to-end.

Open refinements (both suites): force the *remaining* copy-path reasons
live — `auth` turns out to need no setup at all and is exercised by
ordinary traffic (the first RPC on every fresh connection is an
AUTH_NONE ping, classified `mode=copy reason=auth`; 32 distinct XIDs
over the 30-connection x86_64 run), leaving `tls` and gss, which still
need `xprtsec=tls` or krb5 (the lab host's tlshd configuration is stale,
so TLS is not currently reachable there); retransmit/packet-loss
injection for dup-cache coverage; run the suite on real hardware
alongside the tardis1 performance comparison.

## The loan kill-switch (`sunrpc.svc_tcp_rx_loan_pages`)

Added 2026-09-06 (UPSTREAM_DEFENSE.md checklist item 1): a writable
module parameter, default Y, read once per RPC record at classification
time. Clear it and every receive classifies `mode=copy reason=disabled`
— the pre-loan receive, byte-identical. Toggle at runtime:

```sh
echo N > /sys/module/sunrpc/parameters/svc_tcp_rx_loan_pages   # loans off
echo Y > /sys/module/sunrpc/parameters/svc_tcp_rx_loan_pages   # loans on (default)
```

Because the switch is consulted per record (not per connection), a flip
takes effect on the next RPC of an already-open connection; records
already classified complete under their original mode.

Test hooks: the svcsock KUnit suite forces the switch on for every test
(suite init/exit, host setting restored), and the exclusion table
carries a `disabled` case proving an otherwise fully eligible AUTH_SYS
call copies with the named reason when the switch is clear. When
validating the off state live, expect `borrowed=0` on every WRITE
receive, `nfsd_write_direct` still possible (the copied arena feeds the
same DIO gate as before the series), and data `cmp`-clean.

**Qualified (2026-09-06, 4K host, module-only in-place reload):** the
tip was rebuilt on the production config (same release string,
`7.1.8-3.hs.159.el9.aarch64+`), `modules_install`ed, and the whole
sunrpc stack reloaded live (nfs-server stopped, `rpc_pipefs` unmounted
— `nfsdcld`/`gssproxy` hold it — every dependent module removed,
new `sunrpc.ko` srcversion-verified). Results, zero fault signatures:

- `run-kunit.sh`: 7/7, 53/53 (incl. the new `disabled` exclusion
  case), 5/5, 9/9.
- `run-killswitch.sh` (new runner, this directory): **all three
  properties hold** — 16/16 1 MiB receives `reason=disabled` with
  `borrowed=0` and `cmp`-clean data while off; the mid-connection
  flip to Y re-loaned the very next writes on the same TCP connection
  (16/16 published); the svcsock suite runs green (`fail:0`) with the
  host switch off and restores the host's N afterward.
- `run-loan-assert.sh` (CONNS=10, switch on): fails=0, 160/160
  receives loaned, 8/10 fully direct — no regression from the
  classifier check.
- both commits `checkpatch --strict` clean; tip builds warning-free
  on the production config.

## Runtime validation — seeing the copy elimination

**`zcstat.sh` (this directory)** renders all of the below as a
vmstat-like periodic report — one row per interval: loaned vs copy-mode
receives (`pubrx`/`cprx`), bytes borrowed vs copied, fallback reasons,
**bvecs per ≥512K receive** (`avbv`/`mxbv` — ~257/MiB means page-tiled;
many hundreds means per-frame partial-page frags from a no-HDS NIC like
xeu), `nfsd_write_direct` vs `nfsd_write_vector`, the live
`io_cache_write` and loan-switch values, and the summed nfsd-thread
CPU%. Self-contained (tracefs + gawk, private tracing instance), so it
can be dropped onto any host running a page-loan kernel — e.g. tardis1
— and read live while flipping `svc_tcp_rx_loan_pages` for an
A/B at identical offered load:

```sh
./zcstat.sh 5            # 5s interval, until ^C
./zcstat.sh 5 60         # 5s interval, 60 rows
```

Column semantics, the kill-switch A/B recipe, the two things it cannot
tell you, and the LOCALIO trap are documented in
[`zcstat.sh-usage.md`](zcstat.sh-usage.md). Its tracepoint dependencies
are identical on the `-3` and `-5` branches (verified by diffing the
lifetime tracepoint block and both enums), so the same script serves
both kernels.

### Receive-pass amplification — `run-rxpasses.sh` (2026-09-10)

A widened look at 1 MiB writes turned up one structural effect worth
chasing on real hardware. Measured on the 4K loopback rig, 1 GiB of
1 MiB `O_DIRECT` writes per cell, `perf stat` plus kprobes, tracing off
during the counted runs:

| | cs/MiB | nfsd wakeups/MiB | passes/record | us/pass | time in recvfrom |
|---|---|---|---|---|---|
| loans ON | ~40 | 8.9–13.3 | **9.5** | 6.8 | 1237 ms/GiB |
| loans OFF | ~34 | 7.5–10.8 | **4.9** | 16.2 | 1587 ms/GiB |

The loan is doing its job: each `svc_tcp_recvfrom()` pass is ~2.4x
cheaper and total time in the receive path falls ~22% *even on loopback,
where the copy is cache-hot*. The cost is that it needs **1.94x as many
passes per record**, and ~25% more nfsd wakeups per MiB.

Root cause, measured rather than assumed. The first hypothesis —
loopback send-buffer backpressure from the server holding refs on the
client's skb pages — is **wrong**: `svc_data_ready` fires identically
with loans on and off (17 411 vs 17 406 per GiB) and so does
`svc_tcp_recv_actor` (~17.4k), so neither the network behaviour nor the
skb count changes. `desc.count` is identical too, so it is not a budget
limit. What changes is that a loan pass finishes so fast that the socket
queue drains, the thread sleeps, and `sk_data_ready` wakes it again:
the extra context switches are a *consequence of the loan working*, not
of it being slow.

`NFSD_IO_DIRECT` itself is clean on this axis — at equal loan setting,
direct and buffered are within noise on context switches (40.2 vs 41.0;
34.2 vs 35.3) and direct does slightly *fewer* workqueue queueings
(10.1 vs 11.1 per MiB), consistent with skipping writeback.

**What is loopback-specific and what transfers.** The net-CPU verdict
does not transfer: a loopback copy is L2/L3-resident and nearly free, so
the ~350 ms/GiB saved in receive does not cover the added scheduling,
and this rig cannot evaluate loan *performance* at all — only
correctness. The amplification itself should transfer, and may well look
different: at MTU 1500 a 1 MiB record arrives as hundreds of skbs rather
than ~17 here, so the pass arithmetic needs measuring before anyone
writes an optimisation.

[`run-rxpasses.sh`](run-rxpasses.sh) factors this out for tardis1. It is
purely observational — it generates no load and changes no knob unless
asked — and derives everything from what the server sees, so it needs no
knowledge of the client's workload: passes are calls to
`svc_tcp_recvfrom()`, records are the calls that returned > 0 (the
return value *is* the record length, which also validates the 1 MiB
assumption), so passes/record needs no bookkeeping.

```sh
./run-rxpasses.sh 30          # observe the current loan setting
./run-rxpasses.sh 30 --ab     # loans Y, then N, then restore
```

`--ab` prints a comparison table and checks that offered load was
comparable across the two windows, refusing to let absolute rates be
compared when it was not — a trap the first test run here fell into
(the load generator finished early and made loans look 2.5x faster).
The candidate fix, if the amplification proves material: after
`tcp_read_sock()` returns with an incomplete record, look at the socket
once more before releasing the thread, turning a wakeup cycle into a
cheap re-poll. Unwritten and unmeasured on real hardware -- deliberately
so.

The effect is visible in accounting, not guesswork. On a server built with the
series:

1. Enable NFSD direct write: `echo 2 > /sys/kernel/debug/nfsd/io_cache_write`.
2. Mount from a client with **TCP, `sec=sys`, and no `xprtsec`** (loans happen
   only for plaintext TCP, AUTH_SYS/AUTH_NULL, non-TLS).
3. Issue **large, page-aligned** writes.

What to observe:

- The transport reports **loaned vs copied bytes per request**, with a named
  fallback reason for every copy (TLS session, RPC reply, unsupported auth
  flavor, procedure not opted in, zero-copy/unreadable socket fragments) — so a
  request that fell back tells you why.
- The `nfsd_write_direct` and `nfsd_write_vector` tracepoints show which
  segments went to direct I/O vs the buffered vector path.
- A **page-cache footprint that stays flat under load** on the export is the
  signature of the direct path; counting copy-helper (`memmove`) invocations in
  NFSD context quantifies the receive-copy elimination.

To isolate the two mechanisms, compare the same kernel with `io_cache_write` at
`0` and `2`, and against an unpatched kernel: mode 0 vs 2 isolates the direct
write, patched vs unpatched isolates the receive loan. The series itself makes
no throughput claim; throughput numbers belong to the qualification that follows
(1 client, nconnect, 1 MB wsize, NVMe/XFS export — see `PROJECT.md` for the
measured baseline the work targets).

### Host-local rig (nvme-loop over brd) and the LOCALIO trap

For a functional check without a second machine, a RAM-backed NVMe export works
and keeps the measurement off spinning storage:

```sh
modprobe brd rd_nr=1 rd_size=4194304            # 4 GiB /dev/ram0
modprobe nvmet nvme-loop
CFG=/sys/kernel/config/nvmet
mkdir -p $CFG/subsystems/zc/namespaces/1 $CFG/ports/1
echo 1 > $CFG/subsystems/zc/attr_allow_any_host
echo -n /dev/ram0 > $CFG/subsystems/zc/namespaces/1/device_path
echo 1 > $CFG/subsystems/zc/namespaces/1/enable
echo -n loop > $CFG/ports/1/addr_trtype
ln -s $CFG/subsystems/zc $CFG/ports/1/subsystems/zc
nvme connect -t loop -n zc                      # -> /dev/nvmeXn1 (512 logical / 4096 phys)
mkfs.xfs -f /dev/nvmeXn1; mount /dev/nvmeXn1 /export/zc
exportfs -o rw,no_root_squash,insecure,fsid=77 '*:/export/zc'
echo 2 > /sys/kernel/debug/nfsd/io_cache_write
```

**The trap: LOCALIO.** A same-host client mount negotiates LOCALIO and issues
I/O through `nfs_local_open_fh` — the SUNRPC TCP receive, and therefore the
whole page-loan path, is **never entered**. A loopback run with LOCALIO on
measures nothing about this series. Disable it on the client before mounting
(it is a **global** `nfs.ko` knob — restore it afterward):

```sh
echo N > /sys/module/nfs/parameters/localio_enabled     # default Y
mount -t nfs -o vers=4.2,proto=tcp,sec=sys 127.0.0.1:/export/zc /mnt/zc
# ... run writes + traces ...
echo Y > /sys/module/nfs/parameters/localio_enabled     # restore when done
```

Verify with tracepoints: `nfs_local_open_fh` must be **absent** and
`nfsd_write_direct` **present**. The loan accounting rides the
`sunrpc:svcsock_tcp_rx_lifetime` tracepoint — `borrowed=` vs `copied=` bytes
plus the named `reason=`.

**Observed (2026-09-04, this host, brd-backed nvme-loop, 4K kernel):** with
LOCALIO disabled, 16 MiB of page-aligned `O_DIRECT` writes produced
**16 780 596 bytes borrowed, 0 copied** across the WRITE receives (mode
`published`, `reason=none`), and all 16 large WRITEs took `nfsd_write_direct`
(0 `nfsd_write_vector`). An odd-blocksize buffered write still borrowed the full
receive (the client coalesces into aligned WRITE RPCs) and split 2 direct + 1
vector on the sub-page tail — confirming the loan is decided at receive and DIO
eligibility per-segment afterward. The copy-path `reason` codes
(tls/auth/gss/reply/unreadable) are exercised by `svcsock_rx_exclusion_table_test`,
not forced here (would need krb5 or `xprtsec=tls`) — except `auth`,
which the 2026-09-06 x86_64 run showed fires unprompted on every fresh
connection's AUTH_NONE ping.

### 16K runtime run and the loaned-DIO EINVAL finding (2026-09-05)

Same rig on the 16K guest (`7.1.8-3.hs.161`, brd 2 GiB, nvme-loop, XFS
4K blocks, `io_cache_write=2`, LOCALIO off, `vers=4.2,proto=tcp,sec=sys`,
1 MiB wsize). The loan itself is flawless: across every run, all WRITE
receives published borrowed (`reason=none`), e.g. 16 784 683 bytes borrowed /
65 copied for a 16 MiB burst. Each 1 MiB WRITE arrives as **65 bvecs**
(64×16K pages plus spill), and the first fragment's in-page offset is set by
where TCP segmentation anchored the receive pages — an offset that is stable
within a connection burst but varies across connections. That anchor decides
the write path, all-or-nothing per burst:

- **anchor page-aligned** → 64-bvec iterators → 16/16 `nfsd_write_direct`,
  matching the 4K observation;
- **anchor misaligned mod 4** → `nfsd_dio_iter_is_aligned()` fails against
  XFS `dio_mem_align=4` → all writes fall back to the IOCB_DONTCACHE
  buffered path. Benign: `fincore` shows 0 pages cached afterward.
- **anchor mod-4-clean but mid-page** (e.g. `bv0=(408,15976)`) → nfsd's gate
  passes, `nfsd_write_direct` issues the DIO — and the **block layer rejects
  the bio**: `BLK_STS_INVAL` out of `bio_split_rw` under
  `nvme_ns_head_submit_bio` (the per-bvec `bv_offset`/`bv_len` vs
  `dma_alignment` test, or the no-valid-block-aligned-split case, in
  `bio_split_io_at()`), `dio->error=-EINVAL` flows through
  `xfs_dio_write_end_io` → `nfsd_write_err status=-22` → the client WRITE
  fails with **NFS4ERR_INVAL** (`dd: error writing …: Invalid argument`).
  Reproduces within a handful of fresh mounts (~half of connections in this
  rig). Diagnosis chain captured with ftrace + kretprobes: no bio completes
  in error except the split-time rejection; `__iomap_dio_rw`, `iomap_iter`,
  and `bio_iov_iter_get_pages` all clean.
  **[Attribution corrected 2026-09-09]** — this run captured *that*
  `bio_split_io_at()` rejected, not *why*; the anchor/first-bvec framing
  above is wrong. The rejection is always the ALIGN_DOWN-to-zero branch,
  and the trigger is an **interior discontinuity** in the loaned payload
  (a mid-page run boundary at a non-lbs-aligned payload byte), not bv0.
  See "The loaned-DIO EINVAL root cause (2026-09-09)" below and
  [`NFSD-LOAN-PAGES-EINVAL-ROOT-CAUSE.md`](NFSD-LOAN-PAGES-EINVAL-ROOT-CAUSE.md).

Two conclusions — **both revised 2026-09-09**. First conclusion as
originally recorded: "nfsd's DIO admission gate is weaker than the block
stack's", worked around on main ("nfsd: fall back to buffered I/O when a
direct write gets -EINVAL"): `nfsd_direct_write()` restores the segment
iterator and retries the segment (DONTCACHE-)buffered; 30 fresh
connections × 16 MiB then complete with zero client-visible errors
(391 direct / 152 vector, fallback observed on 20 connections). *Revised*:
the gate is fine for every payload upstream TCP can produce (contiguous
runs always split validly); it is only **loaned multi-run payloads** that
need a stronger gate, and the -EINVAL fallback is a stopgap to be replaced
by an admission-time discontinuity check (Chuck Lever NAK'd inferring
alignment from -EINVAL; upstream patch 4/4 withdrawn 2026-09-09). Second
conclusion stands with sharper wording: not a page-size bug —
the 4K host reproduces it too once the loan is enabled (12 rejections in
30 connections, 2026-09-09) — and "all-or-nothing per burst" was
approximate (4 of 16 writes failed per affected connection on 4K). The
nvme(-loop) path *is* tolerant of per-fragment-offset **contiguous**
bvec runs (~960 clean mid-bvec splits, cmp-verified, with brd fixed);
what it cannot handle is a **discontinuity at a non-lbs-aligned payload
position**.

### Root-caused & fixed — silent corruption of loaned direct writes (was: "split" corruption)

Validating the EINVAL fix with data comparison (`cmp` source vs export —
which **no earlier qualification had done**; prior runs only counted
tracepoints) exposed a second, far worse sibling: **when the block layer
splits a loaned direct-write bio, the write completes "successfully" but the
on-disk data is wrong.** Reproduced with the unpatched module too — this
predates and is independent of the EINVAL fallback.

The signature is a clean prefix then a forward shift, e.g. a 1 MiB WRITE
arriving as 65 bvecs with `bv0=(684,15700)`: bytes 0–15359 land correctly,
then the rest of the file continues with source data **+340 bytes ahead**.
The arithmetic identifies the mechanism exactly: 15360 = ALIGN_DOWN(15700,
512) — `bio_split_io_at()` rounded the split down to a logical-block
multiple *inside* bv0 (per its own comment, "Individual bvecs might not be
logical block aligned. Round down…"), and the continuation then **lost
bv0's 340-byte tail** — the remainder resumed at bv1 instead of at
`bvec_done=15360` inside bv0. Every corrupted write in every run fits
`onset = ALIGN_DOWN(bv0.len, 512)`, `shift = bv0.len − onset`. Writes with
identical 65-bvec geometry that are *not* split complete correctly, as do
all buffered/vector and DONTCACHE paths (`io_cache_write=0` matches
byte-for-byte).

Implications, in order of severity:

- **`io_cache_write=2` must not ship for loaned receives until this is
  resolved** — the corruption is silent (the WRITE succeeds, the client
  sees no error).
- The **legacy copied-arena direct path has the same bvec shape**
  (`bv0=(~160, PAGE−160)` then full pages, lengths not 512-multiples), so
  upstream `NFSD_IO_DIRECT` may be exposed to the same split-time tail-drop
  wherever the block stack decides to split — the tardis1 qualification
  measured throughput, not data integrity. Verify.
- The tail-drop itself is plausibly an **upstream block/nvme(-loop) bug**
  in the handling of a split remainder for pass-as-is `ITER_BVEC` bios
  (`bio_iov_bvec_set()` bios with `bi_bvec_done` mid-bvec) — user-space
  O_DIRECT never produces this shape, which would explain why nobody hit
  it. Root-cause with a dedicated instrumented run (dump the remainder
  request's first sg/bvec after a forced split).
- Candidate NFSD-side defense: require **per-bvec offset and length
  alignment to the logical block** for direct-eligible iterators. That
  admits the page-anchored loans (which are split-safe and were the 16/16
  green cases) and rejects the mid-page anchors — but it would also demote
  the legacy arena's `bv0=(~160,…)` geometry to buffered, i.e. gut DIO for
  the copied path, so the choice between gate, block-layer fix, and
  first/last-fragment bounce belongs to series review.

Also learned from the brd-direct control experiment: exporting XFS **directly
on brd** cannot exercise the direct path at all — brd's `dma_alignment` is
511, so `statx dio_mem_align=512` disqualifies every loaned iterator and all
writes go vector (data-correct). Only the nvme-loop stack (dma_alignment=3)
reaches DIO with loaned geometry.

### The root cause (2026-09-05, dedicated session): brd, not the split

A synthetic-bio reproducer (`bvecrepro.c`, this directory — builds
out-of-tree with `obj-m := bvecrepro.o`) took NFS, XFS, and iomap entirely
out of the loop: it submits a 1 MiB write bio whose bvec table is an
external array in the exact `bio_iov_bvec_set()` shape
(`bv0=(off0, 16K−off0)`, 63 full pages, `(0, off0)`), reads the range back,
and compares against a stamped pattern. Result matrix on the unpatched
kernel:

| geometry | target | result |
|---|---|---|
| poison `off0=684` | `/dev/ram0` **directly** | **CORRUPT** onset=15360 shift=+340 — and **no split occurred at all** |
| poison `off0=684` | `/dev/nvme0n1` (loop) | CORRUPT, identical signature |
| legacy `off0=160` | `/dev/ram0` directly | **CORRUPT** onset=15872 shift=+352 |
| aligned `off0=0` | `/dev/nvme0n1` | MATCH |

So the split was a **red herring**, and `blk_rq_map_sg`, nvmet, and
nvme-loop are all innocent (the nvme case corrupts only because brd backs
it). The real defect: `bio_advance_iter_single()` advances
`iter->bi_sector += bytes >> 9` (include/linux/bio.h), so when
`brd_rw_bvec()` consumes a bvec whose length is not a sector multiple, the
sector cursor silently loses the sub-sector residue while the data cursor
consumes the full length — every subsequent byte lands
`bv0.len − ALIGN_DOWN(bv0.len, 512)` short of its true position. Any
per-segment bio walker that re-derives its device position from
`bi_iter.bi_sector` has this exposure; request-based drivers (real NVMe)
are unaffected because nothing in the request path does per-bvec sector
arithmetic.

**Fixed on main** ("brd: iterate the bio by byte position, not
bi_sector"): the submit loop owns a byte-offset cursor advanced by each
segment's actual consumption. With the patched brd, all four reproducer
rows read back byte-identical, and the full NFS rig (patched brd + the
EINVAL fallback) survives **20 fresh connections × 16 MiB O_DIRECT with
zero dd failures and zero `cmp` mismatches** (275 direct / 82 vector).

Follow-ups this leaves open:

- **Send the brd fix upstream** — the legacy-geometry row shows stock
  `NFSD_IO_DIRECT` (pre-loan) corrupts on any brd-backed export today,
  independent of this series.
- **Audit other bio-based drivers** for the same `bi_sector`-derived
  per-segment position pattern before deploying loaned DIO over such
  stacks — tracked in [`BIO-DRIVER-AUDIT.md`](BIO-DRIVER-AUDIT.md)
  (2026-09-05: null_blk and the **full DM sweep** are done — dm core and
  most targets not exposed, but **dm-io/do_region, dm-log-writes,
  dm-writecache-pmem, and dm-integrity at its default block size are
  EXPOSED**, with dm-ebs exposed-in-source and parked; every dm queue's
  `dma_alignment >= 511` keeps NFSD's DIO gate from reaching any of
  them; **MD is not exposed** — md_submit_bio() runs every bio through
  bio_split_to_limits(), rejecting sub-sector bvec geometry with EINVAL
  before any personality sees it, and raid0/1/10/5 all confirm
  empirically; **zram was EXPOSED with confirmed silent corruption and
  is now fixed on main** — two defects: `is_partial_io()` hardwired
  false on 4K pages sent sub-page bvecs down the full-page fast path,
  plus the brd sector-cursor skew — the audit sweep is now
  **complete**, see the audit file); and raise the systemic
  question upstream (should iomap validate/bounce `ITER_BVEC` iterators
  with sub-sector bvec boundaries?).
- **Re-run the 4K host's runtime validation with `cmp`** — its 2026-09-04
  functional check counted tracepoints only, over an unpatched brd, so its
  written data was presumably corrupt too.

### The loaned-DIO EINVAL root cause (2026-09-09): interior discontinuities, not the anchor

Prompted by Chuck Lever's review of upstream patch 4/4 (which -EINVAL in
`bio_split_io_at()` fired, and offset_align vs lbs), an instrumented rerun
on the 4K host (`7.1.8-3.hs.159`, loaded nfsd/brd srcversions matching the
tree objects) with a kprobe/kretprobe pair on `bio_split_io_at()`
(`split_einval.bt`, this directory; raw capture `split_einval.log`)
answered both and overturned the 2026-09-05 mechanism story. Full
write-up: [`NFSD-LOAN-PAGES-EINVAL-ROOT-CAUSE.md`](NFSD-LOAN-PAGES-EINVAL-ROOT-CAUSE.md).
The short form:

- **Which -EINVAL**: the ALIGN_DOWN-to-zero branch, all 12 captured
  rejections. The per-bvec `dma_alignment` test provably cannot fire on a
  gate-admitted iterator (mask 3, page tiling keeps everything mod-4).
- **Mechanism**: a loaned payload with an **interior discontinuity** — a
  bvec starting mid-page (nvme virt-boundary gap) at a payload byte that
  is not an lbs multiple. The first split rounds down mid-bvec and
  succeeds; the remainder bio then holds a sub-sector pre-gap residue
  (e.g. 104 bytes at `bi_idx=63, bi_bvec_done=3992`) and
  `ALIGN_DOWN(104, 512) = 0` → `-EINVAL`.
- **The first bvec is innocent**: copied (loan-off) payloads with the
  exact "suspect" shape — mid-page start, non-sector-multiple first
  fragment — did ~960 mid-bvec splits with zero rejections, cmp-clean.
  Clean A/B on one host+kernel via `svc_tcp_rx_loan_pages`: N → 0
  rejections (480/480 direct), Y → 12 (480 writes = 465 direct + 15
  gate-rejected + 12 direct→fallback pairs; fallback absorbed all).
- **Upstream is not exposed**: copied rq_pages payloads are always one
  contiguous page-tiled run, and contiguous runs always have a valid
  split point. Patch 4/4 withdrawn on-list
  (`aqGL5dh49toktgBI@kernel.org`); patches 1–3 stand.
- **Open**: the loan series needs an admission-time gate in
  `nfsd_write_dio_iters_init()` — walk the payload bvecs and require
  every interior discontinuity to land on an `offset_align` multiple,
  else `no_dio`. Once it lands, revert the -EINVAL fallback
  (`d3276f647bdf`): Chuck's ambiguous-EINVAL objection applies here too,
  and the gate subsumes it. `stx_dio_offset_align=512 ==
  logical_block_size` on the rig (XFS reports the bdev lbs).

## Static / build QA gates

The series' qualification held the exact head to:

- **`W=1`**, **sparse**, **strict `checkpatch`**, and **`git diff --check`** clean;
- every commit builds (bisect-clean);
- production objects carry **no test symbols** (the KUnit accessors are
  `IS_ENABLED(CONFIG_KUNIT)`-gated);
- the receive loan state stays within **two cachelines**.

This tree adds: every code commit of the branch builds with **zero warnings /
zero errors under `CONFIG_WERROR=y`** on the 4K config with the KUnit modules
built, re-verified after each restructuring (`README.md`); the original
series was additionally verified on the 64K config.

## Qualification configuration (reference)

The series' upstream qualification used a config derived from a Fedora Rawhide
AArch64 host's stock `/boot/config-$(uname -r)` via `make olddefconfig` with
`CONFIG_LOCALVERSION="-p718"`, debug info + BTF off, and `CONFIG_KUNIT=n` for the
production kernel (a separate KUnit-enabled build for the suites). This repo's
`v7.1.8-3-aarch64-{4k,16k,64k}.config` play the equivalent role for the
Hammerspace `v7.1.8-3` base, with `CONFIG_WERROR=y` added as the bisect gate and
the four KUnit test modules enabled. (The 16k one is the 4k config with
`ARM64_16K_PAGES=y` resolved by `olddefconfig` — added 2026-09-04, after the
4K/64K verification runs.)
