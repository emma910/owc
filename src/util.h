#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <cstdint>

#define OWC_CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))

namespace owc {

// Branch-free integer absolute value (same as the original FastAbs).
inline int fastAbs(int v) {
    const int m = v >> 31;
    return (v ^ m) - m;
}
inline int64_t fastAbs(int64_t v) {
    const int64_t m = v >> 63;
    return (v ^ m) - m;
}

// QueryPerformanceCounter based nanosecond clock (QPC is the canonical
// high-resolution timer; 100ns resolution guaranteed, far better on real HW).
struct Clock {
    double invFreqNanos = 0.0;
    LARGE_INTEGER base{};

    void init() {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        invFreqNanos = 1e9 / (double)f.QuadPart;
        QueryPerformanceCounter(&base);
    }
    inline int64_t now() const {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return (int64_t)((double)(c.QuadPart - base.QuadPart) * invFreqNanos);
    }
};

// Three precise-sleeper strategies mirrored from the original enum:
//   0 = YIELD      (SwitchToThread, low jitter, low-moderate CPU)
//   1 = SPIN_WAIT  (_mm_pause, lowest jitter, highest CPU)
//   2 = SLEEP      (Sleep(0), high jitter, lowest CPU)
struct PreciseSleeper {
    int type;
    explicit PreciseSleeper(int t = 0) : type(OWC_CLAMP(t, 0, 2)) {}

    void waitOnce() const {
        switch (type) {
        case 1: _mm_pause(); break;
        case 2: Sleep(0); break;
        default: SwitchToThread(); break;
        }
    }
    void sleepNanos(int64_t ns, const Clock& clk) const {
        const int64_t start = clk.now();
        while (clk.now() - start < ns) waitOnce();
    }
};

// Fast non-cryptographic xorshift64* PRNG, mirroring the original FastRandom
// (xorshift64 that multiplies by 2685821657736338717).
struct XorShift64 {
    uint64_t st;

    XorShift64() {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        st = ((uint64_t)c.QuadPart ^ (uint64_t)__rdtsc()) | 1ULL;
    }

    inline uint64_t nextRaw() {
        st ^= st >> 12;
        st ^= st << 25;
        st ^= st >> 27;
        st *= 2685821657736338717ULL;
        return st;
    }

    // Mirrors Java FastAbs(x) / Long.MAX_VALUE as double.
    static inline double toUnit(uint64_t v) {
        int64_t s = (int64_t)v;
        const int64_t m = s >> 63;
        return (double)fastAbs(s) / 9223372036854775807.0;
    }

    int nextInt(int max) { return (max <= 0) ? 0 : (int)((double)max * toUnit(nextRaw())); }
    float nextFloat01() { return (float)toUnit(nextRaw()); }
};

} // namespace owc