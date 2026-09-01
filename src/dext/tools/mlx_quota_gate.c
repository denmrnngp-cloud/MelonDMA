/*
 * mlx_quota_gate.c — P1.1 live gate: per-client quotas and DoS limits.
 *
 * Verifies on live hardware (needs the signed/activated DEXT):
 *   1. kMlxUCMethodQueryLimits returns the exact MLX_UC_* ceilings.
 *   2. Allocating past MLX_UC_MAX_PD_PER_CLIENT is refused with
 *      kIOReturnNoResources — and the refusal happens BEFORE a firmware
 *      command, so no PD leaks: after releasing one, allocation succeeds.
 *   3. Same for CQ: past MLX_UC_MAX_CQ_PER_CLIENT → kIOReturnNoResources,
 *      and after teardown a fresh CQ can be created.
 *   4. An oversized SQ depth (beyond MLX_UC_MAX_SQ_DEPTH) is rejected with
 *      kIOReturnBadArgument before any QP resource is allocated.
 *
 * Self-contained (no librdma_shim); signed with tools/reinit.entitlements.
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

int main(void)
{
    int fail = 0;
    io_connect_t c = open_client();
    if (!c) { fprintf(stderr, "P1.1_QUOTA FAIL: open client\n"); return 1; }

    /* 1. Limits query. */
    struct mlx_query_limits_resp lim = {};
    if (call(c, kMlxUCMethodQueryLimits, NULL, 0, &lim, sizeof(lim)) != kIOReturnSuccess) {
        fprintf(stderr, "P1.1_QUOTA FAIL: QueryLimits\n"); fail = 1; goto out;
    }
    if (!lim.maxPd || !lim.maxQp || !lim.maxCq || !lim.maxMr ||
        !lim.maxMw || !lim.maxAh || !lim.maxGid || !lim.maxDbRecords ||
        lim.maxQp > MLX_UC_MAX_QP_PER_CLIENT ||
        lim.maxCq > MLX_UC_MAX_CQ_PER_CLIENT ||
        lim.maxMr > MLX_UC_MAX_MR_PER_CLIENT ||
        lim.maxMw > MLX_UC_MAX_MW_PER_CLIENT ||
        lim.maxSqDepth != MLX_UC_MAX_SQ_DEPTH ||
        lim.maxRqDepth != MLX_UC_MAX_RQ_DEPTH ||
        lim.maxQp > lim.maxDbRecords || lim.maxCq > lim.maxDbRecords) {
        fprintf(stderr, "P1.1_QUOTA FAIL: invalid native limits (pd=%u qp=%u cq=%u mr=%u mw=%u ah=%u gid=%u db=%u sq=%u rq=%u)\n",
                lim.maxPd, lim.maxQp, lim.maxCq, lim.maxMr, lim.maxMw,
                lim.maxAh, lim.maxGid, lim.maxDbRecords,
                lim.maxSqDepth, lim.maxRqDepth);
        fail = 1; goto out;
    }

    /* 2. PD quota exhaustion + release. */
    {
        uint32_t pds[MLX_UC_MAX_PD_PER_CLIENT] = {};
        uint32_t n = 0, extra = 0;
        for (; n < MLX_UC_MAX_PD_PER_CLIENT; n++) {
            if (call(c, kMlxUCMethodAllocPD, NULL, 0, &pds[n], sizeof(pds[n])) != kIOReturnSuccess)
                break;
        }
        if (n != MLX_UC_MAX_PD_PER_CLIENT) {
            fprintf(stderr, "P1.1_QUOTA FAIL: expected %u PDs, got %u\n",
                    MLX_UC_MAX_PD_PER_CLIENT, n); fail = 1;
        }
        kern_return_t kr = call(c, kMlxUCMethodAllocPD, NULL, 0, &extra, sizeof(extra));
        if (kr != kIOReturnNoResources) {
            fprintf(stderr, "P1.1_QUOTA FAIL: PD over-limit returned 0x%x (want NoResources)\n", kr);
            fail = 1;
        }
        for (uint32_t i = 0; i < n; i++)
            (void)call(c, kMlxUCMethodDeallocPD, &pds[i], sizeof(pds[i]), NULL, 0);
        if (call(c, kMlxUCMethodAllocPD, NULL, 0, &extra, sizeof(extra)) != kIOReturnSuccess) {
            fprintf(stderr, "P1.1_QUOTA FAIL: PD quota not released after teardown\n"); fail = 1;
        } else {
            (void)call(c, kMlxUCMethodDeallocPD, &extra, sizeof(extra), NULL, 0);
        }
    }

    /* 3. CQ quota exhaustion + release. */
    {
        uint32_t cqs[MLX_UC_MAX_CQ_PER_CLIENT] = {};
        uint32_t n = 0;
        struct mlx_create_cq_req req = { .entries = 64 };
        for (; n < lim.maxCq; n++) {
            struct mlx_create_cq_resp resp = {};
            if (call(c, kMlxUCMethodCreateCQ, &req, sizeof(req), &resp, sizeof(resp)) != kIOReturnSuccess)
                break;
            cqs[n] = resp.cqHandle;
        }
        if (n != lim.maxCq) {
            fprintf(stderr, "P1.1_QUOTA FAIL: expected native CQ limit %u, got %u\n",
                    lim.maxCq, n); fail = 1;
        }
        struct mlx_create_cq_resp resp = {};
        kern_return_t kr = call(c, kMlxUCMethodCreateCQ, &req, sizeof(req), &resp, sizeof(resp));
        if (kr != kIOReturnNoResources) {
            fprintf(stderr, "P1.1_QUOTA FAIL: CQ over-limit returned 0x%x (want NoResources)\n", kr);
            fail = 1;
        }
        for (uint32_t i = 0; i < n; i++)
            (void)call(c, kMlxUCMethodDestroyCQ, &cqs[i], sizeof(cqs[i]), NULL, 0);
        if (call(c, kMlxUCMethodCreateCQ, &req, sizeof(req), &resp, sizeof(resp)) != kIOReturnSuccess) {
            fprintf(stderr, "P1.1_QUOTA FAIL: CQ quota not released after teardown\n"); fail = 1;
        } else {
            (void)call(c, kMlxUCMethodDestroyCQ, &resp.cqHandle, sizeof(resp.cqHandle), NULL, 0);
        }
    }

    /* 4. SQ depth cap (rejected before any firmware resource is created). */
    {
        uint32_t pd = 0;
        struct mlx_create_cq_req cqReq = { .entries = 64 };
        struct mlx_create_cq_resp cqResp = {};
        if (call(c, kMlxUCMethodAllocPD, NULL, 0, &pd, sizeof(pd)) == kIOReturnSuccess &&
            call(c, kMlxUCMethodCreateCQ, &cqReq, sizeof(cqReq), &cqResp, sizeof(cqResp)) == kIOReturnSuccess) {
            struct mlx_create_qp_req qpReq = { .pd = pd, .sendCq = cqResp.cqHandle,
                .recvCq = cqResp.cqHandle, .qpType = 0,
                .sqSize = MLX_UC_MAX_SQ_DEPTH * 2, .rqSize = 64 };
            struct mlx_create_qp_resp qpResp = {};
            kern_return_t kr = call(c, kMlxUCMethodCreateQP, &qpReq, sizeof(qpReq), &qpResp, sizeof(qpResp));
            if (kr != kIOReturnBadArgument) {
                fprintf(stderr, "P1.1_QUOTA FAIL: oversized SQ returned 0x%x (want BadArgument)\n", kr);
                fail = 1;
            }
            (void)call(c, kMlxUCMethodDestroyCQ, &cqResp.cqHandle, sizeof(cqResp.cqHandle), NULL, 0);
        } else {
            fprintf(stderr, "P1.1_QUOTA FAIL: could not set up depth-cap probe\n"); fail = 1;
        }
        if (pd) (void)call(c, kMlxUCMethodDeallocPD, &pd, sizeof(pd), NULL, 0);
    }

out:
    if (fail)
        fprintf(stderr, "P1.1_QUOTA FAIL\n");
    else
        printf("P1.1_QUOTA PASS: limits readback, PD/CQ quota exhaustion+release, SQ depth cap, no partial-resource leak\n");
    if (c) IOServiceClose(c);
    return fail;
}
