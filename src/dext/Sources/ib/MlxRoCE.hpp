/*
 * MlxRoCE.hpp — verbs protocol layer entry point (generic Mellanox family)
 *
 * Ported from: kernel_src/mlnx-ofed-kernel-5.9/drivers/infiniband/hw/mlx5/main.c.
 * DriverKit port: MlxRoCE is a plain C++ object owned by MlxPCIDriver (not an
 * IOService nub — the kext published it for IORDMAFamily registration, which
 * is unavailable on macOS, notes/22-*). The DEXT exposes verbs directly
 * through MlxUserClient::ExternalMethod → MlxRoCE methods.
 */
#ifndef MLX_ROCE_HPP
#define MLX_ROCE_HPP

#include <stdint.h>
#include <DriverKit/IOReturn.h>
#include <DriverKit/IOLib.h>
#include "MlxHCA.hpp"
#include "MlxEQ.hpp"
#include "MlxUCIO.h"

class MlxPCIDriver;
class MlxQP;
class MlxCQ;
class MlxMR;
class MlxAH;
class MlxGID;
class MlxCC;
class IOMemoryDescriptor;
struct MlxClientDoorbellBundle;

class MlxRoCE : public MlxEventNotifier {
public:
    MlxRoCE();
    ~MlxRoCE();

    kern_return_t   Init(MlxPCIDriver *core, MlxHCA *hca);
    /* Destroy all firmware-facing verbs resources while preserving the EQ
     * page-request notifier. Stable-driver close/open uses this to observe
     * firmware's negative PAGE_REQUESTs around TEARDOWN_HCA. */
    void            QuiesceVerbsResources();
    void            Free();

    /* ---- verbs operations (called by MlxUserClient) ---- */
    kern_return_t   CreateQP(const struct mlx_create_qp_req *req,
                             struct mlx_create_qp_resp *resp,
                             MlxClientDoorbellBundle *bundle = NULL);
    kern_return_t   ModifyQP(const struct mlx_modify_qp_req *req);
    kern_return_t   DestroyQP(uint32_t qpn);
    kern_return_t   CreateCQ(uint32_t entries,
                             struct mlx_create_cq_resp *resp,
                             MlxClientDoorbellBundle *bundle = NULL);
    kern_return_t   DestroyCQ(uint32_t cqHandle);
    kern_return_t   RegMR(const struct mlx_reg_mr_req *req,
                          IOMemoryDescriptor *clientMemory,
                          struct mlx_reg_mr_resp *resp);
    kern_return_t   RegMRIndirect(const struct mlx_reg_mr_indirect_req *req,
                                  struct mlx_reg_mr_resp *resp);
    kern_return_t   DeregMR(uint32_t mrHandle);
    kern_return_t   AllocMW(uint32_t pd, uint32_t type, uint32_t *handle, uint32_t *rkey);
    kern_return_t   DeallocMW(uint32_t handle);
    kern_return_t   BindMW(const struct mlx_bind_mw_req *req, struct mlx_bind_mw_resp *resp);
    kern_return_t   CreateAH(const struct mlx_create_ah_req *req,
                             struct mlx_create_ah_resp *resp);
    kern_return_t   DestroyAH(uint32_t ahHandle);
    kern_return_t   QueryDevice(struct mlx_query_device_resp *resp);
    kern_return_t   QueryPort(struct mlx_query_port_resp *resp);
    kern_return_t   QueryGidTable(const struct mlx_query_gid_table_req *req,
                                 struct mlx_query_gid_table_resp *resp);
    kern_return_t   PostSendInline(const struct mlx_post_send_inline_req *req);
    kern_return_t   PostSendAtomic(const struct mlx_post_send_atomic_req *req);
    kern_return_t   ArmCQ(const struct mlx_arm_cq_req *req);

    kern_return_t   GetAsyncEvent(struct mlx_async_event *event);
    void            QueueAsyncEvent(uint32_t eventType, uint32_t elementType,
                                     uint32_t elementHandle);
    /* Process deferred runtime PAGE_REQUEST (after EQ Poll).
     * Returns the handler error (first one), if any. */
    kern_return_t   DrainPendingPageRequests();
    /* Page health: true if there was a runtime pages error (block datapath). */
    bool            PageHealthFailed() const;

    /* MlxEventNotifier: dispatch EQ events to the right verbs object. */
    virtual void    HandleEvent(uint32_t type, void *eqe);

    MlxPCIDriver *  GetCore();
    MlxHCA *        GetHCA();
    MlxGID *        GetGID();
    MlxCC *         GetCC();
    MlxCQ *         GetCQ();
    MlxQP *         GetQP();
    MlxMR *         GetMR();

private:
    struct State;
    State *s;
    bool            StageCaps();
    bool            StageGID();
};

#endif /* MLX_ROCE_HPP */
