#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEXT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PEER="${SPARK_SSH:-192.168.100.2}"
IB_DEV="${SPARK_IB_DEV:-rocep1s0f1}"
PORT="${PHASE2_BASE_PORT:-$((20515 + ($$ % 1000)))}"
GID_INDEX="${SPARK_GID_INDEX:-2}"
MTU="$(ssh "$PEER" "ibv_devinfo -d '$IB_DEV' -i 1 | awk '/active_mtu:/ {print \$2; exit}' | tr -cd '0-9'")"
NDEV="$(ssh "$PEER" "cat /sys/class/infiniband/$IB_DEV/ports/1/gid_attrs/ndevs/$GID_INDEX")"
REMOTE_MAC="${SPARK_ROCE_MAC:-}"
if [[ -z "$REMOTE_MAC" ]]; then
    REMOTE_MAC="$(ssh "$PEER" "cat /sys/class/net/$NDEV/address")"
fi
HASH="$(shasum -a 256 "$SCRIPT_DIR/mlx_imm_peer.c" | awk '{print substr($1,1,16)}')"
SRC="/tmp/mlx-imm-peer-$HASH.c"
BIN="/tmp/mlx-imm-peer-$HASH"
if ! ssh "$PEER" "test -x '$BIN'"; then
    scp -q "$SCRIPT_DIR/mlx_imm_peer.c" "$PEER:$SRC"
    ssh "$PEER" "cc -std=gnu11 -O2 -Wall -Wextra -Werror '$SRC' -o '$BIN' -libverbs"
fi
ssh "$PEER" "exec '$BIN' -N -d '$IB_DEV' -i 1 -g '$GID_INDEX' -p '$PORT' -m '$MTU'" &
PID=$!
trap 'kill "$PID" 2>/dev/null || true; wait "$PID" 2>/dev/null || true' EXIT
sleep 1
"$DEXT_DIR/build/mlx_cq_event_gate" -h "${SPARK_CONTROL_HOST:-192.168.100.2}" -p "$PORT" -m "$MTU" \
    -l "${MAC_ROCE_IP:-192.168.200.1}" -a "${MAC_ROCE_MAC:-98:03:9b:80:6a:94}" -r "$REMOTE_MAC"
wait "$PID"
trap - EXIT
