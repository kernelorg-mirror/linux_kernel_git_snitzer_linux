#!/bin/bash
# nfsd-stacks.sh <out prefix> [samples] - where the nfsd threads are, while
# a cell runs: the kernel stack of every nfsd thread, once a second, as a
# count of each distinct stack (top frames), with iostat for the NVMe
# drives and the CPUs' time beside it.  A flat CPU profile does not show a
# thread that waits; this does.
O=${1:?out prefix}; N=${2:-20}
( iostat -x -m 1 $N nvme0n1 nvme1n1 > $O.iostat 2>&1 & mpstat 1 $N > $O.mpstat 2>&1 & wait ) &
for i in $(seq $N); do
	for p in $(pgrep -x nfsd); do
		# frames 1 to 9, without offsets; an idle thread sits in svc_recv
		sed -n '1,9p' /proc/$p/stack 2>/dev/null | sed -e 's/^\[<0>\] //' -e 's/+0x.*//' | tr '\n' ' '
		echo
	done
	sleep 1
done | sort | uniq -c | sort -rn > $O.stacks
wait
echo "threads sampled: $(awk '{s+=$1} END{print s}' $O.stacks) over $N s; distinct stacks: $(wc -l < $O.stacks)"
head -12 $O.stacks
