# Round 148: nconnect 16 against 12

Mike, 19 November: "yes, run the nconnect 16 A/B after the WRITE
passes", from round 147: the server's hot and cool work CPUs run the
same work in the same mix, so the spread is in how much traffic the
connections on each CPU carry.  A correction to what was said then: the
IOPS setting has 96 connections, not 24 (`SRV_ALIAS=1`, eight mounts
of 12), two to each of the 48 work CPUs, so the units are already
small.  nconnect 16 was measured before on an older stack, when the
client was the limit (round 109, step 3: 2.31 M IOPS at 12, 2.20 at
16, t2-r899 and r902).

`ab-nconn.sh`: the client on its `updates/` stack, 4 KiB random READs at
iodepth 96, nconnect 12 16 12 16 12 16 (t2-r1185 to r1190), each with
the server's work-CPU spread by mpstat.  No code or settled setting
changes, so no passes.
