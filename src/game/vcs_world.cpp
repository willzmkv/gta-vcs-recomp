// The world's streamed areas (game/vcs_world.h).
//
// The level object (*(0x48F4F8), 0x750340) streams the high-detail geometry of areas of 3 x 3 hex cells into four
// slots of 0xA0 bytes at +0x2C (buffers in 0xCB7A80..0x112A660): +0x248 the area being drawn, +0x250 the next one
// (chosen in FUN_0023C820 from the camera, 50 m ahead along its front and 150 x the player's velocity), +0x258 another.
// FUN_002415B0(level, cell x, cell y) requests an area; FUN_002417D0(level, piece) reads one of its pieces
// asynchronously (FUN_002213D0, callback FUN_00241440 -> FUN_00241278, which takes the piece and starts the next);
// FUN_0023D740(level) swaps the next area in when the camera is nearer it than the current one.
#include "game/vcs_world.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace vcs::world
{
    namespace
    {
        constexpr uint32_t kRequestArea = 0x002415B0u, kReadPiece = 0x002417D0u, kPieceDone = 0x00241278u, kSwapArea = 0x0023D740u;
        PS2Runtime::RecompiledFunction g_request = nullptr, g_read = nullptr, g_done = nullptr, g_swap = nullptr;
        const auto g_start = std::chrono::steady_clock::now();

        double now() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_start).count(); }

        // the hooks only log, then run the original (DECISIONS.md, hooks: work before calling it)
        PS2_GUEST_CC void request(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            std::fprintf(stderr, "[areas] %.1f request cell %d,%d (area %d,%d)\n", now(), static_cast<int>(GPR_U32(ctx, 5)),
                         static_cast<int>(GPR_U32(ctx, 6)), static_cast<int>(GPR_U32(ctx, 5)) / 3, static_cast<int>(GPR_U32(ctx, 6)) / 3);
            g_request(rdram, ctx, runtime);
        }
        PS2_GUEST_CC void read(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            std::fprintf(stderr, "[areas] %.1f piece read 0x%08x\n", now(), GPR_U32(ctx, 5));
            g_read(rdram, ctx, runtime);
        }
        PS2_GUEST_CC void done(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            std::fprintf(stderr, "[areas] %.1f piece done\n", now());
            g_done(rdram, ctx, runtime);
        }
        PS2_GUEST_CC void swap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            std::fprintf(stderr, "[areas] %.1f swap\n", now());
            g_swap(rdram, ctx, runtime);
        }
    }

    void install(PS2Runtime &runtime)
    {
        const char *e = std::getenv("VCS_TRACE_AREAS");
        if (!e || *e != '1')
            return;
        struct Hook
        {
            uint32_t at;
            PS2Runtime::RecompiledFunction *original;
            PS2Runtime::RecompiledFunction hook;
        } hooks[] = {{kRequestArea, &g_request, &request}, {kReadPiece, &g_read, &read}, {kPieceDone, &g_done, &done}, {kSwapArea, &g_swap, &swap}};
        for (const Hook &h : hooks)
            if ((*h.original = runtime.lookupFunction(h.at)))
                runtime.replaceFunction(h.at, h.hook);
        std::fprintf(stderr, "[areas] tracing the world's area streaming\n");
    }
}
