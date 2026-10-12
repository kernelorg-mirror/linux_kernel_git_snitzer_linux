#!/bin/bash
# drive-cache.sh [through|back]: how Linux sees the write cache of the
# data drives and of the two stripes over them.
#
# "through" (the default, and how the rig runs from 2026-10-12, Mike)
# tells the block layer the drives have no volatile write cache, so it
# sends them no cache flush and no FUA, as with enterprise storage whose
# cache is protected.  The drives' own caches stay enabled; only the
# flushes go.  A COMMIT still syncs the file and forces the XFS log.
# "back" is the drives' real setting.  A udev rule sets "through" at boot.
#
# Either way the data drives' autonomous power states are turned off, so
# an idle drive does not go into a non-operational state between runs
# (the same udev rule file does it at boot).
case ${1:-through} in through) V="write through" ;; back) V="write back" ;; *) echo "usage: $0 [through|back]"; exit 1 ;; esac
for d in nvme{0..7}n1 md0 md1; do
	echo "$V" > /sys/block/$d/queue/write_cache || exit 1
done
for c in nvme{0..7}; do echo 0 > /sys/class/nvme/$c/power/pm_qos_latency_tolerance_us; done
for d in nvme{0..7}n1 md0 md1; do echo "$d: $(cat /sys/block/$d/queue/write_cache)$(case $d in nvme*) echo ", drive cache $(nvme get-feature /dev/${d%n1} -f 6 2>/dev/null | awk '{print $NF}'), autonomous power states $(nvme get-feature /dev/${d%n1} -f 0x0c 2>/dev/null | awk 'NR == 1 {print $NF}')";; esac)"; done
