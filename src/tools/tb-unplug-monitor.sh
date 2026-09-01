#!/bin/bash
# tb-unplug-monitor.sh — background logger of Mellanox/en16/TB-domain state.
# Writes to /tmp/tb-unplug.log every 5 sec: timestamp + Mellanox-in-PCIe + en16 status.
# Runs under launchd (detached from the session). To stop:
#   launchctl bootout gui/$(id -u)/com.rdma.tb-monitor
LOG=/tmp/tb-unplug.log
while true; do
  ts=$(date '+%H:%M:%S')
  mlx=$(ioreg -r -c IOPCIDevice 2>/dev/null | grep -c 'compatible" = <"pci15b3,')
  en16=$(ifconfig en16 2>/dev/null | awk '/status:/{print $2}'; [ -z "$(ifconfig en16 2>/dev/null)" ] && echo "GONE")
  tb=$(system_profiler SPThunderboltDataType 2>/dev/null | grep -A2 "UTE02" | grep -c "Wavlink")
  printf '%s  mlx=%s en16=%s tbUTE02=%s\n' "$ts" "$mlx" "$en16" "$tb" >> "$LOG"
  sleep 5
done