# AGENTS.md

Direction for AI agents (and humans) working in this tree on the branches
`kernel-7.1/hs-7.1.13-5.NFSD_TCP_WRITE_ZEROCOPY` and
`kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY`.

## Branch workflow

- **`kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY`** carries the series on the
  public `kernel-7.1.13/main`, rebased 2026-10-06 onto **`v7.1.13-20`** (the
  whole series, laid out as code, then KUnit preparation, then KUnit
  tests, then docs; `v7.1.13-20` itself carries the write split's cut at
  page boundaries, the page-counted split gate
  `direct_misaligned_min_pages` and the `nfs_dio_iter_aligned()` skip
  fix, so the branch no longer has commits for those). The write
  split lives in `fs/nfs_common/nfs_dio.c` (`nfs_dio_split()`, shared with
  NFS LOCALIO), so the immutable-bvec and gate commits change that file, not
  `fs/nfsd/vfs.c`; see `../NFS_LOCALIO_DONTCACHE/`. The `v7.1.13-12` version
  is kept as `kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY.v7.1.13-12`. It was
  the same 35 commits on `v7.1.13-12`
  instead of the Hammerspace DP stack. The two branches differ only in the
  base adaptations recorded as `[snitzer: …]` notes; a change made on one
  must be carried to the other.
- **`kernel-7.1/hs-7.1.13-5.NFSD_TCP_WRITE_ZEROCOPY` is the current branch**
  (since 2026-09-18): the page-loan series, regrouped into contiguous logical
  sections, on top of `kernel-7.1/hs-7.1.13-5` — which already carries the
  Hammerspace DP downstream stack, so there is no separate integration branch
  to keep in sync any more. Do all development and all `*.md` edits here,
  as **incremental commits at the tip**; fold/regroup only when Mike asks for
  a cleanup (history is rebased then, so don't rely on stable SHAs).
- The earlier branches — `kernel-7.1.8/main-5.NFSD_TCP_WRITE_ZEROCOPY`
  (development, on `v7.1.8-5`), `kernel-7.1/hs-7.1.8-5.NFSD_TCP_WRITE_ZEROCOPY`
  (its Hammerspace integration branch), `kernel-7.1/hs-7.1.8-3.NFSD_TCP_WRITE_ZEROCOPY`
  and `kernel-7.1.8/main.NFSD_TCP_WRITE_ZEROCOPY` (the raw incremental history)
  — are **history**; dated narratives in the docs still name them.

## NFSD TCP write-path zero-copy (page-loan series)

The server-side page-loan work — eliminating the redundant receive-side data
copy on the NFS write path — has its own directory:

- **[`NFSD_TCP_WRITE_ZEROCOPY/README.md`](NFSD_TCP_WRITE_ZEROCOPY/README.md)** —
  the canonical overview: the problem and measurements, David Flynn's page-loan
  design, the two branches and how they relate, the 30-commit series structure
  and provenance, the Hammerspace-base adaptations (`[snitzer: …]` notes),
  build/bisect-test instructions, and Xsight E1 NIC hardware context.
- **[`NFSD_TCP_WRITE_ZEROCOPY/PROJECT.md`](NFSD_TCP_WRITE_ZEROCOPY/PROJECT.md)** —
  deep technical notes: the code path with references, the alignment analysis,
  the candidate zero-copy-receive direction and its ranked risks, the full
  Xsight E1 RX-capability findings, and the design decisions behind the
  page-loan integration (including the series' generic mechanism, per-procedure
  opt-in, and ownership contract).
- **[`NFSD_TCP_WRITE_ZEROCOPY/TESTING.md`](NFSD_TCP_WRITE_ZEROCOPY/TESTING.md)** —
  how to test: the four KUnit suites (build, run, expected results, the known
  NFSv4.1 replay finding), the runtime validation that shows the copy
  elimination (`io_cache_write=2` + page-aligned writes, tracepoints, flat
  page-cache footprint), and the static QA gates (W=1 / sparse / checkpatch).
- **`NFSD_TCP_WRITE_ZEROCOPY/v7.1.8-3-aarch64-4k.config`** and
  **`…-64k.config`** — the ARM test kernel configs used to verify the series is
  bisect-clean (base + all 30 commits build with zero warnings/errors under
  `CONFIG_WERROR=y`, on both 4K and 64K page sizes, with the KUnit tests built).

**Words.** *Budget* is the configured limit on how much of the fixed
receive region loaned WRITEs may hold (`svc_tcp_rx_budget_pages`,
`net.sunrpc.region_budget_*`); a budget of 0 means copy. *Credit* is only
what one WRITE draws against the budget. Results are described by what the
storage can do: it *needs whole pages* (NVMe without SGLs, tardis1,
nvme-loop) or *takes arbitrary pieces* (NVMe with SGLs, NVMe/TCP).

**Results across page sizes** are in
[`NFSD_TCP_WRITE_ZEROCOPY/transplant/RESULTS.md`](NFSD_TCP_WRITE_ZEROCOPY/transplant/RESULTS.md),
one record per host. 16 KiB and 4 KiB are complete (2026-10-05); tardis1
is next. **On tardis1, read
[`NFSD_TCP_WRITE_ZEROCOPY/transplant/tardis1-boot1-testing.md`](NFSD_TCP_WRITE_ZEROCOPY/transplant/tardis1-boot1-testing.md)
first:** its first run found a charge defect at 64 KiB, the fix is
committed but not yet built, and that file is the only record of where
the work stands.

**Moving to another test host** (the 4K host, the 16K host, tardis1 at 64K): start with
[`NFSD_TCP_WRITE_ZEROCOPY/transplant/README.md`](NFSD_TCP_WRITE_ZEROCOPY/transplant/README.md)
-- bootstrap, config merge, build/install, preflight, the resumable test plan
(`run-testplan.sh`), what changes with page size, and the working rules for
this tree (they are not in a new host's Claude memory).  Anything committed on
a test host must pass `transplant/check-grouping.sh` (code, then KUnit, then
docs) before the work is handed back; regroup until it does.

Start with the README before touching the series; it explains why each of the
three base-adaptation commits exists and how to reproduce the bisect walk.
