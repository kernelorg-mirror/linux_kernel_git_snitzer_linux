#!/usr/bin/python3 -I
# rttsplit-read.py RUN: RUN's rttsplit.out (rttsplit.bt) as a few lines:
# the client's round trip (ss on the client at the window's end), then
# the server's ack delay by where the ACK is sent from, the window it
# advertises, how long the window stays closed, the queue wait before
# nfsd's read, the read, nfsd's wake latency and the rings' NAPI cadence,
# each as mean and the median's bucket.
import re, statistics, sys
L = sys.argv[2] if len(sys.argv) > 2 else '/var/tmp/zc-run-logs'
r = sys.argv[1]; d = f'{L}/{r}'
scal = {}; hist = {}
cur = None
for l in open(f'{d}/rttsplit.out'):
    l = l.rstrip('\n')
    m = re.match(r'^(@\w+(?:\[[^\]]*\])?): (-?\d+)$', l)
    if m: scal[m.group(1)] = int(m.group(2)); cur = None; continue
    m = re.match(r'^(@\w+(?:\[[^\]]*\])?):$', l)
    if m: cur = m.group(1); hist[cur] = []; continue
    m = re.match(r'^\[(-?[\d.]+[KMG]?)(?:, (-?[\d.]+[KMG]?|\.\.\.)\))?\]?\s+(\d+) \|', l)
    if m and cur:
        def num(x):
            if x is None or x == '...': return None
            mult = {'K': 1024, 'M': 1 << 20, 'G': 1 << 30}
            return float(x[:-1]) * mult[x[-1]] if x[-1] in mult else float(x)
        lo = num(m.group(1)); hi = num(m.group(2)); hist[cur].append((lo, hi if hi is not None else lo + 1, int(m.group(3))))
    m = re.match(r'^traced_us (\d+)', l)
    if m: traced = int(m.group(1))
def med(name):
    b = hist.get(name)
    if not b: return '-'
    n = sum(c for _, _, c in b); acc = 0
    for lo, hi, c in b:
        acc += c
        if acc * 2 >= n: return f'{lo:g}-{hi:g}' if hi != lo + 1 else f'{lo:g}'
    return '-'
def avg(name, div=1000.0):
    v = scal.get(name); return f'{v / div:.1f}' if v is not None else '-'
def cnt(name): return scal.get(name, 0)
# the client's round trips, from ss on the client at the window's end
rtts = []; minr = []
try:
    for l in open(f'{d}/pmu/ss-client.txt'):
        m = re.search(r' rtt:([\d.]+)/([\d.]+)', l)
        if m: rtts.append(float(m.group(1)) * 1000)
        m = re.search(r'minrtt:([\d.]+)', l)
        if m: minr.append(float(m.group(1)) * 1000)
except FileNotFoundError: pass
tot = re.search(r'^total (\S+) GB/s', open(f'{d}/pmu.log').read(), re.M)
print(f"{r}: total {tot.group(1) if tot else '?'} GB/s; traced {traced / 1e6:.2f} s; segments {cnt('@segs')}, at the window's wall {cnt('@wall')} ({100.0 * cnt('@wall') / max(cnt('@segs'), 1):.1f}%)")
if rtts: print(f"  client's round trip (ss, {len(rtts)} connections): smoothed median {statistics.median(rtts):.0f} us, {min(rtts):.0f} to {max(rtts):.0f}; floor (minrtt) median {statistics.median(minr):.0f} us")
print("  ack delay, arrival of the first unacked segment to the ACK's transmit, by where the ACK is sent from:")
for c in ('ring', 'read', 'nfsd other'):
    k = f'[{c}]'
    print(f"    {c:10} acks {cnt('@acks' + k):>9}  mean {avg('@ack_avg_ns' + k):>6} us  median bucket {med('@ack_us' + k):>10} us | window chosen mean {avg('@adv_avg_b' + k, 1024.0):>6} KiB, median bucket {med('@adv_kib' + k):>10} KiB | transmits with nothing unacked {cnt('@tx_nothing_unacked' + k)}")
print("  the window closed (room under a frame at a segment's arrival) to a window of 64 KiB or more chosen:")
for c in ('ring', 'read', 'nfsd other'):
    k = f'[{c}]'
    if cnt('@reopened' + k): print(f"    {c:10} reopened {cnt('@reopened' + k):>8}  mean {avg('@closed_avg_ns' + k):>6} us  median bucket {med('@closed_us' + k):>10} us")
print(f"  nfsd's read: queue wait (first unread byte's arrival to the read) mean {avg('@qwait_avg_ns')} us, median bucket {med('@qwait_us')} us; reads {cnt('@reads')}, bytes waiting median bucket {med('@avail_kib')} KiB, a read {avg('@read_avg_ns')} us mean, median bucket {med('@read_us')} us, read median bucket {med('@read_kib')} KiB")
print(f"  nfsd threads: wakeup to running mean {avg('@wake_avg_ns')} us, median bucket {med('@wake_us')} us, {cnt('@wakes')} wakes")
print(f"  rings: NAPI polls {cnt('@polls')}, gap mean {avg('@poll_gap_avg_ns')} us, median bucket {med('@poll_gap_us')} us, frames a poll median bucket {med('@poll_work')}")
