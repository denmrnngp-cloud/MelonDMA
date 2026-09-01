#!/bin/bash
# P0.2 live isolation gate: ownership denial plus independent traffic while
# one client's complete UserClient is torn down, followed by surviving traffic.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PHASE2="${PHASE2_GATE_SCRIPT:-$SCRIPT_DIR/run_phase2_gate.sh}"
BASE="${P0_2_BASE_PORT:-$((20500 + ($$ % 300)))}"
TIMEOUT="${P0_2_TIMEOUT:-120}"
A_LOG="${TMPDIR:-/tmp}/p0.2-a.$$"
B_LOG="${TMPDIR:-/tmp}/p0.2-b.$$"
A= B=
cleanup() {
    [[ -n "$A" ]] && kill "$A" 2>/dev/null || true
    [[ -n "$B" ]] && kill "$B" 2>/dev/null || true
    wait "$A" 2>/dev/null || true
    wait "$B" 2>/dev/null || true
    rm -f "$A_LOG" "$B_LOG"
}
trap cleanup EXIT

"$SCRIPT_DIR/../build/mlx_isolation_gate"

run_long_traffic() {
    local base="$1"
    PHASE2_SEND_ITERS="${P0_2_SEND_ITERS:-100000}" \
    PHASE2_LIFECYCLE_CYCLES="${P0_2_LIFECYCLE_CYCLES:-10}" \
    PHASE2_TIMEOUT="$TIMEOUT" PHASE2_BASE_PORT="$base" "$PHASE2"
}

# Both processes own separate UserClients and run real traffic concurrently.
run_long_traffic "$BASE" >"$A_LOG" 2>&1 & A=$!
run_long_traffic "$((BASE + 100))" >"$B_LOG" 2>&1 & B=$!
sleep 2
kill "$A" 2>/dev/null || true
wait "$A" 2>/dev/null || true
A=

# B must continue and complete its remaining lifecycle traffic after A's
# UserClient has been torn down. A short run that already finished before the
# teardown is not accepted as proof.
if ! wait "$B"; then
    echo "P0.2_ISOLATION_GATE FAIL: surviving client B traffic failed" >&2
    cat "$B_LOG" >&2
    exit 1
fi
B=

echo "P0.2_ISOLATION_GATE PASS: ownership denial, concurrent independent traffic, client A teardown, and surviving client B post-teardown traffic"
