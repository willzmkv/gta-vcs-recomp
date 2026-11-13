// The radio's progress while it isn't heard, put on the game's time step (docs/game-internals.md 9, GitHub issue 117).
//
// The music manager (0x4E41E0) keeps a 12-byte record per stream from +0x18: the stream's length (+0x18), the position
// it was stopped at (+0x1C) and a count (+0x20). Its per-frame service (FUN_00189AD0, and FUN_001896A0 while a
// cutscene's stream holds the music) ends with FUN_0018B668, which adds 1 to the count of all 0x35 streams. Tuning back
// in to a station (FUN_0018B6A8) starts it at stop position + count * 1500 + 100, modulo its length: 1500 of the
// position's units is one 1/30 s frame, so on the PS2 a station went on in real time while you were on foot. At 60 fps
// the count runs twice as fast: getting out of a car and straight back in skipped ahead by the time out once more (by
// 3.8 times that at 144 fps).
//
// The hook replaces FUN_0018B668 (a leaf: no calls, no yields) and adds the frame's step over the PS2's (50/30) to a
// running sum instead of 1, handing the whole frames in it to the counts. Longer frames than the PS2's keep 1, as the
// PS2 did when it fell below 30 fps. Only while the frame isn't the PS2's (independent timing, or original timing at
// 60); VCS_MUSIC_TIMESTEP=0 keeps the game's count.
#include "game/vcs_music_timing.h"

#include "game/vcs_framerate.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vcs
{
    namespace
    {
        constexpr uint32_t kAdvanceCounts = 0x0018B668u; // FUN_0018B668(manager): every stream's count += 1
        constexpr uint32_t kStreams = 0x35u;
        constexpr uint32_t kRecordCount = 0x20u, kRecordSize = 0x0Cu;
        constexpr uint32_t kTimeStep = 0x004CD110u; // CTimer::ms_fTimeStep (1.0 = 1/50 s)
        constexpr float kPs2Step = 50.0f / 30.0f;   // a 30 fps frame

        double g_frames = 0.0; // PS2 frames not yet handed to the counts (one music manager)
        bool g_logged = false;

        bool enabled()
        {
            static const bool on = [] {
                const char *v = std::getenv("VCS_MUSIC_TIMESTEP");
                return !(v && v[0] == '0');
            }();
            return on;
        }

        PS2_GUEST_CC void advanceCounts(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t manager = GPR_U32(ctx, 4);
            uint32_t add = 1;
            if (enabled() && (timing() == Timing::Independent || frameCap() != 30))
            {
                float step;
                std::memcpy(&step, rdram + kTimeStep, sizeof(step));
                const double k = std::isfinite(step) ? std::clamp(static_cast<double>(step) / kPs2Step, 0.0, 1.0) : 1.0;
                g_frames += k;
                add = static_cast<uint32_t>(g_frames);
                g_frames -= add;
                if (!g_logged)
                {
                    g_logged = true;
                    std::fprintf(stderr, "[music-timing] stations off the air advance on the time step (step %.3f, x%.3f)\n",
                                 step, k);
                }
            }
            for (uint32_t i = 0; i < kStreams; ++i)
            {
                uint8_t *count = rdram + ((manager + kRecordCount + i * kRecordSize) & 0x01FFFFFFu);
                uint32_t v;
                std::memcpy(&v, count, sizeof(v));
                v += add;
                std::memcpy(count, &v, sizeof(v));
            }
            ctx->pc = GPR_U32(ctx, 31);
        }
    }

    void installMusicTimingHooks(PS2Runtime &runtime)
    {
        if (!runtime.lookupFunction(kAdvanceCounts))
        {
            std::fprintf(stderr, "[music-timing] 0x%x not found: the radio keeps counting frames\n", kAdvanceCounts);
            return;
        }
        runtime.replaceFunction(kAdvanceCounts, &advanceCounts);
    }
}
