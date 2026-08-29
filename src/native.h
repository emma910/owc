#pragma once

#include <cstdint>

namespace owc {

constexpr unsigned long CLICK_HOLD_MS = 300; // between left-button down/up (original behavior)

struct IInput {
    virtual ~IInput() = default;
    virtual void move(int dx, int dy) = 0;
    virtual void click() = 0;
    virtual void keyDown(int scanCode) = 0;
    virtual void keyUp(int scanCode) = 0;
    virtual bool usesDriver() const { return false; }
};

// Interception-based backend: stealthy relative mouse / keyboard via the driver,
// invisible to normal user-mode hooks. driverOk reports whether the driver is present.
IInput* createInterceptionInput(int mouseDeviceId, int keyboardDeviceId, bool& driverOk);

// Lists every keyboard/mouse Interception device id present in the system
// (from the driver's point of view, with hardware ids). Prints to stdout.
void printInterceptionDevices();

// Auto-detects the best Interception mouse device id (11..20) for aiming:
//  - enumerates driver-present mouse ids,
//  - skips obvious HID touchpads/touchscreens,
//  - validates the rest with a live 3px relative-move probe and returns the
//    first id that actually displaces the cursor.
// Returns 0 if the driver is unavailable / has no mice.
int autoPickMouseId();

// SendInput fallback (functional when the Interception driver is not installed).
IInput* createSendInputFallback();

// Polls the async key state; true while the key is held.
bool globalKeyDown(int vk);

int vkToScan(int vk);

} // namespace owc