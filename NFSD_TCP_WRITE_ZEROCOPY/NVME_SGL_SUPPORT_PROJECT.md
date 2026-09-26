# NVMe SGL support for loaned NFSD WRITE payloads

Sub-project of the NFSD TCP write-path zero-copy (page-loan) series, started
2026-09-26. Goal: let a loaned WRITE payload whose fragments are 4-byte
aligned but **not page-tiled** — fragments that end mid-page, successors that
start at a non-zero page offset — go to the device as direct I/O whenever the
device can take it, which on NVMe means the controller submits it with SGLs.

## 0. Status / handoff

- **Phase 1 implemented and runtime-qualified on `7.1.13-14.hs.440.loanpages`
  (2026-09-26; section 6 → "Results"); per-commit bisect walk 51/51
  clean, zero branch-introduced sparse findings.**
  The commits
  below on `kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY`, on top of the
  scoping commit `8e804a14b33a`. Every commit is build-checked with
  `CONFIG_WERROR=y` (the directories it touches plus their users). **KUnit
  (4K and 64K configs) and the validation rigs (section 6) are pending.**
  The review round's fixes were folded into the commits they fix
  (2026-09-26, per Mike: the branch has not been pushed), so there is no
  separate fix sprawl; section 9 keeps the record of what the review
  changed.

  | # | SHA | Subject |
  |---|-----|---------|
  | 1 | `72870a49cb2e` | block: add bdev_dio_seg_boundary() |
  | 2 | `79d0dc0bf66b` | fs: add STATX_DIO_SEG_BOUNDARY |
  | 3 | `2e6c4663bb74` | block: report STATX_DIO_SEG_BOUNDARY for block devices |
  | 4 | `a42fcad8c444` | xfs: report the direct I/O segment boundary |
  | 5 | `1b31a61beac9` | nfsd: zero the whole LOCALIO direct I/O policy before filling it |
  | 6 | `51af155f395f` | nfs_common: let the direct I/O policy carry the device's segment boundary |
  | 7 | `482d4414f9a3` | nfsd: record the direct I/O segment boundary of an nfsd_file |
  | 8 | `96fed0e7083b` | nfsd: split direct writes by the device's segment boundary |
  | 9 | `b2dc54218a0b` | loop: take the backing device's virtual boundary for direct I/O |
  | 10 | `b747c8804176` | nfsd: test the direct I/O segment boundary in the receive-bvec KUnit suite |
  | 11 | `7fcb1077753a` | nfsd: test the segment-boundary translation in the receive-bvec KUnit suite |

  Commits 1-9 are feature commits, 10-11 KUnit. Behaviour changes only at
  commit 8, and only for files whose file system sets
  `STATX_DIO_SEG_BOUNDARY` (XFS from commit 4); every earlier commit, and
  every caller that leaves the new policy field zero, runs today's code.
  Commit 9 (loop) came out of the review round and is outside the scope
  as set: loop is not built by the host `.config`, so it is only
  compile-checked, and **Mike has not yet decided** whether it stays here,
  moves to its own topic, or is dropped.
- Work lands on `kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY` as incremental
  commits at the tip, feature commits separate from KUnit commits (section 5).
- **Phase 1** (sections 4–6): expose the device's memory-segment boundary
  from the block layer, through XFS and statx, into `nfsd_file`, and let the
  DIO admission gate use it. Done when the validation rigs — primarily XFS
  on nvme-over-TCP to a local nvmet target, plus XFS on brd with
  512-byte-aligned fragments and the nvme-loop negative rig — pass with all
  the plumbing in place (section 6, including its 2026-09-26 corrections).
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

- **Meaning:** the memory-segment boundary for direct I/O, as a size, like
  `dio_mem_align`, not a mask. `0` — the device takes discontiguous memory
  segments anywhere (subject to `dio_mem_align`); otherwise a power of two
  `N` — every interior joint between memory segments must fall on a
  multiple of `N` bytes (a PRP NVMe device reports 4096). A joint is
  wherever the buffer is not physically contiguous, which includes page
  boundaries: with `N` larger than a page, even a page-tiled buffer has
  joints the device cares about.
- **Source:** the queue's **virtual boundary** (`virt_boundary_mask`, the
  gap rule `__bvec_gap_to_prev()` enforces), **not** `seg_boundary_mask`
  (which limits where a single segment may cross an address boundary).
- **Final names** (as implemented, following the existing DIO attributes at
  every layer):

  | Layer | Name |
  |-------|------|
  | block helper | `bool bdev_dio_seg_boundary(struct block_device *bdev, u32 *boundary)` (`include/linux/blkdev.h`, after `bdev_dma_alignment()`) |
  | uapi mask bit | `STATX_DIO_SEG_BOUNDARY` = `0x00040000U` |
  | uapi field | `__u32 stx_dio_seg_boundary` at 0xbc, replacing `__spare2[1]`; `__spare3` at 0xc0 and `sizeof(struct statx)` = 0x100 unchanged; synced to `tools/include/uapi/linux/stat.h` and `tools/perf/trace/beauty/include/uapi/linux/stat.h` |
  | kstat | `u32 dio_seg_boundary`, after `dio_read_offset_align` |
  | trace flag | `{ STATX_DIO_SEG_BOUNDARY, "DIO_SEG_BOUNDARY" }` in `show_statx_mask()` |
  | nfsd_file | `u32 nf_dio_seg_boundary`, after `nf_dio_read_offset_align` (policy encoding, not the raw statx value) |
  | policy | `u32 seg_boundary` in `struct nfs_dio_policy`, after `offset_align` |

  `stx_dio_seg_boundary` remains a placeholder that fsdevel may rename
  (e.g. `stx_dio_virt_boundary`).
- **`bdev_dio_seg_boundary()`** does the mask-to-size conversion in one
  place, shared by `bdev_statx()` and XFS. It returns false when the mask
  cannot be expressed as 0 or a power of two that fits in a `u32`
  (`mask > U32_MAX >> 1 || (mask & (mask + 1))`); the caller then leaves
  `STATX_DIO_SEG_BOUNDARY` unset. This replaces the scoped
  `bdev_virt_boundary()` raw-mask helper: with every caller doing
  `mask ? mask + 1 : 0` into a `u32`, a boundary of 2^32 or more (ublk
  passes a `__u64` `virt_boundary_mask` through) would wrap to 0 and read
  as "no boundary", the least strict answer. An unrepresentable boundary
  is now unreported, never 0.
- **"Not reported" means page-sized.** Because `0` is meaningful, the
  result-mask bit is what tells "reported" from "not reported", and a
  consumer that does not see the bit keeps today's rule: joints are tested
  against `PAGE_SIZE` (section 4, correction 1).

## 4. Phase 1 — core implementation (feature commits, no KUnit content)

Scoped as six commits (the scoping commit `8e804a14b33a` has the original
list); implemented as feature commits 1-9 of section 0. Per the series'
KUnit decoupling policy they carry no KUnit references in code or message.

What each does:

1. **block:** `bdev_dio_seg_boundary()` (section 3).
2. **fs:** `STATX_DIO_SEG_BOUNDARY`, `stx_dio_seg_boundary` (and the two
   tools copies), `kstat.dio_seg_boundary`, the copy in `cp_statx()`
   (`fs/stat.c`), the `show_statx_mask()` flag.
3. **block:** `bdev_statx()` reports it for block devices when requested.
4. **xfs:** `xfs_report_dioalign()` reports it from
   `xfs_inode_buftarg(ip)->bt_bdev` (so realtime files report the realtime
   device); `xfs_vn_getattr()` calls it for `STATX_DIO_SEG_BOUNDARY` too.
5. **nfsd (LOCALIO prep, no behaviour change):** `nfsd_file_dio_policy()`
   (`fs/nfsd/localio.c`) starts from one compound-literal assignment of the
   whole policy, then sets the direction-specific fields.
6. **nfs_common:** `struct nfs_dio_policy` gains `seg_boundary`,
   `NFS_DIO_SEG_BOUNDARY_NONE` and `nfs_dio_seg_boundary()`
   (`include/linux/nfs_dio.h`); `nfs_dio_split()` computes
   `nfs_dio_seg_mask(policy)` and passes it to
   `nfs_dio_iter_aligned_and_splittable()` (`fs/nfs_common/nfs_dio.c`).
7. **nfsd:** `fh_getattr()` requests the bit for regular files;
   `nfsd_file_get_dio_attrs()` stores
   `nfs_dio_seg_boundary(reported, stat.dio_seg_boundary)` in
   `nf_dio_seg_boundary` (zeroed in `nfsd_file_alloc()`); tracepoint change.
8. **nfsd + LOCALIO:** `nfsd_write_dio_iters_init()` (`fs/nfsd/vfs.c`) and
   `nfsd_file_dio_policy()` pass `nf_dio_seg_boundary` into the policy. The
   only commit that changes behaviour.

Corrections to the scoping (2026-09-26):

1. **Unknown = `PAGE_SIZE`, not 4096.** Defaulting a not-reported boundary to
   4096 would relax the joint rule on 64 KiB-page kernels for every file
   system that does not report it. The page-sized default is bit for bit
   the `~PAGE_MASK` the gate tested before, so "not reported" behaves
   exactly as today.
2. **Policy encoding** of `nfs_dio_policy.seg_boundary` (and
   `nf_dio_seg_boundary`):
   - `0` — not known: `nfs_dio_seg_mask()` returns `PAGE_SIZE - 1`, today's
     code path. Every caller that leaves the field zero (designated
     initializers that omit it, `nfsd_file_alloc()`, `vfs_getattr_nosec()`'s
     memset kstat) keeps today's behaviour.
   - `NFS_DIO_SEG_BOUNDARY_NONE` (`U32_MAX`) — no boundary: mask 0, so
     `((off + len) | next->bv_offset) & seg_mask` is always 0 and the joint
     rule never fires. The per-fragment `(off | len) & (mem_align - 1)`
     check, the total-length `offset_align` check and the skip accounting
     are unchanged.
   - a power of two `N` — mask `N - 1`, mirroring `__bvec_gap_to_prev()`
     (`block/blk.h`).

   `nfs_dio_seg_boundary(reported, b)` maps a statx report to it the same
   way for every producer: not reported → 0; `b == 0` → NONE; power of two
   → `b`; anything else → 0. `bdev_dio_seg_boundary()` already refuses
   unrepresentable masks, so the power-of-two check is defence against
   other file systems. `struct nfs_dio_policy` grows from 16 to 20 bytes
   (the scoping's "no new padding" expectation was wrong).
3. **Order:** the nfs_common commit comes before the nfsd_file commit, so
   nfsd can store the policy encoding from the start.
4. **LOCALIO prep commit (5).** The NFS client declares
   `struct nfs_dio_policy policy;` on its stack uninitialized
   (`fs/nfs/localio.c`) and `nfsd_file_dio_policy()` filled it field by
   field, so a new field it did not name would reach `nfs_dio_split()` as
   stack garbage at every commit between 6 and 8. With whole-struct
   compound-literal assignments every unnamed field is zero.
5. **`bdev_statx()` reporting (3)**, not in the scoping: an application doing
   direct I/O to the raw device sees the same attribute as a file on it.
6. **Tracepoint:** `nfsd_file_get_dio_attrs` gains a third argument, the
   policy encoding, and records both the raw `stat->dio_seg_boundary`
   (`seg_boundary=`) and the boundary the split enforces
   (`joint_boundary=`: 0 = none, `PAGE_SIZE` = unknown, else `N`). The call
   moved after the `nf_*` assignments. `nfsd_write_dio_split` is **not**
   extended: it already has 12 arguments, and
   `include/trace/bpf_probe.h` says tracepoints with more than 12 arguments
   hit a build error.

Behaviour at commit 8, for files whose file system reports the bit: a
reported 4096 on a 4K-page kernel is identical to today; a reported 0 drops
the joint rule; a reported 4096 on 64K pages tests joints on 4 KiB
multiples; a reported boundary above `PAGE_SIZE` makes page joints count as
discontinuities, so a payload is demoted at a page joint whose position is
not an `offset_align` multiple — the block layer's gap rule, and
conservative.

Later, outside this sub-project's critical path — follow-up reporters, each
next to its existing `STATX_DIOALIGN` handling: `ext4_getattr()`
(`fs/ext4/inode.c` ~6192), `f2fs_getattr()` (`fs/f2fs/file.c` ~1006), the NFS
client (`fs/nfs/inode.c` ~1181); man-pages `statx(2)`.

## 5. Phase 1 — KUnit (separate commits, after the feature commits)

Implemented as commits 9 and 10 (section 0); not yet run.

- **`dio_segments_test`** (`fs/nfsd/nfsd_bvec_kunit.c`, drives
  `nfs_dio_split()` directly) gains a `seg_boundary` column, passed into the
  test's policy. The existing rows leave it zero (unknown = page-sized) and
  keep their expectations. Nine new rows, all `mem_align` 4,
  `offset_align` 512 unless noted:

  | # | Row | Geometry | 4K pages | 64K pages |
  |---|-----|----------|----------|-----------|
  | 1 | `no-boundary-mid-page-joint-direct` | NONE; lengths 4092/4/4096 | direct | direct |
  | 2 | `no-boundary-tcp-segments-direct` | NONE; offsets 0/2048/512, lengths 1448/1448/1200 | direct | direct |
  | 3 | `default-boundary-tcp-segments-buffered` | unknown; row 2's geometry | buffered | buffered |
  | 4 | `no-boundary-2-byte-fragment-buffered` | NONE; lengths 4094/2/4096 (memory alignment still enforced) | buffered | buffered |
  | 5 | `boundary-4096-joint-on-boundary-direct` | 4096; offsets 100/0/0, lengths 3996/4096/100 (joints on 4 KiB multiples at payload byte 3996) | direct | direct |
  | 6 | `default-boundary-joint-on-4096` | unknown; row 5's geometry | direct | **buffered** |
  | 7 | `boundary-4096-mid-page-joint-buffered` | 4096; lengths 4092/4/4096 | buffered | buffered |
  | 8 | `boundary-above-page-page-tiled-buffered` | 2 x `PAGE_SIZE`, `offset_align` 2 x `PAGE_SIZE`; two full pages | buffered | buffered |
  | 9 | `default-boundary-page-tiled-direct` | unknown; row 8's geometry | direct | direct |

  Rows 5 and 6 differ only on a 64K-page kernel, so the 64K KUnit config
  must be run for them to prove anything.
- **`nfsd_bvec_dio_seg_boundary_test`** (new case, commit 10): the
  `nfs_dio_seg_boundary()` translation — unreported 0 and 4096 → 0, reported
  0 → NONE, 4096 and 65536 kept, 3000 → 0. The helper is a static inline, so
  the suite reaches it without an export. `nfsd-receive-bvec` goes from 5 to
  6 cases.
- TESTING.md's KUnit coverage map and "Next regression run" carry the new
  counts.

## 6. Phase 1 — validation (all rigs must pass)

Positive rigs, boundary 0 (the relaxed gate must admit gapped payloads):

1. **XFS on nvme-over-TCP to a local nvmet target — the primary positive
   rig.** The NVMe-oF host reports no boundary (`nvmf_get_virt_boundary()`)
   and `nvme_set_ctrl_limits()` (`drivers/nvme/host/core.c` ~2063) sets
   `dma_alignment = 3`, so XFS reports `dio_mem_align` 4 and
   `dio_seg_boundary` 0, and this exercises the real NVMe driver stack
   without SGL hardware.
2. **XFS on brd, with 512-byte-aligned fragments only.** *Correction
   (2026-09-26):* brd does **not** have 4-byte DMA alignment. `brd.c` sets
   no `.dma_alignment`, so `blk_validate_limits()` defaults it to
   `SECTOR_SIZE - 1` (`/sys/block/ram0/queue/dma_alignment` = 511), and XFS
   on brd reports `dio_mem_align` 512. MSS-sized TCP fragments (1448, 8948)
   are then demoted by the memory-alignment check whatever the boundary, so
   a brd row proves the boundary change only with fragments whose offsets
   and lengths are 512-byte multiples.

Negative rig, boundary 4096 (behaviour must not change): **nvme-loop**, the
existing project rig.

Per rig:

- **Assert the attribute first.** Before any positive result is trusted, the
  `nfsd_file_get_dio_attrs` tracepoint must show `DIO_SEG_BOUNDARY` in
  `flags=` with the expected `joint_boundary=` (0 on the positive rigs, 4096
  on nvme-loop). An export whose file system does not report the bit — ext4,
  for one — keeps the page-sized rule and would silently test nothing.
- **statx end to end:** `statx-dio.c` (this directory; build line in its
  header) on a file of each rig's XFS shows `DIO_SEG_BOUNDARY` in `stx_mask`
  with `dio_seg_boundary` = 0 on the positive rigs and 4096 on nvme-loop
  (exit status 1 if the bit is missing); on the raw block device it shows
  the `bdev_statx()` report. The tracepoint shows nfsd picked up the same
  value.
- **block acceptance:** extend `bvecrepro.c` with a gapped mode — fragments
  ending mid-page, 4-byte aligned (512-byte aligned on brd) — and show it
  MATCHes on the positive rigs and fails with `-EINVAL` on nvme-loop (the
  documented root cause).
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

### Results — `7.1.13-14.hs.440.loanpages` (2026-09-26)

Kernel built by Mike from `5fa44f5060e6` with `NVME_TCP=m`,
`NVME_TARGET_TCP=m`, `BLK_DEV_LOOP=m` added; all 21 relevant installed
modules (sunrpc, nfsd, nfs, nfs_localio, nfs_dio, xfs, loop, nvme-tcp,
nvmet-tcp, nvme-fabrics, nvmet, nvme-loop, brd, the four KUnit modules)
srcversion-match the tree. Rigs: `run-sgl-rigs.sh up` (brd RAM disks:
XFS on `ram0`; nvmet over NVMe/TCP on 127.0.0.1 backed by `ram1`; nvmet over
nvme-loop backed by `ram2`; NVMe devices found by subsystem NQN only).

1. **KUnit:** `sunrpc-xdr-bvec` 7/7, `sunrpc-svcsock-rx` 54/54,
   `nfsd-receive-bvec` **6/6** (adds `nfsd_bvec_dio_seg_boundary_test`),
   `nfsd4-receive-bvec` 9/9. `dio_segments_test` ran all 25 rows (the 12
   existing + 13 segment-boundary rows, skipped-first-fragment ones
   included). No WARN/BUG.
2. **Queue limits as predicted:** brd `virt_boundary_mask=0
   dma_alignment=511`; NVMe/TCP `0 / 3`; nvme-loop `4095 / 3`.
3. **statx end to end** (`statx-dio.c`), `STATX_DIO_SEG_BOUNDARY` set in
   `stx_mask` everywhere: XFS on NVMe/TCP `dio_seg_boundary=0`
   (`dio_mem_align=4`); XFS on brd `0` (`512`); XFS on nvme-loop `4096`
   (`4`); the raw devices report the same through `bdev_statx()`; the host's
   VMware NVMe (`sgls: 0`) reports `4096`.
4. **nfsd picks it up:** `nfsd_file_get_dio_attrs` shows
   `DIO_SEG_BOUNDARY` in `flags=` with `seg_boundary=0 joint_boundary=0` on
   NVMe/TCP and brd, `4096 / 4096` on nvme-loop.
5. **Block acceptance** (`bvecrepro.c`'s new gapped mode, `frag=` /
   `frag_off=`): NVMe/TCP takes 1448-byte fragments at page offset 64
   (MATCH, ~725 segments split legally) and refuses them at offset 66
   (`-22`: the 4-byte rule still holds); nvme-loop refuses the 4-aligned
   gapped payload (`-22`, the documented root cause) but takes 2048-byte
   fragments, whose gaps sit on 512-multiples (MATCH); brd copies and so
   takes anything (its reported 512 is conservative). The existing
   `off0=684` row MATCHes on all three. (Fragments must fit one page:
   `frag=8948` is refused by the module on 4 KiB pages.)
6. **nfsd WRITE path** (`run-sgl-nfsd-cmp.sh`, 10 × 16 MiB O_DIRECT per rig,
   loans on, `io_cache_write=2`, LOCALIO off, `split_einval.bt` attached):
   0 `dd` failures, 0 `cmp` mismatches on every rig, no `-EINVAL` (6741
   unsplit bios, 161 clean splits). Dispositions: NVMe/TCP **160/160
   direct**; nvme-loop 156 direct + 4 demoted (`mem_misaligned`, the gate
   still demoting geometry that device cannot split); brd 160 demoted,
   expected — loaned payloads start at RPC-header offsets that are not
   512-aligned. ~168 MB borrowed per rig. The NVMe/TCP run's 5 locked-head
   records all went direct where nvme-loop demoted 4 of its 10, consistent
   with the relaxation but not attributable without a tracepoint on the
   gate's joint decision; KUnit and the gapped bvecrepro are the
   deterministic proof.
7. **Loop commit:** a direct-I/O loop device over a file on XFS-on-nvme-loop
   reports `virt_boundary_mask=4095` and `dio_seg_boundary=4096` (raw and
   for XFS on it); over XFS-on-NVMe/TCP `0 / 0`; a buffered loop device
   `0 / 0`. Without the commit the first case would report 0.
8. **Existing harness green:** `run-rig-cmp.sh` (0 errors, 457 direct),
   `run-loan-assert.sh` 10/10, `run-killswitch.sh`,
   `run-system-correctness.sh`, `run-bvecrepro.sh` 4/4.
9. **LOCALIO on a boundary-free device** (`run-sgl-localio-ab.sh`; uses the
   tree-root `start-nfsd.sh` and `../NFS_LOCALIO_DONTCACHE/` harness). Same
   tmpfs-file nvmet backing, only the transport changed:

   | | NVMe/TCP (boundary 0) | nvme-loop (4096) |
   |---|---|---|
   | policy the LOCALIO opens got | `seg_boundary=0 joint_boundary=0` (9 opens, 0 RPC WRITEs) | `4096 / 4096` |
   | rs=47008 reads/WRITE, resident | 0.008, 0.0% | 0.010, 0.0% |
   | rs=6000 reads/WRITE, resident | 0.001, 0.0% | 0.000, 0.0% |
   | flusher rounds; short-read; stability | 0; 5/5; FILE_SYNC, no COMMIT | same |

   The relaxed policy reaches LOCALIO and LOCALIO stays correct on such a
   device. No measurable LOCALIO benefit is expected or seen: its pinned
   O_DIRECT buffers are page-tiled after the first entry, so the joint rule
   never had anything to reject. **Rig caveat:** on brd-backed nvmet
   namespaces the LOCALIO boundary test fails identically for both
   boundaries (0.879 reads/WRITE at rs=47008, over the ceiling) — a backing
   artifact, not the boundary; use tmpfs-file backing as above.
10. **Per-commit bisect walk** (`run-bisect-walk.sh` from a scratch copy,
    cold worktree, `BASE=v7.1.13-14`, running config minus debug info with
    `WERROR=y` and the four suites `=m`; `NVME_TCP`, `NVME_TARGET_TCP` and
    `BLK_DEV_LOOP` built): **51/51 clean**, 0 failures (44 built, 7 docs-only
    skipped), sparse verdict **zero branch-introduced findings** against a
    cold whole-tree baseline. The loop commit is built and checked here.
11. **NVMe SGL selection:** for a gapped request nvme-pci does not depend on
    `sgl_threshold`: `nvme_pci_use_sgls()` returns `SGL_FORCED` whenever
    `req_phys_gap_mask(req) & (NVME_CTRL_PAGE_SIZE - 1)` (gaps tracked per
    bio since `2f6b2565d43c`), and the average-segment heuristic applies
    only to gap-free requests. Not exercisable here (no SGL-capable PCIe
    NVMe; NVMe/TCP has no PRP/SGL choice) — confirm on tardis1.

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
- 2026-09-26: phase 1 implemented as ten commits, each
  build-checked with `CONFIG_WERROR=y`; KUnit (4K and 64K) and the rigs
  pending. Departures from the scoping recorded in sections 3-4 (unknown =
  `PAGE_SIZE`; the 0 / NONE / power-of-two policy encoding and
  `nfs_dio_seg_boundary()`; `bdev_dio_seg_boundary()` refusing
  unrepresentable masks; nfs_common before nfsd_file; the LOCALIO
  compound-literal prep commit; `bdev_statx()` reporting; the tracepoint's
  `joint_boundary=`). Section 6 corrected: brd's DMA alignment is 511, so
  nvme-tcp over loopback is the primary positive rig, and every rig first
  asserts the reported boundary in `nfsd_file_get_dio_attrs`. Added
  `statx-dio.c` for the statx end-to-end check.
- 2026-09-26: review round, six commits (since folded, see below), each
  build-checked with
  `CONFIG_WERROR=y` (loop compile-checked only: `CONFIG_BLK_DEV_LOOP` is
  off in this `.config`). Findings acted on: (a) a DIO-mode **loop device
  reported no boundary** because it copied only `dma_alignment` from its
  backing file, so XFS on loop-over-PRP-NVMe reported 0, nfsd admitted
  joints the backing queue cannot split, and the WRITE could fail with
  `-EINVAL` partway through; loop now takes the backing
  `STATX_DIO_SEG_BOUNDARY` (else the backing `s_bdev`'s virt boundary)
  as its own `virt_boundary_mask` while direct I/O is on (now commit 9). The gate
  comment now says "no boundary" is only as good as the reporting queue's
  limits: a stacking driver that forwards bvecs unchanged must carry the
  lower `virt_boundary_mask` up (dm/md via `blk_stack_limits()`, loop via
  12). (b) the uapi comment now states the user-visible contract only
  (folded into 2). (c) the policy -> joint-boundary decode is shared as
  `nfs_dio_joint_boundary()` by the gate and the tracepoint, and the
  tracepoint's `seg_boundary=` prints -1 when the bit is not reported
  (folded into 6 and 7). (d) KUnit rows with a non-zero write position exercise the
  gate's skipped-first-fragment path under a non-zero segment mask (folded into 10).
  Left as is: the `checkpatch --strict` alignment CHECK on the
  `TP_printk` continuation, which matches the surrounding nfsd `trace.h`
  style. KUnit and the rigs still pending; add a loop(dio)-over-XFS-over-
  nvme-loop negative rig to section 6 (the loop device must report 4096).
- 2026-09-26: folded per Mike (the branch has not been pushed): each review
  fix went into the commit it fixed (the uapi-comment fix into 2, the
  shared joint-boundary decode into 6, the tracepoint fix into 7, the
  LOCALIO braces into 5, the skipped-first-fragment rows into 10, the
  review-round docs into the phase-1 docs commit), the loop commit moved
  in among the feature commits, and messages were updated for what each
  commit now contains. The tree is byte-identical to the pre-fold tip; the
  pre-fold history is kept on
  `kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY.pre-fold`. The same fold put
  the merge top-up fix into "SUNRPC: merge locked-head copies into a
  whole-page loan bvec".
