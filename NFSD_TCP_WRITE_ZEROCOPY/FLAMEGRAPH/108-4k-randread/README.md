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
