# Shared-page fast path — design

Goal (Plan P1 "true userspace steady-state fast path"): after QP/MR setup, a normal
post/poll must not cross DriverKit. Today every direct post calls
`kMlxUCMethodSyncFastPath` (validate + bookkeep `sqHead`/`sqWrid`/`sqOpcode`/`sqSpan`)
and every non-empty direct drain calls `kMlxUCMethodSyncQpTails` (publish `sqTail`/
`rqTail`). This doc removes both while preserving the invariants the DEXT needs.

## 1. What the DEXT actually needs the shadow state for

| State | Consumer in DEXT | Used in direct-CQ steady state? |
|---|---|---|
| `sqWrid`/`sqOpcode`/`sqSpan` | kernel-mediated CQ decode | **No** — the shim decodes CQEs from the mapped ring itself |
| `sqHead` | SQ-full credit check, `mlxQpExpandCounter` anchor in CQ decode | credit check only |
| `sqTail` | `DestroyQP` in-flight check, SQ credit | yes |
| `rqHead`/`rqTail` | `DestroyQP` in-flight check, RQ credit | yes |

So in the direct-CQ path the DEXT only needs `sqHead`/`sqTail`/`rqHead`/`rqTail` for
**credit** and **DestroyQP in-flight** — not for completion decode.

## 2. Trust model (unchanged from today)

The client already supplies `sqTail`/`rqTail` to `SyncQpTails` as plain struct fields
that the DEXT applies with a monotonic max — a malicious client can already over-advance
them today. Moving those counters into a client-writable shared page is **the same trust
boundary**, not a new one. Data-access isolation stays on hardware PD/lkey/rkey (the NIC
checks every SGE against the MTT), and the SQ/RQ/CQ buffers are the client's own mapped
memory. `ReleaseOwnedResources` already force-`ResetQP` (hardware flush) + retry on a
`kIOReturnBusy`, which is the safety net for a crashed or lying client.

## 3. Where the shared state lives

The DB-record page is already mapped into userspace (`db_map`) and into the DEXT
(`GetClientDbRecord`). Each QP owns a **128-byte slot**; hardware doorbell records use
only the first 8 bytes:

```
offset 0  : CQ consumer record (hw, big-endian)        — already userspace-written
offset 4  : SQ producer record (hw, big-endian)        — already userspace-written
offset 8  : NEW software shadow state (userspace-written, DEXT-read)
```

```c
struct mlx_qp_shadow {           // 40 bytes at offset 8
    uint64_t sequence;           // userspace seqlock: odd=writing, even=stable
    uint64_t sq_head;            // posted WQE index (spans, not WR count)
    uint64_t sq_tail;            // completed signaled WQE index + span
    uint64_t rq_head;
    uint64_t rq_tail;
};
static_assert(sizeof(struct mlx_qp_shadow) == 40);
```

`sq_head`/`sq_tail` are **WQE-slot indices** (already include `sqSpan`), exactly the
values the shim already maintains locally. The writer acquires the seqlock with a
CAS (`__atomic_compare_exchange_n`) so concurrent post/poll publishers cannot hand
the DEXT a torn snapshot. No new mapping, no new memory kind — the page is already
shared. 40 bytes leaves 80 free in the slot.

## 4. Capability gate

Two-part gate. The DEXT advertises `MLX_UC_FEAT_TRUSTED_FAST_PATH` at open; the shim
additionally opts each QP in explicitly via `mlx_create_qp_req::rsvd` bit0
(`MLX_UC_QP_TRUSTED`). The DEXT sets `trustedFastPath` only when both the fast-path
bundle and that flag are present, and rejects unknown `rsvd` bits. Only then does the
shim stop calling `SyncFastPath`/`SyncQpTails`. The validated ExternalMethod path
stays the default and the capability-gated safe fallback.

## 5. Steady-state flow (trusted mode)

- **Post** (shim): build WQE in the mapped SQ, write `sq_head` into the shadow area,
  write the SQ producer hw record, ring the UAR doorbell. **No ExternalMethod.**
- **Poll** (shim): decode CQEs from the mapped CQ ring; on send completion advance
  `sq_tail` by `mlxQpExpandCounter(sq_head, wqe_counter) + sq_span` (already done),
  write `sq_tail`/`rq_tail` into the shadow area. **No ExternalMethod.**
- **Credit** (DEXT, only when a kernel-mediated op still runs): read `sq_head`/`sq_tail`
  from the shadow area instead of `ctx->sqHead`/`ctx->sqTail`.
- **DestroyQP** (DEXT): read the shadow area for the in-flight check; on `kIOReturnBusy`
  the existing `ResetQP` flush + retry path covers a stale/lying client.

## 6. Lifetime safety

1. The shim guarantees it stops posting before `DestroyQP` (single-threaded teardown).
2. `DestroyQP` reads the shared tails; if in-flight, it returns `kIOReturnBusy` and the
   caller flushes via `ResetQP` (2RST_QP) then retries — this path already exists and is
   exercised by the lifetime/soak gates.
3. MR/QP destruction order is unchanged (reverse dependency).

## 7. Rollout (each step gated + isolated before/after + 1M-message gate)

1. ABI: `mlx_qp_shadow` + feature bit + `static_assert`s. No behavior change.
2. Shim writes the shadow area on post/poll (always; harmless when the DEXT ignores it).
3. DEXT reads the shadow area in `SyncFastPath` credit + `DestroyQP`, behind the feature bit.
4. Shim drops `SyncQpTails` (per-drain crossing) when trusted — measure.
5. Shim drops `SyncFastPath` (per-post crossing) when trusted; DEXT stops writing
   `sqWrid`/`sqOpcode`/`sqSpan` for trusted QPs — measure.
6. Full matrix: `mlx_phase2_gate` 1M, recreate, stale-token, cross-client,
   teardown-under-load, `run_phase5_speed_gate.sh`, TCP40 comparison.
