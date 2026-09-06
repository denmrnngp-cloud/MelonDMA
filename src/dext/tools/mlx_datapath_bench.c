/* mlx_datapath_bench — datapath baseline over versioned telemetry.
 *
 * One QP in RTS with a self destination, one registered buffer, N signaled
 * loopback RDMA_WRITEs (post one, poll one). Snapshots the per-client
 * QueryPerf and provider telemetry counters before/after so kernel crossings,
 * direct SQ/CQ work, fallback reasons and wall latency are visible per message.
 *
 * Usage: mlx_datapath_bench [msg_size] [iterations]
 * Env:   direct SQ/CQ is the capability-driven default.  Use
 *        MELONDMA_DIRECT_UAR=0 / MELONDMA_DIRECT_CQ=0 to force the diagnostic
 *        kernel fallback.
 */
#include <infiniband/verbs.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

#define DELTA(f) ((long long)(after.f - before.f))
#define TDELTA(f) ((long long)(telemetry_after.f - telemetry_before.f))

int main(int argc, char **argv)
{
    size_t msg = argc > 1 ? (size_t)strtoull(argv[1], NULL, 0) : 4096;
    long iters = argc > 2 ? strtol(argv[2], NULL, 0) : 1000;
    if (msg < 8 || msg > (32u << 20)) { fprintf(stderr, "msg out of range\n"); return 2; }
    if (iters < 1) iters = 1;
    int rc = 1;

    struct ibv_device **devs = ibv_get_device_list(NULL);
    if (!devs || !devs[0]) { fprintf(stderr, "no devices\n"); return 1; }
    struct ibv_context *ctx = ibv_open_device(devs[0]);
    if (!ctx) { perror("ibv_open_device"); goto out_devs; }

    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    struct ibv_comp_channel *ch = ibv_create_comp_channel(ctx);
    struct ibv_cq *cq = ch ? ibv_create_cq(ctx, 64, NULL, ch, 0) : NULL;
    if (!pd || !cq) { fprintf(stderr, "pd/cq failed\n"); goto out_cq; }

    size_t size = 2 * msg;
    uint8_t *buf = malloc(size);
    if (!buf) { perror("malloc"); goto out_cq; }
    memset(buf, 0, size);
    struct ibv_mr *mr = ibv_reg_mr(pd, buf, size,
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
    if (!mr) { fprintf(stderr, "ibv_reg_mr failed\n"); goto out_buf; }

    struct ibv_qp_init_attr init = { .send_cq = cq, .recv_cq = cq,
        .qp_type = IBV_QPT_RC, .cap = { .max_send_wr = 128, .max_recv_wr = 8,
        .max_send_sge = 1, .max_recv_sge = 1, .max_inline_data = 0 } };
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
    if (ibv_mlx5_configure_roce(ctx, &roce)) { fprintf(stderr, "configure_roce failed\n"); goto out_qp; }

    union ibv_gid gid = roce.local_gid;
    uint8_t sgid = 0;
    struct ibv_gid_entry entries[64] = {};
    uint32_t n = 0, tableSize = 0;
    if (ibv_query_gid_table(ctx, 1, entries, 64, &n, &tableSize)) { fprintf(stderr, "gid table failed\n"); goto out_qp; }
    int found = 0;
    for (uint32_t i = 0; i < n; i++)
        if (entries[i].gid_type == IBV_GID_TYPE_ROCE_V2 &&
            !memcmp(entries[i].gid.raw, gid.raw, 16)) { sgid = (uint8_t)entries[i].gid_index; found = 1; break; }
    if (!found) { fprintf(stderr, "no RoCEv2 GID\n"); goto out_qp; }

    uint32_t psn = ((uint32_t)getpid() * 2654435761u) & 0xffffff;
    struct ibv_qp_attr attr = { .qp_state = IBV_QPS_INIT, .pkey_index = 0,
        .port_num = 1, .qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ };
    if (ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
        fprintf(stderr, "RST->INIT failed\n"); goto out_qp; }
    attr = (struct ibv_qp_attr){ .qp_state = IBV_QPS_RTR, .path_mtu = IBV_MTU_1024,
        .dest_qp_num = qp->qp_num, .rq_psn = psn, .max_dest_rd_atomic = 4,
        .min_rnr_timer = 12, .ah_attr = { .is_global = 1, .port_num = 1, .sl = 0,
        .grh = { .dgid = gid, .sgid_index = sgid, .hop_limit = 1 } } };
    if (ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                      IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER)) {
        fprintf(stderr, "INIT->RTR failed\n"); goto out_qp; }
    attr = (struct ibv_qp_attr){ .qp_state = IBV_QPS_RTS, .sq_psn = psn, .timeout = 14,
        .retry_cnt = 7, .rnr_retry = 7, .max_rd_atomic = 4 };
    if (ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT |
                      IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC)) {
        fprintf(stderr, "RTR->RTS failed\n"); goto out_qp; }

    for (size_t i = 0; i < msg; i++) buf[i] = (uint8_t)(i * 7 + 1);
    struct ibv_mlx5_perf before = {}, after = {};
    struct ibv_mlx5_telemetry telemetry_before = {}, telemetry_after = {};
    if (ibv_mlx5_query_perf(ctx, &before)) { fprintf(stderr, "perf before failed\n"); goto out_qp; }
    if (ibv_mlx5_query_telemetry(ctx, &telemetry_before)) {
        fprintf(stderr, "telemetry before failed\n"); goto out_qp;
    }

    double t0 = now_s();
    for (long i = 0; i < iters; i++) {
        struct ibv_sge sge = { .addr = (uintptr_t)buf, .length = (uint32_t)msg, .lkey = mr->lkey };
        struct ibv_send_wr wr = { .wr_id = (uint64_t)i + 1, .sg_list = &sge, .num_sge = 1,
            .opcode = IBV_WR_RDMA_WRITE, .send_flags = IBV_SEND_SIGNALED };
        wr.wr.rdma.remote_addr = (uintptr_t)buf + msg;
        wr.wr.rdma.rkey = mr->rkey;
        struct ibv_send_wr *bad = NULL;
        if (ibv_post_send(qp, &wr, &bad)) { fprintf(stderr, "post_send failed at %ld\n", i); goto out_qp; }
        struct ibv_wc wc = {};
        for (;;) {
            int got = ibv_poll_cq(cq, 1, &wc);
            if (got < 0) { fprintf(stderr, "poll_cq error\n"); goto out_qp; }
            if (got == 1) break;
        }
        if (wc.status != IBV_WC_SUCCESS) { fprintf(stderr, "WC error at %ld: %d\n", i, wc.status); goto out_qp; }
    }
    double dt = now_s() - t0;

    if (ibv_mlx5_query_perf(ctx, &after)) { fprintf(stderr, "perf after failed\n"); goto out_qp; }
    if (ibv_mlx5_query_telemetry(ctx, &telemetry_after)) {
        fprintf(stderr, "telemetry after failed\n"); goto out_qp;
    }

    printf("DATAPATH_BENCH: msg=%zu iters=%ld wall=%.3f s\n", msg, iters, dt);
    printf("  per_msg_wall_us=%.2f\n", dt * 1e6 / (double)iters);
    printf("  external_methods=%lld external_method_ns=%lld\n",
           DELTA(external_methods), DELTA(external_method_ns));
    printf("  post_send=%lld post_recv=%lld poll_cq=%lld arm_cq=%lld\n",
           DELTA(post_send_calls), DELTA(post_recv_calls),
           DELTA(poll_cq_calls), DELTA(arm_cq_calls));
    printf("  sync_fast_path=%lld sync_qp_tails=%lld\n",
           DELTA(sync_fast_path_calls), DELTA(sync_qp_tails_calls));
    printf("  doorbells=%lld cqe=%lld cqe_errors=%lld\n",
           DELTA(doorbells), DELTA(cqe_consumed), DELTA(cqe_errors));
    printf("  mr_registers=%lld mr_deregisters=%lld mr_bytes=%lld copied_bytes=%lld\n",
           DELTA(mr_registers), DELTA(mr_deregisters), DELTA(mr_bytes),
           DELTA(copied_bytes));
    printf("  direct_send_batches=%lld direct_send_wrs=%lld direct_doorbells=%lld "
           "mapped_qps=%llu\n",
           TDELTA(direct_send_batches), TDELTA(direct_send_wrs),
           TDELTA(direct_doorbells),
           (unsigned long long)telemetry_after.mapped_qps);
    printf("  direct_poll_calls=%lld direct_poll_empty=%lld direct_cqes=%lld "
           "cq_consumer_publications=%lld\n",
           TDELTA(direct_poll_calls), TDELTA(direct_poll_empty),
           TDELTA(direct_cqes), TDELTA(cq_consumer_publications));
    printf("  kernel_post_send=%lld kernel_poll_methods=%lld kernel_poll_calls=%lld "
           "kernel_cqes=%lld\n",
           TDELTA(kernel_post_send_calls), TDELTA(kernel_poll_cq_methods),
           TDELTA(kernel_poll_calls), TDELTA(kernel_cqes));
    printf("  fallbacks disabled=%lld cq_unmapped=%lld unknown_qp=%lld "
           "metadata=%lld dext_owned=%lld\n",
           TDELTA(fallback_direct_disabled), TDELTA(fallback_cq_unmapped),
           TDELTA(fallback_unknown_qp), TDELTA(fallback_missing_metadata),
           TDELTA(fallback_dext_owned));

    long long fallback_total = TDELTA(fallback_direct_disabled) +
        TDELTA(fallback_cq_unmapped) + TDELTA(fallback_unknown_qp) +
        TDELTA(fallback_missing_metadata) + TDELTA(fallback_dext_owned);
    if (!telemetry_after.mapped_qps || TDELTA(direct_send_wrs) != iters ||
        TDELTA(direct_doorbells) != iters || TDELTA(direct_cqes) != iters ||
        TDELTA(kernel_post_send_calls) || TDELTA(kernel_poll_cq_methods) ||
        TDELTA(kernel_poll_calls) || fallback_total) {
        fprintf(stderr, "DATAPATH_BENCH FAIL: direct-default telemetry invariant failed\n");
        goto out_qp;
    }
    printf("DATAPATH_BENCH PASS\n");
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
    return rc;
}
