/*
 * MlxFwPages.cpp — Firmware page management (DriverKit port).
 *
 * Ported from: kernel_src/mlnx-ofed-kernel-5.9/core/pagealloc.c. Boot/init/
 * runtime pages are provided to firmware via MANAGE_PAGES. Each page
 * descriptor is an IOBufferMemoryDescriptor (4 KiB) pinned with
 * IODMACommand; the IOVA goes into the command. Firmware-owned pages retain
 * their DMA mappings until TAKE/release-all or a trusted graceful teardown
 * (REMEDIATION_PLAN §3, §5.2: ambiguous ownership is quarantined, not freed).
 *
 * v0.35+: diagnostic logs in AllocPage/ManagePages/ProvidePages,
 * ProvidePagesContig (Apple-style single 128 KiB DMA buffer), debug accessors.
 */
#include "MlxFwPages.hpp"
#include "MlxDriverKitCompat.h"
#include "MlxRegs.hpp"
#include "MlxP1Encoding.hpp"
#include "MlxPCIDriver.h"

#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#include <PCIDriverKit/IOPCIDevice.h>
#include <string.h>

#include "MlxLog.hpp"
#define MLX_LOG(fmt, ...)  IOLog("MlxFwPages: " fmt "\n", ##__VA_ARGS__)
#define MLX_DBG(fmt, ...)  MLX_DBGLOG("MlxFwPages: " fmt, ##__VA_ARGS__)

struct MlxFwPageInternal {
    uint32_t                functionId;
    uint8_t                 ownership;       /* boot/init/runtime/own */
    uint8_t                 state;           /* MLX_FW_PG_* */
    IOBufferMemoryDescriptor *mem;
    IODMACommand          *dma;
    uint64_t                iova;
};

struct MlxFwPages::State {
    MlxPCIDriver    *core;
    IOPCIDevice     *pci;
    MlxFwPageInternal pages[MLX_FW_MAX_PAGES];
    bool            inUse[MLX_FW_MAX_PAGES];
    struct IOLock   *lock;
    bool            quarantined;

    /* Page accounting (diagnostics). */
    uint32_t        hostAllocated;   /* state==ALLOCATED */
    uint32_t        firmwareOwned;   /* state==GIVEN */
    uint32_t        ambiguousOwned;  /* state==GIVE_PENDING/QUARANTINE */
    uint32_t        returnedCount;   /* state==RETURNED */
    uint32_t        negativeTakeRequests; /* firmware PAGE_REQUEST n < 0 */
    uint32_t        negativeTakePages;
    uint32_t        negativeTakeReturned;

    /* Chunk mode (ProvidePagesContig): one IOBMD + one IODMACommand */
    IOBufferMemoryDescriptor *chunkMem;
    IODMACommand          *chunkDma;
    uint64_t               chunkIOVA;
    uint32_t               chunkNumPages;
    bool                   chunkMode;

    /* Second chunk for init pages (ownership=2, ~4465 pages = ~17 MB).
     * One chunk is taken by boot — init cannot overwrite it. */
    IOBufferMemoryDescriptor *chunkMem2;
    IODMACommand          *chunkDma2;
    uint64_t               chunkIOVA2;
    uint32_t               chunkNumPages2;
    bool                   chunkMode2;

    /* Runtime extents (ownership=3) — a list of large DMA buffers, one per
     * runtime allocation. A new request (even a smaller one) → a new extent. */
    MlxRuntimeExtent runtimeExtents[MLX_FW_MAX_RUNTIME_EXTENTS];

    /* Runtime page request state machine (coalescing by function_id). */
    bool           rtActive;
    uint32_t       rtFuncId;
    uint32_t       rtTarget;              /* total requested amount */
    uint32_t       rtGiven;               /* already given */
    uint32_t       rtLatestOutstanding;   /* last remainder from fw */
    kern_return_t  rtLastError;
};

MlxFwPages::MlxFwPages() : s(NULL) {}
MlxFwPages::~MlxFwPages() { Free(); }

kern_return_t
MlxFwPages::Init(MlxPCIDriver *core)
{
    if (!core) return kIOReturnBadArgument;
    s = new State;
    if (!s) return kIOReturnNoMemory;
    memset(s, 0, sizeof(*s));
    s->core = core;
    s->pci  = core->GetPCI();
    s->lock = IOLockAlloc();
    if (!s->lock) { delete s; s = NULL; return kIOReturnNoMemory; }
    s->chunkMem = NULL;
    s->chunkDma = NULL;
    s->chunkMode = false;
    s->chunkMem2 = NULL;
    s->chunkDma2 = NULL;
    s->chunkMode2 = false;
    return kIOReturnSuccess;
}

void
MlxFwPages::Free()
{
    if (!s) return;

    /* Free never crosses an unestablished DMA boundary. The caller
     * must keep MlxFwPages itself alive until a confirmed FLR/reset. */
    if (s->quarantined || GetAmbiguousOwned() || GetFirmwareOwned()) {
        MLX_LOG("Free REFUSED: fw_owned=%u ambiguous=%u quarantine=%u — mappings retained",
                GetFirmwareOwned(), GetAmbiguousOwned(), s->quarantined ? 1 : 0);
        return;
    }

    ReleaseHostMappings();
    if (s->lock) { IOLockFree(s->lock); s->lock = NULL; }
    delete s; s = NULL;
}

kern_return_t
MlxFwPages::AllocPage(uint32_t *outIndex)
{
    IOLockLock(s->lock);
    int slot = -1;
    for (uint32_t i = 0; i < MLX_FW_MAX_PAGES; i++)
        if (!s->inUse[i]) { slot = i; break; }
    if (slot < 0) { IOLockUnlock(s->lock); return kIOReturnNoSpace; }

    kern_return_t kr = mlxAllocDmaBuffer(MLX_FW_PAGE_SIZE_BYTES, 4096,
                                         kIOMemoryDirectionOutIn, &s->pages[slot].mem);
    if (kr != kIOReturnSuccess || !s->pages[slot].mem) {
        MLX_LOG("page alloc failed: 0x%x", kr);
        IOLockUnlock(s->lock); return kr ? kr : kIOReturnNoMemory;
    }
    IOAddressSegment segs[32];
    uint32_t segCount = 32;
    kr = mlxPrepareDma(s->pci, s->pages[slot].mem, segs, &segCount, &s->pages[slot].dma);
    if (kr != kIOReturnSuccess || segCount != 1 ||
        segs[0].length < MLX_FW_PAGE_SIZE_BYTES || !segs[0].address ||
        (segs[0].address & (MLX_FW_PAGE_SIZE_BYTES - 1))) {
        if (s->pages[slot].dma) {
            mlxCompleteDma(s->pages[slot].dma);
            s->pages[slot].dma = NULL;
        }
        s->pages[slot].mem->release(); s->pages[slot].mem = NULL;
        IOLockUnlock(s->lock); return kr ? kr : kIOReturnNoSpace;
    }
    s->pages[slot].iova = segs[0].address;
    s->pages[slot].ownership = 0;
    s->pages[slot].state = MLX_FW_PG_ALLOCATED;
    s->pages[slot].functionId = 0;
    s->inUse[slot] = true;
    s->hostAllocated++;
    *outIndex = slot;

    /* Diagnostic delta (notes/35): IOVA + DMA segment detail. */
    MLX_LOG("DBG AllocPage: slot=%u iova=0x%llx segs=%u seg0={0x%llx, %llu}",
            slot, (unsigned long long)segs[0].address, segCount,
            (unsigned long long)segs[0].address, (unsigned long long)segs[0].length);

    IOLockUnlock(s->lock);
    return kIOReturnSuccess;
}

void
MlxFwPages::ReleasePage(uint32_t index)
{
    if (!s || index >= MLX_FW_MAX_PAGES) return;
    IOBufferMemoryDescriptor *mem = NULL;
    IODMACommand *dma = NULL;
    IOLockLock(s->lock);
    if (!s->inUse[index] || s->quarantined ||
        s->pages[index].state == MLX_FW_PG_GIVEN ||
        s->pages[index].state == MLX_FW_PG_GIVE_PENDING ||
        s->pages[index].state == MLX_FW_PG_QUARANTINE) {
        IOLockUnlock(s->lock);
        return;
    }
    mem = s->pages[index].mem;
    dma = s->pages[index].dma;
    s->pages[index].mem = NULL;
    s->pages[index].dma = NULL;
    if (s->pages[index].state == MLX_FW_PG_ALLOCATED && s->hostAllocated) s->hostAllocated--;
    if (s->pages[index].state == MLX_FW_PG_RETURNED && s->returnedCount) s->returnedCount--;
    memset(&s->pages[index], 0, sizeof(s->pages[index]));
    s->inUse[index] = false;
    IOLockUnlock(s->lock);
    if (dma) mlxCompleteDma(dma);
    if (mem) mem->release();
}

kern_return_t
MlxFwPages::ManagePages(uint16_t op, uint32_t *indices, uint32_t count,
                         uint16_t functionId, uint32_t *outCount)
{
    /* MANAGE_PAGES (0x108): op_mod = GIVE(1)/TAKE(2), function_id, page_count,
     * then page IOVAs. Heap buffers sized by count (not MLX_FW_MAX_PAGES — stack). */
    if (!indices || !count || count > MLX_FW_MAX_PAGES)
        return kIOReturnBadArgument;
    uint64_t *pages = static_cast<uint64_t *>(IOMallocZero(count * sizeof(uint64_t)));
    uint8_t  *in    = static_cast<uint8_t *>(IOMallocZero(16 + count * 8));
    uint8_t   out[16] = {};
    if (!pages || !in) {
        if (pages) IOFree(pages, count * sizeof(uint64_t));
        if (in) IOFree(in, 16 + count * 8);
        return kIOReturnNoMemory;
    }
    uint32_t n = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (indices[i] >= MLX_FW_MAX_PAGES || !s->inUse[indices[i]] ||
            (op == MLX_P1_PAGES_GIVE &&
             s->pages[indices[i]].state != MLX_FW_PG_ALLOCATED)) {
            IOFree(pages, count * sizeof(uint64_t));
            IOFree(in, 16 + count * 8);
            return kIOReturnBadArgument;
        }
        pages[n++] = s->pages[indices[i]].iova;
    }
    if (!mlxP1EncodeManagePages(in, 16 + n * 8, op, functionId, false, pages, n)) {
        IOFree(pages, count * sizeof(uint64_t));
        IOFree(in, 16 + count * 8);
        return kIOReturnBadArgument;
    }

    /* Until the doorbell, ownership is already ambiguous: a timeout can mean fw
     * accepted the GIVE even though the completion did not arrive. */
    if (op == MLX_P1_PAGES_GIVE) {
        IOLockLock(s->lock);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t idx = indices[i];
            if (!s->inUse[idx] || s->pages[idx].state != MLX_FW_PG_ALLOCATED)
                continue;
            s->pages[idx].state = MLX_FW_PG_GIVE_PENDING;
            if (s->hostAllocated) s->hostAllocated--;
            s->ambiguousOwned++;
        }
        IOLockUnlock(s->lock);
    }

    kern_return_t kr = s->core->Exec(MLX_CMD_OP_MANAGE_PAGES, in,
                                      16 + n * 8, out, sizeof(out), 5000);
    if (outCount) *outCount = (kr == kIOReturnSuccess) ? n : 0;

    /* Mark pages of the successful GIVE batch as given to fw (ownership
     * transfers to firmware — DMA must not be freed until TAKE). */
    if (op == MLX_P1_PAGES_GIVE) {
        IOLockLock(s->lock);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t idx = indices[i];
            if (!s->inUse[idx] || s->pages[idx].state != MLX_FW_PG_GIVE_PENDING)
                continue;
            if (kr == kIOReturnSuccess) {
                s->pages[idx].state = MLX_FW_PG_GIVEN;
                s->pages[idx].functionId = functionId;
                if (s->ambiguousOwned) s->ambiguousOwned--;
                s->firmwareOwned++;
            } else if (kr == kIOReturnTimeout) {
                s->pages[idx].state = MLX_FW_PG_QUARANTINE;
                s->quarantined = true;
            } else {
                s->pages[idx].state = MLX_FW_PG_ALLOCATED;
                if (s->ambiguousOwned) s->ambiguousOwned--;
                s->hostAllocated++;
            }
        }
        IOLockUnlock(s->lock);
        if (kr == kIOReturnTimeout) {
            s->core->EnterDmaQuarantine(0x1080000u | functionId);
            MLX_LOG("MANAGE_PAGES GIVE timeout: func=%u batch=%u — ambiguous ownership",
                    functionId, n);
        }
    }

    IOFree(pages, count * sizeof(uint64_t));
    IOFree(in, 16 + count * 8);
    return kr;
}

kern_return_t
MlxFwPages::QueryStartupPages(uint16_t queryMode, uint32_t *outNumPages)
{
    return QueryStartupPagesFull(queryMode, outNumPages, NULL);
}

kern_return_t
MlxFwPages::QueryStartupPagesFull(uint16_t queryMode, uint32_t *outNumPages,
                                   uint32_t *outFunctionId)
{
    /* QUERY_PAGES (0x107): op_mod at bit 0x30 (16 bits, mlx5_ifc):
     *   1=BOOT_PAGES, 2=INIT_PAGES, 3=REGULAR_PAGES.
     * Out: num_pages at bit 0x60 (BE32), function_id at bit 0x50 (BE16). */
    uint8_t in[16] = {};
    uint8_t out[16] = {};
    if (queryMode != 1 && queryMode != 2) return kIOReturnBadArgument;
    mlxSetBits(in, 0x00, 16, MLX_CMD_OP_QUERY_PAGES);
    mlxSetBits(in, 0x30, 16, queryMode);   /* op_mod on bits 0x30-0x3f */
    kern_return_t kr = s->core->Exec(MLX_CMD_OP_QUERY_PAGES, in, sizeof(in),
                                     out, sizeof(out), 5000);
    if (kr != kIOReturnSuccess) {
        MLX_LOG("QUERY_PAGES(mode=%u) failed: 0x%x", queryMode, kr);
        return kr;
    }
    uint32_t numPages = (uint32_t)mlxGetBits(out, 0x60, 32);
    uint32_t functionId = (uint32_t)mlxGetBits(out, 0x50, 16);
    if (outFunctionId) *outFunctionId = functionId;
    if (outNumPages) *outNumPages = numPages;
    MLX_DBG("QUERY_PAGES(mode=%u): num_pages=%u function_id=%u",
            queryMode, numPages, functionId);
    return kIOReturnSuccess;
}

kern_return_t
MlxFwPages::ProvidePages(uint32_t numPages, uint8_t ownership, uint32_t functionId)
{
    /* Allocate numPages DMA pages and give them to firmware via MANAGE_PAGES(GIVE)
     * in chunks (pagealloc.c:mlx5_cmd_give_pages). */
    if (!numPages) return kIOReturnSuccess;
    if (numPages > MLX_FW_MAX_PAGES) {
        MLX_LOG("ProvidePages: too many (%u > %u)", numPages, MLX_FW_MAX_PAGES);
        return kIOReturnNoSpace;
    }
    const uint32_t CHUNK = 64;   /* 16 + 64*8 = 528B — fits in one mailbox */
    uint32_t given = 0;
    while (given < numPages) {
        uint32_t n = (numPages - given < CHUNK) ? (numPages - given) : CHUNK;
        uint32_t idx[CHUNK];
        for (uint32_t i = 0; i < n; i++) {
            kern_return_t kr = AllocPage(&idx[i]);
            if (kr != kIOReturnSuccess) {
                MLX_LOG("page alloc failed at %u/%u: 0x%x", given + i, numPages, kr);
                for (uint32_t j = 0; j < i; j++) ReleasePage(idx[j]);
                uint32_t taken = 0;
                kern_return_t takeKr = given ? TakePages(functionId, given, &taken)
                                             : kIOReturnSuccess;
                if (takeKr != kIOReturnSuccess || taken < given) EnterQuarantine();
                else ReleaseHostMappings();
                if (ownership == 3 && !given) NotifyAllocationFailure(functionId);
                return kr;
            }
            s->pages[idx[i]].ownership = ownership;
            s->pages[idx[i]].functionId = functionId;
        }

        /* Diagnostic delta (notes/35): IOVAs of all pages before GIVE. */
        for (uint32_t k = 0; k < n; k++)
            MLX_LOG("DBG ProvidePages: chunk %u/%u [%u] idx=%u iova=0x%llx",
                    given, numPages, k, idx[k],
                    (unsigned long long)s->pages[idx[k]].iova);

        uint32_t outCount = 0;
        kern_return_t kr = ManagePages(MLX_P1_PAGES_GIVE, idx, n,
                                       (uint16_t)functionId, &outCount);
        if (kr != kIOReturnSuccess) {
            MLX_LOG("MANAGE_PAGES(GIVE) failed at %u/%u: 0x%x", given, numPages, kr);
            for (uint32_t j = 0; j < n; j++) ReleasePage(idx[j]);
            if (kr == kIOReturnTimeout || GetAmbiguousOwned()) {
                EnterQuarantine();
            } else {
                uint32_t taken = 0;
                kern_return_t takeKr = given ? TakePages(functionId, given, &taken)
                                             : kIOReturnSuccess;
                if (takeKr != kIOReturnSuccess || taken < given) EnterQuarantine();
                else ReleaseHostMappings();
                if (ownership == 3 && !given) NotifyAllocationFailure(functionId);
            }
            return kr;
        }
        given += n;
        MLX_LOG("GIVE %u pages (%u/%u, own=%u, func=%u)", n, given, numPages,
                ownership, functionId);
    }
    return kIOReturnSuccess;
}

kern_return_t
MlxFwPages::HandleRuntimePageRequest(uint32_t functionId, int32_t numPages)
{
    /* Runtime PAGE_REQUEST (eqe data): positive numPages → GIVE,
     * negative → TAKE/reclaim (pagealloc.c:req_pages_handler).
     * Coalescing during an active GIVE is performed at the EQE level
     * (MlxRoCE::HandleEvent), and here — only reentrancy protection. */
    if (numPages > 0) {
        uint32_t n = (uint32_t)numPages;
        if (s->rtActive) {
            if (functionId == s->rtFuncId) {
                /* Remainder during an active GIVE (defensive — usually
                 * intercepted in MlxRoCE before the FIFO). */
                s->rtLatestOutstanding = n;
                MLX_LOG("PAGE_REQUEST: func=%u outstanding=%u coalesced-active",
                        functionId, n);
                return kIOReturnSuccess;
            }
            MLX_LOG("PAGE_REQUEST: func=%u busy (func=%u active) — deferred",
                    functionId, s->rtFuncId);
            return kIOReturnBusy;
        }

        /* New generation of the request. */
        s->rtActive = true;
        s->rtFuncId = functionId;
        s->rtTarget = n;
        s->rtGiven = 0;
        s->rtLatestOutstanding = n;
        s->rtLastError = 0;
        MLX_LOG("PAGE_REQUEST: func=%u target=%u start", functionId, n);

        kern_return_t kr = ProvidePagesContig(n, 3 /* runtime */, functionId);
        s->rtActive = false;
        s->rtLastError = kr;
        if (kr == kIOReturnSuccess)
            MLX_LOG("PAGE_GIVE: func=%u complete=%u error=0", functionId, s->rtGiven);
        else
            MLX_LOG("PAGE_GIVE: func=%u FAILED given=%u target=%u err=0x%x",
                    functionId, s->rtGiven, n, kr);
        return kr;
    }
    if (numPages < 0) {
        uint32_t n = (uint32_t)(-(int64_t)numPages);
        IOLockLock(s->lock);
        s->negativeTakeRequests++;
        s->negativeTakePages += n;
        IOLockUnlock(s->lock);
        MLX_LOG("PAGE_REQUEST: TAKE %u pages (func=%u)", n, functionId);
        uint32_t taken = 0;
        kern_return_t kr = TakePages(functionId, n, &taken);
        IOLockLock(s->lock);
        s->negativeTakeReturned += taken;
        IOLockUnlock(s->lock);
        if (kr != kIOReturnSuccess || taken < n) {
            MLX_LOG("PAGE_TAKE: func=%u requested=%u returned=%u — quarantine",
                    functionId, n, taken);
            EnterQuarantine();
            return kr ? kr : kIOReturnBusy;
        }
        return ReleaseHostMappings();
    }
    return kIOReturnSuccess;
}

kern_return_t
MlxFwPages::NotifyAllocationFailure(uint32_t functionId)
{
    uint8_t in[16] = {};
    uint8_t out[16] = {};
    if (!mlxP1EncodeManagePages(in, sizeof(in), MLX_P1_PAGES_ALLOCATION_FAIL,
                                (uint16_t)functionId, false, NULL, 0))
        return kIOReturnBadArgument;
    kern_return_t kr = s->core->Exec(MLX_CMD_OP_MANAGE_PAGES, in, sizeof(in),
                                     out, sizeof(out), 5000);
    MLX_LOG("PAGE_REQUEST: ALLOCATION_FAIL func=%u -> 0x%x", functionId, kr);
    return kr;
}

kern_return_t
MlxFwPages::TakePages(uint32_t functionId, uint32_t numPages, uint32_t *outCount)
{
    /* Real TAKE (pagealloc.c:reclaim_pages): input = 16B header with
     * op_mod=TAKE/function_id/input_num_entries; output = 16B + numPages*8,
     * where output_num_entries@0x40 and the returned PAS@0x80. Match each
     * returned IOVA against bookkeeping (function_id, iova), clear
     * GIVEN → RETURNED. */
    if (outCount) *outCount = 0;
    if (!numPages) return kIOReturnSuccess;
    if (!s || s->quarantined || GetAmbiguousOwned()) return kIOReturnNotReady;
    const uint32_t BATCH = MLX_P1_MAX_MANAGE_PAGES;
    uint32_t total = 0;
    uint32_t remaining = numPages;
    while (remaining) {
        uint32_t b = (remaining < BATCH) ? remaining : BATCH;
        uint32_t outSize = 16 + b * 8;
        uint8_t in[16] = {};
        uint8_t *out = static_cast<uint8_t *>(IOMallocZero(outSize));
        if (!out) return kIOReturnNoMemory;
        if (!mlxP1EncodeManagePages(in, sizeof(in), MLX_P1_PAGES_TAKE,
                                    (uint16_t)functionId, false, NULL, b)) {
            IOFree(out, outSize);
            MLX_LOG("PAGE_TAKE: encode failed (b=%u)", b);
            return kIOReturnBadArgument;
        }
        kern_return_t kr = s->core->Exec(MLX_CMD_OP_MANAGE_PAGES, in, sizeof(in),
                                         out, outSize, 5000);
        if (kr != kIOReturnSuccess) {
            IOFree(out, outSize);
            MLX_LOG("PAGE_TAKE: cmd failed (b=%u) -> 0x%x", b, kr);
            if (kr == kIOReturnTimeout) QuarantineFunction(functionId);
            if (outCount) *outCount = total;
            return kr;
        }
        uint32_t claimed = 0;
        uint64_t *pas = static_cast<uint64_t *>(IOMallocZero(b * sizeof(uint64_t)));
        uint32_t *slots = static_cast<uint32_t *>(IOMallocZero(b * sizeof(uint32_t)));
        if (!pas || !slots) {
            IOFree(out, outSize);
            if (pas) IOFree(pas, b * sizeof(uint64_t));
            if (slots) IOFree(slots, b * sizeof(uint32_t));
            QuarantineFunction(functionId);
            if (outCount) *outCount = total;
            return kIOReturnNoMemory;
        }
        if (!mlxP1DecodeManagePagesTake(out, outSize, b, &claimed, pas, b)) {
            IOFree(out, outSize); IOFree(pas, b * sizeof(uint64_t));
            IOFree(slots, b * sizeof(uint32_t));
            MLX_LOG("PAGE_TAKE: corrupt response (returned>requested)");
            QuarantineFunction(functionId);
            if (outCount) *outCount = total;
            return kIOReturnIOError;
        }
        IOFree(out, outSize);

        /* First validate the whole response, including alignment and duplicates.
         * No ownership transitions until the full batch is checked. */
        bool valid = true;
        IOLockLock(s->lock);
        for (uint32_t i = 0; i < claimed; i++) {
            if (!pas[i] || (pas[i] & (MLX_FW_PAGE_SIZE_BYTES - 1))) {
                valid = false;
                break;
            }
            slots[i] = MLX_FW_MAX_PAGES;
            for (uint32_t j = 0; j < MLX_FW_MAX_PAGES; j++) {
                if (s->inUse[j] && s->pages[j].state == MLX_FW_PG_GIVEN &&
                    s->pages[j].functionId == functionId &&
                    s->pages[j].iova == pas[i]) {
                    slots[i] = j;
                    break;
                }
            }
            if (slots[i] == MLX_FW_MAX_PAGES) { valid = false; break; }
            for (uint32_t j = 0; j < i; j++) {
                if (slots[j] == slots[i]) { valid = false; break; }
            }
            if (!valid) break;
        }
        if (valid) {
            for (uint32_t i = 0; i < claimed; i++) {
                uint32_t slot = slots[i];
                s->pages[slot].state = MLX_FW_PG_RETURNED;
                if (s->firmwareOwned) s->firmwareOwned--;
                s->returnedCount++;
                for (uint32_t k = 0; k < MLX_FW_MAX_RUNTIME_EXTENTS; k++) {
                    MlxRuntimeExtent &e = s->runtimeExtents[k];
                    uint64_t end = e.iova + (uint64_t)e.numPages * MLX_FW_PAGE_SIZE_BYTES;
                    if (e.inUse && pas[i] >= e.iova && pas[i] < end) {
                        if (e.firmwareOwned) e.firmwareOwned--;
                        break;
                    }
                }
            }
        }
        IOLockUnlock(s->lock);
        IOFree(pas, b * sizeof(uint64_t));
        IOFree(slots, b * sizeof(uint32_t));

        if (!valid) {
            MLX_LOG("PAGE_TAKE: func=%u invalid/duplicate/unknown PAS — quarantine", functionId);
            QuarantineFunction(functionId);
            if (outCount) *outCount = total;
            return kIOReturnIOError;
        }

        MLX_LOG("PAGE_TAKE: func=%u requested=%u returned=%u total_fw_owned=%u ambiguous=%u",
                functionId, b, claimed, GetFirmwareOwned(), GetAmbiguousOwned());
        total += claimed;
        remaining -= claimed;
        if (!claimed) break;       /* no progress: caller decides retry/quarantine */
    }
    if (outCount) *outCount = total;
    return kIOReturnSuccess;
}

kern_return_t
MlxFwPages::ProvidePagesContig(uint32_t numPages, uint8_t ownership, uint32_t functionId)
{
    /* Apple-style: one large DMA buffer, pages at iova + i*0x1000.
     * ownership 1=boot → chunk, 2=init → chunk2, 3=runtime → runtime extent
     * (a new extent per runtime allocation). GIVE in batches of 512. */
    if (!numPages || numPages > MLX_FW_MAX_PAGES)
        return kIOReturnBadArgument;

    MlxRuntimeExtent *rtExt = NULL;
    IOBufferMemoryDescriptor **cmem;
    IODMACommand           **cdma;
    uint64_t               *ciova;
    uint32_t               *cnp;
    bool                   *cmode;
    switch (ownership) {
    case 3: {
        /* Find a free runtime extent — a new request (even smaller than the
         * previous allocation) gets a SEPARATE extent. */
        IOLockLock(s->lock);
        for (uint32_t k = 0; k < MLX_FW_MAX_RUNTIME_EXTENTS; k++) {
            if (!s->runtimeExtents[k].inUse) { rtExt = &s->runtimeExtents[k]; break; }
        }
        IOLockUnlock(s->lock);
        if (!rtExt) {
            MLX_LOG("ProvidePagesContig: no free runtime extent");
            return kIOReturnBusy;
        }
        cmem = &rtExt->mem; cdma = &rtExt->dma; ciova = &rtExt->iova;
        cnp = &rtExt->numPages; cmode = &rtExt->inUse;
        break;
    }
    case 2: cmem = &s->chunkMem2; cdma = &s->chunkDma2; ciova = &s->chunkIOVA2;
            cnp = &s->chunkNumPages2; cmode = &s->chunkMode2; break;
    default: cmem = &s->chunkMem; cdma = &s->chunkDma; ciova = &s->chunkIOVA;
            cnp = &s->chunkNumPages; cmode = &s->chunkMode; break;
    }

    /* First reserve the descriptor slot, but don't hold the bookkeeping lock during
     * DriverKit allocation/prepare/release. */
    IOLockLock(s->lock);
    if (*cmode || s->quarantined) {
        bool quarantined = s->quarantined;
        IOLockUnlock(s->lock);
        MLX_LOG("ProvidePagesContig: chunk/extent (own=%u) already busy", ownership);
        return quarantined ? kIOReturnNotReady : kIOReturnBusy;
    }
    *cmode = true;                 /* reservation */
    *cmem = NULL; *cdma = NULL; *ciova = 0; *cnp = 0;
    if (rtExt) {
        rtExt->functionId = functionId;
        rtExt->firmwareOwned = 0;
        rtExt->ambiguousOwned = 0;
    }
    IOLockUnlock(s->lock);

    uint64_t bufSize = MLX_FW_CHUNK_SIZE;
    uint64_t minSize = numPages * (uint64_t)MLX_FW_PAGE_SIZE_BYTES;
    if (bufSize < minSize)
        bufSize = (minSize + MLX_FW_CHUNK_ALIGN - 1) & ~(MLX_FW_CHUNK_ALIGN - 1);

    MLX_LOG("DBG ProvidePagesContig: alloc bufSize=%llu numPages=%u own=%u",
            (unsigned long long)bufSize, numPages, ownership);

    uint32_t *idx = static_cast<uint32_t *>(IOMallocZero(numPages * sizeof(uint32_t)));
    if (!idx) {
        IOLockLock(s->lock); *cmode = false; IOLockUnlock(s->lock);
        if (ownership == 3) NotifyAllocationFailure(functionId);
        MLX_LOG("ProvidePagesContig: idx alloc failed");
        return kIOReturnNoMemory;
    }

    IOBufferMemoryDescriptor *newMem = NULL;
    IODMACommand *newDma = NULL;
    kern_return_t kr = mlxAllocDmaBuffer(bufSize, MLX_FW_CHUNK_ALIGN,
                                         kIOMemoryDirectionOutIn, &newMem);
    if (kr != kIOReturnSuccess || !newMem) {
        IOFree(idx, numPages * sizeof(uint32_t));
        IOLockLock(s->lock); *cmode = false; IOLockUnlock(s->lock);
        if (ownership == 3) NotifyAllocationFailure(functionId);
        MLX_LOG("ProvidePagesContig: chunk alloc failed: 0x%x", kr);
        return kr ? kr : kIOReturnNoMemory;
    }

    IOAddressSegment segs[32];
    uint32_t segCount = 32;
    kr = mlxPrepareDma(s->pci, newMem, segs, &segCount, &newDma);
    if (kr != kIOReturnSuccess || segCount != 1 || segs[0].length < bufSize ||
        !segs[0].address || (segs[0].address & (MLX_FW_PAGE_SIZE_BYTES - 1))) {
        if (newDma) mlxCompleteDma(newDma);
        newMem->release();
        IOFree(idx, numPages * sizeof(uint32_t));
        IOLockLock(s->lock); *cmode = false; IOLockUnlock(s->lock);
        if (ownership == 3) NotifyAllocationFailure(functionId);
        MLX_LOG("ProvidePagesContig: DMA mapping not contiguous/aligned: kr=0x%x segs=%u len=%llu",
                kr, segCount, segCount ? (unsigned long long)segs[0].length : 0ULL);
        return kr != kIOReturnSuccess ? kr : kIOReturnNoSpace;
    }

    MLX_LOG("DBG ProvidePagesContig: DMA segs=%u seg0={0x%llx, %llu}",
            segCount, (unsigned long long)segs[0].address,
            (unsigned long long)segs[0].length);

    /* Check capacity before mutating slots: rollback stays atomic. */
    IOLockLock(s->lock);
    uint32_t freeSlots = 0;
    for (uint32_t j = 0; j < MLX_FW_MAX_PAGES; j++)
        if (!s->inUse[j]) freeSlots++;
    if (freeSlots < numPages) {
        *cmode = false;
        IOLockUnlock(s->lock);
        mlxCompleteDma(newDma); newMem->release();
        IOFree(idx, numPages * sizeof(uint32_t));
        if (ownership == 3) NotifyAllocationFailure(functionId);
        MLX_LOG("ProvidePagesContig: no page slots (need=%u free=%u)", numPages, freeSlots);
        return kIOReturnNoSpace;
    }
    *cmem = newMem; *cdma = newDma; *ciova = segs[0].address;
    for (uint32_t i = 0; i < numPages; i++) {
        int slot = -1;
        for (uint32_t j = 0; j < MLX_FW_MAX_PAGES; j++)
            if (!s->inUse[j]) { slot = j; break; }
        if (slot < 0) break; /* pre-count above makes this unreachable */
        s->pages[slot].iova = *ciova + i * MLX_FW_PAGE_SIZE_BYTES;
        s->pages[slot].ownership = ownership;
        s->pages[slot].functionId = functionId;
        s->pages[slot].state = MLX_FW_PG_ALLOCATED;
        s->pages[slot].mem = NULL;
        s->pages[slot].dma = NULL;
        s->inUse[slot] = true;
        s->hostAllocated++;
        (*cnp)++;
        idx[i] = slot;
        if (numPages <= 16)
            MLX_LOG("DBG ProvidePagesContig: [%u] slot=%u iova=0x%llx", i, slot,
                    (unsigned long long)s->pages[slot].iova);
    }
    IOLockUnlock(s->lock);

    /* GIVE in batches of 512 (mailbox MLX_CMD_MAX_SIZE=4112). */
    const uint32_t BATCH = 512;
    uint32_t outCount = 0;
    for (uint32_t start = 0; start < numPages; start += BATCH) {
        uint32_t n = (numPages - start < BATCH) ? (numPages - start) : BATCH;
        kr = ManagePages(MLX_P1_PAGES_GIVE, idx + start, n, (uint16_t)functionId, &outCount);
        if (kr != kIOReturnSuccess) {
            MLX_LOG("PAGE_GIVE failed: known_given=%u target=%u err=0x%x", start, numPages, kr);
            /* Safe failure: [0..start) are already given to fw (state=GIVEN) —
             * they cannot be freed without TAKE. Try to return them; if that
             * fails — the whole chunk goes to quarantine (DMA stays pinned). */
            if (kr == kIOReturnTimeout || GetAmbiguousOwned()) {
                EnterQuarantine();
            } else {
                uint32_t taken = 0;
                kern_return_t takeKr = start ? TakePages(functionId, start, &taken)
                                             : kIOReturnSuccess;
                if (takeKr != kIOReturnSuccess || taken < start) {
                    MLX_LOG("PAGE_CHUNK quarantined: known_given=%u taken=%u", start, taken);
                    EnterQuarantine();
                } else {
                    if (ownership == 3) s->rtGiven = 0;
                    ReleaseHostMappings();
                    if (ownership == 3 && start == 0)
                        NotifyAllocationFailure(functionId);
                }
            }
            IOFree(idx, numPages * sizeof(uint32_t));
            return kr;
        }
        IOLockLock(s->lock);
        if (rtExt) rtExt->firmwareOwned = start + n;
        if (ownership == 3) s->rtGiven = start + n;
        IOLockUnlock(s->lock);
        if (ownership == 3) {
            MLX_LOG("PAGE_GIVE: func=%u given=%u target=%u total_fw_owned=%u ambiguous=%u",
                    functionId, start + n, numPages, GetFirmwareOwned(),
                    GetAmbiguousOwned());
        }
    }
    IOFree(idx, numPages * sizeof(uint32_t));

    MLX_LOG("ProvidePagesContig: GIVE %u pages OK (extent 0x%llx, %llu bytes, own=%u)",
            numPages, (unsigned long long)*ciova, (unsigned long long)bufSize, ownership);
    return kIOReturnSuccess;
}

kern_return_t
MlxFwPages::ReclaimAll(uint32_t *outRequested, uint32_t *outReturned)
{
    /* Stage 1: return pages grouped by the first function_id found.
     * This way there is no fixed limit on the number of functions/VFs. */
    if (outRequested) *outRequested = 0;
    if (outReturned) *outReturned = 0;
    if (!s) return kIOReturnBadArgument;
    if (s->quarantined || GetAmbiguousOwned()) {
        MLX_LOG("ReclaimAll: quarantine active — retain");
        return kIOReturnBusy;
    }

    uint32_t totalTaken = 0;
    uint32_t reclaimTarget = GetFirmwareOwned();
    if (outRequested) *outRequested = reclaimTarget;
    uint32_t noProgressMs = 0;
    while (true) {
        uint32_t func = 0;
        uint32_t cnt = 0;
        bool found = false;
        IOLockLock(s->lock);
        for (uint32_t i = 0; i < MLX_FW_MAX_PAGES; i++) {
            if (!s->inUse[i] || s->pages[i].state != MLX_FW_PG_GIVEN) continue;
            if (!found) { func = s->pages[i].functionId; found = true; }
            if (s->pages[i].functionId == func) cnt++;
        }
        IOLockUnlock(s->lock);
        if (!found) break;
        if (cnt > MLX_P1_MAX_MANAGE_PAGES) cnt = MLX_P1_MAX_MANAGE_PAGES;
        uint32_t taken = 0;
        kern_return_t kr = TakePages(func, cnt, &taken);
        totalTaken += taken;
        if (kr != kIOReturnSuccess) {
            MLX_LOG("ReclaimAll: TAKE failed (func=%u owned=%u taken=%u) — quarantine",
                    func, cnt, taken);
            EnterQuarantine();
            s->rtActive = false; s->rtTarget = 0; s->rtGiven = 0;
            if (outReturned) *outReturned = totalTaken;
            return kr;
        }
        if (!taken) {
            if (noProgressMs >= 5000) {
                MLX_LOG("ReclaimAll: TAKE no progress func=%u for %u ms — quarantine",
                        func, noProgressMs);
                EnterQuarantine();
                if (outReturned) *outReturned = totalTaken;
                return kIOReturnTimeout;
            }
            IOSleep(50);
            noProgressMs += 50;
        } else {
            noProgressMs = 0;
        }
    }

    /* Stage 2: free host mappings (non-firmware pages + chunk/extent
     * DMA), only once firmware no longer owns them. */
    s->rtActive = false; s->rtTarget = 0; s->rtGiven = 0;
    kern_return_t kr2 = ReleaseHostMappings();
    bool accountingOk = ValidateAccounting();
    if (outReturned) *outReturned = totalTaken;
    MLX_LOG("ReclaimAll: reclaimed=%u fw_owned=%u ambiguous=%u host=%u returned=%u accounting=%s",
            totalTaken, GetFirmwareOwned(), GetAmbiguousOwned(), GetHostAllocated(),
            GetReturnedCount(), accountingOk ? "ok" : "BROKEN");
    return accountingOk ? kr2 : kIOReturnInternalError;
}

kern_return_t
MlxFwPages::ReleaseHostMappings()
{
    /* Free host pages (state != GIVEN) and chunk/extent DMA only
     * when there are no firmware-owned pages in the corresponding range. */
    if (!s || s->quarantined || GetAmbiguousOwned()) return kIOReturnNotReady;

    /* Detach bookkeeping under lock, DriverKit completion/release — outside. */
    while (true) {
        IOBufferMemoryDescriptor *mem = NULL;
        IODMACommand *dma = NULL;
        bool found = false;
        IOLockLock(s->lock);
        for (uint32_t i = 0; i < MLX_FW_MAX_PAGES; i++) {
            if (!s->inUse[i] || s->pages[i].state == MLX_FW_PG_GIVEN ||
                s->pages[i].state == MLX_FW_PG_GIVE_PENDING ||
                s->pages[i].state == MLX_FW_PG_QUARANTINE) continue;
            mem = s->pages[i].mem;
            dma = s->pages[i].dma;
            if (s->pages[i].state == MLX_FW_PG_ALLOCATED && s->hostAllocated) s->hostAllocated--;
            if (s->pages[i].state == MLX_FW_PG_RETURNED && s->returnedCount) s->returnedCount--;
            memset(&s->pages[i], 0, sizeof(s->pages[i]));
            s->inUse[i] = false;
            found = true;
            break;
        }
        IOLockUnlock(s->lock);
        if (!found) break;
        if (dma) mlxCompleteDma(dma);
        if (mem) mem->release();
    }

    /* Chunk boot (own=1) / init (own=2): free if there are no GIVEN pages. */
    bool bootGiven = false, initGiven = false;
    IOBufferMemoryDescriptor *bootMem = NULL, *initMem = NULL;
    IODMACommand *bootDma = NULL, *initDma = NULL;
    IOLockLock(s->lock);
    for (uint32_t i = 0; i < MLX_FW_MAX_PAGES; i++) {
        if (s->inUse[i] && (s->pages[i].state == MLX_FW_PG_GIVEN ||
                            s->pages[i].state == MLX_FW_PG_GIVE_PENDING ||
                            s->pages[i].state == MLX_FW_PG_QUARANTINE)) {
            if (s->pages[i].ownership == 1) bootGiven = true;
            if (s->pages[i].ownership == 2) initGiven = true;
        }
    }
    if (s->chunkMode && !bootGiven) {
        bootDma = s->chunkDma; bootMem = s->chunkMem;
        s->chunkDma = NULL; s->chunkMem = NULL;
        s->chunkMode = false; s->chunkNumPages = 0; s->chunkIOVA = 0;
    }
    if (s->chunkMode2 && !initGiven) {
        initDma = s->chunkDma2; initMem = s->chunkMem2;
        s->chunkDma2 = NULL; s->chunkMem2 = NULL;
        s->chunkMode2 = false; s->chunkNumPages2 = 0; s->chunkIOVA2 = 0;
    }
    IOLockUnlock(s->lock);
    if (bootDma) mlxCompleteDma(bootDma);
    if (bootMem) bootMem->release();
    if (initDma) mlxCompleteDma(initDma);
    if (initMem) initMem->release();

    /* Runtime extents: free when firmwareOwned == 0 (all pages
     * returned or never given). */
    for (uint32_t k = 0; k < MLX_FW_MAX_RUNTIME_EXTENTS; k++) {
        IOBufferMemoryDescriptor *mem = NULL;
        IODMACommand *dma = NULL;
        uint32_t pages = 0;
        IOLockLock(s->lock);
        MlxRuntimeExtent &e = s->runtimeExtents[k];
        if (!e.inUse) { IOLockUnlock(s->lock); continue; }
        bool hasGiven = false;
        uint64_t eEnd = e.iova + (uint64_t)e.numPages * MLX_FW_PAGE_SIZE_BYTES;
        for (uint32_t i = 0; i < MLX_FW_MAX_PAGES; i++) {
            if (s->inUse[i] && (s->pages[i].state == MLX_FW_PG_GIVEN ||
                                s->pages[i].state == MLX_FW_PG_GIVE_PENDING ||
                                s->pages[i].state == MLX_FW_PG_QUARANTINE) &&
                s->pages[i].iova >= e.iova && s->pages[i].iova < eEnd) {
                hasGiven = true; break;
            }
        }
        if (!hasGiven) {
            dma = e.dma; mem = e.mem; pages = e.numPages;
            memset(&e, 0, sizeof(e));
        }
        IOLockUnlock(s->lock);
        if (dma) mlxCompleteDma(dma);
        if (mem) mem->release();
        if (pages) MLX_LOG("runtime extent freed (pages=%u)", pages);
    }
    return kIOReturnSuccess;
}

void
MlxFwPages::EnterQuarantine()
{
    if (!s) return;
    /* Mark GIVEN pages as ambiguous — don't touch the mapping. */
    IOLockLock(s->lock);
    s->quarantined = true;
    for (uint32_t i = 0; i < MLX_FW_MAX_PAGES; i++) {
        if (!s->inUse[i]) continue;
        if (s->pages[i].state == MLX_FW_PG_GIVEN) {
            s->pages[i].state = MLX_FW_PG_QUARANTINE;
            if (s->firmwareOwned) s->firmwareOwned--;
            s->ambiguousOwned++;
        } else if (s->pages[i].state == MLX_FW_PG_GIVE_PENDING) {
            s->pages[i].state = MLX_FW_PG_QUARANTINE;
        }
    }
    for (uint32_t k = 0; k < MLX_FW_MAX_RUNTIME_EXTENTS; k++) {
        MlxRuntimeExtent &e = s->runtimeExtents[k];
        if (!e.inUse) continue;
        e.ambiguousOwned += e.firmwareOwned;
        e.firmwareOwned = 0;
    }
    uint32_t ambiguous = s->ambiguousOwned;
    IOLockUnlock(s->lock);
    MLX_LOG("quarantine entered — retaining mappings (ambiguous=%u)", ambiguous);
}

void
MlxFwPages::QuarantineFunction(uint32_t functionId)
{
    if (!s) return;
    IOLockLock(s->lock);
    s->quarantined = true;
    for (uint32_t i = 0; i < MLX_FW_MAX_PAGES; i++) {
        if (!s->inUse[i] || s->pages[i].functionId != functionId) continue;
        if (s->pages[i].state == MLX_FW_PG_GIVEN) {
            s->pages[i].state = MLX_FW_PG_QUARANTINE;
            if (s->firmwareOwned) s->firmwareOwned--;
            s->ambiguousOwned++;
        } else if (s->pages[i].state == MLX_FW_PG_GIVE_PENDING) {
            s->pages[i].state = MLX_FW_PG_QUARANTINE;
        }
    }
    IOLockUnlock(s->lock);
    s->core->EnterDmaQuarantine(0x1081000u | (functionId & 0xfffu));
}

void
MlxFwPages::ReleaseQuarantineAfterReset()
{
    if (!s) return;
    IOLockLock(s->lock);
    uint32_t released = 0;
    for (uint32_t i = 0; i < MLX_FW_MAX_PAGES; i++) {
        if (!s->inUse[i] || s->pages[i].state != MLX_FW_PG_QUARANTINE) continue;
        s->pages[i].state = MLX_FW_PG_RETURNED;
        if (s->ambiguousOwned) s->ambiguousOwned--;
        s->returnedCount++;
        released++;
    }
    for (uint32_t k = 0; k < MLX_FW_MAX_RUNTIME_EXTENTS; k++) {
        s->runtimeExtents[k].ambiguousOwned = 0;
        s->runtimeExtents[k].firmwareOwned = 0;
    }
    s->quarantined = false;
    IOLockUnlock(s->lock);
    MLX_LOG("quarantine released after verified reset — pages=%u", released);
}

bool
MlxFwPages::IsQuarantined() const
{
    if (!s) return false;
    IOLockLock(s->lock);
    bool value = s->quarantined || s->ambiguousOwned != 0 || s->firmwareOwned != 0;
    IOLockUnlock(s->lock);
    return value;
}

/* ---- debug/snapshot accessors ---- */

uint32_t
MlxFwPages::GetPageCount() const
{
    if (!s) return 0;
    IOLockLock(s->lock);
    uint32_t c = 0;
    for (uint32_t i = 0; i < MLX_FW_MAX_PAGES; i++)
        if (s->inUse[i]) c++;
    IOLockUnlock(s->lock);
    return c;
}

uint64_t
MlxFwPages::GetPageIOVA(uint32_t index) const
{
    if (!s || index >= MLX_FW_MAX_PAGES || !s->inUse[index]) return 0;
    return s->pages[index].iova;
}

bool
MlxFwPages::IsChunkMode() const
{
    return s ? s->chunkMode : false;
}

uint64_t
MlxFwPages::GetChunkIOVA() const
{
    return s ? s->chunkIOVA : 0;
}

uint32_t
MlxFwPages::GetFirmwareOwned() const
{
    if (!s) return 0;
    IOLockLock(s->lock);
    uint32_t c = 0;
    for (uint32_t i = 0; i < MLX_FW_MAX_PAGES; i++)
        if (s->inUse[i] && s->pages[i].state == MLX_FW_PG_GIVEN) c++;
    IOLockUnlock(s->lock);
    return c;
}

uint32_t
MlxFwPages::GetAmbiguousOwned() const
{
    if (!s) return 0;
    IOLockLock(s->lock);
    uint32_t c = 0;
    for (uint32_t i = 0; i < MLX_FW_MAX_PAGES; i++)
        if (s->inUse[i] && (s->pages[i].state == MLX_FW_PG_GIVE_PENDING ||
                            s->pages[i].state == MLX_FW_PG_QUARANTINE)) c++;
    IOLockUnlock(s->lock);
    return c;
}

uint32_t
MlxFwPages::GetOutstandingOwned() const
{
    return GetFirmwareOwned() + GetAmbiguousOwned();
}

uint32_t
MlxFwPages::GetHostAllocated() const
{
    if (!s) return 0;
    IOLockLock(s->lock);
    uint32_t c = 0;
    for (uint32_t i = 0; i < MLX_FW_MAX_PAGES; i++)
        if (s->inUse[i] && s->pages[i].state == MLX_FW_PG_ALLOCATED) c++;
    IOLockUnlock(s->lock);
    return c;
}

uint32_t
MlxFwPages::GetReturnedCount() const
{
    if (!s) return 0;
    IOLockLock(s->lock);
    uint32_t c = 0;
    for (uint32_t i = 0; i < MLX_FW_MAX_PAGES; i++)
        if (s->inUse[i] && s->pages[i].state == MLX_FW_PG_RETURNED) c++;
    IOLockUnlock(s->lock);
    return c;
}

uint32_t
MlxFwPages::GetQuarantinedCount() const
{
    if (!s) return 0;
    IOLockLock(s->lock);
    uint32_t c = 0;
    for (uint32_t i = 0; i < MLX_FW_MAX_PAGES; i++)
        if (s->inUse[i] && s->pages[i].state == MLX_FW_PG_QUARANTINE) c++;
    IOLockUnlock(s->lock);
    return c;
}

uint32_t
MlxFwPages::GetNegativeTakeRequests() const
{
    if (!s) return 0;
    IOLockLock(s->lock);
    uint32_t value = s->negativeTakeRequests;
    IOLockUnlock(s->lock);
    return value;
}

uint32_t
MlxFwPages::GetNegativeTakePages() const
{
    if (!s) return 0;
    IOLockLock(s->lock);
    uint32_t value = s->negativeTakePages;
    IOLockUnlock(s->lock);
    return value;
}

uint32_t
MlxFwPages::GetNegativeTakeReturned() const
{
    if (!s) return 0;
    IOLockLock(s->lock);
    uint32_t value = s->negativeTakeReturned;
    IOLockUnlock(s->lock);
    return value;
}

bool
MlxFwPages::ValidateAccounting() const
{
    if (!s) return false;
    uint32_t host = 0, fw = 0, returned = 0, ambiguous = 0, invalid = 0;
    IOLockLock(s->lock);
    for (uint32_t i = 0; i < MLX_FW_MAX_PAGES; i++) {
        if (!s->inUse[i]) continue;
        switch (s->pages[i].state) {
        case MLX_FW_PG_ALLOCATED: host++; break;
        case MLX_FW_PG_GIVEN: fw++; break;
        case MLX_FW_PG_RETURNED: returned++; break;
        case MLX_FW_PG_GIVE_PENDING:
        case MLX_FW_PG_QUARANTINE: ambiguous++; break;
        default: invalid++; break;
        }
    }
    bool ok = !invalid && host == s->hostAllocated && fw == s->firmwareOwned &&
              returned == s->returnedCount && ambiguous == s->ambiguousOwned;
    uint32_t shost = s->hostAllocated, sfw = s->firmwareOwned;
    uint32_t sret = s->returnedCount, samb = s->ambiguousOwned;
    IOLockUnlock(s->lock);
    if (!ok)
        MLX_LOG("ACCOUNTING BROKEN: scan host=%u fw=%u returned=%u ambiguous=%u invalid=%u counters=%u/%u/%u/%u",
                host, fw, returned, ambiguous, invalid, shost, sfw, sret, samb);
    return ok;
}
