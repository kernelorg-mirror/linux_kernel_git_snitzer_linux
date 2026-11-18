import sys, re, collections
rx = collections.defaultdict(collections.Counter)
rd = collections.defaultdict(collections.Counter)
for line in open(sys.argv[1]):
    m = re.match(r'@(rx|rd)\[(\d+), (\d+)\]: (\d+)', line.strip())
    if m:
        (rx if m.group(1) == 'rx' else rd)[int(m.group(2))][int(m.group(3))] += int(m.group(4))
ring = lambda c: c < 8 or c > 55
socks = [s for s in rx if s in rd]
same = tot = 0; rows = []
for s in socks:
    home = rd[s].most_common(1)[0][0]
    n = sum(rx[s].values()); k = rx[s][home]
    same += k; tot += n
    rows.append((k / n, home, n, len(rd[s]), len(rx[s]), rx[s].most_common(2)))
rows.sort()
print(f"sockets {len(socks)} (rx only {len(set(rx) - set(rd))}, read only {len(set(rd) - set(rx))})")
print(f"TCP receive on the CPU that reads the socket most: {100 * same / max(tot, 1):.1f}% of {tot} packets")
print(f"sockets read on more than one CPU in 5 s: {sum(1 for r in rows if r[3] > 1)}")
print(f"sockets whose home is a ring CPU: {sum(1 for r in rows if ring(r[1]))}")
print("worst ten: share on home, home CPU, packets, read CPUs, rx CPUs, top rx CPUs")
for r in rows[:10]:
    print(f"   {100 * r[0]:5.1f}%  cpu {r[1]:2d}  {r[2]:7d}  {r[3]}  {r[4]}  {r[5]}")
