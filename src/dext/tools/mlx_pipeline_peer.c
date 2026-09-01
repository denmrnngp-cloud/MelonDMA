/* Pipelined RC echo peer for the MelonDMA Phase 3 hardware gate.
 *
 * rdma-core's ibv_rc_pingpong intentionally permits only one outstanding
 * SEND.  It cannot echo a burst reliably because multiple receive CQEs can
 * be consumed while its single SEND is pending.  This server keeps one
 * buffer per credit and reposts a receive only after the matching echo SEND
 * completes, so a true bounded window can be tested.
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

#define RECV_WRID(slot) (0x100000000ULL | (uint64_t)(slot))
#define SEND_WRID(slot) (0x200000000ULL | (uint64_t)(slot))
#define WRID_TYPE(id) ((id) >> 32)
#define WRID_SLOT(id) ((uint32_t)(id))

struct destination {
    uint32_t qpn;
    uint32_t psn;
    union ibv_gid gid;
};

struct remote_memory_wire {
    uint64_t address_be;
    uint32_t rkey_be;
    uint32_t length_be;
};

static uint64_t host_to_be64(uint64_t value)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap64(value);
#else
    return value;
#endif
}

static int read_full(int fd, void *buffer, size_t length)
{
    uint8_t *p = buffer;
    while (length) {
        ssize_t n = read(fd, p, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        length -= (size_t)n;
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
        p += n;
        length -= (size_t)n;
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
                                struct destination *remote, int *controlFd)
{
    int listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd < 0) return -1;
    int one = 1;
    (void)setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in address = {
        .sin_family = AF_INET, .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };
    if (bind(listenFd, (struct sockaddr *)&address, sizeof(address)) ||
        listen(listenFd, 1)) {
        perror("listen"); close(listenFd); return -1;
    }
    int fd = accept(listenFd, NULL, NULL);
    close(listenFd);
    if (fd < 0) return -1;

    char message[sizeof "0000:000000:000000:00000000000000000000000000000000"] = {};
    char gidWire[33] = {};
    if (read_full(fd, message, sizeof(message))) goto fail;
    unsigned int lid = 0, qpn = 0, psn = 0;
    if (sscanf(message, "%x:%x:%x:%32s", &lid, &qpn, &psn, gidWire) != 4 ||
        wire_to_gid(gidWire, &remote->gid)) goto fail;
    remote->qpn = qpn;
    remote->psn = psn;

    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTR,
        .path_mtu = mtu,
        .dest_qp_num = remote->qpn,
        .rq_psn = remote->psn,
        .max_dest_rd_atomic = 1,
        .min_rnr_timer = 12,
        .ah_attr = {
            .is_global = 1,
            .port_num = (uint8_t)ibPort,
            .grh = { .dgid = remote->gid, .sgid_index = (uint8_t)gidIndex,
                     .hop_limit = 1 }
        }
    };
    int mask = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
               IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
               IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    if (ibv_modify_qp(qp, &attr, mask)) { perror("INIT->RTR"); goto fail; }
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.sq_psn = local->psn;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.max_rd_atomic = 1;
    mask = IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT |
           IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC;
    if (ibv_modify_qp(qp, &attr, mask)) { perror("RTR->RTS"); goto fail; }

    gid_to_wire(&local->gid, gidWire);
    snprintf(message, sizeof(message), "%04x:%06x:%06x:%s", 0,
             local->qpn, local->psn, gidWire);
    if (write_full(fd, message, sizeof(message)) ||
        read_full(fd, message, sizeof("done"))) goto fail;
    if (controlFd) *controlFd = fd;
    else close(fd);
    return 0;
fail:
    close(fd);
    return -1;
}

static int post_recv(struct ibv_qp *qp, struct ibv_mr *mr, uint8_t *buffer,
                     uint32_t size, uint32_t slot)
{
    struct ibv_sge sge = { .addr = (uintptr_t)(buffer + (size_t)slot * size),
                           .length = size, .lkey = mr->lkey };
    struct ibv_recv_wr wr = { .wr_id = RECV_WRID(slot), .sg_list = &sge,
                              .num_sge = 1 }, *bad = NULL;
    return ibv_post_recv(qp, &wr, &bad);
}

static int post_send(struct ibv_qp *qp, struct ibv_mr *mr, uint8_t *buffer,
                     uint32_t size, uint32_t slot)
{
    struct ibv_sge sge = { .addr = (uintptr_t)(buffer + (size_t)slot * size),
                           .length = size, .lkey = mr->lkey };
    struct ibv_send_wr wr = { .wr_id = SEND_WRID(slot), .sg_list = &sge,
                              .num_sge = 1, .opcode = IBV_WR_SEND,
                              .send_flags = IBV_SEND_SIGNALED }, *bad = NULL;
    return ibv_post_send(qp, &wr, &bad);
}

static int __attribute__((unused)) post_read(struct ibv_qp *qp, struct ibv_mr *mr, uint8_t *buffer,
                     uint32_t size, uint64_t remote_addr, uint32_t remote_rkey,
                     uint32_t slot)
{
    struct ibv_sge sge = { .addr = (uintptr_t)buffer, .length = size, .lkey = mr->lkey };
    struct ibv_send_wr wr = { .wr_id = SEND_WRID(slot), .sg_list = &sge,
        .num_sge = 1, .opcode = IBV_WR_RDMA_READ, .send_flags = IBV_SEND_SIGNALED,
        .wr.rdma = { .remote_addr = remote_addr, .rkey = remote_rkey } }, *bad = NULL;
    return ibv_post_send(qp, &wr, &bad);
}

static int post_write(struct ibv_qp *qp, struct ibv_mr *mr, uint8_t *buffer,
                      uint32_t size, uint64_t remote_addr, uint32_t remote_rkey,
                      uint32_t slot)
{
    struct ibv_sge sge = { .addr = (uintptr_t)buffer, .length = size,
                           .lkey = mr->lkey };
    struct ibv_send_wr wr = { .wr_id = SEND_WRID(slot), .sg_list = &sge,
                              .num_sge = 1, .opcode = IBV_WR_RDMA_WRITE,
                              .send_flags = IBV_SEND_SIGNALED,
                              .wr.rdma = { .remote_addr = remote_addr,
                                           .rkey = remote_rkey } }, *bad = NULL;
    return ibv_post_send(qp, &wr, &bad);
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
    uint32_t size = 1024, iters = 100000, depth = 64, mtuBytes = 1024;
    uint16_t port = 18515;
    int ibPort = 1, gidIndex = 0, opt, writeMode = 0, mwMode = 0, readMode = 0, reverseReadMode = 0, holdRecv = 0;
    int rnrMode = 0;
    while ((opt = getopt(argc, argv, "WMRQqNd:i:g:p:s:m:r:n:")) != -1) {
        switch (opt) {
        case 'W': writeMode = 1; break;
        case 'R': writeMode = 2; break;
        case 'M': mwMode = 1; writeMode = 2; break;
        case 'Q': readMode = 1; writeMode = 2; break;
        case 'q': reverseReadMode = 1; writeMode = 2; break;
        case 'N': holdRecv = 1; rnrMode = 1; break;
        case 'd': deviceName = optarg; break;
        case 'i': ibPort = atoi(optarg); break;
        case 'g': gidIndex = atoi(optarg); break;
        case 'p': port = (uint16_t)strtoul(optarg, NULL, 0); break;
        case 's': size = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 'm': mtuBytes = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 'r': depth = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 'n': iters = (uint32_t)strtoul(optarg, NULL, 0); break;
        default: return 2;
        }
    }
    enum ibv_mtu mtu = mtu_enum(mtuBytes);
    if (!deviceName || !size || !iters || !depth || !mtu) return 2;
    if (depth > iters) depth = iters;

    int result = 1, count = 0;
    struct ibv_device **devices = ibv_get_device_list(&count);
    struct ibv_device *device = NULL;
    for (int i = 0; devices && i < count; i++)
        if (!strcmp(ibv_get_device_name(devices[i]), deviceName)) device = devices[i];
    if (!device) { fprintf(stderr, "RDMA device %s not found\n", deviceName); goto out; }
    struct ibv_context *ctx = ibv_open_device(device);
    struct ibv_pd *pd = ctx ? ibv_alloc_pd(ctx) : NULL;
    struct ibv_cq *cq = ctx ? ibv_create_cq(ctx, (int)(depth * 2 + 1), NULL, NULL, 0) : NULL;
    uint8_t *buffer = NULL;
    size_t bufferBytes = writeMode ? size : (size_t)depth * size;
    if (posix_memalign((void **)&buffer, 4096, bufferBytes)) buffer = NULL;
    if (buffer) memset(buffer, writeMode == 1 ? 0xa5 :
                             writeMode == 2 ? 0xa7 : 0x7b, bufferBytes);
    struct ibv_mr *mr = pd && buffer ? ibv_reg_mr(pd, buffer,
        bufferBytes, IBV_ACCESS_LOCAL_WRITE |
        (writeMode == 1 ? IBV_ACCESS_REMOTE_WRITE :
         (readMode || reverseReadMode) ? IBV_ACCESS_REMOTE_READ : 0)) : NULL;
    struct ibv_qp_init_attr init = {
        .send_cq = cq, .recv_cq = cq, .qp_type = IBV_QPT_RC,
        .cap = { .max_send_wr = depth, .max_recv_wr = depth,
                 .max_send_sge = 1, .max_recv_sge = 1 }
    };
    struct ibv_qp *qp = pd && cq ? ibv_create_qp(pd, &init) : NULL;
    if (!ctx || !pd || !cq || !buffer || !mr || !qp) {
        fprintf(stderr, "pipeline peer resource allocation failed\n"); goto cleanup;
    }
    struct ibv_qp_attr attr = { .qp_state = IBV_QPS_INIT, .pkey_index = 0,
        .port_num = (uint8_t)ibPort,
        .qp_access_flags = writeMode == 1 ? IBV_ACCESS_REMOTE_WRITE :
                           (readMode || reverseReadMode) ? IBV_ACCESS_REMOTE_READ : 0 };
    if (ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                      IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
        perror("RESET->INIT"); goto cleanup;
    }
    for (uint32_t slot = 0; !writeMode && !holdRecv && slot < depth; slot++)
        if (post_recv(qp, mr, buffer, size, slot)) {
            perror("initial post_recv"); goto cleanup;
        }
    struct destination local = { .qpn = qp->qp_num,
        .psn = ((uint32_t)getpid() * 2654435761u) & 0xffffff };
    if (ibv_query_gid(ctx, (uint8_t)ibPort, gidIndex, &local.gid)) {
        perror("query_gid"); goto cleanup;
    }
    struct destination remote = {};
    int controlFd = -1;
    if (exchange_destination(port, qp, ibPort, gidIndex, mtu,
                             &local, &remote, (writeMode || rnrMode) ? &controlFd : NULL))
        goto cleanup;

    if (reverseReadMode) {
        /* The Mac owns the source MR; Spark reads it and returns a
         * completion token after validating the copied payload. */
        struct remote_memory_wire memory = {};
        if (read_full(controlFd, &memory, sizeof(memory)) || !memory.address_be || !memory.rkey_be) {
            fprintf(stderr, "reverse READ target exchange failed\n"); goto cleanup;
        }
        uint64_t remote_addr = host_to_be64(memory.address_be);
        uint32_t remote_rkey = ntohl(memory.rkey_be);
        fprintf(stderr, "reverse READ target addr=0x%llx rkey=0x%x len=%u\n",
                (unsigned long long)remote_addr, remote_rkey, ntohl(memory.length_be));
        for (uint32_t i = 0; i < iters; i++) {
            if (post_read(qp, mr, buffer, size, remote_addr, remote_rkey, i)) {
                fprintf(stderr, "reverse READ post failed iter=%u\n", i); goto cleanup;
            }
            struct ibv_wc wc = {};
            for (;;) { int n = ibv_poll_cq(cq, 1, &wc); if (n < 0) goto cleanup; if (n) break; }
            if (wc.status != IBV_WC_SUCCESS) {
                fprintf(stderr, "reverse READ CQE failed iter=%u status=%d opcode=%d vendor=%u\n",
                        i, wc.status, wc.opcode, wc.vendor_err); goto cleanup;
            }
            for (uint32_t b = 0; b < size; b++) if (buffer[b] != 0xa7) {
                fprintf(stderr, "reverse READ payload mismatch iter=%u byte=%u got=0x%02x\n", i, b, buffer[b]); goto cleanup;
            }
        }
        if (write_full(controlFd, "pass", sizeof("pass"))) goto cleanup;
        close(controlFd); printf("MLX_REVERSE_READ_PEER PASS: Spark read %u bytes x %u\n", size, iters); result = 0; goto cleanup;
    }
    if (readMode) {
        /* The peer owns the source MR; the Mac reads it and returns a
         * completion token after validating the copied payload. */
        struct ibv_sge readSge = { .addr = (uintptr_t)buffer, .length = size, .lkey = mr->lkey };
        struct ibv_send_wr readWr = { .wr_id = SEND_WRID(0), .sg_list = &readSge,
            .num_sge = 1, .opcode = IBV_WR_RDMA_READ, .send_flags = IBV_SEND_SIGNALED,
            .wr.rdma = { .remote_addr = (uint64_t)(uintptr_t)buffer, .rkey = mr->rkey } };
        (void)readWr;
        struct remote_memory_wire memory = {
            .address_be = host_to_be64((uint64_t)(uintptr_t)buffer),
            .rkey_be = htonl(mr->rkey), .length_be = htonl(size) };
        memset(buffer, 0xa7, size);
        char token[sizeof("pass")] = {};
        if (write_full(controlFd, &memory, sizeof(memory)) ||
            read_full(controlFd, token, sizeof(token)) ||
            memcmp(token, "pass", sizeof(token))) goto cleanup;
        close(controlFd); printf("MLX_READ_PEER PASS: Mac read %u bytes x %u\n", size, iters); result = 0; goto cleanup;
    }
    if (rnrMode) {
        /* Keep the QP alive without posting RQ entries. The Mac peer sends
         * one signaled WR and must observe RNR retry exhaustion. */
        char token[sizeof("pass")] = {};
        if (read_full(controlFd, token, sizeof(token))) goto cleanup;
        /* Keep the QP alive while the sender exhausts its RNR retry budget.
         * The Mac returns the observed CQE token before we tear down. */
        if (read_full(controlFd, token, sizeof(token))) goto cleanup;
        if (memcmp(token, "pass", sizeof(token))) goto cleanup;
        close(controlFd); result = 0; printf("MLX_RNR_PEER PASS: receive queue intentionally withheld\n"); goto cleanup;
    }
    if (writeMode == 2) {
        struct remote_memory_wire memory = {};
        char request[sizeof("pass")] = {};
        if (!mwMode && (read_full(controlFd, &memory, sizeof(memory)) ||
            !memory.address_be || !memory.rkey_be ||
            ntohl(memory.length_be) < size)) {
            fprintf(stderr, "reverse WRITE target exchange failed\n");
            close(controlFd); goto cleanup;
        }
        uint64_t remote_addr = host_to_be64(memory.address_be);
        uint32_t remote_rkey = ntohl(memory.rkey_be);
        if (mwMode) {
            /* The Mac sends a sequence of target descriptors and asks us to
             * issue one-sided WRITE with each key. The expected result is
             * encoded in the response token: pass means accepted, fail means
             * the stale/invalid key was correctly rejected. */
            for (uint32_t phase = 0; phase < 3; phase++) {
                if (read_full(controlFd, &memory, sizeof(memory))) goto cleanup;
                remote_addr = host_to_be64(memory.address_be);
                remote_rkey = ntohl(memory.rkey_be);
                if (post_write(qp, mr, buffer, size, remote_addr, remote_rkey, phase)) goto cleanup;
                struct ibv_wc mwc = {};
                for (;;) { int n = ibv_poll_cq(cq, 1, &mwc); if (n < 0) goto cleanup; if (n) break; }
                fprintf(stderr, "MW_INTEROP phase=%u status=%u opcode=%d vendor=%u\n",
                        phase, mwc.status, mwc.opcode, mwc.vendor_err);
                if (phase == 0 || phase == 1) {
                    if (mwc.status != IBV_WC_SUCCESS) goto cleanup;
                } else if (mwc.status == IBV_WC_SUCCESS) {
                    fprintf(stderr, "MW interop accepted stale/invalid rkey phase=%u\n", phase); goto cleanup;
                }
                if (write_full(controlFd, mwc.status == IBV_WC_SUCCESS ? "pass" : "fail", sizeof("pass"))) goto cleanup;
            }
            if (write_full(controlFd, "pass", sizeof("pass"))) goto cleanup;
            close(controlFd); printf("MLX_MW_INTEROP_PEER PASS: accepted valid keys and rejected stale/invalid keys\n"); result = 0; goto cleanup;
        }
        for (uint32_t i = 0; i < iters; i++) {
            if (post_write(qp, mr, buffer, size, remote_addr, remote_rkey, i)) {
                perror("post_write"); close(controlFd); goto cleanup;
            }
            struct ibv_wc wc = {};
            for (;;) {
                int n = ibv_poll_cq(cq, 1, &wc);
                if (n < 0) { fprintf(stderr, "reverse WRITE poll failed\n"); close(controlFd); goto cleanup; }
                if (n == 1) break;
            }
            if (wc.status != IBV_WC_SUCCESS) {
                fprintf(stderr, "reverse WRITE CQE error: %s\n",
                        ibv_wc_status_str(wc.status));
                close(controlFd); goto cleanup;
            }
        }
        if (write_full(controlFd, "verify", sizeof("verify")) ||
            read_full(controlFd, request, sizeof(request)) ||
            memcmp(request, "pass", sizeof(request))) {
            fprintf(stderr, "reverse WRITE Mac verification failed\n");
            close(controlFd); goto cleanup;
        }
        close(controlFd);
        printf("MLX_REVERSE_WRITE_PEER PASS: Spark wrote %u bytes x %u into Mac MR\n",
               size, iters);
        result = 0;
        goto cleanup;
    }
    if (writeMode == 1) {
        struct remote_memory_wire memory = {
            .address_be = host_to_be64((uint64_t)(uintptr_t)buffer),
            .rkey_be = htonl(mr->rkey),
            .length_be = htonl(size),
        };
        char request[sizeof("verify")] = {};
        if (write_full(controlFd, &memory, sizeof(memory)) ||
            read_full(controlFd, request, sizeof(request)) ||
            memcmp(request, "verify", sizeof(request))) {
            fprintf(stderr, "WRITE control exchange failed\n");
            close(controlFd); goto cleanup;
        }
        for (uint32_t i = 0; i < size; i++) {
            if (buffer[i] != 0x7b) {
                fprintf(stderr, "RDMA WRITE mismatch at byte=%u got=0x%02x\n",
                        i, buffer[i]);
                close(controlFd); goto cleanup;
            }
        }
        if (write_full(controlFd, "pass", sizeof("pass"))) {
            close(controlFd); goto cleanup;
        }
        close(controlFd);
        printf("MLX_WRITE_PEER PASS: remote memory verified bytes=%u\n", size);
        result = 0;
        goto cleanup;
    }

    uint32_t recvDone = 0, sendDone = 0, recvPosted = depth;
    while (recvDone < iters || sendDone < iters) {
        struct ibv_wc wc[32];
        int n = ibv_poll_cq(cq, 32, wc);
        if (n < 0) { fprintf(stderr, "poll_cq failed\n"); goto cleanup; }
        if (!n) { usleep(25); continue; }
        for (int i = 0; i < n; i++) {
            if (wc[i].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "CQE error: %s wrid=0x%llx\n",
                        ibv_wc_status_str(wc[i].status),
                        (unsigned long long)wc[i].wr_id);
                goto cleanup;
            }
            uint32_t slot = WRID_SLOT(wc[i].wr_id);
            if (slot >= depth) { fprintf(stderr, "bad WR slot\n"); goto cleanup; }
            if (WRID_TYPE(wc[i].wr_id) == 1) {
                recvDone++;
                /* Echo the received message length.  This also makes the
                 * mixed-size warmup diagnostic model a byte-message peer
                 * instead of incorrectly expanding every small request to
                 * the maximum buffer size. */
                if (post_send(qp, mr, buffer, wc[i].byte_len, slot)) {
                    perror("post_send"); goto cleanup;
                }
            } else if (WRID_TYPE(wc[i].wr_id) == 2) {
                sendDone++;
                if (recvPosted < iters) {
                    if (post_recv(qp, mr, buffer, size, slot)) {
                        perror("repost_recv"); goto cleanup;
                    }
                    recvPosted++;
                }
            } else {
                fprintf(stderr, "unknown WRID\n"); goto cleanup;
            }
        }
    }
    printf("MLX_PIPELINE_PEER PASS: %u echoed messages size=%u depth=%u\n",
           iters, size, depth);
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
