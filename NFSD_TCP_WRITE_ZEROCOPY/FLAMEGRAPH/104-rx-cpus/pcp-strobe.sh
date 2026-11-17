#!/bin/bash
# pcp-strobe.sh <n> <fraction>: txdrain.sh <n> N (the READ strobe with
# skbshape.bt, BQL samples and the mesh's memory traffic) with
# vm.percpu_pagelist_high_fraction at <fraction> (larger per-CPU page
# lists), put back to 0 after, whatever happens; the pagesets of a work
# item's CPU (8) and a ring CPU (1) are saved either side.  settled.sh does
# not hold it.  Round 104, 17 November (Mike: "run the
# percpu_pagelist_high_fraction=8 strobe").
n=${1:?pcp-strobe.sh <n> <fraction>} f=${2:?}
H=$(cd "$(dirname "$0")" && pwd); O=/var/tmp/zc-run-logs/t2-r$n; mkdir -p $O
pcp() { awk '/zone *Normal/{z=1} z && /cpu: (1|8)$/{c=$2; getline; a=$2; getline; h=$2; getline; printf "cpu %s count %s high %s batch %s\n", c, a, h, $2}' /proc/zoneinfo; }
trap 'sysctl -qw vm.percpu_pagelist_high_fraction=0; echo "back: $(sysctl vm.percpu_pagelist_high_fraction)"; pcp' EXIT
sysctl -qw vm.percpu_pagelist_high_fraction=$f || exit 1
echo "$(sysctl vm.percpu_pagelist_high_fraction)"; pcp | tee $O/pcp.0
PROBE=skbshape $H/txdrain.sh $n N; rc=$?
pcp | tee $O/pcp.1
exit $rc
