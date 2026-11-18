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

`SRV_ALIAS=0` under `settled-iops.sh`: a port's four filesystems are
mounted from its one server address, so they share one nfs_client, one
rpc_clnt and its connections.  It cannot be one mount a port: each drive
is its own XFS.  Two settings (Mike: "yes, run nconnect 48 then 64"):

- `NCONN=48`: 96 connections, as settled, two a work CPU; only the
  rpc_clnts change, eight to two;
- `NCONN=64`: 128 connections, two or three a work CPU, David's count
  of connections a client; against 48 it shows what the connections add.

Spill off (the setting at rest), `fh_borrow_slots` 48.

    ../FLAMEGRAPH/117-one-clnt-per-port/ab-clnt.sh 1001:one48 1002:settled 1003:one48 1004:one64 1005:settled 1006:one64

Queued after round 116's "sequence done" line.  The server's established
connections a port are counted at the load's second 10 to confirm 64.
