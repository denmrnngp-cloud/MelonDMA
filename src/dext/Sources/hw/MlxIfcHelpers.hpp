/*
 * MlxIfcHelpers.hpp - portable mlx5 IFC bitfield helpers
 */
#ifndef MLX_IFC_HELPERS_HPP
#define MLX_IFC_HELPERS_HPP

#include <stdint.h>

/* mlx5 IFC offsets count bits from the most significant bit of byte zero. */
static inline void
mlxSetBits(void *buffer, uint32_t bitOffset, uint32_t bitWidth, uint64_t value)
{
    uint8_t *bytes = static_cast<uint8_t *>(buffer);
    for (uint32_t i = 0; i < bitWidth; i++) {
        uint32_t dstBit = bitOffset + i;
        uint8_t mask = static_cast<uint8_t>(1u << (7 - (dstBit & 7)));
        uint64_t srcMask = 1ULL << (bitWidth - i - 1);
        if (value & srcMask)
            bytes[dstBit >> 3] |= mask;
        else
            bytes[dstBit >> 3] &= static_cast<uint8_t>(~mask);
    }
}

static inline uint64_t
mlxGetBits(const void *buffer, uint32_t bitOffset, uint32_t bitWidth)
{
    const uint8_t *bytes = static_cast<const uint8_t *>(buffer);
    uint64_t value = 0;
    for (uint32_t i = 0; i < bitWidth; i++) {
        uint32_t srcBit = bitOffset + i;
        value <<= 1;
        value |= (bytes[srcBit >> 3] >> (7 - (srcBit & 7))) & 1;
    }
    return value;
}

#endif /* MLX_IFC_HELPERS_HPP */
