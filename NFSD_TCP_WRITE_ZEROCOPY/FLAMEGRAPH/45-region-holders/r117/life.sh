#!/bin/bash
# life.sh TAG: round 45's step 5, the region's life on build 505: port a down
# and up, then a resize to 16384 and back, as r113/resize.sh.  Run before any
# load and again after the loads: the bulk return runs only under load, so
# only the second says anything about what it leaves behind.  The slot's
# pages unmapped and the slot given up at each step, escaped 0, no WARNING.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
T=${1:?tag}
O=$L/t2-r117-life-$T
mkdir -p $O
PA=enp161s0f0
PP=/sys/module/page_pool/parameters RG=/sys/kernel/debug/page_pool_rx_region
say() { echo "$(date +%T) $*"; }
reg() { awk -v k=$1 '$1 == k {print $2}' $RG 2>/dev/null; }
die() {
	say "STOPPED: $*"
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || exit 1
	[ "$(reg pages)" = 32768 ] || { echo 32768 > $PP/rx_region_pages; ip link set $PA down; sleep 10; ip link set $PA up; sleep 6; }
	unset EXP_REGION_PAGES
	./port-setup.sh $PA 0 > $O/port-setup-stopped.txt 2>&1
	./settled.sh apply > $O/apply-stopped.txt 2>&1; say "after the stop: region $(reg pages), $(tail -1 $O/apply-stopped.txt)"
	exit 1
}
free_of_clients() {
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || die "$1: an NFS client is connected"
	client_free || die "$1: client not free"
}
downup() {	# the port down and up with the region kept: the slot goes and comes back
	local t
	free_of_clients downup
	( sleep 300; ip link set $PA up ) > /dev/null 2>&1 & local dm=$!
	say "before: $(tr '\n' ' ' < $RG)"
	ip link set $PA down
	for t in $(seq 60); do grep -q '^mapped' $RG 2>/dev/null || break; sleep 1; done
	cp $RG $O/down.txt 2>/dev/null
	say "down after ${t} s: $(tr '\n' ' ' < $RG)"
	[ "$(reg free_unmapped)" = 32768 ] || { ip link set $PA up; kill $dm; die "down: free_unmapped $(reg free_unmapped), not 32768"; }
	[ "$(reg escaped)" = 0 ] || { ip link set $PA up; kill $dm; die "down: escaped $(reg escaped)"; }
	grep -q '^mapped' $RG && { ip link set $PA up; kill $dm; die "down: the slot is still there"; }
	ip link set $PA up; sleep 6; kill $dm 2>/dev/null
	./port-setup.sh $PA 0 > $O/port-setup-downup.txt 2>&1
	./settled.sh apply > $O/apply-downup.txt 2>&1 || die "not settled after the port came up: $(grep -E 'BAD|FAILED' $O/apply-downup.txt | head -3)"
	grep -q '^mapped 0000:a1:00.0 dir 2 pools 4' $RG || die "up: no mapped line: $(tr '\n' ' ' < $RG)"
	say "up: $(tr '\n' ' ' < $RG)"
}
resize() {	# resize PAGES: the region at PAGES, port a back up and settled
	local r=$1 t
	[ "$(reg pages)" = $r ] && return 0
	free_of_clients "resize to $r"
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
W0=$(dmesg | grep -c 'WARNING:')
say "life $T: kernel $(uname -r)"
downup
for r in 16384 32768; do resize $r; say "$(tr '\n' ' ' < $RG)"; done
unset EXP_REGION_PAGES
./settled.sh apply > $O/apply-end.txt 2>&1 || die "not settled at the end: $(grep -E 'BAD|FAILED' $O/apply-end.txt | head -3)"
[ "$(dmesg | grep -c 'WARNING:')" = $W0 ] || die "a new WARNING in dmesg: $(dmesg | grep -A3 'WARNING:' | tail -4)"
say "end: $(tail -1 $O/apply-end.txt); no WARNING; DONE"
