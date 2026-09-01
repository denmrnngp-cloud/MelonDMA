/*
 * MlxHCAConnectX4.cpp — ConnectX-4 / 4Lx hardware implementation (our target)
 *
 * The actual card on this project is a ConnectX-4 Lx (MCX4131A-BCAT,
 * DID 0x1015, subsystem 15b3:0005, board rev AF — notes/09 §2.1.1). Like
 * ConnectX-5, the driver shares one code base; model differences appear only
 * in PCI IDs, firmware capabilities, and ISSI negotiation.
 *
 * DriverKit port: MMIO reads/writes go through MlxPCIDriver's BAR0
 * (IOPCIDevice::MemoryRead32/Write32) via the MlxDriverKitCompat helpers, not
 * through an IOMemoryMap virtual address.
 */
#include "../hw/MlxHCA.hpp"
#include "../core/MlxDriverKitCompat.h"
#include "../hw/MlxRegs.hpp"
#include "../core/MlxCmd.hpp"

class MlxPCIDriver;

class MlxHCAConnectX4 : public MlxHCA {
public:
    MlxHCAConnectX4() : fCore(NULL), fCaps{}, fVendor{} {}
    virtual ~MlxHCAConnectX4() {}

    virtual void AttachCore(MlxPCIDriver *core) { fCore = core; }

    virtual const MlxHcaCaps &Caps() const { return fCaps; }
    virtual const MlxVendorInfo &Vendor() const { return fVendor; }
    virtual MlxHcaCaps &MutableCaps() { return fCaps; }
    virtual MlxVendorInfo &MutableVendor() { return fVendor; }

    virtual kern_return_t Exec(uint32_t opcode, const void *in,
                               uint32_t inSize, void *out,
                               uint32_t outSize, uint32_t timeoutMs);

    virtual uint32_t ReadRegBE(uintptr_t offset);
    virtual void WriteRegBE(uintptr_t offset, uint32_t value);

    virtual void *GetUarVirtual() { return NULL; }
    virtual uint64_t GetUarPhysical() { return 0; }

    kern_return_t LoadCaps();

private:
    MlxPCIDriver  *fCore;
    MlxHcaCaps     fCaps;
    MlxVendorInfo  fVendor;
};

kern_return_t
MlxHCAConnectX4::Exec(uint32_t opcode, const void *in, uint32_t inSize,
                       void *out, uint32_t outSize, uint32_t timeoutMs)
{
    /* Deferred to MlxPCIDriver::Exec once the core pointer is wired up in
     * Start(). The prototype returns kIOReturnNotReady until then. */
    (void)opcode; (void)in; (void)inSize; (void)out; (void)outSize; (void)timeoutMs;
    return kIOReturnNotReady;
}

uint32_t
MlxHCAConnectX4::ReadRegBE(uintptr_t offset)
{
    /* MlxPCIDriver exposes the BAR0 memory index; the compat helper does the
     * big-endian swap. Implemented once MlxPCIDriver::Start wires fBar0Index. */
    (void)offset;
    return 0;
}

void
MlxHCAConnectX4::WriteRegBE(uintptr_t offset, uint32_t value)
{
    (void)offset; (void)value;
}

kern_return_t
MlxHCAConnectX4::LoadCaps()
{
    /* QUERY_HCA_CAP (general, RoCE, Ethernet, NIC flow-table) — filled in once
     * MlxCmd is alive. Skeleton returns success with zeroed caps for now. */
    fVendor.vendorId = 0x15b3;
    fVendor.deviceId = 0x1015;   /* ConnectX-4 Lx */
    fCaps.portType = MLX_PORT_TYPE_ETH;
    fCaps.numPorts = 1;
    return kIOReturnSuccess;
}

/* ---- Factory dispatch (ConnectX-4 family) ---- */

MlxHCA *
MlxHCALoader::CreateCx4(uint16_t deviceId)
{
    /* ConnectX-4 PF 0x1013, VF 0x1014; ConnectX-4LX PF 0x1015, VF 0x1016.
     * Our hardware is 0x1015 — the case below. */
    switch (deviceId) {
    case 0x1013:
    case 0x1014:
    case 0x1015:    /* ConnectX-4LX PF — our card */
    case 0x1016:
        return new MlxHCAConnectX4;
    default:
        return NULL;
    }
}

MlxHCA *
MlxHCALoader::Create(uint16_t deviceId)
{
    MlxHCA *hca = CreateCx4(deviceId);
    if (hca) return hca;
    hca = CreateCx5(deviceId);
    if (hca) return hca;
    hca = CreateCx6(deviceId);
    if (hca) return hca;
    hca = CreateCx7(deviceId);
    if (hca) return hca;
    return NULL;
}

/* Stubs for other generations — return NULL until implemented. */
MlxHCA *MlxHCALoader::CreateCx5(uint16_t) { return NULL; }
MlxHCA *MlxHCALoader::CreateCx6(uint16_t) { return NULL; }
MlxHCA *MlxHCALoader::CreateCx7(uint16_t) { return NULL; }