# Research and confirmed results

This document replaces the scattered notes journal. The numbers below are split
into measured facts, engineering conclusions and open questions.

## 1. The original goal

Give macOS access to a real Mellanox ConnectX RDMA engine and interoperate with a
Linux peer. Apple's `AppleEthernetMLX5` only gives Ethernet, so a custom
PCIDriverKit DEXT plus a userspace verbs-compatible layer was chosen.

## 2. Hardware configuration

The verified setup:

- Mac Studio M2 Ultra;
- Mellanox ConnectX-4 Lx EN `MCX4131A-BCAT`, PCI `15b3:1015`, subsystem
  `15b3:0005`;
- ConnectX-7 in a DGX Spark;
- QSFP+ 40G DAC on an ADT-Link PCIe Gen3 adapter;
- RoCEv2 between the Mac and Spark, plus a separate TCP network for
  rendezvous/control.

The card supports PCIe Gen3 x8, but the Mac-side adapter negotiates Gen3 x4.
Spark shows PCIe 32 GT/s x4; the Mac side shows 8 GT/s x4.

## 3. Why not Apple Ethernet and not Apple RDMA

Stock `AppleEthernetMLX5` caps the Ethernet MTU at 2034 and provides a normal
network path, not verbs/RDMA. TCP/RPC measured about 15–20 Gbit/s one-way and up
to ~27.8 Gbit/s aggregate; the limit is packet-per-second/CPU and the Skywalk
packet pool, not the 40G physical link.

Apple TN3205 RDMA-over-Thunderbolt is not a replacement: it needs TB5, is
Mac↔Mac only, uses a closed Apple wire protocol, exposes only SEND/RECV and up to
10 UC QPs, and has no RDMA READ/WRITE or Linux RoCE compatibility. On M2/TB4 it
does not apply.

## 4. PCI and firmware bring-up

Confirmed:

- the DEXT takes PCI ownership from `AppleEthernetMLX5` in a development
  configuration;
- BAR0 (32 MiB) maps as non-cacheable;
- MMIO reads firmware `0x0016000e` / 14.22.2560;
- after handover, the PCI Command Register needs Memory Space and Bus Master
  re-enabled;
- the binary must be `arm64e`;
- the PCI device ID is read from the upper half of the config-space dword;
- self-FLR returns the card to a clean state without a power cycle.

Working firmware sequence:

```text
ENABLE_HCA
QUERY_ISSI / SET_ISSI
QUERY_PAGES(BOOT)
MANAGE_PAGES(GIVE)
SET_HCA_CAP
QUERY_PAGES(INIT)
MANAGE_PAGES(GIVE)
INIT_HCA
QUERY_HCA_CAP
```

Real values: 6 boot pages, 4465 init pages, then a runtime request of 3332 pages;
accounting completed as `fw_owned=7803, ambiguous=0`. Earlier `delivery_status=6`
and `BAD_BLK_NUM` were caused by a 576-byte mailbox DMA segment instead of a full
4 KiB page and by `block_num` going backwards. A fixed 512-page limit and a stack
scratch buffer also did not scale.

Firmware reported `logMaxQp=14`, `logMaxCq=24`, `logMaxMkey=24`, Ethernet mode,
1 port, RoCE enabled, 4 KiB UAR and a 256-entry GID table.

## 5. RDMA correctness

PD/XRCD/UAR/EQ/CQ/MKEY/QP, GID programming/readback and QP transitions
`RESET → INIT → RTR → RTS` were confirmed on a live Mac↔Spark. A real UserClient
registered client memory; SEND/RECV passed with guard bytes and payload
validation.

Full gate: 1,000,000 bidirectional messages of sizes 1/64/256/1024 bytes, SQ/RQ/CQ
wrap, 10 create/connect/traffic/destroy cycles. Result: 0 CQE errors, 0 timeouts,
0 FLR and 0 resource drift.

The synchronous kernel-mediated baseline is about 73 µs RTT and ~13,700
messages/s. This is a latency baseline, not a bandwidth result: the time barely
depends on message size.

## 6. Performance and one-sided access

RDMA WRITE in both directions is hardware-proven. Persistent MR and direct UAR
gave:

- single-QP 1 MiB: about 20.1–20.7 Gbit/s;
- 8-QP aggregate: up to 21.16 Gbit/s;
- 10-run stability gate: 20.13–20.15 Gbit/s;
- remote memory verification, 0 drops/errors/discards and 0 link recovery.

RoCE MTU 4096 with Ethernet MTU 9000 works; adaptive GID/MTU discovery selects
the active values. Pipelined all-gather gave about 23.15 Gbit/s at 4/16 MiB and
21.69 Gbit/s at 64 MiB. Persistent slots gave up to 10–18% over the sequential
variant.

Changing batch size, window, number of doorbells and direct-vs-fallback did not
lift the ceiling. Most likely the limit is PCIe Gen3 x4 pacing and
DEXT/host serialization. The practical ceiling of the current adapter is about
25–28 Gbit/s, theoretical about 31; it is not a firmware protocol limit.

## 7. Integrations

The MLX backend passed `MELON_MLX_GATE PASS` on two real machines: send/recv,
all_gather and all_sum. On Spark, `cudaHostAlloc` was required because MLX's
unified-memory allocator could not be registered directly through ibverbs. Also
fixed: function-pointer corruption in the RDMA context, decoding the hardware CQE
syndrome, and a stale MAC during RDMA bonding.

llama.cpp/RDMA RPC loaded a real 17.9 GB GGUF; health and chat completion
returned HTTP 200 with no CQ errors. The production DEXT remains generic;
MLX/llama.cpp are separate consumers. (These bespoke consumers were later removed
from the repo — the generic verbs API is the supported surface.)

## 8. Key engineering decisions

1. PCIDriverKit instead of a kext: on modern Apple Silicon the kext path is
   unusable without disabling SIP.
2. A custom UserClient ABI instead of private `IORDMAFamily`: DriverKit cannot
   register through the private kernel C++ APIs.
3. Correctness-first kernel-mediated path, then an isolated per-client direct
   UAR/SQ/RQ/DB; a global UAR is forbidden.
4. Firmware DMA ownership matters more than fast release: ambiguous mappings go
   to quarantine until a verified FLR.
5. Capability-driven hardware abstraction instead of CX-4 constants; extending
   CX-5/6/7/8 requires a PCI match and hardware validation, not a protocol change.
6. No Apple Thunderbolt RDMA, iWARP or software-RoCE: none of them gives a
   compatible Mac↔Linux ConnectX path.

## 9. Open questions

- Confirm same-session teardown/reclaim, negative TAKE and repeated `INIT_HCA`.
- Confirm unattended cold reboot with a takeover LaunchDaemon.
- Obtain the Apple PCI/user-client entitlements and pass SIP-safe
  signing/notarization.
- Test a second ConnectX generation on real hardware; a PCI-ID table alone is
  not enough.
- For throughput above the current baseline, investigate a faster/wider
  adapter or confirmed PCIe transaction settings.
