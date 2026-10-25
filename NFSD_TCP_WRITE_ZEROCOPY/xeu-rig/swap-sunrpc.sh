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
# To go back: rm updates/sunrpc.ko, depmod -a, and run
# this again with SUNRPC_KO=none (and NFSD_KO=none for an nfsd.ko put there).
. "$(dirname "$0")/common.sh"
O=${1:-$L/sunrpc-$(uname -r)}
KO=${SUNRPC_KO:-$O/sunrpc/sunrpc.ko}
# NFSD_KO=path also puts that nfsd.ko in updates/, NFSD_KO=none takes it out
NKO=${NFSD_KO:-}
# STACK=dir puts every .ko in dir in updates/ (build-svcstack.sh's set, with
# SUNRPC_KO=none), STACK=none takes the last such set out first
STK=${STACK:-}
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

# one line, spaces between: start() matches " $u " in it
was=$(for u in $UNITS; do systemctl is-active -q $u && echo $u; done | paste -sd ' ')
echo "running sunrpc: $(cat /sys/module/sunrpc/srcversion); active: $was"

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

# A service with a private mount namespace (systemd-hostnamed, among
# others) keeps the nfsd and rpc_pipefs mounts it started with, and those
# hold nfsd and sunrpc.  Stop the bus-activated ones, which come back on
# demand; name anything else.
ns=$(readlink /proc/self/ns/mnt)
for f in $(grep -l -e ' - nfsd ' -e ' - rpc_pipefs ' /proc/[0-9]*/mountinfo 2>/dev/null); do
	p=${f#/proc/}; p=${p%%/*}
	[ "$(readlink /proc/$p/ns/mnt 2>/dev/null)" = "$ns" ] && continue
	u=$(sed -n 's#.*/\([^/]*\.service\)$#\1#p' /proc/$p/cgroup 2>/dev/null | head -1)
	case "$u" in
	systemd-hostnamed.service|systemd-timedated.service|systemd-localed.service)
		echo "stopping $u: its mount namespace holds nfsd"; systemctl stop $u ;;
	*) echo "pid $p ($(cat /proc/$p/comm 2>/dev/null)${u:+, $u}) has nfsd or rpc_pipefs mounted" ;;
	esac
done

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

case "$STK" in
"") ;;
none) [ -e $UPD/.svcstack ] && (cd $UPD && xargs rm -f < .svcstack) && rm -f $UPD/.svcstack ;;
*) mkdir -p $UPD; (cd "$STK" && ls *.ko) > $UPD/.svcstack; cp "$STK"/*.ko $UPD/ ;;
esac
if [ "$KO" != none ]; then
	mkdir -p $UPD
	cp $KO $UPD/sunrpc.ko
fi
case "$NKO" in
"") ;;
none) rm -f $UPD/nfsd.ko ;;
*) mkdir -p $UPD; cp $NKO $UPD/nfsd.ko ;;
esac
depmod -a
echo "modprobe will load: $(modinfo -n sunrpc)"
modprobe sunrpc || { echo "the new sunrpc did not load"; exit 1; }
start

echo "running sunrpc: $(cat /sys/module/sunrpc/srcversion), nfsd: $(cat /sys/module/nfsd/srcversion 2> /dev/null)"
for p in svc_tcp_rx_linger_us; do
	[ -e /sys/module/sunrpc/parameters/$p ] && echo "$p $(cat /sys/module/sunrpc/parameters/$p)"
done
echo "nfsd threads: $(cat /proc/fs/nfsd/threads 2>&1); pools: $(cat /proc/fs/nfsd/pool_threads 2>&1)"
