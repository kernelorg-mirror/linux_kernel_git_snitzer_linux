#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# check-grouping.sh [BASE] [BRANCH]
#
# Enforce this series' history layout on the commits BASE..BRANCH -- the work
# not yet pushed (BASE default: the branch's upstream, i.e. what Mike pushed;
# with no upstream, BASE must be given: the series as a whole is laid out in
# per-sub-series groups, so only the unpushed range is checked; BRANCH
# default HEAD):
#
#   code commits, then KUnit commits (enablement first, then tests), then
#   docs commits -- never interleaved, never mixed within one commit;
#   every Fixes: tag names a commit that is on the branch (after a fold or
#   regroup the SHAs move and the tags must be re-pointed), or, for a
#   backport that keeps its upstream tag, quotes the subject of one;
#   every commit carries Assisted-by: or no AI trailer, and never
#   Co-Authored-By:.
#
# Classes, by the files a commit touches:
#   docs  -- only NFSD_TCP_WRITE_ZEROCOPY/ and AGENTS.md
#   kunit -- a *kunit* file, or a subject "... for the KUnit suites"
#            (VISIBLE_IF_KUNIT / EXPORT_SYMBOL_IF_KUNIT enablement), or any
#            other subject that names KUnit (a backported fix to KUnit's
#            own support code, the configs that enable the suites)
#   code  -- anything else
# A commit mixing docs with code or KUnit files is reported as mixed.
#
# Exit 0 if the layout holds; 1 with the offending commits listed if not;
# 2 if there is no BASE.
# Run it before handing work back after committing on a test host; if it
# fails, regroup (rebase -i with a scripted todo, backup branch first,
# tree identical after) and re-run it.
set -u
TOP=$(cd "$(dirname "$0")/../.." && pwd)
cd "$TOP" || exit 2
BRANCH=${2:-HEAD}
if [ -n "${1:-}" ]; then
	BASE=$1
elif up=$(git rev-parse -q --verify "@{upstream}" 2>/dev/null) && [ -n "$up" ]; then
	BASE=$up
else
	echo "no upstream for $(git rev-parse --abbrev-ref HEAD): pass BASE, the last commit Mike pushed" >&2
	exit 2
fi
bad=0
phase=0   # 0 code, 1 kunit, 2 docs
name=(code kunit docs)
echo "== $(git rev-list --count "$BASE..$BRANCH") commits $(git rev-parse --short "$BASE")..$(git rev-parse --short "$BRANCH")"
for c in $(git rev-list --reverse "$BASE..$BRANCH"); do
	subj=$(git log -1 --format=%s "$c")
	files=$(git diff-tree --no-commit-id --name-only -r "$c")
	nd=0; nk=0; nc=0
	for f in $files; do
		case "$f" in
		NFSD_TCP_WRITE_ZEROCOPY/*|AGENTS.md) nd=$((nd + 1)) ;;
		*kunit*) nk=$((nk + 1)) ;;
		*) nc=$((nc + 1)) ;;
		esac
	done
	case "$subj" in *[Kk][Uu]nit*) [ $nc -gt 0 ] && { nk=$((nk + nc)); nc=0; } ;; esac
	if [ $nd -gt 0 ] && [ $((nk + nc)) -gt 0 ]; then
		cls=mixed
	elif [ $nd -gt 0 ]; then cls=2
	elif [ $nk -gt 0 ]; then cls=1
	else cls=0
	fi
	msg=""
	if [ "$cls" = mixed ]; then
		msg="MIXED docs and code/KUnit files: split it"; bad=1
	elif [ "$cls" -lt "$phase" ]; then
		msg="${name[$cls]} commit after ${name[$phase]} commits: move it up"; bad=1
	else
		phase=$cls
	fi
	while read -r f fsubj; do
		[ -n "$f" ] || continue
		git merge-base --is-ancestor "$f" "$BRANCH" 2>/dev/null && continue
		# a backport's upstream tag: the commit it fixes is here under another SHA
		fsubj=${fsubj#(\"}; fsubj=${fsubj%\")}
		git log --format=%s "$BASE..$BRANCH" | grep -qxF -- "$fsubj" && continue
		msg="$msg${msg:+; }Fixes: $f is not on the branch (re-point it)"; bad=1
	done < <(git log -1 --format=%B "$c" | sed -n 's/^Fixes: \([0-9a-f]\{12,\}\) \(.*\)/\1 \2/p')
	git log -1 --format=%B "$c" | grep -qi '^Co-Authored-By:' && { msg="$msg${msg:+; }has Co-Authored-By: (use Assisted-by:)"; bad=1; }
	printf '  %-5s %s %s%s\n' "$([ "$cls" = mixed ] && echo mixed || echo "${name[$cls]}")" \
		"$(git rev-parse --short=12 "$c")" "$subj" "${msg:+   <-- $msg}"
done
if [ $bad = 0 ]; then echo "== grouping OK: code, then KUnit, then docs"; else echo "== grouping BROKEN: regroup before handing back"; fi
exit $bad
