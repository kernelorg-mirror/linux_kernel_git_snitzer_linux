#!/bin/bash
# ab607.sh: round 100, the DRC commits' A/B with both nfsd.ko built the
# same way (bnfsd.sh, out of tree against build 531's headers): base is
# 8efa530e9ab01's fs/nfsd, drc the tip's.  Base first (t2-r607 counted,
# r608 call graph, r609 counted), then the DRC commits again (r610
# counted, r611 call graph, r612 counted), each set with run601.sh.
# Leaves the DRC nfsd.ko loaded.
cd "$(dirname "$0")" || exit 1
R=$PWD; X=$R/../../xeu-rig
. $X/common.sh
say() { echo "$(date +%T) $*"; }
swap() {
	(cd $X && SUNRPC_KO=none NFSD_KO=$1 ./swap-sunrpc.sh) > $L/swap-$2.log 2>&1 ||
		{ say "swap to $2 failed: $(tail -3 $L/swap-$2.log)"; exit 1; }
	say "nfsd $2: $(cat /sys/module/nfsd/srcversion)"
}
swap $L/nfsd-base-531/nfsd.ko base
[ "$(cat /sys/module/nfsd/srcversion)" = EF174CABCDAC245CD82D155 ] || { say "not the base nfsd"; exit 1; }
RUNS="607:C 608:G 609:C" $R/run601.sh || exit 1
swap $L/nfsd-drc-531/nfsd.ko drc
[ "$(cat /sys/module/nfsd/srcversion)" = 603083CAF5F5B5E8DE898AC ] || { say "not the DRC nfsd"; exit 1; }
RUNS="610:C 611:G 612:C" $R/run601.sh
