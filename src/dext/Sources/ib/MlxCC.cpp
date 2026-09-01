/*
 * MlxCC.cpp — DCQCN congestion control (DriverKit port).
 *
 * Ported from: drivers/infiniband/hw/mlx5/cong.c. The DCQCN loop runs in
 * firmware; the driver wraps QUERY_CONG_PARAMS (0x824) / MODIFY_CONG_PARAMS
 * (0x825) and stores the reaction-point parameters.
 *
 * Register layout is taken from donors/rxe-reference/mlx5_ifc.h:
 *   modify_cong_params_in:  opcode@0x0, op_mod@0x30, cong_protocol@0x5c,
 *                           field_select@0x60, congestion_parameters@0x100
 *   query_cong_params_out:  status@0x0, syndrome@0x20, congestion_parameters@0x80
 *   cong_control_r_roce_ecn_rp (reaction point) fields, relative to the
 *   congestion_parameters union: rpg_time_reset@0xa0, rpg_byte_reset@0xc0,
 *   rpg_threshold@0xe0, rpg_max_rate@0x100, rpg_ai_rate@0x120,
 *   rpg_hai_rate@0x140, rpg_gd@0x160, rpg_min_dec_fac@0x180, rpg_min_rate@0x1a0.
 */
#include "MlxCC.hpp"
#include "MlxRoCE.hpp"
#include "MlxPCIDriver.h"
#include "MlxCmd.hpp"
#include "MlxRegs.hpp"
#include "MlxUCIO.h"

#include <DriverKit/IOLib.h>
#include <string.h>

#include "MlxIfcHelpers.hpp"   /* mlxSetBits / mlxGetBits */
#include "MlxLog.hpp"
#define MLX_LOG(fmt, ...)  IOLog("MlxCC: " fmt "\n", ##__VA_ARGS__)
#define MLX_DBG(fmt, ...)  MLX_DBGLOG("MlxCC: " fmt, ##__VA_ARGS__)

/* mlx5_ifc bit offsets (donors/rxe-reference/mlx5_ifc.h). */
enum {
    MLX_CC_PROTOCOL_ROCE_ECN_RP = 0x1,   /* RoCEv2 ECN reaction point (DCQCN) */
    MLX_CC_QUERY_CTX_BIT        = 0x80,  /* query_cong_params_out.congestion_parameters */
    MLX_CC_MODIFY_CTX_BIT       = 0x100, /* modify_cong_params_in.congestion_parameters */
    /* cong_control_r_roce_ecn_rp fields, relative to the ctx union. */
    MLX_CC_RP_TIME_RESET   = 0xa0,
    MLX_CC_RP_BYTE_RESET   = 0xc0,
    MLX_CC_RP_THRESHOLD    = 0xe0,
    MLX_CC_RP_MAX_RATE     = 0x100,
    MLX_CC_RP_AI_RATE      = 0x120,
    MLX_CC_RP_HAI_RATE     = 0x140,
    MLX_CC_RP_GD           = 0x160,
    MLX_CC_RP_MIN_DEC_FAC  = 0x180,
    MLX_CC_RP_MIN_RATE     = 0x1a0,
    /* field_select bits (r_roce_rp variant). */
    MLX_CC_SEL_TIME_RESET   = 0x8,
    MLX_CC_SEL_BYTE_RESET   = 0x10,
    MLX_CC_SEL_THRESHOLD    = 0x20,
    MLX_CC_SEL_MAX_RATE     = 0x40,
    MLX_CC_SEL_AI_RATE      = 0x80,
    MLX_CC_SEL_HAI_RATE     = 0x100,
    MLX_CC_SEL_GD           = 0x200,
    MLX_CC_SEL_MIN_DEC_FAC  = 0x400,
    MLX_CC_SEL_MIN_RATE     = 0x800,
};

struct MlxCC::State {
    MlxRoCE      *roce;
    MlxPCIDriver *core;
    struct IOLock *lock;
    bool           enabled;
    struct mlx_cc_params params;
};

MlxCC::MlxCC() : s(NULL) {}
MlxCC::~MlxCC() { Free(); }

kern_return_t
MlxCC::Init(MlxRoCE *roce, MlxPCIDriver *core)
{
    if (!roce || !core) return kIOReturnBadArgument;
    s = new State;
    if (!s) return kIOReturnNoMemory;
    memset(s, 0, sizeof(*s));
    s->roce = roce;
    s->core = core;
    s->lock = IOLockAlloc();
    if (!s->lock) { delete s; s = NULL; return kIOReturnNoMemory; }
    /* DCQCN defaults (AppleMCX README): rpg_min_dec_fac=256, rpg_ai_rate=5,
     * rpg_time_reset=55, rpg_threshold=150. */
    s->params.rpgMinDecFac = 256;
    s->params.rpgAiRate = 5;
    s->params.rpgTimeReset = 55;
    s->params.rpgThreshold = 150;
    s->enabled = false;   /* enabled after a successful firmware query/modify */
    return kIOReturnSuccess;
}

void
MlxCC::Free()
{
    if (!s) return;
    if (s->lock) { IOLockFree(s->lock); s->lock = NULL; }
    delete s; s = NULL;
}

bool
MlxCC::IsEnabled() const
{
    return s ? s->enabled : false;
}

kern_return_t
MlxCC::CmdQuery(uint32_t regId, void *out)
{
    (void)regId;
    if (!s || !out) return kIOReturnBadArgument;
    struct mlx_cc_params *p = (struct mlx_cc_params *)out;
    uint8_t in[16] = {};
    uint8_t qout[512] = {};
    mlxSetBits(in, 0x00, 16, MLX_CMD_OP_QUERY_CONG_PARAMS);
    mlxSetBits(in, 0x5c, 4, MLX_CC_PROTOCOL_ROCE_ECN_RP);
    kern_return_t kr = s->core->Exec(MLX_CMD_OP_QUERY_CONG_PARAMS, in,
                                     sizeof(in), qout, sizeof(qout), 5000);
    if (kr != kIOReturnSuccess) {
        MLX_LOG("QUERY_CONG_PARAMS failed: 0x%x", kr);
        return kr;
    }
    uint32_t base = MLX_CC_QUERY_CTX_BIT;
    p->rpgTimeReset = (uint32_t)mlxGetBits(qout, base + MLX_CC_RP_TIME_RESET, 32);
    p->rpgThreshold = (uint32_t)mlxGetBits(qout, base + MLX_CC_RP_THRESHOLD, 32);
    p->rpgAiRate    = (uint32_t)mlxGetBits(qout, base + MLX_CC_RP_AI_RATE, 32);
    p->rpgHai       = (uint32_t)mlxGetBits(qout, base + MLX_CC_RP_HAI_RATE, 32);
    p->rpgGd        = (uint32_t)mlxGetBits(qout, base + MLX_CC_RP_GD, 32);
    p->rpgMinDecFac = (uint32_t)mlxGetBits(qout, base + MLX_CC_RP_MIN_DEC_FAC, 32);
    p->rpgTimeInc   = 0;   /* no reaction-point register field */
    return kIOReturnSuccess;
}

kern_return_t
MlxCC::CmdModify(uint32_t regId, const void *in)
{
    (void)regId;
    if (!s || !in) return kIOReturnBadArgument;
    const struct mlx_cc_params *p = (const struct mlx_cc_params *)in;
    uint8_t mbin[512] = {};
    uint8_t out[16] = {};
    mlxSetBits(mbin, 0x00, 16, MLX_CMD_OP_MODIFY_CONG_PARAMS);
    mlxSetBits(mbin, 0x5c, 4, MLX_CC_PROTOCOL_ROCE_ECN_RP);
    /* Only touch the fields present in mlx_cc_params; rpg_byte_reset,
     * rpg_max_rate and rpg_min_rate are left at their firmware values. */
    uint32_t fieldSelect = MLX_CC_SEL_TIME_RESET | MLX_CC_SEL_THRESHOLD |
                           MLX_CC_SEL_AI_RATE | MLX_CC_SEL_HAI_RATE |
                           MLX_CC_SEL_GD | MLX_CC_SEL_MIN_DEC_FAC;
    mlxSetBits(mbin, 0x60, 32, fieldSelect);
    uint32_t base = MLX_CC_MODIFY_CTX_BIT;
    mlxSetBits(mbin, base + MLX_CC_RP_TIME_RESET,  32, p->rpgTimeReset);
    mlxSetBits(mbin, base + MLX_CC_RP_THRESHOLD,   32, p->rpgThreshold);
    mlxSetBits(mbin, base + MLX_CC_RP_AI_RATE,     32, p->rpgAiRate);
    mlxSetBits(mbin, base + MLX_CC_RP_HAI_RATE,    32, p->rpgHai);
    mlxSetBits(mbin, base + MLX_CC_RP_GD,          32, p->rpgGd);
    mlxSetBits(mbin, base + MLX_CC_RP_MIN_DEC_FAC, 32, p->rpgMinDecFac);
    kern_return_t kr = s->core->Exec(MLX_CMD_OP_MODIFY_CONG_PARAMS, mbin,
                                     sizeof(mbin), out, sizeof(out), 5000);
    if (kr != kIOReturnSuccess)
        MLX_LOG("MODIFY_CONG_PARAMS failed: 0x%x", kr);
    return kr;
}

kern_return_t
MlxCC::QueryParams(struct mlx_cc_params *out)
{
    if (!s || !out) return kIOReturnBadArgument;
    struct mlx_cc_params fw = {};
    if (CmdQuery(0, &fw) == kIOReturnSuccess) {
        IOLockLock(s->lock);
        s->params = fw;
        s->enabled = true;
        IOLockUnlock(s->lock);
        MLX_DBG("query ok min_dec_fac=%u ai_rate=%u time_reset=%u threshold=%u",
                fw.rpgMinDecFac, fw.rpgAiRate, fw.rpgTimeReset, fw.rpgThreshold);
    }
    IOLockLock(s->lock);
    *out = s->params;
    IOLockUnlock(s->lock);
    return kIOReturnSuccess;
}

kern_return_t
MlxCC::ModifyParams(const struct mlx_cc_params *in)
{
    if (!s || !in) return kIOReturnBadArgument;
    kern_return_t kr = CmdModify(0, in);
    if (kr == kIOReturnSuccess) {
        IOLockLock(s->lock);
        s->params = *in;
        s->enabled = true;
        IOLockUnlock(s->lock);
        MLX_LOG("DCQCN params updated min_dec_fac=%u ai_rate=%u time_reset=%u threshold=%u",
                in->rpgMinDecFac, in->rpgAiRate, in->rpgTimeReset, in->rpgThreshold);
    }
    return kr;
}
