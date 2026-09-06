# Changelog

MelonDMA is a macOS **DriverKit (DEXT)** RoCEv2 RC provider for Mellanox/NVIDIA
ConnectX, with a libibverbs-compatible userspace layer (`libibverbs_compat` +
`librdma_shim`). The project is pre-1.0, so this file describes the current
tree as a single snapshot — **done**, **known gaps**, and **measured
performance** — rather than dated releases.

## Hardware behind every result

- Mac Studio M2 Ultra
- Mellanox **ConnectX-4 Lx EN `MCX4131A-BCAT`**, PCI `15b3:1015` (subsystem `15b3:0005`)
- QSFP+ 40G DAC on an **ADT-Link PCIe Gen3 adapter** board
- Peer: NVIDIA DGX Spark (ConnectX-7), stock rdma-core (`ibv_rc_pingpong`)

The card supports PCIe **Gen3 x8**, but the ADT-Link Gen3 adapter negotiates
**PCIe Gen3 x4** (8 GT/s × 4 on the Mac side). Every bandwidth number below is
capped by that Gen3 x4 link (~31.5 Gbit/s theoretical).

---

## Done — live-verified on hardware

### Driver & firmware bring-up
- PCIDriverKit DEXT takes PCI ownership from Apple's `AppleEthernetMLX5` at
  runtime (dev configuration), maps BAR0, reads firmware `0x0016000e` / 14.22.2560.
- Full firmware lifecycle: `ENABLE_HCA → SET_ISSI → QUERY/GIVE pages (6 boot,
  4465 init, 3332 runtime) → SET_HCA_CAP → INIT_HCA → QUERY_HCA_CAP`.
- Self-triggered FLR resets firmware to a clean state without a power cycle.

### Verbs / datapath
- RoCEv2 RC QP lifecycle `RESET → INIT → RTR → RTS` with `QUERY_QP` verification;
  `ERR/RESET` path.
- PD, MR, CQ, QP, AH, and Type-2 MW (`CREATE_MKEY`, `BIND_MW` UMR, `LOCAL_INV`,
  rkey rotation, stale-rkey rejection).
- SEND/RECV, RDMA READ/WRITE, immediate data, FENCE/SOLICITED, multi-SGE.
- Pinned host MR with fragmented 4 KiB PAS/MTT; GID program/readback, MTU,
  UDP destination port 4791.
- Versioned `IOUserClient` ABI + feature negotiation; `libibverbs_compat` +
  `librdma_shim` userspace stack.

### Production gates — all PASS on an active DEXT
- **P0.1 PASS** — 1,000,000 bidirectional SEND/RECV (1/64/256/4096 B), 10 recreate
  cycles, forward/reverse READ + WRITE, hardware RNR/retry, recovery + reconnect.
- **P0.2 PASS** — multi-client isolation (ownership denial, concurrent traffic,
  client-A teardown without affecting client B).
- **P0.3 PASS** — lifetime/stale-handle hardening (generation tokens, in-flight
  busy, deterministic double-destroy).
- **P1.1 PASS** — per-client quotas and DoS limits.
- **P2.1** — `QueryStats` observability (per-opcode / posted / completed / error /
  occupancy counters).
- **P2.2 PASS** — ABI fuzz/property gate: 24/24 refusals with exact `kIOReturn*`.
- **P2.3** — authenticated control channel (HMAC-SHA256, nonce/expiry/replay,
  GID/rkey/addr binding).
- **P3 PASS** — inline SEND (`max_inline_data=512`), RC atomics (FETCH_ADD /
  CMP_SWAP with `atomic_result` verified), SL=3, `solicited_only` CQ arming,
  GID-table enumeration, DCQCN `QUERY/MODIFY_CONG_PARAMS` roundtrip.

### Performance work — applied
- Removed unconditional `IOLog` from the hot path (5×/CQE + 3×/SEND); verbose
  logs now compile out at `MLX_DEBUG=0`.
- O(1) lkey→slot hash index in `MlxMR` (was an O(512) scan per SGE).
- Narrowed `fMethodLock`; per-QP/per-CQ locks + O(1) QPN/CQN index.
- CQ-depth validation; capability-driven QP/CQ/MR table sizing
  (`min(firmware caps, 4096)`); `MLX_UC_MAX_SGE` raised 4 → 16.
- Blocking completion delivery: a client blocks in the DEXT until the
  completion generation advances instead of scanning the mapped CQE ring on a
  timer. An idle armed channel costs 0.190 % of a core against 2.950 %.
  `MELONDMA_HW_CQ_EVENT=0` keeps the poller; `MELONDMA_HW_WAIT_MS` bounds what a
  missed event costs (default 50 ms).
- Hardware completion moderation (`MODIFY_CQ` with `cq_period` /
  `cq_max_count`), as `ibv_mlx5_modify_cq_moderation` and as
  `MELONDMA_CQ_MODERATION="<period_us>:<max_count>"`. Off by default.
- EQ timer rate is three-tier — 100 ms with no CQ allocated, 10 ms with one,
  50 ms once any interrupt vector proves itself. Idle DEXT cost fell from
  0.750 % to 0.150 % of a core.
- Four completion-path defects fixed: the CQ arm sequence number was advanced
  per arm instead of per delivered event (only ~1/3 of re-arms produced one);
  `MlxEQ::Poll` published the consumer index without the arm bit, so a timer
  drain left the ring disarmed; the compat worker blocked on an already
  disarmed CQ and consumed the next generation edge; and a wait timeout arrived
  with a zeroed output struct, losing the client's generation snapshot.
- `QueryPerf` fills the `doorbells` / `cqeConsumed` / `cqeErrors` fields it had
  always declared and never written, plus device-wide completion-event and
  wakeup counters. `QueryInterrupts` reports the whole interrupt path: granted
  vectors, failing setup step, both EQ numbers, per-vector interrupt counts, the
  EQ timer period, and the completion EQ's bring-up verdict with per-variant
  firmware syndromes.
- Gates added: `mlx_cq_idle_cpu` (idle cost of an armed channel on either
  worker path) and `mlx_irq_probe` (rebind the completion EQ to a chosen
  interrupt index).
- `PortStats` and `AccessReg` implemented. Both selectors had been declared in
  the ABI header and in the privilege check for a long time with no method table
  entry and no handler. `PortStats` reads PPCNT groups 0 and 1 (packets, bytes,
  errors, discards, pause frames) plus link state; `AccessReg` is a
  diagnostics-only ACCESS_REG passthrough, its payload raised 256 → 512 bytes so
  PPCNT's 264 fits. On a host where the DEXT owns the port and no netif exists,
  this is the only receive-side view of the wire.
- `tools/mlx_port_counters` reads them: `--watch <seconds>` prints the delta over
  an interval, `--pcie` adds the PCIe link behind the Thunderbolt tunnel (MPEIN)
  and the device's own stall counters (MPCNT).

---

## Not implemented / not tested

### Blocking production release (external dependency)
- **Apple DriverKit PCI + UserClient entitlements are still pending.**
  Developer ID signing, notarization, and a SIP-on clean-machine install are
  blocked until Apple grants
  `com.apple.developer.driverkit.transport.pci` +
  `com.apple.developer.driverkit.userclient-access`.
- No clean-machine (SIP-on) install / update / activate / deactivate / uninstall
  validation. Note: the bundle ID registered with Apple is
  `com.melondma.rdma.dext`; the repo still uses `com.mlx5.rdma.dext` — to align
  after the grant.

### Untested or partially working
- **no-FLR re-init does not pass on this firmware** (ConnectX-4 Lx 14.22.2560):
  `TEARDOWN_HCA → INIT_HCA` runs, but the vport `roce_en` readback stays 0 after
  `MODIFY_NIC_VPORT_CONTEXT(roce_en=1)` (fw status 0, syndrome 0). FLR recovery
  (`recovered_flr=1`) is the working fallback. `mlx_stable_gate --cycles 1..100`
  exists but fails on cycle 1 for this reason.
- Negative `TAKE` during teardown/reinit — not exercised.
- Repeated `INIT_HCA` within one firmware session (zeroed `sw_owner_id`) — not exercised.
- Boot-time takeover `LaunchDaemon` — not re-validated across an actual cold reboot.
- **MSI-X is configured but never delivered.** Two vectors are granted, the
  dispatch sources are created and enabled, the setup is ordered after the FLR
  that used to wipe it, and firmware accepts `CREATE_EQ` on either interrupt
  index — yet the driver's per-vector counters stay at zero through real traffic
  (32 CQ arms, 64 CQEs), and rebinding the completion EQ to index 0 with
  `mlx_irq_probe` leaves them at zero too, so it is not an index mismatch. The
  EQ timer is the actual delivery path and its period is the latency floor.
  Suspected cause is outside the driver: the card is claimed through IOCatalogue
  injection rather than a normal match, so interrupt routing is probably never
  established. Re-test on a clean-machine install once the entitlements land.
- **Blue-flame** doorbell not used.
- No userspace **MR cache** in the shim (large-buffer registration is 5–20 ms).
- `MlxHealth` health monitor is a skeleton.

### Explicitly out of scope / deferred
- **ConnectX-5/6/7/8** — capability-driven sizing is preparation, not support;
  each generation needs its own backend + capability validation + live gate.
  Only ConnectX-4 Lx (`15b3:1015`) is tested.
- NetworkingDriverKit integration (native `enX`, route, ARP/NDP, automatic
  endpoint discovery) — the DEXT uses explicit GID/MAC config, and the peer needs
  a static neighbour entry.
- UD/DC/XRC/SRQ/multicast, raw Ethernet QP, InfiniBand link layer, software RoCE.
- **GPUDirect RDMA** — N/A on macOS + M2 Ultra (no CUDA / peer-memory path).
- PFC and `QUERY_CONG_STATISTICS` — need switch-side fabric validation.
- Consumer integrations (`llama.cpp` RoCE transport, MLX backend, `mlx-cuda`):
  the MLX backend passed `MELON_MLX_GATE PASS` on two machines, but the reference
  consumer implementations were removed from the repo (consumer layer, not the DEXT).

---

## Real measured performance — ConnectX-4 Lx over PCIe Gen3 x4

| Metric | Value |
|---|---|
| Kernel-mediated RTT (synchronous ping-pong) | **~73 µs / ~13,700 msg/s** — flat across 1/64/256/1024 B (latency-bound) |
| Single-QP RDMA WRITE, 1 MiB | **20.1–20.7 Gbit/s** |
| P0.1 forward RDMA WRITE, 1024 × 1 MiB | **19.74 Gbit/s** |
| 8-QP aggregate | **21.16 Gbit/s** |
| Pipelined all-gather 4/16 MiB | **23.15 Gbit/s** |
| Practical host-link ceiling | **~25–28 Gbit/s** (theoretical ~31.5 Gbit/s for Gen3 x4) |
| TCP baseline (`AppleEthernetMLX5`) | 15–20 Gbit/s one-way, up to ~27.8 Gbit/s aggregate |

**Reading these numbers honestly:**

- The ~73 µs RTT is the current **kernel-mediated posting** path (correctness-first,
  "Option B"). More than 95% of it is software-path overhead, not the wire. Phase 3
  (direct UAR mapping + request pipelining) is the step expected to move below it.
- The 25–28 Gbit/s ceiling is the **PCIe Gen3 x4 host link (ADT-Link adapter)**, not the
  driver and not the 40G link. The card itself supports Gen3 x8, so a faster
  adapter/slot lifts this ceiling.
- The `Mbit/s` figures in the Phase-2 gate output are `size × 2 × 8 / 73µs` — a
  restatement of the fixed message rate, **not** a bandwidth measurement. Cite the
  73 µs RTT as latency, never as throughput.
- Application-layer (LLM inference) RDMA-vs-TCP was re-measured on 2026-09-06
  after the client-side work below, and RDMA now wins on both inference layouts.
  Against a stored 40G TCP baseline on the same hardware, 3 repetitions per
  point, Qwen3.6-35B-A3B: disaggregated TTFT 0.974 → 0.909 of TCP as context
  grows from 512 to 65536 tokens, prefill 1.03 → 1.10; tensor-parallel decode
  55.2 vs 46.9 tok/s at 512 and 22.1 vs 21.2 at 65536. Qwen3.8-27B is smaller
  but the same direction. The one remaining loss is split-mode TTFT at 32k–65k,
  0.6–1.7 % behind. Details and the settings that produce it are in
  `docs/llama-rdma-tuning.md`; raw CSVs stay in the dev tree.
- Three findings from that work belong to the host, not to this driver, and
  dominate anything the transport does. The peer's IOMMU in translation mode
  held one-sided WRITE to 12.6 Gbit/s in loopback; `iommu.passthrough=1` took it
  to 100.8, and across the wire from 13.2 to 23.0. RoCE path MTU is capped at
  4096 by the protocol, so a 9000-byte Ethernet MTU only exists to let 4096 fit
  in a frame. And the KV handoff carries a fixed 65.9 MB per request regardless
  of prompt length plus 10.6 KiB per token, which bounds what any further
  transport work can win.
