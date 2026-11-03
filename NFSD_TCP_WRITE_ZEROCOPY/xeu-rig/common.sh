# Shared by the xeu-rig scripts.  Source it.
# The rules (XEU_CONVERGENCE.md section 0): load goes only to the
# harness's own mount.  Both ports are ours from 2026-10-11 (the default
# here is enp161s0f0; D, SRVIP and CLIIP name the other); check that no
# NFS client is connected before a port is reconfigured.
D=${D:-enp161s0f0}
# What differs between the two servers that share the client (2026-10-17).
# tardis1: the card, eight data drives nvme0 to nvme7, .211 on each port.
# tardis2: the same card and CPUs, eight data drives (from 2026-11-02:
# nvme0 to nvme7 all DapuStor DPRP5 15.36 TB with SGLs; the boot drive, a
# Micron, is nvme8), .213 on each port, and both ports cabled.
HOST=$(hostname -s)
case $HOST in
tardis2) SRVHOST=213; DATA_DRIVES="0 1 2 3 4 5 6 7"; PORTS=${PORTS:-a b} ;;
*)       SRVHOST=211; DATA_DRIVES="0 1 2 3 4 5 6 7"; PORTS=${PORTS:-a b} ;;
esac
# Each port: the server's interface and address, the client's address, and
# the filesystems a run on it writes to by default (twoport.sh's FS_A and
# FS_B).  Each host gives each port four drives.
declare -A PDEVS=([a]=enp161s0f0 [b]=enp164s0f0)
declare -A SRVP=([a]=192.168.1.$SRVHOST [b]=192.168.0.$SRVHOST) CLIP=([a]=192.168.1.106 [b]=192.168.0.106)
declare -A PFS=([a]="/hs/nvme0 /hs/nvme1 /hs/nvme2 /hs/nvme3" [b]="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7")
# The two-port setting's held buffer and connections a port (each port's
# client mount's nconnect, which the server cannot set).  tardis2 from 3
# November (Mike, FLAMEGRAPH round 56): 12 connections a port at 512 KiB
# on rings of 128 carried 66.4 and 70.5 GB/s in loan mode, against 62.96 at
# 16 and 384 KiB, with 48 to 49 MiB of the region in use against 55 and nfsd
# 272k to 279k cycles a WRITE against 282k.  tardis1 keeps 16 at 384 KiB,
# measured in copy mode, until it is measured there.
case $HOST in
tardis2) TWO_RCVBUF=524288; TWO_NCONN=12 ;;
*)       TWO_RCVBUF=393216; TWO_NCONN=16 ;;
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
# a data drive).  The drive of /hs/nvmeN is the one mounted there, which
# after a boot that renamed the drives is not nvmeNn1 (tardis2 on 27
# October: the Micron came up as nvme7, the fourth DPRP5 as nvme8);
# nvmeNn1 only while nothing is mounted there.  With nothing mounted there,
# nvmeNn1 is left out if it is absent or in use, as it is after a boot that
# found fewer drives (tardis2 on 2 November: two SN8100s gone, the DPRP5s
# nvme2 to nvme5 mounted at /hs/nvme4 to 7, and the Micron nvme6).
data_drives() {
	local n d
	for n in $DATA_DRIVES; do
		d=$(findmnt -rno SOURCE -M /hs/nvme$n 2>/dev/null | head -1)
		if [ -z "$d" ]; then
			d=nvme${n}n1
			[ -b /dev/$d ] || continue
			lsblk -nro MOUNTPOINT /dev/$d 2>/dev/null | grep -q . && continue
			echo $d
			continue
		fi
		d=${d#/dev/}
		findmnt -rno TARGET -S /dev/$d 2>/dev/null | grep -qvx "/hs/nvme$n" &&
			{ echo "$d is mounted outside /hs/nvme$n: not a data drive" >&2; return 1; }
		lsblk -nro MOUNTPOINT /dev/$d 2>/dev/null | grep -qE '^/(boot.*)?$' &&
			{ echo "$d holds a system filesystem: not a data drive" >&2; return 1; }
		echo $d
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
SSHC="ssh -o BatchMode=yes -o ConnectTimeout=30 -o ServerAliveInterval=10 -o ServerAliveCountMax=3 -i $KEY root@$CLIMGMT"
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
# The client's puppet agent runs every 30 minutes, ending at :18:16 and
# :48:16, and each of its runs stalls a load for about 20 s from about
# :17:51 (FLAMEGRAPH round 38, section 7).  twoport.sh disables the agent
# for the length of a run and starts no load that would span one of its
# runs, since a disable does not stop a run already under way.
# puppet_pause: disable the agent, unless someone else already has (their
# message is left as it is).  puppet_resume: enable it, only if this rig
# disabled it.
PUPPET_LOCK=/var/lib/puppet/state/agent_disabled.lock
PUPPET_MSG="zc rig: a run from $HOST"
puppet_pause() {
	$SSHC "[ -e $PUPPET_LOCK ] || puppet agent --disable '$PUPPET_MSG'" < /dev/null > /dev/null 2>&1
	$SSHC "cat $PUPPET_LOCK 2>/dev/null" < /dev/null
}
puppet_resume() {
	$SSHC "grep -qF '$PUPPET_MSG' $PUPPET_LOCK 2>/dev/null && puppet agent --enable" < /dev/null > /dev/null 2>&1
	return 0
}
# clear_of_puppet <seconds>: wait until the next <seconds> miss the minute
# around a puppet run, :17:40 to :18:25 and :47:40 to :48:25; says when it
# waits.
clear_of_puppet() {
	local need=$1 p d
	while :; do
		p=$(( $(date +%s) % 1800 ))	# :17:40 is 1060 s into a half hour of the epoch, :18:25 1105
		if [ $p -ge 1060 ] && [ $p -lt 1105 ]; then
			d=$(( 1105 - p ))
		else
			d=$(( (1060 - p + 1800) % 1800 ))
			[ $need -le $d ] && return 0
			d=$(( d + 45 ))
		fi
		echo "clear_of_puppet: waiting $d s, to keep the next $need s clear of the client's puppet run"
		sleep $d
	done
}
# timed_load <run dir> [seconds]: for a look beside a twoport.sh run, wait
# for the run's timed load to start, which is when twoport.sh starts its
# drives-sec.tsv (one older than the call is another run's), then the
# seconds given (10 by default, the looks' warm-up).  A WRITE rate is no
# sign of it: the placement job before it writes as fast, and a look that
# waits for the rate starts about 13 s early (FLAMEGRAPH round 38, section
# 8).  Fails after 10 minutes without it.
timed_load() {
	local t0 s; t0=$(date +%s)
	for s in $(seq 600); do
		if [ -e "$1/drives-sec.tsv" ] && [ "$(stat -c %Y "$1/drives-sec.tsv")" -ge $t0 ]; then
			sleep ${2:-10}
			return 0
		fi
		sleep 1
	done
	return 1
}
