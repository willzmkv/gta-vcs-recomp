// Xbox Rain Droplets (gothamor#212): lens water drops spawned from weather rain, camera pitch/motion,
// and near-camera water splashes. Pauses automatically in interiors, under roofs, cutscenes, pause menus,
// and loading screens; renders behind the Native HUD layer via gsHwSetDroplets() across D3D11/D3D12/Vulkan.
#include "game/vcs_droplets.h"

#include "game/vcs_frontend.h"
#include "ps2_runtime.h"
#include "runtime/gs/gs_hw.h"
#include "runtime/ps2_control.h"
#include "settings/settings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

namespace vcs::droplets
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        constexpr uint32_t kRain = 0x004CD144u;       // CWeather::Rain (float 0..1)
        constexpr uint32_t kCodePause = 0x004CD11Cu;  // CTimer::m_CodePause
        constexpr uint32_t kUserPause = 0x004CD2ACu;  // CTimer::m_UserPause
        constexpr uint32_t kCutsceneMgr = 0x0048F820u;
        constexpr uint32_t kTimeCyclePtr = 0x00486838u;
        constexpr uint32_t kPlayerPed = 0x004E4910u;  // CWorld::Players[0].m_pPed
        constexpr uint32_t kMenuMgr = 0x00729480u;    // +0x40 menu open (byte)
        constexpr uint32_t kCameraMat = 0x006F44D0u;  // TheCamera matrix: right(+0), fwd(+0x10), up(+0x20), pos(+0x30)
        constexpr uint32_t kCameraInterior = 0x006F4DECu; // >0.5 inside interior / fade

        constexpr size_t kMaxDrops = 768;
        constexpr size_t kMaxMoving = 160;
        constexpr size_t kMaxSend = 640;

        struct Drop
        {
            float x = 0.0f;      // normalized picture coords 0..1
            float y = 0.0f;
            float size = 0.015f; // normalized height fraction
            float age = 0.0f;
            float ttl = 2.0f;
            float vx = 0.0f;
            float vy = 0.0f;
            float trailAcc = 0.0f;
            float seed = 0.0f;
            float alphaMul = 1.0f;
            bool active = false;
            bool moving = false;
            bool fades = true;
        };

        std::atomic<bool> g_enabled{false};
        const bool g_envDisabled = [] {
            const char *e = std::getenv("VCS_DROPLETS");
            return e && e[0] == '0' && !e[1];
        }();

        std::vector<Drop> g_drops;
        std::mt19937 g_rng{0x31415927u};
        bool g_hasCam = false;
        float g_prevPos[3] = {0.0f, 0.0f, 0.0f};
        std::chrono::steady_clock::time_point g_lastTime{};
        float g_spawnCarry = 0.0f;
        std::atomic<float> g_pendingSplash{0.0f};
        std::atomic<float> g_forcedRain{-1.0f}; // control-channel test override (< 0 uses game rain)
        std::atomic<uint32_t> g_statActive{0}, g_statMoving{0}, g_statSent{0};
        std::atomic<int> g_statSuppressed{0}; // 0=ok, 1=off, 2=paused/menu, 3=cutscene, 4=interior, 5=sheltered
        std::atomic<float> g_statRain{0.0f};
        bool g_sentEmpty = false;

        bool valid(uint32_t addr, uint32_t size = 4u)
        {
            const uint32_t a = addr & 0x1FFFFFFFu;
            return addr >= 0x00100000u && a < 0x02000000u && size <= 0x02000000u - a;
        }
        uint32_t rd32(const uint8_t *ram, uint32_t addr)
        {
            uint32_t v = 0;
            if (ram && valid(addr, 4u))
                std::memcpy(&v, ram + (addr & kRamMask), 4u);
            return v;
        }
        int8_t rdi8(const uint8_t *ram, uint32_t addr)
        {
            return (ram && valid(addr, 1u)) ? static_cast<int8_t>(ram[addr & kRamMask]) : int8_t(0);
        }
        uint8_t rd8(const uint8_t *ram, uint32_t addr)
        {
            return (ram && valid(addr, 1u)) ? ram[addr & kRamMask] : uint8_t(0);
        }
        float rdf(const uint8_t *ram, uint32_t addr)
        {
            float v = 0.0f;
            if (ram && valid(addr, 4u))
                std::memcpy(&v, ram + (addr & kRamMask), 4u);
            return std::isfinite(v) ? v : 0.0f;
        }

        float randf(float lo, float hi)
        {
            std::uniform_real_distribution<float> dist(lo, hi);
            return dist(g_rng);
        }

        bool isSheltered(const uint8_t *ram)
        {
            // Mirrors FUN_003EA5D0(FindPlayerPed()): (ped+0x160 -> +0xD0 & 0x800) && ped+0x22C && ped+0x22D.
            const uint32_t ped = rd32(ram, kPlayerPed);
            if (!valid(ped, 0x240u))
                return false;
            const uint32_t col = rd32(ram, ped + 0x160u);
            if (!valid(col, 0xE0u))
                return false;
            if ((rd32(ram, col + 0xD0u) & 0x800u) == 0u)
                return false;
            return rdi8(ram, ped + 0x22Cu) != 0 && rdi8(ram, ped + 0x22Du) != 0;
        }

        void clearDrops()
        {
            g_drops.clear();
            g_spawnCarry = 0.0f;
            g_pendingSplash.store(0.0f, std::memory_order_relaxed);
            g_statActive.store(0u, std::memory_order_relaxed);
            g_statMoving.store(0u, std::memory_order_relaxed);
            g_statSent.store(0u, std::memory_order_relaxed);
            if (!g_sentEmpty)
            {
                gsHwSetDroplets(nullptr, 0u);
                g_sentEmpty = true;
            }
        }

        void spawnDrop(float x, float y, float sizeNorm, float ttlSec, bool moving, bool fades, float alphaMul = 1.0f)
        {
            Drop *slot = nullptr;
            size_t movingCount = 0;
            for (Drop &d : g_drops)
            {
                if (!d.active)
                {
                    if (!slot)
                        slot = &d;
                }
                else if (d.moving)
                {
                    ++movingCount;
                }
            }
            if (moving && movingCount >= kMaxMoving)
                moving = false;
            if (!slot)
            {
                if (g_drops.size() < kMaxDrops)
                {
                    g_drops.emplace_back();
                    slot = &g_drops.back();
                }
                else
                {
                    // Recycle the oldest fading drop so storms never stall.
                    float bestRatio = -1.0f;
                    for (Drop &d : g_drops)
                    {
                        const float ratio = d.ttl > 0.0f ? (d.age / d.ttl) : 1.0f;
                        if (ratio > bestRatio)
                        {
                            bestRatio = ratio;
                            slot = &d;
                        }
                    }
                }
            }
            if (!slot)
                return;
            slot->x = x;
            slot->y = y;
            slot->size = sizeNorm;
            slot->age = 0.0f;
            slot->ttl = ttlSec;
            slot->vx = 0.0f;
            slot->vy = moving ? randf(0.025f, 0.075f) : 0.0f;
            slot->trailAcc = 0.0f;
            slot->seed = randf(0.0f, 1.0f);
            slot->alphaMul = alphaMul;
            slot->active = true;
            slot->moving = moving;
            slot->fades = fades;
        }
    } // namespace

    void notifySplash(float intensity)
    {
        if (!g_enabled.load(std::memory_order_relaxed) || g_envDisabled)
            return;
        float prev = g_pendingSplash.load(std::memory_order_relaxed);
        const float next = std::min(1.0f, std::max(prev, intensity));
        g_pendingSplash.store(next, std::memory_order_relaxed);
    }

    void onGameFrame(const uint8_t *rdram)
    {
        const auto now = std::chrono::steady_clock::now();
        float dt = 0.0f;
        if (g_lastTime.time_since_epoch().count() != 0)
            dt = std::chrono::duration<float>(now - g_lastTime).count();
        g_lastTime = now;
        dt = std::clamp(dt, 0.0f, 0.05f);

        if (g_envDisabled || !g_enabled.load(std::memory_order_relaxed) || !rdram)
        {
            g_statSuppressed.store(1, std::memory_order_relaxed);
            clearDrops();
            g_hasCam = false;
            return;
        }

        const bool paused = rd32(rdram, kUserPause) != 0u || rd32(rdram, kCodePause) != 0u ||
                            vcs::frontend::open() || rd32(rdram, kMenuMgr + 0x40u) != 0u;
        if (paused)
        {
            g_statSuppressed.store(2, std::memory_order_relaxed);
            g_statSent.store(0u, std::memory_order_relaxed);
            if (!g_sentEmpty)
            {
                gsHwSetDroplets(nullptr, 0u);
                g_sentEmpty = true;
            }
            return;
        }

        const uint32_t cs = rd32(rdram, kCutsceneMgr);
        const bool inCutscene = valid(cs, 0x20u) && rd32(rdram, cs + 0x1Cu) != 0u;
        if (inCutscene)
        {
            g_statSuppressed.store(3, std::memory_order_relaxed);
            clearDrops();
            g_hasCam = false;
            return;
        }

        const uint32_t tc = rd32(rdram, kTimeCyclePtr);
        const bool interior = rdf(rdram, kCameraInterior) > 0.5f ||
                              (valid(tc, 0x3380u) && rdf(rdram, tc + 0x3378u) > 0.01f);
        if (interior)
        {
            g_statSuppressed.store(4, std::memory_order_relaxed);
            clearDrops();
            g_hasCam = false;
            return;
        }

        // Camera vectors + frame velocity in camera space.
        const float right[3] = {rdf(rdram, kCameraMat + 0x00u), rdf(rdram, kCameraMat + 0x04u), rdf(rdram, kCameraMat + 0x08u)};
        const float fwd[3] = {rdf(rdram, kCameraMat + 0x10u), rdf(rdram, kCameraMat + 0x14u), rdf(rdram, kCameraMat + 0x18u)};
        const float up[3] = {rdf(rdram, kCameraMat + 0x20u), rdf(rdram, kCameraMat + 0x24u), rdf(rdram, kCameraMat + 0x28u)};
        const float pos[3] = {rdf(rdram, kCameraMat + 0x30u), rdf(rdram, kCameraMat + 0x34u), rdf(rdram, kCameraMat + 0x38u)};

        float rightVel = 0.0f, upVel = 0.0f, fwdVel = 0.0f;
        if (g_hasCam && dt > 1e-4f)
        {
            const float dx = pos[0] - g_prevPos[0];
            const float dy = pos[1] - g_prevPos[1];
            const float dz = pos[2] - g_prevPos[2];
            const float moveSq = dx * dx + dy * dy + dz * dz;
            if (moveSq < 25.0f * 25.0f)
            {
                const float vx = dx / dt, vy = dy / dt, vz = dz / dt;
                rightVel = vx * right[0] + vy * right[1] + vz * right[2];
                upVel = vx * up[0] + vy * up[1] + vz * up[2];
                fwdVel = vx * fwd[0] + vy * fwd[1] + vz * fwd[2];
            }
        }
        g_prevPos[0] = pos[0];
        g_prevPos[1] = pos[1];
        g_prevPos[2] = pos[2];
        g_hasCam = true;

        const bool sheltered = isSheltered(rdram);
        const float forced = g_forcedRain.load(std::memory_order_relaxed);
        const float rain = forced >= 0.0f ? forced : std::clamp(rdf(rdram, kRain), 0.0f, 1.0f);
        g_statRain.store(rain, std::memory_order_relaxed);
        g_statSuppressed.store(sheltered ? 5 : 0, std::memory_order_relaxed);

        // Camera pitch angle relative to +Z (0 deg = straight up, 90 deg = horizon, 180 deg = straight down).
        const float pitchCos = std::clamp(fwd[2], -1.0f, 1.0f);
        const float angleDeg = std::acos(pitchCos) * (180.0f / 3.14159265f);
        const float upwardAmt = std::clamp((140.0f - angleDeg) / 140.0f, 0.0f, 1.0f);

        // Spawn rate scaled cleanly in drops/sec (30 Hz equivalent of SkyGFX's FillScreen rate * CWeather::Rain).
        if (!sheltered && rain > 0.01f && upwardAmt > 0.01f)
        {
            const float speedBoost = 1.0f + std::clamp(std::abs(fwdVel) * 0.04f, 0.0f, 1.5f);
            const float ratePerSec = rain * (40.0f + 220.0f * upwardAmt) * speedBoost;
            g_spawnCarry += ratePerSec * dt;
            const int toSpawn = std::min(32, static_cast<int>(g_spawnCarry));
            g_spawnCarry -= static_cast<float>(toSpawn);
            for (int i = 0; i < toSpawn; ++i)
            {
                // Radius scaled in PS2 480p units (4..15 px / 480 -> ~0.008..0.031 of screen height).
                const float rNorm = randf(0.0075f, 0.029f);
                const bool sliding = rNorm >= 0.0185f;
                const float ttl = sliding ? randf(2.2f, 3.6f) : randf(1.4f, 2.5f);
                spawnDrop(randf(0.02f, 0.98f), randf(0.02f, 0.98f), rNorm, ttl, sliding, true, randf(0.75f, 1.0f));
            }
        }
        else
        {
            g_spawnCarry = 0.0f;
        }

        // Near-camera water splashes (swimming, boat spray).
        const float splash = g_pendingSplash.exchange(0.0f, std::memory_order_relaxed);
        if (splash > 0.01f)
        {
            const int count = std::clamp(static_cast<int>(splash * 36.0f), 4, 36);
            for (int i = 0; i < count; ++i)
            {
                const float rNorm = randf(0.010f, 0.034f);
                const bool sliding = rNorm >= 0.020f;
                spawnDrop(randf(0.05f, 0.95f), randf(0.15f, 0.95f), rNorm, randf(1.2f, 2.4f), sliding, true, 0.95f);
            }
        }

        // Update existing droplets: fade, radial speed blow + downward gravity streaks with trail beads.
        const float windX = -rightVel * 0.012f;
        const float windY = upVel * 0.012f;
        const float radialBlow = std::clamp(fwdVel * 0.035f, 0.0f, 1.2f);
        uint32_t activeCount = 0, movingCount = 0;
        for (size_t i = 0; i < g_drops.size(); ++i)
        {
            Drop &d = g_drops[i];
            if (!d.active)
                continue;
            d.age += dt;
            if (d.age >= d.ttl)
            {
                d.active = false;
                continue;
            }
            if (d.moving)
            {
                // Sliding velocity combines downward gravity and camera motion across the lens.
                const float cx = d.x - 0.5f;
                const float cy = d.y - 0.5f;
                const float grav = (0.045f + 0.055f * std::max(0.0f, 1.0f - std::abs(pitchCos))) * (0.7f + 0.6f * d.seed);
                d.vx = std::clamp(windX + cx * radialBlow, -0.45f, 0.45f);
                d.vy = std::clamp(grav + windY + cy * radialBlow, -0.25f, 0.55f);
                d.x += d.vx * dt;
                d.y += d.vy * dt;
                if (d.x < -0.05f || d.x > 1.05f || d.y < -0.05f || d.y > 1.05f)
                {
                    d.active = false;
                    continue;
                }
                const float distStep = std::hypot(d.vx, d.vy) * dt;
                d.trailAcc += distStep;
                if (d.trailAcc >= d.size * 0.85f)
                {
                    d.trailAcc = 0.0f;
                    spawnDrop(d.x, d.y, std::max(0.0055f, d.size * 0.42f), 0.55f, false, true, 0.65f);
                }
                ++movingCount;
            }
            ++activeCount;
        }

        // Pack into GPU descriptor array (using typical aspect 16:9 / adjusted in gs_hw by viewport).
        static thread_local std::vector<GSHwDroplet> sendBuf;
        sendBuf.clear();
        sendBuf.reserve(std::min<size_t>(activeCount, kMaxSend));
        for (const Drop &d : g_drops)
        {
            if (!d.active)
                continue;
            const float lifeFrac = d.ttl > 0.0f ? std::clamp(d.age / d.ttl, 0.0f, 1.0f) : 1.0f;
            const float fadeIn = std::clamp(d.age / 0.07f, 0.0f, 1.0f);
            const float fadeOut = d.fades ? (1.0f - lifeFrac * lifeFrac) : 1.0f;
            const float alpha = fadeIn * fadeOut * d.alphaMul * 0.92f;
            if (alpha <= 0.01f)
                continue;
            GSHwDroplet out{};
            out.x = d.x;
            out.y = d.y;
            out.rx = d.size * 0.5625f; // corrected in gs_hw by actual display height/width aspect
            out.ry = d.size;
            out.alpha = alpha;
            out.trail = d.moving ? 1.0f : 0.0f;
            out.seed = d.seed;
            out.tint = 1.0f;
            sendBuf.push_back(out);
            if (sendBuf.size() >= kMaxSend)
                break;
        }

        g_statActive.store(activeCount, std::memory_order_relaxed);
        g_statMoving.store(movingCount, std::memory_order_relaxed);
        g_statSent.store(static_cast<uint32_t>(sendBuf.size()), std::memory_order_relaxed);
        if (sendBuf.empty())
        {
            if (!g_sentEmpty)
            {
                gsHwSetDroplets(nullptr, 0u);
                g_sentEmpty = true;
            }
        }
        else
        {
            gsHwSetDroplets(sendBuf.data(), static_cast<uint32_t>(sendBuf.size()));
            g_sentEmpty = false;
        }
    }

    void registerSettings()
    {
        settings::Spec s;
        s.key = "graphics.rain_droplets";
        s.page = "Graphics";
        s.section = "Effects";
        s.label = "Rain droplets";
        s.help = "Water drops on the camera lens during rain and splashes (as Xbox Rain Droplets). Drawn behind the HUD "
                 "and paused indoors, in cutscenes and menus. Off matches the PS2.";
        s.type = settings::Type::Choice;
        s.choices = {{"Off", 0}, {"On", 1}};
        s.defaultValue = 0;
        s.env = "VCS_DROPLETS";
        s.onChange = [](int v) { g_enabled.store(v != 0, std::memory_order_relaxed); };
        settings::add(s);
    }

    void install(PS2Runtime &runtime)
    {
        (void)runtime;
        if (g_envDisabled)
            std::fprintf(stderr, "[droplets] off (VCS_DROPLETS=0)\n");

        // droplets [on|off|rain <0..1|-1>|splash|stats] -> automation inspection and deterministic rain/splash tests
        ps2control::addCommand("droplets", [](const std::string &args, ps2control::Reply reply) {
            if (args == "splash")
            {
                notifySplash(0.8f);
            }
            else if (args.rfind("rain ", 0) == 0)
            {
                g_forcedRain.store(static_cast<float>(std::atof(args.c_str() + 5)), std::memory_order_relaxed);
            }
            else if (args == "on")
            {
                settings::set("graphics.rain_droplets", 1);
            }
            else if (args == "off")
            {
                settings::set("graphics.rain_droplets", 0);
            }
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "ok enabled=%d suppressed=%d rain=%.2f forced=%.2f active=%u moving=%u sent=%u",
                          (g_enabled.load() && !g_envDisabled) ? 1 : 0,
                          g_statSuppressed.load(),
                          g_statRain.load(),
                          g_forcedRain.load(),
                          g_statActive.load(),
                          g_statMoving.load(),
                          g_statSent.load());
            reply(buf);
        });
    }
}
