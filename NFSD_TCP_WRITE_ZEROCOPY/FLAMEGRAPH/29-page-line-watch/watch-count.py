import sys, re, collections
def parse(fn, base):
    ev = []; cur = None
    for l in open(fn):
        m = re.match(r'\s*(\S+)\s+(\d+)\s+\[(\d+)\]\s+([\d.]+):\s+mem:0x([0-9a-f]+)', l)
        if m:
            cur = dict(comm=m.group(1), tid=int(m.group(2)), cpu=int(m.group(3)), t=float(m.group(4)), off=int(m.group(5), 16) - base, stack=[])
            ev.append(cur); continue
        m = re.match(r'\s+[0-9a-f]+ (\S+)', l)
        if m and cur is not None: cur['stack'].append(m.group(1))
    return ev
F = {0: 'flags', 8: 'pp_magic', 40: 'pp_ref_count', 48: 'mapcount/refcount'}
for n in range(1, 5):
    w = open('watch.when').read(); base = int(re.search(r'w%d: page ([0-9a-f]+)' % n, w).group(1), 16)
    ev = parse('w%d.txt' % n, base)
    print('== w%d page %x: %d hits in %.2f s' % (n, base, len(ev), ev[-1]['t'] - ev[0]['t']))
    c = collections.Counter()
    for e in ev:
        who = 'ring' if e['cpu'] in (0, 2, 4, 6) else 'nfsd' if 8 <= e['cpu'] <= 55 else 'cpu%d' % e['cpu']
        fn = e['stack'][0] if e['stack'] else '?'
        par = e['stack'][1] if len(e['stack']) > 1 else ''
        c[(F.get(e['off'], e['off']), who, fn, par)] += 1
    for k, v in c.most_common(18): print('  %6d  %-18s %-5s %-36s < %s' % (v, *k))
