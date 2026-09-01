/* Phase 2 hardware gate for MlxRDMA.dext.
 *
 * This is a wire-compatible client for the stock rdma-core ibv_rc_pingpong
 * TCP handshake.  It validates the complete DEXT path: GID programming,
 * client-memory registration, QP RESET->INIT->RTR->RTS (with QUERY_QP after
 * every transition), kernel-mediated post_recv/post_send and CQ polling.
 */
#include "librdma_shim.h"
#include "librdma_shim_diag.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netdb.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define DEFAULT_PORT 18515
#define DEFAULT_ITERS 1000000u
#define DEFAULT_SIZE 1024u
#define WRID_RECV 1u
#define WRID_SEND 2u

struct destination {
    uint32_t qpn;
    uint32_t psn;
    uint8_t gid[16];
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

static uint64_t be64_to_host(uint64_t value)
{
    return host_to_be64(value);
}

static void usage(const char *name)
{
    fprintf(stderr,
        "usage: %s [options] <spark-host>\n"
        "      --preflight          open/query local DEXT only; no DGX needed\n"
        "  -p, --port N            TCP port (default 18515)\n"
        "  -n, --iters N           exchanges (default 1000000)\n"
        "  -s, --size N            bytes per message (default 1024)\n"
        "  -m, --mtu N             path MTU bytes: 256/512/1024/2048/4096\n"
        "      --rdma-read         run one-sided RDMA READ matrix\n"
        "      --local-ip ADDR     local IPv4/IPv6 (default 192.168.200.1)\n"
        "      --local-mac MAC     local MAC (default 98:03:9b:80:6a:94)\n"
        "      --remote-mac MAC    Spark MAC (default 4c:bb:47:7d:a1:a4)\n"
        "      --timeout SEC       no-progress timeout (default 30)\n"
        "      --window N          outstanding WRs per run, 1..256 (default 1)\n"
        "      --shared-cq         force one CQ for send and receive\n"
        "      --separate-cq       force distinct send and receive CQs\n"
        "      --signal-all        request a send CQE for every WR\n"
        "      --reverse-write      accept Spark WRITEs into this MR\n"
        "      --immediate          send/receive immediate data (one exchange)\n"
        "      --write-immediate    RDMA_WRITE_WITH_IMM plus immediate acknowledgement\n"
        "      --indirect-mr       back the buffer with an indirect (KLM)\n"
        "      env PHASE2_BIND_MW_GATE=1  run hardware type-2 BIND_MW gate\n"
        "                          (requires a remote peer connection)\n"
        "                          mkey composed from 4 direct child MRs\n"
        "                          instead of one direct MR (notes/43/44)\n"
        "                          instead of one direct MR (notes/43/44)\n"
        "      --warmup-size N      small SEND/RECV size to exchange --warmup-iters\n"
        "                          times on the live QP before the real --size\n"
        "                          exchange, using the same MR (notes/50)\n"
        "      --warmup-iters N     number of warmup exchanges (default 0 = off)\n"
        "\n"
        "      env MLX_GATE_ALLOW_MULTI_PACKET_SEND=1 lifts the --size <= --mtu\n"
        "      guard for plain SEND (write/reverse-write already allow it) -\n"
        "      diagnostic escape hatch, not part of the normal gate matrix.\n",
        name);
}

static int parse_mac(const char *text, uint8_t mac[6])
{
    unsigned int v[6];
    if (sscanf(text, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2],
               &v[3], &v[4], &v[5]) != 6)
        return -1;
    for (unsigned int i = 0; i < 6; i++) {
        if (v[i] > 255) return -1;
        mac[i] = (uint8_t)v[i];
    }
    return 0;
}

static int parse_gid(const char *text, uint8_t gid[16], uint8_t *l3Type)
{
    if (inet_pton(AF_INET6, text, gid) == 1) {
        *l3Type = 1;
        return 0;
    }
    uint8_t ip4[4];
    if (inet_pton(AF_INET, text, ip4) != 1) return -1;
    memset(gid, 0, 16);
    gid[10] = 0xff; gid[11] = 0xff;
    memcpy(gid + 12, ip4, 4);
    *l3Type = 0;
    return 0;
}

static void gid_to_wire(const uint8_t gid[16], char wire[33])
{
    for (unsigned int i = 0; i < 16; i++)
        snprintf(wire + i * 2, 3, "%02x", gid[i]);
}

static int wire_to_gid(const char *wire, uint8_t gid[16])
{
    for (unsigned int i = 0; i < 16; i++) {
        unsigned int byte = 0;
        if (sscanf(wire + i * 2, "%2x", &byte) != 1) return -1;
        gid[i] = (uint8_t)byte;
    }
    return 0;
}

static int write_full(int fd, const void *buffer, size_t length)
{
    const uint8_t *p = (const uint8_t *)buffer;
    while (length) {
        ssize_t n = write(fd, p, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; length -= (size_t)n;
    }
    return 0;
}

static int read_full(int fd, void *buffer, size_t length)
{
    uint8_t *p = (uint8_t *)buffer;
    while (length) {
        ssize_t n = read(fd, p, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; length -= (size_t)n;
    }
    return 0;
}

static double now_seconds(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

static int connect_until(int fd, const struct sockaddr *address,
                         socklen_t addressLength, double deadline)
{
    int oldFlags = fcntl(fd, F_GETFL, 0);
    if (oldFlags < 0 || fcntl(fd, F_SETFL, oldFlags | O_NONBLOCK) < 0)
        return -1;
    int rc = connect(fd, address, addressLength);
    if (rc && errno != EINPROGRESS) return -1;
    while (rc) {
        double remaining = deadline - now_seconds();
        if (remaining <= 0) { errno = ETIMEDOUT; return -1; }
        int waitMs = (int)(remaining * 1000.0);
        if (waitMs < 1) waitMs = 1;
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        rc = poll(&pfd, 1, waitMs);
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0) { if (!rc) errno = ETIMEDOUT; return -1; }
        int socketError = 0;
        socklen_t errorLength = sizeof(socketError);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socketError, &errorLength) < 0)
            return -1;
        if (socketError) { errno = socketError; return -1; }
        rc = 0;
    }
    return fcntl(fd, F_SETFL, oldFlags);
}

static int exchange_destination(const char *host, uint16_t port,
                                const struct destination *local,
                                struct destination *remote,
                                uint32_t timeoutSec, int *controlFd)
{
    char service[16];
    snprintf(service, sizeof(service), "%u", port);
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *result = NULL;
    int rc = getaddrinfo(host, service, &hints, &result);
    if (rc) { fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc)); return -1; }
    int fd = -1, savedError = ECONNREFUSED;
    double deadline = now_seconds() + timeoutSec;
    do {
        for (struct addrinfo *it = result; it; it = it->ai_next) {
            fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
            if (fd >= 0 && connect_until(fd, it->ai_addr, it->ai_addrlen,
                                         deadline) == 0)
                break;
            if (fd >= 0) close(fd);
            fd = -1;
            savedError = errno;
        }
        if (fd >= 0 || now_seconds() >= deadline) break;
        usleep(50000);
    } while (1);
    freeaddrinfo(result);
    if (fd < 0) {
        errno = savedError;
        perror("connect");
        return -1;
    }
    struct timeval socketTimeout = { .tv_sec = (time_t)timeoutSec, .tv_usec = 0 };
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &socketTimeout,
                     sizeof(socketTimeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &socketTimeout,
                     sizeof(socketTimeout));

    /* Includes the terminating NUL, exactly like rdma-core rc_pingpong. */
    char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"] = {};
    char gidWire[33] = {};
    gid_to_wire(local->gid, gidWire);
    snprintf(msg, sizeof(msg), "%04x:%06x:%06x:%s", 0, local->qpn,
             local->psn, gidWire);
    if (write_full(fd, msg, sizeof(msg)) || read_full(fd, msg, sizeof(msg))) {
        perror("destination exchange"); close(fd); return -1;
    }
    unsigned int lid = 0, qpn = 0, psn = 0;
    memset(gidWire, 0, sizeof(gidWire));
    if (sscanf(msg, "%x:%x:%x:%32s", &lid, &qpn, &psn, gidWire) != 4 ||
        wire_to_gid(gidWire, remote->gid)) {
        fprintf(stderr, "invalid destination reply: %s\n", msg);
        close(fd); return -1;
    }
    remote->qpn = qpn; remote->psn = psn;
    if (write_full(fd, "done", sizeof("done"))) {
        perror("destination ack"); close(fd); return -1;
    }
    if (controlFd) *controlFd = fd;
    else close(fd);
    return 0;
}

static uint32_t mtu_enum(uint32_t bytes)
{
    switch (bytes) {
    case 256: return 1; case 512: return 2; case 1024: return 3;
    case 2048: return 4; case 4096: return 5; default: return 0;
    }
}

static int verify_state(rdma_qp *qp, uint32_t expected, const char *name)
{
    uint32_t actual = UINT32_MAX;
    if (rdma_query_qp(qp, &actual) || actual != expected) {
        fprintf(stderr, "QUERY_QP after %s: expected=%u actual=%u\n",
                name, expected, actual);
        return -1;
    }
    printf("QP transition %-9s verified (state=%u)\n", name, actual);
    return 0;
}

static int post_recv_window(rdma_qp *qp, const struct rdma_sge *sge,
                            uint32_t count)
{
    struct rdma_recv_wr wr[RDMA_MAX_POST_BATCH] = {};
    for (uint32_t i = 0; i < count; i++) {
        wr[i].wr_id = WRID_RECV;
        wr[i].num_sge = 1;
        wr[i].sg_list = sge;
    }
    return rdma_post_recv_batch(qp, wr, count);
}

static int post_read_window(rdma_qp *qp, const struct rdma_sge *sge,
                            uint32_t count, int signalAll, uint64_t remoteAddr, uint32_t rkey)
{
    struct rdma_send_wr wr[RDMA_MAX_POST_BATCH] = {};
    for (uint32_t i = 0; i < count; i++) {
        wr[i].wr_id = WRID_SEND; wr[i].opcode = RDMA_WR_RDMA_READ;
        wr[i].num_sge = 1; wr[i].sg_list = sge;
        wr[i].send_flags = (signalAll || i + 1 == count) ? RDMA_SEND_SIGNALED : 0;
        wr[i].remote_addr = remoteAddr; wr[i].rkey = rkey;
    }
    return rdma_post_send_batch(qp, wr, count);
}

static int post_send_window(rdma_qp *qp, const struct rdma_sge *sge,
                            uint32_t count, int signalAll, uint32_t opcode,
                            uint64_t remoteAddr, uint32_t rkey)
{
    struct rdma_send_wr wr[RDMA_MAX_POST_BATCH] = {};
    for (uint32_t i = 0; i < count; i++) {
        wr[i].wr_id = WRID_SEND;
        wr[i].opcode = opcode;
        wr[i].num_sge = 1;
        wr[i].sg_list = sge;
        wr[i].send_flags = (signalAll || i + 1 == count) ?
                           RDMA_SEND_SIGNALED : 0;
        wr[i].remote_addr = remoteAddr;
        wr[i].rkey = rkey;
    }
    return rdma_post_send_batch(qp, wr, count);
}

int main(int argc, char **argv)
{
    uint16_t tcpPort = DEFAULT_PORT;
    uint32_t iters = DEFAULT_ITERS, size = DEFAULT_SIZE, mtuBytes = 1024;
    uint32_t timeoutSec = 30, window = 1;
    const char *localIp = "192.168.200.1";
    const char *localMacText = "98:03:9b:80:6a:94";
    const char *remoteMacText = "4c:bb:47:7d:a1:a4";
    int preflight = 0, cqMode = -1, signalAll = 0, writeMode = 0;
    int reverseMode = 0, readMode = getenv("P0_READ_MODE") != NULL, readFromPeer = 0, immediateMode = 0, writeImmediateMode = 0;
    int indirectMr = 0;
    int bindMwGate = getenv("PHASE2_BIND_MW_GATE") != NULL;
    int localInvGate = getenv("PHASE2_LOCAL_INV_GATE") != NULL;
    const char *startFile = NULL, *readyFile = NULL;
    uint32_t warmupSize = 0, warmupIters = 0;
    enum { OPT_LOCAL_IP = 1000, OPT_LOCAL_MAC, OPT_REMOTE_MAC, OPT_TIMEOUT,
           OPT_WINDOW, OPT_PREFLIGHT, OPT_SHARED_CQ, OPT_SEPARATE_CQ,
           OPT_SIGNAL_ALL, OPT_RDMA_WRITE, OPT_REVERSE_WRITE, OPT_IMMEDIATE,
           OPT_WRITE_IMMEDIATE, OPT_START_FILE, OPT_READY_FILE, OPT_INDIRECT_MR,
           OPT_WARMUP_SIZE, OPT_WARMUP_ITERS, OPT_RDMA_READ, OPT_READ_FROM_PEER };
    static const struct option options[] = {
        {"port", required_argument, NULL, 'p'}, {"iters", required_argument, NULL, 'n'},
        {"size", required_argument, NULL, 's'}, {"mtu", required_argument, NULL, 'm'},
        {"local-ip", required_argument, NULL, OPT_LOCAL_IP},
        {"local-mac", required_argument, NULL, OPT_LOCAL_MAC},
        {"remote-mac", required_argument, NULL, OPT_REMOTE_MAC},
        {"timeout", required_argument, NULL, OPT_TIMEOUT},
        {"window", required_argument, NULL, OPT_WINDOW},
        {"shared-cq", no_argument, NULL, OPT_SHARED_CQ},
        {"separate-cq", no_argument, NULL, OPT_SEPARATE_CQ},
        {"signal-all", no_argument, NULL, OPT_SIGNAL_ALL},
        {"rdma-write", no_argument, NULL, OPT_RDMA_WRITE},
        {"rdma-read", no_argument, NULL, OPT_RDMA_READ},
        {"read-from-peer", no_argument, NULL, OPT_READ_FROM_PEER},
        {"reverse-write", no_argument, NULL, OPT_REVERSE_WRITE},
        {"immediate", no_argument, NULL, OPT_IMMEDIATE},
        {"write-immediate", no_argument, NULL, OPT_WRITE_IMMEDIATE},
        {"start-file", required_argument, NULL, OPT_START_FILE},
        {"ready-file", required_argument, NULL, OPT_READY_FILE},
        {"preflight", no_argument, NULL, OPT_PREFLIGHT},
        /* Diagnostic for notes/50: several small bidirectional SEND/RECV
         * exchanges on the live QP before the real --size exchange, using
         * the same MR the whole time (matching ggml-rpc's transport.cpp:
         * one MR sized for the max chunk, small early sends only referencing
         * a short prefix of it). Isolates whether a small-to-large size
         * transition on an already-active QP is what triggers the failure,
         * independent of llama.cpp and its RPC content. */
        {"warmup-size", required_argument, NULL, OPT_WARMUP_SIZE},
        {"warmup-iters", required_argument, NULL, OPT_WARMUP_ITERS},
        /* Registers the client buffer as MLX_INDIRECT_MR_CHILDREN separate
         * direct MRs composed under one indirect (KLM) mkey instead of one
         * direct MR, and uses that composed mkey for every SGE — see
         * notes/43/44 and Sources/hw/MlxP0EncodingIndirect.hpp. */
        {"indirect-mr", no_argument, NULL, OPT_INDIRECT_MR},
        {NULL, 0, NULL, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "p:n:s:m:", options, NULL)) != -1) {
        switch (opt) {
        case 'p': tcpPort = (uint16_t)strtoul(optarg, NULL, 0); break;
        case 'n': iters = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 's': size = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 'm': mtuBytes = (uint32_t)strtoul(optarg, NULL, 0); break;
        case OPT_LOCAL_IP: localIp = optarg; break;
        case OPT_LOCAL_MAC: localMacText = optarg; break;
        case OPT_REMOTE_MAC: remoteMacText = optarg; break;
        case OPT_TIMEOUT: timeoutSec = (uint32_t)strtoul(optarg, NULL, 0); break;
        case OPT_WINDOW: window = (uint32_t)strtoul(optarg, NULL, 0); break;
        case OPT_SHARED_CQ: cqMode = 0; break;
        case OPT_SEPARATE_CQ: cqMode = 1; break;
        case OPT_SIGNAL_ALL: signalAll = 1; break;
        case OPT_RDMA_WRITE: writeMode = 1; break;
        case OPT_RDMA_READ: readMode = 1; break;
        case OPT_READ_FROM_PEER: readFromPeer = 1; break;
        case OPT_REVERSE_WRITE: reverseMode = 1; break;
        case OPT_IMMEDIATE: immediateMode = 1; break;
        case OPT_WRITE_IMMEDIATE: writeImmediateMode = 1; break;
        case OPT_START_FILE: startFile = optarg; break;
        case OPT_READY_FILE: readyFile = optarg; break;
        case OPT_PREFLIGHT: preflight = 1; break;
        case OPT_INDIRECT_MR: indirectMr = 1; break;
        case OPT_WARMUP_SIZE: warmupSize = (uint32_t)strtoul(optarg, NULL, 0); break;
        case OPT_WARMUP_ITERS: warmupIters = (uint32_t)strtoul(optarg, NULL, 0); break;
        default: usage(argv[0]); return 2;
        }
    }
    if (preflight) {
        if (optind != argc) { usage(argv[0]); return 2; }
        rdma_device *probe = rdma_open_device();
        if (!probe) {
            fprintf(stderr, "PHASE2_PREFLIGHT FAIL: cannot open MlxRDMA user client\n");
            return 10;
        }
        struct rdma_device_attr deviceAttr = {};
        struct rdma_port_attr portAttr = {};
        struct rdma_health_attr healthAttr = {};
        struct rdma_abi_attr abiAttr = {};
        int abiRc = rdma_query_abi(probe, &abiAttr);
        if (abiRc || !(abiAttr.features & RDMA_FEATURE_HEALTH_QUERY)) {
            fprintf(stderr, "PHASE2_PREFLIGHT FAIL: DEXT lacks health-query feature "
                    "(abi_rc=%d version=%u features=0x%x) - activate fresh build\n",
                    abiRc, abiAttr.version, abiAttr.features);
            rdma_close_device(probe);
            return 14;
        }
        int deviceRc = rdma_query_device(probe, &deviceAttr);
        if (deviceRc) {
            rdma_close_device(probe);
            fprintf(stderr, "PHASE2_PREFLIGHT FAIL: QUERY_DEVICE rc=%d\n", deviceRc);
            return 11;
        }
        int portRc = rdma_query_port(probe, &portAttr);
        int healthRc = rdma_query_health(probe, &healthAttr);
        if (portRc) {
            rdma_close_device(probe);
            fprintf(stderr, "PHASE2_PREFLIGHT FAIL: QUERY_PORT rc=%d\n", portRc);
            return 12;
        }
        rdma_close_device(probe);
        if (healthRc || !healthAttr.healthy) {
            fprintf(stderr, "PHASE2_PREFLIGHT FAIL: health healthy=%u syndrome=0x%x ext=0x%x rc=%d\n",
                    healthAttr.healthy, healthAttr.syndrome,
                    healthAttr.ext_syndrome, healthRc);
            return 15;
        }
        if (!portAttr.port_state) {
            fprintf(stderr, "PHASE2_PREFLIGHT WAIT: port is DOWN\n");
            return 13;
        }
        printf("PHASE2_PREFLIGHT PASS: user client opened, port UP, health OK, max_qp=%u max_cq=%u max_mr=%u max_msg_size=%u\n",
               deviceAttr.max_qp, deviceAttr.max_cq, deviceAttr.max_mr,
               deviceAttr.max_msg_size);
        return 0;
    }
    if (optind + 1 != argc || !iters || !size ||
        (!writeMode && !reverseMode && size > mtuBytes && !getenv("MLX_GATE_ALLOW_MULTI_PACKET_SEND")) ||
        ((immediateMode || writeImmediateMode) &&
         (writeMode || reverseMode || window != 1)) ||
        !mtu_enum(mtuBytes) || !timeoutSec || !window ||
        window > RDMA_MAX_POST_BATCH ||
        (warmupIters && (!warmupSize || warmupSize > size || writeMode || reverseMode))) {
        usage(argv[0]); return 2;
    }
    const char *host = argv[optind];
    uint8_t localGid[16], localMac[6], remoteMac[6], localL3Type = 0;
    if (parse_gid(localIp, localGid, &localL3Type) ||
        parse_mac(localMacText, localMac) ||
        parse_mac(remoteMacText, remoteMac)) {
        fprintf(stderr, "invalid IP or MAC argument\n"); return 2;
    }

    int result = 1, controlFd = -1;
    rdma_device *dev = NULL; rdma_pd *pd = NULL;
    rdma_cq *sendCq = NULL, *recvCq = NULL;
    rdma_qp *qp = NULL; struct rdma_mr *mr = NULL; rdma_mw *mw = NULL;
#define MLX_INDIRECT_MR_CHILDREN 4
    struct rdma_mr *indirectChildren[MLX_INDIRECT_MR_CHILDREN] = {NULL};
    uint32_t indirectChildCount = 0;
    void *allocation = NULL; uint8_t *buffer = NULL;
    uint32_t gidIndex = 0;
    int gidProgrammed = 0;
    int fastPathEnabled = 0;
    struct rdma_fast_path fastPath = {};
    int directRequested = getenv("MELONDMA_DIRECT_UAR") &&
                          strcmp(getenv("MELONDMA_DIRECT_UAR"), "0") != 0;
    dev = rdma_open_device();
    if (!dev) { fprintf(stderr, "cannot open MlxRDMA user client\n"); goto out; }
    struct rdma_port_attr portAttr = {};
    if (rdma_query_port(dev, &portAttr) || !portAttr.port_state) {
        fprintf(stderr, "RoCE port is not UP\n"); goto out;
    }
    if (rdma_set_roce_address(dev, localGid, localMac, localL3Type, &gidIndex)) {
        fprintf(stderr, "SET/QUERY_ROCE_ADDRESS strict readback failed\n"); goto out;
    }
    gidProgrammed = 1;
    struct rdma_gid_attr programmedGid = {};
    if (rdma_query_gid(dev, gidIndex, &programmedGid) ||
        memcmp(programmedGid.gid, localGid, sizeof(localGid)) ||
        memcmp(programmedGid.mac, localMac, sizeof(localMac)) ||
        programmedGid.l3_type != localL3Type) {
        fprintf(stderr, "firmware-backed GID query mismatch\n"); goto out;
    }
    printf("RoCE address programmed and queried (gid_index=%u)\n", gidIndex);
    pd = rdma_alloc_pd(dev);
    if (directRequested) {
        int enableRc = rdma_enable_fast_path(dev, &fastPath);
        if (enableRc) {
            fprintf(stderr, "direct-UAR enable failed rc=%d (PD must exist first)\n", enableRc);
            goto out;
        }
        int mapRc = rdma_map_fast_path(dev, &fastPath);
        if (mapRc) {
            fprintf(stderr, "direct-UAR map failed rc=%d\n", mapRc);
            goto out;
        }
        fastPathEnabled = 1;
        printf("DIRECT_UAR enabled: per-client UAR/DB mapped\n");
    }
    uint32_t cqEntries = window > 64 ? 256 : 64;
    sendCq = rdma_create_cq(dev, cqEntries);
    int separateCq = cqMode >= 0 ? cqMode : window > 1;
    recvCq = separateCq && !writeMode ? rdma_create_cq(dev, cqEntries) : sendCq;
    if (!pd || !sendCq || !recvCq) {
        fprintf(stderr, "PD/CQ allocation failed\n"); goto out;
    }
    uint32_t mrCapacity = size;
    const char *mrCapacityEnv = getenv("MLX_GATE_MR_CAPACITY");
    if (mrCapacityEnv && *mrCapacityEnv) {
        mrCapacity = (uint32_t)strtoul(mrCapacityEnv, NULL, 0);
        if (mrCapacity < size) {
            fprintf(stderr, "MLX_GATE_MR_CAPACITY must be >= payload size\n");
            goto out;
        }
    }
    if (posix_memalign(&allocation, 4096, (size_t)mrCapacity + 128)) allocation = NULL;
    if (!allocation) { fprintf(stderr, "buffer allocation failed\n"); goto out; }
    memset(allocation, 0xa5, (size_t)mrCapacity + 128);
    uint32_t bufferOffset = getenv("MLX_GATE_PAGE_ALIGNED_MR") ? 0u : 64u;
    buffer = (uint8_t *)allocation + bufferOffset;
    /* Stock rc_pingpong initializes its buffer to 0x7b.  Using the same
     * value on both ends lets every receive validate payload corruption. */
    memset(buffer, reverseMode ? 0xa7 : 0x7b, size);
    struct rdma_mr_attr_resp mrInfo = {};
    if (indirectMr) {
        if (size % MLX_INDIRECT_MR_CHILDREN != 0) {
            fprintf(stderr, "--indirect-mr requires --size divisible by %d (got %u)\n",
                    MLX_INDIRECT_MR_CHILDREN, size);
            goto out;
        }
        uint32_t chunk = size / MLX_INDIRECT_MR_CHILDREN;
        for (uint32_t i = 0; i < MLX_INDIRECT_MR_CHILDREN; i++) {
            struct rdma_mr_attr_resp childInfo = {};
            struct rdma_mr *child = rdma_reg_mr(pd, buffer + i * chunk, chunk,
                                                RDMA_ACCESS_LOCAL_WRITE, &childInfo);
            if (!child) {
                fprintf(stderr, "indirect-mr: child %u/%d registration failed\n",
                        i, MLX_INDIRECT_MR_CHILDREN);
                goto out;
            }
            indirectChildren[indirectChildCount++] = child;
            printf("Indirect child MR[%u] registered: addr=%p bytes=%u lkey=0x%x\n",
                   i, (void *)(buffer + i * chunk), chunk, childInfo.lkey);
        }
        mr = rdma_reg_mr_indirect(pd, indirectChildren, MLX_INDIRECT_MR_CHILDREN,
                                  (uint64_t)(uintptr_t)buffer, size,
                                  RDMA_ACCESS_LOCAL_WRITE, &mrInfo);
        if (!mr) { fprintf(stderr, "indirect MR composition failed\n"); goto out; }
        printf("Indirect MR composed: lkey=0x%x rkey=0x%x bytes=%u children=%d\n",
               mrInfo.lkey, mrInfo.rkey, size, MLX_INDIRECT_MR_CHILDREN);
    } else {
        mr = rdma_reg_mr(pd, buffer, mrCapacity,
                          RDMA_ACCESS_LOCAL_WRITE |
                          ((writeMode || reverseMode) ? RDMA_ACCESS_REMOTE_WRITE : 0) |
                          (readFromPeer ? RDMA_ACCESS_REMOTE_READ : 0) |
                          (bindMwGate ? (1u << 4) : 0), &mrInfo);
        if (!mr) { fprintf(stderr, "client MR registration failed\n"); goto out; }
        printf("Client MR registered: lkey=0x%x rkey=0x%x bytes=%u payload=%u\n",
               mrInfo.lkey, mrInfo.rkey, mrCapacity, size);
    }

    uint32_t qpDepth = 256;
    const char *qpDepthEnv = getenv("MLX_GATE_QP_DEPTH");
    if (qpDepthEnv && *qpDepthEnv) {
        qpDepth = (uint32_t)strtoul(qpDepthEnv, NULL, 0);
        if (qpDepth < 64 || (qpDepth & (qpDepth - 1)) != 0) {
            fprintf(stderr, "invalid MLX_GATE_QP_DEPTH=%s\n", qpDepthEnv);
            goto out;
        }
    }
    struct rdma_qp_init_attr init = {
        .send_cq = sendCq, .recv_cq = recvCq, .qp_type = RDMA_QPT_RC,
        .cap_sq = qpDepth, .cap_rq = qpDepth, .max_inline_data = 0
    };
    qp = rdma_create_qp(pd, &init);
    if (!qp) goto out;
    struct rdma_qp_attr attr = {
        .cur_state = RDMA_QPS_RESET, .new_state = RDMA_QPS_INIT,
        .pkey_index = 0, .port_num = 1
    };
    if (rdma_modify_qp(qp, &attr) || verify_state(qp, RDMA_QPS_INIT, "RST->INIT"))
        goto out;

    struct rdma_sge sge = { .lkey = mrInfo.lkey, .addr = (uint64_t)(uintptr_t)buffer,
                            .length = size };
    int multiSgeGate = getenv("PHASE2_MULTI_SGE_GATE") != NULL;
    uint32_t multiSgeCount = 2;
    const char *multiSgeEnv = getenv("PHASE2_MULTI_SGE_COUNT");
    if (multiSgeEnv) multiSgeCount = (uint32_t)strtoul(multiSgeEnv, NULL, 0);
    struct rdma_sge multiSge[RDMA_MAX_SGE] = {};
    if (multiSgeGate) {
        if (writeMode || reverseMode || window != 1 || size < multiSgeCount ||
            multiSgeCount < 2 || multiSgeCount > RDMA_MAX_SGE) {
            fprintf(stderr, "PHASE2_MULTI_SGE_GATE requires 2-4 SGE, SEND/RECV, window=1 and size>=count\n");
            goto out;
        }
        uint32_t offset = 0;
        for (uint32_t i = 0; i < multiSgeCount; i++) {
            uint32_t remaining = size - offset;
            uint32_t parts = multiSgeCount - i;
            multiSge[i] = sge;
            multiSge[i].addr += offset;
            multiSge[i].length = remaining / parts;
            offset += multiSge[i].length;
        }
    }
    uint32_t firstWindow = iters < window ? iters : window;
    if (!getenv("PHASE2_RNR_GATE") && !writeMode && !readMode && !reverseMode &&
        !multiSgeGate && !immediateMode && !writeImmediateMode &&
        post_recv_window(qp, &sge, firstWindow)) {
        fprintf(stderr, "initial post_recv window failed\n"); goto out;
    }
    struct destination local = { .qpn = rdma_qp_number(qp),
        .psn = ((uint32_t)getpid() * 2654435761u) & 0xffffff };
    if (getenv("MLX_GATE_QPN_PSN")) local.psn = local.qpn & 0xffffff;
    memcpy(local.gid, localGid, 16);
    struct destination remote = {};
    if (exchange_destination(host, tcpPort, &local, &remote, timeoutSec,
                             (writeMode || reverseMode || writeImmediateMode || bindMwGate || readMode || getenv("PHASE2_RNR_GATE")) ? &controlFd : NULL)) goto out;
    printf("Peer: qpn=0x%06x psn=0x%06x\n", remote.qpn, remote.psn);

    memset(&attr, 0, sizeof(attr));
    attr.cur_state = RDMA_QPS_INIT; attr.new_state = RDMA_QPS_RTR;
    attr.dest_qpn = remote.qpn; attr.path_mtu = mtu_enum(mtuBytes);
    attr.rq_psn = remote.psn; attr.pkey_index = 0; attr.port_num = 1;
    memcpy(attr.ah_dmac, remoteMac, 6); memcpy(attr.ah_dgid, remote.gid, 16);
    attr.ah_sgid_index = gidIndex; attr.ah_hop_limit = 1;
    /* Zero asks the driver to use the common Linux rdma_get_udp_sport()
     * symmetric QPN/flow-label convention. */
    attr.ah_udp_sport = 0;
    attr.min_rnr_timer = getenv("MLX_GATE_MIN_RNR_ONE") ? 1 : 12;
    attr.max_dest_rd_atomic = 1;
    if (rdma_modify_qp(qp, &attr) || verify_state(qp, RDMA_QPS_RTR, "INIT->RTR"))
        goto out;

    memset(&attr, 0, sizeof(attr));
    attr.cur_state = RDMA_QPS_RTR; attr.new_state = RDMA_QPS_RTS;
    attr.sq_psn = local.psn; attr.max_rd_atomic = 1;
    attr.timeout = getenv("PHASE2_RNR_GATE") ? 1 : 14;
    /* Keep transport retries enabled; this gate isolates RNR retry
     * exhaustion, as required by the verbs contract. */
    attr.retry_cnt = 7;
    attr.rnr_retry = getenv("PHASE2_RNR_GATE") ? 0 : 7;
    if (rdma_modify_qp(qp, &attr) || verify_state(qp, RDMA_QPS_RTS, "RTR->RTS"))
        goto out;

    uint64_t remoteAddr = 0;
    uint32_t remoteRkey = 0;
    if (bindMwGate) {
        if (controlFd < 0) { fprintf(stderr, "MW gate requires control channel\n"); goto out; }
        mw = rdma_alloc_mw(pd, 2);
        if (!mw) { fprintf(stderr, "PHASE2_BIND_MW_GATE alloc_mw failed\n"); goto out; }
        uint32_t oldRkey = rdma_mw_rkey(mw), newRkey = 0;
        const uint64_t bindWrid = 0x42494e4400000001ull;
        if (rdma_bind_mw(qp, mw, mr, (uint64_t)(uintptr_t)buffer, size,
                         RDMA_ACCESS_REMOTE_WRITE, RDMA_SEND_SIGNALED,
                         bindWrid, &newRkey)) {
            fprintf(stderr, "PHASE2_BIND_MW_GATE bind_mw post failed\n"); goto out;
        }
        if (!newRkey || newRkey == oldRkey) {
            fprintf(stderr, "PHASE2_BIND_MW_GATE rkey did not rotate old=0x%x new=0x%x\n", oldRkey, newRkey); goto out;
        }
        int bindOk = 0;
        double bindDeadline = now_seconds() + timeoutSec;
        while (!bindOk && now_seconds() <= bindDeadline) {
            struct rdma_wc wc[4] = {};
            int n = rdma_poll_cq(sendCq, wc, 4);
            if (n < 0) { fprintf(stderr, "PHASE2_BIND_MW_GATE poll failed\n"); goto out; }
            for (int i = 0; i < n; i++) if (wc[i].wr_id == bindWrid) {
                if (wc[i].status != RDMA_WC_SUCCESS || wc[i].opcode != RDMA_WC_UMR_KLM) {
                    fprintf(stderr, "PHASE2_BIND_MW_GATE CQE status=%u opcode=%u vendor=0x%x\n", wc[i].status, wc[i].opcode, wc[i].vendor_err); goto out;
                }
                bindOk = 1;
            }
            if (!n) usleep(50);
        }
        if (!bindOk) { fprintf(stderr, "PHASE2_BIND_MW_GATE CQE timeout\n"); goto out; }
        printf("PHASE2_BIND_MW PASS: hardware UMR bind CQE received old_rkey=0x%x new_rkey=0x%x\n", oldRkey, newRkey);
    }
    if (getenv("PHASE2_BIND_MW_ROTATE_GATE") && !getenv("PHASE2_MW_INTEROP_GATE")) {
        if (!mw) { fprintf(stderr, "BIND_MW rotation requires initial bind\n"); goto out; }
        uint32_t rkey1 = rdma_mw_rkey(mw), rkey2 = 0;
        if (rdma_post_local_inv(qp, 0x42494e5600000001ull, rkey1)) {
            fprintf(stderr, "PHASE2_BIND_MW_ROTATE_GATE local invalidation post failed rkey=0x%x\n", rkey1); goto out;
        }
        int invOk = 0;
        double invDeadline = now_seconds() + timeoutSec;
        while (!invOk && now_seconds() <= invDeadline) {
            struct rdma_wc wc[2] = {};
            int n = rdma_poll_cq(sendCq, wc, 2);
            if (n < 0) goto out;
            for (int i = 0; i < n; i++) if (wc[i].wr_id == 0x42494e5600000001ull) {
                if (wc[i].status != RDMA_WC_SUCCESS) { fprintf(stderr, "PHASE2_BIND_MW_ROTATE_GATE invalidation status=%u vendor=0x%x\n", wc[i].status, wc[i].vendor_err); goto out; }
                invOk = 1;
            }
            if (!n) usleep(50);
        }
        if (!invOk) { fprintf(stderr, "PHASE2_BIND_MW_ROTATE_GATE invalidation CQE timeout\n"); goto out; }
        const uint64_t rotateWrid = 0x42494e4400000002ull;
        printf("PHASE2_BIND_MW_ROTATE: invalidation complete, rebinding...\n");
        int rotatePostRc = rdma_bind_mw(qp, mw, mr, (uint64_t)(uintptr_t)buffer, size,
                         RDMA_ACCESS_REMOTE_WRITE, RDMA_SEND_SIGNALED,
                         rotateWrid, &rkey2);
        if (rotatePostRc || rkey2 == rkey1) {
            fprintf(stderr, "PHASE2_BIND_MW_ROTATE_GATE second bind failed rc=%d or rkey unchanged old=0x%x new=0x%x\n",
                    rotatePostRc, rkey1, rkey2); goto out;
        }
        int rotateOk = 0;
        double rotateDeadline = now_seconds() + timeoutSec;
        while (!rotateOk && now_seconds() <= rotateDeadline) {
            struct rdma_wc wc[2] = {};
            int n = rdma_poll_cq(sendCq, wc, 2);
            if (n < 0) { fprintf(stderr, "PHASE2_BIND_MW_ROTATE_GATE poll rc=%d\n", n); goto out; }
            for (int i = 0; i < n; i++) if (wc[i].wr_id == rotateWrid) {
                if (wc[i].status != RDMA_WC_SUCCESS) {
                    fprintf(stderr, "PHASE2_BIND_MW_ROTATE_GATE CQE status=%u opcode=%u vendor=0x%x\n",
                            wc[i].status, wc[i].opcode, wc[i].vendor_err); goto out;
                }
                rotateOk = 1;
            }
            if (!n) usleep(50);
        }
        if (!rotateOk) { fprintf(stderr, "PHASE2_BIND_MW_ROTATE_GATE CQE timeout rkey1=0x%x rkey2=0x%x\n", rkey1, rkey2); goto out; }
        printf("PHASE2_BIND_MW_ROTATE PASS: invalidated rkey1=0x%x, rebound rkey2=0x%x\n", rkey1, rkey2);
    }
    if (getenv("PHASE2_MW_INTEROP_GATE")) {
        struct remote_memory_wire mwMemory = {
            .address_be = host_to_be64((uint64_t)(uintptr_t)buffer),
            .rkey_be = htonl(rdma_mw_rkey(mw)), .length_be = htonl(size) };
        char token[sizeof("pass")] = {};
        if (write_full(controlFd, &mwMemory, sizeof(mwMemory)) ||
            read_full(controlFd, token, sizeof(token))) {
            fprintf(stderr, "PHASE2_MW_INTEROP_GATE valid rkey remote WRITE exchange failed\n"); goto out;
        }
        if (memcmp(token, "pass", sizeof(token))) {
            fprintf(stderr, "PHASE2_MW_INTEROP_GATE valid rkey rejected by peer token=%s\n", token); goto out;
        }
        printf("PHASE2_MW_INTEROP valid rkey remote WRITE PASS rkey=0x%x\n", rdma_mw_rkey(mw));
        uint32_t staleRkey = rdma_mw_rkey(mw);
        mwMemory.rkey_be = htonl(staleRkey);
        if (rdma_post_local_inv(qp, 0x42494e5600000001ull, staleRkey)) {
            fprintf(stderr, "PHASE2_MW_INTEROP_GATE LOCAL_INV failed\n"); goto out;
        }
        int invOk = 0; double invDeadline = now_seconds() + timeoutSec;
        while (!invOk && now_seconds() <= invDeadline) {
            struct rdma_wc wc[2] = {}; int n = rdma_poll_cq(sendCq, wc, 2);
            if (n < 0) goto out;
            for (int i = 0; i < n; i++) if (wc[i].wr_id == 0x42494e5600000001ull) {
                if (wc[i].status != RDMA_WC_SUCCESS) goto out; invOk = 1;
            }
            if (!n) usleep(50);
        }
        if (!invOk) { fprintf(stderr, "PHASE2_MW_INTEROP_GATE LOCAL_INV timeout\n"); goto out; }
        printf("PHASE2_MW_INTEROP LOCAL_INV PASS rkey=0x%x\n", staleRkey);
        uint32_t rebound = 0;
        /* A rejected remote access moves an RC QP to ERR, so rebind and
         * validate the new key before deliberately probing the stale key. */
        int reboundPostRc = rdma_bind_mw(qp, mw, mr, (uint64_t)(uintptr_t)buffer, size,
                         RDMA_ACCESS_REMOTE_WRITE, RDMA_SEND_SIGNALED,
                         0x42494e4400000002ull, &rebound);
        if (reboundPostRc) {
            fprintf(stderr, "PHASE2_MW_INTEROP rebound bind post failed rc=%d\n", reboundPostRc); goto out;
        }
        int bind2Ok = 0; double bind2Deadline = now_seconds() + timeoutSec;
        while (!bind2Ok && now_seconds() <= bind2Deadline) {
            struct rdma_wc wc[2] = {}; int n = rdma_poll_cq(sendCq, wc, 2);
            if (n < 0) goto out;
            for (int i = 0; i < n; i++) if (wc[i].wr_id == 0x42494e4400000002ull) {
                if (wc[i].status != RDMA_WC_SUCCESS) {
                    fprintf(stderr, "PHASE2_MW_INTEROP rebound bind CQE failed status=%u opcode=%u vendor=0x%x\n", wc[i].status, wc[i].opcode, wc[i].vendor_err); goto out;
                }
                bind2Ok = 1;
            }
            if (!n) usleep(50);
        }
        if (!bind2Ok || rebound == staleRkey) {
            fprintf(stderr, "PHASE2_MW_INTEROP rebound bind timeout or unchanged rkey old=0x%x new=0x%x\n", staleRkey, rebound); goto out;
        }
        mwMemory.rkey_be = htonl(rebound);
        if (write_full(controlFd, &mwMemory, sizeof(mwMemory)) || read_full(controlFd, token, sizeof(token)) || memcmp(token, "pass", sizeof(token))) goto out;
        printf("PHASE2_MW_INTEROP rebound valid remote WRITE PASS rkey=0x%x\n", rebound);
        mwMemory.rkey_be = htonl(staleRkey);
        if (write_full(controlFd, &mwMemory, sizeof(mwMemory)) || read_full(controlFd, token, sizeof(token))) goto out;
        if (memcmp(token, "fail", sizeof(token))) {
            fprintf(stderr, "PHASE2_MW_INTEROP_GATE stale rkey was accepted token=%s\n", token); goto out;
        }
        printf("PHASE2_MW_INTEROP PASS: valid write, LOCAL_INV, rebound write, stale rejection rkey=0x%x\n", staleRkey);
        /* The final stale remote WRITE intentionally produces an error CQE
         * and transitions this RC QP to ERR. Do not enter the ordinary
         * SEND/RECV loop after this expected negative probe. */
        result = 0;
        goto out;
    }
    if (localInvGate) {
        if (rdma_post_local_inv(qp, WRID_SEND, mrInfo.rkey)) {
            fprintf(stderr, "PHASE2_LOCAL_INV_GATE post failed\n"); goto out;
        }
        int invOk = 0;
        double deadline = now_seconds() + timeoutSec;
        while (!invOk && now_seconds() <= deadline) {
            struct rdma_wc wc[2] = {};
            int count = rdma_poll_cq(sendCq, wc, 2);
            if (count < 0) { fprintf(stderr, "PHASE2_LOCAL_INV_GATE poll failed\n"); goto out; }
            for (int i = 0; i < count; i++) {
                if (wc[i].wr_id == WRID_SEND) {
                    if (wc[i].status != RDMA_WC_SUCCESS) {
                        fprintf(stderr, "PHASE2_LOCAL_INV_GATE CQE status=%u vendor=0x%x\n",
                                wc[i].status, wc[i].vendor_err); goto out;
                    }
                    invOk = 1;
                }
            }
        }
        if (!invOk) { fprintf(stderr, "PHASE2_LOCAL_INV_GATE timeout\n"); goto out; }
        struct rdma_send_wr stale = { .wr_id = WRID_SEND, .opcode = RDMA_WR_SEND,
                                       .num_sge = 1, .sg_list = &sge,
                                       .send_flags = RDMA_SEND_SIGNALED };
        sge.lkey = mrInfo.rkey;
        if (rdma_post_send_sge(qp, &stale) == 0) {
            fprintf(stderr, "PHASE2_LOCAL_INV_GATE stale rkey accepted\n"); goto out;
        }
        printf("PHASE2_LOCAL_INV PASS: CQE success and stale lkey rejected\n");
        result = 0;
        goto out;
    }
    if (immediateMode || writeImmediateMode) {
        const uint32_t immediateValue = 0x12345678;
        if (writeImmediateMode) {
            struct remote_memory_wire memory = {};
            if (read_full(controlFd, &memory, sizeof(memory))) {
                fprintf(stderr, "write-immediate remote memory exchange failed\n"); goto out;
            }
            remoteAddr = be64_to_host(memory.address_be);
            remoteRkey = ntohl(memory.rkey_be);
            if (!remoteAddr || !remoteRkey || ntohl(memory.length_be) < size) {
                fprintf(stderr, "write-immediate invalid remote memory\n"); goto out;
            }
        }
        struct rdma_send_wr sendWr = { .wr_id = WRID_SEND,
                                       .opcode = writeImmediateMode ? RDMA_WR_RDMA_WRITE_IMM : RDMA_WR_SEND_IMM,
                                       .num_sge = 1, .sg_list = &sge,
                                       .remote_addr = remoteAddr, .rkey = remoteRkey,
                                       .send_flags = RDMA_SEND_SIGNALED,
                                       .imm_data = htonl(immediateValue) };
        int recvRc = rdma_post_recv(qp, WRID_RECV, &sge, 1);
        int sendRc = recvRc ? 0 : rdma_post_send_sge(qp, &sendWr);
        if (recvRc || sendRc) {
            fprintf(stderr, "PHASE2_IMMEDIATE_GATE post failed recv_rc=%d send_rc=%d\n",
                    recvRc, sendRc); goto out;
        }
        int sendOk = 0, recvOk = 0;
        double deadline = now_seconds() + timeoutSec;
        while (!sendOk || !recvOk) {
            struct rdma_wc wc[2] = {};
            int count = rdma_poll_cq(sendCq, wc, 2);
            if (count < 0) { fprintf(stderr, "PHASE2_IMMEDIATE_GATE poll failed\n"); goto out; }
            for (int i = 0; i < count; i++) {
                if (wc[i].status != RDMA_WC_SUCCESS) {
                    fprintf(stderr, "PHASE2_IMMEDIATE_GATE CQE status=%u vendor=0x%x\n", wc[i].status, wc[i].vendor_err); goto out;
                }
                if (wc[i].wr_id == WRID_SEND) sendOk = 1;
                if (wc[i].wr_id == WRID_RECV && (wc[i].wc_flags & RDMA_WC_WITH_IMM) &&
                    ntohl(wc[i].imm_data) == immediateValue) recvOk = 1;
            }
            if (!count && now_seconds() > deadline) {
                fprintf(stderr, "PHASE2_IMMEDIATE_GATE timeout\n"); goto out;
            }
        }
        printf("PHASE2_IMMEDIATE PASS: %s value=0x%08x\n",
               writeImmediateMode ? "RDMA_WRITE_WITH_IMM" : "bidirectional SEND_WITH_IMM",
               immediateValue);
        result = 0;
        goto out;
    }

    if (multiSgeGate) {
        struct rdma_recv_wr recvWr = { .wr_id = WRID_RECV, .num_sge = multiSgeCount,
                                       .sg_list = multiSge };
        struct rdma_send_wr sendWr = { .wr_id = WRID_SEND, .opcode = RDMA_WR_SEND,
                                       .num_sge = multiSgeCount, .sg_list = multiSge,
                                       .send_flags = RDMA_SEND_SIGNALED };
        int recvRc = rdma_post_recv_sge(qp, &recvWr);
        int sendRc = recvRc ? 0 : rdma_post_send_sge(qp, &sendWr);
        if (recvRc || sendRc) {
            fprintf(stderr, "PHASE2_MULTI_SGE_GATE post failed recv_rc=%d send_rc=%d\n",
                    recvRc, sendRc); goto out;
        }
        int sendOk = 0, recvOk = 0;
        double deadline = now_seconds() + timeoutSec;
        while (!sendOk || !recvOk) {
            struct rdma_wc wc[2] = {};
            int count = rdma_poll_cq(sendCq, wc, 2);
            if (count < 0) { fprintf(stderr, "PHASE2_MULTI_SGE_GATE poll failed\n"); goto out; }
            for (int i = 0; i < count; i++) {
                if (wc[i].status != RDMA_WC_SUCCESS) {
                    fprintf(stderr, "PHASE2_MULTI_SGE_GATE CQE status=%u vendor=0x%x\n",
                            wc[i].status, wc[i].vendor_err); goto out;
                }
                if (wc[i].wr_id == WRID_SEND) sendOk = 1;
                if (wc[i].wr_id == WRID_RECV && wc[i].byte_len == size) recvOk = 1;
            }
            if (!count && now_seconds() > deadline) {
                fprintf(stderr, "PHASE2_MULTI_SGE_GATE timeout\n"); goto out;
            }
        }
        printf("PHASE2_MULTI_SGE PASS: %u-SGE SEND/RECV, %u bytes\n",
               multiSgeCount, size);
        if (directRequested) {
            struct rdma_fast_path_stats stats = {};
            if (rdma_fast_path_get_stats(dev, &stats)) {
                fprintf(stderr, "direct-UAR stats unavailable\n"); goto out;
            }
            printf("DIRECT_UAR_STATS mapped_qps=%llu direct_batches=%llu direct_wrs=%llu direct_doorbells=%llu direct_recv_wrs=%llu direct_cq_consumers=%llu fallback_send=%llu fallback_recv=%llu\n",
                   (unsigned long long)stats.mapped_qps,
                   (unsigned long long)stats.direct_send_batches,
                   (unsigned long long)stats.direct_send_wrs,
                   (unsigned long long)stats.direct_doorbells,
                   (unsigned long long)stats.direct_recv_wrs,
                   (unsigned long long)stats.direct_cq_consumers,
                   (unsigned long long)stats.fallback_send_batches,
                   (unsigned long long)stats.fallback_recv_batches);
        }
        result = 0;
        goto out;
    }

    if (warmupIters) {
        /* Full-length receive every time (matches ggml-rpc's rx ring, which
         * always posts RDMA_CHUNK capacity regardless of the sender's actual
         * message size); small-length send (matches the real header/ack
         * traffic that precedes the first big tensor SEND on the same QP). */
        struct rdma_sge fullRecvSge = { .lkey = mrInfo.lkey,
            .addr = (uint64_t)(uintptr_t)buffer, .length = size };
        struct rdma_sge smallSendSge = { .lkey = mrInfo.lkey,
            .addr = (uint64_t)(uintptr_t)buffer, .length = warmupSize };
        for (uint32_t w = 0; w < warmupIters; w++) {
            if (post_recv_window(qp, &fullRecvSge, 1)) {
                fprintf(stderr, "warmup post_recv failed at %u/%u\n", w, warmupIters);
                goto out;
            }
            if (post_send_window(qp, &smallSendSge, 1, 1, RDMA_WR_SEND, 0, 0)) {
                fprintf(stderr, "warmup post_send failed at %u/%u\n", w, warmupIters);
                goto out;
            }
            int sendOk = 0, recvOk = 0;
            double deadline = now_seconds() + timeoutSec;
            while (!sendOk || !recvOk) {
                struct rdma_wc wc[8];
                int count = rdma_poll_cq(sendCq, wc, 8);
                if (count < 0) { fprintf(stderr, "warmup poll_cq failed\n"); goto out; }
                if (recvCq != sendCq && count < 8) {
                    int received = rdma_poll_cq(recvCq, wc + count, 8 - count);
                    if (received < 0) { fprintf(stderr, "warmup recv poll_cq failed\n"); goto out; }
                    count += received;
                }
                for (int i = 0; i < count; i++) {
                    if (wc[i].status != RDMA_WC_SUCCESS) {
                        fprintf(stderr, "warmup CQE error at %u/%u: status=%u opcode=%u "
                                "qpn=%u wrid=%llu vendor_err=0x%x\n", w, warmupIters,
                                wc[i].status, wc[i].opcode, wc[i].qp_num,
                                (unsigned long long)wc[i].wr_id, wc[i].vendor_err);
                        goto out;
                    }
                    if (wc[i].wr_id == WRID_SEND) sendOk = 1;
                    else if (wc[i].wr_id == WRID_RECV) {
                        if (wc[i].byte_len != warmupSize) {
                            fprintf(stderr, "warmup receive length mismatch at %u/%u: "
                                    "got=%u expected=%u\n", w, warmupIters,
                                    wc[i].byte_len, warmupSize);
                            goto out;
                        }
                        recvOk = 1;
                    }
                }
                if (!count) {
                    if (now_seconds() > deadline) {
                        fprintf(stderr, "warmup timeout at %u/%u (send=%d recv=%d)\n",
                                w, warmupIters, sendOk, recvOk);
                        goto out;
                    }
                    usleep(50);
                }
            }
        }
        printf("=== warmup complete: %u exchanges at %u bytes on qpn=0x%06x; "
               "now sending the real %u-byte message on the SAME QP ===\n",
               warmupIters, warmupSize, rdma_qp_number(qp), size);
    }

    if (indirectMr) {
        /* A freshly CREATE_MKEY'd KLM mkey stays hardware-"free" (unusable
         * for real DMA) until a UMR WQE activates it — notes/48. The QP
         * must be RTS to post that WQE, which is exactly why this can't
         * happen any earlier than here, right after the transition above. */
        if (rdma_activate_indirect_mr(qp, sendCq, mr, indirectChildren,
                                      MLX_INDIRECT_MR_CHILDREN)) {
            fprintf(stderr, "indirect MR UMR activation failed\n");
            goto out;
        }
        printf("Indirect MR activated via UMR (mkey now hardware-valid)\n");

        /* Diagnostic only (notes/48): QUERY_MKEY (0x201) to see what
         * firmware actually committed after the UMR, independent of
         * whatever we assumed we asked for. Input: opcode(2B BE) + mkey
         * index (24 bits at byte offset 9). Output: a 24-byte general
         * command header, then the mkc itself at byte 24 (same offset
         * QUERY_QP uses for its own embedded context — see MLX_QPC_BIT_OFFSET). */
        {
            uint8_t qin[16] = {0};
            qin[0] = 0x02; qin[1] = 0x01;
            uint32_t mkeyIndex = mrInfo.lkey >> 8;
            qin[9]  = (uint8_t)(mkeyIndex >> 16);
            qin[10] = (uint8_t)(mkeyIndex >> 8);
            qin[11] = (uint8_t)(mkeyIndex);
            uint8_t qout[96] = {0};
            uint32_t qoutSize = 0;
            int qrc = rdma_dbg_exec(dev, 0x0201, qin, sizeof(qin), qout,
                                    sizeof(qout), &qoutSize, 5000);
            printf("QUERY_MKEY[%u]: rc=%d outSize=%u\n", mkeyIndex, qrc, qoutSize);
            for (uint32_t i = 0; i + 16 <= qoutSize; i += 16) {
                printf("  [%03u]: %02x %02x %02x %02x %02x %02x %02x %02x "
                       "%02x %02x %02x %02x %02x %02x %02x %02x\n", i,
                       qout[i],qout[i+1],qout[i+2],qout[i+3],
                       qout[i+4],qout[i+5],qout[i+6],qout[i+7],
                       qout[i+8],qout[i+9],qout[i+10],qout[i+11],
                       qout[i+12],qout[i+13],qout[i+14],qout[i+15]);
            }
        }
    }

    if (reverseMode) {
        struct remote_memory_wire memory = {
            .address_be = host_to_be64((uint64_t)(uintptr_t)buffer),
            .rkey_be = htonl(mrInfo.rkey),
            .length_be = htonl(size),
        };
        if (write_full(controlFd, &memory, sizeof(memory))) goto out;
        char reply[sizeof("verify")] = {};
        if (read_full(controlFd, reply, sizeof(reply)) ||
            memcmp(reply, "verify", sizeof(reply))) goto out;
        for (uint32_t byte = 0; byte < size; byte++) {
            if (buffer[byte] != 0xa7) {
                fprintf(stderr, "reverse WRITE mismatch at byte=%u got=0x%02x\n",
                        byte, buffer[byte]); goto out;
            }
        }
        if (write_full(controlFd, "pass", sizeof("pass"))) goto out;
        printf("PHASE3_REVERSE_WRITE PASS: Spark wrote %u bytes into Mac MR; persistent MR verified\n", size);
        result = 0;
        goto out;
    }
    if (getenv("PHASE2_RNR_GATE")) {
        struct rdma_send_wr wr = { .wr_id = WRID_SEND, .opcode = RDMA_WR_SEND,
            .num_sge = 1, .sg_list = &sge, .send_flags = RDMA_SEND_SIGNALED };
        if (write_full(controlFd, "send", sizeof("send"))) goto out;
        int postRc = rdma_post_send_sge(qp, &wr);
        if (postRc) {
            fprintf(stderr, "PHASE2_RNR_GATE post failed rc=%d\n", postRc);
            /* A local post refusal is not an RNR result. */
            goto out;
        }
        double deadline = now_seconds() + timeoutSec;
        int sawRnr = 0;
        while (!sawRnr && now_seconds() <= deadline) {
            struct rdma_wc wc[2] = {}; int n = rdma_poll_cq(sendCq, wc, 2);
            if (n < 0) goto out;
            for (int i = 0; i < n; i++) if (wc[i].wr_id == WRID_SEND) {
                /* A local SEND CQE may be reported before transport retry
                 * exhaustion. The gate must continue polling for the final
                 * RNR/retry CQE rather than treating that success as proof. */
                if (wc[i].status == RDMA_WC_RNR_RETRY || wc[i].status == RDMA_WC_RETRY_EXC) {
                    sawRnr = 1;
                } else if (wc[i].status != RDMA_WC_SUCCESS) {
                    fprintf(stderr, "PHASE2_RNR_GATE unexpected CQE status=%u opcode=%u vendor=0x%x\n", wc[i].status, wc[i].opcode, wc[i].vendor_err); goto out;
                }
            }
            if (!n) usleep(1000);
        }
        if (!sawRnr) { fprintf(stderr, "PHASE2_RNR_GATE timeout waiting for RNR retry CQE\n"); goto out; }
        if (write_full(controlFd, "pass", sizeof("pass"))) goto out;
        printf("PHASE2_RNR PASS: expected RNR retry exhaustion CQE received\n");
        result = 0;
        goto out;
    }
    if (readMode) {
        struct remote_memory_wire memory = {};
        if (readFromPeer) {
            memory.address_be = host_to_be64((uint64_t)(uintptr_t)buffer);
            memory.rkey_be = htonl(mrInfo.rkey);
            memory.length_be = htonl(size);
            memset(buffer, 0xa7, size);
            char token[sizeof("pass")] = {};
            if (write_full(controlFd, &memory, sizeof(memory)) ||
                read_full(controlFd, token, sizeof(token)) || memcmp(token, "pass", sizeof(token))) {
                fprintf(stderr, "PHASE3_REVERSE_READ peer read failed\n"); goto out;
            }
            result = 0;
            printf("PHASE3_REVERSE_READ PASS: Spark read %u bytes x %u\n", size, iters);
            goto out;
        }
        if (read_full(controlFd, &memory, sizeof(memory))) {
            fprintf(stderr, "PHASE3_READ remote memory exchange failed\n"); goto out;
        }
        uint64_t remoteAddr = be64_to_host(memory.address_be); uint32_t remoteRkey = ntohl(memory.rkey_be);
        if (!remoteAddr || !remoteRkey || ntohl(memory.length_be) < size) {
            fprintf(stderr, "PHASE3_READ invalid remote memory addr=0x%llx rkey=0x%x len=%u\n",
                    (unsigned long long)remoteAddr, remoteRkey, ntohl(memory.length_be)); goto out;
        }
        uint32_t done = 0; double deadline = now_seconds() + timeoutSec;
        while (done < iters && now_seconds() <= deadline) {
            uint32_t batch = iters - done; if (batch > window) batch = window;
            if (post_read_window(qp, &sge, batch, signalAll, remoteAddr, remoteRkey)) {
                fprintf(stderr, "PHASE3_READ post failed done=%u batch=%u\n", done, batch); goto out;
            }
            uint32_t completionsNeeded = signalAll ? batch : 1;
            uint32_t batchDone = 0;
            while (batchDone < completionsNeeded && now_seconds() <= deadline) {
                struct rdma_wc wc[16] = {}; int n = rdma_poll_cq(sendCq, wc, 16);
                if (n < 0) goto out;
                for (int i = 0; i < n; i++) {
                    if (wc[i].status != RDMA_WC_SUCCESS ||
                        wc[i].opcode != RDMA_WC_RDMA_READ) {
                        fprintf(stderr, "PHASE3_READ CQE status=%u opcode=%u vendor=0x%x\n", wc[i].status, wc[i].opcode, wc[i].vendor_err); goto out;
                    }
                    batchDone++;
                }
                if (!n) usleep(50);
            }
            if (batchDone != completionsNeeded) break;
            /* With last-signaled batching, one CQE retires the whole batch. */
            done += batch;
        }
        if (done != iters) { fprintf(stderr, "PHASE3_READ timeout done=%u/%u\n", done, iters); goto out; }
        for (uint32_t i = 0; i < size; i++) if (buffer[i] != 0xa7) {
            fprintf(stderr, "PHASE3_READ payload mismatch byte=%u got=0x%02x\n", i, buffer[i]); goto out;
        }
        if (write_full(controlFd, "pass", sizeof("pass"))) goto out;
        result = 0; printf("PHASE3_READ PASS: %u RDMA READ operations, %u bytes\n", iters, size); goto out;
    }
    if (writeMode) {
        struct remote_memory_wire memory = {};
        if (read_full(controlFd, &memory, sizeof(memory))) {
            fprintf(stderr, "remote memory exchange failed\n"); goto out;
        }
        remoteAddr = be64_to_host(memory.address_be);
        remoteRkey = ntohl(memory.rkey_be);
        uint32_t remoteLength = ntohl(memory.length_be);
        if (!remoteAddr || !remoteRkey || remoteLength < size) {
            fprintf(stderr, "invalid remote memory: addr=0x%llx rkey=0x%x length=%u\n",
                    (unsigned long long)remoteAddr, remoteRkey, remoteLength);
            goto out;
        }
        printf("Peer MR: addr=0x%llx rkey=0x%x bytes=%u\n",
               (unsigned long long)remoteAddr, remoteRkey, remoteLength);
        if (readyFile) {
            int readyFd = open(readyFile, O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (readyFd < 0) { perror("create ready file"); goto out; }
            close(readyFd);
        }
        if (startFile) {
            double deadline = now_seconds() + timeoutSec;
            while (access(startFile, F_OK) != 0) {
                if (now_seconds() >= deadline) {
                    fprintf(stderr, "multi-QP start barrier timed out\n");
                    goto out;
                }
                usleep(1000);
            }
        }
    }
    if (post_send_window(qp, &sge, firstWindow, signalAll,
                         writeMode ? RDMA_WR_RDMA_WRITE : RDMA_WR_SEND,
                         remoteAddr, remoteRkey)) {
        fprintf(stderr, "initial post_send window failed\n"); goto out;
    }
    uint32_t sendPosted = firstWindow, sendDone = 0, recvDone = 0;
    uint32_t totalBatches = (iters + window - 1) / window;
    uint32_t batchesPosted = 1;
    uint32_t doorbellsPosted = (firstWindow + RDMA_POST_CHUNK - 1) /
                               RDMA_POST_CHUNK;
    uint32_t expectedSendCqes = signalAll ? iters : totalBatches;
    double started = now_seconds(), lastProgress = started;
    while (sendDone < expectedSendCqes || (!writeMode && recvDone < iters)) {
        struct rdma_wc wc[16];
        int count = rdma_poll_cq(sendCq, wc, 16);
        if (count < 0) { fprintf(stderr, "poll_cq failed\n"); goto out; }
        if (!writeMode && recvCq != sendCq && count < 16) {
            int received = rdma_poll_cq(recvCq, wc + count, 16 - count);
            if (received < 0) {
                fprintf(stderr, "recv poll_cq failed\n"); goto out;
            }
            count += received;
        }
        if (!count) {
            if (now_seconds() - lastProgress > timeoutSec) {
                fprintf(stderr, "no CQ progress for %u seconds "
                        "(send_cqe=%u/%u recv=%u/%u)\n",
                        timeoutSec, sendDone, expectedSendCqes, recvDone, iters);
                goto out;
            }
            usleep(50);
            continue;
        }
        lastProgress = now_seconds();
        for (int i = 0; i < count; i++) {
            if (wc[i].status != RDMA_WC_SUCCESS) {
                fprintf(stderr, "CQE error: status=%u opcode=%u qpn=%u wrid=%llu vendor_err=0x%x\n",
                        wc[i].status, wc[i].opcode, wc[i].qp_num,
                        (unsigned long long)wc[i].wr_id, wc[i].vendor_err);
                goto out;
            }
            if (wc[i].wr_id == WRID_SEND) sendDone++;
            else if (wc[i].wr_id == WRID_RECV) {
                if (wc[i].byte_len != size) {
                    fprintf(stderr, "receive length mismatch: got=%u expected=%u\n",
                            wc[i].byte_len, size); goto out;
                }
                for (uint32_t byte = 0; byte < size; byte++) {
                    if (buffer[byte] != 0x7b) {
                        fprintf(stderr, "payload corruption at exchange=%u byte=%u: 0x%02x\n",
                                recvDone, byte, buffer[byte]); goto out;
                    }
                }
                const uint8_t *raw = (const uint8_t *)allocation;
                for (uint32_t guard = 0; guard < 64; guard++) {
                    bool prefixBad = bufferOffset != 0 && raw[guard] != 0xa5;
                    if (prefixBad || raw[bufferOffset + size + guard] != 0xa5) {
                        fprintf(stderr, "DMA guard corruption at exchange=%u\n", recvDone);
                        goto out;
                    }
                }
                recvDone++;
            } else {
                fprintf(stderr, "unknown completion wrid=%llu\n",
                        (unsigned long long)wc[i].wr_id); goto out;
            }
        }
        if (sendPosted < iters && (writeMode || recvDone == sendPosted) &&
            sendDone == (signalAll ? sendPosted : batchesPosted)) {
            uint32_t next = iters - sendPosted;
            if (next > window) next = window;
            if (!writeMode && post_recv_window(qp, &sge, next)) {
                fprintf(stderr, "post_recv window failed at %u\n", recvDone);
                goto out;
            }
            if (post_send_window(qp, &sge, next, signalAll,
                                 writeMode ? RDMA_WR_RDMA_WRITE : RDMA_WR_SEND,
                                 remoteAddr, remoteRkey)) {
                fprintf(stderr, "post_send window failed at %u\n", sendPosted);
                goto out;
            }
            sendPosted += next;
            batchesPosted++;
            doorbellsPosted += (next + RDMA_POST_CHUNK - 1) / RDMA_POST_CHUNK;
        }
        if ((recvDone % 100000u) == 0 && recvDone && recvDone == sendPosted)
            printf("progress: %u/%u bidirectional exchanges\n", recvDone, iters);
    }
    double elapsed = now_seconds() - started;
    if (writeMode) {
        if (write_full(controlFd, "verify", sizeof("verify"))) {
            fprintf(stderr, "remote verify request failed\n"); goto out;
        }
        char reply[sizeof("pass")] = {};
        if (read_full(controlFd, reply, sizeof(reply)) ||
            memcmp(reply, "pass", sizeof(reply))) {
            fprintf(stderr, "remote RDMA WRITE verification failed\n"); goto out;
        }
        printf("PHASE3_WRITE PASS: %u one-sided RDMA WRITEs, %u bytes, %.3fs, "
               "%.2f Gbit/s; window=%u doorbells=%u remote-memory verified\n",
               iters, size, elapsed,
               ((double)iters * size * 8.0) / elapsed / 1000000000.0,
               window, doorbellsPosted);
        if (directRequested) {
            struct rdma_fast_path_stats stats = {};
            if (rdma_fast_path_get_stats(dev, &stats)) {
                fprintf(stderr, "direct-UAR stats unavailable\n"); goto out;
            }
            printf("DIRECT_UAR_STATS mapped_qps=%llu direct_batches=%llu direct_wrs=%llu direct_doorbells=%llu direct_recv_wrs=%llu direct_cq_consumers=%llu fallback_send=%llu fallback_recv=%llu\n",
                   (unsigned long long)stats.mapped_qps,
                   (unsigned long long)stats.direct_send_batches,
                   (unsigned long long)stats.direct_send_wrs,
                   (unsigned long long)stats.direct_doorbells,
                   (unsigned long long)stats.direct_recv_wrs,
                   (unsigned long long)stats.direct_cq_consumers,
                   (unsigned long long)stats.fallback_send_batches,
                   (unsigned long long)stats.fallback_recv_batches);
        }
        result = 0;
        goto out;
    }
    if (getenv("PHASE2_RECOVERY_GATE")) {
        memset(&attr, 0, sizeof(attr));
        attr.cur_state = RDMA_QPS_RTS;
        attr.new_state = RDMA_QPS_ERR;
        if (rdma_modify_qp(qp, &attr) || verify_state(qp, RDMA_QPS_ERR, "RTS->ERR"))
            goto out;
        attr.cur_state = RDMA_QPS_ERR;
        attr.new_state = RDMA_QPS_RESET;
        if (rdma_modify_qp(qp, &attr) || verify_state(qp, RDMA_QPS_RESET, "ERR->RST"))
            goto out;
        printf("PHASE2_RECOVERY PASS: terminal RTS->ERR->RESET verified\n");
    }
    printf("PHASE2_GATE PASS: %u bidirectional SEND/RECV, %u bytes, %.3fs, "
           "%.2f Mbit/s; window=%u doorbells=%u cqs=%s "
           "signal=%s CQ/SQ/RQ wrap verified\n",
           iters, size, elapsed,
           ((double)iters * size * 2.0 * 8.0) / elapsed / 1000000.0,
           window, doorbellsPosted, recvCq == sendCq ? "shared" : "separate",
           signalAll ? "all" : "last");
    result = 0;
    if (directRequested) {
        struct rdma_fast_path_stats stats = {};
        if (rdma_fast_path_get_stats(dev, &stats)) {
            fprintf(stderr, "direct-UAR stats unavailable\n"); goto out;
        }
        printf("DIRECT_UAR_STATS mapped_qps=%llu direct_batches=%llu direct_wrs=%llu direct_doorbells=%llu direct_recv_wrs=%llu direct_cq_consumers=%llu fallback_send=%llu fallback_recv=%llu\n",
               (unsigned long long)stats.mapped_qps,
               (unsigned long long)stats.direct_send_batches,
               (unsigned long long)stats.direct_send_wrs,
               (unsigned long long)stats.direct_doorbells,
               (unsigned long long)stats.direct_recv_wrs,
               (unsigned long long)stats.direct_cq_consumers,
               (unsigned long long)stats.fallback_send_batches,
               (unsigned long long)stats.fallback_recv_batches);
        if (!stats.mapped_qps || !stats.direct_send_wrs ||
            !stats.direct_doorbells || !stats.direct_recv_wrs ||
            !stats.direct_cq_consumers) {
            fprintf(stderr, "direct-UAR path was not exercised\n");
            result = 1;
        }
    }

out: ;
    int teardownFailed = 0;
    if (mw) {
        int rc = rdma_dealloc_mw(mw);
        if (rc) { fprintf(stderr, "teardown: dealloc_mw rc=%d\n", rc); teardownFailed = 1; }
    }
    if (qp) {
        int rc = rdma_destroy_qp(qp);
        if (rc) { fprintf(stderr, "teardown: destroy_qp rc=%d\n", rc); teardownFailed = 1; }
    }
    if (mr) {
        /* Indirect mkey first — children must outlive it. */
        int rc = rdma_dereg_mr(mr);
        if (rc) { fprintf(stderr, "teardown: dereg_mr rc=%d\n", rc); teardownFailed = 1; }
    }
    for (uint32_t i = 0; i < indirectChildCount; i++) {
        int rc = rdma_dereg_mr(indirectChildren[i]);
        if (rc) {
            fprintf(stderr, "teardown: dereg indirect child %u rc=%d\n", i, rc);
            teardownFailed = 1;
        }
    }
    if (allocation) free(allocation);
    if (recvCq && recvCq != sendCq) {
        int rc = rdma_destroy_cq(recvCq);
        if (rc) { fprintf(stderr, "teardown: destroy_recv_cq rc=%d\n", rc); teardownFailed = 1; }
    }
    if (sendCq) {
        int rc = rdma_destroy_cq(sendCq);
        if (rc) { fprintf(stderr, "teardown: destroy_send_cq rc=%d\n", rc); teardownFailed = 1; }
    }
    if (pd) {
        int rc = rdma_dealloc_pd(pd);
        if (rc) { fprintf(stderr, "teardown: dealloc_pd rc=%d\n", rc); teardownFailed = 1; }
    }
    if (gidProgrammed && rdma_clear_roce_address(dev, gidIndex)) {
        fprintf(stderr, "RoCE address clear/readback failed\n");
        result = 1;
    }
    if (fastPathEnabled && dev) rdma_unmap_fast_path(dev);
    if (dev) rdma_close_device(dev);
    if (controlFd >= 0) close(controlFd);
    if (teardownFailed) {
        fprintf(stderr, "Phase 2 resource teardown failed\n");
        result = 1;
    }
    if (result) fprintf(stderr, "PHASE2_GATE FAIL\n");
    return result;
}
