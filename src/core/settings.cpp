#include "core/settings.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "core/log.h"

namespace swrots {

namespace {

Settings g_Settings;
std::wstring g_Path;

int ReadInt(const wchar_t* section, const wchar_t* key, int def, int lo, int hi)
{
    return std::clamp(int(GetPrivateProfileIntW(section, key, def, g_Path.c_str())), lo, hi);
}

void WriteInt(const wchar_t* section, const wchar_t* key, int value)
{
    WritePrivateProfileStringW(section, key, std::to_wstring(value).c_str(), g_Path.c_str());
}

// Plain ASCII text (class names and the like).
std::string ReadText(const wchar_t* section, const wchar_t* key, const std::string& def)
{
    wchar_t buffer[128];
    GetPrivateProfileStringW(section, key, std::wstring(def.begin(), def.end()).c_str(), buffer, DWORD(std::size(buffer)),
        g_Path.c_str());
    std::string text;
    for (const wchar_t* c = buffer; *c; ++c)
        if (*c > 32 && *c < 127)
            text += char(*c);
    return text;
}

void WriteText(const wchar_t* section, const wchar_t* key, const std::string& value)
{
    WritePrivateProfileStringW(section, key, std::wstring(value.begin(), value.end()).c_str(), g_Path.c_str());
}

// A shape (width / height): "21:9", "3440x1440", "2.39"; "Auto", "0" or anything unreadable is 0 (automatic).
float ParseRatio(const std::string& text)
{
    const size_t sep = text.find_first_of(":xX/");
    float value = 0.0f;
    if (sep != std::string::npos) {
        const float w = float(std::atof(text.substr(0, sep).c_str())), h = float(std::atof(text.substr(sep + 1).c_str()));
        value = h > 0 ? w / h : 0.0f;
    } else {
        value = float(std::atof(text.c_str()));
    }
    // Narrower than 4:3 or wider than 4:1 is not a shape the picture takes.
    return value >= 1.0f && value <= 4.0f ? value : 0.0f;
}

std::string RatioText(float value)
{
    if (value <= 0.0f)
        return "Auto";
    static const struct {
        int w, h;
    } kNamed[] = { { 4, 3 }, { 5, 4 }, { 16, 10 }, { 16, 9 }, { 21, 9 }, { 32, 9 }, { 32, 10 }, { 48, 9 } };
    for (const auto& n : kNamed)
        if (std::fabs(value - float(n.w) / float(n.h)) < 0.0005f)
            return std::to_string(n.w) + ":" + std::to_string(n.h);
    char text[16];
    snprintf(text, sizeof text, "%.3f", value);
    return text;
}

} // namespace

const Settings& GetSettings() { return g_Settings; }
Settings& EditSettings() { return g_Settings; }

void LoadSettings(const std::wstring& path)
{
    g_Path = path;
    const Settings d;
    Settings& s = g_Settings;
    s.width = ReadInt(L"Display", L"Width", d.width, 320, 16384);
    s.height = ReadInt(L"Display", L"Height", d.height, 240, 16384);
    s.fullscreen = ReadInt(L"Display", L"Fullscreen", d.fullscreen, 0, 1) != 0;
    s.vsync = ReadInt(L"Display", L"VSync", d.vsync, 0, 1) != 0;
    s.stretch = ReadInt(L"Display", L"Stretch", d.stretch, 0, 1) != 0;
    s.resolutionScale = ReadInt(L"Graphics", L"ResolutionScale", d.resolutionScale, 0, 8);
    s.widescreen = ReadInt(L"Graphics", L"Widescreen", d.widescreen, 0, 2);
    s.aspectRatio = ParseRatio(ReadText(L"Graphics", L"AspectRatio", RatioText(d.aspectRatio)));
    s.hudAnchor = ReadInt(L"Graphics", L"HudAnchor", d.hudAnchor, 0, 1) != 0;
    s.hudAspect = ParseRatio(ReadText(L"Graphics", L"HudAspect", RatioText(d.hudAspect)));
    s.anisotropy = ReadInt(L"Graphics", L"Anisotropy", d.anisotropy, 1, 16);
    s.bloom = ReadInt(L"Graphics", L"Bloom", d.bloom, 0, 1) != 0;
    s.fpsLimit = ReadInt(L"Game", L"FpsLimit", d.fpsLimit, 30, 60) >= 45 ? 60 : 30;
    {
        wchar_t prompts[32] = {};
        GetPrivateProfileStringW(L"Controls", L"ButtonPrompts", L"Auto", prompts, 32, g_Path.c_str());
        s.buttonPrompts = _wcsicmp(prompts, L"Keyboard") == 0 ? 1 : _wcsicmp(prompts, L"Controller") == 0 ? 0 : -1;
    }
    s.coop = ReadInt(L"Coop", L"Enabled", d.coop, 0, 1) != 0;
    s.coopInput = ReadInt(L"Coop", L"Input", d.coopInput, 0, 2);
    s.coopDeath = ReadInt(L"Coop", L"Player2Death", d.coopDeath, 0, 1);
    s.coopStorySafety = ReadInt(L"Coop", L"StorySafety", d.coopStorySafety, 0, 1) != 0;
    s.coopCamera = ReadInt(L"Coop", L"Camera", d.coopCamera, 0, 1);
    s.coopFriendlyFire = ReadInt(L"Coop", L"FriendlyFire", d.coopFriendlyFire, 0, 1) != 0;
    s.coopBoss = ReadInt(L"Coop", L"Boss", d.coopBoss, 0, 1) != 0;
    s.coopPlayer2 = ReadText(L"Coop", L"Player2", d.coopPlayer2);
    s.hvvOverheadInfo = ReadInt(L"HvV", L"OverheadInfo", d.hvvOverheadInfo, 0, 2);
    s.hvvOthersSlowMotion = ReadInt(L"HvV", L"OthersSlowMotion", d.hvvOthersSlowMotion, 0, 1) != 0;
    SaveSettings(); // writes defaults for missing keys, normalizes values
    static const char* const kShapes[] = { "4:3", "16:9 (anamorphic)", "true widescreen" };
    LOG_INFO("Settings: %dx%d%s%s%s, resolution scale %d%s, %s, %dx anisotropic, bloom %s, %d fps", s.width, s.height,
        s.fullscreen ? " fullscreen" : "", s.vsync ? " vsync" : "", s.stretch ? " stretched" : "", s.resolutionScale,
        s.resolutionScale ? "" : " (auto)", kShapes[s.widescreen], s.anisotropy, s.bloom ? "on" : "off",
        s.fpsLimit);
    if (s.widescreen == 2)
        LOG_INFO("Settings: true widescreen at %.3f (aspect ratio %s), HUD %s, HUD shape %s", PictureAspect(),
            RatioText(s.aspectRatio).c_str(), s.hudAnchor ? "at the edges" : "centred", RatioText(s.hudAspect).c_str());
    static const char* const kInputs[] = { "auto", "keyboard is player 1", "two controllers" };
    LOG_INFO("Settings: co-op %s (input %s, player 2 death: %s, camera %s, friendly fire %s, boss %s, story safety %s, player 2 %s)",
        s.coop ? "on" : "off", kInputs[s.coopInput], s.coopDeath ? "game over" : "comes back", s.coopCamera ? "player 1" : "shared",
        s.coopFriendlyFire ? "on" : "off", s.coopBoss ? "player 2" : "the game's",
        s.coopStorySafety ? "on" : "off", s.coopPlayer2.empty() ? "automatic" : s.coopPlayer2.c_str());
}

float PictureAspect()
{
    const Settings& s = g_Settings;
    if (s.widescreen == 0)
        return 4.0f / 3.0f;
    if (s.widescreen == 1)
        return 16.0f / 9.0f;
    // Read once: the frame buffer's shape is chosen when the game creates its device, and stays.
    static float shape = [&] {
        float value = s.aspectRatio;
        if (value <= 0.0f) {
            // Automatic: the monitor's shape in fullscreen (the window covers the primary monitor),
            // else the window's.
            float w = float(s.width), h = float(s.height);
            if (s.fullscreen) {
                MONITORINFO mi = { sizeof(mi) };
                if (GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi)) {
                    w = float(mi.rcMonitor.right - mi.rcMonitor.left);
                    h = float(mi.rcMonitor.bottom - mi.rcMonitor.top);
                }
            }
            value = h > 0 ? w / h : 16.0f / 9.0f;
        }
        return std::clamp(value, 4.0f / 3.0f, 4.0f);
    }();
    return shape;
}

bool TrueWidescreen() { return g_Settings.widescreen == 2 && PictureAspect() > 4.0f / 3.0f + 0.01f; }

void SaveSettings()
{
    if (g_Path.empty())
        return;
    const Settings& s = g_Settings;
    WriteInt(L"Display", L"Width", s.width);
    WriteInt(L"Display", L"Height", s.height);
    WriteInt(L"Display", L"Fullscreen", s.fullscreen);
    WriteInt(L"Display", L"VSync", s.vsync);
    WriteInt(L"Display", L"Stretch", s.stretch);
    WriteInt(L"Graphics", L"ResolutionScale", s.resolutionScale);
    WriteInt(L"Graphics", L"Widescreen", s.widescreen);
    WriteText(L"Graphics", L"AspectRatio", RatioText(s.aspectRatio));
    WriteInt(L"Graphics", L"HudAnchor", s.hudAnchor);
    WriteText(L"Graphics", L"HudAspect", RatioText(s.hudAspect));
    WriteInt(L"Graphics", L"Anisotropy", s.anisotropy);
    WriteInt(L"Graphics", L"Bloom", s.bloom);
    WriteInt(L"Game", L"FpsLimit", s.fpsLimit);
    WritePrivateProfileStringW(L"Controls", L"ButtonPrompts",
        s.buttonPrompts == 1 ? L"Keyboard" : s.buttonPrompts == 0 ? L"Controller" : L"Auto", g_Path.c_str());
    WriteInt(L"Coop", L"Enabled", s.coop);
    WriteInt(L"Coop", L"Input", s.coopInput);
    WriteInt(L"Coop", L"Player2Death", s.coopDeath);
    WriteInt(L"Coop", L"StorySafety", s.coopStorySafety);
    WriteInt(L"Coop", L"Camera", s.coopCamera);
    WriteInt(L"Coop", L"FriendlyFire", s.coopFriendlyFire);
    WriteInt(L"Coop", L"Boss", s.coopBoss);
    WriteText(L"Coop", L"Player2", s.coopPlayer2);
    WriteInt(L"HvV", L"OverheadInfo", s.hvvOverheadInfo);
    WriteInt(L"HvV", L"OthersSlowMotion", s.hvvOthersSlowMotion);
}

} // namespace swrots
