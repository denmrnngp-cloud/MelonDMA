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
    uint16_t port = 18515; uint32_t mtu_bytes = 4096; int opt;
    while ((opt = getopt(argc, argv, "h:p:m:l:a:r:")) != -1) switch (opt) {
    case 'h': host = optarg; break; case 'p': port = (uint16_t)strtoul(optarg, NULL, 0); break;
    case 'm': mtu_bytes = (uint32_t)strtoul(optarg, NULL, 0); break; case 'l': local_ip = optarg; break;
    case 'a': local_mac = optarg; break; case 'r': remote_mac = optarg; break; default: return 2;
    }
    if (!host || !remote_mac || !mtu(mtu_bytes)) return 2;
    int rc = 1, count = 0, fd = -1;
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
    stage = "arm_and_post";
    if (ibv_req_notify_cq(cq, 0) || ibv_post_recv(qp, &recv, &bad_recv) || ibv_post_send(qp, &send, &bad_send)) goto cleanup;
    struct pollfd event_fd = { .fd = channel->fd, .events = POLLIN };
    stage = "wait_event";
    if (poll(&event_fd, 1, 5000) != 1) goto cleanup;
    struct ibv_cq *event_cq = NULL; void *event_context = NULL;
    stage = "get_event";
    if (ibv_get_cq_event(channel, &event_cq, &event_context) || event_cq != cq || event_context != &marker) goto cleanup;
    ibv_ack_cq_events(cq, 1);
    int completions = 0;
    while (completions < 2) { struct ibv_wc wc[2] = {}; int n = ibv_poll_cq(cq, 2, wc); if (n < 0) goto cleanup; for (int i = 0; i < n; i++) if (wc[i].status == IBV_WC_SUCCESS) completions++; else goto cleanup; }
    printf("R5_CQ_EVENT PASS: fd event, CQ/context association, ack and %d CQEs\n", completions);
    rc = 0;
cleanup:
    if (rc) fprintf(stderr, "R5_CQ_EVENT FAIL at %s (errno=%d)\n", stage, errno);
    if (fd >= 0) close(fd); if (qp) ibv_destroy_qp(qp); if (mr) ibv_dereg_mr(mr); if (cq) ibv_destroy_cq(cq); if (channel) ibv_destroy_comp_channel(channel); if (pd) ibv_dealloc_pd(pd);
out:
    if (ctx) ibv_close_device(ctx); if (devices) ibv_free_device_list(devices); return rc;
}
