// The on-screen button prompts for the keyboard and mouse (see prompts.h).
//
// The game draws every button icon as a picture through its texture interface (GameAPITexture,
// [[0x645F7C]+0x38]+0x4C, vtable 0x561A30): slot 1 (0x1F650) loads a picture (thiscall (EFilePath*, int* w, int* h,
// a4, a5, a6) -> its handle; the path's file name is inline at +8), slot 3 (0x1F5E0) releases one, slot 5 (0x1F040)
// draws a quad (thiscall, 14 dwords: float* top left, float* bottom right, the handle, then its colour and more).
// The menus' icons are interfc\front_end\s_buttons_xbox_{a,b,x,y} (ButtonIcon items: 32 x 32, their label starting
// 30 px right of them; drawn from 0x2C4ECA) and the d-pad pictures s_buttons_ps2_dpad, _dpad_on (left/right lit) and
// _dpad_on2 (up/down lit), layered; the tips' and the HUD's are interfc\hud_tutorialtipbuttons\xboxtipbutton-NN,
// through the tip icon table 0x7F0608: 18 entries {w, h, key, handle} for A, B, X, Y, Black, White, the triggers,
// Start, Back, the stick clicks (these four without a picture), the d-pad's four and the two sticks, whose sizes the
// tips' layout reads. The port records which handle is which icon as the game loads them; while the prompts are the
// keyboard's it draws the key cap of the key bound to that button instead (a wider cap grows from the icon's right
// side in a footer, from its middle elsewhere) and puts the caps in the tip table (the game's own back with the
// controller).
//
// The caps are made the first time they are needed: drawn in the game's two looks (the menus': a black key with a
// frosty blue rim, as the controller screen's trigger caps; the tips': a navy key, as the tips' trigger icons),
// labelled with the game's font_small_white glyphs (interfc\fonts\font_small_white.stx/.txt), written as STX
// pictures (A8R8G8B8, swizzled) into cache\prompts and loaded through a disc path of the port's own
// (interfc\port_prompts\<name>, kernel::SetDiscFileOverride) with the loader's direct path.

#include "game/prompts.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <intrin.h>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "game/characters.h"
#include "game/menus.h"
#include "game/resources.h"
#include "input/controls.h"
#include "kernel/kernel.h"
#include "kernel/clock.h"

namespace swrots::game {

namespace {

// --- The pictures ----------------------------------------------------------------------------------------------------
struct Colour {
    float r, g, b, a;
};

// Straight (not premultiplied) RGBA, 0-1.
struct Image {
    int w = 0, h = 0;
    std::vector<Colour> px;
    Image() = default;
    Image(int width, int height) : w(width), h(height), px(size_t(width) * height, Colour{ 0, 0, 0, 0 }) {}
    Colour& at(int x, int y) { return px[size_t(y) * w + x]; }
    const Colour& at(int x, int y) const { return px[size_t(y) * w + x]; }
};

Colour Rgba(int r, int g, int b, int a = 255)
{
    return { r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f };
}

// `src` (with `coverage` of it, 0-1) over the pixel.
void Blend(Colour& dst, Colour src, float coverage = 1.0f)
{
    const float sa = src.a * coverage;
    if (sa <= 0)
        return;
    const float oa = sa + dst.a * (1 - sa);
    dst.r = (src.r * sa + dst.r * dst.a * (1 - sa)) / oa;
    dst.g = (src.g * sa + dst.g * dst.a * (1 - sa)) / oa;
    dst.b = (src.b * sa + dst.b * dst.a * (1 - sa)) / oa;
    dst.a = oa;
}

void Over(Image& dst, const Image& src, int ox, int oy)
{
    for (int y = 0; y < src.h; ++y)
        for (int x = 0; x < src.w; ++x)
            if (ox + x >= 0 && ox + x < dst.w && oy + y >= 0 && oy + y < dst.h)
                Blend(dst.at(ox + x, oy + y), src.at(x, y));
}

// A rounded rectangle covering the pixels x0..x1, y0..y1 (inclusive), corner radius r, smoothed edges.
void RoundRect(Image& img, float x0, float y0, float x1, float y1, float r, Colour c)
{
    const float cx = (x0 + x1 + 1) / 2, cy = (y0 + y1 + 1) / 2, hx = (x1 + 1 - x0) / 2, hy = (y1 + 1 - y0) / 2;
    for (int y = std::max(0, int(y0) - 1); y < std::min(img.h, int(y1) + 2); ++y)
        for (int x = std::max(0, int(x0) - 1); x < std::min(img.w, int(x1) + 2); ++x) {
            const float qx = std::fabs(x + 0.5f - cx) - hx + r, qy = std::fabs(y + 0.5f - cy) - hy + r;
            const float d = std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - r;
            const float coverage = std::clamp(0.5f - d, 0.0f, 1.0f);
            if (coverage > 0)
                Blend(img.at(x, y), c, coverage);
        }
}

// A rounded rectangle with a one-pixel outline.
void OutlinedRoundRect(Image& img, float x0, float y0, float x1, float y1, float r, Colour fill, Colour outline)
{
    RoundRect(img, x0, y0, x1, y1, r, outline);
    RoundRect(img, x0 + 1, y0 + 1, x1 - 1, y1 - 1, std::max(0.0f, r - 1), fill);
}

// A triangle (filled, smoothed by 4x4 samples a pixel).
void Triangle(Image& img, float ax, float ay, float bx, float by, float cx, float cy, Colour c)
{
    const auto side = [](float px, float py, float x0, float y0, float x1, float y1) {
        return (x1 - x0) * (py - y0) - (y1 - y0) * (px - x0);
    };
    const int minX = std::max(0, int(std::floor(std::min({ ax, bx, cx })))),
              maxX = std::min(img.w - 1, int(std::ceil(std::max({ ax, bx, cx }))));
    const int minY = std::max(0, int(std::floor(std::min({ ay, by, cy })))),
              maxY = std::min(img.h - 1, int(std::ceil(std::max({ ay, by, cy }))));
    for (int y = minY; y <= maxY; ++y)
        for (int x = minX; x <= maxX; ++x) {
            int inside = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) {
                    const float px = x + (sx + 0.5f) / 4, py = y + (sy + 0.5f) / 4;
                    const float d0 = side(px, py, ax, ay, bx, by), d1 = side(px, py, bx, by, cx, cy),
                                d2 = side(px, py, cx, cy, ax, ay);
                    if ((d0 >= 0 && d1 >= 0 && d2 >= 0) || (d0 <= 0 && d1 <= 0 && d2 <= 0))
                        ++inside;
                }
            if (inside)
                Blend(img.at(x, y), c, inside / 16.0f);
        }
}

// A soft glow: the image blurred (three box passes, premultiplied).
Image Blurred(const Image& src, int radius)
{
    Image img = src;
    for (Colour& p : img.px) {
        p.r *= p.a;
        p.g *= p.a;
        p.b *= p.a;
    }
    for (int pass = 0; pass < 3; ++pass)
        for (int axis = 0; axis < 2; ++axis) {
            Image out(img.w, img.h);
            for (int y = 0; y < img.h; ++y)
                for (int x = 0; x < img.w; ++x) {
                    Colour sum = { 0, 0, 0, 0 };
                    int n = 0;
                    for (int k = -radius; k <= radius; ++k) {
                        const int sx = axis == 0 ? x + k : x, sy = axis == 1 ? y + k : y;
                        if (sx < 0 || sx >= img.w || sy < 0 || sy >= img.h)
                            continue;
                        const Colour& p = img.at(sx, sy);
                        sum.r += p.r;
                        sum.g += p.g;
                        sum.b += p.b;
                        sum.a += p.a;
                        ++n;
                    }
                    const float w = 1.0f / float(2 * radius + 1);
                    out.at(x, y) = { sum.r * w, sum.g * w, sum.b * w, sum.a * w };
                }
            img = std::move(out);
        }
    for (Colour& p : img.px)
        if (p.a > 0) {
            p.r /= p.a;
            p.g /= p.a;
            p.b /= p.a;
        }
    return img;
}

// Area-averaged scaling (premultiplied): for the font's glyphs, drawn smaller than the sheet's.
Image Scaled(const Image& src, int w, int h)
{
    Image out(w, h);
    const float sx = float(src.w) / w, sy = float(src.h) / h;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float x0 = x * sx, x1 = (x + 1) * sx, y0 = y * sy, y1 = (y + 1) * sy;
            Colour sum = { 0, 0, 0, 0 };
            float area = 0;
            for (int yy = int(y0); yy < std::min(src.h, int(std::ceil(y1))); ++yy)
                for (int xx = int(x0); xx < std::min(src.w, int(std::ceil(x1))); ++xx) {
                    const float wx = std::min(x1, float(xx + 1)) - std::max(x0, float(xx));
                    const float wy = std::min(y1, float(yy + 1)) - std::max(y0, float(yy));
                    const float a = wx * wy;
                    const Colour& p = src.at(xx, yy);
                    sum.r += p.r * p.a * a;
                    sum.g += p.g * p.a * a;
                    sum.b += p.b * p.a * a;
                    sum.a += p.a * a;
                    area += a;
                }
            if (area > 0 && sum.a > 0)
                out.at(x, y) = { sum.r / sum.a, sum.g / sum.a, sum.b / sum.a, sum.a / area };
        }
    return out;
}

// --- The game's font -------------------------------------------------------------------------------------------------
// interfc\fonts\font_small_white.stx: an STX texture (width +0xC, height +0x10, format +0x3C: 0x52 DXT5, 0x56
// A8R8G8B8 swizzled; the pixels last): white glyphs with a dark edge. font_small_white.txt: the sheet's size, the cell
// width and line height, the space and kerning, then a line a character: the character and its box (x0 y0 x1 y1).
struct Glyph {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool present = false;
};
Image g_Font;
Glyph g_Glyphs[256];
bool g_FontTried = false;

void SwizzleTables(int w, int h, std::vector<uint32_t>& xs, std::vector<uint32_t>& ys)
{
    xs.assign(size_t(w), 0);
    ys.assign(size_t(h), 0);
    uint32_t maskX = 0, maskY = 0, bit = 1, maskBit = 1;
    while (bit < uint32_t(w) || bit < uint32_t(h)) {
        if (bit < uint32_t(w)) { maskX |= maskBit; maskBit <<= 1; }
        if (bit < uint32_t(h)) { maskY |= maskBit; maskBit <<= 1; }
        bit <<= 1;
    }
    const auto spread = [](uint32_t mask, uint32_t v) {
        uint32_t r = 0;
        for (uint32_t b = 1; v && b; b <<= 1)
            if (mask & b) {
                if (v & 1)
                    r |= b;
                v >>= 1;
            }
        return r;
    };
    for (int x = 0; x < w; ++x)
        xs[size_t(x)] = spread(maskX, uint32_t(x));
    for (int y = 0; y < h; ++y)
        ys[size_t(y)] = spread(maskY, uint32_t(y));
}

Colour From565(uint16_t c)
{
    return { ((c >> 11) & 31) / 31.0f, ((c >> 5) & 63) / 63.0f, (c & 31) / 31.0f, 1.0f };
}

void DecodeDxt5(const uint8_t* data, int w, int h, Image& img)
{
    img = Image(w, h);
    for (int by = 0; by < h / 4; ++by)
        for (int bx = 0; bx < w / 4; ++bx) {
            const uint8_t* b = data + (size_t(by) * (w / 4) + bx) * 16;
            float alpha[8];
            alpha[0] = b[0] / 255.0f;
            alpha[1] = b[1] / 255.0f;
            if (b[0] > b[1]) {
                for (int i = 1; i < 7; ++i)
                    alpha[i + 1] = ((7 - i) * alpha[0] + i * alpha[1]) / 7;
            } else {
                for (int i = 1; i < 5; ++i)
                    alpha[i + 1] = ((5 - i) * alpha[0] + i * alpha[1]) / 5;
                alpha[6] = 0;
                alpha[7] = 1;
            }
            uint64_t abits = 0;
            for (int i = 0; i < 6; ++i)
                abits |= uint64_t(b[2 + i]) << (8 * i);
            const uint16_t c0 = uint16_t(b[8] | (b[9] << 8)), c1 = uint16_t(b[10] | (b[11] << 8));
            Colour colours[4] = { From565(c0), From565(c1) };
            colours[2] = { (2 * colours[0].r + colours[1].r) / 3, (2 * colours[0].g + colours[1].g) / 3,
                (2 * colours[0].b + colours[1].b) / 3, 1 };
            colours[3] = { (colours[0].r + 2 * colours[1].r) / 3, (colours[0].g + 2 * colours[1].g) / 3,
                (colours[0].b + 2 * colours[1].b) / 3, 1 };
            const uint32_t cbits = uint32_t(b[12] | (b[13] << 8) | (b[14] << 16) | (uint32_t(b[15]) << 24));
            for (int i = 0; i < 16; ++i) {
                Colour c = colours[(cbits >> (2 * i)) & 3];
                c.a = alpha[(abits >> (3 * i)) & 7];
                img.at(bx * 4 + i % 4, by * 4 + i / 4) = c;
            }
        }
}

bool LoadFont()
{
    if (g_FontTried)
        return g_Font.w > 0;
    g_FontTried = true;
    std::vector<uint8_t> sheet, boxes;
    if (!ReadDiscResource("interfc\\fonts\\font_small_white.stx", sheet) ||
        !ReadDiscResource("interfc\\fonts\\font_small_white.txt", boxes) || sheet.size() < 0x184 ||
        std::memcmp(sheet.data(), "STX", 3) != 0) {
        LOG_WARN("Prompts: the game's small font is not there; keys stay the controller's");
        return false;
    }
    uint32_t w = 0, h = 0;
    std::memcpy(&w, sheet.data() + 0xC, 4);
    std::memcpy(&h, sheet.data() + 0x10, 4);
    const uint8_t format = sheet[0x3C];
    const size_t bytes = format == 0x52 ? size_t(w) * h : size_t(w) * h * 4;
    if ((format != 0x52 && format != 0x56) || !w || !h || w % 4 || h % 4 || bytes > sheet.size()) {
        LOG_WARN("Prompts: the game's small font is not as expected (%ux%u, format 0x%02X)", w, h, format);
        return false;
    }
    const uint8_t* pixels = sheet.data() + sheet.size() - bytes;
    if (format == 0x52) {
        DecodeDxt5(pixels, int(w), int(h), g_Font);
    } else {
        g_Font = Image(int(w), int(h));
        std::vector<uint32_t> xs, ys;
        SwizzleTables(int(w), int(h), xs, ys);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const uint8_t* p = pixels + size_t(xs[x] | ys[y]) * 4; // B G R A
                g_Font.at(int(x), int(y)) = Rgba(p[2], p[1], p[0], p[3]);
            }
    }
    std::istringstream text(std::string(boxes.begin(), boxes.end()));
    std::string line;
    int number = 0;
    while (std::getline(text, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (++number <= 3 || line.size() < 3)
            continue;
        Glyph g;
        std::istringstream(line.substr(1)) >> g.x0 >> g.y0 >> g.x1 >> g.y1;
        g.present = g.x1 > g.x0 && g.y1 > g.y0 && g.x1 <= int(w) && g.y1 <= int(h);
        g_Glyphs[static_cast<unsigned char>(line[0])] = g;
    }
    return true;
}

// The text in the game's font (white glyphs with the font's own dark edge), scaled.
Image Label(const std::string& text, float scale)
{
    std::vector<const Glyph*> glyphs;
    for (char c : text) {
        const unsigned char u = static_cast<unsigned char>(toupper(static_cast<unsigned char>(c)));
        if (g_Glyphs[u].present)
            glyphs.push_back(&g_Glyphs[u]);
    }
    if (glyphs.empty())
        return Image(1, 1);
    int w = 0, h = 0;
    for (const Glyph* g : glyphs) {
        w += g->x1 - g->x0 - 2;
        h = std::max(h, g->y1 - g->y0);
    }
    w += 2;
    Image img(std::max(1, w), std::max(1, h));
    int x = 0;
    for (const Glyph* g : glyphs) {
        for (int yy = g->y0; yy < g->y1; ++yy)
            for (int xx = g->x0; xx < g->x1; ++xx)
                if (x + xx - g->x0 < img.w)
                    Blend(img.at(x + xx - g->x0, yy - g->y0), g_Font.at(xx, yy));
        x += g->x1 - g->x0 - 2;
    }
    return Scaled(img, std::max(1, int(img.w * scale + 0.5f)), std::max(1, int(img.h * scale + 0.5f)));
}

// --- The key caps ----------------------------------------------------------------------------------------------------
enum class Look { Menu, Tip };
enum class Kind { Text, Mouse, Arrows, Blank };

struct CapSpec {
    Look look = Look::Menu;
    Kind kind = Kind::Blank;
    std::string text; // Text: the label; Arrows: the directions ("U", "D", "L", "R" in any mix)
    int mouse = 0;    // Mouse: the button lit (1-5), 0 none (movement)
    std::string Name() const
    {
        std::string n = look == Look::Menu ? "m_" : "t_";
        n += kind == Kind::Text ? "k" : kind == Kind::Mouse ? "mouse" : kind == Kind::Arrows ? "arrows" : "blank";
        if (kind == Kind::Mouse)
            n += std::to_string(mouse);
        for (char c : text) // a file name: letters and digits as they are, the rest by number
            n += isalnum(static_cast<unsigned char>(c)) ? std::string(1, char(tolower(static_cast<unsigned char>(c))))
                                                        : "_" + std::to_string(int(static_cast<unsigned char>(c)));
        return n;
    }
};

// The key's background in a look, `w` wide (centred in the picture).
void KeyBody(Image& img, int ox, int w, Look look)
{
    if (look == Look::Menu) {
        Image glow(img.w, img.h);
        RoundRect(glow, float(ox + 2), 2, float(ox + w - 3), 29, 6, Rgba(164, 198, 246, 200));
        Over(img, Blurred(glow, 1), 0, 0);
        OutlinedRoundRect(img, float(ox + 4), 4, float(ox + w - 5), 27, 5, Rgba(16, 18, 20), Rgba(150, 180, 220));
        RoundRect(img, float(ox + 6), 6, float(ox + w - 7), 22, 4, Rgba(40, 42, 44));
    } else {
        RoundRect(img, float(ox + 2), 3, float(ox + w - 3), 28, 5, Rgba(0, 4, 8, 230));
        OutlinedRoundRect(img, float(ox + 3), 4, float(ox + w - 4), 27, 4, Rgba(0, 12, 57), Rgba(32, 46, 84));
        RoundRect(img, float(ox + 5), 6, float(ox + w - 6), 22, 3, Rgba(8, 22, 70));
    }
}

Image Arrow(char direction, float cx, float cy, float size, Image img)
{
    const Colour white = Rgba(255, 255, 255);
    const float s = size / 2;
    switch (direction) {
    case 'U': Triangle(img, cx - s, cy + s * 0.6f, cx + s, cy + s * 0.6f, cx, cy - s * 0.7f, white); break;
    case 'D': Triangle(img, cx - s, cy - s * 0.6f, cx + s, cy - s * 0.6f, cx, cy + s * 0.7f, white); break;
    case 'L': Triangle(img, cx + s * 0.6f, cy - s, cx + s * 0.6f, cy + s, cx - s * 0.7f, cy, white); break;
    case 'R': Triangle(img, cx - s * 0.6f, cy - s, cx - s * 0.6f, cy + s, cx + s * 0.7f, cy, white); break;
    default: break;
    }
    return img;
}

Image MakeCap(const CapSpec& spec)
{
    if (spec.kind == Kind::Blank)
        return Image(32, 32);
    if (spec.kind == Kind::Mouse) { // a mouse with the bound button lit
        Image img(32, 32);
        const Colour body = spec.look == Look::Menu ? Rgba(16, 18, 20) : Rgba(0, 12, 57);
        const Colour rim = spec.look == Look::Menu ? Rgba(150, 180, 220) : Rgba(164, 174, 197);
        const Colour lit = Rgba(255, 255, 255);
        if (spec.look == Look::Menu) {
            Image glow(32, 32);
            RoundRect(glow, 7, 2, 24, 29, 8, Rgba(164, 198, 246, 160));
            Over(img, Blurred(glow, 1), 0, 0);
        }
        OutlinedRoundRect(img, 8, 3, 23, 28, 7, body, rim);
        RoundRect(img, 15, 4, 15, 13, 0, rim);
        RoundRect(img, 9, 13, 22, 13, 0, rim);
        switch (spec.mouse) {
        case 1: RoundRect(img, 9, 4, 15, 12, 3, lit); break;
        case 2: RoundRect(img, 16, 4, 22, 12, 3, lit); break;
        case 3: RoundRect(img, 14, 5, 17, 11, 1, lit); break;
        case 4:
        case 5:
            RoundRect(img, 6, 15, 8, 21, 0, lit);
            Over(img, Label(std::to_string(spec.mouse), 0.5f), 17, 15);
            break;
        default: // movement: arrows beside it
            img = Arrow('L', 4, 20, 6, img);
            img = Arrow('R', 27, 20, 6, img);
            break;
        }
        return img;
    }
    if (spec.kind == Kind::Arrows) {
        const int w = spec.text.size() > 2 ? 64 : 32;
        Image img(w, 32);
        KeyBody(img, 0, w, spec.look);
        if (spec.text.size() == 1) {
            img = Arrow(spec.text[0], w / 2.0f, 15, 12, img);
        } else if (spec.text == "UD") {
            img = Arrow('U', w / 2.0f, 10, 9, img);
            img = Arrow('D', w / 2.0f, 20, 9, img);
        } else if (spec.text == "LR") {
            img = Arrow('L', w / 2.0f - 5, 15, 9, img);
            img = Arrow('R', w / 2.0f + 5, 15, 9, img);
        } else { // all four
            img = Arrow('U', w / 2.0f, 10, 8, img);
            img = Arrow('D', w / 2.0f, 21, 8, img);
            img = Arrow('L', w / 2.0f - 12, 15, 8, img);
            img = Arrow('R', w / 2.0f + 12, 15, 8, img);
        }
        return img;
    }
    const Image label = Label(spec.text, spec.text.size() <= 2 ? 0.72f : 0.62f);
    const int w = std::max(32, (label.w + 14 + 3) / 4 * 4);
    const int texture = w <= 32 ? 32 : w <= 64 ? 64 : 128;
    Image img(texture, 32);
    const int ox = (texture - w) / 2;
    KeyBody(img, ox, w, spec.look);
    Over(img, label, ox + (w - label.w) / 2, std::max(0, 15 - label.h / 2));
    return img;
}

// An STX picture of the image: A8R8G8B8 (format 0x56), swizzled, its name at 0x40.
std::vector<uint8_t> Stx(const Image& img, const std::string& name)
{
    constexpr size_t kHeader = 0x184;
    const uint32_t pixelBytes = uint32_t(img.w * img.h * 4), bir = 0x80 + pixelBytes;
    std::vector<uint8_t> out(kHeader + pixelBytes, 0);
    const auto put = [&](size_t at, uint32_t v) { std::memcpy(out.data() + at, &v, 4); };
    std::memcpy(out.data(), "STX", 4);
    put(4, 4);
    put(8, 0);
    put(0xC, uint32_t(img.w));
    put(0x10, uint32_t(img.h));
    put(0x14, uint32_t(2 * img.w));
    put(0x18, uint32_t(2 * img.h));
    put(0x24, 1);
    put(0x28, 1);
    put(0x30, 3);
    put(0x34, bir);
    put(0x38, 0x80);
    put(0x3C, 0x56);
    std::memcpy(out.data() + 0x40, name.data(), std::min<size_t>(name.size(), 0x27));
    put(0x68, 2);
    put(0x6C, 0x0A);
    put(0x84, bir);
    put(0x88, 0x80);
    put(0x8C, bir);
    put(0x90, bir);
    std::memcpy(out.data() + 0x104, "BIR", 4);
    std::vector<uint32_t> xs, ys;
    SwizzleTables(img.w, img.h, xs, ys);
    for (int y = 0; y < img.h; ++y)
        for (int x = 0; x < img.w; ++x) {
            const Colour& c = img.at(x, y);
            uint8_t* p = out.data() + kHeader + size_t(xs[size_t(x)] | ys[size_t(y)]) * 4;
            p[0] = uint8_t(std::clamp(c.b, 0.0f, 1.0f) * 255 + 0.5f);
            p[1] = uint8_t(std::clamp(c.g, 0.0f, 1.0f) * 255 + 0.5f);
            p[2] = uint8_t(std::clamp(c.r, 0.0f, 1.0f) * 255 + 0.5f);
            p[3] = uint8_t(std::clamp(c.a, 0.0f, 1.0f) * 255 + 0.5f);
        }
    return out;
}

std::wstring g_Cache;

struct Cap {
    void* handle = nullptr;
    int w = 32, h = 32;
    bool tried = false;
};
std::unordered_map<std::string, Cap> g_Caps; // this start's, by name

// The cap's picture, made and loaded the first time (null when it cannot be).
const Cap* CapOf(const CapSpec& spec)
{
    const std::string name = spec.Name();
    Cap& cap = g_Caps[name];
    if (cap.tried)
        return cap.handle ? &cap : nullptr;
    cap.tried = true;
    if (!LoadFont())
        return nullptr;
    const Image img = MakeCap(spec);
    const std::vector<uint8_t> stx = Stx(img, "port_" + name);
    const std::wstring folder = g_Cache + L"\\prompts";
    CreateDirectoryW(folder.c_str(), nullptr);
    const std::wstring file = folder + L"\\" + std::wstring(name.begin(), name.end()) + L".stx";
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        LOG_WARN("Prompts: %ls could not be written", file.c_str());
        return nullptr;
    }
    DWORD written = 0;
    WriteFile(h, stx.data(), DWORD(stx.size()), &written, nullptr);
    CloseHandle(h);
    kernel::SetDiscFileOverride("\\interfc\\port_prompts\\" + name + ".stx", file);
    cap.handle = LoadPictureFile(("interfc\\port_prompts\\" + name).c_str());
    cap.w = img.w;
    cap.h = img.h;
    if (!cap.handle)
        LOG_WARN("Prompts: the key cap %s did not load", name.c_str());
    return cap.handle ? &cap : nullptr;
}

// --- Which key a button is ------------------------------------------------------------------------------------------
enum Icon {
    MenuA, MenuB, MenuX, MenuY, DpadBase, DpadLeftRight, DpadUpDown,
    TipFirst, // the tip table's 18, in its order
    TipCount = 18,
    IconCount = TipFirst + TipCount
};

int IconOf(const char* name)
{
    static const struct {
        const char* name;
        int icon;
    } kMenu[] = { { "s_buttons_xbox_a", MenuA }, { "s_buttons_xbox_b", MenuB }, { "s_buttons_xbox_x", MenuX },
        { "s_buttons_xbox_y", MenuY }, { "s_buttons_ps2_dpad", DpadBase }, { "s_buttons_ps2_dpad_on", DpadLeftRight },
        { "s_buttons_ps2_dpad_on2", DpadUpDown } };
    for (const auto& m : kMenu)
        if (_stricmp(name, m.name) == 0)
            return m.icon;
    if (_strnicmp(name, "xboxtipbutton-", 14) != 0)
        return -1;
    // the tip pictures by number: A B X Y, Black, White, the triggers (per language), the d-pad, the sticks
    switch (atoi(name + 14)) {
    case 97: return TipFirst + 0;
    case 98: return TipFirst + 1;
    case 99: return TipFirst + 2;
    case 100: return TipFirst + 3;
    case 116: return TipFirst + 4;
    case 115: return TipFirst + 5;
    case 101: case 117: case 121: case 125: return TipFirst + 6;
    case 102: case 126: return TipFirst + 7;
    case 109: return TipFirst + 12;
    case 105: return TipFirst + 13;
    case 107: return TipFirst + 14;
    case 103: return TipFirst + 15;
    case 111: case 119: case 123: case 127: return TipFirst + 16;
    case 113: case 128: return TipFirst + 17;
    default: return -1;
    }
}

std::string KeyText(int vk)
{
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        return std::string(1, char(vk));
    if (vk >= VK_F1 && vk <= VK_F24)
        return "F" + std::to_string(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return "NUM" + std::to_string(vk - VK_NUMPAD0);
    switch (vk) {
    case VK_SPACE: return "SPACE";
    case VK_RETURN: return "ENTER";
    case VK_ESCAPE: return "ESC";
    case VK_TAB: return "TAB";
    case VK_BACK: return "BKSP";
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return "SHIFT";
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return "CTRL";
    case VK_MENU: case VK_LMENU: case VK_RMENU: return "ALT";
    case VK_INSERT: return "INS";
    case VK_DELETE: return "DEL";
    case VK_HOME: return "HOME";
    case VK_END: return "END";
    case VK_PRIOR: return "PGUP";
    case VK_NEXT: return "PGDN";
    case VK_CAPITAL: return "CAPS";
    case VK_OEM_COMMA: return ",";
    case VK_OEM_PERIOD: return ".";
    case VK_OEM_MINUS: return "-";
    case VK_OEM_PLUS: return "=";
    case VK_OEM_1: return ";";
    case VK_OEM_2: return "/";
    case VK_OEM_3: return "~";
    case VK_OEM_4: return "[";
    case VK_OEM_5: return "\\";
    case VK_OEM_6: return "]";
    case VK_OEM_7: return "'";
    default: return "?";
    }
}

// The cap of one key: a mouse button, an arrow key or a labelled key.
bool KeySpec(int vk, Look look, CapSpec& spec)
{
    spec = {};
    spec.look = look;
    static const int kMouse[] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 };
    for (int i = 0; i < 5; ++i)
        if (vk == kMouse[i]) {
            spec.kind = Kind::Mouse;
            spec.mouse = i + 1;
            return true;
        }
    const char* arrow = vk == VK_UP ? "U" : vk == VK_DOWN ? "D" : vk == VK_LEFT ? "L" : vk == VK_RIGHT ? "R" : nullptr;
    if (arrow) {
        spec.kind = Kind::Arrows;
        spec.text = arrow;
        return true;
    }
    spec.kind = Kind::Text;
    spec.text = KeyText(vk);
    return true;
}

// The first key of the first of these actions that has one.
bool ActionSpec(std::initializer_list<const wchar_t*> actions, Look look, CapSpec& spec)
{
    for (const wchar_t* action : actions) {
        const std::vector<int> keys = input::ActionKeys(action);
        if (!keys.empty())
            return KeySpec(keys[0], look, spec);
    }
    return false;
}

// Four movement actions as one cap: "WASD", the arrows, or their labels.
bool StickSpec(const wchar_t* up, const wchar_t* left, const wchar_t* down, const wchar_t* right, Look look,
    CapSpec& spec)
{
    int keys[4];
    const wchar_t* actions[4] = { up, left, down, right };
    for (int i = 0; i < 4; ++i) {
        const std::vector<int> k = input::ActionKeys(actions[i]);
        if (k.empty())
            return false;
        keys[i] = k[0];
    }
    spec = {};
    spec.look = look;
    if (keys[0] == VK_UP && keys[1] == VK_LEFT && keys[2] == VK_DOWN && keys[3] == VK_RIGHT) {
        spec.kind = Kind::Arrows;
        spec.text = "UDLR";
        return true;
    }
    spec.kind = Kind::Text;
    for (int vk : keys) {
        const std::string t = KeyText(vk);
        if (t.size() != 1) { // not four letters: the first one's
            spec.text = KeyText(keys[0]);
            return true;
        }
        spec.text += t;
    }
    return true;
}

// The cap an icon shows with the keyboard; false: the icon's own (no key bound).
bool SpecFor(int icon, CapSpec& spec)
{
    const Look menu = Look::Menu, tip = Look::Tip;
    switch (icon) {
    case MenuA: return ActionSpec({ L"MenuAccept", L"Jump" }, menu, spec);
    case MenuB: return ActionSpec({ L"MenuBack", L"CriticalAttack" }, menu, spec);
    case MenuX: return ActionSpec({ L"FastAttack" }, menu, spec);
    case MenuY: return ActionSpec({ L"StrongAttack" }, menu, spec);
    case DpadBase: spec = { menu, Kind::Blank }; return true;
    case DpadLeftRight: spec = { menu, Kind::Arrows, "LR" }; return true;
    case DpadUpDown: spec = { menu, Kind::Arrows, "UD" }; return true;
    default: break;
    }
    switch (icon - TipFirst) {
    case 0: return ActionSpec({ L"Jump" }, tip, spec);
    case 1: return ActionSpec({ L"CriticalAttack" }, tip, spec);
    case 2: return ActionSpec({ L"FastAttack" }, tip, spec);
    case 3: return ActionSpec({ L"StrongAttack" }, tip, spec);
    case 4: return ActionSpec({ L"StunLightning" }, tip, spec);
    case 5: return ActionSpec({ L"SaberThrow" }, tip, spec);
    case 6: return ActionSpec({ L"Block" }, tip, spec);
    case 7: return ActionSpec({ L"PushGrasp" }, tip, spec);
    case 8: return ActionSpec({ L"Pause" }, tip, spec);
    case 9: return ActionSpec({ L"Back" }, tip, spec);
    case 10: return ActionSpec({ L"LeftStickClick", L"ForceHeal" }, tip, spec);
    case 11: return ActionSpec({ L"RightStickClick", L"ForceHeal" }, tip, spec);
    case 12: return ActionSpec({ L"MenuUp" }, tip, spec);
    case 13: return ActionSpec({ L"MenuDown" }, tip, spec);
    case 14: return ActionSpec({ L"MenuLeft" }, tip, spec);
    case 15: return ActionSpec({ L"MenuRight" }, tip, spec);
    case 16: return StickSpec(L"MoveForward", L"MoveLeft", L"MoveBack", L"MoveRight", tip, spec);
    case 17:
        if (input::MouseIsRightStick()) {
            spec = { tip, Kind::Mouse };
            return true;
        }
        return StickSpec(L"ForceTargetUp", L"ForceTargetLeft", L"ForceTargetDown", L"ForceTargetRight", tip, spec);
    default: return false;
    }
}

int g_Generation = 0; // a new start of the game: the icons' caps are looked up again

const Cap* CapForIcon(int icon)
{
    static bool resolved[IconCount] = {};
    static CapSpec specs[IconCount];
    static bool have[IconCount] = {};
    static int generation = -1;
    if (generation != g_Generation) {
        generation = g_Generation;
        std::memset(resolved, 0, sizeof resolved);
    }
    if (icon < 0 || icon >= IconCount)
        return nullptr;
    if (!resolved[icon]) {
        resolved[icon] = true;
        have[icon] = SpecFor(icon, specs[icon]);
    }
    return have[icon] ? CapOf(specs[icon]) : nullptr;
}

// --- The game's texture interface ------------------------------------------------------------------------------------
constexpr uint32_t kTextureVtable = 0x00561A30;
constexpr uint32_t kLoad = 0x0001F650, kRelease = 0x0001F5E0, kDrawQuad = 0x0001F040;
constexpr uint32_t kFooterIconReturn = 0x002C4ECD; // ButtonIcon's draw
constexpr uint32_t kTipTable = 0x007F0608;         // 18 x {int w, h; const char* key; void* handle}

// The icons' handles (this start's), with the image each held when loaded: a handle the game freed and gave to
// another picture (an effect's) holds another image, and is no icon any more.
struct IconHandle {
    int icon;
    void* image;
};
std::unordered_map<void*, IconHandle> g_Icons;

void* ImageOf(void* handle)
{
    __try {
        return *reinterpret_cast<void**>(handle);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// The icon a handle is now, or -1.
int IconNow(void* handle)
{
    auto it = handle ? g_Icons.find(handle) : g_Icons.end();
    if (it == g_Icons.end())
        return -1;
    if (ImageOf(handle) != it->second.image) {
        g_Icons.erase(it);
        return -1;
    }
    return it->second.icon;
}

using LoadFn = void*(__fastcall*)(void*, void*, const uint8_t*, int*, int*, int, int, int);
using ReleaseFn = void(__fastcall*)(void*, void*, void*);
using DrawQuadFn = uint32_t(__fastcall*)(void*, void*, const float*, const float*, void*, uint32_t, uint32_t, uint32_t,
    uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

void* __fastcall LoadHook(void* self, void*, const uint8_t* path, int* w, int* h, int a4, int a5, int a6)
{
    void* handle = reinterpret_cast<LoadFn>(uintptr_t(kLoad))(self, nullptr, path, w, h, a4, a5, a6);
    if (handle && path) {
        const int icon = IconOf(reinterpret_cast<const char*>(path + 8));
        if (icon >= 0)
            g_Icons[handle] = { icon, ImageOf(handle) };
    }
    return handle;
}

void __fastcall ReleaseHook(void* self, void*, void* handle)
{
    g_Icons.erase(handle);
    reinterpret_cast<ReleaseFn>(uintptr_t(kRelease))(self, nullptr, handle);
}

// The tip table: the keyboard's caps in it, or the game's own pictures back.
struct TipEntry {
    int w, h;
    const char* key;
    void* handle;
};
TipEntry g_TipOriginals[TipCount];
bool g_TipCaps = false; // the table holds the caps

void SyncTipTable(bool keyboard)
{
    auto* table = reinterpret_cast<TipEntry*>(uintptr_t(kTipTable));
    // the game fills the table at each level start: what it holds that is not a cap is its own
    bool filled = false;
    for (int i = 0; i < TipCount; ++i)
        filled = filled || table[i].handle;
    if (!filled)
        return;
    for (int i = 0; i < TipCount; ++i) {
        bool isCap = false;
        for (const auto& [name, cap] : g_Caps)
            isCap = isCap || (cap.handle && cap.handle == table[i].handle);
        if (!isCap)
            g_TipOriginals[i] = table[i];
    }
    for (int i = 0; i < TipCount; ++i) {
        const Cap* cap = keyboard ? CapForIcon(TipFirst + i) : nullptr;
        if (cap) {
            table[i].handle = cap->handle;
            table[i].w = cap->w;
            table[i].h = cap->h;
        } else {
            table[i].handle = g_TipOriginals[i].handle;
            table[i].w = g_TipOriginals[i].w;
            table[i].h = g_TipOriginals[i].h;
        }
    }
    g_TipCaps = keyboard;
}

uint32_t __fastcall DrawQuadHook(void* self, void*, const float* topLeft, const float* bottomRight, void* handle,
    uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7, uint32_t a8, uint32_t a9, uint32_t a10,
    uint32_t a11, uint32_t a12, uint32_t a13)
{
    const auto draw = reinterpret_cast<DrawQuadFn>(uintptr_t(kDrawQuad));
    const uintptr_t from = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const bool keyboard = input::KeyboardPrompts();
    static ULONGLONG lastSync = 0;
    const ULONGLONG now = kernel::GameTickCount64();
    if (now - lastSync >= 100) { // the tip table, a few times a second
        lastSync = now;
        __try {
            SyncTipTable(keyboard);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    const int icon = keyboard ? IconNow(handle) : -1;
    const Cap* cap = icon >= 0 ? CapForIcon(icon) : nullptr;
    if (!cap || !topLeft || !bottomRight)
        return draw(self, nullptr, topLeft, bottomRight, handle, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13);
    // the cap at the icon's height; a wider one grows from the icon's right side in a footer (its label follows the
    // icon), from its middle elsewhere
    float tl[2] = { topLeft[0], topLeft[1] }, br[2] = { bottomRight[0], bottomRight[1] };
    const float height = br[1] - tl[1], width = height * float(cap->w) / float(cap->h);
    if (from == kFooterIconReturn) {
        tl[0] = br[0] - width;
    } else {
        const float middle = (tl[0] + br[0]) / 2;
        tl[0] = middle - width / 2;
        br[0] = middle + width / 2;
    }
    return draw(self, nullptr, tl, br, cap->handle, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13);
}

// --- Texts that name buttons ---------------------------------------------------------------------------------------
// The tips that tell the player to "pull" a trigger (the _XBOX texts) have _PS2 twins that say "press", in every
// language: with the keyboard's prompts, those are shown.
constexpr const char* kPullTips[] = { "IDS_TIPS_BLOCK", "IDS_TIPS_STALK", "IDS_TIPS_SHUNT", "IDS_TIPS_FORCE_PUSH",
    "IDS_TIPS_FORCE_GRASP", "IDS_TIPS_FORCE_FOCUS", "IDS_TIPS_GSHOOT" };

uint32_t RedirectText(uint32_t key)
{
    static std::vector<std::pair<uint32_t, uint32_t>> redirects;
    if (redirects.empty())
        for (const char* tip : kPullTips)
            redirects.push_back({ MenuKey((std::string(tip) + "_XBOX").c_str()), MenuKey((std::string(tip) + "_PS2").c_str()) });
    if (!input::KeyboardPrompts())
        return key;
    for (const auto& [from, to] : redirects)
        if (from == key)
            return to;
    return key;
}

// The pictures the menus draw themselves (picture items: d-pads, ...) take their texture from a handle through
// 0x2049F0 (cdecl (handle, frame) -> the texture); a button's handle gives the key cap's while the prompts are the
// keyboard's. (Its first 6 bytes are position-independent: mov eax, [esp+4]; test eax, eax.)
constexpr uint32_t kTextureOf = 0x002049F0;
constexpr uint8_t kTextureOfBytes[] = { 0x8B, 0x44, 0x24, 0x04, 0x85, 0xC0 };
using TextureOfFn = void*(__cdecl*)(void*, int);
TextureOfFn g_OriginalTextureOf = nullptr;

void* __cdecl TextureOfHook(void* handle, int frame)
{
    if (handle && !g_Icons.empty() && g_Icons.count(handle) && input::KeyboardPrompts()) {
        const int icon = IconNow(handle);
        if (const Cap* cap = icon >= 0 ? CapForIcon(icon) : nullptr)
            handle = cap->handle;
    }
    return g_OriginalTextureOf(handle, frame);
}

} // namespace

void InstallPrompts(const std::wstring& cache)
{
    g_Cache = cache;
    g_Caps.clear(); // the last start's handles are gone with it
    g_Icons.clear();
    std::memset(g_TipOriginals, 0, sizeof g_TipOriginals);
    g_TipCaps = false;
    ++g_Generation;
    const auto slot = [](int i) { return kTextureVtable + uint32_t(i) * 4; };
    const auto holds = [&](int i, uint32_t function) { return *reinterpret_cast<const uint32_t*>(uintptr_t(slot(i))) == function; };
    if (!holds(1, kLoad) || !holds(3, kRelease) || !holds(5, kDrawQuad)) {
        LOG_WARN("Prompts: the game's texture interface is not as expected; the prompts stay the controller's");
        return;
    }
    const auto patch = [&](int i, const void* hook) {
        const uint32_t value = uint32_t(reinterpret_cast<uintptr_t>(hook));
        PatchBytes(slot(i), &value, 4);
    };
    patch(1, reinterpret_cast<const void*>(&LoadHook));
    patch(3, reinterpret_cast<const void*>(&ReleaseHook));
    patch(5, reinterpret_cast<const void*>(&DrawQuadHook));
    if (std::memcmp(reinterpret_cast<const void*>(uintptr_t(kTextureOf)), kTextureOfBytes, sizeof kTextureOfBytes) == 0) {
        uint8_t* stub = [] { // made again at every start (ResetStubsForBoot)
            uint8_t* s = AllocStub(sizeof kTextureOfBytes + 5);
            std::memcpy(s, kTextureOfBytes, sizeof kTextureOfBytes);
            s[sizeof kTextureOfBytes] = 0xE9;
            const int32_t back = int32_t(kTextureOf + sizeof kTextureOfBytes) - int32_t(uintptr_t(s) + sizeof kTextureOfBytes + 5);
            std::memcpy(s + sizeof kTextureOfBytes + 1, &back, 4);
            return s;
        }();
        g_OriginalTextureOf = reinterpret_cast<TextureOfFn>(stub);
        PatchHook(kTextureOf, reinterpret_cast<const void*>(&TextureOfHook));
    }
    SetMenuTextRedirect(&RedirectText);
}

} // namespace swrots::game
