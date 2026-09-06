# MelonDMA implementation update

**Date:** 2026-09-05  
**Source of truth:** `dev/src/dext`  
**Hardware:** Apple Silicon Mac Studio M2 Ultra + ConnectX-4 Lx `15b3:1015`  
**Peer:** `denis-local@192.168.100.2`, RDMA device `rocep1s0f1`  
**Local RoCE GID:** index `2`  
**Active build tag:** `p0-p1-dma-lifetime-runtime-1`

This document records the implementation changes, live acceptance results, release artifacts, and remaining blockers from the September 5 readiness work.

## Status summary

The data path is accepted for the tested ConnectX-4 Lx platform:

- RC/RoCEv2 SEND/RECV, RDMA READ/WRITE, WRITE_WITH_IMM, atomics, inline, multi-SGE, QP recovery and reconnect traffic passed live gates.
- Multi-client ownership isolation and surviving-client traffic passed.
- Stale and cross-client tokens are rejected.
- CQ quota, lifetime, ABI fuzz, soak, CQ event and direct-UAR gates passed.
- Metal unified-memory zero-copy passed in both directions, including a 4 MiB indirect MR.
- Final-KV RDMA WRITE support exists in the llama.cpp RPC path and now emits an explicit runtime marker.
- A local developer flat package and reproducible release evidence collector are available.

The following are not production-accepted:

- Apple Developer ID signing, DriverKit provisioning, notarization and SIP-on install.
- Clean-machine production installation and cold-boot takeover.
- Destructive no-FLR/FLR, unplug/reconnect, sleep/resume and fatal-injection tests.
- Dedicated completion MSI-X delivery. Timer polling remains the proven fallback.
- CX5/CX6/CX7/CX8 support.

## Code changes

### UserClient security boundary

`dev/src/dext/Sources/MlxUserClient.cpp` now reads the opening task's entitlement through `IOUserClient::CopyClientEntitlements`.

Recovery and raw firmware selectors remain denied by default. The intended diagnostic entitlement is:

```text
com.mlx5.rdma.diagnostic
```

The diagnostic client is built from `tools/diagnostic.entitlements`, while normal verbs/runtime clients continue to use `tools/reinit.entitlements` without the diagnostic key.

This does not claim Apple approval for an arbitrary entitlement. On the current developer machine the local signature can carry the key, but the live DEXT authorization still returns `0xe00002e2` until a supported Apple-approved privileged path exists. No UID, process name, environment variable, or global allow-any-userclient bypass is used.

Affected files:

- `dev/src/dext/Sources/MlxUserClient.cpp`
- `dev/src/dext/tools/diagnostic.entitlements`
- `dev/src/dext/Makefile`

### Direct-UAR gate

`dev/src/dext/tools/run_phase3_direct_uar_gate.sh` was updated for current telemetry.

The gate now requires:

- `direct SQ mapped`;
- `DIRECT_UAR_STATS`;
- non-zero `mapped_qps`;
- non-zero `direct_wrs`;
- non-zero `direct_doorbells`;
- non-zero `direct_recv_wrs`;
- `fallback_send=0`;
- `fallback_recv=0`.

`direct_cq_consumers=0` is not a failure in the smoke path because the current CQ consumer is kernel-mediated. The gate result is:

```text
P3_DIRECT_UAR PASS
```

### CQ quota contract

The runtime contract was rechecked rather than changed speculatively. The current implementation reserves the DB slot before CQ creation and rolls back on failure. The quota gate passes:

```text
P1.1_QUOTA PASS
```

The exposed limit must continue to be treated as a runtime capability, not inferred from a stale observation. No blind quota change was made.

### Metal registration and lifetime

The supported Metal contract remains:

- Apple unified memory;
- `MTLResourceStorageModeShared`;
- stable `MTLBuffer.contents`;
- explicit ownership and lifetime protocol;
- `.untracked` only when the application supplies external NIC/GPU ordering;
- no `.private` registration;
- no GPU-issued PCIe UAR or GPUDirect promise.

`MlxRegisteredMetalBuffer` protects the persistent MR/PD, allocation generation, device epoch, slot leasing and close/lifetime behavior.

## Final-KV path

The llama.cpp implementation is in:

```text
/Users/macstudio/llama.cpp/ggml/src/ggml-rpc/ggml-rpc.cpp
```

When all of the following are true:

- RDMA transport is active;
- `GGML_RPC_RDMA_WRITE_KV=1`;
- `GGML_RPC_RDMA_FINAL_DEST=metal`;
- the request has a final stable destination;
- destination registration returns an rkey;
- a notify token is available;

The client sends destination address, size, rkey and notification metadata. The server writes the tensor in RDMA chunks and places the final immediate notification on the last WRITE. The successful path returns without the ordinary trailing payload copy/response path.

The server now emits:

```text
GGML_RPC_FINAL_DEST_ACTIVE kind=<n> bytes=<n> chunks=<n> rdma_write=1 trailing_copy=0
```

A benchmark must not claim final-KV zero-copy unless this marker is present. The environment variable alone is not evidence.

The RDMA-enabled llama-server was rebuilt with the repository's current CMake build:

```text
/Users/macstudio/llama.cpp/build-rdma-macos/bin/llama-server
```

The old top-level Makefile is no longer the build interface for llama.cpp.

## Live Metal acceptance

The live contract now checks both positive and negative cases:

- `.shared` buffer registration succeeds;
- GPU producer and consumer see the same data as the NIC;
- `.private` buffer registration is rejected;
- nil buffer input is rejected;
- close while leases are active returns `EBUSY`;
- stale/lifetime checks remain active.

The peer Metal gate passed:

```text
PHASE3_WRITE_GATE PASS
PHASE3_REVERSE_WRITE_GATE PASS
METAL_DMA_GATE PASS
```

The tested matrix includes:

- 32 x 1 MiB GPU-to-peer one-sided writes;
- 32 x 1 MiB peer-to-Metal reverse writes;
- 4 MiB peer-to-Metal indirect MR;
- GPU consumer verification with zero mismatches;
- direct UAR with zero SEND/RECV fallback.

Observed direct telemetry includes non-zero direct WQEs/doorbells and zero send/receive fallback counters.

## Hardware support policy

`dev/src/dext/SUPPORTED_HARDWARE.md` is the release-facing matrix.

Supported release target:

```text
Mellanox/NVIDIA ConnectX-4 Lx PF
PCI vendor/device: 15b3:1015
Ethernet RoCEv2 RC
```

The DEXT personality matches only:

```text
0x101515b3
```

CX4 PF/VF variants and CX5-CX8 are not claimed, even where factory scaffolding exists. Each additional generation requires a dedicated backend, capability validation, and its own live acceptance record.

Unsupported/deferred features include UD, DC, XRC, SRQ, multicast, raw Ethernet QPs, InfiniBand link layer, software RoCE, automatic route discovery, GPUDirect for Metal `.private`, and GPU-issued UAR.

## Developer packaging

`dev/src/dext/scripts/mlx_developer_package.sh` creates a local developer-mode flat package without pretending to be production signed or notarized.

Run:

```sh
cd dev/src/dext
DIST_DIR=build/developer-package-final ./scripts/mlx_developer_package.sh
```

Artifact:

```text
build/developer-package-final/MlxRDMA-developer-flat.zip
```

The flat DEXT layout is:

```text
com.mlx5.rdma.dext.dext/
  Info.plist
  MlxRDMA
  MlxRDMA.entitlements
```

The package manifest records version, build tag, PCI match, signing status and notarization status. It is not a replacement for the Apple production package path.

## Evidence collection

`dev/src/dext/scripts/mlx_release_evidence.sh` collects reproducible local evidence without installing, resetting, unplugging or FLR-ing the device.

Run:

```sh
cd dev/src/dext
./scripts/mlx_release_evidence.sh build/release-evidence/<timestamp>
```

The bundle captures:

- source Info.plist and entitlements;
- macOS and hardware data;
- PCI and IORegistry state;
- system extension state;
- network configuration;
- peer, control host and GID context;
- host tests;
- DEXT syntax checks;
- Metal contract;
- CQ quota;
- direct-UAR gate;
- live Metal contract;
- SHA256 checksums.

The final collected bundle was:

```text
 dev/src/dext/build/release-evidence/final/
```

Its recorded gate summary was:

```text
host-check=0
dext-check=0
metal-contract=0
quota=0
direct-uar=0
metal-live=0
```

## Benchmark contract for llama.cpp

The complete operator guidance is in:

```text
/Users/macstudio/llama.cpp/RDMA_BENCH_RECOMMENDATIONS.md
```

Required Mac settings:

```sh
export MELONDMA_RUNTIME=/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/src/dext/build
export GGML_RDMA_DEV=mlx5_0
export GGML_RDMA_GID=2
export MELONDMA_DIRECT_UAR=1 MELONDMA_DIRECT_CQ=1
export MELONDMA_BLUE_FLAME=1 MELONDMA_HW_CQ_EVENT=1
export MELONDMA_SINGLE_THREADED=0
export GGML_RPC_REQUIRE_RDMA=1 GGML_RPC_RDMA_STATS=1
export GGML_RPC_RDMA_WRITE_KV=1 GGML_RPC_RDMA_FINAL_DEST=metal
export GGML_RPC_RDMA_ENABLE_BATCH=1
export GGML_RPC_RDMA_SIGNAL_INTERVAL=8
```

Required Spark settings:

```sh
export GGML_RDMA_DEV=rocep1s0f1
export GGML_RDMA_GID=3
export GGML_RPC_REQUIRE_RDMA=1 GGML_RPC_RDMA_STATS=1
export MELONDMA_DIRECT_UAR=1 MELONDMA_DIRECT_CQ=1
export MELONDMA_BLUE_FLAME=1 MELONDMA_HW_CQ_EVENT=1
```

Recommended sweep:

- modes: `disagg`, `split`;
- contexts: 512, 2048, 8192, 16384, 32768, 65536;
- generation: 32 tokens for latency, 256 for decode stability;
- 2 warmups and 5 measured requests;
- batch on/off;
- mailbox on/off;
- signal interval 1/8/32;
- signaling 0/1;
- QP count 1/2/4/8;
- RDMA-required run and TCP comparison with the same model/config.

Record prefill tok/s, decode tok/s, ITL p50/p95, TTFT, KV bytes, KV WRITE time, RPC queue/wait time, Metal wait time, Mac/Spark CPU, CQE errors, retry/RNR counters, fallback counters and the final-destination marker.

Never compare results with different DEXT build tags, firmware, model quantization, GID, MTU, mailbox, batch mode, signal interval or context settings.

## Remaining release blockers

### Apple-controlled

- Developer ID Application signing;
- Developer ID Installer signing;
- Apple DriverKit provisioning profiles;
- notarization;
- SIP-on clean-machine activation.

The existing `scripts/mlx_production.sh` intentionally refuses production packaging when these are absent.

### Hardware/system controlled

- no-FLR privileged diagnostic access;
- FLR recovery and post-recovery traffic;
- process death during traffic;
- Thunderbolt unplug/reconnect;
- sleep/resume;
- cold boot;
- fatal injection;
- suspend/resume matrix;
- dedicated completion MSI-X proof and IRQ-vs-timer benchmark.

These tests require a recovery harness and an explicitly controlled hardware window. They were not run automatically.

## Verification commands

```sh
cd dev/src/dext
make check-host check-dext check-metal-contract
./tools/run_p1_1_quota_gate.sh
SPARK_SSH=denis-local@192.168.100.2 \
SPARK_CONTROL_HOST=192.168.100.2 \
SPARK_GID_INDEX=2 \
./tools/run_phase3_direct_uar_gate.sh
SPARK_SSH=denis-local@192.168.100.2 \
SPARK_CONTROL_HOST=192.168.100.2 \
SPARK_GID_INDEX=2 \
./tools/run_metal_dma_gate.sh
```

All of the above completed successfully during this update.
