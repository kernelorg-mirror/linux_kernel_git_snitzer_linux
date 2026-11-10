#!/bin/bash
# cswap.sh KO|stock: run on the client (106).  With no NFS mounted, stop
# what holds sunrpc (its own NFS server and helpers, rpc_pipefs, nfsd's
# filesystem), unload every module on sunrpc, load KO by path (insmod, so
# nothing is installed and a reboot brings stock back) or the stock one
# (modprobe), and start again what was running, exports and all.
set -u
ko=$1
[ -z "$(findmnt -rn -t nfs,nfs4)" ] || { echo "NFS is mounted"; exit 1; }
U="nfs-server nfs-mountd nfsdcld nfs-idmapd rpc-statd rpc-statd-notify rpc-gssd gssproxy"
A=/root/zc-csunrpc/active.txt
[ -s $A ] || for u in $U; do systemctl -q is-active $u && echo $u; done > $A
systemctl stop $U
systemctl stop var-lib-nfs-rpc_pipefs.mount proc-fs-nfsd.mount 2>/dev/null
umount /var/lib/nfs/rpc_pipefs /proc/fs/nfsd 2>/dev/null
M="nfs_layout_flexfiles rpcsec_gss_krb5 nfsv4 nfsv3 nfsd rpcrdma nfs auth_rpcgss nfsidmap lockd nfs_acl nfs_localio nfs_dio grace"
for m in $M; do lsmod | grep -q "^$m " && modprobe -r $m; done
modprobe -r sunrpc || { echo "sunrpc still held:"; lsmod | grep -E 'sunrpc|nfs|rpc'; exit 1; }
if [ "$ko" = stock ]; then modprobe sunrpc; else insmod "$ko"; fi || { echo "load failed"; modprobe sunrpc; }
systemctl start var-lib-nfs-rpc_pipefs.mount proc-fs-nfsd.mount 2>/dev/null
systemctl start $(cat $A)
if [ "$ko" = stock ]; then modprobe -a rpcrdma rpcsec_gss_krb5 nfsv3 nfsv4 nfs_layout_flexfiles; rm -f $A; fi
echo "sunrpc $(cat /sys/module/sunrpc/srcversion); running: $(for u in $(cat $A 2>/dev/null || echo $U); do systemctl -q is-active $u && echo -n "$u "; done); exports $(exportfs | wc -l)"
