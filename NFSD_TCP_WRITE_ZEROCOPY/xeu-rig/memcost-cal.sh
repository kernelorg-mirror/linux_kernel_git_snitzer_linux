#!/bin/bash
# memcost-cal.sh <out dir> - what the card and the drives ask of memory by
# themselves, as the mesh counts it (arm_cmn/hnf_mc_reqs/: requests the
# cache's home nodes make of the memory controller).  No NFS.  It is the
# calibration under ../LOAN-PERFORMANCE.md, to be run again on any new
# kernel before a loan's or a copy's figure is read:
#
#   idle
#   plain TCP into one port and both, the receivers discarding (the card's
#     writes alone), and into both with the receivers copying
#   every data drive writing from memory and reading into it, with far more
#     buffer than the cache holds and with about what it holds
#
# Each line of <out>/summary.txt is the rate, the requests to memory a
# second and for each 64-byte line moved, and what the cache was filled
# with and evicted.  Nothing may be connected or exported, and the drives
# get 25 s of idle before each of their turns (20 gives a drive its fast
# region back; a turn writes 22 s).
. "$(dirname "$0")/common.sh"
O=${1:?out dir}; mkdir -p $O
EV=arm_cmn/hnf_mc_reqs/,arm_cmn/hnf_cache_miss/,arm_cmn/hnf_slc_sf_cache_access/,arm_cmn/hnf_cache_fill/,arm_cmn/hnf_slc_eviction/,l3d_cache_lmiss_rd,bus_access_wr
[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || { echo "NFS connections are up"; exit 1; }
"$RIG/drive-cache.sh" > /dev/null || exit 1
line() {	# line <tag> <GB/s moved> <perf stat file>: one line of the summary
	awk -F, -v t="$1" -v g="$2" '$1 ~ /^[0-9]/ {split($3, n, "/"); v[n[2] ? n[2] : $3] = $1 / 10}
		END {l = g * 1e9 / 64; printf "%-22s %6.1f GB/s  to memory %.3fe9/s", t, g, v["hnf_mc_reqs"] / 1e9
		     if (l > 0) printf " = %.2f a line", v["hnf_mc_reqs"] / l
		     printf "  cache filled %.3fe9/s evicted %.3fe9/s  lmiss %.3fe9/s bus_wr %.3fe9/s\n", v["hnf_cache_fill"] / 1e9, v["hnf_slc_eviction"] / 1e9, v["l3d_cache_lmiss_rd"] / 1e9, v["bus_access_wr"] / 1e9}' "$3" | tee -a $O/summary.txt
}
: > $O/summary.txt
perf stat -a -x, -e $EV -o $O/idle.txt -- sleep 10 2> /dev/null; line idle 0 $O/idle.txt
for m in "trunc a" "trunc b" "trunc ab" "copy ab"; do
	set -- $m
	( sleep 9; perf stat -a -x, -e $EV -o $O/tcp-$1-$2.txt -- sleep 10 2> /dev/null ) &
	MODE=$1 "$RIG/iperf2port.sh" $2 16 30 > $O/tcp-$1-$2.log 2>&1; wait
	line "tcp $1 $2" "$(sed -n 's/.*total \([0-9.]*\) GB\/s.*/\1/p' $O/tcp-$1-$2.log)" $O/tcp-$1-$2.txt
	sleep 5
done
drv() {	# drv <tag> <read|write> <iodepth> [bytes a second a job]: four jobs a drive, 1 MiB a request
	local tag=$1 rw=$2 qd=$3 rate=${4:-} n
	sleep 25
	for n in 0 1 2 3 4 5 6 7; do
		mkdir -p /hs/nvme$n/cmncal
		fio --name=c --directory=/hs/nvme$n/cmncal --rw=$rw --bs=1M --direct=1 --ioengine=libaio --iodepth=$qd \
			--numjobs=4 --size=512M --time_based --runtime=22 --group_reporting ${rate:+--rate=$rate} \
			--cpus_allowed=8-55 --output=$O/fio-$tag-$n.txt > /dev/null 2>&1 &
	done
	sleep 9
	awk '$3 ~ /^nvme[0-7]n1$/ {print $6 + $10}' /proc/diskstats > $O/.ds0; local t0=$(date +%s.%N)
	perf stat -a -x, -e $EV -o $O/drv-$tag.txt -- sleep 10 2> /dev/null
	awk '$3 ~ /^nvme[0-7]n1$/ {print $6 + $10}' /proc/diskstats > $O/.ds1; local t1=$(date +%s.%N)
	wait
	line "drives $tag qd$qd" "$(paste $O/.ds0 $O/.ds1 | awk -v w=$(echo "$t1 - $t0" | bc) '{s += $2 - $1} END {printf "%.1f", s * 512 / w / 1e9}')" $O/drv-$tag.txt
}
drv write-big write 32 1812000000	# 7.25 GB/s a drive from 128 MiB of buffers a drive
drv read-big read 32
drv write-small write 1			# 4 MiB of buffers a drive: 32 MiB in all
drv read-small read 1
for n in 0 1 2 3 4 5 6 7; do rm -rf /hs/nvme$n/cmncal; done
