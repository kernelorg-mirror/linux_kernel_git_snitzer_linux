#!/bin/bash
# ab-pair2.sh: FLAMEGRAPH round 153 continued.  ab-pair.sh's B, t2-r1203,
# was no control: Release 18 loaded at num_rings=8 and cut to 4 rings
# (port a 1.53 M against b's 1.83 M).  Mike, 19 November: "B is the control
# run?  meaning no change, just using settled settings", and "please fix
# the rings.. cannot have such imbalance".  And t2-r1204 (R8) posted twice
# the receive buffers: twoport.sh gives every ring 128 entries for the
# load, so the settled load has 4 x 128 a port and R8 had 8 x 128.  So:
# R8 P R8 P at 8 x 64 a port (t2-r1205 to r1208) on the load there is,
# then Release 17 back at num_rings=4 and B twice on the plain settled
# setting (t2-r1209, r1210).  packets.sh prints each server ring's frames
# a second.  On t2-r1208, 1 s of bpftrace counting xeu's polls.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh > /dev/null 2>&1
G=/var/tmp/zc-run-logs/541; K=/dev/shm/zc-xeu152/xeu-rel18.ko
U=/lib/modules/$(uname -r)/updates/xeu.ko
say() { echo "$(date +%T) $*"; }
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
	# twoport.sh sizes the rings for the load (ENTRIES, 128 by default) and
	# puts them back to 256 after: settled, a load has 4 x 128 a port, so
	# the 8-ring arms take 8 x 64, the same receive buffers posted
	R8) export EXP_RINGS=8 ENTRIES=64 EXP_ENTRIES=64 EXP_XEU_tx_in_rx_napi=N; r=8; e=64 ;;
	P)  export EXP_RINGS=8 ENTRIES=64 EXP_ENTRIES=64 EXP_XEU_tx_in_rx_napi=Y; r=8; e=64 ;;
	esac
	idle || { say "NFS connections are up: no arm change"; exit 1; }
	echo $EXP_XEU_tx_in_rx_napi > /sys/module/xeu/parameters/tx_in_rx_napi
	./step.sh $G arm-$1-a ./port-setup.sh enp161s0f0 0 $e > /dev/null || exit 1
	./step.sh $G arm-$1-b ./port-setup.sh enp164s0f0 56 $e > /dev/null || exit 1
	say "arm $1: $(./settled-iops.sh apply 2>&1 | tail -1); rings $(ethtool -l enp161s0f0 | awk '/^Combined:/ {v = $2} END {print v}') x $(ethtool -g enp161s0f0 | awk '/^RX:/ {v = $2} END {print v}'); pings $(pings)"
	dmesg | tail -50 | grep -q "tx_in_rx_napi not used" && say "WARNING: tx_in_rx_napi not used on some ring"
}
io() {	# io <n> <arm>
	if [ $2 = P ] && [ $1 = 1208 ]; then
		( for i in $(seq 600); do [ -s $L/t2-r$1/pk-srv-1.txt ] && break; sleep 0.2; done
		  bpftrace -q -e 'kprobe:xeu_rx_poll { @rx = count(); } kprobe:xeu_tx_poll { @tx = count(); } kprobe:xeu_tx_pair_intr { @pair_irq = count(); } interval:s:1 { exit(); }' > $L/t2-r$1-bpf.txt 2>&1 ) &
	fi
	./step.sh $G pk-$1 ../FLAMEGRAPH/151-packets/packets.sh $1 > $G/t2-r$1.log 2>&1
	wait
	line=$(grep -E "t2-r$1 " $G/t2-r$1.log | tail -1)
	say "$(echo "$line" | cut -d' ' -f2-)"
	echo "$line" | awk -v n=$1 -v a=$2 '{ for (i = 1; i <= NF; i++) { if ($i == "IOPS") m = $(i - 2); if ($i == "busy") { k++; if (k == 1) s = $(i + 1); if (k == 2) c = $(i + 1) } } sub("%", "", s); sub("%", "", c); printf "t2-r%s arm %s: server %.2f us, client %.2f us of CPU a READ\n", n, a, s / 100 * 64 / m, c / 100 * 48 / m }'
	grep -E "softirq_NET_RX_(ring|other)|softnet_rps_ipis_other|link_rx_pkts|^ring rx" $L/t2-r$1/packets.txt | sed 's/^/    /'
	[ -s $L/t2-r$1-bpf.txt ] && sed 's/^/    bpftrace 1 s: /' $L/t2-r$1-bpf.txt
}
for n in $(seq 1205 1210); do [ -e $L/t2-r$n ] && { say "t2-r$n exists"; exit 1; }; done
for x in 1205:R8 1206:P 1207:R8 1208:P; do
	n=${x%:*}; a=${x#*:}
	[ "$a" != "${cur:-}" ] && { arm $a; cur=$a; }
	io $n $a
done
rm -f $U && depmod -a && say "updates/xeu.ko removed: $(modinfo -n xeu)"
unset EXP_RINGS EXP_ENTRIES ENTRIES EXP_XEU_tx_in_rx_napi EXP_XEU_num_rings EXP_XEU_RELEASE
reload 4 2
./step.sh $G back-a ./port-setup.sh enp161s0f0 0 > /dev/null
./step.sh $G back-b ./port-setup.sh enp164s0f0 56 > /dev/null
say "B, Release 17: $(./settled-iops.sh apply 2>&1 | tail -1); srcversion $(cat /sys/module/xeu/srcversion); rings $(ethtool -l enp161s0f0 | awk '/^Combined:/ {v = $2} END {print v}') x $(ethtool -g enp161s0f0 | awk '/^RX:/ {v = $2} END {print v}'); pings $(pings)"
io 1209 B
io 1210 B
say "back on Release 17: $(./settled.sh apply 2>&1 | tail -1)"
say "ab-pair2 all done"
