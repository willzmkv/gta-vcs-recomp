// The street lamps' coronas as far as the detailed city (docs/game-internals.md 9k).
//
// What the game does: every instance of the map rests in the dummy pool (CPools::ms_pDummyPool 0x487AAC: 3407 CDummy
// objects of 0x60 bytes in the game image - matrix, model index at +0x56). Within ~80 m of the player a dummy whose
// model has 2D effects (a lamp post's light) becomes an object (pool 0x487AA4, CBuilding vtable 0x48F848) and the dummy
// is destroyed; CEntity::PreRender (FUN_003C35A8) of that object calls ProcessLightsForEntity (FUN_0027E430), which
// registers a corona (CCoronas::RegisterCorona FUN_0027DD80) per night light (2dfx +0x28 type 1: on from 19:00 to
// 06:59) within the effect's show distance (+0x18, 100 m for lamps), into the 56-slot table at 0x512560 (0x90 a slot);
// CCoronas::Render (FUN_0027C418) draws each slot as a sprite (FUN_003A03A8 -> RwIm2DRenderPrimitive FUN_002FBE08, a
// 4-vertex fan at 0x70CE60) fading from half the show distance to all of it. So lamp coronas end ~80 m from the
// player, while the detailed city further (world.hd_radius) draws the posts hundreds of metres away.
//
// What this does, with world.hd_radius > 0 at night: the used dummies with lamp lights within the detailed city's reach
// are projected as the game's sprite routine does (CSprite::CalcScreenCoords FUN_0039FC48: the screen matrix 0x520A00,
// 448 / depth, the Z of FUN_002F9F58 >> 4) and drawn through the game's own corona pass: one slot of the table is kept
// as a size-0 corona with the lamps' texture (so the pass makes an Im2D call with their texture, blend and depth test
// every frame), and that call's vertices are replaced by a triangle list holding every far lamp's quad. A lamp the game
// draws itself gets the complement of its alpha, so the two add up to one. Nothing at Original (0): no slot, no hook
// work.
#include "game/vcs_world_hd_coronas.h"

#include "game/vcs_world_hd.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_control.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace vcs::world_hd_coronas
{
    namespace
    {
        constexpr uint32_t kCoronaRender = 0x0027C418u;  // CCoronas::Render
        constexpr uint32_t kIm2DRender = 0x002FBE08u;    // RwIm2DRenderPrimitive(prim, vertices, count)
        constexpr uint32_t kRenderStateSet = 0x00175198u; // RwRenderStateSet(state, value): 1 = the texture
        // The lamps' sprites: a corona whose texture is not the default one (0x487778) goes through FUN_003A0D08, which
        // draws with the depth test on (its Im2D call returns at 0x3A1260); FUN_003A03A8 (return 0x3A07A8) is the
        // default texture's and the flares', drawn without it. The far lamps ride the first (2026-11-12: riding the second,
        // they were not depth tested - and not seen at all, their vertex alpha 0).
        constexpr uint32_t kSpriteReturn = 0x003A1260u;
        constexpr uint32_t kCoronaTable = 0x00512560u, kCoronaSlots = 56u, kCoronaStride = 0x90u;
        constexpr uint32_t kDummyPool = 0x00487AACu; // CPool {entries, flags, size}: 0x60-byte dummies (bit 7: free)
        constexpr uint32_t kDummySize = 0x60u;
        constexpr uint32_t kModelTable = 0x00486518u, kModelCount = 0x004CD10Cu, kEffects = 0x004E5CA8u; // 0x40 each
        constexpr uint32_t kCameraPos = 0x006F4500u, kCameraFwd = 0x006F44E0u, kHour = 0x004CD148u;
        constexpr uint32_t kScreenMatrix = 0x00520A00u, kOrigin = 0x006FE980u, kNear = 0x004CD130u, kFar = 0x004CD12Cu;
        constexpr uint32_t kZTerms = 0x00520A68u; // A at +0, C at +4, B at +0x10: z24 = (A d + B) / (C d)
        constexpr uint32_t kAspect = 0x00487488u, kSizeGrow = 0x004CD14Cu, kRenderGlobals = 0x00486838u;
        constexpr uint32_t kSizeScale = 0x32FCu; // *(kRenderGlobals) + kSizeScale: the registered size's factor
        constexpr uint32_t kClock = 0x004CD104u;
        constexpr float kBias = 1.5f; // the slots' depth bias (RegisterCorona's 0x3FC00000)
        constexpr uint32_t kBuffer = 0x07C00000u, kBufferBytes = 0x40000u; // our vertices (free guest memory)
        constexpr uint32_t kFakeId = 0x7C000001u;
        constexpr uint32_t kMaxLights = 1000u; // the nearest ones when more
        // how far the lamps reach per setting (the detailed city's), and the fade over its last metres
        constexpr float kReach[] = {0.0f, 450.0f, 580.0f, 720.0f};
        constexpr float kEdgeFade = 100.0f;
        // A far light's size (Project2DFX's distant lights, source/LODLights.ixx): at most kMaxSize metres (the game's
        // neon signs register 3-5 m: too big from afar - the player, 2026-11-12), times a growth from 1 at kGrowFrom to
        // kGrowMax at kGrowTo, so a light 600 m away is still a few pixels without a floor on its screen size (the earlier
        // 3-pixel floor kept every far light the same size - pixel-snapped, shimmering).
        constexpr float kMaxSize = 1.25f, kGrowFrom = 100.0f, kGrowTo = 1000.0f, kGrowMax = 2.0f;
        // and dimmer with distance, down to kFarDim at the reach: a row of lamps far down a street seen from its pavement
        // overlaps, and additive sprites at full brightness summed into one orange blob (pictures, 2026-11-12)
        constexpr float kDimFrom = 150.0f, kFarDim = 0.5f;
        constexpr float kMinHalf = 1.5f;
        constexpr uint32_t kSpecialModels = 0x0048A040u; // *this: the special model ids (+2, +4, +6, +8: traffic lights)
        constexpr uint32_t kTrafficFlash = 0x004CD2A4u;  // > 1.1: the traffic lights flash amber (a storm)
        constexpr uint32_t kTrafficGreen = 0x004883C0u;  // non-zero: every traffic light green
        constexpr uint32_t kRain = 0x004CD13Cu;
        constexpr uint32_t kTrafficBright = 0x3300u;     // *(kRenderGlobals) + this: the traffic coronas' brightness
        // Traffic lights far: half the lamps' reach and dimmer than the game's x 0.7 - seen as far as the lamps they
        // looked unreal (the player, 2026-11-12)
        constexpr float kTrafficReach = 0.5f, kTrafficDim = 0.5f;

        PS2Runtime::RecompiledFunction g_render = nullptr, g_im2d = nullptr, g_stateSet = nullptr;
        uint8_t *g_ram = nullptr;
        const int g_debugLevel = std::getenv("VCS_HD_CORONADEBUG") ? std::atoi(std::getenv("VCS_HD_CORONADEBUG")) : 0;
        const bool g_debug = g_debugLevel > 0;
        const bool g_off = std::getenv("VCS_HD_CORONAS") && std::atoi(std::getenv("VCS_HD_CORONAS")) == 0;

        struct Light
        {
            float x, y, z, size, show;
            uint8_t r, g, b;
            float d;      // from the camera
            bool traffic; // a traffic light: half the reach (TrafficReach)
        };
        std::vector<Light> g_lights; // this frame's far lamps (game thread)
        uint32_t g_texture = 0;      // RwRenderStateSet(1, texture): the current one
        uint32_t g_lampTexture = 0;  // the lamps' corona texture
        bool g_pending = false;      // lights to inject into the pass's first lamp sprite
        int g_fakeSlot = -1;
        // counters (the farcoronas command)
        std::atomic<uint32_t> g_statLights{0}, g_statDrawn{0}, g_statInjected{0}, g_statFrames{0};
        uint32_t g_dummiesLit = 0, g_traffic = 0;
        uint32_t g_otherTexture = 0; // lights within reach left out: another corona texture than the first lamp's
        float g_maxDistance = 0.0f;
        uint32_t g_debugLogged = 0;

        bool valid(uint32_t a, uint32_t n = 4)
        {
            a &= 0x1FFFFFFFu;
            return a >= 0x00100000u && a < PS2_RAM_SIZE && n <= PS2_RAM_SIZE - a;
        }
        uint32_t rd32(uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, g_ram + (a & PS2_RAM_MASK), 4);
            return v;
        }
        uint16_t rd16(uint32_t a)
        {
            uint16_t v;
            std::memcpy(&v, g_ram + (a & PS2_RAM_MASK), 2);
            return v;
        }
        uint8_t rd8(uint32_t a) { return g_ram[a & PS2_RAM_MASK]; }
        float rdf(uint32_t a)
        {
            float v;
            std::memcpy(&v, g_ram + (a & PS2_RAM_MASK), 4);
            return v;
        }
        void wr32(uint32_t a, uint32_t v) { std::memcpy(g_ram + (a & PS2_RAM_MASK), &v, 4); }
        void wr16(uint32_t a, uint16_t v) { std::memcpy(g_ram + (a & PS2_RAM_MASK), &v, 2); }
        void wr8(uint32_t a, uint8_t v) { g_ram[a & PS2_RAM_MASK] = v; }
        void wrf(uint32_t a, float v) { std::memcpy(g_ram + (a & PS2_RAM_MASK), &v, 4); }

        // a model's lights: 2dfx entries of kind 0 (a light) that are not a switch (flags +0x2C bit 3), with their light
        // type (+0x28: when they are on and how they flash - lightOn) and index in the model (the flash's phase); cached
        // per model index
        struct Effect
        {
            float x, y, z, size, show;
            uint8_t r, g, b;
            uint32_t texture;
            uint8_t type, index;
        };
        std::unordered_map<uint16_t, std::vector<Effect>> g_models;
        uint32_t g_modelTable = 0;

        const std::vector<Effect> &effects(uint16_t model)
        {
            const uint32_t table = rd32(kModelTable);
            if (table != g_modelTable)
            {
                g_models.clear();
                g_modelTable = table;
            }
            auto it = g_models.find(model);
            if (it != g_models.end())
                return it->second;
            std::vector<Effect> out;
            const uint32_t mi = model < rd32(kModelCount) && valid(table + model * 4u) ? rd32(table + model * 4u) : 0;
            if (valid(mi, 0x40u))
            {
                const uint32_t n = rd8(mi + 0x11u);
                const int16_t first = static_cast<int16_t>(rd16(mi + 0x18u));
                const uint32_t base = rd32(kEffects);
                if (n && first >= 0 && valid(base + uint32_t(first) * 0x40u, n * 0x40u))
                    for (uint32_t k = 0; k < n; ++k)
                    {
                        const uint32_t e = base + (uint32_t(first) + k) * 0x40u;
                        const uint8_t kind = rd8(e + 0x14u), type = rd8(e + 0x28u), flags = rd8(e + 0x2Cu);
                        if (kind != 0 || type > 11u || (flags & 8u))
                            continue;
                        const float size = rdf(e + 0x20u), show = rdf(e + 0x18u);
                        if (!(size > 0.0f) || !(show > 0.0f))
                            continue;
                        out.push_back({rdf(e), rdf(e + 4u), rdf(e + 8u), size, show, rd8(e + 0x10u), rd8(e + 0x11u), rd8(e + 0x12u),
                                       rd32(e + 0x30u), type, uint8_t(k)});
                    }
            }
            return g_models.emplace(model, std::move(out)).first->second;
        }

        bool night()
        {
            const uint8_t hour = rd8(kHour);
            return hour > 18 || hour < 7;
        }

        // Whether a light is on now, as ProcessLightsForEntity (FUN_0027E430) decides it per light type (+0x28): 0 always,
        // 1 at night (19:00-06:59), 2 flickering, 3 flickering at night (or in the rain), 4 / 6 / 8 flashing every 0x400 /
        // 0x800 / 0x1000 ms (phase by the light's index), 5 / 7 / 9 the same at night, 10 / 11 flickering by the entity's
        // own random bits (11: at night). The flicker: off while both ((seed ^ clock) & 0x60) and ((seed ^ clock >> 11) &
        // 3) are zero, the seed being the entity's bits 54-63 at +0x48 - the neon signs (type 3) the player saw pulse when
        // they were drawn steady and the game's own corona flickered beside them (2026-11-12).
        bool lightOn(const Effect &f, uint64_t bits)
        {
            const uint8_t hour = rd8(kHour);
            const bool day = hour > 6 && hour < 19;
            const uint32_t clock = rd32(kClock), seed = uint32_t(bits >> 54), k = f.index;
            auto flicker = [&](uint32_t s) { return ((s ^ clock) & 0x60u) != 0 || ((uint32_t(bits >> 54) ^ (clock >> 11)) & 3u) != 0; };
            switch (f.type)
            {
            case 0:
                return true;
            case 1:
                return !day;
            case 2:
                return flicker(seed);
            case 3:
                return !day || rdf(kRain) > 0.5f ? flicker(seed) : false;
            case 4:
            case 5:
                return (f.type == 4 || !day) && ((clock + k * 0x80u) & 0x200u) != 0;
            case 6:
            case 7:
                return (f.type == 6 || !day) && ((clock + k * 0x100u) & 0x400u) != 0;
            case 8:
            case 9:
                return (f.type == 8 || !day) && ((clock + k * 0x200u) & 0x800u) != 0;
            case 10:
            case 11:
            {
                if (f.type == 11 && day)
                    return false;
                const uint32_t s8 = seed & 0xFFu;
                return s8 > 0x10u || flicker(s8 << 3);
            }
            }
            return false;
        }

        // A traffic light's lit lamp (CTrafficLights::DisplayActualLight FUN_00313480, for the special models +2..+8):
        // the light's group by its heading (FUN_003155C0: 60-150 or 240-330 degrees -> 1, else 2), the phase by the clock
        // (FUN_003132B0 / FUN_00313318: 0 green, 1 amber, 2 red; amber flashing in a storm, all green when switched so),
        // the lamp the box of the model's lights gives (green at the bottom, amber in the middle, red on top) on the face
        // turned to the camera; 1.75 m, colour x 0.7 x the traffic brightness.
        bool isTrafficLight(uint16_t model)
        {
            const uint32_t ids = rd32(kSpecialModels);
            if (!valid(ids, 0x10u))
                return false;
            for (uint32_t o = 2; o <= 8; o += 2)
                if (rd16(ids + o) == model)
                    return true;
            return false;
        }

        int trafficPhase(uint32_t e)
        {
            float a = std::atan2(rdf(e + 0x14u), rdf(e + 0x10u));
            if (a < 0.0f)
                a += 6.2831855f;
            a *= 57.295776f;
            const int group = (a > 60.0f && a < 150.0f) || (a > 240.0f && a < 330.0f) ? 1 : 2;
            const uint32_t clock = rd32(kClock);
            if (rdf(kTrafficFlash) > 1.1f)
                return (clock & 0x400u) ? -1 : 1;
            if (rd32(kTrafficGreen) != 0)
                return 0;
            const uint32_t t = (clock & 0x7FFFu) >> 1;
            if (group == 1)
                return t < 5000u ? 0 : t < 6000u ? 1 : 2;
            return t < 6000u ? 2 : t < 11000u ? 0 : t < 12000u ? 1 : 2;
        }

        // this frame's far lights: the used dummies with lights on within reach (lamps, signs, traffic lights)
        void collect(float reach)
        {
            g_lights.clear();
            g_lampTexture = 0;
            g_dummiesLit = 0;
            g_traffic = 0;
            g_otherTexture = 0;
            g_maxDistance = 0.0f;
            const uint32_t pool = rd32(kDummyPool);
            if (!valid(pool, 0xCu))
                return;
            const uint32_t entries = rd32(pool), flags = rd32(pool + 4u), size = rd32(pool + 8u);
            if (size == 0 || size > 0x10000u || !valid(entries, size * kDummySize) || !valid(flags, size))
                return;
            const float cx = rdf(kCameraPos), cy = rdf(kCameraPos + 4u), cz = rdf(kCameraPos + 8u);
            const float limit = reach + 10.0f;
            for (uint32_t i = 0; i < size && g_lights.size() < kMaxLights * 4u; ++i)
            {
                if (rd8(flags + i) & 0x80u)
                    continue;
                const uint32_t e = entries + i * kDummySize;
                const float ex = rdf(e + 0x30u), ey = rdf(e + 0x34u);
                if (std::fabs(ex - cx) > limit || std::fabs(ey - cy) > limit)
                    continue;
                const uint16_t model = rd16(e + 0x56u);
                const float ez = rdf(e + 0x38u);
                if (isTrafficLight(model))
                {
                    // (its lights are type 12, none of effects(): the box of the model's first six 2dfx entries, as
                    // DisplayActualLight reads them)
                    const uint32_t table = rd32(kModelTable);
                    const uint32_t mi = model < rd32(kModelCount) && valid(table + model * 4u) ? rd32(table + model * 4u) : 0;
                    const int16_t first = valid(mi, 0x40u) ? static_cast<int16_t>(rd16(mi + 0x18u)) : int16_t(-1);
                    const uint32_t fx0 = rd32(kEffects) + uint32_t(first) * 0x40u;
                    const int phase = trafficPhase(e);
                    if (first < 0 || rd8(mi + 0x11u) < 6u || !valid(fx0, 6u * 0x40u) || phase < 0)
                        continue;
                    const float x0 = rdf(fx0);
                    float y0 = rdf(fx0 + 4u), y1 = y0, z0 = rdf(fx0 + 8u), z1 = z0;
                    for (uint32_t k = 1; k < 6u; ++k)
                    {
                        const float fy = rdf(fx0 + k * 0x40u + 4u), fz = rdf(fx0 + k * 0x40u + 8u);
                        y0 = std::min(y0, fy), y1 = std::max(y1, fy);
                        z0 = std::min(z0, fz), z1 = std::max(z1, fz);
                    }
                    const float fwd = rdf(kCameraFwd) * rdf(e + 0x10u) + rdf(kCameraFwd + 4u) * rdf(e + 0x14u) + rdf(kCameraFwd + 8u) * rdf(e + 0x18u);
                    const float ly = fwd < 0.0f ? y1 : y0, lz = phase == 0 ? z0 : phase == 1 ? (z0 + z1) * 0.5f : z1;
                    const float x = rdf(e) * x0 + rdf(e + 0x10u) * ly + rdf(e + 0x20u) * lz + ex;
                    const float y = rdf(e + 4u) * x0 + rdf(e + 0x14u) * ly + rdf(e + 0x24u) * lz + ey;
                    const float z = rdf(e + 8u) * x0 + rdf(e + 0x18u) * ly + rdf(e + 0x28u) * lz + ez;
                    const uint32_t globals = rd32(kRenderGlobals);
                    const float bright = kTrafficDim * (valid(globals + kTrafficBright) ? rdf(globals + kTrafficBright) : 1.0f);
                    const float r = phase == 0 ? 0.0f : 255.0f, g = phase == 0 ? 255.0f : phase == 1 ? 128.0f : 0.0f;
                    const float d = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy) + (z - cz) * (z - cz));
                    if (d <= reach * kTrafficReach + 10.0f && ++g_traffic)
                        g_lights.push_back({x, y, z, 1.75f, 50.0f, uint8_t(std::clamp(r * bright, 0.0f, 255.0f)),
                                            uint8_t(std::clamp(g * bright, 0.0f, 255.0f)), 0, d, true});
                    continue;
                }
                const std::vector<Effect> &fx = effects(model);
                if (fx.empty())
                    continue;
                ++g_dummiesLit;
                uint64_t bits;
                std::memcpy(&bits, g_ram + ((e + 0x48u) & PS2_RAM_MASK), 8);
                for (const Effect &f : fx)
                {
                    if (!lightOn(f, bits))
                        continue;
                    // the matrix's rows: right (+0), forward (+0x10), up (+0x20)
                    const float x = rdf(e) * f.x + rdf(e + 0x10u) * f.y + rdf(e + 0x20u) * f.z + ex;
                    const float y = rdf(e + 4u) * f.x + rdf(e + 0x14u) * f.y + rdf(e + 0x24u) * f.z + ey;
                    const float z = rdf(e + 8u) * f.x + rdf(e + 0x18u) * f.y + rdf(e + 0x28u) * f.z + ez;
                    const float d = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy) + (z - cz) * (z - cz));
                    if (d > limit)
                        continue;
                    if (!g_lampTexture)
                        g_lampTexture = f.texture;
                    if (f.texture != g_lampTexture)
                    {
                        ++g_otherTexture;
                        continue;
                    }
                    g_maxDistance = std::max(g_maxDistance, d);
                    g_lights.push_back({x, y, z, f.size, f.show, f.r, f.g, f.b, d, false});
                }
            }
            // the nearest when more than the buffer takes (the pool's order dropped lights here and there)
            if (g_lights.size() > kMaxLights)
            {
                std::nth_element(g_lights.begin(), g_lights.begin() + kMaxLights, g_lights.end(),
                                 [](const Light &a, const Light &b) { return a.d < b.d; });
                g_lights.resize(kMaxLights);
            }
        }

        int findSlot(uint32_t id)
        {
            for (uint32_t i = 0; i < kCoronaSlots; ++i)
                if (rd32(kCoronaTable + i * kCoronaStride + 0x20u) == id)
                    return int(i);
            return -1;
        }

        // the slot that makes the pass draw one (empty) lamp sprite a frame: kFakeAhead ahead of the camera, size 0 (at 3 m
        // it fell short of the near plane - CCoronas::Render skips a corona nearer than 0x4CD130 - and nothing far was
        // drawn whenever no lamp of the game's own was near; 2026-11-12)
        void keepFakeSlot()
        {
            int i = findSlot(kFakeId);
            if (i < 0)
                i = findSlot(0u);
            g_fakeSlot = i;
            if (i < 0)
                return;
            const uint32_t s = kCoronaTable + uint32_t(i) * kCoronaStride;
            std::memset(g_ram + (s & PS2_RAM_MASK), 0, kCoronaStride);
            constexpr float kFakeAhead = 20.0f;
            wrf(s + 0x10u, rdf(kCameraPos) + rdf(kCameraFwd) * kFakeAhead);
            wrf(s + 0x14u, rdf(kCameraPos + 4u) + rdf(kCameraFwd + 4u) * kFakeAhead);
            wrf(s + 0x18u, rdf(kCameraPos + 8u) + rdf(kCameraFwd + 8u) * kFakeAhead);
            wrf(s + 0x1Cu, 1.0f);
            wr32(s + 0x20u, kFakeId);
            wr32(s + 0x24u, rd32(kClock)); // the line-of-sight timer (not checked: flag bit 3 clear)
            wr32(s + 0x28u, g_lampTexture);
            wrf(s + 0x2Cu, 0.0f);    // size
            wrf(s + 0x34u, 2000.0f); // far clip
            wrf(s + 0x38u, 0.0f);    // depth bias
            wr8(s + 0x43u, 0xFFu);   // registered alpha
            wr8(s + 0x44u, 0xFFu);   // current alpha
            wr8(s + 0x45u, 1u);      // registered this frame (CCoronas::Update clears it, then zeroes +0x43 without it)
        }

        void dropFakeSlot()
        {
            const int i = findSlot(kFakeId);
            if (i >= 0)
                std::memset(g_ram + ((kCoronaTable + uint32_t(i) * kCoronaStride) & PS2_RAM_MASK), 0, kCoronaStride);
            g_fakeSlot = -1;
        }

        // the game's share of a light it draws itself at this position (0 when it does not): the render's fade from half
        // the show distance to all of it - not the slot's current alpha, which follows the game's flicker and fade-in and
        // made ours pulse against it
        float gameAlpha(const Light &l, float depth)
        {
            for (uint32_t i = 0; i < kCoronaSlots; ++i)
            {
                const uint32_t s = kCoronaTable + i * kCoronaStride;
                const uint32_t id = rd32(s + 0x20u);
                if (id == 0 || id == kFakeId)
                    continue;
                if (std::fabs(rdf(s + 0x10u) - l.x) > 0.25f || std::fabs(rdf(s + 0x14u) - l.y) > 0.25f ||
                    std::fabs(rdf(s + 0x18u) - l.z) > 0.25f)
                    continue;
                const float far = rdf(s + 0x34u), half = far * 0.5f;
                float k = 1.0f;
                if (depth >= far)
                    k = 0.0f;
                else if (depth >= half)
                    k = 1.0f - (depth - half) / half;
                return k;
            }
            return 0.0f;
        }

        // One Im2D vertex (16 bytes): u, v (1.15), r, g, b, a, x, y (whole pixels, 640 x 448), z. Im2D has no fraction
        // of a pixel: a small far sprite stepped a PS2 pixel at a time - 3 to 6 screen pixels at a higher internal
        // resolution - and the lamps wandered as the camera moved (the player, 2026-11-12). The fraction (1/16 pixel)
        // rides in the low bits of u and v with bit 4 set as the mark and alpha 0xFE (0x7F at the GS: colours reach it
        // halved); the renderer adds it back (gs_hw.cpp recordPrimitive, PATH2 only). The texture moves by < 1/1000.
        uint16_t withFraction(float t, int fraction)
        {
            int u = int(t * 32768.0f + 0.5f);
            if (u >= 0x20)
                u -= 0x20; // (stays inside the texture)
            return uint16_t((u & ~0x1F) | 0x10 | (fraction & 0xF));
        }

        void vertex(uint32_t at, float u, float v, uint8_t r, uint8_t g, uint8_t b, float x, float y, uint32_t z)
        {
            const float fx = std::floor(x), fy = std::floor(y);
            wr16(at, withFraction(u, int((x - fx) * 16.0f)));
            wr16(at + 2u, withFraction(v, int((y - fy) * 16.0f)));
            wr8(at + 4u, g_debugLevel == 4 ? 0x41u : r); // (=4: fixed colours, to see what reaches the renderer)
            wr8(at + 5u, g_debugLevel == 4 ? 0x42u : g);
            wr8(at + 6u, g_debugLevel == 4 ? 0x43u : b);
            wr8(at + 7u, 0xFEu); // (0: the renderer drops the pixels; 0xFE marks the fraction above)
            wr16(at + 8u, uint16_t(int16_t(int(fx))));
            wr16(at + 0xAu, uint16_t(int16_t(int(fy))));
            wr32(at + 0xCu, z);
        }

        // the far lamps as quads (two triangles each) after `at`; the number of vertices written
        uint32_t writeQuads(uint32_t at, uint32_t room, float reach)
        {
            float m[4][4];
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    m[r][c] = rdf(kScreenMatrix + uint32_t(r * 16 + c * 4));
            const float ox = rdf(kOrigin), oy = rdf(kOrigin + 4u), oz = rdf(kOrigin + 8u);
            const float near = rdf(kNear), far = rdf(kFar), aspect = rdf(kAspect), grow = rdf(kSizeGrow);
            const float zA = rdf(kZTerms), zC = rdf(kZTerms + 4u), zB = rdf(kZTerms + 0x10u);
            const uint32_t globals = rd32(kRenderGlobals);
            const float scale = valid(globals + kSizeScale) ? rdf(globals + kSizeScale) : 1.0f;
            const float cx = rdf(kCameraPos), cy = rdf(kCameraPos + 4u), cz = rdf(kCameraPos + 8u);
            uint32_t n = 0, drawn = 0;
            for (const Light &l : g_lights)
            {
                if (n + 6u > room)
                    break;
                const float px = l.x + ox, py = l.y + oy, pz = l.z + oz;
                float out[4];
                for (int c = 0; c < 4; ++c)
                    out[c] = m[0][c] * px + m[1][c] * py + m[2][c] * pz + m[3][c];
                const float w = out[3];
                if (!(w >= near) || !(w < far))
                    continue;
                const float sx = out[0] / w, sy = out[1] / w;
                const float d = std::sqrt((l.x - cx) * (l.x - cx) + (l.y - cy) * (l.y - cy) + (l.z - cz) * (l.z - cz));
                // its alpha: what the game's own corona leaves (none beyond its show distance), fading at the reach
                float a = 1.0f - gameAlpha(l, w);
                const float lreach = l.traffic ? reach * kTrafficReach : reach;
                a *= std::clamp((lreach - d) / (l.traffic ? kEdgeFade * 0.5f : kEdgeFade), 0.0f, 1.0f);
                a *= 1.0f - (1.0f - kFarDim) * std::clamp((d - kDimFrom) / (lreach - kDimFrom), 0.0f, 1.0f);
                const float growth = std::clamp(1.0f + (kGrowMax - 1.0f) * (d - kGrowFrom) / (kGrowTo - kGrowFrom), 1.0f, kGrowMax);
                const int alpha = int(a * 255.0f);
                if (alpha < 2)
                    continue;
                // the sprite as CCoronas::Render sizes it: 448 / depth per metre, wider by 1.4285715 / the aspect, grown
                // near the camera by kSizeGrow (0 in the city), the colour divided by the same
                const float f = 1.0f + grow * std::min(w, 40.0f) * 0.025f;
                const float h = 448.0f / w, ws = h * 1.4285715f / aspect;
                const float size = std::min(l.size, kMaxSize) * growth * scale * (g_debugLevel >= 2 ? 6.0f : 1.0f); // (=2: big)
                float hw = size * ws * f, hh = size * h * f;
                if (hh > 0.0f && hh < kMinHalf)
                {
                    hw *= kMinHalf / hh;
                    hh = kMinHalf;
                }
                if (!(hw > 0.0f) || !(hh > 0.0f) || sx + hw < 0.0f || sx - hw > 640.0f || sy + hh < 0.0f || sy - hh > 448.0f)
                    continue;
                const float depth = w - kBias;
                const float z24 = (zA * depth + zB) / (zC * depth);
                uint32_t z = z24 > 0.0f ? uint32_t(int32_t(z24)) >> 4 : 0u;
                if (g_debugLevel >= 3)
                    z = 60000u; // (=3: in front of everything, to see where they land)
                const int ir = int(float(l.r) / f) & 0xFF, ig = int(float(l.g) / f) & 0xFF, ib = int(float(l.b) / f) & 0xFF;
                const uint8_t r = uint8_t((ir * alpha) >> 8), g = uint8_t((ig * alpha) >> 8), b = uint8_t((ib * alpha) >> 8);
                // the corners as FUN_003A03A8 lays them (x - w, y - h), (x - w, y + h), (x + w, y + h), (x + w, y - h), clamped
                // to the screen with the texture coordinates following
                float xs[4] = {sx - hw, sx - hw, sx + hw, sx + hw}, ys[4] = {sy - hh, sy + hh, sy + hh, sy - hh};
                float us[4] = {0.0f, 0.0f, 1.0f, 1.0f}, vs[4] = {0.0f, 1.0f, 1.0f, 0.0f};
                for (int i = 0; i < 4; ++i)
                {
                    if (xs[i] < 0.0f)
                    {
                        us[i] = (xs[i] / hw) * -0.5f;
                        xs[i] = 0.0f;
                    }
                    if (xs[i] > 640.0f)
                    {
                        us[i] = 1.0f - ((xs[i] - 640.0f) * 0.5f) / hw;
                        xs[i] = 640.0f;
                    }
                    if (ys[i] < 0.0f)
                    {
                        vs[i] = (ys[i] / hh) * -0.5f;
                        ys[i] = 0.0f;
                    }
                    if (ys[i] > 448.0f)
                    {
                        vs[i] = 1.0f - ((ys[i] - 448.0f) * 0.5f) / hh;
                        ys[i] = 448.0f;
                    }
                }
                static const int order[6] = {0, 1, 2, 0, 2, 3};
                for (int k = 0; k < 6; ++k)
                {
                    const int i = order[k];
                    vertex(at + n * 16u, us[i], vs[i], r, g, b, xs[i], ys[i], z);
                    ++n;
                }
                ++drawn;
                if (g_debug && drawn <= 3u && (g_statFrames.load(std::memory_order_relaxed) % 300u) == 0u)
                    std::fprintf(stderr, "[hdcorona] lamp (%.1f %.1f %.1f) d %.0f -> screen %.1f %.1f depth %.1f half %.1f x %.1f z %u alpha %d rgb %d %d %d\n",
                                 l.x, l.y, l.z, d, sx, sy, w, hw, hh, z, alpha, ir, ig, ib);
            }
            g_statDrawn.store(drawn, std::memory_order_relaxed);
            return n;
        }

        PS2_GUEST_CC void renderHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_ram = rdram;
            g_pending = false;
            g_statFrames.fetch_add(1, std::memory_order_relaxed);
            const int radius = std::clamp(vcs::world_hd::radius(), 0, 3);
            if (radius > 0 && !g_off && night() && valid(rd32(kModelTable), 4u))
            {
                collect(kReach[radius]);
                g_statLights.store(uint32_t(g_lights.size()), std::memory_order_relaxed);
                if (!g_lights.empty())
                {
                    keepFakeSlot();
                    g_pending = true;
                    g_render(rdram, ctx, runtime);
                    return;
                }
            }
            else
                g_statLights.store(0u, std::memory_order_relaxed);
            g_statDrawn.store(0u, std::memory_order_relaxed);
            if (g_fakeSlot >= 0 || findSlot(kFakeId) >= 0)
                dropFakeSlot();
            g_render(rdram, ctx, runtime);
        }

        // RwIm2DRenderPrimitive(prim, vertices, count): the pass's first lamp sprite carries every far lamp
        PS2_GUEST_CC void im2dHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_ram = rdram;
            if (g_debug && g_pending && getRegU32(ctx, 31) == kSpriteReturn && g_debugLogged < 40u)
            {
                ++g_debugLogged;
                std::fprintf(stderr, "[hdcorona] sprite call: prim %u count %u texture 0x%08x lamps' 0x%08x (*0x%08x) slot %d\n",
                             getRegU32(ctx, 4), getRegU32(ctx, 6), g_texture, valid(g_lampTexture) ? rd32(g_lampTexture) : 0u,
                             g_lampTexture, g_fakeSlot);
            }
            // (a slot's +0x28 points at the texture pointer: the pass sets RwRenderStateSet(1, *(+0x28)))
            if (g_pending && getRegU32(ctx, 31) == kSpriteReturn && valid(g_lampTexture) && g_texture == rd32(g_lampTexture) &&
                getRegU32(ctx, 4) == 5u && getRegU32(ctx, 6) == 4u && valid(getRegU32(ctx, 5), 64u))
            {
                g_pending = false;
                const uint32_t src = getRegU32(ctx, 5);
                uint32_t n = 0;
                // the game's own quad (a fan of four) as two triangles, unless it is the empty one of our slot
                const bool empty = rd16(src + 8u) == rd16(src + 0x28u) && rd16(src + 0xAu) == rd16(src + 0x2Au);
                if (!empty)
                {
                    static const int order[6] = {0, 1, 2, 0, 2, 3};
                    for (int k = 0; k < 6; ++k)
                    {
                        std::memcpy(g_ram + ((kBuffer + n * 16u) & PS2_RAM_MASK), g_ram + ((src + uint32_t(order[k]) * 16u) & PS2_RAM_MASK), 16u);
                        ++n;
                    }
                }
                const int radius = std::clamp(vcs::world_hd::radius(), 0, 3);
                n += writeQuads(kBuffer + n * 16u, kBufferBytes / 16u - n, kReach[radius]);
                if (n >= 3u)
                {
                    SET_GPR_U32(ctx, 4, 3u); // a triangle list
                    SET_GPR_U32(ctx, 5, kBuffer);
                    SET_GPR_U32(ctx, 6, n);
                    g_statInjected.fetch_add(1, std::memory_order_relaxed);
                    if (g_debug && (g_statFrames.load(std::memory_order_relaxed) % 300u) == 0u)
                    {
                        std::fprintf(stderr, "[hdcorona] injected: triangle list of %u vertices at 0x%08x (the game's quad %s); its vertices x y z:",
                                     n, kBuffer, empty ? "empty, dropped" : "kept");
                        for (uint32_t k = 0; k < 4u; ++k)
                            std::fprintf(stderr, " (%d %d %u)", int16_t(rd16(src + k * 16u + 8u)), int16_t(rd16(src + k * 16u + 0xAu)), rd32(src + k * 16u + 0xCu));
                        std::fprintf(stderr, "\n");
                        // the game's lamp slots as we would project them (to compare with the quad above)
                        for (uint32_t i = 0; i < kCoronaSlots; ++i)
                        {
                            const uint32_t s = kCoronaTable + i * kCoronaStride;
                            if (rd32(s + 0x20u) == 0 || rd32(s + 0x28u) != g_lampTexture || rd8(s + 0x44u) == 0)
                                continue;
                            const float px = rdf(s + 0x10u) + rdf(kOrigin), py = rdf(s + 0x14u) + rdf(kOrigin + 4u), pz = rdf(s + 0x18u) + rdf(kOrigin + 8u);
                            float out[4];
                            for (int c = 0; c < 4; ++c)
                                out[c] = rdf(kScreenMatrix + uint32_t(c * 4)) * px + rdf(kScreenMatrix + uint32_t(16 + c * 4)) * py +
                                         rdf(kScreenMatrix + uint32_t(32 + c * 4)) * pz + rdf(kScreenMatrix + uint32_t(48 + c * 4));
                            std::fprintf(stderr, "[hdcorona]   slot %u id 0x%08x pos (%.1f %.1f %.1f) size %.2f -> x %.1f y %.1f w %.1f\n", i, rd32(s + 0x20u),
                                         rdf(s + 0x10u), rdf(s + 0x14u), rdf(s + 0x18u), rdf(s + 0x2Cu), out[0] / out[3], out[1] / out[3], out[3]);
                        }
                        std::fprintf(stderr, "[hdcorona]   camera (%.1f %.1f %.1f) fwd (%.2f %.2f %.2f) matrix row0 (%.1f %.1f %.1f %.3f) row3 (%.1f %.1f %.1f %.1f)\n",
                                     rdf(kCameraPos), rdf(kCameraPos + 4u), rdf(kCameraPos + 8u), rdf(kCameraFwd), rdf(kCameraFwd + 4u), rdf(kCameraFwd + 8u),
                                     rdf(kScreenMatrix), rdf(kScreenMatrix + 4u), rdf(kScreenMatrix + 8u), rdf(kScreenMatrix + 12u),
                                     rdf(kScreenMatrix + 48u), rdf(kScreenMatrix + 52u), rdf(kScreenMatrix + 56u), rdf(kScreenMatrix + 60u));
                    }
                }
                else
                {
                    ctx->pc = getRegU32(ctx, 31); // nothing to draw: not even the empty quad
                    return;
                }
            }
            g_im2d(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void stateSetHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (getRegU32(ctx, 4) == 1u)
                g_texture = getRegU32(ctx, 5);
            g_stateSet(rdram, ctx, runtime);
        }
    }

    void install(PS2Runtime &runtime)
    {
        g_render = runtime.lookupFunction(kCoronaRender);
        g_im2d = runtime.lookupFunction(kIm2DRender);
        g_stateSet = runtime.lookupFunction(kRenderStateSet);
        if (!g_render || !g_im2d || !g_stateSet)
        {
            std::fprintf(stderr, "[hdcorona] hooks not found: the lamp coronas stay the game's\n");
            return;
        }
        runtime.replaceFunction(kCoronaRender, &renderHook);
        runtime.replaceFunction(kIm2DRender, &im2dHook);
        runtime.replaceFunction(kRenderStateSet, &stateSetHook);

        // farcoronas -> ok lights=<far lamps this frame> drawn=<quads> injected=<frames with a draw> dummies=<lit dummies
        // within reach> maxdist=<m> slot=<the empty corona's slot> frames=<n>
        // lightcensus -> ok ...: every 2D effect of the used dummies within 720 m by kind / light type / flags
        // (which lights the far coronas take and which they leave)
        ps2control::addCommand("lightcensus", [](const std::string &, ps2control::Reply reply) {
            if (!g_ram)
                return reply("error not yet");
            const uint32_t pool = rd32(kDummyPool);
            if (!valid(pool, 0xCu))
                return reply("error no pool");
            const uint32_t entries = rd32(pool), flags = rd32(pool + 4u), size = rd32(pool + 8u);
            const float cx = rdf(kCameraPos), cy = rdf(kCameraPos + 4u);
            std::unordered_map<uint32_t, uint32_t> census; // kind << 16 | type << 8 | flags
            uint32_t dummies = 0;
            for (uint32_t i = 0; i < size && i < 0x10000u; ++i)
            {
                if (rd8(flags + i) & 0x80u)
                    continue;
                const uint32_t e = entries + i * kDummySize;
                if (std::fabs(rdf(e + 0x30u) - cx) > 720.0f || std::fabs(rdf(e + 0x34u) - cy) > 720.0f)
                    continue;
                const uint32_t table = rd32(kModelTable), model = rd16(e + 0x56u);
                const uint32_t mi = model < rd32(kModelCount) && valid(table + model * 4u) ? rd32(table + model * 4u) : 0;
                if (!valid(mi, 0x40u))
                    continue;
                const uint32_t n = rd8(mi + 0x11u);
                const int16_t first = static_cast<int16_t>(rd16(mi + 0x18u));
                if (!n || first < 0)
                    continue;
                ++dummies;
                for (uint32_t k = 0; k < n; ++k)
                {
                    const uint32_t fx = rd32(kEffects) + (uint32_t(first) + k) * 0x40u;
                    ++census[uint32_t(rd8(fx + 0x14u)) << 16 | uint32_t(rd8(fx + 0x28u)) << 8 | rd8(fx + 0x2Cu)];
                }
            }
            std::string out = "ok dummies=" + std::to_string(dummies);
            for (const auto &[key, count] : census)
            {
                char t[64];
                std::snprintf(t, sizeof(t), " kind%u/type%u/flags%02x=%u", key >> 16, (key >> 8) & 0xFFu, key & 0xFFu, count);
                out += t;
            }
            reply(out);
        });

        // dummiesnear X Y R -> ok then "model x y z lod0 lod1 type" per used dummy within R (the landmarks the low-detail
        // city stands in for: which entity a hidden low-detail sector was the far image of)
        ps2control::addCommand("dummiesnear", [](const std::string &args, ps2control::Reply reply) {
            if (!g_ram)
                return reply("error not yet");
            float qx = 0.0f, qy = 0.0f, qr = 50.0f;
            unsigned poolPtr = kDummyPool;
            if (std::sscanf(args.c_str(), "%f %f %f %x", &qx, &qy, &qr, &poolPtr) < 2)
                return reply("error usage: dummiesnear X Y [R [POOL_POINTER]]");
            const uint32_t pool = rd32(poolPtr);
            if (!valid(pool, 0xCu))
                return reply("error no pool");
            const uint32_t entries = rd32(pool), flags = rd32(pool + 4u), size = rd32(pool + 8u), table = rd32(kModelTable);
            // (the entries' size: the flags follow them)
            const uint32_t stride = poolPtr == kDummyPool || !size ? kDummySize : (flags - entries) / size;
            std::string out = "ok stride=" + std::to_string(stride) + " size=" + std::to_string(size);
            for (uint32_t i = 0; i < size && i < 0x10000u && out.size() < 3800u; ++i)
            {
                if (rd8(flags + i) & 0x80u)
                    continue;
                const uint32_t e = entries + i * stride;
                const float x = rdf(e + 0x30u), y = rdf(e + 0x34u);
                if (std::hypot(x - qx, y - qy) > qr)
                    continue;
                const uint16_t model = rd16(e + 0x56u);
                const uint32_t mi = model < rd32(kModelCount) && valid(table + model * 4u) ? rd32(table + model * 4u) : 0;
                char t[160];
                std::snprintf(t, sizeof(t), " | %u %.1f %.1f %.1f lod %.0f %.0f type %u", model, x, y, rdf(e + 0x38u),
                              valid(mi, 0x40u) ? rdf(mi + 0x2Cu) : -1.0f, valid(mi, 0x40u) ? rdf(mi + 0x30u) : -1.0f,
                              valid(mi, 0x40u) ? rd8(mi + 0x10u) : 0u);
                out += t;
            }
            reply(out);
        });

        ps2control::addCommand("farcoronas", [](const std::string &, ps2control::Reply reply) {
            char text[256];
            std::snprintf(text, sizeof(text), "ok lights=%u drawn=%u injected=%u dummies=%u maxdist=%.0f slot=%d frames=%u texture=0x%08x lamptex=0x%08x pending=%d other=%u traffic=%u",
                          g_statLights.load(), g_statDrawn.load(), g_statInjected.load(), g_dummiesLit, g_maxDistance, g_fakeSlot,
                          g_statFrames.load(), g_texture, g_lampTexture, int(g_pending), g_otherTexture, g_traffic);
            reply(text);
        });
    }
}
