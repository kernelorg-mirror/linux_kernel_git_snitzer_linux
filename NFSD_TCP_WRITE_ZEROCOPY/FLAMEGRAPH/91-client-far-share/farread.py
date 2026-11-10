#!/usr/bin/python3 -I
# farread.py RUN...: the client's far-node skb sends of round 91, by who.
# Reads cli-st-n0/n1.txt (counts of every skb a port's driver was handed,
# by node) and cli-far-{a,b}.script.xz (one in C of the far ones, with
# call chains).  Each sample is put to a cause by the first frame that
# names one, walking from the outermost frame in.
import collections, lzma, re, sys
L = '/var/tmp/zc-run-logs'
CAUSES = [  # (cause, frame regex), first match from the outside in wins
    ('rpciod: rpc_async_schedule', r'^rpc_async_schedule$'),
    ('xprtiod: transport work', r'^(xs_|xprt_|rpc_)\w*workfn$|^xs_stream_data_receive|^xs_tcp_'),
    ('softirq: rx (ACKs, from NAPI)', r'^(net_rx_action|__napi_poll)$'),
    ('softirq: TCP small queues', r'^(tcp_tsq_handler|tcp_tasklet_func|tcp_tsq_workfn)$'),
    ('timer: TCP', r'^(tcp_delack_timer|tcp_write_timer|tcp_keepalive_timer|tcp_retransmit_timer)'),
    ('qdisc run (net_tx_action)', r'^net_tx_action$'),
    ('release_sock backlog', r'^(release_sock|__release_sock)$'),
    ('syscall', r'^(__x64_sys_|do_syscall_64$)'),
]
def samples(f):
    hdr = None; st = []
    for l in lzma.open(f, 'rt'):
        if not l.strip():
            if hdr: yield hdr, st
            hdr = None; st = []
        elif not l[0].isspace(): hdr = l.split()
        else:
            p = l.split(); st.append(p[1] if len(p) > 1 else '?')
    if hdr: yield hdr, st
def counts(f):
    v = [int(l.split(',')[0]) for l in open(f) if 'net_dev_start_xmit' in l]
    return v  # [ens3np0, ens6np0]
for r in sys.argv[1:]:
    d = f'{L}/{r}'
    n0, n1 = counts(f'{d}/cli-st-n0.txt'), counts(f'{d}/cli-st-n1.txt')
    log = open(f'{d}/pmu.log').read()
    nic = dict(re.findall(r'client NIC of (\w): ([0-9.]+)% of its packets sent from the far', log))
    print(f'== {r}: {re.search(r"^total.*", log, re.M).group(0)}')
    for p, near, far in (('a', n0[0], n1[0]), ('b', n1[1], n0[1])):
        t = near + far
        print(f'port {p}: skbs {t}, from the far node {far} ({100*far/max(t,1):.2f}%); NIC packets from the far node {nic.get(p, "-")}%')
    for p in 'ab':
        cause = collections.Counter(); comm = collections.Counter(); cpu = collections.Counter(); sig = collections.Counter(); n = 0
        for h, st in samples(f'{d}/cli-far-{p}.script.xz'):
            n += 1
            c = re.sub(r'[0-9]+', 'N', h[0]) if h[0].startswith('kworker') else h[0]
            comm[c] += 1; cpu[h[-1].strip('[]').lstrip('0') or '0'] += 1
            k = '(other)'
            for name, rx in CAUSES:
                if any(re.match(rx, s) for s in reversed(st)): k = name; break
            cause[k] += 1
            sig[(k, ' < '.join(s for s in st[1:9]))] += 1
        print(f' port {p} far samples {n}')
        for k, v in cause.most_common(): print(f'   {100*v/max(n,1):5.1f}%  {k}')
        print('   comms: ' + ', '.join(f'{k} {100*v/n:.0f}%' for k, v in comm.most_common(6)) if n else '')
        print('   CPUs:  ' + ' '.join(f'{k}:{v}' for k, v in sorted(cpu.items(), key=lambda x: int(x[0]))) if n else '')
        for (k, s), v in sig.most_common(6): print(f'   {100*v/n:5.1f}%  [{k}] {s}')
