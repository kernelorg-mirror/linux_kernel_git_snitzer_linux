#!/usr/bin/python3 -I
# symmib.py A B N: nfsd-CPU cycles a MiB written (fio's GB/s), A against B
import collections, lzma, re, sys
L='/var/tmp/zc-run-logs'
IDLE=set('default_idle_call cpu_do_idle arch_cpu_idle do_idle cpu_idle_poll'.split())
def load(r):
    gb=float(re.search(r'^total ([0-9.]+)',open(f'{L}/{r}/pmu.log').read(),re.M).group(1))
    mib=gb*1e9/1048576*8.0
    c=collections.Counter()
    for l in lzma.open(f'{L}/{r}/pmu.script.xz','rt'):
        f=l.split()
        if len(f)<4 or f[3] in IDLE: continue
        if f[1].startswith('cycles'): c[f[3]]+=int(f[0])
    return {k:v/mib for k,v in c.items()}
a,b=load(sys.argv[1]),load(sys.argv[2]); N=int(sys.argv[3])
print('all: %.0f -> %.0f cycles a MiB' % (sum(a.values()), sum(b.values())))
d=sorted(set(a)|set(b), key=lambda k: b.get(k,0)-a.get(k,0))
print('gained most:')
for k in d[::-1][:N]: print('%8.0f %8.0f %+8.0f  %s' % (a.get(k,0), b.get(k,0), b.get(k,0)-a.get(k,0), k))
print('lost most:')
for k in d[:8]: print('%8.0f %8.0f %+8.0f  %s' % (a.get(k,0), b.get(k,0), b.get(k,0)-a.get(k,0), k))
