/*
 * MlxDMA.cpp — DMA/IOMMU mapping (DriverKit port).
 *
 * Ported from: kernel_src/mlnx-ofed-kernel-5.9 core/alloc.c + dma mapping path.
 *
 * DriverKit port: replaces the kernel IODMACommand factory with
 * IODMACommand::Create + PrepareForDMA. Client (app) memory arrives as an
 * IOMemoryDescriptor from IOUserClient::CreateMemoryDescriptorFromClient; the
 * returned IOVA segments (≤32 per call) are split into 4 KiB HCA PAS entries
 * for CREATE_MKEY (host page 16 KiB = 4 PAS, notes/11 §0). The mapping stays
 * alive until Unpin (that is the MR/Q lifetime); ambiguous teardown retains
 * mappings (REMEDIATION_PLAN §3).
 */
#include "MlxDMA.hpp"
#include "MlxDriverKitCompat.h"
#include "MlxPCIDriver.h"
#include "MlxP0Encoding.hpp"   /* mlxAppendMttPages / mlxMttPageCount */
#include "MlxSafety.hpp"
#include "MlxUCIO.h"

#include <DriverKit/IOLib.h>
#include <DriverKit/IOMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#include <PCIDriverKit/IOPCIDevice.h>
#include <string.h>

#include "MlxLog.hpp"
#define MLX_LOG(fmt, ...)  IOLog("MlxDMA: " fmt "\n", ##__VA_ARGS__)
#define MLX_DBG(fmt, ...)  MLX_DBGLOG("MlxDMA: " fmt, ##__VA_ARGS__)

struct MlxDMA::State {
    MlxPCIDriver  *core;
    IOPCIDevice   *pci;
    struct IOLock *lock;
    bool           quarantined;
    uint64_t       pinnedBytes, peakPinnedBytes, pinFailures;
};

MlxDMA::MlxDMA() : s(NULL) {}
MlxDMA::~MlxDMA() { Free(); }

kern_return_t
MlxDMA::Init(MlxPCIDriver *core, IOPCIDevice *pci)
{
    if (!core || !pci) return kIOReturnBadArgument;
    s = new State;
    if (!s) return kIOReturnNoMemory;
    memset(s, 0, sizeof(*s));
    s->core = core;
    s->pci  = pci;
    s->lock = IOLockAlloc();
    if (!s->lock) { delete s; s = NULL; return kIOReturnNoMemory; }
    return kIOReturnSuccess;
}

void
MlxDMA::Free()
{
    if (!s) return;
    if (s->lock) { IOLockFree(s->lock); s->lock = NULL; }
    delete s; s = NULL;
}

kern_return_t
MlxDMA::Pin(IOMemoryDescriptor *mem, MlxDMAReq *req)
{
    if (!s || !mem || !req) return kIOReturnBadArgument;
    if (s->core->DmaQuarantined()) return kIOReturnNotReady;

    uint64_t memLen = 0;
    kern_return_t kr = mem->GetLength(&memLen);
    if (kr != kIOReturnSuccess || memLen == 0) return kIOReturnBadArgument;

    memset(req, 0, sizeof(*req));
    req->memDesc = mem;
    req->len = memLen;
    uint64_t charge = mlxPinnedCharge(memLen, IOVMPageSize);
    IOLockLock(s->lock);
    bool allowed = mlxBytesCanReserve(s->pinnedBytes, charge, MLX_UC_PINNED_BYTES_PER_DEVICE);
    if (allowed) {
        s->pinnedBytes += charge;
        if (s->pinnedBytes > s->peakPinnedBytes) s->peakPinnedBytes = s->pinnedBytes;
        req->chargedBytes = charge;
    } else s->pinFailures++;
    IOLockUnlock(s->lock);
    if (!allowed) return kIOReturnNoResources;

    /* Prepare the whole region for DMA → up to 32 IOVA segments. */
    IOAddressSegment segs[32];
    uint32_t segCount = 32;
    kr = mlxPrepareDma(s->pci, mem, segs, &segCount, &req->dmaCmd);
    if (kr != kIOReturnSuccess) {
        MLX_LOG("pin PrepareForDMA failed: 0x%x", kr);
        Unpin(req);
        return kr;
    }
    if (!mlxDmaSegmentsCover(segs, segCount, memLen)) {
        Unpin(req);
        return kIOReturnNoSpace;
    }
    memcpy(req->segments, segs, segCount * sizeof(segs[0]));
    req->segmentCount = segCount;

    /* Record the IOVA base and whether the segments are one contiguous
     * range — the large-MR fast path (MlxMR::RegMR) needs both. */
    req->iovaBase = segs[0].address;
    req->contiguous = true;
    for (uint32_t i = 1; i < segCount; i++) {
        if (segs[i].address != segs[i - 1].address + segs[i - 1].length) {
            req->contiguous = false;
            break;
        }
    }

    /* Split the IOVA segments into 4 KiB HCA PAS entries (notes/11 §0).
     * mlxAppendMttPages handles the page-boundary walk + dedup, matching the
     * CREATE_MKEY encoder expectation (host-tested in test_all.cpp).
     * Large regions (> 480 x 4 KiB) skip this walk: MlxMR::RegMR coalesces
     * a contiguous IOVA onto coarse MTT pages instead, and the 4 KiB list
     * would overflow MLX_MAX_DMA_PAGES long before a large MR fits. */
    if (memLen <= (uint64_t)MLX_MAX_DMA_PAGES * 4096) {
        uint32_t pageCount = 0;
        if (!mlxBuildSegmentPas(segs, segCount, req->iovaBase, memLen,
                                req->pageDMA, MLX_MAX_DMA_PAGES, &pageCount)) {
                MLX_LOG("PAS split overflow: region exceeds %u pages",
                        MLX_MAX_DMA_PAGES);
                Unpin(req);
                return kIOReturnNoSpace;
        }
        req->numPages = pageCount;
    }
    req->va = 0;       /* set by caller (MR start address) */
    req->len = memLen;

    MLX_DBG("pin len=%llu segs=%u pages=%u iova0=0x%llx contiguous=%d",
            memLen, segCount, req->numPages, segs[0].address, req->contiguous);
    return kIOReturnSuccess;
}

void
MlxDMA::Unpin(MlxDMAReq *req)
{
    if (!req) return;
    bool keep = req->dmaCmd && s && s->core->DmaQuarantined();
    if (keep) {
        /* memDesc is borrowed. Give quarantine its own reference before
         * the caller releases the allocation on an error/unwind path. */
        if (req->memDesc) req->memDesc->retain();
        s->core->RetainDmaUntilReset(req->memDesc, req->dmaCmd, 0x444d4155u);
        req->dmaCmd = NULL;
    }
    if (req->dmaCmd) { mlxCompleteDma(req->dmaCmd); req->dmaCmd = NULL; }
    if (s && req->chargedBytes && !keep) {
        IOLockLock(s->lock);
        s->pinnedBytes -= req->chargedBytes;
        IOLockUnlock(s->lock);
    }
    req->chargedBytes = 0;
    /* memDesc is owned by the caller (IOUserClient created it); we do not
     * release it here. The caller releases it after DEREG_MR. */
    req->memDesc = NULL;
    req->numPages = 0;
}

void MlxDMA::GetPinnedStats(uint64_t *bytes, uint64_t *peak, uint64_t *failures)
{
    if (!s) return;
    IOLockLock(s->lock);
    *bytes = s->pinnedBytes; *peak = s->peakPinnedBytes; *failures = s->pinFailures;
    IOLockUnlock(s->lock);
}

void
MlxDMA::EnterQuarantine(uint32_t reason)
{
    if (!s) return;
    IOLockLock(s->lock);
    s->quarantined = true;
    IOLockUnlock(s->lock);
    MLX_LOG("quarantine: reason=0x%x — retaining mappings", reason);
}
