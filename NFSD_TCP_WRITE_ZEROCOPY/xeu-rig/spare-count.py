#!/usr/bin/drgn
# spare-count.py [seconds [interval]] - the pages TCP sockets keep for
# exchange with threads, across all sockets (sunrpc's svc_tcp_spare_pages,
# "SUNRPC: Bound the pages TCP sockets keep for exchange with threads"), read
# from the live kernel; with no seconds, once.  Over a time it prints the
# count at each interval and ends with the most it reached and the bound in
# pages.  With the bound held still the count stays within it (a reservation
# that races another may pass it for an instant before it gives the excess
# back); a bound lowered under load leaves what was kept until it is taken
# back or its socket closes.  With no NFS connection up it is 0.
# Run as root: drgn spare-count.py 60 0.5
import os
import sys
import time

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 0
iv = float(sys.argv[2]) if len(sys.argv) > 2 else 0.5
try:
	addr = prog.symbol("svc_tcp_spare_pages").address
except LookupError:
	print("no svc_tcp_spare_pages in this sunrpc")
	sys.exit(2)
bound = int(open("/sys/module/sunrpc/parameters/svc_tcp_spare_bytes_max").read()) // os.sysconf("SC_PAGE_SIZE")
end = time.time() + secs
def count():
	n = prog.read_u64(addr)		# an atomic_long_t
	return n - (1 << 64) if n >> 63 else n


most = n = count()
while time.time() < end:
	time.sleep(iv)
	n = count()
	most = max(most, n)
	print("%.1f %d" % (time.time(), n), flush=True)
print("spare pages now %d, at most %d; bound %d pages" % (n, most, bound))
