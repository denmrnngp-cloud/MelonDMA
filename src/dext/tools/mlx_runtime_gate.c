/* Non-destructive live P0/P1 checks. Requires runtime-status ABI. */
#include "MlxUCIO.h"
#include "MlxServiceMatch.h"
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static kern_return_t call(io_connect_t c, uint32_t method, const void *in,
                          size_t insize, void *out, size_t outsize) {
    return IOConnectCallStructMethod(c, method, in, insize, out, &outsize);
}
static int snapshot(io_connect_t c, struct mlx_runtime_resp *r) {
    return call(c, kMlxUCMethodQueryRuntime, NULL, 0, r, sizeof(*r)) ||
           r->version != MLX_RUNTIME_VERSION || r->size != sizeof(*r) ||
           !r->deviceEpoch || r->quarantined;
}
int main(int argc, char **argv) {
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, mlxCreateServiceMatching());
    io_connect_t c = IO_OBJECT_NULL;
    if (!service) return 2;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &c);
    IOObjectRelease(service);
    if (kr) { printf("open failed: 0x%x\n", kr); return 2; }
    struct mlx_runtime_resp before = {}, r = {};
    if (snapshot(c, &before)) { puts("runtime ABI unavailable/unhealthy; no mutations attempted"); IOServiceClose(c); return 2; }
    printf("epoch=%llu pinned_charge=%llu peak=%llu quota_rejects=%llu client_limit=%llu device_limit=%llu quarantine=%u quarantine_bytes=%llu irq_eqes=%llu timer_eqes=%llu irq_proven=%u event_ready=%u\n",
        before.deviceEpoch, before.pinnedBytes, before.peakPinnedBytes, before.pinFailures,
        before.clientPinnedLimit, before.devicePinnedLimit, before.quarantined,
        before.quarantineBytes, before.irqCompletionEqes, before.timerCompletionEqes,
        before.completionIrqProven, before.completionEqReady);
    if (argc == 2 && !strcmp(argv[1], "--status")) { IOServiceClose(c); return 0; }
    /* Raw Exec with a deliberately invalid empty payload cannot issue a
     * command even on a regression; the policy must reject it BEFORE parsing. */
    if (call(c, kMlxUCMethodDbgExec, NULL, 0, NULL, 0) != kIOReturnNotPermitted) {
        puts("FAIL admin API not excluded"); IOServiceClose(c); return 1;
    }
    uint32_t pd = 0;
    if (call(c, kMlxUCMethodAllocPD, NULL, 0, &pd, sizeof(pd)) || !pd) return 1;
    const size_t capacity = (size_t)MLX_UC_PINNED_BYTES_PER_CLIENT + 65536;
    void *memory = mmap(NULL, capacity, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (memory == MAP_FAILED) return 1;
    struct mlx_reg_mr_req req = {.startAddr = (uintptr_t)memory, .length = capacity,
                                 .pd = pd, .accessFlags = 1};
    struct mlx_reg_mr_resp mr = {};
    kr = call(c, kMlxUCMethodRegMR, &req, sizeof(req), &mr, sizeof(mr));
    if (kr != kIOReturnNoResources || snapshot(c, &r) || r.pinnedBytes != before.pinnedBytes) {
        puts("FAIL quota rejected too late or not enforced"); return 1;
    }
    const size_t sizes[] = {1, 16385, 2 * 1024 * 1024, 64 * 1024 * 1024};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        req.startAddr = (uintptr_t)memory + (i == 1 ? 7 : 0);
        req.length = sizes[i];
        kr = call(c, kMlxUCMethodRegMR, &req, sizeof(req), &mr, sizeof(mr));
        if (kr) { printf("FAIL MR %zu: 0x%x\n", sizes[i], kr); return 1; }
        if (snapshot(c, &r) || r.clientPinnedBytes < sizes[i] ||
            r.pinnedBytes < before.pinnedBytes + sizes[i]) { puts("FAIL pin accounting"); return 1; }
        if (call(c, kMlxUCMethodDeregMR, &mr.mrHandle, sizeof(mr.mrHandle), NULL, 0)) {
            puts("FAIL deregistration; allocation intentionally retained until process exit"); return 1;
        }
        if (snapshot(c, &r) || r.clientPinnedBytes || r.pinnedBytes != before.pinnedBytes) {
            puts("FAIL pin accounting did not return to baseline"); return 1;
        }
        printf("PASS MR bytes=%zu register/deregister and pin baseline\n", sizes[i]);
    }
    munmap(memory, capacity);
    if (call(c, kMlxUCMethodDeallocPD, &pd, sizeof(pd), NULL, 0)) return 1;
    IOServiceClose(c);
    puts("RUNTIME_GATE PASS: release admin rejection, pin quota, direct/unaligned/large MR accounting");
    return 0;
}
