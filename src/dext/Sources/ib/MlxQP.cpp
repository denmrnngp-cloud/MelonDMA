/*
 * MlxQP.cpp — Queue Pair implementation (DriverKit port).
 *
 * Ported from: drivers/infiniband/hw/mlx5/qp.c (create_user_qp,
 * __mlx5_ib_modify_qp), trimmed to RC/UD. QPC encoding uses the portable
 * MlxP0Encoding.hpp encoders (host-tested in Tests/test_all.cpp).
 *
 * DriverKit port: replaces the kext's OSObject metaclass + OSArray/QP-table
 * with a fixed-capacity C++ array and plain new/delete. DMA pinning goes
 * through MlxDMA::Pin (which splits 16KiB host pages into 4KiB HCA PAS).
 * Firmware commands run via MlxPCIDriver::Exec → MlxCmd (mailbox chains).
 *
 * State machine: RST→INIT (RST2INIT 0x502) → INIT→RTR (INIT2RTR 0x503) →
 * RTR→RTS (RTR2RTS 0x504), per MLNX OFED 5.9 (verified opcodes, notes/19 §2.5).
 */
#include "MlxQP.hpp"
#include "MlxRoCE.hpp"
#include "MlxPCIDriver.h"
#include "MlxCmd.hpp"
#include "MlxDMA.hpp"
#include "MlxDriverKitCompat.h"
#include "MlxUAR.hpp"
#include "MlxRegs.hpp"
#include "MlxWQE.hpp"
#include "MlxP0Encoding.hpp"
#include "MlxKeyIndex.hpp"
#include "MlxCQ.hpp"
#include "MlxMR.hpp"
#include "MlxGID.hpp"

#include <DriverKit/IOLib.h>
#include <string.h>

#include "MlxLog.hpp"
#define MLX_LOG(fmt, ...)  IOLog("MlxQP: " fmt "\n", ##__VA_ARGS__)
#define MLX_DBG(fmt, ...)  MLX_DBGLOG("MlxQP: " fmt, ##__VA_ARGS__)
#define MLX_QP_TABLE_MAX  4096

static bool
mlxQpBytesAreZero(const uint8_t *bytes, uint32_t length)
{
    for (uint32_t i = 0; i < length; i++) if (bytes[i]) return false;
    return true;
}

static bool
mlxQpIsIpv4Mapped(const uint8_t gid[16])
{
    for (uint32_t i = 0; i < 10; i++) if (gid[i]) return false;
    return gid[10] == 0xff && gid[11] == 0xff;
}

/* Linux rdma_get_udp_sport(0, lqpn, rqpn): symmetric 20-bit flow label,
 * folded to the standards-defined RoCEv2 dynamic source-port range. */
static uint16_t
mlxQpStandardUdpSport(uint32_t localQpn, uint32_t remoteQpn)
{
    uint64_t value = (uint64_t)localQpn * remoteQpn;
    value ^= value >> 20;
    value ^= value >> 40;
    uint32_t flowLabel = (uint32_t)value & 0xfffff;
    uint32_t low = flowLabel & 0x3fff;
    uint32_t high = flowLabel & 0xfc000;
    return (uint16_t)((low ^ (high >> 14)) | 0xc000);
}

/* P2.1: tag an accepted SQ WQE by opcode for the per-client stats snapshot.
 * *_IMM variants fold into their base opcode; LOCAL_INV is kept separate. */
static void
mlxQpBumpPosted(MlxQPContext *ctx, uint32_t op)
{
    if (!ctx) return;
    switch (op) {
    case MLX_UC_WR_SEND:
    case MLX_UC_WR_SEND_IMM:        ctx->postedSend++; break;
    case MLX_UC_WR_RDMA_WRITE:
    case MLX_UC_WR_RDMA_WRITE_IMM:  ctx->postedWrite++; break;
    case MLX_UC_WR_RDMA_READ:       ctx->postedRead++; break;
    case MLX_UC_WR_LOCAL_INV:       ctx->postedLocalInv++; break;
    /* Atomics fold into the one-sided remote-op counter (like READ); the
     * mlx_stats_resp ABI has no dedicated atomic slot. */
    case MLX_UC_WR_ATOMIC_CS:
    case MLX_UC_WR_ATOMIC_FA:       ctx->postedRead++; break;
    default: break;
    }
}

struct MlxQP::State {
    MlxRoCE      *roce;
    MlxPCIDriver *core;
    struct IOLock *tableLock;          /* used[] + qpn index + slot alloc */
    struct IOLock **qpLocks;           /* per-QP, pre-allocated */
    MlxQPContext  *table;
    bool          *used;
    uint32_t       tableCap;
    int32_t       *qpnSlots;           /* qpn → slot+1 */
    uint32_t      *qpnKeys;
    uint32_t       hashMask;
    uint32_t       hashShift;
};

MlxQP::MlxQP() : s(NULL) {}
MlxQP::~MlxQP() { Free(); }

kern_return_t
MlxQP::Init(MlxRoCE *roce)
{
    if (!roce) return kIOReturnBadArgument;
    s = new State;
    if (!s) return kIOReturnNoMemory;
    memset(s, 0, sizeof(*s));
    s->roce = roce;
    s->core = roce->GetCore();
    uint32_t fwMax = (s->core->GetHCA() && s->core->GetHCA()->Caps().maxQp) ?
                     s->core->GetHCA()->Caps().maxQp : MLX_QP_TABLE_MAX;
    s->tableCap = fwMax < MLX_QP_TABLE_MAX ? fwMax : MLX_QP_TABLE_MAX;
    if (s->tableCap < 64) s->tableCap = 64;
    uint32_t hashCap = mlxKeyIndexCap(s->tableCap);
    s->hashMask = hashCap - 1u;
    s->hashShift = mlxKeyIndexShift(hashCap);

    s->table = IONewZero(MlxQPContext, s->tableCap);
    s->used = IONewZero(bool, s->tableCap);
    s->qpLocks = IONewZero(IOLock *, s->tableCap);
    s->qpnKeys = IONewZero(uint32_t, hashCap);
    s->qpnSlots = IONewZero(int32_t, hashCap);
    s->tableLock = IOLockAlloc();
    bool ok = s->table && s->used && s->qpLocks && s->qpnKeys && s->qpnSlots &&
              s->tableLock;
    for (uint32_t i = 0; ok && i < s->tableCap; i++) {
        s->qpLocks[i] = IOLockAlloc();
        if (!s->qpLocks[i]) ok = false;
    }
    if (!ok) {
        for (uint32_t i = 0; s->qpLocks && i < s->tableCap; i++)
            if (s->qpLocks[i]) IOLockFree(s->qpLocks[i]);
        if (s->tableLock) IOLockFree(s->tableLock);
        if (s->table) IODelete(s->table, MlxQPContext, s->tableCap);
        if (s->used) IODelete(s->used, bool, s->tableCap);
        if (s->qpLocks) IODelete(s->qpLocks, IOLock *, s->tableCap);
        if (s->qpnKeys) IODelete(s->qpnKeys, uint32_t, hashCap);
        if (s->qpnSlots) IODelete(s->qpnSlots, int32_t, hashCap);
        delete s; s = NULL;
        return kIOReturnNoMemory;
    }
    return kIOReturnSuccess;
}

void
MlxQP::Free()
{
    if (!s) return;
    bool unverified = s->core->DmaQuarantined();
    while (!unverified) {
        uint32_t qpn = 0;
        for (int i = 0; i < s->tableCap; i++) {
            if (s->used[i]) { qpn = s->table[i].qpNum; break; }
        }
        if (!qpn) break;
        if (DestroyQP(qpn) != kIOReturnSuccess) {
            MLX_LOG("QP[%u] destroy unverified; DMA retained", qpn);
            unverified = true;
            break;
        }
    }
    if (unverified) {
        for (int i = 0; i < s->tableCap; i++) {
            if (!s->used[i]) continue;
            MlxQPContext *ctx = &s->table[i];
            if (ctx->sqPinned) {
                s->core->RetainDmaUntilReset(ctx->sqDma.memDesc,
                    ctx->sqDma.dmaCmd, 0x51505300u | (ctx->qpNum & 0xffu));
                ctx->sqDma.memDesc = NULL; ctx->sqDma.dmaCmd = NULL;
                ctx->sqPinned = false;
            }
            if (ctx->rqPinned) {
                s->core->RetainDmaUntilReset(ctx->rqDma.memDesc,
                    ctx->rqDma.dmaCmd, 0x51505200u | (ctx->qpNum & 0xffu));
                ctx->rqDma.memDesc = NULL; ctx->rqDma.dmaCmd = NULL;
                ctx->rqPinned = false;
            }
            if (ctx->sqWrid) IODelete(ctx->sqWrid, uint64_t, ctx->sqSize);
            if (ctx->rqWrid) IODelete(ctx->rqWrid, uint64_t, ctx->rqSize);
            if (ctx->sqOpcode) IODelete(ctx->sqOpcode, uint8_t, ctx->sqSize);
            if (ctx->sqSpan) IODelete(ctx->sqSpan, uint8_t, ctx->sqSize);
            ctx->sqWrid = NULL; ctx->rqWrid = NULL; ctx->sqOpcode = NULL;
            ctx->sqSpan = NULL;
        }
    }
    for (uint32_t i = 0; s->qpLocks && i < s->tableCap; i++)
        if (s->qpLocks[i]) { IOLockFree(s->qpLocks[i]); s->qpLocks[i] = NULL; }
    if (s->tableLock) { IOLockFree(s->tableLock); s->tableLock = NULL; }
    if (s->table) IODelete(s->table, MlxQPContext, s->tableCap);
    if (s->used) IODelete(s->used, bool, s->tableCap);
    if (s->qpLocks) IODelete(s->qpLocks, IOLock *, s->tableCap);
    if (s->qpnKeys) IODelete(s->qpnKeys, uint32_t, s->hashMask + 1u);
    if (s->qpnSlots) IODelete(s->qpnSlots, int32_t, s->hashMask + 1u);
    delete s; s = NULL;
}

kern_return_t
MlxQP::CreateQP(const struct mlx_create_qp_req *req,
                struct mlx_create_qp_resp *resp,
                MlxClientDoorbellBundle *bundle)
{
    if (!s || !req || !resp) return kIOReturnBadArgument;
    /* Validate ring sizes (power of two, at least 64 entries). */
    if (!req->sqSize || !req->rqSize || req->sqSize < 64 || req->rqSize < 64 ||
        req->sqBufAddr || req->rqBufAddr ||
        req->maxInlineData > MLX_UC_MAX_INLINE_DATA ||
        req->rsvd ||
        (req->sqSize & (req->sqSize - 1)) || (req->rqSize & (req->rqSize - 1)))
        return kIOReturnBadArgument;
    uint32_t logSq = 31u - __builtin_clz(req->sqSize);
    uint32_t logRq = 31u - __builtin_clz(req->rqSize);
    if (logSq > 15 || logRq > 15) return kIOReturnBadArgument;
    /* The external user client owns its own firmware PD.  Ownership and
     * client isolation are checked by MlxUserClient before reaching here;
     * comparing against the provider's bootstrap PD rejects valid clients. */
    if (!req->pd || req->qpType != 0 || !s->roce->GetCQ() ||
        !s->roce->GetCQ()->Lookup(req->sendCq) ||
        !s->roce->GetCQ()->Lookup(req->recvCq))
        return kIOReturnNotPermitted;
    /* P1.3: a CQ's depth must cover the SQ+RQ of every QP sharing it, or the
     * NIC overflows the CQ and moves the QP to ERR at high IOPS. Warn (not
     * reject) — a shared CQ may legitimately span several small QPs. */
    {
        MlxCQContext *scq = s->roce->GetCQ()->Lookup(req->sendCq);
        MlxCQContext *rcq = s->roce->GetCQ()->Lookup(req->recvCq);
        uint64_t need = (uint64_t)req->sqSize + req->rqSize;
        if (scq && (1ull << scq->logSize) < need)
            MLX_LOG("CQ[%u] undersized for QP: sq=%u rq=%u sum=%llu cqDepth=%llu",
                    scq->cqNumber, req->sqSize, req->rqSize, need,
                    1ull << scq->logSize);
        if (rcq && rcq != scq && (1ull << rcq->logSize) < need)
            MLX_LOG("CQ[%u] undersized for QP: sq=%u rq=%u sum=%llu cqDepth=%llu",
                    rcq->cqNumber, req->sqSize, req->rqSize, need,
                    1ull << rcq->logSize);
    }

    /* build CREATE_QP input: header + opt_param_mask + ece + qpc(0xc0) + pas(0x880). */
    uint8_t in[MLX_CMD_MAX_SIZE] = {};
    uint8_t out[16] = {};
    uint32_t qpcOff = MLX_QPC_BIT_OFFSET / 8;
    uint32_t pasOff = 0x880 / 8;

    mlxSetBits(in, 0x00, 16, MLX_CMD_OP_CREATE_QP);
    mlxSetBits(in, 0x48, 24, 0);            /* input_qpn = allocate */
    mlxSetBits(in, 0x80, 32, 0);            /* opt_param_mask */

    uint8_t *qpc = in + qpcOff;
    uint32_t st = (req->qpType == 0) ? MLX_QP_ST_RC : MLX_QP_ST_UD;
    uint32_t uarPage = bundle ? bundle->uarIndex :
        (s->core->GetUAR() ? s->core->GetUAR()->GetBootUarIndex() : 0);
    uint64_t dbDma = 0;
    uint32_t dbOffset = 0;
    kern_return_t dkr = !s->core->GetUAR() ? kIOReturnNotReady :
        (bundle ? s->core->GetUAR()->AllocClientDbSlot(
                      bundle, &dbDma, &dbOffset) :
                  s->core->GetUAR()->AllocDbSlot(&dbDma, &dbOffset));
    if (dkr != kIOReturnSuccess)
        return kIOReturnNoSpace;
    mlxSetBits(qpc, 0x08, 8, st);
    mlxSetBits(qpc, 0x13, 2, 3);            /* pm_state = MIGRATED (Linux) */
    mlxSetBits(qpc, 0x28, 24, req->pd);
    /* This provider owns a PAS-backed receive ring per QP. SRQ/RMP is a
     * separate verbs object model and is not mixed into RC QP creation. */
    mlxSetBits(qpc, 0x565, 3, 0);
    mlxSetBits(qpc, 0x49, 4, logRq);
    mlxSetBits(qpc, 0x4d, 3, 2);            /* log_rq_stride = log2(64)-4 = 2 (64B WQE) */
    mlxSetBits(qpc, 0x51, 4, logSq);
    mlxSetBits(qpc, 0x5b, 1, 1);            /* rlky (relaxed ordering) */
    mlxSetBits(qpc, 0x68, 24, uarPage);     /* uar_page — a QP without UAR is invalid */
    mlxSetBits(qpc, 0xa3, 5, 0);            /* log_page_size = 4 KiB */
    mlxSetBits(qpc, 0x380, 4, 8);           /* Linux MLX5_IB_ACK_REQ_FREQ */
    mlxSetBits(qpc, 0x394, 1, 1);           /* fre (fast reg enable) */
    mlxSetBits(qpc, 0x3e8, 24, req->sendCq);
    mlxSetBits(qpc, 0x4c8, 24, s->core->GetXrcd());      /* xrcd (dummy) */
    mlxSetBits(qpc, 0x4e8, 24, req->recvCq);
    mlxSetBits(qpc, 0x500, 64, dbDma);      /* dbr_addr — the real DB record */
    /* Native RQ: srqn_rmpn_xrqn remains zero. */
    MLX_DBG("DBG READBACK qpcOff=%u cqn_snd=%llu cqn_rcv=%llu dbr=0x%llx byte125=%u byte127=%u",
            qpcOff,
            (unsigned long long)mlxGetBits(qpc, 0x3e8, 24),
            (unsigned long long)mlxGetBits(qpc, 0x4e8, 24),
            (unsigned long long)mlxGetBits(qpc, 0x500, 64),
            qpc[125], qpc[127]);
    MLX_DBG("QP rq_type=native srqn=0 native_rq=1");

    /* SQ/RQ are intentionally DEXT-owned for kernel-mediated posting. */
    uint64_t sqLen = (uint64_t)req->sqSize * 64;
    uint64_t rqLen = (uint64_t)req->rqSize * 64;
    IOBufferMemoryDescriptor *sqDesc = NULL;
    IOBufferMemoryDescriptor *rqDesc = NULL;
    MlxDMAReq sqDma = {};
    MlxDMAReq rqDma = {};
    kern_return_t kr = mlxAllocDmaBuffer(sqLen, 4096, kIOMemoryDirectionOutIn, &sqDesc);
    if (kr == kIOReturnSuccess)
        kr = mlxAllocDmaBuffer(rqLen, 4096, kIOMemoryDirectionOutIn, &rqDesc);
    if (kr != kIOReturnSuccess || !sqDesc || !rqDesc) {
        if (sqDesc) sqDesc->release();
        if (rqDesc) rqDesc->release();
        if (bundle) s->core->GetUAR()->FreeClientDbSlot(bundle, dbOffset);
        else s->core->GetUAR()->FreeDbSlot(dbOffset);
        return kr ? kr : kIOReturnNoMemory;
    }
    kr = s->core->GetDMA()->Pin(sqDesc, &sqDma);
    if (kr == kIOReturnSuccess)
        kr = s->core->GetDMA()->Pin(rqDesc, &rqDma);
    if (kr != kIOReturnSuccess) {
        if (sqDma.dmaCmd) s->core->GetDMA()->Unpin(&sqDma);
        if (rqDma.dmaCmd) s->core->GetDMA()->Unpin(&rqDma);
        if (sqDesc) sqDesc->release();
        if (rqDesc) rqDesc->release();
        if (bundle) s->core->GetUAR()->FreeClientDbSlot(bundle, dbOffset);
        else s->core->GetUAR()->FreeDbSlot(dbOffset);
        return kr;
    }
    uint64_t sqAddr = 0, sqMapped = 0, rqAddr = 0, rqMapped = 0;
    kr = sqDesc->Map(0, 0, 0, 0, &sqAddr, &sqMapped);
    if (kr == kIOReturnSuccess)
        kr = rqDesc->Map(0, 0, 0, 0, &rqAddr, &rqMapped);
    if (kr != kIOReturnSuccess || sqMapped < sqLen || rqMapped < rqLen) {
        s->core->GetDMA()->Unpin(&sqDma);
        s->core->GetDMA()->Unpin(&rqDma);
        sqDesc->release(); rqDesc->release();
        if (bundle) s->core->GetUAR()->FreeClientDbSlot(bundle, dbOffset);
        else s->core->GetUAR()->FreeDbSlot(dbOffset);
        return kr ? kr : kIOReturnNoMemory;
    }
    memset((void *)(uintptr_t)sqAddr, 0, sqLen);
    memset((void *)(uintptr_t)rqAddr, 0, rqLen);

    mlxSetBits(in, 0x860, 1, 0);            /* wq_umem_valid=0 → WQ buffers via PAS */
    /* Linux layout (qp.c create_qp): RQ at offset 0, SQ after RQ. Otherwise
     * hardware reads SQ pages as RQ and vice versa. */
    uint32_t pasIndex = 0;
    for (uint32_t i = 0; i < rqDma.numPages; i++)
        mlxSetBits(in, pasOff * 8 + (pasIndex++) * 64, 64, rqDma.pageDMA[i]);
    for (uint32_t i = 0; i < sqDma.numPages; i++)
        mlxSetBits(in, pasOff * 8 + (pasIndex++) * 64, 64, sqDma.pageDMA[i]);
    uint32_t inSize = pasOff + pasIndex * sizeof(uint64_t);

    kr = s->core->Exec(MLX_CMD_OP_CREATE_QP, in, inSize,
                       out, sizeof(out), 5000);
    if (kr != kIOReturnSuccess) {
        s->core->GetDMA()->Unpin(&sqDma);
        s->core->GetDMA()->Unpin(&rqDma);
        sqDesc->release();
        rqDesc->release();
        if (bundle) s->core->GetUAR()->FreeClientDbSlot(bundle, dbOffset);
        else s->core->GetUAR()->FreeDbSlot(dbOffset);
        return kr;
    }

    uint32_t qpn = (uint32_t)mlxGetBits(out, 0x48, 24);
    resp->qpn = qpn;
    resp->sqStrideSize = 64;
    resp->dbRecordOffset = dbOffset;
    resp->bfOffset = MLX_BF_OFFSET;
    resp->mappingVersion = bundle ? MLX_FAST_PATH_ABI_VERSION : 0;
    resp->uarPage = uarPage;

    /* Record the context. */
    IOLockLock(s->tableLock);
    int slot = -1;
    for (int i = 0; i < s->tableCap; i++)
        if (!s->used[i]) { slot = i; break; }
    if (slot < 0) {
        IOLockUnlock(s->tableLock);
        uint8_t din[16] = {}, dout[16] = {};
        mlxSetBits(din, 0x00, 16, MLX_CMD_OP_DESTROY_QP);
        mlxSetBits(din, 0x48, 24, qpn);
        s->core->Exec(MLX_CMD_OP_DESTROY_QP, din, sizeof(din),
                      dout, sizeof(dout), 5000);
        s->core->GetDMA()->Unpin(&sqDma);
        s->core->GetDMA()->Unpin(&rqDma);
        sqDesc->release();
        rqDesc->release();
        if (bundle) s->core->GetUAR()->FreeClientDbSlot(bundle, dbOffset);
        else s->core->GetUAR()->FreeDbSlot(dbOffset);
        return kIOReturnNoMemory;
    }
    uint64_t *sqWrid = IONewZero(uint64_t, req->sqSize);
    uint64_t *rqWrid = IONewZero(uint64_t, req->rqSize);
    uint8_t *sqOpcode = IONewZero(uint8_t, req->sqSize);
    uint8_t *sqSpan = IONewZero(uint8_t, req->sqSize);
    if (!sqWrid || !rqWrid || !sqOpcode || !sqSpan) {
        if (sqWrid) IODelete(sqWrid, uint64_t, req->sqSize);
        if (rqWrid) IODelete(rqWrid, uint64_t, req->rqSize);
        if (sqOpcode) IODelete(sqOpcode, uint8_t, req->sqSize);
        if (sqSpan) IODelete(sqSpan, uint8_t, req->sqSize);
        IOLockUnlock(s->tableLock);
        uint8_t din[16] = {}, dout[16] = {};
        mlxSetBits(din, 0x00, 16, MLX_CMD_OP_DESTROY_QP);
        mlxSetBits(din, 0x48, 24, qpn);
        s->core->Exec(MLX_CMD_OP_DESTROY_QP, din, sizeof(din),
                      dout, sizeof(dout), 5000);
        s->core->GetDMA()->Unpin(&sqDma);
        s->core->GetDMA()->Unpin(&rqDma);
        sqDesc->release(); rqDesc->release();
        if (bundle) s->core->GetUAR()->FreeClientDbSlot(bundle, dbOffset);
        else s->core->GetUAR()->FreeDbSlot(dbOffset);
        return kIOReturnNoMemory;
    }
    MlxQPContext *ctx = &s->table[slot];
    memset(ctx, 0, sizeof(*ctx));
    ctx->qpNum = qpn;
    ctx->state = MLX_QP_STATE_RST;
    ctx->st = st;
    ctx->pd = req->pd;
    ctx->sendCq = req->sendCq;
    ctx->recvCq = req->recvCq;
    ctx->uarPage = uarPage;
    ctx->sqSize = req->sqSize;
    ctx->rqSize = req->rqSize;
    ctx->sqBufAddr = req->sqBufAddr;
    ctx->rqBufAddr = req->rqBufAddr;
    ctx->bfOffset = MLX_BF_OFFSET;
    ctx->dbRecordOffset = dbOffset;
    ctx->sqCpu = (volatile uint8_t *)(uintptr_t)sqAddr;
    ctx->rqCpu = (volatile uint8_t *)(uintptr_t)rqAddr;
    ctx->dbRecord = bundle ? s->core->GetUAR()->GetClientDbRecord(
        bundle, dbOffset) : s->core->GetUAR()->GetDbRecord(dbOffset);
    ctx->clientBundle = bundle;
    ctx->sqWrid = sqWrid;
    ctx->rqWrid = rqWrid;
    ctx->sqOpcode = sqOpcode;
    ctx->sqSpan = sqSpan;
    ctx->sqDma = sqDma;
    ctx->rqDma = rqDma;
    ctx->sqPinned = true;
    ctx->rqPinned = true;
    s->used[slot] = true;
    mlxKeyIndexInsert(s->qpnKeys, s->qpnSlots, s->hashMask, s->hashShift, qpn, slot);
    IOLockUnlock(s->tableLock);

    MLX_DBG("QP[%u] created type=%s sq=%u rq=%u (sqPages=%u rqPages=%u)", qpn,
            (req->qpType == 0) ? "RC" : "UD", req->sqSize, req->rqSize,
            sqDma.numPages, rqDma.numPages);
    return kIOReturnSuccess;
}

kern_return_t
MlxQP::ModifyQP(const struct mlx_modify_qp_req *req)
{
    if (!s || !req) return kIOReturnBadArgument;
    MlxQPContext *ctx = LockQp(req->qpn);
    if (!ctx || ctx->state != req->curState) {
        UnlockQp(ctx);
        return ctx ? kIOReturnNotPermitted : kIOReturnNotFound;
    }

    uint32_t opcode;
    switch (ctx->state) {
    case MLX_QP_STATE_RST:
        if (req->newState != MLX_QP_STATE_INIT) { UnlockQp(ctx); return kIOReturnNotPermitted; }
        opcode = MLX_CMD_OP_RST2INIT_QP; break;
    case MLX_QP_STATE_INIT:
        if (req->newState != MLX_QP_STATE_RTR) { UnlockQp(ctx); return kIOReturnNotPermitted; }
        opcode = MLX_CMD_OP_INIT2RTR_QP; break;
    case MLX_QP_STATE_RTR:
        if (req->newState != MLX_QP_STATE_RTS) { UnlockQp(ctx); return kIOReturnNotPermitted; }
        opcode = MLX_CMD_OP_RTR2RTS_QP; break;
    default:
        if (req->newState == MLX_QP_STATE_ERR) opcode = MLX_CMD_OP_2ERR_QP;
        else if (req->newState == MLX_QP_STATE_RST) opcode = MLX_CMD_OP_2RST_QP;
        else { UnlockQp(ctx); return kIOReturnUnsupported; }
        break;
    }

    uint8_t in[MLX_QP_MODIFY_IN_BYTES] = {};
    uint8_t out[16] = {};
    uint8_t *qpc = in + MLX_QPC_BIT_OFFSET / 8;
    uint32_t optParamMask = 0;
    bool ok = opcode == MLX_CMD_OP_2ERR_QP || opcode == MLX_CMD_OP_2RST_QP;

    if (opcode == MLX_CMD_OP_RST2INIT_QP) {
        ok = mlxEncodeRst2InitQpc(qpc, MLX_QPC_BYTES, req->pkeyIndex,
                                  req->portNum, &optParamMask);
    } else if (opcode == MLX_CMD_OP_INIT2RTR_QP) {
        MlxGID *gidTable = s->roce->GetGID();
        uint16_t minUdpSport = s->core->GetHCA()->Caps().roceMinSrcUdpPort;
        if (minUdpSport < 0xc000) minUdpSport = 0xc000;
        if (!gidTable || !gidTable->IsProgrammed(req->ahSgidIndex) ||
            mlxQpBytesAreZero(req->ahDgid, sizeof(req->ahDgid)) ||
            req->ahDgid[0] == 0xff ||
            (mlxQpIsIpv4Mapped(req->ahDgid) &&
             (req->ahDgid[12] == 0 || req->ahDgid[12] >= 224)) ||
            mlxQpBytesAreZero(req->ahDmac, sizeof(req->ahDmac)) ||
            (req->ahDmac[0] & 1) ||
            (req->ahUdpSport && req->ahUdpSport < minUdpSport) ||
            req->sl > 7) {
            UnlockQp(ctx);
            return kIOReturnBadArgument;
        }
        struct MlxRocePathFields path = {};
        memcpy(path.dmac, req->ahDmac, sizeof(path.dmac));
        memcpy(path.dgid, req->ahDgid, sizeof(path.dgid));
        path.sgidIndex = req->ahSgidIndex;
        path.hopLimit = req->ahHopLimit;
        path.trafficClass = req->ahTrafficClass;
        path.udpSport = req->ahUdpSport ? req->ahUdpSport :
                        mlxQpStandardUdpSport(ctx->qpNum, req->destQpn);
        path.pkeyIndex = ctx->pkeyIndex;
        path.portNum = ctx->portNum;
        path.sl = (uint8_t)req->sl;
        ok = mlxEncodeInit2RtrQpc(qpc, MLX_QPC_BYTES, &path, req->destQpn,
                                  req->pathMtu, req->rqPsn,
                                  req->minRnrTimer, req->maxDestRdAtomic,
                                  s->core->GetHCA()->Caps().atomicMode,
                                  s->core->GetHCA()->Caps().logMaxMsg,
                                  &optParamMask);
    } else if (opcode == MLX_CMD_OP_RTR2RTS_QP) {
        ok = mlxEncodeRtr2RtsQpc(qpc, MLX_QPC_BYTES, req->sqPsn,
                                 req->maxRdAtomic,
                                 req->ackTimeout, req->retryCount,
                                 req->rnrRetry,
                                 &optParamMask);
    }
    if (!ok) { UnlockQp(ctx); return kIOReturnBadArgument; }

    mlxSetBits(in, 0x00, 16, opcode);
    mlxSetBits(in, 0x48, 24, req->qpn);
    mlxSetBits(in, 0x80, 32, optParamMask);

    if (opcode == MLX_CMD_OP_INIT2RTR_QP) {
        MLX_DBG("QP[%u] INIT2RTR: mask=0x%08x mtu=%llu remote_qpn=%llu rra_log=%llu atomic_mode=%llu rre=%llu rae=%llu rwe=%llu sgid=%llu udp=%llu port=%llu pm=%llu",
                req->qpn, optParamMask,
                (unsigned long long)mlxGetBits(qpc, 0x40, 3),
                (unsigned long long)mlxGetBits(qpc, 0xa8, 24),
                (unsigned long long)mlxGetBits(qpc, 0x488, 3),
                (unsigned long long)mlxGetBits(qpc, 0x48c, 4),
                (unsigned long long)mlxGetBits(qpc, 0x490, 1),
                (unsigned long long)mlxGetBits(qpc, 0x492, 1),
                (unsigned long long)mlxGetBits(qpc, 0x491, 1),
                (unsigned long long)mlxGetBits(qpc,
                    MLX_QPC_PRIMARY_PATH_BIT_OFFSET + 0x48, 8),
                (unsigned long long)mlxGetBits(qpc,
                    MLX_QPC_PRIMARY_PATH_BIT_OFFSET + 0x110, 16),
                (unsigned long long)mlxGetBits(qpc,
                    MLX_QPC_PRIMARY_PATH_BIT_OFFSET + 0x128, 8),
                (unsigned long long)mlxGetBits(qpc, 0x13, 2));
    } else if (opcode == MLX_CMD_OP_RTR2RTS_QP) {
        MLX_DBG("QP[%u] RTR2RTS: mask=0x%08x sq_psn=%llu sra_log=%llu timeout=%llu retry=%llu rnr_retry=%llu pm=%llu",
                req->qpn, optParamMask,
                (unsigned long long)mlxGetBits(qpc, 0x3c8, 24),
                (unsigned long long)mlxGetBits(qpc, 0x388, 3),
                (unsigned long long)mlxGetBits(qpc,
                    MLX_QPC_PRIMARY_PATH_BIT_OFFSET + 0x40, 5),
                (unsigned long long)mlxGetBits(qpc, 0x38d, 3),
                (unsigned long long)mlxGetBits(qpc, 0x390, 3),
                (unsigned long long)mlxGetBits(qpc, 0x13, 2));
    }

    kern_return_t kr = s->core->Exec(opcode, in, sizeof(in),
                                      out, sizeof(out), 5000);
    if (kr == kIOReturnSuccess) {
        struct mlx_query_qp_resp queried = {};
        kr = QueryQP(req->qpn, &queried);
        if (kr == kIOReturnSuccess && queried.state != req->newState) {
            MLX_LOG("QP[%u] transition verification failed: wanted=%u fw=%u",
                    req->qpn, req->newState, queried.state);
            kr = kIOReturnIOError;
        }
        if (kr == kIOReturnSuccess) {
            if (opcode == MLX_CMD_OP_RST2INIT_QP) {
                ctx->pkeyIndex = (uint16_t)req->pkeyIndex;
                ctx->portNum = (uint8_t)req->portNum;
            } else if (opcode == MLX_CMD_OP_INIT2RTR_QP) {
                memcpy(ctx->ahDmac, req->ahDmac, 6);
                memcpy(ctx->ahDgid, req->ahDgid, 16);
                ctx->ahSgidIndex = req->ahSgidIndex;
                ctx->ahHopLimit = req->ahHopLimit;
                ctx->ahTrafficClass = req->ahTrafficClass;
                ctx->ahUdpSport = req->ahUdpSport;
                ctx->destQpn = req->destQpn;
                ctx->rqPsn = req->rqPsn;
            } else if (opcode == MLX_CMD_OP_RTR2RTS_QP) {
                ctx->sqPsn = req->sqPsn;
            } else if (opcode == MLX_CMD_OP_2RST_QP) {
                ctx->sqHead = ctx->sqTail = 0;
                ctx->rqHead = ctx->rqTail = 0;
                memset((void *)(uintptr_t)ctx->sqCpu, 0, (uint64_t)ctx->sqSize * 64);
                memset((void *)(uintptr_t)ctx->rqCpu, 0, (uint64_t)ctx->rqSize * 64);
            }
            ctx->state = req->newState;
            MLX_DBG("QP[%u] state %u verified by QUERY_QP",
                    req->qpn, queried.state);
        }
    }
    UnlockQp(ctx);
    return kr;
}

kern_return_t
MlxQP::DestroyQP(uint32_t qpn)
{
    if (!s) return kIOReturnBadArgument;
    /* Fenced device: DESTROY_QP would wait on a command response the card can
     * no longer DMA. Refuse cleanly; only a verified FLR clears these objects. */
    if (s->core->DmaQuarantined()) return kIOReturnNotReady;

    /* Lock the QP and mark it non-RTS so no new post can slip in between the
     * in-flight check and the firmware destroy (P1.2). */
    MlxQPContext *ctx = LockQp(qpn);
    if (!ctx) return kIOReturnNotFound;
    if (ctx->sqHead != ctx->sqTail || ctx->rqHead != ctx->rqTail) {
        UnlockQp(ctx);
        MLX_LOG("QP[%u] destroy refused with in-flight WQEs", qpn);
        return kIOReturnBusy;
    }
    ctx->state = MLX_QP_STATE_ERR;
    UnlockQp(ctx);

    uint8_t in[16] = {};
    uint8_t out[16] = {};
    mlxSetBits(in, 0x00, 16, MLX_CMD_OP_DESTROY_QP);
    mlxSetBits(in, 0x48, 24, qpn);
    kern_return_t kr = s->core->Exec(MLX_CMD_OP_DESTROY_QP, in, sizeof(in),
                                      out, sizeof(out), 5000);
    if (kr == kIOReturnSuccess) {
        IOLockLock(s->tableLock);
        int slot = mlxKeyIndexFind(s->qpnKeys, s->qpnSlots, s->hashMask, s->hashShift, qpn);
        if (slot >= 0 && s->used[slot]) {
            s->used[slot] = false;
            mlxKeyIndexRemove(s->qpnKeys, s->qpnSlots, s->hashMask, s->hashShift, qpn);
        } else {
            slot = -1;
        }
        IOLockUnlock(s->tableLock);

        if (slot >= 0) {
            IOLockLock(s->qpLocks[slot]);
            ctx = &s->table[slot];
            if (ctx->sqPinned) {
                IOMemoryDescriptor *desc = ctx->sqDma.memDesc;
                s->core->GetDMA()->Unpin(&ctx->sqDma);
                if (desc) desc->release();
                ctx->sqPinned = false;
            }
            if (ctx->rqPinned) {
                IOMemoryDescriptor *desc = ctx->rqDma.memDesc;
                s->core->GetDMA()->Unpin(&ctx->rqDma);
                if (desc) desc->release();
                ctx->rqPinned = false;
            }
            if (ctx->sqWrid) IODelete(ctx->sqWrid, uint64_t, ctx->sqSize);
            if (ctx->rqWrid) IODelete(ctx->rqWrid, uint64_t, ctx->rqSize);
            if (ctx->sqOpcode) IODelete(ctx->sqOpcode, uint8_t, ctx->sqSize);
            if (ctx->sqSpan) IODelete(ctx->sqSpan, uint8_t, ctx->sqSize);
            if (s->core->GetUAR()) {
                if (ctx->clientBundle)
                    s->core->GetUAR()->FreeClientDbSlot(
                        ctx->clientBundle, ctx->dbRecordOffset);
                else s->core->GetUAR()->FreeDbSlot(ctx->dbRecordOffset);
            }
            memset(&s->table[slot], 0, sizeof(s->table[slot]));
            IOLockUnlock(s->qpLocks[slot]);
        }
        MLX_DBG("QP[%u] destroyed", qpn);
    }
    return kr;
}

kern_return_t
MlxQP::ResetQP(uint32_t qpn)
{
    if (!s) return kIOReturnBadArgument;
    MlxQPContext *ctx = LockQp(qpn);
    if (!ctx) { UnlockQp(ctx); return kIOReturnNotFound; }
    uint8_t in[16] = {}, out[16] = {};
    mlxSetBits(in, 0x00, 16, MLX_CMD_OP_2RST_QP);
    mlxSetBits(in, 0x48, 24, qpn);
    kern_return_t kr = s->core->Exec(MLX_CMD_OP_2RST_QP, in, sizeof(in),
                                      out, sizeof(out), 5000);
    if (kr == kIOReturnSuccess) {
        ctx->sqHead = ctx->sqTail = 0;
        ctx->rqHead = ctx->rqTail = 0;
        if (ctx->sqCpu) memset((void *)(uintptr_t)ctx->sqCpu, 0,
                               (uint64_t)ctx->sqSize * 64);
        if (ctx->rqCpu) memset((void *)(uintptr_t)ctx->rqCpu, 0,
                               (uint64_t)ctx->rqSize * 64);
        ctx->state = MLX_QP_STATE_RST;
    }
    UnlockQp(ctx);
    return kr;
}

kern_return_t
MlxQP::QueryQP(uint32_t qpn, void *outPtr)
{
    if (!s || !outPtr || !CtxForQpn(qpn)) return kIOReturnNotFound;
    uint8_t in[16] = {};
    uint8_t out[MLX_QP_MODIFY_IN_BYTES] = {};
    mlxSetBits(in, 0x00, 16, MLX_CMD_OP_QUERY_QP);
    mlxSetBits(in, 0x48, 24, qpn);
    kern_return_t kr = s->core->Exec(MLX_CMD_OP_QUERY_QP, in, sizeof(in),
                                      out, sizeof(out), 5000);
    if (kr != kIOReturnSuccess) return kr;
    const uint8_t *qpc = out + MLX_QPC_BIT_OFFSET / 8;
    struct mlx_query_qp_resp *resp =
        (struct mlx_query_qp_resp *)outPtr;
    memset(resp, 0, sizeof(*resp));
    resp->qpn = qpn;
    resp->state = (uint32_t)mlxGetBits(qpc, 0x00, 4);
    resp->pathMtu = (uint32_t)mlxGetBits(qpc, 0x40, 3);
    resp->destQpn = (uint32_t)mlxGetBits(qpc, 0xa8, 24);
    resp->sqPsn = (uint32_t)mlxGetBits(qpc, 0x3c8, 24);
    resp->rqPsn = (uint32_t)mlxGetBits(qpc, 0x4a8, 24);
    resp->sendCq = (uint32_t)mlxGetBits(qpc, 0x3e8, 24);
    resp->recvCq = (uint32_t)mlxGetBits(qpc, 0x4e8, 24);
    return kIOReturnSuccess;
}

static inline uint64_t
mlxQpExpandCounter(uint64_t producer, uint16_t counter)
{
    uint64_t value = (producer & ~0xffffULL) | counter;
    if (value > producer) value -= 0x10000ULL;
    return value;
}

kern_return_t
MlxQP::PostSend(const struct mlx_post_send_req *req)
{
    return PostSendBatch(req, 1);
}

kern_return_t
MlxQP::PostSendSge(const struct mlx_post_send_sge_req *req)
{
    if (!s || !req || !req->numSge || req->numSge > MLX_UC_MAX_SGE)
        return kIOReturnBadArgument;
    MlxQPContext *ctx = LockQp(req->qpn);
    if (!ctx || ctx->state != MLX_QP_STATE_RTS || !ctx->sqCpu || !ctx->dbRecord) {
        UnlockQp(ctx);
        return ctx ? kIOReturnBusy : kIOReturnNotFound;
    }
    if ((req->sendFlags & ~(MLX_UC_SEND_SIGNALED | MLX_UC_SEND_FENCE |
                            MLX_UC_SEND_SOLICITED)) ||
        (req->opcode != MLX_UC_WR_SEND &&
         req->opcode != MLX_UC_WR_RDMA_WRITE &&
         req->opcode != MLX_UC_WR_RDMA_READ &&
         req->opcode != MLX_UC_WR_SEND_IMM &&
         req->opcode != MLX_UC_WR_RDMA_WRITE_IMM &&
         req->opcode != MLX_UC_WR_LOCAL_INV) ||
        ((req->opcode == MLX_UC_WR_RDMA_WRITE || req->opcode == MLX_UC_WR_RDMA_READ ||
          req->opcode == MLX_UC_WR_RDMA_WRITE_IMM) &&
         (!req->remoteAddr || !req->rkey))) {
        UnlockQp(ctx); return kIOReturnBadArgument;
    }
    MlxRcSge sges[MLX_UC_MAX_SGE] = {};
    for (uint32_t i = 0; i < req->numSge; i++) {
        if (!req->sge[i].length || !s->roce->GetMR() ||
            !s->roce->GetMR()->ValidateRange(req->sge[i].lkey, ctx->pd,
                req->sge[i].addr, req->sge[i].length,
                req->opcode == MLX_UC_WR_RDMA_READ)) {
            UnlockQp(ctx); return kIOReturnBadArgument;
        }
        sges[i].addr = req->sge[i].addr;
        sges[i].length = req->sge[i].length;
        sges[i].lkey = req->sge[i].lkey;
    }
    uint8_t flat[128] = {};
    uint8_t hwOpcode = req->opcode == MLX_UC_WR_LOCAL_INV ? MLX_OPCODE_LOCAL_INVAL :
                       req->opcode == MLX_UC_WR_RDMA_WRITE ? MLX_OPCODE_RDMA_WRITE :
                       req->opcode == MLX_UC_WR_RDMA_READ ? MLX_OPCODE_RDMA_READ :
                       req->opcode == MLX_UC_WR_RDMA_WRITE_IMM ? MLX_OPCODE_RDMA_WRITE_IMM :
                       req->opcode == MLX_UC_WR_SEND_IMM ? MLX_OPCODE_SEND_IMM :
                                                             MLX_OPCODE_SEND;
    uint64_t head = ctx->sqHead;
    uint32_t ds = (req->opcode == MLX_UC_WR_SEND_IMM ||
                   req->opcode == MLX_UC_WR_RDMA_WRITE_IMM) ?
        mlxEncodeRcSendWqeImm(flat, sizeof(flat), ctx->qpNum, (uint16_t)head,
                              hwOpcode, sges, req->numSge, req->remoteAddr,
                              req->rkey, req->immData,
                              (req->sendFlags & MLX_UC_SEND_SIGNALED) != 0,
                              (req->sendFlags & MLX_UC_SEND_FENCE) != 0,
                              (req->sendFlags & MLX_UC_SEND_SOLICITED) != 0) :
        mlxEncodeRcSendWqe(flat, sizeof(flat), ctx->qpNum, (uint16_t)head,
                           hwOpcode, sges, req->numSge, req->remoteAddr,
                           req->rkey,
                           (req->sendFlags & MLX_UC_SEND_SIGNALED) != 0,
                           (req->sendFlags & MLX_UC_SEND_FENCE) != 0,
                           (req->sendFlags & MLX_UC_SEND_SOLICITED) != 0);
    uint32_t span = (ds * 16u + 63u) / 64u;
    if (!ds || !span || ctx->sqHead - ctx->sqTail + span > ctx->sqSize) {
        UnlockQp(ctx); return kIOReturnBusy;
    }
    for (uint32_t i = 0; i < span; i++) {
        uint32_t slot = (uint32_t)(head + i) & (ctx->sqSize - 1);
        memcpy((void *)(uintptr_t)(ctx->sqCpu + (uint64_t)slot * 64), flat + i * 64, 64);
    }
    uint32_t index = (uint32_t)head & (ctx->sqSize - 1);
    ctx->sqWrid[index] = req->wrId;
    ctx->sqOpcode[index] = (uint8_t)req->opcode;
    ctx->sqSpan[index] = (uint8_t)span;
    mlxMemoryBarrier();
    ctx->sqHead = head + span;
    ctx->dbRecord[1] = OSSwapHostToBigInt32((uint32_t)ctx->sqHead & 0xffff);
    mlxMemoryBarrier();
    uint64_t doorbell = 0;
    memcpy(&doorbell, flat, sizeof(doorbell));
    kern_return_t kr = s->core->GetUAR()->RingSendDoorbell(ctx->uarPage,
                                                            ctx->bfOffset, doorbell);
    if (kr != kIOReturnSuccess) {
        ctx->sqHead = head;
        ctx->dbRecord[1] = OSSwapHostToBigInt32((uint32_t)head & 0xffff);
    } else { ctx->sqPkts++; mlxQpBumpPosted(ctx, req->opcode); }
    UnlockQp(ctx);
    return kr;
}

kern_return_t
MlxQP::PostSendBatch(const struct mlx_post_send_req *req, uint32_t count)
{
    if (!s || !req || !count || count > MLX_UC_MAX_POST_BATCH)
        return kIOReturnBadArgument;
    MlxQPContext *ctx = LockQp(req->qpn);
    if (!ctx || ctx->state != MLX_QP_STATE_RTS || !ctx->sqCpu ||
        !ctx->dbRecord || ctx->sqHead - ctx->sqTail + count > ctx->sqSize) {
        UnlockQp(ctx);
        return ctx ? kIOReturnBusy : kIOReturnNotFound;
    }
    /* Validate the complete batch before publishing any producer index. */
    for (uint32_t i = 0; i < count; i++) {
        const struct mlx_post_send_req *wr = &req[i];
        if (wr->qpn != ctx->qpNum || (wr->sendFlags &
            ~(MLX_UC_SEND_SIGNALED | MLX_UC_SEND_FENCE | MLX_UC_SEND_SOLICITED)) ||
            wr->opcode > MLX_UC_WR_RDMA_READ || !wr->sge.length ||
            !s->roce->GetMR() ||
            !s->roce->GetMR()->ValidateRange(
                wr->sge.lkey, ctx->pd, wr->sge.addr, wr->sge.length,
                wr->opcode == MLX_UC_WR_RDMA_READ) ||
            ((wr->opcode == MLX_UC_WR_RDMA_WRITE ||
              wr->opcode == MLX_UC_WR_RDMA_READ) &&
             (!wr->remoteAddr || !wr->rkey))) {
            UnlockQp(ctx);
            return kIOReturnBadArgument;
        }
    }

    uint64_t head = ctx->sqHead;
    struct MlxWqeCtrlSeg *lastCtrl = NULL;
    for (uint32_t i = 0; i < count; i++) {
        const struct mlx_post_send_req *wr = &req[i];
        uint64_t producer = head + i;
        uint32_t idx = (uint32_t)producer & (ctx->sqSize - 1);
        volatile uint8_t *wqe = ctx->sqCpu + (uint64_t)idx * 64;
        uint8_t hwOpcode =
            wr->opcode == MLX_UC_WR_RDMA_WRITE ? MLX_OPCODE_RDMA_WRITE :
            wr->opcode == MLX_UC_WR_RDMA_READ  ? MLX_OPCODE_RDMA_READ :
                                                 MLX_OPCODE_SEND;
        if (!mlxEncodeRcSendWqe64Flags((void *)(uintptr_t)wqe, ctx->qpNum,
                                       (uint16_t)producer, hwOpcode,
                                       wr->sge.addr, wr->sge.length, wr->sge.lkey,
                                       wr->remoteAddr, wr->rkey,
                                       (wr->sendFlags & MLX_UC_SEND_SIGNALED) != 0,
                                       (wr->sendFlags & MLX_UC_SEND_FENCE) != 0,
                                       (wr->sendFlags & MLX_UC_SEND_SOLICITED) != 0)) {
            UnlockQp(ctx);
            return kIOReturnBadArgument;
        }
        lastCtrl = (struct MlxWqeCtrlSeg *)(uintptr_t)wqe;
        ctx->sqWrid[idx] = wr->wrId;
        ctx->sqOpcode[idx] = (uint8_t)wr->opcode;
        ctx->sqSpan[idx] = 1;
    }
    mlxMemoryBarrier();
    ctx->sqHead = head + count;
    ctx->dbRecord[1] = OSSwapHostToBigInt32((uint32_t)ctx->sqHead & 0xffff);
    mlxMemoryBarrier();
    uint64_t doorbell = 0;
    memcpy(&doorbell, (const void *)(uintptr_t)lastCtrl, sizeof(doorbell));
    kern_return_t kr = s->core->GetUAR()->RingSendDoorbell(
        ctx->uarPage, ctx->bfOffset, doorbell);
    if (kr != kIOReturnSuccess) {
        ctx->sqHead = head;
        ctx->dbRecord[1] = OSSwapHostToBigInt32((uint32_t)head & 0xffff);
    } else {
        ctx->sqPkts += count;
        for (uint32_t i = 0; i < count; i++)
            mlxQpBumpPosted(ctx, req[i].opcode);
    }
    UnlockQp(ctx);
    return kr;
}

kern_return_t
MlxQP::SyncFastPath(const struct mlx_post_send_req *req, uint32_t count)
{
    if (!s || !req || !count || count > MLX_UC_MAX_POST_BATCH)
        return kIOReturnBadArgument;
    MlxQPContext *ctx = LockQp(req->qpn);
    if (!ctx || ctx->state != MLX_QP_STATE_RTS || !ctx->sqCpu ||
        !ctx->dbRecord || ctx->sqHead - ctx->sqTail + count > ctx->sqSize) {
        UnlockQp(ctx);
        return ctx ? kIOReturnBusy : kIOReturnNotFound;
    }
    uint64_t head = ctx->sqHead;
    for (uint32_t i = 0; i < count; i++) {
        const struct mlx_post_send_req *wr = &req[i];
        if (wr->qpn != ctx->qpNum || (wr->sendFlags &
            ~(MLX_UC_SEND_SIGNALED | MLX_UC_SEND_FENCE | MLX_UC_SEND_SOLICITED)) ||
            wr->opcode > MLX_UC_WR_RDMA_READ || !wr->sge.length ||
            !s->roce->GetMR() || !s->roce->GetMR()->ValidateRange(
                wr->sge.lkey, ctx->pd, wr->sge.addr, wr->sge.length,
                wr->opcode == MLX_UC_WR_RDMA_READ) ||
            ((wr->opcode == MLX_UC_WR_RDMA_WRITE ||
              wr->opcode == MLX_UC_WR_RDMA_READ) &&
             (!wr->remoteAddr || !wr->rkey))) {
            UnlockQp(ctx);
            return kIOReturnBadArgument;
        }
        uint32_t idx = (uint32_t)(head + i) & (ctx->sqSize - 1);
        ctx->sqWrid[idx] = wr->wrId;
        ctx->sqOpcode[idx] = (uint8_t)wr->opcode;
        ctx->sqSpan[idx] = 1;
    }
    mlxMemoryBarrier();
    ctx->sqHead = head + count;
    ctx->dbRecord[1] = OSSwapHostToBigInt32((uint32_t)ctx->sqHead & 0xffff);
    mlxMemoryBarrier();
    ctx->sqPkts += count;
    for (uint32_t i = 0; i < count; i++)
        mlxQpBumpPosted(ctx, req[i].opcode);
    UnlockQp(ctx);
    /* Keep the direct path quiet; per-batch logging serializes the DriverKit
     * process under sustained traffic. Use the existing debug log stream when
     * diagnosing a specific batch issue. */
    return kIOReturnSuccess;
}

kern_return_t
MlxQP::SyncSendSge(const struct mlx_sync_send_sge_req *req)
{
    if (!s || !req || !req->numSge || req->numSge > MLX_UC_MAX_SGE)
        return kIOReturnBadArgument;
    MlxQPContext *ctx = LockQp(req->qpn);
    bool valid = ctx && ctx->state == MLX_QP_STATE_RTS && ctx->sqCpu &&
                 ctx->dbRecord && s->roce->GetMR() &&
                 req->opcode <= MLX_UC_WR_LOCAL_INV &&
                 !(req->sendFlags & ~(MLX_UC_SEND_SIGNALED | MLX_UC_SEND_FENCE |
                                      MLX_UC_SEND_SOLICITED));
    for (uint32_t i = 0; valid && i < req->numSge; i++)
        valid = req->sge[i].length && s->roce->GetMR()->ValidateRange(
            req->sge[i].lkey, ctx->pd, req->sge[i].addr, req->sge[i].length,
            req->opcode == MLX_UC_WR_RDMA_READ);
    if (!valid || req->opcode > MLX_UC_WR_RDMA_WRITE_IMM ||
        ((req->opcode == MLX_UC_WR_RDMA_WRITE || req->opcode == MLX_UC_WR_RDMA_READ ||
          req->opcode == MLX_UC_WR_RDMA_WRITE_IMM) &&
         (!req->remoteAddr || !req->rkey))) {
        UnlockQp(ctx); return kIOReturnBadArgument;
    }
    uint32_t idx = (uint32_t)ctx->sqHead & (ctx->sqSize - 1);
    uint32_t ds = 1 + ((req->opcode == MLX_UC_WR_RDMA_WRITE ||
                        req->opcode == MLX_UC_WR_RDMA_READ ||
                        req->opcode == MLX_UC_WR_RDMA_WRITE_IMM) ? 1 : 0) +
                  req->numSge;
    uint32_t span = (ds + 3u) / 4u;
    if (ctx->sqHead - ctx->sqTail + span > ctx->sqSize) {
        UnlockQp(ctx); return kIOReturnBusy;
    }
    ctx->sqWrid[idx] = req->wrId;
    ctx->sqOpcode[idx] = (uint8_t)req->opcode;
    ctx->sqSpan[idx] = (uint8_t)span;
    ctx->sqHead += span;
    ctx->dbRecord[1] = OSSwapHostToBigInt32((uint32_t)ctx->sqHead & 0xffff);
    mlxMemoryBarrier();
    ctx->sqPkts++;
    mlxQpBumpPosted(ctx, req->opcode);
    UnlockQp(ctx);
    return kIOReturnSuccess;
}

kern_return_t
MlxQP::PostLocalInv(const struct mlx_post_local_inv_req *req)
{
    if (!s || !req || !req->qpn || !req->invalidateRkey ||
        (req->sendFlags & ~MLX_UC_SEND_SIGNALED)) return kIOReturnBadArgument;
    MlxQPContext *ctx = LockQp(req->qpn);
    MlxMRContext *mr = s->roce->GetMR() ?
        s->roce->GetMR()->LookupByRkey(req->invalidateRkey) : NULL;
    if (!ctx || !mr || ctx->state != MLX_QP_STATE_RTS || !ctx->sqCpu ||
        !ctx->dbRecord || ctx->sqHead - ctx->sqTail + 1 > ctx->sqSize) {
        UnlockQp(ctx); return kIOReturnBusy;
    }
    uint8_t wqe[128] = {};
    if (!mlxEncodeLocalInvWqe(wqe, sizeof(wqe), ctx->qpNum,
                              (uint16_t)ctx->sqHead, req->invalidateRkey,
                              (req->sendFlags & MLX_UC_SEND_SIGNALED) != 0)) {
        UnlockQp(ctx); return kIOReturnBadArgument;
    }
    uint32_t idx = (uint32_t)ctx->sqHead & (ctx->sqSize - 1);
    for (uint32_t i = 0; i < 2; i++)
        memcpy((void *)(uintptr_t)(ctx->sqCpu + (uint64_t)(idx + i) * 64),
               wqe + i * 64, 64);
    ctx->sqWrid[idx] = req->wrId;
    ctx->sqOpcode[idx] = MLX_UC_WR_LOCAL_INV;
    ctx->sqSpan[idx] = 2;
    ctx->sqHead += 2;
    ctx->dbRecord[1] = OSSwapHostToBigInt32((uint32_t)ctx->sqHead & 0xffff);
    mlxMemoryBarrier();
    uint64_t doorbell = 0; memcpy(&doorbell, wqe, sizeof(doorbell));
    kern_return_t kr = s->core->GetUAR()->RingSendDoorbell(ctx->uarPage,
                                                            ctx->bfOffset, doorbell);
    if (kr == kIOReturnSuccess) { ctx->sqPkts++; ctx->postedLocalInv++; }
    UnlockQp(ctx);
    return kr;
}

kern_return_t
MlxQP::PostSendInline(const struct mlx_post_send_inline_req *req)
{
    if (!s || !req || !req->inlineLen ||
        req->inlineLen > MLX_UC_MAX_INLINE_DATA ||
        (req->opcode != MLX_UC_WR_SEND && req->opcode != MLX_UC_WR_SEND_IMM) ||
        (req->sendFlags & ~(MLX_UC_SEND_SIGNALED | MLX_UC_SEND_FENCE |
                            MLX_UC_SEND_SOLICITED | MLX_UC_SEND_INLINE)))
        return kIOReturnBadArgument;
    MlxQPContext *ctx = LockQp(req->qpn);
    if (!ctx || ctx->state != MLX_QP_STATE_RTS || !ctx->sqCpu || !ctx->dbRecord) {
        UnlockQp(ctx);
        return ctx ? kIOReturnBusy : kIOReturnNotFound;
    }
    uint8_t hwOpcode = req->opcode == MLX_UC_WR_SEND_IMM ?
                       MLX_OPCODE_SEND_IMM : MLX_OPCODE_SEND;
    uint64_t head = ctx->sqHead;
    uint8_t flat[MLX_UC_MAX_INLINE_DATA + 64] = {};
    uint32_t ds = mlxEncodeRcInlineSendWqe(
        flat, sizeof(flat), ctx->qpNum, (uint16_t)head, hwOpcode,
        req->inlineData, req->inlineLen, req->immData,
        (req->sendFlags & MLX_UC_SEND_SIGNALED) != 0,
        (req->sendFlags & MLX_UC_SEND_FENCE) != 0,
        (req->sendFlags & MLX_UC_SEND_SOLICITED) != 0);
    uint32_t span = (ds * 16u + 63u) / 64u;
    if (!ds || !span || ctx->sqHead - ctx->sqTail + span > ctx->sqSize) {
        UnlockQp(ctx); return kIOReturnBusy;
    }
    for (uint32_t i = 0; i < span; i++) {
        uint32_t slot = (uint32_t)(head + i) & (ctx->sqSize - 1);
        memcpy((void *)(uintptr_t)(ctx->sqCpu + (uint64_t)slot * 64),
               flat + i * 64, 64);
    }
    uint32_t index = (uint32_t)head & (ctx->sqSize - 1);
    ctx->sqWrid[index] = req->wrId;
    ctx->sqOpcode[index] = (uint8_t)req->opcode;
    ctx->sqSpan[index] = (uint8_t)span;
    mlxMemoryBarrier();
    ctx->sqHead = head + span;
    ctx->dbRecord[1] = OSSwapHostToBigInt32((uint32_t)ctx->sqHead & 0xffff);
    mlxMemoryBarrier();
    uint64_t doorbell = 0;
    memcpy(&doorbell, flat, sizeof(doorbell));
    kern_return_t kr = s->core->GetUAR()->RingSendDoorbell(ctx->uarPage,
                                                            ctx->bfOffset, doorbell);
    if (kr != kIOReturnSuccess) {
        ctx->sqHead = head;
        ctx->dbRecord[1] = OSSwapHostToBigInt32((uint32_t)head & 0xffff);
    } else { ctx->sqPkts++; mlxQpBumpPosted(ctx, req->opcode); }
    UnlockQp(ctx);
    return kr;
}

kern_return_t
MlxQP::PostSendAtomic(const struct mlx_post_send_atomic_req *req)
{
    if (!s || !req ||
        (req->opcode != MLX_UC_WR_ATOMIC_CS && req->opcode != MLX_UC_WR_ATOMIC_FA) ||
        (req->sendFlags & ~MLX_UC_SEND_SIGNALED) ||
        !req->remoteAddr || !req->rkey || (req->remoteAddr & 7) ||
        !req->resultAddr || !req->resultLkey || (req->resultAddr & 7))
        return kIOReturnBadArgument;
    MlxQPContext *ctx = LockQp(req->qpn);
    if (!ctx || ctx->state != MLX_QP_STATE_RTS || !ctx->sqCpu || !ctx->dbRecord) {
        UnlockQp(ctx);
        return ctx ? kIOReturnBusy : kIOReturnNotFound;
    }
    if (!s->roce->GetMR() ||
        !s->roce->GetMR()->ValidateRange(req->resultLkey, ctx->pd,
                                         req->resultAddr, 8, true)) {
        UnlockQp(ctx);
        return kIOReturnBadArgument;
    }
    uint8_t hwOpcode = req->opcode == MLX_UC_WR_ATOMIC_CS ?
                       MLX_OPCODE_ATOMIC_CS : MLX_OPCODE_ATOMIC_FA;
    uint64_t head = ctx->sqHead;
    uint8_t flat[64] = {};
    uint32_t ds = mlxEncodeRcAtomicWqe(
        flat, sizeof(flat), ctx->qpNum, (uint16_t)head, hwOpcode,
        req->remoteAddr, req->rkey, req->compare, req->swapAdd,
        req->resultAddr, req->resultLkey,
        (req->sendFlags & MLX_UC_SEND_SIGNALED) != 0);
    if (!ds || ctx->sqHead - ctx->sqTail + 1 > ctx->sqSize) {
        UnlockQp(ctx); return kIOReturnBusy;
    }
    uint32_t slot = (uint32_t)head & (ctx->sqSize - 1);
    memcpy((void *)(uintptr_t)(ctx->sqCpu + (uint64_t)slot * 64), flat, 64);
    ctx->sqWrid[slot] = req->wrId;
    ctx->sqOpcode[slot] = (uint8_t)req->opcode;
    ctx->sqSpan[slot] = 1;
    mlxMemoryBarrier();
    ctx->sqHead = head + 1;
    ctx->dbRecord[1] = OSSwapHostToBigInt32((uint32_t)ctx->sqHead & 0xffff);
    mlxMemoryBarrier();
    uint64_t doorbell = 0;
    memcpy(&doorbell, flat, sizeof(doorbell));
    kern_return_t kr = s->core->GetUAR()->RingSendDoorbell(ctx->uarPage,
                                                            ctx->bfOffset, doorbell);
    if (kr != kIOReturnSuccess) {
        ctx->sqHead = head;
        ctx->dbRecord[1] = OSSwapHostToBigInt32((uint32_t)head & 0xffff);
    } else { ctx->sqPkts++; mlxQpBumpPosted(ctx, req->opcode); }
    UnlockQp(ctx);
    return kr;
}

kern_return_t
MlxQP::SyncRecvSge(const struct mlx_sync_recv_sge_req *req)
{
    if (!s || !req || !req->numSge || req->numSge > MLX_UC_MAX_SGE)
        return kIOReturnBadArgument;
    MlxQPContext *ctx = LockQp(req->qpn);
    bool valid = ctx && ctx->state >= MLX_QP_STATE_INIT &&
                 ctx->state <= MLX_QP_STATE_RTS && ctx->rqCpu &&
                 ctx->dbRecord && s->roce->GetMR() &&
                 ctx->rqHead - ctx->rqTail + 1 <= ctx->rqSize;
    for (uint32_t i = 0; valid && i < req->numSge; i++)
        valid = req->sge[i].length && s->roce->GetMR()->ValidateRange(
            req->sge[i].lkey, ctx->pd, req->sge[i].addr, req->sge[i].length, true);
    if (!valid) { UnlockQp(ctx); return kIOReturnBadArgument; }
    uint32_t idx = (uint32_t)ctx->rqHead & (ctx->rqSize - 1);
    ctx->rqWrid[idx] = req->wrId;
    ctx->rqHead++;
    ctx->dbRecord[0] = OSSwapHostToBigInt32((uint32_t)ctx->rqHead & 0xffff);
    mlxMemoryBarrier();
    ctx->rqPkts++;
    UnlockQp(ctx);
    return kIOReturnSuccess;
}

kern_return_t
MlxQP::SyncRecvFastPath(const struct mlx_post_recv_req *req, uint32_t count)
{
    if (!s || !req || !count || count > MLX_UC_MAX_POST_BATCH)
        return kIOReturnBadArgument;
    MlxQPContext *ctx = LockQp(req->qpn);
    if (!ctx || ctx->state < MLX_QP_STATE_INIT ||
        ctx->state > MLX_QP_STATE_RTS || !ctx->rqCpu || !ctx->dbRecord ||
        ctx->rqHead - ctx->rqTail + count > ctx->rqSize) {
        UnlockQp(ctx);
        return ctx ? kIOReturnBusy : kIOReturnNotFound;
    }
    uint64_t head = ctx->rqHead;
    for (uint32_t i = 0; i < count; i++) {
        if (req[i].qpn != ctx->qpNum || !req[i].sge.length ||
            !s->roce->GetMR() || !s->roce->GetMR()->ValidateRange(
                req[i].sge.lkey, ctx->pd, req[i].sge.addr,
                req[i].sge.length, true)) {
            UnlockQp(ctx);
            return kIOReturnBadArgument;
        }
        uint32_t idx = (uint32_t)(head + i) & (ctx->rqSize - 1);
        ctx->rqWrid[idx] = req[i].wrId;
    }
    ctx->rqHead = head + count;
    ctx->rqPkts += count;
    UnlockQp(ctx);
    return kIOReturnSuccess;
}

kern_return_t
MlxQP::PostRecv(const struct mlx_post_recv_req *req)
{
    return PostRecvBatch(req, 1);
}

kern_return_t
MlxQP::PostRecvSge(const struct mlx_post_recv_sge_req *req)
{
    if (!s || !req || !req->numSge || req->numSge > MLX_UC_MAX_SGE)
        return kIOReturnBadArgument;
    MlxQPContext *ctx = LockQp(req->qpn);
    if (!ctx || ctx->state < MLX_QP_STATE_INIT || ctx->state > MLX_QP_STATE_RTS ||
        !ctx->rqCpu || !ctx->dbRecord || ctx->rqHead - ctx->rqTail >= ctx->rqSize) {
        UnlockQp(ctx); return ctx ? kIOReturnBusy : kIOReturnNotFound;
    }
    uint32_t slot = (uint32_t)ctx->rqHead & (ctx->rqSize - 1);
    uint8_t *wqe = (uint8_t *)(uintptr_t)(ctx->rqCpu + (uint64_t)slot * 64);
    memset(wqe, 0, 64);
    for (uint32_t i = 0; i < req->numSge; i++) {
        if (!req->sge[i].length || !s->roce->GetMR() ||
            !s->roce->GetMR()->ValidateRange(req->sge[i].lkey, ctx->pd,
                req->sge[i].addr, req->sge[i].length, true)) {
            UnlockQp(ctx); return kIOReturnBadArgument;
        }
        MlxWqeDataSeg *data = (MlxWqeDataSeg *)(wqe + i * sizeof(MlxWqeDataSeg));
        data->byte_count = MLX_BE32(req->sge[i].length);
        data->lkey = MLX_BE32(req->sge[i].lkey);
        data->addr = MLX_BE64(req->sge[i].addr);
    }
    ctx->rqWrid[slot] = req->wrId;
    mlxMemoryBarrier();
    ctx->rqHead++;
    ctx->dbRecord[0] = OSSwapHostToBigInt32((uint32_t)ctx->rqHead & 0xffff);
    mlxMemoryBarrier();
    ctx->rqPkts++;
    UnlockQp(ctx);
    return kIOReturnSuccess;
}

kern_return_t
MlxQP::PostRecvBatch(const struct mlx_post_recv_req *req, uint32_t count)
{
    if (!s || !req || !count || count > MLX_UC_MAX_POST_BATCH)
        return kIOReturnBadArgument;
    MlxQPContext *ctx = LockQp(req->qpn);
    if (!ctx || ctx->state < MLX_QP_STATE_INIT ||
        ctx->state > MLX_QP_STATE_RTS || !ctx->rqCpu || !ctx->dbRecord ||
        ctx->rqHead - ctx->rqTail + count > ctx->rqSize) {
        UnlockQp(ctx);
        return ctx ? kIOReturnBusy : kIOReturnNotFound;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (req[i].qpn != ctx->qpNum || !s->roce->GetMR() ||
            !s->roce->GetMR()->ValidateRange(
                req[i].sge.lkey, ctx->pd, req[i].sge.addr,
                req[i].sge.length, true)) {
            UnlockQp(ctx);
            return kIOReturnBadArgument;
        }
    }
    uint64_t head = ctx->rqHead;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t producer = head + i;
        uint32_t idx = (uint32_t)producer & (ctx->rqSize - 1);
        volatile uint8_t *wqe = ctx->rqCpu + (uint64_t)idx * 64;
        if (!mlxEncodeRecvWqe64((void *)(uintptr_t)wqe, req[i].sge.addr,
                                req[i].sge.length, req[i].sge.lkey)) {
            UnlockQp(ctx);
            return kIOReturnBadArgument;
        }
        ctx->rqWrid[idx] = req[i].wrId;
    }
    mlxMemoryBarrier();
    ctx->rqHead = head + count;
    ctx->dbRecord[0] = OSSwapHostToBigInt32((uint32_t)ctx->rqHead & 0xffff);
    mlxMemoryBarrier();
    ctx->rqPkts += count;
    UnlockQp(ctx);
    return kIOReturnSuccess;
}

kern_return_t
MlxQP::PostBindMW(uint32_t qpn, uint32_t mwKey, uint32_t origRkey,
                  uint32_t bindKey, uint32_t mrLkey, uint32_t accessFlags, uint64_t addr,
                  uint64_t length, uint64_t wrId)
{
    if (!s || !qpn || !mwKey || !bindKey || !mrLkey || !length) return kIOReturnBadArgument;
    MlxMRContext *mw = s->roce->GetMR()->Lookup(mwKey);
    if (!mw || !mw->isWindow) return kIOReturnNotFound;
    MlxKlmEntry klm = { addr, length, mrLkey };
    MlxQPContext *ctx = LockQp(qpn);
    if (!ctx || ctx->state != MLX_QP_STATE_RTS || ctx->sqHead - ctx->sqTail) {
        UnlockQp(ctx); return kIOReturnBusy;
    }
    uint8_t flat[MLX_UMR_WQE_FIXED_BYTES + MLX_UMR_KLM_ALIGN_BYTES] = {};
    uint32_t ds = mlxEncodeUmrKlmWqe(flat, sizeof(flat), qpn, (uint16_t)ctx->sqHead,
        bindKey, accessFlags, addr, length, &klm, 1, true);
    if (!ds) { UnlockQp(ctx); return kIOReturnBadArgument; }
    /* rdma-core's set_bind_wr uses the old rkey in ctrl.imm and binds a
     * type-2 mkey to this QP. The generic KLM encoder is also used by
     * indirect-MR activation, so apply the type-2-only fields here. */
    struct MlxWqeCtrlSeg *bindCtrl = (struct MlxWqeCtrlSeg *)flat;
    bindCtrl->imm = MLX_BE32(origRkey);
    struct MlxWqeUmrCtrlSeg *umr = (struct MlxWqeUmrCtrlSeg *)(flat + 16);
    umr->flags |= (1u << 5) | (1u << 3); /* CHECK_FREE | CHECK_QPN */
    umr->mkeyMask = MLX_BE64(MLX_UMR_MASK_FREE | MLX_UMR_MASK_MKEY |
                              (1ull << 14) | MLX_UMR_MASK_LEN |
                              MLX_UMR_MASK_START_ADDR |
                              MLX_UMR_MASK_ACCESS_LOCAL_WRITE |
                              MLX_UMR_MASK_ACCESS_REMOTE_READ |
                              MLX_UMR_MASK_ACCESS_REMOTE_WRITE |
                              MLX_UMR_MASK_ACCESS_ATOMIC);
    struct MlxWqeMkeyContextSeg *bindMkey =
        (struct MlxWqeMkeyContextSeg *)(flat + 16 + 48);
    bindMkey->qpnMkey = MLX_BE32((qpn << 8) | (bindKey & 0xffu));
    uint32_t span = (ds * 16 + 63) / 64, idx = (uint32_t)ctx->sqHead & (ctx->sqSize - 1);
    if (span > ctx->sqSize) { UnlockQp(ctx); return kIOReturnNoSpace; }
    MLX_DBG("BIND_MW WQE qpn=%u mw=%u orig=0x%x new=0x%x mr_lkey=0x%x addr=0x%llx len=%llu ds=%u span=%u flags=0x%x mask=0x%llx",
            qpn, mwKey, origRkey, bindKey, mrLkey, (unsigned long long)addr,
            (unsigned long long)length, ds, span, umr->flags,
            (unsigned long long)MLX_BE64(umr->mkeyMask));
    for (uint32_t i = 0; i < span; i++) memcpy((void *)(uintptr_t)(ctx->sqCpu + (uint64_t)((idx+i)&(ctx->sqSize-1))*64), flat+i*64, 64);
    if (span > UINT8_MAX) { UnlockQp(ctx); return kIOReturnNoSpace; }
    ctx->sqWrid[idx] = wrId; ctx->sqOpcode[idx] = MLX_UC_WR_UMR_KLM; ctx->sqSpan[idx] = (uint8_t)span;
    ctx->sqHead += span; ctx->dbRecord[1] = OSSwapHostToBigInt32((uint32_t)ctx->sqHead & 0xffff); mlxMemoryBarrier();
    uint64_t db = 0; memcpy(&db, flat, sizeof(db));
    kern_return_t kr = s->core->GetUAR()->RingSendDoorbell(ctx->uarPage, ctx->bfOffset, db);
    if (kr != kIOReturnSuccess) ctx->sqHead -= span;
    else ctx->postedBindMw++;
    UnlockQp(ctx); return kr;
}

kern_return_t
MlxQP::PostUmrKlm(uint32_t qpn, uint32_t mrHandle, const uint32_t *childHandles,
                  uint32_t childCount, uint64_t wrId)
{
    if (!s || !childHandles || !childCount ||
        childCount > MLX_UC_MAX_INDIRECT_CHILDREN)
        return kIOReturnBadArgument;

    MlxMR *mr = s->roce->GetMR();
    if (!mr) return kIOReturnNotReady;
    MlxMRContext *mkeyCtx = mr->Lookup(mrHandle);
    if (!mkeyCtx) return kIOReturnNotFound;

    struct MlxKlmEntry klms[MLX_UC_MAX_INDIRECT_CHILDREN];
    for (uint32_t i = 0; i < childCount; i++) {
        MlxMRContext *child = mr->Lookup(childHandles[i]);
        if (!child) return kIOReturnNotFound;
        klms[i].address = child->startAddr;
        klms[i].byteCount = child->length;
        klms[i].mkey = child->lkey;
    }

    MlxQPContext *ctx = LockQp(qpn);
    if (!ctx || ctx->state != MLX_QP_STATE_RTS || !ctx->sqCpu ||
        !ctx->dbRecord) {
        UnlockQp(ctx);
        return ctx ? kIOReturnNotReady : kIOReturnNotFound;
    }
    /* Keep this operation simple: require the SQ idle. A UMR activation is
     * a one-time step right after RegMRIndirect/QP bring-up, before any
     * other traffic — this avoids interleaving multi-WQEBB span accounting
     * with concurrently in-flight single-WQEBB WRs. */
    if (ctx->sqHead != ctx->sqTail) {
        UnlockQp(ctx);
        return kIOReturnBusy;
    }

    uint8_t flat[MLX_UMR_WQE_FIXED_BYTES +
                 MLX_UC_MAX_INDIRECT_CHILDREN * MLX_KLM_ENTRY_BYTES];
    uint32_t ds = mlxEncodeUmrKlmWqe(flat, sizeof(flat), ctx->qpNum,
                                     (uint16_t)ctx->sqHead, mkeyCtx->lkey,
                                     mkeyCtx->accessFlags, mkeyCtx->startAddr,
                                     mkeyCtx->length, klms, childCount, true);
    if (!ds) { UnlockQp(ctx); return kIOReturnBadArgument; }
    uint32_t totalBytes = ds * 16;
    uint32_t wqebbSpan = (totalBytes + 63) / 64;
    if (wqebbSpan > ctx->sqSize) {
        UnlockQp(ctx);
        return kIOReturnNoSpace;
    }

    uint64_t head = ctx->sqHead;
    uint32_t startIdx = (uint32_t)head & (ctx->sqSize - 1);
    uint32_t written = 0;
    uint32_t slot = startIdx;
    while (written < totalBytes) {
        uint32_t chunk = totalBytes - written;
        if (chunk > 64) chunk = 64;
        memcpy((void *)(uintptr_t)(ctx->sqCpu + (uint64_t)slot * 64),
               flat + written, chunk);
        written += chunk;
        slot = (slot + 1) & (ctx->sqSize - 1);
    }
    struct MlxWqeCtrlSeg *ctrl = (struct MlxWqeCtrlSeg *)(uintptr_t)
        (ctx->sqCpu + (uint64_t)startIdx * 64);
    ctx->sqWrid[startIdx] = wrId;
    ctx->sqOpcode[startIdx] = MLX_UC_WR_UMR_KLM;
    ctx->sqSpan[startIdx] = (uint8_t)wqebbSpan;

    mlxMemoryBarrier();
    ctx->sqHead = head + wqebbSpan;
    ctx->dbRecord[1] = OSSwapHostToBigInt32((uint32_t)ctx->sqHead & 0xffff);
    mlxMemoryBarrier();
    uint64_t doorbell = 0;
    memcpy(&doorbell, (const void *)(uintptr_t)ctrl, sizeof(doorbell));
    kern_return_t kr = s->core->GetUAR()->RingSendDoorbell(
        ctx->uarPage, ctx->bfOffset, doorbell);
    if (kr != kIOReturnSuccess) {
        ctx->sqHead = head;
        ctx->dbRecord[1] = OSSwapHostToBigInt32((uint32_t)head & 0xffff);
    } else {
        ctx->sqPkts++;
        ctx->postedUmr++;
    }
    UnlockQp(ctx);
    return kr;
}

/*
 * On an error CQE, the hardware overlays a completely different 64-byte
 * layout (struct mlx5_err_cqe in Linux's include/linux/mlx5/device.h) —
 * MlxCqe64's named fields don't apply. syndrome/vendor_err_synd sit at
 * fixed byte offsets 55/54 regardless of CQE type (confirmed against the
 * actual upstream Linux header, not assumed). Numeric syndrome values are
 * from rdma-core's own providers/mlx5/mlx5dv.h (a public, stable header) —
 * this driver had never decoded them before (notes/42), just reported
 * MLX_UC_WC_GENERAL for every error opcode with no detail.
 */
static uint32_t
MlxSyndromeToWcStatus(uint8_t syndrome)
{
    switch (syndrome) {
    case 0x01: return MLX_UC_WC_LOC_LEN;     /* LOCAL_LENGTH_ERR */
    case 0x02: return MLX_UC_WC_LOC_QP_OP;   /* LOCAL_QP_OP_ERR */
    case 0x05: return MLX_UC_WC_WR_FLUSH;    /* WR_FLUSH_ERR */
    case 0x13: return MLX_UC_WC_REM_ACCESS;  /* REMOTE_ACCESS_ERR */
    case 0x15: return MLX_UC_WC_RETRY_EXC;   /* TRANSPORT_RETRY_EXC_ERR */
    case 0x16: return MLX_UC_WC_RNR_RETRY;   /* RNR_RETRY_EXC_ERR */
    /* 0x04 LOCAL_PROT_ERR, 0x06 MW_BIND_ERR, 0x10 BAD_RESP_ERR,
     * 0x11 LOCAL_ACCESS_ERR, 0x12 REMOTE_INVAL_REQ_ERR,
     * 0x14 REMOTE_OP_ERR, 0x22 REMOTE_ABORTED_ERR: this driver's
     * client-facing MLX_UC_WC_* enum has no matching slot for these yet —
     * still surfaced via wc->vendorError (the raw syndrome/vendor_err_synd
     * pair) and the MLX_LOG line below, just not as a distinct status. */
    default: return MLX_UC_WC_GENERAL;
    }
}

bool
MlxQP::CompleteCQE(uint32_t cqHandle, const struct MlxCqe64 *cqe,
                   struct mlx_work_completion *wc)
{
    if (!s || !cqe || !wc) return false;
    uint8_t cqeOpcode = MLX_CQE_GET_OPCODE(cqe);
    const bool errorCqe = cqeOpcode == MLX_CQE_REQ_ERR ||
                          cqeOpcode == MLX_CQE_RESP_ERR;
    const uint8_t *rawCqe = reinterpret_cast<const uint8_t *>(cqe);
    uint32_t errorQpnBe = 0;
    uint16_t errorCounterBe = 0;
    if (errorCqe) {
        memcpy(&errorQpnBe, rawCqe + 56, sizeof(errorQpnBe));
        memcpy(&errorCounterBe, rawCqe + 60, sizeof(errorCounterBe));
    }
    uint32_t hintedQpn = errorCqe ?
        (OSSwapBigToHostInt32(errorQpnBe) & 0xffffff) :
        (OSSwapBigToHostInt32(cqe->sop_drop_qpn) & 0xffffff);
    uint16_t counter = errorCqe ?
        OSSwapBigToHostInt16(errorCounterBe) :
        OSSwapBigToHostInt16(cqe->wqe_counter);
    bool send = cqeOpcode == MLX_CQE_REQ || cqeOpcode == MLX_CQE_REQ_ERR;
    MlxQPContext *ctx = LockQp(hintedQpn);
    if (!ctx || (send ? ctx->sendCq : ctx->recvCq) != cqHandle) {
        if (ctx) UnlockQp(ctx);
        /* hinted QPN didn't resolve to this CQ (error CQE, stale QPN, or a
         * shared CQ). Fall back to a scan of QPs bound to this CQ. */
        ctx = LockQpByCq(cqHandle, send);
        if (!ctx) return false;
    }
    memset(wc, 0, sizeof(*wc));
    wc->qpNum = ctx->qpNum;
    wc->wqeCounter = counter;
    wc->byteLen = errorCqe ? 0 : OSSwapBigToHostInt32(cqe->byte_cnt);
    if (errorCqe) {
        const uint8_t *raw = reinterpret_cast<const uint8_t *>(cqe);
        uint8_t vendorErrSynd = raw[54];
        uint8_t syndrome = raw[55];
        wc->status = MlxSyndromeToWcStatus(syndrome);
        if (syndrome == 0x16)
            MLX_LOG("QP[%u] RNR retry exhausted (rnr_retry=0 or finite)", ctx->qpNum);
        wc->vendorError = ((uint32_t)vendorErrSynd << 8) | syndrome;
        MLX_LOG("QP[%u] error CQE %s: cqe_opcode=%u syndrome=0x%02x "
                "vendor_err_synd=0x%02x -> status=%u",
                ctx->qpNum, send ? "REQ" : "RESP", cqeOpcode, syndrome,
                vendorErrSynd, wc->status);
        /* Error CQEs make outstanding WQEs unreliable. Keep the software
         * state aligned with the terminal hardware state and force callers
         * through RESET before they reuse the QP. */
        ctx->state = MLX_QP_STATE_ERR;
        ctx->cqeError++;
        if (syndrome == 0x15) ctx->cqeRetryExc++;
        else if (syndrome == 0x16) ctx->cqeRnrRetry++;
        s->roce->QueueAsyncEvent(MLX_EVENT_QP_FATAL,
                                 MLX_ASYNC_ELEMENT_QP, ctx->qpNum);
    }
    if (send) {
        uint32_t idx = counter & (ctx->sqSize - 1);
        wc->wrId = ctx->sqWrid[idx];
        uint8_t op = ctx->sqOpcode[idx];
        wc->opcode = op == MLX_UC_WR_RDMA_WRITE ? MLX_UC_WC_RDMA_WRITE :
                     op == MLX_UC_WR_RDMA_READ ? MLX_UC_WC_RDMA_READ :
                     op == MLX_UC_WR_UMR_KLM ? MLX_UC_WC_UMR_KLM :
                     op == MLX_UC_WR_ATOMIC_CS ? MLX_UC_WC_COMP_SWAP :
                     op == MLX_UC_WR_ATOMIC_FA ? MLX_UC_WC_FETCH_ADD :
                                                MLX_UC_WC_SEND;
        uint8_t span = ctx->sqSpan[idx] ? ctx->sqSpan[idx] : 1;
        uint64_t done = mlxQpExpandCounter(ctx->sqHead, counter) + span;
        if (done > ctx->sqTail) ctx->sqTail = done;
        if (!errorCqe) {
            switch (op) {
            case MLX_UC_WR_SEND:
            case MLX_UC_WR_SEND_IMM:        ctx->completedSend++; break;
            case MLX_UC_WR_RDMA_READ:       ctx->completedRead++; break;
            case MLX_UC_WR_RDMA_WRITE:
            case MLX_UC_WR_RDMA_WRITE_IMM:  ctx->completedWrite++; break;
            case MLX_UC_WR_UMR_KLM:         ctx->completedUmr++; break;
            case MLX_UC_WR_LOCAL_INV:       ctx->completedLocalInv++; break;
            case MLX_UC_WR_ATOMIC_CS:
            case MLX_UC_WR_ATOMIC_FA: {
                /* mlx5 writes the pre-operation value to the local result
                 * data segment, not to the normal CQE byte-count/timestamp
                 * fields. The WQE stores that SGE at offset 48. */
                const volatile uint8_t *wqe = ctx->sqCpu + (uint64_t)idx * 64;
                uint64_t resultAddrBe = 0;
                memcpy(&resultAddrBe, (const void *)(uintptr_t)(wqe + 48 + 8),
                       sizeof(resultAddrBe));
                uint64_t resultAddr = MLX_BE64(resultAddrBe);
                uint32_t resultLkeyBe = 0;
                memcpy(&resultLkeyBe, (const void *)(uintptr_t)(wqe + 48 + 4),
                       sizeof(resultLkeyBe));
                uint32_t resultLkey = MLX_BE32(resultLkeyBe);
                MlxMRContext *resultMr = s->roce->GetMR() ?
                    s->roce->GetMR()->LookupByLkey(resultLkey) : NULL;
                if (!resultMr || resultMr->pd != ctx->pd || !resultAddr ||
                    (resultAddr & 7) || resultAddr < resultMr->startAddr ||
                    resultAddr - resultMr->startAddr > resultMr->length - 8 ||
                    !resultMr->fMemDesc) {
                    wc->status = MLX_UC_WC_LOC_QP_OP;
                    wc->vendorError = 0xffffffffu;
                    break;
                }
                uint64_t mappedAddr = 0, mappedLen = 0;
                if (resultMr->fMemDesc->Map(0, 0, 0, 0, &mappedAddr,
                                             &mappedLen) != kIOReturnSuccess ||
                    mappedLen < resultMr->length ||
                    resultAddr - resultMr->startAddr > mappedLen - 8) {
                    wc->status = MLX_UC_WC_LOC_QP_OP;
                    wc->vendorError = 0xffffffffu;
                    break;
                }
                uint64_t result = 0;
                memcpy(&result,
                       (const void *)(uintptr_t)(mappedAddr +
                           resultAddr - resultMr->startAddr),
                       sizeof(result));
                wc->wcFlags |= MLX_UC_WC_WITH_ATOMIC;
                /* atomic_req_8B_endianness_mode=1 makes the local result
                 * host-order, matching the verbs atomic_result contract. */
                wc->atomicResult = result;
                ctx->completedRead++;
                break;
            }
            default: break;
            }
        }
    } else {
        uint32_t idx = counter & (ctx->rqSize - 1);
        wc->wrId = ctx->rqWrid[idx];
        wc->opcode = MLX_UC_WC_RECV;
        if (cqeOpcode == MLX_CQE_RESP_WR_IMM ||
            cqeOpcode == MLX_CQE_RESP_SEND_IMM) {
            wc->immData = cqe->imm_inval_pkey;
            wc->wcFlags |= MLX_UC_WC_WITH_IMM;
        }
        uint64_t done = mlxQpExpandCounter(ctx->rqHead, counter) + 1;
        if (done > ctx->rqTail) ctx->rqTail = done;
        if (!errorCqe) ctx->completedRecv++;
    }
    UnlockQp(ctx);
    return true;
}

void
MlxQP::HandleQPEvent(uint32_t qpn, uint32_t event)
{
    (void)qpn; (void)event;
}

MlxQPContext *
MlxQP::Lookup(uint32_t qpn) { return CtxForQpn(qpn); }

IOMemoryDescriptor *
MlxQP::GetSqMemDesc(uint32_t qpn)
{
    MlxQPContext *ctx = CtxForQpn(qpn);
    return ctx ? ctx->sqDma.memDesc : NULL;
}

IOMemoryDescriptor *
MlxQP::GetRqMemDesc(uint32_t qpn)
{
    MlxQPContext *ctx = CtxForQpn(qpn);
    return ctx ? ctx->rqDma.memDesc : NULL;
}

MlxQPContext *
MlxQP::CtxForQpn(uint32_t qpn)
{
    if (!s || !qpn) return NULL;
    int slot = mlxKeyIndexFind(s->qpnKeys, s->qpnSlots, s->hashMask, s->hashShift, qpn);
    if (slot < 0 || !s->used[slot]) return NULL;
    return &s->table[slot];
}

MlxQPContext *
MlxQP::LockQp(uint32_t qpn)
{
    if (!s || !qpn) return NULL;
    IOLockLock(s->tableLock);
    int slot = mlxKeyIndexFind(s->qpnKeys, s->qpnSlots, s->hashMask, s->hashShift, qpn);
    if (slot < 0 || !s->used[slot]) {
        IOLockUnlock(s->tableLock);
        return NULL;
    }
    IOLockLock(s->qpLocks[slot]);
    IOLockUnlock(s->tableLock);
    return &s->table[slot];
}

void
MlxQP::UnlockQp(MlxQPContext *ctx)
{
    if (!ctx || !s) return;
    int slot = (int)(ctx - s->table);
    IOLockUnlock(s->qpLocks[slot]);
}

MlxQPContext *
MlxQP::LockQpByCq(uint32_t cqHandle, bool send)
{
    if (!s) return NULL;
    IOLockLock(s->tableLock);
    MlxQPContext *ctx = NULL;
    for (int i = 0; i < s->tableCap; i++) {
        if (s->used[i] &&
            (send ? s->table[i].sendCq : s->table[i].recvCq) == cqHandle) {
            if (ctx) { ctx = NULL; break; }  /* ambiguous shared CQ */
            ctx = &s->table[i];
        }
    }
    if (!ctx) { IOLockUnlock(s->tableLock); return NULL; }
    int slot = (int)(ctx - s->table);
    IOLockLock(s->qpLocks[slot]);
    IOLockUnlock(s->tableLock);
    return ctx;
}
