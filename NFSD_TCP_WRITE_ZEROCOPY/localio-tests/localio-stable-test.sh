#!/bin/bash
#
# localio-stable-test.sh - regression test for
#   NFS/localio: report the stability a DIO WRITE actually has
#
# Since d32ddfeb5593 a DIO WRITE through LOCALIO is issued IOCB_DSYNC|IOCB_SYNC
# whatever the caller asked for, but the reply reported what was asked, so an
# UNSTABLE direct write was persisted and then COMMITted as well.  With the fix
# the reply says FILE_SYNC and no COMMIT follows.
#
# Reads client tracepoints, which fire for LOCALIO just as for RPC:
#   nfs:nfs_initiate_write   stable=   what the client ASKED, per WRITE
#   nfs:nfs_writeback_done   stable=   what LOCALIO REPLIED, per WRITE
#   nfs:nfs_initiate_commit            COMMITs issued (nfs_local_commit)
#
# Workloads and expectations (LOCALIO on; io_cache_write is irrelevant, the
# WRITEs never reach nfsd):
#   dio4m   O_DIRECT 4 MiB records = 4 x wsize, so the client asks UNSTABLE
#           fixed: replied FILE_SYNC, zero COMMITs
#           stock: replied UNSTABLE, COMMITs follow
#   buf     buffered write + fsync: writeback asks UNSTABLE, LOCALIO does not
#           force sync on buffered writes, so UNSTABLE is replied and a COMMIT
#           follows on both arms (the control: the fix must not touch this)
#   dio47k  O_DIRECT 47008-byte records: single RPC, client asks FILE_SYNC,
#           FILE_SYNC replied, no COMMIT, on both arms
#
# Exit: 0 = fixed behaviour, 3 = stock behaviour observed (not a test
# failure, the arm is stock), 1 = something else.
#
# Usage: localio-stable-test.sh [OUTDIR]
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
IOR=${IOR:-$HERE/iorhard}
[ -x "$IOR" ] || gcc -O2 -pthread -o "$IOR" "$HERE/iorhard.c" || exit 1
MNT=/mnt/t4
OUT=${1:-/var/tmp/localio-stable-$(date +%Y%m%d-%H%M%S)}; mkdir -p "$OUT"
T=/sys/kernel/tracing/instances/liostable
EVENTS="nfs/nfs_initiate_write nfs/nfs_writeback_done nfs/nfs_initiate_commit"

[ -x "$IOR" ] || { echo "no iorhard at $IOR" >&2; exit 1; }
findmnt -n $MNT >/dev/null || { echo "$MNT not mounted" >&2; exit 1; }
lio=$(cat /sys/fs/nfs/*/localio 2>/dev/null | head -1)
[ "$lio" = 1 ] || { echo "LOCALIO is not active for this mount (localio=${lio:-none})" >&2; exit 1; }

mkdir -p $T 2>/dev/null
for e in $EVENTS; do echo 1 > $T/events/$e/enable; done
echo 65536 > $T/buffer_size_kb
cleanup() { for e in $EVENTS; do echo 0 > $T/events/$e/enable; done; rmdir $T 2>/dev/null; rm -f $MNT/liostable.dat; }
trap cleanup EXIT

run_wl() {
	local f=$MNT/liostable.dat
	rm -f $f; sync
	case $1 in
	dio4m)  "$IOR" $f 8 30 4194304 $((512*1024*1024)) >/dev/null 2>&1 ;;
	buf)    dd if=/dev/zero of=$f bs=1M count=256 conv=fsync status=none ;;
	dio47k) "$IOR" $f 8 30 47008 $((64*1024*1024)) >/dev/null 2>&1 ;;
	esac
	rm -f $f
}

fixed=0; stock=0; bad=0
{
echo "== localio stability test: nfs=$(cat /sys/module/nfs/srcversion) kernel=$(uname -r) wsize=$(findmnt -n -o OPTIONS $MNT | tr ',' '\n' | sed -n 's/^wsize=//p')"
printf '%-7s | %-36s | %-36s | %-7s | %s\n' wl "client ASKED (WRITEs)" "LOCALIO REPLIED" COMMITs verdict
} | tee "$OUT/summary.txt"
for wl in dio4m buf dio47k; do
	echo 0 > $T/tracing_on; echo > $T/trace; echo 1 > $T/tracing_on
	run_wl $wl; sleep 1
	echo 0 > $T/tracing_on; cat $T/trace > "$OUT/$wl.trace"
	lost=$(grep -o 'entries-in-buffer/entries-written: [0-9]*/[0-9]*' "$OUT/$wl.trace" | awk -F'[ /]' '{print ($3==$4)?"no":"YES"}')
	read -r aU aD aF rU rD rF cc < <(awk '
		/nfs_initiate_write:/ { if (/stable=UNSTABLE/) aU++; else if (/stable=DATA_SYNC/) aD++; else if (/stable=FILE_SYNC/) aF++ }
		/nfs_writeback_done:/ { if (/stable=UNSTABLE/) rU++; else if (/stable=DATA_SYNC/) rD++; else if (/stable=FILE_SYNC/) rF++ }
		/nfs_initiate_commit:/ { cc++ }
		END { printf "%d %d %d %d %d %d %d\n", aU, aD, aF, rU, rD, rF, cc }' "$OUT/$wl.trace")
	v=""
	if [ "$lost" = YES ]; then v="TRACE OVERRUN"; bad=$((bad+1))
	elif [ $((aU + aD + aF)) -eq 0 ]; then v="FAIL: no WRITEs traced"; bad=$((bad+1))
	else case $wl in
		dio4m)
			if [ "$aU" -eq 0 ]; then v="INCONCLUSIVE: client asked no UNSTABLE"; bad=$((bad+1))
			elif [ "$rU" -eq 0 ] && [ "$rF" -ge "$aU" ] && [ "$cc" -eq 0 ]; then v="FIXED: $aU UNSTABLE reported FILE_SYNC, no COMMIT"; fixed=$((fixed+1))
			elif [ "$rU" -eq "$aU" ] && [ "$cc" -gt 0 ]; then v="STOCK: UNSTABLE echoed back, $cc COMMITs for persisted data"; stock=$((stock+1))
			else v="FAIL: unexpected mix"; bad=$((bad+1)); fi ;;
		buf)
			if [ "$aU" -gt 0 ] && [ "$rU" -eq "$aU" ] && [ "$cc" -gt 0 ]; then v="ok: buffered unchanged (UNSTABLE, COMMIT)"
			else v="FAIL: buffered path changed"; bad=$((bad+1)); fi ;;
		dio47k)
			if [ "$aF" -gt 0 ] && [ "$rF" -eq "$aF" ] && [ "$cc" -eq 0 ]; then v="ok: FILE_SYNC asked and replied"
			else v="FAIL"; bad=$((bad+1)); fi ;;
		esac
	fi
	printf '%-7s | U=%-8s D=%-8s F=%-10s | U=%-8s D=%-8s F=%-10s | %-7s | %s\n' $wl $aU $aD $aF $rU $rD $rF $cc "$v" | tee -a "$OUT/summary.txt"
done
[ $bad -gt 0 ] && { echo "== $bad problem(s); traces in $OUT" | tee -a "$OUT/summary.txt"; exit 1; }
[ $stock -gt 0 ] && { echo "== stock behaviour observed (arm is unfixed); traces in $OUT" | tee -a "$OUT/summary.txt"; exit 3; }
echo "== fixed behaviour confirmed; traces in $OUT" | tee -a "$OUT/summary.txt"
