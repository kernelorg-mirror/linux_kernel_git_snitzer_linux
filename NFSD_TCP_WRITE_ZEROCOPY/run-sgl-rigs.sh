#!/bin/bash
# run-sgl-rigs.sh up|down - the NVMe SGL phase-1 rigs (NVME_SGL_SUPPORT_PROJECT.md section 6), all on brd RAM disks:
#   brd:  XFS on /dev/ram0                               -> /export/sgl-brd
#   tcp:  nvmet (ram1) exported over NVMe/TCP 127.0.0.1  -> /export/sgl-tcp
#   loop: nvmet (ram2) exported over nvme-loop           -> /export/sgl-loop
# NVMe devices are found by subsystem NQN only, never by name.
set -u
C=/sys/kernel/config/nvmet
dev_for_nqn() { local c; for c in /sys/class/nvme/nvme*; do [ "$(cat $c/subsysnqn 2>/dev/null)" = "$1" ] || continue; for n in /sys/block/$(basename $c)n*; do [ -e "$n" ] && { echo /dev/$(basename $n); return 0; }; done; done; return 1; }
sub() { # NQN BACKING
	mkdir -p $C/subsystems/$1/namespaces/1
	echo 1 > $C/subsystems/$1/attr_allow_any_host
	echo -n $2 > $C/subsystems/$1/namespaces/1/device_path
	echo 1 > $C/subsystems/$1/namespaces/1/enable
}
case ${1:?up|down} in
up)
	modprobe brd rd_nr=3 rd_size=2097152 || exit 1
	modprobe nvmet; modprobe nvmet-tcp; modprobe nvme-tcp; modprobe nvme-loop
	sub sgl-tcp /dev/ram1; sub sgl-loop /dev/ram2
	mkdir -p $C/ports/2 $C/ports/3
	echo tcp > $C/ports/2/addr_trtype; echo ipv4 > $C/ports/2/addr_adrfam; echo 127.0.0.1 > $C/ports/2/addr_traddr; echo 4420 > $C/ports/2/addr_trsvcid
	echo loop > $C/ports/3/addr_trtype
	ln -sfn $C/subsystems/sgl-tcp $C/ports/2/subsystems/sgl-tcp; ln -sfn $C/subsystems/sgl-loop $C/ports/3/subsystems/sgl-loop
	dev_for_nqn sgl-tcp >/dev/null || nvme connect -t tcp -a 127.0.0.1 -s 4420 -n sgl-tcp >/dev/null
	dev_for_nqn sgl-loop >/dev/null || nvme connect -t loop -n sgl-loop >/dev/null
	sleep 1
	T=$(dev_for_nqn sgl-tcp) && L=$(dev_for_nqn sgl-loop) || { echo "nvme devices not found" >&2; exit 1; }
	for pair in "/dev/ram0 /export/sgl-brd" "$T /export/sgl-tcp" "$L /export/sgl-loop"; do
		set -- $pair; mkdir -p $2
		mountpoint -q $2 || { mkfs.xfs -f -q $1 && mount $1 $2; } || exit 1
		echo "$(basename $2): $1 virt_boundary_mask=$(cat /sys/block/$(basename $1)/queue/virt_boundary_mask) dma_alignment=$(cat /sys/block/$(basename $1)/queue/dma_alignment) lbs=$(cat /sys/block/$(basename $1)/queue/logical_block_size)"
	done ;;
down)
	for m in /export/sgl-brd /export/sgl-tcp /export/sgl-loop; do exportfs -u "*:$m" 2>/dev/null; mountpoint -q $m && umount $m; done
	nvme disconnect -n sgl-tcp >/dev/null 2>&1; nvme disconnect -n sgl-loop >/dev/null 2>&1
	rm -f $C/ports/2/subsystems/sgl-tcp $C/ports/3/subsystems/sgl-loop
	for s in sgl-tcp sgl-loop; do [ -d $C/subsystems/$s ] && { echo 0 > $C/subsystems/$s/namespaces/1/enable; rmdir $C/subsystems/$s/namespaces/1 $C/subsystems/$s; }; done
	rmdir $C/ports/2 $C/ports/3 2>/dev/null; true ;;
esac
