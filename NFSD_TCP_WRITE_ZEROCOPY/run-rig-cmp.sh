#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Runtime validation WITH data comparison (cmp) — patched brd + EINVAL fallback.
# Per TESTING.md "Host-local rig": brd -> nvme-loop -> XFS -> NFS (LOCALIO off),
# io_cache_write=2, N fresh connections x 16 MiB O_DIRECT, cmp source vs export.
set -e
CONNS=${CONNS:-30}
SRC=/dev/shm/zc-src-16m
TRACE=/sys/kernel/tracing
CFG=/sys/kernel/config/nvmet

echo "== kernel: $(uname -r), brd srcversion: $(modinfo -F srcversion brd)"

# --- rig up ---
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
if [ ! -b "$NVME" ]; then
    nvme connect -t loop -n zc
    sleep 1
    NVME=$(find_loop_ns)
fi
echo "== nvme-loop device: $NVME"
mountpoint -q /export/zc || mkfs.xfs -f -q $NVME
mkdir -p /export/zc /mnt/zc
mountpoint -q /export/zc || mount $NVME /export/zc
systemctl is-active -q nfs-server || systemctl start nfs-server
exportfs -o rw,no_root_squash,insecure,fsid=77 '*:/export/zc'
echo 2 > /sys/kernel/debug/nfsd/io_cache_write
modprobe nfs   # fresh boot: nfs.ko (and its localio param) may not exist yet
echo N > /sys/module/nfs/parameters/localio_enabled

# stamped source: 16 MiB of random data
head -c 16777216 /dev/urandom > $SRC

# tracing
echo 0 > $TRACE/tracing_on
echo > $TRACE/trace
for e in nfsd/nfsd_write_direct nfsd/nfsd_write_vector sunrpc/svcsock_tcp_rx_lifetime nfs/nfs_local_open_fh; do
    echo 1 > $TRACE/events/$e/enable 2>/dev/null || echo "(no event $e)"
done
echo 1 > $TRACE/tracing_on

fails=0; mismatches=0
for i in $(seq 1 $CONNS); do
    mount -t nfs -o vers=4.2,proto=tcp,sec=sys 127.0.0.1:/export/zc /mnt/zc
    if ! dd if=$SRC of=/mnt/zc/f$i bs=1M count=16 oflag=direct conv=fsync status=none; then
        fails=$((fails+1)); echo "conn $i: DD FAIL"
    fi
    umount /mnt/zc
    # fincore before cmp — cmp's own local reads populate the cache
    [ $i = 1 ] && fincore /export/zc/f1
    if ! cmp -s $SRC /export/zc/f$i; then
        mismatches=$((mismatches+1))
        echo "conn $i: CMP MISMATCH: $(cmp $SRC /export/zc/f$i 2>&1 | head -1)"
    fi
done

echo 0 > $TRACE/tracing_on
D=$(grep -c nfsd_write_direct $TRACE/trace || true)
V=$(grep -c nfsd_write_vector $TRACE/trace || true)
L=$(grep -c nfs_local_open_fh $TRACE/trace || true)
echo "== RESULT: $CONNS connections x 16 MiB: dd_fails=$fails cmp_mismatches=$mismatches direct=$D vector=$V localio_hits=$L (must be 0)"
echo "== loan accounting (last 5 rx_lifetime):"
grep svcsock_tcp_rx_lifetime $TRACE/trace | tail -5
echo "== borrowed/copied totals:"
grep -o 'borrowed=[0-9]*' $TRACE/trace | awk -F= '{s+=$2} END {print "borrowed="s}'
grep -o 'copied=[0-9]*' $TRACE/trace | awk -F= '{s+=$2} END {print "copied="s}'
echo "== fincore (POLLUTED by this script's cmp reads — see the pre-cmp f1 line above for the write-path footprint):"
fincore /export/zc/f1 /export/zc/f$CONNS 2>/dev/null || true
cp $TRACE/trace /tmp/rig-cmp-trace.txt

# restore localio; leave rig up for inspection (teardown script separate)
echo Y > /sys/module/nfs/parameters/localio_enabled
