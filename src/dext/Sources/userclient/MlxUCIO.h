/*
 * MlxUCIO.h — userspace interface definitions (shared by the driver and libmlx)
 *
 * Purpose:
 *   1. Kernel side: MlxUserClient::externalMethod dispatch table
 *   2. Userspace: libmlx's IOConnectCallMethod selectors
 *
 * Note: this file is included by both the kernel and userspace, so only POD structs are
 *       allowed; it must not depend on any kernel- or userspace-specific headers.
 */
#ifndef MLX_UC_IO_H
#define MLX_UC_IO_H

#include <stdint.h>
#include <stddef.h>

/* class ID: used with IOServiceOpen */
#define MLX_USERCLIENT_CLASS   "MlxUserClient"

/* memory mapping index (IOConnectMapMemory) */
enum {
    kMlxUCMemKindUar        = 1,
    kMlxUCMemKindDbRecord   = 2,
    kMlxUCMemKindCqe        = 3,
    kMlxUCMemKindSq         = 4,
    kMlxUCMemKindRq         = 5,
};
#define MLX_UC_MEM_TYPE(kind, handle) \
    ((uint64_t)((kind) & 0xffu) | ((uint64_t)(handle) << 8))
#define MLX_UC_MEM_KIND(type) ((uint32_t)((type) & 0xffu))
#define MLX_UC_MEM_HANDLE(type) ((uint32_t)((type) >> 8))
#define MLX_FAST_PATH_ABI_VERSION 1u
/* ABI v2: resource handles (PD/CQ/QP/MR/MW) and completion qpNum are opaque
 * per-UserClient tokens with an embedded generation; raw firmware IDs never
 * cross this boundary. Clients must reject a mismatched major version. */
#define MLX_UC_ABI_VERSION 2u

/* Stable provider ABI: feature bits are additive. Clients must negotiate
 * before using optional paths and reject a newer incompatible major value. */
enum {
    MLX_UC_FEATURE_RC              = 1u << 0,
    MLX_UC_FEATURE_ROCE_V2         = 1u << 1,
    MLX_UC_FEATURE_DIRECT_PATH     = 1u << 2,
    MLX_UC_FEATURE_ASYNC_EVENTS    = 1u << 3,
    MLX_UC_FEATURE_INDIRECT_MR     = 1u << 4,
    MLX_UC_FEATURE_QP_RECOVERY     = 1u << 5,
    MLX_UC_FEATURE_MULTI_SGE       = 1u << 6,
    MLX_UC_FEATURE_IMMEDIATE_DATA  = 1u << 7,
    MLX_UC_FEATURE_HEALTH_QUERY    = 1u << 8,
    MLX_UC_FEATURE_STATS           = 1u << 9,
    MLX_UC_FEATURE_INLINE          = 1u << 10,
    MLX_UC_FEATURE_ATOMIC          = 1u << 11,
};

struct mlx_query_abi_resp {
    uint32_t version;
    uint32_t features;
};

/* Read-only per-client diagnostic snapshot; no firmware command payloads or
 * DMA addresses are exposed through this stable ABI. */
struct mlx_health_resp {
    uint32_t healthy;
    uint32_t syndrome;
    uint32_t extSyndrome;
    uint32_t ownedPd;
    uint32_t ownedQp;
    uint32_t ownedCq;
    uint32_t ownedMr;
    uint32_t ownedAh;
};

/* ===== P1.1 per-client quotas (DoS protection) =====
 * Enforced by MlxUserClient. Every refusal happens BEFORE a firmware command
 * is issued, so a rejected request never leaves a partially-created resource.
 * These are per-UserClient ceilings; the DEXT-wide hardware tables (MlxQP /
 * MlxCQ / MlxMR) remain the ultimate bound shared by all clients. */
#define MLX_UC_MAX_PD_PER_CLIENT   16u
#define MLX_UC_MAX_QP_PER_CLIENT   64u
#define MLX_UC_MAX_CQ_PER_CLIENT   64u
#define MLX_UC_MAX_MR_PER_CLIENT   128u
#define MLX_UC_MAX_MW_PER_CLIENT   128u
#define MLX_UC_MAX_AH_PER_CLIENT   8u
#define MLX_UC_MAX_GID_PER_CLIENT  16u
/* WQE ring depth per SQ/RQ (must remain a power of two). CreateQP rejects
 * anything larger before allocating any firmware resource. */
#define MLX_UC_MAX_SQ_DEPTH        4096u
#define MLX_UC_MAX_RQ_DEPTH        4096u
/* Raw firmware-command rate limit on the debug/passthrough selectors (the only
 * unbounded command entry point). Resource create/destroy already rate-bounded
 * by the quotas above. */
#define MLX_UC_FW_CMD_BURST        32u
#define MLX_UC_FW_CMD_WINDOW_NS    2000000u   /* 2 ms */

/* Read-only per-client limits; the same values are enforced, never larger. */
struct mlx_query_limits_resp {
    uint32_t maxPd;
    uint32_t maxQp;
    uint32_t maxCq;
    uint32_t maxMr;
    uint32_t maxMw;
    uint32_t maxAh;
    uint32_t maxGid;
    uint32_t maxSqDepth;
    uint32_t maxRqDepth;
    uint32_t fwCmdBurst;
    uint32_t fwCmdWindowNs;
    uint32_t maxDbRecords;     /* shared CQ+QP DB-record slots */
};

#if defined(__cplusplus)
static_assert(sizeof(struct mlx_query_limits_resp) == 48,
              "mlx_query_limits_resp ABI mismatch");
#endif

/* ===== P2.1 stable observability =====
 * Read-only per-client datapath counters. All values are accumulated across
 * every resource the calling client owns (never across clients). No DMA
 * address, firmware command payload, or raw firmware ID crosses this ABI.
 *
 * Counter semantics:
 *   postedSend/Read/Write/Umr/BindMw/LocalInv — WQEs accepted onto the SQ,
 *       tagged by opcode (SEND_IMM folds into Send, *_IMM folds into Write).
 *   postedRecv  — RECV WQEs accepted onto the RQ.
 *   completed*  — CQEs consumed through PollCQ, by the WQE's opcode.
 *       completedUmr covers both UMR/KLM activation and BIND_MW (the CQE
 *       does not distinguish them; the posted-side counters do).
 *   cqeError / cqeRetryExc / cqeRnrRetry — error-CQE classification.
 *   cqLost      — CQEs dropped because the hardware QPN could not be
 *       attributed (ambiguous shared CQ); overflow needs the EQ CQ-error
 *       decode path and is not yet wired.
 *   sqOccupancy/rqOccupancy — sum of in-flight WQEs across owned QPs. */
struct mlx_stats_resp {
    uint64_t postedSend;
    uint64_t postedRead;
    uint64_t postedWrite;
    uint64_t postedUmr;
    uint64_t postedBindMw;
    uint64_t postedLocalInv;
    uint64_t postedRecv;
    uint64_t completedSend;
    uint64_t completedRead;
    uint64_t completedWrite;
    uint64_t completedRecv;
    uint64_t completedUmr;
    uint64_t completedLocalInv;
    uint64_t cqeError;
    uint64_t cqeRetryExc;
    uint64_t cqeRnrRetry;
    uint64_t cqLost;
    uint32_t sqOccupancy;
    uint32_t rqOccupancy;
    uint32_t reserved0;
    uint32_t reserved1;
};

#if defined(__cplusplus)
static_assert(sizeof(struct mlx_stats_resp) == 152,
              "mlx_stats_resp ABI mismatch");
#endif

struct mlx_create_cq_req {
    uint32_t entries;
};

/* createCQ response; the CQ buffer is mapped through clientMemoryForType. */
struct mlx_create_cq_resp {
    uint32_t  cqHandle;
    uint32_t  logSize;          /* log of depth */
    uint32_t  cqeSize;
    uint32_t  dbRecordOffset;
};

#if defined(__cplusplus)
static_assert(sizeof(struct mlx_query_abi_resp) == 8,
              "mlx_query_abi_resp ABI mismatch");
static_assert(sizeof(struct mlx_create_cq_req) == 4,
              "mlx_create_cq_req ABI mismatch");
static_assert(sizeof(struct mlx_create_cq_resp) == 16,
              "mlx_create_cq_resp ABI mismatch");
#endif

/* externalMethod selector */
enum {
    /* device */
    kMlxUCMethodOpen          = 0x1000,
    kMlxUCMethodClose         = 0x1001,
    kMlxUCMethodQueryDevice   = 0x1002,  /* capability query */
    kMlxUCMethodQueryPort     = 0x1003,  /* port state */
    kMlxUCMethodQueryAbi      = 0x1004,  /* ABI version/features */
    kMlxUCMethodQueryLimits   = 0x1005,  /* per-client quota limits (P1.1) */

    /* PD / UAR */
    kMlxUCMethodAllocPD       = 0x1010,
    kMlxUCMethodDeallocPD     = 0x1011,
    kMlxUCMethodAllocUAR      = 0x1012,

    /* QP */
    kMlxUCMethodCreateQP      = 0x1020,
    kMlxUCMethodModifyQP      = 0x1021,
    kMlxUCMethodDestroyQP     = 0x1022,
    kMlxUCMethodQueryQP       = 0x1023,

    /* CQ */
    kMlxUCMethodCreateCQ      = 0x1030,
    kMlxUCMethodDestroyCQ     = 0x1031,

    /* MR */
    kMlxUCMethodRegMR         = 0x1040,
    kMlxUCMethodDeregMR       = 0x1041,
    /* Indirect (KLM) MR: composes already-registered direct MRs under one
     * rkey/lkey without a bigger single-MR page list — see
     * MlxP0EncodingIndirect.hpp and notes/43/44. DeregMR (0x1041) already
     * handles tearing this down; only registration needs its own request
     * shape (a bounded list of child handles, not a client memory range). */
    kMlxUCMethodRegMRIndirect = 0x1042,

    /* AH */
    kMlxUCMethodCreateAH      = 0x1050,
    kMlxUCMethodDestroyAH     = 0x1051,

    /* GID */
    kMlxUCMethodGetGidIndex   = 0x1060,
    kMlxUCMethodSetGid        = 0x1061,
    kMlxUCMethodDelGid        = 0x1062,
    kMlxUCMethodQueryGid      = 0x1063,

    /* congestion control */
    kMlxUCMethodCCQuery       = 0x1070,
    kMlxUCMethodCCModify      = 0x1071,

    /* ===== firmware management (used by mlxconfig/mlxup/mlxlink) ===== */
    kMlxUCMethodAccessReg     = 0x1080,   /* ACCESS_REG register read/write */
    kMlxUCMethodFwCmd         = 0x1081,   /* firmware command passthrough (used by mlxup) */
    kMlxUCMethodQueryPages    = 0x1082,   /* QUERY_PAGES (firmware page management) */
    kMlxUCMethodPortStats     = 0x1083,   /* port statistics (used by mlxlink) */
    kMlxUCMethodFwReset       = 0x1084,   /* firmware reset (used by mlxfwreset) */
    kMlxUCMethodQueryFwVer    = 0x1085,   /* firmware version query */
    kMlxUCMethodQueryHealth   = 0x1086,   /* health status */

    /* DMA data path */
    kMlxUCMethodVirtToPhys    = 0x1090,   /* virtual address → physical address (used by post_send) */
    kMlxUCMethodGetCqBuffer   = 0x1091,   /* get the CQ buffer descriptor (used by poll_cq) */

    /* completion events */
    kMlxUCMethodQueryCqCompletions = 0x1092,  /* query the CQ completion count */

    /* dev-cycle: restart the firmware init chain WITHOUT changing the card
     * owner (IOPCIFamily performs FLR only when handing resources to a new
     * dext process; ReinitFw performs self-FLR + full FwInit in the same process). */
    kMlxUCMethodFwReinit      = 0x10A0,

    /* ===== debug interface for mlx_probe (notes/35) =====
     * Lets you run init stages from userspace WITHOUT rebuilding the dext
     * (card owner unchanged → no IOPCIFamily FLR → card stays alive). */
    kMlxUCMethodDbgFlr            = 0x10A1,  /* self-FLR without FwInit */
    kMlxUCMethodDbgExec           = 0x10A2,  /* raw firmware command (≤64B in, ≤128B out) */
    kMlxUCMethodDbgQueryPages     = 0x10A3,  /* QUERY_PAGES(mode) */
    kMlxUCMethodDbgProvidePages   = 0x10A4,  /* MANAGE_PAGES(GIVE) — sep or contig */
    kMlxUCMethodDbgDumpState      = 0x10A5,  /* snapshot: fw_rev/cmdq/pages */
    kMlxUCMethodStableInitCycle   = 0x10A6,  /* TEARDOWN -> INIT, same fw session */

    /* async events */
    kMlxUCMethodGetAsyncEvent = 0x1093,  /* get an async event (non-blocking) */

    /* CQ consumer index update (kernel-mediated, replaces direct DB record write) */
    kMlxUCMethodUpdateCqConsumer = 0x1094,

    /* Kernel-mediated data path (Option B). */
    kMlxUCMethodPollCQ          = 0x1095,
    kMlxUCMethodPostSend        = 0x1096,
    kMlxUCMethodPostRecv        = 0x1097,
    kMlxUCMethodPostSendBatch   = 0x1098,
    kMlxUCMethodPostRecvBatch   = 0x1099,
    kMlxUCMethodEnableFastPath  = 0x109a,
    kMlxUCMethodSyncFastPath    = 0x109b,
    kMlxUCMethodSyncRecvFastPath = 0x109c,
    /* Activates an indirect (KLM) mkey created via kMlxUCMethodRegMRIndirect:
     * posts a UMR WQE (see MlxWQE.hpp / notes/48) that clears the mkey's
     * hardware "free" bit and (re-)writes its KLM list through the WQE
     * pipeline. Required before the mkey is safe to use as a WQE SGE — the
     * QP must already be in RTS. Caller polls the same CQ (kMlxUCMethodPollCQ)
     * for this WR's own completion before relying on the mkey. */
    kMlxUCMethodPostUmrKlm      = 0x109d,
    /* ABI v2 bounded multi-SGE fallback. Direct mappings remain ABI v1 until
     * variable-WQEBB synchronization has its own hardware parity gate. */
    kMlxUCMethodPostSendSge     = 0x109e,
    kMlxUCMethodPostRecvSge     = 0x109f,
    kMlxUCMethodSyncSendSge     = 0x10a8,
    kMlxUCMethodSyncRecvSge     = 0x10a9,
    kMlxUCMethodPostLocalInv    = 0x10aa,
    kMlxUCMethodAllocMW          = 0x10ab,
    kMlxUCMethodDeallocMW        = 0x10ac,
    kMlxUCMethodBindMW           = 0x10ad,
    kMlxUCMethodQueryStats       = 0x10ae,

    /* ===== P3: inline / atomics / GID enumeration / CQ arming ===== */
    kMlxUCMethodPostSendInline   = 0x10af,
    kMlxUCMethodPostSendAtomic   = 0x10b0,
    kMlxUCMethodQueryGidTable    = 0x10b1,
    kMlxUCMethodArmCQ            = 0x10b2,
};

struct mlx_fast_path_resp {
    uint32_t version;
    uint32_t uarPageSize;
    uint32_t dbPageSize;
    uint32_t maxBatch;
};

/* updateCqConsumer request: tell the kernel the new consumer index for a CQ */
struct mlx_update_cq_consumer_req {
    uint32_t  cqHandle;
    uint32_t  consumerIndex;
};

#define MLX_UC_MAX_POLL_WC 16

enum {
    MLX_UC_SEND_SIGNALED = 1u << 0,
    MLX_UC_SEND_FENCE    = 1u << 1,
    MLX_UC_SEND_SOLICITED = 1u << 2,
    MLX_UC_SEND_INLINE   = 1u << 3,
};

/* Inline payload ceiling (must match MLX_WQE_MAX_INLINE in MlxWQE.hpp).
 * This is the honest provider capability reported through QueryDevice; the
 * DEXT validates every inline WR against it before copying into the SQ. */
#define MLX_UC_MAX_INLINE_DATA 512u

enum {
    MLX_UC_WR_SEND       = 0,
    MLX_UC_WR_RDMA_WRITE = 1,
    MLX_UC_WR_RDMA_READ  = 2,
    MLX_UC_WR_UMR_KLM    = 3,   /* PostUmrKlm only, not mlx_post_send_req */
    MLX_UC_WR_SEND_IMM   = 4,
    MLX_UC_WR_RDMA_WRITE_IMM = 5,
    MLX_UC_WR_LOCAL_INV = 6,
    MLX_UC_WR_ATOMIC_CS = 7,   /* compare-and-swap */
    MLX_UC_WR_ATOMIC_FA = 8,   /* fetch-and-add */
};

enum {
    MLX_UC_WC_SUCCESS    = 0,
    MLX_UC_WC_LOC_LEN    = 1,
    MLX_UC_WC_LOC_QP_OP  = 3,
    MLX_UC_WC_WR_FLUSH   = 4,
    MLX_UC_WC_REM_ACCESS = 7,
    MLX_UC_WC_RETRY_EXC  = 9,
    MLX_UC_WC_RNR_RETRY  = 10,
    MLX_UC_WC_GENERAL    = 12,
};

enum {
    MLX_UC_WC_SEND       = 0,
    MLX_UC_WC_RDMA_WRITE = 1,
    MLX_UC_WC_RDMA_READ  = 2,
    MLX_UC_WC_RECV       = 3,
    MLX_UC_WC_UMR_KLM    = 4,
    MLX_UC_WC_FETCH_ADD  = 5,
    MLX_UC_WC_COMP_SWAP  = 6,
};

enum {
    MLX_UC_WC_WITH_IMM    = 1u << 0,
    MLX_UC_WC_WITH_ATOMIC = 1u << 1,
};

struct mlx_datapath_sge {
    uint64_t addr;
    uint32_t length;
    uint32_t lkey;
};

struct mlx_post_send_req {
    uint32_t qpn;
    uint32_t opcode;
    uint64_t wrId;
    struct mlx_datapath_sge sge;
    uint64_t remoteAddr;
    uint32_t rkey;
    uint32_t sendFlags;        /* MLX_UC_SEND_* */
};

struct mlx_post_recv_req {
    uint32_t qpn;
    uint32_t reserved;
    uint64_t wrId;
    struct mlx_datapath_sge sge;
};

/* Phase 3 bounded posting ABI. One batch is validated as a unit, written to
 * one QP and published with one final DB-record update. SEND rings the UAR
 * once using the final WQE control segment. */
#define MLX_UC_MAX_POST_BATCH 64
#define MLX_UC_MAX_SGE 16

struct mlx_post_send_sge_req {
    uint32_t qpn;
    uint32_t opcode;
    uint64_t wrId;
    uint32_t numSge;
    uint32_t sendFlags;
    uint64_t remoteAddr;
    uint32_t rkey;
    uint32_t immData;           /* ibverbs network byte order; *_IMM only */
    struct mlx_datapath_sge sge[MLX_UC_MAX_SGE];
};

struct mlx_post_recv_sge_req {
    uint32_t qpn;
    uint32_t numSge;
    uint64_t wrId;
    struct mlx_datapath_sge sge[MLX_UC_MAX_SGE];
};

struct mlx_post_send_batch_req {
    uint32_t count;
    uint32_t reserved;
    struct mlx_post_send_req wr[MLX_UC_MAX_POST_BATCH];
};

struct mlx_post_recv_batch_req {
    uint32_t count;
    uint32_t reserved;
    struct mlx_post_recv_req wr[MLX_UC_MAX_POST_BATCH];
};

/* Direct SQ posting publishes WQEs/DB from userspace. This request only
 * advances DEXT completion bookkeeping; it never rings a doorbell. */
struct mlx_sync_fast_path_req {
    uint32_t count;
    uint32_t reserved;
    struct mlx_post_send_req wr[MLX_UC_MAX_POST_BATCH];
};

struct mlx_sync_recv_fast_path_req {
    uint32_t count;
    uint32_t reserved;
    struct mlx_post_recv_req wr[MLX_UC_MAX_POST_BATCH];
};

struct mlx_sync_send_sge_req {
    uint32_t qpn;
    uint32_t opcode;
    uint64_t wrId;
    uint32_t numSge;
    uint32_t sendFlags;
    uint64_t remoteAddr;
    uint32_t rkey;
    uint32_t immData;
    struct mlx_datapath_sge sge[MLX_UC_MAX_SGE];
};

struct mlx_sync_recv_sge_req {
    uint32_t qpn;
    uint32_t numSge;
    uint64_t wrId;
    uint32_t reserved;
    struct mlx_datapath_sge sge[MLX_UC_MAX_SGE];
};

/* LOCAL_INV carries the key explicitly; it is not a data SGE. */
struct mlx_post_local_inv_req {
    uint32_t qpn;
    uint32_t invalidateRkey;
    uint64_t wrId;
    uint32_t sendFlags;
    uint32_t reserved;
};

/* Inline SEND / SEND_WITH_IMM. The payload travels in this request — the
 * DEXT never dereferences the caller's VA for inline (the NIC would DMA it
 * otherwise), so userspace copies the bytes across the boundary. */
struct mlx_post_send_inline_req {
    uint32_t qpn;
    uint32_t opcode;            /* MLX_UC_WR_SEND or MLX_UC_WR_SEND_IMM */
    uint64_t wrId;
    uint32_t inlineLen;         /* 1..MLX_UC_MAX_INLINE_DATA */
    uint32_t sendFlags;         /* MLX_UC_SEND_* */
    uint32_t immData;           /* SEND_IMM only; device (network) byte order */
    uint32_t reserved;
    uint8_t  inlineData[MLX_UC_MAX_INLINE_DATA];
};

/* RC atomic: compare-and-swap / fetch-and-add on a remote 64-bit word.
 * The remote word must be 8-byte aligned and the responder MR must have
 * IBV_ACCESS_REMOTE_ATOMIC. compare/swapAdd are host-order u64. */
struct mlx_post_send_atomic_req {
    uint32_t qpn;
    uint32_t opcode;            /* MLX_UC_WR_ATOMIC_CS or MLX_UC_WR_ATOMIC_FA */
    uint64_t wrId;
    uint64_t remoteAddr;
    uint32_t rkey;
    uint32_t sendFlags;
    uint64_t compare;           /* CMP_SWAP: expected value */
    uint64_t swapAdd;           /* CMP_SWAP: new value / FETCH_ADD: addend */
    uint64_t resultAddr;         /* local 8-byte result buffer */
    uint32_t resultLkey;         /* local MR lkey */
    uint32_t reserved;
};

/* Hardware CQ arming: request a completion EQ event. solicitedOnly arms on
 * the next solicited CQE only. The DEXT rings the CQ arm doorbell; the
 * resulting EQ event is surfaced through the existing completion path. */
struct mlx_arm_cq_req {
    uint32_t cqHandle;
    uint32_t solicitedOnly;
};

/* Full GID-table enumeration, chunked: a single IOConnectCallStructMethod
 * struct output is bounded by the DriverKit struct-method limit, so the full
 * 256-slot table is walked in MLX_UC_MAX_GID_CHUNK-slot windows. */
#define MLX_UC_MAX_GID_TABLE 256u
#define MLX_UC_MAX_GID_CHUNK 64u
struct mlx_gid_table_entry {
    uint32_t index;
    uint8_t  gid[16];
    uint8_t  mac[6];
    uint8_t  roceVersion;
    uint8_t  l3Type;
    uint8_t  gidType;         /* 2=RoCEv2 */
    uint8_t  vlanValid;
    uint16_t vlanId;
    uint32_t ifindex;         /* 0: no macOS netif attachment */
};
struct mlx_query_gid_table_req {
    uint32_t startIndex;      /* first table slot to inspect */
    uint32_t maxEntries;      /* 1..MLX_UC_MAX_GID_CHUNK */
};
struct mlx_query_gid_table_resp {
    uint32_t count;           /* programmed entries returned in this chunk */
    uint32_t tableSize;       /* total firmware GID table slots */
    uint32_t more;            /* 1 if entries remain past this chunk */
    uint32_t reserved;
    struct mlx_gid_table_entry entry[MLX_UC_MAX_GID_CHUNK];
};



struct mlx_poll_cq_req {
    uint32_t cqHandle;
    uint32_t maxEntries;       /* 1..MLX_UC_MAX_POLL_WC */
};

struct mlx_work_completion {
    uint64_t wrId;
    uint32_t status;
    uint32_t opcode;
    uint32_t byteLen;
    uint32_t qpNum;
    uint32_t immData;
    uint32_t wcFlags;           /* MLX_UC_WC_* */
    uint32_t vendorError;
    uint32_t wqeCounter;
    uint64_t atomicResult;      /* MLX_UC_WC_WITH_ATOMIC: pre-op remote word */
};

struct mlx_poll_cq_resp {
    uint32_t count;
    uint32_t reserved;
    struct mlx_work_completion wc[MLX_UC_MAX_POLL_WC];
};

#if defined(__cplusplus)
static_assert(sizeof(struct mlx_datapath_sge) == 16,
              "mlx_datapath_sge ABI mismatch");
static_assert(sizeof(struct mlx_post_send_req) == 48,
              "mlx_post_send_req ABI mismatch");
static_assert(sizeof(struct mlx_post_recv_req) == 32,
              "mlx_post_recv_req ABI mismatch");
static_assert(sizeof(struct mlx_post_send_sge_req) == 296,
              "mlx_post_send_sge_req ABI mismatch");
static_assert(sizeof(struct mlx_post_recv_sge_req) == 272,
              "mlx_post_recv_sge_req ABI mismatch");
static_assert(sizeof(struct mlx_post_send_batch_req) == 3080,
              "mlx_post_send_batch_req ABI mismatch");
static_assert(sizeof(struct mlx_post_recv_batch_req) == 2056,
              "mlx_post_recv_batch_req ABI mismatch");
static_assert(sizeof(struct mlx_work_completion) == 48,
              "mlx_work_completion ABI mismatch");
static_assert(sizeof(struct mlx_sync_fast_path_req) == 3080,
              "mlx_sync_fast_path_req ABI mismatch");
static_assert(sizeof(struct mlx_sync_recv_fast_path_req) == 2056,
              "mlx_sync_recv_fast_path_req ABI mismatch");
static_assert(sizeof(struct mlx_sync_send_sge_req) == 296,
              "mlx_sync_send_sge_req ABI mismatch");
static_assert(sizeof(struct mlx_sync_recv_sge_req) == 280,
              "mlx_sync_recv_sge_req ABI mismatch");
static_assert(sizeof(struct mlx_post_send_inline_req) == 544,
              "mlx_post_send_inline_req ABI mismatch");
static_assert(sizeof(struct mlx_post_send_atomic_req) == 64,
              "mlx_post_send_atomic_req ABI mismatch");
static_assert(sizeof(struct mlx_arm_cq_req) == 8,
              "mlx_arm_cq_req ABI mismatch");
static_assert(sizeof(struct mlx_gid_table_entry) == 36,
              "mlx_gid_table_entry ABI mismatch");
static_assert(sizeof(struct mlx_query_gid_table_resp) ==
              16 + MLX_UC_MAX_GID_CHUNK * 36,
              "mlx_query_gid_table_resp ABI mismatch");

#endif

/* async event (see rdma-core ibv_async_event) */
struct mlx_async_event {
    uint32_t  eventType;       /* see ibv_event_type */
    uint32_t  elementType;     /* MLX_ASYNC_ELEMENT_*: 0=device 1=CQ 2=QP 3=port */
    uint32_t  elementHandle;   /* CQ/QP handle or port_num */
    uint32_t  reserved;
};

/* element types an async event can belong to */
enum {
    MLX_ASYNC_ELEMENT_DEVICE = 0,
    MLX_ASYNC_ELEMENT_CQ     = 1,
    MLX_ASYNC_ELEMENT_QP     = 2,
    MLX_ASYNC_ELEMENT_PORT   = 3,
};

/* event types (see ibv_event_type) */
enum {
    MLX_EVENT_CQ_ERR = 0,
    MLX_EVENT_QP_FATAL = 1,
    MLX_EVENT_COMM_EST = 4,
    MLX_EVENT_SQ_DRAINED = 5,
    MLX_EVENT_PATH_MIG = 6,
    MLX_EVENT_DEVICE_FATAL = 8,
    MLX_EVENT_PORT_ACTIVE = 9,
    MLX_EVENT_PORT_ERR = 10,
    MLX_EVENT_GID_CHANGE = 18,
    MLX_EVENT_WQ_FATAL = 19,
};

/* ===== firmware management structs (POD) ===== */

/* ACCESS_REG request: register_id + read/write direction + data */
struct mlx_access_reg_req {
    uint32_t  registerId;      /* e.g. PFCC/QTCT/PVLC, etc. */
    uint32_t  opMod;           /* 0=read 1=write */
    uint32_t  argument;        /* additional argument */
    uint8_t   data[256];       /* register data */
    uint32_t  dataSize;
};
struct mlx_access_reg_resp {
    uint8_t   data[256];
    uint32_t  dataSize;
};

/* firmware command passthrough (used by mlxup) */
struct mlx_fw_cmd_req {
    uint16_t  opcode;          /* firmware command opcode */
    uint16_t  opMod;
    uint8_t   in[512];
    uint32_t  inSize;
};
struct mlx_fw_cmd_resp {
    uint8_t   out[512];
    uint32_t  outSize;
};

/* firmware version */
struct mlx_fw_ver_resp {
    uint32_t  fwRev;           /* version number (encoded) */
    uint32_t  cmdifRev;
    uint16_t  deviceId;
    uint8_t   portType;
    uint32_t  numPorts;
};

/* port statistics (used by mlxlink) */
struct mlx_port_stats_resp {
    uint64_t  rxPkts;
    uint64_t  txPkts;
    uint64_t  rxBytes;
    uint64_t  txBytes;
    uint64_t  rxDrop;
    uint64_t  txDrop;
    uint64_t  rxErrors;
    uint64_t  txErrors;
    uint32_t  linkSpeed;
    uint8_t   linkState;       /* 0=down 1=up */
    uint8_t   portNum;
};

/* ========== struct definitions (POD) ========== */

/* device capability query response */
struct mlx_query_device_resp {
    uint64_t  fwVersion;
    uint32_t  deviceId;
    uint32_t  numPorts;
    uint32_t  maxQp;
    uint32_t  maxCq;
    uint32_t  maxMr;
    uint16_t  roceVersions;     /* bit0=RoCEv1 bit1=RoCEv2 */
    uint16_t  maxGid;
    uint32_t  maxMsgSize;
    uint32_t  maxInlineData;    /* MLX_UC_MAX_INLINE_DATA once P3 inline lands */
    uint32_t  maxQpRdAtomic;    /* requester outstanding atomics (power of 2) */
    uint32_t  maxQpInitRdAtomic;/* responder outstanding atomics (power of 2) */
};

/* port attributes response */
/* link layer types (see rdma_link_layer, used for mlx_query_port_resp.linkLayer) */
enum {
    MLX_LINK_LAYER_UNSPECIFIED = 0,
    MLX_LINK_LAYER_INFINIBAND  = 1,
    MLX_LINK_LAYER_ETHERNET    = 2,
};

struct mlx_query_port_resp {
    uint32_t  portNum;
    uint8_t   linkLayer;        /* see MlxLinkLayer: 1=IB 2=Ethernet */
    uint8_t   portState;        /* 0=down 1=up */
    uint8_t   gidType;          /* 2=RoCEv2 */
    uint8_t   rsvd;
    uint32_t  activeSpeed;      /* Mbps */
    uint32_t  maxMtu;
    /* IB attributes (reserved for Option C, see ib_port_attr) */
    uint16_t  lid;              /* local LID */
    uint16_t  smLid;            /* subnet manager LID */
    uint16_t  pkeyTblLen;       /* P_Key table length */
    uint16_t  gidTblLen;        /* GID table length */
};

/* createQP request/response */
struct mlx_create_qp_req {
    uint32_t  pd;
    uint32_t  sendCq;
    uint32_t  recvCq;
    uint32_t  qpType;           /* 0=RC 1=UD */
    uint32_t  sqSize;           /* power of 2 */
    uint32_t  rqSize;
    uint64_t  sqBufAddr;        /* must be 0: DEXT-owned WQ (Option B) */
    uint64_t  rqBufAddr;        /* must be 0: DEXT-owned WQ (Option B) */
    uint32_t  dbRecordOffset;   /* DB record user offset */
    uint32_t  bfOffset;         /* BF doorbell user offset */
    uint32_t  maxInlineData;    /* must be <= MLX_UC_MAX_INLINE_DATA */
    uint32_t  rsvd;                 /* bit0: native RQ (no shared dummy SRQ) */
};
struct mlx_create_qp_resp {
    uint32_t  qpn;              /* opaque client token (ABI v2) */
    uint32_t  hwQpn;            /* raw firmware QPN; direct-mode WQE control
                                 * segment only — never a valid ABI handle */
    uint32_t  sqStrideSize;     /* WQE stride after alignment */
    uint32_t  dbRecordOffset;   /* RQ/SQ DB record pair in mapped DB page */
    uint32_t  bfOffset;         /* BF register offset in mapped UAR */
    uint32_t  mappingVersion;
    uint32_t  uarPage;
    uint32_t  rsvd;
};

#if defined(__cplusplus)
static_assert(sizeof(struct mlx_create_qp_resp) == 32,
              "mlx_create_qp_resp ABI mismatch");
#endif

/* modifyQP (state machine) */
struct mlx_modify_qp_req {
    uint32_t  qpn;
    uint32_t  curState;         /* 0=RST 1=INIT 2=RTR 3=RTS */
    uint32_t  newState;
    uint32_t  attrMask;         /* compatible with the Linux ib_qp_attr_mask */
    uint32_t  destQpn;
    uint32_t  pathMtu;
    uint32_t  rqPsn;
    uint32_t  sqPsn;
    uint32_t  pkeyIndex;
    uint32_t  portNum;
    /* AH (used to encode the path on RTR) */
    uint8_t   ahDmac[6];
    uint8_t   ahDgid[16];       /* destination IP */
    uint32_t  ahSgidIndex;      /* source GID */
    uint8_t   ahHopLimit;
    uint8_t   ahTrafficClass;   /* DSCP */
    uint16_t  ahUdpSport;
    uint32_t  minRnrTimer;
    uint32_t  maxDestRdAtomic;
    uint32_t  maxRdAtomic;
    uint32_t  ackTimeout;
    uint32_t  retryCount;
    uint32_t  rnrRetry;
    uint32_t  sl;              /* RoCEv2 VLAN priority / service level (0..7) */
    uint32_t  rsvd;
};

struct mlx_query_qp_resp {
    uint32_t qpn;
    uint32_t state;
    uint32_t destQpn;
    uint32_t pathMtu;
    uint32_t rqPsn;
    uint32_t sqPsn;
    uint32_t sendCq;
    uint32_t recvCq;
};

/* regMR request/response */
struct mlx_query_gid_resp {
    uint32_t index;
    uint8_t  gid[16];
    uint8_t  mac[6];
    uint8_t  roceVersion;
    uint8_t  l3Type;
    uint8_t  vlanValid;
    uint16_t vlanId;
    uint8_t  gidType;         /* 2=RoCEv2 (ibv_gid_type) */
    uint8_t  ifindex;         /* 0: PCIDriverKit has no macOS netif */
};

struct mlx_reg_mr_req {
    uint64_t  startAddr;
    uint64_t  length;
    uint32_t  accessFlags;      /* bit0=LOCAL_WRITE bit1=REMOTE_WRITE ... */
    uint32_t  pd;
};
struct mlx_reg_mr_resp {
    uint32_t  mrHandle;
    uint32_t  lkey;
    uint32_t  rkey;
    uint32_t  rsvd;
    uint64_t  iova;          /* registered userspace VA used in WQE SGEs */
};

/* regMRIndirect request — composes already-registered direct MRs (their
 * handles from a prior RegMR) under one new mkey/rkey/lkey. Response reuses
 * mlx_reg_mr_resp (iova = the caller-supplied logical startAddr below).
 * childCount is bounded by MLX_MR_TABLE_CAP (MlxMR.cpp) since no more
 * distinct MRs than that can exist to reference in the first place. */
#define MLX_UC_MAX_INDIRECT_CHILDREN   32
struct mlx_reg_mr_indirect_req {
    uint64_t  startAddr;              /* logical base presented to the app */
    uint64_t  length;                 /* logical span (normally the sum of children) */
    uint32_t  accessFlags;
    uint32_t  pd;
    uint32_t  childCount;
    uint32_t  childHandles[MLX_UC_MAX_INDIRECT_CHILDREN];
};

/* postUmrKlm request (kMlxUCMethodPostUmrKlm, see notes/48). childHandles
 * must be the exact same list already passed to RegMRIndirect for
 * mrHandle — the kernel re-derives each child's current lkey/addr/length
 * via MlxMR::Lookup() rather than trusting caller-supplied KLM bytes, and
 * re-checks ownership, matching RegMRIndirect's own validation. accessFlags
 * and length are read back from the mkey's own stored record (set at
 * RegMRIndirect time), not re-supplied here — there is exactly one already-
 * validated source of truth for them. */
struct mlx_post_umr_klm_req {
    uint32_t  qpn;
    uint32_t  mrHandle;
    uint32_t  childCount;
    uint32_t  childHandles[MLX_UC_MAX_INDIRECT_CHILDREN];
    uint64_t  wrId;
};

/* Type-2 memory window. Binding is a QP UMR operation; the DEXT validates
 * the parent MR, PD, range, and requested permissions before publishing it. */
struct mlx_alloc_mw_req { uint32_t pd; uint32_t type; };
struct mlx_alloc_mw_resp { uint32_t mwHandle; uint32_t rkey; };
struct mlx_dealloc_mw_req { uint32_t mwHandle; };
struct mlx_bind_mw_req {
    uint32_t qpn;
    uint32_t mwHandle;
    uint32_t mrHandle;
    uint32_t bindRkey;
    uint32_t accessFlags;
    uint32_t sendFlags;
    uint64_t addr;
    uint64_t length;
    uint64_t wrId;
};
enum {
    MLX_BIND_MW_STAGE_NONE = 0,
    MLX_BIND_MW_STAGE_VALIDATE = 1,
    MLX_BIND_MW_STAGE_POST_UMR = 2,
    MLX_BIND_MW_STAGE_DOORBELL = 3,
};
struct mlx_bind_mw_resp {
    uint32_t rkey;
    uint32_t stage;
    uint32_t status;
    uint32_t reserved;
};

#if defined(__cplusplus)
static_assert(sizeof(struct mlx_post_umr_klm_req) == 152,
              "mlx_post_umr_klm_req ABI mismatch");
#endif

/* createAH */
struct mlx_create_ah_req {
    uint8_t   dmac[6];
    uint8_t   dgid[16];
    uint32_t  sgidIndex;
    uint8_t   hopLimit;
    uint8_t   trafficClass;
    uint16_t  udpSport;
    uint32_t  portNum;
    /* IB addressing (Option C: see ah.c:97-98)
     * ahType: 0=RoCE 1=IB */
    uint32_t  ahType;
    uint16_t  dlid;             /* destination LID (IB) */
    uint8_t   pathBits;         /* path bits (IB) */
    uint8_t   sl;               /* service level (IB) */
};
struct mlx_create_ah_resp {
    uint32_t  ahHandle;
    uint32_t  rsvd;
};

struct mlx_set_gid_req {
    uint32_t index;
    uint8_t  gid[16];
    uint8_t  mac[6];
    uint8_t  roceVersion;     /* 2 = RoCEv2 */
    uint8_t  l3Type;          /* 1 = IPv4, 0 = IPv6 */
    uint16_t vlanId;
    uint8_t  vlanValid;
    uint8_t  reserved[3];
};

/* congestion control */
struct mlx_cc_params {
    uint32_t  rpgMinDecFac;     /* multiplicative decrease factor */
    uint32_t  rpgAiRate;        /* additive increase rate */
    uint32_t  rpgTimeReset;     /* increase timer */
    uint32_t  rpgThreshold;     /* ECN marking threshold */
    uint32_t  rpgHai;
    uint32_t  rpgGd;
    uint32_t  rpgTimeInc;
    uint32_t  rsvd;
};

/* ===== debug interface structs (notes/35, mlx_probe) ===== */

/* raw firmware command passthrough (kMlxUCMethodDbgExec) */
struct mlx_dbg_exec_req {
    uint32_t  opcode;
    uint32_t  inSize;
    uint32_t  outSize;          /* ≤ sizeof(resp.out) */
    uint32_t  timeoutMs;
    uint8_t   in[64];           /* inline input buffer */
};
struct mlx_dbg_exec_resp {
    uint32_t  kr;               /* kern_return_t from Exec */
    uint32_t  outSize;          /* bytes actually written */
    uint8_t   out[128];
};

/* QUERY_PAGES (kMlxUCMethodDbgQueryPages) */
struct mlx_dbg_query_pages_req {
    uint32_t  mode;             /* 1=boot 2=init */
};
struct mlx_dbg_query_pages_resp {
    uint32_t  numPages;
    uint32_t  functionId;
    uint32_t  kr;               /* kern_return_t */
    uint32_t  rsvd;
};

/* MANAGE_PAGES GIVE (kMlxUCMethodDbgProvidePages) */
struct mlx_dbg_provide_pages_req {
    uint32_t  numPages;
    uint32_t  mode;             /* 0=separate 4KiB bufs 1=single 128KiB chunk */
    uint32_t  ownership;        /* 1=boot 2=init 3=runtime */
    uint32_t  rsvd;
};
struct mlx_dbg_provide_pages_resp {
    uint32_t  given;            /* pages actually given */
    uint32_t  kr;               /* kern_return_t */
    uint64_t  iova[16];         /* IOVAs of the given pages */
};

/* snapshot (kMlxUCMethodDbgDumpState) */
struct mlx_dbg_state_resp {
    uint32_t  fwRev;
    uint32_t  cmdifRev;
    uint32_t  initializing;
    uint32_t  cmdqLogSzStride;
    uint64_t  cmdqIOVA;
    uint32_t  issi;
    uint32_t  hcaEnabled;
    uint32_t  pagesInUse;
    uint32_t  chunkMode;
    uint64_t  chunkIOVA;
    uint64_t  pageIOVA[8];
};

/* Stable-driver close/open report. The command queue and firmware revision
 * must remain unchanged: any FLR means this did not exercise the intended
 * repeated INIT_HCA path. */
struct mlx_stable_init_cycle_resp {
    uint32_t  kr;
    uint32_t  cycle;
    uint32_t  fwRevBefore;
    uint32_t  fwRevAfter;
    uint64_t  cmdqIOVABefore;
    uint64_t  cmdqIOVAAfter;
    uint32_t  swOwnerId[4];
    uint32_t  swOwnerIdSupported;
    uint32_t  teardownOk;
    uint32_t  initOk;
    uint32_t  phase2Ok;
    uint32_t  negativeTakeRequests;
    uint32_t  negativeTakePages;
    uint32_t  negativeTakeReturned;
    uint32_t  fwOwnedBefore;
    uint32_t  fwOwnedAfter;
    uint32_t  ambiguousAfter;
    uint32_t  accountingOk;
    uint32_t  recoveredWithFlr;
    uint32_t  reclaimRequested;
    uint32_t  reclaimReturned;
    uint32_t  failureStage;
    uint32_t  lastOpcode;
    uint32_t  lastDeliveryStatus;
    uint32_t  lastFwStatus;
    uint32_t  lastSyndrome;
    /* P1.4 diagnostics: when failureStage == MLX_STABLE_STAGE_PHASE2, these
     * name the exact InitPhase2Runtime sub-step and its return code (see
     * mlx_phase2_substage). phase2Ret is a kern_return_t (or a synthetic
     * kIOReturn* for bool-returning steps). */
    uint32_t  phase2SubStage;
    uint32_t  phase2Ret;
    uint32_t  reserved;
};

enum mlx_phase2_substage {
    MLX_PHASE2_SUB_NONE         = 0,
    MLX_PHASE2_SUB_DMA_INIT     = 1,
    MLX_PHASE2_SUB_UAR_INIT     = 2,
    MLX_PHASE2_SUB_ALLOC_PD     = 3,
    MLX_PHASE2_SUB_ALLOC_XRCD   = 4,
    MLX_PHASE2_SUB_ALLOC_UAR    = 5,
    MLX_PHASE2_SUB_EQ_INIT      = 6,
    MLX_PHASE2_SUB_CREATE_EQ    = 7,
    MLX_PHASE2_SUB_ROCE_INIT    = 8,
    MLX_PHASE2_SUB_ENABLE_VPORT = 9,
    MLX_PHASE2_SUB_QUERY_PORT   = 10,
    MLX_PHASE2_SUB_CREATE_CQ    = 11,
    MLX_PHASE2_SUB_CREATE_QP    = 12,
    MLX_PHASE2_SUB_MODIFY_QP    = 13,
    MLX_PHASE2_SUB_DESTROY_QP   = 14,
    MLX_PHASE2_SUB_DESTROY_CQ   = 15,
    MLX_PHASE2_SUB_HEALTH_INIT  = 16,
    MLX_PHASE2_SUB_EQ_POLLER    = 17
};

enum mlx_stable_failure_stage {
    MLX_STABLE_STAGE_NONE = 0,
    MLX_STABLE_STAGE_TEARDOWN = 1,
    MLX_STABLE_STAGE_EVENT_DRAIN = 2,
    MLX_STABLE_STAGE_RECLAIM = 3,
    MLX_STABLE_STAGE_DISABLE = 4,
    MLX_STABLE_STAGE_ENABLE = 5,
    MLX_STABLE_STAGE_ISSI = 6,
    MLX_STABLE_STAGE_BOOT_QUERY = 7,
    MLX_STABLE_STAGE_BOOT_GIVE = 8,
    MLX_STABLE_STAGE_SET_CAP = 9,
    MLX_STABLE_STAGE_INIT_QUERY = 10,
    MLX_STABLE_STAGE_INIT_GIVE = 11,
    MLX_STABLE_STAGE_INIT_HCA = 12,
    MLX_STABLE_STAGE_QUERY_CAP = 13,
    MLX_STABLE_STAGE_PHASE2 = 14,
    MLX_STABLE_STAGE_VERIFY = 15
};

#if defined(__cplusplus)
static_assert(sizeof(struct mlx_stable_init_cycle_resp) == 136,
              "mlx_stable_init_cycle_resp ABI mismatch");
#endif

#endif /* MLX_UC_IO_H */
