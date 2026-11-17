#!/bin/bash
# local.sh <label> <jobs a drive>:<iodepth> ...: FLAMEGRAPH round 108, how
# many 4 KiB random READs the eight drives do locally (Mike, 17
# November: "we need to do some 4K random READ IOPS runs, starting with
# how fast all 8 drives can do it locally, using the io_uring method is
# perfectly fine").  For each <jobs>:<iodepth>, one fio run on tardis2
# itself, no network: on every drive <jobs> jobs reading the drive's kept
# f-* files (about 2 GB a drive, written whole by the fill; the raw
# device would read trimmed blocks, which a drive answers without its
# flash), 4 KiB random reads, O_DIRECT, io_uring with registered buffers
# and files, ramp 3 s and 20 s counted, with mpstat of the server's CPUs
# for 10 s in the middle.  It prints IOPS in all and a drive, mean and
# 99th-percentile completion latency, and how busy the CPUs were (iowait
# not counted).  The NVMe driver has no poll queues (poll_queues 0), so
# completions are interrupts.
L=${1:?local.sh <label> <jobs>:<iodepth> ...}; shift
O=/var/tmp/zc-run-logs/r108-$L; mkdir -p $O
say() { echo "$(date +%T) $*"; }
for cfg in "$@"; do
	IFS=: read nj qd <<< "$cfg"
	J=""
	for d in 0 1 2 3 4 5 6 7; do
		f=$(cd /hs/nvme$d/zcbudget && ls f-* | tr '\n' ':' | sed 's/:$//')
		[ -n "$f" ] || { say "no f-* files on /hs/nvme$d"; exit 1; }
		J="$J --name=d$d --directory=/hs/nvme$d/zcbudget --filename=$f"
	done
	T=$O/j${nj}-q${qd}
	( sleep 8; mpstat -P ALL 1 10 > $T.mpstat 2>&1 ) &
	fio --ioengine=io_uring --fixedbufs=1 --registerfiles=1 --direct=1 --rw=randread --bs=4k \
		--numjobs=$nj --iodepth=$qd --norandommap=1 --randrepeat=0 --time_based --ramp_time=3 --runtime=20 \
		--output-format=json --output=$T.json $J > $T.out 2>&1
	wait
	python3 -I - $T <<'PY'
import json, sys, statistics as st
t = sys.argv[1]
jobs = json.load(open(t + '.json'))['jobs']
per = {}
for j in jobs:
	per.setdefault(j['jobname'], []).append(j)
iops = sum(j['read']['iops'] for j in jobs)
lat = [j['read']['clat_ns']['mean'] for j in jobs]
p99 = max(float(j['read']['clat_ns']['percentile'].get('99.000000', 0)) for j in jobs)
d = sorted(sum(x['read']['iops'] for x in v) for v in per.values())
busy, col = [], None
for line in open(t + '.mpstat'):
	s = line.split()
	if s[:2] == ['Average:', 'CPU']: col = {k: i for i, k in enumerate(s)}
	elif col and s and s[0] == 'Average:' and s[1].isdigit():
		busy.append(100 - float(s[col['%idle']]) - float(s[col['%iowait']]))
print(f"{t.split('/')[-1]}: {iops / 1e6:.2f} M IOPS ({iops * 4096 / 1e9:.1f} GB/s), a drive {d[0] / 1e6:.2f} to {d[-1] / 1e6:.2f} M, "
      f"completion {st.mean(lat) / 1e3:.0f} us mean, {p99 / 1e3:.0f} us at the 99th percentile (worst job), "
      f"CPUs busy {st.mean(busy):.0f}% (busiest {max(busy):.0f}%)")
PY
done
