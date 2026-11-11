#!/bin/bash
# sglprobe.sh: /hs/nvme0 locally (no NFS), sglwrite.c's 1 MiB O_DIRECT
# writes laid out as a loaned WRITE's payload (sgl: 34 to 35 segments a
# 256 KiB request) or flat (16), THREADS writes outstanding; 20 s a point
# after 3 s of ramp, an fstrim and 30 s of rest, the drive's busy time
# from /sys/block/nvme0n1/stat.  POINTS ("<threads>:<sgl|flat>") in order.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/FLAMEGRAPH/94-held-split || exit 1
say() { echo "$(date +%T) $*"; }
D=/hs/nvme0; F=$D/sglprobe; B=/var/tmp/zc-run-logs/sglprobe; mkdir -p $F $B
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
[ "$(ss -tnH state established '( sport = :2049 )' | wc -l)" = 0 ] || { say "an NFS client is connected"; exit 1; }
[ "$(cat /sys/block/nvme0n1/queue/write_cache)" = "write through" ] || { say "nvme0n1 is not write through"; exit 1; }
gcc -O2 -Wall -Wextra -pthread -o $B/sglwrite sglwrite.c || exit 1
for p in ${POINTS:-2:sgl 2:flat 1:sgl 1:flat 3:sgl 3:flat 4:sgl 4:flat 2:sgl}; do
	fstrim $D; sleep 30
	b0=$(awk '{print $10}' /sys/block/nvme0n1/stat); t0=$(date +%s.%N)
	r=$($B/sglwrite $F ${p%:*} 3 20 ${p#*:}) || { say "sglwrite failed at $p"; exit 1; }
	b1=$(awk '{print $10}' /sys/block/nvme0n1/stat); t1=$(date +%s.%N)
	say "$r; drive busy $(echo "($b1 - $b0) / (($t1 - $t0) * 1000) * 100" | bc -l | xargs printf %.0f)% (ramp included)"
done
rm -rf $F
