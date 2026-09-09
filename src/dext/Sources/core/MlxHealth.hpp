/*
 * MlxHealth.hpp — Firmware health monitoring (generic Mellanox mlx5 family)
 *
 * Ported from: kernel_src/mlnx-ofed-kernel-5.9/core/health.c
 *
 * DriverKit port: the health timer uses IOTimerDispatchSource instead of
 * IOTimerEventSource. The init-segment health buffer is read via MMIO on BAR0
 * (IOPCIDevice::MemoryRead32). Fatal health state disables bus mastering
 * (IOPCIDevice::Close) and enters DMA quarantine.
 */
#ifndef MLX_HEALTH_HPP
#define MLX_HEALTH_HPP

#include <stdint.h>
#include <stdbool.h>
#include <DriverKit/IOReturn.h>
#include <DriverKit/IODispatchSource.h>
#include <DriverKit/IOTimerDispatchSource.h>
#include "MlxRegs.hpp"

class MlxPCIDriver;

/* The init-segment health buffer, decoded. Firmware fills it only when it
 * asserts, so on a healthy card every field reads zero. */
struct MlxHealthSnapshot {
    uint32_t assertVar[6];
    uint32_t assertExitPtr;
    uint32_t assertCallra;
    uint32_t time;
    uint32_t fwVer;
    uint32_t hwId;
    uint8_t  rfrSeverity;
    uint8_t  iriscIndex;
    uint8_t  synd;
    uint16_t extSynd;
    bool     deviceRemoved;
};

class MlxHealth {
public:
    MlxHealth();
    ~MlxHealth();

    kern_return_t   Init(MlxPCIDriver *core);
    void            Free();

    /* Start the periodic health check timer. */
    kern_return_t   StartTimer();
    void            StopTimer();

    /* Read the init-segment health counter and compare. */
    bool            IsHealthy() const;
    uint8_t         Syndrome() const;
    uint16_t        ExtSynd() const;
    /* True once the health counter and fw_rev have both read all-ones, which
     * is how a card that has fallen off the bus looks over MMIO. Latched. */
    bool            DeviceRemoved() const;
    void            ReadBuffer(MlxHealthSnapshot *snap) const;

    /* Called by the timer: check counter, parse syndrome/RFR severity. */
    void            Check();

    /* Force an unhealthy state from an async fatal event (DEVICE_FATAL /
     * WQ_CATAS_ERROR). Same fencing as the watchdog: read syndrome, enter
     * DMA quarantine, disable bus mastering. Idempotent. */
    void            MarkFatal();

private:
    struct State;
    State *s;
};

#endif /* MLX_HEALTH_HPP */