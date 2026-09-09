/*
 * MlxCmd.hpp — Firmware command interface (generic Mellanox mlx5 family)
 *
 * Ported from: kernel_src/mlnx-ofed-kernel-5.9/drivers/net/ethernet/mellanox/mlx5/core/cmd.c
 *
 * DriverKit port: the command queue is allocated via IOBufferMemoryDescriptor
 * and pinned with IODMACommand::PrepareForDMA. The HCA doorbell is rung via
 * IOPCIDevice::MemoryWrite32 on the init-segment cmd_dbell register, and the
 * command is polled for completion by reading the command outbox over MMIO.
 *
 * Large commands (>16B input or output) use a mailbox chain of full 4096-byte
 * DMA pages. Each page carries one 576-byte MlxCmdMailbox block linked via
 * next. This proven layout covers ENABLE_HCA, QUERY_ISSI, QUERY_PAGES,
 * INIT_HCA, QUERY_HCA_CAP and large inline-PAS CREATE_MKEY commands.
 *
 * Commands run on a small ring of slots with a busy bitmap, the way Apple's
 * DEXT does it (AppleEthernetMLX5Cmd keeps a 32-entry bitmap and completes
 * from the command EQ). Completion here is still polled, because the two
 * interrupt vectors this nub was granted are spoken for.
 */
#ifndef MLX_CMD_HPP
#define MLX_CMD_HPP

#include <stdint.h>
#include <DriverKit/IOReturn.h>
#include <DriverKit/IOMemoryDescriptor.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#include <DriverKit/IOMemoryMap.h>
#include "MlxRegs.hpp"

class MlxPCIDriver;
class IOPCIDevice;

#define MLX_CMD_MAX_SIZE        4112   /* max small command input/output (QUERY_HCA_CAP) */
#define MLX_CMD_DATA_BLOCK_SIZE 512    /* mailbox data block (mlx5_cmd_prot_block) */
#define MLX_CMD_MAX_BLOCKS      1024   /* large CREATE_MKEY: 149 MiB @ 4 KiB PAS = 597 blocks */
#define MLX_CMD_MAX_INPUT_SIZE  (16 + MLX_CMD_MAX_BLOCKS * MLX_CMD_DATA_BLOCK_SIZE) /* 524304 */

/* Mailbox block (576 bytes): 512B data + descriptor. device.h:781. */
struct MlxCmdMailbox {
    uint8_t  data[MLX_CMD_DATA_BLOCK_SIZE];
    uint8_t  rsvd0[48];
    uint64_t next;            /* pointer to next block (BE) */
    uint32_t block_num;       /* BE */
    uint8_t  rsvd1;
    uint8_t  token;
    uint8_t  ctrl_sig;
    uint8_t  sig;
} __attribute__((packed));

/* Command descriptor layout (64 bytes). device.h:525 mlx5_cmd_layout. */
struct MlxCmdLayout {
    uint8_t  type;            /* +0  MLX5_PCI_CMD_XPORT = 0x7 */
    uint8_t  rsvd0[3];
    uint32_t inlen;           /* +4  BE */
    uint64_t in_ptr;          /* +8  in mailbox pointer (BE) */
    uint32_t in[4];           /* +16 command header (first 16 bytes) */
    uint32_t out[4];          /* +32 response header */
    uint64_t out_ptr;         /* +48 out mailbox pointer (BE) */
    uint32_t outlen;          /* +56 BE */
    uint8_t  token;           /* +60 */
    uint8_t  sig;             /* +61 XOR checksum */
    uint8_t  rsvd1;
    uint8_t  status_own;      /* +63 [7:1]status [0]ownership: 0=SW,1=HW */
} __attribute__((packed));

#define MLX_CMD_OWNER_HW    (1u << 0)
#define MLX_CMD_TYPE_XPORT  0x7
#define MLX_CMDQ_SIZE       4096

/* Command slots. The hardware queue holds 1 << log_sz descriptors; the last is
 * reserved for MANAGE_PAGES, which firmware refuses anywhere else. The rest are
 * regular slots, capped here so that a machine with a large queue does not
 * allocate more slot state than concurrency can use — each slot carries block
 * arrays sized for a 149 MiB registration, so they are allocated on first use. */
/* The geometry check in Init allows log_sz up to 6 (log_sz + log_stride <= 12
 * with a stride of at least 6), so the queue can hold 64 descriptors and the
 * MANAGE_PAGES slot index can be 63. The busy mask is 64 bits for that reason;
 * a 32-bit one would shift past its width on such a queue. */
#define MLX_CMD_HW_SLOTS    64u
#define MLX_CMD_REG_SLOTS   4u

/* How large a mailbox chain a slot keeps between commands. Small commands
 * dominate and benefit from the cache; a 149 MiB registration needs 597 blocks
 * of pinned DMA and would hold 2.4 MiB per slot for the life of the driver,
 * while its own cost is firmware work measured in tens of milliseconds, so the
 * allocation it saves is noise. Apple's pool is stricter still: allocCmdMsg
 * reuses a cached message only when its block count matches exactly. */
#define MLX_CMD_CACHE_MAX_BLOCKS 64u

class MlxCmd {
public:
    MlxCmd();
    ~MlxCmd();

    kern_return_t   Init(MlxPCIDriver *core);
    void            Free();

    /* Execute a firmware command (See mlx5_cmd_exec). Returns kIOReturnSuccess
     * only when both descriptor delivery and firmware outbox status are zero. */
    kern_return_t   Exec(uint32_t opcode, const void *in, uint32_t inSize,
                         void *out, uint32_t outSize, uint32_t timeoutMs);

    /* A command-completion event named these slots. Sets each slot's done
     * flag; the driver wakes whoever is waiting. Safe to call with bits for
     * slots that are idle or already complete. */
    void            CompleteFromEvent(uint32_t mask);

    uint16_t        CmdifRev() const;

    /* Debug/snapshot accessors (used by mlx_probe / DbgDumpState). */
    uint64_t        CmdqIOVA() const;
    uint8_t         LogSz() const;
    uint8_t         LogStride() const;
    bool            IsUp() const;
    bool            IsQuarantined() const;
    uint32_t        LastOpcode() const;
    uint32_t        LastSyndrome() const;
    /* Firmware commands issued, and how many waits past the spin window they
     * needed. A wait is at most a millisecond and ends early when the
     * command-completion event arrives, so this is a count of waits, not of
     * milliseconds. A rising ratio means the spin budget is short for this
     * firmware or command mix; both are device-wide. */
    void            CommandStats(uint64_t *issued, uint64_t *slept,
                                 uint64_t *slotWaits) const;
    uint8_t         LastDeliveryStatus() const;
    uint8_t         LastFwStatus() const;

private:
    struct State;
    State *s;

    /* Slot arbitration: acquire, run, release. A timed-out slot is never
     * released, so nothing reuses a descriptor firmware may still own. */
    kern_return_t   AcquireSlot(uint32_t opcode, uint32_t timeoutMs,
                                uint32_t *outSlot);
    void            ReleaseSlot(uint32_t slot);
    kern_return_t   ExecOnSlot(uint32_t slot, uint32_t opcode, const void *in,
                               uint32_t inSize, void *out, uint32_t outSize,
                               uint32_t timeoutMs);

    /* Mailbox chain helpers. The chain is cached on its slot and reused; only
     * teardown and quarantine give the blocks back. */
    kern_return_t   AllocMailbox(struct MlxCmdChain *chain, uint32_t size);
    void            ReleaseChain(struct MlxCmdChain *chain);
    static void     SetMailboxSignature(MlxCmdMailbox *mb);
};

#endif /* MLX_CMD_HPP */
