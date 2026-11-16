#!/bin/bash
# readprobe.sh: what the eight drives read locally, with no NFS and no
# network (Mike, 16 November): the READ twin of round 98's orderprobe.sh,
# which found the drives' WRITE ceiling at 81.58 GB/s.  Each drive gets
# eight 2 GiB files, written once with 1 MiB direct writes (a read of
# trimmed, never written blocks is not a read of the media), then every
# point reads them with direct I/O for 20 s after 3 s of ramp, all eight
# drives at once, after 30 s of rest:
#   seq1M-psync-3    3 readers a drive in order, as orderprobe's control
#   seq1M-psync-8    8 readers a drive in order
#   seq1M-aio-1x16   1 reader a drive, 16 outstanding (fio's iodepth 16)
#   seq1M-aio-4x8    4 readers a drive, 8 outstanding each
#   seq1M-aio-8x16   8 readers a drive, 16 outstanding each (128)
#   rand1M-aio-4x8   random 1 MiB reads, 4 readers a drive, 8 each
#   seq512K-aio-4x8  512 KiB reads (an rsize of 512 KiB), 4x8
#   seq8M-aio-4x4    8 MiB reads, 4x4 (the size twoport used to issue)
# Everything it writes it removes.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/FLAMEGRAPH/104-rx-cpus || exit 1
say() { echo "$(date +%T) $*"; }
B=/var/tmp/zc-run-logs/readprobe; mkdir -p $B
[ "$(ss -tnH state established '( sport = :2049 )' | wc -l)" = 0 ] || { say "an NFS client is connected"; exit 1; }
pgrep -f '^/bin/bash \./(twoport|read-check|write-check)\.sh' > /dev/null && { say "a run is going"; exit 1; }
trap 'for d in 0 1 2 3 4 5 6 7; do rm -rf /hs/nvme$d/readprobe; done' EXIT
args() { local d; for d in 0 1 2 3 4 5 6 7; do echo -n " --name=d$d --directory=/hs/nvme$d/readprobe"; done; }
sum() {	# sum <json>
	python3 -I - "$1" <<'PY'
import json, sys
j = json.load(open(sys.argv[1])); per = {}; lat = []
for job in j['jobs']:
    r = job['read'] if job['read']['bw_bytes'] else job['write']
    per[job['jobname']] = per.get(job['jobname'], 0) + r['bw_bytes']
    lat.append(r['clat_ns']['mean'])
print(f"total {sum(per.values())/1e9:.2f} GB/s; a drive " + " ".join(f"{per[k]/1e9:.2f}" for k in sorted(per)) + f"; completion {sum(lat)/len(lat)/1e6:.2f} ms mean")
PY
}
for d in 0 1 2 3 4 5 6 7; do mkdir -p /hs/nvme$d/readprobe; done
fio --ioengine=libaio --iodepth=8 --direct=1 --bs=1M --rw=write --size=2G --numjobs=8 \
	--output-format=json --output=$B/layout.json $(args) > /dev/null 2>&1 || { say "the layout failed"; exit 1; }
say "layout: 8 x 2 GiB a drive, $(sum $B/layout.json)"
point() {	# point <label> <fio args...>
	local label=$1; shift
	sleep 30
	fio --direct=1 --time_based --ramp_time=3 --runtime=20 --size=2G \
		--output-format=json --output=$B/$label.json "$@" $(args) > /dev/null 2>&1 || { say "fio failed: $label"; return; }
	say "$label: $(sum $B/$label.json)"
}
point seq1M-psync-3   --ioengine=psync --rw=read --bs=1M --numjobs=3
point seq1M-psync-8   --ioengine=psync --rw=read --bs=1M --numjobs=8
point seq1M-aio-1x16  --ioengine=libaio --rw=read --bs=1M --numjobs=1 --iodepth=16
point seq1M-aio-4x8   --ioengine=libaio --rw=read --bs=1M --numjobs=4 --iodepth=8
point seq1M-aio-8x16  --ioengine=libaio --rw=read --bs=1M --numjobs=8 --iodepth=16
point rand1M-aio-4x8  --ioengine=libaio --rw=randread --norandommap --bs=1M --numjobs=4 --iodepth=8
point seq512K-aio-4x8 --ioengine=libaio --rw=read --bs=512K --numjobs=4 --iodepth=8
point seq8M-aio-4x4   --ioengine=libaio --rw=read --bs=8M --numjobs=4 --iodepth=4
