#!/bin/bash
# contrast.sh <label> <fio|iperf|both>: what works, beside NFS (round 104,
# Mike, 17 November: "I need contrast with what works: the iperf and the
# fio runs that operate the drives at 104GB/s").  fio: the eight drives
# read locally (direct, 1 MiB, one reader a drive 16 deep, readprobe.sh's
# 104.54 GB/s point) from the READ strobe's own files, f-* in each
# export; iperf: iperf-tx.sh ab with ZC=1 (tardis2 sending with -Z);
# both: the two at once, the drives' DMA writes and the NIC's DMA reads
# with no NFS between them.  5 s of the window: every CPU's cycles and the
# mesh's memory requests and cache fills; the drives' rate from fio, the
# wire's from the ports' tx_bytes over the same 5 s.  No NFS client may be
# connected.  Files are only read.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
L=${1:?contrast.sh <label> <fio|iperf|both>} M=${2:?}
O=/var/tmp/zc-run-logs/contrast-$L; mkdir -p $O
say() { echo "$(date +%T) $*"; }
[ "$(ss -tnH state established '( sport = :2049 )' | wc -l)" = 0 ] || { say "an NFS client is connected"; exit 1; }
D="enp161s0f0 enp164s0f0"
fiojob() {
	local d f
	for d in 0 1 2 3 4 5 6 7; do
		f=$(cd /hs/nvme$d/zcbudget && ls f-* | tr '\n' ':' | sed 's/:$//')
		[ -n "$f" ] || { say "no f-* files on /hs/nvme$d"; exit 1; }
		echo -n " --name=d$d --directory=/hs/nvme$d/zcbudget --filename=$f"
	done
}
J=$(fiojob) || exit 1
case $M in fio|iperf|both) ;; *) say "fio, iperf or both"; exit 1;; esac
if [ $M != fio ]; then ZC=1 ./iperf-tx.sh ab 16 30 > $O/iperf.out 2>&1 & sleep 6; fi
if [ $M != iperf ]; then
	fio --ioengine=libaio --direct=1 --rw=read --bs=1M --numjobs=1 --iodepth=16 \
		--time_based --ramp_time=3 --runtime=18 --output-format=json --output=$O/fio.json $J > /dev/null 2>&1 &
fi
sleep 8
t0=0; for d in $D; do t0=$(( t0 + $(cat /sys/class/net/$d/statistics/tx_bytes) )); done
perf stat -a -x, -e cycles,arm_cmn/hnf_mc_reqs/,arm_cmn/hnf_cache_fill/ -o $O/perf.txt -- sleep 5
t1=0; for d in $D; do t1=$(( t1 + $(cat /sys/class/net/$d/statistics/tx_bytes) )); done
wait
drv=0; [ $M != iperf ] && drv=$(python3 -I -c "import json; print(sum(j['read']['bw_bytes'] for j in json.load(open('$O/fio.json'))['jobs'])/1e9)")
awk -F, -v drv=$drv -v wire=$(( t1 - t0 )) -v L=$L -v M=$M '
	$3 == "cycles" {c = $1} $3 ~ /hnf_mc_reqs/ {m = $1 * 64} $3 ~ /hnf_cache_fill/ {f = $1 * 64}
	END {w = wire / 5e9; b = (drv > w ? drv : w);
	     printf "%s (%s): drives %.2f GB/s, wire %.2f GB/s; memory %.1f GB/s (%.2f a byte of the larger), cache fills %.1f GB/s; cycles %.0f G/s, %.0f a KiB of the larger\n",
	     L, M, drv, w, m / 5e9, m / 5e9 / b, f / 5e9, c / 5e9, c / 5 / (b * 1e9) * 1024}' $O/perf.txt | tee $O/summary.txt
