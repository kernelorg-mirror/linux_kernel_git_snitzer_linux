# loan-budget.sh - sourced by the rig scripts: turn TCP receive loans on or
# off for a run, and put the host's budget back after it.
#
# There is no loan switch since 22 October 2026 ("SUNRPC: let the region
# budget alone turn TCP receive loans on"): a record is loaned only when the
# budget its transport draws on is above 0.  Since the computed default
# ("net: compute the rx loan region's default sizes") the budgets are no
# module parameters: each namespace has its write budget in debugfs, at
# rx_loan/<inum>/write/pages.  At 0 and never written, the namespace lends
# the region's write share once a region is carved; the first write ends
# that computed default, even a write of 0, and nothing but a new namespace
# brings it back.  Pages from outside the region draw no credit, so on a rig
# with no region (loopback, brd, nvme-loop) the budget's size does not
# matter: any value above 0 only turns loans on.  With a region it is the
# region's write share, so that a run never waits for credit.  The scripts
# start nfsd first: the namespace's directory is there from then on.
#
#   loans_save     keep the budget as it is (call once, first)
#   loans_on       a budget, unless the namespace already lends
#   loans_off      the budget 0: every record is copied ("no_budget")
#   loans_restore  the budget as loans_save found it; a computed default
#                  (requested 0) comes back as the budget then in force, a
#                  fixed value that no longer tracks a later carve or
#                  split change
#   loans_state    "on (pages N, budget M)" or "off"
# nfs-server's namespace is init's; root reads it (/proc/self as a fallback)
LB_NS=$({ readlink /proc/1/ns/net 2>/dev/null || readlink /proc/self/ns/net; } | tr -dc 0-9)
LB_W=/sys/kernel/debug/rx_loan/$LB_NS/write
LB_RG=/sys/kernel/debug/page_pool_rx_region

# the budget in force: the first field of the write budget's stats line
lb_budget() {
	awk '$1 == "budget" {print $2}' $LB_W/stats 2>/dev/null
}

loans_save() {
	LB_SAVED=$(cat $LB_W/pages)
	LB_SAVED_B=$(lb_budget)
}

loans_state() {
	local p b
	p=$(cat $LB_W/pages 2>/dev/null || echo 0)
	b=$(lb_budget)
	if [ "$p" != 0 ] || [ "${b:-0}" != 0 ]; then
		echo "on (pages $p, budget ${b:-0})"
	else
		echo off
	fi
}

loans_on() {
	local pages read b
	[ "$(cat $LB_W/pages)" != 0 ] && return 0
	b=$(lb_budget)
	[ "${b:-0}" != 0 ] && return 0
	# no region file until a region is carved
	pages=$(awk '$1 == "pages" {print $2}' $LB_RG 2>/dev/null) || pages=
	read=$(awk '$1 == "read_pages" {print $2}' $LB_RG 2>/dev/null) || read=
	[ -n "$pages" ] && pages=$((pages - ${read:-0}))
	[ "${pages:-0}" -gt 0 ] || pages=1
	echo $pages > $LB_W/pages
}

loans_off() {
	echo 0 > $LB_W/pages
}

# A computed default is restored as the budget in force at loans_save: a
# fixed value that no longer tracks a later carve or split change.
loans_restore() {
	[ -n "${LB_SAVED:-}" ] || return 0
	if [ "$LB_SAVED" != 0 ]; then
		echo "$LB_SAVED" > $LB_W/pages
	else
		echo "${LB_SAVED_B:-0}" > $LB_W/pages
	fi
}
