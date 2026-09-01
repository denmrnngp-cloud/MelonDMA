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
MlxHealth::IsHealthy() const { return s ? s->healthy : false; }
uint8_t
MlxHealth::Syndrome() const { return s ? s->synd : 0; }
uint16_t
MlxHealth::ExtSynd() const { return s ? s->extSynd : 0; }

void
MlxHealth::Check()
{
    if (!s) return;
    uint32_t counter = mlxMMIORead32BE(s->pci, s->barIndex,
                                       offsetof(struct MlxInitSeg, health_counter));
    if (counter == s->healthCounter) {
        if (++s->missed >= MLX_HEALTH_MISSED_THRESHOLD) {
            MLX_LOG("FATAL: watchdog — missed=%u", s->missed);
            MarkFatal();
        }
    } else {
        s->healthCounter = counter;
        s->missed = 0;
        s->healthy = true;
    }
}

void
MlxHealth::MarkFatal()
{
    if (!s) return;
    s->healthy = false;
    /* irisc_index/synd/ext_synd occupy one aligned big-endian dword.
     * DriverKit MemoryRead32 must not be issued at the byte-aligned
     * synd/ext_synd offsets. */
    uint32_t syndromeWord = mlxMMIORead32BE(
        s->pci, s->barIndex,
        offsetof(struct MlxInitSeg, health.irisc_index));
    s->synd = (uint8_t)((syndromeWord >> 16) & 0xff);
    s->extSynd = (uint16_t)(syndromeWord & 0xffff);
    MLX_LOG("FATAL: synd=0x%02x ext_synd=0x%04x — fencing DMA",
            s->synd, s->extSynd);
    /* Enter DMA quarantine via the core. */
    s->core->EnterDmaQuarantine(0x4153594E); /* 'ASYN' */
    /* §5.7 fail-closed: also stop the card's DMA by clearing bus
     * mastering. MSE stays set so MMIO remains readable while the
     * quarantined mappings drain to the next verified FLR. */
    uint16_t cmd = 0;
    s->pci->ConfigurationRead16(4, &cmd);
    s->pci->ConfigurationWrite16(4, cmd & ~0x2u);
    uint16_t verify = 0;
    s->pci->ConfigurationRead16(4, &verify);
    MLX_LOG("FATAL: bus mastering disabled (cmd=0x%04x)", verify);
}
