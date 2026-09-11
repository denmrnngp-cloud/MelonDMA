#!/bin/bash
# Cross-reboot ownership gate for the ConnectX PCI function.
#
# DriverKit deactivation may legitimately return willCompleteAfterReboot. In
# that state macOS, not this script, owns the transition: a live "repair" by
# injecting a catalogue personality, killing Apple's driver, or forcing a
# rematch would invalidate the gate. Persist the stage instead, let reboot
# perform the normal match, then resume and prove the Apple -> MelonDMA half.
#
# The reverse half can legitimately require a reboot too: sysextd may have an
# old activated record to replace even after Apple owns the PCI nub.  Treat
# that as the same macOS-owned transition, not as an activation failure.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEXT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ACT="/Applications/MelonDMA.app/Contents/MacOS/mlx_activate"
CYCLES="${OWNERSHIP_HANDOFF_CYCLES:-3}"
TIMEOUT="${OWNERSHIP_HANDOFF_TIMEOUT:-45}"
STATE="${OWNERSHIP_HANDOFF_STATE:-$DEXT_DIR/build/ownership-handoff.state}"

owner() {
    ioreg -r -n ethernet@0 -w 0 2>/dev/null |
        grep -o 'DriverKit_AppleEthernetMLX5\|MlxPCIDriver' | head -1 |
        sed 's/DriverKit_AppleEthernetMLX5/Apple/; s/MlxPCIDriver/MelonDMA/'
}

wait_owner() {
    local expected="$1" i
    for ((i = 0; i < TIMEOUT * 2; i++)); do
        [[ "$(owner)" == "$expected" ]] && return 0
        sleep 0.5
    done
    return 1
}

snapshot_msix() {
    ioreg -r -n ethernet@0 -l -w 0 2>/dev/null |
        sed -n 's/.*"IOInterruptSpecifiers" = \(.*\)/\1/p' | head -1
}

match_count() {
    ioreg -r -n ethernet@0 -l -w 0 2>/dev/null |
        sed -n 's/.*"IODEXTMatchCount" = \([0-9][0-9]*\).*/\1/p' | head -1
}

write_state() {
    local stage="$1" cycle="$2" tmp
    mkdir -p "$(dirname "$STATE")"
    tmp="${STATE}.tmp.$$"
    (umask 077; printf 'stage=%s\ncycle=%s\n' "$stage" "$cycle" > "$tmp")
    mv "$tmp" "$STATE"
}

read_state() {
    [[ -r "$STATE" ]] || return 1
    STAGE="$(awk -F= '$1 == "stage" {print $2}' "$STATE")"
    CYCLE="$(awk -F= '$1 == "cycle" {print $2}' "$STATE")"
    [[ "$STAGE" =~ ^(await_apple_boot|activate_melon|await_melon_boot)$ ]] || return 1
    [[ "$CYCLE" =~ ^[1-9][0-9]*$ ]] || return 1
}

assert_start() {
    [[ "$CYCLES" =~ ^[1-9][0-9]*$ ]] || { echo "invalid OWNERSHIP_HANDOFF_CYCLES=$CYCLES" >&2; exit 2; }
    [[ -x "$ACT" ]] || { echo "missing installed activation host: $ACT" >&2; exit 2; }
}

prepare_deactivation() {
    local cycle="$1" output
    [[ "$(owner)" == MelonDMA ]] || {
        echo "OWNERSHIP_HANDOFF FAIL: expected MelonDMA before deactivation, got $(owner)" >&2
        exit 1
    }
    write_state await_apple_boot "$cycle"
    output="$($ACT --deactivate 2>&1)" || true
    echo "$output"
    if grep -q 'RESULT: completed' <<<"$output"; then
        # This is uncommon for a DriverKit PCI extension, but it is still a
        # valid clean transition. Do not reboot needlessly in that case.
        if ! wait_owner Apple; then
            echo "OWNERSHIP_HANDOFF FAIL: completed deactivation did not hand PCI ownership to Apple" >&2
            exit 1
        fi
        write_state activate_melon "$cycle"
        echo "OWNERSHIP_HANDOFF Apple PASS: live deactivation owner=Apple msix=$(snapshot_msix)"
        return 0
    fi
    if grep -q 'rawValue: 1' <<<"$output"; then
        echo "OWNERSHIP_HANDOFF REBOOT_REQUIRED: cycle=$cycle state=$STATE"
        echo "Restart macOS normally, then run: $0 resume"
        exit 75
    fi
    rm -f "$STATE"
    echo "OWNERSHIP_HANDOFF FAIL: unexpected deactivation result" >&2
    exit 1
}

resume_after_boot() {
    read_state || { echo "no valid ownership-handoff state at $STATE" >&2; exit 2; }
    if [[ "$STAGE" == await_apple_boot ]]; then
        if ! wait_owner Apple; then
            echo "OWNERSHIP_HANDOFF FAIL: after reboot Apple does not own ethernet@0 (owner=$(owner))" >&2
            exit 1
        fi
        apple_msix="$(snapshot_msix)"
        [[ -n "$apple_msix" ]] || { echo "OWNERSHIP_HANDOFF FAIL: Apple MSI-X specifiers missing" >&2; exit 1; }
        echo "OWNERSHIP_HANDOFF Apple PASS: cycle=$CYCLE owner=Apple msix=$apple_msix"
        write_state activate_melon "$CYCLE"
    fi
    if [[ "$STAGE" == activate_melon ]]; then
        # This is the normal System Extensions activation flow. It may request a
        # user Allow; the activation host stays alive until the user completes it.
        output="$($ACT --activate 2>&1)" || { echo "$output" >&2; exit 1; }
        echo "$output"
        if grep -q 'rawValue: 1' <<<"$output"; then
            write_state await_melon_boot "$CYCLE"
            echo "OWNERSHIP_HANDOFF REBOOT_REQUIRED: MelonDMA activation is deferred to reboot; state=$STATE"
            echo "Restart macOS normally, then run: $0 resume"
            exit 75
        fi
        grep -q 'RESULT: completed' <<<"$output" || {
            echo "OWNERSHIP_HANDOFF FAIL: MelonDMA activation did not complete" >&2
            exit 1
        }
    fi
    if ! wait_owner MelonDMA; then
        echo "OWNERSHIP_HANDOFF FAIL: MelonDMA did not reclaim ethernet@0 after activation/reboot; owner=$(owner) IODEXTMatchCount=$(match_count)" >&2
        exit 1
    fi
    melon_msix="$(snapshot_msix)"
    [[ -n "$melon_msix" ]] || { echo "OWNERSHIP_HANDOFF FAIL: MelonDMA MSI-X specifiers missing" >&2; exit 1; }
    "$DEXT_DIR/scripts/mlx_hot_update.sh" --check
    # (dev tree also runs build/mlx_phase2_gate --preflight; that gate binary
    #  is not shipped in this repo, so the UserClient check above is the
    #  production preflight.)
    echo "OWNERSHIP_HANDOFF MelonDMA PASS: cycle=$CYCLE owner=MelonDMA msix=$melon_msix"
    if (( CYCLE >= CYCLES )); then
        rm -f "$STATE"
        echo "OWNERSHIP_HANDOFF_GATE PASS: $CYCLES reboot-safe Apple <-> MelonDMA cycles"
        exit 0
    fi
    next=$((CYCLE + 1))
    prepare_deactivation "$next"
}

assert_start
case "${1:-start}" in
    start)
        [[ ! -e "$STATE" ]] || { echo "state already exists; run $0 resume" >&2; exit 2; }
        prepare_deactivation 1
        ;;
    resume) resume_after_boot ;;
    *) echo "usage: $0 {start|resume}" >&2; exit 2 ;;
esac
