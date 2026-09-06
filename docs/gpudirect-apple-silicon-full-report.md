# GPUDirect на Apple Silicon — полный отчёт исследования

**Ревизия:** 1.0
**Дата:** 2026
**Автор:** исследование выполнено на машине `Mac Studio M2 Ultra`
**Связанный код:** MelonDMA (macOS DriverKit RoCEv2 RDMA driver, ConnectX)
**Краткая версия:** [`docs/gpudirect-apple-silicon.md`](gpudirect-apple-silicon.md)

---

## 1. Резюме (TL;DR)

Вопрос исследования: **можно ли сделать аналог NVIDIA GPUDirect RDMA (DMA NIC → GPU-память без участия CPU) на Apple Silicon ARM M-чипах, и что это даёт MelonDMA.**

Ответ, в двух словах:

1. **Классический GPUDirect на Apple Silicon невозможен и не нужен.** На NVIDIA-системах GPUDirect решает задачу "NIC не может DMA-писать в чужую VRAM за BAR другой PCIe-функции". На Apple Silicon GPU (AGX) использует **unified memory** — те же физические страницы DRAM, что и CPU. Буфер `MTLBuffer` (в MLX это `StorageModeShared`) — это обычная системная память с валидным CPU-указателем `contents()`. Отдельной VRAM/BAR-границы просто нет.

2. **Zero-copy «NIC → GPU-буфер» на Apple Silicon достижим штатными средствами DriverKit уже сейчас.** Единственное требование — зарегистрировать страницы GPU-буфера в IOMMU (DART) той PCI-функции, за которой сидит NIC. MelonDMA делает ровно это: `RegMR` → `CreateMemoryDescriptorFromClient(VA)` → `IODMACommand::PrepareForDMA` → PAS → MKEY. Ничто в этом пути не проверяет, «CPU-это память» или «GPU-это память» — потому что на unified memory это одно и то же.

3. **GPU никогда не сможет сам звонить в doorbell NIC.** У Apple нет аналога `cudaHostRegisterIoMemory`/`cudaHostGetDevicePointer` — Metal-ядро не может отобразить UAR/MMIO в своё адресное пространство. Значит паттерн «GPU сам строит WQE и ring-ует doorbell» (NCCL Gin / IBGDA / DeepEP) на Apple Silicon **невозможен**; обязателен CPU-proxy-поток.

4. **Apple'овский RDMA (TN3205, Thunderbolt 5)** — отдельный стек, не конкурент и не помощник: он **только send/recv** (без one-sided RDMA Read/Write), **только UC**, **только TB5**, и на исследованной машине kext — **заглушка без бинарника** (нет `IORDMAFamily.kext`).

5. **Что реально нужно MelonDMA для «GPUDirect»:** (a) живой тест регистрации `MTLBuffer.contents()` через существующий `kMlxUCMethodRegMR` (не проверялось!); (b) поднять потолок `MLX_MAX_DMA_PAGES=480` (~1.9 МиБ) или автоматически использовать уже имеющийся indirect MR для гигабайтных тензоров; (c) правильная модель когерентности (GPU-fence после RDMA WRITE); (d) MR-кэш по адресу (паттерн NCCL `reg.cc`).

---

## 2. Аппаратная платформа исследования

Собрано командой `system_profiler SPDisplaysDataType SPHardwareDataType SPThunderboltDataType`.

| Параметр | Значение |
|---|---|
| Модель | Mac Studio (Mac14,14) |
| SoC | Apple M2 Ultra (t602x) |
| CPU | 24 ядра (16 Performance + 8 Efficiency) |
| GPU | 76 ядер, Vendor Apple (0x106b), **Metal 4** |
| Память | 192 ГБ unified memory |
| macOS | 26.6.1, build 25G76 |
| Firmware | 18000.161.9 |
| Thunderbolt | USB4/TB4, «Up to 40 Gb/s», 5 шин (buses 1–5), порты пусты |

**Важное следствие:** на этой машине **Thunderbolt 4**, а не 5. Apple RDMA-over-Thunderbolt (TN3205) требует TB5 — то есть на исследованной машине он недоступен в принципе, независимо от состояния kext. MelonDMA работает через ConnectX в Thunderbolt-корпусе (Gen3 x4), что даёт практический потолок ~25–28 Гбит/с (измерено ранее, см. `docs/research.md`).

---

## 3. Почему GPUDirect-проблема «испаряется» на unified memory

### 3.1 Что решает NVIDIA GPUDirect RDMA

На дискретных NVIDIA-системах память GPU — отдельный физический пул (HBM/GDDR), доступный хосту только через окно BAR. Без GPUDirect путь «NIC → GPU» выглядит так:

```
NIC DMA → host DRAM (bounce) → CPU memcpy → PCIe BAR → VRAM
```

GPUDirect RDMA устраняет bounce и memcpy, позволяя NIC писать прямо в BAR-память GPU:

```
NIC DMA → PCIe P2P → BAR1 (VRAM)
```

Для этого нужны: покрытие всей VRAM окном BAR1 (ReBAR), отсутствие/обход ACS, физические адреса страниц GPU (nv_p2p_get_pages / nvidia-peermem / DMA-BUF), синхронизация после DMA (flush перед сигналом — GDRCopy/BAR1-read трюк).

### 3.2 Что на Apple Silicon

На M-чипах GPU и CPU делят один физический адресный пространство DRAM (unified memory). Тензор в MLX — это `MTLBuffer` в `MTLResourceStorageModeShared`, и:

```cpp
// ml-explore/mlx: mlx/backend/metal/allocator.cpp
constexpr size_t resource_options =
    MTL::ResourceStorageModeShared | MTL::ResourceHazardTrackingModeUntracked;

void* Buffer::raw_ptr() {
  auto* buf = static_cast<MTL::Buffer*>(ptr_);
  assert(buf->storageMode() != MTL::StorageModePrivate);
  return buf->contents();   // ← настоящий CPU-адрес тех же страниц, что читает GPU
}
```

То есть «device memory» = «host memory». Значит **единственная** преграда между NIC и GPU-буфером — это IOMMU: NIC (как любой PCIe-мастер) не может адресовать физическую память напрямую, он должен пройти через таблицы DART. И эту преграду снимает обычная регистрация памяти в DART.

### 3.3 Таблица соответствия NVIDIA → Apple

| NVIDIA GPUDirect | Apple Silicon |
|---|---|
| Отдельная VRAM за BAR | Нет VRAM, unified memory |
| ReBAR / BAR1 покрытие | Не нужно (нет BAR) |
| ACS / PCIe P2P между NIC и GPU | Нет (GPU не PCIe-функция) |
| nvidia-peermem / DMA-BUF импорт | `IODMACommand` → DART NIC |
| `cudaHostRegisterIoMemory` (GPU doorbell) | **Нет аналога — GPU не может ring doorbell** |
| Flush перед сигналом (GDRCopy) | GPU-side fence + Metal sync (см. §8.4) |

---

## 4. IOMMU на Apple Silicon: DART (реверс)

### 4.1 Что видно в ioreg на этой машине

DART — Apple'овский IOMMU (Device Address Resolution Table). В `ioreg` он представлен классом `IODARTMapper`:

```
IONameMatch  = "iommu-mapper"
IOClass      = "IODARTMapper"
IOProviderClass = "IODARTMapperNub"
IOUserClientClass = "IODARTMapperClient"
iommu-dart-translation = Yes
iommu-initial-translations = <00000000000100000040940100010000>
iommu-parent = "IODARTMapper is not serializable"
```

Экземпляров `IODARTMapper` около десятка (id `0x1000004b6` … `0x100000536`), у каждого свой `IOMapperID`. Плюс для каждого PCIe-мастера прошивается собственный `IOMapperID` (stream id):

```
IOMapperID = <13010000>  <19010000>  <1f010000>  <25010000>  <2b010000>  <31010000> ...
```

**GPU имеет отдельный DART-домен** — это важнейший факт:

```
mapper-gfx-asc  (AppleARMIODevice)
  compatible = "iommu-mapper,gfx"
  AAPL,phandle = fb010000
  +-o AGXArmFirmwareMapper  (IOClass AGXArmFirmwareMapper, com.apple.AGXG14X, IOMapperID fb010000)
```

То есть у GPU (AGX) есть свой mapper/IOMMU-домен, отдельный от CPU и от Thunderbolt. Это соответствует Linux-дереву устройств Asahi.

Другие наблюдаемые узлы:

- `APCIECMSIController-apciec0…5` — 6 контроллеров PCIe (Apple PCIe Controller), `compatible = "apciec,t8103"` (строка совместимости t8103!).
- `apcie-piodma`, `apcie-piodma-sid`, `apcie-port`, `apcie-config-tunables` — PCIe controller tunables.
- `mca-switch@9B600000`, `compatible = "mca-switch,t602x"` — подтверждает, что M2 Ultra это t602x.
- `IOThunderboltXDomainServiceClientManager` ×5 — по одному на Thunderbolt-шину; это точка матчинга Apple TB-RDMA.

Kext'ы DART в `/System/Library/Extensions`:

```
AppleT6000DART.kext   (M1 Pro/Max/Ultra)
AppleT8020DART.kext   (M1)
AppleT8110DART.kext   (M2-семейство, базовый DART)
IODARTFamily.kext
SharedDARTMapperProxy.kext
AppleThunderboltRDMA.kext
```

`kmutil showloaded` показывает загруженными `com.apple.driver.IODARTFamily` и `com.apple.driver.AppleT8110DART`.

### 4.2 Аппаратный регистровый уровень (m1n1, Asahi)

Asahi Linux отреверсил DART до регистров. Ключевые выдержки из `src/dart.c` (m1n1):

**T8020-поколение (M1/M1 Pro/Max/Ultra):**

```
DART_T8020_CONFIG 0x60   (LOCK = BIT(15))
DART_T8020_ERROR  0x40   (STREAM_SHIFT 24, READ_FAULT bit4, WRITE_FAULT bit3,
                          NO_PTE bit2, NO_PMD bit1, NO_TTBR bit0)
DART_T8020_STREAM_SELECT 0x34
DART_T8020_STREAM_COMMAND 0x20  (BUSY bit2, INVALIDATE bit20)
DART_T8020_STREAM_REMAP 0x80
DART_T8020_ENABLED_STREAMS 0xfc
DART_T8020_TCR_OFF 0x100   (TRANSLATE_ENABLE bit7, BYPASS_DART bit8, BYPASS_DAPF bit12)
DART_T8020_TTBR_OFF 0x200  (VALID bit31, ADDR[30:0], SHIFT 12)
```

**Формат PTE (запись таблицы страниц):**

```
DART_PTE_OFFSET_SHIFT  14          (страница DART = 16 КиБ)
DART_PTE_SP_START      GENMASK(63,52)
DART_PTE_SP_END        GENMASK(51,40)
DART_T8020_PTE_OFFSET  GENMASK(39,14)
DART_T6000_PTE_OFFSET  GENMASK(39,10)
DART_T8020_PTE_DISABLE_SP BIT(1)
DART_T6000_PTE_REALTIME   BIT(1)
DART_PTE_VALID            BIT(0)
```

**T8110-поколение (M2+):**

```
DART_T8110_TTBR_OFF 0x1400  (VALID bit0, ADDR[29:2], SHIFT 14)
DART_T8110_TCR_OFF  0x1000  (REMAP[11:8], REMAP_EN bit7, BYPASS_DAPF bit2,
                             BYPASS_DART bit1, TRANSLATE_ENABLE bit0)
DART_T8110_TLB_CMD  0x80    (BUSY bit31, OP[10:8], FLUSH_ALL 0, FLUSH_SID 1, STREAM[7:0])
DART_T8110_PROTECT  0x200   (TTBR_TCR bit0)
DART_T8110_ENABLE_STREAMS  0xc00
DART_T8110_DISABLE_STREAMS 0xc20
DART_MAX_TTBR_COUNT 4
```

### 4.3 Linux-драйвер mainline (`drivers/iommu/apple-dart.c`)

Apple DART поддерживается в mainline Linux (авторство Sven Peter / Asahi). Из драйвера:

- DART — «обязательный слой трансляции адресов для различных мастеров».
- Каждый экземпляр DART: **до 16 потоков (streams)**, у каждого **своя таблица страниц** и побитовые флаги защиты R/W; счётчик ошибок генерирует прерывание.
- **Поддержка bypass-режима** (`supports_bypass`, `tcr_bypass`, `apple_dart_hw_enable_bypass`): в bypass DART пропускает адреса без трансляции. Включается на основании device tree (`DART_PARAMS2_BYPASS_SUPPORT`).
- Compatible-строки: `apple,t8103-dart`, `apple,t8103-usb4-dart`, `apple,t8110-dart`, `apple,t6000-dart`, `apple,t6020-dart` (совместим с t8110), `apple,t8122-dart`.

**Вывод из реверса DART:** на macOS NIC за Thunderbolt работает за DART с трансляцией (iommu-dart-translation = Yes), GPU — за собственным gfx-DART. «GPUDirect» сводится к записи правильных PTE в таблицу DART *NIC-мастера*, указывающих на физические страницы GPU-буфера. Это делает `IODMACommand`, и страницы unified memory входят в любой DART одинаково — GPU-домен и NIC-домен могут указывать на одни и те же физические страницы одновременно (это нормальная ситуация: так работает любой буфер, который читают и CPU, и устройство).

---

## 5. Apple'овский RDMA over Thunderbolt (TN3205)

### 5.1 Что это

Apple опубликовала **TN3205 «Low-latency communication with RDMA over Thunderbolt»** (первая публикация 2026-03-19, правка 2026-04-13). Прочитан полный текст (через web.archive.org). Суть:

- RDMA-over-Thunderbolt доступен **начиная с macOS 26.2**, на Apple Silicon **с Thunderbolt 5**.
- Включается в macOS Recovery: `Terminal` → `rdma_ctl enable` → перезагрузка.
- Проверка: `ibv_devices`; интерфейсы называются `rdma_en2`, `rdma_en3` … (парные к TB-IP интерфейсам `en2`, `en3`…).
- Работает параллельно с IP-over-Thunderbolt; железо балансирует между протоколами.
- Топология: **только point-to-point**, маршрутизация запрещена; полный mesh, кольцо — forwarding на приложении. Для кольца из 5 узлов приведены примеры.
- MLX «co-developed» — распределённый MLX должен работать из коробки.
- API: `#include <infiniband/verbs.h>` + линковка `librdma.tbd`.

**Жёсткие ограничения (цитаты из TN3205):**

- «RDMA over Thunderbolt supports **Send and receive operations only**» — **односторонних RDMA Read/Write нет**.
- «A maximum of **10 unreliable connection (UC) queue pairs**».
- «Message sizes of up to **16 777 120 bytes**».
- «A maximum of **4095 work requests** at a time».
- «RDMA over Thunderbolt only supports **2-sided operations** … register memory only as `IBV_ACCESS_LOCAL_WRITE`».
- Глубина очереди меряется в кадрах 4 КиБ (например, глубина 1024 → max 4 194 304 байт).
- «Thunderbolt devices require a receiver and sender to post messages which are **the same number of frames long**» — рассогласование размера recv/send = сбой.
- Кредитная flow-control, **аппаратных ACK нет**: «Completion … does not indicate the receiver has successfully received the sent data or that data was not corrupted in flight».

### 5.2 Реверс kext на исследованной машине

`/System/Library/Extensions/AppleThunderboltRDMA.kext` присутствует, но содержит **только**:

```
Info.plist
_CodeSignature/
version.plist
```

**Бинарника нет вообще** (нет каталога `Contents/MacOS/`). `codesign` сообщает «code object is not signed at all». Это **заглушка**.

`Info.plist` (полностью прочитан):

- `CFBundleIdentifier = com.apple.driver.AppleThunderboltRDMA`
- `CFBundleVersion = 0.0.1`, `LSMinimumSystemVersion = 26.6`
- `DTSDKName = macosx26.6.internal` — kext собран **внутренним** SDK.
- Две personality:
  - `AppleThunderboltRDMAInterface`: `IOClass AppleThunderboltRDMAInterface`, `IOProviderClass IOEthernetInterface`, `IOParentMatch.IOProviderClass AppleThunderboltIPPort`, `IOPropertyExistsMatch "BSD Name"`.
  - `AppleThunderboltRDMAPeerInterface`: `IOClass AppleThunderboltRDMAPeerInterface`, `IOProviderClass IOThunderboltXDomainService`, `IOPropertyMatch { Protocol ID = 64087, Protocol Version = 1 }`.
- `OSBundleLibraries` требует:
  ```
  com.apple.iokit.IONetworkingFamily 3.4
  com.apple.iokit.IOPCIFamily       2.0
  com.apple.iokit.IORDMAFamily      1.0
  com.apple.iokit.IOThunderboltFamily 9.3.3
  kpi.bsd / kpi.iokit / kpi.libkern / kpi.mach 11.0
  ```

**`IORDMAFamily.kext` отсутствует** в `/System/Library/Extensions` на macOS 26.6.1. Без него kext не может загрузиться. Это согласуется с репортом OdinLink-Five: «Apple ships `libthunderboltrdma.dylib` + `libibverbs` on macOS 26.5, but the kernel extension is a stub — `IORDMAFamily` is not shipped. Mac Thunderbolt RDMA is not currently functional.»

**Протокол:** Apple использует XDomain property `Protocol ID = 64087 (0xFA57)`, `Protocol Version = 1`. Транспорт: `IORDMAFamily` (NHI DMA rings) + `IOThunderboltFamily` (XDomain control).

### 5.3 Что лежит в SDK и dyld-кэше

В SDK этой машины (`MacOSX.sdk`):

- `/usr/lib/librdma.tbd` — install-name `/usr/lib/librdma.dylib`, реэкспортирует:
  ```
  /usr/lib/rdma/libibmad.dylib
  /usr/lib/rdma/libibumad.dylib
  /usr/lib/rdma/libibverbs.dylib
  /usr/lib/rdma/libmlx5.dylib
  ```
- `libibmad` экспортирует полный набор MAD (`bm_call_via`, `mad_alloc`, `ib_path_query`, `mad_dump_*` …).
- `libibverbs` экспортирует среди прочего: `ibv_cmd_reg_mr`, `ibv_cmd_reg_dmabuf_mr`, **`ibv_reg_dmabuf_mr`**, `ibv_reg_mr`, `ibv_reg_mr_iova2`, `ibv_create_qp`, `ibv_create_srq`, `ibv_rereg_mr`, …
- `/usr/include/infiniband/verbs.h` — стандартный verbs.h. Любопытно: в header объявлены `ibv_reg_mr`, `ibv_reg_mr_iova2`, `reg_dm_mr` (device memory), но **`ibv_reg_dmabuf_mr` в header НЕ объявлен** — только экспортирован из dylib. То есть это скрытый/полуофициальный символ.

Проверка на живой машине (dlopen из dyld-кэша, файлов на диске нет — `/usr/lib/rdma/` пуст, dylib'ы живут в shared cache):

```
libibverbs.dylib: ibv_reg_dmabuf_mr  EXPORTED
                  ibv_reg_mr         EXPORTED
                  ibv_get_device_list EXPORTED
                  ibv_open_device    EXPORTED
                  ibv_cmd_reg_mr     EXPORTED
libthunderboltrdma.dylib: НЕ найден
```

Бинарники в `/usr/bin`: `ibv_devices`, `ibv_devinfo`, `ibv_uc_pingpong`, `rdma_ctl`.

Запуск на этой машине (без TB5, без работающего kext):

```
$ ibv_devices
    device        node GUID
    ------        ----------------
(пусто)  →  "No IB devices found"
```

### 5.4 Вывод по Apple RDMA

Apple'овский стек — **отдельная от MelonDMA технология**, нацеленная на Mac-to-Mac кластеры (MLX/JACCL). Для GPUDirect-вопроса он не даёт ничего: ни one-sided RDMA, ни dmabuf-регистрации GPU-памяти, ни доступа к doorbell из GPU. Плюс на исследованной машине он просто не работает (TB4, kext-заглушка). Единственная ценность для нас — **подтверждение архитектурной модели**: «each Thunderbolt controller sits behind an IOMMU and thus can only access memory mapped specifically for each controller» (прямая цитата TN3205).

---

## 6. Экосистема: кто что уже сделал (поиск в сети)

### 6.1 Anemll/mlx-rdma (форк MLX)

Автор HN-поста «RDMA over Thunderbolt 5 on Apple Silicon – 14µs latency». Содержимое:

- `ibv_roundtrip.cpp` + `ibv_roundtrip.md` — бенчмарк Apple TB-RDMA.
- `mlx/distributed/jaccl/jaccl.cpp` — интеграция JACCL с RDMA.

**Ключевые факты из `ibv_roundtrip.md`:**

- Apple TB-RDMA экспонирует **только UC/UD**, создание RC возвращает `EOPNOTSUPP`. Используется UC.
- LID всегда `0x0001`, нужна глобальная маршрутизация; GID ищется среди IPv4-mapped (`::ffff:x.x.x.x`).
- `ibv_query_device` возвращает крошечные лимиты (≈11 QP/CQE).
- Буферы page-aligned ≥4 КиБ; флаги регистрации `LOCAL_WRITE | REMOTE_READ | REMOTE_WRITE` обязательны.
- Типичные ошибки: `status=60` на RTR (забыт GRH), `Bad address -14` на `ibv_post_recv` (регистрация после переходов QP).

**Измерения (флуд 4 КиБ, 1M итераций):**

| QP | ГБ/с | Гбит/с | Латентность | msg rate |
|---|---|---|---|---|
| 1 | 2.77 | 22.2 | 12.36 µs | 338K/s |
| 2 | 4.75 | 38.0 | 32.3 µs | 579K/s |
| 4 | 5.77 | 46.1 | 52.3 µs | 704K/s |
| 7 | 6.14 | **49.1** | 85.2 µs | 745K/s |

**JACCL (`jaccl.cpp`)** регистрирует **CPU-буферы** через `posix_memalign` + `ibv_reg_mr(pd, data_, num_bytes_, LOCAL_WRITE|REMOTE_READ|REMOTE_WRITE)`, по одному MR на PD. То есть это **bounce buffer**, а не zero-copy GPU. Что подтверждает: на Apple TB-RDMA GPUDirect-пути нет.

### 6.2 OdinLink-Five (Geramy, форк johndpope)

«Thunderbolt 5 RDMA for Linux — kernel driver, libibverbs provider, NCCL/RCCL plugins». Оборачивает TB-кабель в RDMA-интерконнект. Прогресс: kernel-модуль `odl_tb5.ko` (NHI ring DMA, XDomain handshake), userspace-либа, verbs-провайдер, rdma-core-плагин, NCCL/RCCL net-плагин — всё «зелёное».

- **Zero-copy GPU на Linux:** `ibv_reg_dmabuf_mr` через DMA-buf (CUDA: `cuMemGetHandleForAddressRange`); RCCL/NCCL net-плагины.
- **Архитектура:** `app → libibverbs → libodl_tb5-rdmav34.so → libodl_tb5.so → odl_tb5.ko → TB5 NHI DMA engine`.
- **Cross-platform (COMPAT.md):** подтверждает, что у Apple TB-RDMA kext — заглушка (нет `IORDMAFamily`); протокол Apple `64087 (0xFA57)`, OdinLink `20236 (0x4F4C)`; параметр `protocol=1` заставляет OdinLink рекламировать Apple'овский протокол для Mac↔Linux.
- **Mac-часть** (`mac/kext/OdinLinkNHI.cpp`, `OdinLinkRDMA.h`): IOKit kext, маппит ACIO NHI MMIO (`getDeviceMemoryWithIndex(0)`), постит 4 КиБ RX-дескрипторы, поллит consumer index. Буфер — **физически непрерывная память, чтобы DART мог замапить её для DMA-движка NHI**; userspace видит те же физические страницы (zero copy). Комментарий автора: «map … inferred from AppleThunderboltNHI and has not been proven on silicon».
- Также репозитории: `wkljohn/ds4-strix-halo-tp-odinlink` (DeepSeek V4 tensor-parallel на 2× AMD Strix Halo через OdinLink или RoCEv2), `todd-chamberlain/jaccl-rdma`, `jonathan308/ThunderMLX` (→ OMLX, 428B MoE на двух Mac), `tmc/gojaccl`, `alexziskind1/mlx-jaccl-cluster` (MLX JACCL кластер + OpenAI-совместимый HTTP-сервер), `dev-tb5-tester/rdma_jaccl` (bug-report: JACCL неправильно обрабатывает разные размеры send/recv подряд), `mps-ddp/mccl` («Native Pytorch distributed training backend for Apple Silicon»).

### 6.3 HN-обсуждения

- **46048147** «RDMA over Thunderbolt 5 on Apple Silicon – 14µs latency» — anemll: macOS 26.2 beta добавил low-latency TB5 RDMA драйвер, 80 Гбит/с bidirectional, 14 µs RTT (ibv_uc_pingpong, 4K), custom C++ 6–13 µs/iter; M4 Pro ↔ M3 Ultra.
- **46248644** «macOS 26.2 enables fast AI clusters with RDMA over Thunderbolt» — комментарии: MLX-команда публикует multi-Mac запуски (Kimi K2 Thinking 1T, DeepSeek R1 671B); «M5 Ultra с TB5 может быть контендером»; «6 Mac Studio M3 Ultra = 3 ТБ unified memory в full-mesh».
- **46318352** — Geerling: видео «Apple didn't have to go this hard — Testing LLMs using RDMA».
- **47469070** — mccl (PyTorch distributed backend для Apple Silicon).

---

## 7. GPU-память и доступ к ней из драйвера

### 7.1 Metal: `contents()` и режимы хранения

На Apple Silicon:

- `MTLResourceStorageModeShared` — CPU и GPU видят одни страницы, `contents()` валиден. Это дефолт MLX.
- `MTLResourceStorageModePrivate` — GPU-only, `contents()` = NULL; `Buffer::raw_ptr()` в MLX явно `assert(storageMode != Private)`.
- `MTLResourceStorageModeManaged` — на Apple Silicon ведёт себя как Shared (managed актуален только для дискретных GPU Mac Pro).

Следовательно, **для GPUDirect-стиля регистрации годится любой буфер Shared/Managed**, что покрывает всё, что MLX реально отдаёт наружу.

### 7.2 IOSurface

IOSurface — механизм межпроцессного/меж-стека обмена GPU-буферами на macOS:

- `IOSurfaceCreate`, `IOSurfaceLock`, `IOSurfaceGetBaseAddress` → CPU-адрес тех же страниц.
- `IOSurfaceCreateMachPort` / `IOSurfaceLookupFromMachPort` — передача поверхности между процессами.
- Для DEXT важно: **MelonDMA работает с VA** (`CreateMemoryDescriptorFromClient`), а IOSurface даёт валидный VA после `IOSurfaceGetBaseAddress`. Объект IOSurface как таковой драйверу не нужен — нужен VA, который IOSurface предоставляет. Это и есть обходной путь для поверхностей, которые не являются обычной malloc-памятью.

### 7.3 Metal I/O (MTLIOCommandQueue / MTLIOCommandBuffer)

В SDK (macOS 13+): `MTLIOCommandQueue` / `MTLIOCommandBuffer` — «читает из handle-объектов и пишет в MTLResource». Это Apple'овский примитив **GPU-инициируемого ввода-вывода** (в первую очередь для файлов/SSD). Отношения к RDMA не имеет, но важен как прецедент: Apple уже умеет давать GPU копировать из/в устройство напрямую. Для RDMA-двербелла он бесполезен — GPU всё равно не может писать MMIO NIC.

### 7.4 Metal 4 (новое поколение API)

В SDK этой машины появился слой `MTL4*` (Metal 4): `MTL4CommandQueue`, `MTL4ComputeCommandEncoder`, `MTL4MachineLearningCommandEncoder`, `MTL4Compiler` и т.д. Плюс `MTLAllocation.h` (macOS 15+): протокол `MTLAllocation` с `allocatedSize`. Это новое поколение API, не изученное в рамках данного исследования; на модель unified memory не влияет.

---

## 8. Что уже есть в MelonDMA (анализ кода)

### 8.1 Путь регистрации памяти (готов для GPU-буферов)

`src/dext/Sources/MlxUserClient.cpp`, case `kMlxUCMethodRegMR`:

```cpp
IOAddressSegment ranges[32] = {};
ranges[0].address = req->startAddr;   // произвольный VA от клиента (8 байт)
ranges[0].length  = req->length;
IOMemoryDescriptor *clientMemory = NULL;
kern_return_t r = CreateMemoryDescriptorFromClient(
    kIOMemoryDirectionOutIn | kIOMemoryDisableCopyOnWrite,
    1, ranges, &clientMemory);
...
r = ivars->fRoce->RegMR(&raw, clientMemory, resp);
```

Дальше по стеку: `MlxRoCE::RegMR` → `MlxDMA::Pin` → `IODMACommand::Create` + `PrepareForDMA` → IOVA-сегменты → 4 КиБ PAS → `CREATE_MKEY`. Это **ровно** то, что нужно для zero-copy в unified memory: страницы `MTLBuffer.contents()` — обычные страницы задачи клиента, `CreateMemoryDescriptorFromClient` принимает их без разбора «CPU/GPU».

**Ключевое наблюдение:** ни один элемент этого пути не отличает «host-память» от «GPU-памяти». На unified memory это одно и то же. Значит с точки зрения регистрации MR MelonDMA **уже поддерживает GPUDirect-стиль zero-copy** — при условии, что клиент передаст `MTLBuffer.contents()` как VA. Это **не проверялось** на живой системе (см. §9).

### 8.2 Ограничения, которые надо снять для больших GPU-буферов

- `MLX_MAX_DMA_PAGES = 480` → прямой MR покрывает ~480×4 КиБ ≈ **1.875 МиБ**. Гигабайтный тензор = 512+ прямых MR, либо `kMlxUCMethodRegMRIndirect` (уже реализован, но `MLX_UC_MAX_INDIRECT_CHILDREN = 32`).
- Для MLX-тензоров (гигабайты) нужен либо поднятый лимит, либо автодеградация на indirect/chunked MR. Логика уже есть (`RegMRIndirect`), лимиты надо пересмотреть.

### 8.3 Doorbell: GPU не может его трогать

В MelonDMA doorbell — MMIO-запись в UAR (`MlxUAR`). UAR отображается в процесс через `IOConnectMapMemory`/`CopyClientMemoryForType`. GPU (Metal-ядро) не имеет ни одного механизма отобразить MMIO/UAR в своё адресное пространство — аналога `cudaHostRegisterIoMemory` нет. Следовательно:

- **GPU никогда не сможет сам постнуть WR** (NCCL Gin / IBGDA / DeepEP паттерн недостижим на Apple Silicon).
- Обязателен CPU-proxy: GPU пишет флаг/значение в shared-буфер → CPU-нить (event-driven или poll) вызывает `rdma_post_send`. Это не GPUDirect в NVIDIA-смысле, но это ровно то, что делают JACCL и OdinLink на всех unified-memory платформах.

### 8.4 Когерентность

Вопрос, который предстоит проверить живым тестом: после RDMA WRITE в `MTLBuffer` увидит ли GPU данные без CPU-flush?

- На unified memory CPU↔GPU когерентны на уровне физических страниц (нет раздельных кэш-доменов как на дискретных GPU).
- Но у Metal есть собственный GPU-кэш; аналог NVIDIA «flush before signaling» здесь — GPU-side `MTLFence` + `MTLBlitCommandEncoder` sync (или `MTLCommandBuffer` completion) перед чтением GPU-ядром. Для записи GPU → чтение NIC: завершение `MTLCommandBuffer` гарантирует видимость для CPU/устройств.
- Практический приём: использовать `MTLSharedEvent` для синхронизации CPU-proxy с GPU, а NIC-запись сигнализировать уже после CQE.

---

## 9. Что делать MelonDMA (план)

По убыванию приоритета:

1. **Живой тест (P0-критично, дёшево).** Тестовая программа: аллоцировать `MTLBuffer` (Shared), передать `contents()` в существующий `rdma_reg_mr`, выполнить RDMA WRITE от пира прямо в буфер, затем GPU-ядром прочитать и сверить байты. Это единственный способ подтвердить, что macOS реально пропускает `CreateMemoryDescriptorFromClient` на VA MTLBuffer. Если тест пройдёт — «GPUDirect» для MelonDMA уже существует.

2. **Поднять потолок MR.** `MLX_MAX_DMA_PAGES=480` → для тензоров ≥2 МБ использовать indirect MR (готов) или поднять лимит + добавить chunking в userspace.

3. **Когерентность/сигнализация.** Задокументировать и закодировать контракт: GPU-запись → `MTLCommandBuffer` complete → CPU-proxy ring doorbell; приём → CQE → `MTLSharedEvent` → GPU читает. Добавить пример с `MTLBlitCommandEncoder` sync при необходимости.

4. **MR-кэш по адресу** (паттерн NCCL `reg.cc`): key = page-aligned base, subset-match → reuse lkey/rkey. Регистрация больших GPU-буферов дорогая (5–20 мс), кэш даст многократный выигрыш на повторных send/recv одних и тех же тензоров.

5. **Inline + unsignaled batching** — уже в плане P3 проекта, для мелких сообщений.

**Чего НЕ делать:**

- Не лезть в DART вручную (нет публичного API; `IODMACommand` — правильная точка).
- Не ждать `IORDMAFamily` (kext-заглушка).
- Не проектировать под Apple TB-RDMA (send/recv-only, TB5-only, не наш транспорт).
- Не пытаться дать GPU doorbell (физически невозможно на Apple Silicon).

---

## 10. Сводка производительности (контекст)

| Система | Путь | Показатель |
|---|---|---|
| MelonDMA (ConnectX, TB Gen3 x4) | RoCEv2 one-sided | ~20–23 Гбит/с, потолок ~25–28 Гбит/с |
| MelonDMA (kernel-mediated) | — | ~73 µs RTT |
| Apple TB5 RDMA (Anemll, 1 QP) | send/recv UC | 12.36 µs, 22.2 Гбит/с |
| Apple TB5 RDMA (Anemll, 7 QP) | send/recv UC | 49.1 Гбит/с |
| Apple TB5 RDMA (заявлено) | — | 80 Гбит/с bidirectional |
| OdinLink TB5 (заявлено) | verbs + NCCL/RCCL | 80 Гбит/с, sub-µs latency |
| NVIDIA GPUDirect RDMA (для сравнения) | NIC↔GPU P2P | до сотен Гбит/с (зависит от карт) |

**Важно:** на unified memory zero-copy не даёт прироста пропускной способности (бутылочное горлышко — Thunderbolt/PCIe, а не копирование). Выигрыш — **латентность** (нет bounce-копии) и **свобода CPU/памяти** (нет двойного буфера). Это видно и в vLLM-практике для NVIDIA: GPUDirect даёт заметный выигрыш при большом TP/межнодевом трафике, скромный при TP=1.

---

## 11. Открытые вопросы

1. **Пропускает ли `CreateMemoryDescriptorFromClient` VA MTLBuffer?** (главный, решается живым тестом).
2. **Когерентность GPU-кэша после RDMA WRITE без CPU-flush** — нужно замерить на реальном буфере.
3. **Поведение `kIOMemoryDisableCopyOnWrite`** на Metal-памяти (COW-семантика у `contents()` отсутствует, но стоит убедиться, что страницы не реаллоцируются при `PrepareForDMA`).
4. **Лимит DART на количество потоков** (16 streams на экземпляр DART в Linux) — не упирается ли Thunderbolt-контроллер при многих QP/клиентах.
5. **Metal 4 (`MTL4*`, `MTLAllocation`)** — не исследован; потенциально новый способ аллокации/разделения GPU-памяти.
6. **Реальные цифры zero-copy** (bounce vs direct) на этой машине после теста №1.

---

## 12. Полный список источников

**Официальные:**

- TN3205 «Low-latency communication with RDMA over Thunderbolt» — developer.apple.com (архив web.archive.org, снимок 2026-08-27).
- macOS SDK 26.6.1 (на машине): `librdma.tbd`, `/usr/include/infiniband/verbs.h`, `DriverKit/IODMACommand.h`, `DriverKit/IOUserClient.h`, `Metal/MTLIOCommandQueue.h`, `Metal/MTLIOCommandBuffer.h`, `Metal/MTLAllocation.h`, `IOSurface/IOSurfaceRef.h`.

**Реверс железа (Asahi Linux):**

- `AsahiLinux/m1n1` — `src/dart.c` (регистры DART T8020/T8110, формат PTE).
- Linux mainline `drivers/iommu/apple-dart.c` (t8103/t8110/t6000/t6020, bypass, streams).
- `AsahiLinux/linux` — `Documentation/devicetree/bindings/iommu/apple,dart.yaml`.

**Код проектов:**

- `Anemll/mlx-rdma` — `ibv_roundtrip.cpp`, `ibv_roundtrip.md`, `mlx/distributed/jaccl/jaccl.cpp`.
- `Geramy/OdinLink-Five` — `README.md`, `COMPAT.md`, `docs/GPU.md`, `mac/kext/OdinLinkNHI.cpp`, `mac/kext/OdinLinkRDMA.h`.
- `ml-explore/mlx` — `mlx/backend/metal/allocator.cpp` (StorageModeShared, `raw_ptr()`), `mlx/backend/metal/device.cpp`.
- `wkljohn/ds4-strix-halo-tp-odinlink`, `todd-chamberlain/jaccl-rdma`, `jonathan308/ThunderMLX`, `tmc/gojaccl`, `alexziskind1/mlx-jaccl-cluster`, `dev-tb5-tester/rdma_jaccl`, `mps-ddp/mccl`.
- NVIDIA NCCL — `src/transport/net_ib/gdr.cc`, `src/misc/ibvsymbols.cc` (референс использования `ibv_reg_dmabuf_mr`).

**Обсуждения:**

- HN (algolia): 46048147, 46248644, 46318352, 47469070.

**MelonDMA (репозиторий проекта):**

- `src/dext/Sources/MlxUserClient.cpp` (kMlxUCMethodRegMR), `src/dext/Sources/core/MlxDMA.cpp/.hpp`, `src/dext/Sources/userclient/MlxUCIO.h`, `src/dext/usermode/librdma_shim/librdma_shim.c`, `src/dext/usermode/libibverbs_compat/verbs_compat.c`.

---

## Приложение A. Сырые улики реверса

### A.1 ioreg — DART / PCIe / GPU

```
| |   +-o mapper-gfx-asc  <class AppleARMIODevice>
| |   |   {
| |   |     "compatible" = <"iommu-mapper,gfx">
| |   |     "AAPL,phandle" = <fb010000>
| |   |   }
| |   |   +-o AGXArmFirmwareMapper  <class AGXArmFirmwareMapper>
| |   |     {
| |   |       "CFBundleIdentifier" = "com.apple.AGXG14X"
| |   |       "IOClass" = "AGXArmFirmwareMapper"
| |   |       "IOMapperID" = <fb010000>
| |   |     }

| |   +-o mca-switch@9B600000  <class AppleARMIODevice>
| |   |   { "compatible" = <"mca-switch,t602x"> }

| |   |   "InterruptControllerName" = "APCIECMSIController-apciec0" … apciec5
| |   |   "IONameMatch" = "iommu-mapper"
| |   |   "IOClass" = "IODARTMapper"
| |   |   "IOUserClientClass" = "IODARTMapperClient"
| |   |   "iommu-dart-translation" = Yes
| |   |   "iommu-initial-translations" = <00000000000100000040940100010000>
| |   |   "compatible" = <"apciec,t8103">
| |   |   "apcie-piodma" = <7c000000>
| |   |   "apcie-port" = <03000000>
```

### A.2 AppleThunderboltRDMA.kext — ключевые строки Info.plist

```
CFBundleIdentifier: com.apple.driver.AppleThunderboltRDMA
CFBundleVersion:    0.0.1
LSMinimumSystemVersion: 26.6
DTSDKName:          macosx26.6.internal
Personality AppleThunderboltRDMAPeerInterface:
    IOClass = AppleThunderboltRDMAPeerInterface
    IOProviderClass = IOThunderboltXDomainService
    Protocol ID = 64087, Protocol Version = 1
OSBundleLibraries:
    com.apple.iokit.IORDMAFamily 1.0
    com.apple.iokit.IOThunderboltFamily 9.3.3
    com.apple.iokit.IOPCIFamily 2.0
```

### A.3 Экспорт librdma.tbd (выдержка)

```
tbd-version: 4
targets: [ arm64e-macos ]
install-name: '/usr/lib/librdma.dylib'
reexported-libraries:
  - '/usr/lib/rdma/libibmad.dylib'
    '/usr/lib/rdma/libibumad.dylib'
    '/usr/lib/rdma/libibverbs.dylib'
    '/usr/lib/rdma/libmlx5.dylib'
exports (libibverbs, выдержка):
    _ibv_cmd_reg_dmabuf_mr
    _ibv_reg_dmabuf_mr
    _ibv_reg_mr
    _ibv_reg_mr_iova
    _ibv_reg_mr_iova2
    _ibv_create_qp
    _ibv_create_srq
    ...
```

### A.4 m1n1 — регистры DART (выдержка)

```
T8020: TCR_OFF 0x100  TRANSLATE_ENABLE bit7, BYPASS_DART bit8, BYPASS_DAPF bit12
       TTBR_OFF 0x200 VALID bit31, ADDR[30:0], SHIFT 12
       PTE: OFFSET_SHIFT 14 (16 КиБ), OFFSET[39:14] (t8020) / [39:10] (t6000), VALID bit0
T8110: TCR_OFF 0x1000 BYPASS_DART bit1, TRANSLATE_ENABLE bit0
       TTBR_OFF 0x1400 VALID bit0, ADDR[29:2], SHIFT 14
       TLB_CMD 0x80 FLUSH_ALL op0, FLUSH_SID op1
       ENABLE_STREAMS 0xc00 / DISABLE_STREAMS 0xc20
```

### A.5 Linux apple-dart (выдержки)

```
DART_MAX_STREAMS 256 (config), 16 streams на экземпляр (DT-описание)
supports_bypass / tcr_bypass / apple_dart_hw_enable_bypass
compatible: apple,t8103-dart, apple,t8103-usb4-dart, apple,t8110-dart,
            apple,t6000-dart, apple,t6020-dart (совм. t8110), apple,t8122-dart
```

---

*Конец отчёта.*
