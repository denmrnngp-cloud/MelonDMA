/*
 * MlxDriverKitCompat.h — DriverKit API compatibility helpers.
 *
 * Replaces the kext MlxKernelCompat.hpp. The kext version used
 * IOMemoryMap::getVirtualAddress() + volatile pointer dereference for MMIO and
 * IODMACommand::withSpecification() (kernel factory) for DMA. DriverKit has no
 * kernel virtual address for the BAR — MMIO goes through
 * IOPCIDevice::MemoryRead/Write32 (LOCALONLY, returns void), and DMA uses
 * IODMACommand::Create + PrepareForDMA on the device.
 *
 * References: notes/11 §2.1–2.5 (DriverKit memory API map).
 */
#ifndef MLX_DRIVERKIT_COMPAT_H
#define MLX_DRIVERKIT_COMPAT_H

#include <stdint.h>
#include <stddef.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOReturn.h>
#include <DriverKit/IOService.h>
#include <DriverKit/IOMemoryDescriptor.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#include <PCIDriverKit/IOPCIDevice.h>

/* OSSwapBigToHostInt32 / OSSwapHostToBigInt32 come from <DriverKit/IOLib.h>. */

/* Order regular memory accesses before/after DMA ownership transitions. */
static inline void
mlxMemoryBarrier()
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/*
 * mlx5 init-segment and command registers use big-endian 32-bit MMIO accesses.
 * In DriverKit there is no mapped kernel pointer — every access is a method
 * call on IOPCIDevice with the BAR memory index + offset. MemoryRead32 /
 * MemoryWrite32 are LOCALONLY and return void.
 */
static inline uint32_t
mlxMMIORead32BE(IOPCIDevice *pci, uint8_t barIndex, uintptr_t offset)
{
    uint32_t value = 0;
    pci->MemoryRead32(barIndex, (uint64_t)offset, &value);
    return OSSwapBigToHostInt32(value);
}

static inline void
mlxMMIOWrite32BE(IOPCIDevice *pci, uint8_t barIndex, uintptr_t offset,
                 uint32_t value)
{
    pci->MemoryWrite32(barIndex, (uint64_t)offset,
                       OSSwapHostToBigInt32(value));
}

static inline uint32_t
mlxMMIORead32(IOPCIDevice *pci, uint8_t barIndex, uintptr_t offset)
{
    uint32_t value = 0;
    pci->MemoryRead32(barIndex, (uint64_t)offset, &value);
    return value;
}

static inline void
mlxMMIOWrite32(IOPCIDevice *pci, uint8_t barIndex, uintptr_t offset,
               uint32_t value)
{
    pci->MemoryWrite32(barIndex, (uint64_t)offset, value);
}

/*
 * Allocate DEXT-owned DMA-coherent memory (command queue, mailbox, firmware
 * pages, WQ/CQ, DB records). Direction is from the DMA (device) perspective.
 *   kIOMemoryDirectionIn  = device writes into this memory
 *   kIOMemoryDirectionOut = device reads from this memory
 */
static inline kern_return_t
mlxAllocDmaBuffer(uint64_t capacity, uint64_t alignment,
                  uint64_t direction, IOBufferMemoryDescriptor **out)
{
    return IOBufferMemoryDescriptor::Create(direction, capacity, alignment, out);
}

/*
 * Pin a DEXT-owned buffer for DMA and return up to 32 IOVA segments.
 * The mapping stays alive until CompleteDMA — that is the MR/Q lifetime.
 * Caller must split 16 KiB host-page IOVA segments into 4 KiB HCA PAS entries
 * before handing them to CREATE_MKEY/CREATE_QP (notes/11 §0, §2.3).
 */
static inline kern_return_t
mlxPrepareDma(IOService *device, IOMemoryDescriptor *mem,
              IOAddressSegment segments[32], uint32_t *segmentsCount,
              IODMACommand **outCmd)
{
    IODMACommandSpecification spec = {};
    spec.options = kIODMACommandSpecificationNoOptions;
    spec.maxAddressBits = 64;

    IODMACommand *cmd = NULL;
    kern_return_t kr = IODMACommand::Create(device,
                                            kIODMACommandCreateNoOptions,
                                            &spec, &cmd);
    if (kr != kIOReturnSuccess || !cmd)
        return kr ? kr : kIOReturnNoMemory;

    uint64_t memLen = 0;
    kr = mem->GetLength(&memLen);
    if (kr != kIOReturnSuccess) {
        cmd->release();
        return kr;
    }

    kr = cmd->PrepareForDMA(kIODMACommandPrepareForDMANoOptions, mem,
                            0, memLen, NULL,
                            segmentsCount, segments);
    if (kr != kIOReturnSuccess) {
        cmd->release();
        return kr;
    }
    *outCmd = cmd;
    return kIOReturnSuccess;
}

static inline void
mlxCompleteDma(IODMACommand *cmd)
{
    if (!cmd) return;
    cmd->CompleteDMA(kIODMACommandCompleteDMANoOptions);
    cmd->release();
}

#endif /* MLX_DRIVERKIT_COMPAT_H */