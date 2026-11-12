// See vcs_glyph_art.h. Every shape is a signed distance (in units of the glyph's height, y down) turned into coverage
// over its pixels - smooth edges at any size; text is a stroke font on a 4 x 6 grid drawn the same way.
#include "game/vcs_glyph_art.h"

#include "platform/platform.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <sstream>
#include <vector>

namespace vcs::glyphart
{
    namespace
    {
        struct Rgb
        {
            float r, g, b;
        };
        constexpr Rgb kDark{0.11f, 0.12f, 0.15f}, kRim{0.46f, 0.47f, 0.53f}, kWhite{0.94f, 0.94f, 0.96f},
                      kGrey{0.52f, 0.53f, 0.58f}, kInner{0.24f, 0.25f, 0.30f};
        constexpr Rgb kCapTop{0.93f, 0.93f, 0.95f}, kCapSide{0.56f, 0.57f, 0.63f}, kCapEdge{0.08f, 0.08f, 0.10f},
                      kCapText{0.10f, 0.10f, 0.12f}, kMouseLit{0.96f, 0.47f, 0.24f};
        // PlayStation: cross, circle, triangle, square; Xbox: A, B, X, Y
        constexpr Rgb kPsCross{0.50f, 0.71f, 1.00f}, kPsCircle{1.00f, 0.43f, 0.43f}, kPsTriangle{0.32f, 0.84f, 0.65f},
                      kPsSquare{0.96f, 0.56f, 0.84f};
        constexpr Rgb kXboxA{0.43f, 0.80f, 0.33f}, kXboxB{0.94f, 0.33f, 0.30f}, kXboxX{0.31f, 0.57f, 0.94f},
                      kXboxY{0.97f, 0.78f, 0.21f};

        // ---- the canvas: premultiplied RGBA floats ----
        struct Canvas
        {
            int w = 0, h = 0;
            std::vector<float> px; // r, g, b, a

            template <class Sd>
            void fill(float x0, float y0, float x1, float y1, Rgb c, float alpha, Sd sd)
            {
                const float s = static_cast<float>(h);
                const int ix0 = std::max(0, static_cast<int>(std::floor(x0 * s)) - 1);
                const int iy0 = std::max(0, static_cast<int>(std::floor(y0 * s)) - 1);
                const int ix1 = std::min(w - 1, static_cast<int>(std::ceil(x1 * s)) + 1);
                const int iy1 = std::min(h - 1, static_cast<int>(std::ceil(y1 * s)) + 1);
                for (int y = iy0; y <= iy1; ++y)
                    for (int x = ix0; x <= ix1; ++x)
                    {
                        const float d = sd((x + 0.5f) / s, (y + 0.5f) / s);
                        const float cov = std::clamp(0.5f - d * s, 0.0f, 1.0f) * alpha;
                        if (cov <= 0.0f)
                            continue;
                        float *p = &px[(static_cast<size_t>(y) * w + x) * 4];
                        p[0] = c.r * cov + p[0] * (1.0f - cov);
                        p[1] = c.g * cov + p[1] * (1.0f - cov);
                        p[2] = c.b * cov + p[2] * (1.0f - cov);
                        p[3] = cov + p[3] * (1.0f - cov);
                    }
            }
        };

        float sdCircle(float x, float y, float cx, float cy, float r) { return std::hypot(x - cx, y - cy) - r; }
        float sdRoundRect(float x, float y, float x0, float y0, float x1, float y1, float r)
        {
            const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
            const float hx = (x1 - x0) * 0.5f - r, hy = (y1 - y0) * 0.5f - r;
            const float qx = std::fabs(x - cx) - hx, qy = std::fabs(y - cy) - hy;
            return std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - r;
        }
        float sdSegment(float x, float y, float ax, float ay, float bx, float by)
        {
            const float px = x - ax, py = y - ay, dx = bx - ax, dy = by - ay;
            const float l = dx * dx + dy * dy;
            const float t = l > 0.0f ? std::clamp((px * dx + py * dy) / l, 0.0f, 1.0f) : 0.0f;
            return std::hypot(px - dx * t, py - dy * t);
        }
        // a convex polygon (any winding): the largest signed distance to its edges' lines
        float sdConvex(float x, float y, const float *p, int n)
        {
            float area = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                const int j = (i + 1) % n;
                area += p[2 * i] * p[2 * j + 1] - p[2 * j] * p[2 * i + 1];
            }
            const float sgn = area >= 0.0f ? 1.0f : -1.0f;
            float d = -1e9f;
            for (int i = 0; i < n; ++i)
            {
                const int j = (i + 1) % n;
                const float ex = p[2 * j] - p[2 * i], ey = p[2 * j + 1] - p[2 * i + 1];
                const float len = std::hypot(ex, ey);
                if (len <= 0.0f)
                    continue;
                // outward normal for a counter-clockwise (area > 0, y down: clockwise on screen) polygon
                const float nx = ey / len * sgn, ny = -ex / len * sgn;
                d = std::max(d, (x - p[2 * i]) * nx + (y - p[2 * i + 1]) * ny);
            }
            return d;
        }

        // ---- the stroke font: polylines on a 4 x 6 grid (y down), '|' between strokes ----
        struct Stroke
        {
            std::vector<float> pts; // x, y pairs
        };
        struct Char
        {
            std::vector<Stroke> strokes;
            float width = 4.0f;
        };
        const std::map<char, Char> &font()
        {
            static const std::map<char, Char> f = [] {
                const std::pair<char, const char *> defs[] = {
                    {'A', "0,6 0,2 1.2,0 2.8,0 4,2 4,6|0,3.6 4,3.6"},
                    {'B', "0,3 0,0 2.8,0 3.7,0.8 3.7,2.2 2.8,3 0,3 0,6 3,6 4,5 4,4 3,3"},
                    {'C', "4,1 3,0 1,0 0,1 0,5 1,6 3,6 4,5"},
                    {'D', "0,0 0,6 2.4,6 4,4.4 4,1.6 2.4,0 0,0"},
                    {'E', "4,0 0,0 0,6 4,6|0,3 3,3"},
                    {'F', "4,0 0,0 0,6|0,3 3,3"},
                    {'G', "4,1 3,0 1,0 0,1 0,5 1,6 3,6 4,5 4,3.4 2.2,3.4"},
                    {'H', "0,0 0,6|4,0 4,6|0,3 4,3"},
                    {'I', "0.6,0 3.4,0|2,0 2,6|0.6,6 3.4,6"},
                    {'J', "4,0 4,5 3,6 1,6 0,5"},
                    {'K', "0,0 0,6|4,0 0,3.6|1.3,2.7 4,6"},
                    {'L', "0,0 0,6 4,6"},
                    {'M', "0,6 0,0 2.5,3.6 5,0 5,6"},
                    {'N', "0,6 0,0 4,6 4,0"},
                    {'O', "1,0 3,0 4,1 4,5 3,6 1,6 0,5 0,1 1,0"},
                    {'P', "0,6 0,0 3,0 4,1 4,2 3,3 0,3"},
                    {'Q', "1,0 3,0 4,1 4,5 3,6 1,6 0,5 0,1 1,0|2.5,4.5 4.2,6.2"},
                    {'R', "0,6 0,0 3,0 4,1 4,2 3,3 0,3|2,3 4,6"},
                    {'S', "4,1 3,0 1,0 0,1 0,2 1,3 3,3 4,4 4,5 3,6 1,6 0,5"},
                    {'T', "0,0 4,0|2,0 2,6"},
                    {'U', "0,0 0,5 1,6 3,6 4,5 4,0"},
                    {'V', "0,0 2,6 4,0"},
                    {'W', "0,0 1.2,6 2.5,2.2 3.8,6 5,0"},
                    {'X', "0,0 4,6|4,0 0,6"},
                    {'Y', "0,0 2,3 4,0|2,3 2,6"},
                    {'Z', "0,0 4,0 0,6 4,6"},
                    {'0', "1,0 3,0 4,1 4,5 3,6 1,6 0,5 0,1 1,0"},
                    {'1', "0.8,1.2 2,0 2,6|0.8,6 3.2,6"},
                    {'2', "0,1 1,0 3,0 4,1 4,2 0,6 4,6"},
                    {'3', "0,0 4,0 2,2.5 3,2.5 4,3.5 4,5 3,6 1,6 0,5"},
                    {'4', "3,6 3,0 0,4 4,4"},
                    {'5', "4,0 0,0 0,2.6 3,2.6 4,3.6 4,5 3,6 0,6"},
                    {'6', "3.5,0 1.5,0 0,1.5 0,5 1,6 3,6 4,5 4,4 3,3 0,3"},
                    {'7', "0,0 4,0 1.5,6"},
                    {'8', "1,3 0,2 0,1 1,0 3,0 4,1 4,2 3,3 1,3 0,4 0,5 1,6 3,6 4,5 4,4 3,3"},
                    {'9', "4,3 1,3 0,2 0,1 1,0 3,0 4,1 4,4.5 2.5,6 0.5,6"},
                    {'+', "2,1.5 2,4.5|0.5,3 3.5,3"},
                    {'-', "0.5,3 3.5,3"},
                    {'=', "0.5,2 3.5,2|0.5,4 3.5,4"},
                    {'/', "0.5,6 3.5,0"},
                    {'\\', "0.5,0 3.5,6"},
                    {'.', "0.9,5.9 1.1,5.9"},
                    {',', "1.2,5.4 0.6,6.8"},
                    {';', "1.1,2.2 1.1,2.4|1.2,5.4 0.6,6.8"},
                    {'\'', "1,0 1,2"},
                    {'`', "0.5,0 1.5,1.5"},
                    {'[', "2,0 0.5,0 0.5,6 2,6"},
                    {']', "0.5,0 2,0 2,6 0.5,6"},
                    {'?', "0,1 1,0 3,0 4,1 4,2 2,3.4 2,4.2|2,5.8 2,6"},
                };
                std::map<char, Char> m;
                for (const auto &[c, def] : defs)
                {
                    Char ch;
                    float maxX = 0.0f;
                    std::stringstream strokes(def);
                    std::string stroke;
                    while (std::getline(strokes, stroke, '|'))
                    {
                        Stroke s;
                        std::stringstream points(stroke);
                        std::string point;
                        while (points >> point)
                        {
                            const size_t comma = point.find(',');
                            const float x = std::strtof(point.c_str(), nullptr), y = std::strtof(point.c_str() + comma + 1, nullptr);
                            s.pts.push_back(x);
                            s.pts.push_back(y);
                            maxX = std::max(maxX, x);
                        }
                        ch.strokes.push_back(std::move(s));
                    }
                    ch.width = std::max(maxX, 1.0f);
                    m[c] = std::move(ch);
                }
                return m;
            }();
            return f;
        }
        constexpr float kLetterGap = 1.7f, kSpace = 2.6f;

        float textWidth(const std::string &text, float capH)
        {
            const float k = capH / 6.0f;
            float w = 0.0f;
            for (size_t i = 0; i < text.size(); ++i)
            {
                const auto it = font().find(static_cast<char>(std::toupper(static_cast<unsigned char>(text[i]))));
                w += (it != font().end() ? it->second.width : kSpace) + (i + 1 < text.size() ? kLetterGap : 0.0f);
            }
            return w * k;
        }

        // the text with its left edge at x, centred on cy
        void drawText(Canvas *c, const std::string &text, float x, float cy, float capH, Rgb col)
        {
            if (!c)
                return;
            const float k = capH / 6.0f, top = cy - capH * 0.5f, sw = capH * 0.16f;
            for (char raw : text)
            {
                const auto it = font().find(static_cast<char>(std::toupper(static_cast<unsigned char>(raw))));
                if (it == font().end())
                {
                    x += (kSpace + kLetterGap) * k;
                    continue;
                }
                const Char &ch = it->second;
                std::vector<float> segs; // ax, ay, bx, by
                for (const Stroke &s : ch.strokes)
                    for (size_t i = 0; i + 3 < s.pts.size(); i += 2)
                    {
                        segs.push_back(x + s.pts[i] * k);
                        segs.push_back(top + s.pts[i + 1] * k);
                        segs.push_back(x + s.pts[i + 2] * k);
                        segs.push_back(top + s.pts[i + 3] * k);
                    }
                const float half = sw * 0.5f;
                c->fill(x - sw, top - sw, x + ch.width * k + sw, top + 7.0f * k + sw, col, 1.0f, [&](float px, float py) {
                    float d = 1e9f;
                    for (size_t i = 0; i < segs.size(); i += 4)
                        d = std::min(d, sdSegment(px, py, segs[i], segs[i + 1], segs[i + 2], segs[i + 3]));
                    return d - half;
                });
                x += (ch.width + kLetterGap) * k;
            }
        }
        void drawTextCentred(Canvas *c, const std::string &text, float cx, float cy, float capH, Rgb col)
        {
            drawText(c, text, cx - textWidth(text, capH) * 0.5f, cy, capH, col);
        }

        // a filled arrow head pointing `dir` (0 up, 1 down, 2 left, 3 right) centred at (cx, cy)
        void drawArrow(Canvas *c, float cx, float cy, int dir, float size, Rgb col)
        {
            if (!c)
                return;
            float p[6];
            const float s = size, b = size * 0.62f;
            switch (dir)
            {
            case 0: p[0] = cx; p[1] = cy - s * 0.6f; p[2] = cx + s; p[3] = cy + b * 0.6f; p[4] = cx - s; p[5] = cy + b * 0.6f; break;
            case 1: p[0] = cx; p[1] = cy + s * 0.6f; p[2] = cx - s; p[3] = cy - b * 0.6f; p[4] = cx + s; p[5] = cy - b * 0.6f; break;
            case 2: p[0] = cx - s * 0.6f; p[1] = cy; p[2] = cx + b * 0.6f; p[3] = cy - s; p[4] = cx + b * 0.6f; p[5] = cy + s; break;
            default: p[0] = cx + s * 0.6f; p[1] = cy; p[2] = cx - b * 0.6f; p[3] = cy + s; p[4] = cx - b * 0.6f; p[5] = cy - s; break;
            }
            c->fill(cx - s - 0.05f, cy - s - 0.05f, cx + s + 0.05f, cy + s + 0.05f, col, 1.0f,
                    [&](float x, float y) { return sdConvex(x, y, p, 3) - size * 0.08f; });
        }

        // ---- gamepad pieces ----
        void disc(Canvas *c, float cx)
        {
            if (!c)
                return;
            c->fill(cx - 0.5f, 0.0f, cx + 0.5f, 1.0f, kDark, 1.0f, [&](float x, float y) { return sdCircle(x, y, cx, 0.5f, 0.45f); });
            c->fill(cx - 0.5f, 0.0f, cx + 0.5f, 1.0f, kRim, 1.0f,
                    [&](float x, float y) { return std::fabs(sdCircle(x, y, cx, 0.5f, 0.425f)) - 0.022f; });
        }

        float faceButton(Canvas *c, const std::string &family, const std::string &button)
        {
            if (!c)
                return 1.0f;
            disc(c, 0.5f);
            constexpr float sw = 0.042f;
            if (family == "xbox")
            {
                const char *letter = button == "south" ? "A" : button == "east" ? "B" : button == "west" ? "X" : "Y";
                const Rgb col = button == "south" ? kXboxA : button == "east" ? kXboxB : button == "west" ? kXboxX : kXboxY;
                drawTextCentred(c, letter, 0.5f, 0.5f, 0.42f, col);
                return 1.0f;
            }
            if (button == "south") // cross
                c->fill(0.2f, 0.2f, 0.8f, 0.8f, kPsCross, 1.0f, [&](float x, float y) {
                    return std::min(sdSegment(x, y, 0.33f, 0.33f, 0.67f, 0.67f), sdSegment(x, y, 0.67f, 0.33f, 0.33f, 0.67f)) - sw;
                });
            else if (button == "east") // circle
                c->fill(0.2f, 0.2f, 0.8f, 0.8f, kPsCircle, 1.0f,
                        [&](float x, float y) { return std::fabs(sdCircle(x, y, 0.5f, 0.5f, 0.175f)) - sw; });
            else if (button == "north") // triangle
            {
                const float t[6] = {0.5f, 0.30f, 0.69f, 0.63f, 0.31f, 0.63f};
                c->fill(0.2f, 0.2f, 0.8f, 0.8f, kPsTriangle, 1.0f, [&](float x, float y) {
                    return std::min({sdSegment(x, y, t[0], t[1], t[2], t[3]), sdSegment(x, y, t[2], t[3], t[4], t[5]),
                                     sdSegment(x, y, t[4], t[5], t[0], t[1])}) - sw;
                });
            }
            else // square
                c->fill(0.2f, 0.2f, 0.8f, 0.8f, kPsSquare, 1.0f,
                        [&](float x, float y) { return std::fabs(sdRoundRect(x, y, 0.335f, 0.335f, 0.665f, 0.665f, 0.02f)) - sw; });
            return 1.0f;
        }

        float pill(Canvas *c, const std::string &label, float capH, float minW, bool trigger)
        {
            const float w = std::max(minW, textWidth(label, capH) + 0.5f);
            if (!c)
                return w;
            const float y0 = trigger ? 0.1f : 0.2f, y1 = trigger ? 0.9f : 0.8f, r = trigger ? 0.3f : 0.22f;
            c->fill(0.0f, 0.0f, w, 1.0f, kDark, 1.0f, [&](float x, float y) { return sdRoundRect(x, y, 0.05f, y0, w - 0.05f, y1, r); });
            c->fill(0.0f, 0.0f, w, 1.0f, kRim, 1.0f,
                    [&](float x, float y) { return std::fabs(sdRoundRect(x, y, 0.07f, y0 + 0.02f, w - 0.07f, y1 - 0.02f, r)) - 0.02f; });
            drawTextCentred(c, label, w * 0.5f, 0.5f, capH, kWhite);
            return w;
        }

        // D-pad: the cross, its arms lit for the directions named (bits: 1 up, 2 down, 4 left, 8 right)
        float dpad(Canvas *c, int lit)
        {
            if (!c)
                return 1.0f;
            auto arms = [](float x, float y) {
                return std::min(sdRoundRect(x, y, 0.35f, 0.07f, 0.65f, 0.93f, 0.07f), sdRoundRect(x, y, 0.07f, 0.35f, 0.93f, 0.65f, 0.07f));
            };
            c->fill(0.0f, 0.0f, 1.0f, 1.0f, kDark, 1.0f, arms);
            c->fill(0.0f, 0.0f, 1.0f, 1.0f, kRim, 1.0f, [&](float x, float y) { return std::fabs(arms(x, y) + 0.02f) - 0.02f; });
            // only the directions meant: a lit arm is told from the others at any size
            if (lit & 1)
                drawArrow(c, 0.5f, 0.23f, 0, 0.12f, kWhite);
            if (lit & 2)
                drawArrow(c, 0.5f, 0.77f, 1, 0.12f, kWhite);
            if (lit & 4)
                drawArrow(c, 0.23f, 0.5f, 2, 0.12f, kWhite);
            if (lit & 8)
                drawArrow(c, 0.77f, 0.5f, 3, 0.12f, kWhite);
            return 1.0f;
        }

        // a stick moved (arrows for the directions named; 15 = any) or pressed (click)
        float stick(Canvas *c, const std::string &family, bool left, int dirs, bool click)
        {
            if (!c)
                return 1.0f;
            disc(c, 0.5f);
            const bool xbox = family == "xbox";
            if (click)
            {
                c->fill(0.15f, 0.15f, 0.85f, 0.85f, kInner, 1.0f, [&](float x, float y) { return sdCircle(x, y, 0.5f, 0.5f, 0.31f); });
                drawTextCentred(c, xbox ? (left ? "LS" : "RS") : (left ? "L3" : "R3"), 0.5f, 0.5f, 0.22f, kWhite);
                return 1.0f;
            }
            c->fill(0.15f, 0.15f, 0.85f, 0.85f, kGrey, 1.0f,
                    [&](float x, float y) { return std::fabs(sdCircle(x, y, 0.5f, 0.5f, 0.21f)) - 0.02f; });
            drawTextCentred(c, xbox ? (left ? "LS" : "RS") : (left ? "L" : "R"), 0.5f, 0.5f, xbox ? 0.15f : 0.2f, kWhite);
            const float d = 0.33f, s = 0.095f;
            if (dirs & 1)
                drawArrow(c, 0.5f, 0.5f - d, 0, s, kWhite);
            if (dirs & 2)
                drawArrow(c, 0.5f, 0.5f + d, 1, s, kWhite);
            if (dirs & 4)
                drawArrow(c, 0.5f - d, 0.5f, 2, s, kWhite);
            if (dirs & 8)
                drawArrow(c, 0.5f + d, 0.5f, 3, s, kWhite);
            return 1.0f;
        }

        float xboxView(Canvas *c, bool menu)
        {
            if (!c)
                return 1.0f;
            disc(c, 0.5f);
            if (menu) // three lines
                c->fill(0.2f, 0.2f, 0.8f, 0.8f, kWhite, 1.0f, [&](float x, float y) {
                    return std::min({sdSegment(x, y, 0.37f, 0.4f, 0.63f, 0.4f), sdSegment(x, y, 0.37f, 0.5f, 0.63f, 0.5f),
                                     sdSegment(x, y, 0.37f, 0.6f, 0.63f, 0.6f)}) - 0.025f;
                });
            else // two overlapping windows
                c->fill(0.2f, 0.2f, 0.8f, 0.8f, kWhite, 1.0f, [&](float x, float y) {
                    return std::min(std::fabs(sdRoundRect(x, y, 0.35f, 0.37f, 0.57f, 0.55f, 0.03f)),
                                    std::fabs(sdRoundRect(x, y, 0.43f, 0.45f, 0.65f, 0.63f, 0.03f))) - 0.022f;
                });
            return 1.0f;
        }

        // ---- keyboard and mouse ----
        struct KeyLabel
        {
            std::string text;
            int arrow = -1; // 0 up, 1 down, 2 left, 3 right
        };
        KeyLabel keyLabel(int key)
        {
            using namespace platform;
            if ((key >= KEY_A && key <= KEY_Z) || (key >= KEY_ZERO && key <= KEY_NINE))
                return {std::string(1, static_cast<char>(key))};
            if (key >= KEY_F1 && key <= KEY_F12)
                return {"F" + std::to_string(key - KEY_F1 + 1)};
            if (key >= KEY_KP_0 && key <= KEY_KP_9)
                return {"NUM " + std::to_string(key - KEY_KP_0)};
            switch (key)
            {
            case KEY_SPACE: return {"SPACE"};
            case KEY_APOSTROPHE: return {"'"};
            case KEY_COMMA: return {","};
            case KEY_MINUS: return {"-"};
            case KEY_PERIOD: return {"."};
            case KEY_SLASH: return {"/"};
            case KEY_SEMICOLON: return {";"};
            case KEY_EQUAL: return {"="};
            case KEY_LEFT_BRACKET: return {"["};
            case KEY_BACKSLASH: return {"\\"};
            case KEY_RIGHT_BRACKET: return {"]"};
            case KEY_GRAVE: return {"`"};
            case KEY_ESCAPE: return {"ESC"};
            case KEY_ENTER: case KEY_KP_ENTER: return {"ENTER"};
            case KEY_TAB: return {"TAB"};
            case KEY_BACKSPACE: return {"BKSP"};
            case KEY_INSERT: return {"INS"};
            case KEY_DELETE: return {"DEL"};
            case KEY_UP: return {"", 0};
            case KEY_DOWN: return {"", 1};
            case KEY_LEFT: return {"", 2};
            case KEY_RIGHT: return {"", 3};
            case KEY_PAGE_UP: return {"PGUP"};
            case KEY_PAGE_DOWN: return {"PGDN"};
            case KEY_HOME: return {"HOME"};
            case KEY_END: return {"END"};
            case KEY_CAPS_LOCK: return {"CAPS"};
            case KEY_KP_DECIMAL: return {"NUM ."};
            case KEY_KP_DIVIDE: return {"NUM /"};
            case KEY_KP_MULTIPLY: return {"NUM X"};
            case KEY_KP_SUBTRACT: return {"NUM -"};
            case KEY_KP_ADD: return {"NUM +"};
            case KEY_LEFT_SHIFT: case KEY_RIGHT_SHIFT: return {"SHIFT"};
            case KEY_LEFT_CONTROL: case KEY_RIGHT_CONTROL: return {"CTRL"};
            case KEY_LEFT_ALT: case KEY_RIGHT_ALT: return {"ALT"};
            default: return {"?"};
            }
        }

        float keycap(Canvas *c, float x, int key)
        {
            const KeyLabel l = keyLabel(key);
            const float capH = l.text.size() <= 2 ? 0.36f : 0.25f;
            const float w = std::max(0.84f, l.text.empty() ? 0.84f : textWidth(l.text, capH) + 0.36f);
            if (!c)
                return w;
            const float x0 = x, x1 = x + w;
            c->fill(x0, 0.0f, x1, 1.0f, kCapEdge, 1.0f, [&](float px, float py) { return sdRoundRect(px, py, x0, 0.03f, x1, 0.95f, 0.16f); });
            c->fill(x0, 0.0f, x1, 1.0f, kCapSide, 1.0f,
                    [&](float px, float py) { return sdRoundRect(px, py, x0 + 0.04f, 0.07f, x1 - 0.04f, 0.91f, 0.13f); });
            c->fill(x0, 0.0f, x1, 1.0f, kCapTop, 1.0f,
                    [&](float px, float py) { return sdRoundRect(px, py, x0 + 0.07f, 0.09f, x1 - 0.07f, 0.77f, 0.11f); });
            if (l.arrow >= 0)
                drawArrow(c, (x0 + x1) * 0.5f, 0.43f, l.arrow, 0.13f, kCapText);
            else
                drawTextCentred(c, l.text, (x0 + x1) * 0.5f, 0.43f, capH, kCapText);
            return w;
        }

        // the mouse with a part lit: 0 none, 1 left, 2 right, 3 middle / wheel; wheel arrow (0 up / 1 down, -1 none);
        // label for the side buttons
        float mouse(Canvas *c, float x, int lit, int wheel, const std::string &label)
        {
            const float w = 0.62f + (wheel >= 0 ? 0.3f : 0.0f);
            if (!c)
                return w;
            const float bx0 = x + 0.06f, bx1 = x + 0.56f, mid = (bx0 + bx1) * 0.5f;
            auto body = [&](float px, float py) { return sdRoundRect(px, py, bx0, 0.05f, bx1, 0.95f, 0.24f); };
            c->fill(x, 0.0f, x + 0.62f, 1.0f, kCapEdge, 1.0f, [&](float px, float py) { return body(px, py) - 0.035f; });
            c->fill(x, 0.0f, x + 0.62f, 1.0f, kCapTop, 1.0f, body);
            if (lit == 1 || lit == 2)
            {
                const float hx0 = lit == 1 ? bx0 : mid, hx1 = lit == 1 ? mid : bx1;
                c->fill(x, 0.0f, x + 0.62f, 0.5f, kMouseLit, 1.0f, [&](float px, float py) {
                    return std::max(body(px, py) + 0.03f, sdRoundRect(px, py, hx0, 0.0f, hx1, 0.43f, 0.0f));
                });
            }
            c->fill(x, 0.0f, x + 0.62f, 0.5f, kCapEdge, 1.0f, [&](float px, float py) {
                return std::min(sdSegment(px, py, mid, 0.05f, mid, 0.43f), sdSegment(px, py, bx0, 0.43f, bx1, 0.43f)) - 0.016f;
            });
            c->fill(x, 0.0f, x + 0.62f, 0.5f, lit == 3 ? kMouseLit : kCapSide, 1.0f,
                    [&](float px, float py) { return sdRoundRect(px, py, mid - 0.045f, 0.13f, mid + 0.045f, 0.33f, 0.04f); });
            if (!label.empty())
                drawTextCentred(c, label, mid, 0.68f, 0.2f, kCapText);
            if (wheel >= 0)
                drawArrow(c, x + 0.76f, wheel == 0 ? 0.32f : 0.68f, wheel == 0 ? 0 : 1, 0.12f, kWhite);
            return w;
        }

        float mouseElement(Canvas *c, float x, int key)
        {
            using namespace platform;
            switch (key)
            {
            case KEY_MOUSE_LEFT: return mouse(c, x, 1, -1, "");
            case KEY_MOUSE_RIGHT: return mouse(c, x, 2, -1, "");
            case KEY_MOUSE_MIDDLE: return mouse(c, x, 3, -1, "");
            case KEY_MOUSE_X1: return mouse(c, x, 0, -1, "4");
            case KEY_MOUSE_X2: return mouse(c, x, 0, -1, "5");
            case KEY_WHEEL_UP: return mouse(c, x, 3, 0, "");
            default: return mouse(c, x, 3, 1, "");
            }
        }

        float keys(Canvas *c, const std::string &list)
        {
            std::vector<int> codes;
            std::stringstream ss(list);
            std::string item;
            while (std::getline(ss, item, ','))
                codes.push_back(std::atoi(item.c_str()));
            if (codes.empty())
                codes.push_back(0);
            constexpr float gap = 0.1f, margin = 0.04f;
            float x = margin;
            for (size_t i = 0; i < codes.size(); ++i)
            {
                if (i)
                    x += gap;
                x += codes[i] >= platform::KEY_MOUSE_LEFT ? mouseElement(c, x, codes[i]) : keycap(c, x, codes[i]);
            }
            return x + margin;
        }

        float mouseMove(Canvas *c)
        {
            if (c)
            {
                drawArrow(c, 0.14f, 0.5f, 2, 0.11f, kWhite);
                drawArrow(c, 1.06f, 0.5f, 3, 0.11f, kWhite);
            }
            mouse(c, 0.29f, 0, -1, "");
            return 1.2f;
        }

        // the glyph named by spec: drawn into c when given; its width either way
        float compose(const std::string &spec, Canvas *c)
        {
            const size_t a = spec.find(':');
            const std::string kind = spec.substr(0, a);
            const std::string rest = a == std::string::npos ? "" : spec.substr(a + 1);
            if (kind == "keys")
                return keys(c, rest);
            if (kind == "mouse")
                return mouseMove(c);
            if (kind != "pad")
                return keys(c, "0");
            const size_t b = rest.find(':');
            const std::string family = rest.substr(0, b), button = b == std::string::npos ? "" : rest.substr(b + 1);
            const bool xbox = family == "xbox";
            if (button == "south" || button == "east" || button == "west" || button == "north")
                return faceButton(c, family, button);
            if (button == "back")
                return xbox ? xboxView(c, false) : pill(c, "SELECT", 0.24f, 1.4f, false);
            if (button == "start")
                return xbox ? xboxView(c, true) : pill(c, "START", 0.24f, 1.4f, false);
            if (button == "lb" || button == "rb")
                return pill(c, xbox ? (button == "lb" ? "LB" : "RB") : (button == "lb" ? "L1" : "R1"), 0.3f, 1.3f, false);
            if (button == "lt" || button == "rt")
                return pill(c, xbox ? (button == "lt" ? "LT" : "RT") : (button == "lt" ? "L2" : "R2"), 0.3f, 1.2f, true);
            if (button == "ls" || button == "rs")
                return stick(c, family, button == "ls", 0, true);
            auto dirs = [](const std::string &suffix) {
                return suffix == "_u" ? 1 : suffix == "_d" ? 2 : suffix == "_l" ? 4 : suffix == "_r" ? 8 : suffix == "_ud" ? 3 : suffix == "_lr" ? 12 : 15;
            };
            if (button.rfind("dpad", 0) == 0)
                return dpad(c, dirs(button.substr(4)));
            if (button.rfind("lstick", 0) == 0)
                return stick(c, family, true, dirs(button.substr(6)), false);
            if (button.rfind("rstick", 0) == 0)
                return stick(c, family, false, dirs(button.substr(6)), false);
            return faceButton(c, family, "south");
        }
    }

    float width(const std::string &spec) { return compose(spec, nullptr); }

    void render(const std::string &spec, int w, int h, uint32_t *dst, int stride)
    {
        if (w <= 0 || h <= 0)
            return;
        Canvas c;
        c.w = w;
        c.h = h;
        c.px.assign(static_cast<size_t>(w) * h * 4, 0.0f);
        // drawn at its natural width, centred in the w pixels given
        const float natural = compose(spec, nullptr) * h;
        Canvas *target = &c;
        Canvas wide;
        if (std::fabs(natural - w) > 0.5f)
        {
            wide.w = std::max(1, static_cast<int>(std::ceil(natural)));
            wide.h = h;
            wide.px.assign(static_cast<size_t>(wide.w) * h * 4, 0.0f);
            target = &wide;
        }
        compose(spec, target);
        if (target != &c) // resampled horizontally into w (box / linear)
        {
            const float k = static_cast<float>(wide.w) / w;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                    const float sx0 = x * k, sx1 = (x + 1) * k;
                    float acc[4] = {}, wsum = 0.0f;
                    for (int sx = static_cast<int>(sx0); sx < std::min(wide.w, static_cast<int>(std::ceil(sx1))); ++sx)
                    {
                        const float wgt = std::min(sx1, sx + 1.0f) - std::max(sx0, static_cast<float>(sx));
                        if (wgt <= 0.0f)
                            continue;
                        const float *p = &wide.px[(static_cast<size_t>(y) * wide.w + sx) * 4];
                        for (int i = 0; i < 4; ++i)
                            acc[i] += p[i] * wgt;
                        wsum += wgt;
                    }
                    float *q = &c.px[(static_cast<size_t>(y) * w + x) * 4];
                    for (int i = 0; i < 4; ++i)
                        q[i] = wsum > 0.0f ? acc[i] / wsum : 0.0f;
                }
        }
        // straight alpha; transparent pixels take their lit neighbours' colour (two passes)
        std::vector<float> rgb(static_cast<size_t>(w) * h * 3, 0.0f);
        std::vector<uint8_t> known(static_cast<size_t>(w) * h, 0);
        for (size_t i = 0; i < known.size(); ++i)
            if (c.px[i * 4 + 3] > 1e-4f)
            {
                for (int k = 0; k < 3; ++k)
                    rgb[i * 3 + k] = c.px[i * 4 + k] / c.px[i * 4 + 3];
                known[i] = 1;
            }
        for (int pass = 0; pass < 2; ++pass)
        {
            std::vector<uint8_t> next = known;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                    const size_t i = static_cast<size_t>(y) * w + x;
                    if (known[i])
                        continue;
                    float acc[3] = {};
                    int n = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx)
                        {
                            const int nx = x + dx, ny = y + dy;
                            if (nx < 0 || ny < 0 || nx >= w || ny >= h)
                                continue;
                            const size_t j = static_cast<size_t>(ny) * w + nx;
                            if (!known[j])
                                continue;
                            for (int k = 0; k < 3; ++k)
                                acc[k] += rgb[j * 3 + k];
                            ++n;
                        }
                    if (n)
                    {
                        for (int k = 0; k < 3; ++k)
                            rgb[i * 3 + k] = acc[k] / n;
                        next[i] = 1;
                    }
                }
            known.swap(next);
        }
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                const size_t i = static_cast<size_t>(y) * w + x;
                auto byte = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
                dst[static_cast<size_t>(y) * stride + x] = byte(rgb[i * 3]) | (byte(rgb[i * 3 + 1]) << 8) |
                                                          (byte(rgb[i * 3 + 2]) << 16) | (byte(c.px[i * 4 + 3]) << 24);
            }
    }
}
