// See settings_menu.h.
//
// Layout: a fixed-size window (from the viewport alone), the page tabs, the page's sections in a scrolling child, and a
// footer of fixed height (help text sized for the longest help of any setting, the restart note, the buttons). Nothing
// in the window changes size with what the mouse is over: an auto-sized, centred window whose help line grew and
// shrank with the hovered row moved the rows under a still mouse every other frame, and a click whose press and release
// landed in different layouts was lost (2026-10-24).
#include "ui/settings_menu.h"

#include "platform/platform.h"
#include "runtime/ps2_control.h"
#include "settings/settings.h"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <string>
#include <vector>

namespace ui
{
    namespace
    {
        bool g_open = false;
        bool g_releaseWait = false;  // closed, but the keys / buttons that closed it are still down
        bool g_chordWasDown = false; // Back + Start
        bool g_popupWasOpen = false; // a combo's list: Escape / B close it, not the menu
        bool g_wasFocused = false;   // the menu window had focus last frame
        bool g_focusWindow = false;
        int g_page = 0, g_selectPage = -1;
        std::string g_capturing;  // the Key setting waiting for a key press
        bool g_captureArmed = false; // every key / button was let go since the capture started (the click that began it)
        bool g_mouseHold = false;    // a binding was just made: the menu ignores the mouse until everything is let go
        std::string g_confirming;    // the Action setting waiting for its second click
        double g_confirmUntil = 0.0;
        float g_scrollTo = -1.0f;    // scrollSettingsMenu, at the next frame
        std::string g_helpKey;    // the row whose help shows: the last one hovered or navigated to (kept in between)
        std::string g_navFocused; // the row whose control had keyboard / gamepad focus last frame

        using platform::keyName;

        // Keys the menu keys may not take: the menu's own (Escape, Enter, arrows, Q / E, Backspace, Space), the
        // modifiers and the mouse. Clashes with control bindings are the group's business (settings::Spec::group).
        bool bindable(int key)
        {
            using namespace platform;
            static constexpr int kTaken[] = {KEY_ESCAPE, KEY_ENTER, KEY_KP_ENTER, KEY_BACKSPACE, KEY_SPACE, KEY_Q, KEY_E};
            if (std::find(std::begin(kTaken), std::end(kTaken), key) != std::end(kTaken) || (key >= KEY_RIGHT && key <= KEY_UP) ||
                key >= KEY_LEFT_SHIFT)
                return false;
            return std::string(keyName(key)) != "?";
        }

        // A control binding takes any key, mouse button or wheel notch with a name but Escape (which cancels). Either
        // Shift / Ctrl / Alt is stored as the left one and Keypad Enter as Enter: the bindings take both sides
        // (game/vcs_input.cpp).
        int bindingKey(int key)
        {
            using namespace platform;
            switch (key)
            {
            case KEY_RIGHT_SHIFT: return KEY_LEFT_SHIFT;
            case KEY_RIGHT_CONTROL: return KEY_LEFT_CONTROL;
            case KEY_RIGHT_ALT: return KEY_LEFT_ALT;
            case KEY_KP_ENTER: return KEY_ENTER;
            default: return key == KEY_ESCAPE || std::string(keyName(key)) == "?" ? 0 : key;
            }
        }

        const settings::Spec *findSpec(const std::string &key)
        {
            for (const settings::Spec &s : settings::all())
                if (s.key == key)
                    return &s;
            return nullptr;
        }

        // Binds `key` to `s`, keeping the keys of its group exclusive: an optional setting that had it loses it, a
        // required one (the menu keys) refuses the change. A required key is exclusive across groups too (the driving
        // keys, a group of their own, may share the on-foot keys but not the menu keys). False (with the reason in
        // g_refusal) when refused.
        std::string g_refusal;
        bool bindKey(const settings::Spec &s, int key)
        {
            if (key != 0 && !s.group.empty())
                for (const settings::Spec &o : settings::all())
                    if (&o != &s && o.type == settings::Type::Key && !o.group.empty() &&
                        (o.group == s.group || !o.optional || !s.optional) && settings::value(o.key) == key)
                    {
                        if (!o.optional || !s.optional) // a menu key never takes a control's key, nor gives its own
                        {
                            g_refusal = std::string(keyName(key)) + " is already \"" + o.label + "\" (" + o.page + ").";
                            return false;
                        }
                        settings::set(o.key, 0);
                    }
            g_refusal.clear();
            return settings::set(s.key, key);
        }

        bool gamepadDown(platform::GamepadButton b) { return platform::gamepadConnected() && platform::gamepadButton(b); }

        bool anyInputHeld()
        {
            for (int k = 1; k < platform::KEY_COUNT; ++k)
                if (platform::keyDown(k))
                    return true;
            for (int b = 0; b < platform::GAMEPAD_BUTTON_COUNT; ++b)
                if (gamepadDown(static_cast<platform::GamepadButton>(b)))
                    return true;
            return platform::mouseDown(platform::MOUSE_LEFT) || platform::mouseDown(platform::MOUSE_RIGHT);
        }

        void open()
        {
            settings::refresh();
            g_open = true;
            g_focusWindow = true;
            g_capturing.clear();
            g_confirming.clear();
            g_refusal.clear();
            g_helpKey.clear();
            g_navFocused.clear();
            ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        }

        void close()
        {
            g_open = false;
            g_releaseWait = true; // the game would otherwise see Escape / B / Start held
            g_capturing.clear();
            ImGui::GetIO().ConfigFlags &= ~(ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad);
        }

        bool restartPending(const settings::Spec &s) { return s.apply == settings::Apply::Restart && settings::changedSinceStart(s.key); }

        // What the help area shows for a setting.
        std::string helpText(const settings::Spec &s)
        {
            std::string text = s.help;
            if (s.apply == settings::Apply::Restart)
                text += " Applies the next time the game starts.";
            if (settings::overridden(s.key))
                text += " Locked: the environment variable " + s.env + " sets it for this run (tools/run.py sets some for "
                        "test runs); start the game without it to change it here.";
            else if (s.locked && s.locked())
                text += " " + s.lockedNote;
            if (s.type == settings::Type::Key && s.optional)
                text += " Click, then press a key or mouse button or turn the wheel (Esc: cancel). Right-click: clear.";
            return text;
        }

        // A page's sections in the order their first setting was registered.
        std::vector<std::string> sections(const std::string &page)
        {
            std::vector<std::string> out;
            for (const settings::Spec &s : settings::all())
                if (s.page == page && !s.hidden && std::find(out.begin(), out.end(), s.section) == out.end())
                    out.push_back(s.section);
            return out;
        }

        // One key button: its key's name, or "Press a key..." while it waits for one. `note` runs right after the button.
        template <typename Note> void keySlot(const settings::Spec &s, float width, Note note)
        {
            const int v = settings::value(s.key);
            if (g_capturing == s.key)
            {
                ImGui::Button(s.optional ? "Press a key or button... (Esc)###k" : "Press a key... (Esc: cancel)###k", ImVec2(width, 0));
                note();
                if (!g_captureArmed)
                {
                    g_captureArmed = !anyInputHeld(); // the click / Enter that started it is let go first
                    return;
                }
                const bool clickedAway = !s.optional && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsItemHovered();
                if (platform::keyPressed(platform::KEY_ESCAPE) || clickedAway || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
                {
                    g_capturing.clear();
                    return;
                }
                for (int k = 1; k < platform::KEY_COUNT; ++k)
                {
                    if (!platform::keyPressed(k))
                        continue;
                    const int key = s.optional ? bindingKey(k) : (bindable(k) ? k : 0);
                    if (key == 0)
                        continue;
                    bindKey(s, key);
                    g_capturing.clear();
                    g_mouseHold = s.optional;
                    break;
                }
                return;
            }
            const char *name = v == 0 ? "-" : keyName(v);
            const bool pressed = ImGui::Button((std::string(name) + "###k").c_str(), ImVec2(width, 0));
            note();
            if (pressed)
            {
                g_capturing = s.key;
                g_captureArmed = false;
                g_refusal.clear();
            }
            if (s.optional && ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                settings::set(s.key, 0);
        }

        // The label, then the setting's control. A row is hovered when the mouse is anywhere across it (label, control or
        // the space between); clicking anywhere on a toggle's row flips it.
        void row(const settings::Spec &s, std::string &focusedNow)
        {
            const ImGuiStyle &style = ImGui::GetStyle();
            const bool byEnv = settings::overridden(s.key);
            const bool bySetting = !byEnv && s.locked && s.locked(); // another setting decides it for now
            const bool locked = byEnv || bySetting;
            int v = bySetting ? s.lockedValue : settings::value(s.key);
            ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetFrameHeight() + style.CellPadding.y * 2.0f);
            ImGui::TableNextColumn();
            const float top = ImGui::GetCursorScreenPos().y - style.CellPadding.y;
            ImGui::AlignTextToFramePadding();
            if (byEnv) // greyed out with the reason beside it, so a click that does nothing doesn't look like a bug
                ImGui::TextDisabled("%s (%s)", s.label.c_str(), s.env.c_str());
            else if (bySetting) // the reason is in the help line (lockedNote)
                ImGui::TextDisabled("%s", s.label.c_str());
            else
                ImGui::TextUnformatted(s.label.c_str());
            if (restartPending(s))
            {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "*");
            }
            ImGui::TableNextColumn();
            ImGui::PushID(s.key.c_str());
            ImGui::BeginDisabled(locked);
            ImGui::SetNextItemWidth(-FLT_MIN);
            bool overItem = false;
            auto noteItem = [&] { // right after the control itself (a combo's list is another window)
                overItem = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
                if (ImGui::IsItemFocused())
                    focusedNow = s.key;
            };
            switch (s.type)
            {
            case settings::Type::Toggle:
            {
                bool on = v != 0;
                const bool changed = ImGui::Checkbox(on ? "On###v" : "Off###v", &on);
                noteItem();
                if (changed)
                    settings::set(s.key, on ? 1 : 0);
                // a partner Integer: its slider in the rest of the row (a mod's priority), usable while the switch is on
                if (const settings::Spec *second = s.partner.empty() ? nullptr : findSpec(s.partner);
                    second && second->type == settings::Type::Integer)
                {
                    ImGui::SameLine(0.0f, style.ItemSpacing.x * 2.0f);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::BeginDisabled(v == 0 || settings::overridden(second->key));
                    int edited = settings::value(second->key);
                    const bool moved = ImGui::SliderInt("##partner", &edited, second->min, second->max, second->format.c_str(),
                                                        ImGuiSliderFlags_AlwaysClamp);
                    overItem = overItem || ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
                    if (ImGui::IsItemFocused())
                        focusedNow = s.key;
                    if (moved)
                        settings::set(second->key, edited);
                    ImGui::EndDisabled();
                }
                break;
            }
            case settings::Type::Choice:
            {
                auto current = std::find_if(s.choices.begin(), s.choices.end(), [&](const settings::Choice &c) { return c.value == v; });
                const bool listOpen = ImGui::BeginCombo("##v", current != s.choices.end() ? current->label.c_str() : "?");
                noteItem();
                if (listOpen)
                {
                    for (const settings::Choice &c : s.choices)
                    {
                        if (ImGui::Selectable(c.label.c_str(), c.value == v))
                            settings::set(s.key, c.value);
                        if (c.value == v)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                break;
            }
            case settings::Type::Integer:
            {
                int edited = v;
                const bool changed = ImGui::SliderInt("##v", &edited, s.min, s.max, s.format.c_str(), ImGuiSliderFlags_AlwaysClamp);
                noteItem();
                if (changed)
                    settings::set(s.key, edited);
                break;
            }
            case settings::Type::Key:
            {
                const settings::Spec *second = s.partner.empty() ? nullptr : findSpec(s.partner);
                const float gap = style.ItemSpacing.x;
                const float width = second ? (ImGui::GetContentRegionAvail().x - gap) * 0.5f : -FLT_MIN;
                keySlot(s, width, noteItem);
                if (second)
                {
                    ImGui::SameLine(0.0f, gap);
                    ImGui::PushID(1);
                    keySlot(*second, -FLT_MIN, [&] {
                        overItem = overItem || ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
                        if (ImGui::IsItemFocused())
                            focusedNow = s.key;
                    });
                    ImGui::PopID();
                }
                break;
            }
            case settings::Type::Action:
            {
                const bool confirming = g_confirming == s.key && ImGui::GetTime() < g_confirmUntil;
                const std::string text = confirming ? "Click again to confirm" : s.button.empty() ? s.label : s.button;
                const bool pressed = ImGui::Button((text + "###v").c_str(), ImVec2(-FLT_MIN, 0));
                noteItem();
                if (pressed)
                {
                    if (s.confirm && !confirming)
                    {
                        g_confirming = s.key;
                        g_confirmUntil = ImGui::GetTime() + 3.0;
                    }
                    else
                    {
                        g_confirming.clear();
                        if (s.onChange)
                            s.onChange(0);
                    }
                }
                break;
            }
            }
            ImGui::EndDisabled();
            ImGui::PopID();

            const float bottom = top + ImGui::GetFrameHeight() + style.CellPadding.y * 2.0f; // every control is one frame high
            const ImVec2 mouse = ImGui::GetMousePos();
            const bool hovered = ImGui::IsWindowHovered() && mouse.y >= top && mouse.y < bottom;
            const ImGuiIO &io = ImGui::GetIO();
            // the mouse takes the help over only when it moves or clicks, so a resting cursor doesn't fight the keyboard
            if (hovered && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f || ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                            g_helpKey.empty()))
                g_helpKey = s.key;
            if (hovered && !overItem && !locked && s.type == settings::Type::Toggle && !g_popupWasOpen &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                settings::set(s.key, v != 0 ? 0 : 1);
            if (g_helpKey == s.key)
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_Header, hovered ? 0.6f : 0.35f));
        }
    }

    bool settingsMenuOpen() { return g_open; }

    void openSettingsMenu(const std::string &page)
    {
        const std::vector<std::string> pages = settings::pages();
        for (size_t i = 0; i < pages.size(); ++i)
            if (pages[i] == page)
                g_selectPage = static_cast<int>(i);
        if (!g_open)
            open();
    }

    void scrollSettingsMenu(float fraction) { g_scrollTo = std::clamp(fraction, 0.0f, 1.0f); }

    void closeSettingsMenu()
    {
        if (g_open)
            close();
    }
    bool settingsMenuBlocksInput() { return g_open || g_releaseWait; }
    bool settingsMenuWaitingForKey() { return g_open && !g_capturing.empty(); }

    void drawSettingsMenu()
    {
        if (ps2control::hiddenMode() && !g_open) // a hidden run opens it only through openSettingsMenu
        {
            g_releaseWait = false; // no physical input to wait for
            return;
        }

        const bool chord = gamepadDown(platform::GAMEPAD_BACK) && gamepadDown(platform::GAMEPAD_START);
        const bool chordPressed = chord && !g_chordWasDown;
        g_chordWasDown = chord;
        // the physical key, or one injected over the control channel (scripted checks; physical input to the game is
        // blocked while the menu is open, so ps2control::keyPressed alone would not close it)
        const int key = settings::value("controls.menu_key");
        const bool menuKey = g_capturing.empty() && (platform::keyPressed(key) || ps2control::keyPressed(key));
        if (menuKey || chordPressed)
            g_open ? close() : open();
        else if (g_open && g_capturing.empty() && !g_popupWasOpen &&
                 (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)))
            close();

        if (g_releaseWait && !anyInputHeld())
            g_releaseWait = false;
        // While a control binding waits for its key, and until the button that was bound is let go, the mouse is the
        // binding's, not the menu's: a mouse button bound on its press would otherwise click what is under the cursor
        // when released.
        if (g_mouseHold && !anyInputHeld())
            g_mouseHold = false;
        const settings::Spec *capturing = g_capturing.empty() ? nullptr : findSpec(g_capturing);
        ImGuiIO &io = ImGui::GetIO();
        if (g_open && ((capturing && capturing->optional) || g_mouseHold))
            io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
        else
            io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
        if (!g_open)
            return;

        const std::vector<std::string> pages = settings::pages();
        const int n = static_cast<int>(pages.size());
        if (n == 0)
            return;
        g_page = std::clamp(g_page, 0, n - 1);

        const ImGuiViewport *vp = ImGui::GetMainViewport();
        const float scale = std::max(1.0f, vp->Size.y / 720.0f);
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * scale * 1.25f);
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(std::min(vp->Size.x * 0.92f, 720.0f * scale), std::min(vp->Size.y * 0.88f, 540.0f * scale)),
                                 ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.92f);
        // keeps keyboard / gamepad focus after a click on the game picture behind it (not while a combo list is open:
        // focusing the window would close it)
        if (g_focusWindow || (!g_wasFocused && !g_popupWasOpen))
        {
            ImGui::SetNextWindowFocus();
            g_focusWindow = false;
        }
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoScrollWithMouse;
        if (ImGui::Begin("Settings", nullptr, flags))
        {
            const ImGuiStyle &style = ImGui::GetStyle();
            // LB / RB (or Q / E with the keyboard) switch pages
            if (n > 1 && g_capturing.empty() && !g_popupWasOpen)
            {
                if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false))
                    g_selectPage = (g_page + n - 1) % n;
                if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false))
                    g_selectPage = (g_page + 1) % n;
            }

            // the help area fits the longest help of any page, so it is the same height whatever is selected - but at
            // most four lines (a quarter of the window or so), so the rows keep the room: a longer help is drawn smaller
            const float wrap = ImGui::GetContentRegionAvail().x;
            const float line = ImGui::GetTextLineHeightWithSpacing();
            float helpHeight = line * 2.0f;
            for (const settings::Spec &s : settings::all())
                helpHeight = std::max(helpHeight, ImGui::CalcTextSize(helpText(s).c_str(), nullptr, false, wrap).y);
            helpHeight = std::min(helpHeight, line * 4.0f);
            const float footer = (1.0f + style.ItemSpacing.y) + (helpHeight + style.ItemSpacing.y) +
                                 (ImGui::GetTextLineHeight() + style.ItemSpacing.y) + ImGui::GetFrameHeight() + style.ItemSpacing.y +
                                 2.0f;

            const int shownPage = g_page;
            if (ImGui::BeginTabBar("pages"))
            {
                for (int i = 0; i < n; ++i)
                {
                    const ImGuiTabItemFlags tf = g_selectPage == i ? ImGuiTabItemFlags_SetSelected : 0;
                    if (ImGui::BeginTabItem(pages[i].c_str(), nullptr, tf))
                    {
                        g_page = i;
                        ImGui::EndTabItem();
                    }
                }
                g_selectPage = -1;
                ImGui::EndTabBar();
            }
            if (g_page != shownPage)
                g_helpKey.clear();
            const std::string &page = pages[g_page];

            std::string focusedNow;
            ImGui::PushID(page.c_str());
            if (ImGui::BeginChild("rows", ImVec2(0.0f, -footer), ImGuiChildFlags_NavFlattened))
            {
                if (g_scrollTo >= 0.0f && ImGui::GetScrollMaxY() > 0.0f)
                {
                    ImGui::SetScrollY(g_scrollTo * ImGui::GetScrollMaxY());
                    g_scrollTo = -1.0f;
                }
                for (const std::string &section : sections(page))
                {
                    if (!section.empty())
                        ImGui::SeparatorText(section.c_str());
                    if (ImGui::BeginTable(section.c_str(), 2, ImGuiTableFlags_SizingStretchProp))
                    {
                        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.45f);
                        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.55f);
                        for (const settings::Spec &s : settings::all())
                            if (s.page == page && s.section == section && !s.hidden)
                                row(s, focusedNow);
                        ImGui::EndTable();
                    }
                    ImGui::Spacing();
                }
            }
            ImGui::EndChild();
            ImGui::PopID();
            // keyboard / gamepad navigation moved to another row: its help shows
            if (!focusedNow.empty() && focusedNow != g_navFocused)
                g_helpKey = focusedNow;
            g_navFocused = focusedNow;

            ImGui::Separator();
            const settings::Spec *helpSpec = nullptr;
            bool anyRestart = false;
            for (const settings::Spec &s : settings::all())
            {
                if (s.page == page && s.key == g_helpKey)
                    helpSpec = &s;
                anyRestart = anyRestart || restartPending(s);
            }
            if (ImGui::BeginChild("help", ImVec2(0.0f, helpHeight), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar))
            {
                ImGui::PushTextWrapPos(0.0f);
                if (!g_refusal.empty())
                    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Not changed: %s", g_refusal.c_str());
                else if (helpSpec)
                {
                    const std::string text = helpText(*helpSpec);
                    const float base = ImGui::GetFontSize();
                    float size = base; // the largest size, down to 60%, at which the help fits
                    while (size > base * 0.6f &&
                           ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, ImGui::GetContentRegionAvail().x, text.c_str()).y > helpHeight)
                        size -= base * 0.05f;
                    ImGui::PushFont(nullptr, size);
                    ImGui::TextUnformatted(text.c_str());
                    ImGui::PopFont();
                }
                else
                    ImGui::TextDisabled("Point at or select a setting to see what it does.");
                ImGui::PopTextWrapPos();
            }
            ImGui::EndChild();
            if (anyRestart)
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "* changed: applies the next time the game starts");
            else
                ImGui::TextUnformatted(" ");

            if (ImGui::Button("Reset page to defaults"))
                for (const settings::Spec &s : settings::all())
                    if (s.page == page && s.type != settings::Type::Action)
                        settings::set(s.key, s.defaultValue);
            char hint[96];
            std::snprintf(hint, sizeof(hint), "%s / Back+Start: close  Q/E, LB/RB: page", keyName(settings::value("controls.menu_key")));
            ImGui::SameLine();
            if (const float room = ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(hint).x; room > 0.0f)
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + room); // right-aligned
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", hint);
        }
        g_wasFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        ImGui::End();
        ImGui::PopFont();
        g_popupWasOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    }
}
