#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEXT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PEER="${SPARK_SSH:-192.168.100.2}"
IB_DEV="${SPARK_IB_DEV:-rocep1s0f1}"
LOCAL_IP="${MAC_ROCE_IP:-192.168.200.1}"
LOCAL_MAC="${MAC_ROCE_MAC:-98:03:9b:80:6a:94}"
BASE_PORT="${PHASE2_BASE_PORT:-$((19515 + ($$ % 1000)))}"

"$DEXT_DIR/build/mlx_phase2_gate" --preflight
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
HASH="$(shasum -a 256 "$SCRIPT_DIR/mlx_imm_peer.c" | awk '{print substr($1,1,16)}')"
REMOTE_SRC="/tmp/mlx-imm-peer-$HASH.c"
REMOTE_BIN="/tmp/mlx-imm-peer-$HASH"
if ! ssh "$PEER" "test -x '$REMOTE_BIN'"; then
    scp -q "$SCRIPT_DIR/mlx_imm_peer.c" "$PEER:$REMOTE_SRC"
    ssh "$PEER" "cc -std=gnu11 -O2 -Wall -Wextra -Werror '$REMOTE_SRC' -o '$REMOTE_BIN' -libverbs"
fi
PEER_MODE=""
LOCAL_MODE="--immediate"
if [[ "${R5_WRITE_IMMEDIATE:-0}" == 1 ]]; then
    PEER_MODE="-W"
    LOCAL_MODE="--write-immediate"
fi
ssh "$PEER" "exec '$REMOTE_BIN' $PEER_MODE -d '$IB_DEV' -i 1 -g '$GID_INDEX' -p '$BASE_PORT' -m '$MTU'" &
PEER_PID=$!
trap 'kill "$PEER_PID" 2>/dev/null || true; wait "$PEER_PID" 2>/dev/null || true' EXIT
sleep 1
"$DEXT_DIR/build/mlx_phase2_gate" "$LOCAL_MODE" -p "$BASE_PORT" -n 1 -s 64 -m "$MTU" \
    --local-ip "$LOCAL_IP" --local-mac "$LOCAL_MAC" --remote-mac "$REMOTE_MAC" \
    --timeout "${PHASE2_TIMEOUT:-30}" "${SPARK_CONTROL_HOST:-192.168.100.2}"
wait "$PEER_PID"
trap - EXIT
echo "R5_IMMEDIATE_GATE PASS: ${LOCAL_MODE#--} and CQE immediate-data decode"
