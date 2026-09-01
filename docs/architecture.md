# MelonDMA Architecture

## Purpose

MelonDMA is a macOS DriverKit driver for RDMA over Mellanox/NVIDIA ConnectX. The
driver separates the universal mlx5/RoCE transport from MLX, llama.cpp and any
application-level protocol.

```text
MLX / llama.cpp / libibverbs application
                │
      libibverbs_compat / librdma_shim
                │  IOServiceOpen + ExternalMethod
                ▼
             MlxUserClient
                │
                ▼
             MlxPCIDriver (DEXT)
       ┌────────┼────────┬──────────┐
     BAR0      DMA      EQ/UAR    mlx5 objects
       │        │         │       PD MR CQ QP AH
       └────────┴─────────┴──────────┘
                │ PCIe · ADT-Link Gen3
          Mellanox ConnectX → RoCEv2
```

## Components and responsibilities

### `MlxPCIDriver` — owner of the PCI function

`src/dext/Sources/MlxPCIDriver.*`

- PCI matching, `IOPCIDevice::Open`, BAR0 and the PCI Command Register;
- MMIO and firmware version readback;
- DriverKit lifecycle, power transitions, teardown and FLR;
- creation of the HCA, the firmware command plane and the UserClient;
- fail-closed readiness gate.

`Start()` must stay bounded: DriverKit runs it inside a power-management
transition, so waiting indefinitely for firmware is not allowed.

### `MlxHCA` — hardware abstraction

`src/dext/Sources/hw/`

The shared mlx5 code uses capability-driven sizing. Model-specific classes
(`ConnectX4`, future `ConnectX5/6/7/8`) contain only the PCI ID, revision,
register/quirk and capability differences. A PCI-ID table entry is not support
without a hardware gate.

### `MlxCmd` — firmware command plane

`src/dext/Sources/core/MlxCmd.*`

Owns the command queue, ownership, doorbell, completion polling, the mailbox
chain, and checking of delivery status, firmware status and syndrome. Mailboxes
are full page-sized DMA blocks; multi-block `block_num` goes in ascending order.
Timeout is not a normal error: the related DMA mappings go to quarantine.

HCA initialization:

```text
ENABLE_HCA → QUERY/SET_ISSI → QUERY_PAGES(BOOT)
→ MANAGE_PAGES(GIVE) → SET_HCA_CAP(RoCE)
→ QUERY_PAGES(INIT) → MANAGE_PAGES(GIVE)
→ INIT_HCA → QUERY_HCA_CAP
```

### `MlxFwPages` — firmware page ownership

Manages boot/init/runtime pages and `PAGE_REQUEST` through the EQ. Requests are
handled outside the callback, in batches. States:
`ALLOCATED → GIVE_PENDING → GIVEN → RETURNED`; ambiguous ownership moves the
memory to `QUARANTINE` until a verified FLR. Such memory must not be freed.

### `MlxDMA` — IOMMU and memory registration

Creates an `IODMACommand`, pins client memory, gets IOVA segments and splits
host segments into 4 KiB PAS entries for the HCA. `CREATE_MKEY` binds the PAS to
lkey/rkey. The MR lifetime must outlive every WQE/CQE that references it.

The direct MKEY path is limited to roughly 1.875 MiB; larger logical buffers use
chunking or an indirect KLM/UMR. This is an ABI/implementation limit, not a RoCE
limit.

### `MlxEQ` and `MlxHealth`

`MlxEQ` owns the EQ DMA ring, the consumer index, arm and dispatch. It currently
uses bounded polling about every 10 ms; MSI-X is a future optimization. The
callback only reads/copies the EQE and defers heavy work.

`MlxHealth` periodically reads the health counter and syndrome. On a fatal
firmware state, new operations are blocked, bus mastering is disabled and DMA is
quarantined; after that, recovery is only possible through a verified reset.

### `MlxUAR` and doorbells

`MlxUAR` hands out UARs and computes BAR offsets. The direct fast path publishes
isolated per-client SQ/RQ/UAR/doorbell mappings and the CQ consumer;
kernel-mediated posting remains the fallback (`MELONDMA_FAST_PATH=0`). A global
UAR must never be published to multiple clients without isolation and
revocation.

### RDMA object layer

`src/dext/Sources/ib/`

- PD/XRCD — domains and ownership;
- MR/MKey — registered memory and access flags;
- CQ — completion ring and consumer progress;
- QP — RC state machine `RESET → INIT → RTR → RTS`;
- AH/GID/RoCE — addressing and peer selection;
- CC — congestion-control parameters.

Teardown goes in reverse dependency order: stop posting → QP → CQ → MR → AH/UAR
→ PD.

### `MlxUserClient` — the process boundary

`src/dext/Sources/MlxUserClient.*`, `userclient/MlxUCIO.h`

A POD ABI with fixed selectors for query, PD, UAR, MR, CQ, QP, AH, post/poll and
async events. Every resource belongs to a client, and all handles, ranges,
states, lengths and access flags are validated in the DEXT. The UserClient is
matched exactly by `IOUserClass=MlxPCIDriver` and bundle ID, not by a generic
`IOUserService`.

### Userspace libraries

- `librdma_shim` — thin macOS transport to the UserClient;
- `libibverbs_compat` — ibverbs-shaped API for the RC subset;
- consumer layers — MLX and llama.cpp/RDMA RPC — were removed from the repo as
  bespoke integrations; their place is taken by the documented generic verbs API
  (`docs/inference-client-guide.md`).

## Data path

1. The application opens the device and gets the port/GID.
2. It allocates a PD, registers an MR, creates CQ/QP.
3. The QP goes `RESET → INIT → RTR → RTS`; the `{qpn, psn, gid, rkey/address}`
   exchange happens over a separate control channel.
4. The application posts RECV and SEND/READ/WRITE. The WQE lands in SQ/RQ, the
   doorbell notifies the HCA.
5. The HCA writes a CQE; userspace polls the CQ and checks `wr_id`, status,
   opcode, byte length and vendor syndrome.
6. A QP/firmware error blocks the datapath and is surfaced to the application; a
   silent generic error is not allowed.

## Boundaries and current risks

The DEXT knows nothing about tensor layout, GGUF, KV-cache, RPC or collective
semantics. ConnectX-4 Lx and a peer ConnectX-7 over RoCEv2 are confirmed. For
production, cold-boot takeover, repeated `INIT_HCA`/reclaim and SIP-safe
entitlement/signing remain. Current throughput is also limited by the PCIe Gen3
x4 host link (ADT-Link adapter).
