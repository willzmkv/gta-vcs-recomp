// Native C++ / SSE replacement for VCS's CCollision::ProcessColModels (0x002B8ED0) and its scalar/AABB
// collision helpers (0x00128DB0, 0x00128F60, 0x00129150, 0x0012A038, 0x0012A798, 0x002B78D0).
// See docs/NATIVE_CXX_REWRITE.md (Passo 1.2) and docs/game-internals.md 6c.
//
// VCS_NATIVE_COL=0: fall back to the recompiled guest code.
// VCS_NATIVE_COL_VERIFY=1: compare every output (v0, sphere CColPoints, line CColPoints, lineTs) against guest code.
#pragma STDC FP_CONTRACT OFF

#include "game/vcs_collision_native.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <immintrin.h>
#include <vector>

namespace
{
    bool nativeColEnabled()
    {
        static const bool on = [] {
            const char *e = std::getenv("VCS_NATIVE_COL");
            return !(e && e[0] == '0' && e[1] == '\0');
        }();
        return on;
    }

    bool verifyColEnabled()
    {
        static const bool on = [] {
            const char *e = std::getenv("VCS_NATIVE_COL_VERIFY");
            return e && *e && e[0] != '0';
        }();
        return on;
    }

    PS2Runtime::RecompiledFunction g_origProcessCol = nullptr;

    uint8_t *colFastPtr(uint8_t *rdram, uint32_t vaddr, uint64_t bytes, bool write)
    {
        if (vaddr >= PS2_SCRATCHPAD_BASE)
        {
            const uint64_t off = vaddr - PS2_SCRATCHPAD_BASE;
            if (off + bytes <= PS2_SCRATCHPAD_SIZE)
                return g_ps2ScratchpadHost + off;
            return nullptr;
        }
        const uint32_t limit = write ? g_ps2FastWriteLimit : g_ps2FastReadLimit;
        if (uint64_t(vaddr) + bytes <= limit)
            return rdram + vaddr;
        return nullptr;
    }

    // VU0 collision programs execute in round-toward-zero mode (MXCSR RC = 3, matching ps2_runtime.cpp's VU0 dispatch).
    struct Vu0RoundGuard
    {
        unsigned saved;
        Vu0RoundGuard() noexcept : saved(_mm_getcsr()) { _mm_setcsr((saved & ~0x6000u) | 0x6000u); }
        ~Vu0RoundGuard() noexcept { _mm_setcsr(saved); }
        Vu0RoundGuard(const Vu0RoundGuard &) = delete;
        Vu0RoundGuard &operator=(const Vu0RoundGuard &) = delete;
    };

    inline uint16_t rdU16(const uint8_t *p)
    {
        uint16_t v;
        std::memcpy(&v, p, 2);
        return v;
    }

    inline int16_t rdS16(const uint8_t *p)
    {
        int16_t v;
        std::memcpy(&v, p, 2);
        return v;
    }

    inline uint32_t rdU32(const uint8_t *p)
    {
        uint32_t v;
        std::memcpy(&v, p, 4);
        return v;
    }

    inline float rdF32(const uint8_t *p)
    {
        float v;
        std::memcpy(&v, p, 4);
        return v;
    }

    inline void wrU32(uint8_t *p, uint32_t v)
    {
        std::memcpy(p, &v, 4);
    }

    inline void wrF32(uint8_t *p, float v)
    {
        std::memcpy(p, &v, 4);
    }

    struct Vec4
    {
        float x, y, z, w;
    };

    struct alignas(16) CColPointHost
    {
        float pos[4];    // +0x00 (w ignored / uninitialized in guest)
        float depth;     // +0x10 (+0x14..+0x1f untouched by ProcessColModels)
        uint32_t pad[3]; // +0x14..+0x1f
        float normal[4]; // +0x20 (w ignored)
        uint8_t surf[4]; // +0x30: [surfA, pieceA, surfB, pieceB]
        uint32_t tail[3];
    };
    static_assert(sizeof(CColPointHost) == 64);

    // VU0 VMUL/VADD 4x3 point transform matching FUN_0039DFE0 / FUN_0039E050 and FUN_002B8ED0's inline macros.
    inline __m128 vuTransformPoint(__m128 m0, __m128 m1, __m128 m2, __m128 m3, __m128 p)
    {
        const __m128 px = _mm_shuffle_ps(p, p, _MM_SHUFFLE(0, 0, 0, 0));
        const __m128 py = _mm_shuffle_ps(p, p, _MM_SHUFFLE(1, 1, 1, 1));
        const __m128 pz = _mm_shuffle_ps(p, p, _MM_SHUFFLE(2, 2, 2, 2));
        __m128 acc = PS2_VMUL(m0, px);
        acc = PS2_VADD(acc, PS2_VMUL(m1, py));
        acc = PS2_VADD(acc, PS2_VMUL(m2, pz));
        return PS2_VADD(acc, PS2_VMUL(m3, _mm_set1_ps(1.0f)));
    }

    // VU0 3x3 normal transform (no translation, no W), matching hot.c:6210-6217 & 6449-6456.
    inline __m128 vuTransformNormal(__m128 m0, __m128 m1, __m128 m2, __m128 n)
    {
        const __m128 nx = _mm_shuffle_ps(n, n, _MM_SHUFFLE(0, 0, 0, 0));
        const __m128 ny = _mm_shuffle_ps(n, n, _MM_SHUFFLE(1, 1, 1, 1));
        const __m128 nz = _mm_shuffle_ps(n, n, _MM_SHUFFLE(2, 2, 2, 2));
        __m128 acc = PS2_VMUL(m0, nx);
        acc = PS2_VADD(acc, PS2_VMUL(m1, ny));
        return PS2_VADD(acc, PS2_VMUL(m2, nz));
    }

    // Rigid inverse-compose: M_A_in_B = A * inv(B) (row-vector convention), matching FUN_002b8ed0 scalar FPU order.
    inline void buildRelMatrix(const float *matA, const float *matB, __m128 outRows[4])
    {
        auto dot3 = [](float ax, float ay, float az, float bx, float by, float bz) -> float {
            return FPU_ADD_S(FPU_ADD_S(FPU_MUL_S(bx, ax), FPU_MUL_S(by, ay)), FPU_MUL_S(bz, az));
        };
        const float nb0 = FPU_NEG_S(dot3(matB[12], matB[13], matB[14], matB[0], matB[1], matB[2]));
        const float nb1 = FPU_NEG_S(dot3(matB[12], matB[13], matB[14], matB[4], matB[5], matB[6]));
        const float nb2 = FPU_NEG_S(dot3(matB[12], matB[13], matB[14], matB[8], matB[9], matB[10]));

        for (int r = 0; r < 3; ++r)
        {
            const float *ar = matA + r * 4;
            const float c0 = dot3(ar[0], ar[1], ar[2], matB[0], matB[1], matB[2]);
            const float c1 = dot3(ar[0], ar[1], ar[2], matB[4], matB[5], matB[6]);
            const float c2 = dot3(ar[0], ar[1], ar[2], matB[8], matB[9], matB[10]);
            outRows[r] = _mm_setr_ps(c0, c1, c2, 0.0f);
        }
        const float *a3 = matA + 12;
        const float t0 = FPU_ADD_S(dot3(a3[0], a3[1], a3[2], matB[0], matB[1], matB[2]), nb0);
        const float t1 = FPU_ADD_S(dot3(a3[0], a3[1], a3[2], matB[4], matB[5], matB[6]), nb1);
        const float t2 = FPU_ADD_S(dot3(a3[0], a3[1], a3[2], matB[8], matB[9], matB[10]), nb2);
        outRows[3] = _mm_setr_ps(t0, t1, t2, 0.0f);
    }

    inline bool sphereOverlapsAabb(const Vec4 &s, const float *bMin, const float *bMax)
    {
        return !(FPU_C_OLT_S(FPU_ADD_S(s.x, s.w), bMin[0]) || FPU_C_OLT_S(bMax[0], FPU_SUB_S(s.x, s.w)) ||
                 FPU_C_OLT_S(FPU_ADD_S(s.y, s.w), bMin[1]) || FPU_C_OLT_S(bMax[1], FPU_SUB_S(s.y, s.w)) ||
                 FPU_C_OLT_S(FPU_ADD_S(s.z, s.w), bMin[2]) || FPU_C_OLT_S(bMax[2], FPU_SUB_S(s.z, s.w)));
    }

    // Matches FUN_00128f60 (col_helpers.c:38-99 & FUN_00128f60_0x128f60.cpp).
    inline bool sphereVsSphere(const Vec4 &s1, const uint8_t *sph2Ptr, uint8_t *outPtBytes, float &bestDistSq)
    {
        const float x2 = rdF32(sph2Ptr + 0), y2 = rdF32(sph2Ptr + 4), z2 = rdF32(sph2Ptr + 8), r2 = rdF32(sph2Ptr + 12);
        const float dx = FPU_SUB_S(s1.x, x2);
        const float dy = FPU_SUB_S(s1.y, y2);
        const float dz = FPU_SUB_S(s1.z, z2);
        if (FPU_C_EQ_S(dx, 0.0f) && FPU_C_EQ_S(dy, 0.0f) && FPU_C_EQ_S(dz, 0.0f))
            return false;
        const float d2 = FPU_ADD_S(FPU_ADD_S(FPU_MUL_S(dx, dx), FPU_MUL_S(dy, dy)), FPU_MUL_S(dz, dz));
        const float distCenter = FPU_SQRT_S(d2);
        float dist = FPU_SUB_S(distCenter, r2);
        const float depth = FPU_SUB_S(s1.w, dist);
        if (FPU_C_OLT_S(dist, 0.0f))
            dist = 0.0f;
        const float distSq = FPU_MUL_S(dist, dist);
        if (!FPU_C_OLT_S(distSq, bestDistSq) || !FPU_C_OLT_S(dist, s1.w))
            return false;

        float nx, ny = dy, nz = dz;
        if (FPU_C_OLE_S(d2, 0.0f))
        {
            nx = 1.0f;
        }
        else
        {
            const float inv = FPU_DIV_S(1.0f, distCenter);
            nx = FPU_MUL_S(dx, inv);
            ny = FPU_MUL_S(dy, inv);
            nz = FPU_MUL_S(dz, inv);
        }
        wrF32(outPtBytes + 0x00u, FPU_SUB_S(s1.x, FPU_MUL_S(dist, nx)));
        wrF32(outPtBytes + 0x04u, FPU_SUB_S(s1.y, FPU_MUL_S(dist, ny)));
        wrF32(outPtBytes + 0x08u, FPU_SUB_S(s1.z, FPU_MUL_S(dist, nz)));
        wrF32(outPtBytes + 0x10u, depth);
        wrF32(outPtBytes + 0x20u, nx);
        wrF32(outPtBytes + 0x24u, ny);
        wrF32(outPtBytes + 0x28u, nz);
        outPtBytes[0x32u] = sph2Ptr[0x10u];
        outPtBytes[0x33u] = sph2Ptr[0x11u];
        bestDistSq = distSq;
        return true;
    }

    // Matches FUN_00129150 (col_helpers.c:104-469 & FUN_00129150_0x129150.cpp).
    inline bool sphereVsBox(const Vec4 &s, const uint8_t *boxPtr, uint8_t *outPtBytes, float &bestDistSq)
    {
        const float minX = rdF32(boxPtr + 0x00u), minY = rdF32(boxPtr + 0x04u), minZ = rdF32(boxPtr + 0x08u);
        const float maxX = rdF32(boxPtr + 0x10u), maxY = rdF32(boxPtr + 0x14u), maxZ = rdF32(boxPtr + 0x18u);
        if (FPU_C_OLT_S(FPU_ADD_S(s.x, s.w), minX) || FPU_C_OLT_S(maxX, FPU_SUB_S(s.x, s.w)) ||
            FPU_C_OLT_S(FPU_ADD_S(s.y, s.w), minY) || FPU_C_OLT_S(maxY, FPU_SUB_S(s.y, s.w)) ||
            FPU_C_OLT_S(FPU_ADD_S(s.z, s.w), minZ) || FPU_C_OLT_S(maxZ, FPU_SUB_S(s.z, s.w)))
            return false;

        const bool insideX = !FPU_C_OLT_S(s.x, minX) && !FPU_C_OLT_S(maxX, s.x);
        const bool insideY = !FPU_C_OLT_S(s.y, minY) && !FPU_C_OLT_S(maxY, s.y);
        const bool insideZ = !FPU_C_OLT_S(s.z, minZ) && !FPU_C_OLT_S(maxZ, s.z);

        if (insideX && insideY && insideZ)
        {
            const float halfX = FPU_MUL_S(FPU_ADD_S(maxX, minX), 0.5f);
            const float halfY = FPU_MUL_S(FPU_ADD_S(maxY, minY), 0.5f);
            const float halfZ = FPU_MUL_S(FPU_ADD_S(maxZ, minZ), 0.5f);
            const float dxh = FPU_SUB_S(s.x, halfX);
            const float dyh = FPU_SUB_S(s.y, halfY);
            const float dzh = FPU_SUB_S(s.z, halfZ);
            const float distX = FPU_C_OLT_S(0.0f, dxh) ? FPU_SUB_S(maxX, s.x) : FPU_SUB_S(s.x, minX);
            const float distY = FPU_C_OLT_S(0.0f, dyh) ? FPU_SUB_S(maxY, s.y) : FPU_SUB_S(s.y, minY);
            const float distZ = FPU_C_OLT_S(0.0f, dzh) ? FPU_SUB_S(maxZ, s.z) : FPU_SUB_S(s.z, minZ);

            float px = s.x, py = s.y, pz = s.z, nx = 0.0f, ny = 0.0f, nz = 0.0f, depth = 0.0f;
            if (FPU_C_OLT_S(distX, distY) && FPU_C_OLT_S(distX, distZ))
            {
                if (!FPU_C_OLT_S(FPU_MUL_S(distX, distX), bestDistSq))
                    return false;
                if (FPU_C_OLT_S(0.0f, dxh))
                {
                    px = FPU_SUB_S(s.x, s.w);
                    nx = 1.0f;
                }
                else
                {
                    px = FPU_ADD_S(s.x, s.w);
                    nx = -1.0f;
                }
                depth = distX;
            }
            else if (FPU_C_OLT_S(distY, distX) && FPU_C_OLT_S(distY, distZ))
            {
                if (!FPU_C_OLT_S(FPU_MUL_S(distY, distY), bestDistSq))
                    return false;
                if (FPU_C_OLT_S(0.0f, dyh))
                {
                    py = FPU_SUB_S(s.y, s.w);
                    ny = 1.0f;
                }
                else
                {
                    py = FPU_ADD_S(s.y, s.w);
                    ny = -1.0f;
                }
                depth = distY;
            }
            else
            {
                if (!FPU_C_OLT_S(FPU_MUL_S(distZ, distZ), bestDistSq))
                    return false;
                if (FPU_C_OLT_S(0.0f, dzh))
                {
                    pz = FPU_SUB_S(s.z, s.w);
                    nz = 1.0f;
                }
                else
                {
                    pz = FPU_ADD_S(s.z, s.w);
                    nz = -1.0f;
                }
                depth = distZ;
            }
            wrF32(outPtBytes + 0x00u, px);
            wrF32(outPtBytes + 0x04u, py);
            wrF32(outPtBytes + 0x08u, pz);
            wrF32(outPtBytes + 0x10u, depth);
            wrF32(outPtBytes + 0x20u, nx);
            wrF32(outPtBytes + 0x24u, ny);
            wrF32(outPtBytes + 0x28u, nz);
            outPtBytes[0x32u] = boxPtr[0x20u];
            outPtBytes[0x33u] = boxPtr[0x21u];
            return true;
        }

        const float cx = FPU_C_OLT_S(s.x, minX) ? minX : (FPU_C_OLT_S(maxX, s.x) ? maxX : s.x);
        const float cy = FPU_C_OLT_S(s.y, minY) ? minY : (FPU_C_OLT_S(maxY, s.y) ? maxY : s.y);
        const float cz = FPU_C_OLT_S(s.z, minZ) ? minZ : (FPU_C_OLT_S(maxZ, s.z) ? maxZ : s.z);
        const float dx = FPU_SUB_S(s.x, cx);
        const float dy = FPU_SUB_S(s.y, cy);
        const float dz = FPU_SUB_S(s.z, cz);
        const float d2 = FPU_ADD_S(FPU_ADD_S(FPU_MUL_S(dx, dx), FPU_MUL_S(dy, dy)), FPU_MUL_S(dz, dz));
        if (!FPU_C_OLT_S(d2, bestDistSq))
            return false;

        const float dist = FPU_SQRT_S(d2);
        const float inv = FPU_DIV_S(1.0f, dist);
        wrF32(outPtBytes + 0x00u, cx);
        wrF32(outPtBytes + 0x04u, cy);
        wrF32(outPtBytes + 0x08u, cz);
        wrF32(outPtBytes + 0x10u, FPU_SUB_S(s.w, dist));
        wrF32(outPtBytes + 0x20u, FPU_MUL_S(dx, inv));
        wrF32(outPtBytes + 0x24u, FPU_MUL_S(dy, inv));
        wrF32(outPtBytes + 0x28u, FPU_MUL_S(dz, inv));
        outPtBytes[0x32u] = boxPtr[0x20u];
        outPtBytes[0x33u] = boxPtr[0x21u];
        bestDistSq = d2;
        return true;
    }

    // Matches FUN_0012a038 (col_helpers.c:474-563).
    inline bool lineVsSphere(const Vec4 &p0, const Vec4 &p1, const uint8_t *sphPtr, uint8_t *outPtBytes, float &bestT)
    {
        const float sx = rdF32(sphPtr + 0), sy = rdF32(sphPtr + 4), sz = rdF32(sphPtr + 8), r = rdF32(sphPtr + 12);
        const float dx = FPU_SUB_S(p1.x, p0.x), dy = FPU_SUB_S(p1.y, p0.y), dz = FPU_SUB_S(p1.z, p0.z);
        const float cx = FPU_SUB_S(sx, p0.x), cy = FPU_SUB_S(sy, p0.y), cz = FPU_SUB_S(sz, p0.z);
        const float b = FPU_ADD_S(FPU_ADD_S(FPU_MUL_S(cx, dx), FPU_MUL_S(cy, dy)), FPU_MUL_S(cz, dz));
        const float a = FPU_ADD_S(FPU_ADD_S(FPU_MUL_S(dx, dx), FPU_MUL_S(dy, dy)), FPU_MUL_S(dz, dz));
        const float c = FPU_SUB_S(FPU_ADD_S(FPU_ADD_S(FPU_MUL_S(cx, cx), FPU_MUL_S(cy, cy)), FPU_MUL_S(cz, cz)),
                                  FPU_MUL_S(r, r));
        const float negB = FPU_NEG_S(b);
        const float disc = FPU_SUB_S(FPU_MUL_S(negB, negB), FPU_MUL_S(a, c));
        if (FPU_C_OLT_S(disc, 0.0f))
            return false;
        const float t = FPU_DIV_S(FPU_SUB_S(b, FPU_SQRT_S(disc)), a);
        if (FPU_C_OLT_S(t, 0.0f) || FPU_C_OLT_S(1.0f, t) || !FPU_C_OLT_S(t, bestT))
            return false;

        const float hx = FPU_ADD_S(p0.x, FPU_MUL_S(t, dx));
        const float hy = FPU_ADD_S(p0.y, FPU_MUL_S(t, dy));
        const float hz = FPU_ADD_S(p0.z, FPU_MUL_S(t, dz));
        float nx = FPU_SUB_S(hx, sx), ny = FPU_SUB_S(hy, sy), nz = FPU_SUB_S(hz, sz);
        const float lenSq = FPU_ADD_S(FPU_ADD_S(FPU_MUL_S(nx, nx), FPU_MUL_S(ny, ny)), FPU_MUL_S(nz, nz));
        if (FPU_C_OLE_S(lenSq, 0.0f))
        {
            nx = 1.0f;
        }
        else
        {
            const float inv = FPU_RSQRT_S(1.0f, lenSq);
            nx = FPU_MUL_S(nx, inv);
            ny = FPU_MUL_S(ny, inv);
            nz = FPU_MUL_S(nz, inv);
        }
        wrF32(outPtBytes + 0x00u, hx);
        wrF32(outPtBytes + 0x04u, hy);
        wrF32(outPtBytes + 0x08u, hz);
        wrF32(outPtBytes + 0x20u, nx);
        wrF32(outPtBytes + 0x24u, ny);
        wrF32(outPtBytes + 0x28u, nz);
        outPtBytes[0x30u] = 0u;
        outPtBytes[0x31u] = 0u;
        outPtBytes[0x32u] = sphPtr[0x10u];
        outPtBytes[0x33u] = sphPtr[0x11u];
        bestT = t;
        return true;
    }

    // Matches FUN_0012a798 (col_helpers.c:568-842).
    inline bool lineVsBox(const Vec4 &p0, const Vec4 &p1, const uint8_t *boxPtr, uint8_t *outPtBytes, float &bestT)
    {
        const float minX = rdF32(boxPtr + 0x00u), minY = rdF32(boxPtr + 0x04u), minZ = rdF32(boxPtr + 0x08u);
        const float maxX = rdF32(boxPtr + 0x10u), maxY = rdF32(boxPtr + 0x14u), maxZ = rdF32(boxPtr + 0x18u);
        float bestLocal = 1.0f;
        float hx = 0.0f, hy = 0.0f, hz = 0.0f, nx = 0.0f, ny = 0.0f, nz = 0.0f;

        auto trySlab = [&](float d0, float d1, float a, float b, float bA0, float bA1, float bB0, float bB1,
                           float cx, float cy, float cz, float snx, float sny, float snz) {
            if (FPU_C_OLT_S(FPU_MUL_S(d0, d1), 0.0f) && FPU_C_OLT_S(bA0, a) && FPU_C_OLT_S(a, bA1) &&
                FPU_C_OLT_S(bB0, b) && FPU_C_OLT_S(b, bB1))
            {
                const float t = FPU_DIV_S(d0, FPU_SUB_S(d0, d1));
                if (FPU_C_OLT_S(t, bestLocal))
                {
                    bestLocal = t;
                    hx = cx;
                    hy = cy;
                    hz = cz;
                    nx = snx;
                    ny = sny;
                    nz = snz;
                }
            }
        };
        auto slabT = [](float d0, float d1) { return FPU_DIV_S(d0, FPU_SUB_S(d0, d1)); };
        auto hitPos = [](float p0v, float p1v, float t) { return FPU_ADD_S(FPU_MUL_S(FPU_SUB_S(p1v, p0v), t), p0v); };

        // 1) xMin, 2) xMax
        for (int s = 0; s < 2; ++s)
        {
            const float px = s ? maxX : minX;
            const float d0 = s ? FPU_SUB_S(p0.x, px) : FPU_SUB_S(px, p0.x);
            const float d1 = s ? FPU_SUB_S(p1.x, px) : FPU_SUB_S(px, p1.x);
            if (FPU_C_OLT_S(FPU_MUL_S(d0, d1), 0.0f))
            {
                const float t = slabT(d0, d1);
                const float y = hitPos(p0.y, p1.y, t), z = hitPos(p0.z, p1.z, t);
                trySlab(d0, d1, y, z, minY, maxY, minZ, maxZ, px, y, z, s ? 1.0f : -1.0f, 0.0f, 0.0f);
            }
        }
        // 3) yMin, 4) yMax
        for (int s = 0; s < 2; ++s)
        {
            const float py = s ? maxY : minY;
            const float d0 = s ? FPU_SUB_S(p0.y, py) : FPU_SUB_S(py, p0.y);
            const float d1 = s ? FPU_SUB_S(p1.y, py) : FPU_SUB_S(py, p1.y);
            if (FPU_C_OLT_S(FPU_MUL_S(d0, d1), 0.0f))
            {
                const float t = slabT(d0, d1);
                const float x = hitPos(p0.x, p1.x, t), z = hitPos(p0.z, p1.z, t);
                trySlab(d0, d1, x, z, minX, maxX, minZ, maxZ, x, py, z, 0.0f, s ? 1.0f : -1.0f, 0.0f);
            }
        }
        // 5) zMin, 6) zMax
        for (int s = 0; s < 2; ++s)
        {
            const float pz = s ? maxZ : minZ;
            const float d0 = s ? FPU_SUB_S(p0.z, pz) : FPU_SUB_S(pz, p0.z);
            const float d1 = s ? FPU_SUB_S(p1.z, pz) : FPU_SUB_S(pz, p1.z);
            if (FPU_C_OLT_S(FPU_MUL_S(d0, d1), 0.0f))
            {
                const float t = slabT(d0, d1);
                const float x = hitPos(p0.x, p1.x, t), y = hitPos(p0.y, p1.y, t);
                trySlab(d0, d1, x, y, minX, maxX, minY, maxY, x, y, pz, 0.0f, 0.0f, s ? 1.0f : -1.0f);
            }
        }

        if (!FPU_C_OLT_S(bestLocal, bestT))
            return false;
        wrF32(outPtBytes + 0x00u, hx);
        wrF32(outPtBytes + 0x04u, hy);
        wrF32(outPtBytes + 0x08u, hz);
        wrF32(outPtBytes + 0x20u, nx);
        wrF32(outPtBytes + 0x24u, ny);
        wrF32(outPtBytes + 0x28u, nz);
        outPtBytes[0x30u] = 0u;
        outPtBytes[0x31u] = 0u;
        outPtBytes[0x32u] = boxPtr[0x20u];
        outPtBytes[0x33u] = boxPtr[0x21u];
        bestT = bestLocal;
        return true;
    }

    // LRU relink from FUN_00128db0 + FUN_00288c00 + FUN_00472d40 + FUN_00472d60 when pNormals != 0.
    inline void touchNormalsLru(uint8_t *rdram, uint32_t normalsAddr, uint16_t numTris)
    {
        constexpr uint32_t kLruHead = 0x00711E50u;
        const uint32_t nodePtrAddr = (normalsAddr + static_cast<uint32_t>(numTris) * 10u + 3u) & ~3u;
        if (static_cast<uint64_t>(nodePtrAddr) + 4u > g_ps2FastReadLimit)
            return;
        const uint32_t node = rdU32(rdram + nodePtrAddr);
        if (!node || static_cast<uint64_t>(node) + 12u > g_ps2FastReadLimit)
            return;
        const uint32_t next = rdU32(rdram + node + 4u);
        if (next == kLruHead)
            return; // already at tail (MRU)
        const uint32_t prev = rdU32(rdram + node + 8u);
        if (static_cast<uint64_t>(prev) + 8u <= g_ps2FastWriteLimit)
            wrU32(rdram + prev + 4u, next);
        if (static_cast<uint64_t>(next) + 12u <= g_ps2FastWriteLimit)
            wrU32(rdram + next + 8u, prev);
        const uint32_t headPrev = rdU32(rdram + kLruHead + 8u);
        wrU32(rdram + node + 8u, headPrev);
        if (static_cast<uint64_t>(headPrev) + 8u <= g_ps2FastWriteLimit)
            wrU32(rdram + headPrev + 4u, node);
        wrU32(rdram + node + 4u, kLruHead);
        wrU32(rdram + kLruHead + 8u, node);
    }

    bool runProcessColNative(uint8_t *rdram, const R5900Context *ctx, bool touchLru, int32_t &outRetV0)
    {
        const uint32_t matAAddr = GPR_U32(ctx, 4);
        const uint32_t colAAddr = GPR_U32(ctx, 5);
        const uint32_t matBAddr = GPR_U32(ctx, 6);
        const uint32_t colBAddr = GPR_U32(ctx, 7);
        const uint32_t outSphereAddr = GPR_U32(ctx, 8);
        const uint32_t outLineAddr = GPR_U32(ctx, 9);
        const uint32_t outLineTAddr = GPR_U32(ctx, 10);

        const uint8_t *matABytes = colFastPtr(rdram, matAAddr, 64u, false);
        const uint8_t *colABytes = colFastPtr(rdram, colAAddr, 0x54u, false);
        const uint8_t *matBBytes = colFastPtr(rdram, matBAddr, 64u, false);
        const uint8_t *colBBytes = colFastPtr(rdram, colBAddr, 0x54u, false);
        if (!matABytes || !colABytes || !matBBytes || !colBBytes)
            return false;

        alignas(16) float matA[16], matB[16];
        std::memcpy(matA, matABytes, 64u);
        std::memcpy(matB, matBBytes, 64u);

        __m128 mAinB[4];
        buildRelMatrix(matA, matB, mAinB);

        const float colASphLocal[4] = {
            rdF32(colABytes + 0x00u),
            rdF32(colABytes + 0x04u),
            rdF32(colABytes + 0x08u),
            rdF32(colABytes + 0x0Cu),
        };
        const __m128 sphAPosB = vuTransformPoint(mAinB[0], mAinB[1], mAinB[2], mAinB[3],
                                                 _mm_loadu_ps(colASphLocal));
        alignas(16) Vec4 sphA_in_B;
        _mm_store_ps(&sphA_in_B.x, sphAPosB);
        sphA_in_B.w = colASphLocal[3];

        const float colBMin[3] = {rdF32(colBBytes + 0x10u), rdF32(colBBytes + 0x14u), rdF32(colBBytes + 0x18u)};
        const float colBMax[3] = {rdF32(colBBytes + 0x20u), rdF32(colBBytes + 0x24u), rdF32(colBBytes + 0x28u)};
        if (!sphereOverlapsAabb(sphA_in_B, colBMin, colBMax))
        {
            outRetV0 = 0;
            return true;
        }

        const uint8_t numSpheresA = colABytes[0x34u];
        const uint8_t numLinesA = colABytes[0x35u];
        const uint8_t *spheresAPtr = nullptr;
        if (numSpheresA != 0u)
        {
            spheresAPtr = colFastPtr(rdram, rdU32(colABytes + 0x38u), static_cast<uint64_t>(numSpheresA) * 0x20u, false);
            if (!spheresAPtr)
                return false;
        }

        Vec4 xformedSphA[256];
        uint8_t hitSphA[256];
        uint32_t hitSphACount = 0u;
        for (uint32_t i = 0; i < numSpheresA; ++i)
        {
            const uint8_t *sp = spheresAPtr + i * 0x20u;
            const __m128 local = _mm_setr_ps(rdF32(sp + 0), rdF32(sp + 4), rdF32(sp + 8), 0.0f);
            const __m128 tr = vuTransformPoint(mAinB[0], mAinB[1], mAinB[2], mAinB[3], local);
            _mm_store_ps(&xformedSphA[i].x, tr);
            xformedSphA[i].w = rdF32(sp + 12);
            if (sphereOverlapsAabb(xformedSphA[i], colBMin, colBMax))
                hitSphA[hitSphACount++] = static_cast<uint8_t>(i);
        }

        if (hitSphACount == 0u && numLinesA == 0u)
        {
            outRetV0 = 0;
            return true;
        }

        // Broadphase colB spheres in A space.
        const uint8_t numSpheresB = colBBytes[0x34u];
        const uint8_t *spheresBPtr = nullptr;
        uint8_t hitSphB[256];
        uint32_t hitSphBCount = 0u;
        if (numSpheresB != 0u)
        {
            spheresBPtr = colFastPtr(rdram, rdU32(colBBytes + 0x38u), static_cast<uint64_t>(numSpheresB) * 0x20u, false);
            if (!spheresBPtr)
                return false;
            __m128 mBinA[4];
            buildRelMatrix(matB, matA, mBinA);
            const float colAMin[3] = {rdF32(colABytes + 0x10u), rdF32(colABytes + 0x14u), rdF32(colABytes + 0x18u)};
            const float colAMax[3] = {rdF32(colABytes + 0x20u), rdF32(colABytes + 0x24u), rdF32(colABytes + 0x28u)};
            for (uint32_t i = 0; i < numSpheresB; ++i)
            {
                const uint8_t *sp = spheresBPtr + i * 0x20u;
                const __m128 local = _mm_setr_ps(rdF32(sp + 0), rdF32(sp + 4), rdF32(sp + 8), 0.0f);
                const __m128 tr = vuTransformPoint(mBinA[0], mBinA[1], mBinA[2], mBinA[3], local);
                Vec4 sB;
                _mm_storeu_ps(&sB.x, tr);
                sB.w = rdF32(sp + 12);
                if (sphereOverlapsAabb(sB, colAMin, colAMax))
                    hitSphB[hitSphBCount++] = static_cast<uint8_t>(i);
            }
        }

        // Broadphase colB boxes against sphereA_in_B.
        const uint16_t numBoxesB = rdU16(colBBytes + 0x30u);
        const uint8_t *boxesBPtr = nullptr;
        uint16_t hitBoxesStack[256];
        std::vector<uint16_t> hitBoxesHeap;
        uint16_t *hitBoxesB = hitBoxesStack;
        uint32_t hitBoxesBCount = 0u;
        if (numBoxesB != 0u)
        {
            boxesBPtr = colFastPtr(rdram, rdU32(colBBytes + 0x40u), static_cast<uint64_t>(numBoxesB) * 0x30u, false);
            if (!boxesBPtr)
                return false;
            if (numBoxesB > 256u)
            {
                hitBoxesHeap.resize(numBoxesB);
                hitBoxesB = hitBoxesHeap.data();
            }
            for (uint32_t i = 0; i < numBoxesB; ++i)
            {
                const uint8_t *bp = boxesBPtr + i * 0x30u;
                const float bMin[3] = {rdF32(bp + 0x00u), rdF32(bp + 0x04u), rdF32(bp + 0x08u)};
                const float bMax[3] = {rdF32(bp + 0x10u), rdF32(bp + 0x14u), rdF32(bp + 0x18u)};
                if (sphereOverlapsAabb(sphA_in_B, bMin, bMax))
                    hitBoxesB[hitBoxesBCount++] = static_cast<uint16_t>(i);
            }
        }

        // Cold normal cache fallback (never allocate guest memory from host; see FUN_00128db0).
        const uint16_t numTrisB = rdU16(colBBytes + 0x32u);
        const uint32_t pNormalsAddr = numTrisB ? rdU32(colBBytes + 0x50u) : 0u;
        if (numTrisB != 0u && pNormalsAddr == 0u)
            return false;

        const uint8_t *trisBPtr = nullptr;
        const uint8_t *vertsBPtr = nullptr;
        const uint8_t *normalsBPtr = nullptr;
        uint32_t hitTrisStack[600];
        std::vector<uint32_t> hitTrisHeap;
        uint32_t *hitTrisB = hitTrisStack;
        uint32_t hitTrisCap = 600u;
        uint32_t hitTrisCount = 0u;

        if (numTrisB != 0u)
        {
            const uint32_t pTrisAddr = rdU32(colBBytes + 0x4Cu);
            const uint32_t pVertsAddr = rdU32(colBBytes + 0x48u);
            trisBPtr = colFastPtr(rdram, pTrisAddr, static_cast<uint64_t>(numTrisB) * 8u, false);
            normalsBPtr = colFastPtr(rdram, pNormalsAddr, static_cast<uint64_t>(numTrisB) * 10u, false);
            vertsBPtr = colFastPtr(rdram, pVertsAddr, 6u, false);
            if (!trisBPtr || !normalsBPtr || !vertsBPtr || pVertsAddr >= PS2_SCRATCHPAD_BASE)
                return false;
            const uint64_t vertsRoom = g_ps2FastReadLimit - pVertsAddr;
            if (vertsRoom < 0x10006u)
                return false;

            if (touchLru)
                touchNormalsLru(rdram, pNormalsAddr, numTrisB);

            const __m128 sphAVec = _mm_load_ps(&sphA_in_B.x);
            auto appendTriRange = [&](int32_t start, int32_t end) {
                const int32_t clampedStart = std::max<int32_t>(start, 0);
                const int32_t clampedEnd = std::min<int32_t>(end, static_cast<int32_t>(numTrisB));
                if (clampedStart >= clampedEnd)
                    return;
                Vu0RoundGuard vuRound;
                for (int32_t t = clampedStart; t < clampedEnd; ++t)
                {
                    const uint8_t *tri = trisBPtr + static_cast<uint32_t>(t) * 8u;
                    const int16_t *v0 = reinterpret_cast<const int16_t *>(vertsBPtr + rdU16(tri + 0u));
                    const int16_t *v1 = reinterpret_cast<const int16_t *>(vertsBPtr + rdU16(tri + 2u));
                    const int16_t *v2 = reinterpret_cast<const int16_t *>(vertsBPtr + rdU16(tri + 4u));
                    const int16_t *nrm = reinterpret_cast<const int16_t *>(normalsBPtr + static_cast<uint32_t>(t) * 10u);
                    if (vcs::col::vuSphereTriangleTest(sphAVec, v0, v1, v2, nrm))
                    {
                        if (hitTrisCount == hitTrisCap)
                        {
                            if (hitTrisHeap.empty())
                                hitTrisHeap.assign(hitTrisStack, hitTrisStack + hitTrisCount);
                            hitTrisCap = hitTrisCap * 2u;
                            hitTrisHeap.resize(hitTrisCap);
                            hitTrisB = hitTrisHeap.data();
                        }
                        hitTrisB[hitTrisCount++] = static_cast<uint32_t>(t);
                    }
                }
            };

            const uint8_t numSectorsB = colBBytes[0x36u];
            if (numSectorsB < 2u)
            {
                appendTriRange(0, static_cast<int32_t>(numTrisB));
            }
            else
            {
                const uint8_t *secPtr = colFastPtr(rdram, rdU32(colBBytes + 0x44u),
                                                   static_cast<uint64_t>(numSectorsB) * 0x30u, false);
                if (!secPtr)
                    return false;
                for (uint32_t s = 0; s < numSectorsB; ++s)
                {
                    const uint8_t *sp = secPtr + s * 0x30u;
                    const float sMin[3] = {rdF32(sp + 0x00u), rdF32(sp + 0x04u), rdF32(sp + 0x08u)};
                    const float sMax[3] = {rdF32(sp + 0x10u), rdF32(sp + 0x14u), rdF32(sp + 0x18u)};
                    if (sphereOverlapsAabb(sphA_in_B, sMin, sMax))
                    {
                        const int32_t start = static_cast<int32_t>(rdU16(sp + 0x20u));
                        const int32_t end = static_cast<int32_t>(rdS16(sp + 0x22u)) + 1;
                        appendTriRange(start, end);
                    }
                }
            }
        }

        if (hitSphBCount == 0u && hitBoxesBCount == 0u && hitTrisCount == 0u)
        {
            outRetV0 = 0;
            return true;
        }

        // Validate output buffers upfront so we never partially commit outputs on fallback.
        uint8_t *outSpherePtr = nullptr;
        if (hitSphACount != 0u || outSphereAddr != 0u)
        {
            outSpherePtr = outSphereAddr ? colFastPtr(rdram, outSphereAddr, 0x20u * 0x40u, true) : nullptr;
            // Some callers have smaller stack output arrays when colA has fewer spheres; bound by actual numSpheresA.
            if (!outSpherePtr && outSphereAddr != 0u)
                outSpherePtr = colFastPtr(rdram, outSphereAddr,
                                          static_cast<uint64_t>(std::max<uint32_t>(1u, hitSphACount)) * 0x40u, true);
            if (!outSpherePtr && outSphereAddr != 0u)
                return false;
        }
        uint8_t *outLinePtr = nullptr;
        uint8_t *outLineTPtr = nullptr;
        const uint8_t *linesAPtr = nullptr;
        if (numLinesA != 0u)
        {
            linesAPtr = colFastPtr(rdram, rdU32(colABytes + 0x3Cu), static_cast<uint64_t>(numLinesA) * 0x20u, false);
            outLinePtr = colFastPtr(rdram, outLineAddr, static_cast<uint64_t>(numLinesA) * 0x40u, true);
            outLineTPtr = colFastPtr(rdram, outLineTAddr, static_cast<uint64_t>(numLinesA) * 4u, true);
            if (!linesAPtr || !outLinePtr || !outLineTPtr)
                return false;
        }

        const __m128 matB0 = _mm_loadu_ps(matB + 0);
        const __m128 matB1 = _mm_loadu_ps(matB + 4);
        const __m128 matB2 = _mm_loadu_ps(matB + 8);
        const __m128 matB3 = _mm_loadu_ps(matB + 12);
        const __m128 xyzMask = _mm_castsi128_ps(_mm_set_epi32(0, -1, -1, -1));

        int32_t hitCount = 0;
        if (outSpherePtr)
            wrF32(outSpherePtr + 0x10u, -1.0f);

        for (uint32_t si = 0; si < hitSphACount; ++si)
        {
            if (!outSpherePtr)
                return false;
            const uint32_t idxA = hitSphA[si];
            const Vec4 &sA = xformedSphA[idxA];
            const uint8_t surfA = spheresAPtr[idxA * 0x20u + 0x10u];
            const uint8_t pieceA = spheresAPtr[idxA * 0x20u + 0x11u];

            uint8_t *curPt = outSpherePtr + static_cast<uint32_t>(hitCount) * 0x40u;
            float bestDistSq = std::bit_cast<float>(0x6753C21Cu);
            bool hitAny = false;

            for (uint32_t bi = 0; bi < hitSphBCount; ++bi)
            {
                if (sphereVsSphere(sA, spheresBPtr + static_cast<uint32_t>(hitSphB[bi]) * 0x20u, curPt, bestDistSq))
                {
                    curPt[0x30u] = surfA;
                    curPt[0x31u] = pieceA;
                    hitAny = true;
                }
            }
            for (uint32_t bi = 0; bi < hitBoxesBCount; ++bi)
            {
                if (sphereVsBox(sA, boxesBPtr + static_cast<uint32_t>(hitBoxesB[bi]) * 0x30u, curPt, bestDistSq))
                {
                    curPt[0x30u] = surfA;
                    curPt[0x31u] = pieceA;
                    hitAny = true;
                }
            }
            if (hitTrisCount != 0u)
            {
                const __m128 sAVec = _mm_load_ps(&sA.x);
                struct TriHit
                {
                    __m128 pos, norm;
                    float dist;
                    uint8_t surf;
                };
                TriHit triHitsStack[32];
                std::vector<TriHit> triHitsHeap;
                uint32_t nTriHits = 0u;
                {
                    Vu0RoundGuard vuRound;
                    for (uint32_t ti = 0; ti < hitTrisCount; ++ti)
                    {
                        const uint32_t t = hitTrisB[ti];
                        const uint8_t *tri = trisBPtr + t * 8u;
                        const int16_t *v0 = reinterpret_cast<const int16_t *>(vertsBPtr + rdU16(tri + 0u));
                        const int16_t *v1 = reinterpret_cast<const int16_t *>(vertsBPtr + rdU16(tri + 2u));
                        const int16_t *v2 = reinterpret_cast<const int16_t *>(vertsBPtr + rdU16(tri + 4u));
                        const int16_t *nrm = reinterpret_cast<const int16_t *>(normalsBPtr + t * 10u);
                        __m128 hitPos, hitNorm;
                        float triDist = 0.0f;
                        if (vcs::col::vuSphereTriangleHit(sAVec, v0, v1, v2, nrm, hitPos, hitNorm, triDist))
                        {
                            const TriHit h{hitPos, hitNorm, triDist, tri[6u]};
                            if (nTriHits < 32u)
                                triHitsStack[nTriHits++] = h;
                            else
                            {
                                if (triHitsHeap.empty())
                                    triHitsHeap.assign(triHitsStack, triHitsStack + 32u);
                                triHitsHeap.push_back(h);
                                ++nTriHits;
                            }
                        }
                    }
                }
                const TriHit *triHits = triHitsHeap.empty() ? triHitsStack : triHitsHeap.data();
                for (uint32_t hi = 0; hi < nTriHits; ++hi)
                {
                    const TriHit &h = triHits[hi];
                    const float depth = FPU_SUB_S(sA.w, h.dist);
                    if (FPU_C_OLT_S(rdF32(curPt + 0x10u), depth))
                    {
                        const __m128 prevP = _mm_loadu_ps(reinterpret_cast<const float *>(curPt + 0x00u));
                        const __m128 prevN = _mm_loadu_ps(reinterpret_cast<const float *>(curPt + 0x20u));
                        _mm_storeu_ps(reinterpret_cast<float *>(curPt + 0x00u),
                                      _mm_blendv_ps(prevP, h.pos, xyzMask));
                        _mm_storeu_ps(reinterpret_cast<float *>(curPt + 0x20u),
                                      _mm_blendv_ps(prevN, h.norm, xyzMask));
                        wrF32(curPt + 0x10u, depth);
                        curPt[0x30u] = surfA;
                        curPt[0x31u] = pieceA;
                        curPt[0x32u] = h.surf;
                        curPt[0x33u] = 0u;
                        hitAny = true;
                    }
                }
            }

            if (hitAny)
            {
                ++hitCount;
                if (hitCount == 0x20)
                    break;
                wrF32(outSpherePtr + static_cast<uint32_t>(hitCount) * 0x40u + 0x10u, -1.0f);
            }
        }

        // Transform all sphere CColPoints from B local space to world space via matB.
        for (int32_t i = 0; i < hitCount; ++i)
        {
            uint8_t *pt = outSpherePtr + static_cast<uint32_t>(i) * 0x40u;
            const __m128 p = _mm_loadu_ps(reinterpret_cast<const float *>(pt + 0x00u));
            const __m128 wp = vuTransformPoint(matB0, matB1, matB2, matB3, p);
            _mm_storeu_ps(reinterpret_cast<float *>(pt + 0x00u), _mm_blendv_ps(p, wp, xyzMask));
            const __m128 n = _mm_loadu_ps(reinterpret_cast<const float *>(pt + 0x20u));
            const __m128 wn = vuTransformNormal(matB0, matB1, matB2, n);
            _mm_storeu_ps(reinterpret_cast<float *>(pt + 0x20u), _mm_blendv_ps(n, wn, xyzMask));
        }

        // Lines path (+0x35), if any.
        if (numLinesA != 0u)
        {
            for (uint32_t k = 0; k < numLinesA; ++k)
            {
                const uint8_t *lp = linesAPtr + k * 0x20u;
                const __m128 p0v = vuTransformPoint(mAinB[0], mAinB[1], mAinB[2], mAinB[3],
                                                    _mm_setr_ps(rdF32(lp + 0), rdF32(lp + 4), rdF32(lp + 8), 0.0f));
                const __m128 p1v = vuTransformPoint(mAinB[0], mAinB[1], mAinB[2], mAinB[3],
                                                    _mm_setr_ps(rdF32(lp + 16), rdF32(lp + 20), rdF32(lp + 24), 0.0f));
                alignas(16) Vec4 p0, p1;
                _mm_store_ps(&p0.x, p0v);
                _mm_store_ps(&p1.x, p1v);

                uint8_t *outPt = outLinePtr + k * 0x40u;
                float bestT = rdF32(outLineTPtr + k * 4u);
                bool lineHit = false;

                for (uint32_t bi = 0; bi < hitSphBCount; ++bi)
                {
                    if (lineVsSphere(p0, p1, spheresBPtr + static_cast<uint32_t>(hitSphB[bi]) * 0x20u, outPt, bestT))
                        lineHit = true;
                }
                for (uint32_t bi = 0; bi < hitBoxesBCount; ++bi)
                {
                    if (lineVsBox(p0, p1, boxesBPtr + static_cast<uint32_t>(hitBoxesB[bi]) * 0x30u, outPt, bestT))
                        lineHit = true;
                }
                if (hitTrisCount != 0u)
                {
                    const __m128 segStart = _mm_load_ps(&p0.x);
                    const __m128 segEnd = _mm_load_ps(&p1.x);
                    Vu0RoundGuard vuRound;
                    for (uint32_t ti = 0; ti < hitTrisCount; ++ti)
                    {
                        const uint32_t t = hitTrisB[ti];
                        const uint8_t *tri = trisBPtr + t * 8u;
                        const int16_t *v0 = reinterpret_cast<const int16_t *>(vertsBPtr + rdU16(tri + 0u));
                        const int16_t *v1 = reinterpret_cast<const int16_t *>(vertsBPtr + rdU16(tri + 2u));
                        const int16_t *v2 = reinterpret_cast<const int16_t *>(vertsBPtr + rdU16(tri + 4u));
                        const int16_t *nrm = reinterpret_cast<const int16_t *>(normalsBPtr + t * 10u);
                        __m128 hitPos, hitNorm;
                        float triT = 0.0f;
                        if (vcs::col::vuSegmentTriangleHit(segStart, segEnd, v0, v1, v2, nrm, hitPos, hitNorm, triT))
                        {
                            if (FPU_C_OLT_S(triT, bestT))
                            {
                                const __m128 prevP = _mm_loadu_ps(reinterpret_cast<const float *>(outPt + 0x00u));
                                const __m128 prevN = _mm_loadu_ps(reinterpret_cast<const float *>(outPt + 0x20u));
                                _mm_storeu_ps(reinterpret_cast<float *>(outPt + 0x00u),
                                              _mm_blendv_ps(prevP, hitPos, xyzMask));
                                _mm_storeu_ps(reinterpret_cast<float *>(outPt + 0x20u),
                                              _mm_blendv_ps(prevN, hitNorm, xyzMask));
                                bestT = triT;
                                outPt[0x30u] = 0u;
                                outPt[0x31u] = 0u;
                                outPt[0x32u] = tri[6u];
                                outPt[0x33u] = 0u;
                                lineHit = true;
                            }
                        }
                    }
                }
                wrF32(outLineTPtr + k * 4u, bestT);
                if (lineHit)
                {
                    const __m128 p = _mm_loadu_ps(reinterpret_cast<const float *>(outPt + 0x00u));
                    const __m128 wp = vuTransformPoint(matB0, matB1, matB2, matB3, p);
                    _mm_storeu_ps(reinterpret_cast<float *>(outPt + 0x00u), _mm_blendv_ps(p, wp, xyzMask));
                    const __m128 n = _mm_loadu_ps(reinterpret_cast<const float *>(outPt + 0x20u));
                    const __m128 wn = vuTransformNormal(matB0, matB1, matB2, n);
                    _mm_storeu_ps(reinterpret_cast<float *>(outPt + 0x20u), _mm_blendv_ps(n, wn, xyzMask));
                }
            }
        }

        outRetV0 = hitCount;
        return true;
    }

    PS2_GUEST_CC void processColModelsNative(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x002B8ED0u || !nativeColEnabled())
        {
            g_origProcessCol(rdram, ctx, runtime);
            return;
        }
        if (verifyColEnabled()) [[unlikely]]
        {
            static std::atomic<uint64_t> checked{0}, diffs{0}, fallbacks{0};
            const uint32_t matAAddr = GPR_U32(ctx, 4);
            const uint32_t colAAddr = GPR_U32(ctx, 5);
            const uint32_t matBAddr = GPR_U32(ctx, 6);
            const uint32_t colBAddr = GPR_U32(ctx, 7);
            const uint32_t outSphAddr = GPR_U32(ctx, 8);
            const uint32_t outLineAddr = GPR_U32(ctx, 9);
            const uint32_t outLineTAddr = GPR_U32(ctx, 10);
            const uint8_t *colABytes = colFastPtr(rdram, colAAddr, 0x54u, false);
            const uint8_t numSphA = colABytes ? colABytes[0x34u] : 0u;
            const uint8_t numLineA = colABytes ? colABytes[0x35u] : 0u;
            const uint32_t maxSphPts = std::min<uint32_t>(32u, std::max<uint32_t>(1u, numSphA));

            // Save caller's input buffers cleanly before native runs.
            uint8_t *outSphPtr = outSphAddr ? colFastPtr(rdram, outSphAddr, uint64_t(maxSphPts) * 0x40u, true) : nullptr;
            uint8_t *outLinePtr = (numLineA && outLineAddr) ? colFastPtr(rdram, outLineAddr, uint64_t(numLineA) * 0x40u, true) : nullptr;
            uint8_t *outLineTPtr = (numLineA && outLineTAddr) ? colFastPtr(rdram, outLineTAddr, uint64_t(numLineA) * 4u, true) : nullptr;

            static thread_local std::vector<uint8_t> inSph, inLine, inLineT, natSph, natLine, natLineT;
            if (outSphPtr)
            {
                inSph.assign(outSphPtr, outSphPtr + maxSphPts * 0x40u);
            }
            if (outLinePtr && outLineTPtr)
            {
                inLine.assign(outLinePtr, outLinePtr + numLineA * 0x40u);
                inLineT.assign(outLineTPtr, outLineTPtr + numLineA * 4u);
            }

            const R5900Context savedCtx = *ctx;
            int32_t natV0 = 0;
            // In verify mode, skip LRU mutation in native so guest sees identical state before running.
            const bool didNative = runProcessColNative(rdram, ctx, /*touchLru=*/false, natV0);
            if (!didNative)
            {
                fallbacks.fetch_add(1, std::memory_order_relaxed);
                *ctx = savedCtx;
                g_origProcessCol(rdram, ctx, runtime);
                return;
            }
            if (outSphPtr)
            {
                natSph.assign(outSphPtr, outSphPtr + maxSphPts * 0x40u);
                std::memcpy(outSphPtr, inSph.data(), inSph.size());
            }
            if (outLinePtr && outLineTPtr)
            {
                natLine.assign(outLinePtr, outLinePtr + numLineA * 0x40u);
                natLineT.assign(outLineTPtr, outLineTPtr + numLineA * 4u);
                std::memcpy(outLinePtr, inLine.data(), inLine.size());
                std::memcpy(outLineTPtr, inLineT.data(), inLineT.size());
            }

            *ctx = savedCtx;
            const uint32_t savedRa = GPR_U32(ctx, 31);
            g_origProcessCol(rdram, ctx, runtime);
            if (ctx->pc != savedRa)
                return;
            const int32_t origV0 = GPR_S32(ctx, 2);

            bool same = (natV0 == origV0);
            auto eqPt = [](const uint8_t *a, const uint8_t *b, bool checkDepth) {
                for (uint32_t o : {0x00u, 0x04u, 0x08u, 0x20u, 0x24u, 0x28u})
                {
                    if (std::memcmp(a + o, b + o, 4u) != 0)
                        return false;
                }
                if (checkDepth && std::memcmp(a + 0x10u, b + 0x10u, 4u) != 0)
                    return false;
                return std::memcmp(a + 0x30u, b + 0x30u, 4u) == 0;
            };
            if (same && origV0 > 0 && outSphPtr)
            {
                for (int32_t i = 0; i < origV0; ++i)
                {
                    if (!eqPt(natSph.data() + i * 0x40u, outSphPtr + i * 0x40u, true))
                    {
                        same = false;
                        break;
                    }
                }
            }
            if (same && numLineA && outLinePtr && outLineTPtr)
            {
                if (std::memcmp(natLineT.data(), outLineTPtr, numLineA * 4u) != 0)
                    same = false;
                else
                {
                    for (uint32_t k = 0; k < numLineA; ++k)
                    {
                        if (std::memcmp(outLineTPtr + k * 4u, inLineT.data() + k * 4u, 4u) != 0 &&
                            !eqPt(natLine.data() + k * 0x40u, outLinePtr + k * 0x40u, false))
                        {
                            same = false;
                            break;
                        }
                    }
                }
            }
            const uint64_t n = checked.fetch_add(1, std::memory_order_relaxed) + 1;
            if (!same)
            {
                const uint64_t d = diffs.fetch_add(1, std::memory_order_relaxed) + 1;
                if (d <= 20)
                {
                    int diffIdx = -1;
                    const char *diffKind = "v0";
                    if (natV0 == origV0 && origV0 > 0 && outSphPtr)
                    {
                        for (int32_t i = 0; i < origV0; ++i)
                        {
                            if (!eqPt(natSph.data() + i * 0x40u, outSphPtr + i * 0x40u, true))
                            {
                                diffIdx = i;
                                diffKind = "sphPt";
                                break;
                            }
                        }
                    }
                    if (natV0 == origV0 && diffIdx < 0 && numLineA && outLinePtr && outLineTPtr)
                    {
                        for (uint32_t k = 0; k < numLineA; ++k)
                        {
                            const float nt = rdF32(natLineT.data() + k * 4u);
                            const float ot = rdF32(outLineTPtr + k * 4u);
                            if (std::memcmp(&nt, &ot, 4u) != 0)
                            {
                                diffIdx = static_cast<int>(k);
                                diffKind = "lineT";
                                break;
                            }
                            if (!eqPt(natLine.data() + k * 0x40u, outLinePtr + k * 0x40u, false))
                            {
                                diffIdx = static_cast<int>(k);
                                diffKind = "linePt";
                                break;
                            }
                        }
                    }
                    std::fprintf(stderr, "[col-verify] DIFF #%llu (%s idx=%d) natV0=%d origV0=%d sphA=%u lineA=%u colB=0x%08X",
                                 static_cast<unsigned long long>(d), diffKind, diffIdx, natV0, origV0, numSphA, numLineA, colBAddr);
                    if (diffIdx >= 0 && std::strcmp(diffKind, "sphPt") != 0)
                    {
                        const uint8_t *np = natLine.data() + diffIdx * 0x40u;
                        const uint8_t *op = outLinePtr + diffIdx * 0x40u;
                        std::fprintf(stderr, " T=(%.6f vs %.6f, hex %08X vs %08X, inT=%.6f) P=(%.4f,%.4f,%.4f vs %.4f,%.4f,%.4f) S=(%u,%u,%u,%u vs %u,%u,%u,%u)",
                                     rdF32(natLineT.data() + diffIdx * 4u), rdF32(outLineTPtr + diffIdx * 4u),
                                     rdU32(natLineT.data() + diffIdx * 4u), rdU32(outLineTPtr + diffIdx * 4u),
                                     rdF32(inLineT.data() + diffIdx * 4u),
                                     rdF32(np), rdF32(np + 4), rdF32(np + 8), rdF32(op), rdF32(op + 4), rdF32(op + 8),
                                     np[48], np[49], np[50], np[51], op[48], op[49], op[50], op[51]);
                    }
                    else if (diffIdx >= 0)
                    {
                        const uint8_t *np = natSph.data() + diffIdx * 0x40u;
                        const uint8_t *op = outSphPtr + diffIdx * 0x40u;
                        std::fprintf(stderr, " D=(%.6f vs %.6f) P=(%.4f,%.4f,%.4f vs %.4f,%.4f,%.4f) N=(%.4f,%.4f,%.4f vs %.4f,%.4f,%.4f) S=(%u,%u,%u,%u vs %u,%u,%u,%u)",
                                     rdF32(np + 16), rdF32(op + 16),
                                     rdF32(np), rdF32(np + 4), rdF32(np + 8), rdF32(op), rdF32(op + 4), rdF32(op + 8),
                                     rdF32(np + 32), rdF32(np + 36), rdF32(np + 40), rdF32(op + 32), rdF32(op + 36), rdF32(op + 40),
                                     np[48], np[49], np[50], np[51], op[48], op[49], op[50], op[51]);
                    }
                    std::fprintf(stderr, "\n");
                }
            }
            if ((n >= 4096u && (n & (n - 1u)) == 0u) || (n & 0xFFFFu) == 0u)
            {
                std::fprintf(stderr, "[col-verify] checked=%llu diffs=%llu fallbacks=%llu\n",
                             static_cast<unsigned long long>(n),
                             static_cast<unsigned long long>(diffs.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(fallbacks.load(std::memory_order_relaxed)));
            }
            return;
        }

        int32_t retV0 = 0;
        if (runProcessColNative(rdram, ctx, /*touchLru=*/true, retV0))
        {
            SET_GPR_S32(ctx, 2, retV0);
            ctx->pc = GPR_U32(ctx, 31);
            return;
        }
        g_origProcessCol(rdram, ctx, runtime);
    }
}

namespace vcs::col
{
    void install(PS2Runtime &runtime)
    {
        if (!nativeColEnabled())
            return;
        g_origProcessCol = runtime.lookupFunction(0x002B8ED0u);
        if (g_origProcessCol)
            runtime.replaceFunction(0x002B8ED0u, &processColModelsNative);
    }
}
