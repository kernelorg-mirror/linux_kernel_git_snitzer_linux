#!/bin/bash
# trace-loans.sh LABEL [SECONDS]
#
# Run on tardis1 while the client drives NFS WRITE load. Records the loaned
# receive-fragment geometry (loan-geometry.bt) and the nfsd WRITE dispositions
# for the same interval, plus the state that labels the run.
#
#   ./trace-loans.sh passA-rx0 60     # stock xeu, or rx_ip_align=0
#   ./trace-loans.sh passB-rx2 60     # patched xeu, rx_ip_align=2
set -u
L=${1:?usage: $0 LABEL [SECONDS]}
T=${2:-60}
D=$(cd "$(dirname "$0")" && pwd)
O=$D/out-$L-$(date +%Y%m%d-%H%M%S)
mkdir -p "$O"

{
	echo "label:          $L"
	echo "date:           $(date -Is)"
	echo "kernel:         $(uname -r)"
	echo "xeu module:     $(modinfo -F filename xeu 2>/dev/null)"
	echo "xeu package:    $(rpm -qf "$(modinfo -F filename xeu 2>/dev/null)" 2>/dev/null)"
	echo "rx_ip_align:    $(cat /sys/module/xeu/parameters/rx_ip_align 2>/dev/null || echo 'absent (stock xeu)')"
	echo "loan pages:     $(cat /sys/module/sunrpc/parameters/svc_tcp_rx_loan_pages 2>/dev/null)"
	echo "io_cache_write: $(cat /sys/kernel/debug/nfsd/io_cache_write 2>/dev/null)"
	echo "mtu:"
	ip -o link show | awk '{print "  " $2, $4, $5}'
	echo "xeu dmesg:"
	dmesg | grep -E 'rx_ip_align' | sed 's/^/  /'
} > "$O/env.txt"
cat "$O/env.txt"

echo "tracing for ${T}s into $O ..."
timeout -s INT "$T" bpftrace "$D/loan-geometry.bt" > "$O/loan-geometry.txt" 2>&1 &
timeout -s INT "$T" bpftrace -e '
tracepoint:nfsd:nfsd_write_direct,
tracepoint:nfsd:nfsd_write_vector,
tracepoint:nfsd:nfsd_write_dontcache { @[probe] = count(); }' > "$O/dispositions.txt" 2>&1 &
wait

echo "== loan geometry"; cat "$O/loan-geometry.txt"
echo "== WRITE dispositions"; cat "$O/dispositions.txt"
