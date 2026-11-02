#!/bin/bash
# run141.sh: round 54, the expkey change on the rig before any A/B: KUnit, two
# mutants of the peek, the lookups' paths under load with a flush and a
# short-lived entry in the window, and an unexport seen by a live mount.
S=/tmp/claude-0/-home-serviceadmin-snitm-kernel-linux/d422d5e4-dcd9-4151-84e7-ab3e472a1d51/scratchpad
cd /home/serviceadmin/snitm/kernel/linux/NFSD_TCP_WRITE_ZEROCOPY/xeu-rig || exit 1
. ./common.sh
O=$L/t2-r141; mkdir -p $O
R=../FLAMEGRAPH/54-fh-verify
K=$(uname -r); KB=/lib/modules/$K/build; UPD=/lib/modules/$K/updates
FS="/hs/nvme4 /hs/nvme5 /hs/nvme6 /hs/nvme7"
say() { echo "$(date +%T) $*"; }
[ "$(cat /sys/kernel/kexec_crash_loaded)" = 1 ] || { say "kdump is not armed"; exit 1; }
[ "$(ss -Htn state established '( sport = :2049 )' | wc -l)" = 0 ] || { say "a client is connected"; exit 1; }
swap() { SUNRPC_KO=$1 NFSD_KO=${2:-} ./swap-sunrpc.sh > $O/swap.txt 2>&1 || { say "swap failed: $(tail -2 $O/swap.txt)"; exit 1; }; }
cp $S/all4/svcsock_kunit/svcsock_kunit.ko $UPD/; depmod -a
swap $S/sunE/sunrpc/sunrpc.ko $S/nfsE/nfsd/nfsd.ko
say "on sunrpc $(cat /sys/module/sunrpc/srcversion) nfsd $(cat /sys/module/nfsd/srcversion)"
one() {	# NAME
	systemctl stop nfs-server; sync; dmesg -C
	lsmod | grep -q '^kunit ' || modprobe kunit enable=1
	timeout 170 insmod $S/all4/svcsock_kunit/svcsock_kunit.ko; local rc=$?; sleep 1
	dmesg > $O/$1.dmesg; rmmod svcsock_kunit; systemctl start nfs-server
	say "$1 rc=$rc: $(grep -oE 'sunrpc-svcsock-rx: pass:[0-9]+ fail:[0-9]+ skip:[0-9]+' $O/$1.dmesg | tail -1); not ok: $(grep -E ' not ok ' $O/$1.dmesg | grep -v svcsock-rx | awk '{print $NF}' | sort -u | paste -sd' '); warnings $(grep -cE 'WARNING:|BUG:|Oops' $O/$1.dmesg)"
}
one B
W=$S/mut54; ln -s /sys/kernel/btf/vmlinux $KB/vmlinux 2>/dev/null
for m in M1 M2; do
	case $m in
	M1) r='s/if (!h || cache_is_valid(h) || cache_needs_refresh(h))/if (!h || cache_needs_refresh(h))/';;
	M2) r='s/if (!h || cache_is_valid(h) || cache_needs_refresh(h))/if (!h || cache_is_valid(h))/';;
	esac
	sed -e "$r" $S/sunE/sunrpc/cache.c > $W/sunrpc/cache.c
	cmp -s $S/sunE/sunrpc/cache.c $W/sunrpc/cache.c && { say "$m unchanged"; exit 1; }
	make -C $KB M=$W/sunrpc NOSTDINC_FLAGS="-nostdinc -I$W/inc" -j32 modules > $W/$m.log 2>&1 || { say "$m build failed"; exit 1; }
	swap $W/sunrpc/sunrpc.ko
	one $m
done
rm -f $KB/vmlinux
swap $S/sunE/sunrpc/sunrpc.ko
one B2
systemctl start nfs-server; sleep 2
./settled.sh apply > $O/apply.txt 2>&1 || { grep -E 'BAD|FAILED' $O/apply.txt | head; exit 1; }
client_free || exit 1
for d in 4 5 6 7; do fstrim /hs/nvme$d; done; sleep 60
bpftrace $R/paths.bt > $O/paths.txt 2>&1 &
BT=$!
until grep -q Attached $O/paths.txt; do sleep 0.5; done
( timed_load $O/t 8
  say "flush" | tee -a $O/events.txt
  exportfs -f
  sleep 8
  grep -v '^#' /proc/net/rpc/nfsd.fh/content > $O/fh-content.txt
  exp=$(( $(date +%s) + 14 ))
  say "short-lived keys, expiry +14 s: $(wc -l < $O/fh-content.txt)" | tee -a $O/events.txt
  while read -r dom t fsid path; do
	echo "$dom $t $fsid $exp $path" > /proc/net/rpc/nfsd.fh/channel
  done < $O/fh-content.txt ) &
env FS_A="$FS" FS_B= WARM=10 RUNTIME=30 TAG=t WRITES=64 ./twoport.sh $O/t a > $O/t.log 2>&1; rc=$?
wait %2 2>/dev/null; kill -INT $BT; wait $BT 2>/dev/null
say "load rc=$rc: $(grep '^total' $O/t.log | head -1); fio errors $(grep -oE '"total_err" *: *[0-9]+|"error" *: *[1-9][0-9]*' $O/t/fio-a.json | grep -cv ': 0')"
# An unexported file system, on a live mount.
C=${CLIP[a]} SV=${SRVP[a]} E=/hs/nvme4/zcbudget M=/mnt/rxr-t141
OPT=rw,sync,no_root_squash,insecure,no_subtree_check,fsid=7141
dd1() { $SSHC "timeout 25 dd if=/dev/zero of=$M/u bs=1M count=1 oflag=direct 2>&1 | tail -1; echo rc=\${PIPESTATUS[0]}"; }
systemctl start nfs-server
exportfs -o $OPT "$C:$E" || exit 1
$SSHC "mkdir -p $M; mount -t nfs -o vers=3,proto=tcp,soft,timeo=20,retrans=1 $SV:$E $M && findmnt -no FSTYPE,SOURCE $M"
say "exported: $(dd1 | paste -sd' ')"
exportfs -u "$C:$E"; exportfs -f
say "unexported: $(dd1 | paste -sd' ')"
exportfs -o $OPT "$C:$E"
say "exported again: $(dd1 | paste -sd' ')"
$SSHC "rm -f $M/u; umount $M; rmdir $M"; exportfs -u "$C:$E"
./settled.sh apply > $O/apply-end.txt 2>&1 || grep -E 'BAD|FAILED' $O/apply-end.txt | head
say "end: $(./settled.sh audit rest 2>&1 | tail -1); warnings $(dmesg | grep -cE 'WARNING:|BUG:|Oops|rcu.*stall')"
