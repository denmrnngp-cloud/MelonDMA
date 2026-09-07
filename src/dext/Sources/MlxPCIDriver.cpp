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
#include "MlxSafety.hpp"
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
#include <DriverKit/IOInterruptDispatchSource.h>
#include <DriverKit/IODispatchQueue.h>
#include <PCIDriverKit/IOPCIDevice.h>
#include <PCIDriverKit/IOPCIFamilyDefinitions.h>
#include <string.h>
#include <time.h>

#include "MlxLog.hpp"
#define MLX_LOG(fmt, ...)  IOLog("MlxPCIDriver: " fmt "\n", ##__VA_ARGS__)
#define MLX_DBG(fmt, ...)  MLX_DBGLOG("MlxPCIDriver: " fmt, ##__VA_ARGS__)

/* Two messaged sub-vectors: async on host index 0, completion on index 1.
 * Set back to 1 to share the first sub-vector, which is what the driver did
 * for as long as the MSI-X table was never programmed. The kernel now fills
 * the table (nine vectors, data 1..9, all unmasked), so index 1 is live. */
#define MLX_SINGLE_MSIX_VECTOR 0

/* Quarantined MlxFwPages must outlive the driver instance that lost BAR/MMIO.
 * The registry retains the full object (and therefore every DMA reference)
 * until a later verified FLR establishes a DMA boundary. */
static const uint32_t kMlxQuarantineRegistrySize = 16;
static MlxFwPages *gMlxQuarantinedPages[kMlxQuarantineRegistrySize] = {};
static IOPCIDevice *gMlxQuarantinedPageDevices[kMlxQuarantineRegistrySize] = {};
struct MlxQuarantinedDma {
    IOMemoryDescriptor *mem;
    IODMACommand *dma;
    IOPCIDevice *pci;
    MlxQuarantinedDma *next;
};
static MlxQuarantinedDma *gMlxQuarantinedDma;
static unsigned char gMlxQuarantineLock;
static void MlxLockQuarantine() {
    while (__atomic_test_and_set(&gMlxQuarantineLock, __ATOMIC_ACQUIRE)) IOSleep(1);
}
static void MlxUnlockQuarantine() {
    __atomic_clear(&gMlxQuarantineLock, __ATOMIC_RELEASE);
}

static void
MlxRetainQuarantinedPages(MlxFwPages *pages, IOPCIDevice *pci)
{
    if (!pages) return;
    MlxLockQuarantine();
    for (uint32_t i = 0; i < kMlxQuarantineRegistrySize; i++) {
        if (gMlxQuarantinedPages[i] == pages) { MlxUnlockQuarantine(); return; }
        if (!gMlxQuarantinedPages[i]) {
            gMlxQuarantinedPages[i] = pages;
            gMlxQuarantinedPageDevices[i] = pci;
            if (pci) pci->retain();
            MlxUnlockQuarantine();
            MLX_LOG("DMA quarantine registry: retained slot=%u ambiguous=%u",
                    i, pages->GetAmbiguousOwned());
            return;
        }
    }
    MlxUnlockQuarantine();
    /* Preserve ownership by intentionally not deleting the object even if the
     * diagnostic registry is exhausted. */
    MLX_LOG("DMA quarantine registry FULL — object intentionally leaked/pinned");
}

static void
MlxReleaseQuarantinedPagesAfterReset(IOPCIDevice *pci)
{
    for (uint32_t i = 0; i < kMlxQuarantineRegistrySize; i++) {
        MlxLockQuarantine();
        MlxFwPages *pages = gMlxQuarantinedPages[i];
        if (!pages || gMlxQuarantinedPageDevices[i] != pci) {
            MlxUnlockQuarantine(); continue;
        }
        gMlxQuarantinedPages[i] = NULL;
        gMlxQuarantinedPageDevices[i] = NULL;
        MlxUnlockQuarantine();
        if (pci) pci->release();
        pages->ReleaseQuarantineAfterReset();
        pages->Free();
        delete pages;
        MLX_LOG("DMA quarantine registry: released slot=%u after verified reset", i);
    }
    MlxLockQuarantine();
    MlxQuarantinedDma *release = NULL;
    MlxQuarantinedDma **link = &gMlxQuarantinedDma;
    while (*link) {
        MlxQuarantinedDma *node = *link;
        if (node->pci != pci) { link = &node->next; continue; }
        *link = node->next; node->next = release; release = node;
    }
    MlxUnlockQuarantine();
    while (release) {
        MlxQuarantinedDma *node = release; release = node->next;
        if (node->dma) mlxCompleteDma(node->dma);
        if (node->mem) node->mem->release();
        if (node->pci) node->pci->release();
        IODelete(node, MlxQuarantinedDma, 1);
    }
}

static void
MlxRetainQuarantinedDummy(IOMemoryDescriptor *mem, IODMACommand *dma, IOPCIDevice *pci)
{
    MlxQuarantinedDma *node = IONewZero(MlxQuarantinedDma, 1);
    if (!node) {
        MLX_LOG("DMA quarantine allocation failed — references intentionally retained");
        return; /* fail safe even under memory pressure */
    }
    node->mem = mem; node->dma = dma; node->pci = pci;
    if (pci) pci->retain();
    MlxLockQuarantine();
    node->next = gMlxQuarantinedDma; gMlxQuarantinedDma = node;
    MlxUnlockQuarantine();
}

/* ---- instance state (private; typed via MlxPCIDriver_DECLARE_IVARS) ---- */
struct MlxPCIDriver_IVars {
    IOPCIDevice         *fPci;
    IOMemoryDescriptor  *fBar0Mem;
    uint8_t              fBar0Index;
    uint16_t             fDeviceId;
    MlxCmd              *fCmd;
    MlxEQ               *fEQ;
    MlxEQ               *fCompletionEQ;
    MlxUAR              *fUAR;
    MlxHCA              *fHCA;
    MlxHealth           *fHealth;
    MlxDMA              *fDMA;
    MlxFwPages          *fFwPages;
    MlxRoCE             *fRoCE;
    IODispatchQueue     *fEqQueue;
    IOTimerDispatchSource *fEqTimer;
    OSAction             *fEqTimerAction;
    IODispatchQueue      *fInterruptQueue;
    IODispatchQueue      *fCompletionWaitQueue;
    IOInterruptDispatchSource *fAsyncInterrupt;
    IOInterruptDispatchSource *fCompletionInterrupt;
    /* Host interrupt index map (MLX_IRQ_KIND_* per index) and the indices the
     * two dispatch sources were actually bound to. fMsixIndexBase is the host
     * index carrying firmware vector 0; MLX_IRQ_INDEX_NONE means no messaged
     * pair answered and the historical hardcoded 0/1 were kept instead. */
    uint8_t               fIrqIndexKind[MLX_IRQ_INDEX_MAP];
    uint8_t               fIrqIndexKindPre[MLX_IRQ_INDEX_MAP];
    uint64_t              fIrqIndexType[MLX_IRQ_INDEX_MAP];
    uint32_t              fIrqIndexCount;
    uint32_t              fIrqIndexCountPre;
    uint32_t              fIrqIndexProbeStatus;
    /* Result of claiming the vectors before firmware init; 0 = granted. */
    uint32_t              fEarlyMsixStatus;
    uint32_t              fMsixIndexBase;
    uint32_t              fAsyncIndex;
    uint32_t              fCompletionIndex;
    OSAction             *fAsyncInterruptAction;
    OSAction             *fCompletionInterruptAction;
    uint64_t              fCompletionGeneration;
    /* Completion MSI-X interrupts that advanced fCompletionGeneration. Only
     * the completion-interrupt handler writes it, under the wait queue. */
    uint64_t              fCompletionEvents;
    /* Waits released with a fresh generation. Device-wide because the shim
     * blocks on a dedicated UserClient connection, so a per-client count is
     * invisible to the connection that reads the telemetry. */
    uint64_t              fCompletionWakeups;
    /* IRQ counters are kept per vector. Completion readiness is based only on
     * the dedicated completion vector; an async EQ interrupt is not evidence
     * that completion IRQ delivery works. */
    uint64_t              fAsyncIrqCount;
    uint64_t              fCompletionIrqCount;
    uint64_t              fIrqCompletionEqes, fTimerCompletionEqes, fLastCompletionIrqNs;
    uint64_t              fDeviceEpoch, fQuarantineBytes, fQuarantineObjects;
    bool                  fBmeFenced;
    /* MSI-X bring-up result, kept for QueryInterrupts: the kernel log channel
     * is disabled on some dev machines, so the StartInterrupts log line is
     * not a reliable way to find out why the completion vector is missing. */
    uint32_t              fIrqVectors;
    uint32_t              fIrqSetupStatus;
    uint32_t              fIrqSetupStage;
    /* MSI-X entry 0 read BEFORE our own ConfigureInterrupts, to see whether
     * the previous owner (Apple) programmed the table (front A experiment). */
    uint32_t              fMsixPreConfigureEntry0AddrLo;
    uint32_t              fMsixPreConfigureEntry0Data;
    /* Completion-EQ bring-up result. The MSI-X vectors can be live while this
     * EQ is rejected, which is exactly the case that leaves clients polling. */
    uint32_t              fCqEqStatus;
    uint32_t              fCqEqStage;
    uint32_t              fCqEqSyndrome;
    uint32_t              fCqEqFwStatus;
    uint32_t              fCqEqVariant;          /* 1-based, 0 = none worked */
    uint32_t              fCqEqVariantTried;     /* bitmask of attempts */
    uint32_t              fCqEqVariantSyndrome[4];
    uint64_t             fLastHealthCheck;
    uint64_t             fEqTickCount;
    uint32_t             fEqTimerPaused;
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
MlxEqPollDeadline(uint64_t periodMs)
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + periodMs * 1000 * 1000;
}

/* The EQ timer is the only event delivery while MSI-X is unproven, so it runs
 * hot until the first interrupt arrives. After that it is insurance and can be
 * rare: 50 ms matches the client's own blocking-wait backstop, so a dead
 * vector costs the same bounded delay on both sides instead of 100 pointless
 * wakeups a second inside the DEXT.
 *
 * On a card where MSI-X is never delivered the timer stays the delivery path,
 * and then the thing worth cutting is its cost when nobody is using the
 * device. With no CQ allocated there is no completion to deliver and no
 * traffic to make the firmware ask for pages, so the timer drops to the idle
 * rate and the DEXT stops charging a busy machine for an unused NIC. */
#define MLX_EQ_POLL_UNPROVEN_MS  1ULL
#define MLX_EQ_POLL_PROVEN_MS    50ULL
#define MLX_EQ_POLL_IDLE_MS      100ULL

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
                                          MlxEqPollDeadline(MLX_EQ_POLL_UNPROVEN_MS), 1000000);
}

static void
MlxDrainEqTimerQueue(void *)
{
    /* DispatchSync_f is the teardown barrier; all earlier timer callbacks on
     * this serial queue have completed when it returns. */
}

/* PCIDriverKit exports only the two messaged bits of the GetInterruptType
 * word (IOPCIFamilyDefinitions.h). The edge/level flag lives in the low bit
 * of the same word; the SDK documents it by name but does not export it, so
 * it is spelled out rather than referenced. */
enum { MLX_IRQ_TYPE_LEVEL_BIT = 0x1 };

static uint8_t
MlxClassifyInterruptType(uint64_t type)
{
    if (type & kIOInterruptTypePCIMessagedX) return MLX_IRQ_KIND_MSIX;
    if (type & kIOInterruptTypePCIMessaged)  return MLX_IRQ_KIND_MSI;
    if (type & ~(uint64_t)MLX_IRQ_TYPE_LEVEL_BIT) return MLX_IRQ_KIND_OTHER;
    return (type & MLX_IRQ_TYPE_LEVEL_BIT) ? MLX_IRQ_KIND_LEVEL
                                           : MLX_IRQ_KIND_EDGE;
}

/* Asks the provider what each of the first MLX_IRQ_INDEX_MAP interrupt
 * indices actually is. Read-only and side-effect free: nothing is created,
 * enabled or written to hardware, so it is safe to run before and after
 * ConfigureInterrupts. */
void
MlxPCIDriver::ProbeInterruptIndices(uint8_t *kind, uint64_t *typeRaw,
                                    uint32_t *count,
                                    kern_return_t *firstFailure)
{
    uint32_t answered = 0;
    kern_return_t first = kIOReturnSuccess;
    for (uint32_t i = 0; i < MLX_IRQ_INDEX_MAP; i++) {
        uint64_t type = 0;
        kern_return_t kr = IOInterruptDispatchSource::GetInterruptType(
            ivars->fPci, i, &type);
        if (kr != kIOReturnSuccess) {
            if (kind)    kind[i] = MLX_IRQ_KIND_ABSENT;
            if (typeRaw) typeRaw[i] = 0;
            if (first == kIOReturnSuccess) first = kr;
            continue;
        }
        answered++;
        if (kind)    kind[i] = MlxClassifyInterruptType(type);
        if (typeRaw) typeRaw[i] = type;
    }
    if (count)        *count = answered;
    if (firstFailure) *firstFailure = first;
}

/* Picks the host indices to bind the two dispatch sources to. Firmware vector
 * V is raised on host index base + V, so the async EQ (intr 0) and the
 * completion EQ (intr 1) must land on two consecutive messaged indices.
 * Without such a pair the historical hardcoded 0/1 are kept, so a provider
 * that does not answer GetInterruptType cannot make things worse than before. */
void
MlxPCIDriver::SelectInterruptIndices()
{
    ivars->fMsixIndexBase   = MLX_IRQ_INDEX_NONE;
    ivars->fAsyncIndex      = 0;
    ivars->fCompletionIndex = 0;
#if MLX_SINGLE_MSIX_VECTOR
    for (uint32_t i = 0; i < MLX_IRQ_INDEX_MAP; i++) {
        if (ivars->fIrqIndexKind[i] == MLX_IRQ_KIND_MSIX ||
            ivars->fIrqIndexKind[i] == MLX_IRQ_KIND_MSI) {
            ivars->fMsixIndexBase = i;
            ivars->fAsyncIndex = i;
            ivars->fCompletionIndex = i;
            return;
        }
    }
#else
    for (uint8_t want = MLX_IRQ_KIND_MSIX; ; want = MLX_IRQ_KIND_MSI) {
        for (uint32_t i = 0; i + 1 < MLX_IRQ_INDEX_MAP; i++) {
            if (ivars->fIrqIndexKind[i] == want &&
                ivars->fIrqIndexKind[i + 1] == want) {
                ivars->fMsixIndexBase = i;
                ivars->fAsyncIndex = i;
                ivars->fCompletionIndex = i + 1;
                return;
            }
        }
        if (want == MLX_IRQ_KIND_MSI) break;
    }
#endif
    MLX_LOG("interrupt index probe found no usable messaged vector "
            "(count=%u status=0x%x)", ivars->fIrqIndexCount,
            ivars->fIrqIndexProbeStatus);
}

/* Reads back the MSI-X Message Control and logs whether the enable bit is set
 * at this point in bring-up. The bit is what firmware would look at if it
 * decides once, at its own initialization, whether the function has message
 * interrupts at all. */
void
MlxPCIDriver::LogMsixEnable(const char *stage)
{
    if (!ivars || !ivars->fPci) return;
    uint16_t status = 0;
    ivars->fPci->ConfigurationRead16(0x06, &status);
    if (!(status & 0x0010)) return;
    uint8_t ptr = 0;
    ivars->fPci->ConfigurationRead8(0x34, &ptr);
    for (uint32_t guard = 0; ptr >= 0x40 && guard < 48; guard++) {
        uint16_t idNext = 0;
        ivars->fPci->ConfigurationRead16(ptr, &idNext);
        if ((idNext & 0xff) == 0x11) {
            uint16_t mc = 0;
            ivars->fPci->ConfigurationRead16((uint64_t)ptr + 0x02, &mc);
            MLX_LOG("MSI-X at %s: control=0x%04x enable=%u function_mask=%u",
                    stage, mc, (mc >> 15) & 1u, (mc >> 14) & 1u);
            return;
        }
        ptr = (uint8_t)(idNext >> 8);
    }
}

/* Enables MSI-X in the device before firmware initialization.
 *
 * The order used to be reset, then firmware init, then interrupts. A function
 * level reset clears the MSI-X enable bit, so firmware ran its entire
 * initialization while the capability read disabled. If firmware samples that
 * bit once, when it comes up, to decide whether the function has message
 * interrupts, then enabling MSI-X afterwards is too late: the table can be
 * programmed, the queues armed and events delivered, and no vector is ever
 * raised — which is exactly what was measured, on both queues, regardless of
 * table contents.
 *
 * This only asks for the vectors. Dispatch sources are still created later by
 * StartInterrupts, which tolerates being called twice. */
kern_return_t
MlxPCIDriver::EnableMsixEarly()
{
    if (!ivars || !ivars->fPci) return kIOReturnNotReady;
    LogMsixEnable("after FLR, before enabling");
    /* Allocate once, before firmware init.  ConfigureInterrupts is the
     * provider-side allocation boundary; calling it again in StartInterrupts
     * can retain the first allocation and leave the MSI-X table unrouted.
     *
     * Ask for two required, nine requested.  Two is what the driver actually
     * binds once the vectors are split: async on host index 0, completion on
     * index 1.  Asking for one used to look harmless only because
     * rdar://118153788 keeps a nub's first interrupt configuration for life,
     * so on this machine the request met a pre-existing nine-vector
     * allocation and became a no-op.  On a freshly enumerated nub it would
     * allocate exactly one vector, index 1 would not exist, and the
     * completion source would silently fall back onto the async vector.
     * Nine matches what the platform publishes and what Apple's own driver
     * for this card requests; allocateDeviceInterrupts walks down from there
     * and only fails below the required two. */
    kern_return_t kr = ivars->fPci->ConfigureInterrupts(
        kIOInterruptTypePCIMessagedX, MLX_SINGLE_MSIX_VECTOR ? 1 : 2, 9, 0);
    ivars->fEarlyMsixStatus = (uint32_t)kr;
    if (kr != kIOReturnSuccess)
        MLX_LOG("early MSI-X enable failed: 0x%x — falling back to the old "
                "ordering, interrupts are claimed after firmware init", kr);
    LogMsixEnable("after early enable, before firmware init");
    return kr;
}

kern_return_t
MlxPCIDriver::StartInterrupts()
{
    if (!ivars || !ivars->fPci) return kIOReturnNotReady;
    /* Probe once before allocation, so the map can show whether the messaged
     * indices exist only after ConfigureInterrupts. Skipped on the FLR re-arm
     * path, where the first probe's answer is the one that matters. */
    if (!ivars->fCompletionInterrupt)
        ProbeInterruptIndices(ivars->fIrqIndexKindPre, NULL,
                              &ivars->fIrqIndexCountPre, NULL);
    /* Front A experiment: snapshot MSI-X entry 0 BEFORE our own
     * ConfigureInterrupts, to see whether the previous owner (Apple) left a
     * programmed table (addrLo = the platform MSI doorbell 0xfffff000) or the
     * table was already empty. Walk the capability list like ReadMsixState so
     * the read is the same one the diagnostic uses. */
    ivars->fMsixPreConfigureEntry0AddrLo = 0;
    ivars->fMsixPreConfigureEntry0Data = 0;
    {
        uint16_t status = 0;
        ivars->fPci->ConfigurationRead16(0x06, &status);
        uint8_t ptr = 0;
        ivars->fPci->ConfigurationRead8(0x34, &ptr);
        uint32_t cap = 0;
        for (uint32_t g = 0; (status & 0x10) && ptr >= 0x40 && g < 48; g++) {
            uint16_t idNext = 0;
            ivars->fPci->ConfigurationRead16(ptr, &idNext);
            if ((idNext & 0xff) == 0x11) { cap = ptr; break; }
            ptr = (uint8_t)(idNext >> 8);
        }
        if (cap) {
            uint32_t tbl = 0;
            ivars->fPci->ConfigurationRead32((uint64_t)cap + 0x04, &tbl);
            const uint64_t base = (uint64_t)(tbl & ~0x7u);
            ivars->fPci->MemoryRead32(ivars->fBar0Index, base + 0,
                                      &ivars->fMsixPreConfigureEntry0AddrLo);
            ivars->fPci->MemoryRead32(ivars->fBar0Index, base + 8,
                                      &ivars->fMsixPreConfigureEntry0Data);
        }
        MLX_LOG("MSI-X pre-configure entry0: addrLo=0x%08x data=0x%08x",
                ivars->fMsixPreConfigureEntry0AddrLo,
                ivars->fMsixPreConfigureEntry0Data);
    }
    /* EnableMsixEarly owns the single ConfigureInterrupts allocation.  The
     * dispatch sources are intentionally created here, after firmware init,
     * but the provider must not be configured a second time. */
    kern_return_t kr = (ivars->fEarlyMsixStatus == kIOReturnSuccess)
        ? kIOReturnSuccess : (kern_return_t)ivars->fEarlyMsixStatus;
    if (kr != kIOReturnSuccess) {
        ivars->fIrqSetupStatus = (uint32_t)kr;
        MLX_LOG("MSI-X allocation failed before firmware init: 0x%x", kr);
        return kr;
    }
    /* fIrqVectors reports how many vectors the driver binds, not how many the
     * provider allocated; the real allocation is in fIrqIndexCount. */
    ivars->fIrqVectors = MLX_SINGLE_MSIX_VECTOR ? 1 : 2;
    if (ivars->fCompletionInterrupt) {
        /* Re-arm after a reinit FLR. The host-side dispatch sources survive a
         * reset, the device's MSI-X capability does not, so only the device
         * side is redone; recreating the sources would race DriverKit's
         * asynchronous disable/cancel. */
        ivars->fIrqSetupStage = MLX_IRQ_STAGE_ENABLE;
        kr = ivars->fAsyncInterrupt->SetEnable(true);
        if (kr == kIOReturnSuccess && !MLX_SINGLE_MSIX_VECTOR)
            kr = ivars->fCompletionInterrupt->SetEnable(true);
        if (kr != kIOReturnSuccess) {
            ivars->fIrqSetupStatus = (uint32_t)kr;
            MLX_LOG("MSI-X re-arm after reset failed: 0x%x", kr);
            return kr;
        }
        ivars->fIrqSetupStage = MLX_IRQ_STAGE_NONE;
        ivars->fIrqSetupStatus = 0;
        MLX_LOG("MSI-X re-armed after reset");
        return kIOReturnSuccess;
    }
    /* Now that the vectors are allocated, ask again and choose. */
    kern_return_t probeKr = kIOReturnSuccess;
    ProbeInterruptIndices(ivars->fIrqIndexKind, ivars->fIrqIndexType,
                          &ivars->fIrqIndexCount, &probeKr);
    ivars->fIrqIndexProbeStatus = (uint32_t)probeKr;
    SelectInterruptIndices();

    ivars->fIrqSetupStage = MLX_IRQ_STAGE_QUEUE;
    kr = IODispatchQueue::Create("MlxIRQ", 0, 0, &ivars->fInterruptQueue);
    if (kr == kIOReturnSuccess)
        kr = IODispatchQueue::Create("MlxCQWait", kIODispatchQueueReentrant,
                                     0, &ivars->fCompletionWaitQueue);
    if (kr == kIOReturnSuccess) {
        ivars->fIrqSetupStage = MLX_IRQ_STAGE_SOURCE;
        kr = IOInterruptDispatchSource::Create(
            ivars->fPci, ivars->fAsyncIndex, ivars->fInterruptQueue,
            &ivars->fAsyncInterrupt);
        /* A chosen index that the provider then refuses is worse than the old
         * behaviour, so fall back rather than leave the device without any
         * interrupt path at all. */
        if (kr != kIOReturnSuccess &&
            ivars->fMsixIndexBase != MLX_IRQ_INDEX_NONE) {
            MLX_LOG("async source refused at index %u (0x%x) — falling back "
                    "to 0/1", ivars->fAsyncIndex, kr);
            ivars->fMsixIndexBase   = MLX_IRQ_INDEX_NONE;
            ivars->fAsyncIndex      = 0;
            ivars->fCompletionIndex = 1;
            kr = IOInterruptDispatchSource::Create(
                ivars->fPci, ivars->fAsyncIndex, ivars->fInterruptQueue,
                &ivars->fAsyncInterrupt);
        }
    }
    if (kr == kIOReturnSuccess && !MLX_SINGLE_MSIX_VECTOR)
        kr = IOInterruptDispatchSource::Create(
            ivars->fPci, ivars->fCompletionIndex, ivars->fInterruptQueue,
            &ivars->fCompletionInterrupt);
    else if (kr == kIOReturnSuccess)
        ivars->fCompletionInterrupt = ivars->fAsyncInterrupt;
    if (kr == kIOReturnSuccess) {
        ivars->fIrqSetupStage = MLX_IRQ_STAGE_ACTION;
        kr = CreateActionAsyncInterruptOccurred(0, &ivars->fAsyncInterruptAction);
    }
    if (kr == kIOReturnSuccess && !MLX_SINGLE_MSIX_VECTOR)
        kr = CreateActionCompletionInterruptOccurred(
            0, &ivars->fCompletionInterruptAction);
    if (kr == kIOReturnSuccess) {
        ivars->fIrqSetupStage = MLX_IRQ_STAGE_HANDLER;
        kr = ivars->fAsyncInterrupt->SetHandler(ivars->fAsyncInterruptAction);
    }
    if (kr == kIOReturnSuccess && !MLX_SINGLE_MSIX_VECTOR)
        kr = ivars->fCompletionInterrupt->SetHandler(
            ivars->fCompletionInterruptAction);
    if (kr == kIOReturnSuccess) {
        ivars->fIrqSetupStage = MLX_IRQ_STAGE_ENABLE;
        kr = ivars->fAsyncInterrupt->SetEnable(true);
    }
    if (kr == kIOReturnSuccess && !MLX_SINGLE_MSIX_VECTOR)
        kr = ivars->fCompletionInterrupt->SetEnable(true);
    if (kr != kIOReturnSuccess) {
        ivars->fIrqSetupStatus = (uint32_t)kr;
        MLX_LOG("MSI-X dispatch setup failed at stage %u: 0x%x",
                ivars->fIrqSetupStage, kr);
        StopInterrupts();
        return kr;
    }
    ivars->fIrqSetupStage = MLX_IRQ_STAGE_NONE;
    ivars->fIrqSetupStatus = 0;
    MLX_LOG("MSI-X dispatch active: async index=%u completion index=%u "
            "msixBase=%d probed=%u/%u",
            ivars->fAsyncIndex, ivars->fCompletionIndex,
            ivars->fMsixIndexBase == MLX_IRQ_INDEX_NONE
                ? -1 : (int)ivars->fMsixIndexBase,
            ivars->fIrqIndexCount, ivars->fIrqIndexCountPre);
    return kIOReturnSuccess;
}

void
MlxPCIDriver::GetInterruptStatus(uint32_t *vectors, uint32_t *status,
                                 uint32_t *stage) const
{
    if (vectors) *vectors = ivars ? ivars->fIrqVectors : 0;
    if (status)  *status  = ivars ? ivars->fIrqSetupStatus : 0;
    if (stage)   *stage   = ivars ? ivars->fIrqSetupStage
                                  : (uint32_t)MLX_IRQ_STAGE_NOT_ATTEMPTED;
}

void
MlxPCIDriver::GetEqServiceStats(uint64_t *asyncIrq, uint64_t *completionIrq,
                                uint64_t *timerTicks,
                                uint32_t *timerPeriodMs) const
{
    if (asyncIrq)   *asyncIrq   = ivars ?
        __atomic_load_n(&ivars->fAsyncIrqCount, __ATOMIC_RELAXED) : 0;
    if (completionIrq) *completionIrq = ivars ?
        __atomic_load_n(&ivars->fCompletionIrqCount, __ATOMIC_RELAXED) : 0;
    if (timerTicks) *timerTicks = ivars ? __atomic_load_n(&ivars->fEqTickCount, __ATOMIC_RELAXED) : 0;
    if (timerPeriodMs) {
        bool inUse = ivars && ivars->fRoCE && ivars->fRoCE->GetCQ() &&
                     ivars->fRoCE->GetCQ()->LiveCount();
        *timerPeriodMs = mlxEqPollPeriodMs(inUse, CompletionInterruptReady());
    }
}

void
MlxPCIDriver::GetInterruptIndexMap(uint32_t *count, uint32_t *countPre,
                                   uint32_t *msixBase, uint32_t *asyncIndex,
                                   uint32_t *completionIndex,
                                   uint32_t *probeStatus,
                                   uint8_t *kind, uint8_t *kindPre,
                                   uint64_t *typeRaw) const
{
    if (count)           *count           = ivars ? ivars->fIrqIndexCount : 0;
    if (countPre)        *countPre        = ivars ? ivars->fIrqIndexCountPre : 0;
    if (msixBase)        *msixBase        = ivars ? ivars->fMsixIndexBase
                                                  : MLX_IRQ_INDEX_NONE;
    if (asyncIndex)      *asyncIndex      = ivars ? ivars->fAsyncIndex : 0;
    if (completionIndex) *completionIndex = ivars ? ivars->fCompletionIndex : 0;
    if (probeStatus)     *probeStatus     = ivars ? ivars->fIrqIndexProbeStatus : 0;
    for (uint32_t i = 0; i < MLX_IRQ_INDEX_MAP; i++) {
        if (kind)    kind[i]    = ivars ? ivars->fIrqIndexKind[i] : 0;
        if (kindPre) kindPre[i] = ivars ? ivars->fIrqIndexKindPre[i] : 0;
        if (typeRaw) typeRaw[i] = ivars ? ivars->fIrqIndexType[i] : 0;
    }
}

/* QUERY_EQ (0x302): asks firmware what state an event queue is actually in.
 *
 * Everything else about the interrupt path can be verified by reading our own
 * code; this cannot. An EQ that still reads ARMED after firmware has written
 * an entry into it never fired, which means the silence is upstream of the
 * MSI-X table and no amount of programming the table will help. An EQ that
 * reads FIRED did raise its vector, and the message was lost afterwards.
 *
 * The output mailbox is larger than the raw debug-exec path allows, which is
 * why this has its own selector: query_eq_out is status and syndrome, then the
 * event queue context at 0x40, then the event bitmask and the page list. */
kern_return_t
MlxPCIDriver::QueryEqState(const struct mlx_query_eq_req *req,
                           struct mlx_query_eq_resp *resp)
{
    if (!req || !resp) return kIOReturnBadArgument;
    memset(resp, 0, sizeof(*resp));
    if (!ivars || !ivars->fCmd) return kIOReturnNotReady;

    uint8_t in[16] = {};
    mlxSetBits(in, 0x00, 16, MLX_CMD_OP_QUERY_EQ);
    /* eq_number lives at bit 0x58, after uid, op_mod and the reserved span —
     * the same place DestroyEQ writes it. Putting it at 0x20 lands in a
     * reserved field and firmware rejects the command with BAD_PARAM. */
    mlxSetBits(in, 0x58, 8, req->eqn);

    /* query_eq_out is far larger than the fixed header suggests: after status
     * and syndrome come the 80-byte event queue context, the four event
     * bitmask words, a large reserved span and then the page list. Handing
     * firmware a short output mailbox makes the command fail outright rather
     * than truncate, which is what a 256-byte buffer did here. One kilobyte
     * covers the fixed part and the page list of any EQ this driver creates. */
    const uint32_t outBytes = 0x400;
    uint8_t *out = IONewZero(uint8_t, outBytes);
    if (!out) return kIOReturnNoMemory;
    kern_return_t kr = ivars->fCmd->Exec(MLX_CMD_OP_QUERY_EQ, in, sizeof(in),
                                         out, outBytes, 5000);
    if (kr != kIOReturnSuccess) {
        /* Report the failure in the payload instead of only as a return code:
         * the kernel log channel is dead on this machine, so an opaque error
         * here costs a rebuild to diagnose. */
        resp->status = (uint32_t)kr;
        resp->fwStatus = ivars->fCmd->LastFwStatus();
        resp->syndrome = ivars->fCmd->LastSyndrome();
        resp->state = 0xffffffffu;
        IODelete(out, uint8_t, outBytes);
        return kIOReturnSuccess;
    }
    /* The event queue context begins at bit 0x80 of the output, i.e. byte 16 —
     * the same offset CreateEQ uses on the way in. mlxGetBits takes a BIT
     * offset, so the context is reached by advancing the POINTER by bytes and
     * then using the field offsets from mlx5_ifc_eqc_bits relative to it.
     * Adding 0x40 to the bit offsets instead reads neighbouring fields and
     * yields values that look plausible but are not the ones asked for. */
    const uint8_t *eqc = out + (0x80 / 8);
    resp->status        = 0;
    resp->state         = (uint32_t)mlxGetBits(eqc, 0x14, 4);
    resp->logEqSize     = (uint32_t)mlxGetBits(eqc, 0x63, 5);
    resp->uarPage       = (uint32_t)mlxGetBits(eqc, 0x68, 24);
    resp->intr          = (uint32_t)mlxGetBits(eqc, 0xb4, 12);
    /* Both counters are 24-bit and sit after a 0x60-bit reserved span that
     * ends at 0x140, each preceded by its own reserved byte: consumer at
     * 0x148, producer at 0x168. Reading them 0x18 bits early returns the value
     * shifted, which is what a producer counter of 0x230000 was. */
    resp->consumerIndex = (uint32_t)mlxGetBits(eqc, 0x148, 24);
    resp->producerIndex = (uint32_t)mlxGetBits(eqc, 0x168, 24);
    IODelete(out, uint8_t, outBytes);
    MLX_LOG("QUERY_EQ %u: state=0x%x intr=%u uar=%u ci=%u pi=%u",
            req->eqn, resp->state, resp->intr, resp->uarPage,
            resp->consumerIndex, resp->producerIndex);
    return kIOReturnSuccess;
}

/* Locates the MSI-X table in BAR0 and returns the byte offset of one entry,
 * or 0 when the capability is absent, the table lives in a BAR this driver
 * does not map, or the vector is outside the implemented table. Shared by the
 * readback and the writer so both agree on where the table is. */
uint64_t
MlxPCIDriver::MsixEntryOffset(uint32_t vector, uint32_t *tableSizeOut)
{
    if (tableSizeOut) *tableSizeOut = 0;
    if (!ivars || !ivars->fPci) return 0;
    uint16_t status = 0;
    ivars->fPci->ConfigurationRead16(0x06, &status);
    if (!(status & 0x0010)) return 0;
    uint8_t ptr = 0;
    ivars->fPci->ConfigurationRead8(0x34, &ptr);
    uint32_t cap = 0;
    for (uint32_t guard = 0; ptr >= 0x40 && guard < 48; guard++) {
        uint16_t idNext = 0;
        ivars->fPci->ConfigurationRead16(ptr, &idNext);
        if ((idNext & 0xff) == 0x11) { cap = ptr; break; }
        ptr = (uint8_t)(idNext >> 8);
    }
    if (!cap) return 0;
    uint16_t mc = 0;
    ivars->fPci->ConfigurationRead16((uint64_t)cap + 0x02, &mc);
    uint32_t tableSize = (uint32_t)(mc & 0x07ffu) + 1u;
    if (tableSizeOut) *tableSizeOut = tableSize;
    if (vector >= tableSize) return 0;
    uint32_t tbl = 0;
    ivars->fPci->ConfigurationRead32((uint64_t)cap + 0x04, &tbl);
    if ((tbl & 0x7u) != ivars->fBar0Index) return 0;
    return (uint64_t)(tbl & ~0x7u) + (uint64_t)vector * 16u;
}

/* Writes one MSI-X table entry.
 *
 * Normally the kernel does this: IOPCIFamily's initDevice, reached from
 * allocateDeviceInterrupts, fills every entry with an address and a data base
 * it obtains from the platform. On this machine that never ran to completion —
 * the capability reads enabled with 64 entries, yet every entry is zero, so a
 * raised vector became a DMA write of zero to physical address zero and went
 * nowhere. This writes the entry the kernel would have written.
 *
 * The address is the platform MSI doorbell and the data selects one of the
 * controller's vectors. Both come from the caller: the data base in particular
 * is not derivable inside a DEXT (it lives in the device tree, as
 * msi-vector-offset on the PCIe controller node), so the bring-up finds it by
 * trying candidates and seeing which one actually delivers. */
kern_return_t
MlxPCIDriver::ProgramMsix(struct mlx_program_msix_req *req)
{
    if (!req) return kIOReturnBadArgument;
    if (req->op > MLX_MSIX_OP_PEEK) return kIOReturnBadArgument;

    /* Raw access stays inside the two MSI-X structures and nowhere else: this
     * is a diagnostic, and a stray dword written into a device register window
     * would be a far worse bug than the one it is meant to find. */
    if (req->op == MLX_MSIX_OP_POKE || req->op == MLX_MSIX_OP_PEEK) {
        uint32_t tableSizeRaw = 0;
        if (!MsixEntryOffset(0, &tableSizeRaw) || !tableSizeRaw)
            return kIOReturnUnsupported;
        uint16_t status = 0;
        ivars->fPci->ConfigurationRead16(0x06, &status);
        uint8_t ptr = 0;
        ivars->fPci->ConfigurationRead8(0x34, &ptr);
        uint32_t cap = 0;
        for (uint32_t guard = 0; ptr >= 0x40 && guard < 48; guard++) {
            uint16_t idNext = 0;
            ivars->fPci->ConfigurationRead16(ptr, &idNext);
            if ((idNext & 0xff) == 0x11) { cap = ptr; break; }
            ptr = (uint8_t)(idNext >> 8);
        }
        if (!cap) return kIOReturnUnsupported;
        uint32_t which = req->addrLo;          /* 0 = table, 1 = pending bits */
        uint32_t reg = 0;
        ivars->fPci->ConfigurationRead32((uint64_t)cap + (which ? 0x08 : 0x04),
                                         &reg);
        if ((reg & 0x7u) != ivars->fBar0Index) return kIOReturnUnsupported;
        uint32_t dwords = which ? ((tableSizeRaw + 31u) / 32u)
                                : (tableSizeRaw * 4u);
        if (req->vector >= dwords) return kIOReturnBadArgument;
        uint64_t at = (uint64_t)(reg & ~0x7u) + (uint64_t)req->vector * 4u;
        if (req->op == MLX_MSIX_OP_POKE)
            ivars->fPci->MemoryWrite32(ivars->fBar0Index, at, req->data);
        else
            ivars->fPci->MemoryRead32(ivars->fBar0Index, at, &req->dataOut);
        return kIOReturnSuccess;
    }

    uint32_t tableSize = 0;
    uint64_t off = MsixEntryOffset(req->vector, &tableSize);
    if (!off) return tableSize ? kIOReturnBadArgument : kIOReturnUnsupported;

    if (req->op == MLX_MSIX_OP_MASK) {
        /* Read/modify/write so an entry that is already programmed keeps its
         * address and data; masking must not erase them. */
        uint32_t ctrl = 0;
        ivars->fPci->MemoryRead32(ivars->fBar0Index, off + 12, &ctrl);
        ctrl = req->masked ? (ctrl | 1u) : (ctrl & ~1u);
        ivars->fPci->MemoryWrite32(ivars->fBar0Index, off + 12, ctrl);
        return kIOReturnSuccess;
    }

    /* Mask first, then write address and data, then set the requested mask
     * state. Per the PCIe specification an entry must not be updated while it
     * is unmasked: the function may send a message built from a half-written
     * entry. */
    ivars->fPci->MemoryWrite32(ivars->fBar0Index, off + 12, 1u);
    ivars->fPci->MemoryWrite32(ivars->fBar0Index, off + 0, req->addrLo);
    ivars->fPci->MemoryWrite32(ivars->fBar0Index, off + 4, req->addrHi);
    ivars->fPci->MemoryWrite32(ivars->fBar0Index, off + 8, req->data);
    ivars->fPci->MemoryWrite32(ivars->fBar0Index, off + 12,
                               req->masked ? 1u : 0u);
    MLX_LOG("MSI-X vector %u programmed: addr=0x%08x%08x data=0x%x masked=%u",
            req->vector, req->addrHi, req->addrLo, req->data, req->masked);
    return kIOReturnSuccess;
}

/* Negotiated PCIe link, from the PCI Express Capability (id 0x10) Link Status
 * register at cap+0x12: bits 3:0 encode the generation and bits 9:4 the lane
 * count. This is what a measured transfer has to be divided by for the result
 * to mean anything on another machine — on this one the card sits behind a
 * Thunderbolt tunnel and negotiates far less than its own wire rate. */
void
MlxPCIDriver::GetPcieLink(uint32_t *speed, uint32_t *width)
{
    if (speed) *speed = 0;
    if (width) *width = 0;
    if (!ivars || !ivars->fPci) return;
    uint16_t status = 0;
    ivars->fPci->ConfigurationRead16(0x06, &status);
    if (!(status & 0x0010)) return;          /* no capability list */
    uint8_t ptr = 0;
    ivars->fPci->ConfigurationRead8(0x34, &ptr);
    for (uint32_t guard = 0; ptr >= 0x40 && guard < 48; guard++) {
        uint16_t idNext = 0;
        ivars->fPci->ConfigurationRead16(ptr, &idNext);
        if ((idNext & 0xff) == 0x10) {       /* PCI Express Capability */
            uint16_t link = 0;
            ivars->fPci->ConfigurationRead16((uint64_t)ptr + 0x12, &link);
            if (speed) *speed = link & 0xfu;
            if (width) *width = (link >> 4) & 0x3fu;
            return;
        }
        ptr = (uint8_t)(idNext >> 8);
    }
}

/* Reads the MSI-X capability out of config space and the head of the table
 * and the pending bits out of the BAR the capability points at. Read-only:
 * nothing here writes, masks or enables anything, so it can be taken at any
 * time, including around a traffic burst to see whether pending bits latch.
 *
 * Layout (PCIe base spec, MSI-X capability id 0x11):
 *   cap+0x00  cap id | next
 *   cap+0x02  message control: bit 15 enable, bit 14 function mask,
 *                              bits 10:0 table size minus one
 *   cap+0x04  table offset (bits 31:3) | BIR (bits 2:0)
 *   cap+0x08  PBA offset   (bits 31:3) | BIR (bits 2:0)
 * Each table entry is 16 bytes: addr low, addr high, data, vector control. */
kern_return_t
MlxPCIDriver::ReadMsixState(struct mlx_msix_state_resp *out)
{
    if (!out) return kIOReturnBadArgument;
    memset(out, 0, sizeof(*out));
    if (!ivars || !ivars->fPci) return kIOReturnNotReady;
    out->barIndexUsed = MLX_IRQ_INDEX_NONE;

    uint16_t cmd = 0;
    ivars->fPci->ConfigurationRead16(0x04, &cmd);
    out->commandReg = cmd;

    /* Walk the capability list rather than trusting a known offset: the same
     * code has to survive a different card. */
    uint16_t status = 0;
    ivars->fPci->ConfigurationRead16(0x06, &status);
    if (!(status & 0x0010)) return kIOReturnUnsupported;  /* no capability list */
    uint8_t ptr = 0;
    ivars->fPci->ConfigurationRead8(0x34, &ptr);
    uint32_t cap = 0;
    for (uint32_t guard = 0; ptr >= 0x40 && guard < 48; guard++) {
        uint16_t idNext = 0;
        ivars->fPci->ConfigurationRead16(ptr, &idNext);
        if ((idNext & 0xff) == 0x11) { cap = ptr; break; }
        ptr = (uint8_t)(idNext >> 8);
    }
    if (!cap) return kIOReturnUnsupported;
    out->capOffset = cap;

    uint16_t mc = 0;
    ivars->fPci->ConfigurationRead16((uint64_t)cap + 0x02, &mc);
    out->messageControl = mc;
    out->tableSize = (uint32_t)(mc & 0x07ffu) + 1u;

    uint32_t tbl = 0, pba = 0;
    ivars->fPci->ConfigurationRead32((uint64_t)cap + 0x04, &tbl);
    ivars->fPci->ConfigurationRead32((uint64_t)cap + 0x08, &pba);
    out->tableBir    = tbl & 0x7u;
    out->tableOffset = tbl & ~0x7u;
    out->pbaBir      = pba & 0x7u;
    out->pbaOffset   = pba & ~0x7u;

    /* The BIR is a BAR number while fBar0Index is the memory index used with
     * MemoryRead32; the two numbering schemes coincide on this card, where
     * both the table and the PBA live in BAR0 at memory index 0. A card that
     * puts them elsewhere reports the BIRs and leaves the payload empty rather
     * than reading through an index that means something else. */
    if (out->tableBir != ivars->fBar0Index || out->pbaBir != ivars->fBar0Index) {
        out->status = kIOReturnUnsupported;
        return kIOReturnSuccess;
    }
    out->barIndexUsed = ivars->fBar0Index;
    out->preConfigureEntry0AddrLo = ivars->fMsixPreConfigureEntry0AddrLo;
    out->preConfigureEntry0Data = ivars->fMsixPreConfigureEntry0Data;

    uint32_t entries = out->tableSize;
    if (entries > MLX_MSIX_TABLE_SNAPSHOT) entries = MLX_MSIX_TABLE_SNAPSHOT;
    for (uint32_t i = 0; i < entries; i++) {
        uint64_t base = (uint64_t)out->tableOffset + (uint64_t)i * 16u;
        ivars->fPci->MemoryRead32(ivars->fBar0Index, base + 0,
                                  &out->entry[i].addrLo);
        ivars->fPci->MemoryRead32(ivars->fBar0Index, base + 4,
                                  &out->entry[i].addrHi);
        ivars->fPci->MemoryRead32(ivars->fBar0Index, base + 8,
                                  &out->entry[i].data);
        ivars->fPci->MemoryRead32(ivars->fBar0Index, base + 12,
                                  &out->entry[i].vectorControl);
    }
    out->entriesRead = entries;

    uint32_t words = (out->tableSize + 31u) / 32u;
    if (words > MLX_MSIX_PBA_WORDS) words = MLX_MSIX_PBA_WORDS;
    for (uint32_t i = 0; i < words; i++)
        ivars->fPci->MemoryRead32(ivars->fBar0Index,
                                  (uint64_t)out->pbaOffset + i * 4u,
                                  &out->pba[i]);
    out->pbaWords = words;
    return kIOReturnSuccess;
}

void
MlxPCIDriver::GetCompletionEqVariants(uint32_t *variant, uint32_t *tried,
                                      uint32_t *syndromes) const
{
    if (variant) *variant = ivars ? ivars->fCqEqVariant : 0;
    if (tried)   *tried   = ivars ? ivars->fCqEqVariantTried : 0;
    if (syndromes)
        for (uint32_t i = 0; i < 4; i++)
            syndromes[i] = ivars ? ivars->fCqEqVariantSyndrome[i] : 0;
}

void
MlxPCIDriver::GetCompletionEqStatus(uint32_t *status, uint32_t *stage,
                                    uint32_t *syndrome, uint32_t *fwStatus) const
{
    if (status)   *status   = ivars ? ivars->fCqEqStatus : 0;
    if (stage)    *stage    = ivars ? ivars->fCqEqStage
                                    : (uint32_t)MLX_CQEQ_STAGE_NOT_ATTEMPTED;
    if (syndrome) *syndrome = ivars ? ivars->fCqEqSyndrome : 0;
    if (fwStatus) *fwStatus = ivars ? ivars->fCqEqFwStatus : 0;
}

/* Brings up the dedicated completion EQ, trying the CreateEQ inputs firmware
 * is known to object to. ConnectX-4 rejects the baseline here with BAD_PARAM
 * and the kernel log channel is dead on the dev machine, so each attempt's
 * syndrome is kept for QueryInterrupts instead. Failure is not fatal: without
 * this EQ, MlxCQ::CreateCQ binds CQs to the async EQ and completion events
 * ride vector 0. */
void
MlxPCIDriver::BringUpCompletionEq()
{
    if (!ivars || !ivars->fCompletionInterrupt || ivars->fCompletionEQ) return;

    struct Variant { uint32_t intr; uint32_t logSize; bool ownUar; bool usable; };
#if MLX_SINGLE_MSIX_VECTOR
    static const Variant kVariants[] = {
        { 0, 8, false, true  },  /* shared first sub-vector, 256 entries */
    };
#else
    /* Own sub-vector first, sharing vector 0 as the fallback. The fallback is
     * exactly the old behaviour, so firmware that refuses intr=1 leaves the
     * driver no worse off; which variant won is reported as fCqEqVariant and
     * in the log line below. ProbeCompletionVector can rebind at runtime. */
    static const Variant kVariants[] = {
        { 1, 8, false, true  },  /* own sub-vector, host index 1 */
        { 0, 8, false, true  },  /* fallback: shared with the async vector */
    };
#endif
    const uint32_t count = sizeof(kVariants) / sizeof(kVariants[0]);

    for (uint32_t i = 0; i < count; i++) {
        ivars->fCqEqVariantTried |= (1u << i);
        MlxEQ *eq = new MlxEQ();
        if (!eq) {
            RecordCompletionEqResult(MLX_CQEQ_STAGE_ALLOC, kIOReturnNoMemory);
            return;
        }
        uint32_t stage = MLX_CQEQ_STAGE_INIT;
        uint32_t eqn = 0;
        kern_return_t kr = eq->Init(this, kVariants[i].intr, true,
                                    kVariants[i].logSize);
        if (kr == kIOReturnSuccess) {
            if (kVariants[i].ownUar && ivars->fUAR) {
                uint32_t uar = 0;
                if (ivars->fUAR->AllocUAR(&uar) == kIOReturnSuccess)
                    eq->SetUarPage(uar);
            }
            stage = MLX_CQEQ_STAGE_CREATE;
            kr = eq->CreateEQ(&eqn);
        }
        ivars->fCqEqVariantSyndrome[i] =
            ivars->fCmd ? ivars->fCmd->LastSyndrome() : 0;
        if (kr != kIOReturnSuccess) {
            RecordCompletionEqResult(stage, kr);
            eq->Free();
            delete eq;
            continue;
        }
        ivars->fCqEqVariant = i + 1;
        if (!kVariants[i].usable) {
            /* Accepted only on the async vector: that pins the objection to
             * the interrupt index, and the EQ itself is no use there. */
            (void)eq->DestroyEQ(eqn);
            eq->Free();
            delete eq;
            MLX_LOG("completion EQ: only intr=0 accepted — firmware rejects "
                    "the second interrupt vector");
            return;
        }
        RecordCompletionEqResult(MLX_CQEQ_STAGE_OK, kIOReturnSuccess);
        ivars->fCompletionEQ = eq;
        (void)eq->Arm();
        MLX_LOG("completion EQ created eqn=%u variant=%u vector=%u logSize=%u",
                eqn, i + 1, kVariants[i].intr, kVariants[i].logSize);
        return;
    }
    MLX_LOG("completion EQ unavailable after %u variants — CQs bind to the "
            "async EQ and completion events ride vector 0", count);
}

/* Rebinds the completion EQ to a chosen MSI-X index, so the two failure modes
 * behind a silent vector can be told apart: either MSI-X is not delivered to
 * this dext at all, or the firmware's intr index does not line up with the
 * dispatch-source index the handler is attached to. Refused while any CQ is
 * live, because a CQ carries the old EQ number in its c_eqn. */
kern_return_t
MlxPCIDriver::ProbeCompletionVector(uint32_t intr, uint32_t *outEqn)
{
    if (!ivars || !ivars->fCompletionInterrupt) return kIOReturnNotReady;
    if (intr > 1) return kIOReturnBadArgument;
    if (ivars->fRoCE && ivars->fRoCE->GetCQ() &&
        ivars->fRoCE->GetCQ()->LiveCount())
        return kIOReturnBusy;

    if (ivars->fCompletionEQ) {
        uint32_t eqn = ivars->fCompletionEQ->EqNumber();
        if (eqn) (void)ivars->fCompletionEQ->DestroyEQ(eqn);
        ivars->fCompletionEQ->Free();
        delete ivars->fCompletionEQ;
        ivars->fCompletionEQ = NULL;
    }

    MlxEQ *eq = new MlxEQ();
    if (!eq) return kIOReturnNoMemory;
    uint32_t eqn = 0;
    kern_return_t kr = eq->Init(this, intr, true, 8);
    uint32_t stage = MLX_CQEQ_STAGE_INIT;
    if (kr == kIOReturnSuccess) {
        stage = MLX_CQEQ_STAGE_CREATE;
        kr = eq->CreateEQ(&eqn);
    }
    if (kr != kIOReturnSuccess) {
        RecordCompletionEqResult(stage, kr);
        eq->Free();
        delete eq;
        MLX_LOG("completion vector probe intr=%u failed: 0x%x", intr, kr);
        return kr;
    }
    RecordCompletionEqResult(MLX_CQEQ_STAGE_OK, kIOReturnSuccess);
    ivars->fCompletionEQ = eq;
    if (ivars->fRoCE) ivars->fCompletionEQ->AddNotifier(ivars->fRoCE);
    (void)eq->Arm();
    if (outEqn) *outEqn = eqn;
    MLX_LOG("completion vector probe: EQ %u now on intr=%u", eqn, intr);
    return kIOReturnSuccess;
}

/* Records why the completion EQ did not come up. Called from both Phase 2
 * bring-up paths so the answer survives a dead kernel log channel. */
void
MlxPCIDriver::RecordCompletionEqResult(uint32_t stage, kern_return_t kr)
{
    if (!ivars) return;
    ivars->fCqEqStage = stage;
    ivars->fCqEqStatus = (uint32_t)kr;
    ivars->fCqEqSyndrome = ivars->fCmd ? ivars->fCmd->LastSyndrome() : 0;
    ivars->fCqEqFwStatus = ivars->fCmd ? ivars->fCmd->LastFwStatus() : 0;
}

void
MlxPCIDriver::StopInterrupts()
{
    if (!ivars) return;
    if (ivars->fCompletionWaitQueue)
        ivars->fCompletionWaitQueue->WakeupWithOptions(
            &ivars->fCompletionGeneration, kIODispatchQueueWakeupAll);
    if (ivars->fAsyncInterrupt)
        (void)ivars->fAsyncInterrupt->SetEnableWithCompletion(false, nullptr);
    if (ivars->fCompletionInterrupt &&
        ivars->fCompletionInterrupt != ivars->fAsyncInterrupt)
        (void)ivars->fCompletionInterrupt->SetEnableWithCompletion(false, nullptr);
    /* DriverKit disable/cancel completion is asynchronous. Retain the source
     * objects until the DEXT process exits instead of racing an in-flight IRQ. */
}

/* Releases clients blocked in WaitCompletionEvent. Called from whichever
 * poller actually drained the completion EQEs: the dedicated completion
 * vector when firmware granted that EQ, otherwise the async vector and the
 * EQ timer, because MlxCQ::CreateCQ then binds every CQ to the async EQ. */
void
MlxPCIDriver::SignalCompletionEvent(uint32_t completions)
{
    if (!completions || !ivars || !ivars->fCompletionWaitQueue) return;
    ivars->fCompletionWaitQueue->DispatchSync(^{
        ivars->fCompletionGeneration++;
        __atomic_fetch_add(&ivars->fCompletionEvents, 1, __ATOMIC_RELAXED);
        ivars->fCompletionWaitQueue->WakeupWithOptions(
            &ivars->fCompletionGeneration, kIODispatchQueueWakeupAll);
    });
}

void
MlxPCIDriver::AsyncInterruptOccurred_Impl(OSAction *, uint64_t, uint64_t)
{
    if (!ivars || ivars->fStopping || !ivars->fEQ) return;
    __atomic_fetch_add(&ivars->fAsyncIrqCount, 1, __ATOMIC_RELAXED);
    uint32_t totalAsyncCompletions = 0;
    uint32_t totalCompletionEqes = 0;
    /* One interrupt now represents both EQs. Drain both to quiescence, but
     * keep a hard bound so a hot ring cannot monopolize the DriverKit queue. */
    for (uint32_t pass = 0; pass < 16; pass++) {
        uint32_t asyncCompletions = 0;
        uint32_t completionEqes = 0;
        uint32_t asyncEntries = ivars->fEQ->Poll(&asyncCompletions);
        uint32_t completionEntries = ivars->fCompletionEQ
            ? ivars->fCompletionEQ->Poll(&completionEqes) : 0;
        totalAsyncCompletions += asyncCompletions;
        totalCompletionEqes += completionEqes;
        if (!asyncEntries && !completionEntries) break;
    }
    (void)ivars->fEQ->Arm();
    if (ivars->fCompletionEQ) (void)ivars->fCompletionEQ->Arm();
    if (totalCompletionEqes) {
        __atomic_fetch_add(&ivars->fCompletionIrqCount, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&ivars->fIrqCompletionEqes, totalCompletionEqes,
                           __ATOMIC_RELAXED);
        __atomic_store_n(&ivars->fLastCompletionIrqNs,
                         clock_gettime_nsec_np(CLOCK_UPTIME_RAW),
                         __ATOMIC_RELEASE);
    }
    SignalCompletionEvent(totalAsyncCompletions + totalCompletionEqes);
}

void
MlxPCIDriver::CompletionInterruptOccurred_Impl(OSAction *, uint64_t, uint64_t)
{
    if (!ivars || ivars->fStopping || !ivars->fCompletionEQ) return;
    __atomic_fetch_add(&ivars->fCompletionIrqCount, 1, __ATOMIC_RELAXED);
    uint32_t processed = 0;
    (void)ivars->fCompletionEQ->Poll(&processed);
    (void)ivars->fCompletionEQ->Arm();
    if (processed) {
        __atomic_fetch_add(&ivars->fIrqCompletionEqes, processed, __ATOMIC_RELAXED);
        __atomic_store_n(&ivars->fLastCompletionIrqNs, clock_gettime_nsec_np(CLOCK_UPTIME_RAW), __ATOMIC_RELEASE);
    }
    SignalCompletionEvent(processed);
}

kern_return_t
MlxPCIDriver::WaitCompletionEvent(uint64_t generation, uint32_t timeoutMs,
                                  uint64_t *newGeneration)
{
    if (!ivars || !newGeneration || !ivars->fCompletionWaitQueue ||
        !CompletionEventReady())
        return kIOReturnNotReady;
    __block kern_return_t result = kIOReturnSuccess;
    ivars->fCompletionWaitQueue->DispatchSync(^{
        if (DmaQuarantined() || ivars->fStopping) { result = kIOReturnNotReady; return; }
        if (ivars->fCompletionGeneration == generation) {
            uint64_t timeout = (uint64_t)(timeoutMs ? timeoutMs : 1000u) *
                               1000ULL * 1000ULL;
            result = ivars->fCompletionWaitQueue->Sleep(
                &ivars->fCompletionGeneration, timeout);
        }
        *newGeneration = ivars->fCompletionGeneration;
        if (DmaQuarantined() || ivars->fStopping) result = kIOReturnNotReady;
    });
    if (*newGeneration != generation)
        __atomic_fetch_add(&ivars->fCompletionWakeups, 1, __ATOMIC_RELAXED);
    return result;
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
    uint64_t tick = __atomic_add_fetch(&ivars->fEqTickCount, 1, __ATOMIC_RELAXED);
    if (tick == 1) MLX_LOG("EQ timer first callback entered");
    if (__atomic_load_n(&ivars->fEqTimerPaused, __ATOMIC_ACQUIRE)) {
        (void)ivars->fEqTimer->WakeAtTime(
            kIOTimerClockUptimeRaw, MlxEqPollDeadline(10), 1000000);
        return;
    }
    if (ivars->fEQ) {
        uint32_t completions = 0;
        (void)ivars->fEQ->Poll(&completions);
        __atomic_fetch_add(&ivars->fTimerCompletionEqes, completions, __ATOMIC_RELAXED);
        SignalCompletionEvent(completions);
    }
    /* The dedicated completion EQ needs the same insurance as the async one.
     * Once it exists, MlxCQ::CreateCQ binds CQs to it, so if its vector stays
     * silent nothing else drains the ring and every armed CQ waits out the
     * client's backstop instead. Poll re-arms on drain. */
    if (ivars->fCompletionEQ) {
        uint32_t completions = 0;
        (void)ivars->fCompletionEQ->Poll(&completions);
        __atomic_fetch_add(&ivars->fTimerCompletionEqes, completions, __ATOMIC_RELAXED);
        SignalCompletionEvent(completions);
    }
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
        bool inUse = ivars->fRoCE && ivars->fRoCE->GetCQ() &&
                     ivars->fRoCE->GetCQ()->LiveCount();
        uint64_t periodMs = mlxEqPollPeriodMs(inUse, CompletionInterruptReady());
        kern_return_t kr = ivars->fEqTimer->WakeAtTime(
            kIOTimerClockUptimeRaw, MlxEqPollDeadline(periodMs), 1000000);
        if (kr != kIOReturnSuccess) {
            ivars->fRuntimePagesOk = false;
            MLX_LOG("EQ timer rearm failed tick=%llu kr=0x%x",
                    (unsigned long long)tick, kr);
        } else if (tick == 1) {
            MLX_LOG("EQ timer first callback rearmed");
        }
    }
    if ((tick % 500) == 0)
        MLX_DBG("EQ timer heartbeat ticks=%llu", (unsigned long long)tick);
}

kern_return_t
MlxPCIDriver::SetEqTimerPaused(bool paused)
{
    if (!ivars || !ivars->fEqTimer) return kIOReturnNotReady;
    __atomic_store_n(&ivars->fEqTimerPaused, paused ? 1u : 0u, __ATOMIC_RELEASE);
    MLX_LOG("EQ timer diagnostic pause=%u", paused ? 1u : 0u);
    return kIOReturnSuccess;
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
    if (ivars->fCompletionEQ && ivars->fCompletionEQ->EqNumber()) {
        uint32_t eqn = ivars->fCompletionEQ->EqNumber();
        if (ivars->fCompletionEQ->DestroyEQ(eqn) != kIOReturnSuccess)
            EnterDmaQuarantine(0x43455100u | (eqn & 0xffu));
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
        (void)ivars->fEQ->Arm();
    }
    /* Not fatal: without it, CQs bind to the async EQ (MlxCQ::CreateCQ) and
     * completion events ride vector 0. */
    BringUpCompletionEq();
    ivars->fRoCE = new MlxRoCE();
    if (!ivars->fRoCE)
        MLX_PHASE2_FAIL(MLX_PHASE2_SUB_ROCE_INIT, kIOReturnNoMemory);
    {
        kern_return_t kr = ivars->fRoCE->Init(this, ivars->fHCA);
        if (kr != kIOReturnSuccess)
            MLX_PHASE2_FAIL(MLX_PHASE2_SUB_ROCE_INIT, kr);
    }
    ivars->fEQ->AddNotifier(ivars->fRoCE);
    if (ivars->fCompletionEQ)
        ivars->fCompletionEQ->AddNotifier(ivars->fRoCE);
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
    if (ivars->fCompletionEQ) { ivars->fCompletionEQ->Free(); delete ivars->fCompletionEQ; ivars->fCompletionEQ = NULL; }
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
    /* Index 0 is a real interrupt index, so "not found" needs its own value.
     * Until StartInterrupts runs, the map reports the historical binding. */
    ivars->fMsixIndexBase       = MLX_IRQ_INDEX_NONE;
    ivars->fAsyncIndex          = 0;
    ivars->fCompletionIndex     = MLX_SINGLE_MSIX_VECTOR ? 0 : 1;
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
    /* Stage 0 means "MSI-X is live"; until Start runs it, say so honestly. */
    ivars->fIrqSetupStage       = MLX_IRQ_STAGE_NOT_ATTEMPTED;
    ivars->fCqEqStage           = MLX_CQEQ_STAGE_NOT_ATTEMPTED;
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
    MLX_LOG("Start — ConnectX RDMA DEXT initializing (build p0-p1-dma-lifetime-runtime-1)");

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
    /* MSI-X is configured after the FLR below, not here. A Function Level
     * Reset clears the device's MSI-X capability and table, so vectors
     * configured before it are silently dead: ConfigureInterrupts reports
     * success, dispatch sources exist, and no interrupt is ever raised. That
     * also puts vector 1 out of the firmware's range, which is why CREATE_EQ
     * accepted intr=0 and rejected intr=1 with the same syndrome regardless of
     * ring size or UAR page. Nothing before FwInit needs interrupts: firmware
     * commands poll the command queue and the EQs do not exist yet. */

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
        MlxReleaseQuarantinedPagesAfterReset(ivars->fPci);
    } else {
        if (!PerformFlr()) {
            MLX_LOG("Start: FLR verification failed — initialization aborted");
            Cleanup();
            ivars->fPci->Close(this, 0);
            return failStart(kIOReturnNoDevice);
        }
    }

    /* Claim the MSI-X vectors BEFORE firmware initialization: the FLR above
     * cleared the enable bit, and firmware appears to decide once, at its own
     * init, whether this function has message interrupts. See EnableMsixEarly. */
    (void)EnableMsixEarly();

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
            LogMsixEnable("after firmware init");
            /* Now that the device has been reset and the firmware is up, claim
             * the MSI-X vectors. Additive: if two vectors are unavailable the
             * timer/direct-CQ polling path remains valid. */
            (void)StartInterrupts();
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
                            (void)ivars->fEQ->Arm();
                            MLX_LOG("CREATE_EQ ok — eqn=%u (MSI-X + poll fallback)", eqn);
                        }
                    } else {
                        if (ivars->fEQ) { delete ivars->fEQ; ivars->fEQ = NULL; }
                    }

                    BringUpCompletionEq();

                    ivars->fRoCE = new MlxRoCE();
                    if (!ivars->fRoCE ||
                        ivars->fRoCE->Init(this, ivars->fHCA) != kIOReturnSuccess) {
                        MLX_LOG("Phase 2 FAIL: MlxRoCE init");
                    } else {
                        if (ivars->fEQ) ivars->fEQ->AddNotifier(ivars->fRoCE);
                        if (ivars->fCompletionEQ)
                            ivars->fCompletionEQ->AddNotifier(ivars->fRoCE);
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
    MLX_LOG("Start complete (build p0-p1-dma-lifetime-runtime-1) — device 0x%04x, HCA init done, phase2=%s fw_owned=%u ambiguous=%u host=%u returned=%u accounting=%s",
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
    StopInterrupts();

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
    if (ivars->fCmd->IsQuarantined()) {
        EnterDmaQuarantine(0x434d4400u | (opcode & 0xffu));
        return kr; /* no EQ MMIO or nested page commands after fencing */
    }
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
MlxEQ *               MlxPCIDriver::GetCompletionEQ()  { return ivars->fCompletionEQ; }
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
bool MlxPCIDriver::CompletionInterruptReady() const
{
    /* EQ creation, a timer, or an async-vector interrupt is not proof that the
     * completion vector delivers. Require the dedicated EQ/source and a
     * completion EQE observed on that vector in this reset epoch. */
    if (!CompletionEventReady() || !ivars->fCompletionEQ ||
        !ivars->fCompletionInterrupt)
        return false;
    if (MLX_SINGLE_MSIX_VECTOR) {
        uint64_t irq = __atomic_load_n(&ivars->fAsyncIrqCount, __ATOMIC_ACQUIRE);
        uint64_t last = __atomic_load_n(&ivars->fLastCompletionIrqNs, __ATOMIC_ACQUIRE);
        return irq && last && clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - last < 5000000000ULL;
    }
    uint64_t last = __atomic_load_n(&ivars->fLastCompletionIrqNs, __ATOMIC_ACQUIRE);
    uint64_t irq = __atomic_load_n(&ivars->fCompletionIrqCount, __ATOMIC_ACQUIRE);
    return irq && last && clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - last < 5000000000ULL;
}

bool MlxPCIDriver::CompletionEventReady() const
{
    return ivars && !ivars->fStopping && !DmaQuarantined() &&
           ivars->fCompletionWaitQueue && ivars->fEQ && ivars->fEqTimer;
}

void MlxPCIDriver::GetRuntimeStatus(struct mlx_runtime_resp *r)
{
    memset(r, 0, sizeof(*r)); r->version = MLX_RUNTIME_VERSION; r->size = sizeof(*r);
    r->deviceEpoch = __atomic_load_n(&ivars->fDeviceEpoch, __ATOMIC_ACQUIRE);
    if (ivars->fDMA) ivars->fDMA->GetPinnedStats(&r->pinnedBytes, &r->peakPinnedBytes, &r->pinFailures);
    r->clientPinnedLimit = MLX_UC_PINNED_BYTES_PER_CLIENT;
    r->devicePinnedLimit = MLX_UC_PINNED_BYTES_PER_DEVICE;
    r->quarantineBytes = __atomic_load_n(&ivars->fQuarantineBytes, __ATOMIC_RELAXED);
    r->quarantineObjects = __atomic_load_n(&ivars->fQuarantineObjects, __ATOMIC_RELAXED);
    r->irqCompletionEqes = __atomic_load_n(&ivars->fIrqCompletionEqes, __ATOMIC_RELAXED);
    r->timerCompletionEqes = __atomic_load_n(&ivars->fTimerCompletionEqes, __ATOMIC_RELAXED);
    r->lastCompletionIrqNs = __atomic_load_n(&ivars->fLastCompletionIrqNs, __ATOMIC_ACQUIRE);
    r->quarantined = DmaQuarantined();
    r->bmeFenced = __atomic_load_n(&ivars->fBmeFenced, __ATOMIC_ACQUIRE);
    r->completionEqReady = CompletionEventReady();
    r->completionIrqProven = CompletionInterruptReady();
}

uint64_t MlxPCIDriver::CompletionEventCount() const
{
    return ivars ? __atomic_load_n(&ivars->fCompletionEvents, __ATOMIC_RELAXED) : 0;
}

uint64_t MlxPCIDriver::CompletionWakeupCount() const
{
    return ivars ? __atomic_load_n(&ivars->fCompletionWakeups, __ATOMIC_RELAXED) : 0;
}

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
    return ivars && __atomic_load_n(&ivars->fDmaQuarantined, __ATOMIC_ACQUIRE);
}

void
MlxPCIDriver::RetainDmaUntilReset(void *memPtr, void *dmaPtr, uint32_t reason)
{
    IOMemoryDescriptor *mem = static_cast<IOMemoryDescriptor *>(memPtr);
    IODMACommand *dma = static_cast<IODMACommand *>(dmaPtr);
    if (!mem && !dma) return;
    EnterDmaQuarantine(reason);
    uint64_t bytes = 0;
    if (mem) (void)mem->GetLength(&bytes);
    __atomic_fetch_add(&ivars->fQuarantineBytes, bytes, __ATOMIC_RELAXED);
    __atomic_fetch_add(&ivars->fQuarantineObjects, 1, __ATOMIC_RELAXED);
    MlxRetainQuarantinedDummy(mem, dma, ivars->fPci);
}

void
MlxPCIDriver::EnterDmaQuarantine(uint32_t reason)
{
    if (!ivars) return;
    bool first = !__atomic_exchange_n(&ivars->fDmaQuarantined, true, __ATOMIC_ACQ_REL);
    if (first) {
        bool fenced = DisableBusMasterAndVerify();
        __atomic_store_n(&ivars->fBmeFenced, fenced, __ATOMIC_RELEASE);
        MLX_LOG("DMA quarantine reason=0x%x BME_fenced=%u — mappings retained until reset",
                reason, fenced ? 1u : 0u);
        if (ivars->fCompletionWaitQueue)
            ivars->fCompletionWaitQueue->DispatchSync(^{
                ivars->fCompletionGeneration++;
                ivars->fCompletionWaitQueue->WakeupWithOptions(
                    &ivars->fCompletionGeneration, kIODispatchQueueWakeupAll);
            });
    }
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

    /* Reset through the framework rather than by poking the initiator bit.
     * IOPCIDevice::Reset routes to IOPCIBridge::resetDeviceGated, which calls
     * restoreDeviceState on the way out, and that reaches
     * IOPCIMessagedInterruptController::initDevice — the only code in
     * IOPCIFamily that ever writes address and data into the MSI-X table.
     * A reset performed by writing the bit ourselves is invisible to the
     * kernel, so the table stays wiped and a raised vector becomes a write of
     * zero to physical address zero, which is what every measurement showed.
     * The manual path stays as a fallback: the runtime ownership takeover is
     * not a configuration Apple tests, so Reset may well be refused here. */
    bool frameworkReset = false;
    kern_return_t resetKr = ivars->fPci->Reset(kIOPCIDeviceResetTypeFunctionReset,
                                               kIOPCIDeviceResetOptionNone);
    if (resetKr == kIOReturnSuccess) {
        frameworkReset = true;
        MLX_LOG("PerformFlr: framework Reset() ok — kernel owns the MSI-X restore");
    } else {
        MLX_LOG("PerformFlr: framework Reset() refused (0x%x) — manual FLR bit",
                resetKr);
    }

    if (!frameworkReset) {
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
    }

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
    MlxReleaseQuarantinedPagesAfterReset(ivars->fPci);
    __atomic_store_n(&ivars->fDmaQuarantined, false, __ATOMIC_RELEASE);
    MLX_LOG("PerformFlr: verified DMA boundary");
    __atomic_store_n(&ivars->fDeviceEpoch, clock_gettime_nsec_np(CLOCK_UPTIME_RAW), __ATOMIC_RELEASE);
    __atomic_store_n(&ivars->fLastCompletionIrqNs, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&ivars->fAsyncIrqCount, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&ivars->fCompletionIrqCount, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&ivars->fIrqCompletionEqes, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&ivars->fTimerCompletionEqes, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&ivars->fQuarantineBytes, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&ivars->fQuarantineObjects, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&ivars->fBmeFenced, false, __ATOMIC_RELEASE);
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
    if (ivars->fCompletionEQ) { ivars->fCompletionEQ->Free(); delete ivars->fCompletionEQ; ivars->fCompletionEQ = NULL; }
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
    /* The FLR above cleared the device's MSI-X capability; repeat the single
     * provider allocation before Phase 2 recreates the EQs. */
    (void)EnableMsixEarly();
    (void)StartInterrupts();
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
        if (ivars->fCompletionEQ) {
            ivars->fCompletionEQ->MarkDestroyedByTeardown();
            ivars->fCompletionEQ->Free(); delete ivars->fCompletionEQ;
            ivars->fCompletionEQ = NULL;
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
    StopInterrupts();
    if (ivars->fRoCE)    { ivars->fRoCE->Free(); delete ivars->fRoCE; ivars->fRoCE = NULL; }

    DestroyPhase2FirmwareResources();

    if (ivars->fFwPages) {
        if (ivars->fFwPages->IsQuarantined()) {
            ivars->fFwPages->EnterQuarantine();
            MlxRetainQuarantinedPages(ivars->fFwPages, ivars->fPci);
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
    if (ivars->fCompletionEQ) { ivars->fCompletionEQ->Free(); delete ivars->fCompletionEQ; ivars->fCompletionEQ = NULL; }
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
        caps.maxMttLogPageSize = parsed.maxMkeyLogEntitySizeMtt;
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
    MLX_LOG("QUERY_HCA_CAP: logMaxQp=%u logMaxCq=%u logMaxMkey=%u logMaxMsg=%u logMaxSrqSz=%u logPgSz=%u portType=%u numPorts=%u roce=%u uar4k=%u cacheLine128=%u bf=%u logBfRegSize=%u gidTable=%u roceVersions=0x%x udpDst=%u udpSrcMin=%u atomicOps=0x%x atomicSizeQp=0x%x atomicMode=%u",
            parsed.logMaxQp, parsed.logMaxCq, parsed.logMaxMkey,
            parsed.logMaxMsg, parsed.logMaxSrqSz, parsed.logPgSz,
            parsed.portType, parsed.numPorts, haveRoce ? 1 : 0,
            parsed.uar4k ? 1 : 0, parsed.cacheLine128 ? 1 : 0,
            parsed.bf ? 1 : 0, parsed.logBfRegSize,
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
bool MlxPCIDriver::DisableBusMasterAndVerify()
{
    if (!ivars || !ivars->fPci) return false;
    uint16_t before = 0xffffu, after = 0xffffu;
    ivars->fPci->ConfigurationRead16(4, &before);
    if (before == 0xffffu) return false;
    ivars->fPci->ConfigurationWrite16(4, mlxFencedPciCommand(before));
    ivars->fPci->ConfigurationRead16(4, &after);
    /* Readback verifies BME only; it is NOT proof of drained transactions. */
    return mlxPciFenceVerified(before, after);
}
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

/* ---- RMP context layout probe ---------------------------------------------
 *
 * A basic SRQ on firmware with ISSI set is an RMP, so everything an SRQ needs
 * rests on the rmpc layout being right. Every offset used here is recalled
 * from mlx5_ifc rather than taken from a working path in this tree, and a
 * wrong offset produces a queue that is silently never filled. That failure
 * mode already cost this project an evening on the MSI-X table, so the layout
 * is proven with a readback before anything is built on it.
 *
 * create_rmp_in: opcode @0x00, rmpc @0x100.
 * rmpc:          state @0x08 (4b), basic_cyclic_rcv_wqe @0x20 (1b), wq @0x180.
 * query_rmp_out: rmpc @0x100 as well.
 */
#define MLX_RMPC_OFF        0x100
#define MLX_RMP_WQ_OFF      (MLX_RMPC_OFF + 0x180)
/* wq_bits ends with reserved_at_200[0x400], so the struct spans 0x600 bits and
 * its PAS array starts there, not at 0x300. That lands the PAS at absolute
 * 0x880 — the same offset create_cq_in uses, which is a useful cross-check.
 * Taken from linux/mlx5/mlx5_ifc.h on the Spark, not from recollection. */
#define MLX_RMP_WQ_SIZE     0x600
#define MLX_RMP_PAS_OFF     (MLX_RMP_WQ_OFF + MLX_RMP_WQ_SIZE)
#define MLX_RMPC_STATE_RDY  1

kern_return_t
MlxPCIDriver::ProbeRmpLayout(struct mlx_probe_rmp_layout_resp *resp)
{
    if (!ivars || !resp) return kIOReturnBadArgument;
    if (!ivars->fCmd || !ivars->fUAR) return kIOReturnNotReady;
    memset(resp, 0, sizeof(*resp));
    resp->variantUsed = 0xffffffffu;

    const uint32_t logWqSz     = 4;              /* 16 WQEs */
    const uint32_t logWqStride = 2;              /* log2(64) - 4, a 64B WQE */
    const uint32_t logWqPgSz   = 0;              /* 4 KiB pages */
    const uint64_t wqBytes     = (1ull << logWqSz) * 64;

    IOBufferMemoryDescriptor *desc = NULL;
    IODMACommand *dmaCmd = NULL;
    IOAddressSegment segs[8];
    uint32_t segCount = 8;
    uint64_t pages[8] = {};
    uint32_t pageCount = 0;
    uint64_t dbDma = 0;
    uint32_t dbOffset = 0;
    bool dbHeld = false;
    uint32_t rmpn = 0;
    bool created = false;
    uint32_t exactIn = 0;
    kern_return_t kr;

    kr = mlxAllocDmaBuffer(wqBytes, 4096, kIOMemoryDirectionOutIn, &desc);
    if (kr != kIOReturnSuccess || !desc) { resp->status = kIOReturnNoMemory; goto done; }
    kr = mlxPrepareDma(ivars->fPci, desc, segs, &segCount, &dmaCmd);
    if (kr != kIOReturnSuccess || segCount == 0) { resp->status = kIOReturnNoSpace; goto done; }
    for (uint32_t i = 0; i < segCount && pageCount < 8; i++)
        if (!mlxAppendMttPages(segs[i].address, segs[i].length, pages, 8, &pageCount)) {
            resp->status = kIOReturnNoSpace; goto done;
        }
    if (!pageCount) { resp->status = kIOReturnNoSpace; goto done; }

    kr = ivars->fUAR->AllocDbSlot(&dbDma, &dbOffset);
    if (kr != kIOReturnSuccess) { resp->status = kr; goto done; }
    dbHeld = true;

    /* create_rmp_in is 0x100 header + rmpc, rmpc is 0x180 + wq, wq is 0x600
     * before its PAS array, so 272 bytes plus 8 per page. The first attempt
     * used 0x300 for the wq span and firmware rejected every variant with one
     * syndrome, which is what a length error looks like: the contents are
     * never reached. */
    exactIn = (MLX_RMPC_OFF + 0x180 + MLX_RMP_WQ_SIZE) / 8 + pageCount * 8;

    resp->setState       = MLX_RMPC_STATE_RDY;
    resp->setLogWqStride = logWqStride;
    resp->setLogWqSz     = logWqSz;
    resp->setLogWqPgSz   = logWqPgSz;
    resp->setPd          = ivars->fPd;
    resp->setUarPage     = ivars->fUAR->GetBootUarIndex();
    resp->setDbrAddr     = dbDma;
    resp->setPas0        = pages[0];

    {
        /* Hypotheses, cheapest first. Each rebuild costs a cycle, so they are
         * all tried here rather than one per cycle. */
        /* The length gate is passed: 280 bytes now fails the same way 1024 did,
         * while the short 184 fails differently. Content is the objection, and
         * wq_type and basic_cyclic are ruled out — all four gave one syndrome.
         * Two suspects remain. The QPC expresses a receive stride as
         * log2(bytes) - 4, but wq_bits may want the plain log2; and mlx5's own
         * set_wq never writes uar_page for an RMP, so writing one may itself be
         * the objection. Sweep both. */
        const uint32_t uar = ivars->fUAR->GetBootUarIndex();
        struct { uint32_t inSize, wqType, basicCyclic, stride, uarPage; }
        tries[MLX_RMP_VARIANTS] = {
            { exactIn, 0, 0, 6, 0   },  /* plain log2(64), no uar — most mlx5-faithful */
            { exactIn, 0, 0, 6, uar },  /* plain log2, uar written */
            { exactIn, 0, 0, 2, 0   },  /* QPC convention, no uar */
            { exactIn, 0, 0, 2, uar },  /* what already failed, kept as control */
            { exactIn, 0, 0, 4, 0   },  /* between the two conventions */
            { exactIn, 0, 0, 0, 0   },  /* let firmware default the stride */
        };
        for (uint32_t v = 0; v < MLX_RMP_VARIANTS && !created; v++) {
            uint8_t in[1024] = {};
            uint8_t out[256] = {};
            mlxSetBits(in, 0x00, 16, MLX_CMD_OP_CREATE_RMP);
            mlxSetBits(in, MLX_RMPC_OFF + 0x08, 4, MLX_RMPC_STATE_RDY);
            mlxSetBits(in, MLX_RMPC_OFF + 0x20, 1, tries[v].basicCyclic);
            uint32_t wq = MLX_RMP_WQ_OFF;
            mlxSetBits(in, wq + 0x000, 4,  tries[v].wqType);
            mlxSetBits(in, wq + 0x048, 24, resp->setPd);
            mlxSetBits(in, wq + 0x068, 24, tries[v].uarPage);
            mlxSetBits(in, wq + 0x080, 64, resp->setDbrAddr);
            mlxSetBits(in, wq + 0x10c, 4,  tries[v].stride);
            mlxSetBits(in, wq + 0x113, 5,  logWqPgSz);
            mlxSetBits(in, wq + 0x11b, 5,  logWqSz);
            for (uint32_t i = 0; i < pageCount; i++)
                mlxSetBits(in, MLX_RMP_PAS_OFF + i * 64, 64, pages[i]);

            kern_return_t vkr = ivars->fCmd->Exec(MLX_CMD_OP_CREATE_RMP, in,
                                                  tries[v].inSize, out, sizeof(out), 5000);
            resp->variant[v].inSize      = tries[v].inSize;
            resp->variant[v].wqType      = tries[v].wqType;
            resp->variant[v].basicCyclic = tries[v].basicCyclic;
            resp->variant[v].logWqStride = tries[v].stride;
            resp->variant[v].uarPage     = tries[v].uarPage;
            resp->variant[v].status      = (uint32_t)vkr;
            resp->variant[v].syndrome    = ivars->fCmd->LastSyndrome();
            MLX_LOG("RMP probe v%u: in=%u stride=%u uar=%u -> 0x%x syn=0x%x",
                    v, tries[v].inSize, tries[v].stride, tries[v].uarPage,
                    vkr, resp->variant[v].syndrome);
            if (vkr == kIOReturnSuccess) {
                rmpn = (uint32_t)mlxGetBits(out, 0x48, 24);
                resp->rmpn = rmpn;
                resp->setWqType = tries[v].wqType;
                resp->setLogWqStride = tries[v].stride;
                resp->setUarPage = tries[v].uarPage;
                resp->variantUsed = v;
                created = true;
            }
        }
        if (!created) {
            resp->stage = 0;
            resp->status = (kern_return_t)resp->variant[0].status;
            resp->fwStatus = resp->variant[0].syndrome;
            goto done;
        }
    }

    {
        uint8_t in[16] = {};
        uint8_t out[512] = {};
        mlxSetBits(in, 0x00, 16, MLX_CMD_OP_QUERY_RMP);
        mlxSetBits(in, 0x48, 24, rmpn);
        /* query_rmp_out carries the whole rmpc, so the buffer has to reach
         * past the PAS array or the readback reads zeros. */
        kr = ivars->fCmd->Exec(MLX_CMD_OP_QUERY_RMP, in, sizeof(in),
                               out, sizeof(out), 5000);
        if (kr != kIOReturnSuccess) {
            resp->stage = 1; resp->status = kr;
            resp->fwStatus = ivars->fCmd->LastSyndrome();
            goto destroy;
        }
        uint32_t wq = MLX_RMP_WQ_OFF;
        resp->gotState       = (uint32_t)mlxGetBits(out, MLX_RMPC_OFF + 0x08, 4);
        resp->gotWqType      = (uint32_t)mlxGetBits(out, wq + 0x000, 4);
        resp->gotPd          = (uint32_t)mlxGetBits(out, wq + 0x048, 24);
        resp->gotUarPage     = (uint32_t)mlxGetBits(out, wq + 0x068, 24);
        resp->gotDbrAddr     =           mlxGetBits(out, wq + 0x080, 64);
        resp->gotLogWqStride = (uint32_t)mlxGetBits(out, wq + 0x10c, 4);
        resp->gotLogWqPgSz   = (uint32_t)mlxGetBits(out, wq + 0x113, 5);
        resp->gotLogWqSz     = (uint32_t)mlxGetBits(out, wq + 0x11b, 5);
        resp->gotPas0        =           mlxGetBits(out, MLX_RMP_PAS_OFF, 64);

        uint32_t bad = 0;
        if (resp->gotState       != resp->setState)       bad |= 1u << 0;
        if (resp->gotWqType      != resp->setWqType)      bad |= 1u << 1;
        if (resp->gotLogWqStride != resp->setLogWqStride) bad |= 1u << 2;
        if (resp->gotLogWqSz     != resp->setLogWqSz)     bad |= 1u << 3;
        if (resp->gotLogWqPgSz   != resp->setLogWqPgSz)   bad |= 1u << 4;
        if (resp->gotPd          != resp->setPd)          bad |= 1u << 5;
        if (resp->gotUarPage     != resp->setUarPage)     bad |= 1u << 6;
        if (resp->gotDbrAddr     != resp->setDbrAddr)     bad |= 1u << 7;
        if (resp->gotPas0        != resp->setPas0)        bad |= 1u << 8;
        resp->mismatches = bad;
        resp->stage = 2;
        resp->status = kIOReturnSuccess;
        MLX_LOG("RMP probe: rmpn=%u variant=%u mismatches=0x%x",
                rmpn, resp->variantUsed, bad);
    }

destroy:
    if (created) {
        uint8_t in[16] = {};
        uint8_t out[16] = {};
        mlxSetBits(in, 0x00, 16, MLX_CMD_OP_DESTROY_RMP);
        mlxSetBits(in, 0x48, 24, rmpn);
        kern_return_t dkr = ivars->fCmd->Exec(MLX_CMD_OP_DESTROY_RMP, in, sizeof(in),
                                              out, sizeof(out), 5000);
        if (dkr != kIOReturnSuccess && resp->status == kIOReturnSuccess) {
            resp->stage = 3; resp->status = dkr;
        }
    }

done:
    if (dbHeld) ivars->fUAR->FreeDbSlot(dbOffset);
    if (dmaCmd) mlxCompleteDma(dmaCmd);
    if (desc) desc->release();
    return kIOReturnSuccess;   /* the report is the result; failures live in it */
}
