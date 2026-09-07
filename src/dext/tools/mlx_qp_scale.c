/* mlx_qp_scale — does the driver scale with QPs, measured where bandwidth
 * cannot hide the answer.
 *
 * The only scaling number this project had compared 8 QPs against 1 on 1 MiB
 * messages: 21.16 against 20.7 Gbit/s. That measured the host PCIe link, not
 * the driver. This tool uses a small message and reports operations per
 * second, so the result is about posting and completion, not the wire.
 *
 * Each lane is its own QP and its own CQ in its own thread; all lanes start
 * together on a barrier. Lanes of one client share that client's UAR page, so
 * this is also the test for the per-QP blue-flame register: without it every
 * lane writes the same doorbell and the curve flattens early.
 *
 * Usage: mlx_qp_scale [levels] [msg] [iters_per_qp]
 *        mlx_qp_scale "1 2 4 8" 64 20000
 */
#include <infiniband/verbs.h>
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_LANES 32

/* macOS has no pthread_barrier_t. A spin barrier is right here anyway: the
 * point is that every lane starts posting at the same instant, and a few
 * microseconds of spinning before a multi-second run costs nothing. */
struct barrier { atomic_uint arrived; unsigned n; };

static void barrier_wait(struct barrier *b)
{
    atomic_fetch_add_explicit(&b->arrived, 1u, memory_order_acq_rel);
    while (atomic_load_explicit(&b->arrived, memory_order_acquire) < b->n)
        sched_yield();
}

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

struct lane {
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;
    uint8_t *buf;
    size_t   msg;
    long     iters;
    struct barrier *start;
    uint64_t ns;
    int      failed;
};

static void *lane_run(void *arg)
{
    struct lane *L = (struct lane *)arg;
    barrier_wait(L->start);
    uint64_t t0 = now_ns();
    for (long i = 0; i < L->iters; i++) {
        uint64_t stamp = (uint64_t)i + 0x5eed;
        memcpy(L->buf, &stamp, sizeof(stamp));
        memset(L->buf + L->msg, 0, L->msg);
        struct ibv_sge sge = { .addr = (uintptr_t)L->buf,
                               .length = (uint32_t)L->msg, .lkey = L->mr->lkey };
        struct ibv_send_wr wr = { .wr_id = (uint64_t)i + 1, .sg_list = &sge,
            .num_sge = 1, .opcode = IBV_WR_RDMA_WRITE,
            .send_flags = IBV_SEND_SIGNALED };
        wr.wr.rdma.remote_addr = (uintptr_t)L->buf + L->msg;
        wr.wr.rdma.rkey = L->mr->rkey;
        struct ibv_send_wr *bad = NULL;
        if (ibv_post_send(L->qp, &wr, &bad)) { L->failed = 1; break; }
        struct ibv_wc wc = {};
        int broke = 0;
        for (;;) {
            int got = ibv_poll_cq(L->cq, 1, &wc);
            if (got < 0) { L->failed = 1; broke = 1; break; }
            if (got == 1) break;
        }
        if (broke || wc.status != IBV_WC_SUCCESS) { L->failed = 1; break; }
        /* Every lane checks its own bytes: a scaling bug that corrupts is
         * worse than one that is slow, and only concurrency exposes it. */
        if (memcmp(L->buf, L->buf + L->msg, L->msg)) { L->failed = 2; break; }
    }
    L->ns = now_ns() - t0;
    return NULL;
}

static int setup_roce(struct ibv_context *ctx, union ibv_gid *gid, uint8_t *sgid)
{
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
        return -1;
    memset(&roce.local_gid, 0, sizeof(roce.local_gid));
    roce.local_gid.raw[10] = roce.local_gid.raw[11] = 0xff;
    memcpy(roce.local_gid.raw + 12, ipv4, sizeof(ipv4));
    roce.l3_type = 0;
    memcpy(roce.peer_mac, roce.local_mac, sizeof(roce.peer_mac));
    if (ibv_mlx5_configure_roce(ctx, &roce)) return -1;
    *gid = roce.local_gid;
    struct ibv_gid_entry entries[64] = {};
    uint32_t n = 0, tableSize = 0;
    if (ibv_query_gid_table(ctx, 1, entries, 64, &n, &tableSize)) return -1;
    for (uint32_t i = 0; i < n; i++)
        if (entries[i].gid_type == IBV_GID_TYPE_ROCE_V2 &&
            !memcmp(entries[i].gid.raw, gid->raw, 16)) {
            *sgid = (uint8_t)entries[i].gid_index; return 0;
        }
    return -1;
}

static int to_rts(struct ibv_qp *qp, union ibv_gid gid, uint8_t sgid)
{
    uint32_t psn = ((uint32_t)getpid() * 2654435761u + qp->qp_num) & 0xffffff;
    struct ibv_qp_attr a = { .qp_state = IBV_QPS_INIT, .pkey_index = 0,
        .port_num = 1,
        .qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ };
    if (ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                      IBV_QP_ACCESS_FLAGS)) return -1;
    a = (struct ibv_qp_attr){ .qp_state = IBV_QPS_RTR,
        .path_mtu = IBV_MTU_1024, .dest_qp_num = qp->qp_num, .rq_psn = psn,
        .max_dest_rd_atomic = 4, .min_rnr_timer = 12,
        .ah_attr = { .is_global = 1, .port_num = 1, .sl = 0,
        .grh = { .dgid = gid, .sgid_index = sgid, .hop_limit = 1 } } };
    if (ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                      IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                      IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER))
        return -1;
    a = (struct ibv_qp_attr){ .qp_state = IBV_QPS_RTS, .sq_psn = psn,
        .timeout = 14, .retry_cnt = 7, .rnr_retry = 7, .max_rd_atomic = 4 };
    return ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT |
                         IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                         IBV_QP_MAX_QP_RD_ATOMIC) ? -1 : 0;
}

int main(int argc, char **argv)
{
    const char *levels = argc > 1 ? argv[1] : "1 2 4 8";
    size_t msg = argc > 2 ? (size_t)strtoull(argv[2], NULL, 0) : 64;
    long iters = argc > 3 ? strtol(argv[3], NULL, 0) : 20000;
    if (msg < 8 || msg > (1u << 20)) { fprintf(stderr, "msg out of range\n"); return 2; }

    struct ibv_device **devs = ibv_get_device_list(NULL);
    if (!devs || !devs[0]) { fprintf(stderr, "no devices\n"); return 1; }
    struct ibv_context *ctx = ibv_open_device(devs[0]);
    if (!ctx) { perror("ibv_open_device"); return 1; }
    union ibv_gid gid; uint8_t sgid = 0;
    if (setup_roce(ctx, &gid, &sgid)) { fprintf(stderr, "RoCE setup failed\n"); return 1; }

    struct ibv_mlx5_posting_caps pc = {};
    double line = 0.0;
    if (ibv_mlx5_query_posting_caps(ctx, &pc) == 0) {
        printf("QP_SCALE: bf_regs_per_uar=%u max_qp_per_client=%u msg=%zu "
               "iters/QP=%ld\n", pc.bf_regs_per_uar, pc.max_qp, msg, iters);
        line = ibv_mlx5_pcie_line_gbps(pc.pcie_link_speed, pc.pcie_link_width);
        if (line > 0.0)
            printf("LINE: PCIe gen%u x%u, %.1f Gbit/s per direction\n",
                   pc.pcie_link_speed, pc.pcie_link_width, line);
    }
    /* eta is the achieved payload rate as a fraction of the negotiated line.
     * On a small message it will be tiny and that is the point: this gate is
     * about operations, not bandwidth, and printing the fraction keeps anyone
     * from reading its gigabits as a bandwidth result. */
    printf("%5s %14s %14s %10s %10s\n",
           "QPs", "aggregate op/s", "per-QP op/s", "scaling", "of line");

    double base = 0.0;
    const char *p = levels;
    while (*p) {
        char *end = NULL;
        long lanes = strtol(p, &end, 10);
        if (end == p) break;
        p = end;
        while (*p == ' ') p++;
        if (lanes < 1 || lanes > MAX_LANES) continue;

        struct lane L[MAX_LANES] = {};
        pthread_t th[MAX_LANES];
        struct barrier start = { .n = (unsigned)lanes };
        atomic_init(&start.arrived, 0u);
        int built = 0;
        for (long i = 0; i < lanes; i++) {
            L[i].msg = msg; L[i].iters = iters; L[i].start = &start;
            L[i].pd = ibv_alloc_pd(ctx);
            L[i].cq = ibv_create_cq(ctx, 64, NULL, NULL, 0);
            L[i].buf = calloc(1, 2 * msg);
            if (!L[i].pd || !L[i].cq || !L[i].buf) break;
            L[i].mr = ibv_reg_mr(L[i].pd, L[i].buf, 2 * msg,
                IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
            struct ibv_qp_init_attr ia = { .send_cq = L[i].cq, .recv_cq = L[i].cq,
                .qp_type = IBV_QPT_RC, .cap = { .max_send_wr = 64,
                .max_recv_wr = 4, .max_send_sge = 1, .max_recv_sge = 1 } };
            L[i].qp = L[i].mr ? ibv_create_qp(L[i].pd, &ia) : NULL;
            if (!L[i].qp || to_rts(L[i].qp, gid, sgid)) break;
            for (size_t k = 0; k < msg; k++) L[i].buf[k] = (uint8_t)(k * 7 + 1);
            built++;
        }
        if (built != lanes) {
            printf("%5ld  could not build %ld lanes (built %d) — quota or "
                   "resource limit\n", lanes, lanes, built);
        } else {
            uint64_t t0 = now_ns();
            for (long i = 0; i < lanes; i++)
                pthread_create(&th[i], NULL, lane_run, &L[i]);
            for (long i = 0; i < lanes; i++) pthread_join(th[i], NULL);
            uint64_t wall = now_ns() - t0;
            int bad = 0;
            for (long i = 0; i < lanes; i++) if (L[i].failed) bad = L[i].failed;
            if (bad == 2) printf("%5ld  PAYLOAD MISMATCH under concurrency\n", lanes);
            else if (bad) printf("%5ld  a lane failed to post or complete\n", lanes);
            else {
                double ops = (double)lanes * (double)iters;
                double aggregate = ops / ((double)wall / 1e9);
                if (base == 0.0) base = aggregate;
                double gbps = aggregate * (double)msg * 8.0 / 1e9;
                printf("%5ld %14.0f %14.0f %9.2fx %9s\n", lanes, aggregate,
                       aggregate / (double)lanes, aggregate / base,
                       ({ static char buf[16];
                          if (line > 0.0) snprintf(buf, sizeof(buf), "%.2f%%",
                                                   100.0 * gbps / line);
                          else snprintf(buf, sizeof(buf), "-");
                          buf; }));
            }
        }
        for (long i = 0; i < lanes; i++) {
            if (L[i].qp) ibv_destroy_qp(L[i].qp);
            if (L[i].mr) ibv_dereg_mr(L[i].mr);
            if (L[i].cq) ibv_destroy_cq(L[i].cq);
            if (L[i].pd) ibv_dealloc_pd(L[i].pd);
            free(L[i].buf);
        }
    }
    ibv_close_device(ctx);
    ibv_free_device_list(devs);
    return 0;
}
