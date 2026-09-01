/* Asymmetric-length echo peer for the notes/50 mixed-size diagnostic.
 *
 * Unlike ibv_rc_pingpong and mlx_pipeline_peer.c (both fixed to one uniform
 * message size for the whole run), this peer always posts a full-capacity
 * receive buffer but echoes back exactly wc.byte_len bytes — matching
 * ggml-rpc's transport.cpp rx ring, which always posts RDMA_CHUNK capacity
 * regardless of the sender's actual message size. Lets one QP carry several
 * small exchanges followed by one large exchange, same as the real
 * llama.cpp model-load traffic pattern, entirely outside llama.cpp.
 *
 * Wire-compatible with mlx_phase2_gate's client-side destination exchange
 * (same LID:QPN:PSN:GID handshake as stock ibv_rc_pingpong).
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

struct destination {
    uint32_t qpn;
    uint32_t psn;
    union ibv_gid gid;
};

static int read_full(int fd, void *buffer, size_t length)
{
    uint8_t *p = buffer;
    while (length) {
        ssize_t n = read(fd, p, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; length -= (size_t)n;
    }
    return 0;
}

static int write_full(int fd, const void *buffer, size_t length)
{
    const uint8_t *p = buffer;
    while (length) {
        ssize_t n = write(fd, p, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; length -= (size_t)n;
    }
    return 0;
}

static void gid_to_wire(const union ibv_gid *gid, char wire[33])
{
    for (unsigned int i = 0; i < 16; i++)
        snprintf(wire + i * 2, 3, "%02x", gid->raw[i]);
}

static int wire_to_gid(const char *wire, union ibv_gid *gid)
{
    for (unsigned int i = 0; i < 16; i++) {
        unsigned int byte = 0;
        if (sscanf(wire + i * 2, "%2x", &byte) != 1) return -1;
        gid->raw[i] = (uint8_t)byte;
    }
    return 0;
}

static int exchange_destination(uint16_t port, struct ibv_qp *qp,
                                int ibPort, int gidIndex, enum ibv_mtu mtu,
                                const struct destination *local,
                                struct destination *remote)
{
    int listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd < 0) return -1;
    int one = 1;
    (void)setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(listenFd, (struct sockaddr *)&address, sizeof(address)) ||
        listen(listenFd, 1)) { perror("listen"); close(listenFd); return -1; }
    int fd = accept(listenFd, NULL, NULL);
    close(listenFd);
    if (fd < 0) return -1;

    char message[sizeof "0000:000000:000000:00000000000000000000000000000000"] = {};
    char gidWire[33] = {};
    if (read_full(fd, message, sizeof(message))) goto fail;
    unsigned int lid = 0, qpn = 0, psn = 0;
    if (sscanf(message, "%x:%x:%x:%32s", &lid, &qpn, &psn, gidWire) != 4 ||
        wire_to_gid(gidWire, &remote->gid)) goto fail;
    remote->qpn = qpn; remote->psn = psn;

    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTR, .path_mtu = mtu, .dest_qp_num = remote->qpn,
        .rq_psn = remote->psn, .max_dest_rd_atomic = 1, .min_rnr_timer = 12,
        .ah_attr = { .is_global = 1, .port_num = (uint8_t)ibPort,
            .grh = { .dgid = remote->gid, .sgid_index = (uint8_t)gidIndex, .hop_limit = 1 } }
    };
    int mask = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
               IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    if (ibv_modify_qp(qp, &attr, mask)) { perror("INIT->RTR"); goto fail; }
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS; attr.sq_psn = local->psn;
    attr.timeout = 14; attr.retry_cnt = 7; attr.rnr_retry = 7; attr.max_rd_atomic = 1;
    mask = IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
           IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC;
    if (ibv_modify_qp(qp, &attr, mask)) { perror("RTR->RTS"); goto fail; }

    gid_to_wire(&local->gid, gidWire);
    snprintf(message, sizeof(message), "%04x:%06x:%06x:%s", 0, local->qpn, local->psn, gidWire);
    if (write_full(fd, message, sizeof(message)) ||
        read_full(fd, message, sizeof("done"))) goto fail;
    close(fd);
    return 0;
fail:
    close(fd);
    return -1;
}

static enum ibv_mtu mtu_enum(uint32_t mtu)
{
    switch (mtu) {
    case 256: return IBV_MTU_256; case 512: return IBV_MTU_512;
    case 1024: return IBV_MTU_1024; case 2048: return IBV_MTU_2048;
    case 4096: return IBV_MTU_4096; default: return 0;
    }
}

int main(int argc, char **argv)
{
    const char *deviceName = NULL;
    uint32_t capacity = 262144, exchanges = 100, mtuBytes = 4096;
    uint16_t port = 18515;
    int ibPort = 1, gidIndex = 0, opt;
    while ((opt = getopt(argc, argv, "d:i:g:p:c:m:n:")) != -1) {
        switch (opt) {
        case 'd': deviceName = optarg; break;
        case 'i': ibPort = atoi(optarg); break;
        case 'g': gidIndex = atoi(optarg); break;
        case 'p': port = (uint16_t)strtoul(optarg, NULL, 0); break;
        case 'c': capacity = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 'm': mtuBytes = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 'n': exchanges = (uint32_t)strtoul(optarg, NULL, 0); break;
        default:
            fprintf(stderr, "usage: %s -d dev -i port -g gid -p tcpport "
                    "-c capacity -m mtu -n exchanges\n", argv[0]);
            return 2;
        }
    }
    enum ibv_mtu mtu = mtu_enum(mtuBytes);
    if (!deviceName || !capacity || !exchanges || !mtu) return 2;

    int result = 1, count = 0;
    struct ibv_device **devices = ibv_get_device_list(&count);
    struct ibv_device *device = NULL;
    for (int i = 0; devices && i < count; i++)
        if (!strcmp(ibv_get_device_name(devices[i]), deviceName)) device = devices[i];
    if (!device) { fprintf(stderr, "RDMA device %s not found\n", deviceName); goto out; }
    struct ibv_context *ctx = ibv_open_device(device);
    struct ibv_pd *pd = ctx ? ibv_alloc_pd(ctx) : NULL;
    struct ibv_cq *cq = ctx ? ibv_create_cq(ctx, 64, NULL, NULL, 0) : NULL;
    uint8_t *buffer = NULL;
    if (posix_memalign((void **)&buffer, 4096, capacity)) buffer = NULL;
    if (buffer) memset(buffer, 0x7b, capacity);
    struct ibv_mr *mr = pd && buffer ?
        ibv_reg_mr(pd, buffer, capacity, IBV_ACCESS_LOCAL_WRITE) : NULL;
    struct ibv_qp_init_attr init = {
        .send_cq = cq, .recv_cq = cq, .qp_type = IBV_QPT_RC,
        .cap = { .max_send_wr = 8, .max_recv_wr = 8, .max_send_sge = 1, .max_recv_sge = 1 }
    };
    struct ibv_qp *qp = pd && cq ? ibv_create_qp(pd, &init) : NULL;
    if (!ctx || !pd || !cq || !buffer || !mr || !qp) {
        fprintf(stderr, "asym echo peer resource allocation failed\n"); goto cleanup;
    }
    struct ibv_qp_attr attr = { .qp_state = IBV_QPS_INIT, .pkey_index = 0,
        .port_num = (uint8_t)ibPort, .qp_access_flags = 0 };
    if (ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                      IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
        perror("RESET->INIT"); goto cleanup;
    }
    struct ibv_sge recvSge = { .addr = (uintptr_t)buffer, .length = capacity, .lkey = mr->lkey };
    struct ibv_recv_wr recvWr = { .wr_id = 1, .sg_list = &recvSge, .num_sge = 1 }, *badRecv = NULL;
    if (ibv_post_recv(qp, &recvWr, &badRecv)) { perror("initial post_recv"); goto cleanup; }

    struct destination local = { .qpn = qp->qp_num,
        .psn = ((uint32_t)getpid() * 2654435761u) & 0xffffff };
    if (ibv_query_gid(ctx, (uint8_t)ibPort, gidIndex, &local.gid)) {
        perror("query_gid"); goto cleanup;
    }
    struct destination remote = {};
    if (exchange_destination(port, qp, ibPort, gidIndex, mtu, &local, &remote))
        goto cleanup;

    for (uint32_t i = 0; i < exchanges; i++) {
        struct ibv_wc wc = {};
        for (;;) {
            int n = ibv_poll_cq(cq, 1, &wc);
            if (n < 0) { fprintf(stderr, "recv poll failed at %u\n", i); goto cleanup; }
            if (n == 1) break;
        }
        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "recv CQE error at %u/%u: %s vendor_err=0x%x\n",
                    i, exchanges, ibv_wc_status_str(wc.status), wc.vendor_err);
            goto cleanup;
        }
        uint32_t got = wc.byte_len;
        if (i + 1 < exchanges) {
            struct ibv_sge sge2 = { .addr = (uintptr_t)buffer, .length = capacity, .lkey = mr->lkey };
            struct ibv_recv_wr wr2 = { .wr_id = 1, .sg_list = &sge2, .num_sge = 1 }, *bad2 = NULL;
            if (ibv_post_recv(qp, &wr2, &bad2)) { perror("repost_recv"); goto cleanup; }
        }
        struct ibv_sge sendSge = { .addr = (uintptr_t)buffer, .length = got, .lkey = mr->lkey };
        struct ibv_send_wr sendWr = { .wr_id = 2, .sg_list = &sendSge, .num_sge = 1,
            .opcode = IBV_WR_SEND, .send_flags = IBV_SEND_SIGNALED }, *badSend = NULL;
        if (ibv_post_send(qp, &sendWr, &badSend)) { perror("post_send echo"); goto cleanup; }
        for (;;) {
            int n = ibv_poll_cq(cq, 1, &wc);
            if (n < 0) { fprintf(stderr, "send poll failed at %u\n", i); goto cleanup; }
            if (n == 1) break;
        }
        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "send CQE error at %u/%u: %s vendor_err=0x%x\n",
                    i, exchanges, ibv_wc_status_str(wc.status), wc.vendor_err);
            goto cleanup;
        }
        printf("echo %u/%u: %u bytes\n", i + 1, exchanges, got);
    }
    printf("MLX_ASYM_ECHO_PEER PASS: %u exchanges, capacity=%u\n", exchanges, capacity);
    result = 0;
cleanup:
    if (qp) ibv_destroy_qp(qp);
    if (mr) ibv_dereg_mr(mr);
    free(buffer);
    if (cq) ibv_destroy_cq(cq);
    if (pd) ibv_dealloc_pd(pd);
    if (ctx) ibv_close_device(ctx);
out:
    if (devices) ibv_free_device_list(devices);
    return result;
}
