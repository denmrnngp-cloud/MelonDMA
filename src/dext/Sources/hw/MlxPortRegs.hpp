/*
 * MlxPortRegs.hpp — PTYS / PFCC register decode (mlx5_ifc.h ground truth).
 *
 * Offsets and bit widths come from include/linux/mlx5/mlx5_ifc.h and the
 * legacy protocol table (enum mlx5e_link_mode) in include/linux/mlx5/port.h
 * from the Linux v5.9 tree — the same header set the rest of this driver was
 * checked against. Nothing here is recalled.
 */
#ifndef MLX_PORT_REGS_HPP
#define MLX_PORT_REGS_HPP

#include <stdint.h>

/* PTYS proto_mask values (mlx5_ifc.h: MLX5_PTYS_IB / MLX5_PTYS_EN). */
enum {
    MLX_PTYS_IB = 1 << 0,
    MLX_PTYS_EN = 1 << 2,
};

/* PTYS field bit offsets (struct mlx5_ifc_ptys_reg_bits). */
enum {
    MLX_PTYS_LOCAL_PORT           = 0x08,   /* 8 bits */
    MLX_PTYS_PROTO_MASK           = 0x1d,   /* 3 bits */
    MLX_PTYS_ETH_PROTO_CAPABILITY = 0x60,   /* 32 bits */
    MLX_PTYS_ETH_PROTO_ADMIN      = 0xc0,   /* 32 bits */
    MLX_PTYS_ETH_PROTO_OPER       = 0x120,  /* 32 bits */
};

/* PAOS (port administrative/operational status), mlx5_ifc_paos_reg_bits.
 * A write must set ASE together with admin_status; without ASE the firmware
 * treats the command as an observation-only register access. */
enum {
    MLX_PAOS_LOCAL_PORT   = 0x08,  /* 8 bits */
    MLX_PAOS_ADMIN_STATUS = 0x14,  /* 4 bits */
    MLX_PAOS_OPER_STATUS  = 0x1c,  /* 4 bits */
    MLX_PAOS_ASE          = 0x20,  /* 1 bit */
};

/* PFCC field bit offsets (struct mlx5_ifc_pfcc_reg_bits). */
enum {
    MLX_PFCC_PRIO_MASK_TX = 0x28,   /* 8 bits: per-priority PFC tx enable */
    MLX_PFCC_PRIO_MASK_RX = 0x38,   /* 8 bits */
    MLX_PFCC_PPTX         = 0x40,   /* 1 bit: 802.3x pause transmit */
    MLX_PFCC_APTX         = 0x41,   /* 1 bit: pause tx auto-negotiated */
    MLX_PFCC_PFCTX        = 0x48,   /* 8 bits: PFC tx per priority */
    MLX_PFCC_PPRX         = 0x60,   /* 1 bit: 802.3x pause receive */
    MLX_PFCC_APRX         = 0x61,   /* 1 bit: pause rx auto-negotiated */
    MLX_PFCC_PFCRX        = 0x68,   /* 8 bits: PFC rx per priority */
};

/*
 * Decode a PTYS eth_proto_* word to a link speed in Mbps. eth_proto_oper is a
 * single set bit; capability/admin may carry several, in which case the
 * highest set bit names the fastest advertised mode. Bit position is the
 * mlx5e_link_mode index (include/linux/mlx5/port.h). Returns 0 for zero or an
 * unknown bit.
 */
static inline uint32_t mlxPtysSpeedMbps(uint32_t eth_proto)
{
    if (!eth_proto) return 0;
    uint32_t bit = 31u - (uint32_t)__builtin_clz(eth_proto);
    switch (bit) {
    case 0: case 1: return 1000;       /* 1000BASE_CX_SGMII / 1000BASE_KX */
    case 2: case 3: case 4:            /* 10GBASE_CX4/KX4/KR */
    case 12: case 13: case 14: case 26: return 10000;  /* CR/SR/ER/T */
    case 5: return 20000;              /* 20GBASE_KR2 */
    case 6: case 7: case 15: case 16: return 40000;    /* CR4/KR4/SR4/LR4 */
    case 8: return 56000;              /* 56GBASE_R4 */
    case 18: case 30: case 31: return 50000;           /* SR2/CR2/KR2 */
    case 20: case 21: case 22: case 23: return 100000; /* CR4/SR4/KR4/LR4 */
    case 24: return 100;               /* 100BASE_TX */
    case 25: return 1000;              /* 1000BASE_T */
    case 27: case 28: case 29: return 25000;           /* 25GBASE CR/KR/SR */
    default: return 0;
    }
}

#endif /* MLX_PORT_REGS_HPP */
