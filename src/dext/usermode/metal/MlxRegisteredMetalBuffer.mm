#import "MlxRegisteredMetalBuffer.h"
#include <atomic>
#include <mutex>
#include <vector>
#include <errno.h>

static std::atomic<uint64_t> nextAllocation{1}, nextSequence{1};

@implementation MlxRegisteredMetalBuffer {
    id<MTLBuffer> _buffer;
    struct ibv_context *_context;
    struct ibv_pd *_pd;
    struct ibv_mr *_mr;
    NSUInteger _slotBytes;
    uint64_t _allocation, _epoch;
    std::vector<MlxMetalSlotState> _slots;
    std::mutex _lock;
    /* A live NIC WR must keep the allocation alive even if the application
     * drops its reference before dispatching the WC. Cleared only at idle. */
    MlxRegisteredMetalBuffer *_busyHold;
}

- (instancetype)initWithBuffer:(id<MTLBuffer>)buffer context:(struct ibv_context *)context
                     slotBytes:(NSUInteger)bytes slotCount:(NSUInteger)count error:(int *)error {
    if (error) *error = EINVAL;
    if (!(self = [super init])) return nil;
    if (!context || !buffer || !buffer.device.hasUnifiedMemory ||
        buffer.storageMode != MTLStorageModeShared || !bytes || bytes > UINT32_MAX ||
        !count || count > 4096 || count > buffer.length / bytes || !buffer.contents) return nil;
    struct ibv_mlx5_runtime r = {};
    int rc = ibv_mlx5_query_runtime(context, &r, sizeof(r));
    if (rc || !r.device_epoch || r.quarantined) {
        if (error) *error = rc ? rc : ENODEV;
        return nil;
    }
    _slots.resize(count);
    _context = context; _buffer = buffer; _slotBytes = bytes;
    _epoch = r.device_epoch; _allocation = nextAllocation.fetch_add(1);
    _pd = ibv_alloc_pd(context);
    if (!_pd) { if (error) *error = errno; return nil; }
    /* Named route rather than a bare ibv_reg_mr: this is the supported way to
     * reach GPU memory here, and it should be findable by anyone who went
     * looking for ibv_reg_dmabuf_mr and was refused. */
    _mr = melon_reg_metal_mr(_pd, buffer.contents, bytes * count,
                             IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                             IBV_ACCESS_REMOTE_READ);
    if (!_mr) {
        if (error) *error = errno;
        /* An unsuccessful registration can have an ambiguous firmware result.
         * Pinning alone does not keep the allocator from reusing this object. */
        r = {};
        if (ibv_mlx5_query_runtime(context, &r, sizeof(r)) || r.quarantined)
            (void)CFBridgingRetain(buffer);
        return nil;
    }
    if (error) *error = 0;
    return self;
}
- (id<MTLBuffer>)buffer { return _buffer; }
- (struct ibv_pd *)protectionDomain { return _pd; }
- (NSUInteger)slotBytes { return _slotBytes; }
- (uint64_t)allocationGeneration { return _allocation; }
- (uint64_t)deviceEpoch { return _epoch; }
- (uint64_t)remoteAddress { return _mr ? (uint64_t)(uintptr_t)_mr->addr : 0; }
- (uint32_t)localKey { return _mr ? _mr->lkey : 0; }
- (uint32_t)remoteKey { return _mr ? _mr->rkey : 0; }

/* All private methods below require _lock. */
- (int)check:(MlxMetalLease)lease {
    if (!_mr || lease.slot >= _slots.size() ||
        !_slots[lease.slot].matches(lease, _allocation, _epoch)) return ESTALE;
    struct ibv_mlx5_runtime r = {};
    int rc = ibv_mlx5_query_runtime(_context, &r, sizeof(r));
    if (rc || r.quarantined || r.device_epoch != _epoch) {
        for (auto &s : _slots) if (s.phase != MlxMetalPhase::Free) s.phase = MlxMetalPhase::Poisoned;
        return rc ? rc : ENODEV;
    }
    return 0;
}
- (void)releaseHoldIfIdle {
    for (const auto &s : _slots) if (s.phase != MlxMetalPhase::Free) return;
    _busyHold = nil;
}
- (int)acquireTransmit:(BOOL)transmit lease:(MlxMetalLease *)lease {
    if (!lease) return EINVAL;
    std::lock_guard<std::mutex> lock(_lock);
    if (!_mr) return ENODEV;
    for (uint32_t i = 0; i < _slots.size(); ++i) {
        auto &s = _slots[i];
        if (s.phase != MlxMetalPhase::Free) continue;
        s.sequence = nextSequence.fetch_add(1);
        *lease = {_allocation, _epoch, s.sequence, i};
        int rc = [self check:*lease];
        if (rc) return rc;
        s.phase = transmit ? MlxMetalPhase::TxAcquired : MlxMetalPhase::RxAcquired;
        _busyHold = self;
        return 0;
    }
    return EAGAIN;
}
- (int)cancelLease:(MlxMetalLease)lease {
    std::lock_guard<std::mutex> lock(_lock);
    int rc = [self check:lease]; if (rc) return rc;
    auto &s = _slots[lease.slot];
    if (s.phase != MlxMetalPhase::TxAcquired && s.phase != MlxMetalPhase::RxAcquired) return EBUSY;
    s.phase = MlxMetalPhase::Free; s.sequence = 0;
    [self releaseHoldIfIdle]; return 0;
}
- (int)submitGPU:(id<MTLCommandBuffer>)commands lease:(MlxMetalLease)lease
        producer:(BOOL)producer completion:(void (^)(int))completion {
    if (!commands || commands.status != MTLCommandBufferStatusNotEnqueued ||
        commands.device != _buffer.device) return EINVAL;
    {
        std::lock_guard<std::mutex> lock(_lock);
        int rc = [self check:lease]; if (rc) return rc;
        if (!_slots[lease.slot].move(producer ? MlxMetalPhase::TxAcquired : MlxMetalPhase::RxReady,
                                    producer ? MlxMetalPhase::Producing : MlxMetalPhase::Consuming)) return EBUSY;
    }
    [commands addCompletedHandler:^(id<MTLCommandBuffer> finished) {
        int rc;
        {
            std::lock_guard<std::mutex> lock(self->_lock);
            rc = [self check:lease];
            auto &s = self->_slots[lease.slot];
            if (!rc && finished.status != MTLCommandBufferStatusCompleted) rc = EIO;
            if (rc) s.phase = MlxMetalPhase::Poisoned;
            else {
                s.phase = producer ? MlxMetalPhase::TxReady : MlxMetalPhase::Free;
                if (!producer) s.sequence = 0;
                [self releaseHoldIfIdle];
            }
        }
        if (completion) completion(rc);
    }];
    [commands commit];
    return 0;
}
- (int)submitProducer:(id<MTLCommandBuffer>)commands lease:(MlxMetalLease)lease completion:(void (^)(int))completion {
    return [self submitGPU:commands lease:lease producer:YES completion:completion];
}
- (int)submitConsumer:(id<MTLCommandBuffer>)commands lease:(MlxMetalLease)lease completion:(void (^)(int))completion {
    return [self submitGPU:commands lease:lease producer:NO completion:completion];
}
- (int)post:(MlxMetalLease)lease qp:(struct ibv_qp *)qp bytes:(uint32_t)bytes
      write:(BOOL)write remote:(uint64_t)remote rkey:(uint32_t)rkey {
    std::lock_guard<std::mutex> lock(_lock);
    int rc = [self check:lease]; if (rc) return rc;
    if (!qp || qp->pd != _pd || !bytes || bytes > _slotBytes ||
        (write && (!rkey || remote > UINT64_MAX - bytes))) return EINVAL;
    auto &s = _slots[lease.slot];
    if (!s.move(MlxMetalPhase::TxReady, MlxMetalPhase::Sending)) return EBUSY;
    s.qpn = qp->qp_num;
    struct ibv_sge sg = {(uint64_t)_buffer.contents + lease.slot * _slotBytes, bytes, _mr->lkey};
    struct ibv_send_wr wr = {}, *bad = nullptr;
    wr.wr_id = lease.sequence; wr.sg_list = &sg; wr.num_sge = 1;
    wr.opcode = write ? IBV_WR_RDMA_WRITE : IBV_WR_SEND; wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = remote; wr.wr.rdma.rkey = rkey;
    rc = ibv_post_send(qp, &wr, &bad);
    if (rc) s.phase = MlxMetalPhase::Poisoned;
    return rc;
}
- (int)postSend:(MlxMetalLease)lease qp:(struct ibv_qp *)qp bytes:(uint32_t)bytes {
    return [self post:lease qp:qp bytes:bytes write:NO remote:0 rkey:0];
}
- (int)postWrite:(MlxMetalLease)lease qp:(struct ibv_qp *)qp bytes:(uint32_t)bytes
         remote:(uint64_t)remote rkey:(uint32_t)rkey {
    return [self post:lease qp:qp bytes:bytes write:YES remote:remote rkey:rkey];
}
- (int)postReceive:(MlxMetalLease)lease qp:(struct ibv_qp *)qp {
    std::lock_guard<std::mutex> lock(_lock);
    int rc = [self check:lease]; if (rc) return rc;
    if (!qp || qp->pd != _pd) return EINVAL;
    auto &s = _slots[lease.slot];
    if (!s.move(MlxMetalPhase::RxAcquired, MlxMetalPhase::Receiving)) return EBUSY;
    s.qpn = qp->qp_num;
    struct ibv_sge sg = {(uint64_t)_buffer.contents + lease.slot * _slotBytes, (uint32_t)_slotBytes, _mr->lkey};
    struct ibv_recv_wr wr = {}, *bad = nullptr;
    wr.wr_id = lease.sequence; wr.sg_list = &sg; wr.num_sge = 1;
    rc = ibv_post_recv(qp, &wr, &bad);
    if (rc) s.phase = MlxMetalPhase::Poisoned;
    return rc;
}
- (int)completeNetwork:(const struct ibv_wc *)wc lease:(MlxMetalLease)lease {
    if (!wc) return EINVAL;
    std::lock_guard<std::mutex> lock(_lock);
    int rc = [self check:lease]; if (rc) return rc;
    auto &s = _slots[lease.slot];
    if (wc->wr_id != lease.sequence || wc->qp_num != s.qpn) return ESTALE;
    bool receiving = s.phase == MlxMetalPhase::Receiving;
    if (!receiving && s.phase != MlxMetalPhase::Sending) return EBUSY;
    if (wc->status != IBV_WC_SUCCESS) { s.phase = MlxMetalPhase::Poisoned; return EIO; }
    if (receiving ? (wc->opcode != IBV_WC_RECV || wc->byte_len > _slotBytes) :
        (wc->opcode != IBV_WC_SEND && wc->opcode != IBV_WC_RDMA_WRITE)) return EINVAL;
    s.phase = receiving ? MlxMetalPhase::RxReady : MlxMetalPhase::Free;
    if (!receiving) s.sequence = 0;
    [self releaseHoldIfIdle]; return 0;
}
- (int)close {
    std::lock_guard<std::mutex> lock(_lock);
    for (const auto &s : _slots) if (s.phase != MlxMetalPhase::Free) return EBUSY;
    int rc = _mr ? ibv_dereg_mr(_mr) : 0;
    if (rc) return rc;
    _mr = nullptr;
    rc = _pd ? ibv_dealloc_pd(_pd) : 0;
    if (rc) return rc;
    _pd = nullptr; _context = nullptr; _buffer = nil;
    return 0;
}
- (void)dealloc {
    /* Busy work owns self. This is the idle/initialization-error path only. */
    if (_mr && ibv_dereg_mr(_mr)) {
        (void)CFBridgingRetain(_buffer); /* uncertain DMA: do not recycle VA */
        return;
    }
    if (_pd) (void)ibv_dealloc_pd(_pd);
}
@end
