#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# bvecrepro matrix on the patched brd, 4K host.
# Rows: poison off0=684 and legacy off0=160 on /dev/ram0 directly, aligned off0=0,
# plus poison via nvme-loop if the rig is up. Expect MATCH everywhere on patched brd.
set -e
# default to the tree this script lives in, so the runner is not tied to one
# checkout path (was hardcoded /root/kernel/linux)
K=${K:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}
B=${B:-/tmp/bvecrepro-build}
mkdir -p $B
cp $K/NFSD_TCP_WRITE_ZEROCOPY/bvecrepro.c $B/
echo 'obj-m := bvecrepro.o' > $B/Makefile
make -C $K M=$B modules -j8 >/dev/null

modprobe brd rd_nr=1 rd_size=4194304 2>/dev/null || true

run_row() {
    local d=$1 o=$2
    dmesg -C
    insmod $B/bvecrepro.ko dev=$d off0=$o 2>/dev/null || true  # always exits -EAGAIN
    echo "--- dev=$d off0=$o:"
    dmesg | grep bvecrepro
}

run_row /dev/ram0 684
run_row /dev/ram0 160
run_row /dev/ram0 0
# nvme-loop row only if the rig device exists and is NOT mounted.
# Find it via sysfs, not `nvme list`: nvme-cli >= 2.x prints Model "Linux" for
# an nvmet loop namespace, with no "loop" anywhere in the row, so the old
# `awk '/loop/'` match silently skipped this row on every modern nvme-cli.
find_loop_ns() {
    local c
    for c in /sys/class/nvme/nvme*; do
        [ "$(cat $c/transport 2>/dev/null)" = loop ] || continue
        echo /dev/$(basename $c)n1
        return 0
    done
    return 1
}
NVME=$(find_loop_ns) || true
if [ -b "${NVME:-}" ] && ! mount | grep -q "$NVME"; then
    run_row "$NVME" 684
else
    echo "(nvme-loop row skipped: no unmounted loop namespace — run before mkfs/mount or after teardown)"
fi
