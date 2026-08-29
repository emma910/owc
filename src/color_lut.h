#pragma once

#include <immintrin.h>
#include <algorithm>
#include <cstdint>

namespace owc {

// Quantized 3D color lookup table: 32^3 = 32768 entries (32 KB, fits the CPU's L1).
//
// The key is built from the top 3 bits of every channel:
//     key = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)
//
// A cube is marked when it intersects the tolerance-expanded box of any target
// color. Because every pixel inside a marked cube is within 7 units of that box,
// this is a *superset* of the exact matcher: it never misses a true match and adds
// at most ~7 units/channel of edge latitude (a win for a "color cheat" anyway).
struct ColorLut {
    uint8_t v[32768];

    void build(const uint32_t* colors, int n, int tolerance) {
        for (int q = 0; q < 32768; ++q) {
            const int rq = (q >> 10) & 31;
            const int gq = (q >> 5) & 31;
            const int bq = q & 31;
            const int r0 = rq * 8, g0 = gq * 8, b0 = bq * 8;
            const int r1 = r0 + 7, g1 = g0 + 7, b1 = b0 + 7;

            bool hit = false;
            for (int i = 0; i < n; ++i) {
                const uint32_t col = colors[i];
                const int cr = (int)((col >> 16) & 0xFF);
                const int cg = (int)((col >> 8) & 0xFF);
                const int cb = (int)(col & 0xFF);
                if (cr - tolerance <= r1 && r0 <= cr + tolerance &&
                    cg - tolerance <= g1 && g0 <= cg + tolerance &&
                    cb - tolerance <= b1 && b0 <= cb + tolerance) {
                    hit = true;
                    break;
                }
            }
            v[q] = hit ? (uint8_t)1 : (uint8_t)0;
        }
    }

    struct Result {
        int xMin = 0, xMax = 0, yMin = 0, yMax = 0;
        bool found = false;

        uint64_t pack() const {
            if (!found) return 0;
            return ((uint64_t)(uint32_t)xMin << 48) |
                   ((uint64_t)(uint32_t)xMax << 32) |
                   ((uint64_t)(uint32_t)yMin << 16) |
                   (uint64_t)(uint32_t)yMax;
        }
    };

    // Scans a top-down BGRA32 buffer (packed, no row padding) for the bounding
    // box of every matching pixel. Prefetches rows ~8 ahead to hide DRAM latency.
    Result scanBGRA(const uint8_t* p, int w, int h, int stride) const {
        Result r;
        r.xMin = w;
        r.xMax = -1;
        r.yMin = h;
        r.yMax = -1;

        for (int y = 0; y < h; ++y) {
            const uint8_t* row = p + (size_t)y * stride;
            const int pf = std::min(y + 8, h - 1);
            _mm_prefetch((const char*)(p + (size_t)pf * stride), _MM_HINT_T0);

            for (int x = 0; x < w; ++x) {
                const uint8_t* px = row + (size_t)x * 4; // B, G, R, A
                const int key = ((px[2] >> 3) << 10) | ((px[1] >> 3) << 5) | (px[0] >> 3);
                if (v[key]) {
                    if (x < r.xMin) r.xMin = x;
                    if (x > r.xMax) r.xMax = x;
                    if (y < r.yMin) r.yMin = y;
                    if (y > r.yMax) r.yMax = y;
                }
            }
        }
        r.found = r.xMax >= 0;
        return r;
    }
};

} // namespace owc