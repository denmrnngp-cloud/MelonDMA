/* Idle cost of an armed completion channel.
 *
 * Arms one CQ, then sits idle for a fixed window while the libibverbs
 * completion worker runs behind it, and reports the CPU that window cost.
 * This is the measurement the blocking-delivery path exists for: with
 * MELONDMA_HW_CQ_EVENT=0 the worker polls the mapped CQE ring on a timer,
 * and without it the worker blocks in the DEXT until an MSI-X completion
 * event advances the generation.
 *
 * Usage: mlx_cq_idle_cpu [seconds]
 */
#include <infiniband/verbs.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

static double cpu_seconds(void)
{
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) return 0.0;
    return (double)ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 +
           (double)ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}

static double wall_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    double window = argc > 1 ? strtod(argv[1], NULL) : 5.0;
    if (window < 0.5 || window > 120.0) { fprintf(stderr, "window out of range\n"); return 2; }
    int rc = 1;

    struct ibv_device **devs = ibv_get_device_list(NULL);
    if (!devs || !devs[0]) { fprintf(stderr, "no devices\n"); return 1; }
    struct ibv_context *ctx = ibv_open_device(devs[0]);
    if (!ctx) { perror("ibv_open_device"); goto out_devs; }
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    struct ibv_comp_channel *ch = ibv_create_comp_channel(ctx);
    struct ibv_cq *cq = ch ? ibv_create_cq(ctx, 64, NULL, ch, 0) : NULL;
    if (!pd || !cq) { fprintf(stderr, "pd/cq failed\n"); goto out_ctx; }

    struct ibv_mlx5_interrupts irq = {};
    int irq_rc = ibv_mlx5_query_interrupts(ctx, &irq);
    /* An older provider answers the feature bit but not this query, and the
     * worker still blocks; do not report that as "no blocking delivery". */
    const char *mode = irq_rc != 0 ? "unknown (provider predates the query)" :
        !irq.completion_ready ? "IRQ unproven (timer/hybrid possible)" :
        irq.completion_eqn ? "dedicated completion EQ" : "shared async EQ";

    const char *mod = getenv("MELONDMA_CQ_MODERATION");
    if (mod && mod[0]) {
        char *end = NULL;
        long period = strtol(mod, &end, 10);
        long max_count = (end && *end == ':') ? strtol(end + 1, NULL, 10) : 0;
        int mrc = ibv_mlx5_modify_cq_moderation(cq, (uint32_t)period,
                                                (uint32_t)max_count);
        printf("CQ_IDLE_CPU: moderation=%ld:%ld rc=%d (%s)\n", period, max_count,
               mrc, mrc == 0 ? "accepted" :
                    mrc == ENOTSUP ? "provider predates it" : "rejected");
    } else {
        printf("CQ_IDLE_CPU: moderation=off\n");
    }
    if (ibv_req_notify_cq(cq, 0)) { fprintf(stderr, "req_notify failed\n"); goto out_cq; }

    struct ibv_mlx5_telemetry before = {}, after = {};
    ibv_mlx5_query_telemetry(ctx, &before);
    double cpu0 = cpu_seconds(), wall0 = wall_seconds();
    struct timespec nap = { (time_t)window, (long)((window - (double)(time_t)window) * 1e9) };
    while (nanosleep(&nap, &nap) != 0 && errno == EINTR) {}
    double cpu = cpu_seconds() - cpu0, wall = wall_seconds() - wall0;
    ibv_mlx5_query_telemetry(ctx, &after);

#define TD(f) ((long long)(after.f - before.f))
    printf("CQ_IDLE_CPU: provider=%s window=%.1f s\n", mode, wall);
    printf("  cpu=%.1f ms  core=%.3f %%\n", cpu * 1000.0, wall > 0 ? cpu / wall * 100.0 : 0.0);
    printf("  hw_waits=%lld hw_wakeups=%lld poll_ticks=%lld poll_wakeups=%lld\n",
           TD(completion_hw_waits), TD(completion_hw_wakeups),
           TD(completion_poll_ticks), TD(completion_poll_wakeups));
    printf("  event_waits=%lld (includes timer-backed blocking)\n", TD(completion_waits));
    printf("  driver_cq_events=%lld driver_cq_wakeups=%lld lost=%lld\n",
           TD(driver_cq_events), TD(driver_cq_event_wakeups),
           TD(completion_lost_events));
#undef TD
    rc = 0;
out_cq:
    if (cq) ibv_destroy_cq(cq);
    if (ch) ibv_destroy_comp_channel(ch);
    if (pd) ibv_dealloc_pd(pd);
out_ctx:
    ibv_close_device(ctx);
out_devs:
    ibv_free_device_list(devs);
    return rc;
}
