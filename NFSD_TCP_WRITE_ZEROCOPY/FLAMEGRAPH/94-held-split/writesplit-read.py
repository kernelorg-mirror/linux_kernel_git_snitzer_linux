#!/usr/bin/python3 -I
# writesplit-read.py RUN...: for each of run485.sh's BT=writesplit runs,
# a WRITE's time in nfsd_direct_write() split between XFS and the drive,
# as mean pages held over the trace (writesplit.bt's byte-microseconds
# over the span and 15,480 payload bytes a page) and as each stage's mean
# time a call.
import re, sys
L = '/var/tmp/zc-run-logs'
PPB = 15480
for run in sys.argv[1:]:
    o = open(f'{L}/{run}/writesplit.out').read()
    span = int(re.search(r'traced_us (\d+)', o).group(1))
    s = {k: int(v) for k, v in re.findall(r'@sum\[(.+?)\]: (\d+)', o)}
    n = {k: int(v) for k, v in re.findall(r'@n\[(.+?)\]: (\d+)', o)}
    calls = n.get('calls, I/O seen', 0)
    whole = s['whole call']
    by = int(re.search(r'@bytes: (\d+)', o).group(1))
    # the payload's bytes a call, from the whole call's sum over its time
    print(f'{run}: {re.search(r"^total.*", open(f"{L}/{run}/pmu.log").read(), re.M).group(0)};'
          f' traced {span/1e6:.2f} s; calls {calls} with I/O, {n.get("calls, no I/O seen", 0)} without;'
          f' requests {n.get("requests issued in a WRITE\'s call", 0)} in a call,'
          f' {n.get("requests issued elsewhere", 0)} elsewhere; left {re.search(r"left in the maps: (.*)", o).group(1)}')
    rows = ['a before I/O', 'b I/O', 'drive (issue .. complete)', 'c after I/O', 'calls, no I/O seen', 'whole call']
    for k in rows:
        v = s.get(k, 0)
        print(f'  {k:28s} {v/span/PPB:7.0f} pages {v/span/PPB*16384/2**20:6.1f} MiB {100*v/whole:5.1f}% {v/by:6.0f} us a byte, mean')
    print(f'  {"b less the drive":28s} {(s["b I/O"]-s["drive (issue .. complete)"])/span/PPB:7.0f} pages')
