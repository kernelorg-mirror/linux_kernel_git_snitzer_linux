#!/bin/bash
# ring-irqs.sh <port> <first cpu> - put a port's ring interrupts on eight
# CPUs from <first cpu>: Rx-i on first + 2i, Tx-i on first + 2i + 1, for
# four rings.  xeu puts every port's rings on the same CPUs, Rx-i on 2i
# and Tx-i on 2i + 1 (0 to 7 for four rings), so two ports' rings would
# share eight CPUs: enp164s0f0's go on 56 to 63, which like 0 to 7 are
# outside nfsd's (8-55).  The driver puts them back whenever the port is
# opened (down and up, ethtool -G), so this is run after each.
d=${1:?port}; c0=${2:?first cpu}
pci=$(basename "$(readlink -f /sys/class/net/$d/device)")
nr=$(ethtool -l $d | awk '/^Combined:/ {v = $2} END {print v}')
# More than four rings: a ring's Rx and Tx share a CPU, Rx-i and Tx-i on
# first + i, so that eight rings still keep to the eight CPUs.
st=2; tx=1; [ "$nr" -gt 4 ] && { st=1; tx=0; }
# A third argument, <first tx cpu>: the Rx rings have the eight CPUs to
# themselves, Rx-i on first + i, and the Tx rings go on two CPUs from
# <first tx cpu>, or on TXN CPUs from it.
txc=${3:-}; txn=${TXN:-2}
for i in $(ls /sys/bus/pci/devices/$pci/msi_irqs); do
	n=$(awk -v i="$i:" '$1 == i {print $NF}' /proc/interrupts)
	case "$n" in
	$d-Rx-*) if [ -n "$txc" ]; then echo $(( c0 + ${n##*-} % 8 )); else echo $(( c0 + (st * ${n##*-}) % 8 )); fi > /proc/irq/$i/smp_affinity_list ;;
	$d-Tx-*) if [ -n "$txc" ]; then echo $(( txc + ${n##*-} % txn )); else echo $(( c0 + (st * ${n##*-} + tx) % 8 )); fi > /proc/irq/$i/smp_affinity_list ;;
	esac
done
echo "$d ring interrupts: $(for i in $(ls /sys/bus/pci/devices/$pci/msi_irqs); do n=$(awk -v i="$i:" '$1 == i {print $NF}' /proc/interrupts); case "$n" in $d-[RT]x-*) echo "${n#$d-}:$(cat /proc/irq/$i/smp_affinity_list)";; esac; done | sort | tr '\n' ' ')"
