#!/usr/bin/python3
# SPDX-License-Identifier: GPL-2.0
#
# null-blk-subpage-repro.py DEVICE BLOCKSIZE
#
# One vectored O_DIRECT write that a memory-backed null_blk device loses
# blocks of when its block size is smaller than PAGE_SIZE (and larger than
# 512): four blocks at an offset of one block, as iovecs of 1.5 and 2.5
# blocks, read back and compared.  Use a NEW device each time: a block an
# earlier aligned write marked keeps its bitmap bit and hides the defect.
# Fixed by "null_blk: keep each memory-backed copy within one block".
#
#   mkdir /sys/kernel/config/nullb/t; cd /sys/kernel/config/nullb/t
#   echo 1 > memory_backed; echo 4096 > blocksize; echo 64 > size; echo 1 > power
#   null-blk-subpage-repro.py /dev/t 4096
import os, mmap, sys
dev, bs = sys.argv[1], int(sys.argv[2])
buf = mmap.mmap(-1, 4 * bs)                  # page-aligned, 4 blocks
buf[:] = bytes((i % 251) + 1 for i in range(4 * bs))   # no zero bytes
m = memoryview(buf)
a, b = m[:bs + bs // 2], m[bs + bs // 2:]     # 1.5 blocks, then 2.5 blocks
fd = os.open(dev, os.O_RDWR | os.O_DIRECT)
n = os.pwritev(fd, [a, b], bs)                # at offset one block
rd = mmap.mmap(-1, 4 * bs)
os.preadv(fd, [rd], bs)
bad = [i for i in range(4) if rd[i*bs:(i+1)*bs] != buf[i*bs:(i+1)*bs]]
zero = [i for i in bad if rd[i*bs:(i+1)*bs] == bytes(bs)]
print("wrote", n, "bytes; blocks that differ:", bad, "of which all zeroes:", zero)
