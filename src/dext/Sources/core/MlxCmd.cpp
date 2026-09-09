/*
 * MlxCmd.cpp — Firmware command interface (DriverKit port).
 *
 * Ported from: kernel_src/mlnx-ofed-kernel-5.9 core/cmd.c, trimmed for DEXT.
 *
 * Command flow (cmd.c:969-1056 mlx5_cmd_work_handler + cmd.c:237 poll):
 *   1. Command header (first 16B: opcode/op_mod/...) goes into MlxCmdLayout.in.
 *   2. Large input (>16B) spills into a mailbox chain: each MlxCmdMailbox
 *      block holds 512B of data + a next pointer; the first block's DMA
 *      address goes into layout.in_ptr.
 *   3. Large output (>16B) gets an output mailbox chain (layout.out_ptr).
 *   4. Hand ownership to HW (status_own |= OWNER_HW), set signature, ring
 *      the doorbell (init-seg cmd_dbell, slot 0).
 *   5. Poll status_own until HW flips it back to SW.
 *   6. Copy response header (16B) + output mailbox blocks back to caller.
 *   7. Double success gate: descriptor delivery status (bits [7:1]) AND
 *      firmware outbox status (REMEDIATION_PLAN §5.1, mlxP1ParseOutbox).
 *
 * MVP: single command slot (slot 0), polling completion. The kext donor's
 * 32-slot bitmap + event-mode completion is a later optimization.
 *
 * References: notes/08 (firmware command reference), notes/11 §2 (DriverKit
 * memory API).
 */
#include "MlxCmd.hpp"
#include "MlxDriverKitCompat.h"
#include "MlxRegs.hpp"
#include "MlxP1Encoding.hpp"
#include "MlxPCIDriver.h"

#include <DriverKit/IOLib.h>
#include <time.h>
#include <DriverKit/IOMemoryDescriptor.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#include <PCIDriverKit/IOPCIDevice.h>
#include <string.h>

#include "MlxLog.hpp"
#define MLX_LOG(fmt, ...)  IOLog("MlxPCIDriver: MlxCmd: " fmt "\n", ##__VA_ARGS__)
#define MLX_DBG(fmt, ...)  MLX_DBGLOG("MlxPCIDriver: MlxCmd: " fmt, ##__VA_ARGS__)

static inline uint8_t
xor8(const void *buf, size_t off, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf + off;
    uint8_t acc = 0;
    for (size_t i = 0; i < len; i++) acc ^= p[i];
    return acc;
}

/* Bounded spin before falling back to a 1 ms sleep. Long enough to cover a
 * normal firmware command, short enough that a stalled one does not burn a
 * core for meaningful time. */
#define MLX_CMD_SPIN_NS 400000ull   /* 400 us */

/* One direction of one command slot. The block arrays are what makes a slot
 * expensive (1024 entries so a 149 MiB registration fits inline), which is why
 * slots are allocated on first use rather than up front. */
struct MlxCmdChain {
    IOBufferMemoryDescriptor *desc[MLX_CMD_MAX_BLOCKS];
    IODMACommand             *dma[MLX_CMD_MAX_BLOCKS];
    MlxCmdMailbox            *box[MLX_CMD_MAX_BLOCKS];
    uint64_t                  iova[MLX_CMD_MAX_BLOCKS];
    IOBufferMemoryDescriptor *singleDesc;
    IODMACommand             *singleDma;
    uint32_t                  blocks;   /* blocks the current command uses */
    uint32_t                  cached;   /* blocks allocated and kept for reuse */
    bool                      single;   /* cached chain is one contiguous buffer */
};

/* A command slot owns one hardware descriptor in the command queue and the
 * mailbox chains that go with it. Apple keeps a 32-entry bitmap of these and
 * completes them from the command EQ (AppleEthernetMLX5Cmd::compHandler); this
 * is the same slot bitmap with polling completion, which is what the two
 * interrupt vectors this nub was granted leave room for. */
struct MlxCmdSlot {
    MlxCmdChain in;
    MlxCmdChain out;
    uint8_t     token;
    bool        poisoned;   /* timed out: firmware may still own it, never reuse */
    /* Set by the command-completion event for this slot. It shortens the wait;
     * the descriptor's ownership bit stays the authority for reading the
     * result, because the event says "finished", not "the outbox is yours". */
    uint32_t    done;
};

struct MlxCmd::State {
    MlxPCIDriver              *core;
    IOPCIDevice               *pci;
    uint8_t                    barIndex;
    IOBufferMemoryDescriptor  *cmdqMem;
    IODMACommand             *cmdqDma;
    uint64_t                  cmdqIOVA;
    void                     *cmdqBuf;     /* mapped CPU address (LOCALONLY) */
    uint16_t                  cmdifRev;
    uint8_t                   logSz;
    uint8_t                   logStride;
    bool                      up;
    bool                      quarantined;
    /* Last-command diagnostics, device-wide. With more than one slot in flight
     * these describe whichever command finished last, so a caller that needs
     * its own status must read them immediately after its own Exec — which is
     * what the bring-up paths do, and they run before any client exists. */
    uint32_t                  lastOpcode;
    uint32_t                  lastSyndrome;
    uint8_t                   lastDeliveryStatus;
    /* How many commands were issued, and how many of them outlived the spin
     * window and had to sleep. A rising ratio says the spin budget is too
     * short for this firmware or this command mix. */
    uint64_t                  commandsIssued;
    uint64_t                  spinFellThrough;
    /* How often a command found every regular slot busy and had to wait. Zero
     * on a serial workload; a rising count is the signal to raise
     * MLX_CMD_REG_SLOTS. */
    uint64_t                  slotWaits;
    uint8_t                   lastFwStatus;

    /* Slot arbitration. execLock guards the busy mask and the slot array, not
     * the command itself: a slow command must not block a fast one on another
     * slot, which is the whole point of having more than one. */
    IOLock                   *execLock;
    MlxCmdSlot               *slot[MLX_CMD_HW_SLOTS];
    uint64_t                  busyMask;
    uint32_t                  regSlots;   /* usable slots for regular commands */
    uint32_t                  pagesSlot;  /* reserved for MANAGE_PAGES */
};

MlxCmd::MlxCmd() : s(NULL) {}
MlxCmd::~MlxCmd() { Free(); }

kern_return_t
MlxCmd::Init(MlxPCIDriver *core)
{
    if (!core) return kIOReturnBadArgument;
    s = new State;
    if (!s) return kIOReturnNoMemory;
    memset(s, 0, sizeof(*s));
    s->core = core;
    s->pci  = core->GetPCI();
    s->barIndex = core->GetBar0Index();
    s->execLock = IOLockAlloc();
    if (!s->execLock) {
        delete s; s = NULL;
        return kIOReturnNoMemory;
    }

    /* 1. Validate command interface revision (cmd.c:2239). */
    uint32_t cmdifRevFw = mlxMMIORead32BE(s->pci, s->barIndex,
                                          offsetof(struct MlxInitSeg, cmdif_rev_fw_sub));
    s->cmdifRev = (uint16_t)(cmdifRevFw >> 16);
    {
        /* Initialization-segment delta dump (notes/35): if the registers are zero
         * or 0xFFFFFFFF — BAR/MMIO is not ready yet or fw did not load. */
        uint32_t fwRev    = mlxMMIORead32BE(s->pci, s->barIndex, 0);
        uint32_t initReg  = mlxMMIORead32BE(s->pci, s->barIndex,
                                            offsetof(struct MlxInitSeg, initializing));
        uint32_t addrLSzD = mlxMMIORead32BE(s->pci, s->barIndex,
                                            offsetof(struct MlxInitSeg, cmdq_addr_l_sz));
        MLX_LOG("DBG Init: fw_rev=0x%08x cmdif_fw_sub=0x%08x init=0x%08x addrLSz=0x%08x",
                fwRev, cmdifRevFw, initReg, addrLSzD);
    }
    if (s->cmdifRev != MLX_CMD_IF_REV) {
        MLX_LOG("cmdif rev mismatch (fw=%u, need=%u)", s->cmdifRev, MLX_CMD_IF_REV);
        IOLockFree(s->execLock); s->execLock = NULL;
        delete s; s = NULL; return kIOReturnNoDevice;
    }

    /* 2. Read command queue geometry (cmd.c:2255). */
    uint32_t addrLSz = mlxMMIORead32BE(s->pci, s->barIndex,
                                       offsetof(struct MlxInitSeg, cmdq_addr_l_sz));
    uint8_t params = (uint8_t)(addrLSz & 0xFF);
    s->logSz     = (params >> 4) & 0xF;
    s->logStride = params & 0xF;
    if (s->logSz >= 31 || s->logStride < 6 || s->logSz + s->logStride > 12) {
        MLX_LOG("invalid cmdq geometry log_sz=%u stride=%u", s->logSz, s->logStride);
        IOLockFree(s->execLock); s->execLock = NULL;
        delete s; s = NULL; return kIOReturnNoDevice;
    }

    /* 3. Allocate + DMA-pin the command queue page (notes/11 §2.1). */
    kern_return_t kr = mlxAllocDmaBuffer(MLX_CMDQ_SIZE, 4096,
                                         kIOMemoryDirectionOutIn, &s->cmdqMem);
    if (kr != kIOReturnSuccess || !s->cmdqMem) {
        MLX_LOG("cmdq allocation failed: 0x%x", kr);
        IOLockFree(s->execLock); s->execLock = NULL;
        delete s; s = NULL; return kr ? kr : kIOReturnNoMemory;
    }

    IOAddressSegment segs[32];
    uint32_t segCount = 32;
    kr = mlxPrepareDma(s->pci, s->cmdqMem, segs, &segCount, &s->cmdqDma);
    if (kr != kIOReturnSuccess || segCount == 0) {
        MLX_LOG("cmdq DMA prepare failed: 0x%x", kr);
        s->cmdqMem->release(); s->cmdqMem = NULL;
        IOLockFree(s->execLock); s->execLock = NULL;
        delete s; s = NULL; return kr ? kr : kIOReturnNoMemory;
    }
    s->cmdqIOVA = segs[0].address;
    {
        /* cmdq DMA delta (notes/35): fw reads commands at cmdqIOVA; if the IOVA
         * does not fall in the DART window — cmdq is dead from the start. */
        MLX_LOG("DBG Init: cmdq DMA segs=%u seg0={0x%llx, %llu}", segCount,
                (unsigned long long)segs[0].address,
                (unsigned long long)segs[0].length);
    }

    /* CPU-side mapping for descriptor writes. */
    uint64_t mappedAddr = 0, mappedLen = 0;
    kr = s->cmdqMem->Map(0, 0, 0, 0, &mappedAddr, &mappedLen);
    if (kr != kIOReturnSuccess) {
        MLX_LOG("cmdq CPU map failed: 0x%x", kr);
        mlxCompleteDma(s->cmdqDma); s->cmdqDma = NULL;
        s->cmdqMem->release(); s->cmdqMem = NULL;
        IOLockFree(s->execLock); s->execLock = NULL;
        delete s; s = NULL; return kr;
    }
    s->cmdqBuf = (void *)(uintptr_t)mappedAddr;

    /* 4. Hand the command queue DMA address to firmware (cmd.c:2300). */
    mlxMMIOWrite32BE(s->pci, s->barIndex,
                     offsetof(struct MlxInitSeg, cmdq_addr_h),
                     (uint32_t)(s->cmdqIOVA >> 32));
    mlxMMIOWrite32BE(s->pci, s->barIndex,
                     offsetof(struct MlxInitSeg, cmdq_addr_l_sz),
                     (uint32_t)(s->cmdqIOVA & 0xFFFFFFFF));
    {
        /* Verify write-back of cmdq_addr_l_sz (notes/35): if fw did not accept
         * the address (value did not match) — commands will go nowhere. */
        uint32_t rb = mlxMMIORead32BE(s->pci, s->barIndex,
                                      offsetof(struct MlxInitSeg, cmdq_addr_l_sz));
        MLX_LOG("DBG Init: cmdq_addr_l_sz writeback=0x%08x (expect low=0x%08x)",
                rb, (uint32_t)(s->cmdqIOVA & 0xFFFFFFFF));
    }

    memset(s->cmdqBuf, 0, MLX_CMDQ_SIZE);
    /* Slot geometry. The last hardware slot belongs to MANAGE_PAGES; whatever
     * is left, up to the cap, carries regular commands. A queue with a single
     * descriptor collapses to one shared slot, which is exactly the behaviour
     * this driver had before the ring existed. */
    {
        const uint32_t hwSlots = 1u << s->logSz;
        s->pagesSlot = hwSlots ? hwSlots - 1 : 0;
        uint32_t reg = hwSlots > 1 ? hwSlots - 1 : 1;
        if (reg > MLX_CMD_REG_SLOTS) reg = MLX_CMD_REG_SLOTS;
        if (reg > MLX_CMD_HW_SLOTS) reg = MLX_CMD_HW_SLOTS;
        s->regSlots = reg;
    }
    s->up = true;
    MLX_LOG("ready (rev=%u, log_sz=%u, stride=%u, iova=0x%llx, reg_slots=%u, pages_slot=%u)",
            s->cmdifRev, s->logSz, s->logStride, s->cmdqIOVA,
            s->regSlots, s->pagesSlot);
    return kIOReturnSuccess;
}

void
MlxCmd::Free()
{
    if (!s) return;
    s->up = false;
    for (uint32_t i = 0; i < MLX_CMD_HW_SLOTS; i++) {
        if (!s->slot[i]) continue;
        ReleaseChain(&s->slot[i]->in);
        ReleaseChain(&s->slot[i]->out);
        IODelete(s->slot[i], MlxCmdSlot, 1);
        s->slot[i] = NULL;
    }
    if ((s->quarantined || s->core->DmaQuarantined()) && (s->cmdqDma || s->cmdqMem)) {
        s->core->RetainDmaUntilReset(s->cmdqMem, s->cmdqDma, 0x434d4451u);
        s->cmdqDma = NULL; s->cmdqMem = NULL;
    }
    if (s->cmdqDma) { mlxCompleteDma(s->cmdqDma); s->cmdqDma = NULL; }
    if (s->cmdqMem) { s->cmdqMem->release(); s->cmdqMem = NULL; }
    if (s->execLock) { IOLockFree(s->execLock); s->execLock = NULL; }
    delete s; s = NULL;
}

/* ---- mailbox chain helpers (cmd.c:allocMailbox / freeMailbox) ---- */

/* Give a chain's blocks back to the allocator. Only teardown and quarantine
 * take this path: a completed command leaves its chain cached. */
void
MlxCmd::ReleaseChain(MlxCmdChain *c)
{
    const bool keep = s->quarantined || s->core->DmaQuarantined();
    if (c->single) {
        if (keep && (c->singleDma || c->singleDesc)) {
            s->core->RetainDmaUntilReset(c->singleDesc, c->singleDma, 0x434d424fu);
            c->singleDesc = NULL; c->singleDma = NULL;
        }
        if (c->singleDma) { mlxCompleteDma(c->singleDma); c->singleDma = NULL; }
        if (c->singleDesc) { c->singleDesc->release(); c->singleDesc = NULL; }
    } else {
        for (uint32_t i = 0; i < c->cached; i++) {
            if (keep && (c->dma[i] || c->desc[i])) {
                s->core->RetainDmaUntilReset(c->desc[i], c->dma[i], 0x434d424fu);
                c->desc[i] = NULL; c->dma[i] = NULL;
            }
            if (c->dma[i]) { mlxCompleteDma(c->dma[i]); c->dma[i] = NULL; }
            if (c->desc[i]) { c->desc[i]->release(); c->desc[i] = NULL; }
        }
    }
    for (uint32_t i = 0; i < c->cached; i++) { c->box[i] = NULL; c->iova[i] = 0; }
    c->cached = 0;
    c->blocks = 0;
    c->single = false;
}

/* Make the chain hold at least `size` bytes of command data.
 *
 * The blocks are kept across commands. Before this, every command paid an
 * IODMACommand create + PrepareForDMA + Map and the matching teardown twice
 * over, on top of the firmware round trip — Apple pools them the same way
 * (AppleEthernetMLX5Cmd::allocCmdMsg reuses a cached message whose block count
 * already covers the request, and only allocates when it does not). */
kern_return_t
MlxCmd::AllocMailbox(MlxCmdChain *c, uint32_t size)
{
    uint32_t numBlocks = 0;
    if (size > 16)
        numBlocks = (size - 16 + MLX_CMD_DATA_BLOCK_SIZE - 1) / MLX_CMD_DATA_BLOCK_SIZE;
    if (numBlocks > MLX_CMD_MAX_BLOCKS) {
        MLX_LOG("command too large: needs %u blocks (max %u)", numBlocks, MLX_CMD_MAX_BLOCKS);
        return kIOReturnNoSpace;
    }
    /* blocks is published only once every ReleaseChain below has run, because
     * ReleaseChain clears it: setting it first would leave the chain reporting
     * no blocks and send a command with a null mailbox pointer. */
    c->blocks = 0;
    if (numBlocks == 0) return kIOReturnSuccess;
    /* Give an oversized chain back rather than pinning it for the life of the
     * driver. Two registrations of the same shape still reuse, because the
     * chain is only dropped when this command needs strictly less. */
    if (c->cached > MLX_CMD_CACHE_MAX_BLOCKS && numBlocks < c->cached)
        ReleaseChain(c);
    if (numBlocks <= c->cached) {
        /* Reuse. The blocks are rewritten in full by the caller: input blocks
         * get data plus a fresh descriptor and signature, output blocks get a
         * descriptor and signature, so no zeroing is needed here. */
        c->blocks = numBlocks;
        return kIOReturnSuccess;
    }
    ReleaseChain(c);

    /* Fast path: one page-aligned buffer, blocks at 4096-byte stride. If
     * DriverKit/DART cannot expose that buffer as one contiguous IOVA range,
     * discard it and use the proven per-block layout below. */
    {
        uint32_t bytes = numBlocks * 4096;
        kern_return_t singleKr = mlxAllocDmaBuffer(bytes, 4096,
                                                   kIOMemoryDirectionOutIn,
                                                   &c->singleDesc);
        IOAddressSegment *segments = NULL;
        if (singleKr == kIOReturnSuccess && c->singleDesc) {
            segments = IONew(IOAddressSegment, MLX_CMD_MAX_BLOCKS + 2);
            uint32_t segmentCount = MLX_CMD_MAX_BLOCKS + 2;
            if (segments)
                singleKr = mlxPrepareDma(s->pci, c->singleDesc, segments,
                                         &segmentCount, &c->singleDma);
            else
                singleKr = kIOReturnNoMemory;
            bool contiguous = singleKr == kIOReturnSuccess && segmentCount > 0;
            uint64_t iovaBase = contiguous ? segments[0].address : 0;
            uint64_t span = contiguous ? segments[0].length : 0;
            for (uint32_t i = 1; contiguous && i < segmentCount; i++) {
                if (segments[i].address != segments[i - 1].address +
                                             segments[i - 1].length)
                    contiguous = false;
                else
                    span += segments[i].length;
            }
            uint64_t addr = 0, mappedLength = 0;
            if (contiguous && span >= bytes)
                singleKr = c->singleDesc->Map(0, 0, 0, 0, &addr, &mappedLength);
            else
                singleKr = kIOReturnNoSpace;
            if (singleKr == kIOReturnSuccess && mappedLength >= bytes) {
                memset((void *)(uintptr_t)addr, 0, bytes);
                for (uint32_t i = 0; i < numBlocks; i++) {
                    c->box[i] = (MlxCmdMailbox *)(uintptr_t)(addr + (uint64_t)i * 4096);
                    c->iova[i] = iovaBase + (uint64_t)i * 4096;
                }
                c->single = true;
                c->cached = numBlocks;
                c->blocks = numBlocks;
                if (segments) IODelete(segments, IOAddressSegment, MLX_CMD_MAX_BLOCKS + 2);
                return kIOReturnSuccess;
            }
            if (c->singleDma) { mlxCompleteDma(c->singleDma); c->singleDma = NULL; }
            if (c->singleDesc) { c->singleDesc->release(); c->singleDesc = NULL; }
        }
        if (segments) IODelete(segments, IOAddressSegment, MLX_CMD_MAX_BLOCKS + 2);
    }

    for (uint32_t i = 0; i < numBlocks; i++) {
        kern_return_t kr = mlxAllocDmaBuffer(4096, 4096,
                                             kIOMemoryDirectionOutIn, &c->desc[i]);
        if (kr != kIOReturnSuccess || !c->desc[i]) {
            MLX_LOG("mailbox alloc failed: 0x%x", kr);
            c->cached = i;
            ReleaseChain(c);
            return kr ? kr : kIOReturnNoMemory;
        }
        IOAddressSegment segs[32];
        uint32_t segCount = 32;
        kr = mlxPrepareDma(s->pci, c->desc[i], segs, &segCount, &c->dma[i]);
        if (kr != kIOReturnSuccess || segCount == 0) {
            MLX_LOG("mailbox DMA prepare failed: 0x%x", kr);
            c->desc[i]->release(); c->desc[i] = NULL;
            c->cached = i;
            ReleaseChain(c);
            return kr ? kr : kIOReturnNoMemory;
        }
        c->iova[i] = segs[0].address;
        uint64_t addr = 0, len = 0;
        kr = c->desc[i]->Map(0, 0, 0, 0, &addr, &len);
        if (kr != kIOReturnSuccess) {
            MLX_LOG("mailbox CPU map failed: 0x%x", kr);
            c->cached = i + 1;
            ReleaseChain(c);
            return kr;
        }
        c->box[i] = (MlxCmdMailbox *)(uintptr_t)addr;
        memset(c->box[i], 0, sizeof(MlxCmdMailbox));
    }
    c->cached = numBlocks;
    c->blocks = numBlocks;
    return kIOReturnSuccess;
}

/* Set mailbox ctrl/sig checksums (cmd.c:207 calc_block_sig). */
void
MlxCmd::SetMailboxSignature(MlxCmdMailbox *mb)
{
    size_t ctrl_xor_len = sizeof(MlxCmdMailbox) - sizeof(mb->data) - 2;
    size_t rsvd0_off = offsetof(MlxCmdMailbox, rsvd0);
    mb->ctrl_sig = (uint8_t)~xor8(mb, rsvd0_off, ctrl_xor_len);
    mb->sig = (uint8_t)~xor8(mb, 0, sizeof(*mb) - 1);
}

/* Reserve a command slot.
 *
 * MANAGE_PAGES keeps the dedicated last hardware slot it has always used:
 * firmware refuses it anywhere else with delivery_status 6. Everything else
 * takes any free regular slot, so a 30 ms memory registration no longer stands
 * in front of a QP transition on another thread — that serialisation was the
 * point of this change. Slots are allocated on first use because each one
 * carries block arrays big enough for a 149 MiB registration. */
void
MlxCmd::CompleteFromEvent(uint32_t mask)
{
    if (!s) return;
    for (uint32_t i = 0; i < MLX_CMD_HW_SLOTS && i < 32; i++) {
        if (!(mask & (1u << i))) continue;
        if (s->slot[i]) __atomic_store_n(&s->slot[i]->done, 1, __ATOMIC_RELEASE);
    }
}

kern_return_t
MlxCmd::AcquireSlot(uint32_t opcode, uint32_t timeoutMs, uint32_t *outSlot)
{
    const bool pages = (opcode == MLX_CMD_OP_MANAGE_PAGES);
    const uint64_t spinDeadline =
        clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + MLX_CMD_SPIN_NS;
    uint32_t waitBudget = timeoutMs > 1000 ? timeoutMs : 1000;
    bool counted = false;
    for (;;) {
        IOLockLock(s->execLock);
        uint32_t chosen = MLX_CMD_HW_SLOTS;
        if (pages) {
            if (!(s->busyMask & (1ull << s->pagesSlot))) chosen = s->pagesSlot;
        } else {
            for (uint32_t i = 0; i < s->regSlots; i++)
                if (!(s->busyMask & (1ull << i))) { chosen = i; break; }
        }
        if (chosen < MLX_CMD_HW_SLOTS) {
            if (!s->slot[chosen]) {
                s->slot[chosen] = IONewZero(MlxCmdSlot, 1);
                if (!s->slot[chosen]) {
                    IOLockUnlock(s->execLock);
                    return kIOReturnNoMemory;
                }
                s->slot[chosen]->token = 1;
            }
            s->busyMask |= 1ull << chosen;
            IOLockUnlock(s->execLock);
            *outSlot = chosen;
            return kIOReturnSuccess;
        }
        if (!counted) { s->slotWaits++; counted = true; }   /* under execLock */
        IOLockUnlock(s->execLock);
        if (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) < spinDeadline) continue;
        if (!waitBudget) return kIOReturnBusy;
        waitBudget--;
        IOSleep(1);
    }
}

void
MlxCmd::ReleaseSlot(uint32_t slot)
{
    IOLockLock(s->execLock);
    s->busyMask &= ~(1ull << slot);
    IOLockUnlock(s->execLock);
}

kern_return_t
MlxCmd::Exec(uint32_t opcode, const void *in, uint32_t inSize,
             void *out, uint32_t outSize, uint32_t timeoutMs)
{
    if (!s || !s->up || !s->execLock) return kIOReturnNotReady;
    if (s->quarantined || s->core->DmaQuarantined()) return kIOReturnNotReady;
    uint32_t slot = 0;
    kern_return_t kr = AcquireSlot(opcode, timeoutMs, &slot);
    if (kr != kIOReturnSuccess) return kr;
    kr = ExecOnSlot(slot, opcode, in, inSize, out, outSize, timeoutMs);
    /* A timed-out slot is never released: firmware may still own its
     * descriptor and mailboxes, so nothing may reuse them. The device is
     * quarantined by then anyway, which stops new commands at the door. */
    if (kr != kIOReturnTimeout) ReleaseSlot(slot);
    return kr;
}

kern_return_t
MlxCmd::ExecOnSlot(uint32_t slot, uint32_t opcode, const void *in,
                   uint32_t inSize, void *out, uint32_t outSize,
                   uint32_t timeoutMs)
{
    if (!s || !s->up) return kIOReturnNotReady;
    MlxCmdSlot *sl = s->slot[slot];
    if (!sl) return kIOReturnNotReady;
    s->lastOpcode = opcode;
    s->lastSyndrome = 0;
    s->lastDeliveryStatus = 0;
    s->lastFwStatus = 0;
    if (!in || inSize == 0 || inSize > MLX_CMD_MAX_INPUT_SIZE ||
        !out || outSize < 8 || outSize > MLX_CMD_MAX_SIZE) {
        return kIOReturnBadArgument;
    }
    if (s->quarantined || s->core->DmaQuarantined())
        return kIOReturnNotReady;

    /* Mailbox chains for large commands. Cached across commands on this slot. */
    kern_return_t kr = AllocMailbox(&sl->in, inSize);
    if (kr != kIOReturnSuccess) return kr;
    kr = AllocMailbox(&sl->out, outSize);
    if (kr != kIOReturnSuccess) return kr;

    MlxCmdLayout *lay = (MlxCmdLayout *)
        ((uint8_t *)s->cmdqBuf + ((size_t)slot << s->logStride));
    memset(lay, 0, sizeof(*lay));

    /* Command header: first 16 bytes. */
    memcpy(lay->in, in, (inSize < 16) ? inSize : 16);

    /* Large input -> mailbox chain (cmd.c:980). A reused block still holds the
     * previous command's bytes, so a short tail is zeroed rather than left. */
    for (uint32_t i = 0; i < sl->in.blocks; i++) {
        MlxCmdMailbox *mb = sl->in.box[i];
        uint32_t copied = 16 + i * MLX_CMD_DATA_BLOCK_SIZE;
        uint32_t dataLen = inSize - copied;
        if (dataLen > MLX_CMD_DATA_BLOCK_SIZE) dataLen = MLX_CMD_DATA_BLOCK_SIZE;
        memcpy(mb->data, (const uint8_t *)in + copied, dataLen);
        if (dataLen < MLX_CMD_DATA_BLOCK_SIZE)
            memset(mb->data + dataLen, 0, MLX_CMD_DATA_BLOCK_SIZE - dataLen);
        mb->next      = OSSwapHostToBigInt64(
            (i + 1 < sl->in.blocks) ? sl->in.iova[i + 1] : 0);
        mb->block_num = OSSwapHostToBigInt32(i);   /* big-endian order (AppleMCX) */
        mb->token     = sl->token;
        SetMailboxSignature(mb);
    }
    lay->in_ptr = OSSwapHostToBigInt64(sl->in.blocks ? sl->in.iova[0] : 0);
    lay->inlen  = OSSwapHostToBigInt32(inSize);

    /* Output mailbox chain (pre-linked, no data yet). Cleared for the same
     * reason: the reply is copied out by length and a short one would
     * otherwise hand back the previous command's bytes. */
    for (uint32_t i = 0; i < sl->out.blocks; i++) {
        MlxCmdMailbox *mb = sl->out.box[i];
        memset(mb->data, 0, MLX_CMD_DATA_BLOCK_SIZE);
        mb->next      = OSSwapHostToBigInt64(
            (i + 1 < sl->out.blocks) ? sl->out.iova[i + 1] : 0);
        mb->block_num = OSSwapHostToBigInt32(i);   /* big-endian order (AppleMCX) */
        mb->token     = sl->token;
        SetMailboxSignature(mb);
    }
    lay->out_ptr = OSSwapHostToBigInt64(sl->out.blocks ? sl->out.iova[0] : 0);
    lay->outlen = OSSwapHostToBigInt32(outSize);

    lay->type  = MLX_CMD_TYPE_XPORT;
    lay->token = sl->token++;

    /* Hand ownership to firmware + signature (cmd.c:228). */
    lay->status_own = MLX_CMD_OWNER_HW;
    lay->sig = 0;
    lay->sig = (uint8_t)~xor8(lay, 0, sizeof(*lay));

    /* Clear the event flag and take the generation before the doorbell, so a
     * completion that lands between the two is not missed. */
    __atomic_store_n(&sl->done, 0, __ATOMIC_RELEASE);
    uint64_t cmdGeneration = s->core->CommandGeneration();

    /* Doorbell: set slot bit (cmd.c:1069 writes 1 << ent->idx). */
    mlxMemoryBarrier();
    mlxMMIOWrite32BE(s->pci, s->barIndex,
                     offsetof(struct MlxInitSeg, cmd_dbell), 1u << slot);

    /* Poll for completion (cmd.c:237).
     *
     * Firmware answers most commands in tens of microseconds, but IOSleep
     * cannot wait less than a millisecond, so sleeping on the first miss put
     * a hard millisecond floor under EVERY command: measured, registering a
     * 4 KiB region cost 1.31 ms and deregistering it 1.18 ms, essentially all
     * of it this sleep. Spin for a bounded window first and only then sleep,
     * so the common case is measured in microseconds while a slow or stuck
     * command still yields the CPU rather than burning it. */
    uint32_t waited = 0;
    const uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    const uint64_t spinDeadline = start + MLX_CMD_SPIN_NS;
    /* The timeout is wall-clock, not a count of loop iterations.
     *
     * It used to count iterations on the assumption that each one slept a
     * millisecond, which held while the only wait was IOSleep(1). Adding an
     * event wait broke that silently: the wait returns immediately whenever
     * the completion generation has already moved, which another command's
     * completion does routinely — so the loop spun, charged itself a
     * millisecond per pass, and declared a five-second command timed out in
     * under a second. QUERY_VPORT_STATE died that way at bring-up and took the
     * device into DMA quarantine with it. Elapsed time cannot be fooled by
     * how, or whether, the wait actually sleeps. */
    const uint64_t deadline = timeoutMs
        ? start + (uint64_t)timeoutMs * 1000000ULL : 0;
    while (true) {
        mlxMemoryBarrier();
        if (!(*(volatile uint8_t *)&lay->status_own & MLX_CMD_OWNER_HW)) break;
        if (deadline && clock_gettime_nsec_np(CLOCK_UPTIME_RAW) >= deadline) {
            s->quarantined = true;
            MLX_LOG("opcode 0x%x timed out on slot %u; quarantined", opcode, slot);
            /* The firmware may still own both mailboxes and the command
             * slot. Do not free or reuse them, including for TEARDOWN_HCA. */
            s->core->EnterDmaQuarantine(0x434d4454u);
            return kIOReturnTimeout;
        }
        /* Spinning does not advance `waited`, so the timeout still counts
         * milliseconds actually slept and its meaning is unchanged. */
        if (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) < spinDeadline) continue;
        waited++;
        __atomic_fetch_add(&s->spinFellThrough, 1, __ATOMIC_RELAXED);
        /* Sleep the same millisecond the poll always slept, but wake early
         * when the completion event arrives. The polled sleep remains the
         * fallback for every path with no event queue yet, which is all of
         * Start. `waited` is now only a statistic: the timeout above is
         * wall-clock. */
        /* Consume the flag rather than testing it: an event names the slot
         * slightly before the descriptor's ownership bit is visible, and a
         * flag left set would spin on that gap for the whole timeout. Taking
         * it means at most one extra pass per event. */
        if (__atomic_exchange_n(&sl->done, 0, __ATOMIC_ACQ_REL)) continue;
        uint64_t newGeneration = cmdGeneration;
        if (s->core->WaitCommandEvent(cmdGeneration, 1, &newGeneration) ==
            kIOReturnNotReady)
            IOSleep(1);
        cmdGeneration = newGeneration;
    }
    __atomic_fetch_add(&s->commandsIssued, 1, __ATOMIC_RELAXED);

    mlxDmaReadBarrier();
    s->lastDeliveryStatus = (lay->status_own >> 1) & 0x7f;
    if (s->lastDeliveryStatus) {
        /* A delivery failure is not a confirmed firmware outbox rejection. */
        s->quarantined = true;
        s->core->EnterDmaQuarantine(0x434d4445u);
        return kIOReturnIOError;
    }
    /* Command latency delta (notes/35): waited ~ the number of milliseconds of polling. */
    uint32_t latencyMs = waited;

    /* Copy response header + output mailbox blocks (cmd.c:1007). */
    uint32_t copyLen = (outSize < 16) ? outSize : 16;
    memcpy(out, lay->out, copyLen);
    for (uint32_t i = 0; i < sl->out.blocks; i++) {
        uint32_t copied = 16 + i * MLX_CMD_DATA_BLOCK_SIZE;
        uint32_t mbLen = outSize - copied;
        if (mbLen > MLX_CMD_DATA_BLOCK_SIZE) mbLen = MLX_CMD_DATA_BLOCK_SIZE;
        memcpy((uint8_t *)out + copied, sl->out.box[i]->data, mbLen);
    }

    /* Descriptor delivery status (bits [7:1]). */
    uint8_t status = (lay->status_own >> 1) & 0x7F;
    s->lastDeliveryStatus = status;
    if (status != 0) {
        MLX_LOG("opcode=0x%04x delivery_status=%u latency=%u ms (raw status_own=0x%02x)",
                opcode, status, latencyMs, lay->status_own);
        MLX_DBG("DBG lay: type=%02x inlen=%u in_ptr=%llx outlen=%u out_ptr=%llx tok=%u sig=%02x",
                lay->type, OSSwapBigToHostInt32(lay->inlen),
                (unsigned long long)OSSwapBigToHostInt64(lay->in_ptr),
                OSSwapBigToHostInt32(lay->outlen),
                (unsigned long long)OSSwapBigToHostInt64(lay->out_ptr),
                lay->token, lay->sig);
        return kIOReturnIOError;
    }

    /* Firmware outbox status (REMEDIATION_PLAN §5.1). */
    MlxP1OutboxStatus outbox = {};
    mlxP1ParseOutbox((const uint8_t *)out, outSize, &outbox);
    s->lastFwStatus = outbox.status;
    s->lastSyndrome = outbox.syndrome;
    if (outbox.status) {
        MLX_LOG("opcode=0x%04x fw_status=%u syndrome=0x%08x",
                opcode, outbox.status, outbox.syndrome);
        switch (outbox.status) {
        case 2:  return kIOReturnUnsupported;
        case 3: case 5: case 9: case 0x0a: case 0x10: case 0x30: case 0x40:
            return kIOReturnBadArgument;
        case 6:  return kIOReturnBusy;
        case 8:  return kIOReturnNoResources;
        case 4:  return kIOReturnNotReady;
        case 0x0f: return kIOReturnNoSpace;
        default: return kIOReturnIOError;
        }
    }
    return kIOReturnSuccess;
}

/* ---- debug/snapshot accessors ---- */

uint16_t
MlxCmd::CmdifRev() const
{
    return s ? s->cmdifRev : 0;
}

uint64_t
MlxCmd::CmdqIOVA() const
{
    return s ? s->cmdqIOVA : 0;
}

uint8_t
MlxCmd::LogSz() const
{
    return s ? s->logSz : 0;
}

uint8_t
MlxCmd::LogStride() const
{
    return s ? s->logStride : 0;
}

bool
MlxCmd::IsUp() const
{
    return s ? s->up : false;
}

bool
MlxCmd::IsQuarantined() const
{
    return s ? s->quarantined : false;
}

uint32_t MlxCmd::LastOpcode() const { return s ? s->lastOpcode : 0; }
uint32_t MlxCmd::LastSyndrome() const { return s ? s->lastSyndrome : 0; }
void
MlxCmd::CommandStats(uint64_t *issued, uint64_t *slept,
                     uint64_t *slotWaits) const
{
    if (issued) *issued = s ? s->commandsIssued : 0;
    if (slept)  *slept  = s ? s->spinFellThrough : 0;
    if (slotWaits) *slotWaits = s ? s->slotWaits : 0;
}
uint8_t MlxCmd::LastDeliveryStatus() const
{ return s ? s->lastDeliveryStatus : 0; }
uint8_t MlxCmd::LastFwStatus() const { return s ? s->lastFwStatus : 0; }
