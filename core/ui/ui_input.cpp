// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/ui_input.h"

#include <algorithm>
#include <cmath>

namespace splat {

uint32_t UiRepeat::update(uint32_t pressed, uint32_t held, double dt) {
    if (!(dt > 0.0) || !std::isfinite(dt)) dt = 0.0;
    uint32_t repeats = 0;
    for (int index = 0; index < 4; ++index) {
        const uint32_t bit = kDirections[index];
        Key& key = keys_[index];
        if ((held & bit) == 0) {
            key = Key{};
            continue;
        }
        if ((pressed & bit) != 0) {
            // The caller's edge is this step; the delay starts now.
            key.down = true;
            key.held = 0.0;
            key.next = kRepeatDelaySeconds;
            continue;
        }
        if (!key.down) {
            // Down without an edge (the panel opened under a held stick): start
            // the delay rather than stepping at once.
            key.down = true;
            key.held = 0.0;
            key.next = kRepeatDelaySeconds;
            continue;
        }
        key.held += dt;
        if (key.held + 1.0e-9 < key.next) continue;
        repeats |= bit;
        // At most one repeat per direction per frame: a long hitch must not
        // turn into a burst of steps.
        key.next = key.held + interval_for_held(key.held);
    }
    return repeats;
}

void UiRepeat::reset() {
    for (Key& key : keys_) key = Key{};
}

double UiRepeat::interval_for_held(double held_seconds) {
    const double after = std::max(0.0, held_seconds - kRepeatDelaySeconds);
    const double ramp = std::min(1.0, after / kRepeatRampSeconds);
    const double rate = kRepeatStartHz + (kRepeatEndHz - kRepeatStartHz) * ramp;
    return rate > 0.0 ? 1.0 / rate : 0.05;
}

}  // namespace splat
