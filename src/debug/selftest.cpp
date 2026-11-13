// VCS_SELFTEST=1: before booting, run a few recompiled libgcc helpers on known
// inputs and compare with native results. Catches recompiler/instruction bugs in
// code every other function depends on.
#include "selftest.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include <ps2_recompiled_functions.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#include "runtime/gs/gs_cpu_backend.h"

namespace vcs
{
    namespace
    {
        using GuestFn = PS2Runtime::RecompiledFunction;

        uint64_t call2(PS2Runtime &runtime, GuestFn fn, uint32_t pc, uint64_t a, uint64_t b)
        {
            uint8_t *rdram = runtime.memory().getRDRAM();
            R5900Context storage{};
            R5900Context *ctx = &storage;
            SET_GPR_U64(ctx, 4, a);
            SET_GPR_U64(ctx, 5, b);
            SET_GPR_U32(ctx, 29, 0x01F00000u); // scratch stack, below the game's
            SET_GPR_U32(ctx, 31, 0u);
            ctx->pc = pc;
            fn(rdram, ctx, &runtime);
            return GPR_U64(ctx, 2);
        }

        // The MMI macros the recompiler emits for PADDUH, PSUBUH, PABSW, PABSH, PSLLVW, PSRLVW and PSRAVW against a
        // scalar model of the EE instruction (GitHub issue 80: all seven gave wrong values). Called the way the
        // generated code calls them: rd = op(rs, rt) for the adds, op(rt) for the abs, op(rt value, rs amount) for the
        // shifts. VCS itself uses none of the seven (none in generated/ee), so this is the only thing that runs them.
        int checkMmi()
        {
            struct Q
            {
                uint32_t w[4];
            };
            auto toVec = [](const Q &q) {
                __m128i v;
                std::memcpy(&v, q.w, sizeof(v));
                return v;
            };
            auto fromVec = [](__m128i v) {
                Q q;
                std::memcpy(q.w, &v, sizeof(q.w));
                return q;
            };
            auto half = [](const Q &q, int i) { return static_cast<uint16_t>(q.w[i / 2] >> ((i & 1) * 16)); };
            auto setHalf = [](Q &q, int i, uint16_t h) {
                const int s = (i & 1) * 16;
                q.w[i / 2] = (q.w[i / 2] & ~(0xFFFFu << s)) | (uint32_t(h) << s);
            };

            const char *names[] = {"PADDUH", "PSUBUH", "PABSW", "PABSH", "PSLLVW", "PSRLVW", "PSRAVW"};
            auto model = [&](int op, const Q &rs, const Q &rt) {
                Q r{};
                switch (op)
                {
                case 0:
                case 1:
                    for (int i = 0; i < 8; ++i)
                    {
                        const int a = half(rs, i), b = half(rt, i);
                        const int v = op == 0 ? (a + b > 0xFFFF ? 0xFFFF : a + b) : (a - b < 0 ? 0 : a - b);
                        setHalf(r, i, static_cast<uint16_t>(v));
                    }
                    break;
                case 2:
                    for (int i = 0; i < 4; ++i)
                    {
                        const int64_t x = static_cast<int32_t>(rt.w[i]);
                        const int64_t a = x < 0 ? -x : x;
                        r.w[i] = static_cast<uint32_t>(a > 0x7FFFFFFF ? 0x7FFFFFFF : a);
                    }
                    break;
                case 3:
                    for (int i = 0; i < 8; ++i)
                    {
                        const int x = static_cast<int16_t>(half(rt, i));
                        const int a = x < 0 ? -x : x;
                        setHalf(r, i, static_cast<uint16_t>(a > 0x7FFF ? 0x7FFF : a));
                    }
                    break;
                default:
                    for (int i = 0; i < 2; ++i)
                    {
                        const uint32_t v = rt.w[i * 2], n = rs.w[i * 2] & 31;
                        const uint32_t s = op == 4 ? v << n
                                         : op == 5 ? v >> n
                                                   : static_cast<uint32_t>(static_cast<int32_t>(v) >> n);
                        r.w[i * 2] = s;
                        r.w[i * 2 + 1] = (s & 0x80000000u) ? 0xFFFFFFFFu : 0u;
                    }
                    break;
                }
                return r;
            };
            auto run = [&](int op, const Q &rs, const Q &rt) {
                const __m128i s = toVec(rs), t = toVec(rt);
                switch (op)
                {
                case 0: return fromVec(PS2_PADDUH(s, t));
                case 1: return fromVec(PS2_PSUBUH(s, t));
                case 2: return fromVec(PS2_PABSW(t));
                case 3: return fromVec(PS2_PABSH(t));
                case 4: return fromVec(PS2_PSLLVW(t, s));
                case 5: return fromVec(PS2_PSRLVW(t, s));
                default: return fromVec(PS2_PSRAVW(t, s));
                }
            };

            // Inputs: the issue's PSLLVW example, then 63 pseudo-random pairs whose words are often edge values
            const uint32_t edges[] = {0, 1, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000u, 0xFFFFFFFFu, 0x80008000u,
                                      0x7FFF8000u, 0x0000FFFFu, 31, 32, 33};
            uint64_t seed = 0x9E3779B97F4A7C15ull;
            auto next = [&]() {
                seed = seed * 6364136223846793005ull + 1442695040888963407ull;
                const uint32_t r = static_cast<uint32_t>(seed >> 32);
                return (r & 3) == 0 ? edges[(r >> 8) % (sizeof(edges) / sizeof(edges[0]))] : r;
            };
            std::vector<std::pair<Q, Q>> inputs{{Q{{1, 0, 4, 0}}, Q{{0x40000000u, 0, 1, 0}}}};
            while (inputs.size() < 64)
            {
                Q rs, rt;
                for (uint32_t &w : rs.w)
                    w = next();
                for (uint32_t &w : rt.w)
                    w = next();
                inputs.push_back({rs, rt});
            }
            int failures = 0;
            for (int op = 0; op < 7; ++op)
            {
                int bad = 0;
                for (const auto &[rs, rt] : inputs)
                {
                    const Q want = model(op, rs, rt), got = run(op, rs, rt);
                    if (std::memcmp(want.w, got.w, sizeof(want.w)) != 0 && bad++ < 2)
                        std::fprintf(stderr,
                                     "[selftest] %s rs %08x %08x %08x %08x rt %08x %08x %08x %08x: got %08x %08x %08x "
                                     "%08x want %08x %08x %08x %08x\n",
                                     names[op], rs.w[0], rs.w[1], rs.w[2], rs.w[3], rt.w[0], rt.w[1], rt.w[2],
                                     rt.w[3], got.w[0], got.w[1], got.w[2], got.w[3], want.w[0], want.w[1], want.w[2],
                                     want.w[3]);
                }
                std::fprintf(stderr, "[selftest] %s mmi %s: %d of %zu inputs differ\n", bad ? "FAIL" : "ok  ", names[op],
                             bad, inputs.size());
                failures += bad ? 1 : 0;
            }
            return failures;
        }
    }

    bool runSelfTest(PS2Runtime &runtime)
    {
        if (!std::getenv("VCS_SELFTEST"))
            return true;
        struct Case
        {
            uint64_t a, b;
        };
        const Case cases[] = {
            {100, 7}, {65521ull * 3 + 5, 65521}, {0xFFFFFFFFull, 65521}, {0x123456789ABCDEFull, 65521},
            {0xFFFFFFFFFFFFFFFFull, 3}, {0x100000000ull, 0x10001}, {0x8000000000000000ull, 0xFFFFFFFFull},
            {12345678901234ull, 987654321ull},
        };
        int failures = 0;
        for (const Case &c : cases)
        {
            const uint64_t div = call2(runtime, FUN_00443c30_0x443c30, 0x00443C30u, c.a, c.b);
            const uint64_t mod = call2(runtime, FUN_00444200_0x444200, 0x00444200u, c.a, c.b);
            const uint64_t mul = call2(runtime, FUN_00443bc8_0x443bc8, 0x00443BC8u, c.a, c.b);
            const bool ok = div == c.a / c.b && mod == c.a % c.b && mul == c.a * c.b;
            failures += ok ? 0 : 1;
            std::fprintf(stderr, "[selftest] %s %llx,%llx: udiv %llx (%llx) umod %llx (%llx) mul %llx (%llx)\n",
                         ok ? "ok  " : "FAIL", (unsigned long long)c.a, (unsigned long long)c.b,
                         (unsigned long long)div, (unsigned long long)(c.a / c.b),
                         (unsigned long long)mod, (unsigned long long)(c.a % c.b),
                         (unsigned long long)mul, (unsigned long long)(c.a * c.b));
        }
        // RenderWare's clip-flag helper (0x32E508: VCLIPw.xyz on a clip-space vertex, the flags read back from vi18):
        // bit 0 x > +|w|, bit 1 x < -|w|, then y, z. The recompiler had + / - swapped and compared with w, not |w|, so
        // the Im3D line clipper cut lines at the wrong plane (the plane trails across the screen, game-internals.md 6k)
        {
            struct ClipCase
            {
                float v[4];
                uint32_t flags;
            };
            const ClipCase clipCases[] = {
                {{2, 0, 0, 1}, 0x01},     {{-2, 0, 0, 1}, 0x02},     {{0, 2, 0, 1}, 0x04},  {{0, -2, 0, 1}, 0x08},
                {{0, 0, 2, 1}, 0x10},     {{0, 0, -2, 1}, 0x20},     {{0.5f, 0, 0, 1}, 0},  {{0.5f, -0.5f, 0, -1}, 0},
                {{2, -3, 0.5f, -1}, 0x09}, {{-2, 3, -4, -1}, 0x26}, {{1, 0, 0, 0}, 0x01},  {{0, 0, 0, 0}, 0},
            };
            constexpr uint32_t kScratch = 0x01F80000u; // free before boot
            uint8_t *rdram = runtime.memory().getRDRAM();
            for (const ClipCase &c : clipCases)
            {
                std::memcpy(rdram + kScratch, c.v, sizeof(c.v));
                const uint32_t got = static_cast<uint32_t>(call2(runtime, FUN_0032e508_0x32e508, 0x0032E508u, kScratch, 0));
                const bool ok = got == c.flags;
                failures += ok ? 0 : 1;
                std::fprintf(stderr, "[selftest] %s vclip (%g, %g, %g, %g): flags %02x (%02x)\n", ok ? "ok  " : "FAIL", c.v[0],
                             c.v[1], c.v[2], c.v[3], got, c.flags);
            }
        }
        // GS host->local PSMT8 upload, then T8 reads must give back the linear input
        // (VCS_SELFTEST_XTX=<loading screen .XTX>: 512x512 indices at 0x4B0).
        if (const char *xtx = std::getenv("VCS_SELFTEST_XTX"))
        {
            std::vector<uint8_t> file;
            if (FILE *f = std::fopen(xtx, "rb"))
            {
                file.resize(263372);
                file.resize(std::fread(file.data(), 1, file.size(), f));
                std::fclose(f);
            }
            if (file.size() >= 0x4B0 + 512 * 512)
            {
                std::vector<uint8_t> vram(4u << 20u);
                GSCpuBackend gs;
                gs.Initialize(vram.data(), static_cast<uint32_t>(vram.size()));
                GSTransferCommand t{};
                t.bitbltbuf.dbp = 8960;
                t.bitbltbuf.dbw = 8;
                t.bitbltbuf.dpsm = GS_PSM_T8;
                t.trxreg.rrw = 512;
                t.trxreg.rrh = 512;
                t.direction = 0;
                gs.BeginTransfer(t);
                gs.UploadImage(file.data() + 0x4B0, 512 * 512);
                int bad = 0;
                for (uint32_t y = 0; y < 512; ++y)
                    for (uint32_t x = 0; x < 512; ++x)
                        if (gs.ReadVram(GS_PSM_T8, 8960, 8, x, y) != file[0x4B0 + y * 512 + x] && bad++ < 4)
                            std::fprintf(stderr, "[selftest] T8 mismatch at %u,%u: read %u want %u\n", x, y,
                                         gs.ReadVram(GS_PSM_T8, 8960, 8, x, y), file[0x4B0 + y * 512 + x]);
                std::fprintf(stderr, "[selftest] T8 upload/read round trip: %d mismatches\n", bad);
                failures += bad ? 1 : 0;
            }
        }
        failures += checkMmi();
        std::fprintf(stderr, "[selftest] %d failure(s)\n", failures);
        return failures == 0;
    }
}
