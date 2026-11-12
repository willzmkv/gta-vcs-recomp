// [gta-recomp] See runtime/ps2_hle_survey.h.
#include "runtime/ps2_vu1_tap.h"
#include "runtime/ps2_hle_survey.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include "runtime/ps2_host_std.h"

namespace ps2hle
{
    uint64_t g_gsVerticesRecorded = 0;

    namespace
    {
        constexpr uint32_t kVuCodeBytes = 16384u;
        constexpr uint32_t kVuDataBytes = 16384u;

        uint64_t mix(uint64_t h, uint64_t v)
        {
            h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
            return h * 0xFF51AFD7ED558CCDull;
        }

        struct LayoutStats
        {
            uint64_t calls = 0, vectors = 0, verts = 0, ns = 0;
            uint32_t samples = 0;
            uint32_t exampleTarget = 0;
            std::string description; // the UNPACK sequence of the first call
        };

        struct ProgramStats
        {
            uint32_t pc = 0, codeHash = 0;
            uint64_t calls = 0, inPacket = 0, verts = 0, ns = 0;
            uint64_t nativeCalls = 0, nativeVerts = 0, nativeNs = 0; // [gta-recomp] handled by a game hook, the VU1 skipped
            std::unordered_set<uint32_t> targets;
            std::unordered_map<uint64_t, LayoutStats> layouts;
        };

        std::unordered_map<uint64_t, ProgramStats> g_programs;
        uint64_t g_layout = 0x51A7ull, g_layoutVectors = 0;
        std::string g_layoutText;
        uint64_t g_chains = 0, g_vertsOutside = 0;
        auto g_since = ps2x::hostNow();
        auto g_reportAt = ps2x::hostNow();

        // the MSCAL in flight
        ProgramStats *g_current = nullptr;
        LayoutStats *g_currentLayout = nullptr;
        std::chrono::steady_clock::time_point g_t0;
        uint64_t g_verts0 = 0;
        bool g_dumpAfter = false;
        std::string g_dumpBase;

        const char *unpackName(uint8_t opcode)
        {
            static const char *names[16] = {"S-32", "S-16", "S-8", "?", "V2-32", "V2-16", "V2-8", "?",
                                            "V3-32", "V3-16", "V3-8", "?", "V4-32", "V4-16", "V4-8", "V4-5"};
            return names[opcode & 0xFu];
        }

        void writeFile(const std::string &path, const void *data, size_t size)
        {
            if (FILE *f = std::fopen(path.c_str(), "wb"))
            {
                std::fwrite(data, 1, size, f);
                std::fclose(f);
            }
        }

        void report()
        {
            const double secs = std::max(1e-3, std::chrono::duration<double>(ps2x::hostNow() - g_since).count());
            std::vector<ProgramStats *> order;
            uint64_t allVerts = g_vertsOutside, allNs = 0;
            for (auto &[key, p] : g_programs)
            {
                order.push_back(&p);
                allVerts += p.verts;
                allNs += p.ns;
            }
            std::sort(order.begin(), order.end(), [](auto *a, auto *b) { return a->verts > b->verts; });
            std::string out;
            char line[512];
            std::snprintf(line, sizeof(line),
                          "[hle-survey] %.0f s, %llu VIF1 chains (%.1f/s): %.0fk GS verts/s from VU1 programs, VU1 %.1f ms/s; "
                          "%.0fk verts/s outside programs\n",
                          secs, (unsigned long long)g_chains, g_chains / secs, (allVerts - g_vertsOutside) / secs / 1000.0, allNs / secs / 1e6,
                          g_vertsOutside / secs / 1000.0);
            out += line;
            for (ProgramStats *p : order)
            {
                std::snprintf(line, sizeof(line),
                              "  pc 0x%04x code %08x: %8.0f calls/s (%3.0f%% in packets, %zu packets), %7.1fk verts/s (%4.1f%%), "
                              "VU1 %6.2f ms/s, %zu layouts; native %3.0f%% of calls (%.2f ms/s), on the VU1 %.0f calls/s %.1fk verts/s %.2f ms/s\n",
                              p->pc, p->codeHash, p->calls / secs, p->calls ? 100.0 * p->inPacket / p->calls : 0.0, p->targets.size(),
                              p->verts / secs / 1000.0, allVerts ? 100.0 * p->verts / allVerts : 0.0, p->ns / secs / 1e6, p->layouts.size(),
                              p->calls ? 100.0 * p->nativeCalls / p->calls : 0.0, p->nativeNs / secs / 1e6,
                              (p->calls - p->nativeCalls) / secs, (p->verts - p->nativeVerts) / secs / 1000.0,
                              (p->ns - p->nativeNs) / secs / 1e6);
                out += line;
                std::vector<std::pair<uint64_t, LayoutStats *>> layouts;
                for (auto &[k, l] : p->layouts)
                    layouts.push_back({k, &l});
                std::sort(layouts.begin(), layouts.end(), [](auto &a, auto &b) { return a.second->verts > b.second->verts; });
                for (size_t i = 0; i < layouts.size() && i < 4; ++i)
                {
                    const LayoutStats &l = *layouts[i].second;
                    std::snprintf(line, sizeof(line), "      layout %016llx: %7.0f calls/s, %5.1f vectors/call, %6.1f verts/call, e.g. packet 0x%08x: %s\n",
                                  (unsigned long long)layouts[i].first, l.calls / secs, l.calls ? double(l.vectors) / l.calls : 0.0,
                                  l.calls ? double(l.verts) / l.calls : 0.0, l.exampleTarget, l.description.c_str());
                    out += line;
                }
            }
            std::fputs(out.c_str(), stderr);
            writeFile("hle_survey.txt", out.data(), out.size());
        }
    }

    bool surveyEnabled()
    {
        static const bool on = std::getenv("PS2X_HLE_SURVEY") != nullptr;
        return on;
    }

    void onUnpack(const ChainSegment *segment, uint8_t opcode, uint8_t num, uint16_t imm)
    {
        (void)segment;
        g_layout = mix(g_layout, (uint64_t(opcode) << 16) | imm);
        g_layoutVectors += num ? num : 256u;
        if (g_layoutText.size() < 200)
        {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "%s%s@%03x%s x%u", g_layoutText.empty() ? "" : ", ", unpackName(opcode), imm & 0x3FFu,
                          (imm & 0x8000u) ? "+top" : "", num ? num : 256u);
            g_layoutText += buf;
        }
    }

    void beforeMscal(const ChainSegment *segment, uint32_t startPc, const uint8_t *vuCode, const uint8_t *vuData,
                     const uint8_t *stream, uint32_t streamSize)
    {
        // VCS's renderer at 0x0030 is a dispatcher into stages swapped by per-pass uploads (0x2700..0x2fff): key the
        // program by the code at the start PC and that region
        uint32_t codeHash = 0x811C9DC5u;
        for (uint32_t i = startPc; i < std::min(startPc + 512u, kVuCodeBytes); ++i)
            codeHash = (codeHash ^ vuCode[i]) * 16777619u;
        for (uint32_t i = 0x2700u; i < 0x3000u; ++i)
            codeHash = (codeHash ^ vuCode[i]) * 16777619u;
        ProgramStats &p = g_programs[(uint64_t(startPc) << 32) | codeHash];
        p.pc = startPc;
        p.codeHash = codeHash;
        ++p.calls;
        if (segment)
        {
            ++p.inPacket;
            p.targets.insert(segment->target);
        }
        LayoutStats &l = p.layouts[g_layout];
        if (!l.calls)
        {
            l.exampleTarget = segment ? segment->target : 0u;
            l.description = g_layoutText.empty() ? "(no unpacks)" : g_layoutText;
        }
        ++l.calls;
        l.vectors += g_layoutVectors;

        static const uint32_t maxSamples = std::getenv("PS2X_HLE_SURVEY_DUMP") ? std::atoi(std::getenv("PS2X_HLE_SURVEY_DUMP")) : 0u;
        // PS2X_HLE_SURVEY_DUMP_EVERY=n: sample calls n, 2n, ... of each layout (default 1: the first ones, i.e. boot)
        static const uint64_t every = std::max(1, std::getenv("PS2X_HLE_SURVEY_DUMP_EVERY") ? std::atoi(std::getenv("PS2X_HLE_SURVEY_DUMP_EVERY")) : 1);
        g_dumpAfter = false;
        if (l.samples < maxSamples && l.calls % every == 0u)
        {
            std::filesystem::create_directories("hle_samples");
            char base[96];
            std::snprintf(base, sizeof(base), "hle_samples/pc%04x_%016llx_%u", startPc, (unsigned long long)g_layout, l.samples++);
            g_dumpBase = base;
            if (segment && segment->end <= streamSize)
                writeFile(g_dumpBase + "_packet.bin", stream + segment->begin, segment->end - segment->begin);
            writeFile(g_dumpBase + "_vudata_before.bin", vuData, kVuDataBytes);
            writeFile(g_dumpBase + "_vucode.bin", vuCode, kVuCodeBytes);
            const std::string meta = "pc " + std::to_string(startPc) + "\npacket " + std::to_string(segment ? segment->target : 0u) +
                                     "\nunpacks " + g_layoutText + "\n";
            writeFile(g_dumpBase + "_meta.txt", meta.data(), meta.size());
            g_dumpAfter = true;
        }
        g_current = &p;
        g_currentLayout = &l;
        g_layout = 0x51A7ull;
        g_layoutVectors = 0;
        g_layoutText.clear();
        g_vertsOutside += g_gsVerticesRecorded - g_verts0; // since the last program: DIRECT / PATH3 drawing
        g_verts0 = g_gsVerticesRecorded;
        g_t0 = ps2x::hostNow();
    }

    void afterMscal(const uint8_t *vuData)
    {
        if (!g_current)
            return;
        const uint64_t ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(ps2x::hostNow() - g_t0).count());
        const uint64_t verts = g_gsVerticesRecorded - g_verts0;
        g_current->ns += ns;
        g_current->verts += verts;
        if (ps2vu1tap::g_mscalNative)
        {
            ++g_current->nativeCalls;
            g_current->nativeVerts += verts;
            g_current->nativeNs += ns;
        }
        g_currentLayout->ns += ns;
        g_currentLayout->verts += verts;
        if (g_dumpAfter)
            writeFile(g_dumpBase + "_vudata_after.bin", vuData, kVuDataBytes);
        g_current = nullptr;
        g_currentLayout = nullptr;
        g_verts0 = g_gsVerticesRecorded;
    }

    namespace
    {
        GS *g_gs = nullptr;
        MscalBefore g_before = nullptr;
        MscalAfter g_after = nullptr;
    }

    void setGs(GS *gs) { g_gs = gs; }
    GS *gs() { return g_gs; }

    void setMscalHooks(MscalBefore before, MscalAfter after)
    {
        g_before = before;
        g_after = after;
    }

    MscalBefore mscalBefore() { return g_before; }
    MscalAfter mscalAfter() { return g_after; }

    void onChainEnd()
    {
        ++g_chains;
        const auto now = ps2x::hostNow();
        if (now - g_reportAt >= std::chrono::seconds(10))
        {
            g_reportAt = now;
            report();
        }
    }
}
