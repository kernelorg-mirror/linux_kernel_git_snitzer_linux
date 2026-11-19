#!/bin/bash
# profile-client.sh <n> [iodepth]: FLAMEGRAPH round 161, where the
# client's 11.4 us of CPU a 4 KiB READ goes (Mike, 19 November: "profile
# the client's 11.4 us per READ too").  One settled-iops.sh run of
# nfs4k.sh <n>:<iodepth> (96), as round 160's on the server, and from
# the load's eighth second 8 s of perf record -a -g at 999 Hz on the
# client (106: /dev/shm/zc-c161-<n>.data, kept), its READs counted over
# 8 s beside it.  perf script is read back over ssh and cstages.awk
# charges each sample to one stage, the first frame from the leaf up
# that it names, user-space frames (fio, liburing) apart, and groups the
# samples by command (fio, kworker, idle CPUs' interrupts, other).  The
# client is x86 with NMIs, so leaves are where the cycles are; it runs
# idle=poll, so its idle CPUs spin in cpu_idle_poll, left out as idle.  Into
# $L/t2-r<n>/: cstages.txt, cleaf-<group>.txt, client.svg and .folded.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
n=${1:?profile-client.sh <n> [iodepth]}; q=${2:-96}
O=$L/t2-r$n; D=/dev/shm/zc-c161-$n.data; P=../FLAMEGRAPH/161-client-cost; FG=${FG:-/opt/FlameGraph}
[ -e $O ] && { echo "t2-r$n exists"; exit 1; }
mkdir -p $O
(
	W=$O/run/wire-sec.tsv
	for i in $(seq 1500); do [ -f $W ] && [ $(wc -l < $W) -ge 8 ] && break; sleep 0.2; done
	$SSHC "perf record -q -N -a -g -e cycles -F 999 -o $D -- sleep 8 > /dev/null 2>&1 & p=\$!
		r() { awk '/^proc3 /{print \$9}' /proc/net/rpc/nfs; }
		sleep 0.5; r0=\$(r); t0=\$(date +%s.%N); sleep 8; r1=\$(r); t1=\$(date +%s.%N); wait \$p
		echo reads \$((r1 - r0)) seconds \$(echo \"\$t1 - \$t0\" | bc)" > $O/cprof-window.txt 2>&1
) &
./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $n:$q; rc=$?
wait
R=$(awk '{print $2}' $O/cprof-window.txt)
$SSHC "perf script --no-inline -i $D -F comm,cpu,ip,sym,dso 2>/dev/null" | awk -v reads=$R -v hz=999 -v out=$O -f $P/cstages.awk > $O/cstages.txt
[ -s $O/client.folded ] && $FG/flamegraph.pl --title "t2-r$n client, 4 KiB READ IOPS" $O/client.folded > $O/client.svg 2>/dev/null
cat $O/cprof-window.txt $O/cstages.txt
exit $rc
