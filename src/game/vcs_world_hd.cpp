// HD World Further (FORK.md T2): extend the detailed city radius beyond ~260m (3x3 cells).
// The game streams 3x3 hex cells of high-detail geometry around the camera; low-detail covers the whole map.
// This hooks the area chooser and sector list to request/draw more cells in detail.
//
// Phase 1 (2026-11-09): REMOVED unsafe host-thread streaming and invalid area slots.
// Per docs/HD_WORLD_STREAMING_HANDOFF.md:
// - P0: initHDAreaSlots used level+0x25C..0x288 as slots, but those are level fields, not area slots.
//   Real area slots are at level+0x2C (four slots of 0xA0 bytes).
// - P0: processHdRequestQueue called RequestArea (0x002415B0) from std::thread host worker.
//   Violates runtime rules: guest RAM/streaming/scheduler accessed concurrently; recompiled
//   functions may yield to guest scheduler mid-body.
// - P0: worldDrawHook spliced sector lists without invariants (owner tags, buffer lifetime,
//   cross-fade merge logic from FUN_0023E748).
// - P1: Even if fixed, draw-time list merge doesn't guarantee model streaming for props.
//
// Current state: hd_radius > 0 logs a warning and falls back to original behavior.
// Future work requires Ghidra analysis of FUN_0023C820, FUN_002415B0, FUN_0023E748,
// state machine for 4 area slots, then choose strategy (guest window / preload / RSL).
#include "game/vcs_world_hd.h"

#include "game/vcs_memory.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_control.h"
#include "settings/settings.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>

namespace vcs::world_hd
{
    namespace
    {
        // Level object structure (from game-internals.md 9h):
        // +0x0248: current area (being drawn)
        // +0x0250: next area (chosen ahead of camera)
        // +0x0258: another area (cross-fade / other)
        // +0x024C, +0x0254, +0x025C: associated pairs for above areas (NOT extra slots)
        // +0x0260: cross-fade timer
        // +0x0264..+0x0290: streaming state, queue, buffer cursor
        // +0x002C: four real area slots (0xA0 bytes each)
        // +0x0294: piece queue (8 entries), +0x0414: piece queue count
        // Area structure: +0x08 grid x, +0x0C grid y, +0x20 sector list pointer, +0x18 loaded flag

        constexpr uint32_t kLevelPtr = 0x0048F4F8u;       // * -> level object
        constexpr uint32_t kChooseArea = 0x0023C820u;      // FUN_0023C820: chooses next area
        constexpr uint32_t kSwapArea = 0x0023D740u;        // FUN_0023D740: swaps current/next
        constexpr uint32_t kWorldDraw = 0x0023E748u;       // FUN_0023E748: world draw
        constexpr uint32_t kCameraPos = 0x006F4500u;       // TheCamera position (matrix +0x30)
        constexpr uint32_t kCameraFront = 0x006F44E0u;     // TheCamera forward vector
        constexpr uint32_t kFrameCounter = 0x004CD124u;    // CTimer::m_FrameCounter

        // Settings
        std::atomic<int> g_hdRadius{0};         // 0 = original (3x3), 1..6 = extra rings (currently no-op, falls back)
        std::atomic<int> g_hdFocusAhead{50};    // metres ahead of camera for focus (default 50)
        std::atomic<int> g_hdFocusVelocity{150};// velocity multiplier (default 150)

        // Original function pointers
        PS2Runtime::RecompiledFunction g_origChooseArea = nullptr;
        PS2Runtime::RecompiledFunction g_origSwapArea = nullptr;
        PS2Runtime::RecompiledFunction g_origWorldDraw = nullptr;

        // HD state (stubbed - no longer used)
        std::atomic<uint32_t> g_hdAreaCount{0};

        float rdf(uint8_t *rdram, uint32_t addr)
        {
            float f;
            std::memcpy(&f, rdram + (addr & 0x1FFFFFFFu), 4);
            return f;
        }
        uint32_t rd32(uint8_t *rdram, uint32_t addr)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (addr & 0x1FFFFFFFu), 4);
            return v;
        }

        // Hook: FUN_0023C820 - Choose next area
        // Original: chooses one area 50m ahead + 150x velocity
        // HD: currently falls back to original, logs if hd_radius > 0
        PS2_GUEST_CC void chooseAreaHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const int hdRadius = g_hdRadius.load(std::memory_order_relaxed);
            if (hdRadius > 0)
            {
                // Log once per 600 frames to avoid spam
                static std::atomic<uint32_t> s_logCounter{0};
                uint32_t frame = rd32(rdram, kFrameCounter);
                if ((frame - s_logCounter.load()) >= 600)
                {
                    s_logCounter.store(frame);
                    printf("[vcs_world_hd] hd_radius=%d > 0 but HD world streaming is disabled (P0/P1 issues). "
                           "Falling back to original 3x3 cell window. See docs/HD_WORLD_STREAMING_HANDOFF.md\n",
                           hdRadius);
                }
            }

            // Always call original
            if (g_origChooseArea)
                g_origChooseArea(rdram, ctx, runtime);
        }

        // Hook: FUN_0023D740 - Swap area
        // Original: swaps current and next when camera nears next
        // HD: falls back to original
        PS2_GUEST_CC void swapAreaHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_origSwapArea)
                g_origSwapArea(rdram, ctx, runtime);
        }

        // Hook: FUN_0023E748 - World draw
        // Original: walks sector list from current area (+0x248)
        // HD: falls back to original
        PS2_GUEST_CC void worldDrawHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_origWorldDraw)
                g_origWorldDraw(rdram, ctx, runtime);
        }

    } // anonymous namespace

void registerSettings()
{
    settings::Spec s;
    s.key = "world.hd_radius";
    s.page = "Graphics";
    s.section = "Distance and population (experimental)";
    s.label = "HD world distance";
    s.help = "How many extra rings of high-detail cells to stream around the camera. "
             "0 = original (~260m / 3x3 cells). Values 1..6 are currently NO-OP (falls back to original) "
             "due to P0/P1 safety issues documented in HD_WORLD_STREAMING_HANDOFF.md. "
             "Future implementation requires Ghidra analysis of area streaming state machine.";
    s.type = settings::Type::Integer;
    s.min = 0;
    s.max = 6;
    s.format = "%d";
    s.defaultValue = 0;
    s.env = "VCS_HD_RADIUS";
    s.onChange = [](int v) { g_hdRadius.store(std::clamp(v, 0, 6), std::memory_order_relaxed); };
    settings::add(s);

    s = {};
    s.key = "world.hd_focus_ahead";
    s.page = "Graphics";
    s.section = "Distance and population (experimental)";
    s.label = "HD focus ahead";
    s.help = "Metres ahead of the camera to center the HD detail region (default 50). "
             "Currently unused (HD streaming disabled).";
    s.type = settings::Type::Integer;
    s.min = 0;
    s.max = 500;
    s.format = "%d";
    s.defaultValue = 50;
    s.env = "VCS_HD_FOCUS_AHEAD";
    s.onChange = [](int v) { g_hdFocusAhead.store(std::clamp(v, 0, 500), std::memory_order_relaxed); };
    settings::add(s);

    s = {};
    s.key = "world.hd_focus_velocity";
    s.page = "Graphics";
    s.section = "Distance and population (experimental)";
    s.label = "HD focus velocity factor";
    s.help = "How much the player's velocity moves the HD focus ahead (default 150). "
             "Currently unused (HD streaming disabled).";
    s.type = settings::Type::Integer;
    s.min = 0;
    s.max = 500;
    s.format = "%d";
    s.defaultValue = 150;
    s.env = "VCS_HD_FOCUS_VELOCITY";
    s.onChange = [](int v) { g_hdFocusVelocity.store(std::clamp(v, 0, 500), std::memory_order_relaxed); };
    settings::add(s);

    // Option B: Preload Ahead (Phase 3 short-term)
    s = {};
    s.key = "world.preload_ahead";
    s.page = "Graphics";
    s.section = "Distance and population (experimental)";
    s.label = "Preload ahead factor";
    s.help = "Extra bias along velocity vector when choosing next area (0 = off, 1-100). "
             "Increases the '50m ahead + 150*velocity' prediction by velocity * factor. "
             "Requires speed >= world.preload_min_speed. Improves pop-in ahead without "
             "changing the 4-slot architecture. Described as 'preload ahead', not larger radius.";
    s.type = settings::Type::Integer;
    s.min = 0;
    s.max = 100;
    s.format = "%d";
    s.defaultValue = 0;
    s.env = "VCS_PRELOAD_AHEAD";
    s.onChange = [](int v) { 
        // This setting is read by vcs::world::chooseHook in vcs_world.cpp
        // The setting is applied there via g_preloadAhead
    };
    settings::add(s);

    s = {};
    s.key = "world.preload_min_speed";
    s.page = "Graphics";
    s.section = "Distance and population (experimental)";
    s.label = "Preload min speed";
    s.help = "Minimum speed (m/s) to activate preload ahead (default 10 m/s ≈ 36 km/h).";
    s.type = settings::Type::Integer;
    s.min = 0;
    s.max = 100;
    s.format = "%d";
    s.defaultValue = 10;
    s.env = "VCS_PRELOAD_MIN_SPEED";
    s.onChange = [](int v) { 
        // Applied in vcs::world::chooseHook via g_preloadMinSpeed
    };
    settings::add(s);
}

    void install(PS2Runtime &runtime)
    {
        if ((g_origChooseArea = runtime.lookupFunction(kChooseArea)))
            runtime.replaceFunction(kChooseArea, &chooseAreaHook);

        if ((g_origSwapArea = runtime.lookupFunction(kSwapArea)))
            runtime.replaceFunction(kSwapArea, &swapAreaHook);

        if ((g_origWorldDraw = runtime.lookupFunction(kWorldDraw)))
            runtime.replaceFunction(kWorldDraw, &worldDrawHook);

        // Debug command - shows current HD world status
        ps2control::addCommand("hdworld", [](const std::string &args, ps2control::Reply reply) {
            int r = g_hdRadius.load();
            int fa = g_hdFocusAhead.load();
            int fv = g_hdFocusVelocity.load();
            
            char buf[256];
            if (args.empty())
            {
                std::snprintf(buf, sizeof(buf),
                    "hdworld: radius=%d (0=original, 1..6=NO-OP) focus_ahead=%d focus_vel=%d\n"
                    "  HD world streaming is DISABLED (P0/P1 safety). See docs/HD_WORLD_STREAMING_HANDOFF.md",
                    r, fa, fv);
                reply(buf);
                return;
            }
            // parse args like "radius=2 focus_ahead=100"
            char *end = nullptr;
            const char *p = args.c_str();
            while (*p)
            {
                while (*p == ' ') ++p;
                if (!strncmp(p, "radius=", 7))
                {
                    r = std::strtol(p + 7, &end, 10);
                    p = end;
                }
                else if (!strncmp(p, "focus_ahead=", 12))
                {
                    fa = std::strtol(p + 12, &end, 10);
                    p = end;
                }
                else if (!strncmp(p, "focus_vel=", 10))
                {
                    fv = std::strtol(p + 10, &end, 10);
                    p = end;
                }
                else
                {
                    while (*p && *p != ' ') ++p;
                }
            }
            g_hdRadius.store(std::clamp(r, 0, 6));
            g_hdFocusAhead.store(std::clamp(fa, 0, 500));
            g_hdFocusVelocity.store(std::clamp(fv, 0, 500));
            std::snprintf(buf, sizeof(buf), "hdworld: set radius=%d focus_ahead=%d focus_vel=%d (streaming disabled)", r, fa, fv);
            reply(buf);
        });
    }
}