#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Bisect-safety walk: build every commit $BASE..$BRANCH with a qualification
# config carrying CONFIG_WERROR=y (so rc==0 means the step produced zero
# compiler warnings), sparse-checking each recompiled TU (make C=1), then
# adjudicate the sparse output against the walk's own baseline.
#
# Arch-generic.  Defaults reproduce the aarch64 4K walk; for the x86_64 walk
# point ARCH/WALK_CONFIG/WORKDIR at an x86_64 tree and config, e.g.
#
#   ARCH=x86_64 WORKDIR=/path/to/worktree WALK_CONFIG=/path/to/x86_64.config \
#       NFSD_TCP_WRITE_ZEROCOPY/run-bisect-walk.sh
#
# WALK_CONFIG may be a path in the tree (resolved out of $BRANCH, so per-commit
# checkouts cannot change it) or an absolute path to a config held outside it.
set -u

WORKDIR=${WORKDIR:-/root/kernel/linux}
S=${S:-/tmp/bisect-walk}
ARCH=${ARCH:-arm64}
J=${J:-8}
BRANCH=${BRANCH:-kernel-7.1.8/main-5.NFSD_TCP_WRITE_ZEROCOPY}
BASE=${BASE:-v7.1.8-5}
WALK_CONFIG=${WALK_CONFIG:-NFSD_TCP_WRITE_ZEROCOPY/v7.1.8-3-aarch64-4k.config}

# the checker probe must be given the same target flags kbuild would use
case "$ARCH" in
    arm64)  CHECKER_ARGS=${CHECKER_ARGS:-"-mlittle-endian -m64"} ;;
    *)      CHECKER_ARGS=${CHECKER_ARGS:-"-m64"} ;;
esac

LOGDIR=$S/bisect-$ARCH-logs
SUMMARY=$S/bisect-$ARCH-summary.txt
CONFIG=$S/walk-$ARCH.config

mkdir -p "$LOGDIR"
cd "$WORKDIR" || exit 1

# hold the config outside the tree so per-commit checkouts can't change it
case "$WALK_CONFIG" in
    /*) cp "$WALK_CONFIG" "$CONFIG" ;;
    *)  git show "$BRANCH:$WALK_CONFIG" > "$CONFIG" ;;
esac
grep -q '^CONFIG_WERROR=y' "$CONFIG" || {
    echo "FATAL: $WALK_CONFIG lacks CONFIG_WERROR=y; the walk would be warning-blind" | tee -a "$SUMMARY"
    exit 1
}

: > "$SUMMARY"
echo "$ARCH walk started $(date -Is) on $(uname -r), gcc $(gcc -dumpversion), sparse $(sparse --version 2>/dev/null)" >> "$SUMMARY"

# Pre-flight: kbuild silently disables C=1 when the checker fails its
# validity probe, which would report sparse=0 while checking nothing.
# (el8/el9 ship sparse 0.6.4, which fails the __typeof_unqual__ probe;
# a git-master sparse is required.  Put it first in $PATH.)
if [ "$(bash scripts/checker-valid.sh sparse $CHECKER_ARGS)" != 1 ]; then
    echo "FATAL: sparse missing or too old for this tree (checker-valid.sh)" | tee -a "$SUMMARY"
    exit 1
fi

# The adjudication below treats step 1 as the whole-tree sparse baseline, which
# only holds if step 1 builds from scratch.  Record whether that was true.
if [ -n "$(find . -name '*.o' -not -path './.git/*' -print -quit 2>/dev/null)" ]; then
    BASELINE=warm
else
    BASELINE=cold
fi
echo "baseline=$BASELINE (cold => step 1 sparse-checks the whole tree)" >> "$SUMMARY"

fails=0
n=0
total=$(git rev-list --count $BASE..$BRANCH)
for c in $(git rev-list --reverse $BASE..$BRANCH); do
    n=$((n+1))
    # skip doc-only commits (touch nothing outside the project dir + AGENTS.md)
    if ! git diff-tree --no-commit-id --name-only -r "$c" | grep -qv '^NFSD_TCP_WRITE_ZEROCOPY/\|^AGENTS.md$'; then
        echo "$n/$total $c SKIP-DOC $(git log -1 --format=%s $c)" >> "$SUMMARY"
        continue
    fi
    git checkout -q --detach "$c" || { echo "$n/$total $c CHECKOUT-FAIL" >> "$SUMMARY"; fails=$((fails+1)); continue; }
    cp "$CONFIG" .config
    make ARCH=$ARCH olddefconfig > "$LOGDIR/$n-$c.log" 2>&1
    t0=$SECONDS
    # C=1 sparse-checks each recompiled TU; findings are reported, not fatal
    if make ARCH=$ARCH C=1 -j$J >> "$LOGDIR/$n-$c.log" 2>&1; then
        # with WERROR any compiler warning fails the build, so warning
        # lines surviving in an OK build are sparse findings
        sp=$(grep -cE ':[0-9]+:[0-9]+: +(warning|error):' "$LOGDIR/$n-$c.log" || true)
        echo "$n/$total $c OK $((SECONDS-t0))s sparse=$sp $(git log -1 --format=%s $c)" >> "$SUMMARY"
    else
        fails=$((fails+1))
        echo "$n/$total $c FAIL $((SECONDS-t0))s $(git log -1 --format=%s $c)" >> "$SUMMARY"
    fi
done

git checkout -q --detach $BRANCH
echo "$ARCH walk finished $(date -Is): $((n-fails))/$n clean, $fails failures" >> "$SUMMARY"

# ---------------------------------------------------------------------------
# Adjudicate the sparse output.
#
# A raw per-step count cannot answer the question the walk is actually asking.
# Step 1 builds from scratch and so sparse-checks the whole tree: that is the
# pre-existing baseline.  Every later step is incremental and re-checks only
# the TUs that commit recompiled.  So a finding is branch-introduced iff it
# appears at some later step and is not in step 1's set.
#
# Two artifacts otherwise produce false positives, and are normalized away:
#   1. parallel make interleaves sparse's progress output into the log, so a
#      finding can appear as ".....init/main.c:198:12: ..." — and the
#      progress stream carries '+' as well as '.' (the 2026-09-09 gate-forward
#      walk baselined ".+..+...+init/main.c:198:12: ...", which a dots-only
#      strip left unmatched and misreported as branch-introduced) -> strip a
#      leading run of dots, plus signs, slashes and spaces.
#   2. a commit that inserts lines above a pre-existing warning shifts its line
#      number (the zram fix moved one down by 2, which a count reads as a new
#      finding) -> compare on (file, column, message), dropping the line.
# ---------------------------------------------------------------------------
norm() {
    sed -E 's;^[.+/ ]+;;' | sed -E 's/^([^:]+):[0-9]+:([0-9]+): +/\1:\2: /' | sort -u
}
raw() { grep -hE ':[0-9]+:[0-9]+: +(warning|error):' "$@" 2>/dev/null; }

B=$(ls "$LOGDIR"/1-*.log 2>/dev/null | head -1)
if [ -z "$B" ]; then
    echo "sparse verdict: INDETERMINATE (no step-1 log to baseline against)" >> "$SUMMARY"
    exit $((fails > 0))
fi

raw "$B" | norm > "$S/sparse-baseline-$ARCH.txt"
: > "$S/sparse-later-$ARCH.txt"
for L in "$LOGDIR"/*.log; do
    case "$(basename "$L")" in 1-*) continue;; esac
    raw "$L" | norm >> "$S/sparse-later-$ARCH.txt"
done
sort -u -o "$S/sparse-later-$ARCH.txt" "$S/sparse-later-$ARCH.txt"
comm -13 "$S/sparse-baseline-$ARCH.txt" "$S/sparse-later-$ARCH.txt" > "$S/sparse-new-$ARCH.txt"

{
    echo "sparse baseline (whole tree, step 1, normalized): $(wc -l < "$S/sparse-baseline-$ARCH.txt")"
    echo "sparse findings seen in steps 2+ (normalized):    $(wc -l < "$S/sparse-later-$ARCH.txt")"
    if [ "$BASELINE" != cold ]; then
        echo "sparse verdict: UNSOUND — tree was warm at step 1, so the baseline is"
        echo "                partial.  Re-run from a clean tree to adjudicate."
    elif [ -s "$S/sparse-new-$ARCH.txt" ]; then
        echo "sparse verdict: $(wc -l < "$S/sparse-new-$ARCH.txt") BRANCH-INTRODUCED finding(s):"
        sed 's/^/  /' "$S/sparse-new-$ARCH.txt"
    else
        echo "sparse verdict: zero branch-introduced findings"
    fi
} >> "$SUMMARY"

exit $((fails > 0))
