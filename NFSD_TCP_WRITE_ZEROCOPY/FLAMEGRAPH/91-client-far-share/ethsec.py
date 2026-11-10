#!/usr/bin/python3 -I
# ethsec.py OUT SECS INTERVAL REGEX DEV...: every INTERVAL seconds for SECS,
# each DEV's ethtool -S counters whose names match REGEX, one row a tick
# (the clock, then dev:counter columns; the header names them), the
# counters as read (cumulative): take differences to get rates.
import re, subprocess, sys, time
out, secs, iv, rx = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), re.compile(sys.argv[4])
devs = sys.argv[5:]
def read(d):
    r = {}
    for l in subprocess.run(['ethtool', '-S', d], capture_output=True, text=True).stdout.splitlines():
        k, _, v = l.strip().partition(':')
        if rx.search(k) and v.strip().isdigit():
            r[k] = int(v)
    return r
with open(out, 'w') as f:
    t0 = time.monotonic(); n = 0; cols = None
    while True:
        now = time.time(); row = {}
        for d in devs:
            for k, v in read(d).items():
                row[f'{d}:{k}'] = v
        if cols is None:
            cols = sorted(row); f.write('time\t' + '\t'.join(cols) + '\n')
        f.write('%.3f\t' % now + '\t'.join(str(row.get(c, '')) for c in cols) + '\n'); f.flush()
        n += 1
        if n * iv >= secs: break
        time.sleep(max(0, t0 + n * iv - time.monotonic()))
