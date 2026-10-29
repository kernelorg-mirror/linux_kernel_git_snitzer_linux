#!/bin/bash
# run110.sh: round 45's step 1, what holds the receive region's pages.  The
# diagnostic xeu (xeu-diag-region-held.patch on round 41's kept xeu,
# 7CFF720BA3CE35894DD922E) reads, for each receive ring, the buffers posted
# to the card and the pages its page pool keeps for reuse (alloc cache and
# region stack).  From 10 s into each timed load, every 0.2 s for 10 s:
# the region's free pages, the rings' and pools' figures, the NFS sockets'
# receive and backlog bytes (ss -tm), and the budget's charged credit.  At
# rest first, then four runs at the settled setting (with round 44's NAPI
# deferral), counted and untraced, WARM=10 RUNTIME=30:
#   r110-1  copy, the region at 32768 pages (512 MiB)
#   r110-2  loan64, 32768
#   r110-3  loan, 4096 pages (64 MiB), budget 3032 (23 WRITEs), as run104
#   r110-4  loan, 2048 pages (32 MiB), budget 720, as run109
# then the region at 32768 again, the kept xeu and the settled setting.
# Each xeu swap and region resize refuses to run with an NFS client
# connected.  As run on 29 October the samples summed every xeu port's rings,
# port b's eight idle rings of 4096 entries (outside the region) among them;
# r110/held-a.sh takes those 32768 posted buffers off, and the sample now
# reads port a's alone.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r110
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
DIAG=/tmp/claude-0/-home-serviceadmin-snitm-kernel-linux/b565c22f-4f2e-47c6-9d6c-f73fc2c0b4a9/scratchpad/xeu45held/kmod-xsight-2.0.1/xeu-esdk-v2.0.1/xeu.ko
XD=7CFF720BA3CE35894DD922E XK=CA0931345E46C929B11D01F N=3F0C341E86E266D5FDF2ADF K=DDE22984B5905541BB2CD35
P=/sys/module/xeu/parameters PA=enp161s0f0
PP=/sys/module/page_pool/parameters RG=/sys/kernel/debug/page_pool_rx_region
BS=/sys/module/sunrpc/parameters/svc_tcp_rx_budget_stats
say() { echo "$(date +%T) $*"; }
reg() { awk -v k=$1 '$1 == k {print $2}' $RG 2>/dev/null; }
xeu_load() {	# xeu_load [ko]: reload xeu, set port a up again, settle
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || { say "STOPPED: an NFS client is connected"; exit 1; }
	rmmod xeu || return 1
	if [ -n "${1:-}" ]; then insmod $1 $(awk '$1 == "options" && $2 == "xeu" {for (i = 3; i <= NF; i++) print $i}' /etc/modprobe.d/xeu.conf); else modprobe xeu; fi || return 1
	sleep 8; ./port-setup.sh $PA 0 > $O/port-setup-$(cat /sys/module/xeu/srcversion).txt 2>&1
	./settled.sh apply > $O/apply-$(cat /sys/module/xeu/srcversion).txt 2>&1
}
die() {
	say "STOPPED: $*"
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || exit 1
	[ "$(reg pages)" = 32768 ] || { echo 32768 > $PP/rx_region_pages; ip link set $PA down; sleep 10; ip link set $PA up; sleep 6; }
	unset EXP_REGION_PAGES
	[ "$(cat /sys/module/xeu/srcversion)" = $XK ] || xeu_load
	./settled.sh apply > $O/apply-stopped.txt 2>&1; say "after the stop: region $(reg pages), xeu $(cat /sys/module/xeu/srcversion), $(tail -1 $O/apply-stopped.txt)"
	exit 1
}
resize() {	# resize PAGES: the region at PAGES, port a back up and settled
	local r=$1 t
	[ "$(reg pages)" = $r ] && return 0
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || die "resize to $r: an NFS client is connected"
	client_free || die "resize to $r: client not free"
	echo $r > $PP/rx_region_pages
	( sleep 300; ip link set $PA up ) > /dev/null 2>&1 & local dm=$!
	ip link set $PA down
	for t in $(seq 120); do [ -e $RG ] || break; sleep 1; done
	[ -e $RG ] && { ip link set $PA up; kill $dm; die "resize to $r: the old region was not given back in 120 s: $(tr '\n' ' ' < $RG)"; }
	ip link set $PA up; sleep 6; kill $dm 2>/dev/null
	./port-setup.sh $PA 0 > $O/port-setup-$r.txt 2>&1
	export EXP_REGION_PAGES=$r
	./settled.sh apply > $O/apply-region-$r.txt 2>&1 || die "region $r not settled: $(grep -E 'BAD|FAILED' $O/apply-region-$r.txt | head -3)"
	[ "$(reg pages)" = $r ] || die "region $r not made: $(dmesg | grep 'page_pool:' | tail -2)"
	say "region $r pages ($((r / 64)) MiB) made at $(reg phys_base); $(dmesg | grep 'rx region for .* freed' | tail -1 | sed 's/^\[[^]]*\] //')"
}
sample() {	# one line: the region, the rings and pools, the sockets, the budget
	local t; t=$(date +%s%N)
	echo "t=$(( t / 1000000 )) free=$(reg free) $(awk -v d=$PA '$1 == d {p += $4; s += $6; c += $8; k += $10; z += $12} END {printf "posted=%d split=%d cache=%d stack=%d stack_size=%d", p, s, c, k, z}' $P/region_held)" \
	     "$(ss -tmnH state established '( sport = :2049 )' | grep -o 'skmem:([^)]*)' | awk -F'[(,)]' '{n++; for (i = 2; i < NF; i++) {k = $i; sub(/[0-9]+$/, "", k); v = substr($i, length(k) + 1); if (k == "r") r += v; if (k == "bl") b += v}} END {printf "socks=%d rmem=%d bl=%d", n, r, b}')" \
	     "$(awk '{for (i = 1; i < NF; i += 2) if ($i ~ /^(budget|avail|reserve|inflight)$/) printf "%s=%s ", $i, $(i + 1)}' $BS)"
}
load() {	# load TAG: the settled setting at this size, checked
	client_free || die "client not free"
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || die "an NFS client is connected"
	[ "$(cat /sys/module/xeu/srcversion)" = $XD ] || die "$1: xeu $(cat /sys/module/xeu/srcversion), not $XD"
	[ "$(cat /sys/module/sunrpc/srcversion)" = $K ] || die "$1: sunrpc $(cat /sys/module/sunrpc/srcversion), not $K"
	[ "$(cat /sys/module/nfsd/srcversion)" = $N ] || die "$1: nfsd $(cat /sys/module/nfsd/srcversion), not $N"
	systemctl is-active -q nfs-server || die "$1: nfs-server is not running"
	./settled.sh apply > $O/apply-$1.txt 2>&1 || die "$1 not settled: $(grep -E 'BAD|FAILED' $O/apply-$1.txt | head -3)"
	[ "$(dmesg | grep -c 'WARNING:')" = $W0 ] || die "$1: a new WARNING in dmesg: $(dmesg | grep -A3 'WARNING:' | tail -4)"
	for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	client_free || die "client not free"
}
run() {	# run TAG WRITES [BUDGET_PAGES]: one run, copy when WRITES is 0
	local tag=$1 w=$2 rc f0 f1 lw=
	[ $w -gt 0 ] && lw="WRITES=$w${3:+ BUDGET_PAGES=$3}"
	load $tag
	f0=$(reg alloc_fail)
	( timed_load $O/$tag 10 || exit 1
	  for i in $(seq 50); do sample; sleep 0.2; done > $O/$tag-held.txt ) &
	env FS_A="$FS" FS_B= WARM=10 RUNTIME=30 TSV=$O/results.tsv TAG=$tag $lw ./twoport.sh $O/$tag a > $O/$tag.log 2>&1; rc=$?
	wait
	[ $rc = 0 ] || die "$tag: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$tag.log | head -3)"
	f1=$(reg alloc_fail)
	say "$tag | $(grep '^total' $O/$tag.log) | $(grep -c 'no drive stepped down' $O/$tag.log) no-step | $(grep -c 'placed [0-9]* flows' $O/$tag/geometry.txt) placed | $(./cost.sh $O/$tag) | in use at most $(sed -n 's/^region pages in use at most: //p' $O/$tag/summary.txt) of $(reg pages) | alloc_fail $((f1 - f0)) | $(grep -o 'admitted [0-9]*, denied [0-9]*' $O/$tag/summary.txt || echo copy) | $(grep -o 'client retransmits.*' $O/$tag/window.txt)"
	held $O/$tag-held.txt | tee $O/$tag-held-sum.txt
}
# The samples in pages: the region's in use; posted buffers at two to a
# 16 KiB page; the pools' cache and stack; the sockets' receive and backlog
# bytes at 16 KiB a page (each 8 KiB buffer is charged as its truesize, so
# this is a page count only as near as the skbs' own overhead); the budget's
# charged credit (budget - avail - reserve); and what is left.  The mean of
# the samples, and the sample with the most in use.
held() {
	awk -v pages=$(reg pages) '{for (i = 1; i <= NF; i++) {split($i, kv, "="); v[kv[1]] = kv[2]}
		u = pages - v["free"]; ring = v["posted"] / 2; pool = v["cache"] + v["stack"]; q = (v["rmem"] + v["bl"]) / 16384
		ch = v["budget"] ? v["budget"] - v["avail"] - v["reserve"] : 0; rest = u - ring - pool - q - ch
		n++; su += u; sr += ring; sp += pool; sc += v["cache"]; ss += v["stack"]; sq += q; sch += ch; srest += rest; sinf += v["inflight"]
		if (u > mu) {mu = u; mr = ring; mp = pool; mc = v["cache"]; mk = v["stack"]; mq = q; mch = ch; mrest = rest; minf = v["inflight"]}}
		END {if (!n) {print "no samples"; exit}
		printf "%-10s %7s %7s %7s %7s %7s %7s %7s %8s %9s\n", "", "in use", "posted", "pools", "(cache", "stack)", "queues", "charged", "the rest", "inflight"
		printf "%-10s %7.0f %7.0f %7.0f %7.0f %7.0f %7.0f %7.0f %8.0f %9.1f\n", "mean", su / n, sr / n, sp / n, sc / n, ss / n, sq / n, sch / n, srest / n, sinf / n
		printf "%-10s %7.0f %7.0f %7.0f %7.0f %7.0f %7.0f %7.0f %8.0f %9.0f\n", "most", mu, mr, mp, mc, mk, mq, mch, mrest, minf
		printf "(%d samples; pages)\n", n}' $1
}

grep -qw 'rodata=noalias' /proc/cmdline || die "not booted with rodata=noalias"
[ "$(reg pages)" = 32768 ] || die "the region is $(reg pages) pages, not 32768"
[ "$(cat $PP/rx_region_dev)" = 0000:a1:00.0 ] || die "region devices $(cat $PP/rx_region_dev), not port a's alone"
[ "$(cat /sys/module/xeu/srcversion)" = $XK ] || die "xeu is not the kept $XK"
[ "$(modinfo -F srcversion $DIAG)" = $XD ] || die "$DIAG is not $XD"
W0=$(dmesg | grep -c 'WARNING:')
client_free || die "client not free"
xeu_load $DIAG || die "diagnostic xeu not loaded or not settled: $(grep -E 'BAD|FAILED' $O/apply-$XD.txt | head -3)"
[ "$(cat /sys/module/xeu/srcversion)" = $XD ] || die "xeu is $(cat /sys/module/xeu/srcversion), not $XD"
[ "$(reg pages)" = 32768 ] || die "the region is $(reg pages) pages, not 32768"
say "kernel $(uname -r), rodata=noalias, sunrpc $K, nfsd $N, xeu $XD, region $(reg pages), $(tail -1 $O/apply-$XD.txt)"
for i in 1 2 3 4 5; do sample; sleep 0.2; done > $O/rest-held.txt
say "at rest:"; held $O/rest-held.txt | tee $O/rest-held-sum.txt; cat $P/region_held
run r110-1-copy 0
run r110-2-loan64 64
resize 4096
run r110-3-region4096-loan23 23
resize 2048
run r110-4-region2048-loan-b720 5 720
grep -q 'budget 720 pages' $O/r110-4-region2048-loan-b720/geometry.txt || die "the budget was not 720 pages"
resize 32768
unset EXP_REGION_PAGES
xeu_load || die "the kept xeu not loaded or not settled"
[ "$(cat /sys/module/xeu/srcversion)" = $XK ] || die "xeu is $(cat /sys/module/xeu/srcversion), not $XK"
./settled.sh apply > $O/apply-end.txt 2>&1 || die "not settled at the end: $(grep -E 'BAD|FAILED' $O/apply-end.txt | head -3)"
[ "$(dmesg | grep -c 'WARNING:')" = $W0 ] || die "a new WARNING in dmesg"
say "end: region $(reg pages) pages, xeu $(cat /sys/module/xeu/srcversion), $(tail -1 $O/apply-end.txt); DONE"
