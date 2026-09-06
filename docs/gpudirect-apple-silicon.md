# GPUDirect на Apple Silicon: результаты реверс-инжиниринга

Дата: 2026-06. Машина исследования: Mac Studio M2 Ultra (76 GPU cores, 192 ГБ, macOS 26.6.1 build 25G76).
Цель: выяснить, возможен ли аналог NVIDIA GPUDirect RDMA (DMA NIC → GPU-память без CPU) на Apple Silicon,
и что это означает для MelonDMA.

## Главный вывод

На Apple Silicon **GPUDirect в смысле "отдельная VRAM за BAR" не существует — он не нужен**.
GPU (AGX) использует **unified memory**: буферы MTLBuffer (MLX/Metal) — это обычная DRAM
системы, и любой CPU-указатель `MTLBuffer.contents()` — это реальный virtual address тех же
страниц, которые GPU читает и пишет. Физической границы "host memory vs device memory" нет.

Поэтому "GPUDirect" на Apple Silicon распадается на два отдельных вопроса:

1. **Может ли NIC (ConnectX) DMA-писать напрямую в страницы GPU-буфера, минуя CPU-копию?**
   Да — это чисто вопрос регистрации памяти в IOMMU. Механизм тот же, что и для CPU-памяти.
2. **Есть ли готовый "GPUDirect-like" путь (dmabuf / IOSurface / nvidia-peermem)?**
   Apple оставила в libibverbs.dylib символ `ibv_reg_dmabuf_mr` (проверено в dyld-кэше этой
   машины), но публичной документации нет, а Apple TB-RDMA (TN3205) вообще не использует
   dmabuf — только `ibv_reg_mr` на page-aligned CPU-буферы. Практического dmabuf-пути на
   macOS сегодня нет; на unified memory он и не нужен.

## Факты, установленные реверсом на этой машине

### Apple Silicon IOMMU = DART
- `ioreg` показывает `IODARTMapper` (≈10 экземпляров) и `iommu-mapper,gfx` — у GPU (AGX)
  **свой отдельный DART** с драйвером `AGXArmFirmwareMapper` (com.apple.AGXG14X).
  Т.е. GPU-адреса транслируются собственным IOMMU-доменом, отдельным от CPU.
- В Linux (Asahi) это же железо: `apple,t8110-dart`, `apple,t6000-dart` (M2 Ultra = T6000)
  — драйвер `drivers/iommu/apple-dart.c` в mainline. DART поддерживает **bypass mode**
  (`supports_bypass`), включается в зависимости от device tree. Следовательно: **в macOS
  DART'ы NIC/Thunderbolt и GPU включены с трансляцией; NIC не может "просто так" взять
  физический адрес страницы без записи в таблицы своего DART**. Именно это делает
  `IODMACommand::PrepareForDMA` / `ibv_reg_mr` — программирует DART нужного устройства.
- Вывод: для DMA "NIC → страницы GPU-буфера" достаточно, чтобы эти страницы были
  зарегистрированы в DART **того же экземпляра, за которым сидит NIC**. На M2 Ultra NIC
  за Thunderbolt (Gen3 x4) сидит за DART Thunderbolt-контроллера. Страницы unified memory
  входят в любой DART одинаково — главное, чтобы маппинг создавался через IODMACommand
  **от PCI-функции NIC**, а не через GPU-интерфейсы.

### Apple's собственный RDMA (TN3205, macOS 26.2+)
- `AppleThunderboltRDMA.kext` есть в `/System/Library/Extensions` на macOS 26.6.1, но это
  **только Info.plist + подпись, без исполняемого кода**; `IORDMAFamily.kext` отсутствует
  (`kmutil showloaded` его не знает). Требует **Thunderbolt 5** — Mac Studio M2 Ultra
  имеет TB4, поэтому Apple RDMA на этой машине недоступен в принципе.
- TN3205 (прочитан из web.archive.org полностью): включается `rdma_ctl enable` в Recovery;
  `ibv_devices` показывает `rdma_en2`; verbs API (`infiniband/verbs.h` + `librdma.tbd` — оба
  есть в SDK этой машины). Ограничения: **только SEND/RECV (2-sided)**, UC transport,
  ≤10 QP, сообщения ≤16 777 120 байт, ≤4095 WR. **RDMA Read/Write (one-sided) нет** —
  т.е. это не GPUDirect-стиль транспорт, и для MelonDMA он не конкурент (наш ConnectX
  умеет полный one-sided RC).
- TN3205 прямо говорит: "each Thunderbolt controller sits behind an IOMMU and thus can
  only access memory mapped specifically for each controller" — подтверждение DART-модели.
- Многочисленные репорты (Anemll 14 µs, Geerling) подтверждают: JACCL/MLX распределённый
  backend использует **bounce buffer** (`page_aligned_alloc` + `ibv_reg_mr`), копируя из
  MTLBuffer через `contents()`. Т.е. zero-copy GPU→RDMA у Apple TB-RDMA нет.

### Драйверный уровень: что уже есть в MelonDMA
- MelonDMA **архитектурно готов** к GPU-буферам: путь `kMlxUCMethodRegMR` принимает
  произвольный VA (8 байт) + длину → `CreateMemoryDescriptorFromClient(kIOMemoryDirectionOutIn
  | kIOMemoryDisableCopyOnWrite, 1, ranges, ...)` → `fRoce->RegMR(...)` → `MlxDMA::Pin(...)`
  → `IODMACommand::PrepareForDMA` → 4 KiB PAS → CREATE_MKEY. Это ровно та последовательность,
  которую требует unified memory. `MTLBuffer.contents()` даёт валидный VA.
- Ограничения, которые придётся снять для больших GPU-буферов:
  - `MLX_MAX_DMA_PAGES = 480` (≈1.875 МиБ) на один прямой MR → GPU-буфер 1 ГБ = 512+ прямых
    MR, либо `kMlxUCMethodRegMRIndirect` (уже есть, ≤32 children).
  - DEXT-путь не имеет доступа к IOSurface как к объекту; доступен только VA от клиента.
    Для VA это не проблема (страницы те же), но **нет атомарного "dereg while GPU still
    uses"**-контракта: драйвер видит обычную память, lifetime должен гарантировать клиент
    (то же правило, что для ibv_reg_mr).
- **Важное отличие от NVIDIA GPUDirect**: в MelonDMA SQ/RQ/CQ живут в host DRAM, doorbell —
  MMIO. GPU (Metal-ядро) не может написать doorbell NIC — нет ни BAR1-доступа, ни
  `cudaHostRegisterIoMemory`-аналога. Значит "GPU сам ring-ует doorbell" (IBGDA/NCCL Gin
  паттерн) на Apple Silicon **невозможен**. Возможен только proxy-поток CPU: GPU выставляет
  флаг/событие в разделяемой памяти, CPU-нить постит WR. Это не GPUDirect в NVIDIA-смысле,
  но это и есть то, что делают JACCL и OdinLink на всех платформах unified memory.

### Смежные проекты (найдено в сети)
- **Anemll/mlx-rdma** (форк MLX) — включил RDMA в MLX-распределёнку: `ibv_roundtrip.cpp`
  (UC, 6 QP → 6.1 ГБ/с на TB5, 12.4 µs @ 4K), `jaccl.cpp` — регистрация CPU-буферов,
  никакого zero-copy GPU.
- **OdinLink-Five** (Geramy, форк johndpope) — TB5 RDMA для Linux (NHI ring DMA), verbs,
  `ibv_reg_dmabuf_mr` для CUDA (Linux), **Mac-kext для macOS** (mac/kext/OdinLinkNHI.cpp:
  NHI MMIO ring, RX-буфер в физически непрерывной памяти → DART). Это работающий пример
  "DART-маппинг для NHI" на macOS, можно брать как референс для DEXT-эквивалента.
- **jonathan308/ThunderMLX / OMLX, todd-chamberlain/jaccl-rdma, wkljohn/ds4-strix-halo-tp-odinlink** —
  кластерные демонстрации; все используют bounce-буфер или DART-зарегистрированные
  shared-буферы, а не прямой доступ GPU-железа к NIC.

## Вывод для MelonDMA (план действий)

**Возможность "GPUDirect" = zero-copy "ConnectX DMA → MTLBuffer" — да, на unified memory
она реализуема штатными средствами DriverKit уже сейчас**, без привилегированных хаков:

1. Клиент (наш userspace или MLX-плагин) аллоцирует `MTLBuffer` (Shared storage mode —
   это дефолт MLX, `buf->contents()` валиден), передаёт VA в `rdma_reg_mr` как обычно.
2. DEXT делает `CreateMemoryDescriptorFromClient` → `IODMACommand::PrepareForDMA` от
   PCI-функции ConnectX → DART NIC получает трансляции → NIC пишет прямо в страницы,
   которые GPU затем читает (нужен правильный flush/barrier — см. ниже).
3. Единственное, чего не будет никогда: GPU не может сам ring doorbell (нет доступа к
   UAR из Metal-ядра и нет CUDA-подобного ioMemory API). Proxy-CPU-нить обязательна.

**Что нужно добавить в MelonDMA, чтобы это реально заработало (по убыванию приоритета):**

- **P2-смежное (малое)**: тест-программа, которая регистрирует MTLBuffer через
  `kMlxUCMethodRegMR` и гоняет RDMA WRITE прямо в `contents()`, затем GPU-ядро читает
  буфер. Это единственный честный способ подтвердить, что macOS пропускает
  `CreateMemoryDescriptorFromClient` на VA MTLBuffer (на текущем коде не проверялось!).
- **P3**: поднять `MLX_MAX_DMA_PAGES` или автоматически деградировать на indirect MR
  (уже есть) при большом MR; включить `IBV_SEND_INLINE` для мелких сообщений.
- **Когерентность**: после приёма данных в MTLBuffer из NIC GPU должен видеть их без
  CPU-flush (unified memory, но скептически проверить — Metal имеет свои кэши GPU).
  Аналог NVIDIA "flush before signaling" здесь: GPU-side fence + `MTLBlitCommandEncoder`
  sync перед чтением.
- **Lifetime**: гарантировать, что MLX не реаллоцирует буфер, пока MR жив. Кэш MR по
  адресу (NCCL reg.cc паттерн) — прямое улучшение.

**Чего НЕ делать:** не пытаться залезть в DART вручную (нет API), не ждать IORDMAFamily
(kext — заглушка), не проектировать под Apple TB-RDMA (только send/recv, TB5-only).

## Источники
- TN3205 "Low-latency communication with RDMA over Thunderbolt" (web.archive.org, 2026-08-27).
- macOS 26.6.1 SDK: `librdma.tbd` (reexports libibverbs/libmlx5/libibmad/libibumad),
  `infiniband/verbs.h`; dyld-кэш экспортирует `ibv_reg_dmabuf_mr` из libibverbs.
- mainline `drivers/iommu/apple-dart.c` (Asahi Linux, t6000/t8110/t8103), m1n1 `src/dart.c`.
- Anemll/mlx-rdma: `mlx/distributed/jaccl/jaccl.cpp`, `ibv_roundtrip.md` (анимелл, 14µs).
- Geramy/OdinLink-Five: `COMPAT.md`, `mac/kext/OdinLinkNHI.cpp` (Mac NHI DMA kext).
- ml-explore/mlx: `mlx/backend/metal/allocator.cpp` (MTLResourceStorageModeShared,
  `Buffer::raw_ptr()` = `contents()`), `mlx/backend/metal/device.cpp`.
- hn.algolia.com: items 46048147, 46248644, 46318352.
- MelonDMA: `src/dext/Sources/MlxUserClient.cpp` (kMlxUCMethodRegMR),
  `src/dext/Sources/core/MlxDMA.cpp/.hpp`, `src/dext/Sources/userclient/MlxUCIO.h`.
