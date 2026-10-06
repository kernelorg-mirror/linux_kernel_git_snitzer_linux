#!/bin/bash
#
# localio-boundary-test.sh - the boundary-page measurement of
# ../NFSD_DONTCACHE_FALLBACK/boundary-ab.sh, for writes that go through
# LOCALIO instead of nfsd_vfs_write().  Same workload (iorhard, 32 ranks,
# 15 s, 2 GiB cap, record sizes 47008 = split and 6000 = whole-WRITE
# fallback), same instruments, different WRITE counter: t4-collect.sh keys
# everything off nfsd_write_dio_split, which a LOCALIO write never emits, so
# this counts nfs:nfs_local_dio_write instead, one per O_DIRECT write header
# that had a split geometry computed.
#
# Reports, per record size: WRITEs, READ bios on the export device (read
# amplification: a boundary page dropped before its partner wrote it is
# read back), DONTCACHE flusher rounds (must be ~0: every direct write is
# synchronous), and the page-cache footprint the write leaves, measured
# directly as the file's resident pages (mincore, ../resident) rather than
# from meminfo Cached, which the tmpfs-backed export image confounds.
# Applies boundary-ab.sh's invariants.  Needs LOCALIO active on the mount.
#
# Every write must succeed.  A rank whose write fails stops there, iorhard
# exits 1, and the record size is a FAIL whatever its counters say; the
# reason is in rs<RS>.ior.err.  The kernel log is also checked for a page
# cache invalidation failure on direct I/O, which is what a direct and a
# buffered write meeting in one page leaves behind (it set -EIO on the file
# and failed a neighbour's write, 2026-10-06 at 64 KiB, and this test passed).
# The kernel prints that at most ten times a day, so its absence proves less
# than its presence.
#
# Page size.  The read invariant is about a page two writes share, so it
# holds only for records of at least a page: a smaller record leaves pages
# with three or more writers, which the two-writer claim does not cover.
# The small record is therefore scaled, 6000 * PAGE_SIZE / 4096 (6000,
# 24000, 96000).  On larger pages a raw 6000-byte row is still run, and
# any record smaller than a page has its reads and its kick rounds
# reported, not judged.  The
# two ceilings are the 4 KiB ones; which applies follows what the record
# does (never_split below), not its size.
#
# Usage: localio-boundary-test.sh [-l LABEL] [-r "RS RS ..."] [-n RANKS] [OUTDIR]
#   -n  iorhard ranks (default 8 = the nfsd thread count boundary-ab.sh's
#       ceilings were calibrated at; the claim protocol's residual read rate
#       grows with writer concurrency, and LOCALIO writes in application
#       context, so 32 ranks here is 32 concurrent writers where nfsd would
#       serialise them into 8 threads; measured 2026-09-24: 6000-byte
#       records 0.0005 reads/WRITE at 8 ranks, 0.018 at 32, and nfsd itself
#       0.027 at 32 threads)
set -u
PAGE=$(getconf PAGESIZE)
SMALL=$((6000 * PAGE / 4096))	# the whole-WRITE-fallback record, page-scaled
RS="47008 $SMALL"; [ "$SMALL" != 6000 ] && RS="$RS 6000"
LABEL="" RANKS=8
while getopts "l:r:n:" o; do case $o in l) LABEL=$OPTARG;; r) RS=$OPTARG;; n) RANKS=$OPTARG;; *) exit 2;; esac; done
shift $((OPTIND-1))
HERE=$(cd "$(dirname "$0")" && pwd)
IOR=${IOR:-$HERE/iorhard}
# Rebuild a binary older than its source: an old iorhard exits 0 after a
# failed write, which the check below relies on it not doing.
{ [ -x "$IOR" ] && ! [ "$HERE/iorhard.c" -nt "$IOR" ]; } || gcc -O2 -pthread -o "$IOR" "$HERE/iorhard.c" || exit 2
MNT=/mnt/t4; EXP=/export/t4
OUT=${1:-/var/tmp/localio-boundary-$(date +%Y%m%d-%H%M%S)}; mkdir -p "$OUT"
T=/sys/kernel/tracing/instances/lioboundary
EVENTS="nfs/nfs_local_dio_write block/block_bio_queue writeback/writeback_start writeback/writeback_written"

[ -x "$IOR" ] || { echo "no iorhard at $IOR" >&2; exit 2; }
RES=$HERE/resident
[ -x "$RES" ] || gcc -O2 -o "$RES" "$HERE/resident.c" || exit 2
findmnt -n $MNT >/dev/null || { echo "$MNT not mounted" >&2; exit 1; }
lio=$(cat /sys/fs/nfs/*/localio 2>/dev/null | head -1)
[ "$lio" = 1 ] || { echo "LOCALIO is not active for this mount (localio=${lio:-none})" >&2; exit 1; }
dev=$(findmnt -n -o SOURCE $EXP); bdev=$(basename "$dev")
bdi=$(cat /sys/class/block/$bdev/dev)          # MAJ:MIN, block_bio_queue prints MAJ,MIN
[ -n "$bdi" ] || { echo "cannot find the export device's MAJ:MIN" >&2; exit 1; }

mkdir -p $T 2>/dev/null
for e in $EVENTS; do echo 1 > $T/events/$e/enable; done
echo 262144 > $T/buffer_size_kb
cleanup() { for e in $EVENTS; do echo 0 > $T/events/$e/enable; done; rmdir $T 2>/dev/null; rm -f $MNT/liob.ior; }
trap cleanup EXIT

cached_kb() { awk '/^Cached:/ {print $2}' /proc/meminfo; }
inval_msgs() { dmesg | grep -c 'Page cache invalidation failure on direct I/O'; }

FAILED=0
KICK_BDEV=$bdev
# A buffered DONTCACHE segment that covers a whole page allocates that page
# itself, and mm counts it in WB_DONTCACHE_DIRTY, so its writes wake the
# DONTCACHE writeback kick whatever NFSD does.  The pages NFSD keeps for a
# neighbouring WRITE are marked only once the WRITE is complete, after its
# fsync if it has one, and so should wake it (almost) never.  The kick check
# therefore applies only where no buffered segment can cover a whole page
# and no page has more than two writers: records large enough to always
# split (direct middle of at least direct_misaligned_min_bytes bytes, ends
# within one page each).  A record smaller than a page is not judged, for
# the reason its reads are not: more than two writes share a page, every
# one after the first marks it, and with several writing at once it is
# marked while a neighbour's data is still dirty.  Measured 2026-10-05 at
# 64 KiB, 6000-byte records, 8 ranks, about 11 writes to a page: 0.058
# rounds per WRITE on NVMe/TCP and 0.106 on nvme-loop, following how often
# two tasks wrote the same page within 0.5 ms (41% and 76%).  A ceiling
# scaled by writers per page would reach the 0.10-0.54 measured below for
# pages marked dirty, and so would catch nothing.
# It no longer depends on the export's logical block size: a split
# DONTCACHE write is cut at page boundaries when the DIO offset alignment is
# finer than a page, so its pages are kept there too.  Elsewhere the rounds
# are reported, not judged.  Ceiling: 0.05 rounds per
# WRITE plus 100; measured 2026-10-02 on 7.1.13-18: 0.003-0.015 with the
# pages marked after the fsync, 0.10 (3000-byte records) to 0.54 (47008)
# with them marked dirty, and 2026-09-20 marking at creation: 0.9.
# The split gate: bytes (direct_misaligned_min_bytes) on kernels that have
# it, pages (direct_misaligned_num_pages) on older ones.
gate_bytes() {
	local np
	if [ -r /sys/kernel/debug/nfsd/direct_misaligned_min_bytes ]; then
		cat /sys/kernel/debug/nfsd/direct_misaligned_min_bytes
	else
		np=$(cat /sys/kernel/debug/nfsd/direct_misaligned_num_pages 2>/dev/null || echo 2)
		echo $((PAGE * np))
	fi
}
# A record is never split if the most whole pages it can hold is under the
# gate: 6000 bytes on 4 KiB pages (4096 < 8192).  Such a record has the
# tight read ceiling; one that can split has the split ceiling, as 47008
# does.  On larger pages a record of at least a page can always split under
# the default gate, so the scaled small record is judged as a split one.
never_split() { [ $(( $1 / PAGE * PAGE )) -lt "$(gate_bytes)" ]; }
kick_checked() {	# kick_checked RS -> true if rounds must stay near zero
	local rs=$1 page=$PAGE min
	min=$(gate_bytes)
	[ "$rs" -ge "$page" ] && [ "$rs" -ge $((min + 2 * page)) ]
}
echo "== localio boundary ${LABEL:+[$LABEL] }ranks=$RANKS nfs=$(cat /sys/module/nfs/srcversion) nfsd=$(cat /sys/module/nfsd/srcversion) nfs_dio=$(cat /sys/module/nfs_dio/srcversion 2>/dev/null || echo none) export=$dev($bdi) -> $OUT" | tee "$OUT/summary.txt"
for rs in $RS; do
	rm -f $MNT/liob.ior; sync; echo 3 > /proc/sys/vm/drop_caches
	echo 0 > $T/tracing_on; echo > $T/trace
	c0=$(cached_kb)
	echo 1 > $T/tracing_on
	inval0=$(inval_msgs)
	"$IOR" $MNT/liob.ior $RANKS 15 $rs $((2*1024*1024*1024)) 2> "$OUT/rs$rs.ior.err" > "$OUT/rs$rs.ior"
	ior_rc=$?
	sync; sleep 2
	echo 0 > $T/tracing_on
	c1=$(cached_kb)
	resident=$("$RES" $EXP/liob.ior 2>/dev/null | grep -o '[0-9.]*%' | tr -d '%')
	cat $T/trace > "$OUT/rs$rs.trace"
	lost=$(grep -o 'entries-in-buffer/entries-written: [0-9]*/[0-9]*' "$OUT/rs$rs.trace" | awk -F'[ /]' '{print ($3==$4)?"no":"YES"}')
	read -r w wb rb rsz rounds < <(awk -v bdi="$bdi" '
		/nfs_local_dio_write:/ { w++; for (i=1;i<=NF;i++) if ($i ~ /^count=/) { split($i,kv,"="); wb+=kv[2] } }
		/block_bio_queue:/ {
			for (i=1;i<=NF;i++) if ($i == "block_bio_queue:") { dev=$(i+1); op=$(i+2); n=$(i+5); break }
			gsub(",", ":", dev); if (dev != bdi) next
			if (op ~ /^R/) { r++; rsec+=n }
		}
		/writeback_start:/ && /reason=dontcache/ { rounds++ }
		END { printf "%d %d %d %d %d\n", w, wb, r, rsec, rounds }' "$OUT/rs$rs.trace")
	own=$(( $(stat -c %s "$OUT/rs$rs.trace") / 1024 ))
	grow=$(( c1 - c0 - own ))
	pct=$( [ "$wb" -gt 0 ] && awk -v g=$grow -v b=$wb 'BEGIN{printf "%.1f", 100*g*1024/b}' || echo "?" )
	rpw=$( [ "$w" -gt 0 ] && awk -v r=$rb -v w=$w 'BEGIN{printf "%.3f", r/w}' || echo "?" )
	v="ok"
	if [ "$lost" = YES ]; then v="TRACE OVERRUN"; FAILED=$((FAILED+1))
	elif [ "$w" -eq 0 ]; then v="FAIL: no LOCALIO direct writes traced (LOCALIO off, or not O_DIRECT?)"; FAILED=$((FAILED+1))
	else
		if never_split "$rs"; then lim=$((w / 200 + 50)); else lim=$((w / 40 + 50)); fi
		klim=$((w / 20 + 100))
		if [ "$rs" -lt "$PAGE" ]; then
			:	# said with the reads, below
		elif ! kick_checked "$rs"; then
			v="kick rounds not judged (a buffered segment can cover a whole page)"
		elif [ "$rounds" -gt "$klim" ]; then
			v="FAIL: $rounds DONTCACHE kick rounds, over the $klim ceiling (kept pages marked while dirty)"; FAILED=$((FAILED+1))
		fi
		if [ "$rs" -lt "$PAGE" ]; then
			v="reads and kick rounds not judged: record smaller than a page, more than two writers share one"
		elif [ "$rb" -gt "$lim" ]; then
			v="FAIL: $rb READ bios for $w WRITEs, over the $lim ceiling (shared pages not kept)"; FAILED=$((FAILED+1))
		fi
	fi
	inval=$(( $(inval_msgs) - inval0 ))
	if [ "$ior_rc" -ne 0 ]; then
		v="FAIL: a write failed (iorhard exit $ior_rc: $(grep -m1 'pwrite of record' "$OUT/rs$rs.ior.err" | sed 's/^rank *//')); $v"; FAILED=$((FAILED+1))
	elif [ "$inval" -gt 0 ]; then
		v="FAIL: $inval page cache invalidation failure(s) on direct I/O in the kernel log; $v"; FAILED=$((FAILED+1))
	fi
	printf 'rs=%-6s WRITEs=%-7s reads/WRITE=%-6s READbios=%-6s dontcache_rounds=%-5s resident=%s%% of %s MiB (meminfo Cached +%s%%) | %s\n' \
		"$rs" "$w" "$rpw" "$rb" "$rounds" "${resident:-?}" "$((wb / 1048576))" "$pct" "$v" | tee -a "$OUT/summary.txt"
done
rm -f $MNT/liob.ior
if [ $FAILED -gt 0 ]; then echo "== $FAILED failure(s); see $OUT/summary.txt" | tee -a "$OUT/summary.txt"; exit 1; fi
echo "== every write succeeded and the invariants held: kept pages do not wake the DONTCACHE kick, no read amplification" | tee -a "$OUT/summary.txt"
