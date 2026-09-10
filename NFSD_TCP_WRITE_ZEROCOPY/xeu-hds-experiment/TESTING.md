# Test plan — xeu paired RX posting on tardis1 (E1)

Written to be executable by a future session (human or Claude) with
access to tardis1. Read `README.md` in this directory first for what the
patch does; this file is the procedure.

**What this answers.** One question, narrowly: *can software make the E1
land NFS WRITE payload at page offset 0?* Everything else — throughput,
direct I/O, the firmware ask — depends on that answer and is explicitly
**not** what a pass here claims. See "Interpreting the result".

**Status: never run.** The patch builds warning-free and applies cleanly,
but no E1 was available. Every expectation below is a prediction.

---

## 0. Before touching anything: the access hazard

`xeu` drives both 400G ports. Reloading it drops them, and if your
session rides one you lose the machine mid-run.

```sh
ip route get 1.1.1.1                    # which device carries your traffic?
ip -br link show | grep -iE 'xeu|ens|eth'
```

Proceed only with **one** of:

- an out-of-band path (BMC / serial console), or
- SSH confirmed arriving on a non-xeu management interface.

Either way, stage a deadman switch before every load, so a bad module
self-heals:

```sh
cp /path/to/unmodified/xeu.ko /root/xeu-known-good.ko
( sleep 600; rmmod xeu; insmod /root/xeu-known-good.ko; ip link set <dev> up ) &
echo $! > /tmp/deadman.pid      # kill it once the run is confirmed good
```

## 1. Gates — check these first, one may block the run

### 1a. Page size and MTU

The patch refuses paired posting unless a whole frame fits in one
header + one payload buffer. The payload buffer is one page minus
`skb_shared_info`, so the ceiling scales with `PAGE_SIZE`:

```sh
getconf PAGESIZE            # 4096 or 65536
ip link show <xeu-dev> | grep -o 'mtu [0-9]*'
```

| PAGE_SIZE | Max frame a pair holds | MTU 1500 | MTU 9000 |
|---|---|---|---|
| 4096 | ~3.8 KB | works | **refused** |
| 65536 | ~65 KB | works | works |

The hs.165 delivery ships **both** kmod flavors (`kmod-xsight` for 4K,
`kmod-64k-xsight` for 64K), so booting the 64K kernel is a supported way
out of the jumbo-MTU constraint rather than a rebuild.

If it is a 4K-page kernel at MTU 9000 — the likely 400G configuration —
you must either drop MTU to 1500 for the experiment (fine for the
geometry question; it does change the throughput baseline, so do not
compare throughput across the MTU change) or extend the patch to
multi-page payload buffers. The driver logs its refusal with the
numbers, so a mistake here is loud, not silent.

### 1b. Exact header length

`hds_hdr_len` must equal the stream's real L2+L3+L4 size: that is the one
value where both possible engine semantics coincide (see README). Measure
it on the actual NFS traffic — do not assume:

```sh
tcpdump -i <xeu-dev> -c1 -v 'tcp port 2049'
```

54 = Ethernet(14) + IPv4(20) + TCP(20). 66 with TCP timestamps. Add 4 per
VLAN tag. If client and server negotiate options mid-stream the size can
vary — if so, this experiment cannot be exact and that is itself a
finding worth recording.

### 1c. Loan preconditions

```sh
cat /sys/module/sunrpc/parameters/svc_tcp_rx_loan_pages   # want Y
cat /sys/kernel/debug/nfsd/io_cache_write                 # want 2
mount | grep nfs                                          # plaintext TCP, sec=sys, no xprtsec
command -v gawk bpftrace perf                             # zcstat needs gawk
```

Loans happen only for plaintext TCP with AUTH_SYS/AUTH_NULL. A krb5 or
TLS mount copies by design and the run measures nothing.

## 2. Build the driver for the running kernel

Out-of-tree modules bind to a kernel release and vermagic, so build
against whatever tardis1 actually boots (e.g. a `-3`-based `hs.165`, or a
`-5`-based integration kernel). The patch itself is kernel-version
agnostic — it touches only xeu files and uses no API newer than the 2.0.1
baseline.

```sh
uname -r
tar xf kmod-xsight-2.0.1.tar.gz && cd kmod-xsight-2.0.1
patch -p0 < 0001-xeu-EXPERIMENTAL-paired-RX-posting-to-land-payload-p.patch
cd xeu-esdk-v2.0.1
make KERNELDIR=/lib/modules/$(uname -r)/build
modinfo -F vermagic xeu.ko      # must match the running kernel
modinfo -p xeu.ko | grep hds    # both params present
```

If the delivery is an RPM tarball instead, install it and confirm
`modinfo -p xeu | grep hds` on the installed module.

## 3. The three passes

Keep offered load **identical** across passes — the loan's win is CPU,
not throughput, so anything that changes load invalidates the comparison.
Start with a single connection (`nconnect=1`) to reduce variables, then
scale once the geometry is understood.

Run `zcstat.sh 5` (from `..`) throughout each pass and save its output.
See `../zcstat.sh-usage.md` for column semantics.

### Pass A — baseline, paired posting off

```sh
rmmod xeu; insmod ./xeu.ko                # hds_hdr_len defaults to 0
ip link set <dev> up && sleep 5           # re-establish the data path
# ... drive steady-state NFS write load from the client ...
```

This documents the problem on your hardware and is valuable evidence
even if Pass B fails. Expect:

- `avbv` in the **hundreds** (not ~257/MiB) — per-frame fragments;
- `dirW`/`dirMB` at or near **0** — every write demoted;
- either large `cpdKB` or `reason=capacity`, i.e. the receive copied
  anyway because the fragment count exhausted the bvec budget;
- `pp%` near **100** — xeu RX is page_pool-backed, unlike loopback.

Also capture, for the firmware conversation:

```sh
perf record -a -g -- sleep 10 && perf report --stdio | head -40   # memmove share
ethtool -S <dev> | grep -iE 'csum|drop|error' > /tmp/A-ethtool.txt
```

### Pass B — paired posting on

```sh
rmmod xeu; insmod ./xeu.ko hds_hdr_len=54      # the value from step 1b
ip link set <dev> up && sleep 5
dmesg | tail -20                               # read the decision + geometry lines
```

The one-shot per-ring line **is the result**:

```
xeu: paired RX posting: ring 0 frame_len=1514 num_desc=2 posted_hdr=54 parsed_hdr(L0-L4)=54 linear=54 payload_frag_off=0
```

| Observation | Meaning | Next |
|---|---|---|
| `num_desc=2`, `payload_frag_off=0` | **Success.** Placement is reachable in software. | Continue to step 4. |
| `num_desc=1` | The engine treated the header buffer as end-of-frame and truncated. | Go to Pass C. |
| `parsed_hdr` ≠ `posted_hdr` consistently | Your header size is wrong, or the engine split at the parse boundary. | Reload with `hds_hdr_len=<parsed_hdr>`. |
| No such line, but "paired RX posting active" appeared | No frames have arrived on that ring yet. | Generate traffic. |
| "paired RX posting disabled" + reason | A gate from step 1 failed. | Fix the MTU/length and retry. |

**Then immediately validate data integrity** — a wrong split assumption
corrupts skbs rather than merely misplacing them:

```sh
ping -c5 <peer>                                   # basic liveness
ethtool -S <dev> | grep -iE 'csum|drop|error'     # must not climb vs /tmp/A-ethtool.txt
```

and byte-verify real NFS data, because this project has twice had
tracepoint-clean runs that were silently corrupting:

```sh
dd if=/dev/urandom of=/tmp/src bs=1M count=64
dd if=/tmp/src of=/mnt/<nfs>/probe bs=1M oflag=direct
cmp /tmp/src /mnt/<nfs>/probe && echo "DATA OK"
```

If liveness or `cmp` fails: `rmmod xeu; insmod /root/xeu-known-good.ko`,
record the failure mode, and treat the EOF semantics as refuted.

### Pass C — only if Pass B truncated

```sh
rmmod xeu; insmod ./xeu.ko hds_hdr_len=54 hds_hdr_eof=Y
```

Re-run the Pass B observations. If both EOF readings truncate, the engine
does not honour a short mid-frame buffer and the software-only route is
closed — which is a clean, publishable negative result and redirects the
entire ask to firmware.

## 4. Interpreting the result

**Success here is narrow and must be reported as such.** Payload at page
offset 0 proves the *placement* half of header-data split is reachable
without firmware. It will **not** restore direct I/O, and `zcstat` should
still show `dirW`/`dirMB` at 0, because payload still lands one frame per
page: every inter-frame joint sits at a payload position that is not a
multiple of the logical block size (1448 is not), and NFSD's admission
gate correctly demotes the write. Do not report "the experiment failed"
on that basis — it is the predicted outcome.

What converts this into throughput is **payload coalescing across
frames**: many frames' payload packed contiguously into one page. The
E1's packet processor already maintains per-flow GRO aggregation state
(1024 contexts, `GRO_ADD_TO_AGG`) that this driver never reads — that is
the specific capability to ask Xsight about, and a Pass B success is the
evidence that makes the ask concrete rather than speculative.

## 5. What to record

Regardless of outcome, capture these so the result is reusable:

- `uname -r`, `getconf PAGESIZE`, MTU, `modinfo -F vermagic xeu.ko`
- the measured header length and the `tcpdump` line it came from
- full `dmesg | grep -i 'paired RX posting'`
- `zcstat.sh` output for each pass (identical load, note the load command)
- `perf report` head for each pass (the `memmove` share is the real metric)
- `ethtool -S` before/after, and the `cmp` result
- for Pass A, the `avbv`/`capacity` evidence — this is what the firmware
  request is built on

Then update this file with what actually happened, `../PROJECT.md` if the
hardware behaved differently than its capability section predicts, and
the project artifact if the conclusion changes.

## Recovery cheat-sheet

```sh
rmmod xeu; insmod /root/xeu-known-good.ko; ip link set <dev> up   # revert driver
echo N > /sys/module/sunrpc/parameters/svc_tcp_rx_loan_pages      # disable loans (no reload)
echo 0 > /sys/kernel/debug/nfsd/io_cache_write                    # disable direct writes
```

The loan kill-switch and `io_cache_write` need no module reload, so they
are the safe first response to anything unexpected in the NFS path; only
a driver problem needs the reload.
