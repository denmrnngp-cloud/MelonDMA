#!/bin/bash
# Tuning for the 40G link Mac Studio (en16, ConnectX-4 Lx via Wavlink UTE02 TB3) <-> Spark
# Apply after every reboot (sysctl is not persistent).
# Run: sudo ./tune-40g-mac.sh
set -u

IFACE="${IFACE:-en16}"
MYIP="${MYIP:-192.168.200.1}"
MASK="${MASK:-255.255.255.252}"

echo "== Interface $IFACE =="
ifconfig "$IFACE" inet "$MYIP" netmask "$MASK" 2>/dev/null
# MTU is limited by the AppleEthernetMLX5 driver to 1280..2034 — jumbo unavailable.
ifconfig "$IFACE" mtu 2034 2>/dev/null

echo "== TCP stack =="
sysctl -w kern.ipc.maxsockbuf=33554432
sysctl -w net.inet.tcp.sendspace=2097152
sysctl -w net.inet.tcp.recvspace=2097152
sysctl -w net.inet.tcp.autosndbufmax=33554432
sysctl -w net.inet.tcp.autorcvbufmax=33554432
sysctl -w net.inet.tcp.win_scale_factor=8
sysctl -w net.inet.tcp.delayed_ack=0
sysctl -w net.inet.tcp.mssdflt=1994

echo
echo "== Status =="
ifconfig "$IFACE" | grep -E "mtu|inet |media|status"
echo
echo "Verify: iperf3 -c 192.168.200.2 -t 10 -w 2M -l 1M    # expect ~15 Gbit/s"
