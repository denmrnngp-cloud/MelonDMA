/*
 * MlxDoorbell.hpp — Doorbell mechanism (generic Mellanox mlx5 family)
 *
 * Ported from: kernel_src/mlnx-ofed-kernel-5.9/include/linux/mlx5/doorbell.h
 * and include/linux/mlx5/qp.h (DB record)
 */
#ifndef MLX_DOORBELL_HPP
#define MLX_DOORBELL_HPP

#include <stdint.h>

/* Doorbell offsets are defined in MlxRegs.hpp: MLX_BF_OFFSET / MLX_CQ_DOORBELL / MLX_EQ_DOORBELL */

/*
 * DB record indexes (See qp.h:185 enum mlx5_db_type)
 * A DB record is memory writable from user space that holds the queue head
 * pointer; the hardware reads it.
 */
enum {
    MLX_SND_DBR   = 1,   /* index of the send-queue DB record in db[] */
    MLX_CQ_DBR    = 0,   /* CQ DB record */
    MLX_RCV_DBR   = 0,   /* receive-queue DB record */
};

/*
 * 64-bit doorbell write — equivalent to Linux mlx5_write64
 * Requirement: val 8-byte aligned, dest 8-byte aligned (arm64 guarantees atomicity)
 */
static inline void MlxWrite64(uint32_t val[2], volatile void *dest)
{
    volatile uint64_t *d = (volatile uint64_t *)dest;
    uint64_t packed = ((uint64_t)val[1] << 32) | (uint64_t)val[0];
    *d = packed;   /* arm64 8-byte aligned volatile write = atomic write */
}

/*
 * Send doorbell assembly (See wr.c:1044 mlx5r_ring_db)
 *   low 32 bits: updated DB record value (head pointer)
 *   high 32 bits: reserved (full format implemented in P4)
 */
static inline uint32_t MlxSendDbValue(uint32_t dbRecord)
{
    return dbRecord;
}

#endif /* MLX_DOORBELL_HPP */
