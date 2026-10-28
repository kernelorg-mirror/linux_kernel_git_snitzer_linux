#!/bin/bash
# rundelay.sh SECONDS: nfsd threads' run_delay over a window, untraced
# (FLAMEGRAPH round 40).  /proc/PID/schedstat's second and third fields are
# the time a task has waited on a runqueue and the times it has run; their
# change over the window, summed over every nfsd thread, gives the mean wait
# from being put on a runqueue to running.  A woken thread queued to an
# idle CPU by IPI is put on the runqueue only once that CPU takes the IPI,
# so this leaves out the IPI and the way out of WFI: wakelat.bt's
# "schedule", not its "total".
snap() { for p in $(pgrep -x nfsd); do cat /proc/$p/schedstat 2>/dev/null; done | awk '{c += $1; d += $2; n += $3} END {printf "%d %d %d\n", c, d, n}'; }
w() { awk '/^proc3/{print $10}' /proc/net/rpc/nfsd; }
read c0 d0 n0 <<< "$(snap)"; w0=$(w)
sleep ${1:-8}
read c1 d1 n1 <<< "$(snap)"; w1=$(w)
n=$((n1 - n0)); wr=$((w1 - w0))
echo "runs $n run_delay_ns $((d1 - d0)) mean_ns $(( n ? (d1 - d0) / n : 0 )) on_cpu_ns $((c1 - c0)) WRITEs $wr runs_a_WRITE $(awk -v n=$n -v w=$wr 'BEGIN {printf "%.2f", w ? n / w : 0}')"
