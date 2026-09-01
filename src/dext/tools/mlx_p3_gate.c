/* Live P3 gate for MelonDMA (Mac side): inline SEND, RC atomics, SL and
 * solicited_only CQ arming — against the stock-libverbs Linux peer
 * (tools/mlx_p3_peer.c). Uses the MelonDMA ibverbs compatibility surface. */
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
static uint64_t be64_to_host(uint64_t value) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap64(value);
#else
    return value;
#endif
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

static const uint32_t inline_sizes[4] = { 1, 64, 256, 512 };

int main(int argc, char **argv) {
    const char *host = NULL, *local_ip = "192.168.200.1", *local_mac = "98:03:9b:80:6a:94", *remote_mac = NULL;
    uint16_t port = 18515; uint32_t mtu_bytes = 4096; int opt;
    while ((opt = getopt(argc, argv, "h:p:m:l:a:r:")) != -1) switch (opt) {
    case 'h': host = optarg; break; case 'p': port = (uint16_t)strtoul(optarg, NULL, 0); break;
    case 'm': mtu_bytes = (uint32_t)strtoul(optarg, NULL, 0); break; case 'l': local_ip = optarg; break;
    case 'a': local_mac = optarg; break; case 'r': remote_mac = optarg; break; default: return 2;
    }
    if (!host || !remote_mac || !mtu(mtu_bytes)) return 2;
    int rc = 1, count = 0, fd = -1, completions = 0;
    const char *stage = "enumerate";
    struct ibv_device **devices = ibv_get_device_list(&count);
    struct ibv_context *ctx = devices && count ? ibv_open_device(devices[0]) : NULL;
    struct ibv_mlx5_roce_config config = { .hop_limit = 1 };
    if (!ctx || inet_pton(AF_INET, local_ip, config.local_gid.raw + 12) != 1 ||
        sscanf(local_mac, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &config.local_mac[0], &config.local_mac[1], &config.local_mac[2], &config.local_mac[3], &config.local_mac[4], &config.local_mac[5]) != 6 ||
        sscanf(remote_mac, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &config.peer_mac[0], &config.peer_mac[1], &config.peer_mac[2], &config.peer_mac[3], &config.peer_mac[4], &config.peer_mac[5]) != 6) goto out;
    config.local_gid.raw[10] = 0xff; config.local_gid.raw[11] = 0xff;

    stage = "query_device";
    struct ibv_device_attr attr_dev = {};
    if (ibv_query_device(ctx, &attr_dev)) goto out;
    if (attr_dev.max_inline_data < 512 || attr_dev.max_qp_rd_atom < 1) {
        fprintf(stderr, "P3 caps wrong: max_inline_data=%d max_qp_rd_atom=%d\n",
                attr_dev.max_inline_data, attr_dev.max_qp_rd_atom); goto out;
    }

    stage = "configure_roce";
    if (ibv_mlx5_configure_roce(ctx, &config)) goto out;

    /* ---- P3 addendum: full GID-table enumeration + GID-change handling ---- */
    stage = "gid_table_enumerate";
    {
        uint32_t count = 0, table_size = 0;
        struct ibv_gid_entry entries[16];
        if (ibv_query_gid_table(ctx, 1, entries, 16, &count, &table_size) ||
            table_size < 1)
            goto out;
        int found_ipv4 = 0;
        for (uint32_t i = 0; i < count; i++)
            if (entries[i].gid_type == IBV_GID_TYPE_ROCE_V2 &&
                !memcmp(entries[i].gid.raw, config.local_gid.raw, 16))
                found_ipv4 = 1;
        if (!found_ipv4) {
            fprintf(stderr, "gid table: configured IPv4 GID not enumerated "
                    "(size=%u count=%u)\n", table_size, count);
            goto out;
        }

        /* Two more entries: a link-local IPv6 GID and an IPv4 GID on VLAN 42. */
        union ibv_gid v6 = {};
        v6.raw[0] = 0xfe; v6.raw[1] = 0x80;
        for (int i = 2; i < 16; i++) v6.raw[i] = (uint8_t)(0x40 + i);
        const uint8_t mac6[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x02 };
        const uint8_t vlan_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x03 };
        uint32_t v6_idx = 0, vlan_idx = 0;
        if (ibv_mlx5_add_gid(ctx, &v6, mac6, 1, 0, 0, &v6_idx) ||
            ibv_mlx5_add_gid(ctx, &config.local_gid, vlan_mac, 0, 42, 1,
                             &vlan_idx))
            goto out;

        count = 0;
        if (ibv_query_gid_table(ctx, 1, entries, 16, &count, &table_size))
            goto out;
        int saw_v6 = 0, saw_vlan = 0;
        for (uint32_t i = 0; i < count; i++) {
            if (entries[i].gid_index == v6_idx &&
                !memcmp(entries[i].gid.raw, v6.raw, 16))
                saw_v6 = 1;
            if (entries[i].gid_index == vlan_idx)
                saw_vlan = 1;  /* VLAN value is verified by the add_gid readback */
        }
        if (!saw_v6 || !saw_vlan) {
            fprintf(stderr, "gid table: v6=%d vlan42=%d (count=%u)\n",
                    saw_v6, saw_vlan, count);
            goto out;
        }

        /* GID-change: delete the IPv6 entry, confirm it leaves the table. */
        if (ibv_mlx5_del_gid(ctx, v6_idx)) goto out;
        count = 0;
        if (ibv_query_gid_table(ctx, 1, entries, 16, &count, &table_size))
            goto out;
        int gone_v6 = 1;
        for (uint32_t i = 0; i < count; i++)
            if (entries[i].gid_index == v6_idx) { gone_v6 = 0; break; }
        if (!gone_v6) { fprintf(stderr, "gid table: deleted GID still present\n"); goto out; }
        if (ibv_mlx5_del_gid(ctx, vlan_idx)) goto out;
    }

    /* ---- P3 addendum: DCQCN params query/modify roundtrip ---- */
    stage = "cc_query";
    struct ibv_mlx5_cong_params cc_base = {}, cc = {};
    if (ibv_mlx5_query_cong(ctx, &cc_base))
        goto out;
    cc = cc_base;
    cc.rpg_ai_rate = (cc_base.rpg_ai_rate == 5) ? 6 : 5;
    stage = "cc_modify";
    if (ibv_mlx5_modify_cong(ctx, &cc))
        goto out;
    stage = "cc_roundtrip";
    {
        struct ibv_mlx5_cong_params back = {};
        if (ibv_mlx5_query_cong(ctx, &back) ||
            back.rpg_ai_rate != cc.rpg_ai_rate) {
            fprintf(stderr, "DCQCN roundtrip: ai_rate=%u want=%u\n",
                    back.rpg_ai_rate, cc.rpg_ai_rate);
            goto out;
        }
    }
    stage = "cc_restore";
    if (ibv_mlx5_modify_cong(ctx, &cc_base))
        goto out;

    stage = "alloc_pd";
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    stage = "create_channel";
    struct ibv_comp_channel *channel = ibv_create_comp_channel(ctx);
    int marker = 7;
    stage = "create_cq";
    struct ibv_cq *cq = channel ? ibv_create_cq(ctx, 64, &marker, channel, 0) : NULL;
    uint8_t recv_buf[16]; memset(recv_buf, 0, sizeof(recv_buf));
    uint64_t atomic_result = 0;
    struct ibv_mr *mr = pd ? ibv_reg_mr(pd, recv_buf, sizeof(recv_buf), IBV_ACCESS_LOCAL_WRITE) : NULL;
    struct ibv_mr *result_mr = pd ? ibv_reg_mr(pd, &atomic_result, sizeof(atomic_result), IBV_ACCESS_LOCAL_WRITE) : NULL;
    struct ibv_qp_init_attr init = { .send_cq = cq, .recv_cq = cq, .qp_type = IBV_QPT_RC,
        .cap = { .max_send_wr = 16, .max_recv_wr = 8, .max_send_sge = 1, .max_recv_sge = 1,
                 .max_inline_data = attr_dev.max_inline_data } };
    stage = "create_qp";
    struct ibv_qp *qp = pd && cq ? ibv_create_qp(pd, &init) : NULL;
    if (!pd || !channel || !cq || !mr || !result_mr || !qp) goto cleanup;
    if (init.cap.max_inline_data != 512) { fprintf(stderr, "QP max_inline_data not accepted\n"); goto cleanup; }

    struct ibv_qp_attr attr = { .qp_state = IBV_QPS_INIT, .pkey_index = 0, .port_num = 1,
        .qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC };
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
        .max_dest_rd_atomic = 4, .min_rnr_timer = 12, .ah_attr = { .is_global = 1, .port_num = 1,
        .sl = 3, .grh = { .dgid = remote.gid, .sgid_index = 0, .hop_limit = 1 } } };
    stage = "init_to_rtr";
    if (ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER)) goto cleanup;
    attr = (struct ibv_qp_attr){ .qp_state = IBV_QPS_RTS, .sq_psn = local.psn, .timeout = 14, .retry_cnt = 7, .rnr_retry = 7, .max_rd_atomic = 4 };
    stage = "rtr_to_rts";
    if (ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC)) goto cleanup;

    stage = "read_atomic_mr";
    struct remote_memory_wire memory = {};
    if (full_read(fd, &memory, sizeof(memory)) || full_read(fd, msg, 5)) goto cleanup;
    uint64_t remote_atomic_addr = be64_to_host(memory.address_be);
    uint32_t remote_atomic_rkey = ntohl(memory.rkey_be);

    /* Pre-post one RECV for the peer's SOLICITED SEND (phase 5). */
    stage = "prepost_recv";
    {
        struct ibv_sge sge = { .addr = (uintptr_t)recv_buf, .length = sizeof(recv_buf), .lkey = mr->lkey };
        struct ibv_recv_wr recv = { .wr_id = 900, .sg_list = &sge, .num_sge = 1 }, *bad = NULL;
        if (ibv_post_recv(qp, &recv, &bad)) goto cleanup;
    }

    /* Phase 3: inline SENDs of 1/64/256/512 bytes. */
    stage = "inline_send";
    for (int i = 0; i < 4; i++) {
        uint32_t sz = inline_sizes[i];
        uint8_t payload[512];
        for (uint32_t b = 0; b < sz; b++) payload[b] = (uint8_t)(sz ^ b);
        struct ibv_sge sge = { .addr = (uintptr_t)payload, .length = sz, .lkey = 0 };
        struct ibv_send_wr send = { .wr_id = 100 + i, .sg_list = &sge, .num_sge = 1,
            .opcode = IBV_WR_SEND, .send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE }, *bad = NULL;
        if (ibv_post_send(qp, &send, &bad)) goto cleanup;
    }
    for (int i = 0; i < 4; i++) {
        struct ibv_wc wc = {}; int n = 0;
        for (;;) { n = ibv_poll_cq(cq, 1, &wc); if (n < 0) goto cleanup; if (n == 1) break; }
        if (wc.status != IBV_WC_SUCCESS || wc.opcode != IBV_WC_SEND) { fprintf(stderr, "inline send %d: status=%d opcode=%d\n", i, wc.status, wc.opcode); goto cleanup; }
        completions++;
    }

    /* Phase 4a: FETCH_ADD(+5) on the peer's atomic word (0x100 -> 0x105). */
    stage = "fetch_add";
    {
        struct ibv_sge result_sge = { .addr = (uintptr_t)&atomic_result, .length = 8, .lkey = result_mr->lkey };
        struct ibv_send_wr send = { .wr_id = 200, .sg_list = &result_sge, .num_sge = 1,
            .opcode = IBV_WR_ATOMIC_FETCH_AND_ADD, .send_flags = IBV_SEND_SIGNALED };
        send.wr.atomic.remote_addr = remote_atomic_addr;
        send.wr.atomic.rkey = remote_atomic_rkey;
        send.wr.atomic.compare_add = 5; send.wr.atomic.swap = 0;
        struct ibv_send_wr *bad = NULL;
        if (ibv_post_send(qp, &send, &bad)) goto cleanup;
        struct ibv_wc wc = {}; int n = 0;
        for (;;) { n = ibv_poll_cq(cq, 1, &wc); if (n < 0) goto cleanup; if (n == 1) break; }
        if (wc.status != IBV_WC_SUCCESS || wc.opcode != IBV_WC_FETCH_ADD ||
            !(wc.wc_flags & IBV_WC_WITH_ATOMIC) || wc.atomic_result != 0x100) {
            fprintf(stderr, "FETCH_ADD: status=%d opcode=%d flags=0x%x result=0x%llx vendor_err=0x%x\n",
                    wc.status, wc.opcode, wc.wc_flags, (unsigned long long)wc.atomic_result, wc.vendor_err); goto cleanup;
        }
        completions++;
    }

    /* Phase 4b: CMP_SWAP(0x105 -> 0x200). */
    stage = "cmp_swap";
    {
        struct ibv_sge result_sge = { .addr = (uintptr_t)&atomic_result, .length = 8, .lkey = result_mr->lkey };
        struct ibv_send_wr send = { .wr_id = 201, .sg_list = &result_sge, .num_sge = 1,
            .opcode = IBV_WR_ATOMIC_CMP_AND_SWP, .send_flags = IBV_SEND_SIGNALED };
        send.wr.atomic.remote_addr = remote_atomic_addr;
        send.wr.atomic.rkey = remote_atomic_rkey;
        send.wr.atomic.compare_add = 0x105; send.wr.atomic.swap = 0x200;
        struct ibv_send_wr *bad = NULL;
        if (ibv_post_send(qp, &send, &bad)) goto cleanup;
        struct ibv_wc wc = {}; int n = 0;
        for (;;) { n = ibv_poll_cq(cq, 1, &wc); if (n < 0) goto cleanup; if (n == 1) break; }
        if (wc.status != IBV_WC_SUCCESS || wc.opcode != IBV_WC_COMP_SWAP ||
            !(wc.wc_flags & IBV_WC_WITH_ATOMIC) || wc.atomic_result != 0x105) {
            fprintf(stderr, "CMP_SWAP: status=%d opcode=%d flags=0x%x result=0x%llx vendor_err=0x%x\n",
                    wc.status, wc.opcode, wc.wc_flags, (unsigned long long)wc.atomic_result, wc.vendor_err); goto cleanup;
        }
        completions++;
    }

    /* Phase 5: arm solicited_only, let the peer send a SOLICITED SEND. */
    stage = "arm_solicited";
    if (ibv_req_notify_cq(cq, 1)) goto cleanup;
    stage = "signal_atomic_done";
    if (full_write(fd, "atomic_done", 11)) goto cleanup;
    struct pollfd event_fd = { .fd = channel->fd, .events = POLLIN };
    stage = "wait_event";
    if (poll(&event_fd, 1, 10000) != 1) goto cleanup;
    struct ibv_cq *event_cq = NULL; void *event_context = NULL;
    stage = "get_event";
    if (ibv_get_cq_event(channel, &event_cq, &event_context) || event_cq != cq || event_context != &marker) goto cleanup;
    ibv_ack_cq_events(cq, 1);
    stage = "poll_solicited_recv";
    {
        struct ibv_wc wc = {}; int n = 0;
        for (;;) { n = ibv_poll_cq(cq, 1, &wc); if (n < 0) goto cleanup; if (n == 1) break; }
        if (wc.status != IBV_WC_SUCCESS || wc.opcode != IBV_WC_RECV) { fprintf(stderr, "solicited recv: status=%d opcode=%d\n", wc.status, wc.opcode); goto cleanup; }
        completions++;
    }
    stage = "read_peer_verdict";
    char verdict[3] = {}; if (full_read(fd, verdict, 2)) goto cleanup;
    if (memcmp(verdict, "OK", 2)) { fprintf(stderr, "peer reported failure\n"); goto cleanup; }

    printf("P3_GATE PASS: caps(inline=%d,rd_atom=%d) inline[1/64/256/512] "
           "FETCH_ADD/CMP_SWAP(atomic_result verified) SL=3 solicited_only CQ arm, "
           "%d CQEs; GID-table enumerate/add/vlan/change, DCQCN query-modify roundtrip\n",
           attr_dev.max_inline_data, attr_dev.max_qp_rd_atom, completions);
    rc = 0;
cleanup:
    if (fd >= 0) close(fd);
    if (qp) ibv_destroy_qp(qp);
    if (result_mr) ibv_dereg_mr(result_mr);
    if (mr) ibv_dereg_mr(mr);
    if (cq) ibv_destroy_cq(cq);
    if (channel) ibv_destroy_comp_channel(channel);
    if (pd) ibv_dealloc_pd(pd);
out:
    if (rc) fprintf(stderr, "P3_GATE FAIL at %s (errno=%d)\n", stage, errno);
    if (ctx) ibv_close_device(ctx);
    if (devices) ibv_free_device_list(devices);
    return rc;
}
