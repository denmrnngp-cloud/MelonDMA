# MelonDMA Client Guide

MelonDMA exposes RoCEv2 on macOS through a compatible `libibverbs` layer:

```text
client → libibverbs.dylib / librdma_shim.dylib → MlxUserClient → MlxRDMA DEXT → ConnectX
```

The driver knows nothing about models, tensors, KV caches, or RPC. The client owns the control
connection, endpoint exchange, memory-access protocol, object lifetime, and error handling.
This is the contract for every client: an inference runner, MPI/NCCL-like transport, storage
system, or custom verbs application.

## 1. Prerequisites before the application starts

### 1.1 Card ownership and health

Before a client starts, the card must be owned by **`MlxPCIDriver`**, not
`DriverKit_AppleEthernetMLX5`. On the Mac, check:

```sh
systemextensionsctl list
ioreg -r -n ethernet@0 -l -w 0 | rg 'MlxPCIDriver|DriverKit_AppleEthernetMLX5|IODEXTMatchCount'
cd /path/to/MelonDMA/dev/src/dext && ./build/mlx_phase2_gate --preflight
```

The last command must return `PHASE2_PREFLIGHT PASS`; it validates the UserClient, link UP,
and basic health. If Apple still owns the card, the client must not attempt to take it over.
That is an installation/operator concern, not an application's data path responsibility.

On the current development platform, ordinary `enable + reboot` does not always cause an
Apple→MelonDMA rematch. A development-only takeover exists for driver engineers, but it is
not part of a client or production installation workflow.

### 1.2 Client application signing

`MlxUserClient` is available only to an application with the scoped entitlement:

```xml
<key>com.apple.developer.driverkit.userclient-access</key>
<array><string>com.melondma.rdma.dext</string></array>
```

The library can link into any process, but only a correctly signed client can open the actual
device. In production the entitlement must be present in the provisioning profile; an ad-hoc
command-line binary is not a supported way to access the UserClient.

### 1.3 Network and peer

RoCEv2 is supported. The local IP/GID and the MAC address of both endpoints must be known.
The driver intentionally does not perform ARP or automatic peer discovery. For a direct
Mac↔Linux link, set on the Mac:

```sh
export MELONDMA_LOCAL_IP='192.168.200.1'
export MELONDMA_LOCAL_MAC='02:00:00:00:00:01'
export MELONDMA_REMOTE_MAC='02:00:00:00:00:02'
```

Alternatively, after `ibv_open_device()`, call `ibv_mlx5_configure_roce()` with the same GID,
MAC addresses, traffic class, hop limit, and UDP source port. The API is preferable when a
client manages several network profiles; environment variables are suitable for a simple launch.

Every `ibv_open_device()` receives its own GID slot. Do not persist or share a numerical GID
index on the Mac: it changes across reboots and differs between clients. Obtain the GID for the
current context with `ibv_query_gid()` and send the **16-byte GID itself** to the peer.

On Linux, select a GID by local address using the runner's own configuration (for example,
`GGML_RDMA_GID_ADDR` in a GGML integration), rather than a fragile numerical index.

## 2. Linking the libraries

The compatible API header is:

```text
dev/src/dext/usermode/libibverbs_compat/include/infiniband/verbs.h
```

A development build produces `build/libibverbs.dylib` and `build/librdma_shim.dylib`.
When building a client, use the include directory, `-L…/build -libverbs`, and a correct rpath
to the shipped provider. Do not globally replace a system or Linux `libibverbs` using
`DYLD_LIBRARY_PATH` in production: package the MelonDMA provider with the application and use
an explicit `@rpath`.

Start with discovery rather than hard-coded limits:

```c
int ndev = 0;
struct ibv_device **list = ibv_get_device_list(&ndev);
/* choose a device, ibv_open_device(), then ibv_query_device()/ibv_query_port() */
```

The single current provider device is usually named `mlx5_0`, but that name is not a network
interface and must not be used as a GID index.

## 3. Supported verbs surface

| Area | Client support |
|---|---|
| QP | RC, UD unicast, limited UC |
| RC WR | SEND, SEND_WITH_IMM, SEND_WITH_INV, RDMA WRITE/WRITE_WITH_IMM/READ, 64-bit atomics, LOCAL_INV, BIND_MW, UMR |
| UC | SEND and WRITE, including immediate; no READ, atomics, or retry path |
| Memory | normal MR, direct/indirect MR, UMR, MW type 1/2, SRQ, registration of a shared `MTLBuffer` |
| Completion | CQ polling, completion channels, arm/notify, standard async device/port/CQ/QP/SRQ events |
| QPEx | `wr_start/complete/abort`, send/read/write, immediate, `wr_send_inv`, local invalidate, atomics, SGE/inline |

DC/XRC/RAW, multicast, ODP, DMA-BUF, full `mlx5dv`/DevX, and arbitrary IOVA are not supported.
`ibv_reg_mr_iova2` accepts only IOVA equal to the virtual address. Do not design a client that
silently depends on those Linux or vendor-only features.

`SEND_WITH_INV` is live-validated on the Mac↔Linux path. Use it only as part of an explicit
Type-2 MW/rkey protocol: the peer must handle `WC_WITH_INV`, and an rkey is neither a secret
nor a replacement for authentication.

## 4. Required object lifecycle

```text
open context → configure/query RoCE → allocate PD → register MR
→ create CQ (+ comp-channel for event-driven mode) → create QP/SRQ
→ RESET → INIT → RTR → RTS → post receives before the first SEND
→ post work → drain completions → QP ERR/RESET
→ destroy QP/SRQ/CQ/channel → deregister MR → deallocate PD → close context
```

Rules that must not be broken:

- Do not deregister an MR or free its backing memory while any SQ/RQ operation may still refer
  to it. First wait for the corresponding CQEs, or move the QP to ERR and process flush CQEs.
- Do not reuse a `wr_id` as the address of a freed object before receiving its CQE.
- For SEND/UD, keep receive WRs posted in advance. An empty RQ produces RNR NAKs and
  unpredictable latency.
- After any CQE whose status is not `IBV_WC_SUCCESS`, treat the QP as failed: drain
  `WR_FLUSH_ERR`, then recover through RESET→INIT→RTR→RTS and a fresh endpoint exchange.
  Never ignore `vendor_err`.
- Do not share one QP between threads without your own serialization. For a CQ consumed by
  exactly one thread, `MELONDMA_SINGLE_THREADED=1` may be enabled only after measurement.

Queue depths are rounded up to a power of two, with a minimum of 64 and maximum of 2048.
After `ibv_create_qp()`, always read back `init_attr.cap.*`; obtain actual device limits through
`ibv_query_device()` and `ibv_mlx5_query_posting_caps()`.

## 5. Endpoint exchange and security

An RC QP does not connect itself. Through a separate control plane (TCP, Unix socket, MPI, or
TLS RPC), both sides exchange at least:

```c
struct melon_endpoint {
    uint32_t qpn;
    uint32_t psn;
    uint8_t  gid[16];
    uint32_t mtu;
    uint64_t addr;   /* only when the peer receives one-sided access */
    uint32_t rkey;
    uint64_t length;
};
```

Both sides write their endpoint before reading the peer endpoint; this avoids a full-duplex TCP
deadlock. For one-sided access, publish only the exact range and rkey needed by the operation.
Do not expose a large base MR merely for convenience.

The control plane is the trust boundary. On a multi-tenant or untrusted network, authenticate
the endpoint tuple — for example with HMAC-SHA256 over `{qpn, psn, gid, addr, rkey, length}`
plus a nonce, expiry, and replay protection. Otherwise an attacker can substitute the
destination or rkey.

## 6. Protocol selection for inference and other clients

| Task | Recommended path |
|---|---|
| Large tensor/KV/weights transfer | RDMA WRITE; use WRITE_WITH_IMM for receiver readiness |
| Pull KV / read-only remote state | RDMA READ with a constrained rkey/range |
| Control message or short header | SEND; inline for a small payload |
| Data plus commit sequence | unsignaled WRITE data → signaled zero/small WRITE_WITH_IMM with sequence |
| Many QPs sharing receive buffers | SRQ with a watermark and batch refill |
| UD reply after receive | use `ibv_init_ah_from_wc()` / `ibv_create_ah_from_wc()` to build a RoCEv2 AH from WC/GRH |

`WRITE_WITH_IMM` produces a receiver CQE, but the receiver must have a receive WR available for
the immediate notification. An ordinary WRITE removes peer CPU from the hot path; data readiness
is still defined by your protocol, not by NIC magic.

## 7. Performance: safe baseline

The direct SQ/CQ/UAR path is capability-driven and enabled **by default** when the DEXT supplies
isolated mappings. Do not set `MELONDMA_DIRECT_UAR=1` or `MELONDMA_DIRECT_CQ=1` hoping to make it
faster; that is already the default. Set either to `0` only to diagnose a fallback path.

For a sound baseline:

1. Register long-lived pool/ring buffers once. `ibv_reg_mr()` accepts unaligned addresses; the
   provider pins partial pages and applies the required MTT/KLM composition for large or sparse
   spans. Do not register a buffer for every RPC.
2. Batch several WRs and ring one doorbell. With QPEx use
   `ibv_wr_start()` → setters → `ibv_wr_complete()`; check errors from `complete()`.
3. Do not signal every operation. N−1 unsignaled WRs followed by one signaled WR are valid only
   when the client keeps sufficient bookkeeping until that CQE arrives.
4. Use `IBV_SEND_INLINE` for small control payloads. Obtain the limit from
   `ibv_query_device().max_inline_data` rather than a constant; on the current CX-4 Lx it is
   typically up to 512 B.
5. Poll a CQ into an array rather than one WC at a time. CQ depth must cover the maximum number
   of unconsumed CQEs across its QPs plus headroom. CQ overflow fails the QP.
6. Do not increase QP count or MTU blindly. Measure throughput and RTT on your own PCIe and
   network path first; a tunnel or IOMMU is often the bottleneck rather than a QP.

### Completion policy

For latency-sensitive decode, set on the Mac:

```sh
export MELONDMA_COMPLETION_POLICY=latency
```

For lower idle CPU, use `power`; without an explicit mode, the provider uses its adaptive
event-driven path. The preferred client pattern is: drain `ibv_poll_cq()` →
`ibv_req_notify_cq()` → drain again (to close the arm/poll race) → `poll()` on the
`ibv_comp_channel->fd` → `ibv_get_cq_event()`/`ibv_ack_cq_events()` → drain and re-arm.
Watch the control-plane fd in the same `poll()` so a dead peer cannot leave a worker blocked.

## 8. Apple Silicon and Metal memory

An `MTLBuffer` in `MTLResourceStorageModeShared` has host-visible `.contents` and can be
registered as an MR, including through `melon_reg_metal_mr()`. This enables a remote WRITE
directly into shared UMA memory without an intermediate CPU copy.

- A `.private` Metal buffer cannot be registered because it has no host virtual address.
- Coherence does not provide ordering: a GPU producer must finish before the RDMA post, and a
  GPU consumer may start only after the CQE or your completion protocol. Use Metal command
  buffers/fences at the GPU↔CPU boundary.
- A GPU cannot issue a NIC doorbell itself; a CPU-side thread publishes WQEs and the doorbell.
- CUDA/GPUDirect and `ibv_reg_dmabuf_mr()` are not supported on macOS.

## 9. Provider settings

| Variable | Use case |
|---|---|
| `MELONDMA_LOCAL_IP`, `MELONDMA_LOCAL_MAC`, `MELONDMA_REMOTE_MAC` | simple static RoCEv2 profile; required unless the client calls `ibv_mlx5_configure_roce()` |
| `MELONDMA_COMPLETION_POLICY=latency\|power` | choose latency or idle-CPU priority |
| `MELONDMA_HW_WAIT_MS` | diagnostic blocking-backstop tuning, 1…60000 ms; do not change the default without measurement |
| `MELONDMA_CQ_POLL_US`, `MELONDMA_CQ_BACKOFF_MAX_US` | tune the fallback event worker after profiling |
| `MELONDMA_CQ_MODERATION=<period_us>:<max_count>` | bulk workload where extra latency is acceptable to reduce event rate |
| `MELONDMA_DIRECT_UAR=0`, `MELONDMA_DIRECT_CQ=0`, `MELONDMA_FAST_PATH=0` | fallback A/B and diagnostics only, not production acceleration knobs |
| `MELONDMA_POST_BATCH=16` | diagnostic smaller post chunk; the default 64 is usually preferable |
| `MELONDMA_BLUE_FLAME=0` | disable BlueFlame for A/B testing; default is capability-driven |

`GGML_RPC_*`, `GGML_RDMA_*`, `NCCL_*`, and similar variables belong to a particular runner.
They are not the MelonDMA ABI. Document them next to your own integration and do not require
them from a third-party verbs client.

On a Linux peer, IOMMU passthrough and huge pages can affect results more than WR tuning. They
are host policy: agree them with the host owner and measure before and after rather than enabling
them blindly from client code.

## 10. Diagnostics and escalation package

Before escalating to the driver team, collect:

```sh
# Mac, while MlxPCIDriver owns the card
./build/mlx_phase2_gate --preflight
./build/mlx_wr_ex_gate
systemextensionsctl list
ioreg -r -n ethernet@0 -l -w 0 | rg 'MlxPCIDriver|IODEXTMatchCount'

command log show --last 5m \
  --predicate 'eventMessage CONTAINS "MlxPCIDriver" OR eventMessage CONTAINS "MlxCmd" OR eventMessage CONTAINS "MlxCQ" OR eventMessage CONTAINS "MlxQP"' \
  --info --style compact
```

For the application, provide negotiated endpoint metadata without rkeys or addresses, QP state
transitions, the first bad WC (`status`, `opcode`, `vendor_err`, `wr_id`), the size and number
of outstanding WRs, direct/fallback path usage, and the DEXT version. Do not publish real rkeys,
MR addresses, MAC addresses, or HMAC keys in an external issue.

| Symptom | Usual meaning | Client action |
|---|---|---|
| `RETRY_EXC_ERR` / syndrome `0x15` | peer, QP, or path unavailable | close the control session, recover the QP, and perform endpoint exchange again |
| `RNR_RETRY_EXC_ERR` / `0x16` | peer has no receive WR | grow/refill RQ or SRQ, then reconnect |
| remote access error / `0x13` | stale/wrong rkey, address, or access flags | do not retry the WR; issue a new capability and reconnect |
| `WR_FLUSH_ERR` after another CQE error | expected flush from a failed QP | drain, destroy/reset the QP; do not treat it as the root cause |
| `ibv_open_device` or preflight fails | Apple owns the card, DEXT is unhealthy, or entitlement is absent | give the operator the ownership/preflight output; do not run a takeover from the client |

## 11. Deployment limitations

The current driver is functionally validated on ConnectX-4 Lx and a live Mac↔Linux RoCE path.
Separate platform/release gates remain for production deployment: a signed, notarized SIP-on
installer and a clean Apple⇄MelonDMA ownership lifecycle. They do not change the client API,
but the deployment owner must close them before shipping an application to end users.

ECN/PFC/DCQCN code and readback exist, but switched-fabric validation requires a managed switch
with ECN/PFC; a direct Mac↔Linux cable cannot substitute for it. Treat those offload settings as
network policy, not a client-side default.
