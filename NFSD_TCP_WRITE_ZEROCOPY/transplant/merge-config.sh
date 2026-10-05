#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# merge-config.sh BASE_CONFIG OUT_CONFIG [TREE]
#
# Make a host's known-good kernel config (BASE_CONFIG, e.g. the .config Mike
# points at on the 16K host) carry every option the page-loan series and its
# harness need (zerocopy.fragment beside this script), without touching
# anything else in it:
#
#   1. check BASE_CONFIG as it is and list what it lacks;
#   2. merge in only the options it lacks or has weaker
#      (scripts/kconfig/merge_config.sh -m); one it has =y stays =y;
#   3. resolve the result with olddefconfig in TREE (default: this checkout),
#      which leaves TREE/.config holding it;
#   4. check the resolved config again: an option Kconfig dropped or demoted
#      (an unmet dependency) is named and the script exits 1.
#
# OUT_CONFIG gets the resolved config, OUT_CONFIG.report the two checks.
# BASE_CONFIG is never modified.  The page size stays the base's own.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
BASE=${1:?usage: merge-config.sh BASE_CONFIG OUT_CONFIG [TREE]}
OUT=${2:?usage: merge-config.sh BASE_CONFIG OUT_CONFIG [TREE]}
TREE=${3:-$(cd "$HERE/../.." && pwd)}
FRAG=$HERE/zerocopy.fragment
CHECK=$HERE/check-config.sh
REPORT=$OUT.report

[ -r "$BASE" ] || { echo "cannot read $BASE" >&2; exit 2; }
[ -x "$TREE/scripts/kconfig/merge_config.sh" ] || { echo "$TREE is not a kernel tree" >&2; exit 2; }
BASE=$(readlink -f "$BASE")
case "$(readlink -f "$OUT")" in "$BASE"|"$(readlink -f "$TREE")/.config")
	echo "OUT_CONFIG must not be the base config or TREE/.config" >&2; exit 2 ;;
esac

{
	echo "== base: $BASE"
	"$CHECK" "$BASE" "$FRAG" || true
} | tee "$REPORT"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
# Merge only what the base lacks or has weaker (unset, or =m where =y is
# needed): an option it already satisfies, =y where the fragment says =m
# included, is left exactly as the base has it.
: > "$work/need.fragment"
while IFS='=' read -r opt want; do
	case "$opt" in ''|'#'*) continue ;; esac
	have=$(grep -E "^$opt=" "$BASE" | tail -1 | cut -d= -f2-)
	case "$want:$have" in m:m|m:y|y:y) ;; *) echo "$opt=$want" >> "$work/need.fragment" ;; esac
done < "$FRAG"
sed 's/^/  adding: /' "$work/need.fragment" | tee -a "$REPORT"
# -m: merge only (no make); the base's page-size choice is left alone since
# the fragment names no page-size option.
( cd "$work" && "$TREE/scripts/kconfig/merge_config.sh" -m -O "$work" "$BASE" "$work/need.fragment" ) \
	> "$work/merge.log" 2>&1 || { cat "$work/merge.log" >&2; exit 1; }
grep -E 'Value of|override' "$work/merge.log" | sed 's/^/  merge: /' | tee -a "$REPORT"
cp "$work/.config" "$TREE/.config"
make -C "$TREE" -s olddefconfig || { echo "olddefconfig failed in $TREE" >&2; exit 1; }
cp "$TREE/.config" "$OUT"

{
	echo "== resolved: $OUT"
	"$CHECK" "$OUT" "$FRAG"
} | tee -a "$REPORT"
rc=${PIPESTATUS[0]}
[ "$rc" = 0 ] || echo "Kconfig dropped or demoted the options above: fix their dependencies in the base, then re-run" | tee -a "$REPORT"
exit "$rc"
