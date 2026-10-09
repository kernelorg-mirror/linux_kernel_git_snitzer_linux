#!/bin/bash
# sweep.sh <outdir> <plan file> - run a plan of step cells over the xeu port
# and append one row per cell to <outdir>/summary.tsv.
#
# A plan line is:   <ring entries> <held buffer bytes|-> <writes> <prp|sgl> <cells...>
#   writes  how many 1 MiB WRITEs the budget admits at once (FORCE_BUDGET is
#           worked out from it); 0 for a line of copy cells.
#   -       the harness's own held buffer (256 KiB one connection, 128 KiB sixteen).
# Lines starting with # are skipped.  A line already in <outdir>/done is
# skipped too, so a sweep that was stopped can be started again.
#
# Sequential by construction: nothing here waits on another process by
# name.  (pgrep -f and pkill -f match the shell that runs them; that hung a
# night's queue on 2026-10-07.)
#
# Per cell it records, over 90 s starting 15 s into the cell: the server's
# kernel CPU time, the client's busy CPU time, and the ring tail drops.
# PERF=1 adds cycles and instructions; PROFILE=1 adds a 30 s flat profile by
# function (it sits inside the CPU window and costs a little).
. "$(dirname "$0")/common.sh"
# PRP_DIR and SGL_BACKING say where the two storages are: the export
# directory of the "prp" lines and the file behind the NVMe/TCP target of
# the "sgl" lines.  The defaults are the single drives; a stripe is
#     PRP_DIR=/hs/stripe/zcbudget SGL_BACKING=/hs/stripe/zcbudget-nvmet.img
PRP_DIR=${PRP_DIR:-/hs/nvme0/zcbudget}
SGL_BACKING=${SGL_BACKING:-/hs/nvme1/zcbudget-nvmet.img}
OUT=${1:?outdir}; PLAN=$(readlink -f "${2:?plan file}")	# cd below: a relative plan would be lost
mkdir -p $OUT; touch $OUT/done
[ -e $RG ] || { echo "no region: run bringup.sh first"; exit 1; }
cd $H || exit 1

# --- geometry: what the harness and the kernel charge for a 1 MiB WRITE ---
G=$(GEOMETRY=1 NETDEV=$D CLIENT=root@$CLIIP ./run-budget.sh /dev/null 2>/dev/null | grep '^geometry')
F9=$(sed -n 's/.*factor \([0-9]*\) (MTU 9000).*/\1/p' <<< "$G")
RESERVE=$(sed -n 's/.*reserve \([0-9]*\) \/.*/\1/p' <<< "$G")
[ -n "$F9" ] && [ -n "$RESERVE" ] || { echo "cannot read the geometry: $G"; exit 1; }
# The kernel charges the payload pages times the factor; 2 over for rounding.
CHARGE=$(( ( (1048716 + PS - 1) / PS * F9 + 99 ) / 100 + 2 ))
echo "$G" > $OUT/geometry.txt
echo "storage: prp $PRP_DIR, sgl $SGL_BACKING; nfsd threads $(cat /proc/fs/nfsd/threads); STEP_JOBS=${STEP_JOBS:-} STEP_BS=${STEP_BS:-} EXPORT_SYNC=${EXPORT_SYNC:-} MODE=${MODE:-}" | tee -a $OUT/geometry.txt
echo "page $PS factor $F9 reserve $RESERVE charge-per-1MiB-WRITE $CHARGE region $(reg pages) retry-patch $(has_retry_patch && echo yes || echo no)" | tee -a $OUT/geometry.txt

stat1() { head -1 /proc/stat; }
cstat1() { $SSHC 'head -1 /proc/stat'; }
# cpu_delta "<stat a>" "<stat b>": user sys softirq busy, in CPU-seconds
cpu_delta() { echo "$1 $2" | awk '{u=($13+$14-$2-$3)/100; s=($15-$4)/100; q=($18+$19-$7-$8)/100; printf "%.2f %.2f %.2f %.2f", u, s, q, u+s+q}'; }

if [ ! -s $OUT/idle.txt ]; then
	a=$(stat1); ca=$(cstat1); sleep 30; b=$(stat1); cb=$(cstat1)
	echo "server30 $(cpu_delta "$a" "$b")" > $OUT/idle.txt
	echo "client30 $(cpu_delta "$ca" "$cb")" >> $OUT/idle.txt
	cat $OUT/idle.txt
fi

HDR="tag entries rcvbuf writes budget storage cell conns MBps used_pages admitted denied paused overrun retrans loss_probes rto zw_drop rcvq_drop win_tail_drops run_alloc_fail srv_sys srv_softirq srv_user cli_busy cycles instructions wr_direct wr_dontcache wr_buffered fio_rc"
[ -s $OUT/summary.tsv ] || echo "$HDR" > $OUT/summary.tsv

n=0
while read -r entries rb writes st cells <&3; do
	case "$entries" in ''|'#'*) continue ;; esac
	line="$entries $rb $writes $st $cells"
	grep -qxF "$line" $OUT/done && continue
	n=$(( n + 1 ))
	set_ring $entries || { echo "ring change to $entries failed"; exit 4; }
	budget=0; [ "$writes" -gt 0 ] && budget=$(( writes * CHARGE + RESERVE + 4 ))
	rbp=$(( 131072 / PS + 1 )); [ "$rb" != - ] && rbp=$(( rb / PS + 1 ))
	free=$(reg free); need=$(( budget + 16 * ${STEP_MOUNTS:-1} * rbp + 64 ))
	tag="r${entries}-b${rb/-/def}-w${writes}-$st"; i=1
	while [ -e $OUT/$tag.$i ]; do i=$(( i + 1 )); done
	tag=$tag.$i; o=$OUT/$tag
	if [ "$free" -lt "$need" ]; then
		echo "-- $tag SKIPPED: $free region pages free at idle, need $need" | tee -a $OUT/log
		continue
	fi
	if [ $st = prp ]; then a="STORAGE=prp EXPORT_DIR=$PRP_DIR"; else a="STORAGE=sgl BACKING=$SGL_BACKING"; fi
	[ "$rb" != - ] && a="$a FORCE_RCVBUF=$rb"
	[ "$writes" -gt 0 ] && a="$a FORCE_BUDGET=$budget"
	af0=$(reg alloc_fail)
	env $a NETDEV=$D CLIENT=root@$CLIIP CELLS="$cells" ./run-budget.sh $o > $o.console 2>&1 &
	HP=$!
	: > $o.windows
	for c in $cells; do
		until grep -q " $c fio start" $o/stress-A/delay-steps.txt 2>/dev/null; do
			kill -0 $HP 2>/dev/null || break
			sleep 1
		done
		kill -0 $HP 2>/dev/null || break
		sleep 15
		s0=$(stat1); c0=$(cstat1); t0=$(tail_drops)
		if [ -n "${PERF:-}" ]; then
			perf stat -a -x, -e cycles,instructions -o $o.$c.perf -- sleep 90 2>/dev/null
		elif [ -n "${PROFILE:-}" ]; then
			sleep 20
			perf record -a -F 99 -o $o.$c.data -- sleep 30 > /dev/null 2>&1
			perf report -i $o.$c.data --stdio --sort comm,symbol 2>/dev/null | grep -v '^$' | head -70 > $o.$c.profile.txt
			rm -f $o.$c.data
			sleep 40
		else
			sleep 90
		fi
		s1=$(stat1); c1=$(cstat1); t1=$(tail_drops)
		cy=-; ins=-
		if [ -s $o.$c.perf ]; then
			cy=$(awk -F, '$3 == "cycles" {print $1}' $o.$c.perf); ins=$(awk -F, '$3 == "instructions" {print $1}' $o.$c.perf)
		fi
		echo "$c $(cpu_delta "$s0" "$s1") $(cpu_delta "$c0" "$c1") $(( t1 - t0 )) ${cy:--} ${ins:--}" >> $o.windows
	done
	wait $HP; rc=$?
	daf=$(( $(reg alloc_fail) - af0 ))
	# one row per cell: the harness's columns and the window's
	awk -v tag=$tag -v e=$entries -v rb=$rb -v w=$writes -v b=$budget -v st=$st -v daf=$daf -v wf=$o.windows '
		BEGIN { while ((getline l < wf) > 0) { split(l, x, " "); W[x[1]] = l } }
		$1 != "cell" && $1 != "warmup" {
			split(W[$1], x, " ")
			# x: cell u s q busy | cu cs cq cbusy | drops cycles instr
			print tag, e, rb, w, b, st, $1, $4, $11, $16, $25, $26, $27, $28, $21, $22, $23, $19, $18, \
			      (x[10] == "" ? "-" : x[10]), daf, x[3]+0, x[4]+0, x[2]+0, x[9]+0, \
			      (x[11] == "" ? "-" : x[11]), (x[12] == "" ? "-" : x[12]), $29, $30, $31, $12
		}' $o/stress-A/cells.tsv >> $OUT/summary.tsv
	echo "-- $tag $(date +%T) rc=$rc budget=$budget free $free -> $(reg free) min_free $(reg min_free) alloc_fail +$daf" | tee -a $OUT/log
	tail -n $(echo $cells | wc -w) $OUT/summary.tsv | awk '{printf "   %-14s conns=%-2s MB/s=%-5s used=%-5s admitted=%-7s denied=%-7s paused=%s rto=%s zw=%s tail_drops=%s\n", $7, $8, $9, $10, $11, $12, $13, $17, $18, $20}' | tee -a $OUT/log
	if [ $daf -ne 0 ]; then
		echo "   the region ran dry in this run" | tee -a $OUT/log
		if ! has_retry_patch; then echo "   no retry patch: bouncing the port so no ring stays empty" | tee -a $OUT/log; bounce; fi
	fi
	if [ $rc -ne 0 ]; then
		echo "   harness failed (rc $rc): $(tail -2 $o.console | head -1)" | tee -a $OUT/log
		bounce
		continue
	fi
	echo "$line" >> $OUT/done
done 3< $PLAN
echo "== sweep over: $n lines run; rings 4 x $(rx_entries); $(grep -E '^free|alloc_fail' $RG | tr '\n' ' ')" | tee -a $OUT/log
echo SWEEPDONE
