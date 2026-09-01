/*
 * MlxRegs.hpp — Hardware register layouts (generic Mellanox mlx5 family)
 *
 * Ported from: kernel_src/mlnx-ofed-kernel-5.9/include/linux/mlx5/device.h
 * These layouts are hard-imposed by hardware, consistent across
 * ConnectX-4 ~ ConnectX-8, and must not be changed.
 *
 * DriverKit port: no libkern dependency (pure stdint/stdbool).
 */
#ifndef MLX_REGS_HPP
#define MLX_REGS_HPP

#include <stdint.h>
#include <stdbool.h>

/*
 * Init Segment — BAR0 base address
 * See: device.h:568 struct mlx5_init_seg
 */
struct MlxInitSeg {
    uint32_t fw_rev;               /* +0x0000 firmware version */
    uint32_t cmdif_rev_fw_sub;     /* +0x0004 command interface version */
    uint32_t rsvd0[2];             /* +0x0008 */
    uint32_t cmdq_addr_h;          /* +0x0010 command queue DMA high 32 bits */
    uint32_t cmdq_addr_l_sz;       /* +0x0014 low 12 bits: log_sz/log_stride */
    uint32_t cmd_dbell;            /* +0x0018 command doorbell */
    uint32_t rsvd1[120];           /* +0x001C */
    uint32_t initializing;         /* +0x01FC bit31 = firmware initializing */
    struct MlxHealthBuffer {
        uint32_t assert_var[6];
        uint32_t rsvd0[2];
        uint32_t assert_exit_ptr;
        uint32_t assert_callra;
        uint32_t rsvd1;
        uint32_t time;
        uint32_t fw_ver;
        uint32_t hw_id;
        uint8_t rfr_severity;
        uint8_t rsvd2[3];
        uint8_t irisc_index;
        uint8_t synd;
        uint16_t ext_synd;
    } health;
    uint32_t rsvd2[878];
    uint32_t cmd_exec_to;          /* command execution timeout */
    uint32_t cmd_q_init_to;
    uint32_t internal_timer_h;
    uint32_t internal_timer_l;
    uint32_t rsvd3[2];
    uint32_t health_counter;
};

/* Command interface version (See CMD_IF_REV) */
#define MLX_CMD_IF_REV          5

/* Command descriptor ownership bit */
#define MLX_CMD_OWNER_HW        (1u << 0)

/* Command descriptor size (bytes) */
#define MLX_COMMAND_DESCRIPTOR_SIZE 64

/*
 * Register doorbell offsets (relative to the UAR page)
 * See: include/linux/mlx5/doorbell.h
 */
#define MLX_BF_OFFSET           0x800   /* BF register (user-space doorbell) */
#define MLX_CQ_DOORBELL         0x20    /* CQ doorbell */
#define MLX_EQ_DOORBELL         0x40    /* EQ doorbell */

/* CQ arm doorbell request mode (include/linux/mlx5/cq.h).
 * dbrec[1] stores consumer_index in bits 0..23, request mode in bit 24,
 * and arm_sn in bits 28..29. */
#define MLX_CQ_DB_REQ_NOT       (0u << 24)  /* arm on next CQE */
#define MLX_CQ_DB_REQ_NOT_SOL   (1u << 24)  /* arm on next solicited CQE */

/*
 * Command opcodes — independent of hardware version, shared across the whole family
 * See: mlx5_ifc.h:115-247
 */
enum {
    MLX_CMD_OP_QUERY_HCA_CAP          = 0x100,
    MLX_CMD_OP_SET_HCA_CAP            = 0x109,
    MLX_CMD_OP_ENABLE_HCA             = 0x104,
    MLX_CMD_OP_DISABLE_HCA            = 0x105,
    MLX_CMD_OP_QUERY_ISSI             = 0x10A,
    MLX_CMD_OP_SET_ISSI               = 0x10B,
    MLX_CMD_OP_INIT_HCA               = 0x102,
    MLX_CMD_OP_TEARDOWN_HCA           = 0x103,
    MLX_CMD_OP_MANAGE_PAGES           = 0x108,
    MLX_CMD_OP_QUERY_PAGES            = 0x107,
    MLX_CMD_OP_ALLOC_UAR              = 0x802,
    MLX_CMD_OP_FREE_UAR               = 0x803,
    MLX_CMD_OP_ACCESS_REG             = 0x805,
    MLX_CMD_OP_CREATE_MKEY            = 0x200,
    MLX_CMD_OP_QUERY_MKEY             = 0x201,
    MLX_CMD_OP_DESTROY_MKEY           = 0x202,
    MLX_CMD_OP_QUERY_SPECIAL_CONTEXTS = 0x203,
    MLX_CMD_OP_CREATE_EQ              = 0x301,
    MLX_CMD_OP_DESTROY_EQ             = 0x302,
    MLX_CMD_OP_QUERY_EQ               = 0x303,
    MLX_CMD_OP_CREATE_CQ              = 0x400,
    MLX_CMD_OP_DESTROY_CQ             = 0x401,
    MLX_CMD_OP_QUERY_CQ               = 0x402,
    MLX_CMD_OP_CREATE_QP              = 0x500,
    MLX_CMD_OP_DESTROY_QP             = 0x501,
    MLX_CMD_OP_QUERY_QP               = 0x50B,
    MLX_CMD_OP_RST2INIT_QP            = 0x502,
    MLX_CMD_OP_INIT2RTR_QP            = 0x503,
    MLX_CMD_OP_RTR2RTS_QP             = 0x504,
    MLX_CMD_OP_RTS2RTS_QP             = 0x505,
    MLX_CMD_OP_SQERR2RTS_QP           = 0x506,
    MLX_CMD_OP_2RST_QP                = 0x50A,
    MLX_CMD_OP_2ERR_QP                = 0x507,
    MLX_CMD_OP_SET_ROCE_ADDRESS       = 0x761,
    MLX_CMD_OP_QUERY_ROCE_ADDRESS     = 0x760,
    MLX_CMD_OP_QUERY_VPORT_STATE      = 0x750,
    MLX_CMD_OP_QUERY_NIC_VPORT_CONTEXT = 0x754,
    MLX_CMD_OP_MODIFY_NIC_VPORT_CONTEXT = 0x755,
    MLX_CMD_OP_MODIFY_CONG_PARAMS     = 0x825,
    MLX_CMD_OP_QUERY_CONG_PARAMS      = 0x824,
    MLX_CMD_OP_QUERY_CONG_STATUS      = 0x822,
    MLX_CMD_OP_QUERY_CONG_STATISTICS  = 0x826,
    MLX_CMD_OP_ALLOC_PD                = 0x800,
    MLX_CMD_OP_DEALLOC_PD              = 0x801,
    MLX_CMD_OP_ALLOC_XRCD              = 0x80e,
    MLX_CMD_OP_DEALLOC_XRCD            = 0x80f,
    MLX_CMD_OP_CREATE_SRQ              = 0x700,
    MLX_CMD_OP_DESTROY_SRQ             = 0x701,
    MLX_CMD_OP_CREATE_RMP              = 0x90c,
    MLX_CMD_OP_DESTROY_RMP             = 0x90e,
    MLX_CMD_OP_ALLOC_TRANSPORT_DOMAIN  = 0x816,
    MLX_CMD_OP_DEALLOC_TRANSPORT_DOMAIN = 0x817,
    MLX_CMD_OP_CREATE_TIR              = 0x900,
    MLX_CMD_OP_DESTROY_TIR             = 0x902,
    MLX_CMD_OP_CREATE_SQ               = 0x904,
    MLX_CMD_OP_MODIFY_SQ               = 0x905,
    MLX_CMD_OP_DESTROY_SQ              = 0x906,
    MLX_CMD_OP_CREATE_RQ               = 0x908,
    MLX_CMD_OP_MODIFY_RQ               = 0x909,
    MLX_CMD_OP_DESTROY_RQ              = 0x90a,
    MLX_CMD_OP_CREATE_TIS              = 0x912,
    MLX_CMD_OP_DESTROY_TIS             = 0x914,
    MLX_CMD_OP_CREATE_RQT              = 0x916,
    MLX_CMD_OP_DESTROY_RQT             = 0x918,
    MLX_CMD_OP_SET_FLOW_TABLE_ROOT      = 0x92f,
    MLX_CMD_OP_CREATE_FLOW_TABLE        = 0x930,
    MLX_CMD_OP_DESTROY_FLOW_TABLE       = 0x931,
    MLX_CMD_OP_CREATE_FLOW_GROUP        = 0x933,
    MLX_CMD_OP_DESTROY_FLOW_GROUP       = 0x934,
    MLX_CMD_OP_SET_FLOW_TABLE_ENTRY     = 0x936,
    MLX_CMD_OP_DELETE_FLOW_TABLE_ENTRY  = 0x938,
};

/* RoCE versions (See device.h:402) */
enum {
    MLX_ROCE_VERSION_1 = 0,      /* RoCE v1: EtherType 0x8915 */
    MLX_ROCE_VERSION_2 = 2,      /* RoCE v2: UDP 4791 */
};

/* RoCE v2 ports (See drivers/infiniband/core/lag.c:39) */
#define MLX_ROCE_V2_UDP_DPORT    4791
#define MLX_ROCE_V2_CNP_DPORT    4792   /* CNP = dport + 1 */

/*
 * EQ event types (See device.h:354)
 */
enum {
    MLX_EVENT_TYPE_COMPLETION     = 0x00,
    MLX_EVENT_TYPE_PATH_MIG       = 0x01,
    MLX_EVENT_TYPE_COMM_EST       = 0x02,
    MLX_EVENT_TYPE_SQ_DRAINED     = 0x03,
    MLX_EVENT_TYPE_WQ_CATAS_ERROR = 0x05,
    MLX_EVENT_TYPE_CMD            = 0x0a,   /* command completion */
    MLX_EVENT_TYPE_PAGE_REQUEST   = 0x0b,   /* firmware requests pages */
    MLX_EVENT_TYPE_SRQ_LAST_WQE   = 0x13,
    MLX_EVENT_TYPE_SRQ_RQ_LIMIT   = 0x14,
    MLX_EVENT_TYPE_NIC_VPORT_CHANGE = 0x0d,
    /* port/device-level events (See Linux device.h:354 mlx5_event) */
    MLX_EVENT_TYPE_DEVICE_FATAL      = 0x08,
    MLX_EVENT_TYPE_PORT_STATE_CHANGE = 0x09,
};

/* Transport type st field (QPC) — mlx5_ifc.h: MLX5_QPC_ST_* */
enum {
    MLX_QP_ST_RC   = 0x0,
    MLX_QP_ST_UC   = 0x1,
    MLX_QP_ST_UD   = 0x2,
    MLX_QP_ST_XRC  = 0x3,
};

/* QP states (See qp.c:858 to_mlx5_state) */
enum {
    MLX_QP_STATE_RST   = 0,
    MLX_QP_STATE_INIT  = 1,
    MLX_QP_STATE_RTR   = 2,
    MLX_QP_STATE_RTS   = 3,
    MLX_QP_STATE_SQER  = 4,
    MLX_QP_STATE_SQD   = 5,
    MLX_QP_STATE_ERR   = 6,
};

#endif /* MLX_REGS_HPP */