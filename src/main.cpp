#include "aim_bot.h"
#include "capture.h"
#include "config.h"
#include "native.h"
#include "state.h"
#include "util.h"

#include <windows.h>
#include <mmsystem.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

int main(); // global entry

namespace owc {

namespace {

Shared* g_shared = nullptr;

BOOL WINAPI ctrlHandler(DWORD) {
    if (g_shared) g_shared->running.store(false, std::memory_order_relaxed);
    return TRUE;
}

void captureThreadBody(IRegionCapture* cap, Shared* shared, const Config& cfg) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    cap->setFrameIntervalSeconds(1.0 / std::max(cfg.fps, 1.0f));

    while (shared->running.load(std::memory_order_relaxed)) {
        uint64_t packed = 0;
        if (cap->nextFrame(packed))
            shared->aimData.store(packed, std::memory_order_release);
    }
    printf("  [grabber] stopped\n");
}

void aimThreadBody(AimBot* bot, Shared* shared) {
    bot->run();
    printf("  [aimbot] stopped\n");
}

void toggleThreadBody(const Config& cfg, IInput* input, Shared* shared) {
    // Precompute scan codes (VK -> scan), exactly like the original.
    std::vector<int> scanCodes;
    for (uint32_t vk : cfg.toggle_key_codes)
        scanCodes.push_back(vkToScan((int)vk));

    while (shared->running.load(std::memory_order_relaxed)) {
        if (shared->toggleUI.exchange(false)) {
            for (int sc : scanCodes) input->keyDown(sc);
            Sleep(1);
            for (auto it = scanCodes.rbegin(); it != scanCodes.rend(); ++it)
                input->keyUp(*it);
        } else {
            Sleep(1);
        }
    }
    printf("  [toggleui] stopped\n");
}

} // namespace

int runMain() {
    SetConsoleTitleA("Discord");
    if (!SetConsoleCtrlHandler(ctrlHandler, TRUE)) {
        fprintf(stderr, "[main] could not install Ctrl+C handler\n");
    }

    // DPI awareness: physical-pixel coordinates so capture math is exact.
    typedef HANDLE DpiContext;
    typedef BOOL(WINAPI* SetDpiFn)(DpiContext);
    if (HMODULE user32 = GetModuleHandleA("user32.dll")) {
        if (SetDpiFn fn = (SetDpiFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext"))
            fn((DpiContext)(INT_PTR)-4); // PER_MONITOR_AWARE_V2
        else
            SetProcessDPIAware();
    }

    // 1ms timer resolution for Sleep(1) precision (classic low-latency tweak).
    timeBeginPeriod(1);

    const Config cfg = loadConfig("owc.cfg");

    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    printf("  aim_key          : %d\n", cfg.aim_key);
    printf("  aim_mode         : %s\n", cfg.aim_mode == 1 ? "FLICKING" : "TRACKING");
    printf("  fps cap          : %.1f\n", cfg.fps);
    printf("  sensitivity      : %.1f\n", cfg.sensitivity);
    printf("  target colors    : %zu base +-%d tolerance\n", cfg.target_colors.size(), cfg.target_color_tolerance);

    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    int boxW = std::max(1, std::min(cfg.box_width, screenW));
    int boxH = std::max(1, std::min(cfg.box_height, screenH));
    const int offX = (screenW - boxW) / 2;
    const int offY = (screenH - boxH) / 2;
    const int centerX = boxW / 2;
    const int centerY = boxH / 2;
    const int maxSnapX = (int)(boxW / cfg.max_snap_divisor);
    const int maxSnapY = (int)(boxH / cfg.max_snap_divisor);
    printf("  capture box      : %dx%d @ (%d,%d)  (screen %dx%d)\n", boxW, boxH, offX, offY, screenW, screenH);

    // 1) capture + scan (OWC_CPU=1 forces the GDI fallback for diagnostics)
    IRegionCapture* cap = nullptr;
    if (getenv("OWC_CPU"))
        cap = createCpuCapture(cfg.target_colors, cfg.target_color_tolerance, boxW, boxH, offX, offY);
    if (!cap)
        cap = createGpuCapture(cfg.target_colors, cfg.target_color_tolerance, boxW, boxH, offX, offY);
    if (!cap)
        cap = createCpuCapture(cfg.target_colors, cfg.target_color_tolerance, boxW, boxH, offX, offY);
    if (!cap) {
        fprintf(stderr, "[main] fatal: no capture backend available\n");
        timeEndPeriod(1);
        return 1;
    }
    cap->setFrameIntervalSeconds(1.0 / std::max(cfg.fps, 1.0f));
    printf("  capture          : %s\n", cap->name());

    // 2) input (Interception driver, auto-picked mouse, SendInput fallback)
    bool driverOk = false;
    const int autoMouse = autoPickMouseId();
    const int mouseId = (autoMouse != 0) ? autoMouse : cfg.mouse_id;
    IInput* input = createInterceptionInput(mouseId, cfg.keyboard_id, driverOk);
    if (!input) {
        input = createSendInputFallback();
        printf("  input            : SendInput fallback (Interception driver NOT detected)\n");
    } else {
        const bool replaced = (autoMouse != 0 && autoMouse != cfg.mouse_id);
        printf("  input            : Interception driver (mouse=%d auto-selected, keyboard=%d)%s\n",
               mouseId, cfg.keyboard_id, replaced ? "  [cfg mouse_id ignored]" : "");
        printInterceptionDevices();
    }

    Shared shared;
    g_shared = &shared;

    AimBot aimBot(cfg, *input, shared);
    aimBot.captureCenterX = centerX;
    aimBot.captureCenterY = centerY;
    aimBot.maxSnapX = maxSnapX;
    aimBot.maxSnapY = maxSnapY;

    std::thread tCap(captureThreadBody, cap, &shared, std::cref(cfg));
    std::thread tAim(aimThreadBody, &aimBot, &shared);
    std::thread tToggle(toggleThreadBody, std::cref(cfg), input, &shared);

    printf("[main] running... press Ctrl+C to stop.\n");
    fflush(stdout);

    tCap.join();
    tAim.join();
    tToggle.join();

    g_shared = nullptr;
    destroyCapture(cap);
    delete input;
    timeEndPeriod(1);
    printf("[main] exited cleanly\n");
    return 0;
}

} // namespace owc

int main() {
    return owc::runMain();
}
