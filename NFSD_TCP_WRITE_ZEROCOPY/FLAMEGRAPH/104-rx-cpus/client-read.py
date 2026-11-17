#!/usr/bin/python3
# client-read.py OUT: summarise client-read.sh's probes of one run.
# Times are per NFS READ (an RPC of rsize bytes), from the client's
# /proc/self/mountstats over the window: queue (from the task's start to
# its transmit), rtt (transmit to the reply received) and execute (start
# to end); what execute leaves after queue and rtt is the client's work
# after the reply.  rpc_stats_latency gives their spread.  The ss samples
# give each end's socket queues, and by Little's law the time a byte
# spends in them at the run's rate.
import re, sys, os, statistics as st

O = sys.argv[1]

def mountstats(path):
	"""READ op totals summed over the rxr mounts; tcp transports by port."""
	ops = [0] * 8
	xprts = {}
	mnt = None
	for line in open(path):
		m = re.match(r'device \S+ mounted on (\S+) with fstype nfs', line)
		if m:
			mnt = m.group(1) if '/mnt/rxr-' in m.group(1) else None
			continue
		if not mnt:
			continue
		s = line.split()
		if s and s[0] == 'xprt:' and s[1] == 'tcp':
			v = [int(x) for x in s[2:]]
			xprts[(mnt[:10], v[0])] = v	# one port's transports are shared by its mounts
		elif s and s[0] == 'READ:':
			v = [int(x) for x in s[1:9]]
			ops = [a + b for a, b in zip(ops, v)]
	return ops, xprts

def pct(v, p):
	v = sorted(v)
	return v[min(len(v) - 1, int(p / 100 * len(v)))] if v else 0

o0, x0 = mountstats(f'{O}/cli-mountstats.0')
o1, x1 = mountstats(f'{O}/cli-mountstats.1')
d = [b - a for a, b in zip(o0, o1)]
nops = d[0]
print(f'READs in the window (mountstats): {nops}, timeouts {d[2]}, retransmitted {d[1] - d[0]}')
if nops:
	q, r, e = d[5] / nops, d[6] / nops, d[7] / nops
	print(f'  mean ms a READ: queue {q:.3f}  rtt {r:.3f}  execute {e:.3f}  after the reply {e - q - r:.3f}')
	print(f'  bytes a READ received {d[4] / nops:.0f}')

# transports: xprt tcp fields after 'tcp': port bind_count connect_count
# connect_time idle_time sends recvs bad_xids req_u bklog_u max_slots
# sending_u pending_u
print('transports (per port of the client; means over the window, per send):')
rows = []
for k in sorted(x1):
	if k not in x0:
		continue
	a, b = x0[k], x1[k]
	sends = b[5] - a[5]
	if sends <= 0:
		continue
	rows.append((k, sends, (b[8] - a[8]) / sends, (b[9] - a[9]) / sends,
		     b[10], (b[11] - a[11]) / sends, (b[12] - a[12]) / sends))
for k, sends, req, bk, maxs, sending, pending in rows:
	print(f'  {k[0]} :{k[1]:<5} sends {sends:7d}  in flight {req:6.1f}  backlog {bk:6.1f}  '
	      f'max_slots {maxs:4d}  sending {sending:5.1f}  pending {pending:6.1f}')
if rows:
	print(f'  all {len(rows)}: in flight {st.mean(r[2] for r in rows):.1f} mean a transport, '
	      f'backlog {st.mean(r[3] for r in rows):.1f}, sending {st.mean(r[5] for r in rows):.1f}, '
	      f'pending {st.mean(r[6] for r in rows):.1f}')

# rpc_stats_latency: backlog, rtt, execute in us
lat = {'backlog': [], 'rtt': [], 'execute': []}
for line in open(f'{O}/cli-latency.trace'):
	if 'rpc_stats_latency' not in line or 'READ' not in line:
		continue
	for f in lat:
		m = re.search(rf'\b{f}=(\d+)', line)
		if m:
			lat[f].append(int(m.group(1)))
if lat['execute']:
	print(f'rpc_stats_latency: {len(lat["execute"])} READs in 1 s, us:')
	for f, v in lat.items():
		print(f'  {f:8s} mean {st.mean(v):7.0f}  p50 {pct(v, 50):6d}  p90 {pct(v, 90):6d}  '
		      f'p99 {pct(v, 99):6d}  max {max(v):6d}')

def ssq(path):
	"""Recv-Q and Send-Q of every NFS socket in every sample, bytes."""
	rq, sq, rtt = [], [], []
	for line in open(path):
		s = line.split()
		if len(s) >= 4 and s[0].isdigit() and s[1].isdigit() and ':' in s[2]:
			rq.append(int(s[0])); sq.append(int(s[1]))
		m = re.search(r'\brtt:([\d.]+)/', line)
		if m:
			rtt.append(float(m.group(1)))
	return rq, sq, rtt

rate = None
for line in open(f'{O}/run.log'):
	m = re.match(r'total ([\d.]+) GB/s', line)
	if m:
		rate = float(m.group(1)) * 1e9
for side in ('cli', 'srv'):
	p = f'{O}/{side}-ss.txt'
	if not os.path.exists(p):
		continue
	rq, sq, rtt = ssq(p)
	if not rq:
		continue
	nsock = len(rq) / 10
	line = (f'{side} sockets ({nsock:.0f}): Recv-Q mean {st.mean(rq) / 1e6:.2f} MB max {max(rq) / 1e6:.2f}, '
		f'Send-Q mean {st.mean(sq) / 1e6:.2f} MB max {max(sq) / 1e6:.2f}, rtt mean {st.mean(rtt):.3f} ms')
	if rate:
		per = rate / nsock
		line += (f'; at {per / 1e9:.2f} GB/s a socket a byte waits {1e3 * st.mean(rq) / per:.3f} ms in Recv-Q, '
			 f'{1e3 * st.mean(sq) / per:.3f} ms in Send-Q')
	print(line)

# client CPUs over the window.  Busy is what the CPUs did, 100 less
# %idle and %iowait: a CPU in iowait is idle with I/O outstanding, and
# io_uring's wait for completions counts there where libaio's does not
# (FLAMEGRAPH round 107: t2-r863 9.38% iowait, the same work as libaio's
# t2-r862).  iowait is given beside it.
busy, iow = {}, {}
col = None
for line in open(f'{O}/cli-mpstat-w.txt'):
	s = line.split()
	if s and s[0] == 'Average:' and s[1] == 'CPU':
		col = {k: i for i, k in enumerate(s)}
	elif s and s[0] == 'Average:' and s[1].isdigit():
		idle = float(s[col['%idle']]) if col else float(s[-1])
		w = float(s[col['%iowait']]) if col and '%iowait' in col else 0.0
		busy[int(s[1])] = 100 - idle - w
		iow[int(s[1])] = w
if busy:
	n0 = [busy[c] for c in busy if c < 24]
	n1 = [busy[c] for c in busy if c >= 24]
	top = sorted(busy.items(), key=lambda x: -x[1])[:8]
	print(f'client CPUs busy: node 0 mean {st.mean(n0):.0f}%, node 1 mean {st.mean(n1):.0f}%; busiest '
	      + ' '.join(f'{c}:{b:.0f}%' for c, b in top)
	      + f'; iowait mean {st.mean(iow.values()):.1f}%, not counted as busy')
p = f'{O}/cli-perf-comm.txt'
if os.path.exists(p):
	print('client cycles by command: ' + '; '.join(' '.join(l.split()[:2]) for l in open(p).readlines()[:8]))

# the drives: /proc/diskstats over the window (reads completed, ms reading)
def disks(path):
	l = open(path).read().split('\n')
	t = float(l[0])
	d = {}
	for line in l[1:]:
		s = line.split()
		if len(s) > 7 and re.fullmatch(r'nvme[0-7]n1', s[2]):
			d[s[2]] = (int(s[3]), int(s[6]), int(s[5]))
	return t, d
if os.path.exists(f'{O}/diskstats.w1'):
	t0, a = disks(f'{O}/diskstats.w0')
	t1, b = disks(f'{O}/diskstats.w1')
	dt = t1 - t0
	cmds = sum(b[k][0] - a[k][0] for k in b)
	ms = sum(b[k][1] - a[k][1] for k in b)
	sect = sum(b[k][2] - a[k][2] for k in b)
	if cmds:
		print(f'drives over {dt:.1f} s: {cmds / dt / 1e3:.0f}k commands/s of {sect * 512 / cmds / 1024:.0f} KiB, '
		      f'{sect * 512 / dt / 1e9:.1f} GB/s, {1e3 * ms / cmds:.0f} us a command, '
		      f'{ms / dt / 1e3 / len(b):.1f} in flight a drive')
p = f'{O}/rtcread.txt'
if os.path.exists(p):
	print('server (rtcread.bt):')
	for line in open(p):
		if line.startswith('@') and '_us:' in line:
			print('  ' + line.strip())
