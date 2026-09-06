#!/bin/bash
# Collect reproducible release evidence; does not install, reset, unplug, or FLR.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/build/release-evidence/$(date -u +%Y%m%dT%H%M%SZ)}
mkdir -p "$OUT"
cd "$ROOT"
run() { name=$1; shift; "$@" >"$OUT/$name.log" 2>&1; echo "$name=$?" >>"$OUT/RESULTS"; }
plutil -p MlxRDMA.dext/Contents/Info.plist >"$OUT/source-info.plist.txt" 2>&1 || true
plutil -p MlxRDMA.dext/Contents/MlxRDMA.entitlements >"$OUT/source-entitlements.plist.txt" 2>&1 || true
sw_vers >"$OUT/sw_vers.txt" 2>&1 || true
system_profiler SPHardwareDataType SPDisplaysDataType >"$OUT/system-profiler.txt" 2>&1 || true
uname -a >"$OUT/uname.txt" 2>&1 || true
ioreg -r -c MlxPCIDriver -l >"$OUT/ioreg-driver.txt" 2>&1 || true
systemextensionsctl list >"$OUT/systemextensions.txt" 2>&1 || true
firmware=$(/usr/sbin/system_profiler SPPCIDataType 2>/dev/null || true)
printf '%s\n' "$firmware" >"$OUT/pci.txt"
ifconfig >"$OUT/ifconfig.txt" 2>&1 || true
run host-check make check-host
run dext-check make check-dext
run metal-contract make check-metal-contract
run quota ./tools/run_p1_1_quota_gate.sh
run direct-uar env SPARK_SSH="${SPARK_SSH:-denis-local@192.168.100.2}" SPARK_CONTROL_HOST="${SPARK_CONTROL_HOST:-192.168.100.2}" SPARK_GID_INDEX="${SPARK_GID_INDEX:-2}" ./tools/run_phase3_direct_uar_gate.sh
run metal-live ./build/mlx_metal_contract_live
printf 'captured_utc=%s\npeer=%s\npeer_control=%s\nlocal_gid_index=%s\n' \
  "$(date -u +%FT%TZ)" "${SPARK_SSH:-denis-local@192.168.100.2}" \
  "${SPARK_CONTROL_HOST:-192.168.100.2}" "${SPARK_GID_INDEX:-2}" >"$OUT/CONTEXT.txt"
find "$OUT" -type f -print0 | sort -z | xargs -0 shasum -a 256 >"$OUT/SHA256SUMS"
echo "EVIDENCE_COLLECTED: $OUT"
