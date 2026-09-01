/*
 * MlxKeyIndex.hpp — open-addressing hash index: uint32 key → slot+1.
 *
 * Dependency-free (host-testable, see Tests/test_mkey_index.c). Used by
 * MlxMR/MlxQP/MlxCQ to turn lkey/rkey/qpn/cqn lookups from O(table) into O(1).
 *
 * Bucket layout: `keys[h]` holds the key, `slots[h]` holds slot+1
 * (0 = empty, -1 = tombstone). Linear probing. The table is caller-sized:
 * `mask`/`shift` describe the capacity (a power of two, sized 2× the live
 * entry count so load factor stays ≤ 0.5). Reads are lock-free; writes happen
 * under the caller's lock.
 */
#ifndef MLX_KEY_INDEX_HPP
#define MLX_KEY_INDEX_HPP

#include <stdint.h>

/* Smallest power of two >= 2*entries, so load factor stays <= 0.5. */
static inline uint32_t
mlxKeyIndexCap(uint32_t entries)
{
    uint64_t need = (uint64_t)entries * 2u;
    if (need < 2u) need = 2u;
    uint32_t cap = 1u;
    while ((uint64_t)cap < need) cap <<= 1u;
    return cap;
}

/* shift = 32 - log2(cap), i.e. the number of leading zeros of cap-1
 * (for cap = 2^k, cap-1 has k bits, so clz = 32-k). */
static inline uint32_t
mlxKeyIndexShift(uint32_t cap)
{
    return (uint32_t)__builtin_clz(cap - 1u);
}

/* Multiplicative hash taking the high bits of the 32-bit product (mixes all
 * input bits; keys here are (index<<8)|variant). */
static inline uint32_t
mlxKeyIndexHash(uint32_t key, uint32_t shift)
{
    return (uint32_t)((key * 0x9E3779B9u) >> shift);
}

static inline void
mlxKeyIndexInsert(uint32_t *keys, int32_t *slots, uint32_t mask, uint32_t shift,
                  uint32_t key, int slot)
{
    uint32_t h = mlxKeyIndexHash(key, shift);
    while (slots[h] > 0) h = (h + 1) & mask;
    keys[h] = key;
    slots[h] = slot + 1;
}

/* Returns the slot for `key`, or -1. Tombstones are skipped, empty ends the
 * probe chain. */
static inline int
mlxKeyIndexFind(const uint32_t *keys, const int32_t *slots, uint32_t mask,
                uint32_t shift, uint32_t key)
{
    uint32_t h = mlxKeyIndexHash(key, shift);
    while (slots[h] != 0) {
        if (slots[h] > 0 && keys[h] == key) return slots[h] - 1;
        h = (h + 1) & mask;
    }
    return -1;
}

/* Marks the bucket as a tombstone (-1) so later inserts reuse it. */
static inline void
mlxKeyIndexRemove(uint32_t *keys, int32_t *slots, uint32_t mask, uint32_t shift,
                  uint32_t key)
{
    uint32_t h = mlxKeyIndexHash(key, shift);
    while (slots[h] != 0) {
        if (slots[h] > 0 && keys[h] == key) { slots[h] = -1; return; }
        h = (h + 1) & mask;
    }
}

#endif /* MLX_KEY_INDEX_HPP */
