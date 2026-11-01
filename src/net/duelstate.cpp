// The duel recorder (see duelstate.h).
//
// File: "SWDR", u32 version (1), u32 object bytes; then per frame: u32 frame, u32 p1 address, u32 p2 address,
// GamePad p1, GamePad p2 (the PC's live pads, ports 0 and 1), then each fighter's first `object bytes` (zero when
// it is not there or could not be read).

#include "net/duelstate.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "game/characters.h"
#include "game/coop.h"
#include "input/controls.h"

namespace swrots::net {

namespace {

constexpr uint32_t kObjectBytes = 0x1800;
FILE* g_Out = nullptr;
uint32_t g_Left = 0, g_Frame = 0;
std::string g_Name;

std::wstring ExeDir()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s = path;
    return s.substr(0, s.find_last_of(L"\\/"));
}

bool ReadObject(const uint8_t* object, uint8_t* out)
{
    std::memset(out, 0, kObjectBytes);
    if (!object)
        return false;
    __try {
        std::memcpy(out, object, kObjectBytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::memset(out, 0, kObjectBytes);
        return false;
    }
}

void Close(const char* why)
{
    if (!g_Out)
        return;
    std::fclose(g_Out);
    g_Out = nullptr;
    LOG_INFO("Duel recorder: '%s' closed after %u frames (%s)", g_Name.c_str(), g_Frame, why);
}

} // namespace

void StartDuelRecorder(const std::string& name, uint32_t frames)
{
    Close("a new recording");
    if (!frames)
        return;
    CreateDirectoryW((ExeDir() + L"\\netplay").c_str(), nullptr);
    const std::wstring path = ExeDir() + L"\\netplay\\duel-" + std::wstring(name.begin(), name.end()) + L".bin";
    g_Out = _wfopen(path.c_str(), L"wb");
    if (!g_Out) {
        LOG_WARN("Duel recorder: cannot write %ls", path.c_str());
        return;
    }
    const uint32_t header[3] = { 0x52445753, 1, kObjectBytes }; // "SWDR"
    std::fwrite(header, sizeof(header), 1, g_Out);
    g_Name = name;
    g_Left = frames;
    g_Frame = 0;
    LOG_INFO("Duel recorder: recording %u frames as %ls", frames, path.c_str());
}

std::string DuelRecorderStatus()
{
    char line[160];
    std::snprintf(line, sizeof(line), g_Out ? "duel recorder: '%s', %u frames written, %u to go" : "duel recorder: off",
        g_Name.c_str(), g_Frame, g_Left);
    return line;
}

void DuelRecorderFrame()
{
    if (!g_Out)
        return;
    uint8_t* p1 = game::PlayerObject();
    uint8_t* p2 = game::GamePlayer2();
    if (!p2 && p1) { // the developer map's duel: its opponent is not the game's player 2; the other character
        static uint32_t scan = 0;
        static uint8_t* other = nullptr;
        if (++scan % 30 == 1 || !other) {
            other = nullptr;
            for (uint8_t* c : game::LevelCharacters())
                if (c != p1) {
                    other = c;
                    break;
                }
        }
        p2 = other;
    }
    if (!p1 || !p2)
        return; // not in a duel now (loading, the round's end): only frames with both fighters
    static std::vector<uint8_t> a(kObjectBytes), b(kObjectBytes);
    ReadObject(p1, a.data());
    ReadObject(p2, b.data());
    input::GamePad pads[2] = {};
    input::ReadLivePad(0, pads[0]);
    input::ReadLivePad(1, pads[1]);
    const uint32_t head[3] = { g_Frame, uint32_t(uintptr_t(p1)), uint32_t(uintptr_t(p2)) };
    std::fwrite(head, sizeof(head), 1, g_Out);
    std::fwrite(pads, sizeof(pads), 1, g_Out);
    std::fwrite(a.data(), kObjectBytes, 1, g_Out);
    std::fwrite(b.data(), kObjectBytes, 1, g_Out);
    ++g_Frame;
    if (g_Frame % 300 == 0)
        std::fflush(g_Out);
    if (--g_Left == 0)
        Close("done");
}

} // namespace swrots::net
