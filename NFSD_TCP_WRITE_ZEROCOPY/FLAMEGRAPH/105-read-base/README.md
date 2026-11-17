# Round 105: the READ base, profiled

**The method (Mike, 17 November: "its like you forgot the entire test
methodology used to optimize WRITEs.  We need that same approach applied
to READs"; "you cannot just keep testing 1 thing and expect it to unlock
all the gains"; "if balanced placement is ideal, then do it.. and find
out why it isn't performing with NFS").**  As for WRITEs: each round
profiles the READ strobe on the current base, ranks its costs a READ on
the work items' CPUs (8-55) and the ring CPUs, with the memory bytes a
byte sent beside them, changes the most costly thing, A/Bs it in turns,
and keeps it on a saving.  The base carries what should be right, and a
round finds why it does not perform yet, rather than dropping it.

**What the hardware leaves (round 104):** the drives read 104.58 GB/s
locally, the two ports send 98.31 (iperf3 `-Z`), memory carries about 140
GB/s with both directions mixed; NFS READs at the settled setting move
73 GB/s at 116.5 GB/s of memory.  iperf3 and NFS both carry about
3 GB/s a connection: iperf's 95 is 32 connections, NFS's 73 is 24.

**The base:**

- **One connection to a work CPU** (`svc_rtc_balance` Y over
  `svc_rtc_cpus` 8-55).  The hash shares CPUs (0.7 GB/s a shared
  connection at N, round 104); balanced placement removes that.
- **Each connection's frames on its work item's CPU** (RFS,
  `EXP_RFS=32768`): the ACKs are processed where the send runs, so the
  socket lock is not fought over (4.6% of the work items at N), and a
  READ's pages are freed on the CPU that allocated them, into its own
  per-CPU list (the zone lock is 7% of the work items at N).  RFS failed
  on 15 November (round 104, t2-r688) only through placement: a work CPU
  that took a steered receive was marked a receive CPU for good.
  `08b251d13aa0d` "SUNRPC: Do not mark a run-to-completion work CPU as a
  receive CPU" leaves the CPUs of `svc_rtc_cpus` unmarked; for build 534
  it is `sunrpc.ko` `54503B294CC030DD815A524` in
  `/var/tmp/zc-run-logs/sunrpc-rfsplace-534/` (the `sunrpc-nosplit-534`
  source with that change; every export the same).
- **nconnect 16 a port** (32 connections, as iperf's 95): nconnect 16
  lost at N (r740) when the ring CPUs saturated, with the receive on them.

The base is not the settled setting: [prof.sh](prof.sh) holds a run to
it (`EXP_RTC_BALANCE`, `EXP_RFS`, `NCONN`) and settles without it after.

About to run, through `step.sh`: the module swapped in with no client
connected, `settled.sh apply`, then `prof.sh 821`.

## t2-r821: the base, profiled

**61.22 GB/s** (30.60 and 30.61), no new kernel warning, settled after.
The placement is as meant: 32 connections on 32 CPUs, one each.

- **RFS did what it was there for.**  In the work CPUs' profile the
  zone lock is gone (`rmqueue_bulk` 0.12% of their samples, against
  about 7% at N in t2-r779) and so is the socket's contention
  (`lock_sock_nested` 0.25%, against 4.57%).  The ACKs are processed on
  the work CPUs now (`tcp_v4_do_rcv` 21.9% of their samples,
  `tcp_clean_rtx_queue` 11.2%, `skb_release_data` 5.4% self), which are
  28% busy; the rings 33%.
- **But the server holds far too many bytes.**  Each socket's Send-Q is
  3.50 MB (max 4.19, the 4 MiB send buffer), 2.75 MB of it below TCP
  (`skmem t`: in the qdisc and the NIC), 390 segments unacked; TCP's RTT
  1.71 ms, against 0.27 at N.  32 sockets hold about 112 MB, 88 MB of
  it below TCP, three and a half times the 32 MiB cache: memory 136.2
  GB/s, **2.22 bytes a byte sent**, at the mixed ceiling of about 140.
  The client's sockets hold 0.12 MB unread (N 3.30): the backlog has
  moved from the client to the server.
- **What lets it:** `net.ipv4.tcp_limit_output_bytes` is 4 MiB in
  `settled.sh` (TCP small queues: the bytes a socket may have below
  TCP), so 32 sockets may put 128 MiB in the qdiscs.  The NIC's queues
  take 8 MB (BQL); the rest waits in fq_codel.

So the next fix on the base is the bound on bytes below TCP.
`settled.sh` gains `EXP_TCP_LOB`; [ab.sh](ab.sh) runs the base's strobe
at a given limit.  About to run, through `step.sh`: `ab.sh 822:524288
823:262144 824:4194304`.

**t2-r822, the base with `tcp_limit_output_bytes` 512 KiB: 59.58 GB/s.**
Below TCP 0.56 MB a socket (2.75 at 4 MiB), TCP's RTT 0.49 ms (1.71),
the Send-Q still 3.56 MB: the bytes moved up into TCP, unsent, against
the 4 MiB send buffer.  Memory 136.2 GB/s (2.29 a byte), cycles a KiB
500 / 282 / 782.  **The bound stays in the base** (Mike: "have you kept
the change that allowed for the correct movement of the bottleneck? ...
We need to ensure that the bottlenecks get pinned to their new location
as we move them"): it moves the queue out of the qdiscs and the NIC into
TCP, where the next change, admission by send space, takes it.  The
value is pinned by a sweep: 256 KiB (r823) and 4 MiB (r824) running,
then 768 KiB (r825) and 1 MiB (r826).

**The sweep (t2-r822 to r825): the bound changes where the bytes wait,
not the rate.**

| run | `tcp_limit_output_bytes` | GB/s | below TCP a socket | Send-Q | TCP RTT |
|---|---|---|---|---|---|
| `t2-r823` | 256 KiB | 59.40 | 0.30 MB | 3.57 MB | 0.32 ms |
| `t2-r822` | 512 KiB | 59.58 | 0.56 MB | 3.56 MB | 0.49 ms |
| `t2-r825` | 768 KiB | 59.52 | 0.80 MB | 3.54 MB | 0.62 ms |
| `t2-r824` | 4 MiB | 59.34 | 2.81 MB | 3.50 MB | 1.70 ms |

Memory 2.29 to 2.30 bytes a byte in all.  **256 KiB is in the base**
(Mike: "stop and just use 256K"; `prof.sh` and `ab.sh` set it): the least
below TCP and the shortest RTT for the same rate.  1 MiB (r826) was
stopped by hand after its setup and ran its load out to a clean
teardown; its figure is not counted.  The Send-Q stays at 3.5 MB a
socket at every bound: the bytes wait in TCP, so the next change bounds
what a connection takes on, not what TCP sends.

## Admission by send space

`f379dad69c793` "SUNRPC: Bound the replies a run-to-completion
connection holds": `sunrpc.svc_rtc_send_max`, the bytes a connection's
TCP send queue may hold before its work item takes another call.  Past
it the call stays unread in the socket, where it holds no pages, and the
socket is marked `SOCK_NOSPACE`, so the ACKs that shrink the queue call
`svc_write_space()`, whose enqueue kicks the work item to receive again.
`rtc_stats` counts the receives it held back (`send_full`).  For 534,
on top of the placement fix: `sunrpc.ko` `A00D335DAB7B990CCA5645B` in
`/var/tmp/zc-run-logs/sunrpc-sendmax-534/` (every export the same).
[ab.sh](ab.sh) takes it as a third field and puts it back to 0 after.

About to run, through `step.sh`: the module swapped in with no client
connected, `settled.sh apply`, then `ab.sh 827:262144:0
828:262144:1048576 829:262144:524288`.

**t2-r827, admission off: 59.60 GB/s**, memory 2.28 bytes a byte, below
TCP 0.30 MB a socket, Send-Q 3.52 MB, RTT 0.32 ms: the base as before.

**t2-r828, admission at 1 MiB: every connection hung, and the host with
it.**  Each of the 32 connections hit the bound once (`send_full` 32)
and none ran again: their send queues drained to 0 while 2.3 MB of calls
waited unread, `runs` did not move with the bound set back to 0, and the
run hung in its load from 05:07.  The cause: a work item owns its
transport (`XPT_BUSY`, taken in `svc_xprt_enqueue()` before
`svc_rtc_divert()`) until its receive releases it.  `f379dad69c793`
skipped the receive and so never released it, and with `XPT_BUSY` held
every later `svc_xprt_enqueue()` (new data, write space, close) returns
at its test-and-set.  `systemctl restart nfs-server` (05:23:57) then
stuck in `svc_xprt_destroy_all()`: shutdown closes a transport by
enqueuing it, which the same bit refuses, so `nfsdctl threads 0` sleeps
in its retry loop, unkillable.  Only a reboot clears it.  The client's
fio and 32 hard mounts wait on the server.

`f379dad69c793` is not to run again as it is.  The fix: holding a
receive back must release the transport (clear `XPT_BUSY` without
enqueuing) and let the write-space callback's enqueue take it again,
with a KUnit case for a held-back receive resumed by write space before
it goes back on the rig.

**The fix, `544300fa74a00`** "SUNRPC: Release the transport when a
receive is held back": a receive held back clears `XPT_BUSY` and
enqueues the transport again, as `svc_xprt_received()` does.  The
enqueue does nothing while `SOCK_NOSPACE` is set; the write-space
callback's enqueue takes the transport back once an ACK shrinks the
queue.  (The host went down at 05:31 on 17 November, a second after
that commit was first made: its object, the branch's ref and the index
came back empty.  The commit before the amend, `f5c6c0bcd66f`, had the
same change, and the branch was put back on it and amended again; what
was moved aside is in `/var/tmp/git-crash-1117/`.)

For 534: the tip's `net/sunrpc` no longer builds against 534's headers
(`rx_loan_consumer_out()` and the rest), so the module is built from a
worktree at `a7e5a1b76d046` (the `sunrpc-nosplit-534` source) with the
placement fix, the bound and the fix cherry-picked
(`/var/tmp/sunrpc-holdfix-534-tree`, `574ac2257ea5f`; with the fix
taken off, its `svc_xprt.c` is `sunrpc-sendmax-534`'s byte for byte).
`sunrpc.ko` `2144A0407A3CAD9D9C4112A` in
`/var/tmp/zc-run-logs/sunrpc-holdfix-534/`: no warning but the
compiler's version, 442 exports with no version changed.

**The KUnit case, `rtc_real_send_full_resumed`** (`38a1ebfd0b1ea`,
its interface `d360e7859a809`): on a real service, not the suite's fake
transport (Mike: a fake transport is a fiction the test hopes stays
true).  `svc_create()` with a thread running `svc_recv()`, a TCP
listener on loopback and a client the thread accepts, so that
`svc_handle_xprt()` attaches the work item; the version is hidden, so
nothing asks rpcbind (the fixture is `fs/nfsd/nfsd_kunit_svc.h`'s, with
the thread receiving).  With `svc_rtc_send_max` at 1, the client sends
128 calls at its smallest window and reads none; once no run has come
for 500 ms, the receive must have been held back (`send_full` up, fewer
than 128 served) with `XPT_DATA` set and `XPT_BUSY` clear.  The client
then reads all 128 replies, each checked word for word, all from the
work item, no fallback.  Without the fix the `XPT_BUSY` check fails and
the read times out after 20 s, and the teardown then hangs in
`svc_xprt_destroy_all()` as nfsd's did: the case is not to be run
against `sunrpc-sendmax-534`.

For 534, both commits on the worktree (`87a87bd2d5be3`), built with
`build-sunrpc.sh` into `/var/tmp/zc-run-logs/sunrpc-holdfix-kunit-534/`:
no warning but the compiler's version, 442 exports with no version
changed; `sunrpc.ko` `6FB8AB5B48500088A19D991` (the fix with the
interface, which the suite needs), `svc_rtc_kunit.ko`
`459E7C2D37A6A0314A7456F`.  **Not loaded or run.**

### The boot after the hang, and the suite (17 November, about 06:10)

tardis2 came back (05:37) on build 534 with `sunrpc-sendmax-534`
(`A00D335DAB7B990CCA5645B`) from `updates/`, `svc_rtc_send_max` 0,
nfs-server inactive, kdump armed, no `WARNING:`, `BUG:` or `Oops`;
both ports answer 3 of 3.  The client still holds 8 NFS mounts and 34
fio from t2-r828, so `bringup.sh` (which starts nfsd) is not run and
nfsd is not started: the panic sysctls are set by hand as `bringup.sh`
sets them, and `crashlog.sh` ran alone (netconsole reaches the
client).  Clearing the client is Mike's.

The suite, each through `xeu-rig/step.sh
/var/tmp/zc-run-logs/kunit-holdfix-534 NAME ...`:

1. `swap`: `swap-sunrpc.sh /var/tmp/zc-run-logs/sunrpc-holdfix-kunit-534`
   (no NFS connection; nfs-server stays down).
2. `umount-nfsd`; `rmmod-nfsd`; `rmmod-kunit`; `kunit-on` (`modprobe
   kunit enable=1`); `clear-warn-once`.
3. `insmod-suite`: `insmod svc_rtc_kunit.ko`; `dmesg-save`.
4. `rmmod-suite`; `kunit-off`; `params` (sunrpc's settings read back).
   nfsd is not started and `settled.sh` not applied until the client
   is cleared.

### The suite took tardis2 down (17 November, 06:11:49)

The steps up to `insmod-suite` (06:11:41) all exited 0.  Tests 1 to 40
passed (`rtc_e2e_stop_full` took 4.6 s).  Test 41,
`rtc_real_send_full_resumed`, the new case, failed its last check
(`svc_rtc_kunit.c:2410`, `RTC_FB(a, b, i)` 1, not 0: one call of the
130 fell back to the thread, its reason not printed).  Right after
that came `Unable to handle kernel NULL pointer dereference at virtual
address 0000000000000000`, an instruction abort (`ESR 0x86000007`,
`EC 0x21`, level 3 translation fault): a call through a NULL function
pointer.  KUnit never printed `not ok 41`, so it happened in the case's
teardown (`rtc_real_free()`) or in a context running beside it.

What survived: the journal on disk, which ends at `Internal error:
Oops: 0000000086000007 [#1] SMP` (no registers, no `pc`, no call
trace); netconsole on the client, which ends earlier, at `ok 39`; no
dump.  kdump was armed (`kdumpctl` loaded at boot), `panic_on_oops` 1,
`/var/crash` empty, and nothing shows the crash kernel ran.  The next
boot's journal starts about 7 minutes later.  As on build 506, a crash
here leaves no dump, and the cause is not known.

One lead, in the fix itself: `svc_rtc_hold_recv()` clears
`XPT_BUSY` and enqueues, and the pass then goes on to drain the sends
on a transport it no longer owns (it still holds its xprt reference,
from `svc_rtc_pin()`, so the conn is not freed under it).  An enqueue
that finds write space can then hand the transport to the thread,
which is one way the call falls back.  No cause of the NULL call has
been found in the code.  The `svc_rtc_ops` hooks are all NULL-checked,
and the socket callbacks are saved and restored as for nfsd.

This boot (06:18) loaded the hold-fix `sunrpc.ko`
(`6FB8AB5B48500088A19D991`) from `updates/`, where the swap left it;
nfs-server is inactive, no `WARNING:`, `BUG:` or `Oops`.  The suite is
not to be run again until its teardown is understood and the crash can
be caught (`pc` and `lr` at least).

### The hold-back released last, and the next boot (17 November, 06:40)

`5562f5e440e7f` ("SUNRPC: Release a held-back transport after the
pass's sends") moves `svc_rtc_hold_recv()` to the end of the pass,
after the sends and the requeue, just before the pass drops its xprt
reference.  The ordinary receive path releases the transport before
its sends too (`svc_xprt_received()` in the receive), so this
narrows the hold-back pass to what it needs, and is not shown to be
what crashed.

For 534 it is on the worktree `/var/tmp/sunrpc-holdfix-534-tree` as
`3c583ea8bbcd2` (on `87a87bd2d5be3`), built with `build-sunrpc.sh`
into `/var/tmp/zc-run-logs/sunrpc-holdfix2-534/`: no warning but the
compiler's version, 442 exports with no version changed; `sunrpc.ko`
`94F9970EBA48D601405AD7A`.  It is in `updates/` (Mike: "put the
hold-fix in updates, I'm willing to try it"), with `depmod -a`; the
`6FB8AB5B…` it replaces is kept in
`/var/tmp/zc-run-logs/holdfix2-install-534/`.  The running boot still
has `6FB8AB5B…` loaded.  The initrd carries no `sunrpc.ko`.

`updates/sunrpc.ko` is a hard link shared with four of perf's
build-id cache entries (`~/.debug/.../updates/sunrpc.ko/<build-id>/elf`),
so each `cp` into `updates/`, `swap-sunrpc.sh`'s included, rewrites
them.  They now hold this module.  Mike does not need those four.

The next boot is the one-shot entry `ofp-rocky-534-pstore.conf`:
build 534's own entry with `efi_pstore.pstore_disable=0
pstore.max_reason=2`, so that an oops is written to EFI variables
before a panic hands over to kdump (which, with a crash kernel loaded,
runs before pstore's panic dump).  After it, check
`/sys/module/efi_pstore/parameters/pstore_disable` reads N and
`/sys/module/pstore/parameters/backend` reads `efi`, and read
`/sys/fs/pstore/` after any crash.

### Next boot: the hold-back fix on the rig (handoff, 17 November)

Where it stands: tardis2 runs build 534 with the first hold-back fix
(`6FB8AB5B48500088A19D991`) loaded; `updates/` holds the second
(`94F9970EBA48D601405AD7A`, `5562f5e440e7f`, worktree `3c583ea8bbcd2`),
which loads at the next boot.  nfs-server is down.  The client is
clear (Mike: "go ahead, clear the client", 17 November, 06:44): the 34
fio from t2-r828 killed (32 sat in D on the hard mounts until
`umount -f -l` of the 8 mounts let them go), no NFS mount, no
connection to port 2049; the steps are in
`/var/tmp/zc-run-logs/client-clear-1117/`.  The `svc_rtc_kunit` suite is not
to be run: it took the host down (above) and the NULL call is not
understood.  The one-shot `ofp-rocky-534-pstore.conf` is set.

After the boot, in order, committing each outcome before the next
step, and every risky step through `xeu-rig/step.sh`, one command to a
step:

1. The boot: `cat /proc/cmdline` has `efi_pstore.pstore_disable=0
   pstore.max_reason=2` (if not, the one-shot did not take: say so),
   `/sys/module/efi_pstore/parameters/pstore_disable` reads N and
   `/sys/module/pstore/parameters/backend` reads `efi`.  Anything
   already in `/sys/fs/pstore/` is copied out and read before it is
   removed.
2. kdump armed (`/sys/kernel/kexec_crash_loaded` 1), both ports pinged
   (`PORT-DEAD-AFTER-BOOT.md`), `/sys/module/sunrpc/srcversion` reads
   `94F9970EBA48D601405AD7A` and `svc_rtc_send_max` 0.
3. With no NFS mount on the client (check it): `bringup.sh` (crashlog and
   netconsole included), then `settled.sh apply` ending in "the
   settled setting".
4. The passes this sunrpc owes (AGENTS.md's hard rules), with the
   bound at 0: `xeu-rig/write-check.sh <n>` (77.6 GB/s or more) and
   `xeu-rig/read-check.sh <n>` (71.5 or more).  The next run is
   t2-r829.
5. Then round 105's A/B again, the bound in turns (`ab.sh
   <n>:262144:0 <n+1>:262144:1048576 <n+2>:262144:524288`), the plan
   that t2-r828 hung.  Commit the command before it runs.  If a
   connection stops (`runs` still, `send_full` up, Send-Q 0 with calls
   unread), set `svc_rtc_send_max` back to 0 at once and do not restart
   nfs-server: record `rtc_stats` and the sockets first.
