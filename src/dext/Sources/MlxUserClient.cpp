/*
 * MlxUserClient.cpp — DriverKit user client (app ↔ DEXT boundary).
 *
 * Selector table and POD ABI structs live in MlxUCIO.h (shared with userspace).
 * The kext used IOExternalMethodDispatch + OSArray/OSData ownership tracking;
 * the DEXT uses IOUserClientMethodDispatch + the same ownership logic via
 * OSArray/OSData (both available in DriverKit).
 *
 * Doorbell model: Option B (kernel-mediated posting, REMEDIATION_PLAN §7.1).
 * SQ/RQ/CQ/UAR and DB records are deliberately not mapped into the client;
 * posting and CQ polling use bounded kernel-mediated methods.
 *
 * iig dispatch: Start/Stop/CopyClientMemoryForType/AsyncCompletion are LOCAL
 * → defined as *_Impl; init/free/ExternalMethod are virtual overrides → direct.
 */
#include "MlxDriverKitCompat.h"
#include "../ib/MlxRoCE.hpp"
#include "../ib/MlxCQ.hpp"
#include "../ib/MlxMR.hpp"
#include "../ib/MlxQP.hpp"
#include "../ib/MlxAH.hpp"
#include "../ib/MlxGID.hpp"
#include "../ib/MlxCC.hpp"
#include "core/MlxCmd.hpp"
#include "core/MlxFwPages.hpp"
#include "core/MlxHealth.hpp"
#include "core/MlxUAR.hpp"
#include "hw/MlxHCA.hpp"
#include "hw/MlxIfcHelpers.hpp"
#include "MlxUCIO.h"
#include "MlxPCIDriver.h"

#include <DriverKit/IOLib.h>
#include <DriverKit/IOService.h>
#include <DriverKit/OSArray.h>
#include <DriverKit/OSData.h>
#include <DriverKit/IOMemoryDescriptor.h>
#include <string.h>
#include <time.h>

#define MLX_UC_LOG(fmt, ...)  IOLog("MlxUserClient: " fmt "\n", ##__VA_ARGS__)

/* Opaque client-token registry (P0.3). Raw firmware IDs are resolved only at
 * this ABI boundary; MlxQP/MlxCQ/MlxMR keep working with raw IDs. A token is
 * (type << 28) | (generation << 9) | slot. Generation comes from ONE global
 * counter shared by every UserClient in this DEXT process, so two clients can
 * never mint the same token (cross-client isolation) and a stale token never
 * aliases a recycled raw ID within a 19-bit generation epoch (524288 mints). */
enum { MLX_T_PD = 0, MLX_T_QP, MLX_T_CQ, MLX_T_MR, MLX_T_MW, MLX_T_COUNT };
#define MLX_T_SLOT_BITS 9u
#define MLX_T_SLOTS     (1u << MLX_T_SLOT_BITS)          /* 512 */
#define MLX_T_GEN_BITS  19u
#define MLX_T_GEN_MASK  ((1u << MLX_T_GEN_BITS) - 1u)    /* 0x7ffff */
#define MLX_T_GEN_SHIFT MLX_T_SLOT_BITS

/* Global generation counter: shared by all UserClients of one DEXT
 * process (DriverKit keeps all clients in one process). 19 bits =>
 * 524288 tokens before wrap — (type, slot, gen) collisions between clients
 * are impossible until the generation wraps (unreachable in a live session). */
static uint32_t sTokenGen = 0;

struct MlxTokenSlot { uint32_t raw; uint32_t generation; uint8_t used; };

struct MlxUserClient_IVars {
    MlxRoCE        *fRoce;
    MlxPCIDriver   *fCore;
    OSArray        *fOwnedPd;
    OSArray        *fOwnedQp;
    OSArray        *fOwnedCq;
    OSArray        *fOwnedMr;
    OSArray        *fOwnedMw;
    OSArray        *fOwnedAh;
    OSArray        *fOwnedGid;
    struct MlxTokenSlot fToken[MLX_T_COUNT][MLX_T_SLOTS];
    IOLock         *fOwnedLock;
    /* Serializes ExternalMethod, CopyClientMemoryForType and Cleanup so token
     * resolution, ownership checks and teardown can never interleave. */
    IOLock         *fMethodLock;
    MlxClientDoorbellBundle *fFastBundle;
    /* P1.1: data-path methods are excluded from fMethodLock. fDataInflight
     * counts in-flight data-path calls; fDataTeardown blocks new ones once
     * Cleanup starts. Both are guarded by fOwnedLock. */
    uint32_t        fDataInflight;
    bool            fDataTeardown;
    /* P1.1 per-client quota counters (see MLX_UC_MAX_*_PER_CLIENT). */
    uint32_t        fQuotaPd;
    uint32_t        fQuotaQp;
    uint32_t        fQuotaCq;
    uint32_t        fQuotaMr;
    uint32_t        fQuotaMw;
    uint32_t        fQuotaMkeys;
    uint32_t        fQuotaAh;
    uint32_t        fQuotaGid;
    uint32_t        fQuotaDbRecords;
    uint64_t        fFwCmdWindowStart;
    uint32_t        fFwCmdBurstUsed;
};

#define MlxUserClient_DECLARE_IVARS  struct MlxUserClient_IVars * ivars;

/* RAII: releases fMethodLock on every return path out of a serialized method
 * (ExternalMethod / CopyClientMemoryForType / Cleanup hold this per client). */
struct MlxMethodLockGuard {
    IOLock *lock;
    MlxMethodLockGuard(IOLock *l) : lock(l) { if (lock) IOLockLock(lock); }
    ~MlxMethodLockGuard() { if (lock) IOLockUnlock(lock); }
};

/* P1.1: data-path selectors (post/poll/sync/arm) skip fMethodLock and are
 * refcounted under fOwnedLock instead. They only read the token/ownership
 * tables (already under fOwnedLock) and take per-QP/CQ locks below, so they
 * need no cross-method serialization. Cleanup sets fDataTeardown and drains
 * fDataInflight to zero before freeing those tables. */
static bool
MlxIsDataPathSelector(uint64_t selector)
{
    switch (selector) {
    case kMlxUCMethodPollCQ:
    case kMlxUCMethodPostSend:
    case kMlxUCMethodPostRecv:
    case kMlxUCMethodPostSendBatch:
    case kMlxUCMethodPostRecvBatch:
    case kMlxUCMethodSyncFastPath:
    case kMlxUCMethodSyncRecvFastPath:
    case kMlxUCMethodPostSendSge:
    case kMlxUCMethodPostRecvSge:
    case kMlxUCMethodSyncSendSge:
    case kMlxUCMethodSyncRecvSge:
    case kMlxUCMethodPostLocalInv:
    case kMlxUCMethodPostSendInline:
    case kMlxUCMethodPostSendAtomic:
    case kMlxUCMethodPostUmrKlm:
    case kMlxUCMethodQueryCqCompletions:
    case kMlxUCMethodArmCQ:
    case kMlxUCMethodGetAsyncEvent:
    case kMlxUCMethodUpdateCqConsumer:
        return true;
    default:
        return false;
    }
}

struct MlxDataPathGuard {
    MlxUserClient_IVars *ivars;
    bool entered;
    MlxDataPathGuard(MlxUserClient_IVars *v) : ivars(v), entered(false) {
        if (!v || !v->fOwnedLock) return;
        IOLockLock(v->fOwnedLock);
        if (!v->fDataTeardown) { v->fDataInflight++; entered = true; }
        IOLockUnlock(v->fOwnedLock);
    }
    ~MlxDataPathGuard() {
        if (!entered || !ivars || !ivars->fOwnedLock) return;
        IOLockLock(ivars->fOwnedLock);
        ivars->fDataInflight--;
        IOLockUnlock(ivars->fOwnedLock);
    }
    bool ok() const { return entered; }
};

static uint32_t TokenMake(uint32_t type, uint32_t slot, uint32_t gen)
{
    return (type << 28) |
           ((gen & MLX_T_GEN_MASK) << MLX_T_GEN_SHIFT) |
           (slot & (MLX_T_SLOTS - 1u));
}

static uint32_t TokenCreate(MlxUserClient_IVars *v, uint32_t type, uint32_t raw)
{
    if (!v || !v->fOwnedLock || type >= MLX_T_COUNT || !raw) return 0;
    uint32_t gen = __atomic_fetch_add(&sTokenGen, 1u, __ATOMIC_RELAXED) &
                   MLX_T_GEN_MASK;
    if (!gen)
        gen = __atomic_fetch_add(&sTokenGen, 1u, __ATOMIC_RELAXED) &
              MLX_T_GEN_MASK;
    IOLockLock(v->fOwnedLock);
    for (uint32_t i = 0; i < MLX_T_SLOTS; i++) {
        struct MlxTokenSlot *s = &v->fToken[type][i];
        if (s->used) continue;
        s->generation = gen;
        s->raw = raw;
        s->used = 1;
        uint32_t t = TokenMake(type, i, gen);
        IOLockUnlock(v->fOwnedLock);
        return t;
    }
    IOLockUnlock(v->fOwnedLock);
    return 0;
}

/* Resolve/forRaw take fOwnedLock so a concurrent TokenDrop / slot reuse cannot
 * flip a live token into a different raw ID mid-read. */
static uint32_t TokenResolve(MlxUserClient_IVars *v, uint32_t token, uint32_t type)
{
    if (!v || !v->fOwnedLock || type >= MLX_T_COUNT ||
        (token >> 28) != type) return 0;
    uint32_t slot = token & (MLX_T_SLOTS - 1u);
    uint32_t gen = (token >> MLX_T_GEN_SHIFT) & MLX_T_GEN_MASK;
    IOLockLock(v->fOwnedLock);
    struct MlxTokenSlot *s = &v->fToken[type][slot];
    uint32_t raw = (s->used && s->generation == gen) ? s->raw : 0;
    IOLockUnlock(v->fOwnedLock);
    return raw;
}

static bool TokenDrop(MlxUserClient_IVars *v, uint32_t token, uint32_t type,
                      uint32_t *raw)
{
    if (!v || !v->fOwnedLock || type >= MLX_T_COUNT ||
        (token >> 28) != type) return false;
    uint32_t slot = token & (MLX_T_SLOTS - 1u);
    uint32_t gen = (token >> MLX_T_GEN_SHIFT) & MLX_T_GEN_MASK;
    if (slot >= MLX_T_SLOTS) return false;
    IOLockLock(v->fOwnedLock);
    struct MlxTokenSlot *s = &v->fToken[type][slot];
    bool ok = s->used && s->generation == gen;
    if (ok) {
        if (raw) *raw = s->raw;
        s->used = 0;
        s->raw = 0;
    }
    IOLockUnlock(v->fOwnedLock);
    return ok;
}

static uint32_t TokenForRaw(MlxUserClient_IVars *v, uint32_t type, uint32_t raw)
{
    if (!v || !v->fOwnedLock || type >= MLX_T_COUNT || !raw) return 0;
    IOLockLock(v->fOwnedLock);
    for (uint32_t i = 0; i < MLX_T_SLOTS; i++)
        if (v->fToken[type][i].used && v->fToken[type][i].raw == raw) {
            uint32_t t = TokenMake(type, i, v->fToken[type][i].generation);
            IOLockUnlock(v->fOwnedLock);
            return t;
        }
    IOLockUnlock(v->fOwnedLock);
    return 0;
}

/* ---- P1.1 per-client quotas (DoS protection) ----
 * Reserving BEFORE the firmware command is the guarantee that a refused
 * request never leaves a partially-created resource. Counters are protected
 * by the same fOwnedLock that serializes ownership-table mutation. */
static bool QuotaReserve(MlxUserClient_IVars *v, uint32_t *counter, uint32_t limit)
{
    if (!v || !counter || !v->fOwnedLock) return false;
    IOLockLock(v->fOwnedLock);
    bool ok = *counter < limit;
    if (ok) (*counter)++;
    IOLockUnlock(v->fOwnedLock);
    return ok;
}

static void QuotaRelease(MlxUserClient_IVars *v, uint32_t *counter)
{
    if (!v || !counter || !v->fOwnedLock) return;
    IOLockLock(v->fOwnedLock);
    if (*counter) (*counter)--;
    IOLockUnlock(v->fOwnedLock);
}

static uint32_t CapLimit(uint32_t policy, uint32_t firmware)
{
    return firmware && firmware < policy ? firmware : policy;
}

static const MlxHcaCaps *ClientCaps(MlxUserClient_IVars *v)
{
    return v && v->fCore && v->fCore->GetHCA()
        ? &v->fCore->GetHCA()->Caps() : NULL;
}

static uint32_t DbRecordLimit(MlxUserClient_IVars *v)
{
    return v && v->fCore && v->fCore->GetUAR()
        ? v->fCore->GetUAR()->GetDbSlotCapacity() : 0;
}

static uint32_t ClientQpLimit(MlxUserClient_IVars *v)
{
    const MlxHcaCaps *caps = ClientCaps(v);
    uint32_t limit = CapLimit(MLX_UC_MAX_QP_PER_CLIENT,
                              caps ? caps->maxQp : 0);
    uint32_t db = DbRecordLimit(v);
    return db && db < limit ? db : limit;
}

static uint32_t ClientCqLimit(MlxUserClient_IVars *v)
{
    const MlxHcaCaps *caps = ClientCaps(v);
    uint32_t limit = CapLimit(MLX_UC_MAX_CQ_PER_CLIENT, caps ? caps->maxCq : 0);
    uint32_t db = DbRecordLimit(v);
    return db && db < limit ? db : limit;
}

static uint32_t ClientMrLimit(MlxUserClient_IVars *v)
{
    const MlxHcaCaps *caps = ClientCaps(v);
    return CapLimit(MLX_UC_MAX_MR_PER_CLIENT, caps ? caps->maxMr : 0);
}

static uint32_t ClientMwLimit(MlxUserClient_IVars *v)
{
    const MlxHcaCaps *caps = ClientCaps(v);
    return CapLimit(MLX_UC_MAX_MW_PER_CLIENT, caps ? caps->maxMr : 0);
}

/* Token-bucket firmware-command gate for the raw passthrough selectors. */
static bool QuotaReserveDb(MlxUserClient_IVars *v)
{
    uint32_t limit = DbRecordLimit(v);
    return limit && QuotaReserve(v, &v->fQuotaDbRecords, limit);
}

static void QuotaReleaseDb(MlxUserClient_IVars *v)
{
    QuotaRelease(v, &v->fQuotaDbRecords);
}

static bool QuotaReserveMkey(MlxUserClient_IVars *v)
{
    return QuotaReserve(v, &v->fQuotaMkeys, ClientMrLimit(v));
}

static void QuotaReleaseMkey(MlxUserClient_IVars *v)
{
    QuotaRelease(v, &v->fQuotaMkeys);
}

static bool FwCmdAllowed(MlxUserClient_IVars *v)
{
    if (!v) return false;
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (now - v->fFwCmdWindowStart >= MLX_UC_FW_CMD_WINDOW_NS) {
        v->fFwCmdWindowStart = now;
        v->fFwCmdBurstUsed = 0;
    }
    if (v->fFwCmdBurstUsed >= MLX_UC_FW_CMD_BURST) return false;
    v->fFwCmdBurstUsed++;
    return true;
}

#include "MlxUserClient.h"

bool
MlxUserClient::init()
{
    if (!super::init())
        return false;
    ivars = IONewZero(MlxUserClient_IVars, 1);
    return ivars != NULL;
}

void
MlxUserClient::free()
{
    Cleanup();
    if (ivars)
        IODelete(ivars, MlxUserClient_IVars, 1);
    super::free();
}

kern_return_t
MlxUserClient::Start_Impl(IOService * provider)
{
    kern_return_t kr = Start(provider, SUPERDISPATCH);
    if (kr != kIOReturnSuccess)
        return kr;

    /* The provider is MlxPCIDriver; the verbs object lives on the core. */
    MlxPCIDriver *core = OSDynamicCast(MlxPCIDriver, provider);
    if (!core) {
        MLX_UC_LOG("provider is not MlxPCIDriver");
        (void)Stop(provider, SUPERDISPATCH);
        return kIOReturnNoDevice;
    }
    ivars->fCore = core;
    /* RoCE may not be up yet (FwInit stopped at boot pages) —
    * UserClient still starts: kMlxUCMethodFwReinit works without RoCE
    * and is exactly what brings it up (notes/31). Verbs methods are gated on
    * fRoce in ExternalMethod. */
    ivars->fRoce = core->GetRoCE();

    ivars->fOwnedPd = OSArray::withCapacity(8);
    ivars->fOwnedQp = OSArray::withCapacity(8);
    ivars->fOwnedCq = OSArray::withCapacity(8);
    ivars->fOwnedMr = OSArray::withCapacity(8);
    ivars->fOwnedMw = OSArray::withCapacity(8);
    ivars->fOwnedAh = OSArray::withCapacity(8);
    ivars->fOwnedGid = OSArray::withCapacity(4);
    ivars->fOwnedLock = IOLockAlloc();
    ivars->fMethodLock = IOLockAlloc();
    if (!ivars->fOwnedPd || !ivars->fOwnedQp || !ivars->fOwnedCq || !ivars->fOwnedMr ||
        !ivars->fOwnedMw || !ivars->fOwnedAh || !ivars->fOwnedGid ||
        !ivars->fOwnedLock || !ivars->fMethodLock) {
        Cleanup();
        (void)Stop(provider, SUPERDISPATCH);
        return kIOReturnNoMemory;
    }
    return kIOReturnSuccess;
}

kern_return_t
MlxUserClient::Stop_Impl(IOService * provider)
{
    Cleanup();
    return Stop(provider, SUPERDISPATCH);
}

void
MlxUserClient::Cleanup()
{
    if (!ivars) return;
    /* Serialize teardown against in-flight ExternalMethod/CopyClientMemory so
     * a concurrent resolve/post cannot observe half-freed ownership tables. */
    IOLock *methodLock = ivars->fMethodLock;
    if (methodLock) IOLockLock(methodLock);

    /* P1.1: block new data-path methods and drain in-flight ones before
     * freeing the ownership tables they read. */
    if (ivars->fOwnedLock) {
        IOLockLock(ivars->fOwnedLock);
        ivars->fDataTeardown = true;
        while (ivars->fDataInflight > 0) {
            IOLockUnlock(ivars->fOwnedLock);
            IOSleep(1);
            IOLockLock(ivars->fOwnedLock);
        }
        IOLockUnlock(ivars->fOwnedLock);
    }

    ReleaseOwnedResources();
    if (ivars->fFastBundle && ivars->fCore && ivars->fCore->GetUAR()) {
        ivars->fCore->GetUAR()->FreeClientBundle(ivars->fFastBundle);
        IODelete(ivars->fFastBundle, MlxClientDoorbellBundle, 1);
        ivars->fFastBundle = NULL;
    }
    if (ivars->fOwnedPd && ivars->fCore && !ivars->fCore->DmaQuarantined()) {
        for (uint32_t i = 0; i < ivars->fOwnedPd->getCount(); i++) {
            OSData *record = OSDynamicCast(OSData, ivars->fOwnedPd->getObject(i));
            if (!record || record->getLength() != sizeof(uint32_t)) continue;
            uint32_t pd = *(const uint32_t *)record->getBytesNoCopy();
            uint8_t in[16] = {}, out[16] = {};
            mlxSetBits(in, 0x00, 16, MLX_CMD_OP_DEALLOC_PD);
            mlxSetBits(in, 0x48, 24, pd);
            (void)ivars->fCore->Exec(MLX_CMD_OP_DEALLOC_PD, in, sizeof(in), out, sizeof(out), 5000);
        }
    }
    if (ivars->fOwnedPd)  { ivars->fOwnedPd->release();  ivars->fOwnedPd = NULL; }
    if (ivars->fOwnedQp)  { ivars->fOwnedQp->release();  ivars->fOwnedQp = NULL; }
    if (ivars->fOwnedCq)  { ivars->fOwnedCq->release();  ivars->fOwnedCq = NULL; }
    if (ivars->fOwnedMr)  { ivars->fOwnedMr->release();  ivars->fOwnedMr = NULL; }
    if (ivars->fOwnedMw)  { ivars->fOwnedMw->release();  ivars->fOwnedMw = NULL; }
    if (ivars->fOwnedAh)  { ivars->fOwnedAh->release();  ivars->fOwnedAh = NULL; }
    if (ivars->fOwnedGid) { ivars->fOwnedGid->release(); ivars->fOwnedGid = NULL; }
    if (ivars->fOwnedLock){ IOLockFree(ivars->fOwnedLock); ivars->fOwnedLock = NULL; }
    ivars->fMethodLock = NULL;
    if (methodLock) IOLockUnlock(methodLock);
    ivars->fRoce = NULL;
    ivars->fCore = NULL;
    if (methodLock) IOLockFree(methodLock);
}

/* ---- per-client resource ownership (ported from kext MlxUserClient.cpp) ---- */

bool
MlxUserClient::AddOwned(OSArray *table, uint32_t handle)
{
    if (!table || !ivars->fOwnedLock) return false;
    OSData *record = OSData::withBytes(&handle, sizeof(handle));
    if (!record) return false;
    IOLockLock(ivars->fOwnedLock);
    bool added = table->setObject(record);
    IOLockUnlock(ivars->fOwnedLock);
    record->release();
    return added;
}

bool
MlxUserClient::RemoveOwned(OSArray *table, uint32_t handle)
{
    if (!table || !ivars->fOwnedLock) return false;
    bool found = false;
    IOLockLock(ivars->fOwnedLock);
    for (uint32_t i = 0; i < table->getCount(); i++) {
        OSData *record = OSDynamicCast(OSData, table->getObject(i));
        if (!record || record->getLength() != sizeof(handle)) continue;
        if (*(const uint32_t *)record->getBytesNoCopy() != handle) continue;
        table->removeObject(i);
        found = true;
        break;
    }
    IOLockUnlock(ivars->fOwnedLock);
    return found;
}

bool
MlxUserClient::Owns(OSArray *table, uint32_t handle)
{
    if (!table || !ivars->fOwnedLock) return false;
    bool found = false;
    IOLockLock(ivars->fOwnedLock);
    for (uint32_t i = 0; i < table->getCount(); i++) {
        OSData *record = OSDynamicCast(OSData, table->getObject(i));
        if (record && record->getLength() == sizeof(handle) &&
            *(const uint32_t *)record->getBytesNoCopy() == handle) {
            found = true;
            break;
        }
    }
    IOLockUnlock(ivars->fOwnedLock);
    return found;
}

bool
MlxUserClient::TakeOwned(OSArray *table, uint32_t *handle)
{
    if (!table || !handle || !ivars->fOwnedLock) return false;
    bool found = false;
    IOLockLock(ivars->fOwnedLock);
    if (table->getCount()) {
        OSData *record = OSDynamicCast(OSData, table->getObject(0));
        if (record && record->getLength() == sizeof(*handle)) {
            *handle = *(const uint32_t *)record->getBytesNoCopy();
            found = true;
        }
        table->removeObject(0);
    }
    IOLockUnlock(ivars->fOwnedLock);
    return found;
}

void
MlxUserClient::ReleaseOwnedResources()
{
    if (!ivars->fRoce) return;
    uint32_t handle;
    while (TakeOwned(ivars->fOwnedQp, &handle)) {
        kern_return_t kr = ivars->fRoce->DestroyQP(handle);
        /* DestroyQP refuses a QP with in-flight SQ/RQ WQEs. Force it to RESET
         * (flushes both queues) and retry so teardown cannot leak QP/DMA. */
        if (kr == kIOReturnBusy && ivars->fRoce->GetQP()) {
            (void)ivars->fRoce->GetQP()->ResetQP(handle);
            kr = ivars->fRoce->DestroyQP(handle);
        }
        if (kr != kIOReturnSuccess)
            MLX_UC_LOG("QP[%u] cleanup failed: 0x%x", handle, kr);
    }
    while (TakeOwned(ivars->fOwnedMw,  &handle)) ivars->fRoce->DeallocMW(handle);
    while (TakeOwned(ivars->fOwnedMr,  &handle)) ivars->fRoce->DeregMR(handle);
    while (TakeOwned(ivars->fOwnedAh,  &handle)) ivars->fRoce->DestroyAH(handle);
    while (TakeOwned(ivars->fOwnedCq,  &handle)) ivars->fRoce->DestroyCQ(handle);
    while (TakeOwned(ivars->fOwnedGid, &handle)) {
        if (ivars->fRoce->GetGID()) {
            kern_return_t kr = ivars->fRoce->GetGID()->DelGID(handle);
            if (kr != kIOReturnSuccess)
                MLX_UC_LOG("GID[%u] cleanup failed: 0x%x", handle, kr);
        }
    }
}

/* ---- external method dispatch ---- */

/* DriverKit added checkCompletionExists ahead of the four scalar/structure
 * counts.  Keeping the selector next to a fully spelled six-field dispatch
 * prevents accidentally reusing the old five-field kext initializer. */
struct MlxMethodSpec {
    uint64_t selector;
    IOUserClientMethodDispatch dispatch;
};

#define MLX_UC_METHOD(sel, inSize, outSize) \
    { (sel), { NULL, 0, 0, (inSize), 0, (outSize) } }

static const MlxMethodSpec sMlxMethods[] = {
    MLX_UC_METHOD(kMlxUCMethodQueryDevice, 0,
                  sizeof(struct mlx_query_device_resp)),
    MLX_UC_METHOD(kMlxUCMethodQueryPort, 0,
                  sizeof(struct mlx_query_port_resp)),
    MLX_UC_METHOD(kMlxUCMethodQueryHealth, 0,
                  sizeof(struct mlx_health_resp)),
    MLX_UC_METHOD(kMlxUCMethodQueryAbi, 0,
                  sizeof(struct mlx_query_abi_resp)),
    MLX_UC_METHOD(kMlxUCMethodQueryLimits, 0,
                  sizeof(struct mlx_query_limits_resp)),
    MLX_UC_METHOD(kMlxUCMethodQueryStats, 0,
                  sizeof(struct mlx_stats_resp)),
    MLX_UC_METHOD(kMlxUCMethodAllocPD, 0, sizeof(uint32_t)),
    MLX_UC_METHOD(kMlxUCMethodDeallocPD, sizeof(uint32_t), 0),
    MLX_UC_METHOD(kMlxUCMethodCreateQP, sizeof(struct mlx_create_qp_req),
                  sizeof(struct mlx_create_qp_resp)),
    MLX_UC_METHOD(kMlxUCMethodModifyQP, sizeof(struct mlx_modify_qp_req), 0),
    MLX_UC_METHOD(kMlxUCMethodDestroyQP, sizeof(uint32_t), 0),
    MLX_UC_METHOD(kMlxUCMethodQueryQP, sizeof(uint32_t),
                  sizeof(struct mlx_query_qp_resp)),
    MLX_UC_METHOD(kMlxUCMethodCreateCQ, sizeof(struct mlx_create_cq_req),
                  sizeof(struct mlx_create_cq_resp)),
    MLX_UC_METHOD(kMlxUCMethodDestroyCQ, sizeof(uint32_t), 0),
    MLX_UC_METHOD(kMlxUCMethodRegMR, sizeof(struct mlx_reg_mr_req),
                  sizeof(struct mlx_reg_mr_resp)),
    MLX_UC_METHOD(kMlxUCMethodRegMRIndirect,
                  sizeof(struct mlx_reg_mr_indirect_req),
                  sizeof(struct mlx_reg_mr_resp)),
    MLX_UC_METHOD(kMlxUCMethodDeregMR, sizeof(uint32_t), 0),
    MLX_UC_METHOD(kMlxUCMethodAllocMW, sizeof(struct mlx_alloc_mw_req), sizeof(struct mlx_alloc_mw_resp)),
    MLX_UC_METHOD(kMlxUCMethodDeallocMW, sizeof(struct mlx_dealloc_mw_req), 0),
    MLX_UC_METHOD(kMlxUCMethodBindMW, sizeof(struct mlx_bind_mw_req), sizeof(struct mlx_bind_mw_resp)),
    MLX_UC_METHOD(kMlxUCMethodCreateAH, sizeof(struct mlx_create_ah_req),
                  sizeof(struct mlx_create_ah_resp)),
    MLX_UC_METHOD(kMlxUCMethodDestroyAH, sizeof(uint32_t), 0),
    MLX_UC_METHOD(kMlxUCMethodGetGidIndex, 0, sizeof(uint32_t)),
    MLX_UC_METHOD(kMlxUCMethodSetGid, sizeof(struct mlx_set_gid_req), 0),
    MLX_UC_METHOD(kMlxUCMethodDelGid, sizeof(uint32_t), 0),
    MLX_UC_METHOD(kMlxUCMethodQueryGid, sizeof(uint32_t),
                  sizeof(struct mlx_query_gid_resp)),
    MLX_UC_METHOD(kMlxUCMethodCCQuery, 0, sizeof(struct mlx_cc_params)),
    MLX_UC_METHOD(kMlxUCMethodCCModify, sizeof(struct mlx_cc_params), 0),
    MLX_UC_METHOD(kMlxUCMethodQueryCqCompletions, sizeof(uint32_t),
                  sizeof(uint64_t)),
    MLX_UC_METHOD(kMlxUCMethodGetAsyncEvent, 0,
                  sizeof(struct mlx_async_event)),
    MLX_UC_METHOD(kMlxUCMethodUpdateCqConsumer,
                  sizeof(struct mlx_update_cq_consumer_req), 0),
    MLX_UC_METHOD(kMlxUCMethodPollCQ, sizeof(struct mlx_poll_cq_req),
                  sizeof(struct mlx_poll_cq_resp)),
    MLX_UC_METHOD(kMlxUCMethodPostSend, sizeof(struct mlx_post_send_req), 0),
    MLX_UC_METHOD(kMlxUCMethodPostRecv, sizeof(struct mlx_post_recv_req), 0),
    MLX_UC_METHOD(kMlxUCMethodPostSendBatch,
                  sizeof(struct mlx_post_send_batch_req), 0),
    MLX_UC_METHOD(kMlxUCMethodPostRecvBatch,
                  sizeof(struct mlx_post_recv_batch_req), 0),
    MLX_UC_METHOD(kMlxUCMethodEnableFastPath, 0,
                  sizeof(struct mlx_fast_path_resp)),
    MLX_UC_METHOD(kMlxUCMethodSyncFastPath,
                  sizeof(struct mlx_sync_fast_path_req), 0),
    MLX_UC_METHOD(kMlxUCMethodSyncRecvFastPath,
                  sizeof(struct mlx_sync_recv_fast_path_req), 0),
    MLX_UC_METHOD(kMlxUCMethodSyncSendSge,
                  sizeof(struct mlx_sync_send_sge_req), 0),
    MLX_UC_METHOD(kMlxUCMethodSyncRecvSge,
                  sizeof(struct mlx_sync_recv_sge_req), 0),
    MLX_UC_METHOD(kMlxUCMethodPostLocalInv,
                  sizeof(struct mlx_post_local_inv_req), 0),
    MLX_UC_METHOD(kMlxUCMethodPostUmrKlm,
                  sizeof(struct mlx_post_umr_klm_req), 0),
    MLX_UC_METHOD(kMlxUCMethodPostSendSge,
                  sizeof(struct mlx_post_send_sge_req), 0),
    MLX_UC_METHOD(kMlxUCMethodPostRecvSge,
                  sizeof(struct mlx_post_recv_sge_req), 0),
    MLX_UC_METHOD(kMlxUCMethodFwReinit, 0, 0),
    MLX_UC_METHOD(kMlxUCMethodDbgFlr, 0, 0),
    MLX_UC_METHOD(kMlxUCMethodDbgExec, sizeof(struct mlx_dbg_exec_req),
                  sizeof(struct mlx_dbg_exec_resp)),
    MLX_UC_METHOD(kMlxUCMethodDbgQueryPages,
                  sizeof(struct mlx_dbg_query_pages_req),
                  sizeof(struct mlx_dbg_query_pages_resp)),
    MLX_UC_METHOD(kMlxUCMethodDbgProvidePages,
                  sizeof(struct mlx_dbg_provide_pages_req),
                  sizeof(struct mlx_dbg_provide_pages_resp)),
    MLX_UC_METHOD(kMlxUCMethodDbgDumpState, 0,
                  sizeof(struct mlx_dbg_state_resp)),
    MLX_UC_METHOD(kMlxUCMethodStableInitCycle, 0,
                  sizeof(struct mlx_stable_init_cycle_resp)),
    MLX_UC_METHOD(kMlxUCMethodPostSendInline,
                  sizeof(struct mlx_post_send_inline_req), 0),
    MLX_UC_METHOD(kMlxUCMethodPostSendAtomic,
                  sizeof(struct mlx_post_send_atomic_req), 0),
    MLX_UC_METHOD(kMlxUCMethodQueryGidTable,
                  sizeof(struct mlx_query_gid_table_req),
                  sizeof(struct mlx_query_gid_table_resp)),
    MLX_UC_METHOD(kMlxUCMethodArmCQ, sizeof(struct mlx_arm_cq_req), 0),
};

#undef MLX_UC_METHOD

kern_return_t
MlxUserClient::ExternalMethod(uint64_t selector,
                              IOUserClientMethodArguments *arguments,
                              const IOUserClientMethodDispatch *dispatch,
                              OSObject *target, void *reference)
{
    if (!ivars || !arguments)
        return kIOReturnBadArgument;

    /* P1.1: data-path selectors skip fMethodLock (refcounted under
     * fOwnedLock instead) so concurrent post/poll are not serialized. */
    bool dataPath = MlxIsDataPathSelector(selector);
    MlxDataPathGuard dataGuard(dataPath ? ivars : NULL);
    if (dataPath && !dataGuard.ok())
        return kIOReturnNotReady;
    MlxMethodLockGuard methodGuard(dataPath ? NULL : ivars->fMethodLock);

    const MlxMethodSpec *spec = NULL;
    for (uint32_t i = 0; i < sizeof(sMlxMethods) / sizeof(sMlxMethods[0]); i++) {
        if (sMlxMethods[i].selector == selector) {
            spec = &sMlxMethods[i];
            break;
        }
    }
    if (!spec)
        return kIOReturnUnsupported;

    /* All published selectors intentionally use the bounded inline-structure
     * IOConnectCallStructMethod ABI.  Descriptor-backed calls need a separate
     * copy path and are rejected instead of being treated as a NULL buffer. */
    if (arguments->structureInputDescriptor ||
        arguments->structureOutputDescriptor || arguments->structureOutput)
        return kIOReturnUnsupported;

    kern_return_t kr = super::ExternalMethod(selector, arguments,
                                              &spec->dispatch, target, reference);
    if (kr != kIOReturnSuccess && kr != kIOReturnNoCompletion)
        return kr;

    const void *in = arguments->structureInput ?
        arguments->structureInput->getBytesNoCopy() : NULL;
    void *out = NULL;
    const uint32_t outputSize = spec->dispatch.checkStructureOutputSize;
    if (outputSize) {
        void *zero = IOMallocZero(outputSize);
        if (!zero) return kIOReturnNoMemory;
        OSData *output = OSData::withBytes(zero, outputSize);
        IOFree(zero, outputSize);
        if (!output) return kIOReturnNoMemory;
        arguments->structureOutput = output; /* consumed/released by DriverKit */
        out = const_cast<void *>(output->getBytesNoCopy());
        if (!out) return kIOReturnNoMemory;
    }

    /* kMlxUCMethodFwReinit is available BEFORE RoCE comes up (that is exactly
     * its purpose — restarting initialization) — don't gate on fRoce. */
    if (selector == kMlxUCMethodFwReinit) {
        if (!ivars->fCore) return kIOReturnNotAttached;
        /* Reinit replaces the whole MlxRoCE object graph.  Do not invalidate
         * live handles and refresh the cached pointer after a rebuild. */
        if ((ivars->fOwnedQp && ivars->fOwnedQp->getCount()) ||
            (ivars->fOwnedCq && ivars->fOwnedCq->getCount()) ||
            (ivars->fOwnedMr && ivars->fOwnedMr->getCount()) ||
            (ivars->fOwnedAh && ivars->fOwnedAh->getCount()) ||
            (ivars->fOwnedGid && ivars->fOwnedGid->getCount()))
            return kIOReturnBusy;
        kern_return_t reinitKr = ivars->fCore->ReinitFw();
        ivars->fRoce = ivars->fCore->GetRoCE();
        return reinitKr;
    }

    if (selector == kMlxUCMethodStableInitCycle) {
        if (!ivars->fCore || !out) return kIOReturnNotAttached;
        if ((ivars->fOwnedPd && ivars->fOwnedPd->getCount()) ||
            ivars->fFastBundle ||
            (ivars->fOwnedQp && ivars->fOwnedQp->getCount()) ||
            (ivars->fOwnedCq && ivars->fOwnedCq->getCount()) ||
            (ivars->fOwnedMr && ivars->fOwnedMr->getCount()) ||
            (ivars->fOwnedAh && ivars->fOwnedAh->getCount()) ||
            (ivars->fOwnedGid && ivars->fOwnedGid->getCount()))
            return kIOReturnBusy;
        struct mlx_stable_init_cycle_resp *report =
            (struct mlx_stable_init_cycle_resp *)out;
        (void)ivars->fCore->StableInitCycle(report);
        ivars->fRoce = ivars->fCore->GetRoCE();
        /* The operation status is carried in report->kr so diagnostics are
         * preserved even when the no-FLR cycle failed and auto-recovered. */
        return kIOReturnSuccess;
    }

    /* Debug interface (notes/35): available BEFORE RoCE comes up — mlx_probe
     * runs init stages from userspace without rebuilding the dext. */
    if (selector >= kMlxUCMethodDbgFlr && selector <= kMlxUCMethodDbgDumpState) {
        if (!ivars->fCore) return kIOReturnNotAttached;
        return DispatchDebugMethod(selector, arguments);
    }

    if (!ivars->fRoce)
        return kIOReturnNotReady;

    /* Queries and teardown remain available for diagnosis/recovery, but no
     * new work may reach hardware until the persistent Phase 2 dependencies
     * and continuous EQ service have passed their readback gate. */
    switch (selector) {
    case kMlxUCMethodCreateQP:
    case kMlxUCMethodModifyQP:
    case kMlxUCMethodCreateCQ:
    case kMlxUCMethodRegMR:
    case kMlxUCMethodAllocMW:
    case kMlxUCMethodBindMW:
    case kMlxUCMethodCreateAH:
    case kMlxUCMethodGetGidIndex:
    case kMlxUCMethodSetGid:
    case kMlxUCMethodPostSend:
    case kMlxUCMethodPostRecv:
    case kMlxUCMethodPostSendBatch:
    case kMlxUCMethodPostRecvBatch:
    case kMlxUCMethodSyncFastPath:
    case kMlxUCMethodPostUmrKlm:
    case kMlxUCMethodPostSendSge:
    case kMlxUCMethodPostRecvSge:
    case kMlxUCMethodPostLocalInv:
    case kMlxUCMethodPostSendInline:
    case kMlxUCMethodPostSendAtomic:
    case kMlxUCMethodEnableFastPath:
        if (!ivars->fCore || !ivars->fCore->Phase2Ready())
            return kIOReturnNotReady;
        break;
    default:
        break;
    }

    switch (selector) {
    case kMlxUCMethodQueryDevice:
        return ivars->fRoce->QueryDevice((struct mlx_query_device_resp *)out);
    case kMlxUCMethodQueryPort:
        return ivars->fRoce->QueryPort((struct mlx_query_port_resp *)out);
    case kMlxUCMethodQueryHealth: {
        struct mlx_health_resp *resp = (struct mlx_health_resp *)out;
        MlxHealth *health = ivars->fCore ? ivars->fCore->GetHealth() : NULL;
        resp->healthy = health && health->IsHealthy();
        resp->syndrome = health ? health->Syndrome() : 0;
        resp->extSyndrome = health ? health->ExtSynd() : 0;
        resp->ownedPd = ivars->fOwnedPd ? ivars->fOwnedPd->getCount() : 0;
        resp->ownedQp = ivars->fOwnedQp ? ivars->fOwnedQp->getCount() : 0;
        resp->ownedCq = ivars->fOwnedCq ? ivars->fOwnedCq->getCount() : 0;
        /* Keep the stable field as total MKey ownership; MW has a separate
         * internal table so MR/MW teardown cannot cross-remove handles. */
        resp->ownedMr = (ivars->fOwnedMr ? ivars->fOwnedMr->getCount() : 0) +
                        (ivars->fOwnedMw ? ivars->fOwnedMw->getCount() : 0);
        resp->ownedAh = ivars->fOwnedAh ? ivars->fOwnedAh->getCount() : 0;
        return kIOReturnSuccess;
    }
    case kMlxUCMethodQueryAbi: {
        struct mlx_query_abi_resp *resp = (struct mlx_query_abi_resp *)out;
        resp->version = MLX_UC_ABI_VERSION;
        resp->features = MLX_UC_FEATURE_RC | MLX_UC_FEATURE_ROCE_V2 |
                         MLX_UC_FEATURE_DIRECT_PATH |
                         MLX_UC_FEATURE_ASYNC_EVENTS |
                         MLX_UC_FEATURE_INDIRECT_MR |
                         MLX_UC_FEATURE_QP_RECOVERY |
                         MLX_UC_FEATURE_MULTI_SGE |
                         MLX_UC_FEATURE_IMMEDIATE_DATA |
                         MLX_UC_FEATURE_HEALTH_QUERY |
                         MLX_UC_FEATURE_STATS |
                         MLX_UC_FEATURE_INLINE |
                         MLX_UC_FEATURE_ATOMIC;
        return kIOReturnSuccess;
    }
    case kMlxUCMethodQueryLimits: {
        struct mlx_query_limits_resp *resp = (struct mlx_query_limits_resp *)out;
        const MlxHcaCaps *caps = ClientCaps(ivars);
        resp->maxPd = MLX_UC_MAX_PD_PER_CLIENT;
        resp->maxQp = ClientQpLimit(ivars);
        resp->maxCq = ClientCqLimit(ivars);
        resp->maxMr = ClientMrLimit(ivars);
        resp->maxMw = ClientMwLimit(ivars);
        resp->maxAh = MLX_UC_MAX_AH_PER_CLIENT;
        resp->maxGid = CapLimit(MLX_UC_MAX_GID_PER_CLIENT,
                                caps ? caps->roceMaxGid : 0);
        resp->maxSqDepth = MLX_UC_MAX_SQ_DEPTH;
        resp->maxRqDepth = MLX_UC_MAX_RQ_DEPTH;
        resp->fwCmdBurst = MLX_UC_FW_CMD_BURST;
        resp->fwCmdWindowNs = MLX_UC_FW_CMD_WINDOW_NS;
        resp->maxDbRecords = DbRecordLimit(ivars);
        return kIOReturnSuccess;
    }
    case kMlxUCMethodQueryStats: {
        struct mlx_stats_resp *resp = (struct mlx_stats_resp *)out;
        memset(resp, 0, sizeof(*resp));
        MlxQP *qpTable = ivars->fRoce->GetQP();
        if (qpTable && ivars->fOwnedQp) {
            for (uint32_t i = 0; i < ivars->fOwnedQp->getCount(); i++) {
                OSData *record = OSDynamicCast(OSData, ivars->fOwnedQp->getObject(i));
                if (!record || record->getLength() != sizeof(uint32_t)) continue;
                MlxQPContext *qp = qpTable->Lookup(
                    *(const uint32_t *)record->getBytesNoCopy());
                if (!qp) continue;
                resp->postedSend       += qp->postedSend;
                resp->postedRead       += qp->postedRead;
                resp->postedWrite      += qp->postedWrite;
                resp->postedUmr        += qp->postedUmr;
                resp->postedBindMw     += qp->postedBindMw;
                resp->postedLocalInv   += qp->postedLocalInv;
                resp->postedRecv       += qp->rqPkts;
                resp->completedSend    += qp->completedSend;
                resp->completedRead    += qp->completedRead;
                resp->completedWrite   += qp->completedWrite;
                resp->completedRecv    += qp->completedRecv;
                resp->completedUmr     += qp->completedUmr;
                resp->completedLocalInv += qp->completedLocalInv;
                resp->cqeError         += qp->cqeError;
                resp->cqeRetryExc      += qp->cqeRetryExc;
                resp->cqeRnrRetry      += qp->cqeRnrRetry;
                resp->sqOccupancy      += (uint32_t)(qp->sqHead - qp->sqTail);
                resp->rqOccupancy      += (uint32_t)(qp->rqHead - qp->rqTail);
            }
        }
        MlxCQ *cqTable = ivars->fRoce->GetCQ();
        if (cqTable && ivars->fOwnedCq) {
            for (uint32_t i = 0; i < ivars->fOwnedCq->getCount(); i++) {
                OSData *record = OSDynamicCast(OSData, ivars->fOwnedCq->getObject(i));
                if (!record || record->getLength() != sizeof(uint32_t)) continue;
                MlxCQContext *cq = cqTable->Lookup(
                    *(const uint32_t *)record->getBytesNoCopy());
                if (cq) resp->cqLost += cq->lost;
            }
        }
        return kIOReturnSuccess;
    }
    case kMlxUCMethodAllocPD:
        {
            if (!QuotaReserve(ivars, &ivars->fQuotaPd, MLX_UC_MAX_PD_PER_CLIENT))
                return kIOReturnNoResources;
            uint8_t cmdIn[16] = {}, cmdOut[16] = {};
            mlxSetBits(cmdIn, 0x00, 16, MLX_CMD_OP_ALLOC_PD);
            kern_return_t pdKr = ivars->fCore->Exec(
                MLX_CMD_OP_ALLOC_PD, cmdIn, sizeof(cmdIn), cmdOut,
                sizeof(cmdOut), 5000);
            if (pdKr != kIOReturnSuccess) {
                QuotaRelease(ivars, &ivars->fQuotaPd);
                return pdKr;
            }
            uint32_t pd = (uint32_t)mlxGetBits(cmdOut, 0x48, 24);
            if (!pd) { QuotaRelease(ivars, &ivars->fQuotaPd); return kIOReturnNoMemory; }
            if (!AddOwned(ivars->fOwnedPd, pd)) {
                QuotaRelease(ivars, &ivars->fQuotaPd);
                uint8_t freeIn[16] = {}, freeOut[16] = {};
                mlxSetBits(freeIn, 0x00, 16, MLX_CMD_OP_DEALLOC_PD);
                mlxSetBits(freeIn, 0x48, 24, pd);
                (void)ivars->fCore->Exec(MLX_CMD_OP_DEALLOC_PD, freeIn,
                                         sizeof(freeIn), freeOut, sizeof(freeOut), 5000);
                return kIOReturnNoMemory;
            }
            uint32_t token = TokenCreate(ivars, MLX_T_PD, pd);
            if (!token) {
                QuotaRelease(ivars, &ivars->fQuotaPd);
                (void)RemoveOwned(ivars->fOwnedPd, pd);
                uint8_t freeIn[16] = {}, freeOut[16] = {};
                mlxSetBits(freeIn, 0x00, 16, MLX_CMD_OP_DEALLOC_PD);
                mlxSetBits(freeIn, 0x48, 24, pd);
                (void)ivars->fCore->Exec(MLX_CMD_OP_DEALLOC_PD, freeIn,
                                         sizeof(freeIn), freeOut, sizeof(freeOut), 5000);
                return kIOReturnNoMemory;
            }
            *(uint32_t *)out = token;
        }
        return kIOReturnSuccess;
    case kMlxUCMethodDeallocPD: {
        uint32_t token = *(const uint32_t *)in;
        uint32_t pd = TokenResolve(ivars, token, MLX_T_PD);
        if (!pd || !Owns(ivars->fOwnedPd, pd)) return kIOReturnNotPermitted;
        if (ivars->fOwnedQp && ivars->fRoce && ivars->fRoce->GetQP())
            for (uint32_t i = 0; i < ivars->fOwnedQp->getCount(); i++) {
                OSData *record = OSDynamicCast(OSData, ivars->fOwnedQp->getObject(i));
                MlxQPContext *qp = record && record->getLength() == sizeof(uint32_t)
                    ? ivars->fRoce->GetQP()->Lookup(
                        *(const uint32_t *)record->getBytesNoCopy()) : NULL;
                if (qp && qp->pd == pd) return kIOReturnBusy;
            }
        if (ivars->fOwnedMr && ivars->fRoce && ivars->fRoce->GetMR())
            for (uint32_t i = 0; i < ivars->fOwnedMr->getCount(); i++) {
                OSData *record = OSDynamicCast(OSData, ivars->fOwnedMr->getObject(i));
                MlxMRContext *mr = record && record->getLength() == sizeof(uint32_t)
                    ? ivars->fRoce->GetMR()->Lookup(
                        *(const uint32_t *)record->getBytesNoCopy()) : NULL;
                if (mr && mr->pd == pd) return kIOReturnBusy;
            }
        if (ivars->fOwnedMw && ivars->fRoce && ivars->fRoce->GetMR())
            for (uint32_t i = 0; i < ivars->fOwnedMw->getCount(); i++) {
                OSData *record = OSDynamicCast(OSData, ivars->fOwnedMw->getObject(i));
                MlxMRContext *mw = record && record->getLength() == sizeof(uint32_t)
                    ? ivars->fRoce->GetMR()->Lookup(
                        *(const uint32_t *)record->getBytesNoCopy()) : NULL;
                if (mw && mw->pd == pd) return kIOReturnBusy;
            }
        if (!RemoveOwned(ivars->fOwnedPd, pd)) return kIOReturnNotPermitted;
        {
            uint8_t cmdIn[16] = {}, cmdOut[16] = {};
            mlxSetBits(cmdIn, 0x00, 16, MLX_CMD_OP_DEALLOC_PD);
            mlxSetBits(cmdIn, 0x48, 24, pd);
            kern_return_t pdKr = ivars->fCore->Exec(
                MLX_CMD_OP_DEALLOC_PD, cmdIn, sizeof(cmdIn), cmdOut,
                sizeof(cmdOut), 5000);
            if (pdKr != kIOReturnSuccess)
                AddOwned(ivars->fOwnedPd, pd);
            else {
                (void)TokenDrop(ivars, token, MLX_T_PD, NULL);
                QuotaRelease(ivars, &ivars->fQuotaPd);
            }
            return pdKr;
        }
    }
    case kMlxUCMethodCreateQP: {
        const struct mlx_create_qp_req *req =
            (const struct mlx_create_qp_req *)in;
        if (req->sqSize > MLX_UC_MAX_SQ_DEPTH ||
            req->rqSize > MLX_UC_MAX_RQ_DEPTH)
            return kIOReturnBadArgument;
        struct mlx_create_qp_req raw = *req;
        raw.pd = TokenResolve(ivars, req->pd, MLX_T_PD);
        raw.sendCq = TokenResolve(ivars, req->sendCq, MLX_T_CQ);
        raw.recvCq = TokenResolve(ivars, req->recvCq, MLX_T_CQ);
        if (!raw.pd || !raw.sendCq || !raw.recvCq ||
            !Owns(ivars->fOwnedPd, raw.pd) ||
            !Owns(ivars->fOwnedCq, raw.sendCq) ||
            !Owns(ivars->fOwnedCq, raw.recvCq))
            return kIOReturnNotPermitted;
        if (!QuotaReserve(ivars, &ivars->fQuotaQp, ClientQpLimit(ivars)))
            return kIOReturnNoResources;
        if (!QuotaReserveDb(ivars)) {
            QuotaRelease(ivars, &ivars->fQuotaQp);
            return kIOReturnNoResources;
        }
        struct mlx_create_qp_resp *resp = (struct mlx_create_qp_resp *)out;
        kern_return_t r = ivars->fRoce->CreateQP(&raw, resp,
                                                  ivars->fFastBundle);
        if (r == kIOReturnSuccess) {
            uint32_t rawQpn = resp->qpn;
            uint32_t token = TokenCreate(ivars, MLX_T_QP, rawQpn);
            if (!token || !AddOwned(ivars->fOwnedQp, rawQpn)) {
                ivars->fRoce->DestroyQP(rawQpn);
                QuotaReleaseDb(ivars);
                QuotaRelease(ivars, &ivars->fQuotaQp);
                return kIOReturnNoMemory;
            }
            resp->qpn = token;
            resp->hwQpn = rawQpn;
        } else {
            QuotaReleaseDb(ivars);
            QuotaRelease(ivars, &ivars->fQuotaQp);
        }
        return r;
    }
    case kMlxUCMethodModifyQP: {
        const struct mlx_modify_qp_req *req =
            (const struct mlx_modify_qp_req *)in;
        struct mlx_modify_qp_req raw = *req;
        raw.qpn = TokenResolve(ivars, req->qpn, MLX_T_QP);
        if (!raw.qpn || !Owns(ivars->fOwnedQp, raw.qpn))
            return kIOReturnNotPermitted;
        return ivars->fRoce->ModifyQP(&raw);
    }
    case kMlxUCMethodDestroyQP: {
        uint32_t token = *(const uint32_t *)in;
        uint32_t h = TokenResolve(ivars, token, MLX_T_QP);
        if (!h || !RemoveOwned(ivars->fOwnedQp, h))
            return kIOReturnNotPermitted;
        kern_return_t r = ivars->fRoce->DestroyQP(h);
        if (r != kIOReturnSuccess) AddOwned(ivars->fOwnedQp, h);
        else {
            (void)TokenDrop(ivars, token, MLX_T_QP, NULL);
            QuotaReleaseDb(ivars);
            QuotaRelease(ivars, &ivars->fQuotaQp);
        }
        return r;
    }
    case kMlxUCMethodQueryQP: {
        uint32_t token = *(const uint32_t *)in;
        uint32_t qpn = TokenResolve(ivars, token, MLX_T_QP);
        if (!qpn || !Owns(ivars->fOwnedQp, qpn))
            return kIOReturnNotPermitted;
        struct mlx_query_qp_resp *resp = (struct mlx_query_qp_resp *)out;
        kern_return_t r = ivars->fRoce->GetQP()->QueryQP(qpn, resp);
        if (r == kIOReturnSuccess) {
            resp->qpn = token;
            uint32_t scq = TokenForRaw(ivars, MLX_T_CQ, resp->sendCq);
            uint32_t rcq = TokenForRaw(ivars, MLX_T_CQ, resp->recvCq);
            if (scq) resp->sendCq = scq;
            if (rcq) resp->recvCq = rcq;
        }
        return r;
    }
    case kMlxUCMethodCreateCQ: {
        const struct mlx_create_cq_req *req = (const struct mlx_create_cq_req *)in;
        if (!QuotaReserve(ivars, &ivars->fQuotaCq, ClientCqLimit(ivars)))
            return kIOReturnNoResources;
        if (!QuotaReserveDb(ivars)) {
            QuotaRelease(ivars, &ivars->fQuotaCq);
            return kIOReturnNoResources;
        }
        struct mlx_create_cq_resp *resp = (struct mlx_create_cq_resp *)out;
        kern_return_t r = ivars->fRoce->CreateCQ(req->entries, resp,
                                                  ivars->fFastBundle);
        if (r == kIOReturnSuccess) {
            uint32_t rawCq = resp->cqHandle;
            uint32_t token = TokenCreate(ivars, MLX_T_CQ, rawCq);
            if (!token || !AddOwned(ivars->fOwnedCq, rawCq)) {
                ivars->fRoce->DestroyCQ(rawCq);
                QuotaReleaseDb(ivars);
                QuotaRelease(ivars, &ivars->fQuotaCq);
                return kIOReturnNoMemory;
            }
            resp->cqHandle = token;
        } else {
            QuotaReleaseDb(ivars);
            QuotaRelease(ivars, &ivars->fQuotaCq);
        }
        return r;
    }
    case kMlxUCMethodDestroyCQ: {
        uint32_t token = *(const uint32_t *)in;
        uint32_t h = TokenResolve(ivars, token, MLX_T_CQ);
        if (!h || !RemoveOwned(ivars->fOwnedCq, h)) return kIOReturnNotPermitted;
        if (ivars->fOwnedQp && ivars->fRoce && ivars->fRoce->GetQP()) {
            for (uint32_t i = 0; i < ivars->fOwnedQp->getCount(); i++) {
                OSData *record = OSDynamicCast(OSData, ivars->fOwnedQp->getObject(i));
                if (!record || record->getLength() != sizeof(uint32_t)) continue;
                MlxQPContext *qp = ivars->fRoce->GetQP()->Lookup(
                    *(const uint32_t *)record->getBytesNoCopy());
                if (qp && (qp->sendCq == h || qp->recvCq == h)) {
                    AddOwned(ivars->fOwnedCq, h);
                    return kIOReturnBusy;
                }
            }
        }
        kern_return_t r = ivars->fRoce->DestroyCQ(h);
        if (r != kIOReturnSuccess) AddOwned(ivars->fOwnedCq, h);
        else {
            (void)TokenDrop(ivars, token, MLX_T_CQ, NULL);
            QuotaReleaseDb(ivars);
            QuotaRelease(ivars, &ivars->fQuotaCq);
        }
        return r;
    }
    case kMlxUCMethodRegMR: {
        const struct mlx_reg_mr_req *req =
            (const struct mlx_reg_mr_req *)in;
        struct mlx_reg_mr_req raw = *req;
        raw.pd = TokenResolve(ivars, req->pd, MLX_T_PD);
        if (!raw.pd || !Owns(ivars->fOwnedPd, raw.pd) || !req->startAddr ||
            !req->length || req->startAddr + req->length < req->startAddr)
            return kIOReturnBadArgument;
        if (!QuotaReserve(ivars, &ivars->fQuotaMr, ClientMrLimit(ivars)))
            return kIOReturnNoResources;
        if (!QuotaReserveMkey(ivars)) {
            QuotaRelease(ivars, &ivars->fQuotaMr);
            return kIOReturnNoResources;
        }
        IOAddressSegment ranges[32] = {};
        ranges[0].address = req->startAddr;
        ranges[0].length = req->length;
        IOMemoryDescriptor *clientMemory = NULL;
        kern_return_t r = CreateMemoryDescriptorFromClient(
            kIOMemoryDirectionOutIn | kIOMemoryDisableCopyOnWrite,
            1, ranges, &clientMemory);
        if (r != kIOReturnSuccess || !clientMemory) {
            QuotaReleaseMkey(ivars);
            QuotaRelease(ivars, &ivars->fQuotaMr);
            return r ? r : kIOReturnNoMemory;
        }
        struct mlx_reg_mr_resp *resp = (struct mlx_reg_mr_resp *)out;
        r = ivars->fRoce->RegMR(&raw, clientMemory, resp);
        clientMemory->release();
        if (r == kIOReturnSuccess) {
            uint32_t rawMr = resp->mrHandle;
            uint32_t token = TokenCreate(ivars, MLX_T_MR, rawMr);
            if (!token || !AddOwned(ivars->fOwnedMr, rawMr)) {
                ivars->fRoce->DeregMR(rawMr);
                QuotaReleaseMkey(ivars);
                QuotaRelease(ivars, &ivars->fQuotaMr);
                return kIOReturnNoMemory;
            }
            resp->mrHandle = token;
        } else {
            QuotaReleaseMkey(ivars);
            QuotaRelease(ivars, &ivars->fQuotaMr);
        }
        return r;
    }
    case kMlxUCMethodRegMRIndirect: {
        const struct mlx_reg_mr_indirect_req *req =
            (const struct mlx_reg_mr_indirect_req *)in;
        struct mlx_reg_mr_indirect_req raw = *req;
        raw.pd = TokenResolve(ivars, req->pd, MLX_T_PD);
        if (!raw.pd || !Owns(ivars->fOwnedPd, raw.pd) || !req->startAddr ||
            !req->length || req->startAddr + req->length < req->startAddr ||
            !req->childCount ||
            req->childCount > MLX_UC_MAX_INDIRECT_CHILDREN)
            return kIOReturnBadArgument;
        for (uint32_t i = 0; i < req->childCount; i++) {
            raw.childHandles[i] = TokenResolve(ivars, req->childHandles[i], MLX_T_MR);
            if (!raw.childHandles[i] || !Owns(ivars->fOwnedMr, raw.childHandles[i]))
                return kIOReturnNotPermitted;
        }
        if (!QuotaReserve(ivars, &ivars->fQuotaMr, ClientMrLimit(ivars)))
            return kIOReturnNoResources;
        if (!QuotaReserveMkey(ivars)) {
            QuotaRelease(ivars, &ivars->fQuotaMr);
            return kIOReturnNoResources;
        }
        struct mlx_reg_mr_resp *resp = (struct mlx_reg_mr_resp *)out;
        kern_return_t r = ivars->fRoce->RegMRIndirect(&raw, resp);
        if (r == kIOReturnSuccess) {
            uint32_t rawMr = resp->mrHandle;
            uint32_t token = TokenCreate(ivars, MLX_T_MR, rawMr);
            if (!token || !AddOwned(ivars->fOwnedMr, rawMr)) {
                ivars->fRoce->DeregMR(rawMr);
                QuotaReleaseMkey(ivars);
                QuotaRelease(ivars, &ivars->fQuotaMr);
                return kIOReturnNoMemory;
            }
            resp->mrHandle = token;
        } else {
            QuotaReleaseMkey(ivars);
            QuotaRelease(ivars, &ivars->fQuotaMr);
        }
        return r;
    }
    case kMlxUCMethodAllocMW: {
        const struct mlx_alloc_mw_req *req = (const struct mlx_alloc_mw_req *)in;
        uint32_t pd = TokenResolve(ivars, req->pd, MLX_T_PD);
        if (!pd || !Owns(ivars->fOwnedPd, pd) || req->type != 2)
            return kIOReturnNotPermitted;
        if (!QuotaReserve(ivars, &ivars->fQuotaMw, ClientMwLimit(ivars)))
            return kIOReturnNoResources;
        if (!QuotaReserveMkey(ivars)) {
            QuotaRelease(ivars, &ivars->fQuotaMw);
            return kIOReturnNoResources;
        }
        struct mlx_alloc_mw_resp *resp = (struct mlx_alloc_mw_resp *)out;
        uint32_t rawMw = 0, rkey = 0;
        kern_return_t r = ivars->fRoce->AllocMW(pd, req->type, &rawMw, &rkey);
        if (r == kIOReturnSuccess) {
            uint32_t token = TokenCreate(ivars, MLX_T_MW, rawMw);
            if (!token || !AddOwned(ivars->fOwnedMw, rawMw)) {
                ivars->fRoce->DeallocMW(rawMw);
                QuotaReleaseMkey(ivars);
                QuotaRelease(ivars, &ivars->fQuotaMw);
                return kIOReturnNoMemory;
            }
            resp->mwHandle = token;
            resp->rkey = rkey;
        } else {
            QuotaReleaseMkey(ivars);
            QuotaRelease(ivars, &ivars->fQuotaMw);
        }
        return r;
    }
    case kMlxUCMethodDeallocMW: {
        const struct mlx_dealloc_mw_req *req = (const struct mlx_dealloc_mw_req *)in;
        uint32_t rawMw = TokenResolve(ivars, req->mwHandle, MLX_T_MW);
        if (!rawMw || !RemoveOwned(ivars->fOwnedMw, rawMw))
            return kIOReturnNotPermitted;
        kern_return_t r = ivars->fRoce->DeallocMW(rawMw);
        if (r != kIOReturnSuccess) AddOwned(ivars->fOwnedMw, rawMw);
        else {
            (void)TokenDrop(ivars, req->mwHandle, MLX_T_MW, NULL);
            QuotaReleaseMkey(ivars);
            QuotaRelease(ivars, &ivars->fQuotaMw);
        }
        return r;
    }
    case kMlxUCMethodBindMW: {
        const struct mlx_bind_mw_req *req = (const struct mlx_bind_mw_req *)in;
        struct mlx_bind_mw_req raw = *req;
        raw.qpn = TokenResolve(ivars, req->qpn, MLX_T_QP);
        raw.mwHandle = TokenResolve(ivars, req->mwHandle, MLX_T_MW);
        raw.mrHandle = TokenResolve(ivars, req->mrHandle, MLX_T_MR);
        MlxMRContext *mw = ivars->fRoce->GetMR()->Lookup(raw.mwHandle);
        MlxMRContext *mr = ivars->fRoce->GetMR()->Lookup(raw.mrHandle);
        if (!raw.qpn || !mw || !mr || !mw->isWindow ||
            !Owns(ivars->fOwnedMw, raw.mwHandle) ||
            !Owns(ivars->fOwnedMr, raw.mrHandle) ||
            !Owns(ivars->fOwnedQp, raw.qpn))
            return kIOReturnNotPermitted;
        return ivars->fRoce->BindMW(&raw, (struct mlx_bind_mw_resp *)out);
    }
    case kMlxUCMethodDeregMR: {
        uint32_t token = *(const uint32_t *)in;
        uint32_t h = TokenResolve(ivars, token, MLX_T_MR);
        if (!h || !RemoveOwned(ivars->fOwnedMr, h))
            return kIOReturnNotPermitted;
        kern_return_t r = ivars->fRoce->DeregMR(h);
        if (r != kIOReturnSuccess) AddOwned(ivars->fOwnedMr, h);
        else {
            (void)TokenDrop(ivars, token, MLX_T_MR, NULL);
            QuotaReleaseMkey(ivars);
            QuotaRelease(ivars, &ivars->fQuotaMr);
        }
        return r;
    }
    case kMlxUCMethodCreateAH: {
        if (!QuotaReserve(ivars, &ivars->fQuotaAh, MLX_UC_MAX_AH_PER_CLIENT))
            return kIOReturnNoResources;
        struct mlx_create_ah_resp *resp = (struct mlx_create_ah_resp *)out;
        kern_return_t r = ivars->fRoce->CreateAH((const struct mlx_create_ah_req *)in, resp);
        if (r == kIOReturnSuccess && !AddOwned(ivars->fOwnedAh, resp->ahHandle)) {
            ivars->fRoce->DestroyAH(resp->ahHandle);
            QuotaRelease(ivars, &ivars->fQuotaAh);
            return kIOReturnNoMemory;
        }
        if (r != kIOReturnSuccess) QuotaRelease(ivars, &ivars->fQuotaAh);
        return r;
    }
    case kMlxUCMethodDestroyAH: {
        uint32_t h = *(const uint32_t *)in;
        if (!RemoveOwned(ivars->fOwnedAh, h)) return kIOReturnNotPermitted;
        kern_return_t r = ivars->fRoce->DestroyAH(h);
        if (r != kIOReturnSuccess) AddOwned(ivars->fOwnedAh, h);
        else QuotaRelease(ivars, &ivars->fQuotaAh);
        return r;
    }
    case kMlxUCMethodGetGidIndex: {
        if (!ivars->fRoce->GetGID()) return kIOReturnNoResources;
        if (!QuotaReserve(ivars, &ivars->fQuotaGid, MLX_UC_MAX_GID_PER_CLIENT))
            return kIOReturnNoResources;
        uint32_t index = ivars->fRoce->GetGID()->AllocGIDIndex();
        if (index == 0xFFFFFFFF) {
            QuotaRelease(ivars, &ivars->fQuotaGid);
            return kIOReturnNoResources;
        }
        if (!AddOwned(ivars->fOwnedGid, index)) {
            ivars->fRoce->GetGID()->FreeGIDIndex(index);
            QuotaRelease(ivars, &ivars->fQuotaGid);
            return kIOReturnNoMemory;
        }
        *(uint32_t *)out = index;
        return kIOReturnSuccess;
    }
    case kMlxUCMethodSetGid: {
        const struct mlx_set_gid_req *req =
            (const struct mlx_set_gid_req *)in;
        if (!Owns(ivars->fOwnedGid, req->index))
            return kIOReturnNotPermitted;
        if (req->roceVersion != MLX_ROCE_VERSION_2 || req->l3Type > 1 ||
            (req->vlanValid && req->vlanId > 4095))
            return kIOReturnBadArgument;
        kern_return_t r = ivars->fRoce->GetGID()->SetGID(
            req->index, req->gid, req->mac, req->roceVersion,
            req->l3Type, req->vlanValid != 0, req->vlanId);
        /* SetGID performs firmware readback. If that fails, release the
         * client reservation so a failed transient slot cannot poison every
         * subsequent connection. */
        if (r != kIOReturnSuccess) {
            /* SET may have reached firmware before readback failed. Clear the
             * firmware slot before dropping ownership; otherwise the caller's
             * subsequent DelGID is correctly denied and the slot leaks. */
            (void)ivars->fRoce->GetGID()->DelGID(req->index);
            (void)RemoveOwned(ivars->fOwnedGid, req->index);
            ivars->fRoce->GetGID()->FreeGIDIndex(req->index);
            QuotaRelease(ivars, &ivars->fQuotaGid);
        }
        return r;
    }
    case kMlxUCMethodDelGid: {
        uint32_t index = *(const uint32_t *)in;
        if (!RemoveOwned(ivars->fOwnedGid, index))
            return kIOReturnNotPermitted;
        kern_return_t r = ivars->fRoce->GetGID()->DelGID(index);
        if (r != kIOReturnSuccess) AddOwned(ivars->fOwnedGid, index);
        else QuotaRelease(ivars, &ivars->fQuotaGid);
        return r;
    }
    case kMlxUCMethodQueryGid: {
        uint32_t index = *(const uint32_t *)in;
        if (!Owns(ivars->fOwnedGid, index)) return kIOReturnNotPermitted;
        MlxGIDEntry entry = {};
        kern_return_t r = ivars->fRoce->GetGID()->QueryGID(index, &entry);
        if (r != kIOReturnSuccess) return r;
        struct mlx_query_gid_resp *resp = (struct mlx_query_gid_resp *)out;
        resp->index = entry.index;
        memcpy(resp->gid, entry.gid, sizeof(resp->gid));
        memcpy(resp->mac, entry.mac, sizeof(resp->mac));
        resp->roceVersion = entry.roceVersion;
        resp->l3Type = entry.l3Type;
        resp->vlanValid = entry.vlanEn ? 1 : 0;
        resp->vlanId = entry.vlanId;
        resp->gidType = entry.gidType ? entry.gidType : 2;
        resp->ifindex = 0;   /* no macOS netif behind the DEXT */
        return kIOReturnSuccess;
    }
    case kMlxUCMethodQueryGidTable:
        return ivars->fRoce->QueryGidTable(
            (const struct mlx_query_gid_table_req *)in,
            (struct mlx_query_gid_table_resp *)out);
    case kMlxUCMethodCCQuery:
        return ivars->fRoce->GetCC() ?
               ivars->fRoce->GetCC()->QueryParams((struct mlx_cc_params *)out) :
               kIOReturnNoResources;
    case kMlxUCMethodCCModify:
        return ivars->fRoce->GetCC() ?
               ivars->fRoce->GetCC()->ModifyParams((const struct mlx_cc_params *)in) :
               kIOReturnNoResources;
    case kMlxUCMethodQueryCqCompletions: {
        uint32_t token = *(const uint32_t *)in;
        uint32_t h = TokenResolve(ivars, token, MLX_T_CQ);
        if (!h || !Owns(ivars->fOwnedCq, h)) return kIOReturnNotPermitted;
        *(uint64_t *)out = ivars->fRoce->GetCQ()->GetCompletions(h);
        return kIOReturnSuccess;
    }
    case kMlxUCMethodArmCQ: {
        const struct mlx_arm_cq_req *req = (const struct mlx_arm_cq_req *)in;
        struct mlx_arm_cq_req raw = *req;
        raw.cqHandle = TokenResolve(ivars, req->cqHandle, MLX_T_CQ);
        if (!raw.cqHandle || !Owns(ivars->fOwnedCq, raw.cqHandle))
            return kIOReturnNotPermitted;
        return ivars->fRoce->ArmCQ(&raw);
    }
    case kMlxUCMethodGetAsyncEvent:
        return ivars->fRoce->GetAsyncEvent((struct mlx_async_event *)out);
    case kMlxUCMethodUpdateCqConsumer: {
        const struct mlx_update_cq_consumer_req *req =
            (const struct mlx_update_cq_consumer_req *)in;
        struct mlx_update_cq_consumer_req raw = *req;
        raw.cqHandle = TokenResolve(ivars, req->cqHandle, MLX_T_CQ);
        if (!raw.cqHandle || !Owns(ivars->fOwnedCq, raw.cqHandle))
            return kIOReturnNotPermitted;
        return ivars->fRoce->GetCQ()->UpdateCqConsumer(
            raw.cqHandle, raw.consumerIndex);
    }
    case kMlxUCMethodPollCQ: {
        const struct mlx_poll_cq_req *req =
            (const struct mlx_poll_cq_req *)in;
        struct mlx_poll_cq_req raw = *req;
        raw.cqHandle = TokenResolve(ivars, req->cqHandle, MLX_T_CQ);
        if (!raw.cqHandle || !Owns(ivars->fOwnedCq, raw.cqHandle))
            return kIOReturnNotPermitted;
        struct mlx_poll_cq_resp *resp = (struct mlx_poll_cq_resp *)out;
        kern_return_t r = ivars->fRoce->GetCQ()->PollCQ(&raw, resp);
        if (r == kIOReturnSuccess)
            for (uint32_t i = 0; i < resp->count; i++) {
                uint32_t t = TokenForRaw(ivars, MLX_T_QP, resp->wc[i].qpNum);
                if (t) resp->wc[i].qpNum = t;
            }
        return r;
    }
    case kMlxUCMethodPostSend: {
        const struct mlx_post_send_req *req =
            (const struct mlx_post_send_req *)in;
        struct mlx_post_send_req raw = *req;
        raw.qpn = TokenResolve(ivars, req->qpn, MLX_T_QP);
        if (!raw.qpn || !Owns(ivars->fOwnedQp, raw.qpn))
            return kIOReturnNotPermitted;
        MlxMRContext *mr = ivars->fRoce->GetMR()->LookupByLkey(req->sge.lkey);
        if (!mr || !Owns(ivars->fOwnedMr, mr->mrHandle))
            return kIOReturnNotPermitted;
        return ivars->fRoce->GetQP()->PostSend(&raw);
    }
    case kMlxUCMethodPostRecv: {
        const struct mlx_post_recv_req *req =
            (const struct mlx_post_recv_req *)in;
        struct mlx_post_recv_req raw = *req;
        raw.qpn = TokenResolve(ivars, req->qpn, MLX_T_QP);
        if (!raw.qpn || !Owns(ivars->fOwnedQp, raw.qpn))
            return kIOReturnNotPermitted;
        MlxMRContext *mr = ivars->fRoce->GetMR()->LookupByLkey(req->sge.lkey);
        if (!mr || !Owns(ivars->fOwnedMr, mr->mrHandle))
            return kIOReturnNotPermitted;
        return ivars->fRoce->GetQP()->PostRecv(&raw);
    }
    case kMlxUCMethodPostSendBatch: {
        const struct mlx_post_send_batch_req *batch =
            (const struct mlx_post_send_batch_req *)in;
        if (!batch->count || batch->count > MLX_UC_MAX_POST_BATCH)
            return kIOReturnBadArgument;
        struct mlx_post_send_batch_req raw = *batch;
        uint32_t qpn = TokenResolve(ivars, batch->wr[0].qpn, MLX_T_QP);
        if (!qpn || !Owns(ivars->fOwnedQp, qpn))
            return kIOReturnNotPermitted;
        for (uint32_t i = 0; i < batch->count; i++) {
            if (batch->wr[i].qpn != batch->wr[0].qpn)
                return kIOReturnBadArgument;
            raw.wr[i].qpn = qpn;
            MlxMRContext *mr =
                ivars->fRoce->GetMR()->LookupByLkey(batch->wr[i].sge.lkey);
            if (!mr || !Owns(ivars->fOwnedMr, mr->mrHandle))
                return kIOReturnNotPermitted;
        }
        return ivars->fRoce->GetQP()->PostSendBatch(raw.wr, batch->count);
    }
    case kMlxUCMethodSyncRecvFastPath: {
        const struct mlx_sync_recv_fast_path_req *batch =
            (const struct mlx_sync_recv_fast_path_req *)in;
        if (!ivars->fFastBundle || !batch->count ||
            batch->count > MLX_UC_MAX_POST_BATCH)
            return kIOReturnNotPermitted;
        struct mlx_sync_recv_fast_path_req raw = *batch;
        uint32_t qpn = TokenResolve(ivars, batch->wr[0].qpn, MLX_T_QP);
        if (!qpn || !Owns(ivars->fOwnedQp, qpn))
            return kIOReturnNotPermitted;
        for (uint32_t i = 0; i < batch->count; i++) {
            if (batch->wr[i].qpn != batch->wr[0].qpn)
                return kIOReturnBadArgument;
            raw.wr[i].qpn = qpn;
            MlxMRContext *mr = ivars->fRoce->GetMR()->LookupByLkey(
                batch->wr[i].sge.lkey);
            if (!mr || !Owns(ivars->fOwnedMr, mr->mrHandle))
                return kIOReturnNotPermitted;
        }
        return ivars->fRoce->GetQP()->SyncRecvFastPath(raw.wr,
                                                         batch->count);
    }
    case kMlxUCMethodSyncFastPath: {
        const struct mlx_sync_fast_path_req *batch =
            (const struct mlx_sync_fast_path_req *)in;
        if (!ivars->fFastBundle || !batch->count ||
            batch->count > MLX_UC_MAX_POST_BATCH)
            return kIOReturnNotPermitted;
        struct mlx_sync_fast_path_req raw = *batch;
        uint32_t qpn = TokenResolve(ivars, batch->wr[0].qpn, MLX_T_QP);
        if (!qpn || !Owns(ivars->fOwnedQp, qpn))
            return kIOReturnNotPermitted;
        for (uint32_t i = 0; i < batch->count; i++) {
            if (batch->wr[i].qpn != batch->wr[0].qpn)
                return kIOReturnBadArgument;
            raw.wr[i].qpn = qpn;
            MlxMRContext *mr = ivars->fRoce->GetMR()->LookupByLkey(
                batch->wr[i].sge.lkey);
            if (!mr || !Owns(ivars->fOwnedMr, mr->mrHandle))
                return kIOReturnNotPermitted;
        }
        return ivars->fRoce->GetQP()->SyncFastPath(raw.wr,
                                                    batch->count);
    }
    case kMlxUCMethodSyncSendSge: {
        const struct mlx_sync_send_sge_req *req =
            (const struct mlx_sync_send_sge_req *)in;
        struct mlx_sync_send_sge_req raw = *req;
        raw.qpn = TokenResolve(ivars, req->qpn, MLX_T_QP);
        if (!ivars->fFastBundle || !req->numSge || req->numSge > MLX_UC_MAX_SGE ||
            !raw.qpn || !Owns(ivars->fOwnedQp, raw.qpn))
            return kIOReturnNotPermitted;
        for (uint32_t i = 0; i < req->numSge; i++) {
            MlxMRContext *mr = ivars->fRoce->GetMR()->LookupByLkey(req->sge[i].lkey);
            if (!mr || !Owns(ivars->fOwnedMr, mr->mrHandle))
                return kIOReturnNotPermitted;
        }
        return ivars->fRoce->GetQP()->SyncSendSge(&raw);
    }
    case kMlxUCMethodSyncRecvSge: {
        const struct mlx_sync_recv_sge_req *req =
            (const struct mlx_sync_recv_sge_req *)in;
        struct mlx_sync_recv_sge_req raw = *req;
        raw.qpn = TokenResolve(ivars, req->qpn, MLX_T_QP);
        if (!ivars->fFastBundle || !req->numSge || req->numSge > MLX_UC_MAX_SGE ||
            !raw.qpn || !Owns(ivars->fOwnedQp, raw.qpn))
            return kIOReturnNotPermitted;
        for (uint32_t i = 0; i < req->numSge; i++) {
            MlxMRContext *mr = ivars->fRoce->GetMR()->LookupByLkey(req->sge[i].lkey);
            if (!mr || !Owns(ivars->fOwnedMr, mr->mrHandle))
                return kIOReturnNotPermitted;
        }
        return ivars->fRoce->GetQP()->SyncRecvSge(&raw);
    }
    case kMlxUCMethodPostSendSge: {
        const struct mlx_post_send_sge_req *req =
            (const struct mlx_post_send_sge_req *)in;
        struct mlx_post_send_sge_req raw = *req;
        raw.qpn = TokenResolve(ivars, req->qpn, MLX_T_QP);
        if (!req->numSge || req->numSge > MLX_UC_MAX_SGE ||
            !raw.qpn || !Owns(ivars->fOwnedQp, raw.qpn))
            return kIOReturnBadArgument;
        for (uint32_t i = 0; i < req->numSge; i++) {
            MlxMRContext *mr = ivars->fRoce->GetMR()->LookupByLkey(req->sge[i].lkey);
            if (!mr || !Owns(ivars->fOwnedMr, mr->mrHandle))
                return kIOReturnNotPermitted;
        }
        return ivars->fRoce->GetQP()->PostSendSge(&raw);
    }
    case kMlxUCMethodPostSendInline: {
        const struct mlx_post_send_inline_req *req =
            (const struct mlx_post_send_inline_req *)in;
        if (!req->inlineLen || req->inlineLen > MLX_UC_MAX_INLINE_DATA ||
            (req->opcode != MLX_UC_WR_SEND && req->opcode != MLX_UC_WR_SEND_IMM) ||
            (req->sendFlags & ~(MLX_UC_SEND_SIGNALED | MLX_UC_SEND_FENCE |
                                MLX_UC_SEND_SOLICITED | MLX_UC_SEND_INLINE)))
            return kIOReturnBadArgument;
        struct mlx_post_send_inline_req raw = *req;
        raw.qpn = TokenResolve(ivars, req->qpn, MLX_T_QP);
        if (!raw.qpn || !Owns(ivars->fOwnedQp, raw.qpn))
            return kIOReturnNotPermitted;
        return ivars->fRoce->PostSendInline(&raw);
    }
    case kMlxUCMethodPostSendAtomic: {
        const struct mlx_post_send_atomic_req *req =
            (const struct mlx_post_send_atomic_req *)in;
        if ((req->opcode != MLX_UC_WR_ATOMIC_CS &&
             req->opcode != MLX_UC_WR_ATOMIC_FA) ||
            (req->sendFlags & ~MLX_UC_SEND_SIGNALED) ||
            !req->remoteAddr || !req->rkey || (req->remoteAddr & 7))
            return kIOReturnBadArgument;
        struct mlx_post_send_atomic_req raw = *req;
        raw.qpn = TokenResolve(ivars, req->qpn, MLX_T_QP);
        if (!raw.qpn || !Owns(ivars->fOwnedQp, raw.qpn))
            return kIOReturnNotPermitted;
        return ivars->fRoce->PostSendAtomic(&raw);
    }
    case kMlxUCMethodPostLocalInv: {
        const struct mlx_post_local_inv_req *req =
            (const struct mlx_post_local_inv_req *)in;
        struct mlx_post_local_inv_req raw = *req;
        raw.qpn = TokenResolve(ivars, req->qpn, MLX_T_QP);
        MlxMRContext *mr = ivars->fRoce->GetMR()->LookupByRkey(req->invalidateRkey);
        if (!raw.qpn || !Owns(ivars->fOwnedQp, raw.qpn) || !mr ||
            !Owns(ivars->fOwnedMr, mr->mrHandle)) return kIOReturnNotPermitted;
        kern_return_t r = ivars->fRoce->GetQP()->PostLocalInv(&raw);
        if (r == kIOReturnSuccess)
            r = ivars->fRoce->GetMR()->InvalidateKey(req->invalidateRkey);
        return r;
    }
    case kMlxUCMethodPostRecvSge: {
        const struct mlx_post_recv_sge_req *req =
            (const struct mlx_post_recv_sge_req *)in;
        struct mlx_post_recv_sge_req raw = *req;
        raw.qpn = TokenResolve(ivars, req->qpn, MLX_T_QP);
        if (!req->numSge || req->numSge > MLX_UC_MAX_SGE ||
            !raw.qpn || !Owns(ivars->fOwnedQp, raw.qpn))
            return kIOReturnBadArgument;
        for (uint32_t i = 0; i < req->numSge; i++) {
            MlxMRContext *mr = ivars->fRoce->GetMR()->LookupByLkey(req->sge[i].lkey);
            if (!mr || !Owns(ivars->fOwnedMr, mr->mrHandle))
                return kIOReturnNotPermitted;
        }
        return ivars->fRoce->GetQP()->PostRecvSge(&raw);
    }
    case kMlxUCMethodPostUmrKlm: {
        const struct mlx_post_umr_klm_req *req =
            (const struct mlx_post_umr_klm_req *)in;
        if (!req->childCount ||
            req->childCount > MLX_UC_MAX_INDIRECT_CHILDREN)
            return kIOReturnNotPermitted;
        struct mlx_post_umr_klm_req raw = *req;
        raw.qpn = TokenResolve(ivars, req->qpn, MLX_T_QP);
        raw.mrHandle = TokenResolve(ivars, req->mrHandle, MLX_T_MR);
        if (!raw.qpn || !raw.mrHandle || !Owns(ivars->fOwnedQp, raw.qpn) ||
            !Owns(ivars->fOwnedMr, raw.mrHandle))
            return kIOReturnNotPermitted;
        for (uint32_t i = 0; i < req->childCount; i++) {
            raw.childHandles[i] = TokenResolve(ivars, req->childHandles[i], MLX_T_MR);
            if (!raw.childHandles[i] || !Owns(ivars->fOwnedMr, raw.childHandles[i]))
                return kIOReturnNotPermitted;
        }
        return ivars->fRoce->GetQP()->PostUmrKlm(
            raw.qpn, raw.mrHandle, raw.childHandles, raw.childCount,
            raw.wrId);
    }
    case kMlxUCMethodPostRecvBatch: {
        const struct mlx_post_recv_batch_req *batch =
            (const struct mlx_post_recv_batch_req *)in;
        if (!batch->count || batch->count > MLX_UC_MAX_POST_BATCH)
            return kIOReturnBadArgument;
        struct mlx_post_recv_batch_req raw = *batch;
        uint32_t qpn = TokenResolve(ivars, batch->wr[0].qpn, MLX_T_QP);
        if (!qpn || !Owns(ivars->fOwnedQp, qpn))
            return kIOReturnNotPermitted;
        for (uint32_t i = 0; i < batch->count; i++) {
            if (batch->wr[i].qpn != batch->wr[0].qpn)
                return kIOReturnBadArgument;
            raw.wr[i].qpn = qpn;
            MlxMRContext *mr =
                ivars->fRoce->GetMR()->LookupByLkey(batch->wr[i].sge.lkey);
            if (!mr || !Owns(ivars->fOwnedMr, mr->mrHandle))
                return kIOReturnNotPermitted;
        }
        return ivars->fRoce->GetQP()->PostRecvBatch(raw.wr, batch->count);
    }
    case kMlxUCMethodEnableFastPath: {
        if ((!ivars->fOwnedPd || !ivars->fOwnedPd->getCount()) || ivars->fFastBundle ||
            (ivars->fOwnedQp && ivars->fOwnedQp->getCount()) ||
            (ivars->fOwnedCq && ivars->fOwnedCq->getCount()))
            return kIOReturnBusy;
        MlxClientDoorbellBundle *bundle =
            IONewZero(MlxClientDoorbellBundle, 1);
        if (!bundle) return kIOReturnNoMemory;
        kern_return_t fastKr = ivars->fCore->GetUAR()->AllocClientBundle(bundle);
        if (fastKr != kIOReturnSuccess) {
            IODelete(bundle, MlxClientDoorbellBundle, 1);
            return fastKr;
        }
        ivars->fFastBundle = bundle;
        struct mlx_fast_path_resp *resp = (struct mlx_fast_path_resp *)out;
        resp->version = MLX_FAST_PATH_ABI_VERSION;
        resp->uarPageSize = 4096;
        resp->dbPageSize = 4096;
        resp->maxBatch = MLX_UC_MAX_POST_BATCH;
        return kIOReturnSuccess;
    }
    default:
        return kIOReturnUnsupported;
    }
}

/* ---- debug interface dispatch (mlx_probe, notes/35) ---- */

kern_return_t
MlxUserClient::DispatchDebugMethod(uint64_t selector,
                                    IOUserClientMethodArguments *args)
{
    if (!ivars->fCore) return kIOReturnNotAttached;

    const void *in = NULL;
    void *out = NULL;
    if (args && args->structureInput)
        in = args->structureInput->getBytesNoCopy();
    if (args && args->structureOutput)
        out = const_cast<void *>(args->structureOutput->getBytesNoCopy());

    switch (selector) {
    case kMlxUCMethodDbgFlr:
        if (!FwCmdAllowed(ivars)) return kIOReturnNoResources;
        return ivars->fCore->DbgPerformFlr();

    case kMlxUCMethodDbgExec: {
        if (!in || !out) return kIOReturnBadArgument;
        if (!FwCmdAllowed(ivars)) return kIOReturnNoResources;
        const struct mlx_dbg_exec_req *req = (const struct mlx_dbg_exec_req *)in;
        struct mlx_dbg_exec_resp *resp = (struct mlx_dbg_exec_resp *)out;
        if (req->inSize > sizeof(req->in)) return kIOReturnBadArgument;
        uint32_t outSize = req->outSize;
        if (outSize > sizeof(resp->out)) outSize = sizeof(resp->out);
        kern_return_t kr = ivars->fCore->Exec(req->opcode, req->in, req->inSize,
                                               resp->out, outSize, req->timeoutMs);
        resp->kr = kr;
        resp->outSize = (kr == kIOReturnSuccess) ? outSize : 0;
        return kIOReturnSuccess;
    }

    case kMlxUCMethodDbgQueryPages: {
        if (!in || !out) return kIOReturnBadArgument;
        if (!FwCmdAllowed(ivars)) return kIOReturnNoResources;
        const struct mlx_dbg_query_pages_req *req = (const struct mlx_dbg_query_pages_req *)in;
        struct mlx_dbg_query_pages_resp *resp = (struct mlx_dbg_query_pages_resp *)out;
        if (!ivars->fCore->GetPages()) return kIOReturnNotReady;
        uint32_t np = 0, fid = 0;
        kern_return_t kr = ivars->fCore->GetPages()->QueryStartupPagesFull(
            (uint16_t)req->mode, &np, &fid);
        resp->numPages = np;
        resp->functionId = fid;
        resp->kr = kr;
        return kIOReturnSuccess;
    }

    case kMlxUCMethodDbgProvidePages: {
        if (!in || !out) return kIOReturnBadArgument;
        if (!FwCmdAllowed(ivars)) return kIOReturnNoResources;
        const struct mlx_dbg_provide_pages_req *req = (const struct mlx_dbg_provide_pages_req *)in;
        struct mlx_dbg_provide_pages_resp *resp = (struct mlx_dbg_provide_pages_resp *)out;
        if (!ivars->fCore->GetPages()) return kIOReturnNotReady;
        uint32_t np = req->numPages;
        if (np > 16) np = 16;
        kern_return_t kr;
        if (req->mode == 1) {
            kr = ivars->fCore->GetPages()->ProvidePagesContig(np, (uint8_t)req->ownership);
        } else {
            kr = ivars->fCore->GetPages()->ProvidePages(np, (uint8_t)req->ownership);
        }
        resp->given = np;
        resp->kr = kr;
        /* Copy IOVAs for debugging. */
        for (uint32_t i = 0; i < np && i < 16; i++)
            resp->iova[i] = ivars->fCore->GetPages()->GetPageIOVA(i);
        return kIOReturnSuccess;
    }

    case kMlxUCMethodDbgDumpState: {
        if (!out) return kIOReturnBadArgument;
        struct mlx_dbg_state_resp *resp = (struct mlx_dbg_state_resp *)out;
        memset(resp, 0, sizeof(*resp));

        /* fw_rev and initializing from init-seg. */
        IOPCIDevice *pci = ivars->fCore->GetPCI();
        uint8_t bi = ivars->fCore->GetBar0Index();
        if (pci) {
            resp->fwRev = mlxMMIORead32BE(pci, bi, 0);
            resp->initializing = mlxMMIORead32BE(pci, bi,
                offsetof(struct MlxInitSeg, initializing));
        }

        /* cmdq state. */
        MlxCmd *cmd = ivars->fCore->GetCmd();
        if (cmd) {
            resp->cmdifRev = cmd->CmdifRev();
            resp->cmdqIOVA = cmd->CmdqIOVA();
            resp->cmdqLogSzStride = ((uint32_t)cmd->LogSz() << 4) | cmd->LogStride();
        }

        /* ISSI / HCA state. */
        resp->issi = ivars->fCore->GetIssi();
        resp->hcaEnabled = 1;  /* stub: core doesn't expose fHcaEnabled */

        /* Pages. */
        MlxFwPages *pages = ivars->fCore->GetPages();
        if (pages) {
            resp->pagesInUse = pages->GetPageCount();
            resp->chunkMode = pages->IsChunkMode() ? 1 : 0;
            resp->chunkIOVA = pages->GetChunkIOVA();
            for (uint32_t i = 0; i < 8; i++)
                resp->pageIOVA[i] = pages->GetPageIOVA(i);
        }
        return kIOReturnSuccess;
    }

    default:
        return kIOReturnUnsupported;
    }
}

kern_return_t
MlxUserClient::CopyClientMemoryForType_Impl(uint64_t type, uint64_t *options,
                                             IOMemoryDescriptor **memory)
{
    if (!memory || !ivars)
        return kIOReturnNotReady;
    MlxMethodLockGuard methodGuard(ivars->fMethodLock);
    if (!ivars->fFastBundle)
        return kIOReturnNotReady;
    if (options) *options = 0;

    /* Only the caller's own UAR and DB page are exportable. Queue and CQ
     * memory stays DEXT-owned until a separate mapping ABI exists. */
    uint32_t kind = MLX_UC_MEM_KIND(type);
    uint32_t handle = MLX_UC_MEM_HANDLE(type);

    IOMemoryDescriptor *desc = NULL;
    if (kind == kMlxUCMemKindUar) {
        if (handle != 0) return kIOReturnNotPermitted;
        desc = ivars->fFastBundle->uarMemory;
    } else if (kind == kMlxUCMemKindDbRecord) {
        if (handle != 0) return kIOReturnNotPermitted;
        desc = ivars->fFastBundle->dbMemory;
    } else if (kind == kMlxUCMemKindCqe) {
        uint32_t raw = TokenResolve(ivars, handle, MLX_T_CQ);
        if (!raw || !Owns(ivars->fOwnedCq, raw))
            return kIOReturnNotPermitted;
        MlxCQContext *cq = ivars->fRoce && ivars->fRoce->GetCQ()
            ? ivars->fRoce->GetCQ()->Lookup(raw) : NULL;
        if (!cq || cq->clientBundle != ivars->fFastBundle)
            return kIOReturnNotPermitted;
        desc = ivars->fRoce->GetCQ()->GetCqMemDesc(raw);
    } else if (kind == kMlxUCMemKindSq || kind == kMlxUCMemKindRq) {
        uint32_t raw = TokenResolve(ivars, handle, MLX_T_QP);
        if (!raw || !Owns(ivars->fOwnedQp, raw))
            return kIOReturnNotPermitted;
        MlxQPContext *qp = ivars->fRoce && ivars->fRoce->GetQP()
            ? ivars->fRoce->GetQP()->Lookup(raw) : NULL;
        if (!qp || qp->clientBundle != ivars->fFastBundle)
            return kIOReturnNotPermitted;
        desc = kind == kMlxUCMemKindSq
            ? ivars->fRoce->GetQP()->GetSqMemDesc(raw)
            : ivars->fRoce->GetQP()->GetRqMemDesc(raw);
    } else {
        return kIOReturnUnsupported;
    }

    if (!desc) return kIOReturnNotReady;
    desc->retain();
    *memory = desc;
    return kIOReturnSuccess;
}
