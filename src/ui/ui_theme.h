// Shared visual theme for the in-game menus (F2 settings, F5 trainer): the launcher's Vice City / Miami Vice look
// (launcher/ui/widgets.cpp vc* components), Inter / system UI typography in Regular + SemiBold weights, and vector icons.
#pragma once

#include "imgui.h"

#include <cstdint>
#include <string>

namespace ui
{
    namespace palette
    {
        // Launcher Vice City / Miami Vice palette (launcher/ui/widgets.cpp vcToggleSwitch / vcSliderTrack / vcCycleRow)
        constexpr uint32_t kSkyTop = 0xC6EBF4u;     // backdrop gradient top (icy Miami sky)
        constexpr uint32_t kSkyMid = 0x9BE0ECu;     // backdrop mid
        constexpr uint32_t kSkyBot = 0x3AC2D0u;     // backdrop gradient bottom (Miami turquoise)
        constexpr uint32_t kHeaderBg = 0xDAF3F9u;   // frosted header / rail background
        constexpr uint32_t kRailBg = 0xCBEBF3u;     // side page rail

        constexpr uint32_t kRowEven = 0xFBFDFEu;    // zebra stripe even (crisp white)
        constexpr uint32_t kRowOdd = 0xE8F7FAu;     // zebra stripe odd (soft ice-cyan)
        constexpr uint32_t kRowHover = 0xFFE3F1u;   // hovered / focused row blush pink
        constexpr uint32_t kSectionBg = 0xD2EEF5u;  // section header band
        constexpr uint32_t kTabIdle = 0xD4F0F6u;    // inactive side item
        constexpr uint32_t kTabHover = 0xE6F8FCu;   // hovered side item

        constexpr uint32_t kPink = 0xFF2A85u;       // hot Miami pink (slider ring, accents, active fill)
        constexpr uint32_t kPinkSoft = 0xFE7FB4u;   // flamingo pink (active tab, toggle ON, row accent bar)
        constexpr uint32_t kPinkHover = 0xE03B8Bu;  // chevron / hover pink
        constexpr uint32_t kPinkBorder = 0xC8145Eu; // toggle ON border
        constexpr uint32_t kOnPink = 0xFFFFFFu;     // crisp white text/marks on pink

        constexpr uint32_t kTealDark = 0x00586Eu;   // headings, hovered labels
        constexpr uint32_t kTeal = 0x007A96u;       // primary labels, values, chevrons
        constexpr uint32_t kTealMuted = 0x4E8999u;  // secondary / hint text
        constexpr uint32_t kTealFaint = 0x7BA9B5u;  // disabled text
        constexpr uint32_t kTealLine = 0x0084A1u;   // hairlines / dividers

        constexpr uint32_t kTrackOff = 0xBCE3EDu;   // slider empty track
        constexpr uint32_t kToggleOff = 0x8EC1CEu;  // toggle OFF track
        constexpr uint32_t kToggleOffHi = 0xA2D0DCu;// toggle OFF hover
        constexpr uint32_t kToggleOffBd = 0x005E72u;// toggle OFF border

        constexpr uint32_t kStateOn = 0x0B9C84u;    // trainer ON (deep Miami teal)
        constexpr uint32_t kAmber = 0xB57700u;      // restart pending / warning
        constexpr uint32_t kRed = 0xD6334Au;        // trainer OFF / danger border
        constexpr uint32_t kDangerFill = 0xFFE4E8u;
        constexpr uint32_t kDangerFillHi = 0xFFD4DBu;
        constexpr uint32_t kDangerText = 0xB0243Au;

        inline ImVec4 rgba(uint32_t hex, float a = 1.0f)
        {
            return ImVec4(((hex >> 16) & 0xFFu) / 255.0f, ((hex >> 8) & 0xFFu) / 255.0f, (hex & 0xFFu) / 255.0f, a);
        }

        inline ImU32 u32(uint32_t hex, float a = 1.0f)
        {
            const int alpha = a <= 0.0f ? 0 : (a >= 1.0f ? 255 : static_cast<int>(a * 255.0f + 0.5f));
            return IM_COL32((hex >> 16) & 0xFFu, (hex >> 8) & 0xFFu, hex & 0xFFu, alpha);
        }
    }

    // Loads Inter (or the system UI font fallback) in Regular and SemiBold weights and applies the launcher's Miami
    // Vice palette to ImGui's base style. Call once after ImGui::CreateContext() and before the first ImGui::NewFrame().
    void initTheme();

    // Regular (400) and SemiBold (600) fonts; fall back to ImGui's default font if no TTF was found.
    ImFont *fontRegular();
    ImFont *fontSemibold();

    // Crisp Vice City chevron (< or >), matching launcher/ui/widgets.cpp drawVcChevron.
    void drawChevron(ImDrawList *dl, ImVec2 center, bool rightDir, float scale, ImU32 col);

    // Vector icon for a settings page (Display, Graphics, Audio, Game, Keyboard, Gamepad, Enhancements, Mods, Debug, ...).
    void drawPageIcon(ImDrawList *dl, const std::string &page, ImVec2 center, float size, ImU32 col);
}
