#!/bin/bash
# gate-p1-check.sh - post-reboot verification: activate MlxRDMA.dext and look
# for the Gate P1 log line (MMIO read of fw_rev) from MlxPCIDriver::Start.
set -u
APP=/Applications/MlxRDMA.app
BIN=$APP/Contents/MacOS/mlx_activate

echo "=== developer mode: $(systemextensionsctl developer 2>/dev/null) ==="
echo "=== activating (30s window, approve in System Settings if asked) ==="
$BIN & PID=$!
sleep 30
kill $PID 2>/dev/null

echo "=== installed extensions ==="
systemextensionsctl list | grep -v tailscale

echo "=== MlxRDMA / driver log (last 2 min) ==="
log show --last 2m --style compact --predicate \
  'composedMessage CONTAINS "MlxPCIDriver" OR composedMessage CONTAINS "MlxRDMA"' 2>/dev/null \
  | grep -iv "xpc:connection\|Security\)" | tail -40
