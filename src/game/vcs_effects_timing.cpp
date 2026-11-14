// Visual effects that count frames, put on the PS2's 30 frames a second (docs/game-internals.md 4, "Effects that run
// per frame"; particles are vcs_particle_timing.cpp).
//
// The PS2 ran these once per 1/30 s. Above that they ran faster, flickered more or ended sooner. Two tools:
// - a PS2 frame "tick": true on the frames where another 1/30 s has passed on the real-time clock (CTimer's unscaled
//   ms clock 0x488280, so slow motion and the photo's frozen time don't stop it, as they did not stop the PS2's frames);
//   what the game re-rolls or steps once a frame does so only on ticks;
// - the frame's share of a PS2 frame, for what moves smoothly.
// Both are 1 / always at 30 fps and below, and in original timing at 30: the game's own behaviour.
//
// 1. Lightning (CWeather::Update FUN_003E3720): a burst starts on `(u16)rand() < 200` and ends on `(u8)rand() < 0x20`,
//    rolled every frame - 4.8x as many bursts, each 1/4.8 as long, at 144 fps. The two rolls (rand's return addresses
//    0x3E39DC / 0x3E3924) only count on ticks. The thunder's delay and loudness come from the burst's length in frames
//    (`0x4CD640` = min(frames, 20), thunder at end + (20 - n) x 150 ms, `0x4CD634`): set to its length in PS2 frames.
// 2. Sun glare (same function, 0x3E3F40): `*= 1 - (rand & 31) x 0.007` every frame - the roll is held between ticks.
// 3. Clouds (CClouds::Update FUN_0018CD88): the rotation `0x4CD2B0` gets sin(heading - 0.85) x wind x 0.001 a frame and
//    `0x4CD2B4` a constant 18 a frame besides its step term: both scaled by the frame's share.
// 4. Camera shake (CCamera::Process, rand at 0x37ACC8): a new random offset every frame - held between ticks.
// 5. Video-cam overlay and the photo flash (CSpecialFX::Render2DFXs FUN_001676C8): the scanline offset `0x481D64` flips
//    every frame and 200 noise dashes are re-rolled (0x167970 / 0x16798C, held); the snapshot's counter `0x481CF4` +1 a
//    frame (white for 9, time stopped for 20): flip and counter undone on frames between ticks.
// 6. The HUD bar flash (FUN_002AF6E0 state 4): its count (+2) drops on frames with `(frame & 8) == 0` - only on ticks.
// 7. Peds fading out / in (CPed::ProcessControl, alpha -8 / +16 a frame after FUN_0036ED68 at 0x1F4CFC): the alpha it
//    reads is moved back by that frame's step between ticks.
// 8. Melee swing streaks (CMotionBlurStreak::Update FUN_00169F88, its 4-place history shifted every frame): only on
//    ticks; the newest place still follows the weapon every frame.
// 9. Rubbish (CRubbish::Update FUN_0036AFD0): its visibility `0x4CD5B4` +-5 a frame (scaled), a sheet set blowing on
//    `(frame & mask) == 0` (mask by wind, 0 in strong wind): between ticks rand's pick (0x36B54C) names a sheet that is
//    not lying ready, so none starts.
// 10. Steam after rain (CWeather::AddRain FUN_003E47B8): 800 frames counted down after each FUN_003E4270 call - the
//    count is put back between ticks.
// 11. Explosions (CExplosion::Update FUN_002E23A0): the step count that drives their bursts advances at 30 a second
//    per explosion, the bursts pass on fresh steps only, the coronas' `frame & 1` reads the PS2 frames' parity (see 11.).
// 12. The banknote spray (FUN_0032FD78): runs on ticks only, with the steps since the last one.
//
// rand (FUN_00408058, Park-Miller: seed 0x48A5B8 x 16807 mod 2^31 - 1) is replaced by the same computation, which
// does what is said above for the listed return addresses only.
// VCS_EFFECTS_TIMING=0 leaves all of it per frame; VCS_EFFECTS_LOG=1 prints lightning bursts and cloud rotation every
// 30 s of real time.
#include "game/vcs_effects_timing.h"

#include "game/vcs_framerate.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace vcs
{
    namespace
    {
        constexpr uint32_t kRealClock = 0x00488280u;   // CTimer: ms, unscaled, also while paused
        constexpr uint32_t kGameClock = 0x004CD104u;   // CTimer::m_snTimeInMilliseconds
        constexpr uint32_t kFrameCounter = 0x004CD124u;
        constexpr float kPs2FrameMs = 1000.0f * 1001.0f / 30000.0f; // two NTSC fields

        constexpr uint32_t kRand = 0x00408058u, kSeed = 0x0048A5B8u;
        constexpr uint32_t kWeatherUpdate = 0x003E3720u, kCloudsUpdate = 0x0018CD88u, kRender2DFx = 0x001676C8u;
        constexpr uint32_t kHudFade = 0x002AF6E0u, kGetAlpha = 0x0036ED68u, kStreakUpdate = 0x00169F88u;
        constexpr uint32_t kRubbishUpdate = 0x0036AFD0u, kRainSteam = 0x003E4270u;

        // rand's callers handled here (return addresses)
        constexpr uint32_t kLightningEnd = 0x003E3924u, kLightningStart = 0x003E39DCu, kSunGlare = 0x003E3F40u;
        constexpr uint32_t kCameraShake = 0x0037ACC8u, kVideoDashX = 0x00167970u, kVideoDashY = 0x0016798Cu;
        constexpr uint32_t kRubbishPick = 0x0036B54Cu;
        constexpr uint32_t kPedAlphaRead = 0x001F4CFCu;

        PS2Runtime::RecompiledFunction g_origWeather = nullptr, g_origClouds = nullptr, g_orig2DFx = nullptr,
                                       g_origHudFade = nullptr, g_origStreak = nullptr, g_origRubbish = nullptr,
                                       g_origSteam = nullptr;

        bool enabled()
        {
            static const bool on = [] {
                const char *v = std::getenv("VCS_EFFECTS_TIMING");
                return !(v && v[0] == '0');
            }();
            return on;
        }
        bool logOn()
        {
            static const bool on = std::getenv("VCS_EFFECTS_LOG") != nullptr;
            return on;
        }

        template <typename T>
        T rd(const uint8_t *rdram, uint32_t addr)
        {
            T v;
            std::memcpy(&v, rdram + (addr & 0x1FFFFFFu), sizeof(T));
            return v;
        }
        template <typename T>
        void wr(uint8_t *rdram, uint32_t addr, T v)
        {
            std::memcpy(rdram + (addr & 0x1FFFFFFu), &v, sizeof(T));
        }

        // ---- the PS2 frame clock -------------------------------------------------------------------------------------
        struct Clock
        {
            uint32_t lastMs = 0;
            bool started = false;
            float carry = 0.0f;     // ms since the last tick
            float share = 1.0f;     // this frame's share of a PS2 frame (1 at 30 fps and below)
            bool tick = true;       // a PS2 frame has passed with this frame
            uint64_t seq = 0;       // frames seen
            uint64_t ticks = 0;
            float dt = kPs2FrameMs; // this frame, ms (a PS2 frame's when the fix is off)
            float stepSum = 0.0f;   // the game's time steps since the last tick
            float tickStep = 0.0f;  // on a tick: the steps since the one before (about 50/30)
        } g_clock;

        // updated once a frame (the first caller after the real-time clock moved)
        const Clock &clock(const uint8_t *rdram)
        {
            Clock &c = g_clock;
            const uint32_t now = rd<uint32_t>(rdram, kRealClock);
            if (c.started && now == c.lastMs)
                return c;
            const bool active = enabled() && (timing() == Timing::Independent || frameCap() != 30);
            const float dt = c.started ? static_cast<float>(now - c.lastMs) : kPs2FrameMs;
            c.started = true;
            c.lastMs = now;
            ++c.seq;
            if (!active || dt >= kPs2FrameMs * 0.9f || dt > 1000.0f)
            {
                c.carry = 0.0f;
                c.share = 1.0f;
                c.tick = true;
            }
            else
            {
                c.share = dt / kPs2FrameMs;
                c.carry += dt;
                // a frame about as long as the PS2's ticks on its own; the rest carries, so ticks come 30 times a second
                c.tick = c.carry >= kPs2FrameMs * 0.9f;
                if (c.tick)
                    c.carry = std::clamp(c.carry - kPs2FrameMs, -0.1f * kPs2FrameMs, kPs2FrameMs);
            }
            c.ticks += c.tick ? 1u : 0u;
            c.dt = c.share < 1.0f ? dt : kPs2FrameMs;
            c.stepSum += rd<float>(rdram, 0x004CD110u);
            if (c.tick)
            {
                c.tickStep = c.stepSum;
                c.stepSum = 0.0f;
            }
            return c;
        }

        // a per-frame integer step scaled by the share, the fraction carried
        int dither(float amount, float &acc)
        {
            const float want = amount + acc;
            const float whole = std::trunc(want);
            acc = want - whole;
            return static_cast<int>(whole);
        }

        // ---- rand ----------------------------------------------------------------------------------------------------
        int32_t nextRand(uint8_t *rdram)
        {
            const int32_t seed = rd<int32_t>(rdram, kSeed);
            const int32_t hi = seed % 127773, lo = seed / 127773; // div: quotient lo, remainder hi
            int32_t v = static_cast<int32_t>(static_cast<uint32_t>(hi) * 16807u - static_cast<uint32_t>(lo) * 2836u);
            if (v <= 0)
                v = static_cast<int32_t>(static_cast<uint32_t>(v) + 0x7FFFFFFFu);
            wr<int32_t>(rdram, kSeed, v);
            return v;
        }

        // the values a site rolled on the last tick, replayed in order until the next
        struct Held
        {
            uint32_t ra;
            uint64_t seq = ~0ull;
            size_t next = 0;
            std::vector<int32_t> values;
        };
        Held g_held[] = {{kSunGlare}, {kCameraShake}, {kVideoDashX}, {kVideoDashY}};

        int32_t heldRand(uint8_t *rdram, Held &h, const Clock &c)
        {
            if (h.seq != c.seq)
            {
                h.seq = c.seq;
                h.next = 0;
                if (c.tick)
                    h.values.clear();
            }
            if (c.tick)
            {
                const int32_t v = nextRand(rdram);
                h.values.push_back(v);
                return v;
            }
            return h.next < h.values.size() ? h.values[h.next++] : nextRand(rdram);
        }

        // VCS_EFFECTS_LOG: the steps / rolls that took effect, per second (30 a second each at any frame rate, while the
        // effect runs)
        enum Count
        {
            LightningRolls,
            GlareRolls,
            ShakeRolls,
            DashRolls,
            PhotoSteps,
            VideoFlips,
            HudDrops,
            PedFadeSteps,
            StreakShifts,
            RubbishPicks,
            SteamSteps,
            CountN
        };
        const char *const kCountNames[CountN] = {"lightning rolls", "glare rolls", "shake rolls", "dash rolls", "photo steps",
                                                 "video flips", "hud flash drops", "ped fade steps", "streak shifts",
                                                 "rubbish picks", "steam steps"};
        uint64_t g_counts[CountN] = {};

        struct LightningLog
        {
            uint32_t bursts = 0;
            double burstMs = 0.0;
        } g_lightLog;

        PS2_GUEST_CC void rand(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            (void)runtime;
            const uint32_t ra = GPR_U32(ctx, 31);
            int32_t v;
            const bool special = (enabled() || logOn()) && (ra == kLightningEnd || ra == kLightningStart || ra == kSunGlare ||
                                               ra == kCameraShake || ra == kVideoDashX || ra == kVideoDashY ||
                                               ra == kRubbishPick);
            if (!special)
                v = nextRand(rdram);
            else
            {
                const Clock &c = clock(rdram);
                if (c.tick)
                    ++g_counts[ra == kLightningStart || ra == kLightningEnd ? LightningRolls
                               : ra == kSunGlare                            ? GlareRolls
                               : ra == kCameraShake                         ? ShakeRolls
                               : ra == kRubbishPick                         ? RubbishPicks
                                                                            : DashRolls];
                if (ra == kLightningStart)
                    v = c.tick ? nextRand(rdram) : 0xFFFF; // >= 200: no burst starts
                else if (ra == kLightningEnd)
                    v = c.tick ? nextRand(rdram) : 0xFF;   // >= 0x20: the burst goes on
                else if (ra == kRubbishPick)
                {
                    v = nextRand(rdram);
                    if (!c.tick) // a sheet that is not lying ready (state 1): none starts blowing
                        for (int32_t i = 0; i < 32; ++i)
                            if (rd<uint8_t>(rdram, 0x00746D1Cu + static_cast<uint32_t>(i) * 0x48u) != 1u)
                            {
                                v = (v & ~0x1F) | i;
                                break;
                            }
                }
                else
                {
                    Held *h = std::find_if(std::begin(g_held), std::end(g_held), [ra](const Held &x) { return x.ra == ra; });
                    v = heldRand(rdram, *h, c);
                }
            }
            SET_GPR_S32(ctx, 2, v);
            ctx->pc = ra;
        }

        // ---- 1. lightning's thunder ------------------------------------------------------------------------------------
        struct Lightning
        {
            bool on = false;
            uint64_t startTicks = 0;
            uint32_t startMs = 0;
        } g_lightning;

        void logEffects(uint8_t *rdram);

        PS2_GUEST_CC void weatherUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (enabled() || logOn())
            {
                const Clock &c = clock(rdram);
                const bool on = rd<int32_t>(rdram, 0x004CD638u) != 0;
                Lightning &l = g_lightning;
                if (on && !l.on) // a burst started in the last Update
                {
                    l.startTicks = c.ticks - (c.tick ? 1u : 0u);
                    l.startMs = rd<uint32_t>(rdram, kRealClock);
                }
                else if (!on && l.on) // ... and ended: its length in PS2 frames, as the PS2 counted it
                {
                    const uint32_t thunder = rd<uint32_t>(rdram, 0x004CD634u);
                    const int32_t frames = rd<int32_t>(rdram, 0x004CD640u);
                    const int32_t ps2 = static_cast<int32_t>(std::min<uint64_t>(c.ticks - (c.tick ? 1u : 0u) - l.startTicks, 20u));
                    if (enabled() && thunder && frames >= 0 && frames <= 20 && c.share < 1.0f)
                    {
                        const uint32_t endMs = thunder - static_cast<uint32_t>(20 - frames) * 150u;
                        wr<int32_t>(rdram, 0x004CD640u, ps2);
                        wr<uint32_t>(rdram, 0x004CD634u, endMs + static_cast<uint32_t>(20 - ps2) * 150u);
                    }
                    ++g_lightLog.bursts;
                    g_lightLog.burstMs += rd<uint32_t>(rdram, kRealClock) - l.startMs;
                }
                l.on = on;
            }
            logEffects(rdram);
            g_origWeather(rdram, ctx, runtime);
        }

        // ---- 3. clouds -------------------------------------------------------------------------------------------------
        float g_cloudAcc = 0.0f;
        double g_cloudTurn = 0.0, g_cloudSpin = 0.0; // VCS_EFFECTS_LOG: what the frames added

        PS2_GUEST_CC void cloudsUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const Clock &c = clock(rdram);
            const float wind = rd<float>(rdram, 0x004CD2A4u), heading = rd<float>(rdram, 0x006F4CB8u);
            const float turn = std::sin(heading - 0.85f) * wind * 0.001f;
            const float step = rd<float>(rdram, 0x004CD110u);
            if (c.share < 1.0f)
            {
                wr<float>(rdram, 0x004CD2B0u, rd<float>(rdram, 0x004CD2B0u) - turn * (1.0f - c.share));
                wr<int32_t>(rdram, 0x004CD2B4u, rd<int32_t>(rdram, 0x004CD2B4u) - dither(18.0f * (1.0f - c.share), g_cloudAcc));
            }
            g_cloudTurn += turn * c.share;
            g_cloudSpin += (step * wind * 0.5f + 0.3f) * 60.0f - 18.0f * (1.0f - c.share);
            g_origClouds(rdram, ctx, runtime);
        }

        // ---- 5. video cam / photo ---------------------------------------------------------------------------------------
        PS2_GUEST_CC void render2DFx(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const Clock &c = clock(rdram);
            if (c.tick)
            {
                g_counts[VideoFlips] += rd<int32_t>(rdram, 0x00481CD4u) != 0;
                g_counts[PhotoSteps] += rd<int32_t>(rdram, 0x00481CF8u) != 0;
            }
            else
            {
                if (rd<int32_t>(rdram, 0x00481CD4u) != 0) // the function flips the offset once in all
                    wr<uint8_t>(rdram, 0x00481D64u, rd<uint8_t>(rdram, 0x00481D64u) != 1u ? 1u : 0u);
                if (rd<int32_t>(rdram, 0x00481CF8u) != 0)
                    wr<int32_t>(rdram, 0x00481CF4u, rd<int32_t>(rdram, 0x00481CF4u) - 1);
            }
            g_orig2DFx(rdram, ctx, runtime);
        }

        // ---- 6. HUD bar flash ------------------------------------------------------------------------------------------
        PS2_GUEST_CC void hudFade(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t item = GPR_U32(ctx, 5);
            if (rd<uint8_t>(rdram, item + 1u) == 4u && (rd<uint32_t>(rdram, kFrameCounter) & 8u) == 0u)
            {
                if (!clock(rdram).tick)
                    wr<uint8_t>(rdram, item + 2u, static_cast<uint8_t>(rd<uint8_t>(rdram, item + 2u) + 1u)); // it drops by 1
                else
                    ++g_counts[HudDrops];
            }
            g_origHudFade(rdram, ctx, runtime);
        }

        // ---- 7. peds fading ---------------------------------------------------------------------------------------------
        PS2_GUEST_CC void getAlpha(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            (void)runtime;
            const uint32_t ra = GPR_U32(ctx, 31);
            int32_t alpha = rd<int32_t>(rdram, GPR_U32(ctx, 4) + 0x14u); // FUN_0036ED68: lw $v0, 0x14($a0)
            if (ra == kPedAlphaRead && (enabled() || logOn()) && clock(rdram).tick)
                g_counts[PedFadeSteps] += (rd<uint8_t>(rdram, GPR_U32(ctx, 18) + 0x1CFu) & 0x80u) != 0;
            if (ra == kPedAlphaRead && enabled() && !clock(rdram).tick)
            {
                const uint32_t ped = GPR_U32(ctx, 18); // $s2 (CPed::ProcessControl)
                if (rd<uint8_t>(rdram, ped + 0x1CFu) & 0x80u) // +0x1C8 < 0: fading out, -8 next
                    alpha += 8;
                else if (alpha < 0xFF) // fading in: +16 (to 255) next
                    alpha -= 16;
            }
            SET_GPR_S32(ctx, 2, alpha);
            ctx->pc = ra;
        }

        // ---- 8. streaks ---------------------------------------------------------------------------------------------------
        PS2_GUEST_CC void streakUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!clock(rdram).tick)
            {
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
            ++g_counts[StreakShifts];
            g_origStreak(rdram, ctx, runtime);
        }

        // ---- 9. rubbish ---------------------------------------------------------------------------------------------------
        float g_rubbishAcc = 0.0f;

        PS2_GUEST_CC void rubbishUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const Clock &c = clock(rdram);
            if (c.share < 1.0f)
            {
                const int back = dither(5.0f * (1.0f - c.share), g_rubbishAcc);
                const int32_t vis = rd<int32_t>(rdram, 0x004CD5B4u);
                // fading in (0x4CD5B0 == 0): +5 next, to 255; out: -5, to 0
                wr<int32_t>(rdram, 0x004CD5B4u, rd<uint8_t>(rdram, 0x004CD5B0u) == 0 ? vis - back : vis + back);
            }
            g_origRubbish(rdram, ctx, runtime);
        }

        // ---- 10. steam after rain -------------------------------------------------------------------------------------------
        PS2_GUEST_CC void rainSteam(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!clock(rdram).tick) // AddRain takes 1 off the count after this call
                wr<int32_t>(rdram, 0x0048A120u, rd<int32_t>(rdram, 0x0048A120u) + 1);
            else
                ++g_counts[SteamSteps];
            g_origSteam(rdram, ctx, runtime);
        }

        // ---- 11. explosions --------------------------------------------------------------------------------------------
        // CExplosion::Update (FUN_002E23A0; 48 records of 0x50 bytes from 0x51E510: +0x10 the position, +0x34 a byte that
        // is both "active" and the step count, +1 a frame, +0x3C a start delay) bursts flames by the step (a molotov:
        // (int)(steps x 0.55) x 30 a frame for steps 1-9, a second fire at step 7; the big ones: EXPLOSION_LARGE for steps
        // 1-4), and alternates its coronas and light on `frame & 1`. Each explosion gets steps at the PS2's 30 a second
        // from its own start (set before every Update), its step bursts pass only on a fresh step (the call matched to
        // its explosion by position), and `frame & 1` reads the PS2 frames' parity (put back when CProjectileInfo::Update,
        // the next call of FUN_002C0D88, starts). Before, the count also wrapped past 255 - the explosion's end - in 1.8 s
        // at 144 fps.
        constexpr uint32_t kExplosionUpdate = 0x002E23A0u, kExplosions = 0x0051E510u, kExplosionCount = 48u;
        constexpr uint32_t kProjectileUpdate = 0x00181208u, kTriggerExplosion = 0x00192760u;
        constexpr uint32_t kAddParticle = 0x001E9078u;
        constexpr uint32_t kBurstSites[] = {0x002E2D98u, 0x002E2FECu, 0x002E332Cu}; // flames, step-7 flames, LARGE
        constexpr uint32_t kSecondFire = 0x002E3018u;                                  // TriggerExplosion at step 7

        struct Explosion
        {
            bool active = false, fresh = true;
            uint8_t step = 0;
            float carry = 0.0f;
            uint32_t startMs = 0, bursts = 0; // VCS_EFFECTS_LOG
        };
        Explosion g_explosions[kExplosionCount];
        struct ExplosionLog
        {
            uint32_t ended = 0;
            double ms = 0.0, steps = 0.0, bursts = 0.0;
        } g_explLog;
        bool g_counterSwapped = false;
        uint32_t g_realCounter = 0;
        PS2Runtime::RecompiledFunction g_origExplosions = nullptr, g_origProjectiles = nullptr, g_origTrigger = nullptr,
                                       g_origAddParticle = nullptr;

        PS2_GUEST_CC void explosionUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const Clock &c = clock(rdram);
            if (c.share < 1.0f || g_counterSwapped)
            {
                if (!g_counterSwapped)
                    g_realCounter = rd<uint32_t>(rdram, kFrameCounter);
                g_counterSwapped = true;
                wr<uint32_t>(rdram, kFrameCounter, (g_realCounter & ~1u) | static_cast<uint32_t>(c.ticks & 1u));
            }
            for (uint32_t i = 0; i < kExplosionCount; ++i)
            {
                const uint32_t rec = kExplosions + i * 0x50u;
                Explosion &e = g_explosions[i];
                const uint8_t b = rd<uint8_t>(rdram, rec + 0x34u);
                if (b == 0u)
                {
                    if (e.active)
                    {
                        ++g_explLog.ended;
                        g_explLog.ms += rd<uint32_t>(rdram, kRealClock) - e.startMs;
                        g_explLog.steps += e.step;
                        g_explLog.bursts += e.bursts;
                    }
                    e.active = false;
                    continue;
                }
                if (!e.active || rd<uint32_t>(rdram, rec + 0x3Cu) != 0u) // new (its first Update), or in its delay
                {
                    e = {true, true, b, 0.0f, e.active ? e.startMs : rd<uint32_t>(rdram, kRealClock), e.active ? e.bursts : 0u};
                    continue;
                }
                e.carry += c.dt;
                e.fresh = e.carry >= kPs2FrameMs * 0.9f;
                if (e.fresh)
                {
                    e.carry = std::clamp(e.carry - kPs2FrameMs, -0.1f * kPs2FrameMs, kPs2FrameMs);
                    e.step = static_cast<uint8_t>(std::min(e.step + 1, 255)); // the game's +1 after 255 ends it, as there
                }
                wr<uint8_t>(rdram, rec + 0x34u, e.step);
            }
            g_origExplosions(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void projectileUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_counterSwapped)
            {
                wr<uint32_t>(rdram, kFrameCounter, g_realCounter);
                g_counterSwapped = false;
            }
            g_origProjectiles(rdram, ctx, runtime);
        }

        // whether the explosion nearest to a position has a fresh step this frame
        bool explosionFresh(const uint8_t *rdram, uint32_t posAddr)
        {
            const float x = rd<float>(rdram, posAddr), y = rd<float>(rdram, posAddr + 4u);
            Explosion *best = nullptr;
            float bestD = 1e30f;
            for (uint32_t i = 0; i < kExplosionCount; ++i)
                if (g_explosions[i].active)
                {
                    const uint32_t rec = kExplosions + i * 0x50u;
                    const float dx = rd<float>(rdram, rec + 0x10u) - x, dy = rd<float>(rdram, rec + 0x14u) - y;
                    if (dx * dx + dy * dy < bestD)
                        bestD = dx * dx + dy * dy, best = &g_explosions[i];
                }
            if (best && best->fresh)
                ++best->bursts;
            return !best || best->fresh;
        }

        PS2_GUEST_CC void addParticleBurst(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = GPR_U32(ctx, 31);
            if (std::find(std::begin(kBurstSites), std::end(kBurstSites), ra) != std::end(kBurstSites) &&
                !explosionFresh(rdram, GPR_U32(ctx, 5))) // a1: the particle's position
            {
                SET_GPR_U32(ctx, 2, 0u); // as with the pool full (these callers ignore it)
                ctx->pc = ra;
                return;
            }
            g_origAddParticle(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void triggerExplosion(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (GPR_U32(ctx, 31) == kSecondFire && !explosionFresh(rdram, GPR_U32(ctx, 4))) // a0: the position
            {
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
            g_origTrigger(rdram, ctx, runtime);
        }

        // ---- 12. the banknote spray ------------------------------------------------------------------------------------
        // FUN_0032FD78 (CSpecialFX::Update FUN_00167610 calls it while the "dollar" effect of 0x488784 exists; a script
        // command, FUN_0011D488, throws notes): up to 50 notes whose age counts the step but whose speed is x 0.714 and
        // falls by 0.03 a frame, position and spin added a frame, a 1 in 60 flutter rolled a frame. It runs on PS2 frames
        // only, with the step set to the steps since the last one (put back when FUN_00169CC0, its next call, starts).
        constexpr uint32_t kMoneySpray = 0x0032FD78u, kMoneySprayNext = 0x00169CC0u, kTimeStep = 0x004CD110u;
        bool g_stepSwapped = false;
        float g_realStep = 0.0f;
        PS2Runtime::RecompiledFunction g_origMoney = nullptr, g_origMoneyNext = nullptr;

        PS2_GUEST_CC void moneySpray(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const Clock &c = clock(rdram);
            if (c.share < 1.0f)
            {
                if (!c.tick)
                {
                    ctx->pc = GPR_U32(ctx, 31);
                    return;
                }
                g_realStep = rd<float>(rdram, kTimeStep);
                g_stepSwapped = true;
                wr<float>(rdram, kTimeStep, c.tickStep);
            }
            g_origMoney(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void moneySprayNext(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_stepSwapped)
            {
                wr<float>(rdram, kTimeStep, g_realStep);
                g_stepSwapped = false;
            }
            g_origMoneyNext(rdram, ctx, runtime);
        }

        void logEffects(uint8_t *rdram)
        {
            if (!logOn())
                return;
            static uint32_t lastMs = 0;
            static uint64_t lastSeq = 0, lastTicks = 0;
            const Clock &c = clock(rdram);
            const uint32_t now = rd<uint32_t>(rdram, kRealClock);
            if (!lastMs)
            {
                lastMs = now, lastSeq = c.seq, lastTicks = c.ticks;
                return;
            }
            if (now - lastMs < 30000u)
                return;
            const double secs = (now - lastMs) / 1000.0;
            std::fprintf(stderr,
                         "[effects] %.0f s: %.1f frames/s, %.1f PS2 frames/s; lightning %u bursts (%.1f a minute, %.0f ms each); clouds "
                         "turn %.4f / s, spin %.0f / s\n",
                         secs, (c.seq - lastSeq) / secs, (c.ticks - lastTicks) / secs, g_lightLog.bursts, g_lightLog.bursts * 60.0 / secs,
                         g_lightLog.bursts ? g_lightLog.burstMs / g_lightLog.bursts : 0.0, g_cloudTurn / secs, g_cloudSpin / secs);
            std::fprintf(stderr, "[effects] taking effect, per second:");
            for (int i = 0; i < CountN; ++i)
                if (g_counts[i])
                    std::fprintf(stderr, " %s %.1f", kCountNames[i], g_counts[i] / secs);
            std::fprintf(stderr, "\n");
            std::memset(g_counts, 0, sizeof(g_counts));
            if (g_explLog.ended)
                std::fprintf(stderr, "[effects] explosions: %u ended, %.0f ms, %.1f steps, %.0f burst calls each\n", g_explLog.ended,
                             g_explLog.ms / g_explLog.ended, g_explLog.steps / g_explLog.ended, g_explLog.bursts / g_explLog.ended);
            g_explLog = {};
            g_lightLog = {};
            g_cloudTurn = g_cloudSpin = 0.0;
            lastMs = now, lastSeq = c.seq, lastTicks = c.ticks;
        }
    }

    void installEffectsTimingHooks(PS2Runtime &runtime)
    {
        struct Hook
        {
            uint32_t address;
            PS2Runtime::RecompiledFunction *original;
            PS2Runtime::RecompiledFunction replacement;
        };
        static PS2Runtime::RecompiledFunction noOriginal = nullptr;
        const Hook hooks[] = {
            {kRand, &noOriginal, &rand},
            {kGetAlpha, &noOriginal, &getAlpha},
            {kWeatherUpdate, &g_origWeather, &weatherUpdate},
            {kCloudsUpdate, &g_origClouds, &cloudsUpdate},
            {kRender2DFx, &g_orig2DFx, &render2DFx},
            {kHudFade, &g_origHudFade, &hudFade},
            {kStreakUpdate, &g_origStreak, &streakUpdate},
            {kRubbishUpdate, &g_origRubbish, &rubbishUpdate},
            {kRainSteam, &g_origSteam, &rainSteam},
            {kExplosionUpdate, &g_origExplosions, &explosionUpdate},
            {kProjectileUpdate, &g_origProjectiles, &projectileUpdate},
            {kTriggerExplosion, &g_origTrigger, &triggerExplosion},
            {kAddParticle, &g_origAddParticle, &addParticleBurst}, // chains onto vcs_particle_timing's (installed before)
            {kMoneySpray, &g_origMoney, &moneySpray},
            {kMoneySprayNext, &g_origMoneyNext, &moneySprayNext},
        };
        if (!enabled())
        {
            std::fprintf(stderr, "[effects-timing] off (VCS_EFFECTS_TIMING=0): lightning, clouds, shake ... once a frame\n");
            if (!logOn()) // with VCS_EFFECTS_LOG the hooks go in anyway, every frame a tick: the per-frame counts (before)
                return;
        }
        for (const Hook &h : hooks)
        {
            PS2Runtime::RecompiledFunction original = runtime.lookupFunction(h.address);
            if (!original)
            {
                std::fprintf(stderr, "[effects-timing] 0x%x not found: left per frame\n", h.address);
                continue;
            }
            *h.original = original;
            runtime.replaceFunction(h.address, h.replacement);
        }
    }
}
