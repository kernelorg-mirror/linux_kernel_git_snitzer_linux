#!/bin/bash
# settled.sh - the settled setting of the rig on tardis1 and tardis2, in one
# place, and the audit every test runs against it (Mike, 14 October: "lock it
# down hard").  Nothing here is to be left to boot order, a service's default
# or the last run.
#
# tardis2 (from 17 October) has the same card, CPUs and client, and is held
# to the same setting except where common.sh's host block says otherwise:
# its eight DPRP5 data drives with SGLs (nvme0 to nvme7; nvme8 is its boot
# drive), both ports since 2 November, its own addresses, no tuned (the sysctls below are
# held by apply and by /etc/sysctl.d/90-zc-settled.conf) and none of
# Hammerspace's services (nfsd's 512 threads come from /etc/nfs.conf and are
# pinned by place).
#
#   settled.sh audit <mode>  check the live state; print every item, and
#                            exit 1 if any differs from what <mode> expects
#   settled.sh place [a] [b] put the CPU layout in place, for nfsd and the
#                            ports named (the host's ports): after anything that opens a
#                            port, and safe while a run is under way
#   settled.sh apply         place, put the resting module settings in
#                            place, then audit "rest"
#   settled.sh show          the live state, one line an item, for a record
#
# Modes:
#   rest   no load: both ports at four rings of 256, the module settings
#          of /etc/modprobe.d (the one-port setting's), nothing connected
#   two    a two-port run at the two-port setting: rings of 128 on both
#          ports, common.sh's TWO_RCVBUF held buffer (512 KiB on tardis2,
#          384 KiB on tardis1), no linger; twoport.sh mounts TWO_NCONN
#          connections a port (12 on tardis2, 16 on tardis1)
#   one    a one-port run at the one-port setting: rings of 256, a 1 MiB
#          held buffer (no linger since 15 October, when it was dropped)
#   cell   a loan sweep's cell on port a: its plan line sets the rings, the
#          held buffer and the budget, passed as EXP_ENTRIES_A, EXP_RCVBUF,
#          EXP_BUDGET, EXP_BUDGET_RESERVE and EXP_LINGER
# EXP_SKB_DEFER_MAX holds net.core.skb_defer_max to a run's SKB_DEFER_MAX=.
# EXP_SRV_QDISC holds the server ports' qdisc to a run's SRV_QDISC=.
# EXP_NOTRACK=absent holds a run to no zc_notrack table (twoport.sh's NOTRACK=absent).
# EXP_NETAVARK=present holds a run to podman's netavark table with no container
# (FLAMEGRAPH round 37's replay of round 01, whose host had it).
# EXP_GRO_MAX holds the server ports' GRO max sizes to a run's GRO_MAX=.
# EXP_RINGS holds the ports' ring count to a run's RINGS= (with EXP_LAYOUT).
# EXP_REFILL holds xeu's rx_refill_thresh to a run's REFILL= instead of 8,
# and EXP_XEU_<parameter> (exported) holds any xeu parameter to a value, for
# a host whose xeu was loaded off the setting on purpose (rx_ip_align=0).
# EXP_XEU_<parameter>=absent holds a run to an xeu without that parameter
# (FLAMEGRAPH round 37's replay of round 01 on build 494's own xeu).
# A run that sets something on purpose (twoport.sh's RCVBUF= and the like)
# passes it as EXP_<NAME>: the audit then holds the live state to that and
# says the run is off the settled setting and where.
#
# THE CPU LAYOUT (XEU-4-VS-8-RING.md, "Four rings of 128 a port")
#
#   CPUs 8-55   nfsd, 512 threads in one pool, and nothing of the
#               ports'.  irqbalance is barred from them and from the card's
#               interrupts (--banmod=xeu), so it cannot move either.
#   CPUs 0-7    port a (enp161s0f0): Rx-i on 2i, Tx-i on 2i+1, its async
#               interrupt on 1 with Tx-0
#   CPUs 56-63  port b (enp164s0f0): Rx-i on 56+2i, Tx-i on 57+2i, its
#               async interrupt on 57 with Tx-0
#   The async interrupt is the driver's one that is not a ring's; it fired
#   no times in a boot of runs, and left alone the kernel put it on one of
#   nfsd's CPUs.  It goes with Tx-0 because a port's transmit CPUs are 77
#   to 94% idle at 60 GB/s and its receive CPUs only 12 to 19% (Mike, 14
#   October).
#   no RPS, NAPI not threaded: a frame is received and goes through TCP on
#   the CPU its ring interrupts
#
# What undoes it, and is why this audit exists:
#   - a restart of nfs-server: its new threads run on all 64 CPUs (only
#     hammerspace-nfsd-tuning, at boot, puts them on 8-55)
#   - any open of a port (down and up, ethtool -G, ethtool -L): the driver
#     puts that port's interrupts on 0-7, whichever port it is
#   - irq_tuning.service and rps_xps_tuning.service, which run at boot and
#     again whenever udev adds an xeu port (a reload of xeu): their E1
#     profile puts port a's interrupts on 8-31 and port b's on 32-55, on
#     nfsd's CPUs, and writes its own transmit steering (below)
#   - the svcsock KUnit suite before its fix: it left svc_tcp_rx_rcvbuf at 0
#
# !!! irq_tuning.service AND rps_xps_tuning.service ARE MASKED ON TARDIS1 !!!
# !!! (14 October, Mike) and our tuning is TO BE RECONCILED with them.    !!!
# Hammerspace's platform tuning for E1 lays the ports out against this
# setting (rings on nfsd's CPUs, its own XPS map) and re-ran on any reload
# of xeu, so they are masked, not removed: the units, their udev rule and
# irqbalance's file are kept in /root/hs-tuning-units-masked-2026-10-14/
# with how to put them back.  What irq_tuning did that this setting needs,
# the card's PCIe MaxReadReq of 512, place does now.  Until the two are
# reconciled, a host with those services running is not at this setting,
# and nothing measured here says what Hammerspace's layout carries.
#
# TRANSMIT STEERING (XPS), and the inconsistency it is locked against
#
# Every reply nfsd sends and every ACK TCP sends goes out through one of a
# port's four transmit queues, and the queue decides where the work of
# sending lands: queue i's completion interrupt (Tx-i) is on CPU 2i+1 of
# that port's eight.  xeu has no queue selection of its own, so the
# kernel's applies (netdev_pick_tx() in net/core/dev.c):
#   - A socket keeps the queue it last used.  TCP lets it pick again only
#     when it has nothing in flight (skb->ooo_okay, tcp_transmit_skb()), so
#     that its packets cannot be reordered across queues.
#   - When it picks, XPS decides first: each transmit queue has a set of
#     CPUs (/sys/class/net/<port>/queues/tx-<i>/xps_cpus), and a sender on
#     one of them gets that queue.  (xps_rxqs, which would tie the transmit
#     queue to the receive queue instead, is empty on both ports.)
#   - A sender on a CPU that no queue lists gets one by the flow's hash.
# What the ports have was not chosen.  rps_xps_tuning.service writes one
# CPU to a queue at boot, when each port has eight queues: port a's tx-i
# to CPU 8+i and port b's to 32+i.  bringup.sh and port-setup.sh then cut
# each port to four queues, which leaves port a's queues tied to CPUs 8, 9,
# 10 and 11 and port b's to 32, 33, 34 and 35, and the other 40 of nfsd's
# 48 CPUs to the hash.  So a connection's replies leave by a fixed queue
# while its nfsd threads run on one of those four CPUs, and by its hash
# when they run anywhere else; and the map itself is whatever the boot, a
# reload of xeu or a run with eight rings last left (a reload rewrites it
# for the queues the port has then; eight rings and back keep or drop
# queues 4 to 7's entries).  Two runs could differ in it and nothing would
# say so.
#
# Every settled figure (60.41 GB/s on two ports, 47.39 on one) was measured
# with exactly the map above: XEU-4-VS-8-RING.md records it ("has not been
# looked at").  So that is the map locked here, as recorded and not as
# chosen; whether another map does better is an experiment of its own, to
# be run as one and then settled here.
#
# THE MODULE SETTINGS
#
# /etc/modprobe.d/sunrpc.conf gives the resting values (the one-port
# setting's); a run sets its own and puts them back.  The audit reads the
# module's live values, never the file: the svcsock KUnit suite once left
# svc_tcp_rx_rcvbuf at 0 under a file that said 1048576, and every run
# after it went without a held buffer unseen.
. "$(dirname "$0")/common.sh"

# --- the settled setting --------------------------------------------------
NFSD_CPUSET=8-55
NFSD_THREADS=512
NFSD_POOLS=512	# one pool: pools_per_node was dropped from the series on 15 October
declare -A PDEV=([a]=enp161s0f0 [b]=enp164s0f0)
declare -A PCPU=([a]=0 [b]=56)		# a port's eight CPUs start here
declare -A PXPS=([a]=8 [b]=32)		# tx-i's XPS CPU, as recorded: <this> + i
RINGS=4
MTU=7792
MRRS=512	# PCIe MaxReadReq, bytes, of the card's two functions and the eight drives
REGION_PAGES=32768
# EXP_REGION_PAGES holds another size for a run that resizes the region
# (with the port down: page_pool gives the old one back and makes the new
# one when the port's pools come up again).  The region made is checked
# too, in debugfs: a size written but not made reads as no region.
# The depth of a region pool's stack of returned pages (page_pool's, built
# in): 64 (Mike, 30 October).  It may not grow the region (Mike, 25
# October: "we cannot afford to increase the region"): FLAMEGRAPH round 04
# found 1024 saves the rings about 3 us a loaned WRITE, for about 940 more of
# the region's pages in use at peak (15 MiB), and it was not kept.  With the
# region's per-device mapping (build 504, third boot, six drives), 64 against
# 0 in copy mode on two ports kept the rate, took 3% fewer nfsd cycles a
# WRITE, and held about 23% fewer of the region's pages on average, with a
# lower peak.  It is read when a pool is made, so apply opens the ports again
# when it changes it; set by hand, it waits for the next open.
# EXP_RX_REGION_STACK holds a run to another depth.
RX_REGION_STACK=64
# The data drives' filesystems are mounted lazytime: FLAMEGRAPH round 47 (31
# October), nfsd 292.0k to 287.8k cycles a loaned WRITE (-1.5%), three runs a
# side, the rate and the region's peak the same.  Each reply's post-op
# attributes mark the ctime queried, so without it 0.75 of a WRITE takes an XFS
# transaction for its timestamps.  They are in memory until the COMMIT's fsync
# or inode writeback.  apply remounts each mounted one.  EXP_LAZYTIME=nolazytime
# holds a run without it.
LAZYTIME=lazytime
# The NFSv3 duplicate reply cache's entries (nfsd's drc_max_entries):
# FLAMEGRAPH round 12 (25 October), nfsd -3.4 us a loaned WRITE against the
# 262144 nfsd sizes itself to here, and the latency the same; it never hits
# on this rig.  Kept after the combined A/B of rounds 04 and 12 (Mike).  Read
# when nfsd starts, so apply restarts nfs-server when the live size differs.
# EXP_DRC_MAX_ENTRIES holds a run to another size (0: nfsd's own).
DRC_MAX_ENTRIES=4096
# The sunrpc module's settings that a run may change, and their values at
# rest.  A mode changes some of them (mode_expect below).
declare -A REST=(
	[svc_tcp_rx_rcvbuf]=768000	# 750 KiB from 22 October (was 1 MiB): README.md, "750 KiB on one port"
	[svc_tcp_rx_linger_us]=0
	[svc_tcp_rx_keep]=N
	[svc_tcp_rx_lowat]=0
	[svc_tcp_rx_copy_threads]=0
	[svc_tcp_rx_budget_pages]=0
	[svc_tcp_rx_budget_reserve]=0
	[svc_tcp_rx_budget_default_pages]=0
	[svc_tcp_rx_budget_default_reserve]=0
	[svc_tcp_rx_loan_pages]=Y
	[svc_tcp_rx_copy_unlocked]=Y
	[svc_tcp_rx_copy_below]=1024
	[svc_tcp_spare_bytes_max]=67108864
	[svc_tcp_rx_want_eighths]=6	# FLAMEGRAPH round 25 (26 October): three quarters of the open window
)
# Settings a later kernel no longer has: held where the kernel has them, and
# reported, not failed, where it does not.  svc_tcp_rx_copy_unlocked was
# removed after build 473 ("SUNRPC: Copy with the socket unlocked only when
# records are claimed"): on 473 and before it is Y, as every figure there was
# measured; without it a copy is made under the socket lock unless
# svc_tcp_rx_copy_threads is 1 or more.  svc_tcp_rx_lowat and
# svc_tcp_rx_copy_threads were dropped from the series on 15 October
# (patches/dropped/ 0002 and 0003): builds 474 and before have them, at 0.
# svc_tcp_rx_linger_us and svc_tcp_rx_keep followed that evening (0005 and
# 0006), after an A/B on build 474 ("15 October, evening" in README.md): on
# 474 and before they are held at 0 and N, what a kernel without them does;
# every figure before was measured at 50 us on one port and keep Y.
# svc_tcp_rx_loan_pages was removed on 22 October ("SUNRPC: let the region
# budget alone turn TCP receive loans on"): a budget above 0 is now what
# loans, so at rest, every budget 0, nothing is loaned.  On build 491 and
# before it is held at Y, as every figure there was measured; with the
# budgets at 0 a record carrying region pages was copied either way.
declare -A GONE_LATER=([svc_tcp_rx_copy_unlocked]=1 [svc_tcp_rx_lowat]=1
	[svc_tcp_rx_copy_threads]=1 [svc_tcp_rx_linger_us]=1 [svc_tcp_rx_keep]=1
	[svc_tcp_rx_loan_pages]=1)
# Settings an earlier kernel does not have yet: held where the kernel has
# them, and reported, not failed, where it does not.  svc_tcp_spare_bytes_max
# came after build 474 ("SUNRPC: Bound the pages TCP sockets keep for
# exchange with threads"); at rest it is its default, 64 MiB.
# svc_tcp_rx_want_eighths came with FLAMEGRAPH round 25 ("SUNRPC: Make the
# share of the window a loaned record waits for tunable"): the eighths of
# the open receive window an incomplete loaned record waits for.  Held at 6
# since round 25 (Mike, 26 October): against the default's 4, the half every
# earlier kernel waits for, nfsd -5.30 +- 0.89 us a loaned WRITE, the rings
# -2.2, one ACK a WRITE fewer, and the rate, region and latency the same; 7
# held a WRITE at iodepth 1 up to 5 ms, until the delayed-ACK timer.  Not in
# /etc/modprobe.d/sunrpc.conf, which would name a parameter build 499's own
# sunrpc does not know at every load of it: apply writes it.  EXP_WANT_EIGHTHS holds a run to another share.
declare -A NEW_LATER=([svc_tcp_spare_bytes_max]=1 [svc_tcp_rx_want_eighths]=1)
# The kernel's command line: rodata=noalias, so the linear map is in 32 MiB
# blocks, not 16 KiB pages (rodata=full, the default, maps it with pages on
# a CPU without BBML3, and the Neoverse N2 has none).  Since FLAMEGRAPH
# round 30 (Mike, 27 October): on the tip's sunrpc nfsd -8.83 +- 1.47 us a
# loaned WRITE and the rings -2.95 +- 0.57, page-table walks 108 to 2 a
# WRITE on nfsd's CPUs and 61 to 0.3 on the rings, the rate, region and
# latency the same.  A boot setting, so apply cannot put it in place: add
# it to the boot entry's options and reboot.  On tardis2 /etc/kernel/cmdline
# carries it, so kernel-install writes it into each new kernel's boot
# entry.  EXP_RODATA holds a run to
# another value (default, for a boot without rodata= on the command line).
# Held on tardis2 only for now (Mike, 27 October): elsewhere the audit
# prints the live value and does not hold it.
RODATA=noalias
[ $HOST = tardis2 ] || RODATA=
# The xeu module's, set by /etc/modprobe.d/xeu.conf at load.
# irq_msix_mask (kmod-xsight 2.0.1-9): a ring masks its MSI-X entry in the
# card while it polls instead of going through the irq core and the ITS;
# read when a port opens, so apply opens the ports again to put it in
# place.  Y since FLAMEGRAPH round 20 (Mike, 25 October): the rings 10 us a
# loaned WRITE cheaper, nothing else moved.  A kmod without it audits BAD.
# rx_prefetch (kmod-xsight 2.0.1-10): the receive poll prefetches the next
# frame's headers, skb_shared_info and struct page while it builds the
# current one; read at each frame, so apply writes it.  Y since FLAMEGRAPH
# round 28 (Mike, 27 October): the rings -2.45 +- 0.56 us a loaned WRITE,
# nothing else moved.  A kmod without it audits BAD.
# rx_buf_size below a page (FLAMEGRAPH round 34, kept, Mike, 27 October):
# the ring cuts its buffers from whole pages it counts once, not through
# page_pool_alloc_frag() and its drain atomic.  There is no parameter for
# it: xeu DC23D10E104D370F14FB06E and later do it.  The refill's lean loop
# (FLAMEGRAPH round 35, kept, Mike, 27 October; kmod-xsight 2.0.1-12) has
# none either: xeu 63631721F8B149913C0DC19 and later.
# rx_gro_frags (kmod-xsight 2.0.1-13): the receive poll hands GRO each
# frame's buffers as page fragments of the NAPI context's skb, so a frame
# that merges costs no skb; read at each frame, so apply writes it.  Y since
# FLAMEGRAPH round 41 (Mike, 28 October): the rings 6.5k instructions a
# loaned WRITE fewer, their cycles and nothing else moved.  A kmod without
# it audits BAD.
declare -A XEU=([rx_buf_size]=8192 [rx_ip_align]=2 [rx_refill_thresh]=8 [rss_indir_size]=128 [irq_msix_mask]=Y [rx_prefetch]=Y [rx_gro_frags]=Y)
XP=/sys/module/xeu/parameters/rx_prefetch
XG=/sys/module/xeu/parameters/rx_gro_frags
declare -A NFSD_DBG=([io_cache_read]=2 [io_cache_write]=2)
# The server's network sysctls, as every figure here was measured with them.
# netdev_max_backlog and netdev_budget come from Hammerspace's tuned profile
# hs-e1; the 16384 at the bottom of tcp_wmem and tcp_rmem is the kernel's
# one page.  tcp_wmem's 4194304 caps each NFS connection's send buffer at
# 4 MiB, which is what holds the 1 MiB random READ (14 October).
TUNED=hs-e1
[ $HOST = tardis2 ] && TUNED=	# no tuned: the sysctls are held by apply
declare -A SYSCTL=(
	[net.ipv4.tcp_wmem]="16384 16384 4194304"
	[net.ipv4.tcp_rmem]="16384 131072 33554432"
	[net.ipv4.tcp_mem]="191472 255297 382944"
	[net.core.wmem_max]=4194304
	[net.core.rmem_max]=4194304
	[net.core.wmem_default]=212992
	[net.core.rmem_default]=212992
	[net.core.optmem_max]=81920
	[net.ipv4.tcp_congestion_control]=cubic
	[net.core.default_qdisc]=fq_codel
	[net.core.netdev_max_backlog]=250000
	[net.core.netdev_budget]=600
	[net.core.netdev_budget_usecs]=20000
	[net.core.dev_weight]=64
	[net.core.busy_poll]=0
	[net.core.busy_read]=0
	[net.core.skb_defer_max]=0	# FLAMEGRAPH round 03 (23 October): a consumed skb is freed by the nfsd thread, not on its ring's CPU; the rings -15% a loaned WRITE.  128 from 9 October to then.
	[net.ipv4.tcp_limit_output_bytes]=4194304
	[net.ipv4.tcp_notsent_lowat]=4294967295
	[net.ipv4.tcp_autocorking]=1
	[net.ipv4.tcp_sack]=1
	[net.ipv4.tcp_dsack]=1
	[net.ipv4.tcp_timestamps]=1
	[net.ipv4.tcp_window_scaling]=1
	[net.ipv4.tcp_moderate_rcvbuf]=1
	[net.ipv4.tcp_adv_win_scale]=1
	[net.ipv4.tcp_slow_start_after_idle]=1
	[net.ipv4.tcp_no_metrics_save]=0
	[net.ipv4.tcp_mtu_probing]=0
	[net.ipv4.tcp_ecn]=2
	[net.ipv4.tcp_reordering]=3
	[net.ipv4.tcp_recovery]=1
	[net.ipv4.tcp_early_retrans]=3
	[net.ipv4.tcp_frto]=2
	[net.ipv4.tcp_tso_win_divisor]=3
	[net.ipv4.tcp_min_tso_segs]=2
	[net.ipv4.tcp_pacing_ss_ratio]=200
	[net.ipv4.tcp_pacing_ca_ratio]=120
	[net.ipv4.tcp_rto_min_us]=200000
)
# The client's, read over its management address and never changed from
# here ("the client untouched"): what bears on how it receives and sends.
declare -A CLI_SYSCTL=(
	[net.ipv4.tcp_rmem]="4096 131072 33554432"
	[net.ipv4.tcp_wmem]="4096 16384 4194304"
	[net.core.rmem_max]=4194304
	[net.core.wmem_max]=4194304
	[net.ipv4.tcp_congestion_control]=cubic
	[net.core.netdev_max_backlog]=1000
	[net.core.netdev_budget]=300
	[net.core.netdev_budget_usecs]=2000
	[net.core.busy_poll]=0
	[net.core.busy_read]=0
	[net.ipv4.tcp_limit_output_bytes]=4194304
	[net.ipv4.tcp_moderate_rcvbuf]=1
	[net.ipv4.tcp_sack]=1
	[net.ipv4.tcp_timestamps]=1
	[net.ipv4.tcp_ecn]=2
)
declare -A CLI_DEV=([a]=ens3np0 [b]=ens6np0)	# the client's end of each port
SRV_QDISC=fq_codel	# under the mq of each server port; apply puts it back (round 07 tries pfifo_fast)
GRO_MAX=65536	# each server port's gro_max_size and gro_ipv4_max_size (round 09 tries BIG TCP)
gro_max() { ip -d link show $1 | grep -oE 'gro_(ipv4_)?max_size [0-9]+' | awk '{print $2}' | sort -u | paste -sd,; }
set_gro_max() { ip link set dev $1 gro_max_size $2 gro_ipv4_max_size $2; }
# NAPI's deferral of a port's receive interrupts, as napi_defer_hard_irqs
# and gro_flush_timeout in ns: a poll that completes leaves the ring masked
# and NAPI polls again from the timer, unmasking only after that many polls
# in a row find nothing.  2/10000 on port a since FLAMEGRAPH round 44 (Mike,
# 29 October): the rings 10.0k cycles a loaned WRITE fewer (7.1%), the
# receive interrupts 10.7 to 1.25 a WRITE, no rate lost and the region's
# peak not raised; 2/30000 fell to 34 GB/s for seconds at a time.  Port b
# too since 30 October (Mike): in copy mode on two ports (build 504, third
# boot, six drives, stack 64), 2/10000 on both took the rings 6% fewer
# cycles a WRITE than 0/0 and nfsd 3% fewer, at the same rate and with the
# region's pages in use unchanged, in two runs that count.  Both outlive an
# open of the port; place writes them all the same.
declare -A DEFER=([a]=2/10000 [b]=2/10000)
defer() { echo "$(cat /sys/class/net/$1/napi_defer_hard_irqs)/$(cat /sys/class/net/$1/gro_flush_timeout)"; }
set_defer() { echo ${2%/*} > /sys/class/net/$1/napi_defer_hard_irqs && echo ${2#*/} > /sys/class/net/$1/gro_flush_timeout; }
# NFS is kept out of conntrack (FLAMEGRAPH round 01, 23 October): a table
# that needs conntrack puts it on every packet.  On tardis2's one-port loan
# run, deleting the table and loading zc-notrack.nft took the rings from 92.6
# to 78.7 us a WRITE and nfsd from 14.5% to 13.4% busy.  podman's netavark leaves its inet netavark table
# (NAT chains) behind after a container's network goes, until a reboot; apply
# deletes it while no container runs, and zc-notrack.nft keeps port 2049's
# packets untracked when a container brings it back: loaded only while some
# table registers conntrack hooks, since its own chains cost the rings 2.5 us
# a loaned WRITE when there is nothing to skip (round 08, 24 October).
NOTRACK=$(dirname "$(readlink -f "$0")")/zc-notrack.nft
conntrack_hooks() { nft list hooks 2> /dev/null | grep -c conntrack; }
containers() { command -v podman > /dev/null && podman ps -q 2> /dev/null | wc -l || echo 0; }
SUNRPC=/sys/module/sunrpc/parameters
DRC=/sys/module/nfsd/parameters/drc_max_entries
XM=/sys/module/xeu/parameters/irq_msix_mask

pci_of() { basename "$(readlink -f $1)"; }	# pci_of <sysfs device link>
mrrs() {	# mrrs <pci address>: its PCIe MaxReadReq in bytes, from DevCtl bits 14:12
	local v; v=$(setpci -s $1 CAP_EXP+0x08.w 2> /dev/null) || { echo unreadable; return; }
	echo $(( 128 << ((0x$v >> 12) & 7) ))
}
set_mrrs() {	# set_mrrs <pci address> <bytes>
	local v c=0; v=$(setpci -s $1 CAP_EXP+0x08.w) || return 1
	while [ $(( 128 << c )) -lt $2 ]; do c=$(( c + 1 )); done
	setpci -s $1 CAP_EXP+0x08.w=$(printf %04x $(( (0x$v & ~(7 << 12)) | (c << 12) )))
}

# --- reading the live state -------------------------------------------------
port_irqs() {	# port_irqs <dev>: "<irq> <name>" for each of the port's interrupts
	local pci; pci=$(basename "$(readlink -f /sys/class/net/$1/device)")
	for i in $(ls /sys/bus/pci/devices/$pci/msi_irqs); do
		echo "$i $(awk -v i="$i:" '$1 == i {print $NF}' /proc/interrupts)"
	done
}
xps_cpu() {	# xps_cpu <mask file>: the one CPU a mask names, "none", or "many"
	local v; v=$(tr -d ',\n' < "$1"); v=$(( 16#${v:-0} ))
	[ $v = 0 ] && { echo none; return; }
	[ $(( v & (v - 1) )) != 0 ] && { echo many; return; }
	local c=0; while [ $(( (v >> c) & 1 )) = 0 ]; do c=$(( c + 1 )); done; echo $c
}
nfsd_cpus() {	# how many of nfsd's threads are allowed which CPUs
	for t in $(pgrep -x nfsd); do taskset -cp $t 2> /dev/null | awk '{print $NF}'; done | sort | uniq -c | awk '{printf "%s%s on %s", (NR > 1 ? ", " : ""), $1, $2}'
}

# --- the audit ----------------------------------------------------------------
BAD=0; OFF=""
ok()  { printf '  ok   %-34s %s\n' "$1" "$2"; }
bad() { printf '  BAD  %-34s %s (want %s)\n' "$1" "$2" "$3"; BAD=$(( BAD + 1 )); }
want() {	# want <item> <live> <expected>
	if [ "$2" = "$3" ]; then ok "$1" "$2"; else bad "$1" "$2" "$3"; fi
}

mode_expect() {	# fill EXPECT[] and ENTRIES[] for a mode, EXP_* applied over it
	local mode=$1 k
	declare -gA EXPECT=() ENTRIES=([a]=256 [b]=256) SETTLED=()
	for k in "${!REST[@]}"; do EXPECT[$k]=${REST[$k]}; done
	case $mode in
	rest|one) ;;
	two)	EXPECT[svc_tcp_rx_rcvbuf]=$TWO_RCVBUF; EXPECT[svc_tcp_rx_linger_us]=0
		ENTRIES=([a]=128 [b]=128) ;;
	cell)	: ;;	# its plan line, below
	*)	echo "unknown mode: $mode"; exit 2 ;;
	esac
	for k in "${!EXPECT[@]}"; do SETTLED[$k]=${EXPECT[$k]}; done
	SETTLED[entries_a]=${ENTRIES[a]}; SETTLED[entries_b]=${ENTRIES[b]}
	# What a run sets on purpose
	[ -n "${EXP_RCVBUF:-}" ] && EXPECT[svc_tcp_rx_rcvbuf]=$EXP_RCVBUF
	[ -n "${EXP_LINGER:-}" ] && EXPECT[svc_tcp_rx_linger_us]=$EXP_LINGER
	[ -n "${EXP_LOWAT:-}" ] && EXPECT[svc_tcp_rx_lowat]=$EXP_LOWAT
	[ -n "${EXP_COPY_THREADS:-}" ] && EXPECT[svc_tcp_rx_copy_threads]=$EXP_COPY_THREADS
	[ -n "${EXP_KEEP:-}" ] && EXPECT[svc_tcp_rx_keep]=$EXP_KEEP
	[ -n "${EXP_WANT_EIGHTHS:-}" ] && EXPECT[svc_tcp_rx_want_eighths]=$EXP_WANT_EIGHTHS
	[ -n "${EXP_SPARE:-}" ] && EXPECT[svc_tcp_spare_bytes_max]=$EXP_SPARE
	[ -n "${EXP_BUDGET:-}" ] && EXPECT[svc_tcp_rx_budget_pages]=$EXP_BUDGET
	[ -n "${EXP_BUDGET_RESERVE:-}" ] && EXPECT[svc_tcp_rx_budget_reserve]=$EXP_BUDGET_RESERVE
	[ -n "${EXP_ENTRIES_A:-${EXP_ENTRIES:-}}" ] && ENTRIES[a]=${EXP_ENTRIES_A:-$EXP_ENTRIES}
	[ -n "${EXP_ENTRIES_B:-${EXP_ENTRIES:-}}" ] && ENTRIES[b]=${EXP_ENTRIES_B:-$EXP_ENTRIES}
	for k in "${!EXPECT[@]}"; do
		[ "${EXPECT[$k]}" = "${SETTLED[$k]}" ] || OFF="$OFF $k=${EXPECT[$k]}"
	done
	[ "${ENTRIES[a]}" = "${SETTLED[entries_a]}" ] || OFF="$OFF rings_a=${ENTRIES[a]}"
	[ "${ENTRIES[b]}" = "${SETTLED[entries_b]}" ] || OFF="$OFF rings_b=${ENTRIES[b]}"
	[ -n "${EXP_NFSD_CPUS:-}" ] && OFF="$OFF nfsd_cpus=$EXP_NFSD_CPUS"
	[ -n "${EXP_REFILL:-}" ] && [ "$EXP_REFILL" != "${XEU[rx_refill_thresh]}" ] && OFF="$OFF rx_refill_thresh=$EXP_REFILL"
	[ "${EXP_NOTRACK:-}" = absent ] && OFF="$OFF zc_notrack=absent"
	[ "${EXP_NETAVARK:-}" = present ] && OFF="$OFF netavark=present"
	[ -n "${EXP_GRO_MAX:-}" ] && [ "$EXP_GRO_MAX" != "$GRO_MAX" ] && OFF="$OFF gro_max=$EXP_GRO_MAX"
	for k in $PORTS; do [ -n "${EXP_DEFER:-}" ] && [ "$EXP_DEFER" != "${DEFER[$k]}" ] && OFF="$OFF defer_$k=$EXP_DEFER"; done
	[ -n "${EXP_SRV_QDISC:-}" ] && [ "$EXP_SRV_QDISC" != "$SRV_QDISC" ] && OFF="$OFF srv_qdisc=$EXP_SRV_QDISC"
	[ -n "${EXP_SKB_DEFER_MAX:-}" ] && [ "$EXP_SKB_DEFER_MAX" != "${SYSCTL[net.core.skb_defer_max]}" ] && OFF="$OFF skb_defer_max=$EXP_SKB_DEFER_MAX"
	[ -n "${EXP_RODATA:-}" ] && [ "$EXP_RODATA" != "$RODATA" ] && OFF="$OFF rodata=$EXP_RODATA"
	[ -n "${EXP_RX_REGION_STACK:-}" ] && [ "$EXP_RX_REGION_STACK" != "$RX_REGION_STACK" ] && OFF="$OFF rx_region_stack=$EXP_RX_REGION_STACK"
	[ -n "${EXP_REGION_PAGES:-}" ] && [ "$EXP_REGION_PAGES" != "$REGION_PAGES" ] && OFF="$OFF region_pages=$EXP_REGION_PAGES"
	[ -n "${EXP_DRC_MAX_ENTRIES:-}" ] && [ "$EXP_DRC_MAX_ENTRIES" != "$DRC_MAX_ENTRIES" ] && OFF="$OFF drc_max_entries=$EXP_DRC_MAX_ENTRIES"
	local xk xv
	for xk in "${!XEU[@]}"; do
		xv=EXP_XEU_$xk
		[ -n "${!xv:-}" ] && [ "${!xv}" != "${XEU[$xk]}" ] && OFF="$OFF $xk=${!xv}"
	done
	[ -n "${EXP_LAYOUT:-}" ] && OFF="$OFF layout=$EXP_LAYOUT"
	[ -n "${EXP_QDISC:-}" ] && [ "$EXP_QDISC" != fq_codel ] && OFF="$OFF client_qdisc=$EXP_QDISC"
	[ -n "${EXP_QDISC_A:-}" ] && [ "$EXP_QDISC_A" != fq_codel ] && OFF="$OFF client_qdisc_a=$EXP_QDISC_A"
	[ -n "${EXP_QDISC_B:-}" ] && [ "$EXP_QDISC_B" != fq_codel ] && OFF="$OFF client_qdisc_b=$EXP_QDISC_B"
	return 0
}

audit_nfsd() {
	local n; n=$(pgrep -x nfsd | wc -l)
	want "nfsd threads" "$n" "$NFSD_THREADS"
	want "nfsd threads in each pool" "$(cat /proc/fs/nfsd/pool_threads 2>/dev/null)" "$NFSD_POOLS"
	want "nfsd threads' CPUs" "$(nfsd_cpus)" "$n on ${EXP_NFSD_CPUS:-$NFSD_CPUSET}"
	local k; for k in "${!NFSD_DBG[@]}"; do
		want "nfsd $k" "$(cat /sys/kernel/debug/nfsd/$k 2>/dev/null)" "${NFSD_DBG[$k]}"
	done
	want "nfsd max_block_size" "$(cat /proc/fs/nfsd/max_block_size 2>/dev/null)" 1048576
	local drc=${EXP_DRC_MAX_ENTRIES:-$DRC_MAX_ENTRIES}
	if [ -e $DRC ]; then
		want "nfsd drc_max_entries" "$(cat $DRC)" "$drc"
		# the size nfsd started with, which a change of the parameter waits on
		[ "$drc" = 0 ] || want "nfsd reply cache entries" "$(drc_live)" "$drc"
	else
		printf '  --   %-34s %s\n' "nfsd drc_max_entries" "not in this kernel"
	fi
}

audit_sysctl() {
	local k w
	local t; t=$(tuned-adm active 2>/dev/null | sed -n 's/^Current active profile: //p')
	want "tuned profile" "${t:-none}" "${TUNED:-none}"
	for k in $(printf '%s\n' "${!SYSCTL[@]}" | sort); do
		w=${SYSCTL[$k]}; [ $k = net.core.skb_defer_max ] && w=${EXP_SKB_DEFER_MAX:-$w}
		want "sysctl $k" "$(sysctl -n $k 2>/dev/null | tr -s ' \t' '  ')" "$w"
	done
}

audit_client() {	# read only: nothing on the client is changed from here
	local out k p
	out=$($SSHC "for k in ${!CLI_SYSCTL[*]}; do echo \"\$k \$(sysctl -n \$k | tr -s ' \\t' '  ')\"; done; for d in ${CLI_DEV[*]}; do echo \"qdisc_\$d \$(tc qdisc show dev \$d | awk '\$1 == \"qdisc\" && \$2 != \"mq\" {print \$2}' | sort -u | paste -sd,)\"; echo \"pause_\$d \$(ethtool -a \$d | awk '/^RX:/ {r = \$2} /^TX:/ {t = \$2} END {print r \"/\" t}')\"; done" 2>/dev/null) ||
		{ bad "client reachable at $CLIMGMT" no yes; return; }
	for k in $(printf '%s\n' "${!CLI_SYSCTL[@]}" | sort); do
		want "client sysctl $k" "$(awk -v k=$k '$1 == k {$1 = ""; sub(/^ /, ""); print}' <<< "$out")" "${CLI_SYSCTL[$k]}"
	done
	for p in $PORTS; do
		local eq=EXP_QDISC_$(tr a-z A-Z <<< $p)
		want "client ${CLI_DEV[$p]} qdisc" "$(awk -v k=qdisc_${CLI_DEV[$p]} '$1 == k {print $2}' <<< "$out")" "${!eq:-${EXP_QDISC:-fq_codel}}"
		want "client ${CLI_DEV[$p]} pause rx/tx" "$(awk -v k=pause_${CLI_DEV[$p]} '$1 == k {print $2}' <<< "$out")" off/off
	done
}

audit_netfilter() {
	local c; c=$(containers)
	# zc_notrack only while some table registers conntrack (round 08):
	# with none, its chains cost every packet for nothing.
	local w=0; [ "$(conntrack_hooks)" != 0 ] && [ "${EXP_NOTRACK:-}" != absent ] && w=4
	want "NFS untracked while conntrack is on" "$(nft list table inet zc_notrack 2> /dev/null | grep -c 'port 2049 notrack')" $w
	if [ $c = 0 ]; then
		want "podman's leftover netavark table" "$(nft list table inet netavark > /dev/null 2>&1 && echo present || echo none)" ${EXP_NETAVARK:-none}
	else
		ok "podman's netavark table" "kept: $c container(s) running"
	fi
}

audit_irqbalance() {
	local p; p=$(pidof irqbalance)
	want "irqbalance running" "${p:+yes}" yes
	[ -n "$p" ] || return
	want "irqbalance leaves xeu alone" "$(tr '\0' '\n' < /proc/$p/cmdline | grep -cx -- --banmod=xeu)" 1
	want "irqbalance kept off nfsd's CPUs" "$(tr '\0' '\n' < /proc/$p/environ | sed -n 's/^IRQBALANCE_BANNED_CPULIST=//p')" "$NFSD_CPUSET"
}

srv_qdisc() {	# srv_qdisc <dev>: the qdiscs under the port's mq, one name if all alike
	tc qdisc show dev $1 | awk '$1 == "qdisc" && $2 != "mq" {print $2}' | sort -u | paste -sd,
}
set_srv_qdisc() {	# set_srv_qdisc <dev> <qdisc>: that qdisc on each of the port's transmit queues
	local c
	# noqueue cannot be a class's: as the root it is every queue's (FLAMEGRAPH round 31).
	if [ "$2" = noqueue ]; then tc qdisc replace dev $1 root noqueue; return; fi
	# A root of handle 0: cannot be named as a parent: give the mq one.
	tc qdisc replace dev $1 root handle 100: mq || return 1
	for c in $(tc qdisc show dev $1 | awk '$1 == "qdisc" && $2 != "mq" {sub(/^100:/, "", $5); print $5}'); do
		tc qdisc replace dev $1 parent 100:$c $2 || return 1
	done
}

audit_port() {	# audit_port <a|b>
	local p=$1 d=${PDEV[$1]} c0=${PCPU[$1]} irq name i want_cpu q
	want "$d mtu" "$(cat /sys/class/net/$d/mtu)" $MTU
	want "$d qdisc" "$(srv_qdisc $d)" "${EXP_SRV_QDISC:-$SRV_QDISC}"
	want "$d GRO max size" "$(gro_max $d)" "${EXP_GRO_MAX:-$GRO_MAX}"
	want "$d NAPI deferral" "$(defer $d)" "${EXP_DEFER:-${DEFER[$p]}}"
	want "$d rings" "$(ethtool -l $d | awk '/^Combined:/ {v = $2} END {print v}')" ${EXP_RINGS:-$RINGS}
	want "$d ring depth" "$(ethtool -g $d | awk '/^RX:/ {v = $2} END {print v}')" "${ENTRIES[$p]}"
	want "$d threaded NAPI" "$(cat /sys/class/net/$d/threaded)" 0
	# Pause is off at both ends: it was left tx on from 11 October's pause
	# experiment until 15 October unseen (no pause frame ever reached the
	# client, so nothing measured changed, but the setting is off).
	want "$d pause rx/tx" "$(ethtool -a $d | awk '/^RX:/ {r = $2} /^TX:/ {t = $2} END {print r "/" t}')" off/off
	for q in /sys/class/net/$d/queues/rx-*; do
		want "$d $(basename $q) RPS" "$(xps_cpu $q/rps_cpus)" none
	done
	[ -n "${EXP_LAYOUT:-}" ] && { echo "  --   $d interrupts and XPS: not held ($EXP_LAYOUT)"; return; }
	while read -r irq name; do
		case "$name" in
		$d-Rx-*) i=${name##*-}; want_cpu=$(( c0 + 2 * i )) ;;
		$d-Tx-*) i=${name##*-}; want_cpu=$(( c0 + 2 * i + 1 )) ;;
		$d-async) want_cpu=$(( c0 + 1 )) ;;	# with Tx-0
		*) continue ;;
		esac
		want "$name CPU" "$(cat /proc/irq/$irq/smp_affinity_list)/$(cat /proc/irq/$irq/effective_affinity_list)" "$want_cpu/$want_cpu"
	done < <(port_irqs $d | sort -k2 -V)
	for q in /sys/class/net/$d/queues/tx-*; do
		i=${q##*-}
		want "$d tx-$i XPS CPU" "$(xps_cpu $q/xps_cpus)" $(( ${PXPS[$p]} + i ))
		want "$d tx-$i XPS by receive queue" "$(xps_cpu $q/xps_rxqs)" none
	done
}

audit() {
	local mode=$1 k
	mode_expect $mode
	echo "settled.sh audit $mode: kernel $(uname -r), $(date '+%F %T')"
	k=$(grep -oE '(^| )rodata=[^ ]*' /proc/cmdline | tail -1 | sed 's/.*=//' | grep . || echo default)
	if [ -n "${EXP_RODATA:-$RODATA}" ]; then
		want "kernel rodata" "$k" "${EXP_RODATA:-$RODATA}"
	else
		printf '  --   %-34s %s\n' "kernel rodata" "$k (not held on $HOST)"
	fi
	audit_nfsd
	audit_irqbalance
	audit_netfilter
	audit_sysctl
	audit_client
	for k in $PORTS; do audit_port $k; done
	for k in $(printf '%s\n' "${!EXPECT[@]}" | sort); do
		if [ ! -e $SUNRPC/$k ] && [ -n "${GONE_LATER[$k]:-}${NEW_LATER[$k]:-}" ]; then
			printf '  --   %-34s %s\n' "sunrpc $k" "not in this kernel"
			continue
		fi
		want "sunrpc $k" "$(cat $SUNRPC/$k 2>/dev/null)" "${EXPECT[$k]}"
	done
	for k in $(printf '%s\n' "${!XEU[@]}" | sort); do
		w=${XEU[$k]}; [ $k = rx_refill_thresh ] && w=${EXP_REFILL:-$w}
		v=EXP_XEU_$k; w=${!v:-$w}
		if [ "$w" = absent ]; then	# a kmod from before the parameter, held to not having it
			want "xeu $k" "$(cat /sys/module/xeu/parameters/$k 2>/dev/null || echo absent)" absent
			continue
		fi
		want "xeu $k" "$(cat /sys/module/xeu/parameters/$k 2>/dev/null)" "$w"
	done
	want "receive region pages" "$(cat $PP/rx_region_pages)" ${EXP_REGION_PAGES:-$REGION_PAGES}
	want "receive region made" "$(awk '$1 == "pages" {print $2}' /sys/kernel/debug/page_pool_rx_region 2>/dev/null || echo none)" ${EXP_REGION_PAGES:-$REGION_PAGES}
	if [ -e $PP/rx_region_stack ]; then
		want "receive region stack" "$(cat $PP/rx_region_stack)" "${EXP_RX_REGION_STACK:-$RX_REGION_STACK}"
	else
		printf '  --   %-34s %s\n' "receive region stack" "not in this kernel"
	fi
	want "receive region devices" "$(tr ',' '\n' < $PP/rx_region_dev | grep . | sort | paste -sd ' ')" \
		"$(for p in $PORTS; do basename "$(readlink -f /sys/class/net/${PDEV[$p]}/device)"; done | sort | paste -sd ' ')"
	local drv; drv=$(data_drives) || bad "data drives" "$drv" "$DATA_DRIVES"
	for k in $drv; do
		want "$k cache" "$(cat /sys/block/$k/queue/write_cache)" "write through"
		[ "$(mounted_lazytime $k)" = unmounted ] || want "$k mount" "$(mounted_lazytime $k)" "${EXP_LAZYTIME:-$LAZYTIME}"
		want "$k PCIe MaxReadReq" "$(mrrs $(pci_of /sys/block/$k/device/device))" $MRRS
	done
	for k in $PORTS; do
		want "${PDEV[$k]} PCIe MaxReadReq" "$(mrrs $(pci_of /sys/class/net/${PDEV[$k]}/device))" $MRRS
	done
	[ $mode = rest ] && want "NFS connections" "$(ss -Htn state established '( sport = :2049 )' | wc -l)" 0
	if [ $BAD != 0 ]; then
		echo "settled.sh audit $mode: FAILED, $BAD item(s) BAD"
		return 1
	fi
	if [ -n "$OFF" ]; then
		echo "settled.sh audit $mode: as the run asks, which is off the settled setting in:$OFF"
	else
		echo "settled.sh audit $mode: the settled setting"
	fi
}

# --- putting it in place --------------------------------------------------------
place() {	# place [a] [b]: the CPU layout of nfsd and of the ports named (both)
	local n p d c0 irq name q i m
	n=$(pgrep -x nfsd | wc -l)
	# Without hammerspace-nfsd-tuning (tardis2) the count is /etc/nfs.conf's,
	# and is put back here; the pinning below is the same either way.
	if [ "$n" != $NFSD_THREADS ] && [ "$n" != 0 ] && ! systemctl cat hammerspace-nfsd-tuning.service > /dev/null 2>&1; then
		echo $NFSD_THREADS > /proc/fs/nfsd/threads; sleep 1; n=$(pgrep -x nfsd | wc -l)
	fi
	[ "$n" = $NFSD_THREADS ] || { echo "nfsd has $n threads, not $NFSD_THREADS: $(systemctl cat hammerspace-nfsd-tuning.service > /dev/null 2>&1 && echo systemctl restart hammerspace-nfsd-tuning || echo systemctl start nfs-server)"; return 1; }
	for t in $(pgrep -x nfsd); do taskset -pc $NFSD_CPUSET $t > /dev/null; done
	for p in ${*:-$PORTS}; do
		d=${PDEV[$p]:?port a or b}; c0=${PCPU[$p]}
		[ "$(ethtool -l $d | awk '/^Combined:/ {v = $2} END {print v}')" = $RINGS ] ||
			{ echo "$d does not have $RINGS rings: port-setup.sh (bringup.sh for port a on a fresh boot)"; return 1; }
		# The driver puts a port's interrupts, the async one too, back on
		# CPUs 0-7 at every open; XPS, RPS and threading outlive an open
		# but are set here all the same.
		"$RIG/ring-irqs.sh" $d $c0 > /dev/null
		while read -r irq name; do
			[ "$name" = $d-async ] && echo $(( c0 + 1 )) > /proc/irq/$irq/smp_affinity_list	# with Tx-0
		done < <(port_irqs $d)
		for q in /sys/class/net/$d/queues/tx-*; do
			i=${q##*-}
			m=$(( 1 << (${PXPS[$p]} + i) ))	# as the kernel prints a mask: 32-bit words, high first
			printf '%08x,%08x\n' $(( (m >> 32) & 0xffffffff )) $(( m & 0xffffffff )) > $q/xps_cpus
			echo 0 > $q/xps_rxqs
		done
		for q in /sys/class/net/$d/queues/rx-*; do echo 0 > $q/rps_cpus; done
		echo 0 > /sys/class/net/$d/threaded
		set_defer $d ${DEFER[$p]}
		# irq_tuning.service set this until it was masked (below); a reload
		# of xeu puts it back to the card's 256.
		set_mrrs $(pci_of /sys/class/net/$d/device) $MRRS
		ethtool -A $d rx off tx off 2> /dev/null
	done
}

drc_live() { awk -F: '/^max entries/ {print $2 + 0}' /proc/fs/nfsd/reply_cache_stats 2>/dev/null; }

# The two sizes read only when nfsd starts and when a pool is made, and
# xeu's irq_msix_mask, read when a port opens: put in place by restarting
# nfs-server and opening the ports again, so only with nothing connected.
# Before place, which both undo.  A value written to irq_msix_mask by hand
# without an open is not what the rings run; the audit cannot tell.
sizes() {
	local drc=${EXP_DRC_MAX_ENTRIES:-$DRC_MAX_ENTRIES} stk=${EXP_RX_REGION_STACK:-$RX_REGION_STACK} p
	local idle; [ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] && idle=1
	if [ -e $DRC ] && { [ "$(cat $DRC)" != "$drc" ] || { [ "$drc" != 0 ] && [ "$(drc_live)" != "$drc" ]; }; }; then
		if [ -n "${idle:-}" ]; then
			echo "$drc" > $DRC
			echo "nfsd drc_max_entries $drc: restarting nfs-server"
			systemctl restart nfs-server || return 1
		else
			echo "NFS connections are up: nfsd's reply cache is left as it is"
		fi
	fi
	local msx=${EXP_XEU_irq_msix_mask:-${XEU[irq_msix_mask]}} why=
	[ -e $PP/rx_region_stack ] && [ "$(cat $PP/rx_region_stack)" != "$stk" ] && why="rx_region_stack $stk"
	[ -e $XM ] && [ "$(cat $XM)" != "$msx" ] && why="${why:+$why, }xeu irq_msix_mask $msx"
	if [ -n "$why" ]; then
		if [ -n "${idle:-}" ]; then
			[ -e $PP/rx_region_stack ] && echo "$stk" > $PP/rx_region_stack
			[ -e $XM ] && echo "$msx" > $XM
			for p in $PORTS; do
				echo "$why: opening ${PDEV[$p]} again"
				ip link set ${PDEV[$p]} down && sleep 2 && ip link set ${PDEV[$p]} up || return 1
			done
			sleep 10
		else
			echo "NFS connections are up: $why left as it is"
		fi
	fi
}

mounted_lazytime() {	# DRIVE: lazytime, nolazytime, or unmounted
	local o; o=$(findmnt -rn -o OPTIONS /dev/$1 | head -1)
	[ -n "$o" ] || { echo unmounted; return; }
	case ",$o," in *,lazytime,*) echo lazytime ;; *) echo nolazytime ;; esac
}
set_lazytime() {	# remount each mounted data drive to EXP_LAZYTIME or the settled option
	local k m w=${EXP_LAZYTIME:-$LAZYTIME}
	for k in $(data_drives); do
		m=$(findmnt -rn -o TARGET /dev/$k | head -1)
		[ -n "$m" ] && [ "$(mounted_lazytime $k)" != "$w" ] && mount -o remount,$w "$m"
	done
}
apply() {	# the sizes, the CPU layout, the resting module settings, and the audit
	local k
	sizes || return 1
	place || return 1
	for k in "${!SYSCTL[@]}"; do
		[ "$(sysctl -n $k | tr -s ' \t' '  ')" = "${SYSCTL[$k]}" ] || sysctl -qw "$k=${SYSCTL[$k]}"
	done
	for k in $PORTS; do [ "$(srv_qdisc ${PDEV[$k]})" = "$SRV_QDISC" ] || set_srv_qdisc ${PDEV[$k]} $SRV_QDISC; done
	for k in $PORTS; do [ "$(gro_max ${PDEV[$k]})" = "$GRO_MAX" ] || set_gro_max ${PDEV[$k]} $GRO_MAX; done
	k=${EXP_XEU_rx_prefetch:-${XEU[rx_prefetch]}}
	[ -w $XP ] && [ "$(cat $XP)" != "$k" ] && echo "$k" > $XP
	k=${EXP_XEU_rx_gro_frags:-${XEU[rx_gro_frags]}}
	[ -w $XG ] && [ "$(cat $XG)" != "$k" ] && echo "$k" > $XG
	[ "$(containers)" = 0 ] && nft list table inet netavark > /dev/null 2>&1 && nft delete table inet netavark
	if [ "$(conntrack_hooks)" != 0 ]; then
		[ "$(nft list table inet zc_notrack 2> /dev/null | grep -c 'port 2049 notrack')" = 4 ] || nft -f "$NOTRACK"
	else
		nft delete table inet zc_notrack 2> /dev/null
	fi
	if [ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ]; then
		for k in "${!REST[@]}"; do
			[ -w $SUNRPC/$k ] && [ "$(cat $SUNRPC/$k)" != "${REST[$k]}" ] && echo "${REST[$k]}" > $SUNRPC/$k
		done
		for k in "${!NFSD_DBG[@]}"; do
			[ "$(cat /sys/kernel/debug/nfsd/$k 2>/dev/null)" = "${NFSD_DBG[$k]}" ] || echo "${NFSD_DBG[$k]}" > /sys/kernel/debug/nfsd/$k
		done
	else
		echo "NFS connections are up: the sunrpc settings are left as they are"
	fi
	set_lazytime
	audit rest
}

show() {
	BAD=0; audit "${1:-rest}" 2>&1 | sed -n 's/^  \(ok\|BAD\) *//p'
}

case "${1:-}" in
audit) audit "${2:?mode: rest, two, one or cell}" ;;
apply) apply ;;
place) shift; place "$@" ;;
gro)   set_gro_max "${2:?dev}" "${3:-$GRO_MAX}" ;;	# no size: the settled one
defer) set_defer "${2:?dev}" "${3:?napi_defer_hard_irqs/gro_flush_timeout}" ;;
srvq)  set_srv_qdisc "${2:?dev}" "${3:-$SRV_QDISC}" ;;	# no qdisc: the settled one
show)  show "${2:-rest}" ;;
*)     sed -n '2,36p' "$0"; exit 2 ;;
esac
