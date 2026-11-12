#!/bin/bash
# orderprobe.sh: all eight drives locally (no NFS), three 1 MiB direct
# writes outstanding a drive as before, but in the orders the NFS load
# might present: (1) random 1 MiB writes over a 2 GiB file a thread
# (the client's 8 MiB blocks reach a drive's two files as 1 MiB WRITEs on
# twelve connections, in no order); (2) 24 threads a drive each writing
# its own file in order (nfsd's 48 threads submit to a drive from many
# CPUs, so many NVMe queues); (3) the control, 3 threads a drive in
# order.  20 s a point after 3 s of ramp, an fstrim and 30 s of rest.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/FLAMEGRAPH/98-two-ceilings || exit 1
say() { echo "$(date +%T) $*"; }
B=/var/tmp/zc-run-logs/orderprobe; mkdir -p $B
[ "$(ss -tnH state established '( sport = :2049 )' | wc -l)" = 0 ] || { say "an NFS client is connected"; exit 1; }
for d in 0 1 2 3 4 5 6 7; do [ "$(cat /sys/block/nvme${d}n1/queue/write_cache)" = "write through" ] || { say "nvme${d}n1 is not write through"; exit 1; }; done
trap 'for d in 0 1 2 3 4 5 6 7; do rm -rf /hs/nvme$d/orderprobe; done' EXIT
point() {	# point <label> <fio args...>
	local label=$1; shift; local d args=
	for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; mkdir -p /hs/nvme$d/orderprobe; args="$args --name=d$d --directory=/hs/nvme$d/orderprobe"; done; sleep 30
	fio --ioengine=psync --direct=1 --bs=1M --time_based --ramp_time=3 --runtime=20 --output-format=json --output=$B/$label.json "$@" $args > /dev/null 2>&1 || { say "fio failed: $label"; return; }
	say "$label: $(python3 -I - $B/$label.json <<'PY'
import json, sys
j = json.load(open(sys.argv[1])); per = {}
for job in j['jobs']:
    per[job['jobname']] = per.get(job['jobname'], 0) + job['write']['bw_bytes']
print(f"total {sum(per.values())/1e9:.2f} GB/s; a drive " + " ".join(f"{per[k]/1e9:.2f}" for k in sorted(per)))
PY
)"
}
point rand1M-3  --rw=randwrite --norandommap --size=2G --numjobs=3
point seq-24    --rw=write --size=256M --numjobs=24
point seq-3     --rw=write --size=2G --numjobs=3
