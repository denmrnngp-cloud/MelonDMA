#!/bin/bash
# P0.3 lifetime / stale-handle hardening gate. Self-contained (no remote peer):
# stale-token denial after destroy, generation bump on ID reuse, cross-client
# token isolation, and in-flight QP destroy busy.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEXT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
GATE_BIN="${P0_3_GATE_BIN:-$DEXT_DIR/build/mlx_lifetime_gate}"

# Precondition: an active DEXT owns the card. --check refuses a stuck swap or a
# non-owner so the gate can never pass against a half-loaded driver.
"$DEXT_DIR/scripts/mlx_hot_update.sh" --check >/dev/null 2>&1 || {
    echo "P0.3_LIFETIME_GATE FAIL: DEXT not healthy (run scripts/mlx_hot_update.sh --check)" >&2
    exit 1
}

if [[ ! -x "$GATE_BIN" ]]; then
    echo "P0.3_LIFETIME_GATE FAIL: $GATE_BIN not built" >&2
    exit 1
fi

if "$GATE_BIN"; then
    echo "P0.3_LIFETIME_GATE PASS"
else
    echo "P0.3_LIFETIME_GATE FAIL" >&2
    exit 1
fi
