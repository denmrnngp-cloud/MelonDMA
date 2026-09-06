/*
 * librdma_shim.h — userspace RDMA shim over the MlxRDMA DriverKit DEXT.
 *
 * Provides an ibv_*-shaped subset so llama.cpp's transport.cpp can connect
 * without rewriting its verbs calls (notes/09 §2.3: chunks 256 KiB,
 * RX depth 24, 1 SGE). The control plane goes through IOConnectCallMethod
 * → MlxUserClient::ExternalMethod (selector table in MlxUCIO.h). The CQ
 * The DEXT owns and validates WQ/CQ resources.  Trusted clients map isolated
 * queue/UAR pages and use direct SQ/RQ/CQ by default; unsupported operations
 * fall back to bounded, validated ExternalMethods.
 *
 * This is a host (macOS userspace) library — compiles against the macOS SDK,
 * not DriverKit. It needs the com.apple.developer.driverkit.userclient-access
 * entitlement in the host process once the DEXT is approved.
 */
#ifndef LIBRDMA_SHIM_H
#define LIBRDMA_SHIM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- opaque handles ---- */
typedef struct rdma_device    rdma_device;
typedef struct rdma_pd        rdma_pd;
typedef struct rdma_cq        rdma_cq;
typedef struct rdma_qp        rdma_qp;
typedef struct rdma_mr        rdma_mr;
typedef struct rdma_mw        rdma_mw;
typedef struct rdma_ah        rdma_ah;

struct rdma_fast_path {
    uint32_t version;
    uint32_t uar_page_size;
    uint32_t db_page_size;
    void    *uar;
    void    *db_record;
};

struct rdma_fast_path_stats {
    uint64_t mapped_qps;
    uint64_t direct_send_batches;
    uint64_t direct_send_wrs;
    uint64_t direct_doorbells;
    uint64_t direct_recv_batches;
    uint64_t direct_recv_wrs;
    uint64_t direct_cq_consumers;
    uint64_t fallback_send_batches;
    uint64_t fallback_recv_batches;
    uint64_t shadow_publications;
    uint64_t blue_flame_wqes;
    uint64_t direct_poll_calls;
    uint64_t direct_poll_empty;
    uint64_t direct_cqes;
    uint64_t direct_cqe_errors;
    uint64_t kernel_poll_calls;
    uint64_t kernel_cqes;
    uint64_t fallback_direct_disabled;
    uint64_t fallback_cq_unmapped;
    uint64_t fallback_unknown_qp;
    uint64_t fallback_missing_metadata;
    uint64_t fallback_dext_owned;
};

/* ---- device enumeration / open ---- */
/* Open the first matching DEXT (MlxPCIDriver matching 15b3:1015). */
rdma_device *rdma_open_device(void);
/* Open the named device returned by rdma_list_devices(). */
rdma_device *rdma_open_device_by_name(const char *name);
void         rdma_close_device(rdma_device *dev);

/* Per-client UAR/DB mappings. Direct SQ/RQ/CQ is capability-driven and on by
 * default; MELONDMA_DIRECT_UAR=0 or MELONDMA_DIRECT_CQ=0 is the diagnostic
 * opt-out. Publication uses the isolated per-client bundle. */
int  rdma_enable_fast_path(rdma_device *dev, struct rdma_fast_path *path);
int  rdma_map_fast_path(rdma_device *dev, struct rdma_fast_path *path);
void rdma_unmap_fast_path(rdma_device *dev);
int  rdma_fast_path_get_stats(const rdma_device *dev,
                              struct rdma_fast_path_stats *stats);
int  rdma_qp_direct_enabled(const rdma_qp *qp);
int  rdma_qp_trusted_fast_path(const rdma_qp *qp);

/* ibv_devinfo-style: list device names (caller frees with rdma_free_names). */
int  rdma_list_devices(char ***names, int *count);
void rdma_free_names(char **names, int count);

/* ---- device/port query ---- */
#define RDMA_UC_ABI_VERSION 2u
enum {
    RDMA_FEATURE_RC = 1u << 0,
    RDMA_FEATURE_ROCE_V2 = 1u << 1,
    RDMA_FEATURE_DIRECT_PATH = 1u << 2,
    RDMA_FEATURE_ASYNC_EVENTS = 1u << 3,
    RDMA_FEATURE_INDIRECT_MR = 1u << 4,
    RDMA_FEATURE_QP_RECOVERY = 1u << 5,
    RDMA_FEATURE_MULTI_SGE = 1u << 6,
    RDMA_FEATURE_IMMEDIATE_DATA = 1u << 7,
    RDMA_FEATURE_HEALTH_QUERY = 1u << 8,
    RDMA_FEATURE_INLINE       = 1u << 10,
    RDMA_FEATURE_ATOMIC       = 1u << 11,
    RDMA_FEATURE_TRUSTED_FAST_PATH = 1u << 12,
    RDMA_FEATURE_BLUE_FLAME = 1u << 13,
    RDMA_FEATURE_CQ_INTERRUPT = 1u << 14,
    RDMA_FEATURE_CQ_EVENT_WAIT = 1u << 16,
    RDMA_FEATURE_RUNTIME_STATUS = 1u << 17,
    RDMA_FEATURE_COHERENT_UMA_MR = 1u << 15,
};
struct rdma_abi_attr { uint32_t version; uint32_t features; };
int  rdma_query_abi(rdma_device *dev, struct rdma_abi_attr *attr);
int  rdma_wait_cq_event(rdma_device *dev, uint64_t *generation,
                        uint32_t timeout_ms);

struct rdma_device_attr {
    uint64_t fw_version;
    uint32_t device_id;
    uint32_t num_ports;
    uint32_t max_qp;
    uint32_t max_cq;
    uint32_t max_mr;
    uint16_t roce_versions;     /* bit0=RoCEv1 bit1=RoCEv2 */
    uint16_t max_gid;
    uint32_t max_msg_size;
    uint32_t max_inline_data;
    uint32_t max_qp_rd_atom;
    uint32_t max_qp_init_rd_atom;
};
int  rdma_query_device(rdma_device *dev, struct rdma_device_attr *attr);

struct rdma_port_attr {
    uint8_t  link_layer;        /* 1=IB 2=Ethernet */
    uint8_t  port_state;        /* 0=down 1=up */
    uint8_t  gid_type;          /* 2=RoCEv2 */
    uint32_t active_speed_mbps;
    uint32_t max_mtu;
    uint16_t gid_tbl_len;
    uint16_t pkey_tbl_len;
};
int  rdma_query_port(rdma_device *dev, struct rdma_port_attr *attr);

struct rdma_health_attr {
    uint32_t healthy;
    uint32_t syndrome;
    uint32_t ext_syndrome;
    uint32_t owned_pd;
    uint32_t owned_qp;
    uint32_t owned_cq;
    uint32_t owned_mr;
    uint32_t owned_ah;
};
int  rdma_query_health(rdma_device *dev, struct rdma_health_attr *attr);

/* MSI-X bring-up diagnosis. setup_stage is RDMA_IRQ_STAGE_*; NONE with
 * completion_ready set means the blocking completion path is live. Returns
 * -ENOTSUP on a DEXT that predates the query. */
enum {
    RDMA_IRQ_STAGE_NONE          = 0,
    RDMA_IRQ_STAGE_CONFIGURE     = 1,
    RDMA_IRQ_STAGE_QUEUE         = 2,
    RDMA_IRQ_STAGE_SOURCE        = 3,
    RDMA_IRQ_STAGE_ACTION        = 4,
    RDMA_IRQ_STAGE_HANDLER       = 5,
    RDMA_IRQ_STAGE_ENABLE        = 6,
    RDMA_IRQ_STAGE_NOT_ATTEMPTED = 7,
};
enum {
    RDMA_CQEQ_STAGE_NOT_ATTEMPTED = 0,
    RDMA_CQEQ_STAGE_ALLOC         = 1,
    RDMA_CQEQ_STAGE_INIT          = 2,
    RDMA_CQEQ_STAGE_CREATE        = 3,
    RDMA_CQEQ_STAGE_OK            = 4,
};
struct rdma_interrupt_attr {
    uint32_t vectors;
    uint32_t setup_status;
    uint32_t setup_stage;
    uint32_t async_eqn;
    uint32_t completion_eqn;
    uint32_t completion_ready;
    uint64_t completion_events;
    uint32_t completion_eq_status;
    uint32_t completion_eq_stage;
    uint32_t completion_eq_syndrome;
    uint32_t completion_eq_fw_status;
    uint32_t completion_eq_variant;
    uint32_t completion_eq_variant_tried;
    uint32_t completion_eq_variant_syndrome[4];
    uint64_t async_interrupts;
    uint64_t completion_interrupts;
    uint64_t eq_timer_ticks;
    uint32_t eq_timer_period_ms;
};
int  rdma_query_interrupts(rdma_device *dev, struct rdma_interrupt_attr *attr);

/* Hardware completion moderation. period is microseconds (0..4095), max_count
 * is CQEs (0..65535); zero in a field disables that half. Both zero restores
 * unmoderated behaviour. Returns -ENOTSUP on a DEXT that predates it. */
int  rdma_modify_cq_moderation(rdma_cq *cq, uint32_t period, uint32_t max_count);

/* Diagnostic: rebind the completion EQ to MSI-X index `intr` (0 or 1) and
 * return its new EQ number. Refused with -EBUSY while any CQ is live. */
int  rdma_probe_completion_vector(rdma_device *dev, uint32_t intr, uint32_t *eqn);

struct rdma_perf {
    uint64_t external_methods;
    uint64_t external_method_ns;
    uint64_t post_send_calls;
    uint64_t post_recv_calls;
    uint64_t poll_cq_calls;
    uint64_t sync_fast_path_calls;
    uint64_t sync_qp_tails_calls;
    uint64_t arm_cq_calls;
    uint64_t doorbells;
    uint64_t cqe_consumed;
    uint64_t cqe_errors;
    uint64_t mr_registers;
    uint64_t mr_deregisters;
    uint64_t mr_bytes;
    uint64_t copied_bytes;
    /* Device-wide completion MSI-X interrupts the DEXT serviced, and this
     * client's WaitCqEvent calls that returned a fresh generation. */
    uint64_t cq_events;
    uint64_t cq_event_wakeups;
};
int  rdma_query_perf(rdma_device *dev, struct rdma_perf *perf);
struct rdma_runtime_status {
    uint32_t version, size;
    uint64_t device_epoch;
    uint64_t pinned_bytes, peak_pinned_bytes, pin_failures;
    uint64_t client_pinned_bytes, client_pinned_limit, device_pinned_limit;
    uint64_t quarantine_bytes, quarantine_objects;
    uint64_t irq_completion_eqes, timer_completion_eqes, last_completion_irq_ns;
    uint32_t quarantined, bme_fenced, completion_eq_ready, completion_irq_proven;
};
int rdma_query_runtime(rdma_device *dev, struct rdma_runtime_status *status);

/* DCQCN reaction-point parameters (QUERY/MODIFY_CONG_PARAMS 0x824/0x825). */
struct rdma_cong_params {
    uint32_t rpg_min_dec_fac;
    uint32_t rpg_ai_rate;
    uint32_t rpg_time_reset;
    uint32_t rpg_threshold;
    uint32_t rpg_hai;
    uint32_t rpg_gd;
    uint32_t rpg_time_inc;
    uint32_t rsvd;
};
int  rdma_query_cong(rdma_device *dev, struct rdma_cong_params *params);
int  rdma_modify_cong(rdma_device *dev, const struct rdma_cong_params *params);

/* ---- protection domain ---- */
rdma_pd *rdma_alloc_pd(rdma_device *dev);
int      rdma_dealloc_pd(rdma_pd *pd);

/* ---- completion queue ---- */
rdma_cq *rdma_create_cq(rdma_device *dev, uint32_t cqe_depth);
int      rdma_destroy_cq(rdma_cq *cq);
int      rdma_query_cq_completions(rdma_cq *cq, uint64_t *completions);
/* Fast userspace read of the mapped CQE ring (no kernel call).  Returns the
 * number of pending completions in the ring; -EAGAIN when the ring is not
 * mapped (caller must fall back to rdma_query_cq_completions). */
int      rdma_cq_pending_local(rdma_cq *cq, uint64_t *count);


/* work completion (ibv_wc-shaped) */
enum rdma_wc_status {
    RDMA_WC_SUCCESS   = 0,
    RDMA_WC_LOC_LEN   = 1,
    RDMA_WC_LOC_EEC   = 2,
    RDMA_WC_LOC_QP_OP = 3,
    RDMA_WC_WR_FLUSH  = 4,
    RDMA_WC_MW_BIND   = 5,
    RDMA_WC_BAD_RESP  = 6,
    RDMA_WC_REM_ACCESS= 7,
    RDMA_WC_REM_OP    = 8,
    RDMA_WC_RETRY_EXC = 9,
    RDMA_WC_RNR_RETRY = 10,
    RDMA_WC_REM_ABORT = 11,
    RDMA_WC_GENERAL   = 12,
};
enum rdma_wc_opcode {
    RDMA_WC_SEND        = 0,
    RDMA_WC_RDMA_WRITE  = 1,
    RDMA_WC_RDMA_READ   = 2,
    RDMA_WC_RECV        = 3,
    RDMA_WC_UMR_KLM     = 4,
    RDMA_WC_FETCH_ADD   = 5,
    RDMA_WC_COMP_SWAP   = 6,
};
#define RDMA_WC_WITH_IMM 1u
#define RDMA_WC_WITH_ATOMIC 2u
struct rdma_wc {
    uint64_t wr_id;
    uint32_t status;       /* rdma_wc_status */
    uint32_t opcode;       /* rdma_wc_opcode */
    uint32_t byte_len;
    uint32_t qp_num;
    uint32_t imm_data;
    uint32_t wc_flags;
    uint32_t wqe_counter;
    uint32_t vendor_err;
    uint64_t atomic_result; /* RDMA_WC_WITH_ATOMIC: pre-op remote word */
};

/* poll_cq returns at most 16 completions.  A mapped trusted CQ is consumed
 * directly; unsupported or explicitly disabled cases use the kernel method. */
int  rdma_poll_cq(rdma_cq *cq, struct rdma_wc *wc, int num);


/* ---- queue pair (RC only for v1) ---- */
enum rdma_qp_type { RDMA_QPT_RC = 0, RDMA_QPT_UD = 1 };
enum rdma_qp_state {
    RDMA_QPS_RESET = 0, RDMA_QPS_INIT = 1, RDMA_QPS_RTR = 2,
    RDMA_QPS_RTS   = 3, RDMA_QPS_SQD  = 4, RDMA_QPS_SQERR=5, RDMA_QPS_ERR = 6,
};
enum rdma_access {
    RDMA_ACCESS_LOCAL_WRITE  = 1 << 0,
    RDMA_ACCESS_REMOTE_WRITE = 1 << 1,
    RDMA_ACCESS_REMOTE_READ  = 1 << 2,
    RDMA_ACCESS_REMOTE_ATOMIC= 1 << 3,
};

struct rdma_qp_init_attr {
    rdma_cq *send_cq;
    rdma_cq *recv_cq;
    uint32_t qp_type;       /* rdma_qp_type */
    uint32_t cap_sq;       /* power of 2 */
    uint32_t cap_rq;       /* power of 2 */
    void    *sq_buf;       /* reserved; Phase 2 WQs are DEXT-owned */
    void    *rq_buf;       /* reserved; Phase 2 WQs are DEXT-owned */
    uint32_t max_inline_data; /* must be zero */
};
rdma_qp *rdma_create_qp(rdma_pd *pd, const struct rdma_qp_init_attr *init);
int      rdma_destroy_qp(rdma_qp *qp);

/* QP modify (RST→INIT→RTR→RTS). attr_mask compatible with ibv_qp_attr_mask. */
struct rdma_qp_attr {
    uint32_t cur_state;       /* rdma_qp_state */
    uint32_t new_state;
    uint32_t attr_mask;
    uint32_t dest_qpn;
    uint32_t path_mtu;        /* 1=256, 2=512, 3=1K, 4=2K, 5=4K */
    uint32_t rq_psn;
    uint32_t sq_psn;
    uint32_t pkey_index;
    uint32_t port_num;
    /* remote path (for RTR) */
    uint8_t  ah_dmac[6];
    uint8_t  ah_dgid[16];
    uint32_t ah_sgid_index;
    uint8_t  ah_hop_limit;
    uint8_t  ah_traffic_class;
    uint16_t ah_udp_sport;
    uint32_t min_rnr_timer;
    uint32_t max_dest_rd_atomic;
    uint32_t max_rd_atomic;
    uint32_t timeout;          /* local ACK timeout, 0..31 */
    uint32_t retry_cnt;        /* transport retry count, 0..7 */
    uint32_t rnr_retry;        /* receiver-not-ready retry count, 0..7 */
    uint32_t sl;               /* RoCEv2 VLAN priority / service level 0..7 */
};
int  rdma_modify_qp(rdma_qp *qp, const struct rdma_qp_attr *attr);
uint32_t rdma_qp_number(const rdma_qp *qp);
int  rdma_query_qp(rdma_qp *qp, uint32_t *state);

/* Program and strictly read back a RoCE address table entry. */
int  rdma_set_roce_address(rdma_device *dev, const uint8_t gid[16],
                           const uint8_t mac[6], uint8_t l3_type,
                           uint32_t *gid_index);
int  rdma_set_roce_address_vlan(rdma_device *dev, const uint8_t gid[16],
                                const uint8_t mac[6], uint8_t l3_type,
                                uint16_t vlan_id, uint8_t vlan_valid,
                                uint32_t *gid_index);
int  rdma_clear_roce_address(rdma_device *dev, uint32_t gid_index);
struct rdma_gid_attr {
    uint8_t gid[16];
    uint8_t mac[6];
    uint8_t l3_type;
    uint8_t vlan_valid;
    uint16_t vlan_id;
    uint8_t gid_type;         /* ibv_gid_type: 2=RoCEv2 */
    uint8_t ifindex;          /* 0: no macOS netif */
};
int  rdma_query_gid(rdma_device *dev, uint32_t gid_index,
                    struct rdma_gid_attr *attr);

/* ---- memory registration ---- */
struct rdma_mr_attr_resp { uint32_t mr_handle; uint32_t lkey; uint32_t rkey; };
struct rdma_mr *rdma_reg_mr(rdma_pd *pd, void *addr, uint64_t length,
                             uint32_t access_flags,
                             struct rdma_mr_attr_resp *out);
/* Composes up to RDMA_MAX_INDIRECT_MR_CHILDREN already-registered MRs
 * (from rdma_reg_mr, same pd) under one new lkey/rkey, without a bigger
 * single MR — see notes/43/44. `addr`/`length` are the logical span this
 * new MR presents to callers (normally the union of the children). */
#define RDMA_MAX_INDIRECT_MR_CHILDREN 240
/* Chunk boundary for the *fallback* indirect composition in verbs_compat.c.
 * The DEXT itself now coalesces a contiguous IOVA onto coarse MTT pages
 * (2 MiB), so a single rdma_reg_mr handles large buffers directly; this
 * 480 x 4 KiB bound only still matters when the IOVA is not contiguous. */
#define RDMA_MAX_DIRECT_MR_BYTES (480u * 4096u)
struct rdma_mr *rdma_reg_mr_indirect(rdma_pd *pd, struct rdma_mr *const *children,
                                     uint32_t child_count, uint64_t addr,
                                     uint64_t length, uint32_t access_flags,
                                     struct rdma_mr_attr_resp *out);
/* An indirect MR from rdma_reg_mr_indirect() is NOT usable in a WQE SGE
 * until this is called: hardware leaves a freshly CREATE_MKEY'd KLM mkey
 * in a "free"/unbacked state, unusable for real DMA until a UMR WQE clears
 * it (see notes/48 — LOCAL_PROT_ERR otherwise, on the first real touch).
 * Posts that UMR on `qp` (which must already be in RTS) and blocks until
 * its own completion appears on `cq` (qp's send CQ). `children`/
 * `child_count` must be the exact same list already passed to
 * rdma_reg_mr_indirect() for `mr`. Returns 0 on success, -errno otherwise;
 * safe to call exactly once per indirect MR, before its first use. */
int  rdma_activate_indirect_mr(rdma_qp *qp, rdma_cq *cq, struct rdma_mr *mr,
                               struct rdma_mr *const *children,
                               uint32_t child_count);
int  rdma_dereg_mr(struct rdma_mr *mr);
rdma_mw *rdma_alloc_mw(rdma_pd *pd, uint32_t type);
int      rdma_dealloc_mw(rdma_mw *mw);
int      rdma_bind_mw(rdma_qp *qp, rdma_mw *mw, rdma_mr *mr,
                      uint64_t addr, uint64_t length, uint32_t access_flags,
                      uint32_t send_flags, uint64_t wr_id, uint32_t *new_rkey);
uint32_t rdma_mw_rkey(const rdma_mw *mw);
uint32_t rdma_mr_lkey(const struct rdma_mr *mr);
uint32_t rdma_mr_rkey(const struct rdma_mr *mr);

/* ---- address handle (RoCE) ---- */
struct rdma_ah_attr {
    uint8_t  dmac[6];
    uint8_t  dgid[16];
    uint32_t sgid_index;
    uint8_t  hop_limit;
    uint8_t  traffic_class;
    uint16_t udp_sport;
    uint32_t port_num;
};
rdma_ah *rdma_create_ah(rdma_pd *pd, const struct rdma_ah_attr *attr);
int      rdma_destroy_ah(rdma_ah *ah);

/* ---- data path ---- */
struct rdma_sge { uint32_t lkey; uint64_t addr; uint32_t length; };
enum rdma_wr_opcode {
    RDMA_WR_SEND        = 0,
    RDMA_WR_RDMA_WRITE  = 1,
    RDMA_WR_RDMA_READ   = 2,
    RDMA_WR_SEND_IMM    = 4,
    RDMA_WR_RDMA_WRITE_IMM = 5,
    RDMA_WR_LOCAL_INV = 6,
    RDMA_WR_BIND_MW = 7,
    RDMA_WR_ATOMIC_CS = 8,
    RDMA_WR_ATOMIC_FA = 9,
};
struct rdma_send_wr {
    uint64_t wr_id;
    uint32_t opcode;        /* rdma_wr_opcode */
    uint32_t num_sge;       /* 1 for v1 (notes/09 §2.3) */
    const struct rdma_sge *sg_list;
    /* RDMA fields */
    uint64_t remote_addr;
    uint32_t rkey;
    uint32_t send_flags;    /* bit0: signaled completion */
    uint32_t imm_data;      /* network byte order; *_IMM only */
};
#define RDMA_SEND_SIGNALED  1u << 0
#define RDMA_SEND_FENCE     1u << 1
#define RDMA_SEND_SOLICITED 1u << 2
#define RDMA_SEND_INLINE    1u << 3
#define RDMA_MAX_INLINE_DATA 512u
#define RDMA_MAX_POST_BATCH 256u
#define RDMA_POST_CHUNK 64u
#define RDMA_MAX_POLL_BATCH 16u
#define RDMA_MAX_SGE 16u
/* RQ WQEBBs are 64 bytes, hence at most four 16-byte data segments. */
#define RDMA_MAX_RECV_SGE 4u

struct rdma_recv_wr {
    uint64_t wr_id;
    uint32_t num_sge;
    const struct rdma_sge *sg_list;
};

int  rdma_post_send(rdma_qp *qp, const struct rdma_send_wr *wr);
int  rdma_post_send_sge(rdma_qp *qp, const struct rdma_send_wr *wr);
int  rdma_post_local_inv(rdma_qp *qp, uint64_t wr_id, uint32_t rkey);
int  rdma_post_recv_sge(rdma_qp *qp, const struct rdma_recv_wr *wr);
int  rdma_post_recv(rdma_qp *qp, uint64_t wr_id, const struct rdma_sge *sg_list,
                    uint32_t num_sge);
/* Phase 3: at most 16 WRs cross in one DriverKit call. Direct SEND/WRITE/READ
 * may publish SQ WQEs through the per-client UAR; RQ and CQ consumer updates
 * remain kernel-mediated. Callers should mark the last WR in a bounded run
 * RDMA_SEND_SIGNALED so earlier unsignaled WQEs are reclaimed. */
int  rdma_post_send_batch(rdma_qp *qp, const struct rdma_send_wr *wr,
                          uint32_t count);
int  rdma_post_recv_batch(rdma_qp *qp, const struct rdma_recv_wr *wr,
                          uint32_t count);

/* P3: inline SEND (payload copied into the request) and RC atomics. */
int  rdma_post_send_inline(rdma_qp *qp, uint64_t wr_id, uint32_t opcode,
                           const void *data, uint32_t len,
                           uint32_t imm_data, uint32_t send_flags);
int  rdma_post_send_atomic(rdma_qp *qp, uint64_t wr_id, uint32_t opcode,
                           uint64_t remote_addr, uint32_t rkey,
                           uint64_t compare, uint64_t swap_add,
                           uint64_t result_addr, uint32_t result_lkey,
                           uint32_t send_flags);

/* P3: full GID-table enumeration and hardware CQ arming. */
struct rdma_gid_table_entry {
    uint32_t index;
    uint8_t  gid[16];
    uint8_t  mac[6];
    uint8_t  roce_version;
    uint8_t  l3_type;
    uint8_t  gid_type;
    uint8_t  vlan_valid;
    uint16_t vlan_id;
    uint32_t ifindex;
};
#define RDMA_MAX_GID_TABLE 256u
int  rdma_query_gid_table(rdma_device *dev,
                          struct rdma_gid_table_entry *entries,
                          uint32_t capacity, uint32_t *count,
                          uint32_t *table_size);
int  rdma_arm_cq(rdma_cq *cq, int solicited_only);

/* Advance a QP's completion tails after a userspace direct-CQ decode. */
int  rdma_sync_qp_tails(rdma_qp *qp, uint64_t sq_tail, uint64_t rq_tail);

/* Kernel-mediated CQ consumer update (Option B doorbell fallback). */
int  rdma_update_cq_consumer(rdma_cq *cq, uint32_t consumer_index);

/* Async events (non-blocking; returns -EAGAIN when none). */
struct rdma_async_event {
    uint32_t event_type;
    uint32_t element_type;
    uint32_t element_handle;
};
int  rdma_get_async_event(rdma_device *dev, struct rdma_async_event *event);
/* Resolve an async-event token in the same device object that owns the
 * context. Returned pointers remain valid until the matching destroy call. */
rdma_cq *rdma_find_cq(rdma_device *dev, uint32_t token);
rdma_qp *rdma_find_qp(rdma_device *dev, uint32_t token);

#ifdef __cplusplus
}
#endif

#endif /* LIBRDMA_SHIM_H */
