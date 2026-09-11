#!/bin/bash
# Places a flat .dext bundle in /Library/DriverExtensions so kernelmanagerd's
# boot-time guess-scan can find it when DextRecordTable is empty (the fallback
# that loads our persona BEFORE card matching, so it wins the first match with
# IOProbeScore 5000 vs Apple's 1000). Run via sudo.
#
# kernelmanagerd only recognises the FLAT DriverKit bundle layout here —
# Info.plist and the executable at the bundle root (like Apple's own dexts in
# /System/Library/DriverExtensions/*.dext) — NOT the Contents/ layout of the
# activated .systemextension. This script flattens a Contents/ bundle into that
# layout.
#
# Usage:
#   sudo install-to-libde.sh                          # source = active .systemextension
#   sudo install-to-libde.sh build/MlxRDMA.dext       # source = build product (Contents/ layout)
set -e

SRC="${1:-}"
if [ -z "$SRC" ]; then
    SRC=$(ls -d /Library/SystemExtensions/*/com.melondma.rdma.dext.systemextension 2>/dev/null | tail -1)
fi
[ -n "$SRC" ] || { echo "source bundle not found"; exit 1; }
[ -f "$SRC/Contents/Info.plist" ] || { echo "not a Contents/ bundle: $SRC"; exit 1; }
echo "source: $SRC"

EXEC_NAME=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$SRC/Contents/Info.plist" 2>/dev/null || true)
[ -n "$EXEC_NAME" ] || { echo "cannot read CFBundleExecutable"; exit 1; }
[ -f "$SRC/Contents/MacOS/$EXEC_NAME" ] || { echo "executable missing: $SRC/Contents/MacOS/$EXEC_NAME"; exit 1; }

DEST=/Library/DriverExtensions/com.melondma.rdma.dext.dext
mkdir -p /Library/DriverExtensions
rm -rf "$DEST"
mkdir -p "$DEST"

cp "$SRC/Contents/Info.plist" "$DEST/Info.plist"
cp "$SRC/Contents/MacOS/$EXEC_NAME" "$DEST/$EXEC_NAME"
if [ -d "$SRC/Contents/Resources" ]; then
    cp -R "$SRC/Contents/Resources" "$DEST/"
fi

echo OK; ls -la /Library/DriverExtensions/ "$DEST/"
