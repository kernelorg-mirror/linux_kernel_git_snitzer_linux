# Round 85: staying's slow writes and its SGLs, in three pairs

9 November, tardis2, canary `7.3.3-13.hs.519.canary`, round 82's stack
(`$L/svcstack-ST`, `kernel-7.3/canary.ST`).

Round 84 found the time staying adds to a WRITE inside
`nfsd_vfs_write()`, all of it in writes over 2 ms (1,515 off, 2,572 on).
That was one run each.  This round asks two things in three pairs:

1. does staying's larger tail of slow writes repeat, and if so, does it
   form in the drives (a request's issue to its completion, by drive) or
   before them (a wait for a tag)?
2. does staying change a WRITE's SGLs?  Round 55 found about 125 entries
   a WRITE on stock sunrpc, one a received frame, so a record's pieces
   should not change them; it has not been measured with staying.

`tail-sgl.bt` times, for five seconds in each run's window:

- `nfsd_vfs_write()`: WRITEs, their time, the count and time over 2 ms;
- `nvme_pci_setup_data_sgl()`: requests, `nr_phys_segments`, the SGL
  entries written (read from the command as round 55 did), request bytes;
- `block_rq_issue` to `block_rq_complete`, writes only, by drive's minor
  number: the time a request and the count over 2 ms;
- `blk_mq_get_tag()`: its time, and the count and time over 100 us.

The minor numbers on this boot: nvme0n1 0, nvme1n1 2, nvme2n1 1, nvme3n1
4, nvme4n1 3, nvme5n1 6, nvme6n1 7, nvme7n1 5.

`run349.sh` swaps the stack in once, runs A (`svc_tcp_rx_stay_us=0`) and
B (1000) in the order A B A B A B (`t2-r349` to `t2-r354`), with the
client's mountstats (`84-reply-time/ms.py`), and puts the stock stack
back.
