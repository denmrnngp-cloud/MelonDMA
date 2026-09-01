/*
 * MlxAH.hpp — Address Handle (generic Mellanox family)
 *
 * Ported from: drivers/infiniband/hw/mlx5/ah.c. Pure encoding logic — the AV
 * encoder is host-testable and portable. Storage adapted to DriverKit.
 */
#ifndef MLX_AH_HPP
#define MLX_AH_HPP

#include <stdint.h>
#include <DriverKit/IOReturn.h>
#include <DriverKit/IOLib.h>
#include "MlxWQE.hpp"
#include "MlxUCIO.h"

class MlxRoCE;

struct MlxAHContext {
    uint32_t    ahHandle;
    uint32_t    portNum;
    MlxAV       av;
    bool        isRoCE;
};

class MlxAH {
public:
    MlxAH();
    ~MlxAH();

    kern_return_t   Init(MlxRoCE *roce);
    void            Free();

    kern_return_t   CreateAH(const struct mlx_create_ah_req *req,
                              struct mlx_create_ah_resp *resp);
    kern_return_t   DestroyAH(uint32_t ahHandle);
    MlxAHContext *  Lookup(uint32_t ahHandle);

    static void     EncodeAV(const struct mlx_create_ah_req *req, MlxAV *av);

private:
    struct State;
    State *s;
};

#endif /* MLX_AH_HPP */