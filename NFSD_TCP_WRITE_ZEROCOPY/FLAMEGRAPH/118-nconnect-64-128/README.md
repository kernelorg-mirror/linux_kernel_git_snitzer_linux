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
