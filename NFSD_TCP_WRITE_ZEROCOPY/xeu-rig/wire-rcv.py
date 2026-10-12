#!/usr/bin/python3
# wire-rcv.py <ip> <port> <streams> <copy|trunc> - receive <streams> TCP
# connections on <ip> until they close.  copy reads into a buffer, as a
# server that copies does; trunc has the kernel discard the data without
# copying it (MSG_TRUNC), which leaves the card, the rings and TCP.
import socket, sys, threading, time
ip, port, n, mode = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
ls = socket.socket(); ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
ls.bind((ip, port)); ls.listen(256)
tot = [0] * n
def rx(i, c):
    b = bytearray(1 << 20)
    while True:
        r = c.recv_into(b) if mode == 'copy' else len(c.recv(1 << 20, socket.MSG_TRUNC))
        if not r: break
        tot[i] += r
cs = [ls.accept()[0] for i in range(n)]
t0 = time.time()
ts = [threading.Thread(target=rx, args=(i, c)) for i, c in enumerate(cs)]
[t.start() for t in ts]; [t.join() for t in ts]
dt = time.time() - t0
print("%s %s %d streams: %.2f GB/s over %.1f s" % (ip, mode, n, sum(tot) / dt / 1e9, dt))
