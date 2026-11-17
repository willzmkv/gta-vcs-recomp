// The widgets the screens are made of. Each draws itself in its rectangle and animates by itself (hover, press, focus,
// springs); they take logical sizes through ui::px and colours from theme.h.
#pragma once

#include "render/font.h"
#include "ui/ui.h"

#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace ui
{
    enum class ButtonStyle
    {
        Primary,   // the hot pink gradient: the one thing to do next
        Secondary, // a raised control
        Ghost,     // text until hovered
        Danger,
    };

    // ---- text -----------------------------------------------------------------------------------------------------
    enum class Align
    {
        Left,
        Centre,
        Right
    };
    // one line, vertically centred in r, cut with an ellipsis when too long
    void label(render::Font font, float size, Rect r, Color color, std::string_view text, Align align = Align::Left);
    // wrapped at r.w from r.y down; returns the height used
    float paragraph(render::Font font, float size, Rect r, Color color, std::string_view text, float lineHeight = 1.45f);
    float paragraphHeight(render::Font font, float size, float width, std::string_view text, float lineHeight = 1.45f);
    // a small capitals label above a title
    void eyebrow(float x, float y, Color color, std::string_view text);

    // ---- controls -------------------------------------------------------------------------------------------------
    // badge: a small solid pink tag after the label ("NEW"), counted in the width
    float buttonWidth(std::string_view label, char32_t icon = 0, float size = 0, render::Font font = render::kFontSemibold,
                      std::string_view badge = {});
    bool button(std::string_view label, Rect r, ButtonStyle style = ButtonStyle::Secondary, char32_t icon = 0,
                bool enabled = true, float size = 0, render::Font font = render::kFontSemibold, std::string_view badge = {});
    bool iconButton(std::string_view idLabel, char32_t icon, Rect r, std::string_view tooltip, bool enabled = true,
                    float iconSize = 0);
    bool segmented(std::string_view idLabel, Rect r, const std::vector<std::string> &options, int &selected,
                   bool enabled = true);
    bool toggle(std::string_view idLabel, Rect r, bool &value, bool enabled = true);
    // a selectable row with a radio dot, a title and a line under it
    bool choiceRow(std::string_view idLabel, Rect r, std::string_view title, std::string_view detail, bool selected,
                   bool enabled = true);

    // ---- Rockstar Launcher controls & Vice City 1980s Menu rows ---------------------------------------------------
    bool playGameButton(std::string_view label, Rect r, char32_t icon, bool enabled = true);
    bool rockstarButton(std::string_view idLabel, Rect r, char32_t icon, std::string_view line1,
                        std::string_view line2 = {}, bool enabled = true);
    bool sliderInt(std::string_view idLabel, Rect r, int &value, int minVal, int maxVal, int step = 1,
                   std::string_view suffix = {}, bool enabled = true);

    // Classic GTA: Vice City menu components (matching the original Vice City Options table)
    bool vcSubTab(std::string_view idLabel, Rect r, std::string_view text, bool active);
    bool vcCycleRow(std::string_view idLabel, Rect r, int rowIndex, std::string_view title,
                    const std::vector<std::string> &options, int &selected, bool *hoveredOut = nullptr,
                    bool enabled = true);
    bool vcSliderRow(std::string_view idLabel, Rect r, int rowIndex, std::string_view title, int &value, int minVal,
                     int maxVal, int step = 1, std::string_view suffix = {}, bool *hoveredOut = nullptr,
                     bool enabled = true);
    bool vcModRow(std::string_view idLabel, Rect r, int rowIndex, bool &modEnabled, std::string_view title,
                  int &sliderValue, int minVal, int maxVal, int step = 1, std::string_view suffix = {},
                  bool *hoveredOut = nullptr, bool enabled = true);
    bool vcActionRow(std::string_view idLabel, Rect r, int rowIndex, std::string_view title,
                     std::string_view rightText = {}, Color rightColor = 0, bool *hoveredOut = nullptr,
                     bool enabled = true);

    // the Vice City rows' controls as drawings, shared with the game menu (screens/gamemenu.cpp): the pink track and its
    // double-ring knob (t 0..1, pressed 0..1), the pill switch with its square knob (t 0 off .. 1 on); s scales them
    void vcSliderTrack(Rect track, float t, float pressed, float s = 1.0f);
    void vcToggleSwitch(Rect togR, float t, float hover, float s = 1.0f);

    // Classic GTA: Vice City Commodore 64 Terminal (Terminal.png style) for live CMD build output
    void c64TerminalView(Id wid, Rect r, const std::deque<std::string> &lines, std::string_view statusRight,
                         float progress01, bool finished, bool *closeClicked = nullptr);

    enum class PerfImpact
    {
        Zero = 0,
        Low = 1,
        Medium = 2,
        High = 3,
        Extreme = 4,
    };
    void perfBadge(Rect r, PerfImpact impact, std::string_view text);

    // ---- surfaces and marks ---------------------------------------------------------------------------------------
    void card(Rect r, Color fill = 0, Color edge = 0); // 0: the theme's surface; edge: a coloured left edge
    float pillWidth(std::string_view text);
    void pill(Rect r, std::string_view text, Color color);
    void spinner(float cx, float cy, float radius, Color color, float thickness);
    void checkMark(float cx, float cy, float size, Color color, float thickness, float progress = 1);
    void crossMark(float cx, float cy, float size, Color color, float thickness, float progress = 1);

    enum class Status
    {
        Checking,
        Good,
        Warning,
        Missing,
        Failed,
        Running,
    };
    // a round badge that morphs between states: a spinner becomes a check that draws itself, or a cross
    void statusBadge(Id id, float cx, float cy, float radius, Status status, int number = 0);

    // value 0..1 eased toward; negative: indeterminate (a sliding band)
    void progressBar(Id id, Rect r, float value, Color from, Color to);
    // a number whose digits roll like an odometer when it changes; returns its width
    float rollingNumber(Id id, render::Font font, float size, float x, float baseline, Color color, int value);

    // ---- scrolling ------------------------------------------------------------------------------------------------
    // content between begin and end is drawn at view.y - offset; end() takes the content's bottom (in content
    // coordinates, as drawn) to know how far it can scroll, and draws a scrollbar that shows while it matters
    float beginScroll(Id id, Rect view, bool followEnd = false); // followEnd: stays at the end while it is there (logs)
    void endScroll(Id id, float contentBottom);
    void scrollToEnd(Id id);
    bool scrolledToEnd(Id id);
    std::string_view shownText(std::string_view label); // the part of a label before "##"

    // a monospace log, only its visible lines drawn; follows the end while it is there
    void logView(Id id, Rect r, const std::deque<std::string> &lines);
}
