#!/usr/bin/python3 -I
# symdiff.py A1,A2 B1,B2: cycles a WRITE by function (nfsd's CPUs, the
# idle loop out), the mean of runs A against the mean of runs B, with the
# functions that lost and gained the most.
import collections, lzma, os, re, sys
L='/var/tmp/zc-run-logs'
IDLE=set('default_idle_call cpu_do_idle arch_cpu_idle do_idle cpu_idle_poll'.split())
def load(r):
    d=L+'/'+r; w=int(re.search(r'WRITEs ([0-9]+)',open(d+'/pmu.log').read()).group(1))*8.0
    c=collections.Counter()
    for l in open(d+'/pmu.script'):
        f=l.split()
        if len(f)<4 or not f[1].startswith('cycles') or f[3] in IDLE: continue
        c[f[3]]+=int(f[0])
    return {k:v/w for k,v in c.items()}
A=[load(r) for r in sys.argv[1].split(',')]; B=[load(r) for r in sys.argv[2].split(',')]
def avg(S,k): return sum(s.get(k,0) for s in S)/len(S)
keys=set().union(*A,*B)
d=sorted(((avg(B,k)-avg(A,k),k) for k in keys))
print('total A %.0f B %.0f'%(sum(avg(A,k) for k in keys),sum(avg(B,k) for k in keys)))
for x,k in d[:12]+d[-14:]: print('%8.0f %9.0f %9.0f  %s'%(x,avg(A,k),avg(B,k),k))
