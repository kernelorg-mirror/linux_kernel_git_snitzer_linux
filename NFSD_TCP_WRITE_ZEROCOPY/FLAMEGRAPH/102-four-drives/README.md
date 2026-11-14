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

## The same on all eight drives

Mike, the same day: the same runs on all eight drives.  One port: a
alone on nvme0 to nvme7 at the one-port setting.  Two ports: a on
nvme0 to nvme3 and b on nvme4 to nvme7, twoport.sh's own layout at the
two-port setting (round 100's load, with reads too).  The same order, `t2-r630` to `t2-r637`:
`DRIVES=8 RUNS="630:a:write 631:ab:write 632:a:read 633:ab:read
634:a:write 635:ab:write 636:a:read 637:ab:read" ./run622.sh`.  At
10 GB/s a drive on two ports, 50 s of writing is round 100's load,
inside the fast region; one port gives a drive half that.

### Result on eight drives

[run630.out](run630.out).  No drive stepped down and no new kernel
warning in any run; the two-port reads dropped frames on the rings
(below).  The host at the settled setting after.

| | one port (a, nvme0 to nvme7) | two ports (four drives a port) |
|---|---|---|
| **write** | **49.31, 49.20 GB/s** (`t2-r630`, `r634`) | **78.17, 78.01 GB/s** (`r631`, `r635`) |
| a drive writes | 6.1 to 6.2 GB/s, 67% busy | 9.7 to 9.8 GB/s, 98% busy |
| write latency, mean / p99 | 43 / 76 to 79 ms | 54 / 80 to 82 ms |
| **read** | **43.15, 39.63 GB/s** (`r632`, `r636`) | **54.32, 54.55 GB/s** (`r633`, `r637`) |
| a drive reads | 5.4 and 4.9 to 5.0 GB/s, 86% and 79% busy | a 7.0 and 7.8, b 6.4 and 5.8 GB/s, 69 to 83% busy |
| the ports' split of a read | | 29.0 / 25.3 and 32.1 / 22.5 GB/s |
| ring drops | 0 | 0 writing; 26 and 167 reading, nearly all on b |
| server CPUs busy, write (rings a / b / nfsd) | 21 to 22% / - / 8% | 20 to 21% / 18 to 19% / 22% |
| server CPUs busy, read (rings a / b / nfsd) | 38% / - / 7 to 9% | 36% / 31 to 32% / 16% |
| client CPUs busy, write / read | 29% / 38 to 48% | 49 to 50% / 46 to 47% |

- **One port writes 49.2 to 49.3 GB/s** (the wire 49.6 to 49.7), the
  drives two thirds busy: the port's limit for writes, above the
  47.39 tardis1 carried on one port (AGENTS.md, 13 October) and the 48 the wire
  was said to carry.
- **Two ports write 78.0 to 78.2 GB/s**, round 100's rate: the
  drives' (98% busy).
- **One port reads 40 to 43 GB/s with eight drives as with four**:
  the port's limit for reads, the drives 79 to 86% busy.
- **Two ports read only 54.3 to 54.6 GB/s, barely more than four
  drives' 52.4**, and neither the drives (69 to 83% busy) nor either
  port (22 to 32 GB/s each, against one port's 43) is at its limit.
  The ports split it unevenly, b behind a by 4 and 10 GB/s, and b's
  rings dropped frames (26 and 167) in both runs, where no run of
  round 102 dropped one before.  What holds two ports' reads together
  near 54 GB/s, and why b falls behind, was not looked for in these
  runs; it is the open question this round leaves.

## What the round shows

| GB/s | four drives, one port | four drives, two ports | eight drives, one port | eight drives, two ports |
|---|---|---|---|---|
| write | 40.6 to 40.7 (drives) | 40.1 (drives) | 49.2 to 49.3 (port) | 78.0 to 78.2 (drives) |
| read | 42.9 to 43.2 (port) | 52.4 (drives) | 39.6 to 43.2 (port) | 54.3 to 54.6 (neither) |
