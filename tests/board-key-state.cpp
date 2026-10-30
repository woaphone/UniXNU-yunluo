// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../package/BoardKeyState.h"
#include <cassert>
#include <cstdio>
using namespace yunluo_keys;
int main() {
    assert(reportMask(PowerDebounce | VolumeUpDebounce) == 0);
    assert(reportMask(VolumeUpDebounce) == 2);
    assert(reportMask(PowerDebounce) == 4);
    for (unsigned initial = 0; initial < 4; ++initial)
    for (unsigned levels = 0; levels < 4; ++levels)
    for (unsigned status = 0; status < 16; ++status)
    for (unsigned consumed = 0; consumed < 16; ++consumed) {
        PmicState state;
        state.mask = initial << 1;
        state.consumed = consumed;
        const uint16_t top = (levels & 1 ? PowerDebounce : 0) |
                             (levels & 2 ? VolumeUpDebounce : 0);
        Edge edges[4];
        const auto n = state.update(top, status | 0xfff0, edges);
        assert(n <= 4 && state.mask == reportMask(top));
        assert(state.consumed == (status & OwnedInterrupts));
        uint8_t observed = initial << 1;
        for (unsigned i = 0; i < n; ++i) {
            assert(edges[i].bit == 1 || edges[i].bit == 2);
            const unsigned bit = 1u << edges[i].bit;
            assert(bool(observed & bit) != edges[i].down);
            observed = edges[i].down ? observed | bit : observed & ~bit;
        }
        assert(observed == state.mask);
        assert(state.update(top, status, edges) == 0);
        state.acknowledged(0);
        assert(state.consumed == 0);
    }
    PmicState polling;
    Edge edges[4];
    assert(polling.update(0, 0xffff, edges, false) == 2);
    assert(polling.mask == 6 && polling.fresh == 0);
    assert(polling.update(DebounceMask, 0, edges, false) == 2);
    assert(polling.mask == 0);
    std::puts("PASS: 4096 PMIC state combinations plus polling; register model only, no hardware");
}
