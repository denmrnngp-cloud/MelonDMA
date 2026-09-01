#!/bin/bash
# run_p2_2_abi_fuzz_gate.sh — P2.2 ABI fuzz/property live gate.
#
# Requires the signed/activated DEXT on live hardware (ConnectX-4 Lx). The
# binary is codesigned with tools/reinit.entitlements (userclient-access).
# Every check is a malformed request that must be refused cleanly.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEXT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
GATE_BIN="${P2_2_GATE_BIN:-$DEXT_DIR/build/mlx_abi_fuzz_gate}"

if [[ ! -x "$GATE_BIN" ]]; then
    echo "P2.2_ABI_FUZZ FAIL: $GATE_BIN not built (run: make p2-2-gate)" >&2
    exit 1
fi

echo "=== P2.2 ABI fuzz/property gate ==="
if "$GATE_BIN"; then
    echo "P2.2_ABI_FUZZ PASS"
else
    echo "P2.2_ABI_FUZZ FAIL" >&2
    exit 1
fi
