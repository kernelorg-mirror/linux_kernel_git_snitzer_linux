#!/bin/bash
# Keep a record of what the host was doing when it goes down (Mike, 16
# November: three hangs that day left nothing).  Run by bringup.sh right
# after kdump is armed, and safe to run again.
#
# - The journal is kept on disk (it was volatile on tardis2: /var/log/journal
#   did not exist, so each hang took its boot's log with it), synced every
#   second.
# - netconsole sends every kernel line, over the management network (never a
#   data port), to the client, which appends it to
#   /var/tmp/tardis2-netconsole.log.  It reaches the client when the
#   disk does not, as long as the kernel can still send.
#
# step.sh writes each step's start and output to /dev/kmsg, so both carry it.
cd "$(dirname "$0")" && . ./common.sh || exit 1

MGMT_DEV=${MGMT_DEV:-$(ip -o route get "$CLIMGMT" | sed -n 's/.* dev \([^ ]*\).*/\1/p')}
MGMT_SRC=$(ip -o route get "$CLIMGMT" | sed -n 's/.* src \([^ ]*\).*/\1/p')
PORT=6666
HOST=$(hostname -s)
ok=1

# The journal, on disk.
mkdir -p /etc/systemd/journald.conf.d
printf '[Journal]\nStorage=persistent\nSyncIntervalSec=1s\n' >/etc/systemd/journald.conf.d/zc-crashlog.conf
if [ ! -d /var/log/journal ]; then
	mkdir -p /var/log/journal
	systemd-tmpfiles --create --prefix /var/log/journal
	systemctl restart systemd-journald
fi
journalctl --flush
if [ -d /var/log/journal ] && journalctl --header 2>/dev/null | grep -q '/var/log/journal'; then
	echo "crashlog: journal on disk, synced every 1s"
else
	echo "crashlog: the journal is not on disk"; ok=0
fi

# The listener on the client: the one there since 2 November (a python3 on
# 10.20.12.106:6666 appending to /var/tmp/tardis2-netconsole.log, see
# TESTING.md), started the same way if it is gone.
LOG=/var/tmp/$HOST-netconsole.log
$SSHC "ss -ulnH 'sport = :$PORT' | grep -q . ||
	{ setsid nohup python3 -u -c \"import socket
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.bind(('$CLIMGMT',$PORT))
f=open('$LOG','ab',0)
while 1: f.write(s.recv(65536))\" </dev/null >/dev/null 2>&1 & }; sleep 1;
	ss -ulnH 'sport = :$PORT' | grep -q ." ||
	{ echo "crashlog: no netconsole listener on the client"; ok=0; }

# netconsole, to the client's management address.
if [ ! -d /sys/kernel/config/netconsole/zc ]; then
	ping -c1 -W2 "$CLIMGMT" >/dev/null
	mac=$(ip neigh show "$CLIMGMT" dev "$MGMT_DEV" | sed -n 's/.*lladdr \([^ ]*\).*/\1/p')
	[ -n "$mac" ] || { echo "crashlog: no MAC for $CLIMGMT on $MGMT_DEV"; exit 1; }
	modprobe netconsole
	mountpoint -q /sys/kernel/config || mount -t configfs none /sys/kernel/config
	d=/sys/kernel/config/netconsole/zc
	mkdir $d
	echo 1 >$d/extended 2>/dev/null
	echo "$MGMT_DEV" >$d/dev_name
	echo "$MGMT_SRC" >$d/local_ip
	echo "$CLIMGMT" >$d/remote_ip
	echo "$mac" >$d/remote_mac
	echo $PORT >$d/remote_port
	echo 1 >$d/enabled || { echo "crashlog: netconsole would not enable"; ok=0; }
fi
# Every kernel line goes out (the serial console's own level is untouched:
# netconsole takes the console loglevel too, so set it, but only to 7).
dmesg -n 7
tag="zc-crashlog $(date -Is) check"
echo "$tag" >/dev/kmsg
sleep 1
if $SSHC "grep -qF '$tag' $LOG"; then
	echo "crashlog: netconsole reaches $CLIMGMT:$LOG"
else
	echo "crashlog: netconsole's check line did not reach the client"; ok=0
fi
[ $ok = 1 ]
