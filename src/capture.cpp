#include "capture.h"
#include "color_lut.h"
#include "util.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace owc {

namespace {

struct ScanBand {
    int xMin, xMax, yMin, yMax;
};

// Persistent row-parallel CPU scan. Workers are created once and park on a
// condition variable between frames; each frame is split into height bands and
// the per-band bounds are merged on the caller. This spreads the pixel scan
// across cores instead of one thread walking the whole box.
// Threads: min(hardware_concurrency(), rows) unless OWC_SCAN_THREADS overrides.
class ParallelScanner {
public:
    ~ParallelScanner() { shutdown(); }

    void init(const uint8_t* lutPtr, int rows) {
        if (!workers.empty()) return;
        n = (int)std::thread::hardware_concurrency();
        if (n < 1) n = 1;
        if (const char* e = getenv("OWC_SCAN_THREADS")) {
            const int v = std::atoi(e);
            if (v >= 1) n = v;
        }
        n = std::max(1, std::min(n, rows));
        shared.lut = lutPtr;
        shared.rows = rows;
        bands.resize((size_t)n);
        for (int i = 0; i < n; ++i)
            workers.emplace_back(&ParallelScanner::workerLoop, this, i);
    }

    bool active() const { return !workers.empty(); }

    // Called from the grabber thread. Blocks until every worker has finished.
    ScanBand scan(const uint8_t* p, int w, int h, int stride) {
        {
            std::lock_guard<std::mutex> l(m);
            shared.p = p;
            shared.w = w;
            shared.h = h;
            shared.stride = stride;
            ++generation;
        }
        cvAll.notify_all();
        {
            std::unique_lock<std::mutex> l(mDone);
            cvDone.wait(l, [this] { return pending.load(std::memory_order_acquire) == n; });
        }
        pending.store(0, std::memory_order_release);

        ScanBand r{ w, -1, h, -1 };
        for (const ScanBand& b : bands) {
            if (b.xMin < r.xMin) r.xMin = b.xMin;
            if (b.xMax > r.xMax) r.xMax = b.xMax;
            if (b.yMin < r.yMin) r.yMin = b.yMin;
            if (b.yMax > r.yMax) r.yMax = b.yMax;
        }
        return r;
    }

private:
    void shutdown() {
        stop = true;
        { std::lock_guard<std::mutex> l(m); ++generation; }
        cvAll.notify_all();
        for (auto& t : workers)
            if (t.joinable()) t.join();
        workers.clear();
    }

    void workerLoop(int me) {
        int doneGen = 0;
        for (;;) {
            int cur;
            int w, h, stride, rows;
            const uint8_t* p;
            const uint8_t* lut;
            {
                std::unique_lock<std::mutex> l(m);
                cvAll.wait(l, [this, &doneGen] {
                    return stop || generation.load(std::memory_order_acquire) > doneGen;
                });
                if (stop) return;
                cur = generation.load(std::memory_order_acquire);
                p = shared.p;
                w = shared.w;
                h = shared.h;
                stride = shared.stride;
                lut = shared.lut;
                rows = shared.rows;
            }
            const int y0 = (me * rows) / n;
            const int y1 = std::min(((me + 1) * rows) / n, h);
            ScanBand b{ w, -1, h, -1 };
            for (int y = y0; y < y1; ++y) {
                const uint8_t* row = p + (size_t)y * stride;
                for (int x = 0; x < w; ++x) {
                    const uint8_t* px = row + (size_t)x * 4;
                    if (lut[((px[2] >> 3) << 10) | ((px[1] >> 3) << 5) | (px[0] >> 3)]) {
                        if (x < b.xMin) b.xMin = x;
                        if (x > b.xMax) b.xMax = x;
                        if (y < b.yMin) b.yMin = y;
                        if (y > b.yMax) b.yMax = y;
                    }
                }
            }
            bands[(size_t)me] = b;
            doneGen = cur;
            if (pending.fetch_add(1, std::memory_order_acq_rel) == n - 1)
                cvDone.notify_all();
        }
    }

    std::vector<std::thread> workers;
    std::vector<ScanBand> bands;
    int n = 1;
    std::atomic<bool> stop{false};
    std::atomic<int> generation{0};
    std::atomic<int> pending{0};
    std::mutex m, mDone;
    std::condition_variable cvAll, cvDone;
    struct Shared {
        const uint8_t* lut = nullptr;
        const uint8_t* p = nullptr;
        int w = 0, h = 0, stride = 0, rows = 0;
    } shared;
};

class CpuCapture final : public IRegionCapture {
public:
    ~CpuCapture() override { cleanup(); }

    bool setup(const std::vector<uint32_t>& colors, int tolerance,
               int bw, int bh, int ox, int oy) {
        clk.init();
        w = bw;
        h = bh;
        offX = ox;
        offY = oy;
        stride = w * 4; // BGRA32 rows are always 4-byte aligned

        screenDC = CreateDC(TEXT("DISPLAY"), nullptr, nullptr, nullptr);
        if (!screenDC) return false;

        memDC = CreateCompatibleDC(screenDC);
        if (!memDC) {
            cleanup();
            return false;
        }

        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h; // top-down, no row padding
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        bmp = CreateDIBSection(memDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp || !bits) {
            cleanup();
            return false;
        }
        SelectObject(memDC, bmp);

        lut.build(colors.data(), (int)colors.size(), tolerance);
        scanner.init(lut.v, h);
        return true;
    }

    void setFrameIntervalSeconds(double seconds) override { interval = seconds; }

    bool nextFrame(uint64_t& out) override {
        out = 0;
        const int64_t t0 = clk.now();
        if (!BitBlt(memDC, 0, 0, w, h, screenDC, offX, offY, SRCCOPY)) return false;

        ColorLut::Result res;
        if (scanner.active()) {
            const ScanBand b = scanner.scan((const uint8_t*)bits, w, h, stride);
            res.found = b.xMax >= 0;
            res.xMin = b.xMin;
            res.xMax = b.xMax;
            res.yMin = b.yMin;
            res.yMax = b.yMax;
        } else {
            res = lut.scanBGRA((const uint8_t*)bits, w, h, stride);
        }
        out = res.pack();

        // Pace to the requested FPS: coarse Sleep + short pause-spin.
        if (interval > 0.0) {
            const int64_t target = t0 + (int64_t)(interval * 1e9);
            int64_t remain = target - clk.now();
            if (remain > 0) {
                const DWORD ms = (DWORD)(remain / 1000000);
                if (ms > 0) Sleep(ms);
                while (clk.now() < target) _mm_pause();
            }
        }
        return true;
    }

    const char* name() const override { return "GDI BitBlt (DIB) + CPU LUT scan"; }

private:
    void cleanup() {
        if (bmp) { DeleteObject(bmp); bmp = nullptr; }
        if (memDC) { DeleteDC(memDC); memDC = nullptr; }
        if (screenDC) { DeleteDC(screenDC); screenDC = nullptr; }
    }

    HDC screenDC = nullptr;
    HDC memDC = nullptr;
    HBITMAP bmp = nullptr;
    void* bits = nullptr;
    ColorLut lut;
    ParallelScanner scanner;
    int w = 0, h = 0, offX = 0, offY = 0, stride = 0;
    double interval = 0.0;
    Clock clk{};
};

} // namespace

IRegionCapture* createCpuCapture(const std::vector<uint32_t>& colors, int tolerance,
                                 int boxW, int boxH, int offX, int offY) {
    CpuCapture* c = new CpuCapture();
    if (!c->setup(colors, tolerance, boxW, boxH, offX, offY)) {
        delete c;
        return nullptr;
    }
    return c;
}

void destroyCapture(IRegionCapture* c) { delete c; }

} // namespace owc