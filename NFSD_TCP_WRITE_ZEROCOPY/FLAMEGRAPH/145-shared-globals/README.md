# Round 145: the two global lines

Mike, 19 November: "yes, do fixes 1 and 2 as round 145", from round
144's perf c2c: two global lines carried 533 of the client's 670
cross-socket HITMs, at 850 to 2,200 cycles a load.

- `49c73b8d1c2d8` "SUNRPC: Do not rewrite AUTH_UNIX's reply sizes on
  every reply": `unx_validate()` stores `au_verfsize`, `au_rslack` and
  `au_ralign` only when they change.
- `bdf70175fc522` "NFS: Keep the attribute generation counter on a
  cache line of its own": `____cacheline_aligned_in_smp`, so that
  `nfsiod_workqueue`, read by every `nfs_initiate_pgio()`, no longer
  shares its line.

W=1 clean on the client.  `ab-shared.sh`: B (the client's `updates/`
stack) against P, 4 KiB random READs at iodepth 96, B P B P B P
(t2-r1164 to r1169); round 144's perf c2c on P (r1170), to see both
lines quiet (P's addresses of the three symbols in its `syms.txt`);
then the WRITE and READ passes on P (r1171, r1172).

## The runs (07:51 to 08:25): 0.55 us a READ less on the client

| Run | Arm | 4 KiB IOPS | Port a / b (M) | p99 a / b (ms) | Client busy (busiest) | Client us a READ |
|---|---|---|---|---|---|---|
| t2-r1164 | B | 3.34 M | 1.57 / 1.77 | 4.88 / 3.75 | 83% (100%) | 11.93 |
| t2-r1165 | P | 3.56 M | 1.84 / 1.71 | 3.23 / 3.26 | 83% (92%) | 11.19 |
| t2-r1166 | B | 3.57 M | 1.81 / 1.76 | 3.29 / 3.36 | 88% (100%) | 11.83 |
| t2-r1167 | P | 3.53 M | 1.81 / 1.71 | 3.26 / 3.42 | 84% (92%) | 11.42 |
| t2-r1168 | B | 3.49 M | 1.80 / 1.70 | 3.33 / 3.36 | 86% (98%) | 11.83 |
| t2-r1169 | P | 3.47 M | 1.83 / 1.64 | 3.62 / 4.95 | 82% (97%) | 11.34 |
| mean | B | 3.47 M | | | 86% | 11.86 |
| mean | P | 3.52 M | | | 83% | 11.32 |

- **The client's CPU a READ is 0.55 us (4.6%) less**, and the ranges
  do not meet (P 11.19 to 11.42, B 11.83 to 11.93): the first change
  since round 136 to move it past the spread.
- IOPS 1.5% more, inside the spread: the client now has room (83%
  busy against 86%), and the server is as busy or busier (85 to 89%).
- perf c2c on P (t2-r1170, 3.34 M with port b's drives slow):
  `unix_auth` is gone from the shared lines; the counter's old line
  (with `nfsiod_workqueue`) is gone; the counter on its own line takes
  430 HITMs (167 remote), its increment and `nfs_refresh_inode_locked`'s
  read only.  Cross-socket HITMs 478, from 670.  The HITMs in all are
  about the same (46,730), the per-request lines at some 250 cycles
  each, against about 1,000 for the two lines' remote ones.
- Passes on P: WRITE t2-r1171 **77.96 GB/s PASS**, READ t2-r1172
  **81.60 GB/s PASS**, no new kernel warning.  The client back on its
  `updates/` stack and the settled setting after.

**Kept**: `49c73b8d1c2d8` and `bdf70175fc522` stay at the tip.  The
client's `updates/` does not have them yet; that waits on Mike's word.

**In the client's `updates/` since 19 November** (Mike: "yes, put both
fixes in the client's updates/"): the 17 modules built from
`bdf70175fc522` (sunrpc `036CCE7D414F6EAC3D6FDDD`), `depmod -a`, and
loaded with `cswap131.sh stock`.  The stack before (`9d2c9fcede7b6`,
sunrpc `2355B960B6A9951899AF6B4`) is kept in the client's
`/root/zc-ckern/updates-9d2c9fcede7b6/`.  From here B in an A/B is this
stack, at about 11.3 us of client CPU a READ.
