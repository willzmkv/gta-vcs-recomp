// See vcs_frontend.h. How the menu is built (game-internals.md 6m, found 2026-11-05 with RAM reads of the running
// menu and tools/eedecomp.py):
//
// - The manager at 0x729480 (FUN_00471400 makes it): +0 the tab bar's page ("MASTER": Map_t, Brief_t, ... one text
//   item per tab), +4 / +8 a vector of the pages (MAP_PAGE, BRIEF_PAGE, LOAD_PAGE, ... the save / load pages), +0x24
//   the page shown (s8), +0x28 non-zero while the tab bar has the focus (else the page has it), +0x40 non-zero while
//   the menu is up.
// - A page: +0 its name, +0x2C / +0x30 a vector of items ("widgets"), +0x3C the focused one.
// - An item: +0 its name, +0x10 x, y, w, h (ints, in the menu's 480x272 units), +0x24 visible (byte), +0x28 its class
//   (vtable: +0x14 draw, +0x2C draw focused, +0x3C input), +0x6C + d * 0x10 for d = 0..6 a vector of actions for left,
//   right, up, down, select (Cross), back (Triangle), getting the focus. An action: +0 type, +4 / +8 a string (begin,
//   end). Type 0 moves the focus to the item named; 1 / 2 turn a slider or choice up / down; 3 / 5 open a page; 6 back.
// - Input (FUN_003b5758, once a frame): the D-pad or the left stick gives a direction, Cross's press "select", Triangle's
//   release "back"; nothing happens again until the pad is let go for a frame. Start (any page) and Triangle on the tab
//   bar close the menu (FUN_003b5138).
//
// So the mouse is played as the pad: a step toward the item under the cursor (a breadth-first search over the items'
// type-0 actions), Cross on it, D-pad left / right on sliders and choices, Triangle for back, L1 / R1 to zoom the map.
// Where items are on screen comes from their drawing: each item class's draw is hooked (as the pages draw them) and
// the Im2D primitives drawn inside it make up its box, in the 640x448 picture (what the player sees, text widths and
// alignment included). The map is panned by its own offset (+0xE4 / +0xE8, menu units).
#include "game/vcs_frontend.h"

#include "platform/platform.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "render/presenter.h"
#include "runtime/ps2_control.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace vcs::frontend
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        constexpr uint32_t kManager = 0x00729480u;      // the menu manager (above)
        constexpr uint32_t kManagerBuilt = 0x004CD7B0u; // FUN_00471400's "made" flag
        constexpr uint32_t kFrameCounter = 0x004CD124u; // CTimer::m_FrameCounter (runs in the menu too)
        constexpr uint32_t kMenuDraw = 0x003B53F8u;     // the menu's draw (FUN_0021f7d8 calls it while the menu is up)
        constexpr uint32_t kPageDraw = 0x003CDF20u;     // a page's draw: each visible item's draw (focused: +0x2C)
        constexpr uint32_t kIm2DRender = 0x002FBE08u, kIm2DRenderIndexed = 0x002FBE68u;
        constexpr uint32_t kGlyphBatch = 0x003DFE24u;   // Im2D's return address in the font's glyph batch: text
        // called after the pages' items in the menu's draw and right after it (FUN_0021f7d8's overlay): the item being
        // drawn is over when one of them is entered from further up the stack than the item's draw was
        constexpr uint32_t kAfterItems[] = {0x003E0B10u, 0x003E0C18u, 0x00108640u, 0x002AF108u, 0x00286E90u};
        // item classes (vtables)
        constexpr uint32_t kSlider = 0x00490680u;     // value +0xDC (0..1), steps +0xE8 (Music / SFX volume: 8)
        constexpr uint32_t kMap = 0x004905E8u;        // the map page's map: +0xE4 / +0xE8 its offset (menu units)
        constexpr uint32_t kScrollList = 0x0048F9A8u; // Brief, Stats: scrolled by up / down
        constexpr uint32_t kBackground = 0x0048FE10u; // a page's backdrop (0, 0, 480, 272): the units' place on screen

        // the pad word as game/vcs_input.cpp builds it (pressed = 1)
        constexpr uint16_t kUp = 0x0010, kRight = 0x0020, kDown = 0x0040, kLeft = 0x0080, kL1 = 0x0400, kR1 = 0x0800,
                           kTriangle = 0x1000, kCross = 0x4000;
        constexpr uint16_t kDirButton[4] = {kLeft, kRight, kUp, kDown}; // the menu's directions 0..3
        constexpr int kSelectList = 4;

        uint8_t *g_rdram = nullptr;
        bool g_mouseOn = true; // VCS_MENU_MOUSE

        bool inRam(uint32_t a, uint32_t n = 4) { return a >= 0x100000u && (a & kRamMask) + n <= 0x02000000u; }
        uint32_t rd32(uint32_t a)
        {
            uint32_t v = 0;
            if (inRam(a))
                std::memcpy(&v, g_rdram + (a & kRamMask), 4);
            return v;
        }
        float rdf(uint32_t a)
        {
            float v = 0.0f;
            if (inRam(a))
                std::memcpy(&v, g_rdram + (a & kRamMask), 4);
            return v;
        }
        void wrf(uint32_t a, float v)
        {
            if (inRam(a))
                std::memcpy(g_rdram + (a & kRamMask), &v, 4);
        }
        uint8_t rd8(uint32_t a) { return inRam(a, 1) ? g_rdram[a & kRamMask] : 0; }
        void wr8(uint32_t a, uint8_t v)
        {
            if (inRam(a, 1))
                g_rdram[a & kRamMask] = v;
        }
        void wr32(uint32_t a, uint32_t v)
        {
            if (inRam(a))
                std::memcpy(g_rdram + (a & kRamMask), &v, 4);
        }

        // a std::vector of pointers (begin, end), sanity-checked
        std::vector<uint32_t> pointers(uint32_t at)
        {
            std::vector<uint32_t> out;
            const uint32_t b = rd32(at), e = rd32(at + 4);
            if (!inRam(b) || e < b || e - b > 4u * 256u)
                return out;
            for (uint32_t p = b; p < e; p += 4)
                out.push_back(rd32(p));
            return out;
        }

        std::string itemName(uint32_t item)
        {
            std::string s;
            const uint32_t p = rd32(item);
            for (uint32_t i = 0; inRam(p + i, 1) && i < 47; ++i)
            {
                const char c = static_cast<char>(rd8(p + i));
                if (c == 0)
                    break;
                s += c >= 32 && c < 127 ? c : '?';
            }
            return s;
        }

        // an action's string is the item named `name`
        bool argIs(uint32_t action, uint32_t item)
        {
            const uint32_t b = rd32(action + 4), e = rd32(action + 8), n = rd32(item);
            if (!inRam(b) || !inRam(n) || e < b || e - b > 63)
                return false;
            for (uint32_t i = 0; i < e - b; ++i)
                if (rd8(b + i) != rd8(n + i))
                    return false;
            return rd8(n + (e - b)) == 0;
        }

        std::vector<uint32_t> actions(uint32_t item, int dir) { return pointers(item + 0x6C + static_cast<uint32_t>(dir) * 0x10u); }
        uint32_t itemClass(uint32_t item) { return rd32(item + 0x28); }
        // points somewhere: it moves the focus, selects or turns (not the backdrop, titles, help lines, the map)
        bool interactive(uint32_t item)
        {
            for (int d = 0; d <= kSelectList; ++d)
                if (!actions(item, d).empty())
                    return true;
            return false;
        }
        bool turns(uint32_t item) // right / left turn it (slider, choice) rather than moving the focus
        {
            for (uint32_t a : actions(item, 1))
                if (rd32(a) == 1)
                    return true;
            return false;
        }
        int sliderSteps(uint32_t item)
        {
            const int steps = static_cast<int>(rd32(item + 0xE8));
            return itemClass(item) == kSlider && steps >= 1 && steps <= 64 ? steps : 0;
        }
        int sliderLevel(uint32_t item)
        {
            const float v = rdf(item + 0xDC);
            return static_cast<int>(std::lround(std::clamp(std::isfinite(v) ? v : 0.0f, 0.0f, 1.0f) * sliderSteps(item)));
        }

        // ---- the menu ----------------------------------------------------------------------------------------------
        struct Menu
        {
            bool open = false;
            bool tabBar = false;   // the tab bar has the focus
            uint32_t root = 0;     // the tab bar's page
            uint32_t shown = 0;    // the page shown (under the tab bar, or the one with the focus)
            uint32_t active = 0;   // the page with the focus
            uint32_t focused = 0;  // its focused item
        };
        bool menuUp() { return g_rdram && rd8(kManagerBuilt) && rd32(kManager + 0x40) != 0; }
        Menu readMenu()
        {
            Menu m;
            m.open = menuUp();
            if (!m.open)
                return m;
            m.root = rd32(kManager);
            const std::vector<uint32_t> pages = pointers(kManager + 4);
            const int8_t cur = static_cast<int8_t>(rd8(kManager + 0x24));
            m.shown = cur >= 0 && cur < static_cast<int>(pages.size()) ? pages[cur] : 0;
            m.tabBar = rd32(kManager + 0x28) != 0;
            m.active = m.tabBar ? m.root : m.shown;
            m.focused = inRam(m.active) ? rd32(m.active + 0x3C) : 0;
            return m;
        }

        // ---- where the items are drawn (game thread) ----------------------------------------------------------------
        struct Box
        {
            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
            bool any() const { return x1 >= x0; }
            void add(float x, float y)
            {
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x);
                y1 = std::max(y1, y);
            }
            bool contains(float x, float y, float pad) const { return any() && x >= x0 - pad && x <= x1 + pad && y >= y0 - pad && y <= y1 + pad; }
        };
        struct Drawn
        {
            uint32_t item = 0;
            Box box;   // everything it drew, 640x448 pixels
            Box value; // what it drew besides text (a slider's bars)
        };
        std::vector<Drawn> g_drawing, g_drawn; // this frame's (filling) and the last whole frame's
        bool g_inMenu = false;                 // from the menu's draw to the frame kick
        uint32_t g_item = 0, g_itemSp = 0;     // the item being drawn, the stack pointer its draw was entered with

        const Drawn *drawn(uint32_t item)
        {
            for (const Drawn &d : g_drawn)
                if (d.item == item)
                    return &d;
            return nullptr;
        }

        // the map draws its tiles whole and scissored: its box is its rect (menu units -> the 640x448 drawing)
        void clipToRect(Drawn &d)
        {
            int32_t r[4];
            std::memcpy(r, g_rdram + ((d.item + 0x10u) & kRamMask), 16);
            if (r[2] <= 0 || r[3] <= 0 || r[2] > 2000 || r[3] > 2000)
                return;
            const float x0 = r[0] * (640.0f / 480.0f), y0 = r[1] * (448.0f / 272.0f);
            const float x1 = (r[0] + r[2]) * (640.0f / 480.0f), y1 = (r[1] + r[3]) * (448.0f / 272.0f);
            for (Box *b : {&d.box, &d.value})
                if (b->any())
                {
                    b->x0 = std::max(b->x0, x0);
                    b->y0 = std::max(b->y0, y0);
                    b->x1 = std::min(b->x1, x1);
                    b->y1 = std::min(b->y1, y1);
                }
        }

        // the class draw hooks: a pool of entry points, one per draw routine met (as pages draw their items)
        constexpr int kSlots = 48;
        struct Hook
        {
            uint32_t address = 0;
            PS2Runtime::RecompiledFunction original = nullptr;
        };
        std::array<Hook, kSlots> g_hooks{};
        int g_hookCount = 0;
        std::vector<uint32_t> g_classesSeen;

        template <int I>
        PS2_GUEST_CC void itemDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_inMenu)
            {
                g_item = GPR_U32(ctx, 4) & kRamMask;
                g_itemSp = GPR_U32(ctx, 29);
            }
            g_hooks[I].original(rdram, ctx, runtime);
        }
        template <size_t... I>
        constexpr std::array<PS2Runtime::RecompiledFunction, sizeof...(I)> drawHooks(std::index_sequence<I...>)
        {
            return {&itemDraw<static_cast<int>(I)>...};
        }
        constexpr auto kDrawHooks = drawHooks(std::make_index_sequence<kSlots>{});

        void hookClass(PS2Runtime *runtime, uint32_t vtable)
        {
            if (!inRam(vtable) || std::find(g_classesSeen.begin(), g_classesSeen.end(), vtable) != g_classesSeen.end())
                return;
            g_classesSeen.push_back(vtable);
            for (uint32_t slot : {0x14u, 0x2Cu})
            {
                const uint32_t fn = rd32(vtable + slot);
                if (fn < 0x100000u || fn >= 0x02000000u || g_hookCount >= kSlots)
                    continue;
                bool known = false;
                for (int i = 0; i < g_hookCount; ++i)
                    known = known || g_hooks[i].address == fn;
                if (known)
                    continue;
                PS2Runtime::RecompiledFunction original = runtime->lookupFunction(fn);
                if (!original)
                    continue;
                g_hooks[g_hookCount] = {fn, original};
                runtime->replaceFunction(fn, kDrawHooks[g_hookCount]);
                ++g_hookCount;
            }
        }

        PS2Runtime::RecompiledFunction g_origMenuDraw = nullptr, g_origPageDraw = nullptr, g_origRender = nullptr,
                                       g_origRenderIndexed = nullptr;
        std::array<PS2Runtime::RecompiledFunction, std::size(kAfterItems)> g_origAfter{};

        PS2_GUEST_CC void menuDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            for (Drawn &d : g_drawing)
                if (itemClass(d.item) == kMap)
                    clipToRect(d);
            g_drawn.swap(g_drawing);
            g_drawing.clear();
            g_inMenu = true;
            g_item = 0;
            g_origMenuDraw(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void pageDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_inMenu)
            {
                g_item = 0;
                const uint32_t page = GPR_U32(ctx, 4) & kRamMask;
                for (uint32_t item : pointers(page + 0x2C))
                    hookClass(runtime, itemClass(item));
            }
            g_origPageDraw(rdram, ctx, runtime);
        }

        template <int I>
        PS2_GUEST_CC void afterItems(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_item && GPR_U32(ctx, 29) > g_itemSp) // not called from inside the item's draw
                g_item = 0;
            g_origAfter[I](rdram, ctx, runtime);
        }
        template <size_t... I>
        constexpr std::array<PS2Runtime::RecompiledFunction, sizeof...(I)> afterHooks(std::index_sequence<I...>)
        {
            return {&afterItems<static_cast<int>(I)>...};
        }

        void capture(uint8_t *rdram, R5900Context *ctx)
        {
            if (!g_inMenu || !g_item || GPR_U32(ctx, 29) >= g_itemSp || g_drawing.size() >= 256)
                return;
            const uint32_t verts = GPR_U32(ctx, 5) & kRamMask;
            const uint32_t n = GPR_U32(ctx, 6);
            if (n == 0u || n > 4096u || verts + n * 16u > 0x02000000u)
                return;
            auto it = std::find_if(g_drawing.begin(), g_drawing.end(), [](const Drawn &d) { return d.item == g_item; });
            if (it == g_drawing.end())
            {
                g_drawing.push_back({g_item, {}, {}});
                it = g_drawing.end() - 1;
            }
            const bool text = GPR_U32(ctx, 31) == kGlyphBatch;
            for (uint32_t i = 0; i < n; ++i)
            {
                int16_t xy[2];
                std::memcpy(xy, rdram + verts + i * 16u + 8u, 4); // Im2D vertex: u, v, RGBA, x, y (s16, 640x448), z
                it->box.add(xy[0], xy[1]);
                if (!text)
                    it->value.add(xy[0], xy[1]);
            }
        }

        PS2_GUEST_CC void im2dRender(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            capture(rdram, ctx);
            g_origRender(rdram, ctx, runtime);
        }
        PS2_GUEST_CC void im2dRenderIndexed(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            capture(rdram, ctx);
            g_origRenderIndexed(rdram, ctx, runtime);
        }

        // ---- the cursor (main thread -> game thread) -------------------------------------------------------------------
        std::atomic<float> g_cursorX{0.0f}, g_cursorY{0.0f}; // over the 640x448 picture
        std::atomic<bool> g_cursorValid{false};
        std::atomic<int> g_windowW{0}, g_windowH{0}; // drawable pixels, at the last host frame (menustate)
        // "menumouse" (tests): the cursor set from the control channel - over the 640x448 picture ("menumouse x y") or
        // in window pixels ("menumouse window x y": through onHostFrame's mapping, as the real cursor)
        enum Injected : int { kNotInjected, kInjectedPicture, kInjectedWindow };
        std::atomic<int> g_injected{kNotInjected};
        std::atomic<float> g_injectedX{0.0f}, g_injectedY{0.0f};

        bool cursor(float &x, float &y)
        {
            if (g_injected.load(std::memory_order_relaxed) == kInjectedPicture)
            {
                x = g_injectedX.load(std::memory_order_relaxed);
                y = g_injectedY.load(std::memory_order_relaxed);
                return true;
            }
            x = g_cursorX.load(std::memory_order_relaxed);
            y = g_cursorY.load(std::memory_order_relaxed);
            return g_cursorValid.load(std::memory_order_relaxed);
        }

        // ---- the menu's keys and mouse buttons (game thread) -----------------------------------------------------------
        using namespace platform;
        constexpr int kMenuKeys[] = {KEY_ESCAPE, KEY_ENTER, KEY_KP_ENTER, KEY_MOUSE_LEFT, KEY_MOUSE_RIGHT, KEY_WHEEL_UP, KEY_WHEEL_DOWN};
        constexpr size_t kMenuKeyCount = std::size(kMenuKeys);
        bool mouseKey(int key) { return key >= KEY_MOUSE_LEFT; }

        struct Goal
        {
            enum Act
            {
                Go,     // only move the focus there
                Select, // then Cross
                Turn,   // then right / left `steps` times (+ right)
                Level,  // then right / left until the slider shows `level`
            };
            uint32_t page = 0, item = 0; // where to go (item 0: nowhere)
            Act act = Go;
            int steps = 0, level = 0;
            int ttl = 0; // game frames left to get there
        };

        struct State
        {
            uint32_t frame = ~0u;
            uint16_t out = 0;    // the buttons the menu adds this game frame
            bool rest = false;   // the frame after a tap: nothing, so the next one is a press of its own
            struct Tap
            {
                uint16_t button;
                int frames; // held this many game frames (the map zooms while L1 / R1 are held)
            };
            std::vector<Tap> taps; // to send before the goal (Triangle, Cross to open a page, a zoom)
            uint16_t held = 0;     // a tap still held ...
            int heldFrames = 0;    // ... this many more frames
            Goal goal;
            bool open = false;
            std::array<bool, kMenuKeyCount> was{}, owned{};
            bool escArmed = false, enterArmed = false;
            bool cursorKnown = false;
            float cx = 0.0f, cy = 0.0f; // the cursor at the last frame
            uint32_t dragSlider = 0, dragMap = 0;
            bool publishedOpen = true; // "menustate" last said the menu is open
        } g;

        bool keyHeld(int key) { return ps2control::keyDown(key); }

        // the first step from `from` toward `to` over the page's focus moves (breadth first); -1: no way
        int route(uint32_t page, uint32_t from, uint32_t to)
        {
            const std::vector<uint32_t> items = pointers(page + 0x2C);
            if (items.size() > 64 || from == to)
                return -1;
            auto index = [&](uint32_t it) { return static_cast<int>(std::find(items.begin(), items.end(), it) - items.begin()); };
            const int n = static_cast<int>(items.size()), start = index(from), goal = index(to);
            if (start >= n || goal >= n)
                return -1;
            std::vector<int> firstDir(n, -1);
            std::vector<bool> seen(n, false);
            std::vector<int> queue{start};
            seen[start] = true;
            for (size_t q = 0; q < queue.size(); ++q)
            {
                const int at = queue[q];
                for (int d = 0; d < 4; ++d)
                    for (uint32_t a : actions(items[at], d))
                    {
                        if (rd32(a) != 0)
                            continue;
                        for (int k = 0; k < n; ++k)
                            if (!seen[k] && argIs(a, items[k]))
                            {
                                seen[k] = true;
                                firstDir[k] = at == start ? d : firstDir[at];
                                if (k == goal)
                                    return firstDir[k];
                                queue.push_back(k);
                            }
                    }
            }
            return -1;
        }

        // the interactive item under the cursor on `page` (the smallest box if boxes overlap); `any`: any item drawn
        // (not the backdrop)
        uint32_t hit(uint32_t page, float x, float y, bool any = false)
        {
            uint32_t best = 0;
            float bestArea = 1e30f;
            for (uint32_t item : pointers(page + 0x2C))
            {
                const Drawn *d = drawn(item);
                if (!d || !rd8(item + 0x24) || !d->box.contains(x, y, 3.0f))
                    continue;
                if (any ? itemClass(item) == kBackground : !interactive(item))
                    continue;
                const float area = (d->box.x1 - d->box.x0 + 6.0f) * (d->box.y1 - d->box.y0 + 6.0f);
                if (area < bestArea)
                {
                    best = item;
                    bestArea = area;
                }
            }
            return best;
        }

        // a slider's level for the cursor at x: over the bars, up to the bar under it; just left of them, 0; elsewhere
        // (its label) -1
        int levelAt(uint32_t item, float x)
        {
            const int steps = sliderSteps(item);
            const Drawn *d = drawn(item);
            if (!steps || !d)
                return -1;
            const Box &bars = d->value.any() ? d->value : d->box;
            if (x < bars.x0 - 8.0f)
                return -1;
            if (x < bars.x0)
                return 0;
            const float f = (x - bars.x0) / std::max(1.0f, bars.x1 - bars.x0);
            return std::clamp(static_cast<int>(std::floor(f * steps)) + 1, 0, steps);
        }

        // what a click on `item` does once it has the focus
        Goal clickGoal(uint32_t page, uint32_t item, float x)
        {
            Goal q;
            q.page = page;
            q.item = item;
            q.ttl = 90;
            if (sliderSteps(item))
            {
                q.level = levelAt(item, x);
                q.act = q.level >= 0 ? Goal::Level : Goal::Go;
            }
            else if (!actions(item, kSelectList).empty())
                q.act = Goal::Select;
            else if (turns(item))
            {
                q.act = Goal::Turn;
                q.steps = 1;
            }
            return q;
        }

        // menu units per pixel of the 640x448 picture, from the page's backdrop (480x272 units)
        void unitsPerPixel(uint32_t page, float &ux, float &uy)
        {
            ux = 1.0f;
            uy = 272.0f / 360.0f;
            for (uint32_t item : pointers(page + 0x2C))
                if (itemClass(item) == kBackground)
                    if (const Drawn *d = drawn(item); d && d->box.x1 - d->box.x0 > 100.0f && d->box.y1 - d->box.y0 > 100.0f)
                    {
                        ux = 480.0f / (d->box.x1 - d->box.x0);
                        uy = 272.0f / (d->box.y1 - d->box.y0);
                    }
        }

        // the goal's next button, or 0
        uint16_t goalStep(const Menu &m)
        {
            Goal &q = g.goal;
            if (!q.item)
                return 0;
            if (--q.ttl < 0)
            {
                q = {};
                return 0;
            }
            if (q.page != m.active)
                return 0; // a page still opening (the Cross before it)
            if (m.focused != q.item)
            {
                const int d = route(m.active, m.focused, q.item);
                if (d < 0)
                    q = {};
                return d < 0 ? 0 : kDirButton[d];
            }
            switch (q.act)
            {
            case Goal::Go:
                q = {};
                return 0;
            case Goal::Select:
                q = {};
                return kCross;
            case Goal::Turn:
            {
                if (q.steps == 0)
                {
                    q = {};
                    return 0;
                }
                const bool up = q.steps > 0;
                q.steps += up ? -1 : 1;
                return up ? kRight : kLeft;
            }
            case Goal::Level:
            {
                const int level = sliderLevel(q.item);
                if (level != q.level)
                    return level < q.level ? kRight : kLeft;
                if (g.dragSlider == q.item)
                    q.ttl = 90; // held: it follows the cursor
                else
                    q = {};
                return 0;
            }
            }
            return 0;
        }

        // ---- "menustate" -----------------------------------------------------------------------------------------------
        std::mutex g_stateMutex;
        std::string g_state = "open=0";

        void publish(const Menu &m)
        {
            std::string s;
            char buf[256];
            if (!m.open)
                s = "open=0";
            else
            {
                std::string page = m.shown ? itemName(m.shown) : "-", focus = m.focused ? itemName(m.focused) : "-";
                std::replace(page.begin(), page.end(), ' ', '_');
                std::replace(focus.begin(), focus.end(), ' ', '_');
                std::snprintf(buf, sizeof(buf), "open=1 tabbar=%d page=%s focus=%s", m.tabBar ? 1 : 0, page.c_str(), focus.c_str());
                s = buf;
                // the window's size in pixels ("menumouse window"'s units; a screenshot may be another size) and the
                // cursor over the 640x448 picture, as the menu sees it
                float cx = 0.0f, cy = 0.0f;
                const bool on = cursor(cx, cy);
                std::snprintf(buf, sizeof(buf), " window=%dx%d cursor=", g_windowW.load(std::memory_order_relaxed),
                              g_windowH.load(std::memory_order_relaxed));
                s += buf;
                if (on)
                    std::snprintf(buf, sizeof(buf), "%.1f,%.1f", cx, cy);
                s += on ? buf : "-";
                // the items of the page shown and of the tab bar: name:x0,y0,x1,y1[:i interactive][:v level/steps]
                // [:m pan x, pan y] ("menumouse" takes the same 640x448 coordinates)
                s += " items=";
                for (uint32_t page : {m.shown, m.root})
                    for (uint32_t item : pointers(page + 0x2C))
                    {
                        const Drawn *d = drawn(item);
                        if (!d || !rd8(item + 0x24))
                            continue;
                        std::string name = itemName(item);
                        std::replace_if(name.begin(), name.end(), [](char c) { return c == ' ' || c == ':' || c == ';' || c == '='; }, '_');
                        std::snprintf(buf, sizeof(buf), "%s:%.0f,%.0f,%.0f,%.0f", name.c_str(), d->box.x0, d->box.y0, d->box.x1, d->box.y1);
                        s += buf;
                        if (interactive(item))
                            s += ":i";
                        if (const int steps = sliderSteps(item))
                        {
                            std::snprintf(buf, sizeof(buf), ":v%d/%d", sliderLevel(item), steps);
                            s += buf;
                            if (d->value.any())
                            {
                                std::snprintf(buf, sizeof(buf), ":b%.0f,%.0f", d->value.x0, d->value.x1); // the bars
                                s += buf;
                            }
                        }
                        if (turns(item) && !sliderSteps(item))
                        {
                            std::snprintf(buf, sizeof(buf), ":c%u", rd8(item + 0xED));
                            s += buf;
                        }
                        if (itemClass(item) == kMap)
                        {
                            std::snprintf(buf, sizeof(buf), ":m%.1f,%.1f,%.1f", rdf(item + 0xE4), rdf(item + 0xE8), rdf(item + 0xF0));
                            s += buf;
                        }
                        s += ';';
                    }
            }
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_state = s;
        }

        void step(uint16_t user, bool sticks, bool blocked)
        {
            const Menu m = readMenu();
            // the menu's keys: owned while down since the menu had them (so they stay out of the bindings until let go)
            std::array<bool, kMenuKeyCount> now{};
            for (size_t i = 0; i < kMenuKeyCount; ++i)
            {
                now[i] = keyHeld(kMenuKeys[i]);
                const bool menuHas = m.open && !blocked && (g_mouseOn || !mouseKey(kMenuKeys[i]));
                g.owned[i] = now[i] && (menuHas || g.owned[i]);
            }
            const bool alt = keyHeld(KEY_LEFT_ALT) || keyHeld(KEY_RIGHT_ALT); // Alt+Enter is full screen
            const bool esc = now[0], enter = (now[1] || now[2]) && !alt;
            const bool escPress = esc && !g.was[0], enterPress = enter && !(g.was[1] || g.was[2]);
            const bool left = now[3], right = now[4];
            const bool leftPress = left && !g.was[3], rightPress = right && !g.was[4];
            const bool wheelUp = now[5] && !g.was[5], wheelDown = now[6] && !g.was[6];
            g.was = now;

            if (m.open != g.open)
            {
                g.open = m.open;
                g.goal = {};
                g.taps.clear();
                g.rest = false;
                g.dragSlider = g.dragMap = 0;
                g.escArmed = g.enterArmed = false;
                g.cursorKnown = false;
                if (!m.open)
                    g_drawn.clear();
            }
            if (!m.open || blocked)
            {
                g.out = 0;
                g.goal = {};
                g.taps.clear();
                g.escArmed = g.enterArmed = false;
                g.heldFrames = 0;
                if (m.open || g.publishedOpen)
                    publish(m);
                g.publishedOpen = m.open;
                return;
            }
            g.publishedOpen = true;

            // Enter = Cross, Escape = Triangle while held - pressed in the menu (Escape still down from opening it is
            // not a back)
            g.escArmed = esc && (g.escArmed || escPress);
            g.enterArmed = enter && (g.enterArmed || enterPress);
            const uint16_t hold = static_cast<uint16_t>((g.escArmed ? kTriangle : 0) | (g.enterArmed ? kCross : 0));
            if (user || sticks || hold) // the player's own input takes over
            {
                g.goal = {};
                g.taps.clear();
                g.heldFrames = 0;
            }

            float x = 0.0f, y = 0.0f;
            const bool over = g_mouseOn && cursor(x, y);
            const bool moved = over && g.cursorKnown && std::fabs(x - g.cx) + std::fabs(y - g.cy) > 0.75f;
            const float dx = over && g.cursorKnown ? x - g.cx : 0.0f, dy = over && g.cursorKnown ? y - g.cy : 0.0f;
            if (over)
            {
                g.cx = x;
                g.cy = y;
                g.cursorKnown = true;
            }
            const uint32_t focusClass = m.focused ? itemClass(m.focused) : 0;
            if (g_mouseOn && !user && !sticks && !hold)
            {
                if (!left)
                    g.dragSlider = g.dragMap = 0;
                // pointing at an item gives it the focus (on the tab bar: shows its page, as the D-pad does)
                if (moved && !left && !right && g.goal.act == Goal::Go)
                    if (const uint32_t item = hit(m.active, x, y); item && item != m.focused && item != g.goal.item)
                        g.goal = {m.active, item, Goal::Go, 0, 0, 60};
                if (leftPress && over)
                {
                    g.goal = {};
                    g.taps.clear();
                    g.heldFrames = 0;
                    const Drawn *map = focusClass == kMap ? drawn(m.focused) : nullptr;
                    if (map && map->box.contains(x, y, 0.0f))
                        g.dragMap = m.focused; // dragged: the map moves with the cursor
                    else if (const uint32_t item = hit(m.active, x, y))
                    {
                        g.goal = clickGoal(m.active, item, x);
                        g.dragSlider = sliderSteps(item) ? item : 0;
                    }
                    else if (m.tabBar && m.shown && hit(m.shown, x, y, true))
                    {
                        // the page under the tab bar: open it (Cross on its tab), then do the click there
                        g.taps.push_back({kCross, 1});
                        if (const uint32_t item = hit(m.shown, x, y))
                        {
                            g.goal = clickGoal(m.shown, item, x);
                            g.dragSlider = sliderSteps(item) ? item : 0;
                        }
                    }
                }
                if (left && over && g.dragSlider && g.goal.item == g.dragSlider && g.goal.act == Goal::Level)
                    if (const int level = levelAt(g.dragSlider, x); level >= 0)
                        g.goal.level = level;
                if (left && g.dragMap && g.dragMap == m.focused && (dx != 0.0f || dy != 0.0f))
                {
                    // in the range the D-pad scrolls it: the offset from the middle (240, 114) at most the zoom (+0xF0)
                    // over its smallest (+0x104) - measured at three zooms; past it the game draws the map wrong
                    float ux = 1.0f, uy = 1.0f;
                    unitsPerPixel(m.active, ux, uy);
                    const float reach = std::max(0.0f, rdf(g.dragMap + 0xF0) - rdf(g.dragMap + 0x104));
                    if (std::isfinite(reach) && reach < 2000.0f)
                    {
                        wrf(g.dragMap + 0xE4, std::clamp(rdf(g.dragMap + 0xE4) + dx * ux, 240.0f - reach, 240.0f + reach));
                        wrf(g.dragMap + 0xE8, std::clamp(rdf(g.dragMap + 0xE8) + dy * uy, 114.0f - reach, 114.0f + reach));
                    }
                }
                if (rightPress && over)
                {
                    g.goal = {};
                    g.heldFrames = 0;
                    g.taps.assign(1, {kTriangle, 1}); // back (on the tab bar: the menu closes)
                }
                if ((wheelUp || wheelDown) && over)
                {
                    const int notch = wheelUp ? 1 : -1;
                    const uint32_t item = hit(m.active, x, y);
                    if (item && (turns(item) || sliderSteps(item)))
                    {
                        // a slider or choice under the cursor turns (up: right)
                        if (g.goal.item == item && g.goal.act == Goal::Turn)
                            g.goal.steps = std::clamp(g.goal.steps + notch, -8, 8);
                        else
                            g.goal = {m.active, item, Goal::Turn, notch, 0, 90};
                    }
                    else if (focusClass == kMap && g.taps.size() < 4)
                        g.taps.push_back({static_cast<uint16_t>(wheelUp ? kR1 : kL1), 6}); // zoom (R1 in, L1 out): 48 units
                    else if (focusClass == kScrollList && g.taps.size() < 4)
                        g.taps.push_back({static_cast<uint16_t>(wheelUp ? kUp : kDown), 3});
                }
            }

            uint16_t tap = 0;
            if (g.heldFrames > 0)
            {
                tap = g.held;
                g.rest = --g.heldFrames == 0;
            }
            else if (g.rest)
                g.rest = false;
            else if (!g.taps.empty())
            {
                tap = g.held = g.taps.front().button;
                g.heldFrames = g.taps.front().frames - 1;
                g.taps.erase(g.taps.begin());
                g.rest = g.heldFrames <= 0;
            }
            else
            {
                tap = goalStep(m);
                g.rest = tap != 0;
            }
            g.out = static_cast<uint16_t>(hold | tap);
            publish(m);
        }

        // ---- the Display page's Widescreen row, hidden ------------------------------------------------------------
        // The game's own widescreen (option 14, the item "wide_scr_MI") is the PS2's anamorphic mode, kept off
        // (vcs_display.cpp); its row goes. DISPLAY_PAGE's rows (built once at boot, game-internals.md 6m): Brightness_MI
        // y 40, Subtitles_MI 65, wide_scr_MI 89, RADAR_MODE_MI 116, HUD_MODE_MI 141, Trails_MI 166, screen_pos_MI 191 (480x272
        // units), chained by type-0 actions up (d 2) / down (d 3), the last wrapping to the first. Hidden: its visible
        // byte cleared; an up / down action that names it trades its string with the hidden row's own action of that
        // direction (which names the row past it) - a swap of the two std::strings (+4: begin, end, 0, capacity end),
        // so each buffer keeps one owner; the rows below move up by the gap. Checked each frame kick (a byte read once
        // done), so a page the game rebuilt is hidden again. VCS_HIDE_WIDESCREEN=0, or VCS_WIDESCREEN set (the PS2's mode
        // on purpose): the row stays.
        constexpr const char *kWidescreenRow = "wide_scr_MI";
        bool g_hideWidescreen = true;
        uint32_t g_hiddenRow = 0;           // the item once hidden (its visible byte watched)
        std::vector<std::pair<uint32_t, int32_t>> g_rowsMoved; // item, its y as the game laid it out
        int32_t g_rowGap = 0;

        bool nameIs(uint32_t item, const char *name)
        {
            const uint32_t p = rd32(item);
            for (uint32_t i = 0;; ++i)
            {
                if (!inRam(p + i, 1) || i > 47)
                    return false;
                if (rd8(p + i) != static_cast<uint8_t>(name[i]))
                    return false;
                if (name[i] == 0)
                    return true;
            }
        }

        int32_t itemY(uint32_t item) { return static_cast<int32_t>(rd32(item + 0x14)); }

        void hideWidescreenRow()
        {
            if (!g_hideWidescreen || !g_rdram || !rd8(kManagerBuilt))
                return;
            if (g_hiddenRow && rd8(g_hiddenRow + 0x24) == 0 && nameIs(g_hiddenRow, kWidescreenRow))
                return; // still hidden
            uint32_t page = 0, row = 0;
            for (uint32_t p : pointers(kManager + 4))
                for (uint32_t item : pointers(p + 0x2C))
                    if (nameIs(item, kWidescreenRow))
                        page = p, row = item;
            if (!row)
                return;
            const std::vector<uint32_t> items = pointers(page + 0x2C);
            // the neighbours' focus moves past it
            int swapped = 0;
            for (uint32_t item : items)
            {
                if (item == row)
                    continue;
                for (int d = 2; d <= 3; ++d)
                    for (uint32_t a : actions(item, d))
                    {
                        if (rd32(a) != 0 || !argIs(a, row))
                            continue;
                        const std::vector<uint32_t> own = actions(row, d);
                        if (own.size() != 1 || rd32(own[0]) != 0)
                            continue;
                        for (uint32_t w = 4; w < 0x14; w += 4)
                        {
                            const uint32_t t = rd32(a + w);
                            wr32(a + w, rd32(own[0] + w));
                            wr32(own[0] + w, t);
                        }
                        ++swapped;
                    }
            }
            // the rows below it move up by the gap (rows: items with focus / select actions; not the backdrop, the
            // title or the screen-position frame). Found once, from the game's layout; a second hiding (the page made
            // anew) puts the same rows at the same places.
            if (g_hiddenRow != row)
            {
                g_rowsMoved.clear();
                g_rowGap = 0;
                const int32_t y = itemY(row);
                int32_t next = INT32_MAX;
                for (uint32_t item : items)
                    if (item != row && interactive(item) && itemY(item) > y)
                    {
                        g_rowsMoved.emplace_back(item, itemY(item));
                        next = std::min(next, itemY(item));
                    }
                g_rowGap = g_rowsMoved.empty() ? 0 : next - y;
            }
            for (const auto &[item, y] : g_rowsMoved)
                wr32(item + 0x14, static_cast<uint32_t>(y - g_rowGap));
            wr8(row + 0x24, 0);
            g_hiddenRow = row;
            std::fprintf(stderr, "[menu] the Display page's Widescreen row hidden (%d focus moves past it, %zu rows up %d)\n",
                         swapped, g_rowsMoved.size(), g_rowGap);
        }
    }

    bool open() { return menuUp(); }

    void onHostFrame()
    {
        int w = 0, h = 0;
        windowSize(w, h);
        g_windowW.store(w, std::memory_order_relaxed);
        g_windowH.store(h, std::memory_order_relaxed);
        float mx = 0.0f, my = 0.0f;
        bool over = false;
        if (g_injected.load(std::memory_order_relaxed) == kInjectedWindow)
        {
            mx = g_injectedX.load(std::memory_order_relaxed);
            my = g_injectedY.load(std::memory_order_relaxed);
            over = g_mouseOn && w > 0 && h > 0;
        }
        else
            over = g_mouseOn && ps2control::physicalInputOn() && mousePosition(mx, my) && w > 0 && h > 0;
        if (over)
        {
            // The menu is shown at 4:3 in the game's frame (vcs_hud.cpp anchors it in the middle; the presenter's
            // fitRect: a 4:3 frame fills it, a wide one has the menu squeezed into its middle 4:3), and its 640x448
            // drawing fills that area - the items' boxes are in the same coordinates. (A shrink to 480x360 here once
            // was the frame's own letterbox counted twice: the hit areas sat towards the middle of the screen.)
            float x0 = 0.0f, y0 = 0.0f, fw = 0.0f, fh = 0.0f;
            present::fitRect(4.0f / 3.0f, present::displayAspect(true), static_cast<float>(w), static_cast<float>(h), x0, y0, fw, fh);
            if (fw > 0.0f && fh > 0.0f)
            {
                g_cursorX.store((mx - x0) / fw * 640.0f, std::memory_order_relaxed);
                g_cursorY.store((my - y0) / fh * 448.0f, std::memory_order_relaxed);
            }
            else
                over = false;
        }
        g_cursorValid.store(over, std::memory_order_relaxed);
    }

    uint16_t padButtons(uint16_t user, bool sticks, bool blocked)
    {
        if (!g_rdram)
            return 0;
        const uint32_t frame = rd32(kFrameCounter);
        if (frame != g.frame) // the game reads the pad several times a frame: one step per frame
        {
            g.frame = frame;
            step(user, sticks, blocked);
        }
        return g.out;
    }

    bool ownsKey(int key)
    {
        for (size_t i = 0; i < kMenuKeyCount; ++i)
            if (kMenuKeys[i] == key)
                return g.owned[i] || (g.open && (g_mouseOn || !mouseKey(key)));
        return false;
    }

    uint32_t drawingItem(uint32_t sp) { return g_inMenu && g_item && sp < g_itemSp ? g_item : 0u; }

    void onFrameKick()
    {
        g_inMenu = false;
        g_item = 0;
        hideWidescreenRow();
    }

    void install(PS2Runtime &runtime)
    {
        g_rdram = runtime.memory().getRDRAM();
        const char *env = std::getenv("VCS_MENU_MOUSE");
        g_mouseOn = !(env && env[0] == '0');
        const char *hide = std::getenv("VCS_HIDE_WIDESCREEN"), *wide = std::getenv("VCS_WIDESCREEN");
        g_hideWidescreen = !(hide && hide[0] == '0') && !(wide && *wide && *wide != '0');
        if (g_mouseOn)
        {
            g_origMenuDraw = runtime.lookupFunction(kMenuDraw); // (the HUD's hook when it is in: chained)
            g_origPageDraw = runtime.lookupFunction(kPageDraw);
            g_origRender = runtime.lookupFunction(kIm2DRender);
            g_origRenderIndexed = runtime.lookupFunction(kIm2DRenderIndexed);
            bool ok = g_origMenuDraw && g_origPageDraw && g_origRender && g_origRenderIndexed;
            for (size_t i = 0; i < std::size(kAfterItems); ++i)
                ok = ok && (g_origAfter[i] = runtime.lookupFunction(kAfterItems[i])) != nullptr;
            if (ok)
            {
                runtime.replaceFunction(kMenuDraw, &menuDraw);
                runtime.replaceFunction(kPageDraw, &pageDraw);
                runtime.replaceFunction(kIm2DRender, &im2dRender);
                runtime.replaceFunction(kIm2DRenderIndexed, &im2dRenderIndexed);
                constexpr auto after = afterHooks(std::make_index_sequence<std::size(kAfterItems)>{});
                for (size_t i = 0; i < std::size(kAfterItems); ++i)
                    runtime.replaceFunction(kAfterItems[i], after[i]);
            }
            else
            {
                g_mouseOn = false;
                std::fprintf(stderr, "[menu] the menu's draw routines not found: no mouse in the menu\n");
            }
        }
        std::fprintf(stderr, "[menu] pause menu: Enter selects, Escape goes back%s\n", g_mouseOn ? ", the mouse points and clicks" : "");

        // menustate -> ok open=0|1 tabbar= page= focus= items=name:x0,y0,x1,y1[:i][:vL/S][:cN][:mX,Y,Z];... (the page
        // shown and the tab bar; 640x448 picture coordinates; tools/menu_check.py)
        ps2control::addCommand("menustate", [](const std::string &, ps2control::Reply reply) {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            reply("ok " + g_state);
        });
        // menumouse <x> <y> | menumouse window <x> <y> | menumouse off -> ok: the cursor set from here (hidden test
        // runs): over the 640x448 picture, or in the window's pixels (the real cursor's mapping, at the next host
        // frame). The buttons and the wheel are keys: "key mouseleft down", "press wheelup".
        ps2control::addCommand("menumouse", [](const std::string &args, ps2control::Reply reply) {
            float x = 0.0f, y = 0.0f;
            if (args == "off")
                g_injected.store(kNotInjected, std::memory_order_relaxed);
            else if (std::sscanf(args.c_str(), "window %f %f", &x, &y) == 2 || std::sscanf(args.c_str(), "%f %f", &x, &y) == 2)
            {
                g_injectedX.store(x, std::memory_order_relaxed);
                g_injectedY.store(y, std::memory_order_relaxed);
                g_injected.store(args.rfind("window", 0) == 0 ? kInjectedWindow : kInjectedPicture, std::memory_order_relaxed);
            }
            else
                return reply("error usage: menumouse <x> <y> | menumouse window <x> <y> | menumouse off");
            reply("ok");
        });
    }
}
