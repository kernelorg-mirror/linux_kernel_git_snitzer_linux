#!/usr/bin/env python3
"""waitq.py <run dir>: waitq.sh's samples as bytes and waits by stage.

A reply's bytes, once the work item's sendmsg() has them, are in TCP's
queue unsent (Send-Q less the unacked segments), then below TCP as skbs
in the qdisc (tc backlog) and in the NIC (BQL in flight, till the
transmit completion), then sent and awaiting the client's ACK (unacked
less what is still below TCP), then at the client unread (its Recv-Q).
skmem t is the bytes of the socket's skbs still held below TCP.  Each
stage's mean bytes over the samples, divided by the rate the run sent
(the wire's, from run/total.txt or run.log), is its mean wait (Little).
"""
import re, sys, os
O = sys.argv[1]
rate = None
for line in open(os.path.join(O, 'run.log')):
    m = re.match(r'total ([0-9.]+) GB/s', line)
    if m:
        rate = float(m.group(1)) * 1e9
st = {}
def add(k, v):
    st.setdefault(k, [0.0] * 11)[cur] += v
cur = 0
pend = None
for line in open(os.path.join(O, 'waitq.txt')):
    if line.startswith('# sample'):
        cur = int(line.split()[2])
        continue
    f = line.split()
    if f[0] == 'ss' and len(f) >= 4 and f[1].isdigit():
        pend = int(f[2])			# Send-Q of the socket line
        continue
    if f[0] == 'ss' and pend is not None:
        m = re.search(r'mss:(\d+)', line); mss = int(m.group(1)) if m else 0
        m = re.search(r'unacked:(\d+)', line); un = int(m.group(1)) * mss if m else 0
        m = re.search(r'skmem:\(r\d+,rb\d+,t(\d+)', line); t = int(m.group(1)) if m else 0
        sendq = pend
        add('send-q', sendq)
        add('unsent', max(sendq - un, 0))
        add('unacked', min(un, sendq))
        add('below tcp (skmem t)', t)
        pend = None
    elif f[0] == 'qdisc':
        v = f[3]; mult = 1
        if v.endswith('Kb'): mult, v = 1024, v[:-2]
        elif v.endswith('Mb'): mult, v = 1 << 20, v[:-2]
        elif v.endswith('b'): v = v[:-1]
        add('qdisc', float(v) * mult)
    elif f[0] == 'bql':
        add('nic (bql)', int(f[3]))
    elif f[0] == 'cli':
        add('client recv-q', int(f[1]))
n = max(i for v in st.values() for i, x in enumerate(v) if x) if st else 0
def mean(k):
    v = st.get(k, [0] * 11)[1:n + 1]
    return sum(v) / len(v) if v else 0
rows = [('unsent in TCP', mean('unsent')), ('qdisc', mean('qdisc')),
        ('NIC, till completion', mean('nic (bql)')),
        ('sent, awaiting the ACK', max(mean('unacked') - mean('below tcp (skmem t)'), 0)),
        ('client, unread', mean('client recv-q'))]
print(f"{n} samples at {rate / 1e9:.2f} GB/s; skmem t (below TCP) {mean('below tcp (skmem t)') / 1e6:.2f} MB, qdisc + NIC {(mean('qdisc') + mean('nic (bql)')) / 1e6:.2f} MB")
for k, b in rows:
    print(f"  {k:26s} {b / 1e6:7.2f} MB  {b / rate * 1e6:7.1f} us")
tot = sum(b for _, b in rows)
print(f"  {'in all':26s} {tot / 1e6:7.2f} MB  {tot / rate * 1e6:7.1f} us")
