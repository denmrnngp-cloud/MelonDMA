# MelonDMA — Inference Client Guide

> Everything a client (llama.cpp / MLX / any runner) must do to make the RDMA path beat TCP
> without burning CPU. Consolidated from the driver's performance, KV-transfer and tuning notes
> (2026-09-02 → 2026-09-07).

The driver gives an application a **generic RoCEv2 verbs API** through `libibverbs_compat` +
`librdma_shim` → DEXT → ConnectX. It knows nothing about tensors/KV/RPC — all client code is
on your side.

## Contents

1. [What the API gives you (verbs subset)](#1-what-the-api-gives-you-verbs-subset)
2. [Connecting (endpoint exchange)](#2-connecting-endpoint-exchange)
3. [Two-sided vs one-sided](#3-two-sided-vs-one-sided)
4. [Minimal overhead (apply in the client)](#4-minimal-overhead-apply-in-the-client)
5. [Completion policy](#5-completion-policy)
6. [Direct (zero-syscall) path](#6-direct-zero-syscall-path)
7. [KV-cache and bulk transfer](#7-kv-cache-and-bulk-transfer)
8. [Host DMA settings dominate the transport](#8-host-dma-settings-dominate-the-transport)
9. [Zero-copy into Metal memory (Apple Silicon)](#9-zero-copy-into-metal-memory-apple-silicon)
10. [Split (tensor-parallel) decode](#10-split-tensor-parallel-decode)
11. [Verified launch profile](#11-verified-launch-profile)
12. [Error handling](#12-error-handling)
13. [Known driver limits](#13-known-driver-limits)
14. [Environment variable reference](#14-environment-variable-reference)
15. [Diagnostics](#15-diagnostics)

## 1. What the API gives you (verbs subset)

Header: `src/dext/usermode/libibverbs_compat/include/infiniband/verbs.h`.
Objects: `ibv_context` → `ibv_pd` → `ibv_mr` + `ibv_cq` + `ibv_qp` (+ `ibv_ah`, `ibv_mw`, `ibv_srq`).
Opcodes: `SEND`, `RDMA_WRITE`, `RDMA_READ`, `SEND_WITH_IMM`, `RDMA_WRITE_WITH_IMM`,
`LOCAL_INV`, atomics (CS/FA). Flags: `SIGNALED`, `FENCE`, `SOLICITED`, `INLINE`.
WC: status/opcode/byte_len/qp_num/wr_id/vendor_err/imm_data/atomic_result.

Minimal lifecycle (order is mandatory):

```
open → query device/port/gid → alloc_pd → reg_mr
→ create_cq → create_qp → RESET → INIT → RTR → RTS
→ post_recv (pre-post!) → post_send/read/write → poll_cq
→ QP ERR/RESET → destroy_qp → destroy_cq → dereg_mr → dealloc_pd → close
```

Teardown in strict reverse order; never deregister an MR while unfinished WQEs/CQEs may still
reference it.

## 2. Connecting (endpoint exchange)

An RC QP cannot reach RTS until both sides exchange (over a separate control channel —
TCP/UDP/file/MPI-tag — **not** through the DEXT and **not** over RoCE). Deadlock-safe:
**each side writes first, then reads** (`write` before `read` — TCP is full-duplex).

| Field | From | To |
|---|---|---|
| QPN | `ibv_create_qp` → `qp_num` | `modify_qp(INIT→RTR).dest_qpn` |
| GID | `ibv_query_gid` | `modify_qp(RTR).dgid` |
| PSN | you choose (usually 0) | `modify_qp(RTR).rq_psn` (peer's) |
| remote addr + rkey | `ibv_reg_mr` → `mr->addr`, `mr->rkey` | SGE/WR for one-sided |

```c
struct conn_info { uint32_t qpn, psn; uint8_t gid[16]; uint64_t buf_addr; uint32_t rkey, buf_len; };
```

### 2.1 Authenticating the exchange (multi-tenant)
If the control channel is untrusted, a MITM can swap `addr/rkey`. The scheme (formerly
`melon_auth`):
- A 32-byte pre-shared key, HMAC-SHA256 (RFC 4231) over the tuple `{qpn, psn, gid, addr, rkey, len}`.
- Envelope `nonce(8)+timestamp(8)+payload+tag`. Verify strictly in order: HMAC → expiry (~300 s
  window) → replay cache.
- No key → plaintext (dev); a key is set but malformed → fail-closed, **not** silent downgrade.

### 2.2 QP parameters
`rnr_retry = 7` (retry infinitely on RNR NAK, if the receiver always refills Recv WRs),
`timeout ≈ 14` (~67 ms local ACK timeout), `retry_cnt = 7`.

## 3. Two-sided vs one-sided

| Criterion | RDMA WRITE | Send/Recv |
|---|---|---|
| Peer CPU in the hot path | no | yes |
| Receiver knows where to write | yes (fixed MR) | no (FIFO from RQ) |
| Receiver notification | only WRITE_WITH_IMM | always (recv CQE) |
| Size | any | better < 4 KB |
| Pre-posted buffers | not needed | required |

**For inference**: weights/tensors/activations/KV → RDMA WRITE (+IMM); control, RPC headers,
seq → Send/Recv or inline SEND.

**"payload + seq" (NCCL pattern)**: one unsignaled WRITE of the data, then a zero-length
`RDMA_WRITE_WITH_IMM` (signaled) with `imm_data = seq`. The peer sees a CQE → the data is
already in place. One round trip, no RQ on the data path.

**The receiver must pre-post Recv WRs before the SEND arrives.** An empty RQ → RNR NAK →
latency spike. Keep a watermark and refill in batches.

## 4. Minimal overhead (apply in the client)

1. **Block, don't spin** (removes ~45 % of a core): completion channel + `poll()` instead of a
   busy-poll on `ibv_poll_cq`. See §5.1.
2. **Direct path** (`MELONDMA_DIRECT_UAR=1` / `MELONDMA_DIRECT_CQ=1`) — zero-syscall WQE build/
   doorbell and CQE decode. See §6.
3. **Unsignaled sends**: N−1 WRs without `SIGNALED`, every Nth signaled, advance the completion
   pointer by N. Cuts the CQE rate N×.
4. **Inline** (`IBV_SEND_INLINE`, ≤512 B on this driver): payload copied into the WQE, no DMA
   fetch. Always faster ≤128 B; DMA wins ≥256 B.
5. **Batch posting + one doorbell**: `PostSendBatch` / `SyncFastPath` / `ibv_qp_to_qp_ex`
   (WR chain between `wr_start`/`wr_complete`) — N WRs in one call.
6. **Poll in batches** (`ibv_poll_cq(cq, N, wc)`), always check `wc.status != SUCCESS`.
7. **CQ depth ≥ Σ(SQ+RQ)** of all QPs on the CQ + 10–20 %; otherwise overflow → QP ERR.
8. **Deep SQ / several QPs** — more in-flight WRs hide RTT (but see §7: on this link more QPs
   add no bandwidth).
9. **Register once**: registration costs 5–20 ms (large buffers) / 1.18–1.75 ms. Keep a pool of
   pre-registered buffers + your own MR cache keyed by page-aligned address (NCCL
   `net_ib/reg.cc`). Do not register per call and do not deregister right after use.

## 5. Completion policy

Without `MELONDMA_COMPLETION_POLICY=latency` the wait path falls back to the device's blocking
wait after ~40 empty ticks (median wakeup **205 µs** vs **79 µs** for the mapped-ring poller).
Bulk barely notices; **tensor-parallel split decode** (small activations per layer per token)
loses three quarters — measured 16.3 vs 55.2 tok/s (TCP 46.9). Set it in every mode.

| worker path | idle CPU | wakeup median | wakeup max |
|---|---|---|---|
| blocking, 50 ms backstop (default) | 0.20 % of a core | 205 µs | 51.6 ms |
| blocking, 10 ms backstop | 0.90 % | 546 µs | 11.3 ms |
| mapped-ring polling | 2.95 % | 79 µs | 112 µs |

Knobs:
- `MELONDMA_HW_WAIT_MS` (default 50) — the backstop. Lower shortens the rare tail at a
  proportional CPU cost. Do not drop it below the link's own completion latency (at 2 ms the
  backstop started serving completions itself and the median rose an order of magnitude).
- `MELONDMA_HW_CQ_EVENT=0` — force the mapped-ring poller (for A/B tests).
- `MELONDMA_CQ_POLL_US` (default 50) — the comp-channel poller period.
- `MELONDMA_CQ_MODERATION="<period_us>:<max_count>"` — hardware moderation (the NIC holds an
  event until one of the bounds). Off by default; on this bench it was **closed negatively**
  (`events/iter=1.00` for batches 1..32 — one event already covers the whole batch). The lever
  is **grouping signaled operations**, not moderation: p50 per batch 114 µs(1) → 47.6(8) →
  70.5(32), per operation 5.95/2.20 µs.

**Event-driven poll recipe (block, don't spin):**
`ibv_create_comp_channel()` (one per CQ; separate send/recv CQs → two channels) → on an empty
poll: `ibv_req_notify_cq(cq,0)` → re-poll (close the arm/poll race) → `poll(cc->fd)` →
`ibv_get_cq_event`/`ibv_ack_cq_events` → drain → re-arm. Watch the TCP control fd in the same
`poll()` so a dead peer aborts the block instead of hanging.

## 6. Direct (zero-syscall) path

`MELONDMA_DIRECT_UAR=1` + `MELONDMA_DIRECT_CQ=1`. Verified by `PHASE2_FULL_GATE`: 1,000,000
messages, 0 errors, 10 teardown cycles, no kernel fallback.

Gotchas:
- Queue sizes are rounded to a power of two (min 64) — do not code against the exact requested depth.
- Inline SEND is tracked by the direct SQ metadata and can join a trusted mixed WR chain.
  `GGML_RPC_RDMA_DISABLE_INLINE=1` is an ablation/debug switch only.
- The direct decoder handles success/error SEND/RECV/READ/WRITE/atomic CQEs. UMR/LOCAL_INV,
  UD pairs, SRQ receive, unknown QP and lost metadata fall back to the kernel path.

## 7. KV-cache and bulk transfer

Measured direction ceilings (all tunnel-bound, not protocol): Mac receive **13.4 Gbit/s**
(write to memory), Mac transmit **21.0 Gbit/s**, loopback through the card ~2.03 GB/s per
direction. After `iommu.passthrough=1` on Spark: Spark→Mac **23.0 Gbit/s**.

- **One batch, not 80 RPCs**: `GGML_RPC_RDMA_KV_BATCH=1` — one request for 80 KV spans, one
  final `WRITE_WITH_IMM`. wait_ms 81.9 → 56.9 (−30.5 %), full get −34.6 %.
- **Final destination**: `GGML_RPC_RDMA_FINAL_DEST=host` (Metal final — see §9),
  `GGML_RPC_RDMA_DEST_ARENA_MAX=96`.
- **The ordering barrier is not free**: `GGML_RPC_RDMA_KV_FENCE=0` buys real overlap, but only
  if the peer can absorb a whole graph's uploads; otherwise the sender stalls on RNR and its
  completion wait becomes a spin (1.09 s of CPU per request). Dropping the barrier is bounded
  by the peer's absorb window (at least 32 MiB).
- **Receive depth is a throughput knob**: `GGML_RPC_RDMA_RX_DEPTH` (8..256 slots). 24 slots =
  6 MiB (a 2k-token prefill overruns → stall), **160 slots = 40 MiB** removes the stall. Cost
  is one MR per slot (keep the default with a small mkey quota).
- **WRITE geometry**: `GGML_RPC_RDMA_WRITE_CHUNK` (256/512/1024 KiB), `_WINDOW` (2/4),
  `_DEPTH` (4/8), max 256 regions per batch. On this hardware the geometry **does not matter** —
  keep the defaults.
- **More QPs add no bandwidth** (two QPs share the same 1.68 GB/s) — the ceiling is the path,
  not the queue.
- **Message size and MTU do not matter** for inbound bandwidth (12.6–13.4 Gbit/s over a 256×
  size range; MTU 1024 is even slightly faster than 4096). The RoCE path MTU cap is 4096
  (spec); Ethernet MTU 9000 only exists to fit 4096 in a frame. Do not spend time on MTU.

## 8. Host DMA settings dominate the transport

- **On the Linux peer (Spark): `iommu.passthrough=1`** in the boot line — the single biggest
  lever. One-sided WRITE in loopback 12.6 → **100.8 Gbit/s**, across the wire 13.2 → 23.0.
  SMMU translation throttles the NIC's memory reads on transmit (TLB pressure + latency), not
  the line width.
- Huge pages for the source (2 MiB `MADV_HUGEPAGE`) give +7 % only on a large buffer read once;
  on reused rings they give zero (translations are already cached in the SMMU).
- `pcie_aspm=off` is worth checking separately (line power management adds latency spikes).
- Pin the bond MAC (`nmcli ... 802-3-ethernet.cloned-mac-address <mac>`) and a static neighbour
  entry (`ip neigh replace ... nud permanent`) — after a reboot the link drops without them
  (a service is needed).

## 9. Zero-copy into Metal memory (Apple Silicon)

CPU and GPU share unified memory: a `MTLBuffer` in `MTLResourceStorageModeShared` is an
ordinary virtual address (`.contents`) that `ibv_reg_mr` pins like any malloc'd buffer
(`IODMACommand` → PAS → NIC DMA). The DEXT advertises `MLX_UC_FEATURE_COHERENT_UMA_MR` with no
opt-in; `run_metal_dma_gate.sh` verified GPU→Spark, Spark→GPU and a 4 MiB indirect MR
(MTU 4096, 0 mismatches).

- **One-sided RDMA WRITE straight into a registered Metal buffer** (the final KV layout),
  `RDMA_WRITE_WITH_IMM` as the signal. Removes the `set(local)` memcpy from the receive path.
- **Pull-based KV (KVDirect, arXiv:2501.14743)**: the decode node PULLs the KV tensors it needs
  (one-sided READ) instead of pushing everything — composes with (1).
- **`StorageModeShared` is mandatory** (never `.private` — no host address). Quantized KV
  (q8_0) stays host-addressable.
- **Coherence ≠ ordering**: the GPU producer must finish before the RDMA post, the GPU consumer
  starts after the CQE/WC (Metal fence + `MTLBlitCommandEncoder` sync).
- **A GPU-issued doorbell is impossible** (BAR/MMIO is not DRAM; wrapping it via `bytesNoCopy`
  caused a kernel panic). A CPU proxy thread that posts WRs is mandatory. This is not NVIDIA
  GPUDirect, but it is what JACCL/OdinLink do on unified memory.

**Large MR**: `ibv_reg_mr` transparently chunks >1.875 MiB into direct children and composes
them under an indirect (KLM) MR — up to **450 MiB** (240 × 1.875 MiB). `addr`/`length` must be
4 KiB-aligned.

**Honest assessment**: "no copy into Mac memory" is achieved; "no copy into Metal tensors" is
not (the KV batch hardcodes `RPC_RDMA_DEST_HOST`). The remaining copy is ~9 ms out of a 12.4 s
TTFT — smaller than the fixed 65.9 MB per request and the peer's host DMA settings. Remove the
redundant bytes first, then the Metal final destination.

## 10. Split (tensor-parallel) decode

- **`MELONDMA_COMPLETION_POLICY=latency` is mandatory** (otherwise −75 % decode, see §5).
- Aggregate activations per decode step (4/8/16/32 messages) into one WR chain with one
  signaled WR; remove the doorbell/completion per message. Multi-SGE instead of pack-copy.
- Inline only for headers ≤256 B; do not inline large activations just because the limit is
  512 B.
- `GGML_RPC_RDMA_SIGNAL_INTERVAL=1` + mailbox off is only for bring-up against SQ retries; the
  mailbox-enabled profile is supported in normal operation.

## 11. Verified launch profile

```sh
# Mac (llama-server, signed with userclient-access)
export MELONDMA_COMPLETION_POLICY=latency
export MELONDMA_LOCAL_IP=192.168.200.1
export MELONDMA_LOCAL_MAC=<mac Mac>            # without these the probe fails silently
export MELONDMA_REMOTE_MAC=<mac Spark>
export GGML_RDMA_DEV=mlx5_0                    # device name, NOT a GID index
# Do NOT set a GID index — the driver gives each client its own slot

# Spark (rpc-server)
export GGML_RDMA_DEV=rocep1s0f1 GGML_RDMA_GID_ADDR=192.168.200.2
export GGML_RPC_RDMA_RX_DEPTH=160              # 40 MiB pre-posted receives

# disagg, additionally:
export GGML_RPC_RDMA_KV_BATCH=1 GGML_RPC_RDMA_FINAL_DEST=host \
       GGML_RPC_RDMA_DEST_ARENA_MAX=96 GGML_RPC_RDMA_KV_FENCE=0

# Spark, kernel: iommu.passthrough=1
```

**The GID rule**: the provider hands every `ibv_open_device` its own GID slot and answers
`ibv_query_gid` only for the slot that client owns. A pinned `GGML_RDMA_GID` works for the
first process and breaks the rest — this was the "one model over RDMA" limit. Indices move
across reboots (link-local entries). On the Mac, do not set an index; on Linux use
`GGML_RDMA_GID_ADDR=<local RDMA IPv4>`.

## 12. Error handling

After **any** non-SUCCESS CQE the QP → ERR and the queue flushes (`WR_FLUSH_ERR`). Do not
swallow errors. `vendor_err` carries the raw syndrome (`MlxSyndromeToWcStatus`): 0x15 = retry
exhausted, 0x16 = RNR exhausted, 0x13 = remote access (bad rkey/address or a deregistered MR).
Recovery — `RESET → INIT → RTR → RTS`.

## 13. Known driver limits

- MR ≤ ~1.875 MiB direct (480 × 4 KiB PAS); larger — chunks/indirect KLM (≤240 children ≈ 450 MiB).
- `max_inline_data = 512` B; SGE ≤ 16; SQ/RQ depth ≤ 4096; CQ depth ≤ 2048.
- RC + UD; no DC/XRC/multicast; SRQ exists (RMP, `max_wr`/`max_sge` by capability).
- Device: explicit GID/MAC (`rdma_set_roce_address`), no `enX`/ARP. Without `MELONDMA_LOCAL_IP`
  + both MACs the probe opens the device, creates a QP and fails without explanation.
- NVIDIA GPUDirect/CUDA peer-memory is unavailable on macOS; the Apple equivalent is registering
  a `.shared|.untracked` `MTLBuffer` (§9). `.private` and a GPU doorbell are unsupported.

## 14. Environment variable reference

| Variable | Side | Purpose / value |
|---|---|---|
| `MELONDMA_COMPLETION_POLICY=latency` | Mac | mapped-ring poller with short waits (else blocking 205 µs) |
| `MELONDMA_HW_CQ_EVENT=0` | Mac | force the mapped-ring poller (A/B) |
| `MELONDMA_HW_WAIT_MS` | Mac | blocking-wait backstop (default 50) |
| `MELONDMA_CQ_POLL_US` | Mac | comp-channel poller period (default 50) |
| `MELONDMA_CQ_MODERATION="<us>:<cnt>"` | Mac | hardware CQ moderation (off by default; not needed here) |
| `MELONDMA_DIRECT_UAR=1` / `MELONDMA_DIRECT_CQ=1` | Mac | zero-syscall post/poll |
| `MELONDMA_FAST_PATH=0` | Mac | force kernel-mediated post (fallback) |
| `MELONDMA_LOCAL_IP` / `MELONDMA_LOCAL_MAC` / `MELONDMA_REMOTE_MAC` | Mac | RoCE addressing (mandatory) |
| `GGML_RDMA_DEV` | both | device name (`mlx5_0` / `rocep1s0f1`) |
| `GGML_RDMA_GID_ADDR` | Linux peer | select GID by address (survives reboots) |
| `GGML_RDMA_GID` | — | do NOT set on Mac (per-client slots) |
| `GGML_RPC_RDMA_KV_BATCH=1` | Mac | one batch for the KV handoff |
| `GGML_RPC_RDMA_FINAL_DEST=host\|metal` | Mac | KV final destination |
| `GGML_RPC_RDMA_DEST_ARENA_MAX=96` | Mac | host arena cap (MiB) |
| `GGML_RPC_RDMA_KV_FENCE=0` | Mac | drop the ordering barrier (bounded by peer window) |
| `GGML_RPC_RDMA_RX_DEPTH=160` | Linux peer | 40 MiB pre-posted receives |
| `GGML_RPC_RDMA_WRITE_CHUNK/_WINDOW/_DEPTH` | Mac | WRITE geometry (defaults fine) |
| `GGML_RPC_RDMA_KV_PREFETCH=1` | Mac | re-enable the duplicate prefetch (A/B) |
| `GGML_RPC_REQUIRE_RDMA=1` | both | do not silently fall back to TCP |
| `GGML_RPC_RDMA_DISABLE_MAILBOX=1` / `GGML_RPC_RDMA_DISABLE_INLINE=1` | Mac | ablation switches |
| `GGML_RPC_RDMA_STATS=1` | Mac | poll/wakeup/fallback telemetry |
| `iommu.passthrough=1` | Linux peer (kernel) | passthrough SMMU — the largest transmit win |

## 15. Diagnostics

- Mac: `tools/mlx_port_counters --watch N --pcie` — PPCNT (packets/bytes/errors/discards/pause)
  + the PCIe link behind the tunnel (MPEIN) and the device's stall counters (MPCNT). The only
  receive-side view of the wire when the DEXT owns the port.
- `mlx_perf_test` — prints the interrupt counters and the current EQ timer period.
- `mlx_irq_probe` — rebind the completion EQ to a chosen interrupt index.
- `mlx_gate_report`, `mlx_datapath_bench`, `mlx_qp_scale`, `mlx_rtt_bench`, `mlx_blocking_rtt`.
- Peer (Linux): `ethtool -S <if> | grep -E 'rx_prio3_pause|tx_prio3_pause|rx_discards|tx_discards'`,
  `ib_write_bw`, `rdma stat qp show`. A direct link creates no congestion, so PFC/ECN counters
  read zero and DCQCN cannot be calibrated on this bench.
