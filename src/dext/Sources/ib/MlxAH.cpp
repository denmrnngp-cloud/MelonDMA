/*
 * MlxAH.cpp — Address Handle (DriverKit port).
 *
 * Ported from: drivers/infiniband/hw/mlx5/ah.c. Pure encoding logic: the AV
 * encoder is host-testable and portable. Only the storage is adapted to
 * DriverKit (fixed-capacity array, no OSObject).
 */
#include "MlxAH.hpp"
#include "MlxRoCE.hpp"
#include "MlxPCIDriver.h"
#include "MlxCmd.hpp"
#include "MlxWQE.hpp"
#include "MlxUCIO.h"

#include <DriverKit/IOLib.h>
#include <string.h>

#include "MlxLog.hpp"
#define MLX_LOG(fmt, ...)  IOLog("MlxAH: " fmt "\n", ##__VA_ARGS__)
#define MLX_DBG(fmt, ...)  MLX_DBGLOG("MlxAH: " fmt, ##__VA_ARGS__)
#define MLX_AH_TABLE_CAP   16

struct MlxAH::State {
    MlxRoCE      *roce;
    struct IOLock *lock;
    MlxAHContext  table[MLX_AH_TABLE_CAP];
    bool          used[MLX_AH_TABLE_CAP];
    uint32_t      nextHandle;
};

MlxAH::MlxAH() : s(NULL) {}
MlxAH::~MlxAH() { Free(); }

kern_return_t
MlxAH::Init(MlxRoCE *roce)
{
    if (!roce) return kIOReturnBadArgument;
    s = new State;
    if (!s) return kIOReturnNoMemory;
    memset(s, 0, sizeof(*s));
    s->roce = roce;
    s->lock = IOLockAlloc();
    if (!s->lock) { delete s; s = NULL; return kIOReturnNoMemory; }
    s->nextHandle = 1;
    return kIOReturnSuccess;
}

void
MlxAH::Free()
{
    if (!s) return;
    if (s->lock) { IOLockFree(s->lock); s->lock = NULL; }
    delete s; s = NULL;
}

void
MlxAH::EncodeAV(const struct mlx_create_ah_req *req, MlxAV *av)
{
    /* RoCEv2 address vector (ah.c:59-95, qp.h:327 mlx5_av). */
    memset(av, 0, sizeof(*av));
    av->stat_rate_sl = 0;
    av->udp_sport = req->udpSport;
    memcpy(av->rmac, req->dmac, 6);
    av->tclass = req->trafficClass;
    av->hop_limit = req->hopLimit;
    /* grh_gid_fl: GRH present (bit 30) + sgid_index (bits 29:20). */
    av->grh_gid_fl = (uint32_t)((1u << 30) | (req->sgidIndex << 20));
    memcpy(av->rgid, req->dgid, 16);
}

kern_return_t
MlxAH::CreateAH(const struct mlx_create_ah_req *req, struct mlx_create_ah_resp *resp)
{
    if (!s || !req || !resp) return kIOReturnBadArgument;
    IOLockLock(s->lock);
    int slot = -1;
    for (int i = 0; i < MLX_AH_TABLE_CAP; i++)
        if (!s->used[i]) { slot = i; break; }
    if (slot < 0) { IOLockUnlock(s->lock); return kIOReturnNoMemory; }
    MlxAHContext *ctx = &s->table[slot];
    memset(ctx, 0, sizeof(*ctx));
    ctx->ahHandle = s->nextHandle++;
    ctx->portNum = req->portNum;
    ctx->isRoCE = (req->ahType == 0);
    EncodeAV(req, &ctx->av);
    s->used[slot] = true;
    resp->ahHandle = ctx->ahHandle;
    IOLockUnlock(s->lock);
    MLX_LOG("AH[%u] created", ctx->ahHandle);
    return kIOReturnSuccess;
}

kern_return_t
MlxAH::DestroyAH(uint32_t ahHandle)
{
    if (!s) return kIOReturnBadArgument;
    IOLockLock(s->lock);
    for (int i = 0; i < MLX_AH_TABLE_CAP; i++) {
        if (s->used[i] && s->table[i].ahHandle == ahHandle) {
            s->used[i] = false;
            memset(&s->table[i], 0, sizeof(s->table[i]));
            IOLockUnlock(s->lock);
            MLX_LOG("AH[%u] destroyed", ahHandle);
            return kIOReturnSuccess;
        }
    }
    IOLockUnlock(s->lock);
    return kIOReturnNotFound;
}

MlxAHContext *
MlxAH::Lookup(uint32_t ahHandle)
{
    for (int i = 0; i < MLX_AH_TABLE_CAP; i++)
        if (s->used[i] && s->table[i].ahHandle == ahHandle) return &s->table[i];
    return NULL;
}