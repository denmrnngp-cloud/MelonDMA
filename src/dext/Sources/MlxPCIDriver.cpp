/*
 * MlxPCIDriver.cpp — DriverKit DEXT entry point (core layer).
 *
 * PCIDriverKit port of AppleMCX's MlxPCIDriver (kext). The kext matched via
 * IOPCIMatch and mapped BAR0 with IOMemoryMap. The DEXT uses
 * PCIDriverKit.framework's IOPCIDevice: Open()/GetBARInfo()/
 * _CopyDeviceMemoryWithIndex for the BAR aperture, MemoryRead/Write32 for MMIO
 * (both LOCALONLY, MemoryRead32 returns void), and ConfigureInterrupts for
 * MSI-X. The metaclass/dispatch glue lives in the iig-generated impl file
 * (build/generated/MlxPCIDriver.cpp); this file only provides method bodies.
 *
 * IVars pattern: the iig-generated header exposes an `ivars` member whose type
 * we define here as MlxPCIDriver_IVars. This keeps instance state out of the
 * public header (Apple's recommended DriverKit layout).
 *
 * Bring-up gate P1 (notes/21 §5, notes/09 §1): the first action after a
 * successful Open() is a single MMIO read of the firmware version at
 * BAR0 offset 0 (init segment fw_rev). A sane value means BAR + MMIO ordering
 * work, and the command interface can be wired up next.
 */
#include "MlxDriverKitCompat.h"
#include "MlxRegs.hpp"
#include "MlxCmd.hpp"
#include "MlxIfcHelpers.hpp"
#include "../hw/MlxP1Encoding.hpp"
#include "MlxEQ.hpp"
#include "MlxUAR.hpp"
#include "MlxHealth.hpp"
#include "MlxDMA.hpp"
#include "MlxFwPages.hpp"
#include "../hw/MlxHCA.hpp"
#include "../ib/MlxRoCE.hpp"
#include "../ib/MlxGID.hpp"
#include "../ib/MlxQP.hpp"
#include "../ib/MlxCQ.hpp"
#include "../ib/MlxMR.hpp"
#include "MlxUCIO.h"

#include <DriverKit/IOLib.h>
#include <DriverKit/IOService.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/IOTimerDispatchSource.h>
#include <DriverKit/IODispatchQueue.h>
#include <PCIDriverKit/IOPCIDevice.h>
#include <string.h>
#include <time.h>

#include "MlxLog.hpp"
#define MLX_LOG(fmt, ...)  IOLog("MlxPCIDriver: " fmt "\n", ##__VA_ARGS__)
#define MLX_DBG(fmt, ...)  MLX_DBGLOG("MlxPCIDriver: " fmt, ##__VA_ARGS__)

/* Quarantined MlxFwPages must outlive the driver instance that lost BAR/MMIO.
 * The registry retains the full object (and therefore every DMA reference)
 * until a later verified FLR establishes a DMA boundary. */
static const uint32_t kMlxQuarantineRegistrySize = 16;
static MlxFwPages *gMlxQuarantinedPages[kMlxQuarantineRegistrySize] = {};
struct MlxQuarantinedDma {
    IOMemoryDescriptor *mem;
    IODMACommand *dma;
};
static MlxQuarantinedDma gMlxQuarantinedDummy[kMlxQuarantineRegistrySize] = {};

static void
MlxRetainQuarantinedPages(MlxFwPages *pages)
{
    if (!pages) return;
    for (uint32_t i = 0; i < kMlxQuarantineRegistrySize; i++) {
        if (gMlxQuarantinedPages[i] == pages) return;
        if (!gMlxQuarantinedPages[i]) {
            gMlxQuarantinedPages[i] = pages;
            MLX_LOG("DMA quarantine registry: retained slot=%u ambiguous=%u",
                    i, pages->GetAmbiguousOwned());
            return;
        }
    }
    /* Preserve ownership by intentionally not deleting the object even if the
     * diagnostic registry is exhausted. */
    MLX_LOG("DMA quarantine registry FULL — object intentionally leaked/pinned");
}

static void
MlxReleaseQuarantinedPagesAfterReset()
{
    for (uint32_t i = 0; i < kMlxQuarantineRegistrySize; i++) {
        MlxFwPages *pages = gMlxQuarantinedPages[i];
        if (!pages) continue;
        gMlxQuarantinedPages[i] = NULL;
        pages->ReleaseQuarantineAfterReset();
        pages->Free();
        delete pages;
        MLX_LOG("DMA quarantine registry: released slot=%u after verified reset", i);
    }
    for (uint32_t i = 0; i < kMlxQuarantineRegistrySize; i++) {
        if (gMlxQuarantinedDummy[i].dma)
            mlxCompleteDma(gMlxQuarantinedDummy[i].dma);
        if (gMlxQuarantinedDummy[i].mem)
            gMlxQuarantinedDummy[i].mem->release();
        if (gMlxQuarantinedDummy[i].dma || gMlxQuarantinedDummy[i].mem)
            MLX_LOG("dummy DMA quarantine: released slot=%u after verified reset", i);
        gMlxQuarantinedDummy[i].dma = NULL;
        gMlxQuarantinedDummy[i].mem = NULL;
    }
}

static void
MlxRetainQuarantinedDummy(IOMemoryDescriptor *mem, IODMACommand *dma)
{
    for (uint32_t i = 0; i < kMlxQuarantineRegistrySize; i++) {
        if (!gMlxQuarantinedDummy[i].mem && !gMlxQuarantinedDummy[i].dma) {
            gMlxQuarantinedDummy[i].mem = mem;
            gMlxQuarantinedDummy[i].dma = dma;
            MLX_LOG("dummy DMA quarantine: retained slot=%u", i);
            return;
        }
    }
    MLX_LOG("dummy DMA quarantine registry FULL — mapping intentionally pinned");
}

/* ---- instance state (private; typed via MlxPCIDriver_DECLARE_IVARS) ---- */
struct MlxPCIDriver_IVars {
    IOPCIDevice         *fPci;
    IOMemoryDescriptor  *fBar0Mem;
    uint8_t              fBar0Index;
    uint16_t             fDeviceId;
    MlxCmd              *fCmd;
    MlxEQ               *fEQ;
    MlxUAR              *fUAR;
    MlxHCA              *fHCA;
    MlxHealth           *fHealth;
    MlxDMA              *fDMA;
    MlxFwPages          *fFwPages;
    MlxRoCE             *fRoCE;
    IODispatchQueue     *fEqQueue;
    IOTimerDispatchSource *fEqTimer;
    OSAction             *fEqTimerAction;
    uint64_t             fLastHealthCheck;
    uint64_t             fEqTickCount;
    uint32_t             fIssi;
    uint32_t             fPd;
    uint32_t             fXrcd;
    uint32_t             fDevIdx;
    char                 fDevName[16];
    bool                 fHcaEnabled;
    bool                 fHcaInitialized;
    bool                 fRuntimePagesStarted;
    bool                 fStopping;
    bool                 fDmaQuarantined;
    bool                 fRuntimePagesOk;    /* runtime PAGE_REQUEST without errors */
    bool                 fPhase2ObjectsOk;   /* CQ/QP/RST->INIT + cleanup verified */
    bool                 fWasInReset;        /* card was in reset after IOPCIFamily FLR */
    bool                 fStableCycleActive;
    bool                 fSwOwnerIdSupported;
    uint32_t             fStableCycleCount;
    uint32_t             fPhase2SubStage;   /* P1.4: last InitPhase2Runtime sub-step */
    uint32_t             fPhase2Ret;        /* P1.4: its kern_return_t (or synthetic) */
    uint32_t             fPhase2Opcode;     /* P1.4: failing command opcode (pre-cleanup) */
    uint32_t             fPhase2DeliveryStatus; /* P1.4: failing command delivery status */
    uint32_t             fPhase2FwStatus;   /* P1.4: failing command fw_status */
    uint32_t             fPhase2Syndrome;   /* P1.4: failing command syndrome */
    uint32_t             fSwOwnerId[4];
};

/* Inject our IVars type into the generated class before including its header.
 * The generated header uses the bare macro with no trailing ';', so the macro
 * must supply a complete member declaration. */
#define MlxPCIDriver_DECLARE_IVARS  struct MlxPCIDriver_IVars * ivars;

#include "MlxPCIDriver.h"
#include "MlxUserClient.h"

/* ---- RDMA datapath loopback test (no user client needed) ----
 * Proves the WQE→doorbell→CQE path on live hardware: one SEND to self.
 * Best-effort and fail-closed; never mutates driver health state. */
static bool
MlxLoopbackDatapathTest(MlxPCIDriver *core)
{
    MlxRoCE *roce = core->GetRoCE();
    if (!roce || !roce->GetQP() || !roce->GetCQ() ||
        !roce->GetMR() || !roce->GetGID()) {
        MLX_LOG("Loopback: verbs objects not ready");
        return false;
    }

    /* 1. DMA-coherent payload buffer + CPU mapping. */
    IOBufferMemoryDescriptor *buf = NULL;
    if (mlxAllocDmaBuffer(4096, 4096, kIOMemoryDirectionOutIn, &buf) !=
            kIOReturnSuccess || !buf) {
        MLX_LOG("Loopback: buffer alloc failed");
        return false;
    }
    uint64_t cpuAddr = 0;
    uint64_t cpuLen = 0;
    if (buf->Map(0, 0, 0, 0, &cpuAddr, &cpuLen) != kIOReturnSuccess || !cpuAddr) {
        MLX_LOG("Loopback: buffer Map failed");
        buf->release();
        return false;
    }
    MLX_LOG("Loopback: buf cpuAddr=0x%llx off=0x%llx", cpuAddr, cpuAddr & 0xFFF);
    uint8_t *bytes = (uint8_t *)(uintptr_t)cpuAddr;
    const uint32_t sendOff = 128;
    const uint32_t recvOff = 1024;
    const uint32_t payloadLen = 64;
    memset(bytes, 0xa5, 4096);
    for (uint32_t i = 0; i < payloadLen; i++)
        bytes[sendOff + i] = (uint8_t)(0x31u + i * 37u);
    memset(bytes + recvOff, 0xcc, payloadLen);

    /* 2. Register MR. */
    struct mlx_reg_mr_req mrReq = {};
    mrReq.startAddr = cpuAddr;
    mrReq.length = 4096;
    mrReq.accessFlags = 1;   /* LOCAL_WRITE */
    mrReq.pd = core->GetPd();
    struct mlx_reg_mr_resp mrResp = {};
    if (roce->RegMR(&mrReq, buf, &mrResp) != kIOReturnSuccess) {
        MLX_LOG("Loopback: RegMR failed");
        buf->release();
        return false;
    }
    MLX_LOG("Loopback: MR[%u] lkey=0x%08x va=0x%llx",
            mrResp.mrHandle, mrResp.lkey, mrResp.iova);

    /* 3. Program a loopback RoCEv2 GID (self as peer). */
    uint8_t gid[16] = {0xfe,0x80,0,0,0,0,0,0,
                       0x02,0x00,0x00,0xff,0xfe,0x00,0x00,0x01};
    uint8_t mac[6]  = {0x02,0x00,0x00,0x00,0x00,0x01};
    uint32_t gidIdx = roce->GetGID()->AllocGIDIndex();
    if (gidIdx == 0xFFFFFFFF ||
        roce->GetGID()->SetGID(gidIdx, gid, mac, MLX_ROCE_VERSION_2, 1,
                               false, 0) != kIOReturnSuccess) {
        MLX_LOG("Loopback: SetGID failed");
        roce->DeregMR(mrResp.mrHandle);
        buf->release();
        return false;
    }

    /* 4. CQ + RC QP. */
    struct mlx_create_cq_resp cqResp = {};
    if (roce->CreateCQ(256, &cqResp) != kIOReturnSuccess) {
        MLX_LOG("Loopback: CreateCQ failed");
        roce->GetGID()->DelGID(gidIdx);
        roce->DeregMR(mrResp.mrHandle);
        buf->release();
        return false;
    }
    struct mlx_create_qp_req qpReq = {};
    qpReq.pd = core->GetPd();
    qpReq.sendCq = cqResp.cqHandle;
    qpReq.recvCq = cqResp.cqHandle;
    qpReq.qpType = 0;       /* RC */
    qpReq.sqSize = 256;
    qpReq.rqSize = 256;
    struct mlx_create_qp_resp qpResp = {};
    if (roce->CreateQP(&qpReq, &qpResp) != kIOReturnSuccess) {
        MLX_LOG("Loopback: CreateQP failed");
        roce->DestroyCQ(cqResp.cqHandle);
        roce->GetGID()->DelGID(gidIdx);
        roce->DeregMR(mrResp.mrHandle);
        buf->release();
        return false;
    }
    MLX_LOG("Loopback: QP[%u] CQ[%u] created", qpResp.qpn, cqResp.cqHandle);

    /* 5. RST→INIT→RTR→RTS with self as peer (destQpn = own QPN). */
    uint32_t psn = 0x123456;
    bool rts = true;
    struct mlx_modify_qp_req mod = {};
    mod.qpn = qpResp.qpn;
    mod.curState = MLX_QP_STATE_RST;
    mod.newState = MLX_QP_STATE_INIT;
    mod.pkeyIndex = 0;
    mod.portNum = 1;
    rts = roce->ModifyQP(&mod) == kIOReturnSuccess;

    memset(&mod, 0, sizeof(mod));
    mod.qpn = qpResp.qpn;
    mod.curState = MLX_QP_STATE_INIT;
    mod.newState = MLX_QP_STATE_RTR;
    mod.destQpn = qpResp.qpn;
    mod.pathMtu = 3;        /* 1024 */
    mod.rqPsn = psn;
    mod.pkeyIndex = 0;
    mod.portNum = 1;
    memcpy(mod.ahDmac, mac, 6);
    memcpy(mod.ahDgid, gid, 16);
    mod.ahSgidIndex = gidIdx;
    mod.ahHopLimit = 1;
    mod.ahUdpSport = 0;
    mod.minRnrTimer = 12;
    mod.maxDestRdAtomic = 1;
    rts = rts && roce->ModifyQP(&mod) == kIOReturnSuccess;

    memset(&mod, 0, sizeof(mod));
    mod.qpn = qpResp.qpn;
    mod.curState = MLX_QP_STATE_RTR;
    mod.newState = MLX_QP_STATE_RTS;
    mod.sqPsn = psn;
    mod.maxRdAtomic = 1;
    mod.ackTimeout = 14;
    mod.retryCount = 7;
    mod.rnrRetry = 7;
    rts = rts && roce->ModifyQP(&mod) == kIOReturnSuccess;
    if (!rts) {
        MLX_LOG("Loopback: QP RST→INIT→RTR→RTS FAILED");
    }

    /* 6. Post RECV + SEND to self, poll CQ for both completions. */
    if (rts) {
        struct mlx_post_recv_req recv = {};
        recv.qpn = qpResp.qpn;
        recv.wrId = 0x1111;
        recv.sge.addr = mrResp.iova + recvOff;
        recv.sge.length = payloadLen;
        recv.sge.lkey = mrResp.lkey;
        kern_return_t r1 = roce->GetQP()->PostRecv(&recv);

        struct mlx_post_send_req send = {};
        send.qpn = qpResp.qpn;
        send.opcode = MLX_UC_WR_SEND;
        send.wrId = 0x2222;
        send.sge.addr = mrResp.iova + sendOff;
        send.sge.length = payloadLen;
        send.sge.lkey = mrResp.lkey;
        send.sendFlags = 1;  /* signaled */
        kern_return_t r2 = roce->GetQP()->PostSend(&send);
        MLX_LOG("Loopback: PostRecv=0x%x PostSend=0x%x", r1, r2);

        struct mlx_poll_cq_req pollReq = {};
        pollReq.cqHandle = cqResp.cqHandle;
        pollReq.maxEntries = 16;
        struct mlx_poll_cq_resp pollResp = {};
        struct mlx_work_completion completed[2] = {};
        uint32_t done = 0;
        for (uint32_t i = 0; i < 2000 && done < 2; i++) {
            if (roce->GetCQ()->PollCQ(&pollReq, &pollResp) == kIOReturnSuccess) {
                for (uint32_t j = 0; j < pollResp.count && done < 2; j++)
                    completed[done++] = pollResp.wc[j];
            }
            if (done < 2) IOSleep(1);
        }
        MLX_LOG("Loopback: %u completions", done);
        for (uint32_t i = 0; i < done; i++) {
            MLX_LOG("Loopback: wc[%u] wrId=0x%llx status=%u opcode=%u bytes=%u",
                    i, completed[i].wrId, completed[i].status,
                    completed[i].opcode, completed[i].byteLen);
        }

        bool sawRecv = false, sawSend = false, completionsOk = done == 2;
        for (uint32_t i = 0; i < done; i++) {
            completionsOk = completionsOk && completed[i].status == MLX_UC_WC_SUCCESS;
            sawRecv = sawRecv || completed[i].wrId == 0x1111;
            sawSend = sawSend || completed[i].wrId == 0x2222;
        }
        mlxMemoryBarrier();
        bool payloadOk = memcmp(bytes + sendOff, bytes + recvOff,
                                payloadLen) == 0;
        bool guardsOk = true;
        for (uint32_t i = recvOff - 64; i < recvOff; i++)
            guardsOk = guardsOk && bytes[i] == 0xa5;
        for (uint32_t i = recvOff + payloadLen;
             i < recvOff + payloadLen + 64; i++)
            guardsOk = guardsOk && bytes[i] == 0xa5;
        rts = r1 == kIOReturnSuccess && r2 == kIOReturnSuccess &&
              completionsOk && sawRecv && sawSend && payloadOk && guardsOk;
        MLX_LOG("Loopback: datapath=%s recv=%u send=%u payload=%s guards=%s",
                rts ? "PASS" : "FAIL", sawRecv, sawSend,
                payloadOk ? "ok" : "BAD", guardsOk ? "ok" : "BAD");
    }

    /* 7. Cleanup. */
    roce->DestroyQP(qpResp.qpn);
    roce->DestroyCQ(cqResp.cqHandle);
    roce->GetGID()->DelGID(gidIdx);
    roce->DeregMR(mrResp.mrHandle);
    buf->release();
    MLX_LOG("Loopback: cleanup done");
    if (rts)
        MLX_LOG("Loopback: PASS — 2 CQEs, payload and guards verified");
    else
        MLX_LOG("Loopback: FAIL");
    return rts;
}


/* MODIFY_NIC_VPORT_CONTEXT (0x755): enable RoCE on the native vport
 * (other_vport=0, vport_number=0). field_select.roce_en=1 and
 * nic_vport_context.roce_en=1 (Linux vport.c mlx5_nic_vport_update_roce_state).
 * Without this CREATE_QP returns BAD_RESOURCE 0x15A3C9 — "can not open RoCE QP
 * if vport roce_en == 0" (Mellanox syndrome table). Prints QUERY before/after. */
static kern_return_t
MlxEnableVportRoce(MlxPCIDriver *core)
{
    MlxCmd *cmd = core->GetCmd();
    if (!cmd) return kIOReturnNotReady;

    auto queryRoceEn = [&](uint32_t *roceEnOut) -> kern_return_t {
        uint8_t qin[16] = {};
        /* query_nic_vport_context_out = 16 B header + 256 B
         * nic_vport_context = 272 B (0x110). With a smaller outlen fw returns
         * fw_status=81 syndrome=0x51552b (bad output length). */
        uint8_t qout[0x110] = {};
        mlxSetBits(qin, 0x00, 16, MLX_CMD_OP_QUERY_NIC_VPORT_CONTEXT);
        mlxSetBits(qin, 0x40, 1, 0);       /* other_vport = 0 (native) */
        mlxSetBits(qin, 0x50, 16, 0);      /* vport_number = 0 */
        kern_return_t kr = cmd->Exec(MLX_CMD_OP_QUERY_NIC_VPORT_CONTEXT,
                                     qin, sizeof(qin), qout, sizeof(qout), 5000);
        if (kr != kIOReturnSuccess) {
            MLX_LOG("QUERY_NIC_VPORT_CONTEXT failed: 0x%x synd=0x%x", kr,
                    cmd->LastSyndrome());
            return kr;
        }
        /* query_nic_vport_context_out: nic_vport_context at bit 0x80;
         * roce_en — bit 31 inside the context → 0x80 + 31 = 0x9f. */
        *roceEnOut = (uint32_t)mlxGetBits(qout, 0x9f, 1);
        return kIOReturnSuccess;
    };

    uint32_t before = 0;
    kern_return_t kr = queryRoceEn(&before);
    if (kr != kIOReturnSuccess) return kr;
    MLX_LOG("QUERY_NIC_VPORT_CONTEXT: roce_en=%u", before);

    if (before == 1) return kIOReturnSuccess;

    uint8_t in[0x200] = {};                /* modify_nic_vport_context_in = 512 B */
    uint8_t out[16] = {};
    mlxSetBits(in, 0x00, 16, MLX_CMD_OP_MODIFY_NIC_VPORT_CONTEXT);
    mlxSetBits(in, 0x40, 1, 0);            /* other_vport = 0 */
    mlxSetBits(in, 0x50, 16, 0);           /* vport_number = 0 */
    mlxSetBits(in, 0x7e, 1, 1);            /* field_select.roce_en (bit 96+30) */
    mlxSetBits(in, 0x81f, 1, 1);           /* nic_vport_context.roce_en (0x800+31) */
    kr = cmd->Exec(MLX_CMD_OP_MODIFY_NIC_VPORT_CONTEXT,
                   in, sizeof(in), out, sizeof(out), 5000);
    if (kr != kIOReturnSuccess) {
        MLX_LOG("MODIFY_NIC_VPORT_CONTEXT (roce_en=1) failed: 0x%x synd=0x%x",
                kr, cmd->LastSyndrome());
        return kr;
    }

    uint32_t after = 0;
    kr = queryRoceEn(&after);
    if (kr != kIOReturnSuccess) return kr;
    MLX_LOG("MODIFY_NIC_VPORT_CONTEXT: roce_en=1 — query after=%u", after);
    return after == 1 ? kIOReturnSuccess : kIOReturnNotReady;
}

static uint64_t
MlxEqPollDeadline()
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 10ULL * 1000 * 1000;
}

struct MlxEqTimerArmContext {
    IOTimerDispatchSource *timer;
    kern_return_t result;
};

static void
MlxArmEqTimerOnQueue(void *opaque)
{
    MlxEqTimerArmContext *ctx = static_cast<MlxEqTimerArmContext *>(opaque);
    if (!ctx || !ctx->timer) return;
    ctx->result = ctx->timer->WakeAtTime(kIOTimerClockUptimeRaw,
                                          MlxEqPollDeadline(), 1000000);
}

static void
MlxDrainEqTimerQueue(void *)
{
    /* DispatchSync_f is the teardown barrier; all earlier timer callbacks on
     * this serial queue have completed when it returns. */
}

kern_return_t
MlxPCIDriver::StartEqPoller()
{
    if (!ivars->fEQ || !ivars->fRoCE || ivars->fEqTimer)
        return kIOReturnNotReady;
    IODispatchQueue *queue = NULL;
    OSAction *action = NULL;
    IODispatchQueueName queueName = "Default";
    kern_return_t kr = CopyDispatchQueue(queueName, &queue);
    if (kr != kIOReturnSuccess || !queue) return kr ? kr : kIOReturnNotReady;
    /* WakeAtTime has queue affinity in DriverKit.  Keep the exact queue alive
     * for the complete timer lifetime and execute the initial arm on it. */
    ivars->fEqQueue = queue;
    kr = IOTimerDispatchSource::Create(queue, &ivars->fEqTimer);
    if (kr != kIOReturnSuccess || !ivars->fEqTimer)
        goto fail;
    kr = CreateActionEqTimerOccurred(0, &action);
    if (kr == kIOReturnSuccess)
        kr = ivars->fEqTimer->SetHandler(action);
    if (kr == kIOReturnSuccess) {
        /* Keep the action alive for the dispatch-source lifetime.  Releasing
         * the only reference here crashes at the first timer delivery on the
         * DriverKit runtime used by the target Mac. */
        ivars->fEqTimerAction = action;
        action = NULL;
    }
    if (action) action->release();
    if (kr == kIOReturnSuccess)
        kr = ivars->fEqTimer->SetEnableWithCompletion(true, nullptr);
    if (kr == kIOReturnSuccess) {
        MlxEqTimerArmContext arm = { ivars->fEqTimer, kIOReturnNotReady };
        bool alreadyOnQueue = queue->OnQueue();
        if (alreadyOnQueue) MlxArmEqTimerOnQueue(&arm);
        else                queue->DispatchSync_f(&arm, MlxArmEqTimerOnQueue);
        kr = arm.result;
        if (kr == kIOReturnSuccess)
            MLX_LOG("EQ timer initial arm on Default queue (caller_on_queue=%u)",
                    alreadyOnQueue ? 1 : 0);
    }
    if (kr != kIOReturnSuccess) goto fail;
    ivars->fLastHealthCheck = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    ivars->fEqTickCount = 0;
    MLX_LOG("EQ continuous poller started (10 ms)");
    return kIOReturnSuccess;

fail:
    StopEqPoller();
    return kr ? kr : kIOReturnNoMemory;
}

void
MlxPCIDriver::StopEqPoller()
{
    if (!ivars || (!ivars->fEqTimer && !ivars->fEqQueue)) return;
    IOTimerDispatchSource *timer = ivars->fEqTimer;
    IODispatchQueue *queue = ivars->fEqQueue;
    OSAction *action = ivars->fEqTimerAction;
    /* Publish the stopped state before waiting behind an in-flight callback.
     * That callback will then skip rearming itself. */
    ivars->fEqTimer = NULL;
    ivars->fEqQueue = NULL;
    ivars->fEqTimerAction = NULL;
    if (timer) {
        /* Cancel() is asynchronous: its completion runs only after every
         * pending timer callback has returned.  Releasing timer/action/queue
         * immediately after Cancel() left DriverKit's cancellation block
         * dereferencing a freed source (0.159 crash at address 0x8).  Keep
         * all three +1 references until that completion boundary. */
        if (queue && !queue->OnQueue())
            queue->DispatchSync_f(NULL, MlxDrainEqTimerQueue);
        kern_return_t kr = timer->Cancel(^{
            if (action) action->release();
            if (queue) queue->release();
            timer->release();
        });
        if (kr != kIOReturnSuccess) {
            /* A leak is safer than freeing objects that may still be used by
             * an in-flight callback.  The DEXT process is terminating here. */
            MLX_LOG("EQ timer cancel failed kr=0x%x — references retained", kr);
        }
        return;
    }
    if (action) action->release();
    if (queue) queue->release();
}

void
MlxPCIDriver::EqTimerOccurred_Impl(OSAction *action, uint64_t time)
{
    (void)action; (void)time;
    if (!ivars || ivars->fStopping || !ivars->fEqTimer) return;
    uint64_t tick = ++ivars->fEqTickCount;
    if (tick == 1) MLX_LOG("EQ timer first callback entered");
    if (ivars->fEQ) ivars->fEQ->Poll();
    if (ivars->fRoCE &&
        ivars->fRoCE->DrainPendingPageRequests() != kIOReturnSuccess)
        ivars->fRuntimePagesOk = false;
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (ivars->fHealth &&
        now - ivars->fLastHealthCheck >= 2ULL * 1000 * 1000 * 1000) {
        ivars->fHealth->Check();
        ivars->fLastHealthCheck = now;
        if (!ivars->fHealth->IsHealthy()) ivars->fRuntimePagesOk = false;
    }
    if (tick == 1) MLX_LOG("EQ timer first callback poll/drain passed");
    if (ivars->fEqTimer) {
        kern_return_t kr = ivars->fEqTimer->WakeAtTime(
            kIOTimerClockUptimeRaw, MlxEqPollDeadline(), 1000000);
        if (kr != kIOReturnSuccess) {
            ivars->fRuntimePagesOk = false;
            MLX_LOG("EQ timer rearm failed tick=%llu kr=0x%x",
                    (unsigned long long)tick, kr);
        } else if (tick == 1) {
            MLX_LOG("EQ timer first callback rearmed");
        }
    }
    if ((tick % 500) == 0)
        MLX_LOG("EQ timer heartbeat ticks=%llu", (unsigned long long)tick);
}

void
MlxPCIDriver::DestroyPhase2FirmwareResources()
{
    if (!ivars->fCmd) return;
    /* EQ/UAR/XRCD/PD can still be referenced by an object whose destroy
     * command timed out.  Do not dismantle dependencies after that point;
     * PerformFlr() is the only boundary that releases the quarantine. */
    if (DmaQuarantined()) return;
    uint16_t pciCommand = 0;
    if (ivars->fPci) ivars->fPci->ConfigurationRead16(4, &pciCommand);
    if (!(pciCommand & 0x2)) {
        EnterDmaQuarantine(0x50324442u); /* 'P2DB': no safe command path */
        return;
    }
    if (ivars->fEQ && ivars->fEQ->EqNumber()) {
        uint32_t eqn = ivars->fEQ->EqNumber();
        if (ivars->fEQ->DestroyEQ(eqn) != kIOReturnSuccess)
            EnterDmaQuarantine(0x45514400u | (eqn & 0xffu));
    }
    if (ivars->fUAR && ivars->fUAR->GetBootUarIndex())
        ivars->fUAR->FreeUAR(ivars->fUAR->GetBootUarIndex());
    uint8_t in[16] = {}, out[16] = {};
    if (ivars->fXrcd) {
        mlxSetBits(in, 0x00, 16, MLX_CMD_OP_DEALLOC_XRCD);
        mlxSetBits(in, 0x48, 24, ivars->fXrcd);
        if (ivars->fCmd->Exec(MLX_CMD_OP_DEALLOC_XRCD, in, sizeof(in),
                              out, sizeof(out), 5000) == kIOReturnSuccess)
            ivars->fXrcd = 0;
    }
    memset(in, 0, sizeof(in)); memset(out, 0, sizeof(out));
    if (ivars->fPd) {
        mlxSetBits(in, 0x00, 16, MLX_CMD_OP_DEALLOC_PD);
        mlxSetBits(in, 0x48, 24, ivars->fPd);
        if (ivars->fCmd->Exec(MLX_CMD_OP_DEALLOC_PD, in, sizeof(in),
                              out, sizeof(out), 5000) == kIOReturnSuccess)
            ivars->fPd = 0;
    }
}

/* P1.4 diagnostics: record which Phase2 sub-step failed and its return code
 * before jumping to the shared cleanup path. The sub-stage enum lives in
 * MlxUCIO.h (mlx_phase2_substage) so the userspace gate can print it. */
#define MLX_PHASE2_FAIL(sub, ret)                \
    do {                                         \
        ivars->fPhase2SubStage = (sub);          \
        ivars->fPhase2Ret = (uint32_t)(ret);     \
        ivars->fPhase2Opcode = ivars->fCmd ? ivars->fCmd->LastOpcode() : 0;      \
        ivars->fPhase2DeliveryStatus = ivars->fCmd ? ivars->fCmd->LastDeliveryStatus() : 0; \
        ivars->fPhase2FwStatus = ivars->fCmd ? ivars->fCmd->LastFwStatus() : 0;  \
        ivars->fPhase2Syndrome = ivars->fCmd ? ivars->fCmd->LastSyndrome() : 0;  \
        goto fail;                               \
    } while (0)

bool
MlxPCIDriver::InitPhase2Runtime()
{
    ivars->fPhase2ObjectsOk = false;
    ivars->fPhase2SubStage = MLX_PHASE2_SUB_NONE;
    ivars->fPhase2Ret = 0;
    const MlxHcaCaps &caps = ivars->fHCA->Caps();
    ivars->fDMA = new MlxDMA();
    ivars->fUAR = new MlxUAR();
    if (!ivars->fDMA || !ivars->fUAR)
        MLX_PHASE2_FAIL(MLX_PHASE2_SUB_DMA_INIT, kIOReturnNoMemory);
    {
        kern_return_t kr = ivars->fDMA->Init(this, ivars->fPci);
        if (kr != kIOReturnSuccess)
            MLX_PHASE2_FAIL(MLX_PHASE2_SUB_DMA_INIT, kr);
    }
    {
        kern_return_t kr = ivars->fUAR->Init(this, ivars->fPci, ivars->fBar0Index,
                                             caps.logUarPageSize, caps.uar4k);
        if (kr != kIOReturnSuccess)
            MLX_PHASE2_FAIL(MLX_PHASE2_SUB_UAR_INIT, kr);
    }
    if (!AllocPd())
        MLX_PHASE2_FAIL(MLX_PHASE2_SUB_ALLOC_PD, kIOReturnIOError);
    if (!AllocXrcd())
        MLX_PHASE2_FAIL(MLX_PHASE2_SUB_ALLOC_XRCD, kIOReturnIOError);
    {
        uint32_t uar = 0;
        kern_return_t kr = ivars->fUAR->AllocUAR(&uar);
        if (kr != kIOReturnSuccess)
            MLX_PHASE2_FAIL(MLX_PHASE2_SUB_ALLOC_UAR, kr);
    }
    ivars->fEQ = new MlxEQ();
    if (!ivars->fEQ)
        MLX_PHASE2_FAIL(MLX_PHASE2_SUB_EQ_INIT, kIOReturnNoMemory);
    {
        kern_return_t kr = ivars->fEQ->Init(this, 0);
        if (kr != kIOReturnSuccess)
            MLX_PHASE2_FAIL(MLX_PHASE2_SUB_EQ_INIT, kr);
    }
    {
        uint32_t eqn = 0;
        kern_return_t kr = ivars->fEQ->CreateEQ(&eqn);
        if (kr != kIOReturnSuccess)
            MLX_PHASE2_FAIL(MLX_PHASE2_SUB_CREATE_EQ, kr);
    }
    ivars->fRoCE = new MlxRoCE();
    if (!ivars->fRoCE)
        MLX_PHASE2_FAIL(MLX_PHASE2_SUB_ROCE_INIT, kIOReturnNoMemory);
    {
        kern_return_t kr = ivars->fRoCE->Init(this, ivars->fHCA);
        if (kr != kIOReturnSuccess)
            MLX_PHASE2_FAIL(MLX_PHASE2_SUB_ROCE_INIT, kr);
    }
    ivars->fEQ->AddNotifier(ivars->fRoCE);
    {
        kern_return_t kr = MlxEnableVportRoce(this);
        if (kr != kIOReturnSuccess)
            MLX_PHASE2_FAIL(MLX_PHASE2_SUB_ENABLE_VPORT, kr);
    }
    {
        struct mlx_query_port_resp port = {};
        kern_return_t kr = ivars->fRoCE->QueryPort(&port);
        if (kr != kIOReturnSuccess || !port.portState)
            MLX_PHASE2_FAIL(MLX_PHASE2_SUB_QUERY_PORT,
                            kr != kIOReturnSuccess ? kr : kIOReturnNotReady);
    }
    {
        struct mlx_create_cq_resp cq = {};
        kern_return_t cqKr = ivars->fRoCE->CreateCQ(64, &cq);
        if (cqKr != kIOReturnSuccess)
            MLX_PHASE2_FAIL(MLX_PHASE2_SUB_CREATE_CQ, cqKr);
        /* CREATE_CQ can succeed at the command plane yet return cqn=0; treat
         * that as an explicit failure so it is never hidden behind a later
         * CreateQP lookup miss (which would keep lastOpcode == CREATE_CQ). */
        if (!cq.cqHandle)
            MLX_PHASE2_FAIL(MLX_PHASE2_SUB_CREATE_CQ, kIOReturnNotFound);

        struct mlx_create_qp_req qpReq = {};
        qpReq.pd = ivars->fPd;
        qpReq.sendCq = cq.cqHandle;
        qpReq.recvCq = cq.cqHandle;
        qpReq.qpType = 0;
        qpReq.sqSize = 64;
        qpReq.rqSize = 64;
        struct mlx_create_qp_resp qp = {};
        kern_return_t qpKr = ivars->fRoCE->CreateQP(&qpReq, &qp);
        if (qpKr != kIOReturnSuccess) {
            (void)ivars->fRoCE->DestroyCQ(cq.cqHandle);
            MLX_PHASE2_FAIL(MLX_PHASE2_SUB_CREATE_QP, qpKr);
        }
        if (!qp.qpn) {
            (void)ivars->fRoCE->DestroyCQ(cq.cqHandle);
            MLX_PHASE2_FAIL(MLX_PHASE2_SUB_CREATE_QP, kIOReturnNotFound);
        }
        struct mlx_modify_qp_req mod = {};
        mod.qpn = qp.qpn;
        mod.curState = MLX_QP_STATE_RST;
        mod.newState = MLX_QP_STATE_INIT;
        mod.portNum = 1;
        kern_return_t modKr = ivars->fRoCE->ModifyQP(&mod);
        if (modKr != kIOReturnSuccess) {
            (void)ivars->fRoCE->DestroyQP(qp.qpn);
            (void)ivars->fRoCE->DestroyCQ(cq.cqHandle);
            MLX_PHASE2_FAIL(MLX_PHASE2_SUB_MODIFY_QP, modKr);
        }
        kern_return_t destroyQpKr = ivars->fRoCE->DestroyQP(qp.qpn);
        if (destroyQpKr != kIOReturnSuccess) {
            (void)ivars->fRoCE->DestroyCQ(cq.cqHandle);
            MLX_PHASE2_FAIL(MLX_PHASE2_SUB_DESTROY_QP, destroyQpKr);
        }
        kern_return_t destroyCqKr = ivars->fRoCE->DestroyCQ(cq.cqHandle);
        if (destroyCqKr != kIOReturnSuccess)
            MLX_PHASE2_FAIL(MLX_PHASE2_SUB_DESTROY_CQ, destroyCqKr);
        ivars->fPhase2ObjectsOk = true;
    }
    ivars->fRuntimePagesOk = true;
    ivars->fHealth = new MlxHealth();
    if (!ivars->fHealth)
        MLX_PHASE2_FAIL(MLX_PHASE2_SUB_HEALTH_INIT, kIOReturnNoMemory);
    {
        kern_return_t kr = ivars->fHealth->Init(this);
        if (kr != kIOReturnSuccess)
            MLX_PHASE2_FAIL(MLX_PHASE2_SUB_HEALTH_INIT, kr);
    }
    if (StartEqPoller() != kIOReturnSuccess)
        MLX_PHASE2_FAIL(MLX_PHASE2_SUB_EQ_POLLER, kIOReturnIOError);
    MLX_LOG("Phase 2 runtime rebuilt after firmware reinit");
    return true;

fail:
    ivars->fPhase2ObjectsOk = false;
    MLX_LOG("Phase2 rebuild FAIL sub-stage=%u ret=0x%x",
            ivars->fPhase2SubStage, ivars->fPhase2Ret);
    StopEqPoller();
    if (ivars->fRoCE) { ivars->fRoCE->Free(); delete ivars->fRoCE; ivars->fRoCE = NULL; }
    DestroyPhase2FirmwareResources();
    if (ivars->fEQ) { ivars->fEQ->Free(); delete ivars->fEQ; ivars->fEQ = NULL; }
    if (ivars->fUAR) { ivars->fUAR->Free(); delete ivars->fUAR; ivars->fUAR = NULL; }
    if (ivars->fDMA) { ivars->fDMA->Free(); delete ivars->fDMA; ivars->fDMA = NULL; }
    if (ivars->fHealth) { ivars->fHealth->Free(); delete ivars->fHealth; ivars->fHealth = NULL; }
    return false;
}

#undef MLX_PHASE2_FAIL

bool
MlxPCIDriver::init()
{
    MLX_LOG("init");
    if (!super::init())
        return false;

    ivars = IONewZero(MlxPCIDriver_IVars, 1);
    if (!ivars)
        return false;
    ivars->fBar0Index           = 0;   /* BAR0 = memory index 0 on this card */
    ivars->fHcaEnabled          = false;
    ivars->fHcaInitialized      = false;
    ivars->fRuntimePagesStarted = false;
    ivars->fStopping            = false;
    ivars->fDmaQuarantined      = false;
    ivars->fRuntimePagesOk      = true;
    ivars->fPhase2ObjectsOk     = false;
    ivars->fWasInReset           = false;
    ivars->fStableCycleActive    = false;
    ivars->fStableCycleCount     = 0;
    read_random(ivars->fSwOwnerId, sizeof(ivars->fSwOwnerId));
    if (!(ivars->fSwOwnerId[0] | ivars->fSwOwnerId[1] |
          ivars->fSwOwnerId[2] | ivars->fSwOwnerId[3]))
        ivars->fSwOwnerId[0] = 1;
    ivars->fIssi                = 0;
    ivars->fPd                  = 0;
    ivars->fXrcd                = 0;
    ivars->fDevIdx              = 0;
    strlcpy(ivars->fDevName, "mlx5_0", sizeof(ivars->fDevName));
    return true;
}

void
MlxPCIDriver::free()
{
    MLX_LOG("free");
    Cleanup();
    if (ivars)
        IODelete(ivars, MlxPCIDriver_IVars, 1);
    super::free();
}

kern_return_t
MlxPCIDriver::Start_Impl(IOService * provider)
{
    MLX_LOG("Start — ConnectX RDMA DEXT initializing (build p3-cq-arm-1)");

    /* Establish the IOService/provider relationship before caching or using
     * the IOPCIDevice proxy.  Without SUPERDISPATCH the provider is valid only
     * for the duration of this Start call; the first asynchronous MMIO access
     * then enters MemoryWrite32 with a defunct proxy. */
    kern_return_t kr = Start(provider, SUPERDISPATCH);
    if (kr != kIOReturnSuccess) {
        MLX_LOG("IOService::Start failed: 0x%x", kr);
        return kr;
    }
    auto failStart = [&](kern_return_t error) -> kern_return_t {
        ivars->fPci = NULL;
        (void)Stop(provider, SUPERDISPATCH);
        return error;
    };

    ivars->fPci = OSDynamicCast(IOPCIDevice, provider);
    if (!ivars->fPci) {
        MLX_LOG("provider is not IOPCIDevice");
        return failStart(kIOReturnNoDevice);
    }

    /* Open an exclusive session. Close() resets bus master + memory-space
     * enable, so the device must be torn down before Close (notes/11 §2.5). */
    kr = ivars->fPci->Open(this, 0);
    if (kr != kIOReturnSuccess) {
        MLX_LOG("PCI Open failed: 0x%x (thread 734812 PCIDriverKit bug?)", kr);
        return failStart(kr);
    }

    /* Query BAR0: type + size. Expected kPCIBARTypeM64 on ConnectX-4 Lx. */
    uint8_t barType = 0;
    uint64_t barSize = 0;
    kr = ivars->fPci->GetBARInfo(0, &ivars->fBar0Index, &barSize, &barType);
    if (kr != kIOReturnSuccess) {
        MLX_LOG("GetBARInfo(0) failed: 0x%x", kr);
        ivars->fPci->Close(this, 0);
        return failStart(kIOReturnNoDevice);
    }
    MLX_LOG("BAR0: index=%u size=%llu type=%u", ivars->fBar0Index, barSize, barType);

    /* Obtain the BAR0 memory descriptor (for sub-range mapping, e.g. UAR).
     * _CopyDeviceMemoryWithIndex is the documented-ish path (notes/11 §2.5,
     * notes/09 §17 Q1 to Apple DTS). */
    kr = ivars->fPci->_CopyDeviceMemoryWithIndex(ivars->fBar0Index,
                                                  &ivars->fBar0Mem, this);
    if (kr != kIOReturnSuccess || !ivars->fBar0Mem) {
        MLX_LOG("_CopyDeviceMemoryWithIndex failed: 0x%x — MMIO via MemoryRead/Write only", kr);
        ivars->fBar0Mem = NULL;
    }

    /* ---- Enable memory space + bus master via Command Register (config +4).
     * PCIDriverKit has no SetMemoryEnable — we write the MSE(1)/BME(2) bits
     * ourselves. Without MSE MemoryRead32 returns 0xffffffff. */
    uint16_t cmd = 0;
    ivars->fPci->ConfigurationRead16(4, &cmd);
    MLX_LOG("PCI command: 0x%04x", cmd);
    if (!(cmd & 0x2)) {
        cmd |= 0x2 | 0x4;              /* MSE | BME */
        ivars->fPci->ConfigurationWrite16(4, cmd);
        uint16_t cmd2 = 0;
        ivars->fPci->ConfigurationRead16(4, &cmd2);
        MLX_LOG("PCI command after MSE|BME: 0x%04x", cmd2);
    }

    /* ---- Gate P1: MMIO PoC — read fw_rev at init-seg +0x0. A sane nonzero
     * value means BAR mapping + MMIO ordering work (notes/21 §5). */
    uint32_t fwRevBE = 0;
    ivars->fPci->MemoryRead32(ivars->fBar0Index, 0, &fwRevBE);
    uint32_t fwRev = OSSwapBigToHostInt32(fwRevBE);
    MLX_LOG("Gate P1 OK — fw_rev=0x%08x (firmware version registers reachable)", fwRev);

    /* Select the HCA implementation matching the PCI device ID.
     * PM-PANIC (notes/32): Start blocks the power-management core — on sleep
     * the PM callback doesn't return for >35s → panic "Wake transition timed
     * out". So NO infinite waits in Start: maximum 25s, inside — one self-FLR
     * attempt (up to 20s). If the card never comes alive — an honest
     * kIOReturnNoDevice: the kernel rematches us again and again, each attempt
     * gets fresh 25s (equivalent to an infinite wait, but without blocking PM). */
    uint32_t did32 = 0;
    {
        uint32_t waitedMs = 0;
        bool flrTried = false;
        while (waitedMs < 25000) {
            ivars->fPci->ConfigurationRead32(0, &did32);
            if ((did32 >> 16) != 0xffff) break;
            if (waitedMs == 0) MLX_LOG("card in reset (0xffff) — waiting up to 25s (PM limit)");
            /* after 10s — the single self-FLR attempt (notes/31) */
            if (!flrTried && waitedMs >= 10000) {
                flrTried = true;
                MLX_LOG("card dead 10s — self-FLR attempt");
                PerformFlr();
                ivars->fPci->ConfigurationRead32(0, &did32);
                if ((did32 >> 16) != 0xffff) {
                    MLX_LOG("self-FLR REVIVED the card");
                    break;
                }
                continue;
            }
            IOSleep(500);
            waitedMs += 500;
        }
        if ((did32 >> 16) == 0xffff) {
            MLX_LOG("card did not come alive within 25s — restarting via rematch");
            ivars->fPci->Close(this, 0);
            return failStart(kIOReturnNoDevice);
        }
        if (waitedMs > 0) MLX_LOG("card came alive after %u ms of reset", waitedMs);
        /* IOPCIFamily already performed FLR when handing resources to a new dext
         * process (log: "[getResources()] Performing FLR on nub ethernet"). A
         * repeated FLR now = double reset in a row → fw may not survive (notes/35). */
        ivars->fWasInReset = (waitedMs > 0);
    }
    // config offset 0 = [device_id:16 | vendor_id:16] — device in the HIGH bits
    ivars->fDeviceId = (uint16_t)(did32 >> 16);
    ivars->fHCA = MlxHCALoader::Create(ivars->fDeviceId);
    if (!ivars->fHCA) {
        MLX_LOG("unsupported device 0x%04x (no validated HCA backend)", ivars->fDeviceId);
        ivars->fPci->Close(this, 0);
        return failStart(kIOReturnNoDevice);
    }
    ivars->fHCA->AttachCore(this);

    /* ---- FLR: reset the device + reload firmware (notes/29).
     * If the card JUST came out of reset (IOPCIFamily FLR on ownership change) —
     * do NOT perform a second FLR: fw is already reloading, a double reset in
     * a row leaves the card dead until reboot (notes/35).
     * If the card was alive (our self-FLR from ReinitFw / first load) —
     * FLR is needed for clean firmware. ---- */
    if (ivars->fWasInReset) {
        MLX_LOG("Start: card was in reset (IOPCIFamily FLR) — double FLR skipped, waiting for fw");
        /* IOPCIFamily already did FLR; we only wait for fw load (no second FLR).
         * First restore MSE|BME (FLR cleared them), then wait for fw_rev. */
        uint16_t cmd2 = 0;
        ivars->fPci->ConfigurationRead16(4, &cmd2);
        ivars->fPci->ConfigurationWrite16(4, cmd2 | 0x6);
        IOSleep(10);
        uint32_t fw = 0;
        for (uint32_t i = 0; i < 120; i++) {
            uint32_t revBE = 0;
            ivars->fPci->MemoryRead32(ivars->fBar0Index, 0, &revBE);
            fw = OSSwapBigToHostInt32(revBE);
            if (fw != 0 && fw != 0xFFFFFFFF) break;
            IOSleep(500);
        }
        MLX_LOG("Start: fw_rev after IOPCIFamily FLR = 0x%08x", fw);
        if (fw == 0 || fw == 0xFFFFFFFF) {
            MLX_LOG("Start: IOPCIFamily FLR not confirmed — initialization aborted");
            Cleanup();
            ivars->fPci->Close(this, 0);
            return failStart(kIOReturnNoDevice);
        }
        MlxReleaseQuarantinedPagesAfterReset();
    } else {
        if (!PerformFlr()) {
            MLX_LOG("Start: FLR verification failed — initialization aborted");
            Cleanup();
            ivars->fPci->Close(this, 0);
            return failStart(kIOReturnNoDevice);
        }
    }

    /* ---- Phase 1, step 1-2: MlxCmd (init-seg + cmdq DMA + enable) ---- */
    ivars->fCmd = new MlxCmd();
    kern_return_t cmdKr = ivars->fCmd->Init(this);
    if (cmdKr != kIOReturnSuccess) {
        MLX_LOG("Phase 1 FAIL: MlxCmd::Init 0x%x — card registered, HCA init not started", cmdKr);
        delete ivars->fCmd; ivars->fCmd = NULL;
    } else {
        MLX_LOG("Phase 1 OK: MlxCmd init (cmdq alive, firmware online)");
        /* Steps 4-6: ENABLE_HCA → QUERY/SET_ISSI */
        if (FwInit()) {
            MLX_LOG("Phase 1 COMPLETE: HCA enabled, ISSI set");
            /* ---- Phase 2: PD + UAR + verbs objects ---- */
            const MlxHcaCaps &caps = ivars->fHCA->Caps();
            ivars->fDMA = new MlxDMA();
            ivars->fUAR = new MlxUAR();
            if (!ivars->fDMA || !ivars->fUAR ||
                ivars->fDMA->Init(this, ivars->fPci) != kIOReturnSuccess ||
                ivars->fUAR->Init(this, ivars->fPci, ivars->fBar0Index,
                                  caps.logUarPageSize, caps.uar4k) != kIOReturnSuccess) {
                MLX_LOG("Phase 2 FAIL: DMA/UAR init");
            } else if (!AllocPd()) {
                MLX_LOG("Phase 2 FAIL: ALLOC_PD");
            } else if (!AllocXrcd()) {
                MLX_LOG("Phase 2 FAIL: ALLOC_XRCD");
            } else {
                uint32_t uarIdx = 0;
                if (ivars->fUAR->AllocUAR(&uarIdx) != kIOReturnSuccess) {
                    MLX_LOG("Phase 2 FAIL: ALLOC_UAR");
                } else {
                    /* CREATE_EQ — the foundation of phase 2: events (PAGE_REQUEST etc.)
                     * arrive through the EQ ring. Created AFTER AllocUAR (needs
                     * uar_page) and BEFORE the verbs objects that will consume
                     * events (notes/34 §5.4). */
                    ivars->fEQ = new MlxEQ();
                    if (ivars->fEQ &&
                        ivars->fEQ->Init(this, 0) == kIOReturnSuccess) {
                        uint32_t eqn = 0;
                        if (ivars->fEQ->CreateEQ(&eqn) != kIOReturnSuccess) {
                            MLX_LOG("Phase 2 WARN: CREATE_EQ failed — events unavailable");
                            ivars->fEQ->Free(); delete ivars->fEQ; ivars->fEQ = NULL;
                        } else {
                            MLX_LOG("CREATE_EQ ok — eqn=%u (poll-mode)", eqn);
                        }
                    } else {
                        if (ivars->fEQ) { delete ivars->fEQ; ivars->fEQ = NULL; }
                    }

                    ivars->fRoCE = new MlxRoCE();
                    if (!ivars->fRoCE ||
                        ivars->fRoCE->Init(this, ivars->fHCA) != kIOReturnSuccess) {
                        MLX_LOG("Phase 2 FAIL: MlxRoCE init");
                    } else {
                        if (ivars->fEQ) ivars->fEQ->AddNotifier(ivars->fRoCE);
                        MLX_LOG("Phase 2 control objects ready: pd=%u uar[%u]",
                                ivars->fPd, uarIdx);

                        /* Enable RoCE on the native NIC vport — otherwise CREATE_QP
                         * fails with BAD_RESOURCE 0x15A3C9 (vport roce_en == 0). */
                        if (MlxEnableVportRoce(this) != kIOReturnSuccess)
                            MLX_LOG("Phase 2 FAIL: native vport RoCE enable/readback");

                        /* Port state + GID self-test (Phase 2 gates). */
                        struct mlx_query_port_resp portResp = {};
                        ivars->fRoCE->QueryPort(&portResp);
                        MLX_LOG("SelfTest: port state=%u", portResp.portState);
                        if (ivars->fEQ) ivars->fEQ->Poll();

                        /* Placeholder RoCEv2 GID/MAC — only a check of
                         * SET/QUERY_ROCE_ADDRESS on the wire; the real address
                         * will come from userspace policy when connecting to DGX. */
                        uint8_t gid[16] = {0xfe,0x80,0,0,0,0,0,0,
                                           0x02,0x00,0x00,0xff,0xfe,0x00,0x00,0x01};
                        uint8_t mac[6]  = {0x02,0x00,0x00,0x00,0x00,0x01};
                        uint32_t gidIdx = ivars->fRoCE->GetGID()->AllocGIDIndex();
                        kern_return_t gidKr = ivars->fRoCE->GetGID()->SetGID(
                            gidIdx, gid, mac, MLX_ROCE_VERSION_2, 1, false, 0);
                        MLX_LOG("SelfTest: GID[%u] staged -> 0x%x", gidIdx, gidKr);

                        /* ---- Verbs self-test (local, no peer):
                         * CREATE_CQ → CREATE_QP → RST→INIT (notes/34 §5.7). ---- */
                        struct mlx_create_cq_resp cqResp = {};
                        if (ivars->fRoCE->CreateCQ(256, &cqResp) != kIOReturnSuccess) {
                            MLX_LOG("SelfTest: CREATE_CQ FAILED");
                        } else {
                            MLX_LOG("SelfTest: CQ[%u] created (log=%u)",
                                    cqResp.cqHandle, cqResp.logSize);
                            struct mlx_create_qp_req qpReq = {};
                            qpReq.pd     = ivars->fPd;
                            qpReq.sendCq = cqResp.cqHandle;
                            qpReq.recvCq = cqResp.cqHandle;
                            qpReq.qpType = 0;      /* RC */
                            qpReq.sqSize = 256;
                            qpReq.rqSize = 256;
                            struct mlx_create_qp_resp qpResp = {};
                            bool qpTransitionOk = false;
                            bool qpDestroyOk = false;
                            if (ivars->fRoCE->CreateQP(&qpReq, &qpResp) != kIOReturnSuccess) {
                                MLX_LOG("SelfTest: CREATE_QP FAILED");
                            } else {
                                MLX_LOG("SelfTest: QP[%u] created (RC sq=256 rq=256)",
                                        qpResp.qpn);
                                struct mlx_modify_qp_req mod = {};
                                mod.qpn      = qpResp.qpn;
                                mod.curState = MLX_QP_STATE_RST;
                                mod.newState = MLX_QP_STATE_INIT;
                                mod.pkeyIndex = 0;
                                mod.portNum  = 1;
                                if (ivars->fRoCE->ModifyQP(&mod) == kIOReturnSuccess) {
                                    qpTransitionOk = true;
                                    MLX_LOG("SelfTest: QP[%u] RST→INIT ok", qpResp.qpn);
                                } else {
                                    MLX_LOG("SelfTest: ModifyQP RST→INIT FAILED");
                                }
                                qpDestroyOk = ivars->fRoCE->DestroyQP(qpResp.qpn) ==
                                              kIOReturnSuccess;
                            }
                            bool cqDestroyOk = ivars->fRoCE->DestroyCQ(cqResp.cqHandle) ==
                                               kIOReturnSuccess;
                            /* Remove the placeholder GID so the policy daemon
                             * doesn't inherit the test address. */
                            bool gidDeleteOk = ivars->fRoCE->GetGID()->DelGID(gidIdx) ==
                                               kIOReturnSuccess;
                            ivars->fPhase2ObjectsOk =
                                gidKr == kIOReturnSuccess &&
                                qpTransitionOk && qpDestroyOk && cqDestroyOk && gidDeleteOk;
                            MLX_LOG("SelfTest: full object/address lifecycle %s",
                                    ivars->fPhase2ObjectsOk ? "verified" : "FAILED");
                        }

                        /* RDMA datapath loopback: WQE→doorbell→CQE on self.
                         * Best-effort, fail-closed — no user client needed. */
                        MlxLoopbackDatapathTest(this);
                    }
                }
            }
        } else {
            MLX_LOG("Phase 1 PARTIAL: FwInit failed (see log above)");
        }
    }

    bool eqPollerReady = false;
    if (ivars->fEQ && ivars->fRoCE) {
        ivars->fHealth = new MlxHealth();
        if (ivars->fHealth &&
            ivars->fHealth->Init(this) == kIOReturnSuccess)
            eqPollerReady = StartEqPoller() == kIOReturnSuccess;
    }
    if (!eqPollerReady) {
        ivars->fRuntimePagesOk = false;
        MLX_LOG("Phase 2 FAIL: continuous EQ poller unavailable");
    }

    /* Final fail-closed readiness check. Verbs self-test above runs before
     * publication; clients may create resources only after every persistent
     * dependency is present and both port/vport state read back correctly. */
    bool phase2Ready = eqPollerReady && ivars->fPhase2ObjectsOk &&
                       ivars->fEQ && ivars->fRoCE &&
                       ivars->fHealth && ivars->fHealth->IsHealthy() &&
                       ivars->fDMA && ivars->fUAR && ivars->fPd &&
                       ivars->fXrcd &&
                       MlxEnableVportRoce(this) == kIOReturnSuccess;
    if (phase2Ready) {
        struct mlx_query_port_resp port = {};
        phase2Ready = ivars->fRoCE->QueryPort(&port) == kIOReturnSuccess &&
                      port.portState == 1;
    }
    if (!phase2Ready) {
        ivars->fRuntimePagesOk = false;
        MLX_LOG("Phase 2 NOT READY: persistent dependency/readback gate failed");
    }

    /* Register so userspace can find us via IOServiceMatching. */
    SetName("MlxPCIDriver");
    RegisterService();

    bool pageAccountingOk = ivars->fFwPages ? ivars->fFwPages->ValidateAccounting() : true;
    if ((ivars->fRoCE && ivars->fRoCE->PageHealthFailed()) || !pageAccountingOk)
        ivars->fRuntimePagesOk = false;
    MLX_LOG("Start complete (build p3-cq-arm-1) — device 0x%04x, HCA init done, phase2=%s fw_owned=%u ambiguous=%u host=%u returned=%u accounting=%s",
            ivars->fDeviceId, ivars->fRuntimePagesOk ? "ok" : "degraded",
            ivars->fFwPages ? ivars->fFwPages->GetFirmwareOwned() : 0,
            ivars->fFwPages ? ivars->fFwPages->GetAmbiguousOwned() : 0,
            ivars->fFwPages ? ivars->fFwPages->GetHostAllocated() : 0,
            ivars->fFwPages ? ivars->fFwPages->GetReturnedCount() : 0,
            pageAccountingOk ? "ok" : "BROKEN");
    return kIOReturnSuccess;
}

kern_return_t
MlxPCIDriver::Stop_Impl(IOService * provider)
{
    MLX_LOG("Stop — graceful teardown");
    ivars->fStopping = true;
    StopEqPoller();

    /* [FIX v0.36] Before graceful teardown, check whether the PCI BAR is alive
     * (MSE set). IOPCIFamily may clear MSE/BME BEFORE Stop, and then
     * MemoryWrite32 to the doorbell (DisableHca/TeardownHca → Exec) crashes
     * with SIGSEGV — as happened in 0.35 (notes/35, crash report
     * MlxRDMA-2026-08-25-151426.ips). If MSE=0 — BAR is not accessible,
     * fw will be reset by IOPCIFamily's FLR on the next match anyway.
     * Just clean up in-memory state and exit. */
    uint16_t cmdReg = 0;
    bool barAlive = false;
    if (ivars->fPci) {
        ivars->fPci->ConfigurationRead16(4, &cmdReg);
        barAlive = (cmdReg & 0x2) != 0;  /* bit 1 = MSE */
    }
    MLX_LOG("Stop: PCI cmd=0x%04x MSE=%d — BAR %s", cmdReg, barAlive ? 1 : 0,
             barAlive ? "accessible" : "INACCESSIBLE — skipping MMIO teardown");

    if (barAlive) {
        /* Destroy verbs objects before the CQ/UAR/PD dependencies. */
        if (ivars->fRoCE) {
            ivars->fRoCE->Free(); delete ivars->fRoCE; ivars->fRoCE = NULL;
        }
            DestroyPhase2FirmwareResources();
        if (DmaQuarantined()) {
            /* An object may still DMA through every resource below it.  Keep
             * firmware pages and all descriptors pinned; the next verified
             * FLR is responsible for ending DMA ownership. */
            if (ivars->fFwPages) ivars->fFwPages->EnterQuarantine();
            MLX_LOG("Stop: unverified firmware object — graceful dependency teardown skipped");
            ivars->fHcaInitialized = false;
            ivars->fHcaEnabled = false;
        } else {
            if (ivars->fHcaInitialized) {
                MLX_LOG("Stop: TEARDOWN_HCA (graceful)");
                TeardownHca();
                ivars->fHcaInitialized = false;
            }
            if (ivars->fFwPages) {
                /* Real reclaim: TAKE firmware pages, then free mappings. */
                MLX_LOG("Stop: ReclaimAll (TAKE pages back)");
                kern_return_t kr = ivars->fFwPages->ReclaimAll();
                if (kr != kIOReturnSuccess)
                    MLX_LOG("Stop: ReclaimAll incomplete — mappings retained (quarantine)");
            }
            if (ivars->fHcaEnabled) {
                MLX_LOG("Stop: DISABLE_HCA (graceful)");
                DisableHca();
                ivars->fHcaEnabled = false;
            }
        }
    } else {
        /* BAR dead — clean up only memory, fw lives its own life. Reclaim
         * impossible (no TAKE without BAR): quarantine mode, keep mappings. */
        EnterDmaQuarantine(0x42415200u); /* 'BAR': no firmware command path */
        if (ivars->fFwPages) {
            ivars->fFwPages->EnterQuarantine();
            MLX_LOG("Stop: BAR dead — firmware pages quarantined (mappings retained)");
        }
        ivars->fHcaInitialized = false;
        ivars->fHcaEnabled = false;
    }

    Cleanup();
    if (ivars->fPci)
        ivars->fPci->Close(this, 0);
    /* SUPERDISPATCH releases the cached provider.  Do not keep a borrowed
     * pointer that free()/Cleanup() could use after that boundary. */
    ivars->fPci = NULL;
    return Stop(provider, SUPERDISPATCH);
}

kern_return_t
MlxPCIDriver::NewUserClient_Impl(uint32_t type, IOUserClient **userClient)
{
    MLX_LOG("NewUserClient: ENTER type=%u", type);
    if (!userClient) return kIOReturnBadArgument;
    *userClient = NULL;
    if (type != 0) return kIOReturnUnsupported;

    /* IOService::Create consumes the named property dictionary from our
     * personality and instantiates its IOUserClass in this DriverKit process. */
    IOService *client = NULL;
    kern_return_t kr = Create(this, "MlxRDMAUserClient", &client);
    if (kr != kIOReturnSuccess || !client) {
        MLX_LOG("NewUserClient: Create failed type=%u kr=0x%x", type, kr);
        return kr ? kr : kIOReturnError;
    }
    *userClient = OSDynamicCast(IOUserClient, client);
    if (!*userClient) {
        MLX_LOG("NewUserClient: created service is not IOUserClient");
        client->release();
        return kIOReturnError;
    }
    MLX_LOG("NewUserClient: created type=%u", type);
    return kIOReturnSuccess;
}

/* Firmware command shortcut — delegates to MlxCmd. */
kern_return_t
MlxPCIDriver::Exec(uint32_t opcode, const void *in, uint32_t inSize,
                   void *out, uint32_t outSize, uint32_t timeoutMs)
{
    if (!ivars->fCmd)
        return kIOReturnNotReady;
    kern_return_t kr = ivars->fCmd->Exec(opcode, in, inSize, out, outSize, timeoutMs);
    /* Drain the async EQ after every command (poll-mode, MSI-X not wired yet).
     * PAGE_REQUEST is enqueued inside the EQ handler and serviced here, after
     * fully draining the ring — without a nested Poll. */
    if (ivars->fEQ)
        ivars->fEQ->Poll();
    if (ivars->fRoCE) {
        kern_return_t pageKr = ivars->fRoCE->DrainPendingPageRequests();
        /* Runtime pages error → degraded (blocks datapath). */
        if (pageKr != kIOReturnSuccess) ivars->fRuntimePagesOk = false;
    }
    return kr;
}

/* ---- LOCALONLY accessors (iig emits declarations only; state lives in the
 * IVars struct above, not in the .iig member list which is documentation). ---- */
IOPCIDevice *         MlxPCIDriver::GetPCI()           { return ivars->fPci; }
MlxCmd *              MlxPCIDriver::GetCmd()           { return ivars->fCmd; }
MlxEQ *               MlxPCIDriver::GetEQ()            { return ivars->fEQ; }
MlxUAR *              MlxPCIDriver::GetUAR()           { return ivars->fUAR; }
MlxHCA *              MlxPCIDriver::GetHCA()           { return ivars->fHCA; }
MlxHealth *           MlxPCIDriver::GetHealth()        { return ivars->fHealth; }
MlxDMA *              MlxPCIDriver::GetDMA()           { return ivars->fDMA; }
MlxFwPages *          MlxPCIDriver::GetPages()         { return ivars->fFwPages; }
MlxRoCE *             MlxPCIDriver::GetRoCE()          { return ivars->fRoCE; }
IOMemoryDescriptor *  MlxPCIDriver::GetBar0Memory()    { return ivars->fBar0Mem; }
uint8_t               MlxPCIDriver::GetBar0Index()     { return ivars->fBar0Index; }
uint32_t              MlxPCIDriver::GetIssi() const    { return ivars->fIssi; }
uint32_t              MlxPCIDriver::GetPd() const      { return ivars->fPd; }
uint32_t              MlxPCIDriver::GetXrcd() const    { return ivars->fXrcd; }
uint32_t              MlxPCIDriver::GetDevIdx() const  { return ivars->fDevIdx; }
const char *          MlxPCIDriver::GetDevName() const { return ivars->fDevName; }

bool
MlxPCIDriver::RoCEPublicationAllowed() const
{
    /* RoCE stays fail-closed until per-client isolation (REMEDIATION_PLAN §7.4). */
    return false;
}

bool
MlxPCIDriver::Phase2Ready() const
{
    return ivars && ivars->fRuntimePagesOk && ivars->fPhase2ObjectsOk &&
           ivars->fRoCE && ivars->fEQ &&
           ivars->fHealth && ivars->fHealth->IsHealthy() &&
           !ivars->fDmaQuarantined &&
           ivars->fEqTimer && ivars->fPd && ivars->fXrcd;
}

bool
MlxPCIDriver::DmaQuarantined() const
{
    return ivars && ivars->fDmaQuarantined;
}

void
MlxPCIDriver::RetainDmaUntilReset(void *memPtr, void *dmaPtr, uint32_t reason)
{
    IOMemoryDescriptor *mem = static_cast<IOMemoryDescriptor *>(memPtr);
    IODMACommand *dma = static_cast<IODMACommand *>(dmaPtr);
    if (!mem && !dma) return;
    EnterDmaQuarantine(reason);
    MlxRetainQuarantinedDummy(mem, dma);
}

void
MlxPCIDriver::EnterDmaQuarantine(uint32_t reason)
{
    ivars->fDmaQuarantined = true;
    MLX_LOG("DMA quarantine entered: reason=0x%x — retaining mappings", reason);
}

/* PCIe Function Level Reset + wait for firmware load.
 * After killing the dext without DISABLE_HCA, firmware stays in a poisoned
 * state (delivery 6 on everything, cmdif hangs). FLR reloads it without a
 * power cycle and WITHOUT changing the card owner (notes/29, notes/31).
 *
 * v0.35+: fixed the order — MSE|BME are restored BEFORE waiting for fw_rev,
 * otherwise MemoryRead32(BAR0) always returns 0xFFFFFFFF (notes/35).
 * Added diagnostic dumps: config before/after FLR, PCIe cap scan,
 * fw_rev timing. */
bool
MlxPCIDriver::PerformFlr()
{
    uint8_t capPtr = 0;
    ivars->fPci->ConfigurationRead8(0x34, &capPtr);
    uint8_t pcieCap = 0;

    /* Diagnostic: dump the PCIe capability list (notes/35). */
    MLX_DBG("DBG PerformFlr: capPtr=0x%02x", capPtr);

    /* A card in reset reads 0xFF: capPtr=0xFF, idNext=0xFFFF → capPtr again
     * 0xFF — without a limit this is an infinite loop (spin RPC into the
     * kernel, found via sample, notes/32). Maximum 48 iterations, 0xFF is
     * also an exit. */
    for (uint32_t i = 0; i < 48 && capPtr && capPtr >= 0x40 && capPtr != 0xFF; i++) {
        uint16_t idNext = 0;
        ivars->fPci->ConfigurationRead16(capPtr, &idNext);
        MLX_DBG("DBG PerformFlr: cap[0x%02x]=0x%04x", capPtr, idNext);
        if (idNext == 0xFFFF) break;   /* card in reset */
        if ((idNext & 0xFF) == 0x10 /*PCIe*/) { pcieCap = capPtr; break; }
        capPtr = (uint8_t)((idNext >> 8) & 0xFF);
    }
    if (!pcieCap) {
        MLX_DBG("DBG PerformFlr: PCIe CAP not found (capPtr=0x%02x)", capPtr);
        return false;
    }

    /* Diagnostic: dump device capabilities. */
    uint32_t devCap = 0;
    ivars->fPci->ConfigurationRead32(pcieCap + 4, &devCap);
    MLX_DBG("DBG PerformFlr: pcieCap=0x%02x devCap=0x%08x FLR_supported=%d",
            pcieCap, devCap, (devCap >> 28) & 1);
    if (!(devCap & (1u << 28))) {
        MLX_DBG("DBG PerformFlr: FLR not supported");
        return false;
    }

    /* Diagnostic: config before FLR. */
    uint32_t ctlBefore = 0;
    ivars->fPci->ConfigurationRead32(pcieCap + 8, &ctlBefore);
    uint16_t cmdBefore = 0;
    ivars->fPci->ConfigurationRead16(4, &cmdBefore);
    uint32_t devIdBefore = 0;
    ivars->fPci->ConfigurationRead32(0, &devIdBefore);
    uint32_t fwRevBefore = 0;
    ivars->fPci->MemoryRead32(ivars->fBar0Index, 0, &fwRevBefore);
    fwRevBefore = OSSwapBigToHostInt32(fwRevBefore);
    MLX_DBG("DBG PerformFlr: before FLR ctl=0x%08x cmd=0x%04x devId=0x%08x fw_rev=0x%08x",
            ctlBefore, cmdBefore, devIdBefore, fwRevBefore);

    /* FLR: write initiator bit. */
    ivars->fPci->ConfigurationWrite32(pcieCap + 8, ctlBefore | (1u << 15));

    /* Wait for self-clearing of the FLR bit (up to 2s). */
    bool flrDone = false;
    for (uint32_t i = 0; i < 100; i++) {
        IOSleep(20);
        uint32_t c = 0;
        ivars->fPci->ConfigurationRead32(pcieCap + 8, &c);
        if (!(c & (1u << 15))) { flrDone = true; break; }
        if (i == 0 || i == 49 || i == 99)
            MLX_DBG("DBG PerformFlr: FLR poll i=%u ctl=0x%08x", i, c);
    }
    MLX_DBG("DBG PerformFlr: FLR self-clear: %s (%u ms)",
            flrDone ? "OK" : "TIMEOUT", flrDone ? 0 : 2000);
    if (!flrDone) return false;

    /* Diagnostic: config after FLR (before restoring MSE). */
    uint32_t devIdAfter = 0;
    ivars->fPci->ConfigurationRead32(0, &devIdAfter);
    uint16_t cmdAfter = 0;
    ivars->fPci->ConfigurationRead16(4, &cmdAfter);
    uint32_t ctlAfter = 0;
    ivars->fPci->ConfigurationRead32(pcieCap + 8, &ctlAfter);
    MLX_DBG("DBG PerformFlr: after FLR (before MSE): devId=0x%08x cmd=0x%04x ctl=0x%08x",
            devIdAfter, cmdAfter, ctlAfter);

    /* [FIX v0.35] Restore MSE|BME BEFORE waiting for fw_rev (notes/35).
     * FLR clears MSE — without it MemoryRead32(BAR0) returns 0xFFFFFFFF
     * regardless of fw state, and the wait loop always burns 15s in vain. */
    uint32_t cmd32 = cmdAfter;
    ivars->fPci->ConfigurationWrite32(4, cmd32 | 0x6);
    IOSleep(10);  /* let the register settle */
    ivars->fPci->ConfigurationRead32(4, &cmd32);
    MLX_DBG("DBG PerformFlr: MSE|BME restored: cmd=0x%04x", (uint16_t)cmd32);

    /* Now wait for fw_rev (MemoryRead32 will work — MSE on). */
    uint32_t fw = 0xFFFFFFFF;
    uint32_t waitMs = 0;
    for (uint32_t i = 0; i < 120; i++) {  /* up to 60s */
        uint32_t revBE = 0;
        ivars->fPci->MemoryRead32(ivars->fBar0Index, 0, &revBE);
        fw = OSSwapBigToHostInt32(revBE);
        if (fw != 0 && fw != 0xFFFFFFFF) {
            if (i > 0) MLX_DBG("DBG PerformFlr: fw_rev=0x%08x at i=%u", fw, i);
            break;
        }
        if (i == 0 || i % 20 == 0)
            MLX_DBG("DBG PerformFlr: waiting fw_rev i=%u fw=0x%08x", i, fw);
        IOSleep(500);
        waitMs += 500;
    }
    MLX_DBG("DBG PerformFlr: fw_rev=0x%08x (waited %u ms)", fw, waitMs);
    uint32_t devIdVerified = 0;
    ivars->fPci->ConfigurationRead32(0, &devIdVerified);
    bool deviceAlive = (devIdVerified != 0 && devIdVerified != 0xFFFFFFFF);
    bool firmwareAlive = (fw != 0 && fw != 0xFFFFFFFF);
    if (!deviceAlive || !firmwareAlive) {
        MLX_LOG("PerformFlr: verification FAILED devId=0x%08x fw_rev=0x%08x",
                devIdVerified, fw);
        return false;
    }
    MlxReleaseQuarantinedPagesAfterReset();
    ivars->fDmaQuarantined = false;
    MLX_LOG("PerformFlr: verified DMA boundary");
    return true;
}

/* Restart the firmware init chain WITHOUT changing the card owner:
 * IOPCIFamily performs FLR only when handing resources to a NEW dext process
 * (log: "[getResources()] Performing FLR on nub ethernet"), so the dev cycle
 * needs no kill/rematch — the driver FLRs the card itself and restarts
 * FwInit in the same process (notes/31). */
kern_return_t
MlxPCIDriver::ReinitFw()
{
    MLX_LOG("ReinitFw: restarting firmware init (owner unchanged)");
    StopEqPoller();
    ivars->fPhase2ObjectsOk = false;

    /* Teardown the firmware chain (reverse Cleanup order, without fBar0Mem).
     * First quarantine firmware pages (don't free mappings while the card may
     * write to them), then FLR stops DMA, and only after a successful FLR do
     * we free. */
    if (ivars->fRoCE)    { ivars->fRoCE->Free(); delete ivars->fRoCE; ivars->fRoCE = NULL; }
    DestroyPhase2FirmwareResources();
    if (ivars->fFwPages) ivars->fFwPages->EnterQuarantine();
    if (ivars->fDMA)     { ivars->fDMA->Free(); delete ivars->fDMA; ivars->fDMA = NULL; }
    if (ivars->fHealth)  { ivars->fHealth->Free(); delete ivars->fHealth; ivars->fHealth = NULL; }
    if (ivars->fDmaQuarantined && ivars->fUAR) {
        IOBufferMemoryDescriptor *dbMem = NULL;
        IODMACommand *dbDma = NULL;
        ivars->fUAR->QuarantineDbPage(&dbMem, &dbDma);
        RetainDmaUntilReset(dbMem, dbDma, 0x44425249u); /* 'DBRI' */
    }
    if (ivars->fUAR)     { ivars->fUAR->Free(); delete ivars->fUAR; ivars->fUAR = NULL; }
    if (ivars->fEQ)      { ivars->fEQ->Free(); delete ivars->fEQ; ivars->fEQ = NULL; }
    if (ivars->fCmd)     { ivars->fCmd->Free(); delete ivars->fCmd; ivars->fCmd = NULL; }

    /* Fresh firmware: self-FLR + wait for load. */
    if (!PerformFlr()) {
        MLX_LOG("ReinitFw: FLR verification failed — quarantine retained");
        return kIOReturnTimeout;
    }

    /* After FLR, DMA from the card side has stopped — mappings can be freed. */
    if (ivars->fFwPages) {
        ivars->fFwPages->ReleaseQuarantineAfterReset();
        ivars->fFwPages->Free();
        delete ivars->fFwPages; ivars->fFwPages = NULL;
    }

    /* Full FwInit on clean firmware. */
    ivars->fCmd = new MlxCmd();
    kern_return_t kr = ivars->fCmd->Init(this);
    if (kr != kIOReturnSuccess) {
        MLX_LOG("ReinitFw: MlxCmd::Init 0x%x", kr);
        delete ivars->fCmd; ivars->fCmd = NULL;
        return kr;
    }
    MLX_LOG("ReinitFw: MlxCmd ready");
    if (!FwInit()) {
        MLX_LOG("ReinitFw: FwInit failed (see log above)");
        return kIOReturnIOError;
    }
    if (!InitPhase2Runtime()) {
        MLX_LOG("ReinitFw: Phase 2 rebuild failed");
        return kIOReturnIOError;
    }
    MLX_LOG("ReinitFw: SUCCESS — init chain complete");
    return kIOReturnSuccess;
}

/* Stable-driver gate: repeat the Linux function close/open sequence without
 * resetting firmware, replacing the command queue, or changing ownership.
 * The old firmware EQ remains alive through TEARDOWN_HCA long enough to
 * capture and service signed PAGE_REQUEST events. */
kern_return_t
MlxPCIDriver::StableInitCycle(struct mlx_stable_init_cycle_resp *report)
{
    if (!report || !ivars) return kIOReturnBadArgument;
    memset(report, 0, sizeof(*report));
    report->cycle = ++ivars->fStableCycleCount;
    if (ivars->fStableCycleActive) {
        report->kr = kIOReturnBusy;
        return kIOReturnBusy;
    }
    if (!ivars->fCmd || !ivars->fFwPages || !ivars->fHcaInitialized ||
        !Phase2Ready() || DmaQuarantined()) {
        report->kr = kIOReturnNotReady;
        return kIOReturnNotReady;
    }

    ivars->fStableCycleActive = true;
    report->fwRevBefore = mlxMMIORead32BE(ivars->fPci, ivars->fBar0Index, 0);
    report->cmdqIOVABefore = ivars->fCmd->CmdqIOVA();
    report->swOwnerIdSupported = ivars->fSwOwnerIdSupported ? 1 : 0;
    memcpy(report->swOwnerId, ivars->fSwOwnerId, sizeof(report->swOwnerId));
    report->fwOwnedBefore = ivars->fFwPages->GetFirmwareOwned();
    uint32_t negReqBefore = ivars->fFwPages->GetNegativeTakeRequests();
    uint32_t negPagesBefore = ivars->fFwPages->GetNegativeTakePages();
    uint32_t negReturnedBefore = ivars->fFwPages->GetNegativeTakeReturned();

    MLX_LOG("STABLE_GATE[%u]: BEGIN no-FLR close/open cmdq=0x%llx fw=0x%08x fw_owned=%u owner_cap=%u owner=%08x:%08x:%08x:%08x",
            report->cycle, (unsigned long long)report->cmdqIOVABefore,
            report->fwRevBefore, report->fwOwnedBefore,
            report->swOwnerIdSupported, report->swOwnerId[0],
            report->swOwnerId[1], report->swOwnerId[2], report->swOwnerId[3]);

    ivars->fRuntimePagesOk = false;
    ivars->fPhase2ObjectsOk = false;
    StopEqPoller();

    /* No external handles are allowed by the UserClient dispatch. Destroy
     * verbs children, but keep MlxRoCE as the page-event notifier until the
     * firmware close is complete. */
    if (ivars->fRoCE) ivars->fRoCE->QuiesceVerbsResources();

    kern_return_t cycleKr = kIOReturnSuccess;
    auto failStage = [&](uint32_t stage, kern_return_t error) {
        if (cycleKr == kIOReturnSuccess) {
            report->failureStage = stage;
            cycleKr = error;
        }
    };
    if (!TeardownHca()) {
        failStage(MLX_STABLE_STAGE_TEARDOWN, kIOReturnIOError);
    } else {
        ivars->fHcaInitialized = false;
        report->teardownOk = 1;
        /* Capture any firmware-generated signed negative request first. This
         * ConnectX firmware does not normally emit one for TEARDOWN_HCA, so
         * after the bounded observation window the driver performs Linux's
         * explicit reclaim_startup_pages TAKE sequence below. */
        for (uint32_t i = 0; i < 50; i++) {
            if (ivars->fEQ) ivars->fEQ->Poll();
            kern_return_t pageKr = ivars->fRoCE ?
                ivars->fRoCE->DrainPendingPageRequests() : kIOReturnNotReady;
            if (pageKr != kIOReturnSuccess && pageKr != kIOReturnNotReady) {
                failStage(MLX_STABLE_STAGE_EVENT_DRAIN, pageKr);
                break;
            }
            IOSleep(10);
        }
    }

    report->negativeTakeRequests =
        ivars->fFwPages->GetNegativeTakeRequests() - negReqBefore;
    report->negativeTakePages =
        ivars->fFwPages->GetNegativeTakePages() - negPagesBefore;
    report->negativeTakeReturned =
        ivars->fFwPages->GetNegativeTakeReturned() - negReturnedBefore;
    MLX_LOG("STABLE_GATE[%u]: TEARDOWN ok=%u negative_take requests=%u pages=%u returned=%u fw_owned=%u ambiguous=%u",
            report->cycle, report->teardownOk, report->negativeTakeRequests,
            report->negativeTakePages, report->negativeTakeReturned,
            ivars->fFwPages->GetFirmwareOwned(),
            ivars->fFwPages->GetAmbiguousOwned());

    /* Linux function_disable(): reclaim startup/runtime pages explicitly,
     * then DISABLE_HCA. A negative PAGE_REQUEST is one trigger for the same
     * TAKE machinery, but it is not generated by this firmware on teardown. */
    if (cycleKr == kIOReturnSuccess) {
        kern_return_t reclaimKr = ivars->fFwPages->ReclaimAll(
            &report->reclaimRequested, &report->reclaimReturned);
        if (reclaimKr != kIOReturnSuccess ||
            report->reclaimRequested != report->reclaimReturned)
            failStage(MLX_STABLE_STAGE_RECLAIM,
                      reclaimKr != kIOReturnSuccess ? reclaimKr : kIOReturnIOError);
        MLX_LOG("STABLE_GATE[%u]: explicit TAKE requested=%u returned=%u",
                report->cycle, report->reclaimRequested,
                report->reclaimReturned);
    }
    if (cycleKr == kIOReturnSuccess && !DisableHca())
        failStage(MLX_STABLE_STAGE_DISABLE, kIOReturnIOError);

    if (report->teardownOk) {
        /* TEARDOWN_HCA is the firmware boundary for every HCA object. Do not
         * send individual DESTROY/FREE commands after it; just clear local
         * firmware identities and release their now-quiesced DMA mappings. */
        if (ivars->fRoCE) {
            ivars->fRoCE->Free(); delete ivars->fRoCE; ivars->fRoCE = NULL;
        }
        if (ivars->fEQ) {
            ivars->fEQ->MarkDestroyedByTeardown();
            ivars->fEQ->Free(); delete ivars->fEQ; ivars->fEQ = NULL;
        }
        if (ivars->fUAR) {
            ivars->fUAR->MarkFirmwareResourcesDestroyedByTeardown();
            ivars->fUAR->Free(); delete ivars->fUAR; ivars->fUAR = NULL;
        }
        if (ivars->fDMA) {
            ivars->fDMA->Free(); delete ivars->fDMA; ivars->fDMA = NULL;
        }
        if (ivars->fHealth) {
            ivars->fHealth->Free(); delete ivars->fHealth; ivars->fHealth = NULL;
        }
        ivars->fPd = 0;
        ivars->fXrcd = 0;

        /* Linux function_enable() + function_open() on the same firmware and
         * command queue: ENABLE -> ISSI -> boot pages -> caps -> init pages
         * -> INIT_HCA -> caps readback. */
        if (cycleKr == kIOReturnSuccess && !EnableHca())
            failStage(MLX_STABLE_STAGE_ENABLE, kIOReturnIOError);
        if (cycleKr == kIOReturnSuccess && !SetIssi())
            failStage(MLX_STABLE_STAGE_ISSI, kIOReturnIOError);

        uint32_t bootPages = 0, bootFuncId = 0;
        if (cycleKr == kIOReturnSuccess) {
            kern_return_t kr = ivars->fFwPages->QueryStartupPagesFull(
                1 /* BOOT */, &bootPages, &bootFuncId);
            if (kr != kIOReturnSuccess)
                failStage(MLX_STABLE_STAGE_BOOT_QUERY, kr);
        }
        if (cycleKr == kIOReturnSuccess && bootPages) {
            kern_return_t kr = ivars->fFwPages->ProvidePagesContig(
                bootPages, 1 /* boot */, bootFuncId);
            if (kr != kIOReturnSuccess)
                failStage(MLX_STABLE_STAGE_BOOT_GIVE, kr);
        }
        if (cycleKr == kIOReturnSuccess && !SetHcaCaps())
            failStage(MLX_STABLE_STAGE_SET_CAP, kIOReturnIOError);

        uint32_t initPages = 0, initFuncId = 0;
        if (cycleKr == kIOReturnSuccess) {
            kern_return_t kr = ivars->fFwPages->QueryStartupPagesFull(
                2 /* INIT */, &initPages, &initFuncId);
            if (kr != kIOReturnSuccess)
                failStage(MLX_STABLE_STAGE_INIT_QUERY, kr);
        }
        if (cycleKr == kIOReturnSuccess && initPages) {
            MLX_LOG("STABLE_GATE[%u]: firmware requests %u replacement INIT pages func=%u",
                    report->cycle, initPages, initFuncId);
            kern_return_t kr = ivars->fFwPages->ProvidePagesContig(
                initPages, 2 /* init */, initFuncId);
            if (kr != kIOReturnSuccess)
                failStage(MLX_STABLE_STAGE_INIT_GIVE, kr);
        }
        if (cycleKr == kIOReturnSuccess) {
            if (InitHca()) report->initOk = 1;
            else failStage(MLX_STABLE_STAGE_INIT_HCA, kIOReturnIOError);
        }
        if (cycleKr == kIOReturnSuccess && !QueryHcaCaps())
            failStage(MLX_STABLE_STAGE_QUERY_CAP, kIOReturnIOError);
        if (cycleKr == kIOReturnSuccess && InitPhase2Runtime()) {
            report->phase2Ok = 1;
        } else if (cycleKr == kIOReturnSuccess) {
            report->phase2SubStage = ivars->fPhase2SubStage;
            report->phase2Ret = ivars->fPhase2Ret;
            failStage(MLX_STABLE_STAGE_PHASE2, kIOReturnIOError);
        }
    }

    report->fwRevAfter = mlxMMIORead32BE(ivars->fPci, ivars->fBar0Index, 0);
    report->cmdqIOVAAfter = ivars->fCmd ? ivars->fCmd->CmdqIOVA() : 0;
    report->fwOwnedAfter = ivars->fFwPages ?
        ivars->fFwPages->GetFirmwareOwned() : 0;
    report->ambiguousAfter = ivars->fFwPages ?
        ivars->fFwPages->GetAmbiguousOwned() : 0;
    report->accountingOk = ivars->fFwPages &&
        ivars->fFwPages->ValidateAccounting() ? 1 : 0;

    if (cycleKr == kIOReturnSuccess &&
        (!report->teardownOk || !report->initOk || !report->phase2Ok ||
         report->fwRevBefore != report->fwRevAfter ||
         report->cmdqIOVABefore != report->cmdqIOVAAfter ||
         report->ambiguousAfter || !report->accountingOk))
        failStage(MLX_STABLE_STAGE_VERIFY, kIOReturnIOError);

    /* A failed experimental close/open must not strand the live workstation.
     * Recover through the already-proven FLR path, while keeping the report a
     * failure so the no-FLR stable gate cannot pass accidentally. */
    if (ivars->fCmd) {
        if (report->failureStage == MLX_STABLE_STAGE_PHASE2) {
            /* The phase2 fail path ran cleanup (DEALLOC_XRCD/DEALLOC_PD) after
             * the failure, overwriting the command plane's last-opcode state.
             * Use the snapshot taken at the failure point instead — this is
             * the opcode/fw_status/syndrome of the *failing* command. */
            report->lastOpcode = ivars->fPhase2Opcode;
            report->lastDeliveryStatus = ivars->fPhase2DeliveryStatus;
            report->lastFwStatus = ivars->fPhase2FwStatus;
            report->lastSyndrome = ivars->fPhase2Syndrome;
        } else {
            report->lastOpcode = ivars->fCmd->LastOpcode();
            report->lastDeliveryStatus = ivars->fCmd->LastDeliveryStatus();
            report->lastFwStatus = ivars->fCmd->LastFwStatus();
            report->lastSyndrome = ivars->fCmd->LastSyndrome();
        }
    }
    if (cycleKr != kIOReturnSuccess) {
        MLX_LOG("STABLE_GATE[%u]: FAIL kr=0x%x — recovering with verified FLR",
                report->cycle, cycleKr);
        if (ReinitFw() == kIOReturnSuccess)
            report->recoveredWithFlr = 1;
    }

    report->kr = cycleKr;
    ivars->fStableCycleActive = false;
    MLX_LOG("STABLE_GATE[%u]: %s stage=%u teardown=%u init=%u phase2=%u cmdq_same=%u fw_same=%u accounting=%u firmware_negative=%u/%u/%u explicit_reclaim=%u/%u recovered_flr=%u phase2_sub=%u phase2_ret=0x%x",
            report->cycle, cycleKr == kIOReturnSuccess ? "PASS" : "FAIL",
            report->failureStage, report->teardownOk, report->initOk,
            report->phase2Ok,
            report->cmdqIOVABefore == report->cmdqIOVAAfter,
            report->fwRevBefore == report->fwRevAfter,
            report->accountingOk, report->negativeTakeRequests,
            report->negativeTakePages, report->negativeTakeReturned,
            report->reclaimRequested, report->reclaimReturned,
            report->recoveredWithFlr, report->phase2SubStage,
            report->phase2Ret);
    return cycleKr;
}

/* Debug: FLR only (no full FwInit) — for mlx_probe (notes/35). */
kern_return_t
MlxPCIDriver::DbgPerformFlr()
{
    if (!ivars->fPci) return kIOReturnNotAttached;
    MLX_LOG("DbgPerformFlr: FLR requested by mlx_probe");
    return PerformFlr() ? kIOReturnSuccess : kIOReturnTimeout;
}

void
MlxPCIDriver::Cleanup()
{
    /* Release in reverse dependency order. Ambiguous mappings are retained
     * (REMEDIATION_PLAN §3). */
    StopEqPoller();
    if (ivars->fRoCE)    { ivars->fRoCE->Free(); delete ivars->fRoCE; ivars->fRoCE = NULL; }

    DestroyPhase2FirmwareResources();

    if (ivars->fFwPages) {
        if (ivars->fFwPages->IsQuarantined()) {
            ivars->fFwPages->EnterQuarantine();
            MlxRetainQuarantinedPages(ivars->fFwPages);
            ivars->fFwPages = NULL;
        } else {
            ivars->fFwPages->Free();
            delete ivars->fFwPages;
            ivars->fFwPages = NULL;
        }
    }
    if (ivars->fDMA)     { ivars->fDMA->Free(); delete ivars->fDMA; ivars->fDMA = NULL; }
    if (ivars->fHealth)  { ivars->fHealth->Free(); delete ivars->fHealth; ivars->fHealth = NULL; }
    if (ivars->fDmaQuarantined && ivars->fUAR) {
        IOBufferMemoryDescriptor *dbMem = NULL;
        IODMACommand *dbDma = NULL;
        ivars->fUAR->QuarantineDbPage(&dbMem, &dbDma);
        RetainDmaUntilReset(dbMem, dbDma, 0x4442434cu); /* 'DBCL' */
    }
    if (ivars->fUAR)     { ivars->fUAR->Free(); delete ivars->fUAR; ivars->fUAR = NULL; }
    if (ivars->fEQ)      { ivars->fEQ->Free(); delete ivars->fEQ; ivars->fEQ = NULL; }
    if (ivars->fCmd)     { ivars->fCmd->Free(); delete ivars->fCmd; ivars->fCmd = NULL; }
    if (ivars->fHCA)     { delete ivars->fHCA; ivars->fHCA = NULL; }
    if (ivars->fBar0Mem) { ivars->fBar0Mem->release(); ivars->fBar0Mem = NULL; }
}

/* Phase 1 firmware-init — ENABLE_HCA → QUERY/SET_ISSI → boot pages (notes/08 §8.2). */
bool
MlxPCIDriver::FwInit()
{
    if (!ivars->fCmd) return false;
    MLX_LOG("FwInit: start (ENABLE_HCA → ISSI → boot pages)");

    /* Wait for firmware to leave the initializing state (main.c:125 wait_fw_init).
     * Without this MANAGE_PAGES gets delivery 6 (FW_ERR) — fw is still loading
     * and its page infrastructure isn't ready (notes/29). */
    {
        uint32_t waited = 0;
        uint32_t init = 0;
        uint32_t rev = 0;
        do {
            uint32_t revBE = 0;
            ivars->fPci->MemoryRead32(ivars->fBar0Index, 0, &revBE);
            rev = OSSwapBigToHostInt32(revBE);
            init = mlxMMIORead32BE(ivars->fPci, ivars->fBar0Index,
                                   offsetof(struct MlxInitSeg, initializing));
            if (rev != 0 && rev != 0xFFFFFFFF && !(init >> 31)) break;
            IOSleep(100);
            waited += 100;
        } while (waited < 15000);
        if (init >> 31 || rev == 0 || rev == 0xFFFFFFFF) {
            MLX_LOG("fw not ready after %u ms (init=0x%x fw_rev=0x%08x) — continuing", waited, init, rev);
        } else {
            MLX_LOG("fw ready (initializing cleared after %u ms, fw_rev=0x%08x)", waited, rev);
        }
    }

    /* ENABLE_HCA may return delivery 6 if the HCA was already enabled from a
     * previous dext life (after kill without DISABLE_HCA) — not fatal, continue. */
    EnableHca();

    /* ISSI negotiation BEFORE boot pages (like AppleMCX/Linux: set_issi goes
     * BEFORE giving boot pages). On failure SetIssi falls back to ISSI=0 and
     * continues — non-blocking. */
    SetIssi();

    /* Steps 7-8: QUERY_PAGES(BOOT) → MANAGE_PAGES(GIVE). */
    ivars->fFwPages = new MlxFwPages();
    if (ivars->fFwPages->Init(this) != kIOReturnSuccess) {
        MLX_LOG("MlxFwPages init failed");
        delete ivars->fFwPages; ivars->fFwPages = NULL;
        return false;
    }
    uint32_t bootPages = 0;
    uint32_t bootFuncId = 0;
    if (ivars->fFwPages->QueryStartupPagesFull(1 /*BOOT*/, &bootPages, &bootFuncId) != kIOReturnSuccess)
        return false;
    MLX_LOG("FwInit: QUERY_PAGES(BOOT) → num_pages=%u function_id=%u", bootPages, bootFuncId);

    /* Hand out boot pages. Candidate #1 (notes/35): a single 128 KiB DMA buffer
     * like the Apple driver. ProvidePages (separate 4 KiB buffers) — the old
     * path; both are logged for comparison. */
    MLX_LOG("FwInit: GIVE %u boot pages (ProvidePagesContig — single 128 KiB buffer)", bootPages);
    if (ivars->fFwPages->ProvidePagesContig(bootPages, 1 /*boot*/) != kIOReturnSuccess) {
        MLX_LOG("FwInit: ProvidePagesContig failed — trying separate buffers");
        if (ivars->fFwPages->ProvidePages(bootPages, 1 /*boot*/) != kIOReturnSuccess) {
            MLX_LOG("boot pages not handed out — stopping");
            return false;
        }
    }
    MLX_LOG("boot pages: %u handed to firmware", bootPages);

    /* SET_HCA_CAP — enable RoCE (AppleMCX donor: setHcaCaps after boot pages). */
    if (!SetHcaCaps()) {
        MLX_LOG("FwInit: SET_HCA_CAP failed");
        return false;
    }

    /* 8-byte atomics: select host-endianness request mode via SET_HCA_CAP(ATOMIC),
     * exactly like Linux handle_hca_cap_atomic(). Must run BEFORE INIT_HCA —
     * after it the HCA caps are fixed. */
    (void)SetAtomicReqEndianness();

    /* GIVE init pages (second page pool, separate buffers — chunk taken by boot). */
    uint32_t initPages = 0;
    uint32_t initFuncId = 0;
    if (ivars->fFwPages->QueryStartupPagesFull(2 /*INIT*/, &initPages, &initFuncId) != kIOReturnSuccess) {
        MLX_LOG("FwInit: QUERY_PAGES(INIT) failed");
        return false;
    }
    MLX_LOG("FwInit: QUERY_PAGES(INIT) → num_pages=%u function_id=%u", initPages, initFuncId);
    if (initPages > 0) {
        if (ivars->fFwPages->ProvidePagesContig(initPages, 2 /*init*/) != kIOReturnSuccess) {
            MLX_LOG("FwInit: GIVE init pages failed");
            return false;
        }
        MLX_LOG("FwInit: init pages %u handed out", initPages);
    }

    /* INIT_HCA — full HCA initialization. */
    if (!InitHca()) {
        MLX_LOG("FwInit: INIT_HCA failed");
        return false;
    }

    /* QUERY_HCA_CAP — read capabilities (max QP/CQ/MR, RoCE). */
    if (!QueryHcaCaps()) {
        MLX_LOG("FwInit: QUERY_HCA_CAP failed");
        return false;
    }

    MLX_LOG("Phase 1 COMPLETE: HCA enabled, ISSI=%u, boot+init pages handed out, INIT_HCA ok",
            ivars->fIssi);
    return true;
}

bool
MlxPCIDriver::EnableHca()
{
    /* ENABLE_HCA (0x104): in = {opcode, op_mod=0}; out = 8B status. */
    uint8_t in[16] = {};
    uint8_t out[16] = {};
    mlxSetBits(in, 0x00, 16, MLX_CMD_OP_ENABLE_HCA);
    kern_return_t kr = ivars->fCmd->Exec(MLX_CMD_OP_ENABLE_HCA, in, sizeof(in),
                                         out, sizeof(out), 60000);
    if (kr != kIOReturnSuccess) {
        MLX_LOG("ENABLE_HCA failed: 0x%x", kr);
        return false;
    }
    ivars->fHcaEnabled = true;
    MLX_LOG("ENABLE_HCA ok — HCA enabled");
    return true;
}

bool
MlxPCIDriver::SetIssi()
{
    /* QUERY_ISSI (0x10A) → pick the lowest supported ISSI → SET_ISSI (0x10B).
     * [FIX v0.35] outlen=0x70 (112B) like the Apple driver (notes/35):
     *   mlx5_ifc_query_issi_out_bits:
     *     current_issi      at bit 0x50 (16 bits)
     *     supported_issi_dw0 at bit 0x360 (32 bits = byte 108) — outside 24B!
     * The old outlen=24 gave fw_status=81 (corrupted BAD_OUT_LEN) and
     * supported=0 — ISSI negotiation never worked. */
    uint8_t in[16] = {};
    uint8_t qout[0x70] = {};   /* 112 bytes — full QUERY_ISSI response */
    mlxSetBits(in, 0x00, 16, MLX_CMD_OP_QUERY_ISSI);
    kern_return_t kr = ivars->fCmd->Exec(MLX_CMD_OP_QUERY_ISSI, in, sizeof(in),
                                         qout, sizeof(qout), 5000);
    if (kr != kIOReturnSuccess) {
        MLX_LOG("QUERY_ISSI failed: 0x%x — skipping ISSI (non-critical)", kr);
        return true;   /* ISSI is optional: fw may not support it */
    }
    uint32_t supported = (uint32_t)mlxGetBits(qout, 0x360, 32);
    uint32_t current   = (uint32_t)mlxGetBits(qout, 0x50, 16);
    MLX_LOG("QUERY_ISSI: supported=0x%x current=%u", supported, current);
    if (supported == 0) {
        MLX_LOG("ISSI not supported — skipping");
        return true;
    }
    if (current != 0) { ivars->fIssi = current; return true; }   /* already set */

    /* ISSI=1 = bit 1 in supported_issi_dw0 (like AppleMCX: supIssi & (1<<1)). */
    if (!(supported & (1u << 1))) {
        MLX_LOG("ISSI 1 not supported (sup=0x%x) — ISSI=0", supported);
        ivars->fIssi = 0;
        return true;
    }
    uint32_t issi = 1;
    uint8_t sin[16] = {};
    uint8_t sout[16] = {};
    mlxSetBits(sin, 0x00, 16, MLX_CMD_OP_SET_ISSI);
    /* [FIX v0.35] current_issi in SET_ISSI_in at bit 0x50 (16 bits), not 0x20/8. */
    mlxSetBits(sin, 0x50, 16, issi);
    kr = ivars->fCmd->Exec(MLX_CMD_OP_SET_ISSI, sin, sizeof(sin),
                           sout, sizeof(sout), 5000);
    if (kr != kIOReturnSuccess) {
        MLX_LOG("SET_ISSI failed: 0x%x", kr);
        return false;
    }
    ivars->fIssi = issi;
    MLX_LOG("SET_ISSI ok — ISSI=%u", issi);
    return true;
}

bool
MlxPCIDriver::SetHcaCaps()
{
    /* SET_HCA_CAP (0x109): pin the general caps. AppleMCX donor setHcaCaps:
     * QUERY general MAX+CURRENT → copy current into in+16 → set the bits
     * (cmdif_checksum=0, uar4k from maxCap, log_uar_page_sz @0x490).
     * RoCE is NOT enabled here: that is done by MODIFY_NIC_VPORT_CONTEXT
     * (vport.roce_en) — see MlxEnableVportRoce. Bit 0x20f = drain_sigerr,
     * not roce (roce sits at 0x21c, and it's a read-only cap). */
    uint8_t *maxCap = static_cast<uint8_t *>(IOMallocZero(MLX_P1_HCA_CAP_BYTES));
    uint8_t *curCap = static_cast<uint8_t *>(IOMallocZero(MLX_P1_HCA_CAP_BYTES));
    uint8_t *in = static_cast<uint8_t *>(IOMallocZero(MLX_P1_SET_HCA_CAP_IN_BYTES));
    if (!maxCap || !curCap || !in) {
        if (maxCap) IOFree(maxCap, MLX_P1_HCA_CAP_BYTES);
        if (curCap) IOFree(curCap, MLX_P1_HCA_CAP_BYTES);
        if (in) IOFree(in, MLX_P1_SET_HCA_CAP_IN_BYTES);
        return false;
    }
    bool ok = QueryHcaCap(MLX_P1_CAP_GENERAL, MLX_P1_CAP_MAX, maxCap) &&
              QueryHcaCap(MLX_P1_CAP_GENERAL, MLX_P1_CAP_CURRENT, curCap);
    if (ok) {
        ivars->fSwOwnerIdSupported = mlxGetBits(maxCap, 0x61e, 1) != 0;
        memcpy(in + MLX_P1_CMD_HEADER_BYTES, curCap, 256);
        mlxSetBits(in, 0x00, 16, MLX_CMD_OP_SET_HCA_CAP);
        mlxSetBits(in, 0x30, 16, MLX_P1_CAP_GENERAL << 1);
        mlxSetBits(in + MLX_P1_CMD_HEADER_BYTES, 0x210, 2, 0);   /* cmdif_checksum=0 (Linux) */
        mlxSetBits(in + MLX_P1_CMD_HEADER_BYTES, 0x145, 1,
                   mlxGetBits(maxCap, 0x145, 1));
        uint16_t logUarPageSize = 0;   /* 4 KiB UAR (uar4k) */
        mlxSetBits(in + MLX_P1_CMD_HEADER_BYTES, 0x240, 1,
                   mlxGetBits(maxCap, 0x240, 1));
        mlxSetBits(in + MLX_P1_CMD_HEADER_BYTES, 0x490, 16, logUarPageSize);
        uint8_t out[16] = {};
        ok = ivars->fCmd->Exec(MLX_CMD_OP_SET_HCA_CAP, in,
                               MLX_P1_SET_HCA_CAP_IN_BYTES,
                               out, sizeof(out), 5000) == kIOReturnSuccess;
    }
    IOFree(in, MLX_P1_SET_HCA_CAP_IN_BYTES);
    IOFree(curCap, MLX_P1_HCA_CAP_BYTES);
    IOFree(maxCap, MLX_P1_HCA_CAP_BYTES);
    MLX_LOG("SET_HCA_CAP %s", ok ? "ok" : "FAILED");
    return ok;
}

bool
MlxPCIDriver::InitHca()
{
    /* Linux generates one 128-bit sw_owner_id per driver lifetime and reuses
     * it for each function open. Firmware consumes it only when the general
     * capability advertises sw_owner_id. Keeping it stable is central to the
     * no-FLR repeated INIT_HCA gate. */
    if (!ivars->fCmd) return false;
    uint8_t in[MLX_P1_INIT_HCA_IN_BYTES] = {};
    uint8_t out[16] = {};
    mlxSetBits(in, 0x00, 16, MLX_CMD_OP_INIT_HCA);
    if (ivars->fSwOwnerIdSupported) {
        for (uint32_t i = 0; i < 4; i++)
            mlxSetBits(in, 0x80 + i * 32, 32, ivars->fSwOwnerId[i]);
    }
    kern_return_t kr = ivars->fCmd->Exec(MLX_CMD_OP_INIT_HCA, in, sizeof(in),
                                         out, sizeof(out), 5000);
    if (kr != kIOReturnSuccess) {
        MLX_LOG("INIT_HCA failed: 0x%x", kr);
        return false;
    }
    ivars->fHcaInitialized = true;
    MLX_LOG("INIT_HCA ok — HCA initialized sw_owner_cap=%u owner=%08x:%08x:%08x:%08x",
            ivars->fSwOwnerIdSupported ? 1 : 0,
            ivars->fSwOwnerId[0], ivars->fSwOwnerId[1],
            ivars->fSwOwnerId[2], ivars->fSwOwnerId[3]);
    return true;
}

bool
MlxPCIDriver::TeardownHca()
{
    /* TEARDOWN_HCA (0x103): graceful, if INIT_HCA happened. */
    if (!ivars->fHcaInitialized) return true;
    if (!ivars->fCmd) return false;
    uint8_t in[16] = {};
    uint8_t out[16] = {};
    mlxSetBits(in, 0x00, 16, MLX_CMD_OP_TEARDOWN_HCA);
    kern_return_t kr = ivars->fCmd->Exec(MLX_CMD_OP_TEARDOWN_HCA, in, sizeof(in),
                                         out, sizeof(out), 5000);
    if (kr != kIOReturnSuccess) {
        MLX_LOG("TEARDOWN_HCA failed: 0x%x", kr);
        return false;
    }
    MLX_LOG("TEARDOWN_HCA ok");
    return true;
}

bool
MlxPCIDriver::DisableHca()
{
    /* DISABLE_HCA (0x105): releases fw from the ENABLE_HCA state.
     * [FIX v0.35] implemented — without it fw stays poisoned after kill
     * (notes/35): next-start IOPCIFamily FLR cannot reload fw. */
    if (!ivars->fHcaEnabled) return true;
    if (!ivars->fCmd) return false;
    uint8_t in[16] = {};
    uint8_t out[16] = {};
    mlxSetBits(in, 0x00, 16, MLX_CMD_OP_DISABLE_HCA);
    kern_return_t kr = ivars->fCmd->Exec(MLX_CMD_OP_DISABLE_HCA, in, sizeof(in),
                                         out, sizeof(out), 5000);
    if (kr != kIOReturnSuccess) {
        MLX_LOG("DISABLE_HCA failed: 0x%x", kr);
        return false;
    }
    ivars->fHcaEnabled = false;
    MLX_LOG("DISABLE_HCA ok — HCA disabled");
    return true;
}

bool
MlxPCIDriver::QueryHcaCaps()
{
    /* QUERY_HCA_CAP: GENERAL current → parse → fill MlxHcaCaps.
     * RoCE caps are read only if general.roce is set. Donor: AppleMCX
     * queryHcaCaps (flow/ethernet omitted for the MVP RoCEv2). */
    uint8_t *general = static_cast<uint8_t *>(IOMallocZero(MLX_P1_HCA_CAP_BYTES));
    uint8_t *roce = static_cast<uint8_t *>(IOMallocZero(MLX_P1_HCA_CAP_BYTES));
    uint8_t *atomic = static_cast<uint8_t *>(IOMallocZero(MLX_P1_HCA_CAP_BYTES));
    if (!general || !roce || !atomic) {
        if (general) IOFree(general, MLX_P1_HCA_CAP_BYTES);
        if (roce) IOFree(roce, MLX_P1_HCA_CAP_BYTES);
        if (atomic) IOFree(atomic, MLX_P1_HCA_CAP_BYTES);
        return false;
    }
    MlxP1GeneralCaps parsed = {};
    MlxP1RoceCaps parsedRoce = {};
    bool ok = QueryHcaCap(MLX_P1_CAP_GENERAL, MLX_P1_CAP_CURRENT, general) &&
              mlxP1ParseGeneralCaps(general, MLX_P1_HCA_CAP_BYTES, &parsed);
    bool haveRoce = parsed.roce &&
        QueryHcaCap(MLX_P1_CAP_ROCE, MLX_P1_CAP_CURRENT, roce) &&
        mlxP1ParseRoceCaps(roce, MLX_P1_HCA_CAP_BYTES, &parsedRoce);
    /* ATOMIC caps are best-effort: no atomics (or a command failure) yields
     * atomicMode=0 and requester atomics stay disabled on the QP. */
    MlxP1AtomicCaps parsedAtomic = {};
    uint32_t atomicMode = 0;
    if (QueryHcaCap(MLX_P1_CAP_ATOMIC, MLX_P1_CAP_CURRENT, atomic) &&
        mlxP1ParseAtomicCaps(atomic, MLX_P1_HCA_CAP_BYTES, &parsedAtomic))
        atomicMode = mlxP1AtomicMode(&parsedAtomic);
    /* Fallback: the general caps advertise atomics but the ATOMIC caps read
     * failed (older firmware). Default to 8B, the standard RoCEv2 mode. */
    if (!atomicMode && parsed.atomic)
        atomicMode = 3;  /* MLX_ATOMIC_MODE_8B */
    if (!ok) {
        IOFree(general, MLX_P1_HCA_CAP_BYTES);
        IOFree(roce, MLX_P1_HCA_CAP_BYTES);
        IOFree(atomic, MLX_P1_HCA_CAP_BYTES);
        return false;
    }
    if (ivars->fHCA) {
        MlxHcaCaps &caps = ivars->fHCA->MutableCaps();
        memset(&caps, 0, sizeof(caps));
        caps.portType = parsed.portType;
        caps.numPorts = parsed.numPorts;
        caps.maxQp = mlxP1LogResourceSize(parsed.logMaxQp);
        caps.maxCq = mlxP1LogResourceSize(parsed.logMaxCq);
        caps.maxMr = mlxP1LogResourceSize(parsed.logMaxMkey);
        caps.logMaxMsg = parsed.logMaxMsg;
        caps.roce = haveRoce;
        caps.uar4k = parsed.uar4k;
        caps.logBfRegSize = parsed.bf ? parsed.logBfRegSize : 0;
        caps.numVhcaPorts = parsed.numVhcaPorts;
        caps.swOwnerId = parsed.swOwnerId;
        caps.roceRwSupported = parsed.roceRwSupported;
        caps.roceMaxGid = haveRoce ? parsedRoce.addressTableSize : 0;
        caps.roceVersions = haveRoce ? mlxP1RoceVersionsForAbi(parsedRoce.versions) : 0;
        caps.roceDstUdpPort = haveRoce ? parsedRoce.destinationUdpPort : 0;
        caps.roceMinSrcUdpPort = haveRoce ? parsedRoce.minimumSourceUdpPort : 0;
        caps.swRoceSrcUdpPort = haveRoce && parsedRoce.sourceUdpPortWritable;
        caps.atomicMode = static_cast<uint8_t>(atomicMode);
        caps.ibSupported = caps.portType == MLX_PORT_TYPE_IB;
        caps.ibMaxPkeys = static_cast<uint16_t>(
            mlxP1PkeyTableSize(parsed.pkeyTableEncoding));
    }
    MLX_LOG("QUERY_HCA_CAP: logMaxQp=%u logMaxCq=%u logMaxMkey=%u logMaxMsg=%u logMaxSrqSz=%u logPgSz=%u portType=%u numPorts=%u roce=%u uar4k=%u cacheLine128=%u gidTable=%u roceVersions=0x%x udpDst=%u udpSrcMin=%u atomicOps=0x%x atomicSizeQp=0x%x atomicMode=%u",
            parsed.logMaxQp, parsed.logMaxCq, parsed.logMaxMkey,
            parsed.logMaxMsg, parsed.logMaxSrqSz, parsed.logPgSz,
            parsed.portType, parsed.numPorts, haveRoce ? 1 : 0,
            parsed.uar4k ? 1 : 0, parsed.cacheLine128 ? 1 : 0,
            parsedRoce.addressTableSize,
            mlxP1RoceVersionsForAbi(parsedRoce.versions),
            parsedRoce.destinationUdpPort, parsedRoce.minimumSourceUdpPort,
            parsedAtomic.operations, parsedAtomic.sizeQp, atomicMode);
    IOFree(general, MLX_P1_HCA_CAP_BYTES);
    IOFree(roce, MLX_P1_HCA_CAP_BYTES);
    IOFree(atomic, MLX_P1_HCA_CAP_BYTES);
    return true;
}

bool
MlxPCIDriver::QueryHcaCap(uint16_t type, uint16_t mode, uint8_t *capability)
{
    if (!capability) return false;
    uint8_t in[MLX_P1_QUERY_HCA_CAP_IN_BYTES] = {};
    uint8_t *out = static_cast<uint8_t *>(
        IOMallocZero(MLX_P1_QUERY_HCA_CAP_OUT_BYTES));
    if (!out) return false;
    mlxP1EncodeQueryHcaCap(in, sizeof(in), type, mode);
    kern_return_t kr = ivars->fCmd->Exec(MLX_CMD_OP_QUERY_HCA_CAP, in, sizeof(in),
                                         out, MLX_P1_QUERY_HCA_CAP_OUT_BYTES,
                                         5000);
    if (kr == kIOReturnSuccess)
        memcpy(capability, out + MLX_P1_CMD_HEADER_BYTES, MLX_P1_HCA_CAP_BYTES);
    IOFree(out, MLX_P1_QUERY_HCA_CAP_OUT_BYTES);
    return kr == kIOReturnSuccess;
}

bool
MlxPCIDriver::SetAtomicReqEndianness()
{
    /* Linux mlx5 handle_hca_cap_atomic(): if the card advertises
     * supported_atomic_req_8B_endianness_mode_1, SET_HCA_CAP(ATOMIC) selects
     * atomic_req_8B_endianness_mode=1 (host byte order) so the driver's
     * htobe64 atomic values are interpreted correctly. */
    uint8_t *atomicCaps = static_cast<uint8_t *>(IOMallocZero(MLX_P1_HCA_CAP_BYTES));
    if (!atomicCaps) return false;
    bool ok = QueryHcaCap(MLX_P1_CAP_ATOMIC, MLX_P1_CAP_CURRENT, atomicCaps);
    uint32_t supported = ok ? (uint32_t)mlxGetBits(atomicCaps, 0x46, 1) : 0;
    if (!ok || !supported) {
        /* No support for mode 1, or the query failed: keep the default. */
        MLX_LOG("SET_HCA_CAP ATOMIC: supported_req_8B_endianness_mode_1=%u (query=%u) — keeping default mode",
                supported, ok ? 1 : 0);
        IOFree(atomicCaps, MLX_P1_HCA_CAP_BYTES);
        return ok;
    }
    mlxSetBits(atomicCaps, 0x40, 2, 1);  /* atomic_req_8B_endianness_mode = 1 */
    uint8_t *in = static_cast<uint8_t *>(IOMallocZero(MLX_P1_SET_HCA_CAP_IN_BYTES));
    if (!in) {
        IOFree(atomicCaps, MLX_P1_HCA_CAP_BYTES);
        return false;
    }
    memcpy(in + MLX_P1_CMD_HEADER_BYTES, atomicCaps, MLX_P1_HCA_CAP_BYTES);
    mlxSetBits(in, 0x00, 16, MLX_CMD_OP_SET_HCA_CAP);
    mlxSetBits(in, 0x30, 16, MLX_P1_CAP_ATOMIC << 1);
    uint8_t out[16] = {};
    kern_return_t kr = ivars->fCmd->Exec(MLX_CMD_OP_SET_HCA_CAP, in,
                                         MLX_P1_SET_HCA_CAP_IN_BYTES,
                                         out, sizeof(out), 5000);
    IOFree(in, MLX_P1_SET_HCA_CAP_IN_BYTES);
    IOFree(atomicCaps, MLX_P1_HCA_CAP_BYTES);
    MLX_LOG("SET_HCA_CAP ATOMIC req_8B_endianness_mode=1: 0x%x", kr);
    return kr == kIOReturnSuccess;
}
bool MlxPCIDriver::DisableBusMasterAndVerify() { return false; }
bool MlxPCIDriver::NegotiateRoceCap() { return false; }
bool MlxPCIDriver::PublishNubs() { return false; }

bool
MlxPCIDriver::AllocPd()
{
    /* ALLOC_PD (0x800): allocates a protection domain. Out: pd at bit 0x20 (24b). */
    if (!ivars->fCmd) return false;
    uint8_t in[16] = {};
    uint8_t out[16] = {};
    mlxSetBits(in, 0x00, 16, MLX_CMD_OP_ALLOC_PD);
    kern_return_t kr = ivars->fCmd->Exec(MLX_CMD_OP_ALLOC_PD, in, sizeof(in),
                                         out, sizeof(out), 5000);
    if (kr != kIOReturnSuccess) {
        MLX_LOG("ALLOC_PD failed: 0x%x", kr);
        return false;
    }
    ivars->fPd = (uint32_t)mlxGetBits(out, 0x48, 24);
    MLX_LOG("ALLOC_PD ok — pd=%u", ivars->fPd);
    return true;
}

bool
MlxPCIDriver::AllocXrcd()
{
    /* ALLOC_XRCD (0x80e): fw validates xrcd in the QPC (Linux devr->xrcdn1). */
    if (!ivars->fCmd) return false;
    uint8_t in[16] = {};
    uint8_t out[16] = {};
    mlxSetBits(in, 0x00, 16, MLX_CMD_OP_ALLOC_XRCD);
    kern_return_t kr = ivars->fCmd->Exec(MLX_CMD_OP_ALLOC_XRCD, in, sizeof(in),
                                         out, sizeof(out), 5000);
    if (kr != kIOReturnSuccess) {
        MLX_LOG("ALLOC_XRCD failed: 0x%x", kr);
        return false;
    }
    ivars->fXrcd = (uint32_t)mlxGetBits(out, 0x48, 24);
    MLX_LOG("ALLOC_XRCD ok — xrcd=%u", ivars->fXrcd);
    return true;
}

