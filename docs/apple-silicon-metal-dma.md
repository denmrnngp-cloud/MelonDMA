# Apple Silicon unified memory → DMA прямо в Metal-буферы

Research-заметка: как NIC (ConnectX, RoCEv2) делает DMA прямо в Metal-буфер без
bounce-copy на CPU. Сопоставлено с текущим кодом драйвера (`dev/`).

## 1. Главный вывод (TL;DR)

На Apple Silicon **нет видеопамяти и нет GPU BAR**. CPU, Apple GPU и NIC адресуют
один и тот же физический DRAM. Metal-буфер в режиме `.storageModeShared` — это
обычная системная память со стабильным CPU-указателем (`contents()`).

Этот указатель — ровно то, что уже регистрирует текущий путь драйвера:

```
ibv_reg_mr(pd, buf.contents, len, ...)
  → kMlxUCMethodRegMR (startAddr = contents())
  → CreateMemoryDescriptorFromClient(kIOMemoryDirectionOutIn, ...)
  → MlxDMA::Pin → IODMACommand::PrepareForDMA (DART/IOMMU → IOVA)
  → 4 KiB PAS → CREATE_MKEY
```

Значит **"DMA в Metal-буфер" на Apple Silicon — это не GPUDirect и не peer-mem,
а обычная регистрация shared-буфера как MR.** В DEXT 0.359 этот путь оформлен как
поддерживаемая возможность ABI `MLX_UC_FEATURE_COHERENT_UMA_MR`, включённая по
умолчанию без env-флага или отдельного режима драйвера. Приложение по-прежнему
должно выделить Metal-буфер в `.shared`: `.private` нельзя превратить в host-DMA
memory на уровне DEXT.

## 2. Модель памяти Apple Silicon

- **UMA (unified memory).** `MTLDevice.hasUnifiedMemory == true` — "the GPU shares
  all of its memory with the CPU". CPU, GPU, ANE — один пул DRAM. Дискретной VRAM
  и PCIe BAR под память GPU не существует.
- **IOMMU = DART** (Device Address Resolution Table). DMA от PCIe-устройства
  (Thunderbolt → ConnectX) идёт через DART-трансляцию. `IODMACommand::PrepareForDMA`
  выдаёт device-IOVA сегменты, которые драйвер режет на 4 KiB HCA PAS
  (`MlxDMA::Pin` → `mlxAppendMttPages`, `notes/11 §0`).
- **Когерентность.** Interconnect Apple Silicon когерентный: запись NIC в
  зарегистрированные страницы видна CPU/GPU без явных cache-flush. Это **уже
  доказано на этом железе**: gate 1 000 000 сообщений с guard-bytes, 0 CQE-ошибок
  (`docs/research.md §5`) — NIC↔CPU когерентность зарегистрированных клиентских
  страниц работает. Видимость NIC→GPU переносится не flush'ем, а синхронизацией
  через CQE/WC (см. §5).

## 3. Metal storage modes против регистрируемости

| Mode | Что это | `contents()` | DMA-регистрация |
|---|---|---|---|
| `.shared` | системная память, CPU+GPU общий доступ | валидный VA | ✅ **прямо** (это и есть путь) |
| `.private` | только GPU по публичному контракту Metal | host-доступ не гарантирован; на проверенном AGX возвращался неприменимый opaque VA | ❌ не разыменовывать и не регистрировать; нужен blit в shared |
| `.managed` | deprecated; на Apple Silicon ≈ shared | валидный | ✅ (но не использовать) |
| `.memoryless` | tile-memory на время render pass | нет | ❌ |

Ключевое: регистрируем **`contents()` (CPU VA), а не `gpuAddress()`**.
`gpuAddress()` — это GPU-адресное пространство, `CreateMemoryDescriptorFromClient`
нужен именно CPU VA. Для `.shared` физические страницы одни, но CPU VA ≠ GPU VA.

## 4. Два рабочих рецепта

### A. Регистрировать `contents()` shared-буфера (минимум кода)

```objc
id<MTLBuffer> buf = [device newBufferWithLength:N
                        options:MTLResourceStorageModeShared];
void *p = buf.contents;                       // стабильный CPU VA в DRAM
ibv_mr *mr = ibv_reg_mr(pd, p, N,
    IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
// RDMA WRITE: sge.addr = (uintptr_t)p, sge.lkey = mr->lkey
// GPU читает те же страницы: [buf contents] / в шейдере.
```

### B. Обернуть page-aligned CPU-аллокацию через `bytesNoCopy` (больше контроля)

```objc
void *p; posix_memalign(&p, 4096, N);         // или vm_allocate / mmap
id<MTLBuffer> buf = [device newBufferWithBytesNoCopy:p length:N
                        options:MTLResourceStorageModeShared
                        deallocator:nil];
ibv_mr *mr = ibv_reg_mr(pd, p, N, ...);       // тот же указатель с обеих сторон
```

GPU и NIC бьют в одни и те же физические страницы. Копий нет. Вариант B
предпочтителен: даёт контроль над выравниванием и временем жизни страниц.
`bytesNoCopy` **требует** page-aligned память (posix_memalign/vm_allocate/mmap).

## 5. Hazard tracking и видимость

Запись NIC происходит **вне поля зрения Metal**, поэтому:

- Создавать буфер с `MTLHazardTrackingModeUntracked` (`.untracked`) — иначе Metal
  трекает зависимости, которых не видит, и не защитит от гонки NIC-запись vs
  GPU-чтение; `untracked` убирает и ложный трекинг, и его оверхед.
- Синхронизацию делать самому:
  - **NIC → GPU:** дождаться RDMA-комплита (poll CQ / WC), потом сабмитить GPU-работу.
  - **GPU → NIC:** завершить Metal command buffer + `waitUntilCompleted()` (или
    `MTLSharedEvent`), потом постить RDMA WR.

На Apple Silicon когерентность данных обеспечивает interconnect; CQE/WC/фенсы
обеспечивают **порядок**, не содержимое кэша.

## 6. Ограничения текущего драйвера (важно для Metal KV-cache)

1. **`MLX_MAX_DMA_PAGES = 480`** (`MlxDMA.hpp`) → один прямой MR = 480×4 KiB ≈
   **1.875 MiB**. Metal KV-cache / большие веса (GiB) **не влезут в прямой MR**.
   Путь для них уже есть: `kMlxUCMethodRegMRIndirect` → `MlxMR::RegMRIndirect`
   (indirect KLM из уже зарегистрированных children-MR). Либо много отдельных MR.
2. **Выравнивание.** PAS-walk идёт по 4 KiB границам; `startAddr` и `length`
   должны быть 4 KiB-выровнены, иначе первая/последняя страница режется частично
   (см. `mlxAppendMttPages`). `newBufferWithLength` на практике отдаёт выровненное
   хранилище, но с `bytesNoCopy` выравнивание — ваша ответственность.
3. **`kIOMemoryDisableCopyOnWrite`** уже стоит в `RegMR` — страницы wired и
   стабильны на всё время жизни MR. Это то, что нужно для Metal-буфера.

## 7. Почему GPUDirect на Apple Silicon не нужен

NVIDIA GPUDirect RDMA существует только потому, что VRAM — отдельное адресное
пространство на PCIe BAR: нужны BAR1 + nvidia-peermem + ATS, чтобы NIC мог DMA в
VRAM. На Apple Silicon отдельного адресного пространства нет — unified memory
сворачивает "GPUDirect hop" в обычную регистрацию MR. Ни peer-mem, ни BAR-окна,
ни PCIe P2P добавлять не нужно.

Единственный аналог end-state "GPU-native RDMA" (DeepEP/IBGDA на CUDA) — маппинг
UAR-doorbell в Metal-адресное пространство, чтобы GPU сам строил WQE и звонил в
doorbell. **Проверено и закрыто (kernel panic, см. §7.1): на Apple Silicon GPU не
может писать в PCIe MMIO — направление невозможно, а не просто не нужно.**

### 7.1 GPU-native doorbell на Apple Silicon — ЗАКРЫТО (panic)

Проверка `tools/mlx_gpu_doorbell_gate.m` (Phase B): UAR-doorbell маппится в
userspace через `IOConnectMapMemory64` (`kMlxUCMemKindUar`) — это PCIe BAR MMIO,
**не DRAM**. Попытка обернуть этот VA как Metal-буфер через
`newBufferWithBytesNoCopy:` привела к панике ядра:

```
panic: physical page is before the start of DRAM: 0x290004 < 0x4000000) @vm_resident.c:3369
Panicked task: mlx_gpu_doorbell_gate
Kernel Extensions in backtrace: com.apple.iokit.IOGPUFamily, com.apple.AGXG14X
```

Механизм: Apple GPU-драйвер (IOGPUFamily/AGXG14X) при маппинге буфера идёт
VA → физическая страница и считает её resident-страницей DRAM. У UAR-BAR
физический адрес 0x290004 лежит ниже начала DRAM (0x4000000 = 64 MiB, зона
MMIO/периферии) → проверка `vm_resident.c:3369` падает. Вина не в MelonDMA, а в
Apple-драйвере, которому скормили не-DRAM указатель.

Выводы:
- **Никогда не передавать MMIO/BAR-указатель в `newBufferWithBytesNoCopy:`** —
  только DRAM (malloc/posix_memalign/vm_allocate/mmap). Иначе hard-panic ядра из
  userspace.
- GPU-native doorbell через Metal закрыт: у Apple GPU адресное пространство только
  DRAM, аналога `cudaHostRegisterIoMemory`/mapped-BAR в Metal нет.
- CPU-proxy на data-path остаётся. Потолок всё равно PCIe Gen3 x4, не CPU
  (`docs/research.md §6`), так что потерь нет.

## 8. Что проверить на железе

- [x] Зарегистрировать `contents()` shared `MTLBuffer` как MR, RDMA WRITE из Spark,
      прочитать в Metal-шейдере — end-to-end когерентность без flush подтверждена.
- [x] Проверить `.private`: на текущем AGX `contents()` вернул opaque non-NULL VA,
      но это не контракт host-доступа. Gate его не разыменовывает и не регистрирует;
      поддерживаемый путь остаётся `.shared` или явный blit в `.shared`.
- [x] Большой буфер 4 MiB прогнан через четыре direct child MR и
      `RegMRIndirect`; Spark RDMA WRITE виден Metal-шейдеру без mismatch.
- [x] GPU-native doorbell — ЗАКРЫТО: GPU не пишет в PCIe MMIO, проба даёт kernel
      panic (§7.1). Не пытаться снова через `bytesNoCopy`.

### 8.1 Реализованная проверка (DEXT 0.359, 2026-09-04)

DEXT без opt-in рекламирует `MLX_UC_FEATURE_COHERENT_UMA_MR`. Тестовый провайдер
`tools/mlx_metal_memory.m` выделяет `.shared | .untracked` MTLBuffer, выполняет
producer/consumer kernels и передаёт тот же `contents()` обычному verbs MR.
`tools/run_metal_dma_gate.sh` проверяет обе стороны обмена и indirect MR.

На Mac Studio (Apple M2 Ultra) ↔ Spark успешно пройдены:

- GPU producer → NIC RDMA WRITE → Spark: 32 × 1 MiB, 17.97 Gbit/s, remote verify OK;
- Spark RDMA WRITE → NIC → shared Metal buffer → GPU consumer: 32 × 1 MiB,
  `mismatches=0`;
- Spark RDMA WRITE в 4 MiB indirect KLM MR → Metal consumer: `mismatches=0`;
- direct datapath во всех случаях без SEND/RECV fallback.

Формат RC/RoCEv2 peer-neutral и подходит для второго Mac с ConnectX и MelonDMA,
но live Mac↔Mac проверка не выполнялась: второй Mac сейчас не подключён.

## 9. Источники

- Apple: `MTLStorageMode` (shared/private/managed/memoryless), `MTLDevice.hasUnifiedMemory`,
  `MTLHazardTrackingMode` (untracked), `newBufferWithBytesNoCopy:length:options:deallocator:`.
- Этот репозиторий: `src/dext/Sources/core/MlxDMA.{hpp,cpp}` (Pin/Unpin/IOVA→PAS),
  `src/dext/Sources/ib/MlxMR.cpp` (RegMR / RegMRIndirect / MKEY),
  `src/dext/Sources/MlxUserClient.cpp` (`kMlxUCMethodRegMR` →
  `CreateMemoryDescriptorFromClient`), `docs/research.md §5–6`.
