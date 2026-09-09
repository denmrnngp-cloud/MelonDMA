/* Minimal rdma-core compatible RC verbs surface for MelonDMA on macOS. */
#ifndef MELONDMA_INFINIBAND_VERBS_H
#define MELONDMA_INFINIBAND_VERBS_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Values match rdma-core so consumer sources compile unchanged. */
enum ibv_qp_type { IBV_QPT_RC = 2, IBV_QPT_UC = 3, IBV_QPT_UD = 4 };
enum ibv_qp_state {
    IBV_QPS_RESET = 0, IBV_QPS_INIT = 1, IBV_QPS_RTR = 2,
    IBV_QPS_RTS = 3, IBV_QPS_SQD = 4, IBV_QPS_SQE = 5, IBV_QPS_ERR = 6
};
enum ibv_mtu {
    IBV_MTU_256 = 1, IBV_MTU_512 = 2, IBV_MTU_1024 = 3,
    IBV_MTU_2048 = 4, IBV_MTU_4096 = 5
};
enum ibv_port_state {
    IBV_PORT_NOP = 0, IBV_PORT_DOWN = 1, IBV_PORT_INIT = 2,
    IBV_PORT_ARMED = 3, IBV_PORT_ACTIVE = 4
};
enum ibv_link_layer {
    IBV_LINK_LAYER_UNSPECIFIED = 0,
    IBV_LINK_LAYER_INFINIBAND = 1,
    IBV_LINK_LAYER_ETHERNET = 2
};
enum ibv_gid_type {
    IBV_GID_TYPE_IB = 0,
    IBV_GID_TYPE_ROCE_V1 = 1,
    IBV_GID_TYPE_ROCE_V2 = 2
};
enum ibv_wr_opcode {
    IBV_WR_RDMA_WRITE = 0,
    IBV_WR_RDMA_WRITE_WITH_IMM = 1,
    IBV_WR_SEND = 2,
    IBV_WR_SEND_WITH_IMM = 3,
    IBV_WR_RDMA_READ = 4,
    IBV_WR_LOCAL_INV = 5,
    IBV_WR_ATOMIC_CMP_AND_SWP = 6,
    IBV_WR_ATOMIC_FETCH_AND_ADD = 7,
    IBV_WR_SEND_WITH_INV = 8
};
enum ibv_wc_status {
    IBV_WC_SUCCESS = 0,
    IBV_WC_LOC_LEN_ERR = 1,
    IBV_WC_LOC_QP_OP_ERR = 4,
    IBV_WC_WR_FLUSH_ERR = 5,
    IBV_WC_REM_ACCESS_ERR = 10,
    IBV_WC_RETRY_EXC_ERR = 12,
    IBV_WC_RNR_RETRY_EXC_ERR = 13,
    IBV_WC_GENERAL_ERR = 21
};
enum ibv_wc_opcode {
    IBV_WC_SEND = 0,
    IBV_WC_RDMA_WRITE = 1,
    IBV_WC_RDMA_READ = 2,
    IBV_WC_FETCH_ADD = 3,
    IBV_WC_COMP_SWAP = 4,
    IBV_WC_RECV = 1 << 7
};
/* IBV_WC_GRH marks a datagram receive whose buffer begins with 40 bytes of
 * global routing header; the payload starts after it. */
enum { IBV_WC_WITH_IMM = 1 << 0, IBV_WC_WITH_ATOMIC = 1 << 1,
       IBV_WC_GRH = 1 << 2, IBV_WC_WITH_INV = 1 << 3 };
#define IBV_GRH_BYTES 40

enum {
    IBV_ACCESS_LOCAL_WRITE = 1 << 0,
    IBV_ACCESS_REMOTE_WRITE = 1 << 1,
    IBV_ACCESS_REMOTE_READ = 1 << 2,
    IBV_ACCESS_REMOTE_ATOMIC = 1 << 3,
    IBV_ACCESS_MW_BIND = 1 << 4
};
enum {
    IBV_SEND_FENCE = 1 << 0,
    IBV_SEND_SIGNALED = 1 << 1,
    IBV_SEND_SOLICITED = 1 << 2,
    IBV_SEND_INLINE = 1 << 3
};
enum {
    IBV_QP_STATE = 1 << 0,
    IBV_QP_CUR_STATE = 1 << 1,
    IBV_QP_QKEY = 1 << 2,       /* datagram only */
    IBV_QP_ACCESS_FLAGS = 1 << 3,
    IBV_QP_PKEY_INDEX = 1 << 4,
    IBV_QP_PORT = 1 << 5,
    IBV_QP_AV = 1 << 7,
    IBV_QP_PATH_MTU = 1 << 8,
    IBV_QP_TIMEOUT = 1 << 9,
    IBV_QP_RETRY_CNT = 1 << 10,
    IBV_QP_RNR_RETRY = 1 << 11,
    IBV_QP_RQ_PSN = 1 << 12,
    IBV_QP_MAX_QP_RD_ATOMIC = 1 << 13,
    IBV_QP_MIN_RNR_TIMER = 1 << 15,
    IBV_QP_SQ_PSN = 1 << 16,
    IBV_QP_MAX_DEST_RD_ATOMIC = 1 << 17,
    IBV_QP_DEST_QPN = 1 << 20
};

union ibv_gid {
    uint8_t raw[16];
    struct { uint64_t subnet_prefix, interface_id; } global;
};

/* MelonDMA RoCEv2 provider extension. The DriverKit DEXT owns the PCI
 * function and therefore has no macOS netif/ARP attachment to query. A
 * caller supplies the resolved local GID/MAC and next-hop MAC explicitly.
 * Environment variables are retained only as a legacy compatibility path. */
struct ibv_mlx5_roce_config {
    union ibv_gid local_gid;
    uint8_t local_mac[6];
    uint8_t peer_mac[6];
    uint8_t l3_type;       /* 0=IPv4-mapped GID, 1=IPv6 */
    uint8_t traffic_class;
    uint8_t hop_limit;
    uint16_t udp_sport;    /* 0 selects the standard provider-derived port */
};

/* P0 per-client performance counters. */
struct ibv_mlx5_perf {
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
    /* cq_events is device-wide (every client sees the same completion-MSI-X
     * total); cq_event_wakeups is this client's own woken waits. */
    uint64_t cq_events;
    /* Device-wide firmware command counters. Every control operation goes
     * through the command path, so a high sleep ratio adds a flat millisecond
     * to memory registration, QP creation and GID programming alike. */
    uint64_t fw_commands;
    uint64_t fw_command_sleeps;
    uint64_t fw_command_slot_waits;
    uint64_t cq_event_wakeups;
};

#define IBV_MLX5_TELEMETRY_VERSION 2u
/* Fixed runtime v1 wire shape. Size-aware functions never overwrite a smaller
 * caller buffer; v2 telemetry stays frozen for old dynamically linked apps. */
struct ibv_mlx5_runtime {
    uint32_t version, size;
    uint64_t device_epoch;
    uint64_t pinned_bytes, peak_pinned_bytes, pin_failures;
    uint64_t client_pinned_bytes, client_pinned_limit, device_pinned_limit;
    uint64_t quarantine_bytes, quarantine_objects;
    uint64_t irq_completion_eqes, timer_completion_eqes, last_completion_irq_ns;
    uint32_t quarantined, bme_fenced, completion_eq_ready, completion_irq_proven;
};
struct ibv_context;
int ibv_mlx5_query_runtime(struct ibv_context *, void *, size_t);
int ibv_mlx5_query_telemetry_ex(struct ibv_context *, void *, size_t);
/* Full-stack monotonic telemetry. Unlike ibv_mlx5_perf (the stable DEXT ABI
 * snapshot), this also includes userspace direct-path and completion-channel
 * activity. Callers take a before/after snapshot around one request. */
struct ibv_mlx5_telemetry {
    uint32_t version;
    uint32_t size;
    uint64_t external_methods;
    uint64_t external_method_ns;
    uint64_t kernel_post_send_calls;
    uint64_t kernel_post_recv_calls;
    uint64_t kernel_poll_cq_methods;
    uint64_t kernel_sync_fast_path_calls;
    uint64_t kernel_sync_qp_tails_calls;
    uint64_t kernel_arm_cq_calls;
    uint64_t mr_registers;
    uint64_t mr_deregisters;
    uint64_t mr_bytes;
    uint64_t copied_bytes;
    uint64_t mapped_qps;
    uint64_t direct_send_batches;
    uint64_t direct_send_wrs;
    uint64_t direct_doorbells;
    uint64_t direct_recv_batches;
    uint64_t direct_recv_wrs;
    uint64_t cq_consumer_publications;
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
    uint64_t cq_arm_requests;
    uint64_t cq_arm_cached;
    uint64_t completion_waits;
    uint64_t completion_events;
    uint64_t completion_hw_waits;
    uint64_t completion_hw_wakeups;
    uint64_t completion_poll_ticks;
    uint64_t completion_poll_wakeups;
    uint64_t completion_lost_events;
    uint64_t single_owner_violations;
    uint64_t wc_cache_hits;
    uint64_t wc_cache_prefetched;
    /* Version 2. Driver-counted, so they pair with the direct_* fields above
     * to split one request's work between the direct path and the DEXT. */
    uint64_t kernel_doorbells;
    uint64_t kernel_cqe_errors;
    uint64_t driver_cq_events;
    uint64_t driver_cq_event_wakeups;
};

/* DCQCN reaction-point parameters (QUERY/MODIFY_CONG_PARAMS 0x824/0x825). */
struct ibv_mlx5_cong_params {
    uint32_t rpg_min_dec_fac;  /* multiplicative decrease factor */
    uint32_t rpg_ai_rate;      /* additive increase rate */
    uint32_t rpg_time_reset;   /* increase timer */
    uint32_t rpg_threshold;    /* ECN marking threshold */
    uint32_t rpg_hai;          /* hyper additive increase rate */
    uint32_t rpg_gd;           /* alpha decrease factor */
    uint32_t rpg_time_inc;     /* reserved on this generation */
    uint32_t rsvd;
};

struct ibv_gid_entry {
    union ibv_gid gid;
    uint32_t gid_index;
    uint32_t port_num;
    enum ibv_gid_type gid_type;
    uint32_t ndev_ifindex;
};

struct ibv_device { char name[64]; };
struct ibv_context;
struct ibv_srq;

enum ibv_event_type {
    IBV_EVENT_CQ_ERR = 0,
    IBV_EVENT_QP_FATAL = 1,
    IBV_EVENT_QP_REQ_ERR = 2,
    IBV_EVENT_QP_ACCESS_ERR = 3,
    IBV_EVENT_COMM_EST = 4,
    IBV_EVENT_SQ_DRAINED = 5,
    IBV_EVENT_PATH_MIG = 6,
    IBV_EVENT_PATH_MIG_ERR = 7,
    IBV_EVENT_DEVICE_FATAL = 8,
    IBV_EVENT_PORT_ACTIVE = 9,
    IBV_EVENT_PORT_ERR = 10,
    IBV_EVENT_SRQ_ERR = 13,
    IBV_EVENT_SRQ_LIMIT_REACHED = 14,
    IBV_EVENT_QP_LAST_WQE_REACHED = 16,
    IBV_EVENT_GID_CHANGE = 18,
    IBV_EVENT_WQ_FATAL = 19,
};

struct ibv_async_event {
    enum ibv_event_type event_type;
    union { struct ibv_cq *cq; struct ibv_qp *qp; struct ibv_srq *srq;
            uint8_t port_num; } element;
};

struct ibv_device_attr {
    uint64_t fw_ver;
    uint64_t page_size_cap;
    uint32_t vendor_id;
    uint32_t vendor_part_id;
    uint32_t hw_ver;
    int max_qp;
    int max_cq;
    int max_mr;
    int max_pd;
    int max_sge;
    int max_sge_rd;
    int max_qp_wr;
    int max_sge_qp;
    int max_cqe;
    int max_mr_size;
    int max_inline_data;   /* MelonDMA extension: inline SEND payload cap */
    int max_qp_rd_atom;
    int max_ee_rd_atom;
    int max_res_rd_atom;
    int max_qp_init_rd_atom;
    int max_ee_init_rd_atom;
    uint8_t phys_port_cnt;
    /* Identity, as rdma-core spells it: big-endian on the wire, so a consumer
     * that prints or compares one gets the same bytes it would from a Linux
     * host. Zero when firmware supplied none. */
    uint64_t node_guid;
    uint64_t sys_image_guid;
};
struct ibv_pd;
struct ibv_cq;
struct ibv_qp;
struct ibv_mr;
struct ibv_mw;
struct ibv_ah;
struct ibv_comp_channel;
struct ibv_srq;
struct ibv_cq;
struct ibv_qp;

struct ibv_port_attr {
    enum ibv_port_state state;
    enum ibv_mtu max_mtu;
    enum ibv_mtu active_mtu;
    int gid_tbl_len;
    uint32_t port_cap_flags;
    uint32_t max_msg_sz;
    uint32_t active_width;
    uint32_t active_speed;
    uint16_t pkey_tbl_len;
    uint8_t phys_state;
    uint8_t link_layer;
};

struct ibv_sge { uint64_t addr; uint32_t length; uint32_t lkey; };

struct ibv_send_wr {
    uint64_t wr_id;
    struct ibv_send_wr *next;
    struct ibv_sge *sg_list;
    int num_sge;
    enum ibv_wr_opcode opcode;
    unsigned int send_flags;
    union {
        uint32_t imm_data;
        uint32_t invalidate_rkey;
        struct { uint32_t invalidate_rkey; } ex;
    };
    union {
        struct { uint64_t remote_addr; uint32_t rkey; } rdma;
        struct {
            uint64_t remote_addr;
            uint32_t rkey;
            uint64_t compare_add;   /* CMP_SWAP: expected / FETCH_ADD: addend */
            uint64_t swap;          /* CMP_SWAP: new value */
        } atomic;
        struct { uint32_t invalidate_rkey; } local_inv;
        /* A datagram names its peer per work request: one address handle can
         * serve many of them, so the destination is not baked into the handle. */
        struct {
            struct ibv_ah *ah;
            uint32_t remote_qpn;
            uint32_t remote_qkey;
        } ud;
    } wr;
};

struct ibv_recv_wr {
    uint64_t wr_id;
    struct ibv_recv_wr *next;
    struct ibv_sge *sg_list;
    int num_sge;
};

struct ibv_wc {
    uint64_t wr_id;
    enum ibv_wc_status status;
    enum ibv_wc_opcode opcode;
    uint32_t vendor_err;
    uint32_t byte_len;
    union {
        uint32_t imm_data;
        uint32_t invalidated_rkey;
    };
    uint32_t qp_num;
    uint32_t src_qp;
    int wc_flags;
    uint64_t atomic_result;   /* IBV_WC_WITH_ATOMIC: pre-op remote word */
};

struct ibv_global_route {
    union ibv_gid dgid;
    uint32_t flow_label;
    uint8_t sgid_index;
    uint8_t hop_limit;
    uint8_t traffic_class;
};

struct ibv_ah_attr {
    struct ibv_global_route grh;
    uint16_t dlid;
    uint8_t sl;
    uint8_t src_path_bits;
    uint8_t static_rate;
    uint8_t is_global;
    uint8_t port_num;
};

struct ibv_qp_cap {
    uint32_t max_send_wr, max_recv_wr;
    uint32_t max_send_sge, max_recv_sge;
    uint32_t max_inline_data;
};

struct ibv_qp_init_attr {
    void *qp_context;
    struct ibv_cq *send_cq;
    struct ibv_cq *recv_cq;
    struct ibv_srq *srq;
    struct ibv_qp_cap cap;
    enum ibv_qp_type qp_type;
    int sq_sig_all;
};

struct ibv_qp_attr {
    enum ibv_qp_state qp_state;
    enum ibv_qp_state cur_qp_state;
    enum ibv_mtu path_mtu;
    uint32_t rq_psn, sq_psn, dest_qp_num;
    int qp_access_flags;
    struct ibv_ah_attr ah_attr;
    uint16_t pkey_index;
    uint8_t port_num, timeout, retry_cnt, rnr_retry;
    uint32_t max_rd_atomic, max_dest_rd_atomic, min_rnr_timer;
    /* Datagram only. Appended, never inserted: every field before it is
     * laid out by position, and moving one shifts the rest. */
    uint32_t qkey;
};

/* Public handle fields used by normal consumers such as llama.cpp. */
struct ibv_pd { struct ibv_context *context; void *priv; };
struct ibv_comp_channel {
    struct ibv_context *context;
    int fd;
    void *priv;
};
struct ibv_cq {
    struct ibv_context *context;
    int cqe;
    void *cq_context;
    void *priv;
    struct ibv_comp_channel *channel;
    uint64_t event_count;
    uint64_t acked_events;
    uint64_t lost_events;
    uint64_t notify_generation;
    uint64_t worker_generation;
    int notify_armed;
    struct ibv_cq *channel_next;
    struct ibv_cq *context_next;
    struct ibv_wc wc_cache[16];
    uint32_t wc_cache_head;
    uint32_t wc_cache_count;
    int single_threaded;
    int poll_owner_valid;
    pthread_t poll_owner;
    pthread_mutex_t poll_lock;
    pthread_mutex_t notify_lock;
};
struct ibv_ah {
    struct ibv_pd *pd;
    void *priv;
};
enum ibv_mw_type { IBV_MW_TYPE_1 = 1, IBV_MW_TYPE_2 = 2 };
struct ibv_mw_bind_info {
    struct ibv_mr *mr;
    uint64_t addr;
    uint64_t length;
    int mw_access_flags;
};
struct ibv_mw_bind {
    uint64_t wr_id;
    unsigned int send_flags;
    struct ibv_mw_bind_info bind_info;
};
struct ibv_mw {
    struct ibv_context *context;
    struct ibv_pd *pd;
    enum ibv_mw_type type;
    uint32_t rkey;
    void *priv;
};
struct ibv_mr {
    struct ibv_context *context;
    struct ibv_pd *pd;
    void *addr;
    size_t length;
    uint32_t handle, lkey, rkey;
    void *priv;
};
struct ibv_qp {
    struct ibv_context *context;
    void *qp_context;
    struct ibv_pd *pd;
    struct ibv_cq *send_cq, *recv_cq;
    uint32_t qp_num;
    enum ibv_qp_state state;
    enum ibv_qp_type qp_type;
    void *priv;
    struct ibv_qp *context_next;
};

struct ibv_device **ibv_get_device_list(int *num_devices);
void ibv_free_device_list(struct ibv_device **list);
const char *ibv_get_device_name(struct ibv_device *device);
struct ibv_context *ibv_open_device(struct ibv_device *device);
int ibv_close_device(struct ibv_context *context);
int ibv_query_device(struct ibv_context *context,
                     struct ibv_device_attr *device_attr);
int ibv_mlx5_configure_roce(struct ibv_context *context,
                             const struct ibv_mlx5_roce_config *config);
int ibv_mlx5_query_perf(struct ibv_context *context,
                        struct ibv_mlx5_perf *perf);
int ibv_mlx5_query_telemetry(struct ibv_context *context,
                             struct ibv_mlx5_telemetry *telemetry);

/* MSI-X bring-up diagnosis. A provider that does not advertise the completion
 * interrupt still answers this, so a client can report why the blocking
 * completion path is unavailable instead of silently polling. Returns ENOTSUP
 * on a provider that predates the query. */
enum {
    IBV_MLX5_IRQ_STAGE_NONE          = 0,
    IBV_MLX5_IRQ_STAGE_CONFIGURE     = 1,
    IBV_MLX5_IRQ_STAGE_QUEUE         = 2,
    IBV_MLX5_IRQ_STAGE_SOURCE        = 3,
    IBV_MLX5_IRQ_STAGE_ACTION        = 4,
    IBV_MLX5_IRQ_STAGE_HANDLER       = 5,
    IBV_MLX5_IRQ_STAGE_ENABLE        = 6,
    IBV_MLX5_IRQ_STAGE_NOT_ATTEMPTED = 7,
};
enum {
    IBV_MLX5_CQEQ_STAGE_NOT_ATTEMPTED = 0,
    IBV_MLX5_CQEQ_STAGE_ALLOC         = 1,
    IBV_MLX5_CQEQ_STAGE_INIT          = 2,
    IBV_MLX5_CQEQ_STAGE_CREATE        = 3,
    IBV_MLX5_CQEQ_STAGE_OK            = 4,
};
#define IBV_MLX5_IRQ_INDEX_MAP  16
#define IBV_MLX5_IRQ_INDEX_NONE 0xffffffffu
enum {
    IBV_MLX5_IRQ_KIND_ABSENT = 0,  /* the provider refused this index */
    IBV_MLX5_IRQ_KIND_LEVEL  = 1,  /* level-triggered, i.e. legacy INTx */
    IBV_MLX5_IRQ_KIND_EDGE   = 2,
    IBV_MLX5_IRQ_KIND_MSI    = 3,
    IBV_MLX5_IRQ_KIND_MSIX   = 4,
    IBV_MLX5_IRQ_KIND_OTHER  = 5,
};
const char *ibv_mlx5_irq_kind_name(uint8_t kind);

struct ibv_mlx5_interrupts {
    uint32_t vectors;
    uint32_t setup_status;
    uint32_t setup_stage;
    uint32_t async_eqn;
    uint32_t completion_eqn;
    uint32_t completion_ready;
    uint64_t completion_events;
    /* Completion-EQ bring-up. The vectors can be live while this EQ is
     * missing, and only then does the blocking completion path go away. */
    uint32_t completion_eq_status;
    uint32_t completion_eq_stage;
    uint32_t completion_eq_syndrome;
    uint32_t completion_eq_fw_status;
    /* Which CreateEQ variant firmware accepted, 1-based, 0 = none. */
    uint32_t completion_eq_variant;
    uint32_t completion_eq_variant_tried;
    uint32_t completion_eq_variant_syndrome[4];
    /* Does vector 0 actually deliver, and has the EQ timer stepped down? */
    uint64_t async_interrupts;
    uint64_t completion_interrupts;
    uint64_t eq_timer_ticks;
    uint32_t eq_timer_period_ms;
    /* Host interrupt index map. These are NOT firmware vector numbers:
     * firmware vector V is raised on host index msix_index_base + V. Kind
     * values are IBV_MLX5_IRQ_KIND_*. index_kind_pre is the same probe taken
     * before the provider allocated vectors. When msix_index_base is
     * IBV_MLX5_IRQ_INDEX_NONE no messaged pair answered and the provider kept
     * its historical indices, in which case a level-triggered index 0 cannot
     * deliver on a device that implements only MSI-X. */
    uint32_t index_count;
    uint32_t index_count_pre;
    uint32_t msix_index_base;
    uint32_t async_index;
    uint32_t completion_index;
    uint32_t index_probe_status;
    uint8_t  index_kind[IBV_MLX5_IRQ_INDEX_MAP];
    uint8_t  index_kind_pre[IBV_MLX5_IRQ_INDEX_MAP];
    uint64_t index_type_raw[IBV_MLX5_IRQ_INDEX_MAP];
};
int ibv_mlx5_query_interrupts(struct ibv_context *context,
                              struct ibv_mlx5_interrupts *irq);

/* Posting-path capabilities. bf_regs_per_uar is how many QPs of one client can
 * post without sharing a doorbell register and its ping-pong toggle; zero means
 * the card reports no blue flame and a WQE cannot be written into the register
 * at all. */
struct ibv_mlx5_posting_caps {
    uint32_t bf_supported;
    uint32_t log_bf_reg_size;
    uint32_t uar_page_size;
    uint32_t bf_regs_per_uar;
    uint32_t max_inline_data;
    uint32_t max_sge;
    uint32_t max_sq_depth;
    uint32_t max_qp;
    uint32_t pcie_link_speed;   /* encoded generation, 0 = unknown */
    uint32_t pcie_link_width;   /* lanes, 0 = unknown */
};
int ibv_mlx5_query_posting_caps(struct ibv_context *context,
                                struct ibv_mlx5_posting_caps *caps);
/* Raw per-direction line rate of the negotiated PCIe link in Gbit/s, 0 when
 * unknown. Report measured rates as a fraction of this, not in gigabits: the
 * fraction is what carries over to a different card or enclosure. */
double ibv_mlx5_pcie_line_gbps(uint32_t speed, uint32_t width);

/* MSI-X table access for interrupt bring-up. All three need the privileged
 * diagnostics entitlement and return EPERM without it. `data` selects one of
 * the interrupt controller's own vectors; it is not the global interrupt
 * number, and the base that relates the two lives in the device tree, so it
 * has to be found by experiment. */
#define IBV_MLX5_MSIX_DOORBELL_ADDR 0xfffff000u
#define IBV_MLX5_MSIX_TABLE_SNAPSHOT 16
#define IBV_MLX5_MSIX_PBA_WORDS 4
struct ibv_mlx5_msix_entry {
    uint32_t addr_lo, addr_hi, data, vector_control;
};
struct ibv_mlx5_msix_state {
    uint32_t cap_offset, message_control, table_size;
    uint32_t table_bir, pba_bir, table_offset, pba_offset;
    uint32_t entries_read, pba_words, status, command_reg, bar_index_used;
    struct ibv_mlx5_msix_entry entry[IBV_MLX5_MSIX_TABLE_SNAPSHOT];
    uint32_t pba[IBV_MLX5_MSIX_PBA_WORDS];
};
int ibv_mlx5_query_msix_state(struct ibv_context *context,
                              struct ibv_mlx5_msix_state *state);
int ibv_mlx5_program_msix(struct ibv_context *context, uint32_t vector,
                          uint32_t addr_lo, uint32_t addr_hi, uint32_t data,
                          int masked);
int ibv_mlx5_mask_msix(struct ibv_context *context, uint32_t vector, int masked);
const char *ibv_mlx5_irq_stage_name(uint32_t stage);
const char *ibv_mlx5_cqeq_stage_name(uint32_t stage);

/* Hardware completion moderation: hold a completion event back until `period`
 * microseconds have passed or `max_count` CQEs have accumulated. Zero in a
 * field disables that half; both zero restores unmoderated behaviour. This
 * coalesces in the NIC, so it removes wakeups rather than deciding not to act
 * on them. Returns ENOTSUP on a provider that predates it. */
int ibv_mlx5_modify_cq_moderation(struct ibv_cq *cq, uint32_t period,
                                  uint32_t max_count);
int ibv_mlx5_query_cong(struct ibv_context *context,
                        struct ibv_mlx5_cong_params *params);
int ibv_mlx5_modify_cong(struct ibv_context *context,
                         const struct ibv_mlx5_cong_params *params);
int ibv_mlx5_add_gid(struct ibv_context *context, const union ibv_gid *gid,
                     const uint8_t mac[6], uint8_t l3_type,
                     uint16_t vlan_id, uint8_t vlan_valid,
                     uint32_t *gid_index);
int ibv_mlx5_del_gid(struct ibv_context *context, uint32_t gid_index);
int ibv_query_port(struct ibv_context *context, uint8_t port_num,
                   struct ibv_port_attr *port_attr);
int ibv_query_gid(struct ibv_context *context, uint8_t port_num, int index,
                  union ibv_gid *gid);
int ibv_query_gid_ex(struct ibv_context *context, uint32_t port_num,
                     uint32_t gid_index, struct ibv_gid_entry *entry,
                     uint32_t flags);
int ibv_query_gid_table(struct ibv_context *context, uint32_t port_num,
                        struct ibv_gid_entry *entries, uint32_t capacity,
                        uint32_t *count, uint32_t *table_size);
int ibv_get_async_event(struct ibv_context *context,
                        struct ibv_async_event *event);
void ibv_ack_async_event(struct ibv_async_event *event);

struct ibv_pd *ibv_alloc_pd(struct ibv_context *context);
int ibv_dealloc_pd(struct ibv_pd *pd);
struct ibv_ah *ibv_create_ah(struct ibv_pd *pd, struct ibv_ah_attr *attr);
int ibv_destroy_ah(struct ibv_ah *ah);
struct ibv_comp_channel *ibv_create_comp_channel(struct ibv_context *context);
int ibv_destroy_comp_channel(struct ibv_comp_channel *channel);
struct ibv_cq *ibv_create_cq(struct ibv_context *context, int cqe,
                             void *cq_context,
                             struct ibv_comp_channel *channel,
                             int comp_vector);
int ibv_destroy_cq(struct ibv_cq *cq);
int ibv_req_notify_cq(struct ibv_cq *cq, int solicited_only);
int ibv_get_cq_event(struct ibv_comp_channel *channel, struct ibv_cq **cq,
                     void **cq_context);
void ibv_ack_cq_events(struct ibv_cq *cq, unsigned int nevents);
struct ibv_qp *ibv_create_qp(struct ibv_pd *pd,
                             struct ibv_qp_init_attr *qp_init_attr);
int ibv_modify_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr, int attr_mask);
int ibv_query_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr,
                 int attr_mask, struct ibv_qp_init_attr *init_attr);
int ibv_destroy_qp(struct ibv_qp *qp);
struct ibv_mr *ibv_reg_mr(struct ibv_pd *pd, void *addr, size_t length,
                          int access);
int ibv_dereg_mr(struct ibv_mr *mr);
/* Refresh an indirect MR's KLM list through a RTS QP with an idle SQ.
 * `children` must remain registered for the lifetime of `mr`. */
int ibv_mlx5_retarget_mr(struct ibv_qp *qp, struct ibv_mr *mr,
                         struct ibv_mr *const *children, uint32_t child_count);
struct ibv_mw *ibv_alloc_mw(struct ibv_pd *pd, enum ibv_mw_type type);
int ibv_dealloc_mw(struct ibv_mw *mw);
int ibv_bind_mw(struct ibv_qp *qp, struct ibv_mw *mw,
                struct ibv_mw_bind *bind);
int ibv_post_send(struct ibv_qp *qp, struct ibv_send_wr *wr,
                  struct ibv_send_wr **bad_wr);
int ibv_post_recv(struct ibv_qp *qp, struct ibv_recv_wr *wr,
                  struct ibv_recv_wr **bad_wr);
int ibv_poll_cq(struct ibv_cq *cq, int num_entries, struct ibv_wc *wc);
/* MelonDMA equivalent of a single-threaded CQ/thread-domain contract. Once
 * enabled, the first poller becomes the debug owner and poll_cq skips its
 * mutex. A poll from another thread fails instead of silently racing. */
int ibv_mlx5_set_single_threaded(struct ibv_cq *cq, int enable);
const char *ibv_wc_status_str(enum ibv_wc_status status);

/* ---- Surface required by stock rdma-core consumers -------------------------
 *
 * perftest, rping, UCX (libuct_ib) and NCCL resolve these symbols when they
 * load or dlopen the provider. A symbol that is merely absent stops the
 * consumer from starting at all, even when it would never have called it, so
 * every name below exists. What this hardware and driver actually do is
 * implemented; everything else fails cleanly with EOPNOTSUPP rather than
 * being missing. Signatures are copied verbatim from rdma-core 50.0 so that
 * consumer sources compile unchanged against this include path.
 */

typedef uint16_t __be16;
typedef uint32_t __be32;
typedef uint64_t __be64;

enum ibv_node_type {
    IBV_NODE_UNKNOWN     = -1,
    IBV_NODE_CA          = 1,
    IBV_NODE_SWITCH      = 2,
    IBV_NODE_ROUTER      = 3,
    IBV_NODE_RNIC        = 4,
    IBV_NODE_USNIC       = 5,
    IBV_NODE_USNIC_UDP   = 6,
    IBV_NODE_UNSPECIFIED = 7,
};

/* Global routing header, prepended to a UD receive. Declared so consumers that
 * handle UD compile; UD itself is not implemented yet. */
struct ibv_grh {
    __be32 version_tclass_flow;
    __be16 paylen;
    uint8_t next_hdr;
    uint8_t hop_limit;
    union ibv_gid sgid;
    union ibv_gid dgid;
};

struct ibv_srq_attr { uint32_t max_wr; uint32_t max_sge; uint32_t srq_limit; };
struct ibv_srq_init_attr { void *srq_context; struct ibv_srq_attr attr; };
struct ibv_srq {
    struct ibv_context *context;
    void *srq_context;
    struct ibv_pd *pd;
    uint32_t handle;
    void *priv;
    struct ibv_srq *context_next;
};

struct ibv_flow { uint32_t comp_mask; struct ibv_context *context; uint32_t handle; };
struct ibv_flow_attr {
    uint32_t comp_mask;
    uint32_t type;
    uint16_t size;
    uint16_t priority;
    uint8_t  num_of_specs;
    uint8_t  port;
    uint32_t flags;
};

struct ibv_ece { uint32_t vendor_id; uint32_t options; uint32_t comp_mask; };

/* Extended posting interface. In rdma-core these wrappers are static inline and
 * dispatch through function pointers the provider fills in, so the shape below
 * has to match for consumer sources to compile and link unchanged. The work is
 * accumulated into a chain and handed to the ordinary post path on complete,
 * which is also why a batch here costs one doorbell instead of N. */
struct ibv_data_buf { void *addr; size_t length; };

struct ibv_qp_ex {
    struct ibv_qp qp_base;
    uint64_t comp_mask;
    uint64_t wr_id;
    unsigned int wr_flags;

    void (*wr_atomic_cmp_swp)(struct ibv_qp_ex *qp, uint32_t rkey,
                              uint64_t remote_addr, uint64_t compare, uint64_t swap);
    void (*wr_atomic_fetch_add)(struct ibv_qp_ex *qp, uint32_t rkey,
                                uint64_t remote_addr, uint64_t add);
    void (*wr_rdma_read)(struct ibv_qp_ex *qp, uint32_t rkey, uint64_t remote_addr);
    void (*wr_rdma_write)(struct ibv_qp_ex *qp, uint32_t rkey, uint64_t remote_addr);
    void (*wr_rdma_write_imm)(struct ibv_qp_ex *qp, uint32_t rkey,
                              uint64_t remote_addr, __be32 imm_data);
    void (*wr_send)(struct ibv_qp_ex *qp);
    void (*wr_send_imm)(struct ibv_qp_ex *qp, __be32 imm_data);
    void (*wr_send_inv)(struct ibv_qp_ex *qp, uint32_t invalidate_rkey);
    void (*wr_local_inv)(struct ibv_qp_ex *qp, uint32_t invalidate_rkey);
    void (*wr_set_inline_data)(struct ibv_qp_ex *qp, void *addr, size_t length);
    void (*wr_set_inline_data_list)(struct ibv_qp_ex *qp, size_t num_buf,
                                    const struct ibv_data_buf *buf_list);
    void (*wr_set_sge)(struct ibv_qp_ex *qp, uint32_t lkey, uint64_t addr,
                       uint32_t length);
    void (*wr_set_sge_list)(struct ibv_qp_ex *qp, size_t num_sge,
                            const struct ibv_sge *sg_list);
    void (*wr_start)(struct ibv_qp_ex *qp);
    int  (*wr_complete)(struct ibv_qp_ex *qp);
    void (*wr_abort)(struct ibv_qp_ex *qp);
};

static inline void ibv_wr_start(struct ibv_qp_ex *qp) { qp->wr_start(qp); }
static inline int  ibv_wr_complete(struct ibv_qp_ex *qp) { return qp->wr_complete(qp); }
static inline void ibv_wr_abort(struct ibv_qp_ex *qp) { qp->wr_abort(qp); }
static inline void ibv_wr_rdma_write(struct ibv_qp_ex *qp, uint32_t rkey,
                                     uint64_t remote_addr)
{ qp->wr_rdma_write(qp, rkey, remote_addr); }
static inline void ibv_wr_rdma_write_imm(struct ibv_qp_ex *qp, uint32_t rkey,
                                         uint64_t remote_addr, __be32 imm_data)
{ qp->wr_rdma_write_imm(qp, rkey, remote_addr, imm_data); }
static inline void ibv_wr_rdma_read(struct ibv_qp_ex *qp, uint32_t rkey,
                                    uint64_t remote_addr)
{ qp->wr_rdma_read(qp, rkey, remote_addr); }
static inline void ibv_wr_send(struct ibv_qp_ex *qp) { qp->wr_send(qp); }
static inline void ibv_wr_send_imm(struct ibv_qp_ex *qp, __be32 imm_data)
{ qp->wr_send_imm(qp, imm_data); }
static inline void ibv_wr_send_inv(struct ibv_qp_ex *qp, uint32_t invalidate_rkey)
{ qp->wr_send_inv(qp, invalidate_rkey); }
static inline void ibv_wr_local_inv(struct ibv_qp_ex *qp, uint32_t invalidate_rkey)
{ qp->wr_local_inv(qp, invalidate_rkey); }
static inline void ibv_wr_atomic_cmp_swp(struct ibv_qp_ex *qp, uint32_t rkey,
                                         uint64_t remote_addr, uint64_t compare,
                                         uint64_t swap)
{ qp->wr_atomic_cmp_swp(qp, rkey, remote_addr, compare, swap); }
static inline void ibv_wr_atomic_fetch_add(struct ibv_qp_ex *qp, uint32_t rkey,
                                           uint64_t remote_addr, uint64_t add)
{ qp->wr_atomic_fetch_add(qp, rkey, remote_addr, add); }
static inline void ibv_wr_set_sge(struct ibv_qp_ex *qp, uint32_t lkey,
                                  uint64_t addr, uint32_t length)
{ qp->wr_set_sge(qp, lkey, addr, length); }
static inline void ibv_wr_set_sge_list(struct ibv_qp_ex *qp, size_t num_sge,
                                       const struct ibv_sge *sg_list)
{ qp->wr_set_sge_list(qp, num_sge, sg_list); }
static inline void ibv_wr_set_inline_data(struct ibv_qp_ex *qp, void *addr,
                                          size_t length)
{ qp->wr_set_inline_data(qp, addr, length); }
static inline void ibv_wr_set_inline_data_list(struct ibv_qp_ex *qp, size_t num_buf,
                                               const struct ibv_data_buf *buf_list)
{ qp->wr_set_inline_data_list(qp, num_buf, buf_list); }

const char *ibv_event_type_str(enum ibv_event_type event);
const char *ibv_node_type_str(enum ibv_node_type node_type);
int ibv_fork_init(void);
int ibv_query_pkey(struct ibv_context *context, uint8_t port_num, int index,
                   __be16 *pkey);
struct ibv_mr *ibv_reg_mr_iova2(struct ibv_pd *pd, void *addr, size_t length,
                                uint64_t iova, unsigned int access);
struct ibv_mr *ibv_reg_dmabuf_mr(struct ibv_pd *pd, uint64_t offset,
                                 size_t length, uint64_t iova, int fd, int access);

/* ---- MelonDMA extension: memory that lives in a GPU buffer -----------------
 *
 * This is deliberately not spelled ibv_*. On Linux a consumer reaches GPU
 * memory through ibv_reg_dmabuf_mr, and dma-buf is a kernel mechanism with no
 * counterpart on Darwin, so that call refuses here rather than pretending.
 * The path that does exist is this one: hand it the contents pointer of a
 * shared-storage MTLBuffer and the region it returns can be written into
 * directly by a peer, one-sided, through its rkey.
 *
 * The registration itself is an ordinary one over host-visible pages; the
 * reason this has its own name is that a reader looking for GPU memory should
 * find the supported route instead of concluding it is missing. Pass the
 * pointer from -[MTLBuffer contents]; a private-storage buffer has none and is
 * rejected, which is the same condition MlxRegisteredMetalBuffer checks. */
struct ibv_mr *melon_reg_metal_mr(struct ibv_pd *pd, void *contents,
                                  size_t length, int access);
struct ibv_srq *ibv_create_srq(struct ibv_pd *pd,
                               struct ibv_srq_init_attr *srq_init_attr);
int ibv_destroy_srq(struct ibv_srq *srq);
int ibv_post_srq_recv(struct ibv_srq *srq, struct ibv_recv_wr *recv_wr,
                      struct ibv_recv_wr **bad_recv_wr);
int ibv_modify_srq(struct ibv_srq *srq, struct ibv_srq_attr *srq_attr,
                   int srq_attr_mask);
int ibv_query_srq(struct ibv_srq *srq, struct ibv_srq_attr *srq_attr);
int ibv_get_srq_num(struct ibv_srq *srq, uint32_t *srq_num);
/* Build a reply address from a received RoCEv2 GRH.  As on rdma-core, the
 * caller may inspect the attributes first or create the AH in one call. */
int ibv_init_ah_from_wc(struct ibv_context *context, uint8_t port_num,
                        struct ibv_wc *wc, struct ibv_grh *grh,
                        struct ibv_ah_attr *ah_attr);
struct ibv_ah *ibv_create_ah_from_wc(struct ibv_pd *pd, struct ibv_wc *wc,
                                     struct ibv_grh *grh, uint8_t port_num);

enum { IBV_SRQ_MAX_WR = 1 << 0, IBV_SRQ_LIMIT = 1 << 1 };
int ibv_attach_mcast(struct ibv_qp *qp, const union ibv_gid *gid, uint16_t lid);
int ibv_detach_mcast(struct ibv_qp *qp, const union ibv_gid *gid, uint16_t lid);
struct ibv_ah *ibv_create_ah_from_wc(struct ibv_pd *pd, struct ibv_wc *wc,
                                     struct ibv_grh *grh, uint8_t port_num);
struct ibv_flow *ibv_create_flow(struct ibv_qp *qp, struct ibv_flow_attr *flow);
int ibv_destroy_flow(struct ibv_flow *flow_id);
struct ibv_qp_ex *ibv_qp_to_qp_ex(struct ibv_qp *qp);
int ibv_query_ece(struct ibv_qp *qp, struct ibv_ece *ece);
int ibv_set_ece(struct ibv_qp *qp, struct ibv_ece *ece);

#ifdef __cplusplus
}
#endif
#endif
