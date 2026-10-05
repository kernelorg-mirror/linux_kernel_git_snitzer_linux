#!/bin/bash
#
# localio-short-read-test.sh - regression test for
#   NFS/localio: detect a short read or write before the iterator has moved
#
# The defect: nfs_local_call_read()/_write() compared the bytes transferred
# against iov_iter_count() of the segment AFTER the call had advanced the
# iterator, so a short I/O between 50% and 99% of a segment went undetected
# and the next segment was issued at the wrong offset.
#
# The one deterministic way to make a segment short on a local filesystem is
# a read that hits EOF.  An O_DIRECT read that is misaligned at the start is
# split into a buffered prefix, a direct middle and a buffered suffix; put
# EOF inside the prefix:
#
#   EOF at < 50% of the prefix   detected by old and new code alike (control)
#   EOF at 50..99% of the prefix old code misses it and goes on to issue the
#                                direct middle at ki_pos == EOF
#   EOF at 100% of the prefix    the middle reads 0 bytes at EOF
#   EOF inside the middle        the suffix reads 0 bytes at EOF
#
# MEASURED 2026-09-24 (hs.436, lioA vs lioB): every case passes on the
# UNFIXED code too.  A short read caused by EOF leaves ki_pos exactly at
# i_size, and iomap's direct read returns 0 at its i_size check before it
# reaches the block-alignment check, so the missed detection is masked: the
# extra segments transfer nothing and the count comes out right.  The defect
# only shows when a short segment is FOLLOWED by one that still transfers,
# which on reads takes a writer extending the file between the two segments
# (a race this rig cannot stage), and on writes takes a short write from the
# local filesystem, which iomap never returns for a bvec (a partial direct
# write comes back as an error) and which RLIMIT_FSIZE cannot cause either,
# since nfs_file_direct_write() applies the limit before LOCALIO sees the
# write.  So this test is a NO-REGRESSION check of the fix across every EOF
# geometry of the split, with the data pattern verifying placement; the
# defect itself is established by inspection and by its identity with
# nfsd's 250ec14932d5.
#
# Every case checks the returned byte count AND the data (byte i of the file
# is i % 251), so a read that placed data at the wrong offset is caught even
# when the count looks right.  Runs through LOCALIO only; the alignment
# comes from the nfs_local_dio_read tracepoint of a probe read so the
# prefix length is measured, not assumed.
#
# Usage: localio-short-read-test.sh [OUTDIR]
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
MNT=/mnt/t4; EXP=/export/t4
PREAD=$HERE/odirect_pread
OUT=${1:-/var/tmp/localio-short-read-$(date +%Y%m%d-%H%M%S)}; mkdir -p "$OUT"
T=/sys/kernel/tracing/instances/lioshort
OFF=3000; LEN=8192; MOD=251
DMSG='Unexpected direct I/O read alignment failure'

[ -x "$PREAD" ] || gcc -O2 -o "$PREAD" "$HERE/odirect_pread.c" || exit 2
findmnt -n $MNT >/dev/null || { echo "$MNT not mounted" >&2; exit 1; }
lio=$(cat /sys/fs/nfs/*/localio 2>/dev/null | head -1)
[ "$lio" = 1 ] || { echo "LOCALIO is not active for this mount (localio=${lio:-none}); echo Y > /sys/module/nfs/parameters/localio_enabled and mount with start-nfsd.sh mount-client -L" >&2; exit 1; }

mkdir -p $T 2>/dev/null
for e in nfs_local_dio_read nfs_local_dio_misaligned; do echo 1 > $T/events/nfs/$e/enable; done
cleanup() { for e in nfs_local_dio_read nfs_local_dio_misaligned; do echo 0 > $T/events/nfs/$e/enable; done; rmdir $T 2>/dev/null; rm -f $EXP/lio-eof-*; }
trap cleanup EXIT

mkfile() { # PATH SIZE - byte i = i % MOD, written on the server side
	python3 - "$1" "$2" "$MOD" <<'PY'
import sys
p, n, m = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
open(p, 'wb').write(bytes(i % m for i in range(n)))
PY
	sync
}
dmsg_count() { dmesg | grep -c "$DMSG"; }

echo "== localio short-read test: nfs=$(cat /sys/module/nfs/srcversion) kernel=$(uname -r) read $LEN@$OFF" | tee "$OUT/summary.txt"

# Probe: a whole-file read, to learn the split geometry this rig gives.
mkfile $EXP/lio-eof-probe 16384
echo > $T/trace; echo 1 > $T/tracing_on
"$PREAD" $MNT/lio-eof-probe $OFF $LEN $MOD > "$OUT/probe.out"
echo 0 > $T/tracing_on; cat $T/trace > "$OUT/probe.trace"
ev=$(grep -c 'nfs_local_dio_read:' "$OUT/probe.trace")
[ "$ev" -ge 1 ] || { echo "probe read did not take the localio DIO split path (no nfs_local_dio_read event); cannot test" | tee -a "$OUT/summary.txt"; exit 1; }
align=$(grep -o 'offset_align=[0-9]*' "$OUT/probe.trace" | head -1 | cut -d= -f2)
prefix=$(grep -o 'start=[0-9]*+[0-9]*' "$OUT/probe.trace" | head -1 | cut -d+ -f2)
middle=$(grep -o 'middle=[0-9]*+[0-9]*' "$OUT/probe.trace" | head -1 | cut -d+ -f2)
echo "split geometry: offset_align=$align prefix=$prefix middle=$middle ($(cat "$OUT/probe.out" | head -1))" | tee -a "$OUT/summary.txt"
[ "$prefix" -gt 0 ] && [ "$middle" -gt 0 ] || { echo "read at $OFF is not split into prefix+middle on this rig; pick another OFF" | tee -a "$OUT/summary.txt"; exit 1; }

# cases: name  EOF
cases="control-27pct:$((OFF + prefix * 27 / 100))
bug-82pct:$((OFF + prefix * 82 / 100))
prefix-100pct:$((OFF + prefix))
middle-75pct:$((OFF + prefix + middle * 75 / 100))
full:$((OFF + LEN + 4096))"

FAILED=0
printf '%-14s %-6s %-9s %-9s %-6s %-5s %s\n' case EOF expected got dioev dmesg verdict | tee -a "$OUT/summary.txt"
for c in $cases; do
	name=${c%%:*}; eof=${c#*:}
	f=lio-eof-$name
	mkfile $EXP/$f $eof
	exp=$(( eof - OFF )); [ $exp -gt $LEN ] && exp=$LEN
	d0=$(dmsg_count)
	echo > $T/trace; echo 1 > $T/tracing_on
	"$PREAD" $MNT/$f $OFF $LEN $MOD > "$OUT/$name.out" 2>&1; rc=$?
	echo 0 > $T/tracing_on; cat $T/trace > "$OUT/$name.trace"
	d1=$(dmsg_count)
	got=$(grep -o 'ret=-\?[0-9]*' "$OUT/$name.out" | cut -d= -f2)
	ev=$(grep -c 'nfs_local_dio_read:' "$OUT/$name.trace")
	v=PASS
	if [ "$ev" -lt 1 ]; then v="INCONCLUSIVE: not split"; FAILED=$((FAILED+1))
	elif [ "$got" != "$exp" ]; then v="FAIL: $(grep -o 'errno=.*' "$OUT/$name.out" || echo "wrong count")"; FAILED=$((FAILED+1))
	elif [ $rc -eq 2 ]; then v="FAIL: $(grep mismatch "$OUT/$name.out")"; FAILED=$((FAILED+1))
	fi
	printf '%-14s %-6s %-9s %-9s %-6s %-5s %s\n' $name $eof $exp "$got" $ev $((d1-d0)) "$v" | tee -a "$OUT/summary.txt"
done
if [ $FAILED -gt 0 ]; then echo "== $FAILED failure(s); files in $OUT" | tee -a "$OUT/summary.txt"; exit 1; fi
echo "== all short-read cases passed; files in $OUT" | tee -a "$OUT/summary.txt"
