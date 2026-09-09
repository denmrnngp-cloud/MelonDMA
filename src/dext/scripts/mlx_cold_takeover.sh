#!/bin/bash
# Clean DEXT install with the one required reboot, then runtime takeover.
# Run without sudo: ./scripts/mlx_cold_takeover.sh prepare
# After macOS starts:
#   ./scripts/mlx_cold_takeover.sh resume-standard   # A4/G6: pure capture, no injection/kill
#   ./scripts/mlx_cold_takeover.sh resume            # fallback: injected takeover
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
APP=/Applications/MlxRDMA.app
ACT="$APP/Contents/MacOS/mlx_activate"
TEAM="${MLX_TEAM_ID:-}"
BUNDLE=com.melondma.rdma.dext
APPLE=AppleEthernetMLX5

cd "$ROOT"
fail() { echo "ERROR: $*" >&2; exit 1; }
owner() {
    ioreg -r -n ethernet@0 -w 0 2>/dev/null |
        grep -o 'DriverKit_AppleEthernetMLX5\|MlxPCIDriver' | head -1 |
        sed 's/DriverKit_AppleEthernetMLX5/Apple/; s/MlxPCIDriver/ours/'
}
match_count() {
    ioreg -r -n ethernet@0 -l -w 0 2>/dev/null |
        sed -n 's/.*"IODEXTMatchCount" = \([0-9][0-9]*\).*/\1/p' | head -1
}
wait_for_owner() {
    local wanted=$1 seconds=${2:-15} i
    for ((i=0; i<seconds*2; i++)); do
        [ "$(owner)" = "$wanted" ] && return 0
        sleep 0.5
    done
    return 1
}

# Success: refresh the flat .dext in /Library/DriverExtensions so a future
# boot (or DextRecordTable loss) recovers the persona automatically.
finalize() {
    sudo ./scripts/install-to-libde.sh || true
    ./scripts/mlx_dev.sh status
}

# Boot args both the standard and the injected takeover need. Separate so the
# standard path fails with a clear message before touching anything.
check_boot_args() {
    args=$(nvram boot-args 2>/dev/null || true)
    [[ "$args" == *dextrelaunch=1* ]] || fail "missing boot-arg dextrelaunch=1"
    [[ "$args" == *daily_max_dext_crashes=1000* ]] || fail "missing boot-arg daily_max_dext_crashes=1000"
}

dump_capture_state() {
    echo "  owner       = $(owner)" >&2
    echo "  match_count = $(match_count)" >&2
    pgrep -f "$APPLE" >/dev/null 2>&1 && echo "  AppleEthernetMLX5 still running (pid $(pgrep -f "$APPLE" | head -1))" >&2 || \
        echo "  AppleEthernetMLX5 not running" >&2
}

# resume-standard: A4/G6 experiment. Activate the DEXT and let the card be
# claimed purely by the Info.plist personality — no IOCatalogueSendData
# injection, no kill of Apple's driver. This is the only path that can reveal
# whether IOPCIFamily fills the MSI-X table on a normal capture (run
# mlx_irq_probe after it). On failure it leaves the system untouched and
# tells you to fall back to `resume`.
resume_standard() {
    [ -x "$ACT" ] || fail "$ACT is missing; run prepare first"
    sudo -v
    check_boot_args
    "$ACT"
    for ((i = 0; i < 40; i++)); do
        [ "$(owner)" = ours ] && { echo "STANDARD CAPTURE OK: MlxPCIDriver owns the card without injection"; finalize; return 0; }
        sleep 1
    done
    echo "STANDARD CAPTURE FAILED after 40s" >&2
    dump_capture_state
    echo "Fall back to the injected takeover:  ./scripts/mlx_cold_takeover.sh resume" >&2
    return 1
}

prepare() {
    [ "$(id -u)" -ne 0 ] || fail "run as the login user, not root"
    [ -n "$TEAM" ] || fail "MLX_TEAM_ID is not set (Apple Developer Team ID, 10 chars)"
    ./scripts/mlx_dev.sh build
    sudo systemextensionsctl uninstall "$TEAM" "$BUNDLE" || true
    sudo systemextensionsctl reset
    # Stage a flat .dext so kernelmanagerd's boot guess-scan registers our
    # persona before card matching (otherwise Apple wins the first match).
    sudo ./scripts/install-to-libde.sh build/MlxRDMA.dext || true
    echo "Clean state prepared. Rebooting now; after login run:"
    echo "  cd $ROOT && ./scripts/mlx_cold_takeover.sh resume-standard   # try pure capture first"
    echo "  (if that fails)  ./scripts/mlx_cold_takeover.sh resume"
    sudo reboot
}

resume() {
    [ -x "$ACT" ] || fail "$ACT is missing; run prepare first"
    sudo -v
    check_boot_args

    "$ACT"
    for _ in $(seq 1 30); do
        [ "${m:-0}" -ge 2 ] 2>/dev/null && break
        m=$(match_count)
        sleep 1
    done
    if [ "${m:-0}" -lt 2 ] 2>/dev/null; then
        ./scripts/iocat/build.sh
        sudo /tmp/inject2 5000
        m=$(match_count)
    fi
    [ "${m:-0}" -ge 2 ] 2>/dev/null || fail "our PCI personality is absent (IODEXTMatchCount=${m:-0})"

    for _ in $(seq 1 4); do
        [ "$(owner)" = ours ] && { finalize; return; }
        pid=$(pgrep -f "$APPLE" | head -1 || true)
        if [ -z "$pid" ]; then
            for _ in $(seq 1 30); do
                [ "$(owner)" = ours ] && { finalize; return; }
                pgrep -f "$APPLE" >/dev/null 2>&1 && break
                sleep 1
            done
            [ "$(owner)" = ours ] && { finalize; return; }
            pid=$(pgrep -f "$APPLE" | head -1 || true)
            [ -n "$pid" ] || fail "Apple did not relaunch and MlxPCIDriver did not claim the orphaned card"
        fi
        echo "kill Apple PID $pid"
        sudo kill -9 "$pid"
        sleep 8
    done
    [ "$(owner)" = ours ] || fail "takeover did not complete"
    finalize
}

case "${1:-}" in
    prepare) prepare ;;
    resume) resume ;;
    resume-standard) resume_standard ;;
    *) echo "usage: $0 {prepare|resume|resume-standard}"; exit 2 ;;
esac
