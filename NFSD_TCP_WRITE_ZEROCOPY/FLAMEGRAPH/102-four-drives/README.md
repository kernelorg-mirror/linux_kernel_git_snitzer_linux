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

## Result

[run622.out](run622.out).  Every run clean: no ring drop, no drive
stepped down, no new kernel warning; the host at the settled setting
after.

| | one port (a, nvme0 to nvme3) | two ports (two drives a port) |
|---|---|---|
| **write** | **40.60, 40.68 GB/s** (`t2-r622`, `r626`) | **40.06, 40.10 GB/s** (`r623`, `r627`) |
| a drive writes | 10.2 GB/s, 100% busy | 10.0 GB/s, 99 to 100% busy |
| write latency, mean / p99 | 52 / 82 ms | 106 / 148 to 150 ms |
| **read** | **43.16, 42.88 GB/s** (`r624`, `r628`) | **52.43, 52.44 GB/s** (`r625`, `r629`) |
| a drive reads | 10.7 to 10.8 GB/s, 98 to 99% busy | 13.1 GB/s, 100% busy |
| read latency, mean / p99 | 49 / 74 to 78 ms | 81.5 / 83 to 116 ms |
| server CPUs busy, write (rings a / b / nfsd) | 15 to 16% / - / 7% | 6% / 6% / 10% |
| server CPUs busy, read (rings a / b / nfsd) | 34 to 36% / - / 8% | 12 to 13% / 11 to 13% / 12% |
| client CPUs busy | 23% write, 47% read | |

- **Writes are the drives' either way**: four drives at their cap,
  10.2 GB/s each on one port (local fio's 10.21 to 10.22, round 98),
  10.0 on two.  One port is 1.4% the faster, as round 98 found one
  port holds its drives deeper than two do.  Two ports put 32 writers
  on the drives against 16, so a WRITE waits twice as long for the
  same rate.
- **Reads on one port are the port's, not the drives'**: the same four
  drives read 13.1 GB/s each when two ports carry them (52.4 GB/s,
  100% busy), and 10.7 when one port does (43 GB/s).  So 52.4 GB/s is
  what the four drives read through NFS, and about 43 GB/s what one
  port sends of reads, with its rings' CPUs at 35%.  Where in the port
  the 43 is set was not looked for in these runs.
- The loans play no part in a read; the reads are direct on both
  sides, so every byte came off the drives.
