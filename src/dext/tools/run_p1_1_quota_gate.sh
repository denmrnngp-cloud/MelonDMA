#!/bin/bash
# run_p1_1_quota_gate.sh — P1.1 per-client quota live gate.
#
# Requires the signed/activated DEXT on live hardware (ConnectX-4 Lx). The
# binary is codesigned with tools/reinit.entitlements (userclient-access).
set -euo pipefail

cd "$(dirname "$0")/.."

if [[ ! -x build/mlx_quota_gate ]]; then
    echo "mlx_quota_gate not built — run: make p1-1-gate" >&2
    exit 2
fi

echo "=== P1.1 per-client quota gate ==="
exec ./build/mlx_quota_gate
