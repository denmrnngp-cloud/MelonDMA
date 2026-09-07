/* mlx_rtt_bench — what one operation costs on the direct path.
 *
 * The only latency figure this project has ever published, 73 us, came from
 * the old synchronous kernel-mediated path. The trusted path with a direct
 * UAR and a directly polled CQ was never measured, so there is no number to
 * judge interrupt delivery or completion moderation against. This tool
 * produces one.
 *
 * A single RC QP writes to its own registered buffer, so the measured span is
 * doorbell -> the card's DMA read -> the wire and back -> DMA write -> ACK ->
 * CQE -> poll. That is not a two-machine round trip; it is the cost of one
 * operation on this host's path, which is the quantity every task in front A
 * is trying to move. Small messages keep bandwidth out of the result.
 *
 * Each iteration is timed on its own, so the distribution is visible: a mean
 * hides exactly the tail that a missing interrupt or a moderation period
 * creates.
 *
 * Usage:
 *   mlx_rtt_bench [msg_size] [iterations]
 *   mlx_rtt_bench --sweep 0:0,4:1,8:4,16:8   A/B hardware CQ moderation,
 *                                            each arm "period_us:max_count"
 *   mlx_rtt_bench --post [msg] [iters]       cost of building and ringing one
 *                                            WQE, by shape: inline, 1/4/16 SGE
 *   mlx_rtt_bench --reg [reps]               cost of registering memory, by
 *                                            region size. This is the charge
 *                                            the KV path pays per request when
 *                                            it has no MR cache.
 * Env: MELONDMA_DIRECT_UAR=0 / MELONDMA_DIRECT_CQ=0 force the kernel fallback,
 *      for an A/B against the trusted path.
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

#define MAX_ARMS 16

/* CLOCK_MONOTONIC quantises to whole microseconds on macOS, which is coarse
 * enough to distort a single-digit-microsecond distribution. CLOCK_UPTIME_RAW
 * through the _np accessor is the nanosecond-resolution mach timebase. */
static uint64_t now_ns(void)
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* Percentile on an already-sorted array, nearest-rank. */
static double pct(const uint64_t *sorted, long n, double p)
{
    if (n <= 0) return 0.0;
    long idx = (long)(p * (double)(n - 1) + 0.5);
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    return (double)sorted[idx] / 1000.0;   /* ns -> us */
}

/* How the WQE is shaped. This is what the posting cost is a function of:
 * inline copies the payload into the WQE and saves the card a DMA read, and
 * every extra scatter/gather entry is another 16 bytes the CPU builds and the
 * card parses. */
struct shape {
    const char *name;
    int   inl;      /* IBV_SEND_INLINE */
    int   nsge;
};

struct arm { uint32_t period; uint32_t max_count; };

static const struct shape kDefaultShape = { "1 sge", 0, 1 };

/* Posting-cost sweep: same bytes every time, only the WQE shape changes. */
static const struct shape kPostShapes[] = {
    { "inline",  1,  1 },
    { "1 sge",   0,  1 },
    { "4 sge",   0,  4 },
    { "16 sge",  0, 16 },
};

/* Parses "p:c,p:c,..." into arms. Returns the count, or -1 on a malformed
 * spec: a silently dropped arm would turn a comparison into a wrong one. */
static int parse_sweep(const char *spec, struct arm *arms, int max)
{
    int n = 0;
    const char *p = spec;
    while (*p && n < max) {
        char *end = NULL;
        long period = strtol(p, &end, 10);
        if (end == p || *end != ':') return -1;
        p = end + 1;
        long count = strtol(p, &end, 10);
        if (end == p) return -1;
        if (period < 0 || period > 4095 || count < 0 || count > 65535) return -1;
        arms[n].period = (uint32_t)period;
        arms[n].max_count = (uint32_t)count;
        n++;
        p = end;
        if (*p == ',') p++;
        else if (*p) return -1;
    }
    return n;
}

struct ctx {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;
    uint8_t *buf;
    size_t msg;
};


/* One arm: `iters` timed writes after `warmup` untimed ones. post_ns collects
 * the time ibv_post_send itself takes, so the doorbell cost can be told apart
 * from the wait. */
#define MAX_SGE_ARM 16
/* The provider rejects the shape outright rather than failing mid-run; the
 * compat layer accepts inline only for SEND, not for RDMA_WRITE. */
#define SHAPE_UNSUPPORTED 1
#define GUARD_BYTES 64

static int run_arm(struct ctx *c, const struct shape *sh, long warmup,
                   long iters, uint64_t *lat_ns, uint64_t *post_ns)
{
    /* Split the same payload across nsge entries so every arm moves the same
     * bytes and only the WQE shape differs. */
    struct ibv_sge sge[MAX_SGE_ARM];
    int nsge = sh->nsge < 1 ? 1 : sh->nsge > MAX_SGE_ARM ? MAX_SGE_ARM : sh->nsge;
    size_t chunk = c->msg / (size_t)nsge;
    if (!chunk) { nsge = 1; chunk = c->msg; }
    for (int k = 0; k < nsge; k++) {
        sge[k].addr = (uintptr_t)c->buf + (size_t)k * chunk;
        sge[k].length = (uint32_t)(k == nsge - 1
                                   ? c->msg - chunk * (size_t)(nsge - 1)
                                   : chunk);
        sge[k].lkey = c->mr->lkey;
    }

    /* Guard bytes just past the destination catch a write that lands longer
     * than it should; the pattern below catches one that lands short or stale. */
    uint8_t guard[GUARD_BYTES];
    memset(guard, 0xa5, sizeof(guard));
    memcpy(c->buf + 2 * c->msg, guard, sizeof(guard));

    for (long i = 0; i < warmup + iters; i++) {
        /* Stamp the source so a destination left over from the previous
         * iteration cannot pass the comparison. Done before the timer starts. */
        uint64_t stamp = (uint64_t)i + 0x5eed;
        memcpy(c->buf, &stamp, sizeof(stamp));
        memset(c->buf + c->msg, 0, c->msg);

        struct ibv_send_wr wr = { .wr_id = (uint64_t)i + 1, .sg_list = sge,
            .num_sge = nsge, .opcode = IBV_WR_RDMA_WRITE,
            .send_flags = IBV_SEND_SIGNALED |
                          (sh->inl ? IBV_SEND_INLINE : 0) };
        wr.wr.rdma.remote_addr = (uintptr_t)c->buf + c->msg;
        wr.wr.rdma.rkey = c->mr->rkey;
        struct ibv_send_wr *bad = NULL;

        uint64_t t0 = now_ns();
        int prc = ibv_post_send(c->qp, &wr, &bad);
        if (prc == EINVAL && i == 0) return SHAPE_UNSUPPORTED;
        if (prc) {
            fprintf(stderr, "post_send failed at %ld: %d\n", i, prc);
            return -1;
        }
        uint64_t t1 = now_ns();
        struct ibv_wc wc = {};
        for (;;) {
            int got = ibv_poll_cq(c->cq, 1, &wc);
            if (got < 0) { fprintf(stderr, "poll_cq error\n"); return -1; }
            if (got == 1) break;
        }
        uint64_t t2 = now_ns();
        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "WC error at %ld: %d\n", i, wc.status);
            return -1;
        }
        if (i >= warmup) {
            lat_ns[i - warmup]  = t2 - t0;
            post_ns[i - warmup] = t1 - t0;
        }
        /* Verified outside the timed span, so it costs the numbers nothing. */
        if (memcmp(c->buf, c->buf + c->msg, c->msg)) {
            fprintf(stderr, "PAYLOAD MISMATCH at %ld (%s)\n", i, sh->name);
            return -1;
        }
        if (memcmp(c->buf + 2 * c->msg, guard, sizeof(guard))) {
            fprintf(stderr, "GUARD OVERWRITTEN at %ld (%s)\n", i, sh->name);
            return -1;
        }
    }
    return 0;
}

/* Blue-flame WQEs actually written into the doorbell register during the arm.
 * Without this the A/B is unreadable: a run where blue flame never engaged
 * looks exactly like one where it did and changed nothing. */
static uint64_t bf_wqes(struct ibv_context *ctx)
{
    struct ibv_mlx5_telemetry t = {};
    return ibv_mlx5_query_telemetry(ctx, &t) == 0 ? t.blue_flame_wqes : 0;
}

static void report(const char *label, uint64_t *lat, uint64_t *post, long n)
{
    uint64_t total = 0;
    for (long i = 0; i < n; i++) total += lat[i];
    qsort(lat, (size_t)n, sizeof(*lat), cmp_u64);
    qsort(post, (size_t)n, sizeof(*post), cmp_u64);
    printf("%-14s min %7.2f  p50 %7.2f  p90 %7.2f  p99 %7.2f  max %8.2f  "
           "mean %7.2f  post_p50 %6.2f\n",
           label, pct(lat, n, 0.0), pct(lat, n, 0.50), pct(lat, n, 0.90),
           pct(lat, n, 0.99), pct(lat, n, 1.0),
           (double)total / (double)n / 1000.0, pct(post, n, 0.50));
    fflush(stdout);
}

/* Registration cost by region size. Reported separately from any transfer,
 * because in the production KV path the two are summed into one wait and the
 * registration disappears into it. Each rep registers a freshly touched
 * region so no pinning is reused. */
static int run_reg_bench(struct ibv_pd *pd, long reps)
{
    static const size_t sizes[] = {
        4u << 10, 64u << 10, 1u << 20, 16u << 20, 96u << 20, 149u << 20,
    };
    printf("%12s %10s %10s %10s %12s\n",
           "region", "reg p50", "reg p90", "dereg p50", "MB/s pinned");
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        size_t n = sizes[i];
        uint8_t *buf = malloc(n);
        if (!buf) { printf("%12zu  allocation failed\n", n); continue; }
        uint64_t *reg = calloc((size_t)reps, sizeof(*reg));
        uint64_t *dereg = calloc((size_t)reps, sizeof(*dereg));
        if (!reg || !dereg) { free(buf); free(reg); free(dereg); return -1; }
        long done = 0;
        for (long r = 0; r < reps; r++) {
            /* Touch every page so the cost measured is registration, not the
             * first-touch faults that would otherwise land inside it. */
            for (size_t o = 0; o < n; o += 4096) buf[o] = (uint8_t)r;
            uint64_t t0 = now_ns();
            struct ibv_mr *mr = ibv_reg_mr(pd, buf, n,
                IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
            uint64_t t1 = now_ns();
            if (!mr) break;
            (void)ibv_dereg_mr(mr);
            uint64_t t2 = now_ns();
            reg[done] = t1 - t0;
            dereg[done] = t2 - t1;
            done++;
        }
        if (!done) {
            printf("%12zu  registration refused (quota or size limit)\n", n);
        } else {
            qsort(reg, (size_t)done, sizeof(*reg), cmp_u64);
            qsort(dereg, (size_t)done, sizeof(*dereg), cmp_u64);
            double p50 = pct(reg, done, 0.50);
            printf("%9zu KiB %9.1fus %9.1fus %9.1fus %12.0f\n",
                   n >> 10, p50, pct(reg, done, 0.90),
                   pct(dereg, done, 0.50),
                   p50 > 0 ? (double)n / p50 : 0.0);
        }
        free(reg); free(dereg); free(buf);
    }
    return 0;
}

int main(int argc, char **argv)
{
    size_t msg = 64;
    long iters = 10000, warmup = 1000;
    struct arm arms[MAX_ARMS] = {{0, 0}};
    int narms = 1, post_mode = 0, reg_mode = 0;
    long reg_reps = 20;

    if (argc >= 2 && !strcmp(argv[1], "--reg")) {
        reg_mode = 1;
        if (argc > 2) reg_reps = strtol(argv[2], NULL, 0);
        if (reg_reps < 1) reg_reps = 1;
    } else if (argc >= 2 && !strcmp(argv[1], "--post")) {
        post_mode = 1;
        if (argc > 2) msg = (size_t)strtoull(argv[2], NULL, 0);
        if (argc > 3) iters = strtol(argv[3], NULL, 0);
    } else if (argc >= 3 && !strcmp(argv[1], "--sweep")) {
        narms = parse_sweep(argv[2], arms, MAX_ARMS);
        if (narms <= 0) { fprintf(stderr, "bad sweep spec\n"); return 2; }
        if (argc > 3) msg = (size_t)strtoull(argv[3], NULL, 0);
        if (argc > 4) iters = strtol(argv[4], NULL, 0);
    } else {
        if (argc > 1) msg = (size_t)strtoull(argv[1], NULL, 0);
        if (argc > 2) iters = strtol(argv[2], NULL, 0);
    }
    if (msg < 8 || msg > (1u << 20)) { fprintf(stderr, "msg out of range\n"); return 2; }
    if (iters < 1) iters = 1;
    if (warmup > iters) warmup = iters / 10 + 1;

    int rc = 1;
    uint64_t *lat = calloc((size_t)iters, sizeof(*lat));
    uint64_t *post = calloc((size_t)iters, sizeof(*post));
    if (!lat || !post) { perror("calloc"); goto out_mem; }

    struct ibv_device **devs = ibv_get_device_list(NULL);
    if (!devs || !devs[0]) { fprintf(stderr, "no devices\n"); goto out_mem; }
    struct ibv_context *ictx = ibv_open_device(devs[0]);
    if (!ictx) { perror("ibv_open_device"); goto out_devs; }

    struct ibv_pd *pd = ibv_alloc_pd(ictx);
    struct ibv_cq *cq = ibv_create_cq(ictx, 64, NULL, NULL, 0);
    if (!pd || !cq) { fprintf(stderr, "pd/cq failed\n"); goto out_cq; }

    size_t size = 2 * msg + GUARD_BYTES;
    uint8_t *buf = malloc(size);
    if (!buf) { perror("malloc"); goto out_cq; }
    memset(buf, 0, size);
    struct ibv_mr *mr = ibv_reg_mr(pd, buf, size,
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
    if (!mr) { fprintf(stderr, "ibv_reg_mr failed\n"); goto out_buf; }

    struct ibv_qp_init_attr init = { .send_cq = cq, .recv_cq = cq,
        .qp_type = IBV_QPT_RC, .cap = { .max_send_wr = 128, .max_recv_wr = 8,
        .max_send_sge = MAX_SGE_ARM, .max_recv_sge = 1,
        .max_inline_data = 512 } };
    struct ibv_qp *qp = ibv_create_qp(pd, &init);
    if (!qp) { fprintf(stderr, "ibv_create_qp failed\n"); goto out_mr; }

    const char *ip = getenv("MELONDMA_LOCAL_IP");
    const char *macText = getenv("MELONDMA_LOCAL_MAC");
    if (!ip) ip = "192.168.200.1";
    if (!macText) macText = "98:03:9b:80:6a:94";
    struct ibv_mlx5_roce_config roce = { .hop_limit = 1 };
    uint8_t ipv4[4] = {};
    if (inet_pton(AF_INET, ip, ipv4) != 1 ||
        sscanf(macText, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
               &roce.local_mac[0], &roce.local_mac[1], &roce.local_mac[2],
               &roce.local_mac[3], &roce.local_mac[4], &roce.local_mac[5]) != 6) {
        fprintf(stderr, "bad IP/MAC\n"); goto out_qp;
    }
    memset(&roce.local_gid, 0, sizeof(roce.local_gid));
    roce.local_gid.raw[10] = 0xff;
    roce.local_gid.raw[11] = 0xff;
    memcpy(roce.local_gid.raw + 12, ipv4, sizeof(ipv4));
    roce.l3_type = 0;
    memcpy(roce.peer_mac, roce.local_mac, sizeof(roce.peer_mac));
    if (ibv_mlx5_configure_roce(ictx, &roce)) {
        fprintf(stderr, "configure_roce failed\n"); goto out_qp; }

    union ibv_gid gid = roce.local_gid;
    uint8_t sgid = 0;
    struct ibv_gid_entry entries[64] = {};
    uint32_t n = 0, tableSize = 0;
    if (ibv_query_gid_table(ictx, 1, entries, 64, &n, &tableSize)) {
        fprintf(stderr, "gid table failed\n"); goto out_qp; }
    int found = 0;
    for (uint32_t i = 0; i < n; i++)
        if (entries[i].gid_type == IBV_GID_TYPE_ROCE_V2 &&
            !memcmp(entries[i].gid.raw, gid.raw, 16)) {
            sgid = (uint8_t)entries[i].gid_index; found = 1; break; }
    if (!found) { fprintf(stderr, "no RoCEv2 GID\n"); goto out_qp; }

    uint32_t psn = ((uint32_t)getpid() * 2654435761u) & 0xffffff;
    struct ibv_qp_attr attr = { .qp_state = IBV_QPS_INIT, .pkey_index = 0,
        .port_num = 1,
        .qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ };
    if (ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                      IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
        fprintf(stderr, "RST->INIT failed\n"); goto out_qp; }
    attr = (struct ibv_qp_attr){ .qp_state = IBV_QPS_RTR,
        .path_mtu = IBV_MTU_1024, .dest_qp_num = qp->qp_num, .rq_psn = psn,
        .max_dest_rd_atomic = 4, .min_rnr_timer = 12,
        .ah_attr = { .is_global = 1, .port_num = 1, .sl = 0,
        .grh = { .dgid = gid, .sgid_index = sgid, .hop_limit = 1 } } };
    if (ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                      IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                      IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER)) {
        fprintf(stderr, "INIT->RTR failed\n"); goto out_qp; }
    attr = (struct ibv_qp_attr){ .qp_state = IBV_QPS_RTS, .sq_psn = psn,
        .timeout = 14, .retry_cnt = 7, .rnr_retry = 7, .max_rd_atomic = 4 };
    if (ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_SQ_PSN |
                      IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                      IBV_QP_MAX_QP_RD_ATOMIC)) {
        fprintf(stderr, "RTR->RTS failed\n"); goto out_qp; }

    for (size_t i = 0; i < msg; i++) buf[i] = (uint8_t)(i * 7 + 1);
    printf("RTT_BENCH: every iteration is byte-checked against a stamped "
           "source plus %d guard bytes\n", GUARD_BYTES);

    struct ibv_mlx5_telemetry tel = {};
    int have_tel = (ibv_mlx5_query_telemetry(ictx, &tel) == 0);
    printf("RTT_BENCH: msg=%zu warmup=%ld iters=%ld arms=%d%s\n",
           msg, warmup, iters, narms,
           have_tel && tel.direct_doorbells ? " (direct doorbell path)" : "");

    struct ibv_mlx5_posting_caps pc = {};
    double line = 0.0;
    if (ibv_mlx5_query_posting_caps(ictx, &pc) == 0) {
        printf("POSTING: blue_flame=%s log_bf_reg_size=%u uar_page=%u "
               "bf_regs_per_uar=%u max_inline=%u max_sge=%u\n",
               pc.bf_supported ? "yes" : "NO (card reports none)",
               pc.log_bf_reg_size, pc.uar_page_size, pc.bf_regs_per_uar,
               pc.max_inline_data, pc.max_sge);
        line = ibv_mlx5_pcie_line_gbps(pc.pcie_link_speed, pc.pcie_link_width);
        if (line > 0.0)
            printf("LINE: PCIe gen%u x%u, %.1f Gbit/s per direction — report "
                   "rates as a fraction of this\n",
                   pc.pcie_link_speed, pc.pcie_link_width, line);
        else
            printf("LINE: negotiated PCIe link unreadable; rates below cannot "
                   "be compared across machines\n");
    }

    struct ctx c = { ictx, pd, cq, qp, mr, buf, msg };

    if (reg_mode) {
        struct ibv_mlx5_perf p0 = {};
        int have_p0 = (ibv_mlx5_query_perf(ictx, &p0) == 0);
        printf("RTT_BENCH: registration cost, %ld reps per size\n", reg_reps);
        rc = run_reg_bench(pd, reg_reps) ? 1 : 0;
        struct ibv_mlx5_perf p1 = {};
        if (have_p0 && ibv_mlx5_query_perf(ictx, &p1) == 0) {
            uint64_t cmds = p1.fw_commands - p0.fw_commands;
            uint64_t slept = p1.fw_command_sleeps - p0.fw_command_sleeps;
            printf("FW_CMD: %llu commands, %llu of them slept past the spin "
                   "window (%.1f%%)\n", (unsigned long long)cmds,
                   (unsigned long long)slept,
                   cmds ? 100.0 * (double)slept / (double)cmds : 0.0);
        }
        goto out_qp;
    }

    if (post_mode) {
        printf("%-14s %s\n", "WQE shape", "all figures in microseconds");
        for (size_t i = 0; i < sizeof(kPostShapes) / sizeof(kPostShapes[0]); i++) {
            if (kPostShapes[i].inl && msg > pc.max_inline_data) {
                printf("%-14s skipped, message exceeds the inline ceiling\n",
                       kPostShapes[i].name);
                continue;
            }
            int arc = run_arm(&c, &kPostShapes[i], warmup, iters, lat, post);
            if (arc == SHAPE_UNSUPPORTED) {
                printf("%-14s rejected by the provider for RDMA_WRITE\n",
                       kPostShapes[i].name);
                continue;
            }
            if (arc) goto out_qp;
            report(kPostShapes[i].name, lat, post, iters);
        }
        rc = 0;
        goto out_qp;
    }

    printf("%-14s %s\n", "moderation", "all figures in microseconds");
    for (int a = 0; a < narms; a++) {
        int mrc = ibv_mlx5_modify_cq_moderation(cq, arms[a].period,
                                                arms[a].max_count);
        if (mrc == ENOTSUP && (arms[a].period || arms[a].max_count)) {
            printf("  moderation unsupported by this provider, arm skipped\n");
            continue;
        }
        if (mrc && mrc != ENOTSUP) {
            fprintf(stderr, "moderation %u:%u refused: %d\n",
                    arms[a].period, arms[a].max_count, mrc);
            goto out_qp;
        }
        uint64_t bf0 = bf_wqes(ictx);
        if (run_arm(&c, &kDefaultShape, warmup, iters, lat, post) != 0) goto out_qp;
        uint64_t bfn = bf_wqes(ictx) - bf0;
        char label[32];
        if (!arms[a].period && !arms[a].max_count)
            snprintf(label, sizeof(label), "off");
        else
            snprintf(label, sizeof(label), "%uus:%u",
                     arms[a].period, arms[a].max_count);
        report(label, lat, post, iters);
        printf("%-14s blue-flame WQEs written this arm: %llu of %ld posts\n",
               "", (unsigned long long)bfn, warmup + iters);
    }
    /* Leave the CQ as it was found. */
    (void)ibv_mlx5_modify_cq_moderation(cq, 0, 0);

    struct ibv_mlx5_interrupts irq = {};
    if (ibv_mlx5_query_interrupts(ictx, &irq) == 0)
        printf("RTT_BENCH: async_irq=%" PRIu64 " completion_irq=%" PRIu64
               " timer_period=%ums\n",
               irq.async_interrupts, irq.completion_interrupts,
               irq.eq_timer_period_ms);
    rc = 0;

out_qp:
    ibv_destroy_qp(qp);
out_mr:
    ibv_dereg_mr(mr);
out_buf:
    free(buf);
out_cq:
    if (cq) ibv_destroy_cq(cq);
    if (pd) ibv_dealloc_pd(pd);
    ibv_close_device(ictx);
out_devs:
    ibv_free_device_list(devs);
out_mem:
    free(lat);
    free(post);
    return rc;
}
