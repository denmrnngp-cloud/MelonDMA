/*
 * MlxDMA.hpp — DMA/IOMMU mapping (generic Mellanox mlx5 family)
 *
 * Ported from: kernel_src/mlnx-ofed-kernel-5.9 core/alloc.c + dma mapping path.
 *
 * DriverKit port: replaces kernel IODMACommand factory with
 * IODMACommand::Create + PrepareForDMA on the client (app) memory descriptor
 * obtained via IOUserClient::CreateMemoryDescriptorFromClient. The returned
 * IOVA segments (≤32 per call) are split into 4 KiB HCA PAS entries for
 * CREATE_MKEY (host page 16 KiB = 4 PAS, notes/11 §0).
 */
#ifndef MLX_DMA_HPP
#define MLX_DMA_HPP

#include <stdint.h>
#include <DriverKit/IOReturn.h>
#include <DriverKit/IOMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#include <DriverKit/IOLib.h>

class MlxPCIDriver;
class IOPCIDevice;

#define MLX_MAX_DMA_PAGES 480

/* One pending pin request tracked in the DMA manager. */
struct MlxDMAReq {
    uint64_t            va;         /* MR start address (set by caller) */
    uint64_t            len;        /* length in bytes */
    IOMemoryDescriptor *memDesc;    /* created from client ranges (caller-owned) */
    IODMACommand      *dmaCmd;      /* retained IOMMU mapping */
    uint64_t            pageDMA[MLX_MAX_DMA_PAGES]; /* 4 KiB HCA PAS entries */
    uint32_t            numPages;
};

class MlxDMA {
public:
    MlxDMA();
    ~MlxDMA();

    kern_return_t   Init(MlxPCIDriver *core, IOPCIDevice *pci);
    void            Free();

    /* Pin client memory for DMA (See mlx5_ib_reg_user_mr → ib_umem_get). */
    kern_return_t   Pin(IOMemoryDescriptor *mem, MlxDMAReq *req);
    void            Unpin(MlxDMAReq *req);

    /* Look up the IOVA of a client VA (for post_send data segment). */
    uint64_t        LookupPhys(MlxDMAReq *req, uint64_t va);

    /* DMA quarantine: when firmware teardown cannot be trusted, retain the
     * mappings rather than freeing them (REMEDIATION_PLAN §3). */
    void            EnterQuarantine(uint32_t reason);

private:
    struct State;
    State *s;
};

#endif /* MLX_DMA_HPP */
