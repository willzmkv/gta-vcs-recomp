// Game-specific runtime bindings for SLUS_215.90, applied when the ELF loads.
//
// Use this (not game/vcs/overrides.toml) for quick experiments: changing a stub
// in the recompiler config rewrites the generated headers and rebuilds every
// unity batch (~9 min), while this file only needs a relink.
// Once a binding is settled it can move to overrides.toml.
#include "game_overrides.h"
#include "debug/game_info.h"
#include "debug/profiler.h"
#include "debug/rig.h"
#include "game/vcs_trainer.h"
#include "debug/rsl_shadow.h"
#include "debug/rsl_trace.h"
#include "game/vcs_display.h"
#include "game/vcs_framerate.h"
#include "game/vcs_movie.h"
#include "game/vcs_input.h"
#include "game/vcs_volume.h"
#include "game/vcs_script_timing.h"
#include "game/vcs_physics_timing.h"
#include "game/vcs_inflate.h"
#include "game/vcs_sound.h"
#include "game/vcs_stream.h"
#include "game/vcs_aim.h"
#include "game/vcs_freecam.h"
#include "game/vcs_hud.h"
#include "game/vcs_rsl_native.h"
#include "game/vcs_rsl_world.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_perf.h"
#include <ps2_recompiled_functions.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <map>
#include <mutex>
#include <tuple>
#include <iostream>

namespace
{
    uint32_t g_lastInflateDst = 0;

    struct Binding
    {
        uint32_t address;
        const char *handler; // runtime syscall/stub name
        const char *why;
    };

    // Functions replaced by a runtime handler.
    constexpr Binding kBindings[] = {
        {0, nullptr, nullptr},
    };

    // Functions that just return 0 (skipped).
    struct Skip
    {
        uint32_t address;
        const char *why;
    };
    constexpr Skip kSkips[] = {
        // (the movie player, CMoviePlayer::Play 0x40D8B8, was skipped here until native playback: game/vcs_movie.h)
        {0, nullptr},
    };

    // ---- VCS_TRACE_FILES=1: log the game's own file API (CFileMgr-like, 0x220xxx) ----
    const char *guestString(uint8_t *rdram, uint32_t address)
    {
        return reinterpret_cast<const char *>(rdram + (address & PS2_RAM_MASK));
    }

    PS2_GUEST_CC void traceOpen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t nameAddr = GPR_U32(ctx, 4);
        if (std::strstr(guestString(rdram, nameAddr), "LVZ"))
        {
            // The disc index built at boot by FUN_002209F0: count at 0x486E8C, 16-byte entries at 0x5038A8
            uint32_t count = 0;
            std::memcpy(&count, rdram + 0x00486E8Cu, 4);
            std::fprintf(stderr, "[file] index has %u entries; LVZ/IMG ones:\n", count);
            for (uint32_t i = 0; i < count && i < 4096u; ++i)
            {
                uint32_t e[4];
                std::memcpy(e, rdram + 0x005038A8u + i * 16u, sizeof(e));
                const char *n = guestString(rdram, e[0]);
                if (std::strstr(n, "LVZ") || std::strstr(n, "IMG"))
                    std::fprintf(stderr, "[file]   #%u \"%s\" lsn=0x%x size=0x%x\n", i, n, e[1], e[2]);
            }
        }
        const uint32_t ra = GPR_U32(ctx, 31);
        FUN_00220c68_0x220c68(rdram, ctx, runtime);
        std::fprintf(stderr, "[file] open \"%s\" -> handle %d (ra=0x%08x)\n", guestString(rdram, nameAddr),
                     static_cast<int32_t>(GPR_U32(ctx, 2)), ra);
    }

    PS2_GUEST_CC void traceRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::fprintf(stderr, "[file] read handle %d buf=0x%08x size=0x%x (ra=0x%08x)\n",
                     static_cast<int32_t>(GPR_U32(ctx, 4)), GPR_U32(ctx, 5), GPR_U32(ctx, 6), GPR_U32(ctx, 31));
        const uint32_t buf = GPR_U32(ctx, 5) & PS2_RAM_MASK;
        const uint32_t size = GPR_U32(ctx, 6);
        FUN_00220778_0x220778(rdram, ctx, runtime);
        uint64_t hash = 1469598103934665603ull;
        for (uint32_t i = 0; i < size && buf + i < PS2_RAM_SIZE; ++i)
            hash = (hash ^ rdram[buf + i]) * 1099511628211ull;
        std::fprintf(stderr, "[file]   -> %d, fnv64=%016llx first=%02x%02x%02x%02x%02x%02x%02x%02x\n",
                     static_cast<int32_t>(GPR_U32(ctx, 2)), static_cast<unsigned long long>(hash),
                     rdram[buf], rdram[buf + 1], rdram[buf + 2], rdram[buf + 3], rdram[buf + 4], rdram[buf + 5],
                     rdram[buf + 6], rdram[buf + 7]);
    }

    PS2_GUEST_CC void traceInflate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // FUN_0021FA38(dst, src, dstCapacity, ?) - zlib inflate of e.g. GAME.DTZ
        const uint32_t src = GPR_U32(ctx, 5) & PS2_RAM_MASK;
        uint64_t hash = 1469598103934665603ull;
        for (uint32_t i = 0; i < 0x217E70u && src + i < PS2_RAM_SIZE; ++i)
            hash = (hash ^ rdram[src + i]) * 1099511628211ull;
        std::fprintf(stderr, "[file] inflate dst=0x%08x src=0x%08x cap=0x%x first=%02x%02x fnv64(0x217e70)=%016llx\n",
                     GPR_U32(ctx, 4), GPR_U32(ctx, 5), GPR_U32(ctx, 6), rdram[src], rdram[src + 1],
                     static_cast<unsigned long long>(hash));
        g_lastInflateDst = GPR_U32(ctx, 4);
        FUN_0021fa38_0x21fa38(rdram, ctx, runtime);
    }

    PS2_GUEST_CC void traceRelocate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // FUN_00324A50(header{'GTAG',1,..}, image, flag): relocates a loaded GTAG memory image.
        const uint32_t image = GPR_U32(ctx, 5) & PS2_RAM_MASK;
        uint64_t hash = 1469598103934665603ull;
        for (uint32_t i = 0; i < 0x6495D0u && image + i < PS2_RAM_SIZE; ++i)
            hash = (hash ^ rdram[image + i]) * 1099511628211ull;
        std::fprintf(stderr, "[file] relocate image=0x%08x fnv64(0x6495d0)=%016llx\n", image,
                     static_cast<unsigned long long>(hash));
        if (std::memcmp(rdram + image, "DLRW", 4) == 0) // WRLD level image (from .LVZ): dump before relocation
        {
            if (FILE *f = std::fopen("wrld_image.bin", "wb"))
            {
                std::fwrite(rdram + image, 1, 0x00600000u, f);
                std::fclose(f);
            }
        }
        FUN_00324a50_0x324a50(rdram, ctx, runtime);
    }

    PS2_GUEST_CC void traceInflateEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // inflateEnd(z_stream *strm): strm->next_out (+0x0C) / total_out (+0x14)
        const uint32_t strm = GPR_U32(ctx, 4) & PS2_RAM_MASK;
        uint32_t totalOut = 0;
        std::memcpy(&totalOut, rdram + strm + 0x14, 4);
        const uint32_t dst = g_lastInflateDst & PS2_RAM_MASK;
        uint64_t hash = 1469598103934665603ull;
        for (uint32_t i = 0; i < totalOut && dst + i < PS2_RAM_SIZE; ++i)
            hash = (hash ^ rdram[dst + i]) * 1099511628211ull;
        std::fprintf(stderr, "[file] inflateEnd total_out=0x%x fnv64=%016llx first=%02x%02x%02x%02x\n", totalOut,
                     static_cast<unsigned long long>(hash), rdram[dst], rdram[dst + 1], rdram[dst + 2], rdram[dst + 3]);
        uint32_t f[8];
        std::memcpy(f, rdram + strm, sizeof(f));
        const char *msg = f[6] ? reinterpret_cast<const char *>(rdram + (f[6] & PS2_RAM_MASK)) : "(null)";
        uint32_t state = f[7] & PS2_RAM_MASK, mode = 0;
        if (state)
            std::memcpy(&mode, rdram + state, 4);
        std::fprintf(stderr, "[file]   next_in=0x%08x avail_in=0x%x total_in=0x%x next_out=0x%08x avail_out=0x%x msg=\"%s\" state.mode=%u\n",
                     f[0], f[1], f[2], f[3], f[4], msg, mode);
        FUN_00265208_0x265208(rdram, ctx, runtime);
    }

    PS2_GUEST_CC void returnZero(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        SET_GPR_U64(ctx, 2, 0);
        ctx->pc = GPR_U32(ctx, 31);
    }

    // abort() (0x4604F0 -> exit(1)): the game hit a fatal check. Log a heuristic guest
    // backtrace: stack words that point just past a jal/jalr in .text.
    PS2_GUEST_CC void traceAbort(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::fprintf(stderr, "[vcs] abort()/_Exit pc=0x%08x called from ra=0x%08x sp=0x%08x a0=0x%08x\n[vcs] backtrace:",
                     ctx->pc, GPR_U32(ctx, 31), GPR_U32(ctx, 29), GPR_U32(ctx, 4));
        const uint32_t sp = GPR_U32(ctx, 29) & PS2_RAM_MASK;
        int shown = 0;
        for (uint32_t a = sp; a < sp + 0x2000u && a + 4u <= PS2_RAM_SIZE && shown < 24; a += 4u)
        {
            uint32_t v = 0;
            std::memcpy(&v, rdram + a, 4);
            if (v < 0x00100008u || v >= 0x00480000u || (v & 3u))
                continue;
            uint32_t call = 0;
            std::memcpy(&call, rdram + v - 8u, 4);
            if ((call >> 26) == 3u || (call & 0xFC1F07FFu) == 0x00000009u)
            {
                std::fprintf(stderr, " 0x%08x", v - 8u);
                ++shown;
            }
        }
        std::fprintf(stderr, "\n");
        if (ctx->pc == 0x004672C0u)
            ps2__Exit_0x4672c0(rdram, ctx, runtime);
        else
            FUN_004604f0_0x4604f0(rdram, ctx, runtime);
    }

    // VCS_TRACE_DELAY=1: DelayThread(usec) (0x468318, SetTimerAlarm + WaitSema) with host time between calls.
    PS2_GUEST_CC void traceDelay(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static auto last = std::chrono::steady_clock::now();
        static int count = 0;
        const auto now = std::chrono::steady_clock::now();
        if (count++ < 100000)
            std::fprintf(stderr, "[delay] DelayThread(%u us) ra=0x%08x  %.2f ms since previous call\n", GPR_U32(ctx, 4),
                         GPR_U32(ctx, 31), std::chrono::duration<double, std::milli>(now - last).count());
        last = now;
        FUN_00468318_0x468318(rdram, ctx, runtime);
    }

    // VCS_TRACE_DELAY=1 also: FUN_002FA908(_, mode) - the music manager's mode change (FUN_001891A0 on 0x4E41E0; mode 2
    // stops the stream and waits for it in DelayThread(1000) steps) - with who asked
    PS2_GUEST_CC void traceMusicMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::fprintf(stderr, "[delay] music mode %u ra=0x%08x\n", GPR_U32(ctx, 5) & 0xFFu, GPR_U32(ctx, 31));
        FUN_002fa908_0x2fa908(rdram, ctx, runtime);
    }

    // VCS_TRACE_SOUND=1: the EE sound driver's RPC commands (FUN_0010E970(cmd, arg) -> client 0x4D3D40, sid 0x65686579)
    PS2_GUEST_CC void traceSoundCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static int count = 0;
        const uint32_t cmd = GPR_U32(ctx, 4);
        if ((cmd & 0xFF0000u) != 0x0A0000u && count++ < 300)
        {
            uint32_t server = 0, reply = 0;
            std::memcpy(&server, rdram + 0x004D3D40u + 0x24u, 4);
            std::memcpy(&reply, rdram + 0x004CE300u, 4);
            std::fprintf(stderr, "[sound] cmd=0x%06x arg=0x%x ra=0x%08x client.server=0x%08x lastReply=0x%x\n", GPR_U32(ctx, 4),
                         GPR_U32(ctx, 5), GPR_U32(ctx, 31), server, reply);
        }
        FUN_0010e970_0x10e970(rdram, ctx, runtime);
    }

    PS2_GUEST_CC void traceSoundInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t flag = 0;
        std::memcpy(&flag, rdram + 0x00481900u, 4);
        std::fprintf(stderr, "[sound] init (0x10D868) skipFlag@0x481900=0x%x ra=0x%08x\n", flag, GPR_U32(ctx, 31));
        FUN_0010d868_0x10d868(rdram, ctx, runtime);
    }

    // Level loader FUN_0023BAF0(level, ...): reads <level>.LVZ, stores the WRLD image at level+0x1C
    PS2_GUEST_CC void traceLevelLoad(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::fprintf(stderr, "[level] load level=0x%08x a1=0x%08x a2=0x%08x ra=0x%08x\n", GPR_U32(ctx, 4), GPR_U32(ctx, 5),
                     GPR_U32(ctx, 6), GPR_U32(ctx, 31));
        FUN_0023baf0_0x23baf0(rdram, ctx, runtime);
    }

    // VCS_COUNT_CALLS=addr,addr,...: count calls of up to 32 functions, print every 5 s.
    struct CountedFn
    {
        uint32_t address = 0;
        PS2Runtime::RecompiledFunction original = nullptr;
        uint64_t calls = 0;
    };
    CountedFn g_counted[32];
    uint32_t g_countedN = 0;

    template <int I>
    PS2_GUEST_CC void countCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ++g_counted[I].calls;
        static auto last = std::chrono::steady_clock::now();
        if (I == 0)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now - last > std::chrono::seconds(5))
            {
                last = now;
                std::fprintf(stderr, "[calls]");
                for (uint32_t i = 0; i < g_countedN; ++i)
                {
                    std::fprintf(stderr, " %06x:%llu", g_counted[i].address, static_cast<unsigned long long>(g_counted[i].calls));
                    g_counted[i].calls = 0;
                }
                std::fprintf(stderr, "\n");
            }
        }
        g_counted[I].original(rdram, ctx, runtime);
    }

    template <int... Is>
    constexpr PS2Runtime::RecompiledFunction counterFor(int i, std::integer_sequence<int, Is...>)
    {
        PS2Runtime::RecompiledFunction table[] = {&countCall<Is>...};
        return table[i];
    }

    void installCallCounters(PS2Runtime &runtime)
    {
        const char *env = std::getenv("VCS_COUNT_CALLS");
        if (!env)
            return;
        for (const char *p = env; *p && g_countedN < 32u;)
        {
            char *end = nullptr;
            const unsigned long a = std::strtoul(p, &end, 16);
            if (end == p)
                break;
            CountedFn &c = g_counted[g_countedN];
            c.address = static_cast<uint32_t>(a);
            c.original = runtime.lookupFunction(c.address);
            if (c.original)
                runtime.replaceFunction(c.address, counterFor(static_cast<int>(g_countedN), std::make_integer_sequence<int, 32>{}));
            ++g_countedN;
            p = *end ? end + 1 : end;
        }
    }

    // VCS_TRACE_DMASEND=1: sceDmaSend(channel, tag) callers (ra) with counts, every 5 s.
    // FUN_003F9E50: a 4x4 float matrix at $a0 set to identity - 16 words zeroed in a loop, then 1.0 on the diagonal
    // (game-internals.md 2). ~1% of the guest time driving, most of it the loop's per-iteration checkpoint. Natively,
    // with the registers left as the guest code leaves them ($v1 = -1, $v0 = $a0 + 64, $at / $f0 = 1.0); a matrix not
    // in main RAM (the scratchpad) runs the original. VCS_NATIVE_IDENTITY=0: the original always.
    PS2Runtime::RecompiledFunction g_origIdentity = nullptr;
    PS2_GUEST_CC void identityMatrix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t m = GPR_U32(ctx, 4);
        if (m >= 0x01FFFFC0u || (m & 3u))
        {
            g_origIdentity(rdram, ctx, runtime);
            return;
        }
        static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        std::memcpy(rdram + m, kIdentity, sizeof(kIdentity));
        SET_GPR_S32(ctx, 3, -1);
        SET_GPR_S32(ctx, 2, static_cast<int32_t>(m + 64u));
        SET_GPR_S32(ctx, 1, 0x3F800000);
        ctx->f[0] = 1.0f;
        ctx->pc = GPR_U32(ctx, 31);
    }

    // FUN_00255188(id): the index of `id` in MAIN.SCM's u16 model-id table (game-internals.md 11a; table at *0x487420,
    // count at 0x4CD3CC), -1 when absent - a linear scan in guest code. FUN_0025A348 calls it once per entry of a list
    // every 30 s: a few million iterations, the frame held back by the EE clock's charge for them (PERFORMANCE.md 14).
    // Natively: the same first match, $v0 / $v1 / $a1 / $a2 as the loop leaves them ($a0 compared as the whole 64-bit
    // register, as BNEL does). A table outside main RAM runs the original. VCS_NATIVE_MODEL_INDEX=0: the original.
    PS2Runtime::RecompiledFunction g_origModelIndex = nullptr;
    PS2_GUEST_CC void modelIndex(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        int32_t count;
        std::memcpy(&count, rdram + 0x4CD3CCu, 4);
        if (count <= 0)
        {
            SET_GPR_S32(ctx, 5, count);
            SET_GPR_U64(ctx, 3, 0u);
            SET_GPR_S32(ctx, 2, -1);
            ctx->pc = GPR_U32(ctx, 31);
            return;
        }
        uint32_t table;
        std::memcpy(&table, rdram + 0x487420u, 4);
        const uint32_t at = table & 0x1FFFFFFFu;
        if (at >= PS2_RAM_SIZE || static_cast<uint64_t>(count) * 2u > PS2_RAM_SIZE - at)
        {
            g_origModelIndex(rdram, ctx, runtime);
            return;
        }
        const uint64_t want = GPR_U64(ctx, 4);
        const uint8_t *p = rdram + at;
        int32_t i = 0;
        if (want <= 0xFFFFu)
            for (const uint16_t id = static_cast<uint16_t>(want); i < count; ++i)
            {
                uint16_t v;
                std::memcpy(&v, p + i * 2u, 2);
                if (v == id)
                    break;
            }
        else
            i = count; // (a halfword never equals a value above 0xFFFF)
        SET_GPR_S32(ctx, 6, count);
        SET_GPR_S32(ctx, 5, static_cast<int32_t>(table + static_cast<uint32_t>(i) * 2u)); // (addiu: 32 bits, sign-extended)
        SET_GPR_S32(ctx, 3, i);
        SET_GPR_S32(ctx, 2, i < count ? i : -1);
        ctx->pc = GPR_U32(ctx, 31);
    }

    // FUN_00254B90(script, &ip, n, out): the script VM's CollectParameters (game-internals.md 11c) - n typed arguments
    // from ScriptSpace at *ip into out[] (u32 each), *ip moved past them; a type-0 byte (the end of a variable list)
    // returns the count left - 1, else 0. Variables through FUN_00254FF8's address rules. Called for nearly every
    // script command (1,100 call sites): ~1.4% of the game thread in guest code (PERFORMANCE.md 23). Natively the same
    // reads and writes, $v0 the same; no guest call, no checkpoint. ScriptSpace or a pointer outside main RAM runs the
    // original. VCS_NATIVE_PARAMS=0: the original; VCS_NATIVE_PARAMS_VERIFY=1: the original runs, the native result is
    // computed first and compared with what the original left at the next call.
    PS2Runtime::RecompiledFunction g_origCollectParams = nullptr;
    constexpr uint32_t kScriptSpacePtr = 0x0048741Cu, kArrayDummy = 0x0050C140u;

    struct ParamsResult
    {
        bool valid = false;
        uint32_t ipPtr = 0, outPtr = 0, n = 0, ip = 0, startIp = 0;
        int32_t ret = 0;
        uint32_t out[48]; // (not cleared: n of them are written)
        uint8_t types[48];
    };

    // the work; false when something is outside main RAM (the original then runs)
    bool collectParams(const uint8_t *ram, uint32_t script, uint32_t ipPtr, int64_t n64, uint32_t outPtr, ParamsResult &r)
    {
        constexpr uint32_t kRam = PS2_RAM_SIZE;
        uint32_t space;
        std::memcpy(&space, ram + kScriptSpacePtr, 4);
        space &= 0x1FFFFFFFu;
        script &= 0x1FFFFFFFu;
        ipPtr &= 0x1FFFFFFFu;
        outPtr &= 0x1FFFFFFFu;
        if (space >= kRam || script >= kRam - 0x218u || ipPtr >= kRam - 4u || outPtr >= kRam)
            return false;
        uint32_t ip;
        std::memcpy(&ip, ram + ipPtr, 4);
        r.startIp = ip;
        // the arguments are read from ScriptSpace + ip on: 48 arguments of at most 5 bytes fit in 256, a string is checked
        // byte by byte
        if (ip >= kRam || space + ip > kRam - 256u)
            return false;
        const uint8_t *code = ram + space;
        auto byteAt = [&](uint32_t off, uint32_t &v) {
            if (space + off >= kRam)
                return false;
            v = code[off];
            return true;
        };
        auto bytes = [&](uint32_t off, uint32_t count, uint32_t &v) { // little-endian, as lwl / lwr / lbu pairs read it
            if (off - r.startIp > 250u) // (outside the window checked above)
                return false;
            v = 0;
            std::memcpy(&v, code + off, count);
            return true;
        };
        auto local = [&](uint32_t index, uint32_t &v) {
            const uint64_t a = uint64_t(script) + 0x54u + uint64_t(index) * 4u;
            if (a + 4u > kRam)
                return false;
            std::memcpy(&v, ram + a, 4);
            return true;
        };
        r.n = static_cast<uint32_t>(n64);
        r.ret = 0;
        int32_t left = static_cast<int32_t>(n64) - 1;
        uint32_t written = 0;
        if (n64 != 0)
            do
            {
                if (written >= 48u || outPtr + written * 4u + 4u > kRam)
                    return false;
                const uint32_t at = ip;
                uint32_t type;
                if (!byteAt(ip, type))
                    return false;
                ip += 1u;
                uint32_t v = 0;
                switch (type)
                {
                case 0:
                    r.ret = left;
                    r.ip = ip;
                    r.valid = true;
                    r.outPtr = outPtr;
                    r.ipPtr = ipPtr;
                    r.n = written; // the words written
                    return true;
                case 1:
                case 2:
                    v = 0;
                    break;
                case 3:
                    if (!bytes(ip, 1, v))
                        return false;
                    v <<= 24;
                    ip += 1u;
                    break;
                case 4:
                    if (!bytes(ip, 2, v))
                        return false;
                    v <<= 16;
                    ip += 2u;
                    break;
                case 5:
                {
                    uint32_t lo, hi;
                    if (!bytes(ip, 1, lo) || !bytes(ip + 1u, 2, hi))
                        return false;
                    v = (lo << 8) | (hi << 16);
                    ip += 3u;
                    break;
                }
                case 6:
                case 9:
                    if (!bytes(ip, 4, v))
                        return false;
                    ip += 4u;
                    break;
                case 7:
                    if (!bytes(ip, 1, v))
                        return false;
                    v = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(v)));
                    ip += 1u;
                    break;
                case 8:
                    if (!bytes(ip, 2, v))
                        return false;
                    v = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(v)));
                    ip += 2u;
                    break;
                case 10: // a string: its IP, then past its NUL
                {
                    v = ip;
                    uint32_t len = 0, c;
                    for (;;)
                    {
                        if (!byteAt(ip + len, c))
                            return false;
                        if (!c)
                            break;
                        ++len;
                    }
                    ip += 1u + len;
                    break;
                }
                default: // a variable (FUN_00254FF8 from the type byte)
                {
                    ip = at + 1u;
                    uint32_t addr;
                    uint32_t frame;
                    std::memcpy(&frame, ram + script + 0x1FCu, 4);
                    if (type > 0xCCu)
                    {
                        uint32_t lo;
                        if (!byteAt(ip, lo))
                            return false;
                        ip += 1u;
                        int32_t index;
                        if (type < 0xE6u)
                            index = static_cast<int32_t>((type - 0xCDu) * 0x100u);
                        else
                        {
                            uint32_t idxLocal, size, idx;
                            if (!byteAt(ip, idxLocal) || !byteAt(ip + 1u, size))
                                return false;
                            ip += 2u;
                            if (!local(idxLocal + frame, idx))
                                return false;
                            if (static_cast<int32_t>(idx) < 0 || static_cast<int32_t>(size) <= static_cast<int32_t>(idx))
                            {
                                addr = kArrayDummy;
                                goto read;
                            }
                            index = static_cast<int32_t>((type - 0xE6u) * 0x100u + idx);
                        }
                        addr = space + (lo + static_cast<uint32_t>(index)) * 4u;
                    }
                    else if (type < 0x6Du)
                        addr = script + 0x54u + (type < 0x0Du ? type + 0x5Du : type + frame - 0x0Du) * 4u;
                    else
                    {
                        uint32_t idxLocal, size, idx;
                        if (!byteAt(ip, idxLocal) || !byteAt(ip + 1u, size))
                            return false;
                        ip += 2u;
                        if (!local(idxLocal + frame, idx))
                            return false;
                        if (static_cast<int32_t>(idx) < 0 || static_cast<int32_t>(size) <= static_cast<int32_t>(idx))
                            addr = kArrayDummy;
                        else
                            addr = script + 0x54u + (type + frame - 0x6Du + idx) * 4u;
                    }
                read:
                    addr &= 0x1FFFFFFFu;
                    if (addr >= kRam - 4u)
                        return false;
                    std::memcpy(&v, ram + addr, 4);
                    break;
                }
                }
                r.types[written] = static_cast<uint8_t>(type);
                r.out[written++] = v;
                --left;
            } while (left != -1);
        r.ip = ip;
        r.valid = true;
        r.outPtr = outPtr;
        r.ipPtr = ipPtr;
        r.n = written;
        return true;
    }

    PS2_GUEST_CC void collectParamsNative(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const bool verify = std::getenv("VCS_NATIVE_PARAMS_VERIFY") != nullptr;
        static ParamsResult last;
        static uint64_t checked = 0, differed = 0;
        static uint32_t lastRa = 0;
        if (verify && last.valid) // what the original left at its last call
        {
            // Word 0 is often a command's result by then (the handler writes it into the parameter block before storing
            // it - a new handle, say), and a caller's stack block may be reused: only words 1.. are compared. Each word
            // is decoded by the same code whatever its position, so the types seen there cover the decoder.
            static uint64_t typeSeen[256] = {};
            ++checked;
            for (uint32_t i = 1; i < last.n; ++i)
            {
                uint32_t v;
                std::memcpy(&v, rdram + last.outPtr + i * 4u, 4);
                ++typeSeen[last.types[i]];
                if (v != last.out[i])
                {
                    ++differed;
                    if (differed <= 40)
                        std::fprintf(stderr, "[params] verify: call %llu word %u differs (ip 0x%x, ra 0x%08x): type %02x native %08x original %08x\n",
                                     static_cast<unsigned long long>(checked), i, last.startIp, lastRa, last.types[i], last.out[i], v);
                    break;
                }
            }
            if (checked % 2000000u == 0u)
            {
                uint64_t locals = 0, localArrays = 0, globals = 0, globalArrays = 0;
                for (int t = 0x0B; t < 256; ++t)
                    (t < 0x6D ? locals : t < 0xCD ? localArrays : t < 0xE6 ? globals : globalArrays) += typeSeen[t];
                std::fprintf(stderr, "[params] verify: %llu calls, %llu differed in words 1..; types checked: int8 %llu int16 %llu int32 %llu "
                                     "float32 %llu float24/16/8 %llu/%llu/%llu zero %llu string %llu, locals %llu local arrays %llu globals %llu "
                                     "global arrays %llu\n",
                             static_cast<unsigned long long>(checked), static_cast<unsigned long long>(differed),
                             (unsigned long long)typeSeen[7], (unsigned long long)typeSeen[8], (unsigned long long)typeSeen[6],
                             (unsigned long long)typeSeen[9], (unsigned long long)typeSeen[5], (unsigned long long)typeSeen[4],
                             (unsigned long long)typeSeen[3], (unsigned long long)(typeSeen[1] + typeSeen[2]),
                             (unsigned long long)typeSeen[10], (unsigned long long)locals, (unsigned long long)localArrays,
                             (unsigned long long)globals, (unsigned long long)globalArrays);
            }
            last.valid = false;
        }
        ParamsResult r;
        if (!collectParams(rdram, GPR_U32(ctx, 4), GPR_U32(ctx, 5), GPR_S64(ctx, 6), GPR_U32(ctx, 7), r))
        {
            g_origCollectParams(rdram, ctx, runtime);
            return;
        }
        if (verify)
        {
            last = r;
            lastRa = GPR_U32(ctx, 31);
            g_origCollectParams(rdram, ctx, runtime); // it does the work; compared at the next call
            return;
        }
        std::memcpy(rdram + r.outPtr, r.out, r.n * 4u);
        std::memcpy(rdram + r.ipPtr, &r.ip, 4);
        SET_GPR_S32(ctx, 2, r.ret);
        ctx->pc = GPR_U32(ctx, 31);
    }

    // VCS_TRACE_IMMEDIATE=1: callers of the immediate-mode draw entries (kImmediateEntries) with their first three
    // arguments, counted, every 5 s - which game code draws a given Im2D / Im3D primitive
    struct ImmediateEntry
    {
        uint32_t at;
        const char *name;
    };
    constexpr ImmediateEntry kImmediateEntries[] = {
        {0x002FBE08u, "im2d-prim"}, {0x002FBE68u, "im2d-indexed"}, {0x002FBD78u, "im2d-line"}, {0x0035C8F0u, "im2d-35c8f0"},
        {0x002FB9D0u, "im2d-core"}, {0x002FC2E8u, "im3d-2fc2e8"},  {0x002FC838u, "im3d-2fc838"}, {0x002F8250u, "im3d-submit"},
    };
    constexpr int kImmediateCount = static_cast<int>(sizeof(kImmediateEntries) / sizeof(kImmediateEntries[0]));
    PS2Runtime::RecompiledFunction g_origImmediate[kImmediateCount] = {};
    template <int Which>
    PS2_GUEST_CC void traceImmediate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static std::mutex lock;
        static std::map<std::tuple<int, uint32_t, uint32_t, uint32_t, uint32_t>, uint64_t> counts;
        static auto last = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> g(lock);
            ++counts[{Which, GPR_U32(ctx, 31), GPR_U32(ctx, 4), GPR_U32(ctx, 5), GPR_U32(ctx, 6)}];
            const auto now = std::chrono::steady_clock::now();
            if (now - last > std::chrono::seconds(5))
            {
                last = now;
                for (const auto &[k, n] : counts)
                    std::fprintf(stderr, "[immediate] %s ra=0x%08x a0=0x%x a1=0x%x a2=0x%x x%llu\n",
                                 kImmediateEntries[std::get<0>(k)].name, std::get<1>(k), std::get<2>(k), std::get<3>(k),
                                 std::get<4>(k), static_cast<unsigned long long>(n));
                counts.clear();
            }
        }
        g_origImmediate[Which](rdram, ctx, runtime);
    }
    template <int... I>
    void hookImmediate(PS2Runtime &runtime, std::integer_sequence<int, I...>)
    {
        const PS2Runtime::RecompiledFunction hooks[] = {&traceImmediate<I>...};
        for (int i = 0; i < kImmediateCount; ++i)
            if ((g_origImmediate[i] = runtime.lookupFunction(kImmediateEntries[i].at)))
                runtime.replaceFunction(kImmediateEntries[i].at, hooks[i]);
    }

    PS2Runtime::RecompiledFunction g_origDmaSend = nullptr;
    PS2_GUEST_CC void traceDmaSend(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static std::map<std::pair<uint32_t, uint32_t>, uint64_t> counts;
        static auto last = std::chrono::steady_clock::now();
        ++counts[{GPR_U32(ctx, 31), GPR_U32(ctx, 4)}];
        const auto now = std::chrono::steady_clock::now();
        if (now - last > std::chrono::seconds(5))
        {
            last = now;
            for (const auto &[k, n] : counts)
                std::fprintf(stderr, "[dmasend] ra=0x%08x ch=0x%08x x%llu\n", k.first, k.second, static_cast<unsigned long long>(n));
            counts.clear();
        }
        g_origDmaSend(rdram, ctx, runtime);
    }

    // VCS_TRACE_VISIBLE=1: CCamera::IsSphereVisible(cam, center, matrix, radius) at 0x3816F8 (a leaf, safe to
    // inspect after the call): visible ratio every 5 s and a few samples with the camera-space point.
    float guestFloat(uint8_t *rdram, uint32_t address)
    {
        float f;
        std::memcpy(&f, rdram + (address & PS2_RAM_MASK), 4);
        return f;
    }
    PS2_GUEST_CC void traceSphereVisible(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cam = GPR_U32(ctx, 4), center = GPR_U32(ctx, 5), mat = GPR_U32(ctx, 6);
        const float radius = ctx->f[12];
        const uint32_t sp = GPR_U32(ctx, 29) - 16u;
        vcs::prepareSphereTest(rdram, cam); // this wrapper replaces the display hooks' one
        FUN_003816f8_0x3816f8(rdram, ctx, runtime);
        static uint64_t calls = 0, visible = 0;
        static int samples = 0;
        static auto last = std::chrono::steady_clock::now();
        ++calls;
        visible += GPR_U32(ctx, 2) ? 1u : 0u;
        if (samples < 12 && (calls % 997u) == 0u)
        {
            ++samples;
            std::fprintf(stderr,
                         "[vis] r=%u center=(%.1f,%.1f,%.1f) radius=%.1f camSpace=(%.1f,%.1f,%.1f) near=%.3f far=%.1f "
                         "planes=(%.3f,%.3f)(%.3f,%.3f)(%.3f,%.3f)(%.3f,%.3f) mat.row3=(%.1f,%.1f,%.1f)\n",
                         GPR_U32(ctx, 2), guestFloat(rdram, center), guestFloat(rdram, center + 4), guestFloat(rdram, center + 8), radius,
                         guestFloat(rdram, sp), guestFloat(rdram, sp + 4), guestFloat(rdram, sp + 8), guestFloat(rdram, 0x4CD130u),
                         guestFloat(rdram, 0x4CD12Cu), guestFloat(rdram, cam + 0xB60), guestFloat(rdram, cam + 0xB64),
                         guestFloat(rdram, cam + 0xB70), guestFloat(rdram, cam + 0xB74), guestFloat(rdram, cam + 0xB84),
                         guestFloat(rdram, cam + 0xB88), guestFloat(rdram, cam + 0xB94), guestFloat(rdram, cam + 0xB98),
                         guestFloat(rdram, mat + 48), guestFloat(rdram, mat + 52), guestFloat(rdram, mat + 56));
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last > std::chrono::seconds(5))
        {
            last = now;
            std::fprintf(stderr, "[vis] %llu calls, %llu visible\n", static_cast<unsigned long long>(calls),
                         static_cast<unsigned long long>(visible));
            calls = visible = 0;
        }
    }

    // VCS_TRACE_BOXCLIP=1: FUN_0030C8D4(prog, box*, out0*, out1*) = CTC2 CMSAR0; LQC2 vf1,vf2; VCALLMSR; CFC2 vi1,vi2.
    // The sector visibility test (FUN_002F7528) culls unless the low 6 bits come back 0x3F.
    PS2_GUEST_CC void traceBoxClip(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t prog = GPR_U32(ctx, 4), box = GPR_U32(ctx, 5), out0 = GPR_U32(ctx, 6), out1 = GPR_U32(ctx, 7);
        static bool dumped = false;
        if (!dumped)
        {
            dumped = true;
            if (FILE *f = std::fopen("vu0_code.bin", "wb"))
            {
                std::fwrite(runtime->memory().getVU0Code(), 1, PS2_VU0_CODE_SIZE, f);
                std::fclose(f);
            }
            if (FILE *f = std::fopen("vu0_data.bin", "wb"))
            {
                std::fwrite(runtime->memory().getVU0Data(), 1, PS2_VU0_DATA_SIZE, f);
                std::fclose(f);
            }
        }
        FUN_0030c8d4_0x30c8d4(rdram, ctx, runtime);
        static int samples = 0;
        static uint64_t calls = 0, allIn = 0;
        uint32_t r0 = 0, r1 = 0;
        std::memcpy(&r0, rdram + (out0 & PS2_RAM_MASK), 4);
        std::memcpy(&r1, rdram + (out1 & PS2_RAM_MASK), 4);
        ++calls;
        allIn += ((r1 & 0x3Fu) == 0x3Fu || (r0 & 0x3Fu) == 0x3Fu) ? 1u : 0u;
        if (samples < 10 && (calls % 211u) == 0u)
        {
            ++samples;
            std::fprintf(stderr, "[boxclip] prog=0x%x box=(%.2f,%.2f,%.2f,%.2f)(%.2f,%.2f,%.2f,%.2f) -> vi1>>4=0x%x vi2>>4=0x%x vi15=%u\n", prog,
                         guestFloat(rdram, box), guestFloat(rdram, box + 4), guestFloat(rdram, box + 8), guestFloat(rdram, box + 12),
                         guestFloat(rdram, box + 16), guestFloat(rdram, box + 20), guestFloat(rdram, box + 24), guestFloat(rdram, box + 28), r0, r1,
                         static_cast<uint32_t>(ctx->vi[15]));
        }
        if ((calls % 20000u) == 0u)
            std::fprintf(stderr, "[boxclip] %llu calls, %llu with 0x3F\n", static_cast<unsigned long long>(calls),
                         static_cast<unsigned long long>(allIn));
    }

    // VCS_TEST_SAVE=<s>[:<slot>]: <s> seconds after boot, save to slot 1 (or 1..8) through the game's own memory card code, in the
    // order FUN_00108128 (the safehouse save menu's action) uses: FUN_00276bf0(card manager 0x510480, 0) scans
    // the card, the "saving" flag at 0x48A664 goes up, FUN_002778a8(manager, slot) saves, the flag goes down.
    // Each call is made as a guest tail call from scePadRead (pc = function, ra = scePadRead's return address),
    // so scheduler checkpoints inside the save unwind and resume normally; scePadRead's own result is lost
    // for those frames. Lets the save path be tested without playing to a safehouse.
    //
    // VCS_DUMP_RAM=<s>[,<s>...]: write all 32 MB of EE RAM to ram_<s>.bin at those seconds after boot
    // (finding game structures by diffing snapshots).
    PS2Runtime::RecompiledFunction g_origPadRead = nullptr;
    void dumpRamOnSchedule(const uint8_t *rdram)
    {
        static const char *env = std::getenv("VCS_DUMP_RAM");
        static const auto start = std::chrono::steady_clock::now();
        static size_t next = 0;
        if (!env)
            return;
        const char *p = env;
        for (size_t i = 0; i < next && p; ++i)
            p = std::strchr(p, ',') ? std::strchr(p, ',') + 1 : nullptr;
        if (!p || !*p)
            return;
        const int at = std::atoi(p);
        if (std::chrono::steady_clock::now() - start < std::chrono::seconds(at))
            return;
        ++next;
        char name[64];
        std::snprintf(name, sizeof(name), "ram_%d.bin", at);
        if (FILE *f = std::fopen(name, "wb"))
        {
            std::fwrite(rdram, 1, PS2_RAM_SIZE, f);
            std::fclose(f);
            std::fprintf(stderr, "[ram] wrote %s\n", name);
        }
    }

    PS2_GUEST_CC void testSaveOnPadRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const auto start = std::chrono::steady_clock::now();
        static const char *saveEnv = std::getenv("VCS_TEST_SAVE");
        static const int delay = saveEnv ? std::atoi(saveEnv) : 0;
        static const uint32_t slot = [] {
            const char *colon = saveEnv ? std::strchr(saveEnv, ':') : nullptr;
            const int s = colon ? std::atoi(colon + 1) : 1;
            return static_cast<uint32_t>(std::clamp(s, 1, 8) - 1); // the game counts slots from 0
        }();
        static int step = 0;
        constexpr uint32_t kCardManager = 0x00510480u, kSavingFlag = 0x0048A664u;
        g_origPadRead(rdram, ctx, runtime);
        dumpRamOnSchedule(rdram);
        if (!saveEnv || step > 3 || std::chrono::steady_clock::now() - start < std::chrono::seconds(delay))
            return;
        uint32_t status = 0;
        std::memcpy(&status, rdram + kCardManager + 0x54u, 4);
        const auto tailCall = [&](uint32_t function, uint32_t a1 = 0u)
        {
            SET_GPR_U32(ctx, 4, kCardManager);
            SET_GPR_U32(ctx, 5, a1);
            ctx->pc = function;
        };
        switch (step++)
        {
        case 0:
            std::fprintf(stderr, "[mc-test] scanning the card (status %u)\n", status);
            tailCall(0x00276BF0u);
            break;
        case 1:
        {
            std::fprintf(stderr, "[mc-test] scan done (status %u); saving to slot %u\n", status, slot + 1u);
            const uint32_t one = 1u;
            std::memcpy(rdram + kSavingFlag, &one, 4);
            tailCall(0x002778A8u, slot);
            break;
        }
        case 2:
        {
            const uint32_t zero = 0u;
            std::memcpy(rdram + kSavingFlag, &zero, 4);
            std::fprintf(stderr, "[mc-test] save done, manager status %u\n", status);
            break;
        }
        }
    }

    // VCS_TRACE_SECTORS=1: why FUN_00240ED8(level, sector) skips sectors, every 5 s. It draws a sector only when
    // the sector has a model index (+2) whose slot in the level's model table (*(level+0x1C))[idx*12] holds a
    // loaded model; the culling result decides the rest.
    // VCS_TRACE_TELEPORT: FUN_0020b3d8(ped, CVector *pos) is the ped teleport (remove, set position, reset, add);
    // log who calls it (it is reached through a vtable) and where to.
    uint32_t g_currentScript = 0; // CRunningScript being interpreted (FUN_002560d8 ProcessOneCommand), game thread
    PS2_GUEST_CC void traceScriptCommand(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_currentScript = GPR_U32(ctx, 4);
        FUN_002560d8_0x2560d8(rdram, ctx, runtime);
    }

    PS2_GUEST_CC void traceTeleport(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        float pos[3];
        std::memcpy(pos, rdram + (GPR_U32(ctx, 5) & PS2_RAM_MASK), sizeof(pos));
        char name[9] = {};
        uint32_t ip = 0;
        if (g_currentScript)
        {
            std::memcpy(name, rdram + ((g_currentScript + 0x20F) & PS2_RAM_MASK), 8);
            std::memcpy(&ip, rdram + ((g_currentScript + 0x10) & PS2_RAM_MASK), 4);
        }
        std::fprintf(stderr, "[teleport] ped %08x -> (%.2f, %.2f, %.2f) ra=%08x last script %s ip %x\n", GPR_U32(ctx, 4), pos[0],
                     pos[1], pos[2], GPR_U32(ctx, 31), name, ip);
        FUN_0020b3d8_0x20b3d8(rdram, ctx, runtime);
    }

    // VCS_TRACE_PED_REMOVE: FUN_00210780 = CPed::FlagToDestroyWhenNextProcessed (sets bRemoveFromWorld, bit 32 of +0x48),
    // FUN_00198408 = CWorld::Process's remove + delete of a flagged entity. Logs each ped (pool entries 0xD20 bytes) with
    // its position, model (+0x56), created-by (+0x8F4: 1 random, 2 mission), state (+0x8C4), the script last
    // interpreted and the code addresses on the guest stack (the callers).
    void logPedRemoval(uint8_t *rdram, R5900Context *ctx, const char *what)
    {
        const auto rd32 = [&](uint32_t a) { uint32_t v = 0; std::memcpy(&v, rdram + (a & PS2_RAM_MASK), 4); return v; };
        const uint32_t ped = GPR_U32(ctx, 4);
        const uint32_t pool = rd32(0x00487A94u); // CPools ped pool: entries, flags, size
        const uint32_t entries = rd32(pool), flags = rd32(pool + 4u), size = rd32(pool + 8u);
        if (!pool || ped < entries || ped >= entries + size * 0xD20u || (ped - entries) % 0xD20u)
            return; // not a ped
        const uint32_t slot = (ped - entries) / 0xD20u;
        float pos[3];
        std::memcpy(pos, rdram + ((ped + 0x30u) & PS2_RAM_MASK), sizeof(pos));
        int16_t model = 0;
        std::memcpy(&model, rdram + ((ped + 0x56u) & PS2_RAM_MASK), 2);
        char name[9] = {};
        if (g_currentScript)
            std::memcpy(name, rdram + ((g_currentScript + 0x20F) & PS2_RAM_MASK), 8);
        std::string callers;
        const uint32_t sp = GPR_U32(ctx, 29) & PS2_RAM_MASK;
        for (uint32_t off = 0, n = 0; off < 0x300u && n < 12u; off += 4u)
        {
            const uint32_t w = rd32(sp + off);
            if (w >= 0x100000u && w < 0x480000u && !(w & 3u))
            {
                char buf[16];
                std::snprintf(buf, sizeof(buf), " %x", w);
                callers += buf;
                ++n;
            }
        }
        std::fprintf(stderr, "[ped-remove] %s ped %08x handle %04x (%.2f, %.2f, %.2f) model %d created-by %u state %u "
                     "+1c8 %08x%08x ra=%08x script %s callers%s\n", what, ped, (slot << 8) | rdram[(flags + slot) & PS2_RAM_MASK],
                     pos[0], pos[1], pos[2], model, rd32(ped + 0x8F4u), rd32(ped + 0x8C4u), rd32(ped + 0x1CCu),
                     rd32(ped + 0x1C8u), GPR_U32(ctx, 31), name, callers.c_str());
    }
    PS2_GUEST_CC void tracePedFlagDestroy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        logPedRemoval(rdram, ctx, "flag");
        FUN_00210780_0x210780(rdram, ctx, runtime);
    }
    PS2_GUEST_CC void tracePedWorldRemove(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        logPedRemoval(rdram, ctx, "delete");
        FUN_00198408_0x198408(rdram, ctx, runtime);
    }

    uint64_t g_sectorCalls = 0; // FUN_00240ED8 calls, for VCS_TRACE_CELL
    PS2_GUEST_CC void traceSector(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ++g_sectorCalls;
        static uint64_t calls = 0, noSector = 0, noIndex = 0, notLoaded = 0, loaded = 0;
        static auto last = std::chrono::steady_clock::now();
        static std::map<uint32_t, uint32_t> missing; // model index -> count
        const auto rd32 = [&](uint32_t a) { uint32_t v = 0; std::memcpy(&v, rdram + (a & PS2_RAM_MASK), 4); return v; };
        const uint32_t level = GPR_U32(ctx, 4), sector = GPR_U32(ctx, 5);
        ++calls;
        if (!sector)
            ++noSector;
        else
        {
            uint16_t idx = 0;
            std::memcpy(&idx, rdram + ((sector + 2u) & PS2_RAM_MASK), 2);
            if (!idx)
                ++noIndex;
            else if (!rd32(rd32(rd32(level + 0x1Cu)) + idx * 12u))
            {
                ++notLoaded;
                ++missing[idx];
            }
            else
                ++loaded;
        }
        FUN_00240ed8_0x240ed8(rdram, ctx, runtime);
        const auto now = std::chrono::steady_clock::now();
        if (now - last < std::chrono::seconds(5))
            return;
        last = now;
        std::fprintf(stderr, "[sector] %llu calls: no sector %llu, no model %llu, model not loaded %llu, loaded %llu; missing idx:",
                     (unsigned long long)calls, (unsigned long long)noSector, (unsigned long long)noIndex,
                     (unsigned long long)notLoaded, (unsigned long long)loaded);
        int shown = 0;
        for (const auto &[idx, n] : missing)
            if (shown++ < 12)
                std::fprintf(stderr, " %u(x%u)", idx, n);
        std::fprintf(stderr, " [%zu distinct]\n", missing.size());
        calls = noSector = noIndex = notLoaded = loaded = 0;
        missing.clear();
    }

    // VCS_TRACE_CELL=1: the world is drawn relative to the "current" hex cell (*(level+0x248): grid x at +8, y at
    // +0xC; FUN_00241F00 turns it into a world origin). Logs that cell every 2 s from the sector walk
    // FUN_0023E748, and the focus position each time FUN_0023C188(level, pos) re-evaluates it.
    PS2_GUEST_CC void traceCellWalk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static auto last = std::chrono::steady_clock::now();
        const auto rd32 = [&](uint32_t a) { uint32_t v = 0; std::memcpy(&v, rdram + (a & PS2_RAM_MASK), 4); return v; };
        const uint32_t level = GPR_U32(ctx, 4);
        const auto now = std::chrono::steady_clock::now();
        if (now - last > std::chrono::seconds(2))
        {
            last = now;
            const uint32_t cur = rd32(level + 0x248u), next = rd32(level + 0x250u);
            const int32_t gx = static_cast<int32_t>(rd32(cur + 8u)), gy = static_cast<int32_t>(rd32(cur + 0xCu));
            const float ox = (gx + 0.5f - (gy & 1) * 0.5f) * 125.0f - 2400.0f, oy = (gy + 0.5f) * 108.25f - 2000.0f;
            std::fprintf(stderr, "[cell] level=0x%x current=0x%x grid=(%d,%d) origin=(%.1f,%.1f) loaded=%u next=0x%x grid=(%d,%d) loaded=%u camera=(%.1f,%.1f,%.1f)\n",
                         level, cur, gx, gy, ox, oy, rd32(cur + 0x18u), next, static_cast<int32_t>(rd32(next + 8u)),
                         static_cast<int32_t>(rd32(next + 0xCu)), rd32(next + 0x18u), guestFloat(rdram, 0x006F4500u),
                         guestFloat(rdram, 0x006F4504u), guestFloat(rdram, 0x006F4508u));
        }
        static uint64_t walks = 0, emptyWalks = 0, skippedGrid = 0;
        static auto lastWalk = std::chrono::steady_clock::now();
        const uint64_t before = g_sectorCalls;
        if (static_cast<int32_t>(rd32(rd32(level + 0x248u) + 8u)) <= 0)
            ++skippedGrid;
        FUN_0023e748_0x23e748(rdram, ctx, runtime);
        ++walks;
        emptyWalks += (g_sectorCalls == before) ? 1u : 0u;
        if (now - lastWalk > std::chrono::seconds(1))
        {
            lastWalk = now;
            std::fprintf(stderr, "[cell] walks %llu, without sectors %llu, grid<=0 %llu\n", (unsigned long long)walks,
                         (unsigned long long)emptyWalks, (unsigned long long)skippedGrid);
            walks = emptyWalks = skippedGrid = 0;
        }
    }
    PS2_GUEST_CC void traceCellFocus(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t pos = GPR_U32(ctx, 5);
        const float x = guestFloat(rdram, pos), y = guestFloat(rdram, pos + 4), z = guestFloat(rdram, pos + 8);
        const int row = static_cast<int>((y + 2000.0f) * (1.0f / 108.25f));
        const int col = static_cast<int>((x + 2400.0f) * 0.008f + (row & 1) * 0.5f);
        std::fprintf(stderr, "[cell] focus (%.1f,%.1f,%.1f) -> grid (%d,%d)\n", x, y, z, col, row);
        FUN_0023c188_0x23c188(rdram, ctx, runtime);
    }

    // VCS_TRACE_PACKETS=1: FUN_00476770(buffer) returns the bytes used in the current half of a double-buffered DMA
    // packet buffer (halves of 0xE6670). Draw code skips work once fewer than ~0x12C00 bytes are left; log the
    // peak use and how often that limit was reached, every 2 s.
    PS2_GUEST_CC void tracePacketUse(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static std::map<uint32_t, uint32_t> peak;
        static uint64_t calls = 0, nearFull = 0;
        static auto last = std::chrono::steady_clock::now();
        const uint32_t buffer = GPR_U32(ctx, 4);
        FUN_00476770_0x476770(rdram, ctx, runtime);
        const uint32_t used = GPR_U32(ctx, 2);
        ++calls;
        nearFull += (0xE6670u - used <= 0x12BFFu) ? 1u : 0u;
        uint32_t &p = peak[buffer];
        p = std::max(p, used);
        const auto now = std::chrono::steady_clock::now();
        if (now - last < std::chrono::seconds(2))
            return;
        last = now;
        std::fprintf(stderr, "[packets] %llu checks, %llu near full; peak use:", (unsigned long long)calls, (unsigned long long)nearFull);
        for (const auto &[b, u] : peak)
            std::fprintf(stderr, " 0x%x=%u KB", b, u >> 10);
        std::fprintf(stderr, " (of %u KB)\n", 0xE6670u >> 10);
        peak.clear();
        calls = nearFull = 0;
    }

    // Game frame boundary for the perf overlay (runtime/ps2_perf.h): FUN_002723E8 kicks the frame's VIF1 display
    // list (D1_CHCR = 0x145 at 0x27245C), once per rendered frame.
    PS2_GUEST_CC void markFrameKick(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2perf::markGameFrame();
        vcs::profilerFrame(); // VCS_PROFILE_SPIKES (debug/profiler.h)
        vcs::applyDisplay(rdram); // widescreen preference / aspect (settings menu)
        vcs::rig::onGameFrame(rdram, ctx, runtime); // automation rig jobs (debug/rig.h)
        vcs::input::holdInBackground(runtime);      // paused while the window is in the background (game/vcs_input.h)
        vcs::trainer::onGameFrame(rdram);           // the trainer's actions and held toggles (game/vcs_trainer.h)
        vcs::volume::onGameFrame(rdram);            // the music / effects levels and the pause menu's (game/vcs_volume.h)
        vcs::gameinfo::onGameFrame(rdram);          // the F3 game-state panel's snapshot (debug/game_info.h)
        vcs::onFrameKick();                         // the HUD context ends (game/vcs_hud.h)
        vcs::rsltrace::onFrameKick(rdram);          // VCS_TRACE_RSL (debug/rsl_trace.h)
        vcs::rslshadow::onFrameKick(rdram);         // VCS_RSL_SHADOW (debug/rsl_shadow.h)
        vcs::rslworld::onFrameKick();               // world / model draws from RSL calls (game/vcs_rsl_world.h)
        vcs::rslnative::onFrameKick(rdram);         // native RSL emitters' verify (game/vcs_rsl_native.h)
        FUN_002723e8_0x2723e8(rdram, ctx, runtime);
    }

    // VCS_SKID_LOG=1: cAudioManager::ProcessVehicleSkidding (FUN_001BA600, a1 = cVehicleParams) inputs for vehicles
    // with a wheel not in the normal state
    PS2_GUEST_CC void logSkidding(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const auto rd32 = [&](uint32_t a) { uint32_t v = 0; std::memcpy(&v, rdram + (a & PS2_RAM_MASK), 4); return v; };
        const auto rdf = [&](uint32_t a) { float v = 0; std::memcpy(&v, rdram + (a & PS2_RAM_MASK), 4); return v; };
        const uint32_t p = GPR_U32(ctx, 5);
        const uint32_t type = rd32(p), veh = rd32(p + 0xC), trans = rd32(p + 0x10);
        const bool two = type == 6 || type == 8;
        const uint32_t base = two ? 0x668u : 0x7FCu, wheels = two ? 2u : 4u;
        uint32_t states = 0;
        for (uint32_t i = 0; i < wheels; ++i)
            states = states * 10u + rd32(veh + base + 8u + i * 4u);
        static int logged = 0, calls = 0;
        const uint32_t mine = rd32(rd32(0x004E4910u) + 0x450u); // the player's vehicle
        if (veh && veh == mine && (states || calls++ % 60 == 0) && logged++ < 2000)
            std::fprintf(stderr, "[skid] type %u veh %08x dist %.1f trans %08x drive '%c'(0x%02x) vel %.3f ground %u f %.3f states %0*u"
                                 " surface %u maxvel %.3f turnz %.3f this+4c %.1f\n",
                         type, veh, rdf(p + 8), trans, rdram[(trans + 0x5C) & PS2_RAM_MASK] >= 32 ? rdram[(trans + 0x5C) & PS2_RAM_MASK] : '?',
                         rdram[(trans + 0x5C) & PS2_RAM_MASK], rdf(p + 0x18), rdram[(veh + base) & PS2_RAM_MASK], rdf(veh + base + 4u),
                         static_cast<int>(wheels), states, rdram[(veh + 0x137) & PS2_RAM_MASK], rdf(trans + 0x50), rdf(veh + 0x78),
                         rdf(GPR_U32(ctx, 4) + 0x4C));
        FUN_001ba600_0x1ba600(rdram, ctx, runtime);
    }

    // the per-wheel skid values (FUN_001BAB18 non-driven, FUN_001BA9A8 driven; a1 vehicle, a2 wheel state, a3 transmission)
    template <PS2Runtime::RecompiledFunction Original, char Kind>
    PS2_GUEST_CC void logWheelSkid(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const auto rd32 = [&](uint32_t a) { uint32_t v = 0; std::memcpy(&v, rdram + (a & PS2_RAM_MASK), 4); return v; };
        const auto rdf = [&](uint32_t a) { float v = 0; std::memcpy(&v, rdram + (a & PS2_RAM_MASK), 4); return v; };
        const uint32_t veh = GPR_U32(ctx, 5);
        static int logged = 0;
        if (veh == rd32(rd32(0x004E4910u) + 0x450u) && logged++ < 400)
            std::fprintf(stderr, "[skid] %c state %u f12 %.3f f13 %.3f maxvel %.3f turnz %.3f\n", Kind, GPR_U32(ctx, 6), ctx->f[12],
                         ctx->f[13], rdf(GPR_U32(ctx, 7) + 0x50), rdf(veh + 0x78));
        Original(rdram, ctx, runtime);
    }

    // VCS_COLL_LOG=1 (investigation): CPhysical::ApplyCollisionAlt (FUN_0034D670: a0 A, a1 B, a2 colpoint) for the
    // player's vehicle
    PS2_GUEST_CC void logCollisionAlt(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const auto rd32 = [&](uint32_t a) { uint32_t v = 0; std::memcpy(&v, rdram + (a & PS2_RAM_MASK), 4); return v; };
        const auto rdf = [&](uint32_t a) { float v = 0; std::memcpy(&v, rdram + (a & PS2_RAM_MASK), 4); return v; };
        const uint32_t a = GPR_U32(ctx, 4), b = GPR_U32(ctx, 5), cp = GPR_U32(ctx, 6);
        static int logged = 0;
        if (a == rd32(rd32(0x004E4910u) + 0x450u) && logged++ < 4000)
            std::fprintf(stderr, "[coll] f %u ms %u A %08x B %08x(type %u) p %.3f %.3f %.3f n %.3f %.3f %.3f depth %.3f surf %u/%u"
                                 " v %.4f %.4f %.4f w %.4f %.4f %.4f\n",
                         rd32(0x4CD124u), rd32(0x4CD104u), a, b, rd32(b + 0x48) & 0xEu, rdf(cp), rdf(cp + 4), rdf(cp + 8),
                         rdf(cp + 0x20), rdf(cp + 0x24), rdf(cp + 0x28), rdf(cp + 0x34), rdram[(cp + 0x30) & PS2_RAM_MASK],
                         rdram[(cp + 0x32) & PS2_RAM_MASK], rdf(a + 0x140), rdf(a + 0x144), rdf(a + 0x148), rdf(a + 0x70),
                         rdf(a + 0x74), rdf(a + 0x78));
        FUN_0034d670_0x34d670(rdram, ctx, runtime);
    }

    void applyVcs(PS2Runtime &runtime)
    {
        if (std::getenv("VCS_COLL_LOG"))
            runtime.replaceFunction(0x0034D670u, &logCollisionAlt);
        if (std::getenv("VCS_SKID_LOG"))
        {
            runtime.replaceFunction(0x001BA600u, &logSkidding);
            runtime.replaceFunction(0x001BAB18u, &logWheelSkid<FUN_001bab18_0x1bab18, 'N'>);
            runtime.replaceFunction(0x001BA9A8u, &logWheelSkid<FUN_001ba9a8_0x1ba9a8, 'D'>);
        }
        runtime.replaceFunction(0x002723E8u, &markFrameKick);
        vcs::installDisplayHooks(runtime); // widescreen (settings menu)
        vcs::installHudHooks(runtime);     // 2D at the RenderWare level
        vcs::freecam::install(runtime);    // the trainer's free camera (game/vcs_freecam.h)
        vcs::aim::install(runtime);        // free aim with the mouse / right stick (game/vcs_aim.h)
        vcs::installFrameRateHooks(runtime); // 30 / 60 fps (game/vcs_framerate.h)
        vcs::installScriptTimingHooks(runtime); // per-frame mission meters on the time step (game/vcs_script_timing.h)
        vcs::installPhysicsTimingHooks(runtime); // per-frame vehicle pushes on the time step (game/vcs_physics_timing.h)
        vcs::installMovieHooks(runtime);     // STINGS / INTRO played natively (game/vcs_movie.h)
        const bool stream = vcs::installStreamHooks(runtime); // disc streaming without the IOP (game/vcs_stream.h)
        const bool sound = vcs::installSoundHooks(runtime);   // sound without the IOP (game/vcs_sound.h)
        vcs::retireIop(stream && sound);                      // then no emulated IOP at all
        vcs::installInflate(runtime);                         // zlib's inflate_fast natively (game/vcs_inflate.h)
        if (!std::getenv("VCS_NATIVE_PARAMS") || std::getenv("VCS_NATIVE_PARAMS")[0] != '0')
        {
            g_origCollectParams = runtime.lookupFunction(0x00254B90u);
            if (g_origCollectParams)
                runtime.replaceFunction(0x00254B90u, &collectParamsNative);
        }
        if (!std::getenv("VCS_NATIVE_MODEL_INDEX") || std::getenv("VCS_NATIVE_MODEL_INDEX")[0] != '0')
        {
            g_origModelIndex = runtime.lookupFunction(0x00255188u);
            if (g_origModelIndex)
                runtime.replaceFunction(0x00255188u, &modelIndex);
        }
        if (std::getenv("VCS_TRACE_PACKETS"))
            runtime.replaceFunction(0x00476770u, &tracePacketUse);
        if (std::getenv("VCS_TRACE_TELEPORT"))
        {
            runtime.replaceFunction(0x0020B3D8u, &traceTeleport);
            runtime.replaceFunction(0x002560D8u, &traceScriptCommand);
        }
        if (std::getenv("VCS_TRACE_PED_REMOVE"))
        {
            runtime.replaceFunction(0x00210780u, &tracePedFlagDestroy);
            runtime.replaceFunction(0x00198408u, &tracePedWorldRemove);
            runtime.replaceFunction(0x002560D8u, &traceScriptCommand);
        }
        if (std::getenv("VCS_TRACE_SECTORS"))
            runtime.replaceFunction(0x00240ED8u, &traceSector);
        if (std::getenv("VCS_TRACE_CELL"))
        {
            runtime.replaceFunction(0x00240ED8u, &traceSector);
            runtime.replaceFunction(0x0023E748u, &traceCellWalk);
            runtime.replaceFunction(0x0023C188u, &traceCellFocus);
        }
        installCallCounters(runtime);
        if (!std::getenv("VCS_NATIVE_IDENTITY") || std::getenv("VCS_NATIVE_IDENTITY")[0] != '0')
        {
            g_origIdentity = runtime.lookupFunction(0x003F9E50u);
            if (g_origIdentity)
                runtime.replaceFunction(0x003F9E50u, &identityMatrix);
        }
        vcs::rslnative::install(runtime); // before the RSL tools: they wrap whatever the flush is then
        vcs::rsltrace::install(runtime);
        vcs::rslshadow::install(runtime);
        vcs::rslworld::install(runtime);
        if (std::getenv("VCS_TEST_SAVE") || std::getenv("VCS_DUMP_RAM"))
        {
            g_origPadRead = runtime.lookupFunction(0x00449488u); // scePadRead: polled every frame
            if (g_origPadRead)
                runtime.replaceFunction(0x00449488u, &testSaveOnPadRead);
        }
        if (std::getenv("VCS_TRACE_BOXCLIP"))
            runtime.replaceFunction(0x0030C8D4u, &traceBoxClip);
        if (std::getenv("VCS_EXPERIMENT_NOCLIP")) // timing experiment only: every sector box "fully inside"
        {
            struct AllInside // (a named function: lambdas convert only to the platform's calling convention)
            {
                static PS2_GUEST_CC void function(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
                {
                    const uint32_t v = 0x3Fu;
                    std::memcpy(rdram + (GPR_U32(ctx, 6) & PS2_RAM_MASK), &v, 4);
                    std::memcpy(rdram + (GPR_U32(ctx, 7) & PS2_RAM_MASK), &v, 4);
                    ctx->pc = GPR_U32(ctx, 31);
                }
            };
            runtime.replaceFunction(0x0030C8D4u, &AllInside::function);
        }
        if (std::getenv("VCS_TRACE_VISIBLE"))
            runtime.replaceFunction(0x003816F8u, &traceSphereVisible);
        if (std::getenv("VCS_TRACE_DMASEND"))
        {
            g_origDmaSend = runtime.lookupFunction(0x00448970u);
            if (g_origDmaSend)
                runtime.replaceFunction(0x00448970u, &traceDmaSend);
        }
        if (std::getenv("VCS_TRACE_IMMEDIATE"))
            hookImmediate(runtime, std::make_integer_sequence<int, kImmediateCount>{});
        if (std::getenv("VCS_TRACE_FILES"))
            runtime.replaceFunction(0x0023BAF0u, &traceLevelLoad);
        if (std::getenv("VCS_TRACE_SOUND"))
        {
            runtime.replaceFunction(0x0010E970u, &traceSoundCmd);
            runtime.replaceFunction(0x0010D868u, &traceSoundInit);
        }
        if (std::getenv("VCS_TRACE_DELAY"))
        {
            runtime.replaceFunction(0x00468318u, &traceDelay);
            runtime.replaceFunction(0x002FA908u, &traceMusicMode);
        }
        runtime.replaceFunction(0x004604F0u, &traceAbort);
        runtime.replaceFunction(0x004672C0u, &traceAbort); // _Exit
        for (const Binding &b : kBindings)
        {
            if (b.handler && !ps2_game_overrides::bindAddressHandler(runtime, b.address, b.handler))
                std::cerr << "[vcs] failed to bind 0x" << std::hex << b.address << std::dec << " -> " << b.handler << std::endl;
        }
        for (const Skip &s : kSkips)
        {
            if (s.address && !runtime.replaceFunction(s.address, &returnZero))
                std::cerr << "[vcs] failed to skip 0x" << std::hex << s.address << std::dec << " (" << s.why << ")" << std::endl;
        }
        if (std::getenv("VCS_TRACE_FILES"))
        {
            runtime.replaceFunction(0x00220C68u, &traceOpen);
            runtime.replaceFunction(0x00220778u, &traceRead);
            runtime.replaceFunction(0x0021FA38u, &traceInflate);
            runtime.replaceFunction(0x00324A50u, &traceRelocate);
            runtime.replaceFunction(0x00265208u, &traceInflateEnd);
        }
    }
}

PS2_REGISTER_GAME_OVERRIDE("GTA Vice City Stories (SLUS-21590)", "SLUS_215.90", 0x001DD6A0u, 0x079B8BE6u, &applyVcs)
