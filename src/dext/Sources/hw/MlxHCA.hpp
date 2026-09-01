/*
 * MlxHCA.hpp — Hardware abstraction layer (generic Mellanox mlx5 family)
 *
 * Core decoupling point: all driver logic accesses hardware capabilities
 * through this interface without depending directly on a specific model.
 * First implementation: ConnectX-4 Lx (0x1015, our actual hardware).
 *
 * DriverKit port: MMIO goes through IOPCIDevice (PCIDriverKit) instead of
 * IOMemoryMap dereference. The HCA holds a back-pointer to MlxPCIDriver to
 * reach the BAR index + IOPCIDevice pointer.
 */
#ifndef MLX_HCA_HPP
#define MLX_HCA_HPP

#include <stdint.h>
#include <DriverKit/IOReturn.h>
#include "MlxRegs.hpp"
#include "MlxUCIO.h"

class MlxPCIDriver;

/* Port types (firmware port_type values) */
enum MlxPortType {
    MLX_PORT_TYPE_IB  = 0,
    MLX_PORT_TYPE_ETH = 1,
};

/* IB address parameters (plan C: reserved for IB addressing) */
struct MlxIBAddr {
    uint16_t dlid;
    uint8_t  pathBits;
    uint8_t  sl;
};

/*
 * Hardware capability summary — key fields returned by QUERY_HCA_CAP
 */
struct MlxHcaCaps {
    uint32_t fwRev;
    uint16_t cmdifRev;
    uint8_t  portType;
    uint32_t numPorts;

    uint32_t maxQp;
    uint32_t maxCq;
    uint32_t maxMr;
    uint8_t  logMaxMsg;

    /* RoCE (See mlx5_ifc.h:1140 roce_cap_bits) */
    bool     roce;
    bool     roceRwSupported;
    uint8_t  roceVersions;
    uint16_t roceMaxGid;
    uint16_t roceDstUdpPort;
    uint16_t roceMinSrcUdpPort;
    bool     roceCcCaps;
    bool     dcqcnEnabled;

    bool     uar4k;
    uint16_t logUarPageSize;
    uint8_t  logBfRegSize;
    bool     swRoceSrcUdpPort;
    bool     nicFlowTable;
    bool     ethNetOffloads;
    uint8_t  numVhcaPorts;
    bool     swOwnerId;
    uint8_t  atomicMode;    /* QPC atomic_mode: 0=NONE 1=IB_COMP 3=8B */

    uint16_t ibMaxLids;
    uint16_t ibMaxPkeys;
    bool     ibSupported;

    bool isEthernet() const { return portType == MLX_PORT_TYPE_ETH; }
    bool isIB() const { return portType == MLX_PORT_TYPE_IB; }
    int linkLayer() const {
        return isEthernet() ? MLX_LINK_LAYER_ETHERNET :
               isIB() ? MLX_LINK_LAYER_INFINIBAND :
               MLX_LINK_LAYER_UNSPECIFIED;
    }
};

struct MlxVendorInfo {
    uint16_t vendorId;
    uint16_t deviceId;
    uint32_t revision;
};

/*
 * HCA abstract interface — all sub-modules (QP/CQ/MR/AH) depend only on this.
 * Concrete impl: MlxHCAConnectX4 (our hardware is ConnectX-4 Lx, DID 0x1015).
 */
class MlxHCA {
public:
    virtual ~MlxHCA() {}
    virtual void AttachCore(MlxPCIDriver *core) = 0;

    virtual const MlxHcaCaps &Caps() const = 0;
    virtual const MlxVendorInfo &Vendor() const = 0;
    virtual MlxHcaCaps &MutableCaps() = 0;
    virtual MlxVendorInfo &MutableVendor() = 0;

    /* Command execution (implemented by MlxCmd via MlxPCIDriver::Exec) */
    virtual kern_return_t Exec(uint32_t opcode, const void *in,
                               uint32_t inSize, void *out,
                               uint32_t outSize, uint32_t timeoutMs) = 0;

    /* Register access via MMIO on the BAR (big-endian, mlx5 convention). */
    virtual uint32_t ReadRegBE(uintptr_t offset) = 0;
    virtual void WriteRegBE(uintptr_t offset, uint32_t value) = 0;

    /* UAR mapping (data path/user space). Returns NULL until mapped. */
    virtual void *GetUarVirtual() = 0;
    virtual uint64_t GetUarPhysical() = 0;
};

/*
 * Hardware capability loader — model dispatch by PCI DID.
 */
class MlxHCALoader {
public:
    static MlxHCA *Create(uint16_t deviceId);
    static MlxHCA *CreateCx4(uint16_t deviceId);
    static MlxHCA *CreateCx5(uint16_t deviceId);
    static MlxHCA *CreateCx6(uint16_t deviceId);
    static MlxHCA *CreateCx7(uint16_t deviceId);
};

#endif /* MLX_HCA_HPP */
