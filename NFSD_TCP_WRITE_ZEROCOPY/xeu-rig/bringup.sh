#!/bin/bash
# bringup.sh <region pages> [ring entries] - the data drives mounted, nfsd
# started, the fixed receive region bound to every port of the host (both,
# since tardis2's second port on 2 November) and their rings set, on a fresh
# boot.  Run once per boot, before anything else on a port and before
# anything on veth.
#
# What 64 KiB taught (XEU_CONVERGENCE.md 5b): the region is named by the
# port's PCI device, not the interface; ethtool -L works only with the port
# down and ethtool -G only with it up; either remakes the pools.
. "$(dirname "$0")/common.sh"
PAGES=${1:?region pages (one region size per boot)}
ENTRIES=${2:-128}
PCI=$(basename "$(readlink -f /sys/class/net/$D/device)")
# Nothing runs on a host that could crash without a dump (Mike, 2 November).
# Build 506's first bring-up took tardis2 down with kdump not armed and left
# nothing to read.  A kernel kept in KDUMP_BOOTDIR must be a real file:
# SELinux denies kdump_t a symlink there, so the service fails at boot.
kdump_armed() {
	local k=$(uname -r) dir src
	grep -qw 'crashkernel=[^ ]*' /proc/cmdline || { echo "kdump: no crashkernel= on the kernel command line"; return 1; }
	if [ "$(cat /sys/kernel/kexec_crash_loaded)" != 1 ]; then
		dir=$(. /etc/sysconfig/kdump; echo "$KDUMP_BOOTDIR")
		src=/boot/ofp-rocky/$k/linux
		if [ -n "$dir" ] && [ -f "$src" ] && [ ! -f "$dir/vmlinuz-$k" -o -L "$dir/vmlinuz-$k" ]; then
			cp --remove-destination --preserve=timestamps "$src" "$dir/vmlinuz-$k" &&
				chmod 644 "$dir/vmlinuz-$k" && restorecon "$dir/vmlinuz-$k"
		fi
		systemctl reset-failed kdump
		systemctl restart kdump
	fi
	[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] ||
		{ echo "kdump: not armed: see 'journalctl -b -u kdump'"; return 1; }
	# Armed is not enough: a hang only reaches kdump if it panics, and these
	# are 0 at every boot.
	sysctl -q -w kernel.softlockup_panic=1 kernel.panic_on_rcu_stall=1 kernel.hung_task_panic=1 kernel.sysrq=1 ||
		{ echo "kdump: could not set the panic sysctls"; return 1; }
	echo "kdump armed: $(( $(cat /sys/kernel/kexec_crash_size) >> 20 )) MiB reserved"
}
kdump_armed || exit 1
# The drives are not in /etc/fstab, and place below needs nfsd's threads (on
# tardis1 hammerspace-nfsd-tuning starts them).  data_drives names a drive
# here only while nothing is mounted on /hs/nvmeN.
for dv in $(data_drives); do
	mp=/hs/${dv%n1}
	mountpoint -q $mp || mount -o noatime,lazytime /dev/$dv $mp || { echo "cannot mount $dv on $mp"; exit 1; }
done
if ! systemctl cat hammerspace-nfsd-tuning.service > /dev/null 2>&1; then
	systemctl start nfs-server || { echo "nfs-server did not start"; exit 1; }
fi
[ -e $RG ] && { echo "a region already exists this boot:"; cat $RG; exit 1; }
[ "$(cat /sys/module/xeu/parameters/rx_ip_align)" = 2 ] || echo "WARNING: rx_ip_align is not 2: loans will not be written direct on NVMe/TCP"
$SSHC true || { echo "client $CLIMGMT does not answer"; exit 1; }
echo "page size $PS, kernel $(uname -r), xeu $(cat /sys/module/xeu/srcversion), retry patch: $(has_retry_patch && echo present || echo ABSENT)"
# This session must not arrive over the port (it should be on the management
# interface: xemi0 on tardis1, enaxsl1i0 on tardis2).
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
# Every other port of the host joins the region: port-setup.sh names it in
# rx_region_dev with the port down, and its pools map the region when it opens.
for p in $PORTS; do
	[ "${PDEVS[$p]}" = "$D" ] && continue
	"$RIG/port-setup.sh" ${PDEVS[$p]} $([ $p = b ] && echo 56 || echo 0) $ENTRIES || { echo "port $p not set up"; exit 1; }
	ping -c2 -W2 -I ${PDEVS[$p]} ${CLIP[$p]} | tail -1
done
n=$(grep -c . <<< "$(tr ',' '\n' < $PP/rx_region_dev)")
[ "$n" = "$(wc -w <<< "$PORTS")" ] || { echo "region devices are $(cat $PP/rx_region_dev), not one for each of: $PORTS"; exit 1; }
dmesg | grep 'page_pool: ' | tail -3
echo "alloc_fail now $(reg alloc_fail): the sweep reports its change per run"
