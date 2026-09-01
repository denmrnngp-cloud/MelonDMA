#!/bin/bash
set -euo pipefail

# Live P3 gate: inline SEND + RC atomics + SL + solicited_only CQ arming.
# Runs the stock-libverbs Linux peer on the DGX Spark and the MelonDMA gate
# on this Mac. Same discovery conventions as run_immediate_gate.sh.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEXT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PEER="${SPARK_SSH:-192.168.100.2}"
IB_DEV="${SPARK_IB_DEV:-rocep1s0f1}"
LOCAL_IP="${MAC_ROCE_IP:-192.168.200.1}"
LOCAL_MAC="${MAC_ROCE_MAC:-98:03:9b:80:6a:94}"
BASE_PORT="${P3_BASE_PORT:-$((21515 + ($$ % 1000)))}"

GID_INDEX="${SPARK_GID_INDEX:-auto}"
if [[ "$GID_INDEX" == auto ]]; then
    GID_INDEX="$(ssh "$PEER" "grep -l 'ffff:c0a8:c802' /sys/class/infiniband/$IB_DEV/ports/1/gids/* | head -1 | awk -F/ '{print \$NF}'")"
fi
[[ "$GID_INDEX" =~ ^[0-9]+$ ]] || { echo "cannot find DGX IPv4 RoCE GID" >&2; exit 1; }
MTU="$(ssh "$PEER" "ibv_devinfo -d '$IB_DEV' -i 1 | awk '/active_mtu:/ {print \$2; exit}' | tr -cd '0-9'")"
[[ "$MTU" =~ ^(256|512|1024|2048|4096)$ ]] || { echo "invalid DGX MTU: $MTU" >&2; exit 1; }
REMOTE_MAC="${SPARK_ROCE_MAC:-}"
if [[ -z "$REMOTE_MAC" ]]; then
    REMOTE_NDEV="$(ssh "$PEER" "cat /sys/class/infiniband/$IB_DEV/ports/1/gid_attrs/ndevs/$GID_INDEX")"
    REMOTE_MAC="$(ssh "$PEER" "cat /sys/class/net/$REMOTE_NDEV/address")"
fi

HASH="$(shasum -a 256 "$SCRIPT_DIR/mlx_p3_peer.c" | awk '{print substr($1,1,16)}')"
REMOTE_SRC="/tmp/mlx-p3-peer-$HASH.c"
REMOTE_BIN="/tmp/mlx-p3-peer-$HASH"
if ! ssh "$PEER" "test -x '$REMOTE_BIN'"; then
    scp -q "$SCRIPT_DIR/mlx_p3_peer.c" "$PEER:$REMOTE_SRC"
    ssh "$PEER" "cc -std=gnu11 -O2 -Wall -Wextra -Werror '$REMOTE_SRC' -o '$REMOTE_BIN' -libverbs"
fi

ssh "$PEER" "exec '$REMOTE_BIN' -d '$IB_DEV' -i 1 -g '$GID_INDEX' -p '$BASE_PORT' -m '$MTU'" &
PEER_PID=$!
trap 'kill "$PEER_PID" 2>/dev/null || true; wait "$PEER_PID" 2>/dev/null || true' EXIT
sleep 1

"$DEXT_DIR/build/mlx_p3_gate" -h "${SPARK_CONTROL_HOST:-192.168.100.2}" -p "$BASE_PORT" -m "$MTU" \
    -l "$LOCAL_IP" -a "$LOCAL_MAC" -r "$REMOTE_MAC"
GATE_RC=$?
wait "$PEER_PID"
trap - EXIT
[[ $GATE_RC -eq 0 ]] || { echo "P3_GATE FAILED (gate rc=$GATE_RC)" >&2; exit $GATE_RC; }
echo "P3_GATE PASS: inline + atomics + SL + solicited_only CQ arming"
