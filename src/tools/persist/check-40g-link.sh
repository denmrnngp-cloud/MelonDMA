#!/bin/bash
# Health-check of the 40G link Mac <-> Spark. Run with no arguments, no sudo.
# Checks exactly what should survive a reboot.
# The network service name is derived automatically from the device name (en16),
# so it survives renaming in System Settings.
#
# Peer connection is configurable via SPARK_SSH (ssh target, e.g. user@host)
# and SPARK_SSH_KEY (optional path to a private key).
SPARK_IP=192.168.200.2
IFACE=en16
SSH_HOST="${SPARK_SSH:-$SPARK_IP}"
SSH_KEY="${SPARK_SSH_KEY:-}"
SSH="ssh -o ConnectTimeout=6"
[ -n "$SSH_KEY" ] && SSH="$SSH -i $SSH_KEY"
SSH="$SSH $SSH_HOST"
ok=0; fail=0
chk() { # chk "description" "actual" "expected(substring)"
  if [[ "$2" == *"$3"* ]]; then printf "  \033[32mOK\033[0m   %-34s %s\n" "$1" "$2"; ((ok++))
  else printf "  \033[31mFAIL\033[0m %-34s %s (expected: %s)\n" "$1" "$2" "$3"; ((fail++)); fi
}

# Derive the network service name automatically from the device name (en16)
# rather than hardcoding it — survives service renaming in System Settings.
SVC_NAME=$(networksetup -listnetworkserviceorder 2>/dev/null | awk '
  /^\([0-9]+\) /{name=$0; sub(/^\([0-9]+\) /, "", name)}
  /Device: en16/{print name; exit}')

echo "== Mac ($IFACE, service: $SVC_NAME) =="
chk "IP"          "$(ifconfig $IFACE 2>/dev/null | awk '/inet /{print $2; exit}')"        "192.168.200.1"
chk "MTU"         "$(ifconfig $IFACE 2>/dev/null | grep -o 'mtu [0-9]*')"           "mtu 2034"
chk "Status"      "$(ifconfig $IFACE 2>/dev/null | awk '/status:/{print $2}')"      "active"
chk "Speed"       "$(ifconfig $IFACE 2>/dev/null | grep -o '40Gbase-CR4')"          "40Gbase-CR4"
chk "Service name" "$(echo "$SVC_NAME" | grep -oE 'Mellanox|ConnectX')"             "Mellanox"
chk "ConfigMethod" "$(networksetup -getinfo "$SVC_NAME" 2>/dev/null | head -1)"        "Manual"
chk "win_scale"   "$(sysctl -n net.inet.tcp.win_scale_factor)"                      "8"
chk "delayed_ack" "$(sysctl -n net.inet.tcp.delayed_ack)"                           "0"
chk "sendspace"   "$(sysctl -n net.inet.tcp.sendspace)"                             "2097152"
# The launchd unit runs in the Root domain, which needs sudo; the sudo cache
# may have expired. Check via the log file (created by the unit and world
# readable) instead of `sudo -n`. The watchdog writes «watchdog started» /
# «sysctl applied» / «restored OK» / «en16 IP is» — any of them is valid.
chk "launchd"    "$(tail -3 /var/log/rdma-tune40g.log 2>/dev/null | grep -qE 'watchdog started|sysctl applied|restored OK|en16 IP is' && echo yes)" "yes"
chk "launchd plist" "$(test -f /Library/LaunchDaemons/com.rdma.tune40g.plist && echo yes)" "yes"
# No leftover APIPA 169.254 on en16 (the watchdog may not have cleaned it up).
chk "no APIPA"   "$(ifconfig $IFACE 2>/dev/null | grep -c 'inet 169.254')"                   "0"

echo "== Connectivity =="
if ping -c2 -W2000 $SPARK_IP >/dev/null 2>&1; then
  chk "ping" "$(ping -c3 -W2000 $SPARK_IP | tail -1 | cut -d'=' -f2 | cut -d'/' -f2) ms avg" "ms"
else
  printf "  \033[31mFAIL\033[0m %-34s unreachable\n" "ping"; ((fail++))
  echo; echo "Spark does not answer on 40G. Result: $ok OK, $fail FAIL"; exit 1
fi

echo "== Spark (enp1s0f0np0) =="
chk "IP"        "$($SSH "ip -br a show enp1s0f0np0" 2>/dev/null | awk '{print $3}')"                 "192.168.200.2/30"
chk "MTU"       "$($SSH "ip link show enp1s0f0np0" 2>/dev/null | grep -o 'mtu [0-9]*')"              "mtu 2034"
chk "Link"      "$($SSH "ethtool enp1s0f0np0 2>/dev/null | grep Speed" 2>/dev/null | xargs)"         "40000Mb/s"
chk "nmcli"     "$($SSH "nmcli -t -f NAME,STATE con show --active 2>/dev/null | grep rdma" 2>/dev/null)" "activated"
chk "ring rx"   "$($SSH "ethtool -g enp1s0f0np0 2>/dev/null | sed -n '/Current hardware/,\$p' | awk '/^RX:/{print \$2}'" 2>/dev/null)" "8192"
chk "systemd"   "$($SSH "systemctl is-enabled rdma-ring-tune 2>/dev/null" 2>/dev/null)"              "enabled"
chk "backlog"   "$($SSH "sysctl -n net.core.netdev_max_backlog" 2>/dev/null)"                        "250000"

echo
if [[ $fail -eq 0 ]]; then echo -e "\033[32mAll good: $ok checks passed.\033[0m"
else echo -e "\033[31mProblems: $ok OK, $fail FAIL.\033[0m"; fi

echo
echo "Speed measurement (expect ~15 Gbit/s):"
echo "  ssh -i ${SSH_KEY:-<key>} $SSH_HOST '(setsid iperf3 -s -B $SPARK_IP </dev/null >/dev/null 2>&1 &)'"
echo "  iperf3 -c $SPARK_IP -t 10 -w 2M -l 1M"
