#!/bin/bash
# thermal-gate-1h.sh — non-interactive test: "data plane down for N minutes".
#
# Brings ALL 4 CX-7 ports down on Spark + en16 down on the Mac → holds idle for
# IDLE_SECONDS → logs the Spark CX-7 temperature every 10 min → at the end restores
# the original states (only the ports that were UP before the test).
#
# The control plane enP7s7 ↔ en0 (192.168.100.x) STAYS alive — SSH to Spark runs over it.
# The data plane 192.168.200.x (en16 ↔ enp1s0f0np0) is brought down.
#
# Compatible with macOS /bin/bash 3.2 (no declare -A, no bash-ism awk).
# Run via launchd: see /tmp/com.rdma.thermal-gate.plist
#
# Peer connection is configurable via SPARK_SSH (ssh target, e.g. user@host)
# and SPARK_SSH_KEY (optional path to a private key).

set -u
IDLE_SECONDS="${1:-3600}"
LOG=/tmp/thermal-gate-1h.log
MAC_IF=en16
MAC_IP=192.168.200.1
MAC_NETMASK=255.255.255.252
SPARK_CTRL=192.168.100.2     # control plane (SSH always alive)
SPARK_DATA=192.168.200.2     # data plane (checked after restore)
SPARK_IFS="enp1s0f0np0 enp1s0f1np1 enP2p1s0f0np0 enP2p1s0f1np1"
SSH_HOST="${SPARK_SSH:-$SPARK_CTRL}"
SSH_KEY="${SPARK_SSH_KEY:-}"
SSH="ssh -o ConnectTimeout=10 -o StrictHostKeyChecking=no"
[ -n "$SSH_KEY" ] && SSH="$SSH -i $SSH_KEY"
SSH="$SSH $SSH_HOST"
LOG_INTERVAL=600             # log temperature every 10 min
ORIG_STATES=""               # "port=STATE port=STATE ..." — filled in at baseline

stamp() { date '+%H:%M:%S'; }
log()  { printf '[%s] %s\n' "$(stamp)" "$*" >> "$LOG"; }

# was the port UP before the test?
was_up() { case "$ORIG_STATES" in *"$1=UP"*) return 0;; esac; return 1; }

# --- restore: bring everything back to its ORIGINAL state ---
up_all() {
  log "RESTORE: bringing the data plane back to its original state"
  sudo ifconfig $MAC_IF up 2>/dev/null
  sudo ifconfig $MAC_IF inet $MAC_IP netmask $MAC_NETMASK 2>/dev/null
  for p in $SPARK_IFS; do
    if was_up "$p"; then
      $SSH "sudo ip link set $p up" 2>/dev/null
    fi
  done
  sleep 5
  log "RESTORE: Mac $MAC_IF status=$(ifconfig $MAC_IF 2>/dev/null | grep -oE 'status: [a-z]+' | head -1)"
  log "RESTORE: Spark: $($SSH "for p in $SPARK_IFS; do printf \"%s=%s \" \"\$p\" \"\$(ip -br link show \$p 2>/dev/null | awk '{print \$2}')\"; done" 2>/dev/null)"
  if ping -c2 -W2000 $SPARK_DATA >/dev/null 2>&1; then
    log "RESTORE: data-plane ping OK ($(ping -c3 -W2000 $SPARK_DATA | tail -1 | cut -d'=' -f2 | cut -d'/' -f2) ms) — LINK RESTORED"
  else
    log "RESTORE-WARN: data-plane ping FAILED. Manual recovery:"
    log "  Mac:   sudo ifconfig $MAC_IF inet $MAC_IP netmask $MAC_NETMASK"
    log "  Spark: ssh $SSH_HOST 'sudo ip link set enp1s0f0np0 up'"
  fi
}
trap up_all EXIT INT TERM HUP

spark_temp_inline() {
  $SSH 'for p in enp1s0f0np0 enp1s0f1np1 enP2p1s0f0np0 enP2p1s0f1np1; do
    D=$(ls -d /sys/class/net/$p/device/hwmon/hwmon* 2>/dev/null | head -1)
    if [ -n "$D" ] && [ -f "$D/temp1_input" ]; then
      printf "%s=%.1fC(peak=%.1fC) " "$p" "$(cat $D/temp1_input | awk "{print \$1/1000}")" "$(cat $D/temp1_highest 2>/dev/null | awk "{print \$1/1000}")"
    fi
  done' 2>/dev/null
}

# ===== START =====
log "===== THERMAL-GATE START (idle=${IDLE_SECONDS}s, ctrl=$SPARK_CTRL) ====="
log "ALL 4 Spark CX-7 ports -> DOWN (fully inactive ASIC). Control plane enP7s7 stays UP."

# baseline: record the original Spark port states
log "BASELINE Spark port states (for restore):"
for p in $SPARK_IFS; do
  st=$($SSH "ip -br link show $p 2>/dev/null | awk '{print \$2}'" 2>/dev/null)
  [ -z "$st" ] && st="DOWN"
  ORIG_STATES="$ORIG_STATES $p=$st"
  log "  $p = $st"
done
log "BASELINE Mac $MAC_IF: $(ifconfig $MAC_IF 2>/dev/null | grep -oE 'status: [a-z]+' | head -1)"
log "BASELINE Spark CX-7 temp: $(spark_temp_inline)"

# BRING DOWN all 4 Spark ports + Mac en16
log "DOWN Spark: all 4 CX-7 ports"
for p in $SPARK_IFS; do
  $SSH "sudo ip link set $p down" >>"$LOG" 2>&1 && log "  $p down OK" || log "  $p down FAIL"
done
log "DOWN Mac: sudo ifconfig $MAC_IF down"
sudo ifconfig $MAC_IF down >>"$LOG" 2>&1 && log "  Mac $MAC_IF down OK" || log "  Mac down FAIL"
sleep 3

# status after down
log "STATUS after DOWN:"
log "  Mac $MAC_IF: $(ifconfig $MAC_IF 2>/dev/null | grep -oE 'status: [a-z]+' | head -1)"
log "  Spark: $($SSH "for p in $SPARK_IFS; do printf \"%s=%s \" \"\$p\" \"\$(ip -br link show \$p 2>/dev/null | awk '{print \$2}')\"; done" 2>/dev/null)"
log ""
log ">>> DATA plane FULLY DOWN (all 4 CX-7 ports + en16). <<<"
log ">>> Waiting ${IDLE_SECONDS}s. Note if the fan noise changes. <<<"
log ""

# WAIT + log temperature every LOG_INTERVAL seconds
elapsed=0
while [ "$elapsed" -lt "$IDLE_SECONDS" ]; do
  step=$(( IDLE_SECONDS - elapsed ))
  [ "$step" -gt "$LOG_INTERVAL" ] && step=$LOG_INTERVAL
  sleep "$step"
  elapsed=$(( elapsed + step ))
  log "T+${elapsed}s  Spark CX-7 temp: $(spark_temp_inline)"
done

log ""
log "===== WAIT DONE. Restoring the link ====="
up_all
trap - EXIT INT TERM HUP
log "===== THERMAL-GATE END ====="
