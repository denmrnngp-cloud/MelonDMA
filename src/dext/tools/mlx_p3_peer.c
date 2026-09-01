/* Linux stock-libverbs peer for the MelonDMA P3 gate (inline SEND + RC atomics).
 *
 * Phases (linear protocol, Mac drives):
 *   1. conn-info handshake (qpn/psn/gid) — same wire format as mlx_imm_peer.c
 *   2. peer publishes an 8-byte REMOTE_ATOMIC MR {addr,rkey,len}
 *   3. peer pre-posts 4 RECV WQEs and waits for Mac's inline SENDs (1/64/256/512)
 *   4. Mac performs FETCH_ADD(+5) then CMP_SWAP(0x105 -> 0x200) on that MR
 *   5. peer verifies final word == 0x200, then SOLICITED-SENDs a marker back
 *      so the Mac can validate solicited_only CQ arming.
 */
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

static const uint32_t inline_sizes[4] = { 1, 64, 256, 512 };

int main(int argc, char **argv) {
    const char *dev = NULL; uint16_t port = 18515; int gid_index = 0, ib_port = 1, opt;
    uint32_t mtu_bytes = 4096;
    while ((opt = getopt(argc, argv, "d:i:g:p:m:")) != -1) switch (opt) {
    case 'd': dev = optarg; break; case 'i': ib_port = atoi(optarg); break;
    case 'g': gid_index = atoi(optarg); break; case 'p': port = (uint16_t)strtoul(optarg, NULL, 0); break;
    case 'm': mtu_bytes = (uint32_t)strtoul(optarg, NULL, 0); break; default: return 2;
    }
    enum ibv_mtu path_mtu = mtu(mtu_bytes);
    if (!dev || !path_mtu) return 2;
    int rc = 1, count = 0, listen_fd = -1, control_fd = -1;
    struct ibv_device **devices = ibv_get_device_list(&count); struct ibv_device *device = NULL;
    for (int i = 0; devices && i < count; i++) if (!strcmp(ibv_get_device_name(devices[i]), dev)) device = devices[i];
    struct ibv_context *ctx = device ? ibv_open_device(device) : NULL;
    struct ibv_pd *pd = ctx ? ibv_alloc_pd(ctx) : NULL;
    struct ibv_cq *cq = ctx ? ibv_create_cq(ctx, 64, NULL, NULL, 0) : NULL;
    volatile uint64_t atomic_buf __attribute__((aligned(8))) = 0x100;
    uint8_t recv_buf[4][512]; memset(recv_buf, 0, sizeof(recv_buf));
    struct ibv_mr *atomic_mr = pd ? ibv_reg_mr(pd, (void *)(uintptr_t)&atomic_buf, 8,
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC) : NULL;
    struct ibv_mr *recv_mr = pd ? ibv_reg_mr(pd, recv_buf, sizeof(recv_buf), IBV_ACCESS_LOCAL_WRITE) : NULL;
    struct ibv_qp_init_attr init = { .send_cq = cq, .recv_cq = cq, .qp_type = IBV_QPT_RC,
        .cap = { .max_send_wr = 8, .max_recv_wr = 8, .max_send_sge = 1, .max_recv_sge = 1 } };
    struct ibv_qp *qp = pd && cq ? ibv_create_qp(pd, &init) : NULL;
    if (!ctx || !pd || !cq || !atomic_mr || !recv_mr || !qp) { fprintf(stderr, "p3 peer resources unavailable\n"); goto out; }
    struct ibv_qp_attr attr = { .qp_state = IBV_QPS_INIT, .pkey_index = 0,
        .port_num = (uint8_t)ib_port, .qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC };
    if (ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                      IBV_QP_ACCESS_FLAGS)) goto out;
    /* Pre-post the 4 inline-phase RECVs. */
    for (int i = 0; i < 4; i++) {
        struct ibv_sge sge = { .addr = (uintptr_t)recv_buf[i], .length = 512, .lkey = recv_mr->lkey };
        struct ibv_recv_wr recv = { .wr_id = 1000 + i, .sg_list = &sge, .num_sge = 1 }, *bad = NULL;
        if (ibv_post_recv(qp, &recv, &bad)) { fprintf(stderr, "peer post_recv %d failed\n", i); goto out; }
    }
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
        .max_dest_rd_atomic = 4, .min_rnr_timer = 12, .ah_attr = { .is_global = 1, .port_num = (uint8_t)ib_port,
        .grh = { .dgid = remote.gid, .sgid_index = (uint8_t)gid_index, .hop_limit = 1 } } };
    if (ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER)) goto out;
    attr = (struct ibv_qp_attr){ .qp_state = IBV_QPS_RTS, .sq_psn = local.psn, .timeout = 14, .retry_cnt = 7, .rnr_retry = 7, .max_rd_atomic = 4 };
    if (ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC)) goto out;
    gid_wire(&local.gid, wire); snprintf(msg, sizeof(msg), "%04x:%06x:%06x:%s", 0, local.qpn, local.psn, wire);
    if (full_write(control_fd, msg, sizeof(msg))) goto out;
    struct remote_memory_wire memory = { .address_be = host_to_be64((uint64_t)(uintptr_t)&atomic_buf),
        .rkey_be = htonl(atomic_mr->rkey), .length_be = htonl(8) };
    if (full_write(control_fd, &memory, sizeof(memory)) || full_write(control_fd, "ready", 5)) goto out;

    /* Phase 3: verify the 4 inline SENDs (RC in-order: 1, 64, 256, 512). */
    for (int i = 0; i < 4; i++) {
        struct ibv_wc wc = {}; for (;;) { int n = ibv_poll_cq(cq, 1, &wc); if (n < 0) goto out; if (n == 1) break; }
        uint32_t want = inline_sizes[i];
        if (wc.status != IBV_WC_SUCCESS || wc.opcode != IBV_WC_RECV || wc.byte_len != want) {
            fprintf(stderr, "inline recv %d: status=%d opcode=%d len=%u want=%u\n", i, wc.status, wc.opcode, wc.byte_len, want); goto out;
        }
        for (uint32_t b = 0; b < want; b++)
            if (recv_buf[i][b] != (uint8_t)(want ^ b)) { fprintf(stderr, "inline recv %d payload[%u] mismatch\n", i, b); goto out; }
    }
    /* Phase 4: wait for the Mac's atomic sequence to finish, then verify. */
    if (full_read(control_fd, msg, sizeof("atomic_done") - 1)) goto out;
    if (atomic_buf != 0x200) { fprintf(stderr, "atomic_buf=0x%lx want 0x200\n", (unsigned long)atomic_buf); goto out; }
    /* Phase 5: SOLICITED SEND back so the Mac can validate solicited_only arming. */
    {
        /* The marker must live in a registered region: recv_mr covers recv_buf
         * only, so a stack buffer with recv_mr->lkey would fault the DMA and
         * silently drop the solicited SEND. Reuse recv_buf[0] (phase 3 is
         * already verified); the Mac checks RECV+status, not the bytes. */
        memcpy(recv_buf[0], "SOLICITED-MARKER", 16);
        struct ibv_sge sge = { .addr = (uintptr_t)recv_buf[0], .length = 16, .lkey = recv_mr->lkey };
        struct ibv_send_wr send = { .wr_id = 2, .sg_list = &sge, .num_sge = 1,
            .opcode = IBV_WR_SEND, .send_flags = IBV_SEND_SIGNALED | IBV_SEND_SOLICITED }, *bad = NULL;
        if (ibv_post_send(qp, &send, &bad)) goto out;
        struct ibv_wc wc = {}; for (;;) { int n = ibv_poll_cq(cq, 1, &wc); if (n < 0) goto out; if (n == 1) break; }
        if (wc.status != IBV_WC_SUCCESS || wc.wr_id != 2) goto out;
    }
    if (full_write(control_fd, "OK", 3)) goto out;
    printf("MLX_P3_PEER PASS: inline 1/64/256/512 + FETCH_ADD/CMP_SWAP verified, atomic word 0x200\n");
    rc = 0;
out:
    if (control_fd >= 0) close(control_fd);
    if (listen_fd >= 0) close(listen_fd);
    if (qp) ibv_destroy_qp(qp);
    if (atomic_mr) ibv_dereg_mr(atomic_mr);
    if (recv_mr) ibv_dereg_mr(recv_mr);
    if (cq) ibv_destroy_cq(cq);
    if (pd) ibv_dealloc_pd(pd);
    if (ctx) ibv_close_device(ctx);
    if (devices) ibv_free_device_list(devices);
    return rc;
}
