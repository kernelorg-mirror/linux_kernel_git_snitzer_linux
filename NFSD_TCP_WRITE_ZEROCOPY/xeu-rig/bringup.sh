#!/bin/bash
# bringup.sh <region pages> [ring entries] - bind the fixed receive region to
# enp161s0f0 on a fresh boot and set its rings.  Run once per boot, before
# anything else on the port and before anything on veth.
#
# What 64 KiB taught (XEU_CONVERGENCE.md 5b): the region is named by the
# port's PCI device, not the interface; ethtool -L works only with the port
# down and ethtool -G only with it up; either remakes the pools.
. "$(dirname "$0")/common.sh"
PAGES=${1:?region pages (one region size per boot)}
ENTRIES=${2:-128}
PCI=$(basename "$(readlink -f /sys/class/net/$D/device)")
[ -e $RG ] && { echo "a region already exists this boot:"; cat $RG; exit 1; }
[ "$(cat /sys/module/xeu/parameters/rx_ip_align)" = 2 ] || echo "WARNING: rx_ip_align is not 2: loans will not be written direct on NVMe/TCP"
$SSHC true || { echo "client $CLIMGMT does not answer"; exit 1; }
echo "page size $PS, kernel $(uname -r), xeu $(cat /sys/module/xeu/srcversion), retry patch: $(has_retry_patch && echo present || echo ABSENT)"
# This session must not arrive over the port (it should be on xemi0).
ip route get "${SSH_CONNECTION%% *}" 2>/dev/null | grep -q "dev $D" && { echo "this session arrives on $D"; exit 1; }
# Deadman: put the port back if this script dies with it down.
( sleep 300; ip link set $D up ) > /dev/null 2>&1 & DM=$!
ip link set $D down
ethtool -L $D combined 4
echo "$PCI" > $PP/rx_region_dev
echo "$PAGES" > $PP/rx_region_pages
ip link set $D up; sleep 5
ethtool -G $D rx $ENTRIES; sleep 6
kill $DM 2>/dev/null
ping -c2 -W2 $CLIIP | tail -1
echo "rings $(rx_rings) x $(rx_entries)"
cat $RG || { echo "NO REGION: check 'dmesg | grep page_pool'"; exit 1; }
"$RIG/settled.sh" place a	# the settled CPU layout for nfsd and the port
dmesg | grep 'page_pool: ' | tail -2
[ "$(reg dev)" = "$PCI" ] && [ "$(reg pages)" = "$PAGES" ] || { echo "region is not as asked"; exit 1; }
echo "alloc_fail now $(reg alloc_fail): the sweep reports its change per run"
