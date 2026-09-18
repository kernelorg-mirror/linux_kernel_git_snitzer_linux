# The loaned-DIO -EINVAL, root-caused (2026-09-09)

Answers Chuck Lever's review questions on "[PATCH 4/4] nfsd: fall back to
buffered I/O when a direct write gets -EINVAL"
(<995a853c-2c54-4bd0-9708-f6e0c1be7fd5@slotpi15m67>), and corrects the
patch's own mechanism story — see TESTING.md's "16K runtime run and the
loaned-DIO EINVAL finding" for what this supersedes. Outcome: patch 4/4
withdrawn on-list (<aqGL5dh49toktgBI@kernel.org>, 2026-09-09); the real
fix belongs in the page-loan series.

Data from an instrumented rerun on the 4K Hammerspace host
(`7.1.8-3.hs.159.el9.aarch64+`, loaded nfsd/brd srcversions match the
tree objects — loan + brd fix + EINVAL fallback all in). Probe:
`split_einval.bt` (kprobe/kretprobe pair on `bio_split_io_at()`, dumps the
rejected bio's `bi_iter` and the queue limits); raw capture:
`split_einval.log`.

## The A/B

`run-rig-cmp.sh`, 30 connections x 16 x 1 MiB O_DIRECT client writes,
brd-backed nvme-loop XFS export, `io_cache_write=2`:

| run | svc_tcp_rx_loan_pages | direct | vector | bio_split_io_at -EINVAL | dd/cmp |
|---|---|---|---|---|---|
| 1 | N (copied arena) | 480 | 0 | **0** | clean |
| 2 | Y (loaned) | 465 | 27 | **12** | clean (fallback absorbed all 12) |

Run 2 accounting: 480 writes = 465 direct + 15 gate-rejected (vector only)
+ 12 direct-then-fallback (both tracepoints, same offset) = 465 + 27
trace lines. The 12 fallbacks correspond 1:1 to the 12 captured
rejections.

Queue (nvme loop ns): lbs=512 phys=4096 max_sectors=2040 max_segments=256
virt_boundary_mask=4095 dma_alignment=3. File: stx_dio_mem_align=4,
**stx_dio_offset_align=512 == logical_block_size** (XFS reports the bdev
lbs; fs blocks are 4K).

## Which -EINVAL (Chuck's Q1)

**The zero-length-after-ALIGN_DOWN() branch, all 12 times.** The per-bvec
`bv_offset/bv_len vs dma_alignment` test cannot fire for an iterator the
nfsd gate admits on this stack: mask is 3 and page tiling keeps every
offset/length a multiple of 4. (TESTING.md's 2026-09-05 "one or the
other" hedge is now resolved.)

## The actual mechanism — interior discontinuity, not the first bvec

All 12 captures fit one reconstruction. Example (first event):

- Loaned 1 MiB payload, bv0=(408,3688), then page-tiled (0,4096) bvecs —
  but the payload is **multi-run**: bvec 64 starts mid-page (loaned bvecs
  mirror skb fragment geometry). On nvme (virt_boundary_mask=4095) that
  discontinuity is a segment gap, at payload byte 3688+63*4096=261736 —
  **not a multiple of lbs=512**.
- Split 1: walk reaches the gap with bytes=261736, rounds down:
  ALIGN_DOWN(261736,512)=261632 (511 sectors, `@rets[511]`), splits
  mid-bvec-63. Succeeds.
- Remainder bio: starts at bi_idx=63, bi_bvec_done=3992 — a **104-byte
  pre-gap residue** — then the gap. Its split walk hits the gap with
  bytes=104: ALIGN_DOWN(104,512)=0 → **-EINVAL** (`bio_split_io_at()`'s
  "virtual boundary gaps without a valid block sized split").

Captured remainder iterators (three bursts x four writes):
size=786944/525312/263168/1536 at idx=63/127/191/255, bvec_done
3992/3692/3904/3604 — each a different write in the burst failing at its
first misaligned run boundary.

Corollaries:

- **The first bvec is innocent.** Run 1's copied payloads have the exact
  "suspect" first-fragment shape (mid-page start, length not a sector
  multiple) and did ~960 mid-bvec splits at 2040 sectors with zero
  rejections and cmp-clean data (patched brd). Contiguous runs always
  have a valid split point.
- Chuck's proposed gate (first bvec's remaining length % offset_align)
  rejects the observed bursts only **coincidentally** — it keys on bv0
  while the rejection is caused by interior run boundaries. It
  over-rejects (contiguous mid-page payloads that split fine) and
  under-rejects (a page-aligned-anchor payload with a misaligned interior
  discontinuity still gets -EINVAL).
- **Upstream NFSD cannot currently produce the failing shape over TCP**:
  svc_tcp receive copies into rq_pages, so rq_bvec is always one
  contiguous page-tiled run. The failure requires the receive page-loan
  geometry. Patch 4 as posted defended upstream against a payload shape
  it can't yet see — hence its withdrawal.

## Consequence for the loan series

- Upstream fixes series is now patches 1–3 (1–2 the stable candidates);
  patch 4 withdrawn.
- **Done in the 2026-09-09 rebase onto `v7.1.8-5`** (branch
  `kernel-7.1.8/main-5.NFSD_TCP_WRITE_ZEROCOPY`): the gate landed
  in-series as "nfsd: require interior payload discontinuities to be
  logical-block aligned" — `nfsd_dio_iter_is_splittable()` walks the
  direct segment's fragments and requires every interior discontinuity
  (fragment not ending at a page boundary, or successor not starting at
  offset zero) to land on an offset_align multiple; otherwise the
  segment is demoted like a memory-alignment failure. Single-run
  payloads — everything upstream today — pass unchanged. O(nvecs), only
  data nfsd already has. Ordered ahead of the loan commits so no point
  in the series can send a discontiguous payload to the block stack.
  KUnit coverage: the three `discontinuity-*` cases in
  `nfsd_bvec_dio_segments_test`.
- The -EINVAL fallback was **dropped in the same rebase** (not carried,
  not reverted — the rebase simply doesn't pick it): Chuck's objection —
  -EINVAL from `->write_iter` is ambiguous, and a real filesystem
  -EINVAL costs a doubled full write attempt — applies to this tree as
  much as upstream, and the gate makes the fallback unreachable.
