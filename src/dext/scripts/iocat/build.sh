#!/bin/bash
# Build the iocatalog tools with entitlement (kernel requirement!)
set -e
cd "$(dirname "$0")"
clang -Wno-deprecated-declarations -framework IOKit -framework CoreFoundation inject_ours.c -o /tmp/inject2
clang -Wno-deprecated-declarations -framework IOKit -framework CoreFoundation remove_apple.c -o /tmp/remove_apple
codesign -s - --entitlements ent.plist --force /tmp/inject2
codesign -s - --entitlements ent.plist --force /tmp/remove_apple
echo "OK: /tmp/inject2 /tmp/remove_apple (signed with iocatalog-management)"
