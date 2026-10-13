#!/usr/bin/env python3
# drvchar.py <out dir> <cap seconds> <drive>:<GB/s or 0>[:<fio option>,...] ...
#
# What a data drive takes before it steps down.  Locally, no NFS: one fio
# for each drive named, writing the files the two-port load writes to it
# (four of 512 MiB, direct, over and over) at the rate given (0: as fast
# as it goes), with every drive's rate and busy time read each second
# from /proc/diskstats and its temperature every ten.
#
# Options after the rate go to that drive's fio (fsync=8 for an fsync
# every 8 MiB, as a COMMIT is; rw=randwrite).
#
# A drive has stepped down when it has written under 85% of what it wrote
# over seconds 3 to 12 for five seconds running.  Its fio is stopped
# TAIL seconds (15) after that, or at the cap.  STOP=0 runs every drive
# to the cap.  A second under that 85% is counted as a dip either way.
#
#   drvchar.py $L/drvchar-1 300 0:7.25 1:0 2:7.25 3:0
#
# Writes <out>/sec.tsv (a line a second: for each drive GB/s, % busy and writes),
# <out>/temp.tsv and <out>/summary.txt.
import os, subprocess, sys, time

out, cap = sys.argv[1], int(sys.argv[2])
drives = {}; extra = {}
for a in sys.argv[3:]:
    f = a.split(':')
    drives[int(f[0])] = float(f[1])
    extra[int(f[0])] = ['--' + x for x in f[2].split(',')] if len(f) > 2 else []
TAIL = int(os.environ.get('TAIL', 15))
STOP = os.environ.get('STOP', '1') != '0'
os.makedirs(out, exist_ok=True)

def stats():
    s = {}
    for l in open('/proc/diskstats'):
        f = l.split()
        if f[2].startswith('nvme') and f[2].endswith('n1'):
            s[f[2]] = (int(f[9]), int(f[12]), int(f[7]))
    return s

def temp(n):
    try:
        o = subprocess.run(['nvme', 'smart-log', '/dev/nvme%d' % n], capture_output=True, text=True).stdout
        v = {}
        for l in o.splitlines():
            if l.startswith('temperature'): v['c'] = l.split(':')[1].split()[0]
            if l.startswith('Temperature Sensor 1'): v['s1'] = l.split(':')[1].split()[0]
            if 'T1 Trans Count' in l: v['t1'] = l.split(':')[1].strip()
            if 'T2 Trans Count' in l: v['t2'] = l.split(':')[1].strip()
        return '%s/%s/%s/%s' % (v.get('c'), v.get('s1'), v.get('t1'), v.get('t2'))
    except Exception:
        return '-'

procs = {}
for n, r in drives.items():
    d = '/hs/nvme%d/drvchar' % n
    os.makedirs(d, exist_ok=True)
    cmd = ['fio', '--name=c', '--directory=' + d, '--rw=write', '--bs=1M', '--direct=1',
           '--ioengine=libaio', '--iodepth=32', '--numjobs=4', '--size=512M', '--time_based',
           '--runtime=%d' % (cap + 30), '--group_reporting', '--output=%s/fio-%d.txt' % (out, n)]
    if r:
        cmd.append('--rate=%d' % (r * 1e9 / 4))
    cmd += extra[n]
    procs[n] = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

# fio lays its files out first; the clock starts when every drive is writing.
names = sorted(drives)
sec = open(out + '/sec.tsv', 'w')
tmp = open(out + '/temp.tsv', 'w')
sec.write('s ' + ' '.join('nvme%d_GBps nvme%d_busy nvme%d_wps' % (n, n, n) for n in names) + '\n')
tmp.write('s ' + ' '.join('nvme%d_C/sensor1/T1/T2' % n for n in names) + '\n')
hist = {n: [] for n in names}
dips = {}; dip1 = {}
step = {}	# drive -> (second, GB written by then)
done = {}	# drive -> second its fio was stopped
total = {n: 0.0 for n in names}
time.sleep(4)
p = stats(); t0 = tp = time.time(); s = 0
while s < cap and len(done) < len(names):
    time.sleep(max(0, t0 + s + 1 - time.time()))
    c = stats(); tc = time.time(); s += 1; dt = tc - tp
    row = []
    for n in names:
        k = 'nvme%dn1' % n
        g = (c[k][0] - p[k][0]) * 512 / dt / 1e9
        b = (c[k][1] - p[k][1]) / dt / 10
        row.append('%.2f %.0f %.0f' % (g, b, (c[k][2] - p[k][2]) / dt))
        if n in done:
            continue
        total[n] += g * dt
        h = hist[n]; h.append(g)
        if len(h) >= 13 and g < 0.85 * sorted(h[2:12])[5]:
            dips[n] = dips.get(n, 0) + 1; dip1.setdefault(n, s)
        if n not in step and len(h) >= 17:
            base = sorted(h[2:12])[5]
            if all(x < 0.85 * base for x in h[-5:]):
                step[n] = (s - 4, total[n] - sum(h[-5:]) * 1.0, base)
        if STOP and n in step and s >= step[n][0] + TAIL:
            procs[n].terminate(); done[n] = s
    sec.write('%d %s\n' % (s, ' '.join(row))); sec.flush()
    if s % 10 == 1:
        tmp.write('%d %s\n' % (s, ' '.join(temp(n) for n in names))); tmp.flush()
    p, tp = c, tc
for n in names:
    if n not in done:
        procs[n].terminate(); done[n] = s
for n in names:
    procs[n].wait()
    subprocess.run('rm -rf /hs/nvme%d/drvchar' % n, shell=True)
def tag(n): return ' '.join([str(drives[n] or 'all')] + [x[2:] for x in extra[n]])
with open(out + '/summary.txt', 'w') as f:
    for n in names:
        h = hist[n]; base = sorted(h[2:12])[5] if len(h) >= 12 else float('nan')
        if n in step:
            after = h[step[n][0] - 1:]
            l = 'nvme%d asked %s: %.1f GB/s, stepped down at %d s after %.0f GB, then %.1f GB/s (wrote %.0f GB in %d s)' % (
                n, tag(n), base, step[n][0], step[n][1], sum(after) / max(1, len(after)), total[n], done[n])
        else:
            l = 'nvme%d asked %s: %.1f GB/s, no step down in %d s and %.0f GB (last 10 s %.1f GB/s)' % (
                n, tag(n), base, done[n], total[n], sum(h[-10:]) / max(1, len(h[-10:])))
        l += '; seconds under 85%%: %d, the first at %s s' % (dips.get(n, 0), dip1.get(n, '-'))
        f.write(l + '\n'); print(l)
