# Bio-based driver audit — sub-sector bvec position handling

The brd corruption (see `TESTING.md` → "The root cause") defined a defect
class: a driver that walks a bio per-segment and re-derives its device
position from `bi_iter.bi_sector` — or advances its own sector cursor by
`bv_len >> SECTOR_SHIFT` — silently loses the sub-sector residue whenever a
bvec's length is not a sector multiple. The loaned-receive geometry produces
exactly that shape (`bv0 = (off0, PAGE_SIZE - off0)` with arbitrary `off0`),
and so does the legacy copied-arena direct path (`bv0 ≈ (160, …)`). The
failure is a silent forward shift of every byte after the first segment:
`onset = ALIGN_DOWN(bv0.len, 512)`, `shift = bv0.len - onset`.

This file tracks the audit of the other bio-based backing drivers before
loaned DIO (`io_cache_write=2`) is trusted over them. Request-based drivers
(real NVMe, SCSI) are out of scope: the request path does no per-bvec sector
arithmetic.

## Method

Per driver, two passes:

1. **Source review** — find every per-segment walk in the I/O path and
   classify how it derives the device position: byte cursor advanced by the
   segment's actual consumption (safe) vs sector cursor advanced by
   `len >> SECTOR_SHIFT` (exposed when `len` can be a non-sector multiple).
2. **Empirical** — `run-bvecrepro.sh` rows against the driver's device:
   poison `off0=684`, legacy `off0=160`, aligned `off0=0`. The reproducer
   submits the exact `bio_iov_bvec_set()` shape and compares read-back
   against a stamped pattern.

## Verdicts

| driver | verdict | date |
|---|---|---|
| brd | **EXPOSED — fixed** ("brd: iterate the bio by byte position, not bi_sector") | 2026-09-05 |
| null_blk | **not exposed** (bio path removed upstream; MQ path byte-accurate) | 2026-09-05 |
| dm core (dm.c) + linear + striped | **not exposed** (structurally immune — see below) | 2026-09-05 |
| dm-crypt | **not exposed** (per-bvec guard fails non-512-multiple bvecs with EIO; no silent path) | 2026-09-05 |
| dm-flakey | **not exposed** (byte cursors; whole-bio remap) | 2026-09-05 |
| dm-verity | **not exposed** (read-only; fixed-size byte advance; spans-pages guard) | 2026-09-05 |
| dm-vdo | **not exposed** (byte cursors; position derived once from `bi_sector`) | 2026-09-05 |
| dm-bufio (thin/cache/era metadata substrate) | **not exposed** (own aligned buffers only) | 2026-09-05 |
| dm-thin, dm-cache, dm-snap, dm-raid1 (own code), dm-clone, dm-era, dm-delay, dm-dust, dm-switch, dm-unstripe, dm-zero, dm-zoned | **not exposed** (zero segment-iteration sites — linear-class remap/clone) | 2026-09-05 |
| dm-io `do_region()` | **EXPOSED** (sector-truncating accounting; reachable via `DM_IO_BIO` → dm-raid1) | 2026-09-05 |
| dm-log-writes | **EXPOSED** (log-device placement; data device pass-through intact) | 2026-09-05 |
| dm-writecache | **EXPOSED** (PMEM mode; SSD mode safe) | 2026-09-05 |
| dm-integrity | **EXPOSED** (6 sites at default `sectors_per_block=1`; guard only arms at >1) | 2026-09-05 |
| dm-ebs | **EXPOSED in source** (parked; first empirical run only exercised the aligned pass-through) | 2026-09-05 |
| md core + raid0/1/10/5 (all personalities) | **not exposed** (`bio_split_to_limits()` front gate rejects with EINVAL; internals byte-accurate) | 2026-09-05 |
| zram | **EXPOSED — fixed** ("zram: handle sub-page bvec segments without corrupting data"; two defects, see below) | 2026-09-05 |

### Systemic mitigation — why NFSD loaned DIO cannot reach the dm exposures

Every bio-based dm queue advertises `dma_alignment >= 511`: the stacking
limits start at `SECTOR_SIZE - 1` (block/blk-settings.c:44), validation
restores 511 for a zero value (:495), and stacking takes the max (:858) —
several targets then raise it further (`logical_block_size - 1` in crypt,
verity, integrity, log-writes). So `statx dio_mem_align >= 512` on every dm
device, and `nfsd_dio_iter_is_aligned()` demotes any loaned iterator with
sub-sector bvec offsets/lengths to the buffered path before the dm stack
sees it — the same defense that protected brd-direct. The EXPOSED verdicts
below are therefore upstream driver bugs in their own right, reachable by
in-kernel bio producers that bypass statx-style gating (as `bvecrepro`
does), not by this series' NFSD path. DM core enforces none of this on
incoming normal I/O (`bio_split_to_limits()` runs only for abnormal/zoned
bios; dm.c:1979) — each target's own guard is the only enforcement.

### null_blk — not exposed (2026-09-05)

- **Bio mode no longer exists.** Upstream `8b631f9cf0b8` ("null_blk: remove
  the bio based I/O path", 2024-02, in this tree) deleted it;
  `queue_mode=0` logs "BIO-based IO path is no longer available, using
  blk-mq instead" and upgrades to blk-mq (`null_validate_conf()`).
- **The surviving memory-backed path is byte-accurate.**
  `null_handle_data_transfer()` keeps a byte cursor
  (`loff_t pos = blk_rq_pos(rq) << SECTOR_SHIFT`) advanced by `pos += len`
  — the segment's actual byte length — and `copy_to_nullb()` /
  `copy_from_nullb()` address by byte `pos` throughout. This is the same
  pattern the brd fix adopted. The one `sector += temp >> SECTOR_SHIFT`
  (in `null_handle_discard()`) only ever sees sector-multiple `temp`
  (`min(nr_sectors << 9, blocksize)`) and carries no bvec payload.
- **Empirical:** all three bvecrepro rows MATCH against
  `modprobe null_blk nr_devices=1 memory_backed=1 queue_mode=0` on
  `7.1.8-3.hs.159.el9.aarch64+` (poison 684 / legacy 160 / aligned 0,
  1 MiB each, byte-identical read-back).

### dm core, dm-linear, dm-stripe — not exposed (2026-09-05)

- **No per-segment walk anywhere in the path.** `linear_map()` and
  `stripe_map()` only remap `bio->bi_iter.bi_sector` for the whole bio;
  neither touches a bvec. Splitting belongs to DM core
  (`dm_set_target_max_io_len()` → `bio_split()`), which advances the
  iterator byte-accurately (`bi_bvec_done`) and only ever at
  sector-multiple boundaries — so the `bytes >> 9` advance inside
  `bvec_iter_advance()` never sees a sub-sector residue. A split landing
  mid-bvec hands the *underlying* device a `bi_bvec_done`-offset bio,
  which is the underlying driver's exposure (brd's, before its fix), not
  DM's.
- **Empirical:** all three rows MATCH on `dm-linear` over the patched brd,
  and on a 2-disk `dm-stripe` with 4K chunks (`striped 2 8`) — the 1 MiB
  poison bio forced ~256 sector-granular splits, many mid-bvec, through
  DM core onto brd, all byte-identical.

### dm-crypt — not exposed (2026-09-05)

- **A per-bvec guard makes the failure loud, not silent.** Both convert
  paths reject a bvec whose remaining length is not a multiple of the
  crypto sector: `crypt_convert_block_skcipher()` /
  `crypt_convert_block_aead()` — "Reject unexpected unaligned bio" —
  `if (bv_in.bv_len & (cc->sector_size - 1)) return -EIO`. The sg
  construction (`sg_set_page(..., cc->sector_size, bv_in.bv_offset)`)
  assumes a whole crypto sector inside one bvec, and the guard enforces
  exactly that.
- **Position arithmetic is safe by construction.** `ctx->cc_sector`
  advances by `sector_step` (the crypto sector, a fixed 512-multiple) per
  block, never by a bvec length — no residue to lose.
- **The queue limits keep NFSD out of the EIO path.** `crypt_io_hints()`
  sets `dma_alignment = logical_block_size - 1` (≥ 511), so
  `statx dio_mem_align` ≥ 512 and `nfsd_dio_iter_is_aligned()` demotes
  every loaned mid-page iterator to the buffered path before dm-crypt
  sees it; an iterator that passes the 512 gate also satisfies the
  per-bvec guard.
- **Empirical** (`aes-xts-plain64` over the patched brd, plain `dmsetup`
  table): poison `off0=684` and legacy `off0=160` fail the WRITE with
  `-EIO` (rejected, nothing written); 512-aligned mid-page `off0=512`
  and aligned `off0=0` MATCH byte-identically.

## Full DM sweep (2026-09-05)

Method for the sweep: a mechanical scan of all 94 `drivers/md/dm*.c`
files (plus `dm-vdo/`, `persistent-data/`) for segment iteration
(`bio_for_each_segment`, `bio_for_each_bvec`, `bio_iter_iovec`,
`bio_advance_iter`, `bvec_iter_advance`), parallel per-file source audits
of every hit, key claims re-verified against the tree line-by-line, and
empirical `bvecrepro` runs where the rig allows. Targets with zero
segment-iteration sites (thin, cache, snap, clone, era, delay, dust,
switch, unstripe, zero, zoned; mpath is request-based) are linear-class:
they remap/clone whole bios and can neither create nor consume the
defect.

### dm.c core — not exposed (structurally immune)

Zero bvec dereferences in the file. `__split_and_process_bio()` carries a
private `ci->sector` cursor advanced by the same `len` that sets the
clone's `bi_size` — position is never re-derived from an advanced
iterator. Clones share the parent's bvec table with the `bvec_iter`
copied wholesale (`bi_bvec_done` included), so mid-bvec splits stay
byte-exact. `dm_accept_partial_bio()` only truncates `bi_size` in place.
The one caveat is systemic, not a walk defect: dm core never validates
`dma_alignment` on incoming normal I/O, so each target's guard is the
only enforcement (see "Systemic mitigation" above).

### dm-flakey, dm-verity, dm-vdo, dm-bufio — not exposed

- **dm-flakey**: whole-bio remap like linear; the corrupt-byte feature
  and the corrupt-write clone both walk by byte cursors
  (`bvec_iter_advance`), affecting only *which* byte is deliberately
  corrupted, never placement.
- **dm-verity**: read-only (`verity_map()` kills WRITEs). The hash walk
  advances by a fixed `block_size` per iteration — independent of bvec
  lengths — and a block spanning bvecs trips an explicit `-EIO` guard
  ("data block spans pages"). `dma_alignment = block_size - 1`.
- **dm-vdo**: `copy_from_bio()`/`copy_to_bio()` are pure byte cursors
  into VDO's own 4K buffers; lbn/offset derived from `bi_sector` exactly
  once in `launch_bio()`; DM splits every bio at 4K boundaries
  (`dm_set_target_max_io_len(ti, VDO_SECTORS_PER_BLOCK)`).
- **dm-bufio** (metadata substrate for thin/cache/era via
  dm-block-manager): submits only its own buffers; `submit_io()` derives
  the sector from the block number once and aligns the write range to
  `max(DM_BUFIO_WRITE_ALIGN, physical_block_size)` before any sector
  conversion. Client bvec geometry never enters — insulating dm-thin's
  heavy bufio use by construction.
- **persistent-data/** (dm-block-manager → transaction-manager →
  btree/array/bitset/space-maps, the metadata stack under dm-thin,
  dm-cache, and dm-era): **bio-free by construction** — zero references
  to bios or bvecs anywhere in the layer; every I/O is a
  `dm_bufio_read/new/write` by `(client, block_nr)` funneled through
  `dm-block-manager.c`, executed from bufio's own aligned buffers.
  `dm-cache-metadata.c` and the smq policy are likewise bio-free, and
  dm-cache's data migrations ride dm-kcopyd, which feeds dm-io
  *page-list* dpages (its own page-granular memory) — never the exposed
  `DM_IO_BIO` flavor.

### dm-io `do_region()` — EXPOSED (verified line-by-line)

The page walk itself is byte-accurate, but the accounting is not:
`remaining -= to_sector(len)` debits a truncated sector count against a
byte-length `bio_add_page()`, and a continuation bio anchors at
`where->sector + (where->count - remaining)` — sector-granular. A
non-512-multiple segment (`len=3412` → 6 sectors debited, 3412 bytes
attached) over-fills the region, misplaces any continuation bio, and can
loop forever once the source iterator drains (`len=0` → no progress).
Reachable only when a `DM_IO_BIO` client feeds an incoming bio's
geometry: the in-tree users are **dm-raid1**'s read/write paths
(`.mem.type = DM_IO_BIO, .mem.ptr.bio = bio`). All other dpage flavours
(page-list/vma/kmem — bufio, kcopyd, snapshots) are the caller's own
page-granular memory and cannot trigger it.

### dm-log-writes — EXPOSED (verified line-by-line)

`log_one_block()` advances a private cursor `sector +=
block->vecs[i].bv_len >> SECTOR_SHIFT` while `bio_add_page()` consumes
the full byte length; when a log bio fills and a continuation bio is
allocated, it anchors at the drifted `sector`, silently overwriting the
tail of the just-submitted log bio. The matching accounting half:
`block->nr_sectors = bio_to_dev_sectors(lc, bio_sectors(bio))`
truncates, so log-space reservation and the replayed `entry.nr_sectors`
undercount. The **data device is untouched** — `normal_map_bio()` passes
the original bio through intact — so the corruption is confined to the
log/replay stream, which is the target's entire purpose.

### dm-writecache — EXPOSED in PMEM mode (verified line-by-line)

`bio_copy_block()` copies via `bio_iter_iovec(bio, bio->bi_iter)` +
`bio_advance(bio, size)` where `size` is the raw `bv_len`: the pmem-side
byte cursor is exact, but `bio_advance()` moves `bi_sector` by
`size >> 9`, and both callers (`writecache_map_write()`,
`writecache_map_read()`) re-derive the cache-entry lookup **and** the
persisted `write_original_sector_seq_count()` from the drifted
`bi_iter.bi_sector` on every block of the loop. Writeback then replays
the wrong origin sector from metadata. The SSD mode never walks bvecs
(block-count arithmetic + whole-bio remap) and is safe. The map-time
guard checks only `bi_sector | bio_sectors` against `block_size` —
per-bvec geometry passes. No pmem device on this rig, so the verdict is
source-only.

### dm-integrity — EXPOSED at six sites in the default config (spot-verified)

The one per-bvec alignment guard (`dm_integrity_check_limits()`) is
gated on `ic->sectors_per_block > 1` — at the **default**
`sectors_per_block = 1` nothing validates bvec lengths, unlike
dm-crypt's unconditional guard. Beyond position skew, two sites
degenerate worse: the journal copy loop
(`while (bv.bv_len -= sectors_per_block << SECTOR_SHIFT)`) underflows an
unsigned `bv_len` (340 − 512) and runs off the kmapped page into
successive journal entries, and the inline-mode walks
(`bio_advance_iter_single(..., 512)`) never satisfy
`bvec_iter_advance_single()`'s exact-equality bvec-boundary test with a
3412-byte bvec, so `bi_idx` sticks, checksums read the wrong page, and
the `while (bi_size)` termination wraps. Raising `block_size:` to 4096
both arms the guard and tightens `dma_alignment` — the default 512
config is the unguarded one.

### dm-ebs — EXPOSED in source; parked

`__ebs_rw_bvec()` derives `block`/`buf_off` from `iter->bi_sector` per
bvec while `bio_for_each_bvec()` advances that sector by
`bv_len >> 9` — the exact brd pattern (two independent reads of the code
agree). The first empirical run (3/3 MATCH, `ebs 1 8` over patched brd)
is explained by `ebs_map()`'s routing: sector-aligned bios take the
direct pass-through remap and never reach the bufio walker; only
sector-misaligned bios or an `e_bs == u_bs` table enter it. Parked
before the targeted empirical run — the module also isn't built in this
kernel config (an out-of-tree build works: `obj-m := dm-ebs.o` +
`ccflags-y := -I$(srctree)/drivers/md`).

## MD sweep (2026-09-05)

Scanned md.c, md-bitmap.c, md-linear.c, raid0.c, raid1.c, raid1-10.c,
raid10.c, raid5.c, raid5-cache.c, raid5-ppl.c for segment iteration and
data-access idioms. Verdict: **not exposed, at two independent layers.**

### The front gate — MD enforces what DM delegates

`md_submit_bio()` runs **every** incoming bio through
`bio_split_to_limits()` (md.c:443), which validates per-bvec geometry
against the queue's `dma_alignment`/logical-block limits and fails
violators with `BLK_STS_INVAL`. This is the enforcement dm core
deliberately skips for normal I/O — the structural difference between
the two stacks. Empirically (mdadm arrays over the patched brd,
`--assume-clean`, 64K chunks): **raid0 (2-disk), raid1 (2-disk),
raid10 (4-disk), and raid5 (3-disk) all fail the poison `off0=684` and
legacy `off0=160` writes with `-EINVAL` and MATCH the aligned row
byte-identically.** Loud rejection, nothing written, no silent path.

### The internals are byte-accurate anyway

Only two segment walks exist in all of MD, and neither consumes a
drifted sector:

- **raid5 `async_copy_data()`** (raid5.c:1362-1424) — the stripe-cache
  copy of incoming bio data. `page_offset` is computed **once** from
  `bio->bi_iter.bi_sector` before the loop, then advanced by
  `page_offset += len` — the segment's actual byte length. The
  `skip_copy` page-steal path requires
  `b_offset == 0 && page_offset == 0 && clen == RAID5_STRIPE_SIZE`, so
  only page-aligned full-stripe bvecs can take it.
- **raid1 `process_checks()`** (raid1.c:2342) — walks md's **own**
  resync bios (full r1 sync pages) for memcmp; incoming geometry never
  reaches it.

Everything else moves data through `bio_copy_data()` — the block core's
dual-iterator, byte-accurate helper (raid1 write-behind copying the
incoming bio into its own full pages at raid1.c:1276; raid1/raid10 sync
paths copying md's own pages) — or is whole-bio remap + sector-granular
`bio_split()` at chunk boundaries (raid0, raid1, raid10, md-linear),
which is iterator-correct for the same reason DM core's splitting is.
md.c itself and md-bitmap.c contain zero bvec references.

## zram — EXPOSED, root-caused & fixed (2026-09-05)

Two independent defects, and the fix needed both ("zram: handle
sub-page bvec segments without corrupting data").

**Defect 1 — the dominant corruption on 4K-page kernels, and a
*variant* of the class, not the brd arithmetic.** On
`PAGE_SIZE == 4096`, `is_partial_io()` is hardwired `return false` on
the reasoning that `logical_block_size == PAGE_SIZE` guarantees
whole-page bvecs — the same advisory-limit fallacy as everywhere else
in this audit: queue limits constrain a bio's starting sector and
total size, never per-bvec lengths. Every sub-page segment then takes
the full-page fast path, `zram_write_page(zram, bvec->bv_page, index)`,
which consumes the **entire page and ignores `bv_offset`/`bv_len`** —
consecutive segments mapping to the same page index each rewrite the
whole slot. Observed signature: the poison write completes with
status 0 and reads back with **every byte wrong**, `device[0] ==
stream[bv0.len]` (slot 0 wholesale replaced by bvec1's page) — which is
how the empirical run unmasked this defect after the cursor-only fix
changed nothing. The read side clobbers reader memory outside the bvec
the same way.

**Defect 2 — the brd sector-cursor skew.** The loops re-derive
`index`/`offset` from `iter.bi_sector`, advanced by
`bio_advance_iter_single()` at `bv_len >> 9` — with honest partial
detection this still misplaces every segment after a sub-sector-length
one through the RMW path.

`zram_submit_bio()` validates nothing on entry (no
`bio_split_to_limits()`); `dma_alignment` stays at the 511 default, so
`dio_mem_align = 512` keeps NFSD's gated DIO out, but any direct
in-kernel bio producer corrupted silently.

**Fixed on main**: `is_partial_io()` made honest on every page size
(the partial helpers already exist and honor `bv_offset`/`bv_len`),
and the loops track a byte `pos` advanced by each segment's actual
consumption, brd-style. **Empirical after the fix**: poison `off0=684`,
legacy `off0=160`, 512-aligned mid-page `off0=512`, and aligned
`off0=0` all MATCH byte-identically (before: the unaligned rows
corrupted all 1 MiB silently).
