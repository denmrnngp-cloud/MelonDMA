/*
 * MlxGID.hpp — GID table management (generic Mellanox family)
 *
 * Ported from: drivers/infiniband/hw/mlx5/main.c:577 (set_roce_addr).
 *
 * DriverKit port: the GID table is written to firmware via SET_ROCE_ADDRESS.
 * IP/MAC changes arrive from a userspace policy daemon (no kernel net hooks).
 */
#ifndef MLX_GID_HPP
#define MLX_GID_HPP

#include <stdint.h>
#include <stdbool.h>
#include <DriverKit/IOReturn.h>
#include <DriverKit/IOLib.h>
#include "MlxRegs.hpp"
#include "MlxUCIO.h"

class MlxRoCE;
class MlxPCIDriver;

struct MlxGIDEntry {
    bool        used;
    uint32_t    index;
    uint8_t     gid[16];
    uint8_t     mac[6];
    uint8_t     roceVersion;
    uint8_t     l3Type;
    uint16_t    vlanId;
    bool        vlanEn;
    uint8_t     gidType;      /* ibv_gid_type: 2=RoCEv2 */
};

class MlxGID {
public:
    MlxGID();
    ~MlxGID();

    kern_return_t   Init(MlxRoCE *roce, MlxPCIDriver *core, uint32_t tableSize);
    void            Free();

    uint32_t        AllocGIDIndex();
    void            FreeGIDIndex(uint32_t index);

    kern_return_t   SetGID(uint32_t index, const uint8_t *gid,
                            const uint8_t *mac, uint8_t roceVersion,
                            uint8_t l3Type, bool vlanEn, uint16_t vlanId);
    kern_return_t   DelGID(uint32_t index);

    kern_return_t   GetLocalAddr(uint8_t *gid, uint8_t *mac);
    kern_return_t   QueryGID(uint32_t index, MlxGIDEntry *entry);
    kern_return_t   QueryAll(const struct mlx_query_gid_table_req *req,
                            struct mlx_query_gid_table_resp *resp);
    bool            IsProgrammed(uint32_t index);
    uint32_t        TableSize() const;

private:
    struct State;
    State *s;
    kern_return_t   CmdSetRoceAddr(uint32_t index, const uint8_t *gid,
                                     const uint8_t *mac, uint8_t version,
                                     uint8_t l3Type, bool vlanEn, uint16_t vlanId);
};

#endif /* MLX_GID_HPP */
