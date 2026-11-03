// Mission scripts that count per frame, put on the game's time step (docs/game-internals.md 11f).
//
// The script VM runs every script once per frame; a `WAIT 0` loop is one pass a frame. Most missions time things with
// the timers (TIMERA / TIMERB), GET_GAME_TIMER or the timed opcodes (0x0031 - 0x0034: value * CTimer::ms_fTimeStep),
// but some change a meter by a fixed amount every pass - PHI_A2 "Boomshine Blowout" adds 0.015 to the heat's floor and
// +-1 to the heat each frame. On the PS2 a frame is always 1/30 s, so those meters moved at one speed; with independent
// timing at 144 fps the heat built 4.8 times as fast. The survey of MAIN.SCM that found these is in game-internals.md
// 11f (every non-timed add / subtract of a meter-like variable in a per-frame loop, checked by reading the mission).
//
// At the listed instructions the hooked handler does the instruction itself with the operand scaled by
// ms_fTimeStep / (50/30) - the frame's length over the PS2's - and moves the IP on, the way the timed opcodes would
// have done it. Integer meters keep the fraction for the next pass. Each instruction is identified by the mission's
// offset and checked against its decoded opcode and operands before it is touched; anything else runs the game's
// handler. In original timing at 30 fps nothing is scaled (the step is the PS2's anyway, and the game's own handlers
// keep that path exact).
#include "game/vcs_script_timing.h"

#include "game/vcs_framerate.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <bitset>
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
        constexpr uint32_t kScriptSpace = 0x0048741Cu;   // -> MAIN.SCM in memory; an IP is an offset from it
        constexpr uint32_t kMissionStart = 0x004CD3D8u;  // IP of the loaded mission's first byte
        constexpr uint32_t kMissionRunning = 0x004CD3E7u; // u8
        constexpr uint32_t kTimeStep = 0x004CD110u;      // CTimer::ms_fTimeStep (1.0 = 1/50 s)
        constexpr uint32_t kTimeMs = 0x004CD104u;        // CTimer::m_snTimeInMilliseconds
        constexpr float kPs2Step = 50.0f / 30.0f;          // a 30 fps frame
        constexpr uint32_t kDummyVariable = 0x0050C140u;  // where FUN_00254ff8 points an out-of-range array index

        // CRunningScript (game-internals.md 11b)
        constexpr uint32_t kScriptIp = 0x10u, kScriptLocals = 0x54u, kScriptFrame = 0x1FCu;

        // the handlers (opcode table 0x4BEAF0, game-internals.md 11b); the IP is past the opcode when they run
        constexpr uint32_t kHandlerSet = 0x003B11D8u;      // 0004 / 0005 / 0035 / 0036: dst = src (32-bit copy)
        constexpr uint32_t kHandlerAddInt = 0x003B1248u;   // 0007
        constexpr uint32_t kHandlerAddFloat = 0x003B12C0u; // 0008 / 002A
        constexpr uint32_t kHandlerSubInt = 0x003B1338u;   // 0009 / 002B
        constexpr uint32_t kHandlerSubFloat = 0x003B13B0u; // 000A / 002C
        constexpr uint32_t kHandlerMulFloat = 0x003B14A0u; // 000C / 002E

        struct Operand
        {
            enum Kind : uint8_t
            {
                None,
                Global,      // $G<index>
                Local,       // <index>L, relative to the script's frame
                GlobalArray, // $G<index>[<arrayIndex>L], <arraySize> entries
                Float,
                Int,
            } kind = None;
            uint16_t index = 0;
            uint8_t arrayIndex = 0, arraySize = 0;
            float f = 0.0f;
            int32_t i = 0;

            bool operator==(const Operand &o) const
            {
                if (kind != o.kind)
                    return false;
                switch (kind)
                {
                case Global:
                case Local: return index == o.index;
                case GlobalArray: return index == o.index && arrayIndex == o.arrayIndex && arraySize == o.arraySize;
                case Float: return f == o.f;
                case Int: return i == o.i;
                default: return true;
                }
            }
        };
        constexpr Operand G(uint16_t index) { return {Operand::Global, index}; }
        constexpr Operand L(uint16_t index) { return {Operand::Local, index}; }
        constexpr Operand GA(uint16_t index, uint8_t arrayIndex, uint8_t size) { return {Operand::GlobalArray, index, arrayIndex, size}; }
        constexpr Operand F(float f) { return {Operand::Float, 0, 0, 0, f}; }
        constexpr Operand I(int32_t i) { return {Operand::Int, 0, 0, 0, 0.0f, i}; }

        struct Site
        {
            uint8_t mission;  // MAIN.SCM mission number (game-internals.md 11a)
            uint32_t offset;  // of the instruction from the mission's start (MAIN.SCM IP - mission table entry)
            uint16_t opcode;
            Operand dst, src;
            const char *what;
        };

        // Per-frame meter changes in the missions (game-internals.md 11f: how each was found, what it does)
        const Site kSites[] = {
            {47, 0x2026, 0x0008, G(0x15AB), F(0.015f), "PHI_A2 Boomshine Blowout: heat floor +0.015"},
            {47, 0x202F, 0x002A, G(0x15AA), G(0x15AC), "PHI_A2 Boomshine Blowout: heat += rate (+1 in the fire, -1 out)"},
            {59, 0x06E2, 0x000A, G(0x1513), F(0.04f), "LOU_B1 Hose the Hoes: girls -0.04"},
            {59, 0x06FF, 0x000A, G(0x1513), F(0.03f), "LOU_B1 Hose the Hoes: girls -0.03"},
            {59, 0x070F, 0x000A, G(0x1513), F(0.01f), "LOU_B1 Hose the Hoes: girls -0.01"},
            {67, 0x24CF, 0x0008, G(0x14E9), F(0.005f), "UMB_B2 Balls: havoc +0.005 per Cholo"},
            {97, 0x1E3F, 0x000A, G(0x154C), F(0.04f), "DIA_C2 The Exchange: time -0.04"},
            {80, 0x27EC, 0x000A, G(0x152E), F(0.25f), "REN_C3 Kill Phil: Phil -0.25 (every other frame)"},
            {68, 0x0E8A, 0x0005, L(0), F(-0.2f), "UMB_B3 Papi Don't Screech: stress rate -0.2 (calm)"},
            {68, 0x0EBA, 0x0005, L(0), F(0.25f), "UMB_B3 Papi Don't Screech: stress rate 0.25 (out of the car)"},
            {68, 0x0ED1, 0x0005, L(0), F(2.0f), "UMB_B3 Papi Don't Screech: stress rate 2"},
            {68, 0x0EE6, 0x0005, L(0), F(0.5f), "UMB_B3 Papi Don't Screech: stress rate 0.5 (in the air)"},
            {68, 0x0EFD, 0x0005, L(0), F(0.5f), "UMB_B3 Papi Don't Screech: stress rate 0.5 (over 30)"},
            {68, 0x0F12, 0x0005, L(0), F(0.5f), "UMB_B3 Papi Don't Screech: stress rate 0.5 (upside down)"},
            {35, 0x15E3, 0x0009, G(0x1700), I(1), "FIREHEL: water -1 while dropping"},
            {35, 0x1B73, 0x0009, G(0x15E4), I(5), "FIREHEL: helicopter health -5 in a fire"},
            {77, 0x13BE, 0x0007, G(0x1656), I(1), "MEN_C6 Burning Bridges: pressure +1 away from the depot"},
            {18, 0x0DEC, 0x000A, G(0x14F1), F(0.2f), "driving range: aim left -0.2"},
            {18, 0x0E0F, 0x0008, G(0x14F1), F(0.2f), "driving range: aim right +0.2"},
            {18, 0x0E2F, 0x0008, G(0x14F1), F(0.05f), "driving range: aim back to centre +0.05"},
            {18, 0x0E50, 0x000A, G(0x14F1), F(0.05f), "driving range: aim back to centre -0.05"},
            {93, 0x1255, 0x000A, G(0x150E), F(0.2f), "GON_C2 Home's on the Range: aim left -0.2"},
            {93, 0x1278, 0x0008, G(0x150E), F(0.2f), "GON_C2 Home's on the Range: aim right +0.2"},
            {93, 0x1298, 0x0008, G(0x150E), F(0.05f), "GON_C2 Home's on the Range: aim back to centre +0.05"},
            {93, 0x12B9, 0x000A, G(0x150E), F(0.05f), "GON_C2 Home's on the Range: aim back to centre -0.05"},
            {99, 0x53B7, 0x002E, L(7), GA(0x1587, 0, 5), "DIA_C4 Over the Top: searchlight step (route 10)"},
            {99, 0x5478, 0x0036, L(8), GA(0x1587, 0, 5), "DIA_C4 Over the Top: searchlight step (route 20)"},
            {60, 0x1353, 0x000A, G(0x1513), F(0.1f), "LOU_B2 Robbing the Cradle: escape distance -0.1"},
            {60, 0x1376, 0x000A, G(0x1514), F(0.1f), "LOU_B2 Robbing the Cradle: escape distance -0.1"},
            {60, 0x1399, 0x000A, G(0x1515), F(0.1f), "LOU_B2 Robbing the Cradle: escape distance -0.1"},
        };
        constexpr size_t kSiteCount = sizeof(kSites) / sizeof(kSites[0]);

        std::unordered_map<uint32_t, std::vector<size_t>> g_byOffset; // offset -> kSites indices
        std::bitset<0x10000> g_isSite; // offsets with a site (missions are at most 0x96A9 bytes): most handler calls stop here
        float g_carry[kSiteCount] = {};                                // integer meters: the fraction not applied yet
        bool g_logged[kSiteCount] = {};
        struct Stats
        {
            uint32_t runs = 0;
            double game = 0.0, scaled = 0.0; // the amounts the game would have applied, and what was applied
        } g_stats[kSiteCount];

        enum Handler
        {
            HSet,
            HAddInt,
            HAddFloat,
            HSubInt,
            HSubFloat,
            HMulFloat,
            kHandlers
        };
        PS2Runtime::RecompiledFunction g_orig[kHandlers] = {};

        uint32_t read32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (address & PS2_RAM_MASK), 4);
            return v;
        }
        float readF(const uint8_t *rdram, uint32_t address)
        {
            float v;
            std::memcpy(&v, rdram + (address & PS2_RAM_MASK), 4);
            return v;
        }
        void write32(uint8_t *rdram, uint32_t address, uint32_t v) { std::memcpy(rdram + (address & PS2_RAM_MASK), &v, 4); }
        void writeF(uint8_t *rdram, uint32_t address, float v) { std::memcpy(rdram + (address & PS2_RAM_MASK), &v, 4); }

        bool enabled()
        {
            static const bool on = [] {
                const char *v = std::getenv("VCS_SCRIPT_TIMESTEP");
                return !(v && v[0] == '0');
            }();
            return on;
        }
        // the frame isn't the PS2's 1/30 s: independent timing, or original timing at 60 (one VBlank a frame)
        bool active() { return enabled() && (timing() == Timing::Independent || frameCap() != 30); }

        // One argument at `ip` (FUN_00254b90 / FUN_00254ff8, game-internals.md 11c): what it is, where its variable is
        // (0 for a constant), and the IP after it. Only the forms the sites use; anything else is Kind None.
        Operand decode(const uint8_t *rdram, uint32_t script, uint32_t &ip, uint32_t &address)
        {
            const uint32_t space = read32(rdram, kScriptSpace);
            const auto byte = [&](uint32_t at) { return rdram[(space + at) & PS2_RAM_MASK]; };
            const auto local = [&](uint32_t n) { return script + kScriptLocals + 4u * (read32(rdram, script + kScriptFrame) + n); };
            Operand o;
            address = 0;
            const uint8_t t = byte(ip);
            if (t == 1 || t == 2)
            {
                o = t == 1 ? I(0) : F(0.0f);
                ip += 1;
            }
            else if (t >= 3 && t <= 5) // float with only its top 1 / 2 / 3 bytes stored
            {
                const uint32_t n = t - 2u;
                uint32_t bits = 0;
                for (uint32_t k = 0; k < n; ++k)
                    bits |= static_cast<uint32_t>(byte(ip + 1u + k)) << (8u * (4u - n + k));
                float f;
                std::memcpy(&f, &bits, 4);
                o = F(f);
                ip += 1u + n;
            }
            else if (t == 6 || t == 9)
            {
                uint32_t bits = 0;
                for (uint32_t k = 0; k < 4; ++k)
                    bits |= static_cast<uint32_t>(byte(ip + 1u + k)) << (8u * k);
                if (t == 6)
                    o = I(static_cast<int32_t>(bits));
                else
                {
                    float f;
                    std::memcpy(&f, &bits, 4);
                    o = F(f);
                }
                ip += 5;
            }
            else if (t == 7)
            {
                o = I(static_cast<int8_t>(byte(ip + 1u)));
                ip += 2;
            }
            else if (t == 8)
            {
                o = I(static_cast<int16_t>(byte(ip + 1u) | (byte(ip + 2u) << 8)));
                ip += 3;
            }
            else if (t >= 0x0D && t <= 0x6C)
            {
                o = L(static_cast<uint16_t>(t - 0x0Du));
                address = local(o.index);
                ip += 1;
            }
            else if (t >= 0xCD && t <= 0xE5)
            {
                o = G(static_cast<uint16_t>(((t - 0xCDu) << 8) | byte(ip + 1u)));
                address = space + 4u * o.index;
                ip += 2;
            }
            else if (t >= 0xE6)
            {
                o = GA(static_cast<uint16_t>(((t - 0xE6u) << 8) | byte(ip + 1u)), byte(ip + 2u), byte(ip + 3u));
                const int32_t i = static_cast<int32_t>(read32(rdram, local(o.arrayIndex)));
                address = i >= 0 && i < o.arraySize ? space + 4u * (o.index + static_cast<uint32_t>(i)) : kDummyVariable;
                ip += 4;
            }
            return o; // timers (0x0B / 0x0C), local arrays: None
        }

        // VCS_SCRIPT_TIMESTEP_LOG=1: per site, every 2 s of game time, how often it ran and how much the game would
        // have moved the meter by vs. what was applied
        void logStats(const uint8_t *rdram)
        {
            static const bool log = std::getenv("VCS_SCRIPT_TIMESTEP_LOG") != nullptr;
            static uint32_t since = 0;
            if (!log)
                return;
            const uint32_t now = read32(rdram, kTimeMs);
            if (!since || now < since)
                since = now;
            if (now - since < 2000u)
                return;
            for (size_t n = 0; n < kSiteCount; ++n)
                if (g_stats[n].runs)
                {
                    std::fprintf(stderr, "[script-timing] %u ms: %s: %u runs, %.3f unscaled, %.3f applied\n", now - since,
                                 kSites[n].what, g_stats[n].runs, g_stats[n].game, g_stats[n].scaled);
                    g_stats[n] = {};
                }
            since = now;
        }

        // VCS_SCRIPT_TIMESTEP_CHECK=1: whenever a different mission has been loaded, decode every site at its offset and
        // log the ones that are there (tools/framerate_audit.py --sites: each listed mission's sites must all match)
        void checkSites(const uint8_t *rdram, uint32_t script)
        {
            static uint64_t lastSignature = 0;
            const uint32_t space = read32(rdram, kScriptSpace), start = read32(rdram, kMissionStart);
            uint64_t signature = start;
            for (uint32_t k = 0; k < 64; ++k)
                signature = signature * 1099511628211ull ^ rdram[(space + start + k) & PS2_RAM_MASK];
            if (signature == lastSignature)
                return;
            lastSignature = signature;
            for (const Site &site : kSites)
            {
                const uint32_t at = start + site.offset;
                const uint16_t opcode = static_cast<uint16_t>(rdram[(space + at) & PS2_RAM_MASK] | (rdram[(space + at + 1u) & PS2_RAM_MASK] << 8));
                uint32_t ip = at + 2u, dstAt = 0, srcAt = 0;
                const Operand dst = decode(rdram, script, ip, dstAt);
                const Operand src = decode(rdram, script, ip, srcAt);
                if (opcode == site.opcode && dst == site.dst && src == site.src)
                    std::fprintf(stderr, "[script-timing] check: mission %u +0x%X matches (%s)\n", site.mission, site.offset, site.what);
            }
        }

        // The instruction this handler is about to run, if it is one of kSites (and the frame isn't the PS2's): done
        // here with the operand scaled. True when done (the IP is past it, the handler's result set).
        bool scaled(uint8_t *rdram, R5900Context *ctx, Handler handler)
        {
            if (!rdram[kMissionRunning & PS2_RAM_MASK])
                return false;
            static const bool check = std::getenv("VCS_SCRIPT_TIMESTEP_CHECK") != nullptr;
            if (check)
                checkSites(rdram, GPR_U32(ctx, 4));
            if (!active())
                return false;
            const uint32_t script = GPR_U32(ctx, 4);
            const uint32_t start = read32(rdram, script + kScriptIp) - 2u; // the opcode
            const uint32_t offset = start - read32(rdram, kMissionStart);
            if (offset >= g_isSite.size() || !g_isSite[offset])
                return false;
            const auto it = g_byOffset.find(offset);
            if (it == g_byOffset.end())
                return false;
            const uint32_t space = read32(rdram, kScriptSpace);
            const uint16_t opcode = static_cast<uint16_t>(rdram[(space + start) & PS2_RAM_MASK] | (rdram[(space + start + 1u) & PS2_RAM_MASK] << 8));
            uint32_t ip = start + 2u, dstAt = 0, srcAt = 0;
            const Operand dst = decode(rdram, script, ip, dstAt);
            const Operand src = decode(rdram, script, ip, srcAt);
            for (const size_t n : it->second)
            {
                const Site &site = kSites[n];
                if (site.opcode != opcode || !(site.dst == dst) || !(site.src == src) || !dstAt)
                    continue;
                const float step = readF(rdram, kTimeStep);
                const float k = std::clamp(step / kPs2Step, 0.0f, 2.0f);
                const bool isInt = handler == HAddInt || handler == HSubInt;
                Stats &stats = g_stats[n];
                ++stats.runs;
                if (isInt)
                {
                    const int32_t v = srcAt ? static_cast<int32_t>(read32(rdram, srcAt)) : src.i;
                    const float amount = static_cast<float>(v) * k + g_carry[n];
                    const int32_t whole = static_cast<int32_t>(std::trunc(amount));
                    g_carry[n] = amount - static_cast<float>(whole);
                    const int32_t d = static_cast<int32_t>(read32(rdram, dstAt));
                    write32(rdram, dstAt, static_cast<uint32_t>(handler == HAddInt ? d + whole : d - whole));
                    stats.game += v;
                    stats.scaled += whole;
                }
                else
                {
                    const float v = srcAt ? readF(rdram, srcAt) : (src.kind == Operand::Int ? static_cast<float>(src.i) : src.f);
                    const float d = readF(rdram, dstAt), s = v * k;
                    writeF(rdram, dstAt, handler == HSet ? s : handler == HAddFloat ? d + s : handler == HSubFloat ? d - s : d * s);
                    stats.game += v;
                    stats.scaled += s;
                }
                if (!g_logged[n])
                {
                    g_logged[n] = true;
                    std::fprintf(stderr, "[script-timing] %s (mission %u +0x%X): on the time step (step %.3f, x%.3f)\n", site.what,
                                 site.mission, site.offset, step, k);
                }
                logStats(rdram);
                write32(rdram, script + kScriptIp, ip);
                SET_GPR_U64(ctx, 2, 0); // keep interpreting
                ctx->pc = GPR_U32(ctx, 31);
                return true;
            }
            return false;
        }

        template <Handler H>
        PS2_GUEST_CC void handlerHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!scaled(rdram, ctx, H))
                g_orig[H](rdram, ctx, runtime);
        }
    }

    void installScriptTimingHooks(PS2Runtime &runtime)
    {
        const struct
        {
            Handler h;
            uint32_t address;
            PS2Runtime::RecompiledFunction hook;
        } handlers[] = {
            {HSet, kHandlerSet, &handlerHook<HSet>},
            {HAddInt, kHandlerAddInt, &handlerHook<HAddInt>},
            {HAddFloat, kHandlerAddFloat, &handlerHook<HAddFloat>},
            {HSubInt, kHandlerSubInt, &handlerHook<HSubInt>},
            {HSubFloat, kHandlerSubFloat, &handlerHook<HSubFloat>},
            {HMulFloat, kHandlerMulFloat, &handlerHook<HMulFloat>},
        };
        for (const auto &h : handlers)
        {
            g_orig[h.h] = runtime.lookupFunction(h.address);
            if (!g_orig[h.h])
            {
                std::fprintf(stderr, "[script-timing] handler 0x%x not found: per-frame script meters stay on the frame rate\n", h.address);
                return;
            }
        }
        for (size_t n = 0; n < kSiteCount; ++n)
        {
            g_byOffset[kSites[n].offset].push_back(n);
            g_isSite.set(kSites[n].offset);
        }
        for (const auto &h : handlers)
            runtime.replaceFunction(h.address, h.hook);
    }
}
