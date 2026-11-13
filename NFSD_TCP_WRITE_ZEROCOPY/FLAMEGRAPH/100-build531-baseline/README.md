# Round 100: build 531's baseline and profile, for the next round's focus

13 November, tardis2, `7.3.3-16.hs.531.canary` (the tree's code built
in, nothing in `updates/`: sunrpc `6521F092BF4BEB6575F6802`, nfsd
`D95233FF10702BC0EC82B7B`, xeu Release 15 `C3071685F0211EF8EBA2A7F`
with 4 rings from the kernel's own `extra/`).  Both ports 5 of 5 before
`bringup.sh 32768 256`; kdump armed; `settled.sh apply` ends in "the
settled setting".  The client: sunrpc `AB674CEE87351D814F68CEE` (the
splice), `affinity_strict` 1 on `rpciod` and `xprtiod`.

**The ask (Mike):** a baseline and a profile on the new kernel, to
verify the settled two-port setting still carries about 78 GB/s
(round 99's `t2-r597`, `r598`: 78.06, 78.13) and to see what stands
out for the next round.

## The runs

- Baseline: `CELLS=":10 :10" N0=599 ../99-held-split-splice/run577.sh`,
  as `t2-r597`, `r598`: `t2-r599`, `r600`.
- Profile: [run601.sh](run601.sh), round 95's `run501.sh` at the
  settled setting: `t2-r601` counted on nfsd's CPUs, `t2-r602` call
  graphs on nfsd's CPUs, `t2-r603` call graphs on the rings' CPUs.
