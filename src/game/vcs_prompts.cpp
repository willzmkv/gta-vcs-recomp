// See vcs_prompts.h. How the game shows a prompt (docs/game-internals.md 6n):
//
// 1. Text names controls by action: `Press ~k~ ~TGSUB~ to ...`. FUN_003aff20 expands each `~k~` through FUN_003afd78,
//    which copies the text of the GXT key `C<configuration><ACTION>` (configuration: CPad 0 +0x92): a face-button token
//    (`~X~`) or words ("R3 button", "left analog stick").
// 2. The font (FUN_00261be8) draws a `~c~` token it does not know as a colour from the font's button-glyph table (font
//    +2 count, +0xC table of 14-byte entries: left, width, right, height, character, u, v): only style 1 ("Game") has
//    one - X, O, T, S. The string width functions measure a glyph by the same table.
// 3. The pause menu's help bars draw their buttons as pictures (image items, texture btn_cross_psp ...).
//
// What is done here, with Button prompts other than Original:
// - FUN_003afd78 is replaced: the same GXT text, its phrases turned into one token per control - `~<U+E000 + context x
//   0x40 + control>~` - for the context the action belongs to (on foot, in a vehicle; the menu for the help bars and
//   the plain X / O / T / S while the pause menu is up). The tokens say which control, not which device: a help box
//   keeps its text while it is up, and the glyphs still follow the device.
// - The font's glyph table is a copy in guest memory the game does not use (0x61000, the EE kernel's area: DECISIONS.md,
//   free aim's scratch) holding the four originals and an entry per token; each frame kick writes their widths for the
//   glyph set in use, so the game lays the text out (centring, wrapping) for glyphs of that width. A token's entry has
//   its own texture coordinates (u = 2 x index, v = 255: a row no glyph of the font starts in), so a glyph quad in the
//   font's batch is known by them.
// - The glyphs drawn are found in the font's 2D batches (4 vertices a glyph) and the help bars' image items; their
//   vertex positions go to the renderer at the frame kick (as the HUD's anchors do, game/vcs_hud.cpp), which draws
//   those primitives from the glyph atlas (gsHwSetGlyphClassifier). The atlas holds every glyph of every set, drawn by
//   code (game/vcs_glyph_art.h) at the window's size on a helper thread.
// Original: the table goes back to the font's own, the expander runs the game's code: the game exactly as it was.
#include "game/vcs_prompts.h"

#include "game/vcs_frontend.h"
#include "game/vcs_glyph_art.h"
#include "game/vcs_input.h"
#include "platform/platform.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/gs_hw.h"
#include "runtime/ps2_control.h"
#include "runtime/ps2_disc_image.h"
#include "runtime/ps2_render_worker.h"
#include "settings/settings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vcs::prompts
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        constexpr uint32_t kExpandKey = 0x003AFD78u;     // (src after "~k~ ", out): the C<n><ACTION> text into out
        constexpr uint32_t kFontRender = 0x00261BE8u;    // render(font, string, x, y, colour, rect, ?, shadow)
        constexpr uint32_t kIm2DRender = 0x002FBE08u, kIm2DRenderIndexed = 0x002FBE68u;
        constexpr uint32_t kGlyphBatch = 0x003DFE24u;    // Im2D's return address in the font's glyph batch
        constexpr uint32_t kFontStyles = 0x0048A1D8u;    // -> the fonts by style; 1 = "Game"
        constexpr uint32_t kPad0Mode = 0x005147A8u + 0x92u; // CPad 0: the controller configuration (u16)
        constexpr uint32_t kTextTable = 0x0048A2A0u;     // -> the text table: {entries, count}; entry {u32 text, key[8]}
        constexpr uint32_t kImageItem = 0x0048FE10u;     // the menu's image item class (help-bar buttons)
        constexpr uint32_t kTable = 0x00061000u;         // our copy of the font's glyph table (guest scratch)
        constexpr uint32_t kSubFonts = 0x00064000u;      // the subtitles' copy of the font list (subIn): the four fonts, then ours
        constexpr uint16_t kTokenBase = 0xE000u;
        constexpr uint16_t kTokenV = 255;                // the tokens' texture row
        constexpr int kGlyphHeight = 23;                 // the originals' height (menu units)
        // a glyph `w` units wide is w x 4/3 by 23 x 1.647 pixels of the 640x448 picture: a square glyph is 28.4 wide
        constexpr float kUnitsPerAspect = kGlyphHeight * (448.0f / 272.0f) / (640.0f / 480.0f);

        enum Ctl : int
        {
            Cross, Circle, Triangle, Square, L1, R1, L2, R2, L3, R3, Select, Start,
            DUp, DDown, DLeft, DRight, DPad, DPadUD, DPadLR,
            LS, LSUp, LSDown, LSLeft, LSRight, LSUD, LSLR,
            RS, RSUp, RSDown, RSLeft, RSRight, RSUD, RSLR,
            kCtlCount
        };
        enum Ctx : int
        {
            Foot,
            Vehicle,
            Menu,
            kCtxCount
        };
        constexpr const char *kCtlNames[kCtlCount] = {
            "X", "O", "T", "S", "L1", "R1", "L2", "R2", "L3", "R3", "SELECT", "START",
            "DU", "DD", "DL", "DR", "DPAD", "DPADUD", "DPADLR",
            "LS", "LSU", "LSD", "LSL", "LSR", "LSUD", "LSLR",
            "RS", "RSU", "RSD", "RSL", "RSR", "RSUD", "RSLR"};
        // the bindings' ids (vcs_input.cpp kControls) of the button controls, and their PS2 default on a gamepad
        constexpr const char *kButtonIds[DPad] = {"cross", "circle", "triangle", "square", "l1", "r1", "l2", "r2",
                                                  "l3", "r3", "select", "start", "dpad_up", "dpad_down", "dpad_left", "dpad_right"};
        constexpr const char *kPadDefault[DPad] = {"south", "east", "north", "west", "lb", "rb", "lt", "rt",
                                                   "ls", "rs", "back", "start", "dpad_u", "dpad_d", "dpad_l", "dpad_r"};

        enum Set : int
        {
            Original = 0,
            Automatic = 1,
            PlayStation = 2,
            Xbox = 3,
            Keyboard = 4,
            NintendoSwitch = 5, // after Keyboard: the values already in players' vcs.ini keep their meaning
        };

        uint8_t *g_rdram = nullptr;
        bool g_installed = false;
        std::atomic<int> g_setting{Original};
        PS2Runtime::RecompiledFunction g_origExpand = nullptr, g_origFontRender = nullptr, g_origRender = nullptr,
                                       g_origRenderIndexed = nullptr;

        uint32_t rd32(uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, g_rdram + (a & kRamMask), 4);
            return v;
        }
        uint16_t rd16(uint32_t a)
        {
            uint16_t v;
            std::memcpy(&v, g_rdram + (a & kRamMask), 2);
            return v;
        }
        void wr16(uint32_t a, uint16_t v) { std::memcpy(g_rdram + (a & kRamMask), &v, 2); }
        void wr32(uint32_t a, uint32_t v) { std::memcpy(g_rdram + (a & kRamMask), &v, 4); }
        bool inRam(uint32_t a, uint32_t n = 4) { return a >= 0x00100000u && a + n <= 0x02000000u; }

        // ---- glyph specs: what each control looks like in each set ----------------------------------------------------
        int setting(const std::string &key) { return settings::value(key); }

        int boundKey(const char *id, bool preferMouse)
        {
            const int a = setting(std::string("keys.") + id), b = setting(std::string("keys.") + id + "2");
            if (preferMouse && b >= platform::KEY_MOUSE_LEFT)
                return b;
            return a ? a : b;
        }

        std::string keysSpec(std::initializer_list<int> keys)
        {
            std::string s = "keys:";
            bool any = false;
            for (int k : keys)
                if (k > 0)
                {
                    s += (any ? "," : "") + std::to_string(k);
                    any = true;
                }
            return any ? s : s + "0";
        }

        std::string keyboardSpec(int ctx, int ctl)
        {
            using namespace platform;
            if (ctl < DPad)
            {
                if (ctx == Menu && ctl == Cross)
                    return keysSpec({KEY_ENTER});
                if (ctx == Menu && (ctl == Triangle || ctl == Start))
                    return keysSpec({KEY_ESCAPE});
                if (ctx == Vehicle)
                {
                    const char *drive = ctl == Cross ? "drive_accelerate" : ctl == Square ? "drive_brake" : ctl == R1 ? "drive_handbrake" : nullptr;
                    if (drive)
                        if (const int k = boundKey(drive, false))
                            return keysSpec({k});
                }
                // the mouse where it is bound (attack, target): how a PC player knows those
                return keysSpec({boundKey(kButtonIds[ctl], ctl == Circle || ctl == R1)});
            }
            auto k = [](const char *id) { return boundKey(id, false); };
            switch (ctl)
            {
            case DPad: return keysSpec({k("dpad_up"), k("dpad_left"), k("dpad_down"), k("dpad_right")});
            case DPadUD: return keysSpec({k("dpad_up"), k("dpad_down")});
            case DPadLR: return keysSpec({k("dpad_left"), k("dpad_right")});
            default: break;
            }
            if (ctl >= RS)
            {
                if (setting("mouse.camera"))
                    return "mouse:move";
                switch (ctl)
                {
                case RSUp: return keysSpec({k("cam_up")});
                case RSDown: return keysSpec({k("cam_down")});
                case RSLeft: return keysSpec({k("cam_left")});
                case RSRight: return keysSpec({k("cam_right")});
                case RSUD: return keysSpec({k("cam_up"), k("cam_down")});
                case RSLR: return keysSpec({k("cam_left"), k("cam_right")});
                default: return keysSpec({k("cam_up"), k("cam_left"), k("cam_down"), k("cam_right")});
                }
            }
            if (ctx == Vehicle) // the left stick steers (left / right) and leans (up / down: the Driving keys)
                switch (ctl)
                {
                case LSUp: return keysSpec({k("drive_lean_forward")});
                case LSDown: return keysSpec({k("drive_lean_back")});
                case LSUD: return keysSpec({k("drive_lean_forward"), k("drive_lean_back")});
                case LS:
                case LSLR: return keysSpec({k("move_left"), k("move_right")});
                default: break;
                }
            switch (ctl)
            {
            case LSUp: return keysSpec({k("move_up")});
            case LSDown: return keysSpec({k("move_down")});
            case LSLeft: return keysSpec({k("move_left")});
            case LSRight: return keysSpec({k("move_right")});
            case LSUD: return keysSpec({k("move_up"), k("move_down")});
            case LSLR: return keysSpec({k("move_left"), k("move_right")});
            default: return keysSpec({k("move_up"), k("move_left"), k("move_down"), k("move_right")});
            }
        }

        const char *padSourceName(int source)
        {
            switch (source)
            {
            case 0: return "south";
            case 1: return "east";
            case 2: return "west";
            case 3: return "north";
            case 4: return "back";
            case 5: return "start";
            case 6: return "ls";
            case 7: return "rs";
            case 8: return "lb";
            case 9: return "rb";
            case 10: return "dpad_u";
            case 11: return "dpad_d";
            case 12: return "dpad_l";
            case 13: return "dpad_r";
            case 20: return "lt";
            case 21: return "rt";
            default: return nullptr;
            }
        }

        // the control scheme's moves (game/vcs_input.cpp), which take a PS2 button off its bound pad button: "Drive with
        // the triggers" - in a vehicle R2 accelerates (Cross), L2 brakes (Square), the D-pad looks left / right (L2 / R2);
        // "Shoot with the triggers" - on foot R2 fires (Circle), L2 aims (R1), the D-pad steps the weapons (L2 / R2); GTA V's
        // buttons with the triggers driving: L1 / R1 look, the pad's Cross is the handbrake (R1), the D-pad's down starts a
        // sub-mission (R3). Nullptr: the button the player bound.
        const char *schemeButton(int ctx, int ctl)
        {
            const bool v = setting("pad.v_layout") != 0;
            if (ctx == Vehicle && setting("pad.trigger_drive"))
                switch (ctl)
                {
                case Cross: return "rt";
                case Square: return "lt";
                case L2: return v ? "lb" : "dpad_l";
                case R2: return v ? "rb" : "dpad_r";
                case R1: return v ? "south" : nullptr;
                default: break;
                }
            if (ctx == Vehicle && v && ctl == R3)
                return "dpad_d";
            if (ctx == Foot && setting("pad.trigger_shoot"))
                switch (ctl)
                {
                case Circle: return "rt";
                case R1: return "lt";
                case L2: return "dpad_l";
                case R2: return "dpad_r";
                default: break;
                }
            return nullptr;
        }

        std::string padSpec(const char *family, int ctx, int ctl)
        {
            std::string button;
            if (const char *moved = schemeButton(ctx, ctl))
                button = moved;
            else if (ctl < DPad) // the physical button the player bound (Gamepad page)
            {
                const char *bound = padSourceName(setting(std::string("pad.") + kButtonIds[ctl]));
                button = bound ? bound : kPadDefault[ctl];
            }
            else if (ctl <= DPadLR)
                button = ctl == DPad ? "dpad" : ctl == DPadUD ? "dpad_ud" : "dpad_lr";
            else
            {
                bool left = ctl < RS;
                if (setting("pad.swap_sticks"))
                    left = !left;
                const int d = ctl - (ctl < RS ? LS : RS);
                static const char *const suffix[] = {"", "_u", "_d", "_l", "_r", "_ud", "_lr"};
                button = std::string(left ? "lstick" : "rstick") + suffix[d];
            }
            return std::string("pad:") + family + ":" + button;
        }

        std::string specFor(int set, int ctx, int ctl)
        {
            switch (set)
            {
            case PlayStation: return padSpec("ps", ctx, ctl);
            case Xbox: return padSpec("xbox", ctx, ctl);
            case NintendoSwitch: return padSpec("switch", ctx, ctl);
            default: return keyboardSpec(ctx, ctl);
            }
        }

        // the set in use: Automatic follows the device used last; a gamepad shows its maker's buttons (Xbox for others)
        int effectiveSet()
        {
            const int s = g_setting.load(std::memory_order_relaxed);
            if (s != Automatic)
                return s;
            if (input::lastDevice() == input::Device::Keyboard)
                return Keyboard;
            switch (platform::gamepadFamily())
            {
            case platform::GamepadFamily::PlayStation: return PlayStation;
            case platform::GamepadFamily::Nintendo: return NintendoSwitch;
            default: return Xbox;
            }
        }

        // ---- the atlas ----------------------------------------------------------------------------------------------
        struct Cell
        {
            float u0 = 0, v0 = 0, u1 = 0, v1 = 0; // 0..1
            float aspect = 1.0f;                  // width / height in pixels
        };
        struct Atlas
        {
            uint32_t w = 0, h = 0;
            std::shared_ptr<const std::vector<uint32_t>> pixels; // alpha in the PS2's 0..128
            std::unordered_map<std::string, Cell> cells;
            std::string signature;
        };

        std::shared_ptr<Atlas> buildAtlas(const std::vector<std::string> &specs, int cellH, const std::string &signature)
        {
            auto atlas = std::make_shared<Atlas>();
            atlas->signature = signature;
            constexpr int pad = 2;
            struct Placed
            {
                const std::string *spec;
                int x, y, w;
            };
            std::vector<Placed> placed;
            uint32_t width = 512;
            for (const std::string &s : specs) // wide enough for the widest glyph
                width = std::max<uint32_t>(width, static_cast<uint32_t>(std::ceil(glyphart::width(s) * cellH)) + 2 * pad);
            while (width < 2048 && specs.size() * (cellH + pad) * cellH * 1.4f > static_cast<float>(width) * width)
                width *= 2;
            uint32_t pw = 1;
            while (pw < width)
                pw *= 2;
            width = pw;
            int x = pad, y = pad;
            for (const std::string &s : specs)
            {
                const int w = std::max(1, static_cast<int>(std::lround(glyphart::width(s) * cellH)));
                if (x + w + pad > static_cast<int>(width))
                {
                    x = pad;
                    y += cellH + pad;
                }
                placed.push_back({&s, x, y, w});
                x += w + pad;
            }
            uint32_t height = 1;
            while (height < static_cast<uint32_t>(y + cellH + pad))
                height *= 2;
            auto pixels = std::make_shared<std::vector<uint32_t>>(static_cast<size_t>(width) * height, 0u);
            for (const Placed &p : placed)
            {
                glyphart::render(*p.spec, p.w, cellH, pixels->data() + static_cast<size_t>(p.y) * width + p.x, static_cast<int>(width));
                Cell c;
                // half a texel in: bilinear filtering never reaches a neighbour
                c.u0 = (p.x + 0.5f) / width;
                c.u1 = (p.x + p.w - 0.5f) / width;
                c.v0 = (p.y + 0.5f) / height;
                c.v1 = (p.y + cellH - 0.5f) / height;
                c.aspect = static_cast<float>(p.w) / cellH;
                atlas->cells[*p.spec] = c;
            }
            for (uint32_t &px : *pixels) // the PS2's alpha: 128 = opaque
                px = (px & 0x00FFFFFFu) | (((((px >> 24) * 128u) + 127u) / 255u) << 24);
            atlas->w = width;
            atlas->h = height;
            atlas->pixels = pixels;
            return atlas;
        }

        std::mutex g_atlasMutex;
        std::shared_ptr<Atlas> g_readyAtlas; // made (main thread's helper), not yet taken by the game thread
        std::atomic<bool> g_building{false};
        std::string g_requested;             // main thread: the signature last built or being built
        std::shared_ptr<Atlas> g_atlas;      // game thread: the one the renderer has

        // ---- the font's glyph table ---------------------------------------------------------------------------------
        struct Entry
        {
            uint16_t ch = 0, u = 0, v = 0;
            int ctx = Foot, ctl = Cross;
            bool original = false; // one of the font's own (X, O, T, S)
        };
        std::vector<Entry> g_entries;      // game thread: as written to kTable
        std::vector<int16_t> g_origRecords; // the font's own entries (7 halfwords each)
        uint32_t g_font = 0, g_fontTable = 0;
        uint16_t g_fontCount = 0;
        bool g_tableIn = false;
        int g_appliedSet = -1; // the set whose widths and cells are in the table (-1: none)

        struct Resolved
        {
            bool valid = false;
            Cell cell;
        };
        std::vector<Resolved> g_resolved; // per entry, for the set in use (game thread)

        uint16_t tokenChar(int ctx, int ctl) { return static_cast<uint16_t>(kTokenBase + ctx * 0x40 + ctl); }

        uint32_t gameFont()
        {
            // (the font list may be the subtitles' copy in guest scratch, below inRam's floor: the button prompts found no
            // font while the Oswald subtitles were on and kept the PS2's glyphs - prompts_check, 2026-11-18)
            const uint32_t styles = rd32(kFontStyles);
            if (styles != kSubFonts && !inRam(styles, 8))
                return 0;
            const uint32_t font = rd32(styles + 4);
            if (!inRam(font, 0x30) || std::memcmp(g_rdram + font + 0x2C, "Game", 4) != 0)
                return 0;
            return font;
        }

        void tableIn()
        {
            const uint32_t font = gameFont();
            if (!font)
                return;
            if (g_tableIn && font == g_font && rd32(font + 0xC) == kTable)
                return;
            const uint32_t table = rd32(font + 0xC);
            const uint16_t count = rd16(font + 2);
            if (table == kTable || count == 0 || count > 16 || !inRam(table, count * 14u))
                return;
            g_font = font;
            g_fontTable = table;
            g_fontCount = count;
            g_origRecords.assign(count * 7u, 0);
            std::memcpy(g_origRecords.data(), g_rdram + table, count * 14u);
            g_entries.clear();
            for (uint16_t i = 0; i < count; ++i)
            {
                Entry e;
                e.ch = static_cast<uint16_t>(g_origRecords[i * 7 + 4]);
                e.u = static_cast<uint16_t>(g_origRecords[i * 7 + 5]);
                e.v = static_cast<uint16_t>(g_origRecords[i * 7 + 6]);
                e.original = true;
                e.ctl = e.ch == 'X' ? Cross : e.ch == 'O' ? Circle : e.ch == 'T' ? Triangle : Square;
                g_entries.push_back(e);
            }
            int index = 0;
            for (int ctx = 0; ctx < kCtxCount; ++ctx)
                for (int ctl = 0; ctl < kCtlCount; ++ctl, ++index)
                {
                    Entry e;
                    e.ch = tokenChar(ctx, ctl);
                    e.u = static_cast<uint16_t>(2 * index);
                    e.v = kTokenV;
                    e.ctx = ctx;
                    e.ctl = ctl;
                    g_entries.push_back(e);
                }
            for (size_t i = 0; i < g_entries.size(); ++i)
            {
                const Entry &e = g_entries[i];
                const uint32_t a = kTable + static_cast<uint32_t>(i) * 14u;
                if (e.original)
                    std::memcpy(g_rdram + a, &g_origRecords[i * 7], 14);
                else
                {
                    const int16_t rec[7] = {0, 29, 0, kGlyphHeight, static_cast<int16_t>(e.ch), static_cast<int16_t>(e.u),
                                            static_cast<int16_t>(e.v)};
                    std::memcpy(g_rdram + a, rec, 14);
                }
            }
            g_resolved.assign(g_entries.size(), {});
            g_appliedSet = -1; // the widths and cells written again (onFrameKick)
            wr32(font + 0xC, kTable);
            wr16(font + 2, static_cast<uint16_t>(g_entries.size()));
            g_tableIn = true;
            std::fprintf(stderr, "[prompts] the font's glyph table: %u of its own + %zu tokens at 0x%x\n", count,
                         g_entries.size() - count, kTable);
        }

        void tableOut()
        {
            if (!g_tableIn)
                return;
            if (g_font && rd32(g_font + 0xC) == kTable)
            {
                std::memcpy(g_rdram + kTable, g_origRecords.data(), g_origRecords.size() * 2u); // (unused, as it was)
                wr32(g_font + 0xC, g_fontTable);
                wr16(g_font + 2, g_fontCount);
            }
            g_tableIn = false;
            g_resolved.assign(g_resolved.size(), {});
        }

        // ---- the subtitles' font (Enhancements > Subtitles: Oswald) ---------------------------------------------------
        // The subtitles (CHud's message at the bottom: cutscene lines, mission text; FUN_0031CA80) are drawn with the
        // "Game" font, style 1, as the help box. Their letters come out of Oswald instead, through the game's own text
        // code - its layout (centring, wrapping, colours) unchanged: the font list (*0x48A1D8, four fonts) is copied into
        // guest scratch with a fifth, style 4 = a copy of "Game" whose letter table holds Oswald's advances (width),
        // room for the outline either side (left / right: drawn beyond the advance, FUN_00261600) and texture
        // coordinates on a row no letter of the font has (v kSubV, u 2 x the letter); the subtitle function's choice of
        // style 1 (FUN_003F4000 called from inside it) becomes 4, and its letters are drawn from the glyph atlas's
        // "sub:<char>" cells in the text's colour (gsHw glyph hit `tint`). A letter Oswald lacks keeps the font's own.
        constexpr uint32_t kSubFont = 0x00064040u;    // "Game"'s copy (0x50 bytes)
        constexpr uint32_t kSubLetters = 0x00064100u; // its letter table (7 halfwords a letter)
        constexpr uint32_t kSubButtons = 0x00065000u; // its button glyph table: the prompts' (kTable), kSubGrid x
        constexpr uint32_t kFontSetScale = 0x00262570u; // FUN_00262570(font a0, scale f12): +0x20, each text's scale
        // The copy's measures are kSubGrid x the font's, its scale 1 / kSubGrid of the asked: the same size on screen,
        // each letter's advance rounded to a quarter of the font's unit (whole units left gaps: "volt ar")
        constexpr int kSubGrid = 4;
        constexpr uint32_t kSetFontStyle = 0x003F4000u, kFontStyle = 0x0048A1DCu;
        // CHud's two message drawers (functions.csv): FUN_0031CA80 when the message's +0x21C flag is 1, FUN_003219E0
        // when it is 0 - the same text (the HUD's +0x1C), each with a widescreen (cutscene) branch
        constexpr uint32_t kSubtitleFrom = 0x0031CA80u, kSubtitleTo = 0x0031D8A8u;
        constexpr uint32_t kSubtitle2From = 0x003219E0u, kSubtitle2To = 0x00321CF8u;
        // the help box (FUN_00321CF8: SetFontStyle(1) at 0x322048 / 0x322050 before its text) in Oswald too - the player
        // saw the help boxes still in the game's font (2026-11-18)
        constexpr uint32_t kHelpBoxFrom = 0x00321CF8u, kHelpBoxTo = 0x003221A0u;
        // Oswald is taller than the game's letters at the same scale and reaches lower: in the help box it read too big,
        // the last line's descenders hung below the box and long lines touched its right edge (the player, 2026-11-18).
        // Its text there is drawn at kHelpTextScale of the subtitles' size and the box's line height kept at
        // kHelpBoxScale - room below the last line (fontSetScale / fontHeightField while g_inHelpBox: the last style set
        // came from the help box)
        constexpr float kHelpTextScale = 0.74f, kHelpBoxScale = 0.84f;
        bool g_inHelpBox = false; // game thread
        bool inSubtitleFn(uint32_t ra)
        {
            return (ra >= kSubtitleFrom && ra < kSubtitleTo) || (ra >= kSubtitle2From && ra < kSubtitle2To) ||
                   (ra >= kHelpBoxFrom && ra < kHelpBoxTo);
        }
        constexpr uint16_t kSubV = 253;
        std::atomic<int> g_subSetting{0}; // 0 the game's font, 1 Oswald
        bool g_subIn = false, g_subReady = false;
        uint32_t g_subStylesWas = 0, g_subGame = 0;
        std::vector<uint16_t> g_subChars;    // per letter of the copy: its character (Oswald's when g_subOswald)
        std::vector<uint8_t> g_subOswald;
        std::vector<Cell> g_subCells;        // per letter: its cell in the atlas in use
        std::vector<char> g_subCellValid;
        std::shared_ptr<Atlas> g_subAtlas;   // the atlas g_subCells come from

        bool subtitlesOn() { return g_subSetting.load(std::memory_order_relaxed) != 0; }
        void postSubCells(bool on); // (below)

        void subOut()
        {
            if (!g_subIn)
                return;
            if (rd32(kFontStyles) == kSubFonts)
                wr32(kFontStyles, g_subStylesWas);
            if (rd32(kFontStyle) == 4u)
                wr32(kFontStyle, 1u);
            g_subIn = g_subReady = false;
            postSubCells(false);
        }

        void subIn()
        {
            const uint32_t game = gameFont();
            if (!game)
                return;
            if (g_subIn && game == g_subGame && rd32(kFontStyles) == kSubFonts)
            {
                // the button glyphs follow the game's (tableIn may have moved them, the frame kick re-widths them): a copy
                // in the grid's measures
                const uint16_t buttons = std::min<uint16_t>(rd16(game + 2), 0x600 / 14);
                const uint32_t from = rd32(game + 0xC);
                // (kTable, the prompts' copy, is guest scratch below inRam's floor: the copy never happened and a help box
                // in Oswald drew its buttons from empty records - blank gaps, 2026-11-18)
                if (buttons && (from == kTable || inRam(from, buttons * 14u)))
                    for (uint16_t i = 0; i < buttons; ++i)
                    {
                        int16_t rec[7];
                        std::memcpy(rec, g_rdram + from + i * 14u, 14);
                        for (int k = 0; k < 4; ++k)
                            rec[k] = static_cast<int16_t>(rec[k] * kSubGrid);
                        std::memcpy(g_rdram + kSubButtons + i * 14u, rec, 14);
                    }
                wr16(kSubFont + 2, buttons);
                wr32(kSubFont + 0xC, kSubButtons);
                return;
            }
            const uint32_t styles = rd32(kFontStyles);
            const uint16_t count = rd16(game);
            const uint32_t table = rd32(game + 8);
            if (styles == kSubFonts || !inRam(styles, 16) || count == 0 || count > 255 || !inRam(table, count * 14u))
                return;
            for (uint32_t i = 0; i < 4; ++i)
                wr32(kSubFonts + i * 4u, rd32(styles + i * 4u));
            wr32(kSubFonts + 16u, kSubFont);
            std::memcpy(g_rdram + kSubFont, g_rdram + game, 0x50);
            g_subChars.assign(count, 0);
            g_subOswald.assign(count, 0);
            for (uint16_t i = 0; i < count; ++i)
            {
                int16_t rec[7];
                std::memcpy(rec, g_rdram + table + i * 14u, 14);
                const uint16_t ch = static_cast<uint16_t>(rec[4]);
                g_subChars[i] = ch;
                // (the font's 128-159 are symbols of its own, not Latin-1's)
                const int adv = ch < 128 || ch >= 160 ? glyphart::subtitleAdvance(ch, kSubGrid) : -1;
                if (adv >= 0)
                {
                    const int16_t mine[7] = {static_cast<int16_t>(glyphart::kSubtitleMargin * kSubGrid), static_cast<int16_t>(adv),
                                             static_cast<int16_t>(glyphart::kSubtitleMargin * kSubGrid),
                                             static_cast<int16_t>(glyphart::kSubtitleCell * kSubGrid), rec[4], static_cast<int16_t>(2 * i),
                                             static_cast<int16_t>(kSubV)};
                    std::memcpy(g_rdram + kSubLetters + i * 14u, mine, 14);
                    g_subOswald[i] = 1;
                }
                else
                {
                    for (int k = 0; k < 4; ++k)
                        rec[k] = static_cast<int16_t>(rec[k] * kSubGrid);
                    std::memcpy(g_rdram + kSubLetters + i * 14u, rec, 14);
                }
            }
            wr32(kSubFont + 8, kSubLetters);
            wr16(kSubFont + 4, static_cast<uint16_t>(rd16(game + 4) * kSubGrid)); // the line heights
            wr16(kSubFont + 6, static_cast<uint16_t>(rd16(game + 6) * kSubGrid));
            g_subStylesWas = styles;
            g_subGame = game;
            wr32(kFontStyles, kSubFonts);
            g_subIn = true;
            g_subAtlas.reset(); // the cells looked up again
            std::fprintf(stderr, "[subtitles] Oswald: %d of the font's %u letters, style 4 at 0x%x\n",
                         static_cast<int>(std::count(g_subOswald.begin(), g_subOswald.end(), 1)), count, kSubFont);
        }

        // the letters' cells for the renderer (GS stage): a letter is known there by its texture row and column (the
        // letters touch each other - their corners are shared, so positions cannot tell them apart); empty = none
        struct SubCell
        {
            Cell cell;
            bool valid = false;
        };
        std::shared_ptr<const std::vector<SubCell>> g_gsSubCells = std::make_shared<std::vector<SubCell>>(); // GS stage
        void postSubCells(bool on)
        {
            auto cells = std::make_shared<std::vector<SubCell>>();
            if (on)
                for (size_t i = 0; i < g_subCells.size(); ++i)
                    cells->push_back({g_subCells[i], g_subCellValid[i] != 0});
            std::shared_ptr<const std::vector<SubCell>> shared = std::move(cells);
            ps2_render_worker::post([shared] { ps2_render_worker::runOnGsStage([shared] { g_gsSubCells = shared; }); });
        }

        // the letters' cells in the atlas in use: Oswald's letters are drawn from it once every one has its cell
        void subCells()
        {
            if (!g_subIn || !g_atlas || g_subAtlas == g_atlas)
                return;
            g_subAtlas = g_atlas;
            g_subCells.assign(g_subChars.size(), {});
            g_subCellValid.assign(g_subChars.size(), 0);
            bool all = true;
            for (size_t i = 0; i < g_subChars.size(); ++i)
            {
                if (!g_subOswald[i])
                    continue;
                const auto it = g_atlas->cells.find("sub:" + std::to_string(g_subChars[i]));
                if (it == g_atlas->cells.end())
                {
                    all = false;
                    continue;
                }
                g_subCells[i] = it->second;
                g_subCellValid[i] = 1;
            }
            g_subReady = all;
            postSubCells(all);
        }

        // FUN_003F4000(style): the current style - 1 becomes 4 when asked from the subtitle function
        std::atomic<uint32_t> g_subCalls{0}, g_subSwitched{0}, g_subLettersDrawn{0}; // (prompts subtitles)
        PS2Runtime::RecompiledFunction g_origFontSetScale = nullptr;
        // FUN_00262570(font, scale): for the copy, the scale over kSubGrid (its measures are kSubGrid x)
        PS2_GUEST_CC void fontSetScale(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if ((GPR_U32(ctx, 4) & kRamMask) == kSubFont)
                ctx->f[12] *= (g_inHelpBox ? kHelpTextScale : 1.0f) / static_cast<float>(kSubGrid);
            g_origFontSetScale(rdram, ctx, runtime);
        }
        // FUN_003E5D90(font): its line height (+6), a leaf. CFont::GetHeight (FUN_003F4110, returning to 0x3F4128) times
        // the global scale 0x48A1E4 - not the font's own scale, which fontSetScale divides - sizes the help box: the copy's
        // kSubGrid x height made the box four times as tall as its Oswald text. Its other callers (the text's own line
        // spacing goes by the font's scale) keep the copy's measure.
        constexpr uint32_t kFontHeightField = 0x003E5D90u, kGetHeightReturn = 0x003F4128u;
        PS2_GUEST_CC void fontHeightField(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t font = GPR_U32(ctx, 4) & kRamMask;
            uint32_t h = 0;
            std::memcpy(&h, rdram + ((font + 6u) & kRamMask), 2);
            h &= 0xFFFFu;
            if (font == kSubFont && GPR_U32(ctx, 31) == kGetHeightReturn)
                h = static_cast<uint32_t>(std::lround(h * (g_inHelpBox ? kHelpBoxScale : 1.0f) / kSubGrid));
            SET_GPR_U32(ctx, 2, h);
            ctx->pc = GPR_U32(ctx, 31);
        }
        PS2_GUEST_CC void setFontStyle(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            uint32_t style = GPR_U32(ctx, 4);
            const uint32_t ra = GPR_U32(ctx, 31);
            if (inSubtitleFn(ra))
                g_subCalls.fetch_add(1, std::memory_order_relaxed);
            g_inHelpBox = ra >= kHelpBoxFrom && ra < kHelpBoxTo;
            if (style == 1u && g_subReady && subtitlesOn() && inSubtitleFn(ra) && rd32(kFontStyles) == kSubFonts)
            {
                style = 4u;
                g_subSwitched.fetch_add(1, std::memory_order_relaxed);
            }
            (void)rdram;
            wr32(kFontStyle, style);
            ctx->pc = ra;
        }

        // ---- the expander: phrases into tokens ----------------------------------------------------------------------
        struct Phrase
        {
            std::string pattern, replacement; // UTF-8
            bool exact;                       // case matters (the ~X~ tokens; ~t~ is a colour)
            std::u16string text = {};         // the pattern as the game's text has it
        };
        void prepare(std::vector<Phrase> &v)
        {
            for (Phrase &p : v)
                for (size_t i = 0; i < p.pattern.size();) // UTF-8 (Latin-1's letters at most: two bytes)
                {
                    const unsigned char c = static_cast<unsigned char>(p.pattern[i]);
                    if (c >= 0xC0 && i + 1 < p.pattern.size())
                    {
                        p.text.push_back(static_cast<char16_t>(((c & 0x1F) << 6) | (p.pattern[i + 1] & 0x3F)));
                        i += 2;
                    }
                    else
                        p.text.push_back(c), ++i;
                }
            std::stable_sort(v.begin(), v.end(), [](const Phrase &a, const Phrase &b) { return a.text.size() > b.text.size(); });
        }
        // the official French text's control names (the reference when the disc's English text is translated): the
        // same tokens, joined by the English "and" / "or" (fromReference puts the language shown's in)
        const std::vector<Phrase> &frenchPhrases()
        {
            static const std::vector<Phrase> p = [] {
                std::vector<Phrase> v = {
                    {"joystick analogique gauche à gauche et à droite", "{LSLR}", false},
                    {"joystick analogique gauche en haut et en bas", "{LSUD}", false},
                    {"joystick analogique gauche, ou touches directionnelles", "{LS} or {DPAD}", false},
                    {"joystick analogique gauche ou touches directionnelles", "{LS} or {DPAD}", false},
                    {"joystick analogique gauche", "{LS}", false},
                    {"joystick analogique droit", "{RS}", false},
                    {"pad analogique en haut et pad analogique en bas", "{LSU} and {LSD}", false},
                    {"pad analogique à gauche et pad analogique à droite", "{LSL} and {LSR}", false},
                    {"pad analogique en haut", "{LSU}", false},
                    {"pad analogique en bas", "{LSD}", false},
                    {"pad analogique à gauche", "{LSL}", false},
                    {"pad analogique à droite", "{LSR}", false},
                    {"pad analogique", "{LS}", false},
                    {"touches directionnelles haut et bas", "{DPADUD}", false},
                    {"touches directionnelles gauche et droite", "{DPADLR}", false},
                    {"touches directionnelles", "{DPAD}", false},
                    {"touche directionnelle haut", "{DU}", false},
                    {"touche directionnelle bas", "{DD}", false},
                    {"touche directionnelle gauche", "{DL}", false},
                    {"touche directionnelle droite", "{DR}", false},
                    {"touche haut et touche bas", "{DU} and {DD}", false},
                    {"touche gauche et touche droite", "{DL} and {DR}", false},
                    {"touche haut", "{DU}", false},
                    {"touche bas", "{DD}", false},
                    {"touche gauche", "{DL}", false},
                    {"touche droite", "{DR}", false},
                    {"touche L2 et touche R2", "{L2} and {R2}", false},
                    {"touche R2 + touche L2", "{R2} + {L2}", false},
                    {"touche L1", "{L1}", false},
                    {"touche R1", "{R1}", false},
                    {"touche L2", "{L2}", false},
                    {"touche R2", "{R2}", false},
                    {"touche L3", "{L3}", false},
                    {"touche R3", "{R3}", false},
                    {"touche SELECT", "{SELECT}", false},
                    {"touche START", "{START}", false},
                    {"touche L", "{L1}", false}, // the PSP's names, as in the English text
                    {"touche R", "{R1}", false},
                    {"~X~", "{X}", true},
                    {"~O~", "{O}", true},
                    {"~T~", "{T}", true},
                    {"~S~", "{S}", true},
                };
                prepare(v);
                return v;
            }();
            return p;
        }
        const std::vector<Phrase> &phrases()
        {
            static const std::vector<Phrase> p = [] {
                std::vector<Phrase> v = {
                    {"left analog stick, or left and right directional buttons", "{LSLR} or {DPADLR}", false},
                    {"left analog stick up and down, or up and down directional buttons", "{LSUD} or {DPADUD}", false},
                    {"left analog stick, or directional buttons", "{LS} or {DPAD}", false},
                    {"left analog stick left and right", "{LSLR}", false},
                    {"left analog stick up and down", "{LSUD}", false},
                    {"left analog stick up", "{LSU}", false},
                    {"left analog stick down", "{LSD}", false},
                    {"left analog stick", "{LS}", false},
                    {"right analog stick", "{RS}", false},
                    {"analog stick up and analog stick down", "{LSU} and {LSD}", false},
                    {"analog stick left and analog stick right", "{LSL} and {LSR}", false},
                    {"analog stick up", "{LSU}", false},
                    {"analog stick down", "{LSD}", false},
                    {"analog stick left", "{LSL}", false},
                    {"analog stick right", "{LSR}", false},
                    {"analog stick", "{LS}", false},
                    {"up and down directional buttons", "{DPADUD}", false},
                    {"up directional button", "{DU}", false},
                    {"down directional button", "{DD}", false},
                    {"left directional button", "{DL}", false},
                    {"right directional button", "{DR}", false},
                    {"left and right directional buttons", "{DPADLR}", false},
                    {"directional buttons", "{DPAD}", false},
                    {"up button and down button", "{DU} and {DD}", false},
                    {"left button and right button", "{DL} and {DR}", false},
                    {"the down button", "{DD}", false},
                    {"up button", "{DU}", false},
                    {"down button", "{DD}", false},
                    {"left button", "{DL}", false},
                    {"right button", "{DR}", false},
                    {"L1 and R1 buttons", "{L1} and {R1}", false},
                    {"L2 and R2 buttons", "{L2} and {R2}", false},
                    {"L2 button and R2 button", "{L2} and {R2}", false},
                    {"L2 button + R2 button", "{L2} + {R2}", false},
                    {"L1 button", "{L1}", false},
                    {"R1 button", "{R1}", false},
                    {"L2 button", "{L2}", false},
                    {"R2 button", "{R2}", false},
                    {"L3 button", "{L3}", false},
                    {"R3 button", "{R3}", false},
                    {"SELECT button", "{SELECT}", false},
                    {"START button", "{START}", false},
                    // the PSP's names left in the PS2's text (L / R: the PS2's L1 / R1 - game-internals.md 6n)
                    {"L button", "{L1}", false},
                    {"R button", "{R1}", false},
                    {"~X~", "{X}", true},
                    {"~O~", "{O}", true},
                    {"~T~", "{T}", true},
                    {"~S~", "{S}", true},
                };
                prepare(v);
                return v;
            }();
            return p;
        }

        int ctlByName(const std::string &name)
        {
            for (int i = 0; i < kCtlCount; ++i)
                if (name == kCtlNames[i])
                    return i;
            return -1;
        }

        // the action's context: driving (VE*), flying (FL*, HE*), the taxi's hop, the camera view, the fire truck's hose
        int actionContext(const std::string &action)
        {
            if (action.rfind("VE", 0) == 0 || action.rfind("FL", 0) == 0 || action.rfind("HE", 0) == 0 || action == "TAXJU" ||
                action == "CVEIW" || action == "FIREH")
                return Vehicle;
            return Foot;
        }

        std::u16string translate(const std::u16string &text, int ctx, const std::vector<Phrase> &table = phrases())
        {
            std::u16string out;
            auto lower = [](char16_t c) { return c >= u'A' && c <= u'Z' ? static_cast<char16_t>(c + 32) : c; };
            for (size_t i = 0; i < text.size();)
            {
                const Phrase *hit = nullptr;
                // a phrase starts a word ("pickup button" is no "up button")
                const bool wordStart = i == 0 || !((text[i - 1] >= u'a' && text[i - 1] <= u'z') || (text[i - 1] >= u'A' && text[i - 1] <= u'Z'));
                for (const Phrase &p : table)
                {
                    if (i + p.text.size() > text.size() || (!p.exact && !wordStart))
                        continue;
                    bool same = true;
                    for (size_t k = 0; k < p.text.size() && same; ++k)
                    {
                        const char16_t a = text[i + k], b = p.text[k];
                        same = p.exact ? a == b : lower(a) == lower(b);
                    }
                    if (same)
                    {
                        hit = &p;
                        break;
                    }
                }
                if (!hit)
                {
                    out.push_back(text[i++]);
                    continue;
                }
                i += hit->text.size();
                const std::string &r = hit->replacement;
                for (size_t k = 0; k < r.size();)
                {
                    if (r[k] == '{')
                    {
                        const size_t end = r.find('}', k);
                        const int ctl = ctlByName(r.substr(k + 1, end - k - 1));
                        out.push_back(u'~');
                        out.push_back(static_cast<char16_t>(tokenChar(ctx, ctl < 0 ? Cross : ctl)));
                        out.push_back(u'~');
                        k = end + 1;
                    }
                    else
                        out.push_back(static_cast<char16_t>(r[k++]));
                }
            }
            return out;
        }

        // the text of a key in the game's text table (MAIN), "" when it has none
        std::u16string gameText(const std::string &key)
        {
            const uint32_t table = rd32(kTextTable);
            if (!inRam(table, 8))
                return {};
            const uint32_t entries = rd32(table), count = rd32(table + 4);
            if (!inRam(entries) || count == 0 || count > 20000 || !inRam(entries, count * 12u))
                return {};
            char want[8] = {};
            std::memcpy(want, key.data(), std::min<size_t>(key.size(), 8));
            uint32_t lo = 0, hi = count;
            while (lo < hi)
            {
                const uint32_t mid = (lo + hi) / 2;
                const int c = std::strncmp(reinterpret_cast<const char *>(g_rdram + entries + mid * 12u + 4u), want, 8);
                if (c == 0)
                {
                    std::u16string s;
                    for (uint32_t p = rd32(entries + mid * 12u); inRam(p, 2) && s.size() < 255; p += 2)
                    {
                        const uint16_t ch = rd16(p);
                        if (!ch)
                            break;
                        s.push_back(static_cast<char16_t>(ch));
                    }
                    return s;
                }
                if (c < 0)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            return {};
        }

        // ---- the reference: each control text in a language the phrase tables read ------------------------------------
        // A translated game names the same controls in its own words, and not always the same controls (game-internals.md
        // 6n: a Portuguese fan translation written over ENGLISH.GXT has "right stick up" for C0FLUP's d-pad up, R1 for
        // C0VEWE2's square + cross). So the tokens come from the key's text in the disc's own ENGLISH.GXT, or its
        // FRENCH.GXT when that one is translated (the player's disc; nothing of it is in this source), and only the words
        // joining them ("and", "or") from the language shown. Not AMERICAN.GXT: its control texts are other ones (C0FLUP
        // "left analog stick up", where the English and all four translations say the d-pad).
        std::unordered_map<std::string, std::u16string> g_reference; // "C0TGSUB" -> "R3 button"
        const std::vector<Phrase> *g_referencePhrases = nullptr;
        std::once_flag g_referenceOnce;

        bool loadReferenceFrom(const char *file, const std::vector<Phrase> &table, const char16_t *marker)
        {
            g_reference.clear();
            IPS2DiscImage *disc = ps2_disc::image();
            uint32_t lsn = 0, size = 0;
            bool dir = false;
            if (!disc || !disc->findFile(std::string("cdrom0:\\") + file + ";1", lsn, size, dir) || dir || size < 64 || size > (8u << 20))
                return false;
            std::vector<uint8_t> d(size);
            if (!disc->readBytes(static_cast<uint64_t>(lsn) * 2048u, size, d.data()))
                return false;
            auto u32 = [&](size_t o) {
                uint32_t v = 0;
                if (o + 4 <= d.size())
                    std::memcpy(&v, d.data() + o, 4);
                return v;
            };
            // "TABL" size {name[8], offset}...: MAIN first; its table "TKEY" size {u32 text, key[8]}... "TDAT" size data
            if (std::memcmp(d.data(), "TABL", 4) != 0 || std::memcmp(d.data() + 8, "MAIN", 5) != 0)
                return false;
            const size_t keys = u32(16);
            if (keys + 8 > d.size() || std::memcmp(d.data() + keys, "TKEY", 4) != 0)
                return false;
            const size_t keyBytes = u32(keys + 4), data = keys + 8 + keyBytes + 8;
            if (data > d.size() || std::memcmp(d.data() + data - 8, "TDAT", 4) != 0)
                return false;
            for (size_t e = keys + 8; e + 12 <= keys + 8 + keyBytes; e += 12)
            {
                char key[9] = {};
                std::memcpy(key, d.data() + e + 4, 8);
                if (key[0] != 'C' || key[1] < '0' || key[1] > '9')
                    continue;
                std::u16string s;
                for (size_t p = data + u32(e); p + 2 <= d.size() && s.size() < 255; p += 2)
                {
                    const char16_t ch = static_cast<char16_t>(d[p] | (d[p + 1] << 8));
                    if (!ch)
                        break;
                    s.push_back(ch);
                }
                g_reference[key] = s;
            }
            // in the language the table reads: its word for a button in the R3 button's text
            const auto r3 = g_reference.find("C0TGSUB");
            if (r3 == g_reference.end() || r3->second.find(marker) == std::u16string::npos)
            {
                g_reference.clear();
                return false;
            }
            g_referencePhrases = &table;
            std::fprintf(stderr, "[prompts] %zu control names from the disc's %s\n", g_reference.size(), file);
            return true;
        }

        void loadReference()
        {
            if (!loadReferenceFrom("ENGLISH.GXT", phrases(), u"button") && !loadReferenceFrom("FRENCH.GXT", frenchPhrases(), u"touche"))
                std::fprintf(stderr, "[prompts] no English or French text on the disc: control names read in the language shown\n");
        }

        // the language of a control text, by its words: the joining words to use
        struct Joins
        {
            const char16_t *andWord, *orWord;
        };
        Joins joinsOf(const std::u16string &text)
        {
            std::u16string t;
            for (char16_t c : text)
                t.push_back(c >= u'A' && c <= u'Z' ? static_cast<char16_t>(c + 32) : c);
            auto has = [&](const char16_t *w) { return t.find(w) != std::u16string::npos; };
            if (has(u"touche") || has(u"analogique"))
                return {u"et", u"ou"};
            if (has(u"taste") || has(u"analog-"))
                return {u"und", u"oder"};
            if (has(u"tasto") || has(u"tasti") || has(u"levetta") || has(u"analogico"))
                return {u"e", u"o"};
            if (has(u"botón") || has(u"botones") || has(u"joystick") || has(u"pad analógico"))
                return {u"y", u"o"};
            if (has(u"botão") || has(u"botões") || has(u"analógico") || has(u"direcionais"))
                return {u"e", u"ou"};
            return {u"and", u"or"};
        }

        // the American text's tokens with its "and" / "or" in the language shown; empty when words other than those are
        // left (a text this table does not know: the language's own text is read instead)
        std::u16string fromReference(const std::u16string &tokens, const std::u16string &shown)
        {
            const Joins j = joinsOf(shown);
            std::u16string out;
            for (size_t i = 0; i < tokens.size();)
            {
                if (tokens[i] == u'~') // a token: ~<char>~
                {
                    const size_t end = tokens.find(u'~', i + 1);
                    if (end == std::u16string::npos)
                        return {};
                    out.append(tokens, i, end + 1 - i);
                    i = end + 1;
                    continue;
                }
                size_t w = i;
                while (w < tokens.size() && ((tokens[w] >= u'a' && tokens[w] <= u'z') || (tokens[w] >= u'A' && tokens[w] <= u'Z')))
                    ++w;
                if (w == i)
                {
                    out.push_back(tokens[i++]);
                    continue;
                }
                const std::u16string word = tokens.substr(i, w - i);
                if (word == u"and")
                    out += j.andWord;
                else if (word == u"or")
                    out += j.orWord;
                else
                    return {};
                i = w;
            }
            return out;
        }

        // the control text of C<mode><ACTION> with its controls as tokens: from the American text when the disc has it
        std::u16string controlText(uint16_t mode, const std::string &action, const std::u16string &shown)
        {
            std::call_once(g_referenceOnce, loadReference);
            const auto ref = g_reference.find("C" + std::to_string(mode) + action);
            std::u16string t;
            if (ref != g_reference.end())
                t = fromReference(translate(ref->second, actionContext(action), *g_referencePhrases), shown);
            return t.empty() ? translate(shown, actionContext(action)) : t;
        }

        std::atomic<uint32_t> g_expanded{0};

        // FUN_003afd78(src, out): src points past "~k~ " at "~ACTION~"; the text of C<configuration><ACTION> goes into
        // out; returns 1 + the action's length (the caller skips the action and its closing '~'). Replaced while a set
        // other than Original is chosen; the game's own code otherwise, and for anything it would not find.
        PS2_GUEST_CC void expandKey(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_setting.load(std::memory_order_relaxed) == Original || !g_tableIn)
                return g_origExpand(rdram, ctx, runtime);
            uint32_t p = GPR_U32(ctx, 4) & kRamMask;
            const uint32_t out = GPR_U32(ctx, 5) & kRamMask;
            for (int n = 0; n < 64 && inRam(p, 2) && rd16(p) != '~'; ++n)
                p += 2;
            std::string action;
            if (inRam(p, 2) && rd16(p) == '~')
                for (p += 2; inRam(p, 2) && rd16(p) != '~' && action.size() < 16; p += 2)
                    action.push_back(static_cast<char>(rd16(p)));
            const uint16_t mode = rd16(kPad0Mode);
            const std::u16string text = mode <= 9 && !action.empty() && inRam(out, 512) ? gameText("C" + std::to_string(mode) + action)
                                                                                         : std::u16string();
            if (text.empty())
                return g_origExpand(rdram, ctx, runtime);
            const std::u16string t = controlText(mode, action, text);
            const size_t n = std::min<size_t>(t.size(), 255);
            for (size_t i = 0; i < n; ++i)
                wr16(out + static_cast<uint32_t>(i) * 2u, static_cast<uint16_t>(t[i]));
            wr16(out + static_cast<uint32_t>(n) * 2u, 0);
            g_expanded.fetch_add(1, std::memory_order_relaxed);
            SET_GPR_U32(ctx, 2, static_cast<uint32_t>(1 + action.size()));
            ctx->pc = GPR_U32(ctx, 31);
        }

        // ---- the glyphs drawn this frame ----------------------------------------------------------------------------
        struct Drawn
        {
            int16_t x[4], y[4];
            uint8_t corner[4]; // bit 0 right, bit 1 bottom
            Cell cell;
            float dxLeft = 0.0f; // the left edge moved (pixels): a glyph wider than the game's box
            bool shadow = false;
            bool tint = false; // a subtitle letter: the text's own colour
        };
        std::vector<Drawn> g_drawn; // game thread
        bool g_inGameFont = false, g_shadow = false;
        std::atomic<uint32_t> g_lastFrameGlyphs{0}, g_lastFrameButtons{0};
        uint32_t g_frameButtons = 0;

        // corners from the vertices' box
        bool corners(Drawn &d, int n)
        {
            int x0 = d.x[0], x1 = d.x[0], y0 = d.y[0], y1 = d.y[0];
            for (int i = 1; i < n; ++i)
            {
                x0 = std::min<int>(x0, d.x[i]);
                x1 = std::max<int>(x1, d.x[i]);
                y0 = std::min<int>(y0, d.y[i]);
                y1 = std::max<int>(y1, d.y[i]);
            }
            if (x1 <= x0 || y1 <= y0)
                return false;
            for (int i = 0; i < n; ++i)
                d.corner[i] = static_cast<uint8_t>((d.x[i] * 2 > x0 + x1 ? 1 : 0) | (d.y[i] * 2 > y0 + y1 ? 2 : 0));
            return true;
        }

        bool g_inSubFont = false; // fontRender: drawing with the subtitles' copy (style 4)

        void captureFontBatch(uint8_t *rdram, R5900Context *ctx)
        {
            const uint32_t verts = GPR_U32(ctx, 5) & kRamMask, n = GPR_U32(ctx, 6);
            if (n < 4 || n > 4096 || verts + n * 16u > 0x02000000u)
                return;
            for (uint32_t q = 0; q + 4 <= n; q += 4)
            {
                // a glyph's four vertices: u, v (1.0 = 32768) | RGBA | x, y (640x448) | z
                uint16_t uMin = 0xFFFF, vMin = 0xFFFF;
                Drawn d;
                for (int i = 0; i < 4; ++i)
                {
                    const uint8_t *v = rdram + verts + (q + i) * 16u;
                    uint16_t u16, v16;
                    std::memcpy(&u16, v, 2);
                    std::memcpy(&v16, v + 2, 2);
                    std::memcpy(&d.x[i], v + 8, 2);
                    std::memcpy(&d.y[i], v + 10, 2);
                    uMin = std::min(uMin, u16);
                    vMin = std::min(vMin, v16);
                }
                // texels of the 256-texel atlas: u starts half a texel in, v does not (FUN_003b8ea0)
                const float u = uMin / 128.0f - 0.5f, v = vMin / 128.0f;
                int index = -1;
                if (std::fabs(v - kTokenV) < 0.75f)
                {
                    const int i = static_cast<int>(std::lround(u / 2.0f)) + g_fontCount;
                    if (i >= g_fontCount && i < static_cast<int>(g_entries.size()) && std::fabs(u - g_entries[i].u) < 0.75f)
                        index = i;
                }
                else
                    for (int i = 0; i < g_fontCount && i < static_cast<int>(g_entries.size()); ++i)
                        if (std::fabs(u - g_entries[i].u) < 0.75f && std::fabs(v - g_entries[i].v) < 0.75f)
                            index = i;
                if (index < 0 || !g_resolved[index].valid || !corners(d, 4))
                    continue;
                d.cell = g_resolved[index].cell;
                d.shadow = g_shadow;
                g_drawn.push_back(d);
            }
        }

        // the help bars' buttons: image items drawing a btn_* texture
        int helpBarControl(const char *name)
        {
            static const std::pair<const char *, int> map[] = {
                {"btn_cross", Cross}, {"btn_circle", Circle}, {"btn_triangle", Triangle}, {"btn_square", Square},
                {"btn_up", DUp}, {"btn_down", DDown}, {"btn_left", DLeft}, {"btn_right", DRight},
                {"btn_l", L1}, {"btn_r", R1}, {"btn_start", Start}, {"btn_select", Select}};
            for (const auto &[prefix, ctl] : map)
            {
                const size_t len = std::strlen(prefix);
                if (std::strncmp(name, prefix, len) == 0 && (name[len] == '_' || name[len] == 0))
                    return ctl;
            }
            return -1;
        }

        void captureHelpButton(uint8_t *rdram, R5900Context *ctx)
        {
            const uint32_t item = frontend::drawingItem(GPR_U32(ctx, 29));
            if (!item || !inRam(item, 0xF0) || rd32(item + 0x28) != kImageItem)
                return;
            const uint32_t tex = rd32(item + 0xDC);
            if (!inRam(tex, 0x30))
                return;
            char name[33] = {};
            std::memcpy(name, rdram + tex + 0x10, 32);
            const int ctl = helpBarControl(name);
            if (ctl < 0)
                return;
            const int index = g_fontCount + Menu * kCtlCount + ctl;
            if (index >= static_cast<int>(g_resolved.size()) || !g_resolved[index].valid)
                return;
            const uint32_t verts = GPR_U32(ctx, 5) & kRamMask, n = GPR_U32(ctx, 6);
            if (n < 3 || n > 4 || verts + n * 16u > 0x02000000u)
                return;
            Drawn d;
            for (uint32_t i = 0; i < n; ++i)
            {
                std::memcpy(&d.x[i], rdram + verts + i * 16u + 8u, 2);
                std::memcpy(&d.y[i], rdram + verts + i * 16u + 10u, 2);
            }
            if (!corners(d, static_cast<int>(n)))
                return;
            int x0 = d.x[0], x1 = d.x[0], y0 = d.y[0], y1 = d.y[0];
            for (uint32_t i = 1; i < n; ++i)
            {
                x0 = std::min<int>(x0, d.x[i]);
                x1 = std::max<int>(x1, d.x[i]);
                y0 = std::min<int>(y0, d.y[i]);
                y1 = std::max<int>(y1, d.y[i]);
            }
            d.cell = g_resolved[index].cell;
            const float want = (y1 - y0) * d.cell.aspect; // the glyph's own shape at the box's height
            if (want > (x1 - x0) * 1.05f)
                d.dxLeft = -(want - (x1 - x0));
            // (a quad drawn as three vertices has a fourth corner that never comes: the classifier wants all present)
            for (uint32_t i = n; i < 4; ++i)
            {
                d.x[i] = d.x[0];
                d.y[i] = d.y[0];
                d.corner[i] = d.corner[0];
            }
            g_drawn.push_back(d);
            ++g_frameButtons;
        }

        bool active() { return g_tableIn && g_setting.load(std::memory_order_relaxed) != Original && g_atlas; }

        PS2_GUEST_CC void fontRender(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_inGameFont = g_tableIn && g_font && (GPR_U32(ctx, 4) & kRamMask) == g_font;
            g_inSubFont = g_subIn && (GPR_U32(ctx, 4) & kRamMask) == kSubFont;
            g_shadow = GPR_U32(ctx, 11) != 0;
            g_origFontRender(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void im2dRender(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (active())
                captureHelpButton(rdram, ctx);
            g_origRender(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void im2dRenderIndexed(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_inSubFont && GPR_U32(ctx, 31) == kGlyphBatch)
            {
                // the subtitles' letters are known by their texture coordinates in classify; the button glyphs among them
                // (a help box in Oswald) by the prompts' token rows, as in the game's font
                if (active())
                    captureFontBatch(rdram, ctx);
            }
            else if (active())
            {
                if (GPR_U32(ctx, 31) == kGlyphBatch)
                {
                    if (g_inGameFont)
                        captureFontBatch(rdram, ctx);
                }
                else
                    captureHelpButton(rdram, ctx);
            }
            g_origRenderIndexed(rdram, ctx, runtime);
        }

        // ---- the renderer's side (the GS stage) ---------------------------------------------------------------------
        struct GlyphFrame
        {
            std::unordered_map<uint32_t, uint32_t> keys; // vertex position -> glyph << 2 | corner (~0: two glyphs)
            std::vector<Drawn> glyphs;
        };
        std::shared_ptr<const GlyphFrame> g_gsFrame = std::make_shared<GlyphFrame>(); // GS stage
        uint32_t positionKey(int x, int y) { return uint32_t(uint16_t(x)) | (uint32_t(uint16_t(y)) << 16); }

        bool classify(const float *xy, const float *uv, uint32_t count, GSHwGlyphHit &hit)
        {
            // a subtitle letter: on row kSubV, column 2 x the letter (subIn), its corners by where its texels lie
            if (const std::vector<SubCell> &sub = *g_gsSubCells; !sub.empty() && count >= 3)
            {
                float u0 = 1e9f, v0 = 1e9f;
                for (uint32_t i = 0; i < count; ++i)
                {
                    u0 = std::min(u0, uv[2 * i]);
                    v0 = std::min(v0, uv[2 * i + 1]);
                }
                if (std::fabs(v0 - kSubV) < 0.75f)
                {
                    const int letter = static_cast<int>(std::lround((u0 - 0.5f) / 2.0f));
                    if (letter < 0 || letter >= static_cast<int>(sub.size()) || !sub[letter].valid)
                        return false;
                    const Cell &c = sub[letter].cell;
                    for (uint32_t i = 0; i < count; ++i)
                    {
                        hit.u[i] = uv[2 * i] > u0 + 0.75f ? c.u1 : c.u0;
                        hit.v[i] = uv[2 * i + 1] > v0 + 0.75f ? c.v1 : c.v0;
                        hit.dx[i] = 0.0f;
                    }
                    hit.shadow = false;
                    hit.tint = true;
                    g_subLettersDrawn.fetch_add(1, std::memory_order_relaxed);
                    return true;
                }
            }
            const GlyphFrame &f = *g_gsFrame;
            if (f.glyphs.empty())
                return false;
            uint32_t glyph = ~0u;
            for (uint32_t i = 0; i < count; ++i)
            {
                // the GS sees the 2D's whole pixels half a pixel up-left, or within a sixteenth (vcs_hud.cpp classify)
                const float x = xy[2 * i], y = xy[2 * i + 1];
                const int xs[2] = {static_cast<int>(std::lround(x)), static_cast<int>(std::lround(x + 0.5f))};
                const int ys[2] = {static_cast<int>(std::lround(y)), static_cast<int>(std::lround(y + 0.5f))};
                auto it = f.keys.end();
                for (int k = 0; k < 4 && it == f.keys.end(); ++k)
                    it = f.keys.find(positionKey(xs[k & 1], ys[k >> 1]));
                if (it == f.keys.end() || it->second == ~0u)
                    return false;
                const uint32_t g = it->second >> 2;
                if (glyph != ~0u && g != glyph)
                    return false;
                glyph = g;
                const Drawn &d = f.glyphs[g];
                const uint32_t corner = it->second & 3u;
                hit.u[i] = corner & 1u ? d.cell.u1 : d.cell.u0;
                hit.v[i] = corner & 2u ? d.cell.v1 : d.cell.v0;
                hit.dx[i] = corner & 1u ? 0.0f : d.dxLeft;
                hit.shadow = d.shadow;
                hit.tint = d.tint;
            }
            return glyph != ~0u;
        }

        // ---- the set's widths and cells (game thread, frame kick) -------------------------------------------------------
        std::shared_ptr<Atlas> g_appliedAtlas;
        bool g_appliedMenu = false;

        void resolve(int set, bool menuOpen)
        {
            for (size_t i = 0; i < g_entries.size(); ++i)
            {
                const Entry &e = g_entries[i];
                const int ctx = e.original ? (menuOpen ? Menu : Foot) : e.ctx;
                const auto it = g_atlas->cells.find(specFor(set, ctx, e.ctl));
                if (it == g_atlas->cells.end())
                {
                    g_resolved[i].valid = false;
                    continue;
                }
                g_resolved[i] = {true, it->second};
                const int width = std::clamp(static_cast<int>(std::lround(it->second.aspect * kUnitsPerAspect)), 8, 400);
                wr16(kTable + static_cast<uint32_t>(i) * 14u + 2u, static_cast<uint16_t>(width));
            }
        }

        std::atomic<int> g_shownSet{Original};

        // ---- the main thread: the atlas -----------------------------------------------------------------------------
        int g_hostFrames = 0;
        std::string g_lastSignatureLogged;

        std::vector<std::string> allSpecs()
        {
            std::vector<std::string> specs;
            if (g_setting.load(std::memory_order_relaxed) != Original)
                for (int set : {PlayStation, Xbox, NintendoSwitch, Keyboard})
                    for (int ctx = 0; ctx < kCtxCount; ++ctx)
                        for (int ctl = 0; ctl < kCtlCount; ++ctl)
                            specs.push_back(specFor(set, ctx, ctl));
            if (subtitlesOn()) // the subtitles' letters (Latin-1 less the font's own symbols 128-159)
                for (int ch = 32; ch < 256; ++ch)
                    if ((ch < 128 || ch >= 160) && glyphart::subtitleAdvance(static_cast<uint16_t>(ch)) >= 0)
                        specs.push_back("sub:" + std::to_string(ch));
            std::sort(specs.begin(), specs.end());
            specs.erase(std::unique(specs.begin(), specs.end()), specs.end());
            return specs;
        }

        int cellHeight()
        {
            int w = 0, h = 0;
            platform::windowSize(w, h);
            if (w <= 0 || h <= 0)
                w = 640, h = 448;
            // the 4:3 picture's height in the window, the glyph's share of it (23 menu units of 272)
            const float picture = std::min<float>(static_cast<float>(h), w * 0.75f);
            return std::clamp(static_cast<int>(std::lround(picture * kGlyphHeight / 272.0f)), 24, 128);
        }
    }

    void registerSettings()
    {
        settings::Spec s;
        s.key = "enhance.prompts";
        s.page = "Enhancements";
        s.section = "Button prompts";
        s.label = "Button prompts";
        s.help = "The buttons the game's help text and pause menu show. Automatic follows what you last played with: the "
                 "keys and mouse buttons you bound, or your gamepad's own buttons (Xbox, PlayStation or Nintendo Switch). Original: the "
                 "PS2's text as it is.";
        s.type = settings::Type::Choice;
        s.choices = {{"Original (PS2)", Original},
                     {"Automatic", Automatic},
                     {"PlayStation", PlayStation},
                     {"Xbox", Xbox},
                     {"Nintendo Switch", NintendoSwitch},
                     {"Keyboard & mouse", Keyboard}};
        s.defaultValue = Original;
        s.env = "VCS_PROMPTS";
        s.onChange = [](int v) { g_setting.store(v, std::memory_order_relaxed); };
        settings::add(s);

        s = {};
        s.key = "enhance.prompt_style";
        s.page = "Enhancements";
        s.section = "Button prompts";
        s.label = "Icon style";
        s.help = "How the buttons look in the help text and the pause menu. Kenney: Kenney's Input Prompts, clean icons for "
                 "every controller and key. Classic: the buttons drawn in the game's own style.";
        s.type = settings::Type::Choice;
        s.choices = {{"Kenney", 0}, {"Classic", 1}};
        s.defaultValue = 0;
        s.env = "VCS_PROMPT_STYLE";
        s.onChange = [](int v) { glyphart::setStyle(v); };
        settings::add(s);

        s = {};
        s.key = "enhance.subtitle_font";
        s.page = "Enhancements";
        s.section = "Subtitles";
        s.label = "Subtitle font";
        s.help = "The lines at the bottom of the screen - what people say in cutscenes, the mission's instructions - and "
                 "the help boxes. Original: the game's own letters. Oswald: a clean, tall font, sharp at any resolution, with an outline "
                 "that keeps it readable over anything. The game's layout and colours stay.";
        s.type = settings::Type::Choice;
        s.choices = {{"Original", 0}, {"Oswald", 1}};
        s.defaultValue = 0;
        s.env = "VCS_SUBTITLE_FONT";
        s.onChange = [](int v) { g_subSetting.store(v, std::memory_order_relaxed); };
        settings::add(s);

        s = {};
        s.key = "enhance.subtitle_outline";
        s.page = "Enhancements";
        s.section = "Subtitles";
        s.label = "Subtitle outline";
        s.help = "How thick the dark edge round the subtitles' letters is (with the Oswald font). 0: none.";
        s.type = settings::Type::Integer;
        s.min = 0;
        s.max = 100;
        s.format = "%d%%";
        s.defaultValue = 50;
        s.env = "VCS_SUBTITLE_OUTLINE";
        s.onChange = [](int v) { glyphart::setSubtitleOutline(v); };
        settings::add(s);
    }

    void onHostFrame()
    {
        if (!g_installed || (g_setting.load(std::memory_order_relaxed) == Original && !subtitlesOn()) || (g_hostFrames++ % 20) != 0 ||
            g_building.load())
            return;
        const int cellH = cellHeight();
        std::vector<std::string> specs = allSpecs();
        std::string signature = std::to_string(cellH) + "/" + std::to_string(glyphart::style()) + "/" +
                                std::to_string(glyphart::subtitleOutline()); // (a new style or outline: a new atlas)
        for (const std::string &s : specs)
            signature += "|" + s;
        if (signature == g_requested)
            return;
        g_requested = signature;
        g_building.store(true);
        std::thread([specs = std::move(specs), cellH, signature = std::move(signature)] {
            const auto start = std::chrono::steady_clock::now();
            auto atlas = buildAtlas(specs, cellH, signature);
            std::fprintf(stderr, "[prompts] glyph atlas: %zu glyphs, %u x %u, glyphs %d px high (%.0f ms)\n", specs.size(),
                         atlas->w, atlas->h, cellH,
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            // VCS_PROMPTS_DUMP=<file.png>: the atlas over grey, to look at the glyphs
            if (const char *dump = std::getenv("VCS_PROMPTS_DUMP"); dump && *dump)
            {
                std::vector<uint8_t> rgb(static_cast<size_t>(atlas->w) * atlas->h * 3);
                for (size_t i = 0; i < static_cast<size_t>(atlas->w) * atlas->h; ++i)
                {
                    const uint32_t p = (*atlas->pixels)[i];
                    const float a = std::min(1.0f, (p >> 24) / 128.0f);
                    for (int k = 0; k < 3; ++k)
                        rgb[i * 3 + k] = static_cast<uint8_t>(((p >> (8 * k)) & 0xFF) * a + 96.0f * (1.0f - a) + 0.5f);
                }
                platform::writePng(dump, rgb.data(), static_cast<int>(atlas->w), static_cast<int>(atlas->h));
            }
            {
                std::lock_guard<std::mutex> lock(g_atlasMutex);
                g_readyAtlas = std::move(atlas);
            }
            g_building.store(false);
        }).detach();
    }

    void onFrameKick(uint8_t *rdram)
    {
        if (!g_installed)
            return;
        g_rdram = rdram;
        g_lastFrameGlyphs.store(static_cast<uint32_t>(g_drawn.size()), std::memory_order_relaxed);
        g_lastFrameButtons.store(g_frameButtons, std::memory_order_relaxed);
        g_frameButtons = 0;

        // this frame's glyphs to the renderer, in order with its display list (as the HUD's anchors)
        static bool posted = false;
        if (!g_drawn.empty() || posted)
        {
            auto frame = std::make_shared<GlyphFrame>();
            frame->glyphs = std::move(g_drawn);
            for (size_t g = 0; g < frame->glyphs.size(); ++g)
                for (int i = 0; i < 4; ++i)
                {
                    const uint32_t key = positionKey(frame->glyphs[g].x[i], frame->glyphs[g].y[i]);
                    const uint32_t value = static_cast<uint32_t>(g << 2) | frame->glyphs[g].corner[i];
                    auto [it, added] = frame->keys.emplace(key, value);
                    if (!added && (it->second >> 2) != g)
                        it->second = ~0u; // two glyphs share the point: neither is known by it
                }
            posted = !frame->glyphs.empty();
            std::shared_ptr<const GlyphFrame> shared = std::move(frame);
            ps2_render_worker::post([shared] { ps2_render_worker::runOnGsStage([shared] { g_gsFrame = shared; }); });
        }
        g_drawn.clear();

        // a new atlas: to the renderer before the frames that use its cells
        {
            std::shared_ptr<Atlas> ready;
            {
                std::lock_guard<std::mutex> lock(g_atlasMutex);
                ready = std::move(g_readyAtlas);
            }
            if (ready)
            {
                g_atlas = ready;
                const uint32_t w = ready->w, h = ready->h;
                auto pixels = ready->pixels;
                ps2_render_worker::post([w, h, pixels] { ps2_render_worker::runOnGsStage([w, h, pixels] { gsHwSetGlyphAtlas(w, h, pixels); }); });
            }
        }
        if (subtitlesOn())
        {
            subIn();
            subCells();
        }
        else
            subOut();

        const int choice = g_setting.load(std::memory_order_relaxed);
        if (choice == Original)
        {
            if (g_tableIn)
            {
                tableOut();
                std::fprintf(stderr, "[prompts] Original: the game's own glyphs and text\n");
            }
            g_appliedSet = -1;
            g_shownSet.store(Original, std::memory_order_relaxed);
            return;
        }
        if (!g_atlas)
            return; // the first atlas is being made
        tableIn();
        if (!g_tableIn)
            return;
        const int set = effectiveSet();
        const bool menuOpen = frontend::open();
        if (set != g_appliedSet || g_atlas != g_appliedAtlas || menuOpen != g_appliedMenu)
        {
            resolve(set, menuOpen);
            if (set != g_appliedSet)
                std::fprintf(stderr, "[prompts] %s\n", set == PlayStation ? "PlayStation buttons" : set == Xbox ? "Xbox buttons" : set == NintendoSwitch ? "Nintendo Switch buttons" : "keyboard and mouse");
            g_appliedSet = set;
            g_appliedAtlas = g_atlas;
            g_appliedMenu = menuOpen;
            g_shownSet.store(set, std::memory_order_relaxed);
        }
    }

    void install(PS2Runtime &runtime)
    {
        g_rdram = runtime.memory().getRDRAM();
        const char *gs = std::getenv("PS2X_GS");
        const char *off = std::getenv("VCS_PROMPTS_HOOKS");
        if ((off && off[0] == '0') || !(gs && std::strcmp(gs, "hw") == 0))
        {
            std::fprintf(stderr, "[prompts] button prompts off (%s)\n", off && off[0] == '0' ? "VCS_PROMPTS_HOOKS=0" : "not the hardware renderer");
            return;
        }
        g_origExpand = runtime.lookupFunction(kExpandKey);
        g_origFontRender = runtime.lookupFunction(kFontRender); // (the HUD's hook: chained)
        g_origRender = runtime.lookupFunction(kIm2DRender);     // (the frontend's and the HUD's)
        g_origRenderIndexed = runtime.lookupFunction(kIm2DRenderIndexed);
        if (!g_origExpand || !g_origFontRender || !g_origRender || !g_origRenderIndexed)
        {
            std::fprintf(stderr, "[prompts] the game's text routines not found: button prompts off\n");
            return;
        }
        runtime.replaceFunction(kExpandKey, &expandKey);
        runtime.replaceFunction(kFontRender, &fontRender);
        runtime.replaceFunction(kIm2DRender, &im2dRender);
        runtime.replaceFunction(kIm2DRenderIndexed, &im2dRenderIndexed);
        runtime.replaceFunction(kSetFontStyle, &setFontStyle); // (a leaf: the style stored, here too)
        runtime.replaceFunction(kFontHeightField, &fontHeightField); // (a leaf too)
        if ((g_origFontSetScale = runtime.lookupFunction(kFontSetScale)))
            runtime.replaceFunction(kFontSetScale, &fontSetScale);
        gsHwSetGlyphClassifier(&classify);
        g_installed = true;

        // prompts -> ok setting= shown= device= pad= expanded= glyphs= buttons= drawn= atlas=WxH
        // prompts expand <ACTION> -> ok <its text for the configuration, tokens as {CTL@ctx}>
        ps2control::addCommand("prompts", [](const std::string &args, ps2control::Reply reply) {
            if (args.rfind("text ", 0) == 0) // a text key's own text (non-ASCII as ?)
            {
                std::string s;
                for (char16_t c : gameText(args.substr(5)))
                    s += c < 128 ? static_cast<char>(c) : '?';
                return reply("ok " + s);
            }
            // prompts spec <CTL>@<foot|vehicle|menu> -> ok <the glyph the set shown draws for it>
            if (args.rfind("spec ", 0) == 0)
            {
                const std::string arg = args.substr(5);
                const size_t at = arg.find('@');
                const std::string where = at == std::string::npos ? "foot" : arg.substr(at + 1);
                const int ctl = ctlByName(arg.substr(0, at));
                const int c = where == "vehicle" ? Vehicle : where == "menu" ? Menu : Foot;
                if (ctl < 0)
                    return reply("error unknown control");
                return reply("ok " + specFor(g_shownSet.load(std::memory_order_relaxed), c, ctl));
            }
            // prompts subtitles -> ok in=<copy made> ready=<cells> calls=<style asks from the subtitle function>
            // switched=<made style 4> letters=<Oswald letters drawn>
            if (args == "subtitles")
                return reply("ok in=" + std::to_string(g_subIn) + " ready=" + std::to_string(g_subReady) + " calls=" +
                             std::to_string(g_subCalls.load()) + " switched=" + std::to_string(g_subSwitched.load()) +
                             " letters=" + std::to_string(g_subLettersDrawn.load()) + " style=" + std::to_string(rd32(kFontStyle)));
            // prompts unknown -> ok <the configuration's actions whose American text the phrase table leaves words in>
            if (args == "unknown")
            {
                std::call_once(g_referenceOnce, loadReference);
                const std::string prefix = "C" + std::to_string(rd16(kPad0Mode));
                std::string s;
                for (const auto &[key, text] : g_reference)
                    if (key.rfind(prefix, 0) == 0 && fromReference(translate(text, actionContext(key.substr(2)), *g_referencePhrases), text).empty())
                        s += " " + key;
                return reply("ok references=" + std::to_string(g_reference.size()) + s);
            }
            if (args.rfind("expand ", 0) == 0)
            {
                const std::string action = args.substr(7);
                const uint16_t mode = rd16(kPad0Mode);
                const std::u16string text = controlText(mode, action, gameText("C" + std::to_string(mode) + action));
                std::string s;
                for (char16_t c : text)
                    if (c >= kTokenBase && c < kTokenBase + kCtxCount * 0x40)
                    {
                        const int v = c - kTokenBase;
                        s += std::string("{") + kCtlNames[v & 0x3F] + "@" + (v >> 6 == Foot ? "foot" : v >> 6 == Vehicle ? "vehicle" : "menu") + "}";
                    }
                    else
                        s += c < 128 ? static_cast<char>(c) : '?';
                // the ~ around tokens: left out
                std::string clean;
                for (size_t i = 0; i < s.size(); ++i)
                    if (!(s[i] == '~' && ((i + 1 < s.size() && s[i + 1] == '{') || (i > 0 && s[i - 1] == '}'))))
                        clean += s[i];
                return reply("ok " + clean);
            }
            const int shown = g_shownSet.load(std::memory_order_relaxed);
            char buf[320];
            std::snprintf(buf, sizeof(buf), "ok setting=%d shown=%s device=%s pad=%d expanded=%u glyphs=%u buttons=%u drawn=%llu",
                          g_setting.load(), shown == PlayStation ? "ps" : shown == Xbox ? "xbox" : shown == NintendoSwitch ? "switch" : shown == Keyboard ? "keyboard" : "original",
                          input::lastDevice() == input::Device::Keyboard ? "keyboard" : "gamepad",
                          static_cast<int>(platform::gamepadFamily()), g_expanded.load(), g_lastFrameGlyphs.load(),
                          g_lastFrameButtons.load(), static_cast<unsigned long long>(gsHwGlyphDraws()));
            reply(buf);
        });
        std::fprintf(stderr, "[prompts] button prompts ready (Enhancements > Button prompts)\n");
    }
}
