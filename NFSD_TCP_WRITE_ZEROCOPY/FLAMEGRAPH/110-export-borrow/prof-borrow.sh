#!/bin/bash
# prof-borrow.sh <n> <slots>: FLAMEGRAPH round 110, what limits IOPS
# with the borrow at <slots>, and what a borrow costs (Mike, 17 November:
# "profile the server at 16 to see what limits IOPS now. but please look
# to see if the export borrowing code's complexity with 16 somehow
# negates the 99.98% hit rate").  nfsd's export_borrow_slots set to
# <slots>, then one settled-iops.sh run of nfs4k.sh <n> (iodepth 64,
# nconnect 12, cache_shard) and, from the load's second 8, 8 s of
# perf record -a -g at 999 Hz on the server.  The data stays in
# /dev/shm/zc-p110-<n>.data (not on the root drive) for perf annotate,
# and is reported into $L/t2-r<n>/:
#   prof-srv-{self,children,comm,cpu}.txt as prof4k.sh makes them;
#   prof-srv-fh.txt: with and without children, every symbol of the
#   file handle's export lookup and the borrow (fh_verify,
#   nfsd_set_fh_dentry, nfsd_exp_*, rqst_exp_find, exp_find_key,
#   exp_get_by_name, cache_check, exp_put, cache_put, fh_put, ...);
#   prof-srv-borrow-annotate.txt: perf annotate of nfsd_exp_borrow.
# The parameter is left at 16, the default.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
n=${1:?prof-borrow.sh <n> <slots>}; slots=${2:?prof-borrow.sh <n> <slots>}
P=/sys/module/nfsd/parameters/export_borrow_slots
B=/sys/kernel/debug/nfsd/export_borrow
O=$L/t2-r$n; D=/dev/shm/zc-p110-$n.data
[ -e $O ] && { echo "t2-r$n exists"; exit 1; }
[ -w $P ] || { echo "no $P: not the borrow-fix nfsd"; exit 1; }
[ -r $B ] || mount -t debugfs none /sys/kernel/debug
mkdir -p $O
echo $slots > $P || exit 1
FH='fh_verify|__fh_verify|nfsd_set_fh_dentry|fh_put|nfsd_exp_|rqst_exp_find|exp_find_key|exp_find\b|exp_get_by_name|svc_expkey_lookup|svc_export_lookup|sunrpc_cache_lookup|cache_check|exp_put|cache_put|svc_export_put|expkey_put|nfsd_setuser_and_check_port|check_nfsd_access|nfsexp_flags'
rep() {
	perf report -i $D --children -g none --sort sym --percent-limit 0.5 2>/dev/null | grep -v '^#' | grep . | head -200 > $O/prof-srv-children.txt
	perf report -i $D --no-children -g none --sort sym --percent-limit 0.1 2>/dev/null | grep -v '^#' | grep . | head -200 > $O/prof-srv-self.txt
	perf report -i $D --no-children -g none --sort comm --percent-limit 0.2 2>/dev/null | grep -v '^#' | grep . | head -60 > $O/prof-srv-comm.txt
	perf report -i $D --no-children -g none --sort cpu 2>/dev/null | grep -v '^#' | grep . > $O/prof-srv-cpu.txt
	{
		echo "== with children"
		perf report -i $D --children -g none --sort sym 2>/dev/null | grep -v '^#' | grep -E "$FH"
		echo "== self"
		perf report -i $D --no-children -g none --sort sym 2>/dev/null | grep -v '^#' | grep -E "$FH"
	} > $O/prof-srv-fh.txt
	perf annotate -i $D --stdio -s nfsd_exp_borrow 2>/dev/null | head -400 > $O/prof-srv-borrow-annotate.txt
}
b0=$(cat $B)
(
	W=$O/run/wire-sec.tsv
	for i in $(seq 1500); do [ -f $W ] && [ $(wc -l < $W) -ge 8 ] && break; sleep 0.2; done
	perf record -q -a -g -F 999 -o $D -- sleep 8 > /dev/null 2>&1
) &
./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $n; rc=$?
wait
echo "t2-r$n slots $slots borrow before: $b0" > $O/borrow.txt
echo "t2-r$n slots $slots borrow after:  $(cat $B)" >> $O/borrow.txt
echo 16 > $P
rep
echo "== top self"; head -15 $O/prof-srv-self.txt
echo "== fh and export lookup"; cat $O/prof-srv-fh.txt
exit $rc
