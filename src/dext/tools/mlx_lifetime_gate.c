/*
 * mlx_lifetime_gate.c — P0.3 lifetime / stale-handle hardening gate.
 *
 * Exercises the opaque per-UserClient token ABI (MlxUCIO.h v2) directly, the
 * same way mlx_isolation_gate.c does:
 *   1. stale token denied after destroy, for PD/CQ/QP/MR/MW;
 *   2. stale token denied after raw-ID reuse (generation bump);
 *   3. cross-client token isolation;
 *   4. in-flight QP destroy returns kIOReturnBusy.
 *
 * MW->MR dependency (DeregMR busy while a window is bound) is exercised by
 * the existing PHASE2_BIND_MW_GATE path in the P0 matrix; the DeregMR busy and
 * DeallocMW decrement code paths are source-verified in MlxMR.cpp.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
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

static kern_return_t call(io_connect_t c, uint32_t sel, const void *in,
                          size_t inSize, void *out, size_t outSize)
{
    size_t n = outSize;
    return IOConnectCallStructMethod(c, sel, in, inSize, out, &n);
}

static int denied(io_connect_t c, uint32_t sel, const void *in, size_t size)
{
    return call(c, sel, in, size, NULL, 0) == kIOReturnNotPermitted;
}

static int alloc_pd(io_connect_t c, uint32_t *pd)
{
    return call(c, kMlxUCMethodAllocPD, NULL, 0, pd, sizeof(*pd)) ==
               kIOReturnSuccess && pd && *pd ? 0 : 1;
}
static int dealloc_pd(io_connect_t c, uint32_t pd)
{
    return call(c, kMlxUCMethodDeallocPD, &pd, sizeof(pd), NULL, 0) ==
               kIOReturnSuccess ? 0 : 1;
}
static int create_cq(io_connect_t c, uint32_t *cq)
{
    struct mlx_create_cq_req req = { .entries = 64 };
    struct mlx_create_cq_resp resp = {};
    if (call(c, kMlxUCMethodCreateCQ, &req, sizeof(req), &resp,
             sizeof(resp)) != kIOReturnSuccess) return 1;
    *cq = resp.cqHandle;
    return *cq ? 0 : 1;
}
static int destroy_cq(io_connect_t c, uint32_t cq)
{
    return call(c, kMlxUCMethodDestroyCQ, &cq, sizeof(cq), NULL, 0) ==
               kIOReturnSuccess ? 0 : 1;
}
static int reg_mr(io_connect_t c, uint32_t pd, void *buf, uint32_t *mr,
                  uint32_t *lkey)
{
    struct mlx_reg_mr_req req = { .startAddr = (uint64_t)(uintptr_t)buf,
        .length = 4096, .accessFlags = 1u /* LOCAL_WRITE */, .pd = pd };
    struct mlx_reg_mr_resp resp = {};
    if (call(c, kMlxUCMethodRegMR, &req, sizeof(req), &resp,
             sizeof(resp)) != kIOReturnSuccess) return 1;
    *mr = resp.mrHandle; *lkey = resp.lkey;
    return (*mr && *lkey) ? 0 : 1;
}
static int dereg_mr(io_connect_t c, uint32_t mr)
{
    return call(c, kMlxUCMethodDeregMR, &mr, sizeof(mr), NULL, 0) ==
               kIOReturnSuccess ? 0 : 1;
}
static int alloc_mw(io_connect_t c, uint32_t pd, uint32_t *mw)
{
    struct mlx_alloc_mw_req req = { .pd = pd, .type = 2 };
    struct mlx_alloc_mw_resp resp = {};
    if (call(c, kMlxUCMethodAllocMW, &req, sizeof(req), &resp,
             sizeof(resp)) != kIOReturnSuccess) return 1;
    *mw = resp.mwHandle;
    return *mw ? 0 : 1;
}
static int dealloc_mw(io_connect_t c, uint32_t mw)
{
    struct mlx_dealloc_mw_req req = { .mwHandle = mw };
    return call(c, kMlxUCMethodDeallocMW, &req, sizeof(req), NULL, 0) ==
               kIOReturnSuccess ? 0 : 1;
}
static int create_qp(io_connect_t c, uint32_t pd, uint32_t cq, uint32_t *qp)
{
    struct mlx_create_qp_req req = { .pd = pd, .sendCq = cq, .recvCq = cq,
        .qpType = 0, .sqSize = 64, .rqSize = 64 };
    struct mlx_create_qp_resp resp = {};
    if (call(c, kMlxUCMethodCreateQP, &req, sizeof(req), &resp,
             sizeof(resp)) != kIOReturnSuccess) return 1;
    *qp = resp.qpn;
    return *qp ? 0 : 1;
}
static int destroy_qp(io_connect_t c, uint32_t qp)
{
    return call(c, kMlxUCMethodDestroyQP, &qp, sizeof(qp), NULL, 0) ==
               kIOReturnSuccess ? 0 : 1;
}
static int modify_init(io_connect_t c, uint32_t qp)
{
    struct mlx_modify_qp_req req = { .qpn = qp, .curState = 0, .newState = 1,
        .pkeyIndex = 0, .portNum = 1 };
    return call(c, kMlxUCMethodModifyQP, &req, sizeof(req), NULL, 0) ==
               kIOReturnSuccess ? 0 : 1;
}
static int post_recv(io_connect_t c, uint32_t qp, uint32_t lkey, void *buf)
{
    struct mlx_post_recv_req req = { .qpn = qp,
        .sge = { .addr = (uint64_t)(uintptr_t)buf, .length = 4096,
                 .lkey = lkey } };
    return call(c, kMlxUCMethodPostRecv, &req, sizeof(req), NULL, 0) ==
               kIOReturnSuccess ? 0 : 1;
}

/* Concurrency hammer (P0.3): threads allocate+free PDs on one client,
 * stressing token-map slot reuse and ownership tables under contention. */
#define MLX_CONC_THREADS 8
#define MLX_CONC_ITERS   300

static void *conc_pd_hammer(void *arg)
{
    io_connect_t c = (io_connect_t)(uintptr_t)arg;
    for (int i = 0; i < MLX_CONC_ITERS; i++) {
        uint32_t pd = 0;
        if (alloc_pd(c, &pd) || dealloc_pd(c, pd))
            return (void *)(uintptr_t)1;
    }
    return NULL;
}

struct dbl_destroy_arg { io_connect_t c; uint32_t token; int success; };

static void *dbl_destroy(void *arg)
{
    struct dbl_destroy_arg *d = (struct dbl_destroy_arg *)arg;
    d->success = dealloc_pd(d->c, d->token) ? 0 : 1;
    return NULL;
}

int main(void)
{
    int fails = 0;
    io_connect_t a = open_client();
    if (!a) { fprintf(stderr, "P0.3_LIFETIME FAIL: open client A\n"); return 1; }

    struct mlx_query_abi_resp abi = {};
    if (call(a, kMlxUCMethodQueryAbi, NULL, 0, &abi, sizeof(abi)) !=
            kIOReturnSuccess || abi.version != MLX_UC_ABI_VERSION) {
        fprintf(stderr, "P0.3_LIFETIME FAIL: DEXT ABI v%u, need v%u\n",
                abi.version, MLX_UC_ABI_VERSION);
        return 1;
    }

    /* 1. stale token denied after destroy, per type. */
    uint32_t pd = 0, cq = 0, mr = 0, lkey = 0, mw = 0, qp = 0;
    void *buf = NULL;
    if (posix_memalign(&buf, 4096, 4096)) { fprintf(stderr, "P0.3_LIFETIME FAIL: alloc\n"); return 1; }

    if (alloc_pd(a, &pd)) { fprintf(stderr, "P0.3_LIFETIME FAIL: alloc PD\n"); fails++; goto out; }
    if (dealloc_pd(a, pd)) { fprintf(stderr, "P0.3_LIFETIME FAIL: dealloc PD\n"); fails++; }
    if (!denied(a, kMlxUCMethodDeallocPD, &pd, sizeof(pd))) {
        fprintf(stderr, "P0.3_LIFETIME FAIL: stale PD token accepted\n"); fails++;
    }

    if (alloc_pd(a, &pd)) { fprintf(stderr, "P0.3_LIFETIME FAIL: alloc PD2\n"); fails++; goto out; }
    if (create_cq(a, &cq)) { fprintf(stderr, "P0.3_LIFETIME FAIL: create CQ\n"); fails++; goto out; }
    if (destroy_cq(a, cq)) { fprintf(stderr, "P0.3_LIFETIME FAIL: destroy CQ\n"); fails++; }
    if (!denied(a, kMlxUCMethodDestroyCQ, &cq, sizeof(cq))) {
        fprintf(stderr, "P0.3_LIFETIME FAIL: stale CQ token accepted\n"); fails++;
    }

    if (create_cq(a, &cq)) { fprintf(stderr, "P0.3_LIFETIME FAIL: create CQ2\n"); fails++; goto out; }
    if (reg_mr(a, pd, buf, &mr, &lkey)) { fprintf(stderr, "P0.3_LIFETIME FAIL: reg MR\n"); fails++; goto out; }
    if (dereg_mr(a, mr)) { fprintf(stderr, "P0.3_LIFETIME FAIL: dereg MR\n"); fails++; }
    if (!denied(a, kMlxUCMethodDeregMR, &mr, sizeof(mr))) {
        fprintf(stderr, "P0.3_LIFETIME FAIL: stale MR token accepted\n"); fails++;
    }

    if (alloc_mw(a, pd, &mw)) { fprintf(stderr, "P0.3_LIFETIME FAIL: alloc MW\n"); fails++; goto out; }
    if (dealloc_mw(a, mw)) { fprintf(stderr, "P0.3_LIFETIME FAIL: dealloc MW\n"); fails++; }
    if (!denied(a, kMlxUCMethodDeallocMW,
                &((struct mlx_dealloc_mw_req){ .mwHandle = mw }),
                sizeof(struct mlx_dealloc_mw_req))) {
        fprintf(stderr, "P0.3_LIFETIME FAIL: stale MW token accepted\n"); fails++;
    }

    if (create_qp(a, pd, cq, &qp)) { fprintf(stderr, "P0.3_LIFETIME FAIL: create QP\n"); fails++; goto out; }
    if (destroy_qp(a, qp)) { fprintf(stderr, "P0.3_LIFETIME FAIL: destroy QP\n"); fails++; }
    if (!denied(a, kMlxUCMethodDestroyQP, &qp, sizeof(qp))) {
        fprintf(stderr, "P0.3_LIFETIME FAIL: stale QP token accepted\n"); fails++;
    }

    /* 2. generation bump: stale token stays dead after the slot is reused. */
    uint32_t p1 = 0, p2 = 0;
    if (alloc_pd(a, &p1)) { fprintf(stderr, "P0.3_LIFETIME FAIL: alloc P1\n"); fails++; goto out; }
    if (dealloc_pd(a, p1)) { fprintf(stderr, "P0.3_LIFETIME FAIL: dealloc P1\n"); fails++; }
    if (alloc_pd(a, &p2)) { fprintf(stderr, "P0.3_LIFETIME FAIL: alloc P2\n"); fails++; goto out; }
    if (!denied(a, kMlxUCMethodDeallocPD, &p1, sizeof(p1))) {
        fprintf(stderr, "P0.3_LIFETIME FAIL: reused-slot stale PD accepted\n"); fails++;
    }
    if (dealloc_pd(a, p2)) { fprintf(stderr, "P0.3_LIFETIME FAIL: dealloc P2\n"); fails++; }

    /* 3. cross-client token isolation. */
    io_connect_t b = open_client();
    if (!b) { fprintf(stderr, "P0.3_LIFETIME FAIL: open client B\n"); fails++; goto out; }
    uint32_t a_pd = 0, b_pd = 0;
    if (alloc_pd(a, &a_pd)) { fprintf(stderr, "P0.3_LIFETIME FAIL: alloc A PD\n"); fails++; }
    if (alloc_pd(b, &b_pd)) { fprintf(stderr, "P0.3_LIFETIME FAIL: alloc B PD\n"); fails++; }
    if (!denied(b, kMlxUCMethodDeallocPD, &a_pd, sizeof(a_pd))) {
        fprintf(stderr, "P0.3_LIFETIME FAIL: cross-client token accepted\n"); fails++;
    }
    dealloc_pd(a, a_pd);
    dealloc_pd(b, b_pd);
    IOServiceClose(b);

    /* 3b. concurrent create/destroy hammer + double-destroy atomicity. */
    {
        pthread_t th[MLX_CONC_THREADS];
        int spawned = 0;
        for (int t = 0; t < MLX_CONC_THREADS; t++)
            if (pthread_create(&th[t], NULL, conc_pd_hammer,
                               (void *)(uintptr_t)a)) {
                fprintf(stderr, "P0.3_LIFETIME FAIL: spawn hammer thread\n");
                fails++;
                break;
            } else spawned++;
        for (int t = 0; t < spawned; t++) {
            void *ret = NULL;
            pthread_join(th[t], &ret);
            if (ret != NULL) {
                fprintf(stderr, "P0.3_LIFETIME FAIL: concurrent create/destroy\n");
                fails++;
            }
        }

        uint32_t dd = 0;
        if (alloc_pd(a, &dd)) {
            fprintf(stderr, "P0.3_LIFETIME FAIL: alloc dd\n");
            fails++;
        } else {
            struct dbl_destroy_arg d1 = { a, dd, 0 }, d2 = { a, dd, 0 };
            pthread_t t1, t2;
            if (pthread_create(&t1, NULL, dbl_destroy, &d1) ||
                pthread_create(&t2, NULL, dbl_destroy, &d2)) {
                fprintf(stderr, "P0.3_LIFETIME FAIL: spawn dbl_destroy\n");
                fails++;
            } else {
                pthread_join(t1, NULL);
                pthread_join(t2, NULL);
                if (d1.success + d2.success != 1) {
                    fprintf(stderr, "P0.3_LIFETIME FAIL: concurrent double-destroy (successes=%d)\n",
                            d1.success + d2.success);
                    fails++;
                }
            }
        }
    }

    /* 4. in-flight QP destroy busy (last; leaves an in-flight QP for DEXT
     * teardown on close — the gate runs once per fresh DEXT activation). */
    uint32_t icq = 0, ipd = 0, imr = 0, ilkey = 0, iqp = 0;
    if (alloc_pd(a, &ipd) || create_cq(a, &icq) ||
        reg_mr(a, ipd, buf, &imr, &ilkey) || create_qp(a, ipd, icq, &iqp) ||
        modify_init(a, iqp) || post_recv(a, iqp, ilkey, buf)) {
        fprintf(stderr, "P0.3_LIFETIME FAIL: in-flight setup\n"); fails++;
        goto out;
    }
    if (call(a, kMlxUCMethodDestroyQP, &iqp, sizeof(iqp), NULL, 0) !=
        kIOReturnBusy) {
        fprintf(stderr, "P0.3_LIFETIME FAIL: in-flight QP destroy not busy\n");
        fails++;
    }

out:
    if (a) IOServiceClose(a);
    free(buf);
    if (fails) { fprintf(stderr, "P0.3_LIFETIME FAIL\n"); return 1; }
    printf("P0.3_LIFETIME PASS: stale-token denial (PD/CQ/QP/MR/MW), "
           "generation bump, cross-client isolation, and in-flight destroy busy\n");
    return 0;
}
