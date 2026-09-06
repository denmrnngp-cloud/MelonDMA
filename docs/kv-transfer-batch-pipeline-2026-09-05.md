# KV transfer: один batch и конвейер Spark → Mac

Дата: 2026-09-05. Изменения реализованы в приватном `/Users/macstudio/llama.cpp`, а не в DEXT. Цель этой итерации — сократить прежние ≈82 ms переноса при контексте около 2048; не оптимизировать остальные стадии инференса.

## Что реализовано

- Дополнительная capability в HELLO и новый ограниченный wire command `GET_TENSOR_RDMA_BATCH`: до 256 описателей, один запрос на 80 KV-участков, один итоговый WRITE_WITH_IMM token. Старые opcode не перенумерованы. Без capability используется прежний протокол.
- На Spark backend читает данные прямо в уже зарегистрированные TX-slots. Убрана явная копия temporary vector → TX ring. Два поколения по восемь WR перекрывают подготовку следующего окна с передачей предыдущего, в том числе через границы тензоров. Slot не перезаписывается до успешного локального completion.
- Проверяются размеры frame, версия, token, границы source buffer и тензора, overflow и пересечения destination. При ошибке после начала передачи соединение закрывается, клиент аварийно прекращает работу; частичный batch не считается успешным и не переключается на fallback. Структурированного wire error response пока нет.
- Добавлен контракт владения host allocation: `ggml_backend_rpc_host_alloc/free`. Cached MR держит shared ownership памяти до deregistration; виртуального адреса недостаточно для попадания в этот cache. Не более двух retained owned arenas на соединение; освобождённые владельцем области очищаются при следующей регистрации либо закрытии соединения. Это не моментальный возврат всей pinned quota при `host_free`.
- В `disagg_prefill_finish` повторно используется такой host arena, с ростом по 4 MiB и пределом 96 MiB. Не принадлежащая этому allocator память получает краткоживущую регистрацию на batch. Перед освобождением DMA mappings закрывается QP; отказ освобождения destination MR не игнорируется.
- Маркеры `GGML_RPC_KV_BATCH`, `GGML_RPC_KV_TX`, `GGML_RPC_KV_SERVER` привязаны к token. Клиент отдельно показывает fence/reg/request/wait/dereg; `total_ms` в финальной версии включает fence. Первые отладочные логи до добавления `fence_ms` исключали fence из total.

Это **не** запись сразу в окончательные Metal KV tensors. Текущий llama.cpp сначала сериализует состояние в host buffer, затем импортирует его в локальный контекст. Метка destination теперь `host`. `staging_copy_bytes=0` доказывает отсутствие явного memcpy в транспортном TX-пути, но не отсутствие внутренних копий CUDA runtime и не NVIDIA GPUDirect RDMA.

## Важное условие доставки completion

При `MELONDMA_HW_CQ_EVENT=1` и обычном hybrid-профиле обнаружено: Spark завершал synthetic batch примерно за 60 ms, Mac возвращался из ожидания примерно через 105 ms. Переключение на существующий `MELONDMA_COMPLETION_POLICY=latency` устранило хвост; изменение DEXT для этого не требуется.

Latency-профиль использует mapped-CQ poller с короткими ожиданиями вместо длинного аппаратного backstop. Это не доказательство исправленных IRQ и не обещание снижения idle CPU. Активный DEXT при проверке — 0.396; power/hybrid поведение требует отдельной работы.

## Профиль включения

На Mac, дополнительно к уже работающим сетевым переменным MelonDMA:

```sh
export GGML_RPC_RDMA_KV_BATCH=1
export GGML_RPC_RDMA_WRITE_KV=1
export GGML_RPC_RDMA_FINAL_DEST=host
export GGML_RPC_RDMA_DEST_ARENA_MAX=96
export MELONDMA_COMPLETION_POLICY=latency
```

`GGML_RPC_REQUIRE_RDMA=1` запрещает незаметный TCP fallback. На Spark нужна свежая сборка RPC server с batch capability; для трассировки `GGML_RPC_RDMA_STATS=1`. Проверенная пара — Mac GID0, Spark `rocep1s0f1` GID2, MTU4096. Capability и GID index — разные вещи.

Оптимизация пока **opt-in**. `GGML_RPC_RDMA_KV_BATCH=0` возвращает старый путь для A/B. Для областей сверх ограничения, старого peer и непригодного dense span остаётся fallback. Контексты 16K/32K и несколько одновременно работающих клиентов этой итерацией не аттестованы.

## Проверка

Воспроизводимые инструменты в llama.cpp:

- `tools/rpc/kv-transfer-gate.cpp`: настоящий RPC CUDA backend Spark, 80 участков, 90,177,280 payload bytes, меняющийся byte pattern, offsets и guard bytes. 44 чередующихся A/B вызова; отдельно освобождение/рост owned allocation и unowned scoped MR. Blocking probe перед таймером исключает ещё не завершённую загрузку исходных данных. Первые отладочные варианты gate такого probe не имели, их wall нельзя использовать как чистый transfer.
- `tools/rpc/kv-inference-gate.py`: отдельный Mac server 8092 и уже запущенный отдельный Spark RPC 50053, без остановки основного server. По два warmup перед каждой серией, одинаковые промпты между вариантами, `qwen3.6-35b-a3b`, disagg, gen32, mailbox on, q8 KV, allocated context4096. Реальные промпты около 2K, а не искусственно ровно 2048 токенов. Записываются TTFT, CPU Mac и SHA256 сгенерированного текста.
- Mac RDMA сборка, Spark CUDA/RDMA сборка и отдельная Mac сборка `GGML_RPC_RDMA=OFF` проходят. Это compile-проверка TCP-only варианта, не замена hardware gate.
- Реальный старый Spark peer проверен отдельно на 50054: использовались backup executable **и backup библиотеки**, проверенные через `LD_LIBRARY_PATH`/`ldd`. Новый клиент не посылал batch opcode, все 44 проверки данных и проверки смены allocation прошли через прежний wire path. Метка `scoped_mr PASS` в этом compatibility логе означает только правильность данных unowned-теста: старый wire fallback не использует новый scoped batch MR.

Итог: четыре серии A–B–B–A, **20 измеряемых запросов на вариант**, без warmup; 10 одинаковых промптов повторены в каждой паре. Профиль completion=latency одинаков для A и B. Это ограниченный аппаратный A/B, не испытание под конкурентной нагрузкой или с зафиксированными GPU clocks.

| Метрика, среднее | Старый путь | Batch |
| --- | ---: | ---: |
| wait_ms | 81.868 ms | 56.868 ms |
| Полный get(net) | 88.470 ms | 57.890 ms |
| TTFT | 1580.125 ms | 1552.115 ms |
| CPU Mac | 10.425% | 10.815% |

Median/p95 wait: 78.436/93.918 ms → 56.844/57.288 ms. Median/p95 TTFT: 1580.25/1605.0 ms → 1551.9/1574.2 ms. p95 здесь — эмпирическая порядковая статистика всего 20 запросов, не production tail guarantee.

Все 20 пар совпали по prompt_tokens и SHA256 текста; one_sided=80, fallback=0. CPU улучшение **не установлено**: среднее Mac CPU немного выше. Spark CPU-seconds этой проверкой не измерялись. Нельзя выдавать отличие от исторического TTFT 1912 ms за эффект этой правки: сравнивать нужно внутри текущего A/B.

Экономия wait — **25.00 ms (30.5%)**; полного get(net) — **30.58 ms (34.6%)**; наблюдаемый средний TTFT — **28.01 ms (1.77%)**. 25 ms составляют лишь около 1.31% исторических 1912 ms. Все 82 ms не исчезли: физическая передача данных остаётся на критическом пути. Исходные 4.3% были потолком при полном устранении ожидания, а не гарантированным достижимым выигрышем.

Финальный synthetic gate, после исключения четырёх первых A/B вызовов: 20 измерений на вариант, mean wall 77.332 → 61.162 ms, median 76.496 → 61.077 ms, p95 86.096 → 62.328 ms. Все 44 вызова без byte/guard mismatches; отдельно PASS для трёх смен/роста owned allocation и одного unowned scoped MR. Для scoped MR marker показывает реальный dereg_ms, для owned arena — освобождение lease с сохранением MR до смены владельца/закрытия.

После завершения тестов runtime snapshot: DEXT 0.396, epoch=155906052471416, pinned_charge=0, quarantine=0, quota_rejects=0, irq_proven=0. Peak pinned charge=349388800 B. Это штатная очистка ресурсов, не fatal/reset gate.

Логи и JSONL: [dev/artifacts/kv82-20260905](../artifacts/kv82-20260905/). Файлы `kv82-inference-{baseline,batch,baseline2,batch2}-2k.jsonl` содержат TTFT, фактические tokens и hashes; соответствующие `bench-...log` — KV markers. `kv82-gate-acceptance.log` содержит synthetic gate, lifetime и scoped проверки.

После проверки тестовые процессы 8092/50053/50054 остановлены. Основные 8090/50052 не тронуты. Исходники RPC на Mac/Spark сверены SHA256, свежие binaries собраны; для постоянного использования нужен запуск обычного сервера с новым binary и приведённым opt-in профилем. `mlx_hot_update.sh` не запускался: DEXT этой итерацией не менялся.

## Ограничения безопасности и результата

Новая ownership-схема не делает весь прежний raw-VA MR cache безопасным для любых сторонних allocation. Она защищает новые owned arenas, включая legacy wire fallback для них, и scoped batch destinations. Старые неуправляемые назначения требуют отдельного lifetime-контракта.

Не проводились unplug/reset/fatal injection, fuzzing wire frames, Mac↔Mac и тесты других M-чипов. TCP-only compile, согласование capability и побайтовый штатный gate не сертифицируют эти сценарии. Основные процессы на 8090/50052 не перезапускались; новые binaries и исходники не означают, что старый работающий процесс автоматически загрузил их.
