#!/bin/bash
# port-setup.sh <port> <first irq cpu> [ring entries] - give a second port
# the pinned settings the first has: MTU 7792, 4 rings of 256, its pools in
# the fixed receive region with the first port's, and its rings'
# interrupts on their own CPUs.  For enp164s0f0:  port-setup.sh enp164s0f0 56
# Nothing here outlives a boot or a reload of xeu.
. "$(dirname "$0")/common.sh"
d=${1:?port}; c0=${2:?first irq cpu}; n=${3:-256}
[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || { echo "NFS connections are up: not ours to disturb"; exit 1; }
pci=$(basename "$(readlink -f /sys/class/net/$d/device)")
cur=$(cat $PP/rx_region_dev)
( sleep 300; ip link set $d up ) > /dev/null 2>&1 & DM=$!
ip link set $d down
ethtool -L $d combined 4
case ",$cur," in *",$pci,"*) ;; *) echo "${cur:+$cur,}$pci" > $PP/rx_region_dev ;; esac
ip link set $d mtu 7792
ip link set $d up; sleep 5
ethtool -G $d rx $n; sleep 6
kill $DM 2>/dev/null
"$RIG/ring-irqs.sh" $d $c0
echo "$d: mtu $(cat /sys/class/net/$d/mtu), rings $(ethtool -l $d | awk '/Combined/{v=$2} END{print v}') x $(ethtool -g $d | awk '/^RX:/{v=$2} END{print v}'), region dev $(cat $PP/rx_region_dev)"
cat $RG
