#!/bin/bash
# pending-settled.sh apply|audit|run <command ...>: the setting pending for
# settled.sh, as if it were settled (Mike, 17 November: "configure a
# pending-settled.sh that carries changes as if 'Settle base and bound'
# were selected", for FLAMEGRAPH round 106).  It is settled.sh's setting
# with FLAMEGRAPH round 105's base and the send bound:
#
#   svc_rtc_balance Y         one connection to a work CPU (EXP_RTC_BALANCE)
#   RFS 32768                 each flow's frames on its work item's CPU (EXP_RFS)
#   tcp_limit_output_bytes    256 KiB, the bytes below TCP (EXP_TCP_LOB)
#   svc_rtc_send_max          512 KiB, the bound on a connection's send
#                             queue (EXP_RTC_SEND_MAX)
#   nconnect 16 a port        32 connections (twoport.sh's NCONN)
#
# "apply" and "audit" are settled.sh's, held to it.  "run" runs a command
# (write-check.sh, read-check.sh, a round's script) with the setting in its
# environment, so that the settled.sh apply each of them makes holds it
# too; the host is settled.sh's again after, without it.  Any of the
# variables already set overrides its value here.  Nothing here changes
# settled.sh: a change moves there only on Mike's word.
cd "$(dirname "$0")" || exit 1
export EXP_RTC_BALANCE=${EXP_RTC_BALANCE:-Y} EXP_RFS=${EXP_RFS:-32768} \
	EXP_TCP_LOB=${EXP_TCP_LOB:-262144} EXP_RTC_SEND_MAX=${EXP_RTC_SEND_MAX:-524288} \
	NCONN=${NCONN:-16}
case "${1:-}" in
apply|audit) exec ./settled.sh "$@" ;;
run)
	shift; [ $# -gt 0 ] || { echo "pending-settled.sh run <command ...>"; exit 1; }
	"$@"; rc=$?
	env -u EXP_RTC_BALANCE -u EXP_RFS -u EXP_TCP_LOB -u EXP_RTC_SEND_MAX -u NCONN \
		./settled.sh apply > /var/tmp/zc-run-logs/pending-settled-after.txt 2>&1
	echo "settled again: $(tail -1 /var/tmp/zc-run-logs/pending-settled-after.txt)"
	exit $rc ;;
*) echo "pending-settled.sh apply|audit|run <command ...>"; exit 1 ;;
esac
