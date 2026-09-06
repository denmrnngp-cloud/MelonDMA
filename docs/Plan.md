# MelonDMA Production RoCEv2 RC Plan

This plan is synchronized with the current development tree at `dev/` and with
the companion llama.cpp transport at `/Users/macstudio/llama.cpp/ggml/src/ggml-rpc`.
The public `git-prod/` snapshot is older and is not the source of truth for the
status below.

## Current Snapshot

New TTFT target (2026-09-05): [7–15% end-to-end target, code findings and acceptance](ttft-7-15-target-2026-09-05.md). This remains an unachieved target. [Opt-in KV batch/pipeline implementation and gates](kv-transfer-batch-pipeline-2026-09-05.md) now replace 80 synchronous requests with one batch and retain allocation ownership for cached destination MRs. This is application protocol work, not a new DEXT release. Concurrent releases reached 0.396; older snapshots below retain their historical scope.

- DEXT: `MlxRDMA.dext`, PCIDriverKit, `arm64e`.
- Bundle ID: `com.mlx5.rdma.dext`.
- PCI match: Mellanox/NVIDIA ConnectX-4 Lx `15b3:1015` (`0x101515b3`).
- Current source bundle version: `0.359`; source build tag: `p3-cq-arm-1`.
- ABI: `MLX_UC_ABI_VERSION=2` with additive feature negotiation.
- Provider path: `libibverbs_compat` + `librdma_shim` -> IOUserClient -> DEXT.
- Tested hardware: ConnectX-4 Lx on Apple silicon over a 40G RoCEv2 link to a
  Linux ConnectX peer. Capability-driven sizing is preparation, not support for
  other ConnectX generations.
- Consumer integration is outside this repository: llama.cpp contains the
  RDMA RPC transport and application mailbox/KV logic.

Status notation:

- `[x]` implemented in source and covered by a local or documented live check.
- `[~]` implemented or available but missing a current reproducible acceptance,
  or intentionally limited to a fallback/experimental mode.
- `[ ]` not implemented, not integrated, or not yet accepted for release.

A gate binary or a historical note is not by itself a current live PASS. A live
acceptance record must name the date, DEXT version/build tag, firmware, peer,
configuration and output.

## Implemented Provider

### Hardware and firmware

- [x] PCI discovery/matching, PCI ownership, BAR0/MMIO and PCI command handling.
- [x] ConnectX-4 HCA capability query and bounded capability-driven QP/CQ/MR
  table sizing.
- [x] Firmware command queue with status/syndrome validation and full 4096-byte
  DMA-page mailbox chains for large commands.
- [x] Firmware startup: `ENABLE_HCA`, `SET_ISSI`, `QUERY_PAGES`, `MANAGE_PAGES`,
  `SET_HCA_CAP`, `INIT_HCA` and capability readback.
- [x] Firmware page ownership states, batched GIVE/TAKE and quarantine for
  ambiguous ownership or command timeout.
- [x] Verified FLR and complete firmware reinitialization.
- [x] Fail-closed fatal/DMA handling: block new work, disable bus mastering,
  quarantine DMA and release quarantined objects only after a verified reset.

### RC verbs and ABI

- [x] PD, MR, indirect/KLM MR, CQ, QP, AH and Type-2 MW lifecycle.
- [x] Reverse dependency teardown, MR/MW dependency counts and busy refusal.
- [x] Pinned host memory with fragmented 4 KiB PAS/MTT support.
- [x] Direct MR limit of approximately 1.875 MiB per MKey; larger registrations
  are composed from bounded direct children under an indirect KLM MKey.
- [x] RC QP state machine `RESET -> INIT -> RTR -> RTS`, query and `ERR/RESET`.
- [x] RoCEv2 GID/MAC/VLAN configuration, IPv4/IPv6, UDP destination port and
  path-MTU validation.
- [x] SEND, RECV, RDMA READ/WRITE, SEND_WITH_IMM,
  RDMA_WRITE_WITH_IMM, LOCAL_INV and RC FETCH_ADD/CMP_SWAP.
- [x] FENCE, SOLICITED, SIGNALED, inline SEND up to 512 bytes and up to 16
  SGEs in the bounded SGE ABI.
- [x] CQE decoding with status, opcode, byte length, immediate data, atomic
  result, QP identity and vendor syndrome.
- [x] Async event interface, completion channels and hardware CQ arm request.
- [x] GID table enumeration/update and DCQCN query/modify parameter paths.
- [x] Versioned POD IOUserClient ABI with size, reserved-field, state, range,
  ownership and handle validation.
- [x] Opaque per-UserClient generation handles for PD/CQ/QP/MR/MW.
- [x] Per-client quotas, SQ/RQ depth limit 4096, batch limit 64, SGE limit 16,
  and firmware passthrough burst/window limiting.
- [x] Read-only health and per-client datapath statistics.

## Completed Performance Work

The following optimizations are implemented in `dev/src/dext` and/or the
companion llama.cpp transport. They are transport/provider functionality, not
proof that an end-to-end inference workload beats TCP40.

1. [x] Selective signaling for inline RPC: configurable
   `GGML_RPC_RDMA_SIGNAL_INTERVAL=1..8`; default interval 8. Inline payload is
   already copied into the WQE, so unsignaled WRs do not retain the source.
2. [x] One completion worker/channel per connection for the application path.
   RCQ uses the blocking event path; infrequent SCQ markers are polled
   opportunistically. The compatibility layer retains fallback behavior.
3. [x] Amortized trusted-QP shadow publication through the shared DB-record
   page. Publication is batched and forced at teardown, QP control transitions,
   diagnostics and exceptional paths. The seqlock writer uses CAS.
4. [x] Lazy CQ consumer DB publication. Software CI remains exact while the
   hardware DB record is published in batches and forced before arm/destroy or
   safety watermarks.
5. [x] Explicit single-owner CQ mode with owner-thread validation, scalar
   poll-one path and locked fallback. `ibv_mlx5_set_single_threaded()` is
   exposed by the compatibility layer.
6. [x] Dedicated completion EQ on MSI-X vector 1 in the DEXT, separate from the
   asynchronous EQ, and a blocking userspace completion wait released when the
   device generation advances. Polling remains the fallback. Note that on this
   machine the generation is advanced by the DEXT's EQ timer, not by an
   interrupt: MSI-X is configured and accepted on both sides but never
   delivered (see P3).
7. [x] One-sided KV transport in llama.cpp: persistent aligned staging/mailbox
   MR for the connection, linked 8-16 WR WRITE windows, per-window completion,
   final `RDMA_WRITE_WITH_IMM`, monotonic remote offset and receive token
   validation. The 16-slot, 4 KiB mailbox uses sequence-tagged writes and
   one-sided ACKs; SEND/RECV fallback remains available.
8. [x] Blue Flame path: capability-gated WC UAR, complete eligible inline WQE
   copy, ordered stores, alternating BF halves and ordinary-doorbell fallback.
   It is opt-in through `MELONDMA_BLUE_FLAME=1`.
9. [x] MTU negotiation and 4096-MTU support. The application exchanges path MTU
   and selects the minimum endpoint value; the gate rejects a requested MTU
   above the peer active MTU. 4096 has been exercised as an ablation, but the
   currently active Spark deployment value must be recorded per run.
10. [x] Receive recycle batching: initial receive chain and subsequent groups
    are reposted with bounded RQ doorbells; immediate-notification slots are
    recycled through the same path.
11. [x] Pre-encoded WQE templates and scalar poll-one conversion for common
    SEND/READ/WRITE forms, including signaling/fence/solicited variants.
12. [x] Small-message one-sided mailbox protocol in llama.cpp, with capability
    negotiation, disjoint RX/TX regions, sequence/geometry validation and an
    opt-out fallback.
13. [x] Apple Silicon coherent Metal/UMA MR. DEXT advertises the additive
    `MLX_UC_FEATURE_COHERENT_UMA_MR` capability by default. A shared/untracked
    `MTLBuffer` is registered through the ordinary MR path; GPU producer and
    consumer ordering is enforced around RDMA completion. The live Spark gate
    passed Metal→Spark, Spark→Metal and a 4 MiB indirect KLM MR with zero GPU
    mismatches. `.private` memory and GPU writes to PCIe MMIO remain unsupported.

Provider-side direct mixed SEND/WRITE/READ/inline/atomic batches and receive
chains are implemented. The application must use a linked WR chain to obtain
one doorbell for an application-level transfer.

## Local Verification

`dev/src/dext/Makefile` provides:

```sh
make -C src/dext check-host
make -C src/dext check-dext
make -C src/dext datapath-bench
```

- [x] `check-host` covers IFC/WQE encoders, shim/verbs smoke tests, service
  matching, ABI properties, indirect MKey encoding and key-index tests.
- [x] `check-dext` performs plist checks, IIG generation/dispatch checks and
  arm64e DriverKit syntax checks when the DriverKit SDK and `iig` are available.
- [x] Direct datapath, large-MR, P3-local, CQ-event and gate binaries are built
  by Makefile targets.
- [x] A clean full build/install and live Metal/RDMA gate were captured for DEXT
  `0.359` on 2026-09-04: both transfer directions and the 4 MiB indirect MR
  passed against Spark at MTU 4096. The standard bidirectional SEND/RECV
  regression also passed with zero direct-path fallback.

## P0: Correctness and Isolation

### P0.1 RC interoperability

- [x] Source and gate support covers SEND/RECV size matrices, immediate data,
  READ/WRITE directions, windows/signaling, RNR/retry, reconnect and lifecycle
  recreation.
- [x] The phase-2 gate contains the 1,000,000-message path and direct-path
  telemetry including fallback, doorbell, CQ and shadow counters.
- [x] Direct SQ/RQ/CQ path, direct-CQ decode, send-credit accounting and
  unsignaled completion retirement are implemented.
- [~] The 1M gate has historical PASS reports, including zero fallback in the
  optimized runs. DEXT `0.359` additionally passed the current standard
  bidirectional regression at MTU 4096 with zero direct-path fallback.

### P0.2 multi-client isolation

- [x] Two UserClients can own separate PD/QP/CQ/MR/MW sets; handles are checked
  against client ownership and generation.
- [x] The isolation gate covers cross-client denial, independent traffic and
  teardown of one client's resources while the other continues.
- [~] Current-version live artifact and exact DEXT build tag still need to be
  attached to the release record.

### P0.3 lifetime and stale handles

- [x] Generation tokens, stale-token denial, slot-reuse protection,
  deterministic double-destroy handling and in-flight QP busy refusal exist.
- [x] QP/MR/MW reverse dependency and cleanup rules are implemented.
- [~] Lifetime gate source and historical PASS exist; current `0.359` full
  matrix evidence remains to be captured.

## P1: Protection and Recovery

### P1.1 quotas

- [x] Per-client resource quotas, depth/batch/SGE limits and firmware-command
  rate limiting are implemented before firmware allocation.
- [x] Rejected requests roll back ownership and do not leave partial resources.
- [~] `run_p1_1_quota_gate.sh` is available; attach a current hardware PASS to
  close the release evidence.

### P1.2 crash and device removal

- [x] UserClient close releases owned resources in dependency order.
- [x] BAR-dead and device-fault paths refuse new posts and quarantine DMA/page
  mappings.
- [x] Reconnect scripts and stale-handle invalidation paths exist.
- [~] Physical Thunderbolt unplug, process death under active traffic and
  reconnect need current destructive-hardware acceptance, not just source review.

### P1.3 fatal events, fencing and FLR

- [x] Firmware health counter/watchdog and fatal event handling fence DMA and
  disable bus mastering.
- [x] `PerformFlr()` verifies FLR completion, device identity and firmware
  reload before clearing quarantine.
- [x] Firmware/EQ/UAR/provider state is rebuilt after verified reset.
- [~] Current live fatal-injection and post-FLR traffic evidence is still a
  release-test task.

### P1.4 cold boot and stable lifecycle

- [x] Cold takeover, hot update, driver-release, rematch and production
  preflight/package scripts exist.
- [x] `StableInitCycle` and the stable gate exercise no-FLR
  `TEARDOWN_HCA -> INIT_HCA`, page accounting and owner-state reporting.
- [x] Self-FLR recovery is the working fallback when firmware does not preserve
  RoCE vport state across no-FLR reinitialization.
- [~] No-FLR reinit is not accepted as universally stable until the target
  firmware passes vport `roce_en` readback after repeated cycles.
- [~] 10-100 lifecycle soak, suspend/resume and cold reboot are implemented as
  gates/scripts but require a current recorded run.
- [~] Boot-time flat-Dext staging is implemented on the development path, but
  the production package still has a release-note requirement to install and
  verify the flat `/Library/DriverExtensions` bundle automatically.

## P2: Observability and Security

### P2.1 observability

- [x] `QueryHealth` and `QueryStats` expose health, syndrome, owned-resource
  counts, opcode counters, CQ loss, retry/RNR errors and SQ/RQ occupancy.
- [x] Debug logging is compiled out of the release build with `MLX_DEBUG=0`;
  error/fatal records remain available.
- [x] `QueryPerf` fills every field it declares. `doorbells`, `cqeConsumed` and
  `cqeErrors` had been declared in the ABI and never written, so any per-request
  accounting that used them read zero. They now count kernel-mediated work only,
  which is what makes the direct path measurable: one loopback run reports 200
  doorbells and 200 CQEs through the kernel path and none through the direct
  one, with the userspace `direct_*` counters mirroring it (0.362).
- [x] `cqEvents` and `cqEventWakeups` report completion-event delivery and
  released waits. Both are device-wide: the compat layer blocks on its own
  UserClient connection, so a per-client count would always read zero from the
  connection that queries the counters.
- [x] `QueryInterrupts` (`ibv_mlx5_query_interrupts`) reports the interrupt
  path: granted vectors, which setup step failed and with what status, both EQ
  numbers, per-vector interrupt counts, the EQ timer period, and the completion
  EQ's own bring-up verdict including the firmware syndrome of each attempted
  variant. This exists because the kernel log channel is disabled on the
  development machine, so the driver's own `MLX_LOG` diagnosis is invisible and
  a client that finds `MLX_UC_FEATURE_CQ_INTERRUPT` missing could not otherwise
  tell a vector shortage from a dispatch-source failure.
- [x] Two gates cover the completion path: `mlx_cq_idle_cpu` measures what an
  idle armed channel costs on either worker path, and `mlx_irq_probe` rebinds
  the completion EQ to a chosen interrupt index (refused while any CQ is live)
  to separate a dead vector from a mismatched index.
- [~] CQ overflow accounting is not a complete hardware overflow decoder:
  `cqLost` covers unattributable CQEs, while the overflow-specific decode path
  remains incomplete.
- [~] The health monitor is functional for watchdog/fatal fencing but is not a
  full autonomous recovery supervisor.

### P2.2 ABI fuzz/property

- [x] Host ABI property tests cover struct layout, selectors, feature bits and
  bounds.
- [x] The ABI fuzz gate exercises malformed sizes, reserved fields, handles,
  states, batches, SGEs, MW binds, rkeys and integer-overflow cases.
- [~] A current 24/24 live result must be recorded with the `0.359` artifact.

### P2.3 endpoint authentication

- [ ] HMAC-SHA256, nonce/expiry/replay and GID/address/rkey binding are not part
  of the generic DEXT or the checked-in application transport implementation.
- [x] The client guide documents the recommended fail-closed authenticated
  control-channel scheme. Documentation is not an implementation acceptance.

## P3: Provider Extensions

- [x] Inline SEND up to 512 bytes and capability-gated inline ABI.
- [x] RC atomics with alignment/access validation and atomic-result CQE.
- [x] SL/VLAN fields and RoCEv2 GID table operations.
- [x] DCQCN `QUERY_CONG_PARAMS` / `MODIFY_CONG_PARAMS` firmware roundtrip.
- [x] Hardware CQ arm with `solicitedOnly`. The arm sequence number is read at
  arm time and advanced only when hardware delivers the event, matching
  rdma-core. Bumping it per arm put the CQ out of step with the hardware and
  only about a third of re-arms produced an event (fixed in 0.376).
- [x] Hardware completion moderation: `MODIFY_CQ` with `cq_period` and
  `cq_max_count`, reachable as `ibv_mlx5_modify_cq_moderation` and as
  `MELONDMA_CQ_MODERATION="<period_us>:<max_count>"` applied at CQ creation.
  Firmware accepts it; off by default because the right point is workload
  specific (0.378).
- [x] Dedicated completion EQ on its own MSI-X index. It had been failing with
  `CREATE_EQ` BAD_PARAM, which was read as a firmware limit; the real cause was
  ordering. `StartInterrupts()` ran before `PerformFlr()`, and an FLR clears the
  device's MSI-X capability, so the vectors were configured and then wiped and
  vector 1 fell outside the firmware's range. Configuring MSI-X after the FLR and
  `FwInit` makes the EQ succeed on the first attempt (0.382). A variant probe
  over ring size, UAR page and interrupt index stays in the driver as diagnosis.
- [!] Completion events are **not** delivered by MSI-X on this machine. With the
  ordering fixed and firmware accepting both interrupt indices, the driver's own
  counters stay at zero through real traffic (32 arms, 64 CQEs:
  `async_irq=0 completion_irq=0`), and rebinding the completion EQ to index 0
  with `mlx_irq_probe` leaves them at zero too, so it is not an index mismatch.
  What delivers completions is the EQ timer, which polls both EQs and releases
  blocked waiters. The likely cause is outside the driver: the card is claimed
  through IOCatalogue injection rather than a normal match. Treat the timer
  period as the latency floor on this platform.
- [x] EQ timer rate is three-tier: 10 ms while the interrupt path is unproven
  and a CQ exists, 100 ms when no CQ is allocated, 50 ms once any vector has
  proven itself. Idle cost with no CQ fell from 0.750 % to 0.150 % of a core
  (0.388).
- [x] Blocking completion delivery through `WaitCqEvent`, with the mapped-ring
  poller as the alternative. Measured on an idle armed channel: 0.190 % of a
  core against 2.950 %.
- [x] BF capability negotiation and opt-in full inline-WQE injection.
- [~] P3 local/peer gates and historical live reports exist; rerun against the
  current active DEXT before calling the release gate closed.
- [ ] PFC and congestion statistics require switch/fabric validation and are
  outside the provider-only acceptance.

## Inference Integration Status

The companion llama.cpp files implement the application features described in
this plan: selective signaling, one completion worker, adaptive completion
backoff, direct UAR/CQ operation, persistent staging/mailbox buffers, chained
one-sided WRITE, mailbox sequence/ACK protocol and MTU negotiation.

- [x] Consumer source contains the one-sided WRITE pipeline: bounded linked
  windows, per-window signaled completion and final WRITE_WITH_IMM.
- [x] Consumer source contains the 16-slot x 4 KiB mailbox with capability
  negotiation and SEND/RECV fallback.
- [x] Consumer source contains direct-CQ/direct-UAR and BF feature switches,
  single-owner setup and signal interval configuration.
- [x] DEXT `0.359` exposes Apple Silicon coherent UMA registration through
  `MLX_UC_FEATURE_COHERENT_UMA_MR` by default. An application can register
  `MTLBuffer.contents` from a `.shared | .untracked` buffer as an ordinary MR;
  the live Mac Studio↔Spark gate verified GPU→Spark, Spark→GPU and a 4 MiB
  indirect KLM MR with zero GPU mismatches. This is the supported Apple
  GPUDirect-like method; it does not make `.private` memory or PCIe MMIO
  accessible to Metal.
- [x] The llama inference profile enables the supported RDMA assists together:
  `MELONDMA_DIRECT_CQ=1`, `MELONDMA_DIRECT_UAR=1`,
  `MELONDMA_BLUE_FLAME=1`, `MELONDMA_HW_CQ_EVENT=1`,
  `GGML_RPC_RDMA_WRITE_KV=1` and mailbox on by default. Because llama's
  checkpoint/lease path can poll the same QP from a second thread,
  `MELONDMA_SINGLE_THREADED=0` is required for the shared-RPC profile; the
  single-owner CQ optimization remains available for genuinely single-threaded
  clients and gates.
- [~] Full-profile llama smoke passed on 2026-09-04 with mailbox on, Blue Flame,
  hardware CQ events and one-sided KV enabled: MTU 4096, Mac/Spark
  `RDMA_STATS` errors=0, and a successful 8-token inference request. This
  proves activation and compatibility, not yet a final-KV-layout or workload
  p95/CPU acceptance.
- [x] Transport and RPC builds have been reported clean in the development
  notes.
- [~] Driver support for direct writes into Metal memory is verified, but llama
  still registers the request destination `cmd.dest`, not a proven final
  `ctx_tgt`/KV `MTLBuffer`. Spark also extracts each source range with
  `ggml_backend_tensor_get()` into a temporary chunk before `rdma_write()`.
  Final-layout addressing and removal of these application copies remain open;
  the provider deliberately does not own tensor layout or KV semantics.
- [~] RPC mailbox compatibility is negotiated; old binaries without the mailbox
  capability use fallback, but mixed-version deployment must be tested before
  release.

## Measured Performance Evidence

The checked-in development reports contain these results:

- [x] 35B split decode improved from approximately 39.9 to 46.54 tok/s in one
  optimized checkpoint, close to the stored TCP40 value of 46.61 tok/s.
- [x] 27B disagg at 65K reached approximately 562.45 prefill tok/s and 133.97 s
  TTFT in one optimized checkpoint versus stored TCP40 550.1 and 136.95 s.
- [x] Blocking completion delivery reduced reported Mac busy-poll CPU roughly
  5x in the 2026-09-03 sweep, at the cost of a small wake-up/prefill tradeoff.
- [x] Transport gates reported optimized 1M exchanges without provider fallback,
  128 x 1 MiB one-sided WRITE at approximately 19.96 Gbit/s, and mailbox wrap
  coverage of 1024 bidirectional messages in 64 ring wraps.
- [~] These are dated development measurements, not a statistically complete
  release benchmark. Some reports use different harnesses, contexts, MTU and
  DEXT versions; do not combine them as one baseline.

### 2026-09-04 RDMA inference sweep

- [x] Fresh RDMA sweep completed for both mailbox states:
  `bench_rdma_mailbox_on_2026-09-04.csv` and
  `bench_rdma_mailbox_off_2026-09-04.csv`; 80 request rows per file, zero
  request errors, contexts 512 through 65536, both models and both modes.
- [x] The sweep used negotiated QP MTU 4096 on both endpoints. Spark reported
  `active_mtu=4096`; each active connection logged `mtu=4096` and
  `errors=0` in `GGML_RDMA_STATS`.
- [x] Compared with the stored `bench_tcp40g_results.csv`, RDMA mailbox on
  improved 35B split decode at ctx 512/1024 from 46.77/46.61 to
  52.90/52.03 tok/s. Mailbox off was 52.80/50.90 tok/s. At 27B disagg
  ctx 65536, mailbox on measured 561.32 prefill tok/s and 134.234 s TTFT
  versus TCP40 550.12 and 136.946 s. These are three-repetition medians where
  available; 65K has one sample, so its p95 is not statistically meaningful.
- [x] The sweep now records per-request `itl_p95_s`, `itl_mean_s` and
  `cpu_s_request` (Mac process plus Spark RPC-server process), in addition to
  TTFT, decode tok/s, prefill tok/s and process CPU percentages.
- [~] The full llama profile smoke also passed with mailbox, Blue Flame,
  hardware CQ events and `GGML_RPC_RDMA_WRITE_KV=1`; final-KV-layout placement,
  workload-level CQ-interrupt benefit and total CPU including DEXT/
  `kernel_task`/interrupts remain separate acceptance measurements.
- [x] Single-owner CQ was changed from unconditional to opt-in. Shared llama
  RPC connections use the compat layer poll lock because the lease/checkpoint
  path uses the same QP from more than one thread. This fixes the observed
  `GET_TENSOR_BATCH` crash without removing single-owner support for compatible
  clients.

- [x] Full llama RDMA profile sweep completed on 2026-09-04 with all supported
  assists enabled simultaneously: direct UAR/CQ, mailbox on,
  `GGML_RPC_RDMA_WRITE_KV=1`, `MELONDMA_BLUE_FLAME=1`,
  `MELONDMA_HW_CQ_EVENT=1`, negotiated MTU 4096 and lease-safe
  `MELONDMA_SINGLE_THREADED=0`. The run produced 80/80 successful request rows
  across both models, both modes and contexts 512..65536; all recorded
  `GGML_RDMA_STATS` entries reported `errors=0`.
- [x] Against stored TCP40, the all-optimization RDMA run improved 35B split
  decode from 46.77 to 52.89 tok/s at ctx 512 and from 46.61 to 51.44 tok/s
  at ctx 1024. For 27B disagg, TTFT was 25.9669 vs 26.1112 s at 16K,
  56.7173 vs 56.8620 s at 32K and 136.7369 vs 136.9455 s at 65K. These
  values meet the requested parity direction, but 65K has one sample.
- [x] The benchmark records median/p95-over-repetitions TTFT, decode tok/s,
  per-request inter-token p95 and process CPU-seconds/request. The CPU value
  currently covers the Mac llama process plus Spark RPC-server process; DEXT,
  `kernel_task` and interrupt CPU are not attributable per request yet.
- [~] `GGML_RPC_RDMA_WRITE_KV=1` is live-smoke verified as a one-sided WRITE into
  the registered llama destination buffer. The final internal KV-layout path
  still needs explicit workload verification; enabling the flag alone does not
  prove that arbitrary llama KV storage is directly registered and addressed.
- [~] Hardware CQ event support was enabled and the inference smoke/sweep passed,
  but interrupt-vs-poll CPU and latency benefit still need an isolated workload
  comparison.

### 2026-09-05 optimized sweep attempt

- [x] Rebuilt Mac `llama-server` with `GGML_RPC_RDMA=ON`, `GGML_METAL=ON`, Release mode and the DEXT UserClient entitlement.
- [x] Rebuilt and synchronized Spark `ggml-rpc-server` from the same llama RPC sources; Spark uses `rocep1s0f1/GID3`, Mac uses `mlx5_0/GID0`.
- [x] Benchmark harness now uses the RDMA binary explicitly, sets `GGML_RPC_REQUIRE_RDMA=1`, adds MelonDMA runtime libraries, and refuses to record a TCP fallback as RDMA.
- [x] Mailbox-on RDMA activation passed and both sides logged `RDMA connection active`, MTU 4096, zero transport errors.
- [x] Fixed llama transport CQ batch bookkeeping: pending CQEs are separated between `scq` and `rcq`; a shared pending queue could return a send WC to a receive waiter and break RPC ordering.
- [~] Partial optimized sweep: `/Users/macstudio/llama.cpp/bench_rdma_all_optimizations_2026-09-05.csv`, 34 successful rows, zero reported errors, stopped by the execution timeout at 27B split ctx 8192.
- [!] The partial run is not an acceptance result: 27B split decode measured about 6.7--7.1 tok/s, versus about 14--15 tok/s in the 2026-09-04 reference. The same regression remained with batch aggregation disabled, so it is not attributable to `RPC_CMD_BATCH_SEND`. Investigate current split runtime/model configuration before comparing optimization deltas.
- [x] Harness bug fixed: `GGML_RPC_RDMA_ENABLE_BATCH` and `GGML_RPC_RDMA_DISABLE_BATCH` are now mutually exclusive on Spark and Mac.

### End-to-end TCP40 acceptance still open

- [ ] Randomized/interleaved TCP40 and RDMA comparison with at least 5-10
  repetitions, median, p95 and confidence interval.
- [ ] Split ctx 512/1024: RDMA decode at least 5% above TCP40 with no p95 RPC
  latency regression.
- [ ] Disagg 16K/32K/65K: prefilling and TTFT within 1% of TCP40 and separately
  measured KV handoff at least 15% faster.
- [ ] CPU-seconds/request for application, DEXT, `kernel_task`/interrupts and
  peer RPC/network work no higher than TCP40.
- [ ] Full ablation matrix: signal interval, BF, hardware CQ interrupt, mailbox,
  direct CQ/UAR, adaptive backoff and TCP40 under the same clocks, thermals,
  prompt, model placement and run order.

## Remaining Engineering Work

- [ ] Integrate the verified coherent UMA MR method with the final Metal KV
  allocation: persistent region/rkey publication, direct offset mapping into
  `ctx_tgt`, one terminal WRITE_WITH_IMM, and no post-transfer `set(local)` copy.
  The current llama Metal backend already makes this feasible: on unified-memory
  devices its default buffer type uses page-aligned `vm_allocate` storage and
  `newBufferWithBytesNoCopy(...MTLResourceStorageModeShared)`. Register the
  stable `ggml_backend_buffer_get_base()` region; do not build another staging
  allocator. Prefer a dedicated RDMA-compatible untracked buffer policy instead
  of changing hazard tracking globally.
- [ ] Remove or overlap the Spark-side `ggml_backend_tensor_get()` temporary
  chunk. Use a directly addressable registered source when the backend permits;
  otherwise use a persistent pinned double buffer and record the unavoidable
  GPU→host copy separately.
- [ ] Aggregate independent split-decode activation messages before the mailbox
  post. Existing WR-chain support batches chunks of one byte stream, but the
  RPC layer still emits one WRITE_WITH_IMM per logical mailbox item.
- [ ] Add an active-direct/idle-event completion policy and exact counters for
  ExternalMethod calls, CQ polls, doorbells, wakeups and bytes copied per
  inference request before changing fast-path defaults.
- [ ] Current-version full P0/P1/P2/P3 hardware gate bundle with zero CQE errors,
  no resource drift, no unexpected fallback and no unrecovered FLR.
- [ ] Complete CQ overflow decoder and expand health monitoring/recovery policy.
- [ ] Validate destructive removal, repeated no-FLR firmware cycles, suspend/
  resume and cold boot on the target firmware.
- [ ] Decide and document whether the production bundle ID remains
  `com.mlx5.rdma.dext` or changes to the separately mentioned
  `com.melondma.rdma.dext`; update every plist, loader, entitlement and script
  consistently if it changes.
- [ ] Keep the production package synchronized with flat-Dext boot staging.
- [ ] Validate clean SIP-on install, activation, update, deactivation and
  uninstall on a clean machine.
- [ ] Obtain/grant Apple PCI and per-client UserClient entitlements, Developer ID
  signing, provisioning profiles and notarization.
- [ ] Add separate backend, capability validation and live gates before claiming
  support for CX5/CX6/CX7/CX8.

## Explicitly Deferred

- UD, DC, XRC, SRQ, multicast and raw Ethernet QPs.
- InfiniBand link layer and software RoCE.
- Automatic NetworkingDriverKit `enX`, route, ARP/NDP and endpoint discovery;
  current operation uses explicit GID/MAC and peer configuration.
- NVIDIA-style GPUDirect RDMA, CUDA peer-memory and IBGDA on Apple M2. The
  supported Apple alternative is already implemented: NIC DMA into registered
  `.shared` Metal/UMA memory. GPU-issued PCIe UAR doorbells and `.private`
  Metal-memory registration remain unavailable.
- Provider-owned tensor, GGUF, KV-cache, RPC or collective semantics.
- `rdma-ndd`; it is a Linux identity helper, not a macOS datapath dependency.
- Adding multiple QPs to one 40G flow without a measured benefit.

## Closure Order

1. Keep `check-host` and `check-dext` green for every provider change.
2. Rebuild and identify the active DEXT by version and `MlxBuildTag`.
3. Run current-version correctness, isolation, lifetime, quota, soak, ABI-fuzz,
   P3, direct-path, interrupt, BF, MTU and mailbox gates.
4. Run the controlled TCP40/RDMA inference benchmark and record p95 and total
   CPU-seconds, not only throughput.
5. Resolve firmware no-FLR behavior and destructive recovery tests.
6. Complete flat-Dext boot packaging, Apple entitlements, signing, notarization
   and SIP-on clean-machine lifecycle validation.
7. Only then mark the production release gate complete.
