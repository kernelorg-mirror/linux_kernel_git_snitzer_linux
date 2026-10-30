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
O=$L/t2-r113-resize
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
DIAG=/tmp/claude-0/-home-serviceadmin-snitm-kernel-linux/b565c22f-4f2e-47c6-9d6c-f73fc2c0b4a9/scratchpad/xeu45held/kmod-xsight-2.0.1/xeu-esdk-v2.0.1/xeu.ko
XD=7CFF720BA3CE35894DD922E XK=CA0931345E46C929B11D01F N=3F0C341E86E266D5FDF2ADF K=DDE22984B5905541BB2CD35
P=/sys/module/xeu/parameters PA=enp161s0f0
PP=/sys/module/page_pool/parameters RG=/sys/kernel/debug/page_pool_rx_region
BS=/sys/module/sunrpc/parameters/svc_tcp_rx_budget_stats
say() { echo "$(date +%T) $*"; }
reg() { awk -v k=$1 '$1 == k {print $2}' $RG 2>/dev/null; }
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
W0=$(dmesg | grep -c 'WARNING:')
for r in 16384 32768; do resize $r; cat $RG; done
say "escaped $(reg escaped), WARNINGs new: $(( $(dmesg | grep -c 'WARNING:') - W0 ))"
dmesg | grep 'page_pool:' | tail -4
