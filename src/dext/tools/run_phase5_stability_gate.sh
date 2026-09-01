#!/bin/bash
# Phase 5.1 stability gate for the current 21 Gbit/s baseline.
# Each iteration creates and tears down a fresh QP/MR, so resource leaks and
# intermittent RoCE errors are visible instead of hidden by one long run.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ITERATIONS="${PHASE5_STABILITY_ITERS:-10}"
WRITE_ITERS="${PHASE5_STABILITY_WRITE_ITERS:-1024}"
WINDOW="${PHASE5_STABILITY_WINDOW:-256}"
SIZE="${PHASE5_STABILITY_SIZE:-1048576}"
MIN_GBIT="${PHASE5_STABILITY_MIN_GBIT:-19.0}"
LOG_DIR="${PHASE5_STABILITY_LOG_DIR:-/tmp/melon-p5-stability}"
mkdir -p "$LOG_DIR"

: >"$LOG_DIR/results.tsv"
for ((i = 1; i <= ITERATIONS; i++)); do
    log="$LOG_DIR/run-$i.log"
    echo "=== P5 stability $i/$ITERATIONS ==="
    if ! PHASE5_MTUS=auto PHASE5_WINDOWS="$WINDOW" \
        PHASE5_WRITE_SIZE="$SIZE" PHASE5_WRITE_ITERS="$WRITE_ITERS" \
        PHASE5_DIAGNOSTICS=1 MELONDMA_FAST_PATH=1 \
        MELONDMA_DIRECT_UAR=1 MELONDMA_POST_BATCH=64 \
        "$SCRIPT_DIR/run_phase5_bulk_write_gate.sh" >"$log" 2>&1; then
        cat "$log"
        echo "PHASE5_STABILITY FAIL: run=$i (see $log)" >&2
        exit 1
    fi
    line="$(grep -F 'PHASE3_WRITE PASS:' "$log" | tail -1)"
    speed="$(printf '%s\n' "$line" | awk '{for (i=1; i<=NF; i++) if ($i == "Gbit/s;") {print $(i-1); exit}}')"
    if [[ -z "$speed" ]] || ! awk -v speed="$speed" -v min="$MIN_GBIT" 'BEGIN {exit !(speed >= min)}'; then
        cat "$log"
        echo "PHASE5_STABILITY FAIL: run=$i throughput=${speed:-unknown} minimum=$MIN_GBIT" >&2
        exit 1
    fi
    printf '%s\t%s\n' "$i" "$speed" | tee -a "$LOG_DIR/results.tsv"
done

awk '{sum += $2; if (min == "" || $2 < min) min = $2; if ($2 > max) max = $2}
     END {printf "PHASE5_STABILITY PASS: runs=%d min=%.2f avg=%.2f max=%.2f Gbit/s logs=%s\n", NR, min, sum/NR, max, "'"$LOG_DIR"'"}' "$LOG_DIR/results.tsv"
