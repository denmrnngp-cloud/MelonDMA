# MelonDMA — performance audit

Goal: speed, the most out of any connected card, minimal overhead.
Read the whole hot path of the DEXT (`MlxQP`, `MlxCQ`, `MlxMR`, `MlxUAR`,
`MlxCmd`, `MlxEQ`, `MlxUserClient`, `MlxWQE.hpp`) and the userspace layer
(`librdma_shim`, `libibverbs_compat`).

**Short version:** the protocol and firmware are already "maxed out" — the
current ceiling (~25–28 Gbit/s) is the PCIe Gen3 x4 host link (ADT-Link adapter), not
the driver (§2, `research.md`). The driver does NOT get the most out of faster
cards: the hot path was full of unconditional `IOLog` dumps, and every limit and
table size was hardcoded instead of read from firmware capabilities. Removing the
two P0 items and P1 gives a multiple-x IOPS increase on any card and lifts the
artificial ceiling for ConnectX-5/6/7/8.

---

## 1. Current measured baseline (`research.md` §5–6)

| Configuration | Result |
|---|---|
| Synchronous kernel-mediated RTT | ~73 µs, ~13 700 msg/s |
| One-sided single-QP RDMA WRITE 1 MiB | 20.1–20.7 Gbit/s |
| 8-QP aggregate | 21.16 Gbit/s |
| Pipelined all-gather 4/16 MiB | 23.15 Gbit/s |
| Practical host-link ceiling | ~25–28 Gbit/s (theory ~31) |

The ceiling is PCIe Gen3 x4 (the card supports Gen3 x8), DEXT/host serialization
and host-link pacing. The findings below are what the driver does *on top of*
that ceiling and what holds it back on faster hardware.

---

## 2. Findings (ranked by impact)

### P0.1 — Unconditional `IOLog` in the hot path (the main overhead source)

`IOLog` writes to the kernel log buffer; under sustained traffic it serializes the
whole DEXT process. The code itself acknowledged this in the `SyncFastPath`
comment: "per-batch logging serializes the DriverKit process". Three places logged
unconditionally **on every operation**:

- `MlxQP::CompleteCQE` — **5× IOLog per CQE** (header + 4 lines of a 64-byte CQE
  hex dump). Every CQE goes through `PollCQ → CompleteCQE`, including the "fast
  path" (the poll stays kernel-mediated). This is the dominant per-message cost —
  worse than the doorbell itself.
- `MlxQP::PostSendBatch` — **3× IOLog per SEND WQE**.
- `MlxQP::PostSendAtomic` — 4× IOLog per WQE.

**Fix (applied):** the hex dumps were removed entirely; verbose logs moved to
`MLX_DBG` → `MLX_DBGLOG` (`Sources/core/MlxLog.hpp`), which compiles to nothing at
`MLX_DEBUG=0`. Release (default) is quiet, dev is `make MLX_DEBUG=1 dext`.
Error-CQE/fatal logs stay always-on.

### P0.2 — `ValidateRange` — linear MR-table scan per SGE

`MlxMR::ValidateRange` → `LookupByLkey` was `for (i = 0; i < 512; i++)`. Called
**on every SGE** in `PostSendBatch`, `PostSendSge`, `SyncFastPath`,
`SyncSendSge`, `PostRecvBatch`, `PostRecvSge`. A batch of N WRs = N×512 scan.

**Fix (applied):** a hash index keyed by lkey/rkey (slot+1 open addressing,
`MlxKeyIndex.hpp`), O(1) amortized. Pure internal DEXT structure, no ABI change.
Measured 27× on host (32.3 → 876.9 Mlookup/s).

### P1.1 — `fMethodLock` serializes ALL ExternalMethod per client

Every `ExternalMethod` (including `PollCQ`, `PostSend`, `PostRecv`,
`SyncFastPath`) took `fMethodLock`. A multi-threaded client (prefill+decode in
parallel, several QPs) serialized on one mutex.

**Fix (applied):** data-path selectors skip `fMethodLock` and are refcounted under
`fOwnedLock` instead; Cleanup sets a teardown flag and drains the in-flight count
before freeing the ownership tables.

### P1.2 — `PollCQ` takes two global locks + O(n) scan

`MlxCQ::PollCQ` held the global `MlxCQ::s->lock`, then `MlxQP::CompleteCQE` held
the global `MlxQP::s->lock` and did an O(128) `CtxForQpn` scan (plus a full-table
scan in the ambiguous-shared-CQ fallback).

**Fix (applied):** per-CQ and per-QP locks (pre-allocated arrays), a `tableLock`
for slot lifecycle, and an O(1) QPN/CQN hash index. `DestroyQP` marks the QP
`state=ERR` before the firmware destroy to close the post-in-flight TOCTOU.

### P1.3 — CQ depth not validated against Σ(SQ+RQ)

`MlxCQ::CreateCQ` clamped entries to 64..2048 but never checked
`CQ depth ≥ Σ(SQ+RQ)` of the QPs sharing the CQ. At high IOPS this means CQ
overflow → QP ERR.

**Fix (applied):** `CreateQP` now warns when `CQ depth < SQ+RQ` (warn, not
reject — a shared CQ may legitimately span several small QPs).

---

## 3. "Most out of any card" — hardcoded limits vs firmware capability

Firmware already reports (`research.md` §4): `logMaxQp=14` (16 384),
`logMaxCq=24`, `logMaxMkey=24`. The driver ignored them:

| Constant | Value | Firmware allows | Where |
|---|---|---|---|
| `MLX_QP_TABLE_CAP` | 128 | 16 384 QP | MlxQP.cpp |
| `MLX_CQ_TABLE_CAP` | 256 | 16M CQ | MlxCQ.cpp |
| `MLX_MR_TABLE_CAP` | 512 | 16M MKey | MlxMR.cpp |
| `MLX_UC_MAX_SQ_DEPTH`/`RQ` | 4096 | `log_max_wq_sz` (CX5+ larger) | MlxUCIO.h |
| `MLX_UC_MAX_SGE` / `MLX_RC_MAX_SGE` | 4 / 16 | `log_max_send_sge` (CX5+ 30+) | MlxUCIO.h / MlxWQE.hpp |
| `MLX_UC_MAX_INLINE_DATA` | 512 | inline capability (CX5+ ~1 KB) | MlxUCIO.h / MlxWQE.hpp |

On CX-4 Lx this does not hurt (the card is Gen3 x4 anyway). On CX-5/6/7/8 in a
proper slot these constants **artificially cut** the QP count (channel
parallelism), SQ/RQ depth (in-flight WRs → IOPS), SGE and inline.

**Fix (applied):** the QP/CQ/MR tables are now dynamically sized from firmware
caps at Init (`min(firmware max, 4096)`), and `MLX_UC_MAX_SGE` was raised to 16.
Per-client quotas (`MLX_UC_MAX_*_PER_CLIENT`) stay as a deliberate DoS bound.

**Important:** a PCI-ID table entry is not generation support. CX-5/6/7/8 each
need a backend + capability validation + a live gate. Capability-driven sizing is
preparation, not a substitute for validation.

### What is already done right (do not touch)

- Adaptive GID/MTU discovery selects the active values; RoCE MTU 4096 @ Eth MTU
  9000.
- DCQCN is firmware-driven (`MlxCC` = QUERY/MODIFY_CONG_PARAMS), the driver does
  not implement its own loop.
- Unsignaled sends, inline (512 B), doorbell batching, multi-SGE, poll batches
  (16), immediate, RDMA WRITE_WITH_IMM, atomics — all in the ABI.
- Per-client isolated UAR/SQ/RQ/CQ (a global UAR is forbidden — correct).
- ACK request frequency = 8 (matches Linux `MLX5_IB_ACK_REQ_FREQ`).

---

## 4. Deferred (non-blockers, known plan items)

- **MSI-X is wired but never delivered** (2026-09-04, DEXT 0.388). The vectors
  are configured after the FLR, dispatch sources are created and enabled, and
  firmware accepts `CREATE_EQ` on either interrupt index — yet the driver's own
  per-vector counters stay at zero through real traffic, and rebinding the
  completion EQ to index 0 does not change that, so it is not an index
  mismatch. The EQ timer is therefore the delivery path and its period is the
  latency floor. The blocking-delivery path built on top of it still cuts an
  idle armed channel from 2.950 % to 0.190 % of a core, and the timer now runs
  at 100 ms when no CQ is allocated (0.150 % against 0.750 % for the DEXT
  itself). The likely cause is outside the driver: the card is claimed through
  IOCatalogue injection rather than a normal match, so interrupt routing is
  probably never established. `mlx_perf_test` prints the counters and the
  current period; `mlx_irq_probe` rebinds the vector to re-test.
- **Blue-flame doorbell** not used (regular 64-bit doorbell only). On CX-4 Lx
  through TB Gen3 x4 the gain is minimal and BF adds ordering risk.
- **No userspace MR cache** in the shim. Registration is 5–20 ms for a large
  buffer; the client should register once and keep a buffer pool (NCCL
  `net_ib/reg.cc` pattern). Documented in `inference-client-guide.md`.
- **NVIDIA-style GPUDirect RDMA** — N/A on macOS + M2 Ultra, but DEXT 0.359 has
  a verified Apple alternative for data: register `.shared | .untracked`
  `MTLBuffer.contents` as an ordinary coherent UMA MR. Final inference KV
  placement remains a client integration task; `.private` memory and a
  GPU-issued PCIe doorbell are unsupported.

---

## 5. What each fix gives (application order)

1. **P0.1 (log gating)** — immediate, minimal diff, applied. Removes 5 IOLog/CQE
   and 3 IOLog/SEND. The biggest per-message win on any card.
2. **P0.2 (O(1) lkey index)** — removes the O(512) per-SGE scan; critical for
   batch posting.
3. **P1.1 (narrow `fMethodLock`)** — parallel poll/post on different QPs.
4. **P1.2 (per-resource locks + QPN index)** — removes QP/CQ contention.
5. **P1.3 (CQ depth check)** — guards against QP→ERR at high IOPS.
6. **§3 (capability-driven sizing)** — lifts the artificial ceiling for
   CX-5/6/7/8.

Measurement recipe: before/after run `tools/mlx_phase2_gate.c` (1M messages) and
`run_phase5_speed_gate.sh`; compare `posted*`/`completed*`/`cqLost` via
`kMlxUCMethodQueryStats` and watch `ethtool -S` prio3 pause/discards on the peer.
