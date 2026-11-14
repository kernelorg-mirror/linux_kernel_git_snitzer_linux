#!/bin/bash
# run622.sh: round 102, NFS writes and reads to four drives, nvme0 to
# nvme3, on build 531 with the DRC commits' nfsd.ko: on one port (a
# alone on all four, its one-port setting, as round 98's t2-r525) and on
# two (a on nvme0 and nvme1, b on nvme2 and nvme3, the two-port
# setting).  Loans as settled (WRITES=64, and on two ports BUDGET_PAGES
# 8198).  RUNS is <n>:<ports>:<rw> in turn, default the write and read
# of each, twice, interleaved.  DRIVES=8: all eight drives instead, a
# alone on nvme0 to nvme7, or a on nvme0 to nvme3 and b on nvme4 to
# nvme7 (twoport.sh's own layout).
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
say() { echo "$(date +%T) $*"; }
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump not armed"; exit 1; }
for run in ${RUNS:-622:a:write 623:ab:write 624:a:read 625:ab:read 626:a:write 627:ab:write 628:a:read 629:ab:read}; do
	IFS=: read n ports rw <<< "$run"
	[ "$(df --output=avail -k / | tail -1)" -gt 2097152 ] || { say "root has under 2 GB free"; exit 1; }
	O=$L/t2-r$n; mkdir -p $O
	client_free || { say "client not free"; exit 1; }
	./settled.sh apply > $O/apply.txt 2>&1 || { say "not settled: $(grep -E 'BAD|FAILED' $O/apply.txt | head -3)"; exit 1; }
	ping -c1 -W2 192.168.1.106 > /dev/null && ping -c1 -W2 192.168.0.106 > /dev/null || { say "a port does not answer"; exit 1; }
	sync; for d in 0 1 2 3 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 120
	dmesg > $O/dmesg.0
	case ${DRIVES:-4}$ports in
	4a)  FS="FS_A=/hs/nvme0 /hs/nvme1 /hs/nvme2 /hs/nvme3"; FSB="FS_B=/hs/nvme2 /hs/nvme3"; BP= ;;
	4ab) FS="FS_A=/hs/nvme0 /hs/nvme1"; FSB="FS_B=/hs/nvme2 /hs/nvme3"; BP=8198 ;;
	8a)  FS="FS_A=/hs/nvme0 /hs/nvme1 /hs/nvme2 /hs/nvme3 /hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"; FSB="FS_B=/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"; BP= ;;
	8ab) FS="FS_A=/hs/nvme0 /hs/nvme1 /hs/nvme2 /hs/nvme3"; FSB="FS_B=/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"; BP=8198 ;;
	*)   say "DRIVES=${DRIVES:-4}: 4 or 8"; exit 1 ;;
	esac
	env "$FS" "$FSB" WARM=10 RUNTIME=40 RW=$rw RWTSV=$O/rw.tsv TAG=${DRIVES:-4}d-$ports-$rw \
		WRITES=$TWO_WRITES ${BP:+BUDGET_PAGES=$BP} ./twoport.sh $O/run $ports > $O/run.log 2>&1 ||
		{ say "run failed: $(grep -E 'BAD|FAILED|no answer|not free' $O/run.log | head -3)"; exit 1; }
	dmesg > $O/dmesg.1
	hung=$(diff $O/dmesg.0 $O/dmesg.1 | grep -v 'tracing to debugfs is deprecated' | grep -c -i -E 'hung|blocked for more|soft lockup|rcu.*stall|WARNING|BUG')
	say "t2-r$n $ports $rw | $(grep '^total' $O/run.log) | ring drops $(grep -oE 'ring drops [0-9]+' $O/run.log | awk '{s+=$3} END {print s}') | $(grep -o 'no drive stepped down' $O/run.log || echo 'a drive stepped down') | new kernel warnings $hung"
	grep -E '^  (read|write):|^port ' $O/run.log | sed 's/^/    /'
	grep -E '^ *drives ' $O/run.log | sed 's/^/    /'
	[ "$hung" = 0 ] || { say "the kernel warned; stopping"; diff $O/dmesg.0 $O/dmesg.1 | head -20; exit 1; }
done
./settled.sh apply > $L/t2-r$n/apply-after.txt 2>&1; say "after: $(tail -1 $L/t2-r$n/apply-after.txt)"
