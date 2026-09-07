# MelonDMA — Changelog

A chronological record of what was implemented, decided and measured, consolidated from
`dev/CHANGELOG.md`, `git-prod/CHANGELOG.md` and the dated notes in `dev/docs/*`
(2026-09-02 → 2026-09-07). Dates follow the source files; within a day the order follows the
original notes. Numbers are measured on one bench: Mac Studio M2 Ultra + ConnectX-4 Lx
`15b3:1015` (PCIe Gen3 x4 over Thunderbolt, ~31.5 Gbit/s/direction ceiling) ⇄ NVIDIA DGX Spark
(ConnectX-7, rdma-core 50.0), RoCEv2, MTU 4096.

## 2026-09-02 — bring-up, first benchmarks

**Automatic card takeover at cold boot (dev path).** `kernelmanagerd` guess-scans flat
`.dext` bundles in `/Library/DriverExtensions`; the DEXT wins the first match on IOProbeScore
**5000 vs 1000** (Apple). `install-to-libde.sh` flattens the `.systemextension` into a flat
bundle; `mlx_cold_takeover.sh` stages/refreshes it. Production verification (after
`installer -pkg` + reboot: `ioreg` shows `MlxPCIDriver` as owner + `IODEXTMatchCount >= 2`
without a manual takeover) was left for later.

**Full cluster benchmark (RDMA vs TCP-10G).** `bench_rdma_cluster.py`, models Qwen3.8-27B and
Qwen3.6-35B-A3B, 2 modes (disagg / split), contexts 512–65536. RDMA wins bulk: prefill
**+2–12 %** (grows with context), TTFT **up to −10.7 %** at 65K. The KV handoff is confirmed
byte-exact by NIC counters (`tx_vport_rdma_unicast_bytes` Δ ≈ 2×KV — it is transferred twice:
prefetch chunks + the final `get(net)`). The cost: **~45 % of a Mac core on busy-poll
`ibv_poll_cq`** in disagg; split decode is **−12 %** on RDMA (small activations).

**RDMA vs TCP-10G vs TCP-40G.** TCP-40G (AppleEthernetMLX5, MTU 2034) is **on par** with RDMA
on speed (Δ ≤ 1–2 %), but burns 3–8 % CPU vs 48 %. Conclusion: the disagg bottleneck is compute,
not transport; the cheapest +10 % is moving the RPC from 10GbE to 40GbE without RDMA at all.

## 2026-09-03 — blocking poll, direct path, shared-page fast path

**Track A: busy-poll → blocking poll.** llama.cpp `rdma_poll()` moved to a completion channel
(`ibv_req_notify_cq` → `poll(cc->fd)` → drain → re-arm). Driver side: the comp-channel worker
reads the mapped CQE ring directly (`rdma_cq_pending_local`, ~30 µs). **mac_cpu 48 → 8.5–10 %**;
the cost is TTFT +1–4 % and prefill −1–4 %.

**Direct path.** `MELONDMA_DIRECT_UAR=1` + `MELONDMA_DIRECT_CQ=1` recovered the TTFT
(65K: 137.5 → 137.0 → 139.5 s). After direct + one-sided + blocking poll: macCPU 48 → 9.1 %;
RDMA no longer loses to TCP-40G on speed/latency.

**Shared-page fast path (design).** QP shadow state (`sq_head/sq_tail/rq_head/rq_tail`, a
seqlock) in the already-shared DB-record page — so post/poll never cross into DriverKit.
Capability `MLX_UC_FEATURE_TRUSTED_FAST_PATH` + `MLX_UC_QP_TRUSTED` (`rsvd[0]` bit).

## 2026-09-04 — performance audit, GPUDirect reverse-engineering, Metal UMA

**Performance audit (applied).** Removed unconditional `IOLog` from the hot path (was 5×/CQE +
3×/SEND; now `MLX_DBGLOG` compiles to nothing at `MLX_DEBUG=0`). O(1) lkey→slot hash index (was
an O(512) scan per SGE; 27× on the host). Narrowed `fMethodLock` (data-path selectors under
`fOwnedLock`). Per-QP/per-CQ locks + O(1) QPN/CQN index. CQ-depth validation.
Capability-driven sizing of the QP/CQ/MR tables (`min(firmware caps, 4096)`),
`MLX_UC_MAX_SGE` 4→16.

**GPUDirect on Apple Silicon — reverse-engineering.** The GPU (AGX) uses unified memory; there
is no "separate VRAM behind BAR" and none is needed. DMA "NIC → MTLBuffer pages" is purely a
matter of DART registration through `IODMACommand` from the NIC's PCI function. Apple's TN3205
RDMA is send/recv-only, UC, TB5-only — not a competitor. A GPU-issued doorbell is impossible.
DEXT **0.359**: `MLX_UC_FEATURE_COHERENT_UMA_MR` on by default; `run_metal_dma_gate.sh` verified
GPU→Spark, Spark→GPU, and a 4 MiB indirect MR (MTU 4096, 0 mismatches). Indirect-MR children
32 → **240** (`MLX_UC_MAX_INDIRECT_CHILDREN`).

## 2026-09-05 — entitlements, direct-UAR gate, KV batch, TTFT analysis

**Security/entitlements.** `IOUserClient::CopyClientEntitlements`; diagnostic key
`com.mlx5.rdma.diagnostic` (`tools/diagnostic.entitlements`). Live DEXT authorization still
returns `0xe00002e2` until an Apple-approved path exists.

**Direct-UAR gate PASS** (`run_phase3_direct_uar_gate.sh`): direct SQ mapped,
`DIRECT_UAR_STATS`, non-zero `mapped_qps/direct_wrs/direct_doorbells/direct_recv_wrs`,
`fallback_send=0/fallback_recv=0`.

**Metal contract.** `MTLResourceStorageModeShared`, stable `MTLBuffer.contents`, `.untracked`
only with external NIC/GPU ordering; `.private` and a GPU UAR are closed.
`MlxRegisteredMetalBuffer` protects MR/PD + allocation generation + device epoch + slot leasing.

**Final-KV path.** `GGML_RPC_RDMA_WRITE_KV=1` + `GGML_RPC_RDMA_FINAL_DEST=metal`; marker
`GGML_RPC_FINAL_DEST_ACTIVE ... rdma_write=1 trailing_copy=0`. Live Metal acceptance:
32×1 MiB GPU→peer, 32×1 MiB reverse, 4 MiB indirect, 0 mismatches. Gates `PHASE3_WRITE_GATE`,
`PHASE3_REVERSE_WRITE_GATE`, `METAL_DMA_GATE` PASS.

**Hardware support policy** (`SUPPORTED_HARDWARE.md`): only CX-4 Lx PF `15b3:1015`
(match `0x101515b3`); CX4 PF/VF variants and CX5–CX8 are not claimed.

**TTFT 7–15 % target analysis.** Baseline TTFT (Qwen3.6-35B, disagg, 2048 ctx) **1.9120 s**;
targets −7 % = 1.7782 s, −15 % = 1.6252 s. Found: 80 sequential `submit_rpc_sync` instead of
one batch; Spark does `ggml_backend_tensor_get()` → temp → memcpy → TX slot; `rdma_write`
drains before returning (no overlap). KV ≈ 40 KiB/token; client quota 512 MiB.

**KV batch + Spark→Mac pipeline (llama.cpp, opt-in).** New `GET_TENSOR_RDMA_BATCH` (up to 256
descriptors): one request for 80 KV spans, one final `WRITE_WITH_IMM`. Spark reads backend data
**directly into registered TX slots** (temp vector removed), 2 generations × 8 WR overlap. The
key delivery fix: `MELONDMA_COMPLETION_POLICY=latency` removed the wait tail (Mac waited ~105 ms
with the batch ready in ~60 ms). Result: wait_ms **81.9 → 56.9 (−30.5 %)**, full get
**88.5 → 57.9 (−34.6 %)**, TTFT −28 ms (−1.77 %). DEXT in the run — **0.396**.

## 2026-09-06 — prefetch dedup, WRITE tuning, inbound ceiling, prod rollout

**Prefetch dedup.** In KV_BATCH mode the duplicate attention-KV download is skipped
(`GGML_RPC_RDMA_KV_PREFETCH=1` re-enables for A/B). The naive "drop the whole call" version was
rejected: TTFT −75 ms, but Mac CPU 11 % → **63 %** (blocking on RQ credits → SQ spin).

**WRITE tuning.** `GGML_RPC_RDMA_WRITE_CHUNK` (256/512/1024 KiB), `_WINDOW` (2/4), `_DEPTH`
(4/8), max 32 WR in flight (SQ 64), the last WR of a generation signaled + a final
`WRITE_WITH_IMM`. On this hardware the geometry **does not matter** — defaults.

**Mac inbound ceiling.** Spark→Mac **1.68 GB/s (13.4 Gbit/s)** vs Mac→Spark 2.62 GB/s
(21 Gbit/s); loopback through the card ~2.03 GB/s/direction (~4.06 GB/s total). More QPs do not
help (2 QPs share the same bandwidth); message size and MTU do not matter; no PAUSE/loss. MPEIN:
the line is **PCIe Gen3 x4**. The flip: the bottleneck is on the Spark side — `ib_write_bw`
loopback on Spark gives the same 12.6 Gbit/s (port-to-port 13.25).

**"One model over RDMA" — our mistake, not the driver.** The driver gives each client its own
GID slot (`GetGidIndex`/`SetGid`/`QueryGid` only to the owner). The transport always used
`GGML_RDMA_GID=0` (someone else's slot) → the second client failed. `transport.cpp` fix: without
an explicit index, take the client's own slot. Added `GGML_RDMA_GID_ADDR` (select GID by address,
survives reboots).

**`iommu.passthrough=1` on Spark.** SMMU translation was throttling the NIC's memory reads on
transmit. Spark loopback `ib_write_bw`: **12.6 → 100.8 Gbit/s**; port-to-port 13.25 → 103.8;
Spark→Mac 13.2 → **23.0**; Mac→Spark 21.0 (unchanged). TTFT at 16k: 5548.9 → 5385.9 ms (−2.9 %);
KV in the wild 62.84 MiB: 45.2 → 25.1 ms. Huge pages for the source: +7 % only on a large buffer
(zero on reused rings).

**Prod rollout (the production router).** `CLUSTER_KV_BATCH=1` (+ `KV_BATCH`, `FINAL_DEST=host`,
`DEST_ARENA_MAX=96`, `COMPLETION_POLICY=latency`, `KV_FENCE=0`), 5 RDMA env vars,
`GGML_RPC_RDMA_RX_DEPTH=160` (40 MiB receives). It surfaced that **the router had not been
working over RDMA at all** (it silently fell back to TCP; the cause was the auto-select looking
for a GID matching the TCP socket address 192.168.100.1 + missing IP/MAC). Result: prefill
**+8–10 %**, TTFT **−7–9 %** vs TCP40G, **−21–26 %** vs the old RDMA. Split decode: 55.2 vs
46.9 tok/s (35B, 512 ctx); the only loss is split TTFT at 32–65k (−0.6–1.7 %).
`GGML_RPC_REQUIRE_RDMA=1` is now honoured (it used to silently fall back to TCP).

**Driver: PortStats + AccessReg.** Both selectors were declared but not implemented.
`PortStats` — PPCNT group 0 (IEEE 802.3: packets/bytes/errors/pause) + group 1 (RFC 2863:
discards) + link state (QUERY_VPORT_STATE); `rxPause`/`txPause` fields. `AccessReg` — ACCESS_REG
passthrough (MPEIN for the PCIe link), payload 256 → 512 B. `tools/mlx_port_counters --watch
--pcie`. Inbound through the card's own counters is clean: 24 × 90.18 MB, 0 errors/0 discards/0
pause.

**Changelog updated (git-prod).** Added `PortStats`/`AccessReg`; added the re-measure of
Qwen3.6-35B: disagg TTFT 0.974→0.909 of TCP, prefill 1.03→1.10; TP-decode 55.2 vs 46.9 (512) and
22.1 vs 21.2 (65536). The remaining split TTFT at 32–65k is 0.6–1.7 % behind.

## 2026-09-07 — MSI-X delivery (resolved), SRQ, UD, compatibility, standard capture

**MSI-X is delivered — closed.** The earlier "platform limit" verdict was overturned; the defect
was ours. Root cause: exactly one place in IOPCIFamily writes the MSI-X table
(`IOPCIMessagedInterruptController::initDevice`, reachable from `allocateDeviceInterrupts` /
`restoreDeviceState`); `IOPCIDevice::Reset()` restores the table, but a **manual FLR-bit write is
invisible to the kernel** — the driver was wiping the table the kernel had just filled.
`PerformFlr()` now calls `Reset(kIOPCIDeviceResetTypeFunctionReset)` first, with a manual FLR as
the fallback. The second cause: the table was written with `data = 1834 + V` (the global AIC
number) instead of the index inside the controller's 32-vector map (1..9). Two vectors:
`MLX_SINGLE_MSIX_VECTOR=0`, async → host 0, completion → host 1 (`ConfigureInterrupts(2,9)`).
Verified: loopback 800 iterations — exactly 1 IRQ per iteration; two machines
`irq_eqes=20 timer_eqes=0 lost=0`; wakeup median **80 µs / p99 88**. Platform remainder:
rdar://118153788 (vectors are not reset on re-match; the vector count changes only via
re-enumeration).

**CQ moderation — closed negatively.** `events/iter = 1.00` for batches 1..32 and any
moderation: one event already covers the whole batch. The win is grouping signaled operations
(p50 per operation: 114 µs(1) → 5.95(8) → 2.20(32)).

**Blue-flame per QP — works.** 1.00×/2.01×/3.62×/4.63× (previously ~1.5×); the hardware limit
is `bf_regs_per_uar = 4`, from the 5th QP the registers are shared. WQE shapes: inline 3.79 µs,
1 SGE 6.04, 4 SGE 6.25, 16 SGE 9.58. Units bug: `max_tx_speed` is in ×100 Mbit/s, the driver
multiplied by 1000 (320 instead of 32.0 Gbit/s).

**Compatibility with external clients.** Method — `objdump -T` over `ib_write_bw`/`ib_send_lat`/
`rping`/`libuct_ib.so`: **44 symbols** needed, 28 present, 16 missing. All 44 are now exported.
Principle: never a missing symbol, never a lie; unimplemented things return `EOPNOTSUPP` with a
reason so the consumer falls back instead of crashing. `ibv_qp_to_qp_ex` is real
(`wr_start`/`wr_complete` — a chain in one call).

**SRQ — implemented and gated.** The modern `CREATE_SRQ` is the **RMP** object (the receive
queue is a linked list, not a ring). `MlxSRQ` (create/destroy/`post_srq_recv`/query/
`MODIFY_RMP`/WQE return), 5 selectors, QP binding `rq_type=1` + `srqn_rmpn_xrqn`@0x568,
6 shim functions, real `ibv_create_srq`/`ibv_destroy_srq`/`ibv_post_srq_recv`/`ibv_modify_srq`/
`ibv_query_srq`/`ibv_get_srq_num`. `mlx_srq_gate` PASS: 40 circular exchanges, depth 16, 48
receives, 52 completions. 5 defects found by measurement (queue lookup via CQE is a dead end —
`srqn_uidx=0`; the QPC cannot declare both an SRQ and its own ring; two decoders on one ring;
split wr_id ownership; the free list is a circular chain). Methodological takeaway: "check
whether the data path goes through the driver at all" (a repeat of the SRQ mistake).

**UD (datagram) — implemented and gated.** `mlx_ud_gate` PASS: all three transitions, AH, refuse
a receive without header space, the header flag in the CQE, and the payload after 40 bytes
verified byte-exact. 6 defects (the RST→INIT optional-parameter mask for datagram; the service
type hardcoded; `MlxWqeDatagramSeg` was an 8-byte stub instead of a 48-byte address vector; the
receive-length check only in the driver while receive runs direct in userspace; datagram post
went through the direct path without an address vector — now through the driver; CQE flags
translated one-by-one).

**Standard capture without injection.** `mlx_cold_takeover.sh resume-standard` three times in a
row — `STANDARD CAPTURE OK: MlxPCIDriver owns the card without injection` (no IOCatalogue
injection). This item was listed as requiring the Apple entitlement — it needs to be rewritten
per the evidence.

**Remaining (as of this date):** a registration cache keyed by "owner + generation"
(registration costs 1.18–1.75 ms — the largest item; the current "cache" looks up by VA *after*
registration), several UAR pages per client (lift the 4-QP ceiling), disabling the QP's own RQ
buffer under SRQ, SRQ/UD checks against a peer; DCQCN (needs a switched fabric), moving to
another card, re-validating the standard capture on a SIP-on machine.

---

## Performance summary (ConnectX-4 Lx, PCIe Gen3 x4)

| Metric | Value |
|---|---|
| Kernel-mediated RTT (synchronous ping-pong) | ~73 µs / ~13,700 msg/s (latency-bound) |
| Direct UAR+CQ RTT (mlx_rtt_bench, 64 B) | p50 **8.67 µs**, p99 12.08 (kernel fallback p50 53.92) |
| Single-QP RDMA WRITE, 1 MiB | 20.1–20.7 Gbit/s |
| 8-QP aggregate | 21.16 Gbit/s |
| Pipelined all-gather 4/16 MiB | 23.15 Gbit/s |
| Mac → Spark transmit | 21.0 Gbit/s |
| Mac ← Spark receive (before `iommu.passthrough=1`) | 13.4 Gbit/s |
| Mac ← Spark receive (after `iommu.passthrough=1`) | **23.0 Gbit/s** |
| MSI-X wakeup (two machines) | median 80 µs / p99 88 µs |
| Practical host-link ceiling | ~25–28 Gbit/s (theory ~31.5) |

Production gates P0/P1/P2/P3 (live-verified): P0.1 1M SEND/RECV + recreate + READ/WRITE +
RNR/retry + reconnect; P0.2 multi-client isolation; P0.3 lifetime/stale-handle; P1.1 quotas;
P2.1 QueryStats; P2.2 ABI-fuzz 24/24; P2.3 authentication (HMAC-SHA256); P3 inline (512 B) +
atomics + SL=3 + solicited_only + GID-table + DCQCN roundtrip — all PASS.

External blockers: Apple entitlements (`com.apple.developer.driverkit.transport.pci` +
`...userclient-access`) — pending; no-FLR re-init does not pass on this firmware (FLR recovery
is the working fallback); `MlxHealth` is a skeleton.
