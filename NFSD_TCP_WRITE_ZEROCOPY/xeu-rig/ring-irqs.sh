#!/bin/bash
# ring-irqs.sh <port> <first cpu> - put a port's ring interrupts on eight
# CPUs from <first cpu>: Rx-i on first + 2i, Tx-i on first + 2i + 1, for
# four rings.  xeu hints enp161s0f0's rings to CPUs 0 to 7, which are
# outside nfsd's (8-55), and enp164s0f0's to 32 to 39, which are not: its
# rings go on 56 to 63.  The hints come back when the port is taken down
# and up, so this is run after each.
d=${1:?port}; c0=${2:?first cpu}
pci=$(basename "$(readlink -f /sys/class/net/$d/device)")
nr=$(ethtool -l $d | awk '/^Combined:/ {v = $2} END {print v}')
# More than four rings: a ring's Rx and Tx share a CPU, Rx-i and Tx-i on
# first + i, so that eight rings still keep to the eight CPUs.
st=2; tx=1; [ "$nr" -gt 4 ] && { st=1; tx=0; }
for i in $(ls /sys/bus/pci/devices/$pci/msi_irqs); do
	n=$(awk -v i="$i:" '$1 == i {print $NF}' /proc/interrupts)
	case "$n" in
	$d-Rx-*) echo $(( c0 + (st * ${n##*-}) % 8 )) > /proc/irq/$i/smp_affinity_list ;;
	$d-Tx-*) echo $(( c0 + (st * ${n##*-} + tx) % 8 )) > /proc/irq/$i/smp_affinity_list ;;
	esac
done
echo "$d ring interrupts: $(for i in $(ls /sys/bus/pci/devices/$pci/msi_irqs); do n=$(awk -v i="$i:" '$1 == i {print $NF}' /proc/interrupts); case "$n" in $d-[RT]x-*) echo "${n#$d-}:$(cat /proc/irq/$i/smp_affinity_list)";; esac; done | sort | tr '\n' ' ')"
