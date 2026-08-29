#pragma once

#include <cstdint>
#include <vector>

namespace owc {

// Common capture interface. Implementations produce a packed aim-data value
// exactly like the original: ((xLow << 48) | (xHigh << 32) | (yLow << 16) | yHigh),
// or 0 when nothing matched.
struct IRegionCapture {
    virtual ~IRegionCapture() = default;

    // Grabs the next fresh frame and scans it in place. Returns true when a fresh
    // frame was processed (out then holds the packed bounds); returns false when
    // no new frame arrived (timeout). Implementations pace themselves to the
    // configured frame interval so the CPU sleeps instead of busy-polling.
    virtual bool nextFrame(uint64_t& out) = 0;
    virtual void setFrameIntervalSeconds(double seconds) = 0;
    virtual const char* name() const = 0;
};

// Fast path: DXGI Desktop Duplication + D3D11 compute-shader scanning.
// The frame never leaves the GPU; only a 20-byte bounding-box result is read back.
IRegionCapture* createGpuCapture(const std::vector<uint32_t>& colors, int tolerance,
                                 int boxW, int boxH, int offX, int offY);

// Fallback: GDI BitBlt of the region straight into an aligned DIB section,
// then a CPU LUT scan (L1-resident table + prefetch).
IRegionCapture* createCpuCapture(const std::vector<uint32_t>& colors, int tolerance,
                                 int boxW, int boxH, int offX, int offY);

void destroyCapture(IRegionCapture* c);

} // namespace owc