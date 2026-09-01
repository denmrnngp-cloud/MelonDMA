/*
 * MlxEQ.hpp — Event Queue (generic Mellanox mlx5 family)
 *
 * Ported from: kernel_src/mlnx-ofed-kernel-5.9/core/eq.c
 *
 * DriverKit port: MSI-X vectors come from
 * IOPCIDevice::ConfigureInterrupts(kIOInterruptTypePCIMessagedX, …). Event
 * dispatch uses IOInterruptDispatchSource. EQ masks are four 64-bit words
 * serialized (REMEDIATION_PLAN §5.5). Initial arm happens after the interrupt
 * source attaches.
 */
#ifndef MLX_EQ_HPP
#define MLX_EQ_HPP

#include <stdint.h>
#include <DriverKit/IOReturn.h>
#include <DriverKit/IOMemoryDescriptor.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#include <DriverKit/IOLib.h>
#include "MlxRegs.hpp"

class MlxPCIDriver;
class IOPCIDevice;

#define MLX_EQ_DEPTH        256
#define MLX_EQE_SIZE        64   /* mlx5_eqe = 64 bytes (device.h:769) */
#define MLX_MAX_EQ_PAGES    8

class MlxEventNotifier {
public:
    virtual ~MlxEventNotifier() {}
    virtual void HandleEvent(uint32_t type, void *eqe) = 0;
};

class MlxEQ {
public:
    MlxEQ();
    ~MlxEQ();

    kern_return_t   Init(MlxPCIDriver *core, uint32_t vector);
    void            Free();

    /* Create the EQ in firmware and arm it. */
    kern_return_t   CreateEQ(uint32_t *eqn);
    kern_return_t   DestroyEQ(uint32_t eqn);
    /* TEARDOWN_HCA invalidates all HCA objects. Clear firmware ownership
     * locally so the already-quiesced DMA ring can be released safely. */
    void            MarkDestroyedByTeardown();
    uint32_t        EqNumber() const;

    /* Register/unregister a notifier (MlxRoCE) for event dispatch. */
    void            AddNotifier(MlxEventNotifier *n);
    void            RemoveNotifier(MlxEventNotifier *n);

    /* Poll the EQE ring and dispatch events (called from interrupt source). */
    void            Poll();

    /* Arm the EQ after an interrupt storm or initial setup. */
    kern_return_t   Arm();

private:
    struct State;
    State *s;
    void            UpdateCi(bool arm);
};

#endif /* MLX_EQ_HPP */
