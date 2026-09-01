/*
 * MlxGID.cpp — GID table management (DriverKit port).
 *
 * Ported from: drivers/infiniband/hw/mlx5/main.c:577 (set_roce_addr). The GID
 * table is written to firmware via SET_ROCE_ADDRESS (0x761). macOS does not
 * expose kernel networking hooks to a DEXT, so IP/MAC changes arrive from a
 * userspace policy daemon through an ExternalMethod.
 */
#include "MlxGID.hpp"
#include "MlxRoCE.hpp"
#include "MlxPCIDriver.h"
#include "MlxCmd.hpp"
#include "MlxRegs.hpp"

#include <DriverKit/IOLib.h>
#include <stdlib.h>
#include <string.h>
#include "MlxIfcHelpers.hpp"   /* mlxSetBits */
#include "MlxRegs.hpp"

#include "MlxLog.hpp"
#define MLX_LOG(fmt, ...)  IOLog("MlxGID: " fmt "\n", ##__VA_ARGS__)
#define MLX_DBG(fmt, ...)  MLX_DBGLOG("MlxGID: " fmt, ##__VA_ARGS__)

static bool
mlxBytesAreZero(const uint8_t *bytes, uint32_t length)
{
    for (uint32_t i = 0; i < length; i++) if (bytes[i]) return false;
    return true;
}

static bool
mlxIsIpv4MappedGid(const uint8_t gid[16])
{
    for (uint32_t i = 0; i < 10; i++) if (gid[i]) return false;
    return gid[10] == 0xff && gid[11] == 0xff;
}

struct MlxGID::State {
    MlxRoCE      *roce;
    MlxPCIDriver *core;
    MlxGIDEntry  *table;
    uint32_t       tableSize;
    struct IOLock *lock;
};

MlxGID::MlxGID() : s(NULL) {}
MlxGID::~MlxGID() { Free(); }

kern_return_t
MlxGID::Init(MlxRoCE *roce, MlxPCIDriver *core, uint32_t tableSize)
{
    if (!roce || !core || !tableSize) return kIOReturnBadArgument;
    s = new State;
    if (!s) return kIOReturnNoMemory;
    memset(s, 0, sizeof(*s));
    s->roce = roce;
    s->core = core;
    s->tableSize = tableSize;
    s->table = new MlxGIDEntry[tableSize];
    s->lock = IOLockAlloc();
    if (!s->table || !s->lock) {
        delete[] s->table;
        if (s->lock) IOLockFree(s->lock);
        delete s; s = NULL;
        return kIOReturnNoMemory;
    }
    for (uint32_t i = 0; i < tableSize; i++) s->table[i].index = i;
    MLX_DBG("Init implementation=p2-address-standards-1 table=%u", tableSize);
    return kIOReturnSuccess;
}

void
MlxGID::Free()
{
    if (!s) return;
    /* Clear live firmware entries while the command interface still exists.
     * UserClient cleanup normally did this already; this also covers core
     * reinit and failed/aborted clients. */
    if (!s->core->DmaQuarantined()) {
        for (uint32_t i = 0; i < s->tableSize; i++) {
            if (s->table[i].used) {
                kern_return_t kr = DelGID(i);
                if (kr != kIOReturnSuccess)
                    MLX_LOG("GID[%u] firmware clear unverified: 0x%x", i, kr);
            }
        }
    }
    if (s->lock) { IOLockFree(s->lock); s->lock = NULL; }
    delete[] s->table; s->table = NULL;
    delete s; s = NULL;
}

uint32_t
MlxGID::AllocGIDIndex()
{
    if (!s) return 0xFFFFFFFF;
    uint32_t idx = 0xFFFFFFFF;
    IOLockLock(s->lock);
    for (uint32_t i = 0; i < s->tableSize; i++) {
        if (!s->table[i].used) { idx = i; break; }
    }
    IOLockUnlock(s->lock);
    if (idx == 0xFFFFFFFF) {
        /* A failed SET/readback can leave a local reservation behind. */
        IOLockLock(s->lock);
        for (uint32_t i = 0; i < s->tableSize; i++) {
            if (s->table[i].used && !s->table[i].roceVersion) { idx = i; break; }
        }
        IOLockUnlock(s->lock);
    }
    if (idx == 0xFFFFFFFF) return idx;

    /* Firmware state survives a failed client teardown. The slot has no
     * active local owner at this point, so clear it before reserving it. */
    uint8_t zeroGid[16] = {}, zeroMac[6] = {};
    if (CmdSetRoceAddr(idx, zeroGid, zeroMac, 0, 0, false, 0) != kIOReturnSuccess)
        return 0xFFFFFFFF;
    IOLockLock(s->lock);
    if (s->table[idx].used && s->table[idx].roceVersion) {
        IOLockUnlock(s->lock);
        return 0xFFFFFFFF;
    }
    memset(&s->table[idx], 0, sizeof(s->table[idx]));
    s->table[idx].index = idx;
    s->table[idx].used = true;
    IOLockUnlock(s->lock);
    return idx;
}

void
MlxGID::FreeGIDIndex(uint32_t index)
{
    if (!s || index >= s->tableSize) return;
    IOLockLock(s->lock);
    memset(&s->table[index], 0, sizeof(s->table[index]));
    s->table[index].index = index;
    IOLockUnlock(s->lock);
}

kern_return_t
MlxGID::SetGID(uint32_t index, const uint8_t *gid, const uint8_t *mac,
               uint8_t roceVersion, uint8_t l3Type, bool vlanEn, uint16_t vlanId)
{
    if (!s || index >= s->tableSize || !gid || !mac) return kIOReturnBadArgument;
    const MlxHcaCaps &caps = s->core->GetHCA()->Caps();
    bool macInvalid = mlxBytesAreZero(mac, 6) || (mac[0] & 1);
    bool gidInvalid = mlxBytesAreZero(gid, 16) || gid[0] == 0xff;
    bool addressTypeMismatch = (l3Type == 0) != mlxIsIpv4MappedGid(gid);
    bool ipv4Invalid = l3Type == 0 &&
                       (gid[12] == 0 || gid[12] >= 224);
    if (roceVersion != MLX_ROCE_VERSION_2 || !(caps.roceVersions & 0x2) ||
        l3Type > 1 || macInvalid || gidInvalid || addressTypeMismatch ||
        ipv4Invalid ||
        (vlanEn ? vlanId > 4095 : vlanId != 0))
        return kIOReturnBadArgument;
    /* First firmware SET + strict readback; software table — only after
     * success (otherwise the table would lie about the real fw state). */
    kern_return_t kr = CmdSetRoceAddr(index, gid, mac, roceVersion, l3Type,
                                      vlanEn, vlanId);
    if (kr != kIOReturnSuccess) return kr;
    IOLockLock(s->lock);
    memcpy(s->table[index].gid, gid, 16);
    memcpy(s->table[index].mac, mac, 6);
    s->table[index].roceVersion = roceVersion;
    s->table[index].l3Type = l3Type;
    s->table[index].vlanId = vlanId;
    s->table[index].vlanEn = vlanEn;
    s->table[index].gidType = 2;   /* RoCEv2 */
    s->table[index].used = true;
    IOLockUnlock(s->lock);
    /* PCIDriverKit has no macOS netif hooks; a programmatic GID change is the
     * only signal path. Surface it as a verbs GID_CHANGE async event. */
    if (s->roce) s->roce->QueueAsyncEvent(MLX_EVENT_GID_CHANGE,
                                          MLX_ASYNC_ELEMENT_PORT, 1);
    return kIOReturnSuccess;
}

kern_return_t
MlxGID::QueryAll(const struct mlx_query_gid_table_req *req,
                 struct mlx_query_gid_table_resp *resp)
{
    if (!s || !req || !resp || !req->maxEntries ||
        req->maxEntries > MLX_UC_MAX_GID_CHUNK)
        return kIOReturnBadArgument;
    memset(resp, 0, sizeof(*resp));
    resp->tableSize = s->tableSize;
    uint32_t start = req->startIndex;
    uint32_t end = start + req->maxEntries;
    if (end > s->tableSize) end = s->tableSize;
    IOLockLock(s->lock);
    for (uint32_t i = start; i < end && resp->count < req->maxEntries; i++) {
        if (!s->table[i].used || !s->table[i].roceVersion) continue;
        struct mlx_gid_table_entry *e = &resp->entry[resp->count];
        e->index = s->table[i].index;
        memcpy(e->gid, s->table[i].gid, 16);
        memcpy(e->mac, s->table[i].mac, 6);
        e->roceVersion = s->table[i].roceVersion;
        e->l3Type = s->table[i].l3Type;
        e->gidType = s->table[i].gidType ? s->table[i].gidType : 2;
        e->vlanValid = s->table[i].vlanEn ? 1 : 0;
        e->vlanId = s->table[i].vlanId;
        e->ifindex = 0;   /* no macOS netif behind the PCIDriverKit DEXT */
        resp->count++;
    }
    resp->more = (end < s->tableSize) ? 1 : 0;
    IOLockUnlock(s->lock);
    return kIOReturnSuccess;
}

kern_return_t
MlxGID::DelGID(uint32_t index)
{
    if (!s || index >= s->tableSize) return kIOReturnBadArgument;
    if (s->core->DmaQuarantined()) return kIOReturnNotReady;
    uint8_t zero[16] = {0};
    uint8_t mac[6] = {0};
    /* Firmware canonicalizes an empty slot to an all-zero address layout,
     * including roce_version.  Compare against that canonical form. */
    kern_return_t kr = CmdSetRoceAddr(index, zero, mac, 0, 0, false, 0);
    if (kr == kIOReturnSuccess) {
        FreeGIDIndex(index);
        if (s->roce) s->roce->QueueAsyncEvent(MLX_EVENT_GID_CHANGE,
                                              MLX_ASYNC_ELEMENT_PORT, 1);
    }
    return kr;
}

kern_return_t
MlxGID::GetLocalAddr(uint8_t *gid, uint8_t *mac)
{
    if (!s) return kIOReturnNotReady;
    IOLockLock(s->lock);
    for (uint32_t i = 0; i < s->tableSize; i++) {
        if (s->table[i].used) {
            if (gid) memcpy(gid, s->table[i].gid, 16);
            if (mac) memcpy(mac, s->table[i].mac, 6);
            IOLockUnlock(s->lock);
            return kIOReturnSuccess;
        }
    }
    IOLockUnlock(s->lock);
    return kIOReturnNotFound;
}

kern_return_t
MlxGID::QueryGID(uint32_t index, MlxGIDEntry *entry)
{
    if (!s || !entry || index >= s->tableSize) return kIOReturnBadArgument;
    IOLockLock(s->lock);
    if (!s->table[index].used || !s->table[index].roceVersion) {
        IOLockUnlock(s->lock);
        return kIOReturnNotFound;
    }
    *entry = s->table[index];
    IOLockUnlock(s->lock);
    return kIOReturnSuccess;
}

bool
MlxGID::IsProgrammed(uint32_t index)
{
    if (!s || index >= s->tableSize) return false;
    IOLockLock(s->lock);
    bool programmed = s->table[index].used &&
                      s->table[index].roceVersion != 0;
    IOLockUnlock(s->lock);
    return programmed;
}

uint32_t
MlxGID::TableSize() const
{
    return s ? s->tableSize : 0;
}

kern_return_t
MlxGID::CmdSetRoceAddr(uint32_t index, const uint8_t *gid, const uint8_t *mac,
                        uint8_t version, uint8_t l3Type, bool vlanEn, uint16_t vlanId)
{
    /* SET_ROCE_ADDRESS (0x761), mlx5_ifc_set_roce_address_in_bits:
     *   opcode@0x00, roce_address_index@0x40 (16b), vhca_port_num@0x5c (4b),
     *   roce_address (roce_addr_layout) at bit 0x80:
     *     source_l3_address (GID)  @0x80   (16 B)
     *     vlan_valid@0x103, vlan_id@0x104 (12b)
     *     source_mac_47_32@0x110 (2 B) + source_mac_31_0@0x120 (4 B) = 6 B MAC
     *     roce_l3_type@0x154 (4b), roce_version@0x158 (8b)
     * Total 0x180 bits = 48 B.
     * vhca_port_num != 0 only if capability num_vhca_ports > 0 (Linux gid.c). */
    uint8_t numVhcaPorts = 0;
    if (s->core->GetHCA()) numVhcaPorts = s->core->GetHCA()->Caps().numVhcaPorts;
    uint8_t vhcaPort = (numVhcaPorts > 0) ? 1 : 0;
    MLX_DBG("CmdSet enter idx=%u vhca_port=%u ver=%u l3=%u vlan=%u",
            index, vhcaPort, version, l3Type, vlanEn ? 1 : 0);

    uint8_t in[64] = {};
    uint8_t out[16] = {};
    mlxSetBits(in, 0x00, 16, MLX_CMD_OP_SET_ROCE_ADDRESS);
    mlxSetBits(in, 0x40, 16, index);       /* roce_address_index */
    mlxSetBits(in, 0x5c, 4, vhcaPort);     /* vhca_port_num */
    memcpy(in + 0x10, gid, 16);            /* source_l3_address */
    mlxSetBits(in, 0x103, 1, vlanEn ? 1 : 0);
    mlxSetBits(in, 0x104, 12, vlanId);
    memcpy(in + 0x22, mac, 6);             /* source_mac_47_32 + _31_0 */
    mlxSetBits(in, 0x154, 4, l3Type);      /* roce_l3_type (0=IPv4 1=IPv6) */
    mlxSetBits(in, 0x158, 8, version);     /* roce_version (0=v1, 2=v2) */

    kern_return_t kr = s->core->Exec(MLX_CMD_OP_SET_ROCE_ADDRESS, in, 0x30,
                                     out, sizeof(out), 5000);
    if (kr != kIOReturnSuccess) {
        MLX_LOG("SET_ROCE_ADDRESS(idx=%u) failed: 0x%x", index, kr);
        return kr;
    }
    /* Clearing a GID is idempotent. ConnectX firmware canonicalizes empty
     * entries differently across revisions, so the successful command is the
     * authoritative boundary; a strict query comparison can leak every local
     * slot after an error-path cleanup. */
    if (!version) return kIOReturnSuccess;

    /* Readback: QUERY_ROCE_ADDRESS (0x760) — same layout, out at bit 0x80. */
    uint8_t qin[16] = {};
    uint8_t qout[64] = {};
    mlxSetBits(qin, 0x00, 16, MLX_CMD_OP_QUERY_ROCE_ADDRESS);
    mlxSetBits(qin, 0x40, 16, index);
    mlxSetBits(qin, 0x5c, 4, vhcaPort);
    kr = s->core->Exec(MLX_CMD_OP_QUERY_ROCE_ADDRESS, qin, sizeof(qin),
                       qout, sizeof(qout), 5000);
    if (kr != kIOReturnSuccess) {
        MLX_LOG("QUERY_ROCE_ADDRESS(idx=%u) readback failed: 0x%x", index, kr);
        return kr;
    }
    uint8_t rgid[16] = {};
    uint8_t rmac[6] = {};
    memcpy(rgid, qout + 0x10, 16);
    memcpy(rmac, qout + 0x22, 6);
    uint8_t rver  = (uint8_t)mlxGetBits(qout, 0x158, 8);
    uint8_t rl3   = (uint8_t)mlxGetBits(qout, 0x154, 4);
    uint8_t rvlan = (uint8_t)mlxGetBits(qout, 0x103, 1);
    uint16_t rvlanId = (uint16_t)mlxGetBits(qout, 0x104, 12);

    MLX_DBG("GID readback idx=%u ver=%u l3=%u vlan=%u", index, rver, rl3, rvlan);

    /* Strict comparison against the expected value — do not consider staging successful on mismatch. */
    if (memcmp(rgid, gid, 16) || memcmp(rmac, mac, 6) ||
        rver != version || rl3 != l3Type || rvlan != (vlanEn ? 1 : 0) ||
        rvlanId != (vlanEn ? vlanId : 0)) {
        MLX_LOG("SET_ROCE_ADDRESS(idx=%u) READBACK MISMATCH", index);
        return kIOReturnIOError;
    }
    MLX_DBG("SET_ROCE_ADDRESS(idx=%u) readback match", index);
    return kIOReturnSuccess;
}
