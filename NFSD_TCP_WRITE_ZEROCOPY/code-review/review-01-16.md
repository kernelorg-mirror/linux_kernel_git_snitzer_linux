# Review of positions 01-16 (kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY-code on v7.3-3)

Reviewer stance: Chuck Lever (NFSD/SUNRPC), Christoph Hellwig (block/fs/VFS),
Eric Dumazet / Jakub Kicinski (tcp).  Proposed messages are in new-01.txt ..
new-16.txt.

## Series-level observations for this stretch

1. **Loans are switched on unconditionally at 12 and stay on until 24.**
   After `de24cd7a2cde` every NFSv3 WRITE over plaintext TCP with AUTH_SYS
   holds NIC receive pages (including page_pool pages) for the life of the
   request.  The only way to turn it off ("runtime switch to disable TCP
   receive page loans") is at 24, and the region/budget/flow control that
   keeps a slow WRITE from draining the NIC's pools arrives at 25-39.  The
   cover story ("copying remains the default") is false for 12-38 of a
   bisect.  Upstream expects the opposite order: introduce the mechanism
   disabled (or behind the switch), then the protections, then enable.  At a
   minimum move the switch (24) before 12 and make it default off, or fold
   it into 12.

2. **Fixes: tags inside the series.**  13, 14, 15, 16 all carry `Fixes:` to
   11 or 12.  None of those targets is upstream, so each of these must be
   folded into its target before posting:
   - 13 (rq_maxbvecs) + 16 (loan table sized for fragments) + 15
     (header-only record published as COPY) -> fold into 12.
   - 14 (nfsd_vfs_write bound by rq_maxbvecs) -> fold into 12 together
     with 13 (it depends on 13's `rq_maxbvecs`; its Fixes target 11 is
     wrong in spirit, the bug only exists once 12 can produce sub-page
     fragments).
   - 10 (poison invalid decode streams) is an unmarked fix to 9 -> fold.
   If any of these stay separate, note that new subjects proposed for 11
   and 12 would make the `Fixes:` subject strings in 13-16 stale; folding
   avoids the problem.

3. **The tcp commit (07) is in the wrong place.**  Its only user is 44
   (`f3effc4e538c` "Hold a region transport's receive window through TCP's
   interfaces").  netdev will want it posted with its user, and its message
   currently argues from in-series history.  Move it immediately before 44
   (and fold 44 into the commit that introduced the code 44 replaces, so
   upstream never sees SUNRPC writing `tp->scaling_ratio`).

4. **01 is unrelated to the rest and should leave the series.**  It changes
   the DMA alignment of every MD/DM device in the kernel, its own message
   says it was not audited against md raid1/10/456, drbd and a list of dm
   targets, and its motivation (direct writes to an MD stripe) no longer
   applies to how this work is deployed.  Post it on its own to linux-block,
   dm-devel and linux-raid after the audit, or drop it.

5. **01-06 (statx/block/xfs/loop) are an independent sub-series** with their
   own audience (linux-block, linux-fsdevel, linux-xfs, linux-api,
   man-pages).  They are only consumed at 17-20.  Upstream they need to be
   posted separately (and merged through the VFS/block trees) before the
   nfsd parts; in this branch they would read better directly before 17-20
   than in front of the SUNRPC loan work.  A new statx field also needs a
   man-page patch, a Cc to linux-api, and a test (xfstests/LTP).

6. **Dependencies on non-mainline code.**  11 changes
   `fs/nfs_common/nfs_dio.c` (`nfs_dio_iter_aligned()`), which exists only
   in the downstream v7.3-3 base (`771712d97854` "nfs_common: share direct
   I/O write split..."), not in mainline v7.3-rc7.  The series cannot be
   posted until that base is upstream, or 11 has to be rebased onto
   mainline nfsd's direct-write code.

7. **Ronaldo's author/Signed-off-by address**
   (`ronaldo@ronaldo-mbp.tail83ec88.ts.net`) is a Tailscale host name, not a
   deliverable mail address.  A DCO sign-off with it will not be accepted.
   It is kept verbatim in the proposed messages as instructed, but it must
   be replaced with a real address by its owner before posting (11, 12, and
   23 later).

8. **Size and style of 09 and 12.**  9 is ~470 lines in xdr.c, 12 is
   ~1000 lines in svcsock.c plus 140 lines of trace events.  Chuck will ask
   for these to be split, and will object to the amount of defensive
   bookkeeping (details below).  "Authoritative" as a name for "bvec-backed"
   will not survive review.

---

## 01 8ce3b8845deb block: Let a stacking device take its members' memory alignment

**Sequencing.**  Independent of everything else in the series; nothing in
02-61 requires it to build.  It is not part of the segment-boundary work
(02-06) either.  Drop from this series (see series point 4).

**Upstream merit.**  The idea is defensible: `blk_stack_limits()` already
takes `max()` of members' `dma_alignment`, and `blk_validate_limits()`
already supplies `SECTOR_SIZE - 1` when the value is 0
(`block/blk-settings.c:502`), so starting the stacking device at 0 is
consistent.  But it is a global behaviour change for every bio-based
stacking driver that touches data itself.  md raid1 (write-behind copies),
raid10, raid456 (xor/stripe cache through `async_memcpy`/bounce), dm-
writecache, dm-ebs, dm-vdo, dm-thin/cache via kcopyd, drbd: any of them
may assume 512-byte aligned bio memory.  Christoph and Mikulas will want
that audit in the message, not an admission that it was not done.  The
comment added in `blk_set_stacking_limits()` is longer than the code it
replaces; a one-liner suffices.  The subject is capitalised after the
prefix ("Let"), unlike block's usual lower case.

**Message.**  Overdone and inappropriate: host name (tardis1), WRITE counts,
"Draft, not booted; built with W=1 against the 7.1.13-21 16 KiB config",
and a list of unaudited drivers.  The nfsd-specific motivation (loaned
receive pages on 4-byte boundaries) is not the block maintainers' problem;
the generic one (STATX_DIOALIGN on an MD/DM device is stricter than its
members) is.

## 02 c2882fc1209c block: add bdev_dio_seg_boundary()

**Sequencing.**  Fine as a preparatory patch, but Christoph usually asks
that a helper be added with its first user; consider folding into 04 (or
introducing it in 03 with the field).

**Upstream merit.**  Reasonable, but:
- The `bool` + out-parameter signature is awkward.  Returning a `u32` with a
  sentinel, or having the callers just not report when the mask is
  "odd", would be simpler.  Is a non-`2^k-1` virt boundary actually
  possible?  `blk_validate_limits()` does not check it; ublk accepts it
  from user space.  If it can't happen for real hardware, a `WARN_ON_ONCE`
  in `blk_validate_limits()` would be the better fix than every reporter
  checking.
- Use `bdev_limits(bdev)->virt_boundary_mask` rather than
  `queue_virt_boundary(bdev_get_queue(bdev))`, in line with recent block
  code.
- Expect the question whether exposing virt_boundary is still wanted given
  Keith Busch's recent work relaxing direct I/O vector alignment and
  splitting at gaps in the block layer.  The message should say why
  callers need to know rather than let the block layer handle it (the
  answer: a split that cannot fall on a logical block fails with -EINVAL).

**Message.**  Adequate; slightly tightened in new-02.

## 03 29df8cf690dd fs: add STATX_DIO_SEG_BOUNDARY

**Sequencing.**  Correct place within 02-06.

**Upstream merit.**  A new UAPI field needs: linux-api Cc, a man-pages
patch, an xfstests or LTP test, and a user-space consumer argument (the
in-kernel consumer is nfsd/LOCALIO via `vfs_getattr()`; loop in 06 is a
better example).  Using the last `__spare2` word is fine.  The comment
"otherwise assume the page size" is a policy statement in UAPI that
Christoph may contest: an application that does not request the bit today
assumes only `stx_dio_mem_align`.
The `tools/include/uapi/linux/stat.h` and
`tools/perf/trace/beauty/include/uapi/linux/stat.h` hunks should be dropped:
the perf maintainers ask that kernel patches not update the tools/ copies,
they resync them themselves.  The `struct kstat` comment
"virt_boundary_mask + 1 (gap rule)" leaks block internals into a VFS
structure; describe it in statx terms.

**Message.**  Good.  Keeps a little too much detail that duplicates the UAPI
comment, but acceptable.

## 04 32e78ebdc08c block: report STATX_DIO_SEG_BOUNDARY for block devices

**Sequencing / merit.**  Fine.  Trivial and correct.  Could absorb 02.

**Message.**  Fine; essentially unchanged.

## 05 c5b71babaeea xfs: report the direct I/O segment boundary

**Sequencing / merit.**  Fine.  Darrick/Christoph may ask whether the
value should be reported for all files or only those where direct I/O is
possible (DAX, inline data), mirroring how `xfs_report_dioalign()` handles
the other fields; currently it is set unconditionally whenever any DIO bit
is requested.  ext4 and other filesystems that report STATX_DIOALIGN are
not covered; reviewers will ask whether a generic helper (or
`generic_fillattr`-level reporting from `sb->s_bdev`) would be better than
per-filesystem patches.

**Message.**  Fine; unchanged apart from wording.

## 06 d2a699bdf360 loop: take the backing device's virtual boundary for direct I/O

**Sequencing.**  This mixes two things: (a) a real, pre-existing loop bug
(direct I/O over a backing device with a virtual boundary can fail with
-EINVAL), which only needs `queue_virt_boundary(sb_bdev)` and is a natural
follow-up to Keith Busch's mainline `6c8dec275ccc` ("loop: set
dma_alignment from the backing file for direct I/O"); and (b) using the new
STATX_DIO_SEG_BOUNDARY.  Split it: (a) first, standalone, with a
`Fixes: 6c8dec275ccc` (or to the original DIO support) and a stable Cc;
(b) after 03-05.

**Upstream merit.**  Good fix.  Nits: the `is_power_of_2()` check repeats
what the statx contract already promises; `lo_dio_virt_boundary` is
computed from `sb_bdev` before `vfs_getattr()` and then overwritten, which
reads oddly (compute the fallback in the fallback branch).  Resetting
`max_segment_size = 0` changes the buffered-mode value from whatever it was
to `BLK_MAX_SEGMENT_SIZE`; say so or confirm it is the existing value.

**Message.**  Problems: it cites "commit cdb436f165a4 ("nfsd: split direct
writes by the device's segment boundary")", which is a stale hash for an
unmerged, *later* commit in this series (now 20, `0dfe5616ef63`); it talks
about nfsd and LOCALIO, which block maintainers do not need; it says "the
loop device reports its limits through STATX_DIO_SEG_BOUNDARY", which is
04's doing.  The `[snitzer: rebased onto v7.3-3 ...]` note is internal
rebase bookkeeping; drop it.

## 07 9fda9805446b tcp: let a kernel socket cap the receive scaling ratio

**Sequencing.**  Its only user is 44.  Move it next to 44 (series point 3).

**Upstream merit (netdev).**  Expect strong push-back:
- Eric's first answer will be that `skb->truesize` is supposed to describe
  the memory an skb costs.  If every skb pins whole pages of a bounded
  pool, the driver (or page_pool region) should report a truesize that
  says so, and TCP's learned ratio is then correct with no new knob.  The
  commit has to explain why that is not possible.
- `scaling_ratio_max` is read in `tcp_measure_rcv_mss()`, an RX fast path,
  but placed in the cold part of `struct tcp_sock`
  (next to `keepalive_probes`).  The `tcp_sock_read_rx`/`write_rx`
  cacheline groups exist precisely for this; Eric will ask to place it in
  the RX group (and `tcp_struct_check()` assertions may need updating).
- Data race annotations: the setter writes `scaling_ratio` and
  `scaling_ratio_max` under `lock_sock()` while other readers use
  `READ_ONCE()` on `window_clamp`; expect requests for
  `READ_ONCE`/`WRITE_ONCE` consistency.
- `TCP_RMEM_TO_WIN_SCALE` units leak to callers as a raw `u8`; an API that
  takes a payload/truesize fraction or a per-skb overhead would be easier
  to justify.
- No in-tree user in the same posting = automatic NAK.

**Message.**  Describes SUNRPC's earlier implementation ("rewrote
tp->scaling_ratio and tp->window_clamp from its data_ready callback on every
skb"), which upstream reviewers will never see; replace it with a generic
problem statement (done in new-07).

## 08 4770d94a72f6 SUNRPC: distinguish authoritative XDR bvecs

**Sequencing.**  Data-structure-only prep with no user.  Acceptable, but
Chuck would likely want 08+09(+10) as one patch or at least 08 folded into
09, since 08's `-EOPNOTSUPP`/no-op guards only make sense alongside 09.

**Upstream merit.**  The design question Chuck will ask first: why add a
second representation to `struct xdr_buf` instead of finishing the move of
`xdr_buf` page data to bvecs, which SUNRPC has been heading towards
(`rq_bvec`, `xdr_buf_to_bvec()`)?  Two modes means every page-array helper
grows a mode check (09 adds ~25).  "Authoritative" is not a term anyone
upstream uses; "bvec-backed" is clearer.  `enum` in `struct xdr_buf` plus
two `unsigned int`s grows every `xdr_buf` (embedded in `svc_rqst`,
`rpc_rqst`) by 12 bytes.  The struct comment is inside the `flags` define
block, which reads badly.

**Message.**  "This is representation and ownership plumbing only ... no
allocation, copy, synchronization, or payload traversal" is boilerplate;
one sentence "No caller selects the new mode yet" is enough.

## 09 a3b821de267c SUNRPC: decode XDR from authoritative bvecs

**Sequencing.**  Correct order after 08.  10 should be folded in.

**Upstream merit.**
- Hot-path cost: adds a `page_mode` branch to `xdr_reserve_space()`,
  `xdr_inline_decode()`, `__xdr_commit_encode()`, `xdr_truncate_encode()`
  and others that run for every RPC, to guard against a mode that the
  encode side never uses.  Chuck will want the encode helpers left alone
  (or a `WARN_ON_ONCE` in one place), not silent no-ops: a void function
  such as `xdr_write_pages()` or `xdr_inline_pages()` that silently does
  nothing on a bvec buffer turns a bug into corrupt replies.
- `xdr_buf_validate_page_mode()` walks the whole bvec array, and it is
  called from `xdr_init_decode()`, `xdr_buf_subsegment()` and
  `xdr_buf_to_bvec()`.  For a 1 MB WRITE that is hundreds of entries
  revalidated several times per request (subsegment is also reached via
  `read_bytes_from_xdr_buf()` from the DRC checksum in 11).  The producer
  (the transport) should validate once; consumers should trust the buffer.
  The message's "O(1)" claim is about stepping, not about this.
- `xdr_set_bvec()` stores `&bvec->bv_page` in `xdr->page_ptr`
  (`struct page **`) and later recovers the bvec with `container_of()` in
  `xdr_set_next_page()` and `xdr_set_next_authoritative_buffer()`.  That is
  a type pun Chuck will not accept; add a `const struct bio_vec *` cursor
  to `struct xdr_stream` instead.
- `xdr_buf_subsegment()` for BVECS mode sets `subbuf->pages = buf->pages`
  (meaningless in that mode) and duplicates its bounds check.
- `xdr_copy_authoritative_to_scratch()` duplicates `xdr_copy_to_scratch()`;
  the existing function could be generalised.
- Naming: `*_authoritative_*` throughout.

**Message.**  Overdone ("The authoritative hot path adds one representation
branch ... no allocation, locking, atomics, or syscalls. Direct scalar reads
copy zero bytes ...") and carries two internal notes that must go: the
`[p718 backport to v7.1.8: ... bi_bvec_done ...]` note (internal history on
an old base) and the `[snitzer: rebased onto v7.3-3 ... bi_offset]` note
(upstream already has `bi_offset`, nothing to record).

## 10 b3dd38ff8b16 SUNRPC: poison invalid XDR decode streams

**Sequencing.**  A one-line fix to 09's new code.  Fold into 09.

**Upstream merit.**  Correct but trivially part of 09.

**Message.**  Fine as is (new-10 is unchanged); moot once folded.

## 11 23970ee63bc5 nfsd: accept immutable receive bvec requests

**Sequencing.**  Mixes at least four independent changes across three
subsystems under an `nfsd:` subject:
1. `nfs_dio_iter_aligned()` (fs/nfs_common): check each entry's covered
   length, not only its start.  A standalone correctness change; own
   commit, before 12.
2. `nfsd_cache_csum()` rewritten on `read_bytes_from_xdr_buf()`.  A nice
   cleanup on its own (removes open-coded page walking); own nfsd commit.
3. `svc_proc_lookup()` factored out of `svc_generic_init_request()`, plus
   `svc_proc_accepts_xdr_bvec()` and `pc_xdr_bvec`: SUNRPC, and only used
   by 12.  Belongs with 12 or as a SUNRPC prep commit immediately before it.
4. Setting `.pc_xdr_bvec = true` on NFSv3 WRITE: the actual nfsd opt-in.
Split accordingly.

**Upstream merit.**
- Using `rqstp->rq_res_stream.p` (the reply buffer) as 256 bytes of
  scratch for the checksum, justified by a `static_assert` on
  `PAGE_SIZE`, is a hack Chuck will reject.  `svc_rqst` already has
  `rq_scratch_folio`; or a small on-stack buffer for 256 bytes.
- `svc_proc_lookup()` is `EXPORT_SYMBOL_GPL` but its only caller is in
  sunrpc.ko itself (`svcsock.c`).  Remove the export (it appears to exist
  only for the test module).
- `#include "xdr3.h"` added to `fs/nfsd/vfs.c` is unused, here and at the
  tip of the branch.  Drop it.
- `pc_xdrressize` turned into a 31-bit bit-field to make room for a bool:
  unnecessary churn; `struct svc_procedure` has room for a plain `bool`.
- The auth flavor policy (AUTH_NULL/AUTH_UNIX only) is hard-coded in a
  `svc.h` inline; say why (GSS integrity/privacy unwrap in place), and
  consider whether TLS should be part of the same predicate rather than a
  separate check in svcsock.c.
- Message mentions `nfsd3_procedure()` "for tests" and
  `nfsd_dio_iter_is_aligned()`; neither exists in the diff (stale).

**Message.**  Inadequate structure (lists many things with no "why"), stale
function names, a test reference, a "no ... lock, atomic, syscall ..."
boilerplate sentence, and two `[snitzer: ...]` notes: the first describes a
real code change (the alignment check) and is folded into the body; the
second (an include already present on v7.3-3) is rebase bookkeeping and is
dropped.  Author address problem: see series point 7.

## 12 de24cd7a2cde SUNRPC: loan eligible TCP receive pages

**Sequencing.**  Turns loans on for everyone (series point 1).  13, 15 and
16 fix it and should be folded in.  It should be split for review, e.g.:
receive-state object and its hand-off between socket and request;
header classifier; skb walker/borrowing; fallback/materialize; tracepoint.

**Upstream merit.**  The concept is what the series is for, but in this
form:
- **Layering.**  `svc_prepare_thread()` in generic `net/sunrpc/svc.c` now
  allocates a TCP-specific `svc_tcp_rx_state` (bvec table plus two bitmaps)
  for every thread of every service, including lockd, the NFS callback
  service and RDMA-only servers.  Chuck will want this allocated by the TCP
  transport (or lazily) and `rq_tcp_rx` not in `struct svc_rqst`.
- **Transport parses RPC header to pick a procedure.**  The TCP transport
  reads program/version/procedure/flavor and scans `serv->sv_programs`
  before dispatch.  Expect "why not always hand the upper layer a bvec
  description and let the decoder decide?"
- **Page references on network pages.**  Borrowing takes raw `get_page()`
  on skb frag pages and on `virt_to_page(skb->data)` heads, and uses
  `page_pool_page_is_pp(compound_head())` to classify.  netdev will object
  to a ULP taking plain page refs on page_pool pages instead of
  `skb_frag_ref()`/netmem references, or simply holding the skbs (which is
  how other in-kernel zero-copy receivers keep data alive).  The page_pool
  bitmap and `page_pool_bytes` here serve only accounting/tracing.
- **Defensive bookkeeping.**  `refs_acquired`/`refs_released`,
  `borrowed_count`, `materialized_bytes`, `page_pool_bytes`,
  `svc_tcp_rx_refs_valid()` (bitmap_weight + bitmap_subset on every
  release), `svc_tcp_rx_validate_mixed()` on every publish, and a
  `WARN_ON_ONCE` in nearly every helper.  This is test-harness scaffolding;
  upstream will ask for it to be removed.  Most of the 17-value
  `svc_tcp_rx_reason` enum exists only for the tracepoint.
- **Public header pollution.**  Three enums and the full state struct are
  put in `include/linux/sunrpc/svcsock.h`; they are private to svcsock.c
  except for the tracepoint.  `____cacheline_aligned` on a kmalloc'ed
  object is pointless.
- **Fragment capacity.**  Even after 13/16 the table is sized for 4 KiB
  pieces.  At a 1500-byte MTU (the common case) a 1 MB WRITE has ~725
  fragments against ~260 entries on a 4 KiB kernel, so it always falls
  back to copying after taking and dropping hundreds of page refs.  The
  sizing should follow the MTU/MSS, or the cover letter should say loans
  target jumbo frames.
- `svc_tcp_recvfrom()` `err_nuts` maps errno values back to reason codes
  with a nested ternary; reviewers will ask for that to be simpler.

**Message.**  Unreadable for upstream: internal labels ("P20-capable
procedure", "P10 authoritative bvecs"), boilerplate claims ("adds no
receive path, syscall, lock, global atomic, skb reference ... per-span
trace, or runtime diagnostic") that are also inaccurate (it adds a
tracepoint per record), and no statement of the problem being solved.
Rewritten in new-12.

## 13 9ac0d9420a83 SUNRPC: size rq_bvec for 4 KiB fragments, not pages

**Sequencing.**  `Fixes:` to 12 (in-series).  Fold into 12.

**Upstream merit.**  `SZ_4K` as the unit is arbitrary (fragments at a
1500-byte MTU are ~1.4 KiB); explain the choice or derive it from the
transport.  `svc_pages_to_bvecs()` is in the generic `svc.h` but encodes a
TCP-receive assumption.  It multiplies `rq_bvec` by 16 on 64 KiB kernels for
every service thread, including those that never lend.

**Message.**  Reasonable; tightened.

## 14 1e3d3c6e424f nfsd: convert a WRITE payload into rq_bvec by its own size

**Sequencing.**  `Fixes:` to 11, but the defect only becomes reachable once
12+13 exist.  Fold into 12 (with 13).  If kept separate, it must come after
13 (it does).

**Upstream merit.**  Correct one-liner.

**Message.**  Fine; subject reworded to say what changes.

## 15 73dd7e4434ef SUNRPC: Publish prefix-only loan records as COPY

**Sequencing.**  `Fixes:` to 12.  Fold into 12.

**Upstream merit.**  Correct.  Calling `svc_tcp_rx_validate_mixed()` just to
switch modes is more of the defensive scaffolding noted for 12.  Subject
capitalises after the prefix and uses internal mode names.

**Message.**  The `[snitzer: split the KUnit cases ... rewrote the commit
message ...]` note is internal bookkeeping; dropped.

## 16 5d2a79a5a31d SUNRPC: size the TCP receive loan table for fragments, not pages

**Sequencing.**  `Fixes:` to 12.  Fold into 12 (with 13).  It also makes 12
pass a page count as `capacity` and then reinterpret it, which is the kind
of churn folding removes: in the folded version `svc_tcp_rx_state_alloc()`
should simply take both sizes.

**Upstream merit.**  Correct given 13; same 4 KiB-unit concern.  The
`arena_pages` field must now also be carried through `svc_tcp_rx_reset()`
and compared in `svc_tcp_rx_exchange()`: more state to keep consistent.

**Message.**  Names the NIC ("an E1 NIC") and gives per-WRITE lab numbers;
removed.  Kept the one generic example (64 KiB pages, 9000 MTU, 20 entries)
because it is the clearest justification.

---

## Summary of recommended changes for 01-16

| Pos | Action |
|-----|--------|
| 01 | Drop from series; post separately after auditing stacking drivers. |
| 02 | Keep (or fold into 04); return-value API simplification. |
| 03 | Keep; drop tools/ hunks; add linux-api, man-page, test. |
| 04, 05 | Keep. |
| 06 | Split: standalone loop fix (Fixes: 6c8dec275ccc) + statx use; fix message. |
| 07 | Move next to its user (44); expect netdev to ask for truesize instead. |
| 08 | Fold into 09 (or keep as prep); rename "authoritative". |
| 09 | Keep, rework (no per-call revalidation, no page_ptr pun, no silent encode no-ops). |
| 10 | Fold into 09. |
| 11 | Split into nfs_common alignment fix, nfscache csum cleanup, SUNRPC lookup prep, nfsd opt-in; drop unused include and export; no reply-buffer scratch. |
| 12 | Gate off by default (move 24 before it); fold 13, 14, 15, 16 in; then split for review; remove debug accounting; reconsider page_pool refs. |
| 13-16 | Fold into 12 (Fixes: within series). |
