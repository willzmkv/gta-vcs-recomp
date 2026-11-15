#include "widgets.h"

#include "model/sound.h"
#include "render/draw.h"
#include "ui/theme.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace ui
{
    using namespace render;
    using core::fade;
    using core::mix;
    using core::rgb;
    using core::withAlpha;

    namespace
    {
        // the label's baseline that centres the text's cap height in r
        float centredBaseline(Font font, float size, const Rect &r)
        {
            const FontMetrics m = metrics(font, size);
            return std::floor(r.cy() + (m.ascent - m.descent) * 0.5f + 0.5f);
        }

        void focusRing(const Rect &r, float amount, float radius)
        {
            if (amount <= 0.01f)
                return;
            const float g = px(3);
            outline({r.x - g, r.y - g, r.w + 2 * g, r.h + 2 * g}, withAlpha(theme::kPink, 0.85f * amount), px(2),
                    radius + g);
        }

        struct ScrollState
        {
            float pos = 0, target = 0, content = 0;
            Rect view;
            float bar = 0;          // scrollbar visibility 0..1
            double lastScroll = -10;
            bool dragging = false;
            float dragMouse = 0, dragPos = 0;
            bool stick = true;      // at the end: a log follows it
            bool toEnd = false;
            uint64_t frame = 0;
        };
        std::unordered_map<Id, ScrollState> g_scrolls;
    }

    // ---- text -----------------------------------------------------------------------------------------------------

    void label(Font font, float size, Rect r, Color color, std::string_view text, Align align)
    {
        const std::string shown = ellipsize(font, size, text, r.w);
        const float w = measure(font, size, shown);
        const float x = align == Align::Left ? r.x : align == Align::Centre ? r.cx() - w * 0.5f : r.right() - w;
        drawText(font, size, x, centredBaseline(font, size, r), color, shown);
    }

    float paragraphHeight(Font font, float size, float width, std::string_view text, float lineHeight)
    {
        if (text.empty())
            return 0;
        return float(wrap(font, size, text, width).size()) * std::ceil(size * lineHeight);
    }

    float paragraph(Font font, float size, Rect r, Color color, std::string_view text, float lineHeight)
    {
        if (text.empty())
            return 0;
        const float step = std::ceil(size * lineHeight);
        const FontMetrics m = metrics(font, size);
        float y = r.y;
        for (const std::string &line : wrap(font, size, text, r.w))
        {
            drawText(font, size, r.x, y + std::floor((step + m.ascent - m.descent) * 0.5f), color, line);
            y += step;
        }
        return y - r.y;
    }

    void eyebrow(float x, float y, Color color, std::string_view text)
    {
        // small capitals with a little tracking: one glyph at a time
        const float size = px(theme::kEyebrow);
        float pen = x;
        for (size_t at = 0; at < text.size();)
        {
            const size_t start = at;
            nextCodepoint(text, at);
            pen = drawText(kFontSemibold, size, pen, y + metrics(kFontSemibold, size).ascent, color,
                           text.substr(start, at - start)) + px(1.1f);
        }
    }

    // ---- controls -------------------------------------------------------------------------------------------------

    // what a label shows: the text before "##" (what follows only tells widgets with the same text apart)
    std::string_view shownText(std::string_view label) { return label.substr(0, label.find("##")); }

    namespace
    {
        // a button's badge: small capitals on solid pink
        constexpr float kBadgeText = 10.5f;
        float badgeWidth(std::string_view badge)
        {
            return badge.empty() ? 0 : std::ceil(measure(kFontSemibold, px(kBadgeText), badge) + px(14));
        }
        void drawBadge(float x, float cy, std::string_view badge)
        {
            const Rect r{x, std::round(cy - px(9)), badgeWidth(badge), px(18)};
            shadow(r.offset(0, px(2)), withAlpha(theme::kPink, 0.35f), px(4), r.h * 0.5f);
            gradient(r, theme::kPinkHi, theme::kPink, r.h * 0.5f);
            drawText(kFontSemibold, px(kBadgeText), r.x + px(7), centredBaseline(kFontSemibold, px(kBadgeText), r), theme::kOnPink, badge);
        }
    }

    float buttonWidth(std::string_view label, char32_t icon, float size, Font font, std::string_view badge)
    {
        const std::string_view text = shownText(label);
        const float s = size > 0 ? size : px(theme::kBody);
        return std::ceil(measure(font, s, text) + px(36) + (icon ? s + px(10) : 0) + (badge.empty() ? 0 : badgeWidth(badge) + px(10)));
    }

    bool button(std::string_view label, Rect r, ButtonStyle style, char32_t icon, bool enabled, float size, Font font,
                std::string_view badge)
    {
        const Interaction in = interact(id(label), r, true, enabled);
        const std::string_view text = shownText(label);
        Widget &w = *in.w;
        const float s = size > 0 ? size : px(theme::kBody);
        const float radius = std::min(px(theme::kRadiusSmall + 2), r.h * 0.5f);
        // hovering lifts it a little; pressing sinks it back and shrinks it slightly
        const float lift = (w.hover - w.press) * px(1);
        Rect box = r.scaled(1.0f - 0.025f * w.press).offset(0, -lift);
        render::pushOpacity(enabled ? 1.0f : 0.45f);
        Color textColor = theme::kText;
        switch (style)
        {
        case ButtonStyle::Primary:
            shadow(box.offset(0, px(6)), withAlpha(theme::kPink, 0.22f + 0.30f * w.hover), px(14 + 6 * w.hover), radius);
            gradient(box, mix(theme::kPink, rgb(0xFFFFFF), 0.10f * w.hover), mix(theme::kPinkHi, rgb(0xFFFFFF), 0.10f * w.hover),
                     radius, Gradient::Horizontal);
            // a highlight along the top edge: light from above
            gradient({box.x, box.y, box.w, box.h * 0.5f}, rgb(0xFFFFFF, 40), rgb(0xFFFFFF, 0), {radius, radius, 0, 0});
            textColor = theme::kOnPink;
            break;
        case ButtonStyle::Secondary:
            rectEx(box, mix(theme::kSurfaceHi, theme::kSurfaceTop, w.hover), mix(theme::kSurfaceHi, theme::kSurfaceTop, w.hover * 0.6f),
                   radius, theme::ink(0.07f + 0.06f * w.hover), hairline());
            break;
        case ButtonStyle::Ghost:
            rect(box, theme::ink(0.07f * w.hover + 0.04f * w.press), radius);
            textColor = mix(theme::kDim, theme::kText, w.hover);
            break;
        case ButtonStyle::Danger:
            rectEx(box, mix(theme::kDangerFill, theme::kDangerFillHi, w.hover), mix(theme::kDangerFill, theme::kDangerFillHi, w.hover), radius,
                   withAlpha(theme::kRed, 0.35f), hairline());
            textColor = mix(theme::kDangerText, theme::kText, 0.6f * w.hover);
            break;
        }
        const float textW = measure(font, s, text);
        const float iconW = icon ? s + px(10) : 0;
        const float badgeW = badge.empty() ? 0 : badgeWidth(badge) + px(10);
        float x = std::floor(box.cx() - (textW + iconW + badgeW) * 0.5f);
        if (icon)
        {
            drawIcon(kFontIcons, s * 0.95f, x + s * 0.5f, box.cy(), textColor, icon);
            x += iconW;
        }
        drawText(font, s, x, centredBaseline(font, s, box), textColor, text);
        if (!badge.empty())
            drawBadge(x + textW + px(10), box.cy(), badge);
        render::popOpacity();
        focusRing(r, w.focus, radius);
        return in.clicked && enabled;
    }

    bool iconButton(std::string_view idLabel, char32_t icon, Rect r, std::string_view tip, bool enabled, float iconSize)
    {
        const Interaction in = interact(id(idLabel), r, true, enabled);
        Widget &w = *in.w;
        const float radius = std::min(px(theme::kRadiusSmall), r.h * 0.5f);
        rect(r.scaled(1.0f - 0.06f * w.press), theme::ink(0.08f * w.hover + 0.05f * w.press), radius);
        const Color c = enabled ? mix(theme::kDim, theme::kText, w.hover) : theme::kFaint;
        drawIcon(kFontIcons, iconSize > 0 ? iconSize : px(15), r.cx(), r.cy(), c, icon);
        focusRing(r, w.focus, radius);
        tooltip(id(idLabel), r, tip);
        return in.clicked && enabled;
    }

    bool segmented(std::string_view idLabel, Rect r, const std::vector<std::string> &options, int &selected, bool enabled)
    {
        pushId(idLabel);
        Widget &w = widget(id("#track"));
        const int n = int(options.size());
        const float seg = r.w / float(std::max(n, 1));
        if (w.fresh)
            w.spring.snap(float(selected));
        spring(w.spring, float(selected), 420, 0.82f);
        const float radius = px(theme::kRadiusSmall + 1);
        render::pushOpacity(enabled ? 1.0f : 0.5f);
        rectEx(r, theme::kBase, theme::kBase, radius, theme::ink(0.06f), hairline());
        // the thumb slides on a spring to the chosen segment
        const Rect thumb = Rect{r.x + w.spring.value * seg, r.y, seg, r.h}.inset(px(3));
        shadow(thumb.offset(0, px(2)), theme::shade(0.67f), px(6), radius - px(3));
        rectEx(thumb, theme::kSurfaceTop, theme::kSurfaceHi, radius - px(3), theme::ink(0.08f), hairline());
        bool changed = false;
        for (int i = 0; i < n; ++i)
        {
            const Rect cell{r.x + float(i) * seg, r.y, seg, r.h};
            const Interaction in = interact(id(options[size_t(i)]), cell, true, enabled);
            if (in.clicked && selected != i)
            {
                selected = i;
                changed = true;
            }
            const float near = core::clamp01(1.0f - std::fabs(w.spring.value - float(i)));
            const Color c = mix(mix(theme::kFaint, theme::kDim, in.w->hover), theme::kText, near);
            label(kFontSemibold, px(theme::kSmall), cell.inset(px(8), 0), c, options[size_t(i)], Align::Centre);
            focusRing(cell.inset(px(2)), in.w->focus, radius);
        }
        render::popOpacity();
        popId();
        return changed;
    }

    bool toggle(std::string_view idLabel, Rect r, bool &value, bool enabled)
    {
        const Interaction in = interact(id(idLabel), r, true, enabled);
        Widget &w = *in.w;
        if (in.clicked)
            value = !value;
        if (w.fresh)
            w.spring.snap(value ? 1.0f : 0.0f);
        spring(w.spring, value ? 1.0f : 0.0f, 500, 0.7f);
        const float t = core::clamp01(w.spring.value);
        const Rect track = centred(r, px(38), px(22)).offset(r.w * 0.5f - px(19), 0);
        rectEx(track, mix(theme::kSurfaceTop, theme::kPink, t), mix(theme::kSurfaceTop, theme::kPinkHi, t), track.h * 0.5f,
               theme::ink(0.08f * (1 - t)), hairline());
        const float knob = px(16) * (1.0f + 0.12f * w.press);
        const float kx = track.x + px(3) + knob * 0.5f + w.spring.value * (track.w - px(6) - knob);
        shadow({kx - knob * 0.5f, track.cy() - knob * 0.5f + px(1), knob, knob}, theme::shade(0.67f), px(3), knob * 0.5f);
        circle(kx, track.cy(), knob * 0.5f, mix(theme::kDim, rgb(0xFFFFFF), std::max(t, w.hover * 0.5f)));
        focusRing(track, w.focus, track.h * 0.5f);
        return in.clicked;
    }

    bool choiceRow(std::string_view idLabel, Rect r, std::string_view title, std::string_view detail, bool selected,
                   bool enabled)
    {
        const Interaction in = interact(id(idLabel), r, true, enabled);
        Widget &w = *in.w;
        if (w.fresh)
            w.spring.snap(selected ? 1.0f : 0.0f);
        spring(w.spring, selected ? 1.0f : 0.0f, 520, 0.6f);
        rect(r, theme::ink(0.035f * w.hover + 0.03f * core::clamp01(w.spring.value)), px(10));
        const float cx = r.x + px(22), cy = r.y + px(detail.empty() ? 0 : 0) + r.h * 0.5f;
        const float ring = px(9);
        outline({cx - ring, cy - ring, ring * 2, ring * 2}, mix(theme::kFaint, theme::kPink, core::clamp01(w.spring.value)),
                px(1.6f), ring);
        const float dot = px(4.5f) * std::max(0.0f, w.spring.value);
        if (dot > 0.2f)
            circle(cx, cy, dot, theme::kPink);
        const float tx = r.x + px(44);
        if (detail.empty())
            label(kFontBody, px(theme::kBody), {tx, r.y, r.right() - tx - px(12), r.h}, theme::kText, title);
        else
        {
            label(kFontSemibold, px(theme::kBody), {tx, r.y + r.h * 0.5f - px(19), r.right() - tx - px(12), px(20)},
                  theme::kText, title);
            label(kFontBody, px(theme::kSmall), {tx, r.y + r.h * 0.5f + px(1), r.right() - tx - px(12), px(18)},
                  theme::kDim, detail);
        }
        focusRing(r, w.focus, px(10));
        return in.clicked && enabled;
    }

    bool playGameButton(std::string_view label, Rect r, char32_t icon, bool enabled)
    {
        const Interaction in = interact(id(label), r, true, enabled);
        const std::string_view text = shownText(label);
        Widget &w = *in.w;

        // Dynamic 1980s slanted quad polygon matching exemplo_formato.png & setup.cpp kShapes[0]:
        // Left side narrower (~88% height), right side taller (~112% height), top slopes up to the right.
        const float lift = (w.hover - w.press) * px(1.5f);
        const Rect box = r.scaled(1.0f - 0.04f * w.press).offset(0, -lift);

        const float corners[8] = {
            box.x - px(5.0f), box.y + px(3.5f),
            box.right() + px(7.0f), box.y - px(4.5f),
            box.right() + px(4.0f), box.bottom() + px(5.5f),
            box.x - px(7.0f), box.bottom() - px(2.5f)
        };

        render::pushOpacity(enabled ? 1.0f : 0.50f);

        // Soft drop shadow polygon behind the button
        const float shadowCorners[8] = {
            corners[0], corners[1] + px(5.0f),
            corners[2], corners[3] + px(5.0f),
            corners[4], corners[5] + px(5.0f),
            corners[6], corners[7] + px(5.0f)
        };
        quad(shadowCorners, rgb(0x000000, 150));

        const float phase = float(std::fmod(state.time * 0.85, 1.0));
        if (w.hover > 0.005f && enabled)
            keepAnimating();

        // Bottom neon glow (Vice City Pink -> Electric Cyan on hover)
        shadow(box.offset(0, px(4)),
               withAlpha(mix(rgb(0xFF2A85), rgb(0x27FDF5), w.hover * 0.65f), 0.42f + 0.35f * w.hover),
               px(18 + 8 * w.hover), px(4));

        // Base polygon fill: Vice City Flamingo Pink (#FE7FB4) transitioning to Electric Cyan (#00C6DA) on hover
        const Color baseCol = mix(rgb(0xFE7FB4), rgb(0x00C6DA), w.hover);
        quad(corners, baseCol);

        if (w.hover > 0.005f)
        {
            // Geometrically watertight VHS plasma and CRT effects computed strictly on the slanted quad vertices:
            const float p0x = corners[0] + px(1.0f), p0y = corners[1] + px(1.0f); // Top-left
            const float p1x = corners[2] - px(1.0f), p1y = corners[3] + px(1.0f); // Top-right
            const float p2x = corners[4] - px(1.0f), p2y = corners[5] - px(1.0f); // Bottom-right
            const float p3x = corners[6] + px(1.0f), p3y = corners[7] - px(1.0f); // Bottom-left

            auto lerpL = [p0x, p0y, p3x, p3y](float t) {
                return std::make_pair(p0x + (p3x - p0x) * t, p0y + (p3y - p0y) * t);
            };
            auto lerpR = [p1x, p1y, p2x, p2y](float t) {
                return std::make_pair(p1x + (p2x - p1x) * t, p1y + (p2y - p1y) * t);
            };

            // 1. Center glowing phosphor core (drifting electric cyan hotspot)
            const float coreWave = 0.5f + 0.5f * std::sin(float(state.time * 3.8));
            const float cTop = 0.20f + 0.04f * coreWave, cBot = 0.80f - 0.04f * coreWave;
            const auto [cL0x, cL0y] = lerpL(cTop);
            const auto [cR0x, cR0y] = lerpR(cTop);
            const auto [cR1x, cR1y] = lerpR(cBot);
            const auto [cL1x, cL1y] = lerpL(cBot);
            const float coreCorners[8] = {
                cL0x + px(10.0f), cL0y,
                cR0x - px(10.0f), cR0y,
                cR1x - px(10.0f), cR1y,
                cL1x + px(10.0f), cL1y
            };
            quad(coreCorners, withAlpha(rgb(0x7CF9FF), 0.35f * w.hover));

            // 2. Continuous Top & Bottom Vice City Hot-Pink brush bands (strictly bounded quad strips)
            const auto [topBandL_x, topBandL_y] = lerpL(0.30f);
            const auto [topBandR_x, topBandR_y] = lerpR(0.30f);
            const float topBand[8] = {
                p0x, p0y,
                p1x, p1y,
                topBandR_x, topBandR_y,
                topBandL_x, topBandL_y
            };
            quad(topBand, withAlpha(rgb(0xFF187C), 0.50f * w.hover));

            const auto [botBandL_x, botBandL_y] = lerpL(0.70f);
            const auto [botBandR_x, botBandR_y] = lerpR(0.70f);
            const float botBand[8] = {
                botBandL_x, botBandL_y,
                botBandR_x, botBandR_y,
                p2x, p2y,
                p3x, p3y
            };
            quad(botBand, withAlpha(rgb(0xFF187C), 0.55f * w.hover));

            // 3. Dark VHS tracking line sweeping along lower quad segment
            const float trackT0 = 0.62f, trackT1 = 0.82f;
            const auto [tL0x, tL0y] = lerpL(trackT0);
            const auto [tR0x, tR0y] = lerpR(trackT0);
            const auto [tR1x, tR1y] = lerpR(trackT1);
            const auto [tL1x, tL1y] = lerpL(trackT1);
            const float u0 = core::clamp01(phase - 0.25f);
            const float u1 = core::clamp01(phase + 0.35f);
            if (u1 > u0)
            {
                const float barCorners[8] = {
                    tL0x + (tR0x - tL0x) * u0, tL0y + (tR0y - tL0y) * u0,
                    tL0x + (tR0x - tL0x) * u1, tL0y + (tR0y - tL0y) * u1,
                    tL1x + (tR1x - tL1x) * u1, tL1y + (tR1y - tL1y) * u1,
                    tL1x + (tR1x - tL1x) * u0, tL1y + (tR1y - tL1y) * u0,
                };
                quad(barCorners, withAlpha(rgb(0x002631), 0.40f * w.hover));
            }

            // 4. Fine horizontal CRT scanlines strictly inside polygon endpoints
            for (int i = 1; i <= 18; ++i)
            {
                const float v = float(i) / 19.0f;
                const auto [lx, ly] = lerpL(v);
                const auto [rx, ry] = lerpR(v);
                line(lx + px(5.0f), ly, rx - px(5.0f), ry, px(1.0f), withAlpha(rgb(0x00222B), 0.22f * w.hover));
            }
        }

        // Left signature accent bar along the slanted left edge of the polygon
        line(corners[0], corners[1], corners[6], corners[7], px(5.0f), mix(rgb(0x27FDF5), rgb(0xFFFFFF), w.hover));

        // Perimeter crisp outline around the 4 slanted quad edges
        const Color borderCol = mix(rgb(0xFFFFFF, 215), rgb(0x27FDF5, 255), w.hover);
        line(corners[0], corners[1], corners[2], corners[3], px(1.6f), borderCol);
        line(corners[2], corners[3], corners[4], corners[5], px(1.6f), borderCol);
        line(corners[4], corners[5], corners[6], corners[7], px(1.6f), borderCol);
        line(corners[6], corners[7], corners[0], corners[1], px(1.6f), borderCol);

        const float fontSize = px(18.5f);
        const float iconSize = px(20.0f);
        const float gap = px(12.0f);
        const float textW = measure(kFontSemibold, fontSize, text);
        const float totalW = (icon ? iconSize + gap : 0.0f) + textW;
        float x = std::floor(box.cx() - totalW * 0.5f);
        const Color shadowCol = mix(rgb(0x28091E, 145), rgb(0x00222C, 225), w.hover);
        if (icon)
        {
            const float iconCx = x + iconSize * 0.5f + px(1.5f);
            drawIcon(kFontIcons, iconSize, iconCx, box.cy() + px(1.5f), shadowCol, icon);
            drawIcon(kFontIcons, iconSize, iconCx, box.cy(), rgb(0xFFFFFF), icon);
            x += iconSize + gap;
        }
        const float baseY = centredBaseline(kFontSemibold, fontSize, box);
        drawText(kFontSemibold, fontSize, x, baseY + px(1.5f), shadowCol, text);
        drawText(kFontSemibold, fontSize, x, baseY, rgb(0xFFFFFF), text);
        render::popOpacity();
        focusRing(r, w.focus, px(4));

        if (in.clicked && enabled)
        {
            sound::play(sound::Sfx::Play);
            return true;
        }
        return false;
    }

    bool rockstarButton(std::string_view idLabel, Rect r, char32_t icon, std::string_view line1,
                        std::string_view line2, bool enabled)
    {
        const Interaction in = interact(id(idLabel), r, true, enabled);
        Widget &w = *in.w;
        // Square-cornered 1980s Vice City Menu geometry (px(2)) with Ice-Cyan surface & Flamingo Pink hover
        const float radius = px(2);
        const float lift = (w.hover - w.press) * px(1.2f);
        const Rect box = r.scaled(1.0f - 0.025f * w.press).offset(0, -lift);

        render::pushOpacity(enabled ? 1.0f : 0.48f);
        shadow(box.offset(0, px(4)), rgb(0x002833, 135), px(11), radius);
        if (w.hover > 0.01f)
            shadow(box.offset(0, px(2)), withAlpha(rgb(0xFE7FB4), 0.40f * w.hover), px(12), radius);

        // Ice-Cyan (#E2F6FA -> #C8ECF4) idle surface matching the Vice City Options menu, shifting to Flamingo Pink (#FE7FB4) on hover
        const Color topFill = mix(rgb(0xE6F8FC, 240), rgb(0xFE7FB4, 252), w.hover);
        const Color botFill = mix(rgb(0xC9EDF5, 236), rgb(0xF0569C, 252), w.hover);
        const Color borderCol = mix(rgb(0x007A96, 195), rgb(0xFFFFFF, 245), w.hover);
        rectEx(box, topFill, botFill, radius, borderCol, px(1.4f));

        // Left signature Vice City accent stripe (Pink when idle, Electric Cyan when hovered)
        const Color stripeCol = mix(rgb(0xFE7FB4), rgb(0x27FDF5), w.hover);
        rect({box.x, box.y, px(5.5f), box.h}, stripeCol);

        const Color fgCol = mix(rgb(0x00647C), rgb(0xFFFFFF), w.hover);

        // Left icon: if Steam, use the official circular Steam vector logo from steam.png; otherwise MDL2 font glyph
        const float iconCx = box.x + px(32);
        if (icon == theme::icon::kSteam)
        {
            const float steamSz = px(22.0f);
            image({iconCx - steamSz * 0.5f, box.cy() - steamSz * 0.5f, steamSz, steamSz}, ImageId::SteamLogo, {}, fgCol);
        }
        else
        {
            drawIcon(kFontIcons, px(23), iconCx, box.cy(), fgCol, icon);
        }

        // Right uppercase text (1 or 2 lines)
        const float textX = box.x + px(58);
        const float textW = box.right() - textX - px(10);
        const float fontSize = px(12.5f);
        if (line2.empty())
        {
            label(kFontSemibold, fontSize, {textX, box.y, textW, box.h}, fgCol, line1, Align::Left);
        }
        else
        {
            const float lineH = px(15.0f);
            const float topY = box.cy() - lineH;
            label(kFontSemibold, fontSize, {textX, topY, textW, lineH}, fgCol, line1, Align::Left);
            label(kFontSemibold, fontSize, {textX, box.cy(), textW, lineH}, fgCol, line2, Align::Left);
        }
        render::popOpacity();
        focusRing(r, w.focus, radius);

        if (in.clicked && enabled)
        {
            sound::play(sound::Sfx::Nav);
            return true;
        }
        return false;
    }

    bool sliderInt(std::string_view idLabel, Rect r, int &value, int minVal, int maxVal, int step,
                   std::string_view suffix, bool enabled)
    {
        const Id wid = id(idLabel);
        const float valW = px(68);
        const Rect trackHit{r.x, r.y, std::max(px(40), r.w - valW - px(8)), r.h};
        const Interaction in = interact(wid, trackHit, true, enabled);
        Widget &w = *in.w;

        bool changed = false;
        const int span = std::max(1, maxVal - minVal);
        if (enabled && (in.pressed || (w.value == 1 && platform::input.down[platform::kMouseLeft])))
        {
            w.value = 1;
            const float pad = px(8);
            const float t = core::clamp01((platform::input.mouseX - (trackHit.x + pad)) / std::max(1.0f, trackHit.w - 2 * pad));
            int raw = minVal + int(std::round(t * float(span)));
            if (step > 1)
                raw = minVal + ((raw - minVal + step / 2) / step) * step;
            raw = std::clamp(raw, minVal, maxVal);
            if (raw != value)
            {
                value = raw;
                changed = true;
            }
        }
        else if (!platform::input.down[platform::kMouseLeft])
        {
            w.value = 0;
        }

        const float frac = core::clamp01(float(value - minVal) / float(span));
        if (w.fresh)
            w.t = frac;
        animate(w.t, frac, 18.0f);

        render::pushOpacity(enabled ? 1.0f : 0.45f);
        const float trackH = px(6);
        const Rect track{trackHit.x + px(8), trackHit.cy() - trackH * 0.5f, trackHit.w - px(16), trackH};
        rect(track, rgb(0xFFFFFF, 28), trackH * 0.5f);
        const float fillW = std::max(trackH, track.w * core::clamp01(w.t));
        if (w.t > 0.005f)
            gradient({track.x, track.y, fillW, track.h}, theme::kPink, theme::kPinkHi, trackH * 0.5f, Gradient::Horizontal);

        const float knobX = track.x + track.w * core::clamp01(w.t);
        const float knobR = px(7.5f) + px(1.5f) * std::max(w.hover, float(w.value));
        shadow({knobX - knobR, track.cy() - knobR + px(1), knobR * 2, knobR * 2}, rgb(0x000000, 140), px(4), knobR);
        circle(knobX, track.cy(), knobR, rgb(0xFFFFFF));
        circle(knobX, track.cy(), knobR * 0.45f, theme::kPink);

        std::string valText = std::to_string(value);
        if (!suffix.empty())
            valText += std::string(suffix);
        label(kFontSemibold, px(12.5f), {trackHit.right() + px(6), r.y, valW, r.h}, theme::kText, valText, Align::Right);
        render::popOpacity();
        focusRing(trackHit, w.focus, px(6));
        return changed;
    }

    void perfBadge(Rect r, PerfImpact impact, std::string_view text)
    {
        Color col = theme::kTeal;
        switch (impact)
        {
        case PerfImpact::Zero:
        case PerfImpact::Low: col = rgb(0x2EE59D); break;
        case PerfImpact::Medium: col = rgb(0xFFB020); break;
        case PerfImpact::High: col = rgb(0xFF5C77); break;
        case PerfImpact::Extreme: col = rgb(0xFF2A55); break;
        }
        rectEx(r, withAlpha(col, 0.14f), withAlpha(col, 0.10f), r.h * 0.5f, withAlpha(col, 0.38f), hairline());
        circle(r.x + px(10), r.cy(), px(3.2f), col);
        label(kFontSemibold, px(10.5f), {r.x + px(18), r.y, r.w - px(24), r.h}, col, text, Align::Left);
    }

    // ---- Classic 1980s GTA: Vice City Menu Components & C64 Terminal ----------------------------------------------

    namespace
    {
        void drawVcChevron(float cx, float cy, bool rightDir, Color col, float scale = 1.0f)
        {
            const float dx = px(4.5f) * scale * (rightDir ? 1.0f : -1.0f);
            const float dy = px(7.5f) * scale;
            const float thick = px(2.5f);
            line(cx - dx, cy - dy, cx + dx, cy, thick, col);
            line(cx + dx, cy, cx - dx, cy + dy, thick, col);
        }

        void drawVcRowStripe(Rect r, int rowIndex, float hover, float press)
        {
            const Color baseBg = (rowIndex % 2 == 0) ? rgb(0xFBFDFE, 236) : rgb(0xE8F7FA, 228);
            const Color hoverBg = rgb(0xFFE3F1, 246);
            const Color fill = mix(baseBg, hoverBg, hover * 0.85f + press * 0.15f);
            rect(r, fill);
            if (hover > 0.01f)
            {
                rect({r.x, r.y, px(4.5f) * hover, r.h}, rgb(0xFE7FB4));
            }
        }
    }

    bool vcSubTab(std::string_view idLabel, Rect r, std::string_view text, bool active)
    {
        const Interaction in = interact(id(idLabel), r, true);
        Widget &w = *in.w;
        if (w.fresh)
            w.t = active ? 1.0f : 0.0f;
        animate(w.t, active ? 1.0f : 0.0f, 18.0f);

        const Color inactiveBg = mix(rgb(0xD4F0F6, 215), rgb(0xE6F8FC, 238), w.hover);
        const Color activeBg = rgb(0xFE7FB4);
        const Color bg = mix(inactiveBg, activeBg, w.t);
        rect(r.scaled(1.0f - 0.03f * w.press), bg);

        // Right 1px divider between tabs
        if (w.t < 0.9f)
            rect({r.right() - 1, r.y + px(6), 1, r.h - px(12)}, rgb(0x0084A1, 42));

        const Color textCol = mix(rgb(0x007A96), rgb(0xFFFFFF), w.t);
        const float fontSize = (r.w < px(135.0f)) ? px(11.0f) : px(13.0f);
        const float padX = (r.w < px(135.0f)) ? px(3.0f) : px(6.0f);
        label(kFontSemibold, fontSize, r.inset(padX, 0), textCol, text, Align::Centre);
        focusRing(r, w.focus, px(2));

        if (in.clicked)
        {
            sound::play(sound::Sfx::Nav);
            return true;
        }
        return false;
    }

    bool vcCycleRow(std::string_view idLabel, Rect r, int rowIndex, std::string_view title,
                    const std::vector<std::string> &options, int &selected, bool *hoveredOut, bool enabled)
    {
        pushId(idLabel);
        const Interaction rowIn = interact(id("#row"), r, false, enabled);
        const float rightAreaW = std::min(px(280), r.w * 0.46f);
        const Rect ctrlR{r.right() - rightAreaW - px(14), r.y, rightAreaW, r.h};
        const Rect prevHit{ctrlR.x, ctrlR.y, px(44), ctrlR.h};
        const Rect nextHit{ctrlR.right() - px(44), ctrlR.y, px(44), ctrlR.h};
        const Rect valHit{prevHit.right(), ctrlR.y, nextHit.x - prevHit.right(), ctrlR.h};

        const Interaction prevIn = interact(id("#prev"), prevHit, true, enabled);
        const Interaction nextIn = interact(id("#next"), nextHit, false, enabled);
        const Interaction valIn = interact(id("#val"), valHit, false, enabled);

        const float hover = std::max({rowIn.w->hover, prevIn.w->hover, nextIn.w->hover, valIn.w->hover});
        const float press = std::max({prevIn.w->press, nextIn.w->press, valIn.w->press});
        if (hoveredOut && hover > 0.1f)
            *hoveredOut = true;

        render::pushOpacity(enabled ? 1.0f : 0.48f);
        drawVcRowStripe(r, rowIndex, hover, press);

        const Color tealText = mix(rgb(0x007A96), rgb(0x00586E), hover);
        label(kFontSemibold, px(14.5f), {r.x + px(22), r.y, ctrlR.x - r.x - px(30), r.h}, tealText, title, Align::Left);

        const int n = std::max(1, int(options.size()));
        selected = std::clamp(selected, 0, n - 1);
        bool changed = false;
        if (enabled && (prevIn.clicked || (prevIn.w->focus > 0.5f && platform::input.key(platform::kKeyLeft))))
        {
            selected = (selected - 1 + n) % n;
            changed = true;
        }
        else if (enabled && (nextIn.clicked || valIn.clicked ||
                             (prevIn.w->focus > 0.5f && platform::input.key(platform::kKeyRight))))
        {
            selected = (selected + 1) % n;
            changed = true;
        }

        if (changed)
            sound::play(sound::Sfx::Nav);

        const Color prevCol = mix(rgb(0x007A96), rgb(0xE03B8B), prevIn.w->hover);
        const Color nextCol = mix(rgb(0x007A96), rgb(0xE03B8B), nextIn.w->hover);
        drawVcChevron(prevHit.cx(), prevHit.cy(), false, prevCol, 1.0f - 0.08f * prevIn.w->press);
        drawVcChevron(nextHit.cx(), nextHit.cy(), true, nextCol, 1.0f - 0.08f * nextIn.w->press);

        const std::string_view shownVal = options.empty() ? std::string_view("-") : std::string_view(options[size_t(selected)]);
        label(kFontSemibold, px(14.5f), valHit.inset(px(4), 0), tealText, shownVal, Align::Centre);

        render::popOpacity();
        focusRing(ctrlR.inset(px(2)), prevIn.w->focus, px(4));
        popId();
        return changed;
    }

    bool vcSliderRow(std::string_view idLabel, Rect r, int rowIndex, std::string_view title, int &value, int minVal,
                     int maxVal, int step, std::string_view suffix, bool *hoveredOut, bool enabled)
    {
        pushId(idLabel);
        const Interaction rowIn = interact(id("#row"), r, false, enabled);

        const float rightAreaW = std::min(px(300), r.w * 0.48f);
        const float valW = px(64);
        const float stepBtnW = px(28);
        const Rect ctrlR{r.right() - rightAreaW - px(14), r.y, rightAreaW, r.h};
        const Rect decHit{ctrlR.x, ctrlR.y, stepBtnW, ctrlR.h};
        const Rect trackHit{decHit.right(), ctrlR.y, ctrlR.w - valW - stepBtnW * 2 - px(4), ctrlR.h};
        const Rect incHit{trackHit.right(), ctrlR.y, stepBtnW, ctrlR.h};

        const Interaction decIn = interact(id("#dec"), decHit, false, enabled);
        const Interaction in = interact(id("#track"), trackHit, true, enabled);
        const Interaction incIn = interact(id("#inc"), incHit, false, enabled);
        Widget &w = *in.w;

        bool changed = false;
        const int span = std::max(1, maxVal - minVal);
        const int effStep = std::max(1, step);
        if (enabled && (in.pressed || in.held || (w.value == 1 && platform::input.down[platform::kMouseLeft])))
        {
            w.value = 1;
            const float pad = px(10);
            const float localMouseX = platform::input.mouseX - render::draw.dx;
            const float t = core::clamp01((localMouseX - (trackHit.x + pad)) / std::max(1.0f, trackHit.w - 2 * pad));
            int raw = minVal + int(std::round(t * float(span)));
            if (effStep > 1)
                raw = minVal + ((raw - minVal + effStep / 2) / effStep) * effStep;
            raw = std::clamp(raw, minVal, maxVal);
            if (raw != value)
            {
                value = raw;
                changed = true;
            }
        }
        else if (!platform::input.down[platform::kMouseLeft])
        {
            w.value = 0;
        }

        if (enabled && (decIn.clicked || (w.focus > 0.5f && platform::input.key(platform::kKeyLeft))))
        {
            const int nextVal = std::clamp(value - effStep, minVal, maxVal);
            if (nextVal != value)
            {
                value = nextVal;
                changed = true;
            }
        }
        else if (enabled && (incIn.clicked || (w.focus > 0.5f && platform::input.key(platform::kKeyRight))))
        {
            const int nextVal = std::clamp(value + effStep, minVal, maxVal);
            if (nextVal != value)
            {
                value = nextVal;
                changed = true;
            }
        }

        const float hover = std::max({rowIn.w->hover, in.w->hover, decIn.w->hover, incIn.w->hover, float(w.value)});
        if (hoveredOut && hover > 0.1f)
            *hoveredOut = true;

        const float frac = core::clamp01(float(value - minVal) / float(span));
        if (w.fresh)
            w.t = frac;
        animate(w.t, frac, 20.0f);

        render::pushOpacity(enabled ? 1.0f : 0.48f);
        drawVcRowStripe(r, rowIndex, hover, float(w.value));

        const Color tealText = mix(rgb(0x007A96), rgb(0x00586E), hover);
        label(kFontSemibold, px(14.5f), {r.x + px(22), r.y, ctrlR.x - r.x - px(30), r.h}, tealText, title, Align::Left);

        // Step chevrons < and > flanking the pink slider
        drawVcChevron(decHit.cx(), decHit.cy(), false, mix(rgb(0x007A96), rgb(0xFF2A85), decIn.w->hover), 0.85f);
        drawVcChevron(incHit.cx(), incHit.cy(), true, mix(rgb(0x007A96), rgb(0xFF2A85), incIn.w->hover), 0.85f);

        // High-contrast Vice City Pink horizontal track + filled bar
        const float trackH = px(8);
        const Rect track{trackHit.x + px(10), trackHit.cy() - trackH * 0.5f, trackHit.w - px(20), trackH};
        rectEx(track, rgb(0xBCE3ED), rgb(0xBCE3ED), trackH * 0.5f, rgb(0x007A96, 95), 1.0f);
        const float fillW = std::max(trackH, track.w * core::clamp01(w.t));
        if (w.t > 0.002f)
        {
            gradient({track.x, track.y, fillW, track.h}, rgb(0xFF2A85), rgb(0xFE7FB4), trackH * 0.5f, Gradient::Horizontal);
        }

        // Classic Vice City double-ring target knob (◎) in Pink & White
        const float knobX = track.x + track.w * core::clamp01(w.t);
        const float knobCy = track.cy();
        const float outerR = px(10.5f) * (1.0f + 0.08f * float(w.value));
        shadow({knobX - outerR, knobCy - outerR + px(1), outerR * 2, outerR * 2}, rgb(0x801045, 95), px(4), outerR);
        circle(knobX, knobCy, outerR, rgb(0xFFFFFF));
        outline({knobX - outerR, knobCy - outerR, outerR * 2, outerR * 2}, rgb(0xFF2A85), px(2.2f), outerR);
        circle(knobX, knobCy, outerR * 0.50f, w.value ? rgb(0xFF1476) : rgb(0xFE7FB4));

        // Tabular number readout on the right
        std::string valText = std::to_string(value);
        if (!suffix.empty())
            valText += std::string(suffix);
        label(kFontMono, px(13.0f), {incHit.right() + px(2), r.y, valW - px(6), r.h},
              mix(tealText, rgb(0xD91B70), float(w.value)), valText, Align::Right);

        render::popOpacity();
        focusRing(trackHit, w.focus, px(4));
        popId();
        return changed;
    }

    bool vcModRow(std::string_view idLabel, Rect r, int rowIndex, bool &modEnabled, std::string_view title,
                  int &sliderValue, int minVal, int maxVal, int step, std::string_view suffix,
                  bool *hoveredOut, bool enabled)
    {
        pushId(idLabel);
        const Interaction rowIn = interact(id("#row"), r, false, enabled);

        // 1. Left: Graphical Toggle Switch matching reference (uploaded_media_1794748945526.png)
        // Pill-shaped track + sliding square white beveled knob
        const float togW = px(52);
        const float togH = px(26);
        const Rect togR{r.x + px(18), std::floor(r.cy() - togH * 0.5f), togW, togH};
        const Interaction togIn = interact(id("#toggle"), togR, true, enabled);
        Widget &tw = *togIn.w;

        // 2. Right: High-contrast Vice City Pink Slider [ <  ===◎===  >  Value ]
        const float rightAreaW = std::min(px(275), r.w * 0.42f);
        const float valW = px(58);
        const float stepBtnW = px(24);
        const Rect ctrlR{r.right() - rightAreaW - px(12), r.y, rightAreaW, r.h};
        const Rect decHit{ctrlR.x, ctrlR.y, stepBtnW, ctrlR.h};
        const Rect trackHit{decHit.right(), ctrlR.y, ctrlR.w - valW - stepBtnW * 2 - px(4), ctrlR.h};
        const Rect incHit{trackHit.right(), ctrlR.y, stepBtnW, ctrlR.h};

        const bool sliderActive = enabled && modEnabled;
        const Interaction decIn = interact(id("#dec"), decHit, false, sliderActive);
        const Interaction slIn = interact(id("#slider"), trackHit, true, sliderActive);
        const Interaction incIn = interact(id("#inc"), incHit, false, sliderActive);
        Widget &sw = *slIn.w;

        bool changed = false;
        if (enabled && togIn.clicked)
        {
            modEnabled = !modEnabled;
            sound::play(sound::Sfx::Nav);
            changed = true;
        }

        const int span = std::max(1, maxVal - minVal);
        const int effStep = std::max(1, step);
        if (sliderActive && (slIn.pressed || slIn.held || (sw.value == 1 && platform::input.down[platform::kMouseLeft])))
        {
            sw.value = 1;
            const float pad = px(10);
            const float localMouseX = platform::input.mouseX - render::draw.dx;
            const float t = core::clamp01((localMouseX - (trackHit.x + pad)) / std::max(1.0f, trackHit.w - 2 * pad));
            int raw = minVal + int(std::round(t * float(span)));
            if (effStep > 1)
                raw = minVal + ((raw - minVal + effStep / 2) / effStep) * effStep;
            raw = std::clamp(raw, minVal, maxVal);
            if (raw != sliderValue)
            {
                sliderValue = raw;
                changed = true;
            }
        }
        else if (!platform::input.down[platform::kMouseLeft])
        {
            sw.value = 0;
        }

        if (sliderActive && (decIn.clicked || (sw.focus > 0.5f && platform::input.key(platform::kKeyLeft))))
        {
            const int nextVal = std::clamp(sliderValue - effStep, minVal, maxVal);
            if (nextVal != sliderValue)
            {
                sliderValue = nextVal;
                changed = true;
            }
        }
        else if (sliderActive && (incIn.clicked || (sw.focus > 0.5f && platform::input.key(platform::kKeyRight))))
        {
            const int nextVal = std::clamp(sliderValue + effStep, minVal, maxVal);
            if (nextVal != sliderValue)
            {
                sliderValue = nextVal;
                changed = true;
            }
        }

        const float hover = std::max({rowIn.w->hover, togIn.w->hover, slIn.w->hover, decIn.w->hover, incIn.w->hover, float(sw.value)});
        if (hoveredOut && hover > 0.1f)
            *hoveredOut = true;

        // Animate slider knob
        const float frac = core::clamp01(float(sliderValue - minVal) / float(span));
        if (sw.fresh)
            sw.t = frac;
        animate(sw.t, frac, 20.0f);

        // Animate toggle switch knob with snappy spring
        if (tw.fresh)
            tw.spring.snap(modEnabled ? 1.0f : 0.0f);
        spring(tw.spring, modEnabled ? 1.0f : 0.0f, 520, 0.72f);
        const float togT = core::clamp01(tw.spring.value);

        render::pushOpacity(enabled ? 1.0f : 0.48f);
        drawVcRowStripe(r, rowIndex, hover, std::max(togIn.w->press, float(sw.value)));

        // --- DRAW GRAPHICAL TOGGLE SWITCH (Reference: uploaded_media_1794748945526.png) ---
        // Pill-shaped recessed track
        const float trackRadius = px(7.0f);
        // OFF track: Ice Cyan / Muted Teal (#8EC1CE) | ON track: Vice City Flamingo Pink (#FE7FB4 -> #FF2A85)
        const Color trackOffCol = mix(rgb(0x8EC1CE), rgb(0xA2D0DC), togIn.w->hover);
        const Color trackOnCol = mix(rgb(0xFE7FB4), rgb(0xFF3892), togIn.w->hover);
        const Color trackCol = mix(trackOffCol, trackOnCol, togT);
        const Color trackBorder = mix(rgb(0x005E72, 180), rgb(0xC8145E, 220), togT);

        // Track inset shadow & fill
        shadow(togR.offset(0, px(1)), rgb(0x00222B, 95), px(4), trackRadius);
        rectEx(togR, trackCol, trackCol, trackRadius, trackBorder, 1.2f);

        // Sliding Soft-beveled Square White Knob
        const float knobPad = px(2.5f);
        const float knobSize = togH - knobPad * 2;
        const float knobX = togR.x + knobPad + togT * (togR.w - knobPad * 2 - knobSize);
        const float knobY = togR.y + knobPad;
        const Rect knobR{knobX, knobY, knobSize, knobSize};
        const float knobCorner = px(3.5f);

        // Knob shadow
        shadow(knobR.offset(0, px(1.5f)), rgb(0x001B24, 110), px(4), knobCorner);
        // Beveled white square face
        rectEx(knobR, rgb(0xFFFFFF), rgb(0xEDEBEB), knobCorner, rgb(0xD0CECE), 1.0f);
        // Inner soft bevel highlight
        rectEx(knobR.inset(px(1.5f)), rgb(0xFFFFFF, 240), rgb(0xE2E0E0, 240), px(2.5f), rgb(0xFFFFFF, 180), 0.8f);

        // --- DRAW CENTER MOD NAME ---
        const Color tealText = mix(rgb(0x007A96), rgb(0x00586E), hover);
        const float titleX = togR.right() + px(18);
        label(kFontSemibold, px(14.0f), {titleX, r.y, ctrlR.x - titleX - px(14), r.h}, tealText, title, Align::Left);

        // --- DRAW RIGHT PINK SLIDER ---
        render::pushOpacity(modEnabled ? 1.0f : 0.42f);
        drawVcChevron(decHit.cx(), decHit.cy(), false, mix(rgb(0x007A96), rgb(0xFF2A85), decIn.w->hover), 0.80f);
        drawVcChevron(incHit.cx(), incHit.cy(), true, mix(rgb(0x007A96), rgb(0xFF2A85), incIn.w->hover), 0.80f);

        const float trackH = px(8);
        const Rect track{trackHit.x + px(13), trackHit.cy() - trackH * 0.5f, trackHit.w - px(26), trackH};
        rectEx(track, rgb(0xBCE3ED), rgb(0xBCE3ED), trackH * 0.5f, rgb(0x007A96, 95), 1.0f);
        const float fillW = std::max(trackH, track.w * core::clamp01(sw.t));
        if (sw.t > 0.002f)
            gradient({track.x, track.y, fillW, track.h}, rgb(0xFF2A85), rgb(0xFE7FB4), trackH * 0.5f, Gradient::Horizontal);

        const float kX = track.x + track.w * core::clamp01(sw.t);
        const float kCy = track.cy();
        const float outerR = px(9.2f) * (1.0f + 0.08f * float(sw.value));
        circle(kX, kCy, outerR, rgb(0xFFFFFF));
        outline({kX - outerR, kCy - outerR, outerR * 2, outerR * 2}, rgb(0xFF2A85), px(2.2f), outerR);
        circle(kX, kCy, outerR * 0.50f, sw.value ? rgb(0xFF1476) : rgb(0xFE7FB4));

        std::string valText = std::to_string(sliderValue);
        if (!suffix.empty())
            valText += std::string(suffix);
        label(kFontMono, px(12.5f), {incHit.right() + px(2), r.y, valW - px(6), r.h},
              mix(tealText, rgb(0xD91B70), float(sw.value)), valText, Align::Right);
        render::popOpacity();

        render::popOpacity();
        focusRing(togR, tw.focus, trackRadius);
        focusRing(trackHit, sw.focus, px(2));
        popId();
        return changed;
    }

    bool vcActionRow(std::string_view idLabel, Rect r, int rowIndex, std::string_view title,
                     std::string_view rightText, Color rightColor, bool *hoveredOut, bool enabled)
    {
        const Interaction in = interact(id(idLabel), r, true, enabled);
        Widget &w = *in.w;
        if (hoveredOut && w.hover > 0.1f)
            *hoveredOut = true;

        render::pushOpacity(enabled ? 1.0f : 0.48f);
        drawVcRowStripe(r, rowIndex, w.hover, w.press);

        const Color tealText = mix(rgb(0x007A96), rgb(0x00586E), w.hover);
        label(kFontSemibold, px(14.5f), {r.x + px(22), r.y, r.w * 0.58f, r.h}, tealText, title, Align::Left);

        if (!rightText.empty())
        {
            const Color rc = rightColor ? rightColor : mix(rgb(0x007A96), rgb(0xE03B8B), w.hover);
            label(kFontSemibold, px(13.5f), {r.x + r.w * 0.52f, r.y, r.w * 0.48f - px(22), r.h}, rc, rightText, Align::Right);
        }
        render::popOpacity();
        focusRing(r.inset(px(2)), w.focus, px(4));

        if (in.clicked && enabled)
        {
            sound::play(sound::Sfx::Nav);
            return true;
        }
        return false;
    }

    void c64TerminalView(Id wid, Rect r, const std::deque<std::string> &lines, std::string_view statusRight,
                         float progress01, bool finished, bool *closeClicked)
    {
        // Classic GTA: Vice City Commodore 64 opening screen palette (sampled from Terminal.png):
        // Outer border: #8673E2 (Light Periwinkle Blue)
        // Inner screen: #311FB5 (Deep C64 Royal Blue)
        constexpr Color kC64Border = rgb(0x8673E2);
        constexpr Color kC64Screen = rgb(0x311FB5);
        constexpr Color kC64Text = rgb(0x9382ED);
        constexpr Color kC64Bright = rgb(0xC5BBFF);
        constexpr Color kC64Cyan = rgb(0x27FDF5);
        constexpr Color kC64Pink = rgb(0xFE7FB4);

        shadow(r.offset(0, px(6)), rgb(0x000000, 175), px(18), px(4));
        rect(r, kC64Border, px(3));

        const float borderW = px(11);
        const Rect screen = r.inset(borderW, borderW);
        rect(screen, kC64Screen);

        const float fontSize = px(12.0f);
        const float lineStep = std::ceil(px(16.0f));

        // Top C64 Header inside the blue screen: ****   WMKV / VCS.REC   ****
        label(kFontMono, fontSize, {screen.x + px(12), screen.y + px(4), screen.w - px(24), lineStep}, kC64Text,
              "****   WMKV / VCS.REC   ****", Align::Centre);

        // Status / Progress on top-right of the border or header
        if (!statusRight.empty())
        {
            label(kFontMono, px(11.0f), {screen.x + px(10), screen.y + px(4), screen.w - px(44), lineStep}, kC64Cyan,
                  statusRight, Align::Left);
        }

        // Optional close / cancel [X] button in top-right corner of the C64 screen
        if (closeClicked)
        {
            const Rect closeR{screen.right() - px(32), screen.y + px(2), px(28), px(20)};
            const Interaction closeIn = interact(wid ^ 0xC64C64ull, closeR, true);
            if (closeIn.w->hover > 0.01f)
                rect(closeR, withAlpha(kC64Border, 0.35f * closeIn.w->hover), px(2));
            label(kFontMono, px(11.5f), closeR, closeIn.w->hover > 0.1f ? rgb(0xFFFFFF) : kC64Bright, "[X]", Align::Centre);
            if (closeIn.clicked)
            {
                *closeClicked = true;
                sound::play(sound::Sfx::Back);
            }
        }

        // Classic C64 boot commands
        const float bootY = screen.y + px(22);
        drawText(kFontMono, fontSize, screen.x + px(12), bootY + px(11), kC64Text, "READY.");
        drawText(kFontMono, fontSize, screen.x + px(12), bootY + px(26), kC64Text, "LOAD \"VICE CITY STORIES\",8,1");
        drawText(kFontMono, fontSize, screen.x + px(12), bootY + px(41), kC64Bright,
                 finished ? "READY. RUN" : "PRESS PLAY ON TAPE  //  EXECUTING CMD PIPELINE...");

        // Progress bar in C64 block style along the separator
        const float sepY = bootY + px(48);
        rect({screen.x + px(12), sepY, screen.w - px(24), 1}, withAlpha(kC64Border, 0.45f));
        if (progress01 > 0.005f)
        {
            const float pw = (screen.w - px(24)) * core::clamp01(progress01);
            rect({screen.x + px(12), sepY - px(1), pw, px(3)}, kC64Cyan);
        }

        // Scrolling CMD output area (height snapped to an exact multiple of lineStep so rows never clip half-way)
        const float rawViewH = std::max(lineStep * 2.0f, screen.bottom() - sepY - px(10));
        const float snappedViewH = std::floor(rawViewH / lineStep) * lineStep;
        const Rect view{screen.x + px(12), sepY + px(6), screen.w - px(24), snappedViewH};
        const float pos = beginScroll(wid, view, true);
        const FontMetrics m = metrics(kFontMono, px(11.5f));
        const size_t totalRows = lines.size() + 1; // +1 for blinking cursor row
        const size_t first = size_t(std::max(0.0f, std::floor(pos / lineStep)));
        const size_t last = std::min(totalRows, first + size_t(view.h / lineStep) + 2);
        const size_t fits = size_t(view.w / std::max(1.0f, measure(kFontMono, px(11.5f), "M"))) + 2;

        for (size_t i = first; i < last; ++i)
        {
            const float y = view.y + float(i) * lineStep;
            const float base = y + std::floor((lineStep + m.ascent - m.descent) * 0.5f);
            if (i < lines.size())
            {
                const std::string &line = lines[i];
                const bool isErr = line.find("error") != std::string::npos || line.find("ERROR") != std::string::npos ||
                                   line.find("FAILED") != std::string::npos;
                const bool isTag = line.rfind("[", 0) == 0;
                const Color c = isErr ? kC64Pink : isTag ? kC64Bright : kC64Text;
                drawText(kFontMono, px(11.5f), view.x, base, c,
                         std::string_view(line).substr(0, std::min(line.size(), fits * 2)));
            }
            else
            {
                // Blinking C64 block cursor
                const bool blinkOn = std::fmod(state.time * 2.0, 1.0) < 0.55;
                if (!finished)
                    keepAnimating();
                if (blinkOn || finished)
                {
                    rect({view.x, y + px(2), px(8), lineStep - px(4)}, kC64Border);
                }
            }
        }
        endScroll(wid, view.y + float(totalRows) * lineStep);
    }

    // ---- surfaces and marks ---------------------------------------------------------------------------------------

    void card(Rect r, Color fill, Color edge)
    {
        const float radius = px(theme::kRadius);
        if (!fill)
            fill = theme::kSurface;
        shadow(r.offset(0, px(8)), theme::shade(0.55f), px(18), radius);
        rectEx(r, mix(fill, rgb(0xFFFFFF), 0.025f), fill, radius, theme::ink(0.06f), hairline());
        if (edge)
        {
            pushClip(r);
            rect({r.x, r.y, px(3), r.h}, edge, {radius, 0, 0, radius});
            popClip();
        }
    }

    float pillWidth(std::string_view text) { return std::ceil(measure(kFontSemibold, px(11.5f), text) + px(18)); }

    void pill(Rect r, std::string_view text, Color color)
    {
        rect(r, withAlpha(color, 0.16f), r.h * 0.5f);
        label(kFontSemibold, px(11.5f), r, color, text, Align::Centre);
    }

    void spinner(float cx, float cy, float radius, Color color, float thickness)
    {
        // a head that chases its tail: the arc grows and shrinks while it turns
        const float t = float(std::fmod(state.time, 1000.0));
        const float sweep = core::kPi * (0.35f + 0.55f * (0.5f + 0.5f * std::sin(t * 2.6f)));
        const float start = t * 5.2f + 0.6f * std::sin(t * 2.6f);
        arc(cx, cy, radius, thickness, start, sweep, color);
        keepAnimating();
    }

    void checkMark(float cx, float cy, float size, Color color, float thickness, float progress)
    {
        // two strokes drawn in order: the short one down, then the long one up
        const float ax = cx - size * 0.42f, ay = cy + size * 0.02f;
        const float bx = cx - size * 0.12f, by = cy + size * 0.30f;
        const float ex = cx + size * 0.44f, ey = cy - size * 0.30f;
        const float l1 = std::hypot(bx - ax, by - ay), l2 = std::hypot(ex - bx, ey - by);
        const float drawn = core::clamp01(progress) * (l1 + l2);
        if (drawn <= 0)
            return;
        const float f1 = std::min(drawn / l1, 1.0f);
        line(ax, ay, ax + (bx - ax) * f1, ay + (by - ay) * f1, thickness, color);
        if (drawn > l1)
        {
            const float f2 = std::min((drawn - l1) / l2, 1.0f);
            line(bx, by, bx + (ex - bx) * f2, by + (ey - by) * f2, thickness, color);
        }
    }

    void crossMark(float cx, float cy, float size, Color color, float thickness, float progress)
    {
        const float h = size * 0.32f;
        const float p1 = core::clamp01(progress * 2), p2 = core::clamp01(progress * 2 - 1);
        line(cx - h, cy - h, cx - h + 2 * h * p1, cy - h + 2 * h * p1, thickness, color);
        if (p2 > 0)
            line(cx + h, cy - h, cx + h - 2 * h * p2, cy - h + 2 * h * p2, thickness, color);
    }

    namespace
    {
        void drawStatus(Status status, float cx, float cy, float r, float appear, int number)
        {
            if (appear <= 0.01f)
                return;
            const float pop = core::easeOutBack(appear);
            const float rr = r * (0.6f + 0.4f * pop);
            render::pushOpacity(core::clamp01(appear * 1.6f));
            switch (status)
            {
            case Status::Checking:
                arc(cx, cy, rr - px(1), px(1.5f), 0, 2 * core::kPi, withAlpha(theme::kFaint, 0.35f));
                spinner(cx, cy, rr - px(1), theme::kDim, px(1.8f));
                break;
            case Status::Running:
                circle(cx, cy, rr, withAlpha(theme::kPink, 0.14f));
                spinner(cx, cy, rr - px(1), theme::kPink, px(2));
                break;
            case Status::Good:
                circle(cx, cy, rr, withAlpha(theme::kTeal, 0.16f));
                checkMark(cx, cy, rr * 1.15f, theme::kTeal, px(2), core::clamp01((appear - 0.15f) / 0.85f));
                break;
            case Status::Warning:
                circle(cx, cy, rr, withAlpha(theme::kAmber, 0.16f));
                line(cx, cy - rr * 0.42f, cx, cy + rr * 0.08f, px(2.2f), theme::kAmber);
                circle(cx, cy + rr * 0.40f, px(1.4f), theme::kAmber);
                break;
            case Status::Failed:
                circle(cx, cy, rr, withAlpha(theme::kRed, 0.16f));
                crossMark(cx, cy, rr * 1.2f, theme::kRed, px(2), core::clamp01((appear - 0.15f) / 0.85f));
                break;
            case Status::Missing:
            {
                arc(cx, cy, rr - px(1), px(1.5f), 0, 2 * core::kPi, theme::kFaint);
                if (number > 0)
                {
                    const std::string n = std::to_string(number);
                    const float s = r * 0.95f;
                    const float w = measure(kFontSemibold, s, n);
                    const FontMetrics m = metrics(kFontSemibold, s);
                    drawText(kFontSemibold, s, cx - w * 0.5f, std::floor(cy + (m.ascent - m.descent) * 0.5f), theme::kDim, n);
                }
                break;
            }
            }
            render::popOpacity();
        }
    }

    void statusBadge(Id wid, float cx, float cy, float radius, Status status, int number)
    {
        Widget &w = widget(wid);
        const int s = int(status);
        if (w.fresh || w.value < 0)
        {
            w.value = s;
            w.previous = -1;
            w.t = 1;
        }
        else if (w.value != s)
        {
            w.previous = w.value;
            w.value = s;
            w.t = 0;
        }
        // linear time for the morph, eased inside drawStatus
        if (w.t < 1)
        {
            w.t = state.motion > 0 ? std::min(1.0f, w.t + state.dt / 0.45f) : 1.0f;
            keepAnimating();
        }
        if (w.previous >= 0 && w.t < 1)
            drawStatus(Status(w.previous), cx, cy, radius, 1.0f - core::clamp01(w.t * 2.2f), number);
        drawStatus(status, cx, cy, radius, w.previous >= 0 ? w.t : 1.0f, number);
    }

    void progressBar(Id wid, Rect r, float value, Color from, Color to)
    {
        Widget &w = widget(wid);
        const float radius = r.h * 0.5f;
        rect(r, theme::ink(0.06f), radius);
        const float t = float(state.time);
        if (value < 0)
        {
            // indeterminate: a band slides across and back
            const float phase = float(std::fmod(t / 1.5, 1.0));
            const float e = core::easeInOutCubic(phase);
            const float bw = r.w * 0.32f;
            const float x = r.x - bw + e * (r.w + bw);
            pushClip(r);
            gradient({x, r.y, bw, r.h}, withAlpha(from, 0.0f), from, radius, Gradient::Horizontal);
            gradient({x + bw * 0.5f, r.y, bw * 0.5f, r.h}, from, to, radius, Gradient::Horizontal);
            popClip();
            keepAnimating();
            w.t = 0;
            return;
        }
        if (w.fresh)
            w.t = value;
        animate(w.t, core::clamp01(value), 6);
        const float fw = std::max(r.h, r.w * w.t);
        if (w.t <= 0.0005f)
            return;
        const Rect fill{r.x, r.y, fw, r.h};
        shadow(fill.offset(0, px(1)), withAlpha(from, 0.35f), px(6), radius);
        gradient(fill, from, to, radius, Gradient::Horizontal);
        // a sheen travels along the filled part every couple of seconds
        const float period = 2.4f;
        const float phase = float(std::fmod(t, double(period))) / period;
        const float sw = std::max(px(60), fw * 0.25f);
        const float sx = fill.x - sw + phase * 1.6f * (fw + sw);
        pushClip(fill);
        gradient({sx, r.y, sw * 0.5f, r.h}, rgb(0xFFFFFF, 0), rgb(0xFFFFFF, 70), 0, Gradient::Horizontal);
        gradient({sx + sw * 0.5f, r.y, sw * 0.5f, r.h}, rgb(0xFFFFFF, 70), rgb(0xFFFFFF, 0), 0, Gradient::Horizontal);
        popClip();
        keepAnimating();
    }

    float rollingNumber(Id wid, Font font, float size, float x, float baseline, Color color, int value)
    {
        Widget &w = widget(wid);
        if (w.fresh)
            w.t = float(value);
        animate(w.t, float(value), 4.5f); // unhurried: a change of one takes about half a second to settle
        const float v = std::max(0.0f, w.t);
        const int shownMax = std::max(value, int(std::ceil(v)));
        int digits = 1;
        for (int n = shownMax; n >= 10; n /= 10)
            ++digits;
        const FontMetrics m = metrics(font, size);
        // digits are cap height tall: the strip steps a little more than that, and the clip hides the rest
        const float cap = m.capHeight > 0 ? m.capHeight : m.ascent * 0.7f;
        const float step = std::ceil(cap * 1.35f);
        float digitW = 0;
        for (char c = '0'; c <= '9'; ++c)
            digitW = std::max(digitW, measure(font, size, std::string(1, c)));
        // A digit leaving rises, blurs and fades; the one arriving comes up from below out of a blur into focus. The
        // fades do the edges; the clip (with room for the blur) only keeps the strip's far digits out.
        const float maxBlur = cap * 0.16f;
        pushClip({x - maxBlur - px(2), baseline - cap - cap * 0.5f, digitW * float(digits) + 2 * maxBlur + px(4), cap * 2.4f});
        for (int i = 0; i < digits; ++i)
        {
            const float place = std::pow(10.0f, float(digits - 1 - i));
            const float base = std::floor(v / place);
            const float rem = v - base * place;
            // an odometer: a digit turns only while the ones below it roll over from 9
            const float carry = rem > place - 1 ? rem - (place - 1) : 0;
            const float pos = std::fmod(base, 10.0f) + carry;
            const int d0 = int(std::floor(pos)) % 10;
            const float frac = pos - std::floor(pos);
            const float dx = x + float(i) * digitW;
            const bool leading = i < digits - 1 && v < place; // a digit the number has not reached yet
            const float alpha = leading ? core::clamp01(v / place) : 1.0f;
            const std::string a(1, char('0' + d0)), b(1, char('0' + (d0 + 1) % 10));
            const float ca = (digitW - measure(font, size, a)) * 0.5f, cb = (digitW - measure(font, size, b)) * 0.5f;
            // smooth in, smooth out: the blur and the fade lead a little at both ends of the move
            const float s = frac * frac * (3 - 2 * frac);
            // the one leaving is gone before the motion's slow tail, so no ghost lingers above
            drawText(font, size, dx + ca, baseline - frac * step, fade(color, alpha * (1 - s) * (1 - s)), a, maxBlur * std::sqrt(frac));
            if (frac > 0.001f)
                drawText(font, size, dx + cb, baseline + (1 - frac) * step, fade(color, alpha * s), b, maxBlur * std::sqrt(1 - frac));
        }
        popClip();
        return digitW * float(digits);
    }

    // ---- scrolling ------------------------------------------------------------------------------------------------

    float beginScroll(Id wid, Rect view, bool followEnd)
    {
        ScrollState &s = g_scrolls[wid];
        if (s.frame + 1 < state.frame)
        {
            s = ScrollState();
        }
        s.frame = state.frame;
        s.view = view;
        const float maxScroll = std::max(0.0f, s.content - view.h);
        const platform::Input &in = platform::input;
        if (in.wheel != 0 && mouseOver(view) && maxScroll > 0)
        {
            s.target -= in.wheel * px(72);
            s.lastScroll = state.wall;
        }
        if (s.toEnd)
        {
            s.target = maxScroll;
            s.toEnd = false;
        }
        if (followEnd && s.stick && !s.dragging && in.wheel == 0)
            s.target = maxScroll; // following the end while it is there
        s.target = std::clamp(s.target, 0.0f, maxScroll);
        if (state.motion <= 0)
            s.pos = s.target;
        else
            animate(s.pos, s.target, 18);
        s.stick = followEnd && s.target >= maxScroll - px(2);
        pushClip(view);
        pushOffset(0, -std::round(s.pos));
        return s.pos;
    }

    void endScroll(Id wid, float contentBottom)
    {
        ScrollState &s = g_scrolls[wid];
        popOffset();
        popClip();
        s.content = contentBottom - s.view.y;
        const float maxScroll = std::max(0.0f, s.content - s.view.h);
        if (maxScroll <= 0)
            return;
        // the scrollbar: shown while hovering the area or just after scrolling
        const float thumbH = std::max(px(28), s.view.h * s.view.h / s.content);
        const float y = s.view.y + (s.pos / maxScroll) * (s.view.h - thumbH);
        const Rect thumb{s.view.right() - px(7), y, px(4), thumbH};
        const Interaction in = interact(wid ^ 0x5C5C5C5Cull, thumb.inset(-px(4), 0), false);
        if (in.pressed)
        {
            s.dragging = true;
            s.dragMouse = platform::input.mouseY;
            s.dragPos = s.target;
        }
        if (!platform::input.down[platform::kMouseLeft])
            s.dragging = false;
        if (s.dragging)
        {
            s.target = std::clamp(s.dragPos + (platform::input.mouseY - s.dragMouse) * maxScroll / (s.view.h - thumbH), 0.0f, maxScroll);
            s.pos = s.target;
            s.lastScroll = state.wall;
        }
        const bool show = mouseOver(s.view) || s.dragging || state.wall - s.lastScroll < 1.0;
        if (state.wall - s.lastScroll < 1.0)
            wakeAt(s.lastScroll + 1.0);
        animate(s.bar, show ? 1.0f : 0.0f, show ? 16.0f : 5.0f);
        if (s.bar > 0.01f)
            rect(thumb.inset(-in.w->hover * px(1), 0), theme::ink((0.16f + 0.14f * in.w->hover) * s.bar), px(2) + in.w->hover * px(1));
    }

    void scrollToEnd(Id wid) { g_scrolls[wid].toEnd = true; }

    bool scrolledToEnd(Id wid)
    {
        auto it = g_scrolls.find(wid);
        return it == g_scrolls.end() || it->second.stick;
    }

    void logView(Id wid, Rect r, const std::deque<std::string> &lines)
    {
        rectEx(r, theme::kLogFill, theme::kLogFill, px(10), theme::ink(0.05f), hairline());
        const Rect view = r.inset(px(14), px(10));
        const float size = px(12.5f);
        const float step = std::ceil(px(19));
        const float pos = beginScroll(wid, view, true);
        const FontMetrics m = metrics(kFontMono, size);
        const size_t first = size_t(std::max(0.0f, std::floor(pos / step)));
        const size_t last = std::min(lines.size(), first + size_t(view.h / step) + 2);
        for (size_t i = first; i < last; ++i)
        {
            const float y = view.y + float(i) * step;
            const std::string &line = lines[i];
            // the tools' own tags a little brighter, errors red
            const bool error = line.find("error") != std::string::npos || line.find("ERROR") != std::string::npos ||
                               line.find("FAILED") != std::string::npos;
            const Color c = error ? theme::kRed : line.rfind("[", 0) == 0 ? theme::kDim : theme::kFaint;
            // monospace: only the characters that fit are drawn (the clip takes the last one's overhang)
            const size_t fits = size_t(view.w / std::max(1.0f, measure(kFontMono, size, "M"))) + 2;
            drawText(kFontMono, size, view.x, y + std::floor((step + m.ascent - m.descent) * 0.5f), c,
                     std::string_view(line).substr(0, std::min(line.size(), fits * 2)));
        }
        endScroll(wid, view.y + float(lines.size()) * step);
    }
}
