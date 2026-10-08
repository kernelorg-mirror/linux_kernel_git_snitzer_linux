# Direct writes to a growing file on XFS

Why nfsd's (and LOCALIO's) direct writes to a file that is growing run at
a fraction of the speed of the same writes to a file that already has its
size, whether XFS can be changed, and what the write split can do about
it. Written on tardis1 (64 KiB pages, build 457 with rebuilt modules) on
2026-10-07. Section 5 says how far the proposal has been taken.

It belongs with `NFSD_LOCALIO_DIRECT_SPLIT.md`: once the split leaves an
aligned end of a write where it is, a copied WRITE that is aligned to the
device is direct throughout (`transplant/tardis1-build457-handoff.md`
section 9.7), and this is what that direct I/O then costs on a growing
file. `on-die-receive-copy/SPEC.md` section 3.4 first recorded the
mechanism, on the 16 KiB host on 2026-10-04.

## 1. What was measured

Budget phase A's 16-connection copy cells on nvme-loop (`STORAGE=prp`,
budget 0, every WRITE copied and issued as one direct segment, storage
delay 1 ms): `mixed-copy` writes four files that grow, `mixed-sized-copy`
the same four files given their size first. Traced at nfsd's call into
the file system (`/var/tmp/zc-run-logs/xfs-eof/eof.bt`: from
`vfs_iocb_iter_write()` to the `xfs_file_direct_write` tracepoint is the
time before the I/O starts, from there to return the I/O itself;
`xfs_zero_eof` counts the writes that zeroed beyond end-of-file).

| | growing (`mixed-copy`) | sized (`mixed-sized-copy`) |
|---|---|---|
| throughput | 442 MB/s | 2804 MB/s |
| direct writes | 2913 | 2913 |
| writes that zeroed beyond EOF | 550 (19%) | 4 |
| time before the I/O starts, all writes | 277.9 s (mean 95 ms) | 0.016 s (mean 5 us) |
| time in the I/O, all writes | 9.5 s (mean 3.3 ms) | 11.1 s (mean 3.8 ms) |

One run of each. The I/O is as fast on the growing file; a write there
spends about 97% of its time waiting to start.

The same cells under the page cut of `v7.1.13-20`, where these WRITEs
were issued as 1407 direct and 3938 buffered DONTCACHE segments, ran at
1956 to 2223 MB/s growing and 2391 to 2413 sized (build 457's plan run).

## 2. Why

In `fs/xfs/xfs_file.c`:

- **A write that starts beyond EOF must zero what lies between the old
  EOF and its start** (`xfs_file_write_checks()`). To do that on a file
  size that cannot move, `xfs_file_write_zero_eof()` gives up the shared
  `IOLOCK`, takes it exclusive, and calls `inode_dio_wait()`: no new I/O
  is submitted and every direct write in flight has completed, and so
  has updated the size, before it looks again and zeroes.
- **A direct write holds the `IOLOCK` shared across its I/O when its
  kiocb is synchronous** (`xfs_file_dio_write_aligned()` unlocks after
  `iomap_dio_rw()` returns). nfsd's are. An asynchronous one drops the
  lock early and is waited for by `inode_dio_wait()` instead; either
  way the beyond-EOF write waits for all of them.
- After its checks the beyond-EOF direct write demotes the lock to
  shared and does its own I/O.

So every beyond-EOF direct write waits for all I/O in flight, every
write that arrives meanwhile queues behind it, and the next beyond-EOF
write waits for the previous one's I/O. They run one at a time, storage
time included: 550 of them at 3.3 ms is 1.8 s of the 2.3 s the cell
takes.

They are common because the client keeps 32 WRITEs per file in flight
(fio, libaio, iodepth 32) and nfsd's threads issue them in whatever
order they are received and scheduled: about one in five starts beyond
the EOF of that moment.

## 3. Can XFS be changed?

Not by this project, as far as was found.

- **The drain is deliberate.** Upstream added it for this case; see
  "xfs: always drain dio before extending aio write submission"
  (`3136e8bb3054`) and "xfs: direct IO EOF zeroing needs to drain AIO"
  (`40c63fbc55a9`). The file size is updated only when a direct write
  completes, so without the drain the size the zeroing starts from is
  stale.
- **Skipping it needs proof that nothing in the gap needs zeroing.**
  From this tree: a direct write's new blocks are allocated unwritten
  (`xfs_iomap_write_direct()`, `XFS_BMAPI_PREALLOC`), converted when the
  I/O completes, and only then is the in-core size raised
  (`xfs_dio_write_end_io()`: "Unwritten conversion updates the in-core
  isize after extent conversion"). A zeroing pass run without the drain
  can meet a write in that window: blocks already written, still beyond
  the size it read, and it would zero live data. That is a reading of
  the code, not a test.
- So removing the wait means changing how XFS makes EOF stable. That is
  for the XFS maintainers; nothing was tried.

## 4. What the split can do

**Proposal:** in `nfs_dio_split()`, a write that starts beyond the
file's current size is not issued direct: it is one buffered segment,
DONTCACHE when the policy says so, like any write the split does not
make direct.

- A buffered write beyond EOF takes the same exclusive lock and the
  same drain, but holds the lock for a memory copy, not for its I/O.
  Beyond-EOF writes stop running one at a time on storage time.
- Writes at or below EOF stay direct: 81% of this cell's, and every
  overwrite and every append that arrives in order.
- The size is read without a lock and is a hint. Where it is wrong the
  write takes XFS's slow path as it does today, or is buffered when it
  need not have been.
- NFSD and LOCALIO both get it, since both use the split.
- The reason to expect a gain is section 1's last paragraph: a mix of
  direct and buffered segments ran four to five times faster. That mix
  was made by alignment, not by EOF, so it is an indication only.

Not taken:

- **Extending the file's size ahead of the write** (truncate up, or
  fallocate without KEEP_SIZE): it shows other clients a size no WRITE
  has reached, and takes the same exclusive lock.
- **Holding a beyond-EOF write until its predecessors arrive:** it
  parks nfsd threads on a guess that the gap will be filled.
- **Making nfsd's direct writes asynchronous:** the beyond-EOF write
  then waits in `inode_dio_wait()` instead of on the lock.

## 5. The prototype, measured (2026-10-07)

**Reverted later the same day** (`40f67123e8fa`, a revert of `7809bfb4c8e9`
at the tip of `kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY`): Mike took
the second of the three ways below. The rule is in history to come back
to and is not in the tree. `aeed1c15a2ac` stays. The rest of this
section is as written before the revert.

**Committed the same night, at the tip, pushed by Mike only to
`origin/kernel-7.1.13/main.NFSD_TCP_WRITE_ZEROCOPY-WIP2`, not regrouped,
not on the HS twin, and not decided:** `7809bfb4c8e9` "nfs_common: do not issue
a write that starts beyond EOF as direct I/O" and `aeed1c15a2ac` "NFSD:
give the KUnit split test's file a size". Mike asked for the code to be
saved in a commit; **whether the rule stays is open** (the end of this
section). Between the two commits the KUnit split test dereferences a
file with no inode.

`nfs_dio_split()` with the rule of section 4 as a new disposition,
`NFS_DIO_BEYOND_EOF` (`beyond_eof` in `nfsd_write_dio_split`), built as
modules against build 457 and loaded (`/var/tmp/zc-mod-457fix/eof/`;
`nfs_dio` srcversion `D6126B95144A51490CC12C3`). The same two cells, the
same trace, one run each, in `/var/tmp/zc-run-logs/xfs-eof/proto-*`:

| | growing, before | growing, prototype | sized, before | sized, prototype |
|---|---|---|---|---|
| throughput | 442 MB/s | **2355 MB/s** | 2804 MB/s | 2526 MB/s |
| WRITEs issued direct | 2913 | **668 (23%)** | 2913 | 2912 |
| WRITEs buffered as beyond EOF | 0 | **2245 (77%)** | 0 | 1 |
| time before the I/O starts, direct writes | 277.9 s | 2.0 s (mean 3 ms) | 0.016 s | 0.016 s |
| writes that zeroed beyond EOF | 550 | 396 | 4 | 4 |

- **The growing-file copy is fast again**, a little above what the page
  cut gave (1956 to 2223), and nothing failed (`fio_rc 0`, data
  verified by fio, nothing in the kernel log).
- **But only 23% of its WRITEs are now direct.** Before, 19% of writes
  zeroed beyond EOF; the rest had also started beyond it and waited
  long enough for EOF to pass them. With no such wait the size is read
  at once, and 77% are beyond it at that moment. The rule does what it
  says and gives up most of the direct I/O on a file that grows this
  way.
- The buffered ones are DONTCACHE (`wr_dontcache` 2245), so the page
  cache stays flat; what is given up is the direct path, not the
  footprint.
- The sized-file copy is unchanged: all direct, and its throughput is
  within what single runs of it have shown (2550 to 2811).

**What this settles and what it does not.** The cost of section 2 is
avoidable from the split, and the price is that a file growing under
out-of-order WRITEs is written mostly through the page cache. Whether
that meets the aim of copy mode issuing O_DIRECT more often than not is
Mike's to judge: it does for files that have their size and for in-order
appends, and it does not for this cell. Nothing between the two was
tried (for instance being direct when only a little beyond EOF: any
write beyond EOF takes the exclusive path, so the distance does not
obviously matter).

The KUnit split test's fake file was given an inode whose size no test
write exceeds, so the existing cases are unaffected:
`nfsd-receive-bvec` passes 6/6 on this build, run alone. There is no
case for the new disposition yet, and the other three suites were not
run on this build.

**Open, for Mike.** Three ways on from here, as put to him:

1. Keep the rule, add its KUnit case.
2. Drop it (revert `7809bfb4c8e9`; `aeed1c15a2ac` is harmless without
   it) and accept slow direct writes to a growing file.
3. Make it a policy knob beside `direct_misaligned_dontcache`, so the
   two can be compared per workload. Suggested default on: the
   difference is five-fold and the page cache footprint is unaffected,
   but it trades away direct I/O for files that grow out of order.

Sources for section 3:
<https://kunit.googlesource.com/linux/+/3136e8bb3054d3bb68942f8f1ee6c26c05f798b0>,
<https://kunit.googlesource.com/linux/+/40c63fbc55a968383b8bb5cacad81585e80cd323%5E%21/fs>,
<https://lkml.iu.edu/hypermail/linux/kernel/1704.0/03485.html>.
