# Running llama.cpp on MelonDMA at full speed

The settings below are the difference between "RDMA is connected" and "RDMA
beats TCP". Every one of them was measured on the live cluster (Mac Studio M2
Ultra + NVIDIA DGX Spark, ConnectX over a 40G DAC) on 2026-09-06; the value in
the last column is what happens if you leave it out.

## 1. Mandatory, both inference layouts

| Setting | Side | Leaving it out costs |
|---|---|---|
| `MELONDMA_COMPLETION_POLICY=latency` | Mac | tensor-parallel decode falls from 55.2 to 16.3 tok/s (TCP does 46.9) |
| `MELONDMA_LOCAL_IP`, `MELONDMA_LOCAL_MAC`, `MELONDMA_REMOTE_MAC` | Mac | the probe opens the device, creates a QP, then fails with only `RDMA probe failed locally` |
| `GGML_RDMA_DEV=mlx5_0` | Mac | auto-detect looks for a GID matching the TCP control address and finds none |
| **no** `GGML_RDMA_GID` | Mac | a pinned index belongs to the first process on the host; every later client fails `query_gid` |
| `GGML_RDMA_DEV=rocep1s0f1`, `GGML_RDMA_GID_ADDR=<peer RDMA IPv4>` | Linux peer | a pinned index silently selects the wrong GID after a reboot |
| `GGML_RPC_RDMA_RX_DEPTH=160` | Linux peer | the sender stalls on RNR and its completion wait becomes a spin (63 % of a core) |
| `iommu.passthrough=1` in the peer's kernel cmdline | Linux peer | one-sided WRITE runs at 12.6 instead of 100.8 Gbit/s in loopback, 13.2 instead of 23.0 across the wire |
| `MELONDMA_DIRECT_UAR=1`, `MELONDMA_DIRECT_CQ=1` | Mac | every post and poll becomes a DriverKit call |
| `GGML_RPC_REQUIRE_RDMA=1` | both | a failed probe becomes a silent TCP fallback — this cluster ran on TCP for weeks without anyone noticing |

The Mac binary must be signed with
`com.apple.developer.driverkit.userclient-access = com.mlx5.rdma.dext`, and the
Linux peer needs a static neighbour entry for the Mac's RDMA address: the DEXT
owns the port and answers no ARP, so `modify_qp` to RTR fails without it.

## 2. Additional, disaggregated prefill only

| Setting | Value | Why |
|---|---|---|
| `GGML_RPC_RDMA_KV_BATCH` | `1` | one batched one-sided transfer instead of 80 RPCs |
| `GGML_RPC_RDMA_FINAL_DEST` | `host` | the peer writes straight into a registered arena; without it the KV is streamed |
| `GGML_RPC_RDMA_DEST_ARENA_MAX` | `96` (MiB) | 192 MiB was measured and gains nothing: one larger MR registers as slowly as two smaller ones |
| `GGML_RPC_RDMA_KV_FENCE` | `0` | asks to drop the ordering barrier; the client grants it only while its run-ahead fits the peer's receive window, so it is safe at any context |

Split mode needs none of these: there is no KV handoff between the machines.

## 3. Launch example

```sh
# Linux peer (RPC server)
env GGML_RDMA_DEV=rocep1s0f1 GGML_RDMA_GID_ADDR=192.168.200.2 \
    GGML_RPC_REQUIRE_RDMA=1 GGML_RPC_RDMA_WRITE_KV=1 \
    GGML_RPC_RDMA_RX_DEPTH=160 GGML_RPC_RDMA_STATS=1 \
    ./build/bin/ggml-rpc-server -H <peer ip> -p 50052 -c

# Mac (llama-server), disaggregated prefill
env MELONDMA_LOCAL_IP=192.168.200.1 \
    MELONDMA_LOCAL_MAC=<mac card mac> MELONDMA_REMOTE_MAC=<peer card mac> \
    MELONDMA_DIRECT_UAR=1 MELONDMA_DIRECT_CQ=1 MELONDMA_COMPLETION_POLICY=latency \
    GGML_RDMA_DEV=mlx5_0 GGML_RPC_REQUIRE_RDMA=1 GGML_RPC_RDMA_STATS=1 \
    GGML_RPC_RDMA_WRITE_KV=1 GGML_RPC_RDMA_KV_BATCH=1 \
    GGML_RPC_RDMA_FINAL_DEST=host GGML_RPC_RDMA_DEST_ARENA_MAX=96 \
    GGML_RPC_RDMA_KV_FENCE=0 \
    ./build/bin/llama-server -m <model.gguf> --rpc <peer>:50052 \
      --device MTL0 --prefill-device RPC0 -fa on \
      --cache-type-k q8_0 --cache-type-v q8_0 -c 16384
```

## 4. Verifying it actually runs on RDMA

Both sides must log `RDMA connection active` before the first model operation.
With `GGML_RPC_RDMA_STATS=1` the disaggregated path then prints, per request:

```text
GGML_RPC_KV_BATCH token=1 items=80 bytes=65896320 rpc=1 one_sided=80 fallback=0 \
    fence_ms=48.5 reg_ms=21.3 wait_ms=25.1 total_ms=95.0
GGML_RPC_KV_PREFETCH_SKIP items=20 bytes=32640 fence=0 \
    peer_absorb_bytes=41943040 run_ahead_bytes=...
```

`one_sided` must equal `items` and `fallback` must be 0. `fence=0` means the
barrier was dropped for that chunk; it flips to 1 automatically once the
run-ahead reaches half the peer's window, which is the intended behaviour on
long contexts, not a fault.

The card's own receive-side view is `mlx_port_counters --watch <seconds>`: over
a transfer it must show received bytes matching what the peer sent, and zero
errors, discards and pause frames.

## 5. Failure signatures

| What you see | What it means |
|---|---|
| `RDMA probe: no matching device/GID` | auto GID selection; name the device and use `GGML_RDMA_GID_ADDR` |
| `RDMA probe: query_gid(index=N) failed` | a pinned GID index owned by another process |
| `RDMA probe failed locally` with no further line | the `MELONDMA_*` address variables are missing |
| `RDMA activate failed, staying on TCP` | the peer cannot resolve this host's RDMA address; add the static neighbour entry |
| `Remote did not offer RDMA caps` | the other side failed its own probe; read its log, not this one |
| decode roughly a third of expected | `MELONDMA_COMPLETION_POLICY` is not `latency` |
| Mac CPU tens of percent during prefill | the peer's receive ring is too small for the run-ahead; raise `GGML_RPC_RDMA_RX_DEPTH` |

## 6. What this buys, measured

Qwen3.6-35B-A3B and Qwen3.8-27B, 3 repetitions per point, against a stored
40G TCP baseline on the same hardware. A ratio below 1.0 means RDMA is faster.

| Layout | Model | Context 512 | 8192 | 16384 | 65536 |
|---|---|---:|---:|---:|---:|
| Disaggregated, TTFT ratio | 35B | 0.974 | 0.934 | 0.916 | 0.909 |
| Disaggregated, TTFT ratio | 27B | 1.041 | 0.975 | 0.968 | 0.978 |
| Split, decode tok/s (RDMA vs TCP) | 35B | 55.2 / 46.9 | 48.3 / 41.6 | 41.3 / 35.7 | 22.1 / 21.2 |
| Split, decode tok/s (RDMA vs TCP) | 27B | 16.0 / 14.7 | 14.4 / 12.8 | 12.9 / 11.8 | 8.0 / 7.6 |

RDMA wins on both paths. The one place it does not is split-mode TTFT at 32k
and 65k, where it is 0.6–1.7 % behind while decode is still 4–11 % ahead.

The transport is not the ceiling any more: the Mac's card sits behind a
Thunderbolt-tunnelled PCIe Gen3 x4 link, about 31.5 Gbit/s per direction, and
the KV handoff carries a fixed 65.9 MB per request regardless of prompt length
plus 10.6 KiB per token. Those two facts bound what any further transport work
can win.
