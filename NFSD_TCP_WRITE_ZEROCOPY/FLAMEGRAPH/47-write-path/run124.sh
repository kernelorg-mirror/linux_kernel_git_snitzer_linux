#!/bin/bash
# run124.sh: round 47, NVMe Interrupt Coalescing (feature 8) on the four DPRP5s,
# off against 0x0103 (four entries or 100 us; not saved on the drive), since a
# WRITE's four requests take four hard interrupts on nfsd's CPUs.  Loan64 at the
# settled setting on build 505: untraced A B B A B A; one counted run at B with
# write.bt; lat.sh, O A B B A O, where O is lazytime off (the check owed from
# run122, coalescing off); then 64 KiB writes at iodepth 1, A B B A, where a
# lone request can wait out the 100 us.  Coalescing is put back to 0 on any exit.
HERE=$(dirname "$(readlink -f "$0")")
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r124
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
XK=CA0931345E46C929B11D01F N=3F0C341E86E266D5FDF2ADF K=666175BED7A343E376C1742
PP=/sys/module/page_pool/parameters RG=/sys/kernel/debug/page_pool_rx_region
declare -A STK=([S]=64)
say() { echo "$(date +%T) $*"; }
reg() { awk -v k=$1 '$1 == k {print $2}' $RG 2>/dev/null; }
die() {
	say "STOPPED: $*"; unset EXP_RX_REGION_STACK EXP_LAZYTIME
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] && ./settled.sh apply > $O/apply-stopped.txt 2>&1
	say "after the stop: rx_region_stack $(cat $PP/rx_region_stack), $(tail -1 $O/apply-stopped.txt 2>/dev/null)"
	exit 1
}
load() {	# load TAG STACK: the settled setting with the stack at STACK, checked
	client_free || die "client not free"
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || die "an NFS client is connected"
	[ "$(cat /sys/module/xeu/srcversion)" = $XK ] || die "$1: xeu $(cat /sys/module/xeu/srcversion), not $XK"
	[ "$(cat /sys/module/sunrpc/srcversion)" = $K ] || die "$1: sunrpc $(cat /sys/module/sunrpc/srcversion), not $K"
	[ "$(cat /sys/module/nfsd/srcversion)" = $N ] || die "$1: nfsd $(cat /sys/module/nfsd/srcversion), not $N"
	systemctl is-active -q nfs-server || die "$1: nfs-server is not running"
	unset EXP_RX_REGION_STACK
	./settled.sh apply > $O/apply-$1.txt 2>&1 || die "$1 not settled: $(grep -E 'BAD|FAILED' $O/apply-$1.txt | head -3)"
	[ "$(cat $PP/rx_region_stack)" = $2 ] || die "$1: rx_region_stack $(cat $PP/rx_region_stack), not $2"
	[ "$(reg pages)" = 32768 ] || die "$1: the region is $(reg pages) pages, not 32768"
	[ "$(dmesg | grep -c 'WARNING:')" = $W0 ] || die "$1: a new WARNING in dmesg: $(dmesg | grep -A3 'WARNING:' | tail -4)"
	for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	client_free || die "client not free"
}
run() {	# run TAG WRITES STACK: one run, copy when WRITES is 0
	local tag=$1 w=$2 s=$3 rc f0 f1 lw=
	[ $w -gt 0 ] && lw="WRITES=$w"
	load $tag $s
	f0=$(reg alloc_fail)
	( timed_load $O/$tag 10 || exit 1
	  [ -n "$BT" ] && exec bpftrace $HERE/write.bt > $O/$tag-write.txt 2>&1
	  for i in $(seq 50); do reg free; sleep 0.2; done > $O/$tag-free.txt ) &
	env FS_A="$FS" FS_B= WARM=10 RUNTIME=30 TSV=$O/results.tsv TAG=$tag $lw ./twoport.sh $O/$tag a > $O/$tag.log 2>&1; rc=$?
	wait
	[ $rc = 0 ] || die "$tag: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$tag.log | head -3)"
	f1=$(reg alloc_fail)
	say "$tag | $(grep '^total' $O/$tag.log) | $(grep -c 'no drive stepped down' $O/$tag.log) no-step | $(grep -c 'placed [0-9]* flows' $O/$tag/geometry.txt) placed | $(./cost.sh $O/$tag) | peak $((32768 - $(sort -n $O/$tag/region-min-free.txt | head -1))) | mean in use $(awk '{s += 32768 - $1; n++} END {printf "%.0f", n ? s / n : -1}' $O/$tag-free.txt 2>/dev/null) | coalescing $(cval) | alloc_fail $((f1 - f0)) | $(grep -o 'admitted [0-9]*, denied [0-9]*' $O/$tag/summary.txt || echo copy) | $(grep -o 'client retransmits.*' $O/$tag/window.txt) | stack $(cat $PP/rx_region_stack)"
}

grep -qw 'rodata=noalias' /proc/cmdline || die "not booted with rodata=noalias"
[ -e $PP/rx_region_stack ] || die "no page_pool.rx_region_stack in this kernel"
W0=$(dmesg | grep -c 'WARNING:')
say "kernel $(uname -r), rodata=noalias, sunrpc $K, nfsd $N, xeu $(cat /sys/module/xeu/srcversion), region $(reg pages)"
ctrl() { for d in 4 5 6 7; do nvme set-feature /dev/nvme$d -f 8 -V $1 > /dev/null || die "set-feature nvme$d $1"; done; }
cval() { nvme get-feature /dev/nvme4 -f 8 | sed -n 's/.*Current value: *//p'; }
trap 'for d in 4 5 6 7; do nvme set-feature /dev/nvme$d -f 8 -V 0 > /dev/null; done' EXIT
declare -A CO=([A]=0 [B]=0x0103)
BT=
i=0
for a in A B B A B A; do i=$((i + 1)); ctrl ${CO[$a]}; run r124-$i-loan64-$a 64 64; done
ctrl ${CO[B]}; BT=1; run r124-7-counted-B 64 64; BT=
i=0
for a in O A B B A O; do i=$((i + 1))
	if [ $a = O ]; then ctrl 0; export EXP_LAZYTIME=nolazytime; else ctrl ${CO[$a]}; unset EXP_LAZYTIME; fi
	./lat.sh $O l$i-$a > $O/l$i-$a.out 2>&1 || die "lat l$i-$a: $(tail -2 $O/l$i-$a.out)"
	say "l$i-$a coalescing $(cval), $(grep -m1 -E 'lazytime' <(findmnt -rn -o OPTIONS /dev/nvme4n1) | grep -o 'lazytime' || echo nolazytime): $(cat $O/l$i-$a-u.lat)"
done
unset EXP_LAZYTIME
./settled.sh apply > $O/apply-lat.txt 2>&1 || die "not settled after lat: $(grep -E 'BAD|FAILED' $O/apply-lat.txt | head -3)"
i=0
for a in A B B A; do i=$((i + 1)); ctrl ${CO[$a]}
	t=s$i-$a
	for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 60; client_free || die "client not free"
	env FS_A="$FS" FS_B= WARM=10 RUNTIME=30 JOBS=4 IODEPTH=1 BS=64K TSV=$O/results.tsv RWTSV=$O/lat.tsv TAG=$t WRITES=64 ./twoport.sh $O/$t a > $O/$t.log 2>&1 || die "$t: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$t.log | head -3)"
	say "$t coalescing $(cval): $(python3 -I -c "
import json,sys
j=json.load(open('$O/$t/fio-a.json')); w=j['jobs'][0]['write']; c=w['clat_ns']; p=c['percentile']
print('%.2f GB/s, %.0f IOPS; ms p50 %.3f p90 %.3f p99 %.3f p99.9 %.3f mean %.3f' % (w['bw_bytes']/1e9,w['iops'],p['50.000000']/1e6,p['90.000000']/1e6,p['99.000000']/1e6,p['99.900000']/1e6,c['mean']/1e6))")"
done
ctrl 0
unset EXP_RX_REGION_STACK
./settled.sh apply > $O/apply-end.txt 2>&1 || die "not settled at the end: $(grep -E 'BAD|FAILED' $O/apply-end.txt | head -3)"
[ "$(dmesg | grep -c 'WARNING:')" = $W0 ] || die "a new WARNING in dmesg"
say "ALL end: rx_region_stack $(cat $PP/rx_region_stack), $(tail -1 $O/apply-end.txt); DONE"
