/*
 * MlxUAR.cpp — UAR (User Access Region) management (DriverKit port).
 *
 * Ported from: kernel_src/mlnx-ofed-kernel-5.9/core/uar.c. The BAR aperture
 * containing UAR/BF is obtained via IOPCIDevice::_CopyDeviceMemoryWithIndex.
 * Per-client UAR subranges are carved with IOMemoryDescriptor::
 * CreateSubMemoryDescriptor and handed to the app via CopyClientMemoryForType
 * (kMlxUCMemIndexUar). Device-global UAR stays unmapped until per-client
 * isolation (REMEDIATION_PLAN §7.1).
 */
#include "MlxUAR.hpp"
#include "MlxDriverKitCompat.h"
#include "MlxRegs.hpp"
#include "MlxPCIDriver.h"

#include <DriverKit/IOLib.h>
#include <DriverKit/IOMemoryDescriptor.h>
#include <PCIDriverKit/IOPCIDevice.h>
#include "MlxIfcHelpers.hpp"   /* mlxSetBits / mlxGetBits */

#include "MlxLog.hpp"
#define MLX_LOG(fmt, ...)  IOLog("MlxUAR: " fmt "\n", ##__VA_ARGS__)
#define MLX_DBG(fmt, ...)  MLX_DBGLOG("MlxUAR: " fmt, ##__VA_ARGS__)

struct MlxUAR::State {
    MlxPCIDriver    *core;
    IOPCIDevice     *pci;
    IOMemoryDescriptor *barMem;
    uint8_t          barIndex;
    uint16_t         logUarPageSize;
    bool             uar4k;
    uint32_t         nextUarIdx;
    uint32_t         bootUarIdx;
    IOBufferMemoryDescriptor *dbRecordMem;
    IODMACommand          *dbRecordDma;
    uint64_t               dbRecordIOVA;
    volatile uint8_t       *dbRecordAddr;
    uint32_t               dbSlotBitmap;
    struct IOLock   *lock;
};

MlxUAR::MlxUAR() : s(NULL) {}
MlxUAR::~MlxUAR() { Free(); }

kern_return_t
MlxUAR::Init(MlxPCIDriver *core, IOPCIDevice *pci, uint8_t barIndex,
             uint16_t logUarPageSize, bool uar4k)
{
    if (!core || !pci) return kIOReturnBadArgument;
    s = new State;
    if (!s) return kIOReturnNoMemory;
    memset(s, 0, sizeof(*s));
    s->core = core;
    s->pci  = pci;
    s->barIndex = barIndex;
    s->logUarPageSize = logUarPageSize;
    s->uar4k = uar4k;
    s->nextUarIdx = 1;
    s->bootUarIdx = 0;
    s->dbRecordMem = NULL;
    s->dbRecordDma = NULL;
    s->dbRecordIOVA = 0;
    s->dbRecordAddr = NULL;
    s->dbSlotBitmap = 0;
    s->lock = IOLockAlloc();
    if (!s->lock) { delete s; s = NULL; return kIOReturnNoMemory; }

    /* DB record page: one 4 KiB DMA page for CQ/QP DB records (MVP). */
    kern_return_t dkr = mlxAllocDmaBuffer(4096, 4096, kIOMemoryDirectionOutIn,
                                         &s->dbRecordMem);
    if (dkr == kIOReturnSuccess && s->dbRecordMem) {
        IOAddressSegment segs[32];
        uint32_t segCount = 32;
        dkr = mlxPrepareDma(s->pci, s->dbRecordMem, segs, &segCount, &s->dbRecordDma);
        if (dkr == kIOReturnSuccess && segCount > 0) {
            s->dbRecordIOVA = segs[0].address;
            uint64_t addr = 0, len = 0;
            dkr = s->dbRecordMem->Map(0, 0, 0, 0, &addr, &len);
            if (dkr == kIOReturnSuccess && len >= 4096) {
                s->dbRecordAddr = (volatile uint8_t *)(uintptr_t)addr;
                memset((void *)(uintptr_t)addr, 0, 4096);
            }
        }
    }
    if (dkr != kIOReturnSuccess || !s->dbRecordIOVA || !s->dbRecordAddr) {
        if (s->dbRecordDma) { mlxCompleteDma(s->dbRecordDma); s->dbRecordDma = NULL; }
        if (s->dbRecordMem) { s->dbRecordMem->release(); s->dbRecordMem = NULL; }
        IOLockFree(s->lock); delete s; s = NULL;
        return dkr ? dkr : kIOReturnNoMemory;
    }

    /* Try to get the BAR aperture descriptor for sub-range mapping. */
    kern_return_t kr = pci->_CopyDeviceMemoryWithIndex(barIndex, &s->barMem, core);
    if (kr != kIOReturnSuccess || !s->barMem) {
        MLX_LOG("BAR aperture unavailable (0x%x) — MMIO only", kr);
        s->barMem = NULL;
    }
    return kIOReturnSuccess;
}

void
MlxUAR::Free()
{
    if (!s) return;
    if (s->barMem) { s->barMem->release(); s->barMem = NULL; }
    if (s->dbRecordDma) { mlxCompleteDma(s->dbRecordDma); s->dbRecordDma = NULL; }
    if (s->dbRecordMem) { s->dbRecordMem->release(); s->dbRecordMem = NULL; }
    if (s->lock)  { IOLockFree(s->lock); s->lock = NULL; }
    delete s; s = NULL;
}

kern_return_t
MlxUAR::AllocUAR(uint32_t *uarIdx)
{
    if (!s || !uarIdx) return kIOReturnBadArgument;
    /* ALLOC_UAR (0x802): firmware allocates a UAR page number. */
    uint8_t in[16] = {};
    uint8_t out[16] = {};
    mlxSetBits(in, 0x00, 16, MLX_CMD_OP_ALLOC_UAR);
    kern_return_t kr = s->core->Exec(MLX_CMD_OP_ALLOC_UAR, in, sizeof(in),
                                      out, sizeof(out), 5000);
    if (kr != kIOReturnSuccess) return kr;
    *uarIdx = (uint32_t)mlxGetBits(out, 0x48, 24);
    if (!s->bootUarIdx) s->bootUarIdx = *uarIdx;
    MLX_LOG("UAR[%u] allocated", *uarIdx);
    return kIOReturnSuccess;
}

kern_return_t
MlxUAR::FreeUAR(uint32_t uarIdx)
{
    if (!s) return kIOReturnBadArgument;
    uint8_t in[16] = {};
    uint8_t out[16] = {};
    mlxSetBits(in, 0x00, 16, MLX_CMD_OP_FREE_UAR);
    mlxSetBits(in, 0x48, 24, uarIdx);
    return s->core->Exec(MLX_CMD_OP_FREE_UAR, in, sizeof(in),
                         out, sizeof(out), 5000);
}

void
MlxUAR::MarkFirmwareResourcesDestroyedByTeardown()
{
    if (!s) return;
    IOLockLock(s->lock);
    s->bootUarIdx = 0;
    IOLockUnlock(s->lock);
}

uint8_t
MlxUAR::BarIndex() const { return s ? s->barIndex : 0; }

uint32_t
MlxUAR::GetBootUarIndex() const { return s ? s->bootUarIdx : 0; }

uint64_t
MlxUAR::GetDbRecordDMA() const { return s ? s->dbRecordIOVA : 0; }

uint32_t
MlxUAR::GetDbSlotCapacity() const
{
    return s && s->dbRecordMem ? 4096u / 128u : 0;
}

kern_return_t
MlxUAR::AllocDbSlot(uint64_t *outDMA, uint32_t *outOffset)
{
    if (!s || !outDMA) return kIOReturnBadArgument;
    IOLockLock(s->lock);
    uint32_t slot = 32;
    for (uint32_t i = 0; i < 32; i++) {
        if (!(s->dbSlotBitmap & (1u << i))) { slot = i; break; }
    }
    if (slot == 32) { IOLockUnlock(s->lock); return kIOReturnNoSpace; }
    s->dbSlotBitmap |= 1u << slot;
    uint32_t offset = slot * 128;
    memset((void *)(uintptr_t)(s->dbRecordAddr + offset), 0, 128);
    *outDMA = s->dbRecordIOVA + offset;
    if (outOffset) *outOffset = offset;
    IOLockUnlock(s->lock);
    return kIOReturnSuccess;
}

void
MlxUAR::FreeDbSlot(uint32_t offset)
{
    if (!s || (offset & 127) || offset >= 4096) return;
    uint32_t slot = offset / 128;
    IOLockLock(s->lock);
    memset((void *)(uintptr_t)(s->dbRecordAddr + offset), 0, 128);
    s->dbSlotBitmap &= ~(1u << slot);
    IOLockUnlock(s->lock);
}

volatile uint32_t *
MlxUAR::GetDbRecord(uint32_t offset)
{
    if (!s || !s->dbRecordAddr || (offset & 127) || offset >= 4096)
        return NULL;
    return (volatile uint32_t *)(s->dbRecordAddr + offset);
}

void
MlxUAR::QuarantineDbPage(IOBufferMemoryDescriptor **mem, IODMACommand **dma)
{
    if (mem) *mem = NULL;
    if (dma) *dma = NULL;
    if (!s) return;
    IOLockLock(s->lock);
    if (mem) *mem = s->dbRecordMem;
    if (dma) *dma = s->dbRecordDma;
    s->dbRecordMem = NULL;
    s->dbRecordDma = NULL;
    s->dbRecordIOVA = 0;
    s->dbRecordAddr = NULL;
    s->dbSlotBitmap = 0;
    IOLockUnlock(s->lock);
}

kern_return_t
MlxUAR::RingSendDoorbell(uint32_t uarIdx, uint32_t bfOffset, uint64_t value)
{
    if (!s || !s->pci || bfOffset + sizeof(value) >
        (s->uar4k ? 4096u : (1u << s->logUarPageSize)))
        return kIOReturnBadArgument;
    mlxMemoryBarrier();
    s->pci->MemoryWrite64(s->barIndex,
                          (uint64_t)UarOffset(uarIdx) + bfOffset, value);
    mlxMemoryBarrier();
    return kIOReturnSuccess;
}

kern_return_t
MlxUAR::RingCQDoorbell(uint32_t uarIdx, uint32_t armWord, uint32_t cqn)
{
    if (!s || !s->pci || MLX_CQ_DOORBELL + sizeof(uint64_t) >
        (s->uar4k ? 4096u : (1u << s->logUarPageSize)))
        return kIOReturnBadArgument;
    uint32_t words[2] = {
        OSSwapHostToBigInt32(armWord),
        OSSwapHostToBigInt32(cqn & 0xffffffu)
    };
    uint64_t doorbell = 0;
    memcpy(&doorbell, words, sizeof(doorbell));
    mlxMemoryBarrier();
    s->pci->MemoryWrite64(s->barIndex,
                          (uint64_t)UarOffset(uarIdx) + MLX_CQ_DOORBELL,
                          doorbell);
    mlxMemoryBarrier();
    return kIOReturnSuccess;
}

uintptr_t
MlxUAR::UarOffset(uint32_t uarIdx) const
{
    /* UAR pages are at the BAR offset: uarIdx * (uar4k ? 4096 : system_page). */
    uint32_t stride = s->uar4k ? 4096 : (1u << s->logUarPageSize);
    return (uintptr_t)uarIdx * stride;
}

kern_return_t
MlxUAR::CreateClientSubrange(uint32_t uarIdx, IOMemoryDescriptor **out)
{
    if (!s || !s->barMem || !out) return kIOReturnBadArgument;
    uint64_t off = UarOffset(uarIdx);
    uint64_t len = s->uar4k ? 4096 : (1u << s->logUarPageSize);
    return IOMemoryDescriptor::CreateSubMemoryDescriptor(0, off, len,
                                                         s->barMem, out);
}

kern_return_t
MlxUAR::AllocClientBundle(MlxClientDoorbellBundle *bundle)
{
    if (!s || !bundle) return kIOReturnBadArgument;
    memset(bundle, 0, sizeof(*bundle));
    kern_return_t kr = AllocUAR(&bundle->uarIndex);
    if (kr != kIOReturnSuccess) return kr;
    kr = CreateClientSubrange(bundle->uarIndex, &bundle->uarMemory);
    if (kr != kIOReturnSuccess || !bundle->uarMemory) {
        (void)FreeUAR(bundle->uarIndex);
        memset(bundle, 0, sizeof(*bundle));
        return kr ? kr : kIOReturnNoMemory;
    }
    kr = mlxAllocDmaBuffer(4096, 4096, kIOMemoryDirectionOutIn,
                           &bundle->dbMemory);
    IOAddressSegment segs[4];
    uint32_t segCount = 4;
    if (kr == kIOReturnSuccess && bundle->dbMemory)
        kr = mlxPrepareDma(s->pci, bundle->dbMemory, segs, &segCount,
                           &bundle->dbDma);
    uint64_t cpu = 0, length = 0;
    if (kr == kIOReturnSuccess && segCount == 1 && segs[0].length >= 4096) {
        bundle->dbIOVA = segs[0].address;
        kr = bundle->dbMemory->Map(0, 0, 0, 0, &cpu, &length);
    }
    if (kr != kIOReturnSuccess || !bundle->dbIOVA || length < 4096) {
        FreeClientBundle(bundle);
        return kr ? kr : kIOReturnNoSpace;
    }
    bundle->dbCpu = (volatile uint8_t *)(uintptr_t)cpu;
    memset((void *)(uintptr_t)bundle->dbCpu, 0, 4096);
    MLX_LOG("client bundle allocated uar=%u db=0x%llx",
            bundle->uarIndex, (unsigned long long)bundle->dbIOVA);
    return kIOReturnSuccess;
}

void
MlxUAR::FreeClientBundle(MlxClientDoorbellBundle *bundle)
{
    if (!s || !bundle) return;
    if (bundle->dbDma) { mlxCompleteDma(bundle->dbDma); bundle->dbDma = NULL; }
    if (bundle->dbMemory) { bundle->dbMemory->release(); bundle->dbMemory = NULL; }
    if (bundle->uarMemory) { bundle->uarMemory->release(); bundle->uarMemory = NULL; }
    if (bundle->uarIndex) (void)FreeUAR(bundle->uarIndex);
    memset(bundle, 0, sizeof(*bundle));
}

kern_return_t
MlxUAR::AllocClientDbSlot(MlxClientDoorbellBundle *bundle, uint64_t *outDMA,
                          uint32_t *outOffset)
{
    if (!s || !bundle || !bundle->dbCpu || !outDMA || !outOffset)
        return kIOReturnBadArgument;
    IOLockLock(s->lock);
    uint32_t slot = 32;
    for (uint32_t i = 0; i < 32; i++)
        if (!(bundle->dbSlotBitmap & (1u << i))) { slot = i; break; }
    if (slot == 32) { IOLockUnlock(s->lock); return kIOReturnNoSpace; }
    bundle->dbSlotBitmap |= 1u << slot;
    *outOffset = slot * 128;
    *outDMA = bundle->dbIOVA + *outOffset;
    memset((void *)(uintptr_t)(bundle->dbCpu + *outOffset), 0, 128);
    IOLockUnlock(s->lock);
    return kIOReturnSuccess;
}

void
MlxUAR::FreeClientDbSlot(MlxClientDoorbellBundle *bundle, uint32_t offset)
{
    if (!s || !bundle || !bundle->dbCpu || (offset & 127) || offset >= 4096)
        return;
    IOLockLock(s->lock);
    memset((void *)(uintptr_t)(bundle->dbCpu + offset), 0, 128);
    bundle->dbSlotBitmap &= ~(1u << (offset / 128));
    IOLockUnlock(s->lock);
}

volatile uint32_t *
MlxUAR::GetClientDbRecord(MlxClientDoorbellBundle *bundle, uint32_t offset)
{
    if (!bundle || !bundle->dbCpu || (offset & 127) || offset >= 4096)
        return NULL;
    return (volatile uint32_t *)(bundle->dbCpu + offset);
}
