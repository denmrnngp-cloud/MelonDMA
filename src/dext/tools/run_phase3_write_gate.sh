#!/bin/bash
# Phase 3 bulk one-sided RDMA WRITE gate. The custom DGX peer exports a
# REMOTE_WRITE MR and validates its contents after the Mac observes CQEs.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
export PHASE2_WINDOW="${PHASE3_WINDOW:-16}"
export PHASE3_WRITE_ONLY=1
exec "$SCRIPT_DIR/run_phase2_gate.sh"
