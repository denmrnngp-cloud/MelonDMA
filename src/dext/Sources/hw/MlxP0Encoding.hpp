/*
 * MlxP0Encoding.hpp - host-testable safety-critical mlx5 encoders
 *
 * Offsets are from MLNX OFED 5.9 mlx5_ifc.h and are expressed in bits.
 */
#ifndef MLX_P0_ENCODING_HPP
#define MLX_P0_ENCODING_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "MlxIfcHelpers.hpp"

enum {
    MLX_QP_MODIFY_IN_BYTES = 0x880 / 8,
    MLX_QPC_BIT_OFFSET = 0xc0,
    MLX_QPC_BYTES = 0x740 / 8,
    MLX_QPC_PRIMARY_PATH_BIT_OFFSET = 0xc0,
    MLX_ADS_BYTES = 0x160 / 8,

    MLX_CREATE_MKEY_FIXED_BYTES = 0x880 / 8,
    MLX_CREATE_MKEY_MKC_BIT_OFFSET = 0x80,
    MLX_CREATE_MKEY_ACTUAL_XLT_BIT_OFFSET = 0x300,
    MLX_CREATE_MKEY_PAS_BYTE_OFFSET = 0x880 / 8,
    MLX_MTT_PAGE_SHIFT = 12,
    MLX_MTT_PAGE_SIZE = 1u << MLX_MTT_PAGE_SHIFT,
    MLX_CREATE_MKEY_MAX_PAGES = (4112 - MLX_CREATE_MKEY_FIXED_BYTES) / 8,
};

enum MlxMrAccessFlags {
    MLX_MR_ACCESS_LOCAL_WRITE = 1u << 0,
    MLX_MR_ACCESS_REMOTE_WRITE = 1u << 1,
    MLX_MR_ACCESS_REMOTE_READ = 1u << 2,
    MLX_MR_ACCESS_REMOTE_ATOMIC = 1u << 3,
    /* ACCESS_MW_BIND is a verbs permission used for parent-MR validation;
     * it is not emitted into the mlx5 MKC access bits. */
    MLX_MR_ACCESS_MW_BIND = 1u << 4,
    MLX_MR_ACCESS_SUPPORTED = (1u << 5) - 1,
};

/* mlx5 QPC atomic_mode (0x48c) values: 0=NONE, 1=IB_COMP (standard 8-byte
 * RoCEv2 FETCH_ADD/CMP_SWAP), 3=8B (extended). Matches mlx5_ifc.h
 * MLX5_ATOMIC_MODE_*. The donor mlx5_ib falls back to IB_COMP when the card
 * lacks extended atomics (ConnectX-4 Lx). */
#define MLX_ATOMIC_MODE_IB_COMP 1u
#define MLX_ATOMIC_MODE_8B 3u

/* mlx5 QPC opt_param_mask bits (mlx5_qp.h enum mlx5_qp_optpar). */
#define MLX_QP_OPTPAR_RRE         (1u << 1)
#define MLX_QP_OPTPAR_RAE         (1u << 2)
#define MLX_QP_OPTPAR_RWE         (1u << 3)
#define MLX_QP_OPTPAR_SRA_MAX     (1u << 8)
#define MLX_QP_OPTPAR_RRA_MAX     (1u << 9)
#define MLX_QP_OPTPAR_RETRY_COUNT (1u << 12)
#define MLX_QP_OPTPAR_RNR_RETRY   (1u << 13)
#define MLX_QP_OPTPAR_ACK_TIMEOUT (1u << 14)

struct MlxRocePathFields {
    uint8_t dmac[6];
    uint8_t dgid[16];
    uint32_t sgidIndex;
    uint8_t hopLimit;
    uint8_t trafficClass;
    uint16_t udpSport;
    uint16_t pkeyIndex;
    uint8_t portNum;
    uint8_t sl;               /* RoCEv2 VLAN priority / service level (0..7) */
};

static inline uint32_t
mlxLog2PowerOfTwo(uint32_t value)
{
    uint32_t log = 0;
    while (value > 1) {
        value >>= 1;
        log++;
    }
    return log;
}

static inline bool
mlxEncodeRst2InitQpc(void *qpcBuffer, size_t qpcSize, uint32_t pkeyIndex,
                     uint32_t portNum, uint32_t *optParamMask)
{
    if (!qpcBuffer || qpcSize < MLX_QPC_BYTES || pkeyIndex > 0xffff ||
        !portNum || portNum > 0xff)
        return false;

    uint8_t *qpc = static_cast<uint8_t *>(qpcBuffer);
    mlxSetBits(qpc, 0x08, 8, 0); /* RC transport */
    mlxSetBits(qpc, 0x13, 2, 3); /* pm_state = MIGRATED (Linux default) */
    mlxSetBits(qpc, MLX_QPC_PRIMARY_PATH_BIT_OFFSET + 0x10, 16, pkeyIndex);
    mlxSetBits(qpc, MLX_QPC_PRIMARY_PATH_BIT_OFFSET + 0x128, 8, portNum);
    /* RESET->INIT has no optional parameters in mlx5 opt_mask[][][].
     * P_Key and port are transition-required QPC fields, not optpar bits. */
    if (optParamMask) *optParamMask = 0;
    return true;
}

static inline bool
mlxEncodeRocePrimaryPath(void *qpcBuffer, size_t qpcSize,
                         const struct MlxRocePathFields *path)
{
    if (!qpcBuffer || !path || qpcSize < MLX_QPC_BYTES ||
        path->sgidIndex > 0xff || !path->portNum || path->sl > 7)
        return false;

    uint8_t *qpc = static_cast<uint8_t *>(qpcBuffer);
    const uint32_t ads = MLX_QPC_PRIMARY_PATH_BIT_OFFSET;
    mlxSetBits(qpc, ads + 0x10, 16, path->pkeyIndex);
    mlxSetBits(qpc, ads + 0x48, 8, path->sgidIndex);
    mlxSetBits(qpc, ads + 0x58, 8,
               path->hopLimit ? path->hopLimit : 64);
    mlxSetBits(qpc, ads + 0x64, 8, path->trafficClass);
    /* RoCEv2 uses the DSCP overlay in addition to the full tclass field.
     * ECN stays represented by tclass, matching Linux mlx5_set_path(). */
    mlxSetBits(qpc, ads + 0x10a, 6, path->trafficClass >> 2);
    mlxSetBits(qpc, ads + 0x110, 16, path->udpSport);
    mlxSetBits(qpc, ads + 0x121, 3, path->sl);
    mlxSetBits(qpc, ads + 0x128, 8, path->portNum);

    memcpy(qpc + (ads + 0x80) / 8, path->dgid, sizeof(path->dgid));
    memcpy(qpc + (ads + 0x130) / 8, path->dmac, sizeof(path->dmac));
    return true;
}

static inline bool
mlxEncodeInit2RtrQpc(void *qpcBuffer, size_t qpcSize,
                     const struct MlxRocePathFields *path, uint32_t destQpn,
                     uint32_t pathMtu, uint32_t rqPsn,
                     uint32_t minRnrTimer, uint32_t maxDestRdAtomic,
                     uint32_t atomicMode,
                     uint32_t logMaxMsg,
                     uint32_t *optParamMask)
{
    if (!qpcBuffer || qpcSize < MLX_QPC_BYTES || !path ||
        destQpn > 0xffffff || !pathMtu || pathMtu > 5 || rqPsn > 0xffffff ||
        minRnrTimer > 0x1f || maxDestRdAtomic > 128 || atomicMode > 8 ||
        !logMaxMsg || logMaxMsg > 31 ||
        (maxDestRdAtomic & (maxDestRdAtomic - 1)))
        return false;

    uint8_t *qpc = static_cast<uint8_t *>(qpcBuffer);
    mlxSetBits(qpc, 0x08, 8, 0); /* RC transport */
    mlxSetBits(qpc, 0x13, 2, 3); /* pm_state = MIGRATED (Linux default) */
    mlxSetBits(qpc, 0x40, 3, pathMtu);
    mlxSetBits(qpc, 0x43, 5, logMaxMsg);
    mlxSetBits(qpc, 0xa8, 24, destQpn);
    mlxSetBits(qpc, 0x4a3, 5, minRnrTimer);
    mlxSetBits(qpc, 0x4a8, 24, rqPsn);
    if (!mlxEncodeRocePrimaryPath(qpc, qpcSize, path))
        return false;

    uint32_t mask = 0;
    if (maxDestRdAtomic) {
        mlxSetBits(qpc, 0x488, 3, mlxLog2PowerOfTwo(maxDestRdAtomic));
        mlxSetBits(qpc, 0x490, 1, 1); /* rre: remote read */
        mlxSetBits(qpc, 0x491, 1, 1); /* rwe: remote write */
        mask = MLX_QP_OPTPAR_RRE | MLX_QP_OPTPAR_RWE;
        if (atomicMode) {
            /* rae=1 requires a non-zero atomic_mode (QPC 0x48c): the
             * firmware applies both under the RAE opt_param_mask bit. */
            mlxSetBits(qpc, 0x48c, 4, atomicMode);
            mlxSetBits(qpc, 0x492, 1, 1); /* rae: remote atomic (requester) */
            mask |= MLX_QP_OPTPAR_RAE;
        }
        /* Linux filters ib_mask_to_mlx5_opt(MAX_DEST_RD_ATOMIC) through
         * opt_mask[INIT][RTR][RC].  RRA_MAX (bit 9) is deliberately removed
         * for this transition even though log_rra_max remains in the QPC. */
    }
    /* MTU, remote QPN, RQ PSN, address path and port are required fields. */
    if (optParamMask)
        *optParamMask = mask;
    return true;
}

static inline bool
mlxEncodeRtr2RtsQpc(void *qpcBuffer, size_t qpcSize, uint32_t sqPsn,
                    uint32_t maxRdAtomic,
                    uint32_t ackTimeout, uint32_t retryCount,
                    uint32_t rnrRetry,
                    uint32_t *optParamMask)
{
    if (!qpcBuffer || qpcSize < MLX_QPC_BYTES || sqPsn > 0xffffff ||
        maxRdAtomic > 128 ||
        (maxRdAtomic & (maxRdAtomic - 1)) || ackTimeout > 0x1f ||
        retryCount > 7 || rnrRetry > 7)
        return false;

    uint8_t *qpc = static_cast<uint8_t *>(qpcBuffer);
    mlxSetBits(qpc, 0x08, 8, 0); /* RC transport */
    mlxSetBits(qpc, 0x13, 2, 3); /* pm_state = MIGRATED (Linux default) */
    mlxSetBits(qpc, 0x3c8, 24, sqPsn);
    mlxSetBits(qpc, MLX_QPC_PRIMARY_PATH_BIT_OFFSET + 0x40, 5,
               ackTimeout);
    mlxSetBits(qpc, 0x38d, 3, retryCount);
    mlxSetBits(qpc, 0x390, 3, rnrRetry);
    if (maxRdAtomic)
        mlxSetBits(qpc, 0x388, 3, mlxLog2PowerOfTwo(maxRdAtomic));
    /* RTR->RTS is a state transition: the firmware applies the encoded
     * fields unconditionally and REJECTS a non-zero opt_param_mask (this
     * DEXT's CX4-Lx fw returns syndrome 0x7ec2c on opt bits here — verified
     * live). opt_param_mask only gates same-state RTS->RTS modifies, which
     * this driver does not use. */
    if (optParamMask) *optParamMask = 0;
    return true;
}

static inline bool
mlxAppendMttPages(uint64_t dmaAddress, uint64_t length, uint64_t *pages,
                  uint32_t capacity, uint32_t *pageCount)
{
    if (!length || !pages || !pageCount || length > UINT64_MAX - dmaAddress)
        return false;

    while (length) {
        uint64_t page = dmaAddress & ~(uint64_t)(MLX_MTT_PAGE_SIZE - 1);
        if (!*pageCount || pages[*pageCount - 1] != page) {
            if (*pageCount >= capacity)
                return false;
            pages[(*pageCount)++] = page;
        }

        uint64_t step = MLX_MTT_PAGE_SIZE -
                        (dmaAddress & (MLX_MTT_PAGE_SIZE - 1));
        if (step > length)
            step = length;
        dmaAddress += step;
        length -= step;
    }
    return true;
}

static inline uint64_t
mlxMttPageCount(uint64_t startAddress, uint64_t length)
{
    uint64_t pageOffset = startAddress & (MLX_MTT_PAGE_SIZE - 1);
    if (!length || length > UINT64_MAX - startAddress ||
        length > UINT64_MAX - pageOffset)
        return 0;
    uint64_t span = pageOffset + length;
    return (span >> MLX_MTT_PAGE_SHIFT) +
           !!(span & (MLX_MTT_PAGE_SIZE - 1));
}

static inline uint32_t
mlxMttOctwordCount(uint32_t pageCount)
{
    return (pageCount + 1) / 2;
}

static inline uint32_t
mlxCreateMkeyInputSize(uint32_t pageCount)
{
    return MLX_CREATE_MKEY_FIXED_BYTES + mlxMttOctwordCount(pageCount) * 16;
}

static inline bool
mlxEncodeCreateMkey(void *input, size_t capacity, const uint64_t *pages,
                    uint32_t pageCount, uint64_t startAddress, uint64_t length,
                    uint32_t accessFlags, uint32_t pd, uint8_t keyVariant,
                    uint32_t *inputSize)
{
    if (pageCount > MLX_CREATE_MKEY_MAX_PAGES)
        return false;
    uint64_t expectedPages = mlxMttPageCount(startAddress, length);
    uint32_t size = mlxCreateMkeyInputSize(pageCount);
    if (!input || !pages || !pageCount || !length || !keyVariant ||
        (accessFlags & ~MLX_MR_ACCESS_SUPPORTED) || pd > 0xffffff ||
        expectedPages != pageCount || size > capacity)
        return false;

    uint8_t *in = static_cast<uint8_t *>(input);
    uint8_t *mkc = in + MLX_CREATE_MKEY_MKC_BIT_OFFSET / 8;
    uint32_t xltOctwords = mlxMttOctwordCount(pageCount);

    for (uint32_t i = 0; i < pageCount; i++) {
        if (pages[i] & (MLX_MTT_PAGE_SIZE - 1))
            return false;
    }

    mlxSetBits(mkc, 0x11, 1,
               !!(accessFlags & MLX_MR_ACCESS_REMOTE_ATOMIC));
    mlxSetBits(mkc, 0x12, 1,
               !!(accessFlags & MLX_MR_ACCESS_REMOTE_WRITE));
    mlxSetBits(mkc, 0x13, 1,
               !!(accessFlags & MLX_MR_ACCESS_REMOTE_READ));
    mlxSetBits(mkc, 0x14, 1,
               !!(accessFlags & MLX_MR_ACCESS_LOCAL_WRITE));
    mlxSetBits(mkc, 0x15, 1, 1);             /* local read */
    mlxSetBits(mkc, 0x16, 2, 1);             /* MTT access mode */
    /* Linux mlx5 reg_create() enables UMR updates even for an initially
     * populated MTT mkey. This does not make CREATE_MKEY itself a UMR. */
    mlxSetBits(mkc, 0x10, 1, 1);             /* umr_en */
    mlxSetBits(mkc, 0x20, 24, 0xffffff);      /* unrestricted QPN */
    mlxSetBits(mkc, 0x38, 8, keyVariant);
    /* length64 is the special 2^64-byte-region mode; it is not a selector
     * for the width of the ordinary 64-bit len field. When set, len is
     * reserved. Normal finite MRs must leave it clear. */
    mlxSetBits(mkc, 0x60, 1, 0);             /* finite length from len */
    mlxSetBits(mkc, 0x68, 24, pd);
    mlxSetBits(mkc, 0x80, 64, startAddress);
    mlxSetBits(mkc, 0xc0, 64, length);
    mlxSetBits(mkc, 0x1a0, 32, xltOctwords);
    mlxSetBits(mkc, 0x1da, 6, MLX_MTT_PAGE_SHIFT);
    mlxSetBits(in, MLX_CREATE_MKEY_ACTUAL_XLT_BIT_OFFSET, 32,
               xltOctwords);

    for (uint32_t i = 0; i < pageCount; i++)
        mlxSetBits(in, 0x880 + i * 64, 64, pages[i]);
    if (inputSize)
        *inputSize = size;
    return true;
}

static inline uint32_t
mlxComposeMkey(uint32_t index, uint8_t variant)
{
    return (index << 8) | variant;
}

static inline uint32_t
mlxMkeyIndex(uint32_t key)
{
    return key >> 8;
}

#endif /* MLX_P0_ENCODING_HPP */
