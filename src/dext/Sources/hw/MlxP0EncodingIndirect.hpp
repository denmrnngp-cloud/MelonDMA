/*
 * MlxP0EncodingIndirect.hpp - CREATE_MKEY encoder for indirect (KLM) mkeys.
 *
 * NOT WIRED INTO THE BUILD YET. No Makefile target references this file;
 * `make dext` / `make check-dext` / `./scripts/mlx_dev.sh release` do not
 * see it. Written for review ahead of the P5 "melon per-call reg/dereg
 * overhead" work (notes/43) and the llama.cpp RX-ring MR-size gap
 * (notes/40, notes/44) — see the integration notes at the bottom of this
 * file for exactly what a real wire-in would touch.
 *
 * Companion to MlxP0Encoding.hpp's mlxEncodeCreateMkey(), which builds a
 * *direct* MTT mkey (one physical page list, capped at
 * MLX_CREATE_MKEY_MAX_PAGES = 480 pages / ~1.875 MiB by this driver's
 * single-mailbox-sized CREATE_MKEY command). This file builds the other
 * mlx5 mkey shape instead: an *indirect* KLM mkey, whose translation list
 * is not physical pages but {byte_count, mkey, address} triples pointing
 * at other, already-registered mkeys. Composing e.g. 24 direct 256 KiB
 * mkeys under one indirect mkey presents them to the application as a
 * single rkey/lkey over the whole 6 MiB span — exactly llama.cpp's
 * ggml-rpc RDMA transport's RX-ring registration shape (transport.cpp,
 * RDMA_RX_DEPTH=24 x RDMA_CHUNK=256KiB) — without raising this driver's
 * command-mailbox size at all. The MTT cap is a property of one physical
 * page list; it says nothing about how many *mkeys* an indirect mkey can
 * reference, so this sidesteps notes/40's gap #1 instead of removing it.
 *
 * Offsets follow the same MLNX OFED 5.9 mlx5_ifc.h this driver already
 * cites in MlxP0Encoding.hpp. access_mode=2 (KLM) is
 * MLX5_MKC_ACCESS_MODE_KLMS; each KLM entry is a 16-byte
 * {byte_count:32, mkey:32, address:64} triple (mlx5_ifc_klm_bits) — twice
 * the 8-byte stride of a direct MTT page entry, so unlike
 * mlxMttOctwordCount() (two MTT pages per 16-byte octword) each KLM entry
 * *is* one whole octword: translation_octword_size == klmCount directly.
 */
#ifndef MLX_P0_ENCODING_INDIRECT_HPP
#define MLX_P0_ENCODING_INDIRECT_HPP

#include "MlxP0Encoding.hpp"

enum {
    MLX_KLM_ENTRY_BYTES = 16,
    /* Same 4112-byte single-mailbox CREATE_MKEY budget as the direct path
     * (MLX_CMD_MAX_SIZE in MlxCmd.hpp), just divided by the wider 16-byte
     * KLM stride instead of the 8-byte MTT one: (4112-272)/16 = 240. */
    MLX_CREATE_MKEY_MAX_KLM =
        (4112 - MLX_CREATE_MKEY_FIXED_BYTES) / MLX_KLM_ENTRY_BYTES,
};

/* One indirect-mkey translation entry: a byte range within a child mkey
 * that was already returned by a normal (direct) MlxMR::RegMR() call.
 * `mkey` is that child's *composed* key (mkeyIndex<<8 | variant) — the
 * same 32-bit value already used everywhere else in this driver as an
 * lkey/rkey (MlxP0Encoding.hpp's mlxComposeMkey()), not the bare index. */
struct MlxKlmEntry {
    uint64_t address;
    uint64_t byteCount;
    uint32_t mkey;
};

static inline uint32_t
mlxCreateMkeyIndirectInputSize(uint32_t klmCount)
{
    return MLX_CREATE_MKEY_FIXED_BYTES + klmCount * MLX_KLM_ENTRY_BYTES;
}

/* Mirrors mlxEncodeCreateMkey()'s parameter shape and validation style.
 * `startAddress`/`length` here are the *logical* span the indirect mkey
 * presents to the application (normally the union of the children, e.g.
 * the RX ring's base address and its full 6 MiB span) — independent of
 * each child's own address, exactly as MKC start_addr is independent of
 * the PAS translation in the direct path (see test_create_mkey's "MKC
 * preserves client VA independently of PAS"). */
static inline bool
mlxEncodeCreateMkeyWindow(void *input, size_t capacity, uint32_t pd,
                          uint8_t keyVariant, uint32_t *inputSize)
{
    const uint32_t xltOctwords = 4; /* rdma-core alloc_mw: roundup(1, 4) */
    const uint32_t size = MLX_CREATE_MKEY_FIXED_BYTES + xltOctwords * 16;
    if (!input || capacity < size || !pd || pd > 0xffffff || !keyVariant)
        return false;
    uint8_t *mkc = (uint8_t *)input + MLX_CREATE_MKEY_MKC_BIT_OFFSET / 8;
    memset(input, 0, size);
    mlxSetBits(mkc, 0x01, 1, 1);              /* free until first BIND_MW */
    mlxSetBits(mkc, 0x10, 1, 1);              /* UMR enabled */
    mlxSetBits(mkc, 0x67, 1, 1);              /* en_rinval: type-2 MW */
    mlxSetBits(mkc, 0x15, 1, 1);              /* local read */
    mlxSetBits(mkc, 0x16, 2, 2);              /* access_mode_1_0 = KLMS */
    /* mlx5 alloc_mw() reserves a four-octoword KLM context even before the
     * first bind. Zero translations is rejected by ConnectX firmware. */
    mlxSetBits(mkc, 0x1a0, 32, 4);
    mlxSetBits(input, MLX_CREATE_MKEY_ACTUAL_XLT_BIT_OFFSET, 32, 4);
    mlxSetBits(mkc, 0x20, 24, 0xffffff);     /* unrestricted until bind */
    mlxSetBits(mkc, 0x38, 8, keyVariant);
    mlxSetBits(mkc, 0x68, 24, pd);
    if (inputSize) *inputSize = size;
    return true;
}

static inline bool
mlxEncodeCreateMkeyIndirect(void *input, size_t capacity,
                            const struct MlxKlmEntry *klms, uint32_t klmCount,
                            uint64_t startAddress, uint64_t length,
                            uint32_t accessFlags, uint32_t pd,
                            uint8_t keyVariant, uint32_t *inputSize)
{
    if (!klmCount || klmCount > MLX_CREATE_MKEY_MAX_KLM)
        return false;
    uint32_t size = mlxCreateMkeyIndirectInputSize(klmCount);
    if (!input || !klms || !length || !keyVariant ||
        (accessFlags & ~MLX_MR_ACCESS_SUPPORTED) || pd > 0xffffff ||
        size > capacity)
        return false;
    for (uint32_t i = 0; i < klmCount; i++)
        if (!klms[i].byteCount || !klms[i].mkey)
            return false;

    uint8_t *in = static_cast<uint8_t *>(input);
    uint8_t *mkc = in + MLX_CREATE_MKEY_MKC_BIT_OFFSET / 8;

    mlxSetBits(mkc, 0x11, 1, !!(accessFlags & MLX_MR_ACCESS_REMOTE_ATOMIC));
    mlxSetBits(mkc, 0x12, 1, !!(accessFlags & MLX_MR_ACCESS_REMOTE_WRITE));
    mlxSetBits(mkc, 0x13, 1, !!(accessFlags & MLX_MR_ACCESS_REMOTE_READ));
    mlxSetBits(mkc, 0x14, 1, !!(accessFlags & MLX_MR_ACCESS_LOCAL_WRITE));
    mlxSetBits(mkc, 0x15, 1, 1);              /* local read */
    mlxSetBits(mkc, 0x16, 2, 2);              /* access_mode = KLM (indirect) */
    mlxSetBits(mkc, 0x10, 1, 1);              /* umr_en, same rationale as the direct path */
    mlxSetBits(mkc, 0x20, 24, 0xffffff);      /* unrestricted QPN */
    mlxSetBits(mkc, 0x38, 8, keyVariant);
    mlxSetBits(mkc, 0x60, 1, 0);              /* finite length from len */
    mlxSetBits(mkc, 0x68, 24, pd);
    mlxSetBits(mkc, 0x80, 64, startAddress);
    mlxSetBits(mkc, 0xc0, 64, length);
    mlxSetBits(mkc, 0x1a0, 32, klmCount);     /* one octword per KLM entry, not two-per like MTT */
    /* 0x1da (log_page_size) is an MTT-only field; left zero here — firmware
     * ignores it when access_mode selects KLM translation. */
    mlxSetBits(in, MLX_CREATE_MKEY_ACTUAL_XLT_BIT_OFFSET, 32, klmCount);

    for (uint32_t i = 0; i < klmCount; i++) {
        uint8_t *entry = in + MLX_CREATE_MKEY_PAS_BYTE_OFFSET +
                          i * MLX_KLM_ENTRY_BYTES;
        mlxSetBits(entry, 0x00, 32, (uint32_t)klms[i].byteCount);
        mlxSetBits(entry, 0x20, 32, klms[i].mkey);
        mlxSetBits(entry, 0x40, 64, klms[i].address);
    }
    if (inputSize) *inputSize = size;
    return true;
}

#endif /* MLX_P0_ENCODING_INDIRECT_HPP */

/*
 * ---- Integration notes (not yet applied) ----
 *
 * 1. MlxMR.hpp/.cpp: add
 *      kern_return_t RegMRIndirect(const uint32_t *childHandles,
 *                                   uint32_t childCount, uint32_t pd,
 *                                   uint32_t accessFlags,
 *                                   struct mlx_reg_mr_resp *resp);
 *    Body: IOLockLock, Lookup() each childHandles[i] via the *existing*
 *    s->table to read its startAddr/length/lkey, build MlxKlmEntry[] from
 *    those (byteCount=child length, mkey=child composedLkey,
 *    address=child startAddr), call mlxEncodeCreateMkeyIndirect(), then
 *    Exec(MLX_CMD_OP_CREATE_MKEY, ...) exactly like RegMR() does today,
 *    and store the result in the *same* s->table (it is already sized
 *    MLX_MR_TABLE_CAP=32 — 24 direct RX-ring children + 1 indirect mkey
 *    fits with room to spare, no table resize needed for llama.cpp's
 *    actual shape). DeregMR() already works unchanged for the indirect
 *    mkey's own handle; document that children must outlive it (dereg the
 *    indirect mkey first).
 *
 * 2. MlxUCIO.h: new selector (e.g. kMlxUCMethodRegMRIndirect = 0x1042,
 *    the next free slot after kMlxUCMethodDeregMR=0x1041) and a
 *    mlx_reg_mr_indirect_req { uint32_t pd; uint32_t accessFlags;
 *    uint32_t childCount; uint32_t childHandles[24]; } — bounded array,
 *    same MLX_UC_METHOD-style fixed-size struct this file's IOConnect
 *    surface already uses elsewhere, not a variable-length one.
 *
 * 3. MlxUserClient.cpp: one dispatch case mirroring kMlxUCMethodRegMR's
 *    existing block (ownership tracking via AddOwned(ivars->fOwnedMr, ...)
 *    is unchanged — an indirect mkey's handle goes through the exact same
 *    fOwnedMr bookkeeping as any other MR).
 *
 * 4. librdma_shim.h/.c + libibverbs_compat: a new rdma_reg_mr_indirect()
 *    (or an ibv_alloc_mw-shaped wrapper) so a userspace consumer — melon
 *    for its own per-call reg/dereg overhead, or a future llama.cpp
 *    transport.cpp path — can actually reach this from outside the DEXT.
 *
 * None of this is required for #1 to be safe to land on its own — the
 * kernel-side capability is independently useful before any specific
 * consumer wires into it.
 */
