#!/bin/bash
#
# start-nfsd.sh - bring up a local NFSv3 export backed by an XFS filesystem on
# an emulated NVMe device, for NFSD IO-mode testing (see
# NFSD_DONTCACHE_FALLBACK/).  Uses the stock nfs-utils service units.
#
# Backends:
#   nvme (default): the image file is exported as an NVMe-over-fabrics
#         namespace by the in-kernel target (nvmet, file backend, O_DIRECT to
#         the image) and attached through the loop transport (nvme-loop), so
#         /dev/nvmeXn1 has real NVMe queue limits: dma_alignment=3, hence
#         XFS advertises dio_mem_align=4 exactly like an NVMe rig.  The file
#         backend's logical block size is the backing filesystem's block size
#         (4096 on XFS), so dio_offset_align is 4096, not 512.
#   loop: the image via losetup --direct-io=on.  dma_alignment=511 (XFS then
#         advertises dio_mem_align=512, so the memory-misaligned fallback is
#         reachable) and logical block size 512.
# An nvmet *block* backend over a loop device does not work: nvme-loop
# forwards 4-byte-aligned host buffers unchanged and the loop driver
# rejects them (it always clamps its dma_alignment to 511), so every
# direct write whose payload is not 512-byte aligned in memory fails with
# EIO.  That combination (lbs=512 with dma_alignment=3) cannot be emulated
# without real NVMe or a ublk server.
#
# Usage:
#   start-nfsd.sh start [-t nvme|loop] [-s SIZE] [-i IMAGE] [-m MOUNTPOINT]
#   start-nfsd.sh stop          # unexport, unmount, tear down nvme/loop (keeps image)
#   start-nfsd.sh destroy       # stop + delete the image
#   start-nfsd.sh status
#   start-nfsd.sh mount-client [-L] [-v VERS] [-o OPTS] [-e SRC] [-H] [DIR]
#                               # loopback mount of the export; VERS is 3
#                               # (default, mounted with nolock) or 4.2 (any
#                               # 4.x nfsd advertises); OPTS are appended to
#                               # the mount options (e.g. -v 3 -o lock to get
#                               # NLM locking on v3); SRC overrides the
#                               # exported path to mount (default /export/t4);
#                               # -H mounts the Hammerspace MDS share instead
#                               # (HS_SERVER:HS_EXPORT, port HS_PORT, opts
#                               # HS_OPTS; defaults below) at DIR
#   start-nfsd.sh pnfs-export start|stop|status
#                               # a second, tmpfs-backed export /export/ff
#                               # (fsid=4712, "pnfs") so that a v4.2 client
#                               # mount gets pNFS flexfiles layouts from
#                               # knfsd (CONFIG_NFSD_FLEXFILELAYOUT), with
#                               # the server as its own NFSv3 data server;
#                               # see the pNFS notes below
#   start-nfsd.sh umount-client [DIR]
#   start-nfsd.sh build-module          # build fs/nfsd from this tree as nfsd.ko
#                                       # against the running kernel (out of tree)
#   start-nfsd.sh reload-module [KO]    # swap the running nfsd module for KO
#                                       # (default: the last build-module output;
#                                       #  "stock" = the installed kernel's module)
#
# build-module needs kernel-devel for the running kernel and a source tree
# whose fs/nfsd matches it apart from the changes under test.  reload-module
# stops nfs-server, nfsdcld and the /proc/fs/nfsd mount (the module's
# remaining users), replaces the module, restarts them and re-exports; the
# client mount, if any, is unmounted first and the nvme/loop export is left
# in place.
#
# The server side needs nothing for v4.x: the nfs-utils default nfsd
# configuration advertises +3 +4 +4.1 +4.2 (see /proc/fs/nfsd/versions) and
# the export's fsid=4711 is enough for the v4 pseudo-root.  A v4.2 client
# mount needs the client kernel built with CONFIG_NFS_V4 (nfsv4.ko).
#
# pNFS notes (NFS_REDUCE_ILOCK_CONTENTION): the flexfiles export must not be
# on XFS - XFS breaks layout leases on every write (xfs_break_layouts), and
# with knfsd being its own data server that deadlocks the v3 DS WRITE against
# the recall of the layout it is serving; tmpfs has no such hook.  An XFS
# export also offers the SCSI layout, which the client prefers and which
# would make it write to the block device directly; pnfs-export start
# installs /etc/modprobe.d/nfs-no-blocklayout.conf so the block layout
# driver cannot load.  Files under test must be held open for write on the
# server side (e.g. exec 9>>/export/ff/FILE) while the client uses them,
# otherwise knfsd grants the v4.2 client a write delegation that its own
# anonymous v3 DS WRITEs then conflict with (recall + NFS3ERR_JUKEBOX, 5 s
# per write).  Stock knfsd never recalls flexfiles layouts
# (disable_recalls); NFS_REDUCE_ILOCK_CONTENTION/nfsd-flexfiles-enable-recalls-TEST-ONLY.patch
# plus the layout_break debugfs module exist for recall testing only.
# Keep client writes below the tmpfs size: ENOSPC on the DS marks the device
# unavailable for 2 minutes and every I/O falls back to the MDS.
#
# mount-client disables NFS LOCALIO (nfs.localio_enabled=N) before mounting
# and umount-client restores the previous value.  With LOCALIO on, a client
# on the same host bypasses nfsd_vfs_write() entirely (fs/nfs/localio.c
# drives its own IOCB_DIRECT), so io_cache_write and the direct-mode split
# heuristic would never be exercised.  Pass -L to keep LOCALIO enabled.
#
# Hammerspace testbed (mount-client -H): HS_SERVER=10.200.104.60, the MDS
# itself (2026-09-20 the anvil refused this host and the workaround was an
# ssh forward with HS_SERVER=localhost; set that again only if the direct
# mount is refused)
# HS_EXPORT=/share1 HS_PORT=20492 HS_OPTS=xprtsec=none,timeo=100,retrans=2,vers=4.2
# (Mike's reference mount, 2026-09-20).  A real pNFS flexfiles MDS: no
# server-side holders, no recall injection; use it for A/B of the client
# series (ilock-tests.sh -H / ilock-measure.sh -H).  Note the anvil must
# authorize this client host: an unauthorized client gets NFS4ERR_PERM on
# EXCHANGE_ID ("nfs4_discover_server_trunking unhandled error -1").
#
# Defaults: backend=nvme SIZE=8G IMAGE=/var/tmp/nfsd-test.img MOUNTPOINT=/export/t4
# An image on tmpfs (e.g. -i /dev/shm/nfsd-test.img -s 4G) keeps the test
# storage in RAM: use it when the host's own NVMe is unreliable (this dev box
# logs "nvme0: I/O tag ... timeout, completion polled" under load, stalling
# every WRITE through the export for 30 s).  NFSD's O_DIRECT still goes to
# XFS on the emulated /dev/nvmeXn1 block device; only nvmet's storage of that
# device's sectors changes.  The image's pages inflate the host's Cached
# figure, so read the export-bdi counters instead.
# The image is created sparse; nvmet's file backend uses O_DIRECT on it, so
# the host page cache is not involved in either backend.
# Client mount default: /mnt/t4
#
# It never touches /sys/kernel/debug/nfsd/io_cache_*; set those yourself.

set -u

BACKEND=nvme
SIZE=8G
IMAGE=/var/tmp/nfsd-test.img
MNT=/export/t4
CLIENT_MNT=/mnt/t4
EXPORTS_FILE=/etc/exports.d/t4-loop.exports
EXPORT_OPTS="rw,no_root_squash,no_subtree_check,fsid=4711"
NQN=nqn.2026-09.io.nfsd-test:t4
NVMET=/sys/kernel/config/nvmet
STATE=/run/start-nfsd.state

die() { echo "start-nfsd: $*" >&2; exit 1; }

cmd=${1:-status}; shift || true
# -t/-s/-i/-m belong to "start"; the other commands parse their own options
[ "$cmd" = start ] && while getopts "t:s:i:m:" opt; do
	case $opt in
	t) BACKEND=$OPTARG ;;
	s) SIZE=$OPTARG ;;
	i) IMAGE=$OPTARG ;;
	m) MNT=$OPTARG ;;
	*) die "bad option" ;;
	esac
done
[ "$cmd" = start ] && shift $((OPTIND - 1))

loopdev_for_image() { losetup -j "$IMAGE" 2>/dev/null | head -1 | cut -d: -f1; }

# The nvme namespace whose subsystem NQN is ours, as /dev/nvmeXnY, or empty.
nvme_dev_for_nqn() {
	local n
	for n in /sys/block/nvme*n*; do
		[ -e "$n/device/subsysnqn" ] || continue
		# skip native-multipath path nodes (nvmeXcYnZ), which have no /dev node
		[ "$(cat "$n/hidden" 2>/dev/null)" = 1 ] && continue
		[ "$(cat "$n/device/subsysnqn")" = "$NQN" ] && { echo "/dev/$(basename "$n")"; return; }
	done
}

export_dev() {
	if [ -f "$STATE" ]; then
		. "$STATE"
		[ -n "${DEV:-}" ] && [ -e "$DEV" ] && { echo "$DEV"; return; }
	fi
	case $BACKEND in
	nvme) nvme_dev_for_nqn ;;
	loop) loopdev_for_image ;;
	esac
}

setup_loop() {
	local dev
	dev=$(loopdev_for_image)
	if [ -z "$dev" ]; then
		dev=$(losetup --find --show --direct-io=on "$IMAGE") || die "losetup failed"
		echo "attached $IMAGE as $dev (direct-io)" >&2
	fi
	echo "$dev"
}

setup_nvme() {
	local backing=$1 dev sub=$NVMET/subsystems/$NQN port=$NVMET/ports/1 i
	modprobe nvmet || die "modprobe nvmet failed"
	modprobe nvme-loop || die "modprobe nvme-loop failed"
	[ -d $NVMET ] || die "nvmet configfs not present"
	if [ ! -d "$sub" ]; then
		mkdir "$sub" || die "cannot create nvmet subsystem"
		echo 1 > "$sub/attr_allow_any_host"
		mkdir "$sub/namespaces/1"
		echo -n "$backing" > "$sub/namespaces/1/device_path"
		# On tmpfs the image is RAM already, so let nvmet's file backend use
		# buffered I/O for it: an O_DIRECT open of a tmpfs file is accepted but
		# serviced as page-cache I/O anyway.  This is the storage BELOW the
		# emulated NVMe; NFSD's own O_DIRECT runs against /dev/nvmeXn1 and is
		# unaffected by how nvmet stores that device's sectors.
		if [ "$(stat -f -c %T "$backing" 2>/dev/null)" = tmpfs ]; then
			echo 1 > "$sub/namespaces/1/buffered_io"
			echo "image on tmpfs: nvmet namespace uses buffered_io" >&2
		fi
		echo 1 > "$sub/namespaces/1/enable" || die "cannot enable nvmet namespace on $backing"
	fi
	if [ ! -d "$port" ]; then
		mkdir "$port"
		echo -n loop > "$port/addr_trtype"
	fi
	[ -e "$port/subsystems/$NQN" ] || ln -s "$sub" "$port/subsystems/$NQN"
	dev=$(nvme_dev_for_nqn)
	if [ -z "$dev" ]; then
		nvme connect -t loop -n "$NQN" >/dev/null 2>&1 || die "nvme connect failed"
		for i in 1 2 3 4 5 6 7 8 9 10; do
			dev=$(nvme_dev_for_nqn); [ -n "$dev" ] && break; sleep 0.5
		done
		[ -n "$dev" ] || die "nvme-loop namespace did not appear"
		echo "connected $NQN as $dev" >&2
	fi
	udevadm settle 2>/dev/null
	echo "$dev"
}

teardown_nvme() {
	local sub=$NVMET/subsystems/$NQN port=$NVMET/ports/1
	if [ -n "$(nvme_dev_for_nqn)" ]; then
		nvme disconnect -n "$NQN" >/dev/null 2>&1 && echo "disconnected $NQN"
	fi
	[ -e "$port/subsystems/$NQN" ] && rm "$port/subsystems/$NQN"
	[ -d "$port" ] && rmdir "$port"
	if [ -d "$sub" ]; then
		echo 0 > "$sub/namespaces/1/enable" 2>/dev/null
		rmdir "$sub/namespaces/1" 2>/dev/null
		rmdir "$sub" && echo "removed nvmet subsystem $NQN"
	fi
}

do_start() {
	[ "$(id -u)" -eq 0 ] || die "must run as root"
	if [ ! -e "$IMAGE" ]; then
		echo "creating sparse image $IMAGE ($SIZE)"
		truncate -s "$SIZE" "$IMAGE" || die "truncate failed"
	fi
	local dev
	case $BACKEND in
	nvme) dev=$(setup_nvme "$IMAGE") ;;
	loop) dev=$(setup_loop) ;;
	*) die "unknown backend $BACKEND" ;;
	esac
	printf 'BACKEND=%s\nDEV=%s\nIMAGE=%s\nMNT=%s\n' "$BACKEND" "$dev" "$IMAGE" "$MNT" > "$STATE"
	if ! blkid -o value -s TYPE "$dev" >/dev/null 2>&1; then
		echo "creating XFS on $dev"
		mkfs.xfs -q "$dev" || die "mkfs.xfs failed"
	fi
	mkdir -p "$MNT"
	if ! findmnt -n "$MNT" >/dev/null; then
		mount -t xfs "$dev" "$MNT" || die "mount failed (an image formatted under the other backend has a different sector size: run '$0 destroy' and start again)"
		echo "mounted $dev on $MNT"
	fi
	chmod 1777 "$MNT"
	mkdir -p "$(dirname "$EXPORTS_FILE")"
	printf '%s *(%s)\n' "$MNT" "$EXPORT_OPTS" > "$EXPORTS_FILE"
	systemctl start rpcbind || die "rpcbind failed to start"
	systemctl start nfs-server || die "nfs-server failed to start"
	exportfs -ra || die "exportfs -ra failed"
	do_status
}

do_stop() {
	[ "$(id -u)" -eq 0 ] || die "must run as root"
	[ -f "$STATE" ] && . "$STATE"
	findmnt -n "$CLIENT_MNT" >/dev/null && umount "$CLIENT_MNT"
	exportfs -u "*:$MNT" 2>/dev/null
	rm -f "$EXPORTS_FILE"
	exportfs -ra 2>/dev/null
	if findmnt -n "$MNT" >/dev/null; then
		umount "$MNT" || die "umount $MNT failed (still exported or busy?)"
		echo "unmounted $MNT"
	fi
	teardown_nvme
	local dev
	dev=$(loopdev_for_image)
	[ -n "$dev" ] && { losetup -d "$dev" && echo "detached $dev"; }
	rm -f "$STATE"
	echo "nfs-server left running; 'systemctl stop nfs-server' to stop it"
}

do_status() {
	[ -f "$STATE" ] && . "$STATE"
	echo "== image =="
	ls -lh "$IMAGE" 2>/dev/null || echo "no image $IMAGE"
	[ -n "$(loopdev_for_image)" ] && losetup -l "$(loopdev_for_image)" | tail -n +2
	echo "== export device =="
	local dev b
	dev=$(export_dev)
	if [ -n "$dev" ]; then
		b=$(basename "$dev")
		printf '%s (%s backend): logical_block_size=%s physical_block_size=%s dma_alignment=%s bdi=%s\n' "$dev" "${BACKEND}" \
			"$(cat /sys/block/$b/queue/logical_block_size)" \
			"$(cat /sys/block/$b/queue/physical_block_size)" \
			"$(cat /sys/block/$b/queue/dma_alignment)" "$(cat /sys/block/$b/dev)"
		[ "$BACKEND" = nvme ] && echo "nvmet subsystem: $NQN (file backend on $IMAGE, transport loop)"
	else
		echo "none"
	fi
	echo "== mount =="
	findmnt -n -o TARGET,SOURCE,FSTYPE,OPTIONS "$MNT" 2>/dev/null || echo "not mounted"
	findmnt -n "$MNT" >/dev/null && xfs_info "$MNT" 2>/dev/null | grep -E '^(data|naming)' | head -2
	echo "== services =="
	printf 'rpcbind=%s nfs-server=%s threads=%s\n' \
		"$(systemctl is-active rpcbind)" "$(systemctl is-active nfs-server)" \
		"$(cat /proc/fs/nfsd/threads 2>/dev/null || echo n/a)"
	echo "== exports =="
	exportfs -v 2>/dev/null | grep -A1 "^$MNT" || echo "not exported"
	rpcinfo -p 2>/dev/null | awk '$5=="nfs" && $2==3 { print "  nfs v3 via", $3, "port", $4 }'
	echo "== nfs client localio =="
	echo "  nfs.localio_enabled=$(cat $LOCALIO_PARAM 2>/dev/null || echo n/a)  mounted-server localio=$(cat /sys/fs/nfs/*/localio 2>/dev/null | head -1)"
	echo "== nfsd io modes / knobs (debugfs) =="
	for f in io_cache_read io_cache_write disable-splice-read direct_misaligned_min_bytes direct_misaligned_num_pages direct_misaligned_dontcache; do
		printf '  %s=%s\n' "$f" "$(cat /sys/kernel/debug/nfsd/$f 2>/dev/null || echo n/a)"
	done
	printf '  vm.dontcache_partial_writeback_delay_ms=%s\n' "$(sysctl -n vm.dontcache_partial_writeback_delay_ms 2>/dev/null || echo 'n/a (kernel lacks it)')"
}

MOD_OUT=${MOD_OUT:-/var/tmp/nfsd-mod}
SRC_TREE=$(cd "$(dirname "$0")" && pwd)

do_build_module() {
	local kdir=/lib/modules/$(uname -r)/build
	[ -d "$kdir" ] || die "no $kdir (install kernel-devel-$(uname -r))"
	[ -f "$SRC_TREE/fs/nfsd/Makefile" ] || die "no fs/nfsd under $SRC_TREE"
	mkdir -p "$MOD_OUT"
	make -s -C "$kdir" M="$SRC_TREE/fs/nfsd" MO="$MOD_OUT" modules || die "module build failed"
	[ -f "$MOD_OUT/nfsd.ko" ] || die "build produced no nfsd.ko"
	echo "built $MOD_OUT/nfsd.ko srcversion=$(modinfo -F srcversion "$MOD_OUT/nfsd.ko") vermagic=$(modinfo -F vermagic "$MOD_OUT/nfsd.ko" | cut -d' ' -f1)"
}

do_reload_module() {
	local ko=${1:-$MOD_OUT/nfsd.ko}
	if [ "$ko" = stock ]; then
		ko=$(modinfo -n nfsd -k "$(uname -r)") || die "no stock nfsd module"
	fi
	[ -f "$ko" ] || die "no module $ko"
	[ "$(modinfo -F vermagic "$ko" | cut -d' ' -f1)" = "$(uname -r)" ] || die "$ko was not built for $(uname -r)"
	findmnt -n "$CLIENT_MNT" >/dev/null && umount "$CLIENT_MNT"
	systemctl stop nfs-server nfsdcld.service proc-fs-nfsd.mount || die "could not stop nfsd users"
	if lsmod | grep -q '^nfsd '; then
		rmmod nfsd || die "rmmod nfsd failed (holders: $(lsmod | awk '$1=="nfsd"{print $4}'))"
	fi
	case "$ko" in
	*.xz|*.zst|*.gz) modprobe nfsd || die "modprobe nfsd failed" ;;
	*) insmod "$ko" || die "insmod $ko failed" ;;
	esac
	systemctl start proc-fs-nfsd.mount nfs-server || die "could not restart nfs-server"
	exportfs -ra
	echo "running nfsd srcversion=$(cat /sys/module/nfsd/srcversion) threads=$(cat /proc/fs/nfsd/threads) exported=$(exportfs -v | grep -c "^$MNT")"
}

HS_SERVER=${HS_SERVER:-10.200.104.60}
HS_EXPORT=${HS_EXPORT:-/share1}
HS_PORT=${HS_PORT:-20492}
HS_OPTS=${HS_OPTS:-xprtsec=none,timeo=100,retrans=2,vers=4.2}
LOCALIO_PARAM=/sys/module/nfs/parameters/localio_enabled
LOCALIO_SAVE=/run/start-nfsd.localio

do_mount_client() {
	local keep_localio=0 vers=3 extra="" opts src=$MNT hs=0
	while [ $# -gt 0 ]; do
		case "$1" in
		-L) keep_localio=1; shift ;;
		-v) vers=$2; shift 2 ;;
		-o) extra=$2; shift 2 ;;
		-e) src=$2; shift 2 ;;
		-H) hs=1; vers=4.2; shift ;;
		-*) die "mount-client: unknown option $1" ;;
		*) break ;;
		esac
	done
	local dir=${1:-$CLIENT_MNT}
	case "$vers" in
	3) opts="vers=3,nolock" ;;
	4|4.0|4.1|4.2) opts="vers=$vers"; modprobe nfsv4 2>/dev/null || die "nfsv4 module not available (CONFIG_NFS_V4?)" ;;
	*) die "mount-client: unsupported NFS version $vers" ;;
	esac
	[ -n "$extra" ] && opts="$opts,$extra"
	local target="localhost:$src"
	if [ $hs -eq 1 ]; then
		modprobe nfsv4 2>/dev/null || die "nfsv4 module not available"
		modprobe nfs_layout_flexfiles 2>/dev/null
		opts="$HS_OPTS,port=$HS_PORT${extra:+,$extra}"
		target="$HS_SERVER:$HS_EXPORT"
	fi
	mkdir -p "$dir"
	findmnt -n "$dir" >/dev/null && { echo "$dir already mounted"; return; }
	modprobe nfs 2>/dev/null
	if [ $keep_localio -eq 0 ] && [ -w "$LOCALIO_PARAM" ]; then
		[ -f "$LOCALIO_SAVE" ] || cat "$LOCALIO_PARAM" > "$LOCALIO_SAVE"
		echo N > "$LOCALIO_PARAM"
	fi
	mount -t nfs -o "$opts" "$target" "$dir" || die "client mount failed (opts=$opts target=$target)"
	findmnt -n -o TARGET,SOURCE,FSTYPE,OPTIONS "$dir"
	[ $hs -eq 1 ] && echo "pnfs: $(awk -v d="$dir" '$0 ~ "mounted on "d" " {f=1} f && /nfsv4:/ {match($0,/pnfs=[A-Z_,]*/); print substr($0,RSTART,RLENGTH); exit}' /proc/self/mountstats)"
	local lio
	lio=$(cat /sys/fs/nfs/*/localio 2>/dev/null | head -1)
	echo "localio for this mount: ${lio:-unknown} (nfs.localio_enabled=$(cat "$LOCALIO_PARAM" 2>/dev/null))"
}

do_umount_client() {
	local dir=${1:-$CLIENT_MNT}
	findmnt -n "$dir" >/dev/null && umount "$dir" && echo "unmounted $dir"
	if [ -f "$LOCALIO_SAVE" ] && [ -w "$LOCALIO_PARAM" ]; then
		cat "$LOCALIO_SAVE" > "$LOCALIO_PARAM"
		echo "restored nfs.localio_enabled=$(cat "$LOCALIO_PARAM")"
		rm -f "$LOCALIO_SAVE"
	fi
}

PNFS_MNT=/export/ff
PNFS_EXPORTS_FILE=/etc/exports.d/ff-loop.exports
PNFS_SIZE=${PNFS_SIZE:-4G}
do_pnfs_export() {
	case "${1:-status}" in
	start)
		mkdir -p "$PNFS_MNT"
		findmnt -n "$PNFS_MNT" >/dev/null || mount -t tmpfs -o size=$PNFS_SIZE tmpfs "$PNFS_MNT" || die "tmpfs mount failed"
		echo "$PNFS_MNT *(rw,no_root_squash,no_subtree_check,fsid=4712,pnfs)" > "$PNFS_EXPORTS_FILE"
		printf '# start-nfsd.sh pnfs-export: keep the loopback v4.2 client on the flexfiles layout\ninstall blocklayoutdriver /bin/false\n' > /etc/modprobe.d/nfs-no-blocklayout.conf
		lsmod | grep -q '^blocklayoutdriver ' && rmmod blocklayoutdriver
		exportfs -ra
		grep -q 'CONFIG_NFSD_FLEXFILELAYOUT=y' /lib/modules/$(uname -r)/build/.config 2>/dev/null || echo "warning: running kernel config lacks CONFIG_NFSD_FLEXFILELAYOUT" >&2
		exportfs -v | grep -A1 "^$PNFS_MNT"
		;;
	stop)
		findmnt -n "$CLIENT_MNT" >/dev/null && umount "$CLIENT_MNT"
		exportfs -u "*:$PNFS_MNT" 2>/dev/null; rm -f "$PNFS_EXPORTS_FILE"; exportfs -ra 2>/dev/null
		findmnt -n "$PNFS_MNT" >/dev/null && umount "$PNFS_MNT" && echo "unmounted $PNFS_MNT"
		;;
	status)
		findmnt -n -o TARGET,SOURCE,FSTYPE,SIZE,USED "$PNFS_MNT" 2>/dev/null || echo "$PNFS_MNT not mounted"
		exportfs -v | grep -A1 "^$PNFS_MNT" || echo "not exported"
		;;
	*) die "pnfs-export start|stop|status" ;;
	esac
}

case "$cmd" in
start)          do_start ;;
stop)           do_stop ;;
destroy)        do_stop; rm -f "$IMAGE" && echo "removed $IMAGE" ;;
status)         do_status ;;
mount-client)   do_mount_client "$@" ;;
umount-client)  do_umount_client "$@" ;;
build-module)   do_build_module ;;
reload-module)  do_reload_module "$@" ;;
pnfs-export)    do_pnfs_export "$@" ;;
*)              sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
