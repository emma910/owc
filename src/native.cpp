#include "native.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#include "interception.h"

namespace owc {

namespace {

class InterceptionInput final : public IInput {
public:
    InterceptionContext ctx = nullptr;
    int mouseId = 11;
    int keyboardId = 1;

    bool init(int m, int k) {
        mouseId = m;
        keyboardId = k;
        ctx = interception_create_context();
        return ctx != nullptr;
    }
    ~InterceptionInput() override {
        if (ctx) interception_destroy_context(ctx);
    }

    bool usesDriver() const override { return true; }

    void sendMouse(const InterceptionMouseStroke& s) const {
        if (ctx) interception_send(ctx, mouseId, (const InterceptionStroke*)&s, 1);
    }
    void sendKey(int code, int state) const {
        InterceptionKeyStroke s{};
        s.code = (unsigned short)code;
        s.state = (unsigned short)state;
        s.information = 0;
        if (ctx) interception_send(ctx, keyboardId, (const InterceptionStroke*)&s, 1);
    }

    void move(int dx, int dy) override {
        InterceptionMouseStroke s{};
        s.state = 0;
        s.flags = INTERCEPTION_MOUSE_MOVE_RELATIVE;
        s.rolling = 0;
        s.x = dx;
        s.y = dy;
        s.information = 0;
        sendMouse(s);
    }
    void click() override {
        InterceptionMouseStroke d{};
        d.state = INTERCEPTION_MOUSE_LEFT_BUTTON_DOWN;
        sendMouse(d);
        Sleep(CLICK_HOLD_MS);
        InterceptionMouseStroke u{};
        u.state = INTERCEPTION_MOUSE_LEFT_BUTTON_UP;
        sendMouse(u);
    }
    void keyDown(int scanCode) override { sendKey(scanCode, INTERCEPTION_KEY_DOWN); }
    void keyUp(int scanCode) override { sendKey(scanCode, INTERCEPTION_KEY_UP); }
};

class SendInputInput final : public IInput {
public:
    void move(int dx, int dy) override {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_MOVE; // relative
        in.mi.dx = dx;
        in.mi.dy = dy;
        SendInput(1, &in, sizeof(in));
    }
    void click() override {
        INPUT d{};
        d.type = INPUT_MOUSE;
        d.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        SendInput(1, &d, sizeof(d));
        Sleep(CLICK_HOLD_MS);
        INPUT u{};
        u.type = INPUT_MOUSE;
        u.mi.dwFlags = MOUSEEVENTF_LEFTUP;
        SendInput(1, &u, sizeof(u));
    }
    void keyDown(int scanCode) override {
        INPUT k{};
        k.type = INPUT_KEYBOARD;
        k.ki.wVk = 0;
        k.ki.wScan = (WORD)scanCode;
        k.ki.dwFlags = KEYEVENTF_SCANCODE;
        SendInput(1, &k, sizeof(k));
    }
    void keyUp(int scanCode) override {
        INPUT k{};
        k.type = INPUT_KEYBOARD;
        k.ki.wVk = 0;
        k.ki.wScan = (WORD)scanCode;
        k.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        SendInput(1, &k, sizeof(k));
    }
};

} // namespace

bool looksLikeTouchDevice(const char* hwid); // defined further down

IInput* createInterceptionInput(int mouseDeviceId, int keyboardDeviceId, bool& driverOk) {
    InterceptionInput* in = new InterceptionInput();
    if (!in->init(mouseDeviceId, keyboardDeviceId)) {
        delete in;
        driverOk = false;
        return nullptr;
    }
    driverOk = true;
    return in;
}

void printInterceptionDevices() {
    InterceptionContext ctx = interception_create_context();
    if (!ctx) {
        printf("  input devices    : none (Interception driver not installed)\n");
        return;
    }
    char hwid[512];
    for (int d = 1; d < INTERCEPTION_MAX_DEVICE; ++d) {
        const unsigned int n = interception_get_hardware_id(ctx, d, hwid, sizeof(hwid));
        if (n == 0 || n == (unsigned int)-1) continue; // not present
        const char* type = interception_is_keyboard(d) ? "keyboard"
                         : interception_is_mouse(d)    ? "mouse"
                                                       : "other";
        printf("      [%2d] %-8s %.*s%s\n", d, type, (int)n, hwid,
               (interception_is_mouse(d) && looksLikeTouchDevice(hwid)) ? "  (skipped: touch-like)" : "");
    }
    interception_destroy_context(ctx);
}

// Case-insensitive fingerprints of HID touchpads / touchscreens so the
// auto-picker keeps only real mice.
bool looksLikeTouchDevice(const char* hwid) {
    std::string s;
    for (const char* p = hwid; *p; ++p) s += (char)std::tolower((unsigned char)*p);
    static const char* kPatterns[] = {
        "touchpad", "touchscreen", "touch panel", "pen", "wacom",
        "ipts", "iths", "ntrig", "synaptics", "elan", "etd", "goodix", "neonode",
        "vid_06cb", // Synaptics
        "vid_04f3", // ELAN
        "vid_044e", // Synaptics (older)
        "vid_044c", // Alps
        "vid_1bc0", // N-trig / Surface
    };
    for (const char* pat : kPatterns)
        if (s.find(pat) != std::string::npos) return true;
    return false;
}

int autoPickMouseId() {
    InterceptionContext ctx = interception_create_context();
    if (!ctx) {
        printf("  [pick] no interception context\n");
        return 0;
    }

    struct Cand {
        int id;
        char hw[512];
    };
    std::vector<Cand> mice;
    for (int d = 11; d <= INTERCEPTION_MAX_DEVICE; ++d) { // mouse ids are 11..20
        if (!interception_is_mouse(d)) continue;
        Cand c;
        c.id = d;
        const unsigned int n = interception_get_hardware_id(ctx, d, c.hw, sizeof(c.hw));
        if (n == 0) continue;
        printf("  [pick] mouse id %2d: hwid_n=%u '%s'\n", d, n, c.hw);
        mice.push_back(c);
    }
    printf("  [pick] %zu mouse device(s) found\n", mice.size());
    if (mice.empty()) {
        interception_destroy_context(ctx);
        return 0;
    }

    int best = 0;
    for (const Cand& c : mice) {
        const bool touch = looksLikeTouchDevice(c.hw);
        const char* skip = touch ? ", touch-like, skipped" : "";
        // 3px relative-move probe: the id that actually displaces the cursor
        // is the functional mouse. Real mice are preferred over touch devices.
        if (touch) {
            printf("      [%2d] mouse  %s%s\n", c.id, c.hw, skip);
            continue;
        }
        POINT before{}, after{};
        GetCursorPos(&before);
        InterceptionMouseStroke s{};
        s.flags = INTERCEPTION_MOUSE_MOVE_RELATIVE;
        s.x = 3;
        s.y = 3;
        interception_send(ctx, c.id, (const InterceptionStroke*)&s, 1);
        Sleep(35);
        GetCursorPos(&after);
        const bool moved = (after.x != before.x) || (after.y != before.y);
        printf("      [%2d] mouse  %-20s %s\n", c.id, c.hw, moved ? "-> OK, selected" : "-> no movement");
        fflush(stdout);
        if (moved && best == 0) best = c.id;
    }

    interception_destroy_context(ctx);
    if (best == 0 && !mice.empty()) {
        // Nothing responded; fall back to the first detected mouse id.
        best = mice.front().id;
        printf("      [%2d] mouse  no probe response, using as fallback\n", best);
    }
    return best;
}

IInput* createSendInputFallback() {
    return new SendInputInput();
}

bool globalKeyDown(int vk) {
    // GetAsyncKeyState is the correct async poll for a worker thread (unlike
    // GetKeyState which is bound to the calling thread's message queue).
    return (GetAsyncKeyState((int)vk) & 0x8000) != 0;
}

int vkToScan(int vk) {
    return (int)MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
}

} // namespace owc