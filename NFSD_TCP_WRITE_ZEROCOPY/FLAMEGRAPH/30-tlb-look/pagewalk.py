#!/usr/bin/python3 -I
# pagewalk.py <va>...: walk swapper_pg_dir through /proc/kcore for kernel
# virtual addresses, and say whether each is mapped by a block or a page.
# For arm64 with 16 KiB pages and 48-bit VAs (levels 0 to 3, a 32 MiB block
# at level 2); the linear map's PAGE_OFFSET and memstart_addr are tardis2's.
import struct, sys
f = open('/proc/kcore', 'rb'); hdr = f.read(64)
phoff = struct.unpack_from('<Q', hdr, 32)[0]; phentsize, phnum = struct.unpack_from('<HH', hdr, 54)
f.seek(phoff); segs = []
for i in range(phnum):
    t, fl, off, va, pa, fs, ms, al = struct.unpack('<IIQQQQQQ', f.read(phentsize))
    if t == 1: segs.append((va, ms, off))
def rd(a):
    for va, ms, off in segs:
        if va <= a < va + ms: f.seek(off + a - va); return struct.unpack('<Q', f.read(8))[0]
pgd = int([l.split()[0] for l in open('/proc/kallsyms') if l.split()[2] == 'swapper_pg_dir'][0], 16)
PAGE_OFFSET, MEMSTART = 0xffff000000000000, 0x80000000
def walk(va):
    tbl, out = pgd, []
    for lvl, sh, bits in ((0, 47, 1), (1, 36, 11), (2, 25, 11), (3, 14, 11)):
        d = rd(tbl + ((va >> sh) & ((1 << bits) - 1)) * 8); out.append('L%d %x' % (lvl, d))
        if d & 3 == 1: return out + ['a block at level %d' % lvl]
        if d & 3 != 3: return out + ['not mapped']
        if lvl == 3: return out + ['a 16 KiB page']
        tbl = PAGE_OFFSET + ((d & 0x0000fffffffff000 & ~0x3fff) - MEMSTART)
for a in sys.argv[1:]:
    print(a, ', '.join(walk(int(a, 16))))
