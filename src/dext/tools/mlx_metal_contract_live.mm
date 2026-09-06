/* Real Metal + real NIC, local RC loopback. No mock completion accepted. */
#import "../usermode/metal/MlxRegisteredMetalBuffer.h"
#include <arpa/inet.h>
#include <chrono>
#include <stdio.h>
#include <unistd.h>

#define REQUIRE(expr) do { int e = (expr); if (e) { fprintf(stderr, "FAIL line %d: %s rc=%d\n", __LINE__, #expr, e); return 1; } } while (0)
int main() { @autoreleasepool {
    int count = 0, error = 0;
    auto devices = ibv_get_device_list(&count);
    auto ctx = count ? ibv_open_device(devices[0]) : nullptr;
    if (!ctx) return 2;
    ibv_mlx5_roce_config config = {};
    config.local_gid.raw[10] = config.local_gid.raw[11] = 0xff;
    inet_pton(AF_INET, "192.168.200.1", config.local_gid.raw + 12);
    const uint8_t mac[] = {0x98, 0x03, 0x9b, 0x80, 0x6a, 0x94};
    memcpy(config.local_mac, mac, 6); memcpy(config.peer_mac, mac, 6);
    config.hop_limit = 1;
    REQUIRE(ibv_mlx5_configure_roce(ctx, &config));
    id<MTLDevice> gpu = MTLCreateSystemDefaultDevice();
    REQUIRE(gpu ? 0 : ENODEV);
    id<MTLBuffer> privateMemory = [gpu newBufferWithLength:4096
        options:MTLResourceStorageModePrivate];
    int negativeError = 0;
    auto privatePool = [[MlxRegisteredMetalBuffer alloc]
        initWithBuffer:privateMemory context:ctx slotBytes:4096 slotCount:1
        error:&negativeError];
    REQUIRE(privatePool == nil ? 0 : EINVAL);
    REQUIRE(negativeError == EINVAL ? 0 : EINVAL);
    auto nilPool = [[MlxRegisteredMetalBuffer alloc]
        initWithBuffer:nil context:ctx slotBytes:4096 slotCount:1 error:&negativeError];
    REQUIRE(nilPool == nil ? 0 : EINVAL);
    id<MTLBuffer> memory = [gpu newBufferWithLength:8192 options:MTLResourceStorageModeShared];
    auto pool = [[MlxRegisteredMetalBuffer alloc] initWithBuffer:memory context:ctx
                                                    slotBytes:4096 slotCount:2 error:&error];
    if (!pool) { fprintf(stderr, "pool failed %d\n", error); return 1; }
    auto cq = ibv_create_cq(ctx, 16, nullptr, nullptr, 0);
    ibv_qp_init_attr init = {};
    init.send_cq = init.recv_cq = cq; init.qp_type = IBV_QPT_RC;
    init.cap.max_send_wr = init.cap.max_recv_wr = 4;
    init.cap.max_send_sge = init.cap.max_recv_sge = 1;
    auto qp = ibv_create_qp(pool.protectionDomain, &init);
    if (!cq || !qp) return 1;
    ibv_qp_attr attr = {};
    attr.qp_state = IBV_QPS_INIT; attr.port_num = 1;
    REQUIRE(ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS));
    attr = {}; attr.qp_state = IBV_QPS_RTR; attr.path_mtu = IBV_MTU_4096;
    attr.dest_qp_num = qp->qp_num; attr.rq_psn = 17; attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12; attr.ah_attr.is_global = 1; attr.ah_attr.port_num = 1;
    attr.ah_attr.grh.dgid = config.local_gid; attr.ah_attr.grh.hop_limit = 1;
    REQUIRE(ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
        IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER));
    attr = {}; attr.qp_state = IBV_QPS_RTS; attr.sq_psn = 17; attr.timeout = 14;
    attr.retry_cnt = attr.rnr_retry = 7; attr.max_rd_atomic = 1;
    REQUIRE(ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT |
        IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC));
    REQUIRE(ibv_close_device(ctx) == EBUSY ? 0 : EINVAL);
    auto queue = [gpu newCommandQueue];
    id<MTLBuffer> result = [gpu newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block int gpuError = 0;
    for (unsigned iter = 0; iter < 64; ++iter) {
        MlxMetalLease tx, rx;
        REQUIRE([pool acquireTransmit:YES lease:&tx]);
        REQUIRE([pool acquireTransmit:NO lease:&rx]);
        REQUIRE([pool close] == EBUSY ? 0 : EINVAL);
        REQUIRE([pool postReceive:rx qp:qp]);
        auto producer = [queue commandBuffer]; auto fill = [producer blitCommandEncoder];
        [fill fillBuffer:memory range:NSMakeRange(tx.slot * 4096, 4096) value:(uint8_t)iter];
        [fill endEncoding];
        REQUIRE([pool submitProducer:producer lease:tx completion:^(int e) { gpuError = e; dispatch_semaphore_signal(done); }]);
        REQUIRE((int)dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
        REQUIRE(gpuError);
        REQUIRE([pool postSend:tx qp:qp bytes:4096]);
        int completions = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (completions < 2 && std::chrono::steady_clock::now() < deadline) {
            ibv_wc wc = {}; int n = ibv_poll_cq(cq, 1, &wc);
            if (n < 0) return 1;
            if (!n) { usleep(20); continue; }
            REQUIRE([pool completeNetwork:&wc lease:wc.wr_id == tx.sequence ? tx : rx]);
            ++completions;
        }
        REQUIRE(completions == 2 ? 0 : ETIMEDOUT);
        auto consumer = [queue commandBuffer]; auto copy = [consumer blitCommandEncoder];
        [copy copyFromBuffer:memory sourceOffset:rx.slot * 4096 toBuffer:result destinationOffset:0 size:4096];
        [copy endEncoding];
        REQUIRE([pool submitConsumer:consumer lease:rx completion:^(int e) { gpuError = e; dispatch_semaphore_signal(done); }]);
        REQUIRE((int)dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
        REQUIRE(gpuError);
        for (unsigned j = 0; j < 4096; ++j) REQUIRE(((uint8_t *)result.contents)[j] == iter ? 0 : EIO);
    }
    REQUIRE(ibv_destroy_qp(qp)); REQUIRE(ibv_destroy_cq(cq));
    REQUIRE([pool close]); REQUIRE(ibv_close_device(ctx)); ibv_free_device_list(devices);
    puts("METAL_CONTRACT_LIVE PASS: 64 x 4096 GPU -> NIC RC loopback -> GPU, lease reuse, close EBUSY, MTU4096");
    return 0;
} }
