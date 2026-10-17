# Shared by the xeu-rig scripts.  Source it.
# The rules (XEU_CONVERGENCE.md section 0): load goes only to the
# harness's own mount.  Both ports are ours from 2026-10-11 (the default
# here is enp161s0f0; D, SRVIP and CLIIP name the other); check that no
# NFS client is connected before a port is reconfigured.
D=${D:-enp161s0f0}
# What differs between the two servers that share the client (2026-10-17).
# tardis1: the card, eight data drives nvme0 to nvme7, .211 on each port.
# tardis2: the same card and CPUs, six data drives (nvme0 to nvme3 WD_BLACK
# SN8100 without SGLs, nvme4 and nvme5 DapuStor DP800 with SGLs; nvme6 is
# its boot drive), .213 on each port, and port b not cabled.
HOST=$(hostname -s)
case $HOST in
tardis2) SRVHOST=213; DATA_DRIVES="0 1 2 3 4 5"; PORTS=${PORTS:-a} ;;
*)       SRVHOST=211; DATA_DRIVES="0 1 2 3 4 5 6 7"; PORTS=${PORTS:-a b} ;;
esac
# data_drives: the data drives' block devices, refusing any that holds a
# mounted system filesystem (a drive renamed at boot must not be taken for
# a data drive)
data_drives() {
	local n
	for n in $DATA_DRIVES; do
		findmnt -rno TARGET -S /dev/nvme${n}n1 2>/dev/null | grep -qvx "/hs/nvme$n" &&
			{ echo "nvme${n}n1 is mounted outside /hs: not a data drive" >&2; return 1; }
		lsblk -nro MOUNTPOINT /dev/nvme${n}n1 2>/dev/null | grep -qE '^/(boot.*)?$' &&
			{ echo "nvme${n}n1 holds a system filesystem: not a data drive" >&2; return 1; }
		echo nvme${n}n1
	done
}
SRVIP=${SRVIP:-192.168.1.$SRVHOST}
CLIIP=${CLIIP:-192.168.1.106}
# The client's management address (its ens8f0; tardis1's is 10.20.12.211
# on xemi0, tardis2's 10.20.12.213 on enaxsl1i0).  Every script's ssh and scp to the client goes here, so that the
# harness's own traffic is not on a port under test (2026-10-13).  CLIIP stays
# the client's address on the port: what is mounted from and pinged.
CLIMGMT=${CLIMGMT:-10.20.12.106}
KEY=${KEY:-/root/.ssh/id_ed25519_snitzer2}
export SSH_OPTS="-i $KEY"
SSHC="ssh -o BatchMode=yes -o ConnectTimeout=10 -i $KEY root@$CLIMGMT"
RG=/sys/kernel/debug/page_pool_rx_region
PP=/sys/module/page_pool/parameters
L=${L:-/var/tmp/zc-run-logs}
RIG=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
H=$RIG/..
PS=$(getconf PAGESIZE)

reg() { awk -v k="$1" '$1 == k {print $2}' $RG; }
rx_entries() { ethtool -g $D | awk '/^RX:/{v=$2} END{print v}'; }
rx_rings() { ethtool -l $D | awk '/Combined/{v=$2} END{print v}'; }
# Ring counters are zeroed by a ring change or a down/up: read them before.
tail_drops() { ethtool -S $D | awk -F': ' '/rx_hw_tail_drop_packets/{s+=$2} END{print s+0}'; }
ring_packets() { ethtool -S $D | awk -F': ' '/ring_[0-9]+_rx_packets/{printf "%s ", $2}'; }
# A ring that ran empty never refills on a driver without the retry patch
# (XEU_CONVERGENCE.md 5c).  Down and up reposts every ring.
# An open of the port puts its interrupts back on CPUs 0-7: both put the
# settled CPU layout back after it (settled.sh).
port_of() { case $1 in enp161s0f0) echo a ;; enp164s0f0) echo b ;; esac; }
bounce() { ip link set $D down; sleep 1; ip link set $D up; sleep 6; "$RIG/settled.sh" place $(port_of $D) || return 1; ping -c2 -W2 $CLIIP > /dev/null; }
set_ring() {	# set_ring <entries>: only with the port up
	[ "$(rx_entries)" = "$1" ] && return 0
	ethtool -G $D rx $1 || return 1
	sleep 6
	"$RIG/settled.sh" place $(port_of $D) || return 1
	ping -c2 -W2 $CLIIP > /dev/null
}
# audit_or_die <mode> <file>: settled.sh's audit into <file>; refuse to go on
audit_or_die() { "$RIG/settled.sh" audit $1 > $2 2>&1 || { grep -E 'BAD|FAILED' $2; exit 1; }; tail -1 $2; }
has_retry_patch() { grep -q 'xeu_rx_refill_timer' /proc/kallsyms; }
