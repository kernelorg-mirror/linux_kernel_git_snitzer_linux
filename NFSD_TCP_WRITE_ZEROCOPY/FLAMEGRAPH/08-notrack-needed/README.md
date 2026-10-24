# Round 08: `zc_notrack` only while a table needs conntrack

**The change:** no `zc_notrack` table while nothing registers conntrack hooks.
- Since round 01, podman's leftover netavark table is deleted while no container runs, so nothing registers conntrack hooks. The `notrack` chains still ran on every packet, in prerouting and output, with nothing to skip.
- The A/B used `twoport.sh`'s `NOTRACK=absent`. `settled.sh` held each run to it (`EXP_NOTRACK`) and named it as off the setting.

**Run 2026-10-24 on tardis2**, build 494, round 05's sunrpc (`91F9753F3DA76DEF91A628D`), the settled setting. The driver is `/var/tmp/zc-run-logs/t2-tipkernel/run26.sh`. Latency was measured A B A B with `lat.sh`, because single latency runs scatter by about 10% (L0).

## Outcome: kept. The rings 3.7% cheaper a loaned WRITE; latency unchanged

| run | `zc_notrack` | ring µs a WRITE | nfsd µs a WRITE | refusals |
|---|---|---|---|---|
| r8-1-loan64-A | loaded | 67.5 | 160.7 | 0.92 |
| r8-2-loan64-B | absent | 64.6 | 152.0 | 1.01 |
| r8-3-loan64-A | loaded | 66.9 | 160.0 | 1.00 |
| r8-4-loan64-B | absent | 65.7 | 155.1 | 1.00 |
| r8-5-loan64-A | loaded | 68.5 | 161.2 | 0.77 |
| r8-6-loan64-B | absent | 65.0 | 155.6 | 0.94 |

Every run was untraced, settled, DRIVE-LIMITED, with no drive stepped down.

- **The rings:** 67.6 to 65.1 µs a WRITE (-2.5, -3.7%), with no overlap.
- **nfsd:** 160.6 to 154.2.
- **The profile** (fl7 with the table, fl8 without): `nf_hook_slow` went from 1.77 to 0.01 µs a WRITE on the rings, and from 2.29 to 0.55 on nfsd. What is left on nfsd is SELinux's output hook.

**Latency** ([latency.txt](latency.txt), untraced runs):

| | p50 | p99 | mean |
|---|---|---|---|
| A1, loaded | 0.449 ms | 0.610 | 0.464 |
| B1, absent | 0.506 | 0.659 | 0.505 |
| A2, loaded | 0.494 | 0.676 | 0.494 |
| B2, absent | 0.477 | 0.643 | 0.482 |

No difference beyond the runs' own scatter, as expected for a few µs of CPU.

**The setting** (`xeu-rig/settled.sh`):
- apply deletes the leftover netavark table while no container runs;
- it then loads `zc-notrack.nft` only if some table still registers conntrack hooks (a running container's), and otherwise removes it;
- the audit holds `zc_notrack` to the same rule.

Checked on tardis2 both with podman's table put back and without it. The `notrack` statement registers no conntrack hook itself, so the rule cannot hold itself on.

**Since the start of the rounds:** a loaned WRITE costs the rings about 65 µs, against 92.6 before round 01 (-30%).
