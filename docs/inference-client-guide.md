# MelonDMA — how to write an inference client (llama.cpp / MLX / anything)

MelonDMA gives an application a **generic RoCEv2 RC verbs API** through
`libibverbs_compat` + `librdma_shim` → DriverKit DEXT → ConnectX. The driver
knows nothing about tensors, GGUF, KV-cache or collectives. All client code is on
your side; this document is the contract you need to write your own transport for
llama.cpp, MLX or any runner.

> The bespoke integrations (the MLX `melon_mlx` backend, the llama.cpp RPC
> helpers) were removed from the repo. Everything they did is covered by this
> document: the verbs subset below plus the patterns in §4–§6.

## 1. What the API gives you (verbs subset)

For the disaggregated KV path, see [the 7–15% TTFT target and implementation candidates](ttft-7-15-target-2026-09-05.md). A single destination MR does not imply one batched RPC. Reuse requires allocation ownership/generation, not just a cache hit by virtual address. Current driver pin quotas also require bounded transfer windows for large contexts.

The companion llama.cpp now has an [opt-in one-request KV batch, direct TX-ring filling and owned host arena](kv-transfer-batch-pipeline-2026-09-05.md). Its accepted profile requires `MELONDMA_COMPLETION_POLICY=latency`; this does not establish working low-latency hardware IRQ delivery. The destination is a serialized host-state buffer, not final Metal KV tensors.

Header: `src/dext/usermode/libibverbs_compat/include/infiniband/verbs.h`.
Objects: `ibv_context` → `ibv_pd` → `ibv_mr` + `ibv_cq` + `ibv_qp` (+ `ibv_ah`,
MW).
WR opcodes: `SEND`, `RDMA_WRITE`, `RDMA_READ`, `SEND_WITH_IMM`,
`RDMA_WRITE_WITH_IMM`, `LOCAL_INV`, atomics (CS/FA). Flags: `SIGNALED`, `FENCE`,
`SOLICITED`, `INLINE`. WC: status/opcode/byte_len/qp_num/wr_id/vendor_err/
imm_data/atomic_result.

Minimal lifecycle (required order):

```
open → query device/port/gid → alloc_pd → reg_mr
→ create_cq → create_qp → RESET → INIT → RTR → RTS
→ post_recv (pre-post!) → post_send/read/write → poll_cq
→ QP ERR/RESET → destroy_qp → destroy_cq → dereg_mr → dealloc_pd → close
```

Teardown is strictly in reverse dependency order. Never deregister an MR while an
unfinished WQE/CQE may still reference it.

## 2. Connecting (endpoint exchange) — the most important part for a client

An RC QP cannot reach RTS until both sides exchange (see the book §4.8):

| Field | From | To |
|---|---|---|
| QPN | `ibv_create_qp` → `qp_num` | `modify_qp(INIT→RTR).dest_qpn` |
| GID | `ibv_query_gid` | `modify_qp(RTR).dgid` |
| PSN | you choose (usually 0) | `modify_qp(RTR).rq_psn` (peer's) |
| remote addr + rkey | `ibv_reg_mr` → `mr->addr`, `mr->rkey` | SGE/WR for one-sided |

The exchange happens over a **separate control channel** (TCP/UDP/file/MPI-tag) —
not through the DEXT and not over RoCE. Deadlock-safe order: **each side writes
first, then reads**.

```c
struct conn_info {
    uint32_t qpn, psn;
    uint8_t  gid[16];
    uint64_t buf_addr;   uint32_t rkey, buf_len;   /* for RDMA WRITE/READ */
};
/* both sides: */
write(sock, &local, sizeof(local));   /* write first — TCP is full-duplex */
read_full(sock, &remote, sizeof(remote));
```

### 2.1 Authenticating the exchange (recommended for multi-tenant)

If the control channel is not trusted, a MITM can swap `addr/rkey` and redirect a
one-sided RDMA into someone else's memory. The scheme (previously in the removed
`melon_auth`):

- A shared 32-byte pre-shared key, HMAC-SHA256 (RFC 4231) over the whole
  conn-tuple `{qpn, psn, gid, addr, rkey, len}`.
- Envelope: `nonce(8) + timestamp(8) + payload + tag`. Verify strictly in order:
  HMAC → expiry (a window, e.g. 300 s) → replay-cache (bounded).
- No key → plaintext exchange (dev); a key is set but malformed → fail-closed,
  **not** silent downgrade.

### 2.2 QP state machine

```
RESET ──INIT──► INIT ──RTR──► RTR ──RTS──► RTS
                  (needs peer QPN/GID/PSN)   (timeout, retry, rnr_retry)
```
`rnr_retry = 7` (retry infinitely on RNR NAK) is correct if the receiver always
refills Recv WRs. `timeout ≈ 14` (≈67 ms local ACK timeout), `retry_cnt = 7`.

## 3. Two-sided operations (Send/Recv)

The receiver **must pre-post Recv WRs before the Send arrives**. An empty RQ →
RNR NAK → latency spike. Keep a watermark and refill in batches. SEND suits
request/response and small messages (< 4 KB); for large tensors use one-sided
(below).

## 4. One-sided (RDMA WRITE/READ) — the main inference path

```c
/* WRITE: push into a remote MR without involving the peer CPU */
ibv_post_send(qp, &(struct ibv_send_wr){
    .opcode = IBV_WR_RDMA_WRITE,
    .wr.rdma = { .remote_addr = peer_addr, .rkey = peer_rkey },
    .sg_list = &sge, .num_sge = 1, .send_flags = IBV_SEND_SIGNALED, ...});
```
The responder does nothing: data is DMA'd directly into the registered MR. For the
completion signal use `RDMA_WRITE_WITH_IMM` (a recv CQE on the peer with
`imm_data`), or a sentinel byte at the end of the buffer (RC preserves write
order — no fence needed for two writes in a row; `FENCE` is only for ordering
after READ/Atomic).

**"payload + seq" pattern (like NCCL):** one unsignaled WRITE of the data, then a
zero-length `RDMA_WRITE_WITH_IMM` (signaled) with `imm_data = seq`. The peer sees
a CQE → the data is already in place. One round trip, no RQ on the data path.

## 5. Minimal overhead (apply in the client)

- **Unsignaled sends:** N−1 WRs without `SIGNALED`, every Nth with it; advance the
  completion pointer by N. Cuts the CQE rate N×.
- **Inline** (`IBV_SEND_INLINE`, ≤512 B on this driver): payload copied into the
  WQE, removes the DMA fetch. Always faster ≤128 B; DMA wins ≥256 B.
- **Batch posting + one doorbell:** `PostSendBatch` / `SyncFastPath` — N WRs in
  one call, one doorbell.
- **Poll in batches** (`ibv_poll_cq(cq, N, wc)`), always check `wc.status != SUCCESS`.
- **CQ depth ≥ Σ(SQ+RQ)** of all QPs on the CQ + 10–20% headroom. Otherwise CQ
  overflow → QP ERR.
- **Deep SQ / several QPs:** more in-flight WRs hide RTT, raising IOPS.
- **Register memory once**, keep a pool of pre-registered buffers (registration
  is 5–20 ms). Keep your own MR cache keyed by page-aligned address — re-registering
  the same range returns the cache (NCCL `net_ib/reg.cc` pattern).
- **Do not register per call** and do not deregister right after — keep the region
  while it is reused.

## 6. Choosing between Send/Recv and RDMA WRITE

| Criterion | RDMA WRITE | Send/Recv |
|---|---|---|
| Peer CPU in the hot path | no | yes |
| Receiver knows where to write | yes (fixed MR) | no (FIFO from RQ) |
| Receiver notification | only WRITE_WITH_IMM | always (recv CQE) |
| Size | any | better < 4 KB |
| Pre-posted buffers | not needed | required |

For inference: weights/tensors/activations → RDMA WRITE (+IMM); control messages,
RPC headers, seq → Send/Recv or inline SEND.

## 7. Error handling

After **any** non-SUCCESS CQE the QP moves to ERR and all queued WRs flush
(`WR_FLUSH_ERR`). Do not swallow errors. `vendor_err` carries the raw syndrome
(see `MlxSyndromeToWcStatus` in `MlxQP.cpp`): 0x15 = retry exhausted, 0x16 = RNR
exhausted, 0x13 = remote access (bad rkey/address or MR deregistered). Recovery —
`RESET → INIT → RTR → RTS` again.

## 8. Llama.cpp implementation status

Implemented in the llama.cpp transport:

- One-sided WRITE windows rotate across two persistent staging generations, overlapping preparation of the next window with completion of the previous one.
- The connection MR cache reuses a registered region when a later request is a covered subrange, not only when address and length match exactly.
- CQ polling drains up to 16 completions per verbs call and retains the remainder locally, reducing per-completion polling overhead while preserving the existing one-WC API.
- Server-side one-sided KV extraction reuses a thread-local staging buffer instead of allocating a vector for every window.
- `GGML_RPC_RDMA_STATS=1` reports CQ poll calls, returned completions, empty polls, event wakeups and direct-CQ fallback count.
- TCP liveness is watched together with the completion channel, so event-driven waits do not hang indefinitely after a dead peer.
- The implementation builds successfully for the current ARM64/Metal `build-async` configuration with `ggml-rpc` target.

RPC contract and framing implemented:

- `GET_TENSOR_RDMA` now carries `write_addr`, `write_size`, `write_rkey`, `destination_kind` (`host|metal|cuda`) and `destination_flags` (`FINAL|STABLE|FALLBACK_OK`). The server accepts one-sided delivery only for a valid declared range; otherwise the existing streamed GET path is used.
- `RPC_CMD_BATCH_SEND` is a separate fire-and-forget frame with `{version,count,step}` and bounded `{command,size,payload}` entries. It supports up to 64 items and 1 MiB total; request/response commands are excluded deliberately.
- The client command queue can construct and enqueue a batch of `SET_TENSOR` entries, and the server validates and dispatches it in order.
- Public `ggml_backend_rpc_batch_send()` is exposed in `ggml-rpc.h` and through the backend registry, so a decode producer can submit explicit same-step payload boundaries without depending on internal queue classes.
- The command queue automatically batches adjacent small `SET_TENSOR` items on RDMA (up to 64 items/1 MiB, 100 microsecond collection deadline). Graphs, responses and large items remain ordering barriers and use the old frame.
- RPC protocol minor version is now 1; old major-compatible peers continue using the old framing.

Implemented llama-side changes:

- One-sided WRITE windows rotate across two persistent staging generations, overlapping preparation of the next window with completion of the previous one.
- The connection MR cache reuses a registered region when a later request is a covered subrange, not only when address and length match exactly.
- CQ polling drains up to 16 completions per verbs call and retains the remainder locally.
- Server-side one-sided KV extraction reuses a thread-local staging buffer.
- `GGML_RPC_RDMA_STATS=1` reports CQ poll calls, returned completions, empty polls, event wakeups and direct-CQ fallback count.
- The implementation builds successfully for the current ARM64/Metal `build-async` configuration with `ggml-rpc` target.

Still open at workload integration level:

1. The final Metal/CUDA destination must be explicitly selected with `GGML_RPC_RDMA_FINAL_DEST=metal|cuda|host`; unsupported or unstable backend allocations retain staging fallback. Removing duplicate logical KV transfer still requires the end-to-end KV owner to use this contract.
2. Split aggregation framing, public API and automatic adjacent-`SET_TENSOR` queue batching are implemented. The queue batches only explicit SET payload commands; arbitrary `send_data()` calls and request/response commands are not merged. The public API is `ggml_backend_rpc_batch_send(buffer, step, n_items, items, sizes)`.
3. Hybrid active/idle CQ policy and direct/kernel telemetry correlation still require live gates on both Spark and Mac Studio.

Compatibility contract:

- Mac Studio uses MelonDMA RDMA when negotiated capabilities are available, otherwise TCP fallback.
- Spark uses the Linux verbs-compatible path and the same RPC framing.
- GPU-direct registration is never assumed; unsupported or unstable pointers retain the staging fallback.
- `GGML_RPC_RDMA_DISABLE_MAILBOX=1` and `GGML_RPC_RDMA_WRITE_KV` remain runtime switches.

Acceptance: zero corruption/CQ/RNR/retry errors, one-time KV byte accounting, RDMA split decode at least 5% above TCP40G at contexts 512/1024 without ITL p95 regression, CPU-seconds/request no higher than TCP40G, and 10 interleaved repetitions on both machines.

### 8.1 Current verified llama.cpp RDMA operation

The current llama.cpp RPC path has been smoke-tested end to end with a real
Qwen3.6-35B-A3B request in both inference layouts:

| Mode | Context | Generation | Prefill | Decode | TTFT | Errors |
|---|---:|---:|---:|---:|---:|---:|
| Disaggregated (`--prefill-device RPC0`) | 512 | 8 | 348.29 tok/s | 72.59 tok/s | 1.518 s | 0 |
| Split (`--device MTL0,RPC0`) | 512 | 8 | 604.39 tok/s | 10.97 tok/s | 1.229 s | 0 |

The request was executed with `GGML_RPC_REQUIRE_RDMA=1`, so a TCP fallback would
have failed the connection instead of being silently accepted. The observed
sessions were:

```text
Mac:   RDMA connection active: qpn=144->692 (disaggregated)
Spark: RDMA connection active: qpn=692->144
Mac:   RDMA connection active: qpn=145->693 (split)
Spark: RDMA connection active: qpn=693->145
MTU:   4096, RoCEv2, errors=0
```

The Mac and Spark device names are intentionally different. MelonDMA exposes
`mlx5_0` to the Mac userspace provider; the Spark Linux provider uses
`rocep1s0f1`. The benchmark launcher must therefore use:

```sh
# Mac
GGML_RDMA_DEV=mlx5_0 GGML_RDMA_GID=0
# Spark
GGML_RDMA_DEV=rocep1s0f1 GGML_RDMA_GID=3
```

The control socket remains TCP (`192.168.100.1` to `192.168.100.2`) because it
carries endpoint capability exchange and RPC framing. After HELLO, tensor and
activation payloads use the negotiated RoCEv2 RC QP. This is not a TCP data-path
fallback: `GGML_RPC_REQUIRE_RDMA=1` is checked on both client and server, and
both sides log `RDMA connection active` before the first model operation.

The validated launch profile also requires the Mac llama binary to be signed
with the DEXT UserClient entitlement:

```text
com.apple.developer.driverkit.userclient-access = com.mlx5.rdma.dext
```

For initial bring-up, `GGML_RPC_RDMA_SIGNAL_INTERVAL=1` and mailbox disabled
were used to avoid an SQ retry failure during the large model-load burst. The
ordinary mailbox-enabled profile remains supported and has previously passed
full-model smoke; the signal interval should be relaxed only after a workload
gate confirms that the SQ does not exhaust. The smoke result is stored at
`/tmp/llama-rdma-disagg-split-smoke.csv`.

## 9. Known driver limits (important for a client)

- MR ≤ ~1.875 MiB in one direct mkey (480×4 KiB PAS); larger buffers are chunked
  or use an indirect (KLM) MR (`RegMRIndirect` + `PostUmrKlm`).
- `max_inline_data = 512` B; SGE ≤ 16.
- SQ/RQ depth ≤ 4096; CQ depth ≤ 2048.
- NVIDIA-style GPUDirect RDMA/CUDA peer-memory is unavailable on macOS/M2, but
  DEXT 0.359 supports the Apple Silicon equivalent for data buffers by default:
  register `contents()` of a `.shared | .untracked` `MTLBuffer` as an ordinary
  MR. This gives NIC↔UMA↔Metal zero-copy data movement. `.private` buffers and
  GPU-issued PCIe UAR doorbells are not supported.
- Device: explicit GID/MAC configuration (`rdma_set_roce_address`), no automatic
  `enX`/ARP.
- No UD/DC/XRC/SRQ/multicast; RC only.

The full list — `docs/rdma-driver-spec.md`; the firmware/RoCE protocol facts — `docs/research.md` and `docs/architecture.md`.

## 10. Overhead reduction roadmap — beat TCP

Measured baseline (`docs/benchmark-rdma-cluster-2026-09-02.md`, RDMA vs the
10GbE TCP control-path fallback): RDMA already wins the bulk path (prefill
+2–12 %, TTFT up to −10.7 % at 65K) but pays for it twice — the Mac receiver
burns **~45 % of a core** on `ibv_poll_cq` busy-poll in disagg, and **split
mode decode is 4–13 % *slower* than TCP** (small activation messages, where
per-message cost dominates). The goal is to remove that per-message overhead so
RDMA wins everywhere, not just on bulk.

Historical root cause: every `ibv_poll_cq` / `ibv_post_send` used to be a
kernel-mediated `IOConnectCallStructMethod` into the DEXT. The mapped trusted
path now implements direct WQE construction/doorbell and direct CQE decoding in
userspace, including inline tracking. The remaining work is to keep that path
active for the real workload without unexpected fallback, aggregate logical
RPC messages, and combine direct draining while active with event sleep while
idle.

Levers, highest impact first:

1. **Keep direct CQE decode on and measure it.** ✅ IMPLEMENTED. The mapped CQE
   ring is decoded in userspace and ordinary completions retire WR metadata
   without a DriverKit call. Next: expose per-request direct/kernel/fallback
   counters and require zero unexpected fallback in the llama workload.
2. **Keep direct SQ/RQ posting on.** ✅ IMPLEMENTED. `MELONDMA_DIRECT_UAR=1`
   builds WQEs and rings the doorbell in userspace, including mixed/inline WR
   chains. It is enabled by the validated llama launch profile; making it the
   provider-wide default remains gated on mixed-client compatibility.
3. **Completion channel + event-driven poll.** `ibv_create_comp_channel` +
   `ibv_req_notify_cq` + `poll()` on the channel fd replaces the spin. The
   comp-channel worker now reads the mapped CQE ring directly
   (`rdma_cq_pending_local`) with a ~50 µs wakeup (`MELONDMA_CQ_POLL_US` to
   tune) instead of a 1 ms kernel-query poll — so a client that blocks on the fd
   keeps wakeup inside the inter-completion gap of a streaming KV transfer while
   sleeping the rest of the time. Use in the client:
   `ibv_req_notify_cq` → `poll(cc->fd)` → `ibv_get_cq_event`/`ibv_ack_cq_events`
   → drain → re-arm.
4. **Client-side batching (already §5, restated because it compounds):**
   unsignaled sends (1 signaled per N), `ibv_poll_cq(cq, N, wc)` batch drain,
   inline ≤512 B, pre-posted recv ring kept full, register once + MR cache. With
   the direct fast path these become near-zero-overhead per message.

Order of work now: preserve the direct CQ/UAR gates, implement RPC aggregation
and final Metal KV placement, then tune the active-direct/idle-event threshold.
Only after workload counters show no unexpected fallback should the fast path
become provider-wide default. Gate every step against the same
`bench_rdma_cluster.py` sweep: `mac_cpu`, `prefill tok/s`, `decode tok/s`, and
`TTFT` must not regress.

## 11. Performance recommendations (field-verified)

A prescriptive checklist for a client that wants RDMA inference to beat TCP.
Numbers are from the live cluster sweep + hardware gates (2026-09-03). The
reasoning is §9; this is the "do this, in this order".

### 10.1 Always: block, don't spin (removes the ~45 % CPU)

The receiver's busy-poll on `ibv_poll_cq` was the #1 overhead (~48 % of a core
in disagg). Replace it with a completion channel + blocking poll:

1. `ibv_create_comp_channel()` — **one per CQ** (the compat layer rejects a
   shared channel). Use separate send/recv CQs → two channels.
2. On an empty poll: `ibv_req_notify_cq(cq, 0)` → re-poll (closes the arm/poll
   race) → `poll(cc->fd)` → `ibv_get_cq_event` + `ibv_ack_cq_events` → drain →
   re-arm. Watch the TCP control fd in the same `poll()` so a dead peer aborts
   the block instead of hanging.

Verified: `mac_cpu` 48 % → ~11 % in disagg, `prefill tok/s` / `decode tok/s`
unchanged. Comp-channel wakeup is ~30 µs (`MELONDMA_CQ_POLL_US`, default 50).

#### 10.1.1 Blocking delivery vs mapped-ring polling

From DEXT 0.376 the comp-channel worker blocks inside the DEXT until the device
completion generation advances, instead of scanning the mapped CQE ring on a
timer. The provider reports which delivery mode is live through
`ibv_mlx5_query_interrupts` (printed by `mlx_perf_test`). No client change is
needed — the worker picks the blocking path whenever the provider advertises
`MLX_UC_FEATURE_CQ_INTERRUPT`. What advances that generation on this machine is
the DEXT's EQ timer rather than an interrupt; see the note below.

Measured on an idle armed channel over 10 s (`mlx_cq_idle_cpu`), and on the
32-iteration peer gate (`run_cq_event_gate.sh`):

| worker path | idle CPU | wakeup median | wakeup max |
|---|---|---|---|
| blocking, 50 ms backstop (default) | 0.20 % of a core | 205 µs | 51.6 ms |
| blocking, 10 ms backstop | 0.90 % | 546 µs | 11.3 ms |
| mapped-ring polling | 2.95 % | 79 µs | 112 µs |

The arm doorbell is asynchronous, so hardware can process an arm just after a
CQE lands and raise no event; the worker's ring scan after each wait recovers
it, and the backstop caps what that costs. Two knobs:

- `MELONDMA_HW_WAIT_MS` (default 50) — the backstop. Lower it to shorten the
  rare tail at a proportional CPU cost. Keep it well above the link's own
  completion latency: at 2 ms the backstop started serving completions itself
  and the median rose an order of magnitude.
- `MELONDMA_HW_CQ_EVENT=0` — force the mapped-ring poller. Use it to A/B the
  two paths against one driver build, or if blocking delivery regresses on a
  given card.

Pick polling only when the p99 wakeup matters more than 15x the idle CPU.

On this machine MSI-X is never delivered to the DEXT: with the interrupt setup
ordered correctly and firmware accepting both interrupt indices, the driver's
own per-vector counters stay at zero through real traffic, and rebinding the
completion EQ to index 0 leaves them at zero as well. What delivers completions
is the DEXT's EQ timer, so its period is the latency floor here, not the
interrupt latency. The blocking path's CPU win is unaffected by that, but do not
plan a microsecond wakeup budget around it. `mlx_perf_test` prints the interrupt
counters and the current timer period.

#### 10.1.2 Hardware completion moderation

`ibv_mlx5_modify_cq_moderation(cq, period_us, max_count)` asks the NIC to hold a
completion event back until either bound is reached, so a streaming transfer
raises one event per batch instead of one per arm. It coalesces in the card, so
it removes wakeups rather than spending one to decide not to act. The same thing
is reachable without code changes as
`MELONDMA_CQ_MODERATION="<period_us>:<max_count>"`, applied when the CQ is
created.

Off by default. Moderation trades latency for wakeups and the right point is
workload-specific; a request/response exchange can only lose, while a long KV
stream is where it pays. Firmware accepts the setting on this card.

### 10.2 For small-message throughput: enable the direct (zero-syscall) path

`MELONDMA_DIRECT_UAR=1` (userspace WQE build + doorbell) and
`MELONDMA_DIRECT_CQ=1` (userspace CQE decode). Verified:
`PHASE2_FULL_GATE` 1,000,000 messages, 0 errors, 10 teardown cycles, no kernel
fallback.

Gotchas:
- Queue sizes are rounded to power-of-two (min 64) by the compat layer — don't
  code against the exact requested depth.
- Inline SEND is tracked by the direct SQ metadata path and can participate in
  a trusted mixed WR chain. Keep `GGML_RPC_RDMA_DISABLE_INLINE=1` only as an
  ablation/debug switch, not as a normal workaround.
- The direct decoder handles successful/error SEND/RECV/READ/WRITE/atomic CQEs.
  Exceptional DEXT-owned operations such as UMR/LOCAL_INV, an unknown QP or
  missing WR metadata use the reconciled kernel fallback.

### 10.3 Benchmark-harness gotchas (when re-running the sweep)

- The Spark `rpc-server` needs `GGML_RDMA_DEV=rocep1s0f1 GGML_RDMA_GID=3`
  explicitly — auto-detect fails because the TCP control host (192.168.100.2)
  ≠ the RDMA GID (192.168.200.2).
- `bench_rdma_cluster.py` inherits the parent env, so export
  `MELONDMA_DIRECT_UAR=1 MELONDMA_DIRECT_CQ=1` before launching to test the
  direct path.
- Write to a fresh `--out` file; don't clobber the baseline CSV.

### 10.4 Client basics that compound with the above

§5 in full, but the highest-leverage items: keep the pre-posted recv ring full,
unsignaled sends (1 signaled per N), `ibv_poll_cq(cq, N, wc)` batch drain,
register once + MR cache, one-sided RDMA WRITE for weights/KV (Send/Recv only
for <4 KB control).

## 12. Bypass the CPU: KV cache directly in memory (Apple Silicon)

The next lever after the zero-syscall data path: stop memcpy'ing the KV cache
through a staging buffer. On Apple Silicon the CPU and GPU share **unified
memory (UMA)** — a Metal `MTLBuffer` in `MTLResourceStorageModeShared` is a
regular host virtual address (`.contents`), which MelonDMA's `ibv_reg_mr` can
pin (`IODMACommand` → physical PAS → NIC DMA) exactly like any malloc'd buffer.
So the NIC can DMA the KV cache straight into Metal memory with no CPU copy.
This is now an implemented driver contract, not only a design proposal: DEXT
0.359 advertises `MLX_UC_FEATURE_COHERENT_UMA_MR` without an opt-in flag, and
`run_metal_dma_gate.sh` verified both transfer directions plus a 4 MiB indirect
MR against Spark at MTU 4096. The application is still responsible for choosing
`.shared`, preserving the buffer lifetime, and ordering GPU work around CQE/WC.

Concrete moves, highest impact first:

1. **One-sided RDMA WRITE into a registered Metal buffer.** Instead of
   SEND/RECV (which stages into a host buffer and then memcpy's into the KV
   cache), the Spark side RDMA-WRITEs the KV tensors directly into the Mac's
   KV-cache `MTLBuffer` (`StorageModeShared`, registered once at startup).
   Removes the CPU copy from the receive path entirely; the prefill→decode KV
   handoff becomes NIC→memory. Use `RDMA_WRITE_WITH_IMM` (seq in `imm_data`) as
   the completion signal — one round trip, no RQ on the data path.
2. **Pull-based KV transfer (KVDirect, arXiv:2501.14743).** The decode node
   PULLS the KV tensors it needs (one-sided RDMA READ on demand) instead of the
   prefill node pushing everything up front. KVDirect reports −55% per-request
   latency; it also reduces idling. This composes with (1): both use registered
   buffers and one-sided ops.
3. **Keep the KV buffer `StorageModeShared`** (never `StorageModePrivate` —
   private Metal memory has no host address and cannot be DMA'd by the NIC).
   Quantized KV (`q8_0`) stays host-addressable, so this is compatible.

Expected effect: lower `mac_cpu` (no memcpy) AND lower TTFT (DMA direct +
prefetch of exactly the KV tensors decode needs). Gate against the same
`bench_rdma_cluster.py` sweep; the direct SQ/RQ + direct CQ path (§10.2) must be
on for the one-sided path to be zero-syscall.

### Large-MR support (added for the Metal KV cache)

`ibv_reg_mr` now transparently handles buffers larger than one direct MR:

- Direct MR ≤ **1.875 MiB** (480 × 4 KiB PAS — hard 4112-byte firmware
  command-mailbox limit, `MLX_CMD_MAX_SIZE`).
- Larger buffers are **auto-chunked** into 1.875 MiB direct children and composed
  under one **indirect (KLM) MR**. The child cap was raised 32 → 240
  (`MLX_UC_MAX_INDIRECT_CHILDREN` / `RDMA_MAX_INDIRECT_MR_CHILDREN`), so one
  `ibv_reg_mr` call now covers up to **450 MiB** (240 × 1.875 MiB).

So the client can register a Metal KV region (`contents()`, 4 KiB-aligned) with
one `ibv_reg_mr` call; the compatibility layer creates the required direct
children and indirect parent internally. Requirements:
`addr`/`length` must be 4 KiB-aligned; buffers beyond 450 MiB per MR must be
split by the caller (or use `posix_memalign` + `bytesNoCopy`, §4.B of the recipe).

### Implementation status (one-sided KV handoff)

- [x] **DRIVER/HARDWARE VERIFIED:** `.shared | .untracked` Metal allocation,
      GPU producer → Spark, Spark → GPU consumer, and 4 MiB indirect MR all pass
      on DEXT 0.359 at MTU 4096 with zero GPU mismatches and no datapath fallback.
- [x] **VERIFIED (smoke)** `RPC_CMD_GET_TENSOR_RDMA` + `socket_t::rdma_write()`: the
      Mac advertises a registered dest buffer, Spark RDMA-WRITEs the KV straight
      into it, then sends only the 8-byte size header. Gate `GGML_RPC_RDMA_WRITE_KV=1`.
- [ ] Next: make that destination the final `ctx_tgt`/KV Metal buffer rather
      than an intermediate `cmd.dest`; publish its MR persistently and remove
      the post-transfer `set(local)` copy.
      In the current llama Metal backend this does not require a new allocator:
      Apple unified-memory devices default to page-aligned `vm_allocate` backing
      wrapped by `newBufferWithBytesNoCopy(...StorageModeShared)`. Register the
      stable `ggml_backend_buffer_get_base()` plus its total size, then address
      tensors by checked offsets inside that MR. A dedicated RDMA-compatible
      untracked buffer type is preferable to changing all Metal buffers.
- [ ] Remove the producer-side temporary chunk where possible. The current
      Spark handler still runs `ggml_backend_tensor_get()` into a vector before
      `rdma_write()`; if direct source registration is impossible, keep a
      persistent pinned double buffer and overlap extraction with RDMA.

## 13. Consumer-side transport levers (llama.cpp — not the driver)

These live in `ggml/src/ggml-rpc/transport.cpp`, not MelonDMA. Ordered by impact
(reasoning in `docs/rdma-optimization-levers.md`).

1. **Pipeline the one-sided `rdma_write`.** ✅ DONE — `rdma_write` now posts a
   batch of up to 16 WRITEs in flight (unsignaled except the last) and waits once,
   instead of post→poll per chunk. RC ordering lets the last WRITE's completion
   imply the whole batch. (manual §11.6/§11.7).
2. **Batch poll / hybrid completion.** The provider can drain multiple CQEs, but
   llama's blocking waiter currently requests one WC. Keep the single-WC fast
   path for a single signaled window; add bounded direct draining while active
   and arm/block only when idle.
3. **Unsignaled sends.** ✅ DONE for chained SEND/WRITE windows and periodic
   mailbox/inline traffic; only the final/periodic WR produces a CQE.
4. **Aggregate split activations.** WR chaining and inline ≤256 B already work
   for one contiguous stream. What remains is RPC-level aggregation of several
   independently produced mailbox items into one bounded post/completion.
5. **KV directly into `ctx_tgt`.** DRIVER DONE / CLIENT OPEN — register the final
   `.shared | .untracked` Metal KV buffer and WRITE into its exact offsets.
6. **Remove source staging.** Avoid the current Spark-side
   `ggml_backend_tensor_get()` → temporary vector → `rdma_write()` path when the
   backend exposes a stable registrable address; otherwise double-buffer it.
