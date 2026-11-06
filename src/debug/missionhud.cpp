// The screen of a mission of a mod's own (game/missions.h): cutscene bars, fades, the objective, subtitles, titles,
// notices, the timer and counters, and the end. In the game's own HUD font (gamefont.h), as Heroes vs Villains' screen.

#include "debug/missionhud.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include "imgui.h"

#include "debug/gamefont.h"
#include "game/missions.h"

namespace swrots::debug {
namespace {

constexpr ImU32 kWhite = IM_COL32(240, 240, 240, 255);
constexpr ImU32 kGold = IM_COL32(242, 193, 78, 255);
constexpr ImU32 kRed = IM_COL32(255, 90, 80, 255);
// the game's own messages (its tips, its subtitles): pale blue-white words, a blue for names, over the live view
constexpr ImU32 kGamePale = IM_COL32(222, 230, 240, 255);
constexpr ImU32 kGameBlue = IM_COL32(110, 170, 245, 255);
constexpr ImU32 kGameFailed = IM_COL32(225, 95, 80, 255);

ImU32 Faded(ImU32 colour, float alpha)
{
    const unsigned a = unsigned(((colour >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f));
    return (colour & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

// Text at (x, y): `anchor` 0 left, 0.5 centred, 1 right.
void Text(ImDrawList* draw, float x, float y, float height, ImU32 colour, const std::string& text, float anchor = 0)
{
    const float w = GameFontWidth(height, text);
    if (!GameFontText(draw, ImVec2(x - w * anchor, y), height, colour, text))
        draw->AddText(nullptr, height, ImVec2(x - w * anchor, y), colour, text.c_str());
}

// Text with the soft dark shadow the game's own messages have.
void ShadowedText(ImDrawList* draw, float x, float y, float height, ImU32 colour, float alpha, const std::string& text,
    float anchor = 0)
{
    const float o = std::max(1.0f, height * 0.06f);
    Text(draw, x + o, y + o, height, Faded(IM_COL32(0, 0, 0, 255), alpha * 0.6f), text, anchor);
    Text(draw, x, y, height, Faded(colour, alpha), text, anchor);
}

// A line split to fit `width` (the game font's width), at most a few lines.
std::vector<std::string> Wrap(const std::string& text, float height, float width)
{
    std::vector<std::string> lines;
    std::string line, word;
    auto flush = [&] {
        if (!word.empty()) {
            const std::string candidate = line.empty() ? word : line + " " + word;
            if (!line.empty() && GameFontWidth(height, candidate) > width) {
                lines.push_back(line);
                line = word;
            } else {
                line = candidate;
            }
            word.clear();
        }
    };
    for (char c : text) {
        if (c == ' ')
            flush();
        else
            word += c;
    }
    flush();
    if (!line.empty())
        lines.push_back(line);
    return lines;
}

} // namespace

void DrawMissionHud()
{
    const game::MissionHud h = game::GetMissionHud();
    if (!h.active || h.paused)
        return;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float u = std::max(0.5f, vp->Size.y / 720.0f);
    const float x0 = vp->Pos.x, y0 = vp->Pos.y, w = vp->Size.x, hgt = vp->Size.y;
    // the cutscene's bars are the game's own (its letterbox, about 12.5% of the screen): text keeps clear of them
    const float bar = hgt * 0.125f * h.letterbox;
    // the objective (top left), outside cutscenes
    if (!h.objective.empty() && h.letterbox < 0.05f && h.ending.empty()) {
        const float y = y0 + 24 * u;
        Text(draw, x0 + 28 * u, y, 15 * u, Faded(kGold, 0.9f), "OBJECTIVE");
        float ly = y + 20 * u;
        for (const std::string& l : Wrap(h.objective, 20 * u, w * 0.4f)) {
            Text(draw, x0 + 28 * u, ly, 20 * u, h.objectiveFlash > 0 ? kGold : kWhite, l);
            ly += 24 * u;
        }
    }
    // the timer (top centre) and counters (top right), outside cutscenes
    if (h.timerShown && h.ending.empty() && h.letterbox < 0.05f) {
        const int seconds = int(h.timerSeconds + 0.999f);
        char text[32];
        std::snprintf(text, sizeof text, "%d:%02d", seconds / 60, seconds % 60);
        const float y = y0 + std::max(bar, 0.0f) + 18 * u;
        if (!h.timerLabel.empty())
            Text(draw, x0 + w / 2, y, 14 * u, Faded(kGold, 0.9f), h.timerLabel, 0.5f);
        Text(draw, x0 + w / 2, y + (h.timerLabel.empty() ? 0 : 18 * u), 32 * u, seconds <= 10 ? kRed : kWhite, text, 0.5f);
    }
    if (h.ending.empty() && h.letterbox < 0.05f) {
        float y = y0 + std::max(bar, 0.0f) + 22 * u;
        for (const auto& [label, value] : h.counters) {
            Text(draw, x0 + w - 28 * u, y, 14 * u, Faded(kGold, 0.9f), label, 1.0f);
            Text(draw, x0 + w - 28 * u, y + 16 * u, 26 * u, kWhite, value, 1.0f);
            y += 50 * u;
        }
    }
    // a title (centred), a notice (upper middle)
    if (h.titleAlpha > 0) {
        Text(draw, x0 + w / 2, y0 + hgt * 0.38f, 46 * u, Faded(kWhite, h.titleAlpha), h.title, 0.5f);
        if (!h.subtitle.empty())
            Text(draw, x0 + w / 2, y0 + hgt * 0.38f + 54 * u, 20 * u, Faded(kGold, h.titleAlpha), h.subtitle, 0.5f);
    }
    if (h.noticeAlpha > 0)
        Text(draw, x0 + w / 2, y0 + hgt * 0.22f, 28 * u, Faded(kWhite, h.noticeAlpha), h.notice, 0.5f);
    // the subtitle: above the bottom bar, the speaker's name in gold
    if (!h.line.empty()) {
        const float size = 20 * u;
        const std::vector<std::string> lines = Wrap(h.line, size, w * 0.7f);
        float y = y0 + hgt - std::max(bar, 40.0f * u) - (float(lines.size()) * 26 + 10) * u;
        if (!h.speaker.empty()) {
            Text(draw, x0 + w / 2, y - 22 * u, 16 * u, kGold, h.speaker, 0.5f);
        }
        for (const std::string& l : lines) {
            Text(draw, x0 + w / 2, y, size, kWhite, l, 0.5f);
            y += 26 * u;
        }
    }
    // the fade
    if (h.fade > 0.001f)
        draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + w, y0 + hgt), IM_COL32(0, 0, 0, int(255 * std::min(1.0f, h.fade))));
    // the end: its words over the scene as it stands, in the game's own message style, then the fade to black
    if (!h.ending.empty() && h.endingAlpha > 0) {
        const bool complete = h.ending.find("COMPLETE") != std::string::npos;
        const float y = y0 + hgt * 0.40f;
        ShadowedText(draw, x0 + w / 2, y, 40 * u, complete ? kGamePale : kGameFailed, h.endingAlpha, h.ending, 0.5f);
        if (!h.endingDetail.empty())
            ShadowedText(draw, x0 + w / 2, y + 50 * u, 20 * u, kGameBlue, h.endingAlpha, h.endingDetail, 0.5f);
        if (h.endingFade > 0.001f)
            draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + w, y0 + hgt), IM_COL32(0, 0, 0, int(255 * h.endingFade)));
    }
    // a script error, for the mission's author
    if (!h.error.empty())
        draw->AddText(nullptr, 15 * u, ImVec2(x0 + 12 * u, y0 + hgt - 30 * u), kRed, ("Mission script: " + h.error).c_str());
}

} // namespace swrots::debug
