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
        constexpr uint32_t kMaxProxies = 240;
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
        constexpr float kTemplateRange = 160.0f; // pool cars farther are not added (their models' detail is the low one)
        // The draws' world origin (game-internals.md 9, "Sector record"): the renderer keeps positions relative to a cell's
        // centre (FUN_0018ED28 moves it there): x, y, z here hold minus the centre, added to an entity's matrix for the VU.
        // Read when a car opens its group (found 2026-11-04: a car at -766, -1208.7 drew with its matrix at 8, -21.1; this
        // held 775, 1188.12, 0 - the cell (13, 7)'s centre negated).
        constexpr uint32_t kDrawOrigin = 0x006FE980u;
        constexpr uint32_t kVehiclePool = 0x00487A98u;      // CPool {entries, flags, size, last}, entries 0x8C0 bytes
        constexpr uint32_t kVehicleSize = 0x8C0u;

        std::atomic<int> g_setting{0}; // 0 off .. 100
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
            bool alive = false;
        };
        Car g_cars[kMaxProxies];
        std::mt19937 g_rng{0x5643u};

        std::mutex g_publishMutex;
        std::vector<Proxy> g_published;
        uint32_t g_publishedFrame = 0;

        // step A2: the vehicles rendered this frame (game thread), the models seen lately (proxies take theirs from them)
        struct Rendered
        {
            uint32_t entity;
            uint32_t model;
            float m[16];
            float distance;
            float origin[3]; // the draws' world origin when it rendered (minus the cell's centre)
        };
        std::vector<Rendered> g_rendered;
        uint32_t g_renderedEntity = 0;
        uint16_t g_group = 0;
        std::vector<uint32_t> g_models;
        PS2Runtime::RecompiledFunction g_entityRender = nullptr, g_preRenderLists = nullptr;
        std::atomic<uint64_t> g_statAdded{0};

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

        // the network as the game holds it now (it is loaded with the game; nothing moves it)
        bool network(uint32_t &nodes, uint32_t &count, uint32_t &links)
        {
            const uint32_t paths = rd32(kPaths);
            if (!paths)
                return false;
            nodes = rd32(paths);
            count = rd32(paths + 0x10u);
            links = rd32(paths + 0x7BA0u);
            return nodes && links && count > 0 && count < 0x4000u;
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
        uint16_t nextNode(uint32_t nodes, uint32_t count, uint32_t links, uint16_t at, uint16_t from)
        {
            const Node n = node(nodes, at);
            uint16_t choice[16];
            uint32_t k = 0;
            for (uint32_t i = 0; i < n.links; ++i)
            {
                const uint16_t to = neighbour(links, n, i);
                if (to != from && to < count && usable(node(nodes, to)))
                    choice[k++] = to;
            }
            return k ? choice[g_rng() % k] : from;
        }

        // on a random car node of the band, in front of the camera (within 60 degrees of where it looks: proxies are for
        // being seen; those behind are placed again in front)
        void place(Car &c, uint32_t nodes, uint32_t count, uint32_t links, float cx, float cy, float fx, float fy, float lo, float hi)
        {
            for (int tries = 0; tries < 48; ++tries)
            {
                const uint16_t i = static_cast<uint16_t>(g_rng() % count);
                const Node n = node(nodes, i);
                const float d = std::hypot(n.x - cx, n.y - cy);
                if (!usable(n) || d < lo || d > hi || (n.x - cx) * fx + (n.y - cy) * fy < 0.5f * d)
                    continue;
                const uint16_t to = nextNode(nodes, count, links, i, 0xFFFFu);
                if (to == 0xFFFFu || to == i)
                    continue;
                const Node m = node(nodes, to);
                c.from = i;
                c.to = to;
                c.length = std::max(1.0f, std::hypot(m.x - n.x, m.y - n.y));
                c.t = std::uniform_real_distribution<float>(0.0f, 1.0f)(g_rng);
                c.speed = std::uniform_real_distribution<float>(8.0f, 15.0f)(g_rng);
                c.model = g_models.empty() ? 0u : g_models[g_rng() % g_models.size()];
                c.alive = true;
                return;
            }
            c.alive = false;
        }

        // the band: from past the real traffic (made 100-160 x the generation multiplier, taken away past 160 x) out to
        // the setting's distance
        void band(float &lo, float &hi)
        {
            static const char *forced = std::getenv("VCS_DISTANT_BAND"); // debug: "lo,hi" metres (proxies near, to look at)
            if (forced && std::sscanf(forced, "%f,%f", &lo, &hi) == 2)
                return;
            const float gen = std::max(1.0f, rdf(kGenMultiplier));
            lo = 170.0f * gen;
            hi = lo + 6.0f * static_cast<float>(g_setting.load(std::memory_order_relaxed));
        }

        void tick()
        {
            const int setting = g_setting.load(std::memory_order_relaxed);
            uint32_t nodes, count, links;
            if (setting <= 0 || !network(nodes, count, links))
            {
                std::lock_guard<std::mutex> lock(g_publishMutex);
                g_published.clear();
                g_publishedFrame = 0;
                return;
            }
            const float dt = std::clamp(rdf(kTimeStep), 0.0f, 4.0f) / 50.0f;
            const float cx = rdf(kCameraPos), cy = rdf(kCameraPos + 4u), cz = rdf(kCameraPos + 8u);
            float fx = rdf(kCameraFront), fy = rdf(kCameraFront + 4u);
            const float fl = std::max(0.001f, std::hypot(fx, fy));
            fx /= fl;
            fy /= fl;
            float lo, hi;
            band(lo, hi);
            uint32_t placed = 0; // a few a frame: a turned camera fills its view over a second, not all at once
            const uint32_t wanted = std::min<uint32_t>(kMaxProxies, static_cast<uint32_t>(setting) * 2u);
            std::vector<Proxy> out;
            out.reserve(wanted);
            for (uint32_t i = 0; i < kMaxProxies; ++i)
            {
                Car &c = g_cars[i];
                if (i >= wanted)
                {
                    c.alive = false;
                    continue;
                }
                if (!c.alive)
                {
                    if (placed >= 4u)
                        continue;
                    ++placed;
                    place(c, nodes, count, links, cx, cy, fx, fy, lo, hi);
                    if (!c.alive)
                        continue;
                }
                c.t += c.speed * dt / c.length;
                while (c.t >= 1.0f)
                {
                    const uint16_t next = nextNode(nodes, count, links, c.to, c.from);
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
                const float x = a.x + (b.x - a.x) * c.t + rx * kLane, y = a.y + (b.y - a.y) * c.t + ry * kLane;
                const float z = a.z + (b.z - a.z) * c.t + kCarHeight;
                const float d = std::hypot(x - cx, y - cy);
                if (d < lo * 0.9f || d > hi * 1.05f || (x - cx) * fx + (y - cy) * fy < -0.3f * d)
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
            std::lock_guard<std::mutex> lock(g_publishMutex);
            g_published.swap(out);
            g_publishedFrame = rd32(kFrameCounter) | 1u;
        }

        // before the original (DECISIONS.md, hooks)
        PS2_GUEST_CC void worldProcess(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_rdram = rdram;
            tick();
            g_worldProcess(rdram, ctx, runtime);
        }

        // ---- step A2: templates ------------------------------------------------------------------------------------
        // A car's draws are made between its CEntity::Render and the next entity's: a car opens a group, any other entity
        // closes it. Draws made later
        // for it (an alpha list) or for nothing (effects) after the last entity keep the group: the worker checks that a
        // draw sits on the car (its matrix within a few metres of the car's) before it takes it as a template.
        bool isVehicle(uint32_t entity)
        {
            const uint32_t pool = rd32(kVehiclePool);
            if (!pool)
                return false;
            const uint32_t entries = rd32(pool), size = rd32(pool + 8u);
            return entity >= entries && entity < entries + size * kVehicleSize && (entity - entries) % kVehicleSize == 0u;
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
            if (g_setting.load(std::memory_order_relaxed) <= 0 || g_rendered.size() >= 0xFFF0u || !isVehicle(entity))
                return;
            const uint32_t vtable = rd32(entity + 0x5Cu);
            if (vtable != kCarVtable && vtable != kCar2Vtable)
                return;
            Rendered r{};
            r.entity = entity;
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
            openGroup(GPR_U32(ctx, 4));
            g_entityRender(rdram, ctx, runtime);
        }


        void addTemplates()
        {
            const uint32_t pool = rd32(kVehiclePool);
            const uint32_t listed = rd32(kVehicleListCount);
            if (!pool || listed >= kVehicleListMax - 64u)
                return;
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
                const uint32_t e = entries + i * kVehicleSize;
                const uint32_t vt = rd32(e + 0x5Cu);
                if ((vt != kCarVtable && vt != kCar2Vtable) || !rd32(e + 0x50u))
                    continue;
                const float dx = rdf(e + 0x30u) - cx, dy = rdf(e + 0x34u) - cy, dz = rdf(e + 0x38u) - cz;
                const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (d > kTemplateRange)
                    continue;
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
                    ++count;
                    ++g_statAdded;
                }
            std::memcpy(g_rdram + (kVehicleListCount & PS2_RAM_MASK), &count, 4);
        }

        // before the original (DECISIONS.md, hooks)
        PS2_GUEST_CC void preRenderLists(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_rdram = rdram;
            if (g_setting.load(std::memory_order_relaxed) > 0)
                addTemplates();
            g_preRenderLists(rdram, ctx, runtime);
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

    bool enabled() { return g_setting.load(std::memory_order_relaxed) > 0; }

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
        // the templates: per model, the one drawn farthest from the camera (the fewest details)
        std::vector<uint32_t> templates; // indices into g_rendered
        for (uint32_t i = 0; i < g_rendered.size(); ++i)
        {
            const Rendered &r = g_rendered[i];
            auto at = std::find_if(templates.begin(), templates.end(), [&](uint32_t t) { return g_rendered[t].model == r.model; });
            if (at == templates.end())
                templates.push_back(i);
            else if (r.distance > g_rendered[*at].distance)
                *at = i;
        }
        std::sort(templates.begin(), templates.end(), [](uint32_t a, uint32_t b) { return g_rendered[a].model < g_rendered[b].model; });
        if (!templates.empty())
        {
            g_models.clear();
            for (uint32_t t : templates)
                g_models.push_back(g_rendered[t].model);
            std::vector<Proxy> proxies;
            {
                std::lock_guard<std::mutex> lock(g_publishMutex);
                proxies = g_published;
            }
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
    }

    void install(PS2Runtime &runtime)
    {
        if ((g_worldProcess = runtime.lookupFunction(kWorldProcess)))
            runtime.replaceFunction(kWorldProcess, &worldProcess);
        if ((g_entityRender = runtime.lookupFunction(kEntityRender)))
            runtime.replaceFunction(kEntityRender, &entityRender);
        if ((g_preRenderLists = runtime.lookupFunction(kPreRenderLists)))
            runtime.replaceFunction(kPreRenderLists, &preRenderLists);

        // proxies -> ok frame=<n> count=<n>, the templates / proxy draw counters, then "x,y,z,distance" for the first eight
        ps2control::addCommand("proxies", [](const std::string &, ps2control::Reply reply) {
            std::vector<Proxy> p;
            const uint32_t frame = snapshot(p);
            std::string text = "ok frame=" + std::to_string(frame) + " count=" + std::to_string(p.size()) +
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
    }
}
