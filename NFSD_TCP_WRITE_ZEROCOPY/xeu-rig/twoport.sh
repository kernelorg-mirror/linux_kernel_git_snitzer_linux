#!/bin/bash
# twoport.sh <out dir> [a|b|ab] - the pinned copy load on one port, the
# other, or both at once, each port onto its own stripe from its own mount
# on the client.  sweep.sh and run-budget.sh drive one port and keep the
# server's settings to themselves, so two of them cannot run together;
# this is the same load (16 connections, 16 writers of 8 MiB direct writes,
# every connection placed on a ring) set up once for both.
#
# A run has to stay inside the drives' fast region: about 625 GB a drive
# from its last idle of 20 s, the placement load counted (drvchar.py, and
# "What a drive takes" in XEU_CONVERGENCE.md).  Past it, the first pause
# in the writes leaves the drive at 4 GB/s.  The defaults (15 s of warm-up
# and 45 s counted) keep two ports inside it on four drives each; one port
# on its own four needs WARM=10 RUNTIME=25, or all eight drives in FS_A
# (or FS_B) with RUNTIME=60.  drives.txt says what each drive wrote,
# whether one stepped down, and (stalled.py) every stall of the drives
# together, the warm-up included, with the wire and the client's
# retransmits over it, from drives-sec.tsv, wire-sec.tsv and
# client-sec.tsv, each second of the load.
#
#   a   enp161s0f0  192.168.1.211  /hs/nvme0 to /hs/nvme3  client ens3np0
#   b   enp164s0f0  192.168.0.211  /hs/nvme4 to /hs/nvme7  client ens6np0
#
# That is tardis1.  The ports' addresses and filesystems are common.sh's for
# the host: on tardis2 the same drives on the same ports, at .213, all eight
# with SGLs since 2 November.  The ports default to the host's, ab on both.
#
# A port's drives each carry a filesystem of their own (no MD since
# 2026-10-12): each is exported and mounted, /mnt/rxr-<port><n> on the
# client, and the port's writers are divided evenly among them.  The
# mounts of one port share its connections.  FS_A and FS_B name other
# filesystems, one or several.
#
# It expects what the pinned configuration leaves: svc_tcp_rx_rcvbuf,
# svc_tcp_rx_linger_us and io_cache_write as pinned, both ports set up by
# port-setup.sh (MTU 7792, 4 rings of 256, in the region).  It takes each
# port down and up first, which zeroes its rings' counters, and puts the
# rings' interrupts back on their CPUs afterwards.
# RUNTIME=<s> (45), JOBS=<n> (16), BS (8M), NCONN (16 on one port; on two,
# common.sh's TWO_NCONN: 12 on tardis2, 16 on tardis1), PLACE=0 to leave
# the flows where they fall.  For a run and put back after it:
# The setting is settled.sh's: a run on both ports is at the two-port
# setting (RCVBUF=$TWO_RCVBUF ENTRIES=128 LINGER=0 unless given: 512 KiB on
# tardis2, 384 KiB on tardis1), a run on one
# port at the one-port setting.  settled.sh audits the host before the run
# changes anything (audit-start.txt) and the live state just before the load
# (audit.txt), and the run refuses if anything differs from the setting or
# from what the run itself set; the variables below take a run off the
# settled setting, and audit.txt's last line says where.
#   RCVBUF=<bytes>   svc_tcp_rx_rcvbuf, the held receive buffer
#   LINGER=<us>      svc_tcp_rx_linger_us
#   THREADS=<n>      svc_tcp_rx_copy_threads
#   LOWAT=<bytes>    svc_tcp_rx_lowat, the mark a short WRITE's thread is woken at (0: off)
#   WANT_EIGHTHS=<n> svc_tcp_rx_want_eighths, the eighths of the open window an incomplete
#                    loaned record waits for before a thread takes it up again (settled: 6)
#   KEEP=Y|N         svc_tcp_rx_keep, whether a thread that finds a WRITE short leaves
#                    the transport unqueued for data already there
#   SPARE=<bytes>    svc_tcp_spare_bytes_max, the pages sockets keep for exchange (0: none)
#   REFILL=<n>       xeu's rx_refill_thresh, the empty RX slots at which a poll refills
#                    a ring (settled: 8; the driver's default 32); read every poll
#   SKB_DEFER_MAX=<n> net.core.skb_defer_max for the run (settled: 0 since 23 October; 0 frees a
#                    consumed skb where it was consumed, not on its ring's CPU)
#   SRV_QDISC=<q>    the qdisc on each of the server port's transmit queues (settled: fq_codel)
#   GRO_MAX=<bytes>  the server port's gro_max_size and gro_ipv4_max_size (settled: 65536; above it, BIG TCP)
#   DEFER=<n>/<ns>   napi_defer_hard_irqs and gro_flush_timeout of the ports used
#                    (settled: 2/10000 on port a since 29 October, on port b since 30 October)
#   NOTRACK=absent   no zc_notrack table for the run (settled: loaded while conntrack is on)
#   ENTRIES=<n>      entries of each RX ring, on the ports used
#   RINGS=<n>        rings of each port used (4); over 4, Rx and Tx share CPUs
#   TXIRQ_A=<cpu> TXIRQ_B=<cpu>  a port's Tx rings on two CPUs from <cpu>, and
#                    its Rx rings one to a CPU on the port's eight
#   TXN=<n>          with TXIRQ_A/B: the Tx rings on <n> CPUs from <cpu>, not two
#   RX_RINGS=<n>     with RINGS: frames only to rings 0 to <n>-1 (the RSS table and the
#                    placement), and every ring's interrupts on the four-ring layout,
#                    Rx-i on 2i and Tx-i on 2i+1 of the port's eight, two rings a CPU
#                    over four: more transmit queues without more receiving rings
#   XPS=spread       each transmit queue's XPS map a block of nfsd's CPUs (8-55), so
#                    every sender has a queue of its own (settled: tx-i on 8+i only)
#   NFSD_CPUS=<list> nfsd's threads on these CPUs for the run (8-55 after it), to
#                    keep them off CPUs given to Tx rings
#   QDISC=fq|fqnp|fqNN  fq on the client's ports (fqnp: fq with pacing off; fqNN: maxrate
#                    NN Gbit/s a flow)
#   PROFILE=1        mid-run: perf stat for the rings' CPUs and the rest, then where the
#                    cycles and the fetches from memory go (needs RUNTIME=55 at least)
#   PIN=1            each port's writers on the client CPUs of its NIC's NUMA node
#   WARM=<s>         the writers run this long before fio starts counting (15)
#   REST=<s>         wait this long first, for the drives (30)
#   DRIVE_GB=<n>     mark the run if a drive wrote more than this in it (600)
#   EXPORT_SYNC=async  export async, so a COMMIT does not sync the file or flush the drives
#   RW=<fio rw>      read, randread, rw, randrw, randwrite (write); any run but write first
#                    writes every file whole once (8 MiB direct writes) and rests 20 s
#   RWMIX=<n>        the share of reads in a mixed run, percent (fio's 50)
#   IODEPTH=<n> (16), SIZE=<fio size> (512M): each writer's queue and file
#   WRITES=<n>       loan: a budget that lets <n> 1 MiB WRITEs hold loaned pages at
#                    once, worked out as sweep.sh does from run-budget.sh's geometry
#                    (charge per WRITE, reserve); unset or 0 copies, as settled
#   BUDGET_PAGES=<n> with WRITES: the budget in pages instead (the reserve as
#                    WRITES gives it)
# Every run reports how nfsd issued its WRITEs (segments direct, dontcache and
# buffered, from the nfsd_write_* tracepoints) and, with WRITES, what the
# budget admitted, denied and paused.
# TSV=<file>: append a line of the run's figures (TAG=<label> names it); a write
# run's only.  RWTSV=<file>: the same for any run, reads and writes apart.
. "$(dirname "$0")/common.sh"
"$(dirname "$0")/drive-cache.sh" > /dev/null || { echo "drive-cache.sh failed"; exit 1; }	# no cache flush to the drives (AGENTS.md)
OUT=${1:?out dir}; WHICH=${2:-$DEFWHICH}
which_ok "$WHICH" || exit 1
# A run on both ports is at the two-port setting and one on one port at the
# one-port setting, unless it asks otherwise (settled.sh, README.md "Where
# it stands").
if [ ${#WHICH} = 2 ]; then
	MODE_SET=two; RCVBUF=${RCVBUF-$TWO_RCVBUF}; ENTRIES=${ENTRIES-128}; LINGER=${LINGER-0}; NCONN=${NCONN:-$TWO_NCONN}
else
	MODE_SET=one
fi
RUNTIME=${RUNTIME:-45}; WARM=${WARM:-15}; JOBS=${JOBS:-16}; BS=${BS:-8M}; NCONN=${NCONN:-16}
RW=${RW:-write}; IODEPTH=${IODEPTH:-16}; SIZE=${SIZE:-512M}
YNL="$H/../tools/net/ynl/pyynl/cli.py --no-schema --spec $H/../Documentation/netlink/specs/ethtool.yaml"
declare -A DEV=([a]=${PDEVS[a]} [b]=${PDEVS[b]}) SRV=([a]=${SRVP[a]} [b]=${SRVP[b]})
declare -A CLI=([a]=${CLIP[a]} [b]=${CLIP[b]}) FS=([a]="${FS_A:-${PFS[a]}}" [b]="${FS_B:-${PFS[b]}}")
declare -A IRQ0=([a]=0 [b]=56) FSID=([a]=7701 [b]=7702) TXIRQ=([a]=${TXIRQ_A:-} [b]=${TXIRQ_B:-})
declare -A NFS	# how many filesystems each port has
declare -A CPUS=([a]=0-23 [b]=24-47)	# the client's CPUs on each port's NUMA node, for PIN=1
PORTS=$(sed 's/./& /g' <<< "$WHICH")
SSHC="ssh -o BatchMode=yes -o ConnectTimeout=30 -o ServerAliveInterval=10 -o ServerAliveCountMax=3 -i $KEY root@$CLIMGMT"	# not over a port under test
cl() { $SSHC "$@" < /dev/null; }
clput() { $SSHC "cat > $1"; }	# stdin to a file on the client

[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || { echo "NFS connections are up: not ours to disturb"; exit 1; }
client_free || exit 1	# the client is shared with the other server
mkdir -p $OUT; cd $OUT || exit 1
# The host has to be at the settled setting before the run changes anything.
audit_or_die rest audit-start.txt
sleep ${REST:-30}	# 20 s of idle gives a drive its fast region back
fifo() { ethtool -S $1 | awk '/port_rx_fifo_tc[0-9]_drop_packets/{s+=$2} END{print s+0}'; }
ringdrop() { ethtool -S $1 | awk '/rx_hw_tail_drop_packets/{s+=$2} END{print s+0}'; }
rxbytes() { ethtool -S $1 | awk '$1 ~ /^ring_[0-9]+_rx_bytes:$/{s+=$2} END{print s+0}'; }
txbytes() { ethtool -S $1 | awk '$1 ~ /^ring_[0-9]+_tx_bytes:$/{s+=$2} END{print s+0}'; }
ringshare() { ethtool -S $1 | awk '$1 ~ /^ring_[0-9]+_rx_bytes:$/{print $2}' | tr '\n' ' '; }
cretr() { cl "awk '/^Tcp:/ && !h {for (i = 1; i <= NF; i++) if (\$i == \"RetransSegs\") c = i; h = 1; next} /^Tcp:/ {print \$c}' /proc/net/snmp"; }

SP=/sys/module/sunrpc/parameters
declare -A SAVE
# copy_threads, lowat, linger_us and keep are not in kernels after build 474
# (dropped 15 October)
for k in rcvbuf linger_us copy_threads lowat keep want_eighths; do SAVE[$k]=$(cat $SP/svc_tcp_rx_$k 2>/dev/null); done
SAVE[spare]=$(cat $SP/svc_tcp_spare_bytes_max 2>/dev/null)
SAVE[refill]=$(cat /sys/module/xeu/parameters/rx_refill_thresh)
SAVE[budget]=$(cat $SP/svc_tcp_rx_budget_pages); SAVE[reserve]=$(cat $SP/svc_tcp_rx_budget_reserve)
# The loan's budget, from the same geometry and charge as sweep.sh's cells.
BUDGET=; RESERVE=
if [ "${WRITES:-0}" -gt 0 ]; then
	G=$(cd $H && GEOMETRY=1 NETDEV=${DEV[${PORTS%% *}]} CLIENT=root@$CLIMGMT CLIENT_ADDR=${CLI[${PORTS%% *}]} ./run-budget.sh /dev/null 2>/dev/null | grep '^geometry')
	F9=$(sed -n 's/.*factor \([0-9]*\) (MTU 9000).*/\1/p' <<< "$G")
	RESERVE=$(sed -n 's/.*reserve \([0-9]*\) \/.*/\1/p' <<< "$G")
	[ -n "$F9" ] && [ -n "$RESERVE" ] || { echo "cannot read the geometry: $G"; exit 1; }
	CHARGE=$(( ( (1048716 + PS - 1) / PS * F9 + 99 ) / 100 + 2 ))
	BUDGET=$(( WRITES * CHARGE + RESERVE + 4 ))
	BUDGET=${BUDGET_PAGES:-$BUDGET}
fi
T=/sys/kernel/tracing; WREV="nfsd_write_direct nfsd_write_dontcache nfsd_write_vector"
wr_hits() { local e; for e in $WREV; do awk '/^ *Hits:/ {print $2; f = 1} END {if (!f) print 0}' $T/events/nfsd/$e/hist 2>/dev/null || echo 0; done | paste -sd ' '; }
bstat() { tr ' ' '\n' < $SP/svc_tcp_rx_budget_stats | awk -v k=$1 'p == k {print; exit} {p = $0}'; }
declare -A CDEV=([a]=ens3np0 [b]=ens6np0)
setq() {	# setq <client dev> <codel|fq|fqNN>
	case $2 in
	codel) cl "tc qdisc del dev $1 root 2>/dev/null; true" ;;
	fq)    cl "tc qdisc replace dev $1 root handle 1: mq && for i in \$(seq 1 48); do tc qdisc replace dev $1 parent 1:\$(printf %x \$i) fq || exit 1; done" ;;
	fqnp)  cl "tc qdisc replace dev $1 root handle 1: mq && for i in \$(seq 1 48); do tc qdisc replace dev $1 parent 1:\$(printf %x \$i) fq nopacing || exit 1; done" ;;
	fq*)   cl "tc qdisc replace dev $1 root handle 1: mq && for i in \$(seq 1 48); do tc qdisc replace dev $1 parent 1:\$(printf %x \$i) fq maxrate ${2#fq}gbit || exit 1; done" ;;
	esac
}
cleanup() {
	for k in rcvbuf linger_us copy_threads lowat keep want_eighths; do [ -e $SP/svc_tcp_rx_$k ] && echo ${SAVE[$k]} > $SP/svc_tcp_rx_$k; done
	[ -n "${SAVE[spare]}" ] && echo ${SAVE[spare]} > $SP/svc_tcp_spare_bytes_max
	echo ${SAVE[refill]} > /sys/module/xeu/parameters/rx_refill_thresh
	echo 0 > $SP/svc_tcp_rx_budget_pages; echo ${SAVE[reserve]} > $SP/svc_tcp_rx_budget_reserve; echo ${SAVE[budget]} > $SP/svc_tcp_rx_budget_pages
	for e in $WREV; do echo '!hist:keys=common_type' > $T/events/nfsd/$e/trigger 2>/dev/null; done
	[ -n "${QDISC:-}" ] && for p in $PORTS; do setq ${CDEV[$p]} codel; done
	# The RSS table may name rings that are going: spread it over four first.
	[ -n "${RINGS:-}" ] && { for p in $PORTS; do ethtool -X ${DEV[$p]} equal 4 2>/dev/null; ip link set ${DEV[$p]} down; sleep 1; ethtool -L ${DEV[$p]} combined 4; ip link set ${DEV[$p]} up; done; sleep 7; }
	[ -n "${ENTRIES:-}${RINGS:-}" ] && { for p in $PORTS; do ethtool -G ${DEV[$p]} rx 256 2>/dev/null; done; sleep 7; }
	for p in $PORTS; do
		cl "rm -f /mnt/rxr-$p[0-9]*/f-* /mnt/rxr-$p[0-9]*/bal-* 2>/dev/null; for m in /mnt/rxr-$p[0-9]*; do umount -f \$m 2>/dev/null; done"
		for fs in ${FS[$p]}; do exportfs -u "${CLI[$p]}:$fs/zcbudget" 2>/dev/null; done
	done
	echo N > /sys/module/xeu/parameters/rx_port_stats
	[ -n "${SRV_QDISC:-}" ] && for p in $PORTS; do "$RIG/settled.sh" srvq ${DEV[$p]}; done	# the settled qdisc back
	[ -n "${GRO_MAX:-}" ] && for p in $PORTS; do "$RIG/settled.sh" gro ${DEV[$p]}; done	# the settled GRO sizes back
	[ "${NOTRACK:-}" = absent ] && [ "$(nft list hooks 2> /dev/null | grep -c conntrack)" != 0 ] && nft -f "$RIG/zc-notrack.nft"	# the settled table back, if conntrack is on
	# An open of a port put its interrupts back on CPUs 0-7, and NFSD_CPUS=
	# moved nfsd: the settled CPU layout back, and the host as the run left it.
	"$RIG/settled.sh" place $PORTS
	sleep 2
	"$RIG/settled.sh" audit rest > $OUT/audit-end.txt 2>&1 || { echo "the host is not at the settled setting after the run:"; grep -E 'BAD|FAILED' $OUT/audit-end.txt; }
	puppet_resume	# the client's puppet agent on again, if this run turned it off
}
trap cleanup EXIT

echo "twoport $WHICH: kernel $(uname -r), xeu $(cat /sys/module/xeu/srcversion), sunrpc $(cat /sys/module/sunrpc/srcversion)" | tee geometry.txt
echo "rcvbuf $(cat /sys/module/sunrpc/parameters/svc_tcp_rx_rcvbuf) linger $(cat /sys/module/sunrpc/parameters/svc_tcp_rx_linger_us 2>/dev/null || echo -) copy_threads $(cat /sys/module/sunrpc/parameters/svc_tcp_rx_copy_threads 2>/dev/null || echo -) io_cache_write $(cat /sys/kernel/debug/nfsd/io_cache_write) nfsd threads $(cat /proc/fs/nfsd/threads) rx_refill_thresh $(cat /sys/module/xeu/parameters/rx_refill_thresh) region dev $(cat $PP/rx_region_dev)" | tee -a geometry.txt
echo Y > /sys/module/xeu/parameters/rx_port_stats
[ -n "${RCVBUF:-}" ] && echo $RCVBUF > $SP/svc_tcp_rx_rcvbuf
[ -n "${REFILL:-}" ] && echo $REFILL > /sys/module/xeu/parameters/rx_refill_thresh
[ -n "${SKB_DEFER_MAX:-}" ] && sysctl -qw net.core.skb_defer_max=$SKB_DEFER_MAX	# settled.sh apply puts it back
[ "${NOTRACK:-}" = absent ] && { nft delete table inet zc_notrack 2> /dev/null; echo "zc_notrack: absent for this run" | tee -a geometry.txt; }
# setp <value> <parameter> <off>: set it.  Parameters dropped from the
# series (copy_threads, lowat, linger_us, keep) are not in later kernels: there
# asking for the off value is what the kernel does, and anything else stops.
setp() {
	[ -n "$1" ] || return 0
	if [ ! -e $SP/svc_tcp_rx_$2 ]; then
		[ "$1" = "$3" ] && return 0
		echo "svc_tcp_rx_$2=$1: not in this kernel"; exit 2
	fi
	echo $1 > $SP/svc_tcp_rx_$2
}
setp "${LINGER:-}" linger_us 0
setp "${THREADS:-}" copy_threads 0
setp "${LOWAT:-}" lowat 0
setp "${KEEP:-}" keep N
setp "${WANT_EIGHTHS:-}" want_eighths 4
if [ -n "$BUDGET" ]; then	# reserve first, as run-budget.sh sets them
	echo 0 > $SP/svc_tcp_rx_budget_pages; echo $RESERVE > $SP/svc_tcp_rx_budget_reserve; echo $BUDGET > $SP/svc_tcp_rx_budget_pages
	echo "loan: $G; charge $CHARGE pages a 1 MiB WRITE, budget $BUDGET pages for $WRITES WRITEs, reserve $RESERVE" | tee -a geometry.txt
fi
for e in $WREV; do grep -q '^hist:' $T/events/nfsd/$e/trigger 2>/dev/null || echo 'hist:keys=common_type' > $T/events/nfsd/$e/trigger; done
[ -n "${SPARE:-}" ] && { echo $SPARE > $SP/svc_tcp_spare_bytes_max || { echo "no svc_tcp_spare_bytes_max in this sunrpc"; exit 1; }; }
[ -n "${QDISC:-}" ] && for p in $PORTS; do setq ${CDEV[$p]} $QDISC || { echo "qdisc not set"; exit 1; }; done
echo "this run: rcvbuf $(cat $SP/svc_tcp_rx_rcvbuf) linger $(cat $SP/svc_tcp_rx_linger_us 2>/dev/null || echo -) copy_threads $(cat $SP/svc_tcp_rx_copy_threads 2>/dev/null || echo -) lowat $(cat $SP/svc_tcp_rx_lowat 2>/dev/null || echo -) keep $(cat $SP/svc_tcp_rx_keep 2>/dev/null || echo -) want_eighths $(cat $SP/svc_tcp_rx_want_eighths 2>/dev/null || echo -) spare $(cat $SP/svc_tcp_spare_bytes_max 2>/dev/null || echo -) rings ${RINGS:-4} entries ${ENTRIES:-256} qdisc ${QDISC:-codel} nconn $NCONN jobs $JOBS bs $BS rw $RW${RWMIX:+ rwmixread $RWMIX} iodepth $IODEPTH size $SIZE io_cache_read $(cat /sys/kernel/debug/nfsd/io_cache_read)" | tee -a geometry.txt

for p in $PORTS; do
	d=${DEV[$p]}
	ip link set $d down; sleep 1
	[ -n "${RINGS:-}" ] && { ethtool -X $d equal ${RX_RINGS:-$RINGS} 2>/dev/null; ethtool -L $d combined $RINGS; }
	ip link set $d up
done
sleep 7
[ -n "${ENTRIES:-}" ] && { for p in $PORTS; do ethtool -G ${DEV[$p]} rx $ENTRIES; done; sleep 7; }
[ -n "${GRO_MAX:-}" ] && for p in $PORTS; do
	"$RIG/settled.sh" gro ${DEV[$p]} $GRO_MAX || { echo "GRO max size not set"; exit 1; }
	echo "${DEV[$p]}: gro_max_size and gro_ipv4_max_size $GRO_MAX for this run" | tee -a geometry.txt
done
[ -n "${SRV_QDISC:-}" ] && for p in $PORTS; do
	"$RIG/settled.sh" srvq ${DEV[$p]} $SRV_QDISC || { echo "server qdisc not set"; exit 1; }
	echo "${DEV[$p]}: qdisc $SRV_QDISC for this run" | tee -a geometry.txt
done
for p in $PORTS; do
	d=${DEV[$p]}
	if [ -n "${RINGS:-}${TXIRQ[$p]}" ]; then
		# The table is spread again after the open, which may reset it.
		[ -n "${RX_RINGS:-}" ] && ethtool -X $d equal $RX_RINGS
		PAIRED=${RX_RINGS:+1} "$RIG/ring-irqs.sh" $d ${IRQ0[$p]} ${TXIRQ[$p]} >> geometry.txt	# an experiment's layout
		[ -n "${RX_RINGS:-}" ] && for irq in $(awk -v n=$d-async '$NF == n {sub(/:/, "", $1); print $1}' /proc/interrupts); do
			echo $(( ${IRQ0[$p]} + 1 )) > /proc/irq/$irq/smp_affinity_list	# with Tx-0, as settled.sh places it
		done
	else
		"$RIG/settled.sh" place $p || exit 1
	fi
	if [ "${XPS:-}" = spread ]; then
		nq=$(ls -d /sys/class/net/$d/queues/tx-* | wc -l); per=$(( 48 / nq ))
		for q in /sys/class/net/$d/queues/tx-*; do
			i=${q##*-}; m=$(( ((1 << per) - 1) << (8 + i * per) ))
			printf '%08x,%08x\n' $(( (m >> 32) & 0xffffffff )) $(( m & 0xffffffff )) > $q/xps_cpus
			echo 0 > $q/xps_rxqs
		done
		echo "$p: XPS $(for q in /sys/class/net/$d/queues/tx-*; do echo -n "$(basename $q):$(cat $q/xps_cpus) "; done)" | tee -a geometry.txt
	fi
	echo "$p: $d mtu $(cat /sys/class/net/$d/mtu) rings $(ethtool -l $d | awk '/Combined/{v=$2} END{print v}') x $(ethtool -g $d | awk '/^RX:/{v=$2} END{print v}')" | tee -a geometry.txt
	# Straight after the port is opened the client's entry for the server's
	# address can still be FAILED from the down (seen 14 October): it takes a
	# few seconds to resolve again.  Up to 20 s for an answer, then refuse.
	for i in $(seq 1 10); do ping -c1 -W2 -I $d ${CLI[$p]} > /dev/null && break; done
	ping -c2 -W2 -I $d ${CLI[$p]} > /dev/null || { echo "$p: no answer from ${CLI[$p]} in 20 s"; exit 1; }
	i=0
	for fs in ${FS[$p]}; do
		i=$((i + 1))
		mountpoint -q $fs || { echo "$p: $fs is not mounted"; exit 1; }
		fstrim $fs
		mkdir -p $fs/zcbudget; chmod 777 $fs/zcbudget
		exportfs -o rw,${EXPORT_SYNC:-sync},no_root_squash,insecure,no_subtree_check,fsid=${FSID[$p]}$i "${CLI[$p]}:$fs/zcbudget" || exit 1
		cl "mkdir -p /mnt/rxr-$p$i && mount -t nfs -o vers=3,nolock,proto=tcp,sec=sys,nconnect=$NCONN,rsize=1048576,wsize=1048576 ${SRV[$p]}:$fs/zcbudget /mnt/rxr-$p$i" || { echo "$p: mount of $fs failed"; exit 1; }
	done
	NFS[$p]=$i
	echo "$p: $i filesystems: ${FS[$p]}" | tee -a geometry.txt
done
# After the placement above, which puts each port's settled deferral back.
[ -n "${DEFER:-}" ] && for p in $PORTS; do
	"$RIG/settled.sh" defer ${DEV[$p]} $DEFER || { echo "NAPI deferral not set"; exit 1; }
	echo "${DEV[$p]}: NAPI deferral $DEFER for this run" | tee -a geometry.txt
done

job() {	# job <port> <prefix> <seconds> [seconds of warm-up fio leaves out of its figures]
	local n=${NFS[$1]} per i
	per=$(( JOBS / n )); [ $per -lt 1 ] && per=1
	cat <<J
[global]
rw=${5:-$RW}
${RWMIX:+rwmixread=$RWMIX}
bs=$BS
size=$SIZE
time_based
runtime=$3
${4:+ramp_time=$4}
direct=1
ioengine=libaio
iodepth=$IODEPTH
numjobs=$per
group_reporting
${PIN:+cpus_allowed=${CPUS[$1]}}
J
	for i in $(seq 1 $n); do
		cat <<J
[$2$1$i]
directory=/mnt/rxr-$1$i
filename_format=$2-$1.\$jobnum
J
	done
}

# A drive's fast region is about 625 GB (XEU_CONVERGENCE.md, "What a drive
# takes"): what each has written from here to the end says whether the run
# stayed inside it.
wrote() { for p in $PORTS; do for fs in ${FS[$p]}; do b=$(basename $(findmnt -no SOURCE $fs)); awk -v d=$b '$3 == d {print d, $10}' /proc/diskstats; done; done; }
# The client's puppet agent off for the run, and no load that would span one
# of its runs: the placement job, the audit, the warm-up and the run, with
# room (common.sh, clear_of_puppet).  A run that reads writes its files
# first, which this does not count.
echo "client's puppet agent: $(puppet_pause | head -1)" | tee -a geometry.txt
PL=0; [ "${PLACE:-1}" != 0 ] && PL=36
# Placing the flows reads and writes the RSS table with ynl, which needs
# python3-pyyaml: on 28 October a removal of podman took that package with
# it, and seven runs went on with their flows as they fell, unseen.  Refuse
# a run that is to place its flows when the table cannot be read.
if [ "${PLACE:-1}" != 0 ]; then
	for p in $PORTS; do
		$YNL --do rss-get --json "{\"header\": {\"dev-name\": \"${DEV[$p]}\"}}" --output-json > /dev/null 2> ynl-check-$p.txt ||
			{ echo "FAILED: ynl cannot read ${DEV[$p]}'s RSS table, so the flows cannot be placed: $(tail -1 ynl-check-$p.txt)"; exit 1; }
	done
fi
clear_of_puppet $(( PL + WARM + RUNTIME + 10 )) | tee -a geometry.txt
wrote > wrote.0
# Place every connection on a ring, as run-budget.sh's PLACE=1 does: a
# short load of the same writers, each flow's hash and bytes read from the
# frames the port hands up, and each flow's entry of the RSS table pointed
# at a ring.  The mounts stay up, so the flows measured are those placed.
if [ "${PLACE:-1}" != 0 ]; then
	for p in $PORTS; do job $p bal 16 "" write | clput /tmp/twoport-bal-$p.fio; done
	for p in $PORTS; do cl "fio /tmp/twoport-bal-$p.fio > /tmp/twoport-bal-$p.out 2>&1; rm -f /mnt/rxr-$p[0-9]*/bal-*" & done
	sleep 3
	for p in $PORTS; do
		d=${DEV[$p]}
		timeout -s INT 3 bpftrace -e "tracepoint:net:napi_gro_receive_entry /str(args.name) == \"$d\"/ { @flow[args.hash, args.napi_id] = sum(args.len); }" > place-flows-$p.txt 2>&1 < /dev/null
		nr=$(ethtool -l $d | awk '/^Combined:/ {v = $2} END {print v}')
		n=$($YNL --do rss-get --json "{\"header\": {\"dev-name\": \"$d\"}}" --output-json 2>/dev/null | python3 -c 'import json, sys; print(len(json.load(sys.stdin).get("indir", [])))')
		if tbl=$("$RIG/rss-place.py" place-flows-$p.txt "${RX_RINGS:-$nr}" "$n" 13 2> place-$p.txt) &&
		   $YNL --do rss-set --json "{\"header\": {\"dev-name\": \"$d\"}, \"indir\": $tbl}" >> place-$p.txt 2>&1; then
			echo "$p: placed $(grep -c '^flow' place-$p.txt) flows" | tee -a geometry.txt
		else
			echo "$p: the RSS table was not set; flows as they fell" | tee -a geometry.txt
		fi
	done
	wait
fi

for p in $PORTS; do job $p f $RUNTIME $WARM | clput /tmp/twoport-$p.fio; done
if [ "$RW" != write ]; then	# a run that reads: every file written whole first, then a rest
	for p in $PORTS; do job $p f 0 "" write | sed -e '/^time_based/d' -e '/^runtime=/d' -e '/^rwmixread=/d' -e 's/^bs=.*/bs=8M/' | clput /tmp/twoport-fill-$p.fio; done
	for p in $PORTS; do cl "fio /tmp/twoport-fill-$p.fio > /tmp/twoport-fill-$p.out 2>&1" & done; wait
	echo "files written before the run: $(cl "du -sch /mnt/rxr-*/f-* | tail -1")" | tee -a geometry.txt
	sleep 20
fi
# After the ports are placed: settled.sh place pins nfsd to its settled CPUs.
[ -n "${NFSD_CPUS:-}" ] && { for t in $(pgrep -x nfsd); do taskset -cp $NFSD_CPUS $t > /dev/null 2>&1; done; echo "nfsd on CPUs $NFSD_CPUS for this run" | tee -a geometry.txt; }
# Just before the load: the live state, all of it, against the setting this
# run is at; anything the run set on purpose is held to what it set.
EXPENV="EXP_BUDGET=$BUDGET EXP_BUDGET_RESERVE=$RESERVE EXP_RCVBUF=${RCVBUF:-} EXP_LINGER=${LINGER:-} EXP_LOWAT=${LOWAT:-} EXP_SPARE=${SPARE:-} EXP_COPY_THREADS=${THREADS:-} EXP_KEEP=${KEEP:-} EXP_WANT_EIGHTHS=${WANT_EIGHTHS:-} EXP_NFSD_CPUS=${NFSD_CPUS:-} EXP_REFILL=${REFILL:-} EXP_SKB_DEFER_MAX=${SKB_DEFER_MAX:-} EXP_SRV_QDISC=${SRV_QDISC:-} EXP_NOTRACK=${NOTRACK:-} EXP_GRO_MAX=${GRO_MAX:-} EXP_DEFER=${DEFER:-}"
for p in $PORTS; do [ -n "${ENTRIES:-}" ] && EXPENV="$EXPENV EXP_ENTRIES_$(tr a-z A-Z <<< $p)=$ENTRIES"; done
[ -n "${RINGS:-}${TXIRQ_A:-}${TXIRQ_B:-}${XPS:-}" ] && EXPENV="$EXPENV EXP_RINGS=${RINGS:-} EXP_LAYOUT=RINGS=${RINGS:-4},RX_RINGS=${RX_RINGS:-},TXIRQ_A=${TXIRQ_A:-},TXIRQ_B=${TXIRQ_B:-},XPS=${XPS:-}"
# the client's qdisc is set only on the ports of this run
for p in $PORTS; do case "${QDISC:-}" in fq*) EXPENV="$EXPENV EXP_QDISC_$(tr a-z A-Z <<< $p)=fq" ;; codel) EXPENV="$EXPENV EXP_QDISC_$(tr a-z A-Z <<< $p)=fq_codel" ;; esac; done
env $EXPENV "$RIG/settled.sh" audit $MODE_SET > audit.txt 2>&1 || { grep -E 'BAD|FAILED' audit.txt; exit 1; }
tail -1 audit.txt | tee -a geometry.txt
echo 1 > $RG	# the region's low-water mark starts again from here
WH0=$(wr_hits); declare -A BS0; for k in admitted denied paused overrun; do BS0[$k]=$(bstat $k); done
DRIVES="$(data_drives)" "$RIG/drvsample.py" drives-sec.tsv $(( WARM + RUNTIME + 3 )) &	# every drive, each second of the load
"$RIG/wiresample.py" wire-sec.tsv $(( WARM + RUNTIME + 3 )) $(for p in $PORTS; do echo ${DEV[$p]}; done) &	# the same seconds, row for row
cl "for i in \$(seq $(( WARM + RUNTIME + 5 ))); do echo \$(date +%s.%N) \$(awk '/^Tcp:/ && !h {for (i = 1; i <= NF; i++) if (\$i == \"RetransSegs\") c = i; h = 1; next} /^Tcp:/ {print \$c}' /proc/net/snmp); sleep 1; done" > client-sec.tsv 2>&1 &
for p in $PORTS; do
	cl "fio --output-format=json --output=/tmp/twoport-$p.json /tmp/twoport-$p.fio; rm -f /mnt/rxr-$p[0-9]*/f-*" &
done
W=$(( RUNTIME - 50 )); [ $W -lt 10 ] && W=10
LEAD=25; [ $(( LEAD + W + 3 )) -gt $RUNTIME ] && LEAD=$(( (RUNTIME - W) / 2 ))	# a short run: the window in its middle
sleep $(( WARM + LEAD ))	# the warm-up, then 25 s into what fio counts
# The drives under each port's stripe over the window: sectors written and
# milliseconds busy, from /proc/diskstats.
drives() {
	local fs b m
	for fs in ${FS[$1]}; do
		b=$(basename $(findmnt -no SOURCE $fs)); m=$(ls /sys/block/$b/slaves 2> /dev/null)
		for m in ${m:-$b}; do awk -v d=$m '$3 == d {print d, $10, $13, $6}' /proc/diskstats; done
	done
}
now() { date +%s.%N; }
# WRITEs, threads woken and context switches, for what a WRITE costs in wakeups.
nfsdc() { echo "$(awk '/^proc3/{print $10}' /proc/net/rpc/nfsd) $(awk '!/^#/{s += $4} END {print s}' /proc/fs/nfsd/pool_stats) $(awk '/^ctxt/{print $2}' /proc/stat)"; }
for p in $PORTS; do drives $p > drives-$p.0; done; TD0=$(now)
ss -tmi '( sport = :2049 )' > ss-server.txt 2>&1
cl "ss -tmi '( dport = :2049 )'" > ss-client.txt 2>&1
declare -A F0 T0 B0 S0 TB0 X0
R0=$(cretr)
for p in $PORTS; do d=${DEV[$p]}; F0[$p]=$(fifo $d); T0[$p]=$(ringdrop $d); B0[$p]=$(rxbytes $d); X0[$p]=$(txbytes $d); TB0[$p]=$(now); S0[$p]=$(ringshare $d); done
N0=$(nfsdc); TN0=$(now)
mpstat -P ALL $W 1 > srv-mpstat.txt 2>&1 &
# Over the same window, each CPU's cycles and instructions, counted (not
# sampled): mpstat's %soft comes from the 100 Hz tick (no IRQ time
# accounting in this kernel), and cost.sh gives cycles a WRITE from this.
perf stat -a -A -x, -e cycles,instructions -o perf-stat-window.txt -- sleep $W > /dev/null 2>&1 &
if [ -n "${PROFILE:-}" ]; then
	# 23 s from the start of the window, so RUNTIME=55 at least.  Counters for the
	# rings' CPUs and for the rest apart, then where the cycles go, then where the
	# fetches from memory are made; each by symbol for the two groups of CPUs.
	RC=0-7,56-63; NC=8-55; EV=cycles,instructions,l3d_cache_lmiss_rd,bus_access_rd,bus_access_wr,l2d_cache_refill
	rpt() { perf report -i $1 --no-children --no-inline -g none --percent-limit ${3:-0.5} --sort symbol ${2:+--cpu $2} 2> /dev/null | grep -v '^$' | head -70; }
	( sleep 1
	  perf stat -C $RC -x, -e $EV -o perf-stat-rings.txt -- sleep 8 2> /dev/null &
	  perf stat -C $NC -x, -e $EV -o perf-stat-nfsd.txt -- sleep 8 2> /dev/null; wait
	  perf record -a -g -F 499 -o perf.data -- sleep 8 > /dev/null 2>&1
	  perf record -a -g -e l3d_cache_lmiss_rd -c 20000 -o perf-lmiss.data -- sleep 6 > /dev/null 2>&1
	  rpt perf.data > perf-sym.txt; rpt perf.data $RC > perf-sym-rings.txt; rpt perf.data $NC > perf-sym-nfsd.txt
	  perf report -i perf.data --no-children --no-inline --sort comm 2> /dev/null -g none | grep -v '^$' | head -30 > perf-comm.txt
	  perf report -i perf.data --no-children --no-inline -g caller,0.5,callee,function,percent --percent-limit 1.5 --sort symbol 2> /dev/null | grep -v '^$' | head -400 > perf-callers.txt
	  rpt perf-lmiss.data > perf-lmiss-sym.txt; rpt perf-lmiss.data $RC > perf-lmiss-rings.txt; rpt perf-lmiss.data $NC > perf-lmiss-nfsd.txt
	  perf report -i perf-lmiss.data --no-children --no-inline -g caller,0.5,callee,function,percent --percent-limit 2 --sort symbol 2> /dev/null | grep -v '^$' | head -300 > perf-lmiss-callers.txt
	  [ -n "${KEEP_PERF:-}" ] || rm -f perf.data perf-lmiss.data ) &
fi
cl "mpstat -P ALL $W 1" > cli-mpstat.txt 2>&1 &
sleep $W
N1=$(nfsdc); TN1=$(now)
for p in $PORTS; do drives $p > drives-$p.1; done; TD1=$(now)
for p in $PORTS; do
	paste drives-$p.0 drives-$p.1 | awk -v w=$(echo "$TD1 - $TD0" | bc) -v p=$p -v fs="${FS[$p]}" '
		{g = ($6 - $2) * 512 / w / 1e9; u = ($7 - $3) / w / 10; r = ($8 - $4) * 512 / w / 1e9
		 if (NR == 1 || g < gmin) gmin = g; if (g > gmax) gmax = g; if (NR == 1 || r < rmin) rmin = r; if (r > rmax) rmax = r; if (u > umax) umax = u}
		END {printf "drives of %s (%s): %d, each writing %.1f to %.1f GB/s", p, fs, NR, gmin, gmax
		     if (rmax > 0.05) printf " and reading %.1f to %.1f", rmin, rmax
		     printf ", busiest %.0f%% busy%s\n", umax, (umax >= 90 ? " -- DRIVE-LIMITED" : "")}'
done > drives.txt
{
	echo "port dev fio_GBps wire_GBps fifo_drops ring_drops ring_shares window_s"
	for p in $PORTS; do
		d=${DEV[$p]}; f1=$(fifo $d); t1=$(ringdrop $d); b1=$(rxbytes $d); tb1=$(now); s1=$(ringshare $d)
		sh=$(paste <(tr ' ' '\n' <<< "${S0[$p]}") <(tr ' ' '\n' <<< "$s1") | awk 'NF == 2 {d[NR] = $2 - $1; s += d[NR]} END {for (i = 1; i <= NR; i++) if (d[i] != "") printf "%s%d", (i > 1 ? "/" : ""), 100 * d[i] / s + 0.5}')
		echo "$p $d - $(awk -v b=$(( b1 - ${B0[$p]} )) -v w=$(echo "$tb1 - ${TB0[$p]}" | bc) 'BEGIN {printf "%.2f", b / w / 1e9}') $(( f1 - ${F0[$p]} )) $(( t1 - ${T0[$p]} )) $sh $W"
	done
} > window.txt
for p in $PORTS; do echo "sent: $p ${DEV[$p]} $(awk -v b=$(( $(txbytes ${DEV[$p]}) - ${X0[$p]} )) -v w=$(echo "$(now) - ${TB0[$p]}" | bc) 'BEGIN {printf "%.2f", b / w / 1e9}') GB/s on the wire" >> window.txt; done
R1=$(cretr)
echo "client retransmits in the window: $(( R1 - R0 ))" >> window.txt
echo "$N0 $N1" | awk -v w=$(echo "$TN1 - $TN0" | bc) '{printf "nfsd in the window, a second: WRITEs %d, threads woken %d, context switches %d\n", ($4 - $1) / w, ($5 - $2) / w, ($6 - $3) / w}' >> window.txt
wait
WH1=$(wr_hits)
echo "$WH0 $WH1" | awk '{printf "nfsd issued WRITE segments in the run: direct %d, dontcache %d, buffered %d\n", $4 - $1, $5 - $2, $6 - $3}' >> window.txt
[ -n "$BUDGET" ] && echo "loans: budget $BUDGET pages ($WRITES WRITEs), in the run admitted $(( $(bstat admitted) - ${BS0[admitted]} )), denied $(( $(bstat denied) - ${BS0[denied]} )), paused $(( $(bstat paused) - ${BS0[paused]} )), overrun $(( $(bstat overrun) - ${BS0[overrun]} ))" >> window.txt
wrote > wrote.1
paste wrote.0 wrote.1 | awk -v lim=${DRIVE_GB:-600} '{g = ($4 - $2) * 512 / 1e9; printf "%s%s %.0f", (NR > 1 ? ", " : "written in the run, GB: "), $1, g; if (g > lim) o = o " " $1} END {printf "%s\n", (o ? " -- PAST THE FAST REGION:" o : "")}' >> drives.txt
# A drive that fell under 65% of the busiest drive's median for five
# seconds running while at least 85% busy, in what fio counted, stepped
# down.  One that is slow and not busy is short of work, not slow.  The
# same in the warm-up (from its second second), against the same median,
# for three seconds running, as a warm-up of 8 to 15 s has no room for
# five: low in the warm-up.
awk -v w=$WARM 'NR == 1 {for (i = 2; i <= NF; i += 3) {split($i, x, "_"); nm[i] = x[1]}; next} $1 > w + 3 {n++; for (i in nm) {v[i, n] = $i; u[i, n] = $(i + 1)}}
	$1 >= 2 && $1 <= w + 3 {k++; t[k] = $1; for (i in nm) {wv[i, k] = $i; wu[i, k] = $(i + 1)}}
	END {for (i = 2; i in nm; i += 3) {for (j = 1; j <= n; j++) a[j] = v[i, j]; asort(a); m[i] = a[int((n + 1) / 2)]; if (m[i] > M) M = m[i]}
	     for (i = 2; i in nm; i += 3) {r = 0; s = 0
		for (j = 1; j <= n; j++) {if (v[i, j] < 0.65 * M && u[i, j] >= 85) {if (++r == 5 && !s) s = j - 4 + w + 3} else r = 0}
		if (s && m[i] > 0.5) o = o sprintf(" %s at %d s", nm[i], s)}
	     print (o ? "STEPPED DOWN:" o : "no drive stepped down")
	     for (i = 2; i in nm; i += 3) {r = 0; s = 0
		for (j = 1; j <= k; j++) {if (wv[i, j] < 0.65 * M && wu[i, j] >= 85) {if (++r == 3 && !s) s = t[j] - 2} else r = 0}
		if (s && m[i] > 0.5) l = l sprintf(" %s at %d s", nm[i], s)}
	     print (l ? "low in the warm-up:" l : "no drive low in the warm-up")}' drives-sec.tsv >> drives.txt
"$RIG/stalled.py" drives-sec.tsv wire-sec.tsv client-sec.tsv $WARM $RUNTIME >> drives.txt 2>&1
for p in $PORTS; do cl "cat /tmp/twoport-$p.json" > fio-$p.json; done
reg min_free > region-min-free.txt; cat $RG > region.txt
RW=$RW RWMIX=${RWMIX:-} BS=$BS IODEPTH=$IODEPTH JOBS=$JOBS RWTSV=${RWTSV:-} TAG=${TAG:-} python3 - $PORTS <<'EOF' | tee summary.txt
import json, os, sys
w = {}
for l in open('window.txt'):
    f = l.split()
    if len(f) == 8 and f[0] in ('a', 'b'):
        w[f[0]] = f
tot = 0; rw = {}
def lat(d):	# completion latency, mean and 99th percentile, ms
    c = d.get('clat_ns', {})
    return c.get('mean', 0) / 1e6, c.get('percentile', {}).get('99.000000', 0) / 1e6
for p in sys.argv[1:]:
    try:
        j = json.load(open('fio-%s.json' % p))['jobs'][0]
    except Exception as e:
        print(p, 'no fio result:', e); continue
    rw[p] = {d: (j[d]['bw_bytes'] / 1e9, j[d]['iops'] / 1e3) + lat(j[d]) for d in ('read', 'write')}
    bw = rw[p]['read'][0] + rw[p]['write'][0]
    tot += bw
    f = w.get(p, ['-'] * 8)
    print("port %s %s: %.2f GB/s (fio, after the warm-up); in the %s s window: wire %s GB/s, FIFO drops %s, ring drops %s, rings %s%%" % (p, f[1], bw, f[7], f[3], f[4], f[5], f[6]))
    for d in ('read', 'write'):
        if rw[p][d][1]:
            print("  %s: %.2f GB/s, %.1fk IOPS, completion %.2f ms mean, %.2f ms at the 99th percentile" % ((d,) + rw[p][d]))
print("total %.2f GB/s" % tot)
if os.environ.get('RWTSV'):
    t = os.environ['RWTSV']; e = os.environ
    if not os.path.exists(t) or not os.path.getsize(t):
        open(t, 'w').write('tag which rw rwmixread bs iodepth jobs read_GBps write_GBps read_kiops write_kiops read_ms read_p99_ms write_ms write_p99_ms\n')
    s = lambda d, i: sum(rw[p][d][i] for p in rw)
    m = lambda d, i: max([rw[p][d][i] for p in rw] or [0])
    open(t, 'a').write('%s %s %s %s %s %s %s %.2f %.2f %.1f %.1f %.2f %.2f %.2f %.2f\n' % (e.get('TAG') or os.path.basename(os.getcwd()), ''.join(sys.argv[1:]), e['RW'], e.get('RWMIX') or '-', e['BS'], e['IODEPTH'], e['JOBS'],
        s('read', 0), s('write', 0), s('read', 1), s('write', 1), m('read', 2), m('read', 3), m('write', 2), m('write', 3)))
open('total.txt', 'w').write("%.2f\n" % tot)
for l in open('window.txt'):
    if l.startswith(('client', 'nfsd', 'loans:')) or l.startswith('sent:'): print(l.strip())
print(open('drives.txt').read().strip())
# A CPU waiting on I/O is not in use: busy is what is neither idle nor iowait.
def busy(path, cpus):
    v = []
    for l in open(path):
        f = l.split()
        if len(f) > 5 and f[0] == 'Average:' and f[1].isdigit() and int(f[1]) in cpus:
            v.append(100 - float(f[-1]) - float(f[5]))
    return sum(v) / len(v) if v else float('nan')
print("server CPUs busy: rings of a (0-7) %.0f%%, rings of b (56-63) %.0f%%, nfsd (8-55) %.0f%%" % (busy('srv-mpstat.txt', range(0, 8)), busy('srv-mpstat.txt', range(56, 64)), busy('srv-mpstat.txt', range(8, 56))))
print("client CPUs busy: %.0f%%" % busy('cli-mpstat.txt', range(0, 48)))
EOF
echo "region pages in use at most: $(( $(reg pages) - $(reg min_free) ))" | tee -a summary.txt
if [ -n "${TSV:-}" ] && [ "$RW" = write ]; then
	[ -s $TSV ] || echo "tag which total_GBps a_GBps b_GBps fifo_a fifo_b ringdrop_a ringdrop_b cli_retrans region_pages nfsd_busy ringcpu_a ringcpu_b rcvbuf linger threads entries qdisc nconn jobs drvbusy_a drvbusy_b warm lowat writes_ps woken_ps ctxsw_ps keep" > $TSV
	g() { awk -v p=$1 -v c=$2 '$1 == p {print $c; f = 1} END {if (!f) print "-"}' window.txt; }	# "-" for a port not run, so the columns stay put
	db() { awk -v p=$1 '$3 == p {for (i = 1; i <= NF; i++) if ($i == "busiest") {sub(/%/, "", $(i+1)); print $(i+1); f = 1}} END {if (!f) print "-"}' drives.txt; }
	fb() { python3 -c "import json;print('%.2f' % (json.load(open('fio-$1.json'))['jobs'][0]['write']['bw_bytes']/1e9))" 2> /dev/null || echo -; }
	cb=$(awk '$1 == "Average:" && $2 ~ /^[0-9]+$/ {b = 100 - $NF - $6; c = $2 + 0; if (c < 8) a += b; else if (c >= 56) z += b; else n += b} END {printf "%.0f %.0f %.0f", n / 48, a / 8, z / 8}' srv-mpstat.txt)
	echo "${TAG:-$(basename $OUT)} $WHICH $(cat total.txt) $(fb a) $(fb b) $(g a 5) $(g b 5) $(g a 6) $(g b 6) $(awk '/^client retransmits/{print $NF}' window.txt) $(( $(reg pages) - $(reg min_free) )) $cb $(cat $SP/svc_tcp_rx_rcvbuf) $(cat $SP/svc_tcp_rx_linger_us 2>/dev/null || echo -) $(cat $SP/svc_tcp_rx_copy_threads 2>/dev/null || echo -) ${ENTRIES:-256} ${QDISC:-codel} $NCONN $JOBS $(db a) $(db b) $WARM $(cat $SP/svc_tcp_rx_lowat 2>/dev/null || echo -) $(awk '/^nfsd in the window/{gsub(",", ""); print $8, $11, $14}' window.txt) $(cat $SP/svc_tcp_rx_keep 2>/dev/null || echo -)" | sed 's/  */ /g' >> $TSV
fi
