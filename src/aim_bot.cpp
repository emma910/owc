#include "aim_bot.h"

#include <algorithm>

namespace owc {

AimBot::AimBot(const Config& c, IInput& in, Shared& s)
    : cfg(c), input(in), shared(s), sleeper(c.aim_precise_sleeper_type) {
    clk.init();
    aimDurationNanos = c.aim_duration_millis * 1e6f;              // Float semantics
    flickPauseNanos = c.flick_pause_duration_ms * 1000000LL;      // ms -> ns
}

void AimBot::run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    const int ai = cfg.aim_cpu_thread_affinity_index;
    if (ai >= 0) {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        if ((DWORD)ai < si.dwNumberOfProcessors)
            SetThreadAffinityMask(GetCurrentThread(), 1ULL << ai);
    }

    bool wasPressed = false;
    while (shared.running.load(std::memory_order_relaxed)) {
        const int64_t t0 = clk.now();

        const bool pressed = globalKeyDown(cfg.aim_key);
        if (cfg.toggle_in_game_ui && wasPressed != pressed)
            shared.toggleUI.store(true, std::memory_order_relaxed);
        wasPressed = pressed;

        if (pressed) {
            if (cfg.aim_mode == 1) flicking = true; // FLICKING
            useAimData(shared.aimData.load(std::memory_order_acquire));
        } else {
            shared.aimData.store(0, std::memory_order_release);
        }

        if (pressed) {
            // Adaptive pacing identical to the original while aiming:
            //   sleepTime = aimDuration * max(multMax, multBase + rand01) - elapsed
            const int64_t elapsed = clk.now() - t0;
            const float mult = std::max(cfg.aim_duration_multiplier_max,
                                        cfg.aim_duration_multiplier_base + rng.nextFloat01());
            const int64_t sleepTime = (int64_t)(aimDurationNanos * mult) - elapsed;
            if (sleepTime > 100000) { // >100us
                if (sleeper.type == 1 && sleepTime > 1500000) {
                    // SPIN_WAIT hybrid: bulk Sleep keeps the core nearly free,
                    // a short 0.5ms _mm_pause tail preserves sub-ms precision.
                    ::Sleep((DWORD)((sleepTime - 500000) / 1000000));
                    sleeper.sleepNanos(500000, clk);
                } else {
                    sleeper.sleepNanos(sleepTime, clk);
                }
            }
        } else {
            // Idle: never spin a core while the aim key is released. The precise
            // SPIN_WAIT pacing only matters during active aiming; a 1ms coarse
            // sleep keeps key-down latency <= ~1ms at near-zero CPU.
            ::Sleep(1);
        }
    }
}

void AimBot::useAimData(uint64_t aim) {
    if (aim == 0) return;

    const int dX = calculateDelta(aim, 48, cfg.aim_min_target_width, cfg.aim_offset_x, captureCenterX);
    const int dY = calculateDelta(aim, 16, cfg.aim_min_target_height, cfg.aim_offset_y, captureCenterY);
    performAim(dX, dY);
}

int AimBot::calculateDelta(uint64_t aim, int shiftBits, int minSize, float offset, int subtrahend) const {
    const int64_t low = (aim >> shiftBits) & 0xFFFF;
    const int64_t high = (aim >> (shiftBits - 16)) & 0xFFFF;
    const int64_t size = high - low;
    if (size < minSize) return INT32_MAX;

    // size / 2 * offset  (Long division, Long * Float -> Float, truncating toInt)
    const float half = (float)(size >> 1);
    const float offsetVal = half * offset;
    const float aimPos = (float)low + offsetVal;
    return (int)aimPos - subtrahend;
}

void AimBot::performAim(int dX, int dY) {
    if (fastAbs(dX) > maxSnapX || fastAbs(dY) > maxSnapY) return;

    const float jitter = (float)rng.nextInt(cfg.aim_jitter_percent) / 100.0f;
    const float rndMult = 1.0f - jitter;

    int mX = (int)((float)dX / cfg.sensitivity * rndMult);
    int mY = (int)((float)dY / cfg.sensitivity * rndMult);
    mX = std::min(cfg.aim_max_move_pixels, mX);
    mY = std::min(cfg.aim_max_move_pixels, mY);

    input.move(mX, mY);

    if (flicking && fastAbs(mX) < cfg.flick_shoot_pixels && fastAbs(mY) < cfg.flick_shoot_pixels) {
        flicking = false;
        input.click();
        sleeper.sleepNanos(flickPauseNanos, clk);
    }
}

} // namespace owc