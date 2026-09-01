/* Minimal rdma-core compatible RC verbs surface for MelonDMA on macOS. */
#ifndef MELONDMA_INFINIBAND_VERBS_H
#define MELONDMA_INFINIBAND_VERBS_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

enum ibv_qp_type { IBV_QPT_RC = 2 };
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
    IBV_WR_ATOMIC_FETCH_AND_ADD = 7
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
enum { IBV_WC_WITH_IMM = 1 << 0, IBV_WC_WITH_ATOMIC = 1 << 1 };

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

enum ibv_event_type {
    IBV_EVENT_CQ_ERR = 0,
    IBV_EVENT_QP_FATAL = 1,
    IBV_EVENT_DEVICE_FATAL = 8,
    IBV_EVENT_PORT_ACTIVE = 9,
    IBV_EVENT_PORT_ERR = 10,
    IBV_EVENT_GID_CHANGE = 18,
};

struct ibv_async_event {
    enum ibv_event_type event_type;
    union { struct ibv_cq *cq; struct ibv_qp *qp; uint8_t port_num; } element;
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
};
struct ibv_pd;
struct ibv_cq;
struct ibv_qp;
struct ibv_mr;
struct ibv_mw;
struct ibv_ah;
struct ibv_comp_channel;
struct ibv_srq;

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
    uint32_t imm_data;
    union {
        struct { uint64_t remote_addr; uint32_t rkey; } rdma;
        struct {
            uint64_t remote_addr;
            uint32_t rkey;
            uint64_t compare_add;   /* CMP_SWAP: expected / FETCH_ADD: addend */
            uint64_t swap;          /* CMP_SWAP: new value */
        } atomic;
        struct { uint32_t invalidate_rkey; } local_inv;
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
    uint32_t imm_data;
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
    int notify_armed;
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
struct ibv_mw *ibv_alloc_mw(struct ibv_pd *pd, enum ibv_mw_type type);
int ibv_dealloc_mw(struct ibv_mw *mw);
int ibv_bind_mw(struct ibv_qp *qp, struct ibv_mw *mw,
                struct ibv_mw_bind *bind);
int ibv_post_send(struct ibv_qp *qp, struct ibv_send_wr *wr,
                  struct ibv_send_wr **bad_wr);
int ibv_post_recv(struct ibv_qp *qp, struct ibv_recv_wr *wr,
                  struct ibv_recv_wr **bad_wr);
int ibv_poll_cq(struct ibv_cq *cq, int num_entries, struct ibv_wc *wc);
const char *ibv_wc_status_str(enum ibv_wc_status status);

#ifdef __cplusplus
}
#endif
#endif
