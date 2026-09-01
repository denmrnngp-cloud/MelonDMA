/*
 * MlxFwPages.hpp — Firmware page management (generic Mellanox mlx5 family)
 *
 * Ported from: kernel_src/mlnx-ofed-kernel-5.9/core/pagealloc.c
 *
 * DriverKit port: boot/init/runtime pages are provided to firmware via
 * MANAGE_PAGES. Each page descriptor is an IOBufferMemoryDescriptor pinned with
 * IODMACommand; the IOVA goes into the command. Firmware-owned pages retain
 * their DMA mappings until TAKE/release-all or a trusted graceful teardown
 * (REMEDIATION_PLAN §3, §5.2: ambiguous ownership is quarantined, not freed).
 *
 * v0.35+: added ProvidePagesContig (Apple-style single 128 KiB DMA buffer)
 * for boot pages, and debug accessors for mlx_probe (notes/35).
 */
#ifndef MLX_FW_PAGES_HPP
#define MLX_FW_PAGES_HPP

#include <stdint.h>
#include <DriverKit/IOReturn.h>
#include <DriverKit/IOMemoryDescriptor.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#include <DriverKit/OSArray.h>
#include <DriverKit/IOLib.h>
#include "MlxRegs.hpp"

class MlxPCIDriver;

#define MLX_FW_PAGE_SIZE_BYTES  4096
#define MLX_FW_MAX_PAGES       16384
#define MLX_FW_CHUNK_SIZE      0x20000   /* 128 KiB — Apple FwPage buffer */
#define MLX_FW_CHUNK_ALIGN     0x4000    /* 16 KiB alignment */
#define MLX_FW_MAX_RUNTIME_EXTENTS 8

/* Page ownership state */
enum {
    MLX_FW_PG_ALLOCATED  = 0,   /* host-allocated, not given to fw */
    MLX_FW_PG_GIVE_PENDING = 1, /* GIVE sent, result not yet known */
    MLX_FW_PG_GIVEN      = 2,   /* given to fw (MANAGE_PAGES GIVE ok) */
    MLX_FW_PG_RETURNED   = 3,   /* returned by fw (TAKE, PAS matched) */
    MLX_FW_PG_QUARANTINE = 4,   /* ambiguous ownership — retain */
};

struct MlxFwPage {
    uint32_t               index;       /* firmware page index */
    uint32_t               functionId;
    uint8_t                ownership;   /* boot/init/runtime/own */
    uint8_t                state;       /* MLX_FW_PG_* */
    IOBufferMemoryDescriptor *mem;     /* 4 KiB page (NULL for chunk pages) */
    IODMACommand          *dma;        /* retained IOMMU mapping (NULL for chunk) */
    uint64_t               iova;
};

/* Runtime DMA extent — one large buffer for runtime allocation. */
struct MlxRuntimeExtent {
    uint32_t               functionId;
    uint64_t               iova;
    uint32_t               numPages;
    uint32_t               firmwareOwned;   /* how many of numPages are with fw */
    uint32_t               ambiguousOwned;  /* GIVE timeout / protocol fault */
    IOBufferMemoryDescriptor *mem;
    IODMACommand          *dma;
    bool                   inUse;
};

class MlxFwPages {
public:
    MlxFwPages();
    ~MlxFwPages();

    kern_return_t   Init(MlxPCIDriver *core);
    void            Free();

    /* Query boot/init pages and provide them to firmware in OFED order.
     * queryMode: 1 = BOOT, 2 = INIT (QUERY_PAGES op_mod). */
    kern_return_t   QueryStartupPages(uint16_t queryMode, uint32_t *outNumPages);
    /* Extended: returns function_id from the fw response (notes/35). */
    kern_return_t   QueryStartupPagesFull(uint16_t queryMode, uint32_t *outNumPages,
                                          uint32_t *outFunctionId);

    /* Provide pages via MANAGE_PAGES(GIVE):
     *   ProvidePages      — separate 4 KiB IOBMD per page (current behaviour)
     *   ProvidePagesContig — single 128 KiB DMA buffer, Apple-style (notes/35)
     */
    kern_return_t   ProvidePages(uint32_t numPages, uint8_t ownership,
                                 uint32_t functionId = 0);
    kern_return_t   ProvidePagesContig(uint32_t numPages, uint8_t ownership,
                                       uint32_t functionId = 0);

    /* Runtime PAGE_REQUEST handler (positive numPages=GIVE,
     * negative=TAKE/reclaim). Returns kIOReturnSuccess if handled. */
    kern_return_t   HandleRuntimePageRequest(uint32_t functionId, int32_t numPages);

    /* Reclaim all pages during teardown: TAKE firmware pages, then free the
     * host mappings. Returns an error if reclaim is incomplete (quarantine). */
    kern_return_t   ReclaimAll(uint32_t *outRequested = NULL,
                               uint32_t *outReturned = NULL);
    void            EnterQuarantine();
    /* Allow release only after a confirmed DMA boundary (FLR/reset). */
    void            ReleaseQuarantineAfterReset();
    bool            IsQuarantined() const;

    /* Real TAKE: ask fw to return numPages pages, match the returned PAS
     * against bookkeeping, mark RETURNED. */
    kern_return_t   TakePages(uint32_t functionId, uint32_t numPages,
                              uint32_t *outCount);
    /* Free only host pages not owned by fw (state != GIVEN). */
    kern_return_t   ReleaseHostMappings();

    /* Page accounting (for diagnostics). */
    uint32_t        GetFirmwareOwned() const;
    uint32_t        GetAmbiguousOwned() const;
    uint32_t        GetOutstandingOwned() const;
    uint32_t        GetHostAllocated() const;
    uint32_t        GetReturnedCount() const;
    uint32_t        GetQuarantinedCount() const;
    uint32_t        GetNegativeTakeRequests() const;
    uint32_t        GetNegativeTakePages() const;
    uint32_t        GetNegativeTakeReturned() const;
    bool            ValidateAccounting() const;

    /* Debug/snapshot accessors (for mlx_probe, notes/35). */
    uint32_t        GetPageCount() const;
    uint64_t        GetPageIOVA(uint32_t index) const;
    bool            IsChunkMode() const;
    uint64_t        GetChunkIOVA() const;

private:
    struct State;
    State *s;
    kern_return_t   AllocPage(uint32_t *outIndex);
    void            ReleasePage(uint32_t index);
    kern_return_t   ManagePages(uint16_t op, uint32_t *indices,
                                uint32_t count, uint16_t functionId,
                                uint32_t *outCount);
    kern_return_t   NotifyAllocationFailure(uint32_t functionId);
    void            QuarantineFunction(uint32_t functionId);
};

#endif /* MLX_FW_PAGES_HPP */
