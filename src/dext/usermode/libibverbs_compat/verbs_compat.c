#include <infiniband/verbs.h>
#include "librdma_shim.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct ibv_context {
    struct ibv_device *device;
    rdma_device *dev;
    union ibv_gid gid;
    uint32_t gid_index;
    int gid_programmed;
    uint8_t remote_mac[6];
    uint8_t l3_type;
    uint8_t traffic_class;
    uint8_t hop_limit;
    uint16_t udp_sport;
    int remote_mac_valid;
    struct rdma_fast_path fast_path;
    int fast_path_enabled;
    uint64_t cq_arm_requests;
    uint64_t cq_arm_cached;
    uint64_t completion_waits;
    uint64_t completion_events;
    uint64_t completion_hw_waits;
    uint64_t completion_hw_wakeups;
    uint64_t completion_poll_ticks;
    uint64_t completion_poll_wakeups;
    uint64_t completion_lost_events;
    uint64_t single_owner_violations;
    uint64_t wc_cache_hits;
    uint64_t wc_cache_prefetched;
    struct ibv_cq *cq_list;
    struct ibv_qp *qp_list;
    uint64_t live_objects; /* PD/CQ/channels retain the context */
};

static inline void context_stat_add(uint64_t *counter, uint64_t value)
{
    __atomic_fetch_add(counter, value, __ATOMIC_RELAXED);
}

static inline uint64_t context_stat_get(const uint64_t *counter)
{
    return __atomic_load_n(counter, __ATOMIC_RELAXED);
}

struct melondma_qp_priv {
    rdma_qp *qp;
    int sq_sig_all;
};

/* Large-buffer MR: an indirect (KLM) MR over its chunked direct children. */
struct melondma_mr_priv {
    rdma_mr  *mr;            /* direct MR, or the indirect MR for large buffers */
    rdma_mr **children;      /* child direct MRs (only when indirect) */
    uint32_t  nchildren;
};

struct melondma_comp_channel {
    int write_fd;
    int closing;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    struct ibv_cq *cqs;
    pthread_t worker;
    rdma_device *event_dev;
    int event_dev_owned;
    uint64_t event_generation;
    struct ibv_context *context;
};

static void comp_channel_wait(struct melondma_comp_channel *state, long usec)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += (usec % 1000000L) * 1000L;
    deadline.tv_sec += usec / 1000000L + deadline.tv_nsec / 1000000000L;
    deadline.tv_nsec %= 1000000000L;
    pthread_mutex_lock(&state->lock);
    context_stat_add(&state->context->completion_waits, 1);
    if (!state->closing)
        (void)pthread_cond_timedwait(&state->wake, &state->lock, &deadline);
    pthread_mutex_unlock(&state->lock);
}

static void comp_channel_kick(struct ibv_cq *cq)
{
    if (!cq || !cq->channel) return;
    struct melondma_comp_channel *state = cq->channel->priv;
    /* A hardware-backed channel sleeps on the completion EQ/MSI-X. Posting a
     * WR cannot make a completion visible by itself, so there is no polling
     * worker to kick and no reason to take either hot-path mutex. */
    /* Keep the local kick in hybrid mode even when idle uses DEXT events. */
    pthread_mutex_lock(&cq->notify_lock);
    int armed = cq->notify_armed;
    if (armed) cq->notify_generation++;
    pthread_mutex_unlock(&cq->notify_lock);
    if (!armed) return;
    pthread_mutex_lock(&state->lock);
    pthread_cond_signal(&state->wake);
    pthread_mutex_unlock(&state->lock);
}

/* Ceiling on one blocking wait in the DEXT before the worker re-scans the
 * mapped CQE rings. The wakeup is edge-triggered on a generation counter and
 * the arm doorbell is asynchronous, so hardware can process an arm just after
 * a CQE lands: no event is raised and the CQE waits for the next scan. This
 * bound is therefore a correctness backstop, not a poll interval -- it caps
 * what such a miss costs, and it must stay well above the link's completion
 * latency or it starts serving completions itself. */
static uint32_t melondma_hw_wait_ms(void)
{
    const char *env = getenv("MELONDMA_HW_WAIT_MS");
    if (env) {
        long v = strtol(env, NULL, 10);
        if (v >= 1 && v <= 60000) return (uint32_t)v;
    }
    return 50;
}


static void *comp_channel_worker(void *opaque)
{
    struct ibv_comp_channel *channel = opaque;
    struct melondma_comp_channel *state = channel->priv;
    /* Wakeup granularity while a CQ is attached: trade CPU against latency.
     * rdma_cq_pending_local() reads the mapped CQE ring directly (a few
     * volatile loads, no kernel call), so 50 us costs ~1 % of a core while
     * keeping wakeup (including the re-arm right after a fire) inside the
     * inter-completion gap of a streaming KV transfer. */
    long poll_us = 50;
    const char *env = getenv("MELONDMA_CQ_POLL_US");
    if (env) {
        long v = strtol(env, NULL, 10);
        if (v >= 1 && v <= 10000) poll_us = v;
    }
    /* Adaptive backoff: while armed but idle, ramp the sleep up to backoff_max_us
     * so the worker costs ~0.1 % instead of ~2 %; a fire or a fresh re-arm snaps it
     * back to poll_us so an active transfer still wakes inside ~50 us.  The ceiling
     * is the latency-vs-CPU knob for split decode: a token-stream gap ramps the
     * worker toward the ceiling, and a completion arriving in that window is
     * detected up to `cur_us` late.  Lower MELONDMA_CQ_BACKOFF_MAX_US to bound
     * that (e.g. 100-200 for latency-sensitive split), at the cost of idle CPU. */
    long backoff_max_us = 200;
    const char *backoff_env = getenv("MELONDMA_CQ_BACKOFF_MAX_US");
    if (backoff_env) {
        long v = strtol(backoff_env, NULL, 10);
        if (v >= poll_us && v <= 10000) backoff_max_us = v;
    }
    long cur_us = poll_us;
    int empty_ticks = 0;
    /* Blocking-wait backstop. Flat, not ramped: a bound shorter than the
     * link's own completion latency turns every event-driven wakeup into a
     * timeout poll, which measured far worse in the median than the rare
     * missed event it was meant to shorten. */
    const uint32_t hw_wait = melondma_hw_wait_ms();
    const char *policy = getenv("MELONDMA_COMPLETION_POLICY");
    int latency_only = policy && strcmp(policy, "latency") == 0;
    int power_only = policy && strcmp(policy, "power") == 0;
    struct rdma_runtime_status runtime = {};
    for (;;) {
        pthread_mutex_lock(&state->lock);
        if (state->closing) { pthread_mutex_unlock(&state->lock); break; }
        int any_cq = state->cqs != NULL;
        int any_armed = 0;
        int activity = 0;
        for (struct ibv_cq *cq = state->cqs; cq; cq = cq->channel_next) {
            pthread_mutex_lock(&cq->notify_lock);
            int armed = cq->notify_armed;
            uint64_t event_count = cq->event_count;
            uint64_t generation = cq->notify_generation;
            if (generation != cq->worker_generation) {
                cq->worker_generation = generation;
                activity = 1;
            }
            pthread_mutex_unlock(&cq->notify_lock);
            if (armed) {
                uint64_t completions = 0;
                int rc = rdma_cq_pending_local(cq->priv, &completions);
                if (rc != 0)
                    rc = rdma_query_cq_completions(cq->priv, &completions);
                if (rc == 0 && completions > event_count) {
                    pthread_mutex_lock(&cq->notify_lock);
                    if (cq->notify_armed && completions > cq->event_count) {
                        cq->event_count = completions;
                        cq->notify_armed = 0;
                        if (write(state->write_fd, &cq, sizeof(cq)) !=
                            (ssize_t)sizeof(cq)) {
                            cq->lost_events++;
                            context_stat_add(
                                &state->context->completion_lost_events, 1);
                        } else {
                            context_stat_add(
                                &state->context->completion_poll_wakeups, 1);
                        }
                    }
                    pthread_mutex_unlock(&cq->notify_lock);
                    activity = 1;
                }
                /* Count this CQ as armed only if it is STILL armed after the
                 * pass. Firing above disarms it, and blocking in the DEXT for
                 * a CQ nobody is waiting on consumes the next generation edge
                 * in a window where no one wants it. The edge that belonged to
                 * the next armed window is then gone and the following wait
                 * sits out its whole timeout. */
                pthread_mutex_lock(&cq->notify_lock);
                if (cq->notify_armed) any_armed = 1;
                pthread_mutex_unlock(&cq->notify_lock);
            }
        }
        if (activity) {
            cur_us = poll_us;
            empty_ticks = 0;
        } else if (any_armed && ++empty_ticks > 40) {
            long next = cur_us * 2;
            cur_us = next > backoff_max_us ? backoff_max_us : next;
        } else if (!any_armed) {
            cur_us = poll_us;
            empty_ticks = 0;
        }
        rdma_device *event_dev = state->event_dev;
        pthread_mutex_unlock(&state->lock);
        if (event_dev && any_cq && any_armed && !latency_only &&
            (power_only || empty_ticks > 40)) {
            /* Dedicated connection: the generation can advance via IRQ OR
             * timer. Runtime deltas are evidence, capability bits are not. */
            int have_runtime = rdma_query_runtime(event_dev, &runtime) == 0;
            context_stat_add(&state->context->completion_waits, 1);
            if (runtime.completion_irq_proven)
                context_stat_add(&state->context->completion_hw_waits, 1);
            int wait_rc = rdma_wait_cq_event(
                event_dev, &state->event_generation, hw_wait);
            if (wait_rc == 0 && have_runtime) {
                struct rdma_runtime_status after = {};
                if (rdma_query_runtime(event_dev, &after) == 0 &&
                    after.device_epoch == runtime.device_epoch &&
                    after.irq_completion_eqes > runtime.irq_completion_eqes &&
                    after.timer_completion_eqes == runtime.timer_completion_eqes)
                    context_stat_add(&state->context->completion_hw_wakeups, 1);
            }
            if (wait_rc && wait_rc != -ETIMEDOUT) {
                /* Fail blocked readers with EOF, not an endless stale-ring poll. */
                pthread_mutex_lock(&state->lock);
                close(state->write_fd); state->write_fd = -1;
                pthread_mutex_unlock(&state->lock);
                break;
            }
        } else {
            if (any_cq && any_armed)
                context_stat_add(&state->context->completion_poll_ticks, 1);
            comp_channel_wait(state, any_cq && any_armed ? cur_us : 1000);
        }
    }
    return NULL;
}

#define MELONDMA_MAX_DEVICES 16
static struct ibv_device g_devices[MELONDMA_MAX_DEVICES];

static const char *env_first(const char *primary, const char *legacy)
{
    const char *value = getenv(primary);
    return (value && value[0]) ? value : getenv(legacy);
}

static int parse_mac(const char *text, uint8_t mac[6])
{
    unsigned int value[6];
    if (!text || sscanf(text, "%x:%x:%x:%x:%x:%x", &value[0], &value[1],
                        &value[2], &value[3], &value[4], &value[5]) != 6)
        return -1;
    for (unsigned int i = 0; i < 6; i++) {
        if (value[i] > 255) return -1;
        mac[i] = (uint8_t)value[i];
    }
    return 0;
}

static int parse_gid(const char *text, union ibv_gid *gid, uint8_t *l3_type)
{
    if (!text || !gid || !l3_type) return -1;
    if (inet_pton(AF_INET6, text, gid->raw) == 1) {
        *l3_type = 1;
        return 0;
    }
    uint8_t ipv4[4];
    if (inet_pton(AF_INET, text, ipv4) != 1) return -1;
    memset(gid, 0, sizeof(*gid));
    gid->raw[10] = 0xff;
    gid->raw[11] = 0xff;
    memcpy(gid->raw + 12, ipv4, sizeof(ipv4));
    *l3_type = 0;
    return 0;
}

static int configure_roce(struct ibv_context *context,
                          const struct ibv_mlx5_roce_config *config)
{
    if (!context || !config || config->l3_type > 1) return EINVAL;
    uint32_t gid_index = 0;
    if (rdma_set_roce_address(context->dev, config->local_gid.raw,
                              config->local_mac, config->l3_type,
                              &gid_index) != 0)
        return EIO;
    if (context->gid_programmed)
        (void)rdma_clear_roce_address(context->dev, context->gid_index);
    context->gid = config->local_gid;
    context->gid_index = gid_index;
    context->gid_programmed = 1;
    memcpy(context->remote_mac, config->peer_mac, sizeof(context->remote_mac));
    context->remote_mac_valid = 1;
    context->l3_type = config->l3_type;
    context->traffic_class = config->traffic_class;
    context->hop_limit = config->hop_limit;
    context->udp_sport = config->udp_sport;
    return 0;
}

static void configure_roce_from_env(struct ibv_context *context)
{
    const char *local_ip = env_first("MELONDMA_LOCAL_IP", "MAC_ROCE_IP");
    const char *local_mac_text = env_first("MELONDMA_LOCAL_MAC", "MAC_ROCE_MAC");
    const char *peer_mac_text = env_first("MELONDMA_REMOTE_MAC", "SPARK_ROCE_MAC");
    struct ibv_mlx5_roce_config config = { .hop_limit = 1 };
    if (!local_ip || !local_mac_text || !peer_mac_text ||
        parse_gid(local_ip, &config.local_gid, &config.l3_type) ||
        parse_mac(local_mac_text, config.local_mac) ||
        parse_mac(peer_mac_text, config.peer_mac))
        return;
    (void)configure_roce(context, &config);
}

static enum ibv_mtu mtu_from_bytes(uint32_t bytes)
{
    switch (bytes) {
    case 256: return IBV_MTU_256;
    case 512: return IBV_MTU_512;
    case 1024: return IBV_MTU_1024;
    case 2048: return IBV_MTU_2048;
    case 4096: return IBV_MTU_4096;
    default: return IBV_MTU_1024;
    }
}

struct ibv_device **ibv_get_device_list(int *num_devices)
{
    if (num_devices) *num_devices = 0;
    char **names = NULL;
    int count = 0;
    if (rdma_list_devices(&names, &count) != 0 || count <= 0) {
        rdma_free_names(names, count > 0 ? count : 0);
        return NULL;
    }
    int name_count = count;
    if (count > MELONDMA_MAX_DEVICES) count = MELONDMA_MAX_DEVICES;
    struct ibv_device **list = calloc((size_t)count + 1, sizeof(*list));
    if (!list) { rdma_free_names(names, name_count); return NULL; }
    for (int i = 0; i < count; i++) {
        memset(&g_devices[i], 0, sizeof(g_devices[i]));
        list[i] = &g_devices[i];
        snprintf(list[i]->name, sizeof(list[i]->name),
                 count == 1 ? "mlx5_0" : "mlx5_%d", i);
    }
    rdma_free_names(names, name_count);
    if (num_devices) *num_devices = count;
    return list;
}

void ibv_free_device_list(struct ibv_device **list)
{
    free(list);
}

const char *ibv_get_device_name(struct ibv_device *device)
{
    return device ? device->name : NULL;
}

struct ibv_context *ibv_open_device(struct ibv_device *device)
{
    if (!device) { errno = EINVAL; return NULL; }
    rdma_device *rdma = rdma_open_device_by_name(device->name);
    if (!rdma) { errno = ENODEV; return NULL; }
    struct ibv_context *context = calloc(1, sizeof(*context));
    if (!context) { rdma_close_device(rdma); errno = ENOMEM; return NULL; }
    context->device = device;
    context->dev = rdma;

    /* Legacy environment configuration is deliberately optional. New clients
     * call ibv_mlx5_configure_roce() after resolving their control endpoint. */
    configure_roce_from_env(context);
    return context;
}

int ibv_close_device(struct ibv_context *context)
{
    if (!context) return EINVAL;
    if (__atomic_load_n(&context->live_objects, __ATOMIC_ACQUIRE)) return EBUSY;
    if (context->gid_programmed)
        (void)rdma_clear_roce_address(context->dev, context->gid_index);
    if (context->fast_path_enabled)
        rdma_unmap_fast_path(context->dev);
    rdma_close_device(context->dev);
    free(context);
    return 0;
}

int ibv_mlx5_configure_roce(struct ibv_context *context,
                             const struct ibv_mlx5_roce_config *config)
{
    return configure_roce(context, config);
}

int ibv_mlx5_query_perf(struct ibv_context *context,
                        struct ibv_mlx5_perf *perf)
{
    if (!context || !perf) return EINVAL;
    struct rdma_perf native = {};
    if (rdma_query_perf(context->dev, &native) != 0) return EIO;
    perf->external_methods = native.external_methods;
    perf->external_method_ns = native.external_method_ns;
    perf->post_send_calls = native.post_send_calls;
    perf->post_recv_calls = native.post_recv_calls;
    perf->poll_cq_calls = native.poll_cq_calls;
    perf->sync_fast_path_calls = native.sync_fast_path_calls;
    perf->sync_qp_tails_calls = native.sync_qp_tails_calls;
    perf->arm_cq_calls = native.arm_cq_calls;
    perf->doorbells = native.doorbells;
    perf->cqe_consumed = native.cqe_consumed;
    perf->cqe_errors = native.cqe_errors;
    perf->mr_registers = native.mr_registers;
    perf->mr_deregisters = native.mr_deregisters;
    perf->mr_bytes = native.mr_bytes;
    perf->copied_bytes = native.copied_bytes;
    perf->cq_events = native.cq_events;
    perf->cq_event_wakeups = native.cq_event_wakeups;
    return 0;
}

const char *ibv_mlx5_irq_stage_name(uint32_t stage)
{
    switch (stage) {
    case IBV_MLX5_IRQ_STAGE_NONE:          return "none";
    case IBV_MLX5_IRQ_STAGE_CONFIGURE:     return "ConfigureInterrupts";
    case IBV_MLX5_IRQ_STAGE_QUEUE:         return "IODispatchQueue::Create";
    case IBV_MLX5_IRQ_STAGE_SOURCE:        return "IOInterruptDispatchSource::Create";
    case IBV_MLX5_IRQ_STAGE_ACTION:        return "CreateAction";
    case IBV_MLX5_IRQ_STAGE_HANDLER:       return "SetHandler";
    case IBV_MLX5_IRQ_STAGE_ENABLE:        return "SetEnableWithCompletion";
    case IBV_MLX5_IRQ_STAGE_NOT_ATTEMPTED: return "not attempted";
    default:                               return "unknown";
    }
}

int ibv_mlx5_modify_cq_moderation(struct ibv_cq *cq, uint32_t period,
                                  uint32_t max_count)
{
    if (!cq || !cq->priv) return EINVAL;
    int rc = rdma_modify_cq_moderation((rdma_cq *)cq->priv, period, max_count);
    if (rc == -ENOTSUP) return ENOTSUP;
    if (rc == -EINVAL) return EINVAL;
    return rc == 0 ? 0 : EIO;
}

const char *ibv_mlx5_cqeq_stage_name(uint32_t stage)
{
    switch (stage) {
    case IBV_MLX5_CQEQ_STAGE_NOT_ATTEMPTED: return "not attempted";
    case IBV_MLX5_CQEQ_STAGE_ALLOC:         return "alloc";
    case IBV_MLX5_CQEQ_STAGE_INIT:          return "ring init";
    case IBV_MLX5_CQEQ_STAGE_CREATE:        return "CREATE_EQ";
    case IBV_MLX5_CQEQ_STAGE_OK:            return "ok";
    default:                                return "unknown";
    }
}

int ibv_mlx5_query_interrupts(struct ibv_context *context,
                              struct ibv_mlx5_interrupts *irq)
{
    if (!context || !irq) return EINVAL;
    struct rdma_interrupt_attr native = {};
    int rc = rdma_query_interrupts(context->dev, &native);
    if (rc == -ENOTSUP) return ENOTSUP;
    if (rc != 0) return EIO;
    irq->vectors = native.vectors;
    irq->setup_status = native.setup_status;
    irq->setup_stage = native.setup_stage;
    irq->async_eqn = native.async_eqn;
    irq->completion_eqn = native.completion_eqn;
    irq->completion_ready = native.completion_ready;
    irq->completion_events = native.completion_events;
    irq->completion_eq_status = native.completion_eq_status;
    irq->completion_eq_stage = native.completion_eq_stage;
    irq->completion_eq_syndrome = native.completion_eq_syndrome;
    irq->completion_eq_fw_status = native.completion_eq_fw_status;
    irq->completion_eq_variant = native.completion_eq_variant;
    irq->completion_eq_variant_tried = native.completion_eq_variant_tried;
    for (int i = 0; i < 4; i++)
        irq->completion_eq_variant_syndrome[i] =
            native.completion_eq_variant_syndrome[i];
    irq->async_interrupts = native.async_interrupts;
    irq->completion_interrupts = native.completion_interrupts;
    irq->eq_timer_ticks = native.eq_timer_ticks;
    irq->eq_timer_period_ms = native.eq_timer_period_ms;
    return 0;
}

int ibv_mlx5_query_runtime(struct ibv_context *context, void *out, size_t size)
{
    if (!context || !out || size < 8) return EINVAL;
    struct rdma_runtime_status r = {};
    int rc = rdma_query_runtime(context->dev, &r);
    if (rc) return -rc;
    _Static_assert(sizeof(r) == sizeof(struct ibv_mlx5_runtime), "runtime v1 layout");
    memcpy(out, &r, size < sizeof(r) ? size : sizeof(r));
    return 0;
}

int ibv_mlx5_query_telemetry_ex(struct ibv_context *context, void *out, size_t size)
{
    if (!out || size < 8) return EINVAL;
    struct ibv_mlx5_telemetry t = {};
    int rc = ibv_mlx5_query_telemetry(context, &t);
    if (!rc) memcpy(out, &t, size < sizeof(t) ? size : sizeof(t));
    return rc;
}

int ibv_mlx5_query_telemetry(struct ibv_context *context,
                             struct ibv_mlx5_telemetry *telemetry)
{
    if (!context || !telemetry) return EINVAL;
    struct rdma_perf kernel = {};
    struct rdma_fast_path_stats direct = {};
    if (rdma_query_perf(context->dev, &kernel) != 0 ||
        rdma_fast_path_get_stats(context->dev, &direct) != 0)
        return EIO;
    memset(telemetry, 0, sizeof(*telemetry));
    telemetry->version = IBV_MLX5_TELEMETRY_VERSION;
    telemetry->size = (uint32_t)sizeof(*telemetry);
    telemetry->external_methods = kernel.external_methods;
    telemetry->external_method_ns = kernel.external_method_ns;
    telemetry->kernel_post_send_calls = kernel.post_send_calls;
    telemetry->kernel_post_recv_calls = kernel.post_recv_calls;
    telemetry->kernel_poll_cq_methods = kernel.poll_cq_calls;
    telemetry->kernel_sync_fast_path_calls = kernel.sync_fast_path_calls;
    telemetry->kernel_sync_qp_tails_calls = kernel.sync_qp_tails_calls;
    telemetry->kernel_arm_cq_calls = kernel.arm_cq_calls;
    telemetry->mr_registers = kernel.mr_registers;
    telemetry->mr_deregisters = kernel.mr_deregisters;
    telemetry->mr_bytes = kernel.mr_bytes;
    telemetry->copied_bytes = kernel.copied_bytes;
    telemetry->mapped_qps = direct.mapped_qps;
    telemetry->direct_send_batches = direct.direct_send_batches;
    telemetry->direct_send_wrs = direct.direct_send_wrs;
    telemetry->direct_doorbells = direct.direct_doorbells;
    telemetry->direct_recv_batches = direct.direct_recv_batches;
    telemetry->direct_recv_wrs = direct.direct_recv_wrs;
    telemetry->cq_consumer_publications = direct.direct_cq_consumers;
    telemetry->shadow_publications = direct.shadow_publications;
    telemetry->blue_flame_wqes = direct.blue_flame_wqes;
    telemetry->direct_poll_calls = direct.direct_poll_calls;
    telemetry->direct_poll_empty = direct.direct_poll_empty;
    telemetry->direct_cqes = direct.direct_cqes;
    telemetry->direct_cqe_errors = direct.direct_cqe_errors;
    telemetry->kernel_poll_calls = direct.kernel_poll_calls;
    telemetry->kernel_cqes = direct.kernel_cqes;
    telemetry->fallback_direct_disabled = direct.fallback_direct_disabled;
    telemetry->fallback_cq_unmapped = direct.fallback_cq_unmapped;
    telemetry->fallback_unknown_qp = direct.fallback_unknown_qp;
    telemetry->fallback_missing_metadata = direct.fallback_missing_metadata;
    telemetry->fallback_dext_owned = direct.fallback_dext_owned;
    telemetry->cq_arm_requests = context_stat_get(&context->cq_arm_requests);
    telemetry->cq_arm_cached = context_stat_get(&context->cq_arm_cached);
    telemetry->completion_waits = context_stat_get(&context->completion_waits);
    telemetry->completion_events = context_stat_get(&context->completion_events);
    telemetry->completion_hw_waits = context_stat_get(&context->completion_hw_waits);
    telemetry->completion_hw_wakeups = context_stat_get(&context->completion_hw_wakeups);
    telemetry->completion_poll_ticks = context_stat_get(&context->completion_poll_ticks);
    telemetry->completion_poll_wakeups = context_stat_get(&context->completion_poll_wakeups);
    telemetry->completion_lost_events = context_stat_get(&context->completion_lost_events);
    telemetry->single_owner_violations = context_stat_get(&context->single_owner_violations);
    telemetry->wc_cache_hits = context_stat_get(&context->wc_cache_hits);
    telemetry->wc_cache_prefetched = context_stat_get(&context->wc_cache_prefetched);
    telemetry->kernel_doorbells = kernel.doorbells;
    telemetry->kernel_cqe_errors = kernel.cqe_errors;
    telemetry->driver_cq_events = kernel.cq_events;
    telemetry->driver_cq_event_wakeups = kernel.cq_event_wakeups;
    return 0;
}

int ibv_mlx5_query_cong(struct ibv_context *context,
                        struct ibv_mlx5_cong_params *params)
{
    if (!context || !params) return EINVAL;
    struct rdma_cong_params native = {};
    if (rdma_query_cong(context->dev, &native) != 0) return EIO;
    params->rpg_min_dec_fac = native.rpg_min_dec_fac;
    params->rpg_ai_rate = native.rpg_ai_rate;
    params->rpg_time_reset = native.rpg_time_reset;
    params->rpg_threshold = native.rpg_threshold;
    params->rpg_hai = native.rpg_hai;
    params->rpg_gd = native.rpg_gd;
    params->rpg_time_inc = native.rpg_time_inc;
    params->rsvd = 0;
    return 0;
}

int ibv_mlx5_modify_cong(struct ibv_context *context,
                         const struct ibv_mlx5_cong_params *params)
{
    if (!context || !params) return EINVAL;
    struct rdma_cong_params native = {
        .rpg_min_dec_fac = params->rpg_min_dec_fac,
        .rpg_ai_rate = params->rpg_ai_rate,
        .rpg_time_reset = params->rpg_time_reset,
        .rpg_threshold = params->rpg_threshold,
        .rpg_hai = params->rpg_hai,
        .rpg_gd = params->rpg_gd,
        .rpg_time_inc = params->rpg_time_inc,
    };
    return rdma_modify_cong(context->dev, &native) == 0 ? 0 : EIO;
}

int ibv_mlx5_add_gid(struct ibv_context *context, const union ibv_gid *gid,
                     const uint8_t mac[6], uint8_t l3_type,
                     uint16_t vlan_id, uint8_t vlan_valid,
                     uint32_t *gid_index)
{
    if (!context || !gid || !mac || !gid_index) return EINVAL;
    if (rdma_set_roce_address_vlan(context->dev, gid->raw, mac, l3_type,
                                   vlan_id, vlan_valid, gid_index) != 0)
        return EIO;
    return 0;
}

int ibv_mlx5_del_gid(struct ibv_context *context, uint32_t gid_index)
{
    if (!context) return EINVAL;
    return rdma_clear_roce_address(context->dev, gid_index) == 0 ? 0 : EIO;
}

int ibv_query_device(struct ibv_context *context,
                     struct ibv_device_attr *attr)
{
    if (!context || !attr) return EINVAL;
    struct rdma_device_attr source = {0};
    if (rdma_query_device(context->dev, &source) != 0) return EIO;
    memset(attr, 0, sizeof(*attr));
    attr->fw_ver = source.fw_version;
    attr->vendor_id = 0x15b3;
    attr->vendor_part_id = source.device_id;
    attr->max_qp = source.max_qp > INT_MAX ? INT_MAX : (int)source.max_qp;
    attr->max_cq = source.max_cq > INT_MAX ? INT_MAX : (int)source.max_cq;
    attr->max_mr = source.max_mr > INT_MAX ? INT_MAX : (int)source.max_mr;
    attr->max_pd = attr->max_qp;
    struct rdma_abi_attr abi = {};
    int multi_sge = rdma_query_abi(context->dev, &abi) == 0 &&
                    (abi.features & RDMA_FEATURE_MULTI_SGE);
    attr->max_sge = multi_sge ? RDMA_MAX_SGE : 1;
    attr->max_sge_rd = attr->max_sge;
    attr->max_sge_qp = attr->max_sge;
    attr->max_qp_wr = 32768;
    attr->max_cqe = 2048;
    attr->max_mr_size = source.max_msg_size > INT_MAX ?
        INT_MAX : (int)source.max_msg_size;
    attr->max_inline_data = source.max_inline_data > INT_MAX ?
        INT_MAX : (int)source.max_inline_data;
    attr->max_qp_rd_atom = source.max_qp_rd_atom > INT_MAX ?
        INT_MAX : (int)source.max_qp_rd_atom;
    attr->max_qp_init_rd_atom = source.max_qp_init_rd_atom > INT_MAX ?
        INT_MAX : (int)source.max_qp_init_rd_atom;
    attr->page_size_cap = 4096;
    attr->phys_port_cnt = source.num_ports > UINT8_MAX ?
        UINT8_MAX : (uint8_t)source.num_ports;
    return 0;
}

int ibv_query_port(struct ibv_context *context, uint8_t port_num,
                   struct ibv_port_attr *attr)
{
    if (!context || !attr || port_num != 1) return EINVAL;
    struct rdma_port_attr source = {0};
    if (rdma_query_port(context->dev, &source) != 0) return EIO;
    memset(attr, 0, sizeof(*attr));
    attr->state = source.port_state ? IBV_PORT_ACTIVE : IBV_PORT_DOWN;
    /* QUERY_PORT returns the verbs/PRM MTU enum (1..5), not a byte count.
     * Treating enum 5 as bytes fell through to IBV_MTU_1024, so the Mac QP
     * was programmed for 1024 while the DGX peer used 4096.  Any SEND over
     * 1024 then terminated the responder with LOCAL_LENGTH. */
    attr->max_mtu = source.max_mtu >= IBV_MTU_256 &&
                    source.max_mtu <= IBV_MTU_4096 ?
                        (enum ibv_mtu)source.max_mtu :
                        mtu_from_bytes(source.max_mtu);
    attr->active_mtu = attr->max_mtu;
    attr->gid_tbl_len = source.gid_tbl_len;
    attr->max_msg_sz = UINT32_MAX;
    attr->active_speed = source.active_speed_mbps;
    attr->pkey_tbl_len = source.pkey_tbl_len;
    attr->link_layer = source.link_layer == 2 ? IBV_LINK_LAYER_ETHERNET :
                                                IBV_LINK_LAYER_INFINIBAND;
    return 0;
}

int ibv_query_gid(struct ibv_context *context, uint8_t port_num, int index,
                  union ibv_gid *gid)
{
    if (!context || !gid || port_num != 1 || index < 0) return EINVAL;
    struct rdma_gid_attr source = {};
    int rc = rdma_query_gid(context->dev, (uint32_t)index, &source);
    if (rc) return rc == -ENOENT ? ENODATA : EIO;
    memcpy(gid->raw, source.gid, sizeof(gid->raw));
    return 0;
}

int ibv_query_gid_ex(struct ibv_context *context, uint32_t port_num,
                     uint32_t gid_index, struct ibv_gid_entry *entry,
                     uint32_t flags)
{
    if (!context || !entry || flags || port_num != 1) return EINVAL;
    struct rdma_gid_attr source = {};
    int rc = rdma_query_gid(context->dev, gid_index, &source);
    if (rc) return rc == -ENOENT ? ENODATA : EIO;
    memset(entry, 0, sizeof(*entry));
    memcpy(entry->gid.raw, source.gid, sizeof(entry->gid.raw));
    entry->gid_index = gid_index;
    entry->port_num = port_num;
    entry->gid_type = source.gid_type == 2 ? IBV_GID_TYPE_ROCE_V2 : IBV_GID_TYPE_IB;
    entry->ndev_ifindex = source.ifindex;
    return 0;
}

int ibv_query_gid_table(struct ibv_context *context, uint32_t port_num,
                        struct ibv_gid_entry *entries, uint32_t capacity,
                        uint32_t *count, uint32_t *table_size)
{
    if (!context || !entries || !capacity || !count || !table_size ||
        port_num != 1)
        return EINVAL;
    struct rdma_gid_table_entry *raw =
        (struct rdma_gid_table_entry *)calloc(capacity, sizeof(*raw));
    if (!raw) return ENOMEM;
    int rc = rdma_query_gid_table(context->dev, raw, capacity, count, table_size);
    if (rc) { free(raw); return rc == -EINVAL ? EINVAL : EIO; }
    for (uint32_t i = 0; i < *count; i++) {
        memset(&entries[i], 0, sizeof(entries[i]));
        memcpy(entries[i].gid.raw, raw[i].gid, 16);
        entries[i].gid_index = raw[i].index;
        entries[i].port_num = port_num;
        entries[i].gid_type = raw[i].gid_type == 2 ?
                              IBV_GID_TYPE_ROCE_V2 : IBV_GID_TYPE_IB;
        entries[i].ndev_ifindex = raw[i].ifindex;
    }
    free(raw);
    return 0;
}

int ibv_get_async_event(struct ibv_context *context,
                        struct ibv_async_event *event)
{
    if (!context || !event) return EINVAL;
    struct rdma_async_event native = {};
    int rc = rdma_get_async_event(context->dev, &native);
    if (rc) return rc;
    memset(event, 0, sizeof(*event));
    event->event_type = (enum ibv_event_type)native.event_type;
    if (native.element_type == 3) { /* MLX_ASYNC_ELEMENT_PORT */
        event->element.port_num = (uint8_t)native.element_handle;
    } else if (native.element_type == 1) { /* MLX_ASYNC_ELEMENT_CQ */
        rdma_cq *cq = rdma_find_cq(context->dev, native.element_handle);
        if (!cq) return ENODEV;
        for (struct ibv_cq *candidate = context->cq_list;
             candidate; candidate = candidate->context_next) {
            if (candidate->priv == cq) {
                event->element.cq = candidate;
                return 0;
            }
        }
        return ENODEV;
    } else if (native.element_type == 2) { /* MLX_ASYNC_ELEMENT_QP */
        rdma_qp *qp = rdma_find_qp(context->dev, native.element_handle);
        if (!qp) return ENODEV;
        for (struct ibv_qp *candidate = context->qp_list;
             candidate; candidate = candidate->context_next) {
            if (candidate->priv == qp) {
                event->element.qp = candidate;
                return 0;
            }
        }
        return ENODEV;
    }
    return 0;
}

void ibv_ack_async_event(struct ibv_async_event *event)
{
    (void)event;
}

struct ibv_pd *ibv_alloc_pd(struct ibv_context *context)
{
    if (!context) { errno = EINVAL; return NULL; }
    rdma_pd *rdma = rdma_alloc_pd(context->dev);
    if (!rdma) { errno = EIO; return NULL; }
    struct ibv_pd *pd = calloc(1, sizeof(*pd));
    if (!pd) { (void)rdma_dealloc_pd(rdma); errno = ENOMEM; return NULL; }
    pd->context = context;
    pd->priv = rdma;
    __atomic_fetch_add(&context->live_objects, 1, __ATOMIC_RELAXED);

    /* DEXT allocates the isolated client bundle only after PD creation.
     * Keep an explicit opt-out for bring-up and old DEXTs; a failed enable
     * still falls back to the kernel-mediated posting path. */
    const char *fast = getenv("MELONDMA_FAST_PATH");
    if ((!fast || strcmp(fast, "0") != 0) &&
        rdma_enable_fast_path(context->dev, &context->fast_path) == 0 &&
        rdma_map_fast_path(context->dev, &context->fast_path) == 0)
        context->fast_path_enabled = 1;
    return pd;
}

int ibv_dealloc_pd(struct ibv_pd *pd)
{
    if (!pd) return EINVAL;
    struct ibv_context *context = pd->context;
    int rc = rdma_dealloc_pd((rdma_pd *)pd->priv);
    if (rc) return rc == -EBUSY ? EBUSY : EIO;
    if (context->fast_path_enabled &&
        __atomic_load_n(&context->live_objects, __ATOMIC_RELAXED) == 1) {
        rdma_unmap_fast_path(context->dev);
        context->fast_path_enabled = 0;
    }
    __atomic_fetch_sub(&context->live_objects, 1, __ATOMIC_RELEASE);
    free(pd);
    return 0;
}

struct ibv_ah *ibv_create_ah(struct ibv_pd *pd, struct ibv_ah_attr *attr)
{
    if (!pd || !attr || !attr->is_global || !attr->port_num) {
        errno = EINVAL;
        return NULL;
    }
    struct rdma_ah_attr native = {
        .sgid_index = attr->grh.sgid_index,
        .hop_limit = attr->grh.hop_limit,
        .traffic_class = attr->grh.traffic_class,
        .port_num = attr->port_num,
    };
    memcpy(native.dgid, attr->grh.dgid.raw, sizeof(native.dgid));
    memcpy(native.dmac, pd->context->remote_mac, sizeof(native.dmac));
    native.udp_sport = pd->context->udp_sport;
    if (!pd->context->remote_mac_valid) { errno = EDESTADDRREQ; return NULL; }
    rdma_ah *rdma = rdma_create_ah((rdma_pd *)pd->priv, &native);
    if (!rdma) { errno = EIO; return NULL; }
    struct ibv_ah *ah = calloc(1, sizeof(*ah));
    if (!ah) { (void)rdma_destroy_ah(rdma); errno = ENOMEM; return NULL; }
    ah->pd = pd;
    ah->priv = rdma;
    return ah;
}

int ibv_destroy_ah(struct ibv_ah *ah)
{
    if (!ah) return EINVAL;
    int rc = rdma_destroy_ah((rdma_ah *)ah->priv);
    if (!rc) free(ah);
    return rc ? EIO : 0;
}

struct ibv_comp_channel *ibv_create_comp_channel(struct ibv_context *context)
{
    if (!context) { errno = EINVAL; return NULL; }
    int fd[2];
    if (pipe(fd) != 0) return NULL;
    struct ibv_comp_channel *channel = calloc(1, sizeof(*channel));
    struct melondma_comp_channel *state = calloc(1, sizeof(*state));
    if (!channel || !state || pthread_mutex_init(&state->lock, NULL) != 0) {
        free(channel); free(state); close(fd[0]); close(fd[1]); errno = ENOMEM;
        return NULL;
    }
    if (pthread_cond_init(&state->wake, NULL) != 0) {
        pthread_mutex_destroy(&state->lock); free(state); free(channel);
        close(fd[0]); close(fd[1]); errno = ENOMEM; return NULL;
    }
    if (fcntl(fd[1], F_SETFL, fcntl(fd[1], F_GETFL) | O_NONBLOCK) != 0) {
        pthread_cond_destroy(&state->wake);
        pthread_mutex_destroy(&state->lock); free(state); free(channel);
        close(fd[0]); close(fd[1]); return NULL;
    }
    channel->context = context;
    channel->fd = fd[0];
    channel->priv = state;
    state->write_fd = fd[1];
    state->context = context;
    /* MELONDMA_HW_CQ_EVENT=0 keeps the worker on the mapped-ring poller even
     * when the provider offers blocking delivery. Needed to A/B the two paths
     * against one driver build, and as a safety valve if the blocking path
     * regresses on a given card. */
    const char *hw_event = getenv("MELONDMA_HW_CQ_EVENT");
    int hw_event_off = hw_event && strcmp(hw_event, "0") == 0;
    struct rdma_abi_attr abi = {};
    if (!hw_event_off && rdma_query_abi(context->dev, &abi) == 0 &&
        (abi.features & (RDMA_FEATURE_CQ_INTERRUPT | RDMA_FEATURE_CQ_EVENT_WAIT))) {
        /* Keep completion waits on the exact context selected by the caller.
         * Opening by a synthesized name can bind the wrong HCA in a
         * multi-device process and also creates a second UserClient session. */
        state->event_dev = context->dev;
        state->event_dev_owned = 0;
    }
    if (pthread_create(&state->worker, NULL, comp_channel_worker, channel) != 0) {
        if (state->event_dev && state->event_dev_owned)
            rdma_close_device(state->event_dev);
        pthread_cond_destroy(&state->wake);
        pthread_mutex_destroy(&state->lock); free(state); free(channel);
        close(fd[0]); close(fd[1]); return NULL;
    }
    __atomic_fetch_add(&context->live_objects, 1, __ATOMIC_RELAXED);
    return channel;
}

int ibv_destroy_comp_channel(struct ibv_comp_channel *channel)
{
    if (!channel) return EINVAL;
    struct melondma_comp_channel *state = channel->priv;
    pthread_mutex_lock(&state->lock);
    if (state->cqs) { pthread_mutex_unlock(&state->lock); return EBUSY; }
    state->closing = 1;
    pthread_cond_signal(&state->wake);
    pthread_mutex_unlock(&state->lock);
    pthread_join(state->worker, NULL);
    if (state->event_dev && state->event_dev_owned)
        rdma_close_device(state->event_dev);
    close(channel->fd); close(state->write_fd);
    pthread_cond_destroy(&state->wake);
    pthread_mutex_destroy(&state->lock);
    __atomic_fetch_sub(&channel->context->live_objects, 1, __ATOMIC_RELEASE);
    free(state); free(channel);
    return 0;
}

struct ibv_cq *ibv_create_cq(struct ibv_context *context, int cqe,
                             void *cq_context,
                             struct ibv_comp_channel *channel,
                             int comp_vector)
{
    if (!context || cqe <= 0 || comp_vector != 0 ||
        (channel && channel->context != context)) {
        errno = EINVAL;
        return NULL;
    }
    rdma_cq *rdma = rdma_create_cq(context->dev, (uint32_t)cqe);
    if (!rdma) { errno = EIO; return NULL; }
    struct ibv_cq *cq = calloc(1, sizeof(*cq));
    if (!cq) { (void)rdma_destroy_cq(rdma); errno = ENOMEM; return NULL; }
    cq->context = context;
    cq->cqe = cqe;
    cq->cq_context = cq_context;
    cq->priv = rdma;
    cq->channel = channel;
    const char *single = getenv("MELONDMA_SINGLE_THREADED");
    cq->single_threaded = single && strcmp(single, "0") != 0;
    /* MELONDMA_CQ_MODERATION="<period_us>:<max_count>" asks the NIC to hold a
     * completion event back until either bound is reached, so a streaming
     * transfer raises one event per batch. Off unless set: moderation trades
     * latency for wakeups, and the right point is workload-specific. */
    const char *mod = getenv("MELONDMA_CQ_MODERATION");
    if (mod && mod[0]) {
        char *end = NULL;
        long period = strtol(mod, &end, 10);
        long max_count = (end && *end == ':') ? strtol(end + 1, NULL, 10) : 0;
        if (period >= 0 && period <= 4095 && max_count >= 0 && max_count <= 65535)
            (void)rdma_modify_cq_moderation(rdma, (uint32_t)period,
                                            (uint32_t)max_count);
    }
    if (pthread_mutex_init(&cq->notify_lock, NULL) != 0) {
        (void)rdma_destroy_cq(rdma); free(cq); errno = ENOMEM; return NULL;
    }
    if (pthread_mutex_init(&cq->poll_lock, NULL) != 0) {
        pthread_mutex_destroy(&cq->notify_lock);
        (void)rdma_destroy_cq(rdma); free(cq); errno = ENOMEM; return NULL;
    }
    if (channel) {
        struct melondma_comp_channel *state = channel->priv;
        pthread_mutex_lock(&state->lock);
        cq->channel_next = state->cqs;
        state->cqs = cq;
        pthread_cond_signal(&state->wake);
        pthread_mutex_unlock(&state->lock);
    }
    cq->context_next = context->cq_list;
    context->cq_list = cq;
    __atomic_fetch_add(&context->live_objects, 1, __ATOMIC_RELAXED);
    return cq;
}

int ibv_destroy_cq(struct ibv_cq *cq)
{
    if (!cq) return EINVAL;
    struct melondma_comp_channel *state = cq->channel ? cq->channel->priv : NULL;
    if (state) pthread_mutex_lock(&state->lock);
    int rc = rdma_destroy_cq((rdma_cq *)cq->priv);
    if (rc) {
        if (state) pthread_mutex_unlock(&state->lock);
        return rc == -EBUSY ? EBUSY : EIO;
    }
    if (cq->channel) {
        struct ibv_cq **cursor = &state->cqs;
        while (*cursor && *cursor != cq) cursor = &(*cursor)->channel_next;
        if (*cursor == cq) *cursor = cq->channel_next;
        cq->channel_next = NULL;
        pthread_cond_signal(&state->wake);
        pthread_mutex_unlock(&state->lock);
    }
    if (!rc) {
        struct ibv_cq **list = &cq->context->cq_list;
        while (*list && *list != cq) list = &(*list)->context_next;
        if (*list == cq) *list = cq->context_next;
        pthread_mutex_destroy(&cq->poll_lock);
        pthread_mutex_destroy(&cq->notify_lock);
        __atomic_fetch_sub(&cq->context->live_objects, 1, __ATOMIC_RELEASE);
        free(cq);
    }
    return rc ? EIO : 0;
}

int ibv_req_notify_cq(struct ibv_cq *cq, int solicited_only)
{
    if (!cq || !cq->channel) return EINVAL;
    context_stat_add(&cq->context->cq_arm_requests, 1);
    pthread_mutex_lock(&cq->poll_lock);
    int cached = cq->wc_cache_count != 0;
    pthread_mutex_unlock(&cq->poll_lock);
    /* Interrupt-capable DEXTs arm the hardware CQ. Older builds retain the
     * mapped-ring worker fallback, optionally forced for bring-up. */
    struct melondma_comp_channel *state = cq->channel->priv;
    const char *hardware_arm = getenv("MELONDMA_HW_CQ_EVENT");
    int rc;
    /* Arm BEFORE sampling the ring. The hardware raises a completion event
     * only for a CQE that arrives after the arm doorbell, so a CQE that lands
     * while this function is running must be caught by the sample instead.
     * Sampling first left that CQE with neither an event nor a count above
     * event_count, and the worker only noticed it when its blocking wait
     * timed out. */
    if (state->event_dev || (hardware_arm && strcmp(hardware_arm, "0") != 0)) {
        rc = rdma_arm_cq((rdma_cq *)cq->priv, solicited_only);
        if (rc != 0 && rc != -ENOENT) return EIO;
    }
    uint64_t completions = 0;
    /* Arm against the SAME monotonic count the worker checks (total produced
     * from the mapped CQE ring), so a newly-arrived completion always exceeds
     * it.  Fall back to the kernel query only when the ring isn't mapped. */
    rc = rdma_cq_pending_local(cq->priv, &completions);
    if (rc != 0 && rdma_query_cq_completions(cq->priv, &completions) != 0)
        return EIO;
    pthread_mutex_lock(&cq->notify_lock);
    cq->event_count = completions;
    cq->notify_armed = cached ? 0 : 1;
    cq->notify_generation++;
    pthread_mutex_unlock(&cq->notify_lock);
    pthread_mutex_lock(&state->lock);
    if (cached) {
        context_stat_add(&cq->context->cq_arm_cached, 1);
        if (write(state->write_fd, &cq, sizeof(cq)) != (ssize_t)sizeof(cq)) {
            pthread_mutex_lock(&cq->notify_lock);
            cq->lost_events++;
            context_stat_add(&cq->context->completion_lost_events, 1);
            pthread_mutex_unlock(&cq->notify_lock);
        }
    } else {
        pthread_cond_signal(&state->wake);
    }
    pthread_mutex_unlock(&state->lock);
    return 0;
}

int ibv_get_cq_event(struct ibv_comp_channel *channel, struct ibv_cq **cq,
                     void **cq_context)
{
    if (!channel || !cq || !cq_context) return EINVAL;
    ssize_t n = read(channel->fd, cq, sizeof(*cq));
    if (n != (ssize_t)sizeof(*cq)) return errno ? errno : EIO;
    context_stat_add(&channel->context->completion_events, 1);
    *cq_context = (*cq)->cq_context;
    return 0;
}

void ibv_ack_cq_events(struct ibv_cq *cq, unsigned int nevents)
{
    if (!cq) return;
    pthread_mutex_lock(&cq->notify_lock);
    cq->acked_events += nevents;
    pthread_mutex_unlock(&cq->notify_lock);
}

static uint32_t round_queue_depth(uint32_t depth)
{
    if (depth < 64) depth = 64;
    uint32_t rounded = 1;
    while (rounded < depth && rounded < 2048) rounded <<= 1;
    return rounded;
}

struct ibv_qp *ibv_create_qp(struct ibv_pd *pd,
                             struct ibv_qp_init_attr *init)
{
    if (!pd || !init || !init->send_cq || !init->recv_cq ||
        init->qp_type != IBV_QPT_RC || init->cap.max_send_sge > RDMA_MAX_SGE ||
        init->cap.max_recv_sge > RDMA_MAX_RECV_SGE) {
        errno = EINVAL;
        return NULL;
    }
    struct rdma_qp_init_attr native = {
        .send_cq = (rdma_cq *)init->send_cq->priv,
        .recv_cq = (rdma_cq *)init->recv_cq->priv,
        .qp_type = RDMA_QPT_RC,
        .cap_sq = round_queue_depth(init->cap.max_send_wr),
        .cap_rq = round_queue_depth(init->cap.max_recv_wr),
        .max_inline_data = init->cap.max_inline_data,
    };
    rdma_qp *rdma = rdma_create_qp((rdma_pd *)pd->priv, &native);
    if (!rdma) { errno = EIO; return NULL; }
    struct ibv_qp *qp = calloc(1, sizeof(*qp));
    struct melondma_qp_priv *priv = calloc(1, sizeof(*priv));
    if (!qp || !priv) {
        free(qp); free(priv); (void)rdma_destroy_qp(rdma); errno = ENOMEM;
        return NULL;
    }
    priv->qp = rdma;
    priv->sq_sig_all = init->sq_sig_all;
    qp->context = pd->context;
    qp->context_next = pd->context->qp_list;
    pd->context->qp_list = qp;
    qp->qp_context = init->qp_context;
    qp->pd = pd;
    qp->send_cq = init->send_cq;
    qp->recv_cq = init->recv_cq;
    qp->qp_num = rdma_qp_number(rdma);
    qp->state = IBV_QPS_RESET;
    qp->qp_type = IBV_QPT_RC;
    qp->priv = priv;
    init->cap.max_send_wr = native.cap_sq;
    init->cap.max_recv_wr = native.cap_rq;
    struct rdma_abi_attr abi = {};
    int multi_sge = rdma_query_abi(pd->context->dev, &abi) == 0 &&
                    (abi.features & RDMA_FEATURE_MULTI_SGE);
    init->cap.max_send_sge = multi_sge ? RDMA_MAX_SGE : 1;
    init->cap.max_recv_sge = multi_sge ? RDMA_MAX_RECV_SGE : 1;
    init->cap.max_inline_data = native.max_inline_data;
    return qp;
}

int ibv_modify_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr, int attr_mask)
{
    if (!qp || !attr || !(attr_mask & IBV_QP_STATE)) return EINVAL;
    if ((attr_mask & IBV_QP_CUR_STATE) && attr->cur_qp_state != qp->state)
        return EINVAL;
    struct melondma_qp_priv *priv = qp->priv;
    struct rdma_qp_attr native = {
        .cur_state = qp->state,
        .new_state = attr->qp_state,
        .attr_mask = (uint32_t)attr_mask,
        .dest_qpn = attr->dest_qp_num,
        .path_mtu = attr->path_mtu,
        .rq_psn = attr->rq_psn,
        .sq_psn = attr->sq_psn,
        .pkey_index = attr->pkey_index,
        .port_num = attr->port_num ? attr->port_num : 1,
        .ah_sgid_index = qp->context->gid_programmed ? qp->context->gid_index :
                          attr->ah_attr.grh.sgid_index,
        .ah_hop_limit = attr->ah_attr.grh.hop_limit ? attr->ah_attr.grh.hop_limit :
                        qp->context->hop_limit,
        .ah_traffic_class = attr->ah_attr.grh.traffic_class ?
                            attr->ah_attr.grh.traffic_class : qp->context->traffic_class,
        .ah_udp_sport = qp->context->udp_sport,
        .min_rnr_timer = attr->min_rnr_timer,
        .max_dest_rd_atomic = attr->max_dest_rd_atomic,
        .max_rd_atomic = attr->max_rd_atomic,
        .timeout = attr->timeout,
        .retry_cnt = attr->retry_cnt,
        .rnr_retry = attr->rnr_retry,
        .sl = attr->ah_attr.sl,
    };
    if (attr->qp_state == IBV_QPS_RTR) {
        if (!qp->context->remote_mac_valid) return EDESTADDRREQ;
        memcpy(native.ah_dmac, qp->context->remote_mac, 6);
        memcpy(native.ah_dgid, attr->ah_attr.grh.dgid.raw, 16);
    }
    int modify_rc = rdma_modify_qp(priv->qp, &native);
    if (modify_rc != 0) {
        fprintf(stderr, "libibverbs_compat: modify_qp %u->%u failed rc=%d sgid=%u dmac=%02x:%02x:%02x:%02x:%02x:%02x\n",
                qp->state, attr->qp_state, modify_rc, native.ah_sgid_index,
                native.ah_dmac[0], native.ah_dmac[1], native.ah_dmac[2],
                native.ah_dmac[3], native.ah_dmac[4], native.ah_dmac[5]);
        return EIO;
    }
    qp->state = attr->qp_state;
    return 0;
}

int ibv_query_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr,
                 int attr_mask, struct ibv_qp_init_attr *init_attr)
{
    if (!qp || !attr || attr_mask) return EINVAL;
    struct melondma_qp_priv *priv = qp->priv;
    uint32_t state = 0;
    if (rdma_query_qp(priv->qp, &state) != 0) return EIO;
    memset(attr, 0, sizeof(*attr));
    attr->qp_state = (enum ibv_qp_state)state;
    if (init_attr) {
        memset(init_attr, 0, sizeof(*init_attr));
        init_attr->qp_context = qp->qp_context;
        init_attr->send_cq = qp->send_cq;
        init_attr->recv_cq = qp->recv_cq;
        init_attr->qp_type = qp->qp_type;
        struct rdma_abi_attr abi = {};
        int multi_sge = rdma_query_abi(qp->context->dev, &abi) == 0 &&
                        (abi.features & RDMA_FEATURE_MULTI_SGE);
        init_attr->cap.max_send_sge = multi_sge ? RDMA_MAX_SGE : 1;
        init_attr->cap.max_recv_sge = multi_sge ? RDMA_MAX_RECV_SGE : 1;
    }
    return 0;
}

int ibv_destroy_qp(struct ibv_qp *qp)
{
    if (!qp) return EINVAL;
    struct melondma_qp_priv *priv = qp->priv;
    int rc = rdma_destroy_qp(priv->qp);
    if (!rc) {
        struct ibv_qp **list = &qp->context->qp_list;
        while (*list && *list != qp) list = &(*list)->context_next;
        if (*list == qp) *list = qp->context_next;
        free(priv);
        free(qp);
    }
    return rc == -EBUSY ? EBUSY : (rc ? EIO : 0);
}

struct ibv_mr *ibv_reg_mr(struct ibv_pd *pd, void *addr, size_t length,
                          int access)
{
    if (!pd || !addr || !length) { errno = EINVAL; return NULL; }
    /* No alignment requirement: the DEXT pins client pages and the PAS walk
     * handles partial first/last pages (MlxDMA::Pin → mlxAppendMttPages). */

    rdma_mr *native = NULL;
    rdma_mr **children = NULL;
    uint32_t nchildren = 0;
    struct rdma_mr_attr_resp info = {0};

    {
        /* Try a single MR first: the DEXT coalesces a contiguous IOVA onto
         * coarse MTT pages (2 MiB), so 149 MiB registers with one
         * IODMACommand + one CREATE_MKEY instead of ~80 chunked ones.
         * Only when the IOVA is not contiguous do we fall back to the
         * chunked + indirect composition. */
        native = rdma_reg_mr((rdma_pd *)pd->priv, addr, length,
                             (uint32_t)access, &info);
        if (!native && errno != E2BIG) return NULL;
        if (!native) {
            children = calloc(RDMA_MAX_INDIRECT_MR_CHILDREN, sizeof(*children));
            if (!children) { errno = ENOMEM; return NULL; }
            const uint8_t *base = (const uint8_t *)addr;
            size_t off = 0;
            const size_t page = (size_t)getpagesize();
            size_t max_chunk = (RDMA_MAX_DIRECT_MR_BYTES - page) & ~(page - 1);
            while (off < length) {
                size_t chunk = length - off;
                if (chunk > max_chunk) chunk = max_chunk;
                rdma_mr *child = NULL;
                if (nchildren == RDMA_MAX_INDIRECT_MR_CHILDREN) errno = E2BIG;
                else for (;;) {
                    child = rdma_reg_mr((rdma_pd *)pd->priv, (void *)(base + off),
                                         chunk, (uint32_t)access, NULL);
                    if (child || errno != E2BIG || chunk <= page) break;
                    chunk = (chunk / 2) & ~(page - 1);
                    if (!chunk) chunk = page;
                    max_chunk = chunk;
                }
                if (!child) {
                    int saved = errno;
                    for (uint32_t j = 0; j < nchildren; j++) (void)rdma_dereg_mr(children[j]);
                    free(children);
                    errno = saved;
                    return NULL;
                }
                children[nchildren++] = child;
                off += chunk;
            }
            native = rdma_reg_mr_indirect((rdma_pd *)pd->priv, children, nchildren,
                                          (uint64_t)addr, length, (uint32_t)access, &info);
            if (!native) {
                for (uint32_t j = 0; j < nchildren; j++) (void)rdma_dereg_mr(children[j]);
                free(children);
                errno = EIO;
                return NULL;
            }
        }
    }

    struct ibv_mr *mr = calloc(1, sizeof(*mr));
    struct melondma_mr_priv *priv = calloc(1, sizeof(*priv));
    if (!mr || !priv) {
        (void)rdma_dereg_mr(native);
        for (uint32_t j = 0; j < nchildren; j++)
            if (children) (void)rdma_dereg_mr(children[j]);
        free(children); free(mr); free(priv);
        errno = ENOMEM;
        return NULL;
    }
    priv->mr = native;
    priv->children = children;
    priv->nchildren = nchildren;
    mr->context = pd->context;
    mr->pd = pd;
    mr->addr = addr;
    mr->length = length;
    mr->handle = info.mr_handle;
    mr->lkey = info.lkey;
    mr->rkey = info.rkey;
    mr->priv = priv;
    return mr;
}

struct ibv_mw *ibv_alloc_mw(struct ibv_pd *pd, enum ibv_mw_type type)
{
    if (!pd || type != IBV_MW_TYPE_2) { errno = EINVAL; return NULL; }
    rdma_mw *native = rdma_alloc_mw((rdma_pd *)pd->priv, type);
    if (!native) { errno = EIO; return NULL; }
    struct ibv_mw *mw = calloc(1, sizeof(*mw));
    if (!mw) { (void)rdma_dealloc_mw(native); errno = ENOMEM; return NULL; }
    mw->context = pd->context; mw->pd = pd; mw->type = type;
    mw->rkey = rdma_mw_rkey(native); mw->priv = native;
    return mw;
}

int ibv_dealloc_mw(struct ibv_mw *mw)
{
    if (!mw) return EINVAL;
    int rc = rdma_dealloc_mw((rdma_mw *)mw->priv);
    if (!rc) free(mw);
    return rc ? EIO : 0;
}

int ibv_bind_mw(struct ibv_qp *qp, struct ibv_mw *mw,
                struct ibv_mw_bind *bind)
{
    if (!qp || !mw || !bind || !bind->bind_info.mr ||
        mw->pd != bind->bind_info.mr->pd || mw->type != IBV_MW_TYPE_2 ||
        !bind->bind_info.length) { errno = EINVAL; return EINVAL; }
    uint32_t rkey = 0;
    int rc = rdma_bind_mw((rdma_qp *)((struct melondma_qp_priv *)qp->priv)->qp,
        (rdma_mw *)mw->priv, ((struct melondma_mr_priv *)bind->bind_info.mr->priv)->mr,
        bind->bind_info.addr, bind->bind_info.length,
        (uint32_t)bind->bind_info.mw_access_flags,
        (uint32_t)bind->send_flags, bind->wr_id, &rkey);
    if (!rc) mw->rkey = rkey;
    return rc;
}

int ibv_dereg_mr(struct ibv_mr *mr)
{
    if (!mr) return EINVAL;
    struct melondma_mr_priv *priv = (struct melondma_mr_priv *)mr->priv;
    int rc = EINVAL;
    if (priv) {
        rc = priv->mr ? rdma_dereg_mr(priv->mr) : 0;
        if (rc) return rc == -EBUSY ? EBUSY : EIO;
        priv->mr = NULL;
        for (uint32_t i = 0; i < priv->nchildren; i++)
            if (priv->children && priv->children[i]) {
                rc = rdma_dereg_mr(priv->children[i]);
                if (rc) return rc == -EBUSY ? EBUSY : EIO;
                priv->children[i] = NULL;
            }
        free(priv->children);
        free(priv);
    }
    free(mr);
    return rc ? EIO : 0;
}

static int native_send_opcode(enum ibv_wr_opcode opcode, uint32_t *native)
{
    switch (opcode) {
    case IBV_WR_SEND: *native = RDMA_WR_SEND; return 0;
    case IBV_WR_SEND_WITH_IMM: *native = RDMA_WR_SEND_IMM; return 0;
    case IBV_WR_RDMA_WRITE: *native = RDMA_WR_RDMA_WRITE; return 0;
    case IBV_WR_RDMA_WRITE_WITH_IMM: *native = RDMA_WR_RDMA_WRITE_IMM; return 0;
    case IBV_WR_RDMA_READ: *native = RDMA_WR_RDMA_READ; return 0;
    case IBV_WR_LOCAL_INV: *native = RDMA_WR_LOCAL_INV; return 0;
    case IBV_WR_ATOMIC_CMP_AND_SWP: *native = RDMA_WR_ATOMIC_CS; return 0;
    case IBV_WR_ATOMIC_FETCH_AND_ADD: *native = RDMA_WR_ATOMIC_FA; return 0;
    default: return -1;
    }
}

int ibv_post_send(struct ibv_qp *qp, struct ibv_send_wr *wr,
                  struct ibv_send_wr **bad_wr)
{
    if (bad_wr) *bad_wr = NULL;
    if (!qp || !wr) { if (bad_wr) *bad_wr = wr; return EINVAL; }
    struct melondma_qp_priv *priv = qp->priv;
    while (wr) {
        if (wr->opcode == IBV_WR_LOCAL_INV) {
            if (rdma_post_local_inv(priv->qp, wr->wr_id,
                                    wr->wr.local_inv.invalidate_rkey) != 0) {
                if (bad_wr) *bad_wr = wr;
                return EIO;
            }
            wr = wr->next;
            continue;
        }
        /* A trusted mapped QP can encode heterogeneous WR chains in userspace
         * and publish the whole run with one MMIO doorbell.  Keep operations
         * that mutate DEXT-owned state (LOCAL_INV) and atomics on their
         * dedicated paths below. */
        if (rdma_qp_trusted_fast_path(priv->qp) &&
            wr->opcode != IBV_WR_LOCAL_INV &&
            wr->opcode != IBV_WR_ATOMIC_CMP_AND_SWP &&
            wr->opcode != IBV_WR_ATOMIC_FETCH_AND_ADD) {
            struct rdma_send_wr batch[RDMA_POST_CHUNK];
            struct rdma_sge sges[RDMA_POST_CHUNK][RDMA_MAX_SGE];
            struct ibv_send_wr *cursor = wr;
            uint32_t count = 0;
            while (cursor && count < RDMA_POST_CHUNK &&
                   cursor->opcode != IBV_WR_LOCAL_INV &&
                   cursor->opcode != IBV_WR_ATOMIC_CMP_AND_SWP &&
                   cursor->opcode != IBV_WR_ATOMIC_FETCH_AND_ADD) {
                uint32_t opcode = 0;
                int inline_wr = (cursor->send_flags & IBV_SEND_INLINE) != 0;
                int zero_write_imm =
                    cursor->opcode == IBV_WR_RDMA_WRITE_WITH_IMM &&
                    cursor->num_sge == 0;
                if (native_send_opcode(cursor->opcode, &opcode) ||
                    cursor->num_sge < 0 ||
                    cursor->num_sge > (int)RDMA_MAX_SGE ||
                    (cursor->send_flags & ~(IBV_SEND_SIGNALED | IBV_SEND_FENCE |
                                             IBV_SEND_SOLICITED | IBV_SEND_INLINE)) ||
                    (inline_wr &&
                     (cursor->num_sge != 1 || !cursor->sg_list ||
                      (cursor->opcode != IBV_WR_SEND &&
                       cursor->opcode != IBV_WR_SEND_WITH_IMM) ||
                      !cursor->sg_list[0].addr || !cursor->sg_list[0].length ||
                      cursor->sg_list[0].length > RDMA_MAX_INLINE_DATA)) ||
                    (!inline_wr && !zero_write_imm &&
                     (cursor->num_sge == 0 || !cursor->sg_list))) {
                    if (bad_wr) *bad_wr = cursor;
                    return EINVAL;
                }
                for (int i = 0; i < cursor->num_sge; i++) {
                    if (!cursor->sg_list[i].length) {
                        if (bad_wr) *bad_wr = cursor;
                        return EINVAL;
                    }
                    sges[count][i].addr = cursor->sg_list[i].addr;
                    sges[count][i].length = cursor->sg_list[i].length;
                    sges[count][i].lkey = cursor->sg_list[i].lkey;
                }
                batch[count].wr_id = cursor->wr_id;
                batch[count].opcode = opcode;
                batch[count].num_sge = (uint32_t)cursor->num_sge;
                batch[count].sg_list = cursor->num_sge ? sges[count] : NULL;
                batch[count].remote_addr = cursor->wr.rdma.remote_addr;
                batch[count].rkey = cursor->wr.rdma.rkey;
                batch[count].imm_data = cursor->imm_data;
                batch[count].send_flags =
                    (((cursor->send_flags & IBV_SEND_SIGNALED) || priv->sq_sig_all) ?
                     RDMA_SEND_SIGNALED : 0) |
                    ((cursor->send_flags & IBV_SEND_FENCE) ? RDMA_SEND_FENCE : 0) |
                    ((cursor->send_flags & IBV_SEND_SOLICITED) ? RDMA_SEND_SOLICITED : 0) |
                    (inline_wr ? RDMA_SEND_INLINE : 0);
                count++;
                cursor = cursor->next;
            }
            int rc = rdma_post_send_batch(priv->qp, batch, count);
            if (rc) {
                if (bad_wr) *bad_wr = wr;
                return rc == -EAGAIN ? EAGAIN : (rc == -EINVAL ? EINVAL : EIO);
            }
            wr = cursor;
            continue;
        }
        if (wr->send_flags & IBV_SEND_INLINE) {
            if (wr->num_sge != 1 || !wr->sg_list ||
                (wr->opcode != IBV_WR_SEND && wr->opcode != IBV_WR_SEND_WITH_IMM) ||
                wr->sg_list[0].length > RDMA_MAX_INLINE_DATA) {
                if (bad_wr) *bad_wr = wr;
                return EINVAL;
            }
            uint32_t opcode = wr->opcode == IBV_WR_SEND_WITH_IMM ?
                              RDMA_WR_SEND_IMM : RDMA_WR_SEND;
            uint32_t sflags =
                ((wr->send_flags & IBV_SEND_SIGNALED) || priv->sq_sig_all ?
                 RDMA_SEND_SIGNALED : 0) |
                ((wr->send_flags & IBV_SEND_FENCE) ? RDMA_SEND_FENCE : 0) |
                ((wr->send_flags & IBV_SEND_SOLICITED) ? RDMA_SEND_SOLICITED : 0) |
                RDMA_SEND_INLINE;
            int rc = rdma_post_send_inline(priv->qp, wr->wr_id, opcode,
                (const void *)(uintptr_t)wr->sg_list[0].addr,
                wr->sg_list[0].length, wr->imm_data, sflags);
            if (rc) { if (bad_wr) *bad_wr = wr; return rc == -EAGAIN ? EAGAIN : EIO; }
            wr = wr->next;
            continue;
        }
        if (wr->opcode == IBV_WR_ATOMIC_CMP_AND_SWP ||
            wr->opcode == IBV_WR_ATOMIC_FETCH_AND_ADD) {
            uint32_t opcode = wr->opcode == IBV_WR_ATOMIC_CMP_AND_SWP ?
                              RDMA_WR_ATOMIC_CS : RDMA_WR_ATOMIC_FA;
            uint32_t sflags =
                ((wr->send_flags & IBV_SEND_SIGNALED) || priv->sq_sig_all ?
                 RDMA_SEND_SIGNALED : 0);
            /* mlx5 atomic WQE: swap_add = new value (CS) / addend (FA),
             * compare = expected (CS) / ignored (FA). verbs puts the CS
             * compare in wr.atomic.compare_add and the CS new value in
             * wr.atomic.swap; for FA the addend is in compare_add. */
            uint64_t swap_add = wr->opcode == IBV_WR_ATOMIC_CMP_AND_SWP ?
                                wr->wr.atomic.swap : wr->wr.atomic.compare_add;
            uint64_t compare = wr->opcode == IBV_WR_ATOMIC_CMP_AND_SWP ?
                               wr->wr.atomic.compare_add : 0;
            if (wr->num_sge != 1 || !wr->sg_list ||
                wr->sg_list[0].length != 8 || (wr->sg_list[0].addr & 7) ||
                !wr->sg_list[0].lkey) {
                if (bad_wr) *bad_wr = wr;
                return EINVAL;
            }
            int rc = rdma_post_send_atomic(priv->qp, wr->wr_id, opcode,
                wr->wr.atomic.remote_addr, wr->wr.atomic.rkey,
                compare, swap_add, wr->sg_list[0].addr,
                wr->sg_list[0].lkey, sflags);
            if (rc) { if (bad_wr) *bad_wr = wr; return rc == -EAGAIN ? EAGAIN : EIO; }
            wr = wr->next;
            continue;
        }
        if (wr->num_sge > 1 || wr->opcode == IBV_WR_SEND_WITH_IMM ||
            wr->opcode == IBV_WR_RDMA_WRITE_WITH_IMM) {
            int zero_write_imm = wr->opcode == IBV_WR_RDMA_WRITE_WITH_IMM &&
                                 wr->num_sge == 0;
            if (wr->num_sge > (int)RDMA_MAX_SGE ||
                (!zero_write_imm && !wr->sg_list) ||
                (wr->send_flags & ~(IBV_SEND_SIGNALED | IBV_SEND_FENCE |
                                    IBV_SEND_SOLICITED))) {
                if (bad_wr) *bad_wr = wr;
                return EINVAL;
            }
            struct rdma_sge sges[RDMA_MAX_SGE] = {};
            for (int i = 0; i < wr->num_sge; i++) {
                sges[i].addr = wr->sg_list[i].addr;
                sges[i].length = wr->sg_list[i].length;
                sges[i].lkey = wr->sg_list[i].lkey;
            }
            struct rdma_send_wr native = {
                .wr_id = wr->wr_id, .num_sge = (uint32_t)wr->num_sge,
                .sg_list = wr->num_sge ? sges : NULL,
                .remote_addr = wr->wr.rdma.remote_addr,
                .rkey = wr->wr.rdma.rkey, .imm_data = wr->imm_data,
                .send_flags = ((wr->send_flags & IBV_SEND_SIGNALED) || priv->sq_sig_all ?
                    RDMA_SEND_SIGNALED : 0) |
                    ((wr->send_flags & IBV_SEND_FENCE) ? RDMA_SEND_FENCE : 0) |
                    ((wr->send_flags & IBV_SEND_SOLICITED) ? RDMA_SEND_SOLICITED : 0),
            };
            if (native_send_opcode(wr->opcode, &native.opcode) ||
                rdma_post_send_sge(priv->qp, &native) != 0) {
                if (bad_wr) *bad_wr = wr;
                return EIO;
            }
            wr = wr->next;
            continue;
        }
        struct rdma_send_wr batch[RDMA_POST_CHUNK];
        struct rdma_sge sge[RDMA_POST_CHUNK];
        struct ibv_send_wr *cursor = wr;
        uint32_t count = 0;
        while (cursor && count < RDMA_POST_CHUNK) {
            uint32_t opcode = 0;
            if (cursor->num_sge != 1 || !cursor->sg_list ||
                (cursor->send_flags & ~(IBV_SEND_SIGNALED | IBV_SEND_FENCE |
                                         IBV_SEND_SOLICITED | IBV_SEND_INLINE)) ||
                (cursor->send_flags & IBV_SEND_INLINE) ||
                native_send_opcode(cursor->opcode, &opcode)) {
                if (bad_wr) *bad_wr = cursor;
                return EINVAL;
            }
            sge[count].addr = cursor->sg_list[0].addr;
            sge[count].length = cursor->sg_list[0].length;
            sge[count].lkey = cursor->sg_list[0].lkey;
            batch[count].wr_id = cursor->wr_id;
            batch[count].opcode = opcode;
            batch[count].num_sge = 1;
            batch[count].sg_list = &sge[count];
            batch[count].remote_addr = cursor->wr.rdma.remote_addr;
            batch[count].rkey = cursor->wr.rdma.rkey;
            batch[count].send_flags =
                ((cursor->send_flags & IBV_SEND_SIGNALED) || priv->sq_sig_all ?
                RDMA_SEND_SIGNALED : 0) |
                ((cursor->send_flags & IBV_SEND_FENCE) ? RDMA_SEND_FENCE : 0) |
                ((cursor->send_flags & IBV_SEND_SOLICITED) ? RDMA_SEND_SOLICITED : 0);
            count++;
            cursor = cursor->next;
        }
        if (rdma_post_send_batch(priv->qp, batch, count) != 0) {
            if (bad_wr) *bad_wr = wr;
            return EIO;
        }
        wr = cursor;
    }
    comp_channel_kick(qp->send_cq);
    return 0;
}

int ibv_post_recv(struct ibv_qp *qp, struct ibv_recv_wr *wr,
                  struct ibv_recv_wr **bad_wr)
{
    if (bad_wr) *bad_wr = NULL;
    if (!qp || !wr) { if (bad_wr) *bad_wr = wr; return EINVAL; }
    struct melondma_qp_priv *priv = qp->priv;
    while (wr) {
        if (rdma_qp_trusted_fast_path(priv->qp)) {
            struct rdma_recv_wr batch[RDMA_POST_CHUNK];
            struct rdma_sge sges[RDMA_POST_CHUNK][RDMA_MAX_RECV_SGE];
            struct ibv_recv_wr *cursor = wr;
            uint32_t count = 0;
            while (cursor && count < RDMA_POST_CHUNK) {
                if (cursor->num_sge < 1 ||
                    cursor->num_sge > (int)RDMA_MAX_RECV_SGE ||
                    !cursor->sg_list) {
                    if (bad_wr) *bad_wr = cursor;
                    return EINVAL;
                }
                for (int i = 0; i < cursor->num_sge; i++) {
                    if (!cursor->sg_list[i].length) {
                        if (bad_wr) *bad_wr = cursor;
                        return EINVAL;
                    }
                    sges[count][i].addr = cursor->sg_list[i].addr;
                    sges[count][i].length = cursor->sg_list[i].length;
                    sges[count][i].lkey = cursor->sg_list[i].lkey;
                }
                batch[count].wr_id = cursor->wr_id;
                batch[count].num_sge = (uint32_t)cursor->num_sge;
                batch[count].sg_list = sges[count];
                count++;
                cursor = cursor->next;
            }
            int rc = rdma_post_recv_batch(priv->qp, batch, count);
            if (rc) {
                if (bad_wr) *bad_wr = wr;
                return rc == -EAGAIN ? EAGAIN : (rc == -EINVAL ? EINVAL : EIO);
            }
            wr = cursor;
            continue;
        }
        if (wr->num_sge > 1) {
            if (wr->num_sge > (int)RDMA_MAX_RECV_SGE || !wr->sg_list) {
                if (bad_wr) *bad_wr = wr;
                return EINVAL;
            }
            struct rdma_sge sges[RDMA_MAX_SGE] = {};
            for (int i = 0; i < wr->num_sge; i++) {
                sges[i].addr = wr->sg_list[i].addr;
                sges[i].length = wr->sg_list[i].length;
                sges[i].lkey = wr->sg_list[i].lkey;
            }
            struct rdma_recv_wr native = {
                .wr_id = wr->wr_id, .num_sge = (uint32_t)wr->num_sge,
                .sg_list = sges,
            };
            if (rdma_post_recv_sge(priv->qp, &native) != 0) {
                if (bad_wr) *bad_wr = wr;
                return EIO;
            }
            wr = wr->next;
            continue;
        }
        struct rdma_recv_wr batch[RDMA_POST_CHUNK];
        struct rdma_sge sge[RDMA_POST_CHUNK];
        struct ibv_recv_wr *cursor = wr;
        uint32_t count = 0;
        while (cursor && count < RDMA_POST_CHUNK) {
            if (cursor->num_sge != 1 || !cursor->sg_list) {
                if (bad_wr) *bad_wr = cursor;
                return EINVAL;
            }
            sge[count].addr = cursor->sg_list[0].addr;
            sge[count].length = cursor->sg_list[0].length;
            sge[count].lkey = cursor->sg_list[0].lkey;
            batch[count].wr_id = cursor->wr_id;
            batch[count].num_sge = 1;
            batch[count].sg_list = &sge[count];
            count++;
            cursor = cursor->next;
        }
        if (rdma_post_recv_batch(priv->qp, batch, count) != 0) {
            if (bad_wr) *bad_wr = wr;
            return EIO;
        }
        wr = cursor;
    }
    return 0;
}

static enum ibv_wc_status wc_status(uint32_t status)
{
    switch (status) {
    case RDMA_WC_SUCCESS: return IBV_WC_SUCCESS;
    case RDMA_WC_LOC_LEN: return IBV_WC_LOC_LEN_ERR;
    case RDMA_WC_LOC_QP_OP: return IBV_WC_LOC_QP_OP_ERR;
    case RDMA_WC_WR_FLUSH: return IBV_WC_WR_FLUSH_ERR;
    case RDMA_WC_REM_ACCESS: return IBV_WC_REM_ACCESS_ERR;
    case RDMA_WC_RETRY_EXC: return IBV_WC_RETRY_EXC_ERR;
    case RDMA_WC_RNR_RETRY: return IBV_WC_RNR_RETRY_EXC_ERR;
    default: return IBV_WC_GENERAL_ERR;
    }
}

static enum ibv_wc_opcode wc_opcode(uint32_t opcode)
{
    switch (opcode) {
    case RDMA_WC_SEND: return IBV_WC_SEND;
    case RDMA_WC_RDMA_WRITE: return IBV_WC_RDMA_WRITE;
    case RDMA_WC_RDMA_READ: return IBV_WC_RDMA_READ;
    case RDMA_WC_FETCH_ADD: return IBV_WC_FETCH_ADD;
    case RDMA_WC_COMP_SWAP: return IBV_WC_COMP_SWAP;
    default: return IBV_WC_RECV;
    }
}

static void copy_native_wc(struct ibv_wc *out, const struct rdma_wc *native)
{
    memset(out, 0, sizeof(*out));
    out->wr_id = native->wr_id;
    out->status = wc_status(native->status);
    out->opcode = wc_opcode(native->opcode);
    out->byte_len = native->byte_len;
    out->imm_data = native->imm_data;
    out->wc_flags =
        (native->wc_flags & RDMA_WC_WITH_IMM ? IBV_WC_WITH_IMM : 0) |
        (native->wc_flags & RDMA_WC_WITH_ATOMIC ? IBV_WC_WITH_ATOMIC : 0);
    out->qp_num = native->qp_num;
    out->atomic_result = native->atomic_result;
    out->vendor_err = native->status == RDMA_WC_SUCCESS ? 0 :
                      native->vendor_err != 0xffffffffu ?
                          native->vendor_err : native->status;
}

int ibv_poll_cq(struct ibv_cq *cq, int num_entries, struct ibv_wc *wc)
{
    if (!cq || !wc || num_entries <= 0) return -EINVAL;
    if (cq->single_threaded) {
        pthread_t self = pthread_self();
        if (!cq->poll_owner_valid) {
            cq->poll_owner = self;
            cq->poll_owner_valid = 1;
        } else if (!pthread_equal(cq->poll_owner, self)) {
            context_stat_add(&cq->context->single_owner_violations, 1);
            errno = EPERM;
            return -EPERM;
        }
    } else {
        pthread_mutex_lock(&cq->poll_lock);
    }
    int delivered = 0;
    while (delivered < num_entries && cq->wc_cache_count) {
        wc[delivered++] = cq->wc_cache[cq->wc_cache_head];
        context_stat_add(&cq->context->wc_cache_hits, 1);
        cq->wc_cache_head = (cq->wc_cache_head + 1) % RDMA_MAX_POLL_BATCH;
        cq->wc_cache_count--;
    }
    if (delivered == num_entries) {
        if (!cq->single_threaded) pthread_mutex_unlock(&cq->poll_lock);
        return delivered;
    }
    /* The common latency path asks for one WC and owns the CQ. Avoid the
     * maximum batch stack array, conversion loop and provider-local cache. */
    if (cq->single_threaded && num_entries == 1 && !cq->wc_cache_count) {
        struct rdma_wc native;
        int count = rdma_poll_cq((rdma_cq *)cq->priv, &native, 1);
        if (count == 1) copy_native_wc(wc, &native);
        return count;
    }
    struct rdma_wc native[RDMA_MAX_POLL_BATCH];
    /* Prefetch a full bounded drain even for poll(..., 1). This publishes one
     * CQ consumer update for up to 16 CQEs; surplus WCs stay in provider-local
     * order and never cause another hardware/DriverKit access. */
    int count = rdma_poll_cq((rdma_cq *)cq->priv, native,
                             (int)RDMA_MAX_POLL_BATCH);
    if (count < 0) {
        if (!cq->single_threaded) pthread_mutex_unlock(&cq->poll_lock);
        return delivered ? delivered : count;
    }
    for (int i = 0; i < count; i++) {
        struct ibv_wc converted;
        copy_native_wc(&converted, &native[i]);
        if (delivered < num_entries) {
            wc[delivered++] = converted;
        } else if (cq->wc_cache_count < RDMA_MAX_POLL_BATCH) {
            uint32_t tail = (cq->wc_cache_head + cq->wc_cache_count) %
                            RDMA_MAX_POLL_BATCH;
            cq->wc_cache[tail] = converted;
            cq->wc_cache_count++;
            context_stat_add(&cq->context->wc_cache_prefetched, 1);
        }
    }
    if (!cq->single_threaded) pthread_mutex_unlock(&cq->poll_lock);
    return delivered;
}

int ibv_mlx5_set_single_threaded(struct ibv_cq *cq, int enable)
{
    if (!cq) return EINVAL;
    pthread_mutex_lock(&cq->poll_lock);
    if (cq->wc_cache_count) {
        pthread_mutex_unlock(&cq->poll_lock);
        return EBUSY;
    }
    cq->single_threaded = enable != 0;
    cq->poll_owner_valid = 0;
    pthread_mutex_unlock(&cq->poll_lock);
    return 0;
}

const char *ibv_wc_status_str(enum ibv_wc_status status)
{
    switch (status) {
    case IBV_WC_SUCCESS: return "success";
    case IBV_WC_LOC_LEN_ERR: return "local length error";
    case IBV_WC_LOC_QP_OP_ERR: return "local QP operation error";
    case IBV_WC_WR_FLUSH_ERR: return "work request flushed";
    case IBV_WC_REM_ACCESS_ERR: return "remote access error";
    case IBV_WC_RETRY_EXC_ERR: return "transport retry exceeded";
    case IBV_WC_RNR_RETRY_EXC_ERR: return "RNR retry exceeded";
    default: return "general error";
    }
}
