/*
 * librdma_shim.c — userspace RDMA shim over the MlxRDMA DriverKit DEXT.
 *
 * Control plane: IOConnectCallStructMethod → MlxUserClient::ExternalMethod,
 *   selector table and POD structs from MlxUCIO.h (shared with the DEXT).
 * Data path: capable trusted clients use isolated direct SQ/RQ/CQ/UAR/DB
 * mappings by default.  The validated kernel ExternalMethods remain the
 * compatibility and diagnostic fallback.
 *
 * Compiles against the macOS SDK (userspace), not DriverKit.
 */
#include "librdma_shim.h"
#include "MlxUCIO.h"
#include "MlxWQE.hpp"
#include "MlxServiceMatch.h"

#include <stdlib.h>
#include <os/lock.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>

#define SHIM_LOG(fmt, ...)  fprintf(stderr, "librdma_shim: " fmt "\n", ##__VA_ARGS__)
#define RDMA_QP_HASH_SIZE 256u
#define RDMA_QP_SHADOW_INTERVAL 16u

enum rdma_qp_shadow_field {
    RDMA_QP_SHADOW_SQ_HEAD = 1u << 0,
    RDMA_QP_SHADOW_SQ_TAIL = 1u << 1,
    RDMA_QP_SHADOW_RQ_HEAD = 1u << 2,
    RDMA_QP_SHADOW_RQ_TAIL = 1u << 3,
};

/* Apple Silicon is coherent with PCIe DMA, but the CQ owner byte and the
 * WQE/doorbell publication sequence still require device-scope ordering.
 * Use directional outer-shareable barriers instead of a full bidirectional
 * __sync_synchronize() on every step of the steady path. */
static inline void rdma_dma_read_barrier(void)
{
#if defined(__aarch64__)
    __asm__ volatile("dmb oshld" ::: "memory");
#else
    __sync_synchronize();
#endif
}

static inline void rdma_dma_write_barrier(void)
{
#if defined(__aarch64__)
    __asm__ volatile("dmb oshst" ::: "memory");
#else
    __sync_synchronize();
#endif
}

/* A blue-flame write goes to a page mapped write-combining, which is Normal-NC
 * on arm64, not Device memory. A dmb orders stores against each other but does
 * NOT push the combining buffer out, so a WQE written into the register can sit
 * there past the point where the device was told to look. The buffer is flushed
 * with dsb, and it is needed on BOTH sides of the write: before, so nothing
 * still in flight merges into this register's buffer, and after, so the WQE
 * actually reaches the device. This is rdma-core's mmio_wc_start /
 * mmio_flush_writes on aarch64. */
static inline void rdma_mmio_flush(void)
{
#if defined(__aarch64__)
    __asm__ volatile("dsb st" ::: "memory");
#else
    __sync_synchronize();
#endif
}

/* ---- internal object layouts ---- */

/* One blue-flame register, as the hardware sees it. The toggle belongs here
 * and not to a queue pair: it alternates the doorbell between the two halves
 * of the register so that consecutive doorbells never land on the same
 * address, and a write-combining mapping is free to merge two stores that do.
 * Two QPs sharing a register with private toggles can produce exactly that
 * pair of stores, and the merged doorbell is a work request the card never
 * fetches. rdma-core keeps the toggle in the shared `mlx5_bf` for the same
 * reason. The lock is taken only by QPs that share, so a private register
 * keeps the lock-free path and its numbers. */
struct rdma_bf_reg {
    uint32_t        offset;      /* flat bf_offset; identifies the register */
    uint32_t        toggle;
    os_unfair_lock  lock;
    int             in_use;
};

/* Four UAR pages at four registers each is the ceiling the driver allocates
 * from; the table is sized past it so a firmware that reports more registers
 * per page still resolves rather than silently falling back to a private
 * toggle. */
#define RDMA_BF_REG_SLOTS 64u

struct rdma_device {
    io_connect_t  conn;
    char          name[64];
    /* One entry per UAR page / doorbell page the driver has given this
     * client. Slot 0 and page 0 are mapped when the fast path is enabled; the
     * rest appear lazily, the first time a QP or CQ lands on them. */
    void         *uar_map[MLX_CLIENT_MAX_UAR];
    size_t        uar_map_size;
    unsigned      uar_map_options;
    void         *db_map[MLX_CLIENT_MAX_DB_PAGES];
    size_t        db_map_size;
    struct rdma_qp *qps;
    struct rdma_cq *cqs;
    struct rdma_qp *qpn_buckets[RDMA_QP_HASH_SIZE];
    struct rdma_mr *mrs;
    uint32_t      abi_features;
    struct rdma_fast_path_stats stats;
    /* Registers this client's QPs have landed on. Sharing only ever happens
     * between QPs of one client, and a client is one process, so a
     * process-local table is the whole scope of the problem. */
    struct rdma_bf_reg bf_regs[RDMA_BF_REG_SLOTS];
    os_unfair_lock     bf_table_lock;
};

static inline void rdma_stat_add(uint64_t *counter, uint64_t value)
{
    __atomic_fetch_add(counter, value, __ATOMIC_RELAXED);
}

static inline int rdma_env_default_on(const char *name)
{
    const char *value = getenv(name);
    return !value || strcmp(value, "0") != 0;
}

struct rdma_pd {
    rdma_device *dev;
    uint32_t     pd;             /* firmware PD index (fixed to 1 for MVP) */
};

struct rdma_cq {
    rdma_device *dev;
    /* Set when a QP drawing from a shared receive queue is created on this CQ.
     * That queue, its free list and its work-request ids live in the DEXT, so
     * userspace cannot decode those completions. Falling back per CQE would
     * interleave two decoders over one ring; taking the whole CQ through the
     * kernel keeps a single reader. */
    int          kernel_poll_only;
    uint32_t     cq_handle;
    volatile struct MlxCqe64 *cqe_buf;   /* mapped ring */
    uint32_t     log_size;
    uint32_t     cqe_size;
    uint32_t     consumer_index;
    uint32_t     published_consumer_index; /* last CI written to HW DB record */
    uint32_t     synced_consumer_index;    /* last CI known by the DEXT object */
    uint32_t     consumer_batch;           /* lazy DB publication threshold */
    uint32_t     db_record_offset;
    uint8_t     *db_rec;        /* resolved doorbell record */
    uint32_t     depth;
    struct rdma_cq *next;
};

struct rdma_qp {
    rdma_device *dev;
    rdma_pd     *pd;
    uint32_t     qpn;          /* opaque client token (ABI v2) */
    uint32_t     hw_qpn;       /* raw firmware QPN: peer exchange + WQE ctrl seg */
    /* Non-zero when this QP draws receives from a shared queue. That queue and
     * its free list live in the DEXT, like every other receive ring here, so
     * the direct poll has no metadata for it and must defer to the kernel. */
    uint32_t     srqn;
    int          is_ud;   /* datagram queue pair */
    int          is_uc;   /* unreliable connected queue pair */
    uint32_t     state;
    uint32_t     sq_size;
    uint32_t     rq_size;
    void        *sq_buf;
    void        *rq_buf;
    uint32_t     bf_offset;      /* flat, as the driver reported it */
    uint8_t     *bf_reg;        /* resolved: mapping base + in-page offset */
    uint8_t     *db_rec;        /* resolved doorbell record */
    int          bf_shared;   /* doorbell register shared with another QP */
    uint32_t     bf_buf_size;
    uint32_t     bf_toggle;   /* private toggle; used when nothing shares */
    /* Points at bf_toggle when this QP owns its register outright, and at the
     * register's own toggle when it does not. bf_lock is NULL in the first
     * case, so the common path costs one predicted branch. */
    uint32_t    *bf_togglep;
    os_unfair_lock *bf_lock;
    /* Serialises posts on this queue pair. rdma-core's mlx5 provider takes a
     * per-queue lock unless the QP is created single threaded, and the
     * libibverbs compatibility layer here presents rdma-core semantics, so
     * this defaults on. MELONDMA_SINGLE_THREADED_QP=1 turns it off for a
     * caller that does its own serialisation and wants the last nanoseconds
     * back. */
    os_unfair_lock post_lock;
    int          post_serialize;
    uint32_t     db_record_offset;
    uint32_t     sq_stride;
    uint64_t     sq_head;
    uint64_t     sq_tail;
    uint64_t     rq_head;
    uint64_t     rq_tail;
    uint64_t     shadow_sq_head;
    uint64_t     shadow_sq_tail;
    uint64_t     shadow_rq_head;
    uint64_t     shadow_rq_tail;
    int          direct_sq;
    int          direct_rq;
    int          direct_cq;
    int          trusted_fast_path;
    /* Direct-CQ decode state: per-slot wr_id/opcode/span captured at post
     * time so poll_cq can reconstruct the WC without a kernel round trip. */
    uint64_t    *sq_wrid;
    uint8_t     *sq_opcode;   /* rdma_wr_opcode */
    uint8_t     *sq_span;
    uint64_t    *sq_atomic_result;
    uint64_t    *rq_wrid;
    uint64_t    *rq_sge_addr;  /* first recv SGE addr per slot, for scatter-to-CQE */
    uint8_t      wqe_template[3][8][64]; /* SEND/WRITE/READ x CE/fence/solicit */
    uint8_t      wqe_template_valid[3];
    struct rdma_qp *next;
    struct rdma_qp *hash_next;
};

/* Copy a complete WQE to one Blue Flame buffer. The UAR mapping is WC and
 * every store is volatile MMIO; the device-scope barrier flushes the group
 * before selecting the other half of the BF register for the next post. */
/* Map one page of a client pool on demand and cache it. The driver only
 * exports slots it has actually given this client, so an index it does not
 * own comes back refused rather than as somebody else's page. */
static void *rdma_map_pool_page(rdma_device *dev, uint32_t kind,
                                uint32_t index, size_t expect,
                                unsigned options)
{
    mach_vm_address_t addr = 0;
    mach_vm_size_t size = 0;
    kern_return_t kr = IOConnectMapMemory64(
        dev->conn, MLX_UC_MEM_TYPE(kind, index), mach_task_self(),
        &addr, &size, options);
    if (kr != kIOReturnSuccess) return NULL;
    if (expect && size != (mach_vm_size_t)expect) {
        (void)IOConnectUnmapMemory(dev->conn, MLX_UC_MEM_TYPE(kind, index),
                                   mach_task_self(), addr);
        return NULL;
    }
    return (void *)(uintptr_t)addr;
}

static void *rdma_uar_slot(rdma_device *dev, uint32_t slot)
{
    if (!dev || slot >= MLX_CLIENT_MAX_UAR) return NULL;
    if (!dev->uar_map[slot])
        dev->uar_map[slot] = rdma_map_pool_page(dev, kMlxUCMemKindUar, slot,
                                                dev->uar_map_size,
                                                dev->uar_map_options);
    return dev->uar_map[slot];
}

static void *rdma_db_page(rdma_device *dev, uint32_t page)
{
    if (!dev || page >= MLX_CLIENT_MAX_DB_PAGES) return NULL;
    if (!dev->db_map[page])
        dev->db_map[page] = rdma_map_pool_page(dev, kMlxUCMemKindDbRecord,
                                               page, dev->db_map_size,
                                               kIOMapAnywhere);
    return dev->db_map[page];
}

/* The register write and the toggle that follows it are one indivisible step:
 * a second poster that reads the toggle before this one advances it aims its
 * doorbell at the same half. Only QPs that share a register take the lock.
 */
static int rdma_blue_flame_post(rdma_qp *qp, const void *wqe, uint32_t bytes)
{
    if (!qp || !wqe || bytes <= sizeof(uint64_t) || (bytes & 63u) ||
        !qp->bf_buf_size || bytes > qp->bf_buf_size)
        return 0;
    const uint8_t *src = (const uint8_t *)wqe;
    /* The private and shared cases are written out separately on purpose. A QP
     * that owns its register runs exactly the code it ran before any of this
     * existed: the toggle is a field, not a load through a pointer, and there
     * is no lock. Folding the two together cost 0.25 us of post time for the
     * common case, which is most of the post path. */
    if (!qp->bf_lock) {
        volatile uint64_t *dst =
            (volatile uint64_t *)(qp->bf_reg + qp->bf_toggle);
        rdma_mmio_flush();
        for (uint32_t off = 0; off < bytes; off += sizeof(uint64_t)) {
            uint64_t word;
            memcpy(&word, src + off, sizeof(word));
            dst[off / sizeof(uint64_t)] = word;
        }
        rdma_mmio_flush();
        qp->bf_toggle ^= qp->bf_buf_size;
    } else {
        os_unfair_lock_lock(qp->bf_lock);
        volatile uint64_t *dst =
            (volatile uint64_t *)(qp->bf_reg + *qp->bf_togglep);
        rdma_mmio_flush();
        for (uint32_t off = 0; off < bytes; off += sizeof(uint64_t)) {
            uint64_t word;
            memcpy(&word, src + off, sizeof(word));
            dst[off / sizeof(uint64_t)] = word;
        }
        rdma_mmio_flush();
        *qp->bf_togglep ^= qp->bf_buf_size;
        os_unfair_lock_unlock(qp->bf_lock);
    }
    rdma_stat_add(&qp->dev->stats.blue_flame_wqes, 1);
    return 1;
}

static void rdma_ring_send_doorbell(rdma_qp *qp, uint64_t doorbell)
{
    /* Same split as the blue-flame post above, and for the same reason: a QP
     * that owns its register pays nothing for the fact that sharing exists. */
    if (!qp->bf_lock) {
        *(volatile uint64_t *)(qp->bf_reg + qp->bf_toggle) = doorbell;
        /* The barrier has to match the mapping, and the mapping follows blue
         * flame: with it the UAR page is write-combining and the store can sit
         * in a combining buffer, which only dsb pushes out; without it the
         * page is Device memory, where the store is not buffered and dmb is
         * both sufficient and materially cheaper on this hot path. */
        if (qp->bf_buf_size) rdma_mmio_flush();
        else                 rdma_dma_write_barrier();
        if (qp->bf_buf_size)
            qp->bf_toggle ^= qp->bf_buf_size;
        return;
    }
    os_unfair_lock_lock(qp->bf_lock);
    *(volatile uint64_t *)(qp->bf_reg + *qp->bf_togglep) = doorbell;
    if (qp->bf_buf_size) rdma_mmio_flush();
    else                 rdma_dma_write_barrier();
    if (qp->bf_buf_size)
        *qp->bf_togglep ^= qp->bf_buf_size;
    os_unfair_lock_unlock(qp->bf_lock);
}

/* Resolve the register this QP posts through. A QP the driver reported as
 * sharing gets the register's own toggle and its lock; every other QP keeps
 * the private toggle it already had and no lock at all. Falling back to the
 * private toggle when the table is full is deliberate: it is what the code did
 * before this existed, so a full table is no worse than yesterday. */
static void rdma_bf_bind(rdma_qp *qp)
{
    qp->bf_togglep = &qp->bf_toggle;
    qp->bf_lock = NULL;
    if (!qp->bf_shared || !qp->dev) return;
    os_unfair_lock_lock(&qp->dev->bf_table_lock);
    struct rdma_bf_reg *free_slot = NULL;
    for (uint32_t i = 0; i < RDMA_BF_REG_SLOTS; i++) {
        struct rdma_bf_reg *r = &qp->dev->bf_regs[i];
        if (r->in_use && r->offset == qp->bf_offset) {
            qp->bf_togglep = &r->toggle;
            qp->bf_lock = &r->lock;
            os_unfair_lock_unlock(&qp->dev->bf_table_lock);
            return;
        }
        if (!r->in_use && !free_slot) free_slot = r;
    }
    if (free_slot) {
        free_slot->in_use = 1;
        free_slot->offset = qp->bf_offset;
        /* Seed from the QP so the first shared post continues the alternation
         * this register was already on rather than restarting it. */
        free_slot->toggle = qp->bf_toggle;
        qp->bf_togglep = &free_slot->toggle;
        qp->bf_lock = &free_slot->lock;
    }
    os_unfair_lock_unlock(&qp->dev->bf_table_lock);
}

static inline uint32_t rdma_qpn_hash(uint32_t qpn)
{
    return (qpn * 2654435761u) & (RDMA_QP_HASH_SIZE - 1u);
}

static rdma_qp *rdma_find_qp_hw(rdma_device *dev, uint32_t hw_qpn)
{
    if (!dev || !hw_qpn) return NULL;
    for (rdma_qp *qp = dev->qpn_buckets[rdma_qpn_hash(hw_qpn)]; qp;
         qp = qp->hash_next)
        if (qp->hw_qpn == hw_qpn) return qp;
    return NULL;
}

static rdma_qp *rdma_find_qp_token(rdma_device *dev, uint32_t token)
{
    if (!dev || !token) return NULL;
    for (rdma_qp *qp = dev->qps; qp; qp = qp->next)
        if (qp->qpn == token) return qp;
    return NULL;
}

static rdma_cq *rdma_find_cq_token(rdma_device *dev, uint32_t token)
{
    if (!dev || !token) return NULL;
    for (rdma_cq *cq = dev->cqs; cq; cq = cq->next)
        if (cq->cq_handle == token) return cq;
    return NULL;
}

/* Publish the QP producer/consumer shadow state into the per-QP DB-record
 * slot (docs/shared-page-fast-path.md).  Hardware doorbell records occupy the
 * first 8 bytes; the shadow state starts at MLX_QP_SHADOW_OFFSET and is read
 * by the DEXT once the trusted fast path is enabled.  Writing it is always
 * safe: the rest of the 128-byte slot is otherwise zeroed and unused. */
static inline void rdma_qp_publish_shadow_force(rdma_qp *qp)
{
    if (!qp || !qp->dev || !qp->db_rec ||
        (uint64_t)mlxFlatWithin(qp->db_record_offset,
                                MLX_CLIENT_DB_PAGE_SIZE) +
            MLX_QP_SHADOW_OFFSET + sizeof(struct mlx_qp_shadow) >
                qp->dev->db_map_size)
        return;
    struct mlx_qp_shadow *sh = (struct mlx_qp_shadow *)
        (qp->db_rec + MLX_QP_SHADOW_OFFSET);
    /* Seqlock write side.  post (worker thread) and poll (recv thread) can
     * publish the same QP concurrently, so the sequence must be acquired with
     * a CAS, not a load-then-store: two racers would otherwise interleave
     * their odd/even stores and hand the DEXT reader a torn snapshot. */
    uint64_t seq;
    for (;;) {
        seq = __atomic_load_n(&sh->sequence, __ATOMIC_ACQUIRE);
        if (seq & 1u) continue;   /* another writer holds it — spin */
        if (__atomic_compare_exchange_n(&sh->sequence, &seq, seq + 1u,
                                        false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            break;
    }
    sh->sq_head = qp->sq_head;
    sh->sq_tail = qp->sq_tail;
    sh->rq_head = qp->rq_head;
    sh->rq_tail = qp->rq_tail;
    __atomic_store_n(&sh->sequence, seq + 2u, __ATOMIC_RELEASE);
    __atomic_store_n(&qp->shadow_sq_head, qp->sq_head, __ATOMIC_RELAXED);
    __atomic_store_n(&qp->shadow_sq_tail, qp->sq_tail, __ATOMIC_RELAXED);
    __atomic_store_n(&qp->shadow_rq_head, qp->rq_head, __ATOMIC_RELAXED);
    __atomic_store_n(&qp->shadow_rq_tail, qp->rq_tail, __ATOMIC_RELAXED);
    rdma_stat_add(&qp->dev->stats.shadow_publications, 1);
}

/* The DEXT reads this snapshot only on control, teardown and exceptional
 * fallback paths. Keep the steady data path off the shared seqlock while
 * bounding the stale window to a small fraction of the minimum queue depth. */
static inline void rdma_qp_publish_shadow_maybe(rdma_qp *qp, uint32_t fields)
{
    if (!qp || !qp->trusted_fast_path) return;
    bool due = false;
    if (fields & RDMA_QP_SHADOW_SQ_HEAD)
        due = qp->sq_head - __atomic_load_n(&qp->shadow_sq_head,
                                            __ATOMIC_RELAXED) >=
              RDMA_QP_SHADOW_INTERVAL;
    if (!due && (fields & RDMA_QP_SHADOW_SQ_TAIL))
        due = qp->sq_tail - __atomic_load_n(&qp->shadow_sq_tail,
                                            __ATOMIC_RELAXED) >=
              RDMA_QP_SHADOW_INTERVAL;
    if (!due && (fields & RDMA_QP_SHADOW_RQ_HEAD))
        due = qp->rq_head - __atomic_load_n(&qp->shadow_rq_head,
                                            __ATOMIC_RELAXED) >=
              RDMA_QP_SHADOW_INTERVAL;
    if (!due && (fields & RDMA_QP_SHADOW_RQ_TAIL))
        due = qp->rq_tail - __atomic_load_n(&qp->shadow_rq_tail,
                                            __ATOMIC_RELAXED) >=
              RDMA_QP_SHADOW_INTERVAL;
    if (due) rdma_qp_publish_shadow_force(qp);
}

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
    dev->abi_features = abi.features;
    return dev;
}

void rdma_close_device(rdma_device *dev)
{
    if (!dev) return;
    rdma_unmap_fast_path(dev);
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
    /* The UAR page is whatever geometry the driver negotiated with firmware,
     * so accept any whole number of 4 KiB adapter pages up to a host page
     * rather than the 4 KiB this used to demand. The exact value still has to
     * match what the kernel actually maps, which rdma_map_fast_path checks.
     * dbPageSize is host memory the driver allocates and stays 4 KiB. */
    if (kr != kIOReturnSuccess || out != sizeof(resp) ||
        resp.version != MLX_FAST_PATH_ABI_VERSION ||
        !resp.uarPageSize || (resp.uarPageSize & 4095u) ||
        resp.uarPageSize > 65536u || resp.dbPageSize != 4096)
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
    /* Write-combining is what makes a 64-byte doorbell write worth doing, so
     * it follows blue flame. Measured on ConnectX-4 Lx behind Thunderbolt
     * Gen3 x4: p50 8.4 -> 6.0 us end to end and 2.1 -> 0.6 us on the post
     * itself, reproducible across three runs. Default on; MELONDMA_BLUE_FLAME=0
     * is the kill switch. Every UAR page of this client is mapped the same
     * way, so a QP that lands on a later page keeps the same doorbell cost. */
    dev->uar_map_options = kIOMapAnywhere;
    if ((dev->abi_features & RDMA_FEATURE_BLUE_FLAME) &&
        rdma_env_default_on("MELONDMA_BLUE_FLAME"))
        dev->uar_map_options |= kIOMapWriteCombineCache;
    dev->uar_map_size = path->uar_page_size;
    dev->db_map_size = path->db_page_size;
    if (!rdma_uar_slot(dev, 0) || !rdma_db_page(dev, 0)) {
        rdma_unmap_fast_path(dev);
        return -EIO;
    }
    path->uar = dev->uar_map[0];
    path->db_record = dev->db_map[0];
    return 0;
}

int rdma_fast_path_get_stats(const rdma_device *dev,
                             struct rdma_fast_path_stats *stats)
{
    if (!dev || !stats) return -EINVAL;
    const uint64_t *source = (const uint64_t *)&dev->stats;
    uint64_t *destination = (uint64_t *)stats;
    for (size_t i = 0; i < sizeof(*stats) / sizeof(uint64_t); i++)
        destination[i] = __atomic_load_n(&source[i], __ATOMIC_RELAXED);
    return 0;
}

int rdma_qp_direct_enabled(const rdma_qp *qp)
{
    return qp ? qp->direct_sq : 0;
}

int rdma_qp_trusted_fast_path(const rdma_qp *qp)
{
    return qp ? qp->trusted_fast_path : 0;
}

void rdma_unmap_fast_path(rdma_device *dev)
{
    if (!dev) return;
    for (uint32_t i = 0; i < MLX_CLIENT_MAX_UAR; i++) {
        if (!dev->uar_map[i]) continue;
        (void)IOConnectUnmapMemory(
            dev->conn, MLX_UC_MEM_TYPE(kMlxUCMemKindUar, i),
            mach_task_self(), (mach_vm_address_t)dev->uar_map[i]);
        dev->uar_map[i] = NULL;
    }
    for (uint32_t i = 0; i < MLX_CLIENT_MAX_DB_PAGES; i++) {
        if (!dev->db_map[i]) continue;
        (void)IOConnectUnmapMemory(
            dev->conn, MLX_UC_MEM_TYPE(kMlxUCMemKindDbRecord, i),
            mach_task_self(), (mach_vm_address_t)dev->db_map[i]);
        dev->db_map[i] = NULL;
    }
    dev->uar_map_size = 0;
    dev->db_map_size = 0;
}

/* ---- query ---- */

int rdma_query_abi(rdma_device *dev, struct rdma_abi_attr *attr)
{
    if (!dev || !attr) return -EINVAL;
    struct mlx_query_abi_resp resp = {};
    size_t out = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(
        dev->conn, kMlxUCMethodQueryAbi, NULL, 0, &resp, &out);
    if (kr != kIOReturnSuccess || out != sizeof(resp)) {
        SHIM_LOG("query_abi failed: kr=0x%x out=%zu expected=%zu version=%u features=0x%x",
                 kr, out, sizeof(resp), resp.version, resp.features);
        return -EIO;
    }
    attr->version = resp.version;
    attr->features = resp.features;
    if (resp.version != RDMA_UC_ABI_VERSION) {
        SHIM_LOG("query_abi version mismatch: got=%u expected=%u features=0x%x",
                 resp.version, RDMA_UC_ABI_VERSION, resp.features);
        return -EPROTONOSUPPORT;
    }
    if ((resp.features & (RDMA_FEATURE_RC | RDMA_FEATURE_ROCE_V2)) !=
        (RDMA_FEATURE_RC | RDMA_FEATURE_ROCE_V2)) {
        SHIM_LOG("query_abi missing RC/RoCEv2: version=%u features=0x%x",
                 resp.version, resp.features);
        return -EPROTONOSUPPORT;
    }
    return 0;
}

int rdma_wait_cq_event(rdma_device *dev, uint64_t *generation,
                       uint32_t timeout_ms)
{
    if (!dev || !generation ||
        !(dev->abi_features & (RDMA_FEATURE_CQ_INTERRUPT | RDMA_FEATURE_CQ_EVENT_WAIT))) return -ENOTSUP;
    struct mlx_wait_cq_event_req req = {
        .generation = *generation,
        .timeoutMs = timeout_ms ? timeout_ms : 1000u,
    };
    struct mlx_wait_cq_event_resp resp = {};
    size_t out = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(
        dev->conn, kMlxUCMethodWaitCqEvent, &req, sizeof(req), &resp, &out);
    /* Older DEXTs report the timeout as an error and drop the output struct;
     * leave the caller's snapshot alone in that case. Current DEXTs answer
     * with success and an unchanged generation, which means the same thing. */
    if (kr == kIOReturnTimeout) return -ETIMEDOUT;
    if (kr != kIOReturnSuccess || out != sizeof(resp)) return -EIO;
    if (resp.generation == *generation) return -ETIMEDOUT;
    *generation = resp.generation;
    return 0;
}

int rdma_query_runtime(rdma_device *dev, struct rdma_runtime_status *status)
{
    if (!dev || !status) return -EINVAL;
    if (!(dev->abi_features & RDMA_FEATURE_RUNTIME_STATUS)) return -ENOTSUP;
    struct mlx_runtime_resp raw = {};
    size_t size = sizeof(raw);
    kern_return_t kr = IOConnectCallStructMethod(dev->conn, kMlxUCMethodQueryRuntime,
                                                NULL, 0, &raw, &size);
    if (kr != kIOReturnSuccess) return -EIO;
    if (size != sizeof(raw) || raw.version != MLX_RUNTIME_VERSION || raw.size != size)
        return -EPROTO;
    _Static_assert(sizeof(*status) == sizeof(raw), "runtime v1 layout");
    memcpy(status, &raw, sizeof(raw));
    return 0;
}

int rdma_query_perf(rdma_device *dev, struct rdma_perf *perf)
{
    if (!dev || !perf) return -EINVAL;
    struct mlx_perf_resp resp = {};
    size_t out = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(
        dev->conn, kMlxUCMethodQueryPerf, NULL, 0, &resp, &out);
    if (kr != kIOReturnSuccess || out != sizeof(resp)) return -EIO;
    perf->external_methods = resp.externalMethods;
    perf->external_method_ns = resp.externalMethodNs;
    perf->post_send_calls = resp.postSendCalls;
    perf->post_recv_calls = resp.postRecvCalls;
    perf->poll_cq_calls = resp.pollCqCalls;
    perf->sync_fast_path_calls = resp.syncFastPathCalls;
    perf->sync_qp_tails_calls = resp.syncQpTailsCalls;
    perf->arm_cq_calls = resp.armCqCalls;
    perf->doorbells = resp.doorbells;
    perf->cqe_consumed = resp.cqeConsumed;
    perf->cqe_errors = resp.cqeErrors;
    perf->mr_registers = resp.mrRegisters;
    perf->mr_deregisters = resp.mrDeregisters;
    perf->mr_bytes = resp.mrBytes;
    perf->copied_bytes = resp.copiedBytes;
    perf->cq_events = resp.cqEvents;
    perf->fw_commands = resp.fwCommands;
    perf->fw_command_sleeps = resp.fwCommandSleeps;
    perf->fw_command_slot_waits = resp.fwCommandSlotWaits;
    perf->cq_event_wakeups = resp.cqEventWakeups;
    return 0;
}

int rdma_query_stats(rdma_device *dev, struct rdma_stats *stats)
{
    if (!dev || !stats) return -EINVAL;
    struct mlx_stats_resp resp = {};
    size_t out = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(
        dev->conn, kMlxUCMethodQueryStats, NULL, 0, &resp, &out);
    if (kr != kIOReturnSuccess || out != sizeof(resp)) return -EIO;
    stats->posted_send   = resp.postedSend;
    stats->posted_write  = resp.postedWrite;
    stats->posted_recv   = resp.postedRecv;
    stats->completed_send   = resp.completedSend;
    stats->completed_write  = resp.completedWrite;
    stats->completed_recv   = resp.completedRecv;
    stats->cqe_error     = resp.cqeError;
    stats->cqe_retry_exc = resp.cqeRetryExc;
    stats->cqe_rnr_retry = resp.cqeRnrRetry;
    stats->cq_lost       = resp.cqLost;
    stats->sq_occupancy  = resp.sqOccupancy;
    stats->rq_occupancy  = resp.rqOccupancy;
    return 0;
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
    attr->node_guid = resp.nodeGuid;
    attr->port_guid = resp.portGuid;
    attr->sys_image_guid = resp.sysImageGuid;
    return 0;
}

int rdma_query_interrupts(rdma_device *dev, struct rdma_interrupt_attr *attr)
{
    if (!dev || !attr) return -EINVAL;
    struct mlx_interrupts_resp resp = {};
    size_t out = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(
        dev->conn, kMlxUCMethodQueryInterrupts, NULL, 0, &resp, &out);
    if (kr == kIOReturnUnsupported) return -ENOTSUP;
    if (kr != kIOReturnSuccess || out != sizeof(resp)) return -EIO;
    attr->vectors = resp.vectors;
    attr->setup_status = resp.setupStatus;
    attr->setup_stage = resp.setupStage;
    attr->async_eqn = resp.asyncEqn;
    attr->completion_eqn = resp.completionEqn;
    attr->completion_ready = resp.completionReady;
    attr->completion_events = resp.completionEvents;
    attr->completion_eq_status = resp.completionEqStatus;
    attr->completion_eq_stage = resp.completionEqStage;
    attr->completion_eq_syndrome = resp.completionEqSyndrome;
    attr->completion_eq_fw_status = resp.completionEqFwStatus;
    attr->completion_eq_variant = resp.completionEqVariant;
    attr->completion_eq_variant_tried = resp.completionEqVariantTried;
    for (int i = 0; i < 4; i++)
        attr->completion_eq_variant_syndrome[i] =
            resp.completionEqVariantSyndrome[i];
    attr->async_interrupts = resp.asyncInterrupts;
    attr->completion_interrupts = resp.completionInterrupts;
    attr->eq_timer_ticks = resp.eqTimerTicks;
    attr->eq_timer_period_ms = resp.eqTimerPeriodMs;
    attr->index_count = resp.indexCount;
    attr->index_count_pre = resp.indexCountPre;
    attr->msix_index_base = resp.msixIndexBase;
    attr->async_index = resp.asyncIndex;
    attr->completion_index = resp.completionIndex;
    attr->index_probe_status = resp.indexProbeStatus;
    for (int i = 0; i < RDMA_IRQ_INDEX_MAP; i++) {
        attr->index_kind[i] = resp.indexKind[i];
        attr->index_kind_pre[i] = resp.indexKindPre[i];
        attr->index_type_raw[i] = resp.indexTypeRaw[i];
        attr->index_bind[i] = resp.indexBind[i];
    }
    attr->completion_eq_count = resp.completionEqCount;
    for (unsigned i = 0; i < RDMA_IRQ_COMP_EQ_MAX; i++) {
        attr->completion_irq_by_eq[i] = resp.completionIrqByEq[i];
    }
    return 0;
}

int rdma_query_limits(rdma_device *dev, struct rdma_limits *limits)
{
    if (!dev || !limits) return -EINVAL;
    struct mlx_query_limits_resp resp = {};
    size_t out = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(
        dev->conn, kMlxUCMethodQueryLimits, NULL, 0, &resp, &out);
    if (kr == kIOReturnUnsupported) return -ENOTSUP;
    if (kr != kIOReturnSuccess || out != sizeof(resp)) return -EIO;
    limits->max_pd = resp.maxPd;
    limits->max_qp = resp.maxQp;
    limits->max_cq = resp.maxCq;
    limits->max_mr = resp.maxMr;
    limits->max_mw = resp.maxMw;
    limits->max_ah = resp.maxAh;
    limits->max_gid = resp.maxGid;
    limits->max_sq_depth = resp.maxSqDepth;
    limits->max_rq_depth = resp.maxRqDepth;
    limits->fw_cmd_burst = resp.fwCmdBurst;
    limits->fw_cmd_window_ns = resp.fwCmdWindowNs;
    limits->max_db_records = resp.maxDbRecords;
    limits->bf_supported = resp.bfSupported;
    limits->log_bf_reg_size = resp.logBfRegSize;
    limits->uar_page_size = resp.uarPageSize;
    limits->bf_regs_per_uar = resp.bfRegsPerUar;
    limits->max_inline_data = resp.maxInlineData;
    limits->max_sge = resp.maxSge;
    limits->pcie_link_speed = resp.pcieLinkSpeed;
    limits->pcie_link_width = resp.pcieLinkWidth;
    limits->cache_line_128 = resp.cacheLine128;
    limits->log_uar_page_sz = resp.logUarPageSz;
    memcpy(limits->board_id, resp.boardId, sizeof(resp.boardId));
    limits->board_id[sizeof(limits->board_id) - 1] = '\0';
    return 0;
}

double rdma_pcie_line_gbps(uint32_t speed, uint32_t width)
{
    if (!speed || !width) return 0.0;
    double gt;         /* transfers per second, in giga */
    double coding;     /* useful bits per transfer */
    switch (speed) {
    case 1: gt = 2.5;  coding = 8.0 / 10.0;   break;
    case 2: gt = 5.0;  coding = 8.0 / 10.0;   break;
    case 3: gt = 8.0;  coding = 128.0 / 130.0; break;
    case 4: gt = 16.0; coding = 128.0 / 130.0; break;
    case 5: gt = 32.0; coding = 128.0 / 130.0; break;
    default: return 0.0;
    }
    return gt * coding * (double)width;
}

static int rdma_msix_call(rdma_device *dev, struct mlx_program_msix_req *req)
{
    size_t outsz = sizeof(*req);
    kern_return_t kr = IOConnectCallStructMethod(
        dev->conn, kMlxUCMethodProgramMsix, req, sizeof(*req), req, &outsz);
    if (kr == kIOReturnNotPermitted) return -EPERM;
    if (kr == kIOReturnUnsupported) return -ENOTSUP;
    if (kr == kIOReturnBadArgument) return -EINVAL;
    return kr == kIOReturnSuccess ? 0 : -EIO;
}

int rdma_program_msix(rdma_device *dev, uint32_t vector, uint32_t addr_lo,
                      uint32_t addr_hi, uint32_t data, int masked)
{
    if (!dev) return -EINVAL;
    struct mlx_program_msix_req req = {
        .op = MLX_MSIX_OP_PROGRAM, .vector = vector, .addrLo = addr_lo,
        .addrHi = addr_hi, .data = data, .masked = masked ? 1u : 0u,
    };
    return rdma_msix_call(dev, &req);
}

int rdma_mask_msix(rdma_device *dev, uint32_t vector, int masked)
{
    if (!dev) return -EINVAL;
    struct mlx_program_msix_req req = {
        .op = MLX_MSIX_OP_MASK, .vector = vector,
        .masked = masked ? 1u : 0u,
    };
    return rdma_msix_call(dev, &req);
}

int rdma_set_eq_timer_paused(rdma_device *dev, int paused)
{
    if (!dev) return -EINVAL;
    uint32_t value = paused ? 1u : 0u;
    kern_return_t kr = IOConnectCallStructMethod(
        dev->conn, kMlxUCMethodSetEqTimerPaused,
        &value, sizeof(value), NULL, 0);
    if (kr == kIOReturnNotPermitted) return -EPERM;
    if (kr == kIOReturnUnsupported) return -ENOTSUP;
    return kr == kIOReturnSuccess ? 0 : -EIO;
}

int rdma_msix_poke(rdma_device *dev, uint32_t region, uint32_t index,
                   uint32_t value)
{
    if (!dev) return -EINVAL;
    struct mlx_program_msix_req req = {
        .op = MLX_MSIX_OP_POKE, .vector = index, .addrLo = region,
        .data = value,
    };
    return rdma_msix_call(dev, &req);
}

int rdma_msix_peek(rdma_device *dev, uint32_t region, uint32_t index,
                   uint32_t *value)
{
    if (!dev || !value) return -EINVAL;
    struct mlx_program_msix_req req = {
        .op = MLX_MSIX_OP_PEEK, .vector = index, .addrLo = region,
    };
    int rc = rdma_msix_call(dev, &req);
    if (!rc) *value = req.dataOut;
    return rc;
}

struct rdma_srq { rdma_device *dev; uint32_t srqn; };

rdma_srq *rdma_create_srq(rdma_pd *pd, uint32_t max_wr, uint32_t max_sge,
                          uint32_t limit, struct rdma_srq_attr *out)
{
    if (!pd || !max_wr || !max_sge) { errno = EINVAL; return NULL; }
    rdma_device *dev = pd->dev;
    if (!dev) { errno = EINVAL; return NULL; }
    struct mlx_create_srq_req req = { .pd = pd->pd, .maxWr = max_wr,
                                      .maxSge = max_sge, .limit = limit };
    struct mlx_create_srq_resp resp = {};
    size_t outsz = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(dev->conn, kMlxUCMethodCreateSrq,
                                                 &req, sizeof(req), &resp, &outsz);
    if (kr != kIOReturnSuccess || outsz != sizeof(resp)) {
        errno = (kr == kIOReturnUnsupported) ? ENOTSUP : EIO;
        return NULL;
    }
    rdma_srq *srq = calloc(1, sizeof(*srq));
    if (!srq) { errno = ENOMEM; return NULL; }
    srq->dev = dev;
    srq->srqn = resp.srqn;
    if (out) { out->max_wr = resp.maxWr; out->max_sge = resp.maxSge;
               out->srq_limit = limit; out->free_count = resp.maxWr; out->posted = 0; }
    return srq;
}

int rdma_destroy_srq(rdma_srq *srq)
{
    if (!srq) return -EINVAL;
    uint32_t token = srq->srqn;
    kern_return_t kr = IOConnectCallStructMethod(srq->dev->conn,
        kMlxUCMethodDestroySrq, &token, sizeof(token), NULL, NULL);
    if (kr != kIOReturnSuccess) return -EIO;
    free(srq);
    return 0;
}

int rdma_post_srq_recv(rdma_srq *srq, uint64_t wr_id, uint32_t num_sge,
                       const struct rdma_sge *sge)
{
    if (!srq || !sge || !num_sge || num_sge > MLX_SRQ_MAX_SGE) return -EINVAL;
    struct mlx_post_srq_recv_req req = { .srqn = srq->srqn, .numSge = num_sge,
                                         .wrId = wr_id };
    for (uint32_t i = 0; i < num_sge; i++) {
        req.sge[i].lkey = sge[i].lkey;
        req.sge[i].addr = sge[i].addr;
        req.sge[i].length = sge[i].length;
    }
    kern_return_t kr = IOConnectCallStructMethod(srq->dev->conn,
        kMlxUCMethodPostSrqRecv, &req, sizeof(req), NULL, NULL);
    if (kr == kIOReturnNoSpace) return -ENOMEM;
    return kr == kIOReturnSuccess ? 0 : -EIO;
}

int rdma_query_srq(rdma_srq *srq, struct rdma_srq_attr *out)
{
    if (!srq || !out) return -EINVAL;
    uint32_t token = srq->srqn;
    struct mlx_query_srq_resp resp = {};
    size_t outsz = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(srq->dev->conn,
        kMlxUCMethodQuerySrq, &token, sizeof(token), &resp, &outsz);
    if (kr != kIOReturnSuccess || outsz != sizeof(resp)) return -EIO;
    out->max_wr = resp.maxWr; out->max_sge = resp.maxSge;
    out->srq_limit = resp.limit; out->free_count = resp.freeCount;
    out->posted = resp.posted;
    out->recv_seen = resp.recvSeen; out->recv_with_srqn = resp.recvWithSrqn;
    out->last_cqe_srqn = resp.lastCqeSrqn; out->last_cqe_wqe = resp.lastCqeWqe;
    out->last_cqe_op = resp.lastCqeOp;
    out->wqe_seen_mask = resp.wqeSeenMask;
    return 0;
}

int rdma_modify_srq(rdma_srq *srq, uint32_t limit)
{
    if (!srq) return -EINVAL;
    struct mlx_modify_srq_req req = { .srqn = srq->srqn, .limit = limit };
    kern_return_t kr = IOConnectCallStructMethod(srq->dev->conn,
        kMlxUCMethodModifySrq, &req, sizeof(req), NULL, NULL);
    return kr == kIOReturnSuccess ? 0 : -EIO;
}

uint32_t rdma_srq_number(rdma_srq *srq) { return srq ? srq->srqn : 0; }

int rdma_probe_rmp_layout(rdma_device *dev,
                          struct mlx_probe_rmp_layout_resp *out)
{
    if (!dev || !out) return -EINVAL;
    size_t outsz = sizeof(*out);
    kern_return_t kr = IOConnectCallStructMethod(
        dev->conn, kMlxUCMethodProbeRmpLayout, NULL, 0, out, &outsz);
    if (kr == kIOReturnNotPermitted) return -EPERM;
    if (kr == kIOReturnUnsupported) return -ENOTSUP;
    if (kr != kIOReturnSuccess || outsz != sizeof(*out)) return -EIO;
    return 0;
}

int rdma_query_eq_state(rdma_device *dev, uint32_t eqn,
                        struct rdma_eq_state *out)
{
    if (!dev || !out) return -EINVAL;
    struct mlx_query_eq_req req = { .eqn = eqn };
    struct mlx_query_eq_resp resp = {};
    size_t outsz = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(
        dev->conn, kMlxUCMethodQueryEqState, &req, sizeof(req), &resp, &outsz);
    if (kr == kIOReturnNotPermitted) return -EPERM;
    if (kr == kIOReturnUnsupported) return -ENOTSUP;
    if (kr != kIOReturnSuccess || outsz != sizeof(resp)) return -EIO;
    out->status = resp.status;
    out->state = resp.state;
    out->intr = resp.intr;
    out->uar_page = resp.uarPage;
    out->consumer_index = resp.consumerIndex;
    out->producer_index = resp.producerIndex;
    out->log_eq_size = resp.logEqSize;
    out->fw_status = resp.fwStatus;
    out->syndrome = resp.syndrome;
    return 0;
}

int rdma_query_msix_state(rdma_device *dev, struct rdma_msix_state *state)
{
    if (!dev || !state) return -EINVAL;
    struct mlx_msix_state_resp resp = {};
    size_t out = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(
        dev->conn, kMlxUCMethodQueryMsixState, NULL, 0, &resp, &out);
    if (kr == kIOReturnNotPermitted) return -EPERM;
    if (kr == kIOReturnUnsupported) return -ENOTSUP;
    if (kr != kIOReturnSuccess || out != sizeof(resp)) return -EIO;
    memset(state, 0, sizeof(*state));
    state->cap_offset      = resp.capOffset;
    state->message_control = resp.messageControl;
    state->table_size      = resp.tableSize;
    state->table_bir       = resp.tableBir;
    state->pba_bir         = resp.pbaBir;
    state->table_offset    = resp.tableOffset;
    state->pba_offset      = resp.pbaOffset;
    state->entries_read    = resp.entriesRead;
    state->pba_words       = resp.pbaWords;
    state->status          = resp.status;
    state->command_reg     = resp.commandReg;
    state->bar_index_used  = resp.barIndexUsed;
    state->pre_configure_entry0_addr_lo = resp.preConfigureEntry0AddrLo;
    state->pre_configure_entry0_data    = resp.preConfigureEntry0Data;
    for (uint32_t i = 0; i < resp.entriesRead &&
                         i < RDMA_MSIX_TABLE_SNAPSHOT; i++) {
        state->entry[i].addr_lo        = resp.entry[i].addrLo;
        state->entry[i].addr_hi        = resp.entry[i].addrHi;
        state->entry[i].data           = resp.entry[i].data;
        state->entry[i].vector_control = resp.entry[i].vectorControl;
    }
    for (uint32_t i = 0; i < resp.pbaWords && i < RDMA_MSIX_PBA_WORDS; i++)
        state->pba[i] = resp.pba[i];
    return 0;
}

int rdma_probe_completion_vector(rdma_device *dev, uint32_t intr, uint32_t *eqn)
{
    if (!dev) return -EINVAL;
    struct mlx_probe_completion_vector_req req = { .intr = intr };
    struct mlx_probe_completion_vector_resp resp = {};
    size_t out = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(dev->conn,
        kMlxUCMethodProbeCompletionVector, &req, sizeof(req), &resp, &out);
    if (kr == kIOReturnUnsupported) return -ENOTSUP;
    if (kr == kIOReturnBusy) return -EBUSY;
    if (kr != kIOReturnSuccess || out != sizeof(resp)) return -EIO;
    if (eqn) *eqn = resp.eqn;
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
    for (int i = 0; i < 6; i++) attr->assert_var[i] = resp.assertVar[i];
    attr->assert_exit_ptr = resp.assertExitPtr;
    attr->assert_callra = resp.assertCallra;
    attr->health_time = resp.healthTime;
    attr->fw_ver = resp.fwVer;
    attr->hw_id = resp.hwId;
    attr->rfr_severity = resp.rfrSeverity;
    attr->irisc_index = resp.iriscIndex;
    attr->device_removed = resp.deviceRemoved;
    return 0;
}

int rdma_query_hca_clock(rdma_device *dev, struct rdma_hca_clock *clock)
{
    if (!dev || !clock) return -EINVAL;
    struct mlx_hca_clock_resp resp = {};
    size_t out = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(
        dev->conn, kMlxUCMethodQueryHcaClock, NULL, 0, &resp, &out);
    if (kr == kIOReturnUnsupported) return -ENOTSUP;
    if (kr == kIOReturnBadArgument) return -EINVAL;
    if (kr != kIOReturnSuccess || out != sizeof(resp)) return -EIO;
    clock->ticks = resp.ticks;
    clock->host_uptime_ns = resp.hostUptimeNs;
    clock->frequency_khz = resp.frequencyKhz;
    return 0;
}

int rdma_query_q_counters(rdma_device *dev, uint32_t counter_set_id, int clear,
                          struct rdma_q_counters *counters)
{
    if (!dev || !counters) return -EINVAL;
    struct mlx_q_counters_req req = { .counterSetId = counter_set_id,
                                      .clear = clear ? 1u : 0u };
    struct mlx_q_counters_resp resp = {};
    size_t out = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(
        dev->conn, kMlxUCMethodQueryQCounters, &req, sizeof(req), &resp, &out);
    /* Only a genuinely absent selector is ENOTSUP. Folding BadArgument in as
     * well printed "not supported by this DEXT" for a firmware BAD_PARAM,
     * which sent the search in the wrong direction for one deploy. */
    if (kr == kIOReturnUnsupported) return -ENOTSUP;
    if (kr == kIOReturnBadArgument) return -EINVAL;
    if (kr != kIOReturnSuccess || out != sizeof(resp)) return -EIO;
    counters->rx_write_requests       = resp.rxWriteRequests;
    counters->rx_read_requests        = resp.rxReadRequests;
    counters->rx_atomic_requests      = resp.rxAtomicRequests;
    counters->out_of_buffer           = resp.outOfBuffer;
    counters->out_of_sequence         = resp.outOfSequence;
    counters->duplicate_request       = resp.duplicateRequest;
    counters->rnr_nak_retry_err       = resp.rnrNakRetryErr;
    counters->packet_seq_err          = resp.packetSeqErr;
    counters->implied_nak_seq_err     = resp.impliedNakSeqErr;
    counters->local_ack_timeout_err   = resp.localAckTimeoutErr;
    counters->req_rnr_retries_exceeded = resp.reqRnrRetriesExceeded;
    counters->resp_local_length_error = resp.respLocalLengthError;
    counters->req_local_length_error  = resp.reqLocalLengthError;
    counters->local_operation_error   = resp.localOperationError;
    counters->resp_cqe_error          = resp.respCqeError;
    counters->req_cqe_error           = resp.reqCqeError;
    return 0;
}

int rdma_query_cong_stats(rdma_device *dev, uint32_t priority, int clear,
                          struct rdma_cong_stats *stats)
{
    if (!dev || !stats || priority > 7) return -EINVAL;
    struct mlx_cc_stats_req req = { .priority = priority,
                                    .clear = clear ? 1u : 0u };
    struct mlx_cc_stats_resp resp = {};
    size_t out = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(
        dev->conn, kMlxUCMethodCCStats, &req, sizeof(req), &resp, &out);
    if (kr == kIOReturnUnsupported || kr == kIOReturnBadArgument)
        return -ENOTSUP;
    if (kr != kIOReturnSuccess || out != sizeof(resp)) return -EIO;
    memset(stats, 0, sizeof(*stats));
    stats->enable = resp.enable;
    stats->tag_enable = resp.tagEnable;
    stats->rp_cnp_ignored = resp.rpCnpIgnored;
    stats->rp_cnp_handled = resp.rpCnpHandled;
    stats->np_ecn_marked_roce_packets = resp.npEcnMarkedRocePackets;
    stats->np_cnp_sent = resp.npCnpSent;
    stats->time_stamp = resp.timeStamp;
    stats->rp_cur_flows = resp.rpCurFlows;
    stats->sum_flows = resp.sumFlows;
    stats->accumulators_period = resp.accumulatorsPeriod;
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
    attr->link_speed_mbps   = resp.linkSpeedMbps;
    attr->max_mtu        = resp.maxMtu;
    attr->pause_tx       = resp.pauseTx;
    attr->pause_rx       = resp.pauseRx;
    attr->pfc_tx_mask    = resp.pfcTxMask;
    attr->pfc_rx_mask    = resp.pfcRxMask;
    attr->pfc_prio_mask  = resp.pfcPrioMask;
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
    cq->next = dev->cqs;
    dev->cqs = cq;
    cq->published_consumer_index = 0;
    cq->synced_consumer_index = 0;
    cq->consumer_batch = 8;
    const char *consumer_batch_env = getenv("MELONDMA_CQ_CONSUMER_BATCH");
    if (consumer_batch_env) {
        char *end = NULL;
        unsigned long value = strtoul(consumer_batch_env, &end, 10);
        if (end && !*end && value >= 1 && value <= 64)
            cq->consumer_batch = (uint32_t)value;
    }
    /* Never defer more than a quarter of a small CQ. This leaves ample room
     * for the device even if the application drains one WC at a time. */
    if (cq->consumer_batch > cq->depth / 4u)
        cq->consumer_batch = cq->depth / 4u;
    if (!cq->consumer_batch) cq->consumer_batch = 1;
    cq->db_record_offset = resp.dbRecordOffset;
    /* The offset is flat across this client's doorbell pages: divide for the
     * page, take the remainder inside it. The page is mapped on first use. */
    {
        const uint32_t page = mlxFlatPage(resp.dbRecordOffset,
                                          MLX_CLIENT_DB_PAGE_SIZE);
        const uint32_t within = mlxFlatWithin(resp.dbRecordOffset,
                                              MLX_CLIENT_DB_PAGE_SIZE);
        uint8_t *base = (uint8_t *)rdma_db_page(cq->dev, page);
        if (base && within + sizeof(uint32_t) <= cq->dev->db_map_size)
            cq->db_rec = base + within;
    }
    mach_vm_address_t cqe = 0;
    mach_vm_size_t cqe_size = 0;
    kr = IOConnectMapMemory64(
        dev->conn, MLX_UC_MEM_TYPE(kMlxUCMemKindCqe, resp.cqHandle),
        mach_task_self(), &cqe, &cqe_size, kIOMapAnywhere);
    if (kr == kIOReturnSuccess && cqe_size >= (mach_vm_size_t)cq->depth * 64) {
        cq->cqe_buf = (volatile struct MlxCqe64 *)(uintptr_t)cqe;
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

int rdma_cq_pending_local(rdma_cq *cq, uint64_t *count)
{
    if (!cq || !count) return -EINVAL;
    if (!cq->cqe_buf) return -EAGAIN;   /* ring not mapped — use kernel query */
    const uint32_t depth = cq->depth;
    uint64_t pending = 0;
    while (pending < depth) {
        uint64_t abs = (uint64_t)cq->consumer_index + pending;
        const volatile struct MlxCqe64 *e = &cq->cqe_buf[abs & (depth - 1)];
        uint8_t expected = (uint8_t)((abs >> cq->log_size) & 1);
        if ((e->op_own & MLX_CQE_OWNER_MASK) != expected ||
            MLX_CQE_GET_OPCODE(e) == MLX_CQE_INVALID)
            break;
        pending++;
    }
    /* Total completions produced (monotonic): consumed + in-ring.  This is the
     * semantic the worker's fire condition compares against event_count.  The
     * DEXT's GetCompletions returns (EQ-event count + pending), which advances
     * at a different rate and stalls the channel — do NOT mix the two. */
    *count = (uint64_t)cq->consumer_index + pending;
    return 0;
}

/* Publish the software-exact consumer index lazily. CQ ownership is still
 * advanced on every decoded CQE; only the coherent DB record + device barrier
 * are amortized. Control boundaries call rdma_update_cq_consumer(), which also
 * reconciles the DEXT's dormant consumer index. */
static void rdma_publish_cq_consumer(rdma_cq *cq, int force)
{
    if (!cq || cq->consumer_index == cq->published_consumer_index) return;
    uint32_t pending = cq->consumer_index - cq->published_consumer_index;
    if (!force && pending < cq->consumer_batch &&
        pending < (cq->depth > 4 ? cq->depth / 4u : 1u))
        return;
    if (cq->db_rec) {
        volatile uint32_t *db = (volatile uint32_t *)(cq->db_rec);
        rdma_dma_write_barrier();
        db[0] = __builtin_bswap32(cq->consumer_index & 0xffffffu);
        cq->published_consumer_index = cq->consumer_index;
        rdma_stat_add(&cq->dev->stats.direct_cq_consumers, 1);
    }
}

int rdma_destroy_cq(rdma_cq *cq)
{
    if (!cq) return -EINVAL;
    if (cq->consumer_index != cq->synced_consumer_index &&
        rdma_update_cq_consumer(cq, cq->consumer_index) != 0)
        return -EIO;
    rdma_publish_cq_consumer(cq, 1);
    if (cq->cqe_buf) {
        kern_return_t unmapkr = IOConnectUnmapMemory(
            cq->dev->conn, MLX_UC_MEM_TYPE(kMlxUCMemKindCqe, cq->cq_handle),
            mach_task_self(), (mach_vm_address_t)cq->cqe_buf);
        if (unmapkr != kIOReturnSuccess) return -EIO;
        cq->cqe_buf = NULL;
    }
    uint32_t h = cq->cq_handle;
    struct rdma_cq **cursor = &cq->dev->cqs;
    while (*cursor && *cursor != cq) cursor = &(*cursor)->next;
    if (*cursor == cq) *cursor = cq->next;
    kern_return_t kr = IOConnectCallStructMethod(
        cq->dev->conn, kMlxUCMethodDestroyCQ,
        &h, sizeof(h), NULL, 0);
    if (kr != kIOReturnSuccess) {
        SHIM_LOG("destroy_cq failed: 0x%x", kr);
        cq->next = *cursor;
        *cursor = cq;
        return -EIO;
    }
    free(cq);
    return 0;
}

rdma_cq *rdma_find_cq(rdma_device *dev, uint32_t token)
{
    return rdma_find_cq_token(dev, token);
}

rdma_qp *rdma_find_qp(rdma_device *dev, uint32_t token)
{
    return rdma_find_qp_token(dev, token);
}

static uint32_t rdma_error_cqe_status(uint8_t syndrome)
{
    switch (syndrome) {
    case 0x01: return RDMA_WC_LOC_LEN;
    case 0x02: return RDMA_WC_LOC_QP_OP;
    case 0x05: return RDMA_WC_WR_FLUSH;
    case 0x13: return RDMA_WC_REM_ACCESS;
    case 0x15: return RDMA_WC_RETRY_EXC;
    case 0x16: return RDMA_WC_RNR_RETRY;
    default:   return RDMA_WC_GENERAL;
    }
}

enum {
    RDMA_DIRECT_FALLBACK_UNKNOWN_QP = -1001,
    RDMA_DIRECT_FALLBACK_METADATA   = -1002,
    RDMA_DIRECT_FALLBACK_DEXT_OWNED = -1003,
};

/* Direct CQ decode: read CQEs from the mapped ring and reconstruct WCs in
 * userspace. Successful and error SEND/RECV/READ/WRITE/ATOMIC CQEs are handled
 * here, which keeps the trusted path independent of DEXT WR metadata. UMR and
 * LOCAL_INV retain the validated fallback path because they mutate the
 * DEXT-owned memory-key table. */
static int rdma_poll_cq_direct(rdma_cq *cq, struct rdma_wc *wc, int num)
{
    if (num > (int)MLX_UC_MAX_POLL_WC) num = (int)MLX_UC_MAX_POLL_WC;
    uint32_t depth = cq->depth;
    uint32_t ci = cq->consumer_index;
    int count = 0;
    rdma_qp *touched[MLX_UC_MAX_POLL_WC];
    int ntouched = 0;
    while (count < num) {
        const volatile struct MlxCqe64 *e = &cq->cqe_buf[ci & (depth - 1)];
        uint8_t op_own = e->op_own;
        uint8_t opcode = MLX_CQE_GET_OPCODE(e);
        uint8_t expected = (uint8_t)((ci >> cq->log_size) & 1);
        if ((op_own & MLX_CQE_OWNER_MASK) != expected || opcode == MLX_CQE_INVALID)
            break;
        /* Hardware writes the CQE body before flipping ownership. */
        rdma_dma_read_barrier();
        int error_cqe = opcode == MLX_CQE_REQ_ERR ||
                        opcode == MLX_CQE_RESP_ERR;
        uint32_t qpn_be = 0;
        uint16_t counter_be = 0;
        if (error_cqe) {
            const volatile uint8_t *raw = (const volatile uint8_t *)e;
            memcpy(&qpn_be, (const void *)(uintptr_t)(raw + 56), sizeof(qpn_be));
            memcpy(&counter_be, (const void *)(uintptr_t)(raw + 60),
                   sizeof(counter_be));
        }
        uint32_t hw_qpn = error_cqe ? (__builtin_bswap32(qpn_be) & 0xffffffu) :
                          (__builtin_bswap32(e->sop_drop_qpn) & 0xffffffu);
        uint16_t counter = error_cqe ? __builtin_bswap16(counter_be) :
                                       __builtin_bswap16(e->wqe_counter);
        rdma_qp *qp = rdma_find_qp_hw(cq->dev, hw_qpn);
        if (!qp) {
            if (count) break;
            return RDMA_DIRECT_FALLBACK_UNKNOWN_QP;
        }
        {
            int seen = 0;
            for (int t = 0; t < ntouched; t++)
                if (touched[t] == qp) { seen = 1; break; }
            if (!seen && ntouched < (int)(sizeof(touched) / sizeof(touched[0])))
                touched[ntouched++] = qp;
        }

        struct rdma_wc *out = &wc[count];
        memset(out, 0, sizeof(*out));
        out->qp_num = hw_qpn;
        out->wqe_counter = counter;
        out->status = RDMA_WC_SUCCESS;
        out->byte_len = error_cqe ? 0 : __builtin_bswap32(e->byte_cnt);
        if (error_cqe) {
            const volatile uint8_t *raw = (const volatile uint8_t *)e;
            uint8_t vendor = raw[54];
            uint8_t syndrome = raw[55];
            out->status = rdma_error_cqe_status(syndrome);
            out->vendor_err = ((uint32_t)vendor << 8) | syndrome;
            qp->state = RDMA_QPS_ERR;
        }

        if (opcode == MLX_CQE_REQ || opcode == MLX_CQE_REQ_ERR) {
            if (!qp->direct_sq || !qp->sq_wrid || !qp->sq_opcode ||
                !qp->sq_span) {
                if (count) break;
                return RDMA_DIRECT_FALLBACK_METADATA;
            }
            uint32_t idx = counter & (qp->sq_size - 1);
            if (!qp->sq_span[idx]) {
                if (count) break;
                return RDMA_DIRECT_FALLBACK_DEXT_OWNED;
            }
            uint8_t wrop = qp->sq_opcode[idx];
            if (wrop == RDMA_WR_LOCAL_INV) {
                if (count) break;
                return RDMA_DIRECT_FALLBACK_DEXT_OWNED;
            }
            out->wr_id = qp->sq_wrid[idx];
            out->opcode = (wrop == RDMA_WR_RDMA_WRITE || wrop == RDMA_WR_RDMA_WRITE_IMM)
                              ? RDMA_WC_RDMA_WRITE
                          : (wrop == RDMA_WR_RDMA_READ) ? RDMA_WC_RDMA_READ
                          : (wrop == RDMA_WR_ATOMIC_CS) ? RDMA_WC_COMP_SWAP
                          : (wrop == RDMA_WR_ATOMIC_FA) ? RDMA_WC_FETCH_ADD
                                                        : RDMA_WC_SEND;
            if (!error_cqe &&
                (wrop == RDMA_WR_ATOMIC_CS || wrop == RDMA_WR_ATOMIC_FA)) {
                uint64_t result_addr = qp->sq_atomic_result ?
                                       qp->sq_atomic_result[idx] : 0;
                if (!result_addr) {
                    if (count) break;
                    return RDMA_DIRECT_FALLBACK_METADATA;
                }
                rdma_dma_read_barrier();
                memcpy(&out->atomic_result,
                       (const void *)(uintptr_t)result_addr,
                       sizeof(out->atomic_result));
                out->wc_flags |= RDMA_WC_WITH_ATOMIC;
            }
            /* Advance sq_tail to the completed signaled WQE + its span, not
             * to sq_head: newer WRs may still be in flight (unsignaled
             * batches), and over-advancing would let DestroyQP proceed while
             * the SQ is still live and weaken the SQ-full credit check.
             * Mirrors the DEXT's DecodeWc (mlxQpExpandCounter + sqSpan). */
            uint64_t done = (qp->sq_head & ~0xffffULL) | counter;
            if (done > qp->sq_head) done -= 0x10000ULL;
            uint64_t tail = done + qp->sq_span[idx];
            if (tail > qp->sq_tail) {
                uint64_t old_tail = qp->sq_tail;
                qp->sq_tail = tail;
                /* Retire metadata for the signaled WQE and every preceding
                 * unsignaled WQE reclaimed by it.  This prevents a later
                 * DEXT-posted control WQE at a wrapped index from being
                 * mistaken for stale userspace metadata. */
                while (old_tail < tail) {
                    uint32_t retired = (uint32_t)old_tail & (qp->sq_size - 1);
                    uint8_t span = qp->sq_span[retired];
                    qp->sq_span[retired] = 0;
                    qp->sq_wrid[retired] = 0;
                    qp->sq_opcode[retired] = 0;
                    if (qp->sq_atomic_result) qp->sq_atomic_result[retired] = 0;
                    old_tail += span ? span : 1;
                }
            }
        } else {   /* recv completion */
            /* Belt and braces: a CQ carrying an SRQ-bound QP is marked
             * kernel_poll_only at creation and never reaches this decoder.
             * If one ever does, refuse rather than read a ring that has no
             * metadata here. */
            if (qp->srqn) {
                if (count) break;
                return RDMA_DIRECT_FALLBACK_METADATA;
            }
            if (!qp->direct_rq || !qp->rq_wrid) {
                if (count) break;
                return RDMA_DIRECT_FALLBACK_METADATA;
            }
            uint32_t idx = counter & (qp->rq_size - 1);
            out->wr_id = qp->rq_wrid[idx];
            out->opcode = RDMA_WC_RECV;
            if (!error_cqe &&
                (opcode == MLX_CQE_RESP_WR_IMM ||
                 opcode == MLX_CQE_RESP_SEND_IMM)) {
                out->imm_data = e->imm_inval_pkey;   /* network byte order, as DEXT */
                out->wc_flags |= RDMA_WC_WITH_IMM;
            } else if (!error_cqe && opcode == MLX_CQE_RESP_SEND_INV) {
                out->imm_data = __builtin_bswap32(e->imm_inval_pkey);
                out->wc_flags |= RDMA_WC_WITH_INV;
            }
            /* Scatter-to-CQE (DATA32): the <=32-byte payload is in the CQE
             * bytes 0..31, not the receive buffer. Copy it to the first recv
             * SGE so the caller finds the data where it posted it. */
            if (!error_cqe && (e->op_own & MLX_CQE_INLINE_SCATTER_32) &&
                out->byte_len && out->byte_len <= 32 && qp->rq_sge_addr) {
                void *dst = (void *)(uintptr_t)qp->rq_sge_addr[idx];
                if (dst) memcpy(dst, (const void *)e, out->byte_len);
            }
            uint64_t done = (qp->rq_head & ~0xffffULL) | counter;
            if (done > qp->rq_head) done -= 0x10000ULL;
            if (done + 1 > qp->rq_tail) qp->rq_tail = done + 1;
        }
        ci++;
        count++;
        if (error_cqe)
            rdma_stat_add(&cq->dev->stats.direct_cqe_errors, 1);
    }
    if (count) {
        rdma_stat_add(&cq->dev->stats.direct_cqes, (uint64_t)count);
        cq->consumer_index = ci;
        rdma_publish_cq_consumer(cq, 0);
        /* Publish completion tails into the isolated shadow page. Trusted
         * fast-path DEXT state is harvested only on control/diagnostic paths;
         * the validated compatibility mode keeps the legacy sync call. */
        for (int t = 0; t < ntouched; t++) {
            rdma_qp_publish_shadow_maybe(touched[t],
                RDMA_QP_SHADOW_SQ_TAIL | RDMA_QP_SHADOW_RQ_TAIL);
            if (!touched[t]->trusted_fast_path &&
                rdma_sync_qp_tails(touched[t], touched[t]->sq_tail,
                                   touched[t]->rq_tail) != 0)
                SHIM_LOG("sync_qp_tails failed qpn=%u", touched[t]->qpn);
        }
    }
    return count;
}

int rdma_poll_cq(rdma_cq *cq, struct rdma_wc *wc, int num)
{
    if (!cq || !wc || num <= 0) return -EINVAL;
    const int direct_requested = rdma_env_default_on("MELONDMA_DIRECT_CQ") &&
                                 !cq->kernel_poll_only;
    if (cq->cqe_buf && direct_requested) {
        rdma_stat_add(&cq->dev->stats.direct_poll_calls, 1);
        int n = rdma_poll_cq_direct(cq, wc, num);
        if (n >= 0) {
            if (!n) rdma_stat_add(&cq->dev->stats.direct_poll_empty, 1);
            return n;   /* decoded in userspace (possibly 0) */
        }
        if (n == RDMA_DIRECT_FALLBACK_UNKNOWN_QP)
            rdma_stat_add(&cq->dev->stats.fallback_unknown_qp, 1);
        else if (n == RDMA_DIRECT_FALLBACK_METADATA)
            rdma_stat_add(&cq->dev->stats.fallback_missing_metadata, 1);
        else
            rdma_stat_add(&cq->dev->stats.fallback_dext_owned, 1);
        /* n < 0: unusual CQE. Reconcile the DEXT's dormant consumer index
         * before it decodes the same entry; this call is exceptional and is
         * never made for ordinary trusted-path completions. */
        for (rdma_qp *qp = cq->dev->qps; qp; qp = qp->next)
            rdma_qp_publish_shadow_force(qp);
        if (rdma_update_cq_consumer(cq, cq->consumer_index) != 0)
            return -EIO;
    } else if (!direct_requested) {
        rdma_stat_add(&cq->dev->stats.fallback_direct_disabled, 1);
    } else {
        rdma_stat_add(&cq->dev->stats.fallback_cq_unmapped, 1);
    }
    rdma_stat_add(&cq->dev->stats.kernel_poll_calls, 1);
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
        /* Ownership is split on this path and each side fills what it owns.
         * The DEXT owns the shared receive queue, so it resolved the receive.
         * A trusted fast-path send queue is mapped into this process, so its
         * per-slot work-request ids live here and the DEXT can only guess;
         * its own decoder says as much. Supply the send id from the shadow. */
        if (resp.wc[i].opcode != RDMA_WC_RECV) {
            rdma_qp *sqp = rdma_find_qp_token(cq->dev, resp.wc[i].qpNum);
            if (sqp) {
                wc[i].qp_num = sqp->hw_qpn;
                if (sqp->direct_sq && sqp->sq_wrid && sqp->sq_size) {
                    uint32_t sidx = resp.wc[i].wqeCounter & (sqp->sq_size - 1);
                    wc[i].wr_id = sqp->sq_wrid[sidx];
                }
            }
        }
        if (resp.wc[i].opcode == RDMA_WC_RECV) {
            rdma_qp *qp = rdma_find_qp_token(cq->dev, resp.wc[i].qpNum);
            if (qp) {
                wc[i].qp_num = qp->hw_qpn;
                uint64_t done = (qp->rq_head & ~0xffffULL) |
                                resp.wc[i].wqeCounter;
                if (done > qp->rq_head) done -= 0x10000ULL;
                if (done + 1 > qp->rq_tail) qp->rq_tail = done + 1;
                rdma_qp_publish_shadow_maybe(qp, RDMA_QP_SHADOW_RQ_TAIL);
            }
        }
        if (resp.wc[i].opcode != RDMA_WC_RECV) {
            rdma_qp *qp = rdma_find_qp_token(cq->dev, resp.wc[i].qpNum);
            if (qp) {
                wc[i].qp_num = qp->hw_qpn;
                if (qp->direct_sq) {
                    uint32_t idx = resp.wc[i].wqeCounter & (qp->sq_size - 1);
                    /* The DEXT decodes this CQE without per-slot metadata
                     * (a trusted fast-path SQ posts without SyncFastPath, so
                     * the shared shadow carries counters only), which makes
                     * its wr_id/opcode best-effort. The shim posted the WQE
                     * itself and still holds the real values here — use them
                     * instead of the DEXT's guess. */
                    if (qp->sq_wrid && qp->sq_opcode) {
                        uint8_t wrop = qp->sq_opcode[idx];
                        wc[i].wr_id = qp->sq_wrid[idx];
                        wc[i].opcode =
                            (wrop == RDMA_WR_RDMA_WRITE ||
                             wrop == RDMA_WR_RDMA_WRITE_IMM)
                                ? RDMA_WC_RDMA_WRITE
                            : (wrop == RDMA_WR_RDMA_READ)
                                ? RDMA_WC_RDMA_READ
                            : (wrop == RDMA_WR_ATOMIC_CS)
                                ? RDMA_WC_COMP_SWAP
                            : (wrop == RDMA_WR_ATOMIC_FA)
                                ? RDMA_WC_FETCH_ADD
                                : RDMA_WC_SEND;
                    }
                    uint64_t done = (qp->sq_head & ~0xffffULL) |
                                    resp.wc[i].wqeCounter;
                    if (done > qp->sq_head) done -= 0x10000ULL;
                    uint64_t tail = done + (qp->sq_span ? qp->sq_span[idx] : 1);
                    if (tail > qp->sq_tail) {
                        uint64_t old_tail = qp->sq_tail;
                        qp->sq_tail = tail;
                        while (old_tail < tail) {
                            uint32_t retired = (uint32_t)old_tail &
                                               (qp->sq_size - 1);
                            uint8_t span = qp->sq_span ? qp->sq_span[retired] : 0;
                            if (qp->sq_span) qp->sq_span[retired] = 0;
                            if (qp->sq_wrid) qp->sq_wrid[retired] = 0;
                            if (qp->sq_opcode) qp->sq_opcode[retired] = 0;
                            if (qp->sq_atomic_result)
                                qp->sq_atomic_result[retired] = 0;
                            old_tail += span ? span : 1;
                        }
                    }
                }
                rdma_qp_publish_shadow_maybe(qp, RDMA_QP_SHADOW_SQ_TAIL);
            }
        }
    }
    cq->consumer_index += resp.count;
    cq->synced_consumer_index = cq->consumer_index;
    /* CQE decode remains DEXT-owned; publish the resulting consumer index
     * directly through this CQ's isolated DB record. */
    if (resp.count) rdma_publish_cq_consumer(cq, 0);
    rdma_stat_add(&cq->dev->stats.kernel_cqes, resp.count);
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
    if (kr != kIOReturnSuccess) return -EIO;
    cq->consumer_index = consumer_index;
    cq->published_consumer_index = consumer_index;
    cq->synced_consumer_index = consumer_index;
    return 0;
}

int rdma_modify_cq_moderation(rdma_cq *cq, uint32_t period, uint32_t max_count)
{
    if (!cq || period > 0xfffu || max_count > 0xffffu) return -EINVAL;
    struct mlx_modify_cq_moderation_req req = {
        .cqHandle = cq->cq_handle,
        .cqPeriod = (uint16_t)period,
        .cqMaxCount = (uint16_t)max_count,
    };
    kern_return_t kr = IOConnectCallStructMethod(cq->dev->conn,
        kMlxUCMethodModifyCqModeration, &req, sizeof(req), NULL, 0);
    if (kr == kIOReturnUnsupported) return -ENOTSUP;
    return kr == kIOReturnSuccess ? 0 : -EIO;
}

int rdma_arm_cq(rdma_cq *cq, int solicited_only)
{
    if (!cq) return -EINVAL;
    /* ArmCQ encodes the DEXT object's CI in dbrec[1], so a trusted userspace
     * consumer must reconcile it at this control boundary. */
    if (cq->consumer_index != cq->synced_consumer_index &&
        rdma_update_cq_consumer(cq, cq->consumer_index) != 0)
        return -EIO;
    rdma_publish_cq_consumer(cq, 1);
    struct mlx_arm_cq_req req = { .cqHandle = cq->cq_handle,
                                   .solicitedOnly = solicited_only ? 1 : 0 };
    kern_return_t kr = IOConnectCallStructMethod(cq->dev->conn,
        kMlxUCMethodArmCQ, &req, sizeof(req), NULL, 0);
    return kr == kIOReturnSuccess ? 0 :
           (kr == kIOReturnNotFound ? -ENOENT : -EIO);
}

int rdma_sync_qp_tails(rdma_qp *qp, uint64_t sq_tail, uint64_t rq_tail)
{
    if (!qp) return -EINVAL;
    rdma_qp_publish_shadow_force(qp);
    struct mlx_sync_qp_tails_req req = {
        .qpn = qp->qpn, .sqTail = sq_tail, .rqTail = rq_tail
    };
    kern_return_t kr = IOConnectCallStructMethod(
        qp->dev->conn, kMlxUCMethodSyncQpTails, &req, sizeof(req), NULL, 0);
    return kr == kIOReturnSuccess ? 0 :
           (kr == kIOReturnBusy ? -EAGAIN : -EIO);
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
    req.srqn          = init->srqn;
    /* Option B: user buffers are never exposed as hardware WQs. */
    req.sqBufAddr     = 0;
    req.rqBufAddr     = 0;
    req.maxInlineData = init->max_inline_data;
    /* The trusted fast path is decided before CreateQP. Capability support is
     * the default; environment variables are explicit diagnostic opt-outs. */
    const char *trusted_env = getenv("MELONDMA_TRUSTED_FAST_PATH");
    int want_trusted =
        (pd->dev->abi_features & RDMA_FEATURE_TRUSTED_FAST_PATH) != 0 &&
        rdma_env_default_on("MELONDMA_DIRECT_UAR") &&
        rdma_env_default_on("MELONDMA_DIRECT_CQ") &&
        pd->dev->uar_map[0] && pd->dev->db_map[0] &&
        init->send_cq->cqe_buf && init->recv_cq->cqe_buf &&
        !(trusted_env && strcmp(trusted_env, "0") == 0);
    req.rsvd = want_trusted ? MLX_UC_QP_TRUSTED : 0;
    /* Scatter-to-CQE opt-in for the loopback gate: the byte-exact check in
     * mlx_rtt_bench proves the scattered receive payload is copied back. */
    if (getenv("MELONDMA_SCATTER_CQE"))
        req.rsvd |= MLX_UC_QP_SCATTER_CQE;
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
    qp->srqn = init->srqn;
    qp->is_ud = (init->qp_type == RDMA_QPT_UD);
    qp->is_uc = (init->qp_type == RDMA_QPT_UC);
    if (init->srqn || init->qp_type == RDMA_QPT_UD) {
        /* One owner for this pair's state. A datagram posts through the driver
         * because its address vector lives there, so its completions must be
         * decoded there too: otherwise the driver never sees the send retire
         * and refuses to destroy the pair, and the routing-header flag the
         * driver sets never reaches the caller. */
        if (init->send_cq) init->send_cq->kernel_poll_only = 1;
        if (init->recv_cq) init->recv_cq->kernel_poll_only = 1;
    }
    qp->sq_size = init->cap_sq; qp->rq_size = init->cap_rq;
    qp->sq_buf = NULL; qp->rq_buf = NULL;
    qp->sq_wrid = calloc(qp->sq_size, sizeof(*qp->sq_wrid));
    qp->sq_opcode = calloc(qp->sq_size, sizeof(*qp->sq_opcode));
    qp->sq_span = calloc(qp->sq_size, sizeof(*qp->sq_span));
    qp->sq_atomic_result = calloc(qp->sq_size,
                                   sizeof(*qp->sq_atomic_result));
    qp->rq_wrid = calloc(qp->rq_size, sizeof(*qp->rq_wrid));
    qp->rq_sge_addr = calloc(qp->rq_size, sizeof(*qp->rq_sge_addr));
    if (!qp->sq_wrid || !qp->sq_opcode || !qp->sq_span ||
        !qp->sq_atomic_result || !qp->rq_wrid || !qp->rq_sge_addr) {
        (void)IOConnectCallStructMethod(pd->dev->conn,
                                        kMlxUCMethodDestroyQP,
                                        &resp.qpn, sizeof(resp.qpn), NULL, 0);
        free(qp->sq_wrid);
        free(qp->sq_opcode);
        free(qp->sq_span);
        free(qp->sq_atomic_result);
        free(qp->rq_wrid);
        free(qp->rq_sge_addr);
        free(qp);
        return NULL;
    }
    qp->bf_offset = resp.bfOffset;
    qp->bf_buf_size = resp.bfBufSize;
    qp->bf_shared = (resp.bfFlags & MLX_QP_BF_SHARED) != 0;
    /* Asking for blue flame on a card that reports none is silent otherwise:
     * bf_buf_size stays zero, every post falls back to the plain doorbell, and
     * the run looks identical to one that never asked. */
    if (!resp.bfBufSize && rdma_env_default_on("MELONDMA_BLUE_FLAME"))
        SHIM_LOG("QP[%u] blue flame requested but the card reports no BF "
                 "register; posts use the plain doorbell", resp.qpn);
    if (qp->bf_shared)
        SHIM_LOG("QP[%u] shares blue-flame register 0x%x with another QP of "
                 "this client; concurrent posts serialise on it",
                 resp.qpn, resp.bfOffset);
    qp->db_record_offset = resp.dbRecordOffset;
    qp->sq_stride = resp.sqStrideSize;
    /* Both offsets are flat across this client's pools. Resolve each once
     * here, mapping the page if this is the first QP to land on it, so the
     * post path adds no indirection at all. */
    {
        const uint32_t uarPageSize = (uint32_t)pd->dev->uar_map_size;
        const uint32_t uarSlot = mlxFlatPage(resp.bfOffset, uarPageSize);
        const uint32_t bfWithin = mlxFlatWithin(resp.bfOffset, uarPageSize);
        uint8_t *uarBase = (uint8_t *)rdma_uar_slot(pd->dev, uarSlot);
        if (uarBase && bfWithin + sizeof(uint64_t) <= pd->dev->uar_map_size)
            qp->bf_reg = uarBase + bfWithin;
        const uint32_t dbPage = mlxFlatPage(resp.dbRecordOffset,
                                            MLX_CLIENT_DB_PAGE_SIZE);
        const uint32_t dbWithin = mlxFlatWithin(resp.dbRecordOffset,
                                                MLX_CLIENT_DB_PAGE_SIZE);
        uint8_t *dbBase = (uint8_t *)rdma_db_page(pd->dev, dbPage);
        if (dbBase && dbWithin + 8 <= pd->dev->db_map_size)
            qp->db_rec = dbBase + dbWithin;
    }
    rdma_bf_bind(qp);
    /* On by default, matching rdma-core's mlx5 provider and the contract the
     * libibverbs layer here advertises. A caller that serialises its own posts
     * can take the last nanoseconds back. */
    qp->post_serialize = rdma_env_default_on("MELONDMA_SERIALIZE_POST");

    /* Direct SQ is capability-driven and enabled by default when the DEXT
     * returned the exact isolated mapping contract. Mapping failure remains a
     * transparent compatibility fallback. */
    if (rdma_env_default_on("MELONDMA_DIRECT_UAR") && qp->bf_reg && qp->db_rec &&
        resp.mappingVersion == MLX_FAST_PATH_ABI_VERSION &&
        resp.sqStrideSize == 64) {
        mach_vm_address_t sq = 0;
        mach_vm_size_t sq_size = 0;
        kern_return_t mapkr = IOConnectMapMemory64(
            pd->dev->conn, MLX_UC_MEM_TYPE(kMlxUCMemKindSq, resp.qpn),
            mach_task_self(), &sq, &sq_size, kIOMapAnywhere);
        if (mapkr == kIOReturnSuccess && sq_size >=
            (mach_vm_size_t)init->cap_sq * 64) {
            qp->sq_buf = (void *)(uintptr_t)sq;
            /* A datagram request carries a 48-byte address vector taken from
             * the handle, and those handles live in the DEXT. Encoding one
             * here would mean mapping them into an untrusted client, which is
             * what this whole design avoids, so such a pair posts through the
             * driver where the encoder already is. The receive ring is still
             * mapped: only the send side needs the vector. */
            /* UC stays on the validated kernel posting path until its own
             * direct-WQE gate exists; it must never inherit RC READ/atomic. */
            qp->direct_sq = (init->qp_type == RDMA_QPT_RC) ? 1 : 0;
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
            rdma_stat_add(&pd->dev->stats.mapped_qps, 1);
            qp->trusted_fast_path = want_trusted && init->qp_type != RDMA_QPT_UD;
            rdma_qp_publish_shadow_force(qp);
            /* Zeroing bf_buf_size is what disables blue flame for this QP:
             * every post then falls back to the plain 64-bit doorbell. The
             * bounds check stays mandatory — a register that does not fit in
             * the client's own UAR mapping must never be written. */
            if (!(pd->dev->abi_features & RDMA_FEATURE_BLUE_FLAME) ||
                !rdma_env_default_on("MELONDMA_BLUE_FLAME") ||
                !qp->bf_buf_size || qp->bf_buf_size > 2048u ||
                (size_t)mlxFlatWithin(resp.bfOffset,
                                      (uint32_t)pd->dev->uar_map_size) +
                    2u * qp->bf_buf_size > pd->dev->uar_map_size)
                qp->bf_buf_size = 0;
            SHIM_LOG("QP[%u] direct SQ mapped size=%llu uar=%u bf=0x%x bf_buf=%u db=0x%x",
                     qp->qpn, (unsigned long long)sq_size, resp.uarPage,
                     resp.bfOffset, qp->bf_buf_size, resp.dbRecordOffset);
        } else {
            SHIM_LOG("QP[%u] direct SQ NOT mapped: kr=0x%x size=%llu need=%llu",
                     resp.qpn, mapkr, (unsigned long long)sq_size,
                     (unsigned long long)init->cap_sq * 64);
            if (mapkr == kIOReturnSuccess)
                (void)IOConnectUnmapMemory(
                    pd->dev->conn, MLX_UC_MEM_TYPE(kMlxUCMemKindSq, resp.qpn),
                    mach_task_self(), sq);
        }
    } else {
        SHIM_LOG("QP[%u] direct SQ skipped: uar=%p db=%p mappingVersion=%u "
                 "stride=%u bfOffset=0x%x dbOffset=0x%x uar_size=%zu db_size=%zu",
                 resp.qpn, pd->dev->uar_map[0], pd->dev->db_map[0],
                 resp.mappingVersion, resp.sqStrideSize, resp.bfOffset,
                 resp.dbRecordOffset, pd->dev->uar_map_size,
                 pd->dev->db_map_size);
    }
    qp->next = pd->dev->qps;
    pd->dev->qps = qp;
    uint32_t bucket = rdma_qpn_hash(qp->hw_qpn);
    qp->hash_next = pd->dev->qpn_buckets[bucket];
    pd->dev->qpn_buckets[bucket] = qp;
    return qp;
}

int rdma_destroy_qp(rdma_qp *qp)
{
    if (!qp) return -EINVAL;
    rdma_qp_publish_shadow_force(qp);
    /* Normal verbs teardown is allowed with posted receives and unsignaled
     * sends. Move the QP to RESET first; firmware flushes the WQs and no DMA
     * can reference their MRs after the transition completes. */
    if (qp->state != RDMA_QPS_RESET &&
        ((qp->direct_sq && qp->sq_head != qp->sq_tail) ||
         (qp->direct_rq && qp->rq_head != qp->rq_tail))) {
        struct rdma_qp_attr reset = {
            .cur_state = qp->state,
            .new_state = RDMA_QPS_RESET,
            .attr_mask = 1u, /* IBV_QP_STATE */
        };
        if (rdma_modify_qp(qp, &reset) != 0) return -EBUSY;
        qp->sq_tail = qp->sq_head;
        qp->rq_tail = qp->rq_head;
        if (rdma_sync_qp_tails(qp, qp->sq_tail, qp->rq_tail) != 0)
            return -EBUSY;
        if (qp->sq_span) memset(qp->sq_span, 0, qp->sq_size);
        rdma_qp_publish_shadow_force(qp);
    }
    /* Do not tear down mappings and then discover that firmware correctly
     * refused a live QP.  The trusted counters are monotonic and direct CQ
     * polling advances their tails, so this is a cheap deterministic guard. */
    if ((qp->direct_sq && qp->sq_head != qp->sq_tail) ||
        (qp->direct_rq && qp->rq_head != qp->rq_tail))
        return -EBUSY;
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
    uint32_t bucket = rdma_qpn_hash(qp->hw_qpn);
    cursor = &qp->dev->qpn_buckets[bucket];
    while (*cursor && *cursor != qp) cursor = &(*cursor)->hash_next;
    if (*cursor == qp) *cursor = qp->hash_next;
    free(qp->sq_wrid);
    free(qp->sq_opcode);
    free(qp->sq_span);
    free(qp->sq_atomic_result);
    free(qp->rq_wrid);
    free(qp->rq_sge_addr);
    free(qp);
    return 0;
}

int rdma_modify_qp(rdma_qp *qp, const struct rdma_qp_attr *attr)
{
    if (!qp || !attr) return -EINVAL;
    rdma_qp_publish_shadow_force(qp);
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
    req.qkey           = attr->qkey;
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
    if (kr != kIOReturnSuccess) {
        /* Collapsing every driver refusal into one errno hides which check
         * fired; the transition that failed is rarely guessable without it. */
        SHIM_LOG("modify_qp %u->%u refused: kr=0x%x mask=0x%x qkey=0x%x",
                 attr->cur_state, attr->new_state, kr, attr->attr_mask,
                 attr->qkey);
        return -EIO;
    }
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
    rdma_qp_publish_shadow_force(qp);
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

/* Why the last state transition on this pair was refused. The driver maps
 * several firmware outbox statuses onto one return code, so the syndrome is
 * the only value that names the field, and the driver's log does not reach
 * userspace on this machine. */
int rdma_qp_last_refusal(rdma_qp *qp, uint32_t *fw_status, uint32_t *syndrome)
{
    if (!qp || !fw_status || !syndrome) return -EINVAL;
    struct mlx_qp_refusal_resp resp = {};
    size_t out = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(
        qp->dev->conn, kMlxUCMethodQpLastRefusal, &qp->qpn, sizeof(qp->qpn),
        &resp, &out);
    if (kr != kIOReturnSuccess || out != sizeof(resp)) return -EIO;
    *fw_status = resp.fwStatus;
    *syndrome = resp.syndrome;
    return 0;
}

int rdma_query_qp_marking(rdma_qp *qp, struct rdma_qp_marking *marking)
{
    if (!qp || !marking) return -EINVAL;
    rdma_qp_publish_shadow_force(qp);
    struct mlx_query_qp_resp resp = {};
    size_t out = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(
        qp->dev->conn, kMlxUCMethodQueryQP, &qp->qpn, sizeof(qp->qpn),
        &resp, &out);
    if (kr != kIOReturnSuccess || out != sizeof(resp) || resp.qpn != qp->qpn)
        return -EIO;
    marking->sl = resp.sl;
    marking->traffic_class = resp.trafficClass;
    marking->dscp = resp.dscp;
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

int rdma_query_cap_regs(rdma_device *dev, struct rdma_cap_regs *caps)
{
    if (!dev || !caps) return -EINVAL;
    struct mlx_query_cap_regs_resp resp = {};
    size_t out = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(
        dev->conn, kMlxUCMethodQueryCapRegs, NULL, 0, &resp, &out);
    if (kr != kIOReturnSuccess || out != sizeof(resp)) return -EIO;
    caps->version = resp.version;
    caps->valid_mask = resp.validMask;
    memcpy(caps->pcam, resp.pcam, sizeof(caps->pcam));
    memcpy(caps->mcam, resp.mcam, sizeof(caps->mcam));
    memcpy(caps->qcam, resp.qcam, sizeof(caps->qcam));
    return 0;
}

int rdma_query_port_stats(rdma_device *dev, struct rdma_port_stats *stats)
{
    if (!dev || !stats) return -EINVAL;
    struct mlx_port_stats_resp resp = {};
    size_t out = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(
        dev->conn, kMlxUCMethodPortStats, NULL, 0, &resp, &out);
    if (kr != kIOReturnSuccess || out != sizeof(resp)) return -EIO;
    stats->rx_pkts = resp.rxPkts;      stats->tx_pkts = resp.txPkts;
    stats->rx_bytes = resp.rxBytes;    stats->tx_bytes = resp.txBytes;
    stats->rx_drop = resp.rxDrop;      stats->tx_drop = resp.txDrop;
    stats->rx_errors = resp.rxErrors;  stats->tx_errors = resp.txErrors;
    stats->rx_pause = resp.rxPause;    stats->tx_pause = resp.txPause;
    stats->link_speed = resp.linkSpeed;
    stats->link_state = resp.linkState;
    stats->port_num = resp.portNum;
    return 0;
}

int rdma_access_reg(rdma_device *dev, uint16_t register_id, int write,
                    uint32_t argument, void *data, uint32_t size)
{
    if (!dev || (size && !data) || size > MLX_UC_ACCESS_REG_MAX_DATA) return -EINVAL;
    struct mlx_access_reg_req req = {};
    struct mlx_access_reg_resp resp = {};
    req.registerId = register_id;
    req.opMod = write ? 0 : 1;
    req.argument = argument;
    req.dataSize = size;
    if (size) memcpy(req.data, data, size);
    size_t out = sizeof(resp);
    kern_return_t kr = IOConnectCallStructMethod(
        dev->conn, kMlxUCMethodAccessReg, &req, sizeof(req), &resp, &out);
    if (kr != kIOReturnSuccess) return kr == kIOReturnNotPermitted ? -EPERM : -EIO;
    if (out != sizeof(resp) || resp.dataSize != size) return -EIO;
    if (size) memcpy(data, resp.data, size);
    return 0;
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
    if (!pd || !addr || !length) { errno = EINVAL; return NULL; }
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
        /* Retry fragmentation only; quota exhaustion and an uncertain device
         * state must not trigger another wave of registrations. */
        errno = kr == kIOReturnNoSpace ? E2BIG :
                kr == kIOReturnNoResources ? ENOSPC :
                kr == kIOReturnNoMemory ? ENOMEM :
                kr == kIOReturnBadArgument ? EINVAL :
                kr == kIOReturnNotReady ? ENODEV : EIO;
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

    /* PostUmrKlm is encoded by the DEXT into the mapped SQ.  In trusted
     * direct-CQ mode userspace still has to mirror that WQE's metadata and
     * producer span, otherwise the completion is treated as an unknown
     * DEXT-only WQE and the local shadow can regress to head=0/tail=1.  That
     * stale shadow makes otherwise successful indirect-MR teardown fail.
     *
     * Layout matches mlxEncodeUmrKlmWqe: 128 fixed bytes plus 16-byte KLMs,
     * with the KLM area rounded to 64 bytes; SQ credits are 64-byte WQEBBs. */
    uint64_t direct_head = 0;
    uint32_t direct_idx = 0;
    uint8_t direct_span = 0;
    if (qp->direct_sq) {
        const uint32_t klm_bytes = child_count * 16u;
        const uint32_t total_bytes = 128u + ((klm_bytes + 63u) & ~63u);
        const uint32_t span = (total_bytes + 63u) / 64u;
        if (!span || span > UINT8_MAX || qp->sq_head != qp->sq_tail ||
            qp->sq_head - qp->sq_tail + span > qp->sq_size)
            return -EAGAIN;
        direct_head = qp->sq_head;
        direct_idx = (uint32_t)direct_head & (qp->sq_size - 1u);
        direct_span = (uint8_t)span;
        qp->sq_wrid[direct_idx] = req.wrId;
        qp->sq_opcode[direct_idx] = 0xffu; /* internal UMR; exposed as SEND WC */
        qp->sq_span[direct_idx] = direct_span;
    }
    kern_return_t kr = IOConnectCallStructMethod(
        qp->dev->conn, kMlxUCMethodPostUmrKlm, &req, sizeof(req), NULL, 0);
    if (kr != kIOReturnSuccess) {
        if (direct_span) {
            qp->sq_wrid[direct_idx] = 0;
            qp->sq_opcode[direct_idx] = 0;
            qp->sq_span[direct_idx] = 0;
        }
        SHIM_LOG("activate_indirect_mr: post failed kr=0x%x", kr);
        return kr == kIOReturnBusy ? -EAGAIN : -EIO;
    }
    if (direct_span)
        qp->sq_head = direct_head + direct_span;
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
    /* DbgsExec sits behind the com.melondma.rdma.diagnostic entitlement; a
     * refusal is a client-permission answer, not a firmware verdict, and the
     * caller must be able to tell the two apart. */
    if (kr == kIOReturnNotPermitted) return -EPERM;
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

uint32_t rdma_ah_handle(rdma_ah *ah)
{
    return ah ? ah->ah_handle : 0;
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
static uint32_t rdma_encode_prebuilt_wqe64(rdma_qp *qp, void *out,
                                           uint16_t producer, uint8_t opcode,
                                           const struct rdma_sge *sge,
                                           uint64_t remote_addr, uint32_t rkey,
                                           uint32_t send_flags)
{
    if (!qp || !out || !sge || !sge->length) return 0;
    int kind = opcode == MLX_OPCODE_SEND ? 0 :
               opcode == MLX_OPCODE_RDMA_WRITE ? 1 :
               opcode == MLX_OPCODE_RDMA_READ ? 2 : -1;
    if (kind < 0) return 0;
    uint32_t key = ((send_flags & RDMA_SEND_SIGNALED) ? 1u : 0u) |
                   ((send_flags & RDMA_SEND_FENCE) ? 2u : 0u) |
                   ((send_flags & RDMA_SEND_SOLICITED) ? 4u : 0u);
    if (!(qp->wqe_template_valid[kind] & (1u << key))) {
        struct MlxRcSge seed = { .addr = 1, .length = 1, .lkey = 1 };
        if (!mlxEncodeRcSendWqe(qp->wqe_template[kind][key], 64,
                                qp->hw_qpn, 0, opcode, &seed, 1, 1, 1,
                                (key & 1u) != 0, (key & 2u) != 0,
                                (key & 4u) != 0))
            return 0;
        qp->wqe_template_valid[kind] |= (uint8_t)(1u << key);
    }
    memcpy(out, qp->wqe_template[kind][key], 64);
    struct MlxWqeCtrlSeg *ctrl = (struct MlxWqeCtrlSeg *)out;
    ctrl->opmod_idx_opcode = MLX_BE32(((uint32_t)producer << 8) | opcode);
    uint32_t data_offset = sizeof(*ctrl);
    if (kind != 0) {
        struct MlxWqeRaddrSeg *remote = (struct MlxWqeRaddrSeg *)
            ((uint8_t *)out + data_offset);
        remote->raddr = MLX_BE64(remote_addr);
        remote->rkey = MLX_BE32(rkey);
        data_offset += sizeof(*remote);
    }
    struct MlxWqeDataSeg *data = (struct MlxWqeDataSeg *)
        ((uint8_t *)out + data_offset);
    data->byte_count = MLX_BE32(sge->length);
    data->lkey = MLX_BE32(sge->lkey);
    data->addr = MLX_BE64(sge->addr);
    return kind == 0 ? 2u : 3u;
}

static int rdma_post_send_direct(rdma_qp *qp,
                                 const struct rdma_send_wr *wr,
                                 uint32_t count)
{
    uint32_t post_chunk = 64;
    const char *batch_env = getenv("MELONDMA_POST_BATCH");
    if (batch_env && strcmp(batch_env, "16") == 0) post_chunk = 16;
    if (!qp || !wr || !qp->direct_sq || !qp->sq_buf ||
        !qp->dev->uar_map[0] || !qp->dev->db_map[0] || !count ||
        count > post_chunk || qp->state != RDMA_QPS_RTS)
        return -EINVAL;
    /* The DEXT validates the same producer/consumer distance under its QP
     * lock. Keep posting until the local ring is full; CQ polling advances
     * sq_tail from the returned WQE counter. */
    if (qp->sq_head - qp->sq_tail + count > qp->sq_size)
        return -EAGAIN;

    struct mlx_sync_fast_path_req sync;
    if (!qp->trusted_fast_path) {
        memset(&sync, 0, offsetof(struct mlx_sync_fast_path_req, wr) +
                         count * sizeof(sync.wr[0]));
        sync.count = count;
    }
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
        if (!rdma_encode_prebuilt_wqe64(qp, (void *)(uintptr_t)last,
                                        (uint16_t)(head + i), opcode,
                                        &wr[i].sg_list[0], wr[i].remote_addr,
                                        wr[i].rkey, wr[i].send_flags))
            return -EINVAL;
        if (!qp->trusted_fast_path) {
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
        if (qp->sq_wrid) {
            qp->sq_wrid[index] = wr[i].wr_id;
            qp->sq_opcode[index] = (uint8_t)wr[i].opcode;
            qp->sq_span[index] = 1;
        }
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
    if (!qp->trusted_fast_path) {
        kern_return_t kr = IOConnectCallStructMethod(
            qp->dev->conn, kMlxUCMethodSyncFastPath,
            &sync, sizeof(sync), NULL, 0);
        if (kr != kIOReturnSuccess)
            return kr == kIOReturnBusy ? -EAGAIN : -EIO;
    }
    if (!qp->trusted_fast_path && getenv("MELONDMA_DEBUG_POST"))
        SHIM_LOG("QP[%u] direct SQ synchronized count=%u", qp->qpn, count);

    rdma_dma_write_barrier();
    volatile uint32_t *db = (volatile uint32_t *)
        (qp->db_rec);
    db[1] = __builtin_bswap32((uint32_t)(head + count) & 0xffffu);
    rdma_dma_write_barrier();

    uint64_t doorbell = 0;
    memcpy(&doorbell, (const void *)(uintptr_t)last, sizeof(doorbell));
    rdma_ring_send_doorbell(qp, doorbell);

    qp->sq_head = head + count;
    rdma_qp_publish_shadow_maybe(qp, RDMA_QP_SHADOW_SQ_HEAD);
    rdma_stat_add(&qp->dev->stats.direct_send_batches, 1);
    rdma_stat_add(&qp->dev->stats.direct_send_wrs, count);
    rdma_stat_add(&qp->dev->stats.direct_doorbells, 1);
    return 0;
}

static int rdma_post_send_direct_sge(rdma_qp *qp,
                                     const struct rdma_send_wr *wr)
{
    int zero_write_imm = wr && wr->opcode == RDMA_WR_RDMA_WRITE_IMM &&
                         wr->num_sge == 0;
    if (!qp || !wr || !qp->direct_sq ||
        (!zero_write_imm && (!wr->sg_list || wr->num_sge < 1)) ||
        wr->num_sge > RDMA_MAX_SGE ||
        (wr->send_flags & ~(RDMA_SEND_SIGNALED | RDMA_SEND_FENCE |
                            RDMA_SEND_SOLICITED)) ||
        qp->state != RDMA_QPS_RTS)
        return -EINVAL;
    uint8_t opcode = wr->opcode == RDMA_WR_RDMA_WRITE ? MLX_OPCODE_RDMA_WRITE :
                     wr->opcode == RDMA_WR_RDMA_READ ? MLX_OPCODE_RDMA_READ :
                     wr->opcode == RDMA_WR_RDMA_WRITE_IMM ? MLX_OPCODE_RDMA_WRITE_IMM :
                     wr->opcode == RDMA_WR_SEND_IMM ? MLX_OPCODE_SEND_IMM :
                     wr->opcode == RDMA_WR_SEND_INV ? MLX_OPCODE_SEND_INVAL :
                     wr->opcode == RDMA_WR_LOCAL_INV ? MLX_OPCODE_LOCAL_INVAL : MLX_OPCODE_SEND;
    struct MlxRcSge sges[RDMA_MAX_SGE] = {};
    for (uint32_t i = 0; i < (uint32_t)wr->num_sge; i++) {
        sges[i].addr = wr->sg_list[i].addr;
        sges[i].length = wr->sg_list[i].length;
        sges[i].lkey = wr->sg_list[i].lkey;
    }
    uint8_t flat[256] = {};
    uint32_t ds = opcode == MLX_OPCODE_SEND_INVAL ?
        mlxEncodeRcSendInvalidateWqe(flat, sizeof(flat), qp->hw_qpn,
                                     (uint16_t)qp->sq_head, sges,
                                     (uint32_t)wr->num_sge, wr->rkey,
                                     (wr->send_flags & RDMA_SEND_SIGNALED) != 0,
                                     (wr->send_flags & RDMA_SEND_FENCE) != 0,
                                     (wr->send_flags & RDMA_SEND_SOLICITED) != 0) :
        (opcode == MLX_OPCODE_SEND_IMM || opcode == MLX_OPCODE_RDMA_WRITE_IMM) ?
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
    if (!qp->trusted_fast_path) {
        struct mlx_sync_send_sge_req sync = {
            .qpn = qp->qpn, .opcode = wr->opcode, .wrId = wr->wr_id,
            .numSge = (uint32_t)wr->num_sge, .sendFlags = wr->send_flags,
            .remoteAddr = wr->remote_addr, .rkey = wr->rkey,
            .immData = wr->imm_data
        };

        for (uint32_t i = 0; i < (uint32_t)wr->num_sge; i++) {
            sync.sge[i].addr = wr->sg_list[i].addr;
            sync.sge[i].length = wr->sg_list[i].length;
            sync.sge[i].lkey = wr->sg_list[i].lkey;
        }
        kern_return_t kr = IOConnectCallStructMethod(qp->dev->conn,
            kMlxUCMethodSyncSendSge, &sync, sizeof(sync), NULL, 0);
        if (kr != kIOReturnSuccess)
            return kr == kIOReturnBusy ? -EAGAIN : -EIO;
    }
    if (qp->sq_wrid) {
        uint32_t idx = (uint32_t)qp->sq_head & (qp->sq_size - 1);
        qp->sq_wrid[idx] = wr->wr_id;
        qp->sq_opcode[idx] = (uint8_t)wr->opcode;
        qp->sq_span[idx] = (uint8_t)span;
    }
    rdma_dma_write_barrier();
    volatile uint32_t *db = (volatile uint32_t *)
        (qp->db_rec);
    db[1] = __builtin_bswap32((uint32_t)(qp->sq_head + span) & 0xffffu);
    rdma_dma_write_barrier();
    uint64_t doorbell = 0; memcpy(&doorbell, flat, sizeof(doorbell));
    rdma_ring_send_doorbell(qp, doorbell);
    qp->sq_head += span;
    rdma_qp_publish_shadow_maybe(qp, RDMA_QP_SHADOW_SQ_HEAD);
    rdma_stat_add(&qp->dev->stats.direct_send_batches, 1);
    rdma_stat_add(&qp->dev->stats.direct_send_wrs, 1);
    rdma_stat_add(&qp->dev->stats.direct_doorbells, 1);
    if (!qp->trusted_fast_path && getenv("MELONDMA_DEBUG_POST"))
        SHIM_LOG("QP[%u] direct SQ synchronized SGE count=%u span=%u",
                 qp->qpn, wr->num_sge, span);
    return 0;
}

static int rdma_post_send_direct_mixed(rdma_qp *qp,
                                       const struct rdma_send_wr *wr,
                                       uint32_t count)
{
    if (!qp || !wr || !count || count > RDMA_POST_CHUNK ||
        !qp->direct_sq || !qp->trusted_fast_path || !qp->sq_buf ||
        !qp->dev->uar_map[0] || !qp->dev->db_map[0] || qp->state != RDMA_QPS_RTS)
        return -EINVAL;

    uint8_t spans[RDMA_POST_CHUNK];
    uint32_t total_span = 0;
    for (uint32_t i = 0; i < count; i++) {
        const struct rdma_send_wr *cur = &wr[i];
        if (cur->send_flags & ~(RDMA_SEND_SIGNALED | RDMA_SEND_FENCE |
                                RDMA_SEND_SOLICITED | RDMA_SEND_INLINE))
            return -EINVAL;
        uint32_t ds = 0;
        if (cur->send_flags & RDMA_SEND_INLINE) {
            const int inline_write = cur->opcode == RDMA_WR_RDMA_WRITE ||
                                     cur->opcode == RDMA_WR_RDMA_WRITE_IMM;
            if ((cur->opcode != RDMA_WR_SEND &&
                 cur->opcode != RDMA_WR_SEND_IMM && !inline_write) ||
                cur->num_sge != 1 ||
                !cur->sg_list || !cur->sg_list[0].addr ||
                !cur->sg_list[0].length ||
                cur->sg_list[0].length > RDMA_MAX_INLINE_DATA ||
                (inline_write && (!cur->remote_addr || !cur->rkey)))
                return -EINVAL;
            /* ctrl(16), plus the remote address segment on a write, plus the
             * inline segment header and payload, rounded to 16 bytes. Must
             * match mlxEncodeRcInlineWqe or the span reserved here and the
             * WQE written later disagree. */
            ds = ((inline_write ? 32u : 16u) + 4u +
                  cur->sg_list[0].length + 15u) / 16u;
        } else if (cur->num_sge == 1 &&
                   cur->opcode != RDMA_WR_SEND_IMM &&
                   cur->opcode != RDMA_WR_RDMA_WRITE_IMM) {
            if (!cur->sg_list || !cur->sg_list[0].length ||
                (cur->opcode != RDMA_WR_SEND &&
                 cur->opcode != RDMA_WR_RDMA_WRITE &&
                 cur->opcode != RDMA_WR_RDMA_READ))
                return -EINVAL;
            ds = cur->opcode == RDMA_WR_SEND ? 2u : 3u;
        } else {
            int remote = cur->opcode == RDMA_WR_RDMA_WRITE ||
                         cur->opcode == RDMA_WR_RDMA_WRITE_IMM ||
                         cur->opcode == RDMA_WR_RDMA_READ;
            int zero_write_imm = cur->opcode == RDMA_WR_RDMA_WRITE_IMM &&
                                 cur->num_sge == 0;
            if ((cur->opcode != RDMA_WR_SEND &&
                 cur->opcode != RDMA_WR_SEND_IMM && !remote) ||
                (!zero_write_imm && (!cur->sg_list || !cur->num_sge)) ||
                cur->num_sge > RDMA_MAX_SGE)
                return -EINVAL;
            for (uint32_t s = 0; s < cur->num_sge; s++)
                if (!cur->sg_list[s].length) return -EINVAL;
            ds = 1u + (remote ? 1u : 0u) + cur->num_sge;
        }
        uint32_t span = (ds + 3u) / 4u;
        if (!span || span > UINT8_MAX || total_span > UINT32_MAX - span)
            return -EINVAL;
        spans[i] = (uint8_t)span;
        total_span += span;
    }
    uint64_t head = qp->sq_head;
    if (head - qp->sq_tail + total_span > qp->sq_size) return -EAGAIN;

    uint64_t producer = head;
    uint64_t final_doorbell = 0;
    uint8_t bf_wqe[MLX_WQE_MAX_INLINE + 64] = {};
    for (uint32_t i = 0; i < count; i++) {
        const struct rdma_send_wr *cur = &wr[i];
        uint8_t flat[MLX_WQE_MAX_INLINE + 64] = {};
        uint32_t ds = 0;
        if (cur->send_flags & RDMA_SEND_INLINE) {
            /* Inline applies to one-sided writes too, and that is where it
             * pays: the NIC skips the DMA read of the payload. A write also
             * carries the remote address segment, which the encoder adds. */
            uint8_t hw_opcode =
                cur->opcode == RDMA_WR_SEND_IMM       ? MLX_OPCODE_SEND_IMM :
                cur->opcode == RDMA_WR_RDMA_WRITE     ? MLX_OPCODE_RDMA_WRITE :
                cur->opcode == RDMA_WR_RDMA_WRITE_IMM ? MLX_OPCODE_RDMA_WRITE_IMM :
                                                        MLX_OPCODE_SEND;
            const int is_write = hw_opcode == MLX_OPCODE_RDMA_WRITE ||
                                 hw_opcode == MLX_OPCODE_RDMA_WRITE_IMM;
            ds = mlxEncodeRcInlineWqe(
                flat, sizeof(flat), qp->hw_qpn, (uint16_t)producer, hw_opcode,
                (const void *)(uintptr_t)cur->sg_list[0].addr,
                cur->sg_list[0].length,
                is_write ? cur->remote_addr : 0,
                is_write ? cur->rkey : 0,
                cur->imm_data,
                (cur->send_flags & RDMA_SEND_SIGNALED) != 0,
                (cur->send_flags & RDMA_SEND_FENCE) != 0,
                (cur->send_flags & RDMA_SEND_SOLICITED) != 0);
        } else if (cur->num_sge == 1 &&
                   cur->opcode != RDMA_WR_SEND_IMM &&
                   cur->opcode != RDMA_WR_RDMA_WRITE_IMM) {
            uint8_t hw_opcode = cur->opcode == RDMA_WR_RDMA_WRITE ?
                MLX_OPCODE_RDMA_WRITE :
                cur->opcode == RDMA_WR_RDMA_READ ? MLX_OPCODE_RDMA_READ :
                                                   MLX_OPCODE_SEND;
            ds = rdma_encode_prebuilt_wqe64(qp, flat, (uint16_t)producer,
                                             hw_opcode, &cur->sg_list[0],
                                             cur->remote_addr, cur->rkey,
                                             cur->send_flags);
        } else {
            struct MlxRcSge sges[RDMA_MAX_SGE] = {};
            for (uint32_t s = 0; s < cur->num_sge; s++) {
                sges[s].addr = cur->sg_list[s].addr;
                sges[s].length = cur->sg_list[s].length;
                sges[s].lkey = cur->sg_list[s].lkey;
            }
            uint8_t hw_opcode = cur->opcode == RDMA_WR_RDMA_WRITE ?
                MLX_OPCODE_RDMA_WRITE :
                cur->opcode == RDMA_WR_RDMA_WRITE_IMM ? MLX_OPCODE_RDMA_WRITE_IMM :
                cur->opcode == RDMA_WR_RDMA_READ ? MLX_OPCODE_RDMA_READ :
                cur->opcode == RDMA_WR_SEND_IMM ? MLX_OPCODE_SEND_IMM :
                                                  MLX_OPCODE_SEND;
            ds = (hw_opcode == MLX_OPCODE_SEND_IMM ||
                  hw_opcode == MLX_OPCODE_RDMA_WRITE_IMM) ?
                mlxEncodeRcSendWqeImm(
                    flat, sizeof(flat), qp->hw_qpn, (uint16_t)producer,
                    hw_opcode, cur->num_sge ? sges : NULL, cur->num_sge,
                    cur->remote_addr, cur->rkey, cur->imm_data,
                    (cur->send_flags & RDMA_SEND_SIGNALED) != 0,
                    (cur->send_flags & RDMA_SEND_FENCE) != 0,
                    (cur->send_flags & RDMA_SEND_SOLICITED) != 0) :
                mlxEncodeRcSendWqe(
                    flat, sizeof(flat), qp->hw_qpn, (uint16_t)producer,
                    hw_opcode, sges, cur->num_sge, cur->remote_addr, cur->rkey,
                    (cur->send_flags & RDMA_SEND_SIGNALED) != 0,
                    (cur->send_flags & RDMA_SEND_FENCE) != 0,
                    (cur->send_flags & RDMA_SEND_SOLICITED) != 0);
        }
        if (!ds) return -EINVAL;
        for (uint32_t slot_index = 0; slot_index < spans[i]; slot_index++) {
            volatile uint8_t *slot = (volatile uint8_t *)qp->sq_buf +
                ((producer + slot_index) & (qp->sq_size - 1)) * qp->sq_stride;
            memcpy((void *)(uintptr_t)slot, flat + slot_index * 64, 64);
        }
        uint32_t idx = (uint32_t)producer & (qp->sq_size - 1);
        qp->sq_wrid[idx] = cur->wr_id;
        qp->sq_opcode[idx] = (uint8_t)cur->opcode;
        qp->sq_span[idx] = spans[i];
        producer += spans[i];
        memcpy(&final_doorbell, flat, sizeof(final_doorbell));
        /* Stage the WQE for blue flame whenever there is exactly one of them
         * and it fits in one half of the register. Inline is not the point:
         * blue flame saves the card the DMA read of the WQE itself, and a
         * non-inline WQE still has to be fetched otherwise. The payload is
         * fetched by DMA either way. */
        if (count == 1 && qp->bf_buf_size &&
            (uint32_t)spans[i] * 64u <= qp->bf_buf_size)
            memcpy(bf_wqe, flat, (uint32_t)spans[i] * 64u);
    }

    rdma_dma_write_barrier();
    volatile uint32_t *db = (volatile uint32_t *)
        (qp->db_rec);
    db[1] = __builtin_bswap32((uint32_t)producer & 0xffffu);
    rdma_dma_write_barrier();
    if (!(count == 1 && qp->bf_buf_size &&
          (uint32_t)spans[0] * 64u <= qp->bf_buf_size &&
          rdma_blue_flame_post(qp, bf_wqe, (uint32_t)spans[0] * 64u)))
        rdma_ring_send_doorbell(qp, final_doorbell);
    qp->sq_head = producer;
    rdma_qp_publish_shadow_maybe(qp, RDMA_QP_SHADOW_SQ_HEAD);
    rdma_stat_add(&qp->dev->stats.direct_send_batches, 1);
    rdma_stat_add(&qp->dev->stats.direct_send_wrs, count);
    rdma_stat_add(&qp->dev->stats.direct_doorbells, 1);
    return 0;
}

static int rdma_post_local_inv_unlocked(rdma_qp *qp, uint64_t wr_id, uint32_t rkey)
{
    if (!qp || !rkey) return -EINVAL;
    /* LOCAL_INV mutates the DEXT-owned MR table. Reconcile the trusted shadow
     * once on this exceptional control operation, then mirror the WQEBB span
     * locally so subsequent direct completions remain coherent. */
    if (qp->trusted_fast_path &&
        rdma_sync_qp_tails(qp, qp->sq_tail, qp->rq_tail) != 0)
        return -EIO;
    uint64_t head = qp->sq_head;
    struct mlx_post_local_inv_req req = {
        .qpn = qp->qpn, .invalidateRkey = rkey, .wrId = wr_id,
        .sendFlags = MLX_UC_SEND_SIGNALED
    };
    kern_return_t kr = IOConnectCallStructMethod(qp->dev->conn,
        kMlxUCMethodPostLocalInv, &req, sizeof(req), NULL, 0);
    if (kr == kIOReturnSuccess && qp->direct_sq) {
        uint32_t idx = (uint32_t)head & (qp->sq_size - 1);
        qp->sq_wrid[idx] = wr_id;
        qp->sq_opcode[idx] = RDMA_WR_LOCAL_INV;
        qp->sq_span[idx] = 2;
        qp->sq_head = head + 2;
        rdma_qp_publish_shadow_force(qp);
    }
    return kr == kIOReturnSuccess ? 0 : (kr == kIOReturnBusy ? -EAGAIN : -EIO);
}

static int rdma_post_send_inline_unlocked(rdma_qp *qp, uint64_t wr_id, uint32_t opcode,
                          const void *data, uint32_t len,
                          uint64_t remote_addr, uint32_t rkey,
                          uint32_t imm_data, uint32_t send_flags)
{
    const int is_write = opcode == RDMA_WR_RDMA_WRITE ||
                         opcode == RDMA_WR_RDMA_WRITE_IMM;
    if (!qp || !data || !len || len > RDMA_MAX_INLINE_DATA ||
        (opcode != RDMA_WR_SEND && opcode != RDMA_WR_SEND_IMM && !is_write) ||
        (send_flags & ~(RDMA_SEND_SIGNALED | RDMA_SEND_FENCE |
                        RDMA_SEND_SOLICITED | RDMA_SEND_INLINE)))
        return -EINVAL;
    if (is_write ? (!remote_addr || !rkey) : (remote_addr || rkey))
        return -EINVAL;
    uint32_t native_opcode =
        opcode == RDMA_WR_SEND_IMM       ? MLX_UC_WR_SEND_IMM :
        opcode == RDMA_WR_RDMA_WRITE     ? MLX_UC_WR_RDMA_WRITE :
        opcode == RDMA_WR_RDMA_WRITE_IMM ? MLX_UC_WR_RDMA_WRITE_IMM :
                                           MLX_UC_WR_SEND;
    uint8_t hw_opcode =
        opcode == RDMA_WR_SEND_IMM       ? MLX_OPCODE_SEND_IMM :
        opcode == RDMA_WR_RDMA_WRITE     ? MLX_OPCODE_RDMA_WRITE :
        opcode == RDMA_WR_RDMA_WRITE_IMM ? MLX_OPCODE_RDMA_WRITE_IMM :
                                           MLX_OPCODE_SEND;
    uint32_t native_flags =
        ((send_flags & RDMA_SEND_SIGNALED) ? MLX_UC_SEND_SIGNALED : 0) |
        ((send_flags & RDMA_SEND_FENCE) ? MLX_UC_SEND_FENCE : 0) |
        ((send_flags & RDMA_SEND_SOLICITED) ? MLX_UC_SEND_SOLICITED : 0) |
        MLX_UC_SEND_INLINE;

    if (qp->direct_sq && qp->trusted_fast_path && qp->sq_buf &&
        qp->dev->uar_map[0] && qp->dev->db_map[0] && qp->state == RDMA_QPS_RTS) {
        uint8_t flat[MLX_WQE_MAX_INLINE + 64] = {};
        uint64_t head = qp->sq_head;
        uint32_t ds = mlxEncodeRcInlineWqe(
            flat, sizeof(flat), qp->hw_qpn, (uint16_t)head, hw_opcode,
            data, len, remote_addr, rkey, imm_data,
            (send_flags & RDMA_SEND_SIGNALED) != 0,
            (send_flags & RDMA_SEND_FENCE) != 0,
            (send_flags & RDMA_SEND_SOLICITED) != 0);
        uint32_t span = ds ? (ds * 16u + 63u) / 64u : 0;
        if (!span) return -EINVAL;
        if (head - qp->sq_tail + span > qp->sq_size) return -EAGAIN;
        for (uint32_t i = 0; i < span; i++) {
            volatile uint8_t *slot = (volatile uint8_t *)qp->sq_buf +
                ((head + i) & (qp->sq_size - 1)) * qp->sq_stride;
            memcpy((void *)(uintptr_t)slot, flat + i * 64, 64);
        }
        uint32_t idx = (uint32_t)head & (qp->sq_size - 1);
        qp->sq_wrid[idx] = wr_id;
        qp->sq_opcode[idx] = (uint8_t)opcode;
        qp->sq_span[idx] = (uint8_t)span;
        rdma_dma_write_barrier();
        volatile uint32_t *db = (volatile uint32_t *)
            (qp->db_rec);
        db[1] = __builtin_bswap32((uint32_t)(head + span) & 0xffffu);
        rdma_dma_write_barrier();
        if (!rdma_blue_flame_post(qp, flat, span * 64u)) {
            uint64_t doorbell = 0;
            memcpy(&doorbell, flat, sizeof(doorbell));
            rdma_ring_send_doorbell(qp, doorbell);
        }
        qp->sq_head = head + span;
        rdma_qp_publish_shadow_maybe(qp, RDMA_QP_SHADOW_SQ_HEAD);
        rdma_stat_add(&qp->dev->stats.direct_send_wrs, 1);
        rdma_stat_add(&qp->dev->stats.direct_send_batches, 1);
        rdma_stat_add(&qp->dev->stats.direct_doorbells, 1);
        return 0;
    }

    struct mlx_post_send_inline_req req = {
        .qpn = qp->qpn,
        .opcode = native_opcode,
        .wrId = wr_id,
        .remoteAddr = remote_addr,
        .inlineLen = len,
        .sendFlags = native_flags,
        .immData = imm_data,
        .rkey = rkey,
    };
    memcpy(req.inlineData, data, len);
    kern_return_t kr = IOConnectCallStructMethod(qp->dev->conn,
        kMlxUCMethodPostSendInline, &req, sizeof(req), NULL, 0);
    if (kr == kIOReturnSuccess && qp->sq_wrid) {
        /* Mirror the DEXT's PostSendInline slot tracking so the direct CQ
         * decode can reconstruct this inline send's wr_id (the kernel path
         * advances the DEXT's sqHead but not the shim's).  Same span math,
         * same (uint16_t) wqe counter. */
        uint8_t scratch[MLX_WQE_MAX_INLINE + 64] = {};
        uint32_t ds = mlxEncodeRcInlineWqe(
            scratch, sizeof(scratch), qp->hw_qpn, (uint16_t)qp->sq_head,
            hw_opcode, data, len, remote_addr, rkey, imm_data,
            (send_flags & RDMA_SEND_SIGNALED) != 0,
            (send_flags & RDMA_SEND_FENCE) != 0,
            (send_flags & RDMA_SEND_SOLICITED) != 0);
        uint32_t span = (ds * 16u + 63u) / 64u;
        if (span) {
            uint32_t idx = (uint32_t)qp->sq_head & (qp->sq_size - 1);
            qp->sq_wrid[idx] = wr_id;
            qp->sq_opcode[idx] = (uint8_t)opcode;
            qp->sq_span[idx] = (uint8_t)span;
            qp->sq_head += span;
            rdma_qp_publish_shadow_force(qp);
        }
    }
    return kr == kIOReturnSuccess ? 0 : (kr == kIOReturnBusy ? -EAGAIN : -EIO);
}

static int rdma_post_send_atomic_unlocked(rdma_qp *qp, uint64_t wr_id, uint32_t opcode,
                          uint64_t remote_addr, uint32_t rkey,
                          uint64_t compare, uint64_t swap_add,
                          uint64_t result_addr, uint32_t result_lkey,
                          uint32_t send_flags)
{
    if (!qp || qp->is_uc || !remote_addr || !rkey || (remote_addr & 7) ||
        !result_addr || !result_lkey || (result_addr & 7) ||
        (opcode != RDMA_WR_ATOMIC_CS && opcode != RDMA_WR_ATOMIC_FA) ||
        (send_flags & ~RDMA_SEND_SIGNALED))
        return -EINVAL;
    if (qp->direct_sq && qp->trusted_fast_path && qp->sq_buf &&
        qp->dev->uar_map[0] && qp->dev->db_map[0] && qp->state == RDMA_QPS_RTS) {
        uint64_t head = qp->sq_head;
        if (head - qp->sq_tail + 1 > qp->sq_size) return -EAGAIN;
        uint8_t flat[64] = {};
        uint32_t ds = mlxEncodeRcAtomicWqe(
            flat, sizeof(flat), qp->hw_qpn, (uint16_t)head,
            opcode == RDMA_WR_ATOMIC_CS ? MLX_OPCODE_ATOMIC_CS :
                                          MLX_OPCODE_ATOMIC_FA,
            remote_addr, rkey, compare, swap_add, result_addr, result_lkey,
            (send_flags & RDMA_SEND_SIGNALED) != 0);
        if (!ds) return -EINVAL;
        uint32_t idx = (uint32_t)head & (qp->sq_size - 1);
        volatile uint8_t *slot = (volatile uint8_t *)qp->sq_buf +
                                 idx * qp->sq_stride;
        memcpy((void *)(uintptr_t)slot, flat, sizeof(flat));
        qp->sq_wrid[idx] = wr_id;
        qp->sq_opcode[idx] = (uint8_t)opcode;
        qp->sq_span[idx] = 1;
        qp->sq_atomic_result[idx] = result_addr;
        rdma_dma_write_barrier();
        volatile uint32_t *db = (volatile uint32_t *)
            (qp->db_rec);
        db[1] = __builtin_bswap32((uint32_t)(head + 1) & 0xffffu);
        rdma_dma_write_barrier();
        uint64_t doorbell = 0;
        memcpy(&doorbell, flat, sizeof(doorbell));
        rdma_ring_send_doorbell(qp, doorbell);
        qp->sq_head = head + 1;
        rdma_qp_publish_shadow_maybe(qp, RDMA_QP_SHADOW_SQ_HEAD);
        rdma_stat_add(&qp->dev->stats.direct_send_wrs, 1);
        rdma_stat_add(&qp->dev->stats.direct_send_batches, 1);
        rdma_stat_add(&qp->dev->stats.direct_doorbells, 1);
        return 0;
    }

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
    if (kr == kIOReturnSuccess && qp->direct_sq && qp->sq_wrid &&
        qp->sq_opcode && qp->sq_span && qp->sq_atomic_result) {
        uint32_t idx = (uint32_t)qp->sq_head & (qp->sq_size - 1);
        qp->sq_wrid[idx] = wr_id;
        qp->sq_opcode[idx] = (uint8_t)opcode;
        qp->sq_span[idx] = 1;
        qp->sq_atomic_result[idx] = result_addr;
        qp->sq_head++;
        rdma_qp_publish_shadow_force(qp);
    }
    return kr == kIOReturnSuccess ? 0 : (kr == kIOReturnBusy ? -EAGAIN : -EIO);
}

static int rdma_post_send_sge_unlocked(rdma_qp *qp, const struct rdma_send_wr *wr)
{
    int zero_write_imm = wr && wr->opcode == RDMA_WR_RDMA_WRITE_IMM &&
                         wr->num_sge == 0;
    if (qp && wr && qp->direct_sq && (wr->num_sge >= 1 || zero_write_imm))
        return rdma_post_send_direct_sge(qp, wr);
    if (!qp || !wr || (!zero_write_imm && (!wr->sg_list || !wr->num_sge)) ||
        wr->num_sge > RDMA_MAX_SGE || qp->direct_sq ||
        (wr->send_flags & ~(RDMA_SEND_SIGNALED | RDMA_SEND_FENCE |
                            RDMA_SEND_SOLICITED))) return -EINVAL;
    /* The public shim keeps SEND_INV after the standard verbs opcodes.  The
     * DriverKit ABI deliberately assigns it its own compact value, so never
     * leak the public enum value through the user-client boundary. */
    uint32_t ucOpcode = wr->opcode == RDMA_WR_SEND_INV ?
                        MLX_UC_WR_SEND_INV : wr->opcode;
    struct mlx_post_send_sge_req req = {
        .qpn = qp->qpn, .opcode = ucOpcode, .wrId = wr->wr_id,
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

static int rdma_post_local_inv_unlocked(rdma_qp *qp, uint64_t wr_id, uint32_t rkey);
static int rdma_post_send_inline_unlocked(rdma_qp *qp, uint64_t wr_id, uint32_t opcode, const void *data, uint32_t len, uint64_t remote_addr, uint32_t rkey, uint32_t imm_data, uint32_t send_flags);
static int rdma_post_send_atomic_unlocked(rdma_qp *qp, uint64_t wr_id, uint32_t opcode, uint64_t remote_addr, uint32_t rkey, uint64_t compare, uint64_t swap_add, uint64_t result_addr, uint32_t result_lkey, uint32_t send_flags);
static int rdma_post_send_sge_unlocked(rdma_qp *qp, const struct rdma_send_wr *wr);
static int rdma_post_send_batch_unlocked(rdma_qp *qp, const struct rdma_send_wr *wr, uint32_t count);
static int rdma_post_recv_sge_unlocked(rdma_qp *qp, const struct rdma_recv_wr *wr);
static int rdma_post_recv_batch_unlocked(rdma_qp *qp, const struct rdma_recv_wr *wr, uint32_t count);

/* Public posting entry points. Each forwards to the body above under the
 * queue pair's own lock, so two threads posting on one QP cannot compute the
 * same ring index and write the same work-queue slot. The lock is skipped
 * entirely for a caller that opted out. Entry points that delegate to another
 * entry point, rdma_post_send and rdma_post_recv, are deliberately not wrapped
 * so a post takes the lock exactly once. */
int rdma_post_local_inv(rdma_qp *qp, uint64_t wr_id, uint32_t rkey)
{
    if (!qp) return -EINVAL;
    if (!qp->post_serialize) return rdma_post_local_inv_unlocked(qp, wr_id, rkey);
    os_unfair_lock_lock(&qp->post_lock);
    int rc = rdma_post_local_inv_unlocked(qp, wr_id, rkey);
    os_unfair_lock_unlock(&qp->post_lock);
    return rc;
}

int rdma_post_send_inline(rdma_qp *qp, uint64_t wr_id, uint32_t opcode,
                          const void *data, uint32_t len,
                          uint64_t remote_addr, uint32_t rkey,
                          uint32_t imm_data, uint32_t send_flags)
{
    if (!qp) return -EINVAL;
    if (!qp->post_serialize) return rdma_post_send_inline_unlocked(qp, wr_id, opcode, data, len, remote_addr, rkey, imm_data, send_flags);
    os_unfair_lock_lock(&qp->post_lock);
    int rc = rdma_post_send_inline_unlocked(qp, wr_id, opcode, data, len, remote_addr, rkey, imm_data, send_flags);
    os_unfair_lock_unlock(&qp->post_lock);
    return rc;
}

int rdma_post_send_atomic(rdma_qp *qp, uint64_t wr_id, uint32_t opcode,
                          uint64_t remote_addr, uint32_t rkey,
                          uint64_t compare, uint64_t swap_add,
                          uint64_t result_addr, uint32_t result_lkey,
                          uint32_t send_flags)
{
    if (!qp) return -EINVAL;
    if (!qp->post_serialize) return rdma_post_send_atomic_unlocked(qp, wr_id, opcode, remote_addr, rkey, compare, swap_add, result_addr, result_lkey, send_flags);
    os_unfair_lock_lock(&qp->post_lock);
    int rc = rdma_post_send_atomic_unlocked(qp, wr_id, opcode, remote_addr, rkey, compare, swap_add, result_addr, result_lkey, send_flags);
    os_unfair_lock_unlock(&qp->post_lock);
    return rc;
}

int rdma_post_send_sge(rdma_qp *qp, const struct rdma_send_wr *wr)
{
    if (!qp) return -EINVAL;
    if (!qp->post_serialize) return rdma_post_send_sge_unlocked(qp, wr);
    os_unfair_lock_lock(&qp->post_lock);
    int rc = rdma_post_send_sge_unlocked(qp, wr);
    os_unfair_lock_unlock(&qp->post_lock);
    return rc;
}

int rdma_post_send_batch(rdma_qp *qp, const struct rdma_send_wr *wr,
                         uint32_t count)
{
    if (!qp) return -EINVAL;
    if (!qp->post_serialize) return rdma_post_send_batch_unlocked(qp, wr, count);
    os_unfair_lock_lock(&qp->post_lock);
    int rc = rdma_post_send_batch_unlocked(qp, wr, count);
    os_unfair_lock_unlock(&qp->post_lock);
    return rc;
}

int rdma_post_recv_sge(rdma_qp *qp, const struct rdma_recv_wr *wr)
{
    if (!qp) return -EINVAL;
    if (!qp->post_serialize) return rdma_post_recv_sge_unlocked(qp, wr);
    os_unfair_lock_lock(&qp->post_lock);
    int rc = rdma_post_recv_sge_unlocked(qp, wr);
    os_unfair_lock_unlock(&qp->post_lock);
    return rc;
}

int rdma_post_recv_batch(rdma_qp *qp, const struct rdma_recv_wr *wr,
                         uint32_t count)
{
    if (!qp) return -EINVAL;
    if (!qp->post_serialize) return rdma_post_recv_batch_unlocked(qp, wr, count);
    os_unfair_lock_lock(&qp->post_lock);
    int rc = rdma_post_recv_batch_unlocked(qp, wr, count);
    os_unfair_lock_unlock(&qp->post_lock);
    return rc;
}

int rdma_post_send(rdma_qp *qp, const struct rdma_send_wr *wr)
{
    return wr && (wr->num_sge > 1 || wr->opcode == RDMA_WR_SEND_IMM ||
                  wr->opcode == RDMA_WR_SEND_INV ||
                  wr->opcode == RDMA_WR_RDMA_WRITE_IMM) ?
        rdma_post_send_sge(qp, wr) : rdma_post_send_batch(qp, wr, 1);
}

static int rdma_post_send_batch_unlocked(rdma_qp *qp, const struct rdma_send_wr *wr,
                         uint32_t count)
{
    if (!qp || !wr || !count || count > RDMA_MAX_POST_BATCH)
        return -EINVAL;
    if (qp->trusted_fast_path) {
        for (uint32_t base = 0; base < count; base += RDMA_POST_CHUNK) {
            uint32_t chunk = count - base;
            if (chunk > RDMA_POST_CHUNK) chunk = RDMA_POST_CHUNK;
            int rc = rdma_post_send_direct_mixed(qp, wr + base, chunk);
            if (rc) return rc;
        }
        return 0;
    }
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
        rdma_stat_add(&qp->dev->stats.fallback_send_batches, 1);
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
            if (qp->is_ud) {
                /* A datagram names a peer, not a memory address; the two sets
                 * of fields overlap, so exactly one is written. */
                req.wr[i].ahHandle = wr[index].ah_handle;
                req.wr[i].remoteQpn = wr[index].remote_qpn;
                req.wr[i].remoteQkey = wr[index].remote_qkey;
            } else {
                req.wr[i].remoteAddr = wr[index].remote_addr;
                req.wr[i].rkey = wr[index].rkey;
            }
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

static int rdma_post_recv_sge_unlocked(rdma_qp *qp, const struct rdma_recv_wr *wr)
{
    /* A datagram receive begins with 40 bytes of global routing header, so a
     * buffer with no room for it truncates the payload. The check lives here
     * rather than only in the driver because the direct path below never
     * reaches the driver at all. */
    if (qp && wr && qp->is_ud) {
        uint32_t total = 0;
        for (uint32_t i = 0; i < wr->num_sge; i++) total += wr->sg_list[i].length;
        if (total <= RDMA_GRH_BYTES) return -EINVAL;
    }
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
        wr->num_sge < 2 || wr->num_sge > RDMA_MAX_RECV_SGE ||
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
    if (!qp->trusted_fast_path) {
        kern_return_t kr = IOConnectCallStructMethod(qp->dev->conn,
            kMlxUCMethodSyncRecvSge, &sync, sizeof(sync), NULL, 0);
        if (kr != kIOReturnSuccess) {
            SHIM_LOG("direct multi-SGE receive sync failed: 0x%x", kr);
            return kr == kIOReturnBusy ? -EAGAIN : -EIO;
        }
    }
    if (qp->rq_wrid) qp->rq_wrid[index] = wr->wr_id;
    if (qp->rq_sge_addr) qp->rq_sge_addr[index] = wr->sg_list[0].addr;
    rdma_dma_write_barrier();
    volatile uint32_t *db = (volatile uint32_t *)
        (qp->db_rec);
    db[0] = __builtin_bswap32((uint32_t)(qp->rq_head + 1) & 0xffffu);
    qp->rq_head++;
    rdma_qp_publish_shadow_maybe(qp, RDMA_QP_SHADOW_RQ_HEAD);
    rdma_stat_add(&qp->dev->stats.direct_recv_wrs, 1);
    return 0;
}

static int rdma_post_recv_batch_unlocked(rdma_qp *qp, const struct rdma_recv_wr *wr,
                         uint32_t count)
{
    if (!qp || !wr || !count || count > RDMA_MAX_POST_BATCH)
        return -EINVAL;
    /* Same contract as the single-request entry: a datagram receive begins
     * with 40 bytes of routing header, and a buffer without room for it
     * truncates the payload. Checked here as well because the trusted fast
     * path posts batches and never reaches the other entry. */
    if (qp->is_ud) {
        for (uint32_t b = 0; b < count; b++) {
            uint32_t total = 0;
            for (uint32_t i = 0; i < wr[b].num_sge; i++)
                total += wr[b].sg_list[i].length;
            if (total <= RDMA_GRH_BYTES) return -EINVAL;
        }
    }
    if (qp->direct_rq && count == 1 && wr->num_sge >= 2)
        return rdma_post_recv_direct_sge(qp, wr);
    if (qp->direct_rq) {
        for (uint32_t base = 0; base < count; base += RDMA_POST_CHUNK) {
            uint32_t chunk = count - base;
            if (chunk > RDMA_POST_CHUNK) chunk = RDMA_POST_CHUNK;
            if (qp->rq_head - qp->rq_tail + chunk > qp->rq_size)
                return -EAGAIN;
            struct mlx_sync_recv_fast_path_req req;
            if (!qp->trusted_fast_path) {
                memset(&req, 0, offsetof(struct mlx_sync_recv_fast_path_req, wr) +
                                chunk * sizeof(req.wr[0]));
                req.count = chunk;
            }
            uint64_t head = qp->rq_head;
            for (uint32_t i = 0; i < chunk; i++) {
                if (!wr[base + i].num_sge ||
                    wr[base + i].num_sge >
                        (qp->trusted_fast_path ? RDMA_MAX_RECV_SGE : 1) ||
                    !wr[base + i].sg_list)
                    return -EINVAL;
                uint32_t idx = (uint32_t)(head + i) & (qp->rq_size - 1);
                volatile uint8_t *wqe = (volatile uint8_t *)qp->rq_buf +
                                         (uint64_t)idx * qp->sq_stride;
                if (wr[base + i].num_sge == 1) {
                    if (!mlxEncodeRecvWqe64((void *)(uintptr_t)wqe,
                                            wr[base + i].sg_list[0].addr,
                                            wr[base + i].sg_list[0].length,
                                            wr[base + i].sg_list[0].lkey))
                        return -EINVAL;
                } else {
                    struct MlxRcSge sges[RDMA_MAX_RECV_SGE];
                    for (uint32_t s = 0; s < wr[base + i].num_sge; s++) {
                        sges[s].addr = wr[base + i].sg_list[s].addr;
                        sges[s].length = wr[base + i].sg_list[s].length;
                        sges[s].lkey = wr[base + i].sg_list[s].lkey;
                    }
                    if (!mlxEncodeRecvWqeSge((void *)(uintptr_t)wqe, 64, sges,
                                             wr[base + i].num_sge))
                        return -EINVAL;
                }
                if (!qp->trusted_fast_path) {
                    req.wr[i].qpn = qp->qpn;
                    req.wr[i].wrId = wr[base + i].wr_id;
                    req.wr[i].sge.addr = wr[base + i].sg_list[0].addr;
                    req.wr[i].sge.length = wr[base + i].sg_list[0].length;
                    req.wr[i].sge.lkey = wr[base + i].sg_list[0].lkey;
                }
                if (qp->rq_wrid) qp->rq_wrid[idx] = wr[base + i].wr_id;
                if (qp->rq_sge_addr) qp->rq_sge_addr[idx] = wr[base + i].sg_list[0].addr;
            }
            if (!qp->trusted_fast_path) {
                kern_return_t kr = IOConnectCallStructMethod(
                    qp->dev->conn, kMlxUCMethodSyncRecvFastPath,
                    &req, sizeof(req), NULL, 0);
                if (kr != kIOReturnSuccess) return -EIO;
            }
            rdma_dma_write_barrier();
            volatile uint32_t *db = (volatile uint32_t *)
                (qp->db_rec);
            db[0] = __builtin_bswap32((uint32_t)(head + chunk) & 0xffffu);
            qp->rq_head = head + chunk;
            rdma_qp_publish_shadow_maybe(qp, RDMA_QP_SHADOW_RQ_HEAD);
            rdma_stat_add(&qp->dev->stats.direct_recv_batches, 1);
            rdma_stat_add(&qp->dev->stats.direct_recv_wrs, chunk);
        }
        return 0;
    }
    for (uint32_t base = 0; base < count; base += RDMA_POST_CHUNK) {
        rdma_stat_add(&qp->dev->stats.fallback_recv_batches, 1);
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
