#!/bin/bash
# First Phase 3 hardware increment: bounded batched posting over the proven
# kernel-mediated path. Sixteen WRs cross per IORPC, the final WQE is signaled,
# and one UAR doorbell publishes the batch.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
export PHASE2_WINDOW="${PHASE3_WINDOW:-16}"
export PHASE3_PIPELINE_ONLY=1
exec "$SCRIPT_DIR/run_phase2_gate.sh"
