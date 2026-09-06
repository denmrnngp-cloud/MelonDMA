#ifndef MLX_SAFETY_HPP
#define MLX_SAFETY_HPP
#include <stdint.h>

/* PCI Command bits, not PCI Status. Keep MSE unchanged when fencing DMA. */
static inline uint16_t mlxFencedPciCommand(uint16_t command)
{ return command & (uint16_t)~0x4u; }
static inline bool mlxPciFenceVerified(uint16_t before, uint16_t after)
{ return before != 0xffffu && after != 0xffffu && !(after & 4u) &&
         (before & 2u) == (after & 2u); }

/* Overflow-safe byte quotas: reserve before preparing any DMA mapping. */
static inline bool mlxBytesCanReserve(uint64_t used, uint64_t bytes, uint64_t limit)
{ return bytes && used <= limit && bytes <= limit - used; }

/* Conservative page footprint, including a possibly unaligned first page.
 * No VA-only accounting: a one-byte MR may pin a full host page. */
static inline uint64_t mlxPinnedCharge(uint64_t bytes, uint64_t page)
{
    if (!bytes || !page || (page & (page - 1)) || page > UINT64_MAX / 2 ||
        bytes > UINT64_MAX - 2 * (page - 1)) return 0;
    return (bytes + 2 * (page - 1)) & ~(page - 1);
}

template <class Segment>
static inline bool mlxDmaSegmentsCover(const Segment *segments, uint32_t count,
                                       uint64_t length)
{
    if (!segments || !count || count > 32 || !length) return false;
    uint64_t total = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (!segments[i].length || segments[i].address > UINT64_MAX - segments[i].length ||
            total > length || segments[i].length > length - total) return false;
        total += segments[i].length;
    }
    return total == length;
}

/* Preserve logical-page order across fragmented IOVAs. Adjacent segments
 * may split a page only if they continue the very same physical page. */
template <class Segment>
static inline bool mlxBuildSegmentPas(const Segment *segments, uint32_t count,
    uint64_t va, uint64_t length, uint64_t *pas, uint32_t capacity, uint32_t *produced)
{
    if (!pas || !produced || va > UINT64_MAX - length ||
        !mlxDmaSegmentsCover(segments, count, length)) return false;
    uint32_t n = 0;
    uint64_t logical = va;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t dma = segments[i].address, left = segments[i].length;
        if ((dma & 4095u) != (logical & 4095u)) return false;
        while (left) {
            uint64_t base = dma & ~4095ull;
            if (!n || !(logical & 4095u)) {
                if (n == capacity) return false;
                pas[n++] = base;
            } else if (pas[n - 1] != base) return false;
            uint64_t step = 4096 - (logical & 4095u);
            if (step > left) step = left;
            logical += step; dma += step; left -= step;
        }
    }
    *produced = n;
    return true;
}

/* Proof is per completion vector and reset epoch, never "any IRQ ever". */
static inline uint32_t mlxEqPollPeriodMs(bool liveCq, bool completionIrqProven)
{ return !liveCq ? 100u : completionIrqProven ? 50u : 1u; }
#endif
