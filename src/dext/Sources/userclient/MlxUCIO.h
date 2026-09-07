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
#define MLX_FAST_PATH_ABI_VERSION 2u
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
    MLX_UC_FEATURE_TRUSTED_FAST_PATH = 1u << 12,   /* shared-page shadow state */
    MLX_UC_FEATURE_BLUE_FLAME      = 1u << 13,   /* WC-mapped full-WQE MMIO */
    MLX_UC_FEATURE_CQ_INTERRUPT    = 1u << 14,   /* MSI-X completion wakeup */
    /* Client shared/UMA pages are pinned through the NIC's IODMACommand and
     * remain coherent with CPU/GPU users for the MR lifetime.  On Apple
     * Silicon this is the zero-copy Metal-buffer path: register contents(),
     * then order GPU work after the RDMA CQE (and RDMA after GPU completion). */
    MLX_UC_FEATURE_COHERENT_UMA_MR = 1u << 15,
    MLX_UC_FEATURE_CQ_EVENT_WAIT = 1u << 16, /* may be serviced by timer */
    MLX_UC_FEATURE_RUNTIME_STATUS = 1u << 17,
};

/* Conservative, explicit policy for small-memory Macs, not firmware limits. */
#define MLX_UC_PINNED_BYTES_PER_CLIENT (512ULL * 1024 * 1024)
#define MLX_UC_PINNED_BYTES_PER_DEVICE (1024ULL * 1024 * 1024)
#define MLX_RUNTIME_VERSION 1u
struct mlx_runtime_resp {
    uint32_t version, size;
    uint64_t deviceEpoch;
    uint64_t pinnedBytes, peakPinnedBytes, pinFailures;
    uint64_t clientPinnedBytes, clientPinnedLimit, devicePinnedLimit;
    uint64_t quarantineBytes, quarantineObjects;
    uint64_t irqCompletionEqes, timerCompletionEqes, lastCompletionIrqNs;
    uint32_t quarantined, bmeFenced;
    uint32_t completionEqReady, completionIrqProven;
};

struct mlx_query_abi_resp {
    uint32_t version;
    uint32_t features;
};

/* Shared-page fast path (docs/shared-page-fast-path.md): the per-QP DB-record
 * slot is 128 bytes; hardware doorbell records use offsets 0..7.  The software
 * producer/consumer shadow state lives at offset 8 so a trusted client can
 * publish sq/rq heads and tails without a DriverKit crossing.  All indices are
 * WQE-slot indices (sq_head/sq_tail already include the WQE span). */
#define MLX_QP_SHADOW_OFFSET 8u
struct mlx_qp_shadow {
    /* Userspace seqlock: odd while updating, even when the snapshot is
     * stable.  DEXT consumes this only on control/diagnostic paths. */
    uint64_t sequence;
    uint64_t sq_head;
    uint64_t sq_tail;
    uint64_t rq_head;
    uint64_t rq_tail;
};

/* Which step of the MSI-X bring-up failed. NONE means the vectors are live. */
enum {
    MLX_IRQ_STAGE_NONE          = 0,
    MLX_IRQ_STAGE_CONFIGURE     = 1,  /* IOPCIDevice::ConfigureInterrupts */
    MLX_IRQ_STAGE_QUEUE         = 2,  /* IODispatchQueue::Create */
    MLX_IRQ_STAGE_SOURCE        = 3,  /* IOInterruptDispatchSource::Create */
    MLX_IRQ_STAGE_ACTION        = 4,  /* CreateAction*InterruptOccurred */
    MLX_IRQ_STAGE_HANDLER       = 5,  /* SetHandler */
    MLX_IRQ_STAGE_ENABLE        = 6,  /* SetEnableWithCompletion */
    MLX_IRQ_STAGE_NOT_ATTEMPTED = 7,  /* Start never reached the setup */
};

/* Which step of the completion-EQ bring-up failed. The MSI-X vectors can be
 * live while this EQ is missing, and then blocking completion delivery is
 * unavailable even though the interrupt path itself is fine. */
enum {
    MLX_CQEQ_STAGE_NOT_ATTEMPTED = 0,  /* no completion interrupt to bind to */
    MLX_CQEQ_STAGE_ALLOC         = 1,
    MLX_CQEQ_STAGE_INIT          = 2,  /* ring/DMA setup */
    MLX_CQEQ_STAGE_CREATE        = 3,  /* firmware CREATE_EQ */
    MLX_CQEQ_STAGE_OK            = 4,
};

/* Host interrupt indices live in a DIFFERENT space from the firmware's
 * CREATE_EQ intr field. IOPCIFamily hands out one IOInterruptDispatchSource
 * index per published interrupt specifier, and by long-standing convention
 * index 0 is the legacy INTx line while messaged vectors follow it. Firmware
 * vector V therefore sits at host index msixIndexBase + V, and binding a
 * handler to a hardcoded 0/1 can silently attach it to the wrong source or to
 * INTx, which on an MSI-X-only device behind a Thunderbolt tunnel never fires.
 * The DEXT probes the first MLX_IRQ_INDEX_MAP indices with
 * IOInterruptDispatchSource::GetInterruptType and publishes what it found. */
#define MLX_IRQ_INDEX_MAP 16

enum {
    MLX_IRQ_KIND_ABSENT = 0,  /* GetInterruptType refused this index */
    MLX_IRQ_KIND_LEVEL  = 1,  /* level-triggered, i.e. legacy INTx */
    MLX_IRQ_KIND_EDGE   = 2,
    MLX_IRQ_KIND_MSI    = 3,  /* kIOInterruptTypePCIMessaged  0x00010000 */
    MLX_IRQ_KIND_MSIX   = 4,  /* kIOInterruptTypePCIMessagedX 0x00020000 */
    MLX_IRQ_KIND_OTHER  = 5,  /* answered, but none of the above */
};

/* Interrupt-path diagnosis. Device-wide, read-only; no firmware payload or
 * DMA address crosses this ABI. eqn fields are firmware EQ numbers, which the
 * DEXT already publishes to clients through CreateCQ. */
struct mlx_interrupts_resp {
    uint32_t vectors;           /* MSI-X vectors granted; 0 when none */
    uint32_t setupStatus;       /* kern_return_t of the failing step, 0 = ok */
    uint32_t setupStage;        /* MLX_IRQ_STAGE_* */
    uint32_t asyncEqn;          /* async EQ number, 0 when absent */
    uint32_t completionEqn;     /* completion EQ number, 0 when absent */
    uint32_t completionReady;   /* 1 when MLX_UC_FEATURE_CQ_INTERRUPT is set */
    uint64_t completionEvents;  /* same counter as mlx_perf_resp.cqEvents */
    uint32_t completionEqStatus;    /* kern_return_t of the failing step */
    uint32_t completionEqStage;     /* MLX_CQEQ_STAGE_* */
    uint32_t completionEqSyndrome;  /* firmware syndrome when CREATE_EQ failed */
    uint32_t completionEqFwStatus;  /* firmware status byte for that command */
    /* Which CreateEQ variant firmware accepted, 1-based, 0 = none. Variant 4
     * is diagnostic only: it asks for the async interrupt index, so accepting
     * only that one means firmware objects to the vector, not to the ring. */
    uint32_t completionEqVariant;
    uint32_t completionEqVariantTried;      /* bitmask of attempted variants */
    uint32_t completionEqVariantSyndrome[4];
    /* Async MSI-X interrupts serviced, EQ timer ticks, and the period the
     * timer is currently rearming with. Together they say whether vector 0
     * actually delivers and whether the timer has stepped down from delivery
     * duty to insurance. */
    uint64_t asyncInterrupts;
    uint64_t completionInterrupts;
    uint64_t eqTimerTicks;
    uint32_t eqTimerPeriodMs;
    uint32_t rsvd0;
    /* Host interrupt index map. The "pre" probe runs before
     * ConfigureInterrupts and the plain one after it, because whether the
     * messaged indices exist only after allocation is exactly what was never
     * established. msixIndexBase is the host index carrying firmware vector 0,
     * or MLX_IRQ_INDEX_NONE when no messaged index answered; in that case the
     * DEXT falls back to the historical hardcoded 0/1 rather than not binding. */
    uint32_t indexCount;        /* indices that answered after ConfigureInterrupts */
    uint32_t indexCountPre;     /* indices that answered before it */
    uint32_t msixIndexBase;     /* host index of firmware vector 0 */
    uint32_t asyncIndex;        /* host index the async source is bound to */
    uint32_t completionIndex;   /* host index the completion source is bound to */
    uint32_t indexProbeStatus;  /* kern_return_t of the first refusal, 0 = none */
    uint8_t  indexKind[MLX_IRQ_INDEX_MAP];      /* MLX_IRQ_KIND_*, after */
    uint8_t  indexKindPre[MLX_IRQ_INDEX_MAP];   /* MLX_IRQ_KIND_*, before */
    uint64_t indexTypeRaw[MLX_IRQ_INDEX_MAP];   /* raw GetInterruptType value */
};

#define MLX_IRQ_INDEX_NONE 0xffffffffu

/* MSI-X capability readback (kMlxUCMethodQueryMsixState). Entries and pending
 * bits are read straight out of the BAR the capability points at, so this is
 * what the device itself holds, not what the driver believes it asked for.
 *
 * How to read the result:
 *   messageControl bit 15 clear  -> MSI-X was never enabled; the kernel did
 *                                   not act on ConfigureInterrupts.
 *   entry[V] addr/data all zero  -> that vector was never programmed. If
 *     or vectorControl bit 0 set    firmware raises V, it raises into nothing,
 *                                   which is the host/firmware index mismatch.
 *   entry programmed and a pba   -> the card did raise it and the event was
 *     bit set after a burst         lost past the interrupt controller.
 */
/* Set in mlx_create_qp_resp.bfShared when this QP had to share its blue-flame
 * register with another QP of the same client, so posts from two threads
 * serialise on one MMIO register and on one toggle. */
#define MLX_QP_BF_SHARED 0x1u

/* kMlxUCMethodProgramMsix operations. */
enum {
    MLX_MSIX_OP_PROGRAM = 0,  /* write address, data and mask together */
    MLX_MSIX_OP_MASK    = 1,  /* touch only the mask bit of vectorControl */
    /* Raw dword access inside the MSI-X structures, for one question only:
     * is there hardware behind this window at all? The pending-bit array is
     * read-only in hardware, so a write that comes back on the next read
     * proves we are talking to a shadow rather than to the device — and then
     * masking a vector from here never reached it either. `vector` carries the
     * dword index and `data` the value; addrLo selects the region, 0 for the
     * table and 1 for the pending bits. Bounded to those two ranges. */
    MLX_MSIX_OP_POKE    = 2,
    MLX_MSIX_OP_PEEK    = 3,
};

/* The platform's MSI doorbell. Every message is a DMA write of `data` to this
 * address; the interrupt controller turns `data` into one of its vectors.
 * Taken from the device tree property msi-address, which reads the same for
 * the built-in and the Thunderbolt PCIe controllers on this machine, and which
 * Linux's driver for the same hardware hardcodes with the note that it matches
 * macOS. Passed in the request rather than assumed, so a machine that differs
 * can be told the right value without a rebuild. */
struct mlx_program_msix_req {
    uint32_t op;          /* MLX_MSIX_OP_* */
    uint32_t vector;      /* table entry index */
    uint32_t addrLo;      /* PROGRAM only */
    uint32_t addrHi;      /* PROGRAM only */
    uint32_t data;        /* PROGRAM, or the value for POKE */
    uint32_t masked;      /* 1 = set the mask bit, 0 = clear it */
    uint32_t dataOut;     /* PEEK returns the dword here */
    uint32_t rsvd0;
};

/* QUERY_EQ (kMlxUCMethodQueryEqState). mlx5 EQ states: 0x9 armed, 0xa fired,
 * 0xb always-armed. consumerIndex is the queue's own idea of how far the
 * driver has drained it, which is what the arm doorbell publishes. */
struct mlx_query_eq_req {
    uint32_t eqn;
    uint32_t rsvd0;
};

struct mlx_query_eq_resp {
    uint32_t status;        /* firmware command status */
    uint32_t state;         /* eqc.st */
    uint32_t intr;          /* eqc.intr — the MSI-X vector it would raise */
    uint32_t uarPage;       /* eqc.uar_page — where its doorbells must go */
    uint32_t consumerIndex; /* eqc.consumer_counter */
    uint32_t producerIndex; /* eqc.producer_counter */
    uint32_t logEqSize;
    /* Filled only when the command failed: the firmware status byte and its
     * syndrome. Without them a refusal is a bare kern_return and costs a
     * rebuild to interpret, which is expensive on a machine whose kernel log
     * channel is dead. */
    uint32_t fwStatus;
    uint32_t syndrome;
};

#define MLX_MSIX_TABLE_SNAPSHOT 16
#define MLX_MSIX_PBA_WORDS      4

struct mlx_msix_entry {
    uint32_t addrLo;
    uint32_t addrHi;
    uint32_t data;
    uint32_t vectorControl;   /* bit 0 = masked */
};

struct mlx_msix_state_resp {
    uint32_t capOffset;       /* config offset of the MSI-X capability, 0 = absent */
    uint32_t messageControl;  /* raw 16-bit register, zero-extended */
    uint32_t tableSize;       /* entries the device implements */
    uint32_t tableBir;
    uint32_t pbaBir;
    uint32_t tableOffset;     /* byte offset inside BAR[tableBir] */
    uint32_t pbaOffset;       /* byte offset inside BAR[pbaBir] */
    uint32_t entriesRead;     /* entries actually filled in below */
    uint32_t pbaWords;        /* pba words actually filled in below */
    uint32_t status;          /* kern_return_t of the first failing read */
    uint32_t commandReg;      /* config 0x04: bus master and INTx-disable state */
    uint32_t barIndexUsed;    /* memory index the DEXT read through */
    /* MSI-X entry 0 address/data read BEFORE this DEXT's own ConfigureInterrupts
     * (front A, the 'does the previous owner program the table' experiment).
     * If the previous owner (AppleEthernetMLX5) programmed entry 0, addrLo holds
     * the platform MSI doorbell (e.g. 0xfffff000) and data the vector; zero means
     * the table was already empty before we touched it (or an intervening FLR
     * cleared it). */
    uint32_t preConfigureEntry0AddrLo;
    uint32_t preConfigureEntry0Data;
    struct mlx_msix_entry entry[MLX_MSIX_TABLE_SNAPSHOT];
    uint32_t pba[MLX_MSIX_PBA_WORDS];
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
 * MlxCQ / MlxMR) remain the ultimate bound shared by all clients.
 *
 * These constants are the DEFAULT policy, not the firmware limit: a client
 * signed with the com.mlx5.rdma.entitlement entitlement has the ceilings for
 * QP/CQ/MR/MW raised to the firmware capability (still bounded by the
 * DEXT-wide tables and, for QP/CQ, the shared DB-record slot capacity). This
 * is what lets one trusted client hold as many QPs as it needs without
 * forcing parallel work into separate UserClients (front C, task 2). */
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
    /* Posting-path capabilities. The kernel log channel is dead on the dev
     * machine, so these are the only way to see what the card reported.
     * bfSupported is the firmware's blue-flame bit; when it is clear, or
     * logBfRegSize is zero, bfRegsPerUar is zero and writing a WQE into the
     * doorbell register is not available at all, whatever the environment
     * asks for. bfRegsPerUar is how many independent blue-flame registers a
     * UAR page holds, i.e. how many QPs of one client can post without
     * sharing a doorbell. */
    uint32_t bfSupported;      /* 1 when firmware reports blue flame */
    uint32_t logBfRegSize;     /* log2 of one blue-flame register, 0 = none */
    uint32_t uarPageSize;      /* bytes */
    uint32_t bfRegsPerUar;
    uint32_t maxInlineData;    /* bytes this ABI accepts inline */
    uint32_t maxSge;           /* SGEs this ABI accepts per WR */
    /* Negotiated PCIe link, from the device's PCI Express Capability Link
     * Status register. Gigabits mean nothing across machines; the fraction of
     * this line that a transfer achieves transfers to any card, which is what
     * front E is about. speed is the encoded generation (1=2.5, 2=5, 3=8,
     * 4=16, 5=32 GT/s), width is lanes. Both zero when unreadable. */
    uint32_t pcieLinkSpeed;
    uint32_t pcieLinkWidth;
};

#if defined(__cplusplus)
static_assert(sizeof(struct mlx_query_limits_resp) == 80,
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

struct mlx_probe_completion_vector_req {
    uint32_t intr;      /* 0 = async vector, 1 = completion vector */
};
struct mlx_probe_completion_vector_resp {
    uint32_t eqn;
};

struct mlx_create_cq_req {
    uint32_t entries;
};

struct mlx_modify_cq_moderation_req {
    uint32_t cqHandle;
    uint16_t cqPeriod;      /* microseconds, 0..4095 */
    uint16_t cqMaxCount;    /* CQEs, 0..65535 */
};

/* createCQ response; the CQ buffer is mapped through clientMemoryForType. */
struct mlx_create_cq_resp {
    uint32_t  cqHandle;
    uint32_t  logSize;          /* log of depth */
    uint32_t  cqeSize;
    uint32_t  dbRecordOffset;
};

/* P0 performance counters. Read-only, per UserClient; values are monotonic
 * until that client closes. Time fields are CLOCK_UPTIME_RAW nanoseconds.
 *
 * doorbells    — MMIO doorbells the DEXT rang for this client. Only the
 *     kernel-mediated post/arm selectors ring one here; a direct-UAR client
 *     rings its own from userspace and counts them itself, so the two
 *     together separate the direct path from the kernel path.
 * cqeConsumed / cqeErrors — CQEs the kernel-mediated PollCQ returned to this
 *     client, and how many of them carried a non-success status. A direct-CQ
 *     client decodes the mapped ring itself and these stay flat.
 * cqEvents     — completion MSI-X interrupts the DEXT serviced that advanced
 *     the device completion generation. Device-wide, not per client: it is
 *     the only in-driver view of interrupt delivery, so comparing it with a
 *     client's own wakeup count shows whether wakeups are being missed.
 * cqEventWakeups — WaitCqEvent calls that returned a fresh generation instead
 *     of timing out. Device-wide like cqEvents: the shim blocks on its own
 *     UserClient connection, so a per-client count would always read zero from
 *     the connection that queries the counters. */
struct mlx_perf_resp {
    uint64_t externalMethods;
    uint64_t externalMethodNs;
    uint64_t postSendCalls;
    uint64_t postRecvCalls;
    uint64_t pollCqCalls;
    uint64_t syncFastPathCalls;
    uint64_t syncQpTailsCalls;
    uint64_t armCqCalls;
    uint64_t doorbells;
    uint64_t cqeConsumed;
    uint64_t cqeErrors;
    uint64_t mrRegisters;
    uint64_t mrDeregisters;
    uint64_t mrBytes;
    uint64_t copiedBytes;
    uint64_t cqEvents;
    uint64_t cqEventWakeups;
    /* Firmware commands the DEXT issued, and how many of them outlived the
     * command path's spin window and had to sleep a millisecond. Device-wide,
     * like cqEvents. Every control operation goes through here — registering
     * memory, creating a QP, programming a GID — so a high sleep ratio shows
     * up as a flat millisecond added to all of them. */
    uint64_t fwCommands;
    uint64_t fwCommandSleeps;
};

#if defined(__cplusplus)
static_assert(sizeof(struct mlx_interrupts_resp) == 288,
              "mlx_interrupts_resp ABI mismatch");
static_assert(sizeof(struct mlx_msix_entry) == 16,
              "mlx_msix_entry ABI mismatch");
static_assert(sizeof(struct mlx_msix_state_resp) == 328,
              "mlx_msix_state_resp ABI mismatch");
static_assert(sizeof(struct mlx_perf_resp) == 152,
              "mlx_perf_resp ABI mismatch");
static_assert(sizeof(struct mlx_query_abi_resp) == 8,
              "mlx_query_abi_resp ABI mismatch");
static_assert(sizeof(struct mlx_modify_cq_moderation_req) == 8,
              "mlx_modify_cq_moderation_req ABI mismatch");
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
    /* Hardware completion moderation (MODIFY_CQ). Holds a completion event
     * back until cqPeriod microseconds pass or cqMaxCount CQEs accumulate, so
     * a streaming transfer raises one event per batch instead of one per arm.
     * Zero in a field disables that half; both zero restores no moderation. */
    kMlxUCMethodModifyCqModeration = 0x1032,

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

    /* MSI-X setup diagnosis. The DEXT logs the failing step, but a dev box
     * with the kernel log channel disabled sees nothing, and a client that
     * finds MLX_UC_FEATURE_CQ_INTERRUPT missing otherwise cannot tell a
     * vector shortage from a dispatch-source failure. */
    kMlxUCMethodQueryInterrupts = 0x1087,

    /* Diagnostic: rebind the completion EQ to a chosen MSI-X index and report
     * the new EQ number. Refused while any CQ is live. Separates "MSI-X is
     * never delivered to this dext" from "the firmware intr index does not
     * match the dispatch-source index". */
    kMlxUCMethodProbeCompletionVector = 0x1088,

    /* Diagnostic: read back the device's MSI-X capability, the first entries
     * of its table and the pending-bit array. Splits three causes apart that
     * the counters alone cannot: MSI-X never enabled, the kernel programmed
     * entries other than the ones firmware raises, or the card raised a vector
     * that was lost after the controller. Raw config/BAR reads, so it sits
     * behind the same privileged-diagnostics gate as AccessReg. */
    kMlxUCMethodQueryMsixState = 0x1089,

    /* Diagnostic: write one MSI-X table entry, or just flip its mask bit.
     * The kernel programs this table from allocateDeviceInterrupts -> initDevice
     * using an address and a data base it gets from the platform; on this
     * machine that never happened and the table read back all zeroes, so the
     * card had nowhere to send a vector. This lets the bring-up program it and
     * calibrate the data base by experiment rather than by hardcoding it.
     * Raw BAR writes, so it sits behind the privileged-diagnostics gate. */
    kMlxUCMethodProgramMsix = 0x108a,

    /* Diagnostic: ask firmware for an event queue's own state. This is the
     * one fact that separates "the card never tried to raise the vector" from
     * "it raised and the message went nowhere": an EQ that still reads ARMED
     * after an entry was written to it never fired. QUERY_EQ needs a larger
     * output mailbox than the raw debug exec path allows, hence its own
     * selector rather than a passthrough. */
    kMlxUCMethodQueryEqState = 0x108b,
    /* Diagnostic-only: suspend EQ timer work while MSI-X delivery is tested. */
    kMlxUCMethodSetEqTimerPaused = 0x108c,

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
    kMlxUCMethodQueryPerf        = 0x10b4,  /* P0 performance counters */
    kMlxUCMethodWaitCqEvent      = 0x10b5,  /* blocking wait on MSI-X generation */
    kMlxUCMethodQueryRuntime     = 0x10b6,  /* fixed v1 size; additive selector */

    /* ===== P3: inline / atomics / GID enumeration / CQ arming ===== */
    kMlxUCMethodPostSendInline   = 0x10af,
    kMlxUCMethodPostSendAtomic   = 0x10b0,
    kMlxUCMethodQueryGidTable    = 0x10b1,
    kMlxUCMethodArmCQ            = 0x10b2,
    kMlxUCMethodSyncQpTails      = 0x10b3,
    /* Diagnostic: prove the RMP context layout before anything is built on it. */
    kMlxUCMethodProbeRmpLayout   = 0x10b7,
    kMlxUCMethodCreateSrq        = 0x10b8,
    kMlxUCMethodDestroySrq       = 0x10b9,
    kMlxUCMethodPostSrqRecv      = 0x10ba,
    kMlxUCMethodQuerySrq         = 0x10bb,
    kMlxUCMethodModifySrq        = 0x10bc,
    /* Reads the recorded refusal straight from the QP context. Deliberately
     * not folded into QueryQP: that one asks firmware, and a pair that never
     * left RESET cannot be queried — which is exactly when the reason for a
     * refused transition is wanted. */
    kMlxUCMethodQpLastRefusal    = 0x10bd,
};

/* kMlxUCMethodProbeRmpLayout.
 *
 * Creates an RMP, reads it back with QUERY_RMP, and reports what firmware
 * stored against what was written. Every offset below is recalled from
 * mlx5_ifc rather than read out of a working path in this tree, which is
 * exactly why it is checked instead of trusted: a wrong offset produces a
 * silent queue, and that failure mode already cost this project an evening
 * on the MSI-X table.
 *
 * create_rmp_in: opcode @0x00, rmpc @0x100.
 * rmpc:          state @0x08 (4b), basic_cyclic_rcv_wqe @0x20 (1b), wq @0x180.
 * wq (absolute = 0x280): wq_type @+0x00 (4b), page_offset @+0x2b (5b),
 *                lwm @+0x30 (16b), pd @+0x48 (24b), uar_page @+0x68 (24b),
 *                dbr_addr @+0x80 (64b), log_wq_stride @+0x10c (4b),
 *                log_wq_pg_sz @+0x113 (5b), log_wq_sz @+0x11b (5b),
 *                pas @+0x300.
 */
#define MLX_RMP_VARIANTS 6

/* One CREATE_RMP attempt. Each rebuild costs the owner a cycle, so the probe
 * walks several hypotheses in one call instead of one per cycle. */
struct mlx_rmp_variant {
    uint32_t inSize;        /* bytes handed to the firmware command */
    uint32_t wqType;
    uint32_t basicCyclic;
    uint32_t logWqStride;   /* QPC uses log2(stride)-4; wq may use plain log2 */
    uint32_t uarPage;       /* mlx5's set_wq leaves this zero for an RMP */
    uint32_t status;        /* kIOReturnSuccess when firmware accepted it */
    uint32_t syndrome;
    uint32_t rsvd;
};

struct mlx_probe_rmp_layout_resp {
    uint32_t status;        /* kIOReturnSuccess, or the stage that failed */
    uint32_t stage;         /* 0 create, 1 query, 2 compare, 3 destroy */
    uint32_t rmpn;
    uint32_t fwStatus;      /* firmware syndrome of the failing command */
    struct mlx_rmp_variant variant[MLX_RMP_VARIANTS];
    uint32_t variantUsed;   /* index that firmware accepted, or 0xffffffff */
    uint32_t rsvd2;

    /* written / read back, one pair per field */
    uint32_t setState,        gotState;
    uint32_t setWqType,       gotWqType;
    uint32_t setLogWqStride,  gotLogWqStride;
    uint32_t setLogWqSz,      gotLogWqSz;
    uint32_t setLogWqPgSz,    gotLogWqPgSz;
    uint32_t setPd,           gotPd;
    uint32_t setUarPage,      gotUarPage;
    uint64_t setDbrAddr,      gotDbrAddr;
    uint64_t setPas0,         gotPas0;

    uint32_t mismatches;    /* bit per field, 0 = layout proven */
    uint32_t rsvd;
};
#if defined(__cplusplus)
static_assert(sizeof(struct mlx_probe_rmp_layout_resp) == 312,
              "mlx_probe_rmp_layout_resp ABI mismatch");
#endif

struct mlx_wait_cq_event_req {
    uint64_t generation;
    uint32_t timeoutMs;
    uint32_t rsvd;
};

struct mlx_wait_cq_event_resp {
    uint64_t generation;
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
    /* A datagram receive lands 40 bytes of global routing header ahead of the
     * payload. The flag says the header is there; the client must have posted
     * room for it, and the driver refuses a receive too short to hold it. */
    MLX_UC_WC_GRH         = 1u << 2,
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
    /* A connected pair addresses memory; a datagram addresses a peer. The two
     * never coexist in one request and need the same twelve bytes, so they
     * overlap rather than grow the struct — a batch of 64 of these already
     * sits close to the inline transfer limit. Anonymous members keep every
     * existing use of remoteAddr and rkey untouched. */
    union {
        struct {
            uint64_t remoteAddr;
            uint32_t rkey;
        };
        struct {
            uint32_t ahHandle;      /* address vector to copy into the WQE */
            uint32_t remoteQpn;
            uint32_t remoteQkey;
        };
    };
    uint32_t sendFlags;        /* MLX_UC_SEND_* */
};

struct mlx_post_recv_req {
    uint32_t qpn;
    uint32_t reserved;
    uint64_t wrId;
    struct mlx_datapath_sge sge;
};

/* ---- Shared receive queue ---------------------------------------------------
 * A basic SRQ on this firmware is an RMP; see MlxSRQ.hpp for the layout and
 * the two conventions that differ from the QPC. */
#define MLX_SRQ_MAX_SGE 3       /* a 64-byte WQE holds a next segment plus 3 */

struct mlx_create_srq_req {
    uint32_t pd;
    uint32_t maxWr;
    uint32_t maxSge;
    uint32_t limit;             /* MODIFY_RMP watermark, 0 to leave disarmed */
};

struct mlx_create_srq_resp {
    uint32_t srqn;
    uint32_t logSize;           /* granted, may exceed the request */
    uint32_t maxWr;
    uint32_t maxSge;
};

struct mlx_post_srq_recv_req {
    uint32_t srqn;
    uint32_t numSge;
    uint64_t wrId;
    struct mlx_datapath_sge sge[MLX_SRQ_MAX_SGE];
};

struct mlx_query_srq_resp {
    uint32_t srqn;
    uint32_t maxWr;
    uint32_t maxSge;
    uint32_t limit;
    uint32_t freeCount;         /* WQEs still on the free list */
    uint32_t posted;            /* mirrored into the DB record */
    /* Observation of the completion path, so a receive that is not recognised
     * as coming from this queue says so instead of surfacing later as an
     * unattributable completion. Recorded for every receive CQE, whether or
     * not it carried an srqn. */
    uint32_t recvSeen;          /* receive CQEs observed */
    uint32_t recvWithSrqn;      /* of those, carrying a non-zero srqn */
    uint32_t lastCqeSrqn;
    uint32_t lastCqeWqe;
    uint32_t lastCqeOp;
    /* Bitmap of WQE indices the completion path has seen. A field that is
     * stuck at one value recycles a single WQE while the rest leak, and the
     * symptom is a local protection error once firmware still owns the one
     * being rewritten. One bit per index tells them apart at a glance. */
    uint32_t wqeSeenMask;
};
#if defined(__cplusplus)
static_assert(sizeof(struct mlx_query_srq_resp) == 48,
              "mlx_query_srq_resp ABI mismatch");
#endif

struct mlx_qp_refusal_resp {
    uint32_t qpn;
    uint32_t fwStatus;
    uint32_t syndrome;
    uint32_t rsvd;
};

struct mlx_modify_srq_req {
    uint32_t srqn;
    uint32_t limit;
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

/* Inline SEND / SEND_WITH_IMM / RDMA_WRITE / RDMA_WRITE_WITH_IMM. The payload
 * travels in this request — the DEXT never dereferences the caller's VA for
 * inline (the NIC would DMA it otherwise), so userspace copies the bytes
 * across the boundary. Inline is not a send-only trick: on a one-sided write
 * it removes the NIC's DMA read of the payload, which is what a small KV
 * update spends most of its time on. remoteAddr and rkey are required for the
 * write opcodes and must be zero for the send ones. */
struct mlx_post_send_inline_req {
    uint32_t qpn;
    uint32_t opcode;            /* MLX_UC_WR_SEND, _SEND_IMM, _WRITE, _WRITE_IMM */
    uint64_t wrId;
    uint64_t remoteAddr;        /* write opcodes only */
    uint32_t inlineLen;         /* 1..MLX_UC_MAX_INLINE_DATA */
    uint32_t sendFlags;         /* MLX_UC_SEND_* */
    uint32_t immData;           /* _IMM only; device (network) byte order */
    uint32_t rkey;              /* write opcodes only */
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

/* Advance a QP's completion tails after a userspace direct-CQ decode consumed
 * CQEs without the kernel-mediated PollCQ.  Keeps DestroyQP's in-flight check
 * (sqHead==sqTail && rqHead==rqTail) and the stats consistent. */
struct mlx_sync_qp_tails_req {
    uint32_t  qpn;              /* opaque QP token */
    uint32_t  rsvd;
    uint64_t  sqTail;
    uint64_t  rqTail;
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
/* Grew from 48 to 56: the datagram destination overlaps the connected one,
 * but a union holding a 64-bit address rounds to 16 bytes. A batch of 64
 * therefore goes 3080 -> 3592, still inside the 4096-byte inline limit. */
static_assert(sizeof(struct mlx_post_send_req) == 56,
              "mlx_post_send_req ABI mismatch");
static_assert(sizeof(struct mlx_post_recv_req) == 32,
              "mlx_post_recv_req ABI mismatch");
static_assert(sizeof(struct mlx_post_send_sge_req) == 296,
              "mlx_post_send_sge_req ABI mismatch");
static_assert(sizeof(struct mlx_post_recv_sge_req) == 272,
              "mlx_post_recv_sge_req ABI mismatch");
static_assert(sizeof(struct mlx_post_send_batch_req) == 3592,
              "mlx_post_send_batch_req ABI mismatch");
static_assert(sizeof(struct mlx_post_recv_batch_req) == 2056,
              "mlx_post_recv_batch_req ABI mismatch");
static_assert(sizeof(struct mlx_work_completion) == 48,
              "mlx_work_completion ABI mismatch");
static_assert(sizeof(struct mlx_sync_fast_path_req) == 3592,
              "mlx_sync_fast_path_req ABI mismatch");
static_assert(sizeof(struct mlx_sync_recv_fast_path_req) == 2056,
              "mlx_sync_recv_fast_path_req ABI mismatch");
static_assert(sizeof(struct mlx_sync_send_sge_req) == 296,
              "mlx_sync_send_sge_req ABI mismatch");
static_assert(sizeof(struct mlx_sync_recv_sge_req) == 280,
              "mlx_sync_recv_sge_req ABI mismatch");
static_assert(sizeof(struct mlx_post_send_inline_req) == 552,
              "mlx_post_send_inline_req ABI mismatch");
static_assert(sizeof(struct mlx_post_send_atomic_req) == 64,
              "mlx_post_send_atomic_req ABI mismatch");
static_assert(sizeof(struct mlx_arm_cq_req) == 8,
              "mlx_arm_cq_req ABI mismatch");
static_assert(sizeof(struct mlx_sync_qp_tails_req) == 24,
              "mlx_sync_qp_tails_req ABI mismatch");
static_assert(sizeof(struct mlx_qp_shadow) == 40,
              "mlx_qp_shadow ABI mismatch");
static_assert(MLX_QP_SHADOW_OFFSET + sizeof(struct mlx_qp_shadow) <= 128,
              "mlx_qp_shadow must fit in the 128-byte DB-record slot");
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

/* ACCESS_REG request: register_id + read/write direction + data.
 * 512 bytes of payload so the wide counter registers fit: PPCNT alone is
 * 264 bytes (8 byte header + a 256 byte counter set). */
#define MLX_UC_ACCESS_REG_MAX_DATA 512u
struct mlx_access_reg_req {
    uint32_t  registerId;      /* e.g. PPCNT/MPEIN/PFCC/PTYS */
    uint32_t  opMod;           /* 0=write 1=read, as the firmware defines it */
    uint32_t  argument;        /* additional argument */
    uint8_t   data[MLX_UC_ACCESS_REG_MAX_DATA];  /* register payload */
    uint32_t  dataSize;        /* payload bytes supplied, and requested back */
};
struct mlx_access_reg_resp {
    uint8_t   data[MLX_UC_ACCESS_REG_MAX_DATA];
    uint32_t  dataSize;        /* payload bytes written */
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
    uint64_t  rxDrop;          /* RFC 2863 if_in_discards */
    uint64_t  txDrop;          /* RFC 2863 if_out_discards */
    uint64_t  rxErrors;        /* FCS + alignment + too-long */
    uint64_t  txErrors;        /* RFC 2863 if_out_errors */
    uint64_t  rxPause;         /* 802.3 pause frames received */
    uint64_t  txPause;         /* 802.3 pause frames transmitted */
    uint32_t  linkSpeed;
    uint8_t   linkState;       /* 0=down 1=up */
    uint8_t   portNum;
    uint16_t  reserved0;
};

#if defined(__cplusplus)
static_assert(sizeof(struct mlx_port_stats_resp) == 88,
              "mlx_port_stats_resp ABI mismatch");
static_assert(sizeof(struct mlx_access_reg_req) == 528,
              "mlx_access_reg_req ABI mismatch");
static_assert(sizeof(struct mlx_access_reg_resp) == 516,
              "mlx_access_reg_resp ABI mismatch");
#endif

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
/* Per-QP flags in mlx_create_qp_req::rsvd.  The trusted flag is the client's
 * explicit opt-in: the DEXT consults the shared-page shadow only when the
 * client set it, so a shim that predates the shared page (or that maps the SQ
 * but keeps the validated path) can never make DestroyQP/SyncQpTails read a
 * stale shadow. */
#define MLX_UC_QP_TRUSTED 1u
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
    uint32_t  rsvd;             /* MLX_UC_QP_* flags (bit0: trusted fast path) */
    /* Shared receive queue token, 0 for a QP that owns its receive ring. When
     * set, the QPC carries rq_type = MLX5_SRQ_RQ and srqn_rmpn_xrqn, and no
     * receive ring is allocated for this QP. */
    uint32_t  srqn;
    uint32_t  rsvd2;
};
#if defined(__cplusplus)
/* Grew from 56 to 64 when srqn was added. There was no assertion here before,
 * so the driver and the tools could disagree silently and a QP would simply
 * fail to create. They must be rebuilt together. */
static_assert(sizeof(struct mlx_create_qp_req) == 64,
              "mlx_create_qp_req ABI mismatch");
#endif
struct mlx_create_qp_resp {
    uint32_t  qpn;              /* opaque client token (ABI v2) */
    uint32_t  hwQpn;            /* raw firmware QPN; direct-mode WQE control
                                 * segment only — never a valid ABI handle */
    uint32_t  sqStrideSize;     /* WQE stride after alignment */
    uint32_t  dbRecordOffset;   /* RQ/SQ DB record pair in mapped DB page */
    uint32_t  bfOffset;         /* BF register offset in mapped UAR */
    uint32_t  mappingVersion;
    uint32_t  uarPage;
    uint32_t  bfBufSize;        /* bytes in one of the two BF ping-pong buffers */
    uint32_t  bfFlags;          /* MLX_QP_BF_SHARED when the register is shared */
};

#if defined(__cplusplus)
static_assert(sizeof(struct mlx_create_qp_resp) == 36,
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
    /* Unreliable datagram only: the queue key a receiver must match. UD has no
     * path in the context, so this and the port are all RST->INIT carries. */
    uint32_t  qkey;
    uint32_t  qkeyRsvd;
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
    /* Traffic-marking readback from the firmware QPC primary path (front F,
     * task 2): the values the card actually holds, not the values the client
     * asked for. sl is the RoCEv2 VLAN priority 0..7, trafficClass the full
     * 8-bit TOS (DSCP<<2 | ECN), dscp the 6-bit DSCP the switch sees. These
     * are what both endpoints must agree on for ECN to behave as designed. */
    uint32_t sl;
    uint32_t trafficClass;
    uint32_t dscp;
    /* Why the last state transition was refused. Firmware answers a rejected
     * MODIFY_QP with an outbox status that the driver maps to one coarse
     * return code, and the syndrome behind it is the only thing that names
     * the field. Keeping it here means a refusal can be diagnosed without a
     * driver log, which on this machine does not reach userspace. */
    uint32_t lastFwStatus;
    uint32_t lastFwSyndrome;
};

#if defined(__cplusplus)
static_assert(sizeof(struct mlx_query_qp_resp) == 52,
              "mlx_query_qp_resp ABI mismatch");
#endif

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
 * childCount is bounded by the 4112-byte single-mailbox CREATE_MKEY budget:
 * (4112 - 272) / 16-byte-KLM-entry = 240 (MlxP0EncodingIndirect.hpp). */
#define MLX_UC_MAX_INDIRECT_CHILDREN   240
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
static_assert(sizeof(struct mlx_post_umr_klm_req) == 984,
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
