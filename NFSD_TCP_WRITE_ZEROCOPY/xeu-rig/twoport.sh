#!/bin/bash
# twoport.sh <out dir> [a|b|ab] - the pinned copy load on one port, the
# other, or both at once, each port onto its own stripe from its own mount
# on the client.  sweep.sh and run-budget.sh drive one port and keep the
# server's settings to themselves, so two of them cannot run together;
# this is the same load (16 connections, 16 writers of 8 MiB direct writes
# for 120 s, every connection placed on a ring) set up once for both.
#
#   a   enp161s0f0  192.168.1.211  /hs/stripe/zcbudget   client ens3np0
#   b   enp164s0f0  192.168.0.211  /hs/stripe1/zcbudget  client ens6np0
#
# It expects what the pinned configuration leaves: svc_tcp_rx_rcvbuf,
# svc_tcp_rx_linger_us and io_cache_write as pinned, both ports set up by
# port-setup.sh (MTU 7792, 4 rings of 256, in the region).  It takes each
# port down and up first, which zeroes its rings' counters, and puts the
# rings' interrupts back on their CPUs afterwards.
# RUNTIME=<s> (120), JOBS=<n> (16), BS (8M), NCONN (16), PLACE=0 to leave
# the flows where they fall.  For a run and put back after it:
#   RCVBUF=<bytes>   svc_tcp_rx_rcvbuf, the held receive buffer
#   LINGER=<us>      svc_tcp_rx_linger_us
#   THREADS=<n>      svc_tcp_rx_copy_threads
#   ENTRIES=<n>      entries of each RX ring, on the ports used
#   RINGS=<n>        rings of each port used (4); over 4, Rx and Tx share CPUs
#   QDISC=fq|fqNN    fq on the client's ports (fqNN: maxrate NN Gbit/s a flow)
#   PROFILE=1        a 10 s system-wide perf record and perf stat mid-run
#   PIN=1            each port's writers on the client CPUs of its NIC's NUMA node
#   WARM=<s>         the writers run this long before fio starts counting (40)
#   EXPORT_SYNC=async  export async, so a COMMIT does not sync the file or flush the drives
# TSV=<file>: append a line of the run's figures (TAG=<label> names it).
. "$(dirname "$0")/common.sh"
"$(dirname "$0")/drive-cache.sh" > /dev/null || { echo "drive-cache.sh failed"; exit 1; }	# no cache flush to the drives (AGENTS.md)
OUT=${1:?out dir}; WHICH=${2:-ab}
RUNTIME=${RUNTIME:-120}; WARM=${WARM:-40}; JOBS=${JOBS:-16}; BS=${BS:-8M}; NCONN=${NCONN:-16}
YNL="$H/../tools/net/ynl/pyynl/cli.py --no-schema --spec $H/../Documentation/netlink/specs/ethtool.yaml"
declare -A DEV=([a]=enp161s0f0 [b]=enp164s0f0) SRV=([a]=192.168.1.211 [b]=192.168.0.211)
declare -A CLI=([a]=192.168.1.106 [b]=192.168.0.106) FS=([a]=/hs/stripe [b]=/hs/stripe1)
declare -A IRQ0=([a]=0 [b]=56) FSID=([a]=7701 [b]=7702)
declare -A CPUS=([a]=0-23 [b]=24-47)	# the client's CPUs on each port's NUMA node, for PIN=1
PORTS=$(sed 's/./& /g' <<< "$WHICH")
SSHC="ssh -o BatchMode=yes -o ConnectTimeout=10 -i $KEY root@${CLI[a]}"
cl() { $SSHC "$@" < /dev/null; }
clput() { $SSHC "cat > $1"; }	# stdin to a file on the client

[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || { echo "NFS connections are up: not ours to disturb"; exit 1; }
mkdir -p $OUT; cd $OUT || exit 1
fifo() { ethtool -S $1 | awk '/port_rx_fifo_tc[0-9]_drop_packets/{s+=$2} END{print s+0}'; }
ringdrop() { ethtool -S $1 | awk '/rx_hw_tail_drop_packets/{s+=$2} END{print s+0}'; }
rxbytes() { ethtool -S $1 | awk '$1 ~ /^ring_[0-9]+_rx_bytes:$/{s+=$2} END{print s+0}'; }
ringshare() { ethtool -S $1 | awk '$1 ~ /^ring_[0-9]+_rx_bytes:$/{print $2}' | tr '\n' ' '; }
cretr() { cl "awk '/^Tcp:/ && !h {for (i = 1; i <= NF; i++) if (\$i == \"RetransSegs\") c = i; h = 1; next} /^Tcp:/ {print \$c}' /proc/net/snmp"; }

SP=/sys/module/sunrpc/parameters
declare -A SAVE
for k in rcvbuf linger_us copy_threads; do SAVE[$k]=$(cat $SP/svc_tcp_rx_$k); done
declare -A CDEV=([a]=ens3np0 [b]=ens6np0)
setq() {	# setq <client dev> <codel|fq|fqNN>
	case $2 in
	codel) cl "tc qdisc del dev $1 root 2>/dev/null; true" ;;
	fq)    cl "tc qdisc replace dev $1 root handle 1: mq && for i in \$(seq 1 48); do tc qdisc replace dev $1 parent 1:\$(printf %x \$i) fq || exit 1; done" ;;
	fq*)   cl "tc qdisc replace dev $1 root handle 1: mq && for i in \$(seq 1 48); do tc qdisc replace dev $1 parent 1:\$(printf %x \$i) fq maxrate ${2#fq}gbit || exit 1; done" ;;
	esac
}
cleanup() {
	for k in rcvbuf linger_us copy_threads; do echo ${SAVE[$k]} > $SP/svc_tcp_rx_$k; done
	[ -n "${QDISC:-}" ] && for p in $PORTS; do setq ${CDEV[$p]} codel; done
	# The RSS table may name rings that are going: spread it over four first.
	[ -n "${RINGS:-}" ] && { for p in $PORTS; do ethtool -X ${DEV[$p]} equal 4 2>/dev/null; ip link set ${DEV[$p]} down; sleep 1; ethtool -L ${DEV[$p]} combined 4; ip link set ${DEV[$p]} up; done; sleep 7; for p in $PORTS; do "$RIG/ring-irqs.sh" ${DEV[$p]} ${IRQ0[$p]} > /dev/null; done; }
	[ -n "${ENTRIES:-}${RINGS:-}" ] && for p in $PORTS; do ethtool -G ${DEV[$p]} rx 256 2>/dev/null; done
	# Resizing the rings puts their interrupts back where the driver hints
	# (both ports on CPUs 0-7): place them again.
	[ -n "${ENTRIES:-}${RINGS:-}" ] && { sleep 7; for p in $PORTS; do "$RIG/ring-irqs.sh" ${DEV[$p]} ${IRQ0[$p]} > /dev/null; done; }
	for p in $PORTS; do
		cl "rm -f /mnt/rxr-$p/f-* /mnt/rxr-$p/bal-* 2>/dev/null; umount -f /mnt/rxr-$p 2>/dev/null"
		exportfs -u "${CLI[$p]}:${FS[$p]}/zcbudget" 2>/dev/null
	done
	echo N > /sys/module/xeu/parameters/rx_port_stats
}
trap cleanup EXIT

echo "twoport $WHICH: kernel $(uname -r), xeu $(cat /sys/module/xeu/srcversion), sunrpc $(cat /sys/module/sunrpc/srcversion)" | tee geometry.txt
echo "rcvbuf $(cat /sys/module/sunrpc/parameters/svc_tcp_rx_rcvbuf) linger $(cat /sys/module/sunrpc/parameters/svc_tcp_rx_linger_us) copy_threads $(cat /sys/module/sunrpc/parameters/svc_tcp_rx_copy_threads) io_cache_write $(cat /sys/kernel/debug/nfsd/io_cache_write) nfsd threads $(cat /proc/fs/nfsd/threads) rx_refill_thresh $(cat /sys/module/xeu/parameters/rx_refill_thresh) region dev $(cat $PP/rx_region_dev)" | tee -a geometry.txt
echo Y > /sys/module/xeu/parameters/rx_port_stats
[ -n "${RCVBUF:-}" ] && echo $RCVBUF > $SP/svc_tcp_rx_rcvbuf
[ -n "${LINGER:-}" ] && echo $LINGER > $SP/svc_tcp_rx_linger_us
[ -n "${THREADS:-}" ] && echo $THREADS > $SP/svc_tcp_rx_copy_threads
[ -n "${QDISC:-}" ] && for p in $PORTS; do setq ${CDEV[$p]} $QDISC || { echo "qdisc not set"; exit 1; }; done
echo "this run: rcvbuf $(cat $SP/svc_tcp_rx_rcvbuf) linger $(cat $SP/svc_tcp_rx_linger_us) copy_threads $(cat $SP/svc_tcp_rx_copy_threads) rings ${RINGS:-4} entries ${ENTRIES:-256} qdisc ${QDISC:-codel} nconn $NCONN jobs $JOBS bs $BS" | tee -a geometry.txt

for p in $PORTS; do
	d=${DEV[$p]}
	ip link set $d down; sleep 1
	[ -n "${RINGS:-}" ] && { ethtool -X $d equal $RINGS 2>/dev/null; ethtool -L $d combined $RINGS; }
	ip link set $d up
done
sleep 7
[ -n "${ENTRIES:-}" ] && { for p in $PORTS; do ethtool -G ${DEV[$p]} rx $ENTRIES; done; sleep 7; }
for p in $PORTS; do
	d=${DEV[$p]}
	"$RIG/ring-irqs.sh" $d ${IRQ0[$p]} >> geometry.txt
	echo "$p: $d mtu $(cat /sys/class/net/$d/mtu) rings $(ethtool -l $d | awk '/Combined/{v=$2} END{print v}') x $(ethtool -g $d | awk '/^RX:/{v=$2} END{print v}')" | tee -a geometry.txt
	ping -c2 -W2 -I $d ${CLI[$p]} > /dev/null || { echo "$p: no answer from ${CLI[$p]}"; exit 1; }
	fstrim ${FS[$p]}
	mkdir -p ${FS[$p]}/zcbudget; chmod 777 ${FS[$p]}/zcbudget
	exportfs -o rw,${EXPORT_SYNC:-sync},no_root_squash,insecure,no_subtree_check,fsid=${FSID[$p]} "${CLI[$p]}:${FS[$p]}/zcbudget" || exit 1
	cl "mkdir -p /mnt/rxr-$p && mount -t nfs -o vers=3,nolock,proto=tcp,sec=sys,nconnect=$NCONN,rsize=1048576,wsize=1048576 ${SRV[$p]}:${FS[$p]}/zcbudget /mnt/rxr-$p" || { echo "$p: mount failed"; exit 1; }
done

job() {	# job <port> <prefix> <seconds> [seconds of warm-up fio leaves out of its figures]
	cat <<J
[global]
rw=write
bs=$BS
size=512M
time_based
runtime=$3
${4:+ramp_time=$4}
direct=1
ioengine=libaio
iodepth=16
numjobs=$JOBS
group_reporting
${PIN:+cpus_allowed=${CPUS[$1]}}
[$2$1]
directory=/mnt/rxr-$1
filename_format=$2-$1.\$jobnum
J
}

# Place every connection on a ring, as run-budget.sh's PLACE=1 does: a
# short load of the same writers, each flow's hash and bytes read from the
# frames the port hands up, and each flow's entry of the RSS table pointed
# at a ring.  The mounts stay up, so the flows measured are those placed.
if [ "${PLACE:-1}" != 0 ]; then
	for p in $PORTS; do job $p bal 16 | clput /tmp/twoport-bal-$p.fio; done
	for p in $PORTS; do cl "fio /tmp/twoport-bal-$p.fio > /tmp/twoport-bal-$p.out 2>&1; rm -f /mnt/rxr-$p/bal-*" & done
	sleep 3
	for p in $PORTS; do
		d=${DEV[$p]}
		timeout -s INT 3 bpftrace -e "tracepoint:net:napi_gro_receive_entry /str(args.name) == \"$d\"/ { @flow[args.hash, args.napi_id] = sum(args.len); }" > place-flows-$p.txt 2>&1 < /dev/null
		nr=$(ethtool -l $d | awk '/^Combined:/ {v = $2} END {print v}')
		n=$($YNL --do rss-get --json "{\"header\": {\"dev-name\": \"$d\"}}" --output-json 2>/dev/null | python3 -c 'import json, sys; print(len(json.load(sys.stdin).get("indir", [])))')
		if tbl=$("$RIG/rss-place.py" place-flows-$p.txt "$nr" "$n" 13 2> place-$p.txt) &&
		   $YNL --do rss-set --json "{\"header\": {\"dev-name\": \"$d\"}, \"indir\": $tbl}" >> place-$p.txt 2>&1; then
			echo "$p: placed $(grep -c '^flow' place-$p.txt) flows" | tee -a geometry.txt
		else
			echo "$p: the RSS table was not set; flows as they fell" | tee -a geometry.txt
		fi
	done
	wait
fi

for p in $PORTS; do job $p f $RUNTIME $WARM | clput /tmp/twoport-$p.fio; done
echo 1 > $RG	# the region's low-water mark starts again from here
for p in $PORTS; do
	cl "fio --output-format=json --output=/tmp/twoport-$p.json /tmp/twoport-$p.fio; rm -f /mnt/rxr-$p/f-*" &
done
W=$(( RUNTIME - 50 )); [ $W -lt 10 ] && W=10
sleep $(( WARM + 25 ))	# the warm-up, then 25 s into what fio counts
# The drives under each port's stripe over the window: sectors written and
# milliseconds busy, from /proc/diskstats.
drives() { for m in $(ls /sys/block/$(basename $(findmnt -no SOURCE ${FS[$1]}))/slaves); do awk -v d=$m '$3 == d {print d, $10, $13}' /proc/diskstats; done; }
for p in $PORTS; do drives $p > drives-$p.0; done
ss -tmi '( sport = :2049 )' > ss-server.txt 2>&1
cl "ss -tmi '( dport = :2049 )'" > ss-client.txt 2>&1
declare -A F0 T0 B0 S0
for p in $PORTS; do d=${DEV[$p]}; F0[$p]=$(fifo $d); T0[$p]=$(ringdrop $d); B0[$p]=$(rxbytes $d); S0[$p]=$(ringshare $d); done
R0=$(cretr)
mpstat -P ALL $W 1 > srv-mpstat.txt 2>&1 &
if [ -n "${PROFILE:-}" ]; then
	( sleep 10
	  perf stat -a -x, -e cycles,instructions,l3d_cache_lmiss_rd,l3d_cache_refill,bus_access_rd,bus_access_wr,l2d_cache_refill -o perf-stat.txt -- sleep 10 2> /dev/null
	  perf record -a -g -F 199 -o perf.data -- sleep 10 > /dev/null 2>&1
	  perf report -i perf.data --no-children --no-inline -g none --percent-limit 0.7 --sort symbol 2> /dev/null | grep -v '^$' | head -60 > perf-sym.txt
	  rm -f perf.data ) &
fi
cl "mpstat -P ALL $W 1" > cli-mpstat.txt 2>&1 &
sleep $W
for p in $PORTS; do drives $p > drives-$p.1; done
R1=$(cretr)
for p in $PORTS; do
	paste drives-$p.0 drives-$p.1 | awk -v w=$W -v p=$p -v fs=${FS[$p]} '
		{g = ($5 - $2) * 512 / w / 1e9; u = ($6 - $3) / w / 10; if (NR == 1 || g < gmin) gmin = g; if (g > gmax) gmax = g; if (u > umax) umax = u}
		END {printf "drives of %s (%s): %d, each writing %.1f to %.1f GB/s, busiest %.0f%% busy%s\n", p, fs, NR, gmin, gmax, umax, (umax >= 90 ? " -- DRIVE-LIMITED" : "")}'
done > drives.txt
{
	echo "port dev fio_GBps wire_GBps fifo_drops ring_drops ring_shares window_s"
	for p in $PORTS; do
		d=${DEV[$p]}; f1=$(fifo $d); t1=$(ringdrop $d); b1=$(rxbytes $d); s1=$(ringshare $d)
		sh=$(paste <(tr ' ' '\n' <<< "${S0[$p]}") <(tr ' ' '\n' <<< "$s1") | awk 'NF == 2 {d[NR] = $2 - $1; s += d[NR]} END {for (i = 1; i <= NR; i++) if (d[i] != "") printf "%s%d", (i > 1 ? "/" : ""), 100 * d[i] / s + 0.5}')
		echo "$p $d - $(awk -v b=$(( b1 - ${B0[$p]} )) -v w=$W 'BEGIN {printf "%.2f", b / w / 1e9}') $(( f1 - ${F0[$p]} )) $(( t1 - ${T0[$p]} )) $sh $W"
	done
} > window.txt
echo "client retransmits in the window: $(( R1 - R0 ))" >> window.txt
wait
for p in $PORTS; do cl "cat /tmp/twoport-$p.json" > fio-$p.json; done
reg min_free > region-min-free.txt; cat $RG > region.txt
python3 - $PORTS <<'EOF' | tee summary.txt
import json, sys
w = {}
for l in open('window.txt'):
    f = l.split()
    if len(f) == 8 and f[0] in ('a', 'b'):
        w[f[0]] = f
tot = 0
for p in sys.argv[1:]:
    try:
        j = json.load(open('fio-%s.json' % p))['jobs'][0]['write']
        bw = j['bw_bytes'] / 1e9
    except Exception as e:
        print(p, 'no fio result:', e); continue
    tot += bw
    f = w.get(p, ['-'] * 8)
    print("port %s %s: %.2f GB/s (fio, after the warm-up); in the %s s window: wire %s GB/s, FIFO drops %s, ring drops %s, rings %s%%" % (p, f[1], bw, f[7], f[3], f[4], f[5], f[6]))
print("total %.2f GB/s" % tot)
open('total.txt', 'w').write("%.2f\n" % tot)
for l in open('window.txt'):
    if l.startswith('client'): print(l.strip())
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
if [ -n "${TSV:-}" ]; then
	[ -s $TSV ] || echo "tag which total_GBps a_GBps b_GBps fifo_a fifo_b ringdrop_a ringdrop_b cli_retrans region_pages nfsd_busy ringcpu_a ringcpu_b rcvbuf linger threads entries qdisc nconn jobs drvbusy_a drvbusy_b warm" > $TSV
	g() { awk -v p=$1 -v c=$2 '$1 == p {print $c}' window.txt; }
	db() { awk -v p=$1 '$3 == p {for (i = 1; i <= NF; i++) if ($i == "busiest") {sub(/%/, "", $(i+1)); print $(i+1); f = 1}} END {if (!f) print "-"}' drives.txt; }
	fb() { python3 -c "import json;print('%.2f' % (json.load(open('fio-$1.json'))['jobs'][0]['write']['bw_bytes']/1e9))" 2> /dev/null || echo -; }
	cb=$(awk '$1 == "Average:" && $2 ~ /^[0-9]+$/ {b = 100 - $NF - $6; c = $2 + 0; if (c < 8) a += b; else if (c >= 56) z += b; else n += b} END {printf "%.0f %.0f %.0f", n / 48, a / 8, z / 8}' srv-mpstat.txt)
	echo "${TAG:-$(basename $OUT)} $WHICH $(cat total.txt) $(fb a) $(fb b) $(g a 5) $(g b 5) $(g a 6) $(g b 6) $(awk '/^client retransmits/{print $NF}' window.txt) $(( $(reg pages) - $(reg min_free) )) $cb $(cat $SP/svc_tcp_rx_rcvbuf) $(cat $SP/svc_tcp_rx_linger_us) $(cat $SP/svc_tcp_rx_copy_threads) ${ENTRIES:-256} ${QDISC:-codel} $NCONN $JOBS $(db a) $(db b) $WARM" | sed 's/  */ /g' >> $TSV
fi
