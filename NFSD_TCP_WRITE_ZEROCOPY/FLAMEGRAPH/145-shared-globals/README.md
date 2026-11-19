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
