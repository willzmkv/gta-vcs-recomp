// Native RSL packet emitters - see game/vcs_rsl_native.h.
#include "game/vcs_rsl_native.h"

#include "game/vcs_rsl_world.h"
#include "ps2_runtime.h"

#include <chrono>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vcs::rslnative
{
    namespace
    {
        constexpr uint32_t kRamMask = PS2_RAM_MASK;
        constexpr uint32_t kFlush = 0x002F8710u;
        constexpr uint32_t kPacketBuffer = 0x520F90u; // +0x14 half, +0x18 cursor, +0x1C down pointer (game-internals.md 6i)
        constexpr uint32_t kHalfBytes = 0xE6670u;
        constexpr uint32_t kDirty = 0x4882A0u;      // dirty bits
        constexpr uint32_t kGsQueue = 0x729510u;    // queued GS registers: {value u64, register u64}
        constexpr uint32_t kGsQueueCount = 0x488308u;

        bool g_native = false, g_verify = false, g_log = false;
        PS2Runtime::RecompiledFunction g_original = nullptr;

        uint32_t rd32(const uint8_t *rdram, uint32_t addr)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (addr & kRamMask), 4);
            return v;
        }
        uint64_t rd64(const uint8_t *rdram, uint32_t addr)
        {
            uint64_t v;
            std::memcpy(&v, rdram + (addr & kRamMask), 8);
            return v;
        }
        void wr32(uint8_t *rdram, uint32_t addr, uint32_t v) { std::memcpy(rdram + (addr & kRamMask), &v, 4); }
        void wr64(uint8_t *rdram, uint32_t addr, uint64_t v) { std::memcpy(rdram + (addr & kRamMask), &v, 8); }

        // what one flush writes (Flush, vcs_rsl_native.h): packet bytes at the cursor, GS registers queued, the dirty word after
        using Output = Flush;
        void put64(Output &o, uint64_t v)
        {
            std::memcpy(o.bytes + o.size, &v, 8);
            o.size += 8;
        }
        void put32x4(Output &o, uint32_t a)
        {
            const uint32_t w[4] = {a, 0u, 0u, 0u};
            std::memcpy(o.bytes + o.size, w, 16);
            o.size += 16;
        }
        void copy(Output &o, const uint8_t *rdram, uint32_t addr, uint32_t n)
        {
            std::memcpy(o.bytes + o.size, rdram + (addr & kRamMask), n);
            o.size += n;
        }

        uint64_t g_fallbackCfae0 = 0; // flushes the original ran for bit 0x4 (the rest: plug-in callbacks)

        // FUN_002F8710 without its callback branches (game-internals.md 6i); false: the original must run
        bool build(const uint8_t *rdram, Output &out)
        {
            const uint32_t dirty = rd32(rdram, kDirty);
            if (dirty & 0x4u)
            {
                ++g_fallbackCfae0;
                return false; // FUN_001CFAE0
            }
            const uint32_t context = rd32(rdram, 0x4882A4u);
            // step 5f: the +0x50 callbacks on bit 0x10 that are known (game-internals.md 6i "The state flush"), emitted
            // natively below in the original's place: {callback, node +0x34, +0x54}
            uint32_t plugins[16][3];
            uint32_t pluginCount = 0;
            static const bool nativePlugins = !(std::getenv("VCS_RSL_PLUGINS") && *std::getenv("VCS_RSL_PLUGINS") == '0');
            if (dirty & 0x11u)
            {
                // the render context's plug-in list (+0x114, ends at +0xB0, next at +0x64): any callback the flush would
                // call (+0x48 and +0x4C on a context change, +0x50 on bit 0x10) means the original runs - but for the two
                // +0x50 ones VCS has (0x1CEF90 -> FUN_001CEF18, 0x40F798 -> FUN_0040F6C8), written here
                const uint32_t end = context + 0xB0u;
                uint32_t node = rd32(rdram, context + 0x114u);
                for (int n = 0; node != end; ++n, node = rd32(rdram, node + 0x64u))
                {
                    if (n >= 64 || (node & 0xF0000000u) != 0u)
                        return false;
                    const uint32_t cb50 = (dirty & 0x1u) ? 0u : rd32(rdram, node + 0x50u);
                    if (nativePlugins && cb50 && (cb50 == 0x001CEF90u || cb50 == 0x0040F798u) && pluginCount < 16u)
                    {
                        plugins[pluginCount][0] = cb50;
                        plugins[pluginCount][1] = rd32(rdram, node + 0x34u);
                        plugins[pluginCount][2] = rd32(rdram, node + 0x54u);
                        ++pluginCount;
                        continue;
                    }
                    if ((dirty & 0x1u) ? (rd32(rdram, node + 0x48u) | rd32(rdram, node + 0x4Cu)) != 0u : rd32(rdram, node + 0x50u) != 0u)
                    {
                        static const bool log = std::getenv("VCS_RSL_PLUGIN_LOG") != nullptr; // step 5f: which callbacks
                        if (log)
                        {
                            static std::map<uint64_t, uint64_t> seen;
                            static auto at = std::chrono::steady_clock::now();
                            const uint32_t cb = (dirty & 0x1u) ? (rd32(rdram, node + 0x48u) ? rd32(rdram, node + 0x48u) : rd32(rdram, node + 0x4Cu))
                                                               : rd32(rdram, node + 0x50u);
                            ++seen[(uint64_t(cb) << 32) | ((dirty & 0x1u) ? (rd32(rdram, node + 0x48u) ? 0x48u : 0x4Cu) : 0x50u)];
                            if (std::chrono::steady_clock::now() - at > std::chrono::seconds(5))
                            {
                                at = std::chrono::steady_clock::now();
                                for (const auto &[k, n] : seen)
                                    std::fprintf(stderr, "[rsl-plugin] callback 0x%08x at +0x%02x: %llu flushes\n", unsigned(k >> 32),
                                                 unsigned(k & 0xFFu), (unsigned long long)n);
                                seen.clear();
                            }
                        }
                        return false;
                    }
                }
            }
            if (dirty & 0x3u)
                out.cntOnly = false; // CALLs (also from the table entry, the instance matrix's REF: below)
            if (dirty & 0x1u) // CALL the render context's own packet
            {
                put64(out, (uint64_t(context + 0x10u) << 32) | 0x50000000u);
                put64(out, 0x1000000000000000ull);
            }
            if (dirty & 0x2u) // CALL the render context's packet
            {
                put64(out, (uint64_t(rd32(rdram, 0x4882A8u)) << 32) | 0x50000000u);
                put64(out, 0x1000000000000000ull);
            }
            for (uint32_t i = 0; i < pluginCount; ++i) // step 5f: the +0x50 callbacks, in the list's order
            {
                const uint32_t vu = plugins[i][1], src = plugins[i][2];
                if ((src & 0xF0000000u) != 0u)
                    return false; // (not main RAM: the original)
                if (plugins[i][0] == 0x001CEF90u) // FUN_001CEF18(vu, src): CNT 3, FLUSHE, UNPACK V4-32 x3 -> vu
                {
                    put64(out, 0x10000003u);
                    put64(out, 0x10000000ull | (uint64_t(vu | 0x6C030000u) << 32));
                    copy(out, rdram, src, 48u);
                }
                else if (rd32(rdram, src + 0x90u) != 0u) // FUN_0040F6C8(vu, src) when src+0x90 is set: CNT 9, x9; clears it
                {
                    if (out.clearCount >= 16u)
                        return false;
                    put64(out, 0x10000009u);
                    put64(out, 0x10000000ull | (uint64_t(vu | 0x6C090000u) << 32));
                    copy(out, rdram, src, 144u);
                    out.clears[out.clearCount++] = src + 0x90u;
                }
            }
            if ((dirty & 0x11u) && (dirty & 0x600u) != 0x600u) // MSCALs the context change needs (0x75 / 0x4A) unless done below
            {
                put64(out, 0x10000000u);
                put64(out, (uint64_t((dirty & 0x200u) ? 0u : 0x1400004Au) << 32) | ((dirty & 0x400u) ? 0u : 0x14000075u));
            }
            if (dirty & 0x2000u) // three CALLs from the current table entry
            {
                out.cntOnly = false;
                const uint32_t entry = rd32(rdram, 0x742AB0u + rd32(rdram, 0x4CD57Cu) * 4u);
                for (uint32_t k = 0; k < 3u; ++k)
                {
                    put64(out, (uint64_t(rd32(rdram, rd32(rdram, entry + k * 4u))) << 32) | 0x50000000u);
                    put64(out, k == 0u ? 0x10000000u : 0u);
                }
            }
            if (dirty & 0x8u) // UNPACK V4-32 x5 -> 0x31 (0x742AE0)
            {
                put64(out, 0x10000005u);
                put64(out, 0x6C05003110000000ull);
                copy(out, rdram, 0x742AE0u, 0x50u);
            }
            if (dirty & 0x1000u) // viewport -> 0x0C, 0x520AC0 -> 0x41
            {
                put64(out, 0x10000000u);
                put64(out, 0x10000000u);
                put64(out, 0x10000005u);
                put64(out, 0x6C04000C01000101ull);
                copy(out, rdram, 0x520920u, 0x40u);
                put32x4(out, 0x1400008Fu);
                put64(out, 0x10000001u);
                put64(out, 0x6C01004110000000ull);
                copy(out, rdram, 0x520AC0u, 0x10u);
            }
            if (dirty & 0x800u) // 0x5208E0 -> 0x08, lights 0x5209A0 -> 0x3B
            {
                put64(out, 0x10000000u);
                put64(out, 0x10000000u);
                put64(out, 0x10000005u);
                put64(out, 0x6C04000801000101ull);
                copy(out, rdram, 0x5208E0u, 0x40u);
                put32x4(out, 0x1400008Fu);
                put64(out, 0x10000006u);
                put64(out, 0x6C06003B00000000ull);
                copy(out, rdram, 0x5209A0u, 0x60u);
            }
            if (dirty & 0x400u) // 0x520990 -> 0x20, 0x5208A0 -> 0x04
            {
                put64(out, 0x10000001u);
                put64(out, 0x6C01002010000000ull);
                copy(out, rdram, 0x520990u, 0x10u);
                put64(out, 0x10000000u);
                put64(out, 0x10000000u);
                put64(out, 0x10000005u);
                put64(out, 0x6C04000401000101ull);
                copy(out, rdram, 0x5208A0u, 0x40u);
                put32x4(out, 0x14000083u);
            }
            if ((dirty & 0x200u) && rd32(rdram, 0x4882B0u) == 0u) // the instance matrix 0x520860 -> 0x00 (or a REF to it)
            {
                const uint32_t ref = rd32(rdram, 0x4882B8u);
                if (ref == 0u)
                {
                    put64(out, 0x10000005u);
                    put64(out, 0x6C04800001000101ull);
                    copy(out, rdram, 0x520860u, 0x40u);
                    put32x4(out, 0x1400003Cu);
                }
                else
                {
                    out.cntOnly = false;
                    put64(out, (uint64_t(ref) << 32) | 0x30000004u);
                    put64(out, 0x6C04800000000000ull);
                    put64(out, 0x10000000u);
                    put64(out, 0x1400003Cu);
                }
            }
            // GS registers: FOG (0x80), FOGCOL (0x100), TEST (0x20), ... (0x40)
            const struct
            {
                uint32_t bit, value, reg;
            } regs[4] = {{0x80u, 0x4882E0u, 6u}, {0x100u, 0x4882E8u, 7u}, {0x20u, 0x4882F0u, 0x14u}, {0x40u, 0x4882F8u, 0x15u}};
            for (const auto &g : regs)
                if (dirty & g.bit)
                {
                    out.queue[out.queued][0] = rd64(rdram, g.value);
                    out.queue[out.queued][1] = g.reg;
                    ++out.queued;
                }
            out.dirtyAfter = dirty & 0x80000000u;
            return true;
        }

        // the cursor, when the packet buffer has room for `bytes` (with a margin) and the cursor is in main RAM
        bool cursorWithRoom(const uint8_t *rdram, uint32_t bytes, uint32_t &cursor)
        {
            cursor = rd32(rdram, kPacketBuffer + 0x18u);
            if ((cursor & 0xF0000000u) != 0u || (cursor & 0xFu) != 0u)
                return false;
            // FUN_00476770: bytes used in the current half
            const int32_t base = static_cast<int32_t>(kPacketBuffer + rd32(rdram, kPacketBuffer + 0x14u) * kHalfBytes + 0x20u);
            const int32_t used = ((static_cast<int32_t>(cursor) - (base - 0x10)) >> 4) * 0x10 +
                                 ((base + static_cast<int32_t>(kHalfBytes)) - static_cast<int32_t>(rd32(rdram, kPacketBuffer + 0x1Cu)));
            const int32_t space = static_cast<int32_t>(kHalfBytes) - used;
            return space >= static_cast<int32_t>(bytes + 0x400u);
        }

        void commitState(uint8_t *rdram, const Output &out);
        void commit(uint8_t *rdram, uint32_t cursor, const Output &out)
        {
            std::memcpy(rdram + (cursor & kRamMask), out.bytes, out.size);
            wr32(rdram, kPacketBuffer + 0x18u, cursor + out.size);
            commitState(rdram, out);
        }

        // the queued GS registers and the dirty word (the packet bytes went elsewhere: a state op, vcs_rsl_world.h)
        void commitState(uint8_t *rdram, const Output &out)
        {
            uint32_t count = rd32(rdram, kGsQueueCount);
            for (uint32_t i = 0; i < out.queued; ++i, ++count)
            {
                wr64(rdram, kGsQueue + count * 16u, out.queue[i][0]);
                wr64(rdram, kGsQueue + count * 16u + 8u, out.queue[i][1]);
            }
            wr32(rdram, kGsQueueCount, count);
            wr32(rdram, kDirty, out.dirtyAfter);
            for (uint32_t i = 0; i < out.clearCount; ++i) // step 5f: what the callbacks write besides their packets
                wr32(rdram, out.clears[i], 0u);
        }

        struct Stats
        {
            uint64_t native = 0, fallback = 0, noRoom = 0, verifySame = 0, verifyDiffer = 0, queueMovedOn = 0;
        } g_stats;
        auto g_reportAt = std::chrono::steady_clock::now();

        // verify: the last flush's native bytes, compared with what the original left once it has run
        struct Pending
        {
            bool armed = false;
            uint32_t cursor = 0, queueAt = 0;
            Output out;
        } g_pending;

        void checkPending(const uint8_t *rdram)
        {
            if (!g_pending.armed)
                return;
            g_pending.armed = false;
            const Output &o = g_pending.out;
            const char *what = nullptr;
            if (std::memcmp(rdram + (g_pending.cursor & kRamMask), o.bytes, o.size) != 0)
                what = "packet bytes";
            // the queue is checked while its count is still the flush's (the draw consumes it, later setters refill it)
            // - or where the draw wrote it: right after the flush's bytes, CNT (n + 1) + DIRECTHL (n + 1), a GIFtag, then the
            // n entries (FUN_002F7D58 / FUN_002F80A0 / FUN_002F7EB0, game-internals.md 6i)
            const uint32_t total = g_pending.queueAt + o.queued, after = g_pending.cursor + o.size;
            // (the packet first: a count equal to the flush's may be a refilled queue)
            const bool inPacket = rd32(rdram, after) == ((total + 1u) | 0x10000000u) && rd32(rdram, after + 12u) == ((total + 1u) | 0x51000000u);
            const bool inQueue = !inPacket && rd32(rdram, kGsQueueCount) == total;
            if (o.queued && !inQueue && !inPacket)
                ++g_stats.queueMovedOn;
            for (uint32_t i = 0; !what && (inQueue || inPacket) && i < o.queued; ++i)
            {
                const uint32_t at = inQueue ? kGsQueue + (g_pending.queueAt + i) * 16u : after + 32u + (g_pending.queueAt + i) * 16u;
                if (rd64(rdram, at) != o.queue[i][0] || rd64(rdram, at + 8u) != o.queue[i][1])
                    what = "GS register queue";
            }
            if (!what)
            {
                ++g_stats.verifySame;
                return;
            }
            ++g_stats.verifyDiffer;
            static int logged = 0;
            if (logged++ < 10)
            {
                std::fprintf(stderr, "[rsl-native] verify: flush differs in %s (%u bytes at 0x%08x; queue %u + %u, %s)", what, o.size,
                             g_pending.cursor, g_pending.queueAt, o.queued, inQueue ? "in the queue" : "in the packet");
                for (uint32_t i = 0; std::strcmp(what, "GS register queue") == 0 && i < o.queued; ++i)
                {
                    const uint32_t at = inQueue ? kGsQueue + (g_pending.queueAt + i) * 16u : after + 32u + (g_pending.queueAt + i) * 16u;
                    std::fprintf(stderr, " [native %llx = %016llx, original %llx = %016llx]", (unsigned long long)o.queue[i][1],
                                 (unsigned long long)o.queue[i][0], (unsigned long long)rd64(rdram, at + 8u),
                                 (unsigned long long)rd64(rdram, at));
                }
                for (uint32_t i = 0; i < o.size; i += 8u)
                {
                    const uint64_t want = *reinterpret_cast<const uint64_t *>(o.bytes + i), got = rd64(rdram, g_pending.cursor + i);
                    if (want != got)
                    {
                        std::fprintf(stderr, ": first at +0x%x native %016llx original %016llx", i, (unsigned long long)want,
                                     (unsigned long long)got);
                        break;
                    }
                }
                std::fprintf(stderr, "\n");
            }
        }

        void report()
        {
            const auto now = std::chrono::steady_clock::now();
            if (!g_log || now - g_reportAt < std::chrono::seconds(10))
                return;
            g_reportAt = now;
            std::fprintf(stderr, "[rsl-native] flush: %llu native, %llu original (%llu of them FUN_001CFAE0, the rest plug-in callbacks), %llu original (no room)\n",
                         (unsigned long long)g_stats.native, (unsigned long long)g_stats.fallback, (unsigned long long)g_fallbackCfae0,
                         (unsigned long long)g_stats.noRoom);
            g_fallbackCfae0 = 0;
            if (g_verify)
                std::fprintf(stderr, "[rsl-native] verify: %llu flushes the same as the original, %llu differ (GS queue not checked: %llu, consumed first)\n",
                             (unsigned long long)g_stats.verifySame, (unsigned long long)g_stats.verifyDiffer,
                             (unsigned long long)g_stats.queueMovedOn);
            g_stats = Stats{};
        }

        PS2_GUEST_CC void flushHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            vcs::rslworld::settlePending(rdram, getRegU32(ctx, 29), getRegU32(ctx, 31), ctx->pc); // steps 5c / 5d: a captured region before this flush's
            checkPending(rdram);
            report();
            thread_local Output out;
            out.size = 0;
            out.queued = 0;
            out.cntOnly = true;
            out.clearCount = 0;
            uint32_t cursor = 0;
            if (!build(rdram, out))
            {
                ++g_stats.fallback;
                g_original(rdram, ctx, runtime);
                return;
            }
            if (!cursorWithRoom(rdram, out.size, cursor))
            {
                ++g_stats.noRoom;
                g_original(rdram, ctx, runtime);
                return;
            }
            if (g_verify)
            {
                // the original writes; its bytes are compared with these before the next flush (code after the original
                // returns is not relied on: DECISIONS.md "Rule for hooks on recompiled functions")
                g_pending.armed = true;
                g_pending.cursor = cursor;
                g_pending.queueAt = rd32(rdram, kGsQueueCount);
                g_pending.out = out;
                ++g_stats.native;
                g_original(rdram, ctx, runtime);
                return;
            }
            if (out.cntOnly && vcs::rslworld::flushAsOp(rdram, out.bytes, out.size))
                commitState(rdram, out); // step 5a: its bytes as a state op (docs/FULLY_NATIVE.md)
            else
                commit(rdram, cursor, out);
            ++g_stats.native;
            ctx->pc = getRegU32(ctx, 31);
        }
    }

    void install(PS2Runtime &runtime)
    {
        const char *e = std::getenv("VCS_RSL_NATIVE");
        if (e && *e == '0')
            return;
        g_original = runtime.lookupFunction(kFlush);
        if (!g_original)
            return;
        g_native = true;
        g_verify = std::getenv("VCS_RSL_NATIVE_VERIFY") != nullptr;
        g_log = g_verify || std::getenv("VCS_RSL_NATIVE_LOG") != nullptr;
        runtime.replaceFunction(kFlush, &flushHook);
    }

    void onFrameKick(const uint8_t *rdram)
    {
        if (g_native && g_verify)
            checkPending(rdram);
    }

    bool enabled() { return g_native && !g_verify; }

    bool buildFlush(const uint8_t *rdram, Flush &out)
    {
        out.size = 0;
        out.queued = 0;
        out.cntOnly = true;
        out.clearCount = 0;
        return build(rdram, out);
    }

    bool packetRoom(const uint8_t *rdram, uint32_t bytes, uint32_t &cursor) { return cursorWithRoom(rdram, bytes, cursor); }

    void applyClears(uint8_t *rdram, const Flush &f)
    {
        for (uint32_t i = 0; i < f.clearCount; ++i)
            wr32(rdram, f.clears[i], 0u);
    }
}
