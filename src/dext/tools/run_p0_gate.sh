#!/bin/bash
# P0 production datapath matrix. Runs the existing live gates in dependency
# order and stops on the first failure.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

: "${P0_SEND_ITERS:=250000}"
: "${P0_LIFECYCLE_CYCLES:=10}"
: "${P0_WRITE_ITERS:=1024}"
: "${P0_WRITE_SIZE:=1048576}"
: "${P0_WINDOW:=16}"
: "${P0_TIMEOUT:=30}"
: "${P0_SIGNAL_ALL:=0}"
: "${P0_READ_ITERS:=1000000}"

run_send_matrix() {
    PHASE2_MTU=auto \
    PHASE2_LIFECYCLE_CYCLES="$P0_LIFECYCLE_CYCLES" \
    PHASE2_TIMEOUT="$P0_TIMEOUT" \
    PHASE2_SEND_ITERS="$P0_SEND_ITERS" \
        "$SCRIPT_DIR/run_phase2_gate.sh"
}

run_write_matrix() {
    PHASE5_MTUS=auto \
    PHASE5_WINDOWS="$P0_WINDOW" \
    PHASE5_WRITE_SIZE="$P0_WRITE_SIZE" \
    PHASE5_WRITE_ITERS="$P0_WRITE_ITERS" \
        "$SCRIPT_DIR/run_phase5_bulk_write_gate.sh"
}

run_write_signaling_matrix() {
    for signal in 0 1; do
        echo "=== P0.1/2 WRITE signaling=$signal windows=1 16 64 ==="
        PHASE3_SIGNAL_ALL="$signal" \
        PHASE5_MTUS=auto PHASE5_WINDOWS="1 16 64" \
        PHASE5_WRITE_SIZE="$P0_WRITE_SIZE" PHASE5_WRITE_ITERS=64 \
            "$SCRIPT_DIR/run_phase5_bulk_write_gate.sh"
    done
}

run_read_case() {
    local direction="$1" size="$2" window="$3" signal="$4" iters="$5"
    PHASE3_READ_ONLY=1 PHASE3_WRITE_ITERS="$iters" \
    PHASE3_WRITE_SIZE="$size" PHASE2_WINDOW="$window" \
    PHASE3_CQ_MODE=separate PHASE3_SIGNAL_ALL="$signal" P0_READ_MODE=1 \
    PHASE3_REVERSE_READ="$direction" \
        "$SCRIPT_DIR/run_phase2_gate.sh"
}

run_read_matrix() {
    for size in 1 64 256 1024 2048 4096; do
        for window in 1 16 64; do
            for signal in 0 1; do
                run_read_case 0 "$size" "$window" "$signal" 1024
                run_read_case 1 "$size" "$window" "$signal" 1024
            done
        done
    done
    run_read_case 0 4096 64 0 "$P0_READ_ITERS"
    run_read_case 1 4096 64 0 "$P0_READ_ITERS"
}

run_reverse_write() {
    PHASE3_REVERSE_WRITE_ONLY=1 \
    PHASE3_WRITE_SIZE="$P0_WRITE_SIZE" \
    PHASE3_WRITE_ITERS="$P0_WRITE_ITERS" \
    PHASE2_TIMEOUT="$P0_TIMEOUT" \
        "$SCRIPT_DIR/run_phase2_gate.sh"
}

run_rnr_gate() {
    PHASE2_RNR_GATE=1 PHASE2_TIMEOUT="$P0_TIMEOUT" \
        "$SCRIPT_DIR/run_phase2_gate.sh"
}

run_recovery_reconnect() {
    PHASE2_RECOVERY_GATE=1 PHASE2_SMOKE_ONLY=1 PHASE2_TIMEOUT="$P0_TIMEOUT" \
        "$SCRIPT_DIR/run_phase2_gate.sh"
    # The second session is the reconnect/post-recovery traffic proof. Both
    # sessions use fresh QP/CQ/MR and the same endpoint discovery path.
    PHASE2_SMOKE_ONLY=1 PHASE2_TIMEOUT="$P0_TIMEOUT" \
        "$SCRIPT_DIR/run_phase2_gate.sh"
}

echo '=== P0.1/1 SEND-RECV matrix + lifecycle ==='
run_send_matrix
echo '=== P0.1/2 forward RDMA WRITE matrix ==='
run_write_matrix
echo '=== P0.1/3 RDMA READ matrix ==='
run_read_matrix
echo '=== P0.1/4 reverse RDMA WRITE ==='
run_reverse_write
echo '=== P0.1/5 WRITE windows/signaling matrix ==='
run_write_signaling_matrix
echo '=== P0.1/6 RNR/retry negative gate ==='
run_rnr_gate
echo '=== P0.1/7 QP recovery + reconnect traffic ==='
run_recovery_reconnect

echo 'P0_GATE PASS: SEND/RECV, READ both directions, WRITE both directions, lifecycle, windows/signaling, RNR/retry fault injection, and recovery-reconnect gates passed.'
exit 0
