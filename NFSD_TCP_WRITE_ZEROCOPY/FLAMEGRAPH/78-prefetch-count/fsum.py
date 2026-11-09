#!/usr/bin/python3 -I
# fsum.py FUNCS RUN...: cycles a WRITE in each of FUNCS (comma-separated)
# in each run, from its pmu.script.xz.
import lzma, re, sys, collections
L='/var/tmp/zc-run-logs'
F=sys.argv[1].split(',')
for r in sys.argv[2:]:
    log=open(f'{L}/{r}/pmu.log').read()
    W=int(re.search(r'WRITEs ([0-9]+)',log).group(1))*8.0
    c=collections.Counter()
    for l in lzma.open(f'{L}/{r}/pmu.script.xz','rt'):
        f=l.split()
        if len(f)>=4 and f[1].startswith('cycles') and f[3] in F: c[f[3]]+=int(f[0])
    print(r,' '.join('%s %.0f'%(k,c[k]/W) for k in F))
