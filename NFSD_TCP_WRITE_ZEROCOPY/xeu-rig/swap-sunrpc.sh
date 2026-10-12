#!/bin/bash
# swap-sunrpc.sh [builddir] - put the sunrpc.ko that build-sunrpc.sh built in
# place of the running one, without a reboot.  It stops nfsd and everything
# else that uses sunrpc, unloads sunrpc and every module that depends on it,
# installs the new module in updates/ (the kernel's own sunrpc.ko.xz is left
# where it is; depmod searches updates/ first) and starts the services again.
#
# Every NFS client's connections are dropped, on both ports: this is Mike's
# to ask for.  Nothing is installed until sunrpc has unloaded, so if a module
# will not unload the services are started again on the old one.
#
# POOLS_PER_NODE=n loads the new module with pools_per_node=n (it is read
# when nfsd starts).  To go back: rm updates/sunrpc.ko, depmod -a, and run
# this again with SUNRPC_KO=none.
. "$(dirname "$0")/common.sh"
O=${1:-$L/sunrpc-$(uname -r)}
KO=${SUNRPC_KO:-$O/sunrpc/sunrpc.ko}
UPD=/lib/modules/$(uname -r)/updates
UNITS="nfs-server nfs-mountd nfs-idmapd nfsdcld rpc-statd rpc-gssd gssproxy"
# oneshot units that stay "active" once run, so they are restarted by name:
# the block size before nfsd starts, the tuning after
BEFORE_NFSD="hs-nfsd-blksize"
AFTER_NFSD="hammerspace-nfsd-tuning"

[ "$KO" = none ] || [ -e "$KO" ] || { echo "no $KO: run build-sunrpc.sh"; exit 1; }
if findmnt -t nfs,nfs4 -no TARGET | grep -q .; then
	echo "NFS mounts on this host; unmount them first:"; findmnt -t nfs,nfs4 -no TARGET; exit 1
fi

# A client with connections up would lose them, and a run in progress its
# server: refuse unless told that is meant.
n=$(ss -Htn state established '( sport = :2049 )' | wc -l)
if [ "$n" -gt 0 ] && [ -z "${DROP_CLIENTS:-}" ]; then
	echo "$n NFS connections are up; DROP_CLIENTS=1 to drop them"; exit 1
fi

was=$(for u in $UNITS; do systemctl is-active -q $u && echo $u; done)
echo "running sunrpc: $(cat /sys/module/sunrpc/srcversion); active: "$was

start() {
	systemctl start proc-fs-nfsd.mount var-lib-nfs-rpc_pipefs.mount
	for u in $BEFORE_NFSD; do systemctl is-enabled -q $u 2>/dev/null && systemctl restart $u; done
	for u in gssproxy rpc-statd nfsdcld nfs-idmapd nfs-mountd nfs-server; do
		case " "$was" " in *" $u "*) systemctl start $u || echo "$u did not start" ;; esac
	done
	for u in $AFTER_NFSD; do systemctl is-enabled -q $u 2>/dev/null && systemctl restart $u; done
}

systemctl stop $UNITS
umount /proc/fs/nfsd 2>/dev/null
umount /var/lib/nfs/rpc_pipefs 2>/dev/null

# unload whatever holds sunrpc, leaves first, until nothing does
for pass in 1 2 3 4 5 6 7 8; do
	[ -d /sys/module/sunrpc ] || break
	progress=0
	for m in $(lsmod | awk 'NR > 1 && $3 == 0 {print $1}'); do
		[ $m = sunrpc ] && continue
		modinfo -F depends $m 2>/dev/null | tr ',' '\n' |
			grep -qxE 'sunrpc|nfs|nfsd|lockd|auth_rpcgss|nfsv4|nfs_acl|nfs_localio|grace' || continue
		rmmod $m && progress=1
	done
	[ -z "$(ls /sys/module/sunrpc/holders)" ] && rmmod sunrpc && break
	[ $progress = 1 ] || break
done
if [ -d /sys/module/sunrpc ]; then
	echo "sunrpc did not unload; nothing was installed.  Still loaded:"
	lsmod | awk '$1 == "sunrpc" || $4 ~ /sunrpc/'
	start
	exit 1
fi

if [ "$KO" != none ]; then
	mkdir -p $UPD
	cp $KO $UPD/sunrpc.ko
fi
depmod -a
echo "modprobe will load: $(modinfo -n sunrpc)"
modprobe sunrpc ${POOLS_PER_NODE:+pools_per_node=$POOLS_PER_NODE} || { echo "the new sunrpc did not load"; exit 1; }
start

echo "running sunrpc: $(cat /sys/module/sunrpc/srcversion)"
for p in pools_per_node svc_tcp_rx_linger_us; do
	[ -e /sys/module/sunrpc/parameters/$p ] && echo "$p $(cat /sys/module/sunrpc/parameters/$p)"
done
echo "nfsd threads: $(cat /proc/fs/nfsd/threads 2>&1); pools: $(cat /proc/fs/nfsd/pool_threads 2>&1)"
