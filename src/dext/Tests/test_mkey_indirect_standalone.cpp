/*
 * Standalone host test for MlxP0EncodingIndirect.hpp — verifies the KLM
 * bit-encoding before this ever touches real firmware. Not part of the
 * project's own test_all.cpp on purpose (that file's build is wired into
 * `make check-host`; this one deliberately isn't, matching "write the
 * code, don't build it in yet").
 *
 * Build: clang++ -std=c++17 -I <dext>/Sources/hw this_file.cpp -o t && ./t
 */
#include "MlxP0EncodingIndirect.hpp"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

static int g_fail = 0;
static void check(bool cond, const char *msg) {
    if (!cond) { std::printf("FAIL: %s\n", msg); g_fail++; }
    else       { std::printf("ok:   %s\n", msg); }
}

int main() {
    // ---- basic single-child encode ----
    {
        struct MlxKlmEntry klms[1] = {
            { /*address*/ 0x1000, /*byteCount*/ 0x40000, /*mkey*/ mlxComposeMkey(7, 0x11) }
        };
        uint8_t in[MLX_CREATE_MKEY_FIXED_BYTES + MLX_KLM_ENTRY_BYTES] = {0};
        uint32_t size = 0;
        bool ok = mlxEncodeCreateMkeyIndirect(in, sizeof(in), klms, 1,
                                              /*startAddress*/ 0x1000, /*length*/ 0x40000,
                                              MLX_MR_ACCESS_LOCAL_WRITE | MLX_MR_ACCESS_REMOTE_WRITE,
                                              /*pd*/ 1, /*keyVariant*/ 0x51, &size);
        check(ok, "single-child indirect mkey encodes");
        check(size == mlxCreateMkeyIndirectInputSize(1), "input size = fixed + 1 KLM entry");
        const uint8_t *mkc = in + MLX_CREATE_MKEY_MKC_BIT_OFFSET / 8;
        check(mlxGetBits(mkc, 0x16, 2) == 2, "access_mode = KLM (2), not MTT (1)");
        check(mlxGetBits(mkc, 0x14, 1) == 1, "local_write bit set");
        check(mlxGetBits(mkc, 0x12, 1) == 1, "remote_write bit set");
        check(mlxGetBits(mkc, 0x15, 1) == 1, "local_read bit set (mandatory)");
        check(mlxGetBits(mkc, 0x68, 24) == 1, "pd encoded");
        check(mlxGetBits(mkc, 0x80, 64) == 0x1000, "logical start_addr encoded");
        check(mlxGetBits(mkc, 0xc0, 64) == 0x40000, "logical length encoded");
        check(mlxGetBits(mkc, 0x1a0, 32) == 1, "translations_octword_size = 1 (one KLM = one octword)");
        check(mlxGetBits(in, MLX_CREATE_MKEY_ACTUAL_XLT_BIT_OFFSET, 32) == 1, "actual_xlt matches MKC");
        check(mlxGetBits(mkc, 0x38, 8) == 0x51, "key variant encoded");

        const uint8_t *entry = in + MLX_CREATE_MKEY_PAS_BYTE_OFFSET;
        check(mlxGetBits(entry, 0x00, 32) == 0x40000, "KLM byte_count encoded");
        check(mlxGetBits(entry, 0x20, 32) == mlxComposeMkey(7, 0x11), "KLM child mkey encoded");
        check(mlxGetBits(entry, 0x40, 64) == 0x1000, "KLM address encoded");
    }

    // ---- llama.cpp's actual shape: 24 x 256KiB children under one indirect mkey ----
    {
        constexpr uint32_t N = 24;
        constexpr uint64_t CHUNK = 256 * 1024;
        struct MlxKlmEntry klms[N];
        uint64_t base = 0x7000'0000'0000ULL;
        for (uint32_t i = 0; i < N; i++) {
            klms[i].address   = base + i * CHUNK;
            klms[i].byteCount = CHUNK;
            klms[i].mkey      = mlxComposeMkey(100 + i, 0x60 + (uint8_t)i);
        }
        // one spare KLM slot of headroom so the "didn't write past the
        // last real entry" check below has buffer left to inspect safely
        uint8_t in[MLX_CREATE_MKEY_FIXED_BYTES + (N + 1) * MLX_KLM_ENTRY_BYTES] = {0};
        uint32_t size = 0;
        bool ok = mlxEncodeCreateMkeyIndirect(in, mlxCreateMkeyIndirectInputSize(N), klms, N,
                                              base, N * CHUNK,
                                              MLX_MR_ACCESS_LOCAL_WRITE | MLX_MR_ACCESS_REMOTE_WRITE,
                                              1, 0x70, &size);
        check(ok, "24-child (llama.cpp RX-ring shape) indirect mkey encodes");
        check(size == mlxCreateMkeyIndirectInputSize(N), "input size = fixed + 24 KLM entries");
        check(size < 4112, "fits in a single CREATE_MKEY command — this is the whole point");
        const uint8_t *mkc = in + MLX_CREATE_MKEY_MKC_BIT_OFFSET / 8;
        check(mlxGetBits(mkc, 0xc0, 64) == N * CHUNK, "logical length = 6 MiB (24 x 256KiB)");
        check(mlxGetBits(mkc, 0x1a0, 32) == N, "translations_octword_size = 24");

        // spot-check first, middle, and last KLM entries round-trip independently
        for (uint32_t i : {0u, 12u, N - 1}) {
            const uint8_t *entry = in + MLX_CREATE_MKEY_PAS_BYTE_OFFSET + i * MLX_KLM_ENTRY_BYTES;
            char msg[128];
            std::snprintf(msg, sizeof(msg), "KLM[%u] address round-trips", i);
            check(mlxGetBits(entry, 0x40, 64) == klms[i].address, msg);
            std::snprintf(msg, sizeof(msg), "KLM[%u] mkey round-trips", i);
            check(mlxGetBits(entry, 0x20, 32) == klms[i].mkey, msg);
        }
        check(mlxGetBits(in, MLX_CREATE_MKEY_PAS_BYTE_OFFSET * 8 + N * 128, 64) == 0,
              "does not write past the last KLM entry");
    }

    // ---- rejection paths ----
    {
        uint8_t in[4112] = {0};
        uint32_t size = 0;
        check(!mlxEncodeCreateMkeyIndirect(in, sizeof(in), nullptr, 0, 0x1000, 0x1000,
                                           MLX_MR_ACCESS_LOCAL_WRITE, 1, 1, &size),
              "zero children rejected");
        struct MlxKlmEntry zero_len[1] = { { 0x1000, /*byteCount*/ 0, /*mkey*/ 5 } };
        check(!mlxEncodeCreateMkeyIndirect(in, sizeof(in), zero_len, 1, 0x1000, 0x1000,
                                           MLX_MR_ACCESS_LOCAL_WRITE, 1, 1, &size),
              "zero-length child entry rejected");
        struct MlxKlmEntry zero_mkey[1] = { { 0x1000, 0x1000, /*mkey*/ 0 } };
        check(!mlxEncodeCreateMkeyIndirect(in, sizeof(in), zero_mkey, 1, 0x1000, 0x1000,
                                           MLX_MR_ACCESS_LOCAL_WRITE, 1, 1, &size),
              "mkey==0 child entry rejected");
        struct MlxKlmEntry one[1] = { { 0x1000, 0x1000, 5 } };
        uint8_t tiny[8] = {0};
        check(!mlxEncodeCreateMkeyIndirect(tiny, sizeof(tiny), one, 1, 0x1000, 0x1000,
                                           MLX_MR_ACCESS_LOCAL_WRITE, 1, 1, &size),
              "undersized output buffer rejected");
        check(MLX_CREATE_MKEY_MAX_KLM == 240, "max KLM children = (4112-272)/16 = 240");
    }

    std::printf(g_fail ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
