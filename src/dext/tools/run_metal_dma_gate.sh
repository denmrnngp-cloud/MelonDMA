#!/bin/bash
# End-to-end Apple-Silicon shared-MTLBuffer RDMA acceptance.
# The peer protocol is ordinary RC/RoCEv2, so the same path works with a
# Linux/NVIDIA Spark or another Mac running the compatible MelonDMA peer.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEXT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

export PHASE2_GATE_BIN="${PHASE2_GATE_BIN:-$DEXT_DIR/build/mlx_metal_dma_gate}"
export SPARK_SSH="${SPARK_SSH:-denis-local@192.168.100.2}"
export PHASE2_MTU="${PHASE2_MTU:-4096}"
export MLX_GATE_PAGE_ALIGNED_MR=1
export MELONDMA_FAST_PATH="${MELONDMA_FAST_PATH:-1}"
export MELONDMA_DIRECT_UAR="${MELONDMA_DIRECT_UAR:-1}"
export MELONDMA_DIRECT_CQ="${MELONDMA_DIRECT_CQ:-1}"

if [[ ! -x "$PHASE2_GATE_BIN" ]]; then
    echo "Metal DMA gate binary missing; run: make -C '$DEXT_DIR' metal-dma-gate" >&2
    exit 2
fi

echo "=== Metal UMA preflight: active DEXT feature contract ==="
"$PHASE2_GATE_BIN" --preflight

echo "=== Metal GPU -> NIC -> Spark (one-sided WRITE, no CPU staging copy) ==="
PHASE3_WRITE_ONLY=1 \
PHASE3_WRITE_SIZE="${METAL_DMA_SIZE:-1048576}" \
PHASE3_WRITE_ITERS="${METAL_DMA_ITERS:-32}" \
PHASE3_WINDOW="${METAL_DMA_WINDOW:-16}" \
    "$SCRIPT_DIR/run_phase2_gate.sh"

echo "=== Spark -> NIC -> Metal GPU (one-sided reverse WRITE) ==="
PHASE3_REVERSE_WRITE_ONLY=1 \
PHASE3_WRITE_SIZE="${METAL_DMA_SIZE:-1048576}" \
PHASE3_WRITE_ITERS="${METAL_DMA_ITERS:-32}" \
PHASE3_WINDOW="${METAL_DMA_WINDOW:-16}" \
    "$SCRIPT_DIR/run_phase2_gate.sh"

echo "=== Spark -> large indirect Metal MR -> GPU (>1.875 MiB) ==="
PHASE3_REVERSE_WRITE_ONLY=1 \
PHASE2_INDIRECT_MR=1 \
PHASE3_WRITE_SIZE="${METAL_DMA_LARGE_SIZE:-4194304}" \
PHASE3_WRITE_ITERS=1 \
PHASE3_WINDOW=1 \
    "$SCRIPT_DIR/run_phase2_gate.sh"

echo "METAL_DMA_GATE PASS: coherent shared/untracked MTLBuffer, GPU producer, GPU consumer, direct and indirect MR"
