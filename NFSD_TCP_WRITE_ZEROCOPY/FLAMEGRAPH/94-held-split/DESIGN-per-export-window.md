# A window of loaned WRITEs per export: the design

**The ask (Mike, 11 November):** "how do we get NFSD to bound the number
of requests outstanding to a particular drive?" and, after the budget
sweep, "carry on with the design".  The budget to 64 waits until this
is designed.

## What it is for

Round 94 split the region's 50 MiB held: about 22 MiB is WRITEs at the
drives, each 1 MiB write 360 us in `nfsd_direct_write()`, 82% of it at
the drive itself, with about three writes outstanding at each drive
(`heldsplit.bt`, `writesplit.bt`).  Locally a drive is full at two
writes outstanding and every write past two adds about 100 us to each
and nothing to the rate (`qdprobe.sh`, `sglprobe.sh`, with the payload
laid out as the card delivers it).  The loan budget cannot shorten that
queue on its own: it charges a record from its first payload skb, so
the records in assembly (one a connection) draw on it as much as those
at the drives, and at 48 WRITEs it pauses both alike (`run490.sh`).

So the bound wanted is one that counts the WRITEs in flight for each
drive, lets a drive keep what fills it, and holds the rest back on the
client, where the pages are not the server's.

## Where the bound has to sit

**At admission, not at the write.**  A thread that waits for its
drive's slot in `nfsd_direct_write()` holds a received 1 MiB: the pages
move from the drive's row to the "published, waiting" row and the sum
does not change.  Pages leave the server only when nfsd stops reading
the connection and TCP closes the client's window.  The receive path
already has that point: at a record's first skb that carries region
pages (`net/sunrpc/svcsock.c`, the admission in the `->read_sock`
callback, about line 6117), a record the budget cannot admit is left
unread there (`take -= remaining; paused = true`), the transport waits
on the budget's FIFO (`sk_rx_wait`, oldest ticket first) and
`svc_tcp_rx_waiter_wake()` sets `XPT_DATA` and enqueues it when credit
returns.  A paused record holds nothing: its 28-byte prefix is copied,
no page is borrowed yet.  The window reuses that pause.

## What is known at admission: the handle is in hand

The prefix copied before classification is 7 XDR words: xid, call, RPC
version, program, version, procedure, auth flavor
(`svc_tcp_rx_classify()`), enough to say "an NFSv3 WRITE with AUTH_SYS".
The rest of the head follows in the same skb: the credential (AUTH_SYS:
stamp, machine name, uid, gid, gids; under 100 bytes from this client),
the verifier (8 bytes), then WRITE3args: the file handle (length, up to
64 bytes), offset, count, stable, the data length
(`nfs3svc_decode_writeargs()`).  All of it is within about 300 bytes of
the record's start, in the first frame; GRO puts a flow's next frames in
the same skb, so `skb_copy_bits(skb, body_offset, buf, 512)` into a
stack buffer has it in all but the rare record that begins in the last
few hundred bytes of a GRO batch's last frame.

**The handle names the drive.**  The rig exports each drive with
`fsid=N` (`xeu-rig/twoport.sh`), so the handle's `fh_fsid_type` is
`FSID_NUM` and `fh_fsid()[0]` is N: one value a drive.  Any export has a
fsid of some type in its handles (`fs/nfsd/nfsfh.h`), so the key is a
hash of (`fh_fsid_type`, the fsid words): no export lookup, no
reference, nothing that sleeps, which matters in a `->read_sock`
callback under the socket lock.  Two exports on one filesystem share a
key, which is right: they share the drive.

## The pieces

**nfsd: a procedure hook that names the record's target.**  Beside
`pc_xdr_bvec` on `struct svc_procedure`:

    u32 (*pc_rx_target)(const __be32 *head, unsigned int len);

sunrpc calls it with the record's first bytes (the copied prefix and
what `skb_copy_bits()` fetched) and gets a key, or 0 for "none": the
bytes do not reach the handle, or the procedure's layout is not one it
parses.  NFSv3 WRITE's parses the credential by flavor and length, the
verifier, the handle, and hashes its fsid.  NFSv4 COMPOUND's would walk
SEQUENCE and PUTFH to the handle for the WRITE compound the Linux
client sends (SEQUENCE, PUTFH, WRITE, GETATTR) and return 0 for any
other; the rig mounts `vers=3`, so it is not needed first.  sunrpc
never learns an NFS structure; nfsd never learns about admission.  This
is the same shape as `pc_xdr_bvec`, which is how the loan already
crosses the layer.

**sunrpc: targets, a window and a FIFO each.**  In the namespace's
budget (`struct svc_tcp_rx_budget`, which already carries the consumer):

    struct svc_tcp_rx_target {
    	u32			key;
    	atomic_t		inflight;	/* records admitted, not released */
    	struct list_head	waiters;	/* transports paused for it */
    };

a fixed table of 64 slots indexed by the key's hash (a collision shares
a window between two drives; with eight it is unlikely and harmless),
one spinlock for the table's lists, taken only to pause and to wake.
The window `W` is one value for all targets, a module parameter
`svc_tcp_rx_target_window` (0: off, the default): what fills a drive is
a property of the storage, so a per-export option (exportfs) is the
right home if the window is kept, after the measurement.

**The record remembers its target.**  Two fields in
`struct svc_tcp_rx_state`: `u32 target` (0: none) and a `bool
target_counted`, reset with the rest of the state.

**At admission**, before the budget is asked (so no credit has to be
undone):

1. `pc_rx_target()` on the bytes in hand; 0 admits the record uncounted
   (counted in a stat, `target_unkeyed`).
2. `inflight < W`: `inflight++`, `target_counted = true`, on to the
   budget as now.
3. Otherwise pause at this skb as a refused budget admission does, the
   transport on the target's FIFO with a ticket, a stat
   `target_paused`.  The transport is on one list at a time (it is
   paused at one record), so the budget's `sk_rx_wait` can be reused
   with its own `ops`, or a second waiter embedded beside it.

**At release** (`svc_tcp_release_ctxt()`, the end of the request, which
for a direct write is a few microseconds after `nfsd_direct_write()`
returns; `svc_tcp_rx_release_payload()` and `svc_tcp_rx_abort()` for the
other ways out): if `target_counted`, `inflight--`, and while
`inflight < W` and the FIFO is not empty, wake the oldest waiter
(`set_bit(XPT_DATA)`, `svc_xprt_enqueue()`), one for each free slot.  A
woken transport re-runs admission for the same record and may pause
again behind a newer arrival, keeping its ticket.

**Order against the budget.**  Target first, then budget: a record
paused on its target holds no credit, so nothing is undone; a record
admitted by its target and refused by the budget keeps its count (it
is admitted, in flight, only waiting for credit), which is what the
window means.  The budget's own oldest-first guarantee is untouched.

**Cost.**  One `skb_copy_bits()` of about 300 bytes and a parse of a
dozen words a record, one atomic increment and one decrement; no lock
unless a record pauses or a waiter wakes.  Against the 30-odd us a
record already spends in receive, under 1%.

## Sizing the window: what it would buy

The window counts a record from its admission to its release, so it
holds the assembly as well as the drive, and Little's law sizes it.
A drive takes 8,600 WRITEs a second at 9 GB/s.  From `heldsplit.bt`
(`t2-r487`): assembly holds about 0.157 ms a byte, so a record's
transfer is about 0.31 ms (its first byte waits the whole of it, its
last none), 2.6 records a drive in assembly; the drive holds 3.1
(0.36 ms).  In flight a drive: about 5.7.

- **W = 5** leaves about 2.4 at the drive: the pages there fall from
  about 1,390 to about 1,080, **5 to 6 MiB** of the 50 held, with the
  drive still above two.
- **W = 4** leaves about 1.4: under what fills a drive, so the rate
  falls (one outstanding writes 6.66 GB/s with this payload).  It is
  the run that shows the cliff.
- The window also smooths arrivals a drive, so the peak and the 99th
  percentile should fall as the budget sweep's did.

The assembly's share is set by the connections' speed, not by the
window: a window cannot shrink it without also idling the drive.  So
this is the whole of what a per-drive bound buys: the drive's queue,
from about three writes to about two, 5 to 6 MiB.

## Head-of-line blocking, the risk

The client spreads one file's WRITEs over all twelve of a port's
connections (nconnect's round robin), so every connection carries every
drive's records in turn.  A connection paused for a full drive D holds
back the records behind it on that connection for the other three
drives, until a D record releases, 100 to 300 us.  With twelve
connections a port, about three are paused at a time at W = 5 if a
quarter of the heads are for the full drive; the other nine carry on.
The budget sweep's 48 WRITEs (about six a drive) cost 2% with the
pause falling on whichever connection asked, so a per-drive FIFO
should cost no more; it is the measurement that says.  If it costs
rate, the cure is on the client, not the server: one connection set a
drive (each export mounted through its own server address), which also
makes round 56's lever (fewer connections a drive) available again
without starving the others.

## What a measurement needs

About 150 lines in sunrpc (the table, the admission check, the
release, the wake, three stats in the budget's debugfs line) and about
40 in nfsd (the NFSv3 WRITE hook), as two commits, code only; the KUnit
suites for the admission path would follow.  Then `run490.sh`'s shape
with `svc_tcp_rx_target_window` 0, 5 and 4 on the settled setting:
`heldsplit.bt` for the pages at the drive, the mesh for the bytes
through memory, the three stats, and the rate.  Keep it if W = 5 holds
71.5 GB/s with the drive's row down a fifth.

## Where that leaves the 32 MiB

With the drives at two outstanding (16 MiB), the rings as they are
(8 MiB), the sockets (1.5), the assembly (11) and the way back (6 to
7), the region would hold about 43 MiB, 1.3 times the L3: the bound is
worth its 5 to 6 MiB, and it is not the way to 32.  The next large
share is the assembly, which follows the connections times half a
record.  A smaller record halves it: `wsize=524288` would take it from
11 to about 5.5 MiB at twice the RPCs a second (nfsd's CPUs are 15%
busy), with the drive's queue in bytes unchanged.  That is one run with
no code, worth taking beside the window's.
