# Shared by the xeu-rig scripts.  Source it.
# The rules (XEU_CONVERGENCE.md section 0): load goes only to the
# harness's own mount.  Both ports are ours from 2026-10-11 (the default
# here is enp161s0f0; D, SRVIP and CLIIP name the other); check that no
# NFS client is connected before a port is reconfigured.
D=${D:-enp161s0f0}
SRVIP=${SRVIP:-192.168.1.211}
CLIIP=${CLIIP:-192.168.1.106}
# The client's management address (its ens8f0; the server's is 10.20.12.211
# on xemi0).  Every script's ssh and scp to the client goes here, so that the
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
bounce() { ip link set $D down; sleep 1; ip link set $D up; sleep 6; ping -c2 -W2 $CLIIP > /dev/null; }
set_ring() {	# set_ring <entries>: only with the port up
	[ "$(rx_entries)" = "$1" ] && return 0
	ethtool -G $D rx $1 || return 1
	sleep 6
	ping -c2 -W2 $CLIIP > /dev/null
}
has_retry_patch() { grep -q 'xeu_rx_refill_timer' /proc/kallsyms; }
