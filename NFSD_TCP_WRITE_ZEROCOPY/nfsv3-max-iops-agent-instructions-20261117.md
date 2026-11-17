# Maximum NFSv3 4K random-read IOPS, one client to one server: instructions for an agent

These instructions reproduce the setup that measured **3,594,376 IOPS of 4 KiB random reads over a single NFSv3 mount**, from one Linux client to one Linux kNFSd server. Typical repeated runs gave 3.2–3.4M. The steps are written for an engineering agent: do them in order, check each one before going on, and keep a log of every value you set and every result.

## 0. What you need

- **The patch stack** `hammerspace-kernel-overlay-stack-20261116.tar.gz`. It is 43 patches on Hammerspace `engineering/linux`, branch `kernel-7.3/canary`, commit `2e3a1b18333ef5e31ae270ee5711596da0246cd2`.
- **Kernels:** the same patched kernel goes on both the client and the server. Our best result had both hosts on this kernel.
- **Hardware we used.** Any pair with these properties should work:

  | Role | Instance | Arch | vCPU | NIC | Storage |
  |---|---|---|---|---|---|
  | Server | AWS i8ge.48xlarge | aarch64 | 192 | 300 Gbit nominal | 16 × 7.5 TB local NVMe |
  | Client | AWS c8gn.48xlarge | aarch64 | 192 | 600 Gbit nominal | — |

  Same availability zone. Rocky Linux 10 userland on both hosts.
- **Size the network first.** 4 KiB reads need about IOPS × 4 KiB × 8 × 1.1 bits/s on the wire: about 105 Gbit/s for 3M IOPS and 140 Gbit/s for 4M. Measured throughput is what counts, not the nominal figure. Our "300 Gbit" pair measured **about 137 Gbit/s** (16 iperf3 streams, MTU 9001), which caps NFS 4K reads near **4.0M IOPS**.

## 1. Build and install the kernel (both hosts)

```sh
git clone <engineering/linux> linux && cd linux
git checkout -b nfs-iops 2e3a1b18333ef5e31ae270ee5711596da0246cd2
tar xzf hammerspace-kernel-overlay-stack-20261116.tar.gz
git am hammerspace-kernel-overlay-stack-20261116/patches/*.patch
git rev-parse HEAD^{tree}    # must print bdd84d63f85314e47876a79a8bf70b36c5d9d1b8
```

Build it with your usual config. Keep `CONFIG_DEBUG_FS=y`, because several switches below are in debugfs. Install it, reboot, and record `uname -r` on both hosts.

The tarball README flags a few patches that only add hooks for our own stack: 01, 04, 05 and 06 (an x86 stack export, a FUSE opcode, the Inline NVMe host command set and its target). They are not needed for NFS IOPS. You may leave them out, but then the tree check above no longer applies.

### Patches that matter for NFSv3 IOPS

| # | Side | What it does |
|---|---|---|
| 07 | server | Takes nfsd READ and WRITE off the page-cache LRU by default: `io_cache_read` and `io_cache_write` default to direct. |
| 11 | server | Counts duplicate-request-cache entries per CPU instead of in one shared counter. |
| 16, 17 | server | Gives each CPU its own run-to-completion request pool, instead of one shared free list. On its own this took reads from about 1.2–1.6M to 2.84–2.92M. Switch: debugfs `nfsd/rtc_pcpu_pool`, default Y. |
| 18, 19 | server | Lets each nfsd thread borrow the export instead of taking a shared reference per request. |
| 22, 23 | server | Places each run-to-completion connection on the least-loaded CPU that does not take network receives. Switch: module parameter `sunrpc.svc_rtc_place_off_rx`, default Y. With it off, writes fall about 10×. |
| 27 | client | **The key client patch.** Splits the per-mount RPC client lock and task list into 16 per-CPU-node shards. Without it, one mount at `nconnect=128` stops near 575K IOPS, while 16 mounts reach 2.9M. |
| 28 | client | Adds transports to an NFSv3 mount that was created without `nconnect=`. It does not affect mounts that set `nconnect`. |
| 30 | server | Sizes the svcsock receive-region share by connection count. |
| 29 | test | KUnit coverage for 11. |

Patch 31 and the NFSv4 patches don't affect NFSv3.

## 2. Server setup

### 2.1 Storage

Use XFS on local NVMe, mounted with `-o noatime`. Either layout works:

- **What we used:** all drives striped with dm-stripe (512 KiB chunk), one XFS, one export.
- **Alternative:** one XFS and one export per drive, with the client mounting each. This avoids the stripe layer.

Before you test NFS, measure the local ceiling with fio on the server itself: `io_uring`, `direct=1`, 4k randread, high queue depth, IRQ completion. **Do not** set `nvme.poll_queues`. Polling burned about 65% CPU for fewer IOPS than IRQ in our tests. If the local ceiling is not well above your NFS target, the disks are the limit.

Create the test files before you start, one per fio job (see 4.3). Make the total working set much larger than server RAM, or every run reads from cache.

### 2.2 nfsd

```sh
mkdir -p /etc/nfs.conf.d
printf '[nfsd]\nthreads=768\nvers3=y\n' > /etc/nfs.conf.d/iops.conf
echo 1048576 > /proc/fs/nfsd/max_block_size     # before nfsd starts
systemctl start nfs-server
echo 768 > /proc/fs/nfsd/threads
```

`/etc/exports`:

```
/export  <client-ip>(rw,async,no_root_squash,no_subtree_check,fsid=1)
```

Then run `exportfs -ra`.

### 2.3 Runtime switches (check every one, and log it)

```sh
D=/sys/kernel/debug/nfsd
echo 2 > $D/io_cache_read          # 2 = direct (patch 07 makes this the default; set it anyway)
echo 2 > $D/io_cache_write
cat $D/rtc_pcpu_pool               # expect Y (it takes effect at the next nfsd start)
cat /sys/module/sunrpc/parameters/svc_rtc_place_off_rx   # expect Y
cat /sys/module/sunrpc/parameters/svc_tcp_rx_region_reads 2>/dev/null   # leave at N (the default)
```

### 2.4 Server sysctls

```sh
sysctl -w net.core.rmem_max=268435456 net.core.wmem_max=268435456
sysctl -w net.ipv4.tcp_rmem="4096 1048576 268435456" net.ipv4.tcp_wmem="4096 1048576 268435456"
```

## 3. Network (both hosts)

- MTU 9001 (jumbo frames) end to end. Check it with `ss -ti`: look for `pmtu:9001` on an NFS connection.
- Leave ENA and IRQ settings at their defaults. Our best runs used no IRQ pinning, no RPS/XPS and no busy polling.
- Measure the wire: run iperf3 with 16 parallel streams in each direction, and record Gbit/s. Your IOPS ceiling is about measured Gbit/s × 1e9 / (8 × ~4,230 bytes per 4K READ reply).

## 4. Client setup

### 4.1 Mount

Use exactly **one** mount:

```sh
mount -t nfs -o vers=3,proto=tcp,nconnect=128,rsize=1048576,wsize=1048576,nosharecache \
      <server-ip>:/export /mnt/nfs
```

`nconnect=128` is the main lever: it gives one mount 128 TCP connections. Without patch 27 on the client, raising it beyond a few dozen gains little, because the per-mount lock serialises every RPC.

No other client tuning was needed: no slot-table sysctls, no busy polling, default IRQ placement. If io_uring is restricted on the client, run `sysctl -w kernel.io_uring_disabled=0`.

### 4.2 Optional checks

- `cat /proc/fs/nfsfs/servers` and `cat /proc/self/mountstats`: confirm 128 transports on the mount.
- `nstat` before and after a run: watch for TCP retransmits.

### 4.3 Load (fio)

One job per client CPU, each on its own file, spread across a few directories:

```ini
[global]
ioengine=io_uring
direct=1
rw=randread
bs=4k
iodepth=32
numjobs=1
time_based=1
runtime=240
ramp_time=10
group_reporting=1

; repeat one [jN] section per client CPU (192 on c8gn.48xlarge), e.g. generated by a script:
[j0]
filename=/mnt/nfs/d00/f0
[j1]
filename=/mnt/nfs/d01/f1
; ...
```

This gives 192 jobs × QD32 = 6,144 requests in flight. Raising QD further mostly adds latency.

## 5. Running and reporting

- Run each configuration **at least 3 times, ideally 5**. Report every run, the median and the minimum, not just the best. Runs vary by a few percent from one instance pair to another.
- Record CPU% on both hosts during the run (`mpstat 5` or `sar`). Around 3M IOPS, the limit is usually the client RPC layer or the wire, not the disks.
- With every result, record: both `uname -r`, the patch list applied, every switch value from 2.3, the mount line, the nfsd thread count, MTU, measured iperf3 Gbit/s, and the fio job file.
- Report IOPS as a fraction of the measured wire ceiling as well as the absolute number.

## 6. What to expect, and caveats

| Configuration | 4K randread IOPS, one NFSv3 mount |
|---|---|
| This stack plus one NFSv4.1 slot-table patch not in the tarball (no effect on v3) | **3.59M** best, 3.44M on another lease |
| The stack without that patch | ~3.17M mean (min 3.05M) |
| Stock kernel without patch 27 on the client | ~575K, limited by the per-mount RPC lock |
| Wire ceiling on our pair (137 Gbit/s measured) | ~4.0M |

- We did not A/B each patch on its own. The table gives the configurations we measured, not each patch's share.
- The 3.17M and 3.44–3.59M figures came from different instance pairs, so part of that gap may be variance between instances.
- Write and mixed workloads respond to the same settings but were not the focus here.
- To go beyond about 4M from one client, you need more measured network bandwidth, not more NFS tuning.
