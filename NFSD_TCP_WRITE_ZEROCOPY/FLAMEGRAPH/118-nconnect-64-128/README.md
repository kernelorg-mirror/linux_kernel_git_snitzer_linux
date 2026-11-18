# Round 118: nconnect 64 and 128 on the settled IOPS shape

Mike, 18 November: "I'd prefer we just take the existing newly
settled-iops.sh and increase nconnect to 64 and 128 just to see what
happens".

`settled-iops.sh` as held at `28eb5f75dd587` (eight filesystems each from
its own server address, so eight rpc_clnts; the three borrows; the rtc
spill onto the ring CPUs at weight 1; the client at scope cpu; 48 jobs at
iodepth 64), with only nconnect changed a mount: 64 (512 connections) and
128 (1024, the client's `NFS_MAX_CONNECTIONS`), against the settled 12
(96).  At weight 1 the spill places 512 connections about eight a work
CPU and seven a ring CPU.

    for r in 1003:64 1004:12 1005:128 1006:64 1007:12 1008:128; do
        ./step.sh /var/tmp/zc-run-logs/r118 nc-${r%%:*} \
            ./settled-iops.sh run ../FLAMEGRAPH/108-4k-randread/nfs4k.sh ${r%%:*}::${r#*:}
    done

## Result (18 November, 05:51 to 05:57): stopped after one run

| Run | nconnect | Connections | M IOPS (ports) | p99 a; b (ms) | Work CPUs (max) | Ring CPUs (max) | Server busy |
|---|---|---|---|---|---|---|---|
| t2-r1003 | 64 | 512 | 2.22 (1.11 / 1.11) | 8.29; 8.98 | 68.1% (100%) | 82.5% (100%) | 71% |

20% below the spill's 2.755 M at nconnect 12 (round 116), p99 more than
twice as long.  At about eight connections a CPU the spill's weight of 1
is next to nothing: the ring CPUs take nearly as many as a work CPU and
saturate, as round 115's 0-63 did.  Mike: "ok clearly nconnect 64 or 128
are way too high"; the rest were not run.

t2-r1004 (nconnect 12) was killed in its setup: the loop was stopped with
the agent's TaskStop, which killed the whole process group, the run with
it, and twoport.sh's EXIT cleanup did not run.  The client's four port-a
mounts, the four alias addresses, the client's workqueue scope (cpu) and
its irqbalance (off) were put back by hand, then `settled.sh apply`: the
settled setting.  It ran no load.
