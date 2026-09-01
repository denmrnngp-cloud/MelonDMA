/* Minimal RC RDMA-WRITE test server for the Spark (Linux) side.
 *
 * Unlike `ibv_rc_pingpong` (which tests SEND/RECV and never exposes an
 * rkey/va over the wire), this registers a real memory region and publishes
 * (rkey, va) alongside the usual lid:qpn:psn:gid handshake line, so the
 * macOS software RoCEv2 client (code/rocev2_mac_client.py) can target a
 * real RDMA_WRITE at it and we can verify the buffer contents afterwards.
 *
 * Build on Spark (needs libibverbs-dev, already installed - confirmed in
 * this session via `dpkg -l | grep ibverbs`):
 *   cc -O2 -o spark_write_server spark_write_server.c -libverbs
 *
 * Run (after `sudo rdma link add rxe0 type rxe netdev enP7s7`, see notes/04):
 *   ./spark_write_server -d rxe0 -g 0 -p 18515
 *
 * STATUS: written but NOT compiled/run in this session (no execution
 * environment for Spark-side C code from here - would need to be built and
 * run by the user on the Spark box, or by an agent with a shell on Spark).
 * Cross-check against `research/rxe-reference/rc_pingpong.c` before trusting
 * the ibv_* call sequence verbatim - this follows the same pattern but was
 * not validated by compiling it.
 */
#include <arpa/inet.h>
#include <getopt.h>
#include <infiniband/verbs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BUF_SIZE 4096
#define DEFAULT_PORT 18515

struct dest {
    int lid;
    int qpn;
    int psn;
    union ibv_gid gid;
    uint32_t rkey;
    uint64_t va;
};

static void gid_to_wire(union ibv_gid *gid, char wire[33]) {
    for (int i = 0; i < 16; i++) sprintf(&wire[i * 2], "%02x", gid->raw[i]);
}

static void wire_to_gid(const char *wire, union ibv_gid *gid) {
    char tmp[3] = {0};
    for (int i = 0; i < 16; i++) {
        memcpy(tmp, wire + i * 2, 2);
        gid->raw[i] = (uint8_t)strtol(tmp, NULL, 16);
    }
}

int main(int argc, char **argv) {
    const char *dev_name = NULL;
    int gid_idx = 0, port = DEFAULT_PORT, ib_port = 1;
    int opt;
    while ((opt = getopt(argc, argv, "d:g:p:i:")) != -1) {
        switch (opt) {
            case 'd': dev_name = optarg; break;
            case 'g': gid_idx = atoi(optarg); break;
            case 'p': port = atoi(optarg); break;
            case 'i': ib_port = atoi(optarg); break;
        }
    }
    if (!dev_name) { fprintf(stderr, "usage: %s -d <device> [-g gid_idx] [-p tcp_port]\n", argv[0]); return 1; }

    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    struct ibv_context *ctx = NULL;
    for (int i = 0; dev_list[i]; i++) {
        if (strcmp(ibv_get_device_name(dev_list[i]), dev_name) == 0) {
            ctx = ibv_open_device(dev_list[i]);
            break;
        }
    }
    if (!ctx) { fprintf(stderr, "device %s not found\n", dev_name); return 1; }

    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    char *buf = calloc(1, BUF_SIZE);
    struct ibv_mr *mr = ibv_reg_mr(pd, buf, BUF_SIZE,
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
    if (!mr) { perror("ibv_reg_mr"); return 1; }

    struct ibv_cq *cq = ibv_create_cq(ctx, 16, NULL, NULL, 0);
    struct ibv_qp_init_attr qp_attr = {
        .send_cq = cq, .recv_cq = cq, .qp_type = IBV_QPT_RC,
        .cap = { .max_send_wr = 1, .max_recv_wr = 1, .max_send_sge = 1, .max_recv_sge = 1 },
    };
    struct ibv_qp *qp = ibv_create_qp(pd, &qp_attr);
    if (!qp) { perror("ibv_create_qp"); return 1; }

    {
        struct ibv_qp_attr attr = { .qp_state = IBV_QPS_INIT, .pkey_index = 0,
            .port_num = ib_port, .qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ };
        ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
    }

    union ibv_gid my_gid;
    ibv_query_gid(ctx, ib_port, gid_idx, &my_gid);

    struct dest my_dest = { .lid = 0, .qpn = qp->qp_num, .psn = rand() & 0xffffff,
                             .gid = my_gid, .rkey = mr->rkey, .va = (uint64_t)(uintptr_t)buf };

    /* --- TCP handshake: listen, accept one client, exchange dest info --- */
    int lsock = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = INADDR_ANY };
    bind(lsock, (struct sockaddr *)&addr, sizeof addr);
    listen(lsock, 1);
    printf("listening on :%d (rkey=0x%x va=0x%llx buf=%p)\n", port, my_dest.rkey,
           (unsigned long long)my_dest.va, (void *)buf);
    int csock = accept(lsock, NULL, NULL);

    char rxbuf[128] = {0};
    if (read(csock, rxbuf, sizeof rxbuf) <= 0) { perror("read handshake"); return 1; }
    struct dest peer = {0};
    char gidhex[33] = {0};
    sscanf(rxbuf, "%x:%x:%x:%32s", &peer.lid, &peer.qpn, &peer.psn, gidhex);
    wire_to_gid(gidhex, &peer.gid);

    char gidwire[33] = {0};
    gid_to_wire(&my_dest.gid, gidwire);
    char txbuf[128];
    int n = snprintf(txbuf, sizeof txbuf, "%04x:%06x:%06x:%s:%08x:%016llx", my_dest.lid,
                      my_dest.qpn, my_dest.psn, gidwire, my_dest.rkey,
                      (unsigned long long)my_dest.va);
    if (write(csock, txbuf, n) != n) { perror("write handshake reply"); return 1; }
    close(csock);
    close(lsock);

    /* --- move QP to RTR then RTS, pointed at the macOS client's qpn/psn/gid --- */
    {
        struct ibv_qp_attr attr = {0};
        attr.qp_state = IBV_QPS_RTR;
        attr.path_mtu = IBV_MTU_1024;
        attr.dest_qp_num = peer.qpn;
        attr.rq_psn = peer.psn;
        attr.max_dest_rd_atomic = 1;
        attr.min_rnr_timer = 12;
        attr.ah_attr.is_global = 1;
        attr.ah_attr.grh.dgid = peer.gid;
        attr.ah_attr.grh.sgid_index = gid_idx;
        attr.ah_attr.grh.hop_limit = 1;
        attr.ah_attr.port_num = ib_port;
        int rc = ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
            IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
        if (rc) { perror("modify_qp RTR"); return 1; }
    }
    {
        struct ibv_qp_attr attr = {0};
        attr.qp_state = IBV_QPS_RTS;
        attr.timeout = 14;
        attr.retry_cnt = 7;
        attr.rnr_retry = 7;
        attr.sq_psn = my_dest.psn;
        attr.max_rd_atomic = 1;
        int rc = ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
            IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);
        if (rc) { perror("modify_qp RTS"); return 1; }
    }

    printf("QP is RTS, waiting for RDMA WRITE from peer qpn=0x%06x ...\n", peer.qpn);
    printf("press Enter after the macOS client reports it sent the WRITE, "
           "to dump the buffer contents:\n");
    getchar();
    printf("buffer contents (first 64 bytes): ");
    for (int i = 0; i < 64 && i < BUF_SIZE; i++) printf("%02x ", (unsigned char)buf[i]);
    printf("\n");

    return 0;
}
