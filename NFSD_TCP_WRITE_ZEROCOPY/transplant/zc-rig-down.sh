#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# zc-rig-down.sh - tear down the "zc" rig that run-rig-cmp.sh,
# run-loan-assert.sh, run-killswitch.sh and run-system-correctness.sh bring
# up and leave up (brd /dev/ram0 -> nvmet subsystem "zc" on loop port 1 ->
# /dev/nvmeXnY -> XFS /export/zc, exported to *, mounted on /mnt/zc), so a
# later rig (run-sgl-rigs.sh wants brd with rd_nr=3; run-bvecrepro.sh wants
# an unmounted loop namespace) starts clean.  Undoes only what those scripts
# create; restores io_cache_write to 0 and LOCALIO to on.  Safe to re-run.
set -u
CFG=/sys/kernel/config/nvmet

mountpoint -q /mnt/zc && { umount /mnt/zc || umount -f -l /mnt/zc; }
exportfs -u '*:/export/zc' 2>/dev/null
mountpoint -q /export/zc && umount /export/zc
nvme disconnect -n zc >/dev/null 2>&1
if [ -d $CFG/subsystems/zc ]; then
	rm -f $CFG/ports/1/subsystems/zc
	[ -e $CFG/subsystems/zc/namespaces/1/enable ] && echo 0 > $CFG/subsystems/zc/namespaces/1/enable
	rmdir $CFG/subsystems/zc/namespaces/1 $CFG/subsystems/zc 2>/dev/null
fi
# Port 1 is the zc rig's; leave it if anything else is still linked to it.
[ -d $CFG/ports/1 ] && [ -z "$(ls $CFG/ports/1/subsystems 2>/dev/null)" ] && rmdir $CFG/ports/1
[ -w /sys/kernel/debug/nfsd/io_cache_write ] && echo 0 > /sys/kernel/debug/nfsd/io_cache_write
[ -w /sys/module/nfs/parameters/localio_enabled ] && echo Y > /sys/module/nfs/parameters/localio_enabled
if lsmod | grep -q '^brd '; then
	modprobe -r brd 2>/dev/null || echo "zc-rig-down: brd still in use (another rig on /dev/ram*?)" >&2
fi
echo "zc rig down: /mnt/zc $(mountpoint -q /mnt/zc && echo mounted || echo free)," \
     "/export/zc $(mountpoint -q /export/zc && echo mounted || echo free)," \
     "subsystem zc $([ -d $CFG/subsystems/zc ] && echo present || echo gone)," \
     "brd $(lsmod | grep -q '^brd ' && echo loaded || echo unloaded)"
