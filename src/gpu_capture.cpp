#include "capture.h"
#include "util.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>

#include <cstdio>
#include <cstring>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace owc {

namespace {

// ---------------------------------------------------------------------------
// Compute shader: per-pixel color match with block-local atomic reduction.
//   Result[0..3] = { xMin, xMax, yMin, yMax }  (InterlockedMin/Max)
//   Result[4]    = match counter
// A dedicated 1-thread ResetMain dispatch re-seeds element 0..4 every frame so
// the scan needs zero ClearUAV / CPU staging writes in the hot path.
// ---------------------------------------------------------------------------
static const char* kShaderCS = R"(
cbuffer Params : register(b0)
{
    uint2 RegionSize;
    uint ColorCount;
    float Tolerance;
    float4 QuickLo;
    float4 QuickHi;
};
StructuredBuffer<float4> Colors : register(t0);
Texture2D<float4> Region   : register(t1);
RWBuffer<uint> Result       : register(u0); // [0]=xMin [1]=xMax [2]=yMin [3]=yMax [4]=count

groupshared int gxMin; groupshared int gxMax;
groupshared int gyMin; groupshared int gyMax;

[numthreads(16,16,1)]
void ScanMain(uint3 id : SV_DispatchThreadID, uint3 tid : SV_GroupThreadID)
{
    if (tid.x == 0 && tid.y == 0)
    {
        gxMin = 0x7fffffff; gxMax = -1;
        gyMin = 0x7fffffff; gyMax = -1;
    }
    GroupMemoryBarrierWithGroupSync();

    if (id.x < RegionSize.x && id.y < RegionSize.y)
    {
        // UNORM view over raw bytes => floats are exactly byte/255, no sRGB math.
        float4 px = Region.Load(int3(id.xy, 0));

        // Cheap 3-comparison coarse filter before the color loop.
        bool ok = (px.r >= QuickLo.x && px.r <= QuickHi.x &&
                   px.g >= QuickLo.y && px.g <= QuickHi.y &&
                   px.b >= QuickLo.z && px.b <= QuickHi.z);
        if (ok)
        {
            bool matched = false;
            [loop]
            for (uint i = 0; i < ColorCount; ++i)
            {
                float4 c = Colors[i];
                float dx = (px.r > c.x) ? (px.r - c.x) : (c.x - px.r);
                float dy = (px.g > c.y) ? (px.g - c.y) : (c.y - px.g);
                float dz = (px.b > c.z) ? (px.b - c.z) : (c.z - px.b);
                if (dx <= Tolerance && dy <= Tolerance && dz <= Tolerance)
                {
                    matched = true;
                    break;
                }
            }
            if (matched)
            {
                InterlockedMin(gxMin, (int)id.x);
                InterlockedMax(gxMax, (int)id.x);
                InterlockedMin(gyMin, (int)id.y);
                InterlockedMax(gyMax, (int)id.y);
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (tid.x == 0 && tid.y == 0)
    {
        if (gxMin <= gxMax) // block found at least one match
        {
            InterlockedMin(Result[0], (uint)gxMin);
            InterlockedMax(Result[1], (uint)gxMax);
            InterlockedMin(Result[2], (uint)gyMin);
            InterlockedMax(Result[3], (uint)gyMax);
            InterlockedAdd(Result[4], 1u);
        }
    }
}

[numthreads(1,1,1)]
void ResetMain()
{
    Result[0] = RegionSize.x; // sentinels: min > max range, and vice versa
    Result[1] = 0u;
    Result[2] = RegionSize.y;
    Result[3] = 0u;
    Result[4] = 0u;
}
)";

bool supportedFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return true;
    default:
        return false;
    }
}

// Must match the HLSL cbuffer layout exactly.
struct alignas(16) GpuParams {
    uint32_t regionW, regionH;
    uint32_t colorCount;
    float tolerance;
    float quickLo[4];
    float quickHi[4];
};

} // namespace

// ---------------------------------------------------------------------------
class GpuCapture final : public IRegionCapture {
public:
    ~GpuCapture() override { shutdown(); }

    bool init(const std::vector<uint32_t>& colors, int tolerance,
              int bw, int bh, int ox, int oy) {
        clk.init();
        boxW = bw;
        boxH = bh;
        offX = ox;
        offY = oy;

        D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        ID3D11Device* dev0 = nullptr;
        ID3D11DeviceContext* ctx0 = nullptr;
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                       levels, 2, D3D11_SDK_VERSION,
                                       &dev0, &fl, &ctx0);
        if (FAILED(hr)) {
            fprintf(stderr, "[gpu] D3D11 device creation failed (0x%08lX), falling back to CPU\n", (unsigned long)hr);
            return false;
        }
        dev = dev0;
        ctx = ctx0;

        if (!createResources(colors, tolerance)) return false;
        if (!createDuplication()) return false;

        // Probe the duplicated desktop to establish its format up front.
        probeFormat();

        return true;
    }

    void shutdown() {
        if (ctx) ctx->ClearState();
        if (dup) { dup->Release(); dup = nullptr; }
        if (regionSRV) { regionSRV->Release(); regionSRV = nullptr; }
        if (regionTex) { regionTex->Release(); regionTex = nullptr; }
        if (colorsSRV) { colorsSRV->Release(); colorsSRV = nullptr; }
        if (colorsBuf) { colorsBuf->Release(); colorsBuf = nullptr; }
        if (paramsCB) { paramsCB->Release(); paramsCB = nullptr; }
        if (resetCS) { resetCS->Release(); resetCS = nullptr; }
        if (scanCS) { scanCS->Release(); scanCS = nullptr; }
        if (resultStaging) { resultStaging->Release(); resultStaging = nullptr; }
        if (resultUAV) { resultUAV->Release(); resultUAV = nullptr; }
        if (resultBuf) { resultBuf->Release(); resultBuf = nullptr; }
        if (ctx) { ctx->Release(); ctx = nullptr; }
        if (dev) { dev->Release(); dev = nullptr; }
    }

    void setFrameIntervalSeconds(double seconds) override { interval = seconds; }

    bool nextFrame(uint64_t& out) override {
        out = 0;

        if (!dup) return false;

        // Poll the duplication for a short bounded time; pacing is enforced by
        // the post-processing sleep below, so a static desktop (no presents)
        // cannot spin the grabber or overshoot the FPS target.
        DXGI_OUTDUPL_FRAME_INFO fi{};
        IDXGIResource* res = nullptr;
        HRESULT hr = dup->AcquireNextFrame(2, &fi, &res);
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            recreateDuplication();
            return false;
        }
        if (FAILED(hr) || !res) return false;

        ID3D11Texture2D* desktopTex = nullptr;
        if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&desktopTex))) {
            res->Release();
            return false;
        }

        // Lazily create the region texture with the desktop's actual format.
        if (!regionTex) {
            D3D11_TEXTURE2D_DESC dd{};
            desktopTex->GetDesc(&dd);
            if (!ensureRegion(dd)) {
                fatal = true;
                desktopTex->Release();
                res->Release();
                dup->ReleaseFrame();
                fprintf(stderr, "[gpu] unsupported desktop format, capture disabled\n");
                return false;
            }
        }

        bool fresh = (fi.AccumulatedFrames != 0) || (fi.LastPresentTime.QuadPart != 0);
        if (fresh) {
            D3D11_BOX box{};
            box.left = (UINT)offX;
            box.top = (UINT)offY;
            box.right = (UINT)(offX + boxW);
            box.bottom = (UINT)(offY + boxH);
            box.front = 0;
            box.back = 1;
            // GPU -> GPU region copy (the full desktop never touches the CPU).
            ctx->CopySubresourceRegion(regionTex, 0, 0, 0, 0, desktopTex, 0, &box);
            out = scanGpu();
            ++scannedFrames;
        }

        desktopTex->Release();
        res->Release();
        dup->ReleaseFrame();

        // Enforce the FPS target (Desktop Duplication presents far faster).
        pace();

        return fresh;
    }

    void pace() {
        const int64_t intervalNs = (int64_t)(interval * 1e9);
        const int64_t now = clk.now();
        if (lastRequest) {
            const int64_t leftNs = intervalNs - (now - lastRequest);
            if (leftNs > 0) {
                ::Sleep((DWORD)(leftNs / 1000000));
                // Busy-wait the sub-ms remainder so the Sleep quantum cannot
                // let the grabber overshoot the FPS target.
                const int64_t t0 = clk.now();
                while (clk.now() - t0 < (leftNs % 1000000)) _mm_pause();
            }
        }
        lastRequest = clk.now();
    }

    bool ok() const { return !fatal; }

    const char* name() const override { return "DXGI Desktop Duplication + D3D11 compute scan"; }

private:
    void probeFormat() {
        DXGI_OUTDUPL_FRAME_INFO fi{};
        IDXGIResource* res = nullptr;
        HRESULT hr = dup->AcquireNextFrame(200, &fi, &res);
        if (FAILED(hr) || !res) return;
        ID3D11Texture2D* t = nullptr;
        if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t))) {
            D3D11_TEXTURE2D_DESC dd{};
            t->GetDesc(&dd);
            if (!supportedFormat(dd.Format)) fatal = true;
            else if (!regionTex) ensureRegion(dd);
            t->Release();
        }
        res->Release();
        dup->ReleaseFrame();
    }

    bool ensureRegion(const D3D11_TEXTURE2D_DESC& dd) {
        if (!supportedFormat(dd.Format)) return false;

        // Create the region in a plain non-sRGB UNORM format. The desktop may be
        // _UNORM/_SRGB/_TYPELESS; _SRGB variants are copy-compatible with their
        // _UNORM sibling (no gamma conversion on copy), and a non-sRGB view keeps
        // Load() returning exact byte values (byte/255).
        const bool bgraFamily = (dd.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                                 dd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                                 dd.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS);
        const DXGI_FORMAT regionFmt = bgraFamily ? DXGI_FORMAT_B8G8R8A8_UNORM
                                                 : DXGI_FORMAT_R8G8B8A8_UNORM;

        D3D11_TEXTURE2D_DESC r{};
        r.Width = (UINT)boxW;
        r.Height = (UINT)boxH;
        r.MipLevels = 1;
        r.ArraySize = 1;
        r.Format = regionFmt;
        r.SampleDesc.Count = 1;
        r.Usage = D3D11_USAGE_DEFAULT;
        r.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&r, nullptr, &regionTex))) return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = regionFmt;
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        if (FAILED(dev->CreateShaderResourceView(regionTex, &srv, &regionSRV))) return false;
        return true;
    }

    bool createResources(const std::vector<uint32_t>& colors, int tolerance) {
        // --- compile shaders at runtime (no build-time .cso dependency) ---
        ID3DBlob* code = nullptr;
        ID3DBlob* err = nullptr;
        HRESULT hr = D3DCompile(kShaderCS, strlen(kShaderCS), "OverwatcheatCS",
                                nullptr, nullptr, "ScanMain", "cs_5_0", 0, 0, &code, &err);
        if (FAILED(hr)) {
            if (err) fprintf(stderr, "[gpu] scan shader: %.*s\n",
                             (int)err->GetBufferSize(), (const char*)err->GetBufferPointer());
            return false;
        }
        hr = dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &scanCS);
        code->Release();
        if (FAILED(hr)) return false;

        code = nullptr;
        err = nullptr;
        hr = D3DCompile(kShaderCS, strlen(kShaderCS), "OverwatcheatCS",
                        nullptr, nullptr, "ResetMain", "cs_5_0", 0, 0, &code, &err);
        if (FAILED(hr)) return false;
        hr = dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &resetCS);
        code->Release();
        if (FAILED(hr)) return false;

        // --- constant buffer (matches Params) ---
        GpuParams params{};
        params.regionW = (uint32_t)boxW;
        params.regionH = (uint32_t)boxH;
        params.colorCount = (uint32_t)colors.size();
        params.tolerance = (float)tolerance / 255.0f;

        uint32_t rLo = 255, gLo = 255, bLo = 255;
        uint32_t rHi = 0, gHi = 0, bHi = 0;
        for (uint32_t c : colors) {
            const uint32_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
            rLo = r < rLo ? r : rLo; gLo = g < gLo ? g : gLo; bLo = b < bLo ? b : bLo;
            rHi = r > rHi ? r : rHi; gHi = g > gHi ? g : gHi; bHi = b > bHi ? b : bHi;
        }
        const uint32_t t = (uint32_t)tolerance;
        params.quickLo[0] = (float)((rLo > t) ? rLo - t : 0) / 255.0f;
        params.quickLo[1] = (float)((gLo > t) ? gLo - t : 0) / 255.0f;
        params.quickLo[2] = (float)((bLo > t) ? bLo - t : 0) / 255.0f;
        params.quickLo[3] = 0.0f;
        params.quickHi[0] = OWC_CLAMP(rHi + t, 0u, 255u) / 255.0f;
        params.quickHi[1] = OWC_CLAMP(gHi + t, 0u, 255u) / 255.0f;
        params.quickHi[2] = OWC_CLAMP(bHi + t, 0u, 255u) / 255.0f;
        params.quickHi[3] = 1.0f;

        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = sizeof(GpuParams);
        cb.Usage = D3D11_USAGE_DEFAULT;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(dev->CreateBuffer(&cb, nullptr, &paramsCB))) return false;
        ctx->UpdateSubresource(paramsCB, 0, nullptr, &params, 0, 0);

        // --- structured color list (normalized floats, immutable) ---
        const size_t n = colors.size();
        if (n) {
            std::vector<float> cf(n * 4);
            for (size_t i = 0; i < n; ++i) {
                cf[i * 4 + 0] = ((colors[i] >> 16) & 0xFF) / 255.0f;
                cf[i * 4 + 1] = ((colors[i] >> 8) & 0xFF) / 255.0f;
                cf[i * 4 + 2] = (colors[i] & 0xFF) / 255.0f;
                cf[i * 4 + 3] = 0.0f;
            }
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = (UINT)(n * 16);
            bd.Usage = D3D11_USAGE_IMMUTABLE;
            bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            bd.StructureByteStride = 16;
            D3D11_SUBRESOURCE_DATA sd{};
            sd.pSysMem = cf.data();
            if (FAILED(dev->CreateBuffer(&bd, &sd, &colorsBuf))) return false;

            D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = DXGI_FORMAT_UNKNOWN;
            srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            srv.Buffer.FirstElement = 0;
            srv.Buffer.NumElements = (UINT)n;
            if (FAILED(dev->CreateShaderResourceView(colorsBuf, &srv, &colorsSRV))) return false;
        }

        // --- result buffer: R32_UINT typed, 5 elements ---
        D3D11_BUFFER_DESC rb{};
        rb.ByteWidth = 5 * sizeof(uint32_t);
        rb.Usage = D3D11_USAGE_DEFAULT;
        rb.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(dev->CreateBuffer(&rb, nullptr, &resultBuf))) return false;

        D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32_UINT;
        uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uav.Buffer.FirstElement = 0;
        uav.Buffer.NumElements = 5;
        if (FAILED(dev->CreateUnorderedAccessView(resultBuf, &uav, &resultUAV))) return false;

        D3D11_BUFFER_DESC sb{};
        sb.ByteWidth = 5 * sizeof(uint32_t);
        sb.Usage = D3D11_USAGE_STAGING;
        sb.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateBuffer(&sb, nullptr, &resultStaging))) return false;

        return true;
    }

    bool createDuplication() {
        const auto freeChain = [](IDXGIFactory1* f, IDXGIAdapter1* a, IDXGIOutput* o, IDXGIOutput1* o1) {
            if (o1) o1->Release();
            if (o) o->Release();
            if (a) a->Release();
            if (f) f->Release();
        };

        IDXGIFactory1* factory = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) return false;

        IDXGIAdapter1* adapter = nullptr;
        if (FAILED(factory->EnumAdapters1(0, &adapter))) { freeChain(factory, adapter, nullptr, nullptr); return false; }

        IDXGIOutput* output = nullptr;
        if (FAILED(adapter->EnumOutputs(0, &output))) { freeChain(factory, adapter, output, nullptr); return false; }

        DXGI_OUTPUT_DESC od{};
        output->GetDesc(&od);
        desktopW = od.DesktopCoordinates.right - od.DesktopCoordinates.left;
        desktopH = od.DesktopCoordinates.bottom - od.DesktopCoordinates.top;

        IDXGIOutput1* output1 = nullptr;
        if (FAILED(output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1))) {
            freeChain(factory, adapter, output, output1);
            return false;
        }

        // Keep the output interface for cheap recreation on ACCESS_LOST.
        out1 = output1;
        output1->AddRef();

        if (FAILED(output1->DuplicateOutput(dev, &dup))) {
            out1->Release();
            out1 = nullptr;
            freeChain(factory, adapter, output, nullptr);
            return false;
        }

        freeChain(factory, adapter, output, output1);
        if (offX < 0) offX = 0;
        if (offY < 0) offY = 0;
        if (offX + boxW > desktopW) boxW = desktopW - offX;
        if (offY + boxH > desktopH) boxH = desktopH - offY;
        if (boxW <= 0 || boxH <= 0) return false;
        return true;
    }

    void recreateDuplication() {
        if (dup) { dup->Release(); dup = nullptr; }
        if (!out1) return;
        IDXGIFactory1* factory = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) return;

        IDXGIAdapter1* adapter = nullptr;
        if (SUCCEEDED(factory->EnumAdapters1(0, &adapter))) {
            IDXGIOutput* output = nullptr;
            if (SUCCEEDED(adapter->EnumOutputs(0, &output))) {
                IDXGIOutput1* o1 = nullptr;
                if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&o1))) {
                    if (FAILED(o1->DuplicateOutput(dev, &dup))) dup = nullptr;
                    else {
                        out1->Release();
                        out1 = o1;
                        out1->AddRef();
                    }
                    o1->Release();
                }
                output->Release();
            }
            adapter->Release();
        }
        factory->Release();
    }

    uint64_t scanGpu() {
        ID3D11ShaderResourceView* srvs[2] = { colorsSRV, regionSRV };
        ctx->CSSetShaderResources(0, 2, srvs);
        ctx->CSSetConstantBuffers(0, 1, &paramsCB);

        ID3D11UnorderedAccessView* uav = resultUAV;
        ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        ctx->CSSetShader(resetCS, nullptr, 0);
        ctx->Dispatch(1, 1, 1); // deterministic re-seed, ordered before the scan

        ctx->CSSetShader(scanCS, nullptr, 0);
        const UINT gx = ((UINT)boxW + 15) / 16;
        const UINT gy = ((UINT)boxH + 15) / 16;
        ctx->Dispatch(gx, gy, 1);

        ID3D11UnorderedAccessView* nullUav = nullptr;
        ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr); // unbind before copy

        ctx->CopyResource(resultStaging, resultBuf);

        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(resultStaging, 0, D3D11_MAP_READ, 0, &m))) return 0;
        const uint32_t* p = (const uint32_t*)m.pData;
        const uint32_t xMin = p[0], xMax = p[1], yMin = p[2], yMax = p[3], count = p[4];
        ctx->Unmap(resultStaging, 0);

        if (count == 0) return 0;
        return ((uint64_t)xMin << 48) |
               ((uint64_t)xMax << 32) |
               ((uint64_t)yMin << 16) |
               (uint64_t)yMax;
    }

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGIOutputDuplication* dup = nullptr;
    IDXGIOutput1* out1 = nullptr;
    ID3D11ComputeShader* scanCS = nullptr;
    ID3D11ComputeShader* resetCS = nullptr;
    ID3D11Buffer* paramsCB = nullptr;
    ID3D11Buffer* colorsBuf = nullptr;
    ID3D11ShaderResourceView* colorsSRV = nullptr;
    ID3D11Texture2D* regionTex = nullptr;
    ID3D11ShaderResourceView* regionSRV = nullptr;
    ID3D11Buffer* resultBuf = nullptr;
    ID3D11UnorderedAccessView* resultUAV = nullptr;
    ID3D11Buffer* resultStaging = nullptr;

    int boxW = 0, boxH = 0, offX = 0, offY = 0;
    int desktopW = 0, desktopH = 0;
    double interval = 1.0 / 30.0;
    Clock clk{};
    int64_t lastRequest = 0;
    uint64_t scannedFrames = 0;
    bool fatal = false;
};

IRegionCapture* createGpuCapture(const std::vector<uint32_t>& colors, int tolerance,
                                 int boxW, int boxH, int offX, int offY) {
    GpuCapture* g = new GpuCapture();
    if (!g->init(colors, tolerance, boxW, boxH, offX, offY)) {
        delete g;
        return nullptr;
    }
    return g;
}

} // namespace owc