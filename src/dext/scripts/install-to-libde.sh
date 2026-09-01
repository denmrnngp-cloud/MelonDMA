#!/bin/bash
# Places the dext in /Library/DriverExtensions — kernelmanagerd reads this
# directory at boot if DextRecordTable is empty (our workaround for loading personas
# BEFORE card matching). Run via sudo!
set -e
STAGED=$(ls -d /Library/SystemExtensions/*/com.mlx5.rdma.dext.systemextension 2>/dev/null | tail -1)
[ -z "$STAGED" ] && { echo "active dext not found in /Library/SystemExtensions"; exit 1; }
echo "source: $STAGED"
mkdir -p /Library/DriverExtensions
rm -rf /Library/DriverExtensions/com.mlx5.rdma.dext.systemextension
ditto "$STAGED" /Library/DriverExtensions/com.mlx5.rdma.dext.systemextension
echo OK; ls -la /Library/DriverExtensions/
