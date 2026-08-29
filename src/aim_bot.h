#pragma once

#include <cstdint>

#include "config.h"
#include "native.h"
#include "state.h"
#include "util.h"

namespace owc {

// Faithful port of the original AimBotThread: keyboard gating, delta math,
// jitter, flick shots and the adaptive precise sleep timing loop.
struct AimBot {
    const Config& cfg;
    IInput& input;
    Shared& shared;
    Clock clk;
    PreciseSleeper sleeper;
    XorShift64 rng;

    int captureCenterX = 0, captureCenterY = 0;
    int maxSnapX = 0, maxSnapY = 0;
    float aimDurationNanos = 0;      // millis * 1e6 as Float (mirrors Kotlin)
    int64_t flickPauseNanos = 0;
    bool flicking = false;

    AimBot(const Config& c, IInput& in, Shared& s);

    void run();

private:
    void useAimData(uint64_t aim);
    int calculateDelta(uint64_t aim, int shiftBits, int minSize, float offset, int subtrahend) const;
    void performAim(int dX, int dY);
};

} // namespace owc