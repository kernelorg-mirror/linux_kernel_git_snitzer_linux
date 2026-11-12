#!/usr/bin/python3
# retx-read.py RUNDIR: retx.sh's counters over the window, client and
# server, every TCP counter that moved by more than 0 (the retransmit
# kinds first), and from the client's ss samples the timers its
# connections were on and their retrans/backoff/probes.
import re, sys
from collections import Counter

d = sys.argv[1]

def parse(path):
    vals = {}
    lines = open(path).read().splitlines()
    for a, b in zip(lines[::2], lines[1::2]):
        ka, kb = a.split(), b.split()
        if ka[0] != kb[0]:
            continue
        for k, v in zip(ka[1:], kb[1:]):
            try:
                vals[ka[0] + k] = int(v)
            except ValueError:
                pass
    return vals

KEY = ['Tcp:RetransSegs', 'TcpExt:TCPLossProbes', 'TcpExt:TCPLossProbeRecovery',
       'TcpExt:TCPTimeouts', 'TcpExt:TCPSpuriousRTOs', 'TcpExt:TCPFastRetrans',
       'TcpExt:TCPSlowStartRetrans', 'TcpExt:TCPLostRetransmit', 'TcpExt:TCPRetransFail',
       'TcpExt:TCPDSACKRecv', 'TcpExt:TCPDSACKOldSent', 'TcpExt:TCPDSACKOfoSent',
       'TcpExt:TCPDSACKUndo', 'TcpExt:TCPSpuriousRtxHostQueues', 'TcpExt:TCPWinProbe',
       'TcpExt:TCPKeepAlive', 'TcpExt:TCPZeroWindowDrop', 'TcpExt:TCPToZeroWindowAdv',
       'TcpExt:TCPFromZeroWindowAdv', 'TcpExt:TCPWantZeroWindowAdv', 'TcpExt:TCPRcvQDrop',
       'TcpExt:TCPOFOQueue', 'TcpExt:TCPOFODrop', 'TcpExt:TCPBacklogDrop',
       'TcpExt:TCPRcvCoalesce', 'TcpExt:TCPDelivered']
for side in ('cli', 'srv'):
    try:
        a, b = parse(f'{d}/net0.{side}'), parse(f'{d}/net1.{side}')
    except OSError as e:
        print(f'{side}: {e}')
        continue
    delta = {k: b[k] - a.get(k, 0) for k in b if b[k] - a.get(k, 0)}
    print(f'{side}: ' + ', '.join(f'{k.split(":")[1]} {delta[k]}' for k in KEY if k in delta))

timers, conns, samples = Counter(), Counter(), 0
retr_max = Counter()
try:
    for line in open(f'{d}/cli-ss.txt'):
        if line.startswith('#'):
            samples += 1
            continue
        m = re.search(r'timer:\((\w+)', line)
        if m:
            timers[m.group(1)] += 1
        for k in ('backoff', 'probes'):
            m = re.search(rf'\b{k}:(\d+)', line)
            if m and int(m.group(1)):
                conns[k] += 1
        m = re.search(r'\bretrans:\d+/(\d+)', line)
        if m:
            retr_max['total'] = max(retr_max['total'], int(m.group(1)))
    print(f'client ss: {samples} samples; timers ' + ', '.join(f'{k} {v}' for k, v in timers.most_common())
          + f'; connection-samples with backoff {conns["backoff"]}, probes {conns["probes"]}'
          + f'; most retrans on one connection {retr_max["total"]}')
except OSError as e:
    print(f'client ss: {e}')
