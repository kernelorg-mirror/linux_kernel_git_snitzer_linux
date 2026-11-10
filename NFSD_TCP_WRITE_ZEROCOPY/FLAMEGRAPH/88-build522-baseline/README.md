# Round 88: the profile on build 522, for the next round's focus

10 November, tardis2, `7.3.3-14.hs.522.canary` (built from
`d87b7e7c664b`, round 87's prefetch in), second boot, with the tip's
sunrpc from `updates/` (srcversion `F99F549CC7AF44FA39DAE34`,
`$L/sunrpc-522-tip`) and the kernel's own nfsd.

`run367.sh` checks the loaded sunrpc is the tip's instead of refusing a
non-empty `updates/`, then four two-port loan runs as round 76's:

- `t2-r367`, `t2-r368`: counted profile (cycles, instructions, L1D and
  L2D refills), for the totals and the top functions;
- `t2-r369`: cycles with call graphs, for who calls what (round 77's
  `t2-r327`);
- `t2-r370`: the held mark, for the region's exact peak.

Read with round 76's `read65.py`, `fsum.py`, `top.py`, `groups.py`,
`symdiff.py` and round 77's `callers.py`, `under.py`.
