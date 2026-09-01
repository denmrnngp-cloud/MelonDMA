/*
 * MlxMR.cpp — Memory Registration (DriverKit port).
 *
 * Ported from: drivers/infiniband/hw/mlx5/mr.c (reg_user_mr/dereg). The PBL
 * is built by splitting IOVA segments into 4 KiB HCA PAS via MlxDMA::Pin +
 * mlxAppendMttPages, then encoded with mlxEncodeCreateMkey (host-tested). The
 * lkey/rkey are composed as (mkey_index << 8) | key_variant.
 */
#include "MlxMR.hpp"
#include "MlxRoCE.hpp"
#include "MlxPCIDriver.h"
#include "MlxCmd.hpp"
#include "MlxDMA.hpp"
#include "MlxDriverKitCompat.h"
#include "MlxP0Encoding.hpp"
#include "MlxUCIO.h"

#include <DriverKit/IOLib.h>
#include <string.h>

#include "MlxLog.hpp"
#define MLX_LOG(fmt, ...)  IOLog("MlxPCIDriver: MlxMR: " fmt "\n", ##__VA_ARGS__)
#define MLX_DBG(fmt, ...)  MLX_DBGLOG("MlxPCIDriver: MlxMR: " fmt, ##__VA_ARGS__)
/* §3: table sized from firmware max_mkey at Init (capped; see Init). The
 * 512 here is only the fallback/floor, not a hard capability. */
#define MLX_MR_TABLE_MAX   4096

/* O(1) lkey/rkey → slot index (audit P0.2) — see MlxKeyIndex.hpp. */
#include "MlxKeyIndex.hpp"

struct MlxMR::State {
    MlxRoCE      *roce;
    MlxPCIDriver *core;
    struct IOLock *lock;
    MlxMRContext  *table;
    bool          *used;
    uint32_t       tableCap;
    uint8_t        variant;
    uint32_t      *lkeyBuckets;
    int32_t       *lkeySlots;
    uint32_t      *rkeyBuckets;
    int32_t       *rkeySlots;
    uint32_t       hashMask;
    uint32_t       hashShift;
};

MlxMR::MlxMR() : s(NULL) {}
MlxMR::~MlxMR() { Free(); }

kern_return_t
MlxMR::Init(MlxRoCE *roce)
{
    if (!roce) return kIOReturnBadArgument;
    s = new State;
    if (!s) return kIOReturnNoMemory;
    memset(s, 0, sizeof(*s));
    s->roce = roce;
    s->core = roce->GetCore();
    uint32_t fwMax = (s->core->GetHCA() && s->core->GetHCA()->Caps().maxMr) ?
                     s->core->GetHCA()->Caps().maxMr : MLX_MR_TABLE_MAX;
    s->tableCap = fwMax < MLX_MR_TABLE_MAX ? fwMax : MLX_MR_TABLE_MAX;
    if (s->tableCap < 64) s->tableCap = 64;
    uint32_t hashCap = mlxKeyIndexCap(s->tableCap);
    s->hashMask = hashCap - 1u;
    s->hashShift = mlxKeyIndexShift(hashCap);
    s->table = IONewZero(MlxMRContext, s->tableCap);
    s->used = IONewZero(bool, s->tableCap);
    s->lkeyBuckets = IONewZero(uint32_t, hashCap);
    s->lkeySlots = IONewZero(int32_t, hashCap);
    s->rkeyBuckets = IONewZero(uint32_t, hashCap);
    s->rkeySlots = IONewZero(int32_t, hashCap);
    s->lock = IOLockAlloc();
    if (!s->table || !s->used || !s->lkeyBuckets || !s->lkeySlots ||
        !s->rkeyBuckets || !s->rkeySlots || !s->lock) {
        if (s->table) IODelete(s->table, MlxMRContext, s->tableCap);
        if (s->used) IODelete(s->used, bool, s->tableCap);
        if (s->lkeyBuckets) IODelete(s->lkeyBuckets, uint32_t, hashCap);
        if (s->lkeySlots) IODelete(s->lkeySlots, int32_t, hashCap);
        if (s->rkeyBuckets) IODelete(s->rkeyBuckets, uint32_t, hashCap);
        if (s->rkeySlots) IODelete(s->rkeySlots, int32_t, hashCap);
        if (s->lock) IOLockFree(s->lock);
        delete s; s = NULL;
        return kIOReturnNoMemory;
    }
    s->variant = 1;
    return kIOReturnSuccess;
}

void
MlxMR::Free()
{
    if (!s) return;
    bool unverified = s->core->DmaQuarantined();
    while (!unverified) {
        uint32_t handle = 0;
        for (int i = 0; i < s->tableCap; i++) {
            if (s->used[i]) { handle = s->table[i].mrHandle; break; }
        }
        if (!handle) break;
        if (DeregMR(handle) != kIOReturnSuccess) {
            MLX_LOG("MR[%u] destroy unverified; DMA retained", handle);
            unverified = true;
            break;
        }
    }
    if (unverified) {
        for (int i = 0; i < s->tableCap; i++) {
            if (!s->used[i]) continue;
            MlxMRContext *ctx = &s->table[i];
            s->core->RetainDmaUntilReset(ctx->fMemDesc, ctx->dma.dmaCmd,
                                         0x4d520000u | (ctx->mrHandle & 0xffffu));
            ctx->fMemDesc = NULL;
            ctx->dma.memDesc = NULL;
            ctx->dma.dmaCmd = NULL;
        }
    }
    if (s->lock) { IOLockFree(s->lock); s->lock = NULL; }
    if (s->table) IODelete(s->table, MlxMRContext, s->tableCap);
    if (s->used) IODelete(s->used, bool, s->tableCap);
    if (s->lkeyBuckets) IODelete(s->lkeyBuckets, uint32_t, s->hashMask + 1u);
    if (s->lkeySlots) IODelete(s->lkeySlots, int32_t, s->hashMask + 1u);
    if (s->rkeyBuckets) IODelete(s->rkeyBuckets, uint32_t, s->hashMask + 1u);
    if (s->rkeySlots) IODelete(s->rkeySlots, int32_t, s->hashMask + 1u);
    delete s; s = NULL;
}

kern_return_t
MlxMR::RegMR(const struct mlx_reg_mr_req *req, IOMemoryDescriptor *clientMemory,
             struct mlx_reg_mr_resp *resp)
{
    if (!s || !req || !resp || !clientMemory) return kIOReturnBadArgument;
    if (!req->startAddr || !req->length ||
        req->length > (uint64_t)MLX_MAX_DMA_PAGES * 4096 ||
        req->startAddr + req->length < req->startAddr)
        return kIOReturnBadArgument;

    MlxDMAReq dmaReq;
    kern_return_t kr = s->core->GetDMA()->Pin(clientMemory, &dmaReq);
    if (kr != kIOReturnSuccess) return kr;
    dmaReq.va = req->startAddr;
    dmaReq.len = req->length;

    uint32_t inputSize = 0;
    uint8_t in[MLX_CMD_MAX_SIZE] = {};
    uint8_t out[16] = {};
    IOLockLock(s->lock);
    uint8_t variant = s->variant++;
    if (!s->variant) s->variant = 1; /* zero is reserved; wrap deliberately */
    IOLockUnlock(s->lock);
    if (!mlxEncodeCreateMkey(in, sizeof(in), dmaReq.pageDMA, dmaReq.numPages,
                             req->startAddr, req->length, req->accessFlags & MLX_MR_ACCESS_SUPPORTED,
                             req->pd, variant, &inputSize)) {
        s->core->GetDMA()->Unpin(&dmaReq);
        return kIOReturnBadArgument;
    }
    mlxSetBits(in, 0x00, 16, MLX_CMD_OP_CREATE_MKEY);

    kr = s->core->Exec(MLX_CMD_OP_CREATE_MKEY, in, inputSize,
                       out, sizeof(out), 5000);
    if (kr != kIOReturnSuccess) {
        s->core->GetDMA()->Unpin(&dmaReq);
        return kr;
    }

    uint32_t mkeyIndex = (uint32_t)mlxGetBits(out, 0x48, 24);
    uint32_t composedLkey = mlxComposeMkey(mkeyIndex, variant);
    IOLockLock(s->lock);
    int slot = -1;
    for (int i = 0; i < s->tableCap; i++)
        if (!s->used[i]) { slot = i; break; }
    if (slot < 0) {
        IOLockUnlock(s->lock);
        uint8_t din[16] = {}, dout[16] = {};
        mlxSetBits(din, 0x00, 16, MLX_CMD_OP_DESTROY_MKEY);
        mlxSetBits(din, 0x48, 24, mkeyIndex);
        s->core->Exec(MLX_CMD_OP_DESTROY_MKEY, din, sizeof(din),
                      dout, sizeof(dout), 5000);
        s->core->GetDMA()->Unpin(&dmaReq);
        return kIOReturnNoMemory;
    }
    MlxMRContext *ctx = &s->table[slot];
    memset(ctx, 0, sizeof(*ctx));
    ctx->mrHandle = mkeyIndex;
    ctx->pd = req->pd;
    ctx->lkey = composedLkey;
    ctx->rkey = composedLkey;   /* lkey==rkey for mlx5 (PRM Table 9) */
    /* The MR and WQE addresses remain client virtual addresses. PAS supplies
     * the independent IOMMU translation, matching the standard verbs model. */
    ctx->startAddr = req->startAddr;
    ctx->length = req->length;
    ctx->accessFlags = req->accessFlags;
    clientMemory->retain();
    ctx->fMemDesc = clientMemory;
    ctx->dma = dmaReq;
    s->used[slot] = true;
    mlxKeyIndexInsert(s->lkeyBuckets, s->lkeySlots, s->hashMask, s->hashShift, composedLkey, slot);
    mlxKeyIndexInsert(s->rkeyBuckets, s->rkeySlots, s->hashMask, s->hashShift, composedLkey, slot);
    IOLockUnlock(s->lock);

    resp->mrHandle = mkeyIndex;
    resp->lkey = composedLkey;
    resp->rkey = composedLkey;
    resp->iova = req->startAddr;
    MLX_LOG("MR[%u] registered va=0x%llx len=%llu pages=%u pas0=0x%llx lkey=0x%x",
            mkeyIndex, req->startAddr, req->length, dmaReq.numPages,
            dmaReq.pageDMA[0], composedLkey);
    return kIOReturnSuccess;
}

kern_return_t
MlxMR::RegMRIndirect(const struct mlx_reg_mr_indirect_req *req,
                     struct mlx_reg_mr_resp *resp)
{
    if (!s || !req || !resp) return kIOReturnBadArgument;
    if (!req->childCount || req->childCount > MLX_UC_MAX_INDIRECT_CHILDREN ||
        !req->length)
        return kIOReturnBadArgument;

    /* Children must already be registered (a prior RegMR) — this composes
     * existing direct MRs under one new mkey, it does not pin any new
     * client memory of its own. */
    struct MlxKlmEntry klms[MLX_UC_MAX_INDIRECT_CHILDREN];
    for (uint32_t i = 0; i < req->childCount; i++) {
        MlxMRContext *child = Lookup(req->childHandles[i]);
        if (!child) return kIOReturnNotFound;
        klms[i].address   = child->startAddr;
        klms[i].byteCount = child->length;
        klms[i].mkey      = child->lkey;
    }

    uint32_t inputSize = 0;
    uint8_t in[MLX_CREATE_MKEY_FIXED_BYTES +
               MLX_UC_MAX_INDIRECT_CHILDREN * MLX_KLM_ENTRY_BYTES] = {};
    uint8_t out[16] = {};
    IOLockLock(s->lock);
    uint8_t variant = s->variant++;
    if (!s->variant) s->variant = 1; /* zero is reserved; wrap deliberately */
    IOLockUnlock(s->lock);
    if (!mlxEncodeCreateMkeyIndirect(in, sizeof(in), klms, req->childCount,
                                     req->startAddr, req->length,
                                     req->accessFlags & MLX_MR_ACCESS_SUPPORTED, req->pd, variant,
                                     &inputSize))
        return kIOReturnBadArgument;
    mlxSetBits(in, 0x00, 16, MLX_CMD_OP_CREATE_MKEY);

    kern_return_t kr = s->core->Exec(MLX_CMD_OP_CREATE_MKEY, in, inputSize,
                                     out, sizeof(out), 5000);
    if (kr != kIOReturnSuccess) return kr;

    uint32_t mkeyIndex = (uint32_t)mlxGetBits(out, 0x48, 24);
    uint32_t composedLkey = mlxComposeMkey(mkeyIndex, variant);
    IOLockLock(s->lock);
    int slot = -1;
    for (int i = 0; i < s->tableCap; i++)
        if (!s->used[i]) { slot = i; break; }
    if (slot < 0) {
        IOLockUnlock(s->lock);
        uint8_t din[16] = {}, dout[16] = {};
        mlxSetBits(din, 0x00, 16, MLX_CMD_OP_DESTROY_MKEY);
        mlxSetBits(din, 0x48, 24, mkeyIndex);
        s->core->Exec(MLX_CMD_OP_DESTROY_MKEY, din, sizeof(din),
                      dout, sizeof(dout), 5000);
        return kIOReturnNoMemory;
    }
    MlxMRContext *ctx = &s->table[slot];
    memset(ctx, 0, sizeof(*ctx));
    /* fMemDesc/dma stay zeroed: an indirect mkey pins nothing of its own,
     * it only references its children's already-pinned memory. DeregMR's
     * existing cleanup (Unpin on a zeroed MlxDMAReq, NULL-checked release
     * on fMemDesc) is already safe against exactly this shape. */
    ctx->mrHandle = mkeyIndex;
    ctx->pd = req->pd;
    ctx->lkey = composedLkey;
    ctx->rkey = composedLkey;
    ctx->startAddr = req->startAddr;
    ctx->length = req->length;
    ctx->accessFlags = req->accessFlags;
    ctx->childCount = req->childCount;
    for (uint32_t i = 0; i < req->childCount; i++)
        ctx->childHandles[i] = req->childHandles[i];
    for (uint32_t i = 0; i < req->childCount; i++) {
        MlxMRContext *child = Lookup(req->childHandles[i]);
        if (child) child->dependentCount++;
    }
    s->used[slot] = true;
    mlxKeyIndexInsert(s->lkeyBuckets, s->lkeySlots, s->hashMask, s->hashShift, composedLkey, slot);
    mlxKeyIndexInsert(s->rkeyBuckets, s->rkeySlots, s->hashMask, s->hashShift, composedLkey, slot);
    IOLockUnlock(s->lock);

    resp->mrHandle = mkeyIndex;
    resp->lkey = composedLkey;
    resp->rkey = composedLkey;
    resp->iova = req->startAddr;
    MLX_LOG("MR[%u] indirect registered children=%u va=0x%llx len=%llu lkey=0x%x",
            mkeyIndex, req->childCount, req->startAddr, req->length, composedLkey);
    return kIOReturnSuccess;
}

kern_return_t
MlxMR::DeregMR(uint32_t mrHandle)
{
    if (!s) return kIOReturnBadArgument;
    if (s->core->DmaQuarantined()) return kIOReturnNotReady;
    IOLockLock(s->lock);
    MlxMRContext *target = Lookup(mrHandle);
    if (target && target->isWindow) {
        uint32_t oldRkey = target->rkey;
        int slot = (int)(target - s->table);
        mlxKeyIndexRemove(s->rkeyBuckets, s->rkeySlots, s->hashMask, s->hashShift, oldRkey);
        s->used[slot] = false;
        memset(target, 0, sizeof(*target));
        IOLockUnlock(s->lock);
        return kIOReturnSuccess;
    }
    if (!target) { IOLockUnlock(s->lock); return kIOReturnNotFound; }
    if (target->dependentCount) {
        IOLockUnlock(s->lock);
        return kIOReturnBusy;
    }
    IOLockUnlock(s->lock);
    uint8_t in[16] = {};
    uint8_t out[16] = {};
    mlxSetBits(in, 0x00, 16, MLX_CMD_OP_DESTROY_MKEY);
    mlxSetBits(in, 0x48, 24, mrHandle);
    kern_return_t kr = s->core->Exec(MLX_CMD_OP_DESTROY_MKEY, in, sizeof(in),
                                      out, sizeof(out), 5000);
    if (kr != kIOReturnSuccess) return kr;
    IOLockLock(s->lock);
    for (int i = 0; i < s->tableCap; i++) {
        if (s->used[i] && s->table[i].mrHandle == mrHandle) {
            for (uint32_t j = 0; j < s->table[i].childCount; j++) {
                MlxMRContext *child = Lookup(s->table[i].childHandles[j]);
                if (child && child->dependentCount) child->dependentCount--;
            }
            s->core->GetDMA()->Unpin(&s->table[i].dma);
            if (s->table[i].fMemDesc) {
                s->table[i].fMemDesc->release();
                s->table[i].fMemDesc = NULL;
            }
            if (s->table[i].lkey)
                mlxKeyIndexRemove(s->lkeyBuckets, s->lkeySlots,
                                  s->hashMask, s->hashShift,
                                  s->table[i].lkey);
            mlxKeyIndexRemove(s->rkeyBuckets, s->rkeySlots,
                              s->hashMask, s->hashShift,
                              s->table[i].rkey);
            s->used[i] = false;
            memset(&s->table[i], 0, sizeof(s->table[i]));
            break;
        }
    }
    IOLockUnlock(s->lock);
    MLX_LOG("MR[%u] deregistered", mrHandle);
    return kIOReturnSuccess;
}

MlxMRContext *
MlxMR::Lookup(uint32_t mrHandle)
{
    for (int i = 0; i < s->tableCap; i++)
        if (s->used[i] && s->table[i].mrHandle == mrHandle) return &s->table[i];
    return NULL;
}

MlxMRContext *
MlxMR::LookupByRkey(uint32_t rkey)
{
    if (!s || !rkey) return NULL;
    int slot = mlxKeyIndexFind(s->rkeyBuckets, s->rkeySlots, s->hashMask, s->hashShift, rkey);
    if (slot < 0) return NULL;
    return (s->used[slot] && s->table[slot].rkey == rkey) ? &s->table[slot] : NULL;
}

MlxMRContext *
MlxMR::LookupByLkey(uint32_t lkey)
{
    if (!s || !lkey) return NULL;
    int slot = mlxKeyIndexFind(s->lkeyBuckets, s->lkeySlots, s->hashMask, s->hashShift, lkey);
    if (slot < 0) return NULL;
    return (s->used[slot] && s->table[slot].lkey == lkey) ? &s->table[slot] : NULL;
}

kern_return_t
MlxMR::AllocMW(uint32_t pd, uint32_t type, uint32_t *handle, uint32_t *rkey)
{
    if (!s || !handle || !rkey || type != 2 || !pd) return kIOReturnBadArgument;
    IOLockLock(s->lock);
    int slot = -1;
    for (int i = 0; i < s->tableCap; i++) if (!s->used[i]) { slot = i; break; }
    if (slot < 0) { IOLockUnlock(s->lock); return kIOReturnNoMemory; }
    MlxMRContext *ctx = &s->table[slot];
    memset(ctx, 0, sizeof(*ctx));
    uint8_t variant = s->variant++;
    if (!s->variant) s->variant = 1;
    uint8_t in[MLX_CMD_MAX_SIZE] = {}, out[16] = {};
    uint32_t inputSize = 0;
    if (!mlxEncodeCreateMkeyWindow(in, sizeof(in), pd, variant, &inputSize)) {
        IOLockUnlock(s->lock); return kIOReturnBadArgument;
    }
    mlxSetBits(in, 0x00, 16, MLX_CMD_OP_CREATE_MKEY);
    IOLockUnlock(s->lock);
    kern_return_t kr = s->core->Exec(MLX_CMD_OP_CREATE_MKEY, in, inputSize,
                                     out, sizeof(out), 5000);
    if (kr != kIOReturnSuccess) return kr;
    uint32_t index = (uint32_t)mlxGetBits(out, 0x48, 24);
    if (!index) return kIOReturnIOError;
    IOLockLock(s->lock);
    for (int i = 0; i < s->tableCap; i++) if (!s->used[i]) { slot = i; break; }
    if (slot < 0) { IOLockUnlock(s->lock); return kIOReturnNoMemory; }
    ctx = &s->table[slot];
    memset(ctx, 0, sizeof(*ctx));
    ctx->mrHandle = index;
    ctx->pd = pd; ctx->isWindow = true; ctx->mwType = type;
    ctx->rkey = mlxComposeMkey(index, variant);
    s->used[slot] = true;
    mlxKeyIndexInsert(s->rkeyBuckets, s->rkeySlots, s->hashMask, s->hashShift, ctx->rkey, slot);
    *handle = ctx->mrHandle; *rkey = ctx->rkey;
    IOLockUnlock(s->lock);
    return kIOReturnSuccess;
}

kern_return_t
MlxMR::DeallocMW(uint32_t handle)
{
    if (!s || !handle) return kIOReturnBadArgument;
    if (s->core->DmaQuarantined()) return kIOReturnNotReady;
    IOLockLock(s->lock);
    MlxMRContext *ctx = Lookup(handle);
    if (!ctx || !ctx->isWindow) { IOLockUnlock(s->lock); return kIOReturnNotFound; }
    uint32_t boundMr = ctx->boundMrHandle;
    uint32_t mkey = mlxMkeyIndex(ctx->rkey);
    IOLockUnlock(s->lock);
    uint8_t in[16] = {}, out[16] = {};
    mlxSetBits(in, 0x00, 16, MLX_CMD_OP_DESTROY_MKEY);
    mlxSetBits(in, 0x48, 24, mkey);
    kern_return_t kr = s->core->Exec(MLX_CMD_OP_DESTROY_MKEY, in, sizeof(in),
                                     out, sizeof(out), 5000);
    if (kr != kIOReturnSuccess) return kr;
    IOLockLock(s->lock);
    ctx = Lookup(handle);
    if (!ctx || !ctx->isWindow) { IOLockUnlock(s->lock); return kIOReturnNotFound; }
    int slot = (int)(ctx - s->table);
    uint32_t oldRkey = ctx->rkey;
    if (boundMr) {
        MlxMRContext *parent = Lookup(boundMr);
        if (parent && parent->dependentCount) parent->dependentCount--;
    }
    mlxKeyIndexRemove(s->rkeyBuckets, s->rkeySlots, s->hashMask, s->hashShift, oldRkey);
    s->used[slot] = false;
    memset(ctx, 0, sizeof(*ctx));
    IOLockUnlock(s->lock);
    return kIOReturnSuccess;
}

kern_return_t
MlxMR::BindMW(uint32_t handle, uint32_t qpn, uint32_t mrHandle,
              uint32_t bindRkey, uint32_t accessFlags, uint64_t addr,
              uint64_t length, uint32_t *rkey)
{
    if (!s || !handle || !qpn || !mrHandle || !bindRkey || !length || !rkey ||
        (accessFlags & ~MLX_MR_ACCESS_SUPPORTED)) return kIOReturnBadArgument;
    IOLockLock(s->lock);
    MlxMRContext *mw = Lookup(handle), *mr = Lookup(mrHandle);
    if (!mw || !mw->isWindow || mw->rkey != bindRkey || !mr || mr->isWindow || mw->pd != mr->pd ||
        mr->invalidated || addr < mr->startAddr ||
        length > mr->length || addr > mr->startAddr + mr->length - length ||
        !(mr->accessFlags & (1u << 4))) { /* IBV_ACCESS_MW_BIND */
        IOLockUnlock(s->lock); return kIOReturnNotPermitted;
    }
    if ((accessFlags & (MLX_MR_ACCESS_REMOTE_WRITE | MLX_MR_ACCESS_REMOTE_ATOMIC)) &&
        !(mr->accessFlags & MLX_MR_ACCESS_LOCAL_WRITE)) {
        IOLockUnlock(s->lock); return kIOReturnNotPermitted;
    }
    if (mw->boundMrHandle && mw->boundMrHandle != mrHandle) {
        MlxMRContext *old = Lookup(mw->boundMrHandle);
        if (old && old->dependentCount) old->dependentCount--;
    }
    mw->startAddr = addr; mw->length = length; mw->accessFlags = accessFlags;
    mw->invalidated = false;
    if (mw->boundMrHandle != mrHandle) mr->dependentCount++;
    mw->boundMrHandle = mrHandle; mw->boundQpn = qpn;
    uint32_t oldRkey = mw->rkey;
    mw->rkey = mlxComposeMkey(mlxMkeyIndex(mw->rkey), s->variant++);
    if (!s->variant) s->variant = 1;
    mlxKeyIndexRemove(s->rkeyBuckets, s->rkeySlots, s->hashMask, s->hashShift, oldRkey);
    mlxKeyIndexInsert(s->rkeyBuckets, s->rkeySlots, s->hashMask, s->hashShift, mw->rkey, (int)(mw - s->table));
    *rkey = mw->rkey;
    IOLockUnlock(s->lock);
    return kIOReturnSuccess;
}

kern_return_t
MlxMR::InvalidateKey(uint32_t rkey)
{
    if (!s || !rkey) return kIOReturnBadArgument;
    IOLockLock(s->lock);
    MlxMRContext *ctx = LookupByRkey(rkey);
    if (!ctx) { IOLockUnlock(s->lock); return kIOReturnNotFound; }
    ctx->invalidated = true;
    ctx->boundMrHandle = 0;
    ctx->boundQpn = 0;
    IOLockUnlock(s->lock);
    return kIOReturnSuccess;
}

bool
MlxMR::ValidateRange(uint32_t lkey, uint32_t pd, uint64_t addr,
                     uint32_t length, bool deviceWrites)
{
    if (!length || addr + length < addr) return false;
    MlxMRContext *ctx = LookupByLkey(lkey);
    if (!ctx || ctx->invalidated || ctx->pd != pd || addr < ctx->startAddr ||
        addr + length > ctx->startAddr + ctx->length)
        return false;
    /* Device writes require LOCAL_WRITE on the registered MR. */
    return !deviceWrites || (ctx->accessFlags & 1u);
}
