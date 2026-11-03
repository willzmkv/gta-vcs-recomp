// The RSL shadow draw list - see debug/rsl_shadow.h.
#include "debug/rsl_shadow.h"

#include "ps2_runtime.h"
#include "runtime/ps2_chain_observer.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace vcs::rslshadow
{
    namespace
    {
        constexpr uint32_t kCursor = 0x520FA8u; // the packet buffer's DMA cursor (game-internals.md 6i)
        constexpr uint32_t kRamMask = PS2_RAM_MASK;
        constexpr uint32_t kPktLo = 0x520F90u, kPktHi = 0x6EDC70u; // both halves of the packet buffer
        constexpr uint32_t kCellsLo = 0xCB7A80u, kCellsHi = 0x112A660u, kModelsLo = 0x1200000u, kModelsHi = 0x1800000u;

        enum Kind : uint8_t
        {
            Boundary,
            Model,     // FUN_002F7D58 / FUN_002F80A0 (a0 = packet): CALL a0
            World,     // FUN_002F7EB0 (a0 = geometry, a1 = qwc): REF a0, qwc
            Immediate, // FUN_002F8250: its own REF / UNPACK / MSCAL (counted, not checked)
            KindCount,
        };
        constexpr const char *kKindNames[KindCount] = {"boundary", "model", "world", "immediate"};

        struct Fn
        {
            uint32_t address;
            Kind kind;
        };
        // the draws, then every other function that writes the buffer (the scan of game-internals.md 6i) except the
        // state flush FUN_002F8710, which belongs to the draw that calls it
        constexpr Fn kFns[] = {
            {0x002F7D58, Model},    {0x002F80A0, Model},    {0x002F7EB0, World},    {0x002F8250, Immediate},
            {0x002409C8, Boundary}, {0x00240ED8, Boundary}, {0x001CEF18, Boundary}, {0x00266868, Boundary},
            {0x002A92C0, Boundary}, {0x002ABF68, Boundary}, {0x002F0360, Boundary}, {0x002F39D8, Boundary},
            {0x002F3DD0, Boundary}, {0x002F53D8, Boundary}, {0x002F5520, Boundary}, {0x002F62B8, Boundary},
            {0x002F6A78, Boundary}, {0x002F7090, Boundary}, {0x002F7908, Boundary}, {0x002F7B30, Boundary},
            {0x002F9388, Boundary}, {0x002F9AA0, Boundary}, {0x002F9CE8, Boundary}, {0x002F9FA0, Boundary},
            {0x002FB690, Boundary}, {0x002FB3C0, Boundary}, {0x0032CD60, Boundary}, {0x0032D480, Boundary},
            {0x0032D7C8, Boundary}, {0x0032D880, Boundary}, {0x0040C430, Boundary}, {0x0040F6C8, Boundary},
            {0x002F7388, Boundary}, // runs the state flush without a draw (1b: the resident viewport)
        };
        constexpr int kCount = static_cast<int>(sizeof(kFns) / sizeof(kFns[0]));

        PS2Runtime::RecompiledFunction g_originals[kCount] = {};

        // a draw's state at entry, from the sources its state flush uploads (game-internals.md 6i)
        struct State
        {
            uint32_t object = 0, dirty = 0; // *0x4882A4 (the current object), *0x4882A0 (dirty bits)
            uint8_t vu49[5 * 16] = {};      // 0x742AE0: VU 49-53 (GIF tags, colour, flags, fog)
            uint8_t viewport[4 * 16] = {};  // 0x520920: VU 12-15
            uint8_t guard[16] = {};         // 0x520AC0: VU 65
            uint8_t objectBytes[0x200] = {}; // the object's first 0x200 bytes (VCS_RSL_SHADOW_DUMP: finding its matrix)
            uint8_t blocks[0x340] = {};      // 0x520860..0x520BA0: the matrix blocks (VCS_RSL_SHADOW_DUMP)
        };

        struct Record
        {
            int fn;          // index into kFns
            uint32_t a0, a1; // arguments
            uint32_t start;  // cursor at entry
            bool matched = false;
            State state;     // draws only
        };
        std::vector<Record> g_current, g_kicked; // this frame's / the frame whose chain is being copied
        uint8_t g_residentViewport[4 * 16] = {}; // the viewport the state flush last uploaded (VU 12-15)
        const uint8_t *g_rdram = nullptr;         // VCS_RSL_SHADOW_FIND
        uint32_t g_kickedEnd = 0;                // the main stream's end (the cursor at the kick)
        std::vector<size_t> g_draws;             // indices of g_kicked's draws, by start

        // A copied chain's map from stream offsets to draws, for the thread that processes it (1b): an entry per
        // packet-buffer tag, in chain order - its flat offset and the draw it is the own tag of (else -1). What a
        // draw's CALL / REF brings in runs until the next packet-buffer tag.
        struct ChainTable
        {
            std::vector<std::pair<uint32_t, int>> entries;
            std::vector<Record> records; // the frame's records (copied: the game thread goes on)
        };
        ChainTable g_building;
        std::mutex g_tablesMutex;
        std::map<const void *, std::shared_ptr<ChainTable>> g_tables; // queued chains, by buffer
        thread_local std::shared_ptr<ChainTable> t_processing;      // the chain the calling thread processes
        thread_local const uint8_t *t_stream = nullptr;             // ... and its bytes (the key is the buffer)

        // The draw state checked in 1b: VU qwords and where RSL keeps them (game-internals.md 6i; found from
        // VCS_RSL_SHADOW_DUMP samples). Offsets into State::blocks (0x520860..) unless noted.
        struct Group
        {
            const char *name;
            uint32_t vu, qwords;
            int source; // >= 0: offset into blocks; -1 the resident viewport, -2 render context + 0x60, -3 0x742AE0 (+ 16 * k)
            uint32_t sourceOffset;
        };
        constexpr Group kGroups[] = {
            {"object matrix 0-3", 0, 4, 0x000, 0},
            {"camera 4-11", 4, 8, 0x040, 0},
            {"viewport 12-15", 12, 4, -1, 0},
            {"camera position 32", 32, 1, 0x130, 0},
            {"slot table 34-37", 34, 4, -2, 0},
            {"tags 49-50", 49, 2, -3, 0},
            {"colour 51", 51, 1, -3, 0x20},
            {"fog 53", 53, 1, -3, 0x40},
            {"clip planes 59-64", 59, 6, 0x140, 0},
            {"guard 65", 65, 1, 0x260, 0},
        };
        constexpr int kGroupCount = static_cast<int>(sizeof(kGroups) / sizeof(kGroups[0]));

        // per report window, 1b (the processing thread)
        struct StateStats
        {
            uint64_t mscals = 0, inDraws = 0;
            uint64_t same[kGroupCount] = {}, flagsSame = 0; // flags: VU 52.y
            // not the same as the RSL state: uploaded by the draw's own stream / carried over from the draw before /
            // uploaded earlier in the chain / unexplained
            uint64_t stream[kGroupCount] = {}, carried[kGroupCount] = {}, earlier[kGroupCount] = {}, unexplained[kGroupCount] = {};
            uint64_t byKind[KindCount] = {}, allSameByKind[KindCount] = {}, allExplainedByKind[KindCount] = {};
        };
        std::mutex g_stateMutex;
        StateStats g_stateStats;

        // per report window
        struct Stats
        {
            uint64_t frames = 0;
            uint64_t draws[KindCount] = {}, matched[KindCount] = {};
            uint64_t tags3d = 0, own3d = 0, state3d = 0, unexplained3d = 0; // 3D tags in the packet buffer: the draws'
                                                                               // own, their state, not from a draw
            std::map<std::pair<int, uint32_t>, uint64_t> tagsByFn; // (fn or -1, tag id) -> tags in the packet buffer
            std::map<int, uint64_t> unexplainedByFn;
            uint64_t topTags[8] = {}, top3d = 0; // tags in blocks allocated from the top of the half
        } g_stats;
        auto g_lastReport = std::chrono::steady_clock::now();

        uint32_t cursor(const uint8_t *rdram)
        {
            uint32_t c;
            std::memcpy(&c, rdram + (kCursor & kRamMask), 4);
            return c & kRamMask;
        }

        template <int I>
        PS2_GUEST_CC void hook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            // before the original only (DECISIONS.md "Rule for hooks on recompiled functions")
            g_rdram = rdram;
            Record &r = g_current.emplace_back(Record{I, getRegU32(ctx, 4), getRegU32(ctx, 5), cursor(rdram)});
            const auto at = [&](uint32_t a) { return rdram + (a & kRamMask); };
            // the viewport in VU memory is the one the state flush last uploaded (dirty bit 0x1000), not necessarily
            // the current global: the flush runs in the draws, FUN_002F9FA0 and FUN_002F7388
            const uint32_t address = kFns[I].address;
            if (kFns[I].kind != Boundary || address == 0x002F9FA0u || address == 0x002F7388u)
            {
                uint32_t dirty;
                std::memcpy(&dirty, at(0x4882A0u), 4);
                if (dirty & 0x1000u)
                    std::memcpy(g_residentViewport, at(0x520920u), sizeof(g_residentViewport));
            }
            if (kFns[I].kind != Boundary)
            {
                std::memcpy(&r.state.object, at(0x4882A4u), 4);
                std::memcpy(&r.state.dirty, at(0x4882A0u), 4);
                std::memcpy(r.state.vu49, at(0x742AE0u), sizeof(r.state.vu49));
                std::memcpy(r.state.viewport, g_residentViewport, sizeof(r.state.viewport));
                std::memcpy(r.state.guard, at(0x520AC0u), sizeof(r.state.guard));
                if (r.state.object && (r.state.object & kRamMask) + sizeof(r.state.objectBytes) <= kRamMask)
                    std::memcpy(r.state.objectBytes, at(r.state.object), sizeof(r.state.objectBytes));
                std::memcpy(r.state.blocks, at(0x520860u), sizeof(r.state.blocks));
            }
            g_originals[I](rdram, ctx, runtime);
        }

        template <int... Is>
        PS2Runtime::RecompiledFunction hookFor(int i, std::integer_sequence<int, Is...>)
        {
            static const PS2Runtime::RecompiledFunction table[] = {&hook<Is>...};
            return table[i];
        }

        bool in(uint32_t a, uint32_t lo, uint32_t hi)
        {
            a &= kRamMask;
            return a >= lo && a < hi;
        }

        // the record (draw or boundary) a packet-buffer address was written by: the last one entered before it
        int recordAt(uint32_t tagAddr)
        {
            const auto it = std::upper_bound(g_kicked.begin(), g_kicked.end(), tagAddr,
                                             [](uint32_t a, const Record &r) { return a < r.start; });
            return it == g_kicked.begin() ? -1 : static_cast<int>(it - g_kicked.begin()) - 1;
        }

        // VIFcode DIRECT / DIRECTHL (0x50 / 0x51): the payload goes to the GS (textures, 2D), not to the VU1
        bool direct(uint64_t vifCodes)
        {
            for (int half = 0; half < 2; ++half)
            {
                const uint32_t cmd = static_cast<uint32_t>(vifCodes >> (half * 32 + 24)) & 0x7Fu;
                if (cmd == 0x50u || cmd == 0x51u)
                    return true;
            }
            return false;
        }

        void observeTag(uint32_t tagAddr, uint32_t id, uint32_t tagTarget, uint32_t dataAddr, uint32_t qwc, uint64_t vifCodes,
                     uint32_t flatOffset, bool chainStart)
        {
            if (chainStart)
                g_building = ChainTable{};
            if (g_kicked.empty() || !in(tagAddr, kPktLo, kPktHi))
                return;
            if (g_building.records.empty())
                g_building.records = g_kicked;
            int ownOf = -1; // the draw this tag is the own tag of (1b: its stream region)
            struct Push // every main-stream packet-buffer tag starts a region
            {
                int &own;
                uint32_t flat;
                bool main;
                ~Push()
                {
                    if (main)
                        g_building.entries.push_back({flat, own});
                }
            } push{ownOf, flatOffset, (tagAddr & kRamMask) < g_kickedEnd || (tagAddr & kRamMask) >= g_kickedEnd + 0xE6670u};
            tagAddr &= kRamMask;
            if (tagAddr >= g_kickedEnd && tagAddr < g_kickedEnd + 0xE6670u)
            {
                // a block allocated from the top of the half (the +0x1C pointer), CALLed from the main stream
                ++g_stats.topTags[id & 7u];
                const bool call3d = id == 5u && in(tagTarget, kModelsLo, kModelsHi);
                const bool ref3d = id == 3u && qwc && !direct(vifCodes) && (in(dataAddr, kCellsLo, kCellsHi) || in(dataAddr, kModelsLo, kModelsHi));
                g_stats.top3d += (call3d || ref3d) ? 1u : 0u;
                return;
            }
            const int r = recordAt(tagAddr);
            ++g_stats.tagsByFn[{r >= 0 ? g_kicked[r].fn : -1, id}];
            // 3D: a packet CALLed in the model heap, or VU1 data REFed from the cells / the model heap
            const bool call3d = id == 5u && in(tagTarget, kModelsLo, kModelsHi);
            const bool ref3d = id == 3u && qwc && !direct(vifCodes) && (in(dataAddr, kCellsLo, kCellsHi) || in(dataAddr, kModelsLo, kModelsHi));
            // the draw whose range [start, next draw's start) holds the tag
            const auto it = std::upper_bound(g_draws.begin(), g_draws.end(), tagAddr,
                                             [](uint32_t a, size_t i) { return a < g_kicked[i].start; });
            // the draw's own tag anywhere in its range (a setter the state flush calls may have been entered before it);
            // other tags: the draw's state when the draw itself is the record they fall in, else the function's
            const bool inDraw = r >= 0 && kFns[g_kicked[r].fn].kind != Boundary;
            bool own = false;
            if (it != g_draws.begin())
            {
                Record &d = g_kicked[*(it - 1)];
                const Kind k = kFns[d.fn].kind;
                if (!d.matched && k == Model && id == 5u && (tagTarget & kRamMask) == (d.a0 & kRamMask))
                    d.matched = own = true;
                else if (!d.matched && k == World && id == 3u && (dataAddr & kRamMask) == (d.a0 & kRamMask) &&
                         qwc == (d.a1 & 0xFFFFu))
                    d.matched = own = true;
                if (own)
                    ownOf = static_cast<int>(*(it - 1));
            }
            if (call3d || ref3d)
            {
                ++g_stats.tags3d;
                if (own)
                    ++g_stats.own3d;
                else if (inDraw)
                    ++g_stats.state3d; // the draw's state flush (FUN_002F8710: the object's packet, plug-ins) or immediate data
                else
                {
                    ++g_stats.unexplained3d;
                    ++g_stats.unexplainedByFn[r >= 0 ? g_kicked[r].fn : -1];
                    static int logged = 0;
                    if (logged++ < 12)
                        std::fprintf(stderr, "[rsl-shadow] not from a draw: tag@%08x id %u target %08x data %08x qwc %u vif %016llx; in %06x "
                                             "(a0 %08x, entered at %08x, +%u), main stream ends %08x\n",
                                     tagAddr, id, tagTarget, dataAddr, qwc, static_cast<unsigned long long>(vifCodes),
                                     r >= 0 ? kFns[g_kicked[r].fn].address : 0u, r >= 0 ? g_kicked[r].a0 : 0u,
                                     r >= 0 ? g_kicked[r].start : 0u, r >= 0 ? tagAddr - g_kicked[r].start : 0u, g_kickedEnd);
                }
            }
        }

        uint32_t observe(uint32_t tagAddr, uint32_t id, uint32_t tagTarget, uint32_t dataAddr, uint32_t qwc, uint64_t vifCodes,
                         uint32_t flatOffset, bool chainStart)
        {
            observeTag(tagAddr, id, tagTarget, dataAddr, qwc, vifCodes, flatOffset, chainStart);
            return 0u; // never replaces a payload
        }

        // the copied chain went into the queue: its table goes with it (the game thread)
        void queued(const void *key, uint32_t sizeBytes)
        {
            (void)sizeBytes;
            if (g_building.entries.empty())
                return;
            auto table = std::make_shared<ChainTable>(std::move(g_building));
            g_building = ChainTable{};
            std::lock_guard<std::mutex> lock(g_tablesMutex);
            g_tables[key] = std::move(table);
            if (g_tables.size() > 8u) // chains whose processing was never seen (should not happen)
                g_tables.erase(g_tables.begin());
        }

        // the thread processing a chain takes its table
        void processing(const void *key, bool begin)
        {
            if (!begin)
            {
                t_processing.reset();
                t_stream = nullptr;
                return;
            }
            std::lock_guard<std::mutex> lock(g_tablesMutex);
            const auto it = g_tables.find(key);
            if (it == g_tables.end())
                return;
            t_processing = std::move(it->second);
            t_stream = static_cast<const uint8_t *>(key);
            g_tables.erase(it);
        }

        // the frame whose chain was copied since the last kick
        void evaluate()
        {
            if (g_kicked.empty())
                return;
            ++g_stats.frames;
            for (const Record &d : g_kicked)
            {
                const Kind k = kFns[d.fn].kind;
                if (k == Boundary)
                    continue;
                ++g_stats.draws[k];
                g_stats.matched[k] += (d.matched || k == Immediate) ? 1u : 0u;
            }
        }

        const char *fnName(int fn)
        {
            static char buf[16];
            if (fn < 0)
                return "(none)";
            std::snprintf(buf, sizeof(buf), "%06x", kFns[fn].address);
            return buf;
        }

        void report()
        {
            Stats &s = g_stats;
            const double n = std::max<double>(1.0, static_cast<double>(s.frames));
            std::fprintf(stderr, "[rsl-shadow] %llu frames, per frame:", static_cast<unsigned long long>(s.frames));
            for (int k = Model; k < KindCount; ++k)
                std::fprintf(stderr, " %s %.1f (%.1f matched)", kKindNames[k], s.draws[k] / n, s.matched[k] / n);
            std::fprintf(stderr, "; 3D tags in the packet buffer %.1f: the draws' own %.1f, their state %.1f, not from a draw %.2f\n",
                         s.tags3d / n, s.own3d / n, s.state3d / n, s.unexplained3d / n);
            std::fprintf(stderr, "[rsl-shadow]   top blocks: cnt %.1f ref %.1f call %.1f ret %.1f tags, 3D %.1f\n", s.topTags[1] / n,
                         s.topTags[3] / n, s.topTags[5] / n, s.topTags[6] / n, s.top3d / n);
            for (const auto &[fn, count] : s.unexplainedByFn)
                std::fprintf(stderr, "[rsl-shadow]   3D tag not from a draw, written in %s: %.2f per frame\n", fnName(fn), count / n);
            static const char *const kIds[] = {"refe", "cnt", "next", "ref", "refs", "call", "ret", "end"};
            std::map<int, std::string> byFn;
            for (const auto &[key, count] : s.tagsByFn)
            {
                char t[48];
                std::snprintf(t, sizeof(t), " %s %.1f", kIds[key.second & 7u], count / n);
                byFn[key.first] += t;
            }
            for (const auto &[fn, text] : byFn)
                std::fprintf(stderr, "[rsl-shadow]   tags written in %s:%s\n", fnName(fn), text.c_str());
            s = Stats{};
            StateStats t;
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                t = g_stateStats;
                g_stateStats = StateStats{};
            }
            if (t.inDraws)
            {
                const double d = static_cast<double>(t.inDraws);
                std::fprintf(stderr, "[rsl-shadow] state (1b): %llu draw-program MSCALs, %llu inside a draw's stream; VU as the draw's RSL state:",
                             static_cast<unsigned long long>(t.mscals), static_cast<unsigned long long>(t.inDraws));
                for (int g = 0; g < kGroupCount; ++g)
                    std::fprintf(stderr, " %s %.2f%%,", kGroups[g].name, 100.0 * t.same[g] / d);
                std::fprintf(stderr, " flags 52.y %.2f%%\n", 100.0 * t.flagsSame / d);
                for (int g = 0; g < kGroupCount; ++g)
                    if (t.stream[g] || t.carried[g] || t.earlier[g] || t.unexplained[g])
                        std::fprintf(stderr,
                                     "[rsl-shadow]   %s differs: from the draw's own stream %.2f%%, carried from the draw before %.2f%%, "
                                     "uploaded earlier in the chain %.2f%%, unexplained %.3f%%\n",
                                     kGroups[g].name, 100.0 * t.stream[g] / d, 100.0 * t.carried[g] / d, 100.0 * t.earlier[g] / d,
                                     100.0 * t.unexplained[g] / d);
                for (int k = Model; k < KindCount; ++k)
                    if (t.byKind[k])
                        std::fprintf(stderr, "[rsl-shadow]   %s draws: %llu MSCALs, all groups the RSL state %.2f%%, all explained %.3f%%\n",
                                     kKindNames[k], static_cast<unsigned long long>(t.byKind[k]), 100.0 * t.allSameByKind[k] / t.byKind[k],
                                     100.0 * t.allExplainedByKind[k] / t.byKind[k]);
            }
        }
    }

    void onMscal(uint32_t startPc, const uint8_t *vu)
    {
        const ChainTable *table = t_processing.get();
        if (!table || (startPc != 0x30u && startPc != 0x2C0u))
            return;
        const uint32_t at = ps2chain::streamOffset();
        const auto it = std::upper_bound(table->entries.begin(), table->entries.end(), at,
                                         [](uint32_t a, const std::pair<uint32_t, int> &e) { return a < e.first; });
        const int draw = it == table->entries.begin() ? -1 : (it - 1)->second;
        std::lock_guard<std::mutex> lock(g_stateMutex);
        StateStats &t = g_stateStats;
        ++t.mscals;
        if (draw < 0)
            return;
        ++t.inDraws;
        const Record &r = table->records[static_cast<size_t>(draw)];
        const State &s = r.state;
        // the draw's own stream so far: its state (everything after the previous draw's region: the flush, setters'
        // uploads), its CALL / REF and what that brought in, up to this MSCAL
        auto first = it - 1;
        while (first != table->entries.begin() && (first - 1)->second < 0)
            --first;
        const uint32_t regionStart = first->first;
        const uint8_t *stream = t_stream ? t_stream + regionStart : nullptr;
        const size_t streamBytes = stream && at > regionStart ? at - regionStart : 0u;
        // a block of bytes (the differing rows of a group, together) in the draw's own stream / anywhere in the chain
        // before this MSCAL
        const auto inStream = [&](const uint8_t *rows, size_t bytes) {
            for (size_t o = 0; o + bytes <= streamBytes; o += 4u)
                if (std::memcmp(stream + o, rows, bytes) == 0)
                    return true;
            return false;
        };
        const auto inChainBefore = [&](const uint8_t *rows, size_t bytes) {
            for (size_t o = 0; t_stream && o + bytes <= at; o += 4u)
                if (std::memcmp(t_stream + o, rows, bytes) == 0)
                    return true;
            return false;
        };
        thread_local uint8_t lastVu[1024 * 16]; // the VU memory the previous draw program started with
        thread_local bool haveLast = false;
        bool allSame = true, viewportSame = true, allExplained = true;
        for (int g = 0; g < kGroupCount; ++g)
        {
            const Group &G = kGroups[g];
            const uint8_t *expected = G.source >= 0 ? s.blocks + G.source
                                    : G.source == -1 ? s.viewport
                                    : G.source == -2 ? s.objectBytes + 0x60
                                                     : s.vu49 + G.sourceOffset;
            const bool same = std::memcmp(vu + G.vu * 16u, expected, G.qwords * 16u) == 0;
            t.same[g] += same ? 1u : 0u;
            allSame = allSame && same;
            if (G.source == -1)
                viewportSame = same;
            if (!same)
            {
                // each differing row: uploaded by the draw's own stream (its packet's state), carried over from the
                // draw before (VU memory persists), uploaded earlier in this chain, or unexplained
                // the differing rows as one block (first .. last differing row): sources upload them together
                uint32_t lo = G.qwords, hi = 0;
                for (uint32_t q = 0; q < G.qwords; ++q)
                    if (std::memcmp(vu + (G.vu + q) * 16u, expected + q * 16u, 16) != 0)
                    {
                        lo = std::min(lo, q);
                        hi = q + 1u;
                    }
                const uint8_t *rows = vu + (G.vu + lo) * 16u;
                const size_t bytes = (hi - lo) * 16u;
                int worst; // 0 own stream, 1 carried, 2 earlier in the chain, 3 unexplained
                if (inStream(rows, bytes))
                    worst = 0;
                else if (haveLast && std::memcmp(rows, lastVu + (G.vu + lo) * 16u, bytes) == 0)
                    worst = 1;
                else
                    worst = inChainBefore(rows, bytes) ? 2 : 3;
                (worst == 0 ? t.stream : worst == 1 ? t.carried : worst == 2 ? t.earlier : t.unexplained)[g] += 1u;
                allExplained = allExplained && worst < 3;
            }
        }
        std::memcpy(lastVu, vu, sizeof(lastVu));
        haveLast = true;
        t.allExplainedByKind[kFns[r.fn].kind] += allExplained ? 1u : 0u;
        t.flagsSame += std::memcmp(vu + 52u * 16u + 4u, s.vu49 + 0x34, 4) == 0 ? 1u : 0u;
        // VCS_RSL_SHADOW_FIND=1: where in guest RAM the VU's viewport z row (14) and object matrix (0) of the first
        // mismatching draws are (finding their source)
        static const bool find = std::getenv("VCS_RSL_SHADOW_FIND") != nullptr;
        static int found = 0;
        if (find && g_rdram && found < 16 && (!viewportSame || std::memcmp(vu, s.blocks, 64) != 0))
        {
            ++found;
            for (const uint32_t q : {14u, 15u, 0u, 3u})
            {
                std::fprintf(stderr, "[rsl-shadow] draw %06x a0 %08x dirty %05x: VU %u found at", kFns[r.fn].address, r.a0, s.dirty, q);
                int hits = 0;
                for (uint32_t a = 0; a + 16u <= PS2_RAM_SIZE && hits < 6; a += 16u)
                    if (std::memcmp(g_rdram + a, vu + q * 16u, 16) == 0)
                    {
                        std::fprintf(stderr, " %08x", a);
                        ++hits;
                    }
                std::fprintf(stderr, "%s\n", hits ? "" : " (nowhere)");
            }
        }
        ++t.byKind[kFns[r.fn].kind];
        t.allSameByKind[kFns[r.fn].kind] += allSame ? 1u : 0u;
        // VCS_RSL_SHADOW_DUMP=<n>: the first n draws' state and VU memory at their program (analysis: build/run)
        static const int dumpMax = std::getenv("VCS_RSL_SHADOW_DUMP") ? std::atoi(std::getenv("VCS_RSL_SHADOW_DUMP")) : 0;
        static int dumped = 0, dumpedDiff = 0;
        static uint64_t seen = 0;
        ++seen;
        // spread over the run after the start-up, and as many viewport mismatches
        const bool take = seen > 200000u && (viewportSame ? (seen % 997u == 0u && dumped - dumpedDiff < dumpMax / 2) : dumpedDiff < dumpMax / 2);
        if (dumped < dumpMax && take)
        {
            char name[64];
            dumpedDiff += viewportSame ? 0 : 1;
            std::snprintf(name, sizeof(name), "rsl_shadow_%03d.bin", dumped++);
            if (FILE *f = std::fopen(name, "wb"))
            {
                const uint32_t head[4] = {kFns[r.fn].address, r.a0, r.a1, startPc};
                std::fwrite(head, 1, sizeof(head), f);
                std::fwrite(&s, 1, sizeof(s), f);
                std::fwrite(vu, 1, 16384, f);
                std::fclose(f);
            }
        }
    }

    void install(PS2Runtime &runtime)
    {
        if (!std::getenv("VCS_RSL_SHADOW"))
            return;
        for (int i = 0; i < kCount; ++i)
        {
            g_originals[i] = runtime.lookupFunction(kFns[i].address);
            if (g_originals[i])
                runtime.replaceFunction(kFns[i].address, hookFor(i, std::make_integer_sequence<int, kCount>{}));
        }
        ps2chain::addTagObserver(&observe);
        ps2chain::addChainObservers(&queued, &processing);
    }

    void onFrameKick(const uint8_t *rdram)
    {
        if (!g_originals[0])
            return; // not installed
        evaluate();
        g_kickedEnd = cursor(rdram);
        // the frame's records in buffer order (entries are in call order; the cursor only grows within a frame)
        g_kicked.swap(g_current);
        g_current.clear();
        std::stable_sort(g_kicked.begin(), g_kicked.end(), [](const Record &a, const Record &b) { return a.start < b.start; });
        g_draws.clear();
        for (size_t i = 0; i < g_kicked.size(); ++i)
            if (kFns[g_kicked[i].fn].kind != Boundary)
                g_draws.push_back(i);
        const auto now = std::chrono::steady_clock::now();
        if (now - g_lastReport >= std::chrono::seconds(5))
        {
            g_lastReport = now;
            report();
        }
    }
}
