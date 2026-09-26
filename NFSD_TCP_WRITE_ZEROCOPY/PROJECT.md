# NFSD TCP write-path zero-copy — technical notes

Deep-dive companion to [`README.md`](README.md): the problem in detail, the
code path, the alignment analysis, the candidate zero-copy-receive direction
and its risks, the Xsight E1 NIC capability findings, and the design decisions
behind integrating David Flynn's page-loan series onto the Hammerspace base.

Origin: handoff from Hammerspace engineer **Jonathan Flynn** (measurements on
the OFP box `tardis1`) plus code verification and risk analysis.

(Code line numbers below are approximate, from the analysis snapshot; the files
are identical across `kernel-7.1.8/main` and the `v7.1.8-3` base.)

---

## Problem statement

On NFS/TCP **writes** with `NFSD_IO_DIRECT`, nfsd copies every byte of received
RPC payload out of the socket skbs into the server's own `rq_pages[]` before
issuing the O_DIRECT storage write. On a DRAM-bandwidth-bound platform that
single receive copy is the dominant write-path cost (`memmove` = 41% of
write-path CPU) and roughly doubles the DRAM bandwidth spent per written byte.

Central question: can the DIRECT write path **reference** the received skb data
in place instead of copying it into `rq_pages`, so the DIO write DMAs the
received bytes directly to storage — and what is the win.

## Measured evidence (Jonathan, tardis1)

- Platform: XSight E1 / OFP, ARM Neoverse-N2 64c single-NUMA, dual 400G `xeu`
  ports on the E1 DPU; 8× WD SN8100 NVMe, XFS, `io_cache_write=4` (O_DIRECT).
- Load: 1 client, nconnect 16×2 subnets (32 data conns), 1 MB wsize.
- Write path is DRAM-BW-bound: IPC 0.35, 72% cycles stalled on memory
  (`stall_backend_mem`), `memmove` the #1 function at 41%, workers 86% busy.
- Perf stack of the copy:
  `memmove <- simple_copy_to_iter <- skb_copy_datagram_iter <- tcp_recvmsg
   <- svc_tcp_sock_recvmsg <- svc_tcp_read_msg <- svc_tcp_recvfrom`.
- Read vs write asymmetry: reads ≈53 GiB/s, writes ≈24 GiB/s (≈read/2).

## DRAM-touch accounting

Write path crosses DRAM **4×** per byte:
1. NIC RX DMA lands the skb.
2. `memmove` reads the skb  ⎫ the receive copy
3. `memmove` writes `rq_pages` ⎭
4. NVMe DMA reads `rq_pages`.

Read path crosses DRAM **2×** (NVMe DMA-in + zero-copy NIC TX). The extra two
touches on writes *are* the copy — matches the ≈½ throughput. Removing it:
4 touches → 2 → roughly 2× write throughput / half the write BW.

## Code trace

**The copy — SUNRPC transport receive:**
- `net/sunrpc/svcsock.c:svc_tcp_read_msg()` builds an `ITER_DEST` bvec over the
  server's pre-allocated `rqstp->rq_pages[]`
  (`bvec_set_page(&bvec[i], rq_pages[i], PAGE_SIZE, 0)`) and hands it to
  `svc_tcp_sock_recvmsg()` → `sock_recvmsg` → `tcp_recvmsg`, which copies skb
  payload into `rq_pages`.
- `rq_pages` are the server's own individual base pages (allocated in
  `net/sunrpc/svc.c:svc_init_buffer()`, filled per-request in
  `svc_alloc_arg()`) — not the skb pages.

**Payload placement / alignment:**
- `fs/nfsd/nfs4xdr.c:nfsd4_decode_write()` carves the payload out with
  `xdr_stream_subsegment(argp->xdr, &write->wr_payload, wr_buflen)` at the
  current XDR cursor — right after the COMPOUND/SEQUENCE/PUTFH + WRITE
  stateid/offset/stable/len fields. So
  `wr_payload.page_base = offset_in_page(<cumulative header length>)`, ≈160 B.
- `net/sunrpc/xdr.c:xdr_buf_to_bvec()` propagates it:
  `bvec[0].bv_offset = offset_in_page(page_base)`; subsequent bvecs start at
  offset 0 (page-aligned, contiguous).

**The DIRECT write (already zero-copy from `rq_pages` onward):**
- `fs/nfsd/vfs.c:nfsd_vfs_write()` → `xdr_buf_to_bvec()` →
  `nfsd_direct_write()` → `nfsd_write_dio_iters_init()`.
- `nfsd_write_dio_iters_init()` handles TWO alignment axes:
  - `offset_align` (`nf_dio_offset_align`, logical block) via
    prefix/middle/suffix file-position splitting.
  - `mem_align` (`nf_dio_mem_align`, memory DMA alignment) via
    `iov_iter_bvec_offset(&seg.iter) & (mem_align - 1)`.
- Both come from `fs/nfsd/filecache.c` via `statx.dio_mem_align` /
  `dio_offset_align`.
- The DIRECT write is **synchronous** (`init_sync_kiocb`, `ki_complete == NULL`)
  — bounds any skb-page pin lifetime to one blocking `vfs_iocb_iter_write`.

## Key insight — alignment is a red herring here

It is tempting to say "misaligned TCP payload defeats DIO → NFSD falls back to
buffered." **Wrong on this platform.** Two distinct alignment axes get
conflated:
- `offset_align` gates the *file position* — handled by prefix/middle/suffix.
- `mem_align` gates the *source-buffer memory* alignment. On XFS/NVMe
  `dio_mem_align` = **4 bytes** (NVMe sets `dma_alignment = 3`). Payload offset
  ≈160, and `160 & (4-1) == 0` → the `mem_align` check passes, the middle
  segment gets `IOCB_DIRECT`, DIO **issues**.

So DIO is NOT refused and there is NO buffered fallback on XFS/NVMe. The
"defeats DIO → buffered" story only holds on a device reporting 512-byte memory
alignment (some SATA/SCSI), which is not this platform. The real cost is the
*receive copy* — independent of, and upstream of, the DIO alignment gate. The
iomap/block DIO alignment overhaul (Mike + Keith Busch) is downstream and
already in 7.1.8; it is not this problem.

## Candidate direction

Page-referencing receive for the DIRECT write path (`tcp_read_sock` / holding
refs to skb page_pool pages) so `rq_arg` points at the received data and the
DIO write DMAs it in place, instead of `sock_recvmsg` copying into `rq_pages`.
Open sub-question: whether `xdr_buf`/`rq_arg` can carry skb `(page, offset,
len)` fragments (plus skb page lifetime through write completion), and whether
the ~160B header can be consumed while the bulk payload is referenced.

Staging: a hardware-specific, non-upstreamable short-circuit to demonstrate the
win first; capability-gated auto-detection is the eventual upstream goal.

## Risk analysis (ranked)

1. **Per-fragment alignment, not just the 160B header.** The current DIO code
   relies on "only the first bvec is unaligned; the rest are `bv_offset == 0`."
   Referencing skb frags destroys that: a 1 MB payload becomes many MTU-/GRO-
   sized frags at arbitrary offsets/lengths — each boundary a fresh potential
   `mem_align` violation and a fresh bvec. The DIO path must then tolerate an
   N-segment, arbitrarily-offset bvec. Whether the NIC does header-data split
   (payload landing page-aligned) largely decides how ugly this is.
2. **Header/payload split within one skb.** Record-mark + RPC/NFS header
   (~160B) and payload start share a linear skb region/page. Must consume the
   header but reference the payload tail; `rq_arg.head` still handled while only
   `rq_arg.pages` is referenced.
3. **skb page lifetime.** Tractable because the DIRECT write is synchronous —
   pages pinned only for one blocking write, not across async completion.
   Contract is with the socket receive queue (deferred consume/`sk_eat_skb`).
   Cost: holds socket buffer memory longer under load.
4. **Upstream shape.** A page-referencing sunrpc server receive is a real
   transport change; the enabling gate likely keys off a device/NIC capability
   (HDS + DMA-alignment tolerance), analogous to read-path zero-copy TX keying
   off sendpage support.

## Xsight E1 RX capabilities — answers risk #1

Analyzed from the `xeu-esdk` driver (kmod-xsight 2.0.1), which builds clean
against this kernel. Every claim below cites the file and line it comes from;
**corrected 2026-09-10** after review by Hammerspace hardware engineers (see
"Corrections" at the end of this section — the earlier write-up mis-attributed
the datapath to an FPGA and asserted two things the source does not say).

**Architecture, stated correctly.** The E1 is a **DPU SoC** (PCI vendor
`0x1e6c`, PF `0x00c0` / VF `0x00c1` — `xeu_drv.h:47`) and the NIC datapath —
MACs, packet processor (PP), exact-match engine, DMA/HIU, P-Units/E-Units,
SSMU with PASID/SID two-stage translation — lives **in the E1 chip itself**.
The DPU board also carries an FPGA and an NXP management processor, but those
are board-level support silicon for bring-up/management (APU-on-a-jet), *not*
in the packet path. The driver source contains **zero** occurrences of "FPGA"
— a change to RX placement behavior is E1 firmware / PP configuration, not
gateware.

**The DMA engine already does multi-buffer scatter, with arbitrary per-buffer
address and length.** This is the capability the earlier write-up missed.
`struct rx_sr_bd` (`xsl_dma_hsi.h:~400-445`) carries a 64-bit `dst_addr`, a
length field of bits 19:0 (**up to 1 MB per buffer**, zero-length not
supported), and an `RX_SR_BD_EOF` end-of-frame flag; the completion's
`num_desc` is documented as "Number of descriptors in packet"
(`xsl_dma_hsi.h`, `struct rx_cr_base_desc`). The driver consumes exactly that:
`num_of_frags = num_desc - 1` with one `skb_add_rx_frag()` per extra buffer
(`xeu_drv.c:818-834`). So one frame can already land across many buffers of
software's choosing.

**What produces today's geometry is the driver's uniform buffer posting, not a
hardware single-buffer limit.** The RX ring is filled with order-0 page_pool
pages (`xeu_rx_ring_page_pool_create()`, `.order = 0`, `PP_FLAG_DMA_MAP |
PP_FLAG_DMA_SYNC_DEV`, `.offset = 0`, `.max_len = PAGE_SIZE` —
`xeu_drv.c:414-436`), and every descriptor is posted at a fixed frame-start
offset: `dst_addr = dma_addr + XEU_RX_PAGE_HEADROOM` (`xeu_drv.c:496`), where
`XEU_RX_PAGE_HEADROOM = NET_SKB_PAD + NET_IP_ALIGN` (`xeu_drv.h:56`; ~64B on
arm64, `NET_IP_ALIGN == 0`). Receive builds the skb over that same page with
`napi_build_skb()` + `skb_reserve(XEU_RX_PAGE_HEADROOM)` and puts the linear
head plus every frag at the identical headroom (`xeu_drv.c:797-834`).
Consequence: the first buffer of a frame holds **headers and the start of
payload together**, so payload begins at `headroom + parsed_header_length` — a
header-length-dependent offset (Eth14+IP20+TCP20 = 54; 64+54 = 118;
`118 & 3 == 2`, not even 4-byte aligned) that varies per packet with TCP
options.

**Capabilities confirmed present** (all cited):

1. **page_pool substrate with recycling** — `skb_mark_for_recycle()`
   (`xeu_drv.c:813`) over `PP_FLAG_DMA_MAP` pages: exactly the ref-countable
   substrate a hold-refs `->read_sock` receive needs.
2. **Per-layer header lengths L0–L8 in the 64-byte RX completion** —
   `RX_CR_IN_L0_LEN_SHIFT` … `RX_CR_IN_L8_LEN_SHIFT` across `w8`/`w9` of
   `struct rx_pp_cr_rx_desc` (`xsl_pp_hsi.h:66-96`). The PP hands software the
   exact header/payload boundary with no reparse.
3. **Hardware GRO context state** — `gro_context_flags` with a 10-bit context
   id (1024 contexts) plus `GRO_TRIGGERED`, `GRO_NEW_CONTEXT`,
   `GRO_ADD_TO_AGG`, `GRO_LEN_CHANGED`, `GRO_CONTEXT_MISS`
   (`xsl_pp_hsi.h:100-110`). The PP already tracks per-flow aggregation state;
   the driver reads none of it and calls plain `napi_gro_receive()`
   (`xeu_drv.c:910`), i.e. software GRO.
4. **RoCE/ICRC datapath** — `RX_CR_IN_ROCE`, `RX_CR_IN_PKT_ICRC_PRESENT`,
   `RX_CR_IN_ROCE_ICRC_ERR` (`xsl_pp_hsi.h:36-37,51`): the E1 already does
   zero-copy RDMA receive, the structural model TCP would emulate.
5. **Content-steered placement exists in the descriptor format** — the PP
   metadata union overlays `struct rx_lookup_match lkup2[3]` with a 64-bit
   `buf_addr` (`xsl_pp_hsi.h:124-131`), i.e. a lookup result can carry a
   buffer address.
6. RSS `flow_hash` (`w7`), RX checksum offload (`xeu_drv.c:837-860`).

**Capabilities absent in 2.0.1:**

- **No header-data split.** Nothing in the driver or HSI headers splits headers
  from payload into separate buffers; no `tcp-data-split` / HDS ethtool
  support (`xeu_ethtool.c:287-330` reports only rx/tx pending, with
  mini/jumbo zeroed), and no `NETIF_F_LRO`. Advertised features are
  `IP_CSUM|IPV6_CSUM|RXCSUM|SG|TSO|TSO6|GSO_UDP_L4|RXHASH|GRO`
  (`xeu_drv.c:2973-2981`).
- **No payload-alignment knob.** `struct xsl_dma_profile` is two fields: an
  AXI setting and a routing destination, where 2.0.1 enumerates
  `XSL_AXI_DEFAULT` only and `XSL_ROUTING_PCIE` / `XSL_ROUTING_DRAM`
  (`xsl_dma.h:18-41`). Profiles steer AXI attributes and PCIe-vs-E1-DRAM
  routing; nothing in them expresses an in-page payload offset.

**Verdict.** For the page-loan series to pay off on E1, payload must arrive
**page-aligned and densely packed** — which needs two things together: (a)
header/payload split placement, so payload starts at offset 0 of its own
buffer rather than after the headers, and (b) **coalescing across packets**, so
one page carries payload from many frames instead of ~1448 bytes per page.
(a) alone is insufficient: per-frame payload buffers still put every
inter-frame joint at a non-logical-block-aligned payload position, which NFSD's
DIO admission gate must reject. The encouraging part is that the building
blocks are visibly present — arbitrary per-buffer address/length in the submit
descriptor, the parsed header boundary in the completion, and per-flow GRO
aggregation context — so this reads as a placement/aggregation **binding**
that E1 firmware and PP configuration may be able to express, rather than a
missing hardware function. Whether that binding exists or is reachable is a
question only Xsight's hardware team can answer; the driver source cannot
settle it.

**A software-only experiment worth trying first** (hypothesis, untested):
because the submit ring accepts arbitrary per-buffer lengths and the engine
advances to the next descriptor mid-frame, the driver could post *alternating*
descriptors — a small header-sized buffer followed by a page-aligned
payload buffer — approximating HDS with no firmware change. Limits to confirm
on hardware: the split lands at the posted length, not the actual parsed
header length, so it is only exact while header size is constant (TCP options
shift payload within the second buffer); and it does not solve (b), since each
frame's payload still occupies its own page. It would, however, validate the
alignment half of the direction cheaply and quantify what remains.

**Corrections (2026-09-10).** Three claims in the pre-review version of this
section were wrong and are retracted: (1) the RX datapath is **not** an FPGA —
it is the E1 DPU chip, and the board's FPGA/NXP processor are management
silicon outside the packet path, so "an FPGA/gateware revision" mis-stated
both the component and the cost; (2) **"No page cross per descriptor"** was
attributed to `xsl_dma_hsi.h` but **appears nowhere** in the 2.0.1 (or 0.8.3)
source — and the opposite is closer to true, since multi-descriptor scatter
per frame is a documented, driver-exercised feature; (3) DMA profiles were
described as controlling "AXI cache attributes (RCI/RI = read-clean-invalidate)"
— 2.0.1 enumerates only `XSL_AXI_DEFAULT`, so the RCI/RI detail does not come
from this source and the "cache-pollution lever" aside it supported is
withdrawn pending a citable reference.

## Series design & extension points

(From David Flynn's rationale.) The page-loan mechanism is transport + XDR
infrastructure with a per-procedure opt-in — not an NFS-specific feature:

- **~3/4 of the production change lives in SUNRPC** (`svcsock.c`, `xdr.c`,
  `svc.c`, headers, tracepoints). Any in-kernel RPC service on the `svc`
  framework can use it by marking a procedure as accepting XDR bvecs
  (`pc_xdr_bvec`) and having its decoder consume them. Today two procedures opt
  in — NFSv3 WRITE and the NFSv4 COMPOUND; adding another is a flag + a decoder
  change, not transport work.
- **The `->read_sock` actor classifies each call from its fixed first 28
  bytes** and, when eligible, borrows the socket's page fragments instead of
  copying: one page reference per borrowed fragment, published as an immutable
  "authoritative" bvec array, with only the small fixed prefix copied.
- **Eligibility gate (deliberately narrow):** AUTH_SYS/AUTH_NULL only, RPC
  *calls* only, plaintext TCP only. RPCSEC_GSS, kTLS, and replies each remain an
  extension point with a known fallback in place.
- **Ownership contract (strict, tested):** exactly one reference per borrowed
  fragment; exact unwind on every error/teardown path; no retained socket
  header; page-pool pages tracked with their elevated refcount; borrowed pages
  returned to the network stack after the *synchronous* write completes. A
  failed classification's worst case is the old copy path — no heuristic can
  misfire into a wrong answer.
- **Generic pattern:** "classify early from a fixed header, borrow socket
  fragments under a reference, publish an immutable vector to the consumer,
  return pages on completion" is not RPC-specific — any record-oriented
  in-kernel TCP consumer that copies out of the socket (kernel-served block and
  file protocols) has the same shape of problem and could adopt the same shape
  of solution.

**Deliberately unchanged:** the Linux NFS client and its receive path; the
server send direction (READ replies, already page-based); RDMA transports
(already zero-copy end to end); kTLS / RPCSEC_GSS / replies (copy as before);
and all filesystem/block behaviour (direct I/O only where the fs alignment rules
permit, buffered otherwise).

## Design decisions in the page-loan integration

The page-loan series is David Flynn's realization of the direction above: SUNRPC
lends the TCP receive pages into the request's XDR buffer as *authoritative*
bvecs, NFSD decodes the WRITE arguments from them, and page-aligned segments go
to the filesystem as O_DIRECT (misaligned segments fall back to buffered — the
same alignment gate the E1 analysis lands on). See `README.md` for the branch
and commit-structure overview; the notable design decisions:

- **Whole-iterator alignment check (vfs.c).** The series replaces the
  first-bvec-only `iov_iter_bvec_offset()` check — valid only for the contiguous
  copied-arena bvecs — with a whole-iterator `nfsd_dio_iter_is_aligned()`
  (`iov_iter_alignment()`), required because loaned pages place fragments at
  arbitrary offsets. The Hammerspace base additionally carries an
  `IOCB_DONTCACHE` uncached-buffered fallback (with an
  `nfsd_direct_misaligned_num_pages` tunable); the integration keeps that
  fallback wrapped around the new whole-iterator check — strictly better:
  correct for loaned bvecs *and* preserving the uncached-buffered optimization.

- **Three adaptations for the Hammerspace base** (each `[snitzer: …]`-annotated,
  upstream author preserved). The series was qualified upstream against stable
  v7.1.8 with `NFSD=y`; the Hammerspace base is 7.2-ward in a couple of spots
  and builds `NFSD=m` with `KUNIT_ALL_TESTS=m`:
  1. **`nfsd3_procedure()` gated behind `#if IS_ENABLED(CONFIG_KUNIT)`** — a
     KUnit-only test accessor; without the guard it is an unused `static` under
     `CONFIG_KUNIT=n`, which `CONFIG_WERROR=y` turns into a build failure. This
     mirrors how the series already guards its NFSv4 twin.
  2. **Drop the synthetic `svc_version.vs_count`** in the dispatch-parity test.
     The upstream series added a per-CPU counter because stable v7.1.8 counts
     through `versp->vs_count[proc]`; this base already moved counting to per-net
     `sv_stats->vs_count`, so `struct svc_version` has no such member here.
  3. **`EXPORT_SYMBOL_IF_KUNIT(nfs3svc_decode_writeargs)`** — under `NFSD=m` the
     KUnit test builds as a standalone module that calls this nfsd-internal
     decoder; without the export it fails modpost with the symbol undefined.
     (Upstream this never bit because the test was built into `nfsd` with
     `NFSD=y`.)

With those three adaptations, base + all 30 commits build with **zero warnings
and zero errors under `CONFIG_WERROR=y` on both 4K and 64K page sizes** with the
KUnit test modules compiled and linked — bisect-clean. See `README.md` →
"Building & bisect testing" for the reproduction.

### Follow-on fixes (on main, beyond the 30-commit series)

- **Fetch direct I/O alignment for files handed to the filecache** (David
  Flynn). `nfsd_file_do_acquire()` fetched the DIO alignment attrs only on the
  branch where NFSD opens the file itself; the supplied-file branch (taken by
  NFSv4 OPEN with CREATE, which passes a `struct file` from `dentry_create()`)
  left them zero, so `nfsd_write_dio_iters_init()` refused direct I/O for every
  WRITE to a just-created file. Hoisting `nfsd_file_get_dio_attrs()` to run for
  both branches fixes it (measured: 16/16 post-OPEN(CREATE) writes went buffered
  before, direct after).

- **Build the SUNRPC KUnit suites as standalone modules.** The XDR and svcsock
  suites were linked into `sunrpc.o`, so enabling them made the production
  `sunrpc.ko` depend on `kunit.ko` — which breaks distro module-to-package
  assignment (a core module cannot depend on a `modules-internal` test module;
  `kunit.ko`/`sunrpc.ko`/`rpcrdma.ko` all end up unassignable). Reworked to
  standalone modules (`obj-$(CONFIG_…) +=`, like the NFSD suites), exporting the
  sunrpc internals they exercise with `EXPORT_SYMBOL_IF_KUNIT`. `sunrpc.ko` now
  carries no `kunit` dependency, so the suites can be enabled in a shipped debug
  kernel. See `TESTING.md`.

- **Don't clobber IOCB_DONTCACHE on the no-alignment write fallback**
  (**folded into the base's heuristic commit at the 2026-09-09
  `v7.1.8-5` respin** — no longer carried in-series; was `2c8aa325f72a`,
  Fixes: `eaa2a4f9efb7`). In `nfsd_write_dio_iters_init()`'s
  "filesystem advertises no DIO alignment" path, `IOCB_DONTCACHE` was ORed into
  `segments[0].flags` *before* `goto no_dio`, where
  `nfsd_write_dio_seg_init()` assigns `segment->flags = iocb->ki_flags` —
  silently discarding the bit. The FOP_DONTCACHE uncached-buffered fallback
  therefore always issued plain cached buffered I/O. Found by inspection while
  debugging the KUnit panic below.

- **Give `nfsd_bvec_dio_segments_test` a backing `struct file`**
  (**folded into "nfsd: exhaustively test receive bvec consumers"
  2026-09-09** — no longer a separate commit; was `0d782d32c219`,
  Fixes: `e051ae21f77e`). The test fabricated its
  `struct nfsd_file` with `nf_file = NULL`, but since `eaa2a4f9efb7` the
  production code consults `nf_file->f_op->fop_flags` on every
  buffered-fallback path — the first memory-misaligned case
  ("later-offset-misaligned") oopsed the host (NULL deref at
  `nfsd_write_dio_iters_init+0xf0`, kdump vmcore
  `/var/crash/127.0.0.1-2026-09-04-14:13:04`). Now carries a fake
  file/file_operations with per-case `fop_flags`, plus two new
  FOP_DONTCACHE cases (one regression-tests the clobber fix above).

- **Fix `dio_segments_test`'s 3-way-split expectation for the misaligned gate**
  (**folded into "nfsd: exhaustively test receive bvec consumers"
  2026-09-09** — no longer a separate commit; was `23bd7749260b`,
  Fixes: `e051ae21f77e`). The
  "buffered-prefix-direct-middle-buffered-suffix" case expected a 3584-byte
  direct middle, but the `nfsd_direct_misaligned_num_pages` gate (default 2
  pages, present in the series before the test) correctly refuses to split
  middles that small — the failure had been masked by the panic in the case
  before it. The old geometry is kept as a gate regression test
  ("small-middle-below-misaligned-gate"); the split case now uses a
  `2 * PAGE_SIZE` middle so it clears the gate on every page-size config
  (4K/16K/64K) alike.
  `nfsd-receive-bvec` stays 5 tests; the DIO case table grew 6 → 9
  (later 12, after the fused-gate discontinuity cases).

## Open questions / next steps

- [ ] **NVMe SGL support** (`NVME_SGL_SUPPORT_PROJECT.md`, scoped
      2026-09-26): expose the device's memory-segment boundary from NVMe
      through XFS and statx to `nfsd_file`, and relax the admission gate's
      joint rule when it is 0; phase 1 validated on XFS-on-brd and
      nvme-over-TCP, xeu placement is phase 2.
- [ ] **Run the 2026-09-25 regression plan** (`TESTING.md` → "Next
      regression run (handoff)"): KUnit incl. the back-to-back locked-heads
      case and its fix-reverted A/B, the runtime harness, a deliberate
      receive-queue-collapse run, and the LOCALIO A/B against stock
      `v7.1.13-14`. The merge top-up fix (first `1e48b4d03cb8`,
      now folded into `dd2e42c5f547`) was verified by the 2026-09-25 run; until the
      bisect walk also passes,
      `svc_tcp_rx_loan_pages=N` stays the advice where data matters.
- [ ] Consider letting a second locked head join a merge page that still has
      room instead of falling back to the arena, so collapsed receives keep
      their direct-I/O path (design change, after the fix is qualified).
- [ ] **Re-qualify on `kernel-7.1/hs-7.1.13-5.NFSD_TCP_WRITE_ZEROCOPY`
      (2026-09-18 rebase).** Only a WERROR compile check of the tip has been
      done on this base: run the four KUnit suites, the bisect/sparse walk
      (regenerate the config seeds from the `v7.1.13-5` src.rpm first), and
      the runtime harness; the base's rewritten `NFSD_IO_DIRECT` write path
      (`NFSD_DONTCACHE_FALLBACK`: no split when the middle cannot be direct,
      cached tail page kept for the next WRITE) is where the loan now lands,
      so re-check the loan-assert / rig-cmp numbers rather than assuming the
      hs.160 ones carry over.
- [x] Whether the xeu ports (E1 DPU) support header-data split, and the payload frag
      offset/alignment. **Answered: no HDS; payload at headroom+hdr_len
      (variable, non-4-aligned); page_pool frags; HW reports L0–L8 lengths.**
- [x] Confirm the storage DMA path's tolerance for N-segment, per-frag-offset
      bvecs (NVMe PRP/SGL, iomap `bdev_dma_alignment`): **answered 2026-09-05
      by the 16K runtime rig, refined 2026-09-09 by the root cause**: the
      nvme(-loop) path tolerates per-frag-offset **contiguous** runs fine
      (mid-bvec splits included); what it rejects is an **interior
      discontinuity at a non-lbs-aligned payload byte** —
      `bio_split_io_at()`'s ALIGN_DOWN-to-zero branch (see
      `NFSD-LOAN-PAGES-EINVAL-ROOT-CAUSE.md`, which supersedes the anchor
      framing in `TESTING.md` → "16K runtime run and the loaned-DIO EINVAL
      finding").
- [x] **Fix the loaned-DIO EINVAL**: **closed 2026-09-09 by the
      admission-time gate**, landed in the `v7.1.8-5` rebase
      (`kernel-7.1.8/main-5.NFSD_TCP_WRITE_ZEROCOPY`) as "nfsd: require
      interior payload discontinuities to be logical-block aligned" —
      `nfsd_dio_iter_is_aligned_and_splittable()` demotes any direct
      segment with an interior discontinuity off an `offset_align`
      multiple (one fused walk also covering memory alignment), ordered
      ahead of the loan commits; `dio_segments_test` covers it. The
      interim `nfsd_direct_write()` retry-on-`-EINVAL` fallback was
      dropped in the same rebase — Chuck NAK'd inferring alignment from
      `-EINVAL`, and that reasoning applies to this tree too. (History:
      the fallback had held the rig clean — 30 fresh connections ×
      16 MiB, zero client-visible errors.)
- [x] **Silent corruption of loaned direct writes — root-caused and fixed**
      (found while validating the EINVAL fix with `cmp`; predates the
      series): not the split, not nvme-loop/`blk_rq_map_sg`/nvmet — a
      synthetic-bio reproducer (`bvecrepro.c`) corrupts on `/dev/ram0`
      directly with no split. `bio_advance_iter_single()` advances
      `bi_sector += bytes >> 9`, so brd's per-segment position skews by the
      sub-sector residue of any bvec length that is not a 512-multiple.
      Fixed on main ("brd: iterate the bio by byte position, not
      bi_sector"); reproducer matrix all-MATCH and 20 fresh NFS
      connections × 16 MiB show zero `cmp` mismatches. See `TESTING.md` →
      "The root cause".
- [x] **Send the four standalone fixes upstream** — **POSTED
      2026-09-08**, awaiting review/merge (`TESTING.md` → "Fixes
      that must be sent upstream"; all extraction-ready at the front of
      the branch): brd byte-position (stock `NFSD_IO_DIRECT` corrupts on
      any brd-backed export today), zram sub-page bvec corruption, and
      two nfsd `NFSD_IO_DIRECT` fixes (filecache DIO-alignment fetch,
      direct-write `-EINVAL` buffered fallback). The `IOCB_DONTCACHE`
      clobber fix was dropped from the posting 2026-09-06 — not
      upstream-exposed, its bug arrived with the hs-carried hch
      heuristic RFC (`eaa2a4f9efb7`); it travels with that patch.
      Series drafted in `NFSD_TCP_WRITE_ZEROCOPY/upstream-fixes/`
      (unversioned iteration dir, cover letter included).
      **2026-09-09: patch 4/4 (the `-EINVAL` fallback) withdrawn** after
      Chuck Lever's review — not upstream-exposed, the failing geometry
      is loan-only (`NFSD-LOAN-PAGES-EINVAL-ROOT-CAUSE.md`; Mike's
      on-list reply `aqGL5dh49toktgBI@kernel.org`). Series is now
      patches 1–3, with 1–2 the stable candidates.
- [ ] Raise upstream whether iomap should validate/bounce `ITER_BVEC`
      direct I/O with sub-sector bvec boundaries (nfsd's statx-based gate
      is provably weaker than the block stack's split-time checks).
      The dm-devel half is **done** (2026-09-08): dm-devel was Cc'd on
      the four-fix posting; per Mike (a DM maintainer), the coarse
      `dma_alignment >= 511` front gate obviates concern over the DM
      bvec-iter exposures at this time — the DM maintainers' call from
      here.
- [x] **Audit other bio-based drivers**: **complete** (`BIO-DRIVER-AUDIT.md`,
      2026-09-05) — null_blk, all of DM, MD, zram swept for
      `bi_sector`-derived per-segment positions. zram carried the same
      class (silent corruption on every 4K-page kernel) and is fixed on
      main; four DM sites are exposed but unreachable through NFSD's
      alignment gate; MD, dm core, and the remaining targets are clean.
      The iomap validate/bounce question is split out above.
- [x] Re-run the **4K host runtime validation with `cmp`** and the patched
      brd: **done 2026-09-05** on the rebuilt `hs.159` — 30 fresh
      connections × 16 MiB, 0 failures, 0 `cmp` mismatches, `bvecrepro`
      matrix 4/4 byte-identical (`TESTING.md` → "4K host re-validation").
- [x] Prototype a receive that references payload pages into the request
      instead of copying: **superseded by the page-loan series itself** —
      the `->read_sock`-based loan path on this branch is that prototype,
      generalized (loans socket receive pages, not raw skb frags, so it
      is not hardware-specific). The measurement half lives in the
      hardware-validation item below.
- [ ] **Validate on real hardware (tardis1/E1)**: the perf comparison
      patched-vs-unpatched (`io_cache_write` 0 vs 2) that tests the
      project thesis (4→2 DRAM touches ≈ 2× write throughput), plus
      real-NIC geometry — how often E1 page_pool fragment offsets pass
      the whole-iterator DIO gate on 400G traffic, whether a
      lead-fragment-only copy is needed, with RDMA's structurally
      zero-copy receive as the comparison model. Everything qualified so
      far is functional, on VMs/loopback.
- [x] Re-run the build with `DEBUG_INFO`/BTF enabled for a fully faithful
      configuration: **done 2026-09-05** — tip built with
      `DEBUG_INFO_BTF=y` + `BTF_MODULES=y` (pahole v1.31), booted as
      `7.1.8-3.hs.159`; module BTF accepted for sunrpc/nfsd/lockd/grace,
      BTF ≡ DWARF on every touched struct (incl. the `svc_procedure`
      31/1 bitfield split, size-neutral), pahole shows the series adds
      **zero new holes** (`xdr_buf` 72→80 holeless, `svc_rqst` +24 as
      expected, `svc_sock` +16 into a pre-existing hole region), and the
      `svc_tcp_rx_*` enums + `svcsock_tcp_rx_lifetime` tracepoint are
      BTF/tracefs-visible for typed probing. Per-commit BTF walk judged
      redundant (link-time, type-driven; tip covers the type superset).
      Full record: `TESTING.md` → the `DEBUG_INFO`/BTF entry.
- [x] Run the four KUnit suites on a **16K page-size** kernel on the MacBook
      Pro testbeds (16K stands in for 64K there, since Apple M-series cores
      lack the 64K translation granule and a 64K kernel cannot boot on those
      hosts): **done 2026-09-04** on `7.1.8-3.hs.161` — 7/7, 53/53, 5/5 (all
      9 DIO geometries), 9/9, zero fault signatures. First non-4K execution
      of the suites (`TESTING.md` → "Test coverage status").
- [x] ~~Bisect build walk on the 16K config~~ **dropped 2026-09-05**
      (with the 64K walk item): page size on the same architecture and
      compiler varies only constants, so the completed full 4K walk plus
      the historical 64K base+30 walk already bracket the useful
      endpoints. Superseded by the x86_64 walk item below. The 16K tip
      builds clean regardless (`7.1.8-3.hs.161` production build).
- [x] Run the **bisect build walk on x86_64** (full branch,
      `CONFIG_WERROR=y` + sparse): **done 2026-09-06** on
      `7.1.8-3.hs.297.el8.x86_64` (gcc 11.2.1, sparse v0.6.5-rc1) —
      **42/42 code commits clean, 0 failures, zero branch-introduced
      sparse findings**, 36 doc-only commits auto-skipped. First
      non-aarch64 execution of the walk, so the cross-architecture
      coverage the item was asking for (arch-specific warning surface,
      type/format differences) is now real and found nothing new.
      Full record: `TESTING.md` → the x86_64 walk entry.
- [ ] Force the live **copy-path `reason` codes** (gss / tls / reply) on
      real traffic — needs a krb5 or `xprtsec=tls` mount; the exclusion
      table is KUnit-covered today (`svcsock_rx_exclusion_table_test`).
- [ ] Run the four KUnit suites on the **64K page-size** kernel (only built so
      far; suites have run on 4K and 16K). Depends on E1 hardware or another
      host whose CPU implements the 64K granule (e.g. Ampere lab hosts).
- [x] Extend the bisect build walk over the follow-on commits: **done
      2026-09-05** — the latest walk covers the entire restructured
      branch, 42/42 code commits clean with per-step sparse (git-master
      sparse required; el9's 0.6.4 is silently disabled by kbuild's
      `__typeof_unqual__` probe), zero branch-introduced findings.
- [x] Runtime validation of the copy elimination (`TESTING.md` → "Runtime
      validation"): **done functionally** on 4K and 16K (the 16K run also
      verified the flat page-cache footprint, and surfaced the loaned-DIO
      EINVAL above) — on a brd-backed nvme-loop export —
      16 MiB page-aligned writes borrowed 16 780 596 bytes / copied 0, all
      direct. Key gotcha recorded: a same-host mount must disable
      `nfs.localio_enabled` or LOCALIO bypasses the TCP receive entirely.
      Since extended to **x86_64** (2026-09-06, full runner set incl.
      800/800 loan assertion) and **re-validated on aarch64 4K with the
      corrected harness** (2026-09-05: structural loan bound + sysfs
      nvme-loop row hold on both architectures' skb geometries; zram
      bvecrepro rows first checked on aarch64-4K). Still open:
      flat-page-cache measurement under load; forcing the live copy-path
      `reason` codes (krb5 / `xprtsec=tls`; `auth` fires on ordinary
      traffic).

- [ ] **Run the xeu paired-RX-posting experiment on tardis1.** The series
      cannot pay off on E1 until payload arrives page-aligned, and the
      capability review says the placement may be reachable in software:
      the RX submit descriptor already carries a per-buffer address and
      length and the engine already spreads a frame over several buffers.
      Driver patch, operator README and an executable procedure are in
      [`xeu-hds-experiment/`](xeu-hds-experiment/); RPMs for the hs.165
      kernel were delivered 2026-09-10 as
      `xeu-hds-tardis1-hs.165-2.tar.gz` (both 4K and 64K kmod flavors,
      both verified to carry the patch). Untested on hardware. A pass
      proves *placement* only — direct I/O stays demoted until payload is
      also packed across frames, which is the firmware ask.

- [ ] **Chase receive-pass amplification on real hardware**
      (`run-rxpasses.sh`, `TESTING.md` → "Receive-pass amplification").
      On the 4K loopback rig loans make each `svc_tcp_recvfrom()` pass
      ~2.4x cheaper and cut receive-path time ~22%, but need 1.94x the
      passes per record (9.5 vs 4.9) and ~25% more nfsd wakeups per MiB —
      because a loan pass drains the socket queue fast enough that the
      thread sleeps and `sk_data_ready` wakes it again. Not send-buffer
      backpressure: `svc_data_ready` and actor counts are identical with
      loans on and off. `NFSD_IO_DIRECT` is clean on this axis. The
      net-CPU verdict does not transfer off loopback (the copy is
      cache-hot there); the amplification should, and may look very
      different at MTU 1500 where a 1 MiB record is hundreds of skbs
      rather than ~17. Candidate fix, deliberately unwritten until
      measured: a bounded re-poll before the nfsd thread sleeps.

## Repro / instrumentation

tardis1 is set up. The write-load repro plus the `perf stat` / `ss -tie` /
`iostat` / OCP-WAF instrumentation behind every number above can be shared on
request (Jonathan Flynn).

Server-side instrumentation now lives in this directory and needs no rig:
[`zcstat.sh`](zcstat.sh) (live loan/DIO accounting, documented in
[`zcstat.sh-usage.md`](zcstat.sh-usage.md)) and
[`run-rxpasses.sh`](run-rxpasses.sh) (receive-pass amplification, purely
observational with a loan-switch A/B mode). Both read only tracepoints
that are identical on the `-3` and `-5` branches, so they serve either
kernel.
