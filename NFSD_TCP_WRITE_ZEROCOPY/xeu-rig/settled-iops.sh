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
#   sunrpc.svcauth_borrow Y   the AUTH_SYS borrow: 2.54 to 2.55 M IOPS
#                             against 2.47 to 2.49 at N (FLAMEGRAPH round
#                             111, t2-r957 to r962)       EXP_SVCAUTH_BORROW
#   nfsd.export_borrow_slots 16  the export borrow's entries a thread: 99.98%
#                             hits on the eight exports (round 110)
#                                                     EXP_EXPORT_BORROW_SLOTS
#   nfsd.fh_borrow_slots 48   the file handle borrow's entries a thread:
#                             99.78% hits and no evictions at 64, 95.4% and
#                             3.1 M evictions at 16 (round 114, t2-r981,
#                             r975)                    EXP_FH_BORROW_SLOTS
#                             (Mike, 18 November: "keep fh_borrow_slots
#                             at 48")
#   (no sunrpc.svc_rtc_spill_cpus since 19 November: settled.sh's none.
#   The spill onto the ring CPUs, 0-7,56-63 at weight 1, gave 2.73 to 2.78
#   M IOPS against 2.57 to 2.58 off in round 116 (t2-r995 to r1000), but
#   in round 154 it cost 1.05 us of the server's CPU a READ at the same
#   IOPS (15.75 against 14.70 us, t2-r1211 to r1214); Mike: "shouldn't it
#   be disabled for all now?")
#   sunrpc.svc_rtc_send_more Y  the rtc replies batched with MSG_MORE: 16%
#                             fewer segments out a reply, 2.71 to 2.76 M
#                             IOPS against 2.67 to 2.70 at N and p99 about
#                             0.3 ms lower (round 120, t2-r1009 to r1014;
#                             Mike, 18 November: "worth adding to
#                             settled-iops.sh")          EXP_RTC_SEND_MORE
#   net.ipv4.tcp_limit_output_bytes 1 MiB  (settled on tardis2: 256 KiB)
#                             4 MiB took 0.2 us less server CPU a READ
#                             (round 125, t2-r1030 to r1033); under the
#                             rebalancer every size from 256 KiB to 4 MiB
#                             read 2.91 to 2.95 M, and 1 MiB tied the most
#                             at the lowest p99 (round 126, t2-r1042 to
#                             r1044; Mike, 18 November: "use 1 MiB in
#                             settled-iops.sh")                 EXP_TCP_LOB
#   sunrpc.svc_rtc_rebalance_ms 250  connections moved off the busiest
#                             CPU: 2.91 M IOPS against 2.87, the least busy
#                             CPUs 58% and more against 46 (round 126,
#                             t2-r1038 to r1041; Mike, 18 November: "set
#                             svc_rtc_rebalance_ms to 250 in
#                             settled-iops.sh")      EXP_RTC_REBALANCE_MS
#
# The two borrows since 18 November (Mike: "add both to settled-iops.sh"):
# their defaults, which settled.sh does not hold at rest; held here, a
# run refuses to start with either off.
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
#   NCONN=12                  12 a mount, 96 in all, two on each of nfsd's
#                             48 work CPUs (FLAMEGRAPH round 109, step 3:
#                             2.31 M IOPS against 2.20 at his 16, t2-r899
#                             and r902); 16 until 17 November
#   JOBS=24                   a port: 48 jobs, one a client CPU
#   IODEPTH=64, io_uring with registered buffers, 4 KiB random READs
#                             (round 109 ran every A/B from step 3 on at
#                             64); 32, his, until 17 November.  Round 109
#                             put 12 and 64 only on its command lines, so
#                             runs without them fell back to 16 and 32
#                             (t2-r918, r919: 2.01 to 2.02 M); Mike, 17
#                             November: "that should be reflected in
#                             settled-iops.sh settings!"
#   CLI_SCOPE=cpu             the client's workqueue default_affinity_scope
#                             for the run, one unbound pool a CPU for
#                             nfsiod and xprtiod (Mike, 18 November: "add
#                             it to settled-iops.sh"): 2.54 to 2.55 M IOPS
#                             against cache_shard's 2.52 to 2.54, the
#                             client 3 points less busy in every pair, the
#                             p99 0.1 to 0.3 ms longer (FLAMEGRAPH round
#                             112, t2-r966 to r971: the pool lock nfsiod's
#                             release of every READ fills).  cache_shard,
#                             build 541's own default, from 17 November
#                             (round 109: 2.39 M against numa's 2.31 to
#                             2.32).  The WRITE and READ passes keep numa.
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
export EXP_SVCAUTH_BORROW=${EXP_SVCAUTH_BORROW:-Y}
export EXP_EXPORT_BORROW_SLOTS=${EXP_EXPORT_BORROW_SLOTS:-16}
export EXP_FH_BORROW_SLOTS=${EXP_FH_BORROW_SLOTS:-48}
export EXP_RTC_SEND_MORE=${EXP_RTC_SEND_MORE:-Y}
export EXP_TCP_LOB=${EXP_TCP_LOB:-1048576}
export EXP_RTC_REBALANCE_MS=${EXP_RTC_REBALANCE_MS:-250}
export EXP_SYSCTL=${EXP_SYSCTL:-"net.core.rmem_max=268435456;net.core.wmem_max=268435456;net.ipv4.tcp_rmem=4096 1048576 268435456;net.ipv4.tcp_wmem=4096 1048576 268435456"}
case "${1:-}" in
apply|audit) exec ./settled.sh "$@" ;;
run)
	shift; [ $# -gt 0 ] || { echo "settled-iops.sh run <command ...>"; exit 1; }
	export SRV_ALIAS=${SRV_ALIAS:-1} NCONN=${NCONN:-12} JOBS=${JOBS:-24} IODEPTH=${IODEPTH:-64}
	export CLI_SCOPE=${CLI_SCOPE:-cpu}
	export RW=${RW:-randread} BS=${BS:-4k} IOENGINE=${IOENGINE:-io_uring} FIXEDBUFS=${FIXEDBUFS-1}
	"$@"; rc=$?
	env -u EXP_NFSD_THREADS -u EXP_SYSCTL -u EXP_POOL_MODE -u EXP_SVCAUTH_BORROW \
		-u EXP_EXPORT_BORROW_SLOTS -u EXP_FH_BORROW_SLOTS \
		-u EXP_RTC_SEND_MORE \
		-u EXP_TCP_LOB -u EXP_RTC_REBALANCE_MS ./settled.sh apply > /var/tmp/zc-run-logs/settled-iops-after.txt 2>&1
	echo "settled again: $(tail -1 /var/tmp/zc-run-logs/settled-iops-after.txt)"
	exit $rc ;;
*) echo "settled-iops.sh apply|audit|run <command ...>"; exit 1 ;;
esac
