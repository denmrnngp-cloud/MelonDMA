#!/bin/bash
# Production packaging/deployment for MlxRDMA.  Unlike mlx_dev.sh this script
# never changes SIP, boot-args, System Extension developer mode, or the system
# extension database.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

APP_ID=com.mlx5.rdma.loader
DEXT_ID=com.mlx5.rdma.dext
APP_NAME=MlxRDMA.app
INSTALL_APP=/Applications/$APP_NAME
DIST_DIR=${DIST_DIR:-$ROOT/dist}
PRODUCTION_APP=$DIST_DIR/$APP_NAME
SIGN_ID=${PRODUCTION_SIGN_ID:-}
DEXT_PROFILE=${DEXT_PROFILE:-}
APP_PROFILE=${APP_PROFILE:-}
NOTARY_PROFILE=${NOTARY_PROFILE:-}
INSTALLER_ID=${PRODUCTION_INSTALLER_SIGN_ID:-}
PKG_NAME=MlxRDMA.pkg
PRODUCTION_PKG=$DIST_DIR/$PKG_NAME

die() { echo "PRODUCTION FAIL: $*" >&2; exit 1; }
say() { echo "=== $* ==="; }
need_file() { [ -f "$1" ] || die "$2: $1"; }

entitlement_xml() {
    codesign -d --entitlements :- "$1" 2>/dev/null
}

signed_has_entitlement() {
    local binary=$1 key=$2 tmp
    tmp=$(mktemp)
    entitlement_xml "$binary" >"$tmp" || { rm -f "$tmp"; return 1; }
    /usr/libexec/PlistBuddy -c "Print :$key" "$tmp" >/dev/null 2>&1
    local rc=$?
    rm -f "$tmp"
    return "$rc"
}

signed_entitlement_value() {
    local binary=$1 path=$2 tmp
    tmp=$(mktemp)
    entitlement_xml "$binary" >"$tmp" || { rm -f "$tmp"; return 1; }
    /usr/libexec/PlistBuddy -c "Print :$path" "$tmp" 2>/dev/null
    local rc=$?
    rm -f "$tmp"
    return "$rc"
}

profile_to_plist() {
    security cms -D -i "$1" >"$2" 2>/dev/null ||
        die "cannot decode provisioning profile: $1"
}

profile_has_entitlement() {
    local profile=$1 key=$2 tmp
    tmp=$(mktemp)
    profile_to_plist "$profile" "$tmp"
    /usr/libexec/PlistBuddy -c "Print :Entitlements:$key" "$tmp" >/dev/null 2>&1
    local rc=$?
    rm -f "$tmp"
    return "$rc"
}

check_source_entitlements() {
    plutil -lint MlxRDMA.dext/Contents/MlxRDMA.entitlements >/dev/null
    plutil -lint loader/Loader.entitlements >/dev/null
    local match
    match=$(/usr/libexec/PlistBuddy \
        -c 'Print :com.apple.developer.driverkit.transport.pci:0:IOPCIMatch' \
        MlxRDMA.dext/Contents/MlxRDMA.entitlements 2>/dev/null || true)
    [ "$match" = 0x101515b3 ] || die "PCI entitlement must match 15b3:1015"
    ! /usr/libexec/PlistBuddy \
        -c 'Print :com.apple.developer.driverkit.allow-any-userclient-access' \
        MlxRDMA.dext/Contents/MlxRDMA.entitlements >/dev/null 2>&1 ||
        die "allow-any-userclient-access is forbidden in production"
    local client_id
    client_id=$(/usr/libexec/PlistBuddy \
        -c 'Print :com.apple.developer.driverkit.userclient-access:0' \
        loader/Loader.entitlements 2>/dev/null || true)
    [ "$client_id" = "$DEXT_ID" ] || die "loader userclient-access does not name $DEXT_ID"
    [ "$(plutil -extract CFBundleIdentifier raw MlxRDMA.dext/Contents/Info.plist)" = "$DEXT_ID" ] ||
        die "DEXT bundle identifier changed"
    [ "$(plutil -extract CFBundleIdentifier raw loader/LoaderInfo.plist)" = "$APP_ID" ] ||
        die "loader bundle identifier changed"
}

check_security_posture() {
    local failures=0 sip bootargs devmode
    sip=$(csrutil status 2>/dev/null || true)
    echo "$sip" | grep -qi 'enabled' || { echo "FAIL: SIP is not enabled: $sip" >&2; failures=1; }
    bootargs=$(nvram boot-args 2>/dev/null || true)
    if echo "$bootargs" | grep -Eq 'amfi_get_out_of_my_way|dextrelaunch|daily_max_dext_crashes'; then
        echo "FAIL: developer-only boot-args are present: $bootargs" >&2
        failures=1
    fi
    devmode=$(systemextensionsctl developer 2>&1 || true)
    if echo "$devmode" | grep -qi 'enabled\|on'; then
        echo "FAIL: System Extension developer mode is enabled" >&2
        failures=1
    fi
    [ "$failures" -eq 0 ] || return 1
}

check_inputs() {
    [ -n "$SIGN_ID" ] || die "set PRODUCTION_SIGN_ID='Developer ID Application: … (TEAMID)'"
    security find-identity -v -p codesigning | grep -Fq "\"$SIGN_ID\"" ||
        die "Developer ID signing identity not found: $SIGN_ID"
    case "$SIGN_ID" in
        "Developer ID Application:"*) ;;
        *) die "production identity must be Developer ID Application, not Apple Development/ad-hoc" ;;
    esac
    security find-identity -v -p basic | grep -Fq '"'"$INSTALLER_ID"'"' ||
        die "Developer ID Installer identity not found: $INSTALLER_ID"
    case "$INSTALLER_ID" in
        "Developer ID Installer:"*) ;;
        *) die "installer identity must be Developer ID Installer" ;;
    esac
    [ -n "$DEXT_PROFILE" ] || die "set DEXT_PROFILE to Apple's granted DriverKit profile"
    [ -n "$APP_PROFILE" ] || die "set APP_PROFILE to the loader/client profile"
    [ -n "$NOTARY_PROFILE" ] || die "set NOTARY_PROFILE to a notarytool keychain profile"
    [ -n "$INSTALLER_ID" ] || die "set PRODUCTION_INSTALLER_SIGN_ID='Developer ID Installer: … (TEAMID)'"
    need_file "$DEXT_PROFILE" "DEXT provisioning profile missing"
    need_file "$APP_PROFILE" "app provisioning profile missing"
    profile_has_entitlement "$DEXT_PROFILE" com.apple.developer.driverkit ||
        die "DEXT profile lacks com.apple.developer.driverkit"
    profile_has_entitlement "$DEXT_PROFILE" com.apple.developer.driverkit.transport.pci ||
        die "DEXT profile lacks granted PCI transport entitlement"
    profile_has_entitlement "$APP_PROFILE" com.apple.developer.system-extension.install ||
        die "app profile lacks system-extension.install"
    profile_has_entitlement "$APP_PROFILE" com.apple.developer.driverkit.userclient-access ||
        die "app profile lacks granted userclient-access"
}

verify_artifact() {
    local app=${1:-$PRODUCTION_APP}
    local dext=$app/Contents/Library/SystemExtensions/$DEXT_ID.systemextension
    need_file "$app/Contents/MacOS/mlx_activate" "loader executable missing"
    need_file "$dext/Contents/MacOS/MlxRDMA" "embedded DEXT missing"
    codesign --verify --deep --strict --verbose=2 "$app"
    spctl --assess --type execute --verbose=2 "$app"
    xcrun stapler validate "$app"
    signed_has_entitlement "$dext" com.apple.developer.driverkit.transport.pci ||
        die "signed DEXT lost PCI entitlement"
    [ "$(signed_entitlement_value "$dext" 'com.apple.developer.driverkit.transport.pci:0:IOPCIMatch')" = 0x101515b3 ] ||
        die "signed DEXT PCI match is not 15b3:1015"
    ! signed_has_entitlement "$dext" com.apple.developer.driverkit.allow-any-userclient-access ||
        die "signed DEXT contains allow-any-userclient-access"
    signed_has_entitlement "$app" com.apple.developer.system-extension.install ||
        die "signed app lost system-extension.install"
    signed_has_entitlement "$app" com.apple.developer.driverkit.userclient-access ||
        die "signed app lost userclient-access"
    [ "$(signed_entitlement_value "$app" 'com.apple.developer.driverkit.userclient-access:0')" = "$DEXT_ID" ] ||
        die "signed app authorizes the wrong DEXT"
    codesign -dvv "$app" 2>&1 | grep -q 'Runtime Version' ||
        die "app is not signed with hardened runtime"
    codesign -dvv "$dext" 2>&1 | grep -q 'Runtime Version' ||
        die "DEXT is not signed with hardened runtime"
    codesign -dvvv "$app" 2>&1 | grep -q 'Authority=Developer ID Application:' ||
        die "app is not signed with Developer ID Application"
    codesign -dvvv "$dext" 2>&1 | grep -q 'Authority=Developer ID Application:' ||
        die "DEXT is not signed with Developer ID Application"
    codesign -dvvv "$app" 2>&1 | grep -q '^Timestamp=' ||
        die "app signature has no secure timestamp"
    codesign -dvvv "$dext" 2>&1 | grep -q '^Timestamp=' ||
        die "DEXT signature has no secure timestamp"
    say "production signature, entitlements, Gatekeeper and notarization verified"
}

preflight() {
    check_source_entitlements
    local failed=0
    check_security_posture || failed=1
    if [ -n "$SIGN_ID$INSTALLER_ID$DEXT_PROFILE$APP_PROFILE$NOTARY_PROFILE" ]; then
        check_inputs || failed=1
    else
        echo "INFO: signing/profile variables are not configured; Apple grant path is pending" >&2
        failed=1
    fi
    [ "$failed" -eq 0 ] || die "machine/signing preflight is not production-ready"
    echo "PRODUCTION_PREFLIGHT PASS"
}

package() {
    check_source_entitlements
    check_inputs
    rm -rf "$DIST_DIR"
    mkdir -p "$DIST_DIR"

    make dext shim verbs-compat
    swiftc -O -framework SystemExtensions loader/mlx_activate.swift -o build/mlx_activate

    local dext=build/MlxRDMA.dext
    cp "$DEXT_PROFILE" "$dext/Contents/embedded.provisionprofile"
    codesign --force --options runtime --timestamp --sign "$SIGN_ID" \
        --entitlements MlxRDMA.dext/Contents/MlxRDMA.entitlements "$dext"

    mkdir -p "$PRODUCTION_APP/Contents/MacOS" \
        "$PRODUCTION_APP/Contents/Library/SystemExtensions" \
        "$PRODUCTION_APP/Contents/Resources/runtime/include/infiniband"
    cp build/mlx_activate "$PRODUCTION_APP/Contents/MacOS/mlx_activate"
    cp loader/LoaderInfo.plist "$PRODUCTION_APP/Contents/Info.plist"
    cp build/librdma_shim.dylib "$PRODUCTION_APP/Contents/Resources/runtime/"
    cp build/libibverbs.dylib "$PRODUCTION_APP/Contents/Resources/runtime/"
    cp usermode/libibverbs_compat/include/infiniband/verbs.h \
        "$PRODUCTION_APP/Contents/Resources/runtime/include/infiniband/"
    cp "$APP_PROFILE" "$PRODUCTION_APP/Contents/embedded.provisionprofile"
    cp -R "$dext" "$PRODUCTION_APP/Contents/Library/SystemExtensions/$DEXT_ID.systemextension"
    codesign --force --options runtime --timestamp --sign "$SIGN_ID" \
        --entitlements loader/Loader.entitlements "$PRODUCTION_APP"

    local submit_zip=$DIST_DIR/MlxRDMA-notary.zip
    ditto -c -k --keepParent "$PRODUCTION_APP" "$submit_zip"
    xcrun notarytool submit "$submit_zip" --keychain-profile "$NOTARY_PROFILE" --wait
    xcrun stapler staple "$PRODUCTION_APP"
    verify_artifact "$PRODUCTION_APP"
    rm -f "$submit_zip"

    local payload="$DIST_DIR/payload"
    mkdir -p "$payload/Applications"
    ditto "$PRODUCTION_APP" "$payload/Applications/$APP_NAME"
    pkgbuild --root "$payload" --identifier "$APP_ID" \
        --version "$(plutil -extract CFBundleVersion raw loader/LoaderInfo.plist)" \
        --install-location / --sign "$INSTALLER_ID" "$PRODUCTION_PKG"
    pkgutil --check-signature "$PRODUCTION_PKG" | grep -q 'Developer ID Installer:' ||
        die "package is not signed with Developer ID Installer"
    ditto -c -k --keepParent "$PRODUCTION_APP" "$DIST_DIR/MlxRDMA-distribution.zip"
    shasum -a 256 "$PRODUCTION_PKG" "$DIST_DIR/MlxRDMA-distribution.zip" >"$DIST_DIR/SHA256SUMS"
    printf 'bundle=%s\nabi=%s\nprovider=RoCEv2 RC\nhardware=capability-driven; see supported matrix\n' \
        "$(plutil -extract CFBundleVersion raw loader/LoaderInfo.plist)" \
        "$(grep -E '^#define MLX_UC_ABI_VERSION' Sources/userclient/MlxUCIO.h | awk '{print $3}')" \
        >"$DIST_DIR/RELEASE-MANIFEST.txt"
    rm -rf "$payload"
    say "production artifacts ready: $PRODUCTION_PKG"
}

install_app() {
    check_security_posture || die "refusing production install on developer security settings"
    verify_artifact "$PRODUCTION_APP"
    [ -f "$PRODUCTION_PKG" ] || die "production package missing: $PRODUCTION_PKG"
    sudo /usr/sbin/installer -pkg "$PRODUCTION_PKG" -target /
    "$INSTALL_APP/Contents/MacOS/mlx_activate" --activate
    systemextensionsctl list | grep -Fq "$DEXT_ID" || die "system extension not registered"
    say "install request completed; a cold reboot may be required for first PCI match"
}

uninstall_app() {
    [ -x "$INSTALL_APP/Contents/MacOS/mlx_activate" ] || die "$INSTALL_APP is not installed"
    "$INSTALL_APP/Contents/MacOS/mlx_activate" --deactivate
    sudo rm -rf "$INSTALL_APP"
    if systemextensionsctl list 2>/dev/null | grep -F "$DEXT_ID" | grep -q 'activated enabled'; then
        die "deactivation is pending (usually until reboot); app was removed but extension remains active"
    fi
    say "clean uninstall completed"
}

case "${1:-}" in
    preflight) preflight ;;
    package) package ;;
    verify) verify_artifact "${2:-$PRODUCTION_APP}" ;;
    install) install_app ;;
    uninstall) uninstall_app ;;
    *) echo "usage: $0 {preflight|package|verify [app]|install|uninstall}"; exit 2 ;;
esac
