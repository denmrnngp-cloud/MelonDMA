#!/bin/bash
# Tuning for the 40G link on DGX Spark (enp1s0f0np0, ConnectX-7) <-> Mac Studio
# Run: sudo ./tune-40g-spark.sh
set -u

IFACE="${IFACE:-enp1s0f0np0}"
MYIP="${MYIP:-192.168.200.2/30}"

echo "== Interface $IFACE =="
ip addr replace "$MYIP" dev "$IFACE"
ip link set "$IFACE" up
# Keep MTU 2034 = ceiling of the Apple driver on the other side (otherwise PMTU black hole).
ip link set "$IFACE" mtu 2034

echo "== Ring buffers (max 8192) =="
ethtool -G "$IFACE" rx 8192 tx 8192 2>/dev/null

echo "== Kernel net =="
sysctl -w net.core.netdev_max_backlog=250000
sysctl -w net.core.rmem_max=67108864
sysctl -w net.core.wmem_max=67108864
sysctl -w net.core.rmem_default=16777216
sysctl -w net.core.wmem_default=16777216
sysctl -w net.ipv4.tcp_rmem="4096 131072 67108864"
sysctl -w net.ipv4.tcp_wmem="4096 65536 67108864"
sysctl -w net.ipv4.tcp_mtu_probing=1

echo
echo "== Status =="
ip -br a show "$IFACE"
ethtool "$IFACE" | grep -E "Speed|Duplex|Link detected"
echo
echo "Server:  iperf3 -s -B 192.168.200.2"
