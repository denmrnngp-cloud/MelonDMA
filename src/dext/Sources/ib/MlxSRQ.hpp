/*
 * MlxSRQ.hpp — Shared Receive Queue.
 *
 * On firmware with ISSI set a basic SRQ is an RMP, not a CREATE_SRQ object:
 * mlx5 dispatches IB_SRQT_BASIC to create_rmp_cmd whenever issi is non-zero.
 * Its receive queue is a linked list of WQEs, so it shares none of the QP's
 * cyclic receive-ring machinery.
 *
 * The rmpc layout was proven on this hardware before this file was written
 * (kMlxUCMethodProbeRmpLayout, rmpn 130, nine fields read back verbatim).
 * Two conventions differ from the QPC and both matter: log_wq_stride here is
 * the plain log2 of the stride in bytes, not the QPC's log2 minus 4; and
 * uar_page must be left zero, exactly as mlx5's own set_wq leaves it.
 */
#ifndef MLX_SRQ_HPP
#define MLX_SRQ_HPP

#include <stdint.h>
#include <DriverKit/IOReturn.h>
#include <DriverKit/IOMemoryDescriptor.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#include <DriverKit/IOLib.h>

class MlxRoCE;
class MlxPCIDriver;
struct mlx_create_srq_req;
struct mlx_create_srq_resp;
struct mlx_post_srq_recv_req;
struct mlx_query_srq_resp;

#define MLX_SRQ_TABLE_MAX   32
#define MLX_SRQ_WQE_BYTES   64      /* next segment plus three data segments */
#define MLX_SRQ_MAX_PAGES   16

struct MlxSRQContext {
    uint32_t    srqn;
    uint32_t    pd;
    uint32_t    logSize;        /* log2 of the WQE count */
    uint32_t    maxSge;
    uint64_t    bufAddr;        /* DEXT-side mapping of the WQ */
    uint64_t    pageDMA[MLX_SRQ_MAX_PAGES];
    uint32_t    numPages;
    IOBufferMemoryDescriptor *bufDesc;
    IODMACommand *dmaMap;
    uint32_t    dbRecordOffset;
    volatile uint32_t *dbRecord;

    /* One circular chain, held in the WQEs themselves, exactly as mlx5 keeps
     * it. Posting takes from the head; a completion appends the consumed WQE
     * at the tail. There is no sentinel: firmware walks next_wqe_index and a
     * terminator would send it to an index outside the queue, which is a
     * local protection error the moment the chain is walked to its end.
     * The list is full when head meets tail, so one WQE is always spare and
     * usable depth is one less than the allocation. */
    uint32_t    head;           /* next WQE to hand to hardware */
    uint32_t    tail;           /* last WQE in the chain */
    uint32_t    freeCount;      /* reported; correctness rides on head != tail */
    uint32_t    counter;        /* posted WQEs, mirrored into the DB record */
    uint32_t    limit;          /* MODIFY_RMP watermark, 0 when disarmed */
    /* A QP bound to an SRQ has no receive ring, so the completion path cannot
     * recover a work-request id from the QP. The SRQ keeps it per WQE index,
     * which is what the CQE carries back in wqe_counter. */
    uint64_t   *wrId;
    bool        used;
};

class MlxSRQ {
public:
    MlxSRQ();
    ~MlxSRQ();

    kern_return_t   Init(MlxRoCE *roce);
    void            Free();

    kern_return_t   CreateSRQ(const struct mlx_create_srq_req *req,
                              struct mlx_create_srq_resp *resp);
    kern_return_t   DestroySRQ(uint32_t srqn);
    kern_return_t   PostRecv(const struct mlx_post_srq_recv_req *req);
    kern_return_t   QuerySRQ(uint32_t srqn, struct mlx_query_srq_resp *resp);
    kern_return_t   ModifyLimit(uint32_t srqn, uint32_t limit);

    /* Called from the completion path with the wqe_counter carried by a CQE
     * whose receive came from this SRQ. Without it the free list drains. */
    void            ReturnWqe(uint32_t srqn, uint32_t wqeIndex);
    /* Work-request id stored when that WQE was posted; 0 if unknown. */
    uint64_t        WrIdFor(uint32_t srqn, uint32_t wqeIndex);
    /* Called for every receive completion, matched or not. A queue that never
     * sees its own srqn is a different fault from one whose free list drains,
     * and only this distinguishes them. */
    void            NoteReceiveCqe(uint32_t srqn, uint32_t wqeIndex, uint8_t op);

    MlxSRQContext  *Lookup(uint32_t srqn);
    uint32_t        LiveCount() const;

private:
    kern_return_t   CmdCreateRmp(MlxSRQContext *srq);
    kern_return_t   CmdDestroyRmp(uint32_t srqn);
    void            BuildFreeList(MlxSRQContext *srq);
    void            ReleaseContext(MlxSRQContext *srq);

    struct State;
    State *s;
};

#endif /* MLX_SRQ_HPP */
