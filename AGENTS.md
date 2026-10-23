# AGENTS.md

Direction for AI agents (and humans) working in this tree on the branch
`kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY` and the 7.1 branches it came
from.

## Branch workflow

**Do not concern yourself with the Hammerspace twin branch (Mike,
2026-10-10).** Work on `kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY`
(since 2026-10-14), which has everything needed for now. Do not carry
changes to `kernel-7.1/hs-7.1.13-5.NFSD_TCP_WRITE_ZEROCOPY`, do not list "carry to
the twin" as work owed, and do not raise it in reports. Porting to the
full private Hammerspace kernel is not this work's concern until Mike
says so. This overrides what the two entries below say about keeping
the branches in step and about where development is done.

- **`kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY` is the branch to work
  on**, on hs-gitlab again since 2026-10-22: the `v7.3-7` series was
  pushed first, on 2026-10-20, under the new name
  `kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY-v7.3-7` (left at
  `eb1ca139fcb5`, behind it; do not commit there), then Mike pushed it
  over the old name, whose `v7.3-3` series (`26c5cdcd549a`) is kept as
  `….pre-rebase-v7.3-7`:
  the series on the public `kernel-7.3/main`, rebased 2026-10-20 onto
  **`v7.3-7`** (`git rebase --onto v7.3-7 v7.3-3`, two conflicts resolved
  by hand, see `NFSD_TCP_WRITE_ZEROCOPY/README.md`; the tip before is
  `.pre-rebase-v7.3-7`), and before that 2026-10-14 onto
  **`v7.3-3`** (after `v7.3-rc7`), laid out as code, then KUnit interface, then
  KUnit tests, then docs. `v7.3-3` already carries 15 of the series'
  commits (the TLS control-record and alert fixes, the nfsd session-slot
  fixes, receiving RPC records with `->read_sock`, and the two `bug/kunit`
  warning-suppression commits), so the branch no longer has them. Five
  commits were adapted to the new base, each with a `[snitzer: …]` note:
  three for context `v7.3-3` changed (an include in `fs/nfsd/vfs.c` and in
  `fs/nfsd/nfs3xdr.c`, and loop's direct-I/O alignment checks), one for
  `struct bvec_iter`'s `bi_bvec_done` being renamed `bi_offset`, and one for
  nfsd's status codes moving to `fs/nfsd/nfserr.h`. The tip before the
  rebase, the same as the 7.1.13 branch's, is kept as
  `kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY.pre-rebase-v7.3-3`. Commit at
  its tip.

  **Reworked for upstream on the night of 2026-10-14** (the account is
  steps 3 to 5 of `NFSD_TCP_WRITE_ZEROCOPY/CODE-SERIES.md`, the review
  behind it in `NFSD_TCP_WRITE_ZEROCOPY/code-review/`): 458 commits, 38
  of them code, 44 KUnit interface and tests, 376 docs. The code group
  was reviewed as Chuck Lever, Christoph Hellwig and netdev would read
  it, then folded and split until no `Fixes:` points inside the series
  (each fix is in what it fixes), moved so that every commit builds,
  and given upstream messages (no lab, product, KUnit or downstream
  text; `NFSD:` and `SUNRPC:` subjects in Chuck's style). Every commit
  was built in turn at 16 KiB with `CONFIG_WERROR=y` and sparse; the
  stages are kept as `kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY-code.73rebase1`
  to `.73rebase9` (code only; `.73rebase9` is the finished code group,
  with its calls into TCP through TCP's own interfaces),
  and the tip before the rework as
  `kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY.pre-73rebase`. Two changes
  in behaviour came with it: **TCP receive page loans are off by default**
  (then `sunrpc.svc_tcp_rx_loan_pages=N`; **since 22 October there is no
  switch** (Mike): a budget above 0 is what loans, and with every budget
  at 0, the default and the rig's setting at rest, nothing is loaned,
  behind a static key that makes it free; see "What a server that never
  loans pays" in `NFSD_TCP_WRITE_ZEROCOPY/UPSTREAM_DEFENSE.md`.
  `xeu-rig/settled.sh` still holds the switch at `Y` on build 491 and
  before), and an NFSv4 COMPOUND with a tag keeps its loan until the
  reply is encoded. **On 15 October several thread pools per NUMA node
  (`pools_per_node`) was dropped (Mike: Chuck Lever has called it a
  dead end; the patch is in `NFSD_TCP_WRITE_ZEROCOPY/patches/dropped/`,
  tardis1 runs one pool), and the spare-page bound was folded into the
  page-swap commit it fixes**; the tips before are kept as
  `.pre-drop-pools` and `.pre-fold-spare`. **The same day the copy
  outside the socket lock (`svc_tcp_rx_copy_threads`) and the
  SO_RCVLOWAT mark (`svc_tcp_rx_lowat`) were dropped too (Mike): neither
  showed a gain in practice, and both ran at 0 on the rig**; their
  patches are in `patches/dropped/` as well, and the tip before is kept
  as `.pre-drop-claim-lowat`. **veth's `rx_ip_align` followed (Mike),
  a test aid for the VM rigs that the card does not use; the patch is in
  `patches/dropped/` and the tip before is `.pre-drop-veth`.** **Then
  the linger (`svc_tcp_rx_linger_us`) and keep (`svc_tcp_rx_keep`), after
  an A/B on build 474 showed neither worth its place (Mike); the tip
  before is `.pre-drop-linger-keep`, and the rig runs without them (linger
  0 and keep N on kernels that still have them).** The code group is 32
  commits.
  What of the code group runs in practice is step 6 of
  `NFSD_TCP_WRITE_ZEROCOPY/CODE-SERIES.md` (a survey, not a decision:
  nothing else is to be dropped unless Mike says so). Still open
  (Mike): whether to drop the stacking device's DMA alignment (the
  loan harness's dm-delay needs it), and the page_pool region's future
  upstream. Ronaldo's twelve commits carry
  his mail address, `ronaldo@slipstream-io.com`, as author and in his
  Signed-off-by since 2026-10-15 (Mike); the tips before that are kept
  with the suffix `.pre-ronaldo-addr`.
- **`kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY`** is the same series on
  7.1.13, as regrouped and force-pushed on 2026-10-14 (`ea18d80f8bda`).
  Build 464, which tardis1 runs until a 7.3 kernel is booted, was built
  from it. It is on the public `kernel-7.1.13/main`, rebased 2026-10-07
  onto **`v7.1.13-21`** (the
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

    Assisted-by: LLM
    Signed-off-by: <the configured git user.name> <the configured git user.email>

The Assisted-by names no tool or model: it is `Assisted-by: LLM`
exactly, once, however many agents worked on the commit (Mike,
2026-10-15; every commit on the branch and on `.73rebase9` was
rewritten to it that day, the tips before kept with the suffix
`.pre-llm-trailer`).

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

## One group to a commit (hard rule)

**Keep code, KUnit interface, KUnit tests and docs in separate
commits, so that they can be regrouped later (Mike, 2026-10-14).**
Every commit belongs to exactly one of the four groups the series is
laid out in; a change that spans two is two commits, even when one
cannot be used without the other:

- **code**: the kernel itself, outside the suites and what only they
  use.
- **KUnit interface**: what the kernel exposes or adds only for the
  suites (`VISIBLE_IF_KUNIT`, `EXPORT_SYMBOL_IF_KUNIT`, helpers under
  `CONFIG_KUNIT` and their declarations). Its subject ends "for the
  KUnit suites", as `transplant/check-grouping.sh` expects.
- **KUnit tests**: the `*_kunit.c` files and what builds them.
- **docs**: `NFSD_TCP_WRITE_ZEROCOPY/` (the rig's scripts included)
  and this file.

This holds while regrouping itself is suspended (below): commit at the
tip, but never one commit across two groups. Before committing, check
`git diff --cached --stat` against the list above.

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

**tardis1 runs build 494, the kernel built from the tip of 22 October
(`77f82c816493`; booted that evening): start at "Build 494: the kernel
from the tip" in
[`NFSD_TCP_WRITE_ZEROCOPY/xeu-rig/README.md`](NFSD_TCP_WRITE_ZEROCOPY/xeu-rig/README.md).**
The code that had only been built (the region key in page_pool and
sunrpc, one budget lookup at a loaned record's first region skb, the
budget's lock on its own cache line) has run: KUnit 7, 50, 7 and 10,
copy mode at 491's rate on two ports (58.47 and 57.89 GB/s) and on the
wire on one, and loans admitted with none denied. The follow-up of
Xsight's replies is still
[`NFSD_TCP_WRITE_ZEROCOPY/xeu-rig/XSIGHT-FOLLOWUP.md`](NFSD_TCP_WRITE_ZEROCOPY/xeu-rig/XSIGHT-FOLLOWUP.md).

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

**Every run on tardis1 is audited against
`NFSD_TCP_WRITE_ZEROCOPY/xeu-rig/settled.sh` and refuses to run off
it (Mike, 2026-10-14: "lock it down hard").** It holds the settled
setting in one place: where nfsd's threads and each port's ring,
async and transmit (XPS) work run, the sunrpc, xeu and nfsd settings
at rest and for the one-port and two-port settings, the region and
the drives' caches. A restart of nfs-server, any open of a port, a
reload of xeu and the svcsock KUnit suite each undid part of it on 14
October unseen, and every run on build 473 that day was thrown away
for it. After a boot and after anything that changes the host, run
`./settled.sh apply` and see it end in "the settled setting"; never
get past a BAD item by hand. A change to the setting is made in
`settled.sh`, after a run that shows why, and nowhere else.

**!!! Hammerspace's `irq_tuning.service` and `rps_xps_tuning.service`
are MASKED on tardis1 (Mike, 2026-10-14), and our tuning is TO BE
RECONCILED with them. !!!** Their E1 profile puts port a's interrupts
on CPUs 8-31 and port b's on 32-55, on nfsd's CPUs, writes its own
XPS map, and ran again on every reload of `xeu`, undoing
`settled.sh`'s layout. They are masked, not removed: the units, their
udev rule (`70-xeu-nic.rules`) and the irqbalance file they wrote are
in `/root/hs-tuning-units-masked-2026-10-14/` with how to put them
back, and `settled.sh` now sets the card's PCIe MaxReadReq of 512
that `irq_tuning` used to. Nothing measured on this rig says what
Hammerspace's own layout carries: before this work goes into
Hammerspace's product, the two layouts are to be measured against
each other and made one. Keep this note until that is done.

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

**After the reboot into the build of the reworked tree (pushed
2026-10-14 at `32cf5556bd2e`), on tardis1 start at the first section
of
[`NFSD_TCP_WRITE_ZEROCOPY/xeu-rig/README.md`](NFSD_TCP_WRITE_ZEROCOPY/xeu-rig/README.md),
"15 October: the boot into the reworked tree — read this first"**: the
first checks on that kernel (the settled setting, the KUnit suites, no
regression against build 473) and what is still Mike's to decide. The
section after it, "13 October, evening", is the handoff before that. It is the handoff: where
copy mode stands (60.41 GB/s on two ports, 47.39 on one, the limit
on two ports being memory with the DIMMs fully populated), the two
pinned command lines, the rules for a run, what was decided that day
and is not to be undone from older text (`svc_tcp_rx_lowat=0`, no
`fq` on the client, the client reached at its management address
10.20.12.106), what a loaned byte costs in memory as measured that
evening (`NFSD_TCP_WRITE_ZEROCOPY/LOAN-PERFORMANCE.md`), and **the
next step (Mike): the series is rebased onto `v7.3-3` (after
7.3-rc7; done 2026-10-14, see "Branch workflow"), and on that kernel
the loan's memory-cost battery is to be run again with one-port and
two-port copy mode beside it to show no regression**. The paragraphs below that name earlier starting points
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
worry about regrouping until he asks.** The branch was regrouped on
2026-10-14 and `check-grouping.sh v7.3-3` passed then; a code or KUnit
commit at the tip from here on breaks that, which is expected until
Mike asks for the next regroup. Commit at the tip and stop: do not
offer a regroup. Claude's memory on the test hosts does not
persist, so this is the only place a new session will find it.

Start with the README before touching the series; it explains why each of the
three base-adaptation commits exists and how to reproduce the bisect walk.
