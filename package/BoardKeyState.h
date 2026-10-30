// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

namespace yunluo_keys {
constexpr uint16_t PowerDebounce = 1u << 1;
constexpr uint16_t VolumeUpDebounce = 1u << 3;
constexpr uint16_t DebounceMask = PowerDebounce | VolumeUpDebounce;
constexpr uint16_t OwnedInterrupts = 0x000f;
constexpr uint16_t TopStatus = 0x28, InterruptControl = 0x910;
constexpr uint16_t InterruptSet = 0x912, InterruptStatus = 0x91c;

inline uint16_t ownedInterrupts(uint16_t status) {
    return status & OwnedInterrupts;
}

inline uint8_t reportMask(uint16_t top) {
    return (top & PowerDebounce ? 0 : 2) |
           (top & VolumeUpDebounce ? 0 : 4);
}

struct Edge {
    uint8_t bit;
    bool down;
};

struct PmicState {
    uint8_t mask = 0;
    uint16_t consumed = 0, fresh = 0;

    void acknowledged(uint16_t status) {
        consumed &= ownedInterrupts(status);
    }

    void set(unsigned bit, bool down, Edge *edges, unsigned &count) {
        const uint8_t next = down ? mask | (1u << bit) : mask & ~(1u << bit);
        if (next == mask) return;
        mask = next;
        edges[count++] = {(uint8_t)bit, down};
    }

    // IRQs 48/49 are power/home presses; 50/51 are their releases.
    // Both latches can accumulate between polls. The final debounced level
    // determines their order and also repairs a missed release.
    unsigned update(uint16_t top, uint16_t status, Edge (&edges)[4],
                    bool interruptsEnabled = true) {
        const uint8_t desired = reportMask(top);
        const uint16_t owned = ownedInterrupts(status);
        consumed &= owned;
        fresh = interruptsEnabled ? owned & ~consumed : 0;
        consumed |= owned;
        const uint16_t irq = fresh;
        unsigned count = 0;
        for (unsigned key = 0; key < 2; ++key) {
            const unsigned bit = key + 1;
            const bool down = (desired & (1u << bit)) != 0;
            const bool press = (irq & (1u << key)) != 0;
            const bool release = (irq & (1u << (key + 2))) != 0;
            if (press && release) {
                set(bit, !down, edges, count);
                set(bit, down, edges, count);
            } else {
                if (press) set(bit, true, edges, count);
                if (release) set(bit, false, edges, count);
                set(bit, down, edges, count);
            }
        }
        return count;
    }
};
}
