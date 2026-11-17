#!/bin/bash
# ab-scope.sh <n>:<scope> ...: FLAMEGRAPH round 109, the client's
# workqueue default_affinity_scope for 4 KiB READ IOPS (Mike, 17
# November: "run the cache_shard A/B").  Run under xeu-rig/settled-iops.sh
# ("settled-iops.sh run ab-scope.sh ..."), so the setting is applied once
# for the whole A/B.  Each run is one nfs4k.sh run at iodepth 64 and
# nconnect 12 with twoport.sh's CLI_SCOPE=<scope> (numa, the rig's
# default since 7 November, or cache_shard, build 541's own), which
# twoport.sh sets on the client for the run and puts back after.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
for run in "$@"; do
	IFS=: read n sc <<< "$run"
	[ -n "$n" ] && [ -n "$sc" ] || { echo "ab-scope.sh <n>:<scope> ..."; exit 1; }
	CLI_SCOPE=$sc ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $n:64:12
	echo "$(date +%T) t2-r$n client scope $sc: $(grep -oE 'workqueue scope [a-z_]+ \(was [a-z_]+\)' /var/tmp/zc-run-logs/t2-r$n/run/geometry.txt)"
done
