/*
 * test_abi_properties.c — P2.2 host-side ABI property tests (no hardware).
 *
 * Verifies the stable POD ABI boundary independent of the C++ static_asserts
 * (which only run in DEXT/C++ translation units): struct sizes, feature-bit
 * uniqueness, selector uniqueness, and the DoS bounds the DEXT enforces.
 *
 * The live fuzz gate (tools/mlx_abi_fuzz_gate.c) exercises the *behavioral*
 * side — malformed sizes, stale/forged handles, overflow, bad batches/SGEs —
 * against the activated DEXT. This file is the always-runnable contract check.
 */
#include "MlxUCIO.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int failures = 0;
static void check(int cond, const char *msg)
{
    if (!cond) { failures++; printf("FAIL: %s\n", msg); }
    else        printf("ok:   %s\n", msg);
}

/* Selector list must stay collision-free. Every kMlxUCMethod* value is a
 * distinct uint64_t used as the ExternalMethod dispatch key. */
static const uint32_t sSelectors[] = {
    kMlxUCMethodOpen, kMlxUCMethodClose, kMlxUCMethodQueryDevice,
    kMlxUCMethodQueryPort, kMlxUCMethodQueryAbi, kMlxUCMethodQueryLimits,
    kMlxUCMethodQueryStats, kMlxUCMethodAllocPD, kMlxUCMethodDeallocPD,
    kMlxUCMethodAllocUAR, kMlxUCMethodCreateQP, kMlxUCMethodModifyQP,
    kMlxUCMethodDestroyQP, kMlxUCMethodQueryQP, kMlxUCMethodCreateCQ,
    kMlxUCMethodDestroyCQ, kMlxUCMethodRegMR, kMlxUCMethodDeregMR,
    kMlxUCMethodRegMRIndirect, kMlxUCMethodCreateAH, kMlxUCMethodDestroyAH,
    kMlxUCMethodGetGidIndex, kMlxUCMethodSetGid, kMlxUCMethodDelGid,
    kMlxUCMethodQueryGid, kMlxUCMethodCCQuery, kMlxUCMethodCCModify,
    kMlxUCMethodAccessReg, kMlxUCMethodFwCmd, kMlxUCMethodQueryPages,
    kMlxUCMethodPortStats, kMlxUCMethodFwReset, kMlxUCMethodQueryFwVer,
    kMlxUCMethodQueryHealth, kMlxUCMethodVirtToPhys, kMlxUCMethodGetCqBuffer,
    kMlxUCMethodQueryCqCompletions, kMlxUCMethodGetAsyncEvent,
    kMlxUCMethodUpdateCqConsumer, kMlxUCMethodPollCQ, kMlxUCMethodPostSend,
    kMlxUCMethodPostRecv, kMlxUCMethodPostSendBatch, kMlxUCMethodPostRecvBatch,
    kMlxUCMethodEnableFastPath, kMlxUCMethodSyncFastPath,
    kMlxUCMethodSyncRecvFastPath, kMlxUCMethodSyncSendSge,
    kMlxUCMethodSyncRecvSge, kMlxUCMethodPostSendSge, kMlxUCMethodPostRecvSge,
    kMlxUCMethodPostLocalInv, kMlxUCMethodPostUmrKlm, kMlxUCMethodAllocMW,
    kMlxUCMethodDeallocMW, kMlxUCMethodBindMW, kMlxUCMethodFwReinit,
    kMlxUCMethodDbgFlr, kMlxUCMethodDbgExec, kMlxUCMethodDbgQueryPages,
    kMlxUCMethodDbgProvidePages, kMlxUCMethodDbgDumpState,
    kMlxUCMethodStableInitCycle,
};
#define SELECTOR_COUNT (sizeof(sSelectors) / sizeof(sSelectors[0]))

static void test_struct_sizes(void)
{
    check(sizeof(struct mlx_query_abi_resp) == 8, "mlx_query_abi_resp == 8");
    check(sizeof(struct mlx_health_resp) == 32, "mlx_health_resp == 32");
    check(sizeof(struct mlx_query_limits_resp) == 48, "mlx_query_limits_resp == 48");
    check(sizeof(struct mlx_stats_resp) == 152, "mlx_stats_resp == 152");
    check(sizeof(struct mlx_create_cq_req) == 4, "mlx_create_cq_req == 4");
    check(sizeof(struct mlx_create_cq_resp) == 16, "mlx_create_cq_resp == 16");
    check(sizeof(struct mlx_datapath_sge) == 16, "mlx_datapath_sge == 16");
    check(sizeof(struct mlx_post_send_req) == 48, "mlx_post_send_req == 48");
    check(sizeof(struct mlx_post_recv_req) == 32, "mlx_post_recv_req == 32");
    check(sizeof(struct mlx_post_send_atomic_req) == 64, "mlx_post_send_atomic_req == 64");
    check(sizeof(struct mlx_post_send_sge_req) == 296, "mlx_post_send_sge_req == 296");
    check(sizeof(struct mlx_post_recv_sge_req) == 272, "mlx_post_recv_sge_req == 272");
    check(sizeof(struct mlx_post_send_batch_req) == 3080, "mlx_post_send_batch_req == 3080");
    check(sizeof(struct mlx_post_recv_batch_req) == 2056, "mlx_post_recv_batch_req == 2056");
    check(sizeof(struct mlx_work_completion) == 48, "mlx_work_completion == 48");
    check(sizeof(struct mlx_sync_fast_path_req) == 3080, "mlx_sync_fast_path_req == 3080");
    check(sizeof(struct mlx_sync_recv_fast_path_req) == 2056, "mlx_sync_recv_fast_path_req == 2056");
    check(sizeof(struct mlx_sync_send_sge_req) == 296, "mlx_sync_send_sge_req == 296");
    check(sizeof(struct mlx_sync_recv_sge_req) == 280, "mlx_sync_recv_sge_req == 280");
    check(sizeof(struct mlx_post_umr_klm_req) == 152, "mlx_post_umr_klm_req == 152");
    check(sizeof(struct mlx_stable_init_cycle_resp) == 136, "mlx_stable_init_cycle_resp == 136");
}

static void test_feature_bits(void)
{
    uint32_t bits[] = {
        MLX_UC_FEATURE_RC, MLX_UC_FEATURE_ROCE_V2, MLX_UC_FEATURE_DIRECT_PATH,
        MLX_UC_FEATURE_ASYNC_EVENTS, MLX_UC_FEATURE_INDIRECT_MR,
        MLX_UC_FEATURE_QP_RECOVERY, MLX_UC_FEATURE_MULTI_SGE,
        MLX_UC_FEATURE_IMMEDIATE_DATA, MLX_UC_FEATURE_HEALTH_QUERY,
        MLX_UC_FEATURE_STATS,
    };
    uint32_t seen = 0;
    int unique = 1;
    for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
        if (!bits[i] || (bits[i] & (bits[i] - 1)) || (seen & bits[i])) {
            unique = 0;
            break;
        }
        seen |= bits[i];
    }
    check(unique, "feature bits are distinct powers of two");
}

static void test_selector_uniqueness(void)
{
    int unique = 1;
    for (size_t i = 0; i < SELECTOR_COUNT && unique; i++)
        for (size_t j = i + 1; j < SELECTOR_COUNT; j++)
            if (sSelectors[i] == sSelectors[j]) { unique = 0; break; }
    check(unique, "selector values are unique");
}

static void test_bounds(void)
{
    check(MLX_UC_MAX_POST_BATCH >= 1 && MLX_UC_MAX_POST_BATCH <= 256,
          "post batch bound sane");
    check(MLX_UC_MAX_SGE >= 1 && MLX_UC_MAX_SGE <= 16, "SGE bound sane");
    check(MLX_UC_MAX_SQ_DEPTH == 4096 && MLX_UC_MAX_RQ_DEPTH == 4096,
          "SQ/RQ depth ceilings are 4096");
    check(MLX_UC_MAX_INDIRECT_CHILDREN >= 1 &&
          MLX_UC_MAX_INDIRECT_CHILDREN <= 64,
          "indirect-child bound sane");
}

int main(void)
{
    test_struct_sizes();
    test_feature_bits();
    test_selector_uniqueness();
    test_bounds();
    if (failures) {
        printf("ABI_PROPERTIES FAIL (%d)\n", failures);
        return 1;
    }
    printf("ABI_PROPERTIES PASS: struct sizes, feature bits, selector uniqueness, bounds\n");
    return 0;
}
