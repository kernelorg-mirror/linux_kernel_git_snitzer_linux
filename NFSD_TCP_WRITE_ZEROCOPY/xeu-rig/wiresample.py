#!/usr/bin/env python3
# wiresample.py <out file> <seconds> <dev> [<dev> ...]: each second, what
# each server port received (GB/s, /sys/class/net/<dev>/statistics/rx_bytes)
# and the deltas of TcpExt's receive-side counters, with the time.  Its
# seconds are drvsample.py's when the two start together (twoport.sh), so
# that drives-sec.tsv and wire-sec.tsv line up row for row.
import sys, time

K = ['TCPOFOQueue', 'TCPRcvQDrop', 'PruneCalled', 'TCPBacklogDrop', 'TCPZeroWindowDrop', 'TCPToZeroWindowAdv']

def stats():
    r = [int(open('/sys/class/net/%s/statistics/rx_bytes' % d).read()) for d in devs]
    h = None
    for l in open('/proc/net/netstat'):
        if l.startswith('TcpExt:'):
            if h is None:
                h = l.split()
            else:
                v = dict(zip(h, l.split()))
                r += [int(v.get(k, 0)) for k in K]
    return r

devs = sys.argv[3:]
n = int(sys.argv[2])
o = open(sys.argv[1], 'w')
o.write('s epoch ' + ' '.join('%s_GBps' % d for d in devs) + ' ' + ' '.join(K) + '\n')
p = stats(); t0 = tp = time.time(); s = 0
while s < n:
    time.sleep(max(0, t0 + s + 1 - time.time()))
    c = stats(); tc = time.time(); s += 1; dt = tc - tp
    o.write('%d %.3f %s %s\n' % (s, tc, ' '.join('%.2f' % ((c[i] - p[i]) / dt / 1e9) for i in range(len(devs))),
                                 ' '.join('%d' % (c[i] - p[i]) for i in range(len(devs), len(c)))))
    o.flush()
    p, tp = c, tc
