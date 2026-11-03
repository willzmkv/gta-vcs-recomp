// See rig.h.
#include "debug/rig.h"
#include "game/vcs_freecam.h"
#include "game/vcs_trainer.h"

#include "ps2_runtime.h"
#include "runtime/ps2_control.h"
#include "runtime/gs/gs_hw.h" // gsdraws

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace vcs
{
    bool armWatch(const char *spec); // debug/watch.cpp
}

namespace vcs::rig
{
    namespace
    {
        constexpr uint32_t kRamSize = PS2_RAM_SIZE; // the EE RAM (runtime/ps2_memory.h)
        constexpr uint32_t kRamMask = kRamSize - 1u;

        uint8_t *g_rdram = nullptr;
        std::atomic<uint64_t> g_frames{0};
        std::atomic<bool> g_paused{false};
        std::mutex g_jobsMutex;
        std::deque<std::function<void(uint8_t *, R5900Context *, PS2Runtime *)>> g_jobs;
        FILE *g_trace = nullptr; // "trace": game thread only
        std::vector<std::pair<uint32_t, uint32_t>> g_traceSpans;

        void stopTrace()
        {
            if (g_trace)
                std::fclose(g_trace);
            g_trace = nullptr;
        }

        bool parseU32(const std::string &text, uint32_t &out)
        {
            if (text.empty())
                return false;
            char *end = nullptr;
            const unsigned long v = std::strtoul(text.c_str(), &end, 0);
            if (!end || *end)
                return false;
            out = static_cast<uint32_t>(v);
            return true;
        }

        std::vector<std::string> words(const std::string &args)
        {
            std::istringstream in(args);
            std::vector<std::string> out;
            for (std::string w; in >> w;)
                out.push_back(w);
            return out;
        }

        float loadF32(uint32_t addr)
        {
            float v;
            std::memcpy(&v, g_rdram + (addr & kRamMask), 4);
            return v;
        }

        std::string hex(const uint8_t *data, size_t n)
        {
            static const char *digits = "0123456789abcdef";
            std::string s;
            s.reserve(n * 2);
            for (size_t i = 0; i < n; ++i)
            {
                s.push_back(digits[data[i] >> 4]);
                s.push_back(digits[data[i] & 15]);
            }
            return s;
        }

        std::string fmt(const char *format, ...)
        {
            char buffer[512];
            va_list args;
            va_start(args, format);
            std::vsnprintf(buffer, sizeof(buffer), format, args);
            va_end(args);
            return buffer;
        }

        // Range arguments "lo hi" at words[i], defaulting to all of RAM.
        void range(const std::vector<std::string> &w, size_t i, uint32_t &lo, uint32_t &hi)
        {
            lo = 0x00100000u;
            hi = kRamSize;
            if (w.size() > i + 1)
            {
                parseU32(w[i], lo);
                parseU32(w[i + 1], hi);
            }
            lo &= ~3u;
            hi = std::min(hi, kRamSize);
        }

        void registerMemoryCommands()
        {
            // peek <addr> [bytes=64] -> ok <hex>
            ps2control::addCommand("peek", [](const std::string &args, ps2control::Reply reply) {
                const auto w = words(args);
                uint32_t addr = 0, n = 64;
                if (w.empty() || !parseU32(w[0], addr) || (w.size() > 1 && !parseU32(w[1], n)) || n == 0 || n > 4096)
                    return reply("error usage: peek <addr> [bytes<=4096]");
                if (!g_rdram)
                    return reply("error the game has not started a frame yet");
                addr &= kRamMask;
                n = std::min(n, kRamSize - addr);
                reply("ok " + hex(g_rdram + addr, n));
            });
            // peekf <addr> [count=4] -> ok f0 f1 ...
            ps2control::addCommand("peekf", [](const std::string &args, ps2control::Reply reply) {
                const auto w = words(args);
                uint32_t addr = 0, n = 4;
                if (w.empty() || !parseU32(w[0], addr) || (w.size() > 1 && !parseU32(w[1], n)) || n == 0 || n > 256)
                    return reply("error usage: peekf <addr> [count<=256]");
                if (!g_rdram)
                    return reply("error the game has not started a frame yet");
                std::string out = "ok";
                for (uint32_t i = 0; i < n; ++i)
                    out += fmt(" %.6g", loadF32(addr + i * 4));
                reply(out);
            });
            // poke <addr> <hex bytes> -> written at the next frame kick
            ps2control::addCommand("poke", [](const std::string &args, ps2control::Reply reply) {
                const auto w = words(args);
                uint32_t addr = 0;
                if (w.size() != 2 || !parseU32(w[0], addr) || w[1].size() % 2 || w[1].empty())
                    return reply("error usage: poke <addr> <hexbytes>");
                std::vector<uint8_t> bytes;
                for (size_t i = 0; i < w[1].size(); i += 2)
                    bytes.push_back(static_cast<uint8_t>(std::strtoul(w[1].substr(i, 2).c_str(), nullptr, 16)));
                runOnGameThread([addr, bytes, reply](uint8_t *rdram, R5900Context *, PS2Runtime *) {
                    for (size_t i = 0; i < bytes.size(); ++i)
                        rdram[(addr + i) & kRamMask] = bytes[i];
                    reply("ok");
                });
            });
            // pokef <addr> <f> [f...]
            ps2control::addCommand("pokef", [](const std::string &args, ps2control::Reply reply) {
                const auto w = words(args);
                uint32_t addr = 0;
                if (w.size() < 2 || !parseU32(w[0], addr))
                    return reply("error usage: pokef <addr> <float> [float...]");
                std::vector<float> values;
                for (size_t i = 1; i < w.size(); ++i)
                    values.push_back(std::strtof(w[i].c_str(), nullptr));
                runOnGameThread([addr, values, reply](uint8_t *rdram, R5900Context *, PS2Runtime *) {
                    for (size_t i = 0; i < values.size(); ++i)
                        std::memcpy(rdram + ((addr + i * 4) & kRamMask), &values[i], 4);
                    reply("ok");
                });
            });
            // campose <x> <y> <z> <yaw> <pitch> | off -> ok: the free camera there, looking along yaw / pitch (degrees; yaw from
            // +x toward +y) - pictures from a chosen place (tools/carmod_check.py); off: back behind the player
            ps2control::addCommand("campose", [](const std::string &args, ps2control::Reply reply) {
                const auto w = words(args);
                if (w.size() == 1 && w[0] == "off")
                {
                    trainer::setToggle(trainer::FreeCamera, false);
                    return reply("ok");
                }
                if (w.size() < 5)
                    return reply("error usage: campose <x> <y> <z> <yaw> <pitch> | off");
                float v[5];
                for (int i = 0; i < 5; ++i)
                    v[i] = std::strtof(w[static_cast<size_t>(i)].c_str(), nullptr);
                constexpr float kRad = 3.14159265f / 180.0f;
                freecam::place(v[0], v[1], v[2], v[3] * kRad, v[4] * kRad);
                trainer::setToggle(trainer::FreeCamera, true);
                reply("ok");
            });
            // findvec <x> <y> <z> <tolerance> [lo hi] -> ok addr addr ... (three consecutive floats near the point)
            ps2control::addCommand("findvec", [](const std::string &args, ps2control::Reply reply) {
                const auto w = words(args);
                if (w.size() < 4 || !g_rdram)
                    return reply("error usage: findvec <x> <y> <z> <tolerance> [lo hi]");
                const float x = std::strtof(w[0].c_str(), nullptr), y = std::strtof(w[1].c_str(), nullptr),
                            z = std::strtof(w[2].c_str(), nullptr), tol = std::strtof(w[3].c_str(), nullptr);
                uint32_t lo, hi;
                range(w, 4, lo, hi);
                std::string out = "ok";
                int found = 0;
                for (uint32_t a = lo; a + 12 <= hi && found < 400; a += 4)
                {
                    const float fx = loadF32(a);
                    if (!(std::fabs(fx - x) <= tol))
                        continue;
                    if (std::fabs(loadF32(a + 4) - y) <= tol && std::fabs(loadF32(a + 8) - z) <= tol)
                    {
                        out += fmt(" %08x", a);
                        ++found;
                    }
                }
                reply(out);
            });
            // findu32 <value> [lo hi] -> ok addr ...
            ps2control::addCommand("findu32", [](const std::string &args, ps2control::Reply reply) {
                const auto w = words(args);
                uint32_t value = 0;
                if (w.empty() || !parseU32(w[0], value) || !g_rdram)
                    return reply("error usage: findu32 <value> [lo hi]");
                uint32_t lo, hi;
                range(w, 1, lo, hi);
                std::string out = "ok";
                int found = 0;
                for (uint32_t a = lo; a + 4 <= hi && found < 400; a += 4)
                {
                    uint32_t v;
                    std::memcpy(&v, g_rdram + a, 4);
                    if (v == value)
                    {
                        out += fmt(" %08x", a);
                        ++found;
                    }
                }
                reply(out);
            });
            // findrange <vlo> <vhi> [lo hi] -> ok addr ... (u32 words with vlo <= value < vhi: pointers into a block)
            ps2control::addCommand("findrange", [](const std::string &args, ps2control::Reply reply) {
                const auto w = words(args);
                uint32_t vlo = 0, vhi = 0;
                if (w.size() < 2 || !parseU32(w[0], vlo) || !parseU32(w[1], vhi) || !g_rdram)
                    return reply("error usage: findrange <vlo> <vhi> [lo hi]");
                uint32_t lo, hi;
                range(w, 2, lo, hi);
                std::string out = "ok";
                int found = 0;
                for (uint32_t a = lo; a + 4 <= hi && found < 2000; a += 4)
                {
                    uint32_t v;
                    std::memcpy(&v, g_rdram + a, 4);
                    if (v >= vlo && v < vhi)
                    {
                        out += fmt(" %08x", a);
                        ++found;
                    }
                }
                reply(out);
            });
            // dump <path> [addr len] -> RAM (all 32 MB by default) to a file, taken on the game thread between frames.
            // Game memory: keep dumps under build/analysis/ (never committed).
            ps2control::addCommand("dump", [](const std::string &args, ps2control::Reply reply) {
                // the path is everything before an optional "addr len" pair: it may hold spaces
                const auto w = words(args);
                uint32_t addr = 0, len = kRamSize;
                size_t pathWords = w.size();
                if (w.size() >= 3 && parseU32(w[w.size() - 2], addr) && parseU32(w[w.size() - 1], len))
                    pathWords -= 2;
                else
                    addr = 0, len = kRamSize;
                if (pathWords == 0)
                    return reply("error usage: dump <path> [addr len]");
                addr &= kRamMask;
                len = std::min(len, kRamSize - addr);
                std::string path = w[0];
                for (size_t i = 1; i < pathWords; ++i)
                    path += " " + w[i];
                runOnGameThread([path, addr, len, reply](uint8_t *rdram, R5900Context *, PS2Runtime *) {
                    FILE *f = std::fopen(path.c_str(), "wb");
                    if (!f)
                        return reply("error cannot write " + path);
                    const size_t n = std::fwrite(rdram + addr, 1, len, f);
                    std::fclose(f);
                    reply(n == len ? fmt("ok %u", len) : std::string("error short write"));
                });
            });
            // pause on|off: hold the game thread at the frame kick (the window keeps showing the last frame)
            ps2control::addCommand("pause", [](const std::string &args, ps2control::Reply reply) {
                if (args != "on" && args != "off")
                    return reply("error usage: pause on|off");
                g_paused.store(args == "on");
                reply("ok");
            });
            // frames -> ok <game frames since start>
            ps2control::addCommand("frames", [](const std::string &, ps2control::Reply reply) {
                reply(fmt("ok %llu", static_cast<unsigned long long>(g_frames.load())));
            });
            // trace <path> <addr> <count> [<addr> <count> ...] | trace off -> every frame (at the kick) one line to
            // <path>: frame, game clock ms, time step, then the floats. Per-frame physics without missing frames.
            ps2control::addCommand("trace", [](const std::string &args, ps2control::Reply reply) {
                const auto w = words(args);
                std::vector<std::pair<uint32_t, uint32_t>> spans;
                if (w.size() == 1 && w[0] == "off")
                {
                    runOnGameThread([reply](uint8_t *, R5900Context *, PS2Runtime *) {
                        stopTrace();
                        reply("ok");
                    });
                    return;
                }
                for (size_t i = 1; i + 1 < w.size(); i += 2)
                {
                    uint32_t addr = 0, n = 0;
                    if (!parseU32(w[i], addr) || !parseU32(w[i + 1], n) || n == 0 || n > 64)
                        return reply("error usage: trace <path> <addr> <count<=64> [...] | trace off");
                    spans.emplace_back(addr, n);
                }
                if (spans.empty() || w.size() % 2 == 0)
                    return reply("error usage: trace <path> <addr> <count<=64> [...] | trace off");
                const std::string path = w[0];
                runOnGameThread([path, spans, reply](uint8_t *, R5900Context *, PS2Runtime *) {
                    stopTrace();
                    g_trace = std::fopen(path.c_str(), "w");
                    if (!g_trace)
                        return reply("error cannot write " + path);
                    g_traceSpans = spans;
                    reply("ok");
                });
            });
        }
    }

    namespace
    {
        // ---- The game's script VM (docs/game-internals.md section 11) -------------------------------------------
        constexpr uint32_t kScriptSpacePtr = 0x0048741Cu; // -> ScriptSpace (MAIN.SCM from file offset 8)
        constexpr uint32_t kActiveScripts = 0x004CD3C4u;  // CRunningScript list heads (+0 next, +4 prev)
        constexpr uint32_t kIdleScripts = 0x004CD3C8u;
        constexpr uint32_t kNextScriptId = 0x0048742Cu;
        constexpr uint32_t kMissionRunning = 0x004CD3E7u; // u8, set by LOAD_AND_LAUNCH_MISSION_INTERNAL
        constexpr uint32_t kPlayerPed = 0x004E4910u;      // CWorld::Players[0].m_pPed
        constexpr uint32_t kScriptSize = 0x218u;
        // A rig script's code lives in its own locals 0x30.. (+0x114..+0x1c4): locals 0..0x2f stay free for outputs,
        // the timers (locals 0x68 / 0x69 at +0x1f4) are not touched.
        constexpr uint32_t kCodeOffset = 0x114u;
        constexpr uint32_t kCodeMax = 0x1c4u - kCodeOffset;

        uint32_t rd32(uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }
        void wr32(uint8_t *ram, uint32_t a, uint32_t v) { std::memcpy(ram + (a & kRamMask), &v, 4); }

        // CTheScripts::StartNewScript (FUN_00255950) done from the host: take an idle script, Init it (FUN_002551d0),
        // set its IP and put it at the head of the active list. Game thread, between frames only.
        uint32_t startScript(uint8_t *ram, uint32_t ip, const char *name)
        {
            const uint32_t s = rd32(ram, kIdleScripts);
            if (!s)
                return 0;
            const uint32_t next = rd32(ram, s), prev = rd32(ram, s + 4); // unlink from the idle list
            if (prev)
                wr32(ram, prev, next);
            else
                wr32(ram, kIdleScripts, next);
            if (next)
                wr32(ram, next + 4, prev);
            const uint32_t id = rd32(ram, kNextScriptId);
            wr32(ram, kNextScriptId, id + 1);
            std::memset(ram + (s & kRamMask), 0, kScriptSize); // Init: every field it sets is 0 except +0x20d
            wr32(ram, s + 8, id);
            ram[(s + 0x20d) & kRamMask] = 1;
            std::strncpy(reinterpret_cast<char *>(ram + ((s + 0x20f) & kRamMask)), name, 7);
            wr32(ram, s + 0x10, ip);
            const uint32_t head = rd32(ram, kActiveScripts); // add at the head of the active list
            wr32(ram, s, head);
            wr32(ram, s + 4, 0);
            if (head)
                wr32(ram, head + 4, s);
            wr32(ram, kActiveScripts, s);
            ram[(s + 0x208) & kRamMask] = 1;
            return s;
        }

        struct PendingScript
        {
            uint32_t script = 0;
            uint64_t startFrame = 0;
            ps2control::Reply reply;
        };
        std::vector<PendingScript> g_pending; // game thread

        void checkPendingScripts(uint8_t *ram)
        {
            for (auto it = g_pending.begin(); it != g_pending.end();)
            {
                // TERMINATE_THIS_SCRIPT clears +0x208 and moves the script to the idle list (or a save load took the slot)
                if (!scriptActive(ram, it->script))
                {
                    it->reply(fmt("ok %08x", it->script)); // its outputs: locals at +0x54 of that slot
                    it = g_pending.erase(it);
                }
                else if (g_frames.load() - it->startFrame > 1800) // 60 s at 30 fps: still running
                {
                    it->reply(fmt("error script %08x still running after 1800 frames", it->script));
                    it = g_pending.erase(it);
                }
                else
                    ++it;
            }
        }

        void registerScriptCommands()
        {
            // scm <hex bytecode> -> ok <script slot>: runs it as a new script; answers when the script terminated (end it
            // with 23 00). Its outputs (locals) stay in the slot until the game starts another script in it.
            ps2control::addCommand("scm", [](const std::string &args, ps2control::Reply reply) {
                const auto w = words(args);
                std::string code;
                for (const auto &part : w)
                    code += part;
                if (code.empty() || code.size() % 2 || code.size() / 2 > kCodeMax)
                    return reply(fmt("error usage: scm <hex bytecode, at most %u bytes>", kCodeMax));
                std::vector<uint8_t> bytes;
                for (size_t i = 0; i < code.size(); i += 2)
                    bytes.push_back(static_cast<uint8_t>(std::strtoul(code.substr(i, 2).c_str(), nullptr, 16)));
                runOnGameThread([bytes, reply](uint8_t *ram, R5900Context *, PS2Runtime *) {
                    if (!rd32(ram, kScriptSpacePtr))
                        return reply("error the script engine is not running yet");
                    const uint32_t s = startCodeScript(ram, bytes.data(), bytes.size(), "rig");
                    if (!s)
                        return reply("error no free script slot");
                    g_pending.push_back({s, g_frames.load(), reply});
                });
            });
            // scripts -> ok <id>:<name>:<ip>:<mission> ...
            ps2control::addCommand("scripts", [](const std::string &, ps2control::Reply reply) {
                if (!g_rdram)
                    return reply("error the game has not started a frame yet");
                std::string out = "ok";
                uint32_t s = rd32(g_rdram, kActiveScripts);
                for (int n = 0; s && n < 256; ++n, s = rd32(g_rdram, s))
                {
                    char name[9] = {};
                    std::memcpy(name, g_rdram + ((s + 0x20f) & kRamMask), 8);
                    out += fmt(" %u:%s:%x:%u", rd32(g_rdram, s + 8), name, rd32(g_rdram, s + 0x10),
                               g_rdram[(s + 0x20a) & kRamMask]);
                }
                reply(out);
            });
            // player -> ok <ped> <x> <y> <z> <mission running 0|1>
            ps2control::addCommand("player", [](const std::string &, ps2control::Reply reply) {
                if (!g_rdram)
                    return reply("error the game has not started a frame yet");
                const uint32_t ped = rd32(g_rdram, kPlayerPed);
                if (!ped)
                    return reply("error no player");
                reply(fmt("ok %08x %.3f %.3f %.3f %u", ped, loadF32(ped + 0x30), loadF32(ped + 0x34), loadF32(ped + 0x38),
                          g_rdram[kMissionRunning]));
            });
        }
    }

    namespace
    {
        // ---- Cruise: the player's car moved along a recorded route at a fixed speed (tools/cruise.py) ----------------
        // Every game frame the car is put at the route's point a distance `speed * step` further on, facing along the
        // route, with the move speed of that pace: streaming, the camera and the renderer see a car driving the same
        // roads at the same speed every run - repeatable stutter measurements. The route is the car's own positions from
        // an AI drive (so on roads, z included).
        constexpr uint32_t kPedVehicle = 0x480u;     // CPed: the vehicle it is in (game_info.cpp)
        constexpr uint32_t kMoveSpeed = 0x140u;      // CPhysical: move speed, units per 1/50 s (game-internals.md 4)
        constexpr uint32_t kTimeStep = 0x004CD110u;  // CTimer::ms_fTimeStep (1/50 s units)

        struct Cruise
        {
            std::vector<float> x, y, z, s; // the route; s: distance along it
            float speed = 0.0f;            // m/s
            float at = 0.0f;               // distance done
            bool loop = false, active = false, done = false;
            uint64_t frames = 0;
        };
        Cruise g_cruise; // game thread (commands go through runOnGameThread)

        void cruisePoint(float d, float &x, float &y, float &z)
        {
            const auto &c = g_cruise;
            const size_t n = c.s.size();
            if (d <= 0.0f)
                d = 0.0f;
            const size_t i = std::min<size_t>(n - 1, static_cast<size_t>(std::upper_bound(c.s.begin(), c.s.end(), d) - c.s.begin()));
            if (i == 0 || d >= c.s[n - 1])
            {
                const size_t j = d >= c.s[n - 1] ? n - 1 : 0;
                x = c.x[j], y = c.y[j], z = c.z[j];
                return;
            }
            const float len = c.s[i] - c.s[i - 1], t = len > 0.0f ? (d - c.s[i - 1]) / len : 0.0f;
            x = c.x[i - 1] + (c.x[i] - c.x[i - 1]) * t;
            y = c.y[i - 1] + (c.y[i] - c.y[i - 1]) * t;
            z = c.z[i - 1] + (c.z[i] - c.z[i - 1]) * t;
        }

        void cruiseFrame(uint8_t *ram)
        {
            auto &c = g_cruise;
            if (!c.active)
                return;
            const uint32_t ped = rd32(ram, kPlayerPed);
            const uint32_t veh = ped ? rd32(ram, ped + kPedVehicle) : 0;
            if (!veh)
                return; // not in a car (yet): wait
            float step;
            std::memcpy(&step, ram + kTimeStep, 4);
            const float total = c.s.back();
            c.at += c.speed * std::clamp(step, 0.0f, 3.0f) / 50.0f;
            if (c.at >= total)
            {
                if (!c.loop)
                {
                    c.at = total, c.active = false, c.done = true;
                    return;
                }
                c.at = std::fmod(c.at, total);
            }
            ++c.frames;
            float x, y, z, ax, ay, az;
            cruisePoint(c.at, x, y, z);
            cruisePoint(c.at + 8.0f, ax, ay, az); // facing a few metres ahead: smooth through the recorded corners
            float fx = ax - x, fy = ay - y, fz = az - z;
            const float fl = std::sqrt(fx * fx + fy * fy + fz * fz);
            if (fl < 1e-3f)
                return;
            fx /= fl, fy /= fl, fz /= fl;
            const float hl = std::sqrt(fx * fx + fy * fy);
            const float rx = hl > 1e-4f ? fy / hl : 1.0f, ry = hl > 1e-4f ? -fx / hl : 0.0f; // right = forward x up
            const float ux = ry * fz, uy = -rx * fz, uz = rx * fy - ry * fx;                  // up = right x forward
            const float m[16] = {rx, ry, 0.0f, 0.0f, fx, fy, fz, 0.0f, ux, uy, uz, 0.0f, x, y, z + 0.05f, 1.0f};
            for (int i = 0; i < 15; ++i)
                if ((i & 3) != 3)
                    std::memcpy(ram + ((veh + i * 4) & kRamMask), &m[i], 4);
            const float v = c.speed / 50.0f, mv[3] = {fx * v, fy * v, fz * v};
            std::memcpy(ram + ((veh + kMoveSpeed) & kRamMask), mv, sizeof(mv));
        }

        void registerCruiseCommands()
        {
            // cruise start <route file: "x y z" per line> <speed m/s> [loop] [from <metres>] -> ok <points> <metres>
            // cruise stop -> ok; cruise status -> ok <active 0|1> <done 0|1> <metres done> <metres total> <frames>
            ps2control::addCommand("cruise", [](const std::string &args, ps2control::Reply reply) {
                const auto w = words(args);
                if (w.empty())
                    return reply("error usage: cruise start <file> <speed> [loop] [from <m>] | stop | status");
                if (w[0] == "start" && w.size() >= 3)
                {
                    // the file is every word before the speed (the first number): a path may hold spaces
                    size_t speedAt = 2;
                    for (; speedAt < w.size(); ++speedAt)
                    {
                        char *end = nullptr;
                        std::strtof(w[speedAt].c_str(), &end);
                        if (end && *end == '\0' && end != w[speedAt].c_str())
                            break;
                    }
                    if (speedAt >= w.size())
                        return reply("error usage: cruise start <file> <speed> [loop] [from <m>] | stop | status");
                    std::string path = w[1];
                    for (size_t i = 2; i < speedAt; ++i)
                        path += " " + w[i];
                    Cruise c;
                    FILE *f = std::fopen(path.c_str(), "r");
                    if (!f)
                        return reply("error cannot read " + path);
                    for (float px, py, pz; std::fscanf(f, "%f %f %f", &px, &py, &pz) == 3;)
                    {
                        if (!c.x.empty())
                        {
                            const float dx = px - c.x.back(), dy = py - c.y.back(), dz = pz - c.z.back();
                            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                            if (d < 0.25f)
                                continue;
                            c.s.push_back(c.s.back() + d);
                        }
                        else
                            c.s.push_back(0.0f);
                        c.x.push_back(px), c.y.push_back(py), c.z.push_back(pz);
                    }
                    std::fclose(f);
                    if (c.x.size() < 2)
                        return reply("error the route needs two points or more");
                    c.speed = std::strtof(w[speedAt].c_str(), nullptr);
                    for (size_t i = speedAt + 1; i < w.size(); ++i)
                        if (w[i] == "loop")
                            c.loop = true;
                        else if (w[i] == "from" && i + 1 < w.size())
                            c.at = std::strtof(w[++i].c_str(), nullptr);
                    c.active = true;
                    const std::string ok = fmt("ok %zu %.0f", c.x.size(), c.s.back());
                    runOnGameThread([c = std::move(c), ok, reply](uint8_t *, R5900Context *, PS2Runtime *) mutable {
                        g_cruise = std::move(c);
                        reply(ok);
                    });
                    return;
                }
                if (w[0] == "stop")
                    return runOnGameThread([reply](uint8_t *, R5900Context *, PS2Runtime *) {
                        g_cruise.active = false;
                        reply("ok");
                    });
                if (w[0] == "status")
                    return runOnGameThread([reply](uint8_t *, R5900Context *, PS2Runtime *) {
                        const auto &c = g_cruise;
                        reply(fmt("ok %d %d %.1f %.1f %llu", c.active ? 1 : 0, c.done ? 1 : 0, c.at, c.s.empty() ? 0.0f : c.s.back(),
                                  static_cast<unsigned long long>(c.frames)));
                    });
                reply("error usage: cruise start <file> <speed> [loop] [from <m>] | stop | status");
            });
        }
    }

    uint32_t startScriptAt(uint8_t *ram, uint32_t ip, const char *name, uint32_t returnIp)
    {
        if (!rd32(ram, kScriptSpacePtr) || !rd32(ram, kIdleScripts))
            return 0;
        const uint32_t s = startScript(ram, ip, name);
        if (s && returnIp) // the GOSUB stack (+0x14) holds a GOSUB's return IP as it is; +0x204 (u16) is its depth
        {
            wr32(ram, s + 0x14, returnIp);
            ram[(s + 0x204) & kRamMask] = 1;
        }
        return s;
    }

    uint32_t startCodeScript(uint8_t *ram, const uint8_t *code, size_t bytes, const char *name)
    {
        const uint32_t space = rd32(ram, kScriptSpacePtr);
        const uint32_t idle = rd32(ram, kIdleScripts);
        if (!space || !idle || bytes > kCodeMax)
            return 0;
        // the code goes into the new script's locals, so take the slot first and write after Init
        const uint32_t s = startScript(ram, idle + kCodeOffset - space, name);
        std::memcpy(ram + ((s + kCodeOffset) & kRamMask), code, bytes);
        return s;
    }

    // Active, and its IP still in the slot's own code: the game reuses a finished script's slot, and a save load fills
    // the slots afresh with the save's scripts (game-internals.md 11d), so a slot remembered from before may hold one
    // of the game's scripts now - never to be taken for ours (cancelling it unlinked a script the load had just
    // started: the Load Game hang).
    bool scriptActive(const uint8_t *ram, uint32_t script)
    {
        if (!script || ram[(script + 0x208) & kRamMask] == 0)
            return false;
        uint32_t space, ip;
        std::memcpy(&space, ram + (kScriptSpacePtr & kRamMask), 4);
        std::memcpy(&ip, ram + ((script + 0x10) & kRamMask), 4);
        const uint32_t at = space + ip;
        if (at >= script + kCodeOffset && at < script + kCodeOffset + kCodeMax)
            return true;
        static uint32_t s_logged = 0;
        if (s_logged != script)
        {
            char name[9] = {};
            std::memcpy(name, ram + ((script + 0x20f) & kRamMask), 8);
            std::fprintf(stderr, "[rig] script slot %08x holds the game's '%s' (ip %x) now, not our code\n", script, name, ip);
            s_logged = script;
        }
        return false;
    }

    void cancelCodeScript(uint8_t *ram, uint32_t s)
    {
        if (!scriptActive(ram, s))
            return;
        const uint32_t next = rd32(ram, s), prev = rd32(ram, s + 4); // out of the active list
        if (prev)
            wr32(ram, prev, next);
        else
            wr32(ram, kActiveScripts, next);
        if (next)
            wr32(ram, next + 4, prev);
        const uint32_t head = rd32(ram, kIdleScripts); // back at the head of the idle list, as TERMINATE does
        wr32(ram, s, head);
        wr32(ram, s + 4, 0);
        if (head)
            wr32(ram, head + 4, s);
        wr32(ram, kIdleScripts, s);
        ram[(s + 0x208) & kRamMask] = 0;
    }

    void runOnGameThread(std::function<void(uint8_t *, R5900Context *, PS2Runtime *)> job)
    {
        std::lock_guard<std::mutex> lock(g_jobsMutex);
        g_jobs.push_back(std::move(job));
    }

    uint64_t gameFrames() { return g_frames.load(std::memory_order_relaxed); }

    void install(PS2Runtime &runtime)
    {
        g_rdram = runtime.memory().getRDRAM();
        registerMemoryCommands();
        registerScriptCommands();
        registerCruiseCommands();
        // watch <lo>,<hi>[,<value>[,<mask>]] -> ok: arm the guest write watch now (debug/watch.cpp; VCS_WATCH at start)
        ps2control::addCommand("watch", [](const std::string &args, ps2control::Reply reply) {
            const std::string spec = args;
            runOnGameThread([spec, reply](uint8_t *, R5900Context *, PS2Runtime *) {
                reply(vcs::armWatch(spec.c_str()) ? "ok" : "error usage: watch <lo>,<hi>[,<value>[,<mask>]] (hex)");
            });
        });
        // gsdraws [reset] -> ok <the draws still made from GS primitives, by kind, per frame since the last reset>
        // (gsHwDrawSourceReport; lines joined by " ;; "). The first call starts the table.
        ps2control::addCommand("gsdraws", [](const std::string &args, ps2control::Reply reply) {
            std::string r = gsHwDrawSourceReport(words(args) == std::vector<std::string>{"reset"});
            std::string out;
            for (size_t at = 0; at < r.size();)
            {
                size_t end = r.find('\n', at);
                if (end == std::string::npos)
                    end = r.size();
                std::string line = r.substr(at, end - at);
                if (line.rfind("[gsdraws] ", 0) == 0)
                    line.erase(0, 10);
                if (!line.empty())
                    out += (out.empty() ? "" : " ;; ") + line;
                at = end + 1;
            }
            reply("ok " + out);
        });
    }

    PS2_GUEST_CC void onGameFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_rdram = rdram;
        g_frames.fetch_add(1, std::memory_order_relaxed);
        checkPendingScripts(rdram);
        cruiseFrame(rdram);
        if (g_trace)
        {
            std::fprintf(g_trace, "%llu %u %.6g", static_cast<unsigned long long>(g_frames.load()), rd32(rdram, 0x4CD104u),
                         loadF32(0x4CD110u)); // CTimer: ms clock, ms_fTimeStep
            for (const auto &[addr, n] : g_traceSpans)
                for (uint32_t i = 0; i < n; ++i)
                    std::fprintf(g_trace, " %.6g", loadF32(addr + i * 4));
            std::fputc('\n', g_trace);
        }
        for (;;)
        {
            std::deque<std::function<void(uint8_t *, R5900Context *, PS2Runtime *)>> jobs;
            {
                std::lock_guard<std::mutex> lock(g_jobsMutex);
                jobs.swap(g_jobs);
            }
            for (auto &job : jobs)
                job(rdram, ctx, runtime);
            if (!g_paused.load(std::memory_order_relaxed) || runtime->isStopRequested())
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5)); // paused: keep serving jobs
        }
    }
}
