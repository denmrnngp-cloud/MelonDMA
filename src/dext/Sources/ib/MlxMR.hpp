/*
 * MlxMR.hpp — Memory Registration (generic Mellanox family)
 *
 * Ported from: drivers/infiniband/hw/mlx5/mr.c (trimmed: reg_user_mr/dereg).
 *
 * DriverKit port: client memory is pinned via MlxDMA::Pin
 * (CreateMemoryDescriptorFromClient → IODMACommand::PrepareForDMA). The PBL is
 * built by splitting the DMA segments into 4 KiB HCA PAS entries and encoded
 * with MlxP0Encoding.hpp::mlxEncodeCreateMkey (host-tested). MKC start_addr
 * remains the client VA; PAS supplies its IOMMU translation. The lkey/rkey are
 * composed as (mkey_index << 8) | key_variant.
 */
#ifndef MLX_MR_HPP
#define MLX_MR_HPP

#include <stdint.h>
#include <DriverKit/IOReturn.h>
#include <DriverKit/OSArray.h>
#include <DriverKit/IOMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#include <DriverKit/IOLib.h>
#include "MlxUCIO.h"
#include "MlxDMA.hpp"
#include "../hw/MlxP0Encoding.hpp"
#include "../hw/MlxP0EncodingIndirect.hpp"

class MlxRoCE;
class MlxPCIDriver;

#define MLX_MAX_MR_SEGMENTS     480

struct MlxMRContext {
    uint32_t    mrHandle;
    uint32_t    pd;
    uint32_t    lkey;
    uint32_t    rkey;
    uint64_t    startAddr;
    uint64_t    length;
    uint32_t    accessFlags;
    uint32_t    childCount;
    uint32_t    childHandles[MLX_UC_MAX_INDIRECT_CHILDREN];
    uint32_t    dependentCount;
    bool        invalidated;
    bool        isWindow;
    uint32_t    mwType;
    uint32_t    boundMrHandle;
    uint32_t    boundQpn;
    IOMemoryDescriptor *fMemDesc;
    MlxDMAReq    dma;
};

class MlxMR {
public:
    MlxMR();
    ~MlxMR();

    kern_return_t   Init(MlxRoCE *roce);
    void            Free();

    kern_return_t   RegMR(const struct mlx_reg_mr_req *req,
                           IOMemoryDescriptor *clientMemory,
                           struct mlx_reg_mr_resp *resp);
    kern_return_t   RegMRIndirect(const struct mlx_reg_mr_indirect_req *req,
                                  struct mlx_reg_mr_resp *resp);
    kern_return_t   DeregMR(uint32_t mrHandle);
    MlxMRContext *  Lookup(uint32_t mrHandle);
    MlxMRContext *  LookupByLkey(uint32_t lkey);
    MlxMRContext *  LookupByRkey(uint32_t rkey);
    bool            ValidateRange(uint32_t lkey, uint32_t pd,
                                  uint64_t addr, uint32_t length,
                                  bool deviceWrites);
    kern_return_t   InvalidateKey(uint32_t rkey);
    kern_return_t   AllocMW(uint32_t pd, uint32_t type, uint32_t *handle, uint32_t *rkey);
    kern_return_t   DeallocMW(uint32_t handle);
    kern_return_t   BindMW(uint32_t handle, uint32_t qpn, uint32_t mrHandle,
                            uint32_t bindRkey, uint32_t accessFlags,
                            uint64_t addr, uint64_t length, uint32_t *rkey);
    bool            ValidateRemoteKey(uint32_t rkey, uint32_t pd,
                                      uint64_t addr, uint32_t length,
                                      bool deviceWrites);

private:
    kern_return_t   BuildPBL(IOMemoryDescriptor *mem, uint64_t startAddr,
                             uint64_t length, uint64_t *paList,
                             uint32_t *numSegs, IODMACommand **dmaCommand);
    kern_return_t   CmdCreateMKey(const uint64_t *paList, uint32_t numSegs,
                                  uint64_t startAddr, uint64_t length,
                                  uint32_t accessFlags, uint32_t pd,
                                  uint32_t *mkey, uint32_t *lkey,
                                  uint32_t *rkey);
    kern_return_t   CmdDestroyMKey(uint32_t mkey);

    struct State;
    State *s;
};

#endif /* MLX_MR_HPP */
