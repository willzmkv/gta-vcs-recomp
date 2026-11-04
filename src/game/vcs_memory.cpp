// The streaming heap in the port's upper 32 MB (game/vcs_memory.h, docs/game-internals.md 9e).
//
// FUN_002E5688(heap 0x51F420) is the streaming heap's construction, once at boot: FUN_002BD178(0x567000) allocates 5.4 MB
// from the main heap (0x51D6A0), the pointer goes to heap + 0x130, FUN_002BB768(heap, it, 0x567000, 0) builds the heap
// there, and heap + 0x12C = 1. CStreaming allocates every entry from it (FUN_003C6478) and, when it is full, unloads
// what it can and defragments (FUN_0021C300) - so the heap's size is all of the game's streaming budget
// (FUN_003CB760 makes room against its free space alone). The hook does the same with the upper memory: no main heap
// allocation, the two fields, and FUN_002BB768 as a guest tail call (DECISIONS.md, hooks) with the larger size.
//
// The cache cap: CStreaming's request (FUN_003C7ED8) also unloads the least recently used resource
// (FUN_003C9DE8(streaming, 0x83), the call at 0x3C8030) whenever its list of loaded ones (streaming + 0xD8,
// FUN_0047EBF8 counts it) holds 51 (0x33) or more - whatever the memory. With the larger heap that call is skipped,
// so what stays loaded is bounded by memory: FUN_003CB760 still unloads (through FUN_003C9F10, the other caller)
// when an entry does not fit.
//
// VCS_STREAMING_MB=<n> sets the size for one run (0: the game's own).
#include "game/vcs_memory.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_control.h"
#include "settings/settings.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace vcs::memory
{
    namespace
    {
        constexpr uint32_t kBuildStreamingHeap = 0x002E5688u;
        constexpr uint32_t kBuildHeap = 0x002BB768u;
        constexpr uint32_t kRemoveLeastUsed = 0x003C9DE8u;
        constexpr uint32_t kCapCallReturn = 0x003C8038u; // the return address of the 51-cap's call in FUN_003C7ED8
        constexpr uint32_t kStreamingHeap = 0x0051F420u;
        constexpr uint32_t kMainHeap = 0x0051D6A0u;
        constexpr uint32_t kGameSize = 0x00567000u;
        // the upper memory's top 2 MB stay free for the port's own (resident models, T10 step 4)
        constexpr uint32_t kReserve = 2u * 1024u * 1024u;

        PS2Runtime::RecompiledFunction g_original = nullptr;
        PS2Runtime::RecompiledFunction g_originalRemove = nullptr;
        std::atomic<uint64_t> g_capSkips{0};
        std::atomic<int> g_megabytes{0}; // 0: the game's own heap
        std::atomic<uint32_t> g_end{0};
        uint8_t *g_rdram = nullptr;

        uint32_t rd32(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (a & PS2_RAM_MASK), 4);
            return v;
        }
        void wr32(uint8_t *rdram, uint32_t a, uint32_t v) { std::memcpy(rdram + (a & PS2_RAM_MASK), &v, 4); }

        PS2_GUEST_CC void buildStreamingHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_rdram = rdram;
            const int megabytes = g_megabytes.load(std::memory_order_relaxed);
            if (megabytes <= 0)
                return g_original(rdram, ctx, runtime);
            const uint32_t heap = GPR_U32(ctx, 4);
            const uint32_t room = PS2_RAM_SIZE - kExtendedBase - kReserve;
            const uint32_t size = std::min<uint32_t>(static_cast<uint32_t>(megabytes) * 1024u * 1024u, room);
            wr32(rdram, heap + 0x130u, kExtendedBase); // what FUN_002BD178 would have returned
            wr32(rdram, heap + 0x12Cu, 1u);            // the original sets it after the build; the build does not read it
            SET_GPR_U32(ctx, 5, kExtendedBase);
            SET_GPR_U32(ctx, 6, size);
            SET_GPR_U32(ctx, 7, 0u);
            ctx->pc = kBuildHeap; // FUN_002BB768(heap, base, size, 0), returning to our caller
            g_end.store(kExtendedBase + size, std::memory_order_relaxed);
            std::fprintf(stderr, "[memory] streaming heap: %u KB at 0x%08x (the game's: %u KB from its main heap)\n", size / 1024u,
                         kExtendedBase, kGameSize / 1024u);
        }

        // FUN_003C9DE8(streaming, flags): from the 51-cap's call with the larger heap, nothing unloaded (v0 = 0, the call's
        // result is not read there); every other call runs the original
        PS2_GUEST_CC void removeLeastUsed(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_megabytes.load(std::memory_order_relaxed) > 0 && GPR_U32(ctx, 31) == kCapCallReturn)
            {
                g_capSkips.fetch_add(1, std::memory_order_relaxed);
                SET_GPR_U32(ctx, 2, 0u);
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
            g_originalRemove(rdram, ctx, runtime);
        }
    }

    uint32_t extendedUsedEnd() { return g_end.load(std::memory_order_relaxed); }

    int streamingMegabytes() { return g_megabytes.load(std::memory_order_relaxed); }

    void registerSettings()
    {
        using settings::Type;
        settings::Spec s;
        s.key = "memory.streaming";
        s.page = "Game";
        s.section = "Memory";
        s.label = "Streaming memory";
        s.help = "Room for the models and textures the game streams from the disc (cars, people, props, weapons). The PS2 "
                 "has 5.4 MB; more keeps more of them loaded (more different cars in traffic) and leaves room for large mods "
                 "(PC car mods). The city's buildings stream on their own. From the next start.";
        s.type = Type::Choice;
        s.choices = {{"PS2 (5.4 MB)", 0}, {"12 MB", 12}, {"20 MB", 20}, {"30 MB", 30}};
        s.defaultValue = 0;
        s.apply = settings::Apply::Restart;
        s.env = "VCS_STREAMING_MB";
        s.onChange = [](int v) { g_megabytes.store(std::max(0, v), std::memory_order_relaxed); };
        settings::add(s);
    }

    void install(PS2Runtime &runtime)
    {
        g_original = runtime.lookupFunction(kBuildStreamingHeap);
        if (!g_original)
        {
            std::fprintf(stderr, "[memory] FUN_%08X not found: the streaming heap stays the game's\n", kBuildStreamingHeap);
            return;
        }
        runtime.replaceFunction(kBuildStreamingHeap, &buildStreamingHeap);
        if ((g_originalRemove = runtime.lookupFunction(kRemoveLeastUsed)))
            runtime.replaceFunction(kRemoveLeastUsed, &removeLeastUsed);
        // memstate -> ok streaming=<base>,<size>,<in use> main=<size>,<in use> cap_skips=<n> (bytes; heap +0xC first block,
        // +0 size, +0x14 used; n: the 51-cap's unloads skipped)
        ps2control::addCommand("memstate", [](const std::string &, ps2control::Reply reply) {
            if (!g_rdram)
                return reply("error the heaps are not built yet");
            char text[224];
            std::snprintf(text, sizeof(text), "ok streaming=0x%08x,%u,%u main=%u,%u cap_skips=%llu", rd32(g_rdram, kStreamingHeap + 0xCu),
                          rd32(g_rdram, kStreamingHeap), rd32(g_rdram, kStreamingHeap + 0x14u), rd32(g_rdram, kMainHeap),
                          rd32(g_rdram, kMainHeap + 0x14u), static_cast<unsigned long long>(g_capSkips.load()));
            reply(text);
        });
    }
}
