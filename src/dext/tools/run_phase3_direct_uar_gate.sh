#!/bin/bash
# P3 direct-UAR gate. Requires a live Mac <-> DGX Spark connection.
# This gate is intentionally stricter than the functional pipeline gate:
# a passing workload without direct SQ evidence is a failure.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LOG_FILE="${PHASE3_DIRECT_LOG:-/tmp/melon-phase3-direct-uar.log}"
: "${MELONDMA_DIRECT_UAR:=1}"
export MELONDMA_DIRECT_UAR

if [[ "$MELONDMA_DIRECT_UAR" != "1" ]]; then
    echo "P3_DIRECT_UAR FAIL: MELONDMA_DIRECT_UAR must be 1" >&2
    exit 2
fi

rm -f "$LOG_FILE"
set +e
MELONDMA_FAST_PATH=1 \
MELONDMA_DIRECT_UAR=1 \
PHASE2_MULTI_SGE_GATE=1 \
PHASE2_MULTI_SGE_COUNT="${PHASE2_MULTI_SGE_COUNT:-4}" \
PHASE2_SMOKE_ONLY=1 \
PHASE2_WINDOW=1 \
"$SCRIPT_DIR/run_phase2_gate.sh" 2>&1 | tee "$LOG_FILE"
status=${PIPESTATUS[0]}
set -e
if [[ "$status" != "0" ]]; then
    echo "P3_DIRECT_UAR FAIL: functional pipeline exited with $status" >&2
    exit "$status"
fi

# Userspace evidence proves the mapping and direct publication path selected.
for marker in "direct SQ mapped" "direct SQ synchronized SGE count=" "DIRECT_UAR_STATS"; do
    if ! grep -F "$marker" "$LOG_FILE" >/dev/null; then
        echo "P3_DIRECT_UAR FAIL: missing userspace evidence: $marker" >&2
        exit 20
    fi
done

stats_line="$(grep -F 'DIRECT_UAR_STATS' "$LOG_FILE" | tail -1)"
for field in 'mapped_qps=[1-9]' 'direct_wrs=[1-9]' 'direct_doorbells=[1-9]' \
             'direct_recv_wrs=[1-9]' 'direct_cq_consumers=[1-9]'; do
    if ! printf '%s\n' "$stats_line" | grep -E "$field" >/dev/null; then
        echo "P3_DIRECT_UAR FAIL: counter check failed: $field" >&2
        exit 21
    fi
done

echo "P3_DIRECT_UAR PASS: mapped per-QP SQ/RQ, synchronized metadata, direct BF/RQ/CQ doorbells"
