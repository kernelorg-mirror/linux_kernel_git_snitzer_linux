# Slipstream harness tools (tracked copies)

`xdp_pass.c` and `analyze.py` are copied unchanged from David Flynn's
2026-09-28 Slipstream drop
(`slipstream-kernel-overlays-hs-90059375-2026-09-28/tools/`, which is not
tracked in git), so that a fresh clone of this branch can run
`run-budget.sh` and analyze its results without the drop:

- `xdp_pass.c`: the `SEC("xdp.frags")` XDP_PASS program `run-budget.sh`
  attaches to the server-side veth, so received payload lands in page-pool
  pages (and in the fixed receive region).  `run-budget.sh` builds it with
  `clang -O2 -target bpf`.
- `analyze.py <outdir>`: the acceptance check for phases A and B
  (`stress-A/`, `stress-B/` under one directory).

The drop's other tools (`stress.sh`, `rig.sh`, `veth.sh`, `probe.sh`,
`run-kunit.sh`) are superseded by `run-budget.sh` and `run-kunit.sh` here.
