/* Linux stock-libverbs peer for MelonDMA immediate-data gates. */
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <infiniband/verbs.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct destination { uint32_t qpn, psn; union ibv_gid gid; };
struct remote_memory_wire { uint64_t address_be; uint32_t rkey_be, length_be; };

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
static uint64_t host_to_be64(uint64_t value) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap64(value);
#else
    return value;
#endif
}
static enum ibv_mtu mtu(uint32_t n) {
    switch (n) { case 256: return IBV_MTU_256; case 512: return IBV_MTU_512; case 1024: return IBV_MTU_1024; case 2048: return IBV_MTU_2048; case 4096: return IBV_MTU_4096; default: return 0; }
}

int main(int argc, char **argv) {
    const char *dev = NULL; uint16_t port = 18515; int gid_index = 0, ib_port = 1, opt, write_mode = 0, normal_mode = 0;
    uint32_t mtu_bytes = 4096, expected_imm = 0x12345678;
    while ((opt = getopt(argc, argv, "NWd:i:g:p:m:x:")) != -1) switch (opt) {
    case 'N': normal_mode = 1; break;
    case 'W': write_mode = 1; break;
    case 'd': dev = optarg; break; case 'i': ib_port = atoi(optarg); break;
    case 'g': gid_index = atoi(optarg); break; case 'p': port = (uint16_t)strtoul(optarg, NULL, 0); break;
    case 'm': mtu_bytes = (uint32_t)strtoul(optarg, NULL, 0); break; case 'x': expected_imm = (uint32_t)strtoul(optarg, NULL, 0); break;
    default: return 2;
    }
    enum ibv_mtu path_mtu = mtu(mtu_bytes);
    if (!dev || !path_mtu) return 2;
    int rc = 1, count = 0, listen_fd = -1, control_fd = -1;
    struct ibv_device **devices = ibv_get_device_list(&count); struct ibv_device *device = NULL;
    for (int i = 0; devices && i < count; i++) if (!strcmp(ibv_get_device_name(devices[i]), dev)) device = devices[i];
    struct ibv_context *ctx = device ? ibv_open_device(device) : NULL;
    struct ibv_pd *pd = ctx ? ibv_alloc_pd(ctx) : NULL;
    struct ibv_cq *cq = ctx ? ibv_create_cq(ctx, 16, NULL, NULL, 0) : NULL;
    uint8_t buffer[64] = {}; struct ibv_mr *mr = pd ? ibv_reg_mr(pd, buffer, sizeof(buffer), IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE) : NULL;
    struct ibv_qp_init_attr init = { .send_cq = cq, .recv_cq = cq, .qp_type = IBV_QPT_RC,
        .cap = { .max_send_wr = 4, .max_recv_wr = 4, .max_send_sge = 1, .max_recv_sge = 1 } };
    struct ibv_qp *qp = pd && cq ? ibv_create_qp(pd, &init) : NULL;
    if (!ctx || !pd || !cq || !mr || !qp) { fprintf(stderr, "peer resources unavailable\n"); goto out; }
    struct ibv_qp_attr attr = { .qp_state = IBV_QPS_INIT, .pkey_index = 0,
        .port_num = (uint8_t)ib_port, .qp_access_flags = IBV_ACCESS_REMOTE_WRITE };
    if (ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                      IBV_QP_ACCESS_FLAGS)) goto out;
    struct ibv_sge sge = { .addr = (uintptr_t)buffer, .length = sizeof(buffer), .lkey = mr->lkey };
    struct ibv_recv_wr recv = { .wr_id = 1, .sg_list = &sge, .num_sge = 1 }, *bad_recv = NULL;
    if (ibv_post_recv(qp, &recv, &bad_recv)) goto out;
    listen_fd = socket(AF_INET, SOCK_STREAM, 0); int one = 1; setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (listen_fd < 0 || bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) || listen(listen_fd, 1)) goto out;
    control_fd = accept(listen_fd, NULL, NULL); close(listen_fd); listen_fd = -1; if (control_fd < 0) goto out;
    char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"] = {}, wire[33] = {};
    if (full_read(control_fd, msg, sizeof(msg))) goto out;
    struct destination remote = {}; unsigned lid;
    if (sscanf(msg, "%x:%x:%x:%32s", &lid, &remote.qpn, &remote.psn, wire) != 4 || wire_gid(wire, &remote.gid)) goto out;
    struct destination local = { .qpn = qp->qp_num, .psn = ((uint32_t)getpid() * 2654435761u) & 0xffffff };
    if (ibv_query_gid(ctx, ib_port, gid_index, &local.gid)) goto out;
    attr = (struct ibv_qp_attr){ .qp_state = IBV_QPS_RTR, .path_mtu = path_mtu, .dest_qp_num = remote.qpn, .rq_psn = remote.psn,
        .max_dest_rd_atomic = 1, .min_rnr_timer = 12, .ah_attr = { .is_global = 1, .port_num = (uint8_t)ib_port,
        .grh = { .dgid = remote.gid, .sgid_index = (uint8_t)gid_index, .hop_limit = 1 } } };
    if (ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER)) goto out;
    attr = (struct ibv_qp_attr){ .qp_state = IBV_QPS_RTS, .sq_psn = local.psn, .timeout = 14, .retry_cnt = 7, .rnr_retry = 7, .max_rd_atomic = 1 };
    if (ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC)) goto out;
    gid_wire(&local.gid, wire); snprintf(msg, sizeof(msg), "%04x:%06x:%06x:%s", 0, local.qpn, local.psn, wire);
    if (full_write(control_fd, msg, sizeof(msg)) || full_read(control_fd, msg, sizeof("done"))) goto out;
    if (write_mode) {
        struct remote_memory_wire memory = { .address_be = host_to_be64((uint64_t)(uintptr_t)buffer),
            .rkey_be = htonl(mr->rkey), .length_be = htonl(sizeof(buffer)) };
        if (full_write(control_fd, &memory, sizeof(memory))) goto out;
    }
    struct ibv_wc wc = {}; for (;;) { int n = ibv_poll_cq(cq, 1, &wc); if (n < 0) goto out; if (n == 1) break; }
    if (wc.status != IBV_WC_SUCCESS ||
        (wc.opcode != IBV_WC_RECV && wc.opcode != IBV_WC_RECV_RDMA_WITH_IMM) ||
        (!normal_mode && (!(wc.wc_flags & IBV_WC_WITH_IMM) ||
                          ntohl(wc.imm_data) != expected_imm))) {
        fprintf(stderr, "IMM peer failure: status=%d opcode=%d flags=0x%x imm=0x%x\n", wc.status, wc.opcode, wc.wc_flags, ntohl(wc.imm_data)); goto out;
    }
    if (write_mode && buffer[0] != 0x7b) { fprintf(stderr, "WRITE_WITH_IMM payload mismatch\n"); goto out; }
    struct ibv_send_wr send = { .wr_id = 2, .sg_list = &sge, .num_sge = 1,
        .opcode = normal_mode ? IBV_WR_SEND : IBV_WR_SEND_WITH_IMM,
        .send_flags = IBV_SEND_SIGNALED, .imm_data = htonl(expected_imm) },
        *bad_send = NULL;
    if (ibv_post_send(qp, &send, &bad_send)) goto out;
    for (;;) { int n = ibv_poll_cq(cq, 1, &wc); if (n < 0) goto out; if (n == 1) break; }
    if (wc.status != IBV_WC_SUCCESS || wc.wr_id != 2) goto out;
    printf("MLX_IMM_PEER PASS: %s value=0x%08x\n",
           normal_mode ? "SEND/RECV" :
           write_mode ? "RDMA_WRITE_WITH_IMM" : "bidirectional SEND_WITH_IMM", expected_imm); rc = 0;
out:
    if (control_fd >= 0) close(control_fd);
    if (listen_fd >= 0) close(listen_fd);
    if (qp) ibv_destroy_qp(qp);
    if (mr) ibv_dereg_mr(mr);
    if (cq) ibv_destroy_cq(cq);
    if (pd) ibv_dealloc_pd(pd);
    if (ctx) ibv_close_device(ctx);
    if (devices) ibv_free_device_list(devices);
    return rc;
}
