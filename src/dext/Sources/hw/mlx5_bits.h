/*
 * mlx5_bits.h — Bit manipulation helpers for mlx5 IFC bitfields.
 *
 * The mlx5 command interface uses bit-aligned fields (not byte-aligned).
 * These helpers write/read arbitrary bit ranges within a byte buffer.
 *
 * Ported from: AppleMCX Sources/hw/MlxRegs.hpp
 */
#ifndef MLX5_BITS_H
#define MLX5_BITS_H

#include <stdint.h>
#include <string.h>

/* Write value to bit range [offset, offset+bits) in buffer */
static inline void mlxSetBits(void *buf, uint32_t bitOffset,
                              uint32_t bits, uint64_t value)
{
    uint8_t *b = (uint8_t *)buf;
    uint32_t byteOff = bitOffset / 8;
    uint32_t bitInByte = bitOffset % 8;
    uint32_t remaining = bits;
    uint64_t val = value;

    while (remaining > 0) {
        uint32_t thisByte = (remaining + bitInByte > 8)
                            ? (8 - bitInByte) : remaining;
        uint8_t mask = (uint8_t)((1u << thisByte) - 1u);
        uint8_t shift = bitInByte;
        b[byteOff] &= ~(mask << shift);
        b[byteOff] |= (uint8_t)((val & mask) << shift);
        val >>= thisByte;
        remaining -= thisByte;
        byteOff++;
        bitInByte = 0;
    }
}

/* Read value from bit range [offset, offset+bits) in buffer */
static inline uint64_t mlxGetBits(const void *buf, uint32_t bitOffset,
                                  uint32_t bits)
{
    const uint8_t *b = (const uint8_t *)buf;
    uint32_t byteOff = bitOffset / 8;
    uint32_t bitInByte = bitOffset % 8;
    uint32_t remaining = bits;
    uint64_t val = 0;
    uint32_t shift = 0;

    while (remaining > 0) {
        uint32_t thisByte = (remaining + bitInByte > 8)
                            ? (8 - bitInByte) : remaining;
        uint8_t mask = (uint8_t)((1u << thisByte) - 1u);
        val |= (uint64_t)((b[byteOff] >> bitInByte) & mask) << shift;
        shift += thisByte;
        remaining -= thisByte;
        byteOff++;
        bitInByte = 0;
    }
    return val;
}

#endif /* MLX5_BITS_H */
