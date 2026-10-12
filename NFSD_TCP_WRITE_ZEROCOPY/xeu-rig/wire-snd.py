#!/usr/bin/python3
# wire-snd.py <dst ip> <port> <src ip> <streams> <seconds> - the sender for
# wire-rcv.py: <streams> connections, each sending zeroes as fast as it can.
import socket, sys, threading, time
dst, port, src, n, secs = sys.argv[1], int(sys.argv[2]), sys.argv[3], int(sys.argv[4]), float(sys.argv[5])
buf = bytes(1 << 20)
def tx():
    s = socket.create_connection((dst, port), source_address=(src, 0))
    end = time.time() + secs
    while time.time() < end: s.sendall(buf)
    s.close()
ts = [threading.Thread(target=tx) for _ in range(n)]
[t.start() for t in ts]; [t.join() for t in ts]
