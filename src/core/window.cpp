#include "core/window.h"

#include <shellapi.h>
#include <shobjidl.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>

#include <atomic>
#include <cwchar>
#include <string>
#include <thread>

#include "core/icon.h"
#include "core/log.h"
#include "core/settings.h"
#include "debug/menu.h"
#include "input/controls.h"
#include "net/session.h"

namespace swrots {

namespace {

const wchar_t* kTitle = L"Star Wars: Episode III - Revenge of the Sith";
const wchar_t* kAppUserModelId = L"SWROTS-PC.Game";
const wchar_t* kFrameClass = L"SWROTSWindow";
const wchar_t* kGameClass = L"SWROTSGame";

// Frame -> game window notifications.
constexpr UINT kMsgReleaseMouse = WM_APP + 1; // the frame lost focus or is being moved/resized
constexpr UINT kMsgReclipMouse = WM_APP + 2;  // the frame moved: keep the cursor inside the game

bool FullscreenSetting() { return GetSettings().fullscreen; }

// What the taskbar uses when the window is pinned: this program, its name, and the icon the
// installer extracted from the game (swrots.exe itself carries no game artwork).
void SetRelaunchProperties(HWND hwnd)
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    const std::wstring icon = dir + L"\\swrots.ico";
    IPropertyStore* store = nullptr;
    if (FAILED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store))))
        return;
    auto set = [store](const PROPERTYKEY& key, const std::wstring& value) {
        PROPVARIANT v;
        if (SUCCEEDED(InitPropVariantFromString(value.c_str(), &v))) {
            store->SetValue(key, v);
            PropVariantClear(&v);
        }
    };
    set(PKEY_AppUserModel_ID, kAppUserModelId);
    set(PKEY_AppUserModel_RelaunchCommand, L"\"" + std::wstring(exe) + L"\"");
    set(PKEY_AppUserModel_RelaunchDisplayNameResource, kTitle);
    if (GetFileAttributesW(icon.c_str()) != INVALID_FILE_ATTRIBUTES)
        set(PKEY_AppUserModel_RelaunchIconResource, icon + L",0");
    store->Commit();
    store->Release();
}

// Smallest game area a player can size the window to (16:9).
constexpr LONG kMinClientWidth = 640, kMinClientHeight = 360;

// Set once the game has presented a frame: from then on its frames paint the window, and
// painting it black in between would flicker while the window is resized.
std::atomic<bool> g_GamePresented = false;

// --- Frame window ------------------------------------------------------------------------------

HWND g_Frame = nullptr;

HWND GameChild() { return g_Frame ? FindWindowExW(g_Frame, nullptr, kGameClass, nullptr) : nullptr; }

// Keeps the client area at the game's aspect ratio while the player drags a
// window edge, so a resized window shows the picture without black bars.
void KeepAspect(HWND hwnd, WPARAM edge, RECT* r)
{
    if (GetSettings().stretch)
        return;
    RECT frame = {};
    AdjustWindowRectExForDpi(&frame, DWORD(GetWindowLongW(hwnd, GWL_STYLE)), FALSE,
        DWORD(GetWindowLongW(hwnd, GWL_EXSTYLE)), GetDpiForWindow(hwnd));
    const LONG fw = frame.right - frame.left, fh = frame.bottom - frame.top;
    const double aspect = GetSettings().widescreen ? 16.0 / 9.0 : 4.0 / 3.0;
    LONG cw = r->right - r->left - fw, ch = r->bottom - r->top - fh;
    if (edge == WMSZ_LEFT || edge == WMSZ_RIGHT) {
        ch = LONG(cw / aspect + 0.5);
        r->bottom = r->top + ch + fh;
    } else if (edge == WMSZ_TOP || edge == WMSZ_BOTTOM) {
        cw = LONG(ch * aspect + 0.5);
        r->right = r->left + cw + fw;
    } else {
        // Corners: follow the width, and grow or shrink from the dragged corner.
        ch = LONG(cw / aspect + 0.5);
        if (edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT)
            r->top = r->bottom - ch - fh;
        else
            r->bottom = r->top + ch + fh;
    }
}

LRESULT CALLBACK FrameProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_DPICHANGED: {
        // Moved to a monitor with other display scaling: take the size Windows suggests.
        const RECT* r = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        RECT r = { 0, 0, kMinClientWidth, kMinClientHeight };
        AdjustWindowRectExForDpi(&r, DWORD(GetWindowLongW(hwnd, GWL_STYLE)), FALSE,
            DWORD(GetWindowLongW(hwnd, GWL_EXSTYLE)), GetDpiForWindow(hwnd));
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize.x = r.right - r.left;
        mmi->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }
    case WM_SIZING:
        KeepAspect(hwnd, wParam, reinterpret_cast<RECT*>(lParam));
        return TRUE;
    case WM_SIZE:
        if (HWND child = GameChild())
            SetWindowPos(child, nullptr, 0, 0, LOWORD(lParam), HIWORD(lParam), SWP_NOZORDER | SWP_NOACTIVATE);
        break;
    case WM_MOVE:
        if (HWND child = GameChild())
            PostMessageW(child, kMsgReclipMouse, 0, 0);
        break;
    case WM_ENTERSIZEMOVE:
        if (HWND child = GameChild())
            PostMessageW(child, kMsgReleaseMouse, 0, 0);
        break;
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
            ClipCursor(nullptr);
            if (HWND child = GameChild())
                PostMessageW(child, kMsgReleaseMouse, 0, 0);
        }
        break;
    case WM_SETFOCUS:
        // Keyboard focus belongs to the game's window.
        if (HWND child = GameChild()) {
            SetFocus(child);
            return 0;
        }
        break;
    case WM_PARENTNOTIFY:
        // The game's window appeared: give it the focus if the frame is active.
        if (LOWORD(wParam) == WM_CREATE && GetForegroundWindow() == hwnd)
            SetFocus(reinterpret_cast<HWND>(lParam));
        break;
    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_KEYMENU) // Alt / F10 menu mode: this window has no menu
            return 0;
        break;
    case WM_CLOSE:
        net::OnlineGoodbye(); // online: the other player told at once (Alt+F4, the window's close)
        LogFlush();
        ExitProcess(0);
    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(hwnd, &r);
        FillRect(reinterpret_cast<HDC>(wParam), &r, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        return 1;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// --- Game window -------------------------------------------------------------------------------

HWND g_Window = nullptr;
std::atomic<bool> g_MouseCaptured = false;
// Set when the window gets the focus or the mouse is captured by a click: that
// click (and any button still held) is not game input.
std::atomic<bool> g_IgnoreHeldButtons = false;

void CaptureMouse(HWND hwnd, bool capture)
{
    g_MouseCaptured = capture && !IsIconic(GetAncestor(hwnd, GA_ROOT));
    if (!g_MouseCaptured) {
        ClipCursor(nullptr);
        return;
    }
    RECT r;
    GetClientRect(hwnd, &r);
    MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&r), 2);
    ClipCursor(&r);
}

// The mouse is captured (hidden and kept inside the game area) when the
// player clicks into the game, or whenever a fullscreen game has the focus;
// losing focus or grabbing the window frame releases it, so the title bar
// and its buttons stay reachable.
LRESULT CALLBACK GameProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // The debug menu takes the keyboard and mouse while it is open (F1), with the cursor free.
    const bool menuWasOpen = debug::MenuOpen();
    LRESULT menuResult = 0;
    if (debug::MenuWindowMessage(hwnd, msg, wParam, lParam, &menuResult)) {
        if (debug::MenuOpen() && !menuWasOpen)
            CaptureMouse(hwnd, false);
        return menuResult;
    }
    switch (msg) {
    case WM_SETFOCUS:
        g_IgnoreHeldButtons = true;
        CaptureMouse(hwnd, FullscreenSetting());
        break;
    case WM_KILLFOCUS:
    case kMsgReleaseMouse:
        CaptureMouse(hwnd, false);
        break;
    case kMsgReclipMouse:
    case WM_SIZE:
        if (g_MouseCaptured)
            CaptureMouse(hwnd, true);
        break;
    case WM_MOUSEACTIVATE:
        // A click on the game activates the frame (cross-process children do not by themselves).
        SetForegroundWindow(GetAncestor(hwnd, GA_ROOT));
        return MA_ACTIVATE;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
        if (GetFocus() != hwnd)
            SetFocus(hwnd);
        if (!g_MouseCaptured) {
            g_IgnoreHeldButtons = true;
            CaptureMouse(hwnd, true);
        }
        break;
    case WM_INPUT: {
        RAWINPUT raw;
        UINT size = sizeof(raw);
        if (g_MouseCaptured && GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &raw, &size,
                sizeof(RAWINPUTHEADER)) != UINT(-1) && raw.header.dwType == RIM_TYPEMOUSE) {
            if (!(raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
                input::AddMouseMotion(raw.data.mouse.lLastX, raw.data.mouse.lLastY);
            if (raw.data.mouse.usButtonFlags & RI_MOUSE_WHEEL)
                input::AddMouseWheel(SHORT(raw.data.mouse.usButtonData));
        }
        break;
    }
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_SYSCHAR:
        // With Alt held every key is a system key: Windows would look for a menu shortcut
        // (menu mode releases the mouse, a missing shortcut beeps). The game reads the keys
        // itself; only Alt+F4 still closes the window.
        if (msg != WM_SYSCHAR && wParam == VK_F4)
            break;
        return 0;
    case WM_SETCURSOR:
        if (g_MouseCaptured && LOWORD(lParam) == HTCLIENT) {
            SetCursor(nullptr);
            return TRUE;
        }
        break;
    case WM_ERASEBKGND: {
        if (g_GamePresented)
            return 1;
        RECT r;
        GetClientRect(hwnd, &r);
        FillRect(reinterpret_cast<HDC>(wParam), &r, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        return 1;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

void NoteGameFramePresented() { g_GamePresented = true; }

HWND CreateMainWindow(const XbeFile& xbe)
{
    const Settings& s = GetSettings();
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = 0; // no full repaint per size step: the game's frames cover the window
    wc.lpfnWndProc = FrameProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kFrameClass;
    RegisterClassExW(&wc);

    if (s.fullscreen) {
        HMONITOR monitor = MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(monitor, &mi);
        const RECT& m = mi.rcMonitor;
        g_Frame = CreateWindowExW(0, kFrameClass, kTitle, WS_POPUP | WS_CLIPCHILDREN, m.left, m.top, m.right - m.left,
            m.bottom - m.top, nullptr, nullptr, wc.hInstance, nullptr);
    } else {
        const DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
        RECT r = { 0, 0, s.width, s.height };
        AdjustWindowRect(&r, style, FALSE);
        g_Frame = CreateWindowExW(0, kFrameClass, kTitle, style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
            r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    }
    if (!g_Frame)
        Fatal("Could not create the game window");
    SetWindowIconFromXbe(g_Frame, xbe);
    SetRelaunchProperties(g_Frame);
    // Development aid: SWROTS_BACKGROUND=1 starts the game minimised and inactive, for unattended tests
    // that leave the desktop alone (frames are still drawn: d3d/device.cpp; the sound is muted: audio.cpp).
    ShowWindow(g_Frame, RunningInBackground() ? SW_SHOWMINNOACTIVE : SW_SHOW);
    UpdateWindow(g_Frame);
    return g_Frame;
}

HWND CreateGameWindow(bool fullscreen)
{
    (void)fullscreen;
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = 0; // no full repaint per size step: the game's frames cover the window
    wc.lpfnWndProc = GameProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kGameClass;
    RegisterClassExW(&wc);

    HWND parent = g_Frame;
    if (!parent)
        Fatal("The game window does not exist.");

    RECT r;
    GetClientRect(parent, &r);
    g_Window = CreateWindowExW(0, kGameClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, r.right, r.bottom,
        parent, nullptr, wc.hInstance, nullptr);
    if (!g_Window)
        Fatal("Could not create the game window");
    // Relative mouse motion for the right stick (controls.cpp).
    RAWINPUTDEVICE mouse = { 0x01, 0x02, 0, g_Window }; // generic desktop, mouse
    RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
    if (GetForegroundWindow() == parent)
        SetFocus(g_Window);
    return g_Window;
}

HWND GameWindow() { return g_Window; }

bool RunningInBackground()
{
    static const bool background = GetEnvironmentVariableW(L"SWROTS_BACKGROUND", nullptr, 0) != 0;
    return background;
}

bool GameWindowActive() { return g_Window && GetForegroundWindow() == GetAncestor(g_Window, GA_ROOT); }

bool MouseButtonsAreGameInput()
{
    if (!g_MouseCaptured || !GameWindowActive())
        return false;
    if (g_IgnoreHeldButtons) {
        const int buttons[] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 };
        for (int vk : buttons)
            if (GetAsyncKeyState(vk) & 0x8000)
                return false;
        g_IgnoreHeldButtons = false;
    }
    return true;
}

void RunMessageLoop()
{
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    net::OnlineGoodbye();
    ExitProcess(0);
}

} // namespace swrots
