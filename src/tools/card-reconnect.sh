#!/bin/bash
# card-reconnect.sh — safely reconnect the Mellanox card to the Mac.
#
# Run AFTER the Thunderbolt cable is plugged back in (or the enclosure is
# powered on). Waits for the card to re-enumerate in PCIe, verifies en16
# appears, brings the Spark ports back up, waits for the IP watchdog to
# restore addressing, and checks ping.
#
# Usage:
#   bash code/card-reconnect.sh
#
# Requires passwordless sudo (configured in /etc/sudoers.d/rdma-thermal):
#   Mac:   /sbin/ifconfig en16 ...
#   Spark: /usr/sbin/ip link set enp1s0f0np0 up  (and other ports)
#
# Peer connection is configurable via SPARK_SSH (ssh target, e.g. user@host)
# and SPARK_SSH_KEY (optional path to a private key).

set -u
MAC_IF=en16
MAC_IP=192.168.200.1
MAC_NETMASK=255.255.255.252
SPARK_CTRL=192.168.100.2
SPARK_DATA=192.168.200.2
SPARK_IFS_ORIG_UP="enp1s0f0np0 enP2p1s0f0np0"   # ports that were UP before the disconnect
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

echo "${C_CYAN}=== RECONNECT the Mellanox card to the Mac ===${C_RST}"
echo "Make sure the Thunderbolt cable is in Receptacle 4 (or the enclosure is powered on)"
echo
read -r -p "Is the card physically connected? (y/N): " ans
[ "$ans" = "y" ] || { echo "Canceled. Plug the cable in and run again."; exit 0; }
echo

# --- 1. wait for the card to re-enumerate in PCIe (up to 60 s) ---
echo "${C_CYAN}=== 1. Waiting for Mellanox re-enumeration in PCIe (up to 60 s) ===${C_RST}"
for i in $(seq 1 12); do
  n=$(ioreg -r -c IOPCIDevice 2>/dev/null | grep -c 'compatible" = <"pci15b3,')
  if [ "$n" -ge 1 ]; then say_ok "Mellanox in PCIe ($n node) after $((i*5))s"; break; fi
  printf "    waiting... (%ds)\n" $((i*5)); sleep 5
done
n=$(ioreg -r -c IOPCIDevice 2>/dev/null | grep -c 'compatible" = <"pci15b3,')
[ "$n" -ge 1 ] || { say_fail "Mellanox did not appear in PCIe within 60s — check the cable/enclosure power"; exit 1; }
echo

# --- 2. wait for en16 to appear (DEXT re-bind, up to 30 s) ---
echo "${C_CYAN}=== 2. Waiting for en16 (DEXT re-bind, up to 30 s) ===${C_RST}"
for i in $(seq 1 6); do
  if ifconfig $MAC_IF >/dev/null 2>&1; then say_ok "en16 appeared after $((i*5))s"; break; fi
  printf "    waiting... (%ds)\n" $((i*5)); sleep 5
done
ifconfig $MAC_IF >/dev/null 2>&1 || { say_fail "en16 did not appear within 30s — DEXT did not re-bind"; echo "  Try: sudo ifconfig en16 up, or check ioreg"; exit 1; }
echo "  en16: $(ifconfig $MAC_IF 2>/dev/null | grep -oE 'status: [a-z]+'|head -1)"
echo

# --- 3. bring up en16 + IP (if the watchdog has not already done it) ---
echo "${C_CYAN}=== 3. Bring up en16 + IP (if the watchdog has not restored it) ===${C_RST}"
sudo -n ifconfig $MAC_IF up 2>&1 | sed 's/^/    /'
sleep 2
cur_ip=$(ifconfig $MAC_IF 2>/dev/null | awk '/inet /{print $2; exit}')
if [ "$cur_ip" = "$MAC_IP" ]; then
  say_ok "IP already $MAC_IP (watchdog got there first)"
else
  say_info "IP='$cur_ip' (not $MAC_IP) — applying manually"
  sudo -n ifconfig $MAC_IF inet $MAC_IP netmask $MAC_NETMASK 2>&1 | sed 's/^/    /'
  sleep 2
  cur_ip=$(ifconfig $MAC_IF 2>/dev/null | awk '/inet /{print $2; exit}')
  [ "$cur_ip" = "$MAC_IP" ] && say_ok "IP restored: $MAC_IP" || say_fail "IP did not take: '$cur_ip'"
fi
echo

# --- 4. bring Spark ports back up (originally UP) ---
echo "${C_CYAN}=== 4. Bring Spark ports back up (originally UP) ===${C_RST}"
if ping -c1 -W1000 $SPARK_CTRL >/dev/null 2>&1; then
  for p in $SPARK_IFS_ORIG_UP; do
    $SSH "sudo -n ip link set $p up" 2>/dev/null && say_ok "$p up" || say_warn "$p — failed"
  done
else
  say_warn "Spark unreachable on the control plane — bring the ports up manually on Spark"
fi
echo

# --- 5. connectivity check ---
echo "${C_CYAN}=== 5. Data-plane check ===${C_RST}"
sleep 3
if ifconfig $MAC_IF 2>/dev/null | grep -q "status: active"; then
  say_ok "en16 status: active"
else
  say_warn "en16 still inactive — link not up (Spark side not up?)"
fi
if ping -c3 -W2000 $SPARK_DATA >/dev/null 2>&1; then
  rt=$(ping -c3 -W2000 $SPARK_DATA | tail -1 | cut -d'=' -f2 | cut -d'/' -f2)
  say_ok "data-plane ping OK (~${rt} ms) — LINK RESTORED"
else
  say_fail "ping $SPARK_DATA fails — check Spark ports and IP"
fi
echo
echo "${C_GREEN}============================================================${C_RST}"
echo "  RESULT: $ok OK, $fail FAIL"
if [ "$fail" -eq 0 ]; then
  echo "${C_GREEN}  ✓ Card connected, link up${C_RST}"
else
  echo "${C_YEL}  ! Some checks failed — see above; some may need manual fixes${C_RST}"
fi
echo "${C_GREEN}============================================================${C_RST}"
