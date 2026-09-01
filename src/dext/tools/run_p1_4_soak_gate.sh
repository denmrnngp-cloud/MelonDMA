#!/bin/bash
# run_p1_4_soak_gate.sh — P1.4 long resource-lifecycle soak with counters.
#
# Requires the signed/activated DEXT on live hardware. No peer needed.
# SOAK_ITERATIONS overrides the default (100).
set -euo pipefail

cd "$(dirname "$0")/.."

if [[ ! -x build/mlx_soak_gate ]]; then
    echo "mlx_soak_gate not built — run: make p1-4-gate" >&2
    exit 2
fi

ITER=${SOAK_ITERATIONS:-100}
echo "=== P1.4 resource-counter soak (${ITER} cycles) ==="
exec ./build/mlx_soak_gate "$ITER"
