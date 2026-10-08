#!/bin/bash
# ringsize.sh [entries...] - a plain network load from the client at each
# ring size, one and sixteen streams, with the receive buffer left to grow
# and then held as the cells hold it.  Traffic goes to tardis1 itself, not to
# a mount.  Reads the ring drop counter before the next ring change zeroes it.
. "$(dirname "$0")/common.sh"
OUT=${OUT:-$L/xeu-$(( PS / 1024 ))k-ringsize.tsv}
trap '$SSHC "pkill -f zc-snd.py; rm -f /var/tmp/zc-snd.py" 2>/dev/null; pkill -f "$RIG/rcv.py" 2>/dev/null' EXIT
scp -q -o BatchMode=yes -i $KEY $RIG/snd.py root@$CLIIP:/var/tmp/zc-snd.py
[ -s $OUT ] || echo "entries streams rcvbuf MBps tail_drops peak_pages alloc_fail retrans" > $OUT
for e in "${@:-64 128 256 512}"; do for e in $e; do
	set_ring $e || exit 1
	for cfg in "1 0" "16 0" "1 262144" "16 131072"; do set -- $cfg
		af0=$(reg alloc_fail); td0=$(tail_drops); echo > $RG
		python3 $RIG/rcv.py $2 $1 20 > /tmp/.rcv.out 2>&1 & R=$!
		sleep 1
		rt=$(timeout 90 $SSHC "nstat -n >/dev/null 2>&1; timeout 60 python3 /var/tmp/zc-snd.py $1 20 2>/dev/null; nstat -a TcpRetransSegs 2>/dev/null | awk 'NR>1{print \$2}'")
		sleep 2; kill $R 2>/dev/null; wait $R 2>/dev/null
		mb=$(sed -n 's/.*: \([0-9]*\) MB\/s.*/\1/p' /tmp/.rcv.out)
		echo "$e $1 $2 ${mb:-0} $(( $(tail_drops) - td0 )) $(( $(reg pages) - $(reg min_free) )) $(( $(reg alloc_fail) - af0 )) ${rt:-?}" | tee -a $OUT
		if [ "$(reg alloc_fail)" != "$af0" ] && ! has_retry_patch; then echo "  region ran dry: bouncing the port (no retry patch)"; bounce; fi
	done
done; done
column -t $OUT
