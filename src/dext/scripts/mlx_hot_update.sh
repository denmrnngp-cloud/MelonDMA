#!/bin/bash
# Replace a working MlxRDMA DEXT without rebooting.
# Run without sudo: ./scripts/mlx_hot_update.sh
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

owner() {
    ioreg -r -n ethernet@0 -w 0 2>/dev/null |
        grep -o 'MlxPCIDriver\|DriverKit_AppleEthernetMLX5' | head -1 || true
}

active_pid() {
    pgrep -f '/Library/SystemExtensions/.*/com.mlx5.rdma.dext.systemextension/Contents/MacOS/MlxRDMA' |
        head -1 || true
}

swap_stuck() {
    systemextensionsctl list 2>/dev/null | grep -i 'com.mlx5.rdma.dext' |
        grep -E 'terminating|waiting to upgrade' || true
}

recover_swap() {
    local pid i
    [ -z "$(swap_stuck)" ] && return 0

    echo "=== recovering stale DEXT swap ==="
    sudo -v
    pid=$(active_pid)
    [ -n "$pid" ] || {
        echo "ERROR: stale swap has no live MlxRDMA process to restart." >&2
        return 1
    }
    sudo kill -TERM "$pid"

    for ((i = 0; i < 30; i++)); do
        [ -z "$(swap_stuck)" ] && [ -n "$(active_pid)" ] &&
            [ "$(owner)" = "MlxPCIDriver" ] && return 0
        sleep 1
    done

    echo "ERROR: DEXT swap did not recover after terminating PID $pid." >&2
    systemextensionsctl list 2>/dev/null | grep -i 'com.mlx5.rdma.dext' >&2 || true
    return 1
}

[ "$(id -u)" -ne 0 ] || {
    echo "ERROR: run this script as the login user, not root." >&2
    exit 1
}

if [ "${1:-}" = "--check" ]; then
    [ "$(owner)" = "MlxPCIDriver" ] || {
        echo "ERROR: MlxPCIDriver does not own ethernet@0." >&2
        exit 1
    }
    if [ -n "$(swap_stuck)" ]; then
        echo "ERROR: system extension swap is stuck:" >&2
        swap_stuck >&2
        exit 1
    fi
    [ -n "$(active_pid)" ] || {
        echo "ERROR: no active MlxRDMA process." >&2
        exit 1
    }
    echo "HOT_UPDATE_CHECK PASS: one active MlxRDMA process owns ethernet@0"
    systemextensionsctl list 2>/dev/null | grep -i 'com.mlx5.rdma.dext'
    exit 0
fi

[ -z "${1:-}" ] || {
    echo "usage: $0 [--check]" >&2
    exit 2
}

[ "$(owner)" = "MlxPCIDriver" ] || {
    echo "ERROR: MlxPCIDriver must own ethernet@0 before a hot update." >&2
    echo "Use ./scripts/mlx_cold_takeover.sh prepare, reboot, then resume." >&2
    exit 1
}

recover_swap
./scripts/mlx_dev.sh release
recover_swap

echo "=== hot update PASS ==="
systemextensionsctl list 2>/dev/null | grep -i 'com.mlx5.rdma.dext'
