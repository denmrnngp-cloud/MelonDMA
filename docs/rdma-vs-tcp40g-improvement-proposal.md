# RDMA против TCP40G: результаты, пробелы и план достижения победы

Дата: 2026-09-05  
Статус: engineering proposal, основано на свежих CSV, исходниках llama.cpp/MelonDMA и внешних документах.

## 1. Исполнительный вывод

На текущей конфигурации RDMA пока **не победил TCP40G end-to-end**:

- Qwen3.8-27B disagg: RDMA и TCP40G практически равны по скорости, но RDMA использует существенно больше CPU.
- Qwen3.8-27B split: RDMA немного быстрее по prefill, но decode на малых сообщениях не показывает устойчивой победы.
- Qwen3.6-35B-A3B split: RDMA проигрывает TCP40G по decode примерно на 2--7% в свежем прогоне; это главный незакрытый функциональный разрыв.
- RDMA не может получить большой абсолютный выигрыш на текущем Mac Studio, пока ConnectX-4 Lx подключен через PCIe Gen3 x4/Thunderbolt: измеренный практический потолок всей платформы около 25--28 Gbit/s, тогда как карта рассчитана на Gen3 x8.

Цель нужно формулировать так:

1. **Disagg/bulk:** RDMA должен убрать лишнюю CPU-копию и дать меньший handoff time, но общие TTFT/prefill могут отличаться слабо, если вычисление занимает почти весь запрос.
2. **Split/small messages:** RDMA должен превзойти TCP40G по decode и p95 RPC latency за счет меньшего числа постов, doorbell writes, CQ completions и host copies.
3. **CPU:** сравнивать не только `mac_cpu_pct`, а CPU-seconds/request всей цепочки, включая RPC worker, DEXT, `kernel_task`/interrupts на Mac и rpc-server на Spark.
4. **Инфраструктура:** для абсолютной победы по bulk сначала нужен более широкий PCIe путь или нативная RDMA-карта/адаптер. Оптимизация DEXT не отменяет физический потолок.

Фраза «RDMA намного эффективнее по накладным расходам» относится к аппаратному
Linux verbs/GPU-direct пути. На Mac DEXT 0.359 уже умеет NIC DMA непосредственно
в `.shared | .untracked` Metal/UMA buffer, а direct SQ/CQ убирает ExternalMethod
из нормального datapath. Оставшийся разрыв находится в inference integration:
финальный KV layout ещё не является зарегистрированным destination во всех
путях, а Spark всё ещё может копировать backend tensor во временный host chunk.

## 2. Данные свежего сравнения

Источники данных:

- TCP40G: `/Users/macstudio/llama.cpp/bench_tcp40g_results.csv`
- RDMA: `/Users/macstudio/llama.cpp/bench_rdma_all_optimizations_2026-09-04.csv`
- Предыдущий анализ: `dev/docs/benchmark-rdma-vs-tcp-2026-09-02.md`
- Предыдущие RDMA-оптимизации: `dev/docs/benchmark-rdma-cluster-2026-09-03.md`

Свежий RDMA прогон: mailbox on, MTU 4096, zero errors; для 27B доступно 3 повтора до 8K, 2 повтора на 16K/32K и 1 повтор на 65K. Поэтому значения 65K считаются ориентировочными, а не статистическим доказательством.

### 2.1 Qwen3.8-27B disagg

| context | prefill RDMA/TCP40G, tok/s | decode RDMA/TCP40G, tok/s | TTFT RDMA/TCP40G, s | Mac CPU RDMA/TCP40G |
|---:|---:|---:|---:|---:|
| 512 | 558.6 / 574.2 | 24.66 / 24.79 | 0.945 / 0.920 | 9.5 / 8.1 |
| 1024 | 662.3 / 672.0 | 23.89 / 23.90 | 1.631 / 1.612 | 8.5 / 6.8 |
| 2048 | 713.9 / 723.0 | 23.58 / 23.74 | 3.067 / 3.062 | 7.9 / 5.2 |
| 4096 | 724.1 / 729.2 | 21.54 / 21.97 | 6.074 / 6.036 | 7.1 / 4.5 |
| 8192 | 723.3 / 725.9 | 18.78 / 19.61 | 12.47 / 12.45 | 6.6 / 3.9 |
| 16384 | 701.6 / 699.0 | 15.31 / 15.69 | 25.97 / 26.13 | 6.45 / 3.5 |
| 32768 | 646.1 / 644.6 | 11.09 / 11.09 | 56.72 / 56.86 | 6.45 / 3.3 |
| 65536 | 551.0 / 550.1 | 6.94 / 6.95 | 136.74 / 136.95 | 5.9 / 3.3 |

RDMA is within roughly 1% on bulk TTFT at 16K--65K, but does not reduce CPU below TCP40G. At 65K the reported `cpu_s_request` is about 141 s because it includes the Spark-side compute process; this metric must be decomposed before using it for transport conclusions.

### 2.2 Qwen3.8-27B split

RDMA prefill is close to TCP40G. Decode is close at the measured 27B points, but no robust advantage is established. The fresh RDMA result has `itl_mean_s` around 68--75 ms; TCP40G does not yet record equivalent ITL fields, so p95 comparison is currently impossible.

### 2.3 Qwen3.6-35B-A3B split

The important point is small-message behavior. In the earlier controlled comparison, RDMA was approximately 39.9 tok/s versus TCP40G 46.6 tok/s at context 1024. The newer RDMA file improves this family to about 47--48 tok/s at small contexts, but the comparison must be rerun interleaved with TCP40G using the same exact harness and clocks. Until that is done, the 5% victory gate is open, not passed.

### 2.4 What the previous TCP40G baseline proves

TCP40G already uses a high-quality kernel/NIC path. Its application CPU is typically 3--8% on disagg. Therefore the relevant RDMA target is not the 45% busy-poll baseline anymore; that was fixed by blocking completion delivery. The remaining target is approximately:

- Mac application CPU: RDMA <= TCP40G + 1 percentage point in disagg.
- Split decode: RDMA >= TCP40G * 1.05 at context 512/1024, with no p95 regression.
- Bulk KV handoff: RDMA handoff time >= 15% faster than TCP40G in an isolated transfer benchmark.

## 3. What is already implemented

### MelonDMA provider

The current development tree already contains:

- Direct per-client UAR/SQ/RQ/CQ path with isolation.
- O(1) lkey/rkey lookup instead of a linear MR-table scan.
- Batched WQE posting and doorbell publication.
- Selective signaling and CQ polling in batches in the provider ABI.
- Inline SEND up to 512 bytes.
- MTU/GID negotiation, including 4096-byte RDMA MTU where the path permits it.
- CQ event path, direct CQ visibility and fallback to kernel-mediated polling.
- Persistent MR strategy for transport buffers.
- Apple Silicon coherent UMA MR, enabled by default through
  `MLX_UC_FEATURE_COHERENT_UMA_MR`; live Metal→Spark, Spark→Metal and 4 MiB
  indirect-MR gates pass at MTU 4096.
- RC READ/WRITE, `WRITE_WITH_IMM`, atomics, CQE status/syndrome reporting.
- Firmware DCQCN query/modify path. The host does not reimplement DCQCN.
- Correct lifetime, stale-handle, teardown and fail-closed FLR behavior.

These are necessary correctness/performance features, but most are not still open optimization levers.

### llama.cpp transport

Current code already has:

- `rdma_send()` chained SEND windows, two staging generations and one signaled WR at the end of a window.
- Mailbox for small messages, sequence validation and one-sided ACKs.
- `rdma_write()` with chained WRITE windows and a final `WRITE_WITH_IMM` notification.
- Blocking completion-channel polling with TCP liveness in the same `poll()`.
- Direct UAR/CQ and Blue Flame feature switches.
- Negotiated MTU and persistent connection resources.
- Selective signaling for mailbox traffic.

This changes the next actions: repeating the same batching or adding another generic QP is unlikely to solve the remaining problem.

## 4. What is still missing or not optimized

### P0: remove the extra KV transfer and host staging copy [IN PROGRESS]

Implemented: `rdma_write()` now rotates the existing two staging generations and overlaps copy/post of the next one-sided WRITE window with completion of the previous window. This removes the old per-window stop-and-wait behavior and uses the already allocated `RDMA_TX_WINDOW=2` resources.

Not yet complete: the driver can now register Metal UMA memory directly, but
the application still calls `ggml_backend_tensor_get()` into a temporary
`std::vector<uint8_t>` before `rdma_write()`, and the registered Mac destination
is not yet proven to be the final `ctx_tgt`/KV Metal layout. The
direct-final-layout change and byte-accounting gate remain open.

In `ggml-rpc.cpp`, the one-sided GET path registers `cmd.dest`, asks the server to write into it, and then waits for the write completion. However, the server-side SET/GET flow still contains a separate tensor extraction/staging path in places, and the historical measurements explicitly show KV bytes transferred twice during disagg. The code also calls `ggml_backend_tensor_get()` into a temporary `std::vector<uint8_t>` before `rdma_write()`.

Required design:

- Make the destination buffer used by the inference consumer the final KV layout, not a temporary transport buffer.
- Reuse the existing llama Metal allocation: on Apple unified-memory devices it
  is already page-aligned `vm_allocate` memory wrapped by
  `newBufferWithBytesNoCopy(...StorageModeShared)`. Register the backend buffer
  base once and validate tensor offsets; do not introduce another staging pool.
- Add a dedicated RDMA-compatible/untracked Metal buffer policy or explicit
  command-buffer ordering instead of silently changing hazard tracking for all
  Metal allocations.
- Advertise its address/rkey once per stable allocation, not per request.
- Have the producer write directly into that registered destination region.
- Send one final completion token after all payload writes; do not perform a follow-up GET of the same ranges.
- Keep the MR alive across requests and deregister only after all QP work is retired.
- Add a byte-accounting assertion: logical KV bytes, RDMA TX bytes and application-consumed bytes must match exactly once.

Expected result: remove one full memory copy and one full network transfer from the disagg handoff. This is the most direct TTFT improvement available in the current architecture.

Acceptance:

- `kv_logical_bytes == rdma_wire_payload_bytes` within defined RoCE overhead.
- `duplicate_kv_bytes == 0`.
- Isolated handoff p50 and p95 at least 15% better than TCP40G.
- Final KV consumer sees correct tensor values without a post-transfer copy.

### P0: make split traffic genuinely batched

`rdma_mailbox_send()` sends one `RDMA_WRITE_WITH_IMM` per mailbox item and waits on every `inline_signal_interval` item. The mailbox reduces protocol overhead, but it is still one verbs post per logical message. `rdma_send()` batches only when the caller presents a contiguous byte stream; it cannot combine several independently produced activation messages unless the RPC layer coalesces them first.

Required design:

- Add a bounded activation aggregate: collect messages for one decode step or up to a small byte/time threshold.
- Encode one compact header containing count, lengths and sequence numbers.
- Copy/pack once into a pre-registered staging region, or use a multi-SGE WR when source buffers are already stable and non-contiguous.
- Post one WR chain per aggregate, with only the last WR signaled.
- Use inline only for genuinely small control/header payloads. Do not inline large activation data merely because the provider supports 512 bytes.
- Preserve message boundaries and bounded maximum aggregate size; never trade correctness for coalescing.

Start with aggregate sizes 4, 8, 16 and 32 messages and measure. The optimum is not assumed to be 16.

Acceptance:

- at least 5% decode improvement over TCP40G at 35B split context 512 and 1024;
- p95 per-token RPC latency no worse than TCP40G;
- doorbell writes/token and CQEs/token reduced by at least 4x;
- no increase in `RNR`, retry, CQ overflow or fallback counters.

### P0: prove direct CQE coverage in the actual llama path [INSTRUMENTATION OPEN]

The provider already decodes ordinary CQEs directly, tracks inline/mixed WRs and
has passed direct-path hardware gates. The open task is not another decoder; it
is proof that the exact llama workload stays on this path without a DriverKit
call per ordinary completion, plus attribution of every exceptional fallback.

Required instrumentation:

- Count `direct_cq_polls`, `kernel_cq_polls`, `direct_cq_fallbacks`, `direct_db_rings` and `kernel_db_rings` per request.
- Record the reason for every fallback: unknown QP, UMR/LOCAL_INV,
  stale/missing WR metadata, or feature disabled. Ordinary error and atomic
  CQEs are already decoded directly.
- Correlate every CQE with the posted WR without scanning a large table.
- Drain multiple CQEs per poll and retire unsignaled WR ranges from the final signaled completion.
- Keep the completion channel for idle periods, but use direct CQ polling while a transfer batch is active.

This hybrid policy is likely better than permanently blocking or permanently spinning: low latency while active, near-zero CPU while idle.

Acceptance:

- direct path >= 99.9% of steady-state data-path polls;
- zero unexpected fallback in a 1M-message gate;
- CPU-seconds/request no higher than TCP40G;
- no TTFT regression greater than 1% from blocking mode.

### P1: eliminate remaining per-request allocation and registration bookkeeping

The transport uses fixed stack arrays for WR/SGE windows and keeps an MR cache,
so actual DEXT registration is already reused for an identical address/length.
However, inference still creates temporary vectors/chunks, performs a linear MR
cache lookup and acquires/releases the destination registration per request.
These costs should be removed from the stable final-KV path.

Required design:

- Preallocate WR, SGE and completion metadata arrays per connection.
- Replace request-local `std::vector<uint8_t>` growth with a persistent aligned pool.
- Cache registration by page-aligned address, length and allocation identity.
- Reuse two or more staging generations, with explicit completion ownership.
- Add a hard limit and LRU eviction only if multiple long-lived buffers are needed.

Do not add ODP on macOS: it is not available in the current MelonDMA subset and would add page-fault latency rather than solve this workload.

### P1: tune active-vs-idle completion policy

The current blocking path fixed the 45--49% busy-poll CPU problem, reducing RDMA disagg CPU to roughly 6--10%, but it introduced wakeup overhead and leaves a worker cost during low-rate split traffic. Implement a two-state policy:

- Active state: direct CQ drain, bounded spin/yield budget, batch poll.
- Idle state: arm CQ and block on completion channel plus control socket.
- Transition to idle after a bounded number of empty polls or a short inactivity interval.
- Wake immediately on a new request and on peer failure.

Tune thresholds from measured p50/p95, not from a fixed 50 microsecond constant.

### P1: optimize the macOS DEXT boundary

The Linux model assumes `ibv_post_send()` and `ibv_poll_cq()` are userspace operations. A DriverKit ExternalMethod is not equivalent. Any remaining per-WR ExternalMethod call is a structural disadvantage against TCP40G.

Required checks:

- Verify that all steady-state SQ/RQ/CQ operations in the llama profile avoid `IOConnectCallStructMethod`.
- Batch all control metadata updates, including DB-record publication, up to a safety watermark.
- Keep ownership/lifetime validation out of the hot loop once a trusted QP/MR token is established, while retaining validation at setup and fail-closed paths.
- Measure ExternalMethod calls/request, not only CPU percentage.
- Keep DEXT `IOLog` disabled in release and verify no debug logging is enabled by inherited environment.

### P1: compare the correct CPU quantity

The previous CSV reports process CPU percentages with noisy Spark sampling. Add a common measurement record for both transports:

- Mac llama process CPU time.
- Mac RPC worker CPU time if separate.
- DEXT CPU/interrupt time where observable.
- `kernel_task` and interrupt contribution on Mac.
- Spark rpc-server CPU time.
- Wall time and CPU-seconds/request.
- Number of requests and exact completed token count.

TCP40G must use the same process placement, affinity, thermal state and run order. Otherwise the CPU conclusion is not defensible.

## 5. Hardware and fabric work

### Current Mac Studio limitation

The ConnectX-4 Lx supports PCIe Gen3 x8, but the measured Mac path is Gen3 x4 through the adapter. This is consistent with the observed 20--23 Gbit/s application results and the estimated 25--28 Gbit/s platform ceiling. No amount of DCQCN, inline tuning or extra QPs can produce a sustained 2x win through that link.

Required experiment before deeper driver work:

1. Record PCIe negotiated width/speed and AER counters on both ends.
2. Run an isolated RDMA WRITE bandwidth/latency gate with 1, 2, 4, 8 and 16 QPs.
3. Run the equivalent TCP40G socket bulk gate with the same payload and buffer lifetime.
4. Test a Gen3 x8 or newer direct adapter/path if available.
5. Only call a transport optimization successful when isolated RDMA bandwidth rises; otherwise classify it as application CPU/latency work.

### RoCE QoS

The source documentation and industry deployment guidance agree on the expected order:

- ECN/DCQCN reacts before queues fill.
- PFC is a backstop, not a normal rate-control mechanism.
- RDMA traffic needs a dedicated priority/DSCP class.
- Drops, pause storms and retransmissions invalidate latency comparisons.

Collect from the switch/NIC during each benchmark:

- ECN-marked packets and CNP count.
- PFC pause frames and pause duration for the RDMA class.
- RX/TX discards and CRC errors.
- Retries, RNR and QP error events.
- Per-QP or per-destination rate limiter state where the NIC exposes it.

Do not tune DCQCN blindly on this point-to-point test. If PFC/ECN counters are zero, congestion control is not the cause of the TCP comparison gap.

### Multi-QP and multi-rail

Multiple QPs help hide RTT and ECMP collisions, but on the current x4 single-adapter path they can only approach the same bottleneck. Keep the existing single-QP default for inference and test 2/4/8 QPs only in an ablation. Multi-rail becomes high priority after a wider PCIe path or a second physical rail exists.

## 6. Apple Silicon M-series: отдельный анализ платформы

### 6.1 Подтвержденная топология текущего Mac

Снято на текущем Mac Studio:

- Apple M2 Ultra, 24 CPU cores, 192 GB unified memory.
- Wavlink UTE02, Thunderbolt 3, link 40 Gb/s.
- ConnectX-4 Lx `15b3:1015`, link up.
- PCIe negotiated speed `8.0 GT/s`, width `x4`.

Карта поддерживает PCIe Gen3 x8, но фактический путь Mac проходит через Thunderbolt enclosure и работает как Gen3 x4. Это согласуется с измеренным RDMA потолком около 25--28 Gbit/s. Поэтому гарантированный большой bandwidth-win невозможен на этом тракте: сначала ограничитель нужно убрать физически.

### 6.2 Что специфично для Apple Silicon

Apple Silicon имеет unified memory. Это не даёт внешней PCIe NIC доступ к любому
Metal resource, но для host-visible `.shared` buffer MelonDMA теперь имеет
явный и проверенный zero-copy контракт. Актуальны следующие границы:

- DriverKit DEXT выполняется вне обычного kernel-kext пути; приложение общается с ним через UserClient/ExternalMethod и mapped memory. Это отдельная IPC/validation граница, а не прямой `libmlx5` userspace provider.
- DMA проходит через macOS DriverKit/IODMACommand/resource mapping. Нельзя предполагать, что CPU virtual address, IOVA и PCIe bus address совпадают или что relaxed ordering можно включить без измерения.
- `.shared | .untracked` `MTLBuffer.contents` регистрируется обычным MR через
  DriverKit/IODMACommand/DART. DEXT 0.359 рекламирует этот путь по умолчанию;
  Mac Studio↔Spark hardware gate подтвердил обе стороны и indirect MR.
- Когерентность данных не заменяет порядок: GPU producer должен завершиться до
  RDMA post, а GPU consumer запускается после CQE/WC.
- Apple GPU и Metal не предоставляют аналога CUDA `nvidia-peermem`,
  GPU-resident mlx5 WQE/CQ или GPU-issued UAR doorbell. `.private` buffer и BAR
  MMIO не являются регистрируемой Metal memory.
- Thunderbolt tunneling добавляет контроллеры, буферизацию и flow control между NIC и SoC. Даже идеальный verbs hot path не устраняет эту транспортную задержку и ограничение ширины.
- Power management и link state внешнего enclosure могут влиять на latency. Для сравнений нужны одинаковые power/thermal условия и проверка, что link не retrain'ится.

Публичные Apple материалы по DriverKit/PCIDriverKit описывают extension/UserClient модель и PCI device ownership, но не обещают userspace DMA rings или GPU peer-memory semantics. Поэтому подобные возможности нельзя считать доступными только по наличию M-series unified memory.

### 6.3 Лучшие точки оптимизации именно на M2/macOS

#### A. Убрать UserClient вызов с каждого WR/WC

Это самый важный программный рычаг. TCP40G получает kernel/NIC offload через оптимизированный socket path, а RDMA проигрывает, если `IOConnectCallStructMethod` остаётся на каждом post/poll.

Проверить и сделать acceptance-метрикой:

- `ExternalMethod calls/request`;
- `direct_cq_polls`, `kernel_cq_polls`, `direct_cq_fallbacks`;
- `direct_db_rings`, `kernel_db_rings`;
- время в `MlxUserClient::ExternalMethod`, `MlxCQ::PollCQ`, `MlxQP::PostSendBatch`;
- число DEXT wakeups и CPU time DEXT/interrupts.

Целевой steady state: mapped SQ/CQ/UAR, direct WQE/CQE operations, один UserClient вызов только для setup, recovery и редкого control path. Сохранять проверку ownership/range/lifetime на trust boundary; оптимизация не должна превращать security validation в unchecked pointer ABI.

#### B. Использовать режим active-direct + idle-event

На активной передаче: короткий bounded spin/direct CQ drain и batch poll. В простое: arm CQ и `poll/kqueue` на completion/control fd. Постоянный busy-poll оказался причиной 45--49% Mac CPU; постоянный blocking path добавляет wakeup latency. Гибридная политика должна дать TCP-подобный idle CPU и RDMA-подобную активную latency.

#### C. Уменьшить host memory traffic

Приоритет для текущего M2:

1. KV write непосредственно в финальный зарегистрированный host/shared destination.
2. Не делать `ggml_backend_tensor_get()` в промежуточный `std::vector`, если layout можно предоставить как готовые contiguous ranges.
3. Для non-contiguous ranges использовать SGE вместо pack-copy.
4. Переиспользовать pinned buffers и MR; не вызывать register/deregister на запрос.
5. Изолировать mailbox/control cache lines и выровнять sequence/ACK слова на cache line.

Проверять не только wall time, но и bytes read/write по host buffers, потому что unified memory может скрывать цену копии в общей memory fabric.

#### D. Настроить Apple CPU affinity осторожно

На M2 нет Linux NUMA-интерфейса, аналогичного `local_cpulist`. Не переносить Linux NUMA-рецепты автоматически. Нужно измерить:

- P-core против E-core для completion worker;
- закрепление worker на одном P-core;
- влияние QoS/priority и timer coalescing;
- cache contention с Metal/llama main thread.

Для latency-sensitive split использовать P-core и active-direct короткими burst'ами; для idle disagg worker должен блокироваться. Любое affinity-решение принимать по p95 ITL и CPU-seconds, а не по nominal utilization.

#### E. Проверить PCIe transaction behavior, не угадывать

На Apple Silicon через enclosure нельзя без доказательства включать relaxed ordering, larger MRRS, no-snoop или аналогичные PCIe настройки. Добавить диагностический gate:

- negotiated speed/width до и после запуска;
- PCIe correctable/AER/retrain counters;
- RDMA payload bandwidth и CQ latency;
- CPU/DMA buffer traffic;
- повторяемость после cold boot и длительного soak.

Если изменение повышает throughput только на синтетическом тесте, но ухудшает CQ p95 или вызывает retrain, его не принимать.

### 6.4 Что нужно проверить экспериментом на Apple Silicon

1. **Прямой PCIe adapter/path:** карта без Thunderbolt tunneling, Gen3 x8 или Gen4 x4; это главный тест физического потолка.
2. **Thunderbolt enclosure A/B:** тот же NIC, тот же firmware, другой enclosure/controller.
3. **Buffer placement:** `malloc`, `posix_memalign`, `IOBufferMemoryDescriptor`, pinned/shared mapping; сравнить registration cost, one-sided bandwidth и p95.
4. **CQ mode:** direct polling, event blocking, hybrid thresholds; записывать wakeups и CPU-seconds.
5. **WQE mode:** ordinary doorbell против Blue Flame; сравнить только при одинаковом PCIe path и размере inline payload.
6. **Batch shape:** 1/2/4/8/16 WR per post и 1/2/4/8 QPs, отдельно для 4 KiB, 64 KiB, 256 KiB, 1 MiB.
7. **Metal handoff:** базовая coherent visibility уже подтверждена hardware
   gate; осталось сравнить staging с финальным inference KV `MTLBuffer` и
   измерить время до фактического consumer visibility, а не только до CQE.
8. **Power/thermal:** warm-up, fixed run order randomization, fan/power state and link retrain logging.

### 6.5 Реалистичная цель на текущем M2

На текущей машине можно с высокой уверенностью добиться:

- RDMA CPU overhead не выше TCP40G за счет direct CQ/UAR, hybrid polling и устранения duplicate KV/staging copy;
- RDMA >= TCP40G по split decode после aggregation of activations и batched WR chains;
- RDMA handoff latency ниже TCP40G в isolated one-sided benchmark.

Нельзя честно гарантировать устойчивый 2x bulk bandwidth-win, пока карта остаётся Gen3 x4 через Thunderbolt. Для этого нужен прямой/wider PCIe path или другая Apple-compatible сетевой платформа.

## 7. Идеи, которые не применяются сейчас

### NVIDIA GPUDirect RDMA / IBGDA / DeepEP

NVIDIA documentation describes GPUDirect RDMA as Linux/NVIDIA peer-memory integration with GPU BAR mappings, memory-ordering requirements and lazy unpinning. DeepEP v2 uses NCCL Gin and requires CUDA/NCCL/NVLink/RDMA environments. These are valid references for the DGX Spark side, but they cannot be implemented on the Mac M2 Metal path by adding a verbs flag.

On this project:

- Mac M2 has no CUDA HBM and no `nvidia-peermem`.
- DriverKit cannot use NVIDIA's Linux peer-memory kernel API.
- GPU-issued UAR/doorbell and GPU-resident CQ require a Metal/PCIe mechanism
  that does not exist; the attempted BAR→Metal mapping is unsafe and closed.
- Apple Silicon's supported equivalent for **data buffers** is implemented:
  register `.shared | .untracked` Metal UMA memory and let the NIC DMA into the
  same physical pages. This is not CUDA peer-memory and does not include a GPU
  doorbell.

A future Linux Spark-side optimization may use GPUDirect/GIN for GPU-to-NIC
traffic, but it will not by itself connect the received bytes to the final Mac
Metal KV layout; that application mapping remains required.

### ODP, SRQ, XRC, UD, adaptive packet spraying

These are not justified for the current one-peer inference flow. ODP adds first-touch faults; SRQ/XRC address many-connection receive management; UD sacrifices RC guarantees; packet spraying risks reordering. Add only after a measured workload requires them.

### SHARP/NVLS

These optimize collectives, not the current point-to-point llama RPC handoff. They are irrelevant unless the application is changed to use distributed collectives.

## 8. Implementation order

### Completed During This Work

- [x] Implemented RPC final-destination descriptor for `GET_TENSOR_RDMA`: address, size, rkey, backend kind and explicit final/stable/fallback flags.
- [x] Implemented bounded `RPC_CMD_BATCH_SEND` framing and ordered server dispatch for fire-and-forget payloads.
- [x] Added command-queue construction for batch `SET_TENSOR` items.
- [x] Exposed `ggml_backend_rpc_batch_send()` through `ggml-rpc.h` and the backend registry for explicit split activation aggregation.
- [x] Added automatic bounded batching of adjacent small `SET_TENSOR` commands on RDMA, with graph/request barriers preserved.
- [x] Bumped RPC minor protocol version to 1 while preserving old framing.
- [x] Connected automatic adjacent-small-`SET_TENSOR` aggregation to the llama command queue with a bounded deadline and ordering barriers.

- [x] Applied one-sided WRITE window overlap in `/Users/macstudio/llama.cpp/ggml/src/ggml-rpc/transport.cpp`: rotate `RDMA_TX_WINDOW` generations and drain the previous signaled window after posting the next one.
- [x] Added llama-side covered-subrange MR reuse for stable final destinations.
- [x] Added llama-side CQ batch drain (up to 16 WCs per poll) with local pending completion retention.
- [x] Reused a thread-local server KV staging buffer to remove per-window allocation.
- [x] Added CQ poll/empty/wakeup/fallback telemetry to `GGML_RPC_RDMA_STATS`.
- [x] Built the current ARM64/Metal `ggml-rpc` target successfully.
- [x] Confirmed the TCP40G comparison is fair for the current experiment because both transports use the same Mac Gen3 x4 physical path.
- [x] Confirmed the current Mac topology: M2 Ultra, Wavlink UTE02 Thunderbolt 3, ConnectX-4 Lx, PCIe 8.0 GT/s x4.
- [x] Added the default coherent Metal/UMA MR contract and verified
  Metal→Spark, Spark→Metal and 4 MiB indirect KLM transfer on DEXT 0.359,
  MTU 4096, with zero GPU mismatches.

### Phase A: prove the current baseline

- Add symmetric TCP40G metrics: ITL p95/mean, CPU-seconds and transport counters.
- Run randomized/interleaved 10 repetitions per transport for 27B disagg and 35B split contexts 512/1024/4096/16384.
- Run isolated KV handoff with no model compute.
- Verify direct CQ/UAR activation counters.

### Phase B: close application-level waste

- Remove duplicate KV transfer.
- Make the final `.shared | .untracked` Metal KV layout the persistent
  registered destination; the driver mechanism is complete, integration is not.
- Remove temporary staging copy where final layout permits.
- Add exact byte accounting and correctness checks.

### Phase C: close split small-message gap

- Aggregate activation messages per decode step.
- Use one WR chain and one completion per aggregate.
- Add multi-SGE path before adding more copies.
- Tune inline threshold from data; start <=128 bytes, test 256 and 512 separately.

### Phase D: close host/DEXT overhead

- Make direct CQ decode the default for the validated llama profile.
- Use active direct polling plus idle event blocking.
- Preallocate all hot-path metadata.
- Confirm no per-message ExternalMethod remains.

### Phase E: hardware validation

- Repeat the full matrix on a direct/wider PCIe path.
- Test two physical rails only after the first rail is saturated.
- Validate RoCE QoS counters under concurrent load.

### Recommended next implementation slice after DEXT 0.359

1. **Final Metal KV MR (highest impact).** Register the existing page-aligned
   shared Metal backend allocation once, publish `{base, length, rkey,
   generation}`, translate validated tensor offsets into that MR, and launch the
   consumer only after the matching WRITE_WITH_IMM token. This removes the Mac
   landing copy and directly exercises the newly verified driver method.
2. **Spark source pipeline.** Replace the per-chunk vector with a persistent
   pinned two-generation pool and overlap `ggml_backend_tensor_get()` with the
   previous RDMA window. If the Spark backend later exposes a supported
   GPUDirect registration, substitute it behind the same source-region API.
3. **Split activation aggregate.** Combine 4/8/16 independently produced RPC
   items under one compact header and one WR chain; retain a latency deadline so
   batching cannot stall a token waiting for a full group.
4. **Active-direct/idle-event CQ policy.** Drain direct CQEs for a bounded active
   budget, then arm and sleep. Add per-request direct/kernel poll, doorbell,
   wakeup and fallback-reason counters before tuning the thresholds.
5. **Defaults only after evidence.** Keep single-owner CQ opt-in for llama's
   shared-QP lease path. Promote direct UAR/CQ from the validated launch profile
   to provider-wide defaults only after mixed-client and fallback telemetry is
   clean.

## 9. Definition of done

RDMA is a demonstrated winner only when all conditions below pass in the same controlled benchmark campaign:

| Gate | Required result |
|---|---|
| Correctness | 0 errors, 0 timeouts, 0 fallback-induced corruption, 0 resource drift |
| 35B split decode | >= TCP40G * 1.05 at context 512 and 1024 |
| Split tail latency | ITL p95 <= TCP40G p95 |
| 27B disagg TTFT | <= TCP40G * 0.99 after isolating compute and measuring handoff separately |
| KV handoff | >= 15% faster than TCP40G, one logical transfer only |
| CPU | application + DEXT + interrupt CPU-seconds/request <= TCP40G |
| Data path | >=99.9% direct CQ/UAR operations, no unexpected kernel fallback |
| Fabric | no unexplained PFC pause, drops, retries, RNR or ECN instability |
| Reproducibility | 10 interleaved repetitions, median, p95 and confidence interval |

If the split gates pass but disagg TTFT does not, RDMA has won the small-message transport case but not the end-to-end disagg case. If handoff wins but total TTFT does not, the system is compute-bound and the transport is no longer the dominant opportunity.

## 10. External research used

- NVIDIA GPUDirect RDMA documentation: https://docs.nvidia.com/cuda/gpudirect-rdma/index.html
  - confirms GPU peer mapping, PCIe topology, memory ordering and lazy unpinning/registration cache requirements.
- NVIDIA NCCL environment documentation: https://docs.nvidia.com/deeplearning/nccl/user-guide/docs/env.html
  - confirms that NCCL exposes topology, algorithm/protocol and network tuning controls rather than treating all RDMA paths as equivalent.
- Linux rdma-core mlx5 provider: https://github.com/linux-rdma/rdma-core/tree/master/providers/mlx5
  - confirms the reference provider structure for CQ, QP, WQE, doorbell, Blue Flame, tracing and evolving out-of-order support.
- DeepEP: https://github.com/deepseek-ai/DeepEP
  - current README confirms Gin/NCCL-based GPU communication, analytical QP/channel sizing, zero/minimal-SM communication and that current support is CUDA/NVIDIA-oriented; it explicitly describes RoCE compatibility as theoretical rather than the primary tested path.
- RDMA technical reference in this repository: `dev/rdma-intro (1).md`, especially sections on selective signaling, batching, WRITE_WITH_IMM, CQ polling, NCCL CTS, GPUDirect ordering, PFC and DCQCN.

## 11. Final conclusion

The next decisive work is not another generic RDMA feature. It is:

1. transfer KV exactly once into its final destination;
2. aggregate split activations so one logical decode step becomes one or a few WR chains;
3. prove that llama uses direct CQ/UAR without a DriverKit call per message;
4. compare CPU-seconds and p95 under a symmetric benchmark;
5. validate on a PCIe path wider than the current Gen3 x4 enclosure.

Without item 5, RDMA can still win on CPU overhead and small-message latency, but a large bulk-bandwidth victory over TCP40G is physically unlikely on this machine. Without items 1--3, the theoretical RDMA advantage is consumed by application staging and host progress overhead before it reaches the wire.
