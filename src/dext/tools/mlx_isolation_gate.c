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

static int denied(io_connect_t c, uint32_t sel, const void *in, size_t size)
{
    kern_return_t kr = call(c, sel, in, size, NULL, 0);
    return kr == kIOReturnNotPermitted;
}

static void destroy_all(io_connect_t c, uint32_t qp, uint32_t cq, uint32_t mr, uint32_t mw, uint32_t pd)
{
    if (qp) (void)call(c, kMlxUCMethodDestroyQP, &qp, sizeof(qp), NULL, 0);
    if (mw) { struct mlx_dealloc_mw_req r = { mw }; (void)call(c, kMlxUCMethodDeallocMW, &r, sizeof(r), NULL, 0); }
    if (mr) (void)call(c, kMlxUCMethodDeregMR, &mr, sizeof(mr), NULL, 0);
    if (cq) (void)call(c, kMlxUCMethodDestroyCQ, &cq, sizeof(cq), NULL, 0);
    if (pd) (void)call(c, kMlxUCMethodDeallocPD, &pd, sizeof(pd), NULL, 0);
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

int main(void)
{
    io_connect_t a = open_client(), b = open_client();
    if (!a || !b) { fprintf(stderr, "P0.2_ISOLATION FAIL: opening two clients\n"); return 1; }
    void *bufA = NULL, *bufB = NULL;
    if (posix_memalign(&bufA, 4096, 4096) || posix_memalign(&bufB, 4096, 4096)) return 1;
    uint32_t apd=0, acq=0, amr=0, amw=0, aqp=0, bpd=0, bcq=0, bmr=0, bmw=0, bqp=0;
    int fail = make_set(a, bufA, &apd, &acq, &amr, &amw, &aqp) ||
               make_set(b, bufB, &bpd, &bcq, &bmr, &bmw, &bqp);
    if (fail) { fprintf(stderr, "P0.2_ISOLATION FAIL: resource creation\n"); goto out; }
    int cross = denied(b, kMlxUCMethodDeallocPD, &apd, sizeof(apd)) &&
                denied(b, kMlxUCMethodDestroyCQ, &acq, sizeof(acq)) &&
                denied(b, kMlxUCMethodDeregMR, &amr, sizeof(amr)) &&
                denied(b, kMlxUCMethodDeallocMW, &((struct mlx_dealloc_mw_req){amw}), sizeof(struct mlx_dealloc_mw_req)) &&
                denied(b, kMlxUCMethodDestroyQP, &aqp, sizeof(aqp));
    if (!cross) { fprintf(stderr, "P0.2_ISOLATION FAIL: cross-client handle accepted\n"); goto out; }
    if (call(a, kMlxUCMethodDestroyCQ, &acq, sizeof(acq), NULL, 0) != kIOReturnBusy) {
        fprintf(stderr, "P0.2_ISOLATION FAIL: CQ teardown dependency\n"); goto out;
    }
    printf("P0.2_ISOLATION PASS: two UserClients, independent PD/CQ/MR/MW/QP sets, cross-client denial, and CQ dependency enforcement\n");
out:
    destroy_all(a, aqp, acq, amr, amw, apd);
    destroy_all(b, bqp, bcq, bmr, bmw, bpd);
    free(bufA); free(bufB);
    if (a) IOServiceClose(a); if (b) IOServiceClose(b);
    return fail ? 1 : (cross ? 0 : 1);
}
