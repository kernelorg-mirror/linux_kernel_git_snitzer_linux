# FLAMEGRAPH round 108: 4 KiB random READ IOPS

Mike, 17 November: "we need to do some 4K random READ IOPS runs,
starting with how fast all 8 drives can do it locally, using the
io_uring method is perfectly fine".  Rounds 104 to 107 measured
bandwidth; this round measures operations a second, from the drives
alone first, then over NFS.

## The drives alone

tardis2's eight data drives are DapuStor DPRP51016TTL15T3000 (PCIe 5,
15.36 TB, firmware FE003208), each with its own XFS; the server is 64
Neoverse-N2 cores on one node.  The NVMe driver has no poll queues
(`poll_queues` 0; the boot drive is NVMe too, so the driver is not
reloaded for them), so completions are interrupts.

`local.sh <label> <jobs a drive>:<iodepth> ...`: fio on tardis2 itself,
each drive's kept `f-*` files (about 2 GB a drive, written whole by
the fill: the raw device would read trimmed blocks, which a drive
answers without its flash), 4 KiB random reads, O_DIRECT, io_uring with
registered buffers and files, ramp 3 s and 20 s counted, mpstat for 10
s in the middle.  About to run through `xeu-rig/step.sh
/var/tmp/zc-run-logs/r108 local`: `local.sh sweep 1:32 2:64 4:64 8:32`.

### The sweep (`/var/tmp/zc-run-logs/r108-sweep`)

| jobs a drive : iodepth | jobs | M IOPS | GB/s | a drive, M | completion mean / p99, us | CPUs busy (busiest) |
|---|---|---|---|---|---|---|
| 1 : 32 | 8 | 2.99 | 12.2 | 0.37 to 0.39 | 83 / 118 | 15% (100%) |
| 2 : 64 | 16 | 6.22 | 25.5 | 0.66 to 0.83 | 163 / 232 | 28% (100%) |
| 4 : 64 | 32 | 12.59 | 51.6 | 1.55 to 1.62 | 160 / 234 | 52% (100%) |
| **8 : 32** | **64** | **16.05** | **65.7** | 1.81 to 2.21 | 126 / 334 | 72% (88%) |

- Up to 32 jobs a fio thread is its core's: about 390 K IOPS a job,
  the rate doubling with the jobs.
- At 64, a job to a CPU, 27% more and not twice: no CPU full, but at
  72% busy with every core running a job the CPUs are close too, and
  the drives spread from 1.81 to 2.21 M.  16 M IOPS is a floor on the
  eight drives, not their ceiling: the server's CPUs and the drives run
  out about together.
- No NVMe error or kernel warning.  (A "FAT-fs (nvme8n1p1): Volume was
  not properly unmounted" in the kernel log about 10:56 is the boot
  drive's EFI partition, most likely marked dirty by the morning's
  crash; not these runs.)

## Next (Mike: "run the 8:64 local point, then the NFS 4K runs")

`local.sh deep 8:64`, then over NFS `nfs4k.sh <n>:<iodepth>[:<nconnect>]`:
twoport.sh on both ports at the settled setting, fio on the client 16
jobs a port reading the same kept files with 4 KiB random reads on
io_uring with registered buffers, WARM=5 RUNTIME=20, mpstat on both
ends in the load.  `twoport.sh`'s fill now always says `iodepth=16`
(its default, so the fill's checksum is unchanged and the kept files
hold at any IODEPTH).  About to run through `xeu-rig/step.sh
/var/tmp/zc-run-logs/r108`: `local.sh deep 8:64`, then `nfs4k.sh
872:16 873:32 874:64`, 512 to 2048 READs outstanding a port.

### The 8:64 point and 4 KiB over NFS

| run | outstanding | M IOPS | completion mean / p99 | server busy (busiest) | client busy (busiest) |
|---|---|---|---|---|---|
| local 8 : 32 | 256 a drive | 16.05 | 126 / 334 us | 72% (88%) | |
| **local 8 : 64** | 512 a drive | **18.78** (2.29 to 2.39 a drive) | 215 / 659 us | 93% (99%) | |
| t2-r872, NFS, iodepth 16 | 256 a port | 0.98 (0.49 / 0.50) | 0.52 / 1.32 ms | 33% (100%) | 85% (89%) |
| t2-r873, NFS, iodepth 32 | 512 a port | 0.98 (0.49 / 0.49) | 1.05 / 2.41 ms | 34% (93%) | 85% (86%) |
| t2-r874, NFS, iodepth 64 | 1024 a port | 0.95 (0.47 / 0.48) | 2.17 / 4.69 ms | 30% (90%) | 89% (92%) |

- **Locally the server's 64 cores are the limit at 18.78 M IOPS**,
  93% busy, the drives even at 2.3 M each.
- **Over NFS about 1 M IOPS, 5% of that, at any depth**: twice the
  queue is twice the latency and no more IOPS, a fixed service rate.
  The client is 85 to 89% busy, evenly; the server about a third,
  though one of its CPUs is near full (32 connections, each one's READ
  work on one CPU).  Every run kept its files, new kernel warnings 0.
- Next: where a 4 KiB READ's time goes, a profile of both ends at
  iodepth 16.
