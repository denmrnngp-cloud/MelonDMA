# MelonDMA — Changelog

A chronological record of what was implemented, decided and measured, consolidated from
`dev/CHANGELOG.md`, `git-prod/CHANGELOG.md` and the dated notes in `dev/docs/*`
(2026-09-02 → 2026-09-07). Dates follow the source files; within a day the order follows the
original notes. Numbers are measured on one bench: Mac Studio M2 Ultra + ConnectX-4 Lx
`15b3:1015` (PCIe Gen3 x4 over Thunderbolt, ~31.5 Gbit/s/direction ceiling) ⇄ NVIDIA DGX Spark
(ConnectX-7, rdma-core 50.0), RoCEv2, MTU 4096.

## 2026-09-09 — UC QP, SEND_WITH_INV and repeatable rebuild gate

**UC QP implemented.** The stable UserClient ABI, `librdma_shim` and the
`libibverbs` compatibility layer now create a UC QP. Its QPC transitions use
the connected RoCE address path with RC-only retry/atomic/read permissions
cleared. SEND, SEND_WITH_IMM, RDMA_WRITE and RDMA_WRITE_WITH_IMM are accepted;
READ, atomics, LOCAL_INV, UMR and MW bind are rejected rather than being
silently given RC semantics.

**SEND_WITH_INV live peer PASS.** The provider emits mlx5 `SEND_INVAL`
(`0x01`), places the invalidated rkey in the control segment, and reports
receive CQEs as `WC_WITH_INV` with `invalidated_rkey`. The compatible public
surface is `IBV_WR_SEND_WITH_INV` with `ex.invalidate_rkey`. A stock
rdma-core peer bound a Type-2 MW; the Mac received a successful SEND CQE and
the peer observed `WITH_INV` with the exact bound rkey (`0x10790d`). The live
test exposed and fixed a public-shim-to-DEXT opcode translation mismatch, then
passed on DEXT 0.528.

**Gate tooling fixed and rerun.** `mlx_metal_dma_gate` had retained the old
`com.mlx5.rdma.dext` entitlement; rebuilding it with the current
`com.melondma.rdma.dext` entitlement restored UserClient access. DEXT 0.527:
hot update PASS, SRQ_GATE PASS and AFTER_REBUILD PASS (direct/reverse Metal
WRITE plus indirect 4 MiB MR).

**Ownership handoff gate is reboot-safe; final run pending.** The initial
live attempt established that this DriverKit deactivation returns
`willCompleteAfterReboot` (rawValue 1). That is the documented System
Extensions lifecycle, so `run_ownership_handoff_gate.sh` is now a persisted
cross-reboot state machine rather than an orphan-prone live loop: it records
the next stage, requires Apple ownership after a normal restart, then
reactivates MelonDMA and checks MSI-X/UserClient. It refuses reset, injection,
manual Apple termination and forced rematch. DEXT 0.528 recovered normally
after the exploratory run; hot-update check, UserClient preflight and live
SEND_WITH_INV peer gate all passed. The three ordinary reboot cycles still
need to be executed before this item can be marked PASS.

**No-FLR rebuild boundary measured precisely.** DEXT 0.534 exercised the full
same-firmware close/open: `TEARDOWN_HCA`, explicit `TAKE 4471/4471`,
`DISABLE/ENABLE`, ISSI, boot/init pages and `INIT_HCA` all completed with the
same command queue and software-owner ID. The CX-4 Lx nevertheless accepted
`MODIFY_NIC_VPORT_CONTEXT(roce_en=1)` while readback remained zero for the
bounded 200 ms; FLR immediately restored RoCE. This remains a firmware limit,
with verified FLR recovery retained. The live run also proves explicit page
reclaim, but firmware emitted no negative `PAGE_REQUEST` on teardown (`0/0/0`),
so that specific event path remains hardware-unobserved. After the update,
hot-update check, UserClient preflight and the Spark peer SEND_WITH_INV gate
passed again (`rkey=0x107910`).

**Port control path added without touching the live fabric.** DEXT 0.535
whitelists PAOS in the diagnostic ACCESS_REG path and `mlx_port_link` now
reads PAOS/PTYS/PFCC and provides Linux-compatible, explicitly guarded writes
for pause, PFC, Ethernet admin protocol mask and admin UP/DOWN. Every write
requires its operation plus `--apply`; the default is read-only. Live readback
passed: port admin/oper UP, 40 Gbit/s, pause RX/TX enabled and PFC disabled.
No port-setting write was issued on the direct production link; its functional
validation belongs to the future switched-fabric ECN/PFC test.

**ECN/PFC/DCQCN validation explicitly unavailable on this bench.** The
available topology is only the direct Mac⇄Spark pair; there is no managed
switch able to mark ECN or exercise priority flow control. Accordingly, no
PFCC/PAOS/PTYS write is run against the live link and no CNP/rate-reaction
claim is made. This is recorded as a hardware-dependent untested scenario,
not an unfinished driver implementation.

**QPEx SEND_INVALIDATE completed and deployed.** DEXT 0.537 adds the upstream
rdma-core-compatible `ibv_wr_send_inv(qp, invalidate_rkey)` hook. The QPEx
builder translates it to `IBV_WR_SEND_WITH_INV`; the ordinary provider path
then emits the mlx5 `SEND_INVAL` WQE. `WR_EX_GATE` passed on the live DEXT,
then the independent exact `WR_EX_SEND_INVALIDATE` gate called that QPEx API
on Mac and the Spark Type-2 MW peer confirmed `WITH_INV` with the same rkey
(`0x108413`). The production-readiness map now also records that command
completion is already event-driven through the async EQ; a duplicate
command-only MSI-X vector would add no missing functionality.

**Apple direct-memory audit and verbs async-EQ completion.** The useful technical
findings from `RDMA-BUFFER-PATH.md` and `REVERSE-ENGINEERING.md` were compared
with `PRODUCTION-DRIVER-ANALYSIS.md` and `DEEP-ANALYSIS.md`. Direct WQE/CQE/UAR
mapping, DB-record ordering, barriers, DS×16, shared BF toggle/rearm, kernel-owned
MR plus cache, command geometry and safe clock calibration were already present.
The one software-only residual was standard QP/SRQ/CQ async events: DEXT **0.538**
now subscribes to and decodes path migration, communication established, SQ drained,
QP request/access/fatal, QP last-WQE, SRQ error/limit and CQ error. CQ/QP/SRQ object
ids cross the UserClient only as owning-client generation tokens and land in the
matching standard `ibv_async_event` union member. The CQ-error IFC offset was corrected
to `cqn@0x108`, `syndrome@0x158`; ABI/host/DriverKit checks, hot update, active-driver
preflight and `WR_EX_GATE` all passed. Individual fault events are correctly marked
hardware-unobserved rather than claimed as live-tested.

The post-update attempt to rerun the independent Spark `WR_EX_SEND_INVALIDATE` peer gate
did not begin because the current SSH credential was rejected before the test was launched.
This is recorded as an environment-access interruption, not a PASS or a driver failure;
the exact live peer PASS remains the DEXT 0.537 result above.

**UD reply-address helper completed without a hardware dependency.** The public
`ibv_init_ah_from_wc` and `ibv_create_ah_from_wc` now construct a RoCEv2 reply AH
from a receive WC/GRH instead of returning `EOPNOTSUPP`. They validate IPv6+UDP GRH,
resolve the received destination GID to the correct local index, preserve flow label,
traffic class and hop limit, and retain the provider's explicit peer-MAC requirement.
This closes a real compatibility hole in the existing UD receive path without adding
unsafe ARP inference or changing the DEXT. Host compatibility tests PASS.

**Async events are now multi-client safe.** Completing the full verbs async-EQ route
revealed that a device-wide FIFO could let client B dequeue client A's CQ/QP/SRQ event,
then reject its foreign token and lose the event. DEXT **0.539** now scans the bounded
FIFO and removes only an event matching the requesting client's generation-token table;
events for another client keep their ordering and remain queued. Device and port events
retain their existing device-wide semantics. Host/DriverKit checks, hot update, preflight
and `WR_EX_GATE` passed; natural QP/CQ/SRQ fault delivery remains hardware-unobserved.

**FLR recovery reverified on DEXT 0.539.** A live `mlx_probe --reinit` completed successfully;
both the independent post-recovery preflight and WR-EX data-path gate passed. The framework
reset path confirmed its DMA boundary and restored/re-armed MSI-X, without a reboot.

**Ownership gate handles both macOS reboot boundaries.** The reverse Apple → MelonDMA
activation can also return `willCompleteAfterReboot` when `sysextd` has an old DEXT record
to replace. `run_ownership_handoff_gate.sh` now persists `await_melon_boot` and resumes by
verifying ownership, MSI-X and preflight after the normal reboot; it never substitutes
catalogue injection, process kill or forced rematch for that transition.

**Clean enable/disable ownership result is now measured, not assumed.** After the deferred
activation completed on reboot, `com.melondma.rdma.dext` 0.539 was installed and
`activated enabled`, its PCI personality was present (`IODEXTMatchCount=2`), but Apple still
owned the already-bound PCI nub. Thus ordinary System Extension enable plus reboot does not
perform Apple → MelonDMA rematch on this macOS installation. The three-cycle clean ownership
gate is recorded as a platform blocker; dev-only catalogue/cold-capture paths are deliberately
not counted as a solution.

**Targeted rematch trigger was rejected by macOS.** `mlx_rematch_probe` was run as root with
its iocatalog entitlements verified, but `IOServiceRequestProbe()` returned
`kIOReturnNotPrivileged` for the occupied ConnectX nub and `kIOReturnUnsupported` for its PCI
parent. It did not alter ownership: Apple remains owner and MelonDMA remains activated/enabled.
Taking the card by stopping Apple's driver would be a separate dev-only disruptive operation,
not a normal enable/disable implementation.

**Authorized dev takeover verified.** After stopping the live AppleEthernetMLX5 process, the
kernel selected `MlxPCIDriver` on the third restart. UserClient preflight and the complete
WR-EX data-path gate passed with MelonDMA as the active PCI owner. This proves the development
recovery path works; it intentionally does not change the clean ownership-gate verdict.

**Production delivery boundary audited.** The active development bundle carries both PCI and
scoped UserClient entitlements, but this host has SIP disabled and development boot arguments.
Developer ID signing, Apple provisioning profiles, notarization and a clean SIP-on install have
not yet been configured and verified through `mlx_production.sh`; this is a release-validation
blocker, not a missing data-path feature.

**Client integration guide refreshed.** `inference-client-guide.md` now describes the actual
provider contract for any verbs client: entitlement and ownership prerequisites, explicit RoCE
addressing, connection/authentication protocol, supported verbs/QPEx surface, MR/QP/CQ lifetime,
capability-driven direct path, Metal UMA rules, diagnostics and the remaining deployment limits.
Runner-specific GGML tuning is explicitly separated from the driver ABI.

**Live Apple handback helper repaired and verified.** `mlx_dev.sh driver-release` now accepts
the loader's asynchronous `rawValue: 1` result and waits for actual PCI ownership rather than
failing on the intermediate status. A live MelonDMA → Apple transition unloaded the DEXT and
returned the card to `DriverKit_AppleEthernetMLX5` without reboot. The command is explicitly
dev-only: it refuses active RDMA clients; the reverse direction still uses activation plus the
disruptive dev takeover and is not a production ownership solution.

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

## 2026-09-08 — Apple DEXT parity: cache line, board id, UAR geometry

Source: the decompiled `com.apple.DriverKit-AppleEthernetMLX5.dext` under
`dev/donors/apple-rdma/`, cross-checked field by field against `mlx5_ifc.h` on the
Spark, then measured on the card at DEXT 0.482.

**`cache_line_128byte` is now set.** Apple propagates this cap from the max caps in
`handleHCACap` (`0x100010c4c`); Linux sets it whenever the host cache line is at least
128 bytes. MelonDMA read the bit and only logged it, so firmware was told to assume a
64-byte line on a host whose every core has a 128-byte one, and each CQE and EQE write
landed as a partial line.

**`QUERY_ADAPTER` (0x101) added**, mirroring Apple's `queryBoardId`: the 16-byte
`vsd_contd_psid` and the IEEE OUI, logged once after the capability read. `15b3:1015`
covers every ConnectX-4 Lx ever built, so this is the only thing that names the board.

**UAR page 4 KiB → 16 KiB.** Apple writes `log_uar_page_sz = 2` (`0x100010c44`) and
addresses UAR pages as `index << 14`; MelonDMA wrote 0 and additionally set `uar_4k`,
which makes firmware hand out indices in 4 KiB units. The pair now matches Apple.
Consequences:

- Blue-flame registers per client stay at 4, and the first attempt to claim
  otherwise was wrong. A UAR page larger than 4 KiB is padding, not four
  register blocks: with the striding version in place, `mlx_qp_scale` at 8
  lanes built all eight QPs, handed four of them registers at 0x1800-0x1e00,
  and left exactly those four spinning in `ibv_poll_cq` forever — the posts
  are accepted and never complete. `mlxBfRegOffset`/`mlxBfRegIndex` now
  confine registers to the UAR's first adapter page, with a host test naming
  the measurement. More registers per client have to come from allocating
  more UAR indices.
- A client's UAR sub-range is now exactly one host page. A 4 KiB sub-range mapped into
  a 16 KiB-page process necessarily covered three neighbouring UARs.
- `MlxHcaCaps::logUarPageSize` was never assigned and every consumer computed
  `uar4k ? 4096 : (1 << logUarPageSize)`, a one-byte stride the moment `uar_4k` went
  clear. Replaced by one `uarPageSize` in bytes, derived in `QueryHcaCaps` from the
  post-INIT_HCA readback — so a firmware refusal degrades to the old geometry instead
  of producing wrong doorbell addresses.
- `MLX_FAST_PATH_ABI_VERSION` 2 → 3; the shim no longer demands a 4 KiB UAR page.

Measured at 0.482. Firmware accepted the geometry: the driver reports
`uar_page=16384` with `regs_per_uar=4`, both derived from the post-INIT_HCA readback.
`P3_DIRECT_UAR PASS` against the Spark with `fallback_send=0 fallback_recv=0`.
`mlx_qp_scale "1 2 4 8" 64 5000`, four runs across 0.482 and 0.483:
1.93-1.97 / 3.41-3.68 / 4.10-4.97, against the recorded 1.95 / 3.58 / 4.19 — no
regression. `mlx_rtt_bench 64 20000`, five runs: p50 5.96-6.25 us, post 0.42-0.71,
blue flame on every post. P0.2 isolation, P0.3 lifetime, P1.1 quota, P2.2 ABI fuzz and
the runtime gate all PASS, re-run after the ABI growth.

The driver's log channel returns nothing for the DEXT process, so
`cache_line_128byte`, the board id and the raw `log_uar_page_sz` were not observable
at all. `mlx_query_limits_resp` now carries them (80 -> 104 bytes, ABI property
updated), the shim's `rdma_limits` mirrors them, and `mlx_limits` prints the firmware
readback. Confirmed at 0.483:

```
uar_page=16384 log_uar_page_sz=2 cache_line_128=1 board_id=MT_2430110027
```

so firmware accepted both the 16 KiB UAR geometry and the 128-byte cache line, and
QUERY_ADAPTER returns a real psid. The QueryLimits output size is checked exactly, so
a binary built against the 80-byte form now fails loudly instead of silently.

The Makefile did not list `Sources/userclient/MlxUCIO.h` as a prerequisite of
`mlx_isolation_gate` or `mlx_lifetime_gate`, so the ABI bump left both binaries stale
and the isolation gate reported a false `FAIL: resource creation` until it was
rebuilt. Both rules now carry the dependency.

## 2026-09-08 (later) — per-client UAR and doorbell pools

Two measured ceilings, both from a client owning exactly one UAR page and one
doorbell page. Blue-flame registers ran out at four QPs, and the 32 doorbell records
on that single page were shared by QPs and CQs, so 16 QPs plus 16 CQs exhausted a
client while the driver advertised 32 of each. Apple's DEXT grows its doorbell pages
from a linked directory (`allocDBPgDir`/`allocDB`, 64-bit occupancy word per page) and
treats a UAR as an ordinary allocated object (`allocUAR`/`allocMapUAR`); this is the
same idea with fixed bounds.

- `MlxClientDoorbellBundle` is now two pools: up to `MLX_CLIENT_MAX_UAR` (4) UAR pages
  and up to `MLX_CLIENT_MAX_DB_PAGES` (8) doorbell pages, each grown only when the
  current ones are full. A client that creates two QPs still costs exactly what it did.
- Blue-flame selection counts load across every UAR page the client holds, grows the
  pool by one page when every register is taken, and shares the least-loaded register
  only once the pool is full. Sixteen QPs now get sixteen private registers.
  Selection moved ahead of `CREATE_QP`, because the QPC names one `uar_page` and a
  register belonging to a different UAR is simply the wrong doorbell.
- Doorbell records: 32 per client shared by QPs and CQs becomes 256. `DbRecordLimit`
  follows the pool instead of the one device-global page, which was silently pinning
  a fast-path client's QP and CQ ceilings at 32.
- Offsets that cross the ABI (`bfOffset`, `dbRecordOffset`) are now flat across the
  pool: page index times page size plus the offset inside it. `mlxFlatOffset` /
  `mlxFlatPage` / `mlxFlatWithin` in `MlxUCIO.h` are shared by both sides so the
  arithmetic cannot drift, with host tests walking every slot and register.
  `MLX_FAST_PATH_ABI_VERSION` 3 -> 4; a v3 shim would read a flat offset as an in-page
  one and write a doorbell into the wrong page, so the version gate is the guard.
- `CopyClientMemoryForType` serves a UAR slot or doorbell page by index, and only ones
  the requesting client actually owns.
- The shim maps pool pages lazily and resolves each QP's blue-flame register and
  doorbell record once at creation, so the post path now dereferences one stored
  pointer instead of recomputing base plus offset.

Measured at 0.484. The pool does what it was built to do and does not move this
benchmark. `mlx_qp_scale` at 16 lanes gives every QP a private register: four UAR pages
(firmware indices 5, 6, 7, 8), four registers each, sixteen distinct flat offsets
0x800 / 0x4800 / 0x8800 / 0xc800 — exactly slot * 16384 + 0x800. Growth is visible
mid-run: the fifth QP of an eight-lane level lands on a freshly allocated second page.

Throughput does not follow. Aggregate operations per second peak at eight lanes and
fall monotonically after: 747k at 8, 602k at 10, 555k at 12, 542k at 14, 509k at 16,
with per-lane rate dropping roughly as 1/n and no cliff anywhere. Every lane busy-polls
its own CQ on its own thread, so past eight lanes this benchmark measures the host, not
the doorbell path — the blue-flame register count was not the binding constraint at
these lane counts. The 1/2/4/8 points are unchanged against the recorded baseline
(1.95 / 3.63 / 4.58 against 1.95 / 3.58 / 4.19).

`mlx_limits` still prints `db=32` because it never enables the fast path, so it has no
bundle and reads the device-global page; the pooled ceiling applies only to a client
that has one. `mlx_client_scale` builds 16 clients and refuses the 17th, identically
across repeated runs, so the pool releases everything it takes — that ceiling is a
pre-existing device resource limit, not a leak. `run_phase3_direct_uar_gate.sh` PASS
against the Spark, `mlx_rtt_bench` p50 6.04-6.12 us unchanged, and P0.2 isolation,
P0.3 lifetime, P1.1 quota, P2.2 ABI fuzz and the runtime gate all PASS.

## 2026-09-08 (later still) — command slots and a mailbox pool

The command plane ran every firmware command on slot 0 under one lock, so a
memory registration costing 1.3 to 31 ms stood in front of every QP transition,
GID write and CQ creation in the driver. It also built and tore down its mailbox
chain per command: an IODMACommand create, PrepareForDMA and Map, twice over, on top
of the firmware round trip. Apple's DEXT does neither — `AppleEthernetMLX5Cmd` keeps a
32-entry slot bitmap completed from the command EQ, and `allocCmdMsg` reuses a cached
message whenever its block count already covers the request.

- **Slot ring.** Commands take any free regular slot from a busy bitmap, up to
  `MLX_CMD_REG_SLOTS` (4). MANAGE_PAGES keeps the dedicated last hardware slot it has
  always needed, because firmware refuses it anywhere else with delivery status 6.
  This is the same model Linux runs (`max_reg_cmds` concurrent, last slot for pages),
  so the hardware side is not new ground. A queue with a single descriptor collapses
  to one shared slot, which is exactly the old behaviour.
- **Mailbox pool.** Each slot keeps its chain and reuses it whenever the next command
  needs no more blocks, so the DMA setup happens once per size class instead of once
  per command. A chain above `MLX_CMD_CACHE_MAX_BLOCKS` (64) is released on the slot's
  next smaller command rather than pinning 2.4 MiB for the life of the driver.
  Reused blocks are zeroed where a short reply or a short input tail could otherwise
  expose the previous command's bytes.
- Completion stays polled. Apple completes from a command EQ, which would need a third
  interrupt vector; this nub was granted two and keeps its first configuration for life
  (rdar://118153788), so that part is not portable here.
- A timed-out slot is never released, so nothing reuses a descriptor firmware may still
  own. The device is quarantined by then, which stops new commands at the door.
- `mlx_perf_resp` gains `fwCommandSlotWaits` (152 -> 160 bytes): how many commands
  found every regular slot busy. Zero on a serial workload; a rising count is the
  signal to raise the slot count. `mlx_rtt_bench --reg` prints it beside the sleep
  ratio.

Measured at 0.485. Bring-up itself is the deepest test the command plane has:
SET_HCA_CAP and QUERY_HCA_CAP move 4112 bytes through eight mailbox blocks, and the
card comes up, so slots and the pooled chain carry the large-command path.

Two `mlx_rtt_bench --reg 8` processes run concurrently both complete with no timeout
and no quarantine, and both report **0 slot waits** — the two clients each took their
own slot instead of serialising, which is what the ring is for. Registration cost about
doubles under that concurrency (4 KiB 265 -> 534 us, 96 MiB 19 -> 37 ms), as expected
when two clients share one card's firmware. Single-stream registration p50 is 265 us at
4 KiB, inside the 400 us spin window, so small commands no longer sleep at all.

`mlx_qp_scale` 1.92 / 3.65 / 4.32 and `mlx_client_scale` to 16 clients are unchanged
within their own run-to-run spread; the control path is not the bottleneck in either.
`run_phase3_direct_uar_gate.sh` PASS against the Spark, and P0.2 isolation, P0.3
lifetime, P1.1 quota, P2.2 ABI fuzz and the runtime gate all PASS.

Two things the measurement corrected:

- **The recorded deregistration figures are stale.** `verbs_compat` holds a userspace
  MR cache: `ibv_dereg_mr` drops a lease and the native registration lives until the
  context closes. Deregistration therefore costs 0.1 us and issues no firmware command,
  and a benchmark registering many distinct buffers exhausts the per-client mkey quota
  instead of recycling. The 1.18-1.75 ms dereg numbers in the 2026-09-07 note predate
  that cache and no longer describe this code.
- **`mlx_rtt_bench` was printing the sleep counter as a percentage of commands.** It
  counts every 1 ms IOSleep, so a 30 ms registration contributes 30, and the line read
  "144.3%" once large registrations entered the mix. It now prints milliseconds slept
  per command, and says both counters are device-wide.

## 2026-09-08 (last) — health buffer, async event decode, DCQCN counters

Three observability gaps, all of them things the driver already had the data for
and threw away.

**Health.** `MarkFatal` read two bytes of the init-segment health buffer and logged
them. It now decodes the whole thing — six assert words, the assert exit and
call-return pointers, timestamp, firmware version, hardware id, rfr_severity and
irisc_index — from mlx5's own `struct health_buffer` at BAR0 0x200, taken from the
kernel header rather than recalled. A syndrome number alone says an assert happened;
the assert words and the firmware version that produced them are what a report can be
matched against, and they are gone after the next reset. `mlx_health_resp` carries
them (32 -> 88 bytes) because this machine's kernel log channel is dead, and
`mlx_limits` prints them whenever any of them is set.

`Check` also learned Apple's device-removed test (`pollHealth`): a health counter
reading all-ones is confirmed against fw_rev, and when both are all-ones the card has
fallen off the bus. That is latched and reported. What was **not** taken from Apple is
the rest of `pollHealth`: it terminates the driver after three identical counter reads,
and on ConnectX-4 Lx that counter is a stable snapshot rather than a heartbeat, so the
same logic would kill a healthy card. The comment now says so.

**Async events.** `HandleEvent` decoded five types and dropped everything else with a
bare `default: break`. It now handles PORT_MODULE_EVENT with Apple's sub-type mapping
(`portModuleEvent` reads the same EQE byte and maps 1/2/3 to plugged/unplugged/error),
SRQ_CATAS_ERROR and CQ_ERROR, and logs the type and sub-type of anything else instead
of discarding it. The EQ subscribes to the three new types. A pulled or failing
transceiver used to appear only as a port going down with no reason attached.

**DCQCN.** `QUERY_CONG_STATUS` (0x822) and `QUERY_CONG_STATISTICS` (0x826) are wired
behind one new selector, layouts from `mlx5_ifc.h` on the Spark. Status says whether
firmware's loop is enabled for a priority; the counters are the reaction point
(cnp_handled, cnp_ignored, cur_flows, sum_flows), the notification point
(ecn_marked_roce_packets, cnp_sent) and the timestamp and accumulator period that let
two samples become a rate. The high/low halves are joined into one 64-bit counter each
rather than reported as halves nobody can add up. `mlx_cong_ctl stats [priority]
[clear]` prints them, and says explicitly when every counter is zero because nothing on
a directly attached pair marks ECN — which is the expected reading here, not a fault.

Measured at 0.487.

Health decodes to plausible firmware state on a healthy card:
`hw_id=0x0000020b` (ConnectX-4 Lx), `fw_ver=0xe01603ea`, `assert_exit=0x0085c020`
`callra=0x0085bfe4`, `irisc=4`, syndrome and rfr_severity zero. Correction to the note
above: the assert words stay zero until firmware asserts, but fw_ver, hw_id and the
assert pointers are populated all the time, so the detail block prints on a healthy
card as well — which is what makes it usable as a baseline.

DCQCN answers: priority 0 and priority 3 both report `enable=1`, so firmware's loop is
switched on, with `accumulators_period=1000000` and a timestamp that advanced between
two reads (1323461222 -> 1442391754) — the block is live firmware data, not a stale
copy, which is the check that the bit offsets are right. Every counter is zero, correct
for a directly attached pair where nothing marks ECN.

The extended event mask did not disturb the EQ: `mlx_irq_probe` reports both vectors
MSI-X, `async_irq` climbing, `setup_stage=0`, so CREATE_EQ accepted the three added
event types. `mlx_rtt_bench` p50 6.12 us and `mlx_qp_scale` 1.95 / 3.73 / 4.42 are
unchanged, `run_phase3_direct_uar_gate.sh` PASS against the Spark, and P0.2, P0.3,
P1.1, P2.2 and the runtime gate all PASS.

One defect in the tooling, found by it not printing: the health block was spliced into
`mlx_limits`'s error branch instead of its success path, so it only ever ran when the
limits query had already failed. Moved.

## 2026-09-08 (closing) — the Apple comparison is finished

`docs/DEEP-ANALYSIS.md` and `docs/MSI-X-ANALYSIS.md` were both assembled from a
changelog that was already a day stale, and stated as open several things that had been
closed: MSI-X delivery, the 73 us latency, blue flame. Both are rewritten.

DEEP-ANALYSIS is now a closed/open ledger rather than a plan. Everything Apple's DEXT
has that an RDMA driver can use has been read out of the decompilation, checked against
the kernel headers on the Spark, and either ported or rejected with a reason:

- **Ported:** `QUERY_ADAPTER` board id, `cache_line_128byte`, the 16 KiB UAR geometry,
  the growable doorbell-page directory, multiple UAR pages per client, the command slot
  ring, mailbox pooling, the health buffer decode with the device-removed check, async
  event sub-types, and `QUERY_CONG_STATUS` / `QUERY_CONG_STATISTICS`.
- **Rejected with a reason:** more blue-flame registers from a larger UAR page (measured
  false on this card); Apple's terminate-on-static-health-counter rule (that counter is
  a snapshot on CX-4 Lx and the rule would kill a healthy card); event-driven command
  completion (needs a third interrupt vector this nub cannot be given).
- **Deliberately unported:** the Ethernet flow-steering tree, which a verbs driver has
  no use for.

MSI-X-ANALYSIS keeps its SDK model, Apple reference and forum sections, and its
diagnosis section is marked superseded: the silence was not environmental, it was a
manual FLR bit write wiping the table `IOPCIMessagedInterruptController::initDevice` had
just filled, plus a wrong data value and both vectors landing on one handler.

The remaining work is no longer Apple-derived. The largest item is that the userspace MR
cache never evicts: `ibv_dereg_mr` drops a lease and the native registration lives until
the context closes, so a client registering many distinct buffers exhausts the
per-client mkey quota and starts refusing registrations. That is visible today in
`mlx_rtt_bench --reg`, which fails at its largest size every run.

## 2026-09-08 (after the review) — a leaked mkey on every repeat registration

Reading the code the rewritten DEEP-ANALYSIS pointed at turned up a defect worse than
the missing eviction it was written to describe. `ibv_reg_mr` **registered first and
consulted the cache afterwards**: on a hit it bumped the lease, returned the cached
lkey/rkey, and dropped the registration it had just made without deregistering it. One
leaked mkey per repeat registration of the same buffer, until the per-client quota
refused the next one with `0xe00002be`. The race path a few lines below already
released its redundant registration; the early path simply never did.

Fixed by looking the region up before registering it, which is also what makes the
cache a cache. The post-registration lookup is still there for the case where another
thread won the race, and now releases what this thread registered.

Measured with `mlx_rtt_bench --reg 8`, 48 registrations across six sizes:

| | before | after |
|---|---|---|
| firmware commands | 62 | 6 |
| 149 MiB region | refused every run | registers |

`mlx_rtt_bench --reg` also stopped being a registration benchmark once the cache
started working — every repetition after the first is a lease. It now prints the first
repetition beside the median, so the column means what its heading says: 340 us at
4 KiB, 1.8 ms at 1 MiB, 20.6 ms at 96 MiB, 32.9 ms at 149 MiB, against 0.0-0.2 us for a
cache hit.

Still open, and now the clearest next item: the cache has **no eviction**. Entries live
until `ibv_close_device`, so a client that registers many *distinct* buffers still walks
into the mkey and pinned-byte quotas. A lease count that reaches zero is the natural
trigger; the quota headroom the driver already reports is the natural budget.

## 2026-09-08 (after that) — the MR cache evicts

The other half of the same defect. Cached registrations lived until
`ibv_close_device`, so a client walking through *distinct* buffers still accumulated
mkeys and pinned bytes until the driver refused the next registration.

Entries with no leases are now kept for reuse and released when a new registration
needs the room, least recently used first. A hit moves its entry to the front, which is
what makes the order mean anything. Both ceilings come from the driver rather than
being invented in the shim: the mkey count from `QueryLimits` and the pinned-byte quota
from the runtime status, each taken at three quarters so the client keeps headroom for
the registration it is about to make. Deregistration of an evicted entry happens after
the cache lock is dropped, because it is a firmware command and holding a mutex across
it would serialise every other registration on the context.

Measured with a probe registering genuinely distinct buffers — the first version of it
was wrong, because `malloc` after `free` returns the same address and the cache keys on
the address, so it measured cache hits:

| workload | total | result |
|---|---|---|
| 80 x 16 MiB, byte budget | 1280 MiB against a 512 MiB quota | 80/80, mean 3.09 ms |
| 300 x 1 MiB, entry budget | 300 mkeys against a 128 limit | 300/300, mean 0.54 ms |

The mean costs are real registration costs, so nothing was served from cache; without
eviction both runs would have stopped at the quota. Reuse is unaffected:
`mlx_rtt_bench --reg 8` still issues 6 firmware commands for 48 registrations, with a
repeat costing 0.1-0.5 us against a first registration of 477 us at 4 KiB and 44 ms at
149 MiB. P0.2, P0.3, P1.1, P2.2 and the runtime gate all PASS.

This is a userspace change only; the driver does not need redeploying, but the rebuilt
`libibverbs.dylib` and `librdma_shim.dylib` do need to be in place.

## 2026-09-08 (rename) — com.mlx5.rdma.dext becomes com.melondma.rdma.dext

46 replacements across the Info.plist, the entitlements, the service-match header, the
loader, the packaging and takeover scripts and the tools, plus the documentation. The
loader app id and the two private entitlement keys moved with it
(`com.melondma.rdma.loader`, `com.melondma.rdma.entitlement`,
`com.melondma.rdma.diagnostic`).

Nothing external pinned the old name. There is no provisioning profile in the tree or
on this machine, so `com.apple.developer.driverkit.userclient-access` is granted by
plain codesign against whatever identifier the entitlement file names, and both sides
move together. The DEXT's own entitlements carry only the PCI match, no identifier.

**Two things this breaks until the driver is deployed**, both by design rather than by
accident:

1. `MLX_SERVICE_BUNDLE_ID` is an `IOPropertyMatch` on `CFBundleIdentifier`, so a
   rebuilt tool looks for the new identifier and will not find a driver still declaring
   the old one. Verified: `mlx_limits` rebuilt against the new name reports
   `no MlxPCIDriver service found` against the running 0.488. Tools and driver ship
   together from here.
2. The installed extension still carries the old identifier and matches the same card
   at the same probe score, so it has to be removed by name. `mlx_activate` now takes
   the identifier as an argument for exactly that, since a deactivation request must
   name what is actually installed and that is no longer this bundle's own id:
   `mlx_activate --deactivate com.mlx5.rdma.dext`. If sysextd refuses it because the
   installing app's identifier changed too, `systemextensionsctl uninstall 2B387C58L7
   com.mlx5.rdma.dext` is the fallback.

`make dext`, `make check-host` and the Swift loader all build; the stale
`build/MlxRDMA.app` and developer packages were removed so the old
`.systemextension` directory name cannot ship alongside the new one.

## 2026-09-08 (correction) — the kernel log channel was never dead

Several entries above justify moving values into query selectors with "the DEXT's log
channel is dead on this machine". That is wrong, and the mistake was mine. A DriverKit
DEXT's `IOLog` output is relayed by the **kernel** process, so the predicate is
`process == "kernel"` and the lines carry a
`(com.melondma.rdma.dext.systemextension)` prefix — which is exactly what
`mlx_dev.sh do_log` has always used. My queries filtered on the dext process and its
image path and therefore matched nothing. Worse, they ran bare `log`, which this shell
shadows with a function: `log show` there answers "too many arguments" and prints
nothing at all. `/usr/bin/log` works.

Read straight out of the driver's own log at 0.489, all three of the things that were
called unobservable:

```
MlxCmd: ready (rev=5, log_sz=5, stride=6, iova=0x80000000, reg_slots=4, pages_slot=31)
QUERY_HCA_CAP: ... uar4k=0 logUarPageSz=2 uarPageSize=16384 cacheLine128=1 ...
QUERY_ADAPTER: board id 'MT_2430110027' ieeeVendorId=0x0002c9
```

The command ring reports 32 hardware slots with 4 regular and slot 31 reserved for
MANAGE_PAGES, and `0x0002c9` is Mellanox's IEEE OUI.

The selector work stands on its own merits — a value in `mlx_limits` needs no log
parsing, no time window and no root — but it was argued for on a false premise, and the
premise is corrected here rather than quietly dropped.

**Use `/usr/bin/log show --last 10m --predicate 'process == "kernel"' --info | grep
MlxPCIDriver`.**

## 2026-09-08 (measured) — nine MSI-X vectors, of which the driver binds two

Asked whether this signed build can get vectors for MSI-X, and the answer overturns
something written into these documents several times.

`ConfigureInterrupts(kIOInterruptTypePCIMessagedX, 2, 9, 0)` asks for nine. Nine
indices probe as MSI-X (`probed=9/9`), and the kernel has programmed MSI-X table
entries 0 through 8 with the platform doorbell `0xfffff000` and controller-local data
values 1 through 9. Entry 9 and above are untouched.

The `vectors=2` this driver reports is `fIrqVectors = MLX_SINGLE_MSIX_VECTOR ? 1 : 2`,
a constant describing how many dispatch sources it chooses to create — async on host
index 0, completion on index 1 — with a comment in `MlxPCIDriver.cpp` saying exactly
that. It was read here as a platform grant and used to justify calling two Apple
mechanisms unportable:

- event-driven command completion (`compHandler`), and
- multiple completion EQs (`allocCompEQs`, vector base 3, depth 1024).

Neither is blocked. Seven allocated vectors are unused. What is not yet established is
whether `IOInterruptDispatchSource::Create` binds on index 2 and above — the table
entry is programmed and the index probes as MSI-X, which is all that is visible from
outside, but that is not proof. One dispatch source and a counter settle it, and that
experiment decides whether both items become doable.

Also added to `DEEP-ANALYSIS.md`: implementation plans for the four Apple facilities
still unported — per-vport queue counters (`out_of_buffer` and the RC error set), the
node/port/system-image GUIDs, the PCAM/MCAM/QCAM capability registers, and port and
link management including PFC. Offsets and register ids in the plans come from
`mlx5_ifc.h` and `driver.h` on the Spark.

## 2026-09-08 (probe) — does a spare MSI-X index actually bind?

Nine vectors are allocated and the kernel programmed nine table entries, but neither
fact says `IOInterruptDispatchSource::Create` will succeed on an index the driver does
not already use — and that is what decides whether Apple's event-driven command
completion and its set of completion EQs are available here at all.

`ProbeSpareInterruptSources()` runs once, after the two real sources are enabled. For
every probed index that reports MSI-X and is not the async or completion index, it
creates a dispatch source, records the `kern_return_t`, and cancels it again straight
away — release rides in the cancel completion block, because Cancel is asynchronous and
releasing immediately is a use-after-free, the same rule the health timer follows.
Nothing is left holding a vector.

The result is per index in `mlx_interrupts_resp.indexBind` (288 -> 352 bytes) and
`mlx_irq_probe` prints it as a `bind` column: `BINDS` for zero, the error code
otherwise, `-` for the two indices in use. The driver also logs one summary line,
`spare interrupt indices: N bound, M refused (of 9 probed)`.

Measured at 0.490: **7 bound, 0 refused of 9 probed.** Every spare index —
2 through 8 — yields a dispatch source, and the driver's own line says so:

```
spare interrupt indices: 7 bound, 0 refused (of 9 probed)
  idx 2..8   MSI-X   0x30000   BINDS
```

So both Apple mechanisms written off earlier as unportable are available:
event-driven command completion (`compHandler`) can have its own vector, and the set of
completion EQs (`allocCompEQs` on vector base 3) can have theirs. Nothing about the two
vectors this driver binds was ever a platform limit.

The probe leaves no residue: P0.2, P0.3, P1.1, P2.2 and the runtime gate all PASS after
it, `mlx_rtt_bench` p50 6.04 us and `mlx_qp_scale` 3.68 / 4.63 are unchanged.

## 2026-09-08 (answered) — a vector above 1 delivers

The unknown left by the bind probe is settled. Binding a dispatch source is not
delivery, and nothing had shown that firmware would raise a vector above 1 or that a
source on index 2 would fire when it did.

`ProbeCompletionVector` was the right instrument but had two faults. Its bound was
`intr > 1`, a leftover from believing two vectors were all the platform gave. Worse, it
rebound the firmware EQ and left the dispatch source on its old index — so the
interrupt would have arrived where nothing was listening, and the symptom is silence,
which is the very thing this diagnostic exists to explain. It would have answered its
own question wrongly. It now moves the source with the EQ, releasing the old one from
the cancel completion block, and its bound is what probed as messaged.

Measured at 0.491 with `mlx_blocking_rtt`, the only tool that arms a CQ and therefore
the only one that proves anything about interrupts:

| vector | completion_irq over 200 iterations | events/iter |
|---|---|---|
| 1 (control) | 0 -> 200 | 1.00 |
| 2 | 200 -> 400 | 1.00 |

Firmware accepted `CREATE_EQ` with `intr=2` and the source on host index 2 fired once
per iteration, exactly as vector 1 does. Blocking RTT p50 over three runs each: vector
2 gives 114.50 / 105.21 / 104.62 us, vector 1 gives 105.08 / 105.50 / 107.50 — the same
within the warm-up spread of the first run in each series, so a higher vector costs
nothing.

Both Apple mechanisms are therefore reachable in practice, not just in principle:
event-driven command completion on its own vector, and a set of completion EQs on
vectors 2 and up. The rebind returns cleanly to vector 1 and P0.2, P0.3, P1.1 and the
runtime gate all pass afterwards.

## 2026-09-08 (implementation) — multiple completion queues

Apple's `allocCompEQs` builds one completion EQ per vector from base 3 and spreads
completions across them. This is the same idea, kept **additive**: the primary EQ and
its dispatch source stay exactly where they were, so every teardown, quarantine, FLR
and re-init path that already handles them is untouched, and up to three extra vectors
are carried alongside. Rewriting the primary into an array would have meant editing
around seventy call sites in the two most failure-sensitive paths in the driver for no
behavioural gain.

- `BringUpExtraCompletionEqs()` runs after the primary is up — without it there is
  nothing to spread and the platform is not delivering at all. For each spare index
  that probed as MSI-X it creates a dispatch source, an action, an EQ with the matching
  `intr`, then publishes before enabling so an interrupt arriving immediately finds
  both. Every step may fail without consequence: an extra that does not come up is not
  counted and the driver runs on what it got, down to just the primary.
- One handler serves every completion vector and identifies its queue by the OSAction
  it was called with, so the primary's creation path is unchanged.
- The EQ timer drains and re-arms the extras too. An EQ nobody drains stops delivering
  once its ring fills, and the timer is the insurance against a missed interrupt on any
  of them.
- `TeardownExtraCompletionEqs(destroyFirmware)` is called at all five sites where the
  primary is released; the flag is false where the card is quarantined or gone and
  DESTROY_EQ must not be issued.
- `MlxCQ::CreateCQ` binds a new CQ round-robin across the queues.
- `mlx_interrupts_resp` gains `completionEqCount` and `completionIrqByEq[4]`
  (352 -> 392 bytes), and `mlx_irq_probe` prints a `COMP_EQ` line with the per-queue
  counts. `completionInterrupts` stays the sum: it answers "did an interrupt arrive",
  which is not the same question as "did the work spread", and the spreading is the
  entire point.

Measured at 0.492, and the first measurement found a defect in this very change.

All three extra queues came up — EQ 18, 19 and 20 on firmware vectors 2, 3 and 4 — and
`COMP_EQ` reported four queues. But the interrupts did not spread: a hundred-iteration
blocking run added **one** interrupt to its queue instead of a hundred, while blocking
RTT stayed a normal 114 us and `events/iter` stayed 1.00. Work was completing promptly
with almost no completion interrupts, which is the signature of a client that has
quietly stopped using the interrupt path.

Cause: the extra queues are created before `MlxRoCE` exists, so their `AddNotifier` at
bring-up is a no-op, and the late attach was wired into only one of the two places
where the primary gets its notifier. An EQ with no notifier still takes its interrupt
and still drains, but hands the completion to nobody — the CQ is never told, the driver
never re-arms it, and the client falls back to reading its own CQE ring, fast enough
that nothing looks broken. Fixed by attaching the extras wherever the primary is
attached.

This is exactly the failure the per-queue counters were added for: the sum said
"interrupts arrive" and would have been believed.

**Re-measured at 0.493 and the spread is exact.** One client of 100 iterations puts
exactly 100 interrupts on the one queue its CQ was routed to. Four sequential clients
rotate cleanly across queues 3, 0, 1, 2, a hundred each. Four concurrent clients of 200
gain 204 / 200 / 200 / 200 — one interrupt per completion on every queue, evenly
divided.

No regression: P0.2, P0.3, P1.1, P2.2 and the runtime gate all PASS, `mlx_rtt_bench`
p50 6.00 us, `mlx_qp_scale` 3.67 / 5.02.

## 2026-09-08 (implementation) — command completions were arriving all along

Step three turned out to be far smaller than planned, and the reason is a finding
rather than a design: **`MLX_EVENT_TYPE_CMD` has always been subscribed on the async
EQ.** Firmware has been reporting every command completion as an event since the EQ
existed. Nothing decoded it, so `HandleEvent` fell through to its default case — and
only because that case learned to log unknown types a few hours ago was it visible at
all: **4477 dropped command completions in twenty minutes.**

So no command EQ and no extra vector are needed. What was added:

- `MlxRoCE::HandleEvent` decodes `MLX_EVENT_TYPE_CMD`. The EQE carries
  `mlx5_eqe_cmd`, a 32-bit bitmap of completed command slots at the start of the event
  data — the same thing Apple's `compHandler` consumes.
- `MlxCmd::CompleteFromEvent(mask)` sets a per-slot done flag. The descriptor's
  ownership bit stays the authority for reading a result: the event says "finished",
  not "the outbox is yours".
- `WaitCommandEvent` sleeps on a reentrant dispatch queue and the handler wakes it,
  following the completion path's proven `DispatchSync` + `Sleep` shape rather than
  inventing one. The sleep keeps its one-millisecond timeout, so the command timeout
  still counts whole milliseconds and its meaning is unchanged; what changes is that a
  command finishing at 1.2 ms is seen at 1.2 rather than at 2.
- Polling stays the fallback and is taken whenever there is no event path — which is
  all of Start, before the queue and the EQ exist. Every command issued then behaves
  exactly as before.
- `StopInterrupts` wakes command sleepers, so none is left behind when the interrupt
  path goes away.

**It broke bring-up on the first deploy, and the cause was the timeout accounting.**
`QUERY_VPORT_STATE` timed out on slot 0 at 0.494, the device went into DMA quarantine,
and every call after it returned `kIOReturnNotReady` — the card came up owned but with
no usable port.

The wait loop counted its timeout in **iterations**, on the assumption that each pass
slept a millisecond. That held while the only wait was `IOSleep(1)`. The event wait
returns immediately whenever the completion generation has already moved, which another
command's completion does routinely, so the loop spun, charged itself a millisecond per
pass, and declared a five-second command timed out in under a second. Nothing about
firmware was wrong; the driver simply stopped waiting.

Fixed by measuring the timeout against the clock, which cannot be fooled by how — or
whether — the wait actually sleeps. `waited` survives only as the sleep statistic. The
done flag is also consumed rather than polled: an event names its slot slightly before
the descriptor's ownership bit is visible, and a flag left set would spin on that gap
for the whole timeout.

The lesson is narrow and worth keeping: a loop that counts iterations as time is
correct only for as long as every iteration takes that time, and adding a faster wait
is exactly the change that breaks it silently.

Verified at 0.495. Bring-up reaches `PHASE2_PREFLIGHT PASS`, the card is held, and the
unhandled-event line for type 0x0a is **gone** — command completions are decoded rather
than dropped. P0.2, P0.3, P1.1, P2.2 and the runtime gate pass, `P3_DIRECT_UAR PASS`
against the Spark, `mlx_rtt_bench` p50 5.96 us with post 0.38, `mlx_qp_scale` 3.69 /
4.82, and the completion spread still lands 200 interrupts on one queue for 200
iterations.

**No latency benefit is demonstrated, and the counter that would have shown one had to
be relabelled first.** `fw_command_sleeps` counts passes past the spin window; a pass
now waits *at most* a millisecond because it ends early on the event. It was
milliseconds while the only wait was `IOSleep(1)`, and reporting it as milliseconds
after the event wait landed would have been the same mistake as the timeout, twice in
one change. It now reads "waits past the spin window, each up to 1 ms" — 4.17 per
command over a registration benchmark. Whether the event wake shortens those waits in
practice needs a measurement this counter cannot make.

What the change is worth on its own terms: firmware's own report of which command
finished is no longer discarded, and the wait ends on that report instead of on a
timer tick.

## 2026-09-08 (Apple parity) — queue counters and identity GUIDs

Two of the four facilities still unported from Apple's DEXT, both read-only, both in
one pass because they touch the same files.

**Per-vport queue counters** (`vPortQueryQCounter`). Cheaper than the plan said: the
plan assumed `ALLOC_Q_COUNTER` plus writing a counter id into every QPC. Reading the
headers instead of the plan, `query_q_counter_in` carries `counter_set_id` at bit 0xf8
and a QP's `counter_set_id` in its QPC is zero unless something writes it — nothing
here does — so set 0 is already where this driver's traffic lands. One command, no
allocation, no change to QP creation. Sixteen counters are exposed, led by
**`out_of_buffer`**: packets dropped because nothing was posted to receive them, the
one number that names a starved receiver while every port counter stays clean. The rest
are the RC error set — out of sequence, packet sequence errors, RNR NAK retries, retries
exceeded, local ack timeouts, and the local and CQE error pairs.

**Identity GUIDs** (`queryNicVPortNodeGuid` and friends). No new command at all: the
driver already issues `QUERY_NIC_VPORT_CONTEXT` with a 272-byte reply to read
`roce_en`, and `system_image_guid`, `port_guid` and `node_guid` sit in that same reply
at bits 0x140, 0x180 and 0x1c0 of the context. They now reach `mlx_query_device_resp`
(48 -> 72 bytes), the shim, and `ibv_device_attr` as `node_guid` and `sys_image_guid` —
byte-swapped there, because rdma-core keeps them big-endian and a consumer comparing a
GUID against one read on Linux must get the same bytes. Before this, a consumer that
keys a connection on `node_guid` got nothing.

`mlx_limits` prints both. Verified at 0.496: the GUIDs read
`0x98039b0300806a94`, derived from the card's MAC and identical across node, port and
system image.

**The counter shortcut was wrong and firmware said so.** Reading counter set 0 answers
`fw_status=3` (BAD_PARAM), syndrome `0x1507c` — the opcode exists, the parameter does
not. Set 0 is not readable without allocating one, which is what the plan said before I
talked myself out of it on the strength of the QPC field defaulting to zero. Corrected
to the plan's shape: `ALLOC_Q_COUNTER` once at bring-up, the returned id written into
every QPC at `CreateQP` so traffic accumulates there, and the query aimed at that set.
A request for set 0 now means "the device's own set". Allocation failing is not fatal —
the counters are simply unavailable.

A second, smaller fault the same run exposed: the shim mapped both
`kIOReturnUnsupported` and `kIOReturnBadArgument` to `-ENOTSUP`, so a firmware
BAD_PARAM printed as "not supported by this DEXT" and pointed the search at the wrong
thing for a deploy. Only an absent selector is ENOTSUP now.

**And a third, which was the actual cause.** `ALLOC_Q_COUNTER` had been succeeding all
along; the driver read its answer at the wrong offset. `counter_set_id` sits at bit
0x58 of `alloc_q_counter_out`, after a 24-bit reserved field — the code read 0x40, the
top of that reserved field, and got a guaranteed zero. Zero was then taken to mean "no
counter set", so the query was refused before it was ever sent. Read at 0x58 now, and
allocation success is tracked in its own flag rather than inferred from a non-zero id:
a validly allocated set may legitimately be numbered zero, and inferring otherwise is
what hid the wrong offset for a deploy.

**Fourth attempt — and it broke the card.** With the set allocated
(id 1) and the query answering, every counter still read zero through 5000 loopback
RDMA writes and a peer exchange. `counter_set_id` is an **optional parameter of the QP
state transition** — `MLX5_QP_OPTPAR_COUNTER_SET_ID`, bit 25 in mlx5's `qp.h` — not a
plain QPC field. Written at `CREATE_QP` alone, firmware ignores it. Written into the RST2INIT
transition with that bit set, **firmware refused the transition outright**: every QP
failed RST->INIT, the card came up unable to create a working pair, and P0.2, P0.3,
P1.1, the runtime gate and the peer gate all failed at once. Reverted — RST->INIT
declares no optional parameters, which is what it did before and what works.

The bit is real; RST->INIT is not the transition that accepts it, and which one does has
not been established from anything better than a guess, which is exactly how the
previous three attempts went.

**Status of this feature, stated plainly: the queue counters do not work.**
`ALLOC_Q_COUNTER` returns set 1, `QUERY_Q_COUNTER` answers without error, and every
counter reads zero through 5000 loopback RDMA writes. Four hypotheses, four wrong. The
plumbing was verified end to end each time and still produced nothing, which is the
shape this failure keeps taking — every call succeeding is not evidence that the
feature works. What is still unchecked: whether a QP's QPC actually holds the counter
set after its transitions, which needs a `QUERY_QP` readback no tool can issue today,
and whether these vport counters register loopback traffic at all. The peer write gate
that would settle the second question fails at its RoCE-address readback before any
traffic runs, and that failure is its own question.

The GUIDs from the same pass are correct and verified: `0x98039b0300806a94`.

Recovery confirmed at 0.500: P0.2, P0.3, P1.1, P2.2 and the runtime gate all PASS,
`P3_DIRECT_UAR PASS` against the Spark, `mlx_rtt_bench` p50 6.00 us with post 0.42,
`mlx_qp_scale` 3.68 / 4.34, and the completion queues still take one interrupt per
completion. The only thing left behind by the four attempts is a counter set that is
allocated and readable and counts nothing.

Remaining from Apple: the PCAM/MCAM/QCAM capability registers and port and link
management including PFC. Neither fits one pass — the first is only worth doing if the
ACCESS_REG selector is gated on it, which risks refusing a register that works and can
only be checked by trying them all, and the second is mostly a write path that needs a
switched fabric this bench does not have.

## 2026-09-08 (Apple parity) — queue counters actually count

**`counter_set_id` belongs in the RST->INIT QPC body, not in the optpar mask.** The
queue counters had been allocated, bound and queried without a single failed command,
and read zero through every kind of traffic. `mlxEncodeRst2InitQpc` now writes the
counter set at bit 0x60 of the QPC with an empty optional-parameter mask.

Five attempts, four wrong. Set 0 is not readable without allocating; firmware answers
BAD_PARAM. The allocated id is at bit 0x58 of the reply, not 0x40. Writing the field at
CREATE_QP alone does not survive. Naming it in `MLX5_QP_OPTPAR_COUNTER_SET_ID` on
RST->INIT made firmware refuse the transition outright, and every QP on the card failed
to be created until that was reverted.

That refusal was the answer rather than a dead end. For this transition the field is not
optional, so it belongs in the QPC like p_key and port. And because the CREATE_QP value
did not survive, firmware applies the whole transition-relevant field set from the
supplied QPC, so a zero there overwrote what creation had chosen. The optpar bit is for
rebinding a live QP's counter from RTS, a different operation.

**The diagnostic that ended it.** `MlxQP::QueryQP` logs `counter_set_id` next to
`uar_page`. The neighbouring field is the point: a sane `uar_page` proves the QPC parse
is aligned, so a zero counter set beside it is the real value and not a misread. Live
QPs now log `state=1/2/3 counter_set_id=1 uar_page=4` across all three transitions. Four
rounds were spent guessing because every call kept succeeding, and a working call chain
was read as a working feature.

**Verified.** Four loopback RDMA writes and two loopback RDMA reads, one counter tick
each.

| After | Value |
|---|---|
| `rx_write_requests` | 4 |
| `rx_read_requests` | 2 |
| every error counter | 0 |

**How to read this set, proved on the wire.** Every counter in it belongs to the
receiving side. The phase-3 write gate then sent 1024 one-sided writes from the Mac to
the Spark at 19.77 Gbit/s. The Mac's `rx_write_requests` did not move, and the Spark's
climbed by exactly 1024. So a requester reading zero is correct behaviour, not a fault,
and both sides count the same events. Measuring the Mac's own set needs loopback on the
Mac, or a peer that writes into it.

| Counter | Before | After 1024 Mac to Spark writes |
|---|---|---|
| Mac `rx_write_requests` | 4 | 4 |
| Spark `rx_write_requests` | 344050 | 345074 |

Command layouts were re-checked against the Spark's own `mlx5_ifc.h` and all offsets
match: `counter_set_id` at 0x58 in the alloc reply and 0xf8 in the query request,
`clear` at 0xc0, `rx_write_requests` at 0x80, `out_of_buffer` at 0x180, and
`counter_set_id` at bit 0x60 of the QPC.

Of the four Apple facilities left unported, two now remain: the capability registers
PCAM/MCAM/QCAM, and port and link management including PFC.

## 2026-09-08 (Apple parity) — the userspace provider read against the code

Second donor: the decompilation of Apple's `libmlx5.dylib` data path in
`dev/donors/apple-rdma/RDMA-BUFFER-PATH.md`. Its five actionable points had been copied
into the analysis as open items. Checking them against the source closes three and
opens two, plus two facilities the donor names in passing that MelonDMA does not have.

**Closed, because they were already done.** The SQ doorbell record is published before
the doorbell on both paths, in Apple's order and at Apple's offset: the shim stores the
byte-swapped producer index to `db[1]` between two write barriers, and the DEXT does the
same at every post site. MelonDMA carries one barrier more than Apple. The inline length
unit matches too, `ds` being the low byte of `qpn_ds` in units of 16 bytes on both sides.
The MR cache, which the donor calls a MelonDMA advantage rather than a parity gap,
landed earlier the same day.

**Open, and both are correctness rather than performance.**

Nothing serialises two threads posting on one QP. Apple's receive path has an explicit
single-batch guard; MelonDMA's direct send and receive paths advance `sq_head`/`rq_head`
with no lock and no atomic, so two threads compute the same ring index and write the same
WQE slot. rdma-core's mlx5 takes a per-queue spinlock unless the QP is created
single-threaded, and MelonDMA ships a libibverbs compatibility layer presenting
rdma-core semantics.

Shared blue-flame registers are detected and then ignored. The allocator's own policy
ends "if the pool is full, share the least-loaded", it returns `MLX_QP_BF_SHARED`, and
the shim logs that concurrent posts serialise on the register. Nothing serialises on it;
the flag is set at creation and never read again. Two consequences: the toggle is
per-QP where it belongs to the register, so two sharers can write the same half twice in
a row and a write-combining mapping may merge the second doorbell away; and the 64-byte
blue-flame copy is unlocked, so interleaved posts hand the card a torn control segment.
Sharing begins at the seventeenth QP of one client, four UAR pages at four registers
each. `mlx_qp_scale` has only been run to eight, so this is an unguarded path rather
than a measured failure.

**Two unported facilities.** The HCA core clock page, which Apple maps alongside the UAR
and the rings: MelonDMA declares `timestamp_h`/`timestamp_l` in the CQE and discards
them, and does not read `device_frequency_khz` into `MlxHcaCaps` at all. Mapping it turns
a completion timestamp into a real time and gives one-way latency instead of a halved
round trip. And scatter to CQE, `cs_req`/`cs_res` in the QPC, which Apple's provider
supports and MelonDMA does not; it is the only item here that could move latency rather
than diagnostics, and also the least certain on a ConnectX-4 Lx.

## 2026-09-08 (implementation) — post-path serialisation and the HCA clock

Three of the four gaps the userspace-provider donor turned up are closed. The fourth is
deliberately left alone.

**Posts on one queue pair are serialised.** Every public posting entry point now
forwards to its body under the QP's own lock: batch and single send, multi-SGE send,
inline, atomic, local invalidate, and both receive paths. `rdma_post_send` and
`rdma_post_recv` are deliberately left unwrapped because they delegate to other entry
points, so a post takes the lock exactly once. On by default, matching rdma-core's mlx5
provider and the contract the libibverbs layer here advertises;
`MELONDMA_SERIALIZE_POST=0` opts out.

**Shared blue-flame registers carry the register's toggle under the register's lock.**
The shim keeps a per-device table of blue-flame registers, each with its own toggle and
its own lock. A QP the driver reported as sharing binds to that record; every other QP
keeps its private toggle and a null lock, so the private path costs one predicted branch
and its numbers are untouched. Sharing only happens between QPs of one client and a
client is one process, so the table needs no ABI change. This closes both halves of the
defect: a per-QP toggle that let two sharers write the same half twice in a row, which a
write-combining mapping may merge into one lost doorbell, and an unlocked 64-byte
blue-flame copy that could hand the card a torn control segment.

**The HCA internal timer is readable, with its frequency.** `device_frequency_khz` at
capability bit 0x4e0 now reaches `MlxHcaCaps`, which carried no notion of the card's tick
rate before. A new selector returns the 64-bit timer from init-segment offset 0x1000
together with the host clock sampled between the halves, retrying when the high half
moves because the two halves are separate registers and the low one can wrap. The CQE
already declared `timestamp_h` and `timestamp_l` and discarded them; they now have a
unit.

Read as a command, not as a mapped page, unlike Apple. On a 16 KiB host page the
smallest window containing the timer also contains the command doorbell and the
command-queue address, and exporting that to an untrusted client to save a syscall buys
nothing: correlating the two clocks is an occasional calibration, after which converting
a timestamp is arithmetic with no device access.

The offset came from compiling `offsetof` against the Spark's own `device.h` rather than
counting the struct by hand, which was worth doing: the health buffer is 64 bytes, not
the 76 a hand count gives.

`mlx_limits` takes two clock samples a known interval apart and checks the measured tick
rate against the rate firmware reports. A printed tick count would prove nothing.

**Measured on the card, DEXT 0.508.** The clock verifies itself: two samples a known
interval apart give a tick rate that matches the rate firmware reports, so this register
really is the clock behind the CQE timestamps.

| Reading | Value |
|---|---|
| Firmware `device_frequency_khz` | 156250 |
| Measured from two host-timed samples | 156249 |

The posting lock costs about 2% of round-trip time and nothing measurable on the post
itself. Three runs each of the blue-flame path, which fires on every post in this bench.

| | post p50 | RTT p50 |
|---|---|---|
| Lock on, the default | 0.42-0.46 us | 5.96-6.04 us |
| `MELONDMA_SERIALIZE_POST=0` | 0.42 us | 5.83-5.88 us |

One earlier reading needs correcting rather than explaining away. The bench has two
modes and they are not comparable: its default drives the blue-flame path and reports
post p50 near 0.42 us, while `--post` sweeps SGE counts without blue flame and reports
0.62-0.67. A 0.42 against a 0.67 from those two modes is not a regression, and the first
hypothesis here, that the shared-toggle indirection had cost 0.25 us, was wrong. The
private doorbell path was restructured anyway to hold no indirection and no lock at all,
which is the better shape regardless of what it measures.

Gates after the change: P3_LOCAL, P3_DIRECT_UAR, PHASE3_WRITE at 19.77 Gbit/s, loopback
write and read, and the queue counters still counting.

**Scatter to CQE, not attempted.** `cs_req` and `cs_res` exist in the QPC, but what to
write into them is internal to the kernel driver and to rdma-core's provider source,
neither of which is on this bench; the installed `mlx5dv.h` does not carry the values and
the Apple donor mentions the feature only in prose. Four rounds have already been lost in
this project to values recalled instead of read, so no remembered constant is going into
a QPC. The value is lower than it looks in any case: it is a responder-side optimisation,
and the headline path here is one-sided RDMA WRITE, which produces no responder
completion at all.

---

## 2026-09-09 — cross-machine UD and SRQ peer tests PASS (Spark)

Both §2.5 peer gates now run over the wire, not just loopback. Connection made through
`ssh spark` (mDNS `spark-a1a3.local`; the Spark's RoCE port is `rocep1s0f1`, GID
`192.168.200.2` on `mac-rdma-bond`). The standard rdma-core `ud_pingpong`/`srq_pingpong`
were built against the compat layer with the macOS patches now kept in
`tools/macos_endian_shim.h` and `tools/mlx_pingpong_roce_shim.c`.

Two real findings, both test-plumbing rather than driver:
- the Mac GID is per-client (not set at bring-up), so the client needs
  `ibv_mlx5_configure_roce` first; and `peer_mac` must be the Spark's real RoCE MAC
  (`4c:bb:47:7d:a1:a5`), not loopback — with loopback the RDMA frames went nowhere and the
  client hung with zero Spark `port_rcv_packets`.
- the pingpong handshake port is firewalled on the Spark, so the TCP exchange runs
  through an SSH `-L` tunnel.

Results: UD 1000 iterations ~88 us/iter both directions; SRQ (16 QPs on one SRQ) 1000
iterations, 29.8 us/iter Mac / 38.2 us/iter Spark; Spark `port_rcv_packets` advances,
confirming real wire traffic, and the stamped payload is byte-checked each round.




## 2026-09-08 (health) — syndrome names

A syndrome is now named as well as numbered. `MlxHealthSyndrome.hpp` transcribes the
table from `mlx5_ifc.h` (`MLX5_INITIAL_SEG_HEALTH_SYNDROME_*`) and `health.c`
(`hsynd_str`, Linux v5.9) — ground truth, not recall, per the rule that cost this
project several rounds elsewhere. `MlxHealth::MarkFatal` logs the name beside the
number, `mlx_limits` prints it, and host tests pin the table including the healthy
`0x0 = "none"` case that `hsynd_str` itself omits (it is only called on a nonzero
syndrome).

## 2026-09-08 (Apple parity) — PTYS/PFCC semantic decode

PTYS and PFCC are now decoded to semantic link/flow-control state, with ground truth
from `mlx5_ifc.h` and the legacy protocol table in `port.h` (Linux v5.9) — the same
headers the rest of the driver is checked against, not recall. `MlxPortRegs.hpp` holds
the field offsets and `mlxPtysSpeedMbps`, pinned by host tests against the live payloads.

**A proto-mask bug hid the whole decode.** The first PTYS read sent `proto_mask=1`
(`MLX5_PTYS_IB`) and came back with `eth_proto_*` all zero and `ib_proto_*` populated —
which read as an empty register until the mlx5_ifc.h layout and Apple's own
`queryPortProtoCap`/`updateCarrier` offsets were checked. The Ethernet view needs
`proto_mask=4` (`MLX5_PTYS_EN`). Fixed.

**Live result.** `eth_proto_oper = 0x40` = **40GBASE_CR4**: the network port is a 40
Gbit/s link, so the ~25–28 Gbit/s ceiling this bench measures is the PCIe Gen3 x4 tunnel,
not the network side. PFCC reads `pptx=pprx=1` (802.3x global pause on, both directions,
auto-negotiated) and `pfctx=pfcrx=0` (per-priority PFC off) — the fact that makes a
DCQCN measurement interpretable: ECN/DCQCN is the only congestion mechanism in play.

`mlx_query_port_resp` carries `linkSpeedMbps`, `pauseTx`, `pauseRx`, `pfcTxMask`,
`pfcRxMask` and `pfcPrioMask`; the shim mirrors them in `rdma_port_attr`, and
`mlx_port_link` prints the decode. `activeSpeed` stays the `QUERY_VPORT_STATE` value.
PFCC writes and PAOS/protocol changes remain deferred behind a switched-fabric test.

## 2026-09-08 (closing) — capability maps and UD teardown

The remaining read-only Apple parity work now has a stable surface and live verification.

**PCAM/MCAM/QCAM.** `MlxRoCE` reads PCAM `0x507f`, MCAM `0x907f` and QCAM `0x4019`
once during initialization with 80-byte payloads. Successful maps are cached in
`MlxHcaCaps`; a best-effort failure does not block RDMA bring-up. `QueryCapRegs` returns
version 1, a valid mask and all three raw maps. The shim exposes `rdma_query_cap_regs` and
`mlx_cap_regs`. Raw `ACCESS_REG` is now whitelisted: PPCNT, MPEIN and MPCNT remain valid;
PCAM/MCAM/QCAM are valid only after a successful bootstrap read; unknown IDs return
`kIOReturnUnsupported` before reaching firmware. Live result: `valid_mask=0x7`.

The maps are kept raw. Their semantic per-bit group names are not guessed without a
generation-specific IFC definition.

**UD teardown.** UD loopback now passes repeatedly on the active DEXT with no
`destroy_qp`, `destroy_cq` or `dealloc_pd` errors. The root cause was a trusted fast-path
shadow left inconsistent after the kernel-owned CQ consumed the UD receive: the DEXT
advanced the tails past the client snapshot, so `RefreshFastPathStateLocked` failed.
`DestroyQP` now flushes only that invalid-shadow case via `2RST_QP` and still refuses a
valid shadow with genuine in-flight WQEs (`kIOReturnBusy`), which is what `P0.3_LIFETIME`
checks. The speculative firmware `2ERR_QP`/retry fallback was removed — firmware never
refused `DESTROY_QP`; the `busy` was the driver's own shadow guard.

Live baseline: DEXT `0.515`, UAR 16 KiB, blue flame enabled, link up at 320, zero port
errors/discards/pause, and `UD_GATE PASS`. PFCC writes, PAOS and protocol changes remain
deferred because they need a switched-fabric test and can disrupt the only network path.



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
