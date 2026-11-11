#!/usr/bin/python3 -I
# sockq.py OUT SECONDS MS: every MS ms for SECONDS, the bytes charged to
# nfsd's established connections' receive queues (ss's skmem r, the
# socket's rmem_alloc: the truesize of the skbs it holds, read or not
# yet read by nfsd), summed over the connections.  One line a sample:
# epoch, connections, bytes.  A region page holds two 8 KiB receive
# buffers, so pages are about bytes over 16,384.
import re, subprocess, sys, time
out, secs, ms = sys.argv[1], float(sys.argv[2]), int(sys.argv[3])
cmd = ['ss', '-tmnH', 'state', 'established', '( sport = :2049 )']
end = time.time() + secs
with open(out, 'w') as f:
    while time.time() < end:
        t = time.time()
        o = subprocess.run(cmd, capture_output=True, text=True).stdout
        r = [int(m) for m in re.findall(r'skmem:\(r(\d+)', o)]
        f.write(f'{t:.3f} {len(r)} {sum(r)}\n')
        time.sleep(max(0, ms / 1000 - (time.time() - t)))
