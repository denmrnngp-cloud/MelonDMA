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
 * Large commands (>16B input or output) use a mailbox chain (one or more
 * 576-byte MlxCmdMailbox blocks linked via next, each DMA-pinned). This
 * covers ENABLE_HCA, QUERY_ISSI, QUERY_PAGES, INIT_HCA, and QUERY_HCA_CAP
 * (4112B output) — the full Gate P1 / Phase 1 bring-up sequence (notes/08).
 *
 * MVP scope: single command slot (no concurrency), polling completion. The
 * kext donor's 32-slot bitmap + event-mode completion is a later optimization.
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

#define MLX_CMD_MAX_SIZE        4112   /* max command input/output (QUERY_HCA_CAP) */
#define MLX_CMD_DATA_BLOCK_SIZE 512    /* mailbox data block (mlx5_cmd_prot_block) */
#define MLX_CMD_MAX_BLOCKS      8      /* 8 * 512 = 4096B + 16B header = 4112B */

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

    uint16_t        CmdifRev() const;

    /* Debug/snapshot accessors (used by mlx_probe / DbgDumpState). */
    uint64_t        CmdqIOVA() const;
    uint8_t         LogSz() const;
    uint8_t         LogStride() const;
    bool            IsUp() const;
    bool            IsQuarantined() const;
    uint32_t        LastOpcode() const;
    uint32_t        LastSyndrome() const;
    uint8_t         LastDeliveryStatus() const;
    uint8_t         LastFwStatus() const;

private:
    struct State;
    State *s;

    kern_return_t   ExecLocked(uint32_t opcode, const void *in, uint32_t inSize,
                               void *out, uint32_t outSize, uint32_t timeoutMs);

    /* Mailbox chain helpers (cmd.c:allocMailbox / freeMailbox / setMailboxSignature). */
    kern_return_t   AllocMailbox(bool out, uint32_t size);
    void            FreeMailbox(bool out);
    static void     SetMailboxSignature(MlxCmdMailbox *mb);
};

#endif /* MLX_CMD_HPP */
