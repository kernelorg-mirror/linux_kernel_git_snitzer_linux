#!/bin/bash
# wirebuf.sh [streams] [rcvbuf bytes...] - what the wire carries to a
# receiver that discards, with the server's receive buffer held at each
# size (0: left to grow): no NFS and no storage, the rings as they are.
# It is the ceiling of a load whose connections hold that buffer.
. "$(dirname "$0")/common.sh"
N=${1:-16}; shift
OUT=${OUT:-$L/xeu-$(( PS / 1024 ))k-wirebuf.tsv}
trap '$SSHC "pkill -f zc-snd.py; rm -f /var/tmp/zc-snd.py" 2>/dev/null; pkill -f "$RIG/rcv.py" 2>/dev/null' EXIT
client_free || exit 1
scp -q -o BatchMode=yes -i $KEY $RIG/snd.py root@$CLIMGMT:/var/tmp/zc-snd.py
[ -s $OUT ] || echo "rings entries streams rcvbuf MBps tail_drops peak_pages alloc_fail retrans rwnd_limited_pct" > $OUT
for rb in "${@:-262144 524288 1048576 2097152 4194304 0}"; do for rb in $rb; do
	af0=$(reg alloc_fail); td0=$(tail_drops); echo > $RG
	python3 $RIG/rcv.py $rb $N 20 $SRVIP > /tmp/.rcv.out 2>&1 & R=$!
	sleep 1
	rt=$(timeout 90 $SSHC "nstat -n >/dev/null 2>&1; (sleep 12; ss -tin dst $SRVIP dport = :5202 | grep -o 'rwnd_limited:[0-9]*ms([0-9.]*%)' | sed 's/.*(\(.*\)%)/\1/' | sort -n | awk '{a[NR]=\$1} END{print a[int((NR+1)/2)]+0}' > /tmp/.zc-rw) & timeout 60 python3 /var/tmp/zc-snd.py $N 20 $SRVIP $CLIIP 2>/dev/null; wait; echo \"\$(nstat TcpRetransSegs 2>/dev/null | awk 'NR>1{print \$2}') \$(cat /tmp/.zc-rw)\"")
	sleep 2; kill $R 2>/dev/null; wait $R 2>/dev/null
	mb=$(sed -n 's/.*: \([0-9]*\) MB\/s.*/\1/p' /tmp/.rcv.out)
	echo "$(rx_rings) $(rx_entries) $N $rb ${mb:-0} $(( $(tail_drops) - td0 )) $(( $(reg pages) - $(reg min_free) )) $(( $(reg alloc_fail) - af0 )) ${rt:-? ?}" | tee -a $OUT
done; done
column -t $OUT
