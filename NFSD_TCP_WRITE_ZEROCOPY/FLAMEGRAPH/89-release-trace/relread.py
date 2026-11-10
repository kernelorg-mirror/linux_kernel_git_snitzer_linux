#!/usr/bin/python3 -I
# relread.py RUN...: rel.bt's counts a WRITE (nfsd3_proc_write calls in
# the trace's 10 s), side by side for each RUN.
import re, sys
L = '/var/tmp/zc-run-logs'
K = [('releases', r'@release: (\d+)'),
     ('table entries', r'@sum\[entries\]: (\d+)'),
     ('runs (pages put)', r'@sum\[runs\]: (\d+)'),
     ('  of them 2 refs', r'@run_refs:\n\[1\]\s+\d+.*\n\[2\]\s+(\d+)'),
     ('pages into bulk', r'@bulk_pages\[nfsd\]: (\d+)'),
     ('bulk calls', r'@bulk_calls\[nfsd\]: (\d+)'),
     ('pages to the stack (ring_bulk)', r'@ring_pages\[nfsd\]: (\d+)'),
     ('ring_bulk calls', r'@ring_calls\[nfsd\]: (\d+)'),
     ('  of them overflowing', r'@ring_over_calls\[nfsd\]: (\d+)'),
     ('pages overflowing the stack', r'@ring_over\[nfsd\]: (\d+)'),
     ('page_pool_return_netmem, nfsd', r'@return_one\[nfsd\]: (\d+)'),
     ('rx_region_page_put, nfsd', r'@region_page_put\[nfsd\]: (\d+)'),
     ('rx_region_slot_put, nfsd', r'@slot_put\[nfsd\]: (\d+)'),
     ('page_pool_rx_region_put, nfsd', r'@region_shard_put\[nfsd\]: (\d+)'),
     ('rings: stack refills', r'@stack_refill\[other\]: (\d+)'),
     ('rings: region allocs', r'@region_alloc\[other\]: (\d+)'),
     ('rings: pages from slot caches', r'@slot_take_pages: (\d+)'),
     ('rings: DMA maps', r'@dma_map\[other\]: (\d+)'),
     ('napi_pp_put_page, nfsd', r'@napi_pp_put\[nfsd\]: (\d+)'),
     ('slot_put ns (traced)', r'@ns_sum\[slot_put\]: (\d+)'),
     ('table_release ns (traced)', r'@ns_sum\[table_release\]: (\d+)')]
T = {r: open(f'{L}/{r}/rel.txt').read() for r in sys.argv[1:]}
W = {r: int(re.search(r'@writes: (\d+)', t).group(1)) for r, t in T.items()}
print('%-34s' % 'a WRITE' + ''.join('%10s' % r for r in T))
print('%-34s' % 'WRITEs traced' + ''.join('%10d' % W[r] for r in T))
for name, p in K:
    row = []
    for r, t in T.items():
        m = re.search(p, t)
        row.append('%10.2f' % (int(m.group(1)) / W[r]) if m else '%10s' % '0')
    print('%-34s' % name + ''.join(row))
