# MelonDMA Architecture

> Current architecture: DEXT 0.539, 2026-09-09. MelonDMA is a macOS DriverKit
> (`.dext`) RoCEv2 provider for the validated ConnectX-4 Lx platform. It exposes an
> ibverbs-compatible userspace API over a capability-driven direct data path.

## Contents

1. [Purpose and boundary](#1-purpose-and-boundary)
2. [Validated platform](#2-validated-platform)
3. [System topology](#3-system-topology)
4. [Control plane](#4-control-plane)
5. [Memory and direct data path](#5-memory-and-direct-data-path)
6. [RDMA object model](#6-rdma-object-model)
7. [Events, interrupts, and health](#7-events-interrupts-and-health)
8. [Isolation and invariants](#8-isolation-and-invariants)
9. [Recovery](#9-recovery)
10. [Userspace provider](#10-userspace-provider)
11. [Deployment boundary and limits](#11-deployment-boundary-and-limits)

## 1. Purpose and boundary

MelonDMA separates a generic mlx5/RoCE transport from application protocols. The DEXT does
not know about models, GGUF, KV caches, RPC, collectives, or application-level routing. It
owns verbs objects, NIC programming, DMA safety, and the UserClient boundary; clients own the
connection protocol and endpoint exchange.

```text
Inference runner / storage / MPI-like application
                    │
     libibverbs_compat + librdma_shim
                    │  IOServiceOpen, ExternalMethod, isolated mappings
                    ▼
               MlxUserClient
                    │  validated per-client generation tokens
                    ▼
             MlxPCIDriver (DriverKit DEXT)
       ┌────────────┼─────────────────────┐
       │            │                     │
   MlxCmd/BAR0   MlxDMA/MR            MlxEQ/MlxUAR
       │            │                     │
       └────────────┴── PD MR MW CQ QP AH SRQ CC ─┘
                    │ PCIe / Thunderbolt tunnel
                    ▼
          ConnectX-4 Lx → Ethernet → RoCEv2 UDP/4791
```

## 2. Validated platform

| Area | Validated scope |
|---|---|
| Mac | Mac Studio M2 Ultra, Apple DART IOMMU, macOS DriverKit |
| NIC | Mellanox ConnectX-4 Lx EN `MCX4131A-BCAT`, PCI `15b3:1015` |
| PCIe path | Thunderbolt/ADT-Link tunnel, Gen3 x4; this is the practical bandwidth ceiling |
| Peer | NVIDIA DGX Spark / ConnectX-7 with rdma-core 50.0, plus local loopback gates |
| Transport | RoCEv2, UDP destination port 4791; RC, UD unicast, and limited UC |

Only the ConnectX-4 Lx backend is supported. A PCI ID entry for a later generation is not a
backend: CX5/6/7/8 need a generation-specific implementation, capability validation, and live
hardware gates.

## 3. System topology

### 3.1 Two planes

| Plane | Operations | Main properties |
|---|---|---|
| Control | context, PD/MR/CQ/QP/SRQ creation, QP transitions, GID setup, capability/health queries | UserClient external methods; every input is validated in the DEXT |
| Data | WQE construction, doorbell, CQE consumption | isolated per-client UAR/doorbell/CQ mappings; direct path is used when capabilities allow it |

Mapped memory reduces syscall crossings; it does not bypass DEXT ownership. The DEXT allocates
the object, pins pages, owns the hardware ID, validates the mapping contract, and can revoke the
object during teardown or recovery.

### 3.2 Core DEXT components

| Component | Responsibility |
|---|---|
| `MlxPCIDriver` | PCI match/open, BAR0/MMIO, lifecycle, framework FLR, interrupt setup, readiness gate |
| `MlxHCAConnectX4` | ConnectX-4 capabilities, registers, WQE and command encodings |
| `MlxCmd` | command queue, mailbox DMA, command ownership, status/syndrome validation |
| `MlxFwPages` | firmware boot/init/runtime page GIVE/TAKE accounting and quarantine |
| `MlxDMA` / `MlxMR` | pinning, IOVA/PAS, MKEY, KLM/UMR, key index, and MR lifetime |
| `MlxEQ` / `MlxCQ` | command, async, page, and completion event queues; CQ progress and moderation |
| `MlxQP` / `MlxAH` / `MlxGID` / `MlxSRQ` | verbs state and RoCE addressing |
| `MlxCC` | DCQCN configuration, status, and statistics |
| `MlxHealth` | health buffer, named syndrome decoding, device-removal checks, FLR entry point |
| `MlxUserClient` | ABI boundary, entitlement check, token ownership, quotas, mapped-memory publication |

## 4. Control plane

### 4.1 PCI and HCA bring-up

`MlxPCIDriver` matches the exact `15b3:1015` PCI function, opens it through DriverKit, maps
BAR0, enables bus mastering, configures MSI-X once, and performs bounded firmware bring-up.
The mlx5 PRM ordering is fixed:

```text
ENABLE_HCA → SET_ISSI → QUERY/MANAGE_PAGES(boot) → SET_HCA_CAP(RoCE)
→ QUERY/MANAGE_PAGES(init) → INIT_HCA → QUERY_HCA_CAP → phase-2 runtime
```

`MlxCmd` validates host delivery, firmware status, and syndrome for every command. A timeout is
not an ordinary command error: potentially referenced DMA memory is quarantined and recovery
uses the verified FLR path.

### 4.2 Firmware pages and RoCE addressing

`MlxFwPages` owns boot, init, and runtime page accounting. A page follows an explicit lifecycle,
such as `ALLOCATED → GIVE_PENDING → GIVEN → RETURNED`. `PAGE_REQUEST` arrives through the EQ
and is handled outside the interrupt callback in bounded batches. Ambiguous ownership is
quarantined until FLR; memory whose firmware ownership is not proved is never freed.

`MlxGID` programs RoCEv2 source addressing and assigns every UserClient an independent GID slot.
`MlxAH` supplies UD address vectors. Clients exchange GID values, not numerical GID indices,
because indices differ between processes and may change after reboot. The provider requires
explicit local GID/IP and local/peer MAC configuration; it intentionally does not implement
NetworkingDriverKit, an `enX` interface, ARP/NDP, or peer discovery.

## 5. Memory and direct data path

### 5.1 Memory registration

`MlxDMA` registers client memory through `IODMACommand::PrepareForDMA`, turns it into IOVA/PAS
entries, and creates an MKEY with lkey/rkey. The DEXT maintains O(1) key lookup and guarantees
that an MR outlives every WQE and CQE that refers to it.

Contiguous IOVA is coalesced on coarse MTT pages, so normal large buffers do not require a
client-visible size split. Fragmented spans can be composed under an indirect KLM MR and
activated with UMR. The userspace provider keeps an MR cache with leases to avoid repeatedly
pinning and registering reused buffers.

Supported facilities include direct and indirect MR, UMR, MW type 1/2, SRQ, and shared UMA Metal
memory. DMA-BUF, ODP, and arbitrary IOVA are intentionally unsupported on macOS.

### 5.2 Direct posting and polling

When the DEXT advertises the isolated mapping ABI, `librdma_shim` maps a per-client UAR,
doorbell record, SQ/RQ, and CQ. The client constructs WQEs, rings the doorbell, and decodes
CQEs without an external-method call per operation:

```text
client WQE build → release/order barrier → UAR doorbell → HCA
HCA CQE DMA      → mapped CQ ring       → client CQ poll
```

The direct path is capability-driven and enabled by default. `MELONDMA_DIRECT_UAR=0`,
`MELONDMA_DIRECT_CQ=0`, or `MELONDMA_FAST_PATH=0` are diagnostic opt-outs, not acceleration
switches. Unsupported or exceptional paths safely use the mediated fallback. BlueFlame is also
capability-driven; a global UAR must never be published.

### 5.3 Apple Silicon UMA

An `MTLBuffer` in `MTLResourceStorageModeShared` exposes host-visible `.contents` and can be
registered as an MR. A remote RDMA WRITE can therefore land directly in shared UMA memory.
This is data placement, not NVIDIA GPUDirect:

- `.private` Metal buffers have no host address and cannot be registered.
- GPU↔NIC ordering still needs CPU/Metal synchronization around RDMA posting and CQ completion.
- A GPU cannot issue BAR/MMIO doorbells; a CPU-side thread performs the post.

## 6. RDMA object model

The DEXT implements PD, MR, MW, CQ, QP, AH, GID, SRQ, and DCQCN configuration. QPs follow:

```text
RESET → INIT → RTR → RTS
              │
              └── error → ERR → RESET or destruction
```

RC supports SEND, immediate variants, SEND_WITH_INV, RDMA READ/WRITE, 64-bit atomics,
LOCAL_INV, BIND_MW, and UMR. UD supports unicast send/receive. UC supports SEND and WRITE,
including immediate, but deliberately rejects READ, atomics, and retry-dependent behavior.

The compatibility layer exposes QPEx for common WQE builder operations, including
`wr_send_inv`. Full `mlx5dv`, DC/XRC/RAW, multicast, and raw vendor-command APIs are outside
the current architecture.

`MlxCQ` owns CQ allocation, consumer progress, arm/notify, and moderation. Completion channels
provide event-driven waiting while direct CQ polling remains available for low latency. Clients
must size CQs for their maximum unconsumed completions and drain them promptly; overflow fails
the affected QP. `MlxSRQ` uses mlx5 RMP semantics and is available to compatible QPs, while the
client remains responsible for receive watermarks and refill.

## 7. Events, interrupts, and health

### 7.1 Interrupts and event queues

The driver configures MSI-X through the DriverKit framework and uses an async vector plus up to
four completion EQs. Command completion is event-driven through the async EQ and needs no
separate command interrupt. `MlxEQ` performs bounded EQE collection in callbacks and defers
heavy work. It handles command, firmware page, port, health, module, CQ error, QP, and SRQ
event classes.

### 7.2 Standard async events

DEXT 0.539 maps firmware CQ/QP/SRQ event IDs to standard `ibv_async_event` values and replaces
raw object IDs with owning-client generation tokens. Its device-wide EQ FIFO removes only an
event belonging to the requesting client, so another UserClient cannot dequeue and lose a
foreign CQ/QP/SRQ event. Device and port events retain device-wide semantics.

The ABI and control route are verified. Natural QP/CQ/SRQ fault injection has not been observed
on the current hardware, so individual fault subtype delivery remains a hardware-observation gap
rather than an unimplemented route.

### 7.3 Health and observability

`MlxHealth` decodes the health buffer and firmware syndromes by name, detects device removal,
and gates new work during a fatal condition. Telemetry includes port counters, per-QP counters,
link/PFC semantic decode, runtime status, GUIDs, HCA clock correlation, and interrupt mapping.

There is deliberately no automatic “three strikes then kill” watchdog: the CX-4 Lx counter is a
snapshot, not a trustworthy heartbeat, so that policy would create false resets.

## 8. Isolation and invariants

`MlxUserClient` is the trust boundary. Its ABI is POD and selector-based; it publishes opaque
generation tokens, never raw firmware IDs. Every resource belongs to one client, and every
handle, range, state transition, length, and access flag is validated inside the DEXT.

Core invariants:

- Command timeout or ambiguous firmware-page ownership implies DMA quarantine and FLR recovery.
- A partially constructed object is never returned to a client.
- Per-client UAR, doorbell, token, GID, and object ownership isolation is mandatory.
- MR backing memory lives until every dependent WQE and CQE completes.
- Receive WRs are posted before SEND/UD traffic; an empty RQ produces RNR NAKs.
- A WC error moves the QP to ERR and flushes outstanding work with `WR_FLUSH_ERR`; errors,
  syndromes, and flushes must never be swallowed.
- FENCE orders a post-Read/Atomic operation; ordinary writes are already ordered by the QP.

Default per-client quotas prevent an application from consuming all HCA objects. An entitlement
may raise them only to firmware-discovered limits.

## 9. Recovery

### 9.1 FLR

`MlxHealth::PerformFlr()` first calls:

```text
IOPCIDevice::Reset(kIOPCIDeviceResetTypeFunctionReset)
```

The framework owns PCI state and MSI-X restoration. A manual FLR-bit write is fallback only:
it historically bypassed the framework and erased the MSI-X table that the kernel restored.

`ReinitFw()` quiesces verbs objects, EQ/UAR/DMA/health state, quarantines firmware pages,
performs FLR, releases the quarantine, and runs full firmware initialization again. It is
live-verified on DEXT 0.539: `mlx_probe --reinit` succeeds and post-recovery preflight and
WR-EX data-path gates pass without reboot.

### 9.2 No-FLR close/open

The soft `TEARDOWN_HCA` and rebuild path is retained for diagnostics, not recovery. The validated
CX-4 Lx firmware does not restore clean RoCE state without FLR; the driver detects this and uses
FLR. This is a firmware limitation, not a missing reset step in the DEXT.

## 10. Userspace provider

`usermode/librdma_shim` is the transport layer to `MlxUserClient`. It manages mappings,
doorbell/CQ mechanics, object requests, and diagnostic queries.

`usermode/libibverbs_compat` exposes 93 ibverbs-shaped symbols, including device/port/GID
queries, PD/MR/MW/CQ/QP/AH/SRQ management, completion channels, async events, QPEx common WR
builders, and `ibv_init_ah_from_wc` / `ibv_create_ah_from_wc` for UD reply addressing.

The provider reads RoCE settings from `MELONDMA_LOCAL_IP`, `MELONDMA_LOCAL_MAC`, and
`MELONDMA_REMOTE_MAC`, or clients can call `ibv_mlx5_configure_roce()` directly. Client-facing
integration details are in [`inference-client-guide.md`](inference-client-guide.md).

## 11. Deployment boundary and limits

### 11.1 Ownership and packaging

The current development bundle contains PCI and scoped UserClient entitlements. Dev takeover is
verified: stopping the live Apple Ethernet DEXT lets the kernel select `MlxPCIDriver`, after
which preflight and WR-EX pass.

That is not a production ownership lifecycle. On the current macOS installation, ordinary
enable+reboot leaves an already-bound Apple PCI nub with Apple even when MelonDMA's personality
is registered. A clean Apple⇄MelonDMA enable/disable cycle is platform-blocked until Apple
provides a supported detach/rematch path or it is demonstrated on a platform where the lifecycle
works without kill, injection, or forced rematch.

The current host is a development configuration with SIP disabled and development boot arguments.
A final distribution still requires Developer ID signing, Apple provisioning profiles,
notarization, and installer validation on a clean SIP-on Mac.

### 11.2 Hardware-dependent validation

DCQCN configuration, status, and statistics are implemented, but ECN/PFC/DCQCN reaction cannot
be validated over a direct Mac↔Linux cable. It needs a managed switched fabric capable of ECN
marking and PFC. The firmware negative `PAGE_REQUEST` teardown path and natural object fault
events likewise need a hardware condition that the present testbed has not produced.

### 11.3 Intentional scope limits

ODP, DMA-BUF, DC/XRC/RAW, full `mlx5dv`, flow steering, multicast, eswitch/SR-IOV, crypto
offloads, and per-QP QoS/rate limiting are not required for the current one-sided RoCE product.
They are future product tracks, not hidden partial implementations.
