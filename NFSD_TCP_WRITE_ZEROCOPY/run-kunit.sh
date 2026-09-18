#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# KUnit re-run on the updated 4K/159 kernel (per NFSD_TCP_WRITE_ZEROCOPY/TESTING.md)
set -x
dmesg -C

# kunit must be loaded with enable=1 (param has 0 sysfs perms but is real)
modprobe -r kunit 2>/dev/null
modprobe kunit enable=1

modprobe xdr_kunit;      sleep 1
modprobe svcsock_kunit;  sleep 1
modprobe nfsd_bvec_kunit; sleep 1

# nfsd4_bvec_kunit needs a live nn->nfsd_serv (v4-only hand-start, no rpcbind)
mountpoint -q /proc/fs/nfsd || mount -t nfsd nfsd /proc/fs/nfsd
echo "-2 -3 +4 +4.1 +4.2" > /proc/fs/nfsd/versions
grep -q 2049 /proc/fs/nfsd/portlist 2>/dev/null || echo "tcp 2049" > /proc/fs/nfsd/portlist
echo 4 > /proc/fs/nfsd/threads
sleep 1
modprobe nfsd4_bvec_kunit; sleep 2
echo 0 > /proc/fs/nfsd/threads

set +x
echo "=================== KTAP ==================="
dmesg | grep -E 'KTAP|ok [0-9]|not ok|# (Totals|sunrpc|nfsd)'
echo "=================== summary ==================="
dmesg | grep -E '# Totals|not ok'
