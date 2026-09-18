# The loaned-DIO -EINVAL, root-caused (2026-09-09)

Answers Chuck Lever's review questions on "[PATCH 4/4] nfsd: fall back to
buffered I/O when a direct write gets -EINVAL"
(<995a853c-2c54-4bd0-9708-f6e0c1be7fd5@slotpi15m67>), and corrects the
patch's own mechanism story. Data from a fresh instrumented rerun on this
host (4K `7.1.8-3.hs.159.el9.aarch64+`, loaded nfsd/brd srcversions match
the tree objects — loan + brd fix + EINVAL fallback all in). Probe:
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

Queue (nvme1n1, loop): lbs=512 phys=4096 max_sectors=2040 max_segments=256
virt_boundary_mask=4095 dma_alignment=3. File: stx_dio_mem_align=4,
**stx_dio_offset_align=512 == logical_block_size** (XFS reports the bdev
lbs; fs blocks are 4K).

## Which -EINVAL (Chuck's Q1)

**The zero-length-after-ALIGN_DOWN() branch, all 12 times.** The per-bvec
`bv_offset/bv_len vs dma_alignment` test cannot fire for an iterator the
nfsd gate admits on this stack: mask is 3 and page tiling keeps every
offset/length a multiple of 4. (TESTING.md's "one or the other" hedge is
now resolved.)

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
  contiguous page-tiled run. The failure requires the (unposted) receive
  page-loan geometry. Patch 4 as posted defends upstream against a
  payload shape it can't yet see.

## Consequence for the series / the loan work

- v2 of the upstream fixes: **drop patch 4** (and fix patch names per
  Chuck: the gate is `nfsd_write_dio_iters_init()`, `bio_iov_iter_set()`).
  Patches 1-3 stand alone; 1-2 remain the stable candidates.
- The real gate belongs in the loan series, at DIO admission: walk the
  payload bvecs, track payload position, and require every interior
  discontinuity (bvec not ending at page end, or successor not starting
  at 0) to land on an offset_align multiple; otherwise no_dio
  (DONTCACHE-buffered). Single-run payloads — everything upstream today —
  pass unchanged. O(nvecs), only data nfsd already has, and it subsumes
  the -EINVAL fallback.
- TESTING.md's 16K-finding section needs an update on the doc branch: the
  mechanism is discontinuity gaps (page-size-independent, reproduced on
  the 4K host once the loan is enabled); the per-bvec-vs-ALIGN_DOWN
  ambiguity is resolved; "all-or-nothing per connection" was approximate
  (4 of 16 writes failed per affected connection here).
