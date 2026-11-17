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
