#!/bin/bash
# qdprobe.sh: what one data drive (/hs/nvme0) writes, locally with no NFS,
# for each number of 1 MiB direct writes outstanding, as nfsd issues them:
# JOBS threads of fio's psync engine, each one 1 MiB O_DIRECT pwrite at a
# time to its own 1 GiB file, over and over (bs 1M, which the block layer
# cuts into four 256 KiB requests, max_sectors_kb 256).  Each point is
# 20 s after 3 s of ramp, after an fstrim and 30 s of rest, inside the
# drive's fast region (about 625 GB from a 20 s idle).  The two-port load
# keeps about three WRITEs (twelve requests) at each drive.  DEPTHS in
# the order run; 3 first and last, to see drift.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
say() { echo "$(date +%T) $*"; }
D=/hs/nvme0; F=$D/qdprobe; O=$L/qdprobe-nvme0; mkdir -p $O $F
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
[ "$(ss -tnH state established '( sport = :2049 )' | wc -l)" = 0 ] || { say "an NFS client is connected"; exit 1; }
[ "$(cat /sys/block/nvme0n1/queue/write_cache)" = "write through" ] || { say "nvme0n1 is not write through"; exit 1; }
for j in ${DEPTHS:-3 1 2 4 6 8 12 3}; do
	fstrim $D; sleep 30
	fio --name=qd$j --directory=$F --ioengine=psync --direct=1 --rw=write --bs=1M \
		--size=1G --numjobs=$j --time_based --ramp_time=3 --runtime=20 --group_reporting \
		--output-format=json --output=$O/qd$j-$(date +%H%M%S).json > /dev/null 2>&1 || { say "fio failed at $j"; exit 1; }
	f=$(ls -t $O/qd$j-*.json | head -1)
	say "$(python3 -I - $f $j <<'PY'
import json, sys
j = json.load(open(sys.argv[1])); w = j['jobs'][0]['write']; c = w['clat_ns']['percentile']
d = j.get('disk_util', [{}])[0]
print(f"jobs {sys.argv[2]:>2}: {w['bw_bytes']/1e9:5.2f} GB/s, a 1 MiB write {w['clat_ns']['mean']/1e3:5.0f} us mean,"
      f" p50 {c['50.000000']/1e3:.0f}, p99 {c['99.000000']/1e3:.0f}, p99.9 {c['99.900000']/1e3:.0f} us;"
      f" drive busy {d.get('util', 0):.0f}%")
PY
)"
done
rm -rf $F
