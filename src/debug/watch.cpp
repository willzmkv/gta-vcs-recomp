// Write watchpoint backend for ps2TraceGuestWrite (third_party ps2_runtime.h).
//   VCS_WATCH=<lo>,<hi>[,<value>[,<mask>]]   hex guest addresses, optional value to match;
//                                   with a mask, any 32-bit lane where (lane & mask) == value
//   VCS_WATCH_MAX=<n>               stop logging after n hits (default 50)
//   VCS_WATCH_STACK=1               also list the code addresses on the guest stack above sp at each hit (callers' saved
//                                   $ra among them) - who called the function that wrote
//   VCS_WATCH_STRIDE=<stride>,<offset>   hex; only writes touching bytes offset..offset+7 of each stride-sized entry from lo
//                                   (one field of every entry of a pool, e.g. 0xD20,0x1C8 for the peds' flags at +0x1C8)
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

namespace
{
    uint8_t *g_watchRam = nullptr;
    bool g_watchStack = false;
    uint32_t g_watchLo = 0, g_watchStride = 0, g_watchOffset = 0;

    // words above sp that look like return addresses into the game's code (.text 0x100000..0x480000)
    std::string stackCallers(const R5900Context *ctx)
    {
        std::string out;
        if (!g_watchRam || !ctx)
            return out;
        const uint32_t sp = GPR_U32(ctx, 29) & PS2_RAM_MASK;
        int found = 0;
        for (uint32_t off = 0; off < 0x400u && sp + off + 4u <= PS2_RAM_SIZE && found < 16; off += 4u)
        {
            uint32_t w;
            std::memcpy(&w, g_watchRam + sp + off, 4);
            if (w >= 0x100000u && w < 0x480000u && (w & 3u) == 0u)
            {
                char buf[24];
                std::snprintf(buf, sizeof(buf), " %x@+%x", w, off);
                out += buf;
                ++found;
            }
        }
        return out;
    }
}

void ps2WriteWatchHit(uint32_t guestAddr, uint32_t size, uint64_t value, const char *op, const R5900Context *ctx)
{
    if (g_watchStride)
    {
        const uint32_t rel = (guestAddr - g_watchLo) % g_watchStride; // keep writes overlapping offset..offset+7
        if (rel + size <= g_watchOffset || rel >= g_watchOffset + 8u)
            return;
    }
    static std::atomic<int> hits{0};
    static const int maxHits = std::getenv("VCS_WATCH_MAX") ? std::atoi(std::getenv("VCS_WATCH_MAX")) : 50;
    const int n = hits.fetch_add(1);
    if (n >= maxHits)
        return;
    // a0 / s0 / s1: in the script command handlers one of them is the running CRunningScript (game-internals.md 11b)
    std::fprintf(stderr, "[watch] %s addr=0x%08x size=%u value=0x%llx pc=0x%08x ra=0x%08x sp=0x%08x a0=0x%08x s0=0x%08x s1=0x%08x\n",
                 op, guestAddr, size, static_cast<unsigned long long>(value), ctx ? ctx->pc : 0u, ctx ? GPR_U32(ctx, 31) : 0u,
                 ctx ? GPR_U32(ctx, 29) : 0u, ctx ? GPR_U32(ctx, 4) : 0u, ctx ? GPR_U32(ctx, 16) : 0u,
                 ctx ? GPR_U32(ctx, 17) : 0u);
    if (g_watchStack && ctx)
        std::fprintf(stderr, "[watch]   s2-s7 %08x %08x %08x %08x %08x %08x stack%s\n", GPR_U32(ctx, 18), GPR_U32(ctx, 19),
                     GPR_U32(ctx, 20), GPR_U32(ctx, 21), GPR_U32(ctx, 22), GPR_U32(ctx, 23), stackCallers(ctx).c_str());
}

// VCS_RWATCH=<lo>,<hi>: report each distinct pc that reads the range (first 300).
void ps2ReadWatchHit(uint32_t guestAddr, uint32_t size, const R5900Context *ctx)
{
    static std::mutex mutex;
    static std::unordered_set<uint32_t> seen;
    const uint32_t pc = ctx ? ctx->pc : 0u;
    std::lock_guard<std::mutex> lock(mutex);
    if (seen.size() >= 300u || !seen.insert(pc).second)
        return;
    std::fprintf(stderr, "[rwatch] READ%u addr=0x%08x pc=0x%08x ra=0x%08x sp=0x%08x\n", size * 8u, guestAddr, pc,
                 ctx ? GPR_U32(ctx, 31) : 0u, ctx ? GPR_U32(ctx, 29) : 0u);
}

namespace vcs
{
    bool armWatch(const char *spec);

    void configureWatch(uint8_t *rdram)
    {
        g_watchRam = rdram;
        g_watchStack = std::getenv("VCS_WATCH_STACK") && std::getenv("VCS_WATCH_STACK")[0] == '1';
        if (const char *st = std::getenv("VCS_WATCH_STRIDE"))
        {
            unsigned stride = 0, offset = 0;
            if (std::sscanf(st, "%x,%x", &stride, &offset) == 2 && stride)
                g_watchStride = stride, g_watchOffset = offset;
        }
        if (const char *r = std::getenv("VCS_RWATCH"))
        {
            unsigned long long lo = 0, hi = 0;
            if (std::sscanf(r, "%llx,%llx", &lo, &hi) == 2 && hi > lo)
            {
                g_ps2ReadWatch.lo = static_cast<uint32_t>(lo);
                g_ps2ReadWatch.span = static_cast<uint32_t>(hi - lo);
                g_ps2FastReadLimit = 0u; // every read through the checked path
                std::fprintf(stderr, "[rwatch] armed 0x%08llx..0x%08llx\n", lo, hi);
            }
        }
        if (const char *env = std::getenv("VCS_WATCH"))
            armWatch(env);
    }

    // "<lo>,<hi>[,<value>[,<mask>]]" as VCS_WATCH; also the rig's `watch` command (a watch armed mid-run, e.g. just
    // before the moment of interest). Returns false on a malformed spec.
    bool armWatch(const char *spec)
    {
        unsigned long long lo = 0, hi = 0, value = 0, mask = 0;
        const int n = std::sscanf(spec, "%llx,%llx,%llx,%llx", &lo, &hi, &value, &mask);
        if (n < 2 || hi <= lo)
            return false;
        g_ps2WriteWatch.lo = g_watchLo = static_cast<uint32_t>(lo);
        g_ps2WriteWatch.matchValue = n >= 3;
        g_ps2WriteWatch.value = value;
        g_ps2WriteWatch.laneMask = n >= 4 ? static_cast<uint32_t>(mask) : 0u;
        g_ps2WriteWatch.span = static_cast<uint32_t>(hi - lo);
        g_ps2FastWriteLimit = 0u; // every write through the checked path
        std::fprintf(stderr, "[watch] armed 0x%08llx..0x%08llx%s\n", lo, hi, n >= 3 ? " (value match)" : "");
        return true;
    }
}
