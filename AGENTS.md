# AGENTS.md

Direction for AI agents (and humans) working in this tree on the branches
`kernel-7.1/hs-7.1.13-5.NFSD_TCP_WRITE_ZEROCOPY` and
`kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY`.

## Branch workflow

- **`kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY`** carries the series on the
  public `kernel-7.1.13/main`, rebased 2026-10-07 onto **`v7.1.13-21`** (the
  whole series, laid out as code, then KUnit preparation, then KUnit
  tests, then docs; `v7.1.13-21` itself carries the write split's cut
  per end, the byte-counted split gate `direct_misaligned_min_bytes`,
  LOCALIO's completion of a direct write that shares a page and the
  `nfs_dio_iter_aligned()` skip fix, so the branch no longer has commits
  for those; the tip before that rebase is kept as
  `kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY.pre-rebase-v7.1.13-21`). The write
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
one record per host. All three page sizes are complete: 16 KiB and
4 KiB on 2026-10-05, 64 KiB (tardis1, build 458) on 2026-10-07; the
64 KiB account is "64 KiB host" in `transplant/README.md`. **On tardis1,
read
[`NFSD_TCP_WRITE_ZEROCOPY/transplant/tardis1-build457-handoff.md`](NFSD_TCP_WRITE_ZEROCOPY/transplant/tardis1-build457-handoff.md)
first** (and `tardis1-boot1-testing.md` for builds 454 and 455): it says
which commits at the tip are still to be regrouped, carried to the HS
twin or decided.

**Converging on the Xsight driver** (tardis1, from 2026-10-07).
**Mike is rebooting tardis1 into a 16 KiB kernel on 2026-10-08. If
`getconf PAGESIZE` says 16384, start at
[`NFSD_TCP_WRITE_ZEROCOPY/xeu-rig/README.md`](NFSD_TCP_WRITE_ZEROCOPY/xeu-rig/README.md).**
It is the handoff for that boot: the job (find the best sizing against
the L3 at 16 KiB on the real card, client and drives; Mike wants solid
results the same day and it is the top priority), the first ten
minutes, the scripts and plans that run it, and what went wrong at
64 KiB. The 64 KiB account is the "64K" section of
[`NFSD_TCP_WRITE_ZEROCOPY/XEU_CONVERGENCE.md`](NFSD_TCP_WRITE_ZEROCOPY/XEU_CONVERGENCE.md)
(detail in its 5b to 5g), with the rules for the rig in its section 0:
only `enp161s0f0`, load only to the harness's own mount, `xeu` never
reloaded. Run nothing on veth on a boot that is to use the card.

**The primary focus from the evening of 2026-10-08 (Mike): NFS writes
from the remote client as fast as they will go, near line rate.** Start
at the "Hero" section of
[`NFSD_TCP_WRITE_ZEROCOPY/XEU_CONVERGENCE.md`](NFSD_TCP_WRITE_ZEROCOPY/XEU_CONVERGENCE.md):
what there is to reach for (the wire carries 48 GB/s, one drive takes
11.2), the brakes known to be on, the ladder
(`xeu-rig/plans/16k-7-hero.plan`) and where to look next. The 16 KiB
sizing results are its "16K" section. The step cell's verifying fio job
limits every cell near 2.3 GB/s; use `STEP_JOBS` for anything about
rate.

**Moving to another test host** (the 4K host, the 16K host, tardis1 at 64K): start with
[`NFSD_TCP_WRITE_ZEROCOPY/transplant/README.md`](NFSD_TCP_WRITE_ZEROCOPY/transplant/README.md)
-- bootstrap, config merge, build/install, preflight, the resumable test plan
(`run-testplan.sh`), what changes with page size, and the working rules for
this tree (they are not in a new host's Claude memory).  Anything committed on
a test host must pass `transplant/check-grouping.sh` (code, then KUnit, then
docs) before the work is handed back; regroup until it does.

**That rule is suspended on every test host (Mike, 2026-10-07): do not
worry about regrouping until he asks.** The tip is known not to be grouped.
Commit at the tip and stop: do not run `check-grouping.sh`, report its
result or offer a regroup. Claude's memory on the test hosts does not
persist, so this is the only place a new session will find it.

Start with the README before touching the series; it explains why each of the
three base-adaptation commits exists and how to reproduce the bisect walk.
