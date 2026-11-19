#!/bin/bash
# ab-pair.sh: FLAMEGRAPH round 153, xeu Release 18's one NAPI context a
# ring pair (tx_in_rx_napi) and eight rings of 128 a port (Mike, 19
# November: "yes, do the reloads and run round 153"; "these additional
# tunings should only be used for the IOPS pass at this point, so don't
# change READ and WRITE passes", so no passes here: they stay on the
# settled Release 17 at 4 rings; "maybe we try 8
# rings that are smaller in size so as not to expand overall pool region
# size").  Release 18 (kernel-rpmtree 081f549, built at
# /dev/shm/zc-xeu152/xeu-rel18.ko, srcversion D8F04765830566CCA26AE4B) is
# put in updates/ and loaded once with num_rings=8, so all three arms run
# on it and only reopen the ports between them:
#   B   4 rings of 256 a port, tx_in_rx_napi N (the settled layout: Rx-i
#       on c0 + 2i, Tx-i on c0 + 2i + 1)
#   R8  8 rings of 128 (the same receive buffers posted), N: Rx-i and
#       Tx-i both on c0 + i (ring-irqs.sh), two NAPI contexts a CPU
#   P   8 rings of 128, tx_in_rx_napi Y: one NAPI context a CPU
# Each run is ../151-packets/packets.sh (an IOPS run at iodepth 96 with
# the per-READ counts): B R8 P B P R8, t2-r1202 to r1207.  On P's last
# run, after the counts, 1 s of bpftrace counting xeu's poll calls, to
# show the TX polls gone.  Then Release 17 back: updates/xeu.ko removed, xeu
# reloaded at its settled num_rings=4, the settled setting.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh > /dev/null 2>&1
G=/var/tmp/zc-run-logs/541; K=/dev/shm/zc-xeu152/xeu-rel18.ko
U=/lib/modules/$(uname -r)/updates/xeu.ko
say() { echo "$(date +%T) $*"; }
for n in $(seq 1202 1207); do [ -e $L/t2-r$n ] && { say "t2-r$n exists"; exit 1; }; done
idle() { [ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ]; }
pings() { for p in a b; do ping -c2 -W2 -I ${PDEVS[$p]} ${CLIP[$p]} > /dev/null && echo "$p ok" || echo "$p NO REPLY"; done | paste -sd' '; }
reload() {	# reload <num_rings>
	idle || { say "NFS connections are up: no reload"; exit 1; }
	./step.sh $G xeu-rmmod-$2 rmmod xeu || exit 1
	./step.sh $G xeu-modprobe-$2 modprobe xeu num_rings=$1 || exit 1
	sleep 10
	say "xeu loaded: srcversion $(cat /sys/module/xeu/srcversion), num_rings $(cat /sys/module/xeu/parameters/num_rings)"
}
arm() {	# arm B|R8|P: the arm's environment, the ports opened again for it
	export EXP_XEU_num_rings=8 EXP_XEU_RELEASE=18
	case $1 in
	B)  unset EXP_RINGS EXP_ENTRIES; export EXP_XEU_tx_in_rx_napi=N; r=4; e=256 ;;
	R8) export EXP_RINGS=8 EXP_ENTRIES=128 EXP_XEU_tx_in_rx_napi=N; r=8; e=128 ;;
	P)  export EXP_RINGS=8 EXP_ENTRIES=128 EXP_XEU_tx_in_rx_napi=Y; r=8; e=128 ;;
	esac
	idle || { say "NFS connections are up: no arm change"; exit 1; }
	echo $EXP_XEU_tx_in_rx_napi > /sys/module/xeu/parameters/tx_in_rx_napi
	./step.sh $G arm-$1-a ./port-setup.sh enp161s0f0 0 $e > /dev/null || exit 1
	./step.sh $G arm-$1-b ./port-setup.sh enp164s0f0 56 $e > /dev/null || exit 1
	say "arm $1: $(./settled-iops.sh apply 2>&1 | tail -1); rings $(ethtool -l enp161s0f0 | awk '/^Combined:/ {v = $2} END {print v}') x $(ethtool -g enp161s0f0 | awk '/^RX:/ {v = $2} END {print v}'); pings $(pings)"
	dmesg | tail -50 | grep -q "tx_in_rx_napi not used" && say "WARNING: tx_in_rx_napi not used on some ring"
}
io() {	# io <n> <arm>
	if [ $2 = P ] && [ $1 = 1206 ]; then
		( for i in $(seq 600); do [ -s $L/t2-r$1/pk-srv-1.txt ] && break; sleep 0.2; done
		  bpftrace -q -e 'kprobe:xeu_rx_poll { @rx = count(); } kprobe:xeu_tx_poll { @tx = count(); } kprobe:xeu_tx_pair_intr { @pair_irq = count(); } interval:s:1 { exit(); }' > $L/t2-r$1-bpf.txt 2>&1 ) &
	fi
	./step.sh $G pk-$1 ../FLAMEGRAPH/151-packets/packets.sh $1 > $G/t2-r$1.log 2>&1
	wait
	line=$(grep -E "t2-r$1 " $G/t2-r$1.log | tail -1)
	say "$(echo "$line" | cut -d' ' -f2-)"
	echo "$line" | awk -v n=$1 -v a=$2 '{ for (i = 1; i <= NF; i++) { if ($i == "IOPS") m = $(i - 2); if ($i == "busy") { k++; if (k == 1) s = $(i + 1); if (k == 2) c = $(i + 1) } } sub("%", "", s); sub("%", "", c); printf "t2-r%s arm %s: server %.2f us, client %.2f us of CPU a READ\n", n, a, s / 100 * 64 / m, c / 100 * 48 / m }'
	grep -E "softirq_NET_RX_(ring|other)|softnet_rps_ipis_other|link_rx_pkts" $L/t2-r$1/packets.txt | sed 's/^/    /'
	[ -s $L/t2-r$1-bpf.txt ] && sed 's/^/    bpftrace 1 s: /' $L/t2-r$1-bpf.txt
}
say "pings before: $(pings)"
cp $K $U && depmod -a && say "Release 18 in updates/: $(modinfo -F srcversion xeu)"
reload 8 1
for x in 1202:B 1203:R8 1204:P 1205:B 1206:P 1207:R8; do
	n=${x%:*}; a=${x#*:}
	[ "$a" != "${cur:-}" ] && { arm $a; cur=$a; }
	io $n $a
done
rm -f $U && depmod -a && say "updates/xeu.ko removed: $(modinfo -n xeu)"
unset EXP_RINGS EXP_ENTRIES EXP_XEU_tx_in_rx_napi EXP_XEU_num_rings EXP_XEU_RELEASE
reload 4 2
./step.sh $G back-a ./port-setup.sh enp161s0f0 0 > /dev/null
./step.sh $G back-b ./port-setup.sh enp164s0f0 56 > /dev/null
say "back on Release 17: $(./settled.sh apply 2>&1 | tail -1); srcversion $(cat /sys/module/xeu/srcversion); pings $(pings)"
say "ab-pair all done"
