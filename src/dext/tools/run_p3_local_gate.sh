#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEXT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

"$DEXT_DIR/build/mlx_p3_local_gate" \
    -l "${MAC_ROCE_IP:-192.168.200.1}" -a "${MAC_ROCE_MAC:-98:03:9b:80:6a:94}"
