#include "core/settings.h"

#include <windows.h>

#include <algorithm>
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
    s.widescreen = ReadInt(L"Graphics", L"Widescreen", d.widescreen, 0, 1) != 0;
    s.anisotropy = ReadInt(L"Graphics", L"Anisotropy", d.anisotropy, 1, 16);
    s.bloom = ReadInt(L"Graphics", L"Bloom", d.bloom, 0, 1) != 0;
    s.fpsLimit = ReadInt(L"Game", L"FpsLimit", d.fpsLimit, 30, 60) >= 45 ? 60 : 30;
    s.coop = ReadInt(L"Coop", L"Enabled", d.coop, 0, 1) != 0;
    s.coopInput = ReadInt(L"Coop", L"Input", d.coopInput, 0, 2);
    s.coopDeath = ReadInt(L"Coop", L"Player2Death", d.coopDeath, 0, 1);
    s.coopStorySafety = ReadInt(L"Coop", L"StorySafety", d.coopStorySafety, 0, 1) != 0;
    s.coopCamera = ReadInt(L"Coop", L"Camera", d.coopCamera, 0, 1);
    s.coopPlayer2 = ReadText(L"Coop", L"Player2", d.coopPlayer2);
    SaveSettings(); // writes defaults for missing keys, normalizes values
    LOG_INFO("Settings: %dx%d%s%s%s, resolution scale %d%s, %s, %dx anisotropic, bloom %s, %d fps", s.width, s.height,
        s.fullscreen ? " fullscreen" : "", s.vsync ? " vsync" : "", s.stretch ? " stretched" : "", s.resolutionScale,
        s.resolutionScale ? "" : " (auto)", s.widescreen ? "16:9" : "4:3", s.anisotropy, s.bloom ? "on" : "off",
        s.fpsLimit);
}

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
    WriteInt(L"Graphics", L"Anisotropy", s.anisotropy);
    WriteInt(L"Graphics", L"Bloom", s.bloom);
    WriteInt(L"Game", L"FpsLimit", s.fpsLimit);
    WriteInt(L"Coop", L"Enabled", s.coop);
    WriteInt(L"Coop", L"Input", s.coopInput);
    WriteInt(L"Coop", L"Player2Death", s.coopDeath);
    WriteInt(L"Coop", L"StorySafety", s.coopStorySafety);
    WriteInt(L"Coop", L"Camera", s.coopCamera);
    WriteText(L"Coop", L"Player2", s.coopPlayer2);
}

} // namespace swrots
