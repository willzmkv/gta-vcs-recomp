// Emergency vehicle siren / lights horn-tap history timing above 30 fps (docs/game-internals.md 4).
//
// CAutomobile::ProcessControl (0x0013A0A0 at 0x13E5EC) and CBike::ProcessControl (0x00337208 at 0x33BF9C) check
// CPad 0's 8-slot horn history ring (pad+0x9A..0xA1, index pad+0xA2, updated once a frame in CPad::Update 0x002843D8):
// - On horn release (hist[cur] == 0 && hist[(cur + 7) & 7] != 0): if hist[(cur + 1) & 7] == 0 (held < 7 PS2 frames,
//   ~233 ms at 30 fps), toggle vehicle flag 0x200000 at veh+0x268 (m_bSirenOrAlarm); otherwise stop horn without toggle.
// - While held (hist[cur] != 0): if hist[(cur + 1) & 7] != 0 (held >= 7 PS2 frames), blow the continuous horn (veh+0x2B4 = 1).
// Above 30 fps (e.g. 48 ms for 7 frames at 144 fps), nearly any human tap exceeds 7 frames and never toggles the lights.
// Here CPad 0's horn history is driven by CTimer's unscaled ms clock (0x00488280), emitting the exact 3-slot pattern at
// the real-time threshold (~230 ms, matching 7 PS2 intervals) on the exact press/release frame without dropping fast taps.
// VCS_SIREN_TIMING=0 leaves CPad's history per frame (the PS2's own).
#include "game/vcs_siren_timing.h"

#include "game/vcs_framerate.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vcs
{
    namespace
    {
        constexpr uint32_t kRealClock = 0x00488280u;      // CTimer::m_snTimeInMillisecondsNonClipped (unscaled ms)
        constexpr uint32_t kPad0 = 0x005147A8u;           // CPad 0
        constexpr uint32_t kHornHistoryOff = 0x9Au;       // m_bHornHistory[8]
        constexpr uint32_t kHornCurOff = 0xA2u;           // m_nCurrentHornHistory (0..7)
        constexpr uint32_t kPadUpdate = 0x002843D8u;      // CPad::Update(pad, index)
        constexpr uint32_t kPadClear = 0x00283788u;       // CPad::Clear(padState)
        constexpr uint32_t kAfterPad0UpdateRa = 0x002843BCu; // CPad::UpdatePads's next call after CPad::Update(pad0, 0)
        constexpr double kPs2FrameMs = 1000.0 * 1001.0 / 30000.0;
        // 7 PS2 frame intervals (~233.6 ms on NTSC PS2, 231-234 ms with ms truncation at 30 fps)
        constexpr double kHoldThresholdMs = 6.9 * kPs2FrameMs;

        PS2Runtime::RecompiledFunction g_origPadUpdate = nullptr, g_origPadClear = nullptr;

        struct SirenPadState
        {
            bool started = false;
            bool down = false;
            bool longHold = false;
            uint8_t prevGuestCur = 0;
            uint32_t lastMs = 0;
            double downMs = 0.0;
        } g_state;

        bool enabled()
        {
            static const bool on = [] {
                const char *v = std::getenv("VCS_SIREN_TIMING");
                return !(v && v[0] == '0');
            }();
            return on;
        }

        bool logOn()
        {
            static const bool on = std::getenv("VCS_SIREN_LOG") != nullptr;
            return on;
        }

        bool active() { return enabled() && (timing() == Timing::Independent || frameCap() != 30); }

        uint32_t readU32(const uint8_t *rdram, uint32_t addr)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (addr & PS2_RAM_MASK), sizeof(v));
            return v;
        }

        PS2_GUEST_CC void padUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (active() && (GPR_U32(ctx, 4) & PS2_RAM_MASK) == kPad0)
                g_state.prevGuestCur = rdram[kPad0 + kHornCurOff];
            return g_origPadUpdate(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void padClear(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (GPR_U32(ctx, 31) == kAfterPad0UpdateRa && active())
            {
                const uint8_t curAfter = rdram[kPad0 + kHornCurOff];
                const uint8_t cur = static_cast<uint8_t>(curAfter & 7u);
                uint8_t *hist = rdram + kPad0 + kHornHistoryOff;
                const uint8_t hornNow = (curAfter != g_state.prevGuestCur && hist[cur] != 0) ? 1u : 0u;

                const uint32_t nowMs = readU32(rdram, kRealClock);
                double dtMs = 0.0;
                if (g_state.started)
                {
                    const uint32_t delta = nowMs - g_state.lastMs;
                    dtMs = delta <= 500u ? static_cast<double>(delta) : 0.0;
                }
                g_state.started = true;
                g_state.lastMs = nowMs;

                const uint8_t prevIdx = static_cast<uint8_t>((cur + 7u) & 7u);
                const uint8_t oldIdx = static_cast<uint8_t>((cur + 1u) & 7u);
                std::memset(hist, 0, 8u);

                if (hornNow)
                {
                    const bool wasDown = g_state.down;
                    if (!wasDown)
                    {
                        g_state.down = true;
                        g_state.longHold = false;
                        g_state.downMs = 0.0;
                        if (logOn())
                            std::fprintf(stderr, "[siren] horn press (ms=%u)\n", nowMs);
                    }
                    else
                    {
                        g_state.downMs += dtMs;
                        if (!g_state.longHold && g_state.downMs >= kHoldThresholdMs)
                        {
                            g_state.longHold = true;
                            if (logOn())
                                std::fprintf(stderr, "[siren] horn hold %.1f ms -> continuous horn\n", g_state.downMs);
                        }
                    }
                    hist[cur] = 1u;
                    if (wasDown)
                        hist[prevIdx] = 1u;
                    if (g_state.longHold)
                        hist[oldIdx] = 1u;
                }
                else if (g_state.down)
                {
                    hist[prevIdx] = 1u;
                    if (g_state.longHold)
                        hist[oldIdx] = 1u;
                    if (logOn())
                        std::fprintf(stderr, "[siren] horn release after %.1f ms -> %s\n", g_state.downMs,
                                     g_state.longHold ? "horn off (no toggle)" : "toggle lights/siren");
                    g_state.down = false;
                    g_state.longHold = false;
                    g_state.downMs = 0.0;
                }
            }
            return g_origPadClear(rdram, ctx, runtime);
        }
    }

    void installSirenTimingHooks(PS2Runtime &runtime)
    {
        if (!enabled())
        {
            std::fprintf(stderr, "[siren-timing] off (VCS_SIREN_TIMING=0): horn history stays per frame\n");
            return;
        }
        g_origPadUpdate = runtime.lookupFunction(kPadUpdate);
        g_origPadClear = runtime.lookupFunction(kPadClear);
        if (!g_origPadUpdate || !g_origPadClear)
        {
            std::fprintf(stderr, "[siren-timing] 0x%x / 0x%x not found: horn history left per frame\n", kPadUpdate, kPadClear);
            return;
        }
        runtime.replaceFunction(kPadUpdate, &padUpdate);
        runtime.replaceFunction(kPadClear, &padClear);
    }
}
