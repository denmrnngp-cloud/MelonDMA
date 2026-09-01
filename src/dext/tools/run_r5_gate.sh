#!/bin/bash
# One command for the R5 contract. The optional live phase requires the
# currently built DEXT to be activated and a reachable DGX Spark peer.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEXT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

"$DEXT_DIR/build/mlx_phase2_gate" --preflight
"$DEXT_DIR/build/mlx_shim_tests"

if [[ "${R5_LIVE:-0}" != "1" ]]; then
    echo "R5_GATE PASS: host ABI/encoder/shim/verbs checks and live-DIEXT preflight"
    exit 0
fi

# A real peer is required for transport semantics; the phase gate already
# owns endpoint discovery, GID/MAC setup, and remote process cleanup.
PHASE2_SMOKE_ONLY=1 "$SCRIPT_DIR/run_phase2_gate.sh"
for sge_count in 2 3 4; do
    PHASE2_MULTI_SGE_GATE=1 PHASE2_MULTI_SGE_COUNT="$sge_count" \
        PHASE2_SMOKE_ONLY=1 "$SCRIPT_DIR/run_phase2_gate.sh"
done
PHASE2_LOCAL_INV_GATE=1 PHASE2_SMOKE_ONLY=1 "$SCRIPT_DIR/run_phase2_gate.sh"
for immediate_mode in send write; do
    if [[ "$immediate_mode" == send ]]; then
        "$SCRIPT_DIR/run_immediate_gate.sh"
    else
        R5_WRITE_IMMEDIATE=1 "$SCRIPT_DIR/run_immediate_gate.sh"
    fi
done
"$SCRIPT_DIR/run_cq_event_gate.sh"
MELONDMA_DEBUG_POST=1 MELONDMA_DIRECT_UAR=1 "$SCRIPT_DIR/run_phase3_direct_uar_gate.sh"
PHASE3_WRITE_ONLY=1 PHASE3_WRITE_ITERS="${R5_WRITE_ITERS:-16}" \
    PHASE3_WRITE_SIZE="${R5_WRITE_SIZE:-4096}" "$SCRIPT_DIR/run_phase2_gate.sh"

echo "R5_GATE PASS: host contract plus live direct 2-4 SGE SEND/RECV, direct immediate operations, CQ events, and WRITE"
