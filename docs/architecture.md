# MelonDMA — Architecture

> Current as of the code tree (`dev/src/dext/Sources/`) and the 2026-09-07 state.
> MelonDMA is a macOS **DriverKit (DEXT)** **RoCEv2 RC/UD** provider for Mellanox/NVIDIA
> ConnectX, with a libibverbs-compatible userspace layer (`libibverbs_compat` + `librdma_shim`).

## Contents

1. [Purpose and boundaries](#1-purpose-and-boundaries)
2. [Hardware and platform facts](#2-hardware-and-platform-facts)
3. [Components and files](#3-components-and-files)
4. [Data path](#4-data-path)
5. [Correctness invariants](#5-correctness-invariants)
6. [Limits and risks](#6-limits-and-risks)

## 1. Purpose and boundaries

The driver separates the universal mlx5/RoCE transport from application-level protocols
(MLX, llama.cpp, any runner). The DEXT knows **nothing** about tensors, GGUF, KV-cache, RPC
or collectives — only verbs objects and the transport.

```text
MLX / llama.cpp / any application (verbs client)
                │
      libibverbs_compat / librdma_shim
                │  IOServiceOpen + ExternalMethod (POD ABI) + mapped rings
                ▼
             MlxUserClient            ← trust boundary (validates every handle)
                │
                ▼
             MlxPCIDriver (DEXT, owner of the PCI function)
   ┌────────────┼──────────────┬───────────────┐
  BAR0/CMD     MlxDMA         MlxEQ/MlxUAR    mlx5 objects (ib/)
   (MMIO)     (IOMMU/PAS)    (events/doorbell) PD MR CQ QP AH SRQ CC
   └────────────┴──────────────┴───────────────┘
                │ PCIe · ADT-Link Gen3 x4 (Thunderbolt)
          Mellanox ConnectX → RoCEv2 (UDP/4791)
```

## 2. Hardware and platform facts

- Mac Studio **M2 Ultra** (24 CPU cores, 192 GB unified memory, macOS 26.6.1).
- Mellanox **ConnectX-4 Lx EN `MCX4131A-BCAT`**, PCI `15b3:1015` (subsystem `15b3:0005`),
  firmware `0x0016000e` / 14.22.2560. The only confirmed card.
- The card sits behind a Thunderbolt tunnel (ADT-Link / Wavlink UTE02), PCIe **Gen3 x4**
  (8 GT/s × 4 ≈ 31.5 Gbit/s per direction) — this is the hardware ceiling, not the protocol.
- Peers: NVIDIA DGX Spark (ConnectX-7, rdma-core 50.0), plus loopback.
- The IOMMU is Apple **DART**; DMA registration goes through `IODMACommand::PrepareForDMA`
  from the NIC's PCI function.

## 3. Components and files

### 3.1 `MlxPCIDriver` — owner of the PCI function
`Sources/MlxPCIDriver.cpp` (+ `.iig`)

- PCI matching (`15b3:1015`), `IOPCIDevice::Open`, BAR0, PCI Command Register, MMIO.
- DriverKit lifecycle, power transitions, teardown.
- **MSI-X**: a single `ConfigureInterrupts(2, 9)` call (2 required, 9 desired);
  `IOInterruptDispatchSource` on probed host indices (async → host 0, completion → host 1).
- **`ProgramMsix`** (selector `kMlxUCMethodProgramMsix`) — read/program the MSI-X table.
- **`PerformFlr()`**: first `IOPCIDevice::Reset(kIOPCIDeviceResetTypeFunctionReset)` — the
  kernel then restores the MSI-X table itself; a manual FLR-bit write is only the fallback.
- Creates the HCA, the command plane and the UserClient; fail-closed readiness gate.
  `Start()` must stay bounded (it runs inside a PM transition).

### 3.2 `MlxHCA` — hardware abstraction
`Sources/hw/` — `MlxHCA.hpp`, `MlxHCAConnectX4.cpp`

- Capability-driven sizing from `QUERY_HCA_CAP`. Generation classes (CX4, future CX5/6/7/8)
  carry only PCI ID, revision, register/capability differences. Today `CreateCx5/6/7`
  return NULL — **a PCI-ID table entry ≠ support** without a backend + capability validation
  + a live gate.
- `MlxRegs.hpp`, `MlxP0Encoding.hpp`, `MlxP1Encoding.hpp`, `MlxP0EncodingIndirect.hpp`,
  `MlxWQE.hpp`, `MlxDoorbell.hpp`, `mlx5_bits.h` — command and WQE encoders/decoders.

### 3.3 `MlxCmd` — firmware command plane
`Sources/core/MlxCmd.{hpp,cpp}`

- Owns the command queue, ownership, doorbell, completion polling and the mailbox chain.
- Checks delivery status + firmware status + syndrome. Mailboxes are full page-sized DMA
  blocks (`MLX_CMD_MAX_SIZE` ≈ 4112 bytes); `block_num` in ascending order.
- **A timeout is not a normal error**: related DMA mappings go to quarantine; the only
  recovery is a verified FLR.
- HCA startup sequence (order is fixed):

```
ENABLE_HCA → QUERY/SET_ISSI → QUERY_PAGES(BOOT) → MANAGE_PAGES(GIVE)
→ SET_HCA_CAP(RoCE) → QUERY_PAGES(INIT) → MANAGE_PAGES(GIVE)
→ INIT_HCA → QUERY_HCA_CAP
```

### 3.4 `MlxFwPages` — firmware page ownership
`Sources/core/MlxFwPages.{hpp,cpp}`

- Boot/init/runtime pages + `PAGE_REQUEST` via the EQ. Handling happens outside the callback,
  in batches.
- States `ALLOCATED → GIVE_PENDING → GIVEN → RETURNED`; ambiguous ownership → `QUARANTINE`
  until a verified FLR (such memory must not be freed).

### 3.5 `MlxDMA` — IOMMU / memory registration
`Sources/core/MlxDMA.{hpp,cpp}`, `MlxKeyIndex.hpp`

- `IODMACommand` → IOVA segments → 4 KiB PAS → `CREATE_MKEY` → lkey/rkey.
- A direct MKEY ≈ **1.875 MiB** (480 × 4 KiB PAS). Larger buffers are chunked into direct
  children + composed under an **indirect (KLM) MR** (`RegMRIndirect` + `PostUmrKlm`), up to
  **240 children ≈ 450 MiB** per `ibv_reg_mr` (`MLX_UC_MAX_INDIRECT_CHILDREN=240`).
- O(1) lkey/rkey→slot index (was an O(512) scan per SGE).
- An MR's lifetime must outlive every WQE/CQE that references it.

### 3.6 `MlxEQ` / `MlxHealth`
`Sources/core/MlxEQ.{hpp,cpp}`, `MlxHealth.{hpp,cpp}`

- `MlxEQ`: EQ DMA ring, consumer index, arm, dispatch. The callback only reads the EQE and
  defers heavy work. Three-tier timer (100/10/50 ms depending on allocated CQs/vectors).
- `MlxHealth`: periodically reads the health counter and syndrome; on a fatal state it blocks
  new operations, disables bus mastering and quarantines DMA. Currently a skeleton.

### 3.7 `MlxUAR` — user access region / doorbell
`Sources/core/MlxUAR.{hpp,cpp}`

- Hands out UARs and computes BAR offsets. **Per-client isolation is mandatory**; a global UAR
  must never be published. Blue-flame registers are distributed per QP; the hardware limit is
  `bf_regs_per_uar = 4` (from the 5th QP registers are shared and posting serializes).

### 3.8 RDMA object layer — `Sources/ib/`
- `MlxPD` — protection domain / ownership.
- `MlxMR` — memory registration, access flags, indirect/KLM, UMR.
- `MlxCQ` — completion ring, consumer progress, moderation.
- `MlxQP` — RC/UD state machine `RESET → INIT → RTR → RTS` (+ `ERR/RESET`, `2RST_QP`).
- `MlxAH` — address vector (for UD).
- `MlxGID` — RoCEv2 addressing; **each client gets its own GID slot**.
- `MlxSRQ` — Shared Receive Queue = **RMP** (a linked list, not a ring); QP binding via
  `rq_type=1` + `srqn_rmpn_xrqn`; such a QP's CQ is read only by the kernel (single reader).
- `MlxCC` — congestion control (DCQCN): wraps `QUERY/MODIFY_CONG_PARAMS`; **the firmware runs
  the DCQCN loop**, the driver only configures it.
- `MlxRoCE` — verbs-subset entry, GID program/readback, MTU, UDP dport 4791, `AccessReg`,
  `PortStats` (PPCNT).

Teardown goes in strict reverse dependency order: stop posting → QP → CQ → MR → AH/UAR → PD.

### 3.9 `MlxUserClient` — the process boundary
`Sources/MlxUserClient.cpp` (+ `.iig`), `Sources/userclient/MlxUCIO.h`, `MlxServiceMatch.h`

- POD ABI, fixed selectors (query, PD, UAR, MR, CQ, QP, AH, SRQ, post/poll, ProgramMsix,
  PortStats, AccessReg, async events). Handles are **opaque per-client tokens with generation**
  (ABI v2), resolved only inside the DEXT.
- Matching is exact by `IOUserClass=MlxPCIDriver` + bundle ID; deny-by-default.
- Every resource belongs to a client; all handles/ranges/states/lengths/access flags are
  validated in the DEXT. Entitlements read via `IOUserClient::CopyClientEntitlements`.

Per-client quotas (defaults; the `com.mlx5.rdma.entitlement` entitlement raises them to the
firmware caps): PD 16, QP 64, CQ 64, MR 128, MW 128, AH 8, GID 16. SQ/RQ depth ≤ 4096,
SGE ≤ 16, inline ≤ 512 B, batch ≤ 64 WR.

Feature bits (`MlxUCIO.h`): RC, ROCE_V2, DIRECT_PATH, ASYNC_EVENTS, INDIRECT_MR,
QP_RECOVERY, MULTI_SGE, IMMEDIATE_DATA, HEALTH_QUERY, STATS, INLINE, ATOMIC,
TRUSTED_FAST_PATH, BLUE_FLAME, CQ_INTERRUPT, COHERENT_UMA_MR, CQ_EVENT_WAIT,
RUNTIME_STATUS.

### 3.10 Userspace
`Sources/../usermode/`

- `librdma_shim` — thin transport to the UserClient (mapped rings, the shadow page,
  `rdma_query_port_stats`/`rdma_access_reg`).
- `libibverbs_compat` — ibverbs-shaped API; exports **44 symbols** (exact count, verified with
  `objdump -T` against `ib_write_bw`/`ib_send_lat`/`rping`/`libuct_ib`), including a real
  `ibv_qp_to_qp_ex` (a WR chain between `wr_start`/`wr_complete` goes out in one call).
- `metal/MlxRegisteredMetalBuffer.mm` — the contract for registering `.shared|.untracked`
  MTLBuffers.

## 4. Data path

1. The application opens the device and gets the port/GID (its own slot).
2. `alloc_pd` → `reg_mr` → `create_cq` → `create_qp`.
3. QP `RESET→INIT→RTR→RTS`; the `{qpn, psn, gid, rkey/addr}` exchange happens over a separate
   control channel.
4. Post RECV/SEND/READ/WRITE: the WQE lands in SQ/RQ, the doorbell wakes the HCA.
5. The HCA writes a CQE; userspace polls the CQ (checking wr_id, status, opcode, byte_len,
   syndrome).
6. A QP/firmware error blocks the data path and surfaces to the application; silent swallowing
   is forbidden.

**Direct (zero-syscall) path** (`MELONDMA_DIRECT_UAR=1` / `MELONDMA_DIRECT_CQ=1`):
SQ/RQ/CQ/UAR/doorbell are mapped into the client; the WQE is built and the doorbell rung in
userspace, and CQEs are decoded from the mapped ring. Exceptional operations (UMR/LOCAL_INV,
UD pairs, SRQ receive, unknown QP, lost WR metadata) fall back to the kernel path.

**Trusted fast path** (`MLX_UC_FEATURE_TRUSTED_FAST_PATH` + `MLX_UC_QP_TRUSTED`):
shadow state (seqlock: `sq_head/sq_tail/rq_head/rq_tail`) is published in the shared DB-record
page, so post/poll never cross into DriverKit. The trust model is the same as `SyncQpTails`
(a client could already monotonically advance the tails).

## 5. Correctness invariants (non-negotiable)

- A command timeout → quarantine of the related DMA mappings; only a verified FLR recovers.
- Ambiguous firmware page ownership → quarantine.
- Validate at the DEXT boundary; a partially built handle is never returned.
- CQ depth ≥ Σ(SQ+RQ) of all QPs on that CQ (+10–20 %); otherwise overflow → QP ERR.
- RNR: Recv WRs must be pre-posted; an empty RQ → RNR NAK → latency spikes.
- An MR outlives every WQE/CQE that references it.
- After any WC error the QP → ERR and the queue flushes (`WR_FLUSH_ERR`); never swallow it.
- FENCE is needed only to order a WR *after* a prior Read/Atomic; two WRITEs are already ordered.
- MSI-X: `PerformFlr` must go through the framework `Reset()`, otherwise the driver wipes the
  table the kernel just filled (this was the root cause of "MSI-X never delivered").

## 6. Limits and risks

- Only ConnectX-4 Lx `15b3:1015`; CX5/6/7/8 are preparation, not support.
- No NetworkingDriverKit (`enX`, ARP/NDP, auto-discovery) — explicit GID/MAC config; the peer
  needs a static neighbour entry.
- No GPUDirect in the NVIDIA sense; the Apple equivalent for data is registering a
  `.shared|.untracked` `MTLBuffer.contents` as an ordinary MR (NIC↔UMA↔Metal without a CPU copy).
  A GPU-issued doorbell (BAR/MMIO) is impossible.
- The performance ceiling is the PCIe Gen3 x4 tunnel (~25–28 Gbit/s practical).
- Apple entitlements (`transport.pci` + `userclient-access`) are still pending; the dev path is
  Apple Development signing + SIP off + IOCatalogue injection (though
  `mlx_cold_takeover.sh resume-standard` now achieves a standard capture without injection).
- Remaining code items: a registration cache (registration costs 1.18–1.75 ms — the largest
  item), several UAR pages per client, DCQCN requires a switched fabric.
