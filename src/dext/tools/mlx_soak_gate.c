/*
 * mlx_soak_gate.c — P1.4 live gate: long resource-lifecycle soak with counters.
 *
 * No peer needed. Repeatedly creates a full per-client resource set (PD, CQ,
 * MR, MW, QP), verifies the owned-resource counters via kMlxUCMethodQueryHealth
 * at each step, tears everything down, and verifies the counters return to
 * zero — catching counter/ownership drift, quota leaks and health degradation
 * over many cycles. Also exercises the P1.1 quota release paths end-to-end.
 *
 * Usage: mlx_soak_gate [iterations]   (default 100)
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <IOKit/IOKitLib.h>
#include "MlxServiceMatch.h"
#include "MlxUCIO.h"

static io_connect_t open_client(void)
{
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault,
                                                        mlxCreateServiceMatching());
    if (!service) return IO_OBJECT_NULL;
    io_connect_t c = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &c);
    IOObjectRelease(service);
    return kr == kIOReturnSuccess ? c : IO_OBJECT_NULL;
}

static kern_return_t call(io_connect_t c, uint32_t sel, const void *in, size_t inSize,
                          void *out, size_t outSize)
{
    size_t n = outSize;
    return IOConnectCallStructMethod(c, sel, in, inSize, out, &n);
}

static int read_health(io_connect_t c, struct mlx_health_resp *h)
{
    return call(c, kMlxUCMethodQueryHealth, NULL, 0, h, sizeof(*h)) == kIOReturnSuccess;
}

static int make_set(io_connect_t c, void *buf, uint32_t *pd, uint32_t *cq,
                    uint32_t *mr, uint32_t *mw, uint32_t *qp)
{
    struct mlx_create_cq_req cqReq = { .entries = 64 };
    struct mlx_create_cq_resp cqResp = {};
    if (call(c, kMlxUCMethodAllocPD, NULL, 0, pd, sizeof(*pd)) != kIOReturnSuccess ||
        call(c, kMlxUCMethodCreateCQ, &cqReq, sizeof(cqReq), &cqResp, sizeof(cqResp)) != kIOReturnSuccess)
        return 1;
    *cq = cqResp.cqHandle;
    struct mlx_reg_mr_req mrReq = { .startAddr = (uint64_t)(uintptr_t)buf,
        .length = 4096, .accessFlags = 1u | (1u << 1) | (1u << 4), .pd = *pd };
    struct mlx_reg_mr_resp mrResp = {};
    if (call(c, kMlxUCMethodRegMR, &mrReq, sizeof(mrReq), &mrResp, sizeof(mrResp)) != kIOReturnSuccess)
        return 1;
    *mr = mrResp.mrHandle;
    struct mlx_alloc_mw_req mwReq = { .pd = *pd, .type = 2 };
    struct mlx_alloc_mw_resp mwResp = {};
    if (call(c, kMlxUCMethodAllocMW, &mwReq, sizeof(mwReq), &mwResp, sizeof(mwResp)) != kIOReturnSuccess)
        return 1;
    *mw = mwResp.mwHandle;
    struct mlx_create_qp_req qpReq = { .pd = *pd, .sendCq = *cq, .recvCq = *cq,
        .qpType = 0, .sqSize = 64, .rqSize = 64 };
    struct mlx_create_qp_resp qpResp = {};
    if (call(c, kMlxUCMethodCreateQP, &qpReq, sizeof(qpReq), &qpResp, sizeof(qpResp)) != kIOReturnSuccess)
        return 1;
    *qp = qpResp.qpn;
    return 0;
}

static void destroy_set(io_connect_t c, uint32_t qp, uint32_t cq, uint32_t mr,
                        uint32_t mw, uint32_t pd)
{
    if (qp) (void)call(c, kMlxUCMethodDestroyQP, &qp, sizeof(qp), NULL, 0);
    if (mw) { struct mlx_dealloc_mw_req r = { mw }; (void)call(c, kMlxUCMethodDeallocMW, &r, sizeof(r), NULL, 0); }
    if (mr) (void)call(c, kMlxUCMethodDeregMR, &mr, sizeof(mr), NULL, 0);
    if (cq) (void)call(c, kMlxUCMethodDestroyCQ, &cq, sizeof(cq), NULL, 0);
    if (pd) (void)call(c, kMlxUCMethodDeallocPD, &pd, sizeof(pd), NULL, 0);
}

int main(int argc, char **argv)
{
    unsigned iterations = 100;
    if (argc > 1) {
        char *end = NULL;
        errno = 0;
        unsigned long p = strtoul(argv[1], &end, 10);
        if (errno || !end || *end || p < 1 || p > 1000000) {
            fprintf(stderr, "usage: %s [iterations 1..1000000]\n", argv[0]);
            return 2;
        }
        iterations = (unsigned)p;
    }

    io_connect_t c = open_client();
    if (!c) { fprintf(stderr, "P1.4_SOAK FAIL: open client\n"); return 1; }
    void *buf = NULL;
    if (posix_memalign(&buf, 4096, 4096)) { IOServiceClose(c); return 1; }

    struct mlx_health_resp h = {};
    if (!read_health(c, &h) || !h.healthy) {
        fprintf(stderr, "P1.4_SOAK FAIL: pre-soak health not healthy\n");
        free(buf); IOServiceClose(c); return 1;
    }

    int fail = 0;
    for (unsigned i = 0; i < iterations && !fail; i++) {
        uint32_t pd = 0, cq = 0, mr = 0, mw = 0, qp = 0;
        if (make_set(c, buf, &pd, &cq, &mr, &mw, &qp)) {
            fprintf(stderr, "P1.4_SOAK FAIL: iteration %u resource creation\n", i);
            fail = 1;
            destroy_set(c, qp, cq, mr, mw, pd);
            break;
        }
        if (!read_health(c, &h) || !h.healthy ||
            h.ownedPd != 1 || h.ownedQp != 1 || h.ownedCq != 1 ||
            h.ownedMr != 2 || h.ownedAh != 0) {
            fprintf(stderr, "P1.4_SOAK FAIL: iteration %u post-create counters "
                    "(healthy=%u pd=%u qp=%u cq=%u mr+mw=%u ah=%u synd=0x%x)\n",
                    i, h.healthy, h.ownedPd, h.ownedQp, h.ownedCq,
                    h.ownedMr, h.ownedAh, h.syndrome);
            fail = 1;
        }
        destroy_set(c, qp, cq, mr, mw, pd);
        if (!read_health(c, &h) || !h.healthy ||
            h.ownedPd != 0 || h.ownedQp != 0 || h.ownedCq != 0 ||
            h.ownedMr != 0 || h.ownedAh != 0) {
            fprintf(stderr, "P1.4_SOAK FAIL: iteration %u post-teardown counters "
                    "(healthy=%u pd=%u qp=%u cq=%u mr+mw=%u ah=%u)\n",
                    i, h.healthy, h.ownedPd, h.ownedQp, h.ownedCq,
                    h.ownedMr, h.ownedAh);
            fail = 1;
        }
        if ((i % 1000) == 999)
            printf("  soak progress: %u/%u iterations, healthy=%u\n",
                   i + 1, iterations, h.healthy);
    }

    free(buf);
    IOServiceClose(c);
    if (fail) {
        fprintf(stderr, "P1.4_SOAK FAIL\n");
        return 1;
    }
    printf("P1.4_SOAK PASS: %u create/teardown cycles, owned counters returned "
           "to zero each cycle, health stayed healthy\n", iterations);
    return 0;
}
