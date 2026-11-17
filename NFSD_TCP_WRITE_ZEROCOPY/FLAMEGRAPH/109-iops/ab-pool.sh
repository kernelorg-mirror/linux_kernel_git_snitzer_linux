#!/bin/bash
# ab-pool.sh <n>:<pool_mode> ...: FLAMEGRAPH round 109, the server's
# sunrpc.pool_mode for 4 KiB READ IOPS (Mike, 17 November: the server A/B
# on IOPS.md's list).  Each run is one nfs4k.sh run at iodepth 32 under
# xeu-rig/settled-iops.sh with EXP_POOL_MODE=<pool_mode> (settled.sh
# restarts nfs-server to change it; percpu, what auto picks on this
# kernel, 64 of 12 threads, pernode one pool of 768), in the order
# given; settled.sh's setting (pernode, 512 threads) after each.
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
for run in "$@"; do
	IFS=: read n pm <<< "$run"
	[ -n "$n" ] && [ -n "$pm" ] || { echo "ab-pool.sh <n>:<pool_mode> ..."; exit 1; }
	EXP_POOL_MODE=$pm ./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh $n:32
	echo "$(date +%T) t2-r$n pool_mode $pm: pool_threads $(grep -m1 "nfsd threads in each pool" /var/tmp/zc-run-logs/t2-r$n/apply.txt | sed "s/.*pool *//" | wc -w) pools"
done
