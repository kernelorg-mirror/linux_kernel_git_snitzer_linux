#!/usr/bin/python3 -I
# top.py N RUN...: the N functions with the most cycles a WRITE (nfsd's
# CPUs, the idle loop out), the mean over RUNs, with share and IPC.
import collections, lzma, re, sys
L='/var/tmp/zc-run-logs'
IDLE=set('default_idle_call cpu_do_idle arch_cpu_idle do_idle cpu_idle_poll'.split())
N=int(sys.argv[1]); R=sys.argv[2:]
c=collections.Counter(); i=collections.Counter(); l2=collections.Counter(); W=0
for r in R:
    W+=int(re.search(r'WRITEs ([0-9]+)',open(f'{L}/{r}/pmu.log').read()).group(1))*8.0
    for l in lzma.open(f'{L}/{r}/pmu.script.xz','rt'):
        f=l.split()
        if len(f)<4 or f[3] in IDLE: continue
        e=f[1]; p=int(f[0])
        if e.startswith('cycles'): c[f[3]]+=p
        elif e.startswith('instructions'): i[f[3]]+=p
        elif e.startswith('l2d'): l2[f[3]]+=p
T=sum(c.values())
print('all %.0f cycles a WRITE, IPC %.2f' % (T/W, sum(i.values())/T))
for k,v in c.most_common(N):
    print('%7.0f %5.1f%%  IPC %4.2f  L2 refill/W %6.1f  %s' % (v/W, 100*v/T, i[k]/v if v else 0, l2[k]/W, k))
