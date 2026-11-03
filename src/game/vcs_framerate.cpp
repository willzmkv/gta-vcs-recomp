// VCS frame rate and game timing (docs/NATIVE_PORT_ROADMAP.md milestone 4; docs/game-internals.md 4).
//
// The game paces itself on the VBlank: a VBlank-start handler (FUN_00370300) runs the registered VBlank callbacks,
// one of which (FUN_00317aa8 -> FUN_00317ac8) counts VBlanks in the graphics object at 0x520F90 (+0x4). At the end of
// a frame FUN_003179a0 kicks the display list, waits `do { WaitVBlank(); } while (vblanks < 2);` and flips
// (FUN_003706c8): 30 fps. CTimer::Update (FUN_002f4960) measures the frame in bus-clock ticks read from a kernel timer
// (FUN_00451c00, its only caller), so the time step follows the measured frame time - in emulated time.
//
// Original timing: as on the PS2; a cap of 60 or more makes the counting callback add 2 per VBlank, so the limiter
// lets a frame through after one (the 60 fps mode).
//
// Independent timing:
// - the tick count CTimer reads is the host clock (147.456 MHz units) - or exactly 1/N s per frame with
//   VCS_TIMESTEP=<N> (tests: behaviour per simulated second at any step, game unpaced);
// - CTimer's minimum step of 0.5 (1/100 s) is lifted to 50/240 (kMaxFps): above 100 fps the clamped step ran the
//   game fast (the ped movement that broke below 0.5 is patched - minStep below);
// - the limiter's VBlank wait paces on the host clock to the frame-rate cap instead, brings the emulated clock up to
//   real time (VBlank events, timers and the IOP follow it) and presents the frame (runtime/ps2_present.h).
#include "game/vcs_framerate.h"

#include "platform/platform.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "runtime/gs/gs_hw.h"
#include "runtime/ps2_perf.h"
#include "runtime/ps2_present.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace vcs
{
    namespace
    {
        constexpr uint32_t kCountVBlank = 0x00317AC8u; // (graphics object): +0x4 += 1, from a VBlank callback
        constexpr uint32_t kTimerTicks = 0x00451C00u;  // kernel timer count (64-bit), read by CTimer::Update only
        constexpr uint32_t kTimerEnsure = 0x002F47C0u; // CTimer: create the kernel timer if needed (2 calls in Update)
        constexpr uint32_t kWaitVBlank = 0x003703B8u;
        constexpr uint32_t kAfterMinClamp = 0x002F4C04u; // return address of Update's 2nd kTimerEnsure call
        constexpr uint32_t kLimiterWait = 0x00317A50u;   // return address of the limiter's VBlank wait
        constexpr uint32_t kTimerUpdate = 0x002F4960u;   // CTimer::Update
        constexpr uint32_t kTwoDPass = 0x0021F348u;      // the frame's 2D pass: radar, HUD, text (game-internals.md 6f)
        constexpr uint32_t kFrameCounter = 0x004CD124u;  // CTimer::m_FrameCounter
        constexpr uint32_t kTimeMs = 0x004CD104u;        // CTimer::m_snTimeInMilliseconds

        constexpr uint32_t kPrevTicks = 0x00488298u;  // CTimer: tick count of the last Update (64-bit)
        constexpr uint32_t kFrameTicks = 0x004CD564u; // CTimer: this frame's ticks
        constexpr uint32_t kTimeScale = 0x004CD168u;  // CTimer::ms_fTimeScale
        constexpr uint32_t kTimeStep = 0x004CD110u;   // CTimer::ms_fTimeStep (1.0 = 1/50 s)
        constexpr uint32_t kTimeStepNonClipped = 0x004CD300u;

        constexpr uint64_t kBusHz = 147456000ull;     // the tick rate (CTimer: ms = ticks / 147456)
        constexpr double kEeCyclesPerNs = 0.294912;   // EE clock 294.912 MHz
        // Independent timing's frame-rate ceiling and the step floor that goes with it (CTimer's step: 1.0 = 1/50 s).
        // The game floors the step at 0.5 (100 fps); below that peds ran at a quarter of their speed - FUN_001f1d38
        // turned animation movement into a velocity with `* 0.5` instead of `/ step` there, a branch dead on the PS2
        // (patched in game/vcs/overrides.toml, 2026-10-25). With that fixed the audit (tools/framerate_audit.py) runs,
        // sprints, drives and falls at 1/144 - 1/240 s as at 1/30 - 1/100 (sprint -5% / -11% at 1/200 / 1/240), so the
        // ceiling is 240 fps (it was 100). Faster frames get the floor (the game runs a little fast). VCS_MIN_STEP=<x>
        // lowers the floor further for experiments (then an unlimited cap is unlimited).
        constexpr double kMaxFps = 240.0;
        constexpr float kDefaultMinStep = static_cast<float>(50.0 / kMaxFps);
        float minStep()
        {
            static const float step = [] {
                const char *v = std::getenv("VCS_MIN_STEP");
                return v && *v ? static_cast<float>(std::atof(v)) : kDefaultMinStep;
            }();
            return step;
        }

        std::atomic<int> g_timing{static_cast<int>(Timing::Original)};
        std::atomic<int> g_cap{30};
        PS2Runtime::RecompiledFunction g_origTicks = nullptr, g_origEnsure = nullptr, g_origWait = nullptr, g_origTwoD = nullptr,
                                           g_origUpdate = nullptr;
        bool g_counterSwapped = false; // the 2D pass runs on the 30 Hz counter (game thread)
        uint32_t g_realCounter = 0;
        uint64_t g_lastSleepNs = 0; // the limiter's last host wait (VCS_TIMING_LOG)

        bool independent() { return g_timing.load(std::memory_order_relaxed) == static_cast<int>(Timing::Independent); }

        uint64_t hostNs()
        {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        }

        // the cap in frames per second: display refresh and unlimited included, at most kMaxFps unless the floor is
        // lowered (then unlimited is unlimited)
        double effectiveFps()
        {
            const int cap = g_cap.load(std::memory_order_relaxed);
            double fps = cap == kCapRefresh ? platform::displayRefreshHz() : cap == kCapUnlimited ? 0.0 : cap;
            const double limit = minStep() < kDefaultMinStep ? 0.0 : kMaxFps;
            if (limit > 0.0 && (fps <= 0.0 || fps > limit))
                fps = limit;
            return fps;
        }

        uint32_t forcedStepFps()
        {
            static const uint32_t fps = [] {
                const char *v = std::getenv("VCS_TIMESTEP");
                const int n = v ? std::atoi(v) : 0;
                if (n > 0)
                    std::fprintf(stderr, "[framerate] forced time step: 1/%d s per frame, unpaced\n", n);
                return static_cast<uint32_t>(n > 0 ? n : 0);
            }();
            return fps;
        }

        uint32_t read32(uint8_t *rdram, uint32_t address)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (address & PS2_RAM_MASK), 4);
            return v;
        }
        void write32(uint8_t *rdram, uint32_t address, uint32_t v) { std::memcpy(rdram + (address & PS2_RAM_MASK), &v, 4); }
        float readF(uint8_t *rdram, uint32_t address)
        {
            float f;
            std::memcpy(&f, rdram + (address & PS2_RAM_MASK), 4);
            return f;
        }
        void writeF(uint8_t *rdram, uint32_t address, float f) { std::memcpy(rdram + (address & PS2_RAM_MASK), &f, 4); }

        // replaces FUN_00317ac8 (a leaf: lw / addiu / sw / jr)
        PS2_GUEST_CC void countVBlank(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t at = GPR_U32(ctx, 4) + 4u;
            const bool sixty = !independent() && g_cap.load(std::memory_order_relaxed) != 30;
            write32(rdram, at, read32(rdram, at) + (sixty ? 2u : 1u));
            ctx->pc = GPR_U32(ctx, 31);
        }

        // FUN_00451c00 (independent timing): the tick count from the host clock, continuing CTimer's last count
        PS2_GUEST_CC void timerTicks(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!independent())
            {
                g_origTicks(rdram, ctx, runtime);
                return;
            }
            static bool started = false;
            static uint64_t base = 0, originNs = 0, calls = 0;
            if (!started)
            {
                std::memcpy(&base, rdram + kPrevTicks, 8);
                originNs = hostNs();
                started = true;
            }
            uint64_t elapsed; // ticks since the start of independent timing
            if (const uint32_t fps = forcedStepFps())
                elapsed = ++calls * kBusHz / fps;
            else
            {
                const uint64_t ns = hostNs() - originNs;
                elapsed = (ns / 1000000000ull) * kBusHz + (ns % 1000000000ull) * kBusHz / 1000000000ull;
            }
            // Whole milliseconds, the rest carried to the next frame: CTimer adds each frame's milliseconds truncated
            // to its clock (m_snTimeInMilliseconds), which lost up to 1 ms a frame - 1.5% slow at 30 fps, ~5% at 100
            // (game-internals.md 4). Whole-ms deltas convert back exactly (checked for 1-399 ms).
            constexpr uint64_t kTicksPerMs = kBusHz / 1000u;
            SET_GPR_U64(ctx, 2, base + elapsed / kTicksPerMs * kTicksPerMs);
            ctx->pc = GPR_U32(ctx, 31);
        }

        // FUN_002f47c0; on Update's second call (independent timing) its minimum step is lifted first: the step was
        // just computed and raised to 0.5 when below; recompute it from the frame's ticks as Update does
        // (ticks * scale * 50/147456000) and put it back, floored at kMinStep. The maximum clamp follows the call.
        PS2_GUEST_CC void timerEnsure(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (independent() && GPR_U32(ctx, 31) == kAfterMinClamp && readF(rdram, kTimeStep) == 0.5f)
            {
                const int32_t ticks = static_cast<int32_t>(read32(rdram, kFrameTicks));
                const float t = ticks >= 0 ? static_cast<float>(ticks) : static_cast<float>(static_cast<uint32_t>(ticks)); // Update's cvt / unsigned path
                uint32_t kBits = 0x34B60B61u; // 50 / 147456000 as the game has it
                float k;
                std::memcpy(&k, &kBits, 4);
                const float step = t * readF(rdram, kTimeScale) * k;
                if (step < 0.5f)
                {
                    const float lifted = std::max(step, minStep());
                    writeF(rdram, kTimeStep, lifted);
                    writeF(rdram, kTimeStepNonClipped, lifted);
                }
            }
            g_origEnsure(rdram, ctx, runtime);
        }

        // The end of a game frame in independent timing (the limiter's VBlank wait): present it, wait for the cap's
        // deadline on the host clock, bring the emulated clock up to real time, and end the limiter's wait.
        PS2_GUEST_CC void limiterWait(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ps2PresentGameFrame(); // the frame's display list was kicked just before

            // at a cap of the host's rate (or none) the main thread's frame start starts the next frame (fresh input, ready
            // for the next host frame: runtime/ps2_present.h); below it (30) the game's own deadline paces it
            static uint64_t deadline = 0;
            const double fps = effectiveFps();
            if (ps2WaitHostTick())
                deadline = 0;
            else if (fps > 0.0 && !forcedStepFps())
            {
                const uint64_t period = static_cast<uint64_t>(1e9 / fps);
                const uint64_t now = hostNs();
                deadline = deadline && deadline + period > now ? deadline + period : now; // behind: no catching up
                g_lastSleepNs = deadline > now ? deadline - now : 0u;
                if (deadline > now)
                {
                    ps2perf::PhaseScope waitTime(ps2perf::Phase::Wait); // the perf line's wait_ms, not game_ms
                    platform::delayPrecise(deadline - now);
                }
            }

            // emulated time follows real time (it only advances as guest code runs otherwise): up to where real time is
            // on the VBlank chain - never past it, or the EE is held at every VBlank (runtime/ee_scheduler.h)
            static uint64_t lastNs = 0, lastCycle = 0;
            EeScheduler &scheduler = runtime->eeScheduler();
            // guest work charged at a quarter of its estimated cycles: topped up to real time here anyway, and at the
            // full estimate the clock ran ahead of real time above ~100 fps, holding the EE at every VBlank until real
            // time caught up (~3 ms a frame). VCS_EE_CHARGE_SHIFT=<n>: 1/2^n (0 = as in original timing).
            static const uint32_t chargeShift = [] {
                const char *v = std::getenv("VCS_EE_CHARGE_SHIFT");
                return v && *v ? static_cast<uint32_t>(std::atoi(v)) : 2u;
            }();
            scheduler.setGuestChargeShift(chargeShift);
            const uint64_t now = hostNs();
            scheduler.syncToRealTime(static_cast<uint64_t>(200e6 * kEeCyclesPerNs)); // a longer hitch is forgiven
            { // VCS_TIMING_LOG=1: frames, sleep, emulated-clock top-up, every 2 s
                static const bool log = std::getenv("VCS_TIMING_LOG") != nullptr;
                static uint64_t logStart = 0, frames = 0, toppedCycles = 0;
                if (log)
                {
                    ++frames;
                    if (!logStart)
                        logStart = now;
                    if (now - logStart > 2000000000ull)
                    {
                        std::fprintf(stderr, "[timing] %.1f fps, ee clock %.3f of real time, last frame slept %.1f ms\n",
                                     frames / ((now - logStart) / 1e9), toppedCycles / ((now - logStart) * kEeCyclesPerNs),
                                     g_lastSleepNs / 1e6);
                        logStart = now;
                        frames = 0;
                        toppedCycles = 0;
                    }
                    toppedCycles += lastNs ? scheduler.fastClock().cycle - lastCycle : 0u;
                }
            }
            lastNs = now;
            lastCycle = scheduler.fastClock().cycle;

            write32(rdram, GPR_U32(ctx, 18) + 4u, 2u); // the limiter's VBlank count ($s2 = the graphics object): done
            ctx->pc = GPR_U32(ctx, 31);
        }

        // FUN_0021f348, the 2D pass: the HUD's flashing (CHud: `m_FrameCounter & 8` for a flashing item, `& 4` for
        // health under 10, more `& 2 / 4 / 8` toggles - game-internals.md 4) counts frames, so it flashed twice as fast
        // at 60 fps. Above 30 fps the pass sees a counter whose low bits advance 30 times a game second (game clock);
        // the real one is put back as CTimer::Update starts (timerUpdate), which counts the next frame on it.
        PS2_GUEST_CC void twoDPass(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const bool fast = independent() ? effectiveFps() != 30.0 : g_cap.load(std::memory_order_relaxed) != 30;
            if (fast && !g_counterSwapped)
            {
                g_realCounter = read32(rdram, kFrameCounter);
                // only the low bits the flashing tests (& 2 / 4 / 8 ..) look at: the value stays within 63 of the real
                // one for any other reader before the next Update (frame stamps compared with `counter - 1`)
                const uint32_t at30 = static_cast<uint32_t>(static_cast<uint64_t>(read32(rdram, kTimeMs)) * 3u / 100u);
                write32(rdram, kFrameCounter, (g_realCounter & ~0x3Fu) | (at30 & 0x3Fu));
                g_counterSwapped = true;
            }
            g_origTwoD(rdram, ctx, runtime);
        }

        // FUN_002f4960 = CTimer::Update: the real frame counter back first (twoDPass), then the original
        PS2_GUEST_CC void timerUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_counterSwapped)
            {
                write32(rdram, kFrameCounter, g_realCounter);
                g_counterSwapped = false;
            }
            g_origUpdate(rdram, ctx, runtime);
        }

        // FUN_003703b8 = WaitVBlank
        PS2_GUEST_CC void waitVBlank(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            { // VCS_TIMING_LOG=1: WaitVBlank calls by return address, every 2 s
                static const bool log = std::getenv("VCS_TIMING_LOG") != nullptr;
                static std::unordered_map<uint32_t, uint32_t> byCaller;
                static uint64_t since = 0;
                if (log)
                {
                    ++byCaller[GPR_U32(ctx, 31)];
                    const uint64_t now = hostNs();
                    if (!since)
                        since = now;
                    if (now - since > 2000000000ull)
                    {
                        std::fprintf(stderr, "[timing] WaitVBlank callers:");
                        for (const auto &[ra, n] : byCaller)
                            std::fprintf(stderr, " %08x x%u", ra, n);
                        std::fprintf(stderr, "\n");
                        byCaller.clear();
                        since = now;
                    }
                }
            }
            if (independent() && GPR_U32(ctx, 31) == kLimiterWait)
            {
                limiterWait(rdram, ctx, runtime);
                return;
            }
            g_origWait(rdram, ctx, runtime);
        }

        // the main loop's rate: it executes the frames the game queued (at most 3 ahead, runtime/gs/gs_hw.h) and presents
        // the last. Unlimited paces it at the platform's maximum: at 60 the queue held the game to 180 fps.
        void applyPresentRate()
        {
            const double fps = effectiveFps();
            platform::setFrameRate(independent() ? (fps > 0.0 ? std::max(60.0, fps) : 1000.0) : 60.0);
            // the host runs at the cap: its frames can pace the game's (docs/PERFORMANCE.md 19)
            ps2SetHostPacedFrames(independent() && (fps <= 0.0 || fps >= 60.0) && !forcedStepFps());
        }
    }

    void setTiming(Timing timing)
    {
        g_timing.store(static_cast<int>(timing), std::memory_order_relaxed);
        ps2SetPresentOnGameFrames(timing == Timing::Independent);
        gsHwSetShowEveryFrame(timing == Timing::Independent); // game time follows the clock: show every frame rendered
        applyPresentRate();
    }

    Timing timing() { return static_cast<Timing>(g_timing.load(std::memory_order_relaxed)); }

    void setFrameCap(int fps)
    {
        g_cap.store(fps, std::memory_order_relaxed);
        applyPresentRate();
    }

    int frameCap() { return g_cap.load(std::memory_order_relaxed); }

    void setFrameRate60(bool on) { setFrameCap(on ? 60 : 30); }
    bool frameRate60() { return frameCap() != 30; }

    void installFrameRateHooks(PS2Runtime &runtime)
    {
        g_origTicks = runtime.lookupFunction(kTimerTicks);
        g_origEnsure = runtime.lookupFunction(kTimerEnsure);
        g_origWait = runtime.lookupFunction(kWaitVBlank);
        g_origTwoD = runtime.lookupFunction(kTwoDPass);
        g_origUpdate = runtime.lookupFunction(kTimerUpdate);
        runtime.replaceFunction(kCountVBlank, &countVBlank);
        if (g_origTwoD && g_origUpdate)
        {
            runtime.replaceFunction(kTwoDPass, &twoDPass);
            runtime.replaceFunction(kTimerUpdate, &timerUpdate);
        }
        if (!g_origTicks || !g_origEnsure || !g_origWait)
        {
            std::fprintf(stderr, "[framerate] CTimer / WaitVBlank not found: independent timing unavailable\n");
            g_timing.store(static_cast<int>(Timing::Original), std::memory_order_relaxed);
            return;
        }
        runtime.replaceFunction(kTimerTicks, &timerTicks);
        runtime.replaceFunction(kTimerEnsure, &timerEnsure);
        runtime.replaceFunction(kWaitVBlank, &waitVBlank);
    }
}
