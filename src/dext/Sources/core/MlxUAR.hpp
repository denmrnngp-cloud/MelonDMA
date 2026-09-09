/*
 * MlxUAR.hpp — UAR (User Access Region) management (generic Mellanox family)
 *
 * Ported from: kernel_src/mlnx-ofed-kernel-5.9/core/uar.c
 *
 * DriverKit port: the BAR aperture that contains UAR/BF is obtained via
 * IOPCIDevice::_CopyDeviceMemoryWithIndex. Per-client UAR subranges are carved
 * out with IOMemoryDescriptor::CreateSubMemoryDescriptor and handed to the app
 * via IOUserClient::CopyClientMemoryForType (kMlxUCMemIndexUar). Device-global
 * UAR stays unmapped until per-client isolation (REMEDIATION_PLAN §7.1).
 */
#ifndef MLX_UAR_HPP
#define MLX_UAR_HPP

#include <stdint.h>
#include <DriverKit/IOReturn.h>
#include <DriverKit/IOMemoryDescriptor.h>
#include "MlxRegs.hpp"
#include "MlxUCIO.h"

class MlxPCIDriver;
class IOPCIDevice;
class IOBufferMemoryDescriptor;
class IODMACommand;

/* One client's private doorbell resources: a small pool of UAR pages and a
 * growable set of doorbell-record pages. Both start with one entry and gain
 * another only when the current ones are exhausted, so a client that creates
 * two QPs still costs exactly what it used to.
 *
 * Offsets handed to the client are flat across the pool: a blue-flame offset
 * is uarSlot * uarPageSize + register offset, and a doorbell-record offset is
 * page * MLX_CLIENT_DB_PAGE_SIZE + slot offset. The client divides by the page
 * size to learn which mapping to use. Inside the DEXT, RingSendDoorbell still
 * takes a firmware UAR index and an offset within that page, so the kernel
 * path never sees a flat offset. */
struct MlxClientDoorbellBundle {
    uint32_t uarCount;
    uint32_t uarIndex[MLX_CLIENT_MAX_UAR];
    IOMemoryDescriptor *uarMemory[MLX_CLIENT_MAX_UAR];
    uint32_t dbPageCount;
    IOBufferMemoryDescriptor *dbMemory[MLX_CLIENT_MAX_DB_PAGES];
    IODMACommand *dbDma[MLX_CLIENT_MAX_DB_PAGES];
    uint64_t dbIOVA[MLX_CLIENT_MAX_DB_PAGES];
    volatile uint8_t *dbCpu[MLX_CLIENT_MAX_DB_PAGES];
    uint32_t dbSlotBitmap[MLX_CLIENT_MAX_DB_PAGES];
};

class MlxUAR {
public:
    MlxUAR();
    ~MlxUAR();

    /* uarPageSize is the BAR stride between UAR pages, in bytes: what
     * mlxUarPageSizeBytes() derives from the uar_4k / log_uar_page_sz pair the
     * driver negotiated in SET_HCA_CAP. */
    kern_return_t   Init(MlxPCIDriver *core, IOPCIDevice *pci, uint8_t barIndex,
                         uint32_t uarPageSize);
    void            Free();

    /* Allocate a UAR page number from firmware (ALLOC_UAR). */
    kern_return_t   AllocUAR(uint32_t *uarIdx);
    kern_return_t   FreeUAR(uint32_t uarIdx);
    void            MarkFirmwareResourcesDestroyedByTeardown();

    /* The BAR memory index + offset where the UAR page lives. */
    uint8_t         BarIndex() const;
    uintptr_t       UarOffset(uint32_t uarIdx) const;
    uint32_t        UarPageSize() const;
    uint32_t        GetBootUarIndex() const;
    uint64_t        GetDbRecordDMA() const;
    /* Number of DB-record slots backed by this provider's current UAR page. */
    uint32_t        GetDbSlotCapacity() const;
    /* Allocate an isolated 128-byte DB-record slot. The spacing is
     * conservative; CREATE_QP itself only requires the PRM alignment. */
    kern_return_t   AllocDbSlot(uint64_t *outDMA, uint32_t *outOffset = NULL);
    void            FreeDbSlot(uint32_t offset);
    volatile uint32_t *GetDbRecord(uint32_t offset);
    /* Detach the DB page without completing DMA. Used only when a firmware
     * object destroy is unverified; caller retains it until a verified FLR. */
    void            QuarantineDbPage(IOBufferMemoryDescriptor **mem,
                                     IODMACommand **dma);

    /* Ring a send-queue BlueFlame doorbell through DriverKit MMIO. */
    kern_return_t   RingSendDoorbell(uint32_t uarIdx, uint32_t bfOffset,
                                     uint64_t value);
    /* Ring the mlx5 CQ arm doorbell: arm word followed by CQN. */
    kern_return_t   RingCQDoorbell(uint32_t uarIdx, uint32_t armWord,
                                   uint32_t cqn);

    /* Subrange for a per-client UAR mapping (future Option A). */
    kern_return_t   CreateClientSubrange(uint32_t uarIdx,
                                         IOMemoryDescriptor **out);
    kern_return_t   AllocClientDbPage(MlxClientDoorbellBundle *bundle,
                                     uint32_t page);
    kern_return_t   AllocClientBundle(MlxClientDoorbellBundle *bundle);
    void            FreeClientBundle(MlxClientDoorbellBundle *bundle);
    /* Add one more UAR page to the pool and return its slot. Refused with
     * kIOReturnNoSpace once the pool is full; the caller then shares an
     * existing blue-flame register instead, which is slower but correct. */
    kern_return_t   AllocClientUar(MlxClientDoorbellBundle *bundle,
                                   uint32_t *outSlot);
    uint32_t        ClientUarCount(const MlxClientDoorbellBundle *bundle) const;
    uint32_t        ClientUarIndex(const MlxClientDoorbellBundle *bundle,
                                   uint32_t slot) const;
    IOMemoryDescriptor *ClientUarMemory(const MlxClientDoorbellBundle *bundle,
                                        uint32_t slot) const;
    /* outOffset is flat across the client's doorbell pages. A new page is
     * allocated when every existing one is full. */
    kern_return_t   AllocClientDbSlot(MlxClientDoorbellBundle *bundle,
                                      uint64_t *outDMA, uint32_t *outOffset);
    void            FreeClientDbSlot(MlxClientDoorbellBundle *bundle,
                                     uint32_t offset);
    volatile uint32_t *GetClientDbRecord(MlxClientDoorbellBundle *bundle,
                                         uint32_t offset);
    IOBufferMemoryDescriptor *ClientDbMemory(
        const MlxClientDoorbellBundle *bundle, uint32_t page) const;

private:
    struct State;
    State *s;
};

#endif /* MLX_UAR_HPP */
