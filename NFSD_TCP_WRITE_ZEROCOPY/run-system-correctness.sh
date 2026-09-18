#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# System-level functional & correctness suite — KUnit-decoupled.
#
# Exercises the production receive/decode/write stack end-to-end over real
# NFS traffic and verifies CORRECTNESS by byte-comparison against a local
# model file that receives the identical operation sequence.  Derived from
# the KUnit suites' coverage areas (see TESTING.md "System-level coverage")
# and from the geometries that triggered the 2026-09-05 corruption findings.
#
# Rig: brd -> nvme-loop -> XFS -> NFS (LOCALIO off).  Every case runs for
# vers in {3, 4.2} x io_cache_write in {0, 2}; a mismatch anywhere is a
# hard failure.  Mode 0 vs 2 also cross-checks the direct path against the
# buffered path on identical inputs.
set -u
CONNS=${CONNS:-10}
TRACE=/sys/kernel/tracing
CFG=/sys/kernel/config/nvmet
SRC=/dev/shm/sysc-src-64m
MODELDIR=/dev/shm/sysc-model
FAILS=0

say() { echo "== $*"; }
fail() { echo "FAIL: $*"; FAILS=$((FAILS+1)); }

# --- rig up (idempotent, mirrors run-rig-cmp.sh) ---
modprobe brd rd_nr=1 rd_size=4194304
modprobe nvmet nvme_loop
mkdir -p $CFG/subsystems/zc/namespaces/1 $CFG/ports/1
if [ "$(cat $CFG/subsystems/zc/namespaces/1/enable)" != 1 ]; then
    echo 1 > $CFG/subsystems/zc/attr_allow_any_host
    echo -n /dev/ram0 > $CFG/subsystems/zc/namespaces/1/device_path
    echo 1 > $CFG/subsystems/zc/namespaces/1/enable
    echo -n loop > $CFG/ports/1/addr_trtype
fi
ln -sf $CFG/subsystems/zc $CFG/ports/1/subsystems/zc 2>/dev/null || true
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
if [ ! -b "${NVME:-}" ]; then
    nvme connect -t loop -n zc
    sleep 1
    NVME=$(find_loop_ns)
fi
say "nvme-loop device: $NVME"
mountpoint -q /export/zc || mkfs.xfs -f -q $NVME
mkdir -p /export/zc /mnt/zc $MODELDIR
mountpoint -q /export/zc || mount $NVME /export/zc
systemctl is-active -q nfs-server || systemctl start nfs-server
exportfs -o rw,no_root_squash,insecure,fsid=77 '*:/export/zc'
modprobe nfs
echo N > /sys/module/nfs/parameters/localio_enabled
head -c 67108864 /dev/urandom > $SRC

echo 0 > $TRACE/tracing_on; echo > $TRACE/trace
for e in nfsd/nfsd_write_direct nfsd/nfsd_write_vector nfs/nfs_local_open_fh; do
    echo 1 > $TRACE/events/$e/enable 2>/dev/null || true
done
echo 1 > $TRACE/tracing_on

# op FILE <dd-args...>: apply identical dd to the NFS file and the model.
op() {
    local f=$1; shift
    dd if=$SRC of=/mnt/zc/$f "$@" conv=notrunc,fsync status=none || return 1
    dd if=$SRC of=$MODELDIR/$f "$@" conv=notrunc status=none
}

check() {  # check FILE LABEL — model vs NFS read-back, then vs export
    local f=$1 label=$2
    cmp -s $MODELDIR/$f /mnt/zc/$f || fail "$label: NFS read-back mismatch ($f)"
}
check_export() {
    local f=$1 label=$2
    cmp -s $MODELDIR/$f /export/zc/$f || fail "$label: on-disk mismatch ($f)"
}

run_matrix() {
    local vers=$1 mode=$2 tag="v$1/mode$2"
    echo $mode > /sys/kernel/debug/nfsd/io_cache_write
    rm -rf /export/zc/* $MODELDIR/*; sync
    mount -t nfs -o vers=$vers,proto=tcp,sec=sys 127.0.0.1:/export/zc /mnt/zc || {
        fail "$tag: mount"; return; }

    # 1. large page-aligned O_DIRECT stream (the loan/DIO fast path)
    op d16 bs=1M count=16 oflag=direct                 || fail "$tag: d16 write"
    # 2. O_DIRECT with a trailing odd block (per-segment split decisions)
    op dodd bs=1M count=8 oflag=direct                 || fail "$tag: dodd write"
    op dodd bs=4096 count=3 seek=2048 oflag=direct     || fail "$tag: dodd tail"
    # 3. buffered odd record sizes (sub-sector lengths through the receive)
    op bo1 bs=3000 count=7                             || fail "$tag: bo1"
    op bo2 bs=999 count=1000                           || fail "$tag: bo2"
    # 4. sub-block overwrite at an odd offset (RMW correctness)
    op rmw bs=16384 count=1                            || fail "$tag: rmw base"
    op rmw bs=100 count=1 oflag=seek_bytes seek=4000   || fail "$tag: rmw poke"
    # 5. append-style growth in odd increments
    local off=0 len
    for i in 1 2 3 4 5; do
        len=$((4096 * i + 137))
        op app bs=$len count=1 oflag=seek_bytes seek=$off || fail "$tag: app$i"
        off=$((off + len))
    done
    # 6. large buffered stream
    op big bs=128K count=512                           || fail "$tag: big"
    # 7. small-file churn (decode/dispatch across many RPCs)
    for i in $(seq 1 100); do
        op s$i bs=$((512 * (i % 16 + 1) + i)) count=1  || fail "$tag: s$i"
    done

    for f in d16 dodd bo1 bo2 rmw app big; do check $f "$tag"; done
    for i in $(seq 1 100); do check s$i "$tag"; done
    umount /mnt/zc
    for f in d16 dodd bo1 bo2 rmw app big; do check_export $f "$tag"; done
    say "$tag: matrix complete"
}

conn_anchor_loop() {  # fresh-connection anchor phases (the EINVAL/corruption trigger)
    local vers=$1 tag="v$1/anchors"
    echo 2 > /sys/kernel/debug/nfsd/io_cache_write
    for i in $(seq 1 $CONNS); do
        mount -t nfs -o vers=$vers,proto=tcp,sec=sys 127.0.0.1:/export/zc /mnt/zc
        dd if=$SRC of=/mnt/zc/anch$i bs=1M count=16 oflag=direct conv=fsync \
           status=none || fail "$tag: conn $i write"
        umount /mnt/zc
        cmp -s -n 16777216 $SRC /export/zc/anch$i || fail "$tag: conn $i mismatch"
    done
    rm -f /export/zc/anch*
    say "$tag: $CONNS fresh connections complete"
}

for vers in 3 4.2; do
    for mode in 0 2; do
        run_matrix $vers $mode
    done
    conn_anchor_loop $vers
done

echo 0 > $TRACE/tracing_on
D=$(grep -c nfsd_write_direct $TRACE/trace || true)
V=$(grep -c nfsd_write_vector $TRACE/trace || true)
L=$(grep -c nfs_local_open_fh $TRACE/trace || true)
[ "$L" = 0 ] || fail "LOCALIO engaged ($L hits) — run measured nothing"
[ "$D" -gt 0 ] || fail "no nfsd_write_direct events in any mode-2 run"

echo Y > /sys/module/nfs/parameters/localio_enabled
say "RESULT: fails=$FAILS direct=$D vector=$V localio=$L"
[ $FAILS = 0 ] && say "ALL SYSTEM CORRECTNESS CASES PASS"
exit $FAILS
