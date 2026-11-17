#!/bin/bash
# prof4k.sh <n> [iodepth]: FLAMEGRAPH round 108, where a 4 KiB READ's time
# goes on each end (Mike, 17 November: "profile both ends at iodepth 32
# while I build the client kernel").  One nfs4k.sh run under
# xeu-rig/settled-iops.sh (David Flynn's setting, iodepth 32 unless
# given), and from the load's second 8, beside nfs4k.sh's mpstat:
#   8 s of perf record -a -g at 999 Hz on the server and on the client
#   at once (into /dev/shm on each, removed after), each reported by
#   symbol with and without children, by command, and by CPU;
#   5 s of perf stat on the client, cycles and instructions a socket.
# Into $L/t2-r<n>/prof-{srv,cli}-*.txt.  The client is only read.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
n=${1:?prof4k.sh <n> [iodepth]}; qd=${2:-32}; O=$L/t2-r$n; mkdir -p $O || exit 1
row() { W=$O/run/wire-sec.tsv; for i in $(seq 1500); do [ -f $W ] && [ $(wc -l < $W) -ge $1 ] && return 0; sleep 0.2; done; return 1; }
rep() {	# rep <prefix> <command that runs perf report with its arguments>
	local p=$1; shift
	"$@" --children -g none --sort sym --percent-limit 0.5 2>/dev/null | grep -v '^#' | grep . | head -200 > $O/$p-children.txt
	"$@" --no-children -g none --sort sym --percent-limit 0.1 2>/dev/null | grep -v '^#' | grep . | head -200 > $O/$p-self.txt
	"$@" --no-children -g none --sort comm --percent-limit 0.2 2>/dev/null | grep -v '^#' | grep . | head -60 > $O/$p-comm.txt
	"$@" --no-children -g none --sort cpu 2>/dev/null | grep -v '^#' | grep . > $O/$p-cpu.txt
}
(
	row 8 || exit 0
	$SSHC "perf stat -a --per-socket -x, -e cycles,instructions -- sleep 5" > $O/prof-cli-stat.txt 2>&1 &
	$SSHC "perf record -q -a -g -F 999 -o /dev/shm/zc-p108.data -- sleep 8 > /dev/null 2>&1" &
	perf record -q -a -g -F 999 -o /dev/shm/zc-p108.data -- sleep 8 > /dev/null 2>&1
	wait
	rep prof-srv perf report -i /dev/shm/zc-p108.data
	rm -f /dev/shm/zc-p108.data
	rep prof-cli $SSHC perf report -i /dev/shm/zc-p108.data
	$SSHC "rm -f /dev/shm/zc-p108.data"
) &
./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $n:$qd; rc=$?
wait
for e in srv cli; do
	echo "== $e: top self"; head -12 $O/prof-$e-self.txt
	echo "== $e: by command"; head -8 $O/prof-$e-comm.txt
done
exit $rc
