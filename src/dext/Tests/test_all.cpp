/*
 * test_all.cpp — host-testable suite for the portable mlx5 IFC slice.
 *
 * Verifies the bit helpers (MlxIfcHelpers, MSB-first) and the P0/P1 encoders
 * (QP state transitions, RoCE primary path, CREATE_MKEY/MKL, MTT page count).
 * These encoders are identical between the kext and the DriverKit DEXT, so
 * passing here is the correctness gate before any hardware is touched
 * (REMEDIATION_PLAN §4.1, §4.2, §11.2).
 */
#include "../Sources/hw/MlxIfcHelpers.hpp"
#include "../Sources/hw/MlxP0Encoding.hpp"
#include "../Sources/hw/MlxP1Encoding.hpp"
#include "../Sources/hw/MlxWQE.hpp"
#include "../Sources/hw/MlxRegs.hpp"
#include <cstdint>
#include <cstring>
#include <cstdio>

static int failures = 0;
static void check(bool cond, const char *msg)
{
    if (!cond) { failures++; printf("FAIL: %s\n", msg); }
    else        printf("ok:   %s\n", msg);
}

/* ---- bit helpers ---- */
static void test_bits(void)
{
    uint8_t buf[16] = {0};
    mlxSetBits(buf, 0, 4, 0xA);
    check(mlxGetBits(buf, 0, 4) == 0xA, "4-bit field at bit 0 round-trips");
    check(buf[0] == 0xA0, "high nibble stored MSB-first (buf[0]==0xA0)");

    memset(buf, 0, sizeof(buf));
    mlxSetBits(buf, 4, 8, 0x5A);
    check(mlxGetBits(buf, 4, 8) == 0x5A, "8-bit field crossing byte 0/1 round-trips");
    /* MSB-first layout: value 0x5A = 01011010 spreads as
     *   byte 0 bits[7:4] = 0101 -> 0x05
     *   byte 1 bits[7:4] = 1010 -> 0xA0 */
    check(buf[0] == 0x05, "cross-byte field low half (buf[0]==0x05)");
    check(buf[1] == 0xA0, "cross-byte field high half (buf[1]==0xA0)");

    memset(buf, 0, sizeof(buf));
    uint64_t val = 0x0123456789ABCDEFULL;
    mlxSetBits(buf, 8, 64, val);
    check(mlxGetBits(buf, 8, 64) == val, "64-bit field at bit 8 round-trips");

    memset(buf, 0, sizeof(buf));
    mlxSetBits(buf, 0, 8, 0xFF);
    mlxSetBits(buf, 12, 8, 0x3C);
    check(mlxGetBits(buf, 0, 8) == 0xFF, "byte 0 untouched by later write");
    check(mlxGetBits(buf, 12, 8) == 0x3C, "straddling field round-trips");
}

/* ---- QP RST->INIT encoder ---- */
static void test_rst2init(void)
{
    uint8_t qpc[MLX_QPC_BYTES] = {0};
    uint32_t mask = 0;
    bool ok = mlxEncodeRst2InitQpc(qpc, sizeof(qpc), 0x10, 1, &mask);
    check(ok, "RST2INIT encoder returns success");
    check(mlxGetBits(qpc, 0x08, 8) == MLX_QP_ST_RC, "QPC transport = RC (0)");
    check(mlxGetBits(qpc, 0x13, 2) == 3, "RST2INIT path migration state = MIGRATED");
    check(mlxGetBits(qpc, MLX_QPC_PRIMARY_PATH_BIT_OFFSET + 0x10, 16) == 0x10,
          "QPC pkey_index encoded");
    check(mlxGetBits(qpc, MLX_QPC_PRIMARY_PATH_BIT_OFFSET + 0x128, 8) == 1,
          "QPC port_num encoded");
    check(mask == 0, "RST2INIT required fields do not enter opt mask");
}

/* ---- RoCE primary path encoder ---- */
static void test_roce_path(void)
{
    uint8_t qpc[MLX_QPC_BYTES] = {0};
    MlxRocePathFields path = {};
    uint8_t dgid[16] = {0,0,0,0,0,0,0,0,0,0,0xff,0xff,192,168,200,2};
    uint8_t dmac[6]  = {0x4c,0xbb,0x47,0x7d,0xa1,0xa4};
    memcpy(path.dgid, dgid, 16);
    memcpy(path.dmac, dmac, 6);
    path.sgidIndex = 1;
    path.hopLimit = 64;
    path.udpSport = 0x1234;
    path.pkeyIndex = 0;
    path.portNum = 1;
    bool ok = mlxEncodeRocePrimaryPath(qpc, sizeof(qpc), &path);
    check(ok, "RoCE primary path encoder returns success");
    const uint32_t ads = MLX_QPC_PRIMARY_PATH_BIT_OFFSET;
    check(memcmp(qpc + (ads + 0x80)/8, dgid, 16) == 0, "dgid copied into QPC");
    check(memcmp(qpc + (ads + 0x130)/8, dmac, 6) == 0, "dmac copied into QPC");
    check(mlxGetBits(qpc, ads + 0x110, 16) == 0x1234, "udp source port encoded");
    check(mlxGetBits(qpc, ads + 0x58, 8) == 64, "hop limit encoded");
    check(mlxGetBits(qpc, ads + 0x48, 8) == 1, "sgid_index encoded");
}

static void test_qp_transitions(void)
{
    uint8_t qpc[MLX_QPC_BYTES] = {0};
    MlxRocePathFields path = {};
    path.dmac[0] = 0x4c; path.dmac[5] = 0xa4;
    path.dgid[10] = 0xff; path.dgid[11] = 0xff;
    path.dgid[12] = 192; path.dgid[13] = 168;
    path.dgid[14] = 200; path.dgid[15] = 2;
    path.sgidIndex = 3;
    path.hopLimit = 64;
    path.trafficClass = 0xab;
    path.udpSport = 0xc123;
    path.portNum = 1;
    uint32_t mask = 0;
    bool ok = mlxEncodeInit2RtrQpc(qpc, sizeof(qpc), &path, 0x123456,
                                    3, 0x654321, 12, 8, 3, 30, &mask);
    check(ok, "INIT2RTR encoder returns success");
    check(mlxGetBits(qpc, 0x40, 3) == 3, "INIT2RTR MTU=1024 encoded");
    check(mlxGetBits(qpc, 0x43, 5) == 30, "INIT2RTR log_msg_max encoded");
    check(mlxGetBits(qpc, 0xa8, 24) == 0x123456, "remote QPN encoded");
    check(mlxGetBits(qpc, 0x4a8, 24) == 0x654321, "RQ PSN encoded");
    check(mlxGetBits(qpc, 0x4a3, 5) == 12, "INIT2RTR min RNR timer encoded");
    check(mlxGetBits(qpc, 0x488, 3) == 3, "max_dest_rd_atomic log encoded");
    check(mlxGetBits(qpc, 0x48c, 4) == 3,
          "atomic_mode encoded from the passed value (8B)");
    check(mlxGetBits(qpc, 0x492, 1) == 1, "remote atomic (rae) enabled");
    check(mlxGetBits(qpc, 0x13, 2) == 3, "INIT2RTR path migration state = MIGRATED");
    check(mlxGetBits(qpc, MLX_QPC_PRIMARY_PATH_BIT_OFFSET + 0x64, 8) == 0xab,
          "full traffic class encoded");
    check(mask == ((1u << 1) | (1u << 2) | (1u << 3)),
          "INIT2RTR mask excludes transition-illegal RRA_MAX");

    /* Standard ops take precedence over an extended size mask. ConnectX-4
     * reports this combination; encoding sizeQp's highest bit as mode 5
     * makes the 8-byte atomic WQE fail with syndrome 0x6802. */
    MlxP1AtomicCaps atomicCaps = {
        (uint16_t)(MLX_P1_ATOMIC_OPS_CMP_SWAP |
                   MLX_P1_ATOMIC_OPS_FETCH_ADD |
                   MLX_P1_ATOMIC_OPS_EXTENDED_FETCH_ADD),
        (uint16_t)(1u << 5)
    };
    check(mlxP1AtomicMode(&atomicCaps) == MLX_ATOMIC_MODE_IB_COMP,
          "standard atomics use IB_COMP despite extended size mask");

    memset(qpc, 0, sizeof(qpc));
    mask = 0;
    ok = mlxEncodeRtr2RtsQpc(qpc, sizeof(qpc), 0xabcdef, 8,
                              14, 7, 7, &mask);
    check(ok, "RTR2RTS encoder returns success");
    check(mlxGetBits(qpc, 0x3c8, 24) == 0xabcdef, "SQ PSN encoded");
    check(mlxGetBits(qpc, MLX_QPC_PRIMARY_PATH_BIT_OFFSET + 0x40, 5) == 14,
          "ACK timeout encoded");
    check(mlxGetBits(qpc, 0x38d, 3) == 7 && mlxGetBits(qpc, 0x390, 3) == 7,
          "transport retry counters encoded");
    check(mlxGetBits(qpc, 0x13, 2) == 3, "RTR2RTS path migration state = MIGRATED");
    check(mask == 0, "RTR2RTS transition uses a zero opt mask");
}

static void test_wqe_encoding(void)
{
    uint8_t wqe[64] = {};
    uint32_t ds = mlxEncodeRcSendWqe64(wqe, 0x123456, 0xabcd,
                                        MLX_OPCODE_RDMA_READ,
                                        0x1122334455667788ULL, 4096, 0xaabbccdd,
                                        0x8877665544332211ULL, 0xdeadbeef);
    const MlxWqeCtrlSeg *ctrl = (const MlxWqeCtrlSeg *)wqe;
    const MlxWqeRaddrSeg *remote = (const MlxWqeRaddrSeg *)(wqe + 16);
    const MlxWqeDataSeg *data = (const MlxWqeDataSeg *)(wqe + 32);
    check(ds == 3, "RDMA READ WQE has three data segments");
    check(MLX_BE32(ctrl->opmod_idx_opcode) ==
          ((0xabcdu << 8) | MLX_OPCODE_RDMA_READ),
          "RDMA READ uses hardware opcode 0x10");
    check(MLX_BE32(ctrl->qpn_ds) == ((0x123456u << 8) | 3),
          "qpn_ds uses qpn<<8 | ds layout");
    check(MLX_BE64(remote->raddr) == 0x8877665544332211ULL &&
          MLX_BE32(remote->rkey) == 0xdeadbeef,
          "remote address segment encoded");
    check(MLX_BE64(data->addr) == 0x1122334455667788ULL &&
          MLX_BE32(data->lkey) == 0xaabbccdd &&
          MLX_BE32(data->byte_count) == 4096,
          "local data segment encoded");

    memset(wqe, 0, sizeof(wqe));
    ds = mlxEncodeRcSendWqe64(wqe, 1, 0xffff, MLX_OPCODE_SEND,
                               0x1000, 64, 0x1234, 0, 0);
    check(ds == 2 && MLX_BE32(((MlxWqeCtrlSeg *)wqe)->qpn_ds) == 0x102,
          "SEND WQE has two segments");

    memset(wqe, 0, sizeof(wqe));
    ds = mlxEncodeRcSendWqe64Ex(wqe, 1, 7, MLX_OPCODE_RDMA_WRITE,
                                 0x2000, 64, 0x1234, 0x3000, 0x5678, false);
    check(ds == 3 && ((MlxWqeCtrlSeg *)wqe)->fm_ce_se == 0,
          "unsignaled RDMA WRITE suppresses its CQE");
    ds = mlxEncodeRcSendWqe64Ex(wqe, 1, 8, MLX_OPCODE_RDMA_WRITE,
                                 0x2000, 64, 0x1234, 0x3000, 0x5678, true);
    check(ds == 3 &&
          ((MlxWqeCtrlSeg *)wqe)->fm_ce_se == MLX_WQE_CTRL_CQ_UPDATE,
          "final signaled RDMA WRITE requests one CQE");

    ds = mlxEncodeRcSendWqe64Flags(wqe, 1, 9, MLX_OPCODE_SEND,
                                   0x1000, 64, 0x1234, 0, 0,
                                   true, true, true);
    check(ds == 2 &&
          ((MlxWqeCtrlSeg *)wqe)->fm_ce_se == (MLX_WQE_CTRL_CQ_UPDATE |
                                               MLX_WQE_CTRL_FENCE |
                                               MLX_WQE_CTRL_SOLICIT),
          "FENCE and SOLICITED reach WQE control segment");

    MlxRcSge immediateSge = { 0x1000, 64, 0x1234 };
    ds = mlxEncodeRcSendWqeImm(wqe, sizeof(wqe), 1, 10, MLX_OPCODE_SEND_IMM,
                               &immediateSge, 1, 0, 0,
                               0x11223344, true, false, false);
    check(ds == 2 && MLX_BE32(((MlxWqeCtrlSeg *)wqe)->opmod_idx_opcode) ==
          ((10u << 8) | MLX_OPCODE_SEND_IMM) &&
          ((MlxWqeCtrlSeg *)wqe)->imm == 0x11223344,
          "SEND_WITH_IMM opcode and immediate data encoded");
    ds = mlxEncodeRcSendWqeImm(wqe, sizeof(wqe), 1, 11,
                               MLX_OPCODE_RDMA_WRITE_IMM, &immediateSge, 1,
                               0x2000, 0x5678, 0xaabbccdd,
                               false, false, false);
    check(ds == 3 && MLX_BE32(((MlxWqeCtrlSeg *)wqe)->opmod_idx_opcode) ==
          ((11u << 8) | MLX_OPCODE_RDMA_WRITE_IMM) &&
          ((MlxWqeCtrlSeg *)wqe)->imm == 0xaabbccdd,
          "RDMA_WRITE_WITH_IMM opcode and immediate data encoded");

    MlxRcSge sges[2] = {
        { 0x100000, 1024, 0x111 },
        { 0x200000, 2048, 0x222 },
    };
    uint8_t multi[128] = {};
    ds = mlxEncodeRcSendWqe(multi, sizeof(multi), 0x123, 7,
                            MLX_OPCODE_SEND, sges, 2, 0, 0, true,
                            false, false);
    check(ds == 3, "multi-SGE SEND has ctrl plus two data segments");
    check(MLX_BE32(((MlxWqeCtrlSeg *)multi)->qpn_ds) == ((0x123u << 8) | 3),
          "multi-SGE SEND qpn_ds encodes DS count");
    const MlxWqeDataSeg *first = (const MlxWqeDataSeg *)(multi + 16);
    const MlxWqeDataSeg *second = (const MlxWqeDataSeg *)(multi + 32);
    check(MLX_BE32(first->byte_count) == 1024 && MLX_BE32(first->lkey) == 0x111,
          "first multi-SGE data segment encoded");
    check(MLX_BE32(second->byte_count) == 2048 && MLX_BE32(second->lkey) == 0x222,
          "second multi-SGE data segment encoded");
    check(!mlxEncodeRcSendWqe(multi, 32, 0x123, 7, MLX_OPCODE_SEND,
                              sges, 2, 0, 0, true, false, false),
          "multi-SGE encoder rejects undersized WQE buffer");
}

/* ---- CREATE_MKEY encoder ---- */
static void test_create_mkey(void)
{
    uint64_t pages[1] = { 0x10000 };
    uint8_t in[MLX_CREATE_MKEY_FIXED_BYTES + 16] = {0};
    uint32_t inputSize = 0;
    bool ok = mlxEncodeCreateMkey(in, sizeof(in), pages, 1,
                                  0x10000, 0x1000,
                                  MLX_MR_ACCESS_LOCAL_WRITE | MLX_MR_ACCESS_REMOTE_WRITE,
                                  1, 0x42, &inputSize);
    check(ok, "CREATE_MKEY encoder returns success");
    check(inputSize > MLX_CREATE_MKEY_FIXED_BYTES, "input size includes MTT octword");
    const uint8_t *mkc = in + MLX_CREATE_MKEY_MKC_BIT_OFFSET/8;
    check(mlxGetBits(mkc, 0x14, 1) == 1, "MKC local_write bit set");
    check(mlxGetBits(mkc, 0x12, 1) == 1, "MKC remote_write bit set");
    check(mlxGetBits(mkc, 0x15, 1) == 1, "MKC local_read bit set (mandatory)");
    check(mlxGetBits(mkc, 0x10, 1) == 1,
          "MKC umr_en matches Linux populated MTT registration");
    check(mlxGetBits(mkc, 0x60, 1) == 0,
          "MKC length64 clear for a finite-length MR");
    check(mlxGetBits(mkc, 0x68, 24) == 1, "MKC pd encoded");
    check(mlxGetBits(mkc, 0x80, 64) == 0x10000, "MKC start_addr encoded");
    check(mlxGetBits(mkc, 0xc0, 64) == 0x1000, "MKC length encoded");
    check(mlxGetBits(mkc, 0x1a0, 32) == 1,
          "MKC translations size is one octword for one MTT entry");
    check(mlxGetBits(in, MLX_CREATE_MKEY_ACTUAL_XLT_BIT_OFFSET, 32) == 1,
          "CREATE_MKEY actual translation size matches MKC");
    check(mlxGetBits(mkc, 0x1da, 6) == MLX_MTT_PAGE_SHIFT,
          "MKC absolute log_page_size encoded");
    check(mlxGetBits(mkc, 0x38, 8) == 0x42, "MKC key variant encoded");
    check(mlxGetBits(in, 0x880, 64) == 0x10000, "MTT page 0 IOVA encoded");
    check(mlxComposeMkey(mlxMkeyIndex(mlxComposeMkey(5, 0x42)), 0x42) ==
          mlxComposeMkey(5, 0x42), "lkey round-trips index/variant split");

    /* The MR virtual address is independent from its DMA translation.  A
     * two-page unaligned virtual span must retain that VA in MKC while PAS
     * carries unrelated device addresses. */
    uint64_t translated[2] = { 0x90000, 0xa0000 };
    memset(in, 0, sizeof(in));
    ok = mlxEncodeCreateMkey(in, sizeof(in), translated, 2,
                             0x700000000800ULL, 0x1000,
                             MLX_MR_ACCESS_LOCAL_WRITE, 1, 0x43, &inputSize);
    mkc = in + MLX_CREATE_MKEY_MKC_BIT_OFFSET/8;
    check(ok && mlxGetBits(mkc, 0x80, 64) == 0x700000000800ULL,
          "MKC preserves client VA independently of PAS");
    check(mlxGetBits(in, 0x880, 64) == translated[0] &&
          mlxGetBits(in, 0x8c0, 64) == translated[1],
          "MTT carries independent DMA translations");
}

/* ---- Fragmented / multi-page MR (AppleMCX §4.2 acceptance) ---- */
static void test_create_mkey_fragmented(void)
{
    /* 4 KiB / 64 KiB / 1 MiB spans must yield the exact MTT page count the
     * Linux mlx5 PBL would produce. */
    check(mlxMttPageCount(0x10000, 0x1000) == 1,     "4 KiB MR -> 1 PAS page");
    check(mlxMttPageCount(0x10000, 0x10000) == 16,   "64 KiB MR -> 16 PAS pages");
    check(mlxMttPageCount(0x10000, 0x100000) == 256, "1 MiB MR -> 256 PAS pages");

    /* A contiguous client VA with a NON-contiguous (fragmented) DMA
     * translation: 4 scattered 4 KiB pages. This is the same geometry class
     * as the 576-byte-mailbox bug (notes/35) — a page list that is neither
     * single-segment nor single-page must round-trip bit-for-bit. */
    uint64_t frag[4] = { 0x10000, 0x40000, 0x50000, 0x90000 };
    uint8_t  in[4096] = {0};
    uint32_t size = 0;
    bool ok = mlxEncodeCreateMkey(in, sizeof(in), frag, 4,
                                  0x700000000000ULL, 0x4000,
                                  MLX_MR_ACCESS_LOCAL_WRITE, 1, 0x44, &size);
    check(ok, "fragmented 4-page MR encodes");
    check(size == MLX_CREATE_MKEY_FIXED_BYTES + mlxMttOctwordCount(4) * 16,
          "fragmented MR input size accounts for every octword");
    const uint8_t *mkc = in + MLX_CREATE_MKEY_MKC_BIT_OFFSET/8;
    check(mlxGetBits(mkc, 0x1a0, 32) == mlxMttOctwordCount(4),
          "fragmented MR translations_octword_size = 2 octwords");
    check(mlxGetBits(in, MLX_CREATE_MKEY_ACTUAL_XLT_BIT_OFFSET, 32) ==
          mlxMttOctwordCount(4), "fragmented MR actual xlt matches MKC");
    for (uint32_t i = 0; i < 4; i++)
        check(mlxGetBits(in, 0x880 + i * 64, 64) == frag[i],
              "fragmented MR PAS[i] encoded at its bit offset");
    check(mlxGetBits(in, 0x880 + 4 * 64, 64) == 0,
          "fragmented MR does not write past its PAS list");

    /* 1 MiB single-segment MR: the largest finite span in the matrix. */
    uint64_t big[256];
    for (uint32_t i = 0; i < 256; i++)
        big[i] = 0x80000000ULL + i * 0x1000ULL;
    memset(in, 0, sizeof(in));
    ok = mlxEncodeCreateMkey(in, sizeof(in), big, 256,
                             0x800000000000ULL, 0x100000,
                             MLX_MR_ACCESS_REMOTE_WRITE, 1, 0x45, &size);
    check(ok, "1 MiB MR encodes");
    check(size == MLX_CREATE_MKEY_FIXED_BYTES + 128 * 16,
          "1 MiB MR input size = 272 + 128 octwords");
    check(mlxGetBits(in + MLX_CREATE_MKEY_MKC_BIT_OFFSET/8, 0x1a0, 32) == 128,
          "1 MiB MR translations_octword_size = 128");
    check(mlxGetBits(in, 0x880, 64) == big[0] &&
          mlxGetBits(in, 0x880 + 255 * 64, 64) == big[255],
          "1 MiB MR first and last PAS entries round-trip");
}

/* ---- MTT page splitting (16KiB host → 4KiB HCA) ---- */
static void test_mtt(void)
{
    check(mlxMttPageCount(0x10000, 0x4000) == 4, "16KiB region -> 4 PAS pages");
    check(mlxMttPageCount(0x10500, 0x4000) == 5, "misaligned 16KiB -> 5 PAS pages");
    /* appendMttPages dedups adjacent pages within the same 4KiB page */
    uint64_t out[8] = {0};
    uint32_t n = 0;
    bool ok = mlxAppendMttPages(0x1000, 0x800, out, 8, &n);
    check(ok && n == 1, "two offsets in one 4KiB page dedup to 1 MTT entry");
}

/* ---- MANAGE_PAGES(TAKE) decoder ---- */
static void test_manage_pages_take(void)
{
    uint8_t out[16 + 8 * 2] = {0};   /* 16B header + 2 PAS */
    uint64_t pas[8] = {0};
    uint32_t n = 0;

    /* Empty response: output_num_entries = 0. */
    bool ok = mlxP1DecodeManagePagesTake(out, sizeof(out), 2, &n, pas, 8);
    check(ok && n == 0, "TAKE decoder: zero response ok");

    /* Partial return: 1 of 2 requested. */
    memset(out, 0, sizeof(out));
    mlxSetBits(out, 0x40, 32, 1);
    mlxSetBits(out, 0x80, 64, 0x81234000ULL);
    ok = mlxP1DecodeManagePagesTake(out, sizeof(out), 2, &n, pas, 8);
    check(ok && n == 1 && pas[0] == 0x81234000ULL,
          "TAKE decoder: partial return parse");

    /* Full return: 2 of 2, two PAS. */
    memset(out, 0, sizeof(out));
    mlxSetBits(out, 0x40, 32, 2);
    mlxSetBits(out, 0x80, 64, 0x81234000ULL);
    mlxSetBits(out, 0xc0, 64, 0x81235000ULL);
    ok = mlxP1DecodeManagePagesTake(out, sizeof(out), 2, &n, pas, 8);
    check(ok && n == 2 && pas[0] == 0x81234000ULL && pas[1] == 0x81235000ULL,
          "TAKE decoder: full return, 2 PAS");

    /* Corruption: returned > requested → false. */
    memset(out, 0, sizeof(out));
    mlxSetBits(out, 0x40, 32, 3);   /* requested == 2 */
    ok = mlxP1DecodeManagePagesTake(out, sizeof(out), 2, &n, pas, 8);
    check(!ok, "TAKE decoder: returned>requested rejected");

    /* Short buffer: n PAS does not fit. */
    memset(out, 0, sizeof(out));
    mlxSetBits(out, 0x40, 32, 2);
    ok = mlxP1DecodeManagePagesTake(out, 16 + 8 * 2, 2, &n, pas, 8);   /* enough length */
    check(ok && n == 2, "TAKE decoder: exact-length buffer ok");
    ok = mlxP1DecodeManagePagesTake(out, 16 + 8, 2, &n, pas, 8);       /* not enough */
    check(!ok, "TAKE decoder: short buffer rejected");

    /* Runtime allocation failure: op_mod=0, no PAS, input_num_entries=0. */
    uint8_t failIn[16] = {};
    ok = mlxP1EncodeManagePages(failIn, sizeof(failIn),
                                MLX_P1_PAGES_ALLOCATION_FAIL,
                                0x1234, false, NULL, 0);
    check(ok, "MANAGE_PAGES allocation-fail encoder accepts empty PAS");
    check(mlxGetBits(failIn, 0x30, 16) == MLX_P1_PAGES_ALLOCATION_FAIL,
          "MANAGE_PAGES allocation-fail op_mod encoded");
    check(mlxGetBits(failIn, 0x50, 16) == 0x1234,
          "MANAGE_PAGES allocation-fail function_id encoded");
    check(mlxGetBits(failIn, 0x60, 32) == 0,
          "MANAGE_PAGES allocation-fail has zero entries");
}

static void test_general_caps_sw_owner(void)
{
    uint8_t caps[MLX_P1_HCA_CAP_BYTES] = {};
    MlxP1GeneralCaps parsed = {};
    mlxSetBits(caps, 0x61e, 1, 1);
    bool ok = mlxP1ParseGeneralCaps(caps, sizeof(caps), &parsed);
    check(ok && parsed.swOwnerId,
          "general caps parse sw_owner_id at IFC bit 0x61e");
}

static void test_inline_atomic_wqe(void)
{
    uint8_t wqe[MLX_WQE_MAX_INLINE + 64] = {};
    const uint8_t payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    /* Inline SEND: ctrl(16) + inline_seg(4+len), ds = ceil((20+len)/16). */
    uint32_t ds = mlxEncodeRcInlineSendWqe(wqe, sizeof(wqe), 0x123456, 0xabcd,
                                            MLX_OPCODE_SEND, payload, 8, 0,
                                            true, false, false);
    const MlxWqeCtrlSeg *ctrl = (const MlxWqeCtrlSeg *)wqe;
    const MlxWqeInlineSeg *seg = (const MlxWqeInlineSeg *)(wqe + 16);
    check(ds == 2, "inline SEND of 8 bytes has ds=2 (28 -> 32)");
    check(MLX_BE32(ctrl->opmod_idx_opcode) ==
          ((0xabcdu << 8) | MLX_OPCODE_SEND), "inline SEND uses SEND opcode");
    check(MLX_BE32(ctrl->qpn_ds) == ((0x123456u << 8) | 2),
          "inline SEND qpn_ds encodes ds=2");
    check(MLX_BE32(seg->byte_count) == (8u | MLX5_INLINE_SEG),
          "inline seg byte_count carries the MLX5_INLINE_SEG flag");
    check(memcmp(seg->data, payload, 8) == 0, "inline payload copied verbatim");

    /* Inline SEND_IMM: imm lands in ctrl, ds grows with payload. */
    memset(wqe, 0, sizeof(wqe));
    uint8_t big[100];
    for (int i = 0; i < 100; i++) big[i] = (uint8_t)i;
    ds = mlxEncodeRcInlineSendWqe(wqe, sizeof(wqe), 7, 1, MLX_OPCODE_SEND_IMM,
                                  big, 100, 0xdeadbeef, false, true, true);
    check(ds == 8, "inline SEND_IMM of 100 bytes -> ds=8 (120 -> 128)");
    check(((MlxWqeCtrlSeg *)wqe)->imm == 0xdeadbeef, "SEND_IMM imm in ctrl");
    check(((MlxWqeCtrlSeg *)wqe)->fm_ce_se == (MLX_WQE_CTRL_FENCE |
                                              MLX_WQE_CTRL_SOLICIT),
          "inline SEND_IMM fence+solicit flags set");

    /* Inline validation: zero length and non-SEND opcode are refused. */
    ds = mlxEncodeRcInlineSendWqe(wqe, sizeof(wqe), 1, 1, MLX_OPCODE_SEND,
                                  payload, 0, 0, true, false, false);
    check(ds == 0, "inline zero-length refused");
    ds = mlxEncodeRcInlineSendWqe(wqe, sizeof(wqe), 1, 1, MLX_OPCODE_RDMA_WRITE,
                                  payload, 8, 0, true, false, false);
    check(ds == 0, "inline non-SEND opcode refused");

    /* Atomic CMP_SWAP: ctrl + raddr + atomic, ds=3. */
    memset(wqe, 0, sizeof(wqe));
    ds = mlxEncodeRcAtomicWqe(wqe, sizeof(wqe), 0x123456, 0xbeef,
                              MLX_OPCODE_ATOMIC_CS, 0x8877665544332211ULL,
                              0xdeadbeef, 0x0102030405060708ULL,
                              0x1112131415161718ULL, 0x2000, 0x3000, true);
    const MlxWqeRaddrSeg *raddr = (const MlxWqeRaddrSeg *)(wqe + 16);
    const MlxWqeAtomicSeg *atom = (const MlxWqeAtomicSeg *)(wqe + 32);
    check(ds == 4, "atomic CMP_SWAP has ds=4");
    check(MLX_BE32(ctrl->qpn_ds) == ((0x123456u << 8) | 4u),
          "atomic qpn_ds includes result data segment");
    check(MLX_BE32(ctrl->opmod_idx_opcode) ==
          ((0xbeefu << 8) | MLX_OPCODE_ATOMIC_CS), "atomic CS opcode 0x11");
    check(MLX_BE64(raddr->raddr) == 0x8877665544332211ULL &&
          MLX_BE32(raddr->rkey) == 0xdeadbeef, "atomic raddr encoded");
    check(MLX_BE64(atom->swap_add) == 0x1112131415161718ULL &&
          MLX_BE64(atom->compare) == 0x0102030405060708ULL,
          "atomic swap_add and compare encoded");

    /* Atomic FETCH_ADD uses opcode 0x12 and skips compare. */
    memset(wqe, 0, sizeof(wqe));
    ds = mlxEncodeRcAtomicWqe(wqe, sizeof(wqe), 5, 3, MLX_OPCODE_ATOMIC_FA,
                              0x1000, 0x2000, 0, 0x42, 0x4000, 0x5000, false);
    check(ds == 4 &&
          MLX_BE32(((MlxWqeCtrlSeg *)wqe)->opmod_idx_opcode) ==
          ((3u << 8) | MLX_OPCODE_ATOMIC_FA), "atomic FA opcode 0x12");
    check(MLX_BE64(((MlxWqeAtomicSeg *)(wqe + 32))->swap_add) == 0x42,
          "atomic FA addend encoded");
    check(((MlxWqeCtrlSeg *)wqe)->fm_ce_se == 0, "atomic FA unsignaled");

    /* Atomic validation: bad opcode / null remote refused. */
    ds = mlxEncodeRcAtomicWqe(wqe, sizeof(wqe), 5, 3, MLX_OPCODE_SEND,
                              0x1000, 0x2000, 0, 0x42, 0x4000, 0x5000, true);
    check(ds == 0, "atomic bad opcode refused");
    ds = mlxEncodeRcAtomicWqe(wqe, sizeof(wqe), 5, 3, MLX_OPCODE_ATOMIC_FA,
                              0, 0x2000, 0, 0x42, 0x4000, 0x5000, true);
    check(ds == 0, "atomic null remote addr refused");

    /* SL / VLAN priority threading into the RoCE primary path. */
    uint8_t qpc[MLX_QPC_BYTES] = {};
    MlxRocePathFields path = {};
    path.sgidIndex = 1;
    path.hopLimit = 64;
    path.portNum = 1;
    path.sl = 3;
    bool ok = mlxEncodeRocePrimaryPath(qpc, sizeof(qpc), &path);
    check(ok && mlxGetBits(qpc, MLX_QPC_PRIMARY_PATH_BIT_OFFSET + 0x121, 3) == 3,
          "RoCE primary path VLAN priority (SL) encoded");
}

int main(void)
{
    test_bits();
    test_rst2init();
    test_roce_path();
    test_qp_transitions();
    test_wqe_encoding();
    test_create_mkey();
    test_create_mkey_fragmented();
    test_mtt();
    test_manage_pages_take();
    test_general_caps_sw_owner();
    test_inline_atomic_wqe();
    if (failures) { printf("\n%d test(s) FAILED\n", failures); return 1; }
    printf("\nALL portable IFC tests passed\n");
    return 0;
}
