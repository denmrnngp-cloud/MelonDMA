/*
 * test_mkey_index.c — host test for MlxKeyIndex.hpp (audit P0.2/P1.2).
 *
 * No hardware, no DriverKit. Exercises the open-addressing hash index used by
 * MlxMR/MlxQP/MlxCQ: insert/find, miss, forced collisions, tombstone remove +
 * re-insert, and a 512-key load roundtrip.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "MlxKeyIndex.hpp"

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); failures++; } } while (0)

#define N 512u

int main(void)
{
    uint32_t keys[2048];
    int32_t  slots[2048];
    uint32_t cap   = mlxKeyIndexCap(N);
    uint32_t mask  = cap - 1u;
    uint32_t shift = mlxKeyIndexShift(cap);

    /* ---- basic insert / find / miss ---- */
    memset(keys, 0, sizeof(keys));
    memset(slots, 0, sizeof(slots));
    mlxKeyIndexInsert(keys, slots, mask, shift, 0x12345, 7);
    CHECK(mlxKeyIndexFind(keys, slots, mask, shift, 0x12345) == 7, "basic find");
    CHECK(mlxKeyIndexFind(keys, slots, mask, shift, 0x99999) == -1, "miss on absent key");

    /* ---- forced collision: two distinct keys hashing to the same bucket ---- */
    uint32_t k1 = 0x1001, k2 = 0;
    uint32_t h1 = mlxKeyIndexHash(k1, shift);
    for (uint32_t cand = 0x2001; cand < 0x00ffffffu; cand += 0x101) {
        if (cand != k1 && mlxKeyIndexHash(cand, shift) == h1) { k2 = cand; break; }
    }
    CHECK(k2 != 0, "found a colliding key");
    mlxKeyIndexInsert(keys, slots, mask, shift, k1, 3);
    mlxKeyIndexInsert(keys, slots, mask, shift, k2, 9);
    CHECK(mlxKeyIndexFind(keys, slots, mask, shift, k1) == 3, "collision resolves k1");
    CHECK(mlxKeyIndexFind(keys, slots, mask, shift, k2) == 9, "collision resolves k2");

    /* ---- tombstone remove + re-insert ---- */
    mlxKeyIndexRemove(keys, slots, mask, shift, k1);
    CHECK(mlxKeyIndexFind(keys, slots, mask, shift, k1) == -1, "removed k1");
    CHECK(mlxKeyIndexFind(keys, slots, mask, shift, k2) == 9, "k2 intact after k1 tombstone");
    mlxKeyIndexInsert(keys, slots, mask, shift, 0xabcdef, 5);
    CHECK(mlxKeyIndexFind(keys, slots, mask, shift, 0xabcdef) == 5, "insert reuses tombstone");

    /* ---- remove of an absent key must not corrupt the table ---- */
    mlxKeyIndexRemove(keys, slots, mask, shift, 0xdeadbeef);
    CHECK(mlxKeyIndexFind(keys, slots, mask, shift, k2) == 9, "k2 intact after remove-miss");

    /* ---- 512-key load roundtrip ---- */
    memset(keys, 0, sizeof(keys));
    memset(slots, 0, sizeof(slots));
    for (uint32_t i = 0; i < N; i++) {
        uint32_t key = ((i + 1u) << 8) | ((i & 0xffu) + 1u);
        mlxKeyIndexInsert(keys, slots, mask, shift, key, (int)i);
    }
    int ok = 1;
    for (uint32_t i = 0; i < N; i++) {
        uint32_t key = ((i + 1u) << 8) | ((i & 0xffu) + 1u);
        if (mlxKeyIndexFind(keys, slots, mask, shift, key) != (int)i) { ok = 0; break; }
    }
    CHECK(ok, "512-key load roundtrip");

    /* ---- bulk remove first half, second half must survive ---- */
    for (uint32_t i = 0; i < N / 2; i++) {
        uint32_t key = ((i + 1u) << 8) | ((i & 0xffu) + 1u);
        mlxKeyIndexRemove(keys, slots, mask, shift, key);
    }
    ok = 1;
    for (uint32_t i = N / 2; i < N; i++) {
        uint32_t key = ((i + 1u) << 8) | ((i & 0xffu) + 1u);
        if (mlxKeyIndexFind(keys, slots, mask, shift, key) != (int)i) { ok = 0; break; }
    }
    CHECK(ok, "surviving half after bulk remove");

    if (failures == 0) { printf("MKEY_INDEX PASS\n"); return 0; }
    printf("MKEY_INDEX FAIL (%d)\n", failures);
    return 1;
}
