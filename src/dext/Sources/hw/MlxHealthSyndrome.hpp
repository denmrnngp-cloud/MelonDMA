/*
 * MlxHealthSyndrome.hpp — firmware health syndrome names.
 *
 * Values from enum MLX5_INITIAL_SEG_HEALTH_SYNDROME_* in mlx5_ifc.h; strings
 * from hsynd_str() in drivers/net/ethernet/mellanox/mlx5/core/health.c (Linux
 * v5.9). The project's rule is to never write such a table from memory — this
 * one is transcribed from those two ground-truth sources.
 */
#ifndef MLX_HEALTH_SYNDROME_HPP
#define MLX_HEALTH_SYNDROME_HPP

#include <stdint.h>

static inline const char *mlxHealthSyndromeName(uint8_t synd)
{
    switch (synd) {
    case 0x0:  return "none";
    case 0x1:  return "firmware internal error";
    case 0x7:  return "irisc not responding";
    case 0x8:  return "unrecoverable hardware error";
    case 0x9:  return "firmware CRC error";
    case 0xa:  return "ICM fetch PCI error";
    case 0xb:  return "HW fatal error";
    case 0xc:  return "async EQ buffer overrun";
    case 0xd:  return "EQ error";
    case 0xe:  return "invalid EQ referenced";
    case 0xf:  return "FFSER error";
    case 0x10: return "high temperature";
    default:   return "unrecognized error";
    }
}

#endif /* MLX_HEALTH_SYNDROME_HPP */
