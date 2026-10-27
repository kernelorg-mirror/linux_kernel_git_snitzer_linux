import re, collections
exec(open('watch-count.py').read().split('F = {')[0])
first = collections.Counter(); seq = collections.Counter(); gap = collections.defaultdict(list)
for n in range(1, 5):
    w = open('watch.when').read(); base = int(re.search(r'w%d: page ([0-9a-f]+)' % n, w).group(1), 16)
    ev = parse('w%d.txt' % n, base)
    for i in range(1, len(ev)):
        e, p = ev[i], ev[i-1]
        if 8 <= e['cpu'] <= 55 and p['cpu'] != e['cpu']:
            k = e['stack'][0] + (' < ' + e['stack'][1] if e['stack'][0].startswith('do_perf') else '')
            first[(k, 'after ring' if p['cpu'] in (0,2,4,6) else 'after nfsd')] += 1
            gap[(k, 'after ring' if p['cpu'] in (0,2,4,6) else 'after nfsd')].append((e['t']-p['t'])*1e6)
            # the next 4 accesses on that cpu
            s = [x['stack'][0] for x in ev[i:i+5] if x['cpu']==e['cpu']]
            seq[' > '.join(s[:4])] += 1
med = lambda x: sorted(x)[len(x)//2]
print('== an nfsd CPU\'s first access to the line after another CPU\'s: count, median us after it')
for k, v in first.most_common(10): print('  %6d %7.1f  %s, %s' % (v, med(gap[k]), *k))
print('== what that CPU does to it next')
for k, v in seq.most_common(6): print('  %6d  %s' % (v, k))
