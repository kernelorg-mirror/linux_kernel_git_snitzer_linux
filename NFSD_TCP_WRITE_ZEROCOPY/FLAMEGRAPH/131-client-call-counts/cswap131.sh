#!/bin/bash
# cswap131.sh: round 91's cswap.sh with NFSM from the environment, so that
# round 131 can load the client side only (NFSM="sunrpc nfsv3 nfsv4"):
# the tree's nfsd links in a KUnit suite the client does not load.
# cswap.sh KO|ROOT/|stock: run on the client (106).  With no NFS mounted,
# stop what holds sunrpc (its own NFS server and helpers, rpc_pipefs,
# nfsd's filesystem), unload every module on sunrpc, load KO by path
# (insmod), or every NFS module from the module root ROOT/ that
# cbuild.sh made (modprobe -d), or the stock ones (modprobe), and start
# again what was running, exports and all.  "stock" is what modprobe
# resolves: since 10 November the WQ_SYSFS sunrpc.ko in updates/ with the
# rpm's NFS modules.  A KO or ROOT/ is loaded, not installed: a reboot
# brings back what modprobe resolves.
set -u
ko=$1
[ -z "$(findmnt -rn -t nfs,nfs4)" ] || { echo "NFS is mounted"; exit 1; }
U="nfs-server nfs-mountd nfsdcld nfs-idmapd rpc-statd rpc-statd-notify rpc-gssd gssproxy"
A=/root/zc-csunrpc/active.txt
[ -s $A ] || for u in $U; do systemctl -q is-active $u && echo $u; done > $A
systemctl stop $U
systemctl stop var-lib-nfs-rpc_pipefs.mount proc-fs-nfsd.mount 2>/dev/null
umount /var/lib/nfs/rpc_pipefs /proc/fs/nfsd 2>/dev/null
M="nfs_layout_flexfiles nfs_layout_nfsv41_files blocklayoutdriver auth_name rpcsec_gss_krb5 nfsv4 nfsv3 nfsd rpcrdma nfs auth_rpcgss nfsidmap lockd nfs_acl nfs_localio nfs_dio grace"
for m in $M; do lsmod | grep -q "^$m " && modprobe -r $m; done
modprobe -r sunrpc || { echo "sunrpc still held:"; lsmod | grep -E 'sunrpc|nfs|rpc'; exit 1; }
NFSM=${NFSM:-"sunrpc nfsd nfsv3 nfsv4 nfs_layout_flexfiles rpcsec_gss_krb5 rpcrdma"}
case $ko in
stock) modprobe -a $NFSM ;;
*/) modprobe -d $ko -a $NFSM ;;
*) insmod "$ko" ;;
esac || { echo "load of $ko failed, stock instead"; for m in $M; do lsmod | grep -q "^$m " && modprobe -r $m; done; modprobe -r sunrpc; modprobe -a $NFSM; fail=1; }
systemctl start var-lib-nfs-rpc_pipefs.mount proc-fs-nfsd.mount 2>/dev/null
systemctl start $(cat $A)
[ "$ko" = stock ] && rm -f $A
echo "sunrpc $(cat /sys/module/sunrpc/srcversion); running: $(for u in $(cat $A 2>/dev/null || echo $U); do systemctl -q is-active $u && echo -n "$u "; done); exports $(exportfs | wc -l)"
exit ${fail:-0}
