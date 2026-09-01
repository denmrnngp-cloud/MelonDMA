#!/bin/bash
# gate-p1-post-reboot.sh — after reboot: dext activated, the persona should have
# gone into the kernel EARLY (from the /Library/DriverExtensions guess-scan when the
# DextRecordTable was empty) and won the first card match (score 5000 vs 1000).
echo "=== extensions ==="
systemextensionsctl list | grep mlx5

echo "=== IOUserServer process ==="
ps aux | grep "com.mlx5.rdma.dext" | grep -v grep

echo "=== ioreg: who owns ethernet@0 ==="
ioreg -r -n ethernet@0 | grep -E "\+-o" | head -6

echo "=== boot: DextRecordTable guess-scan ==="
log show --predicate 'process == "kernelmanagerd"' --info --style compact 2>/dev/null \
  | grep -iE "DextRecordTable was empty|No dexts were found|com.mlx5.rdma.dext.*state: loaded" | head -8

echo "=== Gate P1 log (MlxPCIDriver console) ==="
log show --last 10m --style compact --info --predicate \
  'composedMessage CONTAINS "MlxPCIDriver"' 2>/dev/null \
  | grep -viE "log run|sysextd" | tail -30
