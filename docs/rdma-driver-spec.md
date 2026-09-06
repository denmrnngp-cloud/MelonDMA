# Standard RDMA driver spec for Mellanox ConnectX on macOS

## Goal

The driver must look like a standard RDMA/libibverbs provider to an application,
while internally servicing the mlx5 firmware through DriverKit. One shared
transport must work for the supported Mellanox/NVIDIA ConnectX generations;
generation differences are hidden behind a capability-driven HCA layer.

```text
libibverbs-compatible application
  → libibverbs_compat
  → librdma_shim
  → IOUserClient / fixed POD ABI
  → PCIDriverKit DEXT
  → mlx5 firmware + BAR0/DMA/EQ/UAR
  → Ethernet/RoCEv2 (and IB only with separate validation)
```

The DEXT contains no MLX, llama.cpp, GGUF, KV-cache, tensor or RPC semantics.

## Device support

- The PCI match must include only known IDs and safely verify
  vendor/device/revision/subsystem;
- `MlxHCA` selects a model-specific implementation for ConnectX-4/5/6/7/8;
- the shared layer must not hardcode generation details where the firmware
  exposes a capability;
- "support" means a hardware-tested gate, not the presence of a class or an ID in
  a table;
- the first required configuration is Ethernet + RoCEv2 RC.

The card's line rate must not change the API. Real throughput is determined by
the NIC, PCIe and adapter; the RoCEv2 and mlx5 object model stay the same
across generations. For 100G+ fabrics, PFC/ECN and matching PCIe/host
infrastructure are needed separately.

## Layers

### PCI/DriverKit

Owns the PCI function, BAR0, MMIO, PCI Memory Space/Bus Master, DMA setup,
lifecycle, reset, power and interrupts. `Start()` is bounded and fail-closed. On
Apple Silicon the DEXT builds `arm64e`. Production requires Apple PCI transport
and user-client entitlements; takeover under relaxed security is development
only.

### Firmware command plane

The command transport must have a DMA-visible command queue, page-sized mailbox
blocks, correct ownership/doorbells, timeout, and checking of every result level:
delivery status, firmware status, syndrome. On timeout the device and its
mappings are not considered safe.

HCA startup:

```text
ENABLE_HCA → SET_ISSI
→ QUERY_PAGES(BOOT) → GIVE boot pages
→ SET_HCA_CAP(RoCE)
→ QUERY_PAGES(INIT) → batched GIVE init pages
→ INIT_HCA → QUERY_HCA_CAP
```

Runtime `PAGE_REQUEST` is serviced through the EQ/work queue. GIVE/TAKE keeps
per-function ownership accounting; an incomplete or unclear TAKE means
quarantine until a verified FLR.

### DMA/IOMMU

User memory is pinned through DriverKit `IOMemoryDescriptor`/`IODMACommand`,
IOVA segments are translated into 4 KiB HCA PAS entries, then an MKey is created
with access flags. An MR cannot be deregistered while a QP may use its WQEs.
Large buffers need chunked MRs or an indirect KLM/UMR.

### EQ/health

The EQ handles CQ/QP/port/async events, `PAGE_REQUEST` and fatal events.
Owner/phase, wrap, consumer doorbell, arm and bounded fallback polling are
supported; MSI-X is preferred after hardware validation. The callback does not
run heavy firmware commands.

The health monitor detects a stuck firmware, publishes the syndrome, blocks new
operations, disables bus mastering and quarantines DMA. Recovery is only after a
confirmed reset.

### Verbs objects

```text
Context/device → Port/GID
PD → MR/MKey, CQ, QP, AH
QP → SQ/RQ → WQE → CQE
```

Minimal lifecycle:

```text
open → query device/port/GID → alloc PD → reg MR
→ create CQ/QP → RESET → INIT → RTR → RTS
→ post RECV → post SEND/READ/WRITE → poll CQ
→ QP ERR/RESET → destroy QP/CQ → dereg MR → dealloc PD → close
```

Teardown is strictly in reverse dependency order. Every object has an owning
client and is validated by handle, state, range, length, lkey/rkey and access
flags.

## Required userspace API

The API surface must be compatible with a common `libibverbs` subset:

```c
ibv_get_device_list / ibv_open_device / ibv_close_device
ibv_query_device / ibv_query_port / ibv_query_gid
ibv_alloc_pd / ibv_dealloc_pd
ibv_reg_mr / ibv_dereg_mr
ibv_create_cq / ibv_destroy_cq / ibv_poll_cq / ibv_req_notify_cq
ibv_create_qp / ibv_modify_qp / ibv_query_qp / ibv_destroy_qp
ibv_post_send / ibv_post_recv
ibv_create_ah / ibv_destroy_ah
```

Minimal opcodes: `IBV_WR_SEND`, `IBV_WR_RDMA_WRITE`, `IBV_WR_RDMA_READ`; one-SGE,
`wr_id`, signaled/unsignaled, local/remote address, lkey/rkey. The WC returns
status, opcode, byte length, QPN, `wr_id` and vendor syndrome. Inline SEND is
implemented (`max_inline_data=512`); RC atomics FETCH_ADD/CMP_SWAP are
capability-gated with `atomic_result` verified (P3 live gate).

## macOS transport ABI

Linux's `/dev/infiniband/uverbsN` is replaced by `IOServiceOpen` and
`IOUserClient::ExternalMethod`. The shared header contains only POD structures
and stable selectors for query, PD, UAR, MR, CQ, QP, AH, post/poll and async
events.

Every operation checks structure size, version, reserved fields, client ownership
and numeric bounds. An error must not return a partially-built handle. Critical
security properties: exact service matching by `IOUserClass` and bundle ID,
deny-by-default user-client access, mapping revocation, and no shared UAR
between clients.

## Fast path

The base fallback is bounded kernel-mediated posting/polling. After proven
isolation, a direct per-client mapping of SQ/RQ/UAR/doorbell and the CQ consumer
is allowed. The direct path must have:

- separate mappings and ownership;
- bounded queue depths and WR batch size;
- revocation before destroy;
- synchronization of DEXT metadata;
- a fallback when mapping is impossible.

## RoCEv2 contract

The provider picks the active GID and peer MAC/neighbor, programs the address
through firmware and strictly verifies the readback. Ethernet media MTU and the
discrete RoCE QP MTU are different values; the allowed QP MTU is computed from
the peer `active_mtu` and capability. RoCEv2 uses UDP destination port 4791. QP
connection parameters (`qpn`, `psn`, `gid`, address/rkey) are passed over a
separate control channel, not through the DEXT wire protocol.

## Acceptance gates

1. Host tests of the bit encoders, QPC/MKC/WQE and fragmented PAS; ASan/UBSan.
2. Build gate: IIG, DriverKit `arm64e`, signable DEXT and client.
3. Live HCA init with page accounting and no ambiguous ownership.
4. A real peer: GID readback and QP `RESET→INIT→RTR→RTS`.
5. SEND/RECV with payload, guard bytes, CQ/SQ/RQ wrap and error CQEs.
6. A million messages and repeated lifecycle cycles without leak, FLR or drift.
7. RDMA READ/WRITE in both directions with remote verification.
8. Repeated init, explicit reclaim/TAKE and unattended cold reboot.
9. For each new ConnectX model, a separate hardware matrix.
10. Production: Apple entitlements, SIP-on install, Developer ID, notarization,
    safe activate/deactivate.

## Plan toward a full RoCEv2

### R1. Basic provider and object model

- [x] stable enumeration and `mlx5_N` device selection;
- [x] multiple PDs on a UserClient with ownership and dependent teardown;
- [x] RC QP, CQ, MR, AH, SEND/RECV/RDMA READ/RDMA WRITE and CQE error decode;
- [x] `ibv_query_device`, `ibv_query_port`, `ibv_query_gid`, `ibv_query_qp`, AH
  and capability reporting for the actually-supported subset;
- [ ] **Dependency: live hardware gate** for multiple PDs and independent
  QP/CQ/MR lifecycles on the DEXT.

### R2. Full required RoCEv2 RC path

- [x] GID table: enumerate/query all programmed entries, GID type and interface
  identity; add/change/delete verified in the P3 gate (`ibv_query_gid_table` +
  `ibv_mlx5_add_gid`/`del_gid`, IPv4/IPv6/VLAN).
- [ ] **Dependency: a separate NetworkingDriverKit Ethernet driver and
  entitlement.** Only it can create an `enX` interface; the current exclusive
  PCIDriverKit DEXT has no public access to macOS route/ARP/NDP. Programmatic
  endpoint configuration is ready; environment variables are a legacy override.
- [x] QP lifecycle: `RESET/INIT/RTR/RTS/ERR`, `QUERY_QP`, error CQE → `ERR`,
  `2RST_QP` software ring reset; the live recovery gate remains.
- [x] RoCEv2 address-vector validation: IPv4/IPv6, unicast MAC, VLAN validation,
  provider-derived UDP source port, active MTU through the peer gate; UDP
  destination port 4791 is set by the hardware RoCEv2 transport.
- [x] Async event polling: QP/device/port events; CQ arm/`solicited_only` and the
  waitable completion channel passed the P3 live gate; event-loss accounting
  remains.
- [ ] **Next live gate:** the full RC interoperability matrix. Stock Linux
  `ibv_rc_pingpong` smoke and type-2 BIND_MW CQE/live gates passed; the 1M,
  full READ/WRITE in both directions, retry/RNR/error CQE and recovery remain.

### R3. Production provider boundary

- [x] versioned IOUserClient ABI with explicit feature negotiation; diagnostic
  firmware passthrough isolated in a diagnostic-only header;
- [x] source-compatible documented `libibverbs` RC/RoCEv2 subset, including
  `ibv_query_qp`, the AH API and accurate feature refusal;
- [x] per-client quotas, process-death/device-removal cleanup and stale-handle
  resistance (P1.1/P1.2/P0.3 live gates);
- [x] direct path optional; mapping revocation, ordering, wrap and kernel-mediated
  fallback implemented. **Dependency: live long-run gate.**

### R4. Distribution and hardware support

- [ ] **Dependency per generation:** capability-driven Mellanox/NVIDIA ConnectX
  matrix. The current hardware gate covers ConnectX-4 Lx (`15b3:1015`); each
  additional PCI ID needs a generation backend, an Apple PCI entitlement entry,
  capability validation and a live RoCEv2 traffic gate. Do not expose unsupported
  IDs through broad matching. CX5/CX6/CX7 backends are not implemented.
- [x] `.pkg`/ZIP release pipeline, runtime libraries/headers, manifest,
  checksums, install/uninstall path and fail-closed production preflight.
- [ ] **Dependency: Apple approval.** PCI/UserClient entitlements, Developer ID
  profiles, notarization credentials, SIP-on clean-machine
  install/upgrade/deactivate validation.
- [ ] **Dependency: live release matrix.** Cold boot, repeated firmware init,
  reset/recovery, long-run resource drift, bidirectional one-sided IO and the
  compatibility matrix.

## Not in the first full RoCEv2 RC release

UD/DC/XRC/SRQ/multicast, raw Ethernet datapath, the InfiniBand link layer,
vendor-specific collectives, software-RoCE and Apple's private `IORDMAFamily`.
Add them only under a specific confirmed requirement and a hardware test. They
are not required to provide a full RoCEv2 RC driver/API to the user. RC atomics
(FETCH_ADD/CMP_SWAP) are implemented and closed by the P3 live gate.
