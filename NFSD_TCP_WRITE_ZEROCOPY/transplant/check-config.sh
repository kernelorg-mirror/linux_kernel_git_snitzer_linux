#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# check-config.sh CONFIG [FRAGMENT]
#
# Check a finished kernel config against the options the page-loan series
# and its harness need (zerocopy.fragment beside this script by default).
# CONFIG is a build's .config, a redhat/configs flavor
# (redhat/configs/kernel-aarch64-64k-rhel.config) or a running kernel's
# /boot/config-$(uname -r).  A fragment "=m" is satisfied by =m or =y; "=y"
# only by =y.  Prints every option that is missing or weaker than required
# and exits 1 if there is any; prints the page size the config builds for.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
CFG=${1:?usage: check-config.sh CONFIG [FRAGMENT]}
FRAG=${2:-$HERE/zerocopy.fragment}
[ -r "$CFG" ] || { echo "cannot read $CFG" >&2; exit 2; }
[ -r "$FRAG" ] || { echo "cannot read $FRAG" >&2; exit 2; }

bad=0
while IFS='=' read -r opt want; do
	case "$opt" in ''|'#'*) continue ;; esac
	have=$(grep -E "^$opt=" "$CFG" | tail -1 | cut -d= -f2-)
	case "$want:$have" in
	m:m|m:y|y:y) ;;
	*) echo "  $opt: need ${want}, have ${have:-unset}"; bad=1 ;;
	esac
done < "$FRAG"

ps=4k
grep -q '^CONFIG_ARM64_16K_PAGES=y' "$CFG" && ps=16k
grep -q '^CONFIG_ARM64_64K_PAGES=y' "$CFG" && ps=64k
if [ $bad = 0 ]; then
	echo "OK: $CFG has every option in $(basename "$FRAG") (page size $ps)"
else
	echo "FAIL: $CFG lacks the options above (page size $ps)"
fi
exit $bad
