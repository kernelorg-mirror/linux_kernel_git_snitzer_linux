# Round 13: install a thread's last credentials again

**The change:** "NFSD: Install a thread's last credentials again rather than build them" (`916c11a2d1fc`).
- On a `no_subtree_check` export, `fh_verify()` built two creds for every request: `nfsd_set_fh_dentry()` copied the current cred to raise the nfsd capabilities for the handle's decode, and `nfsd_setuser()` built the user's. Each was a `prepare_creds()` with the LSM's copy of its blob, and each cred it replaced was freed after an RCU grace period.
- Each nfsd thread now keeps the cred `nfsd_setuser()` last built. A request that would get the same fsuid, fsgid and groups (after squashing) installs it again, or leaves it in place.
- The thread also keeps the decode's raised copy with the cred it was copied from. Since the same user leaves the same cred in place, the next decode finds that base and installs the same copy.
- A LOCALIO caller, which has no `svc_rqst`, keeps nothing. The thread's creds are put when it exits.

**Runs 2026-10-24 on tardis2**, build 494, the settled setting, round 10b's sunrpc (`6D7E259F6D29200D03F15FC`) throughout. The driver is `/var/tmp/zc-run-logs/t2-tipkernel/run31.sh`.
- A is nfsd built from the tree before round 13 (`74894D7FBB35A6557BEF163`), B with it (`8EF3B4156F6AAF38EC5CD9E`). Both were built by `xeu-rig/build-nfsd.sh` and swapped in with `swap-sunrpc.sh NFSD_KO=`, with sunrpc reloaded before every run.
- KUnit on B: 7, 10, 52 and 7 pass.
- **Identity** (`credcheck.sh`, [credcheck.txt](credcheck.txt)): two v3 exports, `no_root_squash` and `root_squash`, with files written by root and by uid 1000 alternately, 20 times on each.
  - Owners came out 0 and 1000 under `no_root_squash`, and 65534 and 1000 under `root_squash`, where root's 20 writes into the sticky directory were refused.
  - B's listing is identical to A's and to the installed nfsd's.

## Outcome: kept. nfsd 6.5% cheaper a WRITE

| run | GB/s | ring µs a WRITE | nfsd µs a WRITE |
|---|---|---|---|
| r13-1-loan64-A | 40.50 | 62.1 | 127.0 |
| r13-2-loan64-B | 40.52 | 59.0 | 121.5 |
| r13-3-loan64-A | 40.49 | 60.2 | 127.1 |
| r13-4-loan64-B | 40.58 | 57.6 | 113.1 |
| r13-5-loan64-A | 40.53 | 59.6 | 125.3 |
| r13-6-loan64-B | 40.50 | 60.0 | 120.2 |

Every run was settled and DRIVE-LIMITED, with no drive stepped down.

- **nfsd:** 126.5 to 118.3 µs a WRITE (-8.2, -6.5%), with no overlap.
- **The rings:** 60.6 against 58.9, inside the runs' scatter; the rings build no creds.
- **The profile** (fl10w, round 10b with A's nfsd, against fl13), inclusive µs a WRITE on nfsd:

| frame | fl10w | fl13 |
|---|---|---|
| `fh_verify` | 5.72 | 3.37 |
| `nfsd_set_fh_dentry` | 4.20 | 2.89 |
| `nfsd_setuser` | 1.09 | 0.18 |
| `prepare_creds` | 1.34 | 0 |
| `rcu_core` | 2.88 | 0.07 |

  - `prepare_creds()` is gone from the profile, and with it `rcu_core`: the creds freed after a grace period were nearly all of nfsd's RCU callback work. That is 5.2 µs of the 8.2.
  - What is left of `fh_verify` is the handle's decode (`exportfs_decode_fh_raw`, 1.65) and the export lookup (`rqst_exp_find`, 0.91).

**Latency** ([latency.txt](latency.txt), untraced runs):

| | p50 | p90 | p99 | mean |
|---|---|---|---|---|
| B1, round 13 | 0.453 ms | 0.537 | 0.594 | 0.462 |
| A1 | 0.432 | 0.514 | 0.618 | 0.445 |
| B2, round 13 | 0.420 | 0.494 | 0.561 | 0.432 |
| A2 | 0.489 | 0.561 | 0.627 | 0.488 |

No difference beyond the runs' scatter, as expected of 2-3 µs in a 0.45 ms WRITE.

**Since the start of the rounds:** a loaned WRITE costs nfsd about 118 µs (184 in round 01's profile) and the rings about 60 (92.6).

**Left as it is:**
- The second part of the plan, finding the open file by handle before the decode, is not done: what is left of `fh_verify` is 2.6 µs.
- tardis2's base for the next rounds is round 10b's sunrpc with B's nfsd.
