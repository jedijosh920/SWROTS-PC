// Online play's notices (see onlinehud.h).

#include "debug/onlinehud.h"

#include <windows.h>

#include <algorithm>
#include <cfloat>
#include <string>

#include "debug/gamefont.h"
#include "imgui.h"
#include "net/session.h"

namespace swrots::debug {

namespace {

constexpr ImU32 kWhite = IM_COL32(235, 235, 235, 255);
constexpr ImU32 kDim = IM_COL32(200, 200, 210, 200);
constexpr ImU32 kGold = IM_COL32(255, 196, 90, 255);
constexpr ImU32 kRed = IM_COL32(255, 85, 70, 255);
constexpr ImU32 kPanel = IM_COL32(10, 12, 18, 175);
constexpr ULONGLONG kEndShownMs = 10000;

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

} // namespace

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
    switch (s.phase) {
    case net::OnlinePhase::Preparing:
        Banner(vp, u, 0.12f, kGold, "Online", s.host ? "Sending the profile to your friend..." : "Getting the host's profile...");
        break;
    case net::OnlinePhase::Starting:
        Banner(vp, u, 0.12f, kGold, "Online", "Starting the match...");
        break;
    case net::OnlinePhase::Playing: {
        std::string line = s.host ? "Online  P1 (host)" : "Online  P2";
        if (s.pingMs >= 0)
            line += "  " + std::to_string(s.pingMs) + " ms";
        Text(vp->Pos.x + vp->Size.x - 16 * u, vp->Pos.y + 10 * u, 14 * u, kDim, line, 1.0f);
        if (s.desync)
            Banner(vp, u, 0.12f, kRed, "Desync", "The two games no longer match: leave and start again");
        break;
    }
    default:
        break;
    }
    if (endedAt && GetTickCount64() - endedAt < kEndShownMs && s.phase == net::OnlinePhase::Ended)
        Banner(vp, u, 0.12f, kRed, "Online session over", endedWhy);
}

} // namespace swrots::debug
