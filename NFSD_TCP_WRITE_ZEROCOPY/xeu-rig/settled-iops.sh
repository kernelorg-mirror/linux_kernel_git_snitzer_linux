#!/bin/bash
# settled-iops.sh apply|audit|run <command ...>: the setting for 4 KiB
# random-READ IOPS, after David Flynn's instructions
# (../nfsv3-max-iops-agent-instructions-20261117.md; Mike, 17 November:
# "create settled-iops.sh to use as means to tune for IOPS based on what
# David prescribes").  It is settled.sh's setting with these held over it:
#
#   nfsd threads 768          David's 2.2 (settled: 512)    EXP_NFSD_THREADS
#   net.core.rmem_max, wmem_max 256 MiB                       EXP_SYSCTL
#   net.ipv4.tcp_rmem, tcp_wmem "4096 1048576 268435456"   (his 2.4)
#   sunrpc.pool_mode percpu   64 pools of 12 threads (settled: pernode,
#                             one pool)                    EXP_POOL_MODE
#
# pool_mode percpu since 17 November (Mike: "for settled-iops.sh settle
# percpu"): what auto picks, as David's servers ran, 1.93 M IOPS median
# against pernode's 1.89 (FLAMEGRAPH round 109, t2-r888 to r893).  It
# stays here and not in settled.sh: per-CPU pools cost WRITEs on builds
# 512 and 514.  settled.sh restarts nfs-server to change it, on the way
# in and again on the way out of "run".
#
# and, for "run", the load his section 4 describes, set in the environment
# for twoport.sh (and so for FLAMEGRAPH/108-4k-randread/nfs4k.sh):
#
#   SRV_ALIAS=1               each of the eight filesystems mounted from its own
#                             server address, so each has its own rpc_clnt
#                             and its own nconnect connections
#   NCONN=16                  16 a mount, 128 in all, his connection count
#   JOBS=24                   a port: 48 jobs, one a client CPU
#   IODEPTH=32, io_uring with registered buffers, 4 KiB random READs
#   CLI_SCOPE=cache_shard     the client's workqueue default_affinity_scope
#                             for the run, build 541's own default, not
#                             twoport.sh's numa (Mike, 17 November: "add
#                             cache_shard to settled-iops.sh"): 2.39 M IOPS
#                             in three runs against numa's 2.31 to 2.32,
#                             the client 68% busy against 77%, the p99
#                             0.8 ms longer (FLAMEGRAPH round 109, t2-r909
#                             to r914).  The WRITE and READ passes keep numa.
#
# What he prescribes that settled.sh already holds: max_block_size 1 MiB,
# io_cache_read and io_cache_write 2 (direct), svc_tcp_rx_region_reads N,
# per-CPU rtc pools (89a3792597d7e here; his patches 16 and 17, with no
# switch), and each connection on the least-loaded CPU apart from the
# rings (svc_rtc_balance Y, fe25e474fa3de; his 22 and 23).
#
# What is not taken here, and why:
#   - the export stays sync (AGENTS.md: the mode that acts like enterprise
#     storage); a READ does not wait on it.
#   - MTU: the ports run 7792, not his 9001.
#   - his default IRQ placement with no RPS or XPS: settled.sh's layout of
#     rings, nfsd and RFS stays; trying his is an A/B of its own.
#   - his patch 27, the key client patch (the rpc_clnt's lock and task
#     list in 16 shards): it is 7cc54f28c69fc in this tree, but the client
#     runs build 469, from before it, and the client is held as it is.
#     SRV_ALIAS spreads the RPCs over eight rpc_clnts instead, as his 16
#     mounts did without it.
#
# "apply" and "audit" are settled.sh's with the overrides above.  "run"
# runs a command with the whole setting in its environment, so that the
# settled.sh apply each of them makes holds it; settled.sh's own setting
# is applied again after.  Any of the variables already set overrides its
# value here.  Nothing here changes the settled setting.
cd "$(dirname "$0")" || exit 1
export EXP_NFSD_THREADS=${EXP_NFSD_THREADS:-768}
export EXP_POOL_MODE=${EXP_POOL_MODE:-percpu}
export EXP_SYSCTL=${EXP_SYSCTL:-"net.core.rmem_max=268435456;net.core.wmem_max=268435456;net.ipv4.tcp_rmem=4096 1048576 268435456;net.ipv4.tcp_wmem=4096 1048576 268435456"}
case "${1:-}" in
apply|audit) exec ./settled.sh "$@" ;;
run)
	shift; [ $# -gt 0 ] || { echo "settled-iops.sh run <command ...>"; exit 1; }
	export SRV_ALIAS=${SRV_ALIAS:-1} NCONN=${NCONN:-16} JOBS=${JOBS:-24} IODEPTH=${IODEPTH:-32}
	export CLI_SCOPE=${CLI_SCOPE:-cache_shard}
	export RW=${RW:-randread} BS=${BS:-4k} IOENGINE=${IOENGINE:-io_uring} FIXEDBUFS=${FIXEDBUFS-1}
	"$@"; rc=$?
	env -u EXP_NFSD_THREADS -u EXP_SYSCTL -u EXP_POOL_MODE ./settled.sh apply > /var/tmp/zc-run-logs/settled-iops-after.txt 2>&1
	echo "settled again: $(tail -1 /var/tmp/zc-run-logs/settled-iops-after.txt)"
	exit $rc ;;
*) echo "settled-iops.sh apply|audit|run <command ...>"; exit 1 ;;
esac
