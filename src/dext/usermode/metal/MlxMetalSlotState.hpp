#pragma once
#include <stdint.h>

/* Shared by the application wrapper and hardware-independent tests.
 * A poisoned slot is deliberately not reusable without a new allocation. */
enum class MlxMetalPhase : uint8_t {
    Free, TxAcquired, Producing, TxReady, Sending,
    RxAcquired, Receiving, RxReady, Consuming, Poisoned
};
struct MlxMetalLease {
    uint64_t allocation, epoch, sequence;
    uint32_t slot;
};
struct MlxMetalSlotState {
    MlxMetalPhase phase = MlxMetalPhase::Free;
    uint64_t sequence = 0;
    uint32_t qpn = 0;
    bool matches(const MlxMetalLease &lease, uint64_t allocation, uint64_t epoch) const {
        return lease.allocation == allocation && lease.epoch == epoch &&
               lease.sequence && lease.sequence == sequence;
    }
    bool move(MlxMetalPhase from, MlxMetalPhase to) {
        if (phase != from) return false;
        phase = to; return true;
    }
};
