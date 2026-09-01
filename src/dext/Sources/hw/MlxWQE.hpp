/*
 * MlxWQE.hpp — WQE/CQE/AV hardware structures (generic Mellanox mlx5 family)
 *
 * Ported from: kernel_src/mlnx-ofed-kernel-5.9/include/linux/mlx5/qp.h
 *              kernel_src/mlnx-ofed-kernel-5.9/include/linux/mlx5/device.h
 * The layouts are hard-imposed by hardware, consistent across the whole family,
 * and must not be changed. Use static_assert to validate sizes.
 */
#ifndef MLX_WQE_HPP
#define MLX_WQE_HPP

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#if defined(__cplusplus)
/* librdma_shim.c includes this file as plain C11 — MlxP0EncodingIndirect.hpp
 * (transitively MlxIfcHelpers.hpp/MlxP0Encoding.hpp) uses static_cast, so it
 * must stay behind this guard. Only mlxEncodeUmrKlmWqe() and its structs
 * below need it; nothing else in this file does. */
#include "MlxP0EncodingIndirect.hpp"
#endif

/* 32/64-bit network byte-order macros (arm64 little-endian, for reading hardware fields) */
#define MLX_BE32(val)  __builtin_bswap32((uint32_t)(val))
#define MLX_BE16(val)  __builtin_bswap16((uint16_t)(val))
#define MLX_BE64(val)  __builtin_bswap64((uint64_t)(val))

/*
 * WQE control segment (16 bytes) — at the start of every WQE
 * See: qp.h:204 struct mlx5_wqe_ctrl_seg
 */
struct MlxWqeCtrlSeg {
    uint32_t opmod_idx_opcode;  /* [31:24]opmod [23:8]WQE index [7:0]opcode */
    uint32_t qpn_ds;            /* [31:8]QPN [7:0]WQE size (16B units) */
    uint8_t  signature;         /* XOR checksum */
    uint8_t  rsvd[2];
    uint8_t  fm_ce_se;          /* [3]fm [2]ce [1]se */
    union {
        uint32_t general_id;    /* [31:16]general_id [15:0]imm */
        uint32_t imm;
        uint32_t umr_mkey;
        uint32_t tis_tir_num;
    };
};

/* ctrl segment fm_ce_se bits */
#define MLX_WQE_CTRL_CQ_UPDATE  0x08  /* 2 << 2: generate CQE */
#define MLX_WQE_CTRL_SOLICIT    0x02  /* 1 << 1: solicited event */
#define MLX_WQE_CTRL_FENCE      0x40  /* fm=2: fence prior RDMA READ/atomic */

/*
 * WQE data segment (16 bytes) — one per SGE
 * See: qp.h:376 struct mlx5_wqe_data_seg
 */
struct MlxWqeDataSeg {
    uint32_t byte_count;
    uint32_t lkey;
    uint64_t addr;
};

/*
 * WQE remote address segment (remote address) — RDMA WRITE/READ
 * See: qp.h:372 struct mlx5_wqe_raddr_seg
 */
struct MlxWqeRaddrSeg {
    uint64_t raddr;
    uint32_t rkey;
    uint32_t rsvd;
};

/*
 * WQE atomic segment (16 bytes) — remote 64-bit compare-and-swap / fetch-add
 * See: qp.h struct mlx5_wqe_atomic_seg
 *   swap_add: swap value for CMP_SWAP, add value for FETCH_ADD
 *   compare:  compare value for CMP_SWAP (ignored by FETCH_ADD)
 */
struct MlxWqeAtomicSeg {
    uint64_t swap_add;
    uint64_t compare;
};

/*
 * WQE datagram segment — for UD, embeds mlx5_av
 * See: qp.h:388 struct mlx5_wqe_datagram_seg
 */
struct MlxWqeDatagramSeg {
    uint32_t dqp_dct;       /* destination QPN | MLX5_EXTENDED_UD_AV */
    uint32_t av;
};

/*
 * WQE Ethernet segment (16 bytes) — for Ethernet TX
 * See: qp.h:276 struct mlx5_wqe_eth_seg
 */
struct MlxWqeEthSeg {
    uint8_t  swp_outer_l4_offset;
    uint8_t  swp_outer_l3_offset;
    uint8_t  swp_inner_l4_offset;
    uint8_t  swp_inner_l3_offset;
    uint8_t  cs_flags;          /* checksum offload flags */
    uint8_t  swp_flags;
    uint16_t mss;               /* GSO segmentation size */
    uint32_t flow_table_metadata;
    union {
        struct {
            uint16_t sz;        /* inline header size */
            uint8_t  start[2];
        } inline_hdr;
        struct {
            uint16_t type;
            uint16_t vlan_tci;
        } insert;
        uint32_t trailer;
    };
};

/* cs_flags bits (See en_tx.c) */
#define MLX_ETH_WQE_L3_CSUM     0x01
#define MLX_ETH_WQE_L4_CSUM     0x02
#define MLX_ETH_WQE_L3_INNER_CSUM 0x08
#define MLX_ETH_WQE_L4_INNER_CSUM 0x10

/*
 * WQE inline segment — embedded data (small-packet optimization)
 * See: qp.h:439 struct mlx5_wqe_inline_seg
 */
struct MlxWqeInlineSeg {
    uint32_t byte_count;
    uint32_t data[];            /* embedded data */
};

/* mlx5 qp.h MLX5_INLINE_SEG: the inline segment's byte_count must set bit 31
 * to mark it as inline (a regular data seg has no flag); without it the NIC
 * decodes the payload bytes as an SGE lkey/addr and faults. */
#define MLX5_INLINE_SEG (1u << 31)

/* Inline payload ceiling. Must match MLX_UC_MAX_INLINE_DATA in MlxUCIO.h:
 * the DEXT and shim each enforce this bound, the encoder just defensively
 * refuses anything larger than the largest WQE a SQ slot can hold. */
#define MLX_WQE_MAX_INLINE 512u

/* Complete Ethernet TX WQE layout (See en.h:244 mlx5e_tx_wqe) */
struct MlxEthTxWqe {
    struct MlxWqeCtrlSeg ctrl;
    struct MlxWqeEthSeg  eth;
    struct MlxWqeDataSeg data[];
};

/*
 * Address Vector AV (28 bytes) — the core of RoCE addressing
 * See: qp.h:327 struct mlx5_av (exact replica, including qkey.reserved and reserved0)
 */
struct MlxAV {
    union {
        struct {
            uint32_t qkey;      /* UD QKEY */
            uint32_t reserved;
        } qkey;
        uint64_t dc_key;       /* for DC */
    } key;                     /* 8 bytes */
    uint32_t dqp_dct;          /* destination QPN (UD) */
    uint8_t  stat_rate_sl;     /* [7:4]static_rate [3:0]SL (Ethernet priority) */
    uint8_t  fl_mlid;          /* IB path bits */
    union {
        uint16_t rlid;         /* IB DLID */
        uint16_t udp_sport;    /* RoCEv2 source UDP port ★ */
    };                         /* 2 bytes */
    uint8_t  reserved0[4];     /* alignment padding */
    uint8_t  rmac[6];          /* destination MAC ★ */
    uint8_t  tclass;           /* DSCP | ECN bits ★ */
    uint8_t  hop_limit;        /* TTL ★ */
    uint32_t grh_gid_fl;       /* [30]=GRH present [29:20]=sgid_index [19:0]=flow_label */
    uint8_t  rgid[16];         /* destination GID (remote IP) ★ */
};

/*
 * grh_gid_fl bit definitions (See ah.c:63)
 *   grh_gid_fl = flow_label | (1 << 30) | sgid_index << 20
 *   bit30  = GRH present flag
 *   bits 29:20 = sgid_index (index into the source GID table)
 *   bits 19:0  = flow_label
 */
#define MLX_AV_GRH_PRESENT     (1u << 30)
#define MLX_AV_SGID_INDEX_SHIFT 20
#define MLX_AV_FLOW_LABEL_MASK  0xFFFFF

/* ECN bit (See ah.c:94 MLX5_ECN_ENABLED) */
#define MLX_AV_ECN_ENABLED     (1u << 1)

/*
 * CQE (64 bytes) — Completion Queue Element
 * See: device.h:808 struct mlx5_cqe64 (exact replica of all fields)
 */
struct MlxCqe64 {
    uint8_t  tls_outer_l3_tunneled;
    uint8_t  rsvd0;
    uint16_t wqe_id;
    union {
        struct {
            uint8_t  tcppsh_abort_dupack;
            uint8_t  min_ttl;
            uint16_t tcp_win;
            uint32_t ack_seq_num;
        } lro;
        struct {
            uint8_t  reserved0;
            uint8_t  header_size;
            uint16_t header_entry_index;
            uint32_t data_offset;
        } shampo;
    };
    uint32_t rss_hash_result;
    uint8_t  rss_hash_type;
    uint8_t  ml_path;
    uint8_t  rsvd20[2];
    uint16_t check_sum;
    uint16_t slid;              /* 0 under RoCE */
    uint32_t flags_rqpn;        /* low 24 bits RQPN */
    uint8_t  hds_ip_ext;
    uint8_t  l4_l3_hdr_type;
    uint16_t vlan_info;         /* RoCE VLAN */
    uint32_t srqn;              /* [31:24]lro_num_seg [23:0]srqn */
    uint32_t imm_inval_pkey;    /* immediate / inval_rkey / pkey */
    uint8_t  rsvd40[4];
    uint32_t byte_cnt;          /* data length */
    uint32_t timestamp_h;
    uint32_t timestamp_l;
    uint32_t sop_drop_qpn;      /* [31:28]sop [27:24]drop [23:0]QPN */
    uint16_t wqe_counter;
    union {
        uint8_t  signature;
        uint8_t  validity_iteration_count;
    };
    uint8_t  op_own;            /* ★ [7:4]opcode [1:0]owner */
};

/* CQE ownership phase bit is bit 0 of op_own. */
#define MLX_CQE_OWNER_MASK      1u

/* CQE opcode bits: op_own >> 4 */
#define MLX_CQE_GET_OPCODE(cqe) ((cqe)->op_own >> 4)

enum {
    MLX_CQE_REQ      = 0,
    MLX_CQE_RESP_WR_IMM = 1,
    MLX_CQE_RESP     = 2,
    MLX_CQE_RESP_SEND_IMM = 3,
    MLX_CQE_RESP_SEND_INV = 4,
    MLX_CQE_RESIZE_CQ = 5,
    MLX_CQE_SIG_ERR  = 12,
    MLX_CQE_REQ_ERR  = 13,
    MLX_CQE_RESP_ERR = 14,
    MLX_CQE_INVALID  = 15,
};

/* Compute static checks
 * Note: AV is 48 bytes (the original driver is not packed, includes alignment padding) */
#if defined(__cplusplus)
static_assert(sizeof(struct MlxWqeCtrlSeg) == 16, "ctrl seg must be 16 bytes");
static_assert(sizeof(struct MlxWqeDataSeg) == 16,  "data seg must be 16 bytes");
static_assert(sizeof(struct MlxWqeRaddrSeg) == 16, "raddr seg must be 16 bytes");
static_assert(sizeof(struct MlxWqeEthSeg) == 16,   "eth seg must be 16 bytes");
static_assert(sizeof(struct MlxAV) == 48,          "AV must be 48 bytes");
static_assert(sizeof(struct MlxCqe64) == 64,       "CQE64 must be 64 bytes");
#endif

/*
 * WQE opcodes (See wr.c)
 */
enum {
    MLX_OPCODE_SEND             = 0x0A,
    MLX_OPCODE_SEND_IMM         = 0x0B,
    MLX_OPCODE_LOCAL_INVAL      = 0x1B,
    MLX_OPCODE_RDMA_WRITE       = 0x08,
    MLX_OPCODE_RDMA_WRITE_IMM   = 0x09,
    MLX_OPCODE_RDMA_READ        = 0x10,
    MLX_OPCODE_ATOMIC_CS        = 0x11,
    MLX_OPCODE_ATOMIC_FA        = 0x12,
    MLX_OPCODE_UMR              = 0x25,
    MLX_OPCODE_NOP              = 0x00,
    MLX_OPCODE_RECV             = 0x20,
};

#define MLX_RC_MAX_SGE 16u

struct MlxRcSge {
    uint64_t addr;
    uint32_t length;
    uint32_t lkey;
};

/* Encode an RC SEND/READ/WRITE WQE with a bounded SGE list. `bufferBytes`
 * must cover the 64-byte WQEBBs used by the result; the return value is the
 * data-segment count, or zero on validation/capacity failure. */
static inline uint32_t
mlxEncodeRcSendWqe(void *buffer, size_t bufferBytes, uint32_t qpn,
                   uint16_t wqeCounter, uint8_t opcode,
                   const struct MlxRcSge *sges, uint32_t numSge,
                   uint64_t remoteAddr, uint32_t rkey, bool signaled,
                   bool fenced, bool solicited)
{
    if (!buffer || !qpn || !sges || !numSge || numSge > MLX_RC_MAX_SGE ||
        (opcode != MLX_OPCODE_SEND && opcode != MLX_OPCODE_SEND_IMM &&
         opcode != MLX_OPCODE_RDMA_WRITE && opcode != MLX_OPCODE_RDMA_WRITE_IMM &&
         opcode != MLX_OPCODE_RDMA_READ && opcode != MLX_OPCODE_LOCAL_INVAL))
        return 0;
    uint32_t ds = opcode == MLX_OPCODE_LOCAL_INVAL ? 2 :
        1 + ((opcode == MLX_OPCODE_SEND || opcode == MLX_OPCODE_SEND_IMM) ? 0 : 1) + numSge;
    size_t bytes = (size_t)ds * sizeof(struct MlxWqeDataSeg);
    size_t wqebbBytes = (bytes + 63u) & ~63u;
    if (bufferBytes < wqebbBytes) return 0;
    uint8_t *wqe = (uint8_t *)buffer;
    __builtin_memset(wqe, 0, wqebbBytes);
    struct MlxWqeCtrlSeg *ctrl = (struct MlxWqeCtrlSeg *)wqe;
    ctrl->opmod_idx_opcode = MLX_BE32(((uint32_t)wqeCounter << 8) | opcode);
    ctrl->qpn_ds = MLX_BE32((qpn << 8) | ds);
    ctrl->fm_ce_se = (signaled ? MLX_WQE_CTRL_CQ_UPDATE : 0) |
                      (fenced ? MLX_WQE_CTRL_FENCE : 0) |
                      (solicited ? MLX_WQE_CTRL_SOLICIT : 0);
    uint32_t offset = sizeof(*ctrl);
    if (opcode == MLX_OPCODE_LOCAL_INVAL) {
        struct MlxWqeDataSeg *invalidate = (struct MlxWqeDataSeg *)(wqe + offset);
        invalidate->lkey = MLX_BE32(sges[0].lkey);
        offset += sizeof(*invalidate);
    } else if (opcode != MLX_OPCODE_SEND && opcode != MLX_OPCODE_SEND_IMM) {
        struct MlxWqeRaddrSeg *remote = (struct MlxWqeRaddrSeg *)(wqe + offset);
        remote->raddr = MLX_BE64(remoteAddr);
        remote->rkey = MLX_BE32(rkey);
        offset += sizeof(*remote);
    }
    for (uint32_t i = opcode == MLX_OPCODE_LOCAL_INVAL ? 1 : 0; i < numSge; i++) {
        if (!sges[i].length) return 0;
        struct MlxWqeDataSeg *data = (struct MlxWqeDataSeg *)(wqe + offset);
        data->byte_count = MLX_BE32(sges[i].length);
        data->lkey = MLX_BE32(sges[i].lkey);
        data->addr = MLX_BE64(sges[i].addr);
        offset += sizeof(*data);
    }
    return ds;
}

static inline uint32_t
mlxEncodeLocalInvWqe(void *buffer, size_t bufferBytes, uint32_t qpn,
                     uint16_t wqeCounter, uint32_t invalidateRkey,
                     bool signaled)
{
    /* rdma-core providers/mlx5/qp.c builds LOCAL_INV as a zero-length UMR:
     * ctrl.imm=old rkey, UMR ctrl with CHECK_QPN, then a FREE mkey context.
     * It is opcode UMR (0x25), 8 DS / 128 bytes, not a standalone key seg. */
    if (!buffer || bufferBytes < 128 || !qpn || !invalidateRkey) return 0;
    uint8_t *wqe = (uint8_t *)buffer;
    __builtin_memset(wqe, 0, 128);
    struct MlxWqeCtrlSeg *ctrl = (struct MlxWqeCtrlSeg *)wqe;
    ctrl->opmod_idx_opcode = MLX_BE32(((uint32_t)wqeCounter << 8) | MLX_OPCODE_UMR);
    ctrl->qpn_ds = MLX_BE32((qpn << 8) | 8u);
    ctrl->fm_ce_se = signaled ? MLX_WQE_CTRL_CQ_UPDATE : 0;
    ctrl->imm = MLX_BE32(invalidateRkey);

    uint8_t *umr = wqe + sizeof(*ctrl);
    umr[0] = (uint8_t)(1u << 7 | 1u << 4 | 1u << 3); /* INLINE|TRANSLATION_OFFSET|CHECK_QPN */
    uint64_t mask = MLX_BE64((1ull << 29) | (1ull << 14) | (1ull << 13));
    __builtin_memcpy(umr + 8, &mask, sizeof(mask));

    uint8_t *mkey = wqe + sizeof(*ctrl) + 48;
    mkey[0] = (uint8_t)(1u << 6); /* MLX5_WQE_MKEY_CONTEXT_FREE */
    uint32_t qpnMkey = MLX_BE32(0xffffff00u);
    __builtin_memcpy(mkey + 4, &qpnMkey, sizeof(qpnMkey));
    return 8;
}

static inline uint32_t
mlxEncodeRcSendWqeImm(void *buffer, size_t bufferBytes, uint32_t qpn,
                      uint16_t wqeCounter, uint8_t opcode,
                      const struct MlxRcSge *sges, uint32_t numSge,
                      uint64_t remoteAddr, uint32_t rkey, uint32_t immData,
                      bool signaled, bool fenced, bool solicited)
{
    if (opcode != MLX_OPCODE_SEND_IMM && opcode != MLX_OPCODE_RDMA_WRITE_IMM)
        return 0;
    uint32_t ds = mlxEncodeRcSendWqe(buffer, bufferBytes, qpn, wqeCounter,
                                     opcode, sges, numSge, remoteAddr, rkey,
                                     signaled, fenced, solicited);
    if (ds) ((struct MlxWqeCtrlSeg *)buffer)->imm = immData;
    return ds;
}

/* Encode an RC inline SEND / SEND_WITH_IMM WQE. The payload is copied into
 * the WQE itself (no NIC DMA fetch) — ctrl seg + inline seg, padded to the
 * 16-byte WQE granularity. `immData` is already in device (big-endian) order
 * and only used for SEND_IMM. Returns the WQE size in 16-byte units. */
static inline uint32_t
mlxEncodeRcInlineSendWqe(void *buffer, size_t bufferBytes, uint32_t qpn,
                         uint16_t wqeCounter, uint8_t opcode,
                         const void *inlineData, uint32_t inlineLen,
                         uint32_t immData, bool signaled, bool fenced,
                         bool solicited)
{
    if (!buffer || !inlineData || !inlineLen ||
        inlineLen > MLX_WQE_MAX_INLINE ||
        (opcode != MLX_OPCODE_SEND && opcode != MLX_OPCODE_SEND_IMM))
        return 0;
    /* ctrl(16) + inline_seg(4 + len), rounded up to 16 B */
    uint32_t ds = (16u + 4u + inlineLen + 15u) / 16u;
    uint32_t bytes = ds * 16u;
    if (bufferBytes < bytes) return 0;
    uint8_t *wqe = (uint8_t *)buffer;
    __builtin_memset(wqe, 0, bytes);
    struct MlxWqeCtrlSeg *ctrl = (struct MlxWqeCtrlSeg *)wqe;
    ctrl->opmod_idx_opcode = MLX_BE32(((uint32_t)wqeCounter << 8) | opcode);
    ctrl->qpn_ds = MLX_BE32((qpn << 8) | ds);
    ctrl->fm_ce_se = (signaled ? MLX_WQE_CTRL_CQ_UPDATE : 0) |
                      (fenced ? MLX_WQE_CTRL_FENCE : 0) |
                      (solicited ? MLX_WQE_CTRL_SOLICIT : 0);
    if (opcode == MLX_OPCODE_SEND_IMM) ctrl->imm = immData;
    struct MlxWqeInlineSeg *seg = (struct MlxWqeInlineSeg *)(wqe + 16);
    seg->byte_count = MLX_BE32(inlineLen | MLX5_INLINE_SEG);
    __builtin_memcpy(seg->data, inlineData, inlineLen);
    return ds;
}

/* Encode an RC atomic WQE (CMP_SWAP 0x11 / FETCH_ADD 0x12). Layout:
 * ctrl(16) + raddr(16) + atomic(16) + result data(16) = 64 bytes (ds=4).
 * mlx5 writes the pre-operation value to the local result data segment.
 * `compare`/`swapAdd` are host-order 64-bit values, encoded big-endian. */
static inline uint32_t
mlxEncodeRcAtomicWqe(void *buffer, size_t bufferBytes, uint32_t qpn,
                     uint16_t wqeCounter, uint8_t opcode,
                     uint64_t remoteAddr, uint32_t rkey,
                     uint64_t compare, uint64_t swapAdd,
                     uint64_t resultAddr, uint32_t resultLkey, bool signaled)
{
    if (!buffer || bufferBytes < 64 || !qpn || !remoteAddr || !rkey ||
        !resultAddr || !resultLkey || (resultAddr & 7) ||
        (opcode != MLX_OPCODE_ATOMIC_CS && opcode != MLX_OPCODE_ATOMIC_FA))
        return 0;
    uint8_t *wqe = (uint8_t *)buffer;
    __builtin_memset(wqe, 0, 64);
    struct MlxWqeCtrlSeg *ctrl = (struct MlxWqeCtrlSeg *)wqe;
    ctrl->opmod_idx_opcode = MLX_BE32(((uint32_t)wqeCounter << 8) | opcode);
    ctrl->qpn_ds = MLX_BE32((qpn << 8) | 4u);
    ctrl->fm_ce_se = signaled ? MLX_WQE_CTRL_CQ_UPDATE : 0;
    struct MlxWqeRaddrSeg *raddr = (struct MlxWqeRaddrSeg *)(wqe + 16);
    raddr->raddr = MLX_BE64(remoteAddr);
    raddr->rkey = MLX_BE32(rkey);
    struct MlxWqeAtomicSeg *atom = (struct MlxWqeAtomicSeg *)(wqe + 32);
    atom->swap_add = MLX_BE64(swapAdd);
    atom->compare = MLX_BE64(compare);
    struct MlxWqeDataSeg *result = (struct MlxWqeDataSeg *)(wqe + 48);
    result->byte_count = MLX_BE32(8);
    result->lkey = MLX_BE32(resultLkey);
    result->addr = MLX_BE64(resultAddr);
    return 4;
}

/* Portable, host-tested encoder for the fixed one-SGE RC WQEBB used by
 * the current DriverKit data-path ABI. All fields are device big-endian. */
static inline uint32_t
mlxEncodeRcSendWqe64Flags(void *buffer, uint32_t qpn, uint16_t wqeCounter,
                          uint8_t opcode, uint64_t localAddr, uint32_t length,
                          uint32_t lkey, uint64_t remoteAddr, uint32_t rkey,
                          bool signaled, bool fenced, bool solicited)
{
    struct MlxRcSge sge = { localAddr, length, lkey };
    return mlxEncodeRcSendWqe(buffer, 64, qpn, wqeCounter, opcode, &sge, 1,
                              remoteAddr, rkey, signaled, fenced, solicited);
}

static inline uint32_t
mlxEncodeRcSendWqe64Ex(void *buffer, uint32_t qpn, uint16_t wqeCounter,
                       uint8_t opcode, uint64_t localAddr, uint32_t length,
                       uint32_t lkey, uint64_t remoteAddr, uint32_t rkey,
                       bool signaled)
{
    return mlxEncodeRcSendWqe64Flags(buffer, qpn, wqeCounter, opcode,
                                     localAddr, length, lkey, remoteAddr, rkey,
                                     signaled, false, false);
}

static inline uint32_t
mlxEncodeRcSendWqe64(void *buffer, uint32_t qpn, uint16_t wqeCounter,
                     uint8_t opcode, uint64_t localAddr, uint32_t length,
                     uint32_t lkey, uint64_t remoteAddr, uint32_t rkey)
{
    return mlxEncodeRcSendWqe64Ex(buffer, qpn, wqeCounter, opcode,
                                  localAddr, length, lkey, remoteAddr, rkey,
                                  true);
}

static inline bool
mlxEncodeRecvWqe64(void *buffer, uint64_t localAddr, uint32_t length,
                   uint32_t lkey)
{
    if (!buffer || !length) return false;
    uint8_t *wqe = (uint8_t *)buffer;
    __builtin_memset(wqe, 0, 64);
    struct MlxWqeDataSeg *data = (struct MlxWqeDataSeg *)wqe;
    data->byte_count = MLX_BE32(length);
    data->lkey = MLX_BE32(lkey);
    data->addr = MLX_BE64(localAddr);
    return true;
}

static inline bool
mlxEncodeRecvWqeSge(void *buffer, size_t bufferBytes,
                    const struct MlxRcSge *sges, uint32_t numSge)
{
    if (!buffer || !sges || !numSge || numSge > MLX_RC_MAX_SGE ||
        bufferBytes < (size_t)numSge * sizeof(struct MlxWqeDataSeg))
        return false;
    uint8_t *wqe = (uint8_t *)buffer;
    __builtin_memset(wqe, 0, bufferBytes);
    for (uint32_t i = 0; i < numSge; i++) {
        if (!sges[i].length) return false;
        struct MlxWqeDataSeg *data = (struct MlxWqeDataSeg *)
            (wqe + i * sizeof(*data));
        data->byte_count = MLX_BE32(sges[i].length);
        data->lkey = MLX_BE32(sges[i].lkey);
        data->addr = MLX_BE64(sges[i].addr);
    }
    return true;
}

/*
 * UMR (User-Mode Memory Registration) WQE — activates an indirect (KLM)
 * mkey. See notes/48: CREATE_MKEY alone leaves a KLM mkey in hardware's
 * "free" (unbacked) state — real DMA through it fails with LOCAL_PROT_ERR
 * until a UMR WQE clears the free bit and (re-)writes its KLM list via the
 * WQE pipeline, not the firmware command mailbox. Layout and field values
 * mirror rdma-core's authoritative userspace reference for exactly this
 * operation: providers/mlx5/qp.c's mlx5_send_wr_mkey_configure() +
 * mlx5_send_wr_set_mkey_access_flags() + mlx5_send_wr_set_mkey_layout()
 * (the implementation behind mlx5dv_wr_mkey_configure() /
 * mlx5dv_wr_set_mkey_layout_list(), the public API for composing existing
 * mkeys under one indirect mkey — our exact use case), and
 * providers/mlx5/mlx5dv.h's struct mlx5_wqe_umr_ctrl_seg /
 * mlx5_wqe_mkey_context_seg / MLX5_WQE_UMR_CTRL_* / MLX5_WQE_MKEY_CONTEXT_*
 * constants (upstream linux-rdma/rdma-core, retrieved 2026-08-28).
 *
 * C++-only from here down (struct MlxKlmEntry comes from
 * MlxP0EncodingIndirect.hpp, included above only under __cplusplus).
 */
#if defined(__cplusplus)

/* UMR control segment (48 bytes) — immediately follows the WQE ctrl seg. */
struct MlxWqeUmrCtrlSeg {
    uint8_t  flags;
    uint8_t  rsvd0[3];
    uint16_t klmOctowords;       /* be16: inline KLM data size, in octwords */
    uint16_t translationOffset;  /* be16: unused here (fresh list, offset 0) */
    uint64_t mkeyMask;           /* be64: which mkey-context fields to apply */
    uint8_t  rsvd1[32];
};

/* mkey context segment (64 bytes) — immediately follows the UMR ctrl seg.
 * Distinct wire layout from the CREATE_MKEY command's MKC (mlx5_ifc_mkc_bits,
 * see MlxP0Encoding.hpp) — same information, different transport. */
struct MlxWqeMkeyContextSeg {
    uint8_t  free;                       /* 0 = not free (activate) */
    uint8_t  reserved1;
    uint8_t  accessFlags;
    uint8_t  sf;
    uint32_t qpnMkey;                    /* be32: [31:8]=QPN [7:0]=key variant */
    uint32_t reserved2;
    uint32_t flagsPd;                    /* be32: left 0 — PD stays from CREATE_MKEY */
    uint64_t startAddr;                  /* be64: re-asserted (see notes/48) */
    uint64_t len;                        /* be64 */
    uint32_t bsfOctwordSize;             /* be32 */
    uint32_t reserved3[4];
    uint32_t translationsOctwordSize;    /* be32 */
    uint8_t  reserved4[3];
    uint8_t  logPageSize;
    uint32_t reserved5;
};

#if defined(__cplusplus)
static_assert(sizeof(struct MlxWqeUmrCtrlSeg) == 48,
              "UMR ctrl seg must be 48 bytes");
static_assert(sizeof(struct MlxWqeMkeyContextSeg) == 64,
              "mkey context seg must be 64 bytes");
#endif

enum {
    MLX_UMR_CTRL_FLAG_INLINE = 1 << 7,
};

enum {
    MLX_UMR_MASK_LEN                 = 1ull << 0,
    MLX_UMR_MASK_START_ADDR          = 1ull << 6,
    MLX_UMR_MASK_MKEY                = 1ull << 13,
    MLX_UMR_MASK_ACCESS_LOCAL_WRITE  = 1ull << 18,
    MLX_UMR_MASK_ACCESS_REMOTE_READ  = 1ull << 19,
    MLX_UMR_MASK_ACCESS_REMOTE_WRITE = 1ull << 20,
    MLX_UMR_MASK_ACCESS_ATOMIC       = 1ull << 21,
    MLX_UMR_MASK_FREE                = 1ull << 29,
};

enum {
    MLX_MKEY_CTX_ACCESS_LOCAL_WRITE  = 1 << 3,
    MLX_MKEY_CTX_ACCESS_REMOTE_READ  = 1 << 4,
    MLX_MKEY_CTX_ACCESS_REMOTE_WRITE = 1 << 5,
    MLX_MKEY_CTX_ACCESS_ATOMIC       = 1 << 6,
};

#define MLX_UMR_KLM_ALIGN_BYTES  64
#define MLX_UMR_WQE_FIXED_BYTES  (16 + 48 + 64)  /* ctrl + umr_ctrl + mkey_ctx */

/* Total WQE byte size for a given KLM entry count — for sizing caller buffers. */
static inline uint32_t
mlxUmrKlmWqeBytes(uint32_t klmCount)
{
    uint32_t klmBytes = klmCount * MLX_KLM_ENTRY_BYTES;
    uint32_t klmAligned = ((klmBytes + MLX_UMR_KLM_ALIGN_BYTES - 1) /
                           MLX_UMR_KLM_ALIGN_BYTES) * MLX_UMR_KLM_ALIGN_BYTES;
    return MLX_UMR_WQE_FIXED_BYTES + klmAligned;
}

/* Encodes a full UMR WQE (ctrl + umr_ctrl + mkey_context + inline KLM list)
 * into a flat, linear buffer (the caller copies it into the SQ ring,
 * wrapping across WQEBBs as needed — this encoder has no notion of ring
 * wraparound). `mkey` is the indirect mkey's own composed key (the same
 * value already used as its lkey/rkey). `accessFlags`/`startAddress`/
 * `totalLen` are the mkey's own access flags, base address, and logical
 * length (as already set at CREATE_MKEY time) — start_addr and len are
 * re-asserted here (MASK_START_ADDR/MASK_LEN) rather than left to whatever
 * CREATE_MKEY set, since it's untested whether firmware preserves
 * un-masked fields across the free-bit-clearing transition for a mkey
 * that has never had any other UMR applied to it (unlike the upstream
 * reference's typical lifecycle, where a mkey usually already went
 * through at least one prior UMR — see notes/48). PD and access_mode are
 * NOT touched, matching the upstream reference (there is no mask bit for
 * access_mode at all, and PD isn't ours to change).
 * Returns the WQE size in 16-byte units (`ds`, for the ctrl segment's own
 * qpn_ds field and for the caller's WQEBB-span accounting), or 0 on error. */
static inline uint32_t
mlxEncodeUmrKlmWqe(void *buffer, size_t capacity, uint32_t qpn,
                   uint16_t wqeCounter, uint32_t mkey, uint32_t accessFlags,
                   uint64_t startAddress, uint64_t totalLen,
                   const struct MlxKlmEntry *klms, uint32_t klmCount,
                   bool signaled)
{
    if (!buffer || !qpn || !mkey || !klms || !klmCount)
        return 0;
    uint32_t totalBytes = mlxUmrKlmWqeBytes(klmCount);
    if (totalBytes > capacity) return 0;

    uint8_t *wqe = (uint8_t *)buffer;
    memset(wqe, 0, totalBytes);
    uint32_t ds = totalBytes / 16;

    struct MlxWqeCtrlSeg *ctrl = (struct MlxWqeCtrlSeg *)wqe;
    ctrl->opmod_idx_opcode = MLX_BE32(((uint32_t)wqeCounter << 8) | MLX_OPCODE_UMR);
    ctrl->qpn_ds = MLX_BE32((qpn << 8) | ds);
    ctrl->fm_ce_se = signaled ? MLX_WQE_CTRL_CQ_UPDATE : 0;
    ctrl->umr_mkey = MLX_BE32(mkey);

    struct MlxWqeUmrCtrlSeg *u = (struct MlxWqeUmrCtrlSeg *)(wqe + 16);
    u->flags = MLX_UMR_CTRL_FLAG_INLINE;
    uint32_t klmAligned = totalBytes - MLX_UMR_WQE_FIXED_BYTES;
    u->klmOctowords = MLX_BE16((uint16_t)(klmAligned / 16));
    uint64_t mask = MLX_UMR_MASK_FREE | MLX_UMR_MASK_MKEY | MLX_UMR_MASK_LEN |
                    MLX_UMR_MASK_START_ADDR;
    if (accessFlags & MLX_MR_ACCESS_LOCAL_WRITE)
        mask |= MLX_UMR_MASK_ACCESS_LOCAL_WRITE;
    if (accessFlags & MLX_MR_ACCESS_REMOTE_READ)
        mask |= MLX_UMR_MASK_ACCESS_REMOTE_READ;
    if (accessFlags & MLX_MR_ACCESS_REMOTE_WRITE)
        mask |= MLX_UMR_MASK_ACCESS_REMOTE_WRITE;
    if (accessFlags & MLX_MR_ACCESS_REMOTE_ATOMIC)
        mask |= MLX_UMR_MASK_ACCESS_ATOMIC;
    u->mkeyMask = MLX_BE64(mask);

    struct MlxWqeMkeyContextSeg *mk =
        (struct MlxWqeMkeyContextSeg *)(wqe + 16 + 48);
    mk->free = 0;
    mk->accessFlags = (uint8_t)(
        (accessFlags & MLX_MR_ACCESS_LOCAL_WRITE  ? MLX_MKEY_CTX_ACCESS_LOCAL_WRITE  : 0) |
        (accessFlags & MLX_MR_ACCESS_REMOTE_READ  ? MLX_MKEY_CTX_ACCESS_REMOTE_READ  : 0) |
        (accessFlags & MLX_MR_ACCESS_REMOTE_WRITE ? MLX_MKEY_CTX_ACCESS_REMOTE_WRITE : 0) |
        (accessFlags & MLX_MR_ACCESS_REMOTE_ATOMIC ? MLX_MKEY_CTX_ACCESS_ATOMIC : 0));
    mk->qpnMkey = MLX_BE32(0xffffff00u | (mkey & 0xffu));
    mk->startAddr = MLX_BE64(startAddress);
    mk->len = MLX_BE64(totalLen);

    uint8_t *entries = wqe + MLX_UMR_WQE_FIXED_BYTES;
    for (uint32_t i = 0; i < klmCount; i++) {
        struct MlxWqeDataSeg *e = (struct MlxWqeDataSeg *)(entries + i * 16);
        e->byte_count = MLX_BE32((uint32_t)klms[i].byteCount);
        e->lkey = MLX_BE32(klms[i].mkey);
        e->addr = MLX_BE64(klms[i].address);
    }
    return ds;
}

#endif /* defined(__cplusplus) */

#endif /* MLX_WQE_HPP */
