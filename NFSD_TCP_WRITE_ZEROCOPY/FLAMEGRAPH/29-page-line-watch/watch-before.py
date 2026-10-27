import re, collections
exec(open('watch-count.py').read().split('F = {')[0])
F = {0: 'flags', 8: 'magic', 40: 'ppref', 48: 'refcnt'}
W = {('ppref',)}
def side(c): return 'ring' if c in (0,2,4,6) else 'nfsd'
def isw(e): return e['off'] != 8 or e['stack'][0] in ('page_pool_clear_pp_info','page_pool_set_pp_info')
tot = collections.Counter(); gaps = collections.defaultdict(list); lastw = collections.Counter(); wgap = collections.defaultdict(list)
for n in range(1, 5):
    w = open('watch.when').read(); base = int(re.search(r'w%d: page ([0-9a-f]+)' % n, w).group(1), 16)
    ev = parse('w%d.txt' % n, base)
    for i, e in enumerate(ev):
        if e['stack'] and e['stack'][0] == 'svc_tcp_rx_borrow_segment' or (len(e['stack'])>1 and e['stack'][1]=='svc_tcp_rx_borrow_segment' and e['off']==8 and e['stack'][0]!='svc_tcp_rx_append_ref' and e['stack'][0]!='svc_tcp_rx_get_borrowed'):
            p = ev[i-1] if i else None
            if not p: continue
            k = ('same cpu' if p['cpu']==e['cpu'] else 'other '+side(p['cpu'])) + ' ' + F[p['off']] + ' ' + p['stack'][0]
            tot[k] += 1; gaps[k].append((e['t']-p['t'])*1e6)
            # last write before it
            for j in range(i-1, -1, -1):
                q = ev[j]
                if isw(q):
                    kk = ('same cpu' if q['cpu']==e['cpu'] else 'other '+side(q['cpu'])) + ' ' + F[q['off']] + ' ' + q['stack'][0]
                    lastw[kk] += 1; wgap[kk].append((e['t']-q['t'])*1e6); break
def med(x): x=sorted(x); return x[len(x)//2]
print('== the access just before each borrow_segment read of the line: count, median us before it')
for k, v in tot.most_common(12): print('  %6d  %8.1f  %s' % (v, med(gaps[k]), k))
print('== the last write before each borrow_segment read')
for k, v in lastw.most_common(10): print('  %6d  %8.1f  %s' % (v, med(wgap[k]), k))
