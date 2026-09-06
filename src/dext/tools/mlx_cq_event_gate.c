/* Live completion-channel gate for the MelonDMA ibverbs compatibility API. */
#include <infiniband/verbs.h>

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netdb.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

struct destination { uint32_t qpn, psn; union ibv_gid gid; };

static int full_read(int fd, void *buf, size_t len) {
    uint8_t *p = buf;
    while (len) { ssize_t n = read(fd, p, len); if (n < 0 && errno == EINTR) continue; if (n <= 0) return -1; p += n; len -= (size_t)n; }
    return 0;
}
static int full_write(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    while (len) { ssize_t n = write(fd, p, len); if (n < 0 && errno == EINTR) continue; if (n <= 0) return -1; p += n; len -= (size_t)n; }
    return 0;
}
static void gid_wire(const union ibv_gid *gid, char out[33]) {
    for (int i = 0; i < 16; i++) snprintf(out + i * 2, 3, "%02x", gid->raw[i]);
}
static int wire_gid(const char *in, union ibv_gid *gid) {
    for (int i = 0; i < 16; i++) { unsigned v; if (sscanf(in + i * 2, "%2x", &v) != 1) return -1; gid->raw[i] = (uint8_t)v; }
    return 0;
}
static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}
static enum ibv_mtu mtu(uint32_t n) {
    switch (n) { case 256: return IBV_MTU_256; case 512: return IBV_MTU_512; case 1024: return IBV_MTU_1024; case 2048: return IBV_MTU_2048; case 4096: return IBV_MTU_4096; default: return 0; }
}
static int connect_peer(const char *host, uint16_t port) {
    char text[16]; snprintf(text, sizeof(text), "%u", port);
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *result = NULL;
    if (getaddrinfo(host, text, &hints, &result)) return -1;
    int fd = -1;
    for (int attempt = 0; attempt < 50 && fd < 0; attempt++) {
        for (struct addrinfo *it = result; it; it = it->ai_next) {
            fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
            if (fd >= 0 && connect(fd, it->ai_addr, it->ai_addrlen) == 0) break;
            if (fd >= 0) close(fd); fd = -1;
        }
        if (fd < 0) usleep(100000);
    }
    freeaddrinfo(result); return fd;
}

int main(int argc, char **argv) {
    const char *host = NULL, *local_ip = "192.168.200.1", *local_mac = "98:03:9b:80:6a:94", *remote_mac = NULL;
    uint16_t port = 18515; uint32_t mtu_bytes = 4096; int opt, iterations = 1;
    while ((opt = getopt(argc, argv, "h:p:m:l:a:r:n:")) != -1) switch (opt) {
    case 'h': host = optarg; break; case 'p': port = (uint16_t)strtoul(optarg, NULL, 0); break;
    case 'm': mtu_bytes = (uint32_t)strtoul(optarg, NULL, 0); break; case 'l': local_ip = optarg; break;
    case 'a': local_mac = optarg; break; case 'r': remote_mac = optarg; break; case 'n': iterations = atoi(optarg); break; default: return 2;
    }
    if (iterations < 1 || iterations > 100000) iterations = 1;
    if (!host || !remote_mac || !mtu(mtu_bytes)) return 2;
    int rc = 1, count = 0, fd = -1;
    uint64_t *lat_ns = NULL;
    const char *stage = "enumerate";
    struct ibv_device **devices = ibv_get_device_list(&count);
    struct ibv_context *ctx = devices && count ? ibv_open_device(devices[0]) : NULL;
    struct ibv_mlx5_roce_config config = { .hop_limit = 1 };
    if (!ctx || inet_pton(AF_INET, local_ip, config.local_gid.raw + 12) != 1 ||
        sscanf(local_mac, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &config.local_mac[0], &config.local_mac[1], &config.local_mac[2], &config.local_mac[3], &config.local_mac[4], &config.local_mac[5]) != 6 ||
        sscanf(remote_mac, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &config.peer_mac[0], &config.peer_mac[1], &config.peer_mac[2], &config.peer_mac[3], &config.peer_mac[4], &config.peer_mac[5]) != 6) goto out;
    config.local_gid.raw[10] = 0xff; config.local_gid.raw[11] = 0xff;
    stage = "configure_roce";
    if (ibv_mlx5_configure_roce(ctx, &config)) goto out;
    stage = "alloc_pd";
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    stage = "create_channel";
    struct ibv_comp_channel *channel = ibv_create_comp_channel(ctx);
    int marker = 7;
    stage = "create_cq";
    struct ibv_cq *cq = channel ? ibv_create_cq(ctx, 16, &marker, channel, 0) : NULL;
    uint8_t buffer[64]; memset(buffer, 0x7b, sizeof(buffer));
    struct ibv_mr *mr = pd ? ibv_reg_mr(pd, buffer, sizeof(buffer), IBV_ACCESS_LOCAL_WRITE) : NULL;
    struct ibv_qp_init_attr init = { .send_cq = cq, .recv_cq = cq, .qp_type = IBV_QPT_RC,
        .cap = { .max_send_wr = 4, .max_recv_wr = 4, .max_send_sge = 1, .max_recv_sge = 1 } };
    stage = "create_qp";
    struct ibv_qp *qp = pd && cq ? ibv_create_qp(pd, &init) : NULL;
    if (!pd || !channel || !cq || !mr || !qp) goto cleanup;
    struct ibv_qp_attr attr = { .qp_state = IBV_QPS_INIT, .pkey_index = 0, .port_num = 1 };
    stage = "reset_to_init";
    if (ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) goto cleanup;
    stage = "connect_peer";
    fd = connect_peer(host, port); if (fd < 0) goto cleanup;
    struct destination local = { .qpn = qp->qp_num, .psn = ((uint32_t)getpid() * 2654435761u) & 0xffffff }, remote = {};
    stage = "query_gid";
    if (ibv_query_gid(ctx, 1, 0, &local.gid)) goto cleanup;
    char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"] = {}, wire[33] = {};
    gid_wire(&local.gid, wire); snprintf(msg, sizeof(msg), "%04x:%06x:%06x:%s", 0, local.qpn, local.psn, wire);
    stage = "destination_exchange";
    if (full_write(fd, msg, sizeof(msg)) || full_read(fd, msg, sizeof(msg))) goto cleanup;
    unsigned lid; if (sscanf(msg, "%x:%x:%x:%32s", &lid, &remote.qpn, &remote.psn, wire) != 4 || wire_gid(wire, &remote.gid)) goto cleanup;
    attr = (struct ibv_qp_attr){ .qp_state = IBV_QPS_RTR, .path_mtu = mtu(mtu_bytes), .dest_qp_num = remote.qpn, .rq_psn = remote.psn,
        .max_dest_rd_atomic = 1, .min_rnr_timer = 12, .ah_attr = { .is_global = 1, .port_num = 1,
        .grh = { .dgid = remote.gid, .sgid_index = 0, .hop_limit = 1 } } };
    stage = "init_to_rtr";
    if (ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER)) goto cleanup;
    attr = (struct ibv_qp_attr){ .qp_state = IBV_QPS_RTS, .sq_psn = local.psn, .timeout = 14, .retry_cnt = 7, .rnr_retry = 7, .max_rd_atomic = 1 };
    stage = "rtr_to_rts";
    if (ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC) || full_write(fd, "done", sizeof("done"))) goto cleanup;
    struct ibv_sge sge = { .addr = (uintptr_t)buffer, .length = sizeof(buffer), .lkey = mr->lkey };
    struct ibv_recv_wr recv = { .wr_id = 1, .sg_list = &sge, .num_sge = 1 }, *bad_recv = NULL;
    struct ibv_send_wr send = { .wr_id = 2, .sg_list = &sge, .num_sge = 1, .opcode = IBV_WR_SEND, .send_flags = IBV_SEND_SIGNALED }, *bad_send = NULL;
    struct pollfd event_fd = { .fd = channel->fd, .events = POLLIN };
    struct ibv_cq *event_cq = NULL; void *event_context = NULL;
    lat_ns = calloc((size_t)iterations, sizeof(*lat_ns));
    if (!lat_ns) goto cleanup;
    int completions = 0;
    struct ibv_mlx5_runtime runtime_before = {}, runtime_after = {};
    (void)ibv_mlx5_query_runtime(ctx, &runtime_before, sizeof(runtime_before));
    for (int iter = 0; iter < iterations; iter++) {
        stage = "arm_and_post";
        if (ibv_req_notify_cq(cq, 0) || ibv_post_recv(qp, &recv, &bad_recv) || ibv_post_send(qp, &send, &bad_send)) goto cleanup;
        struct timespec t0, t1;
        stage = "wait_event";
        clock_gettime(CLOCK_MONOTONIC, &t0);
        if (poll(&event_fd, 1, 5000) != 1) goto cleanup;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        lat_ns[iter] = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ull + (uint64_t)(t1.tv_nsec - t0.tv_nsec);
        stage = "get_event";
        if (ibv_get_cq_event(channel, &event_cq, &event_context) || event_cq != cq || event_context != &marker) goto cleanup;
        ibv_ack_cq_events(cq, 1);
        int got = 0;
        while (got < 2) { struct ibv_wc wc[2] = {}; int n = ibv_poll_cq(cq, 2, wc); if (n < 0) goto cleanup; for (int i = 0; i < n; i++) if (wc[i].status == IBV_WC_SUCCESS) got++; else goto cleanup; }
        completions += got;
    }
    qsort(lat_ns, (size_t)iterations, sizeof(*lat_ns), cmp_u64);
    /* Legacy events include IRQ, timer and control-plane EQ drains. */
    if (ibv_mlx5_query_runtime(ctx, &runtime_after, sizeof(runtime_after)) == 0)
        printf("R5_CQ_EVENT SOURCES: irq_eqes=%llu timer_eqes=%llu irq_proven=%u event_ready=%u\n",
            (unsigned long long)(runtime_after.irq_completion_eqes - runtime_before.irq_completion_eqes),
            (unsigned long long)(runtime_after.timer_completion_eqes - runtime_before.timer_completion_eqes),
            runtime_after.completion_irq_proven, runtime_after.completion_eq_ready);
    struct ibv_mlx5_telemetry tele = {};
    if (ibv_mlx5_query_telemetry(ctx, &tele) == 0)
        printf("R5_CQ_EVENT DRIVER_EVENTS: cq_events=%llu cq_event_wakeups=%llu "
               "hw_waits=%llu hw_wakeups=%llu poll_wakeups=%llu lost=%llu\n",
               (unsigned long long)tele.driver_cq_events,
               (unsigned long long)tele.driver_cq_event_wakeups,
               (unsigned long long)tele.completion_hw_waits,
               (unsigned long long)tele.completion_hw_wakeups,
               (unsigned long long)tele.completion_poll_wakeups,
               (unsigned long long)tele.completion_lost_events);
    printf("R5_CQ_EVENT PASS: fd event, CQ/context association, ack and %d CQEs\n", completions);
    if (iterations > 1)
        printf("R5_CQ_EVENT WAKEUP_LAT_US: n=%d min=%.1f median=%.1f p95=%.1f p99=%.1f max=%.1f\n",
               iterations, lat_ns[0] / 1000.0, lat_ns[iterations / 2] / 1000.0,
               lat_ns[((size_t)iterations - 1) * 95 / 100] / 1000.0,
               lat_ns[((size_t)iterations - 1) * 99 / 100] / 1000.0,
               lat_ns[iterations - 1] / 1000.0);
    rc = 0;
cleanup:
    if (rc) fprintf(stderr, "R5_CQ_EVENT FAIL at %s (errno=%d)\n", stage, errno);
    if (fd >= 0) close(fd); if (qp) ibv_destroy_qp(qp); if (mr) ibv_dereg_mr(mr); if (cq) ibv_destroy_cq(cq); if (channel) ibv_destroy_comp_channel(channel); if (pd) ibv_dealloc_pd(pd);
    free(lat_ns);
out:
    if (ctx) ibv_close_device(ctx); if (devices) ibv_free_device_list(devices); return rc;
}
