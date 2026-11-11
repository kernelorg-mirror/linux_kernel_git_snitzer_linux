#!/usr/bin/python3 -I
# heldsplit-read.py RUN...: for each of run485.sh's runs, the region's
# pages held split by where they sit, as means over heldsplit.bt's span:
# the rings' posted buffers (two ports, four rings of 128 buffers, two
# buffers a page: 512 pages, taken as full), the sockets' receive queues
# (sockq.py's bytes over 16,384, the median), a loaned record's stages
# (heldsplit.bt's page-microseconds over the span; a record's pages are
# its page-pool bytes over 15,480, its bytes at the read for assembly),
# and the rest: the median held (regionsample.py, over the same span) less
# all of those, which is a page's way back after its release (the skb
# that still holds its other buffer, a pool's stack or alloc cache before
# the next post) and any page the trace did not see.
import re, statistics as st, sys
L = '/var/tmp/zc-run-logs'
PAGE, L3, PPB = 16384, 32 << 20, 15480
STAGES = ['1 assembly', '2 to WRITE', '3 drive', '4 to release', '5 no WRITE']
for run in sys.argv[1:]:
    d = f'{L}/{run}'
    o = open(f'{d}/heldsplit.out').read()
    span = int(re.search(r'traced_us (\d+)', o).group(1))
    t1 = float(open(f'{d}/heldsplit.t1').read()); t0 = t1 - span / 1e6
    s = {k: int(v) for k, v in re.findall(r'@sum\[(.+?)\]: (\d+)', o)}
    n = {k: int(v) for k, v in re.findall(r'@n\[(.+?)\]: (\d+)', o)}
    pages = {k: s.get(k, 0) / span / PPB for k in STAGES}
    held = [int(l.split()[1]) for l in list(open(f'{d}/pmu/region-sec.tsv'))[1:]
            if t0 <= float(l.split()[0]) <= t1]
    sq = [int(l.split()[2]) for l in open(f'{d}/sockq.txt')]
    conns = st.median(int(l.split()[1]) for l in open(f'{d}/sockq.txt'))
    rows = [('rings, posted (taken as full)', 512), ('sockets, receive queues', st.median(sq) / PAGE)]
    rows += [(k, pages[k]) for k in STAGES]
    hmed, hmean = st.median(held), st.mean(held)
    seen = sum(v for _, v in rows)
    rows += [('rest: the way back, unseen', hmean - seen)]
    wr = re.search(r'^total.*', open(f'{d}/pmu.log').read(), re.M)
    print(f'{run}: {wr.group(0) if wr else "?"}; traced {span/1e6:.2f} s; held over it: mean {hmean:.0f},'
          f' median {hmed:.0f} ({len(held)} samples); {conns:.0f} connections;'
          f' records published {n.get("published", 0)}, WRITE seen {n.get("released, WRITE seen", 0)},'
          f' no WRITE {n.get("released, no WRITE seen", 0)}, error/abort {n.get("error or abort", 0)}')
    for k, v in rows:
        print(f'  {k:32s} {v:7.0f} pages {v*PAGE/2**20:6.1f} MiB {100*v/hmean:5.1f}%')
