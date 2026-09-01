/*
 * test_shim.c — environment-independent smoke test for librdma_shim.dylib.
 *
 * A DEXT may or may not be active on the build host. This verifies that the
 * shim links, enumeration is internally consistent, and unavailable/open
 * paths are graceful. Hardware readiness is covered by mlx_phase2_gate
 * --preflight rather than by this portable target.
 */
#include "librdma_shim.h"
#include <stdio.h>
#include <stdlib.h>

static int failures = 0;
static void check(int cond, const char *msg)
{
    if (!cond) { failures++; printf("FAIL: %s\n", msg); }
    else        printf("ok:   %s\n", msg);
}

int main(void)
{
    /* Either a valid handle or graceful unavailability is acceptable here. */
    rdma_device *dev = rdma_open_device();
    check(1, dev ? "rdma_open_device opened live DEXT"
                 : "rdma_open_device unavailable gracefully");

    /* Enumeration must agree with ownership of its returned name table. */
    char **names = NULL; int count = -1;
    int rc = rdma_list_devices(&names, &count);
    check(rc == 0 && count >= 0 && ((count == 0 && names == NULL) ||
                                   (count > 0 && names != NULL)),
          "rdma_list_devices result is internally consistent");

    /* NULL-input guards do not crash. */
    check(rdma_query_abi(NULL, NULL) < 0, "query_abi(NULL) guarded");
    check(rdma_query_device(NULL, NULL) < 0, "query_device(NULL) guarded");
    check(rdma_query_health(NULL, NULL) < 0, "query_health(NULL) guarded");
    if (dev) {
        struct rdma_health_attr health = {};
        check(rdma_query_health(dev, &health) == 0 && health.healthy == 1,
              "live health snapshot is healthy");
    }
    check(rdma_alloc_pd(NULL) == NULL, "alloc_pd(NULL) guarded");
    check(rdma_create_cq(NULL, 64) == NULL, "create_cq(NULL) guarded");
    check(rdma_reg_mr(NULL, NULL, 0, 0, NULL) == NULL, "reg_mr(NULL) guarded");
    check(rdma_create_ah(NULL, NULL) == NULL, "create_ah(NULL) guarded");
    check(rdma_modify_qp(NULL, NULL) < 0, "modify_qp(NULL) guarded");
    check(rdma_poll_cq(NULL, NULL, 0) < 0, "poll_cq(NULL) guarded");
    check(rdma_post_send(NULL, NULL) < 0, "post_send(NULL) guarded");
    check(rdma_post_send_batch(NULL, NULL, 0) < 0,
          "post_send_batch(NULL) guarded");
    check(rdma_post_recv_batch(NULL, NULL, 0) < 0,
          "post_recv_batch(NULL) guarded");

    rdma_free_names(names, count);
    rdma_close_device(dev);

    if (failures) { printf("\n%d shim test(s) FAILED\n", failures); return 1; }
    printf("\nALL shim smoke tests passed\n");
    return 0;
}
