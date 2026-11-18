# Round 117: one RPC client a port at nconnect 64

Mike, 18 November: "after round 116, run one mount per port at nconnect
64".

## Why

David Flynn's instructions (`nfsv3-max-iops-agent-instructions-20261117.md`)
reach 3.59 M (3.17 M typical) with one NFSv3 mount at `nconnect=128`:
128 connections to one share behind one rpc_clnt, which patch 27
(`7cc54f28c69fc`, "SUNRPC: Shard the rpc_clnt task list to remove cl_lock
contention", in build 541) makes possible.  The settled IOPS load
spreads 96 connections over eight rpc_clnts instead (`SRV_ALIAS=1
NCONN=12`, one server address a filesystem).

## The run

`SRV_ALIAS=0 NCONN=64` under `settled-iops.sh`: a port's four
filesystems are mounted from its one server address, so they share one
nfs_client, one rpc_clnt and its 64 connections, 128 in all.  It cannot
be one mount a port: each drive is its own XFS.  128 connections over
48 work CPUs place two or three a CPU, where 96 place two.  Spill off
(round 116's setting at rest), `fh_borrow_slots` 48.

    ../FLAMEGRAPH/117-one-clnt-per-port/ab-clnt.sh 1001:one 1002:settled 1003:one 1004:settled 1005:one

Queued after round 116's "sequence done" line.  The server's established
connections a port are counted at the load's second 10 to confirm 64.
