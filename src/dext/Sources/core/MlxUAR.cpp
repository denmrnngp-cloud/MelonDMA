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
    uint32_t         uarPageSize;
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
             uint32_t uarPageSize)
{
    if (!core || !pci) return kIOReturnBadArgument;
    s = new State;
    if (!s) return kIOReturnNoMemory;
    memset(s, 0, sizeof(*s));
    s->core = core;
    s->pci  = pci;
    s->barIndex = barIndex;
    s->uarPageSize = uarPageSize ? uarPageSize : MLX_UAR_ADAPTER_PAGE_SIZE;
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
MlxUAR::UarPageSize() const { return s ? s->uarPageSize : 0; }

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
    if (s->core->DmaQuarantined()) return;
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
    if (!s || !s->pci || bfOffset + sizeof(value) > s->uarPageSize)
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
    if (!s || !s->pci || MLX_CQ_DOORBELL + sizeof(uint64_t) > s->uarPageSize)
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
    /* UAR pages are laid out contiguously from the start of the BAR, one
     * uarPageSize apart. */
    return (uintptr_t)uarIdx * s->uarPageSize;
}

kern_return_t
MlxUAR::CreateClientSubrange(uint32_t uarIdx, IOMemoryDescriptor **out)
{
    if (!s || !s->barMem || !out) return kIOReturnBadArgument;
    /* One UAR page exactly. With the 16 KiB geometry this is also one host
     * page, so the client's mapping cannot reach a neighbouring UAR. */
    const uint64_t off = UarOffset(uarIdx);
    uint64_t barLen = 0;
    if (s->barMem->GetLength(&barLen) == kIOReturnSuccess && barLen &&
        off + s->uarPageSize > barLen) {
        /* Firmware sizes UAR indices to the page size we asked it for, so this
         * should not happen — log it rather than letting the mapping fail with
         * no explanation of which index went past the aperture. */
        MLX_LOG("UAR[%u] at 0x%llx + %u past BAR length 0x%llx",
                uarIdx, (unsigned long long)off, s->uarPageSize,
                (unsigned long long)barLen);
        return kIOReturnNoSpace;
    }
    return IOMemoryDescriptor::CreateSubMemoryDescriptor(0, off,
                                                         s->uarPageSize,
                                                         s->barMem, out);
}

/* Allocate one 4 KiB doorbell-record page into the bundle's page array. The
 * DMA work happens outside s->lock; the caller publishes the new count. */
kern_return_t
MlxUAR::AllocClientDbPage(MlxClientDoorbellBundle *bundle, uint32_t page)
{
    if (!s || !bundle || page >= MLX_CLIENT_MAX_DB_PAGES)
        return kIOReturnBadArgument;
    IOBufferMemoryDescriptor *mem = NULL;
    IODMACommand *dma = NULL;
    kern_return_t kr = mlxAllocDmaBuffer(MLX_CLIENT_DB_PAGE_SIZE,
                                         MLX_CLIENT_DB_PAGE_SIZE,
                                         kIOMemoryDirectionOutIn, &mem);
    IOAddressSegment segs[4];
    uint32_t segCount = 4;
    if (kr == kIOReturnSuccess && mem)
        kr = mlxPrepareDma(s->pci, mem, segs, &segCount, &dma);
    uint64_t cpu = 0, length = 0;
    if (kr == kIOReturnSuccess && segCount == 1 &&
        segs[0].length >= MLX_CLIENT_DB_PAGE_SIZE)
        kr = mem->Map(0, 0, 0, 0, &cpu, &length);
    if (kr != kIOReturnSuccess || !cpu || length < MLX_CLIENT_DB_PAGE_SIZE) {
        if (dma) mlxCompleteDma(dma);
        if (mem) mem->release();
        return kr ? kr : kIOReturnNoSpace;
    }
    memset((void *)(uintptr_t)cpu, 0, MLX_CLIENT_DB_PAGE_SIZE);
    bundle->dbMemory[page] = mem;
    bundle->dbDma[page] = dma;
    bundle->dbIOVA[page] = segs[0].address;
    bundle->dbCpu[page] = (volatile uint8_t *)(uintptr_t)cpu;
    bundle->dbSlotBitmap[page] = 0;
    return kIOReturnSuccess;
}

kern_return_t
MlxUAR::AllocClientBundle(MlxClientDoorbellBundle *bundle)
{
    if (!s || !bundle) return kIOReturnBadArgument;
    memset(bundle, 0, sizeof(*bundle));
    uint32_t slot = 0;
    kern_return_t kr = AllocClientUar(bundle, &slot);
    if (kr != kIOReturnSuccess) return kr;
    kr = AllocClientDbPage(bundle, 0);
    if (kr != kIOReturnSuccess) {
        FreeClientBundle(bundle);
        return kr;
    }
    bundle->dbPageCount = 1;
    MLX_LOG("client bundle allocated uar=%u db=0x%llx",
            bundle->uarIndex[0], (unsigned long long)bundle->dbIOVA[0]);
    return kIOReturnSuccess;
}

kern_return_t
MlxUAR::AllocClientUar(MlxClientDoorbellBundle *bundle, uint32_t *outSlot)
{
    if (!s || !bundle) return kIOReturnBadArgument;
    if (bundle->uarCount >= MLX_CLIENT_MAX_UAR) return kIOReturnNoSpace;
    uint32_t uarIdx = 0;
    /* ALLOC_UAR is a firmware command with a multi-second timeout, so it stays
     * off s->lock: one client growing its pool must not stall another's
     * doorbell allocation. */
    kern_return_t kr = AllocUAR(&uarIdx);
    if (kr != kIOReturnSuccess) return kr;
    IOMemoryDescriptor *mem = NULL;
    kr = CreateClientSubrange(uarIdx, &mem);
    if (kr != kIOReturnSuccess || !mem) {
        (void)FreeUAR(uarIdx);
        return kr ? kr : kIOReturnNoMemory;
    }
    IOLockLock(s->lock);
    const uint32_t slot = bundle->uarCount;
    if (slot >= MLX_CLIENT_MAX_UAR) {
        IOLockUnlock(s->lock);
        mem->release();
        (void)FreeUAR(uarIdx);
        return kIOReturnNoSpace;
    }
    bundle->uarIndex[slot] = uarIdx;
    bundle->uarMemory[slot] = mem;
    bundle->uarCount = slot + 1;
    IOLockUnlock(s->lock);
    if (outSlot) *outSlot = slot;
    MLX_LOG("client UAR slot %u = UAR[%u]", slot, uarIdx);
    return kIOReturnSuccess;
}

uint32_t
MlxUAR::ClientUarCount(const MlxClientDoorbellBundle *bundle) const
{
    return bundle ? bundle->uarCount : 0;
}

uint32_t
MlxUAR::ClientUarIndex(const MlxClientDoorbellBundle *bundle,
                       uint32_t slot) const
{
    if (!bundle || slot >= bundle->uarCount) return 0;
    return bundle->uarIndex[slot];
}

IOMemoryDescriptor *
MlxUAR::ClientUarMemory(const MlxClientDoorbellBundle *bundle,
                        uint32_t slot) const
{
    if (!bundle || slot >= bundle->uarCount) return NULL;
    return bundle->uarMemory[slot];
}

IOBufferMemoryDescriptor *
MlxUAR::ClientDbMemory(const MlxClientDoorbellBundle *bundle,
                       uint32_t page) const
{
    if (!bundle || page >= bundle->dbPageCount) return NULL;
    return bundle->dbMemory[page];
}

void
MlxUAR::FreeClientBundle(MlxClientDoorbellBundle *bundle)
{
    if (!s || !bundle) return;
    const bool quarantined = s->core->DmaQuarantined();
    for (uint32_t p = 0; p < MLX_CLIENT_MAX_DB_PAGES; p++) {
        if (quarantined) {
            s->core->RetainDmaUntilReset(bundle->dbMemory[p], bundle->dbDma[p],
                                         0x55415244u);
            bundle->dbMemory[p] = NULL;
            bundle->dbDma[p] = NULL;
            continue;
        }
        if (bundle->dbDma[p]) {
            mlxCompleteDma(bundle->dbDma[p]);
            bundle->dbDma[p] = NULL;
        }
        if (bundle->dbMemory[p]) {
            bundle->dbMemory[p]->release();
            bundle->dbMemory[p] = NULL;
        }
    }
    for (uint32_t u = 0; u < MLX_CLIENT_MAX_UAR; u++) {
        if (bundle->uarMemory[u]) {
            bundle->uarMemory[u]->release();
            bundle->uarMemory[u] = NULL;
        }
        /* A quarantined device may still be reading through this UAR, so the
         * index is deliberately leaked rather than handed back to firmware. */
        if (bundle->uarIndex[u] && !quarantined)
            (void)FreeUAR(bundle->uarIndex[u]);
        bundle->uarIndex[u] = 0;
    }
    memset(bundle, 0, sizeof(*bundle));
}

kern_return_t
MlxUAR::AllocClientDbSlot(MlxClientDoorbellBundle *bundle, uint64_t *outDMA,
                          uint32_t *outOffset)
{
    if (!s || !bundle || !outDMA || !outOffset) return kIOReturnBadArgument;
    for (;;) {
        IOLockLock(s->lock);
        for (uint32_t p = 0; p < bundle->dbPageCount; p++) {
            if (!bundle->dbCpu[p]) continue;
            for (uint32_t i = 0; i < MLX_CLIENT_DB_SLOTS_PER_PAGE; i++) {
                if (bundle->dbSlotBitmap[p] & (1u << i)) continue;
                bundle->dbSlotBitmap[p] |= 1u << i;
                const uint32_t within = i * MLX_CLIENT_DB_SLOT_SIZE;
                *outOffset = p * MLX_CLIENT_DB_PAGE_SIZE + within;
                *outDMA = bundle->dbIOVA[p] + within;
                memset((void *)(uintptr_t)(bundle->dbCpu[p] + within), 0,
                       MLX_CLIENT_DB_SLOT_SIZE);
                IOLockUnlock(s->lock);
                return kIOReturnSuccess;
            }
        }
        const uint32_t grow = bundle->dbPageCount;
        IOLockUnlock(s->lock);
        if (grow >= MLX_CLIENT_MAX_DB_PAGES) return kIOReturnNoSpace;
        kern_return_t kr = AllocClientDbPage(bundle, grow);
        if (kr != kIOReturnSuccess) return kr;
        IOLockLock(s->lock);
        bundle->dbPageCount = grow + 1;
        IOLockUnlock(s->lock);
    }
}

void
MlxUAR::FreeClientDbSlot(MlxClientDoorbellBundle *bundle, uint32_t offset)
{
    if (!s || !bundle || (offset & (MLX_CLIENT_DB_SLOT_SIZE - 1))) return;
    if (s->core->DmaQuarantined()) return;
    const uint32_t page = offset / MLX_CLIENT_DB_PAGE_SIZE;
    const uint32_t within = offset % MLX_CLIENT_DB_PAGE_SIZE;
    IOLockLock(s->lock);
    if (page < bundle->dbPageCount && bundle->dbCpu[page]) {
        memset((void *)(uintptr_t)(bundle->dbCpu[page] + within), 0,
               MLX_CLIENT_DB_SLOT_SIZE);
        bundle->dbSlotBitmap[page] &=
            ~(1u << (within / MLX_CLIENT_DB_SLOT_SIZE));
    }
    IOLockUnlock(s->lock);
}

volatile uint32_t *
MlxUAR::GetClientDbRecord(MlxClientDoorbellBundle *bundle, uint32_t offset)
{
    if (!bundle || (offset & (MLX_CLIENT_DB_SLOT_SIZE - 1))) return NULL;
    const uint32_t page = offset / MLX_CLIENT_DB_PAGE_SIZE;
    const uint32_t within = offset % MLX_CLIENT_DB_PAGE_SIZE;
    if (page >= bundle->dbPageCount || !bundle->dbCpu[page]) return NULL;
    return (volatile uint32_t *)(bundle->dbCpu[page] + within);
}
