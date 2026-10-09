// Controllers: native replacement for the XDK input library (XPP).
//
// Port 1 is always a connected gamepad driven by the keyboard and the first
// host controller; ports 2-4 follow additional controllers. Host controllers
// are XInput pads (Xbox and compatible) first, then PlayStation pads.
// Xbox "Black" and "White" buttons map to the right and left shoulder buttons.

#include <windows.h>
#include <Xinput.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include "core/log.h"
#include "core/settings.h"
#include "core/window.h"
#include "debug/menu.h"
#include "input/controls.h"
#include "input/playstation.h"
#include "game/game.h"
#include "xapi/xapi.h"

namespace swrots::input {

// XPP_DEVICE_TYPE, owned by the game (XDEVICE_TYPE_GAMEPAD etc.).
struct XppDeviceType {
    ULONG CurrentConnected;
    ULONG ChangeConnected;
    ULONG PreviousConnected;
};

struct XGamepad {
    WORD wButtons;
    BYTE bAnalogButtons[8];
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
};

struct XInputState {
    DWORD dwPacketNumber;
    XGamepad Gamepad;
};

struct XFeedbackHeader {
    DWORD dwStatus;
    HANDLE hEvent;
    BYTE Reserved[58];
};

struct XRumble {
    WORD wLeftMotorSpeed;
    WORD wRightMotorSpeed;
};

struct XFeedback {
    XFeedbackHeader Header;
    XRumble Rumble;
};

enum : WORD {
    XB_DPAD_UP = 0x0001, XB_DPAD_DOWN = 0x0002, XB_DPAD_LEFT = 0x0004, XB_DPAD_RIGHT = 0x0008,
    XB_START = 0x0010, XB_BACK = 0x0020, XB_LEFT_THUMB = 0x0040, XB_RIGHT_THUMB = 0x0080,
};
enum { XB_A, XB_B, XB_X, XB_Y, XB_BLACK, XB_WHITE, XB_LEFT_TRIGGER, XB_RIGHT_TRIGGER };

struct Port {
    bool open = false;
    DWORD packet = 0;
    XGamepad last = {};
};

static std::mutex g_Lock;
static Port g_Ports[4];

void ResetPortsForReboot()
{
    std::lock_guard<std::mutex> lock(g_Lock);
    for (Port& p : g_Ports)
        p = Port();
}
static XppDeviceType* const g_Gamepad = reinterpret_cast<XppDeviceType*>(uintptr_t(0x00557F14));

// A host controller: an XInput user index, or the n-th PlayStation pad.
struct Source {
    bool playStation;
    DWORD index;
};

// The connected controllers, refreshed at most twice a second (polling
// XInput slots that have no controller is slow).
static std::vector<Source> Sources()
{
    static std::mutex lock;
    static std::vector<Source> cached;
    static ULONGLONG refreshed = 0;
    std::lock_guard<std::mutex> g(lock);
    const ULONGLONG now = GetTickCount64();
    if (refreshed && now - refreshed < 500)
        return cached;
    refreshed = now;
    std::vector<Source>& sources = cached;
    sources.clear();
    for (DWORD i = 0; i < 4; ++i) {
        XINPUT_STATE s;
        if (XInputGetState(i, &s) == ERROR_SUCCESS)
            sources.push_back({ false, i });
    }
    for (int i = 0, n = PlayStationPadCount(); i < n; ++i)
        sources.push_back({ true, DWORD(i) });
    return sources;
}

// Co-op (SetCoopInput): whether the keyboard alone is player 1, the host controllers then starting at port 1.
static std::atomic<bool> g_CoopInput = false;

static bool KeyboardAlone(size_t controllers)
{
    if (!g_CoopInput)
        return false;
    const int mode = GetSettings().coopInput;
    return mode == 1 || (mode == 0 && controllers == 1);
}

// The host controller playing a port, or -1 (port 0's keyboard is read apart).
static int ControllerOfPort(DWORD port, size_t controllers)
{
    const int index = KeyboardAlone(controllers) ? int(port) - 1 : int(port);
    return index >= 0 && size_t(index) < controllers ? index : -1;
}

void SetCoopInput(bool active)
{
    if (g_CoopInput.exchange(active) != active)
        LOG_INFO("Input: co-op %s (%s)", active ? "on" : "off",
            active ? (KeyboardAlone(Sources().size()) ? "the keyboard is player 1, the first controller player 2"
                                                      : "player 2 is the second controller") : "as before");
}

// Test switch SWROTS_TEST_PAD2=<seconds>[:run]: a scripted controller on port 1 (player 2), from that many
// seconds on: its stick circles and it attacks, or with ":run" it runs straight ahead.
// ":until<seconds>" unplugs it then, ":start<seconds>" presses player 1's Start then (pause), ":pick<seconds>"
// presses player 1's Up then A (in the pause menu: the entry above the first). Seconds count from the first
// input read.
struct TestPad2Script {
    bool on = false, run = false;
    double start = 0, until = 0, pause = -1, pick = -1;
    ULONGLONG origin = 0;
};

static const TestPad2Script& TestPad2Spec()
{
    static const TestPad2Script spec = [] {
        TestPad2Script s;
        char v[64] = {};
        if (!GetEnvironmentVariableA("SWROTS_TEST_PAD2", v, sizeof(v)))
            return s;
        s.on = true;
        s.start = atof(v);
        s.run = std::strstr(v, ":run") != nullptr;
        if (const char* until = std::strstr(v, ":until"))
            s.until = atof(until + 6);
        if (const char* pause = std::strstr(v, ":start"))
            s.pause = atof(pause + 6);
        if (const char* pick = std::strstr(v, ":pick"))
            s.pick = atof(pick + 5);
        s.origin = GetTickCount64();
        return s;
    }();
    return spec;
}

static double TestPad2Seconds()
{
    return double(GetTickCount64() - TestPad2Spec().origin) / 1000.0;
}

static bool TestPad2()
{
    const TestPad2Script& s = TestPad2Spec();
    return s.on && (s.until <= 0 || TestPad2Seconds() < s.until);
}

bool Player2HasController()
{
    return TestPad2() || ControllerOfPort(1, Sources().size()) >= 0;
}

// Connected-port mask: port 0 always, others by host controller presence.
static ULONG ConnectedMask()
{
    ULONG mask = TestPad2() ? 3 : 1;
    const size_t count = Sources().size();
    for (DWORD i = 1; i < 4; ++i)
        if (ControllerOfPort(i, count) >= 0)
            mask |= 1u << i;
    return mask;
}

static void RefreshConnections()
{
    ULONG now = ConnectedMask();
    if (now != g_Gamepad->CurrentConnected) {
        g_Gamepad->ChangeConnected |= now ^ g_Gamepad->CurrentConnected;
        g_Gamepad->CurrentConnected = now;
    }
}

static BYTE Digital(bool down) { return down ? 0xFF : 0x00; }

static void ReadPlayStation(DWORD index, XGamepad& g)
{
    PadState s;
    if (!ReadPlayStationPad(int(index), s))
        return;
    g.wButtons |= s.buttons;
    for (int i = 0; i < 8; ++i)
        g.bAnalogButtons[i] |= s.analog[i];
    g.sThumbLX = s.lx;
    g.sThumbLY = s.ly;
    g.sThumbRX = s.rx;
    g.sThumbRY = s.ry;
}

static void ReadXInput(DWORD index, XGamepad& g)
{
    XINPUT_STATE s;
    if (XInputGetState(index, &s) != ERROR_SUCCESS)
        return;
    const XINPUT_GAMEPAD& p = s.Gamepad;
    if (p.wButtons & XINPUT_GAMEPAD_DPAD_UP) g.wButtons |= XB_DPAD_UP;
    if (p.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) g.wButtons |= XB_DPAD_DOWN;
    if (p.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) g.wButtons |= XB_DPAD_LEFT;
    if (p.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) g.wButtons |= XB_DPAD_RIGHT;
    if (p.wButtons & XINPUT_GAMEPAD_START) g.wButtons |= XB_START;
    if (p.wButtons & XINPUT_GAMEPAD_BACK) g.wButtons |= XB_BACK;
    if (p.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) g.wButtons |= XB_LEFT_THUMB;
    if (p.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) g.wButtons |= XB_RIGHT_THUMB;
    g.bAnalogButtons[XB_A] |= Digital(p.wButtons & XINPUT_GAMEPAD_A);
    g.bAnalogButtons[XB_B] |= Digital(p.wButtons & XINPUT_GAMEPAD_B);
    g.bAnalogButtons[XB_X] |= Digital(p.wButtons & XINPUT_GAMEPAD_X);
    g.bAnalogButtons[XB_Y] |= Digital(p.wButtons & XINPUT_GAMEPAD_Y);
    g.bAnalogButtons[XB_BLACK] |= Digital(p.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER);
    g.bAnalogButtons[XB_WHITE] |= Digital(p.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER);
    g.bAnalogButtons[XB_LEFT_TRIGGER] |= p.bLeftTrigger;
    g.bAnalogButtons[XB_RIGHT_TRIGGER] |= p.bRightTrigger;
    g.sThumbLX = p.sThumbLX;
    g.sThumbLY = p.sThumbLY;
    g.sThumbRX = p.sThumbRX;
    g.sThumbRY = p.sThumbRY;
}

static void ReadHostPad(DWORD port, XGamepad& g)
{
    const std::vector<Source> sources = Sources();
    const int index = ControllerOfPort(port, sources.size());
    if (index < 0)
        return;
    if (sources[index].playStation)
        ReadPlayStation(sources[index].index, g);
    else
        ReadXInput(sources[index].index, g);
}

// Keyboard and mouse (controls.ini) as player 1's controller.
static void ReadKeyboard(XGamepad& g)
{
    KeyboardPad k = ReadKeyboardPad();
    g.wButtons |= k.buttons;
    for (int i = 0; i < 8; ++i)
        g.bAnalogButtons[i] |= k.analog[i];
    if (k.lx) g.sThumbLX = k.lx;
    if (k.ly) g.sThumbLY = k.ly;
    if (k.rx) g.sThumbRX = k.rx;
    if (k.ry) g.sThumbRY = k.ry;
}

// For unattended tests: SWROTS_TEST_INPUT=<seconds> plays player 1 like a busy player from that many
// seconds after the first input read on: the left stick turning round, and A, B, X, Y, the triggers
// and the white and black buttons pressed in turn (never Start or Back); "<seconds>:run" only holds
// the stick forward. The variable clears itself,
// so a relaunched process does not inherit it.
static double g_TestInputFrom = -1.0;
static bool g_TestInputRun = false; // "<seconds>:run": the stick held forward, no buttons

static void ReadTestInput(XGamepad& g)
{
    static ULONGLONG start = 0;
    static bool read = false;
    if (!read) {
        read = true;
        char value[32] = {};
        if (GetEnvironmentVariableA("SWROTS_TEST_INPUT", value, sizeof(value))) {
            g_TestInputFrom = atof(value);
            g_TestInputRun = std::strstr(value, ":run") != nullptr;
            SetEnvironmentVariableA("SWROTS_TEST_INPUT", nullptr);
            LOG_INFO("Input: scripted player 1 from %.1f s", g_TestInputFrom);
        }
        start = GetTickCount64();
    }
    if (g_TestInputFrom < 0)
        return;
    const double t = double(GetTickCount64() - start) / 1000.0 - g_TestInputFrom;
    if (t < 0)
        return;
    if (g_TestInputRun) {
        g.sThumbLY = 30000;
        return;
    }
    const double angle = t * 1.3;
    g.sThumbLX = SHORT(std::cos(angle) * 30000.0);
    g.sThumbLY = SHORT(std::sin(angle) * 30000.0);
    // A press of 0.15 s every 0.4 s, cycling A, X, X, Y, B, right trigger, left trigger, white, black.
    static const int kOrder[] = { 0, 2, 2, 3, 1, 7, 6, 5, 4 };
    const int step = int(t / 0.4);
    if (t - step * 0.4 < 0.15)
        g.bAnalogButtons[kOrder[step % int(std::size(kOrder))]] = 0xFF;
}

// ---------------------------------------------------------------------------
// Free camera
// ---------------------------------------------------------------------------
static std::atomic<bool> g_HoldPlayer = false;

void HoldPlayerInput(bool hold)
{
    g_HoldPlayer = hold;
}

static float Stick(SHORT value)
{
    constexpr float kDeadZone = 0.2f;
    float v = value / 32768.0f;
    if (v > -kDeadZone && v < kDeadZone)
        return 0.0f;
    return (v - (v > 0 ? kDeadZone : -kDeadZone)) / (1.0f - kDeadZone);
}

// Modern free-camera controls. Keyboard and mouse: mouse to look, W A S D to move, E / Space up,
// Q / Ctrl down, hold Shift faster and Alt slower, the wheel for the base speed. Controller: the
// left stick to move, the right stick to look, RB up, LB down, RT faster and LT slower (by how far
// they are pressed), D-pad up / down for the base speed.
FreeCameraControls ReadFreeCameraControls()
{
    FreeCameraControls c = {};
    c.speed = 1.0f;
    LONG dx, dy, wheel;
    TakeMouseInput(dx, dy, wheel);
    if (!GameWindowActive() || debug::MenuOpen())
        return c;

    XGamepad g = {};
    ReadHostPad(0, g);
    c.right = Stick(g.sThumbLX);
    c.forward = Stick(g.sThumbLY);
    c.lookX = Stick(g.sThumbRX);
    c.lookY = Stick(g.sThumbRY);
    c.up = (g.bAnalogButtons[XB_BLACK] - g.bAnalogButtons[XB_WHITE]) / 255.0f; // RB, LB
    const float faster = g.bAnalogButtons[XB_RIGHT_TRIGGER] / 255.0f, slower = g.bAnalogButtons[XB_LEFT_TRIGGER] / 255.0f;
    c.speed *= (1.0f + 3.0f * faster) * (1.0f - 0.8f * slower);
    static WORD lastButtons = 0;
    const WORD pressed = g.wButtons & ~lastButtons;
    lastButtons = g.wButtons;
    if (pressed & XB_DPAD_UP)
        ++c.speedSteps;
    if (pressed & XB_DPAD_DOWN)
        --c.speedSteps;

    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    c.forward += (down('W') ? 1.0f : 0.0f) - (down('S') ? 1.0f : 0.0f);
    c.right += (down('D') ? 1.0f : 0.0f) - (down('A') ? 1.0f : 0.0f);
    c.up += (down('E') || down(VK_SPACE) ? 1.0f : 0.0f) - (down('Q') || down(VK_CONTROL) ? 1.0f : 0.0f);
    if (down(VK_SHIFT))
        c.speed *= 4.0f;
    if (down(VK_MENU))
        c.speed *= 0.2f;
    c.forward = std::clamp(c.forward, -1.0f, 1.0f);
    c.right = std::clamp(c.right, -1.0f, 1.0f);
    c.up = std::clamp(c.up, -1.0f, 1.0f);
    if (MouseButtonsAreGameInput()) {
        c.mouseDx = dx;
        c.mouseDy = dy;
        c.speedSteps += wheel / WHEEL_DELTA;
    }
    return c;
}

// ---------------------------------------------------------------------------
// XDK API
// ---------------------------------------------------------------------------
static void __stdcall XbInitDevices(DWORD preallocTypeCount, void* preallocTypes)
{
    (void)preallocTypeCount;
    (void)preallocTypes;
    std::lock_guard<std::mutex> lock(g_Lock);
    StartPlayStationPads();
    g_Gamepad->CurrentConnected = ConnectedMask();
    g_Gamepad->ChangeConnected = g_Gamepad->CurrentConnected;
    g_Gamepad->PreviousConnected = 0;
    LOG_INFO("XInitDevices: gamepads on ports mask %lX", g_Gamepad->CurrentConnected);
}

static DWORD __stdcall XbGetDevices(XppDeviceType* type)
{
    std::lock_guard<std::mutex> lock(g_Lock);
    if (type == g_Gamepad)
        RefreshConnections();
    DWORD connected = type->CurrentConnected;
    type->ChangeConnected = 0;
    type->PreviousConnected = connected;
    return connected;
}

static BOOL __stdcall XbGetDeviceChanges(XppDeviceType* type, DWORD* insertions, DWORD* removals)
{
    std::lock_guard<std::mutex> lock(g_Lock);
    if (type == g_Gamepad)
        RefreshConnections();
    *insertions = *removals = 0;
    if (!type->ChangeConnected)
        return FALSE;
    *insertions = type->CurrentConnected & ~type->PreviousConnected;
    *removals = type->PreviousConnected & ~type->CurrentConnected;
    ULONG bounced = type->ChangeConnected & type->CurrentConnected & type->PreviousConnected;
    *insertions |= bounced;
    *removals |= bounced;
    type->ChangeConnected = 0;
    type->PreviousConnected = type->CurrentConnected;
    return (*insertions | *removals) ? TRUE : FALSE;
}

static HANDLE __stdcall XbInputOpen(XppDeviceType* type, DWORD port, DWORD slot, void* pollingParameters)
{
    (void)slot;
    (void)pollingParameters;
    if (type != g_Gamepad || port >= 4)
        return nullptr;
    std::lock_guard<std::mutex> lock(g_Lock);
    if (!(g_Gamepad->CurrentConnected & (1u << port)))
        return nullptr;
    g_Ports[port].open = true;
    LOG_INFO("XInputOpen: gamepad port %lu", port);
    return &g_Ports[port];
}

static void __stdcall XbInputClose(HANDLE device)
{
    if (device)
        static_cast<Port*>(device)->open = false;
}

static DWORD __stdcall XbInputGetState(HANDLE device, XInputState* state)
{
    auto* port = static_cast<Port*>(device);
    if (!port || !port->open)
        return ERROR_DEVICE_NOT_CONNECTED;
    DWORD index = DWORD(port - g_Ports);
    XGamepad g = {};
    if (index != 0 || !g_HoldPlayer) { // the free camera has player 1's input
        ReadHostPad(index, g);
        if (index == 0) {
            ReadKeyboard(g);
            ReadTestInput(g);
            const double now = TestPad2Seconds(); // SWROTS_TEST_PAD2's ":start": player 1 pauses
            if (TestPad2Spec().on && TestPad2Spec().pause >= 0 && now >= TestPad2Spec().pause && now < TestPad2Spec().pause + 0.2)
                g.wButtons |= XB_START;
            if (TestPad2Spec().on && TestPad2Spec().pick >= 0 && now >= TestPad2Spec().pick && now < TestPad2Spec().pick + 0.15)
                g.wButtons |= XB_DPAD_UP;
            if (TestPad2Spec().on && TestPad2Spec().pick >= 0 && now >= TestPad2Spec().pick + 0.6 && now < TestPad2Spec().pick + 0.75)
                g.bAnalogButtons[0] = 0xFF;
        }
        if (index == 1 && TestPad2()) { // the scripted player 2 (TestPad2)
            const double t = TestPad2Seconds() - TestPad2Spec().start;
            if (t >= 0 && TestPad2Spec().run) {
                g.sThumbLY = 30000;
            } else if (t >= 0) {
                g.sThumbLX = SHORT(-std::cos(t * 0.9) * 30000.0);
                g.sThumbLY = SHORT(std::sin(t * 0.9) * 30000.0);
                const int step = int(t / 0.5);
                if (t - step * 0.5 < 0.15)
                    g.bAnalogButtons[(step % 2) ? 2 : 0] = 0xFF;
            }
        }
    }
    if (std::memcmp(&g, &port->last, sizeof(g)) != 0) {
        port->last = g;
        ++port->packet;
    }
    state->dwPacketNumber = port->packet;
    state->Gamepad = g;
    return ERROR_SUCCESS;
}

static DWORD __stdcall XbInputSetState(HANDLE device, XFeedback* feedback)
{
    auto* port = static_cast<Port*>(device);
    if (!port || !port->open)
        return ERROR_DEVICE_NOT_CONNECTED;
    const std::vector<Source> sources = Sources();
    const size_t index = size_t(port - g_Ports);
    if (index < sources.size()) {
        const Source& s = sources[index];
        if (s.playStation) {
            SetPlayStationRumble(int(s.index), feedback->Rumble.wLeftMotorSpeed, feedback->Rumble.wRightMotorSpeed);
        } else {
            XINPUT_VIBRATION v = { feedback->Rumble.wLeftMotorSpeed, feedback->Rumble.wRightMotorSpeed };
            XInputSetState(s.index, &v);
        }
    }
    // Rumble completes asynchronously on the Xbox; here it is done at once.
    feedback->Header.dwStatus = ERROR_SUCCESS;
    if (feedback->Header.hEvent)
        SetEvent(feedback->Header.hEvent);
    return ERROR_IO_PENDING;
}

SDK_REPLACE("XInitDevices", XbInitDevices);
SDK_REPLACE("XPP_sub_558F20", XbInitDevices); // jump thunk to XInitDevices
SDK_REPLACE("XGetDevices", XbGetDevices);
SDK_REPLACE("XGetDeviceChanges", XbGetDeviceChanges);
SDK_REPLACE("XInputOpen", XbInputOpen);
SDK_REPLACE("XInputClose", XbInputClose);
SDK_REPLACE("XInputGetState", XbInputGetState);
SDK_REPLACE("XInputSetState", XbInputSetState);

} // namespace swrots::input
