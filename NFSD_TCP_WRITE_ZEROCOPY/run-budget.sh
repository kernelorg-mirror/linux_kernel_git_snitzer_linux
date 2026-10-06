#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# run-budget.sh - runtime qualification of region budget admission (Slipstream
# patch 18) with the fixed receive region (patch 16), on one host.
#
# Adapted from David Flynn's Slipstream stress campaign (tools/stress.sh,
# rig.sh, veth.sh in slipstream/slipstream-kernel-overlays-hs-*/): same cells,
# counters and acceptance criteria, but a self-contained rig that touches
# nothing else on the host -- no /etc/exports or nfs.conf edits, no
# exportfs -ua, no brd, its own nvmet port and names:
#
#   null_blk "zcbudget" (memory backed) -> dm-delay "zcbudget" (storage
#   latency, reloadable) -> nvmet subsystem "zc-budget" on a free port ->
#   /dev/nvmeXnY -> XFS /export/zcbudget, exported with exportfs -o to
#   10.77.0.0/24.
#
#   STORAGE picks the NVMe transport, and with it which kind of storage the
#   export stands for:
#     STORAGE=prp (default)  nvme-loop.  Like PRP-only NVMe it needs whole
#                            pages (virt_boundary_mask 4095), so a loaned
#                            WRITE cannot go direct.
#     STORAGE=sgl            NVMe/TCP to 127.0.0.1.  Like NVMe with SGLs it
#                            takes arbitrary pieces (virt_boundary_mask 0),
#                            so a loaned WRITE goes direct.
#   Results go to OUTDIR/stress-<PHASE> either way: give each STORAGE its
#   own OUTDIR.  rig.txt there records which it was.
#
#   Cell names: "-on" loans (budget set, receive buffer held); "-copy" is
#   its twin that copies (budget 0, the same held buffer, everything else
#   the same), so the two can be compared; "-off" has neither budget nor
#   held buffer and shows what happens without the mechanism.  "-sized-"
#   cells write files that already have their size.
#
#   PREALLOC=1 gives every cell's files their size before the client writes
#   (the "-sized-" cells do it for themselves).  XFS makes a direct write
#   that starts beyond the end of a growing file wait for every direct
#   write in progress; this takes that out of a measurement.
#
#   FORCE_DELAY=<ms> and FORCE_RCVBUF=<bytes> override every cell's storage
#   delay and held receive buffer, to compare cells that differ in them.
#
#   RX_IP_ALIGN=<0-3> is veth's rx_ip_align: extra bytes of headroom for the
#   frames veth copies into page-pool pages.  At 0 the TCP payload is 2 bytes
#   off a 4-byte boundary, as on a NIC that posts its buffers the usual way,
#   and NVMe refuses loaned WRITEs as direct I/O for that alone.  2 puts it
#   on the boundary.  Default: 2 with STORAGE=sgl, else 0.  veth is reloaded
#   to change it, so no other veth device may exist; a veth without the
#   parameter runs at 0 and says so.  Client: network namespace rxcli over veth rxsrv<->rxcli0
#   with XDP_PASS on rxsrv, so received payload lands in page-pool pages
#   (patch 16's region binds those pools).
#
# Needs: veth, dm-delay and null_blk (modules), fio, clang (BPF target),
# nfs-server running, LOCALIO off for the run (set and restored here);
# nvmet-tcp and nvme-tcp for STORAGE=sgl.
#
# The fixed region is created once per boot, at the first matching pool, and
# never freed: one boot tests one region size.  Phase A (default) uses a
# 4096-page region on 4 KiB (16 MiB), scaled on larger pages (see "page
# geometry"); run phase B (region at the configured minimum) on a fresh boot.
#
# Usage: run-budget.sh [OUTDIR]        (PHASE=A)
#        PHASE=TLP run-budget.sh [OUTDIR]
#        PARKED=<phase A parked.txt> PHASE=B run-budget.sh [OUTDIR]
#        PHASE=X run-budget.sh [OUTDIR]
#        PHASE=D run-budget.sh [OUTDIR]
#        PHASE=N run-budget.sh [OUTDIR]
#   PHASE=N: per-namespace budgets.  A second nfsd runs in network namespace
#   zcsrvB with its own mount namespace (tmpfs /var/lib/nfs, its own exports,
#   its own nfsd fs; NFSv4.2 only), reached from client namespace rxcl3 over
#   veth rxsrv3 (XDP, in the region's device list).  Cells: B on its own
#   budget, on the default budget, copying at 0, beside the initial
#   namespace's client on separate budgets, and the sum check.
#   PHASE=D: two devices sharing the region (page_pool.rx_region_dev is a
#   list): as PHASE=X, but the region is bound to rxsrv,rxsrv2 and rxsrv2
#   has XDP too, so both clients are region clients.  Fresh boot: the first
#   cell creates the region, bound to the list.
#   PHASE=X: the budget's scope -- a region client (rxsrv, XDP, region
#   pages) and a non-region client (rxsrv2 in netns rxcl2, no XDP, never
#   region pages) at once, no budget, on and tight; results in xcells.tsv.
#   PHASE=B: on a fresh boot, before anything creates the region; the region
#   is sized to the configured minimum (parked + 4 x 128 KiB receive buffers
#   + one record's reserve + 64), as in David's stress.sh, with the edge
#   cells at and below it.
#   PHASE=TLP: the spurious tail-loss-probe A/B on the cells with a budget --
#   baseline, server per-route quickack, client TLP off, and rcvbuf/clamp
#   without budget admission (same rig, region and warm-up as phase A).
# Results: OUTDIR/stress-A/cells.tsv, per-cell dirs, campaign.log.  The
# last three columns of cells.tsv say how nfsd issued the cell's WRITEs,
# in segments: wr_direct (O_DIRECT), wr_dontcache, wr_buffered.
set -Eeuo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
PHASE=${PHASE:-A}
STORAGE=${STORAGE:-prp}
case $STORAGE in prp|sgl) ;; *) echo "STORAGE=$STORAGE: prp or sgl" >&2; exit 2 ;; esac
[ $STORAGE = sgl ] && RX_IP_ALIGN=${RX_IP_ALIGN:-2}
RX_IP_ALIGN=${RX_IP_ALIGN:-0}
case $RX_IP_ALIGN in 0|1|2|3) ;; *) echo "RX_IP_ALIGN=$RX_IP_ALIGN: 0 to 3" >&2; exit 2 ;; esac

# --- page geometry ---------------------------------------------------------
# Every page budget here was first sized on 4 KiB pages.  They are derived
# from the running kernel's page size with the kernel's own admission
# formula, so on 4 KiB they are exactly the qualified constants (region 4096,
# reserve 390 / 720, receive buffers 128 / 64 / 32 pages, factor 150 / 280)
# and on 16 or 64 KiB each cell tests the same thing: the same number of
# records per budget, the same receive-buffer share.  GEOMETRY=1 prints the
# derived values and exits (GEOMETRY_PS=<bytes> for another page size).
PS=$(getconf PAGESIZE)
# GEOMETRY=1 GEOMETRY_PS=16384: print what another page size would derive.
[ -n "${GEOMETRY:-}" ] && [ -n "${GEOMETRY_PS:-}" ] && PS=$GEOMETRY_PS
MIB_PAGES=$(( (1048576 + PS - 1) / PS ))
# Admission estimate for a 1 MiB record (svc_tcp_rx_credit_cost()).
rec_cost() { echo $(( (MIB_PAGES * $1 + 99) / 100 + 1 )); }
# One record's reserve: its cost rounded up to 10 pages (4 KiB: 385 -> 390).
rec_reserve() { local c; c=$(rec_cost "$1"); echo $(( (c + 9) / 10 * 10 )); }
# Region pages per 100 payload pages for an MTU: whole pages per frame when
# a frame takes more than half a page (page_pool_alloc_netmem()), frames
# packed into a page otherwise; never below 150.  4 KiB keeps the measured
# values.  Provisional on 16 / 64 KiB: paused / overrun in cells with a budget
# say whether it is low.
factor() {
	local mtu=$1 fb pay frames per pages f
	case "$PS:$mtu" in 4096:9000) echo 150; return ;; 4096:1500) echo 280; return ;; esac
	pay=$(( mtu - 52 )); frames=$(( (1048576 + pay - 1) / pay )); fb=$(( mtu + 576 ))
	if [ $(( fb * 2 )) -gt "$PS" ]; then
		pages=$(( frames * ((fb + PS - 1) / PS) ))
	else
		per=$(( PS / fb )); pages=$(( (frames + per - 1) / per ))
	fi
	f=$(( (100 * pages + MIB_PAGES - 1) / MIB_PAGES )); f=$(( (f + 9) / 10 * 10 ))
	[ "$f" -lt 150 ] && f=150
	echo "$f"
}
# Region pages a locked receive buffer of $1 bytes can pin at MTU $2: 2 KiB of
# payload per page (4 KiB) or one page per frame, whichever is more pages.
rcvpages() {
	local a=$(( $1 * 2 / PS )) fr=$(( $2 > PS / 2 ? $2 : PS / 2 )) b
	b=$(( ($1 + fr - 1) / fr ))
	echo $(( a > b ? a : b ))
}
F9=$(factor 9000); F15=$(factor 1500)
R9=$(rec_reserve "$F9"); R15=$(rec_reserve "$F15")
RB256=$(rcvpages 262144 9000); RB128=$(rcvpages 131072 9000); RB64=$(rcvpages 65536 9000)
RB256_15=$(rcvpages 262144 1500)
# The region holds the parked pages (about 310, a page count) plus the same
# number of records as 4096 pages hold on 4 KiB (3786 pages at 385 each).
REGION_DEFAULT=$(( (4096 - 310) * $(rec_cost "$F9") / 385 + 310 ))
RMEMS="$PS $(( 8 * PS )) $(( 16 * PS ))"
# Phase N budgets were written for a 4096-page region: scale to the region.
nf() { echo $(( $1 * SIZE / 4096 )); }
geometry() {
	echo "geometry: page $PS, 1 MiB = $MIB_PAGES pages; factor $F9 (MTU 9000) $F15 (MTU 1500);" \
	     "record cost $(rec_cost "$F9") / $(rec_cost "$F15"), reserve $R9 / $R15;" \
	     "rcvbuf pages 256K $RB256 128K $RB128 64K $RB64 (256K at MTU 1500 $RB256_15);" \
	     "region default $REGION_DEFAULT; collapse tcp_rmem $RMEMS"
}
if [ -n "${GEOMETRY:-}" ]; then geometry; exit 0; fi
EV=${1:-/var/tmp/budget-$(date +%Y%m%d-%H%M%S)}
E=$EV/stress-$PHASE
mkdir -p "$E"
exec > >(tee -a "$E/campaign.log") 2>&1

SP=/sys/module/sunrpc/parameters
PP=/sys/module/page_pool/parameters
RG=/sys/kernel/debug/page_pool_rx_region
CFG=/sys/kernel/config
T=/sys/kernel/tracing
NQN=zc-budget
NVME_TCP_PORT=${NVME_TCP_PORT:-4477}
NB=zcbudget
DM=zcbudget
EXPORT=/export/zcbudget
FSID=7712
XDPO=$EV/xdp_pass.o
XDPC=${XDPC:-$(ls "$HERE"/slipstream/tools/xdp_pass.c "$HERE"/slipstream/slipstream-kernel-overlays-hs-*/tools/xdp_pass.c 2>/dev/null | head -1 || true)}

# --- save everything this script changes, restore on any exit ------------
# On a fresh boot nothing has loaded these yet; their settings are saved below.
modprobe nfsd; modprobe nfs
SAVE_RMEM=$(sysctl -n net.ipv4.tcp_rmem)
SAVE_DEFER=$(sysctl -n net.core.skb_defer_max)
SAVE_LIO=$(cat /sys/module/nfs/parameters/localio_enabled 2>/dev/null || echo Y)
SAVE_ICW=$(cat /sys/kernel/debug/nfsd/io_cache_write)
SAVE_TON=$(cat $T/tracing_on)
SAVE_REGDEV=$(cat $PP/rx_region_dev)
SAVE_REGPAGES=$(cat $PP/rx_region_pages)
for p in budget_pages budget_reserve credit_factor rcvbuf; do
	eval "SAVE_$p=\$(cat $SP/svc_tcp_rx_$p)"
done
PORT=""

# firewalld puts a new interface in the default zone, which rejects NFS from
# the client namespace; trust rxsrv at runtime only, for as long as it exists.
fw() { command -v firewall-cmd >/dev/null && firewall-cmd -q --state 2>/dev/null && firewall-cmd -q "$@" 2>/dev/null; }
veth_down() {
	fw --zone=trusted --remove-interface=rxsrv || true
	ip netns del rxcli 2>/dev/null || true
	ip link del rxsrv 2>/dev/null || true
}
# The non-region path for PHASE=X: no XDP, so no page pool and no region.
veth2_down() {
	fw --zone=trusted --remove-interface=rxsrv2 || true
	ip netns del rxcl2 2>/dev/null || true
	ip link del rxsrv2 2>/dev/null || true
}
veth2_up() {
	veth2_down
	ip netns add rxcl2
	ip link add rxsrv2 type veth peer name rxcl2a netns rxcl2
	ip addr add 10.78.0.1/24 dev rxsrv2
	ip link set rxsrv2 up
	fw --zone=trusted --add-interface=rxsrv2 || true
	ip -n rxcl2 addr add 10.78.0.2/24 dev rxcl2a
	ip -n rxcl2 link set lo up
	ip -n rxcl2 link set rxcl2a up
	ip link set rxsrv2 mtu "${MTU:-1500}"
	ip -n rxcl2 link set rxcl2a mtu "${MTU:-1500}"
	# PHASE=D: rxsrv2's pools are created here and bind the shared region.
	[ "${XDP2:-0}" = 1 ] && ip link set dev rxsrv2 xdp obj "$XDPO" sec xdp.frags
	return 0
}
# Client mounts use nolock: a v3 mount in a client namespace starts lockd
# there, which registers nlockmgr in the host's rpcbind (reached by the path
# /run/rpcbind.sock, shared with the host) and unregisters it on unmount,
# taking the host server's registration with it.  No cell takes locks.

# --- PHASE=N: a second nfsd in namespace zcsrvB -------------------------
EB=""
srvb_down() {
	local pid
	if [ -n "$EB" ] && [ -s "$EB/nspid" ] && kill -0 "$(cat "$EB/nspid")" 2>/dev/null; then
		nsenter -t "$(cat "$EB/nspid")" -m -n sh -c 'rpc.nfsd 0; exportfs -ua' 2>/dev/null
	fi
	# Everything left in the namespace is ours: mountd and the holder.
	for pid in $(ip netns pids zcsrvB 2>/dev/null); do kill "$pid" 2>/dev/null; done
	sleep 1
	ip netns del rxcl3 2>/dev/null || true
	ip netns del zcsrvB 2>/dev/null || true
}
srvb_up() {
	srvb_down
	EB=$E/srvB; mkdir -p "$EB/exports.d"
	printf '%s 10.79.0.0/24(rw,sync,no_root_squash,insecure,no_subtree_check,fsid=%s)\n' \
		"$EXPORT" "$FSID" > "$EB/exports"
	ip netns add zcsrvB
	ip netns add rxcl3
	ip link add rxsrv3 type veth peer name rxcl3a
	ip link set rxsrv3 netns zcsrvB
	ip link set rxcl3a netns rxcl3
	ip -n zcsrvB addr add 10.79.0.1/24 dev rxsrv3
	ip -n rxcl3 addr add 10.79.0.2/24 dev rxcl3a
	ip -n zcsrvB link set lo up; ip -n rxcl3 link set lo up
	ip -n zcsrvB link set rxsrv3 mtu "${MTU:-1500}" up
	ip -n rxcl3 link set rxcl3a mtu "${MTU:-1500}" up
	# rxsrv3's pools bind the region: it must be listed, and rx_region_pages
	# set (a pool binds only while it is non-zero), before XDP creates them.
	echo "$REGDEV" > $PP/rx_region_dev
	echo "$SIZE" > $PP/rx_region_pages
	[ -f "$XDPO" ] || clang -O2 -target bpf -c "$XDPC" -o "$XDPO"
	ip -n zcsrvB link set dev rxsrv3 xdp obj "$XDPO" sec xdp.frags
	# nfsd in zcsrvB, with private /run, /var/lib/nfs and exports.  A
	# private /run matters: rpcbind is reached through the path
	# /run/rpcbind.sock, not the network namespace, so without it B's nfsd
	# would register in, and on shutdown unregister NFS from, the host's
	# rpcbind.  NFSv4.2 does not need rpcbind.  The holder keeps the mount
	# namespace (and so its nfsd fs mount) alive.
	ip netns exec zcsrvB unshare -m --propagation private sh -c "
		echo \$\$ > $EB/nspid
		mount -t tmpfs none /run &&
		mount -t tmpfs none /var/lib/nfs &&
		mkdir -p /var/lib/nfs/v4recovery /var/lib/nfs/rpc_pipefs &&
		mount --bind $EB/exports /etc/exports &&
		mount --bind $EB/exports.d /etc/exports.d &&
		mount -t nfsd nfsd /proc/fs/nfsd &&
		exportfs -r && rpc.mountd -N 2 -N 3 &&
		rpc.nfsd -N 3 -G 10 -L 10 8 || exit 1
		exec sleep infinity" > "$EB/srvb.log" 2>&1 &
	local i
	for i in $(seq 30); do
		[ -s "$EB/nspid" ] && ip netns exec zcsrvB ss -tln 2>/dev/null | grep -q ':2049 ' && break
		sleep 1
	done
	ip netns exec zcsrvB ss -tln | grep -q ':2049 ' || { echo "nfsd in zcsrvB did not start:" >&2; cat "$EB/srvb.log" >&2; return 1; }
	nsenter -t "$(cat "$EB/nspid")" -m -n sh -c 'echo Y > /proc/fs/nfsd/v4_end_grace' 2>/dev/null
	echo "srvB: nfsd in zcsrvB, pid $(cat "$EB/nspid"), exports $(nsenter -t "$(cat "$EB/nspid")" -m -n exportfs | tr -s ' \n' ' ')"
}
# B's budget as it stands; all zero once zcsrvB is gone (n-exit).
bstats() {
	ip netns exec zcsrvB sysctl -n net.sunrpc.region_budget_stats 2>/dev/null ||
		echo "budget 0 reserve_size 0 avail 0 reserve 0 inflight 0 waiters 0 admitted 0 denied 0 paused 0 overrun 0 orphaned 0"
}
# set_budgets <init> <default> <B> (pages; one record's reserve where non-zero)
set_budgets() {
	local r
	echo 0 > $SP/svc_tcp_rx_budget_pages; echo 0 > $SP/svc_tcp_rx_budget_default_pages
	ip netns exec zcsrvB sysctl -qw net.sunrpc.region_budget_pages=0
	r=0; [ "$1" != 0 ] && r=$R9; echo $r > $SP/svc_tcp_rx_budget_reserve
	r=0; [ "$2" != 0 ] && r=$R9; echo $r > $SP/svc_tcp_rx_budget_default_reserve
	r=0; [ "$3" != 0 ] && r=$R9; ip netns exec zcsrvB sysctl -qw net.sunrpc.region_budget_reserve=$r
	echo "$1" > $SP/svc_tcp_rx_budget_pages &&
	echo "$2" > $SP/svc_tcp_rx_budget_default_pages &&
	ip netns exec zcsrvB sysctl -qw net.sunrpc.region_budget_pages="$3"
}
# ncell <name> <init> <default> <B> <clients: B or RB>
ncell() {
	local name=$1 O=$E/$1 si0 sd0 sb0 si1 sd1 sb1 pr= pb= hk= rcr=- rcb=- mr=- mb=- nb c w0
	rm -rf "$O"; mkdir -p "$O/R" "$O/B"
	echo "$RCVBUF" > $SP/svc_tcp_rx_rcvbuf
	echo "$F9" > $SP/svc_tcp_rx_credit_factor
	setdelay "$DELAY"
	set_budgets "$2" "$3" "$4" || { echo "$name: budgets $2/$3/$4 refused"; return 0; }
	if [ "$5" = RB ]; then MTU=$MTU veth_up "$SIZE"; fi
	jobfile "$O/R" "$PROF" "$name-R"; jobfile "$O/B" "$PROF" "$name-B"
	[ -w $RG ] && echo reset > $RG
	si0=$(cat $SP/svc_tcp_rx_budget_stats); sd0=$(cat $SP/svc_tcp_rx_budget_default_stats); sb0=$(bstats)
	echo > $T/trace; echo 1 > $T/events/sunrpc/svcsock_tcp_rx_lifetime/enable; echo 1 > $T/tracing_on
	w0=$(dmesg | grep -c 'WARNING' || true)
	set +e
	# HOOK: what to do to B while the load runs (n-reconf, n-exit).
	[ -n "${HOOK:-}" ] && { ( eval "$HOOK" ) > "$O/hook.log" 2>&1 & hk=$!; }
	[ "$5" = RB ] && { xclient rxcli 10.77.0.1 "$O/R" "$name-R" & pr=$!; }
	ip netns exec rxcl3 bash -c "
		mkdir -p /mnt/rxr && mount -t nfs -o vers=4.2,nolock,${BOPT:-hard},proto=tcp,sec=sys,nconnect=$NCONN,rsize=1048576,wsize=1048576 10.79.0.1:$EXPORT /mnt/rxr || exit 90
		timeout ${TMO:-900} fio --output-format=json --output=$O/B/fio.json $O/B/job.fio > $O/B/fio.out 2>&1
		rc=\$?; rm -f /mnt/rxr/f-$name-B* 2>/dev/null; umount /mnt/rxr 2>/dev/null || umount -f -l /mnt/rxr; exit \$rc" & pb=$!
	( sleep 8; ip netns exec zcsrvB ss -tmin '( sport = :2049 )' > "$O/ss-B.txt" 2>&1 ) &
	[ -n "$pr" ] && { wait $pr; rcr=$?; mr=$(fiombps "$O/R/fio.json"); }
	wait $pb; rcb=$?; mb=$(fiombps "$O/B/fio.json")
	[ -n "$hk" ] && wait $hk
	set -e
	echo 0 > $T/events/sunrpc/svcsock_tcp_rx_lifetime/enable
	nb=$(grep -c 'no_budget' $T/trace || true)
	si1=$(cat $SP/svc_tcp_rx_budget_stats); sd1=$(cat $SP/svc_tcp_rx_budget_default_stats); sb1=$(bstats)
	for c in si sd sb; do eval "echo \"\$${c}1\"" > "$O/$c.after"; done
	d() { echo $(( $(kv "$2" $3) - $(kv "$1" $3) )); }
	echo "$name $2 $3 $4 $5 $mr $rcr $mb $rcb $(d "$si0" "$si1" admitted) $(d "$si0" "$si1" denied) $(d "$sd0" "$sd1" admitted) $(d "$sd0" "$sd1" denied) $(d "$sb0" "$sb1" admitted) $(d "$sb0" "$sb1" denied) $nb $(( SIZE - $(reg min_free) )) $(reg escaped)" >> "$E/ncells.tsv"
	tail -1 "$E/ncells.tsv"
	echo "$name end: B [$(bstats)] default [$(cat $SP/svc_tcp_rx_budget_default_stats)] new WARNINGs $(( $(dmesg | grep -c 'WARNING' || true) - w0 ))"
	[ -n "${HOOK:-}" ] && sed "s/^/$name hook: /" "$O/hook.log"
	[ "$5" = RB ] && veth_down
	return 0
}
nrun() { ( export "$@"; ncell "$CELL" "$I" "$DF" "$BB" "$CL" ) || echo "CELL aborted rc=$? ($*)"; }

rig_down() {
	veth_down
	veth2_down
	srvb_down
	exportfs -u "10.77.0.0/24:$EXPORT" 2>/dev/null || true
	exportfs -u "10.78.0.0/24:$EXPORT" 2>/dev/null || true
	mountpoint -q "$EXPORT" && umount "$EXPORT"
	nvme disconnect -n "$NQN" >/dev/null 2>&1 || true
	local l
	for l in "$CFG"/nvmet/ports/*/subsystems/"$NQN"; do
		[ -L "$l" ] || continue
		rm -f "$l"
		rmdir "$(dirname "$(dirname "$l")")" 2>/dev/null || true
	done
	[ -d "$CFG/nvmet/subsystems/$NQN/namespaces/1" ] && { echo 0 > "$CFG/nvmet/subsystems/$NQN/namespaces/1/enable"; rmdir "$CFG/nvmet/subsystems/$NQN/namespaces/1"; }
	rmdir "$CFG/nvmet/subsystems/$NQN" 2>/dev/null || true
	dmsetup remove "$DM" 2>/dev/null || true
	if [ -d "$CFG/nullb/$NB" ]; then
		echo 0 > "$CFG/nullb/$NB/power" 2>/dev/null || true
		rmdir "$CFG/nullb/$NB"
	fi
}
cleanup() {
	set +e
	ip netns exec rxcli umount -f /mnt/rxr 2>/dev/null
	ip netns exec rxcl2 umount -f /mnt/rxr 2>/dev/null
	ip netns exec rxcl3 umount -f /mnt/rxr 2>/dev/null
	rig_down
	[ -n "${SAVE_VETH_ALIGN:-}" ] && { modprobe -r veth 2>/dev/null; veth_load "$SAVE_VETH_ALIGN"; }
	sysctl -qw net.ipv4.tcp_rmem="$SAVE_RMEM"
	sysctl -qw net.core.skb_defer_max="$SAVE_DEFER"
	echo "$SAVE_LIO" > /sys/module/nfs/parameters/localio_enabled
	echo "$SAVE_ICW" > /sys/kernel/debug/nfsd/io_cache_write
	echo 0 > $SP/svc_tcp_rx_budget_pages
	echo 0 > $SP/svc_tcp_rx_budget_default_pages
	echo 0 > $SP/svc_tcp_rx_budget_default_reserve
	for p in budget_reserve credit_factor rcvbuf budget_pages; do
		eval "echo \$SAVE_$p > $SP/svc_tcp_rx_$p"
	done
	echo 0 > $T/events/sunrpc/svcsock_tcp_rx_lifetime/enable
	wr_disarm
	echo "$SAVE_TON" > $T/tracing_on
	# The region object outlives this run (never freed); stop new pools binding it.
	echo -n "$SAVE_REGDEV" > $PP/rx_region_dev
	echo "$SAVE_REGPAGES" > $PP/rx_region_pages
	echo "== restored host settings"
}
trap cleanup EXIT

# --- how nfsd issued the WRITEs ---------------------------------------------
# One histogram trigger per write tracepoint counts segments without filling
# the trace buffer; a cell reports the difference across its run.
WREV="nfsd_write_direct nfsd_write_dontcache nfsd_write_vector"
wr_arm() {
	local e
	for e in $WREV; do
		grep -q '^hist:' $T/events/nfsd/$e/trigger 2>/dev/null ||
			echo 'hist:keys=common_type' > $T/events/nfsd/$e/trigger 2>/dev/null || true
	done
}
wr_disarm() {
	local e
	for e in $WREV; do echo '!hist:keys=common_type' > $T/events/nfsd/$e/trigger 2>/dev/null || true; done
}
wr_hits() {	# -> "direct dontcache buffered"
	local e o=
	for e in $WREV; do
		o="$o $(awk '/^ *Hits:/ {print $2; f = 1} END {if (!f) print 0}' $T/events/nfsd/$e/hist 2>/dev/null || echo 0)"
	done
	echo $o
}

# --- veth's receive placement ------------------------------------------------
# rx_ip_align is a load-time parameter: reload veth to change it.
VETH_ALIGN=/sys/module/veth/parameters/rx_ip_align
veth_load() {	# veth_load <align>
	modprobe -q veth rx_ip_align="$1" 2>/dev/null && return 0
	[ -f "$HERE/../drivers/net/veth.ko" ] && insmod "$HERE/../drivers/net/veth.ko" rx_ip_align="$1"
}
veth_align() {	# veth_align <align>: make veth run with it, or say why not
	local want=$1 cur
	[ -d /sys/module/veth ] || veth_load "$want" 2>/dev/null || modprobe -q veth || true
	if [ ! -r $VETH_ALIGN ]; then
		[ "$want" = 0 ] || echo "!! this veth has no rx_ip_align: running at 0, not $want" >&2
		RX_IP_ALIGN=0; return 0
	fi
	cur=$(cat $VETH_ALIGN)
	[ "$cur" = "$want" ] && return 0
	if ip -o link show type veth | grep -q .; then
		echo "veth rx_ip_align is $cur, want $want, and veth devices exist: cannot reload veth" >&2; exit 2
	fi
	SAVE_VETH_ALIGN=${SAVE_VETH_ALIGN:-$cur}
	modprobe -r veth && veth_load "$want" || { echo "cannot reload veth with rx_ip_align=$want" >&2; exit 2; }
}

# --- rig ---------------------------------------------------------------
rig_up() {
	rig_down
	veth_align "$RX_IP_ALIGN"
	# Not in the installed kernel's modules: load the tree's build.
	local m ko
	for m in veth dm_delay null_blk; do
		[ -d /sys/module/$m ] && continue
		modprobe -q $m 2>/dev/null && continue
		case $m in
		veth) ko=$HERE/../drivers/net/veth.ko ;;
		dm_delay) modprobe -q dm_mod; ko=$HERE/../drivers/md/dm-delay.ko ;;
		null_blk) ko=$HERE/../drivers/block/null_blk/null_blk.ko ;;
		esac
		[ -f "$ko" ] && insmod "$ko" || { echo "module $m not available" >&2; exit 2; }
	done
	command -v fio >/dev/null || { echo "fio missing" >&2; exit 2; }
	systemctl is-active -q nfs-server || systemctl start nfs-server
	mountpoint -q $CFG || mount -t configfs none $CFG
	modprobe nvmet
	if [ $STORAGE = sgl ]; then modprobe nvmet-tcp; modprobe nvme-tcp; else modprobe nvme-loop; fi
	mkdir "$CFG/nullb/$NB"
	echo 1 > "$CFG/nullb/$NB/memory_backed"
	echo 4096 > "$CFG/nullb/$NB/blocksize"
	echo "${NULLB_MIB:-4096}" > "$CFG/nullb/$NB/size"
	echo 2 > "$CFG/nullb/$NB/queue_mode"
	echo 1 > "$CFG/nullb/$NB/power"
	local nbdev=/dev/$NB
	[ -b $nbdev ] || { echo "no null_blk device $nbdev" >&2; exit 1; }
	dmsetup create "$DM" --table "0 $(blockdev --getsz $nbdev) delay $nbdev 0 0 $nbdev 0 0"
	mkdir "$CFG/nvmet/subsystems/$NQN"
	echo 1 > "$CFG/nvmet/subsystems/$NQN/attr_allow_any_host"
	mkdir "$CFG/nvmet/subsystems/$NQN/namespaces/1"
	echo /dev/mapper/$DM > "$CFG/nvmet/subsystems/$NQN/namespaces/1/device_path"
	echo 1 > "$CFG/nvmet/subsystems/$NQN/namespaces/1/enable"
	for PORT in $(seq 7 64); do [ -e "$CFG/nvmet/ports/$PORT" ] || break; done
	mkdir "$CFG/nvmet/ports/$PORT"
	if [ $STORAGE = sgl ]; then
		echo tcp > "$CFG/nvmet/ports/$PORT/addr_trtype"
		echo ipv4 > "$CFG/nvmet/ports/$PORT/addr_adrfam"
		echo 127.0.0.1 > "$CFG/nvmet/ports/$PORT/addr_traddr"
		echo "$NVME_TCP_PORT" > "$CFG/nvmet/ports/$PORT/addr_trsvcid"
		ln -s "$CFG/nvmet/subsystems/$NQN" "$CFG/nvmet/ports/$PORT/subsystems/$NQN"
		nvme connect -t tcp -a 127.0.0.1 -s "$NVME_TCP_PORT" -n "$NQN" >/dev/null
	else
		echo loop > "$CFG/nvmet/ports/$PORT/addr_trtype"
		# nvme-loop takes the first loop port when no address is given; name ours.
		echo "$NB" > "$CFG/nvmet/ports/$PORT/addr_traddr"
		ln -s "$CFG/nvmet/subsystems/$NQN" "$CFG/nvmet/ports/$PORT/subsystems/$NQN"
		nvme connect -t loop -n "$NQN" --traddr "$NB" >/dev/null
	fi
	local dev="" c
	for _ in $(seq 50); do
		for c in /sys/class/nvme/nvme*; do
			[ "$(cat $c/subsysnqn 2>/dev/null)" = "$NQN" ] && dev=/dev/$(basename $c)n1
		done
		[ -b "$dev" ] && break; sleep 0.1
	done
	[ -b "$dev" ] || { echo "no NVMe namespace for $NQN (STORAGE=$STORAGE)" >&2; exit 1; }
	# The rig is only what it claims to be if the device says so.
	local vbm want=4095; vbm=$(cat /sys/block/$(basename $dev)/queue/virt_boundary_mask)
	[ $STORAGE = sgl ] && want=0
	[ "$vbm" = $want ] || { echo "STORAGE=$STORAGE: $dev has virt_boundary_mask $vbm, expected $want" >&2; exit 1; }
	mkfs.xfs -q -f "$dev"
	mkdir -p "$EXPORT"
	mount "$dev" "$EXPORT"
	chmod 0777 "$EXPORT"
	exportfs -o rw,sync,no_root_squash,insecure,no_subtree_check,fsid=$FSID "10.77.0.0/24:$EXPORT"
	echo "rig: storage=$STORAGE virt_boundary_mask=$vbm rx_ip_align=$RX_IP_ALIGN nullb=$nbdev dm=/dev/mapper/$DM nvmet port=$PORT dev=$dev export=$EXPORT"
	echo "storage=$STORAGE virt_boundary_mask=$vbm rx_ip_align=$RX_IP_ALIGN dev=$dev page_size=$PS kernel=$(uname -r)" > "$E/rig.txt"
}
setdelay() {
	local ms=$1 sz; sz=$(blockdev --getsz /dev/$NB)
	dmsetup suspend "$DM"
	dmsetup reload "$DM" --table "0 $sz delay /dev/$NB 0 0 /dev/$NB 0 $ms"
	dmsetup resume "$DM"
	echo "$(date +%s.%N) delay ${ms}ms" >> "$E/delay-steps.txt"
}
veth_up() {
	local pages=${1:-0}
	veth_down
	ip netns add rxcli
	ip link add rxsrv type veth peer name rxcli0 netns rxcli
	ip addr add 10.77.0.1/24 dev rxsrv
	ip link set rxsrv up
	# Per-route quickack: the server ACKs every segment, never delays.
	[ "${QUICKACK:-0}" = 1 ] &&
		ip route replace 10.77.0.0/24 dev rxsrv src 10.77.0.1 quickack 1
	fw --zone=trusted --add-interface=rxsrv || true
	ip -n rxcli addr add 10.77.0.2/24 dev rxcli0
	ip -n rxcli link set lo up
	ip -n rxcli link set rxcli0 up
	# Client without tail-loss probes (the namespace is new each cell).
	[ "${NOTLP:-0}" = 1 ] &&
		ip netns exec rxcli sysctl -qw net.ipv4.tcp_early_retrans=0
	ip link set rxsrv mtu "${MTU:-1500}"
	ip -n rxcli link set rxcli0 mtu "${MTU:-1500}"
	if [ "$pages" -gt 0 ]; then
		echo "${REGDEV:-rxsrv}" > $PP/rx_region_dev
		echo "$pages" > $PP/rx_region_pages
	fi
	[ -f "$XDPO" ] || clang -O2 -target bpf -c "$XDPC" -o "$XDPO"
	# Pools are created here, when XDP is enabled on the server end.
	ip link set dev rxsrv xdp obj "$XDPO" sec xdp.frags
}

# --- measurement helpers (as in David's stress.sh) -------------------------
reg() { [ -r $RG ] && awk -v k="$1" '$1 == k {print $2}' $RG || echo 0; }
drops() { ip -s link show rxsrv | awk '/RX:/{getline; print $4}'; }
kv() { awk -v k="$2" '{for (i = 1; i < NF; i += 2) if ($i == k) print $(i + 1)}' <<< "$1"; }
SRV="TcpExtTCPRcvCollapsed TcpExtRcvPruned TcpExtTCPRcvQDrop TcpExtTCPZeroWindowDrop TcpExtTCPBacklogDrop TcpExtTCPOFODrop TcpExtTCPToZeroWindowAdv TcpExtTCPWantZeroWindowAdv TcpExtDelayedACKs TcpExtDelayedACKLocked TcpExtDelayedACKLost TcpExtTCPDSACKOldSent TcpExtTCPDSACKOfoSent TcpOutSegs TcpInSegs"
CLI="TcpRetransSegs TcpExtTCPLossProbes TcpExtTCPTimeouts TcpExtTCPFastRetrans TcpExtTCPSlowStartRetrans TcpExtTCPSpuriousRTOs TcpExtTCPDSACKRecv TcpExtTCPDSACKOfoRecv TcpExtTCPLossProbeRecovery TcpExtTCPDSACKUndo TcpExtTCPLossUndo TcpOutSegs"
ctr() { nstat -saz $2 2>/dev/null | awk 'NR > 1 && $1 !~ /^#/ {print $1, $2}' > "$1"; }
cctr() { ip netns exec rxcli nstat -saz $2 2>/dev/null | awk 'NR > 1 && $1 !~ /^#/ {print $1, $2}' > "$1"; }
delta() { join <(sort "$1") <(sort "$2") | awk -v k="$3" '$1 == k {print $3 - $2; f = 1} END {if (!f) print 0}'; }

jobfile() {	# jobfile <dir> <profile> <name>
	local f=$1/job.fio
	case $2 in
	seq) cat > "$f" <<J
[seq]
filename=/mnt/rxr/f-$3
rw=write
bs=1M
size=${MIB:-512}M
direct=1
ioengine=libaio
iodepth=${DEPTH:-16}
sync=${FSYNC:-0}
verify=crc32c
do_verify=1
J
	;;
	mixed) cat > "$f" <<J
[global]
directory=/mnt/rxr
rw=write
bssplit=4k/20:64k/30:256k/20:1m/30
size=${MIXMIB:-256}M
loops=${LOOPS:-1}
direct=1
ioengine=libaio
iodepth=32
verify=crc32c
do_verify=1
group_reporting
[mixed]
filename_format=f-$3.\$jobnum
numjobs=4
J
	;;
	step) cat > "$f" <<J
[step]
filename=/mnt/rxr/f-$3
rw=write
bs=1M
size=1G
time_based
runtime=120
direct=1
ioengine=libaio
iodepth=16
verify=crc32c
verify_backlog=64
do_verify=1
write_bw_log=$1/bw
log_avg_msec=500
J
	;;
	esac
}

printf 'cell prof vers nconn mtu delay_ms budget reserve factor rcvbuf MBps fio_rc drops alloc_fail escaped used_pages collapsed rcvq_drop zw_drop backlog_drop retrans loss_probes rto dsack admitted denied paused overrun wr_direct wr_dontcache wr_buffered\n' > "$E/cells.tsv"
cell() {
	local name=$1 O=$E/$1
	# FORCE_DELAY=<ms> overrides every cell's storage delay (0: none);
	# FORCE_RCVBUF=<bytes> every cell's held receive buffer (0: not held).
	[ -n "${FORCE_DELAY:-}" ] && DELAY=$FORCE_DELAY
	[ -n "${FORCE_RCVBUF:-}" ] && RCVBUF=$FORCE_RCVBUF
	rm -rf "$O"; mkdir -p "$O"
	echo 0 > $SP/svc_tcp_rx_budget_pages
	echo "$RESERVE" > $SP/svc_tcp_rx_budget_reserve
	echo "$FACTOR" > $SP/svc_tcp_rx_credit_factor
	echo "$RCVBUF" > $SP/svc_tcp_rx_rcvbuf
	echo "$BUDGET" > $SP/svc_tcp_rx_budget_pages
	sysctl -qw net.ipv4.tcp_rmem="${RMEM:-$SAVE_RMEM}"
	setdelay "$DELAY"
	MTU=$MTU veth_up "$SIZE"
	[ -w $RG ] && echo reset > $RG
	echo "${MODE:-2}" > /sys/kernel/debug/nfsd/io_cache_write
	jobfile "$O" "$PROF" "$name"
	local s0 d0 f0 x0 rc w0 w1
	wr_arm; w0=$(wr_hits)
	s0=$(cat $SP/svc_tcp_rx_budget_stats); d0=$(drops); f0=$(reg alloc_fail); x0=$(reg escaped)
	ctr "$O/srv0" "$SRV"; cctr "$O/cli0" "$CLI"
	local stepper=
	if [ "$PROF" = step ]; then
		( sleep 40; setdelay "$SPIKE"; sleep 20
		  ss -tmin state established '( sport = :2049 )' > "$O/ss-spike.txt" 2>&1
		  nstat -saz TcpExtTCPRcvQDrop TcpExtTCPRcvCollapsed > "$O/nstat-spike.txt" 2>&1
		  sleep 20; setdelay "$DELAY" ) & stepper=$!
	fi
	# SIZED=1 (a cell's own setting) or PREALLOC=1 (every cell): the files
	# already have their size (unwritten extents) and do not grow as they
	# are written.
	if [ -n "${SIZED:-}${PREALLOC:-}" ]; then
		local pf psz
		case $PROF in
		seq)   psz=${MIB:-512};     pf="$EXPORT/f-$name" ;;
		step)  psz=1024;            pf="$EXPORT/f-$name" ;;
		mixed) psz=${MIXMIB:-256};  pf="$EXPORT/f-$name.0 $EXPORT/f-$name.1 $EXPORT/f-$name.2 $EXPORT/f-$name.3" ;;
		esac
		for pf in $pf; do fallocate -l ${psz}M "$pf" && chmod 0666 "$pf"; done
	fi
	echo "$(date +%s.%N) $name fio start" >> "$E/delay-steps.txt"
	set +e
	ip netns exec rxcli bash -c "
		mkdir -p /mnt/rxr && mount -t nfs -o vers=$VERS,nolock,proto=tcp,sec=sys,nconnect=$NCONN,rsize=1048576,wsize=1048576 10.77.0.1:$EXPORT /mnt/rxr || exit 90
		timeout ${TMO:-900} fio --output-format=json --output=$O/fio.json $O/job.fio > $O/fio.out 2>&1
		rc=\$?; ${KEEPFILES:+: }rm -f /mnt/rxr/f-$name*; umount /mnt/rxr; exit \$rc"
	rc=$?
	# POSTCELL: a command run here, on the server, with the rig still up
	# (and the cell's files still on the export if KEEPFILES is set).
	[ -n "${POSTCELL:-}" ] && O=$O EXPORT=$EXPORT name=$name bash -c "$POSTCELL" || true
	set -e
	[ -n "$stepper" ] && wait $stepper
	cat $SP/svc_tcp_rx_budget_stats > "$O/budget-after.txt"
	[ -r $RG ] && cat $RG > "$O/region-after.txt"
	ctr "$O/srv1" "$SRV"; cctr "$O/cli1" "$CLI"
	local s1 mbps c; s1=$(cat $SP/svc_tcp_rx_budget_stats)
	mbps=$(python3 -c "import json;t=open('$O/fio.json').read();j=json.loads(t[t.index('{'):])['jobs'];print(round(sum(x['write']['bw_bytes'] for x in j)/1e6))" 2>/dev/null || echo 0)
	for c in admitted denied paused overrun; do eval "$c=\$(( \$(kv \"\$s1\" $c) - \$(kv \"\$s0\" $c) ))"; done
	w1=$(paste -d- <(wr_hits | tr ' ' '\n') <(echo $w0 | tr ' ' '\n') | while read x; do echo -n "$(( x )) "; done)
	echo "$name $PROF $VERS $NCONN $MTU $DELAY $BUDGET $RESERVE $FACTOR $RCVBUF $mbps $rc $(( $(drops) - d0 )) $(( $(reg alloc_fail) - f0 )) $(( $(reg escaped) - x0 )) $(( SIZE - $(reg min_free) )) $(delta $O/srv0 $O/srv1 TcpExtTCPRcvCollapsed) $(delta $O/srv0 $O/srv1 TcpExtTCPRcvQDrop) $(delta $O/srv0 $O/srv1 TcpExtTCPZeroWindowDrop) $(delta $O/srv0 $O/srv1 TcpExtTCPBacklogDrop) $(delta $O/cli0 $O/cli1 TcpRetransSegs) $(delta $O/cli0 $O/cli1 TcpExtTCPLossProbes) $(delta $O/cli0 $O/cli1 TcpExtTCPTimeouts) $(delta $O/cli0 $O/cli1 TcpExtTCPDSACKRecv) $admitted $denied $paused $overrun $w1" >> "$E/cells.tsv"
	tail -1 "$E/cells.tsv"
	veth_down
	return 0
}
# CELLS="name ..." runs only the named cells of a phase (the warm-up still runs).
cell_wanted() { [ -z "${CELLS:-}" ] && return 0; [ "$1" = warmup ] && return 0; case " $CELLS " in *" $1 "*) return 0 ;; esac; return 1; }
run() { ( export "$@"; cell_wanted "$CELL" || exit 0; cell "$CELL" ) || echo "CELL aborted rc=$? ($*)"; }

# --- PHASE=X: a region client and a non-region client at once ------------
cctrns() { ip netns exec $1 nstat -saz $3 2>/dev/null | awk 'NR > 1 && $1 !~ /^#/ {print $1, $2}' > "$2"; }
fiombps() { python3 -c "import json;t=open('$1').read();j=json.loads(t[t.index('{'):])['jobs'];print(round(sum(x['write']['bw_bytes'] for x in j)/1e6))" 2>/dev/null || echo 0; }
xclient() {	# xclient <netns> <server> <dir> <name>
	ip netns exec $1 bash -c "
		mkdir -p /mnt/rxr && mount -t nfs -o vers=$VERS,nolock,proto=tcp,sec=sys,nconnect=$NCONN,rsize=1048576,wsize=1048576 $2:$EXPORT /mnt/rxr || exit 90
		timeout ${TMO:-900} fio --output-format=json --output=$3/fio.json $3/job.fio > $3/fio.out 2>&1
		rc=\$?; rm -f /mnt/rxr/f-$4*; umount /mnt/rxr; exit \$rc"
}
# R: the region client (rxcli via rxsrv, XDP: region pages); N: the
# non-region client (rxcl2 via rxsrv2, no XDP). ONLY=R or ONLY=N runs one.
xcell() {
	local name=$1 O=$E/$1
	rm -rf "$O"; mkdir -p "$O/R" "$O/N"
	echo 0 > $SP/svc_tcp_rx_budget_pages
	echo "$RESERVE" > $SP/svc_tcp_rx_budget_reserve
	echo "$FACTOR" > $SP/svc_tcp_rx_credit_factor
	echo "$RCVBUF" > $SP/svc_tcp_rx_rcvbuf
	echo "$BUDGET" > $SP/svc_tcp_rx_budget_pages
	setdelay "$DELAY"
	MTU=$MTU veth_up "$SIZE"
	MTU=$MTU veth2_up
	[ -w $RG ] && echo reset > $RG
	echo "${MODE:-2}" > /sys/kernel/debug/nfsd/io_cache_write
	jobfile "$O/R" "$PROF" "$name-R"
	jobfile "$O/N" "$PROF" "$name-N"
	local s0 s1 d0 f0 x0 pr= pn= rcr=- rcn=- mr=- mn=- c
	s0=$(cat $SP/svc_tcp_rx_budget_stats); d0=$(drops); f0=$(reg alloc_fail); x0=$(reg escaped)
	cctrns rxcli "$O/R/cli0" "$CLI"; cctrns rxcl2 "$O/N/cli0" "$CLI"
	set +e
	[ "${ONLY:-}" != N ] && { xclient rxcli 10.77.0.1 "$O/R" "$name-R" & pr=$!; }
	[ "${ONLY:-}" != R ] && { xclient rxcl2 10.78.0.1 "$O/N" "$name-N" & pn=$!; }
	# Receive buffers mid-run: held (svc_tcp_rx_rcvbuf) or autotuned.
	( sleep 8; ss -tmin '( sport = :2049 )' > "$O/ss.txt" 2>&1 ) &
	[ -n "$pr" ] && { wait $pr; rcr=$?; mr=$(fiombps "$O/R/fio.json"); }
	[ -n "$pn" ] && { wait $pn; rcn=$?; mn=$(fiombps "$O/N/fio.json"); }
	set -e
	s1=$(cat $SP/svc_tcp_rx_budget_stats)
	echo "$s1" > "$O/budget-after.txt"
	cctrns rxcli "$O/R/cli1" "$CLI"; cctrns rxcl2 "$O/N/cli1" "$CLI"
	for c in admitted denied paused overrun; do eval "$c=\$(( \$(kv \"\$s1\" $c) - \$(kv \"\$s0\" $c) ))"; done
	echo "$name $PROF $NCONN $BUDGET $RESERVE $RCVBUF ${ONLY:-RN} $mr $rcr $mn $rcn $(( $(drops) - d0 )) $(( $(reg alloc_fail) - f0 )) $(( $(reg escaped) - x0 )) $(( SIZE - $(reg min_free) )) $(delta $O/R/cli0 $O/R/cli1 TcpRetransSegs) $(delta $O/R/cli0 $O/R/cli1 TcpExtTCPTimeouts) $(delta $O/N/cli0 $O/N/cli1 TcpRetransSegs) $(delta $O/N/cli0 $O/N/cli1 TcpExtTCPTimeouts) $admitted $denied $paused $overrun" >> "$E/xcells.tsv"
	tail -1 "$E/xcells.tsv"
	veth_down
	veth2_down
	return 0
}
xrun() { ( export "$@"; xcell "$CELL" ) || echo "CELL aborted rc=$? ($*)"; }
budget() { echo $(( SIZE - PARKED - $1 * $2 - 64 )); }

# --- campaign ------------------------------------------------------------
echo "== kernel $(uname -r); sunrpc $(cat /sys/module/sunrpc/srcversion); nfsd $(cat /sys/module/nfsd/srcversion)"
geometry | tee "$EV/geometry.txt"
[ -n "$XDPC" ] && [ -f "$XDPC" ] || { echo "xdp_pass.c not found (XDPC=)" >&2; exit 2; }
sysctl -qw net.core.skb_defer_max=0
modprobe nfs; echo N > /sys/module/nfs/parameters/localio_enabled
rig_up > "$E/rig.txt"; cat "$E/rig.txt"
if [ "$PHASE" = N ]; then
	export REGDEV=rxsrv,rxsrv2,rxsrv3
fi
if [ "$PHASE" = D ]; then
	# A region that phase D itself created this boot (bound to both
	# devices) is the one it wants: that is how the phase is repeated on
	# the other STORAGE.
	if [ -r $RG ] && [ "$(awk '$1 == "dev" {print $2}' $RG)" != rxsrv,rxsrv2 ]; then
		echo "a region already exists this boot (dev $(awk '$1 == "dev" {print $2}' $RG)); PHASE=D needs a fresh boot" >&2; exit 1
	fi
	export REGDEV=rxsrv,rxsrv2 XDP2=1
fi
if [ "$PHASE" = A ] || [ "$PHASE" = TLP ] || [ "$PHASE" = X ] || [ "$PHASE" = D ]; then
	SIZE=${SIZE:-$REGION_DEFAULT}
	# Parked pages: what the idle pool keeps after a verified warm-up write.
	run BUDGET=0 RESERVE=0 FACTOR=$F9 RCVBUF=0 MTU=9000 NCONN=1 PROF=seq VERS=3 DELAY=0 MIB=64 SIZE=$SIZE CELL=warmup
	wrc=$(tail -1 "$E/cells.tsv" | awk '{print $12}')
	[ "$wrc" = 0 ] || { echo "warm-up cell failed (fio_rc $wrc); not running the campaign" >&2; exit 1; }
	MTU=9000 veth_up "$SIZE"
	ip netns exec rxcli bash -c "mkdir -p /mnt/rxr && mount -t nfs -o vers=3,nolock,proto=tcp,nconnect=16 10.77.0.1:$EXPORT /mnt/rxr && dd if=/dev/zero of=/mnt/rxr/warm bs=1M count=64 oflag=direct status=none && rm /mnt/rxr/warm && umount /mnt/rxr"
	sleep 3
	if [ ! -r $RG ] || [ "$(reg pages)" != "$SIZE" ]; then
		echo "no $SIZE-page fixed region (allocation failed or a region of another size exists this boot):" >&2
		[ -r $RG ] && cat $RG >&2
		dmesg | grep -i 'rx region' | tail -2 >&2
		exit 1
	fi
	PARKED=$(( SIZE - $(reg free) ))
	veth_down
	echo "$PARKED" > "$EV/parked.txt"; echo "parked=$PARKED region=$SIZE"
	export SIZE PARKED
	base="MTU=9000 FACTOR=$F9 VERS=3 DELAY=1"
	on1="BUDGET=$(budget 1 $RB256) RESERVE=$R9 RCVBUF=262144"
	on16="BUDGET=$(budget 16 $RB128) RESERVE=$R9 RCVBUF=131072"
	off="BUDGET=0 RESERVE=0 RCVBUF=0"
	# A loan cell's twin that copies: budget 0, everything else the same,
	# the held receive buffer included.  ("off" holds no buffer either: it
	# shows what happens without the mechanism, and is not a twin.)
	copy1="BUDGET=0 RESERVE=0 RCVBUF=262144"
	copy16="BUDGET=0 RESERVE=0 RCVBUF=131072"
	step="MTU=9000 FACTOR=$F9 VERS=3 DELAY=0 SPIKE=10"
	if [ "$PHASE" = X ] || [ "$PHASE" = D ]; then
	exportfs -o rw,sync,no_root_squash,insecure,no_subtree_check,fsid=$FSID "10.78.0.0/24:$EXPORT"
	printf 'cell prof nconn budget reserve rcvbuf clients R_MBps R_rc N_MBps N_rc drops alloc_fail escaped used_pages R_retrans R_rto N_retrans N_rto admitted denied paused overrun\n' > "$E/xcells.tsv"
	xb="MTU=9000 FACTOR=$F9 VERS=3 DELAY=1 PROF=mixed NCONN=16"
	xoff="BUDGET=0 RESERVE=0 RCVBUF=0"
	xon="BUDGET=$(budget 16 $RB128) RESERVE=$R9 RCVBUF=131072"
	xtight="BUDGET=$R9 RESERVE=$R9 RCVBUF=131072"
	if [ "$PHASE" = D ]; then
	echo "region dev: $(awk '$1 == "dev" {print $2}' $RG)"
	xrun $xb $xoff ONLY=N CELL=d-off-N
	xrun $xb $xon ONLY=N CELL=d-on-N
	xrun $xb $xon CELL=d-on-RN
	xrun $xb $xtight ONLY=N CELL=d-tight-N
	xrun $xb $xtight CELL=d-tight-RN
	column -t "$E/xcells.tsv"
	else
	xrun $xb $xoff ONLY=R CELL=x-off-R
	xrun $xb $xoff ONLY=N CELL=x-off-N
	xrun $xb $xoff CELL=x-off-RN
	xrun $xb $xon ONLY=R CELL=x-on-R
	xrun $xb $xon ONLY=N CELL=x-on-N
	xrun $xb $xon CELL=x-on-RN
	xrun $xb $xtight ONLY=R CELL=x-tight-R
	xrun $xb $xtight ONLY=N CELL=x-tight-N
	xrun $xb $xtight CELL=x-tight-RN
	column -t "$E/xcells.tsv"
	fi
	elif [ "$PHASE" = TLP ]; then
	run $step $on16 NCONN=16 PROF=step CELL=step-on-n16
	run $step $on16 NCONN=16 PROF=step QUICKACK=1 CELL=step-on-n16-quickack
	run $step $on16 NCONN=16 PROF=step NOTLP=1 CELL=step-on-n16-notlp
	run $step BUDGET=0 RESERVE=0 RCVBUF=131072 NCONN=16 PROF=step CELL=step-rcvbuf-n16
	run $base $on16 NCONN=16 PROF=mixed CELL=mixed-on
	run $base $on16 NCONN=16 PROF=mixed QUICKACK=1 CELL=mixed-on-quickack
	run $base BUDGET=0 RESERVE=0 RCVBUF=131072 NCONN=16 PROF=mixed CELL=mixed-rcvbuf
	else
	run $base $off NCONN=1 PROF=seq CELL=seq-off
	run $base $copy1 NCONN=1 PROF=seq CELL=seq-copy
	run $base $on1 NCONN=1 PROF=seq CELL=seq-on
	# The same pair writing a file that already has its size: a direct
	# write to a growing file waits on XFS, which hides everything else.
	run $base $copy1 NCONN=1 PROF=seq SIZED=1 CELL=seq-sized-copy
	run $base $on1 NCONN=1 PROF=seq SIZED=1 CELL=seq-sized-on
	run $base $off NCONN=16 PROF=mixed CELL=mixed-off
	run $base $copy16 NCONN=16 PROF=mixed CELL=mixed-copy
	run $base $on16 NCONN=16 PROF=mixed CELL=mixed-on
	run $base $copy16 NCONN=16 PROF=mixed SIZED=1 CELL=mixed-sized-copy
	run $base $on16 NCONN=16 PROF=mixed SIZED=1 CELL=mixed-sized-on
	run $base $on16 NCONN=16 PROF=mixed VERS=4.2 CELL=mixed-on-v42
	run MTU=9000 FACTOR=$F9 VERS=3 DELAY=0 SPIKE=10 $off NCONN=1 PROF=step CELL=step-off
	run MTU=9000 FACTOR=$F9 VERS=3 DELAY=0 SPIKE=10 $copy1 NCONN=1 PROF=step CELL=step-copy
	run MTU=9000 FACTOR=$F9 VERS=3 DELAY=0 SPIKE=10 $on1 NCONN=1 PROF=step CELL=step-on
	run MTU=9000 FACTOR=$F9 VERS=3 DELAY=0 SPIKE=10 $copy16 NCONN=16 PROF=step CELL=step-copy-n16
	run MTU=9000 FACTOR=$F9 VERS=3 DELAY=0 SPIKE=10 $on16 NCONN=16 PROF=step CELL=step-on-n16
	run MTU=1500 FACTOR=$F15 VERS=3 DELAY=1 $off NCONN=1 PROF=seq CELL=mtu1500-off
	run MTU=1500 FACTOR=$F15 VERS=3 DELAY=1 BUDGET=$(budget 1 $RB256_15) RESERVE=$R15 RCVBUF=262144 NCONN=1 PROF=seq CELL=mtu1500-on
	run $base $on16 "RMEM=$RMEMS" NCONN=16 PROF=mixed CELL=collapse-on
	run $base BUDGET=$(budget 16 $RB64) RESERVE=$R9 RCVBUF=65536 NCONN=16 PROF=mixed CELL=floor-refused
	run $base $off "RMEM=$RMEMS" NCONN=16 PROF=mixed CELL=collapse-off
	# Early release (patch 19) with a budget set: DONTCACHE mode, 10 ms storage.
	run $base $on1 NCONN=1 PROF=seq MODE=1 DELAY=10 MIB=256 CELL=early-release-m1-d10
	fi
elif [ "$PHASE" = B ]; then
	[ -n "${PARKED:-}" ] || { echo "PHASE=B needs PARKED= (phase A's parked.txt)" >&2; exit 2; }
	SIZE=$(( PARKED + 4 * RB128 + R9 + 64 ))
	# The region is created at the first matching pool and never freed.
	# One that phase B itself created this boot (same size) is the one it
	# wants: that is how the phase is repeated on the other STORAGE.
	if [ -r $RG ] && [ "$(reg pages)" != "$SIZE" ]; then
		echo "a region of another size exists this boot (pages $(reg pages), phase B wants $SIZE); phase B needs a fresh boot" >&2
		exit 1
	fi
	echo "$PARKED" > "$EV/parked.txt"
	echo "parked=$PARKED region=$SIZE (configured minimum)"
	export SIZE PARKED
	base="MTU=9000 FACTOR=$F9 VERS=3 DELAY=1 MIB=256 TMO=600"
	run $base BUDGET=$R9 RESERVE=$R9 RCVBUF=262144 NCONN=1 PROF=seq CELL=edge-at-n1
	if [ ! -r $RG ] || [ "$(reg pages)" != "$SIZE" ]; then
		echo "no $SIZE-page fixed region after the first cell:" >&2
		[ -r $RG ] && cat $RG >&2
		dmesg | grep -i 'rx region' | tail -2 >&2
		exit 1
	fi
	wrc=$(tail -1 "$E/cells.tsv" | awk '{print $12}')
	[ "$wrc" = 0 ] || { echo "first cell failed (fio_rc $wrc); not running the campaign" >&2; exit 1; }
	run $base BUDGET=$R9 RESERVE=$R9 RCVBUF=131072 NCONN=4 PROF=seq CELL=edge-at-n4
	run $base BUDGET=$R9 RESERVE=$R9 RCVBUF=131072 NCONN=4 PROF=mixed CELL=edge-at-mixed-n4
	run $base BUDGET=$R9 RESERVE=$R9 RCVBUF=262144 NCONN=1 DELAY=0 SPIKE=10 PROF=step CELL=edge-at-step
	# 64 pages below the minimum.
	run $base BUDGET=$(( SIZE + 64 - PARKED - 128 )) RESERVE=$R9 RCVBUF=262144 NCONN=1 PROF=seq CELL=edge-below-n1
	run $base BUDGET=0 RESERVE=0 RCVBUF=0 NCONN=1 PROF=seq CELL=edge-off-n1
	# edge-at-n1's twin that copies: budget 0, the same held buffer.
	run $base BUDGET=0 RESERVE=0 RCVBUF=262144 NCONN=1 PROF=seq CELL=edge-copy-n1
elif [ "$PHASE" = N ]; then
	SIZE=${SIZE:-$REGION_DEFAULT}
	if [ ! -r $RG ]; then
		run BUDGET=0 RESERVE=0 FACTOR=$F9 RCVBUF=0 MTU=9000 NCONN=1 PROF=seq VERS=3 DELAY=0 MIB=64 SIZE=$SIZE CELL=warmup
	fi
	[ "$(reg pages)" = "$SIZE" ] || { echo "PHASE=N needs a $SIZE-page region (have $(reg pages))" >&2; exit 1; }
	export SIZE
	MTU=9000 srvb_up
	printf 'cell init default B clients R_MBps R_rc B_MBps B_rc init_admitted init_denied default_admitted default_denied B_admitted B_denied no_budget used_pages escaped\n' > "$E/ncells.tsv"
	nb="MTU=9000 DELAY=1 PROF=mixed NCONN=16 RCVBUF=131072 VERS=3"
	nrun $nb I=0 DF=0 BB=$(nf 2000) CL=B CELL=n-own
	nrun $nb I=0 DF=$(nf 2000) BB=0 CL=B CELL=n-default
	nrun $nb I=$(nf 1000) DF=0 BB=0 CL=B CELL=n-copy
	nrun $nb I=$(nf 1600) DF=0 BB=$(nf 1600) CL=RB CELL=n-both
	# B's budget changed under load: 2000, 400, 0 (new records go to the
	# default budget), 2000 again; both pools must end idle and whole.  4 jobs of
	# 768 MiB fit the 4096 MiB export; ten passes over them (each verified)
	# keep the load running past the hook's last change at 18 s -- one pass
	# took about 4 s once dm-delay stopped rounding the delay up to a tick.
	nrun $nb I=0 DF=$(nf 1500) BB=$(nf 2000) CL=B MIXMIB=768 LOOPS=10 CELL=n-reconf \
		HOOK='sleep 6; ip netns exec zcsrvB sysctl -qw net.sunrpc.region_budget_pages=$(nf 400); echo "6s B=$(nf 400) [$(bstats)]"; sleep 6; ip netns exec zcsrvB sysctl -qw net.sunrpc.region_budget_pages=0; echo "12s B=0 [$(bstats)]"; sleep 6; ip netns exec zcsrvB sysctl -qw net.sunrpc.region_budget_pages=$(nf 2000); echo "18s B=$(nf 2000) [$(bstats)]"'
	# The sum check: the region has $SIZE pages.
	# sysctl(8) exits 0 even when the write is refused: read the value back.
	set_budgets "$(nf 2000)" 0 0 || true
	ip netns exec zcsrvB sysctl -qw net.sunrpc.region_budget_pages="$(nf 2500)" 2>"$E/n-sum.err" || true
	if [ "$(ip netns exec zcsrvB sysctl -n net.sunrpc.region_budget_pages)" = "$(nf 2500)" ]; then
		echo "n-sum: init $(nf 2000) + B $(nf 2500) over $SIZE ACCEPTED (unexpected)"
	else
		echo "n-sum: init $(nf 2000) + B $(nf 2500) over $SIZE refused ($(tr -d '\n' < "$E/n-sum.err")), B stays $(ip netns exec zcsrvB sysctl -n net.sunrpc.region_budget_pages)"
	fi
	set_budgets 0 0 0
	# B's namespace removed with records in flight: its nfsd stopped and
	# zcsrvB deleted mid-load (soft mount: the client errors out).  Last,
	# since it takes B away.
	nrun $nb I=0 DF=0 BB=$(nf 2000) CL=B MIXMIB=1024 BOPT=soft,timeo=10,retrans=2 TMO=180 CELL=n-exit \
		HOOK='sleep 5; echo "5s: stopping nfsd in zcsrvB and deleting it"; srvb_down; echo "zcsrvB present: $(ip netns list | grep -c zcsrvB)"'
	for _ in $(seq 60); do [ "$(reg free)" = "$SIZE" ] && break; sleep 1; done
	echo "n-exit region: free $(reg free) of $SIZE, escaped $(reg escaped); init [$(cat $SP/svc_tcp_rx_budget_stats)]"
	column -t "$E/ncells.tsv"
	srvb_down
else
	echo "PHASE=$PHASE: only phases A, TLP, B, X, D and N are implemented here (phase B needs a fresh boot for a minimum-size region)" >&2
	exit 2
fi
# Pages held by closing server sockets come back once they are gone.
for _ in $(seq 60); do [ "$(reg free)" = "$SIZE" ] && break; sleep 1; done
[ -r $RG ] && cat $RG > "$E/region-final.txt"
column -t "$E/cells.tsv"
