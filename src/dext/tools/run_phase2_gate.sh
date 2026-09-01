#!/bin/bash
# Complete Phase 2 traffic matrix against stock rdma-core ibv_rc_pingpong.
# Override any setting through the environment; defaults match notes/18.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEXT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
GATE_BIN="${PHASE2_GATE_BIN:-$DEXT_DIR/build/mlx_phase2_gate}"
SPARK_SSH="${SPARK_SSH:-192.168.100.2}"
SPARK_CONTROL_HOST="${SPARK_CONTROL_HOST:-192.168.100.2}"
LOCAL_ROCE_IP="${MAC_ROCE_IP:-192.168.200.1}"
LOCAL_ROCE_MAC="${MAC_ROCE_MAC:-98:03:9b:80:6a:94}"
SPARK_IB_DEV="${SPARK_IB_DEV:-rocep1s0f1}"
SPARK_GID_INDEX="${SPARK_GID_INDEX:-auto}"
SPARK_PINGPONG="${SPARK_PINGPONG:-ibv_rc_pingpong}"
# Avoid colliding with an orphaned remote peer from an interrupted run.
# Set PHASE2_BASE_PORT when a fixed port is required for debugging.
BASE_PORT="${PHASE2_BASE_PORT:-$((18515 + ($$ % 1000)))}"
MTU="${PHASE2_MTU:-auto}"
REMOTE_MAC="${SPARK_ROCE_MAC:-}"
LIFECYCLE_CYCLES="${PHASE2_LIFECYCLE_CYCLES:-10}"
SMOKE_ONLY="${PHASE2_SMOKE_ONLY:-0}"
WINDOW="${PHASE2_WINDOW:-1}"
PIPELINE_ONLY="${PHASE3_PIPELINE_ONLY:-0}"
PIPELINE_ITERS="${PHASE3_PIPELINE_ITERS:-100000}"
PIPELINE_SIZE="${PHASE3_PIPELINE_SIZE:-}"
WRITE_ONLY="${PHASE3_WRITE_ONLY:-0}"
READ_ONLY="${PHASE3_READ_ONLY:-0}"
REVERSE_READ="${PHASE3_REVERSE_READ:-0}"
WRITE_ITERS="${PHASE3_WRITE_ITERS:-1024}"
WRITE_SIZE="${PHASE3_WRITE_SIZE:-1048576}"
PIPELINE_DIAGNOSTIC="${PHASE3_DIAGNOSTIC:-0}"
DIAGNOSTIC_ITERS="${PHASE3_DIAGNOSTIC_ITERS:-32}"
CQ_MODE="${PHASE3_CQ_MODE:-auto}"
SIGNAL_ALL="${PHASE3_SIGNAL_ALL:-0}"
RNR_GATE="${PHASE2_RNR_GATE:-0}"
PEER_RX_DEPTH="${PHASE2_PEER_RX_DEPTH:-64}"
WARMUP_SIZE="${PHASE2_WARMUP_SIZE:-0}"
WARMUP_ITERS="${PHASE2_WARMUP_ITERS:-0}"
NO_PROGRESS_TIMEOUT="${PHASE2_TIMEOUT:-30}"
START_FILE="${PHASE3_START_FILE:-}"
READY_FILE="${PHASE3_READY_FILE:-}"
PIPELINE_PEER_SOURCE="${PHASE3_PEER_SOURCE:-$SCRIPT_DIR/mlx_pipeline_peer.c}"
EXPECTED_DEXT_VERSION=""
EXPECTED_BUILD_TAG=""
EMBEDDED_INFO="$DEXT_DIR/build/MlxRDMA.app/Contents/Library/SystemExtensions/com.mlx5.rdma.dext.systemextension/Contents/Info.plist"
SSH_CONTROL_DIR=""
SSH_CONTROL_SOCKET=""

if [[ ! -x "$GATE_BIN" ]]; then
    echo "Gate binary missing; run: make -C '$DEXT_DIR' phase2-gate" >&2
    exit 2
fi

# Fail before prompting for the remote password when the local DriverKit
# service is not actually published. systemextensionsctl may still call a
# crashed extension "activated enabled", which is not sufficient for IOServiceOpen.
if ! ioreg -r -c MlxPCIDriver -l 2>/dev/null | grep -q 'MlxPCIDriver'; then
    echo "Local MlxPCIDriver service is not published." >&2
    echo "Activate the new DEXT, confirm its first EQ callback/heartbeat, then retry." >&2
    exit 3
fi

# IOService::Create looks this dictionary up as a published property of the
# provider.  A bundle-level dictionary is invisible here and causes the
# otherwise vague kIOReturnError (0xe00002bc) from IOServiceOpen.
if ! ioreg -r -c MlxPCIDriver -l 2>/dev/null | grep -q '"MlxRDMAUserClient"'; then
    echo "Installed MlxPCIDriver does not publish MlxRDMAUserClient." >&2
    echo "Install a DEXT whose user-client dictionary is inside the PCI personality." >&2
    exit 4
fi

if [[ -f "$EMBEDDED_INFO" ]]; then
    EXPECTED_DEXT_VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$EMBEDDED_INFO" 2>/dev/null || true)"
    EXPECTED_BUILD_TAG="$(/usr/libexec/PlistBuddy -c 'Print :IOKitPersonalities:MlxPCIDriver:MlxBuildTag' "$EMBEDDED_INFO" 2>/dev/null || true)"
fi
if [[ -n "$EXPECTED_DEXT_VERSION" ]] &&
   ! systemextensionsctl list 2>/dev/null | grep -F "com.mlx5.rdma.dext ($EXPECTED_DEXT_VERSION/$EXPECTED_DEXT_VERSION)" >/dev/null; then
    echo "Installed MlxRDMA version does not match the built version $EXPECTED_DEXT_VERSION." >&2
    echo "Activate $DEXT_DIR/build/MlxRDMA.app before running the gate." >&2
    exit 5
fi
if [[ -n "$EXPECTED_BUILD_TAG" ]] &&
   ! ioreg -r -c MlxPCIDriver -l 2>/dev/null | grep -F "\"MlxBuildTag\" = \"$EXPECTED_BUILD_TAG\"" >/dev/null; then
    echo "Loaded MlxRDMA is not build $EXPECTED_BUILD_TAG (stale DEXT is attached)." >&2
    echo "Install and activate $DEXT_DIR/build/MlxRDMA.app, then take the card over again." >&2
    exit 5
fi

# Exercise IOServiceOpen and two read-only selectors exactly once before
# touching SSH. A correctly tagged MlxPCIDriver is published only after Start
# completes, so repeating a failure cannot repair it and only floods the log.
set +e
"$GATE_BIN" --preflight
preflight_status=$?
set -e
if [[ "$preflight_status" != "0" ]]; then
    if ! ioreg -r -c MlxPCIDriver -l 2>/dev/null | grep -q 'MlxPCIDriver'; then
        echo "MlxRDMA DEXT disappeared during preflight (process crash/stop)." >&2
        echo "Inspect the newest /Library/Logs/DiagnosticReports/MlxRDMA-*.ips." >&2
    fi
    echo "Local Phase 2 user-client preflight failed (status=$preflight_status); DGX was not contacted." >&2
    exit 6
fi

remote_pid=""
cleanup() {
    if [[ -n "$remote_pid" ]]; then
        kill "$remote_pid" 2>/dev/null || true
        wait "$remote_pid" 2>/dev/null || true
    fi
    if [[ -n "$SSH_CONTROL_SOCKET" && -S "$SSH_CONTROL_SOCKET" ]]; then
        ssh -S "$SSH_CONTROL_SOCKET" -O exit "$SPARK_SSH" >/dev/null 2>&1 || true
    fi
    if [[ -n "$SSH_CONTROL_DIR" ]]; then
        rm -rf "$SSH_CONTROL_DIR"
    fi
}
trap cleanup EXIT INT TERM

# Authenticate synchronously exactly once. Subsequent background SSH commands
# reuse this master connection and therefore cannot leave a password prompt
# racing with the local gate process.
SSH_CONTROL_DIR="$(mktemp -d /tmp/mlx-p2-ssh.XXXXXX)"
SSH_CONTROL_SOCKET="$SSH_CONTROL_DIR/control"
echo "=== SSH preflight: $SPARK_SSH ==="
ssh -MNf -o ControlMaster=yes -o ControlPersist=600 \
    -S "$SSH_CONTROL_SOCKET" "$SPARK_SSH"
if [[ "$SPARK_GID_INDEX" == "auto" ]]; then
    SPARK_GID_INDEX="$(ssh -S "$SSH_CONTROL_SOCKET" "$SPARK_SSH" \
        "for gidfile in /sys/class/infiniband/$SPARK_IB_DEV/ports/1/gids/*; do i=\${gidfile##*/}; gid=\$(cat \"\$gidfile\"); ndev=\$(cat /sys/class/infiniband/$SPARK_IB_DEV/ports/1/gid_attrs/ndevs/\$i 2>/dev/null || true); if [[ \"\$gid\" == *ffff:c0a8:c802 && \"\$ndev\" == mac-rdma-bond ]]; then echo \"\$i\"; break; fi; done" | head -1)"
    if [[ ! "$SPARK_GID_INDEX" =~ ^[0-9]+$ ]]; then
        echo "Could not find active IPv4 GID for 192.168.200.2 on Spark." >&2
        exit 10
    fi
    echo "=== adaptive GID: Spark index=$SPARK_GID_INDEX ==="
fi
if [[ "$PIPELINE_ONLY" == "1" || "$WRITE_ONLY" == "1" ||
      "${PHASE3_REVERSE_WRITE_ONLY:-0}" == "1" ||
      "${PHASE3_READ_ONLY:-0}" == "1" ||
      "${PHASE2_MW_INTEROP_GATE:-0}" == "1" ]]; then
    if [[ ! -r "$PIPELINE_PEER_SOURCE" ]]; then
        echo "Phase 3 peer source missing: $PIPELINE_PEER_SOURCE" >&2
        exit 7
    fi
    PEER_HASH="$(shasum -a 256 "$PIPELINE_PEER_SOURCE" | awk '{print substr($1,1,16)}')"
    REMOTE_PEER_SOURCE="/tmp/melon-pipeline-peer-$PEER_HASH.c"
    SPARK_PINGPONG="/tmp/melon-pipeline-peer-$PEER_HASH"
    ssh -S "$SSH_CONTROL_SOCKET" "$SPARK_SSH" \
        "command -v cc >/dev/null && test -d '/sys/class/infiniband/$SPARK_IB_DEV' && test -r '/sys/class/infiniband/$SPARK_IB_DEV/ports/1/gids/$SPARK_GID_INDEX'"
    if ! ssh -S "$SSH_CONTROL_SOCKET" "$SPARK_SSH" "test -x '$SPARK_PINGPONG'"; then
        echo "=== building Phase 3 pipelined peer on DGX ==="
        scp -q -o ControlPath="$SSH_CONTROL_SOCKET" \
            "$PIPELINE_PEER_SOURCE" "$SPARK_SSH:$REMOTE_PEER_SOURCE"
        ssh -S "$SSH_CONTROL_SOCKET" "$SPARK_SSH" \
            "cc -std=gnu11 -O2 -Wall -Wextra -Werror '$REMOTE_PEER_SOURCE' -o '$SPARK_PINGPONG' -libverbs"
    fi
else
    ssh -S "$SSH_CONTROL_SOCKET" "$SPARK_SSH" \
        "command -v '$SPARK_PINGPONG' >/dev/null && test -d '/sys/class/infiniband/$SPARK_IB_DEV' && test -r '/sys/class/infiniband/$SPARK_IB_DEV/ports/1/gids/$SPARK_GID_INDEX'"
fi
REMOTE_GID="$(ssh -S "$SSH_CONTROL_SOCKET" "$SPARK_SSH" \
    "cat '/sys/class/infiniband/$SPARK_IB_DEV/ports/1/gids/$SPARK_GID_INDEX'")"
REMOTE_NDEV="$(ssh -S "$SSH_CONTROL_SOCKET" "$SPARK_SSH" \
    "cat '/sys/class/infiniband/$SPARK_IB_DEV/ports/1/gid_attrs/ndevs/$SPARK_GID_INDEX'")"
if [[ "$REMOTE_GID" == "0000:0000:0000:0000:0000:0000:0000:0000" ]]; then
    echo "DGX GID index $SPARK_GID_INDEX is empty on $SPARK_IB_DEV." >&2
    exit 7
fi
if [[ -z "$REMOTE_MAC" ]]; then
    REMOTE_MAC="$(ssh -S "$SSH_CONTROL_SOCKET" "$SPARK_SSH" \
        "cat '/sys/class/net/$REMOTE_NDEV/address'")"
fi
REMOTE_OPERSTATE="$(ssh -S "$SSH_CONTROL_SOCKET" "$SPARK_SSH" \
    "cat '/sys/class/net/$REMOTE_NDEV/operstate'")"
if [[ "$REMOTE_OPERSTATE" != "up" ]]; then
    echo "DGX RoCE netdev $REMOTE_NDEV is not UP (state=$REMOTE_OPERSTATE)." >&2
    exit 8
fi

# Select the MTU the peer is actually using. max_mtu is only a capability;
# sending with it while active_mtu is smaller causes transport retry errors.
REMOTE_ACTIVE_MTU="$(ssh -S "$SSH_CONTROL_SOCKET" "$SPARK_SSH" \
    "ibv_devinfo -d '$SPARK_IB_DEV' -i 1 2>/dev/null | awk '/active_mtu:/ {print \$2; exit}'" | tr -cd '0-9')"
REMOTE_MAX_MTU="$(ssh -S "$SSH_CONTROL_SOCKET" "$SPARK_SSH" \
    "ibv_devinfo -d '$SPARK_IB_DEV' -i 1 2>/dev/null | awk '/max_mtu:/ {print \$2; exit}'" | tr -cd '0-9')"
REMOTE_NETDEV_MTU="$(ssh -S "$SSH_CONTROL_SOCKET" "$SPARK_SSH" \
    "cat '/sys/class/net/$REMOTE_NDEV/mtu'")"
if [[ ! "$REMOTE_ACTIVE_MTU" =~ ^(256|512|1024|2048|4096)$ ]]; then
    echo "Could not read Spark active RDMA MTU (got '$REMOTE_ACTIVE_MTU')." >&2
    exit 10
fi
if [[ "$MTU" == "auto" ]]; then
    # QP MTU is discrete and is not the same as Ethernet MTU. Use the
    # peer's active RDMA MTU, which already reflects Ethernet/PPPoE/VLAN/
    # legacy-media overhead and the negotiated path.
    MTU="$(ssh -S "$SSH_CONTROL_SOCKET" "$SPARK_SSH" \
        "ibv_devinfo -d '$SPARK_IB_DEV' -i 1 2>/dev/null | awk '/active_mtu:/ {print \$2}' | head -1" | tr -cd '0-9')"
    case "$MTU" in
        256|512|1024|2048|4096) ;;
        *) echo "Could not determine active RDMA MTU on Spark; set PHASE2_MTU explicitly." >&2; exit 10 ;;
    esac
    echo "=== adaptive MTU: Spark active_mtu=$MTU ==="
elif [[ "$MTU" =~ ^(1500|1492|1480|17914|4464|4352|576|9000)$ ]]; then
    # Media MTU input: select the largest QP MTU that fits without assuming
    # that a large Ethernet jumbo frame implies a large RDMA active_mtu.
    MEDIA_MTU="$MTU"
    if (( MEDIA_MTU >= 4096 )); then MTU=4096
    elif (( MEDIA_MTU >= 2048 )); then MTU=2048
    elif (( MEDIA_MTU >= 1024 )); then MTU=1024
    elif (( MEDIA_MTU >= 512 )); then MTU=512
    else MTU=256
    fi
    echo "=== normalized media MTU: $MEDIA_MTU -> RDMA QP MTU=$MTU ==="
elif [[ ! "$MTU" =~ ^(256|512|1024|2048|4096)$ ]]; then
    echo "Invalid PHASE2_MTU=$MTU (use auto, RDMA 256/512/1024/2048/4096, or media 576/1480/1492/1500/4352/4464/9000/17914)." >&2
    exit 10
fi
if (( MTU > REMOTE_ACTIVE_MTU )); then
    echo "Requested RDMA QP MTU=$MTU exceeds Spark active_mtu=$REMOTE_ACTIVE_MTU." >&2
    echo "Spark media=$REMOTE_NETDEV_MTU max_mtu=$REMOTE_MAX_MTU; configure jumbo Ethernet and retry:" >&2
    echo "  sudo ip link set dev $REMOTE_NDEV mtu 9000" >&2
    echo "  sudo ip link set dev $(ssh -S "$SSH_CONTROL_SOCKET" "$SPARK_SSH" "readlink -f /sys/class/infiniband/$SPARK_IB_DEV/device/net/* 2>/dev/null | xargs -r -n1 basename | head -1" || printf '<active-slave>') mtu 9000" >&2
    echo "  sudo ip neigh replace $LOCAL_ROCE_IP lladdr $LOCAL_ROCE_MAC nud permanent dev $REMOTE_NDEV" >&2
    exit 10
fi
LOCAL_ROCE_MAC="$(printf '%s' "$LOCAL_ROCE_MAC" | tr '[:upper:]' '[:lower:]')"
if [[ ! "$LOCAL_ROCE_MAC" =~ ^([0-9a-f]{2}:){5}[0-9a-f]{2}$ ]]; then
    echo "Invalid MAC_ROCE_MAC: $LOCAL_ROCE_MAC" >&2
    exit 9
fi

# The custom macOS DEXT has no IP/ARP stack. The Linux RC path resolves the
# peer GID through the neighbour table before it can build its address vector,
# so the DGX must have a permanent mapping for the Mac RoCE endpoint.
REMOTE_NEIGH="$(ssh -S "$SSH_CONTROL_SOCKET" "$SPARK_SSH" \
    "ip neigh show to '$LOCAL_ROCE_IP' dev '$REMOTE_NDEV' || true")"
if [[ "$REMOTE_NEIGH" != *"lladdr $LOCAL_ROCE_MAC"* ||
      "$REMOTE_NEIGH" == *"FAILED"* || "$REMOTE_NEIGH" == *"INCOMPLETE"* ]]; then
    echo "DGX neighbour entry for $LOCAL_ROCE_IP is missing or stale:" >&2
    echo "  ${REMOTE_NEIGH:-<empty>}" >&2
    echo "Configure it once, then rerun:" >&2
    echo "  ssh -t '$SPARK_SSH' \"sudo ip neigh replace '$LOCAL_ROCE_IP' lladdr '$LOCAL_ROCE_MAC' nud permanent dev '$REMOTE_NDEV'\"" >&2
    exit 9
fi
echo "=== DGX RoCE: ibdev=$SPARK_IB_DEV netdev=$REMOTE_NDEV gid[$SPARK_GID_INDEX]=$REMOTE_GID mac=$REMOTE_MAC ==="
echo "=== DGX neighbour: $REMOTE_NEIGH ==="
remote_diag() {
    ssh -S "$SSH_CONTROL_SOCKET" "$SPARK_SSH" bash -s -- \
        "$REMOTE_NDEV" "$SPARK_IB_DEV" <<'REMOTE_DIAG'
set -u
netdev="$1"
ibdev="$2"
ringdev="$netdev"
if [[ -r "/sys/class/net/$netdev/bonding/active_slave" ]]; then
    ringdev="$(cat "/sys/class/net/$netdev/bonding/active_slave")"
fi
printf 'netdev_mtu='
cat "/sys/class/net/$netdev/mtu"
printf ' active_mtu='
ibv_devinfo -d "$ibdev" -i 1 2>/dev/null | awk '/active_mtu:/ {print $2; exit}'
printf ' max_mtu='
ibv_devinfo -d "$ibdev" -i 1 2>/dev/null | awk '/max_mtu:/ {print $2; exit}'
printf ' pcie='
dev=$(readlink -f "/sys/class/infiniband/$ibdev/device")
cat "$dev/current_link_speed" 2>/dev/null || printf '?'
printf ' x'
cat "$dev/current_link_width" 2>/dev/null || printf '?'
ring_info=$(ethtool -g "$ringdev" 2>/dev/null | awk '
    /Pre-set maximums/ {section="max"; next}
    /Current hardware/ {section="cur"; next}
    $1 == "RX:" && $2 ~ /^[0-9]+$/ {if (section == "max") maxrx=$2; if (section == "cur") rx=$2}
    $1 == "TX:" && $2 ~ /^[0-9]+$/ {if (section == "max") maxtx=$2; if (section == "cur") tx=$2}
    END {printf "rx=%s/%s tx=%s/%s", rx ? rx : "?", maxrx ? maxrx : "?", tx ? tx : "?", maxtx ? maxtx : "?"}')
printf ' ringdev=%s rings=%s' "$ringdev" "${ring_info:-?}"
for counter in port_xmit_data port_rcv_data port_xmit_packets port_rcv_packets port_xmit_discards port_rcv_errors link_error_recovery; do
    printf ' %s=' "$counter"
    cat "/sys/class/infiniband/$ibdev/ports/1/counters/$counter" 2>/dev/null || printf '?'
done
printf '\n'
REMOTE_DIAG
}
if [[ "${PHASE5_DIAGNOSTICS:-0}" == "1" ]]; then
    echo "=== DGX diagnostics before ==="
    remote_diag
fi
echo "=== preflight OK: local DEXT published, DGX RDMA endpoint available ==="
if [[ "$RNR_GATE" == "1" ]]; then
    # ibv_rc_pingpong -N means new_send, not no-receive. Use the dedicated
    # passive peer so the remote QP has no posted RQ and must emit RNR.
    RNR_SOURCE="$SCRIPT_DIR/mlx_rx_drain_peer.c"
    RNR_HASH="$(shasum -a 256 "$RNR_SOURCE" | awk '{print substr($1,1,16)}')"
    RNR_REMOTE_SOURCE="/tmp/melon-rnr-peer-$RNR_HASH.c"
    RNR_REMOTE_BIN="/tmp/melon-rnr-peer-$RNR_HASH"
    scp -q -o ControlPath="$SSH_CONTROL_SOCKET" "$RNR_SOURCE" \
        "$SPARK_SSH:$RNR_REMOTE_SOURCE"
    ssh -S "$SSH_CONTROL_SOCKET" "$SPARK_SSH" \
        "cc -std=gnu11 -O2 -Wall -Wextra -Werror '$RNR_REMOTE_SOURCE' -o '$RNR_REMOTE_BIN' -libverbs"
fi

case_no=0
run_case() {
    local size="$1" iters="$2" label="$3"
    local case_window="${4:-$WINDOW}"
    local cq_mode="${5:-$CQ_MODE}"
    local signal_all="${6:-$SIGNAL_ALL}"
    local operation="${7:-send}"
    local port=$((BASE_PORT + case_no))
    local peer_iters=$((iters + WARMUP_ITERS))
    case_no=$((case_no + 1))
    echo "=== $label: size=$size iters=$iters port=$port ==="
    peer_mode=""
    if [[ "$operation" == "write" ]]; then peer_mode="-W"; fi
    if [[ "$operation" == "reverse" ]]; then peer_mode="-R"; fi
    if [[ "$operation" == "read" ]]; then peer_mode="-Q"; fi
    if [[ "$operation" == "reverse-read" ]]; then peer_mode="-q"; fi
    if [[ "${PHASE2_MW_INTEROP_GATE:-0}" == "1" ]]; then peer_mode="-M"; fi
    if [[ "$RNR_GATE" == "1" ]]; then
        ssh -S "$SSH_CONTROL_SOCKET" "$SPARK_SSH" \
            "exec $RNR_REMOTE_BIN -N -d '$SPARK_IB_DEV' -i 1 -g '$SPARK_GID_INDEX' -p '$port' -c '$size' -m '$MTU' -n '$peer_iters'" &
    else
        ssh -S "$SSH_CONTROL_SOCKET" "$SPARK_SSH" \
            "exec $SPARK_PINGPONG $peer_mode -d '$SPARK_IB_DEV' -i 1 -g '$SPARK_GID_INDEX' -p '$port' -s '$size' -m '$MTU' -r '$PEER_RX_DEPTH' -n '$peer_iters'" &
    fi
    remote_pid=$!
    sleep 1
    if ! kill -0 "$remote_pid" 2>/dev/null; then
        local remote_status=0
        wait "$remote_pid" || remote_status=$?
        remote_pid=""
        echo "Remote ibv_rc_pingpong exited before the local gate started." >&2
        if [[ "$remote_status" == "0" ]]; then remote_status=5; fi
        return "$remote_status"
    fi
    local gate_args=(--port "$port" --iters "$iters" --size "$size" \
        --local-ip "$LOCAL_ROCE_IP" \
        --local-mac "$LOCAL_ROCE_MAC" \
        --mtu "$MTU" --window "$case_window" --timeout "$NO_PROGRESS_TIMEOUT" \
        --remote-mac "$REMOTE_MAC")
    case "$cq_mode" in
        auto) ;;
        shared) gate_args+=(--shared-cq) ;;
        separate) gate_args+=(--separate-cq) ;;
        *) echo "Invalid PHASE3_CQ_MODE: $cq_mode" >&2; return 2 ;;
    esac
    if [[ "$signal_all" == "1" ]]; then gate_args+=(--signal-all); fi
    if [[ "$WARMUP_ITERS" != "0" ]]; then
        gate_args+=(--warmup-size "$WARMUP_SIZE" --warmup-iters "$WARMUP_ITERS")
    fi
    if [[ "$operation" == "write" ]]; then gate_args+=(--rdma-write); fi
    if [[ "$operation" == "reverse" ]]; then gate_args+=(--reverse-write); fi
    if [[ "$operation" == "read" || "$operation" == "reverse-read" ]]; then gate_args+=(--rdma-read); fi
    if [[ "$operation" == "reverse-read" ]]; then gate_args+=(--read-from-peer); fi
    if [[ -n "$START_FILE" ]]; then gate_args+=(--start-file "$START_FILE"); fi
    if [[ -n "$READY_FILE" ]]; then gate_args+=(--ready-file "$READY_FILE"); fi
    local gate_status=0
    "$GATE_BIN" "${gate_args[@]}" "$SPARK_CONTROL_HOST" || gate_status=$?
    if [[ "$gate_status" != "0" ]]; then
        kill "$remote_pid" 2>/dev/null || true
        wait "$remote_pid" 2>/dev/null || true
        remote_pid=""
        return "$gate_status"
    fi
    if [[ "$RNR_GATE" == "1" ]]; then
        # The RNR peer is only a passive QP holder. Its lifetime is not part of
        # the result once the local error CQE has been observed.
        kill "$remote_pid" 2>/dev/null || true
        wait "$remote_pid" 2>/dev/null || true
        remote_pid=""
        return 0
    fi
    # LOCAL_INV is a local QP operation; its peer intentionally remains
    # blocked after the TCP endpoint exchange. Do not wait for that peer after
    # the local gate has already passed.
    if [[ "${PHASE2_LOCAL_INV_GATE:-0}" == "1" ]]; then
        kill "$remote_pid" 2>/dev/null || true
        wait "$remote_pid" 2>/dev/null || true
        remote_pid=""
        return 0
    fi
    local remote_status=0
    wait "$remote_pid" || remote_status=$?
    remote_pid=""
    if [[ "$remote_status" != "0" ]]; then
        echo "Remote peer exited with status=$remote_status after its gate output." >&2
    fi
    return "$remote_status"
}

if [[ "$READ_ONLY" == "1" ]]; then
    read_operation=read
    if [[ "$REVERSE_READ" == "1" ]]; then read_operation=reverse-read; fi
    run_case "$WRITE_SIZE" "$WRITE_ITERS" "Phase 3 one-sided READ" "$WINDOW" "$CQ_MODE" "$SIGNAL_ALL" "$read_operation"
    echo "PHASE3_READ_GATE PASS: window=$WINDOW iters=$WRITE_ITERS size=$WRITE_SIZE direction=$read_operation"
    exit 0
fi

if [[ "$RNR_GATE" == "1" ]]; then
    run_case 64 1 "RNR/retry negative" 1 shared 1 send
    echo "PHASE2_RNR_GATE PASS"
    exit 0
fi

if [[ "$SMOKE_ONLY" == "1" ]]; then
    run_case 64 1 "smoke"
    echo "PHASE2_SMOKE PASS: one bidirectional 64-byte exchange"
    exit 0
fi

if [[ "$READ_ONLY" == "1" ]]; then
    read_operation=read
    if [[ "$REVERSE_READ" == "1" ]]; then read_operation=reverse-read; fi
    run_case "$WRITE_SIZE" "$WRITE_ITERS" "Phase 3 one-sided READ" "$WINDOW" "$CQ_MODE" "$SIGNAL_ALL" "$read_operation"
    echo "PHASE3_READ_GATE PASS: window=$WINDOW iters=$WRITE_ITERS size=$WRITE_SIZE direction=$read_operation"
    exit 0
fi

if [[ "$PIPELINE_ONLY" == "1" ]]; then
    if [[ -z "$PIPELINE_SIZE" ]]; then PIPELINE_SIZE="$MTU"; fi
    if [[ "$PIPELINE_DIAGNOSTIC" == "1" ]]; then
        NO_PROGRESS_TIMEOUT="${PHASE3_DIAGNOSTIC_TIMEOUT:-5}"
        failures=0
        run_case "$MTU" "$DIAGNOSTIC_ITERS" "diag 1/5: batch API count=1, shared CQ" 1 shared 0 || failures=$((failures + 1))
        run_case "$MTU" "$DIAGNOSTIC_ITERS" "diag 2/5: batch API count=1, separate CQs" 1 separate 0 || failures=$((failures + 1))
        run_case "$MTU" "$DIAGNOSTIC_ITERS" "diag 3/5: two WQEs, shared CQ, all signaled" 2 shared 1 || failures=$((failures + 1))
        run_case "$MTU" "$DIAGNOSTIC_ITERS" "diag 4/5: two WQEs, shared CQ, final signaled" 2 shared 0 || failures=$((failures + 1))
        run_case "$MTU" "$DIAGNOSTIC_ITERS" "diag 5/5: two WQEs, separate CQs, final signaled" 2 separate 0 || failures=$((failures + 1))
        if [[ "$failures" != "0" ]]; then
            echo "PHASE3_DIAGNOSTIC FAIL: $failures/5 cases failed" >&2
            exit 1
        fi
        echo "PHASE3_DIAGNOSTIC PASS: batch, signaling, and separate-CQ paths isolated"
        exit 0
    fi
    run_case "$PIPELINE_SIZE" "$PIPELINE_ITERS" "Phase 3 pipeline"
    echo "PHASE3_PIPELINE PASS: window=$WINDOW iters=$PIPELINE_ITERS size=$PIPELINE_SIZE"
    if [[ "${PHASE5_DIAGNOSTICS:-0}" == "1" ]]; then
        echo "=== DGX diagnostics after ==="
        remote_diag
    fi
    exit 0
fi

if [[ "${PHASE3_REVERSE_WRITE_ONLY:-0}" == "1" ]]; then
    run_case "$WRITE_SIZE" "$WRITE_ITERS" "Phase 3 reverse one-sided WRITE" \
        "$WINDOW" shared 0 reverse
    echo "PHASE3_REVERSE_WRITE_GATE PASS: window=$WINDOW iters=$WRITE_ITERS size=$WRITE_SIZE"
    exit 0
fi

if [[ "$WRITE_ONLY" == "1" ]]; then
    run_case "$WRITE_SIZE" "$WRITE_ITERS" "Phase 3 one-sided WRITE" \
        "$WINDOW" shared 0 write
    echo "PHASE3_WRITE_GATE PASS: window=$WINDOW iters=$WRITE_ITERS size=$WRITE_SIZE"
    if [[ "${PHASE5_DIAGNOSTICS:-0}" == "1" ]]; then
        echo "=== DGX diagnostics after ==="
        remote_diag
    fi
    exit 0
fi

# One million total bidirectional exchanges across the required size matrix.
run_case 1    250000 "traffic matrix 1/4"
run_case 64   250000 "traffic matrix 2/4"
run_case 256  250000 "traffic matrix 3/4"
run_case "$MTU" 250000 "traffic matrix 4/4"

# Recreate every PD/CQ/MR/QP, reconnect, exchange traffic and destroy it.
for ((cycle = 1; cycle <= LIFECYCLE_CYCLES; cycle++)); do
    run_case 64 128 "lifecycle $cycle/$LIFECYCLE_CYCLES"
done

echo "PHASE2_FULL_GATE PASS: 1,000,000 bidirectional messages, size matrix, wrap, payload guards, and $LIFECYCLE_CYCLES recreate cycles"
