#!/usr/bin/python3
# regionsample.py <out.tsv> <seconds> [interval_ms]: the receive region's
# pages held, sampled, for a kernel whose region has no low-water mark of its
# own over the whole region.
#
# On such a kernel (shards, CPU caches and per-device slots), `min_free` is
# the sum of the shards' low-water marks only: a page in a CPU cache or free
# on a device's slot counts as taken, and each shard reaches its low at its
# own moment, so `pages - min_free` overstates the peak, by up to about
# 20,000 pages on tardis2.  Here a page is held when it is in none of the
# shards, the CPU caches (both in `free`) or the slots (`free` and `cached`
# of each `mapped` line).  A sample can miss a dip shorter than the
# interval, so the maximum is a lower bound of the peak.
import sys, time

RG = '/sys/kernel/debug/page_pool_rx_region'
out, secs = sys.argv[1], float(sys.argv[2])
step = (float(sys.argv[3]) if len(sys.argv) > 3 else 20.0) / 1000


def held():
    pages = free = slots = 0
    with open(RG) as f:
        for l in f:
            w = l.split()
            if not w:
                continue
            if w[0] == 'pages':
                pages = int(w[1])
            elif w[0] in ('free', 'read_free'):	# `pages` holds both parts
                free += int(w[1])
            elif w[0] == 'mapped':
                for i in range(len(w) - 1):
                    if w[i] in ('free', 'cached'):
                        slots += int(w[i + 1])
    return pages - free - slots


end = time.monotonic() + secs
with open(out, 'w') as o:
    o.write('t\theld\n')
    while time.monotonic() < end:
        o.write('%.3f\t%d\n' % (time.time(), held()))
        time.sleep(step)
