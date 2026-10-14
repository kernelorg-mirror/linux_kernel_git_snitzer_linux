#!/bin/bash
# settled.sh - the settled setting of the rig on tardis1, in one place, and
# the audit every test runs against it (Mike, 14 October: "lock it down
# hard").  Nothing here is to be left to boot order, a service's default or
# the last run.
#
#   settled.sh audit <mode>  check the live state; print every item, and
#                            exit 1 if any differs from what <mode> expects
#   settled.sh place [a] [b] put the CPU layout in place, for nfsd and the
#                            ports named (both): after anything that opens a
#                            port, and safe while a run is under way
#   settled.sh apply         place, put the resting module settings in
#                            place, then audit "rest"
#   settled.sh show          the live state, one line an item, for a record
#
# Modes:
#   rest   no load: both ports at four rings of 256, the module settings
#          of /etc/modprobe.d (the one-port setting's), nothing connected
#   two    a two-port run at the two-port setting: rings of 128 on both
#          ports, a 384 KiB held buffer, no linger
#   one    a one-port run at the one-port setting: rings of 256, a 1 MiB
#          held buffer, a linger of 50 us
#   cell   a loan sweep's cell on port a: its plan line sets the rings, the
#          held buffer and the budget, passed as EXP_ENTRIES_A, EXP_RCVBUF,
#          EXP_BUDGET, EXP_BUDGET_RESERVE and EXP_LINGER
# A run that sets something on purpose (twoport.sh's RCVBUF= and the like)
# passes it as EXP_<NAME>: the audit then holds the live state to that and
# says the run is off the settled setting and where.
#
# THE CPU LAYOUT (XEU-4-VS-8-RING.md, "Four rings of 128 a port")
#
#   CPUs 8-55   nfsd, 512 threads in four pools of 128, and nothing of the
#               ports'.  irqbalance is barred from them and from the card's
#               interrupts (--banmod=xeu), so it cannot move either.
#   CPUs 0-7    port a (enp161s0f0): Rx-i on 2i, Tx-i on 2i+1, its async
#               interrupt on 1 with Tx-0
#   CPUs 56-63  port b (enp164s0f0): Rx-i on 56+2i, Tx-i on 57+2i, its
#               async interrupt on 57 with Tx-0
#   The async interrupt is the driver's one that is not a ring's; it fired
#   no times in a boot of runs, and left alone the kernel put it on one of
#   nfsd's CPUs.  It goes with Tx-0 because a port's transmit CPUs are 77
#   to 94% idle at 60 GB/s and its receive CPUs only 12 to 19% (Mike, 14
#   October).
#   no RPS, NAPI not threaded: a frame is received and goes through TCP on
#   the CPU its ring interrupts
#
# What undoes it, and is why this audit exists:
#   - a restart of nfs-server: its new threads run on all 64 CPUs (only
#     hammerspace-nfsd-tuning, at boot, puts them on 8-55)
#   - any open of a port (down and up, ethtool -G, ethtool -L): the driver
#     puts that port's interrupts on 0-7, whichever port it is
#   - irq_tuning.service and rps_xps_tuning.service, which run at boot and
#     again whenever udev adds an xeu port (a reload of xeu): their E1
#     profile puts port a's interrupts on 8-31 and port b's on 32-55, on
#     nfsd's CPUs, and writes its own transmit steering (below)
#   - the svcsock KUnit suite before its fix: it left svc_tcp_rx_rcvbuf at 0
#
# TRANSMIT STEERING (XPS), and the inconsistency it is locked against
#
# Every reply nfsd sends and every ACK TCP sends goes out through one of a
# port's four transmit queues, and the queue decides where the work of
# sending lands: queue i's completion interrupt (Tx-i) is on CPU 2i+1 of
# that port's eight.  xeu has no queue selection of its own, so the
# kernel's applies (netdev_pick_tx() in net/core/dev.c):
#   - A socket keeps the queue it last used.  TCP lets it pick again only
#     when it has nothing in flight (skb->ooo_okay, tcp_transmit_skb()), so
#     that its packets cannot be reordered across queues.
#   - When it picks, XPS decides first: each transmit queue has a set of
#     CPUs (/sys/class/net/<port>/queues/tx-<i>/xps_cpus), and a sender on
#     one of them gets that queue.  (xps_rxqs, which would tie the transmit
#     queue to the receive queue instead, is empty on both ports.)
#   - A sender on a CPU that no queue lists gets one by the flow's hash.
# What the ports have was not chosen.  rps_xps_tuning.service writes one
# CPU to a queue at boot, when each port has eight queues: port a's tx-i
# to CPU 8+i and port b's to 32+i.  bringup.sh and port-setup.sh then cut
# each port to four queues, which leaves port a's queues tied to CPUs 8, 9,
# 10 and 11 and port b's to 32, 33, 34 and 35, and the other 40 of nfsd's
# 48 CPUs to the hash.  So a connection's replies leave by a fixed queue
# while its nfsd threads run on one of those four CPUs, and by its hash
# when they run anywhere else; and the map itself is whatever the boot, a
# reload of xeu or a run with eight rings last left (a reload rewrites it
# for the queues the port has then; eight rings and back keep or drop
# queues 4 to 7's entries).  Two runs could differ in it and nothing would
# say so.
#
# Every settled figure (60.41 GB/s on two ports, 47.39 on one) was measured
# with exactly the map above: XEU-4-VS-8-RING.md records it ("has not been
# looked at").  So that is the map locked here, as recorded and not as
# chosen; whether another map does better is an experiment of its own, to
# be run as one and then settled here.
#
# THE MODULE SETTINGS
#
# /etc/modprobe.d/sunrpc.conf gives the resting values (the one-port
# setting's); a run sets its own and puts them back.  The audit reads the
# module's live values, never the file: the svcsock KUnit suite once left
# svc_tcp_rx_rcvbuf at 0 under a file that said 1048576, and every run
# after it went without a held buffer unseen.
. "$(dirname "$0")/common.sh"

# --- the settled setting --------------------------------------------------
NFSD_CPUSET=8-55
NFSD_THREADS=512
NFSD_POOLS="128 128 128 128"
declare -A PDEV=([a]=enp161s0f0 [b]=enp164s0f0)
declare -A PCPU=([a]=0 [b]=56)		# a port's eight CPUs start here
declare -A PXPS=([a]=8 [b]=32)		# tx-i's XPS CPU, as recorded: <this> + i
RINGS=4
MTU=7792
REGION_PAGES=32768
# The sunrpc module's settings that a run may change, and their values at
# rest.  A mode changes some of them (mode_expect below).
declare -A REST=(
	[svc_tcp_rx_rcvbuf]=1048576
	[svc_tcp_rx_linger_us]=50
	[svc_tcp_rx_keep]=Y
	[svc_tcp_rx_lowat]=0
	[svc_tcp_rx_copy_threads]=0
	[svc_tcp_rx_budget_pages]=0
	[svc_tcp_rx_budget_reserve]=0
	[svc_tcp_rx_budget_default_pages]=0
	[svc_tcp_rx_budget_default_reserve]=0
	[svc_tcp_rx_loan_pages]=Y
	[svc_tcp_rx_copy_unlocked]=Y
	[svc_tcp_rx_copy_below]=1024
	[pools_per_node]=4
)
# The xeu module's, set by /etc/modprobe.d/xeu.conf at load.
declare -A XEU=([rx_buf_size]=8192 [rx_ip_align]=2 [rx_refill_thresh]=8 [rss_indir_size]=128)
declare -A NFSD_DBG=([io_cache_read]=2 [io_cache_write]=2)
SUNRPC=/sys/module/sunrpc/parameters

# --- reading the live state -------------------------------------------------
port_irqs() {	# port_irqs <dev>: "<irq> <name>" for each of the port's interrupts
	local pci; pci=$(basename "$(readlink -f /sys/class/net/$1/device)")
	for i in $(ls /sys/bus/pci/devices/$pci/msi_irqs); do
		echo "$i $(awk -v i="$i:" '$1 == i {print $NF}' /proc/interrupts)"
	done
}
xps_cpu() {	# xps_cpu <mask file>: the one CPU a mask names, "none", or "many"
	local v; v=$(tr -d ',\n' < "$1"); v=$(( 16#${v:-0} ))
	[ $v = 0 ] && { echo none; return; }
	[ $(( v & (v - 1) )) != 0 ] && { echo many; return; }
	local c=0; while [ $(( (v >> c) & 1 )) = 0 ]; do c=$(( c + 1 )); done; echo $c
}
nfsd_cpus() {	# how many of nfsd's threads are allowed which CPUs
	for t in $(pgrep -x nfsd); do taskset -cp $t 2> /dev/null | awk '{print $NF}'; done | sort | uniq -c | awk '{printf "%s%s on %s", (NR > 1 ? ", " : ""), $1, $2}'
}

# --- the audit ----------------------------------------------------------------
BAD=0; OFF=""
ok()  { printf '  ok   %-34s %s\n' "$1" "$2"; }
bad() { printf '  BAD  %-34s %s (want %s)\n' "$1" "$2" "$3"; BAD=$(( BAD + 1 )); }
want() {	# want <item> <live> <expected>
	if [ "$2" = "$3" ]; then ok "$1" "$2"; else bad "$1" "$2" "$3"; fi
}

mode_expect() {	# fill EXPECT[] and ENTRIES[] for a mode, EXP_* applied over it
	local mode=$1 k
	declare -gA EXPECT=() ENTRIES=([a]=256 [b]=256) SETTLED=()
	for k in "${!REST[@]}"; do EXPECT[$k]=${REST[$k]}; done
	case $mode in
	rest|one) ;;
	two)	EXPECT[svc_tcp_rx_rcvbuf]=393216; EXPECT[svc_tcp_rx_linger_us]=0
		ENTRIES=([a]=128 [b]=128) ;;
	cell)	: ;;	# its plan line, below
	*)	echo "unknown mode: $mode"; exit 2 ;;
	esac
	for k in "${!EXPECT[@]}"; do SETTLED[$k]=${EXPECT[$k]}; done
	SETTLED[entries_a]=${ENTRIES[a]}; SETTLED[entries_b]=${ENTRIES[b]}
	# What a run sets on purpose
	[ -n "${EXP_RCVBUF:-}" ] && EXPECT[svc_tcp_rx_rcvbuf]=$EXP_RCVBUF
	[ -n "${EXP_LINGER:-}" ] && EXPECT[svc_tcp_rx_linger_us]=$EXP_LINGER
	[ -n "${EXP_LOWAT:-}" ] && EXPECT[svc_tcp_rx_lowat]=$EXP_LOWAT
	[ -n "${EXP_COPY_THREADS:-}" ] && EXPECT[svc_tcp_rx_copy_threads]=$EXP_COPY_THREADS
	[ -n "${EXP_BUDGET:-}" ] && EXPECT[svc_tcp_rx_budget_pages]=$EXP_BUDGET
	[ -n "${EXP_BUDGET_RESERVE:-}" ] && EXPECT[svc_tcp_rx_budget_reserve]=$EXP_BUDGET_RESERVE
	[ -n "${EXP_ENTRIES_A:-${EXP_ENTRIES:-}}" ] && ENTRIES[a]=${EXP_ENTRIES_A:-$EXP_ENTRIES}
	[ -n "${EXP_ENTRIES_B:-${EXP_ENTRIES:-}}" ] && ENTRIES[b]=${EXP_ENTRIES_B:-$EXP_ENTRIES}
	for k in "${!EXPECT[@]}"; do
		[ "${EXPECT[$k]}" = "${SETTLED[$k]}" ] || OFF="$OFF $k=${EXPECT[$k]}"
	done
	[ "${ENTRIES[a]}" = "${SETTLED[entries_a]}" ] || OFF="$OFF rings_a=${ENTRIES[a]}"
	[ "${ENTRIES[b]}" = "${SETTLED[entries_b]}" ] || OFF="$OFF rings_b=${ENTRIES[b]}"
	[ -n "${EXP_NFSD_CPUS:-}" ] && OFF="$OFF nfsd_cpus=$EXP_NFSD_CPUS"
	[ -n "${EXP_LAYOUT:-}" ] && OFF="$OFF layout=$EXP_LAYOUT"
	return 0
}

audit_nfsd() {
	local n; n=$(pgrep -x nfsd | wc -l)
	want "nfsd threads" "$n" "$NFSD_THREADS"
	want "nfsd threads in each pool" "$(cat /proc/fs/nfsd/pool_threads 2>/dev/null)" "$NFSD_POOLS"
	want "nfsd threads' CPUs" "$(nfsd_cpus)" "$n on ${EXP_NFSD_CPUS:-$NFSD_CPUSET}"
	local k; for k in "${!NFSD_DBG[@]}"; do
		want "nfsd $k" "$(cat /sys/kernel/debug/nfsd/$k 2>/dev/null)" "${NFSD_DBG[$k]}"
	done
	want "nfsd max_block_size" "$(cat /proc/fs/nfsd/max_block_size 2>/dev/null)" 1048576
}

audit_irqbalance() {
	local p; p=$(pidof irqbalance)
	want "irqbalance running" "${p:+yes}" yes
	[ -n "$p" ] || return
	want "irqbalance leaves xeu alone" "$(tr '\0' '\n' < /proc/$p/cmdline | grep -cx -- --banmod=xeu)" 1
	want "irqbalance kept off nfsd's CPUs" "$(tr '\0' '\n' < /proc/$p/environ | sed -n 's/^IRQBALANCE_BANNED_CPULIST=//p')" "$NFSD_CPUSET"
}

audit_port() {	# audit_port <a|b>
	local p=$1 d=${PDEV[$1]} c0=${PCPU[$1]} irq name i want_cpu q
	want "$d mtu" "$(cat /sys/class/net/$d/mtu)" $MTU
	want "$d rings" "$(ethtool -l $d | awk '/^Combined:/ {v = $2} END {print v}')" $RINGS
	want "$d ring depth" "$(ethtool -g $d | awk '/^RX:/ {v = $2} END {print v}')" "${ENTRIES[$p]}"
	want "$d threaded NAPI" "$(cat /sys/class/net/$d/threaded)" 0
	for q in /sys/class/net/$d/queues/rx-*; do
		want "$d $(basename $q) RPS" "$(xps_cpu $q/rps_cpus)" none
	done
	[ -n "${EXP_LAYOUT:-}" ] && { echo "  --   $d interrupts and XPS: not held ($EXP_LAYOUT)"; return; }
	while read -r irq name; do
		case "$name" in
		$d-Rx-*) i=${name##*-}; want_cpu=$(( c0 + 2 * i )) ;;
		$d-Tx-*) i=${name##*-}; want_cpu=$(( c0 + 2 * i + 1 )) ;;
		$d-async) want_cpu=$(( c0 + 1 )) ;;	# with Tx-0
		*) continue ;;
		esac
		want "$name CPU" "$(cat /proc/irq/$irq/smp_affinity_list)/$(cat /proc/irq/$irq/effective_affinity_list)" "$want_cpu/$want_cpu"
	done < <(port_irqs $d | sort -k2 -V)
	for q in /sys/class/net/$d/queues/tx-*; do
		i=${q##*-}
		want "$d tx-$i XPS CPU" "$(xps_cpu $q/xps_cpus)" $(( ${PXPS[$p]} + i ))
		want "$d tx-$i XPS by receive queue" "$(xps_cpu $q/xps_rxqs)" none
	done
}

audit() {
	local mode=$1 k
	mode_expect $mode
	echo "settled.sh audit $mode: kernel $(uname -r), $(date '+%F %T')"
	audit_nfsd
	audit_irqbalance
	audit_port a
	audit_port b
	for k in $(printf '%s\n' "${!EXPECT[@]}" | sort); do
		want "sunrpc $k" "$(cat $SUNRPC/$k 2>/dev/null)" "${EXPECT[$k]}"
	done
	for k in $(printf '%s\n' "${!XEU[@]}" | sort); do
		want "xeu $k" "$(cat /sys/module/xeu/parameters/$k 2>/dev/null)" "${XEU[$k]}"
	done
	want "receive region pages" "$(cat $PP/rx_region_pages)" $REGION_PAGES
	want "receive region devices" "$(tr ',' '\n' < $PP/rx_region_dev | grep . | sort | paste -sd ' ')" \
		"$(for p in a b; do basename "$(readlink -f /sys/class/net/${PDEV[$p]}/device)"; done | sort | paste -sd ' ')"
	for k in 0 1 2 3 4 5 6 7; do
		want "nvme${k}n1 cache" "$(cat /sys/block/nvme${k}n1/queue/write_cache)" "write through"
	done
	[ $mode = rest ] && want "NFS connections" "$(ss -Htn state established '( sport = :2049 )' | wc -l)" 0
	if [ $BAD != 0 ]; then
		echo "settled.sh audit $mode: FAILED, $BAD item(s) BAD"
		return 1
	fi
	if [ -n "$OFF" ]; then
		echo "settled.sh audit $mode: as the run asks, which is off the settled setting in:$OFF"
	else
		echo "settled.sh audit $mode: the settled setting"
	fi
}

# --- putting it in place --------------------------------------------------------
place() {	# place [a] [b]: the CPU layout of nfsd and of the ports named (both)
	local n p d c0 irq name q i m
	n=$(pgrep -x nfsd | wc -l)
	[ "$n" = $NFSD_THREADS ] || { echo "nfsd has $n threads, not $NFSD_THREADS: systemctl restart hammerspace-nfsd-tuning"; return 1; }
	for t in $(pgrep -x nfsd); do taskset -pc $NFSD_CPUSET $t > /dev/null; done
	for p in ${*:-a b}; do
		d=${PDEV[$p]:?port a or b}; c0=${PCPU[$p]}
		[ "$(ethtool -l $d | awk '/^Combined:/ {v = $2} END {print v}')" = $RINGS ] ||
			{ echo "$d does not have $RINGS rings: port-setup.sh (bringup.sh for port a on a fresh boot)"; return 1; }
		# The driver puts a port's interrupts, the async one too, back on
		# CPUs 0-7 at every open; XPS, RPS and threading outlive an open
		# but are set here all the same.
		"$RIG/ring-irqs.sh" $d $c0 > /dev/null
		while read -r irq name; do
			[ "$name" = $d-async ] && echo $(( c0 + 1 )) > /proc/irq/$irq/smp_affinity_list	# with Tx-0
		done < <(port_irqs $d)
		for q in /sys/class/net/$d/queues/tx-*; do
			i=${q##*-}
			m=$(( 1 << (${PXPS[$p]} + i) ))	# as the kernel prints a mask: 32-bit words, high first
			printf '%08x,%08x\n' $(( (m >> 32) & 0xffffffff )) $(( m & 0xffffffff )) > $q/xps_cpus
			echo 0 > $q/xps_rxqs
		done
		for q in /sys/class/net/$d/queues/rx-*; do echo 0 > $q/rps_cpus; done
		echo 0 > /sys/class/net/$d/threaded
	done
}

apply() {	# the CPU layout, the resting module settings, and the audit
	local k
	place || return 1
	if [ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ]; then
		for k in "${!REST[@]}"; do
			[ -w $SUNRPC/$k ] && [ "$(cat $SUNRPC/$k)" != "${REST[$k]}" ] && echo "${REST[$k]}" > $SUNRPC/$k
		done
	else
		echo "NFS connections are up: the sunrpc settings are left as they are"
	fi
	audit rest
}

show() {
	BAD=0; audit "${1:-rest}" 2>&1 | sed -n 's/^  \(ok\|BAD\) *//p'
}

case "${1:-}" in
audit) audit "${2:?mode: rest, two, one or cell}" ;;
apply) apply ;;
place) shift; place "$@" ;;
show)  show "${2:-rest}" ;;
*)     sed -n '2,30p' "$0"; exit 2 ;;
esac
