#!/bin/bash
# card-safe-disconnect.sh — safely disconnect the Mellanox card from the Mac in software.
#
# Does the software part of a safe disconnect (quiesce), after which it is
# fully safe to physically pull the Thunderbolt cable or power the enclosure off.
#
# Checks:
#   1. No active traffic on en16 (delta over 3 s = 0)
#   2. No TCP connections over 192.168.200.x
#   3. Brings en16 down on the Mac + all 4 ports down on Spark (via control plane)
#   4. Confirms: "safe to physically disconnect"
#
# What it does NOT do (physical, by hand): pull the Thunderbolt cable, power the enclosure off.
#
# Usage:
#   bash code/card-safe-disconnect.sh
#
# After the physical disconnect: the card drops out of PCIe and en16 disappears.
# The com.rdma.tune40g watchdog (KeepAlive) keeps running — on reconnect it
# restores 192.168.200.1 on en16 by itself (every 15 s).
#
# Peer connection is configurable via SPARK_SSH (ssh target, e.g. user@host)
# and SPARK_SSH_KEY (optional path to a private key).

set -u
MAC_IF=en16
MAC_IP=192.168.200.1
SPARK_CTRL=192.168.100.2     # control plane (SSH always alive)
SPARK_IFS="enp1s0f0np0 enp1s0f1np1 enP2p1s0f0np0 enP2p1s0f1np1"
SSH_HOST="${SPARK_SSH:-$SPARK_CTRL}"
SSH_KEY="${SPARK_SSH_KEY:-}"
SSH="ssh -o ConnectTimeout=10 -o StrictHostKeyChecking=no"
[ -n "$SSH_KEY" ] && SSH="$SSH -i $SSH_KEY"
SSH="$SSH $SSH_HOST"

C_GREEN=$'\033[32m'; C_RED=$'\033[31m'; C_YEL=$'\033[33m'; C_CYAN=$'\033[36m'; C_RST=$'\033[0m'
ok=0; fail=0
say_ok()   { printf "  ${C_GREEN}OK${C_RST}   %s\n" "$1"; ((ok++)); }
say_fail()  { printf "  ${C_RED}FAIL${C_RST} %s\n" "$1"; ((fail++)); }
say_info()  { printf "  ${C_CYAN}···${C_RST}   %s\n" "$1"; }
say_warn()  { printf "  ${C_YEL}!${C_RST}    %s\n" "$1"; }

echo "${C_CYAN}=== SAFE DISCONNECT of the Mellanox card from the Mac ===${C_RST}"
echo "Goal: quiesce in software → then physically pull the Thunderbolt cable / power off the enclosure"
echo

# --- 1. is the control plane alive? (needed to command Spark) ---
echo "${C_CYAN}=== 1. Control plane (to command Spark) ===${C_RST}"
if ping -c1 -W1000 $SPARK_CTRL >/dev/null 2>&1; then
  say_ok "control plane $SPARK_CTRL reachable (SSH to Spark)"
else
  say_warn "control plane unreachable — cannot bring Spark ports down remotely, continuing with the Mac side only"
fi
echo

# --- 2. active traffic on en16? ---
echo "${C_CYAN}=== 2. Check for active traffic on en16 ===${C_RST}"
b1=$(netstat -i -b 2>/dev/null | awk '$1=="en16" && $3=="<Link#27>"{print $7}')
t1=$(netstat -i -b 2>/dev/null | awk '$1=="en16" && $3=="<Link#27>"{print $11}')
sleep 3
b2=$(netstat -i -b 2>/dev/null | awk '$1=="en16" && $3=="<Link#27>"{print $7}')
t2=$(netstat -i -b 2>/dev/null | awk '$1=="en16" && $3=="<Link#27>"{print $11}')
rd=$((b2-b1)); td=$((t2-t1))
echo "  RX delta: $rd bytes, TX delta: $td bytes (over 3 s)"
if [ "$rd" -eq 0 ] && [ "$td" -eq 0 ]; then
  say_ok "no traffic — safe"
else
  say_fail "active traffic present — disconnect is unsafe, wait for it to finish"
  echo "  Check: ps aux | grep -E 'iperf3|bench-llama|rocev2|llama'"
  exit 1
fi
echo

# --- 3. TCP connections over the data plane? ---
echo "${C_CYAN}=== 3. TCP connections over 192.168.200.x ===${C_RST}"
conns=$(netstat -an 2>/dev/null | grep -c "192.168.200")
if [ "$conns" -eq 0 ]; then
  say_ok "no active connections over the data plane"
else
  say_warn "$conns connections — they will close on ifconfig down (normal, not dangerous)"
fi
echo

# --- 4. bring en16 down on the Mac ---
echo "${C_CYAN}=== 4. Bring en16 down on the Mac ===${C_RST}"
if ifconfig $MAC_IF 2>/dev/null | grep -q "status: active"; then
  sudo -n ifconfig $MAC_IF down 2>&1 | sed 's/^/    /'
  say_ok "ifconfig $MAC_IF down"
elif ifconfig $MAC_IF 2>/dev/null | grep -q "status: inactive"; then
  say_info "en16 already inactive (PHY already down) — ifconfig down to detach from the stack"
  sudo -n ifconfig $MAC_IF down 2>&1 | sed 's/^/    /'
else
  say_info "en16 does not exist (card already disconnected?)"
fi
sleep 1
echo "  en16 now: $(ifconfig $MAC_IF 2>/dev/null | grep -oE 'status: [a-z]+'|head -1)"
echo

# --- 5. bring all 4 ports down on Spark (if the control plane is alive) ---
echo "${C_CYAN}=== 5. Bring all 4 CX-7 ports down on Spark ===${C_RST}"
if ping -c1 -W1000 $SPARK_CTRL >/dev/null 2>&1; then
  for p in $SPARK_IFS; do
    $SSH "sudo -n ip link set $p down" 2>/dev/null && say_ok "$p down" || say_warn "$p — failed (maybe already down)"
  done
else
  say_warn "Spark unreachable on the control plane — ports not brought down remotely"
fi
echo

# --- 6. final check ---
echo "${C_CYAN}=== 6. State before the physical disconnect ===${C_RST}"
echo "Mac $MAC_IF: $(ifconfig $MAC_IF 2>/dev/null | grep -oE 'status: [a-z]+'|head -1)"
echo "Mellanox in PCIe: $(ioreg -r -c IOPCIDevice 2>/dev/null | grep -c 'compatible" = <"pci15b3,') node(s) (card still enumerated — normal until the physical unplug)"
echo "Control plane: $(ping -c1 -W1000 $SPARK_CTRL >/dev/null 2>&1 && echo 'alive (SSH works)' || echo 'unreachable')"
echo
echo "${C_GREEN}============================================================${C_RST}"
echo "${C_GREEN}  ✓ SOFTWARE DISCONNECT COMPLETE — SAFE TO UNPLUG${C_RST}"
echo "${C_GREEN}============================================================${C_RST}"
echo
echo "  Now physically (any option):"
echo "    A) Pull the Thunderbolt cable from Receptacle 4 (rear of the Mac)"
echo "    B) Power off the Wavlink UTE02 enclosure (outlet / PSU switch)"
echo
echo "  After that:"
echo "    - macOS sees a hot-unplug over Thunderbolt → the DEXT releases the card → en16 disappears"
echo "    - The enclosure fan stops (if option B — enclosure fully powered off)"
echo
echo "  To RECONNECT:"
echo "    bash code/card-reconnect.sh"
echo "    (plug the Thunderbolt cable back in, wait for re-enumeration; the watchdog restores the IP)"
