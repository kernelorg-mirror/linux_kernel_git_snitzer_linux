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
# Each port: the server's interface and address, the client's address, and
# the filesystems a run on it writes to by default (twoport.sh's FS_A and
# FS_B).  tardis1 gives each port four drives; tardis2's one port has all
# six (FS_A="/hs/nvme4 /hs/nvme5" for the SGL drives alone).
declare -A PDEVS=([a]=enp161s0f0 [b]=enp164s0f0)
declare -A SRVP=([a]=192.168.1.$SRVHOST [b]=192.168.0.$SRVHOST) CLIP=([a]=192.168.1.106 [b]=192.168.0.106)
ALL_FS=$(for n in $DATA_DRIVES; do printf '/hs/nvme%s ' $n; done); ALL_FS=${ALL_FS% }
case $HOST in
tardis2) declare -A PFS=([a]="$ALL_FS" [b]="") ;;
*)       declare -A PFS=([a]="/hs/nvme0 /hs/nvme1 /hs/nvme2 /hs/nvme3" [b]="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7") ;;
esac
DEFWHICH=$(tr -d ' ' <<< "$PORTS")	# a script's default ports: "ab" or "a"
# which_ok <a|b|ab>: the ports asked for are this host's
which_ok() {
	local p; [ -n "$1" ] || return 1
	for p in $(sed 's/./& /g' <<< "$1"); do
		case " $PORTS " in *" $p "*) ;; *) echo "port $p is not one of $HOST's ($PORTS)" >&2; return 1 ;; esac
	done
}
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
SRVIP=${SRVIP:-${SRVP[a]}}
CLIIP=${CLIIP:-${CLIP[a]}}
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
# client_free: tardis1 and tardis2 share the client, and a run cleans up the
# client's /mnt/rxr-* mounts, its fio, iperf3 and senders before it starts,
# which would end the other server's run.  Refuse while the client has an NFS
# mount from any address but this host's, or one of those load tools running.
client_free() {
	local out
	out=$($SSHC "findmnt -rn -t nfs,nfs4 -o SOURCE | cut -d: -f1 | sort -u | paste -sd ' '; pgrep -l -f '^(fio|iperf3)( |$)|zc-snd[.]py|wire-snd[.]py' | grep -v pgrep | paste -sd ' '" < /dev/null) ||
		{ echo "client $CLIMGMT does not answer" >&2; return 1; }
	local m p; m=$(sed -n 1p <<< "$out"); p=$(sed -n 2p <<< "$out")
	for a in $m; do
		[ "$a" = "${SRVP[a]}" ] || [ "$a" = "${SRVP[b]}" ] ||
			{ echo "the client has NFS mounted from $a, not $HOST: another server's run? ($m)" >&2; return 1; }
	done
	[ -z "$p" ] || { echo "the client is running load already: $p" >&2; return 1; }
}
has_retry_patch() { grep -q 'xeu_rx_refill_timer' /proc/kallsyms; }
