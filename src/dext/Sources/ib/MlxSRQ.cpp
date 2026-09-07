/*
 * MlxSRQ.cpp — shared receive queue, backed by an RMP.
 *
 * The rmpc layout used here was proven on this hardware before the file was
 * written; see MlxSRQ.hpp. Offsets come from linux/mlx5/mlx5_ifc.h, not from
 * recollection: wq_bits ends with reserved_at_200[0x400], so the struct spans
 * 0x600 bits and its PAS array starts there, landing at absolute 0x880.
 */
#include "MlxSRQ.hpp"
#include "MlxRoCE.hpp"
#include "MlxRegs.hpp"
#include "MlxIfcHelpers.hpp"
#include "MlxP0Encoding.hpp"
#include "MlxSafety.hpp"
#include "MlxWQE.hpp"
#include "MlxUCIO.h"
#include "MlxLog.hpp"
#include "MlxPCIDriver.h"
#include "MlxUAR.hpp"
#include "MlxDriverKitCompat.h"
#include "MlxCmd.hpp"
#include "MlxDMA.hpp"

#define MLX_LOG(fmt, ...)  IOLog("MlxSRQ: " fmt "\n", ##__VA_ARGS__)

/* create_rmp_in: ctx @0x100. rmpc: state @+0x08, wq @+0x180. */
#define SRQ_RMPC_OFF     0x100
#define SRQ_WQ_OFF       (SRQ_RMPC_OFF + 0x180)
#define SRQ_WQ_SPAN      0x600
#define SRQ_PAS_OFF      (SRQ_WQ_OFF + SRQ_WQ_SPAN)
/* The linked-list terminator is outside the valid WQE index range. */
#define SRQ_RMPC_RDY     1
#define SRQ_INVALID_LKEY 0x100u

struct MlxSRQ::State {
    MlxRoCE      *roce;
    MlxPCIDriver *core;
    IOLock       *tableLock;
    MlxSRQContext table[MLX_SRQ_TABLE_MAX];
    uint32_t      recvSeen;
    uint32_t      recvWithSrqn;
    uint32_t      lastCqeSrqn;
    uint32_t      lastCqeWqe;
    uint32_t      lastCqeOp;
    uint32_t      wqeSeenMask;
};

MlxSRQ::MlxSRQ() : s(NULL) {}
MlxSRQ::~MlxSRQ() { Free(); }

kern_return_t
MlxSRQ::Init(MlxRoCE *roce)
{
    if (!roce) return kIOReturnBadArgument;
    s = new State;
    if (!s) return kIOReturnNoMemory;
    memset(s, 0, sizeof(*s));
    s->roce = roce;
    s->core = roce->GetCore();
    s->tableLock = IOLockAlloc();
    if (!s->tableLock) { delete s; s = NULL; return kIOReturnNoMemory; }
    return kIOReturnSuccess;
}

void
MlxSRQ::Free()
{
    if (!s) return;
    for (uint32_t i = 0; i < MLX_SRQ_TABLE_MAX; i++)
        if (s->table[i].used) {
            (void)CmdDestroyRmp(s->table[i].srqn);
            ReleaseContext(&s->table[i]);
        }
    if (s->tableLock) IOLockFree(s->tableLock);
    delete s;
    s = NULL;
}

MlxSRQContext *
MlxSRQ::Lookup(uint32_t srqn)
{
    if (!s) return NULL;
    for (uint32_t i = 0; i < MLX_SRQ_TABLE_MAX; i++)
        if (s->table[i].used && s->table[i].srqn == srqn) return &s->table[i];
    return NULL;
}

uint32_t
MlxSRQ::LiveCount() const
{
    if (!s) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < MLX_SRQ_TABLE_MAX; i++) if (s->table[i].used) n++;
    return n;
}

/* Each WQE opens with a next segment. The free list and the RMP's posted
 * list are separate chains, although they share this next-index field. */
void
MlxSRQ::BuildFreeList(MlxSRQContext *srq)
{
    const uint32_t count = 1u << srq->logSize;
    uint8_t *base = (uint8_t *)(uintptr_t)srq->bufAddr;
    memset(base, 0, (size_t)count * MLX_SRQ_WQE_BYTES);
    /* Circular: the last WQE links back to the first. mlx5 builds it the same
     * way, and it matters — firmware follows these links, so the chain must
     * never run off its end. */
    for (uint32_t i = 0; i < count; i++) {
        uint8_t *wqe = base + (size_t)i * MLX_SRQ_WQE_BYTES;
        uint16_t next = (uint16_t)((i + 1 < count) ? (i + 1) : 0);
        *(volatile uint16_t *)(wqe + 2) = OSSwapHostToBigInt16(next);
    }
    srq->head = 0;
    srq->tail = count - 1;
    srq->freeCount = count - 1;   /* the tail is the spare */
    if (srq->wrId) memset(srq->wrId, 0, (size_t)count * sizeof(*srq->wrId));
    srq->counter = 0;
    if (srq->dbRecord) *srq->dbRecord = 0;
}

kern_return_t
MlxSRQ::CmdCreateRmp(MlxSRQContext *srq)
{
    uint8_t in[512] = {};
    uint8_t out[64] = {};
    mlxSetBits(in, 0x00, 16, MLX_CMD_OP_CREATE_RMP);
    mlxSetBits(in, SRQ_RMPC_OFF + 0x08, 4, SRQ_RMPC_RDY);
    /* wq_type LINKED_LIST, and uar_page deliberately left zero: firmware
     * rejects an RMP that carries one, exactly as mlx5's set_wq implies. */
    mlxSetBits(in, SRQ_WQ_OFF + 0x000, 4,  0);
    mlxSetBits(in, SRQ_WQ_OFF + 0x048, 24, srq->pd);
    mlxSetBits(in, SRQ_WQ_OFF + 0x080, 64, 0);   /* filled below */
    /* log_wq_stride is the plain log2 of the stride here, not the QPC's -4. */
    mlxSetBits(in, SRQ_WQ_OFF + 0x10c, 4,  6);   /* 64-byte WQE */
    mlxSetBits(in, SRQ_WQ_OFF + 0x113, 5,  0);   /* 4 KiB pages */
    mlxSetBits(in, SRQ_WQ_OFF + 0x11b, 5,  srq->logSize);

    uint64_t dbDma = 0;
    uint32_t dbOffset = 0;
    if (!s->core->GetUAR()) return kIOReturnNotReady;
    kern_return_t dkr = s->core->GetUAR()->AllocDbSlot(&dbDma, &dbOffset);
    if (dkr != kIOReturnSuccess) return kIOReturnNoSpace;
    srq->dbRecordOffset = dbOffset;
    srq->dbRecord = s->core->GetUAR()->GetDbRecord(dbOffset);
    mlxSetBits(in, SRQ_WQ_OFF + 0x080, 64, dbDma);

    for (uint32_t i = 0; i < srq->numPages; i++)
        mlxSetBits(in, SRQ_PAS_OFF + i * 64, 64, srq->pageDMA[i]);

    const uint32_t inSize = (SRQ_RMPC_OFF + 0x180 + SRQ_WQ_SPAN) / 8 +
                            srq->numPages * 8;
    kern_return_t kr = s->core->Exec(MLX_CMD_OP_CREATE_RMP, in, inSize,
                                     out, sizeof(out), 5000);
    if (kr != kIOReturnSuccess) {
        s->core->GetUAR()->FreeDbSlot(dbOffset);
        srq->dbRecord = NULL;
        MLX_LOG("CREATE_RMP failed 0x%x", kr);
        return kr;
    }
    srq->srqn = (uint32_t)mlxGetBits(out, 0x48, 24);
    return kIOReturnSuccess;
}

kern_return_t
MlxSRQ::CmdDestroyRmp(uint32_t srqn)
{
    uint8_t in[16] = {};
    uint8_t out[16] = {};
    mlxSetBits(in, 0x00, 16, MLX_CMD_OP_DESTROY_RMP);
    mlxSetBits(in, 0x48, 24, srqn);
    return s->core->Exec(MLX_CMD_OP_DESTROY_RMP, in, sizeof(in),
                         out, sizeof(out), 5000);
}

void
MlxSRQ::ReleaseContext(MlxSRQContext *srq)
{
    if (srq->wrId) IOSafeDeleteNULL(srq->wrId, uint64_t, 1u << srq->logSize);
    if (srq->dbRecord && s->core->GetUAR())
        s->core->GetUAR()->FreeDbSlot(srq->dbRecordOffset);
    if (srq->dmaMap) mlxCompleteDma(srq->dmaMap);
    if (srq->bufDesc) srq->bufDesc->release();
    memset(srq, 0, sizeof(*srq));
}

kern_return_t
MlxSRQ::CreateSRQ(const struct mlx_create_srq_req *req,
                  struct mlx_create_srq_resp *resp)
{
    if (!s || !req || !resp) return kIOReturnBadArgument;
    if (!req->maxWr || req->maxWr > 4096) return kIOReturnBadArgument;
    if (!req->maxSge || req->maxSge > MLX_SRQ_MAX_SGE) return kIOReturnBadArgument;

    uint32_t logSize = 0;
    while ((1u << logSize) < req->maxWr) logSize++;
    if (logSize < 4) logSize = 4;

    IOLockLock(s->tableLock);
    MlxSRQContext *srq = NULL;
    for (uint32_t i = 0; i < MLX_SRQ_TABLE_MAX; i++)
        if (!s->table[i].used) { srq = &s->table[i]; srq->used = true; break; }
    IOLockUnlock(s->tableLock);
    if (!srq) return kIOReturnNoResources;

    memset(srq, 0, sizeof(*srq));
    srq->used = true;
    srq->pd = req->pd;
    srq->logSize = logSize;
    srq->maxSge = req->maxSge;

    const uint64_t bytes = (uint64_t)(1u << logSize) * MLX_SRQ_WQE_BYTES;
    IOAddressSegment segs[MLX_SRQ_MAX_PAGES];
    uint32_t segCount = MLX_SRQ_MAX_PAGES;
    uint64_t mapped = 0, mappedLen = 0;
    kern_return_t kr;

    kr = mlxAllocDmaBuffer(bytes, 4096, kIOMemoryDirectionOutIn, &srq->bufDesc);
    if (kr != kIOReturnSuccess || !srq->bufDesc) { kr = kIOReturnNoMemory; goto fail; }
    kr = mlxPrepareDma(s->core->GetPCI(), srq->bufDesc, segs, &segCount, &srq->dmaMap);
    if (kr != kIOReturnSuccess || segCount == 0) { kr = kIOReturnNoSpace; goto fail; }
    for (uint32_t i = 0; i < segCount && srq->numPages < MLX_SRQ_MAX_PAGES; i++)
        if (!mlxAppendMttPages(segs[i].address, segs[i].length,
                               srq->pageDMA, MLX_SRQ_MAX_PAGES, &srq->numPages)) {
            kr = kIOReturnNoSpace; goto fail;
        }
    if (!srq->numPages) { kr = kIOReturnNoSpace; goto fail; }
    if (srq->bufDesc->Map(0, 0, 0, 0, &mapped, &mappedLen) != kIOReturnSuccess) {
        kr = kIOReturnNoMemory; goto fail;
    }
    srq->bufAddr = mapped;
    srq->wrId = IONewZero(uint64_t, 1u << logSize);
    if (!srq->wrId) { kr = kIOReturnNoMemory; goto fail; }

    kr = CmdCreateRmp(srq);
    if (kr != kIOReturnSuccess) goto fail;

    /* The free list is built after creation so the DB record exists; firmware
     * reads WQEs only once one is published through the doorbell. */
    BuildFreeList(srq);
    if (req->limit) (void)ModifyLimit(srq->srqn, req->limit);

    resp->srqn = srq->srqn;
    resp->logSize = srq->logSize;
    resp->maxWr = 1u << srq->logSize;
    resp->maxSge = srq->maxSge;
    MLX_LOG("SRQ[%u] created wqes=%u sge=%u pages=%u",
            srq->srqn, 1u << logSize, srq->maxSge, srq->numPages);
    return kIOReturnSuccess;

fail:
    ReleaseContext(srq);
    return kr ? kr : kIOReturnNoMemory;
}

kern_return_t
MlxSRQ::DestroySRQ(uint32_t srqn)
{
    if (!s) return kIOReturnBadArgument;
    MlxSRQContext *srq = Lookup(srqn);
    if (!srq) return kIOReturnNotFound;
    kern_return_t kr = CmdDestroyRmp(srqn);
    if (kr != kIOReturnSuccess) return kr;
    IOLockLock(s->tableLock);
    ReleaseContext(srq);
    IOLockUnlock(s->tableLock);
    return kIOReturnSuccess;
}

kern_return_t
MlxSRQ::PostRecv(const struct mlx_post_srq_recv_req *req)
{
    if (!s || !req) return kIOReturnBadArgument;
    if (!req->numSge || req->numSge > MLX_SRQ_MAX_SGE) return kIOReturnBadArgument;

    IOLockLock(s->tableLock);
    MlxSRQContext *srq = Lookup(req->srqn);
    if (!srq) { IOLockUnlock(s->tableLock); return kIOReturnNotFound; }
    /* Full when the head meets the tail: the tail WQE is the spare that keeps
     * the chain linkable, so it is never handed out. */
    if (srq->head == srq->tail) { IOLockUnlock(s->tableLock); return kIOReturnNoSpace; }

    const uint32_t index = srq->head;
    uint8_t *wqe = (uint8_t *)(uintptr_t)srq->bufAddr +
                   (size_t)index * MLX_SRQ_WQE_BYTES;
    /* The link stays as it is: it is the chain firmware walks. Only the head
     * moves on. */
    srq->head = OSSwapBigToHostInt16(*(volatile uint16_t *)(wqe + 2));
    if (srq->freeCount) srq->freeCount--;

    /* Data segments follow the 16-byte next segment. A short list is closed
     * with an invalid lkey, which is how firmware learns where it ends. */
    uint8_t *scat = wqe + 16;
    for (uint32_t i = 0; i < req->numSge; i++) {
        *(volatile uint32_t *)(scat + i * 16 + 0) =
            OSSwapHostToBigInt32(req->sge[i].length);
        *(volatile uint32_t *)(scat + i * 16 + 4) =
            OSSwapHostToBigInt32(req->sge[i].lkey);
        *(volatile uint64_t *)(scat + i * 16 + 8) =
            OSSwapHostToBigInt64(req->sge[i].addr);
    }
    if (req->numSge < MLX_SRQ_MAX_SGE) {
        uint8_t *end = scat + req->numSge * 16;
        *(volatile uint32_t *)(end + 0) = 0;
        *(volatile uint32_t *)(end + 4) = OSSwapHostToBigInt32(SRQ_INVALID_LKEY);
        *(volatile uint64_t *)(end + 8) = 0;
    }

    if (srq->wrId) srq->wrId[index] = req->wrId;
    __sync_synchronize();
    srq->counter++;
    if (srq->dbRecord) *srq->dbRecord = OSSwapHostToBigInt32(srq->counter);
    IOLockUnlock(s->tableLock);
    return kIOReturnSuccess;
}

/* A receive completion carries the WQE index it consumed. Without handing it
 * back the free list drains and posting stops after one queue's worth. */
void
MlxSRQ::ReturnWqe(uint32_t srqn, uint32_t wqeIndex)
{
    if (!s) return;
    IOLockLock(s->tableLock);
    MlxSRQContext *srq = Lookup(srqn);
    if (srq && wqeIndex < (1u << srq->logSize)) {
        /* Append at the tail, never at the head: firmware walks the chain
         * forward, and putting a WQE back in front of it makes it cycle over
         * the few entries ahead while the rest are never reached. This is
         * exactly what mlx5_free_srq_wqe does. */
        uint8_t *tailWqe = (uint8_t *)(uintptr_t)srq->bufAddr +
                           (size_t)srq->tail * MLX_SRQ_WQE_BYTES;
        *(volatile uint16_t *)(tailWqe + 2) =
            OSSwapHostToBigInt16((uint16_t)wqeIndex);
        srq->tail = wqeIndex;
        srq->freeCount++;
    }
    IOLockUnlock(s->tableLock);
}

void
MlxSRQ::NoteReceiveCqe(uint32_t srqn, uint32_t wqeIndex, uint8_t op)
{
    if (!s) return;
    IOLockLock(s->tableLock);
    s->recvSeen++;
    if (srqn) s->recvWithSrqn++;
    s->lastCqeSrqn = srqn;
    s->lastCqeWqe = wqeIndex;
    s->lastCqeOp = op;
    if (wqeIndex < 32) s->wqeSeenMask |= (1u << wqeIndex);
    IOLockUnlock(s->tableLock);
}

uint64_t
MlxSRQ::WrIdFor(uint32_t srqn, uint32_t wqeIndex)
{
    if (!s) return 0;
    IOLockLock(s->tableLock);
    uint64_t id = 0;
    MlxSRQContext *srq = Lookup(srqn);
    if (srq && srq->wrId && wqeIndex < (1u << srq->logSize))
        id = srq->wrId[wqeIndex];
    IOLockUnlock(s->tableLock);
    return id;
}

kern_return_t
MlxSRQ::ModifyLimit(uint32_t srqn, uint32_t limit)
{
    if (!s) return kIOReturnBadArgument;
    MlxSRQContext *srq = Lookup(srqn);
    if (!srq) return kIOReturnNotFound;
    uint8_t in[512] = {};
    uint8_t out[64] = {};
    mlxSetBits(in, 0x00, 16, MLX_CMD_OP_MODIFY_RMP);
    mlxSetBits(in, 0x48, 24, srqn);
    /* modify_rmp_in carries a field-select bitmask before the context; bit 0
     * selects the watermark, matching mlx5's MLX5_RMP_MODIFY_BITMASK_LWM. */
    mlxSetBits(in, 0x60, 32, 1);
    mlxSetBits(in, SRQ_RMPC_OFF + 0x08, 4, SRQ_RMPC_RDY);
    mlxSetBits(in, SRQ_WQ_OFF + 0x030, 16, limit);
    const uint32_t inSize = (SRQ_RMPC_OFF + 0x180 + SRQ_WQ_SPAN) / 8;
    kern_return_t kr = s->core->Exec(MLX_CMD_OP_MODIFY_RMP, in, inSize,
                                     out, sizeof(out), 5000);
    if (kr == kIOReturnSuccess) srq->limit = limit;
    return kr;
}

kern_return_t
MlxSRQ::QuerySRQ(uint32_t srqn, struct mlx_query_srq_resp *resp)
{
    if (!s || !resp) return kIOReturnBadArgument;
    IOLockLock(s->tableLock);
    MlxSRQContext *srq = Lookup(srqn);
    if (!srq) { IOLockUnlock(s->tableLock); return kIOReturnNotFound; }
    resp->srqn = srq->srqn;
    resp->maxWr = 1u << srq->logSize;
    resp->maxSge = srq->maxSge;
    resp->limit = srq->limit;
    resp->freeCount = srq->freeCount;
    resp->posted = srq->counter;
    resp->recvSeen = s->recvSeen;
    resp->recvWithSrqn = s->recvWithSrqn;
    resp->lastCqeSrqn = s->lastCqeSrqn;
    resp->lastCqeWqe = s->lastCqeWqe;
    resp->lastCqeOp = s->lastCqeOp;
    resp->wqeSeenMask = s->wqeSeenMask;
    IOLockUnlock(s->tableLock);
    return kIOReturnSuccess;
}
