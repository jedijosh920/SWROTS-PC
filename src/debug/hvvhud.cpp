// Heroes vs Villains' screen (see hvvhud.h).

#include "debug/hvvhud.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "debug/console.h"
#include "core/settings.h"
#include "debug/gamefont.h"
#include "game/cody.h"
#include "game/devmap.h"
#include "game/freecam.h"
#include "game/hvv.h"
#include "game/hvvmenu.h"
#include "imgui.h"
#include "input/controls.h"

namespace swrots::debug {

namespace {

constexpr ImU32 kHeroes = IM_COL32(110, 175, 255, 255);
constexpr ImU32 kVillains = IM_COL32(255, 85, 70, 255);
constexpr ImU32 kGold = IM_COL32(255, 196, 90, 255);
constexpr ImU32 kWhite = IM_COL32(235, 235, 235, 255);
constexpr ImU32 kDim = IM_COL32(110, 110, 110, 220);
constexpr ImU32 kPanel = IM_COL32(10, 12, 18, 175);
constexpr ImU32 kEdge = IM_COL32(200, 200, 210, 110);

ImU32 SideColour(int side) { return side == 0 ? kHeroes : side == 1 ? kVillains : kWhite; }

// The sides' names: the heroes and villains, or Battlefront's republic and separatists.
const char* SideName(const game::HvvState& s, int side)
{
    if (s.conquest)
        return side == 0 ? "REPUBLIC" : "SEPARATISTS";
    return side == 0 ? "HEROES" : "VILLAINS";
}

const char* WinText(const game::HvvState& s)
{
    if (s.conquest)
        return s.winner == 0 ? "REPUBLIC WINS" : "SEPARATISTS WIN";
    return s.winner == 0 ? "HEROES WIN" : "VILLAINS WIN";
}

ImU32 Faded(ImU32 colour, float alpha)
{
    const ImU32 a = ImU32(float(colour >> 24) * std::clamp(alpha, 0.0f, 1.0f));
    return (colour & 0x00FFFFFF) | (a << 24);
}

ImDrawList* g_Draw = nullptr;

float Width(float h, const std::string& t)
{
    if (GameFontReady())
        return GameFontWidth(h, t);
    return ImGui::GetFont()->CalcTextSizeA(h, FLT_MAX, 0, t.c_str()).x;
}

// Text at (x, y: its top), aligned (0 left, 0.5 middle, 1 right); its width.
float Text(float x, float y, float h, ImU32 colour, const std::string& t, float align = 0)
{
    const float w = Width(h, t);
    const ImVec2 at(x - w * align, y);
    if (!GameFontText(g_Draw, at, h, colour, t)) {
        g_Draw->AddText(ImGui::GetFont(), h, ImVec2(at.x + 1, at.y + 1), IM_COL32(0, 0, 0, colour >> 24), t.c_str());
        g_Draw->AddText(ImGui::GetFont(), h, at, colour, t.c_str());
    }
    return w;
}

// A point of the world on the screen, through the game camera's placement (rows right, up, forward, position) and
// field of view (the renderer's: horizontal, radians; taken as vertical, points off the middle came out 16:9 times
// too near it).
bool OnScreen(const float p[3], const ImGuiViewport* vp, ImVec2& out, float* depth = nullptr)
{
    float m[16], fov = 0;
    float x, y, z;
    if (game::GameRenderedView(m, fov) && fov > 0) { // the view as drawn (row vectors: p * view)
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
    if (depth)
        *depth = z;
    // The field of view spans the picture's width, or with true widescreen the game's 4:3 frame in its middle.
    const float span = TrueWidescreen() ? vp->Size.y * 4.0f / 3.0f : vp->Size.x;
    const float focal = (span / 2) / std::tan(fov / 2);
    // a Versus duel's camera spans the field of view across the height too (measured: its picture is stretched to the
    // screen's shape); the dev map's (Heroes vs Villains) has square pixels
    const float focalY = game::BootIsVersusDuel() ? (vp->Size.y / 2) / std::tan(fov / 2) : focal;
    out = ImVec2(vp->Pos.x + vp->Size.x / 2 + x / z * focal, vp->Pos.y + vp->Size.y / 2 - y / z * focalY);
    return true;
}

void Scoreboard(const game::HvvState& s, const ImGuiViewport* vp, float u)
{
    const float h = 26 * u, middle = vp->Pos.x + vp->Size.x / 2, top = vp->Pos.y + 14 * u;
    char line[32];
    for (int side = 0; side < 2; ++side) {
        const float sign = side == 0 ? -1.0f : 1.0f;
        snprintf(line, sizeof line, "%d", s.lives[side]);
        Text(middle + sign * 18 * u, top, h, SideColour(side), line, side == 0 ? 1.0f : 0.0f);
        Text(middle + sign * 70 * u, top + 6 * u, h * 0.6f, Faded(SideColour(side), 0.85f),
            SideName(s, side), side == 0 ? 1.0f : 0.0f);
        if (s.conquest) // tickets, no dots: the command posts under them
            continue;
        const int from = side == 0 ? 0 : s.heroes, to = side == 0 ? s.heroes : int(s.fighters.size());
        for (int k = 0; k < to - from; ++k) {
            const auto& f = s.fighters[size_t(from + k)];
            const ImVec2 at(middle + sign * (24 + k * 16) * u, top + h + 9 * u);
            g_Draw->AddCircleFilled(at, 5 * u, f.standing || !s.started ? SideColour(side) : kDim);
            if (f.isPlayer)
                g_Draw->AddCircle(at, 8 * u, IM_COL32(255, 255, 255, 230), 0, 1.5f * u);
        }
    }
    g_Draw->AddLine(ImVec2(middle, top), ImVec2(middle, top + h), IM_COL32(230, 230, 230, 200), 2 * u);
}

// Over a fighter's head (as Battlefront's, small): its side's mark, its name, its health and its Force, fading out
// with distance; for everyone or the allies only, as settings.ini's [HvV] OverheadInfo says.
void OverheadInfo(const game::HvvState& s, const ImGuiViewport* vp, float u)
{
    const int show = GetSettings().hvvOverheadInfo;
    if (!s.fighting || show == 0)
        return;
    const int mySide = s.fighters[size_t(s.player)].side;
    for (const auto& f : s.fighters) {
        if (f.isPlayer || !f.standing || !f.placed || (show == 1 && f.side != mySide))
            continue;
        ImVec2 at;
        float depth = 0;
        if (!OnScreen(f.head, vp, at, &depth) || depth > 2200.0f)
            continue;
        // an enemy's only while in sight (an ally's through walls too, as the markers)
        float m[16], fov = 0;
        if (f.side != mySide && game::GameCameraPlacement(m, fov) && !game::HvvLineOfSight(m + 12, f.head))
            continue;
        const float alpha = std::clamp((2200.0f - depth) / 700.0f, 0.0f, 1.0f) * 0.9f;
        const float k = std::clamp(700.0f / std::max(depth, 1.0f), 0.65f, 1.0f); // a little smaller far away
        const float w = 54 * u * k, bar = 3.5f * u * k;
        const float top = at.y - (f.side == mySide ? 30 : 20) * u * k; // above the allies' chevrons
        const ImU32 colour = SideColour(f.side);
        // the bars: health (the side's colour), Force under it (blue-white)
        const float x0 = at.x - w / 2;
        float y = top;
        g_Draw->AddRectFilled(ImVec2(x0 - 1, y - 1), ImVec2(x0 + w + 1, y + bar + 1), IM_COL32(0, 0, 0, int(160 * alpha)));
        g_Draw->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + w * f.health, y + bar), Faded(colour, alpha));
        if (f.force >= 0) {
            y += bar + 1.5f * u;
            const float fb = bar * 0.6f;
            g_Draw->AddRectFilled(ImVec2(x0 - 1, y - 1), ImVec2(x0 + w + 1, y + fb + 1), IM_COL32(0, 0, 0, int(160 * alpha)));
            g_Draw->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + w * f.force, y + fb), Faded(IM_COL32(150, 200, 255, 255), alpha));
        }
        // the name, with the side's mark before it
        const float th = 11 * u * k;
        const float nameW = Width(th, f.name), mark = 3 * u * k;
        const float nx = at.x - (nameW + 3 * mark) / 2;
        g_Draw->AddCircleFilled(ImVec2(nx + mark, top - th * 0.55f - 2 * u), mark, Faded(colour, alpha));
        Text(nx + 3 * mark, top - th - 3 * u, th, Faded(kWhite, alpha), f.name);
    }
}

// Over each teammate standing: a small marker of the side's colour (as Battlefront's).
void Markers(const game::HvvState& s, const ImGuiViewport* vp, float u)
{
    if (!s.fighting)
        return;
    const int mySide = s.fighters[size_t(s.player)].side;
    for (const auto& f : s.fighters) {
        if (f.isPlayer || !f.standing || !f.placed || f.side != mySide)
            continue;
        ImVec2 at;
        float depth = 0;
        if (!OnScreen(f.head, vp, at, &depth))
            continue;
        // a chevron pointing down at the head, outlined, a little smaller far away
        const float r = std::clamp(9.0f * u * (600.0f / std::max(depth, 1.0f)), 6.0f * u, 10.0f * u);
        const ImVec2 a(at.x - r, at.y - 1.6f * r), b(at.x + r, at.y - 1.6f * r), c(at.x, at.y);
        g_Draw->AddTriangleFilled(a, b, c, SideColour(mySide));
        g_Draw->AddTriangle(a, b, c, IM_COL32(0, 0, 0, 200), 1.5f * u);
    }
}

void KillFeed(const game::HvvState& s, const ImGuiViewport* vp, float u)
{
    const float h = 15 * u, right = vp->Pos.x + vp->Size.x - 18 * u;
    float y = vp->Pos.y + 16 * u;
    for (const auto& line : s.feed) {
        const float alpha = std::min(1.0f, (7.0f - line.age) / 1.0f);
        float x = right;
        x -= Text(x, y, h, Faded(SideColour(line.victimSide), alpha), line.victim, 1.0f) + 10 * u;
        if (!line.killer.empty()) {
            // a small blade between the two
            g_Draw->AddLine(ImVec2(x - 18 * u, y + h * 0.5f), ImVec2(x - 2 * u, y + h * 0.5f),
                Faded(kWhite, alpha), 2 * u);
            x -= 26 * u;
            Text(x, y, h, Faded(SideColour(line.killerSide), alpha), line.killer, 1.0f);
        } else {
            Text(x, y, h, Faded(kDim, alpha), "FELL", 1.0f);
        }
        if (line.mine)
            g_Draw->AddRectFilled(ImVec2(right + 6 * u, y), ImVec2(right + 9 * u, y + h), Faded(kGold, alpha));
        y += h + 7 * u;
    }
}

// The player's notice: "DEFEATED <NAME>" with a killstreak, "ASSIST <NAME>", "DEFEATED BY <NAME>".
void Notice(const game::HvvState& s, const ImGuiViewport* vp, float u)
{
    if (s.noticeAge < 0)
        return;
    const float alpha = std::min({ 1.0f, s.noticeAge / 0.15f, (3.5f - s.noticeAge) / 0.5f });
    const float h = 24 * u, middle = vp->Pos.x + vp->Size.x / 2, y = vp->Pos.y + vp->Size.y * 0.62f;
    std::string lead = s.notice, name;
    // "DEFEATED NAME..." / "ASSIST NAME": the verb white, the name in its side's colour
    const char* verbs[] = { "DEFEATED BY ", "DEFEATED ", "ASSIST " };
    for (const char* v : verbs) {
        if (s.notice.rfind(v, 0) == 0) {
            lead = v;
            name = s.notice.substr(strlen(v));
            break;
        }
    }
    const float total = Width(h, lead) + Width(h, name);
    float x = middle - total / 2;
    x += Text(x, y, h, Faded(kWhite, alpha), lead);
    Text(x, y, h, Faded(SideColour(s.noticeSide), alpha), name);
    float below = y + h + 6 * u;
    if (!s.noticeSub.empty()) {
        Text(middle, below, h * 0.62f, Faded(kGold, alpha), s.noticeSub, 0.5f);
        below += h * 0.62f + 5 * u;
    }
    if (s.noticePoints > 0) {
        char points[32];
        snprintf(points, sizeof points, "+%d", s.noticePoints);
        Text(middle, below, h * 0.55f, Faded(kWhite, alpha * 0.85f), points, 0.5f);
    }
}

void Captions(const game::HvvState& s, const ImGuiViewport* vp, float u)
{
    if (s.caption.empty())
        return;
    const float alpha = std::min({ 1.0f, s.captionFade / 0.12f, (1.0f - s.captionFade) / 0.12f });
    const float middle = vp->Pos.x + vp->Size.x / 2;
    if (s.caption == "FIGHT!") {
        Text(middle, vp->Pos.y + vp->Size.y * 0.3f, 64 * u, Faded(kGold, alpha), s.caption, 0.5f);
        return;
    }
    const float y = vp->Pos.y + vp->Size.y * 0.76f;
    Text(middle, y, 40 * u, Faded(SideColour(s.captionSide), alpha), s.caption, 0.5f);
    if (!s.subcaption.empty())
        Text(middle, y + 48 * u, 16 * u, Faded(kWhite, alpha), s.subcaption, 0.5f);
}

void Leaderboard(const game::HvvState& s, const ImGuiViewport* vp, float u)
{
    const float rowH = 22 * u, colW = 330 * u, gap = 24 * u;
    const float width = 2 * colW + gap, left = vp->Pos.x + (vp->Size.x - width) / 2;
    const float top = vp->Pos.y + vp->Size.y * 0.2f;
    const int rows = std::max(s.heroes, int(s.fighters.size()) - s.heroes);
    g_Draw->AddRectFilled(ImVec2(left - 14 * u, top - 14 * u), ImVec2(left + width + 14 * u, top + (rows + 3) * rowH + 6 * u),
        kPanel, 6 * u);
    for (int side = 0; side < 2; ++side) {
        const float x = left + side * (colW + gap);
        Text(x, top, 22 * u, SideColour(side), SideName(s, side));
        const float cols[] = { x + colW * 0.56f, x + colW * 0.70f, x + colW * 0.84f, x + colW };
        const char* heads[] = { "D", "A", "F", "SCORE" };
        for (int c = 0; c < 4; ++c)
            Text(cols[c], top + 6 * u, 12 * u, kDim, heads[c], 1.0f);
        std::vector<int> order;
        for (int i = side == 0 ? 0 : s.heroes; i < (side == 0 ? s.heroes : int(s.fighters.size())); ++i)
            order.push_back(i);
        std::sort(order.begin(), order.end(), [&](int a, int b) { return s.fighters[a].score > s.fighters[b].score; });
        float y = top + 1.6f * rowH;
        for (int i : order) {
            const auto& f = s.fighters[i];
            if (f.isPlayer)
                g_Draw->AddRectFilled(ImVec2(x - 6 * u, y - 3 * u), ImVec2(x + colW + 6 * u, y + rowH - 4 * u),
                    IM_COL32(255, 196, 90, 40), 3 * u);
            const ImU32 colour = f.out ? kDim : kWhite;
            Text(x, y, 15 * u, colour, f.isPlayer ? f.name + "  (YOU)" : f.name);
            const int values[] = { f.defeats, f.assists, f.falls, f.score };
            for (int c = 0; c < 4; ++c)
                Text(cols[c], y, 15 * u, c == 3 ? kGold : colour, std::to_string(values[c]), 1.0f);
            y += rowH;
        }
    }
}

// The end: the winner, and a card under each of the top five (as Battlefront's end screen).
void EndScreen(const game::HvvState& s, const ImGuiViewport* vp, float u)
{
    const float alpha = std::min(1.0f, s.endingAge / 0.8f);
    const float middle = vp->Pos.x + vp->Size.x / 2;
    if (s.winner >= 0)
        Text(middle, vp->Pos.y + vp->Size.y * 0.08f, 40 * u, Faded(SideColour(s.winner), alpha), WinText(s), 0.5f);
    static const char* kRanks[] = { "FIRST", "SECOND", "THIRD", "FOURTH", "FIFTH" };
    static const float kAt[5][2] = { { 0.5f, 0.68f }, { 0.30f, 0.62f }, { 0.70f, 0.62f }, { 0.11f, 0.70f },
        { 0.89f, 0.70f } };
    ImVec4 cards[5] = {}; // each card's left, top, right, bottom
    for (size_t r = 0; r < s.top.size() && r < 5; ++r) {
        const auto& f = s.fighters[s.top[r]];
        // the top three's cards stand out; fourth and fifth (further back) get smaller ones
        static const float kSize[5][3] = { { 200, 96, 26 }, { 175, 84, 21 }, { 175, 84, 21 }, { 132, 64, 15 },
            { 132, 64, 15 } }; // width, height, rank text
        const float w = kSize[r][0] * u, h = kSize[r][1] * u;
        // under the fighter's feet where it stands on the podium (else a place of its rank)
        float cx = vp->Pos.x + vp->Size.x * kAt[r][0], y = vp->Pos.y + vp->Size.y * kAt[r][1];
        ImVec2 at;
        if (f.placed && OnScreen(f.feet, vp, at)) {
            cx = std::clamp(at.x, vp->Pos.x + w / 2 + 4 * u, vp->Pos.x + vp->Size.x - w / 2 - 4 * u);
            y = std::min(at.y + 8 * u, vp->Pos.y + vp->Size.y - h - 8 * u);
        }
        if (r >= 3) { // fourth and fifth (the back row) stand behind second and third: their cards go outside of those
            const ImVec4& n = cards[r - 2];
            const float lo = vp->Pos.x + w / 2 + 4 * u, hi = vp->Pos.x + vp->Size.x - w / 2 - 4 * u;
            cx = std::clamp(r == 3 ? std::min(cx, n.x - 6 * u - w / 2) : std::max(cx, n.z + 6 * u + w / 2), lo, hi);
            if (cx - w / 2 < n.z && cx + w / 2 > n.x && y < n.w && y + h > n.y) // no room beside it: above it
                y = n.y - h - 6 * u;
        }
        cards[r] = ImVec4(cx - w / 2, y, cx + w / 2, y + h);
        const float a = std::clamp((s.endingAge - 0.4f - 0.25f * float(r)) / 0.5f, 0.0f, 1.0f);
        g_Draw->AddRectFilled(ImVec2(cx - w / 2, y), ImVec2(cx + w / 2, y + h), Faded(kPanel, a * 1.3f), 4 * u);
        g_Draw->AddRect(ImVec2(cx - w / 2, y), ImVec2(cx + w / 2, y + h), Faded(kEdge, a), 4 * u, 0, 1.5f * u);
        const float big = kSize[r][2] * u;
        Text(cx, y + 6 * u, big, Faded(kGold, a), kRanks[r], 0.5f);
        g_Draw->AddRectFilled(ImVec2(cx - w / 2 + 6 * u, y + 10 * u + big), ImVec2(cx + w / 2 - 6 * u, y + 30 * u + big),
            Faded(IM_COL32(255, 255, 255, 18), a));
        Text(cx, y + 12 * u + big, (r < 3 ? 15 : 13) * u, Faded(SideColour(f.side), a),
            game::HvvDisplayName(f.className), 0.5f);
        if (f.isPlayer)
            Text(cx, y - 18 * u, 13 * u, Faded(kWhite, a), "YOU", 0.5f);
        char score[32];
        snprintf(score, sizeof score, "%d SCORE", f.score);
        Text(cx, y + h - (r < 3 ? 22 : 19) * u, (r < 3 ? 16 : 13) * u, Faded(kGold, a), score, 0.5f);
    }
}

// Battlefront's command posts: a row of their letters under the tickets (each in its holder's colour, a bar under it
// filling as a side turns it), and a marker where each stands in the world (its letter in a diamond).
ImU32 PostColour(int owner) { return owner == 0 ? kHeroes : owner == 1 ? kVillains : IM_COL32(200, 200, 200, 255); }

void PostRow(const game::HvvState& s, const ImGuiViewport* vp, float u)
{
    const float size = 22 * u, gap = 8 * u, top = vp->Pos.y + 58 * u;
    const float width = float(s.posts.size()) * size + float(s.posts.size() - 1) * gap;
    float x = vp->Pos.x + (vp->Size.x - width) / 2;
    for (const auto& post : s.posts) {
        const ImU32 colour = PostColour(post.owner);
        g_Draw->AddRectFilled(ImVec2(x, top), ImVec2(x + size, top + size), IM_COL32(10, 12, 18, 170), 3 * u);
        g_Draw->AddRect(ImVec2(x, top), ImVec2(x + size, top + size), colour, 3 * u, 0, 2 * u);
        Text(x + size / 2, top + 3 * u, size - 6 * u, colour, post.name, 0.5f);
        // how far it is turned: towards the clones (left, blue) or the droids (right, red)
        const float hold = std::abs(post.hold);
        if (hold > 0.01f && hold < 0.99f)
            g_Draw->AddRectFilled(ImVec2(x, top + size + 3 * u), ImVec2(x + size * hold, top + size + 6 * u),
                PostColour(post.hold < 0 ? 0 : 1));
        x += size + gap;
    }
}

void PostMarkers(const game::HvvState& s, const ImGuiViewport* vp, float u)
{
    for (const auto& post : s.posts) {
        const float at3[3] = { post.at[0], post.at[1] + 130.0f, post.at[2] };
        ImVec2 at;
        float depth = 0;
        if (!OnScreen(at3, vp, at, &depth))
            continue;
        const float r = std::clamp(14.0f * u * (900.0f / std::max(depth, 1.0f)), 9.0f * u, 16.0f * u);
        const ImU32 colour = PostColour(post.owner);
        const ImVec2 n(at.x, at.y - r), e(at.x + r, at.y), so(at.x, at.y + r), w(at.x - r, at.y);
        g_Draw->AddQuadFilled(n, e, so, w, IM_COL32(10, 12, 18, 170));
        g_Draw->AddQuad(n, e, so, w, colour, 2.5f * u);
        Text(at.x, at.y - r * 0.55f, r * 1.1f, colour, post.name, 0.5f);
        const float hold = std::abs(post.hold);
        if (hold > 0.01f && hold < 0.99f) // being turned: a bar under it
            g_Draw->AddRectFilled(ImVec2(at.x - r, at.y + r + 3 * u), ImVec2(at.x - r + 2 * r * hold, at.y + r + 6 * u),
                PostColour(post.hold < 0 ? 0 : 1));
        if (depth > 300.0f) {
            char metres[16];
            snprintf(metres, sizeof metres, "%dM", int(depth / 36.0f));
            Text(at.x, at.y + r + 8 * u, 11 * u, Faded(kWhite, 0.8f), metres, 0.5f);
        }
    }
}

// The player's hero: its points towards one, then "HERO READY" with the button (B, or the key bound to it).
void HeroPrompt(const game::HvvState& s, const ImGuiViewport* vp, float u)
{
    if (s.playerDown)
        return;
    const float middle = vp->Pos.x + vp->Size.x / 2, y = vp->Pos.y + vp->Size.y * 0.86f;
    if (s.heroReady) {
        std::string key = "B";
        if (input::KeyboardPrompts()) {
            const std::vector<int> keys = input::ActionKeys(L"CriticalAttack");
            char name[64] = {};
            if (!keys.empty() && GetKeyNameTextA(LONG(MapVirtualKeyA(UINT(keys[0]), MAPVK_VK_TO_VSC)) << 16, name, sizeof name) > 0)
                key = name;
        }
        for (char& c : key)
            c = char(toupper(static_cast<unsigned char>(c)));
        const float pulse = 0.75f + 0.25f * std::sin(float(GetTickCount64() % 100000) / 180.0f);
        Text(middle, y, 22 * u, Faded(kGold, pulse), "HERO READY  -  PRESS " + key, 0.5f);
        return;
    }
    if (s.heroCost > 0) {
        char line[48];
        snprintf(line, sizeof line, "HERO  %d / %d", std::min(s.heroPoints, s.heroCost), s.heroCost);
        Text(middle, y + 6 * u, 13 * u, Faded(kWhite, 0.6f), line, 0.5f);
    }
}

bool HelperStanding(const uint8_t* object)
{
    __try {
        return *reinterpret_cast<const float*>(object + 0x130) > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Over each of Commander Cody's clones and the droid commander's droids (cody.h), in Versus and Heroes vs Villains: a
// chevron and their mark ("CODY'S CLONE"), blue when they fight for the player (their master is the player's), red
// against.
void CodyMarks(const ImGuiViewport* vp, float u)
{
    for (const game::CodyHelper& h : game::CodyHelpers()) {
        if (!HelperStanding(h.object))
            continue;
        const ImU32 colour = h.enemy ? kVillains : kHeroes;
        const float* p = reinterpret_cast<const float*>(h.object + 0x150 + 48);
        const float head[3] = { p[0], p[1] + 82.0f, p[2] };
        ImVec2 at;
        float depth = 0;
        if (!OnScreen(head, vp, at, &depth) || depth > 2500.0f)
            continue;
        const float alpha = std::clamp((2500.0f - depth) / 700.0f, 0.0f, 1.0f) * 0.9f;
        const float k = std::clamp(700.0f / std::max(depth, 1.0f), 0.65f, 1.0f);
        const float r = 8.0f * u * k;
        const ImVec2 a(at.x - r, at.y - 1.6f * r), b(at.x + r, at.y - 1.6f * r), c(at.x, at.y);
        g_Draw->AddTriangleFilled(a, b, c, Faded(colour, alpha));
        g_Draw->AddTriangle(a, b, c, IM_COL32(0, 0, 0, int(200 * alpha)), 1.5f * u);
        Text(at.x, at.y - 1.6f * r - 17 * u * k, 14 * u * k, Faded(colour, alpha), h.label, 0.5f);
    }
}

bool TabHeld()
{
    if (!(GetAsyncKeyState(VK_TAB) & 0x8000))
        return false;
    DWORD process = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &process);
    return process == GetCurrentProcessId();
}

} // namespace

void DrawHvvHud()
{
    const game::HvvState s = game::GetHvvState();
    // the game's own HUD hidden for the end and while the player is down (its `hud` variable, the opacity), given
    // back after
    static bool hudHidden = false;
    const bool hide = s.active && (s.ending || s.playerDown || s.intro);
    if (hide != hudHidden) {
        hudHidden = hide;
        WriteGameVariable("hud", hudHidden ? "0" : "1");
    }
    g_Draw = ImGui::GetForegroundDrawList();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float u = std::max(0.5f, vp->Size.y / 720.0f);
    if (!game::GamePaused() && !(s.active && (s.ending || s.intro)))
        CodyMarks(vp, u);
    // before the fighters are there (the loading screen) and in the game's pause menu: nothing (Tab neither)
    if (!s.active || !s.started || s.paused)
        return;
    if (s.ending) {
        EndScreen(s, vp, u);
        return;
    }
    if (s.intro) { // the intro: its titles alone
        Captions(s, vp, u);
        return;
    }
    OverheadInfo(s, vp, u);
    Markers(s, vp, u);
    Scoreboard(s, vp, u);
    if (s.conquest) {
        PostMarkers(s, vp, u);
        PostRow(s, vp, u);
        HeroPrompt(s, vp, u);
    }
    KillFeed(s, vp, u);
    Captions(s, vp, u);
    Notice(s, vp, u);
    if (s.started && (s.board || TabHeld()))
        Leaderboard(s, vp, u);
    if (s.winner >= 0)
        Text(vp->Pos.x + vp->Size.x / 2, vp->Pos.y + vp->Size.y * 0.3f, 48 * u, SideColour(s.winner), WinText(s), 0.5f);
}

} // namespace swrots::debug
