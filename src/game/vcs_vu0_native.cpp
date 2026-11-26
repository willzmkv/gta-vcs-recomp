// Native versions of VCS's two hottest VU0 microprograms (about a quarter of the EE thread when interpreted:
// they read MAC flags, so the interpreter runs them on its exact path). Transcribed pair by pair from the
// microcode; the maths uses the same per-lane rules as the compiled VU1 code (runtime/ps2_vu1_compiled.h), so
// results are bit-identical to the interpreter. Registered by code hash: they only run while VU0 code memory
// holds exactly the game's programs. PS2X_VU0_VERIFY=1 checks them against the interpreter on live data.
#include "ps2_runtime.h"
#include "game/vcs_collision_native.h"
#include "runtime/ps2_vu0_native.h"
#include "runtime/ps2_vu1_compiled.h"

#include <cstdint>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
    using vu1v::fused;
    using vu1v::load;
    using vu1v::operand;
    using vu1v::splat;
    using vu1v::store;

    // The program's view of VU0's registers. [gta-recomp] VF and ACC are the context's own (`__m128 vu0_vf[32]` has
    // this layout), worked on in place: copying the whole register file in and out was ~1 KB per call at ~3,100 calls
    // a frame. VF0 reads (0, 0, 0, 1) during the call and the context's own VF0 is put back after, as the copy left it
    // (writes to VF0 were never stored); VI is copied (VI0 reads 0; VI1..15 stored back by storeRegs).
    struct Vu0Regs
    {
        float (*vf)[4] = nullptr;
        float *acc = nullptr;
        int16_t vi[16] = {};
        R5900Context *ctx = nullptr;
        __m128 savedVf0 = _mm_setzero_ps();

        Vu0Regs() = default;
        Vu0Regs(const Vu0Regs &) = delete;
        Vu0Regs &operator=(const Vu0Regs &) = delete;
        ~Vu0Regs()
        {
            if (ctx)
                ctx->vu0_vf[0] = savedVf0;
        }
    };

    void loadRegs(R5900Context *ctx, Vu0Regs &r)
    {
        r.ctx = ctx;
        r.savedVf0 = ctx->vu0_vf[0];
        ctx->vu0_vf[0] = _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f); // as copyVu0ContextToState
        r.vf = reinterpret_cast<float (*)[4]>(ctx->vu0_vf);
        r.acc = reinterpret_cast<float *>(&ctx->vu0_acc);
        for (int i = 0; i < 16; ++i)
            r.vi[i] = static_cast<int16_t>(ctx->vi[i]);
        r.vi[0] = 0;
    }

    void storeRegs(const Vu0Regs &r, R5900Context *ctx)
    {
        for (int i = 1; i < 16; ++i) // (VF / ACC were written in place)
            ctx->vi[i] = static_cast<uint16_t>(r.vi[i]);
    }

    // VF = a * b.bc  (MUL / MULA with a broadcast)
    vu1v::V mulBc(const float *a, const float *b, int bc) { return operand(vu1v::mul(operand(load(a)), splat(vu1fast::operand(b[bc])))); }
    // ACC + a * b.bc  (MADD / MADDA)
    vu1v::V maddBc(const float *acc, const float *a, const float *b, int bc)
    {
        return fused<0xFu, false>(operand(load(acc)), operand(load(a)), splat(vu1fast::operand(b[bc])));
    }

    // MAC sign flags of a full-dest result, as FMAND sees them: bit 7 = x ... bit 4 = w
    int16_t signFlags(const __m128 v)
    {
        static constexpr int16_t kLut[16] = {
            0x00, 0x80, 0x40, 0xC0, 0x20, 0xA0, 0x60, 0xE0,
            0x10, 0x90, 0x50, 0xD0, 0x30, 0xB0, 0x70, 0xF0,
        };
        return kLut[_mm_movemask_ps(v)];
    }
    int16_t signFlags(const float *v) { return signFlags(_mm_loadu_ps(v)); }

    void lqi(Vu0Regs &r, int vf, int vi, const uint8_t *data, uint32_t size)
    {
        const uint32_t addr = (static_cast<uint32_t>(static_cast<uint16_t>(r.vi[vi])) * 16u) & (size - 1u);
        std::memcpy(r.vf[vf], data + addr, 16);
        r.vi[vi] = static_cast<int16_t>(r.vi[vi] + 1);
    }

    void sqi(Vu0Regs &r, int vf, int vi, uint8_t *data, uint32_t size)
    {
        const uint32_t addr = (static_cast<uint32_t>(static_cast<uint16_t>(r.vi[vi])) * 16u) & (size - 1u);
        std::memcpy(data + addr, r.vf[vf], 16);
        r.vi[vi] = static_cast<int16_t>(r.vi[vi] + 1);
    }

    // VU0 0x0CF8: sector box vs three plane sets (FUN_002F7528 via FUN_0030C8D4). In: vf1 = box min, vf2 = box max,
    // vi15 = VU address of the matrices. Out: vi1 / vi2 = per set the sign flags of the box's most negative /
    // most positive transformed corner (4 bits each, shifted in by 4 per set).
    void boxClip(R5900Context *ctx, uint8_t *data, uint32_t size)
    {
        Vu0Regs r;
        loadRegs(ctx, r);
        r.vi[4] = static_cast<int16_t>(r.vi[15] + 24);                     // 0cf8 IADDIU vi4, vi15, 24
        store<0xFu>(r.vf[31], mulBc(r.vf[0], r.vf[0], 0));                 // 0d00 MULx vf31, vf0, vf0x | IADD vi1 = 0
        r.vi[1] = 0;
        store<0x7u>(r.vf[2], operand(vu1v::sub(operand(load(r.vf[2])), operand(load(r.vf[1]))))); // 0d08 SUB.xyz | vi2 = 0
        r.vi[2] = 0;
        r.vi[3] = 3;                                                        // 0d10
        r.vi[5] = 240;                                                      // 0d18
        auto loadFour = [&](vu1v::V &a0, vu1v::V &a1, vu1v::V &a2, vu1v::V &a3) {
            const uint32_t addr = (static_cast<uint32_t>(static_cast<uint16_t>(r.vi[4])) * 16u) & (size - 1u);
            if (addr + 64u <= size)
            {
                a0 = _mm_loadu_ps(reinterpret_cast<const float *>(data + addr));
                a1 = _mm_loadu_ps(reinterpret_cast<const float *>(data + addr + 16u));
                a2 = _mm_loadu_ps(reinterpret_cast<const float *>(data + addr + 32u));
                a3 = _mm_loadu_ps(reinterpret_cast<const float *>(data + addr + 48u));
                r.vi[4] = static_cast<int16_t>(r.vi[4] + 4);
            }
            else
            {
                lqi(r, 3, 4, data, size);
                lqi(r, 4, 4, data, size);
                lqi(r, 5, 4, data, size);
                lqi(r, 6, 4, data, size);
                a0 = load(r.vf[3]);
                a1 = load(r.vf[4]);
                a2 = load(r.vf[5]);
                a3 = load(r.vf[6]);
            }
        };
        vu1v::V m0, m1, m2, m3;
        loadFour(m0, m1, m2, m3);
        // [perf] as matrixTimes3From: the broadcasts of vf1 / vf2 / vf0.w (not written in the loop) cleaned once, vf31
        // (0) cleaned once, ACC kept in a register and stored at the end - the same operations (PS2X_VU0_VERIFY=1)
        const vu1v::V b1x = splat(vu1fast::operand(r.vf[1][0])), b1y = splat(vu1fast::operand(r.vf[1][1])),
                      b1z = splat(vu1fast::operand(r.vf[1][2])), b2x = splat(vu1fast::operand(r.vf[2][0])),
                      b2y = splat(vu1fast::operand(r.vf[2][1])), b2z = splat(vu1fast::operand(r.vf[2][2])),
                      one = splat(vu1fast::operand(r.vf[0][3])), zero = operand(load(r.vf[31]));
        vu1v::V acc = load(r.acc);
        vu1v::V v7{}, v8{}, v9{}, v10{}, lo0{}, lo1{}, lo2{}, hi0{}, hi1{}, hi2{}, v17{}, v18{};
        do
        {
            r.vi[1] = static_cast<int16_t>(r.vi[1] << 4);                   // 0d40..0d58 IADD vi1, vi1, vi1 (x4)
            const vu1v::V v3 = operand(m0), v4 = operand(m1), v5 = operand(m2);
            acc = operand(vu1v::mul(v3, b1x));                              // 0d40 MULAx  acc = vf3 * vf1x
            acc = fused<0xFu, false>(operand(acc), v4, b1y);                // 0d48 MADDAy
            acc = fused<0xFu, false>(operand(acc), v5, b1z);                // 0d50 MADDAz
            v7 = fused<0xFu, false>(operand(acc), operand(m3), one);        // 0d58 MADDw vf7 = acc + vf6 * 1
            v8 = operand(vu1v::mul(v3, b2x));                               // 0d60 MULx vf8
            v9 = operand(vu1v::mul(v4, b2y));                               // 0d68 MULy vf9
            v10 = operand(vu1v::mul(v5, b2z));                              // 0d70 MULz vf10
            r.vi[2] = static_cast<int16_t>(r.vi[2] << 4);                   // 0d60..0d78 IADD vi2, vi2, vi2 (x4)
            lo0 = vu1v::min(operand(v8), zero);
            lo1 = vu1v::min(operand(v9), zero);
            lo2 = vu1v::min(operand(v10), zero);
            hi0 = vu1v::max(operand(v8), zero);
            hi1 = vu1v::max(operand(v9), zero);
            hi2 = vu1v::max(operand(v10), zero);
            const vu1v::V base = operand(vu1v::mul(operand(v7), one));      // 0db0 / 0dd0 MULAw acc = vf7
            loadFour(m0, m1, m2, m3);                                       // | LQI vf3..vf6 (next set)
            acc = fused<0xFu, false>(operand(base), operand(lo0), one);     // 0db8 MADDAw
            acc = fused<0xFu, false>(operand(acc), operand(lo1), one);      // 0dc0 MADDAw
            v17 = fused<0xFu, false>(operand(acc), operand(lo2), one);      // 0dc8 MADDw vf17 (most negative corner)
            acc = fused<0xFu, false>(operand(base), operand(hi0), one);     // 0dd8 MADDAw
            acc = fused<0xFu, false>(operand(acc), operand(hi1), one);      // 0de0 MADDAw
            v18 = fused<0xFu, false>(operand(acc), operand(hi2), one);      // 0de8 MADDw vf18 (most positive corner)
            r.vi[6] = static_cast<int16_t>(r.vi[5] & signFlags(v17));       // 0de8 FMAND: flags of 0dc8 (4 pairs earlier)
            r.vi[1] = static_cast<int16_t>(r.vi[1] | r.vi[6]);              // 0df0 IOR
            r.vi[3] = static_cast<int16_t>(r.vi[3] - 1);                    // 0df8 ISUBIU
            r.vi[7] = static_cast<int16_t>(r.vi[5] & signFlags(v18));       // 0e08 FMAND: flags of 0de8
            r.vi[2] = static_cast<int16_t>(r.vi[2] | r.vi[7]);              // 0e18 IOR (delay slot of 0e10 IBNE)
        } while (r.vi[3] != 0);
        store<0xFu>(r.vf[3], m0); store<0xFu>(r.vf[4], m1); store<0xFu>(r.vf[5], m2); store<0xFu>(r.vf[6], m3);
        store<0xFu>(r.vf[7], v7); store<0xFu>(r.vf[8], v8); store<0xFu>(r.vf[9], v9); store<0xFu>(r.vf[10], v10);
        store<0xFu>(r.vf[11], lo0); store<0xFu>(r.vf[12], lo1); store<0xFu>(r.vf[13], lo2);
        store<0xFu>(r.vf[14], hi0); store<0xFu>(r.vf[15], hi1); store<0xFu>(r.vf[16], hi2);
        store<0xFu>(r.vf[17], v17); store<0xFu>(r.vf[18], v18);
        store<0xFu>(r.acc, acc);
        storeRegs(r, ctx);
    }

    // VU0 0x0C10: a sphere against three plane sets (the sphere variant of boxClip). In: vf1 = centre (xyz) and
    // radius (w), vi15 = VU address of the matrices (at vi15+24). Out: vi1 / vi2 = per set the sign flags of
    // radius - distance / distance + radius (4 bits each, shifted in by 4 per set). 18.8k calls / 5 s while driving.
    void sphereClip(R5900Context *ctx, uint8_t *data, uint32_t size)
    {
        Vu0Regs r;
        loadRegs(ctx, r);
        r.vi[4] = static_cast<int16_t>(r.vi[15] + 24);                     // 0c10 IADDIU vi4, vi15, 24
        store<0xFu>(r.vf[31], mulBc(r.vf[0], r.vf[0], 0));                 // 0c18 MULx vf31, vf0, vf0x | IADD vi1 = 0
        r.vi[1] = 0;
        store<0xFu>(r.vf[31], operand(vu1v::add(operand(load(r.vf[31])), splat(vu1fast::operand(r.vf[0][3]))))); // 0c20 ADDw vf31 (= 1) | vi2 = 0
        r.vi[2] = 0;
        r.vi[3] = 3;                                                        // 0c28
        r.vi[5] = 240;                                                      // 0c30
        do
        {
            lqi(r, 2, 4, data, size);                                       // 0c38..0c50
            lqi(r, 3, 4, data, size);
            lqi(r, 4, 4, data, size);
            lqi(r, 5, 4, data, size);
            r.vi[1] = static_cast<int16_t>(r.vi[1] << 4);                   // 0c58..0c70 IADD vi1, vi1, vi1 (x4)
            store<0xFu>(r.acc, mulBc(r.vf[2], r.vf[1], 0));                 // 0c58 MULAx
            store<0xFu>(r.acc, maddBc(r.acc, r.vf[3], r.vf[1], 1));         // 0c60 MADDAy
            store<0xFu>(r.acc, maddBc(r.acc, r.vf[4], r.vf[1], 2));         // 0c68 MADDAz
            store<0xFu>(r.vf[6], maddBc(r.acc, r.vf[5], r.vf[0], 3));       // 0c70 MADDw vf6 = distances
            r.vi[2] = static_cast<int16_t>(r.vi[2] << 4);                   // 0c78..0c90 IADD vi2, vi2, vi2 (x4)
            store<0xFu>(r.acc, mulBc(r.vf[31], r.vf[1], 3));                // 0c78 MULAw acc = radius
            store<0xFu>(r.vf[7], fused<0xFu, true>(operand(load(r.acc)), operand(load(r.vf[6])), splat(vu1fast::operand(r.vf[0][3])))); // 0c80 MSUBw vf7
            store<0xFu>(r.acc, mulBc(r.vf[6], r.vf[0], 3));                 // 0c98 MULAw acc = vf6
            store<0xFu>(r.vf[8], maddBc(r.acc, r.vf[31], r.vf[1], 3));      // 0ca0 MADDw vf8 = vf6 + radius
            r.vi[6] = static_cast<int16_t>(r.vi[5] & signFlags(r.vf[7]));   // 0ca0 FMAND: flags of 0c80 (4 pairs earlier)
            r.vi[1] = static_cast<int16_t>(r.vi[1] | r.vi[6]);              // 0ca8 IOR
            r.vi[3] = static_cast<int16_t>(r.vi[3] - 1);                    // 0cb0 ISUBIU
            r.vi[6] = static_cast<int16_t>(r.vi[5] & signFlags(r.vf[8]));   // 0cc0 FMAND: flags of 0ca0
            r.vi[2] = static_cast<int16_t>(r.vi[2] | r.vi[6]);              // 0cd0 IOR (delay slot of 0cc8 IBNE)
        } while (r.vi[3] != 0);
        storeRegs(r, ctx);
    }

    // VU0 0x0E30: vf1..vf12 stored to vi15+0 .. vi15+11 (the last in the E bit's delay slot)
    void storeTwelve(R5900Context *ctx, uint8_t *data, uint32_t size)
    {
        const uint32_t base = static_cast<uint16_t>(ctx->vi[15]);
        for (uint32_t n = 0; n < 12u; ++n)
        {
            float v[4];
            _mm_storeu_ps(v, ctx->vu0_vf[1 + n]);
            std::memcpy(data + (((base + n) * 16u) & (size - 1u)), v, 16);
        }
    }

    // VU0 0x0EB8: three 4x4 matrix products. In: vf1..vf4 = vectors, vi15 = VU address; matrices at vi15+12,
    // results to vi15+24 (12 quadwords). 0x0E98 runs the same loop on the matrices at vi15 into vi15+12.
    void matrixTimes3From(R5900Context *ctx, uint8_t *data, uint32_t size, int16_t from, int16_t to)
    {
        Vu0Regs r;
        loadRegs(ctx, r);
        r.vi[1] = static_cast<int16_t>(r.vi[15] + from);                    // 0eb8 / 0e98
        r.vi[2] = static_cast<int16_t>(r.vi[15] + to);                      // 0ec0 / 0ea0
        r.vi[3] = 3;                                                        // 0ec8 / 0eb0 (delay slot of 0ea8 B)
        // [perf] the same operations as mulBc / maddBc, with what repeats taken out of the loop: the inputs vf1..vf4
        // (not written here) as cleaned broadcasts once per call, each matrix row cleaned once per matrix, ACC kept in
        // a register (still cleaned as an operand each time) and stored once at the end (bit-identical:
        // PS2X_VU0_VERIFY=1; PERFORMANCE.md 22)
        vu1v::V in[4][4];
        for (int v = 0; v < 4; ++v)
            for (int c = 0; c < 4; ++c)
                in[v][c] = splat(vu1fast::operand(r.vf[1 + v][c]));
        vu1v::V acc = load(r.acc);
        vu1v::V m0{}, m1{}, m2{}, m3{}, o0{}, o1{}, o2{}, o3{};
        do
        {
            const uint32_t a = (static_cast<uint32_t>(static_cast<uint16_t>(r.vi[1])) * 16u) & (size - 1u);
            if (a + 64u <= size)
            {
                m0 = operand(_mm_loadu_ps(reinterpret_cast<const float *>(data + a)));
                m1 = operand(_mm_loadu_ps(reinterpret_cast<const float *>(data + a + 16u)));
                m2 = operand(_mm_loadu_ps(reinterpret_cast<const float *>(data + a + 32u)));
                m3 = operand(_mm_loadu_ps(reinterpret_cast<const float *>(data + a + 48u)));
                r.vi[1] = static_cast<int16_t>(r.vi[1] + 4);
            }
            else
            {
                lqi(r, 5, 1, data, size);
                lqi(r, 6, 1, data, size);
                lqi(r, 7, 1, data, size);
                lqi(r, 8, 1, data, size);
                m0 = operand(load(r.vf[5]));
                m1 = operand(load(r.vf[6]));
                m2 = operand(load(r.vf[7]));
                m3 = operand(load(r.vf[8]));
            }
            vu1v::V out[4];
            for (int v = 0; v < 4; ++v) // 0ef0..0f68: vf13+v = M * vf(1+v)
            {
                acc = operand(vu1v::mul(m0, in[v][0]));                     // MULAx
                acc = fused<0xFu, false>(operand(acc), m1, in[v][1]);       // MADDAy
                acc = fused<0xFu, false>(operand(acc), m2, in[v][2]);       // MADDAz
                out[v] = fused<0xFu, false>(operand(acc), m3, in[v][3]);    // MADDw
            }
            o0 = out[0], o1 = out[1], o2 = out[2], o3 = out[3];
            r.vi[3] = static_cast<int16_t>(r.vi[3] - 1);                    // 0f68 ISUBIU
            const uint32_t dst = (static_cast<uint32_t>(static_cast<uint16_t>(r.vi[2])) * 16u) & (size - 1u);
            if (dst + 64u <= size)
            {
                _mm_storeu_ps(reinterpret_cast<float *>(data + dst), o0);
                _mm_storeu_ps(reinterpret_cast<float *>(data + dst + 16u), o1);
                _mm_storeu_ps(reinterpret_cast<float *>(data + dst + 32u), o2);
                _mm_storeu_ps(reinterpret_cast<float *>(data + dst + 48u), o3);
                r.vi[2] = static_cast<int16_t>(r.vi[2] + 4);
            }
            else
            {
                store<0xFu>(r.vf[13], o0);
                store<0xFu>(r.vf[14], o1);
                store<0xFu>(r.vf[15], o2);
                store<0xFu>(r.vf[16], o3);
                sqi(r, 13, 2, data, size);
                sqi(r, 14, 2, data, size);
                sqi(r, 15, 2, data, size);
                sqi(r, 16, 2, data, size);
            }
        } while (r.vi[3] != 0);
        store<0xFu>(r.vf[5], m0);
        store<0xFu>(r.vf[6], m1);
        store<0xFu>(r.vf[7], m2);
        store<0xFu>(r.vf[8], m3);
        store<0xFu>(r.vf[13], o0);
        store<0xFu>(r.vf[14], o1);
        store<0xFu>(r.vf[15], o2);
        store<0xFu>(r.vf[16], o3);
        store<0xFu>(r.acc, acc);
        storeRegs(r, ctx);
    }

    void matrixTimes3(R5900Context *ctx, uint8_t *data, uint32_t size) { matrixTimes3From(ctx, data, size, 12, 24); }
    void matrixTimes3First(R5900Context *ctx, uint8_t *data, uint32_t size) { matrixTimes3From(ctx, data, size, 0, 12); }

    // ---- the collision overlay (.vutext MPG at 0x48055C -> VU0 0x000, 245 pairs; game-internals.md 6c) ----------
    // Two programs ran ~22% of the EE thread interpreted (2026-10-23): 0x020 line segment vs triangle and 0x3A0 sphere
    // vs triangle (with the subroutine at 0x228). Both read MAC / clip flags, so the interpreter ran them on its exact
    // path. Their code is padded with NOPs so that every flag read sees one known instruction: FMAND sees the MAC
    // flags of the FMAC 4 pairs before it, FCAND the CLIPs issued 4+ pairs before it, Q is read after WAITQ or 7+
    // pairs after the division; a pair's LOI sets I for the following pairs; a lower MOVE reads its source before the
    // same pair's upper write. Their I, Q and clip flags are left as the interpreter leaves them.
    struct CollisionVu
    {
        Vu0Regs r;
        float q, i;
        uint32_t clip;

        explicit CollisionVu(R5900Context *ctx) : q(ctx->vu0_q), i(ctx->vu0_i), clip(ctx->vu0_clip_flags) { loadRegs(ctx, r); }
        CollisionVu(float (*vfPtr)[4], float *accPtr) : q(0.0f), i(0.0f), clip(0u)
        {
            r.vf = vfPtr;
            r.acc = accPtr;
            r.vf[0][0] = 0.0f;
            r.vf[0][1] = 0.0f;
            r.vf[0][2] = 0.0f;
            r.vf[0][3] = 1.0f;
        }
        void finish(R5900Context *ctx) const
        {
            if (!ctx)
                return;
            storeRegs(r, ctx);
            ctx->vu0_q = q;
            ctx->vu0_i = i;
            ctx->vu0_clip_flags = clip;
            ctx->vu0_clip_flags2 = clip;
        }

        vu1v::V vf(int n) const { return operand(load(r.vf[n])); }
        vu1v::V lane(int n, int c) const { return splat(vu1fast::operand(r.vf[n][c])); }
        vu1v::V acc() const { return operand(load(r.acc)); }
        static vu1v::V add(vu1v::V a, vu1v::V b) { return operand(vu1v::add(a, b)); }
        static vu1v::V sub(vu1v::V a, vu1v::V b) { return operand(vu1v::sub(a, b)); }
        static vu1v::V mul(vu1v::V a, vu1v::V b) { return operand(vu1v::mul(a, b)); }
        // (x, y, z) -> (y, z, x) and (z, x, y): OPMULA / OPMSUB operands
        static vu1v::V yzx(vu1v::V v) { return vu1v::yzx(v); }
        static vu1v::V zxy(vu1v::V v) { return vu1v::zxy(v); }

        void itof0(int n)
        {
            store<0xFu>(r.vf[n], vu1v::intToFloat(load(r.vf[n])));
        }
        // ACC.w / dest.w = the sum of v.xyz (MULAx.w, MADDAy.w, MADDz.w with vf0.w = 1)
        void dotW(int dest, int v)
        {
            const __m128 one = _mm_set1_ps(1.0f);
            const vu1v::V x = operand(vu1v::mul(one, splat(vu1fast::operand(r.vf[v][0]))));
            const vu1v::V y = fused<0x8u, false>(x, one, splat(vu1fast::operand(r.vf[v][1])));
            const vu1v::V z = fused<0x8u, false>(y, one, splat(vu1fast::operand(r.vf[v][2])));
            store<0x8u>(r.acc, y);
            store<0x8u>(r.vf[dest], z);
        }
        // the MAC sign flags FMAND sees for a result written to `lanes` (bit 0 = x ... bit 3 = w): x 0x80 ... w 0x10
        static uint16_t vecSigns(const float *p, unsigned lanes)
        {
            static constexpr uint16_t kLut[16] = {
                0x00, 0x80, 0x40, 0xC0, 0x20, 0xA0, 0x60, 0xE0,
                0x10, 0x90, 0x50, 0xD0, 0x30, 0xB0, 0x70, 0xF0,
            };
            return kLut[static_cast<unsigned>(_mm_movemask_ps(_mm_loadu_ps(p))) & lanes];
        }
        uint16_t signs(int n, unsigned lanes) const { return vecSigns(r.vf[n], lanes); }
        uint16_t accSigns(unsigned lanes) const { return vecSigns(r.acc, lanes); }
        // CLIPw.xyz fs, ft.w (as VU1Interpreter: the bit patterns against |w|), shifted into the clip flags
        void clipW(int fs, int ft)
        {
            uint32_t wBits;
            std::memcpy(&wBits, &r.vf[ft][3], 4);
            const int32_t limit = (wBits & 0x7F800000u) != 0u ? static_cast<int32_t>(wBits & 0x7FFFFFFFu) : 0x007FFFFF;
            const __m128i bits = _mm_loadu_si128(reinterpret_cast<const __m128i *>(r.vf[fs]));
            const __m128i vlimit = _mm_set1_epi32(limit);
            const __m128i signFlip = _mm_set1_epi32(static_cast<int>(0x80000000u));
            const unsigned posMask = static_cast<unsigned>(_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpgt_epi32(bits, vlimit)))) & 7u;
            const unsigned negMask =
                static_cast<unsigned>(_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpgt_epi32(_mm_xor_si128(bits, signFlip), vlimit)))) & 7u;
            static constexpr uint8_t kPos[8] = {0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15};
            static constexpr uint8_t kNeg[8] = {0x00, 0x02, 0x08, 0x0A, 0x20, 0x22, 0x28, 0x2A};
            const uint32_t flags = static_cast<uint32_t>(kPos[posMask] | kNeg[negMask]);
            clip = ((clip << 6) | flags) & 0xFFFFFFu;
        }
        int16_t fcand(uint32_t mask) const { return (clip & mask) != 0u ? 1 : 0; }
        static float maxSigned(bool negative) { return negative ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max(); }
        void div(int fs, int fsf, int ft, int ftf)
        {
            const float num = vu1fast::operand(r.vf[fs][fsf]), den = vu1fast::operand(r.vf[ft][ftf]);
            q = vu1fast::result(den == 0.0f ? maxSigned(std::signbit(num) != std::signbit(den)) : num / den);
        }
        void sqrtQ(int ft, int ftf) { q = vu1fast::result(std::sqrt(std::fabs(vu1fast::operand(r.vf[ft][ftf])))); }
        void rsqrt(int fs, int fsf, int ft, int ftf)
        {
            const float num = vu1fast::operand(r.vf[fs][fsf]), den = std::sqrt(std::fabs(vu1fast::operand(r.vf[ft][ftf])));
            q = vu1fast::result(den != 0.0f ? num / den : maxSigned(std::signbit(num)));
        }
        vu1v::V qv() const { return splat(vu1fast::operand(q)); }
        void move(int dst, int src) { std::memcpy(r.vf[dst], r.vf[src], 16); }

        // 0x000..0x018: the exits (vi1 = 0 / 1)
        void exit(int16_t vi1) { r.vi[1] = vi1; }

        // the fixed-point inputs (0x020..0x060 / 0x3A0..0x3E0): vf17.xyz in 1/4096, the rest in 1/128
        void convertInputs()
        {
            const float i4096 = vu1fast::operand(vu1fast::fromBits(0x397FFFD5u));
            i = 0.0078125f;
            const __m128 scale17 = _mm_setr_ps(i4096, i4096, i4096, i);
            const __m128 i128 = splat(i);
            const __m128 v17 = _mm_mul_ps(vu1v::intToFloat(load(r.vf[17])), scale17);
            const __m128 v14 = _mm_mul_ps(vu1v::intToFloat(load(r.vf[14])), i128);
            const __m128 v15 = _mm_mul_ps(vu1v::intToFloat(load(r.vf[15])), i128);
            const __m128 v16 = _mm_mul_ps(vu1v::intToFloat(load(r.vf[16])), i128);
            store<0xFu>(r.vf[17], operand(v17));
            store<0xFu>(r.vf[14], operand(v14));
            store<0xFu>(r.vf[15], operand(v15));
            store<0xFu>(r.vf[16], operand(v16));
        }

        // 0x228: the point of segment vf14 + t * vf15 nearest to vf12 (clamped to the segment) -> vf16.xyz, the
        // squared distance -> vf16.w
        void nearestOnSegment()
        {
            store<0x7u>(r.vf[20], sub(vf(12), vf(14)));                     // 0228 SUB vf20
            store<0x7u>(r.vf[21], mul(vf(15), vf(15)));                     // 0230 MUL vf21
            store<0x7u>(r.acc, add(vf(14), vf(15)));                        // 0238 ADDA
            store<0x7u>(r.vf[25], fused<0x7u, true>(acc(), vf(12), lane(0, 3))); // 0240 MSUBw vf25
            store<0x7u>(r.vf[22], mul(vf(20), vf(20)));                     // 0248 MUL vf22
            store<0x7u>(r.vf[23], mul(vf(20), vf(15)));                     // 0250 MUL vf23
            dotW(21, 21);                                                   // 0258..0268
            dotW(23, 23);                                                   // 0270..0280
            const uint16_t mac0280 = signs(23, 0x8u);
            dotW(22, 22);                                                   // 0288..0298 | IADDIU vi8, 16
            r.vi[8] = 16;
            store<0x7u>(r.vf[25], mul(vf(25), vf(25)));                     // 02a0 MUL vf25 | FMAND vi8 (flags of 0280)
            r.vi[8] = static_cast<int16_t>(r.vi[8] & mac0280);
            div(23, 3, 21, 3);                                              // 02a8 DIV Q, vf23.w, vf21.w
            if (r.vi[8] != 0)                                               // 02b0 IBNE vi0, vi8 -> 0350: t < 0
            {
                store<0x7u>(r.vf[16], add(vf(0), vf(14)));                  // 0350 ADD vf16 = start
                store<0x8u>(r.vf[16], mul(vf(0), lane(22, 3)));             // 0358 MUL.w vf16.w
                return;                                                     // 0360 JR vi15
            }
            store<0x7u>(r.acc, add(vf(0), vf(14)));                         // 02c0 ADDA
            store<0x7u>(r.vf[16], fused<0x7u, false>(acc(), vf(15), qv())); // 02c8 MADDq vf16 | WAITQ
            store<0x1u>(r.vf[24], add(vf(0), qv()));                        // 02d0 ADDq.x vf24
            store<0x7u>(r.vf[26], sub(vf(16), vf(12)));                     // 02e8 SUB vf26
            clipW(24, 0);                                                   // 02f0 CLIPw vf24 vs 1
            store<0x7u>(r.vf[26], mul(vf(26), vf(26)));                     // 0308 MUL vf26
            r.vi[1] = fcand(0x1u);                                          // 0310 FCAND vi1, 1: t > 1
            if (r.vi[1] != 0)                                               // 0318 IBNE vi0, vi1 -> 0370
            {
                dotW(16, 25);                                               // 0370..0380 vf16.w = |end - centre|^2
                store<0x7u>(r.vf[16], add(vf(14), vf(15)));                 // 0388 ADD vf16 = end
                return;                                                     // 0390 JR vi15
            }
            dotW(16, 26);                                                   // 0328..0338
        }                                                                   // 0340 JR vi15
    };

    // VU0 0x020 (collision overlay): segment vf17 (start, w = ?) .. vf13 against the triangle vf14 / vf15 / vf16 (fixed
    // point, converted here). vi1 = 1 when it crosses, with the crossing in vf1 and the segment's direction in vf2.
    void segmentTriangleFrom0068(CollisionVu &v, R5900Context *ctx);

    void segmentTriangle(R5900Context *ctx, uint8_t *, uint32_t)
    {
        CollisionVu v(ctx);
        v.convertInputs();                                                  // 0020..0060
        segmentTriangleFrom0068(v, ctx);
    }

    // VU0 0x068: the same test entered after the conversion (inputs already floats; 5.8k calls / 5 s while driving)
    void segmentTriangleFloat(R5900Context *ctx, uint8_t *, uint32_t)
    {
        CollisionVu v(ctx);
        segmentTriangleFrom0068(v, ctx);
    }

    void segmentTriangleFrom0068(CollisionVu &v, R5900Context *ctx)
    {
        Vu0Regs &r = v.r;
        using CV = CollisionVu;
        store<0x7u>(r.vf[10], CV::mul(v.vf(17), v.vf(12)));                 // 0068 MUL vf10 | LOI 0.5
        v.i = 0.5f;
        store<0x7u>(r.vf[11], CV::mul(v.vf(17), v.vf(13)));                 // 0070 MUL vf11
        store<0x7u>(r.vf[2], CV::sub(v.vf(13), v.vf(12)));                  // 0078 SUB vf2
        store<0x7u>(r.vf[17], CV::add(v.vf(17), v.vf(0)));                  // 0080 ADD vf17 (sets its sign flags)
        const uint16_t mac0080 = v.signs(17, 0x7u);
        store<0x8u>(r.vf[3], CV::mul(v.vf(0), splat(v.i)));                 // 0088 MULi.w vf3
        store<0x8u>(r.acc, CV::mul(v.vf(0), v.lane(10, 0)));                // 0090 MULAx.w
        store<0x8u>(r.acc, fused<0x8u, false>(v.acc(), v.vf(0), v.lane(10, 1))); // 0098 MADDAy.w | IADDIU vi6, 224
        r.vi[6] = 224;
        store<0x8u>(r.vf[10], fused<0x8u, false>(v.acc(), v.vf(0), v.lane(10, 2))); // 00a0 MADDz.w vf10 | FMAND vi5
        r.vi[5] = static_cast<int16_t>(r.vi[6] & mac0080);
        v.dotW(11, 11);                                                     // 00a8..00b8
        store<0x8u>(r.vf[9], CV::sub(v.vf(17), v.vf(10)));                  // 00c0 SUB.w vf9
        const uint16_t mac00c0 = v.signs(9, 0x8u);
        v.clipW(17, 3);                                                     // 00c8 CLIPw vf17 vs vf3.w
        r.vi[2] = 16;                                                       // 00d0
        store<0x8u>(r.acc, CV::sub(v.vf(17), v.vf(11)));                    // 00d8 SUBA.w
        const uint16_t mac00d8 = v.accSigns(0x8u);
        store<0x8u>(r.vf[8], CV::sub(v.vf(11), v.vf(10)));                  // 00e0 SUB.w vf8 | FMAND vi1 (flags of 00c0)
        r.vi[1] = static_cast<int16_t>(r.vi[2] & mac00c0);
        r.vi[2] = static_cast<int16_t>(r.vi[2] & mac00d8);                  // 00f8 FMAND vi2 (flags of 00d8)
        if (r.vi[1] == r.vi[2])                                             // 0100 IBEQ vi1, vi2 -> 0000: both ends on one side
            return v.exit(0), v.finish(ctx);
        v.div(9, 3, 8, 3);                                                  // 0110 DIV Q, vf9.w, vf8.w
        r.vi[1] = v.fcand(0x03u);                                           // 0118 FCAND
        r.vi[2] = static_cast<int16_t>((r.vi[1] + 127) & 128);              // 0120..0130
        r.vi[6] = 128;
        r.vi[1] = v.fcand(0x0Cu);                                           // 0138 FCAND
        r.vi[3] = static_cast<int16_t>(r.vi[1] + 63);                       // 0140
        store<0x7u>(r.acc, CV::mul(v.vf(12), v.lane(0, 3)));                // 0148 MULAw.xyz | IADDIU vi6, 64
        r.vi[6] = 64;
        store<0x7u>(r.vf[1], fused<0x7u, false>(v.acc(), v.vf(2), v.qv())); // 0150 MADDq vf1 | IAND vi3
        r.vi[3] = static_cast<int16_t>(r.vi[3] & r.vi[6]);
        store<0x8u>(r.vf[1], CV::mul(v.vf(0), v.lane(0, 0)));               // 0158 MULx.w vf1 | FCAND
        r.vi[1] = v.fcand(0x30u);
        store<0x1u>(r.vf[3], CV::add(v.vf(0), v.qv()));                     // 0160 ADDq.x vf3 | IADDIU vi4
        r.vi[4] = static_cast<int16_t>(r.vi[1] + 31);
        store<0x7u>(r.vf[5], CV::sub(v.vf(15), v.vf(14)));                  // 0168 | IADDIU vi6, 32
        r.vi[6] = 32;
        store<0x7u>(r.vf[8], CV::sub(v.vf(1), v.vf(14)));                   // 0170 | IAND vi4
        r.vi[4] = static_cast<int16_t>(r.vi[4] & r.vi[6]);
        store<0x7u>(r.vf[6], CV::sub(v.vf(16), v.vf(15)));                  // 0178 | IADD vi6, vi2, vi3
        r.vi[6] = static_cast<int16_t>(r.vi[2] + r.vi[3]);
        store<0x7u>(r.vf[9], CV::sub(v.vf(1), v.vf(15)));                   // 0180 | IADD vi6, vi6, vi4
        r.vi[6] = static_cast<int16_t>(r.vi[6] + r.vi[4]);
        store<0x7u>(r.vf[7], CV::sub(v.vf(14), v.vf(16)));                  // 0188
        store<0x7u>(r.vf[10], CV::sub(v.vf(1), v.vf(16)));                  // 0190
        // the three edge cross products (OPMULA / OPMSUB): their sign flags say on which side of each edge vf1 is
        uint16_t mac[3];
        for (int e = 0; e < 3; ++e)                                         // 0198..01c0
        {
            const int a = 8 + e, b = 5 + e;
            store<0x7u>(r.acc, CV::mul(CV::yzx(v.vf(a)), CV::zxy(v.vf(b))));
            store<0x7u>(r.vf[18 + e], fused<0x7u, true>(v.acc(), CV::yzx(v.vf(b)), CV::zxy(v.vf(a))));
            mac[e] = v.signs(18 + e, 0x7u);
        }
        r.vi[2] = static_cast<int16_t>(r.vi[6] & mac[0]);                   // 01c0 FMAND (flags of 01a0)
        r.vi[3] = static_cast<int16_t>(r.vi[6] & mac[1]);                   // 01d0 FMAND (flags of 01b0)
        r.vi[4] = static_cast<int16_t>(r.vi[6] & mac[2]);                   // 01e0 FMAND (flags of 01c0)
        if (r.vi[3] != r.vi[2] || r.vi[4] != r.vi[2])                       // 01f0 / 0200 IBNE -> 0000
            return v.exit(0), v.finish(ctx);
        store<0x7u>(r.vf[2], CV::mul(v.vf(17), v.lane(0, 3)));              // 0210 MULw vf2 | IADDIU vi1, 1
        r.vi[1] = 1;
        v.finish(ctx);
    }

    // VU0 0x3A0 (collision overlay): sphere vf12 (centre, w = radius) against the triangle vf14 / vf15 / vf16 with
    // normal vf17 (fixed point). vi1 = 1 when they touch, with the contact point in vf1 and the push-out direction /
    // depth in vf2 / vf3.
    template <bool NeedOutputs>
    bool sphereTriangleCore(CollisionVu &v, R5900Context *ctx)
    {
        Vu0Regs &r = v.r;
        using CV = CollisionVu;
        v.convertInputs();                                                  // 03a0..03e0
        store<0x7u>(r.vf[2], CV::mul(v.vf(12), v.vf(17)));                  // 03e8 MUL vf2 | LOI 0.1
        v.i = vu1fast::fromBits(0x3DCCCCCDu);
        store<0x7u>(r.vf[17], CV::add(v.vf(17), v.vf(0)));                  // 03f0 ADD vf17 (sets its sign flags)
        const uint16_t mac03f0 = v.signs(17, 0x7u);
        store<0x1u>(r.vf[13], CV::add(v.vf(0), v.lane(12, 3)));             // 03f8 ADDw.x vf13 = radius
        store<0x8u>(r.acc, CV::mul(v.vf(0), v.lane(2, 0)));                 // 0408 MULAx.w | IADDIU vi6, 224
        r.vi[6] = 224;
        store<0x8u>(r.acc, fused<0x8u, false>(v.acc(), v.vf(0), v.lane(2, 1))); // 0410 MADDAy.w | FMAND vi5 (03f0)
        r.vi[5] = static_cast<int16_t>(r.vi[6] & mac03f0);
        store<0x8u>(r.acc, fused<0x8u, false>(v.acc(), v.vf(0), v.lane(2, 2))); // 0418 MADDAz.w
        store<0x8u>(r.vf[2], fused<0x8u, true>(v.acc(), v.vf(0), v.vf(17))); // 0420 MSUB.w vf2: signed distance
        if constexpr (NeedOutputs)
            v.move(4, 3);                                                   // 0428 MOVE vf4, vf3 (before the upper)
        store<0x8u>(r.vf[3], CV::mul(v.vf(0), splat(vu1fast::operand(v.i)))); // 0428 MULi.w vf3 = 0.1
        v.clipW(13, 2);                                                     // 0440 CLIPw vf13 vs vf2.w
        const uint32_t clip0440 = v.clip;
        v.clipW(17, 3);                                                     // 0448 CLIPw vf17 vs 0.1
        store<0x7u>(r.acc, CV::mul(v.vf(12), v.lane(0, 3)));                // 0450 MULAw.xyz | IADDIU vi7, 0
        r.vi[7] = 0;
        store<0x7u>(r.vf[1], fused<0x7u, true>(v.acc(), v.vf(17), v.lane(2, 3))); // 0458 MSUBw vf1: centre on the plane
        store<0x8u>(r.vf[1], CV::mul(v.vf(0), v.lane(0, 0)));               // 0460 MULx.w vf1 | FCAND (sees 0440 only)
        r.vi[1] = (clip0440 & 0x3u) != 0u ? 1 : 0;
        store<0x8u>(r.vf[2], vu1v::abs(v.vf(2)));                           // 0468 ABS.w vf2 | IBEQ vi0, vi1 -> 0000
        if (r.vi[1] == 0)
            return v.exit(0), v.finish(ctx), false;
        r.vi[1] = v.fcand(0x03u);                                           // 0478 FCAND
        store<0x7u>(r.vf[2], CV::sub(v.vf(12), v.vf(1)));                   // 0480 | IADDIU vi2
        r.vi[2] = static_cast<int16_t>(r.vi[1] + 127);
        r.vi[6] = 128;                                                      // 0488
        store<0x7u>(r.vf[5], CV::sub(v.vf(15), v.vf(14)));                  // 0490 | IAND vi2
        r.vi[2] = static_cast<int16_t>(r.vi[2] & r.vi[6]);
        store<0x7u>(r.vf[8], CV::sub(v.vf(1), v.vf(14)));                   // 0498 | FCAND
        r.vi[1] = v.fcand(0x0Cu);
        store<0x7u>(r.vf[6], CV::sub(v.vf(16), v.vf(15)));                  // 04a0 | IADDIU vi3
        r.vi[3] = static_cast<int16_t>(r.vi[1] + 63);
        store<0x7u>(r.vf[9], CV::sub(v.vf(1), v.vf(15)));                   // 04a8 | IADDIU vi6, 64
        r.vi[6] = 64;
        store<0x7u>(r.vf[7], CV::sub(v.vf(14), v.vf(16)));                  // 04b0 | IAND vi3
        r.vi[3] = static_cast<int16_t>(r.vi[3] & r.vi[6]);
        store<0x7u>(r.vf[10], CV::sub(v.vf(1), v.vf(16)));                  // 04b8 | FCAND
        r.vi[1] = v.fcand(0x30u);
        store<0x7u>(r.vf[3], CV::mul(v.vf(2), v.vf(2)));                    // 04c0 MUL vf3 | IADDIU vi4
        r.vi[4] = static_cast<int16_t>(r.vi[1] + 31);
        uint16_t mac[3];
        for (int e = 0; e < 3; ++e)                                         // 04c8..04f0 OPMULA / OPMSUB
        {
            const int a = 8 + e, b = 5 + e;
            store<0x7u>(r.acc, CV::mul(CV::yzx(v.vf(a)), CV::zxy(v.vf(b))));
            if (e == 0)
                r.vi[6] = 32;                                               // 04c8 IADDIU vi6, 32
            store<0x7u>(r.vf[18 + e], fused<0x7u, true>(v.acc(), CV::yzx(v.vf(b)), CV::zxy(v.vf(a))));
            mac[e] = v.signs(18 + e, 0x7u);
            if (e == 0)
                r.vi[4] = static_cast<int16_t>(r.vi[4] & r.vi[6]);          // 04d0 IAND vi4
            if (e == 1)
                r.vi[6] = static_cast<int16_t>(r.vi[2] + r.vi[3]);          // 04e0 IADD vi6
        }
        r.vi[6] = static_cast<int16_t>(r.vi[6] + r.vi[4]);                  // 04e8 IADD vi6
        r.vi[2] = static_cast<int16_t>(r.vi[6] & mac[0]);                   // 04f0 FMAND (04d0)
        if constexpr (NeedOutputs)
            store<0x8u>(r.acc, CV::mul(v.vf(0), v.lane(3, 0)));             // 04f8 MULAx.w | IAND vi5
        r.vi[5] = static_cast<int16_t>(r.vi[5] & r.vi[6]);
        if constexpr (NeedOutputs)
            store<0x8u>(r.acc, fused<0x8u, false>(v.acc(), v.vf(0), v.lane(3, 1))); // 0500 MADDAy.w | FMAND vi3 (04e0)
        r.vi[3] = static_cast<int16_t>(r.vi[6] & mac[1]);
        if constexpr (NeedOutputs)
            store<0x8u>(r.vf[3], fused<0x8u, false>(v.acc(), v.vf(0), v.lane(3, 2))); // 0508 MADDz.w vf3 | IADDIU vi8, 3
        r.vi[8] = 3;
        r.vi[4] = static_cast<int16_t>(r.vi[6] & mac[2]);                   // 0510 FMAND vi4 (04f0)
        if constexpr (NeedOutputs)
            v.rsqrt(0, 3, 3, 3);                                            // 0528 RSQRT Q (delay slot of 0520)
        if (r.vi[2] == r.vi[5])                                             // 0520 IBNE vi2, vi5 -> 0538
        {
            store<0x7u>(r.vf[4], CV::add(v.vf(0), v.vf(16)));               // 0530 | IADDIU vi7, 1
            ++r.vi[7];
        }
        if (r.vi[3] == r.vi[5])                                             // 0538 IBNE vi3, vi5 -> 0550
        {
            store<0x7u>(r.vf[4], CV::add(v.vf(0), v.vf(14)));               // 0548
            ++r.vi[7];
        }
        if (r.vi[4] == r.vi[5])                                             // 0550 IBNE vi4, vi5 -> 0580
        {
            store<0x7u>(r.vf[4], CV::add(v.vf(0), v.vf(15)));               // 0560
            ++r.vi[7];
            if (r.vi[7] == r.vi[8])                                         // 0570 IBEQ vi7, vi8 -> 0720: inside all edges
            {
                if constexpr (NeedOutputs)
                {
                    store<0x1u>(r.vf[3], CV::add(v.vf(0), v.lane(2, 3)));   // 0720 ADDw.x vf3 = depth | WAITQ
                    store<0x7u>(r.vf[2], CV::mul(v.vf(2), v.qv()));         // 0728 MULq vf2
                }
                r.vi[1] = 1;                                                // 0730
                return v.finish(ctx), true;
            }
        }
        store<0x1u>(r.vf[13], CV::mul(v.vf(13), v.vf(13)));                 // 0580 MUL.x vf13 (radius^2) | IADDIU vi8, 2
        r.vi[8] = 2;
        if constexpr (NeedOutputs)
            store<0x7u>(r.vf[2], CV::mul(v.vf(2), v.qv()));                 // 0588 MULq vf2 | WAITQ
        if (r.vi[7] == r.vi[8])                                             // 0590 IBNE vi7, vi8 -> 0648
        {
            // outside one edge: the nearest point of that edge (subroutine 0228), then 0740
            if (r.vi[2] != r.vi[5])                                         // 05a0 IBEQ vi2, vi5 -> 05d8
            {
                v.move(15, 5);                                              // 05b0
                r.vi[15] = 0x5C8 / 8;                                       // 05b8 BAL vi15, 0228
            }
            else if (r.vi[3] != r.vi[5])                                    // 05d8 IBEQ vi3, vi5 -> 0618
            {
                v.move(14, 15);                                             // 05e8
                v.move(15, 6);                                              // 05f0
                r.vi[15] = 0x608 / 8;                                       // 05f8 BAL
            }
            else
            {
                v.move(14, 16);                                             // 0618
                v.move(15, 7);                                              // 0620
                r.vi[15] = 0x638 / 8;                                       // 0628 BAL
            }
            v.nearestOnSegment();
            v.clipW(13, 16);                                                // 0740 CLIPw vf13 vs vf16.w | SQRT Q
            if constexpr (NeedOutputs)
                v.sqrtQ(16, 3);
            if constexpr (NeedOutputs)
            {
                store<0x7u>(r.vf[1], CV::add(v.vf(0), v.vf(16)));           // 0748 ADD vf1 = nearest point
                store<0x7u>(r.vf[2], CV::sub(v.vf(12), v.vf(16)));          // 0750 SUB vf2
            }
            r.vi[1] = v.fcand(0x1u);                                        // 0760 FCAND vi1, 1: r^2 > d^2
            if constexpr (NeedOutputs)
                store<0x1u>(r.vf[3], CV::add(v.vf(0), v.qv()));             // 0768 ADDq.x vf3 = distance | WAITQ
            if (r.vi[1] == 0)                                               // 0770 IBEQ vi0, vi1 -> 0000
                return v.exit(0), v.finish(ctx), false;
            if constexpr (NeedOutputs)
            {
                v.div(0, 3, 3, 0);                                          // 0788 DIV Q = 1 / distance
                store<0x7u>(r.vf[2], CV::mul(v.vf(2), v.qv()));             // 0790 MULq vf2 | WAITQ
            }
            return v.finish(ctx), true;                                     // 0798 E (vi1 = 1 from 0760)
        }
        // outside two edges: the nearest corner vf4
        store<0x7u>(r.vf[5], CV::sub(v.vf(4), v.vf(12)));                   // 0648 SUB vf5
        if constexpr (NeedOutputs)
        {
            store<0x7u>(r.vf[1], CV::add(v.vf(0), v.vf(4)));                // 0650 ADD vf1 = corner
            store<0x7u>(r.vf[2], CV::sub(v.vf(12), v.vf(4)));               // 0658 SUB vf2
        }
        store<0x7u>(r.vf[5], CV::mul(v.vf(5), v.vf(5)));                    // 0668 MUL vf5
        v.dotW(5, 5);                                                       // 0688..0698
        v.clipW(13, 5);                                                     // 06b8 CLIPw vf13 vs vf5.w | SQRT Q
        if constexpr (NeedOutputs)
            v.sqrtQ(5, 3);
        r.vi[1] = v.fcand(0x1u);                                            // 06d8 FCAND vi1, 1
        if constexpr (NeedOutputs)
            store<0x1u>(r.vf[3], CV::add(v.vf(0), v.qv()));                 // 06e0 ADDq.x vf3 | WAITQ
        if (r.vi[1] == 0)                                                   // 06e8 IBEQ vi0, vi1 -> 0000
            return v.exit(0), v.finish(ctx), false;
        if constexpr (NeedOutputs)
        {
            v.div(0, 3, 3, 0);                                              // 0700 DIV Q = 1 / distance
            store<0x7u>(r.vf[2], CV::mul(v.vf(2), v.qv()));                 // 0708 MULq vf2 | WAITQ
        }
        v.finish(ctx);                                                      // 0710 E
        return r.vi[1] != 0;
    }

    void sphereTriangle(R5900Context *ctx, uint8_t *, uint32_t)
    {
        CollisionVu v(ctx);
        sphereTriangleCore<true>(v, ctx);
    }

    inline void loadPackedTriangle(float (*vf)[4], const int16_t *v0, const int16_t *v1, const int16_t *v2,
                                   const int16_t *norm4)
    {
        auto loadTriVert = [](float *dst, const int16_t *src) {
            const __m128i iv = _mm_setr_epi32(src[0], src[1], src[2], (src[2] < 0) ? -1 : 0);
            _mm_storeu_ps(dst, _mm_castsi128_ps(iv));
        };
        loadTriVert(vf[14], v0);
        loadTriVert(vf[15], v1);
        loadTriVert(vf[16], v2);
        const __m128i in = _mm_setr_epi32(norm4[0], norm4[1], norm4[2], norm4[3]);
        _mm_storeu_ps(vf[17], _mm_castsi128_ps(in));
    }

    struct Registrar
    {
        Registrar()
        {
            // the collision overlay's code from 0x000 (exits, subroutine and both programs: 245 pairs)
            ps2RegisterVu0NativeRange(0x0020u, 0x0000u, 0x7A8u, 0xBD552AA3D758E079ull, &segmentTriangle,
                                      "VCS VU0 segment vs triangle (0x020)");
            ps2RegisterVu0NativeRange(0x03A0u, 0x0000u, 0x7A8u, 0xBD552AA3D758E079ull, &sphereTriangle,
                                      "VCS VU0 sphere vs triangle (0x3A0)");
            ps2RegisterVu0Native(0x0CF8u, 0x138u, 0x3679405981306074ull, &boxClip, "VCS VU0 sector box clip (0x0CF8)");
            ps2RegisterVu0Native(0x0EB8u, 0x0F0u, 0xEA4DCB2F62C1EC30ull, &matrixTimes3, "VCS VU0 3x matrix product (0x0EB8)");
            // 2026-10-24: the rest that ran interpreted while driving (PS2X_TRACE_VU0; ~5% of the game thread)
            ps2RegisterVu0NativeRange(0x0068u, 0x0000u, 0x7A8u, 0xBD552AA3D758E079ull, &segmentTriangleFloat,
                                      "VCS VU0 segment vs triangle, float inputs (0x068)");
            ps2RegisterVu0Native(0x0C10u, 0x0D8u, 0x29291AE3B92E1ED5ull, &sphereClip, "VCS VU0 sphere vs three plane sets (0x0C10)");
            ps2RegisterVu0Native(0x0E98u, 0x110u, 0xD8B903EF47C24DF7ull, &matrixTimes3First, "VCS VU0 3x matrix product (0x0E98)");
            ps2RegisterVu0Native(0x0E30u, 0x068u, 0x0EAEDF438E869B5Bull, &storeTwelve, "VCS VU0 store vf1..vf12 (0x0E30)");
        }
    } registrar;
}

namespace vcs::col
{
    bool vuSphereTriangleTest(const __m128 &sphereCenterR, const int16_t *v0, const int16_t *v1, const int16_t *v2,
                              const int16_t *norm4)
    {
        alignas(16) float vf[32][4];
        alignas(16) float acc[4];
        _mm_storeu_ps(vf[12], sphereCenterR);
        loadPackedTriangle(vf, v0, v1, v2, norm4);
        CollisionVu v(vf, acc);
        return sphereTriangleCore<false>(v, nullptr);
    }

    bool vuSphereTriangleHit(const __m128 &sphereCenterR, const int16_t *v0, const int16_t *v1, const int16_t *v2,
                             const int16_t *norm4, __m128 &outPos, __m128 &outNorm, float &outDist)
    {
        alignas(16) float vf[32][4];
        alignas(16) float acc[4];
        _mm_storeu_ps(vf[12], sphereCenterR);
        loadPackedTriangle(vf, v0, v1, v2, norm4);
        CollisionVu v(vf, acc);
        if (!sphereTriangleCore<true>(v, nullptr))
            return false;
        outPos = _mm_loadu_ps(vf[1]);
        outNorm = _mm_loadu_ps(vf[2]);
        outDist = vf[3][0];
        return true;
    }

    bool vuSegmentTriangleHit(const __m128 &segStart, const __m128 &segEnd, const int16_t *v0, const int16_t *v1,
                              const int16_t *v2, const int16_t *norm4, __m128 &outPos, __m128 &outNorm, float &outT)
    {
        alignas(16) float vf[32][4];
        alignas(16) float acc[4];
        _mm_storeu_ps(vf[12], segStart);
        _mm_storeu_ps(vf[13], segEnd);
        loadPackedTriangle(vf, v0, v1, v2, norm4);
        CollisionVu v(vf, acc);
        v.convertInputs();
        segmentTriangleFrom0068(v, nullptr);
        if (v.r.vi[1] == 0)
            return false;
        outPos = _mm_loadu_ps(vf[1]);
        outNorm = _mm_loadu_ps(vf[2]);
        outT = vf[3][0];
        return true;
    }
}
