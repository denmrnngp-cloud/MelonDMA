/* Real Metal GPU, mocked verbs transport. This does NOT claim a NIC test. */
#import "../usermode/metal/MlxRegisteredMetalBuffer.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <atomic>

static uint64_t epoch = 7;
static int registered, deallocBusy, deregFailure;
extern "C" int ibv_mlx5_query_runtime(struct ibv_context *, void *out, size_t size) {
    assert(size == sizeof(ibv_mlx5_runtime));
    auto r = (ibv_mlx5_runtime *)out; *r = {}; r->device_epoch = epoch; return 0;
}
extern "C" ibv_pd *ibv_alloc_pd(ibv_context *ctx) {
    auto pd = new ibv_pd{}; pd->context = ctx; return pd;
}
extern "C" int ibv_dealloc_pd(ibv_pd *pd) {
    if (deallocBusy) return EBUSY;
    delete pd; return 0;
}
extern "C" ibv_mr *ibv_reg_mr(ibv_pd *pd, void *addr, size_t len, int) {
    auto mr = new ibv_mr{}; mr->pd = pd; mr->addr = addr; mr->length = len;
    mr->lkey = 123; ++registered; return mr;
}
extern "C" int ibv_dereg_mr(ibv_mr *mr) {
    if (deregFailure) return EIO;
    --registered; delete mr; return 0;
}
extern "C" int ibv_post_send(ibv_qp *, ibv_send_wr *wr, ibv_send_wr **) {
    assert(wr->send_flags & IBV_SEND_SIGNALED);
    auto bytes = (uint8_t *)(uintptr_t)wr->sg_list->addr;
    for (uint32_t i = 0; i < wr->sg_list->length; ++i) assert(bytes[i] == 0x5a);
    return 0;
}
extern "C" int ibv_post_recv(ibv_qp *, ibv_recv_wr *wr, ibv_recv_wr **) {
    memset((void *)(uintptr_t)wr->sg_list->addr, 0xa5, wr->sg_list->length); return 0;
}

int main() { @autoreleasepool {
    id<MTLDevice> gpu = MTLCreateSystemDefaultDevice();
    if (!gpu || !gpu.hasUnifiedMemory) { puts("SKIP: unified-memory Metal GPU unavailable"); return 77; }
    auto ctx = (ibv_context *)(uintptr_t)1;
    int error = 0;
    id<MTLBuffer> privateBuffer = [gpu newBufferWithLength:4096 options:MTLResourceStorageModePrivate];
    assert(![[MlxRegisteredMetalBuffer alloc] initWithBuffer:privateBuffer context:ctx
                                                slotBytes:4096 slotCount:1 error:&error]);
    id<MTLBuffer> buffer = [gpu newBufferWithLength:8192 options:MTLResourceStorageModeShared];
    auto pool = [[MlxRegisteredMetalBuffer alloc] initWithBuffer:buffer context:ctx
                                                    slotBytes:4096 slotCount:2 error:&error];
    assert(pool && !error && registered == 1);
    ibv_qp qp = {}; qp.pd = pool.protectionDomain; qp.qp_num = 99;
    MlxMetalLease tx, rx, extra;
    assert(![pool acquireTransmit:YES lease:&tx]);
    assert(![pool acquireTransmit:NO lease:&rx]);
    assert([pool acquireTransmit:YES lease:&extra] == EAGAIN);
    assert([pool close] == EBUSY);
    assert([pool postSend:tx qp:&qp bytes:4096] == EBUSY);
    auto queue = [gpu newCommandQueue];
    auto commands = [queue commandBuffer];
    auto fill = [commands blitCommandEncoder];
    [fill fillBuffer:buffer range:NSMakeRange(tx.slot * 4096, 4096) value:0x5a];
    [fill endEncoding];
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    assert(![pool submitProducer:commands lease:tx completion:^(int rc) {
        assert(!rc); dispatch_semaphore_signal(done);
    }]);
    assert(!dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
    assert(![pool postSend:tx qp:&qp bytes:4096]);
    assert([pool cancelLease:tx] == EBUSY);
    ibv_wc wc = {}; wc.wr_id = tx.sequence; wc.qp_num = 98; wc.opcode = IBV_WC_SEND;
    assert([pool completeNetwork:&wc lease:tx] == ESTALE);
    wc.qp_num = 99;
    assert(![pool completeNetwork:&wc lease:tx]);
    assert([pool completeNetwork:&wc lease:tx] == ESTALE);
    assert(![pool postReceive:rx qp:&qp]);
    wc.wr_id = rx.sequence; wc.opcode = IBV_WC_RDMA_WRITE;
    assert([pool completeNetwork:&wc lease:rx] == EINVAL);
    wc.opcode = IBV_WC_RECV; wc.byte_len = 4096;
    assert(![pool completeNetwork:&wc lease:rx]);
    id<MTLBuffer> result = [gpu newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    commands = [queue commandBuffer];
    auto copy = [commands blitCommandEncoder];
    [copy copyFromBuffer:buffer sourceOffset:rx.slot * 4096 toBuffer:result destinationOffset:0 size:4096];
    [copy endEncoding];
    assert(![pool submitConsumer:commands lease:rx completion:^(int rc) {
        assert(!rc); dispatch_semaphore_signal(done);
    }]);
    assert(!dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
    for (unsigned i = 0; i < 4096; ++i) assert(((uint8_t *)result.contents)[i] == 0xa5);
    assert(![pool acquireTransmit:YES lease:&extra]);
    auto stale = extra; ++stale.allocation;
    assert([pool cancelLease:stale] == ESTALE);
    assert(![pool cancelLease:extra]);
    deregFailure = 1; assert([pool close] == EIO && registered == 1);
    deregFailure = 0; deallocBusy = 1; assert([pool close] == EBUSY && registered == 0);
    deallocBusy = 0; assert(![pool close]);
    puts("PASS: real Metal producer/consumer; MOCK verbs lifecycle, stale WC, close failure, bounds, private rejection");
} }
