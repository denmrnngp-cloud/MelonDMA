/*
 * MlxEQ.cpp — Event Queue (DriverKit port).
 *
 * Ported from: kernel_src/mlnx-ofed-kernel-5.9/core/eq.c, trimmed for DEXT.
 * CREATE_EQ encoding matches AppleMCX donor byte-for-byte (notes/35,
 * donors/applemcx/Sources/core/MlxEQ.cpp).
 *
 * MVP: polling-driven dispatch (no MSI-X yet — intr=vector, poll the ring).
 * EQE is 64 bytes (mlx5_ifc_eqe_bits): event_type@byte1, event_sub_type@byte3,
 * owner@byte63 bit0. Owner bit toggles each ring wrap (lib/eq.h:61).
 */
#include "MlxEQ.hpp"
#include "MlxDriverKitCompat.h"
#include "MlxRegs.hpp"
#include "MlxP1Encoding.hpp"   /* mlxP1SetEvent */
#include "MlxP0Encoding.hpp"   /* mlxAppendMttPages */
#include "MlxPCIDriver.h"
#include "MlxUAR.hpp"
#include "MlxIfcHelpers.hpp"   /* mlxSetBits / mlxGetBits */

#include <DriverKit/IOLib.h>
#include <DriverKit/IOMemoryDescriptor.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#include <PCIDriverKit/IOPCIDevice.h>
#include <string.h>

#include "MlxLog.hpp"
#define MLX_LOG(fmt, ...)  IOLog("MlxEQ: " fmt "\n", ##__VA_ARGS__)
#define MLX_DBG(fmt, ...)  MLX_DBGLOG("MlxEQ: " fmt, ##__VA_ARGS__)

/* mlx5_eqe — 64 bytes, memory layout as firmware DMA-writes it (device.h:769). */
struct MlxEqe {
    uint8_t  rsvd0;              /* byte 0 */
    uint8_t  type;               /* byte 1  event_type */
    uint8_t  rsvd1;              /* byte 2 */
    uint8_t  subType;            /* byte 3  event_sub_type */
    uint8_t  rsvd2[28];          /* bytes 4..31 */
    uint8_t  eventData[28];      /* bytes 32..59 (union mlx5_ifc_event_auto_bits) */
    uint16_t rsvd3;              /* bytes 60..61 */
    uint8_t  signature;          /* byte 62 */
    uint8_t  owner;              /* byte 63, bit0 = ownership */
} __attribute__((packed));

struct MlxEQ::State {
    MlxPCIDriver          *core;
    IOPCIDevice           *pci;
    uint8_t                barIndex;
    uint32_t               vector;       /* MSI-X vector index (MVP: 0) */
    uint32_t               eqn;         /* firmware EQ number */
    IOBufferMemoryDescriptor *eqeMem;
    IODMACommand          *eqeDma;
    uint64_t               eqeIOVA;
    MlxEqe                *eqeBuf;       /* mapped CPU address */
    uint32_t               head;        /* consumer index */
    uint32_t               depth;
    uint32_t               logSize;
    uint64_t               mask[4];      /* event mask (4×64-bit) */
    uint64_t               pageDMA[MLX_MAX_EQ_PAGES];
    uint32_t               numPages;
    MlxEventNotifier      *notifier;
    struct IOLock         *lock;
    bool                   armed;
    bool                   polling;      /* reentrancy guard: nested Poll() */
    uint32_t               unknown;      /* EQE with an unregistered type */
    uint32_t               overflow;     /* budget exceeded in one pass */
};

MlxEQ::MlxEQ() : s(NULL) {}
MlxEQ::~MlxEQ() { Free(); }

kern_return_t
MlxEQ::Init(MlxPCIDriver *core, uint32_t vector)
{
    if (!core) return kIOReturnBadArgument;
    s = new State;
    if (!s) return kIOReturnNoMemory;
    memset(s, 0, sizeof(*s));
    s->core     = core;
    s->pci      = core->GetPCI();
    s->barIndex = core->GetBar0Index();
    s->vector   = vector;
    s->depth    = MLX_EQ_DEPTH;
    s->logSize  = 8;   /* log2(256) */
    s->lock     = IOLockAlloc();
    if (!s->lock) { delete s; s = NULL; return kIOReturnNoMemory; }

    /* Allocate the EQE ring (DEXT-owned DMA-coherent, 4 KiB aligned). */
    uint64_t ringBytes = (uint64_t)s->depth * MLX_EQE_SIZE;
    kern_return_t kr = mlxAllocDmaBuffer(ringBytes, 4096,
                                         kIOMemoryDirectionOutIn, &s->eqeMem);
    if (kr != kIOReturnSuccess || !s->eqeMem) {
        MLX_LOG("eqe ring alloc failed: 0x%x", kr);
        IOLockFree(s->lock); delete s; s = NULL;
        return kr ? kr : kIOReturnNoMemory;
    }

    IOAddressSegment segs[32];
    uint32_t segCount = 32;
    kr = mlxPrepareDma(s->pci, s->eqeMem, segs, &segCount, &s->eqeDma);
    if (kr != kIOReturnSuccess || segCount == 0) {
        MLX_LOG("eqe DMA prepare failed: 0x%x", kr);
        s->eqeMem->release(); s->eqeMem = NULL;
        IOLockFree(s->lock); delete s; s = NULL;
        return kr ? kr : kIOReturnNoMemory;
    }
    s->eqeIOVA = segs[0].address;

    /* Split IOVA segments into 4 KiB PAS pages (mlx5 CREATE_EQ pas[]). */
    s->numPages = 0;
    for (uint32_t i = 0; i < segCount && s->numPages < MLX_MAX_EQ_PAGES; i++) {
        if (!mlxAppendMttPages(segs[i].address, segs[i].length,
                               s->pageDMA, MLX_MAX_EQ_PAGES, &s->numPages)) {
            MLX_LOG("eqe PAS split failed at seg %u", i);
            mlxCompleteDma(s->eqeDma); s->eqeDma = NULL;
            s->eqeMem->release(); s->eqeMem = NULL;
            IOLockFree(s->lock); delete s; s = NULL;
            return kIOReturnNoSpace;
        }
    }

    uint64_t addr = 0, len = 0;
    kr = s->eqeMem->Map(0, 0, 0, 0, &addr, &len);
    if (kr != kIOReturnSuccess) {
        MLX_LOG("eqe CPU map failed: 0x%x", kr);
        mlxCompleteDma(s->eqeDma); s->eqeDma = NULL;
        s->eqeMem->release(); s->eqeMem = NULL;
        IOLockFree(s->lock); delete s; s = NULL;
        return kr;
    }
    s->eqeBuf = (MlxEqe *)(uintptr_t)addr;
    memset(s->eqeBuf, 0, ringBytes);
    /* Owner bit = 1 for every slot: SW owns, HW flips to 0 on write. */
    for (uint32_t i = 0; i < s->depth; i++)
        s->eqeBuf[i].owner = 1;
    mlxMemoryBarrier();

    /* Event mask: CMD completion + PAGE_REQUEST + port events.
     * NOTE: MLX_EVENT_TYPE_COMPLETION (bit 0) is deliberately NOT set here.
     * This single EQ is created with intr=0 (async vector), and ConnectX-4 Lx
     * firmware rejects the completion bit on an async-vector EQ → CREATE_EQ
     * fails → fEQ stays NULL → fHealth is never created → QueryHealth reports
     * healthy=0. The userspace completion channel does not need the EQ
     * completion event: GetCompletions() scans the CQE ring for pending CQEs
     * directly. A dedicated completion EQ (with a real MSI-X vector) is the
     * future path if EQ-driven wakeups ever replace the 1 ms poll. */
    memset(s->mask, 0, sizeof(s->mask));
    mlxP1SetEvent(s->mask, MLX_EVENT_TYPE_CMD);
    mlxP1SetEvent(s->mask, MLX_EVENT_TYPE_PAGE_REQUEST);
    mlxP1SetEvent(s->mask, MLX_EVENT_TYPE_PORT_STATE_CHANGE);
    mlxP1SetEvent(s->mask, MLX_EVENT_TYPE_NIC_VPORT_CHANGE);
    mlxP1SetEvent(s->mask, MLX_EVENT_TYPE_DEVICE_FATAL);
    mlxP1SetEvent(s->mask, MLX_EVENT_TYPE_WQ_CATAS_ERROR);

    return kIOReturnSuccess;
}

void
MlxEQ::Free()
{
    if (!s) return;
    if (s->eqn && (s->eqeDma || s->eqeMem)) {
        s->core->RetainDmaUntilReset(s->eqeMem, s->eqeDma,
                                     0x45510000u | (s->eqn & 0xffffu));
        s->eqeDma = NULL;
        s->eqeMem = NULL;
    }
    if (s->eqeDma) { mlxCompleteDma(s->eqeDma); s->eqeDma = NULL; }
    if (s->eqeMem) { s->eqeMem->release(); s->eqeMem = NULL; }
    if (s->lock)   { IOLockFree(s->lock); s->lock = NULL; }
    delete s; s = NULL;
}

kern_return_t
MlxEQ::CreateEQ(uint32_t *eqn)
{
    if (!s || !eqn) return kIOReturnBadArgument;

    /* CREATE_EQ IFC (AppleMCX donor): EQC@0x80, event mask@0x2c0, PAS@0x880. */
    uint8_t in[4096] = {};
    uint8_t out[64] = {};
    const uint32_t eqcOff   = 0x80 / 8;    /* 16 */
    const uint32_t maskOff  = 0x2c0 / 8;   /* 88 */
    const uint32_t pasOff   = 0x880 / 8;   /* 272 */

    mlxSetBits(in, 0x00, 16, MLX_CMD_OP_CREATE_EQ);

    /* eqc (mlx5_ifc_eqc_bits) */
    uint8_t *eqc = in + eqcOff;
    mlxSetBits(eqc, 0x63, 5, s->logSize);                     /* log_eq_size */
    uint32_t uarPage = s->core->GetUAR() ? s->core->GetUAR()->GetBootUarIndex() : 0;
    mlxSetBits(eqc, 0x68, 24, uarPage);                       /* uar_page */
    mlxSetBits(eqc, 0xb4, 12, s->vector);                     /* intr (MVP: 0 = poll) */
    mlxSetBits(eqc, 0xc3, 5, 0);                              /* log_page_size = 4 KiB */

    /* event_bitmask: four 64-bit IFC words (BE). */
    for (uint32_t i = 0; i < 4; i++)
        mlxSetBits(in, 0x2c0 + i * 64, 64, s->mask[i]);

    /* pas: 4 KiB page addresses (BE). */
    for (uint32_t i = 0; i < s->numPages; i++)
        mlxSetBits(in, 0x880 + i * 64, 64, s->pageDMA[i]);

    uint32_t inSize = pasOff + s->numPages * 8;
    MLX_DBG("DBG CREATE_EQ: logSz=%u uar=%u intr=%u pages=%u iova=0x%llx",
            s->logSize, uarPage, s->vector, s->numPages,
            (unsigned long long)s->eqeIOVA);

    kern_return_t kr = s->core->Exec(MLX_CMD_OP_CREATE_EQ, in, inSize,
                                     out, sizeof(out), 5000);
    if (kr != kIOReturnSuccess) {
        MLX_LOG("CREATE_EQ FAILED: 0x%x", kr);
        return kr;
    }
    s->eqn = (uint32_t)mlxGetBits(out, 0x58, 8);
    s->armed = true;
    *eqn = s->eqn;
    MLX_DBG("EQ created (eqn=%u, logSz=%u, depth=%u, pages=%u, vec=%u)",
            s->eqn, s->logSize, s->depth, s->numPages, s->vector);
    return kIOReturnSuccess;
}

kern_return_t
MlxEQ::DestroyEQ(uint32_t eqn)
{
    if (!s) return kIOReturnBadArgument;
    uint8_t in[16] = {};
    uint8_t out[16] = {};
    mlxSetBits(in, 0x00, 16, MLX_CMD_OP_DESTROY_EQ);
    mlxSetBits(in, 0x58, 8, eqn);
    kern_return_t kr = s->core->Exec(MLX_CMD_OP_DESTROY_EQ, in, sizeof(in),
                                     out, sizeof(out), 5000);
    if (kr == kIOReturnSuccess) {
        s->armed = false;
        s->eqn = 0;
    }
    return kr;
}

void
MlxEQ::MarkDestroyedByTeardown()
{
    if (!s) return;
    IOLockLock(s->lock);
    s->armed = false;
    s->eqn = 0;
    IOLockUnlock(s->lock);
}

uint32_t
MlxEQ::EqNumber() const { return s ? s->eqn : 0; }

void
MlxEQ::AddNotifier(MlxEventNotifier *n)
{
    if (!s) return;
    IOLockLock(s->lock);
    s->notifier = n;
    IOLockUnlock(s->lock);
}

void
MlxEQ::RemoveNotifier(MlxEventNotifier *n)
{
    if (!s) return;
    IOLockLock(s->lock);
    if (s->notifier == n) s->notifier = NULL;
    IOLockUnlock(s->lock);
}

void
MlxEQ::Poll()
{
    if (!s || !s->armed || !s->eqeBuf) return;
    /* Exec/user-client callbacks and the continuous timer run on different
     * DriverKit queues.  Serialize ownership of head/owner processing; a
     * plain bool is not a cross-queue exclusion primitive. */
    IOLockLock(s->lock);
    if (s->polling) {
        IOLockUnlock(s->lock);
        return;
    }
    s->polling = true;
    IOLockUnlock(s->lock);
    uint32_t sizeMask = s->depth - 1;
    const uint32_t BUDGET = 64;   /* don't monopolize the workloop */
    uint32_t processed = 0;
    while (processed < BUDGET) {
        MlxEqe *eqe = &s->eqeBuf[s->head & sizeMask];
        mlxMemoryBarrier();
        /* owner toggles each ring wrap (lib/eq.h:61): new iff owner == expected. */
        uint32_t owner    = eqe->owner & 1;
        uint32_t expected = (s->head >> s->logSize) & 1;
        if ((owner ^ expected) != 0) break;   /* HW still owns — drained */
        /* Copy the EQE and advance head BEFORE dispatch: a nested Poll (from GIVE
         * inside the handler) must not see this same EQE again. */
        MlxEqe localEqe = *eqe;
        s->head++;
        processed++;
        MlxEventNotifier *n = NULL;
        IOLockLock(s->lock);
        n = s->notifier;
        IOLockUnlock(s->lock);
        if (n) n->HandleEvent(localEqe.type, &localEqe);
        else   s->unknown++;
    }
    if (processed == BUDGET) s->overflow++;
    /* Publish the consumer index only when head actually advanced.
     * An empty 10-ms poll must not generate meaningless PCIe MMIO. */
    if (processed) UpdateCi(false);
    IOLockLock(s->lock);
    s->polling = false;
    IOLockUnlock(s->lock);
}

void
MlxEQ::UpdateCi(bool arm)
{
    if (!s || !s->eqn) return;
    MlxUAR *uar = s->core->GetUAR();
    if (!uar) return;
    uintptr_t uarOff = uar->UarOffset(uar->GetBootUarIndex());
    uint32_t ci = (s->head & 0xFFFFFF) | (s->eqn << 24);
    /* lib/eq.h:68 — arm → +0x40, plain CI → +0x48. */
    uintptr_t off = uarOff + MLX_EQ_DOORBELL + (arm ? 0 : 8);
    mlxMMIOWrite32BE(s->pci, s->barIndex, off, ci);
}

kern_return_t
MlxEQ::Arm()
{
    if (!s) return kIOReturnNotReady;
    /* MVP: poll-mode — CI + arm doorbell via UAR (MSI-X later). */
    s->armed = true;
    UpdateCi(true);
    return kIOReturnSuccess;
}
