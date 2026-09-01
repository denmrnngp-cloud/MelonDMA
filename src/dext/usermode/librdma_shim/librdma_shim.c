/*
 * librdma_shim.c — userspace RDMA shim over the MlxRDMA DriverKit DEXT.
 *
 * Control plane: IOConnectCallStructMethod → MlxUserClient::ExternalMethod,
 *   selector table and POD structs from MlxUCIO.h (shared with the DEXT).
 * Data path: post_send/post_recv/poll_cq are kernel-mediated ExternalMethods.
 * SQ/RQ/CQ/UAR and DB records stay private to the DEXT (Option B).
 *
 * Compiles against the macOS SDK (userspace), not DriverKit.
 */
#include "librdma_shim.h"
#include "MlxUCIO.h"
#include "MlxWQE.hpp"
#include "MlxServiceMatch.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>

#define SHIM_LOG(fmt, ...)  fprintf(stderr, "librdma_shim: " fmt "\n", ##__VA_ARGS__)

static inline void rdma_memory_barrier(void)
{
    __sync_synchronize();
}

/* ---- internal object layouts ---- */

struct rdma_device {
    io_connect_t  conn;
    char          name[64];
    void         *uar_map;
    size_t        uar_map_size;
    void         *db_map;
    size_t        db_map_size;
    struct rdma_qp *qps;
    struct rdma_mr *mrs;
    struct rdma_fast_path_stats stats;
};

struct rdma_pd {
    rdma_device *dev;
    uint32_t     pd;             /* firmware PD index (fixed to 1 for MVP) */
};

struct rdma_cq {
    rdma_device *dev;
    uint32_t     cq_handle;
    volatile struct MlxCqe64 *cqe_buf;   /* mapped ring */
    uint32_t     log_size;
    uint32_t     cqe_size;
    uint32_t     consumer_index;
    uint32_t     db_record_offset;
    uint32_t     depth;
};

struct rdma_qp {
    rdma_device *dev;
    rdma_pd     *pd;
    uint32_t     qpn;          /* opaque client token (ABI v2) */
    uint32_t     hw_qpn;       /* raw firmware QPN: peer exchange + WQE ctrl seg */
    uint32_t     state;
    uint32_t     sq_size;
    uint32_t     rq_size;
    void        *sq_buf;
    void        *rq_buf;
    uint32_t     bf_offset;
    uint32_t     db_record_offset;
    uint32_t     sq_stride;
    uint64_t     sq_head;
    uint64_t     sq_tail;
    uint64_t     rq_head;
    uint64_t     rq_tail;
    int          direct_sq;
    int          direct_rq;
    int          direct_cq;
    struct rdma_qp *next;
};

struct rdma_mw {
    rdma_device *dev;
    rdma_pd *pd;
    uint32_t mw_handle;
    uint32_t type;
    uint32_t rkey;
};

struct rdma_mr {
    rdma_device *dev;
    rdma_pd     *pd;
    uint32_t     mr_handle;
    uint32_t     lkey;
    uint32_t     rkey;
    void        *addr;
    uint64_t     length;
    struct rdma_mr *next;
};

struct rdma_ah {
    rdma_device *dev;
    uint32_t     ah_handle;
};

/* ---- byte order / barrier helpers ---- */

/* ---- device enumeration / open ---- */

static io_service_t find_service(const char *name);
rdma_device *rdma_open_device_by_name(const char *name);

static io_service_t
find_service(const char *name)
{
    io_iterator_t iter = 0;
    CFMutableDictionaryRef matching = mlxCreateServiceMatching();
    if (!matching) return 0;
    kern_return_t kr = IOServiceGetMatchingServices(
        kIOMainPortDefault, matching, &iter);
    if (kr != kIOReturnSuccess || !iter)
        return 0;
    io_service_t svc = 0, found = 0;
    int wanted = 0;
    bool indexed = false;
    if (name && sscanf(name, "mlx5_%d", &wanted) == 1 && wanted >= 0)
        indexed = true;
    int index = 0;
    while ((svc = IOIteratorNext(iter))) {
        if (!name || name[0] == '\0') {
            found = svc;
            break;
        }
        if (indexed) {
            if (index++ == wanted) {
                found = svc;
                break;
            }
        } else {
            CFTypeRef prop = IORegistryEntryCreateCFProperty(
                svc, CFSTR("IOName"), kCFAllocatorDefault, 0);
            char buf[64] = {0};
            bool matches = prop && CFGetTypeID(prop) == CFStringGetTypeID() &&
                CFStringGetCString((CFStringRef)prop, buf, sizeof(buf),
                                   kCFStringEncodingUTF8) &&
                strcmp(buf, name) == 0;
            if (prop) CFRelease(prop);
            if (matches) {
                found = svc;
                break;
            }
        }
        IOObjectRelease(svc);
    }
    IOObjectRelease(iter);
    return found;
}

rdma_device *rdma_open_device(void)
{
    return rdma_open_device_by_name(NULL);
}

rdma_device *rdma_open_device_by_name(const char *name)
{
    io_service_t svc = find_service(name);
    if (!svc) { SHIM_LOG("no MlxPCIDriver service found"); return NULL; }
    rdma_device *dev = (rdma_device *)calloc(1, sizeof(*dev));
    if (!dev) { IOObjectRelease(svc); return NULL; }
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &dev->conn);
    IOObjectRelease(svc);
    if (kr != kIOReturnSuccess) {
        if (kr == kIOReturnError)
            SHIM_LOG("IOServiceOpen failed: 0x%x (DEXT NewUserClient/Create failed)", kr);
        else
            SHIM_LOG("IOServiceOpen failed: 0x%x (access or DEXT user-client failure)", kr);
        free(dev); return NULL;
    }
    if (name) strlcpy(dev->name, name, sizeof(dev->name));
    struct rdma_abi_attr abi = {};
    if (rdma_query_abi(dev, &abi) != 0 ||
        (abi.features & (RDMA_FEATURE_RC | RDMA_FEATURE_ROCE_V2)) !=
        (RDMA_FEATURE_RC | RDMA_FEATURE_ROCE_V2)) {
        SHIM_LOG("DEXT lacks compatible RoCEv2 RC ABI");
        IOServiceClose(dev->conn);
        free(dev);
        return NULL;
    }
    return dev;
}

void rdma_close_device(rdma_device *dev)
{
    if (!dev) return;
    if (dev->uar_map) {
        IOConnectUnmapMemory(dev->conn,
                             MLX_UC_MEM_TYPE(kMlxUCMemKindUar, 0),
                             mach_task_self(), (mach_vm_address_t)dev->uar_map);
        dev->uar_map = NULL;
    }
    if (dev->db_map) {
        IOConnectUnmapMemory(dev->conn,
                             MLX_UC_MEM_TYPE(kMlxUCMemKindDbRecord, 0),
                             mach_task_self(), (mach_vm_address_t)dev->db_map);
        dev->db_map = NULL;
    }
    if (dev->conn) IOServiceClose(dev->conn);
    free(dev);
}

int rdma_list_devices(char ***names, int *count)
{
    if (!names || !count) return -EINVAL;
    *names = NULL; *count = 0;
    io_iterator_t iter = 0;
    CFMutableDictionaryRef matching = mlxCreateServiceMatching();
    if (!matching) return 0;
    kern_return_t kr = IOServiceGetMatchingServices(
        kIOMainPortDefault, matching, &iter);
    if (kr != kIOReturnSuccess) return 0;
    io_service_t svc; int n = 0;
    while (n < 16 && (svc = IOIteratorNext(iter))) {
        n++;
        IOObjectRelease(svc);
    }
    IOObjectRelease(iter);
    if (n == 0) return 0;
    char **arr = (char **)calloc((size_t)n, sizeof(char *));
    if (!arr) return -ENOMEM;
    /* The provider owns the stable namespace, not IORegistry's incidental
     * IOName property.  The index is also what open_device_by_name uses. */
    for (int i = 0; i < n; i++) {
        char device_name[32];
        snprintf(device_name, sizeof(device_name), "mlx5_%d", i);
        arr[i] = strdup(device_name);
        if (!arr[i]) { rdma_free_names(arr, n); return -ENOMEM; }
    }
    *names = arr; *count = n;
    return 0;
}

void rdma_free_names(char **names, int count)
{
    if (!names) return;
    for (int i = 0; i < count; i++) free(names[i]);
    free(names);
}

/* ---- isolated fast-path mapping ---- */

int rdma_enable_fast_path(rdma_device *dev, struct rdma_fast_path *path)
{
    if (!dev || !path) return -EINVAL;
    struct mlx_fast_path_resp resp = {};
    size_t out = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(
        dev->conn, kMlxUCMethodEnableFastPath, NULL, 0, &resp, &out);
    if (kr != kIOReturnSuccess || out != sizeof(resp) ||
        resp.version != MLX_FAST_PATH_ABI_VERSION ||
        resp.uarPageSize != 4096 || resp.dbPageSize != 4096)
        return -EIO;
    memset(path, 0, sizeof(*path));
    path->version = resp.version;
    path->uar_page_size = resp.uarPageSize;
    path->db_page_size = resp.dbPageSize;
    return 0;
}

int rdma_map_fast_path(rdma_device *dev, struct rdma_fast_path *path)
{
    if (!dev || !path || path->version != MLX_FAST_PATH_ABI_VERSION)
        return -EINVAL;
    mach_vm_address_t uar = 0, db = 0;
    mach_vm_size_t uar_size = 0, db_size = 0;
    kern_return_t kr = IOConnectMapMemory64(
        dev->conn, MLX_UC_MEM_TYPE(kMlxUCMemKindUar, 0), mach_task_self(),
        &uar, &uar_size, kIOMapAnywhere);
    if (kr != kIOReturnSuccess) return -EIO;
    kr = IOConnectMapMemory64(
        dev->conn, MLX_UC_MEM_TYPE(kMlxUCMemKindDbRecord, 0),
        mach_task_self(), &db, &db_size, kIOMapAnywhere);
    if (kr != kIOReturnSuccess) {
        (void)IOConnectUnmapMemory(
            dev->conn, MLX_UC_MEM_TYPE(kMlxUCMemKindUar, 0),
            mach_task_self(), uar);
        return -EIO;
    }
    if (uar_size != path->uar_page_size || db_size != path->db_page_size) {
        (void)IOConnectUnmapMemory(
            dev->conn, MLX_UC_MEM_TYPE(kMlxUCMemKindUar, 0),
            mach_task_self(), uar);
        (void)IOConnectUnmapMemory(
            dev->conn, MLX_UC_MEM_TYPE(kMlxUCMemKindDbRecord, 0),
            mach_task_self(), db);
        return -EPROTO;
    }
    dev->uar_map = (void *)(uintptr_t)uar;
    dev->uar_map_size = (size_t)uar_size;
    dev->db_map = (void *)(uintptr_t)db;
    dev->db_map_size = (size_t)db_size;
    path->uar = dev->uar_map;
    path->db_record = dev->db_map;
    return 0;
}

int rdma_fast_path_get_stats(const rdma_device *dev,
                             struct rdma_fast_path_stats *stats)
{
    if (!dev || !stats) return -EINVAL;
    *stats = dev->stats;
    return 0;
}

int rdma_qp_direct_enabled(const rdma_qp *qp)
{
    return qp ? qp->direct_sq : 0;
}

void rdma_unmap_fast_path(rdma_device *dev)
{
    if (!dev) return;
    if (dev->uar_map) {
        (void)IOConnectUnmapMemory(
            dev->conn, MLX_UC_MEM_TYPE(kMlxUCMemKindUar, 0),
            mach_task_self(), (mach_vm_address_t)dev->uar_map);
        dev->uar_map = NULL;
        dev->uar_map_size = 0;
    }
    if (dev->db_map) {
        (void)IOConnectUnmapMemory(
            dev->conn, MLX_UC_MEM_TYPE(kMlxUCMemKindDbRecord, 0),
            mach_task_self(), (mach_vm_address_t)dev->db_map);
        dev->db_map = NULL;
        dev->db_map_size = 0;
    }
}

/* ---- query ---- */

int rdma_query_abi(rdma_device *dev, struct rdma_abi_attr *attr)
{
    if (!dev || !attr) return -EINVAL;
    struct mlx_query_abi_resp resp = {};
    size_t out = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(
        dev->conn, kMlxUCMethodQueryAbi, NULL, 0, &resp, &out);
    if (kr != kIOReturnSuccess || out != sizeof(resp)) return -EIO;
    attr->version = resp.version;
    attr->features = resp.features;
    return resp.version == RDMA_UC_ABI_VERSION ? 0 : -EPROTONOSUPPORT;
}

int rdma_query_device(rdma_device *dev, struct rdma_device_attr *attr)
{
    if (!dev || !attr) return -EINVAL;
    struct mlx_query_device_resp resp = {};
    size_t out = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(
        dev->conn, kMlxUCMethodQueryDevice, NULL, 0, &resp, &out);
    if (kr != kIOReturnSuccess) {
        SHIM_LOG("query_device selector 0x%x failed: 0x%x",
                 kMlxUCMethodQueryDevice, kr);
        return -EIO;
    }
    if (out != sizeof(resp)) {
        SHIM_LOG("query_device returned %zu bytes, expected %zu",
                 out, sizeof(resp));
        return -EPROTO;
    }
    attr->fw_version     = resp.fwVersion;
    attr->device_id      = resp.deviceId;
    attr->num_ports      = resp.numPorts;
    attr->max_qp         = resp.maxQp;
    attr->max_cq         = resp.maxCq;
    attr->max_mr         = resp.maxMr;
    attr->roce_versions  = resp.roceVersions;
    attr->max_gid        = resp.maxGid;
    attr->max_msg_size   = resp.maxMsgSize;
    attr->max_inline_data = resp.maxInlineData;
    attr->max_qp_rd_atom = resp.maxQpRdAtomic;
    attr->max_qp_init_rd_atom = resp.maxQpInitRdAtomic;
    return 0;
}

int rdma_query_health(rdma_device *dev, struct rdma_health_attr *attr)
{
    if (!dev || !attr) return -EINVAL;
    struct mlx_health_resp resp = {};
    size_t out = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(
        dev->conn, kMlxUCMethodQueryHealth, NULL, 0, &resp, &out);
    if (kr != kIOReturnSuccess || out != sizeof(resp)) return -EIO;
    attr->healthy = resp.healthy;
    attr->syndrome = resp.syndrome;
    attr->ext_syndrome = resp.extSyndrome;
    attr->owned_pd = resp.ownedPd;
    attr->owned_qp = resp.ownedQp;
    attr->owned_cq = resp.ownedCq;
    attr->owned_mr = resp.ownedMr;
    attr->owned_ah = resp.ownedAh;
    return 0;
}

int rdma_query_cong(rdma_device *dev, struct rdma_cong_params *params)
{
    if (!dev || !params) return -EINVAL;
    struct mlx_cc_params raw = {};
    size_t out = sizeof(raw);
    kern_return_t kr = IOConnectCallStructMethod(
        dev->conn, kMlxUCMethodCCQuery, NULL, 0, &raw, &out);
    if (kr != kIOReturnSuccess || out != sizeof(raw)) return -EIO;
    params->rpg_min_dec_fac = raw.rpgMinDecFac;
    params->rpg_ai_rate = raw.rpgAiRate;
    params->rpg_time_reset = raw.rpgTimeReset;
    params->rpg_threshold = raw.rpgThreshold;
    params->rpg_hai = raw.rpgHai;
    params->rpg_gd = raw.rpgGd;
    params->rpg_time_inc = raw.rpgTimeInc;
    params->rsvd = 0;
    return 0;
}

int rdma_modify_cong(rdma_device *dev, const struct rdma_cong_params *params)
{
    if (!dev || !params) return -EINVAL;
    struct mlx_cc_params raw = {};
    raw.rpgMinDecFac = params->rpg_min_dec_fac;
    raw.rpgAiRate = params->rpg_ai_rate;
    raw.rpgTimeReset = params->rpg_time_reset;
    raw.rpgThreshold = params->rpg_threshold;
    raw.rpgHai = params->rpg_hai;
    raw.rpgGd = params->rpg_gd;
    raw.rpgTimeInc = params->rpg_time_inc;
    kern_return_t kr = IOConnectCallStructMethod(
        dev->conn, kMlxUCMethodCCModify, &raw, sizeof(raw), NULL, 0);
    return kr == kIOReturnSuccess ? 0 : -EIO;
}

int rdma_query_port(rdma_device *dev, struct rdma_port_attr *attr)
{
    if (!dev || !attr) return -EINVAL;
    struct mlx_query_port_resp resp = {};
    size_t out = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(
        dev->conn, kMlxUCMethodQueryPort, NULL, 0, &resp, &out);
    if (kr != kIOReturnSuccess) {
        SHIM_LOG("query_port selector 0x%x failed: 0x%x",
                 kMlxUCMethodQueryPort, kr);
        return -EIO;
    }
    if (out != sizeof(resp)) {
        SHIM_LOG("query_port returned %zu bytes, expected %zu",
                 out, sizeof(resp));
        return -EPROTO;
    }
    attr->link_layer     = resp.linkLayer;
    attr->port_state     = resp.portState;
    attr->gid_type       = resp.gidType;
    attr->active_speed_mbps = resp.activeSpeed;
    attr->max_mtu        = resp.maxMtu;
    attr->gid_tbl_len    = resp.gidTblLen;
    attr->pkey_tbl_len   = resp.pkeyTblLen;
    return 0;
}

/* ---- protection domain ---- */

rdma_pd *rdma_alloc_pd(rdma_device *dev)
{
    if (!dev) return NULL;
    uint32_t pdIndex = 0;
    size_t out = sizeof(pdIndex);
    kern_return_t kr = IOConnectCallStructMethod(
        dev->conn, kMlxUCMethodAllocPD, NULL, 0, &pdIndex, &out);
    if (kr != kIOReturnSuccess || out != sizeof(pdIndex) || !pdIndex) {
        SHIM_LOG("alloc_pd failed: kr=0x%x out=%zu pd=%u", kr, out, pdIndex);
        return NULL;
    }
    rdma_pd *pd = (rdma_pd *)calloc(1, sizeof(*pd));
    if (!pd) return NULL;
    pd->dev = dev; pd->pd = pdIndex;
    return pd;
}

int rdma_dealloc_pd(rdma_pd *pd)
{
    if (!pd) return -EINVAL;
    kern_return_t kr = IOConnectCallStructMethod(
        pd->dev->conn, kMlxUCMethodDeallocPD,
        &pd->pd, sizeof(pd->pd), NULL, 0);
    if (kr != kIOReturnSuccess) {
        SHIM_LOG("dealloc_pd failed: 0x%x", kr);
        return -EIO;
    }
    free(pd);
    return 0;
}

/* ---- completion queue ---- */

rdma_cq *rdma_create_cq(rdma_device *dev, uint32_t cqe_depth)
{
    if (!dev) return NULL;
    struct mlx_create_cq_req req = { .entries = cqe_depth };
    struct mlx_create_cq_resp resp = {};
    size_t out = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(
        dev->conn, kMlxUCMethodCreateCQ, &req, sizeof(req), &resp, &out);
    if (kr != kIOReturnSuccess || out != sizeof(resp) || !resp.cqHandle ||
        resp.logSize >= 31 || resp.cqeSize != 64) {
        SHIM_LOG("create_cq failed: kr=0x%x out=%zu cqn=%u log=%u cqe=%u",
                 kr, out, resp.cqHandle, resp.logSize, resp.cqeSize);
        if (kr == kIOReturnSuccess && resp.cqHandle)
            (void)IOConnectCallStructMethod(dev->conn, kMlxUCMethodDestroyCQ,
                                            &resp.cqHandle, sizeof(resp.cqHandle),
                                            NULL, 0);
        return NULL;
    }

    rdma_cq *cq = (rdma_cq *)calloc(1, sizeof(*cq));
    if (!cq) {
        (void)IOConnectCallStructMethod(dev->conn, kMlxUCMethodDestroyCQ,
                                        &resp.cqHandle, sizeof(resp.cqHandle),
                                        NULL, 0);
        return NULL;
    }
    cq->dev = dev;
    cq->cq_handle = resp.cqHandle;
    cq->log_size  = resp.logSize;
    cq->cqe_size  = resp.cqeSize;
    cq->depth     = 1u << resp.logSize;
    cq->consumer_index = 0;
    cq->db_record_offset = resp.dbRecordOffset;
    mach_vm_address_t cqe = 0;
    mach_vm_size_t cqe_size = 0;
    kr = IOConnectMapMemory64(
        dev->conn, MLX_UC_MEM_TYPE(kMlxUCMemKindCqe, resp.cqHandle),
        mach_task_self(), &cqe, &cqe_size, kIOMapAnywhere);
    if (kr == kIOReturnSuccess && cqe_size >= (mach_vm_size_t)cq->depth * 64) {
        cq->cqe_buf = (volatile struct MlxCqe64 *)(uintptr_t)cqe;
        cq->dev->stats.direct_cq_consumers = 0;
    }
    else if (kr == kIOReturnSuccess)
        (void)IOConnectUnmapMemory(
            dev->conn, MLX_UC_MEM_TYPE(kMlxUCMemKindCqe, resp.cqHandle),
            mach_task_self(), cqe);

    return cq;
}

int rdma_query_cq_completions(rdma_cq *cq, uint64_t *completions)
{
    if (!cq || !completions) return -EINVAL;
    size_t out = sizeof(*completions);
    kern_return_t kr = IOConnectCallStructMethod(
        cq->dev->conn, kMlxUCMethodQueryCqCompletions,
        &cq->cq_handle, sizeof(cq->cq_handle), completions, &out);
    return kr == kIOReturnSuccess && out == sizeof(*completions) ? 0 : -EIO;
}

int rdma_destroy_cq(rdma_cq *cq)
{
    if (!cq) return -EINVAL;
    if (cq->cqe_buf) {
        kern_return_t unmapkr = IOConnectUnmapMemory(
            cq->dev->conn, MLX_UC_MEM_TYPE(kMlxUCMemKindCqe, cq->cq_handle),
            mach_task_self(), (mach_vm_address_t)cq->cqe_buf);
        if (unmapkr != kIOReturnSuccess) return -EIO;
        cq->cqe_buf = NULL;
    }
    uint32_t h = cq->cq_handle;
    kern_return_t kr = IOConnectCallStructMethod(
        cq->dev->conn, kMlxUCMethodDestroyCQ,
        &h, sizeof(h), NULL, 0);
    if (kr != kIOReturnSuccess) {
        SHIM_LOG("destroy_cq failed: 0x%x", kr);
        return -EIO;
    }
    free(cq);
    return 0;
}

int rdma_poll_cq(rdma_cq *cq, struct rdma_wc *wc, int num)
{
    if (!cq || !wc || num <= 0) return -EINVAL;
    struct mlx_poll_cq_req req = {
        .cqHandle = cq->cq_handle,
        .maxEntries = (uint32_t)(num > MLX_UC_MAX_POLL_WC ?
                                 MLX_UC_MAX_POLL_WC : num),
    };
    struct mlx_poll_cq_resp resp = {};
    size_t out = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(
        cq->dev->conn, kMlxUCMethodPollCQ, &req, sizeof(req), &resp, &out);
    if (kr != kIOReturnSuccess || out != sizeof(resp) ||
        resp.count > req.maxEntries || resp.count > MLX_UC_MAX_POLL_WC) {
        SHIM_LOG("poll_cq failed: kr=0x%x out=%zu count=%u max=%u",
                 kr, out, resp.count, req.maxEntries);
        return -EIO;
    }
    for (uint32_t i = 0; i < resp.count; i++) {
        wc[i].wr_id = resp.wc[i].wrId;
        wc[i].status = resp.wc[i].status;
        wc[i].opcode = resp.wc[i].opcode;
        wc[i].byte_len = resp.wc[i].byteLen;
        wc[i].qp_num = resp.wc[i].qpNum; /* token; translated to raw QPN on match */
        wc[i].imm_data = resp.wc[i].immData;
        wc[i].wc_flags = resp.wc[i].wcFlags;
        wc[i].vendor_err = resp.wc[i].vendorError;
        wc[i].wqe_counter = resp.wc[i].wqeCounter;
        wc[i].atomic_result = resp.wc[i].atomicResult;
        if (resp.wc[i].opcode == RDMA_WC_RECV)
            for (rdma_qp *qp = cq->dev->qps; qp; qp = qp->next)
                if (qp->qpn == resp.wc[i].qpNum) {
                    wc[i].qp_num = qp->hw_qpn;
                    uint64_t done = (qp->rq_head & ~0xffffULL) |
                                    resp.wc[i].wqeCounter;
                    if (done > qp->rq_head) done -= 0x10000ULL;
                    if (done + 1 > qp->rq_tail) qp->rq_tail = done + 1;
                }
        if (resp.wc[i].opcode != RDMA_WC_RECV) {
            for (rdma_qp *qp = cq->dev->qps; qp; qp = qp->next) {
                if (qp->qpn == resp.wc[i].qpNum) {
                    wc[i].qp_num = qp->hw_qpn;
                    if (qp->direct_sq)
                        qp->sq_tail = qp->sq_head;
                }
            }
        }
    }
    cq->consumer_index += resp.count;
    /* CQE decode remains DEXT-owned; publish the resulting consumer index
     * directly through this CQ's isolated DB record. */
    if (resp.count && cq->dev->db_map &&
        cq->db_record_offset + sizeof(uint32_t) <= cq->dev->db_map_size) {
        volatile uint32_t *db = (volatile uint32_t *)
            ((uint8_t *)cq->dev->db_map + cq->db_record_offset);
        rdma_memory_barrier();
        db[0] = __builtin_bswap32(cq->consumer_index & 0xffffffu);
        rdma_memory_barrier();
        cq->dev->stats.direct_cq_consumers++;
    }
    return (int)resp.count;
}

int rdma_update_cq_consumer(rdma_cq *cq, uint32_t consumer_index)
{
    if (!cq) return -EINVAL;
    struct mlx_update_cq_consumer_req req = {
        .cqHandle = cq->cq_handle,
        .consumerIndex = consumer_index,
    };
    kern_return_t kr = IOConnectCallStructMethod(
        cq->dev->conn, kMlxUCMethodUpdateCqConsumer,
        &req, sizeof(req), NULL, 0);
    return (kr == kIOReturnSuccess) ? 0 : -EIO;
}

int rdma_arm_cq(rdma_cq *cq, int solicited_only)
{
    if (!cq) return -EINVAL;
    struct mlx_arm_cq_req req = { .cqHandle = cq->cq_handle,
                                   .solicitedOnly = solicited_only ? 1 : 0 };
    kern_return_t kr = IOConnectCallStructMethod(cq->dev->conn,
        kMlxUCMethodArmCQ, &req, sizeof(req), NULL, 0);
    return kr == kIOReturnSuccess ? 0 :
           (kr == kIOReturnNotFound ? -ENOENT : -EIO);
}

/* ---- queue pair ---- */

rdma_qp *rdma_create_qp(rdma_pd *pd, const struct rdma_qp_init_attr *init)
{
    if (!pd || !init || !init->send_cq || !init->recv_cq) return NULL;
    struct mlx_create_qp_req req = {};
    req.pd            = pd->pd;
    req.sendCq        = init->send_cq->cq_handle;
    req.recvCq        = init->recv_cq->cq_handle;
    req.qpType        = init->qp_type;
    req.sqSize        = init->cap_sq;
    req.rqSize        = init->cap_rq;
    /* Option B: user buffers are never exposed as hardware WQs. */
    req.sqBufAddr     = 0;
    req.rqBufAddr     = 0;
    req.maxInlineData = init->max_inline_data;
    struct mlx_create_qp_resp resp = {};
    size_t out = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(
        pd->dev->conn, kMlxUCMethodCreateQP, &req, sizeof(req), &resp, &out);
    if (kr != kIOReturnSuccess || out != sizeof(resp) || !resp.qpn ||
        resp.sqStrideSize != 64) {
        SHIM_LOG("create_qp failed: kr=0x%x out=%zu qpn=%u stride=%u",
                 kr, out, resp.qpn, resp.sqStrideSize);
        if (kr == kIOReturnSuccess && resp.qpn)
            (void)IOConnectCallStructMethod(pd->dev->conn,
                                            kMlxUCMethodDestroyQP,
                                            &resp.qpn, sizeof(resp.qpn), NULL, 0);
        return NULL;
    }
    rdma_qp *qp = (rdma_qp *)calloc(1, sizeof(*qp));
    if (!qp) {
        (void)IOConnectCallStructMethod(pd->dev->conn, kMlxUCMethodDestroyQP,
                                        &resp.qpn, sizeof(resp.qpn), NULL, 0);
        return NULL;
    }
    qp->dev = pd->dev; qp->pd = pd;
    qp->qpn = resp.qpn; qp->hw_qpn = resp.hwQpn; qp->state = RDMA_QPS_RESET;
    qp->sq_size = init->cap_sq; qp->rq_size = init->cap_rq;
    qp->sq_buf = NULL; qp->rq_buf = NULL;
    qp->bf_offset = resp.bfOffset;
    qp->db_record_offset = resp.dbRecordOffset;
    qp->sq_stride = resp.sqStrideSize;

    /* Direct SQ is opt-in and only enabled when the DEXT returned the exact
     * mapping contract. Failure is deliberately a transparent fallback. */
    const char *direct = getenv("MELONDMA_DIRECT_UAR");
    if (direct && strcmp(direct, "0") != 0 && pd->dev->uar_map &&
        pd->dev->db_map && resp.mappingVersion == MLX_FAST_PATH_ABI_VERSION &&
        resp.sqStrideSize == 64 && resp.bfOffset + sizeof(uint64_t) <=
        pd->dev->uar_map_size && resp.dbRecordOffset + 8 <=
        pd->dev->db_map_size) {
        mach_vm_address_t sq = 0;
        mach_vm_size_t sq_size = 0;
        kern_return_t mapkr = IOConnectMapMemory64(
            pd->dev->conn, MLX_UC_MEM_TYPE(kMlxUCMemKindSq, resp.qpn),
            mach_task_self(), &sq, &sq_size, kIOMapAnywhere);
        if (mapkr == kIOReturnSuccess && sq_size >=
            (mach_vm_size_t)init->cap_sq * 64) {
            qp->sq_buf = (void *)(uintptr_t)sq;
            qp->direct_sq = 1;
            mach_vm_address_t rq = 0;
            mach_vm_size_t rq_size = 0;
            kern_return_t rqkr = IOConnectMapMemory64(
                pd->dev->conn, MLX_UC_MEM_TYPE(kMlxUCMemKindRq, resp.qpn),
                mach_task_self(), &rq, &rq_size, kIOMapAnywhere);
            if (rqkr == kIOReturnSuccess && rq_size >=
                (mach_vm_size_t)init->cap_rq * 64) {
                qp->rq_buf = (void *)(uintptr_t)rq;
                qp->direct_rq = 1;
            } else if (rqkr == kIOReturnSuccess) {
                (void)IOConnectUnmapMemory(
                    pd->dev->conn, MLX_UC_MEM_TYPE(kMlxUCMemKindRq, resp.qpn),
                    mach_task_self(), rq);
            }
            pd->dev->stats.mapped_qps++;
            SHIM_LOG("QP[%u] direct SQ mapped size=%llu uar=%u bf=0x%x db=0x%x",
                     qp->qpn, (unsigned long long)sq_size, resp.uarPage,
                     resp.bfOffset, resp.dbRecordOffset);
        } else if (mapkr == kIOReturnSuccess) {
            (void)IOConnectUnmapMemory(
                pd->dev->conn, MLX_UC_MEM_TYPE(kMlxUCMemKindSq, resp.qpn),
                mach_task_self(), sq);
        }
    }
    qp->next = pd->dev->qps;
    pd->dev->qps = qp;
    return qp;
}

int rdma_destroy_qp(rdma_qp *qp)
{
    if (!qp) return -EINVAL;
    /* Revoke the client mapping before asking firmware to release the WQ.
     * This is the direct-UAR lifetime boundary: userspace must lose access
     * before the DEXT can release the DMA-backed SQ. */
    if (qp->direct_rq && qp->rq_buf) {
        kern_return_t unmapkr = IOConnectUnmapMemory(
            qp->dev->conn, MLX_UC_MEM_TYPE(kMlxUCMemKindRq, qp->qpn),
            mach_task_self(), (mach_vm_address_t)qp->rq_buf);
        if (unmapkr != kIOReturnSuccess) return -EIO;
        qp->rq_buf = NULL;
        qp->direct_rq = 0;
    }
    if (qp->direct_sq && qp->sq_buf) {
        kern_return_t unmapkr = IOConnectUnmapMemory(
            qp->dev->conn, MLX_UC_MEM_TYPE(kMlxUCMemKindSq, qp->qpn),
            mach_task_self(), (mach_vm_address_t)qp->sq_buf);
        if (unmapkr != kIOReturnSuccess) {
            SHIM_LOG("destroy_qp refused: SQ unmap failed: 0x%x", unmapkr);
            return -EIO;
        }
        qp->sq_buf = NULL;
        qp->direct_sq = 0;
    }
    uint32_t h = qp->qpn;
    kern_return_t kr = IOConnectCallStructMethod(
        qp->dev->conn, kMlxUCMethodDestroyQP,
        &h, sizeof(h), NULL, 0);
    if (kr != kIOReturnSuccess) {
        SHIM_LOG("destroy_qp failed: 0x%x", kr);
        return -EIO;
    }
    struct rdma_qp **cursor = &qp->dev->qps;
    while (*cursor && *cursor != qp) cursor = &(*cursor)->next;
    if (*cursor == qp) *cursor = qp->next;
    free(qp);
    return 0;
}

int rdma_modify_qp(rdma_qp *qp, const struct rdma_qp_attr *attr)
{
    if (!qp || !attr) return -EINVAL;
    struct mlx_modify_qp_req req = {};
    req.qpn            = qp->qpn;
    req.curState       = attr->cur_state;
    req.newState       = attr->new_state;
    req.attrMask       = attr->attr_mask;
    req.destQpn        = attr->dest_qpn;
    req.pathMtu        = attr->path_mtu;
    req.rqPsn          = attr->rq_psn;
    req.sqPsn          = attr->sq_psn;
    req.pkeyIndex      = attr->pkey_index;
    req.portNum        = attr->port_num;
    memcpy(req.ahDmac, attr->ah_dmac, 6);
    memcpy(req.ahDgid, attr->ah_dgid, 16);
    req.ahSgidIndex    = attr->ah_sgid_index;
    req.ahHopLimit     = attr->ah_hop_limit;
    req.ahTrafficClass = attr->ah_traffic_class;
    req.ahUdpSport     = attr->ah_udp_sport;
    req.minRnrTimer    = attr->min_rnr_timer;
    req.maxDestRdAtomic= attr->max_dest_rd_atomic;
    req.maxRdAtomic    = attr->max_rd_atomic;
    req.ackTimeout     = attr->timeout;
    req.retryCount     = attr->retry_cnt;
    req.rnrRetry       = attr->rnr_retry;
    req.sl             = attr->sl;
    kern_return_t kr = IOConnectCallStructMethod(
        qp->dev->conn, kMlxUCMethodModifyQP, &req, sizeof(req), NULL, 0);
    if (kr != kIOReturnSuccess) return -EIO;
    qp->state = attr->new_state;
    return 0;
}

uint32_t rdma_qp_number(const rdma_qp *qp)
{
    return qp ? qp->hw_qpn : 0;
}

int rdma_query_qp(rdma_qp *qp, uint32_t *state)
{
    if (!qp || !state) return -EINVAL;
    struct mlx_query_qp_resp resp = {};
    size_t out = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(
        qp->dev->conn, kMlxUCMethodQueryQP, &qp->qpn, sizeof(qp->qpn),
        &resp, &out);
    if (kr != kIOReturnSuccess || out != sizeof(resp) || resp.qpn != qp->qpn)
        return -EIO;
    *state = resp.state;
    return 0;
}

int rdma_set_roce_address(rdma_device *dev, const uint8_t gid[16],
                          const uint8_t mac[6], uint8_t l3_type,
                          uint32_t *gid_index)
{
    if (!dev || !gid || !mac || !gid_index || l3_type > 1) return -EINVAL;
    struct mlx_set_gid_req req = {};
    req.roceVersion = 2;
    req.l3Type = l3_type;
    memcpy(req.gid, gid, sizeof(req.gid));
    memcpy(req.mac, mac, sizeof(req.mac));

    /* A previous client can leave a firmware GID slot stale while its
     * UserClient ownership has already gone away. Retry a bounded number of
     * newly allocated slots; never reuse a slot still owned by this client. */
    for (unsigned int attempt = 0; attempt < 4; attempt++) {
        uint32_t index = 0;
        size_t out = sizeof(index);
        kern_return_t kr = IOConnectCallStructMethod(
            dev->conn, kMlxUCMethodGetGidIndex, NULL, 0, &index, &out);
        if (kr != kIOReturnSuccess || out != sizeof(index)) {
            SHIM_LOG("set_roce_address: GET_GID attempt=%u kr=0x%x out=%zu",
                     attempt, kr, out);
            if (kr == kIOReturnNoResources || kr == kIOReturnBusy) {
                usleep(1000);
                continue;
            }
            return -EIO;
        }
        req.index = index;
        kr = IOConnectCallStructMethod(dev->conn, kMlxUCMethodSetGid,
                                       &req, sizeof(req), NULL, 0);
        if (kr == kIOReturnSuccess) {
            struct mlx_query_gid_resp verify = {};
            size_t verifySize = sizeof(verify);
            kr = IOConnectCallStructMethod(dev->conn, kMlxUCMethodQueryGid,
                                           &index, sizeof(index), &verify, &verifySize);
            if (kr == kIOReturnSuccess && verifySize == sizeof(verify) &&
                verify.index == index && verify.roceVersion == 2 &&
                !memcmp(verify.gid, gid, sizeof(verify.gid)) &&
                !memcmp(verify.mac, mac, sizeof(verify.mac)) &&
                verify.l3Type == l3_type) {
                *gid_index = index;
                return 0;
            }
            SHIM_LOG("set_roce_address: QUERY mismatch attempt=%u idx=%u kr=0x%x out=%zu",
                     attempt, index, kr, verifySize);
        } else {
            SHIM_LOG("set_roce_address: SET attempt=%u idx=%u kr=0x%x",
                     attempt, index, kr);
        }
        (void)IOConnectCallStructMethod(dev->conn, kMlxUCMethodDelGid,
                                        &index, sizeof(index), NULL, 0);
    }
    return -EIO;
}

int rdma_set_roce_address_vlan(rdma_device *dev, const uint8_t gid[16],
                               const uint8_t mac[6], uint8_t l3_type,
                               uint16_t vlan_id, uint8_t vlan_valid,
                               uint32_t *gid_index)
{
    if (!dev || !gid || !mac || !gid_index || l3_type > 1 ||
        (vlan_valid ? vlan_id > 4095 : vlan_id != 0))
        return -EINVAL;
    struct mlx_set_gid_req req = {};
    req.roceVersion = 2;
    req.l3Type = l3_type;
    req.vlanId = vlan_id;
    req.vlanValid = vlan_valid ? 1 : 0;
    memcpy(req.gid, gid, sizeof(req.gid));
    memcpy(req.mac, mac, sizeof(req.mac));

    for (unsigned int attempt = 0; attempt < 4; attempt++) {
        uint32_t index = 0;
        size_t out = sizeof(index);
        kern_return_t kr = IOConnectCallStructMethod(
            dev->conn, kMlxUCMethodGetGidIndex, NULL, 0, &index, &out);
        if (kr != kIOReturnSuccess || out != sizeof(index)) {
            if (kr == kIOReturnNoResources || kr == kIOReturnBusy) {
                usleep(1000);
                continue;
            }
            return -EIO;
        }
        req.index = index;
        kr = IOConnectCallStructMethod(dev->conn, kMlxUCMethodSetGid,
                                       &req, sizeof(req), NULL, 0);
        if (kr == kIOReturnSuccess) {
            struct mlx_query_gid_resp verify = {};
            size_t verifySize = sizeof(verify);
            kr = IOConnectCallStructMethod(dev->conn, kMlxUCMethodQueryGid,
                                           &index, sizeof(index), &verify, &verifySize);
            if (kr == kIOReturnSuccess && verifySize == sizeof(verify) &&
                verify.index == index && verify.roceVersion == 2 &&
                !memcmp(verify.gid, gid, sizeof(verify.gid)) &&
                !memcmp(verify.mac, mac, sizeof(verify.mac)) &&
                verify.l3Type == l3_type &&
                verify.vlanValid == (vlan_valid ? 1 : 0) &&
                verify.vlanId == vlan_id) {
                *gid_index = index;
                return 0;
            }
        }
        (void)IOConnectCallStructMethod(dev->conn, kMlxUCMethodDelGid,
                                        &index, sizeof(index), NULL, 0);
    }
    return -EIO;
}

int rdma_query_gid(rdma_device *dev, uint32_t gid_index,
                   struct rdma_gid_attr *attr)
{
    if (!dev || !attr) return -EINVAL;
    struct mlx_query_gid_resp resp = {};
    size_t out = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(
        dev->conn, kMlxUCMethodQueryGid, &gid_index, sizeof(gid_index),
        &resp, &out);
    if (kr != kIOReturnSuccess || out != sizeof(resp) || resp.index != gid_index ||
        resp.roceVersion != 2) return -ENOENT;
    memcpy(attr->gid, resp.gid, sizeof(attr->gid));
    memcpy(attr->mac, resp.mac, sizeof(attr->mac));
    attr->l3_type = resp.l3Type;
    attr->vlan_valid = resp.vlanValid;
    attr->vlan_id = resp.vlanId;
    attr->gid_type = resp.gidType;
    attr->ifindex = resp.ifindex;
    return 0;
}

int rdma_clear_roce_address(rdma_device *dev, uint32_t gid_index)
{
    if (!dev) return -EINVAL;
    kern_return_t kr = IOConnectCallStructMethod(
        dev->conn, kMlxUCMethodDelGid,
        &gid_index, sizeof(gid_index), NULL, 0);
    return kr == kIOReturnSuccess ? 0 : -EIO;
}

int rdma_query_gid_table(rdma_device *dev, struct rdma_gid_table_entry *entries,
                         uint32_t capacity, uint32_t *count, uint32_t *table_size)
{
    if (!dev || !entries || !capacity || !count || !table_size) return -EINVAL;
    uint32_t total = 0, table = 0, start = 0;
    for (;;) {
        struct mlx_query_gid_table_req req = {
            .startIndex = start, .maxEntries = MLX_UC_MAX_GID_CHUNK,
        };
        struct mlx_query_gid_table_resp resp = {};
        size_t out = sizeof(resp);
        kern_return_t kr = IOConnectCallStructMethod(dev->conn,
            kMlxUCMethodQueryGidTable, &req, sizeof(req), &resp, &out);
        if (kr != kIOReturnSuccess || out != sizeof(resp)) {
            SHIM_LOG("query_gid_table chunk start=%u failed: kr=0x%x out=%zu expect=%zu",
                     start, kr, out, sizeof(resp));
            return -EIO;
        }
        table = resp.tableSize;
        for (uint32_t i = 0; i < resp.count && total < capacity; i++) {
            entries[total].index = resp.entry[i].index;
            memcpy(entries[total].gid, resp.entry[i].gid, 16);
            memcpy(entries[total].mac, resp.entry[i].mac, 6);
            entries[total].roce_version = resp.entry[i].roceVersion;
            entries[total].l3_type = resp.entry[i].l3Type;
            entries[total].gid_type = resp.entry[i].gidType;
            entries[total].vlan_valid = resp.entry[i].vlanValid;
            entries[total].vlan_id = resp.entry[i].vlanId;
            entries[total].ifindex = resp.entry[i].ifindex;
            total++;
        }
        if (!resp.more) break;
        start += MLX_UC_MAX_GID_CHUNK;
    }
    *count = total;
    *table_size = table;
    return 0;
}

/* ---- memory registration ---- */

struct rdma_mr *rdma_reg_mr(rdma_pd *pd, void *addr, uint64_t length,
                             uint32_t access_flags,
                             struct rdma_mr_attr_resp *out)
{
    if (!pd || !addr || !length) return NULL;
    struct mlx_reg_mr_req req = {
        .startAddr   = (uint64_t)addr,
        .length      = length,
        .accessFlags = access_flags,
        .pd          = pd->pd,
    };
    struct mlx_reg_mr_resp resp = {};
    size_t outsz = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(
        pd->dev->conn, kMlxUCMethodRegMR, &req, sizeof(req), &resp, &outsz);
    if (kr != kIOReturnSuccess || outsz != sizeof(resp) || !resp.mrHandle ||
        !resp.lkey || resp.iova != req.startAddr) {
        SHIM_LOG("reg_mr failed: kr=0x%x out=%zu mr=%u lkey=0x%x va=0x%llx expected=0x%llx",
                 kr, outsz, resp.mrHandle, resp.lkey,
                 (unsigned long long)resp.iova,
                 (unsigned long long)req.startAddr);
        if (kr == kIOReturnSuccess && resp.mrHandle)
            (void)IOConnectCallStructMethod(pd->dev->conn,
                                            kMlxUCMethodDeregMR,
                                            &resp.mrHandle,
                                            sizeof(resp.mrHandle), NULL, 0);
        return NULL;
    }
    struct rdma_mr *mr = (struct rdma_mr *)calloc(1, sizeof(*mr));
    if (!mr) {
        (void)IOConnectCallStructMethod(pd->dev->conn, kMlxUCMethodDeregMR,
                                        &resp.mrHandle, sizeof(resp.mrHandle),
                                        NULL, 0);
        return NULL;
    }
    mr->dev = pd->dev; mr->pd = pd; mr->mr_handle = resp.mrHandle;
    mr->lkey = resp.lkey; mr->rkey = resp.rkey;
    mr->addr = addr; mr->length = length;
    if (out) { out->mr_handle = resp.mrHandle; out->lkey = resp.lkey; out->rkey = resp.rkey; }
    return mr;
}

struct rdma_mr *rdma_reg_mr_indirect(rdma_pd *pd, struct rdma_mr *const *children,
                                     uint32_t child_count, uint64_t addr,
                                     uint64_t length, uint32_t access_flags,
                                     struct rdma_mr_attr_resp *out)
{
    if (!pd || !children || !child_count || !addr || !length ||
        child_count > RDMA_MAX_INDIRECT_MR_CHILDREN)
        return NULL;
    struct mlx_reg_mr_indirect_req req = {
        .startAddr   = addr,
        .length      = length,
        .accessFlags = access_flags,
        .pd          = pd->pd,
        .childCount  = child_count,
    };
    for (uint32_t i = 0; i < child_count; i++) {
        if (!children[i]) return NULL;
        req.childHandles[i] = children[i]->mr_handle;
    }
    struct mlx_reg_mr_resp resp = {};
    size_t outsz = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(
        pd->dev->conn, kMlxUCMethodRegMRIndirect, &req, sizeof(req), &resp, &outsz);
    if (kr != kIOReturnSuccess || outsz != sizeof(resp) || !resp.mrHandle ||
        !resp.lkey || resp.iova != req.startAddr) {
        SHIM_LOG("reg_mr_indirect failed: kr=0x%x out=%zu mr=%u lkey=0x%x children=%u",
                 kr, outsz, resp.mrHandle, resp.lkey, child_count);
        if (kr == kIOReturnSuccess && resp.mrHandle)
            (void)IOConnectCallStructMethod(pd->dev->conn,
                                            kMlxUCMethodDeregMR,
                                            &resp.mrHandle,
                                            sizeof(resp.mrHandle), NULL, 0);
        return NULL;
    }
    struct rdma_mr *mr = (struct rdma_mr *)calloc(1, sizeof(*mr));
    if (!mr) {
        (void)IOConnectCallStructMethod(pd->dev->conn, kMlxUCMethodDeregMR,
                                        &resp.mrHandle, sizeof(resp.mrHandle),
                                        NULL, 0);
        return NULL;
    }
    mr->dev = pd->dev; mr->pd = pd; mr->mr_handle = resp.mrHandle;
    mr->lkey = resp.lkey; mr->rkey = resp.rkey;
    mr->addr = (void *)(uintptr_t)addr; mr->length = length;
    if (out) { out->mr_handle = resp.mrHandle; out->lkey = resp.lkey; out->rkey = resp.rkey; }
    return mr;
}

int rdma_activate_indirect_mr(rdma_qp *qp, rdma_cq *cq, struct rdma_mr *mr,
                              struct rdma_mr *const *children,
                              uint32_t child_count)
{
    if (!qp || !cq || !mr || !children || !child_count ||
        child_count > RDMA_MAX_INDIRECT_MR_CHILDREN)
        return -EINVAL;
    struct mlx_post_umr_klm_req req = {
        .qpn = qp->qpn,
        .mrHandle = mr->mr_handle,
        .childCount = child_count,
        .wrId = 0x554d5200ull | (mr->mr_handle & 0xffull), /* 'UMR\0'-ish tag */
    };
    for (uint32_t i = 0; i < child_count; i++) {
        if (!children[i]) return -EINVAL;
        req.childHandles[i] = children[i]->mr_handle;
    }
    kern_return_t kr = IOConnectCallStructMethod(
        qp->dev->conn, kMlxUCMethodPostUmrKlm, &req, sizeof(req), NULL, 0);
    if (kr != kIOReturnSuccess) {
        SHIM_LOG("activate_indirect_mr: post failed kr=0x%x", kr);
        return kr == kIOReturnBusy ? -EAGAIN : -EIO;
    }
    /* Block until this WR's own completion appears. There is no async wait
     * primitive in this shim; every other "signaled and awaited" op in the
     * test harnesses that use it already spins on rdma_poll_cq the same
     * way, just one layer up — this pulls that pattern down for the one
     * caller (activation) that can never be skipped or reordered. */
    struct rdma_wc wc[MLX_UC_MAX_POLL_WC];
    for (int spins = 0; spins < 2000000; spins++) {
        int n = rdma_poll_cq(cq, wc, MLX_UC_MAX_POLL_WC);
        if (n < 0) return n;
        for (int i = 0; i < n; i++) {
            if (wc[i].qp_num != qp->hw_qpn || wc[i].wr_id != req.wrId)
                continue;
            if (wc[i].status != RDMA_WC_SUCCESS) {
                SHIM_LOG("activate_indirect_mr: UMR completed with error "
                         "status=%u vendor_err=0x%x", wc[i].status,
                         wc[i].vendor_err);
                return -EIO;
            }
            return 0;
        }
    }
    SHIM_LOG("activate_indirect_mr: timed out waiting for UMR completion");
    return -ETIMEDOUT;
}

int rdma_dbg_exec(rdma_device *dev, uint32_t opcode, const void *in,
                  uint32_t inSize, void *out, uint32_t outCapacity,
                  uint32_t *outSize, uint32_t timeoutMs)
{
    if (!dev || inSize > 64 || outCapacity > 128) return -EINVAL;
    struct mlx_dbg_exec_req req = {
        .opcode = opcode, .inSize = inSize,
        .outSize = outCapacity, .timeoutMs = timeoutMs,
    };
    if (in && inSize) memcpy(req.in, in, inSize);
    struct mlx_dbg_exec_resp resp = {};
    size_t outsz = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(
        dev->conn, kMlxUCMethodDbgExec, &req, sizeof(req), &resp, &outsz);
    if (kr != kIOReturnSuccess || outsz != sizeof(resp) ||
        resp.kr != kIOReturnSuccess)
        return -EIO;
    if (out && resp.outSize) memcpy(out, resp.out, resp.outSize);
    if (outSize) *outSize = resp.outSize;
    return 0;
}

rdma_mw *rdma_alloc_mw(rdma_pd *pd, uint32_t type)
{
    if (!pd || type != 2) return NULL;
    struct mlx_alloc_mw_req req = { .pd = pd->pd, .type = type };
    struct mlx_alloc_mw_resp resp = {};
    size_t outsz = sizeof(resp);
    if (IOConnectCallStructMethod(pd->dev->conn, kMlxUCMethodAllocMW,
                                  &req, sizeof(req), &resp, &outsz) != kIOReturnSuccess ||
        outsz != sizeof(resp) || !resp.mwHandle || !resp.rkey) return NULL;
    rdma_mw *mw = calloc(1, sizeof(*mw));
    if (!mw) {
        struct mlx_dealloc_mw_req cleanup = { .mwHandle = resp.mwHandle };
        (void)IOConnectCallStructMethod(pd->dev->conn, kMlxUCMethodDeallocMW,
                                        &cleanup, sizeof(cleanup), NULL, 0);
        return NULL;
    }
    mw->dev = pd->dev; mw->pd = pd; mw->mw_handle = resp.mwHandle;
    mw->type = type; mw->rkey = resp.rkey;
    return mw;
}

int rdma_dealloc_mw(rdma_mw *mw)
{
    if (!mw) return -EINVAL;
    struct mlx_dealloc_mw_req req = { .mwHandle = mw->mw_handle };
    kern_return_t kr = IOConnectCallStructMethod(mw->dev->conn,
        kMlxUCMethodDeallocMW, &req, sizeof(req), NULL, 0);
    if (kr != kIOReturnSuccess) return kr == kIOReturnBusy ? -EBUSY : -EIO;
    free(mw); return 0;
}

int rdma_bind_mw(rdma_qp *qp, rdma_mw *mw, rdma_mr *mr,
                 uint64_t addr, uint64_t length, uint32_t access_flags,
                 uint32_t send_flags, uint64_t wr_id, uint32_t *new_rkey)
{
    if (!qp || !mw || !mr || mw->pd != mr->pd || !length ||
        addr < (uint64_t)(uintptr_t)mr->addr ||
        length > mr->length || addr > (uint64_t)(uintptr_t)mr->addr + mr->length - length)
        return -EINVAL;
    struct mlx_bind_mw_req req = { .qpn = qp->qpn, .mwHandle = mw->mw_handle,
        .mrHandle = mr->mr_handle, .bindRkey = mw->rkey,
        .accessFlags = access_flags, .sendFlags = send_flags, .addr = addr,
        .length = length, .wrId = wr_id };
    struct mlx_bind_mw_resp resp = {};
    size_t outsz = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(qp->dev->conn,
        kMlxUCMethodBindMW, &req, sizeof(req), &resp, &outsz);
    if (kr != kIOReturnSuccess || outsz != sizeof(resp) || !resp.rkey) {
        SHIM_LOG("bind_mw failed: kr=0x%x out=%zu stage=%u status=0x%x mw=%u mr=%u qpn=%u old_rkey=0x%x addr=0x%llx len=%llu flags=0x%x",
                 kr, outsz, resp.stage, resp.status, mw->mw_handle, mr->mr_handle,
                 qp->qpn, mw->rkey, (unsigned long long)addr,
                 (unsigned long long)length, access_flags);
        return kr == kIOReturnBusy ? -EAGAIN :
               kr == kIOReturnNotPermitted ? -EPERM : -EIO;
    }
    mw->rkey = resp.rkey; if (new_rkey) *new_rkey = resp.rkey; return 0;
}

uint32_t rdma_mw_rkey(const rdma_mw *mw) { return mw ? mw->rkey : 0; }

int rdma_dereg_mr(struct rdma_mr *mr)
{
    if (!mr) return -EINVAL;
    uint32_t h = mr->mr_handle;
    kern_return_t kr = IOConnectCallStructMethod(
        mr->dev->conn, kMlxUCMethodDeregMR,
        &h, sizeof(h), NULL, 0);
    if (kr != kIOReturnSuccess) {
        SHIM_LOG("dereg_mr failed: 0x%x", kr);
        return kr == kIOReturnBusy ? -EBUSY : -EIO;
    }
    free(mr);
    return 0;
}

uint32_t rdma_mr_lkey(const struct rdma_mr *mr) { return mr ? mr->lkey : 0; }
uint32_t rdma_mr_rkey(const struct rdma_mr *mr) { return mr ? mr->rkey : 0; }

/* ---- address handle ---- */

rdma_ah *rdma_create_ah(rdma_pd *pd, const struct rdma_ah_attr *attr)
{
    if (!pd || !attr) return NULL;
    struct mlx_create_ah_req req = {};
    memcpy(req.dmac, attr->dmac, 6);
    memcpy(req.dgid, attr->dgid, 16);
    req.sgidIndex   = attr->sgid_index;
    req.hopLimit    = attr->hop_limit;
    req.trafficClass= attr->traffic_class;
    req.udpSport    = attr->udp_sport;
    req.portNum     = attr->port_num;
    req.ahType      = 0;  /* RoCE */
    struct mlx_create_ah_resp resp = {};
    size_t out = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(
        pd->dev->conn, kMlxUCMethodCreateAH, &req, sizeof(req), &resp, &out);
    if (kr != kIOReturnSuccess || out != sizeof(resp) || !resp.ahHandle) {
        if (kr == kIOReturnSuccess && resp.ahHandle)
            (void)IOConnectCallStructMethod(pd->dev->conn,
                                            kMlxUCMethodDestroyAH,
                                            &resp.ahHandle,
                                            sizeof(resp.ahHandle), NULL, 0);
        return NULL;
    }
    rdma_ah *ah = (rdma_ah *)calloc(1, sizeof(*ah));
    if (!ah) {
        (void)IOConnectCallStructMethod(pd->dev->conn, kMlxUCMethodDestroyAH,
                                        &resp.ahHandle, sizeof(resp.ahHandle),
                                        NULL, 0);
        return NULL;
    }
    ah->dev = pd->dev; ah->ah_handle = resp.ahHandle;
    return ah;
}

int rdma_destroy_ah(rdma_ah *ah)
{
    if (!ah) return -EINVAL;
    uint32_t h = ah->ah_handle;
    kern_return_t kr = IOConnectCallStructMethod(
        ah->dev->conn, kMlxUCMethodDestroyAH,
        &h, sizeof(h), NULL, 0);
    if (kr != kIOReturnSuccess) {
        SHIM_LOG("destroy_ah failed: 0x%x", kr);
        return -EIO;
    }
    free(ah);
    return 0;
}

/* ---- data path ---- */
/* Direct mode owns only this client's SQ/UAR/DB mappings. DEXT still checks
 * MR ownership and mirrors completion metadata through SyncFastPath. */
static int rdma_post_send_direct(rdma_qp *qp,
                                 const struct rdma_send_wr *wr,
                                 uint32_t count)
{
    uint32_t post_chunk = 64;
    const char *batch_env = getenv("MELONDMA_POST_BATCH");
    if (batch_env && strcmp(batch_env, "16") == 0) post_chunk = 16;
    if (!qp || !wr || !qp->direct_sq || !qp->sq_buf ||
        !qp->dev->uar_map || !qp->dev->db_map || !count ||
        count > post_chunk || qp->state != RDMA_QPS_RTS)
        return -EINVAL;
    /* The DEXT validates the same producer/consumer distance under its QP
     * lock. Keep posting until the local ring is full; CQ polling advances
     * sq_tail from the returned WQE counter. */
    if (qp->sq_head - qp->sq_tail + count > qp->sq_size)
        return -EAGAIN;

    struct mlx_sync_fast_path_req sync = { .count = count };
    uint64_t head = qp->sq_head;
    volatile uint8_t *last = NULL;
    for (uint32_t i = 0; i < count; i++) {
        if (wr[i].num_sge != 1 || !wr[i].sg_list ||
            (wr[i].send_flags & ~(RDMA_SEND_SIGNALED | RDMA_SEND_FENCE |
                                  RDMA_SEND_SOLICITED)))
            return -EINVAL;
        uint8_t opcode = wr[i].opcode == RDMA_WR_RDMA_WRITE ?
            MLX_OPCODE_RDMA_WRITE : wr[i].opcode == RDMA_WR_RDMA_READ ?
            MLX_OPCODE_RDMA_READ : MLX_OPCODE_SEND;
        uint32_t index = (uint32_t)(head + i) & (qp->sq_size - 1);
        last = (volatile uint8_t *)qp->sq_buf + index * qp->sq_stride;
        if (!mlxEncodeRcSendWqe64Flags((void *)(uintptr_t)last, qp->hw_qpn,
                                       (uint16_t)(head + i), opcode,
                                       wr[i].sg_list[0].addr,
                                       wr[i].sg_list[0].length,
                                       wr[i].sg_list[0].lkey,
                                       wr[i].remote_addr, wr[i].rkey,
                                       (wr[i].send_flags & RDMA_SEND_SIGNALED) != 0,
                                       (wr[i].send_flags & RDMA_SEND_FENCE) != 0,
                                       (wr[i].send_flags & RDMA_SEND_SOLICITED) != 0))
            return -EINVAL;
        sync.wr[i].qpn = qp->qpn;
        sync.wr[i].opcode = wr[i].opcode;
        sync.wr[i].wrId = wr[i].wr_id;
        sync.wr[i].sge.addr = wr[i].sg_list[0].addr;
        sync.wr[i].sge.length = wr[i].sg_list[0].length;
        sync.wr[i].sge.lkey = wr[i].sg_list[0].lkey;
        sync.wr[i].remoteAddr = wr[i].remote_addr;
        sync.wr[i].rkey = wr[i].rkey;
        sync.wr[i].sendFlags = wr[i].send_flags;
    }

    if (getenv("MELONDMA_DEBUG_WQE")) {
        uint32_t index = (uint32_t)head & (qp->sq_size - 1);
        const uint8_t *dump = (const uint8_t *)qp->sq_buf + index * qp->sq_stride;
        SHIM_LOG("WQE qpn=%u pi=%llu opcode=%u len=%u lkey=0x%x addr=0x%llx flags=0x%x",
                 qp->qpn, (unsigned long long)head, wr[0].opcode,
                 wr[0].sg_list[0].length, wr[0].sg_list[0].lkey,
                 (unsigned long long)wr[0].sg_list[0].addr, wr[0].send_flags);
        for (uint32_t off = 0; off < 64; off += 16)
            SHIM_LOG("WQE[%02u]: %02x %02x %02x %02x %02x %02x %02x %02x "
                     "%02x %02x %02x %02x %02x %02x %02x %02x", off,
                     dump[off], dump[off+1], dump[off+2], dump[off+3],
                     dump[off+4], dump[off+5], dump[off+6], dump[off+7],
                     dump[off+8], dump[off+9], dump[off+10], dump[off+11],
                     dump[off+12], dump[off+13], dump[off+14], dump[off+15]);
    }

    /* Validate and publish DEXT completion metadata before exposing the
     * final WQE to hardware. SyncFastPath never rings the doorbell. */
    kern_return_t kr = IOConnectCallStructMethod(
        qp->dev->conn, kMlxUCMethodSyncFastPath, &sync, sizeof(sync), NULL, 0);
    if (kr != kIOReturnSuccess)
        return kr == kIOReturnBusy ? -EAGAIN : -EIO;
    if (getenv("MELONDMA_DEBUG_POST"))
        SHIM_LOG("QP[%u] direct SQ synchronized count=%u", qp->qpn, count);

    rdma_memory_barrier();
    volatile uint32_t *db = (volatile uint32_t *)
        ((uint8_t *)qp->dev->db_map + qp->db_record_offset);
    db[1] = __builtin_bswap32((uint32_t)(head + count) & 0xffffu);
    rdma_memory_barrier();

    uint64_t doorbell = 0;
    memcpy(&doorbell, (const void *)(uintptr_t)last, sizeof(doorbell));
    volatile uint64_t *bf = (volatile uint64_t *)
        ((uint8_t *)qp->dev->uar_map + qp->bf_offset);
    *bf = doorbell;
    rdma_memory_barrier();

    qp->sq_head = head + count;
    qp->dev->stats.direct_send_batches++;
    qp->dev->stats.direct_send_wrs += count;
    qp->dev->stats.direct_doorbells++;
    return 0;
}

static int rdma_post_send_direct_sge(rdma_qp *qp,
                                     const struct rdma_send_wr *wr)
{
    if (!qp || !wr || !qp->direct_sq || !wr->sg_list ||
        wr->num_sge < 1 || wr->num_sge > RDMA_MAX_SGE ||
        (wr->send_flags & ~(RDMA_SEND_SIGNALED | RDMA_SEND_FENCE |
                            RDMA_SEND_SOLICITED)) ||
        qp->state != RDMA_QPS_RTS)
        return -EINVAL;
    uint8_t opcode = wr->opcode == RDMA_WR_RDMA_WRITE ? MLX_OPCODE_RDMA_WRITE :
                     wr->opcode == RDMA_WR_RDMA_READ ? MLX_OPCODE_RDMA_READ :
                     wr->opcode == RDMA_WR_RDMA_WRITE_IMM ? MLX_OPCODE_RDMA_WRITE_IMM :
                     wr->opcode == RDMA_WR_SEND_IMM ? MLX_OPCODE_SEND_IMM :
                     wr->opcode == RDMA_WR_LOCAL_INV ? MLX_OPCODE_LOCAL_INVAL : MLX_OPCODE_SEND;
    struct MlxRcSge sges[RDMA_MAX_SGE] = {};
    for (uint32_t i = 0; i < (uint32_t)wr->num_sge; i++) {
        sges[i].addr = wr->sg_list[i].addr;
        sges[i].length = wr->sg_list[i].length;
        sges[i].lkey = wr->sg_list[i].lkey;
    }
    uint8_t flat[256] = {};
    uint32_t ds = (opcode == MLX_OPCODE_SEND_IMM || opcode == MLX_OPCODE_RDMA_WRITE_IMM) ?
        mlxEncodeRcSendWqeImm(flat, sizeof(flat), qp->hw_qpn, (uint16_t)qp->sq_head,
                               opcode, sges, (uint32_t)wr->num_sge,
                               wr->remote_addr, wr->rkey, wr->imm_data,
                               (wr->send_flags & RDMA_SEND_SIGNALED) != 0,
                               (wr->send_flags & RDMA_SEND_FENCE) != 0,
                               (wr->send_flags & RDMA_SEND_SOLICITED) != 0) :
        mlxEncodeRcSendWqe(flat, sizeof(flat), qp->hw_qpn, (uint16_t)qp->sq_head,
                           opcode, sges, (uint32_t)wr->num_sge,
                           wr->remote_addr, wr->rkey,
                           (wr->send_flags & RDMA_SEND_SIGNALED) != 0,
                           (wr->send_flags & RDMA_SEND_FENCE) != 0,
                           (wr->send_flags & RDMA_SEND_SOLICITED) != 0);
    uint32_t span = ds ? (ds * 16u + 63u) / 64u : 0;
    if (!span || qp->sq_head - qp->sq_tail + span > qp->sq_size) return -EAGAIN;
    for (uint32_t i = 0; i < span; i++) {
        volatile uint8_t *slot = (volatile uint8_t *)qp->sq_buf +
                                  ((qp->sq_head + i) & (qp->sq_size - 1)) * qp->sq_stride;
        memcpy((void *)(uintptr_t)slot, flat + i * 64, 64);
    }
    struct mlx_sync_send_sge_req sync = { .qpn = qp->qpn, .opcode = wr->opcode,
        .wrId = wr->wr_id, .numSge = (uint32_t)wr->num_sge,
        .sendFlags = wr->send_flags, .remoteAddr = wr->remote_addr,
        .rkey = wr->rkey, .immData = wr->imm_data };
    for (uint32_t i = 0; i < (uint32_t)wr->num_sge; i++) {
        sync.sge[i].addr = wr->sg_list[i].addr;
        sync.sge[i].length = wr->sg_list[i].length;
        sync.sge[i].lkey = wr->sg_list[i].lkey;
    }
    kern_return_t kr = IOConnectCallStructMethod(qp->dev->conn,
        kMlxUCMethodSyncSendSge, &sync, sizeof(sync), NULL, 0);
    if (kr != kIOReturnSuccess) return kr == kIOReturnBusy ? -EAGAIN : -EIO;
    rdma_memory_barrier();
    volatile uint32_t *db = (volatile uint32_t *)
        ((uint8_t *)qp->dev->db_map + qp->db_record_offset);
    db[1] = __builtin_bswap32((uint32_t)(qp->sq_head + span) & 0xffffu);
    rdma_memory_barrier();
    uint64_t doorbell = 0; memcpy(&doorbell, flat, sizeof(doorbell));
    *(volatile uint64_t *)((uint8_t *)qp->dev->uar_map + qp->bf_offset) = doorbell;
    rdma_memory_barrier();
    qp->sq_head += span;
    qp->dev->stats.direct_send_wrs++;
    qp->dev->stats.direct_doorbells++;
    if (getenv("MELONDMA_DEBUG_POST"))
        SHIM_LOG("QP[%u] direct SQ synchronized SGE count=%u span=%u",
                 qp->qpn, wr->num_sge, span);
    return 0;
}

int rdma_post_local_inv(rdma_qp *qp, uint64_t wr_id, uint32_t rkey)
{
    if (!qp || !rkey) return -EINVAL;
    struct mlx_post_local_inv_req req = {
        .qpn = qp->qpn, .invalidateRkey = rkey, .wrId = wr_id,
        .sendFlags = MLX_UC_SEND_SIGNALED
    };
    kern_return_t kr = IOConnectCallStructMethod(qp->dev->conn,
        kMlxUCMethodPostLocalInv, &req, sizeof(req), NULL, 0);
    return kr == kIOReturnSuccess ? 0 : (kr == kIOReturnBusy ? -EAGAIN : -EIO);
}

int rdma_post_send_inline(rdma_qp *qp, uint64_t wr_id, uint32_t opcode,
                          const void *data, uint32_t len,
                          uint32_t imm_data, uint32_t send_flags)
{
    if (!qp || !data || !len || len > RDMA_MAX_INLINE_DATA ||
        (opcode != RDMA_WR_SEND && opcode != RDMA_WR_SEND_IMM) ||
        (send_flags & ~(RDMA_SEND_SIGNALED | RDMA_SEND_FENCE |
                        RDMA_SEND_SOLICITED | RDMA_SEND_INLINE)))
        return -EINVAL;
    struct mlx_post_send_inline_req req = {
        .qpn = qp->qpn,
        .opcode = opcode == RDMA_WR_SEND_IMM ? MLX_UC_WR_SEND_IMM : MLX_UC_WR_SEND,
        .wrId = wr_id,
        .inlineLen = len,
        .sendFlags = ((send_flags & RDMA_SEND_SIGNALED) ? MLX_UC_SEND_SIGNALED : 0) |
                     ((send_flags & RDMA_SEND_FENCE) ? MLX_UC_SEND_FENCE : 0) |
                     ((send_flags & RDMA_SEND_SOLICITED) ? MLX_UC_SEND_SOLICITED : 0) |
                     MLX_UC_SEND_INLINE,
        .immData = imm_data,
    };
    memcpy(req.inlineData, data, len);
    kern_return_t kr = IOConnectCallStructMethod(qp->dev->conn,
        kMlxUCMethodPostSendInline, &req, sizeof(req), NULL, 0);
    return kr == kIOReturnSuccess ? 0 : (kr == kIOReturnBusy ? -EAGAIN : -EIO);
}

int rdma_post_send_atomic(rdma_qp *qp, uint64_t wr_id, uint32_t opcode,
                          uint64_t remote_addr, uint32_t rkey,
                          uint64_t compare, uint64_t swap_add,
                          uint64_t result_addr, uint32_t result_lkey,
                          uint32_t send_flags)
{
    if (!qp || !remote_addr || !rkey || (remote_addr & 7) ||
        !result_addr || !result_lkey || (result_addr & 7) ||
        (opcode != RDMA_WR_ATOMIC_CS && opcode != RDMA_WR_ATOMIC_FA) ||
        (send_flags & ~RDMA_SEND_SIGNALED))
        return -EINVAL;
    struct mlx_post_send_atomic_req req = {
        .qpn = qp->qpn,
        .opcode = opcode == RDMA_WR_ATOMIC_CS ?
                  MLX_UC_WR_ATOMIC_CS : MLX_UC_WR_ATOMIC_FA,
        .wrId = wr_id,
        .remoteAddr = remote_addr,
        .rkey = rkey,
        .sendFlags = (send_flags & RDMA_SEND_SIGNALED) ? MLX_UC_SEND_SIGNALED : 0,
        .compare = compare,
        .swapAdd = swap_add,
        .resultAddr = result_addr,
        .resultLkey = result_lkey,
    };
    kern_return_t kr = IOConnectCallStructMethod(qp->dev->conn,
        kMlxUCMethodPostSendAtomic, &req, sizeof(req), NULL, 0);
    return kr == kIOReturnSuccess ? 0 : (kr == kIOReturnBusy ? -EAGAIN : -EIO);
}

int rdma_post_send_sge(rdma_qp *qp, const struct rdma_send_wr *wr)
{
    if (qp && wr && qp->direct_sq && wr->num_sge >= 1)
        return rdma_post_send_direct_sge(qp, wr);
    if (!qp || !wr || !wr->sg_list || !wr->num_sge ||
        wr->num_sge > RDMA_MAX_SGE || qp->direct_sq ||
        (wr->send_flags & ~(RDMA_SEND_SIGNALED | RDMA_SEND_FENCE |
                            RDMA_SEND_SOLICITED))) return -EINVAL;
    struct mlx_post_send_sge_req req = {
        .qpn = qp->qpn, .opcode = wr->opcode, .wrId = wr->wr_id,
        .numSge = wr->num_sge, .sendFlags = wr->send_flags,
        .remoteAddr = wr->remote_addr, .rkey = wr->rkey,
        .immData = wr->imm_data,
    };
    for (uint32_t i = 0; i < wr->num_sge; i++) {
        req.sge[i].addr = wr->sg_list[i].addr;
        req.sge[i].length = wr->sg_list[i].length;
        req.sge[i].lkey = wr->sg_list[i].lkey;
    }
    kern_return_t kr = IOConnectCallStructMethod(qp->dev->conn,
        kMlxUCMethodPostSendSge, &req, sizeof(req), NULL, 0);
    if (kr != kIOReturnSuccess && getenv("MELONDMA_DEBUG_POST"))
        SHIM_LOG("post_send_sge failed kr=0x%x qpn=%u opcode=%u lkey=0x%x addr=0x%llx len=%u",
                 kr, qp->qpn, wr->opcode, wr->sg_list[0].lkey,
                 (unsigned long long)wr->sg_list[0].addr, wr->sg_list[0].length);
    return kr == kIOReturnSuccess ? 0 : (kr == kIOReturnBusy ? -EAGAIN : -EIO);
}

int rdma_post_send(rdma_qp *qp, const struct rdma_send_wr *wr)
{
    return wr && (wr->num_sge > 1 || wr->opcode == RDMA_WR_SEND_IMM ||
                  wr->opcode == RDMA_WR_RDMA_WRITE_IMM) ?
        rdma_post_send_sge(qp, wr) : rdma_post_send_batch(qp, wr, 1);
}

int rdma_post_send_batch(rdma_qp *qp, const struct rdma_send_wr *wr,
                         uint32_t count)
{
    if (!qp || !wr || !count || count > RDMA_MAX_POST_BATCH)
        return -EINVAL;
    if (qp->direct_sq) {
        uint32_t post_chunk = 64;
        const char *batch_env = getenv("MELONDMA_POST_BATCH");
        if (batch_env && strcmp(batch_env, "16") == 0) post_chunk = 16;
        for (uint32_t base = 0; base < count; base += post_chunk) {
            uint32_t chunk = count - base;
            if (chunk > post_chunk) chunk = post_chunk;
            int rc = rdma_post_send_direct(qp, wr + base, chunk);
            if (rc) return rc;
        }
        return 0;
    }
    for (uint32_t base = 0; base < count; base += RDMA_POST_CHUNK) {
        qp->dev->stats.fallback_send_batches++;
        uint32_t chunk = count - base;
        if (chunk > RDMA_POST_CHUNK) chunk = RDMA_POST_CHUNK;
        struct mlx_post_send_batch_req req = { .count = chunk };
        for (uint32_t i = 0; i < chunk; i++) {
            uint32_t index = base + i;
            if (wr[index].num_sge != 1 || !wr[index].sg_list ||
                (wr[index].send_flags & ~(RDMA_SEND_SIGNALED | RDMA_SEND_FENCE |
                                          RDMA_SEND_SOLICITED)))
                return -EINVAL;
            req.wr[i].qpn = qp->qpn;
            req.wr[i].opcode = wr[index].opcode;
            req.wr[i].wrId = wr[index].wr_id;
            req.wr[i].sge.addr = wr[index].sg_list[0].addr;
            req.wr[i].sge.length = wr[index].sg_list[0].length;
            req.wr[i].sge.lkey = wr[index].sg_list[0].lkey;
            req.wr[i].remoteAddr = wr[index].remote_addr;
            req.wr[i].rkey = wr[index].rkey;
            req.wr[i].sendFlags = wr[index].send_flags;
        }
        if (getenv("MELONDMA_DEBUG_WQE")) {
            SHIM_LOG("WQE(kernel-mediated) qpn=%u count=%u opcode=%u len=%u "
                     "lkey=0x%x addr=0x%llx raddr=0x%llx rkey=0x%x flags=0x%x",
                     qp->qpn, chunk, req.wr[0].opcode, req.wr[0].sge.length,
                     req.wr[0].sge.lkey, (unsigned long long)req.wr[0].sge.addr,
                     (unsigned long long)req.wr[0].remoteAddr, req.wr[0].rkey,
                     req.wr[0].sendFlags);
        }
        kern_return_t kr = IOConnectCallStructMethod(
            qp->dev->conn, kMlxUCMethodPostSendBatch,
            &req, sizeof(req), NULL, 0);
        if (kr != kIOReturnSuccess) {
            if (getenv("MELONDMA_DEBUG_WQE"))
                SHIM_LOG("WQE(kernel-mediated) qpn=%u FAILED kr=0x%x", qp->qpn, kr);
            return kr == kIOReturnBusy ? -EAGAIN : -EIO;
        }
    }
    return 0;
}

static int rdma_post_recv_direct_sge(rdma_qp *qp,
                                     const struct rdma_recv_wr *wr);

int rdma_post_recv_sge(rdma_qp *qp, const struct rdma_recv_wr *wr)
{
    if (qp && wr && qp->direct_rq && wr->num_sge >= 2)
        return rdma_post_recv_direct_sge(qp, wr);
    if (!qp || !wr || !wr->sg_list || !wr->num_sge ||
        wr->num_sge > RDMA_MAX_SGE || qp->direct_rq) return -EINVAL;
    struct mlx_post_recv_sge_req req = {
        .qpn = qp->qpn, .numSge = wr->num_sge, .wrId = wr->wr_id,
    };
    for (uint32_t i = 0; i < wr->num_sge; i++) {
        req.sge[i].addr = wr->sg_list[i].addr;
        req.sge[i].length = wr->sg_list[i].length;
        req.sge[i].lkey = wr->sg_list[i].lkey;
    }
    kern_return_t kr = IOConnectCallStructMethod(qp->dev->conn,
        kMlxUCMethodPostRecvSge, &req, sizeof(req), NULL, 0);
    return kr == kIOReturnSuccess ? 0 : (kr == kIOReturnBusy ? -EAGAIN : -EIO);
}

int rdma_post_recv(rdma_qp *qp, uint64_t wr_id, const struct rdma_sge *sg_list,
                   uint32_t num_sge)
{
    struct rdma_recv_wr wr = {
        .wr_id = wr_id, .num_sge = num_sge, .sg_list = sg_list
    };
    return rdma_post_recv_batch(qp, &wr, 1);
}

static int rdma_post_recv_direct_sge(rdma_qp *qp,
                                     const struct rdma_recv_wr *wr)
{
    if (!qp || !wr || !qp->direct_rq || !wr->sg_list ||
        wr->num_sge < 2 || wr->num_sge > RDMA_MAX_SGE ||
        qp->rq_head - qp->rq_tail + 1 > qp->rq_size)
        return -EINVAL;
    uint32_t index = (uint32_t)qp->rq_head & (qp->rq_size - 1);
    volatile uint8_t *wqe = (volatile uint8_t *)qp->rq_buf + index * qp->sq_stride;
    struct MlxRcSge sges[RDMA_MAX_SGE] = {};
    struct mlx_sync_recv_sge_req sync = { .qpn = qp->qpn,
        .numSge = (uint32_t)wr->num_sge, .wrId = wr->wr_id };
    for (uint32_t i = 0; i < (uint32_t)wr->num_sge; i++) {
        sges[i].addr = sync.sge[i].addr = wr->sg_list[i].addr;
        sges[i].length = sync.sge[i].length = wr->sg_list[i].length;
        sges[i].lkey = sync.sge[i].lkey = wr->sg_list[i].lkey;
    }
    if (!mlxEncodeRecvWqeSge((void *)(uintptr_t)wqe, 64, sges,
                             (uint32_t)wr->num_sge)) return -EINVAL;
    kern_return_t kr = IOConnectCallStructMethod(qp->dev->conn,
        kMlxUCMethodSyncRecvSge, &sync, sizeof(sync), NULL, 0);
    if (kr != kIOReturnSuccess) {
        SHIM_LOG("direct multi-SGE receive sync failed: 0x%x", kr);
        return kr == kIOReturnBusy ? -EAGAIN : -EIO;
    }
    rdma_memory_barrier();
    volatile uint32_t *db = (volatile uint32_t *)
        ((uint8_t *)qp->dev->db_map + qp->db_record_offset);
    db[0] = __builtin_bswap32((uint32_t)(qp->rq_head + 1) & 0xffffu);
    rdma_memory_barrier();
    qp->rq_head++;
    qp->dev->stats.direct_recv_wrs++;
    return 0;
}

int rdma_post_recv_batch(rdma_qp *qp, const struct rdma_recv_wr *wr,
                         uint32_t count)
{
    if (!qp || !wr || !count || count > RDMA_MAX_POST_BATCH)
        return -EINVAL;
    if (qp->direct_rq && count == 1 && wr->num_sge >= 2)
        return rdma_post_recv_direct_sge(qp, wr);
    if (qp->direct_rq) {
        for (uint32_t base = 0; base < count; base += RDMA_POST_CHUNK) {
            uint32_t chunk = count - base;
            if (chunk > RDMA_POST_CHUNK) chunk = RDMA_POST_CHUNK;
            if (qp->rq_head - qp->rq_tail + chunk > qp->rq_size)
                return -EAGAIN;
            struct mlx_sync_recv_fast_path_req req = { .count = chunk };
            uint64_t head = qp->rq_head;
            for (uint32_t i = 0; i < chunk; i++) {
                if (wr[base + i].num_sge != 1 || !wr[base + i].sg_list)
                    return -EINVAL;
                uint32_t idx = (uint32_t)(head + i) & (qp->rq_size - 1);
                volatile uint8_t *wqe = (volatile uint8_t *)qp->rq_buf +
                                         (uint64_t)idx * qp->sq_stride;
                if (!mlxEncodeRecvWqe64((void *)(uintptr_t)wqe,
                                         wr[base + i].sg_list[0].addr,
                                         wr[base + i].sg_list[0].length,
                                         wr[base + i].sg_list[0].lkey))
                    return -EINVAL;
                req.wr[i].qpn = qp->qpn;
                req.wr[i].wrId = wr[base + i].wr_id;
                req.wr[i].sge.addr = wr[base + i].sg_list[0].addr;
                req.wr[i].sge.length = wr[base + i].sg_list[0].length;
                req.wr[i].sge.lkey = wr[base + i].sg_list[0].lkey;
            }
            kern_return_t kr = IOConnectCallStructMethod(
                qp->dev->conn, kMlxUCMethodSyncRecvFastPath,
                &req, sizeof(req), NULL, 0);
            if (kr != kIOReturnSuccess) return -EIO;
            rdma_memory_barrier();
            volatile uint32_t *db = (volatile uint32_t *)
                ((uint8_t *)qp->dev->db_map + qp->db_record_offset);
            db[0] = __builtin_bswap32((uint32_t)(head + chunk) & 0xffffu);
            rdma_memory_barrier();
            qp->rq_head = head + chunk;
            qp->dev->stats.direct_recv_batches++;
            qp->dev->stats.direct_recv_wrs += chunk;
        }
        return 0;
    }
    for (uint32_t base = 0; base < count; base += RDMA_POST_CHUNK) {
        qp->dev->stats.fallback_recv_batches++;
        uint32_t chunk = count - base;
        if (chunk > RDMA_POST_CHUNK) chunk = RDMA_POST_CHUNK;
        struct mlx_post_recv_batch_req req = { .count = chunk };
        for (uint32_t i = 0; i < chunk; i++) {
            uint32_t index = base + i;
            if (wr[index].num_sge != 1 || !wr[index].sg_list)
                return -EINVAL;
            req.wr[i].qpn = qp->qpn;
            req.wr[i].wrId = wr[index].wr_id;
            req.wr[i].sge.addr = wr[index].sg_list[0].addr;
            req.wr[i].sge.length = wr[index].sg_list[0].length;
            req.wr[i].sge.lkey = wr[index].sg_list[0].lkey;
        }
        kern_return_t kr = IOConnectCallStructMethod(
            qp->dev->conn, kMlxUCMethodPostRecvBatch,
            &req, sizeof(req), NULL, 0);
        if (kr != kIOReturnSuccess)
            return kr == kIOReturnBusy ? -EAGAIN : -EIO;
    }
    return 0;
}

/* ---- async events ---- */

int rdma_get_async_event(rdma_device *dev, struct rdma_async_event *event)
{
    if (!dev || !event) return -EINVAL;
    struct mlx_async_event ev = {};
    size_t out = sizeof(ev);
    kern_return_t kr = IOConnectCallStructMethod(
        dev->conn, kMlxUCMethodGetAsyncEvent, NULL, 0, &ev, &out);
    if (kr != kIOReturnSuccess) return -EAGAIN;
    if (out != sizeof(ev)) return -EPROTO;
    event->event_type     = ev.eventType;
    event->element_type   = ev.elementType;
    event->element_handle = ev.elementHandle;
    return 0;
}
