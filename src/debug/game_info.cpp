// See game_info.h.
#include "debug/game_info.h"

#include "debug/rig.h"
#include "ps2_runtime.h"
#include "runtime/ps2_control.h"
#include "runtime/ps2_perf.h"

#include "imgui.h" // IM_COL32

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace vcs::gameinfo
{
    namespace
    {
        constexpr uint32_t kRamMask = PS2_RAM_MASK;

        // ---- Game addresses (game-internals.md 4, 11, 12, 12a, 12b) -----------------------------------------------
        constexpr uint32_t kPlayerPed = 0x004E4910u;      // CWorld::Players[0].m_pPed
        constexpr uint32_t kMoney = 0x004E49CCu;          // CPlayerInfo: the player's money
        constexpr uint32_t kGameTimeMs = 0x004CD104u;     // CTimer::m_snTimeInMilliseconds (stands still while paused)
        constexpr uint32_t kTimeStep = 0x004CD110u;       // CTimer::ms_fTimeStep (1.0 = 1/50 s)
        constexpr uint32_t kFrameCounter = 0x004CD124u;   // CTimer::m_FrameCounter
        constexpr uint32_t kUserPause = 0x004CD2ACu;      // CTimer::m_UserPause (the pause menu), byte
        constexpr uint32_t kCodePause = 0x004CD11Cu;      // CTimer::m_CodePause, byte
        constexpr uint32_t kClockHours = 0x004CD148u;     // CClock: hours, minutes (bytes)
        constexpr uint32_t kCurrArea = 0x00489F7Cu;       // CGame::currArea (0 outdoors)
        constexpr uint32_t kLevelWanted = 0x004CD118u;    // the world level the player's zone asks for (6b)
        constexpr uint32_t kMissionRunning = 0x004CD3E7u; // u8, set by LOAD_AND_LAUNCH_MISSION_INTERNAL
        constexpr uint32_t kActiveScripts = 0x004CD3C4u;  // CRunningScript active list (newest first): +0 next, +0x10
                                                          // IP, +0x20f name
        constexpr uint32_t kPad0 = 0x005147A8u;           // CPad 0: +0x96 DisablePlayerControls (s16)
        constexpr uint32_t kModelInfos = 0x00486518u;     // -> CModelInfo::ms_modelInfoPtrs
        constexpr uint32_t kModelGameName = 0x158u;       // vehicle model info: its text key (8 chars)
        constexpr uint32_t kPedPool = 0x00487A94u;        // -> CPool {entries, flags, size}; flag bit 0x80 = free
        constexpr uint32_t kVehiclePool = 0x00487A98u;
        constexpr uint32_t kCamera = 0x006F44D0u;         // TheCamera: +0x30 position, +0x50 active CCam (byte),
                                                          // +0x70 Cams[3] (0x270 each), +0x888 m_bLookingAtPlayer
        // entities: matrix right +0, forward +0x10, up +0x20, position +0x30; +0x56 model index
        constexpr uint32_t kPedVehicle = 0x480u, kPedHealth = 0x4E4u, kPedArmour = 0x4E8u, kPedState = 0x8C4u;
        constexpr uint32_t kPedWeapons = 0x578u, kWeaponSlotSize = 0x1Cu; // 10 slots {type, state, clip, total}
        constexpr uint32_t kCarHealth = 0x278u, kCarVelocity = 0x140u;     // velocity: units per 1/50 s

        const uint8_t *g_rdram = nullptr;

        // snapshot (game thread writes, main thread reads)
        std::mutex g_mutex;
        std::vector<ps2perf::InfoLine> g_lines;
        std::chrono::steady_clock::time_point g_linesAt;
        std::atomic<int64_t> g_wantedUntil{0}; // steady_clock ticks: the panel asked recently

        int64_t ticksNow() { return std::chrono::steady_clock::now().time_since_epoch().count(); }

        uint32_t rd32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }
        int16_t rd16(const uint8_t *ram, uint32_t a)
        {
            int16_t v;
            std::memcpy(&v, ram + (a & kRamMask), 2);
            return v;
        }
        float rdf(const uint8_t *ram, uint32_t a)
        {
            float v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }
        uint8_t rd8(const uint8_t *ram, uint32_t a) { return ram[a & kRamMask]; }
        std::string name8(const uint8_t *ram, uint32_t a)
        {
            char s[9] = {};
            std::memcpy(s, ram + (a & kRamMask), 8);
            for (char &c : s)
                if (c && (c < 0x20 || c > 0x7E))
                    c = '?';
            return s;
        }

        std::string fmt(const char *f, ...)
        {
            char buf[320];
            va_list ap;
            va_start(ap, f);
            std::vsnprintf(buf, sizeof(buf), f, ap);
            va_end(ap);
            return buf;
        }

        // GTA's heading: 0 = north (+y), counter-clockwise, in degrees 0..360 (as GET_CHAR_HEADING answers)
        float headingDeg(float fx, float fy)
        {
            float h = std::atan2(-fx, fy) * 57.2957795f;
            return h < 0.0f ? h + 360.0f : h + 0.0f; // + 0: no "-0.0"
        }

        const char *pedStateName(uint32_t s)
        {
            switch (s)
            {
            case 1: return "on foot";
            case 0x37: return "driving";
            case 0x39: return "dying";
            case 0x3A: return "dead";
            default: return "";
            }
        }

        const char *cameraModeName(int m) // VC's numbering (game-internals.md 12b)
        {
            switch (m)
            {
            case 4: return "follow ped";
            case 11: return "lock-on";
            case 15: return "fixed";
            case 16: return "first person";
            case 17: return "flyby / cutscene";
            case 18: return "behind vehicle";
            default: return "";
            }
        }

        const char *levelName(uint32_t l)
        {
            switch (l)
            {
            case 1: return "BEACH";
            case 2: return "MAINLA";
            case 3: return "MALL";
            default: return "?";
            }
        }

        // pool entries in use / size (0 / 0 before the pool exists)
        void poolUse(const uint8_t *ram, uint32_t poolPtr, int &used, int &size)
        {
            used = size = 0;
            const uint32_t pool = rd32(ram, poolPtr);
            if (!pool)
                return;
            const uint32_t flags = rd32(ram, pool + 4);
            size = static_cast<int>(rd32(ram, pool + 8));
            if (!flags || size <= 0 || size > 4096)
            {
                size = 0;
                return;
            }
            for (int i = 0; i < size; ++i)
                used += (rd8(ram, flags + i) & 0x80) ? 0 : 1;
        }

        std::vector<ps2perf::InfoLine> build(const uint8_t *ram)
        {
            using Line = ps2perf::InfoLine;
            const uint32_t warn = IM_COL32(240, 210, 90, 255), bad = IM_COL32(245, 100, 90, 255);
            std::vector<Line> out;
            auto add = [&](std::string t, uint32_t color = 0) { out.push_back({std::move(t), color, false}); };
            auto header = [&](std::string t) { out.push_back({std::move(t), 0, true}); };

            // ---- player
            const uint32_t ped = rd32(ram, kPlayerPed);
            const uint32_t veh = ped ? rd32(ram, ped + kPedVehicle) : 0;
            const uint32_t state = ped ? rd32(ram, ped + kPedState) : 0;
            const bool driving = veh && state == 0x37;
            if (!ped)
            {
                header("PLAYER");
                add("no player ped yet", warn);
            }
            else
            {
                header(fmt("PLAYER  ped %08x  model %d  state %s (0x%x)", ped, rd16(ram, ped + 0x56), pedStateName(state), state));
                const float x = rdf(ram, ped + 0x30), y = rdf(ram, ped + 0x34), z = rdf(ram, ped + 0x38);
                if (driving)
                    add("pos   in the vehicle (the ped's own matrix is relative)");
                else
                    add(fmt("pos   %9.2f %9.2f %8.2f   heading %5.1f", x, y, z, headingDeg(rdf(ram, ped + 0x10), rdf(ram, ped + 0x14))));
                const float health = rdf(ram, ped + kPedHealth), armour = rdf(ram, ped + kPedArmour);
                add(fmt("health %5.1f  armour %5.1f  money $%d", health, armour, static_cast<int32_t>(rd32(ram, kMoney))),
                    health < 25.0f ? bad : 0);
                std::string weapons = "weapons";
                for (uint32_t i = 1; i < 10; ++i) // slot 0 is unarmed
                {
                    const uint32_t w = ped + kPedWeapons + i * kWeaponSlotSize;
                    const uint32_t type = rd32(ram, w);
                    if (!type)
                        continue;
                    weapons += fmt("  %u:t%u", i, type);
                    const uint32_t total = rd32(ram, w + 12);
                    if (total)
                        weapons += fmt(" %u/%u", rd32(ram, w + 8), total);
                }
                add(weapons == "weapons" ? std::string("weapons  none") : weapons);
            }

            // ---- vehicle
            if (veh)
            {
                const int16_t model = rd16(ram, veh + 0x56);
                std::string name;
                if (const uint32_t infos = rd32(ram, kModelInfos); infos && model > 0 && model < 4096)
                    if (const uint32_t mi = rd32(ram, infos + static_cast<uint32_t>(model) * 4u))
                        name = name8(ram, mi + kModelGameName);
                const float health = rdf(ram, veh + kCarHealth);
                header(fmt("VEHICLE  %08x  model %d %s", veh, model, name.c_str()));
                const float vx = rdf(ram, veh + kCarVelocity), vy = rdf(ram, veh + kCarVelocity + 4), vz = rdf(ram, veh + kCarVelocity + 8);
                const float kmh = std::sqrt(vx * vx + vy * vy + vz * vz) * 50.0f * 3.6f;
                add(fmt("pos   %9.2f %9.2f %8.2f   heading %5.1f", rdf(ram, veh + 0x30), rdf(ram, veh + 0x34), rdf(ram, veh + 0x38),
                        headingDeg(rdf(ram, veh + 0x10), rdf(ram, veh + 0x14))));
                add(fmt("health %6.1f  speed %5.1f km/h%s", health, kmh, rdf(ram, veh + 0x28) < 0.0f ? "  UPSIDE DOWN" : ""),
                    health < 250.0f ? bad : 0);
            }

            // ---- camera
            {
                const uint32_t cam = kCamera + 0x70u + (rd8(ram, kCamera + 0x50) % 3u) * 0x270u;
                const int mode = static_cast<uint16_t>(rd16(ram, cam));
                const bool atPlayer = rd8(ram, kCamera + 0x888) != 0;
                header(fmt("CAMERA  mode %d %s%s", mode, cameraModeName(mode), atPlayer ? "" : "  (script camera)"));
                // heading / pitch from the CCam's Front (+0x10), so they read like the ped's and the vehicle's (its own
                // beta +0x7C counts from another axis: 270 behind a car heading north)
                const float fx = rdf(ram, cam + 0x10), fy = rdf(ram, cam + 0x14), fz = rdf(ram, cam + 0x18);
                const float pitch = std::atan2(fz, std::sqrt(fx * fx + fy * fy)) * 57.2957795f;
                add(fmt("pos   %9.2f %9.2f %8.2f   heading %5.1f  pitch %5.1f  fov %.0f", rdf(ram, kCamera + 0x30), rdf(ram, kCamera + 0x34),
                        rdf(ram, kCamera + 0x38), headingDeg(fx, fy), pitch, rdf(ram, cam + 0x134)));
            }

            // ---- world
            {
                header("WORLD");
                const uint32_t area = rd32(ram, kCurrArea), level = rd32(ram, kLevelWanted);
                add(fmt("clock %02u:%02u  area %u%s  level %u %s", rd8(ram, kClockHours), rd8(ram, kClockHours + 1), area,
                        area ? " (interior)" : " (outdoors)", level, levelName(level)));
                add(fmt("game time %.1f s  frame %u  step %.3f", rd32(ram, kGameTimeMs) / 1000.0, rd32(ram, kFrameCounter), rdf(ram, kTimeStep)));
                const bool userPause = rd8(ram, kUserPause), codePause = rd8(ram, kCodePause);
                const bool noControls = rd16(ram, kPad0 + 0x96) != 0;
                if (userPause || codePause || noControls)
                    add(fmt("%s%s%s", userPause ? "PAUSED (menu)  " : "", codePause ? "PAUSED (code)  " : "",
                            noControls ? "player controls off" : ""),
                        warn);
                int pedsUsed, pedsSize, carsUsed, carsSize;
                poolUse(ram, kPedPool, pedsUsed, pedsSize);
                poolUse(ram, kVehiclePool, carsUsed, carsSize);
                add(fmt("pools  peds %d/%d  vehicles %d/%d", pedsUsed, pedsSize, carsUsed, carsSize));
            }

            // ---- scripts: the newest (a mission's script, one just started) with their IPs, then the rest by name
            {
                constexpr int kWithIp = 4, kPerLine = 9, kNameLines = 2;
                std::vector<std::string> names;
                std::string newest, mission;
                int count = 0;
                for (uint32_t s = rd32(ram, kActiveScripts); s && count < 256; s = rd32(ram, s), ++count)
                {
                    const std::string name = name8(ram, s + 0x20f);
                    if (rd8(ram, s + 0x217)) // "is the mission" (11b); +0x20a is set on ambient scripts too
                        mission += fmt("  %s @%x", name.c_str(), rd32(ram, s + 0x10));
                    if (count < kWithIp)
                        newest += fmt("  %s @%x", name.c_str(), rd32(ram, s + 0x10));
                    else
                        names.push_back(name);
                }
                header(fmt("SCRIPTS  %d active%s", count, rd8(ram, kMissionRunning) ? "  mission running" : ""));
                if (!mission.empty())
                    add("mission" + mission, IM_COL32(120, 200, 245, 255));
                if (!newest.empty())
                    add("newest" + newest);
                for (int l = 0; l < kNameLines && l * kPerLine < static_cast<int>(names.size()); ++l)
                {
                    std::string t;
                    for (int k = l * kPerLine; k < std::min(static_cast<int>(names.size()), (l + 1) * kPerLine); ++k)
                        t += (t.empty() ? "" : " ") + names[k];
                    const int left = static_cast<int>(names.size()) - (l + 1) * kPerLine;
                    if (l == kNameLines - 1 && left > 0)
                        t += fmt("  +%d more", left);
                    add(t);
                }
            }
            return out;
        }
    }

    void install(PS2Runtime &runtime)
    {
        g_rdram = runtime.memory().getRDRAM();
        ps2perf::setInfoProvider([](std::vector<ps2perf::InfoLine> &lines) {
            g_wantedUntil.store(ticksNow() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::seconds(1)).count(),
                                std::memory_order_relaxed);
            std::chrono::steady_clock::time_point at;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                lines = g_lines;
                at = g_linesAt;
            }
            if (lines.empty())
                return;
            // the snapshot comes from the frame kick: say so when the game has not finished a frame for a while
            const double age = std::chrono::duration<double>(std::chrono::steady_clock::now() - at).count();
            if (age > 0.5)
                lines.insert(lines.begin(), {fmt("no game frame for %.1f s (loading or stopped): the last one's state", age),
                                             IM_COL32(240, 210, 90, 255), false});
        });
        // gameinfo -> ok <the panel's lines, " | " between them, sections as [NAME ...]> (read now, any time)
        ps2control::addCommand("gameinfo", [](const std::string &, ps2control::Reply reply) {
            if (!g_rdram)
                return reply("error the game has not started");
            std::string out = fmt("ok frames=%llu", static_cast<unsigned long long>(vcs::rig::gameFrames()));
            for (const auto &l : build(g_rdram))
                out += l.header ? " | [" + l.text + "]" : " | " + l.text;
            reply(out);
        });
    }

    void onGameFrame(const uint8_t *rdram)
    {
        if (ticksNow() > g_wantedUntil.load(std::memory_order_relaxed))
            return; // the panel is not on screen
        std::vector<ps2perf::InfoLine> lines = build(rdram);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_lines = std::move(lines);
        g_linesAt = std::chrono::steady_clock::now();
    }
}
