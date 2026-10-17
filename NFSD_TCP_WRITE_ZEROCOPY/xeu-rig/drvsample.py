#!/usr/bin/env python3
# drvsample.py <out file> [seconds]: each second, what every data drive
# wrote (GB/s), how busy it was (%) and its writes, from /proc/diskstats,
# in drvchar.py's columns.  Runs until killed, or for the seconds given.
# Every fifth second each drive's two temperature sensors go to
# <out file>.temp (composite/sensor 1, C).
import os, subprocess, sys, time

def temp(k):
    v = {}
    for l in subprocess.run(['nvme', 'smart-log', '/dev/' + k[:5]], capture_output=True, text=True).stdout.splitlines():
        if l.startswith('temperature'): v['c'] = l.split(':')[1].split()[0]
        if l.startswith('Temperature Sensor 1'): v['s'] = l.split(':')[1].split()[0]
    return '%s/%s' % (v.get('c', '-'), v.get('s', '-'))

def stats():
    s = {}
    for l in open('/proc/diskstats'):
        f = l.split()
        if f[2] in names:
            s[f[2]] = (int(f[9]), int(f[12]), int(f[7]))
    return s

# The host's data drives (DRIVES, from common.sh's data_drives), tardis1's eight
# by default.
names = os.environ.get('DRIVES', '').split() or ['nvme%dn1' % n for n in range(8)]
n = int(sys.argv[2]) if len(sys.argv) > 2 else 1 << 30
o = open(sys.argv[1], 'w')
t = open(sys.argv[1] + '.temp', 'w')
o.write('s ' + ' '.join('%s_GBps %s_busy %s_wps' % (k[:5], k[:5], k[:5]) for k in names) + '\n')
p = stats(); t0 = tp = time.time(); s = 0
while s < n:
    time.sleep(max(0, t0 + s + 1 - time.time()))
    c = stats(); tc = time.time(); s += 1; dt = tc - tp
    o.write('%d %s\n' % (s, ' '.join('%.2f %.0f %.0f' % ((c[k][0] - p[k][0]) * 512 / dt / 1e9, (c[k][1] - p[k][1]) / dt / 10, (c[k][2] - p[k][2]) / dt) for k in names)))
    o.flush()
    if s % 5 == 1:
        t.write('%d %s\n' % (s, ' '.join(temp(k) for k in names))); t.flush()
    p, tp = c, tc
