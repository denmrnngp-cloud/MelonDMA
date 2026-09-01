#!/bin/bash
# Phase 5.1 speed sweep. Reuses the proven Phase 3 pipeline workload and
# varies only outstanding WR depth; every result must come from live hardware.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR="${PHASE5_LOG_DIR:-/tmp/melon-p5-speed}"
mkdir -p "$OUT_DIR"

# The DEXT mailbox remains bounded at RDMA_POST_CHUNK (16); the gate splits
# larger user windows into safe chunks while measuring outstanding WR depth.
for window in ${PHASE5_WINDOWS:-16 64 256}; do
    log="$OUT_DIR/window-${window}.log"
    echo "=== Phase 5.1 speed: window=$window log=$log ==="
    if ! PHASE3_WINDOW="$window" "$SCRIPT_DIR/run_phase3_pipeline_gate.sh" 2>&1 | tee "$log"; then
        echo "PHASE5_SPEED FAIL: window=$window (see $log)" >&2
        exit 1
    fi
    grep -E 'PHASE3_PIPELINE PASS|PHASE2_GATE PASS' "$log"
done

echo "PHASE5_SPEED PASS: windows=${PHASE5_WINDOWS:-16 64 256} logs=$OUT_DIR"
