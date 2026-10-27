#!/usr/bin/python3 -I
# pickpage.py [n]: print the struct page address of n region pages now in a
# page pool with both halves posted (pp_magic set, pp_ref_count 2), chosen
# at random, read from /proc/kcore.  The region's struct pages start at the
# vmemmap entry of the region's phys_base.
import random, struct, sys
n = int(sys.argv[1]) if len(sys.argv) > 1 else 1
rg = dict(l.split(None, 1) for l in open('/sys/kernel/debug/page_pool_rx_region') if ' ' in l)
phys, pages = int(rg['phys_base'], 16), int(rg['pages'])
f = open('/proc/kcore', 'rb'); hdr = f.read(64)
phoff = struct.unpack_from('<Q', hdr, 32)[0]; phentsize, phnum = struct.unpack_from('<HH', hdr, 54)
f.seek(phoff); segs = []
for i in range(phnum):
    t, fl, off, va, pa, fs, ms, al = struct.unpack('<IIQQQQQQ', f.read(phentsize))
    if t == 1: segs.append((va, ms, off))
def rd(a, n):
    for va, ms, off in segs:
        if va <= a < va + ms: f.seek(off + a - va); return f.read(n)
# vmemmap and memstart from the first RAM range's struct page (see README)
memstart = 0x80000000; vmemmap = 0xffffff7fc0000000
base = vmemmap + ((phys - memstart) >> 14) * 64
c = []
for i in range(pages):
    b = rd(base + i * 64, 48); magic = struct.unpack_from('<Q', b, 8)[0]; ref = struct.unpack_from('<q', b, 40)[0]
    if magic >> 48 == 0xdead and ref == 2: c.append(base + i * 64)
for a in random.sample(c, min(n, len(c))): print('%x' % a)
