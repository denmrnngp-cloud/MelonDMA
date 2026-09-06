#include "MlxSafety.hpp"
#include "../usermode/metal/MlxMetalSlotState.hpp"
#include <assert.h>
#include <stdio.h>

int main() {
    for (uint32_t command = 0; command < 65535; ++command) {
        uint16_t fenced = mlxFencedPciCommand(command);
        assert(!(fenced & 4));
        assert((fenced & 2) == (command & 2));
        assert(mlxPciFenceVerified(command, fenced));
    }
    assert(!mlxPciFenceVerified(7, 7));
    assert(!mlxPciFenceVerified(7, 1));
    assert(!mlxPciFenceVerified(0xffff, 3));
    assert(!mlxPciFenceVerified(7, 0xffff));
    assert(mlxBytesCanReserve(8, 8, 16));
    assert(!mlxBytesCanReserve(8, 9, 16));
    assert(!mlxBytesCanReserve(UINT64_MAX, 1, UINT64_MAX));
    assert(!mlxBytesCanReserve(0, 0, 16));
    assert(mlxPinnedCharge(1, 16384) == 16384);
    assert(mlxPinnedCharge(16384, 16384) == 32768);
    assert(!mlxPinnedCharge(UINT64_MAX, 16384));
    assert(!mlxPinnedCharge(1, 3));
    struct Segment { uint64_t address, length; } seg[33] = {};
    for (unsigned i = 0; i < 33; ++i) seg[i] = {i * 8192ull, 4096};
    assert(mlxDmaSegmentsCover(seg, 32, 32 * 4096));
    assert(!mlxDmaSegmentsCover(seg, 33, 33 * 4096));
    assert(!mlxDmaSegmentsCover(seg, 32, 32 * 4096 + 1));
    seg[0].address = UINT64_MAX;
    assert(!mlxDmaSegmentsCover(seg, 1, 4096));
    uint64_t pas[32]; uint32_t produced = 0;
    Segment split[] = {{0x1007, 20}, {0x101b, 4069}, {0x9000, 4096}};
    assert(mlxBuildSegmentPas(split, 3, 0x2007, 8185, pas, 32, &produced));
    assert(produced == 2 && pas[0] == 0x1000 && pas[1] == 0x9000);
    assert(!mlxBuildSegmentPas(split, 3, 0x2007, 8185, pas, 1, &produced));
    split[1].address = 0x501b; /* change translation in the middle of a page */
    assert(!mlxBuildSegmentPas(split, 3, 0x2007, 8185, pas, 32, &produced));
    assert(mlxEqPollPeriodMs(false, false) == 100);
    assert(mlxEqPollPeriodMs(true, false) == 1);
    assert(mlxEqPollPeriodMs(true, true) == 50);
    MlxMetalSlotState slot;
    MlxMetalLease lease{1, 2, 3, 0};
    slot.sequence = 3;
    assert(slot.matches(lease, 1, 2));
    assert(!slot.matches(lease, 2, 2));
    assert(!slot.matches(lease, 1, 3));
    assert(slot.move(MlxMetalPhase::Free, MlxMetalPhase::TxAcquired));
    assert(!slot.move(MlxMetalPhase::TxReady, MlxMetalPhase::Sending));
    assert(slot.move(MlxMetalPhase::TxAcquired, MlxMetalPhase::Producing));
    assert(!slot.move(MlxMetalPhase::TxReady, MlxMetalPhase::Free));
    puts("PASS: fencing masks, quotas, DMA coverage, EQ policy, slot identity");
}
