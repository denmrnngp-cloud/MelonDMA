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

    uint64_t memLen = 0;
    kern_return_t kr = mem->GetLength(&memLen);
    if (kr != kIOReturnSuccess || memLen == 0) return kIOReturnBadArgument;

    memset(req, 0, sizeof(*req));
    req->memDesc = mem;

    /* Prepare the whole region for DMA → up to 32 IOVA segments. */
    IOAddressSegment segs[32];
    uint32_t segCount = 32;
    kr = mlxPrepareDma(s->pci, mem, segs, &segCount, &req->dmaCmd);
    if (kr != kIOReturnSuccess) {
        MLX_LOG("pin PrepareForDMA failed: 0x%x", kr);
        return kr;
    }
    if (segCount == 0) {
        mlxCompleteDma(req->dmaCmd); req->dmaCmd = NULL;
        return kIOReturnNoSpace;
    }

    /* Split the IOVA segments into 4 KiB HCA PAS entries (notes/11 §0).
     * mlxAppendMttPages handles the page-boundary walk + dedup, matching the
     * CREATE_MKEY encoder expectation (host-tested in test_all.cpp). */
    uint32_t pageCount = 0;
    for (uint32_t i = 0; i < segCount && pageCount < MLX_MAX_DMA_PAGES; i++) {
        if (!mlxAppendMttPages(segs[i].address, segs[i].length,
                               req->pageDMA, MLX_MAX_DMA_PAGES, &pageCount)) {
            MLX_LOG("PAS split overflow: region exceeds %u pages",
                    MLX_MAX_DMA_PAGES);
            mlxCompleteDma(req->dmaCmd); req->dmaCmd = NULL;
            return kIOReturnNoSpace;
        }
    }
    req->numPages = pageCount;
    req->va = 0;       /* set by caller (MR start address) */
    req->len = memLen;

    MLX_LOG("pin len=%llu segs=%u pages=%u iova0=0x%llx",
            memLen, segCount, pageCount, segs[0].address);
    return kIOReturnSuccess;
}

void
MlxDMA::Unpin(MlxDMAReq *req)
{
    if (!req) return;
    if (req->dmaCmd) { mlxCompleteDma(req->dmaCmd); req->dmaCmd = NULL; }
    /* memDesc is owned by the caller (IOUserClient created it); we do not
     * release it here. The caller releases it after DEREG_MR. */
    req->memDesc = NULL;
    req->numPages = 0;
}

uint64_t
MlxDMA::LookupPhys(MlxDMAReq *req, uint64_t va)
{
    if (!req || !req->dmaCmd) return 0;
    /* The IOVA segments are retained by the IODMACommand. For v1 we walk the
     * recorded pageDMA[] (4 KiB pages) to locate the page covering va. This
     * is what post_send's data segment needs (kext donor lookupPhys). */
    if (va < req->va || va >= req->va + req->len) return 0;
    uint64_t off = va - req->va;
    uint32_t pageIdx = (uint32_t)(off / MLX_MTT_PAGE_SIZE);
    if (pageIdx >= req->numPages) return 0;
    return req->pageDMA[pageIdx] + (off & (MLX_MTT_PAGE_SIZE - 1));
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
