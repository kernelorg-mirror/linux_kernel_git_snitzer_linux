#!/usr/bin/python3 -I
# markread.py RUN...: for each run, the mark it ran at (geometry.txt),
# GB/s, the cycles a WRITE on nfsd's CPUs (8-55) and the ring CPUs (0-7,
# 56-63) from twoport's perf stat of the window (idle cycles counted as the
# core counts them), the held median and peak, and the client's round
# trip.
import re, statistics, sys
L = '/var/tmp/zc-run-logs'
for r in sys.argv[1:]:
    d = f'{L}/{r}'; log = open(f'{d}/pmu.log').read()
    mark = re.search(r'want_eighths (\S+)', open(f'{d}/pmu/geometry.txt').read())
    tot = re.search(r'^total (\S+) GB/s', log, re.M)
    wps = re.search(r'WRITEs (\d+), threads woken (\d+), context switches (\d+)', log)
    win = re.search(r'perf stat.*-- sleep (\d+)', open(f'{d}/pmu.log').read())
    cyc = {}
    for l in open(f'{d}/pmu/perf-stat-window.txt'):
        f = l.split(',')
        if len(f) > 3 and f[3] == 'cycles' and f[0].startswith('CPU'):
            cyc[int(f[0][3:])] = int(f[1])
    W = 10.0
    writes = int(wps.group(1)) * W if wps else 0
    nfsd = sum(v for c, v in cyc.items() if 8 <= c < 56); ring = sum(v for c, v in cyc.items() if c < 8 or c >= 56)
    held = sorted(int(l.split()[1]) for l in list(open(f'{d}/pmu/region-sec.tsv'))[1:])
    peak = re.search(r'region pages in use at most[^:]*: (\d+)', log)
    rtts = [float(m) * 1000 for m in re.findall(r' rtt:([\d.]+)/', open(f'{d}/pmu/ss-client.txt').read())]
    print(f"{r}: mark {mark.group(1) if mark else '?'} | {tot.group(1) if tot else '?'} GB/s | a WRITE: nfsd {nfsd / writes / 1000:.1f}k cycles, rings {ring / writes / 1000:.1f}k, {int(wps.group(2)) / int(wps.group(1)):.2f} woken, {int(wps.group(3)) / int(wps.group(1)):.2f} switches | held median {held[len(held) // 2]} ({held[len(held) // 2] * 16 / 1024:.0f} MiB), peak {peak.group(1) if peak else '?'} | client rtt median {statistics.median(rtts):.0f} us ({min(rtts):.0f} to {max(rtts):.0f})")
