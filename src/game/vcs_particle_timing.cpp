// Particles on the game's time step instead of the frame (docs/game-internals.md 4, "Effects that run per frame").
//
// The PS2 ran the particle system at 30 fps, and two parts of it count frames instead of time:
// 1. CParticle::Update (FUN_001E9C90) moves particles by the step, but changes each one once a frame by its own rates:
//    brightness -= +0x38 (+0x2C), alpha -= +0x3A (+0x2D; freed at 0), size += +0x34 x (speed x step + 1) (+0x30),
//    rotation += +0x3E (+0x40), and the animation counter +0x3C += 1 against the type's period (system +0x32). Splashes
//    over size 1.2 also lose 0.15 of vertical speed a frame. At 144 fps a smoke puff faded out in 1/4.8 of the time.
//    Before each Update the rates are set to the PS2's amount per 1/30 s times step / (50/30) - the integer ones
//    dithered per particle, so their sum over time is exact - and put back to the game's own at a frame as long as the
//    PS2's. The originals are kept per pool slot; a particle is recognised as new by its destroy time or by a rate that
//    is not the one written. The animation period is scaled per type the same way.
// 2. The continuous emitters - exhaust, tyre smoke, wheel spray, engine smoke and fire, rain, splashes, wakes, dust,
//    particle objects (fountains, steam), insects - call AddParticle (FUN_001E9078, FUN_001E9040) once a frame. Their
//    calls (by return address; only sites that ignore the result, which is 0 also when the pool is full) pass at the
//    PS2's rate: each (site, entity, n-th call of the frame) carries a sum of step / (50/30) and passes when it reaches
//    one. Shots, hits and crashes spawn per event and are not touched; of an explosion, the smoke and jets of a burning
//    car are throttled, its opening burst (which counts frames itself) is not.
//
// Only while frames are shorter than the PS2's (independent timing, or original timing at 60) and a frame is shorter
// than 0.9 of 1/30 s: at 30 fps everything is the game's own. VCS_PARTICLE_TIMING=0 leaves the particles alone;
// VCS_PARTICLE_LOG=1 prints the AddParticle calls per second by return address every 10 s (which sites run per frame).
#include "game/vcs_particle_timing.h"

#include "game/vcs_framerate.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace vcs
{
    namespace
    {
        constexpr uint32_t kAddParticle = 0x001E9078u;  // CParticle::AddParticle(type a0, pos a1, dir a2, entity a3, ...)
        constexpr uint32_t kAddParticle2 = 0x001E9040u; // the overload without a colour (calls kAddParticle)
        constexpr uint32_t kUpdate = 0x001E9C90u;       // CParticle::Update
        constexpr uint32_t kSystems = 0x0070F318u;      // pointer to the particle systems: 0x84 bytes each
        constexpr uint32_t kSystemCount = 90u;
        constexpr uint32_t kPool = 0x004EFC30u, kPoolSize = 750u, kParticleSize = 0x50u;
        constexpr uint32_t kTimeStep = 0x004CD110u;     // CTimer::ms_fTimeStep (1.0 = 1/50 s)
        constexpr uint32_t kFrameCounter = 0x004CD124u; // CTimer::m_FrameCounter
        constexpr uint32_t kUserPause = 0x004CD2ACu, kCodePause = 0x004CD11Cu;
        constexpr float kPs2Step = 50.0f / 30.0f;

        // The continuous emitters' AddParticle calls: return addresses (jal + 8). From build/analysis/particle_sites.txt
        // (the 330 calls, decompiled callers in particle_callers*.c); the two that read the particle back (0x229BBC,
        // 0x33D9B8) are left out.
        constexpr uint32_t kThrottled[] = {
            // 0x138b68 car in water: splashes, smoke
            0x001398ACu, // 37=RAIN_SPLASH_BIGGROW
            0x00139DBCu, // 50=PED_SPLASH
            0x00139E3Cu, // 66=RUBBER_SMOKE
            // 0x13a0a0 car fire, engine steam (ProcessControl)
            0x0013C644u, // 33=ENGINE_STEAM
            0x0013C6DCu, // 55=ENGINE_SMOKE
            0x0013C774u, // 33=ENGINE_STEAM
            0x0013C80Cu, // 55=ENGINE_SMOKE
            0x0013C8D0u, // ?
            0x0013C990u, // ?
            0x0013CA50u, // ?
            0x0013CB10u, // 55=ENGINE_SMOKE
            0x0013ED64u, // 27=CARFLAME
            0x0013EE10u, // 57=CARFLAME_SMOKE
            0x0013EE48u, // 56=ENGINE_SMOKE2
            0x0013FBE8u, // ?
            // 0x140b28 car engine smoke / steam (damaged)
            0x001410A0u, // 1=SPARK_SMALL
            0x00141240u, // 1=SPARK_SMALL
            0x00141310u, // 56=ENGINE_SMOKE2
            0x001413A8u, // 33=ENGINE_STEAM
            0x001413F8u, // 33=ENGINE_STEAM
            0x0014145Cu, // 55=ENGINE_SMOKE
            0x00141520u, // 33=ENGINE_STEAM
            0x00141574u, // 33=ENGINE_STEAM
            0x001415CCu, // 33=ENGINE_STEAM
            0x0014161Cu, // 33=ENGINE_STEAM
            0x001417E8u, // 1=SPARK_SMALL
            0x0014188Cu, // 55=ENGINE_SMOKE
            0x001418E0u, // 33=ENGINE_STEAM
            0x00141908u, // 33=ENGINE_STEAM
            0x0014199Cu, // 33=ENGINE_STEAM
            // 0x1419d0 car wheel dirt / spray
            0x00141B34u, // 3=WHEEL_DIRT
            0x00141C2Cu, // 3=WHEEL_DIRT
            0x00141D14u, // 3=WHEEL_DIRT
            0x00141E80u, // 4=SAND
            0x00141FA8u, // 39=WATERSPRAY
            // 0x141fe8 car PreRender: exhaust, tyre smoke, rim sparks, rain on the body
            0x0014236Cu, // 66=RUBBER_SMOKE
            0x001423E8u, // 66=RUBBER_SMOKE
            0x00142C84u, // 67=BURNINGRUBBER_SMOKE
            0x00142CF8u, // 67=BURNINGRUBBER_SMOKE
            0x00142D6Cu, // 66=RUBBER_SMOKE
            0x00142E50u, // 66=RUBBER_SMOKE
            0x00142F28u, // 66=RUBBER_SMOKE
            0x00143204u, // 1=SPARK_SMALL
            0x00143274u, // 1=SPARK_SMALL
            0x001432E4u, // 1=SPARK_SMALL
            0x00143334u, // 1=SPARK_SMALL
            0x0014358Cu, // 66=RUBBER_SMOKE
            0x00143694u, // 66=RUBBER_SMOKE
            0x00143828u, // 66=RUBBER_SMOKE
            0x00143930u, // 66=RUBBER_SMOKE
            0x00143D94u, // 38=RAIN_SPLASHUP
            0x00144158u, // 65=EXHAUST_FUMES
            0x00144190u, // 65=EXHAUST_FUMES
            0x001441F4u, // 65=EXHAUST_FUMES
            0x00144234u, // 65=EXHAUST_FUMES
            0x00148080u, // 31=STEAM_NY_SLOWMOTION
            0x001481A4u, // 44=CAR_SPLASH
            0x00148918u, // 31=STEAM_NY_SLOWMOTION
            0x00148A3Cu, // 44=CAR_SPLASH
            0x0014B268u, // 31=STEAM_NY_SLOWMOTION
            0x0014B38Cu, // 44=CAR_SPLASH
            0x0014BA6Cu, // 31=STEAM_NY_SLOWMOTION
            0x0014BB90u, // 44=CAR_SPLASH
            // 0x181208 teargas
            0x001814C8u, // 23=TEARGAS
            0x00181508u, // 23=TEARGAS
            0x0018153Cu, // 23=TEARGAS
            // 0x19fda0 boat propeller
            0x001A21ACu, // 45=BOAT_SPLASH
            0x001A2228u, // 44=CAR_SPLASH
            // 0x1a5648 vehicle fire / smoke (other class)
            0x001A6014u, // 27=CARFLAME
            0x001A60B8u, // 56=ENGINE_SMOKE2
            0x001A619Cu, // 33=ENGINE_STEAM
            0x001A6228u, // 55=ENGINE_SMOKE
            // 0x1f3390 swimming wake / splashes
            0x001F3E3Cu, // 37=RAIN_SPLASH_BIGGROW
            0x001F3FA0u, // reg:movz
            0x001F4170u, // 44=CAR_SPLASH
            0x001F41E8u, // 75=SWIM_SPLASH
            0x001F47D4u, // 37=RAIN_SPLASH_BIGGROW
            // 0x200660 ped PreRender: blood from a limb, rain on peds
            0x00200F68u, // 8=BLOOD_SPURT
            0x002012B0u, // 38=RAIN_SPLASHUP
            // 0x2294a8 particle objects: fountains, hydrants, steam, smoke
            0x002296FCu, // 13=FLAME
            0x002297B4u, // 57=CARFLAME_SMOKE
            0x00229884u, // 13=FLAME
            0x0022993Cu, // 57=CARFLAME_SMOKE
            0x00229B28u, // 58=FIREBALL_SMOKE
            0x00229C80u, // 58=FIREBALL_SMOKE
            0x00229DC4u, // reg:lw
            0x00229FB0u, // 66=RUBBER_SMOKE
            0x00229FE8u, // 44=CAR_SPLASH
            0x0022A0C0u, // 66=RUBBER_SMOKE
            0x0022A0F8u, // 44=CAR_SPLASH
            0x0022A1D0u, // 66=RUBBER_SMOKE
            0x0022A208u, // 44=CAR_SPLASH
            0x0022A2C4u, // 66=RUBBER_SMOKE
            0x0022A2FCu, // 44=CAR_SPLASH
            0x0022A4A8u, // 44=CAR_SPLASH
            0x0022A59Cu, // 44=CAR_SPLASH
            0x0022A690u, // 44=CAR_SPLASH
            0x0022A784u, // 44=CAR_SPLASH
            0x0022A99Cu, // 66=RUBBER_SMOKE
            0x0022A9C8u, // 44=CAR_SPLASH
            0x0022AAA4u, // 66=RUBBER_SMOKE
            0x0022AAD0u, // 44=CAR_SPLASH
            0x0022ABACu, // 66=RUBBER_SMOKE
            0x0022ABD8u, // 44=CAR_SPLASH
            0x0022ACB4u, // 66=RUBBER_SMOKE
            0x0022ACE0u, // 44=CAR_SPLASH
            0x0022AE58u, // 44=CAR_SPLASH
            0x0022AF20u, // 44=CAR_SPLASH
            0x0022AFE8u, // 44=CAR_SPLASH
            0x0022B0B0u, // 44=CAR_SPLASH
            0x0022B1A4u, // 36=RAIN_SPLASH
            0x0022B1ECu, // 38=RAIN_SPLASHUP
            0x0022B400u, // 44=CAR_SPLASH
            0x0022B4A8u, // 44=CAR_SPLASH
            0x0022B550u, // 44=CAR_SPLASH
            0x0022B5F0u, // 44=CAR_SPLASH
            0x0022B638u, // reg:lw
            0x0022B7ECu, // 26=SPLASH
            0x0022B8ACu, // 26=SPLASH
            0x0022B96Cu, // 26=SPLASH
            0x0022BA14u, // 26=SPLASH
            0x0022BB0Cu, // 44=CAR_SPLASH
            0x0022BC40u, // reg:lw
            0x0022BC90u, // reg:lw
            // 0x337208 bike fire / smoke (ProcessControl)
            0x0033C4B4u, // ?
            0x0033C4ECu, // 56=ENGINE_SMOKE2
            0x0033C574u, // 57=CARFLAME_SMOKE
            // 0x33d5a0 bike engine smoke
            0x0033D90Cu, // 33=ENGINE_STEAM
            0x0033DA84u, // 33=ENGINE_STEAM
            0x0033DB9Cu, // 33=ENGINE_STEAM
            0x0033DC80u, // ?
            // 0x33dca0 bike wheel dirt / spray
            0x0033DDF8u, // 3=WHEEL_DIRT
            0x0033DEE8u, // 3=WHEEL_DIRT
            0x0033DFC4u, // 3=WHEEL_DIRT
            0x0033E0E8u, // 4=SAND
            0x0033E200u, // 39=WATERSPRAY
            // 0x33f010 bike PreRender
            0x0033F24Cu, // 66=RUBBER_SMOKE
            0x0033F310u, // 67=BURNINGRUBBER_SMOKE
            0x0033F390u, // 67=BURNINGRUBBER_SMOKE
            0x00340F70u, // ?
            0x00340FDCu, // 1=SPARK_SMALL
            0x00341048u, // 1=SPARK_SMALL
            0x00341094u, // 1=SPARK_SMALL
            0x003433F0u, // 65=EXHAUST_FUMES
            0x0034341Cu, // 65=EXHAUST_FUMES
            // 0x3621f8 vehicle PreRender (other class)
            0x00362A94u, // 66=RUBBER_SMOKE
            0x00362C80u, // 66=RUBBER_SMOKE
            0x00362E78u, // 66=RUBBER_SMOKE
            0x00363650u, // 1=SPARK_SMALL
            0x00363700u, // 1=SPARK_SMALL
            0x003637B4u, // 1=SPARK_SMALL
            0x00363840u, // 1=SPARK_SMALL
            // 0x3638d8 vehicle engine steam (other class)
            0x00363CD0u, // 33=ENGINE_STEAM
            0x00363D34u, // 55=ENGINE_SMOKE
            0x00363DCCu, // 33=ENGINE_STEAM
            0x00363ECCu, // 33=ENGINE_STEAM
            0x00363F60u, // 33=ENGINE_STEAM
            // 0x399fa0 vehicle PreRender (other class)
            0x0039A3CCu, // 66=RUBBER_SMOKE
            0x0039A490u, // 67=BURNINGRUBBER_SMOKE
            0x0039A50Cu, // 67=BURNINGRUBBER_SMOKE
            0x0039C19Cu, // 65=EXHAUST_FUMES
            0x0039C1C8u, // 65=EXHAUST_FUMES
            // 0x3e4270 ground steam after rain
            0x003E44C8u, // 32=GROUND_STEAM
            // 0x3e4528 ground steam after rain
            0x003E4774u, // 32=GROUND_STEAM
            // 0x3e47b8 rain on screen / ground
            0x003E4A3Cu, // 85=RAINDROP_2D
            0x003E4AFCu, // 85=RAINDROP_2D
            0x003E4B90u, // 85=RAINDROP_2D
            // 0x3e5b20 insects
            0x003E5C70u, // 84=BEASTIE
            // 0x3ee308 rotor blade sparks
            0x003EE7D8u, // 7=BLOOD_SMALL
            0x003EE828u, // 81=TEST
            0x003EE8B0u, // 81=TEST
            0x003EEB30u, // 1=SPARK_SMALL
            0x003EEB7Cu, // reg:daddu
            // 0x3ef3b8 rotor downwash dust / spray
            0x003EF6F4u, // 31=STEAM_NY_SLOWMOTION
            0x003EF7BCu, // 44=CAR_SPLASH
            0x003EF88Cu, // 53=HELI_DUST
            // 0x3f29d8 bow splashes
            0x003F2F9Cu, // 44=CAR_SPLASH
            0x003F3004u, // 45=BOAT_SPLASH
            0x003F31D8u, // 44=CAR_SPLASH
            0x003F3240u, // 45=BOAT_SPLASH
            // 0x1e9c90 CParticle::Update itself: the fountain at (557, -4, 151) on the quarter hours
            0x001E9E58u, // 44=CAR_SPLASH
            // 0x2e23a0 CExplosion::Update: the smoke while a car burns out (a frame's 4 puffs once the burst is over;
            // engine smoke on every other frame). The burst itself counts steps per frame (left as it is)
            0x002E2EC8u, // 57=CARFLAME_SMOKE (through the overload)
            0x002E25FCu, // 56=ENGINE_SMOKE2
            0x002E268Cu, // 56=ENGINE_SMOKE2
            // ... and its jets from a burning car (AddJetExplosion, kAddJetExplosion): a 1-in-16 chance a frame
            0x002E2734u,
        };

        constexpr uint32_t kAddJetExplosion = 0x001EBD28u; // CParticle::AddJetExplosion (a burst of its own)

        PS2Runtime::RecompiledFunction g_origAdd = nullptr, g_origAdd2 = nullptr, g_origUpdate = nullptr, g_origJet = nullptr;

        bool enabled()
        {
            static const bool on = [] {
                const char *v = std::getenv("VCS_PARTICLE_TIMING");
                return !(v && v[0] == '0');
            }();
            return on;
        }
        bool active() { return enabled() && (timing() == Timing::Independent || frameCap() != 30); }

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

        // the frame's share of a PS2 frame; 1 at about 30 fps and below (the game's own amounts)
        float frameScale(const uint8_t *rdram)
        {
            if (!active())
                return 1.0f;
            const float step = rd<float>(rdram, kTimeStep);
            return step > 0.0f && step < kPs2Step * 0.9f ? step / kPs2Step : 1.0f;
        }

        // ---- 2. the emitters ----------------------------------------------------------------------------------------
        bool throttled(uint32_t ra)
        {
            static const std::vector<uint32_t> sorted = [] {
                std::vector<uint32_t> v(std::begin(kThrottled), std::end(kThrottled));
                std::sort(v.begin(), v.end());
                return v;
            }();
            return std::binary_search(sorted.begin(), sorted.end(), ra);
        }

        struct Emitter
        {
            float sum = 1.0f; // the first call passes
            uint32_t frame = 0;
        };
        std::unordered_map<uint64_t, Emitter> g_emitters;
        std::unordered_map<uint64_t, uint32_t> g_callsThisFrame; // (site, entity) -> calls so far this frame
        uint32_t g_callsFrame = ~0u;

        std::unordered_map<uint32_t, uint64_t> g_logCalls, g_logPassed;
        uint64_t g_logFrames = 0;

        void logCall(uint8_t *rdram, uint32_t ra, bool passed)
        {
            static const bool log = std::getenv("VCS_PARTICLE_LOG") != nullptr;
            if (!log)
                return;
            ++g_logCalls[ra];
            if (passed)
                ++g_logPassed[ra];
            (void)rdram;
        }

        // whether this call spawns: the PS2's count per second at any frame rate
        bool emitterPasses(uint8_t *rdram, uint32_t ra, uint32_t entity)
        {
            const float scale = frameScale(rdram);
            if (scale >= 1.0f || !throttled(ra))
                return true;
            const uint32_t frame = rd<uint32_t>(rdram, kFrameCounter);
            if (frame != g_callsFrame)
            {
                g_callsFrame = frame;
                g_callsThisFrame.clear();
                if ((frame & 0x3FFu) == 0u) // forget emitters not seen for a while (cars driven off, objects gone)
                    for (auto it = g_emitters.begin(); it != g_emitters.end();)
                        it = frame - it->second.frame > 600u ? g_emitters.erase(it) : std::next(it);
            }
            const uint64_t site = static_cast<uint64_t>(ra) << 32 | entity;
            const uint32_t n = g_callsThisFrame[site]++;
            Emitter &e = g_emitters[site ^ (static_cast<uint64_t>(n) * 0x9E3779B97F4A7C15ull)];
            // one that stopped and starts again (a car stood, rain began) starts like on the PS2: its first call passes
            if (frame - e.frame > 30u)
                e.sum = 1.0f;
            e.frame = frame;
            e.sum += scale;
            if (e.sum < 1.0f + 1e-4f)
            {
                e.sum = std::max(e.sum, 0.0f);
                return false;
            }
            e.sum = std::min(e.sum - 1.0f, 1.0f);
            return true;
        }

        template <PS2Runtime::RecompiledFunction *Original>
        PS2_GUEST_CC void addParticle(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = GPR_U32(ctx, 31);
            const bool passes = !enabled() || emitterPasses(rdram, ra, GPR_U32(ctx, 7));
            logCall(rdram, ra, passes);
            if (!passes)
            {
                SET_GPR_U32(ctx, 2, 0u); // as AddParticle with the pool full (these callers ignore it)
                ctx->pc = ra;
                return;
            }
            (*Original)(rdram, ctx, runtime);
        }

        // ---- 1. the update -------------------------------------------------------------------------------------------
        struct Slot
        {
            bool valid = false;
            uint32_t destroy = 0;                       // +0x20: the particle's destroy time (ms)
            int16_t orig[3] = {}, wrote[3] = {};        // fade to black +0x38, alpha fade +0x3A, rotation +0x3E
            float acc[3] = {};                          // what the dithering carries
            float origRate = 0.0f, wroteRate = 0.0f;    // size rate +0x34
            uint32_t spawnMs = 0, seen = 0;             // VCS_PARTICLE_LOG: first seen (game clock); the last walk it was in
            int type = 0;
        };
        uint32_t g_walk = 0;
        std::unordered_map<int, std::pair<uint64_t, uint64_t>> g_lifetimes; // VCS_PARTICLE_LOG: type -> (count, ms)

        void particleDied(const Slot &s, uint32_t nowMs)
        {
            auto &l = g_lifetimes[s.type];
            ++l.first;
            l.second += nowMs - s.spawnMs;
        }
        Slot g_slots[kPoolSize];
        constexpr uint32_t kRateOffsets[3] = {0x38u, 0x3Au, 0x3Eu};

        struct Period
        {
            bool valid = false;
            uint16_t orig = 0, wrote = 0;
        };
        Period g_periods[kSystemCount];

        // this frame's amount of a per-frame integer rate: orig x scale, the fraction carried to the next frames
        int16_t dither(int16_t orig, float scale, float &acc)
        {
            if (scale >= 1.0f)
            {
                acc = 0.0f;
                return orig;
            }
            const float want = static_cast<float>(orig) * scale + acc;
            const float whole = std::trunc(want);
            acc = want - whole;
            return static_cast<int16_t>(whole);
        }

        // write: scale the rates (else only follow the particle, for VCS_PARTICLE_LOG)
        void scaleParticle(uint8_t *rdram, uint32_t p, int type, float scale, float step, bool write, uint32_t nowMs)
        {
            const uint32_t i = (p - kPool) / kParticleSize;
            if (p < kPool || i >= kPoolSize || (p - kPool) % kParticleSize)
                return;
            Slot &s = g_slots[i];
            const uint32_t destroy = rd<uint32_t>(rdram, p + 0x20);
            int16_t cur[3];
            for (int k = 0; k < 3; ++k)
                cur[k] = rd<int16_t>(rdram, p + kRateOffsets[k]);
            const float rate = rd<float>(rdram, p + 0x34);
            bool fresh = !s.valid || s.destroy != destroy || rate != s.wroteRate;
            for (int k = 0; k < 3 && !fresh; ++k)
                fresh = cur[k] != s.wrote[k];
            if (fresh)
            {
                if (s.valid)
                    particleDied(s, nowMs); // the slot's particle died and a new one took it since the last walk
                s.spawnMs = nowMs;
                s.type = type;
                s.valid = true;
                s.destroy = destroy;
                for (int k = 0; k < 3; ++k)
                    s.orig[k] = cur[k], s.acc[k] = 0.0f;
                s.origRate = rate;
            }
            s.seen = g_walk;
            if (!write)
            {
                for (int k = 0; k < 3; ++k)
                    s.wrote[k] = cur[k];
                s.wroteRate = rate;
                return;
            }
            for (int k = 0; k < 3; ++k)
            {
                s.wrote[k] = dither(s.orig[k], scale, s.acc[k]);
                wr<int16_t>(rdram, p + kRateOffsets[k], s.wrote[k]);
            }
            // size: Update adds rate x (speed x step + 1) a frame (speed doubled for types 0x21 / 0x41); the PS2 added
            // rate x (speed x 50/30 + 1) per 1/30 s
            float newRate = s.origRate;
            if (scale < 1.0f && s.origRate != 0.0f)
            {
                const float vx = rd<float>(rdram, p + 0x10), vy = rd<float>(rdram, p + 0x14), vz = rd<float>(rdram, p + 0x18);
                float speed = std::sqrt(vx * vx + vy * vy + vz * vz);
                if (type == 0x21 || type == 0x41)
                    speed *= 2.0f;
                newRate = s.origRate > 0.0f ? s.origRate * scale * (speed * kPs2Step + 1.0f) / (speed * step + 1.0f)
                                            : s.origRate * scale;
                // car / boat splashes past size 1.2 shrink and lose 0.15 of vertical speed a frame: the PS2's per 1/30 s
                if ((type == 0x2C || type == 0x2D) && s.origRate > 0.0f && rd<float>(rdram, p + 0x30) > 1.2f)
                    wr<float>(rdram, p + 0x18, vz + 0.15f * (1.0f - scale));
            }
            s.wroteRate = newRate;
            wr<float>(rdram, p + 0x34, newRate);
        }

        void scaleParticles(uint8_t *rdram)
        {
            static const bool log = std::getenv("VCS_PARTICLE_LOG") != nullptr;
            const bool write = enabled() && active();
            if ((!write && !log) || rd<uint8_t>(rdram, kUserPause) || rd<uint8_t>(rdram, kCodePause))
                return;
            ++g_walk;
            const uint32_t nowMs = rd<uint32_t>(rdram, 0x004CD104u);
            const float scale = write ? frameScale(rdram) : 1.0f, step = rd<float>(rdram, kTimeStep);
            const uint32_t systems = rd<uint32_t>(rdram, kSystems);
            if (!systems)
                return;
            for (uint32_t t = 0; t < kSystemCount; ++t)
            {
                const uint32_t sys = systems + t * 0x84u;
                // the animation period (+0x32): the counter +0x3C counts up to it and resets the frame after, so a
                // picture lasts period + 2 frames - on the PS2 (period + 2) / 30 s
                Period &per = g_periods[t];
                const uint16_t cur = rd<uint16_t>(rdram, sys + 0x32);
                if (!per.valid || cur != per.wrote)
                    per = {true, cur, cur};
                if (per.orig && write)
                {
                    const float frames = (per.orig + 2.0f) / scale - 2.0f;
                    per.wrote = scale >= 1.0f ? per.orig : static_cast<uint16_t>(std::clamp(std::lround(frames), 1l, 0xFFFFl));
                    wr<uint16_t>(rdram, sys + 0x32, per.wrote);
                }
                const int type = rd<int32_t>(rdram, sys);
                uint32_t p = rd<uint32_t>(rdram, sys + 0x80);
                for (uint32_t n = 0; p && n < kPoolSize; ++n)
                {
                    scaleParticle(rdram, p, type, scale, step, write, nowMs);
                    p = rd<uint32_t>(rdram, p + 0x48);
                }
            }
            for (Slot &sl : g_slots) // not on any list now: freed by the last Update
                if (sl.valid && sl.seen != g_walk)
                {
                    if (log)
                        particleDied(sl, nowMs);
                    sl.valid = false;
                }
        }

        void logRates(uint8_t *rdram)
        {
            static const bool log = std::getenv("VCS_PARTICLE_LOG") != nullptr;
            if (!log)
                return;
            static int64_t lastMs = -1;
            const int64_t ms = static_cast<int64_t>(rd<uint32_t>(rdram, 0x004CD104u)); // the game clock
            ++g_logFrames;
            if (lastMs < 0)
                lastMs = ms;
            if (ms - lastMs < 10000)
                return;
            const double secs = (ms - lastMs) / 1000.0;
            std::vector<std::pair<uint32_t, uint64_t>> rows(g_logCalls.begin(), g_logCalls.end());
            std::sort(rows.begin(), rows.end(), [](auto &a, auto &b) { return a.second > b.second; });
            std::fprintf(stderr, "[particles] %.1f s, %.1f frames/s, AddParticle calls/s by return address (passed):", secs,
                         g_logFrames / secs);
            for (const auto &[ra, calls] : rows)
                std::fprintf(stderr, " %x:%.1f(%.1f)%s", ra, calls / secs, g_logPassed[ra] / secs, throttled(ra) ? "*" : "");
            std::fprintf(stderr, "\n");
            std::vector<std::pair<int, std::pair<uint64_t, uint64_t>>> lives(g_lifetimes.begin(), g_lifetimes.end());
            std::sort(lives.begin(), lives.end(), [](auto &a, auto &b) { return a.second.first > b.second.first; });
            std::fprintf(stderr, "[particles] lifetimes by type (died/s, mean ms):");
            for (const auto &[type, l] : lives)
                std::fprintf(stderr, " %d:%.1f/%.0f", type, l.first / secs, static_cast<double>(l.second) / l.first);
            std::fprintf(stderr, "\n");
            g_lifetimes.clear();
            g_logCalls.clear();
            g_logPassed.clear();
            g_logFrames = 0;
            lastMs = ms;
        }

        PS2_GUEST_CC void update(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            scaleParticles(rdram);
            logRates(rdram);
            g_origUpdate(rdram, ctx, runtime);
        }
    }

    void installParticleTimingHooks(PS2Runtime &runtime)
    {
        g_origAdd = runtime.lookupFunction(kAddParticle);
        g_origAdd2 = runtime.lookupFunction(kAddParticle2);
        g_origUpdate = runtime.lookupFunction(kUpdate);
        if (!g_origAdd || !g_origAdd2 || !g_origUpdate)
        {
            std::fprintf(stderr, "[particle-timing] AddParticle / Update (0x%x / 0x%x / 0x%x) not found: particles stay on the frame rate\n",
                         kAddParticle, kAddParticle2, kUpdate);
            return;
        }
        runtime.replaceFunction(kAddParticle, &addParticle<&g_origAdd>);
        runtime.replaceFunction(kAddParticle2, &addParticle<&g_origAdd2>);
        runtime.replaceFunction(kUpdate, &update);
        g_origJet = runtime.lookupFunction(kAddJetExplosion);
        if (g_origJet)
            runtime.replaceFunction(kAddJetExplosion, &addParticle<&g_origJet>);
        if (!enabled())
            std::fprintf(stderr, "[particle-timing] off (VCS_PARTICLE_TIMING=0): particles fade and spawn per frame\n");
    }
}
