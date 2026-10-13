# AGENTS.md

Direction for AI agents (and humans) working in this tree on the branches
`kernel-7.1/hs-7.1.13-5.NFSD_TCP_WRITE_ZEROCOPY` and
`kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY`.

## Branch workflow

**Do not concern yourself with the Hammerspace twin branch (Mike,
2026-10-10).** Work on `kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY`,
which has everything needed for now. Do not carry changes to
`kernel-7.1/hs-7.1.13-5.NFSD_TCP_WRITE_ZEROCOPY`, do not list "carry to
the twin" as work owed, and do not raise it in reports. Porting to the
full private Hammerspace kernel is not this work's concern until Mike
says so. This overrides what the two entries below say about keeping
the branches in step and about where development is done.

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

## Commit trailers

Every commit an agent makes in this tree, code or docs, ends with these
two lines and no others of its own:

    Assisted-by: Claude:<model id, e.g. claude-opus-5-5>
    Signed-off-by: <the configured git user.name> <the configured git user.email>

The Signed-off-by is whoever's tree this is: take it from
`git config user.name` and `git config user.email` (`git commit -s`
writes it), never from a name written in a document or copied from
another commit. Never `Co-Authored-By`, whatever a tool's default says,
and never leave the Signed-off-by off.

A commit message says what the change is and why. It carries no
disclaimer about the state of the work ("Not booted.", "untested",
"compile-tested only"): this series is being polished, and such lines
read as a mark against the code (Mike, 2026-10-09). What has and has
not been run belongs in the handoff documents.

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

**Both ports are ours from 2026-10-11 (Mike): the rule that
`enp164s0f0` is never touched is lifted**, for runs on two ports at
once. The eight data drives were two stripes of four for it until
2026-10-12; **since then there is no MD (Mike): each drive has its own
XFS, `/hs/nvme0` to `/hs/nvme7`**, the first four for `enp161s0f0`
and the other four for `enp164s0f0`, and `xeu-rig/twoport.sh` exports
and mounts each and divides a port's writers among its four. They are
not in `/etc/fstab`: after a boot, `mount -o noatime /dev/nvmeNn1
/hs/nvmeN` for each. Still check that no NFS client is connected before a port is
reconfigured or `xeu` reloaded (192.168.0.105 has mounted over
`enp164s0f0` before). The first section of `xeu-rig/README.md` has
where that stands.

**Always run tardis1 in the mode that acts like enterprise storage
(Mike, 2026-10-12: "seriously: never forget this").** Advertise WCE=0
to Linux, so the sync to storage is elided, and leave the drives' own
write caches enabled: `/sys/block/<dev>/queue/write_cache` reads
`write through` on the eight data drives (and on `md0` and `md1`
when there were stripes). The
export stays `sync`; an `async` export is not the way to get this.
`xeu-rig/drive-cache.sh` applies and prints it, `twoport.sh` and
`sweep.sh` apply it at the start of a run, and
`/etc/udev/rules.d/99-zc-write-through.rules` sets it at boot. Check it
before any run. With the drives flushing (`write back`, everything on
the four-drive stripes before that date) each COMMIT flushes every
drive of the stripe and one port carries 36 GB/s where it otherwise
carries 43; those figures are not comparable with later ones.

**Keep a run inside the drives' fast region (found 2026-10-12).** A
data drive takes about 625 GB at full rate from its last idle of 20 s.
Past that, any pause in the writes (0.3 s) leaves it at 4 GB/s for 25
to 55 s, and the drives decide the figure. So it is the length of a
run that matters, not rest: `twoport.sh` defaults to `WARM=15
RUNTIME=45` for it (two ports, four drives each); one port on its own
four needs `WARM=10 RUNTIME=25`, or all eight drives in `FS_A`. Read
`drives.txt` of every run (what each drive wrote, and `no drive
stepped down`) before believing its figure. `xeu-rig/drvchar.py` is
the local probe; the account is "What a drive takes" in
`NFSD_TCP_WRITE_ZEROCOPY/XEU_CONVERGENCE.md`.

**From the evening of 2026-10-13, on tardis1 start at the first
section of
[`NFSD_TCP_WRITE_ZEROCOPY/xeu-rig/README.md`](NFSD_TCP_WRITE_ZEROCOPY/xeu-rig/README.md),
"13 October, evening: read this first".** It is the handoff: where
copy mode stands (60.41 GB/s on two ports, 47.39 on one, the limit
on two ports being memory with the DIMMs fully populated), the two
pinned command lines, the rules for a run, what was decided that day
and is not to be undone from older text (`svc_tcp_rx_lowat=0`, no
`fq` on the client, the client reached at its management address
10.20.12.106), and **the next step (Mike): what a loaned byte costs
in memory**. The paragraphs below that name earlier starting points
are history.

**Do not optimize copy mode at the loan's expense (Mike,
2026-10-13).** The loan path is to stay as fast as it can be, for
NVMe drives with SGLs when they come. Copy mode is at what a
piecewise copy carries on this machine (60 to 61 GB/s on two ports),
and the last 4% of it would take receive buffers in blocks, pools
outside the region or packed payloads, each of which a loan pays
for. Keep the receive path as a loan needs it: one frame to a
buffer, pages returned last in, first out, all inside the region.
The account is "The 4%" in
`NFSD_TCP_WRITE_ZEROCOPY/TRACING-NFSD-AND-XEU-MEMCPY-TIME.md`.

**The primary focus from the evening of 2026-10-08 (Mike): NFS writes
from the remote client as fast as they will go, near line rate.** On
tardis1 start at the first section of
[`NFSD_TCP_WRITE_ZEROCOPY/xeu-rig/README.md`](NFSD_TCP_WRITE_ZEROCOPY/xeu-rig/README.md)
("where it stands", written 2026-10-09 before a reboot into the kernel
built from `…-WIP3`: its section "The boot after build 461" is the
list to work from). **The direction is copy mode (Mike, 2026-10-09):
the direct copy carries 35.9 GB/s with a 1 MiB held receive buffer,
against 27.7 at 256 KiB and the loan's 30.3**, and a change is judged
as one of many and not by its own effect on the rate. The harness
turned deferred skb frees off until that afternoon. The account is
`XEU_CONVERGENCE.md` from "Build 461" to "The copy with a larger held
buffer". Then the "Hero" section of
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
