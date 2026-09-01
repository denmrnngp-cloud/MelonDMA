/*
 * MlxQP.hpp — Queue Pair management (generic Mellanox family)
 *
 * Ported from: kernel_src/mlnx-ofed-kernel-5.9/drivers/infiniband/hw/mlx5/qp.c
 * Trimmed: RC/UD types only, supporting RDMA WRITE/READ/SEND + UD datagram.
 *
 * DriverKit port: SQ/RQ buffers are DEXT-owned IOBufferMemoryDescriptor pinned
 * with IODMACommand; the IOVA goes into the QPC wq_umem PAS. State machine
 * uses correct opcodes RST2INIT (0x502)/INIT2RTR (0x503)/RTR2RTS (0x504).
 * QPC encoding via MlxP0Encoding.hpp (host-tested, portable).
 */
#ifndef MLX_QP_HPP
#define MLX_QP_HPP

#include <stdint.h>
#include <DriverKit/IOReturn.h>
#include <DriverKit/IOLib.h>
#include "MlxRegs.hpp"
#include "MlxWQE.hpp"
#include "MlxDMA.hpp"
#include "MlxUCIO.h"

class MlxRoCE;
class MlxPCIDriver;
struct MlxClientDoorbellBundle;

struct MlxQPContext {
    uint32_t    qpNum;
    uint32_t    state;
    uint32_t    st;
    uint32_t    pd;
    uint32_t    sendCq;
    uint32_t    recvCq;
    uint32_t    uarPage;
    uint64_t    sqBufAddr;
    uint64_t    rqBufAddr;
    uint32_t    sqSize;
    uint32_t    rqSize;
    uint32_t    dbRecordOffset;
    uint32_t    bfOffset;
    uint64_t    sqPhys;
    uint64_t    rqPhys;
    volatile uint8_t *sqCpu;
    volatile uint8_t *rqCpu;
    volatile uint32_t *dbRecord;
    MlxDMAReq   sqDma;
    MlxDMAReq   rqDma;
    bool        sqPinned;
    bool        rqPinned;
    uint64_t   *sqWrid;
    uint64_t   *rqWrid;
    uint8_t    *sqOpcode;
    uint8_t    *sqSpan;    /* WQEBBs occupied by the WR starting at this slot;
                             * 1 for every WR type except UMR (PostUmrKlm). */
    uint64_t    sqHead;
    uint64_t    sqTail;
    uint64_t    rqHead;
    uint64_t    rqTail;
    /* RC path */
    uint8_t     ahDmac[6];
    uint8_t     ahDgid[16];
    uint32_t    ahSgidIndex;
    uint8_t     ahHopLimit;
    uint8_t     ahTrafficClass;
    uint16_t    ahUdpSport;
    uint16_t    pkeyIndex;
    uint8_t     portNum;
    uint32_t    destQpn;
    uint32_t    rqPsn;
    uint32_t    sqPsn;
    uint64_t    sqPkts;
    uint64_t    rqPkts;
    /* P2.1 per-QP datapath counters (aggregated into mlx_stats_resp).
     * Posted counters are tagged at WQE-accept time; completed counters at
     * CQE-consume time. completedUmr folds BIND_MW and UMR/KLM (the CQE
     * carries no bind/umr discriminator). */
    uint64_t    postedSend;
    uint64_t    postedRead;
    uint64_t    postedWrite;
    uint64_t    postedUmr;
    uint64_t    postedBindMw;
    uint64_t    postedLocalInv;
    uint64_t    completedSend;
    uint64_t    completedRead;
    uint64_t    completedWrite;
    uint64_t    completedRecv;
    uint64_t    completedUmr;
    uint64_t    completedLocalInv;
    uint64_t    cqeError;
    uint64_t    cqeRetryExc;
    uint64_t    cqeRnrRetry;
    MlxClientDoorbellBundle *clientBundle;
};

class MlxQP {
public:
    MlxQP();
    ~MlxQP();

    kern_return_t   Init(MlxRoCE *roce);
    void            Free();

    kern_return_t   CreateQP(const struct mlx_create_qp_req *req,
                              struct mlx_create_qp_resp *resp,
                              MlxClientDoorbellBundle *bundle = NULL);
    kern_return_t   ModifyQP(const struct mlx_modify_qp_req *req);
    kern_return_t   DestroyQP(uint32_t qpn);
    kern_return_t   ResetQP(uint32_t qpn);
    kern_return_t   QueryQP(uint32_t qpn, void *out);
    kern_return_t   PostSend(const struct mlx_post_send_req *req);
    kern_return_t   PostRecv(const struct mlx_post_recv_req *req);
    kern_return_t   PostSendSge(const struct mlx_post_send_sge_req *req);
    kern_return_t   PostRecvSge(const struct mlx_post_recv_sge_req *req);
    kern_return_t   PostSendBatch(const struct mlx_post_send_req *req,
                                  uint32_t count);
    kern_return_t   PostRecvBatch(const struct mlx_post_recv_req *req,
                                  uint32_t count);
    kern_return_t   SyncFastPath(const struct mlx_post_send_req *req,
                                 uint32_t count);
    kern_return_t   SyncRecvFastPath(const struct mlx_post_recv_req *req,
                                     uint32_t count);
    kern_return_t   SyncSendSge(const struct mlx_sync_send_sge_req *req);
    kern_return_t   SyncRecvSge(const struct mlx_sync_recv_sge_req *req);
    kern_return_t   PostLocalInv(const struct mlx_post_local_inv_req *req);
    kern_return_t   PostSendInline(const struct mlx_post_send_inline_req *req);
    kern_return_t   PostSendAtomic(const struct mlx_post_send_atomic_req *req);
    kern_return_t   PostUmrKlm(uint32_t qpn, uint32_t mrHandle,
                               const uint32_t *childHandles,
                               uint32_t childCount, uint64_t wrId);
    kern_return_t   PostBindMW(uint32_t qpn, uint32_t mwKey, uint32_t origRkey,
                               uint32_t bindKey, uint32_t mrLkey, uint32_t accessFlags,
                               uint64_t addr, uint64_t length, uint64_t wrId);
    bool            CompleteCQE(uint32_t cqHandle, const struct MlxCqe64 *cqe,
                                struct mlx_work_completion *wc);

    void            HandleQPEvent(uint32_t qpn, uint32_t event);
    MlxQPContext *  Lookup(uint32_t qpn);
    IOMemoryDescriptor *GetSqMemDesc(uint32_t qpn);
    IOMemoryDescriptor *GetRqMemDesc(uint32_t qpn);

private:
    struct State;
    State *s;
    MlxQPContext *CtxForQpn(uint32_t qpn);
    MlxQPContext *LockQp(uint32_t qpn);
    MlxQPContext *LockQpByCq(uint32_t cqHandle, bool send);
    void          UnlockQp(MlxQPContext *ctx);
};

#endif /* MLX_QP_HPP */
