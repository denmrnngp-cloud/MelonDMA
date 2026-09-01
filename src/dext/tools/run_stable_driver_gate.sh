#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

CYCLES=${STABLE_INIT_CYCLES:-2}
STAMP=$(date -u '+%Y%m%dT%H%M%SZ')
OUT_DIR="$ROOT/build/stable-driver-logs/$STAMP"
mkdir -p "$OUT_DIR"

make build/mlx_stable_gate >/dev/null

{
    echo "utc=$STAMP"
    echo "os=$(sw_vers -productVersion 2>/dev/null || true)"
    echo "boot_session=$(sysctl -n kern.bootsessionuuid 2>/dev/null || true)"
    echo "source_version=$(plutil -extract CFBundleVersion raw MlxRDMA.dext/Contents/Info.plist 2>/dev/null || true)"
    echo "source_build_tag=$(plutil -extract IOKitPersonalities.MlxPCIDriver.MlxBuildTag raw MlxRDMA.dext/Contents/Info.plist 2>/dev/null || true)"
    ioreg -r -c MlxPCIDriver -l 2>/dev/null \
        | grep -E 'MlxBuildTag|CFBundleVersion|IOUserClass' || true
} >"$OUT_DIR/metadata.txt"

START=$(date -u '+%Y-%m-%d %H:%M:%S')
set +e
"$ROOT/build/mlx_stable_gate" --cycles "$CYCLES" 2>&1 | tee "$OUT_DIR/gate.txt"
RC=${PIPESTATUS[0]}
set -e

/usr/bin/log show --start "$START" --style compact --info \
    --predicate 'process == "kernel" AND (eventMessage CONTAINS "STABLE_GATE" OR eventMessage CONTAINS "PAGE_REQUEST" OR eventMessage CONTAINS "PAGE_TAKE" OR eventMessage CONTAINS "INIT_HCA" OR eventMessage CONTAINS "TEARDOWN_HCA" OR eventMessage CONTAINS "VPORT" OR eventMessage CONTAINS "Phase2")' \
    >"$OUT_DIR/kernel.log" 2>&1 || true

if [ ! -s "$OUT_DIR/kernel.log" ]; then
    echo "Unified Log returned no matching DEXT records; gate.txt contains the firmware stage/opcode/status/syndrome report." \
        >"$OUT_DIR/kernel-log-note.txt"
fi

echo "Stable-driver evidence: $OUT_DIR"
if [ "$RC" -ne 0 ]; then
    echo "STABLE_DRIVER_GATE FAIL"
    exit "$RC"
fi
echo "STABLE_DRIVER_GATE PASS"
