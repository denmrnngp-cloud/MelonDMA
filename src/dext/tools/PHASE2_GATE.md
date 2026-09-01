# Phase 2 hardware gate

The gate uses the stock `ibv_rc_pingpong` on the DGX Spark and the MlxRDMA
shim on macOS. It verifies real client memory registration, every RC QP
transition through `QUERY_QP`, CQ/SQ/RQ wrap, payload length and pattern,
one million bidirectional messages across 1/64/256/MTU sizes, then ten full
create/connect/traffic/destroy cycles without an FLR.

Build the signed macOS client:

```sh
make -C src/dext phase2-gate SIGN_ID="Apple Development"
```

Run the complete matrix from the repository root after loading the new DEXT:

```sh
src/dext/tools/run_phase2_gate.sh
```

Because the macOS DEXT does not implement an IP/ARP responder, the DGX needs a
permanent neighbour entry for the Mac endpoint. The runner discovers the
active DGX GID, netdev and bond MAC, validates the entry, and prints the exact
repair command if it is missing. If the Mac-side configured address changed,
pass it explicitly:

```sh
MAC_ROCE_IP=<mac-roce-ip> MAC_ROCE_MAC=<mac-roce-mac> \
  src/dext/tools/run_phase2_gate.sh
```

The preflight also compares the published `MlxBuildTag` with the app in
`build/`. This prevents an activated extension with the same marketing version
but an older user-client ABI from being mistaken for the current build.

SSH and TCP control use the management link, while the exchanged GID selects
the separate RoCE data link. `SPARK_GID_INDEX=auto` scans the configured RDMA
device for the active IPv4 RoCEv2 GID. `PHASE2_MTU=auto` reads `active_mtu` and
maps the media MTU to the standard QP values 256/512/1024/2048/4096. The runner
reads the active netdev MAC instead of assuming a physical-port address.
Override them when needed, for example:

```sh
SPARK_SSH=192.168.100.2 SPARK_CONTROL_HOST=192.168.100.2 \
  SPARK_IB_DEV=rocep1s0f1 \
  src/dext/tools/run_phase2_gate.sh
```

For the first diagnostic exchange only:

```sh
# Use the runner's adaptive GID/MTU discovery, but stop after one exchange.
PHASE2_SMOKE_ONLY=1 PHASE2_DIAGNOSTIC_ITERS=1 \
  src/dext/tools/run_phase2_gate.sh
```

Phase 2 is closed only when the final line is `PHASE2_FULL_GATE PASS` and
the driver log shows clean resource destruction with no page-accounting or
firmware health error.
