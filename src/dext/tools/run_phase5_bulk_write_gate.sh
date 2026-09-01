#!/bin/bash
# Phase 5.1 adaptive bulk WRITE sweep.
# The peer's active MTU is authoritative; PHASE2_MTU=auto discovers it.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SIZE="${PHASE5_WRITE_SIZE:-1048576}"
ITERS="${PHASE5_WRITE_ITERS:-1024}"
WINDOWS="${PHASE5_WINDOWS:-16 64 256}"
MTUS="${PHASE5_MTUS:-auto}"

for mtu in $MTUS; do
    for window in $WINDOWS; do
        echo "=== Phase 5.1 bulk WRITE: mtu=$mtu window=$window size=$SIZE iters=$ITERS ==="
        PHASE2_MTU="$mtu" \
        PHASE3_WINDOW="$window" \
        PHASE3_WRITE_SIZE="$SIZE" \
        PHASE3_WRITE_ITERS="$ITERS" \
        MELONDMA_FAST_PATH="${MELONDMA_FAST_PATH:-1}" \
            "$SCRIPT_DIR/run_phase3_write_gate.sh"
    done
done

echo "PHASE5_BULK_WRITE PASS: mtus=$MTUS windows=$WINDOWS size=$SIZE iters=$ITERS"
