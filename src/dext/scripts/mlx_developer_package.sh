#!/bin/bash
# Build a local developer-mode package. This is intentionally unsigned for
# the DEXT payload and is not a production/notarization artifact.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
DIST_DIR=${DIST_DIR:-$ROOT/dist/developer}
rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR/flat/com.mlx5.rdma.dext.dext"
make dext shim verbs-compat
cp -R build/MlxRDMA.dext/Contents/. "$DIST_DIR/flat/com.mlx5.rdma.dext.dext/"
cp MlxRDMA.dext/Contents/MlxRDMA.entitlements \
  "$DIST_DIR/flat/com.mlx5.rdma.dext.dext/"
cp build/librdma_shim.dylib build/libibverbs.dylib "$DIST_DIR/flat/"
plutil -lint "$DIST_DIR/flat/com.mlx5.rdma.dext.dext/Info.plist"
plutil -lint "$DIST_DIR/flat/com.mlx5.rdma.dext.dext/MlxRDMA.entitlements"
plutil -extract IOKitPersonalities.MlxPCIDriver.MlxBuildTag raw \
  "$DIST_DIR/flat/com.mlx5.rdma.dext.dext/Info.plist" > "$DIST_DIR/BUILD_TAG"
plutil -extract CFBundleVersion raw \
  "$DIST_DIR/flat/com.mlx5.rdma.dext.dext/Info.plist" > "$DIST_DIR/DEXT_VERSION"
find "$DIST_DIR/flat" -type f -print0 | sort -z | xargs -0 shasum -a 256 > "$DIST_DIR/SHA256SUMS"
ditto -c -k --keepParent "$DIST_DIR/flat" "$DIST_DIR/MlxRDMA-developer-flat.zip"
printf 'artifact=developer-flat\nversion=%s\nbuild_tag=%s\npci_match=0x101515b3\nsigning=local/developer-only\nnotarized=no\n' \
  "$(cat "$DIST_DIR/DEXT_VERSION")" "$(cat "$DIST_DIR/BUILD_TAG")" > "$DIST_DIR/MANIFEST.txt"
echo "DEVELOPER_PACKAGE PASS: $DIST_DIR/MlxRDMA-developer-flat.zip"
