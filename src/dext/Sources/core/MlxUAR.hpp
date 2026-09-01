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

class MlxPCIDriver;
class IOPCIDevice;
class IOBufferMemoryDescriptor;
class IODMACommand;

struct MlxClientDoorbellBundle {
    uint32_t uarIndex;
    IOMemoryDescriptor *uarMemory;
    IOBufferMemoryDescriptor *dbMemory;
    IODMACommand *dbDma;
    uint64_t dbIOVA;
    volatile uint8_t *dbCpu;
    uint32_t dbSlotBitmap;
};

class MlxUAR {
public:
    MlxUAR();
    ~MlxUAR();

    kern_return_t   Init(MlxPCIDriver *core, IOPCIDevice *pci, uint8_t barIndex,
                         uint16_t logUarPageSize, bool uar4k);
    void            Free();

    /* Allocate a UAR page number from firmware (ALLOC_UAR). */
    kern_return_t   AllocUAR(uint32_t *uarIdx);
    kern_return_t   FreeUAR(uint32_t uarIdx);
    void            MarkFirmwareResourcesDestroyedByTeardown();

    /* The BAR memory index + offset where the UAR page lives. */
    uint8_t         BarIndex() const;
    uintptr_t       UarOffset(uint32_t uarIdx) const;
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
    kern_return_t   AllocClientBundle(MlxClientDoorbellBundle *bundle);
    void            FreeClientBundle(MlxClientDoorbellBundle *bundle);
    kern_return_t   AllocClientDbSlot(MlxClientDoorbellBundle *bundle,
                                      uint64_t *outDMA, uint32_t *outOffset);
    void            FreeClientDbSlot(MlxClientDoorbellBundle *bundle,
                                     uint32_t offset);
    volatile uint32_t *GetClientDbRecord(MlxClientDoorbellBundle *bundle,
                                         uint32_t offset);

private:
    struct State;
    State *s;
};

#endif /* MLX_UAR_HPP */
