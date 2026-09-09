/*
 * MlxHealth.cpp — Firmware health monitoring (DriverKit port).
 *
 * Ported from: kernel_src/mlnx-ofed-kernel-5.9/core/health.c. The health
 * timer uses IOTimerDispatchSource. The init-segment health buffer is read
 * via MMIO on BAR0 (IOPCIDevice::MemoryRead32). Fatal health state enters DMA
 * quarantine.
 */
#include "MlxHealth.hpp"
#include "MlxDriverKitCompat.h"
#include "MlxRegs.hpp"
#include "MlxHealthSyndrome.hpp"
#include "MlxPCIDriver.h"

#include <DriverKit/IOLib.h>
#include <DriverKit/IOTimerDispatchSource.h>
#include <PCIDriverKit/IOPCIDevice.h>

#include "MlxLog.hpp"
#define MLX_LOG(fmt, ...)  IOLog("MlxHealth: " fmt "\n", ##__VA_ARGS__)
#define MLX_DBG(fmt, ...)  MLX_DBGLOG("MlxHealth: " fmt, ##__VA_ARGS__)
#define MLX_HEALTH_MISSED_THRESHOLD 5

struct MlxHealth::State {
    MlxPCIDriver          *core;
    IOPCIDevice           *pci;
    uint8_t                barIndex;
    IOTimerDispatchSource *timer;
    uint32_t               healthCounter;
    uint32_t               missed;
    uint8_t                synd;
    uint16_t               extSynd;
    bool                   healthy;
    bool                   deviceRemoved;
};

MlxHealth::MlxHealth() : s(NULL) {}
MlxHealth::~MlxHealth() { Free(); }

kern_return_t
MlxHealth::Init(MlxPCIDriver *core)
{
    if (!core) return kIOReturnBadArgument;
    s = new State;
    if (!s) return kIOReturnNoMemory;
    memset(s, 0, sizeof(*s));
    s->core = core;
    s->pci  = core->GetPCI();
    s->barIndex = core->GetBar0Index();
    s->healthy = true;
    /* Read the initial health counter so the first Check() compares against
     * a baseline (device.h health_counter at end of init-seg). */
    s->healthCounter = mlxMMIORead32BE(s->pci, s->barIndex,
                                        offsetof(struct MlxInitSeg, health_counter));
    return kIOReturnSuccess;
}

void
MlxHealth::Free()
{
    if (!s) return;
    StopTimer();
    delete s; s = NULL;
}

kern_return_t
MlxHealth::StartTimer()
{
    if (!s) return kIOReturnNotReady;
    /* The core's continuously armed EQ timer invokes Check() every two
     * seconds.  A second dispatch source would duplicate lifecycle and
     * cancellation state, so this method only validates initialization. */
    return kIOReturnSuccess;
}

void
MlxHealth::StopTimer()
{
    if (!s || !s->timer) return;
    IOTimerDispatchSource *timer = s->timer;
    s->timer = NULL;
    /* Cancel is asynchronous. Keep the +1 reference until DriverKit confirms
     * that every callback has completed; immediate release is a UAF. */
    kern_return_t kr = timer->Cancel(^{ timer->release(); });
    if (kr != kIOReturnSuccess)
        MLX_LOG("timer cancel failed kr=0x%x — reference retained", kr);
}

bool
MlxHealth::IsHealthy() const {
    return s && __atomic_load_n(&s->healthy, __ATOMIC_ACQUIRE) &&
           !s->core->DmaQuarantined();
}
uint8_t
MlxHealth::Syndrome() const { return s ? s->synd : 0; }
uint16_t
MlxHealth::ExtSynd() const { return s ? s->extSynd : 0; }

/* Read and decode the init-segment health buffer.
 *
 * Layout is mlx5's struct health_buffer at BAR0 offset 0x200, taken from the
 * kernel header rather than recalled: six assert words, the assert exit and
 * call-return pointers, a timestamp, firmware version and hardware id, then
 * two packed bytes-and-halfword dwords carrying rfr_severity, irisc_index,
 * synd and ext_synd. The last two dwords must be read as dwords: DriverKit's
 * MemoryRead32 cannot be issued at the byte offsets of synd or ext_synd.
 *
 * Firmware fills this only when it asserts, so on a healthy card it reads as
 * zeros, which is the correct answer rather than a missing one. */
void
MlxHealth::ReadBuffer(MlxHealthSnapshot *snap) const
{
    if (!snap) return;
    memset(snap, 0, sizeof(*snap));
    if (!s) return;
    for (uint32_t i = 0; i < 6; i++)
        snap->assertVar[i] = mlxMMIORead32BE(
            s->pci, s->barIndex,
            offsetof(struct MlxInitSeg, health.assert_var) + i * 4);
    snap->assertExitPtr = mlxMMIORead32BE(s->pci, s->barIndex,
        offsetof(struct MlxInitSeg, health.assert_exit_ptr));
    snap->assertCallra = mlxMMIORead32BE(s->pci, s->barIndex,
        offsetof(struct MlxInitSeg, health.assert_callra));
    snap->time = mlxMMIORead32BE(s->pci, s->barIndex,
        offsetof(struct MlxInitSeg, health.time));
    snap->fwVer = mlxMMIORead32BE(s->pci, s->barIndex,
        offsetof(struct MlxInitSeg, health.fw_ver));
    snap->hwId = mlxMMIORead32BE(s->pci, s->barIndex,
        offsetof(struct MlxInitSeg, health.hw_id));
    const uint32_t severityWord = mlxMMIORead32BE(s->pci, s->barIndex,
        offsetof(struct MlxInitSeg, health.rfr_severity));
    snap->rfrSeverity = (uint8_t)((severityWord >> 24) & 0xff);
    const uint32_t syndromeWord = mlxMMIORead32BE(s->pci, s->barIndex,
        offsetof(struct MlxInitSeg, health.irisc_index));
    snap->iriscIndex = (uint8_t)((syndromeWord >> 24) & 0xff);
    snap->synd = (uint8_t)((syndromeWord >> 16) & 0xff);
    snap->extSynd = (uint16_t)(syndromeWord & 0xffff);
    snap->deviceRemoved = __atomic_load_n(&s->deviceRemoved, __ATOMIC_ACQUIRE);
}

bool
MlxHealth::DeviceRemoved() const
{
    return s && __atomic_load_n(&s->deviceRemoved, __ATOMIC_ACQUIRE);
}

void
MlxHealth::Check()
{
    if (!IsHealthy()) return; /* fatal is latched until a new Init after reset */
    uint32_t counter = mlxMMIORead32BE(s->pci, s->barIndex,
                                       offsetof(struct MlxInitSeg, health_counter));
    /* All-ones from MMIO is what a card that has fallen off the bus reads as,
     * not a counter value. Apple's pollHealth makes the same check and
     * confirms it against fw_rev before believing it, because a real counter
     * can legitimately be 0xffffffff for one tick. Latch it: every later MMIO
     * read would return the same garbage, and a client polling health should
     * be told the device is gone rather than that it is fine. */
    if (counter == 0xffffffffu) {
        uint32_t fwRev = mlxMMIORead32BE(s->pci, s->barIndex,
                                         offsetof(struct MlxInitSeg, fw_rev));
        if (fwRev == 0xffffffffu) {
            if (!__atomic_exchange_n(&s->deviceRemoved, true, __ATOMIC_ACQ_REL)) {
                MLX_LOG("device removed: health counter and fw_rev both read all-ones");
                __atomic_store_n(&s->healthy, false, __ATOMIC_RELEASE);
            }
            return;
        }
    }
    /* On ConnectX-4 Lx firmware health_counter is a stable snapshot in the
     * normal state; it is not a periodic heartbeat. Treating an unchanged
     * value as fatal quarantines a healthy card after ~10 seconds — which is
     * exactly what Apple's driver does after three identical reads, and is
     * wrong for this part. Fatal hardware state is delivered through
     * DEVICE_FATAL/WQ_FATAL and enters MarkFatal() from
     * MlxRoCE::HandleAsyncEvent(). */
    if (counter != s->healthCounter) {
        s->healthCounter = counter;
        s->missed = 0;
    }
}

void
MlxHealth::MarkFatal()
{
    if (!s) return;
    if (!__atomic_exchange_n(&s->healthy, false, __ATOMIC_ACQ_REL)) return;
    MlxHealthSnapshot snap = {};
    ReadBuffer(&snap);
    s->synd = snap.synd;
    s->extSynd = snap.extSynd;
    /* Everything firmware left behind, in one line. A syndrome number on its
     * own says an assert happened; the assert words and the firmware version
     * that produced them are what a bug report can actually be matched
     * against, and they are unreadable once the card is reset. */
    MLX_LOG("FATAL: synd=0x%02x (%s) ext_synd=0x%04x rfr_severity=0x%02x "
            "irisc=%u fw_ver=0x%08x hw_id=0x%08x time=0x%08x "
            "assert_exit=0x%08x assert_callra=0x%08x",
            snap.synd, mlxHealthSyndromeName(snap.synd), snap.extSynd,
            snap.rfrSeverity, snap.iriscIndex,
            snap.fwVer, snap.hwId, snap.time,
            snap.assertExitPtr, snap.assertCallra);
    MLX_LOG("FATAL: assert_var %08x %08x %08x %08x %08x %08x — fencing DMA",
            snap.assertVar[0], snap.assertVar[1], snap.assertVar[2],
            snap.assertVar[3], snap.assertVar[4], snap.assertVar[5]);
    /* Enter DMA quarantine via the core. */
    s->core->EnterDmaQuarantine(0x4153594E); /* 'ASYN' */
    /* §5.7 fail-closed: also stop the card's DMA by clearing bus
     * mastering. MSE stays set so MMIO remains readable while the
     * quarantined mappings drain to the next verified FLR. */
    /* EnterDmaQuarantine performs and verifies the common BME fence. */
}
