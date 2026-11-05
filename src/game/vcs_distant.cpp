// Distant life, step A1 (docs/DISTANT_LIFE.md): decorative traffic on the game's car path network, beyond the
// simulated traffic's reach. Simulated here, on the host, once a game frame: no AI, no physics - a proxy follows the
// path links at road speed, takes a link at random at each node, keeps to the right; it lives in a band of distance from
// the camera (past where the real traffic is made and taken away, out to the setting's distance) and is placed again
// on a random car node of the band when it leaves it. The renderer draws them on the real cars' models (step A2).
//
// The path network (game-internals.md 9h, "Random traffic"): `*(0x487490)` -> {+0 nodes, +0x10 car node count,
// +0x7BA0 links}. A node is 10 bytes: x * 8 (s16), y * 8 (s16), z (s8, metres; less 100 when negative, as the game
// reads it), a byte, its first link (s16), flags (u16: links & 0xF, 0x20 switched off, 0x100 a boat path). A link is a
// u16, the neighbour & 0x3FFF. The car nodes are the first `count` (checked: the spot finder's list holds only those).
#include "game/vcs_distant.h"
#include "game/vcs_distance.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_control.h"
#include "settings/settings.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <string>

namespace vcs::distant
{
    namespace
    {
        constexpr uint32_t kPaths = 0x00487490u;        // the path network object
        constexpr uint32_t kWorldProcess = 0x001933C0u; // CWorld::Process: the frame's simulation
        constexpr uint32_t kTimeStep = 0x004CD110u;     // CTimer::ms_fTimeStep (1.0 = 1/50 s)
        constexpr uint32_t kFrameCounter = 0x004CD124u;
        constexpr uint32_t kCameraPos = 0x006F4500u;    // TheCamera's position (matrix +0x30)
        constexpr uint32_t kGenMultiplier = 0x006F4CA0u; // TheCamera.GenerationDistMultiplier (the traffic's, scaled)
        constexpr float kCarHeight = 0.8f;               // a car's origin above its path node
        constexpr float kLane = 2.6f;                    // to the right of the link's middle
        // Realistic Project2DFX / OLA limits for 1986 Vice City:
        // 60 distant cars on bridges/expressways, 60 distant walkers on beaches/promenades
        constexpr uint32_t kMaxProxies = 60;
        constexpr uint32_t kCameraFront = 0x006F44E0u;  // TheCamera's matrix: forward (+0x10)
        // step A2: the vehicles' classes by their vtable (entity +0x5C; found 2026-11-04 from the vehicle pool in a street
        // and a car made by the rig): 0x48F8C0 cars (a Sentinel, 211, 236, 243; Render, slot +0x44, 0x14C308), 0x490F10
        // model 212 (0x343F30), 0x4901C8 the ambient planes (280, 1-2 km away; 0x22CF80). Each class's Render ends in
        // CEntity::Render (+0x50 the clump; FUN_0039EC18 renders it), where the group opens.
        constexpr uint32_t kEntityRender = 0x003C3E58u; // CEntity::Render (entity $a0): every entity's model
        constexpr uint32_t kCarVtable = 0x0048F8C0u, kCar2Vtable = 0x00490F10u;
        // Templates whatever is on screen: CRenderer's PreRender of the visible lists (FUN_0035C4B8: each entry's vtable
        // +0x40; the lists hold entity pointers) runs after the lists are made and before they are rendered. Before it, the
        // farthest car of each model in the pool that is not listed (off screen) joins the vehicles' list (0x744AC0, count
        // 0x4CD59C; 0x1F40 bytes to the next list): the game renders it, the worker culls its draws (off screen) and draws
        // the proxies with them.
        constexpr uint32_t kPreRenderLists = 0x0035C4B8u;
        constexpr uint32_t kVehicleList = 0x00744AC0u, kVehicleListCount = 0x004CD59Cu, kVehicleListMax = 0x7D0u;
        constexpr float kTemplateRange = 450.0f; // cover the extended real vehicle band (~340-400 m)
        // The draws' world origin (game-internals.md 9, "Sector record"): the renderer keeps positions relative to a cell's
        // centre (FUN_0018ED28 moves it there): x, y, z here hold minus the centre, added to an entity's matrix for the VU.
        // Read when a car opens its group (found 2026-11-04: a car at -766, -1208.7 drew with its matrix at 8, -21.1; this
        // held 775, 1188.12, 0 - the cell (13, 7)'s centre negated).
        constexpr uint32_t kDrawOrigin = 0x006FE980u;
        constexpr uint32_t kVehiclePool = 0x00487A98u;      // CPool {entries, flags, size, last}, entries 0x8C0 bytes
        constexpr uint32_t kVehicleSize = 0x8C0u;
        // Distant crowds (DISTANT_LIFE.md, peds): the ped path nodes follow the car nodes in the same array - paths +0x0C
        // all (8387), +0x10 cars (3079), +0x14 peds (5308), one links table (found 2026-11-04 with the rig: the nodes past
        // the cars' lie on the pavements by the docks' streets). Random peds (vtable 0x490840) are the templates; with the
        // crowds on the real peds keep the extended population distance (vcs_distance.cpp's pedDistance) and these walk
        // strictly beyond the real pedestrian band (>= 310 m).
        constexpr uint32_t kPedPool = 0x00487A94u; // entries 0xD20 bytes
        constexpr uint32_t kPedSize = 0xD20u;
        constexpr uint32_t kRandomPedVtable = 0x00490840u;
        constexpr float kPedHeight = 1.0f;          // a ped's origin above its node
        constexpr float kCrowdNear = 310.0f;        // strictly beyond the real pedestrian band (~300 m)
        constexpr uint32_t kMaxWalkers = 60;
        constexpr float kPedTemplateRange = 360.0f; // cover the extended real pedestrian band (~325 m)
        constexpr uint32_t kRegisterCorona = 0x0027DD80u;
        constexpr uint32_t kCoronaTable = 0x00512560u, kCoronaSlots = 56u, kCoronaStride = 0x30u;

        std::atomic<int> g_setting{0}; // 0 off .. 100
        std::atomic<int> g_crowds{0};  // 0 off .. 100
        PS2Runtime::RecompiledFunction g_worldProcess = nullptr;

        struct Node
        {
            float x, y, z;
            uint16_t first, links, flags;
        };
        struct Car
        {
            uint16_t from = 0, to = 0;
            float t = 0.0f, length = 1.0f, speed = 10.0f;
            uint32_t model = 0;
            float side = 0.0f; // to the right of the link (walkers: a pavement's width)
            bool alive = false;
        };
        Car g_cars[kMaxProxies];
        Car g_walkers[kMaxWalkers];
        std::mt19937 g_rng{0x5643u};

        std::mutex g_publishMutex;
        std::vector<Proxy> g_published, g_publishedPeds;
        uint32_t g_publishedFrame = 0;

        // step A2: the vehicles rendered this frame (game thread), the models seen lately (proxies take theirs from them)
        struct Rendered
        {
            uint32_t entity;
            uint32_t model;
            float m[16];
            float distance;
            float origin[3]; // the draws' world origin when it rendered (minus the cell's centre)
            bool ped = false;
            bool moving = false; // a ped walking (its pose suits a walker)
        };
        std::vector<Rendered> g_rendered;
        uint32_t g_renderedEntity = 0;
        uint16_t g_group = 0;
        std::vector<uint32_t> g_models, g_pedModels;
        std::vector<uint32_t> g_addedCarTemplates;
        PS2Runtime::RecompiledFunction g_entityRender = nullptr, g_preRenderLists = nullptr, g_registerCorona = nullptr;
        std::atomic<uint64_t> g_statAdded{0};
        std::atomic<uint64_t> g_statCoronaCalls{0}, g_statCoronaTemplateSkip{0}, g_statCoronaBehindSkip{0}, g_statCoronaEvicted{0};

        // a frame's groups for the worker: one per rendered vehicle; a template (the one of its model drawn farthest from
        // the camera) holds its proxies' matrices
        struct FrameGroups
        {
            uint32_t frame = 0;
            struct Group
            {
                float inverse[16];
                uint32_t first = 0, count = 0; // in matrices, 16 floats each
            };
            std::vector<Group> groups;
            std::vector<float> matrices;
        };
        std::mutex g_framesMutex;
        std::shared_ptr<const FrameGroups> g_frames[8];
        std::atomic<uint64_t> g_statFrames{0}, g_statTemplates{0}, g_statAssigned{0}, g_statDrawn{0}, g_statStale{0};
        thread_local std::shared_ptr<const FrameGroups> t_frame;
        thread_local const FrameGroups::Group *t_group = nullptr;

        uint8_t *g_rdram = nullptr;
        uint32_t rd32(uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, g_rdram + (a & PS2_RAM_MASK), 4);
            return v;
        }
        float rdf(uint32_t a)
        {
            float f;
            std::memcpy(&f, g_rdram + (a & PS2_RAM_MASK), 4);
            return f;
        }

        // the network as the game holds it now (it is loaded with the game; nothing moves it): the car nodes are
        // [0, cars), the ped nodes [cars, cars + peds)
        bool network(uint32_t &nodes, uint32_t &count, uint32_t &links, uint32_t *peds = nullptr)
        {
            const uint32_t paths = rd32(kPaths);
            if (!paths)
                return false;
            nodes = rd32(paths);
            count = rd32(paths + 0x10u);
            links = rd32(paths + 0x7BA0u);
            if (peds)
                *peds = rd32(paths + 0x14u);
            return nodes && links && count > 0 && count < 0x4000u && (!peds || count + *peds < 0x4000u);
        }
        Node node(uint32_t nodes, uint32_t i)
        {
            const uint8_t *p = g_rdram + ((nodes + i * 10u) & PS2_RAM_MASK);
            int16_t x, y, first;
            uint16_t flags;
            std::memcpy(&x, p, 2);
            std::memcpy(&y, p + 2, 2);
            std::memcpy(&first, p + 6, 2);
            std::memcpy(&flags, p + 8, 2);
            float z = static_cast<float>(static_cast<int8_t>(p[4]));
            if (static_cast<int8_t>(p[4]) < 0)
                z -= 100.0f;
            return {x / 8.0f, y / 8.0f, z, static_cast<uint16_t>(first), static_cast<uint16_t>(flags & 0xFu), flags};
        }
        bool usable(const Node &n) { return n.links && !(n.flags & 0x20u) && !(n.flags & 0x100u); }
        uint16_t neighbour(uint32_t links, const Node &n, uint32_t k)
        {
            uint16_t l;
            std::memcpy(&l, g_rdram + ((links + (n.first + k) * 2u) & PS2_RAM_MASK), 2);
            return static_cast<uint16_t>(l & 0x3FFFu);
        }

        // the next link at a node: any but the one back, else back
        uint16_t nextNode(uint32_t nodes, uint32_t count, uint32_t links, uint16_t at, uint16_t from, uint32_t first = 0)
        {
            const Node n = node(nodes, at);
            uint16_t choice[16];
            uint32_t k = 0;
            for (uint32_t i = 0; i < n.links; ++i)
            {
                const uint16_t to = neighbour(links, n, i);
                if (to != from && to >= first && to < count && usable(node(nodes, to)))
                    choice[k++] = to;
            }
            return k ? choice[g_rng() % k] : from;
        }

        // on a random car node of the band, in front of the camera (within 60 degrees of where it looks: proxies are for
        // being seen; those behind are placed again in front)
        // walker: a ped on the pavement nodes [first, count) at walking pace, else a car on [0, count)
        void place(Car &c, uint32_t nodes, uint32_t count, uint32_t links, float cx, float cy, float fx, float fy, float lo, float hi,
                   bool walker = false, uint32_t first = 0)
        {
            for (int tries = 0; tries < 48; ++tries)
            {
                const uint16_t i = static_cast<uint16_t>(first + g_rng() % (count - first));
                const Node n = node(nodes, i);
                const float d = std::hypot(n.x - cx, n.y - cy);
                if (!usable(n) || d < lo || d > hi || (n.x - cx) * fx + (n.y - cy) * fy < 0.5f * d)
                    continue;
                const uint16_t to = nextNode(nodes, count, links, i, 0xFFFFu, first);
                if (to == 0xFFFFu || to == i)
                    continue;
                const Node m = node(nodes, to);
                c.from = i;
                c.to = to;
                c.length = std::max(1.0f, std::hypot(m.x - n.x, m.y - n.y));
                c.t = std::uniform_real_distribution<float>(0.0f, 1.0f)(g_rng);
                if (walker)
                {
                    c.speed = std::uniform_real_distribution<float>(1.1f, 1.7f)(g_rng);
                    c.side = std::uniform_real_distribution<float>(-2.0f, 2.0f)(g_rng);
                    c.model = g_pedModels.empty() ? 0u : g_pedModels[g_rng() % g_pedModels.size()];
                }
                else
                {
                    c.speed = std::uniform_real_distribution<float>(8.0f, 15.0f)(g_rng);
                    c.side = kLane;
                    c.model = g_models.empty() ? 0u : g_models[g_rng() % g_models.size()];
                }
                c.alive = true;
                return;
            }
            c.alive = false;
        }

        // the band: strictly past the real traffic outer radius (at least 100 m) out to the setting's distance
        void band(float &lo, float &hi)
        {
            static const char *forced = std::getenv("VCS_DISTANT_BAND"); // debug: "lo,hi" metres (proxies near, to look at)
            if (forced && std::sscanf(forced, "%f,%f", &lo, &hi) == 2)
                return;
            lo = std::max(100.0f, vcs::distance::realCarOuterRadius() + 5.0f);
            hi = lo + 5.5f * static_cast<float>(g_setting.load(std::memory_order_relaxed));
        }

        // the crowd band: strictly past the real pedestrian outer radius (at least 50 m) out to the setting's distance
        void crowdBand(float &lo, float &hi)
        {
            static const char *forced = std::getenv("VCS_DISTANT_CROWD_BAND"); // debug: "lo,hi" metres
            if (forced && std::sscanf(forced, "%f,%f", &lo, &hi) == 2)
                return;
            lo = std::max(50.0f, vcs::distance::realPedOuterRadius() + 5.0f);
            hi = lo + 3.0f * static_cast<float>(g_crowds.load(std::memory_order_relaxed));
        }

        // One kind's proxies a frame: placed in front of the camera in [lo, hi] (a few a frame), moved along their links,
        // taken away when they leave the band or fall behind the camera
        void move(Car *all, uint32_t max, uint32_t wanted, bool walker, uint32_t nodes, uint32_t first, uint32_t count, uint32_t links,
                  float lo, float hi, float height, std::vector<Proxy> &out)
        {
            const float dt = std::clamp(rdf(kTimeStep), 0.0f, 4.0f) / 50.0f;
            const float cx = rdf(kCameraPos), cy = rdf(kCameraPos + 4u), cz = rdf(kCameraPos + 8u);
            float vx = rdf(kCameraFront), vy = rdf(kCameraFront + 4u);
            const float vl = std::max(0.001f, std::hypot(vx, vy));
            vx /= vl;
            vy /= vl;
            uint32_t placed = 0; // a few a frame: a turned camera fills its view over a second, not all at once
            out.reserve(wanted);
            for (uint32_t i = 0; i < max; ++i)
            {
                Car &c = all[i];
                if (i >= wanted)
                {
                    c.alive = false;
                    continue;
                }
                if (!c.alive)
                {
                    if (placed >= (walker ? 8u : 4u))
                        continue;
                    ++placed;
                    place(c, nodes, count, links, cx, cy, vx, vy, lo, hi, walker, first);
                    if (!c.alive)
                        continue;
                }
                c.t += c.speed * dt / c.length;
                while (c.t >= 1.0f)
                {
                    const uint16_t next = nextNode(nodes, count, links, c.to, c.from, first);
                    c.t -= 1.0f;
                    c.from = c.to;
                    c.to = next;
                    const Node a = node(nodes, c.from), b = node(nodes, c.to);
                    c.length = std::max(1.0f, std::hypot(b.x - a.x, b.y - a.y));
                }
                const Node a = node(nodes, c.from), b = node(nodes, c.to);
                float fx = b.x - a.x, fy = b.y - a.y;
                const float len = std::max(0.001f, std::hypot(fx, fy));
                fx /= len;
                fy /= len;
                const float rx = fy, ry = -fx; // right of the direction (z up)
                const float x = a.x + (b.x - a.x) * c.t + rx * c.side, y = a.y + (b.y - a.y) * c.t + ry * c.side;
                const float z = a.z + (b.z - a.z) * c.t + height;
                const float d = std::hypot(x - cx, y - cy);
                if (d < lo || d > hi * 1.05f || (x - cx) * vx + (y - cy) * vy < -20.0f)
                {
                    c.alive = false;
                    continue;
                }
                Proxy p{};
                const float m[16] = {rx, ry, 0.0f, 0.0f, fx, fy, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, x, y, z, 1.0f};
                std::memcpy(p.m, m, sizeof(m));
                p.distance = std::sqrt(d * d + (z - cz) * (z - cz));
                p.id = i;
                p.model = c.model;
                out.push_back(p);
            }
        }

        void tick()
        {
            const int setting = g_setting.load(std::memory_order_relaxed), crowds = g_crowds.load(std::memory_order_relaxed);
            uint32_t nodes, count, links, peds = 0;
            std::vector<Proxy> cars, walkers;
            if ((setting > 0 || crowds > 0) && network(nodes, count, links, &peds))
            {
                if (setting > 0)
                {
                    float lo, hi;
                    band(lo, hi);
                    const uint32_t wantedCars = std::min<uint32_t>(kMaxProxies, static_cast<uint32_t>(setting) * 60u / 100u);
                    move(g_cars, kMaxProxies, wantedCars, false, nodes, 0, count,
                         links, lo, hi, kCarHeight, cars);
                }
                if (crowds > 0 && peds > 0)
                {
                    float pedLo, pedHi;
                    crowdBand(pedLo, pedHi);
                    const uint32_t wantedWalkers = std::min<uint32_t>(kMaxWalkers, static_cast<uint32_t>(crowds) * 60u / 100u);
                    move(g_walkers, kMaxWalkers, wantedWalkers, true, nodes, count,
                         count + peds, links, pedLo, pedHi, kPedHeight, walkers);
                }
            }
            std::lock_guard<std::mutex> lock(g_publishMutex);
            g_published.swap(cars);
            g_publishedPeds.swap(walkers);
            g_publishedFrame = (g_published.empty() && g_publishedPeds.empty()) ? 0u : (rd32(kFrameCounter) | 1u);
        }

        // before the original (DECISIONS.md, hooks)
        PS2_GUEST_CC void worldProcess(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_rdram = rdram;
            if (ctx->pc == kWorldProcess)
                tick();
            g_worldProcess(rdram, ctx, runtime);
        }

        // ---- step A2: templates ------------------------------------------------------------------------------------
        // A car's draws are made between its CEntity::Render and the next entity's: a car opens a group, any other entity
        // closes it. Draws made later
        // for it (an alpha list) or for nothing (effects) after the last entity keep the group: the worker checks that a
        // draw sits on the car (its matrix within a few metres of the car's) before it takes it as a template.
        bool inPool(uint32_t global, uint32_t entrySize, uint32_t entity)
        {
            const uint32_t pool = rd32(global);
            if (!pool)
                return false;
            const uint32_t entries = rd32(pool), size = rd32(pool + 8u);
            return entity >= entries && entity < entries + size * entrySize && (entity - entries) % entrySize == 0u;
        }
        bool isVehicle(uint32_t entity) { return inPool(kVehiclePool, kVehicleSize, entity); }
        bool isRandomPed(uint32_t entity) { return inPool(kPedPool, kPedSize, entity) && rd32(entity + 0x5Cu) == kRandomPedVtable; }
        bool pedMoving(uint32_t ped)
        {
            const float vx = rdf(ped + 0x140u), vy = rdf(ped + 0x144u);
            return vx * vx + vy * vy > 0.0004f; // (0.02 m a 1/50 s: ~1 m/s)
        }

        void readMatrix(uint32_t at, float m[16])
        {
            for (uint32_t r = 0; r < 4; ++r)
            {
                for (uint32_t k = 0; k < 3; ++k)
                    m[r * 4 + k] = rdf(at + r * 16u + k * 4u);
                m[r * 4 + 3] = r == 3 ? 1.0f : 0.0f;
            }
        }

        void openGroup(uint32_t entity)
        {
            if (entity == g_renderedEntity)
                return;
            g_renderedEntity = entity;
            g_group = 0;
            if (g_rendered.size() >= 0xFFF0u)
                return;
            bool ped = false;
            if (g_setting.load(std::memory_order_relaxed) > 0 && isVehicle(entity))
            {
                const uint32_t vtable = rd32(entity + 0x5Cu);
                if (vtable != kCarVtable && vtable != kCar2Vtable)
                    return;
            }
            else if (g_crowds.load(std::memory_order_relaxed) > 0 && isRandomPed(entity))
                ped = true;
            else
                return;
            Rendered r{};
            r.entity = entity;
            r.ped = ped;
            r.moving = ped && pedMoving(entity);
            uint16_t model;
            std::memcpy(&model, g_rdram + ((entity + 0x56u) & PS2_RAM_MASK), 2);
            r.model = model;
            readMatrix(entity, r.m);
            for (uint32_t k = 0; k < 3; ++k)
                r.origin[k] = rdf(kDrawOrigin + k * 4u);
            const float dx = r.m[12] - rdf(kCameraPos), dy = r.m[13] - rdf(kCameraPos + 4u), dz = r.m[14] - rdf(kCameraPos + 8u);
            r.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            g_rendered.push_back(r);
            g_group = static_cast<uint16_t>(g_rendered.size());
        }

        PS2_GUEST_CC void entityRender(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_rdram = rdram;
            if (ctx->pc == kEntityRender)
                openGroup(GPR_U32(ctx, 4));
            g_entityRender(rdram, ctx, runtime);
        }


        // ped: the random peds within kPedTemplateRange, a walking one of each model rather than the farthest
        void addTemplates(bool ped)
        {
            const uint32_t pool = rd32(ped ? kPedPool : kVehiclePool);
            const uint32_t listed = rd32(kVehicleListCount);
            if (!pool || listed >= kVehicleListMax - 64u)
                return;
            const uint32_t entrySize = ped ? kPedSize : kVehicleSize;
            const uint32_t entries = rd32(pool), flags = rd32(pool + 4u), size = std::min(rd32(pool + 8u), 1024u);
            const float cx = rdf(kCameraPos), cy = rdf(kCameraPos + 4u), cz = rdf(kCameraPos + 8u);
            struct Pick
            {
                uint32_t model, entity;
                float distance;
                bool listed;
            };
            Pick picks[64];
            uint32_t n = 0;
            for (uint32_t i = 0; i < size; ++i)
            {
                if (g_rdram[(flags + i) & PS2_RAM_MASK] & 0x80u)
                    continue;
                const uint32_t e = entries + i * entrySize;
                const uint32_t vt = rd32(e + 0x5Cu);
                if ((ped ? vt != kRandomPedVtable : (vt != kCarVtable && vt != kCar2Vtable)) || !rd32(e + 0x50u))
                    continue;
                const float dx = rdf(e + 0x30u) - cx, dy = rdf(e + 0x34u) - cy, dz = rdf(e + 0x38u) - cz;
                float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (d > (ped ? kPedTemplateRange : kTemplateRange))
                    continue;
                if (ped && pedMoving(e))
                    d += 1000.0f; // a walking ped of the model first
                uint16_t model;
                std::memcpy(&model, g_rdram + ((e + 0x56u) & PS2_RAM_MASK), 2);
                uint32_t k = 0;
                while (k < n && picks[k].model != model)
                    ++k;
                if (k == n)
                {
                    if (n == 64u)
                        continue;
                    picks[n++] = {model, e, d, false};
                }
                else if (d > picks[k].distance)
                    picks[k] = {model, e, d, false};
            }
            // those already listed (on screen) render anyway
            for (uint32_t j = 0; j < listed; ++j)
            {
                const uint32_t e = rd32(kVehicleList + j * 4u);
                for (uint32_t k = 0; k < n; ++k)
                    if (picks[k].entity == e)
                        picks[k].listed = true;
            }
            uint32_t count = listed;
            for (uint32_t k = 0; k < n; ++k)
                if (!picks[k].listed)
                {
                    std::memcpy(g_rdram + ((kVehicleList + count * 4u) & PS2_RAM_MASK), &picks[k].entity, 4);
                    if (!ped)
                        g_addedCarTemplates.push_back(picks[k].entity);
                    ++count;
                    ++g_statAdded;
                }
            std::memcpy(g_rdram + (kVehicleListCount & PS2_RAM_MASK), &count, 4);
        }

        // before the original (DECISIONS.md, hooks)
        PS2_GUEST_CC void preRenderLists(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_rdram = rdram;
            if (ctx->pc == kPreRenderLists)
            {
                g_addedCarTemplates.clear();
                if (g_setting.load(std::memory_order_relaxed) > 0)
                    addTemplates(false);
                if (g_crowds.load(std::memory_order_relaxed) > 0)
                    addTemplates(true);
            }
            g_preRenderLists(rdram, ctx, runtime);
        }

        // CCoronas::RegisterCorona (0x0027DD80):
        // 1. Suppress corona registrations from off-screen template cars added by addTemplates(false).
        // 2. Cull behind-camera coronas (and evict any existing behind-camera slot) so the 56-slot table
        //    at 0x00512560 is not filled by invisible behind-camera streetlamps.
        // 3. Scale vehicle corona farClip ($f13 <= 55m) with carDistance() and building/2DFX farClip with
        //    objectDistance().
        // 4. When all 56 slots are occupied, evict the farthest / behind-camera non-sun/moon slot in favor
        //    of closer lights.
        PS2_GUEST_CC void registerCorona(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_rdram = rdram;
            if (ctx->pc == kRegisterCorona)
            {
                g_statCoronaCalls.fetch_add(1, std::memory_order_relaxed);
                const uint32_t id = GPR_U32(ctx, 4);
                for (uint32_t e : g_addedCarTemplates)
                {
                    if (id >= e && id < e + kVehicleSize)
                    {
                        g_statCoronaTemplateSkip.fetch_add(1, std::memory_order_relaxed);
                        ctx->pc = GPR_U32(ctx, 31);
                        return;
                    }
                }
                if (id > 2u)
                {
                    const uint32_t posAddr = GPR_U32(ctx, 9);
                    if (posAddr >= 0x00100000u && posAddr + 12u <= PS2_RAM_SIZE)
                    {
                        const float px = rdf(posAddr), py = rdf(posAddr + 4u), pz = rdf(posAddr + 8u);
                        const float cx = rdf(kCameraPos), cy = rdf(kCameraPos + 4u), cz = rdf(kCameraPos + 8u);
                        const float fx = rdf(kCameraFront), fy = rdf(kCameraFront + 4u), fz = rdf(kCameraFront + 8u);
                        const float dx = px - cx, dy = py - cy, dz = pz - cz;
                        const float dist2 = dx * dx + dy * dy + dz * dz;
                        const float dot = dx * fx + dy * fy + dz * fz;
                        if (dot < 1.2f || dist2 < 1.8f)
                        {
                            for (uint32_t i = 0; i < kCoronaSlots; ++i)
                            {
                                const uint32_t slot = kCoronaTable + i * kCoronaStride;
                                if (rd32(slot + 0x20u) == id)
                                {
                                    const uint32_t zero = 0u;
                                    std::memcpy(g_rdram + ((slot + 0x20u) & PS2_RAM_MASK), &zero, 4);
                                    g_rdram[(slot + 0x24u) & PS2_RAM_MASK] = 0u;
                                    g_rdram[(slot + 0x25u) & PS2_RAM_MASK] = 0u;
                                    break;
                                }
                            }
                            g_statCoronaBehindSkip.fetch_add(1, std::memory_order_relaxed);
                            ctx->pc = GPR_U32(ctx, 31);
                            return;
                        }
                        const float dist = std::sqrt(dist2);
                        const float origFarClip = ctx->f[13];
                        const bool isVehicleLight = (origFarClip > 0.0f && origFarClip <= 55.0f);
                        // Cap world-space corona base radius and smoothly shrink & fade it as the camera approaches
                        // so coronas never blow up across the screen when passing near a streetlamp or car.
                        const float maxBaseSize = isVehicleLight ? 0.65f : 0.85f;
                        const float clampedBaseSize = std::min(std::abs(ctx->f[12]), maxBaseSize);
                        if (dot < 9.0f || dist < 20.0f)
                        {
                            const float fade = std::clamp((dot - 1.5f) / 7.5f, 0.0f, 1.0f) *
                                               std::clamp((dist - 2.0f) / 18.0f, 0.0f, 1.0f);
                            const float scaledSize = clampedBaseSize * std::max(0.04f, fade);
                            ctx->f[12] = std::copysign(scaledSize, ctx->f[12]);
                            const uint32_t origAlpha = GPR_U32(ctx, 8) & 0xFFu;
                            const uint32_t newAlpha = static_cast<uint32_t>(origAlpha * fade);
                            SET_GPR_U32(ctx, 8, newAlpha);
                        }
                        else
                        {
                            ctx->f[12] = std::copysign(clampedBaseSize, ctx->f[12]);
                        }
                        if (isVehicleLight)
                        {
                            const float scale = std::clamp(vcs::distance::realCarOuterRadius() / 160.0f, 1.0f, 5.0f);
                            ctx->f[13] = std::min(origFarClip * scale, 250.0f);
                        }
                        else if (origFarClip > 55.0f && origFarClip <= 200.0f)
                        {
                            const float scale = std::clamp(rdf(0x004887F8u), 1.0f, 2.5f);
                            ctx->f[13] = std::min(origFarClip * scale, 300.0f);
                        }
                        const float farClip = ctx->f[13];
                        if (dist2 < farClip * farClip)
                        {
                            bool hasSlot = false;
                            uint32_t worstSlot = 0u;
                            float worstDist2 = -1.0f;
                            for (uint32_t i = 0; i < kCoronaSlots; ++i)
                            {
                                const uint32_t slot = kCoronaTable + i * kCoronaStride;
                                const uint32_t slotId = rd32(slot + 0x20u);
                                if (slotId == id || slotId == 0u)
                                {
                                    hasSlot = true;
                                    break;
                                }
                                if (slotId <= 2u)
                                    continue;
                                const float sx = rdf(slot + 0x10u) - cx;
                                const float sy = rdf(slot + 0x14u) - cy;
                                const float sz = rdf(slot + 0x18u) - cz;
                                const float sdot = sx * fx + sy * fy + sz * fz;
                                float sdist2 = sx * sx + sy * sy + sz * sz;
                                if (sdot < 1.2f)
                                    sdist2 += 1.0e8f;
                                if (sdist2 > worstDist2)
                                {
                                    worstDist2 = sdist2;
                                    worstSlot = slot;
                                }
                            }
                            const float candidateDist2 = isVehicleLight ? (dist2 * 0.44f) : dist2;
                            if (!hasSlot && worstSlot != 0u && worstDist2 > candidateDist2)
                            {
                                const uint32_t zero = 0u;
                                std::memcpy(g_rdram + ((worstSlot + 0x20u) & PS2_RAM_MASK), &zero, 4);
                                g_rdram[(worstSlot + 0x24u) & PS2_RAM_MASK] = 0u;
                                g_rdram[(worstSlot + 0x25u) & PS2_RAM_MASK] = 0u;
                                g_statCoronaEvicted.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                    }
                }
            }
            g_registerCorona(rdram, ctx, runtime);
        }

        // the inverse of a rigid matrix (rows right, forward, up, position)
        void invertRigid(const float m[16], float o[16])
        {
            for (int r = 0; r < 3; ++r)
            {
                for (int k = 0; k < 3; ++k)
                    o[r * 4 + k] = m[k * 4 + r];
                o[r * 4 + 3] = 0.0f;
            }
            for (int k = 0; k < 3; ++k)
                o[12 + k] = -(m[12] * o[k] + m[13] * o[4 + k] + m[14] * o[8 + k]);
            o[15] = 1.0f;
        }
    }

    uint32_t snapshot(std::vector<Proxy> &out)
    {
        std::lock_guard<std::mutex> lock(g_publishMutex);
        out = g_published;
        return g_publishedFrame;
    }

    bool enabled() { return g_setting.load(std::memory_order_relaxed) > 0 || g_crowds.load(std::memory_order_relaxed) > 0; }

    bool crowdsOn() { return g_crowds.load(std::memory_order_relaxed) > 0; }

    uint16_t currentGroup() { return g_group; }

    void frameKicked(uint32_t itemFrame)
    {
        g_renderedEntity = 0;
        g_group = 0;
        if (g_rendered.empty())
        {
            if (g_frames[itemFrame & 7u])
            {
                std::lock_guard<std::mutex> lock(g_framesMutex);
                g_frames[itemFrame & 7u].reset();
            }
            return;
        }
        auto table = std::make_shared<FrameGroups>();
        table->frame = itemFrame;
        table->groups.resize(g_rendered.size());
        // the templates: per kind and model, the one drawn farthest from the camera (the fewest details); for peds a
        // walking one first (its pose suits a walker)
        std::vector<Proxy> carProxies, pedProxies;
        {
            std::lock_guard<std::mutex> lock(g_publishMutex);
            carProxies = g_published;
            pedProxies = g_publishedPeds;
        }
        for (int kind = 0; kind < 2; ++kind)
        {
        const bool peds = kind == 1;
        std::vector<uint32_t> templates; // indices into g_rendered
        for (uint32_t i = 0; i < g_rendered.size(); ++i)
        {
            const Rendered &r = g_rendered[i];
            if (r.ped != peds)
                continue;
            auto at = std::find_if(templates.begin(), templates.end(), [&](uint32_t t) { return g_rendered[t].model == r.model; });
            if (at == templates.end())
                templates.push_back(i);
            else
            {
                const Rendered &was = g_rendered[*at];
                if ((r.moving && !was.moving) || (r.moving == was.moving && r.distance > was.distance))
                    *at = i;
            }
        }
        std::sort(templates.begin(), templates.end(), [](uint32_t a, uint32_t b) { return g_rendered[a].model < g_rendered[b].model; });
        if (!templates.empty())
        {
            std::vector<uint32_t> &models = peds ? g_pedModels : g_models;
            models.clear();
            for (uint32_t t : templates)
                models.push_back(g_rendered[t].model);
            const std::vector<Proxy> &proxies = peds ? pedProxies : carProxies;
            // each proxy to the template of its model, else to one chosen by its slot; those behind the camera dropped
            const float cx = rdf(kCameraPos), cy = rdf(kCameraPos + 4u), cz = rdf(kCameraPos + 8u);
            const float fx = rdf(kCameraFront), fy = rdf(kCameraFront + 4u), fz = rdf(kCameraFront + 8u);
            std::vector<std::vector<const Proxy *>> by(templates.size());
            for (const Proxy &p : proxies)
            {
                if ((p.m[12] - cx) * fx + (p.m[13] - cy) * fy + (p.m[14] - cz) * fz < -20.0f)
                    continue;
                size_t k = 0;
                while (k < templates.size() && g_rendered[templates[k]].model != p.model)
                    ++k;
                if (k == templates.size())
                    k = ((p.id * 2654435761u) >> 8) % templates.size();
                by[k].push_back(&p);
            }
            for (size_t k = 0; k < templates.size(); ++k)
            {
                FrameGroups::Group &g = table->groups[templates[k]];
                const Rendered &t = g_rendered[templates[k]];
                float m[16];
                std::memcpy(m, t.m, sizeof(m));
                for (int c = 0; c < 3; ++c)
                    m[12 + c] += t.origin[c]; // in the draws' space
                invertRigid(m, g.inverse);
                g.first = static_cast<uint32_t>(table->matrices.size() / 16u);
                g.count = static_cast<uint32_t>(by[k].size());
                for (const Proxy *p : by[k])
                {
                    table->matrices.insert(table->matrices.end(), p->m, p->m + 16);
                    for (int c = 0; c < 3; ++c)
                        table->matrices[table->matrices.size() - 4u + c] += t.origin[c];
                }
                g_statAssigned += g.count;
            }
            g_statTemplates += templates.size();
        }
        }
        ++g_statFrames;
        g_rendered.clear();
        std::lock_guard<std::mutex> lock(g_framesMutex);
        g_frames[itemFrame & 7u] = std::move(table);
    }

    std::atomic<uint64_t> g_statProxy[3] = {};
    void noteProxy(int outcome)
    {
        if (outcome >= 0 && outcome < 3)
            g_statProxy[outcome].fetch_add(1, std::memory_order_relaxed);
    }

    void workerItem(uint32_t itemFrame, uint16_t group)
    {
        t_group = nullptr;
        if (!group)
            return;
        if (!t_frame || t_frame->frame != itemFrame)
        {
            std::lock_guard<std::mutex> lock(g_framesMutex);
            t_frame = g_frames[itemFrame & 7u];
        }
        if (!t_frame || t_frame->frame != itemFrame || group > t_frame->groups.size())
        {
            g_statStale.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const FrameGroups::Group &g = t_frame->groups[group - 1u];
        if (g.count)
            t_group = &g;
    }

    bool workerProxies(Proxies &out)
    {
        if (!t_group || !t_frame)
            return false;
        out.inverse = t_group->inverse;
        out.matrices = t_frame->matrices.data() + size_t(t_group->first) * 16u;
        out.count = t_group->count;
        g_statDrawn.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void registerSettings()
    {
        settings::Spec s;
        s.key = "world.distant_traffic";
        s.page = "Graphics";
        s.section = "Distance and population (experimental)";
        s.label = "Distant traffic";
        s.help = "Cars far away, beyond the simulated traffic, that only drive the roads (as Project2DFX's distant cars): "
                 "0 none (the original), 100 the most and the farthest. They cost little: no AI, no physics.";
        s.type = settings::Type::Integer;
        s.min = 0;
        s.max = 100;
        s.format = "%d";
        s.defaultValue = 0;
        s.env = "VCS_DISTANT_TRAFFIC";
        s.onChange = [](int v) { g_setting.store(std::clamp(v, 0, 100), std::memory_order_relaxed); };
        settings::add(s);

        s = {};
        s.key = "world.distant_crowds";
        s.page = "Graphics";
        s.section = "Distance and population (experimental)";
        s.label = "Distant crowds";
        s.help = "People far away, beyond the simulated pedestrians (> 300 m), who walk the pavements drawn with the real "
                 "people's models: 0 none (the original), 100 the most and the farthest. Real pedestrians keep their full "
                 "population distance and density.";
        s.type = settings::Type::Integer;
        s.min = 0;
        s.max = 100;
        s.format = "%d";
        s.defaultValue = 0;
        s.env = "VCS_DISTANT_CROWDS";
        s.onChange = [](int v) { g_crowds.store(std::clamp(v, 0, 100), std::memory_order_relaxed); };
        settings::add(s);
    }

    void install(PS2Runtime &runtime)
    {
        if ((g_worldProcess = runtime.lookupFunction(kWorldProcess)))
            runtime.replaceFunction(kWorldProcess, &worldProcess);
        if ((g_entityRender = runtime.lookupFunction(kEntityRender)))
            runtime.replaceFunction(kEntityRender, &entityRender);
        if ((g_preRenderLists = runtime.lookupFunction(kPreRenderLists)))
            runtime.replaceFunction(kPreRenderLists, &preRenderLists);
        if ((g_registerCorona = runtime.lookupFunction(kRegisterCorona)))
            runtime.replaceFunction(kRegisterCorona, &registerCorona);

        // proxies -> ok frame=<n> count=<n>, the templates / proxy draw counters, then "x,y,z,distance" for the first eight
        ps2control::addCommand("proxies", [](const std::string &, ps2control::Reply reply) {
            std::vector<Proxy> p, wp;
            const uint32_t frame = snapshot(p);
            {
                std::lock_guard<std::mutex> lock(g_publishMutex);
                wp = g_publishedPeds;
            }
            float carLo = 0.0f, carHi = 0.0f, pedLo = 0.0f, pedHi = 0.0f;
            band(carLo, carHi);
            crowdBand(pedLo, pedHi);
            float minCar = 0.0f, minPed = 0.0f;
            for (const Proxy &c : p)
                minCar = (minCar == 0.0f) ? c.distance : std::min(minCar, c.distance);
            for (const Proxy &w : wp)
                minPed = (minPed == 0.0f) ? w.distance : std::min(minPed, w.distance);
            char bandBuf[160];
            std::snprintf(bandBuf, sizeof(bandBuf), " carband=%.0f..%.0f pedband=%.0f..%.0f mincar=%.0f minped=%.0f",
                          carLo, carHi, pedLo, pedHi, minCar, minPed);
            std::string text = "ok frame=" + std::to_string(frame) + " count=" + std::to_string(p.size()) +
                               " walkers=" + std::to_string(wp.size()) + bandBuf +
                               " frames=" + std::to_string(g_statFrames.load()) + " templates=" + std::to_string(g_statTemplates.load()) +
                               " added=" + std::to_string(g_statAdded.load()) + " assigned=" + std::to_string(g_statAssigned.load()) +
                               " drawn=" + std::to_string(g_statDrawn.load()) + " proxydraws=" + std::to_string(g_statProxy[0].load()) + "/" +
                               std::to_string(g_statProxy[1].load()) + "/" + std::to_string(g_statProxy[2].load()) +
                               " stale=" + std::to_string(g_statStale.load());
            for (size_t i = 0; i < p.size() && i < 8; ++i)
            {
                char b[96];
                std::snprintf(b, sizeof(b), " %.1f,%.1f,%.1f,%.0f", p[i].m[12], p[i].m[13], p[i].m[14], p[i].distance);
                text += b;
            }
            reply(text);
        });

        ps2control::addCommand("coronas", [](const std::string &, ps2control::Reply reply) {
            if (!g_rdram)
            {
                reply("ok active=0");
                return;
            }
            const float cx = rdf(kCameraPos), cy = rdf(kCameraPos + 4u), cz = rdf(kCameraPos + 8u);
            uint32_t active = 0, veh = 0;
            float maxDist = 0.0f, maxSize = 0.0f;
            const uint32_t vpool = rd32(kVehiclePool);
            const uint32_t ventries = vpool ? rd32(vpool) : 0u;
            const uint32_t vsize = vpool ? rd32(vpool + 8u) : 0u;
            for (uint32_t i = 0; i < kCoronaSlots; ++i)
            {
                const uint32_t slot = kCoronaTable + i * kCoronaStride;
                const uint32_t id = rd32(slot + 0x20u);
                if (!id)
                    continue;
                ++active;
                if (ventries && id >= ventries && id < ventries + vsize * kVehicleSize)
                    ++veh;
                if (id > 2u)
                {
                    const float dx = rdf(slot + 0x10u) - cx, dy = rdf(slot + 0x14u) - cy, dz = rdf(slot + 0x18u) - cz;
                    maxDist = std::max(maxDist, std::sqrt(dx * dx + dy * dy + dz * dz));
                    maxSize = std::max(maxSize, std::abs(rdf(slot + 0x00u)));
                }
            }
            char buf[280];
            std::snprintf(buf, sizeof(buf),
                          "ok active=%u veh=%u maxdist=%.0f maxsz=%.2f calls=%llu tmplskip=%llu behindskip=%llu evicted=%llu",
                          active, veh, maxDist, maxSize,
                          static_cast<unsigned long long>(g_statCoronaCalls.load()),
                          static_cast<unsigned long long>(g_statCoronaTemplateSkip.load()),
                          static_cast<unsigned long long>(g_statCoronaBehindSkip.load()),
                          static_cast<unsigned long long>(g_statCoronaEvicted.load()));
            reply(buf);
        });
    }
}
