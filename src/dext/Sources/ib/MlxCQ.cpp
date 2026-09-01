/*
 * MlxCQ.cpp — Completion Queue (DriverKit port).
 *
 * Ported from: drivers/infiniband/hw/mlx5/cq.c, trimmed to create/destroy +
 * kernel-mediated poll_cq. The CQE buffer is a DEXT-owned
 * IOBufferMemoryDescriptor pinned with IODMACommand; firmware writes CQEs
 * via DMA and the DEXT owns the consumer-index DB record.
 */
#include "MlxCQ.hpp"
#include "MlxRoCE.hpp"
#include "MlxQP.hpp"
#include "MlxPCIDriver.h"
#include "MlxCmd.hpp"
#include "MlxDMA.hpp"
#include "MlxDriverKitCompat.h"
#include "MlxUAR.hpp"
#include "MlxRegs.hpp"
#include "MlxWQE.hpp"
#include "MlxP0Encoding.hpp"
#include "MlxKeyIndex.hpp"

#include <DriverKit/IOLib.h>
#include <string.h>
#include "MlxIfcHelpers.hpp"   /* mlxSetBits / mlxGetBits */

#include "MlxLog.hpp"
#define MLX_LOG(fmt, ...)  IOLog("MlxCQ: " fmt "\n", ##__VA_ARGS__)
#define MLX_DBG(fmt, ...)  MLX_DBGLOG("MlxCQ: " fmt, ##__VA_ARGS__)
#define MLX_CQ_TABLE_MAX   4096

struct MlxCQ::State {
    MlxRoCE      *roce;
    MlxPCIDriver *core;
    struct IOLock *tableLock;          /* used[] + cqn index + slot alloc */
    struct IOLock **cqLocks;           /* per-CQ, pre-allocated */
    MlxCQContext  *table;
    bool          *used;
    uint32_t       tableCap;
    int32_t       *cqnSlots;           /* cqn → slot+1 */
    uint32_t      *cqnKeys;
    uint32_t       hashMask;
    uint32_t       hashShift;
};

MlxCQ::MlxCQ() : s(NULL) {}
MlxCQ::~MlxCQ() { Free(); }

kern_return_t
MlxCQ::Init(MlxRoCE *roce)
{
    if (!roce) return kIOReturnBadArgument;
    s = new State;
    if (!s) return kIOReturnNoMemory;
    memset(s, 0, sizeof(*s));
    s->roce = roce;
    s->core = roce->GetCore();
    s->roce = roce;
    s->core = roce->GetCore();
    uint32_t fwMax = (s->core->GetHCA() && s->core->GetHCA()->Caps().maxCq) ?
                     s->core->GetHCA()->Caps().maxCq : MLX_CQ_TABLE_MAX;
    s->tableCap = fwMax < MLX_CQ_TABLE_MAX ? fwMax : MLX_CQ_TABLE_MAX;
    if (s->tableCap < 64) s->tableCap = 64;
    uint32_t hashCap = mlxKeyIndexCap(s->tableCap);
    s->hashMask = hashCap - 1u;
    s->hashShift = mlxKeyIndexShift(hashCap);

    s->table = IONewZero(MlxCQContext, s->tableCap);
    s->used = IONewZero(bool, s->tableCap);
    s->cqLocks = IONewZero(IOLock *, s->tableCap);
    s->cqnKeys = IONewZero(uint32_t, hashCap);
    s->cqnSlots = IONewZero(int32_t, hashCap);
    s->tableLock = IOLockAlloc();
    bool ok = s->table && s->used && s->cqLocks && s->cqnKeys && s->cqnSlots &&
              s->tableLock;
    for (uint32_t i = 0; ok && i < s->tableCap; i++) {
        s->cqLocks[i] = IOLockAlloc();
        if (!s->cqLocks[i]) ok = false;
    }
    if (!ok) {
        for (uint32_t i = 0; s->cqLocks && i < s->tableCap; i++)
            if (s->cqLocks[i]) IOLockFree(s->cqLocks[i]);
        if (s->tableLock) IOLockFree(s->tableLock);
        if (s->table) IODelete(s->table, MlxCQContext, s->tableCap);
        if (s->used) IODelete(s->used, bool, s->tableCap);
        if (s->cqLocks) IODelete(s->cqLocks, IOLock *, s->tableCap);
        if (s->cqnKeys) IODelete(s->cqnKeys, uint32_t, hashCap);
        if (s->cqnSlots) IODelete(s->cqnSlots, int32_t, hashCap);
        delete s; s = NULL;
        return kIOReturnNoMemory;
    }
    return kIOReturnSuccess;
}

void
MlxCQ::Free()
{
    if (!s) return;
    bool unverified = s->core->DmaQuarantined();
    while (!unverified) {
        uint32_t cqn = 0;
        for (int i = 0; i < s->tableCap; i++) {
            if (s->used[i]) { cqn = s->table[i].cqNumber; break; }
        }
        if (!cqn) break;
        if (DestroyCQ(cqn) != kIOReturnSuccess) {
            MLX_LOG("CQ[%u] destroy unverified; DMA retained", cqn);
            unverified = true;
            break;
        }
    }
    if (unverified) {
        for (int i = 0; i < s->tableCap; i++) {
            if (!s->used[i]) continue;
            MlxCQContext *cq = &s->table[i];
            s->core->RetainDmaUntilReset(cq->cqeBufDesc, cq->cqeDmaMap,
                                         0x43510000u | (cq->cqNumber & 0xffffu));
            cq->cqeBufDesc = NULL;
            cq->cqeDmaMap = NULL;
        }
    }
    for (uint32_t i = 0; s->cqLocks && i < s->tableCap; i++)
        if (s->cqLocks[i]) { IOLockFree(s->cqLocks[i]); s->cqLocks[i] = NULL; }
    if (s->tableLock) { IOLockFree(s->tableLock); s->tableLock = NULL; }
    if (s->table) IODelete(s->table, MlxCQContext, s->tableCap);
    if (s->used) IODelete(s->used, bool, s->tableCap);
    if (s->cqLocks) IODelete(s->cqLocks, IOLock *, s->tableCap);
    if (s->cqnKeys) IODelete(s->cqnKeys, uint32_t, s->hashMask + 1u);
    if (s->cqnSlots) IODelete(s->cqnSlots, int32_t, s->hashMask + 1u);
    delete s; s = NULL;
}

MlxCQContext *
MlxCQ::LockCq(uint32_t cqHandle)
{
    if (!s || !cqHandle) return NULL;
    IOLockLock(s->tableLock);
    int slot = mlxKeyIndexFind(s->cqnKeys, s->cqnSlots, s->hashMask, s->hashShift, cqHandle);
    if (slot < 0 || !s->used[slot]) {
        IOLockUnlock(s->tableLock);
        return NULL;
    }
    IOLockLock(s->cqLocks[slot]);
    IOLockUnlock(s->tableLock);
    return &s->table[slot];
}

void
MlxCQ::UnlockCq(MlxCQContext *cq)
{
    if (!cq || !s) return;
    int slot = (int)(cq - s->table);
    IOLockUnlock(s->cqLocks[slot]);
}

kern_return_t
MlxCQ::CmdCreateCQ(MlxCQContext *cq, uint32_t eqNumber)
{
    /* mlx5_ifc create_cq_in: CQC at bit 0x80, PAS at bit 0x880 (AppleMCX donor). */
    uint8_t in[4096] = {};
    uint8_t out[64] = {};
    uint32_t cqcOff = 0x80 / 8;
    uint32_t pasOff = 0x880 / 8;

    mlxSetBits(in, 0x00, 16, MLX_CMD_OP_CREATE_CQ);
    uint8_t *cqc = in + cqcOff;
    mlxSetBits(cqc, 0x08, 3, 0);   /* 64-byte CQE */
    mlxSetBits(cqc, 0x63, 5, cq->logSize);
    uint32_t uarPage = cq->clientBundle ? cq->clientBundle->uarIndex :
        (s->core->GetUAR() ? s->core->GetUAR()->GetBootUarIndex() : 0);
    mlxSetBits(cqc, 0x68, 24, uarPage);
    /* c_eqn — 8 bits @0xb8 (AppleMCX), NOT 0xa0/32bit. The real EQ number. */
    mlxSetBits(cqc, 0xb8, 8, eqNumber);
    mlxSetBits(cqc, 0xc3, 5, 0);    /* 4 KiB pages */
    uint64_t dbDma = 0;
    uint32_t dbOffset = 0;
    kern_return_t dkr = !s->core->GetUAR() ? kIOReturnNotReady :
        (cq->clientBundle ?
         s->core->GetUAR()->AllocClientDbSlot(cq->clientBundle, &dbDma, &dbOffset) :
         s->core->GetUAR()->AllocDbSlot(&dbDma, &dbOffset));
    if (dkr != kIOReturnSuccess)
        return kIOReturnNoSpace;
    cq->dbRecordOffset = dbOffset;
    mlxSetBits(cqc, 0x1c0, 64, dbDma);  /* db_record_dma */
    for (uint32_t i = 0; i < cq->numPages; i++)
        mlxSetBits(in, (pasOff * 8) + i * 64, 64, cq->pageDMA[i]);

    uint32_t inSize = pasOff + cq->numPages * 8;
    /* Diagnostic: dump CQC [16..47] + PAS [272..] */
    MLX_DBG("DBG CQC: eqn=0x%x uar=%u logSz=%u pages=%u cqeDMA=0x%llx dbDMA=0x%llx",
            eqNumber, uarPage, cq->logSize, cq->numPages,
            (unsigned long long)cq->pageDMA[0],
            (unsigned long long)dbDma);
    kern_return_t kr = s->core->Exec(MLX_CMD_OP_CREATE_CQ, in, inSize,
                                      out, sizeof(out), 5000);
    if (kr != kIOReturnSuccess) {
        if (cq->clientBundle)
            s->core->GetUAR()->FreeClientDbSlot(cq->clientBundle, dbOffset);
        else s->core->GetUAR()->FreeDbSlot(dbOffset);
        cq->dbRecordOffset = 0;
        return kr;
    }
    cq->cqNumber = (uint32_t)mlxGetBits(out, 0x48, 24);
    cq->eqNumber = eqNumber;
    return kIOReturnSuccess;
}

kern_return_t
MlxCQ::CmdDestroyCQ(uint32_t cqNumber)
{
    uint8_t in[16] = {};
    uint8_t out[16] = {};
    mlxSetBits(in, 0x00, 16, MLX_CMD_OP_DESTROY_CQ);
    mlxSetBits(in, 0x48, 24, cqNumber);   /* cqn @0x48 (destroy_cq_in) */
    return s->core->Exec(MLX_CMD_OP_DESTROY_CQ, in, sizeof(in),
                         out, sizeof(out), 5000);
}

kern_return_t
MlxCQ::CreateCQ(uint32_t entries, struct mlx_create_cq_resp *resp,
                MlxClientDoorbellBundle *bundle)
{
    if (!s || !resp) return kIOReturnBadArgument;
    if (entries < 64) entries = 64;
    if (entries > 2048) return kIOReturnBadArgument;
    uint32_t logSize = 0;
    while ((1u << logSize) < entries) logSize++;

    /* Reserve a slot under the table lock, then fill it without holding the
     * lock (DMA alloc + firmware command can take ms). The CQ is not findable
     * until the cqn index is published below. */
    IOLockLock(s->tableLock);
    int slot = -1;
    for (int i = 0; i < s->tableCap; i++)
        if (!s->used[i]) { slot = i; break; }
    if (slot < 0) { IOLockUnlock(s->tableLock); return kIOReturnNoMemory; }
    s->used[slot] = true;
    IOLockUnlock(s->tableLock);

    MlxCQContext *cq = &s->table[slot];
    memset(cq, 0, sizeof(*cq));
    cq->logSize = logSize;
    cq->cqeSize = 64;
    cq->clientBundle = bundle;

    uint64_t cqBytes = (uint64_t)(1u << logSize) * 64;
    IOBufferMemoryDescriptor *desc = NULL;
    IODMACommand *dmaCmd = NULL;
    IOAddressSegment segs[32];
    uint32_t segCount = 32;
    uint32_t pageCount = 0;
    uint64_t mappedAddr = 0, mappedLen = 0;
    kern_return_t kr = kIOReturnSuccess;

    kr = mlxAllocDmaBuffer(cqBytes, 4096, kIOMemoryDirectionOutIn, &desc);
    if (kr != kIOReturnSuccess || !desc) {
        if (!desc) kr = kIOReturnNoMemory;
        goto fail;
    }

    kr = mlxPrepareDma(s->core->GetPCI(), desc, segs, &segCount, &dmaCmd);
    if (kr != kIOReturnSuccess || segCount == 0) {
        desc->release();
        if (kr == kIOReturnSuccess) kr = kIOReturnNoSpace;
        goto fail;
    }
    for (uint32_t i = 0; i < segCount && pageCount < 32; i++) {
        if (!mlxAppendMttPages(segs[i].address, segs[i].length,
                               cq->pageDMA, 32, &pageCount)) {
            mlxCompleteDma(dmaCmd);
            desc->release();
            kr = kIOReturnNoSpace;
            goto fail;
        }
    }
    cq->numPages = pageCount;
    cq->cqeDMA = cq->pageDMA[0];
    cq->cqeBufDesc = desc;
    cq->cqeDmaMap = dmaCmd;

    if (desc->Map(0, 0, 0, 0, &mappedAddr, &mappedLen) == kIOReturnSuccess) {
        memset((void *)(uintptr_t)mappedAddr, 0, cqBytes);
        for (uint32_t i = 0; i < (1u << logSize); i++)
            ((uint8_t *)(uintptr_t)mappedAddr)[i * 64 + 63] = 0xf1;
        cq->cqeBufAddr = mappedAddr;
        cq->cqeCpu = (volatile struct MlxCqe64 *)(uintptr_t)mappedAddr;
    } else {
        mlxCompleteDma(cq->cqeDmaMap); cq->cqeDmaMap = NULL;
        desc->release(); cq->cqeBufDesc = NULL;
        kr = kIOReturnNoMemory;
        goto fail;
    }

    kr = CmdCreateCQ(cq, s->core->GetEQ() ? s->core->GetEQ()->EqNumber() : 0);
    if (kr != kIOReturnSuccess) {
        mlxCompleteDma(cq->cqeDmaMap); cq->cqeDmaMap = NULL;
        if (cq->cqeBufDesc) { cq->cqeBufDesc->release(); cq->cqeBufDesc = NULL; }
        /* CmdCreateCQ already returned its DB slot on command failure. */
        goto fail;
    }

    IOLockLock(s->tableLock);
    mlxKeyIndexInsert(s->cqnKeys, s->cqnSlots, s->hashMask, s->hashShift, cq->cqNumber, slot);
    IOLockUnlock(s->tableLock);

    resp->cqHandle = cq->cqNumber;
    resp->logSize = cq->logSize;
    resp->cqeSize = cq->cqeSize;
    resp->dbRecordOffset = cq->dbRecordOffset;
    MLX_DBG("CQ[%u] created entries=%u log=%u pages=%u", cq->cqNumber, entries, logSize, pageCount);
    return kIOReturnSuccess;

fail:
    IOLockLock(s->tableLock);
    s->used[slot] = false;
    IOLockUnlock(s->tableLock);
    return kr ? kr : kIOReturnNoMemory;
}

kern_return_t
MlxCQ::DestroyCQ(uint32_t cqHandle)
{
    if (!s) return kIOReturnBadArgument;
    if (s->core->DmaQuarantined()) return kIOReturnNotReady;
    kern_return_t kr = CmdDestroyCQ(cqHandle);
    if (kr != kIOReturnSuccess) return kr;

    IOLockLock(s->tableLock);
    int slot = mlxKeyIndexFind(s->cqnKeys, s->cqnSlots, s->hashMask, s->hashShift, cqHandle);
    if (slot < 0 || !s->used[slot]) {
        IOLockUnlock(s->tableLock);
        return kIOReturnNotFound;
    }
    s->used[slot] = false;
    mlxKeyIndexRemove(s->cqnKeys, s->cqnSlots, s->hashMask, s->hashShift, cqHandle);
    IOLockUnlock(s->tableLock);

    IOLockLock(s->cqLocks[slot]);   /* drain in-flight poll/arm/update */
    MlxCQContext *cq = &s->table[slot];
    if (cq->cqeDmaMap) {
        mlxCompleteDma(cq->cqeDmaMap);
        cq->cqeDmaMap = NULL;
    }
    if (cq->cqeBufDesc) {
        cq->cqeBufDesc->release();
        cq->cqeBufDesc = NULL;
    }
    if (s->core->GetUAR()) {
        if (cq->clientBundle)
            s->core->GetUAR()->FreeClientDbSlot(
                cq->clientBundle, cq->dbRecordOffset);
        else s->core->GetUAR()->FreeDbSlot(cq->dbRecordOffset);
    }
    memset(cq, 0, sizeof(*cq));
    IOLockUnlock(s->cqLocks[slot]);
    MLX_DBG("CQ[%u] destroyed", cqHandle);
    return kIOReturnSuccess;
}

MlxCQContext *
MlxCQ::Lookup(uint32_t cqHandle)
{
    if (!s || !cqHandle) return NULL;
    int slot = mlxKeyIndexFind(s->cqnKeys, s->cqnSlots, s->hashMask, s->hashShift, cqHandle);
    if (slot < 0 || !s->used[slot]) return NULL;
    return &s->table[slot];
}

IOMemoryDescriptor *
MlxCQ::GetCqMemDesc(uint32_t cqHandle)
{
    MlxCQContext *cq = Lookup(cqHandle);
    return cq ? cq->cqeBufDesc : NULL;
}

void
MlxCQ::HandleCompletion(uint32_t cqn)
{
    MlxCQContext *cq = LockCq(cqn);
    if (!cq) return;
    cq->completions++;
    MLX_DBG("completion event cq=%u total=%llu", cqn,
            (unsigned long long)cq->completions);
    if (cq->completionHandler) cq->completionHandler(cqn, cq->completionContext);
    UnlockCq(cq);
}

uint64_t
MlxCQ::GetCompletions(uint32_t cqHandle)
{
    MlxCQContext *cq = LockCq(cqHandle);
    if (!cq || !cq->cqeCpu) {
        if (cq) UnlockCq(cq);
        return 0;
    }
    uint32_t depth = 1u << cq->logSize;
    uint64_t pending = 0;
    while (pending < depth) {
        uint64_t index = cq->consumerIndex + pending;
        const volatile struct MlxCqe64 *entry = &cq->cqeCpu[index & (depth - 1)];
        uint8_t expectedOwner = (uint8_t)((index >> cq->logSize) & 1);
        if ((entry->op_own & MLX_CQE_OWNER_MASK) != expectedOwner ||
            (entry->op_own >> 4) == MLX_CQE_INVALID)
            break;
        pending++;
    }
    uint64_t total = cq->completions + pending;
    UnlockCq(cq);
    return total;
}

kern_return_t
MlxCQ::UpdateCqConsumer(uint32_t cqHandle, uint32_t consumerIndex)
{
    MlxCQContext *cq = LockCq(cqHandle);
    if (!cq) return kIOReturnNotFound;
    uint32_t current = cq->consumerIndex & 0xffffffu;
    uint32_t requested = consumerIndex & 0xffffffu;
    uint32_t forward = (requested - current) & 0xffffffu;
    uint32_t depth = 1u << cq->logSize;
    if (forward > depth) { UnlockCq(cq); return kIOReturnBadArgument; }
    cq->consumerIndex = requested;
    volatile uint32_t *db = s->core->GetUAR() ?
        (cq->clientBundle ? s->core->GetUAR()->GetClientDbRecord(
            cq->clientBundle, cq->dbRecordOffset) :
         s->core->GetUAR()->GetDbRecord(cq->dbRecordOffset)) : NULL;
    if (!db) { UnlockCq(cq); return kIOReturnNotReady; }
    db[0] = OSSwapHostToBigInt32(consumerIndex & 0xffffff);
    mlxMemoryBarrier();
    UnlockCq(cq);
    return kIOReturnSuccess;
}

kern_return_t
MlxCQ::PollCQ(const struct mlx_poll_cq_req *req, struct mlx_poll_cq_resp *resp)
{
    if (!s || !req || !resp || !req->maxEntries ||
        req->maxEntries > MLX_UC_MAX_POLL_WC)
        return kIOReturnBadArgument;
    MlxCQContext *cq = LockCq(req->cqHandle);
    if (!cq || !cq->cqeCpu) {
        if (cq) UnlockCq(cq);
        return kIOReturnNotFound;
    }
    memset(resp, 0, sizeof(*resp));
    uint32_t depth = 1u << cq->logSize;
    while (resp->count < req->maxEntries) {
        uint32_t idx = (uint32_t)cq->consumerIndex & (depth - 1);
        const volatile struct MlxCqe64 *entry = &cq->cqeCpu[idx];
        uint8_t expectedOwner = (uint8_t)((cq->consumerIndex >> cq->logSize) & 1);
        uint8_t opOwn = entry->op_own;
        if ((opOwn & MLX_CQE_OWNER_MASK) != expectedOwner ||
            (opOwn >> 4) == MLX_CQE_INVALID)
            break;
        mlxMemoryBarrier();
        struct MlxCqe64 local;
        memcpy(&local, (const void *)(uintptr_t)entry, sizeof(local));
        struct mlx_work_completion *wc = &resp->wc[resp->count];
        if (!s->roce->GetQP() ||
            !s->roce->GetQP()->CompleteCQE(cq->cqNumber, &local, wc)) {
            MLX_LOG("PollCQ: CompleteCQE fallback cq=%u getQP=%s op_own=0x%02x",
                    cq->cqNumber, s->roce->GetQP() ? "ok" : "NULL", opOwn);
            cq->lost++;
            wc->status = MLX_UC_WC_GENERAL;
            wc->vendorError = 0xffffffffu;
            wc->byteLen = OSSwapBigToHostInt32(local.byte_cnt);
            wc->qpNum = OSSwapBigToHostInt32(local.sop_drop_qpn) & 0xffffff;
        }
        cq->consumerIndex++;
        resp->count++;
    }
    uint32_t ci = (uint32_t)cq->consumerIndex;
    volatile uint32_t *db = s->core->GetUAR() ?
        (cq->clientBundle ? s->core->GetUAR()->GetClientDbRecord(
            cq->clientBundle, cq->dbRecordOffset) :
         s->core->GetUAR()->GetDbRecord(cq->dbRecordOffset)) : NULL;
    if (resp->count && db && !cq->clientBundle) {
        db[0] = OSSwapHostToBigInt32(ci & 0xffffff);
        mlxMemoryBarrier();
    }
    UnlockCq(cq);
    return kIOReturnSuccess;
}

kern_return_t
MlxCQ::ArmCQ(uint32_t cqHandle, uint32_t solicitedOnly)
{
    if (!s) return kIOReturnBadArgument;
    MlxCQContext *cq = LockCq(cqHandle);
    if (!cq) return kIOReturnNotFound;
    volatile uint32_t *db = s->core->GetUAR() ?
        (cq->clientBundle ? s->core->GetUAR()->GetClientDbRecord(
            cq->clientBundle, cq->dbRecordOffset) :
         s->core->GetUAR()->GetDbRecord(cq->dbRecordOffset)) : NULL;
    if (!db) { UnlockCq(cq); return kIOReturnNotReady; }
    /* mlx5 CQ arm: dbrec[1] = ci | (sn<<28) | request mode. */
    uint32_t ci = (uint32_t)cq->consumerIndex & 0xffffffu;
    uint32_t sn = (cq->armSn++) & 3u;
    uint32_t val = ci | (sn << 28) |
                   (solicitedOnly ? MLX_CQ_DB_REQ_NOT_SOL : MLX_CQ_DB_REQ_NOT);
    db[1] = OSSwapHostToBigInt32(val);
    mlxMemoryBarrier();
    kern_return_t ringKr = s->core->GetUAR()->RingCQDoorbell(
        cq->clientBundle ? cq->clientBundle->uarIndex :
                           s->core->GetUAR()->GetBootUarIndex(),
        val, cq->cqNumber);
    if (ringKr != kIOReturnSuccess) {
        UnlockCq(cq);
        return ringKr;
    }
    cq->armed = true;
    UnlockCq(cq);
    MLX_DBG("CQ[%u] armed solicited=%u ci=%u sn=%u",
            cqHandle, solicitedOnly ? 1 : 0, ci, sn & 3u);
    return kIOReturnSuccess;
}
