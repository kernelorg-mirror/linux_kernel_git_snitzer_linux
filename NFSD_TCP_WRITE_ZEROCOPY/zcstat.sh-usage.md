# zcstat.sh — usage guide

Periodic, vmstat-style accounting for the NFSD TCP write-path page loan.
One row per interval tells you whether the loan engaged, what payload
geometry the NIC delivered, whether the direct-I/O path was taken, and
what it cost in nfsd CPU.

Self-contained: tracefs plus `gawk`, in a private tracing instance. It
does not disturb global tracing state, needs no rig, and works on any
host running a page-loan kernel — the tracepoints it reads are identical
on the `-3` and `-5` branches of this series.

```
zcstat.sh [interval_seconds] [count]
```

`interval_seconds` defaults to 5; `count` defaults to unlimited. Stop an
unlimited run with **Ctrl-C** (it tears down its tracing instance and
temporary files on the way out).

Environment:

| Variable | Default | Effect |
|---|---|---|
| `BUFKB` | `8192` | Per-CPU trace buffer, KB. Raise if `drop` is non-zero. |
| `NOFILTER` | `0` | `1` disables the in-kernel event filter (see *Event filtering*). |

Must run as root (tracefs, and `/sys/kernel/debug/nfsd`).

## Reading a row

```
sw iocw |  pubrx   cprx    brwMB   cpdKB  pp%   mat |  lgrx  avbv  mxbv |   dirW    dirMB   vecW    vecMB |  nfsdC  drop  fallback-reasons
 Y    2 |     61      1     32.0     4.0    0     0 |    32   257   257 |     32     32.0      0      0.0 |   0.01     0  locked-head=1,auth=1
```

**Live configuration**

| Column | Meaning |
|---|---|
| `sw` | `sunrpc.svc_tcp_rx_loan_pages` — `Y` loans are enabled, `N` every receive copies. Read fresh each interval, so a mid-run flip is visible in place. |
| `iocw` | `/sys/kernel/debug/nfsd/io_cache_write` — `0` buffered, `2` direct. The loan's second half is inert unless this is ≥ 2. |

**Did the loan engage?** (one `action=publish` event per completed receive)

| Column | Meaning |
|---|---|
| `pubrx` | Receives published with **loaned** pages. |
| `cprx` | Receives published in **copy** mode — the pre-loan path. |
| `brwMB` | Payload MB borrowed (not copied) in the interval. |
| `cpdKB` | Payload KB copied. Small values are normal: the transport's designed locked-head merge costs one page per event, and the AUTH_NONE ping on every fresh connection is a copy. |
| `pp%` | Share of borrowed bytes living in **page_pool** pages. ~100 on a NIC with page_pool RX (xeu does); **0 on loopback**, where skbs are not page_pool-backed. A real NIC showing 0 here means the loan is holding pages of a kind the lifetime analysis did not assume — worth investigating. |
| `mat` | Materialized bytes: loaned pages the transport had to copy out after the fact. Expect 0. |

**What geometry arrived?** (published receives of ≥ 512 KB only)

| Column | Meaning |
|---|---|
| `lgrx` | Count of such large receives. |
| `avbv` | Mean bvecs per large receive. **The NIC-geometry tell**: ~257 per MiB is page-tiled 4K payload (what the loan wants); many hundreds means one partial-page fragment per frame, i.e. a NIC without header-data split. |
| `mxbv` | Largest bvec count seen since start (not an interval delta). |

**Did direct I/O happen?**

| Column | Meaning |
|---|---|
| `dirW` / `dirMB` | `nfsd_write_direct` segment count and MB. |
| `vecW` / `vecMB` | `nfsd_write_vector` (buffered) segment count and MB. |

Bytes matter more than counts: a few large direct segments beat many
small ones, and the split by volume is what maps to DRAM traffic.

**Cost and trust**

| Column | Meaning |
|---|---|
| `nfsdC` | CPU consumed by all `nfsd` kernel threads, in **cores-equivalent** — `1.00` is one core saturated, `12.40` is twelve. Computed over actual elapsed time, not the nominal interval. This is the number the receive loan improves. |
| `drop` | Trace events the kernel discarded this interval (per-CPU ring overruns). **Must be 0.** Anything else means events were lost and every count in the row is an undercount — raise `BUFKB`. |
| `fallback-reasons` | Named copy reasons for the interval, e.g. `auth=1,locked-head=1,disabled=34,capacity=12`. Also carries two diagnostics: `[EVENTS LOST - counts are low]` and `[no publish events: rerun with NOFILTER=1]`. |

## Fallback reasons you will actually see

| Reason | Means |
|---|---|
| `auth` | First RPC on a fresh connection is an AUTH_NONE ping — always copies. Normal. |
| `locked-head` | Bytes overlapping a locked skb linear head; the transport merges them into one whole page. Designed behavior. |
| `disabled` | The kill-switch is `N`. |
| `capacity` | The payload needed more bvecs than the receive state can hold. **On a no-HDS NIC this is the expected failure mode** — hundreds of per-frame fragments exhaust the budget and the receive copies instead. |
| `tls`, `gss`, `reply`, `unreadable` | Structurally ineligible receives. |

## The kill-switch A/B — the cleanest experiment

The loan's benefit is **CPU, not throughput**. At a wire- or
storage-bound operating point throughput will not move, and only `nfsdC`
(or a `perf` profile) shows the win. Compare at identical offered load,
on one kernel, with no reboot:

```sh
zcstat.sh 5 &                 # keep it running across the flip
# ... drive steady-state write load ...
echo N > /sys/module/sunrpc/parameters/svc_tcp_rx_loan_pages   # loans off
# ... same load, same duration ...
echo Y > /sys/module/sunrpc/parameters/svc_tcp_rx_loan_pages   # loans on
```

Expect `sw` to follow the flip, `pubrx`→`cprx` with `reason=disabled`
while off, and `brwMB`→`cpdKB` to swap. Any difference in `nfsdC` at
equal load is the loan's contribution.

For the copy elimination itself, run `perf` alongside — `memmove` share
is the direct measure:

```sh
perf top -e cycles --sort symbol          # or: perf record -a -g -- sleep 10
```

## Event filtering

`zcstat.sh` consumes only `action=publish`, but the lifetime tracepoint
fires six actions — three per receive — so by default it installs an
in-kernel filter (`action != 0 && action != 4`, dropping `CLASSIFY` and
`RELEASE`) and cuts the text it must parse by about two thirds. On a fast
server that reduces both parsing cost and the chance of losing events.

The filter is numeric because tracefs offers no way to resolve an enum
symbolically from userspace. `SVC_TCP_RX_CLASSIFY == 0` and
`SVC_TCP_RX_RELEASE == 4` hold on both the `-3` and `-5` branches
(verified by diffing `enum svc_tcp_rx_action`), but a future kernel that
reorders the enum would silently keep the wrong events. Two safeguards:
the filter is best-effort (a write failure just continues unfiltered),
and any interval that sees lifetime events but parses **no** publish
among them prints `[no publish events: rerun with NOFILTER=1]` — which is
exactly what a stale numeric mapping looks like.

## What zcstat cannot tell you

**Payload alignment.** Neither tracepoint carries a bvec offset:
`svcsock_tcp_rx_lifetime` reports counts and byte classes,
`nfsd_write_direct` carries only `xid`, `fh_hash`, `offset`, `len`. So
`avbv` tells you the *number* of fragments but not where they start. For
offsets use the xeu driver's own one-shot geometry log (see
`xeu-hds-experiment/`), or a bpftrace probe on
`nfsd_write_dio_iters_init()` in the style of `split_einval.bt`.

**Whether the data is correct.** This project has twice had
tracepoint-clean runs that were silently corrupting data. Byte-compare
something (`run-rig-cmp.sh`, `run-system-correctness.sh`, or a plain
`cmp`) before trusting any conclusion drawn from counters alone.

## Gotchas

- **LOCALIO voids a same-host test.** A loopback mount negotiates LOCALIO
  and bypasses the SUNRPC TCP receive entirely, so the whole loan path is
  never entered. Disable it on the client first:
  `echo N > /sys/module/nfs/parameters/localio_enabled` (global `nfs.ko`
  knob — restore it afterward). A `localio` count of anything but 0 in
  the rig runners means the same mistake.
- Loans require **plaintext TCP, AUTH_SYS/AUTH_NULL, no TLS**. A krb5 or
  `xprtsec=tls` mount copies by design and reasons will say so.
- `mxbv` is a running maximum since start, not an interval value — it
  stays put when traffic stops.
- Rows are emitted on a `interval + ~0.3s` cadence (a `trace_marker` tick
  flushes the reader so a burst that goes quiet is not held back). Rates
  divide by actual elapsed time, so this does not bias `nfsdC`.
