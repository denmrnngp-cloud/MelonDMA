# RDMA inference — рычаги оптимизации (глубокий ресерч)

> 2026-09-03. Сопоставление трёх узких мест (`benchmark-rdma-cluster-2026-09-03.md` §6)
> с техниками из `rdma-intro (1).md`. Упорядочено по ожидаемому эффекту.

Три цели: **TTFT** (KV-хендофф ~65–105 ms на 150–167 MiB), **CPU 9→2.5 %**
(disagg/rdma), **split-decode** (RDMA −12 % vs TCP-40G на мелких активациях).

---

## 1. TTFT — пайплайн one-sided KV WRITE (главный рычаг)

Текущий `rdma_write()` в `transport.cpp` **синхронный**: post чанка → poll
completion → следующий чанк. На 256 KiB чанк это сериализует всю передачу в
цепочку «post→RTT→post→RTT…» — полоса упирается в латентность, а не в линк.

Мануал §11.6 (chunking + channel pipelining): после заполнения пайплайна каждый
слот линка занят — утилизация →100 %. §6.4/§11.7: **RDMA WRITE + WRITE_WITH_IMM**
как сигнал завершения (unsignaled WRITE данных + zero-length `RDMA_WRITE_WITH_IMM`
signaled с `imm_data = size`) — одна RT, без RQ на дата-пути.

**Действие:** переписать `rdma_write` на пайплайн: держать N (например 8–16) WRITE
в полёте, `WRITE_WITH_IMM` в конце батча, один poll на батч. Ожидание: KV-хендофф
перестаёт быть latency-bound, TTFT падает к физике линка.

**Дополнительно:** регистрировать `ctx_tgt` KV-буфер (Metal `contents()`) напрямую и
WRITE в него — убрать `set(local)` (~3.8 ms/150 MiB memcpy) из `disagg_prefill_finish`.

---

## 2. CPU 9→2.5 % — три дешёвых рычага

### 2.1 Batch poll drain (мануал §2.8)

`rdma_poll()` дранит **по одному** WC (`ibv_poll_cq(cq, 1, wc)`). Мануал: «always
drain as many as possible per call». Драйн N комплишенов за вызов сокращает число
poll-вызовов и arm/re-arm циклов (каждый — kernel call или прямой decode).

### 2.2 Unsignaled sends (мануал §2.6)

TX-путь сигналит каждый чанк. Пост N−1 unsignaled + 1 signaled, продвигать
completion-указатель на N — CQE-поток падает в N раз (меньше poll + меньше
comp-channel пробуждений).

### 2.3 Адаптивный backoff comp-channel воркера

Воркер сейчас крутит `MELONDMA_CQ_POLL_US=50 µs` постоянно (~2–3 % ядра). Backoff:
после K пустых проверок подряд спать 500–1000 µs, сбрасывать на первом комплишене.
Убирает постоянную стоимость воркера в простое (disagg между префиллами).

---

## 3. split-decode — batch WR + inline (мелкие активации)

### 3.1 WR-цепочка в один post (мануал §5.3)

Сейчас каждая мелкая активация = отдельный `ibv_post_send` (свой doorbell). Мануал:
`ibv_send_wr.next` — цепочка WR в одном `ibv_post_send`, один doorbell на батч.
Для split (активации на каждый токен) это режет число doorbell-ринг и poll на
порядок.

### 3.2 Inline для ≤256 B (мануал §2.7)

Inline копирует payload в WQE, убирает DMA-fetch. Для мелких активаций (≤256 B)
всегда быстрее. Уже работает для RPC-заголовков; распространить на split-активации.

### 3.3 Apple Metal/UMA direct MR — data path готов, inference integration открыта

DEXT 0.359 по умолчанию поддерживает регистрацию `contents()` от
`.shared | .untracked` `MTLBuffer`. Hardware gate подтвердил GPU→Spark,
Spark→GPU и 4 MiB indirect MR без bounce-copy на Mac. Следующий шаг — сделать
этот буфер финальным `ctx_tgt`/KV layout, а не промежуточным `cmd.dest`.

GPU-native UAR doorbell через Metal **закрыт**: BAR/MMIO не является DRAM и
попытка завернуть его через `bytesNoCopy` вызвала kernel panic. Не повторять.

---

## Приоритет (что делать дальше)

Сделано (2026-09-03): **пайплайн `rdma_write`** (TTFT), **адаптивный backoff
воркера** (CPU, драйвер). Осталось:

1. ~~Пайплайн `rdma_write`~~ ✅ — TTFT (см. `inference-client-guide.md` §12).
2. ~~**Active-direct drain + idle event**~~ ✅ (2026-09-04, DEXT 0.376–0.388).
   Воркер comp-channel блокируется в дехте и просыпается по событию, а опрос
   кольца остался как альтернатива под `MELONDMA_HW_CQ_EVENT=0`. Цена простоя
   взведённого канала: 0.190 % ядра против 2.950 % на опросе. Подробности и
   ручки — `inference-client-guide.md` §10.1.1.

   По дороге закрыты четыре дефекта: порядковый номер взвода CQ увеличивался на
   каждом arm вместо доставленного события; `Poll()` публиковал consumer index
   EQ без бита arm, из-за чего таймерный поллер оставлял кольцо невзведённым;
   воркер уходил блокироваться за уже разоружённую очередь и съедал чужой фронт;
   таймаут ожидания приходил из DriverKit с обнулённой структурой и клиент терял
   снимок generation.

   **Важно для планирования:** MSI-X на этой машине не доставляются ни на одном
   векторе — проверено перепривязкой completion EQ на индекс 0 при реальном
   трафике, оба счётчика драйвера остаются нулевыми. Доставку событий выполняет
   таймер EQ, и его период является нижней границей задержки на этой платформе.
   Причина, скорее всего, вне драйвера: карта захватывается инъекцией в
   IOCatalogue, а не штатным матчингом.
3. ~~Адаптивный backoff воркера~~ ✅ — CPU (в `verbs_compat.c`).
4. **RPC aggregation поверх готовой WR-цепочки** — split-decode.
5. **Финальный Metal KV layout как persistent MR** — главный оставшийся TTFT/CPU
   рычаг; драйвер и hardware gate готовы, нужна интеграция в inference client.
