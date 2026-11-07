# A port dead after boot

Started 2026-11-07 on tardis2, from three boots in one night that each
lost a port. Earlier cases are in the handoffs they happened in; this
file gathers them. It is not the silent port (`SILENT-PORT.md`), which
is a port that stops for about 25 s in the middle of a run, started on
the client's side, and comes back on its own. A port dead after boot
does not come back until the host's power is cut.

## What it looks like

On the server, one of the two E1 ports (`enp161s0f0` port a,
`enp164s0f0` port b) after a boot:

- `carrier` 1, `operstate` up, the address set; `bringup.sh` and
  `settled.sh apply` pass, and nothing in dmesg says anything is wrong
  (no fault, no IOMMU message; the ports have no `iommu_group`).
- It **sends**: ARP requests for the client go out (tcpdump shows
  them, `tx_packets` counts them).
- It **receives nothing**: `rx_packets` 0 on every ring, `rx_dropped`
  10 to 17 and rising slowly, the neighbour `FAILED`. Its rings are
  filled.
- The client's port on the same link is up at 400 Gb/s and cannot ping
  the server's port either, while the client's other port pings the
  server's other port. So the failure is at the server's receiver.
- `twoport.sh` stops after its rest with `a: no answer from
  192.168.1.106 in 20 s` (or `b: … 192.168.0.106`).

**The card's port counters do not say what is wrong.**
`port_rx_fifo_tc0_packets` read 4596 on the dead port of one boot, 1804
(stuck) on another, and 0 on a third, and it reads 0 on a working port
too (tardis2, port a, 7 November 04:35). Do not read a 0 there as "the
port saw no frame", and do not read a count as "frames reached the
port and were lost after it".

## What brings it back, and what does not

- **A hard power reset** (the host's power cut and restored). Every
  time so far, but it can lose the other port instead (tardis2, 7
  November).
- **Not** a reopen of the port: `port-setup.sh enp164s0f0 56` once on
  tardis1, four reopens by `twoport.sh` on tardis2.
- **Not** a reboot (Mike, from earlier cases on tardis1).
- **Not** a reload of `xeu` (tardis2, 7 November 04:43, port b, on
  Mike's word): `modprobe -r xeu; modprobe xeu`, then `bringup.sh 32768
  256` and `settled.sh apply`. Port b came back up with a link and its
  address, sent 23 packets and received none, the neighbour FAILED,
  as before. The reload also leaves the rig off its setting: port a
  answered but logged `Failed to allocate Rx page from pool` every 1
  to 3 s (the region's `alloc_fail` 8), the region showed 8 pools a
  port where a fresh boot has 4, and `bringup.sh` stopped at
  `enp161s0f0 does not have 4 rings`. So after a reload the boot is
  good for nothing but the power reset. Counters before and after are
  in `/var/tmp/zc-run-logs/t2-514-canary/port-dead/`.
- **Not** a PCI remove and rescan of the port's function (tardis2, 7
  November 04:46, port b, after the reload above, on Mike's word):
  `echo 1 > /sys/bus/pci/devices/0000:a4:00.0/remove`, `echo 1 >
  /sys/bus/pci/rescan`. The function came back with its BARs assigned,
  `xeu` probed it and named it `enp164s0f0` again, and after
  `bringup.sh` and `settled.sh apply` it had its address and received
  nothing, the neighbour FAILED, as before. A remove and rescan need
  not reset the card itself: it unbinds and probes again.
- **Not** a function-level reset (tardis2, 7 November 04:47, port b,
  after the two above, on Mike's word): the port down, `xeu` unbound
  from `0000:a4:00.0`, `echo flr > reset_method; echo 1 > reset` (no
  error; `reset_method` put back to `flr bus` after), `xeu` bound again.
  dmesg shows only the card's bus-master enable going off and on and
  the probe. After `bringup.sh` and `settled.sh apply` the port had its
  address, sent 21 packets and received none, the neighbour FAILED, as
  before.
- **Not** a reset of the secondary bus above the port (tardis2, 7
  November 04:48, port b, after the three above, on Mike's word): the
  port down, `xeu` unbound, `echo bus > reset_method; echo 1 > reset`
  (no error; put back to `flr bus` after), `xeu` bound again. The bridge
  above it, `0000:a3:00.0`, has port b alone under it, so nothing else
  was reset. dmesg as for the FLR: bus mastering off and on, the probe.
  After `bringup.sh` and `settled.sh apply`: its address, 21 packets
  sent, none received, the neighbour FAILED, as before; port a still
  answered.

So on a boot that has lost a port, nothing short of cutting the host's
power has been found to bring it back. Do not spend a boot on these
again: record the case and hand the host over for the power reset.

`lspci -vv` reads `LnkSta: Speed 2.5GT/s, Width x1` on the bridges
above both ports and `Speed unknown, Width x0` on both ports' functions,
the working port a's as well as the dead port b's. That is how the
card presents itself, not a sign of the fault.

A power cycle uses up a one-shot boot entry, so set it again before
the reset when the boot is into a test kernel: `bootctl set-oneshot
<entry>.conf`, and check that `bootctl status` shows `OneShot Entry`.

## The cases

| when | host, kernel | port lost | `port_rx_fifo_tc0_packets` | after |
|---|---|---|---|---|
| 29 Oct, night | tardis1, build 504, first boot | b | 1804, stuck | power reset: b back (that boot lost four drives instead) |
| 29 to 30 Oct | tardis1, build 504, third boot | none | | |
| 7 Nov, 02:00 | tardis2, A/B kernel (514 + bulk return), first boot | a | 4596 | power reset |
| 7 Nov, after reset | tardis2, A/B kernel, second boot | none | | runs done |
| 7 Nov, 04:25 | tardis2, held-mark kernel (514 + held mark), first boot | a | 0 | power reset |
| 7 Nov, 04:35 | tardis2, held-mark kernel, second boot | **b** | 0 (and 0 on the working a) | `xeu` reload, PCI remove and rescan, FLR, bus reset: still dead; power reset |
| 7 Nov, 04:55 | tardis2, held-mark kernel, third boot | **a and b** | 0 on both | nothing tried; power reset |
| 7 Nov, 05:05 | tardis2, held-mark kernel, fourth boot | **b**, dead before `bringup.sh` | 0 | nothing tried; power reset pending |

The tardis1 case was seen after the first boot's slot test, which
takes each port down and up, so it is not known whether that port was
dead from boot or went dead at the reopen. The tardis2 cases were all
seen at the first ping after `bringup.sh` and `settled.sh apply`, with
no port reopened before it except by those two.

Details are in the handoffs: "tardis1 on build 504 (29 October,
night)" in `xeu-rig/README.md`, and under "Next boot: the A/B of the
bulk return" and "Next boot: the region's held mark" in
`HANDOFF-tardis2-canary.md`.

## What is known and what is not

- **It does not stay with one port, cable or client NIC.** The two
  links are not alike (port a optical, port b copper; see "The two
  ports are not the same kind of link" in `SILENT-PORT.md`), and both
  have failed on tardis2, port a then port b on consecutive boots of
  the same kernel. That fits something going wrong in the card's
  receive path as it comes up, more than a link fault.
- **It can take both ports at once** (tardis2, held-mark kernel's
  third boot, 7 November 04:55): both had their link (the client at
  400 Gb/s on both), sent ARP (9 and 11 packets) and received nothing,
  `rx_dropped` 21 and 12, both neighbours FAILED, while the client's
  management address answered. Counters and dmesg are in
  `/var/tmp/zc-run-logs/t2-514-canary/port-dead/held-boot3/`.
- **A port can be dead from boot, before `bringup.sh`'s reopen**
  (tardis2, held-mark kernel's fourth boot, 7 November 05:05): pinged
  on the ports as the boot left them, a answered 3 of 3 and b 0 of 3,
  neighbour FAILED, so the reopen is not what kills it (at least not
  always). Unlike the earlier cases b was not silent: 38 frames
  received (31 on ring 3, one or two on six others), 65 dropped, 21
  sent, and still no ARP reply reached it. Counters and dmesg are in
  `/var/tmp/zc-run-logs/t2-514-canary/port-dead/held-boot4/`. Keep
  pinging before `bringup.sh` (step 0 below) to see how often a port is
  dead before the reopen.
- **It happens on both hosts**, so it is not one card.
- **Whether the test kernels cause it is not shown.** On tardis2 five
  of the six boots into a test kernel that night lost a port (the
  third both, the fourth b before `bringup.sh` had run). Both
  test kernels are build 514's source plus a small `page_pool` change
  (the bulk return, the held mark), with no change to `xeu` or the
  receive path's setup. Nobody has counted how often a boot of build
  514 itself, or of any other kernel, loses a port. Until that is
  counted, a lost port is not evidence against the kernel booted.
- **What the card's receiver is doing is not known.** `xeu` exposes no
  counter that shows it. A reload of `xeu` and a PCI remove and
  rescan, which give the driver a fresh probe, do not reach it, and nor
  does a function-level reset of the port or a reset of the bus above
  it: whatever holds the receiver survives every reset the host can
  give one port, so it is in logic a PCIe reset does not reach (the
  card's shared logic, its port's MAC or PHY, or its firmware), and
  only the power cut clears it. That is a question for Xsight.

## On every boot

0. Before `bringup.sh`, on the ports as the boot left them, ping both
   (the two commands in 1) and record the answer: it says whether a
   port lost later was dead from boot or went dead at the reopen.
1. After `bringup.sh` and `settled.sh apply`, ping both ports before
   anything else: `ping -c3 -I enp161s0f0 192.168.1.106` and `ping -c3
   -I enp164s0f0 192.168.0.106`. On tardis1 also after the slot test
   and anything else that reopens a port.
2. If one gets nothing: check the client's end over its management
   address (`$SSHC` in `xeu-rig/common.sh`, 10.20.12.106: its port up,
   and pinging the server's other port), so that a client fault is not
   taken for this.
3. Record the port, `rx_packets`, `rx_dropped`,
   `port_rx_fifo_tc0_packets`, the kernel and which boot of it this
   is, here in "The cases" and in the boot's handoff, and commit before
   the reset.
4. Set the one-shot entry again if the boot was a one-shot, and hand
   the host to Mike for a hard power reset.

Add every boot's outcome to "The cases", the boots that lose no port
too: the count of good boots is what will say whether a kernel makes
it more likely.
