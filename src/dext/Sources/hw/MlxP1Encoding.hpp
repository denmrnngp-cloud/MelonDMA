#ifndef MLX_P1_ENCODING_HPP
#define MLX_P1_ENCODING_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "MlxIfcHelpers.hpp"

enum {
    MLX_P1_CMD_HEADER_BYTES = 16,
    MLX_P1_HCA_CAP_BYTES = 4096,
    MLX_P1_QUERY_HCA_CAP_IN_BYTES = 16,
    MLX_P1_QUERY_HCA_CAP_OUT_BYTES = 4112,
    MLX_P1_SET_HCA_CAP_IN_BYTES = 4112,
    MLX_P1_INIT_HCA_IN_BYTES = 32,
    MLX_P1_MAX_MANAGE_PAGES = 512,
};

enum MlxP1CapType {
    MLX_P1_CAP_GENERAL = 0,
    MLX_P1_CAP_ETHERNET_OFFLOADS = 1,
    MLX_P1_CAP_ATOMIC = 3,
    MLX_P1_CAP_ROCE = 4,
    MLX_P1_CAP_FLOW_TABLE = 7,
};

enum MlxP1CapMode {
    MLX_P1_CAP_MAX = 0,
    MLX_P1_CAP_CURRENT = 1,
};

enum MlxP1PageOpMod {
    MLX_P1_PAGES_ALLOCATION_FAIL = 0,
    MLX_P1_PAGES_GIVE = 1,
    MLX_P1_PAGES_TAKE = 2,
};

struct MlxP1GeneralCaps {
    uint8_t logMaxQp;
    uint8_t logMaxCq;
    uint8_t logMaxMkey;
    uint8_t logMaxMsg;
    uint8_t portType;
    uint8_t numPorts;
    uint8_t logBfRegSize;
    uint8_t gidTableEncoding;
    uint8_t pkeyTableEncoding;
    bool roce;
    bool atomic;
    bool roceRwSupported;
    bool uar4k;
    bool cacheLine128;
    bool bf;
    uint8_t logMaxSrqSz;
    uint8_t logPgSz;      /* adapter min page size (MKC log_page_size lower bound) */
    bool nicFlowTable;
    bool ethNetOffloads;
    uint8_t numVhcaPorts;
    bool swOwnerId;
};

struct MlxP1RoceCaps {
    bool sourceUdpPortWritable;
    uint8_t versions;
    uint16_t destinationUdpPort;
    uint16_t minimumSourceUdpPort;
    uint16_t addressTableSize;
};

/* mlx5_ifc_atomic_caps_bits (QUERY_HCA_CAP op_mod=ATOMIC, 0x3):
 *   atomic_operations @0x90 (16b), atomic_size_qp @0xb0 (16b). */
struct MlxP1AtomicCaps {
    uint16_t operations;   /* bit0=CMP_SWAP bit1=FETCH_ADD bit2/3=extended */
    uint16_t sizeQp;       /* max atomic size for QP, bit N = 2^N bytes */
};

/* Atomic op bits (mlx5_ifc.h MLX5_ATOMIC_OPS_*). */
enum {
    MLX_P1_ATOMIC_OPS_CMP_SWAP          = 1u << 0,
    MLX_P1_ATOMIC_OPS_FETCH_ADD         = 1u << 1,
    MLX_P1_ATOMIC_OPS_EXTENDED_CMP_SWAP = 1u << 2,
    MLX_P1_ATOMIC_OPS_EXTENDED_FETCH_ADD = 1u << 3,
};

static inline bool mlxP1ParseAtomicCaps(const uint8_t *cap, size_t length,
                                        MlxP1AtomicCaps *out)
{
    if (!cap || !out || length < MLX_P1_HCA_CAP_BYTES)
        return false;
    memset(out, 0, sizeof(*out));
    out->operations = static_cast<uint16_t>(mlxGetBits(cap, 0x90, 16));
    out->sizeQp = static_cast<uint16_t>(mlxGetBits(cap, 0xb0, 16));
    return true;
}

/* Return the QPC atomic_mode for the operations emitted by this provider.
 * Standard 8-byte RC atomics use IB_COMP(1).  Some ConnectX firmware also
 * advertises extended sizes; that size mask must not override the standard
 * mode, or an 8-byte WQE is rejected with syndrome 0x6802. */
static inline uint32_t mlxP1AtomicMode(const MlxP1AtomicCaps *caps)
{
    if (!caps) return 0;
    uint16_t ops = caps->operations;
    if ((ops & MLX_P1_ATOMIC_OPS_CMP_SWAP) &&
        (ops & MLX_P1_ATOMIC_OPS_FETCH_ADD))
        return 1;  /* MLX_ATOMIC_MODE_IB_COMP */
    if ((ops & MLX_P1_ATOMIC_OPS_EXTENDED_CMP_SWAP) ||
        (ops & MLX_P1_ATOMIC_OPS_EXTENDED_FETCH_ADD)) {
        uint32_t mask = caps->sizeQp & 0x1ff;
        if (mask & (1u << 3)) return 3;  /* MLX_ATOMIC_MODE_8B */
    }
    return 0;
}

struct MlxP1FlowCaps {
    bool nicRx;
    bool nicTx;
};

struct MlxP1EthernetCaps {
    bool checksum;
    bool vlan;
};

struct MlxP1OutboxStatus {
    uint8_t status;
    uint32_t syndrome;
};

static inline bool mlxP1ParseOutbox(const uint8_t *out, size_t length,
                                    MlxP1OutboxStatus *result)
{
    if (!out || !result || length < 8)
        return false;
    result->status = static_cast<uint8_t>(mlxGetBits(out, 0x00, 8));
    result->syndrome = static_cast<uint32_t>(mlxGetBits(out, 0x20, 32));
    return true;
}

static inline uint16_t mlxP1HcaCapOpMod(uint16_t type, uint16_t mode)
{
    return static_cast<uint16_t>((type << 1) | (mode & 1));
}

static inline bool mlxP1EncodeQueryHcaCap(uint8_t *in, size_t length,
                                          uint16_t type, uint16_t mode)
{
    if (!in || length < MLX_P1_QUERY_HCA_CAP_IN_BYTES)
        return false;
    memset(in, 0, MLX_P1_QUERY_HCA_CAP_IN_BYTES);
    mlxSetBits(in, 0x00, 16, 0x100);
    mlxSetBits(in, 0x30, 16, mlxP1HcaCapOpMod(type, mode));
    return true;
}

static inline bool mlxP1ParseGeneralCaps(const uint8_t *cap, size_t length,
                                         MlxP1GeneralCaps *out)
{
    if (!cap || !out || length < MLX_P1_HCA_CAP_BYTES)
        return false;
    memset(out, 0, sizeof(*out));
    out->logMaxQp = static_cast<uint8_t>(mlxGetBits(cap, 0x9b, 5));
    out->logMaxCq = static_cast<uint8_t>(mlxGetBits(cap, 0xdb, 5));
    out->logMaxMkey = static_cast<uint8_t>(mlxGetBits(cap, 0xea, 6));
    out->logMaxMsg = static_cast<uint8_t>(mlxGetBits(cap, 0x1c3, 5));
    out->gidTableEncoding = static_cast<uint8_t>(mlxGetBits(cap, 0x170, 16));
    out->pkeyTableEncoding = static_cast<uint8_t>(mlxGetBits(cap, 0x190, 16));
    out->nicFlowTable = mlxGetBits(cap, 0x1a6, 1) != 0;
    out->portType = static_cast<uint8_t>(mlxGetBits(cap, 0x1b6, 2));
    out->numPorts = static_cast<uint8_t>(mlxGetBits(cap, 0x1b8, 8));
    out->ethNetOffloads = mlxGetBits(cap, 0x21b, 1) != 0;
    out->roce = mlxGetBits(cap, 0x21c, 1) != 0;
    out->atomic = mlxGetBits(cap, 0x21d, 1) != 0;
    out->uar4k = mlxGetBits(cap, 0x240, 1) != 0;
    out->cacheLine128 = mlxGetBits(cap, 0x164, 1) != 0;
    out->bf = mlxGetBits(cap, 0x260, 1) != 0;
    out->logMaxSrqSz = static_cast<uint8_t>(mlxGetBits(cap, 0x80, 8));
    out->logPgSz = static_cast<uint8_t>(mlxGetBits(cap, 0x258, 8));
    out->logBfRegSize = static_cast<uint8_t>(mlxGetBits(cap, 0x26b, 5));
    out->roceRwSupported = mlxGetBits(cap, 0x3a1, 1) != 0;
    out->numVhcaPorts = static_cast<uint8_t>(mlxGetBits(cap, 0x610, 8));
    out->swOwnerId = mlxGetBits(cap, 0x61e, 1) != 0;
    return true;
}

static inline bool mlxP1ParseRoceCaps(const uint8_t *cap, size_t length,
                                      MlxP1RoceCaps *out)
{
    if (!cap || !out || length < MLX_P1_HCA_CAP_BYTES)
        return false;
    memset(out, 0, sizeof(*out));
    out->sourceUdpPortWritable = mlxGetBits(cap, 0x04, 1) != 0;
    out->versions = static_cast<uint8_t>(mlxGetBits(cap, 0x98, 8));
    out->destinationUdpPort = static_cast<uint16_t>(mlxGetBits(cap, 0xb0, 16));
    out->minimumSourceUdpPort = static_cast<uint16_t>(mlxGetBits(cap, 0xd0, 16));
    out->addressTableSize = static_cast<uint16_t>(mlxGetBits(cap, 0xf0, 16));
    return true;
}

static inline bool mlxP1ParseFlowCaps(const uint8_t *cap, size_t length,
                                      MlxP1FlowCaps *out)
{
    if (!cap || !out || length < MLX_P1_HCA_CAP_BYTES)
        return false;
    out->nicRx = mlxGetBits(cap, 0x200, 1) != 0;
    out->nicTx = mlxGetBits(cap, 0x800, 1) != 0;
    return true;
}

static inline bool mlxP1ParseEthernetCaps(const uint8_t *cap, size_t length,
                                          MlxP1EthernetCaps *out)
{
    if (!cap || !out || length < MLX_P1_HCA_CAP_BYTES)
        return false;
    out->checksum = mlxGetBits(cap, 0x00, 1) != 0;
    out->vlan = mlxGetBits(cap, 0x01, 1) != 0;
    return true;
}

static inline uint8_t mlxP1RoceVersionsForAbi(uint8_t firmwareVersions)
{
    return static_cast<uint8_t>((firmwareVersions & 0x1 ? 0x1 : 0) |
                                (firmwareVersions & 0x4 ? 0x2 : 0));
}

static inline uint32_t mlxP1GidTableSize(uint16_t encoding)
{
    return encoding <= 4 ? (8u << encoding) : 0;
}

static inline uint32_t mlxP1PkeyTableSize(uint16_t encoding)
{
    return encoding <= 5 ? (128u << encoding) : 0;
}

static inline uint32_t mlxP1LogResourceSize(uint8_t logSize)
{
    return logSize < 32 ? (1u << logSize) : 0;
}

static inline bool mlxP1EncodeQueryPages(uint8_t *in, size_t length,
                                         uint16_t opMod, bool embedded)
{
    if (!in || length < 16 || (opMod != 1 && opMod != 2))
        return false;
    memset(in, 0, 16);
    mlxSetBits(in, 0x00, 16, 0x107);
    mlxSetBits(in, 0x30, 16, opMod);
    mlxSetBits(in, 0x40, 1, embedded ? 1 : 0);
    return true;
}

static inline uint32_t mlxP1ManagePagesSize(uint32_t count)
{
    return count <= MLX_P1_MAX_MANAGE_PAGES ? 16 + count * 8 : 0;
}

static inline bool mlxP1EncodeManagePages(uint8_t *in, size_t length,
                                          uint16_t opMod, uint16_t functionId,
                                          bool embedded, const uint64_t *pages,
                                          uint32_t count)
{
    uint32_t needed = opMod == MLX_P1_PAGES_GIVE ?
        mlxP1ManagePagesSize(count) : 16;
    if (!in || !needed || length < needed || count > MLX_P1_MAX_MANAGE_PAGES ||
        opMod > MLX_P1_PAGES_TAKE ||
        (opMod == MLX_P1_PAGES_GIVE && (!pages || !count)))
        return false;
    memset(in, 0, needed);
    mlxSetBits(in, 0x00, 16, 0x108);
    mlxSetBits(in, 0x30, 16, opMod);
    mlxSetBits(in, 0x40, 1, embedded ? 1 : 0);
    mlxSetBits(in, 0x50, 16, functionId);
    mlxSetBits(in, 0x60, 32, count);
    if (opMod == MLX_P1_PAGES_GIVE) {
        for (uint32_t i = 0; i < count; i++)
            mlxSetBits(in, 0x80 + i * 64, 64, pages[i]);
    }
    return true;
}

/* Decoder for the MANAGE_PAGES(TAKE) response. mlx5_ifc_manage_pages_out_bits:
 *   output_num_entries@0x40 (32b), pas[]@0x80 (64b each).
 * Returns false on corrupted/exceeded count. */
static inline bool mlxP1DecodeManagePagesTake(const uint8_t *out, size_t length,
                                              uint32_t requested,
                                              uint32_t *outCount,
                                              uint64_t *pas, uint32_t maxPas)
{
    if (!out || length < 16 || requested > MLX_P1_MAX_MANAGE_PAGES)
        return false;
    uint32_t n = (uint32_t)mlxGetBits(out, 0x40, 32);
    if (n > requested || n > maxPas || length < 16 + n * 8)
        return false;
    if (outCount) *outCount = n;
    for (uint32_t i = 0; i < n; i++)
        pas[i] = mlxGetBits(out, 0x80 + i * 64, 64);
    return true;
}

static inline bool mlxP1SetEvent(uint64_t mask[4], uint32_t eventType)
{
    if (!mask || eventType >= 256)
        return false;
    mask[eventType / 64] |= 1ULL << (eventType % 64);
    return true;
}

#endif
