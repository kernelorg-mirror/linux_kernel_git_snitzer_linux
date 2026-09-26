# NFSD TCP write-path zero-copy (page-loan series)

Server-side elimination of the redundant receive-side data copy on the NFS
write path. This directory collects the context, build/test configs, and
reproduction instructions for the "page-loan" patch series.

> **Corruption fix + handoff (2026-09-25, later):** David Flynn's regression
> testing found silent WRITE-payload corruption with loans on (clean with
> `sunrpc.svc_tcp_rx_loan_pages=N`): two locked skb heads back to back — TCP
> receive-queue collapse — send the second into the `rq_pages` arena while
> `merge_fill` is still set, and the next borrowed bytes are copied into the
> arena page as if it were the merge page. It was broken by the locked-head
> merge and first fixed incrementally (`1e48b4d03cb8`, `Fixes: f2ecc83eea82`,
> so the defect stayed visible); on 2026-09-26 the fix was folded into
> `dd2e42c5f547` ("SUNRPC: merge locked-head copies into a whole-page loan bvec",
> one assignment in `svc_tcp_rx_append_arena()`), with the KUnit regression
> case in `1fe6949a3f28`. Verified on 7.1.13-14.hs.439.loanpages by KUnit (the case fails
> without the fix) and the runtime harness (`TESTING.md` → "Results"); the
> collapse geometry itself could not be forced on loopback, so an end-to-end
> reproduction on a real NIC is still outstanding.
>
> **Branch state (2026-09-25):** `kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY`
> is rebased onto **`v7.1.13-14`** (= `kernel-7.1.13/main`); the `-12`
> version is kept as `….v7.1.13-12`. It is now **35 commits**: David Flynn's
> LOCALIO post-op-attrs fix shipped in the tag (`2894b8952dcf`) and dropped
> out, and a standalone fix leads the branch instead —
> "nfs_common: fix the skip accounting in nfs_dio_iter_aligned()"
> (`Fixes: 25ba2b84c38f`, the LOCALIO commit the helper came from, in
> mainline since v6.18): after a prefix skip the walk subtracted the first
> bio_vec's whole `bv_len`, so it could stop before the last fragment. It is
> latent — every vector nfsd and LOCALIO build today is page-tiled after its
> first entry, so the missed fragment always starts at offset 0 — which is
> why it carries no `Cc: stable`; the same slip is in mainline's
> `nfs_iov_iter_aligned_bvec()` in `fs/nfs/localio.c`. `-14` moved
> the NFSD DIRECT write split into the shared `fs/nfs_common/nfs_dio.c`
> (`nfs_dio_split()`, also used by NFS LOCALIO; `../NFS_LOCALIO_DONTCACHE/`),
> which reshapes three commits:
> "nfsd: accept immutable receive bvec requests" now adds the per-fragment
> covered-length check to that split's `nfs_dio_iter_aligned()`, completing
> the `iov_iter_alignment()` test (it checked each fragment's offset and only
> the total length, enough for contiguous vectors, not for loaned pages); the
> discontinuity gate is folded into the same walk as
> `nfs_dio_iter_aligned_and_splittable()` and so also guards LOCALIO (whose
> pinned user buffers are page-tiled and unaffected); and `dio_segments_test`
> drives `nfs_dio_split()` directly with nfsd's default policy, so nothing in
> `fs/nfsd/vfs.c` is exposed for KUnit any more. The 2026-09-24 note below
> describes the `-12` version. Tip and every hand-resolved commit build
> `CONFIG_WERROR=y`-clean (`fs/nfs_common/`, `fs/nfsd/`, `fs/nfs/`,
> `net/sunrpc/`, `lib/kunit/`); not yet run.
>
> **Branch state (2026-09-24):** the same 35-commit series is also on
> **`kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY`**, based on the public
> `kernel-7.1.13/main` (`v7.1.13-12`, no Hammerspace DP stack). Its base
> adaptations differ from the hs branch's: `main` carries the newer
> `NFSD_DONTCACHE_FALLBACK` write path (single `NFSD_WRITE_DIO_MEM_MISALIGNED`
> disposition, boundary-page claim, `boundary`/`edges` segment marks, no
> head/tail page indices), so the immutable-bvec and gate commits demote into
> that path and `dio_segments_test` checks the segment marks and pins
> `nfsd_direct_misaligned_dontcache`; the LOCALIO fix joins the conditional
> `i_lock` of the i_lock-contention series in `nfs_direct_write_completion()`;
> and `svc_setup_socket()` keeps the plain `if (err < 0)` error path (no
> `SVC_SOCK_RPCBIND_NOERR` without the HS stack). The tip builds
> `CONFIG_WERROR=y`-clean; runtime/KUnit qualification is pending as on the
> hs branch.
>
> **Branch state (2026-09-18):** the series now lives on
> **`kernel-7.1/hs-7.1.13-5.NFSD_TCP_WRITE_ZEROCOPY`**, rebased onto
> `kernel-7.1/hs-7.1.13-5` (tag `v7.1.13-5` plus the 29-commit Hammerspace
> DP stack, so the branch is its own integration branch). The 44-commit
> `main-5` history was regrouped into **35 commits in contiguous sections**:
> LOCALIO fix (1) → upstream SUNRPC TLS/receive + NFSv4 session hardening (8)
> → `->read_sock` receive rework (5) → the feature: authoritative XDR bvecs,
> NFSD consumer, DIO admission gate, page loan + locked-head merge, NFSv4
> COMPOUND, kill-switch (9) → KUnit: kunit-core backports, expose-internals,
> the four suites, configs (10) → the xeu experiment and this documentation
> commit (2). The incremental fixes were folded into the commits they belong
> to: the standalone-module wiring and IF_KUNIT exports into the
> expose-internals and suite commits, the discontinuity-gate and kill-switch
> covers and the mixed-geometry adaptation into their suites, the NFSv4
> module's hide/module-only fixes into its split commit, and the post-09-09
> harness/docs increments into the two tip commits. Four adaptations to the
> new base, each recorded as a `[snitzer: …]` note: this base's rewritten
> `NFSD_IO_DIRECT` write path (the `NFSD_DONTCACHE_FALLBACK` series) already
> sends a memory-misaligned middle down a single whole-WRITE buffered
> segment, so "accept immutable receive bvec requests" and the discontinuity
> gate demote into that path instead of the old split-with-DONTCACHE-middle
> fallback; `nfsd_write_dio_iters_init()` now takes the request/file handle
> (for `trace_nfsd_write_dio_split()`) and reports the buffered head/tail
> page indices, so the exported prototype and `dio_segments_test` follow
> (every segment expectation unchanged; the test additionally checks the
> reported page indices); and `svc_setup_socket()`'s receive-state free
> rides the base's `SVC_SOCK_RPCBIND_NOERR`-conditional error path. The
> tip builds `CONFIG_WERROR=y`-clean with the running kernel's config and
> all four suites as modules; **the KUnit runs, the bisect/sparse walk and
> the runtime harness have not yet been re-run on this base** (see
> `TESTING.md` → "Test coverage status").
>
> **Branch state (2026-09-09):** development moved to
> `kernel-7.1.8/main-5.NFSD_TCP_WRITE_ZEROCOPY`, rebased onto `v7.1.8-5`
> with a regrouped clean history. The base tag already carries the posted
> brd/zram/filecache fixes, so the series no longer does; the -EINVAL
> buffered fallback was dropped in favor of the in-series admission gate
> ("nfsd: require interior payload discontinuities to be logical-block
> aligned" + its `dio_segments_test` cases — see
> [`NFSD-LOAN-PAGES-EINVAL-ROOT-CAUSE.md`](NFSD-LOAN-PAGES-EINVAL-ROOT-CAUSE.md)).
> Dated narratives below and in the other docs predate the rebase and
> reference the old `kernel-7.1.8/main.NFSD_TCP_WRITE_ZEROCOPY` branch and
> `v7.1.8-3` base/configs; they remain accurate as history. The
> `v7.1.8-3-aarch64-*.config` files still serve as the KUnit/bisect config
> seeds until regenerated against `v7.1.8-5`.
>
> **Series layout** (bottom to top), sections kept contiguous — first
> the standalone **LOCALIO fix** (the direct-write post-op attribute
> invalidation; parked at the front because LOCALIO is not this
> project's focus — its submission-path work is tracked separately);
> then the feature section: SUNRPC TLS/receive hardening → NFSv4
> session hardening → `->read_sock` receive rework →
> authoritative XDR bvecs → DIO admission discontinuity gate (fused
> into the single alignment walk) → the page loan + locked-head
> merge → NFSv4 COMPOUND bvecs → loan kill-switch; then the KUnit
> section: kunit-core backports → the suites → configs → the kill-switch
> and gate covers; then this documentation commit at the tip.

Sub-project: letting loaned payloads with mid-page joints stay on the direct
path on devices without a virtual boundary (NVMe with SGLs), with the device
attribute exposed through XFS and statx to nfsd —
[`NVME_SGL_SUPPORT_PROJECT.md`](NVME_SGL_SUPPORT_PROJECT.md).

Deep technical notes (code path, alignment analysis, E1 NIC capabilities,
design decisions): [`PROJECT.md`](PROJECT.md).
How to test (KUnit suites + runtime validation + QA gates):
[`TESTING.md`](TESTING.md).
Robustness posture without special hardware + pre-submission checklist:
[`UPSTREAM_DEFENSE.md`](UPSTREAM_DEFENSE.md).

Live instrumentation for a running server (`zcstat.sh` + its usage guide),
and the receive-pass profiler (`run-rxpasses.sh`): see
[`zcstat.sh-usage.md`](zcstat.sh-usage.md) and `TESTING.md`.
The software-only NIC experiment that must pass before this series can
pay off on Xsight E1: [`xeu-hds-experiment/`](xeu-hds-experiment/).

Visual summaries (shareable reports):
- v7.1.8-5 status, and why E1 blocks the win:
  <https://claude.ai/code/artifact/4d9b9734-2ab5-485d-80b3-1e55d8f1eeba>
- tardis1 runbook for the xeu experiment:
  <https://claude.ai/code/artifact/12b1de8a-a8aa-49db-a233-3ecf312ce641>
- original v7.1.8-3 baseline analysis (superseded, kept for the problem
  derivation): <https://claude.ai/code/artifact/ef217765-7c3c-45c4-999b-b1d4b88ff85f>

---

## The problem

On NFS/TCP **writes** with `NFSD_IO_DIRECT`, nfsd copies every byte of the
received RPC payload out of the socket's skbs into the server's own
`rq_pages[]` before issuing the O_DIRECT storage write. The O_DIRECT write is
already zero-copy from `rq_pages` onward; the waste is one layer up, in the
SUNRPC TCP receive (`svc_tcp_read_msg()` hands `sock_recvmsg()` a destination
over `rq_pages`, so `tcp_recvmsg()` copies).

On a DRAM-bandwidth-bound server that single copy dominates the write path:

| | |
|---|---|
| `memmove` share of write-path CPU | ~41% |
| DRAM touches / written byte | **4** (NIC RX + copy read + copy write + NVMe DMA) |
| DRAM touches / read byte | 2 (NVMe DMA-in + zero-copy TX) |
| measured write vs read | ~24 vs ~53 GiB/s (writes ≈ read/2) |

Measurements: Jonathan Flynn (Hammerspace), OFP box `tardis1` (XSight E1,
ARM Neoverse-N2, dual 400G FPGA NICs, 8× NVMe, XFS, `io_cache_write=4`).

**Alignment is a red herring on XFS/NVMe.** It is tempting to blame misaligned
TCP payload for defeating O_DIRECT, but XFS/NVMe DIO memory alignment is 4
bytes and the payload starts ~160 bytes in (4-byte aligned), so DIO issues
fine. The cost is purely the receive-by-copy, upstream of the DIO gate.

## The solution — David Flynn's page loan

Instead of receiving into `rq_pages`, SUNRPC **lends** the TCP receive pages
that already hold the WRITE payload straight into the request's XDR buffer,
tagged as *authoritative* bvecs. NFSD decodes the WRITE arguments directly
from those borrowed pages and hands page-aligned segments to the filesystem as
O_DIRECT — collapsing the 4-touch write path to 2. It is conservative: loans
happen only for TCP, AUTH_SYS, non-TLS receives whose payload pages are
loanable; GSS, kTLS, and replies still copy. Misaligned segments fall back to
buffered I/O (the whole-iterator `nfsd_dio_iter_is_aligned()` check).

## Repository & branches

Hammerspace Linux repo: `ssh://git@gitlab.lab.hammer.space:2222/engineering/linux.git`

- **`kernel-7.1/hs-7.1.13-5.NFSD_TCP_WRITE_ZEROCOPY`** — **the current
  branch** (2026-09-18): the regrouped 35-commit series on
  `kernel-7.1/hs-7.1.13-5`. Base tag `v7.1.13-5`; the Hammerspace DP stack is
  below the series, so this is both the development and the integration
  branch. Everything below this bullet describes the superseded branches and
  is kept as history.
- **`kernel-7.1.8/main.NFSD_TCP_WRITE_ZEROCOPY`** — the **incremental-development
  branch**: the page-loan series plus follow-on fixes, on a clean `v7.1.8-3`
  base. **Bisect-clean** (see below). All development and all documentation
  changes land here; fixes are periodically folded into the base series commits.
  This is the branch to send upstream once further code review and testing is
  complete.
  <https://gitlab.lab.hammer.space/engineering/linux/-/commits/kernel-7.1.8/main.NFSD_TCP_WRITE_ZEROCOPY>
- **`kernel-7.1/hs-7.1.8-3.NFSD_TCP_WRITE_ZEROCOPY`** — the same series with
  Hammerspace's DP downstream kernel changes (28 commits: nfs inode
  attributes, statx ioctls, AUTH_NAME, the idmapper module, offline bit,
  re-export/readdirplus tweaks, …) on top. Integration branch, **periodically
  force-rebased onto main** so it inherits main's changes; not a place to develop
  (anything worthwhile must be on main before the rebase, or it is lost).
  <https://gitlab.lab.hammer.space/engineering/linux/-/commits/kernel-7.1/hs-7.1.8-3.NFSD_TCP_WRITE_ZEROCOPY>

Base tag: `v7.1.8-3` (Hammerspace milestone, carries the in-tree
`NFSD_IO_DIRECT` direct-write path the loan feeds into).

The branch was restructured 2026-09-05 (see `TESTING.md` → "The 2026-09-05
series restructuring"): the MLDSA config answer and the **five standalone
upstream fixes** (brd + zram sub-page-bvec corruption, three nfsd
`NFSD_IO_DIRECT` fixes — `TESTING.md` → "Fixes that must be sent upstream")
sit at the very front of the branch; the 29-commit page-loan series and its
code follow-ons form the middle (*build the SUNRPC KUnit suites as standalone
modules*, the two `dio_segments_test` fixes from the 2026-09-04 host
qualification, and the KUnit CONFIG enablement); every internal
`NFSD_TCP_WRITE_ZEROCOPY:` project-doc commit comes at the end. Current test
coverage status: `TESTING.md` → "Test coverage status".

## The 29-commit series

Ordered by provenance, most-settled first:

| # | Group | Origin |
|---|-------|--------|
| 1–2 | bug/kunit warning-suppression core + module fix | mainline |
| 3–7 | SUNRPC TLS / cmsg receive fixes | cel/nfsd-testing |
| 8–10 | nfsd session-replay fixes | cel/nfsd-testing |
| 11–15 | SUNRPC `->read_sock` receive conversion | Chuck Lever (not yet in cel) |
| 16–21 | page-loan feature code — SUNRPC bvec layer + NFSD consumers (zero KUnit content) | David Flynn / Ronaldo Pagani Yamashita |
| 22–29 | KUnit suites + module housekeeping — all test code, wiring, and accessors | David Flynn / Ronaldo Pagani Yamashita |

1–15 are prerequisites that bring the tree up to the receive/session
infrastructure the feature assumes; 16–29 are the feature and its tests,
with every KUnit file, Kconfig/Makefile test wiring, and test accessor
factored into the test commits — the production feature is complete and
operational before any test code exists. Provenance trailers: mainline
commits keep `(cherry picked from commit …)`; the cel/nfsd-testing eight
use `(cherry picked from cel/nfsd-testing commit …)`; the read_sock and
feature commits carry direct sign-off chains (author → David Flynn →
Mike Snitzer), and every commit on the branch ends with Mike Snitzer's
`Signed-off-by`.

### Adaptations for the Hammerspace base

The series was authored/qualified upstream against stable v7.1.8 with
`NFSD=y`. Three small adaptations were needed for the Hammerspace v7.1.8 base
(which carries some 7.2-ward changes and builds `NFSD=m` with
`KUNIT_ALL_TESTS=m`). Each is annotated with a `[snitzer: …]` note in the
commit body; the upstream author is preserved:

- **Commit 21** — gate `nfsd3_procedure()` behind `#if IS_ENABLED(CONFIG_KUNIT)`
  (otherwise an unused `static` under `CONFIG_KUNIT=n`).
- **Commit 22** — drop the synthetic `svc_version.vs_count` (this base already
  moved counting to per-net `sv_stats->vs_count`, so the member is gone), and
  `EXPORT_SYMBOL_IF_KUNIT(nfs3svc_decode_writeargs)` so the KUnit test resolves
  it as a loadable module under `NFSD=m`.

## Building & bisect testing

Three ARM test configs are provided, derived from the Fedora/RHEL aarch64
configurations in the `v7.1.8-3` (`kernel-7.1.8-*.hs.*`) src.rpm and adjusted
for bisect-safety testing:

- `v7.1.8-3-aarch64-4k.config`  — 4K pages
- `v7.1.8-3-aarch64-16k.config` — 16K pages (the 4k config with
  `ARM64_16K_PAGES=y`, resolved by `olddefconfig`; the redhat infra has no
  16k rhel flavor of its own. This is the non-4K page size the MacBook Pro
  testbeds can actually boot — Apple M-series lacks the 64K granule)
- `v7.1.8-3-aarch64-64k.config` — 64K pages (boot-testable only on hosts
  with the 64K granule: E1 hardware / Ampere)

All three set `CONFIG_WERROR=y` (so any warning fails the build — the bisect gate),
build the series' KUnit test modules (`NFSD_BVEC_KUNIT_TEST`,
`NFSD4_BVEC_KUNIT_TEST`, `SUNRPC_XDR_KUNIT_TEST`,
`SUNRPC_SVCSOCK_KUNIT_TEST` = m, `KUNIT_ALL_TESTS=n`), and disable
`DEBUG_INFO`/BTF (no pahole needed; does not affect compile warnings —
re-enabling them, `dnf install dwarves` for pahole, is a pending item).

Build (native aarch64):

```sh
cp NFSD_TCP_WRITE_ZEROCOPY/v7.1.8-3-aarch64-4k.config .config   # or -16k, -64k
make ARCH=arm64 olddefconfig
make ARCH=arm64 -j"$(nproc)"
```

**Bisect-safety verification.** Walk the series building every commit; with
`CONFIG_WERROR=y`, a clean build (`rc == 0`) means zero warnings and zero
errors:

```sh
for c in $(git rev-list --reverse v7.1.8-3..kernel-7.1.8/main.NFSD_TCP_WRITE_ZEROCOPY); do
    git checkout -q "$c"
    make ARCH=arm64 -j"$(nproc)" || { echo "FAIL at $c"; break; }
done
```

Verified result: the base and the original series build with **zero
warnings and zero errors on both 4K and 64K page sizes**, with the KUnit
test modules compiled and linked (native gcc 11.5, Rocky Linux 9.6). On 4K
the walk covers the **entire branch** and has been re-run clean after every
restructuring — most recently after the 2026-09-05 KUnit factoring: **all
39 code commits build clean, zero failures** (26 doc-only commits
auto-skipped by `run-bisect-walk.sh`; the walk re-applies the tip's 4k
config via `olddefconfig` at each commit so the KUnit options stay `=m`
once their Kconfig symbols exist). Further page-size walks (a 16K full walk, the 64K follow-ons) are
dropped as build gates — page size on the same architecture and compiler
varies only constants, so the 4K full walk and the 64K base+30 walk
bracket the useful endpoints (the series tip also builds clean at 16K —
the `7.1.8-3.hs.161` production build). The walk has since also been run
in full on **x86_64** (2026-09-06, `7.1.8-3.hs.297.el8.x86_64`, gcc
11.2.1, sparse v0.6.5-rc1): **42/42 code commits clean, zero failures,
zero branch-introduced sparse findings** — cross-architecture coverage
that found nothing new (`TESTING.md`). To see the copy
elimination at runtime, enable NFSD direct writes
(`/sys/kernel/debug/nfsd/io_cache_write = 2`) and use page-aligned TCP
AUTH_SYS non-TLS client writes.

## Hardware context — Xsight E1 NIC (for the in-place-reference direction)

The page-loan series references receive pages that still land via a copy on
generic hardware; the eventual zero-copy-receive goal depends on the NIC. The
E1's `xeu` FPGA driver was analyzed:

- **No hardware header-data split.** RX is single-buffer: the FPGA DMAs the
  whole frame (headers + payload) into one page_pool page at a fixed
  frame-start offset, so the payload lands at a header-length-dependent,
  non-page-aligned offset. The NIC will not hand NFSD pre-aligned payload.
- **But the substrate helps:** payload lives in ref-countable page_pool pages
  (a `tcp_read_sock` hold-refs receive is mechanically supported), and the
  FPGA parser reports per-layer header lengths (L0–L8) in the RX completion
  descriptor, so the exact header/payload boundary is handed to software with
  no reparse. DMA profiles steer AXI cache attributes and routing, not payload
  byte-offset.

So a true zero-copy write receive on E1 would split on the hardware L0–L8
lengths, reference the payload fragments in place, and rely on the storage DMA
path tolerating per-fragment offsets (or copy only the misaligned lead
fragment). True header-data split would need an Xsight firmware change.

## Origin & credits

- Investigation & measurements: **Jonathan Flynn** (Hammerspace).
- Page-loan design: **David Flynn** (Hammerspace).
- NFSD receive-bvec consumers + KUnit: **Ronaldo Pagani Yamashita**.
- SUNRPC `->read_sock` / TLS receive prerequisites: **Chuck Lever**.
