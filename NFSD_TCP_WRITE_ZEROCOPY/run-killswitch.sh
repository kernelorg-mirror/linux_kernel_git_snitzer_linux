#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Loan kill-switch validation (sunrpc.svc_tcp_rx_loan_pages).
#
# Proves the three properties the switch claims:
#   1. OFF: every WRITE receive classifies mode=copy reason=disabled,
#      borrowed=0 — and the data is byte-correct (the pre-loan receive).
#   2. Flipping to ON takes effect on the same transport's later records
#      (per-record consultation), and those receives publish loans again.
# Optional KUnit execution is diagnostic here.  Required KUnit-profile rows
# own KUnit coverage; this production oracle claims only runtime properties.
#
# Rig: brd -> nvme-loop -> XFS -> NFS, LOCALIO off (mirrors run-rig-cmp.sh).
set -u
TRACE=/sys/kernel/tracing
CFG=/sys/kernel/config/nvmet
SRC=/dev/shm/ks-src-16m
PARAM=/sys/module/sunrpc/parameters/svc_tcp_rx_loan_pages
FAILS=0
fail() { echo "FAIL: $*"; FAILS=$((FAILS+1)); }

require_default_on() {
    [ -r "$1" ] && [ "$(cat "$1")" = Y ]
}

# Correlate classification and publication for each large RPC record.  A
# common mount is not transport identity: xprt+sequence is the record key.
check_trace() {
    awk '
        /svcsock_tcp_rx_lifetime/ {
            delete v
            for (f = 1; f <= NF; f++) {
                split($f, kv, "=")
                v[kv[1]] = kv[2]
            }
            key = v["xprt"] SUBSEP v["sequence"]
            if (v["action"] == "classify") {
                if (key in class_mode) {
                    print "duplicate classify for " v["xprt"] "/" v["sequence"] > "/dev/stderr"
                    bad = 1
                }
                class_mode[key] = v["mode"]
                class_reason[key] = v["reason"]
                next
            }
            if (v["action"] != "publish" || v["body"] + 0 < 1000000)
                next
            if (key in published) {
                print "duplicate publish for " v["xprt"] "/" v["sequence"] > "/dev/stderr"
                bad = 1
                next
            }
            published[key] = 1
            if (!(key in class_mode)) {
                print "publish without classify for " v["xprt"] "/" v["sequence"] > "/dev/stderr"
                bad = 1
                next
            }
            if (only_xprt == "")
                only_xprt = v["xprt"]
            else if (v["xprt"] != only_xprt) {
                print "large records crossed transports: " only_xprt " and " v["xprt"] > "/dev/stderr"
                bad = 1
            }
            sequence = v["sequence"] + 0
            if (class_mode[key] == "copy" && class_reason[key] == "disabled") {
                if (v["mode"] != "copy" || v["reason"] != "disabled" ||
                    v["borrowed"] + 0 != 0) {
                    print "invalid disabled publish for " v["xprt"] "/" v["sequence"] > "/dev/stderr"
                    bad = 1
                }
                disabled++
                if (sequence > last_disabled)
                    last_disabled = sequence
            } else if (class_mode[key] == "mixed" && class_reason[key] == "none") {
                if (v["mode"] != "published" ||
                    (v["reason"] != "none" && v["reason"] != "locked-head") ||
                    v["borrowed"] + 0 == 0) {
                    print "invalid enabled publish for " v["xprt"] "/" v["sequence"] > "/dev/stderr"
                    bad = 1
                }
                enabled++
                if (!first_enabled || sequence < first_enabled)
                    first_enabled = sequence
            } else {
                print "unexpected classify mode/reason " class_mode[key] "/" class_reason[key] \
                      " for " v["xprt"] "/" v["sequence"] > "/dev/stderr"
                bad = 1
            }
        }
        END {
            if (disabled != 16 || enabled != 16) {
                print "expected 16 disabled and 16 enabled correlated records, saw " \
                      disabled + 0 " and " enabled + 0 > "/dev/stderr"
                bad = 1
            }
            if (!last_disabled || !first_enabled || last_disabled >= first_enabled) {
                print "disabled records do not precede enabled records on one transport" > "/dev/stderr"
                bad = 1
            }
            if (!bad)
                print "correlated disabled=" disabled " enabled=" enabled \
                      " xprt=" only_xprt " sequence=" last_disabled "<" first_enabled
            exit bad
        }' "$1"
}

if [ "${1:-}" = --check-trace ]; then
    [ "$#" = 2 ] || { echo "usage: $0 --check-trace TRACE" >&2; exit 2; }
    check_trace "$2"
    exit $?
fi
if [ "${1:-}" = --check-default ]; then
    [ "$#" = 2 ] || { echo "usage: $0 --check-default PARAM" >&2; exit 2; }
    require_default_on "$2"
    exit $?
fi

[ -w $PARAM ] || { echo "FAIL: $PARAM missing or not writable"; exit 1; }
require_default_on "$PARAM" || {
    echo "FAIL: $PARAM must be Y before production validation"
    exit 1
}

# --- rig up (idempotent) ---
modprobe brd rd_nr=1 rd_size=4194304
modprobe -a nvmet nvme_loop
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

L=$(grep -c nfs_local_open_fh $TRACE/trace || true)
[ "$L" = 0 ] || fail "LOCALIO engaged"
check_trace "$TRACE/trace" || fail "classify/publish correlation"
rm -f /export/zc/ks-off /export/zc/ks-on

# --- optional diagnostic: required KUnit-profile rows own KUnit coverage ---
KUNIT=unavailable
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
    KUNIT=pass
    echo Y > $PARAM
    modprobe -r svcsock_kunit 2>/dev/null || true
else
    echo "(KUnit phase skipped: svcsock_kunit not installed)"
    echo Y > $PARAM
fi

echo Y > /sys/module/nfs/parameters/localio_enabled
echo "== RESULT: fails=$FAILS (switch restored to Y; optional-kunit=$KUNIT)"
[ $FAILS = 0 ] && echo "== KILL-SWITCH VALIDATED: correlated off copies and later on loans on one transport"
exit $FAILS
