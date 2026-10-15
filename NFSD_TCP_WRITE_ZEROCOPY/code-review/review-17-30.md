# Review of positions 17-30

Reviewed with the eyes of Chuck Lever (nfsd/SUNRPC), Christoph Hellwig
(block/fs) and the page_pool maintainers (Kicinski, Brouer, Apalodimas).
Proposed messages are in new-17.txt .. new-30.txt.

## Series-level observations for this stretch

1. **17 is a correctness fix that lands after the bug it fixes is exposed.**
   Commit 12 starts loaning receive pages. That yields bvec arrays with
   interior gaps, and commit 11 has already let them go to
   `nfs_dio_split()` as direct I/O. Between 12 and 17 a loaned WRITE to an
   NVMe-backed export with direct writes enabled can fail with -EINVAL
   from `bio_split_io_at()` after part of it was written. It is not a build
   break, but it breaks bisect behaviour and anyone reading the series would
   flag it. Move 17 before 11/12. Better still, pull the
   `nfs_dio_iter_aligned()` rewrite out of commit 11 (its `[snitzer:]`
   note describes a change to `fs/nfs_common/nfs_dio.c` buried in an nfsd
   commit), combine it with 17 into one `nfs_common:` commit, and put that
   commit, followed by 18, ahead of 11.
2. **The nfs_common/nfsd boundary block (17-20) belongs next to 1-6.** 18-20
   use `STATX_DIO_SEG_BOUNDARY` from 3-5 and do not depend on SUNRPC.
   Place 17, 18, 19 and 20 right after 6 (or right after the nfs_common
   prep described above). Fold 20, which adds two lines, into 19: "record a
   value" and "use it" are one change here.
3. **Fold 22 into 21.** 22 has a `Fixes:` tag on 21, which is not merged.
4. **23 is out of place, mixes changes, and its message is stale.** It is the
   NFSv4 counterpart of 11 and should follow 11/12, before the
   locked-head and DIO work. It holds two independent decoder fixes plus the
   enabling bit. Its message claims a SETXATTR repair that is not in the diff:
   `8fcdabd5ffff` "NFSD: copy SETXATTR data from all XDR buffer segments" is
   already in the v7.3-3 base. Split it (see below).
5. **Fold 24 (kill switch) into 12** or put it right after 12. Its own
   `[snitzer:]` note says it is a placement instruction. Separately, see the
   merit concerns about defaulting loans on.
6. **25 mixes two unrelated changes.** Split the SUNRPC page-pool-reference
   change, which fixes 12's handling of page-pool pages and has nothing to
   do with the region, from the page_pool region. Fold the SUNRPC half
   into 12 or make it its own commit right after 12.
7. **Fold 27 and 29 into 25.** Single-device-then-several and a region that
   is never freed are development history, not upstream steps. 29 also
   carries a fix to 25 (the `page_pool_create_percpu()` error path leaks the
   binding).
8. **Move 26 and 28 to their first users**, or fold each into its first user's
   preceding page_pool commit. 26 (`page_pool_page_in_rx_region()`) is
   first used in 34. 28 (`page_pool_rx_region_size()`, exported) is first
   used in 38. Adding an API, and above all an `EXPORT_SYMBOL_GPL`, with no
   user is what netdev rejects outright.
9. **The page_pool region (25-30) is the stretch least likely to be accepted
   in its current form**; see 25 below. Before posting, decide whether it
   goes upstream at all or is kept as a platform-specific out-of-tree piece.

## 17 30b2668e7aad nfsd: require interior payload discontinuities to be logical-block aligned

- **Sequencing:** wrong place (see series point 1). The subject says
  `nfsd:`, but the diff touches only `fs/nfs_common/nfs_dio.c`
  (`nfs_dio_iter_aligned_and_splittable()`, `nfs_dio_split()`), so it should
  be `nfs_common:`. It is a natural fold candidate with the alignment walk
  rewrite now hidden in commit 11, and arguably with 18: 18 immediately
  replaces the `~PAGE_MASK` test 17 introduces with `seg_mask`. Keeping 17
  and 18 separate is defensible (fix, then refinement), but only if 17 moves
  ahead of the loan enablement.
- **Merit:** sound and needed, because without it loans produce -EINVAL
  writes. The new name `nfs_dio_iter_aligned_and_splittable()` is long.
  Hellwig would ask whether this belongs in the block layer, as a helper
  such as "would this iter be splittable for this queue" next to
  `bio_split_io_at()`, rather than a re-derivation of block-layer rules in
  nfs_common. Expect that question. The comment block above the function is
  essay-length; trim it.
- **Message:** overdone. It includes the live-capture narrative ("brd-backed
  nvme-loop", "12/12 rejections", byte 261736) and a reference to an
  in-tree analysis document that will not exist upstream. A paragraph
  justifies the single walk with O(nvecs) reasoning, and a long `[snitzer:]`
  note is rebase bookkeeping (old name, v7.1.13-14). Drop all of it. The
  problem statement itself is good.

## 18 46e8d4a7c82c nfs_common: let the direct I/O policy carry the device's segment boundary

- **Sequencing:** fine after 17. It depends on 3-5 (statx field) only
  through its callers in 19, so the whole nfs_common block can move up
  (series point 2).
- **Merit:** reasonable, but the encoding will draw comment. `seg_boundary`
  is three-state (0 = unknown -> PAGE_SIZE, `U32_MAX` = none, power of two),
  translated by `nfs_dio_seg_boundary()` and decoded by
  `nfs_dio_joint_boundary()`, and the internal `seg_mask` inverts the
  meaning of 0 again. Two helpers and a sentinel to carry one mask is heavy.
  A reviewer would suggest storing the mask the way the block layer does
  (`virt_boundary_mask`, 0 = none) and resolving "unknown" once when the
  nfsd_file is filled in. The `#include <asm/page.h>` in a generic header is
  a minor smell. The comment admits that zoned devices with
  `zone_write_granularity` > logical block size are not covered. Since the
  check exists to prevent -EINVAL, a maintainer will ask why not handle it
  or refuse direct I/O there. Dropping the gap rule for devices with no
  boundary rests on stacking drivers propagating `virt_boundary_mask`
  (commits 1 and 6), so say so in those commits.
- **Message:** good, slightly long. Lightly trimmed.

## 19 86dd05b884dc nfsd: record the direct I/O segment boundary of an nfsd_file

- **Sequencing:** fine. Fold 20 into it.
- **Merit:** fine. `fh_getattr()` now asks for one more attribute on every
  regular-file GETATTR, the same way it already does for DIOALIGN, so no new
  cost class. The tracepoint shows both raw and decoded values plus an s64
  for "-1 = not reported", which is debugging convenience. One value (the
  enforced boundary) would do. Using `nfs_dio_joint_boundary()` inside nfsd's
  trace.h leaks nfs_common's encoding into nfsd tracing.
- **Message:** fine; tightened.

## 20 0dfe5616ef63 nfsd: split direct writes by the device's segment boundary

- **Sequencing:** two-line wiring of 19's field into the policy for WRITE and
  LOCALIO. Fold into 19.
- **Merit:** fine. The note that a boundary larger than PAGE_SIZE turns the
  page joints of a contiguous payload into gaps is a behaviour change for
  LOCALIO as well, so keep it in the message.
- **Message:** fine; the parenthetical about "receive pages used in place"
  is reworded.

## 21 b5b0f1a837b5 SUNRPC: merge locked-head copies into a whole-page loan bvec

- **Sequencing:** OK after the loan-table commits (15, 16), but 22 must be
  folded in.
- **Merit:** a loan-path optimization with real complexity:
  `svc_tcp_rx_copy_segment_merged()`, `svc_tcp_rx_retract_last()`, and a
  `merge_fill` top-up path in `svc_tcp_rx_borrow_segment()` that copies
  loaned bytes after all. A maintainer will ask how often locked heads occur
  in practice (skb collapse, kmalloc'd heads from drivers that do not use
  head frags) and whether the simpler answer is to materialize the record.
  Lying about a copied page by entering it as "borrowed", then fixing the
  byte counts after the fact, is fragile: 22 is the proof, a WARN plus
  -EINVAL from exactly that. Consider a distinct entry kind from the start
  (22's `merged` bitmap) rather than "borrowed but adjusted". It calls
  `alloc_page(GFP_KERNEL)` from the `->read_sock` actor under the socket
  lock. That is legal in process context, but say so.
- **Message:** the measurement paragraph ("NFSD-on-XFS loan assertion rig",
  "10/10 connections") and the raw bvec triple are lab narrative; dropped.
  The subject is reworded so it does not use "locked-head", which is
  shorthand.

## 22 a744a283d962 SUNRPC: Count a merge page's bytes once when a TCP receive record is materialized

- **Sequencing:** `Fixes: b5b0f1a837b5` points inside the series. **Fold it
  into 21**; upstream would never see the bug. The new message is written
  for the case where it stays separate.
- **Merit:** a necessary fix. Once folded, 21 should introduce the `merged`
  bitmap directly.
- **Message:** names the host and build ("tardis1 (7.1.13-20.hs.457 ...)").
  Dropped, and the trigger is described generically. The backtrace is kept,
  since it is useful if this stays separate.

## 23 1af3854fa0a9 nfsd: accept receive bvecs for NFSv4 COMPOUND

- **Sequencing:** belongs right after 11 (NFSv3 WRITE) or right after 12. It
  has nothing to do with 17-22.
- **Split:** (a) `svcxdr_savemem()` HIGHMEM handling, (b)
  `nfsd4_decode_layoutreturn4()` saving `lrf_body`, (c) setting
  `.pc_xdr_bvec = true` on COMPOUND. (b) looks like a pre-existing upstream
  bug independent of bvecs: `xdr_inline_decode()` can return the scratch
  buffer whenever the body crosses a page in `rq_pages`, and the next decode
  overwrites it. If so, it wants its own commit with a `Fixes:` on the
  commit that added `lrf_body` decoding and a stable Cc, and it should go
  early in the series or separately.
- **Merit:** turning bvec decoding on for all of COMPOUND is a broad claim
  ("Audit all 73 ... decoder slots"). Chuck will want evidence in the form
  of a statement of what invariants decoders may rely on, not "audited".
  The `IS_ENABLED(CONFIG_HIGHMEM) && page_kaddr` test copies every item
  decoded from a mapped page on HIGHMEM, which is fine.
- **Message:** stale and inaccurate. It claims a SETXATTR fix that is not in
  the diff (already in base as `8fcdabd5ffff`). It is written as an audit
  report ("no allocation, payload copy, lock, atomic, syscall, or HIGHMEM
  load/branch"), which is neither a problem statement nor a description of
  the change. Rewritten.
- **Authorship:** the author/SoB address
  `ronaldo@ronaldo-mbp.tail83ec88.ts.net` is a machine hostname on a
  private tailnet. Upstream requires a real, reachable address for DCO; get
  the author's proper address before posting (the same applies to 11 and
  12). *Done 2026-10-15: all twelve of his commits now carry
  `ronaldo@slipstream-io.com`.*

## 24 f5426ad04d24 SUNRPC: add a runtime switch to disable TCP receive page loans

- **Sequencing:** fold into 12, or put it immediately after 12. Its
  `[snitzer:]` note is purely a placement instruction; drop it.
- **Merit:** upstream concerns:
  - The loan path is on by default for every eligible plaintext call on
    every NIC. That contradicts the series' framing that copying is the
    default and loans are for storage that takes arbitrary pieces. A new,
    intricate receive mode in svcsock would be expected to be opt-in.
    Consider default N, or a per-namespace sysctl or netlink knob next to
    the region budget sysctls added later (38).
  - A module parameter for a per-namespace service setting is legacy style.
    Chuck has been moving nfsd/sunrpc configuration to netlink. With the
    budget sysctls arriving in 33/38/43, there are two overlapping controls
    for "copy instead of loan". Pick one.
- **Message:** good apart from the note; "field mitigation" wording trimmed.

## 25 47a1390e23b5 page_pool: Fixed receive-buffer region for one NIC; recycle loaned pages

- **Split:** the `net/sunrpc/svcsock.c` part (`svc_tcp_rx_get_borrowed()`
  and `svc_tcp_rx_put_borrowed()` taking and dropping page-pool references,
  `walk->pp_recycle`) is a SUNRPC change that fixes 12: `get_page()` on a pp
  page makes `page_pool_return_netmem()` release the page from the pool when
  the skb is freed. It belongs in 12 or in a separate `SUNRPC:` commit right
  after 12, not in a page_pool commit. The proposed message keeps it
  described in case the split is not done.
- **Merit (page_pool maintainers' view), harsh:** in this form this would be
  NAKed.
  - Device selection by module-parameter string matching in core page_pool
    (`rx_region_dev`, `page_pool_rx_region_get()` matching on the parent
    `dev_name()`, else the netdev name, else the DMA device) is a layering
    violation. Per-queue memory configuration now goes through netdev
    netlink queue config and **memory providers** (`pp_memory_provider_params`,
    `mp_ops`, as devmem TCP and io_uring zcrx use). The expected shape is a
    memory provider bound to the receive queues, configured over netlink, not
    a global singleton keyed by a name string.
  - A global single region (`static struct page_pool_rx_region *rx_region`)
    that one first pool allocates on its NUMA node, with `alloc_contig_pages()`
    and no free path (fixed only in 29).
  - The motivation, physical memory a platform can keep cache-resident, is
    platform-specific. Nothing in the kernel locks that range, so in mainline
    the feature only confines receive buffers to a contiguous range. The
    justification has to stand without the out-of-tree cache locking, or this
    stays out of tree.
  - Writable debugfs with "any write resets min_free" is a debugging aid.
    page_pool stats and the netlink page-pool API are where pool statistics
    go.
  - `page->pp_magic = 0` initialization assumes pp_magic layout details.
  - Silently losing pages ("escaped") permanently shrinks the region. Upstream
    would ask what bounds that.
  - Loans holding page-pool references for the duration of a storage write
    delay `page_pool_destroy()` (the release-retry warning loop) on interface
    down. This is the classic objection to non-network subsystems holding pp
    pages long-term; expect it.
- **Message:** unacceptable as is: "Origin: ours", "Authors/attribution", AI
  co-author text, "Slipstream overlay ... series 16/20", "navigator page",
  "delivery package", "On the target platform ... L3". It also says "Two
  changes in one overlay:" and then lists only one. The KUnit `[snitzer:]`
  note is dropped. Rewritten, with the platform motivation stated
  generically.

## 26 d79138c90f5c page_pool: Add page_pool_page_in_rx_region()

- **Sequencing:** no user until 34. Move it next to 34, or into the page_pool
  commit that lands immediately before 34's SUNRPC user. Netdev does not
  take helpers without users.
- **Merit:** a small inline. It reaches into `struct page_pool` from
  helpers.h, and using `struct page` rather than netmem goes against the
  current netmem conversion direction. Expect "take a netmem_ref".
- **Message:** fine; lightly edited.

## 27 aedf6fd4a714 page_pool: Let several devices share the fixed receive region

- **Sequencing:** fold into 25; there is no reason to merge a single-device
  version first.
- **Merit:** comma-separated device lists in a 256-byte module_param_string,
  parsed with `strsep` on every pool creation
  (`page_pool_rx_region_listed()`), make the module-parameter design problem
  above worse. One region shared by devices that may sit on different NUMA
  nodes, allocated on the first pool's node, is unaddressed.
- **Message:** "on-die region" and "high-performance NIC" are mild
  platform-speak; trimmed.

## 28 5de11c15ec41 page_pool: Export page_pool_rx_region_size()

- **Sequencing:** an exported symbol with no user until 38. Move it next to 38
  or fold it into 25.
- **Merit:** returning the configured size when no region exists conflates
  "configured" and "allocated". A caller sizing budgets against a region that
  failed to allocate gets a non-zero answer. Taking a mutex for a read of one
  word is heavier than needed. The concept, another subsystem partitioning a
  NIC's page-pool memory, reinforces the layering concern in 25.
- **Message:** fine.

## 29 387fa26ffad4 page_pool: Give the fixed receive region back when it is resized

- **Sequencing:** fold into 25. It includes a real fix to 25
  (`page_pool_rx_region_unbind()` in the `page_pool_create_percpu()` error
  path), and "never freed" should not be merged in the first place.
- **Merit:** "resize by taking every listed device down, writing the
  parameter, bringing them up" is an operational procedure built on a module
  parameter, which reinforces that this wants a netlink/memory-provider
  interface. `page_pool_rx_region_reap()` runs on every pool creation for a
  listed device; that is fine. "Budgets set against the old size are not
  checked again" refers to SUNRPC budgets that do not exist yet in the
  series and is a known hole, so note it in the SUNRPC budget commit
  instead.
- **Message:** contains "Draft: built with W=1 against the 7.1.13-21 16 KiB
  config and through checkpatch; not booted." This must go, and it also
  breaks the tree's own rule against state-of-work disclaimers. The sentence
  about budgets is removed.

## 30 365ad6e8cab5 page_pool: Recycle a region-bound pool's pages last in, first out

- **Sequencing:** fine as a separate commit after the region. It is a
  distinct behaviour change with its own justification.
- **Merit:** the netdev maintainers will push back. The ptr_ring exists so
  that producers and the NAPI consumer do not share a lock. This makes the
  consumer take `ring.producer_lock` once per refill. It reuses the "ring" and
  "ring_full" stat names for a stack, which is misleading in
  `page_pool_get_stats()` output. It adds three fields to `struct page_pool`
  for every pool. It changes `page_pool_empty_ring()` to lock per page. They
  will ask why LIFO should not be the generic answer (cache warmth argues the
  same for any pool), or why not keep the ring and fix the remote-free path
  instead. The NUMA-preference check in `page_pool_refill_alloc_cache()` is
  skipped for the stack path. That is harmless for a single-node region, but
  it needs saying.
- **Message:** "With nfsd reading sixteen connections on other CPUs than the
  four receive rings', 41.08 million of 41.1 million pages ..." is a lab
  measurement. Replaced with the generic statement that nearly every page
  returns through the ring when the consumer runs off the NAPI CPU.

## Summary of recommended actions (17-30)

| Pos | Action |
|-----|--------|
| 17 | Move before 11/12, retitle `nfs_common:`, combine with the nfs_dio.c change now inside 11 |
| 18 | Keep; move with 17 ahead of the SUNRPC series; simplify the encoding |
| 19+20 | Fold 20 into 19; move next to 1-6 |
| 21+22 | Fold 22 (in-series Fixes:) into 21 |
| 23 | Split into savemem-HIGHMEM, layoutreturn (likely an upstream fix with Fixes:), enable COMPOUND; move after 11/12; fix stale message; author address |
| 24 | Fold into 12 or place right after; reconsider default Y and module-param vs sysctl/netlink, overlap with budget 0 |
| 25 | Split SUNRPC pp-ref half out (into or after 12); fold 27 and 29 into the page_pool half; expect NAK without a memory-provider/netlink design |
| 26 | Move to its first user (34) |
| 27 | Fold into 25 |
| 28 | Move to its first user (38) or fold into 25 |
| 29 | Fold into 25; drop "Draft ... not booted" |
| 30 | Keep separate; trim the measurement; expect pushback on the lock and stats naming |
