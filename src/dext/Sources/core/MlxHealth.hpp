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