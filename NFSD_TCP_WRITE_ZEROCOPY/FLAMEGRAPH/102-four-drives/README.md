# Round 102: NFS writes and reads to four drives, one port and two

14 November, tardis2, build 531 (`7.3.3-16.hs.531.canary`), the DRC
commits' `nfsd.ko` in `updates/` (round 100).  Asked for by Mike before
round 101's `psi=0` boot: write and read results to only four of the
NVMe drives, on one port (all four on it) and on two (two a port).

## The runs

The same four drives in both, nvme0 to nvme3, so the drives are not
what differs:

| | ports | drives | setting |
|---|---|---|---|
| one port | a (`enp161s0f0`) | nvme0, nvme1, nvme2, nvme3 | the one-port setting (rings of 256, 750 KiB held buffer, 16 connections, 1 MiB rsize and wsize), as round 98's `t2-r525` |
| two ports | a, b (`enp164s0f0`) | a: nvme0, nvme1; b: nvme2, nvme3 | the two-port setting (rings of 128, 512 KiB held buffer, 12 connections a port, 512 KiB wsize, 1 MiB rsize, window 10) |

- `twoport.sh` with `FS_A` and `FS_B` naming the drives, 16 writers
  (or readers) a port of 8 MiB direct I/O at iodepth 16, so two ports
  put 32 on the four drives where one port puts 16.
- Loans as settled: `WRITES=64`, and `BUDGET_PAGES=8198` on two ports
  (round 100's).
- Writes: `WARM=10 RUNTIME=40`, inside each drive's fast region (at
  10 GB/s a drive about 500 GB, round 100's per-drive load).
- Reads: `RW=read` writes every file whole first and rests 20 s, then
  reads.  Direct on the client (`direct=1`) and on the server
  (`io_cache_read` 2), so every byte comes off the drives.
- Each run: `settled.sh apply`, both ports pinged, every drive
  trimmed, 120 s rest.  Order: one-port write, two-port write,
  one-port read, two-port read, twice (`t2-r622` to `t2-r629`):
  [run622.sh](run622.sh).
