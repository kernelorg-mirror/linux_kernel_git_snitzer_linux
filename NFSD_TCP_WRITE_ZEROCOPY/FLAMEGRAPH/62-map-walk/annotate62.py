#!/usr/bin/python3 -I
# annotate62.py PMU.SCRIPT WRITES: the loan's functions annotated from
# t2-r235's samples (perf script -F ip,sym,period,event), cycles and
# instructions a WRITE an instruction, by source line and by kind of
# instruction, for build 514's held-mark kernel.
#
# The code: sunrpc is 514's stock module (/lib/modules), its lines from 514's
# debug file; vmlinux is the held-mark build, whose image was not kept, so
# its functions are read from 514's debug vmlinux at the same offset in the
# same function (the four have the same size in both System.maps).  The
# samples are mapped by offset from the running kallsyms.
#
# Kinds: entry/exit (paciasp, autiasp, ret, bti, the frame's stp/ldp of
# x29/x30), calls (bl, blr, br, and the instruction after a bl, where a
# call's return lands its skid), atomics, loads, stores, branches, other.
# PMU samples skid: a sample on an instruction is mostly the cost of the
# one or two before it.  Also: the samples in sunrpc's PLT (calls from
# sunrpc into vmlinux, which is out of a bl's range on this boot).
import collections, os, re, subprocess, sys
REL = os.uname().release
DBG = '/usr/lib/debug/lib/modules/%s' % REL
SCR = os.environ.get('ANN_DIR', '.')
# SUNRPC_KO: the running sunrpc.ko (default: an xz -dc of the stock module in
# ANN_DIR); SUNRPC_DBG: its lines (default: 514's debug file; a module built
# by build-sunrpc.sh carries its own, so give it as both)
SUNRPC_KO = os.environ.get('SUNRPC_KO', os.path.join(SCR, 'sunrpc.ko'))
SUNRPC_DBG = os.environ.get('SUNRPC_DBG', DBG + '/kernel/net/sunrpc/sunrpc.ko.debug')
HELD_MAP = '/var/tmp/zc-run-logs/t2-514-canary/held-kernel/System.map'
FUNCS = [('sunrpc', 'svc_tcp_rx_append_ref'), ('sunrpc', '__rx_loan_table_append_ref'),
         ('vmlinux', 'rx_loan_table_release'), ('vmlinux', 'rx_loan_prefetch_pages'),
         ('vmlinux', 'rx_region_page_put')]
# ANN_FUNCS=sunrpc:svc_recv,vmlinux:__lwq_dequeue annotates those instead
if os.environ.get('ANN_FUNCS'):
    FUNCS = [tuple(x.split(':')) for x in os.environ['ANN_FUNCS'].split(',')]

def kallsyms():
    k = {}
    for l in open('/proc/kallsyms'):
        f = l.split()
        k.setdefault(f[2], int(f[0], 16))
    return k

def nm(path):
    if path.endswith('System.map'):
        out = sorted(open(path).read().split('\n'), key=lambda l: int(l.split()[0], 16) if len(l.split()) == 3 else 0)
    else:
        out = subprocess.run(['nm', '-n', path], capture_output=True, text=True).stdout.split('\n')
    a, nxt = {}, {}
    rows = [(int(l.split()[0], 16), l.split()[2]) for l in out if len(l.split()) == 3]
    for i, (v, s) in enumerate(rows):
        if s not in a:
            a[s] = v
            nxt[s] = next((w for w, _ in rows[i + 1:] if w > v), v)
    return a, nxt

ks = kallsyms()
kaslr = ks['_text'] - 0xffff800080000000
held, held_end = nm(HELD_MAP)
stock, _ = nm(DBG + '/vmlinux')
# SUNRPC_TEXT: sunrpc's .text base when the run's module is no longer the
# one loaded (saved by the run); its PLT is then not read
modbase = int(os.environ.get('SUNRPC_TEXT') or open('/sys/module/sunrpc/sections/.text').read(), 16)
mod, mod_end = nm(SUNRPC_DBG)
plt = 0 if os.environ.get('SUNRPC_TEXT') else int(open('/sys/module/sunrpc/sections/.plt').read(), 16)

# per function: running start, size, the address to disassemble at, file
F = {}
for where, fn in FUNCS:
    if where == 'sunrpc':
        F[fn] = (modbase + mod[fn], mod_end[fn] - mod[fn], mod[fn], SUNRPC_KO,
                 SUNRPC_DBG)
    else:
        F[fn] = (held[fn] + kaslr, held_end[fn] - held[fn], stock[fn], DBG + '/vmlinux',
                 DBG + '/vmlinux')

samp = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
pltc = collections.Counter()
for l in open(sys.argv[1]):
    f = l.split()
    if len(f) < 3:
        continue
    try:
        p, ip = int(f[0]), int(f[2], 16)
    except ValueError:
        continue
    ev = f[1].rstrip(':')
    if plt and plt <= ip < plt + 0x2000:
        pltc[ev] += p
    for fn, (lo, sz, _, _, _) in F.items():
        if lo <= ip < lo + sz:
            samp[fn][ip - lo][ev] += p
W = float(sys.argv[2])

def disasm(path, a, sz):
    out = subprocess.run(['objdump', '-d', '--no-show-raw-insn', '--start-address=%#x' % a,
                          '--stop-address=%#x' % (a + sz), path], capture_output=True, text=True).stdout
    ins = {}
    for l in out.split('\n'):
        m = re.match(r'\s*([0-9a-f]+):\s+(\S+)\s*(.*)', l)
        if m:
            ins[int(m.group(1), 16) - a] = (m.group(2), m.group(3).split('//')[0].strip())
    return ins

def lines(path, a, offs):
    # one addr2line for the function: -a heads each address's group of
    # (function, file:line) pairs, innermost first with -i
    q = '\n'.join('%#x' % (a + o) for o in offs)
    out = subprocess.run(['addr2line', '-a', '-i', '-f', '-e', path], input=q,
                         capture_output=True, text=True).stdout.split('\n')
    res, cur, i = {}, None, 0
    while i < len(out):
        l = out[i]
        if re.match(r'^0x[0-9a-f]+$', l):
            cur = int(l, 16) - a
            res[cur] = []
            i += 1
        elif cur is not None and i + 1 < len(out):
            res[cur].append((l, re.sub(r'.*/linux-[^/]*/(\./)?', '', out[i + 1]).split(' ')[0]))
            i += 2
        else:
            i += 1
    return res

ATOM = re.compile(r'^(ld(add|clr|set|eor|smax|umax|smin|umin)|cas|swp|ld[a]?x[rp]|st[l]?x[rp]|stadd|stclr|stset)')
def kind(mn, ops, prev):
    if mn in ('paciasp', 'autiasp', 'ret', 'bti', 'hint') or (mn in ('stp', 'ldp') and 'x29, x30' in ops):
        return 'entry/exit'
    if mn in ('bl', 'blr', 'br') or (prev and prev[0] in ('bl', 'blr')):
        return 'calls'
    if ATOM.match(mn) or mn.startswith('dmb') or mn.startswith('prfm'):
        return 'atomics, barriers, prefetch' if not mn.startswith('prfm') else 'prefetch'
    if mn.startswith('ld'):
        return 'loads'
    if mn.startswith('st'):
        return 'stores'
    if mn.startswith('b') or mn in ('cbz', 'cbnz', 'tbz', 'tbnz'):
        return 'branches'
    return 'other'

tot = collections.Counter()
for fn in [f for _, f in FUNCS]:
    lo, sz, a, code, dbg = F[fn]
    ins = disasm(code, a, sz)
    s = samp[fn]
    c = sum(v['cycles'] for v in s.values()) / W
    i = sum(v['instructions'] for v in s.values()) / W
    tot['cycles'] += c; tot['instructions'] += i
    print('=' * 100)
    print('%s: %d bytes, %.0f cycles and %.0f instructions a WRITE, IPC %.2f' % (fn, sz, c, i, i / c if c else 0))
    kinds = collections.Counter(); kindi = collections.Counter(); kindn = collections.Counter()
    offs = sorted(ins)
    prev = None
    for o in offs:
        k = kind(ins[o][0], ins[o][1], prev)
        prev = ins[o]
        kinds[k] += s[o]['cycles'] / W; kindi[k] += s[o]['instructions'] / W; kindn[k] += 1
    print('  by kind:            %8s %8s %6s %6s' % ('cycles', 'insns', 'cyc %', 'count'))
    for k, v in kinds.most_common():
        print('    %-26s %7.0f %8.0f %5.1f%% %6d' % (k, v, kindi[k], 100 * v / c if c else 0, kindn[k]))
    hot = [o for o in offs if s[o]['cycles']]
    ln = lines(dbg, a, hot)
    bysrc = collections.Counter()
    for o in hot:
        bysrc[' < '.join('%s %s' % (f_, l_) for f_, l_ in ln[o][:2])] += s[o]['cycles'] / W
    print('  by source line (innermost < caller), 85% of cycles:')
    acc = 0
    for k, v in bysrc.most_common():
        print('    %6.0f %5.1f%%  %s' % (v, 100 * v / c, k))
        acc += v
        if acc > 0.85 * c:
            break
    print('  hottest instructions:')
    for o in sorted(hot, key=lambda o: -s[o]['cycles'])[:14]:
        mn, ops = ins.get(o, ('?', ''))
        print('    +%#05x %6.0f %5.1f%%  L2 %5.2f  %-8s %-34s %s' % (o, s[o]['cycles'] / W, 100 * s[o]['cycles'] / W / c,
              s[o]['l2d_cache_refill'] / W, mn, ops[:34], ln[o][0][1] if ln[o] else ''))
print('=' * 100)
print('all %d: %.0f cycles, %.0f instructions a WRITE' % (len(FUNCS), tot['cycles'], tot['instructions']))
print("sunrpc's PLT (veneers into vmlinux): %.0f cycles, %.0f instructions a WRITE" % (pltc['cycles'] / W, pltc['instructions'] / W))
