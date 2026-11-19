#!/bin/bash
# spe-probe.sh: FLAMEGRAPH round 150, whether tardis2 can record perf
# c2c.  perf c2c on Neoverse N2 needs ARM SPE; arm_spe_pmu is built as a
# module and not loaded.  Loads it and says whether a PMU appeared (the
# firmware must describe SPE's interrupt in the MADT), and what perf c2c
# would record with.  Run through step.sh, between runs.
modprobe arm_spe_pmu; rc=$?
echo "modprobe arm_spe_pmu: exit $rc"
ls /sys/bus/event_source/devices/ | grep -i spe || echo "no SPE PMU"
dmesg | grep -iE "arm_spe|spe_pmu|SPE" | tail -5
perf c2c record -e list 2>&1 | head -5
