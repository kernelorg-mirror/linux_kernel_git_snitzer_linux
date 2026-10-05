# Cutting a misaligned write into direct and buffered parts

How nfsd and NFS LOCALIO write a WRITE that is not aligned to the storage,
what was wrong with it on anything but 4 KiB pages over 4096-byte blocks,
and what was changed on the 16 KiB test host on 2026-10-04. The code is
`nfs_dio_split()` in `fs/nfs_common/nfs_dio.c`, shared by both.

This belongs with `NFS_LOCALIO_DONTCACHE/`, which lives on another system;
it is kept here until it can be moved. The session that did the work had
not read that directory or the history of the split. What this says the
split was *meant* to do is Mike's statement and the code's comments, not
something checked against the original design record.

## 1. TODO

- [ ] **Revisit the byte-counted split gate.** `NFSD: count the
      misaligned-WRITE split gate in bytes, not pages` replaced
      `direct_misaligned_num_pages` (default 2) with
      `direct_misaligned_min_bytes` (default 8192). Mike, 2026-10-04: the
      split changes are generally right, **the byte gate less so**; it is to
      be confronted again, not treated as settled. What is known:
      - The old gate was a minimum middle counted in pages: 8, 32 and
        128 KiB at 4, 16 and 64 KiB pages. That scaling was not chosen on
        purpose that the session could find.
      - With the page cut (section 3) a misaligned write's middle is whole
        pages, so its smallest non-empty middle is one page. On 16 KiB the
        byte gate lets a one-page middle split where the page gate needed
        two; on 64 KiB one page (65536) is already above 8192, so the gate
        never decides anything for a misaligned write there. On 4 KiB the
        default is what it was.
      - It does nothing for misaligned writes smaller than a page on
        64 KiB (section 5).
      - Measured only on 16 KiB, LOCALIO, one rig (section 4). The choice
        of 8192 was not derived from a measurement of where three I/Os
        stop being worth it.
      - It renames a debugfs file and a policy field
        (`min_middle_pages` -> `min_middle_bytes`). The rename was made so a
        value written to the old name is not silently read as bytes.
      - Questions to settle: whether the gate should exist at all once the
        middle is whole pages; whether its unit should be bytes, pages or
        DIO-alignment units; and whether its default should differ by page
        size.
- [ ] **Upstream order.** The page-cut commit carries
      `[snitzer: FIXME must first fix NFSD DIRECT code and then carry this
      change to the shared nfs_common code]`. The oversight (section 2) is
      in the NFSD DIRECT write split that is upstream; it needs a
      `stable@vger.kernel.org` fix there first, and only then the port to
      the shared `nfs_common` implementation. Mike will split the commit
      with a Claude session.
- [ ] **Sub-page misaligned records on 64 KiB** (section 5): undecided
      whether they should stay fully buffered or keep a direct middle at
      the cost of reading the shared pages. Wants an iorhard run on
      tardis1.
- [ ] **More than two writers to a page.** The boundary claim
      (`nfs_dio_boundary_claim()`) is a two-writer protocol. A record
      smaller than a page leaves pages with three or more writers and they
      are read back (0.55 reads per WRITE for 6000-byte records on 16 KiB
      with DONTCACHE ends). Not addressed.
- [ ] **Coverage of the fix so far.** LOCALIO at 16 KiB only (section 4).
      The NFSD (non-LOCALIO) stages ran once on the final build, in boot 1
      of the test plan that followed (rig-cmp, loan-assert, killswitch and
      system-correctness PASS, data compared), but how NFSD's own WRITEs
      are now split has not been looked at. Nothing has run at 4 KiB or
      64 KiB. The 4 KiB case on 512-byte blocks changes behaviour
      (section 3) and has not been run.
- [ ] **Fold into the baseline.** The two code commits lead the series,
      and their two KUnit commits open its first KUnit section, so that
      Mike can fold them into the baseline commits they correct. Until
      then the KUnit suites do not build between the byte-gate commit and
      its test: the code renames a policy field the existing split test
      sets, and the test comes with the other KUnit commits. Folding
      removes that, since the code and the test each go into the commit
      that introduced them.
- [ ] **Sparse has not run** on any of these commits (the 16 KiB host's
      sparse 0.6.4 is refused by the kernel Makefile). W=1 is quiet.
- [ ] **A runtime test of a DIO-aligned, not page-aligned write staying
      direct.** Covered by KUnit only
      (`dontcache-aligned-ends-mid-page-all-direct`).

## 2. What the split is for, and the oversight

A write that is not aligned to the file system's direct I/O alignment is
split in three: a **direct middle** and a **buffered start and end**. The
point is to keep as much of the write as possible out of the page cache.

That only works if **direct and buffered I/O never meet inside a page**
(Mike, 2026-10-04: "The DIO and the buffered IO was always intended to be
isolated on PAGE_SIZE boundaries no matter the PAGE_SIZE and dio alignment
of the underlying device"). The direct middle invalidates the page cache
over its range. If the middle starts or stops inside a page that a buffered
end also writes, that page is dropped, and whichever of the two
neighbouring writes comes second has to read it back from the device. The
write then costs a read per shared page on top of its three I/Os, which
negates the split.

The split cut the middle at the **DIO offset alignment**:

```c
prefix_end = round_up(pos, offset_align);
middle_end = round_down(orig_end, offset_align);
```

On 4 KiB pages over 4096-byte blocks the alignment *is* the page size, the
cut is a page boundary, and the problem cannot arise. That is the only
configuration the split had been run on. The isolation it depends on was a
property of that configuration, not of the code.

**It is broken wherever the page is larger than the DIO alignment:**

| page size | DIO alignment | cut on a page boundary? |
|---|---|---|
| 4 KiB | 4096 | yes (the tested case) |
| 4 KiB | 512 | **no** |
| 16 KiB | 4096 or 512 | **no** |
| 64 KiB | 4096 or 512 | **no** |
| any | a multiple of the page size | yes |

So it affects every arm64 kernel with 16 or 64 KiB pages on ordinary
4096-byte-block storage, and 4 KiB kernels on 512-byte-block devices.

One more thing the code tied to the same condition: a buffered DONTCACHE
end holds the page it shares with the neighbouring write
(`nfs_dio_seg_hold()`) only if its cut is on a page boundary, and the code
tested that as `IS_ALIGNED(offset_align, PAGE_SIZE)`. Where the alignment
is finer than the page the claim was simply off.

## 3. The fix: cut each end on its own

`nfs_common: split a write's direct middle at page boundaries`. For
writes, regardless of DONTCACHE:

- **An end that is already a DIO-alignment multiple** needs no buffered
  segment. The middle starts or stops right there, mid-page or not.
- **A misaligned end** needs a buffered segment whatever is done. That
  segment now runs to the nearest page boundary **inside** the write, not
  to the nearest DIO-alignment multiple.

The three segments always tile exactly the bytes of the write. A buffered
end grows *inward*, at the middle's expense; nothing is written before
`pos` or after `pos + len`. If the two cuts meet or cross (a misaligned
write holding no whole page) there is no middle and the write is one
buffered segment. Reads are split as before: a direct read invalidates
nothing. Where the alignment is a page-size multiple nothing changes.

Worked examples, 16 KiB pages, 4096-byte alignment:

| write | original code | per-end cut |
|---|---|---|
| offset 188032, length 47008 (both ends misaligned) | prefix 384, middle 188416+45056, suffix 1568 | prefix 8576, **middle 196608+32768**, suffix 5664 |
| offset 4096, length 8192 (both ends aligned, mid-page) | all direct | all direct (unchanged) |
| offset 4096, length 40000 (aligned start, misaligned end) | middle 4096+36864, suffix 3136 | **no prefix**, middle 4096+28672, suffix 11328 |

Why per end and not "always cut at pages": a buffered segment exists at an
end only when that end is misaligned, so that is the only place a page is
shared between a buffered write and the direct middle of the same write or
of its back-to-back neighbour (one record's end is misaligned exactly when
the next record's start is). A write that is DIO-aligned stays entirely
direct at every page size.

**What per-end does not guarantee** (Mike chose it knowing this,
2026-10-04): two *unrelated* writes can still put direct and buffered data
in the same page. On 16 KiB pages, a write of 4096-8192 (aligned, direct)
and one of 100-4096 (misaligned, buffered) both land in page 0. Data stays
correct; the buffered one may read the page back. The strict rule that
would close this, cut at `max(DIO alignment, PAGE_SIZE)` always, makes a
4 KiB- or 8 KiB-aligned O_DIRECT write on a 64 KiB kernel fully buffered,
and was not taken for that reason.

Two simpler rules are wrong, and are worth recording so they are not tried
again:

- **Cutting at page boundaries only when the buffered ends are DONTCACHE.**
  The page a buffered end shares is dropped by the direct middle whatever
  kind of buffered write it was: with plain buffered ends and the old cut
  the test below read 0.522 per WRITE.
- **Cutting both ends at page boundaries regardless of alignment.** That
  turns a write that is aligned to the storage but not to a page into a
  buffered one: on 16 KiB pages a 4096-byte write at offset 4096 holds no
  whole page.

## 4. Measurements (16 KiB host)

Kernel `7.1.13-19.hs.451.el9.aarch64+` on the 16 KiB host, before and
after the change. Workload: `localio-tests/localio-boundary-test.sh`
(iorhard, 8 writers, 15 s, O_DIRECT through LOCALIO), XFS on a file-backed
nvmet namespace over NVMe/TCP, which reports `dio_offset_align=4096`.
Figures are READ bios on the export device per WRITE.

| record | original code | fixed, DONTCACHE ends | fixed, plain buffered ends |
|---|---|---|---|
| 47008 | 0.988 (DONTCACHE); 0.522 (plain, with the byte gate) | **0.003** (138 READ bios for 45680 WRITEs), 139 DONTCACHE kick rounds | **0.000** (16) |
| 24000 | not run | 0.010 | 0.001 |
| 6000 (smaller than a page) | 0.553 | 0.565 | 0.002 |

Controls that established the cause before any code changed:

- Same original kernel and test on a **16 KiB-logical-block** device (loop,
  `losetup -b 16384`, XFS with 16 KiB blocks, `dio_offset_align=16384`):
  **0.001** reads per WRITE at 47008. The cost follows the alignment.
- `mkfs.xfs -b size=16384 -s size=16384` on the 4096-byte-block device:
  1.342, unchanged in kind. XFS takes its DIO alignment from the device's
  logical block size, not from the mkfs sector size.

Other observations:

- With the 2-page gate only the 47008-byte records with a 32 KiB middle
  were split; with the byte gate every one is (`middle=32768` or 16384 in
  `nfs:nfs_local_dio_write`), and DONTCACHE kick rounds fell from 2333 to
  about 150.
- Plain buffered ends leave the file partly resident (8.6% at 47008) where
  DONTCACHE ends leave 0.0%, as intended.
- KUnit after the change: xdr 7/7, svcsock 40/40, nfsd receive 6/6 (32
  split cases), nfsd4 receive 10/10.

## 5. 64 KiB, from the code, not run

- A **DIO-aligned** write stays fully direct, whatever its size.
- A **misaligned 47008-byte record** (the iorhard size) is smaller than a
  page, so it never holds a whole one: no middle, one buffered DONTCACHE
  write, whatever the gate. The original code did the same there, because
  its 40-44 KiB middle was under its 128 KiB gate.
- Such a page can have **three writers** (tail of one record, a whole
  second, head of a third) when the first record boundary falls in the
  page's first 18528 bytes, about 28% of pages. The two-writer claim does
  not cover that, so some read-back is expected. This is an estimate.
- A misaligned write gets a direct middle only if it holds a whole 64 KiB
  page: never under 64 KiB, by luck up to about 128 KiB, always above.

Options for sub-page misaligned writes on 64 KiB, none started:

1. Leave them buffered (as now, and as the original code did).
2. Where the page cut leaves no middle, fall back to the DIO-alignment cut
   with no claim: the direct middle comes back, each end reads its shared
   64 KiB page.
3. Rework the claim so a page partly written by direct I/O can still be
   kept for the neighbour: direct I/O and no reads; the large change.

Whether 2 beats 1 for 47008-byte records is unknown and needs measuring on
a 64 KiB host.

## 6. Commits and tests

On `kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY`, by subject:

| | subject |
|---|---|
| code | nfs_common: split a write's direct middle at page boundaries |
| code | NFSD: count the misaligned-WRITE split gate in bytes, not pages |
| KUnit | NFSD: give the KUnit split test the byte-counted gate |
| KUnit | NFSD: test the per-end page cut of a split write in the KUnit suite |

`Documentation/filesystems/nfs/nfsd-io-modes.rst` is updated in the two
code commits (where the misaligned segments end; the gate's name and unit).

**KUnit (`nfsd_bvec_dio_segments_test`).** Five existing cases asserted the
old 512-byte cut and were reworked in `PAGE_SIZE` terms, each keeping what
it tests: `buffered-prefix-direct-middle-buffered-suffix` (now needs a
fourth vector; the table takes four) and the four `skipped-first-*` cases
(the middle now starts `PAGE_SIZE - 128` bytes into `bvec[0]`). Seven
added: a misaligned start and a misaligned end, each with and without
DONTCACHE; both ends aligned and mid-page (stays one direct segment); an
aligned mid-page start with a misaligned end (no prefix); a misaligned
write holding no whole page (one buffered segment that is exactly the
write). The claim expectation is now simply "a buffered DONTCACHE segment
claims". These reworked cases deserve a second look when the fix is split
for upstream.

**The boundary test is page-size aware** (Mike agreed, 2026-10-04). Its
6000-byte record spans two pages on 4 KiB and is smaller than a page on 16
and 64 KiB, so the stage failed on every larger-page run for a reason
unrelated to what it checks:

- the small record is scaled, `6000 * PAGE_SIZE / 4096`;
- a record smaller than a page has its reads reported, not judged, and a
  raw 6000-byte row still runs on larger pages so the gap stays visible;
- kick rounds are judged without requiring a page-multiple block size;
- the two read ceilings keep their values, but which applies follows
  whether the record **can split under the gate**, not its literal size:
  a 24000-byte record on 16 KiB can split, and against the never-split
  ceiling (497) it read 19, 365 and 610 READ bios in three runs.

On 4 KiB the defaults and their ceilings are as before; a sub-page record
given with `-r` there is now reported only.

## 7. How the problem was found

The 16 KiB host's first pass of `transplant/run-testplan.sh` completed
every LOCALIO stage, but `localio-boundary-test.sh` inside it exited 1:
about one READ per WRITE at 47008 bytes against 0.004-0.007 on 4 KiB.
`nfs_dio_split()` held shared pages only when the alignment was a page
multiple; `statx` on the rig showed `dio_offset_align=4096`; the same test
on a device with a 16 KiB logical block showed 0.001.
