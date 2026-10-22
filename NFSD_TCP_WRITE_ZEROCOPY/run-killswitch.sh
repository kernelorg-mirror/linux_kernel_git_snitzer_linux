#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Loan on/off validation: the region budget as the loans' switch.
#
# There is no loan switch since 22 October 2026: a record is loaned only
# when the budget its transport draws on is above 0 (loan-budget.sh says
# how).  Proves the two properties that turning loans on and off by the
# budget claims:
#   1. OFF (every budget 0): every WRITE receive classifies mode=copy
#      reason=no_budget, borrowed=0 -- and the data is byte-correct.
#   2. Setting a budget takes effect on the same transport's later records
#      (per-record consultation), and those receives publish loans again.
# The host's budgets are put back at the end.
# Optional KUnit execution is diagnostic here.  Required KUnit-profile rows
# own KUnit coverage; this production oracle claims only runtime properties.
#
# Rig: brd -> nvme-loop -> XFS -> NFS, LOCALIO off (mirrors run-rig-cmp.sh).
set -u
TRACE=/sys/kernel/tracing
CFG=/sys/kernel/config/nvmet
SRC=/dev/shm/ks-src-16m
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/loan-budget.sh"
FAILS=0
fail() { echo "FAIL: $*"; FAILS=$((FAILS+1)); }

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
            if (class_mode[key] == "copy" && class_reason[key] == "no_budget") {
                if (v["mode"] != "copy" || v["reason"] != "no_budget" ||
                    v["borrowed"] + 0 != 0) {
                    print "invalid no-budget publish for " v["xprt"] "/" v["sequence"] > "/dev/stderr"
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
                print "expected 16 no-budget and 16 loaned correlated records, saw " \
                      disabled + 0 " and " enabled + 0 > "/dev/stderr"
                bad = 1
            }
            if (!last_disabled || !first_enabled || last_disabled >= first_enabled) {
                print "no-budget records do not precede loaned records on one transport" > "/dev/stderr"
                bad = 1
            }
            if (!bad)
                print "correlated no_budget=" disabled " loaned=" enabled \
                      " xprt=" only_xprt " sequence=" last_disabled "<" first_enabled
            exit bad
        }' "$1"
}

if [ "${1:-}" = --check-trace ]; then
    [ "$#" = 2 ] || { echo "usage: $0 --check-trace TRACE" >&2; exit 2; }
    check_trace "$2"
    exit $?
fi
[ -w $LB_SP/svc_tcp_rx_budget_default_pages ] ||
    { echo "FAIL: $LB_SP/svc_tcp_rx_budget_default_pages missing or not writable"; exit 1; }
loans_save
trap loans_restore EXIT

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
loans_off || fail "could not set every budget to 0"
mount -t nfs -o vers=4.2,proto=tcp,sec=sys 127.0.0.1:/export/zc /mnt/zc
dd if=$SRC of=/mnt/zc/ks-off bs=1M count=16 oflag=direct conv=fsync \
   status=none || fail "off-state dd"
loans_on || fail "could not set a budget"      # mid-connection: same mount, same TCP connection
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
    loans_off
    dmesg -C
    modprobe svcsock_kunit; sleep 1
    totals=$(dmesg | grep -E '# sunrpc-svcsock-rx: pass' | tail -1)
    echo "== KUnit (loans off): ${totals:-NO RESULT}"
    echo "$totals" | grep -q 'fail:0' || fail "svcsock suite not green with loans off"
    [ "$(loans_state)" = off ] || fail "suite did not restore the host's budgets ($(loans_state))"
    KUNIT=pass
    modprobe -r svcsock_kunit 2>/dev/null || true
else
    echo "(KUnit phase skipped: svcsock_kunit not installed)"
fi

echo Y > /sys/module/nfs/parameters/localio_enabled
loans_restore
echo "== RESULT: fails=$FAILS (budgets restored: loans $(loans_state); optional-kunit=$KUNIT)"
[ $FAILS = 0 ] && echo "== LOAN ON/OFF VALIDATED: correlated no-budget copies and later loans on one transport"
exit $FAILS
