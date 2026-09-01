/* notes/50 diagnostic: the mixed small-then-large SEND/RECV pattern that
 * mlx_asym_echo_peer.c + mlx_phase2_gate's --warmup-* already proved works
 * fine through librdma_shim's *native* API — but run through
 * usermode/libibverbs_compat/verbs_compat.c instead, the one layer
 * llama.cpp's ggml-rpc transport.cpp actually uses that mlx_phase2_gate
 * bypasses entirely. Isolates whether the compat shim's ibv_create_qp/
 * ibv_post_send/ibv_post_recv translation is where the bug lives.
 *
 * QP capabilities are set to match transport.cpp's rdma_probe() exactly
 * (max_send_wr=4, max_recv_wr=28, max_inline_data=256, scq depth=16 by
 * default) rather than mlx_phase2_gate's own values.
 *
 * Peer: mlx_asym_echo_peer (already Linux/stock-libverbs, unchanged).
 */
#include <infiniband/verbs.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/* Optional artificial concurrent CPU load (notes/50: every structural
 * variable tested so far passes clean; real system load under the actual
 * llama serve process is the one remaining untested difference). */
static atomic_int g_stress_stop = 0;
static void *stress_worker(void *arg)
{
    (void)arg;
    volatile double x = 1.0000001;
    while (!atomic_load_explicit(&g_stress_stop, memory_order_relaxed)) {
        for (int i = 0; i < 1000000; i++) x = x * 1.0000001 - 0.0000001;
    }
    return NULL;
}

/* Real mmap'd file I/O concurrent with the RDMA send — a plain CPU spin
 * loop generates zero page faults and zero memory-bus/disk traffic, but
 * llama.cpp's real model-loading main thread is reading the *next* tensor
 * off the (likely mmap'd) GGUF file at the same time the RPC worker thread
 * sends *this* one ("overlap data transfers with computation" per
 * ggml-rpc.cpp's own comment on the async tensor-set path) — genuinely
 * different kind of concurrent load than pure ALU work. */
static const char * g_io_stress_path = NULL;
static void *io_stress_worker(void *arg)
{
    (void)arg;
    int fd = open(g_io_stress_path, O_RDONLY);
    if (fd < 0) { perror("io_stress open"); return NULL; }
    struct stat st;
    if (fstat(fd, &st) || st.st_size < 4096) { close(fd); return NULL; }
    size_t len = (size_t)st.st_size;
    void * map = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) { perror("io_stress mmap"); close(fd); return NULL; }
    volatile uint8_t sink = 0;
    while (!atomic_load_explicit(&g_stress_stop, memory_order_relaxed)) {
        const uint8_t * p = (const uint8_t *)map;
        for (size_t off = 0; off < len; off += 4096) {
            sink ^= p[off];
            if (atomic_load_explicit(&g_stress_stop, memory_order_relaxed)) break;
        }
        madvise(map, len, MADV_DONTNEED); /* force real re-faulting next pass */
    }
    (void)sink;
    munmap(map, len);
    close(fd);
    return NULL;
}

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

static enum ibv_mtu mtu_enum(uint32_t mtu)
{
    switch (mtu) {
    case 256: return IBV_MTU_256; case 512: return IBV_MTU_512;
    case 1024: return IBV_MTU_1024; case 2048: return IBV_MTU_2048;
    case 4096: return IBV_MTU_4096; default: return 0;
    }
}

static int connect_to(const char *host, uint16_t port)
{
    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%u", port);
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, portStr, &hints, &res)) return -1;
    int fd = -1;
    for (struct addrinfo *rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* One full connection lifecycle: fresh ibv_open_device (matches
 * transport.cpp's rdma_probe(), called once per RPC connection, not once
 * per process), PD/CQ/QP/MR, TCP handshake against a peer already
 * listening on `port`, `warmupIters` small exchanges of `warmupSize` bytes
 * then one final exchange of `finalSize` bytes, full teardown. Used both
 * for short-lived "probe" connections (finalSize == warmupSize, 0 warmups)
 * and the real one (finalSize == capacity, 11 warmups) — replicates
 * llama.cpp's own pattern of several throwaway RPC connections (device
 * free-memory probing) before the connection that actually loads weights,
 * all within one process. */
static int run_connection(const char *host, uint16_t port, enum ibv_mtu mtu,
                          uint32_t capacity, const uint32_t *warmupSizes,
                          uint32_t warmupCount, uint32_t finalSize,
                          const char *label)
{
    int result = 1, count = 0, fd = -1;
    struct ibv_device **devices = ibv_get_device_list(&count);
    if (!devices || count < 1) { fprintf(stderr, "no RDMA devices\n"); goto out; }
    struct ibv_context *ctx = ibv_open_device(devices[0]);
    if (!ctx) { fprintf(stderr, "ibv_open_device failed\n"); goto out; }
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    /* Match transport.cpp's rdma_probe() exactly (RDMA_RX_DEPTH=24). */
    struct ibv_cq *scq = ibv_create_cq(ctx, 16, NULL, NULL, 0);
    struct ibv_cq *rcq = ibv_create_cq(ctx, 24 + 4, NULL, NULL, 0);
    uint8_t *buffer = NULL;
    if (posix_memalign((void **)&buffer, 4096, capacity)) buffer = NULL;
    /* Varied content, not a constant fill: closes the one remaining gap
     * between this repro and real tensor weight data, in case a
     * content-dependent path exists anywhere (never found by code reading,
     * but every size/structure/timing variable tested clean, so this is
     * the last cheap thing left to rule out). */
    if (buffer) for (uint32_t i = 0; i < capacity; i++) buffer[i] = (uint8_t)(i * 2654435761u);
    struct ibv_mr *mr = pd && buffer ?
        ibv_reg_mr(pd, buffer, capacity, IBV_ACCESS_LOCAL_WRITE) : NULL;

    /* transport.cpp registers 24 separate RDMA_CHUNK-sized rx_mrs on top of
     * the one tx_mr (rx_buf = RDMA_RX_DEPTH * RDMA_CHUNK contiguous, one MR
     * per slot) — 25 MRs of 256 KiB on this same PD in the real code, not
     * the 1 MR mlx_phase2_gate and this tool's earlier runs used. Replicate
     * that memory footprint (unused for traffic — matching the real RX
     * ring's initial state, which sits idle at connection setup too) in
     * case a per-PD cumulative MTT/page-table limit is what the earlier
     * clean single-MR runs never touched. */
#define RX_RING_MRS 24
    void *rxBuf = NULL;
    struct ibv_mr *rxMrs[RX_RING_MRS] = {0};
    if (pd) {
        if (posix_memalign(&rxBuf, 4096, (size_t)RX_RING_MRS * capacity)) rxBuf = NULL;
        if (rxBuf) {
            for (int i = 0; i < RX_RING_MRS; i++) {
                rxMrs[i] = ibv_reg_mr(pd, (uint8_t *)rxBuf + (size_t)i * capacity,
                                     capacity, IBV_ACCESS_LOCAL_WRITE);
                if (!rxMrs[i]) {
                    fprintf(stderr, "[%s] rx_mr[%d]/%d registration failed\n",
                            label, i, RX_RING_MRS);
                    break;
                }
            }
        }
    }
    struct ibv_qp_init_attr init = {
        .send_cq = scq, .recv_cq = rcq, .qp_type = IBV_QPT_RC,
        .cap = { .max_send_wr = 4, .max_recv_wr = 24 + 4,
                 .max_send_sge = 1, .max_recv_sge = 1, .max_inline_data = 256 }
    };
    struct ibv_qp *qp = pd && scq && rcq ? ibv_create_qp(pd, &init) : NULL;
    if (!ctx || !pd || !scq || !rcq || !buffer || !mr || !qp || !rxBuf ||
        !rxMrs[RX_RING_MRS - 1]) {
        fprintf(stderr, "compat mixed gate resource allocation failed "
                "(rxBuf=%p last_rx_mr=%p)\n", (void *)rxBuf,
                (void *)rxMrs[RX_RING_MRS - 1]);
        goto cleanup;
    }
    printf("[%s] QP created: qpn=%u max_inline_data(negotiated)=%u; "
           "1 tx_mr + %d rx_mrs registered on this PD (matches transport.cpp)\n",
           label, qp->qp_num, init.cap.max_inline_data, RX_RING_MRS);
    struct ibv_qp_attr attr = { .qp_state = IBV_QPS_INIT, .pkey_index = 0,
        .port_num = 1, .qp_access_flags = 0 };
    if (ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                      IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
        perror("RESET->INIT"); goto cleanup;
    }

    fd = connect_to(host, port);
    if (fd < 0) { fprintf(stderr, "connect to %s:%u failed\n", host, port); goto cleanup; }

    struct destination local = { .qpn = qp->qp_num,
        .psn = ((uint32_t)getpid() * 2654435761u) & 0xffffff };
    if (ibv_query_gid(ctx, 1, 0, &local.gid)) {
        /* gid_idx 0 is what rdma_set_roce_address()'s readback always uses
         * in this shim (see verbs_compat.c ibv_open_device). */
        perror("query_gid"); goto cleanup;
    }
    char gidWire[33] = {};
    gid_to_wire(&local.gid, gidWire);
    char message[sizeof "0000:000000:000000:00000000000000000000000000000000"] = {};
    snprintf(message, sizeof(message), "%04x:%06x:%06x:%s", 0,
             local.qpn, local.psn, gidWire);
    if (write_full(fd, message, sizeof(message))) {
        fprintf(stderr, "destination write failed\n"); goto cleanup;
    }
    memset(message, 0, sizeof(message));
    if (read_full(fd, message, sizeof(message))) {
        fprintf(stderr, "destination read failed\n"); goto cleanup;
    }
    struct destination remote = {};
    unsigned int lid = 0, qpn = 0, psn = 0;
    if (sscanf(message, "%x:%x:%x:%32s", &lid, &qpn, &psn, gidWire) != 4 ||
        wire_to_gid(gidWire, &remote.gid)) {
        fprintf(stderr, "destination parse failed\n"); goto cleanup;
    }
    remote.qpn = qpn; remote.psn = psn;
    printf("Peer: qpn=0x%06x psn=0x%06x\n", remote.qpn, remote.psn);

    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR; attr.path_mtu = mtu;
    attr.dest_qp_num = remote.qpn; attr.rq_psn = remote.psn;
    attr.max_dest_rd_atomic = 1; attr.min_rnr_timer = 12;
    attr.ah_attr.is_global = 1; attr.ah_attr.port_num = 1;
    attr.ah_attr.grh.dgid = remote.gid; attr.ah_attr.grh.sgid_index = 0;
    attr.ah_attr.grh.hop_limit = 1;
    if (ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                      IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                      IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER)) {
        perror("INIT->RTR"); goto cleanup;
    }
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS; attr.sq_psn = local.psn;
    attr.timeout = 14; attr.retry_cnt = 7; attr.rnr_retry = 7; attr.max_rd_atomic = 1;
    if (ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT |
                      IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC)) {
        perror("RTR->RTS"); goto cleanup;
    }
    if (write_full(fd, "done", sizeof("done"))) {
        fprintf(stderr, "handshake done-ack failed\n"); goto cleanup;
    }
    printf("[%s] QP active. Starting %u warmup exchanges, then one "
           "%u-byte exchange, all on qpn=%u.\n",
           label, warmupCount, finalSize, qp->qp_num);

    /* True fire-and-forget, matching transport.cpp's rdma_send() exactly:
     * post SEND, wait ONLY for the local send completion, move on — no
     * recv posted on this side at all, no waiting for the peer in any way.
     * Every earlier version of this tool synchronized via a mutual echo
     * after each phase, which real fire-and-forget SET_TENSOR traffic never
     * does; this is the last untested structural gap (notes/50). A message
     * bigger than `capacity` (== RDMA_CHUNK) is chunked into several
     * back-to-back SENDs, matching rdma_send()'s own internal loop. */
    for (uint32_t phase = 0; phase < warmupCount + 1; phase++) {
        int isFinal = phase == warmupCount;
        uint32_t remaining = isFinal ? finalSize : warmupSizes[phase];
        uint32_t chunkIndex = 0;
        while (remaining > 0 || chunkIndex == 0) {
            uint32_t thisSize = remaining < capacity ? remaining : capacity;
            struct ibv_sge sendSge = { .addr = (uintptr_t)buffer, .length = thisSize,
                                       .lkey = mr->lkey };
            struct ibv_send_wr sendWr = { .wr_id = 2, .sg_list = &sendSge, .num_sge = 1,
                .opcode = IBV_WR_SEND, .send_flags = IBV_SEND_SIGNALED }, *badSend = NULL;
            if (ibv_post_send(qp, &sendWr, &badSend)) {
                fprintf(stderr, "post_send failed at phase %u chunk %u (size=%u): %s\n",
                        phase, chunkIndex, thisSize, strerror(errno));
                goto cleanup;
            }
            struct ibv_wc wc = {};
            for (;;) {
                int n = ibv_poll_cq(scq, 1, &wc);
                if (n < 0) { fprintf(stderr, "send poll_cq failed at phase %u chunk %u\n", phase, chunkIndex); goto cleanup; }
                if (n == 1) break;
            }
            if (wc.status != IBV_WC_SUCCESS) {
                fprintf(stderr, "CQE error at phase %u chunk %u (size=%u): status=%d (%s) "
                        "vendor_err=0x%x qpn=%u wr_id=%llu byte_len=%u\n",
                        phase, chunkIndex, thisSize, wc.status, ibv_wc_status_str(wc.status),
                        wc.vendor_err, wc.qp_num, (unsigned long long)wc.wr_id, wc.byte_len);
                goto cleanup;
            }
            printf("[%s] phase %u/%u chunk %u OK: %u bytes (fire-and-forget, no wait)\n",
                   label, phase + 1, warmupCount + 1, chunkIndex, thisSize);
            remaining -= thisSize;
            chunkIndex++;
        }
    }
    printf("[%s] CONNECTION PASS: %u warmups + 1 exchange of "
           "%u bytes, all through libibverbs_compat, on one QP\n",
           label, warmupCount, finalSize);
    result = 0;
cleanup:
    if (fd >= 0) close(fd);
    if (qp) ibv_destroy_qp(qp);
    if (mr) ibv_dereg_mr(mr);
    free(buffer);
    for (int i = 0; i < RX_RING_MRS; i++) if (rxMrs[i]) ibv_dereg_mr(rxMrs[i]);
    free(rxBuf);
    if (rcq && rcq != scq) ibv_destroy_cq(rcq);
    if (scq) ibv_destroy_cq(scq);
    if (pd) ibv_dealloc_pd(pd);
    if (ctx) ibv_close_device(ctx);
out:
    if (devices) ibv_free_device_list(devices);
    return result;
}

int main(int argc, char **argv)
{
    if (argc != 7 && argc != 8 && argc != 9) {
        fprintf(stderr, "usage: %s <spark-host> <base-port> <mtu> <capacity> "
                "<final-size> <warmup-sizes> [stress-threads] [io-stress-file]\n"
                "  <warmup-sizes> is a comma-separated list, e.g. "
                "1,8,12,1,8,8,1,8,312,1,8 (the exact sequence observed on\n"
                "  qpn=213 before the real crash, see notes/50). One "
                "throwaway connection (single 8-byte exchange, full\n"
                "  teardown) is opened per warmup value's position beyond "
                "the first, matching llama.cpp's device-memory-fit probe\n"
                "  connections, then the real connection replays the exact "
                "warmup sequence followed by one <final-size>-byte\n"
                "  exchange — all in this ONE process, fresh "
                "ibv_open_device() each time.\n", argv[0]);
        return 2;
    }
    const char *host = argv[1];
    uint16_t basePort = (uint16_t)strtoul(argv[2], NULL, 0);
    uint32_t mtuBytes = (uint32_t)strtoul(argv[3], NULL, 0);
    uint32_t capacity = (uint32_t)strtoul(argv[4], NULL, 0);
    uint32_t finalSize = (uint32_t)strtoul(argv[5], NULL, 0);
    enum ibv_mtu mtu = mtu_enum(mtuBytes);
    if (!mtu || !capacity) return 2;

    uint32_t warmupSizes[64];
    uint32_t warmupCount = 0;
    char *sizesCopy = strdup(argv[6]);
    for (char *tok = strtok(sizesCopy, ","); tok && warmupCount < 64; tok = strtok(NULL, ",")) {
        uint32_t v = (uint32_t)strtoul(tok, NULL, 0);
        if (!v || v > capacity) { fprintf(stderr, "bad warmup size: %s\n", tok); free(sizesCopy); return 2; }
        warmupSizes[warmupCount++] = v;
    }
    free(sizesCopy);
    if (!warmupCount) { fprintf(stderr, "no warmup sizes given\n"); return 2; }
    uint32_t stressThreads = argc >= 8 ? (uint32_t)strtoul(argv[7], NULL, 0) : 0;
    if (argc == 9) g_io_stress_path = argv[8];

    uint32_t priorConnections = 3;
    for (uint32_t c = 0; c < priorConnections; c++) {
        char label[32];
        snprintf(label, sizeof(label), "throwaway %u/%u", c + 1, priorConnections);
        uint32_t one = 8;
        if (run_connection(host, (uint16_t)(basePort + c), mtu, capacity,
                           &one, 0, 8, label)) {
            fprintf(stderr, "MLX_COMPAT_MIXED_GATE FAIL: throwaway connection "
                    "%u/%u failed\n", c + 1, priorConnections);
            return 1;
        }
    }
    pthread_t stressTids[64];
    if (stressThreads > 64) stressThreads = 64;
    for (uint32_t i = 0; i < stressThreads; i++) {
        if (pthread_create(&stressTids[i], NULL, stress_worker, NULL)) {
            fprintf(stderr, "stress thread %u/%u failed to start\n", i, stressThreads);
            stressThreads = i;
            break;
        }
    }
    if (stressThreads) printf("=== %u artificial CPU-load threads running "
                              "concurrently with the real connection ===\n", stressThreads);
    pthread_t ioTid;
    int ioStarted = 0;
    if (g_io_stress_path) {
        if (pthread_create(&ioTid, NULL, io_stress_worker, NULL)) {
            fprintf(stderr, "io-stress thread failed to start\n");
        } else {
            ioStarted = 1;
            printf("=== concurrent mmap'd re-read of %s running against the "
                   "real connection (real page faults, real I/O — not just "
                   "CPU cycles) ===\n", g_io_stress_path);
        }
    }

    int realFailed = run_connection(host, (uint16_t)(basePort + priorConnections),
        mtu, capacity, warmupSizes, warmupCount, finalSize, "real");

    atomic_store_explicit(&g_stress_stop, 1, memory_order_relaxed);
    for (uint32_t i = 0; i < stressThreads; i++) pthread_join(stressTids[i], NULL);
    if (ioStarted) pthread_join(ioTid, NULL);

    if (realFailed) {
        fprintf(stderr, "MLX_COMPAT_MIXED_GATE FAIL: real connection failed\n");
        return 1;
    }
    printf("MLX_COMPAT_MIXED_GATE PASS: %u throwaway connections + 1 real "
           "connection (%u warmups replaying the exact size sequence + 1 "
           "exchange of %u bytes), all through libibverbs_compat, one process\n",
           priorConnections, warmupCount, finalSize);
    return 0;
}
