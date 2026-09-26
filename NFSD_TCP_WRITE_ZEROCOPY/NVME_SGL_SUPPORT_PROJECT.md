# NVMe SGL support for loaned NFSD WRITE payloads

Sub-project of the NFSD TCP write-path zero-copy (page-loan) series, started
2026-09-26. Goal: let a loaned WRITE payload whose fragments are 4-byte
aligned but **not page-tiled** — fragments that end mid-page, successors that
start at a non-zero page offset — go to the device as direct I/O whenever the
device can take it, which on NVMe means the controller submits it with SGLs.

## 0. Status / handoff

- **Scoped, not started.** Everything below is from reading the tree
  (`kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY` on `v7.1.13-14`); nothing is
  implemented, built or measured yet.
- Work lands on `kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY` as incremental
  commits at the tip, feature commits separate from KUnit commits (section 5).
- **Phase 1** (sections 4–6): expose the device's memory-segment boundary
  from the block layer, through XFS and statx, into `nfsd_file`, and let the
  DIO admission gate use it. Done when both validation rigs — XFS on brd and
  XFS on nvme-over-TCP to a local nvmet target — pass with all the plumbing
  in place (section 6).
- **Phase 2** (section 7): the Xsight E1 (xeu) receive-placement changes.
  Not started until phase 1 is done.
- **No shortcuts.** The attribute is exposed from inception on the full
  path — NVMe driver → block queue limits → XFS → statx → `nfsd_file` —
  exactly like the other DIO alignment attributes. No nfsd-side peeking at
  `inode->i_sb->s_bdev`, no module-parameter override.
- **Reporting:** this line of work gets its own claude.ai artifact, separate
  from the zero-copy series' status reports. Not yet created.

## 1. Are the page loans still needed if we simply use SGL? Yes.

SGL and the loans solve different halves of the problem; neither delivers
the win without the other.

- **The loans remove the copy.** Without them, SUNRPC copies every WRITE
  payload out of the skbs into the contiguous `rq_pages` arena
  (`svc_tcp_read_msg()`), and that copy is the cost this project exists to
  remove (memmove ~41% of write-path CPU, 4 DRAM touches per byte → 2).
- **The copied arena needs no SGL.** It is one contiguous page-tiled run, so
  it already satisfies a PRP (virtual-boundary) device and goes direct on any
  NVMe today. Turning SGL on changes nothing for copied payloads.
- **SGL is what a loaned payload needs to stay direct.** A loaned payload is
  the skb frag pages themselves: one fragment per TCP segment, ending
  mid-page, with the next starting in a new buffer. On a device with a
  virtual boundary each such joint forces a bio split that
  `bio_split_io_at()` can only make on a logical-block multiple, so the
  admission gate (`nfs_dio_iter_aligned_and_splittable()`) must demote the
  WRITE to buffered I/O — and a buffered WRITE copies the bytes into the page
  cache, putting the copy back. On a device with no virtual boundary those
  joints are legal and the WRITE can stay direct: zero copies end to end.

So: **loans eliminate the receive copy; SGL (no virtual boundary) keeps a
loaned payload with mid-page joints on the direct path.** Both, plus 4-byte
aligned placement by the NIC (phase 2), are needed for zero-copy direct
writes on real NIC geometry.

## 2. What the tree already does (v7.1.13-14)

- **NVMe drops the virtual boundary when it can use SGLs.**
  `bc840b21a25a` ("nvme: remove virtual boundary for sgl capable devices",
  Keith Busch): `nvme_pci_get_virt_boundary()` (`drivers/nvme/host/pci.c`)
  returns 0 when `nvme_ctrl_sgl_supported()` (dword- or byte-aligned SGLs
  advertised), else `NVME_CTRL_PAGE_SIZE - 1`; `nvme_set_ctrl_limits()`
  (`drivers/nvme/host/core.c`) stores it in `lim->virt_boundary_mask` and sets
  `lim->dma_alignment = 3`.
- **Who has no boundary:** nvme-pci with SGLs; NVMe-oF TCP and FC
  (`nvmf_get_virt_boundary()` returns 0); brd; most SCSI and virtio devices.
  **Who keeps 4 KiB:** nvme-pci without SGLs, nvme-rdma, **nvme-loop**
  (`drivers/nvme/target/loop.c` uses `nvme_get_virt_boundary()`), apple.
  dm/md devices inherit through stacked limits.
- **Block layer rule with no boundary** (`bio_split_io_at()`,
  `block/blk-merge.c`; `bio_split_rw_at()` passes
  `len_align_mask = lim->dma_alignment`): every bvec's offset and length must
  be a multiple of `dma_alignment + 1` (4 on NVMe), else `-EINVAL`; gaps do not
  force splits. Splits for the segment limit (`NVME_MAX_SEGS` = 4096/16 = 256
  SGL descriptors) land on a logical-block multiple inside a bvec, which is
  legal without a boundary; the only residual failure is 256 consecutive
  fragments totalling less than one logical block.
- **PRP vs SGL is automatic:** a request with gaps cannot be expressed as
  PRPs, so nvme-pci uses SGLs regardless of `sgl_threshold`
  (`!prp_possible`, `pci.c`).
- **Nobody re-checks our bvecs:** iomap forwards an `ITER_BVEC` as-is
  (`bio_iov_bvec_set()`); `bio_iov_bvec_aligned()` checks only in debug
  builds. The caller must guarantee the 4-byte rule — the gate's memory
  alignment half already does, since `dio_mem_align == dma_alignment + 1`.
- **What statx reports today:** `xfs_report_dioalign()`
  (`fs/xfs/xfs_iops.c`) fills `dio_mem_align`, `dio_offset_align` and
  `dio_read_offset_align` from `xfs_inode_buftarg(ip)->bt_bdev` (data or
  realtime device, per inode). Nothing about the virtual boundary. nfsd reads
  those three in `nfsd_file_get_dio_attrs()` (`fs/nfsd/filecache.c`) into
  `nf_dio_mem_align`, `nf_dio_offset_align`, `nf_dio_read_offset_align`.
- **This host:** `nvme0n1` (VMware virtual NVMe, `sgls: 0`) and the nvme-loop
  rig both show `virt_boundary_mask=4095`, `dma_alignment=3`,
  `max_segments=256`. There is no local SGL device; phase 1 validates on
  boundary-free substitutes (section 6).

## 3. The interface

One new direct-I/O attribute, reported per file like the existing ones.

- **Meaning:** the memory-segment boundary for direct I/O. `0` — the device
  takes discontiguous memory segments anywhere (subject to
  `dio_mem_align`); `N` — every interior joint between memory segments must
  fall on a multiple of `N` bytes (a PRP NVMe device reports 4096). Reported
  as a size, like `dio_mem_align`, not a mask.
- **Working name:** `dio_seg_boundary` / `stx_dio_seg_boundary`, mask bit
  `STATX_DIO_SEG_BOUNDARY`. The name is a placeholder for upstream review;
  because `0` is a meaningful value, the result-mask bit is what tells
  "reported" from "not reported".
- **uapi space:** `struct statx` has a free `__u32 __spare2[1]` at 0xbc,
  directly after `stx_dio_read_offset_align`; the next free mask bit is
  `0x00040000U`. `tools/include/uapi/linux/stat.h` and
  `tools/perf/trace/beauty/include/uapi/linux/stat.h` carry copies to sync.
- **Consumers treat "not reported" as conservative** (assume a 4 KiB
  boundary), so every filesystem that does not report it keeps today's
  behaviour.

## 4. Phase 1 — core implementation (feature commits, no KUnit content)

Each bullet is intended as one commit, in order. Per the series' KUnit
decoupling policy these commits carry no KUnit references in code or
message.

1. **block:** add `bdev_virt_boundary(bdev)` next to `bdev_dma_alignment()`
   (`include/linux/blkdev.h`), returning
   `queue_virt_boundary(bdev_get_queue(bdev))` — so filesystems read the
   limit through the same kind of helper they use for the DMA alignment.
2. **vfs/statx:** `STATX_DIO_SEG_BOUNDARY`, `stx_dio_seg_boundary` in the
   uapi (and its tools copies), `u32 dio_seg_boundary` in `struct kstat`,
   the copy in `cp_statx()` (`fs/stat.c`).
3. **xfs:** report it in `xfs_report_dioalign()` when requested, from the same
   `xfs_inode_buftarg(ip)->bt_bdev` the other DIO attributes use (so realtime
   files report the realtime device): `mask ? mask + 1 : 0`.
4. **nfsd:** `nf_dio_seg_boundary` in `struct nfsd_file`
   (`fs/nfsd/filecache.h`); request `STATX_DIO_SEG_BOUNDARY` and fill it in
   `nfsd_file_get_dio_attrs()`, defaulting to 4096 when not reported; add it
   to the `nfsd_file_get_dio_attrs` tracepoint.
5. **nfs_common:** `struct nfs_dio_policy` (`include/linux/nfs_dio.h`) gains
   `seg_boundary`; `nfs_dio_iter_aligned_and_splittable()`
   (`fs/nfs_common/nfs_dio.c`) keeps the per-fragment memory-alignment check
   unchanged, skips the joint rule when `seg_boundary == 0`, and otherwise
   tests joints against `seg_boundary` instead of `PAGE_SIZE` (more precise
   on 64 KiB-page kernels, where NVMe's boundary is still 4 KiB).
6. **nfsd + LOCALIO:** set `policy.seg_boundary` from `nf_dio_seg_boundary`
   in `nfsd_write_dio_iters_init()` (`fs/nfsd/vfs.c`) and in the
   `nfsd_file_dio_policy` op (`fs/nfsd/localio.c`), so LOCALIO's shared split
   gets it too.

Later, outside this sub-project's critical path: ext4 and other iomap
filesystems report the attribute the same way; man-pages `statx(2)`.

## 5. Phase 1 — KUnit (separate commits, after the feature commits)

- `nfsd-receive-bvec`'s `dio_segments_test` (drives `nfs_dio_split()`
  directly): new cases with `seg_boundary = 0` — fragments ending mid-page,
  4-byte aligned, go direct; 2-byte-aligned ones are still demoted — and with
  `seg_boundary = 4096` on a 64 KiB-page-style geometry. The existing cases
  (implicit 4 KiB boundary) keep their expectations.
- If `nfsd_file_get_dio_attrs()` gains logic worth isolating (the
  not-reported default), a case for it in the same suite.
- TESTING.md's KUnit coverage map gains the new cases.

## 6. Phase 1 — validation (both rigs must pass)

Positive rigs, boundary 0 (the relaxed gate must admit gapped payloads):

1. **XFS on brd.** brd has no virtual boundary and 4-byte DMA alignment.
2. **XFS on nvme-over-TCP to a local nvmet target.** The NVMe-oF host
   reports no boundary (`nvmf_get_virt_boundary()`), so this exercises the
   real NVMe driver stack without SGL hardware.

Negative rig, boundary 4096 (behaviour must not change): **nvme-loop**, the
existing project rig.

Per rig:

- **statx end to end:** a small test program (or `samples/vfs/test-statx.c`
  extended) shows `stx_dio_seg_boundary` = 0 on the positive rigs and 4096 on
  nvme-loop; the `nfsd_file_get_dio_attrs` tracepoint shows nfsd picked up
  the same value.
- **block acceptance:** extend `bvecrepro.c` with a gapped mode — 4-byte
  aligned fragments ending mid-page — and show it MATCHes on both positive
  rigs and fails with `-EINVAL` on nvme-loop (the documented root cause).
- **nfsd path:** `run-rig-cmp.sh`-style data comparison with loans on, plus
  the `nfsd_write_dio_split` disposition counts: on the positive rigs
  gapped-but-aligned loaned WRITEs go direct; on nvme-loop they are still
  demoted. `split_einval.bt` attached: zero `-EINVAL` everywhere.
- **Open question:** how to make loopback produce loaned payloads with
  mid-page joints on demand (loopback skbs are mostly page-tiled). KUnit and
  the gapped bvecrepro cover the logic deterministically; the runtime
  disposition counts need either such a generator or a real NIC.
- The full existing harness (KUnit, bvecrepro, loan-assert, kill-switch,
  system-correctness) and the LOCALIO A/B stay green.

## 7. Phase 2 — Xsight E1 (xeu) placement (after phase 1)

- **4-byte payload alignment without header-data split.** xeu posts every RX
  buffer at `XEU_RX_PAGE_HEADROOM = NET_SKB_PAD + NET_IP_ALIGN`; on arm64
  `NET_IP_ALIGN` is 0, so payload lands at 64 + 54 = 118, which is 2 mod 4.
  Ethernet is 14 bytes and IPv4/IPv6/TCP headers are always multiples of 4
  (VLAN adds 4), so a headroom that is 2 mod 4 — the classic
  `NET_IP_ALIGN = 2` — puts every payload on a 4-byte boundary and aligns
  the IP header too.
- **Continuation buffers:** a frame spanning several buffers is placed at the
  same headroom in each, so continuation buffers would start at 2 mod 4; they
  need a headroom that is 0 mod 4 and lengths that are multiples of 4. The
  per-descriptor `buf_off`/`buf_len` of the xeu paired-posting experiment
  (`xeu-hds-experiment/`) covers this. At MTU 1500 each frame fits one
  buffer.
- **Questions for Xsight:** does the E1 DMA engine accept a `dst_addr` that
  is 2 mod 4? Encapsulation (VXLAN/Geneve inner Ethernet shifts payload by
  14) breaks the rule — is it in scope?
- **Fragment lengths:** MSS 1448 and 8948 are multiples of 4; a
  window-limited sub-MSS segment of odd length demotes that one WRITE, which
  is correct.
- **Hardware check on tardis1:** `cat /sys/block/nvme*/queue/virt_boundary_mask`
  and `nvme id-ctrl … | grep sgls` to confirm its NVMe runs without a
  boundary.
- **Cost to measure:** a 1 MiB WRITE at MTU 1500 is ~725 fragments, so at
  least three SGL commands; at MTU 9000 ~118, one command. The per-segment
  SGL cost against the saved copy, and the receive-pass amplification in
  TESTING.md, are unmeasured.

## 8. Upstream considerations

- A statx field and mask bit need linux-fsdevel and man-pages review; the
  name and the size-versus-mask representation will be debated. The same
  question faces any O_DIRECT user submitting iovecs, which is the argument
  for it being generic rather than nfsd-private (compare Keith Busch's recent
  per-bio gap tracking, `bi_bvec_gap_bit`, and `806a0504895d` "block:
  validate user space vectors during extraction").
- The gate change depends on the statx plumbing but is otherwise local to
  this series.

## 9. Log

- 2026-09-26: scoped from a code read of `v7.1.13-14` + the series
  (sections 1–8). Mike: separate KUnit from core; xeu work is phase 2, after
  both boundary-free rigs pass; expose the attribute on the full path from
  inception, no shortcuts; separate claude.ai artifact for this line of work.
