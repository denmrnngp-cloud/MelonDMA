/* QueryPerf smoke: prove the active DEXT exposes per-client P0 counters. */
#include <infiniband/verbs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static void print_delta(const struct ibv_mlx5_perf *a,
                        const struct ibv_mlx5_perf *b)
{
#define D(name) ((unsigned long long)(b->name - a->name))
    printf("perf delta:\n");
    printf("  external_methods=%llu external_method_ns=%llu\n",
           D(external_methods), D(external_method_ns));
    printf("  mr_registers=%llu mr_deregisters=%llu mr_bytes=%llu\n",
           D(mr_registers), D(mr_deregisters), D(mr_bytes));
    printf("  post_send=%llu post_recv=%llu poll_cq=%llu\n",
           D(post_send_calls), D(post_recv_calls), D(poll_cq_calls));
    printf("  sync_fast_path=%llu sync_qp_tails=%llu arm_cq=%llu\n",
           D(sync_fast_path_calls), D(sync_qp_tails_calls), D(arm_cq_calls));
    printf("  doorbells=%llu cqe=%llu cqe_errors=%llu copied_bytes=%llu\n",
           D(doorbells), D(cqe_consumed), D(cqe_errors), D(copied_bytes));
    printf("  cq_events=%llu cq_event_wakeups=%llu\n",
           D(cq_events), D(cq_event_wakeups));
#undef D
}

static void print_telemetry_delta(const struct ibv_mlx5_telemetry *a,
                                  const struct ibv_mlx5_telemetry *b)
{
#define TD(name) ((unsigned long long)(b->name - a->name))
    printf("telemetry v%u delta:\n", b->version);
    printf("  direct_poll=%llu empty=%llu cqe=%llu errors=%llu kernel_poll=%llu kernel_cqe=%llu\n",
           TD(direct_poll_calls), TD(direct_poll_empty), TD(direct_cqes),
           TD(direct_cqe_errors), TD(kernel_poll_calls), TD(kernel_cqes));
    printf("  direct_send_batches=%llu wrs=%llu doorbells=%llu direct_recv_batches=%llu wrs=%llu\n",
           TD(direct_send_batches), TD(direct_send_wrs), TD(direct_doorbells),
           TD(direct_recv_batches), TD(direct_recv_wrs));
    printf("  fallback disabled=%llu unmapped=%llu unknown_qp=%llu metadata=%llu dext_owned=%llu\n",
           TD(fallback_direct_disabled), TD(fallback_cq_unmapped),
           TD(fallback_unknown_qp), TD(fallback_missing_metadata),
           TD(fallback_dext_owned));
    printf("  arms=%llu cached=%llu waits=%llu events=%llu hw_waits=%llu hw_wakeups=%llu poll_ticks=%llu poll_wakeups=%llu lost=%llu\n",
           TD(cq_arm_requests), TD(cq_arm_cached), TD(completion_waits),
           TD(completion_events), TD(completion_hw_waits),
           TD(completion_hw_wakeups), TD(completion_poll_ticks),
           TD(completion_poll_wakeups), TD(completion_lost_events));
    printf("  kernel_doorbells=%llu kernel_cqe_errors=%llu driver_cq_events=%llu driver_cq_wakeups=%llu\n",
           TD(kernel_doorbells), TD(kernel_cqe_errors),
           TD(driver_cq_events), TD(driver_cq_event_wakeups));
#undef TD
}

int main(int argc, char **argv)
{
    size_t bytes = argc > 1 ? (size_t)strtoull(argv[1], NULL, 0)
                            : (149u << 20);
    int count = 0, rc = 1;
    struct ibv_device **devices = ibv_get_device_list(&count);
    if (!devices || count < 1) { fprintf(stderr, "no devices\n"); goto out_devices; }
    struct ibv_context *ctx = ibv_open_device(devices[0]);
    if (!ctx) { fprintf(stderr, "ibv_open_device failed: %s\n", strerror(errno)); goto out_devices; }
    struct ibv_mlx5_perf before = {}, after = {};
    struct ibv_mlx5_telemetry telemetry_before = {}, telemetry_after = {};
    if (ibv_mlx5_query_perf(ctx, &before)) {
        fprintf(stderr, "QueryPerf before failed: %s\n", strerror(errno)); goto out_ctx;
    }
    if (ibv_mlx5_query_telemetry(ctx, &telemetry_before)) {
        fprintf(stderr, "QueryTelemetry before failed: %s\n", strerror(errno)); goto out_ctx;
    }
    struct ibv_mlx5_interrupts irq = {};
    int irq_rc = ibv_mlx5_query_interrupts(ctx, &irq);
    if (irq_rc == 0)
        printf("interrupts: vectors=%u completion_ready=%u async_eqn=%u "
               "completion_eqn=%u events=%llu stage=%s status=0x%08x\n",
               irq.vectors, irq.completion_ready, irq.async_eqn,
               irq.completion_eqn, (unsigned long long)irq.completion_events,
               ibv_mlx5_irq_stage_name(irq.setup_stage), irq.setup_status);
    if (irq_rc == 0)
        printf("completion delivery: %s\n",
               !irq.completion_ready ? "unavailable (clients poll the mapped ring)" :
               irq.completion_eqn ? "dedicated completion EQ on vector 1" :
                                    "shared async EQ on vector 0");
    if (irq_rc == 0)
        printf("completion EQ: stage=%s status=0x%08x syndrome=0x%08x fw_status=0x%02x\n",
               ibv_mlx5_cqeq_stage_name(irq.completion_eq_stage),
               irq.completion_eq_status, irq.completion_eq_syndrome,
               irq.completion_eq_fw_status);
    if (irq_rc == 0)
        printf("completion EQ variants: accepted=%u tried=0x%x syndromes=%08x %08x %08x %08x\n",
               irq.completion_eq_variant, irq.completion_eq_variant_tried,
               irq.completion_eq_variant_syndrome[0],
               irq.completion_eq_variant_syndrome[1],
               irq.completion_eq_variant_syndrome[2],
               irq.completion_eq_variant_syndrome[3]);
    if (irq_rc == 0)
        printf("EQ service: async_irq=%llu completion_irq=%llu timer_ticks=%llu timer_period=%u ms\n",
               (unsigned long long)irq.async_interrupts,
               (unsigned long long)irq.completion_interrupts,
               (unsigned long long)irq.eq_timer_ticks, irq.eq_timer_period_ms);
    else
        printf("interrupts: query unavailable (rc=%d)\n", irq_rc);
    printf("before: external_methods=%llu mr_registers=%llu mr_bytes=%llu\n",
           (unsigned long long)before.external_methods,
           (unsigned long long)before.mr_registers,
           (unsigned long long)before.mr_bytes);

    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    if (!pd) { fprintf(stderr, "ibv_alloc_pd failed\n"); goto out_ctx; }
    void *buffer = malloc(bytes);
    if (!buffer) { fprintf(stderr, "malloc failed\n"); ibv_dealloc_pd(pd); goto out_ctx; }
    memset(buffer, 0x5a, bytes);
    struct ibv_mr *mr = ibv_reg_mr(pd, buffer, bytes,
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!mr) {
        fprintf(stderr, "ibv_reg_mr failed: %s\n", strerror(errno));
        free(buffer); ibv_dealloc_pd(pd); goto out_ctx;
    }
    printf("MR OK: bytes=%zu lkey=0x%x\n", bytes, mr->lkey);
    if (ibv_dereg_mr(mr)) fprintf(stderr, "ibv_dereg_mr failed\n");
    free(buffer);
    ibv_dealloc_pd(pd);

    if (ibv_mlx5_query_perf(ctx, &after)) {
        fprintf(stderr, "QueryPerf after failed: %s\n", strerror(errno)); goto out_ctx;
    }
    if (ibv_mlx5_query_telemetry(ctx, &telemetry_after)) {
        fprintf(stderr, "QueryTelemetry after failed: %s\n", strerror(errno)); goto out_ctx;
    }
    print_delta(&before, &after);
    print_telemetry_delta(&telemetry_before, &telemetry_after);
    if (after.mr_registers > before.mr_registers &&
        after.mr_deregisters > before.mr_deregisters &&
        after.mr_bytes - before.mr_bytes == bytes) {
        printf("QUERY_PERF PASS\n");
        rc = 0;
    } else {
        printf("QUERY_PERF FAIL: MR counters did not advance as expected\n");
    }
out_ctx:
    ibv_close_device(ctx);
out_devices:
    ibv_free_device_list(devices);
    return rc;
}
