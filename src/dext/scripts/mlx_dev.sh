#!/bin/bash
# mlx_dev.sh — FULL automation of the MlxRDMA dext dev cycle (without reboot).
#
# Commands:
#   build         build a new version (auto-bump 0.N -> 0.N+1) + install app
#   release       build + activation (REPLACE) + card takeover + verification
#   takeover      card takeover only (no build) — the main state-machine
#   release       safely return the card to AppleEthernetMLX5 without reboot
#   status        card owner, counters, dext version, processes
#   log [N]       tail of the kernel log (MlxPCIDriver/MlxCmd)
#   rematch       synonym for takeover (compatibility with the old script)
#
# INVARIANTS (notes/27, notes/33, notes/35):
#   1. NEVER systemextensionsctl reset on a live system — breaks the launch of
#      ALL dexts until reboot (notes/35).
#   2. NEVER activate a LIVE dext — re-activation = a new process = system FLR
#      = card in reset (notes/35).
#   3. kill only a LIVE Apple process — killing a dead one doesn't increment
#      IORematchCount and doesn't take part in the inversion (notes/27).
#   4. Activating a new binary: first kill the OLD dext → REPLACE returns
#      completed (not willCompleteAfterReboot) → the new one gets picked up
#      on rematch WITHOUT reboot (notes/35).
#   5. Order for a fresh install (only if the dext was not activated):
#      reset → install → activate → REBOOT → rematch. reset is allowed ONLY
#      when a stuck state must be cleaned up, and ALWAYS with a reboot after.
#
# Requires boot-args (nvram): dextrelaunch=1 daily_max_dext_crashes=1000
# (and amfi_get_out_of_my_way=0x1 for ad-hoc iocatalog signing).
#
# Run:  WITHOUT sudo!  (build/sign must run as the user — otherwise
# codesign won't find the Apple Development identity in the login keychain;
# the script uses sudo itself for kill/cp/probe).
#   ./scripts/mlx_dev.sh release
#   ./scripts/mlx_dev.sh takeover

set -u
cd "$(dirname "$0")/.." || exit 1

APP=/Applications/MlxRDMA.app
ACT="$APP/Contents/MacOS/mlx_activate"
PLIST_DEXT=MlxRDMA.dext/Contents/Info.plist
PLIST_LOADER=loader/LoaderInfo.plist
SIGN_ID="${MLX_SIGN_ID:-Apple Development}"
PROBE_TOOL="$PWD/build/mlx_rematch_probe"
APPLE_PAT="AppleEthernetMLX5"
OUR_PAT="com.mlx5.rdma.dext.systemextension/Contents/MacOS/MlxRDMA"

MAX_ROUNDS="${MLX_MAX_ROUNDS:-12}"
LOCK_DIR="${TMPDIR:-/tmp}/melon-mlx-dev-${UID}.lock"

log()  { echo "[$(date '+%H:%M:%S')] $*"; }
fail() { echo "ERROR: $*" >&2; exit 1; }

acquire_mutation_lock() {
    local old_pid=""
    if ! mkdir "$LOCK_DIR" 2>/dev/null; then
        [ -f "$LOCK_DIR/pid" ] && old_pid=$(cat "$LOCK_DIR/pid" 2>/dev/null)
        if [ -n "$old_pid" ] && kill -0 "$old_pid" 2>/dev/null; then
            fail "another dev cycle is already running (PID $old_pid). Wait for it to finish; parallel release is forbidden"
        fi
        rm -rf "$LOCK_DIR" 2>/dev/null
        mkdir "$LOCK_DIR" 2>/dev/null || fail "could not acquire lock $LOCK_DIR"
    fi
    echo "$$" > "$LOCK_DIR/pid"
    trap 'rm -rf "$LOCK_DIR"' EXIT INT TERM HUP
}

# ---------- watchers ----------
# owner: Apple | ours | empty (orphan)
owner() {
    ioreg -r -n ethernet@0 -w 0 2>/dev/null \
        | grep -o "DriverKit_AppleEthernetMLX5\|MlxPCIDriver" | head -1 \
        | sed 's/DriverKit_AppleEthernetMLX5/Apple/; s/MlxPCIDriver/ours/'
}
apple_pid() { pgrep -f "$APPLE_PAT" | head -1; }
our_pid()   { pgrep -f "$OUR_PAT" | head -1; }

# CFBundleVersion of the LIVE MlxRDMA process (empty if not running).
# After kill BEFORE activation, sysextd may manage to restart the OLD dext —
# it re-captures the card and starves the new binary. This watcher lets us
# tell a respawned old-timer from the freshly activated version.
running_dext_version() {
    local pid path plist
    pid=$(our_pid)
    [ -n "$pid" ] || return 0
    path=$(ps -o command= -p "$pid" 2>/dev/null | awk '{print $1}')
    [ -n "$path" ] || return 0
    plist="${path%/Contents/MacOS/MlxRDMA}/Contents/Info.plist"
    [ -f "$plist" ] || return 0
    plutil -extract CFBundleVersion raw "$plist" 2>/dev/null || true
}
ext_match() {
    ioreg -r -n ethernet@0 -l -w 0 2>/dev/null \
        | grep -o 'IODEXTMatchCount" = [0-9]*' | grep -o '[0-9]*$'
}
rematch_count() {
    ioreg -r -n ethernet@0 -l -w 0 2>/dev/null \
        | grep -o 'IORematchCount" = [0-9]*' | grep -o '[0-9]*$'
}

# wait_owner <target> <seconds> — 0 if we waited successfully
wait_owner() {
    local target="$1" secs="${2:-20}" i o
    for ((i=0; i<secs*2; i++)); do
        o=$(owner)
        [ "$o" = "$target" ] && return 0
        sleep 0.5
    done
    return 1
}
wait_proc() {  # $1=pattern $2=seconds
    local i
    for ((i=0; i<$2*2; i++)); do
        [ -n "$(pgrep -f "$1" | head -1)" ] && return 0
        sleep 0.5
    done
    return 1
}
# wait_any_owner <seconds> — 0 if ANY owner appeared
wait_any_owner() {
    local i o
    for ((i=0; i<$1*2; i++)); do
        o=$(owner)
        [ -n "$o" ] && { echo "$o"; return 0; }
        sleep 0.5
    done
    return 1
}

# The card may already be listed under MlxPCIDriver while DriverKit is still
# running Start() and IOService is not published. A later gate in that short
# moment gets a false "no MlxPCIDriver service found". We wait not for the
# owner name, but for a real signed UserClient preflight (service +
# NewUserClient + port UP).
wait_userclient_ready() {
    local secs="${1:-90}" i out status expected_tag saw_dext=0
    expected_tag=$(plutil -extract IOKitPersonalities.MlxPCIDriver.MlxBuildTag \
        raw "$PLIST_DEXT" 2>/dev/null)
    [ -n "$expected_tag" ] || return 1
    for ((i=0; i<secs*4; i++)); do
        if [ -n "$(our_pid)" ]; then
            saw_dext=1
        elif [ "$saw_dext" -eq 1 ] && [ -z "$(owner)" ]; then
            # Start() exited and the provider became an orphan. Waiting the
            # remainder of 90 seconds cannot make the UserClient appear.
            return 2
        fi
        if ioreg -r -c MlxPCIDriver -l -w 0 2>/dev/null \
            | grep -Fq "\"MlxBuildTag\" = \"$expected_tag\"" &&
           [ -x build/mlx_phase2_gate ]; then
            out=$(./build/mlx_phase2_gate --preflight 2>&1)
            status=$?
            if [ "$status" -eq 0 ]; then
                echo "$out"
                return 0
            fi
        fi
        sleep 0.25
    done
    [ -n "${out:-}" ] && echo "$out" >&2
    return 1
}

# ---------- operations ----------
activate() {
    log "activating dext (REPLACE, old process must be dead)..."
    local out
    out=$("$ACT" 2>&1 | tail -8)
    echo "$out"
    # IMPORTANT: "NEEDS APPROVAL" is printed BEFORE completion, and
    # "RESULT: completed" — AFTER the user approves in System Settings (the
    # mlx_activate process hangs in the RunLoop and waits all that time).
    # completed = success, even if NEEDS APPROVAL is also present in the output.
    if echo "$out" | grep -q "RESULT: completed"; then
        return 0
    fi
    if echo "$out" | grep -qi "willCompleteAfterReboot\|rawValue: 1"; then
        fail "REPLACE returned willCompleteAfterReboot — the old process is still alive/stuck. Need reset+reboot (notes/35) or takeover first."
    fi
    if echo "$out" | grep -q "NEEDS APPROVAL"; then
        fail "approval needed: System Settings > Login Items & Extensions > Allow, then run ./scripts/mlx_dev.sh takeover"
    fi
    fail "activation not completed: $out"
}

kill_pat() {  # $1=pattern; kill if alive. 0 = killed, 1 = already dead
    local pid
    pid=$(pgrep -f "$1" | head -1)
    [ -z "$pid" ] && { log "  ($1 not running — skip)"; return 1; }
    sudo kill -9 "$pid" 2>/dev/null && log "  kill -9 $pid ($1)"
    return 0
}

# force_rematch — yank IOServiceRequestProbe on ConnectX (escape from orphan)
force_rematch() {
    if [ ! -x "$PROBE_TOOL" ]; then
        log "  (no $PROBE_TOOL — building)"
        # The script already cd'd to the src/dext root. Jumping into dirname($0)
        # turned the path below into scripts/scripts/mlx_rematch_probe.c and was
        # invisible while an old probe stayed in build.
        ( clang -Wno-deprecated-declarations \
            -framework IOKit -framework CoreFoundation \
            scripts/mlx_rematch_probe.c -o "$PROBE_TOOL" \
            && codesign -s - --entitlements scripts/catalogctl.entitlements \
            --force "$PROBE_TOOL" ) 2>&1 | tail -2
    fi
    [ -x "$PROBE_TOOL" ] || { log "  (mlx_rematch_probe did not build)"; return 1; }
    log "  forced rematch (IOServiceRequestProbe)..."
    sudo "$PROBE_TOOL" probe 2>&1 | sed 's/^/    /'
}

# ---------- diagnostics ----------
diagnose() {
    echo ""
    echo "=== Diagnostics ==="
    echo "  owner:             ${1:-$(owner)}"
    echo "  IORematchCount:    $(rematch_count)"
    echo "  IODEXTMatchCount:  $(ext_match)"
    echo "  our dext pid:      $(our_pid)"
    echo "  Apple pid:         $(apple_pid)"
    systemextensionsctl list 2>/dev/null | grep -i mlx | head -2
}

# ---------- main state-machine: card takeover ----------
takeover() {
    # 0. preconditions (sudo cache in advance — several sudo kill/probe follow)
    sudo -v 2>/dev/null
    local args
    args=$(nvram boot-args 2>/dev/null)
    echo "$args" | grep -q "dextrelaunch=1" || fail "missing boot-arg dextrelaunch=1"
    echo "$args" | grep -q "daily_max_dext_crashes=1000" || fail "missing boot-arg daily_max_dext_crashes=1000"

    local O round MC
    O=$(owner)
    MC=$(ext_match)
    log "takeover start: owner = ${O:-orphan}, IODEXTMatchCount=${MC:-0}, IORematchCount=$(rematch_count)"

    # Our persona MUST be in the kernel catalog (counter >= 2: Apple + ours).
    # If it isn't — takeover CANNOT work. Do NOT try to activate here:
    # activating the same version (0.54 -> 0.54) gets stuck in
    # willCompleteAfterReboot and spawns stuck records (notes/35). This is
    # the territory of release or a full cleanup.
    if [ -z "$MC" ] || [ "$MC" -lt 2 ]; then
        log "OUR PERSONA IS NOT in the kernel catalog (IODEXTMatchCount=${MC:-0})."
        log "  takeover without a persona is impossible. You need ONE of:"
        log "  (a) a new version: sudo ./scripts/mlx_dev.sh release"
        log "  (b) a full cleanup:  systemextensionsctl reset → reboot →"
        log "                      activation → reboot → takeover"
        diagnose "${O:-orphan}"
        return 1
    fi

    [ "$O" = "ours" ] && {
        log "PCI provider assigned to MlxPCIDriver — Start/UserClient still being checked"
        return 0
    }

    for round in $(seq 1 "$MAX_ROUNDS"); do
        O=$(owner)
        log "round $round: owner = ${O:-orphan}"

        # --- case 1: card is ours ---
        [ "$O" = "ours" ] && {
            log "PCI provider assigned to MlxPCIDriver — Start/UserClient still being checked"
            return 0
        }

        # --- case 2: card is with Apple ---
        if [ "$O" = "Apple" ]; then
            kill_pat "$APPLE_PAT"
            # wait until Apple restarts OR the card comes to us
            local i
            for ((i=0; i<30; i++)); do
                sleep 0.5
                O=$(owner)
                [ "$O" = "ours" ] && break
                [ "$O" = "Apple" ] && break
            done
            log "  after kill: ${O:-orphan}"
            continue
        fi

        # --- case 3: our dext alive, but the card is not ours (restart-cycle) ---
        local OP
        OP=$(our_pid)
        if [ -n "$OP" ]; then
            log "our dext is in a restart cycle (PID $OP) — kill so Apple brings the card up from scratch"
            kill_pat "$OUR_PAT"
            sleep 12
            continue
        fi

        # --- case 4: orphan (nobody) ---
        # The persona in the catalog is already verified (counter >= 2). An
        # orphan with a persona present = matching didn't finish by itself →
        # force a probe.
        log "orphan card — persona present, forcing rematch without reboot"

        # Forced rematch: IOServiceRequestProbe on the PCI node.
        force_rematch

        # Wait for any owner.
        local got
        got=$(wait_any_owner 20)
        if [ -n "$got" ]; then
            log "  rematch brought up: $got"
            continue
        fi

        # Still an orphan. If there is no persona in the catalog — it is
        # unrecoverable without a reboot (notes/27: registration in
        # kernelmanagerd happens only at boot).
        MC=$(ext_match)
        if [ -z "$MC" ] || [ "$MC" -lt 2 ]; then
            log "  UNRECOVERABLE without a reboot: no persona in the catalog."
            log "  → DO A REBOOT, then again: sudo ./scripts/mlx_dev.sh takeover"
            diagnose "orphan"
            return 1
        fi
        log "  orphan did not come alive (persona present) — next round"
        sleep 5
    done

    log "failed after $MAX_ROUNDS rounds"
    diagnose "$(owner)"
    return 1
}

do_takeover_ready() {
    takeover || return 1
    log "checking Start() completion, UserClient publication and port state..."
    if wait_userclient_ready 90; then
        log "=== SUCCESS: MlxPCIDriver, UserClient and port are ready ==="
        return 0
    fi
    local ready_rc=$? failed_owner failed_pid
    failed_owner=$(owner)
    failed_pid=$(our_pid)
    diagnose "$failed_owner"
    if [ "$ready_rc" -eq 2 ] || { [ -z "$failed_owner" ] && [ -z "$failed_pid" ]; }; then
        fail "DEXT finished Start, but the PCI card is left without an owner/config=0xffff. A full cold power cycle is needed; re-activation will not restore PCIe power"
    fi
    fail "provider assigned, but UserClient/port did not become ready; see ./scripts/mlx_dev.sh log 100"
}

# ---------- commands ----------
do_driver_release() {
    sudo -v 2>/dev/null || fail "interactive sudo needed for driver-release"
    local O OP i out
    O=$(owner)
    if [ "$O" = "Apple" ]; then
        log "card is already with AppleEthernetMLX5 — done"
        return 0
    fi
    [ "$O" = "ours" ] || fail "card does not belong to our DEXT (owner=${O:-orphan})"

    # Do not tear down a live client. A QP/MR process must exit first so the
    # DEXT can revoke mappings and release DMA before the provider disappears.
    if pgrep -f 'mlx_phase2|melon_mlx|melon_py|rdma_bench|mlx_mlx_gate' >/dev/null 2>&1; then
        fail "active RDMA clients found; terminate them first"
    fi
    [ -x "$ACT" ] || fail "no installed activation host: $ACT (first run ./scripts/mlx_dev.sh release)"
    strings "$ACT" 2>/dev/null | grep -Fq 'REQUEST: deactivate' ||
        fail "installed loader is outdated and does not support deactivation; first run ./scripts/mlx_dev.sh release, then repeat driver-release"
    log "requesting proper deactivation of the system extension from /Applications without reboot"
    out=$("$ACT" --deactivate 2>&1) || {
        echo "$out"
        fail "deactivation request failed"
    }
    echo "$out"
    echo "$out" | grep -q 'REQUEST: deactivate com.mlx5.rdma.dext' ||
        fail "loader did not send the deactivation request; check a fresh build/MlxRDMA.app"
    echo "$out" | grep -q 'REPLACE:' &&
        fail "loader sent replace instead of deactivation"
    echo "$out" | grep -q 'RESULT: completed' ||
        fail "deactivation did not complete successfully; reset/reboot were not performed"
    for ((i=0; i<40; i++)); do
        sleep 0.5
        O=$(owner)
        [ "$O" = "Apple" ] && {
            log "=== SUCCESS: card returned to AppleEthernetMLX5 without reboot ==="
            return 0
        }
        [ -z "$O" ] && force_rematch >/dev/null 2>&1 || true
    done
    diagnose "${O:-orphan}"
    fail "system extension deactivated, but AppleEthernetMLX5 did not take the card back; reset/reboot were not performed"
}


do_status() {
    echo "=== card owner ==="
    owner | sed 's/^$/orphan/'
    echo "=== counters ==="
    echo "  IORematchCount:   $(rematch_count)"
    echo "  IODEXTMatchCount: $(ext_match)"
    echo "=== dext ==="
    systemextensionsctl list 2>/dev/null | grep -i mlx | head -3
    echo "=== processes ==="
    pgrep -fl "$OUR_PAT|$APPLE_PAT" | head -4
}

do_log() {
    local n="${1:-15}"
    /usr/bin/log show --last 2m --predicate 'process == "kernel"' --info 2>/dev/null \
        | grep -E "MlxPCIDriver|MlxCmd|DK: Mlx|server launched|server exit" | tail -"$n" || true
}

# doctor — diagnosis + what to do next (does not change the system)
do_doctor() {
    local list mc rc stuck
    list=$(systemextensionsctl list 2>/dev/null | grep -i mlx)
    mc=$(ext_match)
    rc=$(rematch_count)
    echo "=== Card owner ==="
    echo "  $(owner | sed 's/^$/orphan/')"
    echo "=== Counters ==="
    echo "  IORematchCount:   ${rc:-?}"
    echo "  IODEXTMatchCount: ${mc:-?}  (>=2 = our persona in the catalog)"
    echo "=== dext in systemextensionsctl ==="
    echo "$list" | sed 's/^/  /'
    echo "=== Processes ==="
    pgrep -fl "$OUR_PAT|$APPLE_PAT" | sed 's/^/  /' | head -4
    echo ""
    echo "=== Diagnosis ==="
    if echo "$list" | grep -qE "terminating|waiting to upgrade"; then
        echo "  ❗ STUCK RECORDS (stuck swap). The only cure:"
        echo "     sudo systemextensionsctl reset"
        echo "     sudo reboot"
        echo "     # after reboot: activation → reboot → takeover (full cleanup below)"
        echo ""
        echo "  Full cleanup (notes/35):"
        echo "    1) sudo systemextensionsctl reset"
        echo "    2) sudo reboot"
        echo "    3) systemextensionsctl list  → expect 0 extension(s)"
        echo "    4) sudo rm -rf /Applications/MlxRDMA.app"
        echo "    5) sudo ./scripts/mlx_dev.sh build   (builds a new version + installs the app)"
        echo "    6) sudo /Applications/MlxRDMA.app/Contents/MacOS/mlx_activate"
        echo "       → NEEDS APPROVAL → System Settings > Login Items & Extensions > Allow"
        echo "       → repeat the activation → RESULT: completed"
        echo "    7) sudo reboot   (so the persona gets into the kernel catalog)"
        echo "    8) sudo ./scripts/mlx_dev.sh takeover"
    elif [ -n "$mc" ] && [ "$mc" -ge 2 ]; then
        echo "  ✅ persona in the catalog. Action: sudo ./scripts/mlx_dev.sh takeover"
    else
        echo "  ⚠️  our persona is not in the catalog (IODEXTMatchCount=${mc:-0})."
        echo "     New version:  sudo ./scripts/mlx_dev.sh release"
        echo "     Full cleanup: see above (reset → reboot → activation → reboot → takeover)"
    fi
}

do_build() {
    [ "$(id -u)" -eq 0 ] && fail "run as root — build/sign will break (codesign won't find the identity). Run WITHOUT sudo: ./scripts/mlx_dev.sh $*"
    [ -f "$PLIST_DEXT" ] || fail "no $PLIST_DEXT (run from src/dext?)"
    local ver loader_ver minor newver
    ver=$(plutil -extract CFBundleVersion raw "$PLIST_DEXT")
    loader_ver=$(plutil -extract CFBundleVersion raw "$PLIST_LOADER")
    minor=$(( ${ver#*.} + 1 ))
    newver="0.$minor"
    log "version $ver -> $newver"
    plutil -replace CFBundleVersion -string "$newver" "$PLIST_DEXT"
    plutil -replace CFBundleShortVersionString -string "$newver" "$PLIST_DEXT"
    plutil -replace CFBundleVersion -string "$newver" "$PLIST_LOADER"

    # One dependency graph builds and validates the portable encoders, IIG,
    # DEXT, signed gate and app.  Do not pipe make through grep: that used to
    # hide make's exit status and could install a stale binary after a failure.
    if ! make SIGN_ID="$SIGN_ID" check-host check-dext phase2-gate phase3-gate p3-gate app; then
        plutil -replace CFBundleVersion -string "$ver" "$PLIST_DEXT"
        plutil -replace CFBundleShortVersionString -string "$ver" "$PLIST_DEXT"
        plutil -replace CFBundleVersion -string "$loader_ver" "$PLIST_LOADER"
        fail "build/check/sign failed; version restored to $ver"
    fi

    # rm -rf is mandatory: cp -R onto an existing folder does NOT overwrite but NESTS (notes/35)
    sudo rm -rf "$APP" && sudo cp -R build/MlxRDMA.app "$APP" || fail "cp to /Applications"
    log "installed: $APP ($newver)"
}

do_release() {
    sudo -v 2>/dev/null
    do_build

    # 1. kill our OLD dext BEFORE activation (otherwise REPLACE returns willCompleteAfterReboot)
    log "killing old dext before activation (REPLACE without reboot)..."
    kill_pat "$OUR_PAT" || true
    sleep 2

    # 2. activation of the new binary
    activate

    # 2b. kill BEFORE activation — race with sysextd: the old dext manages to
    # respawn (in the sleep 2) and re-captures the card, so REPLACE is
    # completed, but the live process stays the old version and starves the new
    # binary. We finish off the respawned old-timer until newver comes up.
    local newver rv
    newver=$(plutil -extract CFBundleVersion raw "$PLIST_DEXT")
    for _ in 1 2 3; do
        rv=$(running_dext_version)
        [ -z "$rv" ] && break
        [ "$rv" = "$newver" ] && break
        log "old dext $rv still alive (need $newver) — finishing it off, waiting for rematch"
        kill_pat "$OUR_PAT" || true
        sleep 2
    done

    # 3. Check whether the persona got into the kernel catalog (counter >= 2).
    #    - >= 2 → a normal version-swap, can capture WITHOUT reboot.
    #    - <  2 → a fresh activation after reset: the persona appears only
    #            after a reboot (kernelmanagerd sends it at boot).
    local MC
    MC=$(ext_match)
    if [ -z "$MC" ] || [ "$MC" -lt 2 ]; then
        log "after activation the persona is still not in the catalog (IODEXTMatchCount=${MC:-0})."
        log "This is a fresh install → a reboot is needed, then takeover:"
        log "  sudo reboot"
        log "  sudo ./scripts/mlx_dev.sh takeover"
        return 1
    fi

    # 4. card capture
    takeover || fail "takeover failed (see diagnostics above)"
    log "waiting for MlxPCIDriver publication and UserClient readiness..."
    wait_userclient_ready 90 || {
        local ready_rc=$? failed_owner failed_pid
        failed_owner=$(owner)
        failed_pid=$(our_pid)
        diagnose "$failed_owner"
        if [ "$ready_rc" -eq 2 ] || { [ -z "$failed_owner" ] && [ -z "$failed_pid" ]; }; then
            fail "DEXT finished Start, but the PCI card is left without an owner (usually config=0xffff after reset). Do not run release again: a full cold reboot is needed, then a single run of ./scripts/mlx_dev.sh takeover"
        fi
        fail "MlxPCIDriver got the card, but UserClient/port did not become ready within 90 seconds; see ./scripts/mlx_dev.sh log 100"
    }
    log "=== SUCCESS: card is with MlxPCIDriver, UserClient and port are ready ==="
    do_log 12
}

case "${1:-}" in
    build|release|driver-release|takeover|rematch) acquire_mutation_lock ;;
esac

case "${1:-}" in
    build)    do_build ;;
    release)  do_release ;;
    driver-release) do_driver_release ;;
    takeover) do_takeover_ready ;;
    rematch)  do_takeover_ready ;;
    status)   do_status ;;
    doctor)   do_doctor ;;
    log)      do_log "${2:-15}" ;;
    *) echo "usage: $0 {build|release|driver-release|takeover|rematch|status|doctor|log [N]}"; exit 1 ;;
esac
