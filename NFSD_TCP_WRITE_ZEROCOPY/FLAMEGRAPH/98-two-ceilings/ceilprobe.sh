#!/bin/bash
# ceilprobe.sh: the drives' aggregate locally (no NFS), all eight at once,
# three writes outstanding a drive, 20 s after 3 s of ramp, each after an
# fstrim and 30 s of rest: (1) laid out as a loaned WRITE's payload (round
# 94's sglwrite, sgl) and (2) flat; then flat again (3) beside memory
# traffic from membw (write, MEMBW_A and MEMBW_B threads on nfsd's CPUs),
# to see whether the drives slow when memory is busy, as under the NFS
# load (121 of about 148 GB/s).  First membw alone at those thread counts,
# for the load each makes.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/FLAMEGRAPH/98-two-ceilings || exit 1
say() { echo "$(date +%T) $*"; }
B=/var/tmp/zc-run-logs/ceilprobe; mkdir -p $B
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
[ "$(ss -tnH state established '( sport = :2049 )' | wc -l)" = 0 ] || { say "an NFS client is connected"; exit 1; }
for d in 0 1 2 3 4 5 6 7; do [ "$(cat /sys/block/nvme${d}n1/queue/write_cache)" = "write through" ] || { say "nvme${d}n1 is not write through"; exit 1; }; done
gcc -O2 -pthread -o $B/membw membw.c || exit 1
gcc -O2 -pthread -o $B/sglwrite ../94-held-split/sglwrite.c || exit 1
trap 'for d in 0 1 2 3 4 5 6 7; do rm -rf /hs/nvme$d/ceilprobe; done' EXIT
MEMBW_A=${MEMBW_A:-3}; MEMBW_B=${MEMBW_B:-6}
for n in $MEMBW_A $MEMBW_B; do say "membw alone: $($B/membw $n 64 4 write)"; done
PIDS=
drives() {	# drives <layout> <label>: eight sglwrite at once, 3 threads each
	local lay=$1 label=$2 d
	for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; mkdir -p /hs/nvme$d/ceilprobe; done; sleep 30
	PIDS=
	for d in 0 1 2 3 4 5 6 7; do $B/sglwrite /hs/nvme$d/ceilprobe 3 3 20 $lay > $B/$label-nvme$d.txt 2>&1 & PIDS="$PIDS $!"; done
}
finish() {	# finish <label> <text>: after the eight, their sum and each
	local label=$1 text=$2; wait $PIDS
	say "$text: total $(cat $B/$label-nvme?.txt | awk '{s += $4} END {printf "%.2f", s}') GB/s; a drive $(for d in 0 1 2 3 4 5 6 7; do awk '{printf "%s ", $4}' $B/$label-nvme$d.txt; done)"
}
# (The first run of this script, 03:34, had drives() in a subshell, so the
# waits failed and each rest overlapped the tail of the run before; the
# runs themselves were 20 s each and 30 s apart.  Its files are read as
# ceilprobe.out's figures.)
drives sgl sgl8; finish sgl8 "sgl, 8 drives x 3"
drives flat flat8; finish flat8 "flat, 8 drives x 3"
for n in $MEMBW_A $MEMBW_B; do
	drives flat flat8-mem$n; sleep 2; m=$($B/membw $n 64 19 write); finish flat8-mem$n "flat, 8 drives x 3, with $m"
done
