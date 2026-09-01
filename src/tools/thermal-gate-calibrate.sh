#!/bin/bash
# thermal-gate-calibrate.sh — cheap check of the hypothesis:
#   "does ifconfig/ip-down quiet the enclosure/card fan?"
#
# Makes exactly one measurement and does NOT touch the daemon-mode settings:
#   1. Baseline: CX-7 temperature on Spark (4 ports) + traffic counters.
#   2. Brings the data plane down on BOTH sides (Mac ifconfig down, Spark ip link down).
#   3. Waits IDLE_SECONDS (default 120).
#   4. Reads the temperature again.
#   5. Brings the link back up + reapplies the static IP (notes/18 §2/§7 gotcha).
#   6. Prints the temperature delta and asks you to rate the fan noise BEFORE/AFTER.
#
# Usage:
#   bash code/thermal-gate-calibrate.sh [IDLE_SECONDS]
#
# sudo is interactive: on the Mac it asks for the Mac password, on Spark
# (via ssh -t) for the Spark password. The production daemon (not this
# script) needs passwordless sudo on both machines.
#
# Peer connection is configurable via SPARK_SSH (ssh target, e.g. user@host)
# and SPARK_SSH_KEY (optional path to a private key).

set -u
IDLE_SECONDS="${1:-120}"
SPARK_IP=192.168.200.2
MAC_IF=en16
SPARK_IF=enp1s0f0np0
MAC_IP=192.168.200.1
MAC_NETMASK=255.255.255.252
SSH_HOST="${SPARK_SSH:-$SPARK_IP}"
SSH_KEY="${SPARK_SSH_KEY:-}"
SSH="ssh -t -o ConnectTimeout=10 -o StrictHostKeyChecking=no"
[ -n "$SSH_KEY" ] && SSH="$SSH -i $SSH_KEY"
SSH="$SSH $SSH_HOST"

C_GREEN=$'\033[32m'; C_RED=$'\033[31m'; C_YEL=$'\033[33m'; C_CYAN=$'\033[36m'; C_RST=$'\033[0m'
ok=0; fail=0
say_ok()   { printf "  ${C_GREEN}OK${C_RST}   %s\n" "$1"; ((ok++)); }
say_fail()  { printf "  ${C_RED}FAIL${C_RST} %s\n" "$1"; ((fail++)); }
say_info()  { printf "  ${C_CYAN}···${C_RST}   %s\n" "$1"; }
say_warn()  { printf "  ${C_YEL}!${C_RST}    %s\n" "$1"; }

# --- on Ctrl-C / error: bring the link back up at any cost ---
restore() {
  echo
  echo "${C_YEL}=== Restoring link (trap) ===${C_RST}"
  echo "Mac: bringing $MAC_IF up..."
  sudo ifconfig $MAC_IF up 2>/dev/null
  sudo ifconfig $MAC_IF inet $MAC_IP netmask $MAC_NETMASK 2>/dev/null
  $SSH "sudo ip link set $SPARK_IF up 2>/dev/null" 2>/dev/null
  sleep 3
  if ifconfig $MAC_IF 2>/dev/null | grep -q "status: active"; then
    say_ok "Mac $MAC_IF active"
  else
    say_warn "Mac $MAC_IF not active yet — fix manually: sudo ifconfig $MAC_IF up"
  fi
}
trap restore EXIT INT TERM

# --- read CX-7 temperature (4 ports) from Spark ---
spark_temp() {
  $SSH 'for p in enp1s0f0np0 enp1s0f1np1 enP2p1s0f0np0 enP2p1s0f1np1; do
    D=$(ls -d /sys/class/net/$p/device/hwmon/hwmon* 2>/dev/null | head -1)
    if [ -n "$D" ] && [ -f "$D/temp1_input" ]; then
      cur=$(cat $D/temp1_input)
      hi=$(cat $D/temp1_highest 2>/dev/null)
      printf "  %-16s cur=%5.1f°C peak=%5.1f°C [%s]\n" "$p" \
        "$(awk "BEGIN{print $cur/1000}")" \
        "$(awk "BEGIN{print $hi/1000}")" \
        "$(cat $D/temp1_label 2>/dev/null)"
    fi
  done' 2>/dev/null
}

# --- read traffic counters ---
mac_bytes() {
  netstat -i -b 2>/dev/null | awk -v if=$MAC_IF '$1==if && $3=="<Link#27>" {printf "RX=%sB TX=%sB", $7, $11; exit}'
  # fallback if the columns shift
  netstat -i -b 2>/dev/null | awk -v if=$MAC_IF '$1==if{r=$7; t=$11} END{print "  (alt) RX="r"B TX="t"B"}' 2>/dev/null
}
spark_bytes() {
  $SSH "awk -v if=$SPARK_IF ': ' \$1==if\":\" {print \$1, \"RX=\"\$2\" TX=\"\$10}' /proc/net/dev" 2>/dev/null
}

echo "${C_CYAN}============================================================${C_RST}"
echo "  THERMAL-GATE CALIBRATION  (idle = ${IDLE_SECONDS}s)"
echo "  Goal: see whether ifconfig/ip-down quiets the fan"
echo "${C_CYAN}============================================================${C_RST}"
echo

echo "${C_CYAN}=== 1. Baseline: link + Spark CX-7 temperature + traffic ===${C_RST}"
echo "Mac $MAC_IF:"
ifconfig $MAC_IF 2>/dev/null | grep -E "status|inet 192" | sed 's/^/    /'
mac_bytes | sed 's/^/    /'
echo "Spark $SPARK_IF:"
$SSH "ip -br a show $SPARK_IF 2>/dev/null; ip link show $SPARK_IF 2>/dev/null | head -1" | sed 's/^/    /'
spark_bytes | sed 's/^/    /'
echo "Spark CX-7 temperature (ASIC, all 4 ports):"
spark_temp
echo

echo "${C_CYAN}=== 2. POLL: listen to the fan NOW (card up, ~$(date +%H:%M:%S)) ===${C_RST}"
echo "    ${C_YEL}>>> How loud is it, subjectively? (1=quiet ... 10=screaming) <<<${C_RST}"
read -r -p "    Noise before (1-10): " noise_before
echo

echo "${C_CYAN}=== 3. Bring the data plane down on both sides ===${C_RST}"
echo "Mac: sudo ifconfig $MAC_IF down"
if sudo ifconfig $MAC_IF down 2>&1 | sed 's/^/    /'; then say_ok "Mac $MAC_IF down"; else say_fail "Mac down"; fi
echo "Spark: sudo ip link set $SPARK_IF down (will ask for the Spark sudo password)"
if $SSH "sudo ip link set $SPARK_IF down" 2>&1 | sed 's/^/    /'; then say_ok "Spark $SPARK_IF down"; else say_warn "Spark down — continuing (Mac test is still valid)"; fi
sleep 2
echo "Mac $MAC_IF status now:"
ifconfig $MAC_IF 2>/dev/null | grep -E "status|flags" | sed 's/^/    /'
echo

echo "${C_CYAN}=== 4. Waiting ${IDLE_SECONDS}s — card cools, fan (maybe) quiets ===${C_RST}"
echo "    ${C_YEL}>>> Listen closely to the fan for these ${IDLE_SECONDS} seconds <<<${C_RST}"
echo "    $(date +%H:%M:%S) — wait start"
sleep $IDLE_SECONDS
echo "    $(date +%H:%M:%S) — wait end"
echo

echo "${C_CYAN}=== 5. POLL: how is the noise AFTER idle ===${C_RST}"
read -r -p "    Noise after (1-10): " noise_after
echo

echo "${C_CYAN}=== 6. Spark temperature after idle ===${C_RST}"
spark_temp
echo

echo "${C_CYAN}=== 7. Bring the link back up + reapply IP ===${C_RST}"
echo "Mac: sudo ifconfig $MAC_IF up + inet $MAC_IP"
sudo ifconfig $MAC_IF up 2>&1 | sed 's/^/    /'
sudo ifconfig $MAC_IF inet $MAC_IP netmask $MAC_NETMASK 2>&1 | sed 's/^/    /'
echo "Spark: sudo ip link set $SPARK_IF up"
$SSH "sudo ip link set $SPARK_IF up" 2>&1 | sed 's/^/    /'
sleep 3
echo "Mac $MAC_IF:"
ifconfig $MAC_IF 2>/dev/null | grep -E "status|inet 192" | sed 's/^/    /'
echo "Spark $SPARK_IF:"
$SSH "ip -br a show $SPARK_IF 2>/dev/null; ip link show $SPARK_IF 2>/dev/null | head -1" | sed 's/^/    /'
echo

echo "${C_CYAN}=== 8. Connectivity check ===${C_RST}"
if ping -c2 -W2000 $SPARK_IP >/dev/null 2>&1; then
  rt=$(ping -c3 -W2000 $SPARK_IP | tail -1 | cut -d'=' -f2 | cut -d'/' -f2)
  say_ok "ping $SPARK_IP OK (~${rt} ms)"
else
  say_fail "ping $SPARK_IP — no answer. The IP may have fallen back to APIPA (notes/18 §2)."
  say_warn "Fix manually: sudo ifconfig $MAC_IF inet $MAC_IP netmask $MAC_NETMASK"
fi
echo

# the restore trap must not fire twice — disarm it
trap - EXIT INT TERM

echo "${C_CYAN}============================================================${C_RST}"
echo "  CALIBRATION RESULT"
echo "${C_CYAN}============================================================${C_RST}"
printf "  Fan noise:    %s → %s (1-10)\n" "${noise_before:-?}" "${noise_after:-?}"
delta=$(( ${noise_after:-0} - ${noise_before:-0} ))
if [ "$delta" -lt 0 ]; then
  echo "  ${C_GREEN}↓ Fan QUIETER on down — the daemon script makes sense.${C_RST}"
elif [ "$delta" -eq 0 ]; then
  echo "  ${C_YEL}= Fan UNCHANGED — the noise is NOT from card heat (enclosure controller).${C_RST}"
  echo "    ${C_YEL}The daemon is useless here; solve it in hardware (different enclosure / PWM resistor).${C_RST}"
else
  echo "  ${C_RED}↑ Fan LOUDER on down? — anomaly, check manually.${C_RST}"
fi
echo
echo "  Detailed log kept. Repeat with a different IDLE_SECONDS if the noise changes"
echo "  slowly (the card cools over >2 min). E.g.: bash code/thermal-gate-calibrate.sh 300"
echo "${C_CYAN}============================================================${C_RST}"
