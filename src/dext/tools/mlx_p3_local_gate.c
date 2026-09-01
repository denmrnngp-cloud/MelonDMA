/* Local P3 gate (Mac, no peer): capabilities, ABI feature bits, GID-table
 * enumeration + type, CQ arming acceptance, inline posting bounds. Uses the
 * librdma_shim transport directly. Needs the DEXT activated. */
#include "librdma_shim.h"

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
static void check(int cond, const char *msg) {
    if (!cond) { failures++; printf("P3_LOCAL FAIL: %s\n", msg); }
    else printf("P3_LOCAL ok:   %s\n", msg);
}

int main(int argc, char **argv) {
    const char *local_ip = "192.168.200.1", *local_mac = "98:03:9b:80:6a:94";
    int opt;
    while ((opt = getopt(argc, argv, "l:a:")) != -1) switch (opt) {
    case 'l': local_ip = optarg; break; case 'a': local_mac = optarg; break; default: return 2;
    }
    int rc = 1;
    rdma_device *dev = rdma_open_device();
    if (!dev) { printf("P3_LOCAL FAIL: rdma_open_device (DEXT not running?)\n"); return 1; }

    /* ABI feature negotiation. */
    struct rdma_abi_attr abi = {};
    check(rdma_query_abi(dev, &abi) == 0 &&
          (abi.features & RDMA_FEATURE_INLINE) && (abi.features & RDMA_FEATURE_ATOMIC),
          "ABI advertises INLINE and ATOMIC feature bits");

    /* Device capabilities. */
    struct rdma_device_attr devattr = {};
    check(rdma_query_device(dev, &devattr) == 0 &&
          devattr.max_inline_data >= 512 &&
          devattr.max_qp_rd_atom >= 1 && devattr.max_qp_init_rd_atom >= 1,
          "query_device reports inline + atomic caps");

    /* Program a GID, then enumerate the table and check type. */
    uint8_t gid[16] = {0}; gid[10] = 0xff; gid[11] = 0xff;
    unsigned a, b, c, d;
    if (sscanf(local_ip, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
        gid[12] = (uint8_t)a; gid[13] = (uint8_t)b; gid[14] = (uint8_t)c; gid[15] = (uint8_t)d;
    }
    uint8_t mac[6] = {0};
    if (sscanf(local_mac, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) != 6) {
        printf("P3_LOCAL FAIL: bad local MAC\n"); rdma_close_device(dev); return 1;
    }
    uint32_t gid_index = 0;
    int programmed = (rdma_set_roce_address(dev, gid, mac, 0, &gid_index) == 0);
    check(programmed, "program a RoCE GID entry");

    struct rdma_gid_table_entry entries[RDMA_MAX_GID_TABLE];
    uint32_t count = 0, table_size = 0;
    int enumerated = rdma_query_gid_table(dev, entries, RDMA_MAX_GID_TABLE, &count, &table_size) == 0;
    /* CX4-Lx firmware GID table is 128-256 slots; the meaningful assertion is
     * that enumeration succeeds and finds the entry we just programmed. */
    check(enumerated && table_size >= 128, "GID table enumerates with table_size>=128");
    int found = 0, typed = 0;
    for (uint32_t i = 0; enumerated && i < count; i++) {
        if (entries[i].index == gid_index) {
            found = 1;
            if (entries[i].gid_type == 2) typed = 1;   /* RoCEv2 */
        }
    }
    check(enumerated && found && typed, "programmed GID appears with gid_type=RoCEv2");
    if (enumerated) check(count >= 1, "GID table returns >=1 programmed entry");

    struct rdma_gid_attr single = {};
    check(rdma_query_gid(dev, gid_index, &single) == 0 && single.gid_type == 2,
          "ibv_query_gid_ex path reports gid_type=RoCEv2");

    /* CQ arming: both modes must be accepted. */
    rdma_cq *cq = rdma_create_cq(dev, 64);
    check(cq != NULL, "create CQ");
    check(cq && rdma_arm_cq(cq, 0) == 0, "arm CQ (any completion) accepted");
    check(cq && rdma_arm_cq(cq, 1) == 0, "arm CQ (solicited only) accepted");

    /* QP accepts the inline capability and bounds inline posting. */
    rdma_pd *pd = rdma_alloc_pd(dev);
    struct rdma_qp_init_attr init = { .send_cq = cq, .recv_cq = cq, .qp_type = RDMA_QPT_RC,
        .cap_sq = 64, .cap_rq = 64, .max_inline_data = 512 };
    rdma_qp *qp = pd && cq ? rdma_create_qp(pd, &init) : NULL;
    check(qp != NULL, "create QP with max_inline_data=512");

    uint8_t payload[RDMA_MAX_INLINE_DATA]; memset(payload, 0x5a, sizeof(payload));
    int too_big = qp ? rdma_post_send_inline(qp, 1, RDMA_WR_SEND, payload,
        RDMA_MAX_INLINE_DATA + 1, 0, RDMA_SEND_SIGNALED | RDMA_SEND_INLINE) : -1;
    check(too_big == -EINVAL, "inline len > max_inline_data rejected with EINVAL");
    int zero_len = qp ? rdma_post_send_inline(qp, 1, RDMA_WR_SEND, payload, 0, 0,
        RDMA_SEND_SIGNALED | RDMA_SEND_INLINE) : -1;
    check(zero_len == -EINVAL, "inline zero length rejected");
    /* A valid inline WR on a RESET QP reaches the DEXT and is refused for
     * state (EAGAIN), proving the full post path is wired end to end. */
    int not_rts = qp ? rdma_post_send_inline(qp, 1, RDMA_WR_SEND, payload, 64, 0,
        RDMA_SEND_SIGNALED | RDMA_SEND_INLINE) : -1;
    check(not_rts == -EAGAIN, "inline WR reaches DEXT (EAGAIN on non-RTS QP)");

    if (qp) rdma_destroy_qp(qp);
    if (pd) rdma_dealloc_pd(pd);
    if (cq) rdma_destroy_cq(cq);
    if (programmed) (void)rdma_clear_roce_address(dev, gid_index);
    rdma_close_device(dev);

    if (failures) { printf("\nP3_LOCAL: %d check(s) FAILED\n", failures); rc = 1; }
    else { printf("\nP3_LOCAL_GATE PASS\n"); rc = 0; }
    return rc;
}
