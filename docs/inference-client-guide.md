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

## 8. Known driver limits (important for a client)

- MR ≤ ~1.875 MiB in one direct mkey (480×4 KiB PAS); larger buffers are chunked
  or use an indirect (KLM) MR (`RegMRIndirect` + `PostUmrKlm`).
- `max_inline_data = 512` B; SGE ≤ 16.
- SQ/RQ depth ≤ 4096; CQ depth ≤ 2048.
- GPUDirect RDMA is unavailable on macOS/M2 (no CUDA) — GPU/MLX data goes through
  pinned host staging.
- Device: explicit GID/MAC configuration (`rdma_set_roce_address`), no automatic
  `enX`/ARP.
- No UD/DC/XRC/SRQ/multicast; RC only.

The full architecture — `docs/architecture.md`; current status and gate results — `CHANGELOG.md` (repo root).
