// See settings_menu.h.
//
// Layout (issue #80, the launcher's Vice City / Miami Vice look): frosted icy-sky to turquoise backdrop, crisp
// header bar with pink underline, left side page rail (active page in Flamingo Pink, matching launcher vcSubTab),
// zebra-striped Vice City rows (drawVcRowStripe) with squared-beveled toggles (vcToggleSwitch), double-ring pink
// sliders with < > chevrons (vcSliderTrack), < Value > cycle rows (vcCycleRow), and a white footer help card with
// a pink left accent bar and top-right badge. Fixed layout heights so nothing ever shifts under the mouse.
#include "ui/settings_menu.h"

#include "platform/platform.h"
#include "runtime/ps2_control.h"
#include "settings/settings.h"
#include "ui/ui_theme.h"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
        std::string g_capturing;     // the Key setting waiting for a key press
        bool g_captureArmed = false; // every key / button was let go since the capture started (the click that began it)
        bool g_mouseHold = false;    // a binding was just made: the menu ignores the mouse until everything is let go
        std::string g_confirming;    // the Action setting waiting for its second click
        double g_confirmUntil = 0.0;
        float g_scrollTo = -1.0f;    // scrollSettingsMenu, at the next frame
        std::string g_helpKey;       // the row whose help shows: the last one hovered or navigated to (kept in between)
        std::string g_navFocused;    // the row whose control had keyboard / gamepad focus last frame

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

        // Formats an integer setting's value using Spec::format ("%d", "%d%%", "Priority %d", ...).
        std::string formatInt(const std::string &fmt, int v)
        {
            if (fmt.empty())
                return std::to_string(v);
            std::string out;
            out.reserve(fmt.size() + 12);
            for (size_t i = 0; i < fmt.size(); ++i)
            {
                if (fmt[i] == '%' && i + 1 < fmt.size())
                {
                    if (fmt[i + 1] == '%')
                    {
                        out.push_back('%');
                        ++i;
                        continue;
                    }
                    if (fmt[i + 1] == 'd' || fmt[i + 1] == 'i')
                    {
                        out += std::to_string(v);
                        ++i;
                        continue;
                    }
                }
                out.push_back(fmt[i]);
            }
            return out;
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

        // ---- Vice City / Miami Vice Controls (matching launcher/ui/widgets.cpp) -----------------------------------

        // Squared-beveled toggle switch (launcher vcToggleSwitch): right-aligned when fullWidth > 0, inline when 0.
        bool vcToggle(const char *id, bool &on, float fullWidth, float s)
        {
            using namespace palette;
            const float frameH = ImGui::GetFrameHeight();
            const float togW = 46.0f * s;
            const float togH = 24.0f * s;
            const float stateSize = 13.0f * s;
            const char *stateLabel = on ? "On" : "Off";
            ImFont *semi = fontSemibold();
            const float labelW = semi->CalcTextSizeA(stateSize, FLT_MAX, 0.0f, "Off").x;
            const float gap = 9.0f * s;
            const float hitW = fullWidth > 0.0f ? fullWidth : (togW + gap + labelW);

            const bool clicked = ImGui::InvisibleButton(id, ImVec2(hitW, frameH));
            if (clicked)
                on = !on;

            const bool hovered = ImGui::IsItemHovered();
            const bool focused = ImGui::IsItemFocused();
            const ImVec2 rMin = ImGui::GetItemRectMin();
            const ImVec2 rMax = ImGui::GetItemRectMax();
            const float cy = (rMin.y + rMax.y) * 0.5f;
            const float px0 = fullWidth > 0.0f ? (rMax.x - togW) : rMin.x;
            const ImVec2 p0(px0, cy - togH * 0.5f);
            const ImVec2 p1(px0 + togW, cy + togH * 0.5f);

            ImDrawList *dl = ImGui::GetWindowDrawList();
            const float alpha = ImGui::GetStyle().Alpha;
            const float trackRad = 6.5f * s;

            // Subtle drop shadow + recessed track
            dl->AddRectFilled(ImVec2(p0.x, p0.y + 1.5f * s), ImVec2(p1.x, p1.y + 1.5f * s),
                              IM_COL32(0, 34, 43, static_cast<int>(55 * alpha)), trackRad);
            const ImU32 trackCol = on ? u32(hovered ? kPink : kPinkSoft, alpha)
                                      : u32(hovered ? kToggleOffHi : kToggleOff, alpha);
            const ImU32 trackBd = on ? u32(kPinkBorder, 0.85f * alpha) : u32(kToggleOffBd, 0.72f * alpha);
            dl->AddRectFilled(p0, p1, trackCol, trackRad);
            dl->AddRect(p0, p1, trackBd, trackRad, 0, std::max(1.0f, 1.2f * s));

            // Sliding soft-beveled square white knob
            const float knobPad = 2.5f * s;
            const float knobSz = togH - knobPad * 2.0f;
            const float kx = on ? (p1.x - knobPad - knobSz) : (p0.x + knobPad);
            const float ky = p0.y + knobPad;
            const float knobRad = 3.5f * s;
            dl->AddRectFilled(ImVec2(kx, ky + 1.5f * s), ImVec2(kx + knobSz, ky + knobSz + 1.5f * s),
                              IM_COL32(0, 27, 36, static_cast<int>(85 * alpha)), knobRad);
            dl->AddRectFilled(ImVec2(kx, ky), ImVec2(kx + knobSz, ky + knobSz), u32(kOnPink, alpha), knobRad);
            dl->AddRect(ImVec2(kx, ky), ImVec2(kx + knobSz, ky + knobSz), u32(0xD0CECEu, alpha), knobRad, 0, 1.0f);

            if (focused)
                dl->AddRect(ImVec2(p0.x - 2.0f * s, p0.y - 2.0f * s), ImVec2(p1.x + 2.0f * s, p1.y + 2.0f * s),
                            u32(kPink, 0.9f * alpha), trackRad + 2.0f * s, 0, std::max(1.5f, 1.4f * s));

            // "On" / "Off" teal text beside the switch (when right-aligned)
            if (fullWidth > 0.0f)
            {
                const ImVec2 ts = semi->CalcTextSizeA(stateSize, FLT_MAX, 0.0f, stateLabel);
                dl->AddText(semi, stateSize, ImVec2(p0.x - gap - ts.x, cy - ts.y * 0.5f),
                            u32(on ? kTealDark : kTealMuted, alpha), stateLabel);
            }

            return clicked;
        }

        // Vice City double-ring pink slider with < > chevrons (launcher vcSliderTrack / vcSliderRow).
        bool vcSlider(const char *id, int &val, int minVal, int maxVal, const std::string &fmt, float width, float s,
                      bool &overItem, bool &focusedOut)
        {
            using namespace palette;
            ImFont *semi = fontSemibold();
            const float frameH = ImGui::GetFrameHeight();
            const float valSize = 13.5f * s;
            const std::string maxSample = formatInt(fmt, std::abs(minVal) > std::abs(maxVal) ? minVal : maxVal);
            const float valW = std::max(50.0f * s, semi->CalcTextSizeA(valSize, FLT_MAX, 0.0f, maxSample.c_str()).x + 6.0f * s);
            const float chevW = 24.0f * s;
            const float trackHitW = std::max(48.0f * s, width - valW - chevW * 2.0f - 4.0f * s);
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const float cy = origin.y + frameH * 0.5f;
            const float alpha = ImGui::GetStyle().Alpha;
            ImDrawList *dl = ImGui::GetWindowDrawList();

            bool changed = false;
            const int step = std::max(1, (maxVal - minVal) >= 100 ? 5 : 1);

            // Left '<' decrement button
            ImGui::SetCursorScreenPos(origin);
            ImGui::PushID(id);
            ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
            const bool decClick = ImGui::InvisibleButton("##dec", ImVec2(chevW, frameH));
            const bool decHover = ImGui::IsItemHovered();
            ImGui::PopItemFlag();
            overItem = overItem || ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
            if (decClick && val > minVal)
            {
                val = std::max(minVal, val - step);
                changed = true;
            }
            drawChevron(dl, ImVec2(origin.x + chevW * 0.5f, cy), false, s * 0.85f,
                        u32(decHover ? kPinkHover : kTeal, alpha));

            // Center slider track backed by ImGui::SliderInt (gives native drag + keyboard/gamepad Left/Right)
            ImGui::SetCursorScreenPos(ImVec2(origin.x + chevW, origin.y));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
            ImGui::SetNextItemWidth(trackHitW);
            if (ImGui::SliderInt("##track", &val, minVal, maxVal, "", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput))
                changed = true;
            ImGui::PopStyleColor(6);

            const bool hovered = ImGui::IsItemHovered();
            const bool active = ImGui::IsItemActive();
            const bool focused = ImGui::IsItemFocused();
            overItem = overItem || ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
            focusedOut = focusedOut || focused;

            const ImVec2 rMin = ImGui::GetItemRectMin();
            const ImVec2 rMax = ImGui::GetItemRectMax();
            const float pad = 10.0f * s;
            const float trackH = 7.5f * s;
            const float tx0 = rMin.x + pad;
            const float tx1 = std::max(tx0 + trackH, rMax.x - pad);
            const float ty0 = cy - trackH * 0.5f;
            const float ty1 = cy + trackH * 0.5f;
            const float span = static_cast<float>(std::max(1, maxVal - minVal));
            const float t = std::clamp(static_cast<float>(val - minVal) / span, 0.0f, 1.0f);

            // Ice-blue empty track + teal hairline
            dl->AddRectFilled(ImVec2(tx0, ty0), ImVec2(tx1, ty1), u32(kTrackOff, alpha), trackH * 0.5f);
            dl->AddRect(ImVec2(tx0, ty0), ImVec2(tx1, ty1), u32(kTeal, 0.37f * alpha), trackH * 0.5f, 0, 1.0f);

            // Flamingo pink active fill
            const float knobX = tx0 + (tx1 - tx0) * t;
            if (t > 0.002f)
            {
                const float fillX1 = std::max(tx0 + trackH, knobX);
                dl->AddRectFilled(ImVec2(tx0, ty0), ImVec2(fillX1, ty1), u32(kPink, alpha), trackH * 0.5f);
            }

            // Double-ring target knob (◎) in Pink & White
            const float outerR = (hovered || active) ? 10.0f * s : 9.0f * s;
            dl->AddCircleFilled(ImVec2(knobX, cy + 1.2f * s), outerR, IM_COL32(128, 16, 69, static_cast<int>(70 * alpha)));
            dl->AddCircleFilled(ImVec2(knobX, cy), outerR, u32(kOnPink, alpha));
            dl->AddCircle(ImVec2(knobX, cy), outerR, u32(kPink, alpha), 0, std::max(1.8f, 2.1f * s));
            dl->AddCircleFilled(ImVec2(knobX, cy), outerR * 0.48f, u32(active ? kPink : kPinkSoft, alpha));

            if (focused)
                dl->AddRect(ImVec2(origin.x, origin.y + 1.0f * s), ImVec2(origin.x + width, origin.y + frameH - 1.0f * s),
                            u32(kPink, 0.85f * alpha), 3.0f * s, 0, std::max(1.5f, 1.3f * s));

            // Right '>' increment button
            ImGui::SetCursorScreenPos(ImVec2(origin.x + chevW + trackHitW, origin.y));
            ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
            const bool incClick = ImGui::InvisibleButton("##inc", ImVec2(chevW, frameH));
            const bool incHover = ImGui::IsItemHovered();
            ImGui::PopItemFlag();
            overItem = overItem || ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
            if (incClick && val < maxVal)
            {
                val = std::min(maxVal, val + step);
                changed = true;
            }
            drawChevron(dl, ImVec2(origin.x + chevW + trackHitW + chevW * 0.5f, cy), true, s * 0.85f,
                        u32(incHover ? kPinkHover : kTeal, alpha));

            ImGui::PopID();

            // Right-aligned formatted value in Vice City teal
            const std::string valText = formatInt(fmt, val);
            const ImVec2 ts = semi->CalcTextSizeA(valSize, FLT_MAX, 0.0f, valText.c_str());
            dl->AddText(semi, valSize, ImVec2(origin.x + width - ts.x, cy - ts.y * 0.5f), u32(kTeal, alpha), valText.c_str());

            return changed;
        }

        // Vice City < Value > cycle control (launcher vcCycleRow): click < / > or value, or Left/Right when focused.
        bool vcCycle(const settings::Spec &s, int v, float width, float scale, bool &overItem, bool &focusedOut)
        {
            using namespace palette;
            const int n = static_cast<int>(s.choices.size());
            if (n <= 0)
                return false;
            int idx = 0;
            for (int i = 0; i < n; ++i)
                if (s.choices[static_cast<size_t>(i)].value == v)
                    idx = i;

            const float frameH = ImGui::GetFrameHeight();
            const float chevW = 32.0f * scale;
            const float valW = std::max(40.0f * scale, width - chevW * 2.0f);
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const float cy = origin.y + frameH * 0.5f;
            const float alpha = ImGui::GetStyle().Alpha;
            ImDrawList *dl = ImGui::GetWindowDrawList();

            // Left '<'
            ImGui::SetCursorScreenPos(origin);
            ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
            const bool prevClick = ImGui::InvisibleButton("##prev", ImVec2(chevW, frameH));
            const bool prevHover = ImGui::IsItemHovered();
            ImGui::PopItemFlag();
            overItem = overItem || ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);

            // Center focusable value button (Enter/A/click cycles forward; Left/Right cycles both ways)
            ImGui::SetCursorScreenPos(ImVec2(origin.x + chevW, origin.y));
            const bool valClick = ImGui::InvisibleButton("##val", ImVec2(valW, frameH));
            const bool valHover = ImGui::IsItemHovered();
            const bool focused = ImGui::IsItemFocused();
            overItem = overItem || ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
            focusedOut = focusedOut || focused;

            // Right '>'
            ImGui::SetCursorScreenPos(ImVec2(origin.x + chevW + valW, origin.y));
            ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
            const bool nextClick = ImGui::InvisibleButton("##next", ImVec2(chevW, frameH));
            const bool nextHover = ImGui::IsItemHovered();
            ImGui::PopItemFlag();
            overItem = overItem || ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);

            int nextIdx = idx;
            const bool keyLeft = focused && (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true) ||
                                             ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, true));
            const bool keyRight = focused && (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true) ||
                                              ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, true));
            if (prevClick || keyLeft)
                nextIdx = (idx + n - 1) % n;
            else if (nextClick || valClick || keyRight)
                nextIdx = (idx + 1) % n;

            drawChevron(dl, ImVec2(origin.x + chevW * 0.5f, cy), false, scale * 0.9f,
                        u32(prevHover ? kPinkHover : kTeal, alpha));
            drawChevron(dl, ImVec2(origin.x + chevW + valW + chevW * 0.5f, cy), true, scale * 0.9f,
                        u32(nextHover ? kPinkHover : kTeal, alpha));

            ImFont *semi = fontSemibold();
            const float fontSize = 14.0f * scale;
            const std::string &label = s.choices[static_cast<size_t>(idx)].label;
            const ImVec2 ts = semi->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, label.c_str());
            dl->PushClipRect(ImVec2(origin.x + chevW, origin.y), ImVec2(origin.x + chevW + valW, origin.y + frameH), true);
            dl->AddText(semi, fontSize, ImVec2(origin.x + chevW + (valW - ts.x) * 0.5f, cy - ts.y * 0.5f),
                        u32(valHover || focused ? kTealDark : kTeal, alpha), label.c_str());
            dl->PopClipRect();

            if (focused)
                dl->AddRect(ImVec2(origin.x, origin.y + 1.0f * scale), ImVec2(origin.x + width, origin.y + frameH - 1.0f * scale),
                            u32(kPink, 0.85f * alpha), 3.0f * scale, 0, std::max(1.5f, 1.3f * scale));

            if (nextIdx != idx)
            {
                settings::set(s.key, s.choices[static_cast<size_t>(nextIdx)].value);
                return true;
            }
            return false;
        }

        // One key button: its key's name, or "Press a key..." while it waits for one. `note` runs right after the button.
        template <typename Note> void keySlot(const settings::Spec &s, float width, float scale, Note note)
        {
            using namespace palette;
            const int v = settings::value(s.key);
            const bool capturing = g_capturing == s.key;
            ImGui::PushFont(fontSemibold(), 13.0f * scale);
            if (capturing)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, rgba(kPinkSoft));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rgba(kPink));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, rgba(kPink));
                ImGui::PushStyleColor(ImGuiCol_Border, rgba(kPinkBorder));
                ImGui::PushStyleColor(ImGuiCol_Text, rgba(kOnPink));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.5f);
                ImGui::Button(s.optional ? "Press a key or button... (Esc)###k" : "Press a key... (Esc: cancel)###k",
                              ImVec2(width, 0));
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(5);
                ImGui::PopFont();
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
            ImGui::PushStyleColor(ImGuiCol_Button, rgba(kRowEven, 0.95f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rgba(kRowHover));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, rgba(kPinkSoft));
            ImGui::PushStyleColor(ImGuiCol_Border, rgba(kTealLine, 0.35f));
            ImGui::PushStyleColor(ImGuiCol_Text, rgba(kTealDark));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            const bool pressed = ImGui::Button((std::string(name) + "###k").c_str(), ImVec2(width, 0));
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(5);
            ImGui::PopFont();
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

        // One zebra-striped Vice City row: label on the left, Miami Vice control on the right.
        void row(const settings::Spec &s, int rowIndex, float left, float right, float ctrlX, float ctrlW, float scale,
                 std::string &focusedNow)
        {
            using namespace palette;
            const ImGuiStyle &style = ImGui::GetStyle();
            const bool byEnv = settings::overridden(s.key);
            const bool bySetting = !byEnv && s.locked && s.locked(); // another setting decides it for now
            const bool locked = byEnv || bySetting;
            const int v = bySetting ? s.lockedValue : settings::value(s.key);

            const float frameH = ImGui::GetFrameHeight();
            const float padY = 5.5f * scale;
            const float rowH = frameH + padY * 2.0f;
            const float rowTop = ImGui::GetCursorScreenPos().y;
            const float rowBot = rowTop + rowH;

            ImDrawList *dl = ImGui::GetWindowDrawList();
            const ImVec2 mouse = ImGui::GetMousePos();
            const bool hoveredRow = ImGui::IsWindowHovered() && mouse.x >= left && mouse.x <= right &&
                                    mouse.y >= rowTop && mouse.y < rowBot;
            const bool activeRow = g_helpKey == s.key || hoveredRow;

            // Zebra stripe (launcher drawVcRowStripe)
            const ImU32 stripeBg = activeRow ? u32(kRowHover, 0.96f)
                                             : (rowIndex % 2 == 0 ? u32(kRowEven, 0.94f) : u32(kRowOdd, 0.91f));
            dl->AddRectFilled(ImVec2(left, rowTop), ImVec2(right, rowBot), stripeBg);
            if (activeRow)
                dl->AddRectFilled(ImVec2(left, rowTop), ImVec2(left + 4.5f * scale, rowBot), u32(kPinkSoft));

            // Check if this is a Mod toggle row (toggle + partner Integer slider): put toggle on the left like launcher vcModRow
            const settings::Spec *partner = s.partner.empty() ? nullptr : findSpec(s.partner);
            const bool isModRow = s.type == settings::Type::Toggle && partner && partner->type == settings::Type::Integer;

            float labelX = left + 18.0f * scale;
            bool overItem = false;
            bool itemFocused = false;
            auto noteItem = [&] {
                overItem = overItem || ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
                if (ImGui::IsItemFocused())
                    itemFocused = true;
            };

            ImGui::PushID(s.key.c_str());

            if (isModRow)
            {
                ImGui::BeginDisabled(locked);
                ImGui::SetCursorScreenPos(ImVec2(left + 14.0f * scale, rowTop + padY));
                bool on = v != 0;
                if (vcToggle("##v", on, 0.0f, scale))
                    settings::set(s.key, on ? 1 : 0);
                noteItem();
                ImGui::EndDisabled();
                labelX = left + 14.0f * scale + 46.0f * scale + 14.0f * scale;
            }

            // Label (left column, clipped so a long label never collides with the control)
            ImGui::PushFont(fontSemibold(), 14.0f * scale);
            ImGui::PushClipRect(ImVec2(labelX - 2.0f * scale, rowTop), ImVec2(ctrlX - 8.0f * scale, rowBot), true);
            ImGui::SetCursorScreenPos(ImVec2(labelX, rowTop + padY));
            ImGui::AlignTextToFramePadding();
            if (byEnv)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, rgba(kTealFaint));
                ImGui::Text("%s (%s)", s.label.c_str(), s.env.c_str());
                ImGui::PopStyleColor();
            }
            else if (bySetting)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, rgba(kTealFaint));
                ImGui::TextUnformatted(s.label.c_str());
                ImGui::PopStyleColor();
            }
            else
            {
                ImGui::PushStyleColor(ImGuiCol_Text, rgba(activeRow ? kTealDark : kTeal));
                ImGui::TextUnformatted(s.label.c_str());
                ImGui::PopStyleColor();
            }
            if (restartPending(s))
            {
                ImGui::SameLine(0.0f, 5.0f * scale);
                ImGui::PushStyleColor(ImGuiCol_Text, rgba(kPink));
                ImGui::TextUnformatted("*");
                ImGui::PopStyleColor();
            }
            ImGui::PopClipRect();
            ImGui::PopFont();

            // Control (right column)
            ImGui::SetCursorScreenPos(ImVec2(ctrlX, rowTop + padY));
            ImGui::BeginDisabled(locked);

            switch (s.type)
            {
            case settings::Type::Toggle:
            {
                if (isModRow)
                {
                    ImGui::BeginDisabled(v == 0 || settings::overridden(partner->key));
                    int edited = settings::value(partner->key);
                    if (vcSlider("##partner", edited, partner->min, partner->max, partner->format, ctrlW, scale, overItem,
                                 itemFocused))
                        settings::set(partner->key, edited);
                    ImGui::EndDisabled();
                }
                else
                {
                    bool on = v != 0;
                    if (vcToggle("##v", on, ctrlW, scale))
                        settings::set(s.key, on ? 1 : 0);
                    noteItem();
                }
                break;
            }
            case settings::Type::Choice:
            {
                vcCycle(s, v, ctrlW, scale, overItem, itemFocused);
                break;
            }
            case settings::Type::Integer:
            {
                int edited = v;
                if (vcSlider("##v", edited, s.min, s.max, s.format, ctrlW, scale, overItem, itemFocused))
                    settings::set(s.key, edited);
                break;
            }
            case settings::Type::Key:
            {
                const settings::Spec *second = s.partner.empty() ? nullptr : findSpec(s.partner);
                const float gap = style.ItemSpacing.x;
                const float width = second ? (ctrlW - gap) * 0.5f : ctrlW;
                keySlot(s, width, scale, noteItem);
                if (second)
                {
                    ImGui::SameLine(0.0f, gap);
                    ImGui::PushID(1);
                    keySlot(*second, ctrlW - width - gap, scale, noteItem);
                    ImGui::PopID();
                }
                break;
            }
            case settings::Type::Action:
            {
                const bool confirming = g_confirming == s.key && ImGui::GetTime() < g_confirmUntil;
                const std::string labelText = confirming ? "Click again to confirm" : (s.button.empty() ? s.label : s.button);
                const std::string text = confirming ? labelText : ("<  " + labelText + "  >");
                ImGui::PushFont(fontSemibold(), 13.0f * scale);
                if (confirming)
                {
                    ImGui::PushStyleColor(ImGuiCol_Button, rgba(kDangerFill));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rgba(kDangerFillHi));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, rgba(kDangerFillHi));
                    ImGui::PushStyleColor(ImGuiCol_Border, rgba(kRed));
                    ImGui::PushStyleColor(ImGuiCol_Text, rgba(kDangerText));
                }
                else
                {
                    ImGui::PushStyleColor(ImGuiCol_Button, rgba(kRowEven, 0.92f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rgba(kRowHover));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, rgba(kPinkSoft));
                    ImGui::PushStyleColor(ImGuiCol_Border, rgba(kTealLine, 0.30f));
                    ImGui::PushStyleColor(ImGuiCol_Text, rgba(kTeal));
                }
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                const bool pressed = ImGui::Button((text + "###v").c_str(), ImVec2(ctrlW, 0));
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(5);
                ImGui::PopFont();
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

            if (itemFocused)
                focusedNow = s.key;

            ImGui::SetCursorScreenPos(ImVec2(left, rowBot));

            const ImGuiIO &io = ImGui::GetIO();
            if (hoveredRow && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f || ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                               g_helpKey.empty()))
                g_helpKey = s.key;
            if (hoveredRow && !overItem && !locked && s.type == settings::Type::Toggle && !g_popupWasOpen &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                settings::set(s.key, v != 0 ? 0 : 1);
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
        const int key = settings::value("controls.menu_key");
        const bool menuKey = g_capturing.empty() && (platform::keyPressed(key) || ps2control::keyPressed(key));
        if (menuKey || chordPressed)
            g_open ? close() : open();
        else if (g_open && g_capturing.empty() && !g_popupWasOpen &&
                 (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)))
            close();

        if (g_releaseWait && !anyInputHeld())
            g_releaseWait = false;
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

        using namespace palette;
        const ImGuiViewport *vp = ImGui::GetMainViewport();
        const float s = std::clamp((vp->Size.y / 720.0f) * 0.85f, 0.72f, 3.4f);
        const float bodySize = 14.0f * s;

        ImGui::PushFont(fontRegular(), bodySize);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0)); // painted with Miami sky->turquoise gradient
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border, rgba(kTealLine, 0.35f));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, rgba(kHeaderBg, 0.40f));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, rgba(kTeal, 0.45f));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, rgba(kPinkSoft, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, rgba(kPink));
        ImGui::PushStyleColor(ImGuiCol_NavCursor, rgba(kPink));
        ImGui::PushStyleColor(ImGuiCol_Text, rgba(kTealDark));

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f * s);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f * s, 5.5f * s));
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 2.0f * s);
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 9.0f * s);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f * s, 5.0f * s));

        const ImVec2 winSize(std::min(vp->Size.x * 0.90f, 900.0f * s), std::min(vp->Size.y * 0.88f, 590.0f * s));
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always,
                                ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(winSize, ImGuiCond_Always);

        if (g_focusWindow || (!g_wasFocused && !g_popupWasOpen))
        {
            ImGui::SetNextWindowFocus();
            g_focusWindow = false;
        }
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        if (ImGui::Begin("Settings", nullptr, flags))
        {
            // LB / RB (or Q / E with the keyboard) switch pages
            if (n > 1 && g_capturing.empty() && !g_popupWasOpen)
            {
                if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false))
                    g_selectPage = (g_page + n - 1) % n;
                if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false))
                    g_selectPage = (g_page + 1) % n;
            }
            if (g_selectPage >= 0 && g_selectPage < n)
            {
                if (g_selectPage != g_page)
                    g_helpKey.clear();
                g_page = g_selectPage;
            }
            g_selectPage = -1;

            const ImVec2 wPos = ImGui::GetWindowPos();
            const ImVec2 wSize = ImGui::GetWindowSize();
            ImDrawList *wdl = ImGui::GetWindowDrawList();
            ImFont *semi = fontSemibold();
            ImFont *reg = fontRegular();

            // ---- Frosted Miami Vice backdrop (icy sky -> turquoise gradient) --------------------------------------
            const float midY = wPos.y + wSize.y * 0.45f;
            wdl->AddRectFilledMultiColor(wPos, ImVec2(wPos.x + wSize.x, midY), u32(kSkyTop, 0.93f), u32(kSkyTop, 0.93f),
                                         u32(kSkyMid, 0.92f), u32(kSkyMid, 0.92f));
            wdl->AddRectFilledMultiColor(ImVec2(wPos.x, midY), ImVec2(wPos.x + wSize.x, wPos.y + wSize.y),
                                         u32(kSkyMid, 0.92f), u32(kSkyMid, 0.92f), u32(kSkyBot, 0.94f), u32(kSkyBot, 0.94f));

            // ---- Header bar (frosted sky with hot-pink underline under active page) -------------------------------
            const float headerH = 48.0f * s;
            wdl->AddRectFilled(wPos, ImVec2(wPos.x + wSize.x, wPos.y + headerH), u32(kHeaderBg, 0.88f));
            wdl->AddLine(ImVec2(wPos.x, wPos.y + headerH), ImVec2(wPos.x + wSize.x, wPos.y + headerH), u32(kTealLine, 0.22f),
                         1.0f);

            const float titleSize = 16.5f * s;
            const char *titleStr = "SETTINGS";
            const ImVec2 titleTs = semi->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, titleStr);
            const float titleX = wPos.x + 20.0f * s;
            const float titleY = wPos.y + (headerH - titleTs.y) * 0.5f;
            wdl->AddText(semi, titleSize, ImVec2(titleX, titleY), u32(kTealDark), titleStr);
            // Hot-pink underline (matching launcher's top nav active underline)
            wdl->AddRectFilled(ImVec2(titleX, wPos.y + headerH - 3.5f * s),
                               ImVec2(titleX + titleTs.x, wPos.y + headerH), u32(kPink));

            const std::string &curPage = pages[static_cast<size_t>(g_page)];
            const float subSize = 14.0f * s;
            const std::string crumb = "/   " + curPage;
            const ImVec2 crumbTs = semi->CalcTextSizeA(subSize, FLT_MAX, 0.0f, crumb.c_str());
            wdl->AddText(semi, subSize,
                         ImVec2(titleX + titleTs.x + 14.0f * s, wPos.y + (headerH - crumbTs.y) * 0.5f),
                         u32(kTeal), crumb.c_str());

            // Close button (top right)
            const float closeSz = 30.0f * s;
            const ImVec2 closePos(wPos.x + wSize.x - 14.0f * s - closeSz, wPos.y + (headerH - closeSz) * 0.5f);
            ImGui::SetCursorScreenPos(closePos);
            ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
            if (ImGui::InvisibleButton("##close", ImVec2(closeSz, closeSz)))
                close();
            ImGui::PopItemFlag();
            const bool closeHover = ImGui::IsItemHovered();
            if (closeHover)
                wdl->AddRectFilled(closePos, ImVec2(closePos.x + closeSz, closePos.y + closeSz), u32(kPinkSoft, 0.85f));
            const ImVec2 cc(closePos.x + closeSz * 0.5f, closePos.y + closeSz * 0.5f);
            const float xr = 5.0f * s;
            const ImU32 xCol = u32(closeHover ? kOnPink : kTealDark);
            wdl->AddLine(ImVec2(cc.x - xr, cc.y - xr), ImVec2(cc.x + xr, cc.y + xr), xCol, std::max(1.4f, 1.5f * s));
            wdl->AddLine(ImVec2(cc.x + xr, cc.y - xr), ImVec2(cc.x - xr, cc.y + xr), xCol, std::max(1.4f, 1.5f * s));

            // ---- Body: left side page rail + right content pane ---------------------------------------------------
            const float bodyTop = wPos.y + headerH;
            const float bodyH = wSize.y - headerH;
            const float sideW = std::clamp(192.0f * s, 150.0f * s, wSize.x * 0.27f);

            wdl->AddRectFilled(ImVec2(wPos.x, bodyTop), ImVec2(wPos.x + sideW, wPos.y + wSize.y), u32(kRailBg, 0.72f));
            wdl->AddLine(ImVec2(wPos.x + sideW, bodyTop), ImVec2(wPos.x + sideW, wPos.y + wSize.y), u32(kTealLine, 0.22f),
                         1.0f);

            const float sidePad = 10.0f * s;
            const float resetH = 32.0f * s;
            const float listH = std::max(60.0f * s, bodyH - resetH - sidePad * 2.0f - 8.0f * s);
            const float itemH = 32.0f * s;
            const float itemGap = 2.0f * s;
            const float navFontSize = 13.5f * s;

            ImGui::SetCursorScreenPos(ImVec2(wPos.x, bodyTop + 8.0f * s));
            ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
            if (ImGui::BeginChild("pages", ImVec2(sideW - 1.0f, listH), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav))
            {
                ImDrawList *sdl = ImGui::GetWindowDrawList();
                const float availW = ImGui::GetContentRegionAvail().x;
                float navY = ImGui::GetCursorScreenPos().y;
                const float navX = ImGui::GetCursorScreenPos().x;

                for (int i = 0; i < n; ++i)
                {
                    const bool sel = i == g_page;
                    const ImVec2 p0(navX + sidePad, navY);
                    const ImVec2 p1(navX + availW - sidePad, navY + itemH);
                    ImGui::SetCursorScreenPos(p0);
                    ImGui::PushID(i);
                    const bool clicked = ImGui::InvisibleButton("##page", ImVec2(p1.x - p0.x, itemH));
                    const bool hov = ImGui::IsItemHovered();
                    ImGui::PopID();
                    if (clicked && !sel)
                    {
                        g_page = i;
                        g_helpKey.clear();
                    }

                    // Active tab = solid Flamingo Pink (#FE7FB4) with white text/icon (exact match to launcher vcSubTab)
                    if (sel)
                        sdl->AddRectFilled(p0, p1, u32(kPinkSoft));
                    else
                        sdl->AddRectFilled(p0, p1, u32(hov ? kTabHover : kTabIdle, hov ? 0.94f : 0.80f));

                    const ImU32 iconCol = u32(sel ? kOnPink : (hov ? kPinkHover : kTeal));
                    const ImU32 textCol = u32(sel ? kOnPink : (hov ? kTealDark : kTeal));
                    drawPageIcon(sdl, pages[static_cast<size_t>(i)], ImVec2(p0.x + 17.0f * s, (p0.y + p1.y) * 0.5f), 14.5f * s,
                                 iconCol);
                    const ImVec2 pts = semi->CalcTextSizeA(navFontSize, FLT_MAX, 0.0f, pages[static_cast<size_t>(i)].c_str());
                    sdl->AddText(semi, navFontSize, ImVec2(p0.x + 34.0f * s, (p0.y + p1.y - pts.y) * 0.5f), textCol,
                                 pages[static_cast<size_t>(i)].c_str());

                    navY += itemH + itemGap;
                }
                ImGui::SetCursorScreenPos(ImVec2(navX, navY));
                ImGui::Dummy(ImVec2(0.0f, 0.0f));
            }
            ImGui::EndChild();
            ImGui::PopItemFlag();

            // Reset page button at bottom of side rail
            const ImVec2 resetPos(wPos.x + sidePad, wPos.y + wSize.y - sidePad - resetH);
            ImGui::SetCursorScreenPos(resetPos);
            ImGui::PushFont(semi, 12.5f * s);
            ImGui::PushStyleColor(ImGuiCol_Button, rgba(kRowEven, 0.92f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rgba(kRowHover));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, rgba(kPinkSoft));
            ImGui::PushStyleColor(ImGuiCol_Border, rgba(kTealLine, 0.30f));
            ImGui::PushStyleColor(ImGuiCol_Text, rgba(kTeal));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            if (ImGui::Button("Reset page to defaults", ImVec2(sideW - sidePad * 2.0f, resetH)))
                for (const settings::Spec &sp : settings::all())
                    if (sp.page == pages[static_cast<size_t>(g_page)] && sp.type != settings::Type::Action)
                        settings::set(sp.key, sp.defaultValue);
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(5);
            ImGui::PopFont();

            // ---- Right pane: scrollable zebra rows + white Miami inspection/help card -----------------------------
            const float panePadX = 16.0f * s;
            const float panePadY = 12.0f * s;
            const float paneX = wPos.x + sideW + panePadX;
            const float paneW = wSize.x - sideW - panePadX * 2.0f;

            // Fixed-height footer help card (matching the bottom white card with pink left bar in the launcher)
            const float helpPadX = 16.0f * s;
            const float helpPadY = 10.0f * s;
            const float helpTitleSize = 12.5f * s;
            const float helpFontSize = 13.0f * s;
            const float helpWrap = std::max(100.0f * s, paneW - helpPadX * 2.0f);
            const float helpLine = helpFontSize * 1.32f;
            float helpTextH = helpLine * 2.0f;
            for (const settings::Spec &sp : settings::all())
                helpTextH = std::max(helpTextH, reg->CalcTextSizeA(helpFontSize, FLT_MAX, helpWrap, helpText(sp).c_str()).y);
            helpTextH = std::min(helpTextH, helpLine * 3.0f);
            const float wellH = helpPadY + helpTitleSize + 5.0f * s + helpTextH + helpPadY;
            const float metaH = 18.0f * s;
            const float footerGap = 10.0f * s;
            const float footerTotalH = footerGap + wellH + 6.0f * s + metaH + panePadY;
            const float rowsH = std::max(80.0f * s, bodyH - panePadY - footerTotalH);

            const std::string &shownPage = pages[static_cast<size_t>(g_page)];
            std::string focusedNow;
            ImGui::SetCursorScreenPos(ImVec2(paneX, bodyTop + panePadY));
            ImGui::PushID(shownPage.c_str());
            if (ImGui::BeginChild("rows", ImVec2(paneW, rowsH), ImGuiChildFlags_NavFlattened))
            {
                if (g_scrollTo >= 0.0f && ImGui::GetScrollMaxY() > 0.0f)
                {
                    ImGui::SetScrollY(g_scrollTo * ImGui::GetScrollMaxY());
                    g_scrollTo = -1.0f;
                }
                const float availW = ImGui::GetContentRegionAvail().x;
                const float secFontSize = 12.5f * s;
                const float secH = 26.0f * s;
                ImDrawList *cdl = ImGui::GetWindowDrawList();

                for (const std::string &section : sections(shownPage))
                {
                    const ImVec2 secStart = ImGui::GetCursorScreenPos();
                    const float left = secStart.x;
                    const float right = left + availW;
                    const float ctrlW = std::min(310.0f * s, availW * 0.48f);
                    const float ctrlX = right - 14.0f * s - ctrlW;

                    if (!section.empty())
                    {
                        cdl->AddRectFilled(ImVec2(left, secStart.y), ImVec2(right, secStart.y + secH), u32(kSectionBg, 0.92f));
                        cdl->AddRectFilled(ImVec2(left, secStart.y), ImVec2(left + 4.0f * s, secStart.y + secH), u32(kPink));
                        const ImVec2 sts = semi->CalcTextSizeA(secFontSize, FLT_MAX, 0.0f, section.c_str());
                        cdl->AddText(semi, secFontSize, ImVec2(left + 14.0f * s, secStart.y + (secH - sts.y) * 0.5f),
                                     u32(kTealDark), section.c_str());
                        ImGui::SetCursorScreenPos(ImVec2(left, secStart.y + secH));
                    }

                    int rowIdx = 0;
                    for (const settings::Spec &sp : settings::all())
                    {
                        if (sp.page == shownPage && sp.section == section && !sp.hidden)
                        {
                            row(sp, rowIdx++, left, right, ctrlX, ctrlW, s, focusedNow);
                        }
                    }

                    const float secEndY = ImGui::GetCursorScreenPos().y;
                    cdl->AddRect(ImVec2(left, secStart.y), ImVec2(right, secEndY), u32(kTealLine, 0.22f), 0.0f, 0, 1.0f);
                    ImGui::SetCursorScreenPos(ImVec2(left, secEndY + 10.0f * s));
                    ImGui::Dummy(ImVec2(0.0f, 0.0f));
                }
            }
            ImGui::EndChild();
            ImGui::PopID();

            if (!focusedNow.empty() && focusedNow != g_navFocused)
                g_helpKey = focusedNow;
            g_navFocused = focusedNow;

            // ---- White Miami footer help card with pink left bar & top-right badge --------------------------------
            const settings::Spec *helpSpec = nullptr;
            bool anyRestart = false;
            for (const settings::Spec &sp : settings::all())
            {
                if (sp.page == shownPage && sp.key == g_helpKey)
                    helpSpec = &sp;
                anyRestart = anyRestart || restartPending(sp);
            }

            const float wellY = bodyTop + panePadY + rowsH + footerGap;
            wdl->AddRectFilled(ImVec2(paneX, wellY), ImVec2(paneX + paneW, wellY + wellH), u32(kRowEven, 0.96f));
            wdl->AddRectFilled(ImVec2(paneX, wellY), ImVec2(paneX + 4.5f * s, wellY + wellH), u32(kPinkSoft));
            wdl->AddRect(ImVec2(paneX, wellY), ImVec2(paneX + paneW, wellY + wellH), u32(kTealLine, 0.20f), 0.0f, 0, 1.0f);

            // Card header: setting name on left, pink badge on right
            const std::string cardTitle = !g_refusal.empty() ? "KEY ALREADY IN USE"
                                                             : (helpSpec ? helpSpec->label : "SETTINGS");
            wdl->AddText(semi, helpTitleSize, ImVec2(paneX + helpPadX, wellY + helpPadY),
                         u32(!g_refusal.empty() ? kRed : kTealDark), cardTitle.c_str());

            const char *badgeText = anyRestart ? "RESTART REQUIRED" : "DIRECT VCS.INI";
            const float badgeFontSize = 10.5f * s;
            const ImVec2 bts = semi->CalcTextSizeA(badgeFontSize, FLT_MAX, 0.0f, badgeText);
            const float badgeW = bts.x + 18.0f * s;
            const float badgeH = bts.y + 7.0f * s;
            const ImVec2 bp0(paneX + paneW - 12.0f * s - badgeW, wellY + helpPadY - 2.0f * s);
            const ImVec2 bp1(bp0.x + badgeW, bp0.y + badgeH);
            wdl->AddRectFilled(bp0, bp1, u32(anyRestart ? kPink : kPinkSoft));
            wdl->AddText(semi, badgeFontSize, ImVec2(bp0.x + (badgeW - bts.x) * 0.5f, bp0.y + (badgeH - bts.y) * 0.5f),
                         u32(kOnPink), badgeText);

            ImGui::SetCursorScreenPos(ImVec2(paneX + helpPadX, wellY + helpPadY + helpTitleSize + 5.0f * s));
            if (ImGui::BeginChild("help", ImVec2(paneW - helpPadX * 2.0f, helpTextH), ImGuiChildFlags_None,
                                  ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar))
            {
                ImGui::PushTextWrapPos(0.0f);
                if (!g_refusal.empty())
                {
                    ImGui::PushFont(semi, helpFontSize);
                    ImGui::PushStyleColor(ImGuiCol_Text, rgba(kRed));
                    ImGui::Text("Not changed: %s", g_refusal.c_str());
                    ImGui::PopStyleColor();
                    ImGui::PopFont();
                }
                else if (helpSpec)
                {
                    const std::string text = helpText(*helpSpec);
                    float size = helpFontSize;
                    while (size > helpFontSize * 0.6f &&
                           reg->CalcTextSizeA(size, FLT_MAX, ImGui::GetContentRegionAvail().x, text.c_str()).y > helpTextH)
                        size -= helpFontSize * 0.05f;
                    ImGui::PushFont(reg, size);
                    ImGui::PushStyleColor(ImGuiCol_Text, rgba(kTeal));
                    ImGui::TextUnformatted(text.c_str());
                    ImGui::PopStyleColor();
                    ImGui::PopFont();
                }
                else
                {
                    ImGui::PushFont(reg, helpFontSize);
                    ImGui::PushStyleColor(ImGuiCol_Text, rgba(kTealMuted));
                    ImGui::TextUnformatted("Point at or select a setting to see what it does.");
                    ImGui::PopStyleColor();
                    ImGui::PopFont();
                }
                ImGui::PopTextWrapPos();
            }
            ImGui::EndChild();

            // Bottom hint strip
            const float metaY = wellY + wellH + 6.0f * s;
            const float hintSize = 12.0f * s;
            if (anyRestart)
                wdl->AddText(semi, hintSize, ImVec2(paneX + 2.0f * s, metaY), u32(kTealDark),
                             "* marked settings apply the next time the game starts");

            char hint[96];
            std::snprintf(hint, sizeof(hint), "%s / Back+Start: close   Q/E, LB/RB: page",
                          keyName(settings::value("controls.menu_key")));
            const ImVec2 hts = semi->CalcTextSizeA(hintSize, FLT_MAX, 0.0f, hint);
            wdl->AddText(semi, hintSize, ImVec2(paneX + paneW - hts.x - 2.0f * s, metaY), u32(kTealDark, 0.85f), hint);
        }
        g_wasFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        ImGui::End();
        ImGui::PopStyleVar(8);
        ImGui::PopStyleColor(9);
        ImGui::PopFont();
        g_popupWasOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    }
}
