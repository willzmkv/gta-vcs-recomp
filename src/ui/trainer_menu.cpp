// See trainer_menu.h.
//
// Drawn straight into Dear ImGui's foreground draw list (no ImGui widgets): the look is the trainer's own, the input is
// read here (keyboard, numpad, gamepad, with key repeat), and nothing moves under the mouse. Pages are rebuilt every
// frame from the trainer's state, so a row always shows what is true now.
#include "ui/trainer_menu.h"

#include "game/vcs_trainer.h"
#include "platform/platform.h"
#include "runtime/ps2_control.h"
#include "settings/settings.h"
#include "ui/settings_menu.h"
#include "ui/ui_theme.h"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace ui
{
    namespace
    {
        namespace tr = vcs::trainer;

        enum Page
        {
            Main,
            Player,
            Weapons,
            WeaponList,
            Vehicles,
            VehicleList,
            Teleport,
            Camera,
            World,
            Missions,
            StoryList,
            JobList,
        };
        const char *const kPageTitles[] = {"MAIN MENU", "PLAYER", "WEAPONS", "GIVE WEAPON", "VEHICLES", "SPAWN VEHICLE",
                                           "TELEPORT", "CAMERA", "WORLD", "MISSIONS", "STORY", "SIDE JOBS"};

        enum class Value
        {
            None,
            On,
            Off,
            Choice,
            Page,
        };

        struct Row
        {
            std::string label;
            Value kind = Value::None;
            std::string value; // Choice
            std::string help;
            std::function<void()> select;    // Enter / A
            std::function<void(int)> change; // Left / Right: -1 / +1
            bool enabled = true;
        };

        struct Level
        {
            Page page;
            int selected = 0;
            int top = 0; // first row shown
        };

        bool g_open = false;
        bool g_releaseWait = false;
        bool g_chordWasDown = false;
        bool g_camChordWasDown = false; // Back + LB: the free camera
        bool g_hideShotNotices = false; // this frame only: hideScreenshotNotices
        std::vector<Level> g_stack{{Main}};
        bool g_padWas[platform::GAMEPAD_BUTTON_COUNT] = {};

        // the choices' current values (the menu's own; applied with Enter, or at once where noted)
        int g_wanted = 3;
        int g_moneyIndex = 1;
        int g_loadout = 0;
        int g_hour = 12;
        int g_weather = 0; // index into tr::weathers()
        constexpr int kMoney[] = {1000, 10000, 100000, 1000000};

        // ---- Input ------------------------------------------------------------------------------------------------
        bool pressedKey(int k) { return platform::keyPressed(k) || ps2control::keyPressed(k); }
        bool heldKey(int k) { return platform::keyDown(k) || ps2control::keyDown(k); }
        bool padDown(platform::GamepadButton b) { return platform::gamepadConnected() && platform::gamepadButton(b); }

        // A direction held down repeats, like a text cursor.
        struct Repeat
        {
            double next = 0.0;
            bool fire(bool pressed, bool held)
            {
                const double t = ImGui::GetTime();
                if (pressed)
                {
                    next = t + 0.35;
                    return true;
                }
                if (held && t >= next)
                {
                    next = t + 0.06;
                    return true;
                }
                return false;
            }
        };
        Repeat g_up, g_down, g_left, g_right;

        struct Input
        {
            bool up = false, down = false, left = false, right = false, select = false, back = false;
        };

        Input readInput()
        {
            using namespace platform;
            bool padPressed[GAMEPAD_BUTTON_COUNT] = {};
            for (int b = 0; b < GAMEPAD_BUTTON_COUNT; ++b)
            {
                const bool d = padDown(static_cast<GamepadButton>(b));
                padPressed[b] = d && !g_padWas[b];
                g_padWas[b] = d;
            }
            auto dir = [&](Repeat &r, int key, int numpad, GamepadButton pad) {
                return r.fire(pressedKey(key) || pressedKey(numpad) || padPressed[pad], heldKey(key) || heldKey(numpad) || padDown(pad));
            };
            Input in;
            in.up = dir(g_up, KEY_UP, KEY_KP_8, GAMEPAD_DPAD_UP);
            in.down = dir(g_down, KEY_DOWN, KEY_KP_2, GAMEPAD_DPAD_DOWN);
            in.left = dir(g_left, KEY_LEFT, KEY_KP_4, GAMEPAD_DPAD_LEFT);
            in.right = dir(g_right, KEY_RIGHT, KEY_KP_6, GAMEPAD_DPAD_RIGHT);
            const bool alt = heldKey(KEY_LEFT_ALT) || heldKey(KEY_RIGHT_ALT); // Alt+Enter is full screen
            in.select = (!alt && (pressedKey(KEY_ENTER) || pressedKey(KEY_KP_ENTER))) || pressedKey(KEY_KP_5) ||
                        padPressed[GAMEPAD_SOUTH];
            in.back = pressedKey(KEY_BACKSPACE) || pressedKey(KEY_KP_0) || pressedKey(KEY_ESCAPE) || padPressed[GAMEPAD_EAST];
            return in;
        }

        bool anyInputHeld()
        {
            for (int k = 1; k < platform::KEY_COUNT; ++k)
                if (platform::keyDown(k))
                    return true;
            for (int b = 0; b < platform::GAMEPAD_BUTTON_COUNT; ++b)
                if (padDown(static_cast<platform::GamepadButton>(b)))
                    return true;
            return false;
        }

        // ---- Pages ------------------------------------------------------------------------------------------------
        Row pageRow(const char *label, Page page, const char *help)
        {
            Row r{label, Value::Page, {}, help};
            r.select = [page] { g_stack.push_back({page}); };
            return r;
        }

        Row toggleRow(const char *label, tr::Toggle t, const char *help)
        {
            Row r{label, tr::toggle(t) ? Value::On : Value::Off, {}, help};
            r.select = [t] { tr::setToggle(t, !tr::toggle(t)); };
            r.change = [t](int) { tr::setToggle(t, !tr::toggle(t)); };
            return r;
        }

        Row actionRow(const std::string &label, std::function<void()> fn, const std::string &help, bool enabled = true)
        {
            Row r{label, Value::None, {}, help};
            r.select = std::move(fn);
            r.enabled = enabled;
            return r;
        }

        Row choiceRow(const char *label, std::string value, std::function<void(int)> change, std::function<void()> apply,
                      const char *help)
        {
            Row r{label, Value::Choice, std::move(value), help};
            r.change = std::move(change);
            r.select = std::move(apply);
            return r;
        }

        int wrap(int v, int n) { return ((v % n) + n) % n; }

        std::string money(int v)
        {
            std::string digits = std::to_string(v), out;
            for (size_t k = 0; k < digits.size(); ++k)
            {
                if (k && (digits.size() - k) % 3 == 0)
                    out += ',';
                out += digits[k];
            }
            return "$" + out;
        }

        std::vector<Row> buildPage(Page page)
        {
            const bool inCar = tr::inVehicle();
            std::vector<Row> rows;
            switch (page)
            {
            case Main:
                rows.push_back(pageRow("Player", Player, "God mode, health, wanted level, money."));
                rows.push_back(pageRow("Weapons", Weapons, "Any weapon, loadouts, unlimited ammo."));
                rows.push_back(pageRow("Vehicles", Vehicles, "Spawn any vehicle; repair, flip and boost the one you drive."));
                rows.push_back(pageRow("Teleport", Teleport, "Every district of Vice City, and a position of your own."));
                rows.push_back(pageRow("Camera", Camera, "Free camera: fly anywhere. Hide the HUD."));
                rows.push_back(pageRow("World", World, "Time of day, weather, game speed."));
                rows.push_back(pageRow("Missions", Missions, "Start any story mission or side job."));
                break;
            case Player:
                rows.push_back(toggleRow("God mode", tr::GodMode,
                                         "No damage from bullets, fire, explosions, falls or fists; no drowning; health "
                                         "kept full."));
                rows.push_back(actionRow("Heal", tr::heal, "Health and armour to full."));
                rows.push_back(toggleRow("Never wanted", tr::NeverWanted, "The police forget you as soon as they notice you."));
                rows.push_back(choiceRow("Set wanted level", std::string(g_wanted, '*') + (g_wanted ? "" : "none"),
                                         [](int d) { g_wanted = std::clamp(g_wanted + d, 0, 6); },
                                         [] { tr::setWanted(g_wanted); }, "Left / Right: how many stars. Enter: set it."));
                rows.push_back(choiceRow("Add money", money(kMoney[g_moneyIndex]),
                                         [](int d) { g_moneyIndex = wrap(g_moneyIndex + d, static_cast<int>(std::size(kMoney))); },
                                         [] { tr::addMoney(kMoney[g_moneyIndex]); }, "Left / Right: how much. Enter: add it."));
                rows.push_back(toggleRow("Infinite sprint", tr::InfiniteSprint, "Run as long as you like."));
                rows.push_back(toggleRow("Police ignore you", tr::PoliceIgnore, "Cops leave you alone, whatever you do."));
                rows.push_back(toggleRow("Everyone ignores you", tr::EveryoneIgnores, "Nobody reacts to you: cops, gangs, people."));
                break;
            case Weapons:
            {
                rows.push_back(pageRow("Give weapon", WeaponList, "Any of the game's weapons, with ammunition."));
                const std::vector<tr::Entry> loadouts = tr::loadouts();
                g_loadout = std::clamp(g_loadout, 0, static_cast<int>(loadouts.size()) - 1);
                rows.push_back(choiceRow("Give loadout", loadouts[g_loadout].name,
                                         [n = static_cast<int>(loadouts.size())](int d) { g_loadout = wrap(g_loadout + d, n); },
                                         [] { tr::giveLoadout(g_loadout); },
                                         "Street: bat, molotovs, pistol, shotgun, micro SMG. Military: machete, grenades, "
                                         "Equalizer, Spaz, AK, sniper, rocket launcher. Heavy: chainsaw, grenades, "
                                         "Equalizer, Spaz, SMG, assault rifle, laser sniper, minigun."));
                rows.push_back(toggleRow("Unlimited ammo", tr::UnlimitedAmmo, "Every weapon you carry stays full."));
                rows.push_back(toggleRow("No reload", tr::NoReload, "The clip never runs out: fire without reloading."));
                rows.push_back(toggleRow("Fast reload", tr::FastReload, "The game's own quick reload."));
                rows.push_back(actionRow("Remove all weapons", tr::removeWeapons, "Empty hands."));
                break;
            }
            case WeaponList:
                for (const tr::Entry &e : tr::weapons())
                    rows.push_back(actionRow(e.name, [id = e.id] { tr::giveWeapon(id); }, "Enter: give it, with ammunition, "
                                                                                       "and hold it."));
                break;
            case Vehicles:
            {
                const char *needCar = "Get in a vehicle first.";
                rows.push_back(pageRow("Spawn vehicle", VehicleList, "Every car, bike, boat and aircraft in the game."));
                rows.push_back(toggleRow("Get in spawned vehicles", tr::WarpIntoSpawned,
                                         "On: you are put behind the wheel. Off: it appears ahead of you."));
                rows.push_back(toggleRow("Vehicle invincible", tr::VehicleInvincible,
                                         "The vehicle you drive takes no damage and does not burn."));
                rows.push_back(actionRow("Repair vehicle", tr::repairVehicle,
                                         inCar ? "Engine and health to full; puts a fire out. (The dents stay: a Pay 'n' "
                                                 "Spray fixes those.)" : needCar, inCar));
                rows.push_back(actionRow("Flip upright", tr::flipVehicle, inCar ? "Back on its wheels, facing the same way." : needCar, inCar));
                rows.push_back(actionRow("Boost", tr::boostVehicle, inCar ? "A shove forward." : needCar, inCar));
                rows.push_back(actionRow("Random paint", tr::paintVehicle, inCar ? "New colours." : needCar, inCar));
                break;
            }
            case VehicleList:
            {
                const std::vector<tr::Entry> list = tr::vehicles();
                if (list.empty())
                    rows.push_back(actionRow("(the game's names are not loaded yet)", {}, "", false));
                for (const tr::Entry &e : list)
                    rows.push_back(actionRow(e.name, [id = e.id] { tr::spawnVehicle(id); },
                                             tr::toggle(tr::WarpIntoSpawned) ? "Enter: spawn it and get in." : "Enter: spawn it ahead of you."));
                break;
            }
            case Missions:
                rows.push_back(pageRow("Story missions", StoryList, "Every story mission, in story order."));
                rows.push_back(pageRow("Side jobs", JobList, "Paramedic, Vigilante, Fire Truck, Air Rescue, Taxi."));
                break;
            case StoryList:
            case JobList:
            {
                const std::vector<tr::Entry> list = tr::missions(page == StoryList);
                if (list.empty())
                    rows.push_back(actionRow("(the game's names are not loaded yet)", {}, "", false));
                const bool running = tr::missionRunning();
                const std::string help = running ? "A mission is running: finish or fail it first."
                                                 : "Enter: start it here, as its marker would. Saving after a mission "
                                                   "played out of turn keeps what it changed.";
                for (const tr::Entry &e : list)
                    rows.push_back(actionRow(e.name, [id = e.id] { tr::launchMission(id); }, help, !running));
                break;
            }
            case Teleport:
                rows.push_back(actionRow("Save position", tr::savePosition, "Remember where you are now."));
                rows.push_back(actionRow("Return to saved position", tr::returnToSaved,
                                         tr::hasSavedPosition() ? "Back to the position you saved." : "Save a position first.",
                                         tr::hasSavedPosition()));
                for (const tr::Entry &e : tr::landmarks())
                    rows.push_back(actionRow(e.name, [id = e.id] { tr::teleport(id); }, "Enter: go there (in your vehicle, if you drive one)."));
                break;
            case Camera:
            {
                rows.push_back(toggleRow("Free camera", tr::FreeCamera,
                                         "Fly: W / A / S / D, E up, Q down, the mouse looks, Shift faster, Ctrl slower, "
                                         "Z / X zoom. Gamepad: sticks, RT / LT, A / B, D-pad. The player waits. Hotkey: "
                                         "F6 (Back + LB)."));
                static const int kSpeeds[] = {5, 10, 20, 40, 80};
                int at = 0;
                while (at + 1 < static_cast<int>(std::size(kSpeeds)) && kSpeeds[at] < tr::freeCameraSpeed())
                    ++at;
                char speed[16];
                std::snprintf(speed, sizeof(speed), "%d m/s", kSpeeds[at]);
                rows.push_back(choiceRow("Free camera speed", speed,
                                         [at](int d) {
                                             const int n = static_cast<int>(std::size(kSpeeds));
                                             tr::setFreeCameraSpeed(kSpeeds[std::clamp(at + d, 0, n - 1)]);
                                         },
                                         [] { tr::setFreeCameraSpeed(20); }, "Left / Right: slower or faster. Enter: 20 m/s."));
                rows.push_back(actionRow("Teleport player here", tr::teleportToCamera,
                                         tr::toggle(tr::FreeCamera) ? "Puts you on the ground below the camera and ends it."
                                                                    : "Turn the free camera on first.",
                                         tr::toggle(tr::FreeCamera)));
                rows.push_back(toggleRow("Hide HUD", tr::HideHud, "No radar, money, weapon or stars: for clean pictures."));
                break;
            }
            case World:
            {
                char hour[16];
                std::snprintf(hour, sizeof(hour), "%02d:00", g_hour);
                rows.push_back(choiceRow("Time of day", hour, [](int d) { g_hour = wrap(g_hour + d, 24); tr::setTime(g_hour); },
                                         [] { tr::setTime(g_hour); }, "Left / Right: an hour back or on."));
                rows.push_back(toggleRow("Freeze time", tr::FreezeTime, "The clock stands still at the time it shows now."));
                const std::vector<tr::Entry> weathers = tr::weathers();
                g_weather = std::clamp(g_weather, 0, static_cast<int>(weathers.size()) - 1);
                rows.push_back(choiceRow("Weather", weathers[g_weather].name,
                                         [n = static_cast<int>(weathers.size())](int d) {
                                             g_weather = wrap(g_weather + d, n);
                                             tr::setWeather(tr::weathers()[g_weather].id);
                                         },
                                         [] { tr::setWeather(tr::weathers()[g_weather].id); },
                                         "Left / Right: change it now. Normal lets it change by itself again."));
                char speed[16];
                std::snprintf(speed, sizeof(speed), "%d%%", tr::gameSpeed());
                rows.push_back(choiceRow("Game speed", speed,
                                         [](int d) { tr::setGameSpeed(std::clamp(tr::gameSpeed() + d * 25, 25, 200)); },
                                         [] { tr::setGameSpeed(100); }, "Left / Right: slower or faster. Enter: normal speed."));
                break;
            }
            }
            if (!tr::ready())
                for (Row &r : rows)
                    if (r.kind != Value::Page)
                    {
                        r.enabled = false;
                        r.help = "Available once you are in the game.";
                    }
            return rows;
        }

        void open()
        {
            g_open = true;
            g_hour = tr::clockHour();
        }

        void close()
        {
            g_open = false;
            g_releaseWait = true; // the game would otherwise see the keys that closed the menu held
        }

        // ---- Drawing ----------------------------------------------------------------------------------------------
        ImVec2 textSize(ImFont *font, float size, const char *text, float wrapWidth = 0.0f)
        {
            return font->CalcTextSizeA(size, FLT_MAX, wrapWidth, text);
        }

        void text(ImDrawList *dl, ImFont *font, float size, ImVec2 at, ImU32 col, const std::string &s, bool shadow = false)
        {
            if (shadow)
                dl->AddText(font, size, ImVec2(at.x + size * 0.05f, at.y + size * 0.05f), IM_COL32(0, 0, 0, 180), s.c_str());
            dl->AddText(font, size, at, col, s.c_str());
        }

        void drawNotices(const ImGuiViewport *vp, float s)
        {
            using namespace palette;
            const double now = tr::now();
            std::vector<tr::Notice> list = tr::notices(4.0);
            if (g_hideShotNotices)
                std::erase_if(list, [](const tr::Notice &n) { return n.screenshot; });
            g_hideShotNotices = false;
            if (list.empty())
                return;
            ImDrawList *dl = ImGui::GetForegroundDrawList();
            ImFont *semi = fontSemibold();
            const float size = 15.0f * s, pad = 9.0f * s;
            float y = vp->Pos.y + vp->Size.y - 90.0f * s;
            const size_t first = list.size() > 4 ? list.size() - 4 : 0;
            for (size_t k = list.size(); k-- > first;)
            {
                const float age = static_cast<float>(now - list[k].time);
                const float a = std::clamp((4.0f - age) / 0.5f, 0.0f, 1.0f) * std::clamp(age / 0.15f, 0.0f, 1.0f);
                const ImVec2 ts = textSize(semi, size, list[k].text.c_str());
                const float w = ts.x + pad * 2.0f + 5.0f * s, h = ts.y + pad * 1.4f;
                const ImVec2 p0(vp->Pos.x + vp->Size.x - 24.0f * s - w, y - h);
                const ImVec2 p1(p0.x + w, p0.y + h);
                dl->AddRectFilled(p0, p1, u32(kRowEven, 0.95f * a));
                dl->AddRect(p0, p1, u32(kTealLine, 0.30f * a), 0.0f, 0, 1.0f);
                dl->AddRectFilled(p0, ImVec2(p0.x + 4.5f * s, p1.y), u32(kPinkSoft, a));
                text(dl, semi, size, ImVec2(p0.x + 4.5f * s + pad, p0.y + (h - ts.y) * 0.5f), u32(kTealDark, a), list[k].text);
                y -= h + 6.0f * s;
            }
        }

        void drawPanel(const ImGuiViewport *vp, float s, const std::vector<Row> &rows, Level &level)
        {
            using namespace palette;
            ImDrawList *dl = ImGui::GetForegroundDrawList();
            ImFont *reg = fontRegular();
            ImFont *semi = fontSemibold();
            const float w = 400.0f * s;
            const float x = vp->Pos.x + 36.0f * s;
            const float yStart = vp->Pos.y + 48.0f * s;
            float y = yStart;

            // header: Vice City flamingo pink gradient
            const float headerH = 60.0f * s;
            dl->AddRectFilledMultiColor(ImVec2(x, y), ImVec2(x + w, y + headerH), u32(kPinkSoft, 0.97f), u32(kPink, 0.97f),
                                        u32(kPink, 0.97f), u32(kPinkSoft, 0.97f));
            const std::string title = "VCS TRAINER";
            const float titleSize = 25.0f * s;
            const ImVec2 ts = textSize(semi, titleSize, title.c_str());
            text(dl, semi, titleSize, ImVec2(x + (w - ts.x) * 0.5f, y + (headerH - ts.y) * 0.5f), u32(kOnPink), title);
            y += headerH;

            // the path and the position in the list
            const float subH = 28.0f * s, rowSize = 15.0f * s;
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + subH), u32(kSectionBg, 0.95f));
            std::string path;
            for (const Level &l : g_stack)
                path += (path.empty() ? "" : "  >  ") + std::string(kPageTitles[l.page]);
            if (g_stack.size() > 2) // the last two are enough
                path = std::string(kPageTitles[g_stack[g_stack.size() - 2].page]) + "  >  " + kPageTitles[g_stack.back().page];
            const float subFont = 12.5f * s;
            const ImVec2 pts = textSize(semi, subFont, path.c_str());
            text(dl, semi, subFont, ImVec2(x + 14.0f * s, y + (subH - pts.y) * 0.5f), u32(kTealDark), path);
            char count[32];
            std::snprintf(count, sizeof(count), "%d / %d", rows.empty() ? 0 : level.selected + 1, static_cast<int>(rows.size()));
            const ImVec2 cts = textSize(semi, subFont, count);
            text(dl, semi, subFont, ImVec2(x + w - 14.0f * s - cts.x, y + (subH - cts.y) * 0.5f), u32(kTeal), count);
            dl->AddLine(ImVec2(x, y + subH), ImVec2(x + w, y + subH), u32(kTealLine, 0.25f), 1.0f);
            y += subH;

            // the rows: zebra-striped Vice City rows (12 at most)
            const float rowH = 32.0f * s, padX = 14.0f * s;
            const float hs = 13.5f * s, wrapW = w - 2.0f * padX, ks = 11.5f * s;
            const std::string &help = rows.empty() ? std::string() : rows[level.selected].help;
            const float helpH = help.empty() ? 0.0f : textSize(reg, hs, help.c_str(), wrapW).y + 20.0f * s;
            const float below = 20.0f * s + helpH + ks + 12.0f * s + 12.0f * s;
            const int visible = std::clamp(static_cast<int>((vp->Pos.y + vp->Size.y - y - below) / rowH), 4, 12);
            level.top = std::clamp(level.top, std::max(0, level.selected - visible + 1), std::max(0, level.selected));
            level.top = std::clamp(level.top, 0, std::max(0, static_cast<int>(rows.size()) - visible));
            const int end = std::min(static_cast<int>(rows.size()), level.top + visible);
            for (int i = level.top; i < end; ++i)
            {
                const Row &r = rows[i];
                const bool sel = i == level.selected;
                const ImU32 bg = sel ? u32(kRowHover, 0.97f)
                                     : ((i - level.top) % 2 == 0 ? u32(kRowEven, 0.94f) : u32(kRowOdd, 0.91f));
                dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + rowH), bg);
                if (sel)
                    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + 4.5f * s, y + rowH), u32(kPink));

                ImFont *rf = semi;
                const ImU32 col = !r.enabled ? u32(kTealFaint) : sel ? u32(kTealDark) : u32(kTeal);
                const ImVec2 lts = textSize(rf, rowSize, r.label.c_str());
                const float ty = y + (rowH - lts.y) * 0.5f;
                text(dl, rf, rowSize, ImVec2(x + padX, ty), col, r.label);
                std::string value;
                ImU32 vcol = col;
                switch (r.kind)
                {
                case Value::On: value = "ON", vcol = u32(kStateOn); break;
                case Value::Off: value = "OFF", vcol = u32(kRed); break;
                case Value::Choice: value = sel ? "<  " + r.value + "  >" : r.value, vcol = sel ? u32(kTealDark) : u32(kTeal); break;
                case Value::Page: value = ">", vcol = sel ? u32(kPink) : u32(kTeal); break;
                case Value::None: break;
                }
                if (!value.empty())
                {
                    const ImVec2 vts = textSize(semi, rowSize, value.c_str());
                    text(dl, semi, rowSize, ImVec2(x + w - padX - vts.x, y + (rowH - vts.y) * 0.5f), vcol, value);
                }
                y += rowH;
            }
            // more above / below
            if (level.top > 0 || end < static_cast<int>(rows.size()))
            {
                const float ah = 14.0f * s;
                dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + ah + 6.0f * s), u32(kSectionBg, 0.95f));
                const float cx = x + w * 0.5f, cy = y + 3.0f * s;
                const ImU32 on = u32(kTealDark), off = u32(kTealFaint, 0.55f);
                dl->AddTriangleFilled(ImVec2(cx - 18.0f * s, cy + ah * 0.5f), ImVec2(cx - 10.0f * s, cy),
                                      ImVec2(cx - 2.0f * s, cy + ah * 0.5f), level.top > 0 ? on : off);
                dl->AddTriangleFilled(ImVec2(cx + 2.0f * s, cy + ah * 0.5f - 2.0f * s),
                                      ImVec2(cx + 18.0f * s, cy + ah * 0.5f - 2.0f * s), ImVec2(cx + 10.0f * s, cy + ah),
                                      end < static_cast<int>(rows.size()) ? on : off);
                y += ah + 6.0f * s;
            }

            // the selected row's help, and the keys
            if (!help.empty())
            {
                const float h = helpH - 4.0f * s;
                dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), u32(kRowEven, 0.96f));
                dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + 2.5f * s), u32(kPinkSoft));
                dl->AddText(reg, hs, ImVec2(x + padX, y + 8.0f * s), u32(kTeal), help.c_str(), nullptr, wrapW);
                y += h;
            }
            const std::string keys = "Up/Down: select   Left/Right: change   Enter: apply   Backspace: back";
            const float keysH = ks + 12.0f * s;
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + keysH), u32(kHeaderBg, 0.95f));
            dl->AddLine(ImVec2(x, y), ImVec2(x + w, y), u32(kTealLine, 0.22f), 1.0f);
            const ImVec2 kts = textSize(semi, ks, keys.c_str());
            text(dl, semi, ks, ImVec2(x + padX, y + (keysH - kts.y) * 0.5f), u32(kTealMuted), keys);
            y += keysH;

            // Outer teal hairline
            dl->AddRect(ImVec2(x, yStart), ImVec2(x + w, y), u32(kTealLine, 0.35f), 0.0f, 0, 1.0f);
        }
    }

    bool trainerMenuOpen() { return g_open; }
    bool trainerMenuBlocksInput() { return g_open || g_releaseWait; }
    void hideScreenshotNotices() { g_hideShotNotices = true; }

    void drawTrainerMenu()
    {
        const bool chord = padDown(platform::GAMEPAD_BACK) && padDown(platform::GAMEPAD_RIGHT_SHOULDER);
        const bool chordPressed = chord && !g_chordWasDown;
        g_chordWasDown = chord;
        const int key = settings::value("controls.trainer_key");
        // the free camera's own hotkey (any time, the trainer open or not)
        const bool camChord = padDown(platform::GAMEPAD_BACK) && padDown(platform::GAMEPAD_LEFT_SHOULDER);
        const bool camChordPressed = camChord && !g_camChordWasDown;
        g_camChordWasDown = camChord;
        if (!settingsMenuOpen() && (pressedKey(settings::value("controls.freecam_key")) || camChordPressed))
            tr::setToggle(tr::FreeCamera, !tr::toggle(tr::FreeCamera));
        if (settingsMenuOpen()) // one menu at a time
        {
            if (g_open)
                close();
        }
        else if (pressedKey(key) || chordPressed)
            g_open ? close() : open();

        if (g_releaseWait && !anyInputHeld())
            g_releaseWait = false;

        const ImGuiViewport *vp = ImGui::GetMainViewport();
        const float s = std::clamp((vp->Size.y / 720.0f) * 0.85f, 0.65f, 3.4f);
        drawNotices(vp, s);
        if (!g_open)
            return;

        Level &level = g_stack.back();
        std::vector<Row> rows = buildPage(level.page);
        const int n = static_cast<int>(rows.size());
        level.selected = n ? std::clamp(level.selected, 0, n - 1) : 0;
        const Input in = readInput();
        if (n)
        {
            if (in.up)
                level.selected = wrap(level.selected - 1, n);
            if (in.down)
                level.selected = wrap(level.selected + 1, n);
            Row &r = rows[level.selected]; // (a row's action may push a page: `level` is not used after it)
            if (r.enabled && r.change && (in.left || in.right))
                r.change(in.right ? 1 : -1);
            else if (r.enabled && r.select && in.select)
                r.select();
        }
        if (in.back)
        {
            if (g_stack.size() > 1)
                g_stack.pop_back();
            else
                close();
        }
        if (!g_open)
            return;
        // drawn after the input, so a press shows in the same frame (the page may have changed)
        Level &shown = g_stack.back();
        const std::vector<Row> now = buildPage(shown.page);
        shown.selected = now.empty() ? 0 : std::clamp(shown.selected, 0, static_cast<int>(now.size()) - 1);
        drawPanel(vp, s, now, shown);
    }
}
