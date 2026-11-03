// VCS 2D at the RenderWare level (docs/NATIVE_PORT_ROADMAP.md milestone 3 step 2): the HUD, text, radar and menus as
// the game hands them to RenderWare's Im2D, before they become GS packets. See docs/game-internals.md 6f.
//
// Im2D (FUN_002fbe08 / FUN_002fbe68) takes 16-byte vertices: u, v (u16 fractions), RGBA (bytes), x, y (s16 pixels in
// 640x448), z (u32; the HUD's is 0xfffffe, the far plane). The game's 2D pass (FUN_0021f348) draws the sky and clouds,
// renders the radar map off screen, then calls CHud::Draw (FUN_0031c628), inside which every HUD element is drawn, then
// the message / text functions (FUN_001676c8, FUN_00396670 ...).
//
// HUD context: from CHud::Draw's entry to the end of the frame (the display-list kick). Inside it, primitives drawn with
// RenderWare's depth test off (RwRenderStateSet ZTESTENABLE = 0: the GS's ZTST ALWAYS) and not at the far plane get its
// z - the weapon icon comes through a sprite routine (0x3a07a8) at z 0x4dcc3, so the renderer's widescreen placement
// (far plane = HUD) left it out, at the 4:3 position on a 16:9 picture. Without a depth test the z changes nothing
// else; depth-tested HUD draws keep theirs (the radar map is masked to its circle by depth). VCS_HUD_RW=0 turns it off.
//
// Native HUD (docs/NATIVE_RENDERER.md "HUD layer"): the HUD context's primitives are collected per frame, grouped into
// elements and anchored (resolveAnchors); at the frame kick their vertex positions go to the renderer, which asks
// classify() about each 2D primitive it gets. Named ones are squeezed towards their anchor in widescreen (HUD "Screen
// edges"; otherwise all to the middle) and drawn over the picture at the window's size (VCS_HUD_NATIVE=0: into the
// frame buffer as before). VCS_TRACE_HUDKEY=<n> logs n primitives that were not found; VCS_TRACE_HUDANCHOR=<n> the
// elements and their anchors for n frames.
//
// VCS_TRACE_IM2D=<n>: log n Im2D calls (caller, primitive, vertex count, the first vertices' raw words) and markers at
// the 2D pass's calls (VCS_TRACE_IM2D_MARKERS=<1..6>), from 30 s on.
#include "game/vcs_hud.h"

#include "game/vcs_trainer.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/gs_hw.h"
#include "runtime/ps2_render_worker.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <numeric>
#include <unordered_map>
#include <vector>

namespace vcs
{
    namespace
    {
        constexpr uint32_t kIm2DRender = 0x002FBE08u;        // (prim, vertices, count): RwIm2DRenderPrimitive
        constexpr uint32_t kIm2DRenderIndexed = 0x002FBE68u; // (prim, vertices, count, indices, indexCount)
        constexpr uint32_t kHudDraw = 0x0031C628u;           // CHud::Draw
        constexpr uint32_t kHudZ = 0x00FFFFFEu;              // the HUD's Im2D z (the far plane)
        constexpr uint32_t kRenderStateSet = 0x00175198u;    // RwRenderStateSet(state, value); 6 = rwRENDERSTATEZTESTENABLE

        PS2Runtime::RecompiledFunction g_origRender = nullptr, g_origRenderIndexed = nullptr, g_origHudDraw = nullptr,
                                       g_origRenderStateSet = nullptr;
        bool g_inHud = false;    // game thread
        bool g_inMenu = false;   // the frontend's draw (FUN_003b53f8) to the frame kick; game thread
        constexpr uint32_t kMenuDraw = 0x003B53F8u; // the frontend / pause menu's draw, called from FUN_0021f7d8
        PS2Runtime::RecompiledFunction g_origMenuDraw = nullptr;
        // the loading screen (FUN_0021E170, GTA's LoadingScreen: splash, text, progress bar, its own frame) to the frame
        // kick; laid out like a menu (game-internals.md 6f)
        constexpr uint32_t kLoadingScreen = 0x0021E170u;
        bool g_inLoading = false; // game thread
        PS2Runtime::RecompiledFunction g_origLoadingScreen = nullptr;
        bool g_zTest = true;     // RenderWare's ZTESTENABLE as the game last set it (off: the GS's ZTST ALWAYS)

        // What the HUD is drawing (game-internals.md 6f "Radar and text"), for the anchoring below. Set at the entry of
        // the routines (hooks work before the original only - AGENTS.md rule 2): the radar from its entry to the entry
        // of whatever CHud::Draw calls after it; a text line from the font's render to its glyph batch.
        constexpr uint32_t kRadarDraw = 0x00321500u;  // CHud's radar: map tiles (huge quads, scissored), ring, blips
        constexpr uint32_t kAfterRadar[] = {0x00113D50u, 0x00321758u, 0x002DFE10u}; // CHud::Draw's next call on each path
        constexpr uint32_t kFontRender = 0x00261BE8u; // font render(font, string, x, y, colour, rect, ...): every line
        constexpr uint32_t kGlyphBatch = 0x003DFE24u; // Im2D's return address in the font's glyph batch (FUN_003dfdd8)
        PS2Runtime::RecompiledFunction g_origRadarDraw = nullptr, g_origFontRender = nullptr;
        PS2Runtime::RecompiledFunction g_origAfterRadar[3] = {};
        bool g_inRadar = false;      // game thread
        bool g_textCentred = false;  // the line being printed is centred across the screen; game thread
        int g_traceAnchors = 0;      // VCS_TRACE_HUDANCHOR=<frames>

        // one window for all trace lines: from 30 s after the hooks went in, VCS_TRACE_IM2D lines in total
        auto g_traceStart = std::chrono::steady_clock::now();
        int g_traceLeft = 0;
        int g_traceDelay = 30; // VCS_TRACE_IM2D_DELAY=<s>
        bool traceNow()
        {
            if (g_traceLeft <= 0 || std::chrono::steady_clock::now() - g_traceStart < std::chrono::seconds(g_traceDelay))
                return false;
            --g_traceLeft;
            return true;
        }

        void trace(uint8_t *rdram, R5900Context *ctx, bool indexed)
        {
            if (!traceNow())
                return;
            const uint32_t verts = GPR_U32(ctx, 5) & PS2_RAM_MASK;
            std::fprintf(stderr, "[im2d] %s ra %08x prim %u n %u%s%s", indexed ? "idx" : "prim", GPR_U32(ctx, 31), GPR_U32(ctx, 4),
                         GPR_U32(ctx, 6), g_inHud ? " hud" : "", indexed ? "" : "\n");
            if (indexed)
                std::fprintf(stderr, " indices %u\n", GPR_U32(ctx, 8));
            // VCS_TRACE_IM2D_STACK=1: the return addresses on the guest stack (words in the code range right after a
            // JAL / JALR) - who called the 2D routine
            static const bool stack = std::getenv("VCS_TRACE_IM2D_STACK") != nullptr;
            if (stack)
            {
                const uint32_t sp = GPR_U32(ctx, 29) & PS2_RAM_MASK;
                std::fprintf(stderr, "[im2d]   stack:");
                for (uint32_t off = 0; off < 0x400u && sp + off + 4u <= PS2_RAM_SIZE; off += 4u)
                {
                    uint32_t w;
                    std::memcpy(&w, rdram + sp + off, 4);
                    if (w < 0x00100008u || w >= 0x00480000u || (w & 3u))
                        continue;
                    uint32_t call;
                    std::memcpy(&call, rdram + w - 8u, 4);
                    const bool jal = (call >> 26) == 0x03u, jalr = (call >> 26) == 0u && (call & 0x3Fu) == 0x09u;
                    if (jal || jalr)
                        std::fprintf(stderr, " +%x:%08x", off, w);
                }
                std::fprintf(stderr, "\n");
            }
            for (uint32_t q = 0; q < 4u; ++q)
            {
                uint32_t w[4];
                float f[4];
                std::memcpy(w, rdram + verts + q * 16u, 16);
                std::memcpy(f, w, 16);
                std::fprintf(stderr, "[im2d]   +%02x %08x %08x %08x %08x  (%g %g %g %g)\n", q * 16u, w[0], w[1], w[2], w[3], f[0], f[1],
                             f[2], f[3]);
            }
        }

        struct Marker
        {
            uint32_t address;
            const char *name;
            PS2Runtime::RecompiledFunction original;
        };
        Marker g_markers[] = {{0x001676C8u, "after1", nullptr}, {0x00396670u, "after2", nullptr}, {0x003AEE80u, "after3", nullptr},
                              {0x002A9708u, "after4", nullptr}, {0x0025CD98u, "after5", nullptr}};

        template <int I>
        PS2_GUEST_CC void marker(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (traceNow())
                std::fprintf(stderr, "[im2d] --- %s (%08x)\n", g_markers[I].name, g_markers[I].address);
            g_markers[I].original(rdram, ctx, runtime);
        }

        // ---- Per-element anchoring (widescreen, HUD "Screen edges"; see the top) ----
        enum class Part : uint8_t
        {
            Other,       // anchored by where its element sits
            Radar,       // always the left edge
            CentredText, // a line centred across the screen (subtitles, mission text, stunt results): always the middle
        };
        struct Prim
        {
            float x0, y0, x1, y1;        // bounding box, 640x448 pixels
            uint32_t firstKey, keyCount; // into g_primKeys
            bool menu = false;           // drawn by the frontend (stays in the middle)
            Part part = Part::Other;
        };
        std::atomic<bool> g_anchoring{false};            // HUD primitives are captured and classified (updateClassifier)
        std::atomic<bool> g_edges{false};                // anchored to the screen edges (else all to the middle)
        std::atomic<bool> g_hooked{false}, g_layer{false}; // the HUD context is known; the native HUD layer is on
        std::atomic<uint32_t> g_hudDraws{0};                // CHud::Draw calls let through (Hide HUD's check)
        std::vector<Prim> g_prims;                       // this frame's HUD primitives (one per Im2D call); game thread
        std::vector<uint32_t> g_primKeys;                // their vertex positions (positionKey)
        using AnchorMap = std::unordered_map<uint32_t, uint8_t>;
        // the kicked frame's vertex positions -> anchor: built here at the kick, handed to the GS side (the render worker,
        // runtime/ps2_render_worker.h) in order with the frame's display list, read there by classify
        std::shared_ptr<const AnchorMap> g_anchors = std::make_shared<AnchorMap>();
        int g_traceKeys = 0;                             // VCS_TRACE_HUDKEY=<n>

        uint32_t positionKey(int x, int y) { return uint32_t(uint16_t(x)) | (uint32_t(uint16_t(y)) << 16); } // whole pixels

        void capture(uint8_t *rdram, R5900Context *ctx)
        {
            if (!(g_inHud || g_inMenu || g_inLoading) || !g_anchoring.load(std::memory_order_relaxed) || g_prims.size() >= 8192u)
                return;
            const uint32_t verts = GPR_U32(ctx, 5) & PS2_RAM_MASK;
            const uint32_t n = GPR_U32(ctx, 6);
            if (n == 0u || n > 1024u || verts + n * 16u > PS2_RAM_SIZE)
                return;
            Prim p{1e9f, 1e9f, -1e9f, -1e9f, static_cast<uint32_t>(g_primKeys.size()), n, g_inMenu || g_inLoading};
            if (GPR_U32(ctx, 31) == kGlyphBatch)
                p.part = g_textCentred ? Part::CentredText : Part::Other;
            else if (g_inRadar)
                p.part = Part::Radar;
            for (uint32_t i = 0; i < n; ++i)
            {
                int16_t xy[2];
                std::memcpy(xy, rdram + verts + i * 16u + 8u, 4);
                p.x0 = std::min<float>(p.x0, xy[0]);
                p.x1 = std::max<float>(p.x1, xy[0]);
                p.y0 = std::min<float>(p.y0, xy[1]);
                p.y1 = std::max<float>(p.y1, xy[1]);
                g_primKeys.push_back(positionKey(xy[0], xy[1]));
            }
            g_prims.push_back(p);
        }

        // The frame's HUD primitives grouped into elements (boxes closer than a word gap merge: a line of text, the
        // clock / weapon / money cluster), each anchored by where it sits on the 4:3 screen: the left or right edge when
        // it is off to that side, the middle otherwise; full-width draws (fades, letterbox bars) stretch. The radar and
        // centred text lines are known by who draws them and kept apart: the radar's map tiles reach far past its circle
        // (to x 410), so text near it merged with it and the pair flipped between the left edge and the middle as the
        // map turned - the text, and with it the radar, jumped across the screen.
        void resolveAnchors()
        {
            auto anchors = std::make_shared<AnchorMap>();
            const uint32_t n = static_cast<uint32_t>(g_prims.size());
            std::vector<uint32_t> parent(n), order(n);
            std::iota(parent.begin(), parent.end(), 0u);
            std::iota(order.begin(), order.end(), 0u);
            auto find = [&](uint32_t i) {
                while (parent[i] != i)
                    i = parent[i] = parent[parent[i]];
                return i;
            };
            auto full = [](const Prim &p) { return p.x0 <= 1.0f && p.x1 >= 639.0f; };
            // menus are laid out as a whole: never grouped with the HUD, never moved to an edge
            auto apart = [&](const Prim &p) { return full(p) || p.menu || p.part != Part::Other; };
            constexpr float gapX = 12.0f, gapY = 3.0f;
            std::sort(order.begin(), order.end(), [](uint32_t a, uint32_t b) { return g_prims[a].x0 < g_prims[b].x0; });
            for (uint32_t oi = 0; oi < n; ++oi)
            {
                const Prim &a = g_prims[order[oi]];
                if (apart(a))
                    continue;
                for (uint32_t oj = oi + 1; oj < n && g_prims[order[oj]].x0 <= a.x1 + gapX; ++oj)
                {
                    const Prim &b = g_prims[order[oj]];
                    if (apart(b) || b.y0 > a.y1 + gapY || a.y0 > b.y1 + gapY)
                        continue;
                    parent[find(order[oj])] = find(order[oi]);
                }
            }
            const bool edges = g_edges.load(std::memory_order_relaxed);
            std::vector<Prim> boxes(n, Prim{1e9f, 1e9f, -1e9f, -1e9f, 0u, 0u});
            for (uint32_t i = 0; i < n; ++i)
            {
                Prim &b = boxes[find(i)];
                b.x0 = std::min(b.x0, g_prims[i].x0);
                b.x1 = std::max(b.x1, g_prims[i].x1);
                b.y0 = std::min(b.y0, g_prims[i].y0);
                b.y1 = std::max(b.y1, g_prims[i].y1);
                b.keyCount += 1; // (prims in the element, for the trace)
            }
            const bool traceFrame = g_traceAnchors > 0 &&
                                    std::chrono::steady_clock::now() - g_traceStart >= std::chrono::seconds(g_traceDelay);
            if (traceFrame)
                std::fprintf(stderr, "[hud-anchor] frame (%d left)\n", --g_traceAnchors);
            for (uint32_t i = 0; i < n; ++i)
            {
                const Prim &p = g_prims[i];
                const Prim &b = boxes[find(i)];
                const float centre = (b.x0 + b.x1) * 0.5f;
                const uint8_t anchor = full(p)                                      ? GSHwAnchorStretch
                                       : !edges || p.menu                            ? GSHwAnchorCentre
                                       : p.part == Part::Radar                       ? GSHwAnchorLeft
                                       : p.part == Part::CentredText                 ? GSHwAnchorCentre
                                       : centre < 280.0f && b.x0 < 160.0f            ? GSHwAnchorLeft
                                       : centre > 360.0f && b.x1 > 480.0f            ? GSHwAnchorRight
                                                                                     : GSHwAnchorCentre;
                for (uint32_t k = 0; k < p.keyCount; ++k)
                    (*anchors)[g_primKeys[p.firstKey + k]] = static_cast<uint8_t>(anchor | (p.menu ? GSHwAnchorMenu : 0u));
                if (traceFrame && find(i) == i)
                    std::fprintf(stderr, "[hud-anchor]   x %4.0f..%4.0f y %4.0f..%4.0f prims %3u part %d%s -> %s\n", b.x0, b.x1,
                                 b.y0, b.y1, b.keyCount, static_cast<int>(p.part), p.menu ? " menu" : "",
                                 anchor == GSHwAnchorStretch ? "stretch"
                                 : anchor == GSHwAnchorLeft  ? "left"
                                 : anchor == GSHwAnchorRight ? "right"
                                                             : "centre");
            }
            g_prims.clear();
            g_primKeys.clear();
            ps2_render_worker::post([anchors] { ps2_render_worker::runOnGsStage([anchors] { g_anchors = anchors; }); });
        }

        // gsHwSetHudClassifier: a primitive whose every vertex is a HUD vertex of the kicked frame takes its anchor
        uint8_t classify(const float *xy, uint32_t count)
        {
            const AnchorMap &anchors = *g_anchors;
            if (anchors.empty())
                return GSHwAnchorNone;
            // The GS sees the game's whole-pixel positions half a pixel up-left (the 2D drawing's pixel-centre bias) or
            // within a sixteenth of them: either rounding finds the pixel.
            uint8_t anchor = GSHwAnchorNone;
            for (uint32_t i = 0; i < count; ++i)
            {
                const float x = xy[2 * i], y = xy[2 * i + 1];
                const int xs[2] = {static_cast<int>(std::lround(x)), static_cast<int>(std::lround(x + 0.5f))};
                const int ys[2] = {static_cast<int>(std::lround(y)), static_cast<int>(std::lround(y + 0.5f))};
                auto it = anchors.end();
                for (int k = 0; k < 4 && it == anchors.end(); ++k)
                    it = anchors.find(positionKey(xs[k & 1], ys[k >> 1]));
                if (it == anchors.end())
                {
                    if (g_traceKeys > 0)
                    {
                        --g_traceKeys;
                        std::fprintf(stderr, "[hud-key] miss %.4f,%.4f (vertex %u of %u)\n", x, y, i, count);
                    }
                    return GSHwAnchorNone;
                }
                anchor = it->second;
            }
            return anchor;
        }

        // HUD primitives are classified for the renderer while there is a use for it: the native layer or edge anchoring
        void updateClassifier()
        {
            const bool on = g_hooked.load(std::memory_order_relaxed) &&
                            (g_layer.load(std::memory_order_relaxed) || g_edges.load(std::memory_order_relaxed));
            g_anchoring.store(on, std::memory_order_relaxed);
            gsHwSetHudClassifier(on ? &classify : nullptr);
        }

        // HUD context: everything but the far plane moved onto it (see the top)
        void toHudPlane(uint8_t *rdram, R5900Context *ctx)
        {
            if (!g_inHud || g_zTest) // depth-tested HUD draws keep their z (the radar is masked to its circle by depth)
                return;
            const uint32_t verts = GPR_U32(ctx, 5) & PS2_RAM_MASK;
            const uint32_t n = GPR_U32(ctx, 6);
            if (n == 0u || n > 1024u || verts + n * 16u > PS2_RAM_SIZE)
                return;
            for (uint32_t i = 0; i < n; ++i)
            {
                uint8_t *z = rdram + verts + i * 16u + 12u;
                uint32_t v;
                std::memcpy(&v, z, 4);
                if (v != kHudZ)
                    std::memcpy(z, &kHudZ, 4);
            }
        }

        PS2_GUEST_CC void im2dRender(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            trace(rdram, ctx, false);
            capture(rdram, ctx);
            toHudPlane(rdram, ctx);
            g_origRender(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void im2dRenderIndexed(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            trace(rdram, ctx, true);
            capture(rdram, ctx);
            toHudPlane(rdram, ctx);
            g_origRenderIndexed(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void renderStateSet(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (GPR_U32(ctx, 4) == 6u)
                g_zTest = GPR_U32(ctx, 5) != 0u;
            g_origRenderStateSet(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void hudDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (vcs::trainer::toggle(vcs::trainer::HideHud)) // the trainer's Hide HUD: none of it this frame
            {
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
            g_hudDraws.fetch_add(1, std::memory_order_relaxed);
            if (!g_hooked) // VCS_HUD_RW=0: only for Hide HUD
                return g_origHudDraw(rdram, ctx, runtime);
            if (traceNow())
                std::fprintf(stderr, "[im2d] --- hud (%08x)\n", kHudDraw);
            g_inHud = true;
            g_inRadar = false;
            g_origHudDraw(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void radarDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_inRadar = true;
            g_origRadarDraw(rdram, ctx, runtime);
        }

        template <int I>
        PS2_GUEST_CC void afterRadar(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_inRadar = false;
            g_origAfterRadar[I](rdram, ctx, runtime);
        }

        // The font's justify (font + 0x28; game-internals.md 6f): bit 1 left, 2 centred, 4 right in the rect
        // (left, bottom, right, top floats, in the PSP's 480-wide units). A line centred in a rect at least half the
        // screen wide is centred across the screen (subtitles and mission text: 24..456); narrow centred rects belong to
        // their element (the ammo count under the weapon icon: 411..462). The stunt results are centred in 116..456,
        // right of the middle to clear the 4:3 radar; with the radar at the screen edge they go to the middle too.
        PS2_GUEST_CC void fontRender(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t font = GPR_U32(ctx, 4) & PS2_RAM_MASK, rect = GPR_U32(ctx, 9) & PS2_RAM_MASK;
            g_textCentred = false;
            if (font + 0x2Cu <= PS2_RAM_SIZE && rect + 16u <= PS2_RAM_SIZE)
            {
                uint32_t justify;
                float r[4];
                std::memcpy(&justify, rdram + font + 0x28u, 4);
                std::memcpy(r, rdram + rect, 16);
                g_textCentred = !(justify & 1u) && (justify & 2u) && r[2] - r[0] >= 240.0f;
                // the rect itself moves (the glyphs are scissored to it); it is the caller's (the font's global rect
                // or the drop shadow's copy), and moving it again finds it centred already
                const float shift = 240.0f - (r[0] + r[2]) * 0.5f;
                if (g_textCentred && g_inHud && !g_inMenu && !g_inLoading && g_edges.load(std::memory_order_relaxed) &&
                    std::fabs(shift) >= 0.5f)
                {
                    const float moved[2] = {r[0] + shift, r[2] + shift};
                    std::memcpy(rdram + rect, &moved[0], 4);
                    std::memcpy(rdram + rect + 8u, &moved[1], 4);
                }
                if (g_traceAnchors > 0 && (g_inHud || g_inMenu) &&
                    std::chrono::steady_clock::now() - g_traceStart >= std::chrono::seconds(g_traceDelay))
                    std::fprintf(stderr, "[hud-anchor]   text justify %#x rect %.0f..%.0f%s\n", justify, r[0], r[2],
                                 g_textCentred ? " centred" : "");
            }
            g_origFontRender(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void menuDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (traceNow())
                std::fprintf(stderr, "[im2d] --- menu (%08x)\n", kMenuDraw);
            g_inMenu = true;
            g_origMenuDraw(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void loadingScreen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (traceNow())
                std::fprintf(stderr, "[im2d] --- loading screen (%08x)\n", kLoadingScreen);
            g_inLoading = true;
            g_origLoadingScreen(rdram, ctx, runtime);
        }
    }

    void onFrameKick()
    {
        if (traceNow())
            std::fprintf(stderr, "[im2d] --- frame kick\n");
        g_inHud = false;
        g_inMenu = false;
        g_inLoading = false;
        g_inRadar = false;
        g_textCentred = false;
        if (g_anchoring.load(std::memory_order_relaxed))
            resolveAnchors(); // the display list goes to the GS from here on
    }

    uint32_t hudDrawCount() { return g_hudDraws.load(std::memory_order_relaxed); }

    void setHudAnchoring(bool on)
    {
        g_edges.store(on, std::memory_order_relaxed);
        updateClassifier();
    }

    void installHudHooks(PS2Runtime &runtime)
    {
        const char *rw = std::getenv("VCS_HUD_RW");
        const bool on = !(rw && rw[0] == '0');
        if (const char *keys = std::getenv("VCS_TRACE_HUDKEY"))
            g_traceKeys = std::atoi(keys);
        if (const char *frames = std::getenv("VCS_TRACE_HUDANCHOR"))
            g_traceAnchors = std::atoi(frames);
        const char *traceEnv = std::getenv("VCS_TRACE_IM2D");
        g_traceStart = std::chrono::steady_clock::now();
        if (const char *delay = std::getenv("VCS_TRACE_IM2D_DELAY")) // (VCS_TRACE_HUDANCHOR's too)
            g_traceDelay = std::atoi(delay);
        g_origHudDraw = runtime.lookupFunction(kHudDraw);
        if (g_origHudDraw) // (the trainer's Hide HUD needs it in any case)
            runtime.replaceFunction(kHudDraw, &hudDraw);
        if (!on && !traceEnv)
            return;
        if (traceEnv)
            g_traceLeft = std::atoi(traceEnv);
        g_origRender = runtime.lookupFunction(kIm2DRender);
        g_origRenderIndexed = runtime.lookupFunction(kIm2DRenderIndexed);
        g_origRenderStateSet = runtime.lookupFunction(kRenderStateSet);
        if (!g_origRender || !g_origRenderIndexed || !g_origHudDraw || !g_origRenderStateSet)
            return;
        runtime.replaceFunction(kRenderStateSet, &renderStateSet);
        runtime.replaceFunction(kIm2DRender, &im2dRender);
        runtime.replaceFunction(kIm2DRenderIndexed, &im2dRenderIndexed);
        if (on)
        {
            const char *menus = std::getenv("VCS_MENU_NATIVE"); // 0: menus drawn into the frame buffer as before
            g_origMenuDraw = runtime.lookupFunction(kMenuDraw);
            if (g_origMenuDraw && !(menus && menus[0] == '0'))
                runtime.replaceFunction(kMenuDraw, &menuDraw);
            const char *loading = std::getenv("VCS_LOADING_NATIVE"); // 0: loading screens drawn into the frame buffer
            g_origLoadingScreen = runtime.lookupFunction(kLoadingScreen);
            if (g_origLoadingScreen && !(loading && loading[0] == '0'))
                runtime.replaceFunction(kLoadingScreen, &loadingScreen);
            // the radar and centred text, known by who draws them (resolveAnchors)
            const PS2Runtime::RecompiledFunction after[] = {&afterRadar<0>, &afterRadar<1>, &afterRadar<2>};
            bool parts = (g_origRadarDraw = runtime.lookupFunction(kRadarDraw)) != nullptr &&
                         (g_origFontRender = runtime.lookupFunction(kFontRender)) != nullptr;
            for (int i = 0; i < 3; ++i)
                parts = parts && (g_origAfterRadar[i] = runtime.lookupFunction(kAfterRadar[i])) != nullptr;
            if (parts)
            {
                runtime.replaceFunction(kRadarDraw, &radarDraw);
                runtime.replaceFunction(kFontRender, &fontRender);
                for (int i = 0; i < 3; ++i)
                    runtime.replaceFunction(kAfterRadar[i], after[i]);
            }
            const char *native = std::getenv("VCS_HUD_NATIVE");
            g_layer = !(native && native[0] == '0');
            g_hooked = true;
            gsHwSetHudLayer(g_layer);
            updateClassifier();
        }
        const PS2Runtime::RecompiledFunction markers[] = {&marker<0>, &marker<1>, &marker<2>, &marker<3>, &marker<4>};
        const int markerCount = std::getenv("VCS_TRACE_IM2D_MARKERS") ? std::atoi(std::getenv("VCS_TRACE_IM2D_MARKERS")) : 0;
        for (int i = 0; i < markerCount && i < 5; ++i)
        {
            g_markers[i].original = runtime.lookupFunction(g_markers[i].address);
            if (g_markers[i].original)
                runtime.replaceFunction(g_markers[i].address, markers[i]);
        }
    }
}
