# David Flynn's performance series on the 7.3 canary

How the 83 patches of `hammerspace-7.3-performance-review` (all From
David Flynn) were carried onto `kernel-7.3/canary`, together with this
branch's own commits, and every place where they were changed to fit.
What to test, and in what order: `HANDOFF-tardis2-canary.md`.  Written
2026-11-04.

## The package and its base

The package is made against bca1a2f143eb: the local
`kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY` of 2026-10-20 (d4d8add316a5, on
`v7.3-7`) plus 132 of David's commits, the last on 2026-10-25 00:44 UTC.
Applied there in order, the patches give tree 5612c4412fc2, as the
package says.  On the build host they are rebuilt as commits on that base
(workspace branch `hsperf.review-base-applied`), the source of every
cherry-pick below.  The package itself is in
`/root/kernel/linux/patches/hammerspace-7.3-performance-review/`.

The canary is a newer base: `v7.3-11`, and hs-gitlab's ZEROCOPY branch
had moved on (e999ccf776a9, regrouped and rebased onto `v7.3-11` on
2026-11-04).  So the series is carried in a sandwich around that cut.

## The topics, in merge order

`hs/merge703_canary.sh` (kernel-rpmtree) checks out `v7.3-<N>` and merges:

| order | topic | what |
|---|---|---|
| 1 | nfsd-testing-canary-nfs_common-XFS-IOLOCK-and-async-DIO-middle (2649cfbf2d43) | unchanged |
| 2 | nfs-localio-performance-canary (6c4afbe39222) | David's 13 LOCALIO patches, on the merge of 1 into `v7.3-11` (bc9109b74db6) |
| 3 | NFSD_TCP_WRITE_ZEROCOPY-canary (96438024c362) | hs-gitlab ZEROCOPY e999ccf776a9 up to David's base, rebased onto 2 |
| 4 | nfs-and-nfsd-performance-canary (cfcdd9807468) | David's other 69, on 3 |
| 5 | NFSD_TCP_WRITE_ZEROCOPY-canary-part2 (this branch) | ZEROCOPY's code after David's base, ported onto 4 |
| 6 | changelog-canary | the `[7.3-11.canary]` entries |

Topic 2 starts at the merge of 1 into the release, not at 1 itself: the
patches also need NFS client work that reaches the canary only through
`kernel-7.3/main` (Jonathan Flynn's i_lock and ff-layout series,
UNCACHEABLE, the DS linger, nfs4_acl passthru, the LOCALIO re-probe and
writeback-deadlock fixes).

## Topic 2: LOCALIO

Package numbers 0001-0004, 0007, 0008, 0010-0015, 0044.  0001 and 0004 are
in for 0002's context and for LOCALIO READ completion; 0008 (io_uring
NOWAIT admission) because 0010 and later build on it; 0044 for 0008's
measured default.

- 0007 (per-CPU admission): kept `nfs_local_probe_all()` from the re-probe
  fixes; `nfs_local_commit_release_async()` (writeback deadlock v3, patch
  4) drops the UUID admission reference instead of the nfsd_file.  Its rule
  that only the last segment may return -EIOCBQUEUED holds with topic 1: a
  direct segment that is not last waits in `nfs_local_dio_write_wait()`.
- 0008: `xfs_file.c` keeps both the IOCB_NOSERIAL path of topic 1 and the
  staged input; `IOCB_DIO_GEOMETRY` moved to bit 26 (25 is
  `IOCB_NOSERIAL`); its caching of `nfs_dio_policy.seg_boundary` for NOWAIT
  is left out, as the canary's policy had no such field (restored in topic
  3, see there).
- 0013: topic 1's async-middle branch kept, the completion hook renamed.
- 0039 is not here: it needs ZEROCOPY's nfs_common (`NFS_DIO_GAP_MISALIGNED`,
  `seg_boundary`) and goes with topic 4.

## Topic 3: ZEROCOPY up to David's base

hs-gitlab e999ccf776a9 is `v7.3-11` plus 946 commits.  Left out: its 36
code commits authored after the cut (2026-10-25 00:44 UTC); every doc
commit stays, so this topic's docs are hs-gitlab's.  Rebased onto topic 2
(910 commits), with one hand resolution: the regroup had folded post-cut
code into "SUNRPC/nfsd: expose the receive internals for the KUnit suites"
(c555c2f2ff96), which is kept to its `VISIBLE_IF_KUNIT` change.

Last, 96438024c362 "NFS/localio: cache the segment boundary for NOWAIT
direct I/O" restores 0008's caching of `seg_boundary`, which this topic
brings into `nfs_dio_policy`.  The field lets a device with no virtual
boundary (SGL NVMe), or NVMe's 4 KiB boundary on larger pages, take
discontiguous receive-loan fragments direct; 0 keeps the page-sized rule.

The 1-2 November regroup also folded two post-cut changes into older
commits; topic 4 overwrote both and topic 5 ports them back.

## Topic 4: the rest of David's series

69 of the 70 left (0039 included), in package order.  0021 ("NFS: defer
the final superblock deactivation") is dropped: Mike's writeback deadlock
series replaced it.  Hand resolutions:

- 0009 (763be4181a15): topic 1 issues NFSD's direct middle as an async
  kiocb that NFSD waits on, and iomap gave -EDIOALIGN only to a
  synchronous kiocb, so the buffered retry could never run for that
  segment.  `iomap_dio_result()` now lets `IOCB_DIO_GEOMETRY` alone report
  it: only NFSD sets the flag, and it waits for the completion, so the
  attempt is drained before the retry.
- 0022 (df9469382452): a comment.
- 0038 (d6b0c82333a4) and 0049 (2a68b3a50d7c): David's move into
  `net/core/rx_loan.c` taken over the folded post-cut code.
- 0083 (cd57a466c835): the `rtc_read`/`rtc_write` knobs, without
  restoring a comment the DONTCACHE v10 trim removed.
- c0400502bcec: David's `localio_cpu_kunit` split on a zeroed `struct
  file`; `nfs_dio_split()` here reads `f_mapping` for a write's cut (his
  base cut at `PAGE_SIZE` without reading it), the crash of the fake files
  in `nfsd_bvec_kunit` on tardis2.  It now splits on a shmem file, and the
  expected splits stand.
- cfcdd9807468 "configs: enable the hammerspace-7.3-performance Kunit
  tests": `CONFIG_<suite>=m` in `redhat/configs/common/generic/` for the
  eight KUnit options the series adds (its LOCALIO one included), which
  dist-srpm otherwise reports unset.  NVME_TARGET_TCP_KUNIT_TEST stays out:
  it needs KUNIT=y.

## Topic 5 (this branch): ZEROCOPY after David's base

The 36 post-cut code commits, in hs-gitlab's order, ported onto David's
layout: the receive loans live in `net/core/rx_loan.c` and the region is
partitions of pfn-owned shards behind per-CPU caches.

- "page_pool: keep a fixed receive region page mapped for its device":
  Mike's device slots on David's layout, so that the two xeu NICs share the
  region and each keeps its mappings.  Each device and direction that maps
  has a slot with its own CPU cache and a free list of pages still mapped
  for it; the shards and shared caches hold only unmapped pages.  A pool
  takes its slot's pages, then the shards' (and maps them without an
  xarray index), then its slot's pages in other CPUs' caches, then another
  device's, unmapped under that slot's lock.  When a device's last pool
  goes, the region unmaps every page still mapped for it and hands the
  slot's pages to the shards.  A pool that waits for the region gets its
  slot when the carve attaches it.  A take outside the pools that finds the
  write partition short moves slot pages, unmapped, to the shards.
- Dropped (Mike): "return a bulk's overflow to the fixed receive region at
  once" and "prefetch the region list's top" (written for one region lock
  and list), and "count a bulk returned to the receive region with one
  atomic" (it edits the first's function).
- The SUNRPC commits are re-expressed on `rx_loan_table`: the run drop and
  both prefetches in `rx_loan_table_release()`, the skb reference takeover
  through `__rx_loan_table_append_ref()`, the published-table check through
  `rx_loan_table_refs_valid_checked()`; the others are resolved picks.
- KUnit: 14 of 20, with `state->lt` for the moved fields and
  `svc_tcp_rx_kunit_want_eighths()` put back.  Not carried: the six
  commits that put `svcsock_kunit` on a real service and real transports,
  as David's suites extend the old fixture.  That is the TODO in the
  handoff, once his suites pass as they are.
- The two folded changes: "net: rx_loan: walk the credit lists from the
  newest" and "net: rx_loan: raise a re-held socket's window with
  tcp_sock_raise_rcv_window()".

## State

Every commit of topics 2 to 5 builds with W=1 and no warning, all KUnit
suites enabled.  Nothing has run: no KUnit, no boot.  See the handoff.

All of David's tests are carried: the 17 test files his package adds or
changes (the KUnit suites and the `open_nomode` selftest) are all in the
canary, and every `KUNIT_CASE` and `KUNIT_CASE_PARAM` each of them
registers in his series is registered here.  Two differ from his text:
`localio_cpu_kunit.c` splits on a real file (topic 4, c0400502bcec), and
`nfsd_bvec_kunit.c` merges his includes with this branch's.
