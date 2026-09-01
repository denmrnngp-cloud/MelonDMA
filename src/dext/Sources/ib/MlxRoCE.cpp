/*
 * MlxRoCE.cpp — verbs protocol layer entry point (DriverKit port).
 *
 * Ported from: kernel_src/mlnx-ofed-kernel-5.9/drivers/infiniband/hw/mlx5/main.c.
 * DriverKit port: MlxRoCE is a plain C++ object owned by MlxPCIDriver (not an
 * IOService nub — the kext published it for IORDMAFamily registration, which
 * is unavailable on macOS, notes/22-*). The DEXT exposes verbs directly
 * through MlxUserClient::ExternalMethod → MlxRoCE methods.
 *
 * MlxEventNotifier: EQ events dispatched here, then forwarded to the right
 * verbs object (CQ completion, QP error, port state).
 */
#include "MlxRoCE.hpp"
#include "MlxQP.hpp"
#include "MlxCQ.hpp"
#include "MlxMR.hpp"
#include "MlxAH.hpp"
#include "MlxGID.hpp"
#include "MlxCC.hpp"
#include "MlxPCIDriver.h"
#include "MlxFwPages.hpp"
#include "MlxHealth.hpp"
#include "MlxHCA.hpp"
#include "MlxRegs.hpp"
#include "MlxIfcHelpers.hpp"   /* mlxGetBits for EQE parsing */

#include <DriverKit/IOLib.h>
#include <string.h>

#include "MlxLog.hpp"
#define MLX_LOG(fmt, ...)  IOLog("MlxRoCE: " fmt "\n", ##__VA_ARGS__)
#define MLX_DBG(fmt, ...)  MLX_DBGLOG("MlxRoCE: " fmt, ##__VA_ARGS__)

struct MlxRoCE::State {
    MlxPCIDriver  *core;
    MlxHCA        *hca;
    MlxQP         *qp;
    MlxCQ         *cq;
    MlxMR         *mr;
    MlxAH         *ah;
    MlxGID        *gid;
    MlxCC         *cc;
    struct IOLock *resourceLock;
    struct IOLock *eventLock;
    struct mlx_async_event eventRing[16];
    uint32_t        eventHead;
    uint32_t        eventTail;

    /* Deferred runtime page requests (processed after EQ drain,
     * not inline from the EQ callback — otherwise GIVE inside the handler
     * spawns a nested Poll and reprocessing of the same EQE). */
    struct PendingPageReq {
        uint32_t functionId;
        int32_t  numPages;
    } pageReqQueue[8];
    uint32_t        pageReqHead;
    uint32_t        pageReqTail;
    bool            pageDrainActive;   /* reentrancy guard for drain */
    bool            pageReqInFlight;   /* a request is being processed (for coalescing) */
    uint32_t        pageActiveFuncId;  /* function_id of the active operation */
    int             pageActiveSign;    /* +1 GIVE / -1 TAKE */
    kern_return_t   pageLastError;     /* first handler error */
    bool            pageErrorLatched;  /* error that affects health */
};

MlxRoCE::MlxRoCE() : s(NULL) {}
MlxRoCE::~MlxRoCE() { Free(); }

kern_return_t
MlxRoCE::Init(MlxPCIDriver *core, MlxHCA *hca)
{
    if (!core || !hca) return kIOReturnBadArgument;
    s = new State;
    if (!s) return kIOReturnNoMemory;
    memset(s, 0, sizeof(*s));
    s->core = core;
    s->hca  = hca;
    s->resourceLock = IOLockAlloc();
    s->eventLock    = IOLockAlloc();
    if (!s->resourceLock || !s->eventLock) {
        if (s->resourceLock) IOLockFree(s->resourceLock);
        if (s->eventLock) IOLockFree(s->eventLock);
        delete s; s = NULL;
        return kIOReturnNoMemory;
    }

    s->qp = new MlxQP();
    s->cq = new MlxCQ();
    s->mr = new MlxMR();
    s->ah = new MlxAH();
    s->gid = new MlxGID();
    s->cc = new MlxCC();
    kern_return_t kr = s->qp->Init(this);
    if (kr != kIOReturnSuccess) { Free(); return kr; }
    kr = s->cq->Init(this);
    if (kr != kIOReturnSuccess) { Free(); return kr; }
    kr = s->mr->Init(this);
    if (kr != kIOReturnSuccess) { Free(); return kr; }
    kr = s->ah->Init(this);
    if (kr != kIOReturnSuccess) { Free(); return kr; }
    uint32_t gidTableSize = hca->Caps().roceMaxGid;
    if (gidTableSize > 256) gidTableSize = 256; /* QPC src_addr_index is 8-bit */
    if (!gidTableSize) { Free(); return kIOReturnUnsupported; }
    kr = s->gid->Init(this, core, gidTableSize);
    if (kr != kIOReturnSuccess) { Free(); return kr; }
    kr = s->cc->Init(this, core);
    if (kr != kIOReturnSuccess) { Free(); return kr; }

    MLX_LOG("initialized");
    return kIOReturnSuccess;
}

void
MlxRoCE::QuiesceVerbsResources()
{
    if (!s) return;
    if (s->cc) { s->cc->Free(); delete s->cc; s->cc = NULL; }
    /* Destroy consumers before the firmware objects they reference. */
    if (s->qp)  { s->qp->Free(); delete s->qp; s->qp = NULL; }
    if (s->ah)  { s->ah->Free(); delete s->ah; s->ah = NULL; }
    if (s->mr)  { s->mr->Free(); delete s->mr; s->mr = NULL; }
    if (s->cq)  { s->cq->Free(); delete s->cq; s->cq = NULL; }
    if (s->gid) { s->gid->Free(); delete s->gid; s->gid = NULL; }
}

void
MlxRoCE::Free()
{
    if (!s) return;
    if (s->core && s->core->GetEQ())
        s->core->GetEQ()->RemoveNotifier(this);
    QuiesceVerbsResources();
    if (s->resourceLock) { IOLockFree(s->resourceLock); s->resourceLock = NULL; }
    if (s->eventLock) { IOLockFree(s->eventLock); s->eventLock = NULL; }
    delete s; s = NULL;
}

/* ---- verbs forwarding ---- */
kern_return_t MlxRoCE::CreateQP(const struct mlx_create_qp_req *req, struct mlx_create_qp_resp *resp, MlxClientDoorbellBundle *bundle) { return s && s->qp && !PageHealthFailed() ? s->qp->CreateQP(req, resp, bundle) : kIOReturnNotReady; }
kern_return_t MlxRoCE::ModifyQP(const struct mlx_modify_qp_req *req) { return s && s->qp && !PageHealthFailed() ? s->qp->ModifyQP(req) : kIOReturnNotReady; }
kern_return_t MlxRoCE::DestroyQP(uint32_t qpn) { return s && s->qp ? s->qp->DestroyQP(qpn) : kIOReturnNotReady; }
kern_return_t MlxRoCE::CreateCQ(uint32_t entries, struct mlx_create_cq_resp *resp, MlxClientDoorbellBundle *bundle) { return s && s->cq && !PageHealthFailed() ? s->cq->CreateCQ(entries, resp, bundle) : kIOReturnNotReady; }
kern_return_t MlxRoCE::DestroyCQ(uint32_t cqHandle) { return s && s->cq ? s->cq->DestroyCQ(cqHandle) : kIOReturnNotReady; }
kern_return_t MlxRoCE::RegMR(const struct mlx_reg_mr_req *req,
                             IOMemoryDescriptor *clientMemory,
                             struct mlx_reg_mr_resp *resp) { return s && s->mr && !PageHealthFailed() ? s->mr->RegMR(req, clientMemory, resp) : kIOReturnNotReady; }
kern_return_t MlxRoCE::RegMRIndirect(const struct mlx_reg_mr_indirect_req *req,
                                     struct mlx_reg_mr_resp *resp) { return s && s->mr && !PageHealthFailed() ? s->mr->RegMRIndirect(req, resp) : kIOReturnNotReady; }
kern_return_t MlxRoCE::DeregMR(uint32_t mrHandle) { return s && s->mr ? s->mr->DeregMR(mrHandle) : kIOReturnNotReady; }
kern_return_t MlxRoCE::AllocMW(uint32_t pd, uint32_t type, uint32_t *handle, uint32_t *rkey) { return s && s->mr ? s->mr->AllocMW(pd, type, handle, rkey) : kIOReturnNotReady; }
kern_return_t MlxRoCE::DeallocMW(uint32_t handle) { return s && s->mr ? s->mr->DeallocMW(handle) : kIOReturnNotReady; }
kern_return_t MlxRoCE::BindMW(const struct mlx_bind_mw_req *req, struct mlx_bind_mw_resp *resp) {
    if (resp) { resp->rkey = 0; resp->stage = MLX_BIND_MW_STAGE_NONE; resp->status = kIOReturnNotReady; }
    if (!s || !s->mr || !s->qp) return kIOReturnNotReady;
    MlxMRContext *mr = s->mr->Lookup(req->mrHandle);
    if (!mr) { if (resp) { resp->stage = MLX_BIND_MW_STAGE_VALIDATE; resp->status = kIOReturnNotFound; } return kIOReturnNotFound; }
    uint32_t next = 0;
    uint32_t original = req->bindRkey;
    kern_return_t kr = s->mr->BindMW(req->mwHandle, req->qpn, req->mrHandle, req->bindRkey,
        req->accessFlags, req->addr, req->length, &next);
    if (kr != kIOReturnSuccess) { if (resp) { resp->stage = MLX_BIND_MW_STAGE_VALIDATE; resp->status = kr; } return kr; }
    kr = s->qp->PostBindMW(req->qpn, req->mwHandle, original, next, mr->lkey,
                           req->accessFlags, req->addr, req->length, req->wrId);
    if (kr != kIOReturnSuccess) { if (resp) { resp->stage = MLX_BIND_MW_STAGE_POST_UMR; resp->status = kr; } return kr; }
    resp->rkey = next; resp->stage = MLX_BIND_MW_STAGE_DOORBELL; resp->status = kIOReturnSuccess;
    return kIOReturnSuccess;
}
kern_return_t MlxRoCE::CreateAH(const struct mlx_create_ah_req *req, struct mlx_create_ah_resp *resp) { return s && s->ah && !PageHealthFailed() ? s->ah->CreateAH(req, resp) : kIOReturnNotReady; }
kern_return_t MlxRoCE::DestroyAH(uint32_t ahHandle) { return s && s->ah ? s->ah->DestroyAH(ahHandle) : kIOReturnNotReady; }
kern_return_t MlxRoCE::PostSendInline(const struct mlx_post_send_inline_req *req) { return s && s->qp && !PageHealthFailed() ? s->qp->PostSendInline(req) : kIOReturnNotReady; }
kern_return_t MlxRoCE::PostSendAtomic(const struct mlx_post_send_atomic_req *req) { return s && s->qp && !PageHealthFailed() ? s->qp->PostSendAtomic(req) : kIOReturnNotReady; }
kern_return_t MlxRoCE::QueryGidTable(const struct mlx_query_gid_table_req *req, struct mlx_query_gid_table_resp *resp) { return s && s->gid ? s->gid->QueryAll(req, resp) : kIOReturnNotReady; }
kern_return_t MlxRoCE::ArmCQ(const struct mlx_arm_cq_req *req) { return s && s->cq && req ? s->cq->ArmCQ(req->cqHandle, req->solicitedOnly) : kIOReturnNotReady; }

kern_return_t
MlxRoCE::QueryDevice(struct mlx_query_device_resp *resp)
{
    if (!s || !resp) return kIOReturnBadArgument;
    memset(resp, 0, sizeof(*resp));
    if (s->hca) {
        const MlxHcaCaps &c = s->hca->Caps();
        resp->fwVersion = c.fwRev;
        resp->deviceId = s->hca->Vendor().deviceId;
        resp->numPorts = c.numPorts;
        resp->maxQp = c.maxQp;
        resp->maxCq = c.maxCq;
        resp->maxMr = c.maxMr;
        resp->roceVersions = c.roceVersions;
        resp->maxGid = c.roceMaxGid;
        resp->maxMsgSize = c.logMaxMsg && c.logMaxMsg < 32
                         ? (1u << c.logMaxMsg) : 0;
        resp->maxInlineData = MLX_UC_MAX_INLINE_DATA;
        /* Honest requester/responder atomic depths. The mlx5 general caps
         * log_max_ra_req_qp/log_max_ra_res_qp would raise these once their
         * bit offsets are verified on hardware — until then the QP encoder
         * already bounds them (<=128, power of two). */
        resp->maxQpRdAtomic = 4;
        resp->maxQpInitRdAtomic = 4;
    }
    return kIOReturnSuccess;
}

kern_return_t
MlxRoCE::QueryPort(struct mlx_query_port_resp *resp)
{
    if (!s || !resp) return kIOReturnBadArgument;
    memset(resp, 0, sizeof(*resp));
    resp->portNum = 1;
    resp->linkLayer = MLX_LINK_LAYER_ETHERNET;
    resp->gidType = (s->hca && (s->hca->Caps().roceVersions & 0x2)) ? 2 : 0;
    resp->gidTblLen = s->gid ? (uint16_t)s->gid->TableSize() : 0;
    resp->pkeyTblLen = 1;

    /* QUERY_VPORT_STATE (0x750): real port state (0=DOWN 1=UP).
     * out: max_tx_speed@0x60, admin_state@0x78 (4b), state@0x7c (4b). */
    uint8_t in[16] = {};
    uint8_t out[16] = {};
    mlxSetBits(in, 0x00, 16, MLX_CMD_OP_QUERY_VPORT_STATE);
    mlxSetBits(in, 0x40, 1, 0);            /* other_vport = 0 */
    mlxSetBits(in, 0x50, 16, 0);           /* vport_number = 0 */
    kern_return_t kr = s->core->Exec(MLX_CMD_OP_QUERY_VPORT_STATE, in, sizeof(in),
                                     out, sizeof(out), 5000);
    if (kr != kIOReturnSuccess) {
        MLX_LOG("QUERY_VPORT_STATE failed: 0x%x", kr);
        return kr;
    }
    uint8_t state = (uint8_t)mlxGetBits(out, 0x7c, 4);
    uint8_t admin = (uint8_t)mlxGetBits(out, 0x78, 4);
    uint32_t speed = (uint32_t)mlxGetBits(out, 0x60, 16);
    resp->portState = (state == 1) ? 1 : 0;
    resp->activeSpeed = speed * 1000;      /* Gb/s → Mbps (MVP approximation) */
    resp->maxMtu = 5;                      /* 4 KiB (RoCEv2 max MTU) */
    MLX_LOG("QUERY_VPORT_STATE: state=%u (UP=%u) admin=%u maxTxSpeed=%u",
            state, resp->portState, admin, speed);
    return kIOReturnSuccess;
}

/* ---- async event ring ---- */
kern_return_t
MlxRoCE::GetAsyncEvent(struct mlx_async_event *event)
{
    if (!s || !event) return kIOReturnBadArgument;
    IOLockLock(s->eventLock);
    if (s->eventHead == s->eventTail) { IOLockUnlock(s->eventLock); return kIOReturnNoResources; }
    *event = s->eventRing[s->eventTail];
    s->eventTail = (s->eventTail + 1) % 16;
    IOLockUnlock(s->eventLock);
    return kIOReturnSuccess;
}

void
MlxRoCE::QueueAsyncEvent(uint32_t eventType, uint32_t elementType, uint32_t elementHandle)
{
    IOLockLock(s->eventLock);
    uint32_t next = (s->eventHead + 1) % 16;
    if (next != s->eventTail) {
        s->eventRing[s->eventHead].eventType = eventType;
        s->eventRing[s->eventHead].elementType = elementType;
        s->eventRing[s->eventHead].elementHandle = elementHandle;
        s->eventHead = next;
    }
    IOLockUnlock(s->eventLock);
}

kern_return_t
MlxRoCE::DrainPendingPageRequests()
{
    if (!s || !s->core->GetPages()) return kIOReturnNotReady;
    IOLockLock(s->eventLock);
    if (s->pageDrainActive) {
        IOLockUnlock(s->eventLock);
        return kIOReturnSuccess;   /* nested drain */
    }
    s->pageDrainActive = true;
    s->pageLastError = 0;
    IOLockUnlock(s->eventLock);
    while (true) {
        IOLockLock(s->eventLock);
        if (s->pageReqTail == s->pageReqHead) {
            s->pageDrainActive = false;
            kern_return_t result = s->pageLastError;
            if (result == kIOReturnSuccess && s->pageErrorLatched)
                result = kIOReturnIOError;
            IOLockUnlock(s->eventLock);
            return result;
        }
        uint32_t funcId   = s->pageReqQueue[s->pageReqTail].functionId;
        int32_t  numPages = s->pageReqQueue[s->pageReqTail].numPages;
        s->pageReqTail = (s->pageReqTail + 1) % 8;

        /* Mark the active operation (for coalescing when an EQE arrives). */
        s->pageReqInFlight = true;
        s->pageActiveFuncId = funcId;
        s->pageActiveSign = (numPages >= 0) ? 1 : -1;
        IOLockUnlock(s->eventLock);
        kern_return_t kr = s->core->GetPages()->HandleRuntimePageRequest(funcId, numPages);
        IOLockLock(s->eventLock);
        s->pageReqInFlight = false;
        s->pageActiveFuncId = 0;
        s->pageActiveSign = 0;
        if (kr != kIOReturnSuccess && !s->pageLastError) {
            s->pageLastError = kr;
            s->pageErrorLatched = true;
            MLX_LOG("PAGE_REQUEST: handler error func=%u n=%d -> 0x%x — latched",
                    funcId, numPages, kr);
        }
        IOLockUnlock(s->eventLock);
    }
}

bool
MlxRoCE::PageHealthFailed() const
{
    if (!s) return true;
    IOLockLock(s->eventLock);
    bool failed = s->pageErrorLatched;
    IOLockUnlock(s->eventLock);
    MlxFwPages *pages = s->core ? s->core->GetPages() : NULL;
    return failed || (pages && (pages->GetAmbiguousOwned() != 0 ||
                                !pages->ValidateAccounting()));
}

/* ---- EQ event dispatch ---- */
void
MlxRoCE::HandleEvent(uint32_t type, void *eqe)
{
    /* mlx5_ifc_eqe_bits: event_type@byte1, event_sub_type@byte3,
     * event_data@bit 0x100 (byte 32), owner@byte63 bit0. Event payload:
     *   completion (comp_event):  cq_number@0x1c8 (24b)
     *   page request (pages_req): function_id@0x110 (16b), num_pages@0x120 (32b)
     *   port state change:        port_num@0x140 (4b) */
    const uint8_t *e = (const uint8_t *)eqe;
    if (!e) return;
    switch (type) {
    case MLX_EVENT_TYPE_COMPLETION: {
        uint32_t cqn = (uint32_t)mlxGetBits(e, 0x1c8, 24);
        if (s->cq) s->cq->HandleCompletion(cqn);
        break;
    }
    case MLX_EVENT_TYPE_PAGE_REQUEST: {
        uint32_t funcId   = (uint32_t)mlxGetBits(e, 0x110, 16);
        int32_t  numPages = (int32_t)mlxGetBits(e, 0x120, 32);
        int sign = (numPages >= 0) ? 1 : -1;

        if (!numPages) {
            IOLockLock(s->eventLock);
            s->pageErrorLatched = true;
            IOLockUnlock(s->eventLock);
            MLX_LOG("PAGE_REQUEST: malformed zero request func=%u — health latched", funcId);
            break;
        }

        /* Coalescing at the EQE level: if a request for the same function_id
         * and the same sign is currently being processed — it's a remainder
         * update (3332→2820→...), it must not be enqueued in the FIFO or
         * reallocated. */
        IOLockLock(s->eventLock);
        if (s->pageReqInFlight && funcId == s->pageActiveFuncId && sign == s->pageActiveSign) {
            IOLockUnlock(s->eventLock);
            MLX_LOG("PAGE_REQUEST: func=%u outstanding=%d coalesced-active", funcId, numPages);
            break;
        }

        /* Otherwise — a new request: enqueue it (not inline from the EQ callback). */
        uint32_t next = (s->pageReqHead + 1) % 8;
        if (next != s->pageReqTail) {
            s->pageReqQueue[s->pageReqHead].functionId = funcId;
            s->pageReqQueue[s->pageReqHead].numPages   = numPages;
            s->pageReqHead = next;
            MLX_LOG("PAGE_REQUEST: func=%u num_pages=%d queued", funcId, numPages);
        } else {
            MLX_LOG("PAGE_REQUEST: queue overflow — dropped func=%u n=%d",
                    funcId, numPages);
            s->pageErrorLatched = true;
        }
        IOLockUnlock(s->eventLock);
        break;
    }
    case MLX_EVENT_TYPE_PORT_STATE_CHANGE: {
        uint32_t port = (uint32_t)mlxGetBits(e, 0x140, 4);
        QueueAsyncEvent(MLX_EVENT_PORT_ACTIVE, MLX_ASYNC_ELEMENT_PORT,
                        port ? port : 1);
        break;
    }
    case MLX_EVENT_TYPE_DEVICE_FATAL:
        /* Fail-closed: fence DMA before publishing the event, so a client
         * polling async events can never race a still-live datapath. */
        if (s->core && s->core->GetHealth())
            s->core->GetHealth()->MarkFatal();
        QueueAsyncEvent(MLX_EVENT_DEVICE_FATAL, MLX_ASYNC_ELEMENT_DEVICE, 0);
        break;
    case MLX_EVENT_TYPE_WQ_CATAS_ERROR:
        /* Conservative fail-closed: a catastrophic WQ error means firmware's
         * DMA is no longer trustworthy. Fence the whole device rather than
         * guess which QP to flush (per-QP ERR transition is a follow-up once
         * the EQE qpn layout is verified on hardware). */
        if (s->core && s->core->GetHealth())
            s->core->GetHealth()->MarkFatal();
        QueueAsyncEvent(MLX_EVENT_WQ_FATAL, MLX_ASYNC_ELEMENT_DEVICE, 0);
        break;
    default:
        break;
    }
}

/* accessors */
MlxPCIDriver *MlxRoCE::GetCore() { return s ? s->core : NULL; }
MlxHCA *      MlxRoCE::GetHCA() { return s ? s->hca : NULL; }
MlxGID *      MlxRoCE::GetGID() { return s ? s->gid : NULL; }
MlxCC *       MlxRoCE::GetCC()  { return s ? s->cc  : NULL; }
MlxCQ *       MlxRoCE::GetCQ()  { return s ? s->cq  : NULL; }
MlxQP *       MlxRoCE::GetQP()  { return s ? s->qp  : NULL; }
MlxMR *       MlxRoCE::GetMR()  { return s ? s->mr  : NULL; }

bool
MlxRoCE::StageCaps()
{
    /* QUERY_HCA_CAP (general/roce/ethernet/flow) wired in Phase 1. */
    return s && s->hca;
}

bool
MlxRoCE::StageGID()
{
    /* SET_ROCE_ADDRESS with the local GID (::ffff:192.168.200.1) wired in Phase 1. */
    return s && s->gid;
}
