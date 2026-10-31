#!/bin/bash
# run125.sh: round 47, what the DPRP5s do with Interrupt Coalescing.  run124
# found a lone completion held about 280 us at 0x0103 (four entries, TIME 1),
# not 100.  64 KiB writes at iodepth 1, one run a value, not saved on the
# drive, back to 0 on any exit, at the settled setting on build 505.  The low
# byte is the threshold less one, the next the time in units of 100 us:
#   0        off
#   0x0100   threshold 1, TIME 1: no wait, if the threshold is honoured
#   0x0200   threshold 1, TIME 2: the same
#   0x0203   threshold 4, TIME 2: twice 0x0103's wait, if TIME scales
#   0x0103   the control, run124's value
#   0        off again
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r125
mkdir -p $O
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
say() { echo "$(date +%T) $*"; }
die() { say "STOPPED: $*"; exit 1; }
ctrl() { for d in 4 5 6 7; do nvme set-feature /dev/nvme$d -f 8 -V $1 > /dev/null || die "set-feature nvme$d $1"; done; }
cval() { nvme get-feature /dev/nvme4 -f 8 | sed -n 's/.*Current value: *//p'; }
trap 'for d in 4 5 6 7; do nvme set-feature /dev/nvme$d -f 8 -V 0 > /dev/null; done' EXIT
W0=$(dmesg | grep -c 'WARNING:')
./settled.sh apply > $O/apply-start.txt 2>&1 || die "not settled: $(grep -E 'BAD|FAILED' $O/apply-start.txt | head -3)"
say "kernel $(uname -r)"
i=0
for v in 0 0x0100 0x0200 0x0203 0x0103 0; do i=$((i + 1)); t=c$i-$v
	[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || die "an NFS client is connected"
	ctrl $v
	for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 60; client_free || die "client not free"
	env FS_A="$FS" FS_B= WARM=10 RUNTIME=30 JOBS=4 IODEPTH=1 BS=64K TSV=$O/results.tsv RWTSV=$O/lat.tsv TAG=$t WRITES=64 ./twoport.sh $O/$t a > $O/$t.log 2>&1 || die "$t: $(grep -E 'BAD|FAILED|no answer|refuse|not free' $O/$t.log | head -3)"
	say "$t coalescing $(cval), $(grep -c 'no drive stepped down' $O/$t.log) no-step: $(python3 -I -c "
import json
j=json.load(open('$O/$t/fio-a.json')); w=j['jobs'][0]['write']; c=w['clat_ns']; p=c['percentile']
print('%.2f GB/s, %.0f IOPS; ms p50 %.3f p90 %.3f p99 %.3f p99.9 %.3f mean %.3f' % (w['bw_bytes']/1e9,w['iops'],p['50.000000']/1e6,p['90.000000']/1e6,p['99.000000']/1e6,p['99.900000']/1e6,c['mean']/1e6))")"
done
ctrl 0
./settled.sh apply > $O/apply-end.txt 2>&1 || die "not settled at the end: $(grep -E 'BAD|FAILED' $O/apply-end.txt | head -3)"
[ "$(dmesg | grep -c 'WARNING:')" = $W0 ] || die "a new WARNING in dmesg"
say "ALL end: coalescing $(cval), $(tail -1 $O/apply-end.txt); DONE"
