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
- No **MSI-X** (the EQ polls ~10 ms), so event-driven completions have a ~10 ms floor.
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
- Application-layer (LLM inference) RDMA-vs-TCP runs were also collected
  (`llama_bench_*.csv`, `muser_rdma_*.csv` in the dev tree): RDMA and TCP are within
  a few percent at those sizes (RDMA slightly better on decode tok/s, mixed on TTFT).
  Those results are workload-bound, not driver-bound, and are kept in the dev tree
  only.
