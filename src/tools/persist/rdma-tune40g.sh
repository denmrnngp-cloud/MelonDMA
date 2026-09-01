#!/bin/bash
# Watchdog for the 40G link Mac <-> Spark (en16, ConnectX-4 Lx via Wavlink UTE02).
# Launched by the com.rdma.tune40g launchd daemon (KeepAlive=true — runs continuously).
#
# Problem it solves: after reboot/wake configd sometimes resets
# the static IP on en16 to APIPA 169.254.x. The standard persistence (preferences.plist,
# ConfigMethod=Manual) survives reboot, but not runtime TB reconfiguration.
# This watchdog checks and restores the IP every 15 seconds.
#
# IMPORTANT: under non-interactive launchd-sudo `networksetup -setmanual` fails (exit=1).
# The working path is `ifconfig en16 inet <ip> netmask <mask>` (works despite exit=1,
# it's a warning). Before setting we clear any foreign inet address (otherwise ifconfig adds a second one).
#
# Rationale for the values: notes/18-live-40g-link-bringup-and-tuning.md
set -u

IFACE=en16
TARGET_IP=192.168.200.1
TARGET_MASK=255.255.255.252
INTERVAL=15
SYSCTL_APPLIED=0

log() { echo "$(date '+%H:%M:%S') $*"; }

apply_sysctl() {
  sysctl -w kern.ipc.maxsockbuf=33554432     >/dev/null 2>&1
  sysctl -w net.inet.tcp.sendspace=2097152   >/dev/null 2>&1
  sysctl -w net.inet.tcp.recvspace=2097152   >/dev/null 2>&1
  sysctl -w net.inet.tcp.autosndbufmax=33554432 >/dev/null 2>&1
  sysctl -w net.inet.tcp.autorcvbufmax=33554432 >/dev/null 2>&1
  sysctl -w net.inet.tcp.win_scale_factor=8  >/dev/null 2>&1
  sysctl -w net.inet.tcp.delayed_ack=0       >/dev/null 2>&1
  sysctl -w net.inet.tcp.mssdflt=1994        >/dev/null 2>&1
}

# Remove all inet addresses on the interface EXCEPT target
strip_inet() {
  # ifconfig prints lines like "inet 169.254.99.99 netmask ..."
  ifconfig "$IFACE" 2>/dev/null | awk '/inet /{print $2}' | while read -r ip; do
    [ "$ip" = "$TARGET_IP" ] && continue
    ifconfig "$IFACE" inet "$ip" delete >/dev/null 2>&1
    log "removed stale inet $ip"
  done
}

log "=== rdma-tune40g watchdog started ==="

while true; do
  CUR=$(ifconfig "$IFACE" 2>/dev/null | awk '/inet /{print $2; exit}')

  if [ "$SYSCTL_APPLIED" -eq 0 ]; then
    apply_sysctl; SYSCTL_APPLIED=1; log "sysctl applied"
  fi

  if [ "$CUR" != "$TARGET_IP" ]; then
    log "en16 IP is '$CUR' (target $TARGET_IP) — restoring"
    strip_inet
    ifconfig "$IFACE" inet "$TARGET_IP" netmask "$TARGET_MASK" >/dev/null 2>&1
    sleep 2
    NEW=$(ifconfig "$IFACE" 2>/dev/null | awk '/inet /{print $2; exit}')
    if [ "$NEW" = "$TARGET_IP" ]; then
      log "restored OK: $NEW"
      # Clean up possible leftover APIPA 169.254.x so they don't hang around in parallel
      # (otherwise the "no APIPA" health-check fails and ifconfig shows two inet).
      ifconfig "$IFACE" 2>/dev/null | awk '/inet 169\.254/{print $2}' | while read -r ip; do
        ifconfig "$IFACE" inet "$ip" delete >/dev/null 2>&1
        log "cleaned leftover APIPA $ip"
      done
    else
      log "still '$NEW' — retry next cycle"
    fi
  fi

  sleep "$INTERVAL"
done