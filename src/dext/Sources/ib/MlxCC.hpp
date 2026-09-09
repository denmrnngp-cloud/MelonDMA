/*
 * MlxCC.hpp — DCQCN congestion control (generic Mellanox family)
 *
 * Ported from: drivers/infiniband/hw/mlx5/cong.c. The DCQCN loop runs in
 * firmware; the driver only wraps QUERY/MODIFY_CONG_PARAMS.
 */
#ifndef MLX_CC_HPP
#define MLX_CC_HPP

#include <stdint.h>
#include <DriverKit/IOReturn.h>
#include <DriverKit/IOLib.h>
#include "MlxRegs.hpp"
#include "MlxUCIO.h"

class MlxRoCE;
class MlxPCIDriver;

class MlxCC {
public:
    MlxCC();
    ~MlxCC();

    kern_return_t   Init(MlxRoCE *roce, MlxPCIDriver *core);
    void            Free();

    kern_return_t   QueryParams(struct mlx_cc_params *out);
    /* QUERY_CONG_STATUS (0x822) and QUERY_CONG_STATISTICS (0x826) together:
     * whether firmware's loop is enabled for a priority, and what it has done
     * since the counters were last cleared. Both are read-only. */
    kern_return_t   QueryStats(const struct mlx_cc_stats_req *req,
                               struct mlx_cc_stats_resp *out);
    kern_return_t   ModifyParams(const struct mlx_cc_params *in);
    bool            IsEnabled() const;

private:
    struct State;
    State *s;
    kern_return_t   CmdQuery(uint32_t regId, void *out);
    kern_return_t   CmdModify(uint32_t regId, const void *in);
};

#endif /* MLX_CC_HPP */