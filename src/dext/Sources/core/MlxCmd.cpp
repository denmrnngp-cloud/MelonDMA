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
    uint8_t                   token;
    bool                      up;
    bool                      quarantined;
    uint32_t                  lastOpcode;
    uint32_t                  lastSyndrome;
    uint8_t                   lastDeliveryStatus;
    uint8_t                   lastFwStatus;
    IOLock                   *execLock;

    /* In-flight mailbox chains for the current command (single-slot MVP). */
    IOBufferMemoryDescriptor *inMailboxDesc[MLX_CMD_MAX_BLOCKS];
    IOBufferMemoryDescriptor *outMailboxDesc[MLX_CMD_MAX_BLOCKS];
    IODMACommand            *inMailboxDma[MLX_CMD_MAX_BLOCKS];
    IODMACommand            *outMailboxDma[MLX_CMD_MAX_BLOCKS];
    MlxCmdMailbox           *inMailbox[MLX_CMD_MAX_BLOCKS];
    MlxCmdMailbox           *outMailbox[MLX_CMD_MAX_BLOCKS];
    uint64_t                 inMailboxIOVA[MLX_CMD_MAX_BLOCKS];
    uint64_t                 outMailboxIOVA[MLX_CMD_MAX_BLOCKS];
    uint32_t                 inNumBlocks;
    uint32_t                 outNumBlocks;
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
    s->token = 1;
    s->up = true;
    MLX_LOG("ready (rev=%u, log_sz=%u, stride=%u, iova=0x%llx)",
            s->cmdifRev, s->logSz, s->logStride, s->cmdqIOVA);
    return kIOReturnSuccess;
}

void
MlxCmd::Free()
{
    if (!s) return;
    s->up = false;
    FreeMailbox(false);
    FreeMailbox(true);
    if (s->cmdqDma) { mlxCompleteDma(s->cmdqDma); s->cmdqDma = NULL; }
    if (s->cmdqMem) { s->cmdqMem->release(); s->cmdqMem = NULL; }
    if (s->execLock) { IOLockFree(s->execLock); s->execLock = NULL; }
    delete s; s = NULL;
}

/* ---- mailbox chain helpers (cmd.c:allocMailbox / freeMailbox) ---- */

kern_return_t
MlxCmd::AllocMailbox(bool out, uint32_t size)
{
    /* Number of blocks needed for `size` bytes after the 16B inline header. */
    uint32_t numBlocks = 0;
    if (size > 16)
        numBlocks = (size - 16 + MLX_CMD_DATA_BLOCK_SIZE - 1) / MLX_CMD_DATA_BLOCK_SIZE;
    if (numBlocks > MLX_CMD_MAX_BLOCKS) {
        MLX_LOG("command too large: needs %u blocks (max %u)", numBlocks, MLX_CMD_MAX_BLOCKS);
        return kIOReturnNoSpace;
    }
    if (out) s->outNumBlocks = numBlocks; else s->inNumBlocks = numBlocks;

    for (uint32_t i = 0; i < numBlocks; i++) {
        IOBufferMemoryDescriptor **descp = out ? &s->outMailboxDesc[i] : &s->inMailboxDesc[i];
        IODMACommand            **dmap   = out ? &s->outMailboxDma[i]   : &s->inMailboxDma[i];
        MlxCmdMailbox           **boxp    = out ? &s->outMailbox[i]      : &s->inMailbox[i];
        uint64_t                *iovap    = out ? &s->outMailboxIOVA[i] : &s->inMailboxIOVA[i];

        /* [FIX v0.38] allocate the mailbox buffer as a WHOLE 4096 page with
         * 4096 alignment (like AppleMCX: inTaskWithPhysicalMask with mask
         * 0xFFFFFFF000). Previously it was 576 bytes with 1024 alignment — the
         * DART segment came out as {iova, 576}, and fw, when DMA-reading the
         * mailbox, ran past the end of the non-page segment →
         * delivery_status=6 (FW_ERR). */
        kern_return_t kr = mlxAllocDmaBuffer(4096, 4096,
                                             kIOMemoryDirectionOutIn, descp);
        if (kr != kIOReturnSuccess || !*descp) {
            MLX_LOG("mailbox alloc failed: 0x%x", kr);
            FreeMailbox(out);
            return kr ? kr : kIOReturnNoMemory;
        }

        IOAddressSegment segs[32];
        uint32_t segCount = 32;
        kr = mlxPrepareDma(s->pci, *descp, segs, &segCount, dmap);
        if (kr != kIOReturnSuccess || segCount == 0) {
            MLX_LOG("mailbox DMA prepare failed: 0x%x", kr);
            (*descp)->release(); *descp = NULL;
            FreeMailbox(out);
            return kr ? kr : kIOReturnNoMemory;
        }
        /* DART segment diagnostics (notes/29): the mailbox must be a single
         * contiguous segment ≥ 576 bytes — otherwise fw reads out of bounds. */
        if (!out && i == 0) {
            MLX_LOG("DBG mb DMA: segs=%u seg0={0x%llx, %llu}", segCount,
                    (unsigned long long)segs[0].address,
                    (unsigned long long)segs[0].length);
        }
        if (segCount != 1 || segs[0].length < sizeof(MlxCmdMailbox)) {
            MLX_LOG("mailbox NOT a single segment/too short: segs=%u len=%llu — fw will not be able to read",
                    segCount, (unsigned long long)segs[0].length);
        }
        *iovap = segs[0].address;

        uint64_t addr = 0, len = 0;
        kr = (*descp)->Map(0, 0, 0, 0, &addr, &len);
        if (kr != kIOReturnSuccess) {
            MLX_LOG("mailbox CPU map failed: 0x%x", kr);
            FreeMailbox(out);
            return kr;
        }
        *boxp = (MlxCmdMailbox *)(uintptr_t)addr;
        memset(*boxp, 0, sizeof(MlxCmdMailbox));
    }
    return kIOReturnSuccess;
}

void
MlxCmd::FreeMailbox(bool out)
{
    uint32_t n = out ? s->outNumBlocks : s->inNumBlocks;
    for (uint32_t i = 0; i < n; i++) {
        IODMACommand **dmap  = out ? &s->outMailboxDma[i]   : &s->inMailboxDma[i];
        IOBufferMemoryDescriptor **descp = out ? &s->outMailboxDesc[i] : &s->inMailboxDesc[i];
        if (*dmap) { mlxCompleteDma(*dmap); *dmap = NULL; }
        if (*descp) { (*descp)->release(); *descp = NULL; }
        if (out) s->outMailbox[i] = NULL; else s->inMailbox[i] = NULL;
    }
    if (out) s->outNumBlocks = 0; else s->inNumBlocks = 0;
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

kern_return_t
MlxCmd::Exec(uint32_t opcode, const void *in, uint32_t inSize,
             void *out, uint32_t outSize, uint32_t timeoutMs)
{
    if (!s || !s->up || !s->execLock) return kIOReturnNotReady;
    IOLockLock(s->execLock);
    kern_return_t kr = ExecLocked(opcode, in, inSize, out, outSize, timeoutMs);
    IOLockUnlock(s->execLock);
    return kr;
}

kern_return_t
MlxCmd::ExecLocked(uint32_t opcode, const void *in, uint32_t inSize,
                   void *out, uint32_t outSize, uint32_t timeoutMs)
{
    if (!s || !s->up) return kIOReturnNotReady;
    s->lastOpcode = opcode;
    s->lastSyndrome = 0;
    s->lastDeliveryStatus = 0;
    s->lastFwStatus = 0;
    if (!in || inSize == 0 || inSize > MLX_CMD_MAX_SIZE ||
        !out || outSize < 8 || outSize > MLX_CMD_MAX_SIZE) {
        return kIOReturnBadArgument;
    }
    if (s->quarantined && opcode != MLX_CMD_OP_TEARDOWN_HCA &&
        opcode != MLX_CMD_OP_DISABLE_HCA)
        return kIOReturnNotReady;

    /* Allocate mailbox chains for large commands. */
    kern_return_t kr = AllocMailbox(false, inSize);
    if (kr != kIOReturnSuccess) return kr;
    kr = AllocMailbox(true, outSize);
    if (kr != kIOReturnSuccess) { FreeMailbox(false); return kr; }

    /* Slot selection (cmd.c:1032): regular commands use slots 0..N-2, the
     * LAST slot (max_reg_cmds = (1<<log_sz)-1) is reserved exclusively for
     * MANAGE_PAGES. Firmware rejects MANAGE_PAGES on any other slot with
     * delivery_status=6 (FW_ERR) — exactly what we saw on GIVE. */
    uint32_t slot = (opcode == MLX_CMD_OP_MANAGE_PAGES) ?
        ((1u << s->logSz) - 1) : 0;

    MlxCmdLayout *lay = (MlxCmdLayout *)
        ((uint8_t *)s->cmdqBuf + ((size_t)slot << s->logStride));
    memset(lay, 0, sizeof(*lay));

    /* Command header: first 16 bytes. */
    memcpy(lay->in, in, (inSize < 16) ? inSize : 16);

    /* Large input → mailbox chain (cmd.c:980). */
    for (uint32_t i = 0; i < s->inNumBlocks; i++) {
        MlxCmdMailbox *mb = s->inMailbox[i];
        uint32_t copied = 16 + i * MLX_CMD_DATA_BLOCK_SIZE;
        uint32_t dataLen = inSize - copied;
        if (dataLen > MLX_CMD_DATA_BLOCK_SIZE) dataLen = MLX_CMD_DATA_BLOCK_SIZE;
        memcpy(mb->data, (const uint8_t *)in + copied, dataLen);
        mb->next      = OSSwapHostToBigInt64(
            (i + 1 < s->inNumBlocks) ? s->inMailboxIOVA[i + 1] : 0);
        mb->block_num = OSSwapHostToBigInt32(i);   /* big-endian order (AppleMCX) */
        mb->token     = s->token;
        SetMailboxSignature(mb);
    }
    lay->in_ptr = OSSwapHostToBigInt64(
        s->inNumBlocks ? s->inMailboxIOVA[0] : 0);
    lay->inlen  = OSSwapHostToBigInt32(inSize);

    /* Output mailbox chain (pre-linked, no data yet). */
    for (uint32_t i = 0; i < s->outNumBlocks; i++) {
        MlxCmdMailbox *mb = s->outMailbox[i];
        mb->next      = OSSwapHostToBigInt64(
            (i + 1 < s->outNumBlocks) ? s->outMailboxIOVA[i + 1] : 0);
        mb->block_num = OSSwapHostToBigInt32(i);   /* big-endian order (AppleMCX) */
        mb->token     = s->token;
        SetMailboxSignature(mb);
    }
    lay->out_ptr = OSSwapHostToBigInt64(
        s->outNumBlocks ? s->outMailboxIOVA[0] : 0);
    lay->outlen = OSSwapHostToBigInt32(outSize);

    lay->type  = MLX_CMD_TYPE_XPORT;
    lay->token = s->token++;

    /* Hand ownership to firmware + signature (cmd.c:228). */
    lay->status_own = MLX_CMD_OWNER_HW;
    lay->sig = 0;
    lay->sig = (uint8_t)~xor8(lay, 0, sizeof(*lay));

    /* Doorbell: set slot bit (cmd.c:1069 writes 1 << ent->idx). */
    mlxMemoryBarrier();
    mlxMMIOWrite32BE(s->pci, s->barIndex,
                     offsetof(struct MlxInitSeg, cmd_dbell), 1u << slot);

    /* Poll for completion (cmd.c:237). */
    uint32_t waited = 0;
    while (true) {
        mlxMemoryBarrier();
        if (!(lay->status_own & MLX_CMD_OWNER_HW)) break;
        if (timeoutMs && waited++ >= timeoutMs) {
            s->quarantined = true;
            MLX_LOG("opcode 0x%x timed out; quarantined", opcode);
            FreeMailbox(false); FreeMailbox(true);
            return kIOReturnTimeout;
        }
        IOSleep(1);
    }

    /* Command latency delta (notes/35): waited ~ the number of milliseconds of polling. */
    uint32_t latencyMs = waited;

    /* Copy response header + output mailbox blocks (cmd.c:1007). */
    uint32_t copyLen = (outSize < 16) ? outSize : 16;
    memcpy(out, lay->out, copyLen);
    for (uint32_t i = 0; i < s->outNumBlocks; i++) {
        uint32_t copied = 16 + i * MLX_CMD_DATA_BLOCK_SIZE;
        uint32_t mbLen = outSize - copied;
        if (mbLen > MLX_CMD_DATA_BLOCK_SIZE) mbLen = MLX_CMD_DATA_BLOCK_SIZE;
        memcpy((uint8_t *)out + copied, s->outMailbox[i]->data, mbLen);
    }

    FreeMailbox(false);
    FreeMailbox(true);

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
uint8_t MlxCmd::LastDeliveryStatus() const
{ return s ? s->lastDeliveryStatus : 0; }
uint8_t MlxCmd::LastFwStatus() const { return s ? s->lastFwStatus : 0; }
