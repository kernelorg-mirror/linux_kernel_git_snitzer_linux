#!/bin/bash
# prof-both.sh <n> <Y|N>: FLAMEGRAPH round 111, where a 4 KiB READ's time
# goes on both ends with sunrpc's svcauth_borrow at <Y|N> (Mike, 18
# November: "do the auth cache borrow and profile the client too").  One
# settled-iops.sh run of nfs4k.sh <n> (iodepth 64, nconnect 12,
# cache_shard) and, from the load's second 8, 8 s of perf record -a -g at
# 999 Hz on the server and on the client at once (into /dev/shm on
# each), with 5 s of perf stat (cycles, instructions) on the client.
# The server's data stays in /dev/shm/zc-p111-<n>.data for annotate; the
# client's is reported on the client and removed.  Into $L/t2-r<n>/:
# prof-{srv,cli}-{self,children,comm,cpu}.txt, prof-srv-auth.txt (the
# auth and file handle symbols), prof-cli-stat.txt.  svcauth_borrow is
# left at Y.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
n=${1:?prof-both.sh <n> <Y|N>}; on=${2:?prof-both.sh <n> <Y|N>}
P=/sys/module/sunrpc/parameters/svcauth_borrow
S=/sys/module/sunrpc/parameters/svcauth_borrow_stats
O=$L/t2-r$n; D=/dev/shm/zc-p111-$n.data; CD=/dev/shm/zc-p111-cli.data
[ -e $O ] && { echo "t2-r$n exists"; exit 1; }
[ -w $P ] || { echo "no $P: not the auth-borrow sunrpc"; exit 1; }
mkdir -p $O
echo $on > $P || exit 1
AUTH='svc_set_client|svcauth_unix_|svcauth_borrow|ip_map|unix_gid|auth_domain|refcount_dec_not_one|group_info|groups_alloc|sunrpc_cache_|cache_check|fh_verify|nfsd_exp_borrow|exportfs_decode_fh_raw|nfsd_file_lookup_cached'
rep() {	# rep <prefix> <data> [ssh prefix...]
	local p=$1 d=$2; shift 2
	"$@" perf report -i $d --children -g none --sort sym --percent-limit 0.5 2>/dev/null | grep -v '^#' | grep . | head -200 > $O/$p-children.txt
	"$@" perf report -i $d --no-children -g none --sort sym --percent-limit 0.1 2>/dev/null | grep -v '^#' | grep . | head -200 > $O/$p-self.txt
	"$@" perf report -i $d --no-children -g none --sort comm --percent-limit 0.2 2>/dev/null | grep -v '^#' | grep . | head -60 > $O/$p-comm.txt
	"$@" perf report -i $d --no-children -g none --sort cpu 2>/dev/null | grep -v '^#' | grep . > $O/$p-cpu.txt
}
s0=$(cat $S)
(
	W=$O/run/wire-sec.tsv
	for i in $(seq 1500); do [ -f $W ] && [ $(wc -l < $W) -ge 8 ] && break; sleep 0.2; done
	$SSHC "perf stat -a --per-socket -x, -e cycles,instructions -- sleep 5" > $O/prof-cli-stat.txt 2>&1 &
	$SSHC "perf record -q -a -g -F 999 -o $CD -- sleep 8 > /dev/null 2>&1" &
	perf record -q -a -g -F 999 -o $D -- sleep 8 > /dev/null 2>&1
	wait
) &
./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $n; rc=$?
wait
echo "t2-r$n svcauth_borrow $on before: $s0" > $O/borrow.txt
echo "t2-r$n svcauth_borrow $on after:  $(cat $S)" >> $O/borrow.txt
echo Y > $P
rep prof-srv $D
{
	echo "== with children"
	perf report -i $D --children -g none --sort sym 2>/dev/null | grep -v '^#' | grep -E "$AUTH"
	echo "== self"
	perf report -i $D --no-children -g none --sort sym 2>/dev/null | grep -v '^#' | grep -E "$AUTH"
} > $O/prof-srv-auth.txt
rep prof-cli $CD $SSHC
$SSHC "rm -f $CD"
for e in srv cli; do
	echo "== $e: top self"; head -15 $O/prof-$e-self.txt
	echo "== $e: by command"; head -8 $O/prof-$e-comm.txt
done
echo "== server auth and file handle"; cat $O/prof-srv-auth.txt
exit $rc
