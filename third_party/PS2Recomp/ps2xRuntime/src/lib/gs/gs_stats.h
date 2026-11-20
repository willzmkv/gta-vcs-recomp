// [gta-recomp] VCS_GS_STATS=<seconds>: histogram of the GS state a game draws with
// (formats, blend/test modes, texture functions...), to scope a hardware renderer.
#pragma once

#include "runtime/gs/gs_types.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include "runtime/ps2_host_std.h"

class GSStats
{
public:
    static GSStats &instance()
    {
        static GSStats stats;
        return stats;
    }

    bool enabled() const { return m_period > 0.0; }

    void count(const GSPrimitiveBatch &b)
    {
        const GSDrawState &s = b.state;
        const GSContext &c = s.context;
        char key[256];
        const uint64_t test = c.test;
        const unsigned ate = test & 1u, atst = (test >> 1) & 7u, afail = (test >> 12) & 3u, date = (test >> 14) & 1u;
        const unsigned zte = (test >> 16) & 1u, ztst = (test >> 17) & 3u;
        const uint64_t a = c.alpha;
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_total;
        std::snprintf(key, sizeof(key), "prim=%u tme=%u abe=%u fge=%u fst=%u", s.prim.type, s.prim.tme, s.prim.abe, s.prim.fge, s.prim.fst);
        ++m_maps["prim"][key];
        std::snprintf(key, sizeof(key), "fpsm=%u zpsm=%u zmsk=%u fbmsk=%s", c.frame.psm, c.zbuf.psm, c.zbuf.zmask,
                      c.frame.fbmsk == 0 ? "0" : (c.frame.fbmsk == 0xFF000000u ? "ff000000" : "other"));
        ++m_maps["target"][key];
        std::snprintf(key, sizeof(key), "ate=%u atst=%u afail=%u date=%u zte=%u ztst=%u", ate, atst, afail, date, zte, ztst);
        ++m_maps["test"][key];
        if (s.prim.abe)
        {
            std::snprintf(key, sizeof(key), "A=%u B=%u C=%u D=%u fix=%u pabe=%u", unsigned(a & 3u), unsigned((a >> 2) & 3u),
                          unsigned((a >> 4) & 3u), unsigned((a >> 6) & 3u), unsigned((a >> 32) & 0xFFu), s.pabe);
            ++m_maps["blend"][key];
        }
        if (s.prim.tme)
        {
            std::snprintf(key, sizeof(key), "psm=%u cpsm=%u csm=%u tfx=%u tcc=%u wms=%u wmt=%u linear=%u", c.tex0.psm, c.tex0.cpsm,
                          c.tex0.csm, c.tex0.tfx, c.tex0.tcc, unsigned(c.clamp & 3u), unsigned((c.clamp >> 2) & 3u), s.linearFilter);
            ++m_maps["texture"][key];
        }
        std::snprintf(key, sizeof(key), "colclamp=%u dthe=%u fba=%u", unsigned(s.colclamp & 1u), unsigned(s.dthe & 1u), unsigned(c.fba & 1u));
        ++m_maps["misc"][key];
        const auto now = ps2x::hostNow();
        if (std::chrono::duration<double>(now - m_since).count() >= m_period)
        {
            std::fprintf(stderr, "[gs-stats] %llu primitives\n", static_cast<unsigned long long>(m_total));
            for (const auto &[group, entries] : m_maps)
                for (const auto &[k, n] : entries)
                    std::fprintf(stderr, "[gs-stats] %-8s %8llu  %s\n", group.c_str(), static_cast<unsigned long long>(n), k.c_str());
            std::fflush(stderr);
            m_since = now;
        }
    }

private:
    GSStats()
    {
        const char *env = std::getenv("VCS_GS_STATS");
        m_period = env ? std::atof(env) : 0.0;
        m_since = ps2x::hostNow();
    }
    double m_period = 0.0;
    std::mutex m_mutex;
    uint64_t m_total = 0;
    std::map<std::string, std::map<std::string, uint64_t>> m_maps;
    std::chrono::steady_clock::time_point m_since;
};
