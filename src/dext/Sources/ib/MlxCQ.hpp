/*
 * MlxCQ.hpp — Completion Queue (generic Mellanox family)
 *
 * Ported from: kernel_src/mlnx-ofed-kernel-5.9/drivers/infiniband/hw/mlx5/cq.c
 *
 * DriverKit port: CQE buffer is DEXT-owned and pinned with IODMACommand.
 * Firmware writes CQEs via DMA; polling and consumer DB updates remain
 * kernel-mediated so no hardware queue is mapped into an untrusted client.
 */
#ifndef MLX_CQ_HPP
#define MLX_CQ_HPP

#include <stdint.h>
#include <DriverKit/IOReturn.h>
#include <DriverKit/OSArray.h>
#include <DriverKit/IOMemoryDescriptor.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#include <DriverKit/IOLib.h>
#include "MlxWQE.hpp"

class MlxRoCE;
struct MlxClientDoorbellBundle;

struct MlxCQContext {
    uint32_t    cqNumber;
    uint32_t    logSize;
    uint32_t    cqeSize;        /* 64 bytes */
    uint64_t    cqeBufAddr;
    volatile struct MlxCqe64 *cqeCpu;
    uint64_t    cqeDMA;
    IOBufferMemoryDescriptor *cqeBufDesc;
    IODMACommand *cqeDmaMap;
    uint64_t    pageDMA[32];
    uint32_t    numPages;
    uint32_t    dbRecordOffset;
    uint32_t    eqNumber;
    uint32_t    compVector;
    uint32_t    armSn;
    uint32_t    modPeriod;      /* applied MODIFY_CQ moderation, microseconds */
    uint32_t    modMaxCount;    /* applied MODIFY_CQ moderation, CQEs */
    void       (*completionHandler)(uint32_t cqn, void *context);
    void       *completionContext;
    uint64_t    completions;
    uint64_t    consumerIndex;
    uint64_t    lost;            /* P2.1: CQEs dropped (unattributable QPN) */
    bool        armed;           /* hardware arm pending (ArmCQ) */
    MlxClientDoorbellBundle *clientBundle;
};

class MlxCQ {
public:
    MlxCQ();
    ~MlxCQ();

    kern_return_t   Init(MlxRoCE *roce);
    void            Free();

    kern_return_t   CreateCQ(uint32_t entries,
                              struct mlx_create_cq_resp *resp,
                              MlxClientDoorbellBundle *bundle = NULL);
    kern_return_t   DestroyCQ(uint32_t cqHandle);

    /* Allocated CQ slots across every client. The completion-vector probe
     * needs this: rebinding the completion EQ changes its EQ number, and a
     * live CQ still carries the old one in its c_eqn. */
    uint32_t        LiveCount();
    MlxCQContext *  Lookup(uint32_t cqHandle);
    IOMemoryDescriptor *GetCqMemDesc(uint32_t cqHandle);
    void            HandleCompletion(uint32_t cqn);
    uint64_t        GetCompletions(uint32_t cqHandle);
    kern_return_t   UpdateCqConsumer(uint32_t cqHandle, uint32_t consumerIndex);
    /* Hardware completion moderation (MODIFY_CQ). period is in microseconds,
     * maxCount in CQEs; either zero disables that half. */
    kern_return_t   ModifyModeration(uint32_t cqHandle, uint32_t period,
                                     uint32_t maxCount);
    kern_return_t   ArmCQ(uint32_t cqHandle, uint32_t solicitedOnly);
    kern_return_t   PollCQ(const struct mlx_poll_cq_req *req,
                           struct mlx_poll_cq_resp *resp);

private:
    struct State;
    State *s;
    MlxCQContext *  LockCq(uint32_t cqHandle);   /* returns with per-CQ lock held */
    void            UnlockCq(MlxCQContext *cq);
    kern_return_t   CmdCreateCQ(MlxCQContext *cq, uint32_t eqNumber);
    kern_return_t   CmdDestroyCQ(uint32_t cqNumber);
};

#endif /* MLX_CQ_HPP */
