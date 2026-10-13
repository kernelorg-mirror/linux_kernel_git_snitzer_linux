#!/usr/bin/env python3
# stall-sample.py <out file> <seconds> <port> [<port> ...]
#
# For the silent port: five times a second, the counters either side of the
# wire, so that when a port stops there is a before and an after.  Run on the
# server and on the client at once (over the management network, not over a
# port under test), then stall-read.py lines the two up.
#
# Each line is a time, then name=value pairs, cumulative as the kernel has
# them: for each port its frames and bytes sent and received at the wire
# (mlx5: *_phy) or summed over its rings (xeu), its drops, pauses and FEC
# corrections and its interrupts taken; then TCP's counters, ARP's failures,
# and once a second the state of each port's neighbours.
import re, subprocess, sys, time

out, secs, ports = sys.argv[1], float(sys.argv[2]), sys.argv[3:]
MLX = ('tx_packets_phy rx_packets_phy tx_bytes_phy rx_bytes_phy rx_steer_missed_packets '
       'rx_corrected_bits_phy rx_discards_phy tx_discards_phy rx_out_of_buffer tx_queue_stopped '
       'tx_queue_wake rx_pcs_symbol_err_phy rx_symbol_err_phy tx_pause_ctrl_phy rx_pause_ctrl_phy '
       'rx_crc_errors_phy link_down_events_phy').split()
XEU = ('port_rx_fifo_tc0_packets port_rx_fifo_tc0_drop_packets port_rx_non_prom_drop_packets '
       'dev_tx_timeouts dev_restart').split()
RING = ('rx_packets rx_bytes tx_packets tx_bytes tx_tx_stop_q rx_hw_tail_drop_packets '
        'rx_hw_pp_drop_packets rx_hw_other_packets rx_sw_dropped rx_pp_alloc_empty').split()
TCP = ('InSegs OutSegs RetransSegs TCPTimeouts TCPLossProbes TCPSpuriousRTOs '
       'TCPSpuriousRtxHostQueues TCPFastRetrans DelayedACKs TCPBacklogDrop TCPRcvQDrop '
       'TCPZeroWindowDrop TCPToZeroWindowAdv TCPFromZeroWindowAdv TCPWantZeroWindowAdv '
       'PruneCalled TCPOFOQueue TCPDSACKOldSent TCPDSACKRecv TCPLostRetransmit TCPPureAcks '
       'InDiscards OutDiscards OutNoRoutes InNoRoutes TCPDelivered').split()
ring = re.compile(r'ring_\d+_(\w+)')

def port(d):
    v = {}
    o = subprocess.run(['ethtool', '-S', d], capture_output=True, text=True).stdout
    for l in o.splitlines():
        k, _, x = l.strip().partition(': ')
        if not x.isdigit():
            continue
        m = ring.fullmatch(k)
        if m:
            if m.group(1) in RING:
                v[m.group(1)] = v.get(m.group(1), 0) + int(x)
        elif k in MLX or k in XEU:
            v[k] = int(x)
    return v

def tcp():
    v = {}
    for f in ('/proc/net/netstat', '/proc/net/snmp'):
        L = open(f).read().splitlines()
        for i in range(0, len(L) - 1, 2):
            for k, x in zip(L[i].split()[1:], L[i + 1].split()[1:]):
                if k in TCP:
                    v[k] = x
    return v

def arp():
    r = u = 0
    for l in open('/proc/net/stat/arp_cache').read().splitlines()[1:]:
        f = l.split(); r += int(f[6], 16); u += int(f[11], 16)
    return {'arp_res_failed': r, 'arp_unres_discards': u}

def irqs():
    v = {p: 0 for p in ports}
    for l in open('/proc/interrupts'):
        for p in ports:
            if p in l or ('mlx5' in l and False):
                v[p] += sum(int(x) for x in l.split()[1:] if x.isdigit())
    return v

def neigh(p):
    o = subprocess.run(['ip', 'neigh', 'show', 'dev', p], capture_output=True, text=True).stdout
    return ','.join('%s:%s' % (l.split()[0], l.split()[-1]) for l in o.splitlines())

f = open(out, 'w'); t0 = time.time(); n = 0
while time.time() - t0 < secs:
    t = time.time(); row = ['%.3f' % t]
    iq = irqs()
    for p in ports:
        row += ['%s.%s=%s' % (p, k, x) for k, x in sorted(port(p).items())]
        row.append('%s.irqs=%d' % (p, iq[p]))
        if n % 5 == 0:
            row.append('%s.neigh=%s' % (p, neigh(p)))
    row += ['%s=%s' % kv for kv in sorted(tcp().items())]
    row += ['%s=%s' % kv for kv in arp().items()]
    f.write(' '.join(row) + '\n'); f.flush(); n += 1
    time.sleep(max(0, 0.2 - (time.time() - t)))
