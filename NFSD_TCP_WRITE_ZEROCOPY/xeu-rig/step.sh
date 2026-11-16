#!/bin/bash
# step.sh DIR NAME CMD...: run one step that could take the host down, so that
# if it does, the record says which step it was and how far it got (Mike, 16
# November).  One command per step: never chain the risky ones (rmmod, insmod,
# a parameter, nfsd's start or stop) in one step.
#
# Before the step: its name and command go to DIR/steps.log, the journal and
# /dev/kmsg (so netconsole carries it to the client, see crashlog.sh), and are
# synced.  During it: each output line goes to DIR/NAME.out and /dev/kmsg as it
# comes, synced.  After it: the exit status, the same way.
[ $# -ge 3 ] || { echo "usage: $0 DIR NAME CMD..."; exit 2; }
dir=$1 name=$2; shift 2
mkdir -p "$dir" || exit 2
out=$dir/$name.out
note() {
	echo "$(date -Is) $name: $*" >>"$dir/steps.log"
	echo "zc-step $name: $*" >/dev/kmsg
	sync "$dir/steps.log"
	journalctl --sync 2>/dev/null
}
note "start: $*"
: >"$out"
"$@" 2>&1 | while IFS= read -r l; do
	printf '%s\n' "$l"
	printf '%s\n' "$l" >>"$out"
	sync "$out"
	printf 'zc-step %s| %s\n' "$name" "$l" >/dev/kmsg
done
rc=${PIPESTATUS[0]}
note "done, exit $rc"
exit $rc
