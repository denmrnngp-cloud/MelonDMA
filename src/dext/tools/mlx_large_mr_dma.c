/* mlx_large_mr_dma — loopback translation check for the large-MR fast path.
 *
 * Registers one big buffer, brings a QP to RTS with a *self* destination
 * (dest_qpn = own QPN, dgid = own GID), then RDMA_WRITEs a known pattern
 * from a low offset to a disjoint high offset within the SAME registered MR.
 * If the coalesced MTT maps VA -> the wrong IOVA, the write lands somewhere
 * else and the memcmp fails (or the completion errors) — the definitive
 * proof that 16 KiB-granular inline PAS translates correctly, not just that
 * CREATE_MKEY was accepted by firmware.
 *
 * Usage: mlx_large_mr_dma [size_bytes]   (default 32 MiB)
 */
#include <infiniband/verbs.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    size_t size = argc > 1 ? (size_t)strtoull(argv[1], 0, 0) : (32u << 20);
    const size_t chunk = 4096;   /* small write: MTU-insensitive, fast */
    int rc = 1;

    struct ibv_device **devs = ibv_get_device_list(NULL);
    if (!devs || !devs[0]) { fprintf(stderr, "no devices\n"); return 1; }
    struct ibv_context *ctx = ibv_open_device(devs[0]);
    if (!ctx) { perror("ibv_open_device"); goto out_devs; }

    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    struct ibv_comp_channel *ch = ibv_create_comp_channel(ctx);
    struct ibv_cq *cq = ch ? ibv_create_cq(ctx, 32, NULL, ch, 0) : NULL;
    if (!pd || !cq) { fprintf(stderr, "pd/cq failed\n"); goto out_cq; }

    uint8_t *buf = malloc(size);
    if (!buf) { perror("malloc"); goto out_cq; }
    memset(buf, 0, size);

    struct ibv_mr *mr = ibv_reg_mr(pd, buf, size,
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
    if (!mr) { fprintf(stderr, "ibv_reg_mr(%zu MiB) failed: errno=%d\n", size >> 20, errno); goto out_buf; }
    printf("reg OK: %zu MiB lkey=0x%x rkey=0x%x\n", size >> 20, mr->lkey, mr->rkey);

    struct ibv_qp_init_attr init = { .send_cq = cq, .recv_cq = cq,
        .qp_type = IBV_QPT_RC, .cap = { .max_send_wr = 16, .max_recv_wr = 8,
        .max_send_sge = 1, .max_recv_sge = 1, .max_inline_data = 0 } };
    struct ibv_qp *qp = ibv_create_qp(pd, &init);
    if (!qp) { fprintf(stderr, "ibv_create_qp failed\n"); goto out_mr; }

    /* The DEXT does not invent a usable local RoCE address. Program one
     * explicitly, matching mlx_p3_gate's configure_roce step. */
    const char *ip = getenv("MELONDMA_LOCAL_IP");
    const char *macText = getenv("MELONDMA_LOCAL_MAC");
    if (!ip) ip = "192.168.200.1";
    if (!macText) macText = "98:03:9b:80:6a:94";
    struct ibv_mlx5_roce_config roce = { .hop_limit = 1 };
    uint8_t ipv4[4] = {};
    if (inet_pton(AF_INET, ip, ipv4) != 1 ||
        sscanf(macText, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
               &roce.local_mac[0], &roce.local_mac[1], &roce.local_mac[2],
               &roce.local_mac[3], &roce.local_mac[4], &roce.local_mac[5]) != 6) {
        fprintf(stderr, "bad local IP/MAC (use MELONDMA_LOCAL_IP/MAC)\n");
        goto out_qp;
    }
    memset(&roce.local_gid, 0, sizeof(roce.local_gid));
    roce.local_gid.raw[10] = 0xff;
    roce.local_gid.raw[11] = 0xff;
    memcpy(roce.local_gid.raw + 12, ipv4, sizeof(ipv4));
    roce.l3_type = 0;
    memcpy(roce.peer_mac, roce.local_mac, sizeof(roce.peer_mac));
    if (ibv_mlx5_configure_roce(ctx, &roce)) {
        fprintf(stderr, "ibv_mlx5_configure_roce failed for %s %s\n", ip, macText);
        goto out_qp;
    }
    union ibv_gid gid = roce.local_gid;
    uint8_t sgidIndex = 0;
    struct ibv_gid_entry entries[64] = {};
    uint32_t entryCount = 0, tableSize = 0;
    if (ibv_query_gid_table(ctx, 1, entries, 64, &entryCount, &tableSize)) {
        fprintf(stderr, "ibv_query_gid_table after configure failed\n");
        goto out_qp;
    }
    int gidFound = 0;
    for (uint32_t i = 0; i < entryCount; i++) {
        if (entries[i].gid_type == IBV_GID_TYPE_ROCE_V2 &&
            !memcmp(entries[i].gid.raw, gid.raw, sizeof(gid.raw))) {
            sgidIndex = (uint8_t)entries[i].gid_index;
            gidFound = 1;
            break;
        }
    }
    if (!gidFound) {
        fprintf(stderr, "configured RoCEv2 GID not found in table\n");
        goto out_qp;
    }
    printf("using configured RoCEv2 GID %s index=%u\n", ip, sgidIndex);
    uint32_t psn = ((uint32_t)getpid() * 2654435761u) & 0xffffff;

    struct ibv_qp_attr attr = { .qp_state = IBV_QPS_INIT, .pkey_index = 0,
        .port_num = 1, .qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ };
    if (ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
        fprintf(stderr, "RST->INIT failed\n"); goto out_qp; }

    /* Loopback destination: our own QPN + our own GID. */
    attr = (struct ibv_qp_attr){ .qp_state = IBV_QPS_RTR, .path_mtu = IBV_MTU_1024,
        .dest_qp_num = qp->qp_num, .rq_psn = psn, .max_dest_rd_atomic = 4,
        .min_rnr_timer = 12, .ah_attr = { .is_global = 1, .port_num = 1, .sl = 0,
        .grh = { .dgid = gid, .sgid_index = sgidIndex, .hop_limit = 1 } } };
    if (ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                      IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER)) {
        fprintf(stderr, "INIT->RTR failed\n"); goto out_qp; }
    attr = (struct ibv_qp_attr){ .qp_state = IBV_QPS_RTS, .sq_psn = psn, .timeout = 14,
        .retry_cnt = 7, .rnr_retry = 7, .max_rd_atomic = 4 };
    if (ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT |
                      IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC)) {
        fprintf(stderr, "RTR->RTS failed\n"); goto out_qp; }

    /* Fill the first chunk with a pattern, RDMA_WRITE it into the last chunk. */
    size_t dst_off = size - chunk;
    for (size_t i = 0; i < chunk; i++) buf[i] = (uint8_t)(i * 31 + 7);
    struct ibv_sge sge = { .addr = (uintptr_t)buf, .length = chunk, .lkey = mr->lkey };
    struct ibv_send_wr wr = { .wr_id = 1, .sg_list = &sge, .num_sge = 1,
        .opcode = IBV_WR_RDMA_WRITE, .send_flags = IBV_SEND_SIGNALED };
    wr.wr.rdma.remote_addr = (uintptr_t)buf + dst_off;
    wr.wr.rdma.rkey = mr->rkey;
    struct ibv_send_wr *bad = NULL;
    if (ibv_post_send(qp, &wr, &bad)) { fprintf(stderr, "ibv_post_send failed\n"); goto out_qp; }

    struct ibv_wc wc = {};
    int n = 0, spins = 0;
    for (;;) {
        n = ibv_poll_cq(cq, 1, &wc);
        if (n < 0) { fprintf(stderr, "poll_cq error\n"); goto out_qp; }
        if (n == 1) break;
        if (++spins > 5000000) { fprintf(stderr, "completion timeout\n"); goto out_qp; }
    }
    if (wc.status != IBV_WC_SUCCESS || wc.opcode != IBV_WC_RDMA_WRITE) {
        fprintf(stderr, "RDMA_WRITE failed: status=%d (%s) opcode=%d vendor=0x%x\n",
                wc.status, ibv_wc_status_str(wc.status), wc.opcode, wc.vendor_err);
        goto out_qp;
    }

    if (memcmp(buf + dst_off, buf, chunk) != 0) {
        fprintf(stderr, "LOOPBACK MISMATCH: destination bytes != source pattern\n");
        goto out_qp;
    }
    printf("loopback RDMA_WRITE OK: %zu bytes, large-MR translation correct\n", chunk);
    rc = 0;

out_qp:
    ibv_destroy_qp(qp);
out_mr:
    ibv_dereg_mr(mr);
out_buf:
    free(buf);
out_cq:
    if (cq) ibv_destroy_cq(cq);
    if (ch) ibv_destroy_comp_channel(ch);
    if (pd) ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
out_devs:
    ibv_free_device_list(devs);
    return rc;
}
