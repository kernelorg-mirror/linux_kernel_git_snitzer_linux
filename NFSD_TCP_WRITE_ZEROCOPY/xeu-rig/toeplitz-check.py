#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# toeplitz-check.py KEY FLOWS -- find the Toeplitz input that gives the
# hashes the E1 put on received packets.
#
# KEY:   the key as `ethtool -x <port>` prints it (colon-separated hex), or
#        a file holding that line.
# FLOWS: a file of lines "SRC_IP DST_IP SRC_PORT DST_PORT HASH", one line per
#        flow, SRC the sender (the client); the hash in decimal as xeu-rx.bt's
#        @flow prints it, or in hex with a 0x prefix; '#' starts a comment.
#
# Xsight (Don Wallwork, October 2026): for IPv4 TCP the hash is computed over
# src addr (32), dst addr (32), src port (16), dst port (16), protocol (8), in
# that order.  Not said: which bytes of the 68-byte key, nor the byte order.
# This tries the 13-byte input and the standard 12-byte one, each with the key
# taken from every starting byte, the fields in network order and with each
# field byte-swapped, and the hash as computed and byte-swapped; it prints
# every combination that matches every flow.
#
# `toeplitz-check.py --selftest` checks the hash itself against the
# Microsoft RSS verification vector (IPv4 TCP, the standard 40-byte key).
import socket
import struct
import sys

MS_KEY = bytes.fromhex(
    "6d5a56da255b0ec24167253d43a38fb0d0ca2bcbae7b30b477cb2da38030f20c"
    "6a42b73bbeac01fa")


def toeplitz(key, data):
    """The Toeplitz hash of data under key (key at least len(data) + 4 bytes)."""
    k = int.from_bytes(key, "big")
    kbits = len(key) * 8
    h = 0
    for i, byte in enumerate(data):
        for b in range(8):
            if byte & (0x80 >> b):
                shift = kbits - 32 - (i * 8 + b)
                h ^= (k >> shift) & 0xffffffff
    return h


def fields(src, dst, sport, dport, swap):
    s, d = socket.inet_aton(src), socket.inet_aton(dst)
    sp, dp = struct.pack(">H", sport), struct.pack(">H", dport)
    if swap:
        s, d, sp, dp = s[::-1], d[::-1], sp[::-1], dp[::-1]
    return s + d + sp + dp


def bswap32(x):
    return struct.unpack("<I", struct.pack(">I", x))[0]


def selftest():
    # 66.9.149.187:2794 -> 161.142.100.80:1766, hash 0x51ccc178
    got = toeplitz(MS_KEY, fields("66.9.149.187", "161.142.100.80",
                                  2794, 1766, False))
    ok = got == 0x51ccc178
    print("selftest: %08x %s" % (got, "ok" if ok else "FAIL (want 51ccc178)"))
    return 0 if ok else 1


def read_key(arg):
    try:
        with open(arg) as f:
            arg = f.read()
    except OSError:
        pass
    hexs = "".join(c for c in arg if c in "0123456789abcdefABCDEF:")
    return bytes(int(x, 16) for x in hexs.split(":") if x)


def main():
    if sys.argv[1:] == ["--selftest"]:
        return selftest()
    if len(sys.argv) != 3:
        print("usage: toeplitz-check.py KEY FLOWS | --selftest")
        return 2
    key = read_key(sys.argv[1])
    flows = []
    with open(sys.argv[2]) as f:
        for line in f:
            line = line.split("#")[0].split()
            if len(line) == 5:
                flows.append((line[0], line[1], int(line[2]), int(line[3]),
                              int(line[4], 0)))
    if not flows:
        print("no flows read")
        return 2
    print("key %d bytes, %d flows" % (len(key), len(flows)))
    found = 0
    for proto in (True, False):
        for swap in (False, True):
            for off in range(len(key)):
                k = key[off:]
                need = 13 if proto else 12
                if len(k) < need + 4:
                    break
                for hswap in (False, True):
                    if all((lambda h: bswap32(h) if hswap else h)(
                            toeplitz(k, fields(s, d, sp, dp, swap) +
                                     (b"\x06" if proto else b"")))
                           == want for s, d, sp, dp, want in flows):
                        found += 1
                        print("MATCH: %s input, fields %s, key from byte %d,"
                              " hash %s" % (
                                  "13-byte (with protocol)" if proto
                                  else "12-byte (standard)",
                                  "byte-swapped" if swap else "network order",
                                  off, "byte-swapped" if hswap else "as is"))
    if not found:
        print("no combination matches every flow")
    return 0 if found else 1


if __name__ == "__main__":
    sys.exit(main())
