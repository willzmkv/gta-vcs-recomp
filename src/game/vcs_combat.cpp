#include "game/vcs_combat.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_control.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace vcs::combat
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        constexpr uint32_t kScanForEntities = 0x001EEEB8u; // CPed::ScanForEntities
        constexpr uint32_t kMeleeAttack = 0x001D47B8u;     // CPed::EvaluateMeleeAttack (AI)
        constexpr uint32_t kFireMelee = 0x002C1D80u;       // CWeapon::FireMelee (Player & all weapons)
        constexpr uint32_t kPlayerPed = 0x004E4910u;       // CWorld::Players[0].m_pPed
        constexpr uint32_t kFrameCounter = 0x004CD124u;    // CTimer::m_FrameCounter
        constexpr uint32_t kSectorBase = 0x00486464u;      // CSector array
        constexpr uint32_t kSectorSize = 0x38u;            // 56 bytes per CSector

        PS2Runtime::RecompiledFunction g_origScanForEntities = nullptr;
        PS2Runtime::RecompiledFunction g_origMeleeAttack = nullptr;
        PS2Runtime::RecompiledFunction g_origFireMelee = nullptr;

        uint8_t rd8(const uint8_t *ram, uint32_t a)
        {
            return ram[a & kRamMask];
        }

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

        void wrf(uint8_t *ram, uint32_t a, float v)
        {
            std::memcpy(ram + (a & kRamMask), &v, 4);
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

        struct NearCandidate
        {
            uint32_t cand;
            float dist;
            float dot;
            float score;
        };

        void collectNearPedsInCone(const uint8_t *rdram, uint32_t attacker, std::vector<NearCandidate> &outCandidates)
        {
            outCandidates.clear();
            if (!attacker)
                return;

            const float px = rdf(rdram, attacker + 0x30);
            const float py = rdf(rdram, attacker + 0x34);
            const float pz = rdf(rdram, attacker + 0x38);

            // Forward vector from row at +0x10
            float fx = rdf(rdram, attacker + 0x10);
            float fy = rdf(rdram, attacker + 0x14);
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

            const int minSx = std::max(0, getSectorX(px - 4.0f));
            const int maxSx = std::min(49, getSectorX(px + 4.0f));
            const int minSy = std::max(0, getSectorY(py - 4.0f));
            const int maxSy = std::min(49, getSectorY(py + 4.0f));

            const uint32_t sectorsBase = rd32(rdram, kSectorBase);
            if (!sectorsBase)
                return;

            for (int sy = minSy; sy <= maxSy; ++sy)
            {
                for (int sx = minSx; sx <= maxSx; ++sx)
                {
                    const uint32_t sector = sectorsBase + static_cast<uint32_t>((sy * 50 + sx) * kSectorSize);
                    uint32_t node = rd32(rdram, sector + 0x24); // m_pedList
                    while (node)
                    {
                        const uint32_t cand = rd32(rdram, node);
                        node = rd32(rdram, node + 8);

                        if (!cand || cand == attacker)
                            continue;

                        // Alive check: pedState != 0x37 (PEDSTATE_DEAD)
                        const uint32_t state = rd32(rdram, cand + 0x8C4);
                        if (state == 0x37)
                            continue;

                        const float cx = rdf(rdram, cand + 0x30);
                        const float cy = rdf(rdram, cand + 0x34);
                        const float cz = rdf(rdram, cand + 0x38);

                        const float dz = cz - pz;
                        if (std::abs(dz) > 2.5f)
                            continue;

                        const float dx = cx - px;
                        const float dy = cy - py;
                        const float distSq = dx * dx + dy * dy;

                        // Radius up to 3.5m
                        if (distSq > (3.5f * 3.5f))
                            continue;

                        const float dist = std::sqrt(distSq);
                        const float dot = (dist > 1e-4f) ? ((dx * fx + dy * fy) / dist) : 1.0f;

                        // Must be either:
                        // 1. Within forward cone (dot >= 0.20f, ~78 deg) and dist <= 3.2m
                        // 2. OR close body-to-body contact (dist <= 1.4m) regardless of angle
                        if ((dist <= 3.2f && dot >= 0.20f) || dist <= 1.4f)
                        {
                            const float score = dist - dot * 0.5f;
                            outCandidates.push_back({cand, dist, dot, score});
                        }
                    }
                }
            }

            std::sort(outCandidates.begin(), outCandidates.end(), [](const NearCandidate &a, const NearCandidate &b) {
                return a.score < b.score;
            });
        }

        void populateAttackerNearPeds(uint8_t *rdram, uint32_t attacker, const std::vector<NearCandidate> &candidates)
        {
            if (!attacker || candidates.empty())
                return;

            const size_t count = std::min<size_t>(candidates.size(), 10);
            for (size_t i = 0; i < count; ++i)
            {
                uint32_t cand = candidates[i].cand;
                // Ensure Bit 9 is set on candidate CPedFlags (cand + 0x48)
                uint64_t flags = rd64(rdram, cand + 0x48);
                flags |= (1ULL << 9);
                wr64(rdram, cand + 0x48, flags);

                // Put cand into attacker->m_nearPeds[i] (attacker + 0x6F8 + i*4)
                wr32(rdram, attacker + 0x6F8 + static_cast<uint32_t>(i * 4), cand);
            }

            // Set m_numNearPeds (attacker + 0x77A) to at least count
            uint16_t numNear = rd16(rdram, attacker + 0x77A);
            if (numNear < count)
            {
                numNear = static_cast<uint16_t>(count);
                wr16(rdram, attacker + 0x77A, numNear);
            }
        }

        void adjustFirePosForTarget(uint8_t *rdram, uint32_t attacker, uint32_t pFirePos, const NearCandidate &bestTarget)
        {
            if (!attacker || !pFirePos)
                return;

            const uint32_t cand = bestTarget.cand;
            const float cx = rdf(rdram, cand + 0x30);
            const float cy = rdf(rdram, cand + 0x34);
            const float cz = rdf(rdram, cand + 0x38);

            const float px = rdf(rdram, attacker + 0x30);
            const float py = rdf(rdram, attacker + 0x34);

            const float fpx = rdf(rdram, pFirePos + 0);
            const float fpy = rdf(rdram, pFirePos + 4);
            const float fpz = rdf(rdram, pFirePos + 8);

            // Check distance between current pFirePos and candidate
            const float cdx = cx - fpx;
            const float cdy = cy - fpy;
            const float cdz = cz - fpz;
            const float cDistSq = cdx * cdx + cdy * cdy + cdz * cdz;

            // If pFirePos is farther than 1.0m from target, nudge it closer so melee sphere test passes
            if (cDistSq > 1.0f * 1.0f)
            {
                float dirX = cx - px;
                float dirY = cy - py;
                float len = std::hypot(dirX, dirY);
                if (len > 1e-4f)
                {
                    dirX /= len;
                    dirY /= len;
                }
                else
                {
                    dirX = 0.0f;
                    dirY = 1.0f;
                }

                // Place pFirePos 0.6m away from candidate towards attacker
                const float newX = cx - dirX * 0.6f;
                const float newY = cy - dirY * 0.6f;
                const float newZ = cz + 0.1f;

                wrf(rdram, pFirePos + 0, newX);
                wrf(rdram, pFirePos + 4, newY);
                wrf(rdram, pFirePos + 8, newZ);
            }
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
            {
                ctx->pc = kScanForEntities;
                g_origScanForEntities(rdram, ctx, runtime);
            }

            // After scan completes for player, ensure all near peds have Bit 9 set
            if (ped && ped == playerPed)
            {
                const uint16_t numNear = rd16(rdram, ped + 0x77A);
                for (uint16_t i = 0; i < numNear && i < 10; ++i)
                {
                    const uint32_t cand = rd32(rdram, ped + 0x6F8 + i * 4);
                    if (cand)
                    {
                        uint64_t flags = rd64(rdram, cand + 0x48);
                        flags |= (1ULL << 9);
                        wr64(rdram, cand + 0x48, flags);
                    }
                }
            }
        }

        // 3. AI Melee Attack Hook: Forward cone fallback for AI ped decisions
        PS2_GUEST_CC void meleeAttackHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ped = GPR_U32(ctx, 4); // attacker CPed*
            if (ped)
            {
                std::vector<NearCandidate> candidates;
                collectNearPedsInCone(rdram, ped, candidates);
                if (!candidates.empty())
                {
                    populateAttackerNearPeds(rdram, ped, candidates);
                }
            }

            if (g_origMeleeAttack)
            {
                ctx->pc = kMeleeAttack;
                g_origMeleeAttack(rdram, ctx, runtime);
            }
        }

        // 4. Player & Weapon Melee Attack Hook: CWeapon::FireMelee (0x002C1D80)
        PS2_GUEST_CC void fireMeleeHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t attacker = GPR_U32(ctx, 5);
            const uint32_t pFirePos = GPR_U32(ctx, 6);

            if (attacker)
            {
                std::vector<NearCandidate> candidates;
                collectNearPedsInCone(rdram, attacker, candidates);

                if (!candidates.empty())
                {
                    populateAttackerNearPeds(rdram, attacker, candidates);
                    adjustFirePosForTarget(rdram, attacker, pFirePos, candidates[0]);
                }
            }

            if (g_origFireMelee)
            {
                ctx->pc = kFireMelee;
                g_origFireMelee(rdram, ctx, runtime);
            }
        }
    }

    void install(PS2Runtime &runtime)
    {
        if ((g_origScanForEntities = runtime.lookupFunction(kScanForEntities)))
        {
            runtime.replaceFunction(kScanForEntities, &scanForEntitiesHook);
            std::fprintf(stderr, "[combat] CPed::ScanForEntities (0x1EEEB8) hooked\n");
        }
        else
        {
            std::fprintf(stderr, "[combat] Warning: CPed::ScanForEntities (0x1EEEB8) not found\n");
        }

        if ((g_origMeleeAttack = runtime.lookupFunction(kMeleeAttack)))
        {
            runtime.replaceFunction(kMeleeAttack, &meleeAttackHook);
            std::fprintf(stderr, "[combat] CPed::EvaluateMeleeAttack (0x1D47B8) hooked\n");
        }
        else
        {
            std::fprintf(stderr, "[combat] Warning: CPed::EvaluateMeleeAttack (0x1D47B8) not found\n");
        }

        if ((g_origFireMelee = runtime.lookupFunction(kFireMelee)))
        {
            runtime.replaceFunction(kFireMelee, &fireMeleeHook);
            std::fprintf(stderr, "[combat] CWeapon::FireMelee (0x2C1D80) hooked (player melee cone active)\n");
        }
        else
        {
            std::fprintf(stderr, "[combat] Warning: CWeapon::FireMelee (0x2C1D80) not found\n");
        }
    }
}
