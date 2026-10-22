# loan-budget.sh - sourced by the rig scripts: turn TCP receive loans on or
# off for a run, and put the host's budgets back after it.
#
# There is no loan switch since 22 October 2026 ("SUNRPC: let the region
# budget alone turn TCP receive loans on"): a record is loaned only when the
# budget its transport draws on is above 0.  A transport in the initial
# namespace draws on svc_tcp_rx_budget_pages when that is above 0, else on
# svc_tcp_rx_budget_default_pages.  Pages from outside the region draw no
# credit, so on a rig with no region (loopback, brd, nvme-loop) the budget's
# size does not matter: any value above 0 only turns loans on.  With a region
# it is the region's size, so that a run never waits for credit.
#
#   loans_save     keep the two budgets as they are (call once, first)
#   loans_on       a default budget, unless the transport already has one
#   loans_off      both budgets 0: every record is copied ("no_budget")
#   loans_restore  the budgets as loans_save found them
#   loans_state    "on (initial N, default M)" or "off"
LB_SP=/sys/module/sunrpc/parameters
LB_RG=/sys/kernel/debug/page_pool_rx_region

loans_save() {
	LB_SAVED="$(cat $LB_SP/svc_tcp_rx_budget_pages) $(cat $LB_SP/svc_tcp_rx_budget_default_pages)"
}

loans_state() {
	local i d
	i=$(cat $LB_SP/svc_tcp_rx_budget_pages 2>/dev/null || echo 0)
	d=$(cat $LB_SP/svc_tcp_rx_budget_default_pages 2>/dev/null || echo 0)
	if [ "$i" != 0 ] || [ "$d" != 0 ]; then
		echo "on (initial $i, default $d)"
	else
		echo off
	fi
}

loans_on() {
	local pages
	[ "$(cat $LB_SP/svc_tcp_rx_budget_pages)" != 0 ] && return 0
	[ "$(cat $LB_SP/svc_tcp_rx_budget_default_pages)" != 0 ] && return 0
	pages=$(awk '$1 == "pages" {print $2}' $LB_RG 2>/dev/null)
	echo "${pages:-1}" > $LB_SP/svc_tcp_rx_budget_default_pages
}

loans_off() {
	echo 0 > $LB_SP/svc_tcp_rx_budget_pages &&
	echo 0 > $LB_SP/svc_tcp_rx_budget_default_pages
}

# Both to 0 first: the budgets together may not exceed the region, so
# restoring one while the other still holds a run's value can be refused.
loans_restore() {
	[ -n "${LB_SAVED:-}" ] || return 0
	set -- $LB_SAVED
	loans_off
	echo "$1" > $LB_SP/svc_tcp_rx_budget_pages
	echo "$2" > $LB_SP/svc_tcp_rx_budget_default_pages
}
