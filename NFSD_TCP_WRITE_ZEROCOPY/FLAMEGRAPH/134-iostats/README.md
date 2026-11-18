# Round 134: block accounting off on the data drives

Mike, 18 November: "run the iostats=0 A/B, quick".  Round 133's
profile put 0.34 us of the server's CPU a READ in reading the counter
(`arch_counter_get_cntpct`), 0.12 of it for block accounting
(`blk_account_io_start`, `blk_mq_start_request`), with the accounting's
own work beside it.  `queue/iostats` is 1 on the eight data drives and
`settled.sh` does not hold it.

`ab-iostats.sh 1066:1 1067:0 1068:1 1069:0`: the client on round 132's
P stack (as t2-r1063, 3.50 M), iodepth 96, iostats set on nvme0n1 to
nvme7n1 for the run and back to 1 after each.  The client goes back to
build 541's stack after.

## The passes (Mike: "run a READ and WRITE pass and record result with a commit", and "I've taken to skip the READ and WRITE pass verification steps but they are important")

After the A/B, with the client on round 132's P stack (so that the
client's change is under them, and the server on round 129's stack,
so that rounds 129 to 134 are): `write-check.sh 1070`, then
`read-check.sh 1071`, each through `step.sh`; the client back on build
541's stack after.
