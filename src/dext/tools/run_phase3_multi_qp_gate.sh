#!/bin/bash
# Phase 3 multi-QP scaling gate. Each lane is a separate IOUserClient with an
# independent GID slot, PD, MR, CQ, QP and DGX peer. A filesystem barrier is
# released only after every RC QP reached RTS and received its remote MR.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BASE_PORT="${PHASE3_MULTI_BASE_PORT:-18700}"
SIZE="${PHASE3_MULTI_SIZE:-1048576}"
ITERS="${PHASE3_MULTI_ITERS:-1024}"
WINDOW="${PHASE3_WINDOW:-16}"
LEVELS="${PHASE3_MULTI_LEVELS:-1 2 4 8}"
READY_TIMEOUT="${PHASE3_MULTI_READY_TIMEOUT:-60}"

run_level() {
    local lanes="$1" stage="$2"
    local work_dir
    work_dir="$(mktemp -d "/tmp/melon-p3-${lanes}qp.XXXXXX")"
    local start_file="$work_dir/start"
    local pids=()
    echo "=== Phase 3 multi-QP: qps=$lanes size=$SIZE iters/qp=$ITERS window=$WINDOW ==="
    for ((lane = 0; lane < lanes; lane++)); do
        PHASE2_BASE_PORT=$((BASE_PORT + stage * 16 + lane)) \
        PHASE3_WINDOW="$WINDOW" PHASE3_WRITE_SIZE="$SIZE" \
        PHASE3_WRITE_ITERS="$ITERS" PHASE2_TIMEOUT="$READY_TIMEOUT" \
        PHASE3_START_FILE="$start_file" \
        PHASE3_READY_FILE="$work_dir/ready.$lane" \
            "$SCRIPT_DIR/run_phase3_write_gate.sh" \
            >"$work_dir/lane.$lane.log" 2>&1 &
        pids+=("$!")
    done

    local deadline=$((SECONDS + READY_TIMEOUT)) ready=0
    while (( SECONDS < deadline )); do
        ready=0
        for ((lane = 0; lane < lanes; lane++)); do
            [[ -f "$work_dir/ready.$lane" ]] && ready=$((ready + 1))
        done
        (( ready == lanes )) && break
        for ((lane = 0; lane < lanes; lane++)); do
            if ! kill -0 "${pids[$lane]}" 2>/dev/null; then
                echo "multi-QP lane $lane exited before the start barrier" >&2
                cat "$work_dir/lane.$lane.log" >&2
                return 1
            fi
        done
        sleep 0.05
    done
    if (( ready != lanes )); then
        echo "multi-QP ready timeout: $ready/$lanes lanes" >&2
        return 1
    fi

    local start_ns end_ns level_rc=0
    start_ns="$(python3 -c 'import time; print(time.monotonic_ns())')"
    : >"$start_file"
    for pid in "${pids[@]}"; do wait "$pid" || level_rc=1; done
    end_ns="$(python3 -c 'import time; print(time.monotonic_ns())')"

    for ((lane = 0; lane < lanes; lane++)); do
        grep -E 'PHASE3_WRITE PASS|MLX_WRITE_PEER PASS|PHASE3_WRITE_GATE PASS' \
            "$work_dir/lane.$lane.log" || level_rc=1
    done
    if (( level_rc != 0 )); then
        echo "PHASE3_MULTI_QP FAIL: qps=$lanes logs=$work_dir" >&2
        return 1
    fi
    awk -v bytes="$((lanes * ITERS * SIZE))" -v start="$start_ns" -v end="$end_ns" \
        -v qps="$lanes" 'BEGIN {
            seconds=(end-start)/1000000000.0;
            printf("PHASE3_MULTI_QP PASS: qps=%d bytes=%d elapsed=%.3fs aggregate=%.2f Gbit/s\n",
                   qps, bytes, seconds, bytes*8.0/seconds/1000000000.0);
        }'
}

stage=0
for lanes in $LEVELS; do
    case "$lanes" in 1|2|4|8) ;; *) echo "invalid QP level: $lanes" >&2; exit 2;; esac
    run_level "$lanes" "$stage"
    stage=$((stage + 1))
done
echo "PHASE3_MULTI_QP_GATE PASS: levels=$LEVELS"
