#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Loan kill-switch validation (sunrpc.svc_tcp_rx_loan_pages).
#
# Proves the three properties the switch claims:
#   1. OFF: every WRITE receive classifies mode=copy reason=disabled,
#      borrowed=0 — and the data is byte-correct (the pre-loan receive).
#   2. Flipping to ON takes effect on the SAME connection's next RPCs
#      (per-record consultation), receives publish loans again.
#   3. The svcsock KUnit suite passes with the switch OFF on the host
#      (its init/exit forces the switch on per test and restores it).
#
# Rig: brd -> nvme-loop -> XFS -> NFS, LOCALIO off (mirrors run-rig-cmp.sh).
set -u
TRACE=/sys/kernel/tracing
CFG=/sys/kernel/config/nvmet
SRC=/dev/shm/ks-src-16m
PARAM=/sys/module/sunrpc/parameters/svc_tcp_rx_loan_pages
FAILS=0
fail() { echo "FAIL: $*"; FAILS=$((FAILS+1)); }

[ -w $PARAM ] || { echo "FAIL: $PARAM missing or not writable"; exit 1; }

# --- rig up (idempotent) ---
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
    nvme connect -t loop -n zc; sleep 1; NVME=$(find_loop_ns)
fi
mountpoint -q /export/zc || mkfs.xfs -f -q $NVME
mkdir -p /export/zc /mnt/zc
mountpoint -q /export/zc || mount $NVME /export/zc
systemctl is-active -q nfs-server || systemctl start nfs-server
exportfs -o rw,no_root_squash,insecure,fsid=77 '*:/export/zc'
echo 2 > /sys/kernel/debug/nfsd/io_cache_write
modprobe nfs
echo N > /sys/module/nfs/parameters/localio_enabled
head -c 16777216 /dev/urandom > $SRC

echo 0 > $TRACE/tracing_on
for e in sunrpc/svcsock_tcp_rx_lifetime nfsd/nfsd_write_direct \
         nfsd/nfsd_write_vector nfs/nfs_local_open_fh; do
    echo 1 > $TRACE/events/$e/enable 2>/dev/null || true
done

# Count 1 MiB WRITE publishes by (reason, borrowed-or-not) over the trace.
count_big() {  # count_big REASON_REGEX BORROWED_TEST
    awk -v re="$1" -v bt="$2" '
        /svcsock_tcp_rx_lifetime/ && /action=publish/ {
            delete v
            for (f = 1; f <= NF; f++) { split($f, kv, "="); v[kv[1]] = kv[2] }
            if (v["body"] + 0 < 1000000) next
            if (v["reason"] !~ re) next
            if (bt == "zero" && v["borrowed"] + 0 != 0) next
            if (bt == "nonzero" && v["borrowed"] + 0 == 0) next
            n++
        }
        END { print n + 0 }' $TRACE/trace
}

# --- phase 1+2: OFF then ON across one connection ---
echo > $TRACE/trace
echo 1 > $TRACE/tracing_on
echo N > $PARAM
mount -t nfs -o vers=4.2,proto=tcp,sec=sys 127.0.0.1:/export/zc /mnt/zc
dd if=$SRC of=/mnt/zc/ks-off bs=1M count=16 oflag=direct conv=fsync \
   status=none || fail "off-state dd"
echo Y > $PARAM      # flip mid-connection: same mount, same TCP connection
dd if=$SRC of=/mnt/zc/ks-on bs=1M count=16 oflag=direct conv=fsync \
   status=none || fail "on-state dd"
umount /mnt/zc
echo 0 > $TRACE/tracing_on

cmp -s $SRC /export/zc/ks-off || fail "off-state data mismatch"
cmp -s $SRC /export/zc/ks-on  || fail "on-state data mismatch"

disabled=$(count_big '^disabled$' zero)
loaned=$(count_big '^(none|locked-head)$' nonzero)
leaked=$(count_big '^(none|locked-head)$' zero)
L=$(grep -c nfs_local_open_fh $TRACE/trace || true)
[ "$L" = 0 ] || fail "LOCALIO engaged"
[ "$disabled" = 16 ] || fail "expected 16 disabled 1M receives, saw $disabled"
[ "$loaned" = 16 ] || fail "expected 16 loaned 1M receives after flip, saw $loaned"
[ "$leaked" = 0 ] || fail "$leaked loan-reason receives with borrowed=0"
borrowed_off=$(awk '/svcsock_tcp_rx_lifetime/ && /reason=disabled/ {
        for (f = 1; f <= NF; f++) { split($f, kv, "="); v[kv[1]] = kv[2] }
        s += v["borrowed"] } END { print s + 0 }' $TRACE/trace)
[ "$borrowed_off" = 0 ] || fail "disabled receives borrowed $borrowed_off bytes"
echo "== off/on: disabled-1M=$disabled loaned-1M-after-flip=$loaned" \
     "borrowed-while-off=$borrowed_off"
rm -f /export/zc/ks-off /export/zc/ks-on

# --- phase 3: svcsock KUnit suite with the switch OFF on the host ---
if modinfo svcsock_kunit >/dev/null 2>&1; then
    modprobe -r svcsock_kunit 2>/dev/null || true
    modprobe -r kunit 2>/dev/null || true
    modprobe kunit enable=1
    echo N > $PARAM
    dmesg -C
    modprobe svcsock_kunit; sleep 1
    totals=$(dmesg | grep -E '# sunrpc-svcsock-rx: pass' | tail -1)
    echo "== KUnit (switch off): ${totals:-NO RESULT}"
    echo "$totals" | grep -q 'fail:0' || fail "svcsock suite not green with switch off"
    [ "$(cat $PARAM)" = N ] || fail "suite did not restore the host's switch setting"
    echo Y > $PARAM
    modprobe -r svcsock_kunit 2>/dev/null || true
else
    echo "(KUnit phase skipped: svcsock_kunit not installed)"
    echo Y > $PARAM
fi

echo Y > /sys/module/nfs/parameters/localio_enabled
echo "== RESULT: fails=$FAILS (switch restored to Y)"
[ $FAILS = 0 ] && echo "== KILL-SWITCH VALIDATED: off copies with reason=disabled, flip re-loans mid-connection, suite green with host switch off"
exit $FAILS
