// Native C++ / SSE implementation of VCS's CCollision::ProcessColModels (0x002B8ED0) and helpers
// (docs/NATIVE_CXX_REWRITE.md Passo 1.2, docs/game-internals.md 6c).
#pragma once

#include <cstdint>
#include <immintrin.h>

class PS2Runtime;

namespace vcs::col
{
    void install(PS2Runtime &runtime);

    // Exact VU0 collision microprogram entrypoints (from src/game/vcs_vu0_native.cpp),
    // running on local registers with unpacked s16 geometry — zero R5900Context bank copies.
    bool vuSphereTriangleTest(const __m128 &sphereCenterR, const int16_t *v0, const int16_t *v1, const int16_t *v2,
                              const int16_t *norm4);
    bool vuSphereTriangleHit(const __m128 &sphereCenterR, const int16_t *v0, const int16_t *v1, const int16_t *v2,
                             const int16_t *norm4, __m128 &outPos, __m128 &outNorm, float &outDist);
    bool vuSegmentTriangleHit(const __m128 &segStart, const __m128 &segEnd, const int16_t *v0, const int16_t *v1,
                              const int16_t *v2, const int16_t *norm4, __m128 &outPos, __m128 &outNorm, float &outT);
}
