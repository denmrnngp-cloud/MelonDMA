# Готовность MelonDMA к полноценному RDMA на Apple Silicon

Дата аудита: 5 сентября 2026 года. Исследован рабочий код `dev/src/dext`, DEXT **0.389**, build tag `p0-eq-timer-idle-rate-1`.

Это анализ и план доведения до готовности, а не отчёт о реализации исправлений. Код драйвера, прошивка, сетевые настройки и активный DEXT в рамках аудита не изменялись.

## 1. Главный вывод

**Для обычного аппаратного RC/RoCEv2 и DMA в общую память Metal на M2 Ultra не требуется открывать новую скрытую возможность Apple GPU. Основной путь уже реализован.** Главный недостающий слой — корректность при отказах, доказанная доставка событий, управление временем жизни памяти и проверенная совместимость.

Но называть текущую реализацию «полноценным надёжным драйвером для всех M‑чипов» пока нельзя. Обнаружены конкретные дефекты:

1. Аварийный обработчик очищает не тот бит PCI Command: выключает Memory Space Enable, а не Bus Master Enable.
2. После таймаута команды освобождаются DMA-mailbox, хотя завершение доступа firmware к ним не подтверждено. Аналогичный риск есть в ошибочном выходе регистрации MR.
3. В EQ отсутствует барьер чтения между успешной проверкой owner и чтением тела события.
4. Опасные отладочные операции доступны обычному авторизованному клиенту драйвера без отдельного административного уровня.
5. Наличие completion EQ принимается за готовность IRQ, хотя предыдущие аппаратные проверки фиксировали доставку через таймер, а не MSI-X.

Это более высокий приоритет, чем дальнейшее уменьшение количества mutex, doorbell или CPU-копий.

Три разных цели нельзя смешивать:

| Уровень | Оценка |
|---|---|
| NIC выполняет SEND/RECV, RDMA READ/WRITE над зарегистрированной памятью | Реализован RC/RoCEv2; требуется завершить проверку отказов и совместимости |
| NIC пишет в страницы `MTLBuffer.shared`, затем их читает GPU без промежуточной CPU-копии | Реализован путь; есть историческая Mac↔Spark проверка на 0.359 |
| GPU сам управляет NIC, включая PCIe doorbell, без CPU-координатора | Поддерживаемого публичного Metal-пути в исследованных API не найдено; текущий драйвер этого не обеспечивает |

## 2. Что действительно проверено сейчас

### Снимок машины и сборки

| Параметр | Наблюдение 05.09.2026 |
|---|---|
| Машина | Mac Studio M2 Ultra, `Mac14,14`, 192 GiB общей памяти |
| ОС | macOS 26.6.1, build 25G76 |
| Страница памяти хоста | 16 384 байта |
| Загруженная extension | `com.mlx5.rdma.dext`, 0.389/0.389, activated/enabled |
| IORegistry | `MlxPCIDriver` active, build tag совпадает с исходниками |
| Подпись / SIP | Apple Development; SIP отключён |
| Подключённый корпус | Wavlink UTE02, определяется как **Thunderbolt 3**, 40 Gb/s |
| PCIe карты | `15b3:1015`, Link up, **8.0 GT/s ×4** |
| Второй Mac | Не подключён; Mac↔Mac аппаратно не проверялся в этом аудите |

Порт Mac поддерживает Thunderbolt 4, но конкретный подключённый UTE02 работает как TB3. Надпись «40 Gb/s» не означает 40 Gb/s полезного RDMA-трафика. Поле `MSI: Yes` в системном отчёте не доказывает, что IRQ доходит до DEXT; `Pause Compatible` также нельзя читать как результат проверки Ethernet PFC.

Версия, статус extension и build tag проверены чтением системного состояния. Это **не побайтовое доказательство воспроизводимой сборки исходников**: для него нужен release manifest с хешами исходников, бинарника, SDK и подписи.

### Проверки, выполненные при аудите

- `make check-host` — успешно: portable IFC, shim smoke, service matching, verbs smoke, ABI properties, MKey hash index.
- `make check-dext` — успешно: plist/согласованность tag и синтаксическая проверка DEXT под DriverKit25.5 SDK.
- В shim smoke `IOServiceOpen` вернул `0xe00002e2`; тест корректно допускает недоступность устройства. Поэтому этот PASS **не является аппаратным PASS**.
- Полная линковка/подпись новой DEXT, перезагрузка драйвера, peer gate, fault injection, GPU-MMIO эксперимент не выполнялись.

### Исторические результаты — отдельно от сегодняшней проверки

В [исследовании Metal DMA](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/docs/apple-silicon-metal-dma.md) записан gate DEXT 0.359:

- GPU producer → RDMA WRITE → Spark: 32 × 1 MiB, 17.97 Gbit/s, remote verify OK.
- Spark → shared Metal buffer → GPU consumer: 32 × 1 MiB, без несовпадений.
- Spark → 4 MiB indirect MR → GPU consumer: без несовпадений.

Это полезное доказательство работоспособности конкретного пути на M2 Ultra. Оно не заменяет повторный gate 0.389, длительный стресс, проверку allocator reuse и других M‑чипов.

## 3. Что уже есть и не нужно реализовывать заново

- DriverKit/PCIDriverKit: PCI matching, BAR, firmware command interface, firmware pages, HCA bring-up, FLR, фазовая готовность и части DMA quarantine.
- RC QP, CQ, PD, MR, SEND/RECV, READ/WRITE, immediate data, inline, atomics и indirect MKey.
- Per-client ownership/token checks, лимиты числа объектов, отдельные клиентские UAR/DB-ресурсы.
- Userspace SQ/RQ/CQ mapping; direct polling/posting, batching, selective signaling, lazy CQ consumer publication, single-owner fast path и счётчики fallback.
- Большой прямой MR через расширенный CREATE_MKEY и более крупную гранулярность MTT, если IOVA непрерывен; indirect fallback.
- Регистрация CPU VA общей Metal-памяти через обычный MR; отдельный feature bit `COHERENT_UMA_MR`.
- Completion EQ, CQ arm/moderation, блокирующий event API, таймерный fallback и IRQ-счётчики.
- Программирование RoCE-адресов и MTU; телеметрия DEXT и userspace provider.

Основные точки: [UserClient/ABI](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/src/dext/Sources/MlxUserClient.cpp:799), [DMA](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/src/dext/Sources/core/MlxDMA.cpp:64), [MR](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/src/dext/Sources/ib/MlxMR.cpp:128), [direct CQ](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/src/dext/usermode/librdma_shim/librdma_shim.c:978), [telemetry](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/src/dext/usermode/libibverbs_compat/verbs_compat.c:492), [Metal gate helper](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/src/dext/tools/mlx_metal_memory.m:94).

Наличие реализации означает «есть код», а не автоматическое подтверждение всех заявленных семантик и режимов на железе.

## 4. Уточнения к двум исследованиям реверс-инжиниринга

Исследования [Metal DMA](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/docs/apple-silicon-metal-dma.md) и [GPUDirect](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/docs/gpudirect-apple-silicon.md) полезны как история экспериментов. Несколько формулировок требуют исправления перед использованием как спецификации.

| Формулировка в исследованиях | Корректная трактовка |
|---|---|
| «Прямой MR ограничен 480 × 4 KiB» | Это предел малого PAS-массива, не общий предел нынешнего MR. В 0.389 существует large-MR путь |
| «Indirect MR ≤32 children» | Текущий ABI допускает 240 children; фактический предел зависит также от ресурсов/разбиения |
| «Metal DMA ещё не проверялся» | Устарело: есть записанный gate 0.359; повторная сертификация текущего релиза остаётся открытой |
| «Любой `contents()` годится для регистрации» | Только документированная CPU-доступная память. Opaque non-NULL от `.private` не даёт права её регистрировать |
| «На UMA нет отдельного адресного пространства» | Общая физическая DRAM не означает одинаковые CPU VA, GPU VA и NIC IOVA |
| «Достаточно `posix_memalign(...,4096,...)` для Metal bytesNoCopy» | Metal требует page-aligned адрес и размер и одну VM region; на этой машине страница 16 KiB |
| «DisableCopyOnWrite сам по себе wires память» | Это COW-политика дескриптора; DMA lifetime обеспечивается также prepare/mapping и удержанием дескриптора |
| «Untracked обязателен для NIC» | Отключение Metal hazard tracking само не синхронизирует внешнюю NIC; это отдельный выбор GPU-runtime |
| «Нужен обязательный managed-resource blit sync» | Нельзя переносить managed-рецепт на shared Apple GPU память; нужен корректный порядок владения |
| «После любого remote WRITE будет локальный receive CQE» | Обычный RDMA WRITE не создаёт receive completion на получателе. Нужен протокол уведомления |
| «Panic доказал, что GPU никогда не сможет обращаться к MMIO» | Доказан отказ конкретного неподдерживаемого пути. Публичный Metal BAR-mapping API не найден; прогноз «никогда» не обоснован |
| «Info.plist без отдельного executable доказывает, что Apple RDMA — заглушка» | Содержимое одного bundle не доказывает отсутствие реализации в системных collections/cache |
| «CPU-proxy ничего не стоит, поскольку потолок PCIe» | Для bulk возможен PCIe bottleneck; для мелких сообщений и TTFT стоимость CPU/wakeup остаётся существенной |

Apple документирует shared/private как разные режимы доступа даже при UMA: [storage modes для Apple GPU](https://developer.apple.com/documentation/metal/choosing-a-resource-storage-mode-for-apple-gpus). Условия оборачивания памяти: [makeBuffer(bytesNoCopy)](https://developer.apple.com/documentation/metal/mtldevice/makebuffer%28bytesnocopy%3Alength%3Aoptions%3Adeallocator%3A%29). Shared не следует подменять обещанием универсальной поддержки managed.

Наличие символа `ibv_reg_dmabuf_mr` также не доказывает наличие рабочего импортера Metal/IOSurface. Для существующего shared-MR пути такой импортёр не является обязательным.

## 5. P0 — исправления корректности и безопасности

Здесь «подтверждено кодом» означает конкретную ветвь исходников. Реальные последствия отказа на живой карте намеренно не провоцировались.

### P0.1. Исправить аварийное прекращение DMA

**Подтверждённый дефект.** [MlxHealth::MarkFatal](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/src/dext/Sources/core/MlxHealth.cpp:129) записывает `cmd & ~0x2u` в PCI Command, затем сообщает об отключении bus mastering.

Но `0x2` — Memory Space Enable, а Bus Master Enable — `0x4`. Получается обратное заявленному контракту: BAR-доступ выключается, а BME не очищается этой операцией. Определения подтверждаются [PCI register definitions Linux](https://github.com/torvalds/linux/blob/master/include/uapi/linux/pci_regs.h). Кроме того, [DisableBusMasterAndVerify](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/src/dext/Sources/MlxPCIDriver.cpp:2750) пока возвращает `false` без реализации.

Что требуется:

- единая процедура device fencing с правильным битом, проверкой результатов config access и readback;
- не считать один сброс BME доказательством завершения уже отправленных PCIe-транзакций;
- удерживать DMA mappings до подтверждённого безопасного завершения/reset;
- сделать fatal state «липким» до recovery: [Health::Check](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/src/dext/Sources/core/MlxHealth.cpp:99) сейчас снова выставляет `healthy=true`, хотя core quarantine остаётся.

Приёмка: тест битовых масок; сценарий fatal → fenced → reset → reinitialize; после fatal новые WR не принимаются, состояние не становится healthy без recovery, нет MMIO к недоступному BAR.

### P0.2. Довести quarantine до всех неоднозначных команд и unwind-ветвей

**Подтверждённые опасные пути.**

- [MlxCmd::ExecLocked](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/src/dext/Sources/core/MlxCmd.cpp:458) при timeout выставляет внутренний `quarantined`, но сразу вызывает `FreeMailbox`.
- [FreeMailbox](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/src/dext/Sources/core/MlxCmd.cpp:327) выполняет CompleteDMA и release без проверки quarantine. Отсутствие firmware completion не доказывает, что mailbox больше не читается/не записывается.
- [MlxMR::RegMR](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/src/dext/Sources/ib/MlxMR.cpp:211) при любой ошибке CREATE_MKEY делает Unpin. При timeout результат создания MKey может быть неизвестен.
- При успешном CREATE_MKEY и отсутствии свободного software slot результат DESTROY_MKEY игнорируется, после чего память unpin.
- Core [Exec](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/src/dext/Sources/MlxPCIDriver.cpp:1736) возвращает ошибку команды, но не переводит автоматически внутренний command quarantine в общий device quarantine.
- В [UserClient MR unwind](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/src/dext/Sources/MlxUserClient.cpp:1326) ошибка cleanup после невозможности создать token также не обрабатывается как отдельное состояние.

Это не утверждение о случившейся порче памяти. Это отсутствие необходимой гарантии lifetime в отказном пути.

Нужна единая модель результата: «успех», «firmware отказала и точно не создала объект», «результат неизвестен». Последняя категория должна сохранять command queue/mailbox, MR/QP/CQ и их зависимости до безопасного reset, запрещать новые операции и не терять учёт объектов.

Приёмка: fault injection до/после doorbell, задержанный ответ, ошибки DESTROY и исчерпание таблиц. Ни один потенциально доступный NIC буфер не освобождается преждевременно; после проверенного recovery ресурсы возвращаются, quarantine имеет измеримый размер.

### P0.3. Исправить DMA ordering EQ и унифицировать барьеры

**Подтверждено кодом.** [MlxEQ::Poll](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/src/dext/Sources/core/MlxEQ.cpp:299) делает barrier, читает обычный owner, затем копирует EQE без барьера после успешной проверки owner.

На ARM проверка ownership должна предшествовать чтению данных с надлежащим DMA read barrier между ними. Барьер до owner не задаёт этот порядок. В [эталонном mlx5 EQ poller](https://raw.githubusercontent.com/torvalds/linux/master/drivers/net/ethernet/mellanox/mlx5/core/eq.c) `dma_rmb()` расположен после проверки ownership.

Собственный [direct CQ poller](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/src/dext/usermode/librdma_shim/librdma_shim.c:987) уже использует такой порядок. DEXT CQ также имеет барьер после owner; нельзя ошибочно описывать весь CQ-путь как лишённый барьеров.

Требуется:

- явное одиночное чтение ownership, затем DMA read barrier, затем тело EQE;
- отдельная проверка command completion polling на аналогичный порядок;
- согласованный контракт CPU↔NIC для чтения, публикации WQE/DB и MMIO: обычный C++ fence не следует без проверки объявлять универсальным заменителем device-scope barrier;
- не снимать барьеры ради скорости до проверки области shareability и атрибутов отображения.

Приёмка: wrap-around, concurrent EQ producers, смена CPU, ошибки CQE/EQE, длительный ARM stress; отсутствие повторов/пропусков событий и нарушений порядка.

### P0.4. Отделить административный интерфейс от обычного verbs-клиента

Авторизация подключения уже есть: DEXT требует `userclient-access`; это не доступ для любого процесса.

Однако [DispatchDebugMethod](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/src/dext/Sources/MlxUserClient.cpp:1908) позволяет подключившемуся клиенту запросить FLR, raw firmware Exec и выдачу страниц. [FwCmdAllowed](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/src/dext/Sources/MlxUserClient.cpp:414) — только rate limiter, не проверка привилегии и не opcode allowlist. [DbgPerformFlr](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/src/dext/Sources/MlxPCIDriver.cpp:2277) делает reset без собственного запрета при живых объектах других клиентов.

Для доверенного лабораторного стенда это инструмент bring-up. Для нескольких приложений это нарушение границы изоляции.

Приёмка: административные методы выделены в отдельный доверенный control endpoint либо исключены из release API; обычный разрешённый клиент не может менять firmware/reset чужой сессии. Проверки destructive control plane учитывают **все** живые клиенты устройства, не только вызывающего.

## 6. P1 — доставка completion с малой задержкой и низким CPU

### Что мешает сейчас

В [Plan, P3](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/docs/Plan.md) записано: после исправления порядка FLR/MSI-X второй EQ создаётся, но при реальном трафике `async_irq=0` и `completion_irq=0`; проба обоих векторов не изменила результат. Это историческое измерение, не повторный live gate текущего аудита.

Источник событий при таком состоянии — EQ timer. В [0.389](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/src/dext/Sources/MlxPCIDriver.cpp:500) интервалы составляют 10 ms с активными CQ при недоказанных IRQ, 100 ms без CQ и 50 ms после любого наблюдавшегося IRQ.

**10 ms — не минимальная задержка каждого RDMA WR.** Active polling mapped CQ не ждёт этот таймер. Но для заснувшего event consumer добавляется ожидание следующего tick, примерно 0–10 ms плюс планирование. Это большой штраф для микросекундного RPC.

### Что необходимо

1. Разделить `EQ создан`, `IRQ настроен`, `IRQ действительно доставлен`, `событие доставлено таймером`.
2. [CompletionInterruptReady](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/src/dext/Sources/MlxPCIDriver.cpp:1774) сейчас проверяет наличие объектов, не доказанную доставку. Feature и отчёт не должны выдавать это за работающий MSI-X.
3. Не переводить completion fallback на 50 ms лишь из-за одного async IRQ: [политика таймера](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/src/dext/Sources/MlxPCIDriver.cpp:979) суммирует оба вектора. Доказательство должно относиться к нужному вектору и текущему reset epoch.
4. Локализовать потерю IRQ: firmware EQ state/arm → MSI-X table/PBA/masks → bridge/PCI provider → DriverKit dispatch. Настройка после FLR уже исправлена; её не нужно «реализовывать заново».
5. Проверить нормальный matching и чистую активацию как отдельный эксперимент. Гипотеза из Plan о влиянии IOCatalogue injection **не доказана**. Сейчас extension видна как активная system extension; это само по себе не устанавливает способ захвата PCI-функции.
6. Сохранить active-direct/hybrid режим; после idle — корректный arm/recheck/wait без потери событий. Уменьшение таймера допустимо как измеряемый fallback, но повышает CPU и не заменяет исправление IRQ.

Приёмка: известное число armed CQ events, IRQ handler counters и независимые timer counters; event wakeup без таймерной помощи; p50/p95/p99 и CPU idle/burst. Отдельно гонка completion между последним poll и sleep.

**Дефект будущего multi-device режима:** [создание completion channel](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/src/dext/usermode/libibverbs_compat/verbs_compat.c:842) открывает event connection через `rdma_open_device()`, то есть устройство по умолчанию. Основной context открывается по имени. На второй NIC ожидание может попасть не на тот HCA; привязку надо сохранять явно.

## 7. P1 — lifetime, hot-unplug, sleep и восстановление

[Stop](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/src/dext/Sources/MlxPCIDriver.cpp:1631) уже содержит остановку poller/IRQ, проверку BAR и quarantine. Поэтому «teardown отсутствует» было бы неверно.

Но в текущем `MlxPCIDriver.iig/.cpp` не найден полноценный явный sleep/resume-контракт уровня RDMA. Наличие системного IOPowerManagement в IORegistry его не доказывает.

Нужно специфицировать:

- состояния running → quiescing → fenced → removed/resetting → ready;
- запрет новых WR и wakeup всех ожидателей при удалении/ошибке;
- завершение callbacks до уничтожения EQ/CQ/notifier;
- недействительность старых handles, rkeys, mapped queues и сессий после reset;
- повторное программирование MSI-X, port/GID/MTU и пересоздание объектов;
- освобождение либо безопасное удержание mappings при crash самого DEXT, а не только при штатном Stop.

Особенно важно проверить **отзыв mapping**: [CopyClientMemoryForType](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/src/dext/Sources/MlxUserClient.cpp:2021) проверяет владельца при выдаче, но [FreeClientBundle](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/src/dext/Sources/core/MlxUAR.cpp:308) освобождает UAR index и дескрипторы. Из этих функций нельзя заключить, что все ранее выданные отображения гарантированно отозваны до повторного использования UAR. Это открытая проверка безопасности, не доказанный межклиентский exploit. Также проверить допустимость read-only CQ mapping.

Приёмка: завершение клиента во время traffic; два клиента; loss of link; sleep/wake; disconnect/reconnect enclosure; ошибки firmware. Нет panic/hang, stale mapping не управляет новым объектом, восстановление имеет ограниченное время. Опасные unplug/fatal тесты проводить только на выделенном стенде.

## 8. P1 — MR для больших буферов и небольших M‑Mac

### Что осталось после large-MR оптимизации

[MlxDMA::Pin](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/src/dext/Sources/core/MlxDMA.cpp:77) запрашивает весь диапазон одним PrepareForDMA с массивом из 32 segments. Для больших MR [MlxMR](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/src/dext/Sources/ib/MlxMR.cpp:153) требует непрерывный **IOVA**, выбирает MTT page shift и проверяет вместимость firmware mailbox. Это не требование физически непрерывной DRAM.

Поэтому оставшиеся задачи:

- обрабатывать фрагментацию и ограничения выдачи DMA segments предсказуемо; при необходимости использовать несколько mappings/child MR с корректным временем жизни;
- проверять полное покрытие диапазона и переполнения, выводить причину fallback;
- проверять крупные, невыровненные и пограничные диапазоны, memory pressure, разные allocators;
- не смешивать 4 KiB HCA PAS, 16 KiB страницы ОС и размер крупной MTT-записи;
- отдельно измерять холодную регистрацию и steady-state с переиспользованием MR.

Публичный [PrepareForDMA](https://developer.apple.com/documentation/driverkit/iodmacommand/preparefordma) возвращает bus/DMA segments; численные совпадения CPU VA, IOVA или GPU VA не являются контрактом.

### Не хватает ограничения pinned bytes

[Квоты UserClient](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/src/dext/Sources/MlxUserClient.cpp:324) ограничивают количество PD/QP/CQ/MR/MKey и DB records. В исследованном пути нет отдельного резервирования суммарных pinned bytes до PrepareForDMA. После поддержки больших MR лимит числа MR уже недостаточен.

Нужны per-client и device-wide лимиты pinned bytes, high-water mark, bounded registration cache, понятный отказ до тяжёлой регистрации. Нельзя переносить policy с 192 GiB M2 Ultra на Mac с существенно меньшим объёмом памяти.

Приёмка: под давлением памяти отказ предсказуем и изолирован; dereg возвращает счётчики к baseline; суммарный предел невозможно обойти несколькими connections.

## 9. P1 — из демонстрации Metal DMA в контракт для приложений

### Поддерживаемый путь

Приложение удерживает `MTLBuffer.shared`; регистрируется **CPU `contents()`**, а не `gpuAddress`. NIC получает собственный IOVA через PCIDriverKit. GPU и NIC работают с общей backing memory, но имеют разные адресные пространства и независимое выполнение.

Apple описывает shared память и CPU/GPU синхронизацию; это не автоматическая гарантия всех свойств внешней NIC на каждом SoC. Имеющийся GPU gate нужно повторять для поддерживаемых сочетаний Mac/OS/enclosure. [Синхронизация Metal](https://developer.apple.com/documentation/metal/resource-synchronization).

### Чего не хватает в продуктовой интеграции

DEXT видит VA/MR, а не Objective-C объект и не граф GPU-команд. Нужна userspace-обёртка registered Metal buffer:

- удерживает allocation/`MTLBuffer`, MR и ссылку на device/PD;
- учитывает GPU jobs и незавершённые WR;
- запрещает reuse/освобождение до завершения соответствующих потребителей;
- имеет allocation generation и session epoch; cache только по `VA+length` небезопасен из-за повторного использования адреса;
- поддерживает разделение буфера на независимые slots и bounded pipeline.

Порядок **GPU → NIC**: завершение producer GPU work → CPU получает completion/shared-event notification → post WR → после local send completion можно переиспользовать source, если нет других владельцев.

Порядок **NIC → GPU**: протокол подтверждает готовность данных **на получателе** → CPU планирует GPU consumer либо сигнализирует ожидаемое им событие → после GPU completion возвращается credit на reuse.

Для обычного RDMA WRITE локальный CQE отправителя не является уведомлением процесса-получателя. Подходят WRITE_WITH_IMM с заранее выставленным receive, упорядоченный SEND-notify на том же RC QP либо явно спроектированный ACK-протокол. Несколько QP требуют отдельного порядка между ними. Успешный WC не означает, что удалённое приложение уже обработало данные.

`MTLSharedEvent` здесь — способ координации через CPU/Metal, а не найденный способ для ConnectX непосредственно сигнализировать объект Metal. Untracked не устраняет внешние гонки. CPU memory barrier не дожидается работающего GPU.

### Что не должно стать default

- Регистрация `.private`, memoryless, произвольной texture или opaque GPU address как host MR.
- Оборачивание BAR/UAR в `bytesNoCopy`: в исследовании уже был kernel panic; не повторять.
- Утверждение «нулевая копия во всём инференсе» лишь по счётчику DEXT.
- Синхронный `waitUntilCompleted` на каждый мелкий фрагмент без измерения; он годится для gate, но может уничтожить overlap.

Полезная следующая интеграция — приём прямо в конечные shared tensor/KV slots и GPU-native packing/unpacking там, где layout отличается. Это работа runtime/приложения, не только DEXT.

На Spark требуется отдельная проверка фактического CUDA/runtime-буфера и его регистрации. Успешный Mac Metal → Spark host-memory gate не доказывает отсутствие промежуточной копии до GPU-потребителя Spark. Нельзя переносить контракт `MTLBuffer.shared` на CUDA-аллокации только на основании общей памяти обеих платформ.

## 10. Совместимость и эксплуатация

### 10.1. M‑чипы и поколения ConnectX — независимые оси

Текущие matching и PCI entitlement ограничены `15b3:1015`. [Factory ConnectX-5/6/7](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/src/dext/Sources/hw/MlxHCAConnectX4.cpp:121) возвращают NULL. Даже перечисленные factory идентификаторы семейства ConnectX-4 не равны подтверждённой поддержке всех PF/VF.

Поддержка другого M‑чипа с той же NIC **не требует автоматически** отдельного HCA backend. Поддержка другой Mellanox NIC на том же M2, наоборот, требует capability/firmware/PCI-ID qualification.

Не следует добавлять запись идентификатора и объявлять ConnectX-5/6/7 готовыми без проверки command interface, UAR, CQE/WQE, page sizes, RoCE caps, atomics и firmware quirks.

### 10.2. Матрица, которой пока не хватает

| Конфигурация | Что известно | Что требуется |
|---|---|---|
| M2 Ultra + текущая ConnectX-4 Lx + UTE02 | Работающий стенд и исторические peer/GPU gates | Закрыть P0, затем повторить полный release gate |
| Другие M2; M1/M1 Pro/Max/Ultra с подходящим PCIe-туннелем | Архитектурные кандидаты; в аудите нет аппаратного подтверждения | DMA fragmentation, page/BAR mapping, IRQ, power, GPU visibility |
| M3/M4/M5 и последующие доступные M‑Mac | Нельзя автоматически наследовать сертификат M2 Ultra | Та же матрица на реально поддерживаемой ОС и конкретном enclosure |
| Mac↔Mac с двумя поддержанными ConnectX | RoCE wire protocol этому не препятствует | Двусторонние SEND/READ/WRITE, immediate, Metal producer/consumer |
| Mac↔Spark | Исторические gates есть | Регрессия текущего релиза, runtime lifetime и end-to-end copy accounting |
| Две NIC / два контроллера | Есть enumeration/open-by-name, но event connection требует исправления | Раздельные IOMMU/MR/IRQ domains, независимый recovery, multi-rail routing |

Проверять capabilities, а не делать вывод по строке «M‑series»: CPU page size, GPU unified-memory/storage support, DMA mapping, negotiated PCIe link, BAR/UAR granularity, firmware caps, версия ОС/SDK. Изучение DART полезно для понимания; ручное программирование DART не является необходимым шагом этого driver path.

### 10.3. Apple Thunderbolt RDMA — другой транспорт

По актуальной [Apple TN3205](https://developer.apple.com/documentation/technotes/tn3205-low-latency-communication-with-rdma-over-thunderbolt), Apple TB-RDMA доступен начиная с macOS 26.2 на Apple Silicon с **Thunderbolt 5**. В описанном API — UC, SEND/RECV; лимиты до 10 QP и 4095 frames, максимум 16 773 120 байт на сообщение при полной глубине.

Это не RoCEv2 ConnectX через PCIe-туннель. Требование TB5 **не запрещает MelonDMA на M2/TB4**, а включение Apple TB-RDMA не превращает его интерфейс в RoCE peer для Spark. Общие названия verbs не обеспечивают совместимость транспорта или ABI.

### 10.4. Verbs и управление сетью

Собственная `libibverbs_compat` — полезный совместимый по части исходного API слой, но не доказанная drop-in binary replacement системной Apple libibverbs или полного rdma-core.

Конкретные пробелы:

- [ibv_get_async_event](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/src/dext/usermode/libibverbs_compat/verbs_compat.c:720) не восстанавливает QP/CQ object в `event.element`; приложение не получает стандартную привязку события к объекту.
- [Async EQ mask](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/src/dext/Sources/core/MlxEQ.cpp:146) не включает CQ_ERROR; [port handler](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/src/dext/Sources/ib/MlxRoCE.cpp:380) публикует PORT_ACTIVE на событие изменения порта без различения down/up.
- Нужны негативные conformance tests: invalid lkey/rkey, RNR/retries, CQ overrun, QP ERR/flush, unsignaled chains, shared CQ, atomics и MR access permissions.
- RC — допустимый продуктовый scope. UD/DC/XRC/SRQ, InfiniBand mode и multicast не являются обязательными для полноценного RC/RoCEv2.
- Свой `enX`, ARP/NDP/route integration и RDMA CM/стандартный connection manager пока не заменены полноценной системной интеграцией. Для статической Mac↔Spark RoCE-связи это не блокер data plane; для plug-and-play — отдельная работа provider/службы конфигурации, а при необходимости Ethernet — NetworkingDriverKit.
- Проверка MTU должна охватывать QP active_mtu обеих сторон, Ethernet media MTU с запасом под заголовки и весь физический путь. `QP MTU=4096` не равняется `ifconfig mtu 4096`.
- DCQCN command readback не доказывает настроенный ECN/PFC fabric. Для прямого соединения не требуется выдумывать switch-тюнинг; при переходе на коммутатор нужны реальные counters/drop tests.

### 10.5. Production-доставка

Сейчас уже используется активная system extension. Не хватает не «первого установщика», а **доказанного production-пути**: одобренные entitlements/provisioning, релизная подпись, notarization и работа с включённым SIP на чистой машине.

[Apple описывает](https://developer.apple.com/documentation/systemextensions/installing-system-extensions-and-drivers) активацию extension из приложения с проверкой подписи и выданных team entitlements. Текущая Apple Development подпись при отключённом SIP не подтверждает успешность такого релиза.

Приёмка: install/update/uninstall/reboot с SIP on, без ручного перехвата штатного Apple-драйвера и без отладочных boot-настроек; стабильное PCI matching и понятный отказ при конфликте владельцев карты.

## 11. Телеметрия: уже полезная, но ещё не полная

[QueryPerf](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/src/dext/Sources/MlxUserClient.cpp:1016) и [объединённая telemetry v2](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/src/dext/usermode/libibverbs_compat/verbs_compat.c:492) уже считают вызовы, длительность boundary, MR bytes, direct WR/CQE/doorbell, fallback, события и ошибки. Не нужно проектировать это заново.

Оставшиеся требования:

| Область | Недостающая гарантия/измерение |
|---|---|
| Область счётчика | Пометить per-context/per-device. Сейчас CQ event counters device-wide соседствуют с per-client perf |
| События | Раздельно IRQ handler entry, полученные EQE, timer-delivered EQE, wakeup, lost event; последний доказанный IRQ и reset epoch |
| DMA lifetime | Текущие pinned bytes, peak, quarantine bytes/objects, ambiguous commands, failed destroy, возраст quarantine |
| MR | Registration latency histogram, число segments, MTT shift/count, direct/indirect и причина fallback, cache hit/miss |
| Ошибки | Opcode/status/syndrome, QPN/CQN, link state, retrain/reset reason; недоступные hardware counters как unavailable, не 0 |
| Сеть | CRC/discards/retry/RNR/CNP/ECN/PFC и packet rate там, где firmware/fabric позволяют получить достоверные данные |
| Приложение | Copy bytes по стадиям, GPU wait/packing time, CPU-seconds/request обеих машин, TTFT breakdown |

`copied_bytes=0` в DEXT означает только отсутствие учтённых копий в этой области. Оно не доказывает отсутствие staging/memcpy/Metal blit в llama/MLX или на Spark.

**ABI:** telemetry имеет поля version/size, но функция заполняет `sizeof(*telemetry)` без размера буфера от вызывающего. При росте структуры это небезопасно для старого клиента с новой dylib. Нужен size-aware API либо версионированный symbol/фиксированная структура с согласованным расширением.

Нагрузку измерять как сумму затрат приложения, DEXT и относимой kernel/interrupt работы; не путать переименование `kernel_*` счётчиков с реально измеренным временем ядра. Стоимость самой телеметрии тоже проверить A/B; дорогие snapshots не ставить на каждый WR.

## 12. Чего драйвер не сможет исправить один

### Ограничение физического тракта

Для PCIe Gen3 ×4 расчёт после 128b/130b кодирования даёт около **31.5 Gbit/s до накладных расходов PCIe/TB/RoCE**: `8 GT/s × 4 × 128/130`. Это не прогноз полезного bandwidth. В [предыдущем предложении](/Users/macstudio/Documents/mac-spark-rdma-project/MelonDMA/dev/docs/rdma-vs-tcp40g-improvement-proposal.md) приведены результаты порядка 20–23 Gbit/s и оценка потолка 25–28; оценка не заменяет текущий benchmark.

Большая MTU, batching и несколько QP могут лучше использовать тракт, но не расширят ×4. Переход на другой Mac без смены ограничивающего enclosure также не гарантирует прироста.

### Победа над TCP40G

Потенциал есть прежде всего в меньшем CPU/request, меньшем количестве копий и более короткой цепочке готовности GPU-данных. Гарантировать выигрыш во всех размерах сообщений и режимах инференса нельзя.

Нужен interleaved A/B на одной версии модели/runtime с одинаковыми buffers, payload, MTU и длительностью:

- tiny RPC latency и p99 после idle;
- bulk bandwidth, messages/s и packets/s;
- TTFT по стадиям и tokens/s;
- CPU-seconds/request на Mac и Spark с DEXT/kernel contribution;
- cold setup/MR registration отдельно от warm connection;
- direct-path ratio, copied bytes, ошибки, retries и memory footprint.

Важное исправление к старому предложению: неизменный bulk bandwidth **не означает бесполезность оптимизации**, если достоверно улучшились CPU, TTFT или tail latency при той же корректности.

## 13. Рекомендуемый порядок доведения до готовности

| Этап | Работа | Условие завершения |
|---|---|---|
| A — безопасность | P0.1–P0.4: fencing, ambiguous DMA lifetime, ordering, admin separation | Host/fault tests; нет преждевременного unpin, ложного healthy и reset чужого клиента |
| B — события | Реальная IRQ-доставка, честные feature bits, event-device identity | Armed CQ events доставляются без timer assistance; CPU и p99 измерены |
| C — жизненный цикл | Stop/remove/sleep/recovery, revoke mappings, pinned-byte quotas | Два клиента и повторные циклы отказ/восстановление без утечек и stale access |
| D — shared Metal runtime | Registered-buffer lifetime, slots/credits/epochs, конечные KV/tensor buffers | GPU↔NIC обе стороны, reuse/overlap/memory-pressure без mismatch |
| E — переносимость | M‑Mac/NIC/OS/enclosure matrix, verbs и network conformance | PASS привязан к точным сочетаниям, неизвестные конфигурации не объявлены поддержанными |
| F — релиз и скорость | SIP-on release gate, interleaved TCP/RDMA, полная телеметрия | Воспроизводимый пакет и опубликованные latency/CPU/throughput результаты |

Минимальная аппаратная программа после исправлений: SEND/RECV/READ/WRITE/WRITE_WITH_IMM; поддержанные atomics; tiny/4 KiB/1 MiB/large MR; MTU 2048/4096; ring wrap и mixed direct/fallback; shared-CQ и single-owner; two-client isolation; Metal producer/consumer с изменяющимися sequence-паттернами и guard bytes; allocator reuse; длительный стресс. Известный panic-путь BAR→Metal в неё **не включать**.

**Итог:** проект уже прошёл основной архитектурный барьер M2 — NIC DMA в зарегистрированные страницы, доступные Metal. Дальнейший путь — не «сделать GPU BAR», а закрыть конкретные ошибки аварийного завершения и ordering, получить настоящие IRQ, оформить ownership/lifetime и доказать поддержку каждой конфигурации. Часть zero-copy инференса принципиально должна быть реализована в runtime, а не спрятана в DEXT.

## 14. Идентификация исследованного кода

Исследован изменяемый рабочий каталог, а не объявленный immutable release commit. Для повторного аудита ниже SHA-256 ключевых файлов на момент проверки; они не являются хешами установленного Mach-O.

| Файл внутри src/dext | SHA-256 |
|---|---|
| Sources/MlxPCIDriver.cpp | `293e6636cc7d665480a64d17bbd09167f26b534043bf65a8a65f403a0b5130f3` |
| Sources/core/MlxCmd.cpp | `b1feb6ca2ed4bee0e1f9ab816e1d9288d3c93803a828f54fbfef11ca29ef2531` |
| Sources/core/MlxDMA.cpp | `d05a43b75ffa1e92b6814ba83b4dd19dbb969ad02f544dfde5ff07b119731d2f` |
| Sources/core/MlxEQ.cpp | `7d2157299170380bb785ec531d0a0f64022016c85cb4dee81ad3e7e53d28c03b` |
| Sources/core/MlxHealth.cpp | `54869222825b000428d477276f23724e620fcf9893603fdb541bdb5b58e4cec6` |
| Sources/ib/MlxMR.cpp | `4ebfff3573a8e9c046fa4cc2b1b5748adb80fd36c9957c3c68feaf1bbdde2ad9` |
| Sources/MlxUserClient.cpp | `ccbd20a2ba94e443cf9981805fd4bd1164b145ae7736ef3ab562f190ce0fb5c4` |
| usermode/libibverbs_compat/verbs_compat.c | `cd90efe8141eb1f91ed665773c7b214ac04792bbb92c94844ca4238bcf2cad59` |
