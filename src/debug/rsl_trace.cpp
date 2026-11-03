// The RSL submission trace - see debug/rsl_trace.h.
#include "debug/rsl_trace.h"

#include "ps2_runtime.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

namespace vcs::rsltrace
{
    namespace
    {
        // the packet buffer object (game-internals.md 6i): +0x14 the half in use (two of 0xE6670 bytes from +0x20),
        // +0x18 the DMA cursor growing up from the half's start, +0x1C a data pointer growing down from its end
        constexpr uint32_t kBuffer = 0x520F90u, kHalf = 0xE6670u;
        constexpr uint32_t kRamMask = PS2_RAM_MASK;

        // Functions that move the cursor (found by scanning the code for the object at 0x520F90 and its +0x18 field),
        // plus the sector draw and walk (they call emitters; game-internals.md 6b).
        constexpr uint32_t kFunctions[] = {
            0x0023E748, 0x00240ED8, 0x002409C8, 0x001CEF18, 0x00266868, 0x002A92C0, 0x002ABF68, 0x002F0360,
            0x002F39D8, 0x002F3DD0, 0x002F53D8, 0x002F5520, 0x002F62B8, 0x002F6A78, 0x002F7090, 0x002F7908,
            0x002F7B30, 0x002F7D58, 0x002F7EB0, 0x002F80A0, 0x002F8250, 0x002F8710, 0x002F9388, 0x002F9AA0,
            0x002F9CE8, 0x002F9FA0, 0x002FB690, 0x002FB3C0, 0x0032CD60, 0x0032D480, 0x0032D7C8, 0x0032D880,
            0x0040C430, 0x0040F6C8,
        };
        constexpr int kCount = static_cast<int>(sizeof(kFunctions) / sizeof(kFunctions[0]));

        struct Fn
        {
            PS2Runtime::RecompiledFunction original = nullptr;
            uint64_t calls = 0, bytes = 0;
        };
        Fn g_fns[kCount];
        int g_last = -1;          // the hooked function entered last this frame
        uint32_t g_lastUsed = 0;
        uint64_t g_frames = 0, g_frameBytes = 0, g_restBytes = 0; // rest: before the first hooked entry of a frame
        bool g_frameOpen = false;
        auto g_lastReport = std::chrono::steady_clock::now();

        uint32_t word(const uint8_t *rdram, uint32_t address)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (address & kRamMask), 4);
            return v;
        }

        // the half's bytes in use, both ends (FUN_00476770)
        uint32_t used(const uint8_t *rdram)
        {
            const uint32_t start = kBuffer + word(rdram, kBuffer + 0x14u) * kHalf + 0x20u;
            const uint32_t up = word(rdram, kBuffer + 0x18u) - (start - 0x10u), down = start + kHalf - word(rdram, kBuffer + 0x1Cu);
            return (up & ~15u) + down;
        }

        uint64_t delta(uint32_t from, uint32_t to)
        {
            return to >= from && to - from <= kHalf ? to - from : 0u;
        }

        void note(const uint8_t *rdram, int index)
        {
            const uint32_t u = used(rdram);
            if (!g_frameOpen)
            {
                g_frameOpen = true;
                g_restBytes += u <= kHalf ? u : 0u;
            }
            else if (g_last >= 0)
                g_fns[g_last].bytes += delta(g_lastUsed, u);
            else
                g_restBytes += delta(g_lastUsed, u);
            if (index >= 0)
                ++g_fns[index].calls;
            g_last = index;
            g_lastUsed = u;
        }

        template <int I>
        PS2_GUEST_CC void hook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            note(rdram, I); // before the original only (DECISIONS.md "Rule for hooks on recompiled functions")
            g_fns[I].original(rdram, ctx, runtime);
        }

        template <int... Is>
        PS2Runtime::RecompiledFunction hookFor(int i, std::integer_sequence<int, Is...>)
        {
            static const PS2Runtime::RecompiledFunction table[] = {&hook<Is>...};
            return table[i];
        }

        void report()
        {
            const double n = std::max<double>(1.0, static_cast<double>(g_frames));
            std::vector<int> order(kCount);
            for (int i = 0; i < kCount; ++i)
                order[i] = i;
            std::sort(order.begin(), order.end(), [](int a, int b) { return g_fns[a].bytes > g_fns[b].bytes; });
            std::fprintf(stderr, "[rsl] %llu frames: %.0f bytes of packets built per frame (%.0f before the first hooked call)\n",
                         static_cast<unsigned long long>(g_frames), g_frameBytes / n, g_restBytes / n);
            for (int i : order)
                if (g_fns[i].calls)
                    std::fprintf(stderr, "[rsl]   %06x %8.1f calls %9.0f bytes per frame\n", kFunctions[i], g_fns[i].calls / n,
                                 g_fns[i].bytes / n);
            for (Fn &f : g_fns)
                f.calls = f.bytes = 0;
            g_frames = g_frameBytes = g_restBytes = 0;
        }
    }

    void install(PS2Runtime &runtime)
    {
        if (!std::getenv("VCS_TRACE_RSL"))
            return;
        for (int i = 0; i < kCount; ++i)
        {
            g_fns[i].original = runtime.lookupFunction(kFunctions[i]);
            if (g_fns[i].original)
                runtime.replaceFunction(kFunctions[i], hookFor(i, std::make_integer_sequence<int, kCount>{}));
        }
    }

    void onFrameKick(const uint8_t *rdram)
    {
        if (!g_fns[0].original && !g_fns[kCount - 1].original)
            return; // not installed
        if (g_frameOpen)
        {
            note(rdram, -1); // the last hooked function's tail
            const uint32_t u = used(rdram);
            g_frameBytes += u <= kHalf ? u : 0u;
            ++g_frames;
        }
        g_frameOpen = false;
        g_last = -1;
        const auto now = std::chrono::steady_clock::now();
        if (now - g_lastReport >= std::chrono::seconds(5))
        {
            g_lastReport = now;
            report();
        }
    }
}
