// Online play's notices (see onlinehud.h).

#include "debug/onlinehud.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cfloat>
#include <string>

#include "debug/gamefont.h"
#include "game/characters.h"
#include "game/coop.h"
#include "game/devmap.h"
#include "game/freecam.h"
#include "game/hvvmenu.h"
#include "game/menus.h"
#include "imgui.h"
#include "net/session.h"

namespace swrots::debug {

namespace {

constexpr ImU32 kWhite = IM_COL32(235, 235, 235, 255);
constexpr ImU32 kDim = IM_COL32(200, 200, 210, 200);
constexpr ImU32 kGold = IM_COL32(255, 196, 90, 255);
constexpr ImU32 kRed = IM_COL32(255, 85, 70, 255);
constexpr ImU32 kPanel = IM_COL32(10, 12, 18, 175);
constexpr ULONGLONG kEndShownMs = 7000;

ImDrawList* g_Draw = nullptr;

std::string Upper(std::string t)
{
    for (char& c : t)
        if (c >= 'a' && c <= 'z')
            c = char(c - 'a' + 'A');
    return t;
}

float Width(float h, const std::string& t)
{
    if (GameFontReady())
        return GameFontWidth(h, t);
    return ImGui::GetFont()->CalcTextSizeA(h, FLT_MAX, 0, t.c_str()).x;
}

void Text(float x, float y, float h, ImU32 colour, const std::string& text, float align)
{
    const std::string t = Upper(text);
    const ImVec2 at(x - Width(h, t) * align, y);
    if (!GameFontText(g_Draw, at, h, colour, t)) {
        g_Draw->AddText(ImGui::GetFont(), h, ImVec2(at.x + 1, at.y + 1), IM_COL32(0, 0, 0, colour >> 24), t.c_str());
        g_Draw->AddText(ImGui::GetFont(), h, at, colour, t.c_str());
    }
}

// A banner across the screen at `y` (a fraction of its height): a title and a line under it.
void Banner(const ImGuiViewport* vp, float u, float y, ImU32 colour, const std::string& title, const std::string& line)
{
    const float cx = vp->Pos.x + vp->Size.x / 2, top = vp->Pos.y + vp->Size.y * y;
    const float h1 = 28 * u, h2 = 18 * u, pad = 10 * u;
    const float w = std::max(Width(h1, Upper(title)), Width(h2, Upper(line))) + 4 * pad;
    g_Draw->AddRectFilled(ImVec2(cx - w / 2, top - pad), ImVec2(cx + w / 2, top + h1 + h2 + 2 * pad), kPanel, 6 * u);
    Text(cx, top, h1, colour, title, 0.5f);
    if (!line.empty())
        Text(cx, top + h1 + pad * 0.6f, h2, kWhite, line, 0.5f);
}

// A point of the world on the screen (as Heroes vs Villains' screen does, hvvhud.cpp).
bool OnScreen(const float p[3], const ImGuiViewport* vp, ImVec2& out, float& depth)
{
    float m[16], fov = 0;
    float x, y, z;
    if (game::GameRenderedView(m, fov) && fov > 0) {
        x = p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12];
        y = p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13];
        z = p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + m[14];
    } else {
        if (!game::GameCameraPlacement(m, fov) || fov <= 0)
            return false;
        const float d[3] = { p[0] - m[12], p[1] - m[13], p[2] - m[14] };
        x = d[0] * m[0] + d[1] * m[1] + d[2] * m[2];
        y = d[0] * m[4] + d[1] * m[5] + d[2] * m[6];
        z = d[0] * m[8] + d[1] * m[9] + d[2] * m[10];
    }
    if (fov > 3.2f)
        fov *= 3.14159265f / 180.0f;
    if (z < 10.0f)
        return false;
    depth = z;
    const float focal = (vp->Size.x / 2) / std::tan(fov / 2);
    const float focalY = game::BootIsVersusDuel() ? (vp->Size.y / 2) / std::tan(fov / 2) : focal;
    out = ImVec2(vp->Pos.x + vp->Size.x / 2 + x / z * focal, vp->Pos.y + vp->Size.y / 2 - y / z * focalY);
    return true;
}

// Over the other player's character (the host's view: player 2's; the guest's: player 1's): their name and health.
constexpr uint32_t kCharacterDead = 0x12C, kCharacterHealth = 0x130, kCharacterMaxHealth = 0x134,
    kCharacterPosition = 0x150 + 48;
constexpr ImU32 kAlly = IM_COL32(110, 175, 255, 255);

void OtherPlayer(const net::OnlineStatusInfo& s, const ImGuiViewport* vp, float u)
{
    uint8_t* other = s.host ? game::GamePlayer2() : game::GamePlayerSlot(0);
    if (!other && !s.host)
        other = game::PlayerObject();
    if (!other || other[kCharacterDead])
        return;
    const float health = *reinterpret_cast<const float*>(other + kCharacterHealth);
    const float maxHealth = *reinterpret_cast<const float*>(other + kCharacterMaxHealth);
    if (health <= 0)
        return;
    const float* feet = reinterpret_cast<const float*>(other + kCharacterPosition);
    const float head[3] = { feet[0], feet[1] + 86.0f, feet[2] };
    ImVec2 at;
    float depth = 0;
    if (!OnScreen(head, vp, at, depth) || depth > 2500.0f)
        return;
    const float alpha = std::clamp((2500.0f - depth) / 700.0f, 0.0f, 1.0f) * 0.9f;
    const float k = std::clamp(700.0f / std::max(depth, 1.0f), 0.65f, 1.0f);
    const float w = 58 * u * k, bar = 4 * u * k, top = at.y - 14 * u * k;
    const auto faded = [alpha](ImU32 c) { return (c & 0x00FFFFFF) | (ImU32(float(c >> 24) * alpha) << 24); };
    const float x0 = at.x - w / 2;
    const float part = maxHealth > 0 ? std::clamp(health / maxHealth, 0.0f, 1.0f) : 1.0f;
    g_Draw->AddRectFilled(ImVec2(x0 - 1, top - 1), ImVec2(x0 + w + 1, top + bar + 1), IM_COL32(0, 0, 0, int(160 * alpha)));
    g_Draw->AddRectFilled(ImVec2(x0, top), ImVec2(x0 + w * part, top + bar), faded(kAlly));
    const float th = 12 * u * k;
    Text(at.x, top - th - 3 * u, th, faded(kWhite), s.remoteName, 0.5f);
}

// A Versus duel: each player's name over their health bar (the game's own bars, at the bottom: player 1's on the
// left, player 2's on the right), in the game's font.
void VersusNames(const net::OnlineStatusInfo& s, const ImGuiViewport* vp, float u)
{
    if (!game::GamePlayerSlot(0) || !game::GamePlayerSlot(1))
        return;
    const std::string p1 = s.host ? s.localName : s.remoteName, p2 = s.host ? s.remoteName : s.localName;
    // in the space above each bar (player 1's from 17% of the width, player 2's up to 83.6%)
    const float h = 27 * u, y = vp->Pos.y + vp->Size.y * 0.806f;
    const auto name = [&](float x, const std::string& text, float align) {
        const std::string t = Upper(text);
        const ImVec2 at(x - GameFontWidthOf(GameFont::Menu, h, t) * align, y);
        if (!GameFontTextOf(GameFont::Menu, g_Draw, at, h, kWhite, t))
            Text(x, y, h, kWhite, text, align);
    };
    name(vp->Pos.x + vp->Size.x * 0.170f, p1, 0.0f);
    name(vp->Pos.x + vp->Size.x * 0.836f, p2, 1.0f);
}

// From the match's start to its room: both games restart (the host's profile, no mods) and come up at their main
// menu before the room; that is covered, with a line in the menus' font.
void EnteringCover(const net::OnlineStatusInfo& s, const ImGuiViewport* vp, float u)
{
    g_Draw->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), IM_COL32(0, 0, 0, 255));
    const int dots = int(GetTickCount64() / 400 % 4);
    const std::string line = Upper(s.leaving ? "Leaving the room" : s.host ? "Opening the room" : "Joining the room");
    const float h = 30 * u, cx = vp->Pos.x + vp->Size.x / 2, y = vp->Pos.y + vp->Size.y * 0.47f;
    const float w = GameFontWidthOf(GameFont::Menu, h, line);
    const ImVec2 at(cx - w / 2, y);
    if (GameFontTextOf(GameFont::Menu, g_Draw, at, h, kWhite, line + std::string(size_t(dots), '.')))
        return;
    Text(cx, y, h, kWhite, line + std::string(size_t(dots), '.'), 0.5f);
}

} // namespace

bool OnlineHudWanted()
{
    return net::OnlineStatus().phase != net::OnlinePhase::Off;
}

void DrawOnlineHud()
{
    static net::OnlinePhase last = net::OnlinePhase::Off;
    static ULONGLONG endedAt = 0;
    static std::string endedWhy;
    const net::OnlineStatusInfo s = net::OnlineStatus();
    if (s.phase != last) {
        if (s.phase == net::OnlinePhase::Ended && last != net::OnlinePhase::Off) {
            endedAt = GetTickCount64();
            endedWhy = s.message;
        }
        last = s.phase;
    }
    g_Draw = ImGui::GetForegroundDrawList();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float u = std::max(0.5f, vp->Size.y / 720.0f);
    if (s.entering || s.leaving) {
        EnteringCover(s, vp, u);
        return;
    }
    switch (s.phase) {
    case net::OnlinePhase::Preparing:
        Banner(vp, u, 0.12f, kGold, "Online", s.host ? "Sending the profile to your friend..." : "Getting the host's profile...");
        break;
    case net::OnlinePhase::Starting:
        Banner(vp, u, 0.12f, kGold, "Online", "Starting the match...");
        break;
    case net::OnlinePhase::Playing: {
        if (s.pingMs >= 0)
            Text(vp->Pos.x + vp->Size.x - 16 * u, vp->Pos.y + 10 * u, 14 * u, kDim, std::to_string(s.pingMs) + " ms", 1.0f);
        if (game::BootIsVersusDuel())
            VersusNames(s, vp, u);
        else if (!s.remoteName.empty())
            OtherPlayer(s, vp, u);
        if (s.desync)
            Banner(vp, u, 0.12f, kRed, "Desync", "The two games no longer match: leave and start again");
        break;
    }
    default:
        break;
    }
    (void)endedAt; // a lost session: the game's own popup (onlinemenu.cpp)
}

} // namespace swrots::debug
