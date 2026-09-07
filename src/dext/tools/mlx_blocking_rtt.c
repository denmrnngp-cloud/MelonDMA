/* mlx_blocking_rtt — moderation sweep on the BLOCKING completion path.
 *
 * mlx_rtt_bench --sweep measures the busy-poll path, where hardware CQ
 * moderation is a no-op (it delays the completion EVENT, not the CQE write).
 * The blocking consumer (completion channel + WaitCqEvent) is the path that
 * actually benefits: moderation coalesces events, so fewer wakeups at the cost
 * of up to `period` us of added latency. This tool measures that tradeoff:
 * a loopback RDMA_WRITE, armed CQ, block on the channel event, poll the CQE,
 * with per-arm p50 RTT and the event count.
 *
 * Usage: mlx_blocking_rtt <sweep-spec> [msg] [iters]
 *        mlx_blocking_rtt 0:0,4:1,16:4,64:16 64 20000
 *        (sweep spec: period_us:max_count, comma-separated)
 */
#include <infiniband/verbs.h>
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_ARMS 8
struct arm { uint32_t period, max_count; };

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}
static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

static int parse_sweep(const char *spec, struct arm *arms, int max)
{
    int n = 0; const char *p = spec;
    while (*p && n < max) {
        char *end = NULL;
        arms[n].period = (uint32_t)strtoul(p, &end, 0);
        if (end == p || *end != ':') return -1;
        p = end + 1;
        arms[n].max_count = (uint32_t)strtoul(p, &end, 0);
        if (end == p) return -1;
        p = *end == ',' ? end + 1 : end;
        n++;
    }
    return n;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <sweep-spec> [msg] [iters] [batch]\n", argv[0]); return 2; }
    struct arm arms[MAX_ARMS];
    int narms = parse_sweep(argv[1], arms, MAX_ARMS);
    if (narms <= 0) { fprintf(stderr, "bad sweep spec\n"); return 2; }
    size_t msg = argc > 2 ? (size_t)strtoull(argv[2], NULL, 0) : 64;
    long iters = argc > 3 ? strtol(argv[3], NULL, 0) : 20000;
    /* Operations posted per arm. Moderation coalesces CQEs, so it can only
     * show itself when more than one completion is outstanding; with a batch
     * of one the max_count threshold can never bind. Capped by the CQ depth. */
    long batch = argc > 4 ? strtol(argv[4], NULL, 0) : 1;
    if (batch < 1 || batch > 32) { fprintf(stderr, "batch must be 1..32\n"); return 2; }
    if (msg < 8 || msg > (1u << 20) || iters < 1) { fprintf(stderr, "bad msg/iters\n"); return 2; }

    int rc = 1;
    uint64_t *lat = calloc((size_t)iters, sizeof(*lat));
    if (!lat) return 1;

    struct ibv_device **devs = ibv_get_device_list(NULL);
    if (!devs || !devs[0]) { fprintf(stderr, "no devices\n"); goto out_mem; }
    struct ibv_context *ctx = ibv_open_device(devs[0]);
    if (!ctx) { perror("ibv_open_device"); goto out_devs; }
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    struct ibv_comp_channel *ch = pd ? ibv_create_comp_channel(ctx) : NULL;
    struct ibv_cq *cq = ch ? ibv_create_cq(ctx, 64, NULL, ch, 0) : NULL;
    if (!pd || !ch || !cq) { fprintf(stderr, "pd/ch/cq failed\n"); goto out_cq; }

    size_t size = 2 * msg;
    uint8_t *buf = calloc(1, size);
    struct ibv_mr *mr = buf ? ibv_reg_mr(pd, buf, size,
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE) : NULL;
    if (!mr) { fprintf(stderr, "mr failed\n"); goto out_buf; }
    struct ibv_qp_init_attr init = { .send_cq = cq, .recv_cq = cq,
        .qp_type = IBV_QPT_RC, .cap = { .max_send_wr = 128, .max_recv_wr = 8,
        .max_send_sge = 1, .max_recv_sge = 1, .max_inline_data = 512 } };
    struct ibv_qp *qp = ibv_create_qp(pd, &init);
    if (!qp) { fprintf(stderr, "qp failed\n"); goto out_mr; }

    const char *ip = getenv("MELONDMA_LOCAL_IP");
    const char *macText = getenv("MELONDMA_LOCAL_MAC");
    if (!ip) ip = "192.168.200.1";
    if (!macText) macText = "98:03:9b:80:6a:94";
    struct ibv_mlx5_roce_config roce = { .hop_limit = 1 };
    uint8_t ipv4[4] = {};
    if (inet_pton(AF_INET, ip, ipv4) != 1 ||
        sscanf(macText, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
               &roce.local_mac[0], &roce.local_mac[1], &roce.local_mac[2],
               &roce.local_mac[3], &roce.local_mac[4], &roce.local_mac[5]) != 6)
        goto out_qp;
    memset(&roce.local_gid, 0, sizeof(roce.local_gid));
    roce.local_gid.raw[10] = 0xff; roce.local_gid.raw[11] = 0xff;
    memcpy(roce.local_gid.raw + 12, ipv4, 4);
    roce.l3_type = 0;
    memcpy(roce.peer_mac, roce.local_mac, 6);
    if (ibv_mlx5_configure_roce(ctx, &roce)) goto out_qp;

    union ibv_gid gid = roce.local_gid;
    uint8_t sgid = 0;
    struct ibv_gid_entry entries[64] = {};
    uint32_t n = 0, tableSize = 0;
    if (ibv_query_gid_table(ctx, 1, entries, 64, &n, &tableSize)) goto out_qp;
    for (uint32_t i = 0; i < n; i++)
        if (entries[i].gid_type == IBV_GID_TYPE_ROCE_V2 &&
            !memcmp(entries[i].gid.raw, gid.raw, 16)) { sgid = (uint8_t)entries[i].gid_index; break; }

    uint32_t psn = ((uint32_t)getpid() * 2654435761u) & 0xffffff;
    struct ibv_qp_attr a = { .qp_state = IBV_QPS_INIT, .pkey_index = 0,
        .port_num = 1, .qp_access_flags = IBV_ACCESS_REMOTE_WRITE };
    if (ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) goto out_qp;
    a = (struct ibv_qp_attr){ .qp_state = IBV_QPS_RTR, .path_mtu = IBV_MTU_1024,
        .dest_qp_num = qp->qp_num, .rq_psn = psn, .max_dest_rd_atomic = 1,
        .min_rnr_timer = 12, .ah_attr = { .is_global = 1, .port_num = 1, .sl = 0,
        .grh = { .dgid = gid, .sgid_index = sgid, .hop_limit = 1 } } };
    if (ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                      IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER)) goto out_qp;
    a = (struct ibv_qp_attr){ .qp_state = IBV_QPS_RTS, .sq_psn = psn,
        .timeout = 14, .retry_cnt = 7, .rnr_retry = 7, .max_rd_atomic = 1 };
    if (ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT |
                      IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC)) goto out_qp;

    printf("BLOCKING_RTT: msg=%zu iters=%ld batch=%ld, moderation sweep (period_us:max_count)\n"
           "  RTT is per batch; events/iter is CQ events consumed per batch\n", msg, iters, batch);
    printf("%-12s %10s %10s %10s\n", "moderation", "p50 RTT", "p99 RTT", "events/iter");

    for (int arm = 0; arm < narms; arm++) {
        int mrc = ibv_mlx5_modify_cq_moderation(cq, arms[arm].period, arms[arm].max_count);
        if (mrc && mrc != ENOTSUP) { fprintf(stderr, "moderation %u:%u refused: %d\n",
            arms[arm].period, arms[arm].max_count, mrc); goto out_qp; }
        /* A flat curve means nothing unless the setting was actually applied.
         * ENOTSUP is tolerated above, so say so instead of swallowing it. */
        fprintf(stderr, "moderation %u:%u -> %s\n", arms[arm].period,
                arms[arm].max_count, mrc == 0 ? "applied" :
                mrc == ENOTSUP ? "ENOTSUP (NOT applied)" : "error");
        uint64_t ev_before = 0; (void)ev_before;
        uint64_t events_total = 0;
        for (long i = 0; i < iters; i++) {
            struct ibv_send_wr *bad = NULL;
            /* Arm BEFORE posting. req_notify_cq only covers the next CQE, and
             * on this path the completion lands in single-digit microseconds,
             * so arming after the post loses the race and blocks forever. The
             * moderation period delays the event, not the CQE. */
            struct ibv_cq *ev_cq = NULL; void *ev_ctx = NULL;
            if (ibv_req_notify_cq(cq, 0)) { fprintf(stderr, "arm failed\n"); goto out_qp; }
            uint64_t t0 = now_ns();
            for (long b = 0; b < batch; b++) {
                struct ibv_sge sg = { (uintptr_t)buf, (uint32_t)msg, mr->lkey };
                struct ibv_send_wr wr = { .wr_id = (uint64_t)(i * batch + b) + 1,
                    .sg_list = &sg, .num_sge = 1, .opcode = IBV_WR_RDMA_WRITE,
                    .send_flags = IBV_SEND_SIGNALED };
                wr.wr.rdma.remote_addr = (uintptr_t)buf + msg;
                wr.wr.rdma.rkey = mr->rkey;
                if (ibv_post_send(qp, &wr, &bad)) { fprintf(stderr, "post failed\n"); goto out_qp; }
            }
            /* Reap the whole batch, counting how many events it took. One event
             * covering many CQEs is exactly what moderation is supposed to buy,
             * so this ratio is the measurement; the absolute value also counts
             * the software batching implied by arm-once-drain-many, which is why
             * only the trend across moderation settings is meaningful. */
            long reaped = 0;
            while (reaped < batch) {
                if (ibv_get_cq_event(ch, &ev_cq, &ev_ctx)) { fprintf(stderr, "get_cq_event failed\n"); goto out_qp; }
                ibv_ack_cq_events(cq, 1);
                events_total++;
                if (ibv_req_notify_cq(cq, 0)) { fprintf(stderr, "re-arm failed\n"); goto out_qp; }
                struct ibv_wc wc[32];
                int got;
                while ((got = ibv_poll_cq(cq, 32, wc)) > 0) {
                    for (int k = 0; k < got; k++)
                        if (wc[k].status != IBV_WC_SUCCESS) {
                            fprintf(stderr, "wc status %d\n", wc[k].status); goto out_qp; }
                    reaped += got;
                }
                if (got < 0) { fprintf(stderr, "poll err\n"); goto out_qp; }
            }
            lat[i] = now_ns() - t0;
        }
        qsort(lat, (size_t)iters, sizeof(*lat), cmp_u64);
        char label[16];
        if (!arms[arm].period && !arms[arm].max_count) snprintf(label, sizeof(label), "off");
        else snprintf(label, sizeof(label), "%u:%u", arms[arm].period, arms[arm].max_count);
        printf("%-12s %10.2f %10.2f %10.2f\n", label,
               lat[iters / 2] / 1000.0, lat[iters * 99 / 100] / 1000.0,
               (double)events_total / (double)iters);
    }
    (void)ibv_mlx5_modify_cq_moderation(cq, 0, 0);
    rc = 0;

out_qp:
    ibv_destroy_qp(qp);
out_mr:
    ibv_dereg_mr(mr);
out_buf:
    free(buf);
out_cq:
    if (cq) ibv_destroy_cq(cq);
    if (ch) ibv_destroy_comp_channel(ch);
    if (pd) ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
out_devs:
    ibv_free_device_list(devs);
out_mem:
    free(lat);
    return rc;
}
