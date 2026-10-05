#!/usr/bin/env python3
"""Summarize a stress evidence tree: cell table, step recovery, pass/fail.

usage: analyze.py <evidence dir containing stress-A/ and stress-B/>
"""
import glob, json, os, statistics, sys

root = sys.argv[1]
SPIKE_AT, BACK_AT, END = 40.0, 80.0, 120.0

def cells(phase):
    p = os.path.join(root, f'stress-{phase}', 'cells.tsv')
    if not os.path.exists(p):
        return []
    rows = [l.split() for l in open(p) if l.strip()]
    head, body = rows[0], rows[1:]
    return [dict(zip(head, r)) for r in body]

def write_bw(cell_dir):
    logs = glob.glob(os.path.join(cell_dir, 'bw_bw*.log'))
    pts = []
    for f in logs:
        for line in open(f):
            parts = [x.strip() for x in line.split(',')]
            if len(parts) >= 3 and parts[2] == '1':
                pts.append((int(parts[0]) / 1000.0, int(parts[1]) * 1024 / 1e6))
    return sorted(pts)

def recovery(cell_dir):
    pts = write_bw(cell_dir)
    if not pts:
        return None
    pre = [b for t, b in pts if 10 <= t < SPIKE_AT]
    spike = [b for t, b in pts if SPIKE_AT + 5 <= t < BACK_AT]
    post = [b for t, b in pts if BACK_AT + 10 <= t < END]
    if not pre:
        return None
    base = statistics.median(pre)
    rec = None
    after = [(t, b) for t, b in pts if t >= BACK_AT]
    for i in range(len(after)):
        window = [b for t, b in after[i:i + 4]]   # 2 s at 500 ms samples
        if len(window) == 4 and statistics.mean(window) >= 0.9 * base:
            rec = after[i][0] - BACK_AT
            break
    return dict(pre_MBps=round(base), spike_MBps=round(statistics.median(spike)) if spike else 0,
                post_MBps=round(statistics.median(post)) if post else 0,
                recovery_s=None if rec is None else round(rec, 1))

def fio_mbps(cell_dir):
    try:
        text = open(os.path.join(cell_dir, 'fio.json')).read()
        jobs = json.loads(text[text.index('{'):])['jobs']
        return str(round(sum(j['write']['bw_bytes'] for j in jobs) / 1e6))
    except (OSError, ValueError, KeyError):
        return None

ok = True
for phase in 'ABCEQS':
    rows = cells(phase)
    if not rows:
        continue
    print(f'## phase {phase}')
    for r in rows:
        asked = r['budget'] != '0'
        refused = asked and int(r['rcvbuf']) < 131072
        on = asked and not refused
        if refused:
            stats = open(os.path.join(root, f'stress-{phase}', r['cell'], 'budget-after.txt')).read().split()
            budget = stats[stats.index('budget') + 1] if 'budget' in stats else '?'
            good = budget == '0' and r['fio_rc'] == '0' and r['escaped'] == '0'
            ok = ok and good
            print(r['cell'], 'PASS refused (budget 0)' if good else f'FAIL refused budget={budget}',
                  {k: r[k] for k in ('MBps', 'drops', 'alloc_fail', 'rcvq_drop', 'fio_rc')})
            continue
        r['MBps'] = fio_mbps(os.path.join(root, f'stress-{phase}', r['cell'])) or r['MBps']
        rec = recovery(os.path.join(root, f'stress-{phase}', r['cell'])) if r['prof'] == 'step' else None
        verdict = []
        if r['fio_rc'] != '0':
            verdict.append('fio_rc=' + r['fio_rc'])
        if r['escaped'] != '0':
            verdict.append('escaped')
        at_min = on and r['cell'] not in ('edge-below-n1',) and phase != 'C'
        if at_min:
            for k in ('drops', 'alloc_fail', 'rcvq_drop', 'zw_drop', 'backlog_drop'):
                if r[k] != '0':
                    verdict.append(f'{k}={r[k]}')
            if rec and (rec['recovery_s'] is None):
                verdict.append('no recovery')
        status = 'PASS' if not verdict else 'FAIL ' + ','.join(verdict)
        if not at_min:
            status = 'REPORT' + ('' if r['fio_rc'] == '0' and r['escaped'] == '0' else ' ' + ','.join(verdict))
        elif verdict:
            ok = False
        print(r['cell'], status, {k: r[k] for k in ('MBps', 'drops', 'alloc_fail', 'used_pages', 'collapsed',
              'retrans', 'loss_probes', 'rto', 'dsack', 'denied', 'paused', 'overrun')}, rec or '')
print('OVERALL', 'PASS' if ok else 'FAIL')
