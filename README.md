# MelonDMA

<p align="center">
  <img src="MelonDMA.png" width="640" alt="MelonDMA logo" />
</p>

<p align="center">
  <img src="assets/stats-ticker.svg" alt="MelonDMA live stats: 14-day views, total downloads, GitHub stars" />
</p>

A macOS **DriverKit (DEXT)** RDMA provider for **Mellanox/NVIDIA ConnectX** cards
over **RoCEv2**, with a **libibverbs-compatible userspace layer**.

MelonDMA brings a Linux-style verbs programming model to macOS. A userspace
process links against a compatibility library and drives real RDMA SEND/RECV,
RDMA WRITE, RDMA READ, RC atomics and QP/CQ/MR lifecycle operations; the actual
data path runs inside a PCI DriverKit extension that owns the ConnectX HCA.

```
        userspace                                   kernel / DriverKit
 ┌──────────────────────┐                    ┌──────────────────────────────┐
 │  libibverbs_compat   │  (verbs API)       │                              │
 │       +              │──────────────────▶│  MlxRDMA.dext (PCIDriverKit) │
 │    librdma_shim      │   IOUserClient     │   RoCEv2 RC data path on     │
 └──────────────────────┘                    │   ConnectX HCA (15b3:1015)   │
                                             └──────────────────────────────┘
```

---

## What it is

* **A DEXT (DriverKit system extension)**, `com.mlx5.rdma.dext`, built with
  `PCIDriverKit`. It is a port of Apple's own `AppleEthernetMLX5` mlx5 driver
  concept to the PCI DriverKit surface. When active, it *replaces* Apple's
  `AppleEthernetMLX5` as the owner of the ConnectX card.
* **A verbs-compatible userspace stack**: `usermode/libibverbs_compat/` provides
  an `infiniband/verbs.h`-shaped API, and `usermode/librdma_shim/` is the
  IOUserClient bridge that talks to the DEXT.
* **A hardware gate/tool suite** that runs real traffic against a Linux peer
  (stock `ibv_rc_pingpong` and custom peer programs) to prove the data path.

The driver implements the verbs primitives you expect: protection domains (PD),
memory regions (MR), queue pairs (QP), completion queues (CQ), send/recv work
requests and work completions, GID resolution, RC atomics, and QoS/service-level.

## Honest current status

* **P0 / P1 / P2 / P3 production gates pass on the maintainer's hardware**
  (ConnectX-4 Lx on an ADT-Link PCIe Gen3 adapter ↔ NVIDIA DGX Spark peer over 40G).
  This includes the SEND/RECV, one-sided WRITE, one-sided READ, RC atomics,
  multi-QP scaling, and no-FLR stable-driver (INIT_HCA) re-init cycles.
* **Apple has not yet granted the PCI / UserClient entitlements.** A production,
  SIP-on install requires Apple to grant `com.apple.developer.driverkit.transport.pci`
  and `com.apple.developer.driverkit.userclient-access`. Until then, the driver is
  developed and exercised with **SIP disabled** (or `systemextensionsctl developer on`)
  and a locally signed driver.
* These two facts — passing hardware gates and the outstanding entitlement grant —
  are the real state of the project. See `docs/architecture.md` for the design and
  `docs/inference-client-guide.md` for the client API; status and gate results
  are in `CHANGELOG.md`.

---

## Repository layout

| Path | What it is |
|------|-----------|
| `src/dext/Sources/` | The DEXT itself (PCI driver, user client, hw/core/ib layers) |
| `src/dext/MlxRDMA.dext/` | DEXT bundle source: `Info.plist` + entitlements |
| `src/dext/usermode/libibverbs_compat/` | verbs-compatible API surface |
| `src/dext/usermode/librdma_shim/` | IOUserClient shim library |
| `src/dext/Tests/` | Host self-tests (portable IFC encoders, shim, ABI, mkey) |
| `src/dext/tools/` | Hardware gates, Linux peer programs, gate runners |
| `src/dext/scripts/` | Dev-cycle, takeover, hot-update, production scripts |
| `src/dext/loader/` | The `MlxRDMA.app` activation host |
| `src/dext/kext-test/` | Standalone Phase-1a test kext (parallel to the DEXT) |
| `src/tools/` | Thermal / 40G link / card reconnect helpers |
| `docs/` | Architecture and the inference-client guide |

---

## Prerequisites

* **Apple Developer account** (paid program) for code signing and the
  entitlement request. A plain ad-hoc signature is **not** enough for the
  `com.apple.developer.*` entitlements the DEXT needs.
* **Xcode** with the **DriverKit SDK** (and the `iig` tool) installed.
* An **Apple silicon Mac** (the DEXT builds for `arm64e`).
* A **Mellanox/NVIDIA ConnectX card** (ConnectX-4 Lx / ConnectX-7 tested) on an
  ADT-Link PCIe Gen3 adapter or a PCIe slot. The DEXT matches PCI device `15b3:1015`.
* A **peer Linux host** with `rdma-core` / `libibverbs` (`ibv_rc_pingpong`,
  `ibv_devinfo`) reachable over SSH — needed for every live hardware gate.
* For development bring-up today: **SIP disabled** (or
  `systemextensionsctl developer on`), plus the boot-args
  `dextrelaunch=1 daily_max_dext_crashes=1000` (see the scripts).

---

## Building

Everything is driven from `src/dext/Makefile`. Three surfaces:

```sh
# 1. Host self-tests — no DriverKit SDK needed (portable encoders + ABI,
#    ASan/UBSan). Runs on any Mac with clang.
make -C src/dext check-host

# 2. DEXT source check — generates IIG headers and syntax-checks every DEXT
#    source against the DriverKit SDK. No signing/loading.
make -C src/dext check-dext

# 3. Full signed build — DEXT + activation app, release build (MLX_DEBUG=0).
make -C src/dext MLX_DEBUG=0 app
```

`MLX_DEBUG=1` keeps verbose debug logging compiled in (development builds);
`MLX_DEBUG=0` (the default) compiles it out for release.

Signing identity defaults to `Apple Development` and is overridable:

```sh
make -C src/dext app SIGN_ID="Apple Development: Your Name (TEAMID)"
```

The production packaging path (`./src/dext/scripts/mlx_production.sh package`) requires
`Developer ID Application` / `Developer ID Installer` identities and Apple's
granted provisioning profiles — see
`./src/dext/scripts/mlx_production.sh preflight` for the exact requirements.

---

## Installing & activating the DEXT

Be clear-eyed about this: **this is a kernel-level driver**, and until Apple
grants the PCI/UserClient entitlements it can only run with SIP relaxed.

**Why:** a DriverKit PCI driver must carry the
`com.apple.developer.driverkit.transport.pci` entitlement, which Apple grants
per-team through a request form rather than through normal Developer Program
membership. Without it the only way to load the DEXT is the developer path:
SIP disabled or `systemextensionsctl developer on`, plus a locally signed
driver and developer boot-args.

The supported bring-up flow is encoded in the scripts (see next section). In
short:

1. Sign with your Apple Development identity and install `MlxRDMA.app`.
2. Activate the system extension (the loader requests activation; approve it in
   System Settings → Login Items & Extensions).
3. Reboot once so the kernel catalog knows the new PCI personality.
4. Take the card over from Apple's `AppleEthernetMLX5`.

A cold, clean install is one command away:

```sh
MLX_TEAM_ID=<your-10-char-TeamID> ./src/dext/scripts/mlx_cold_takeover.sh prepare
# …reboots…
./src/dext/scripts/mlx_cold_takeover.sh resume
```

> The `MLX_TEAM_ID` variable is the Apple Developer Team ID (10 characters).
> It was previously hardcoded in the script; it is now required via the
> environment.

---

## Driver takeover & safe-run guidance

The DEXT **replaces Apple's `AppleEthernetMLX5`** for the ConnectX card. The
scripts below encode the hard-won rules for doing that without bricking the
card or wedging the machine. Read them before running anything against real
hardware: `src/dext/scripts/mlx_dev.sh`, `src/dext/scripts/mlx_hot_update.sh`,
`src/dext/scripts/mlx_production.sh`, `src/dext/scripts/mlx_cold_takeover.sh`, and
`src/dext/tools/run_stable_driver_gate.sh`.

### (a) Build and activate

```sh
# builds + bumps version + installs app + activates + takes the card over
./src/dext/scripts/mlx_dev.sh release
```

If it is a fresh install (the kernel catalog does not yet know the PCI
personality), `release` will tell you a reboot is required before takeover:

```sh
sudo reboot
./src/dext/scripts/mlx_dev.sh takeover
```

### (b) Hot-update the running DEXT without a reboot

```sh
./src/dext/scripts/mlx_hot_update.sh
```

This replaces the running `MlxRDMA` system extension in place: it recovers any
stale swap state, then drives `mlx_dev.sh release` (which kills the old DEXT
process *before* activation so `REPLACE` completes without `willCompleteAfterReboot`),
and re-takes the card over — no reboot. Verify first with `./src/dext/scripts/mlx_hot_update.sh --check`.

### (c) Recover if the card / driver wedges (FLR / stable gate / reboot)

* **Diagnose** first: `./src/dext/scripts/mlx_dev.sh doctor` and `./src/dext/scripts/mlx_dev.sh status`.
* **Stuck system-extension swap state** (the usual wedge): the only clean cure is
  `sudo systemextensionsctl reset` + `sudo reboot`, then re-activate and re-takeover.
* **Firmware-level recovery**: the stable gate (`src/dext/tools/run_stable_driver_gate.sh`)
  drives the HCA through Linux's complete close/open sequence
  (`TEARDOWN_HCA → TAKE pages → DISABLE_HCA → ENABLE_HCA → startup pages →
  SET_HCA_CAP → INIT_HCA`) **without FLR**. If an experimental cycle fails, the
  DEXT auto-recovers through FLR and reports which stage failed.
* **Card left with no owner** (PCI config `0xffff`, the "orphan" state): this is
  not recoverable by re-activation — you need a full **cold power cycle** of the
  card, then a single `./src/dext/scripts/mlx_dev.sh takeover`. The scripts print this
  explicitly when they detect it.

### (d) Control the number of cycles

Two knobs matter:

* **`STABLE_INIT_CYCLES`** (default `2`, range `1..100`) — how many no-FLR
  HCA re-init cycles the stable gate runs. This is the "stable gate" cycle
  control: it bounds how many times the driver tears down and re-initializes the
  HCA firmware context before stopping, which is what keeps the machine from
  looping into a reset and lets Apple's driver disconnect cleanly:

  ```sh
  STABLE_INIT_CYCLES=5 ./src/dext/tools/run_stable_driver_gate.sh
  ```

* **`MLX_MAX_ROUNDS`** (default `12`) — how many takeover rounds
  `mlx_dev.sh` will attempt when racing Apple's driver for the card
  (kill Apple → rematch → re-check owner, each round).

---

## Logging

The DEXT logs with `IOLog`, prefixed by component (`MlxPCIDriver:`,
`MlxCmd:`, `MlxQP:`, …). Collect it from the unified log:

```sh
log show --last 5m --predicate 'process == "MlxRDMA"' --info
log stream --predicate 'process == "MlxRDMA"'
```

The dev script ships a convenience tail that greps the kernel log:

```sh
./src/dext/scripts/mlx_dev.sh log 50
# equivalent: log show --last 2m --predicate 'process == "kernel"' --info \
#              | grep -E 'MlxPCIDriver|MlxCmd|DK: Mlx|server launched|server exit'
```

Gate artifacts land in the build tree and under `/tmp`:

* `src/dext/build/stable-driver-logs/<timestamp>/` — `metadata.txt`, `gate.txt`
  (firmware stage/opcode/status/syndrome report) and `kernel.log` from the stable gate.
* `/tmp/melon-*` and `/tmp/mlx-*` — per-gate work dirs (speed sweeps, multi-QP
  barriers, SSH control sockets).

### When filing a bug, attach

1. The **gate output** (`build/stable-driver-logs/*/gate.txt`, or the failing
   runner's stdout).
2. The **kernel/DEXT log** for the window of the failure
   (`log show --start <time> --predicate 'process == "MlxRDMA"'`).
3. `src/dext/build/stable-driver-logs/*/metadata.txt` (OS version, boot session,
   source build tag).
4. Any `MlxRDMA-*.ips` crash reports from
   `/Library/Logs/DiagnosticReports/`.

That combination (firmware stage + DEXT log + build tag + crash report) is what
the maintainers need to debug.

---

## Running the tests & gates

### No hardware needed

```sh
make -C src/dext check-host    # host encoder/ABI/shim/verbs tests (ASan/UBSan)
make -C src/dext check-dext    # iig + arm64e DriverKit syntax check
```

These also run in CI (`.github/workflows/ci.yml`).

### Live hardware gates (need a Linux peer)

Every live gate needs a Linux peer reachable at the IP configured in the runner
scripts, with `ibv_rc_pingpong` / `ibv_devinfo` available:

```sh
./src/dext/tools/run_p0_gate.sh                    # P0: full datapath matrix
./src/dext/tools/run_phase3_multi_qp_gate.sh       # Phase 3: multi-QP scaling (1/2/4/8 lanes)
./src/dext/tools/run_phase5_speed_gate.sh          # Phase 5: outstanding-WR speed sweep
./src/dext/tools/run_stable_driver_gate.sh         # stable driver: no-FLR INIT_HCA cycles
```

Other gates: `run_phase2_gate.sh` (SEND/RECV matrix), `run_p3_gate.sh` (inline +
atomics + SL), `run_phase5_bulk_write_gate.sh`, `run_r5_gate.sh`, and the
P0.x/P1.x/P2.x isolation/lifetime/quota/soak/ABI-fuzz runners under `src/dext/tools/`.

### Peer configuration

The peer IPs in the scripts are **test defaults** and every one of them is
overridable through environment variables:

| Variable | Default | Meaning |
|----------|---------|---------|
| `SPARK_SSH` | `192.168.100.2` | SSH target for the Linux peer (`user@host` works) |
| `SPARK_SSH_KEY` | *(unset)* | Optional SSH private-key path |
| `SPARK_CONTROL_HOST` | `192.168.100.2` | Peer control-plane IP (TCP endpoint exchange) |
| `MAC_ROCE_IP` | `192.168.200.1` | Mac RoCE data-plane IP |
| `MAC_ROCE_MAC` | `98:03:9b:80:6a:94` | Mac RoCE endpoint MAC |
| `SPARK_IB_DEV` | `rocep1s0f1` | Peer RDMA device |
| `SPARK_GID_INDEX` | `auto` | Peer RoCEv2 GID index |
| `PHASE2_MTU` | `auto` | RDMA QP MTU (256/512/1024/2048/4096 or auto) |

Set them to match your lab, e.g.:

```sh
SPARK_SSH=labuser@10.0.0.2 SPARK_SSH_KEY=$HOME/.ssh/lab_ed25519 \
  MAC_ROCE_IP=10.0.0.1 MAC_ROCE_MAC=aa:bb:cc:dd:ee:ff \
  ./src/dext/tools/run_phase2_gate.sh
```

---

## Security & safety notes

* **Do not run this on a machine you cannot afford to reboot.** It is an
  experimental kernel-level driver; a bad cycle can wedge the card (recoverable
  via FLR / cold power cycle / reboot, but still disruptive).
* **SIP-off development is a stopgap**, not a release posture. The production
  script (`./src/dext/scripts/mlx_production.sh preflight`) refuses to package while SIP,
  developer boot-args, or system-extension developer mode are present.
* The takeover scripts deliberately refuse to tear down a live RDMA client —
  close QP/MR-holding processes before deactivating the driver.

## License

GPL-2.0 — see [LICENSE](LICENSE). MelonDMA is a port of GPL-2.0-only mlx5
driver code (AppleMCX → MLNX_OFED 5.9), so the project is licensed as a whole
under GPL-2.0. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for the full
provenance and what is deliberately kept out of the repository.

---

## Support the Project

This is a one-person effort, tested against real hardware out of pocket. If
MelonDMA is useful to you and you'd like to help fund new network cards, better
test rigs, and continued development, donations are welcome:

| Currency | Address |
|----------|---------|
| BTC | `bc1q0grdsuh0a5hh360yksa5j07zqme2vgaxuwettl` |
| ETH | `0xC19ee0fE4e39A877Fa170fDa17B551b5e2815BA5` |
| USDT (ERC-20) | `0xC19ee0fE4e39A877Fa170fDa17B551b5e2815BA5` |
| USDC (ERC-20) | `0xC19ee0fE4e39A877Fa170fDa17B551b5e2815BA5` |

USDT/USDC addresses are on the **Ethereum network only** — do not send on
another chain.
