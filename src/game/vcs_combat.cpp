#include "game/vcs_combat.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_control.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace vcs::combat
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        constexpr uint32_t kScanForEntities = 0x001EEEB8u; // CPed::ScanForEntities
        constexpr uint32_t kMeleeAttack = 0x001D47B8u;     // CPed::EvaluateMeleeAttack
        constexpr uint32_t kPlayerPed = 0x004E4910u;       // CWorld::Players[0].m_pPed
        constexpr uint32_t kFrameCounter = 0x004CD124u;    // CTimer::m_FrameCounter
        constexpr uint32_t kPedPool = 0x00487A94u;         // CPool<CPed>
        constexpr uint32_t kSectorBase = 0x00486464u;      // CSector array
        constexpr uint32_t kSectorSize = 0x38u;            // 56 bytes per CSector

        PS2Runtime::RecompiledFunction g_origScanForEntities = nullptr;
        PS2Runtime::RecompiledFunction g_origMeleeAttack = nullptr;

        uint16_t rd16(const uint8_t *ram, uint32_t a)
        {
            uint16_t v;
            std::memcpy(&v, ram + (a & kRamMask), 2);
            return v;
        }

        void wr16(uint8_t *ram, uint32_t a, uint16_t v)
        {
            std::memcpy(ram + (a & kRamMask), &v, 2);
        }

        uint32_t rd32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }

        void wr32(uint8_t *ram, uint32_t a, uint32_t v)
        {
            std::memcpy(ram + (a & kRamMask), &v, 4);
        }

        uint64_t rd64(const uint8_t *ram, uint32_t a)
        {
            uint64_t v;
            std::memcpy(&v, ram + (a & kRamMask), 8);
            return v;
        }

        void wr64(uint8_t *ram, uint32_t a, uint64_t v)
        {
            std::memcpy(ram + (a & kRamMask), &v, 8);
        }

        float rdf(const uint8_t *ram, uint32_t a)
        {
            float v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }

        inline int getSectorX(float x)
        {
            int s = static_cast<int>(x * (1.0f / 80.0f) + 25.0f);
            return std::clamp(s, 0, 49);
        }

        inline int getSectorY(float y)
        {
            int s = static_cast<int>(y * (1.0f / 80.0f) + 25.0f);
            return std::clamp(s, 0, 49);
        }

        // 1 & 2. ScanForEntities Hook: Force continuous every-frame scan for player ped
        PS2_GUEST_CC void scanForEntitiesHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ped = GPR_U32(ctx, 4);
            const uint32_t playerPed = rd32(rdram, kPlayerPed);

            if (ped && ped == playerPed)
            {
                // Force ((flags >> 54) + frameCounter) & 0xF == 0 so sector scan runs every single frame for the player
                const uint32_t frame = rd32(rdram, kFrameCounter);
                uint64_t flags = rd64(rdram, ped + 0x48);
                uint64_t desired = (16u - (frame & 0xFu)) & 0xFu;
                flags = (flags & ~(0xFULL << 54)) | (desired << 54);
                wr64(rdram, ped + 0x48, flags);
            }

            if (g_origScanForEntities)
                g_origScanForEntities(rdram, ctx, runtime);
        }

        // 3. Melee Attack Hook: Forward cone fallback check (guarantees targeted NPC is in m_nearPeds[0])
        PS2_GUEST_CC void meleeAttackHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ped = GPR_U32(ctx, 4); // attacker CPed*
            const uint32_t playerPed = rd32(rdram, kPlayerPed);

            if (ped && ped == playerPed)
            {
                const float px = rdf(rdram, ped + 0x30);
                const float py = rdf(rdram, ped + 0x34);
                const float pz = rdf(rdram, ped + 0x38);

                // Attacker forward vector in 2D (matrix forward row at +0x10)
                float fx = rdf(rdram, ped + 0x10);
                float fy = rdf(rdram, ped + 0x14);
                float flen = std::sqrt(fx * fx + fy * fy);
                if (flen > 1e-4f)
                {
                    fx /= flen;
                    fy /= flen;
                }
                else
                {
                    fx = 0.0f;
                    fy = 1.0f;
                }

                // Check sectors around the player for any ped in the forward cone (up to 3.0m, +-65 deg)
                uint32_t bestCandidate = 0;
                float bestDistSq = 3.0f * 3.0f;

                const int minSx = std::max(0, getSectorX(px - 3.5f));
                const int maxSx = std::min(49, getSectorX(px + 3.5f));
                const int minSy = std::max(0, getSectorY(py - 3.5f));
                const int maxSy = std::min(49, getSectorY(py + 3.5f));

                const uint32_t sectorsBase = rd32(rdram, kSectorBase);

                if (sectorsBase)
                {
                    for (int sy = minSy; sy <= maxSy; ++sy)
                    {
                        for (int sx = minSx; sx <= maxSx; ++sx)
                        {
                            const uint32_t sector = sectorsBase + static_cast<uint32_t>((sy * 50 + sx) * kSectorSize);
                            uint32_t node = rd32(rdram, sector + 0x24); // m_pedList
                            while (node)
                            {
                                const uint32_t cand = rd32(rdram, node);
                                node = rd32(rdram, node + 0x8);

                                if (!cand || cand == ped)
                                    continue;

                                // Check ped state (alive, not dead 0x37)
                                const uint32_t state = rd32(rdram, cand + 0x8C4);
                                if (state == 0x37) // PEDSTATE_DEAD
                                    continue;

                                const float cx = rdf(rdram, cand + 0x30);
                                const float cy = rdf(rdram, cand + 0x34);
                                const float cz = rdf(rdram, cand + 0x38);

                                const float dx = cx - px;
                                const float dy = cy - py;
                                const float dz = cz - pz;

                                if (std::abs(dz) > 2.0f)
                                    continue;

                                const float distSq = dx * dx + dy * dy;
                                if (distSq <= bestDistSq)
                                {
                                    const float dist = std::sqrt(distSq);
                                    if (dist > 1e-4f)
                                    {
                                        const float dot = (dx * fx + dy * fy) / dist;
                                        // Cosine of 65 deg is ~0.42
                                        if (dot >= 0.40f)
                                        {
                                            bestDistSq = distSq;
                                            bestCandidate = cand;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // If a ped is in front of Vic, ensure they occupy slot 0 in m_nearPeds
                if (bestCandidate)
                {
                    uint16_t numNear = rd16(rdram, ped + 0x77A);
                    bool alreadyPresent = false;
                    for (uint16_t i = 0; i < numNear && i < 10; ++i)
                    {
                        if (rd32(rdram, ped + 0x6F8 + i * 4) == bestCandidate)
                        {
                            alreadyPresent = true;
                            // If present but not at index 0, swap to front
                            if (i > 0)
                            {
                                uint32_t prev0 = rd32(rdram, ped + 0x6F8);
                                wr32(rdram, ped + 0x6F8, bestCandidate);
                                wr32(rdram, ped + 0x6F8 + i * 4, prev0);
                            }
                            break;
                        }
                    }

                    if (!alreadyPresent)
                    {
                        // Shift down existing peds and insert at 0
                        const int count = std::min<int>(numNear, 9);
                        for (int j = count; j > 0; --j)
                        {
                            wr32(rdram, ped + 0x6F8 + j * 4, rd32(rdram, ped + 0x6F8 + (j - 1) * 4));
                        }
                        wr32(rdram, ped + 0x6F8, bestCandidate);
                        if (numNear < 10)
                            numNear++;
                        wr16(rdram, ped + 0x77A, numNear);
                    }
                }
            }

            if (g_origMeleeAttack)
                g_origMeleeAttack(rdram, ctx, runtime);
        }
    }

    void install(PS2Runtime &runtime)
    {
        if ((g_origScanForEntities = runtime.lookupFunction(kScanForEntities)))
        {
            runtime.replaceFunction(kScanForEntities, &scanForEntitiesHook);
            std::fprintf(stderr, "[combat] CPed::ScanForEntities hooked (continuous player scan active)\n");
        }
        else
        {
            std::fprintf(stderr, "[combat] Warning: CPed::ScanForEntities (0x1EEEB8) not found\n");
        }

        if ((g_origMeleeAttack = runtime.lookupFunction(kMeleeAttack)))
        {
            runtime.replaceFunction(kMeleeAttack, &meleeAttackHook);
            std::fprintf(stderr, "[combat] CPed::EvaluateMeleeAttack hooked (forward cone fallback active)\n");
        }
        else
        {
            std::fprintf(stderr, "[combat] Warning: CPed::EvaluateMeleeAttack (0x1D47B8) not found\n");
        }
    }
}
