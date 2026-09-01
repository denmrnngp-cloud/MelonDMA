/* notes/50 diagnostic: passive RX-ring peer, no echo, no synchronization
 * with the sender at all — matches transport.cpp's real rx ring exactly:
 * RX_RING_MRS separate RDMA_CHUNK-sized MRs, all pre-posted upfront, each
 * slot reposted immediately after its completion is drained. This is what
 * a real ggml-rpc-server's RDMA rx ring looks like from the moment the
 * connection activates — no artificial mutual-echo synchronization like
 * mlx_asym_echo_peer.c, which every earlier isolated repro used. Tests
 * whether a sender blasting several small SENDs and one multi-chunk large
 * SEND *back-to-back*, waiting only on its own local send completions (the
 * real fire-and-forget SET_TENSOR pattern), survives against a purely
 * passive, non-synchronizing receiver.
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
    /* Keep the control socket open while the no-receive QP is held. The
     * sender uses it as the lifetime barrier for the RNR probe. */
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

#define RX_RING 24

int main(int argc, char **argv)
{
    const char *deviceName = NULL;
    uint32_t chunkSize = 262144, expected = 100, mtuBytes = 4096;
    uint16_t port = 18515;
    int ibPort = 1, gidIndex = 0, noRecv = 0, opt;
    while ((opt = getopt(argc, argv, "d:i:g:p:c:m:n:N")) != -1) {
        switch (opt) {
        case 'd': deviceName = optarg; break;
        case 'i': ibPort = atoi(optarg); break;
        case 'g': gidIndex = atoi(optarg); break;
        case 'p': port = (uint16_t)strtoul(optarg, NULL, 0); break;
        case 'c': chunkSize = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 'm': mtuBytes = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 'n': expected = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 'N': noRecv = 1; break;
        default:
            fprintf(stderr, "usage: %s -d dev -i port -g gid -p tcpport "
                    "-c chunk-size -m mtu -n expected-messages\n", argv[0]);
            return 2;
        }
    }
    enum ibv_mtu mtu = mtu_enum(mtuBytes);
    if (!deviceName || !chunkSize || !expected || !mtu) return 2;

    int result = 1, count = 0;
    struct ibv_device **devices = ibv_get_device_list(&count);
    struct ibv_device *device = NULL;
    for (int i = 0; devices && i < count; i++)
        if (!strcmp(ibv_get_device_name(devices[i]), deviceName)) device = devices[i];
    if (!device) { fprintf(stderr, "RDMA device %s not found\n", deviceName); goto out; }
    struct ibv_context *ctx = ibv_open_device(device);
    struct ibv_pd *pd = ctx ? ibv_alloc_pd(ctx) : NULL;
    struct ibv_cq *cq = ctx ? ibv_create_cq(ctx, RX_RING + 4, NULL, NULL, 0) : NULL;
    uint8_t *rxBuf = NULL;
    if (posix_memalign((void **)&rxBuf, 4096, (size_t)RX_RING * chunkSize)) rxBuf = NULL;
    struct ibv_mr *rxMrs[RX_RING] = {0};
    if (pd && rxBuf) {
        for (int i = 0; i < RX_RING; i++) {
            rxMrs[i] = ibv_reg_mr(pd, rxBuf + (size_t)i * chunkSize, chunkSize,
                                  IBV_ACCESS_LOCAL_WRITE);
            if (!rxMrs[i]) { fprintf(stderr, "rx_mr[%d] registration failed\n", i); break; }
        }
    }
    struct ibv_qp_init_attr init = {
        .send_cq = cq, .recv_cq = cq, .qp_type = IBV_QPT_RC,
        .cap = { .max_send_wr = 4, .max_recv_wr = RX_RING + 4,
                 .max_send_sge = 1, .max_recv_sge = 1 }
    };
    struct ibv_qp *qp = pd && cq ? ibv_create_qp(pd, &init) : NULL;
    if (!ctx || !pd || !cq || !rxBuf || !rxMrs[RX_RING - 1] || !qp) {
        fprintf(stderr, "rx drain peer resource allocation failed\n"); goto cleanup;
    }
    struct ibv_qp_attr attr = { .qp_state = IBV_QPS_INIT, .pkey_index = 0,
        .port_num = (uint8_t)ibPort, .qp_access_flags = 0 };
    if (ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                      IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
        perror("RESET->INIT"); goto cleanup;
    }
    for (int i = 0; i < RX_RING && !noRecv; i++) {
        struct ibv_sge sge = { .addr = (uintptr_t)(rxBuf + (size_t)i * chunkSize),
                               .length = chunkSize, .lkey = rxMrs[i]->lkey };
        struct ibv_recv_wr wr = { .wr_id = (uint64_t)i, .sg_list = &sge, .num_sge = 1 },
            *bad = NULL;
        if (ibv_post_recv(qp, &wr, &bad)) { perror("initial post_recv"); goto cleanup; }
    }

    struct destination local = { .qpn = qp->qp_num,
        .psn = ((uint32_t)getpid() * 2654435761u) & 0xffffff };
    if (ibv_query_gid(ctx, (uint8_t)ibPort, gidIndex, &local.gid)) {
        perror("query_gid"); goto cleanup;
    }
    struct destination remote = {};
    if (exchange_destination(port, qp, ibPort, gidIndex, mtu, &local, &remote))
        goto cleanup;
    if (noRecv) {
        printf("QP active with no receive buffers, waiting for RNR test traffic.\n");
        sleep(30);
        result = 0;
        goto cleanup;
    }
    printf("QP active, %d receive buffers of %u bytes pre-posted, "
           "passively draining %u expected messages, no echo.\n",
           RX_RING, chunkSize, expected);

    for (uint32_t i = 0; i < expected; i++) {
        struct ibv_wc wc = {};
        for (;;) {
            int n = ibv_poll_cq(cq, 1, &wc);
            if (n < 0) { fprintf(stderr, "poll failed at %u/%u\n", i, expected); goto cleanup; }
            if (n == 1) break;
        }
        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "CQE error at %u/%u: %s vendor_err=0x%x wr_id=%llu\n",
                    i, expected, ibv_wc_status_str(wc.status), wc.vendor_err,
                    (unsigned long long)wc.wr_id);
            goto cleanup;
        }
        int slot = (int)wc.wr_id;
        printf("drained %u/%u: slot=%d bytes=%u\n", i + 1, expected, slot, wc.byte_len);
        struct ibv_sge sge = { .addr = (uintptr_t)(rxBuf + (size_t)slot * chunkSize),
                               .length = chunkSize, .lkey = rxMrs[slot]->lkey };
        struct ibv_recv_wr wr = { .wr_id = (uint64_t)slot, .sg_list = &sge, .num_sge = 1 },
            *bad = NULL;
        if (ibv_post_recv(qp, &wr, &bad)) { perror("repost_recv"); goto cleanup; }
    }
    printf("MLX_RX_DRAIN_PEER PASS: %u messages absorbed, no echo, no sync\n", expected);
    result = 0;
cleanup:
    if (qp) ibv_destroy_qp(qp);
    for (int i = 0; i < RX_RING; i++) if (rxMrs[i]) ibv_dereg_mr(rxMrs[i]);
    free(rxBuf);
    if (cq) ibv_destroy_cq(cq);
    if (pd) ibv_dealloc_pd(pd);
    if (ctx) ibv_close_device(ctx);
out:
    if (devices) ibv_free_device_list(devices);
    return result;
}
