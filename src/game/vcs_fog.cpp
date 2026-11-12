// The game's distance fog as settings (docs/game-internals.md 6t) - the fog of PC shader mods (Proper Shaders, SkyGFX),
// done the game's own way, so it is depth-correct on every surface and keeps the time cycle's colour.
//
// VCS fogs like the PS2's GS: per vertex F = clamp(53.z + 53.w * w, 0, 255) from the camera-space depth w, blended per
// pixel with FOGCOL. The camera's begin-update FUN_0018EE48 queues FOGCOL from 0x4E1B30 (FUN_002F5FB8) and calls
// FUN_002F6008(start f12 = camera +0x80, end f13 = camera +0x7C), a leaf that writes 53.w = -255 / (end - start) and
// 53.z = 255 * (start / (end - start) + 1) (0x742B2C / 0x742B28, so F = 255 (end - w) / (end - start)) and sets dirty bit
// 0x8 when they change. The state flush uploads 0x742AE0 (VU 49-53) and every path reads 53 from there: the VU1
// programs, the CPU HLE, the native vertex shaders (gs_draw.hlsl nativeFog) and the clipper. Scaling the setter's two
// arguments therefore moves the fog everywhere at once; the game's own values go in untouched at 100 %.
//
// VCS_TRACE_FOG=1 logs the distinct (start, end) pairs the game asks for, every 5 s.
#include "game/vcs_fog.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_control.h"
#include "settings/settings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <utility>

namespace vcs::fog
{
    namespace
    {
        constexpr uint32_t kSetFogRange = 0x002F6008u;

        PS2Runtime::RecompiledFunction g_original = nullptr;
        std::atomic<int> g_distance{100}; // percent of the game's end distance (and its start with it)
        std::atomic<int> g_start{100};    // percent of the game's start, on top of the distance

        // the last call, for `fogstate`: the game's (start, end) and what went in
        std::mutex g_lastMutex;
        float g_last[4] = {};
        bool g_trace = false;

        void trace(float start, float end)
        {
            static std::map<std::pair<float, float>, unsigned> seen;
            static auto at = std::chrono::steady_clock::now();
            ++seen[{start, end}];
            if (std::chrono::steady_clock::now() - at < std::chrono::seconds(5))
                return;
            at = std::chrono::steady_clock::now();
            for (const auto &[range, n] : seen)
                std::fprintf(stderr, "[fog] the game's range %.2f .. %.2f: %u calls\n", range.first, range.second, n);
            seen.clear();
        }

        // FUN_002F6008(start f12, end f13): a leaf, so the original runs to the end (DECISIONS.md, hooks)
        PS2_GUEST_CC void setFogRange(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const float start = ctx->f[12], end = ctx->f[13];
            if (g_trace)
                trace(start, end);
            const int distance = g_distance.load(std::memory_order_relaxed), first = g_start.load(std::memory_order_relaxed);
            if ((distance != 100 || first != 100) && end > start)
            {
                const float e = end * (static_cast<float>(distance) / 100.0f);
                const float s = start * (static_cast<float>(distance) / 100.0f) * (static_cast<float>(first) / 100.0f);
                ctx->f[12] = std::min(s, e * 0.95f); // the setter divides by end - start
                ctx->f[13] = e;
            }
            {
                std::lock_guard<std::mutex> lock(g_lastMutex);
                g_last[0] = start;
                g_last[1] = end;
                g_last[2] = ctx->f[12];
                g_last[3] = ctx->f[13];
            }
            g_original(rdram, ctx, runtime);
        }
    }

    void registerSettings()
    {
        using settings::Type;
        settings::Spec s;
        s.key = "fog.distance";
        s.page = "Graphics";
        s.section = "Fog";
        s.label = "Fog distance";
        s.help = "Where the game's distance fog swallows the city, percent of the original (100). Lower is thicker fog, "
                 "closer to the camera, in the time of day's own fog colour.";
        s.type = Type::Integer;
        s.min = 10;
        s.max = 100;
        s.format = "%d%%";
        s.defaultValue = 100;
        s.env = "VCS_FOG_DISTANCE";
        s.onChange = [](int v) { g_distance.store(v, std::memory_order_relaxed); };
        settings::add(s);

        s = {};
        s.key = "fog.start";
        s.page = "Graphics";
        s.section = "Fog";
        s.label = "Fog start";
        s.help = "Where the fog begins to show, percent of the original start (100). Lower brings a light haze right up "
                 "to the camera; higher keeps the near city clear.";
        s.type = Type::Integer;
        s.min = 0;
        s.max = 300;
        s.format = "%d%%";
        s.defaultValue = 100;
        s.env = "VCS_FOG_START";
        s.onChange = [](int v) { g_start.store(v, std::memory_order_relaxed); };
        settings::add(s);
    }

    void install(PS2Runtime &runtime)
    {
        if (const char *e = std::getenv("VCS_FOG_HOOK"); e && *e == '0')
        {
            std::fprintf(stderr, "[fog] the game's fog left alone (VCS_FOG_HOOK=0): the fog settings do nothing\n");
            return;
        }
        g_original = runtime.lookupFunction(kSetFogRange);
        if (!g_original)
        {
            std::fprintf(stderr, "[fog] the fog-range setter 0x%x not found: the fog settings do nothing\n", kSetFogRange);
            return;
        }
        g_trace = std::getenv("VCS_TRACE_FOG") != nullptr;
        runtime.replaceFunction(kSetFogRange, &setFogRange);
        // fogstate -> ok game=<start>,<end> used=<start>,<end> (the last camera update; tools/fog_check.py)
        ps2control::addCommand("fogstate", [](const std::string &, ps2control::Reply reply) {
            float last[4];
            {
                std::lock_guard<std::mutex> lock(g_lastMutex);
                std::copy(g_last, g_last + 4, last);
            }
            char text[128];
            std::snprintf(text, sizeof(text), "ok game=%.3f,%.3f used=%.3f,%.3f", last[0], last[1], last[2], last[3]);
            reply(text);
        });
    }
}
