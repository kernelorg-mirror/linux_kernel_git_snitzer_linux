#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# run-testplan.sh [--list] [--only STAGE] [--redo STAGE] [--force] [--results DIR]
#
# Run the page-loan series' whole test plan on this host's running kernel,
# stage by stage, resumably across the reboots it needs.  Every stage's exit
# status, output directory and log go into the results directory (default
# /var/tmp/zc-testplan-<release>), whose status.tsv records what is done; a
# re-run skips finished stages.  Two stages need a fresh boot (no receive
# region created yet): budget-D, which leads boot 1, and budget-B, which is
# left for boot 2 -- the runner stops and says "reboot" when it reaches one
# on a boot that is no longer fresh.
#
#   boot 1: preflight budget-D kunit bvecrepro rig-cmp loan-assert killswitch
#           system-correctness zc-down sgl-up sgl-brd sgl-tcp sgl-loop sgl-down
#           localio-tcp localio-loop budget-A budget-TLP budget-X budget-N
#           budget-A-sgl budget-X-sgl
#   boot 2: preflight budget-B budget-B-sgl analyze-AB analyze-AB-sgl summary
#
# The budget phases run on storage that needs whole pages (nvme-loop, standing
# in for PRP-only NVMe).  The -sgl stages repeat phases A, X and B on storage
# that takes arbitrary pieces (NVMe/TCP, standing in for NVMe with SGLs), into
# their own directory (budget-sgl), so every page size has both results.
# budget-B-sgl reuses the region budget-B created on its boot.
#
# A stage that needs something this host lacks (bvecrepro needs a built tree
# matching the running kernel) is recorded as SKIP with the reason.  Results
# are judged per TESTING.md and slipstream/README.md: scripts with their own
# pass/fail (KUnit, loan-assert, killswitch, system-correctness, rig-cmp,
# analyze-AB) are PASS/FAIL here; the budget phases' tables are DONE and
# compared by hand against the step-4 / 4 KiB results (see transplant/README.md
# for what changes with page size).
#
#   --list          show stages and their status, run nothing
#   --only STAGE    run just STAGE (and record it)
#   --redo STAGE    forget STAGE's result, then continue the plan
#   --force         run on past a preflight failure
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
P=$(cd "$HERE/.." && pwd)
TOP=$(cd "$P/.." && pwd)
KREL=$(uname -r)
PS=$(getconf PAGESIZE)
RES=/var/tmp/zc-testplan-$KREL
LIST=0; ONLY=""; REDO=""; FORCE=0
while [ $# -gt 0 ]; do
	case "$1" in
	--list) LIST=1; shift ;;
	--only) ONLY=$2; shift 2 ;;
	--redo) REDO=$2; shift 2 ;;
	--force) FORCE=1; shift ;;
	--results) RES=$2; shift 2 ;;
	*) sed -n '3,33p' "$0"; exit 2 ;;
	esac
done
[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 2; }
mkdir -p "$RES"
ST=$RES/status.tsv
[ -e "$ST" ] || printf 'stage\tresult\trc\tboot\twhen\tlog\n' > "$ST"
BOOT=$(cat /proc/sys/kernel/random/boot_id)
RG=/sys/kernel/debug/page_pool_rx_region
CR=$RES/budget          # all run-budget.sh phases share one OUTDIR (analyze.py wants stress-A and stress-B together)
CRS=$RES/budget-sgl     # the same, for the phases repeated on NVMe/TCP

STAGES="preflight budget-D kunit bvecrepro rig-cmp loan-assert killswitch system-correctness zc-down sgl-up sgl-brd sgl-tcp sgl-loop sgl-down localio-tcp localio-loop budget-A budget-TLP budget-X budget-N budget-A-sgl budget-X-sgl budget-B budget-B-sgl analyze-AB analyze-AB-sgl summary"
FRESH="budget-D budget-B"

result_of() { awk -F'\t' -v s="$1" '$1 == s {r = $2} END {print r}' "$ST"; }
record() { printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$BOOT" "$(date +%F_%T)" "$4" >> "$ST"; }
done_ok() { case "$(result_of "$1")" in PASS|DONE|SKIP) return 0 ;; esac; return 1; }
fresh_boot() { [ ! -e $RG ]; }
ran_this_boot() { awk -F'\t' -v b="$BOOT" '$4 == b && $1 != "preflight"' "$ST" | grep -q .; }

if [ $LIST = 1 ]; then
	echo "kernel $KREL, page size $PS, results $RES, boot $(fresh_boot && echo fresh || echo 'not fresh')"
	for s in $STAGES; do printf '  %-20s %s\n' "$s" "$(result_of $s)"; done
	exit 0
fi
if [ -n "$REDO" ]; then
	awk -F'\t' -v s="$REDO" '$1 != s' "$ST" > "$ST.new" && mv "$ST.new" "$ST"
fi

# --- stages: each echoes to its log and returns 0 PASS/DONE, 1 FAIL, 3 SKIP
need_nfs_server() { systemctl is-active -q nfs-server || systemctl start nfs-server; }
st_preflight()  { "$HERE/preflight.sh"; }
st_kunit()      { systemctl stop nfs-server; "$P/run-kunit.sh" "$1/kunit.log"; local rc=$?; need_nfs_server; return $rc; }
st_bvecrepro() {
	if [ ! -r "$TOP/Module.symvers" ] || [ "$(make -s -C "$TOP" kernelrelease 2>/dev/null)" != "$KREL" ]; then
		echo "no built tree for $KREL at $TOP (packaged kernel): out-of-tree module cannot be built"; return 3
	fi
	B=$1/bvecrepro-build bash "$P/run-bvecrepro.sh"
}
st_rig_cmp()    { need_nfs_server; bash "$P/run-rig-cmp.sh"; }
st_loan_assert(){ need_nfs_server; bash "$P/run-loan-assert.sh"; }
st_killswitch() { need_nfs_server; bash "$P/run-killswitch.sh"; }
st_syscorr()    { need_nfs_server; bash "$P/run-system-correctness.sh"; }
st_zc_down()    { bash "$HERE/zc-rig-down.sh"; }
st_sgl_up()     { lsmod | grep -q '^brd ' && modprobe -r brd; bash "$P/run-sgl-rigs.sh" up; }
st_sgl()        { need_nfs_server; bash "$P/run-sgl-nfsd-cmp.sh" "$2" 10; }
st_sgl_down()   { bash "$P/run-sgl-rigs.sh" down; modprobe -r brd 2>/dev/null; true; }
st_localio()    { need_nfs_server; bash "$P/run-sgl-localio-ab.sh" "$2" "$1/localio-$2"; }
# A budget phase's tables are compared by hand, but two things are never a
# matter of reading: fio failing its data verification, and a cell that had
# to be killed by its timeout (rc 124: a hang).  Either fails the stage.
# (2026-10-04, 16 KiB: both happened in stages recorded DONE.)
budget_verdict() {
	local d=$2/stress-$1 bad=0 n f
	n=$(cat "$d"/*/fio.out "$d"/*/*/fio.out 2>/dev/null | grep -c 'verify:')
	if [ "$n" -gt 0 ]; then
		echo "!! budget-$1: $n fio verify failures:"
		grep -l 'verify:' "$d"/*/fio.out "$d"/*/*/fio.out 2>/dev/null | sed "s|^$d/|     |"
		bad=1
	fi
	for f in "$d"/cells.tsv "$d"/xcells.tsv "$d"/ncells.tsv; do
		[ -r "$f" ] || continue
		awk 'NR == 1 { for (i = 1; i <= NF; i++) if ($i ~ /(^|_)rc$/) c[i] = $i; next }
		     { for (i in c) if ($i == 124) { print "!! " $1 ": " c[i] " 124 (killed by its timeout: hung)"; r = 1 } }
		     END { exit r }' "$f" || bad=1
	done
	return $bad
}
st_budget() {
	local ph=$2 rc cr=$CR st=prp
	case $ph in *-sgl) ph=${ph%-sgl}; cr=$CRS; st=sgl ;; esac
	need_nfs_server
	# Phases A, TLP, X and D each re-measure the parked pages into
	# parked.txt; phase B is sized from phase A's, kept as parked-A.txt.
	# (The region is the PRP run's: both storages see the same one.)
	if [ "$ph" = B ]; then
		[ -r "$CR/parked-A.txt" ] || { echo "budget-B needs budget-A's $CR/parked-A.txt"; return 1; }
		STORAGE=$st PARKED=$(cat "$CR/parked-A.txt") PHASE=B bash "$P/run-budget.sh" "$cr"; rc=$?
	else
		STORAGE=$st PHASE=$ph bash "$P/run-budget.sh" "$cr" || return
		[ "$ph" = A ] && cp "$cr/parked.txt" "$cr/parked-A.txt"
		rc=0
	fi
	budget_verdict "$ph" "$cr" || return 1
	return $rc
}
st_analyze() {
	# analyze.py passes when it finds nothing to judge: require both tables.
	local ph
	for ph in A B; do
		[ -r "$1/stress-$ph/cells.tsv" ] || { echo "no $1/stress-$ph/cells.tsv: phase $ph has not run"; return 1; }
	done
	python3 "$P/slipstream/tools/analyze.py" "$1"
}
st_summary() {
	echo "kernel $KREL, page size $PS, $(sed -n 's/^describe=//p' /var/lib/zc-transplant/manifest 2>/dev/null)"
	column -t -s $'\t' "$ST"
	echo "== history layout (transplant/check-grouping.sh)"
	"$HERE/check-grouping.sh"
	case $? in
	1) echo "!! commits made on this host are not grouped code/KUnit/docs: regroup before handing back (transplant/README.md)" ;;
	2) echo "(no upstream branch: run transplant/check-grouping.sh <last pushed commit> by hand)" ;;
	esac
	for f in "$CR"/geometry.txt "$CR"/stress-*/rig.txt "$CR"/stress-*/cells.tsv "$CR"/stress-*/xcells.tsv "$CR"/stress-*/ncells.tsv \
		 "$CRS"/stress-*/rig.txt "$CRS"/stress-*/cells.tsv "$CRS"/stress-*/xcells.tsv; do
		[ -r "$f" ] && { echo "== $f"; column -t "$f"; }
	done
	true
}

run_stage() {
	local s=$1 d=$RES/$1 rc
	mkdir -p "$d"
	echo "=== $s ($(date +%T)), log $d/stage.log"
	case $s in
	preflight)          st_preflight "$d" ;;
	kunit)              st_kunit "$d" ;;
	bvecrepro)          st_bvecrepro "$d" ;;
	rig-cmp)            st_rig_cmp "$d" ;;
	loan-assert)        st_loan_assert "$d" ;;
	killswitch)         st_killswitch "$d" ;;
	system-correctness) st_syscorr "$d" ;;
	zc-down)            st_zc_down "$d" ;;
	sgl-up)             st_sgl_up "$d" ;;
	sgl-brd|sgl-tcp|sgl-loop) st_sgl "$d" "${s#sgl-}" ;;
	sgl-down)           st_sgl_down "$d" ;;
	localio-tcp|localio-loop) st_localio "$d" "${s#localio-}" ;;
	budget-*)           st_budget "$d" "${s#budget-}" ;;
	analyze-AB)         st_analyze "$CR" ;;
	analyze-AB-sgl)     st_analyze "$CRS" ;;
	summary)            st_summary "$d" ;;
	*) echo "unknown stage $s"; return 1 ;;
	esac > >(tee "$d/stage.log") 2>&1
	rc=$?
	sleep 1
	local res
	case $rc in
	0) case $s in budget-*|summary|zc-down|sgl-*|localio-*) res=DONE ;; *) res=PASS ;; esac ;;
	3) res=SKIP ;;
	*) res=FAIL ;;
	esac
	record "$s" "$res" "$rc" "$d/stage.log"
	echo "=== $s: $res (rc $rc)"
	[ "$res" != FAIL ]
}

if [ -n "$ONLY" ]; then run_stage "$ONLY"; exit $?; fi

echo "== test plan on $KREL (page size $PS); results $RES"
run_stage preflight || [ $FORCE = 1 ] || { echo "preflight failed: fix it, or --force"; exit 1; }
for s in $STAGES; do
	[ "$s" = preflight ] && continue
	done_ok "$s" && continue
	case " $FRESH " in *" $s "*)
		if ! fresh_boot || ran_this_boot; then
			echo "== $s needs a fresh boot (no receive region yet, nothing run first): reboot, then run $0 again"
			exit 3
		fi ;;
	esac
	# budget-B goes on its own boot: stop before anything but it runs on this one
	if [ "$s" = budget-B ] && ! done_ok budget-B && ran_this_boot; then
		echo "== budget-B next: reboot, then run $0 again"; exit 3
	fi
	run_stage "$s" || echo "(continuing past $s's failure; see its log)"
done
echo "== test plan complete: $ST"
column -t -s $'\t' "$ST"
