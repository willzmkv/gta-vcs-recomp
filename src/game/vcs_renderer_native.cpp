#include "game/vcs_renderer_native.h"
#include "game/vcs_display.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include <ps2_recompiled_functions.h>
#include <ps2_recompiled_stubs.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

extern void ps2PushCheckpointDefer() noexcept;
extern void ps2PopCheckpointDefer() noexcept;

namespace vcs::renderer
{
namespace
{
    // ---- Memory / FPU / Guest-Call Helpers -------------------------------------------------

    constexpr uint32_t kRamMask = PS2_RAM_MASK;

    inline bool validRam(uint32_t a) noexcept
    {
        return a >= 0x00010000u && a < 0x02000000u;
    }

    inline uint8_t rd8(const uint8_t *rdram, uint32_t a) noexcept
    {
        return rdram[a & kRamMask];
    }

    inline uint16_t rd16(const uint8_t *rdram, uint32_t a) noexcept
    {
        uint16_t v;
        std::memcpy(&v, rdram + (a & kRamMask), 2);
        return v;
    }

    inline int16_t rdS16(const uint8_t *rdram, uint32_t a) noexcept
    {
        return static_cast<int16_t>(rd16(rdram, a));
    }

    inline uint32_t rd32(const uint8_t *rdram, uint32_t a) noexcept
    {
        uint32_t v;
        std::memcpy(&v, rdram + (a & kRamMask), 4);
        return v;
    }

    inline int32_t rdS32(const uint8_t *rdram, uint32_t a) noexcept
    {
        return static_cast<int32_t>(rd32(rdram, a));
    }

    inline uint64_t rd64(const uint8_t *rdram, uint32_t a) noexcept
    {
        uint64_t v;
        std::memcpy(&v, rdram + (a & kRamMask), 8);
        return v;
    }

    inline float rdF32(const uint8_t *rdram, uint32_t a) noexcept
    {
        float v;
        std::memcpy(&v, rdram + (a & kRamMask), 4);
        return v;
    }

    inline void wr8(uint8_t *rdram, uint32_t a, uint8_t v) noexcept
    {
        rdram[a & kRamMask] = v;
    }

    inline void wr16(uint8_t *rdram, uint32_t a, uint16_t v) noexcept
    {
        std::memcpy(rdram + (a & kRamMask), &v, 2);
    }

    inline void wr32(uint8_t *rdram, uint32_t a, uint32_t v) noexcept
    {
        std::memcpy(rdram + (a & kRamMask), &v, 4);
    }

    inline void wr64(uint8_t *rdram, uint32_t a, uint64_t v) noexcept
    {
        std::memcpy(rdram + (a & kRamMask), &v, 8);
    }

    inline void wrF32(uint8_t *rdram, uint32_t a, float v) noexcept
    {
        std::memcpy(rdram + (a & kRamMask), &v, 4);
    }

    inline int32_t ps2CvtWS(float f) noexcept
    {
        uint32_t bits;
        std::memcpy(&bits, &f, 4);
        if (((bits >> 23) & 0xFFu) >= 158u)
            return (bits & 0x80000000u) ? INT32_MIN : INT32_MAX;
        return static_cast<int32_t>(f);
    }

    inline int ps2Fcmp(float a, float b) noexcept
    {
        auto key = [](float x) noexcept -> int32_t {
            const uint32_t u = std::bit_cast<uint32_t>(x);
            const uint32_t mag = ((u & 0x7F800000u) == 0u) ? 0u : (u & 0x7FFFFFFFu);
            return (u & 0x80000000u) ? -static_cast<int32_t>(mag) : static_cast<int32_t>(mag);
        };
        const int32_t ka = key(a);
        const int32_t kb = key(b);
        return (ka > kb) - (ka < kb);
    }

    inline bool ps2Lt(float a, float b) noexcept { return ps2Fcmp(a, b) < 0; }
    inline bool ps2Le(float a, float b) noexcept { return ps2Fcmp(a, b) <= 0; }

    inline uint32_t getModelInfo(const uint8_t *rdram, uint32_t ent) noexcept
    {
        const int32_t idx = rdS16(rdram, ent + 0x56u);
        const int32_t count = rdS32(rdram, 0x004CD10Cu);
        if (idx >= 0 && idx < count)
            return rd32(rdram, rd32(rdram, 0x00486518u) + static_cast<uint32_t>(idx) * 4u);
        return 0u;
    }

    inline uint32_t getModelInfoByIndex(const uint8_t *rdram, int32_t idx) noexcept
    {
        const int32_t count = rdS32(rdram, 0x004CD10Cu);
        if (idx >= 0 && idx < count)
            return rd32(rdram, rd32(rdram, 0x00486518u) + static_cast<uint32_t>(idx) * 4u);
        return 0u;
    }

    struct CallArgsBackup
    {
        uint64_t r[8];
        uint64_t s[9];
        uint32_t sp;
        uint32_t ra;
        uint32_t pc;
        float f[4];

        explicit CallArgsBackup(const R5900Context *ctx) noexcept
        {
            for (int i = 0; i < 8; ++i)
                r[i] = GPR_U64(ctx, 4 + i);
            for (int i = 0; i < 8; ++i)
                s[i] = GPR_U64(ctx, 16 + i);
            s[8] = GPR_U64(ctx, 30);
            sp = GPR_U32(ctx, 29);
            ra = GPR_U32(ctx, 31);
            pc = ctx->pc;
            for (int i = 0; i < 4; ++i)
                f[i] = ctx->f[12 + i];
        }

        void restoreArgsAndFrame(R5900Context *ctx) const noexcept
        {
            for (int i = 0; i < 8; ++i)
                SET_GPR_U64(ctx, 4 + i, r[i]);
            for (int i = 0; i < 8; ++i)
                SET_GPR_U64(ctx, 16 + i, s[i]);
            SET_GPR_U64(ctx, 30, s[8]);
            SET_GPR_S32(ctx, 29, static_cast<int32_t>(sp));
            SET_GPR_S32(ctx, 31, static_cast<int32_t>(ra));
            ctx->pc = pc;
            for (int i = 0; i < 4; ++i)
                ctx->f[12 + i] = f[i];
        }

        void restoreSavedOnly(R5900Context *ctx) const noexcept
        {
            for (int i = 0; i < 8; ++i)
                SET_GPR_U64(ctx, 16 + i, s[i]);
            SET_GPR_U64(ctx, 30, s[8]);
            SET_GPR_S32(ctx, 29, static_cast<int32_t>(sp));
            SET_GPR_S32(ctx, 31, static_cast<int32_t>(ra));
            ctx->pc = pc;
        }
    };

    class CheckpointDeferGuard
    {
    public:
        CheckpointDeferGuard() noexcept : m_active(true) { ps2PushCheckpointDefer(); }
        ~CheckpointDeferGuard() noexcept { popNow(); }
        void popNow() noexcept
        {
            if (m_active)
            {
                m_active = false;
                ps2PopCheckpointDefer();
            }
        }
        CheckpointDeferGuard(const CheckpointDeferGuard &) = delete;
        CheckpointDeferGuard &operator=(const CheckpointDeferGuard &) = delete;

    private:
        bool m_active;
    };

    inline bool callWrapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                            PS2Runtime::RecompiledFunction fn, uint32_t targetPc, uint32_t retAddr,
                            uint32_t frameBytes = 0x20u) noexcept
    {
        if (!fn || !runtime)
            return false;
        const uint32_t savedSp = GPR_U32(ctx, 29);
        const uint32_t savedRa = GPR_U32(ctx, 31);
        const uint32_t callSp = (savedSp - frameBytes) & ~0xFu;
        SET_GPR_S32(ctx, 29, static_cast<int32_t>(callSp));
        SET_GPR_S32(ctx, 31, static_cast<int32_t>(retAddr));
        ctx->pc = targetPc;
        fn(rdram, ctx, runtime);
        const uint32_t postPc = ctx->pc;
        SET_GPR_S32(ctx, 29, static_cast<int32_t>(savedSp));
        SET_GPR_S32(ctx, 31, static_cast<int32_t>(savedRa));
        return !runtime->isStopRequested() && !g_checkpointUnwind && postPc == retAddr;
    }

    // ---- Hook Registry & Flags -------------------------------------------------------------

    enum HookId : int
    {
        // Group A: math, box, interior, frustum, sphere checks
        kH_PointInBox = 0,          // 0x00367528
        kH_ZoneBoxLookup,           // 0x003679D8
        kH_SectorBoxScan1,          // 0x00367AC8
        kH_SectorBoxScan2,          // 0x00367BB0
        kH_ClipEdgeZ,               // 0x0035E590
        kH_GetCamDistSq,            // 0x0036F0C0
        kH_IsEntityInActiveInterior,// 0x003C34D0
        kH_CalcFrustumPlanes,       // 0x003814A0
        kH_IsSphereVisible,         // 0x003816F8
        kH_RetZero241A80,           // 0x00241A80
        kH_GetPedFadeAlphaMode,     // 0x003774A8
        kH_TestEntitySphereGate,    // 0x003C42C0
        kH_TestEntitySphereVisible, // 0x003C4318

        // Group B: visibility setup & world/sector traversal
        kH_SetupEntityVisibility,   // 0x0035EFD8
        kH_SetupBigBuildingVis,     // 0x0035F910
        kH_ScanSectorList,          // 0x0035FAE8
        kH_ScanBigBuildingList,     // 0x0035FED0
        kH_ScanSectorRequestModels, // 0x003602F8
        kH_ScanWorld,               // 0x0035E620
        kH_ScanWorldWrapper,        // 0x0035C310

        // Group C: render lists & CVisibilityPlugins
        kH_ResetVisibleLists,       // 0x0035C2C8
        kH_RenderBodies,            // 0x0035C618
        kH_RenderUnderwaterAlpha,   // 0x0035C710
        kH_RenderRoads,             // 0x0035C748
        kH_RenderEverythingBarRoads,// 0x0035CD98
        kH_RenderOpaqueVehicleProps,// 0x0035D0D0
        kH_RenderAlphaAndSorted,    // 0x0035D308
        kH_RenderFirstPersonVehicle,// 0x0035D560
        kH_RenderOneEntity,         // 0x0035D5E8
        kH_RenderEntityWithAlpha,   // 0x0035D610
        kH_SetAlphaAndRenderState,  // 0x0035DA40
        kH_InsertEntityIntoList,    // 0x0036D480
        kH_RenderClumpDist,         // 0x0036DB80
        kH_RenderAtomicAlphaCb,     // 0x0036DBF8
        kH_RenderWeaponPedAlphaCb,  // 0x0036EB70
        kH_SetAtomicAlpha,          // 0x0036ED60
        kH_ClearVehicleAlphaList,   // 0x0036ED70
        kH_ClearWeaponAlphaList,    // 0x0036ED90
        kH_ClearEntityAlphaLists,   // 0x0036EDB0
        kH_InsertEntitySortedFade,  // 0x0036EE70
        kH_RenderWeaponAlphaList,   // 0x0036EED0
        kH_RenderBoatAlphaList,     // 0x0036EF38
        kH_RenderSortedFadeList,    // 0x0036EFA8
        kH_RenderFadingInEntities,  // 0x0036F0A0

        kNumHooks
    };

    struct HookSlot
    {
        uint32_t addr;
        PS2Runtime::RecompiledFunction hook;
        PS2Runtime::RecompiledFunction orig;
        PS2Runtime::RecompiledFunction outer;
    };

    extern HookSlot g_hooks[kNumHooks];

    // Downstream outer hooks (some installed before or after vcs::renderer::install)
    PS2Runtime::RecompiledFunction g_outer1B50B0 = nullptr; // RpWorld/RwFrameGetLTM
    PS2Runtime::RecompiledFunction g_outer36DA30 = nullptr; // Clump/Atomic helper
    PS2Runtime::RecompiledFunction g_outer36ED68 = nullptr; // CVisibilityPlugins::GetAtomicAlpha (vcs_effects_timing)
    PS2Runtime::RecompiledFunction g_outer3A52F8 = nullptr; // CBaseModelInfo::GetAtomicFromDistance (vcs_distance)
    PS2Runtime::RecompiledFunction g_outer3A5380 = nullptr; // CBaseModelInfo::GetLargestLodDistance (vcs_distance)
    PS2Runtime::RecompiledFunction g_outer3A53E0 = nullptr; // CBaseModelInfo::GetAtomicFromDistanceWithFlags (vcs_distance)
    PS2Runtime::RecompiledFunction g_outer3C3E58 = nullptr; // CEntity::Render (vcs_rslrender + vcs_distant)
    PS2Runtime::RecompiledFunction g_outer39EC18 = nullptr; // RpAtomicRender (vcs_rslrender)
    PS2Runtime::RecompiledFunction g_outer3CB900 = nullptr; // CStreaming::RequestModel (vcs_stream)
    PS2Runtime::RecompiledFunction g_outer35DEA8 = nullptr; // ScanSectorList_FrustumTriangle (vcs_distance)
    std::once_flag g_outerOnce;
    std::atomic<bool> g_outerReady{false};
    bool g_verify = false;
    int g_rangeLo = 0;
    int g_rangeHi = kNumHooks - 1;
    bool g_fullRange = true;

    void initOuterHooks(PS2Runtime *runtime) noexcept
    {
        std::call_once(g_outerOnce, [runtime]() {
            if (!runtime)
                return;
            for (int i = 0; i < kNumHooks; ++i)
                g_hooks[i].outer = runtime->lookupFunction(g_hooks[i].addr);
            g_outer1B50B0 = runtime->lookupFunction(0x001B50B0u);
            g_outer36DA30 = runtime->lookupFunction(0x0036DA30u);
            g_outer36ED68 = runtime->lookupFunction(0x0036ED68u);
            g_outer3A52F8 = runtime->lookupFunction(0x003A52F8u);
            g_outer3A5380 = runtime->lookupFunction(0x003A5380u);
            g_outer3A53E0 = runtime->lookupFunction(0x003A53E0u);
            g_outer3C3E58 = runtime->lookupFunction(0x003C3E58u);
            g_outer39EC18 = runtime->lookupFunction(0x0039EC18u);
            g_outer3CB900 = runtime->lookupFunction(0x003CB900u);
            g_outer35DEA8 = runtime->lookupFunction(0x0035DEA8u);
            g_outerReady.store(true, std::memory_order_release);
        });
    }

    inline void ensureOuterHooks(PS2Runtime *runtime) noexcept
    {
        if (!g_outerReady.load(std::memory_order_acquire))
            initOuterHooks(runtime);
    }

    inline bool enabled() noexcept
    {
        static const bool on = [] {
            const char *v = std::getenv("VCS_NATIVE_RENDERER");
            return !(v && v[0] == '0' && v[1] == '\0');
        }();
        return on;
    }

    inline bool verifyEnabled() noexcept
    {
        return g_verify;
    }

    inline bool hookActive(int id) noexcept
    {
        return id >= g_rangeLo && id <= g_rangeHi;
    }

    thread_local int t_verifyDepth = 0;

    struct VerifyStats
    {
        std::atomic<uint64_t> checked{0};
        std::atomic<uint64_t> diffs{0};
        std::atomic<uint64_t> fallbacks{0};
        std::atomic<uint64_t> perHookChecked[kNumHooks]{};
        std::atomic<uint64_t> perHookDiffs[kNumHooks]{};
        std::atomic<uint64_t> perHookFallbacks[kNumHooks]{};
        std::atomic<uint32_t> frames{0};
        std::atomic<uint32_t> perHookPrinted[kNumHooks]{};
        std::atomic<bool> dumpDone{false};
    } g_verifyStats;

    struct VerifyDepthGuard
    {
        VerifyDepthGuard() noexcept { ++t_verifyDepth; }
        ~VerifyDepthGuard() noexcept { --t_verifyDepth; }
    };

    inline bool bypassVerifyChild(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, int id) noexcept
    {
        if (!g_fullRange || g_verify)
        {
            if (!hookActive(id) || (g_verify && t_verifyDepth > 0))
            {
                g_hooks[id].orig(rdram, ctx, runtime);
                return true;
            }
        }
        return false;
    }

    inline void finishNative(R5900Context *ctx) noexcept
    {
        ctx->pc = GPR_U32(ctx, 31);
    }

    // ---- Downstream Guest Call Wrappers ----------------------------------------------------

    inline PS2Runtime::RecompiledFunction resolveOuterOrRaw(uint32_t addr,
                                                            PS2Runtime::RecompiledFunction outerFn,
                                                            PS2Runtime::RecompiledFunction rawFn,
                                                            PS2Runtime *runtime) noexcept
    {
        if (outerFn)
            return outerFn;
        return runtime ? runtime->lookupFunction(addr) : rawFn;
    }

    inline bool callVoid1(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                          PS2Runtime::RecompiledFunction fn, uint32_t targetPc, uint32_t retAddr,
                          uint32_t a0) noexcept
    {
        SET_GPR_S32(ctx, 4, static_cast<int32_t>(a0));
        return callWrapped(rdram, ctx, runtime, fn, targetPc, retAddr, 0x20u);
    }

    inline bool callU32_0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                          PS2Runtime::RecompiledFunction fn, uint32_t targetPc, uint32_t retAddr,
                          uint32_t &out) noexcept
    {
        if (!callWrapped(rdram, ctx, runtime, fn, targetPc, retAddr, 0x20u))
            return false;
        out = GPR_U32(ctx, 2);
        return true;
    }

    inline bool callU32_1(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                          PS2Runtime::RecompiledFunction fn, uint32_t targetPc, uint32_t retAddr,
                          uint32_t a0, uint32_t &out) noexcept
    {
        SET_GPR_S32(ctx, 4, static_cast<int32_t>(a0));
        if (!callWrapped(rdram, ctx, runtime, fn, targetPc, retAddr, 0x20u))
            return false;
        out = GPR_U32(ctx, 2);
        return true;
    }

    inline bool callU32_2(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                          PS2Runtime::RecompiledFunction fn, uint32_t targetPc, uint32_t retAddr,
                          uint32_t a0, uint32_t a1, uint32_t &out) noexcept
    {
        SET_GPR_S32(ctx, 4, static_cast<int32_t>(a0));
        SET_GPR_S32(ctx, 5, static_cast<int32_t>(a1));
        if (!callWrapped(rdram, ctx, runtime, fn, targetPc, retAddr, 0x20u))
            return false;
        out = GPR_U32(ctx, 2);
        return true;
    }

    inline bool callU32_3(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                          PS2Runtime::RecompiledFunction fn, uint32_t targetPc, uint32_t retAddr,
                          uint32_t a0, uint32_t a1, uint32_t a2, uint32_t &out) noexcept
    {
        SET_GPR_S32(ctx, 4, static_cast<int32_t>(a0));
        SET_GPR_S32(ctx, 5, static_cast<int32_t>(a1));
        SET_GPR_S32(ctx, 6, static_cast<int32_t>(a2));
        if (!callWrapped(rdram, ctx, runtime, fn, targetPc, retAddr, 0x20u))
            return false;
        out = GPR_U32(ctx, 2);
        return true;
    }

    inline bool callEntityVirtual(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                  uint32_t targetPc, uint32_t retAddr, uint32_t a0, uint32_t a1 = 0u,
                                  bool hasA1 = false, uint32_t *outV0 = nullptr) noexcept
    {
        PS2Runtime::RecompiledFunction fn = nullptr;
        if (targetPc == 0x003C3E58u)
            fn = resolveOuterOrRaw(0x003C3E58u, g_outer3C3E58, nullptr, runtime);
        else
            fn = runtime ? runtime->lookupFunction(targetPc) : nullptr;
        if (!fn)
            return false;
        SET_GPR_S32(ctx, 4, static_cast<int32_t>(a0));
        if (hasA1)
            SET_GPR_S32(ctx, 5, static_cast<int32_t>(a1));
        if (!callWrapped(rdram, ctx, runtime, fn, targetPc, retAddr, 0x20u))
            return false;
        if (outV0)
            *outV0 = GPR_U32(ctx, 2);
        return true;
    }

    // ---- Bounded State Snapshot for VERIFY=1 -----------------------------------------------

    struct LinkListSnap
    {
        uint32_t listAddr;
        uint8_t header[0x48];
        struct NodeEntry
        {
            uint32_t addr;
            uint8_t data[16];
        };
        std::vector<NodeEntry> nodes;

        void capture(const uint8_t *rdram, uint32_t addr)
        {
            listAddr = addr;
            nodes.clear();
            std::memcpy(header, rdram + (addr & kRamMask), 0x48);
            auto walk = [&](uint32_t startOff, uint32_t endOff) {
                uint32_t head = addr + endOff;
                uint32_t cur = rd32(rdram, addr + startOff);
                int guard = 0;
                while (validRam(cur) && cur != head && guard++ < 4096)
                {
                    NodeEntry ne{};
                    ne.addr = cur;
                    std::memcpy(ne.data, rdram + (cur & kRamMask), 16);
                    nodes.push_back(ne);
                    cur = rd32(rdram, cur + 8u); // next is at +8 (+8 next, +12 prev)
                }
            };
            walk(0x18u, 0x10u); // active list
            walk(0x2Cu, 0x30u); // free list (wait: +0x2C holds free node head ptr, sentinel at +0x30)
            // +0x2C itself is free head singly-linked via +0 (Wait: walk both +8 and +0 safely)
            uint32_t fcur = rd32(rdram, addr + 0x2Cu);
            int guard = 0;
            while (validRam(fcur) && fcur != addr + 0x30u && guard++ < 4096)
            {
                NodeEntry ne{};
                ne.addr = fcur;
                std::memcpy(ne.data, rdram + (fcur & kRamMask), 16);
                nodes.push_back(ne);
                uint32_t nxt8 = rd32(rdram, fcur + 8u);
                uint32_t nxt0 = rd32(rdram, fcur + 0u);
                fcur = validRam(nxt8) ? nxt8 : nxt0;
            }
        }

        void restore(uint8_t *rdram) const noexcept
        {
            std::memcpy(rdram + (listAddr & kRamMask), header, 0x48);
            for (const NodeEntry &ne : nodes)
                std::memcpy(rdram + (ne.addr & kRamMask), ne.data, 16);
        }

        bool equal(const LinkListSnap &other) const noexcept
        {
            if (std::memcmp(header, other.header, 0x48) != 0)
                return false;
            if (nodes.size() != other.nodes.size())
                return false;
            for (size_t i = 0; i < nodes.size(); ++i)
            {
                if (nodes[i].addr != other.nodes[i].addr ||
                    std::memcmp(nodes[i].data, other.nodes[i].data, 16) != 0)
                    return false;
            }
            return true;
        }
    };

    struct EntityLinkSnap
    {
        uint32_t entAddr = 0;
        uint32_t entHead44 = 0;
        uint32_t globalFreeHead = 0;
        struct Node8
        {
            uint32_t addr;
            uint8_t data[8];
        };
        std::vector<Node8> nodes;

        void capture(const uint8_t *rdram, uint32_t ent)
        {
            entAddr = ent;
            entHead44 = rd32(rdram, ent + 0x44u);
            globalFreeHead = rd32(rdram, 0x004CD5B8u);
            nodes.clear();
            auto walk = [&](uint32_t p) {
                int g = 0;
                while (validRam(p) && g++ < 512)
                {
                    Node8 n{};
                    n.addr = p;
                    std::memcpy(n.data, rdram + (p & kRamMask), 8);
                    nodes.push_back(n);
                    p = rd32(rdram, p);
                }
            };
            walk(entHead44);
            if (validRam(globalFreeHead))
            {
                Node8 n{};
                n.addr = globalFreeHead;
                std::memcpy(n.data, rdram + (globalFreeHead & kRamMask), 8);
                nodes.push_back(n);
            }
        }

        void restore(uint8_t *rdram) const noexcept
        {
            wr32(rdram, entAddr + 0x44u, entHead44);
            wr32(rdram, 0x004CD5B8u, globalFreeHead);
            for (const Node8 &n : nodes)
                std::memcpy(rdram + (n.addr & kRamMask), n.data, 8);
        }

        bool equal(const EntityLinkSnap &o) const noexcept
        {
            if (entHead44 != o.entHead44 || globalFreeHead != o.globalFreeHead || nodes.size() != o.nodes.size())
                return false;
            for (size_t i = 0; i < nodes.size(); ++i)
                if (nodes[i].addr != o.nodes[i].addr || std::memcmp(nodes[i].data, o.nodes[i].data, 8) != 0)
                    return false;
            return true;
        }
    };

    // ---- Forward Declarations of Impl Functions --------------------------------------------

    bool clipEdgeZImpl(uint8_t *rdram, R5900Context *ctx);
    bool pointInBoxImpl(uint8_t *rdram, R5900Context *ctx);
    bool zoneBoxLookupImpl(uint8_t *rdram, R5900Context *ctx);
    bool sectorBoxScan1Impl(uint8_t *rdram, R5900Context *ctx);
    bool sectorBoxScan2Impl(uint8_t *rdram, R5900Context *ctx);
    bool getCamDistSqImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    bool isEntityInActiveInteriorImpl(uint8_t *rdram, R5900Context *ctx);
    bool calcFrustumPlanesImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    bool isSphereVisibleImpl(uint8_t *rdram, R5900Context *ctx);
    bool retZero241A80Impl(uint8_t *rdram, R5900Context *ctx);
    bool getPedFadeAlphaModeImpl(uint8_t *rdram, R5900Context *ctx);
    bool testEntitySphereGateImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    bool testEntitySphereVisibleImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

    bool setupEntityVisibilityImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    bool setupBigBuildingVisibilityImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    bool scanSectorListImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    bool scanBigBuildingListImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    bool scanSectorRequestModelsImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    bool scanWorldImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    bool scanWorldWrapperImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

    bool resetVisibleListsImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    bool renderBodiesImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    bool renderUnderwaterAlphaImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    bool renderRoadsImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    bool renderEverythingBarRoadsImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    bool renderOpaqueVehiclePropsImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    bool renderAlphaAndSortedImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    bool renderFirstPersonVehicleImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    bool renderOneEntityImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    bool renderEntityWithAlphaImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    bool setAlphaAndRenderStateImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    bool insertEntityIntoListImpl(uint8_t *rdram, R5900Context *ctx);
    bool renderClumpDistImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    bool renderAtomicAlphaCbImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    bool renderWeaponPedAlphaCbImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    bool setAtomicAlphaImpl(uint8_t *rdram, R5900Context *ctx);
    bool clearVehicleAlphaListImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    bool clearWeaponAlphaListImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    bool clearEntityAlphaListsImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    bool insertEntitySortedFadeImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    bool renderWeaponAlphaListImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    bool renderBoatAlphaListImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    bool renderSortedFadeListImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    bool renderFadingInEntitiesImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

    // ---- Group A Implementations -----------------------------------------------------------

    // 0x00367528(ignored a0, pos a1, box a2) -> int
    bool pointInBoxImpl(uint8_t *rdram, R5900Context *ctx)
    {
        const uint32_t pos = GPR_U32(ctx, 5);
        const uint32_t box = GPR_U32(ctx, 6);
        const float px = rdF32(rdram, pos + 0u);
        const float py = rdF32(rdram, pos + 4u);
        const float pz = rdF32(rdram, pos + 8u);
        const bool ok = !ps2Lt(px, rdF32(rdram, box + 0x08u)) &&
                        !ps2Lt(rdF32(rdram, box + 0x14u), px) &&
                        !ps2Lt(py, rdF32(rdram, box + 0x0Cu)) &&
                        !ps2Lt(rdF32(rdram, box + 0x18u), py) &&
                        !ps2Lt(pz, rdF32(rdram, box + 0x10u)) &&
                        !ps2Lt(rdF32(rdram, box + 0x1Cu), pz);
        SET_GPR_S32(ctx, 2, ok ? 1 : 0);
        return true;
    }

    // 0x003679D8(obj a0, pos a1) -> int
    bool zoneBoxLookupImpl(uint8_t *rdram, R5900Context *ctx)
    {
        const uint32_t obj = GPR_U32(ctx, 4);
        const uint32_t pos = GPR_U32(ctx, 5);
        const float px = rdF32(rdram, pos + 0u);
        const float py = rdF32(rdram, pos + 4u);
        const float pz = rdF32(rdram, pos + 8u);
        if (ps2Le(pz, -100.0f))
        {
            SET_GPR_S32(ctx, 2, 3);
            return true;
        }
        const int32_t defVal = rdS32(rdram, 0x004CD118u);
        int32_t res = defVal;
        const uint32_t n = rd16(rdram, obj + 0x6Cu);
        const uint32_t base = rd32(rdram, obj + 0x20u);
        for (uint32_t i = 1u; i < n; ++i)
        {
            const uint32_t e = base + i * 0x38u;
            if (!ps2Lt(px, rdF32(rdram, e + 0x08u)) &&
                !ps2Lt(rdF32(rdram, e + 0x14u), px) &&
                !ps2Lt(py, rdF32(rdram, e + 0x0Cu)) &&
                !ps2Lt(rdF32(rdram, e + 0x18u), py))
            {
                int32_t v = rdS32(rdram, e + 0x24u);
                if (v == 4)
                    v = 1;
                else if (v == 5)
                    v = 2;
                if (v == defVal)
                {
                    SET_GPR_S32(ctx, 2, v);
                    return true;
                }
                res = v;
            }
        }
        SET_GPR_S32(ctx, 2, res);
        return true;
    }

    inline uint32_t boxScanCommon(const uint8_t *rdram, uint32_t base, uint32_t n, uint32_t pos) noexcept
    {
        const float px = rdF32(rdram, pos + 0u);
        const float py = rdF32(rdram, pos + 4u);
        const float pz = rdF32(rdram, pos + 8u);
        auto inBox = [&](uint32_t box) noexcept {
            return !ps2Lt(px, rdF32(rdram, box + 0x08u)) &&
                   !ps2Lt(rdF32(rdram, box + 0x14u), px) &&
                   !ps2Lt(py, rdF32(rdram, box + 0x0Cu)) &&
                   !ps2Lt(rdF32(rdram, box + 0x18u), py) &&
                   !ps2Lt(pz, rdF32(rdram, box + 0x10u)) &&
                   !ps2Lt(rdF32(rdram, box + 0x1Cu), pz);
        };
        for (uint32_t i = 1u; i < n; ++i)
        {
            const uint32_t e = base + i * 0x38u;
            if (inBox(e))
                return e;
        }
        return base;
    }

    // 0x00367AC8(obj a0, pos a1) -> uint32_t
    bool sectorBoxScan1Impl(uint8_t *rdram, R5900Context *ctx)
    {
        const uint32_t obj = GPR_U32(ctx, 4);
        const uint32_t pos = GPR_U32(ctx, 5);
        const uint32_t base = rd32(rdram, obj + 0x0Cu);
        const uint32_t n = rd16(rdram, obj + 0x18u);
        SET_GPR_S32(ctx, 2, static_cast<int32_t>(boxScanCommon(rdram, base, n, pos)));
        return true;
    }

    // 0x00367BB0(obj a0, pos a1) -> uint32_t
    bool sectorBoxScan2Impl(uint8_t *rdram, R5900Context *ctx)
    {
        const uint32_t obj = GPR_U32(ctx, 4);
        const uint32_t pos = GPR_U32(ctx, 5);
        const uint32_t base = rd32(rdram, obj + 0x10u);
        const uint32_t n = rd16(rdram, obj + 0x1Au);
        SET_GPR_S32(ctx, 2, static_cast<int32_t>(boxScanCommon(rdram, base, n, pos)));
        return true;
    }

    // 0x0035E590(out a0, p1 a1, p2 a2, f12=z)
    bool clipEdgeZImpl(uint8_t *rdram, R5900Context *ctx)
    {
        const uint32_t out = GPR_U32(ctx, 4);
        const uint32_t p1 = GPR_U32(ctx, 5);
        const uint32_t p2 = GPR_U32(ctx, 6);
        const float z = ctx->f[12];
        const float p1z = rdF32(rdram, p1 + 8u);
        const float p2z = rdF32(rdram, p2 + 8u);
        const float t = (z - p2z) / (p1z - p2z);
        const float p1x = rdF32(rdram, p1 + 0u), p2x = rdF32(rdram, p2 + 0u);
        const float p1y = rdF32(rdram, p1 + 4u), p2y = rdF32(rdram, p2 + 4u);
        wrF32(rdram, out + 0u, p2x + t * (p1x - p2x));
        wrF32(rdram, out + 4u, p2y + t * (p1y - p2y));
        wrF32(rdram, out + 8u, p2z + t * (p1z - p2z));
        return true;
    }

    // 0x0036F0C0(frame a0) -> f0 sqDist
    bool getCamDistSqImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        CheckpointDeferGuard deferGuard;
        CallArgsBackup bak(ctx);
        const uint32_t frame = GPR_U32(ctx, 4);
        uint32_t ltm = 0u;
        const auto fn1B50B0 = resolveOuterOrRaw(0x001B50B0u, g_outer1B50B0, FUN_001b50b0_0x1b50b0, runtime);
        if (!callU32_1(rdram, ctx, runtime, fn1B50B0, 0x001B50B0u, 0x0036F0D0u, frame, ltm))
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }
        bak.restoreSavedOnly(ctx);
        deferGuard.popNow();
        const uint32_t camPos = rd32(rdram, 0x00488E0Cu);
        const float dx = rdF32(rdram, ltm + 0x30u) - rdF32(rdram, camPos + 0u);
        const float dy = rdF32(rdram, ltm + 0x34u) - rdF32(rdram, camPos + 4u);
        const float dz = rdF32(rdram, ltm + 0x38u) - rdF32(rdram, camPos + 8u);
        ctx->f[0] = (dx * dx + dy * dy) + dz * dz;
        return true;
    }

    // 0x003C34D0(ent a0) -> int
    bool isEntityInActiveInteriorImpl(uint8_t *rdram, R5900Context *ctx)
    {
        const uint32_t ent = GPR_U32(ctx, 4);
        const uint64_t f = rd64(rdram, ent + 0x48u);
        if ((f & 0x000080000000000EULL) == 0x0000800000000002ULL)
        {
            SET_GPR_S32(ctx, 2, 1);
            return true;
        }
        const int32_t idx = rdS16(rdram, ent + 0x56u);
        const uint32_t zone = rd32(rdram, 0x0048A040u);
        static constexpr uint32_t kSlots[12] = {
            0x16u, 0x18u, 0x1Au, 0x1Cu,
            0xD2u, 0xD4u, 0xD6u, 0xD8u, 0xDAu, 0xDCu, 0xDEu, 0xE0u};
        for (uint32_t off : kSlots)
        {
            if (static_cast<int32_t>(rdS16(rdram, zone + off)) == idx)
            {
                SET_GPR_S32(ctx, 2, 1);
                return true;
            }
        }
        SET_GPR_S32(ctx, 2, 0);
        return true;
    }

    // 0x003814A0(cam a0)
    bool calcFrustumPlanesImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        CheckpointDeferGuard deferGuard;
        CallArgsBackup bak(ctx);
        const uint32_t cam = GPR_U32(ctx, 4);
        const uint32_t rwCam = rd32(rdram, cam + 0x7F4u);
        uint32_t frame = 0u;
        if (!callU32_1(rdram, ctx, runtime, FUN_0018f440_0x18f440, 0x0018F440u, 0x003814B8u, rwCam, frame))
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }
        uint32_t ltm = 0u;
        const auto fn1B50B0 = resolveOuterOrRaw(0x001B50B0u, g_outer1B50B0, FUN_001b50b0_0x1b50b0, runtime);
        if (!callU32_1(rdram, ctx, runtime, fn1B50B0, 0x001B50B0u, 0x003814C0u, frame, ltm))
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }
        bak.restoreSavedOnly(ctx);
        deferGuard.popNow();

        const uint32_t v1 = rd32(rdram, cam + 0x7F4u);
        const float px = rdF32(rdram, ltm + 0x30u);
        const float py = rdF32(rdram, ltm + 0x34u);
        const float pz = rdF32(rdram, ltm + 0x38u);
        const float ax = rdF32(rdram, ltm + 0x20u);
        const float ay = rdF32(rdram, ltm + 0x24u);
        const float az = rdF32(rdram, ltm + 0x28u);

        // Plane 0: right/left
        {
            const float s = rdF32(rdram, v1 + 0x68u) * (-0.5f);
            const float x = rdF32(rdram, ltm + 0x00u) * s + ax * 0.5f;
            const float y = rdF32(rdram, ltm + 0x04u) * s + ay * 0.5f;
            const float z = rdF32(rdram, ltm + 0x08u) * s + az * 0.5f;
            wrF32(rdram, v1 + 0x20u, x);
            wrF32(rdram, v1 + 0x30u, y);
            wrF32(rdram, v1 + 0x40u, z);
            wrF32(rdram, v1 + 0x50u, -((px * x + py * y) + pz * z));
        }
        // Plane 1: top/bottom
        {
            const float s = rdF32(rdram, v1 + 0x6Cu) * (-0.5f);
            const float x = rdF32(rdram, ltm + 0x10u) * s + ax * 0.5f;
            const float y = rdF32(rdram, ltm + 0x14u) * s + ay * 0.5f;
            const float z = rdF32(rdram, ltm + 0x18u) * s + az * 0.5f;
            wrF32(rdram, v1 + 0x24u, x);
            wrF32(rdram, v1 + 0x34u, y);
            wrF32(rdram, v1 + 0x44u, z);
            wrF32(rdram, v1 + 0x54u, -((px * x + py * y) + pz * z));
        }
        // Plane 2: at
        {
            wrF32(rdram, v1 + 0x28u, ax);
            wrF32(rdram, v1 + 0x38u, ay);
            wrF32(rdram, v1 + 0x48u, az);
            wrF32(rdram, v1 + 0x58u, -((px * ax + py * ay) + pz * az));
        }
        // Copy inv matrix *(cam + 0xB50) -> cam + 0xB10
        const uint32_t invPtr = rd32(rdram, cam + 0xB50u);
        const uint32_t dst = cam + 0xB10u;
        wrF32(rdram, cam + 0xB10u, rdF32(rdram, invPtr + 0x00u));
        wrF32(rdram, dst + 0x04u, rdF32(rdram, invPtr + 0x04u));
        wrF32(rdram, dst + 0x08u, rdF32(rdram, invPtr + 0x08u));
        wrF32(rdram, dst + 0x10u, rdF32(rdram, invPtr + 0x10u));
        wrF32(rdram, dst + 0x14u, rdF32(rdram, invPtr + 0x14u));
        wrF32(rdram, dst + 0x18u, rdF32(rdram, invPtr + 0x18u));
        wrF32(rdram, dst + 0x20u, rdF32(rdram, invPtr + 0x20u));
        wrF32(rdram, dst + 0x24u, rdF32(rdram, invPtr + 0x24u));
        wrF32(rdram, dst + 0x28u, rdF32(rdram, invPtr + 0x28u));
        wrF32(rdram, dst + 0x30u, rdF32(rdram, invPtr + 0x30u));
        wrF32(rdram, dst + 0x34u, rdF32(rdram, invPtr + 0x34u));
        wrF32(rdram, dst + 0x38u, rdF32(rdram, invPtr + 0x38u));
        return true;
    }

    // 0x003816F8(cam a0, centre a1, matrix a2, radius f12) -> int
    bool isSphereVisibleImpl(uint8_t *rdram, R5900Context *ctx)
    {
        const uint32_t cam = GPR_U32(ctx, 4);
        const uint32_t centre = GPR_U32(ctx, 5);
        const uint32_t mtx = GPR_U32(ctx, 6);
        const float radius = ctx->f[12];
        vcs::prepareSphereTest(rdram, cam);

        const float cx = rdF32(rdram, centre + 0u);
        const float cy = rdF32(rdram, centre + 4u);
        const float cz = rdF32(rdram, centre + 8u);

        const float tx = ((rdF32(rdram, mtx + 0x00u) * cx + rdF32(rdram, mtx + 0x10u) * cy) +
                          rdF32(rdram, mtx + 0x20u) * cz) +
                         rdF32(rdram, mtx + 0x30u);
        const float ty = ((rdF32(rdram, mtx + 0x04u) * cx + rdF32(rdram, mtx + 0x14u) * cy) +
                          rdF32(rdram, mtx + 0x24u) * cz) +
                         rdF32(rdram, mtx + 0x34u);
        const float tz = ((rdF32(rdram, mtx + 0x08u) * cx + rdF32(rdram, mtx + 0x18u) * cy) +
                          rdF32(rdram, mtx + 0x28u) * cz) +
                         rdF32(rdram, mtx + 0x38u);

        if (ps2Lt(ty + radius, rdF32(rdram, 0x004CD130u)) ||
            ps2Lt(rdF32(rdram, 0x004CD12Cu), ty - radius))
        {
            SET_GPR_S32(ctx, 2, 0);
            return true;
        }
        const float dR = tx * rdF32(rdram, cam + 0xB60u) + ty * rdF32(rdram, cam + 0xB64u);
        if (ps2Lt(radius, dR))
        {
            SET_GPR_S32(ctx, 2, 0);
            return true;
        }
        const float dL = tx * rdF32(rdram, cam + 0xB70u) + ty * rdF32(rdram, cam + 0xB74u);
        if (ps2Lt(radius, dL))
        {
            SET_GPR_S32(ctx, 2, 0);
            return true;
        }
        const float dB = ty * rdF32(rdram, cam + 0xB84u) + tz * rdF32(rdram, cam + 0xB88u);
        if (ps2Lt(radius, dB))
        {
            SET_GPR_S32(ctx, 2, 0);
            return true;
        }
        const float dT = ty * rdF32(rdram, cam + 0xB94u) + tz * rdF32(rdram, cam + 0xB98u);
        if (ps2Lt(radius, dT))
        {
            SET_GPR_S32(ctx, 2, 0);
            return true;
        }
        SET_GPR_S32(ctx, 2, 1);
        return true;
    }

    // 0x00241A80() -> 0
    bool retZero241A80Impl(uint8_t *, R5900Context *ctx)
    {
        SET_GPR_S32(ctx, 2, 0);
        return true;
    }

    // 0x003774A8(cam a0) -> int
    bool getPedFadeAlphaModeImpl(uint8_t *rdram, R5900Context *ctx)
    {
        const uint32_t cam = GPR_U32(ctx, 4);
        const uint32_t off = static_cast<uint32_t>(rd8(rdram, cam + 0x50u)) * 0x270u;
        const int16_t type = rdS16(rdram, cam + off + 0x70u);
        if (type == 0x12 || type == 0x10 || type == 0x16 || type == 4)
        {
            const int32_t val = rdS32(rdram, cam + off + 0xE4u);
            SET_GPR_S32(ctx, 2, val == 3 ? 3 : val);
            return true;
        }
        SET_GPR_S32(ctx, 2, 3);
        return true;
    }

    // 0x003C42C0(ent a0) -> int
    bool testEntitySphereGateImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ent = GPR_U32(ctx, 4);
        if (rd32(rdram, ent + 0x50u) == 0u)
        {
            SET_GPR_S32(ctx, 2, 0);
            return true;
        }
        const uint64_t f = rd64(rdram, ent + 0x48u);
        if ((f & 0x80000ULL) == 0ULL || ((f >> 52) & 1ULL) != 0ULL)
        {
            SET_GPR_S32(ctx, 2, 0);
            return true;
        }
        // Tail calls 0x003C4318
        if (hookActive(kH_TestEntitySphereVisible) && (!verifyEnabled() || t_verifyDepth == 0))
            return testEntitySphereVisibleImpl(rdram, ctx, runtime);
        CheckpointDeferGuard deferGuard;
        CallArgsBackup bak(ctx);
        const bool ok = callWrapped(rdram, ctx, runtime, g_hooks[kH_TestEntitySphereVisible].outer,
                                    0x003C4318u, 0x003C4304u, 0x20u);
        if (!ok)
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }
        bak.restoreSavedOnly(ctx);
        return true;
    }

    // 0x003C4318(ent a0) -> int
    bool testEntitySphereVisibleImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ent = GPR_U32(ctx, 4);
        const uint32_t mi = getModelInfo(rdram, ent);
        const uint32_t col = rd32(rdram, mi + 0x14u);
        const float lx = rdF32(rdram, col + 0x00u);
        const float ly = rdF32(rdram, col + 0x04u);
        const float lz = rdF32(rdram, col + 0x08u);

        float x = ((rdF32(rdram, ent + 0x00u) * lx + rdF32(rdram, ent + 0x10u) * ly) +
                   rdF32(rdram, ent + 0x20u) * lz) +
                  rdF32(rdram, ent + 0x30u);
        float y = ((rdF32(rdram, ent + 0x04u) * lx + rdF32(rdram, ent + 0x14u) * ly) +
                   rdF32(rdram, ent + 0x24u) * lz) +
                  rdF32(rdram, ent + 0x34u);
        float z = ((rdF32(rdram, ent + 0x08u) * lx + rdF32(rdram, ent + 0x18u) * ly) +
                   rdF32(rdram, ent + 0x28u) * lz) +
                  rdF32(rdram, ent + 0x38u);

        int32_t inActive = 0;
        {
            SET_GPR_S32(ctx, 4, static_cast<int32_t>(ent));
            if (hookActive(kH_IsEntityInActiveInterior) && (!verifyEnabled() || t_verifyDepth == 0))
            {
                isEntityInActiveInteriorImpl(rdram, ctx);
                inActive = GPR_S32(ctx, 2);
            }
            else
            {
                uint32_t r = 0u;
                CheckpointDeferGuard deferGuard;
                CallArgsBackup bak(ctx);
                if (!callU32_1(rdram, ctx, runtime, g_hooks[kH_IsEntityInActiveInterior].outer,
                               0x003C34D0u, 0x003C440Cu, ent, r))
                {
                    deferGuard.popNow();
                    bak.restoreArgsAndFrame(ctx);
                    return false;
                }
                bak.restoreSavedOnly(ctx);
                inActive = static_cast<int32_t>(r);
            }
        }

        if (inActive != 0 && ps2Lt(z, -150.0f) && ps2Lt(0.0f, x))
            z = -190.0f;

        const uint32_t mi2 = getModelInfo(rdram, ent);
        const uint32_t col2 = rd32(rdram, mi2 + 0x14u);
        const float radius = rdF32(rdram, col2 + 0x0Cu);

        // Allocate 16 bytes on guest stack for CVector centre
        const uint32_t sp = GPR_U32(ctx, 29);
        const uint32_t vecSp = (sp - 0x20u) & ~0xFu;
        wrF32(rdram, vecSp + 0u, x);
        wrF32(rdram, vecSp + 4u, y);
        wrF32(rdram, vecSp + 8u, z);

        // If native IsSphereVisible is active and not overridden by external outer hook (e.g. VCS_TRACE_VISIBLE)
        const bool canDirectSphere = hookActive(kH_IsSphereVisible) &&
                                     (!verifyEnabled() || t_verifyDepth == 0) &&
                                     (g_hooks[kH_IsSphereVisible].outer == g_hooks[kH_IsSphereVisible].hook);
        if (canDirectSphere)
        {
            SET_GPR_S32(ctx, 4, static_cast<int32_t>(0x006F44D0u));
            SET_GPR_S32(ctx, 5, static_cast<int32_t>(vecSp));
            SET_GPR_S32(ctx, 6, static_cast<int32_t>(0x006F44D0u + 0xAA0u));
            ctx->f[12] = radius;
            return isSphereVisibleImpl(rdram, ctx);
        }

        CheckpointDeferGuard deferGuard;
        CallArgsBackup bak(ctx);
        SET_GPR_S32(ctx, 29, static_cast<int32_t>(vecSp));
        SET_GPR_S32(ctx, 4, static_cast<int32_t>(0x006F44D0u));
        SET_GPR_S32(ctx, 5, static_cast<int32_t>(vecSp));
        SET_GPR_S32(ctx, 6, static_cast<int32_t>(0x006F44D0u + 0xAA0u));
        ctx->f[12] = radius;
        const bool ok = callWrapped(rdram, ctx, runtime, g_hooks[kH_IsSphereVisible].outer,
                                    0x003816F8u, 0x003C4488u, 0x20u);
        bak.restoreSavedOnly(ctx);
        deferGuard.popNow();
        return ok;
    }

    // ---- Helper Invokers Between Hooks -----------------------------------------------------

    inline bool invokeTestEntitySphereVisible(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                              uint32_t ent, uint32_t retAddr, int32_t &outVis) noexcept
    {
        if (hookActive(kH_TestEntitySphereVisible) && (!verifyEnabled() || t_verifyDepth == 0) &&
            g_hooks[kH_TestEntitySphereVisible].outer == g_hooks[kH_TestEntitySphereVisible].hook)
        {
            SET_GPR_S32(ctx, 4, static_cast<int32_t>(ent));
            if (testEntitySphereVisibleImpl(rdram, ctx, runtime))
            {
                outVis = GPR_S32(ctx, 2);
                return true;
            }
        }
        uint32_t r = 0u;
        if (!callU32_1(rdram, ctx, runtime, g_hooks[kH_TestEntitySphereVisible].outer,
                       0x003C4318u, retAddr, ent, r))
            return false;
        outVis = static_cast<int32_t>(r);
        return true;
    }

    inline bool invokeTestEntitySphereGate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                           uint32_t ent, uint32_t retAddr, int32_t &outVis) noexcept
    {
        if (hookActive(kH_TestEntitySphereGate) && (!verifyEnabled() || t_verifyDepth == 0) &&
            g_hooks[kH_TestEntitySphereGate].outer == g_hooks[kH_TestEntitySphereGate].hook)
        {
            SET_GPR_S32(ctx, 4, static_cast<int32_t>(ent));
            if (testEntitySphereGateImpl(rdram, ctx, runtime))
            {
                outVis = GPR_S32(ctx, 2);
                return true;
            }
        }
        uint32_t r = 0u;
        if (!callU32_1(rdram, ctx, runtime, g_hooks[kH_TestEntitySphereGate].outer,
                       0x003C42C0u, retAddr, ent, r))
            return false;
        outVis = static_cast<int32_t>(r);
        return true;
    }

    inline bool invokeInsertEntitySortedFade(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                             uint32_t ent, float dist, uint32_t retAddr, int32_t &outAdded) noexcept
    {
        if (hookActive(kH_InsertEntitySortedFade) && (!verifyEnabled() || t_verifyDepth == 0) &&
            g_hooks[kH_InsertEntitySortedFade].outer == g_hooks[kH_InsertEntitySortedFade].hook)
        {
            SET_GPR_S32(ctx, 4, static_cast<int32_t>(ent));
            ctx->f[12] = dist;
            if (insertEntitySortedFadeImpl(rdram, ctx, runtime))
            {
                outAdded = GPR_S32(ctx, 2);
                return true;
            }
        }
        SET_GPR_S32(ctx, 4, static_cast<int32_t>(ent));
        ctx->f[12] = dist;
        if (!callWrapped(rdram, ctx, runtime, g_hooks[kH_InsertEntitySortedFade].outer,
                         0x0036EE70u, retAddr, 0x20u))
            return false;
        outAdded = GPR_S32(ctx, 2);
        return true;
    }

    inline bool invokeLargestLodDist(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                     uint32_t mi, uint32_t entForS0, uint32_t retAddr, float &outDist) noexcept
    {
        const uint64_t savedS0 = GPR_U64(ctx, 16);
        SET_GPR_S32(ctx, 16, static_cast<int32_t>(entForS0));
        SET_GPR_S32(ctx, 4, static_cast<int32_t>(mi));
        const auto fn = resolveOuterOrRaw(0x003A5380u, g_outer3A5380, FUN_003a5380_0x3a5380, runtime);
        const bool ok = callWrapped(rdram, ctx, runtime, fn, 0x003A5380u, retAddr, 0x20u);
        outDist = ctx->f[0];
        SET_GPR_U64(ctx, 16, savedS0);
        return ok;
    }

    inline bool invokeAtomicFromDistance(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                         uint32_t mi, float dist, uint32_t entForS0, uint32_t retAddr,
                                         uint32_t &outAtomic) noexcept
    {
        const uint64_t savedS0 = GPR_U64(ctx, 16);
        SET_GPR_S32(ctx, 16, static_cast<int32_t>(entForS0));
        SET_GPR_S32(ctx, 4, static_cast<int32_t>(mi));
        ctx->f[12] = dist;
        const auto fn = resolveOuterOrRaw(0x003A52F8u, g_outer3A52F8, FUN_003a52f8_0x3a52f8, runtime);
        const bool ok = callWrapped(rdram, ctx, runtime, fn, 0x003A52F8u, retAddr, 0x20u);
        outAtomic = GPR_U32(ctx, 2);
        SET_GPR_U64(ctx, 16, savedS0);
        return ok;
    }

    inline bool invokeFunc3A53E0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                 uint32_t mi, float dist, uint32_t entForS0, uint32_t retAddr,
                                 uint32_t &outAtomic) noexcept
    {
        const uint64_t savedS0 = GPR_U64(ctx, 16);
        SET_GPR_S32(ctx, 16, static_cast<int32_t>(entForS0));
        SET_GPR_S32(ctx, 4, static_cast<int32_t>(mi));
        ctx->f[12] = dist;
        const auto fn = resolveOuterOrRaw(0x003A53E0u, g_outer3A53E0, FUN_003a53e0_0x3a53e0, runtime);
        const bool ok = callWrapped(rdram, ctx, runtime, fn, 0x003A53E0u, retAddr, 0x20u);
        outAtomic = GPR_U32(ctx, 2);
        SET_GPR_U64(ctx, 16, savedS0);
        return ok;
    }

    // ---- Group B Implementations -----------------------------------------------------------

    // 0x0035EFD8(ent a0) -> int (0 skip, 1 visible, 2 superlod/fadebox, 3 request model)
    bool setupEntityVisibilityImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        CheckpointDeferGuard deferGuard;
        CallArgsBackup bak(ctx);

        const uint32_t ent = GPR_U32(ctx, 4);
        const int32_t miIdx = rdS16(rdram, ent + 0x56u);
        const uint32_t mi = getModelInfoByIndex(rdram, miIdx);

        if (miIdx == 0x1D1E || miIdx == 0x18F5)
            wrF32(rdram, mi + 0x2Cu, 300.0f);

        const uint64_t flags = rd64(rdram, ent + 0x48u);
        const uint32_t typeBits = static_cast<uint32_t>(flags & 0xEu);

        // 0x35F078: type == 10 (ped?): if 0x3C42C0 == 0 return 2 else return 1
        if (typeBits == 10u)
        {
            int32_t g = 0;
            if (!invokeTestEntitySphereGate(rdram, ctx, runtime, ent, 0x0035F098u, g))
            {
                deferGuard.popNow();
                bak.restoreArgsAndFrame(ctx);
                return false;
            }
            bak.restoreSavedOnly(ctx);
            SET_GPR_S32(ctx, 2, g ? 1 : 2);
            return true;
        }

        const uint8_t miType = rd8(rdram, mi + 0x10u);
        uint32_t s3_dummy = 0u;
        int32_t s5_isVehicleOrType = 1;

        if (miType == 1u)
        {
            if (typeBits == 12u || typeBits == 8u)
            {
                if (miType == 0u && (flags & 0x02000000ULL) != 0ULL)
                {
                    const uint32_t mi3 = getModelInfo(rdram, ent);
                    s3_dummy = rd16(rdram, mi3 + 0x3Au) & 3u;
                }
                if (rd32(rdram, 0x0048F4F8u) == 0u)
                {
                    if (!callVoid1(rdram, ctx, runtime, FUN_00471458_0x471458, 0x00471458u, 0x0035F150u, 0u))
                    {
                        deferGuard.popNow();
                        bak.restoreArgsAndFrame(ctx);
                        return false;
                    }
                }
                uint32_t r241 = 0u;
                // FUN_00241A80 always returns 0, checked in Group A
                if (hookActive(kH_RetZero241A80) && (!verifyEnabled() || t_verifyDepth == 0))
                    r241 = 0u;
                else
                {
                    if (!callU32_3(rdram, ctx, runtime, g_hooks[kH_RetZero241A80].outer,
                                   0x00241A80u, 0x0035F160u, rd32(rdram, 0x0048F4F8u),
                                   static_cast<uint32_t>(miIdx), s3_dummy, r241))
                    {
                        deferGuard.popNow();
                        bak.restoreArgsAndFrame(ctx);
                        return false;
                    }
                }
                if (r241 != 0u)
                {
                    if (((rd64(rdram, ent + 0x48u) >> 19) & 1ULL) == 0ULL)
                    {
                        bak.restoreSavedOnly(ctx);
                        SET_GPR_S32(ctx, 2, 0);
                        return true;
                    }
                    int32_t vis = 0;
                    if (!invokeTestEntitySphereVisible(rdram, ctx, runtime, ent, 0x0035F18Cu, vis))
                    {
                        deferGuard.popNow();
                        bak.restoreArgsAndFrame(ctx);
                        return false;
                    }
                    if (vis == 0)
                    {
                        bak.restoreSavedOnly(ctx);
                        SET_GPR_S32(ctx, 2, 0);
                        return true;
                    }
                    if (rd32(rdram, 0x0048F4F8u) == 0u)
                    {
                        if (!callVoid1(rdram, ctx, runtime, FUN_00471458_0x471458, 0x00471458u, 0x0035F1A4u, 0u))
                        {
                            deferGuard.popNow();
                            bak.restoreArgsAndFrame(ctx);
                            return false;
                        }
                    }
                    uint32_t ign = 0u;
                    if (!callU32_3(rdram, ctx, runtime, FUN_00241d28_0x241d28, 0x00241D28u, 0x0035F1B4u,
                                   rd32(rdram, 0x0048F4F8u), r241, ent, ign))
                    {
                        deferGuard.popNow();
                        bak.restoreArgsAndFrame(ctx);
                        return false;
                    }
                    bak.restoreSavedOnly(ctx);
                    SET_GPR_S32(ctx, 2, 1);
                    return true;
                }
            }
        }
        else if (miType == 3u)
        {
            const uint8_t txSlot = rd8(rdram, mi + 0x40u);
            const uint8_t txBit = rd8(rdram, mi + 0x44u);
            const int32_t s1_sub = rdS32(rdram, mi + 0x48u);
            uint32_t r228 = 0u;
            if (!callU32_2(rdram, ctx, runtime, FUN_002287e8_0x2287e8, 0x002287E8u, 0x0035F1D8u,
                           txSlot, txBit, r228))
            {
                deferGuard.popNow();
                bak.restoreArgsAndFrame(ctx);
                return false;
            }
            if (r228 != 0u)
            {
                if (s1_sub != -1)
                {
                    const uint32_t subMi = getModelInfoByIndex(rdram, s1_sub);
                    const uint32_t vt = rd32(rdram, subMi + 0x24u);
                    const uint32_t fn = rd32(rdram, vt + 0x1Cu);
                    uint32_t rVt = 0u;
                    if (!callEntityVirtual(rdram, ctx, runtime, fn, 0x0035F278u, subMi, 0u, false, &rVt))
                    {
                        deferGuard.popNow();
                        bak.restoreArgsAndFrame(ctx);
                        return false;
                    }
                    if (rVt != 0u)
                        wr8(rdram, mi + 0x39u, static_cast<uint8_t>(rVt));
                    // branches to 0x35F56C (distance calculation) regardless of rVt
                }
                goto label_calc_distance;
            }
            else
            {
                if (s1_sub != -1)
                {
                    const uint32_t subMi = getModelInfoByIndex(rdram, s1_sub);
                    const uint32_t vt = rd32(rdram, subMi + 0x24u);
                    const uint32_t fn = rd32(rdram, vt + 0x1Cu);
                    uint32_t rVt = 0u;
                    if (!callEntityVirtual(rdram, ctx, runtime, fn, 0x0035F21Cu, subMi, 0u, false, &rVt))
                    {
                        deferGuard.popNow();
                        bak.restoreArgsAndFrame(ctx);
                        return false;
                    }
                    if (rVt == 0u)
                    {
                        s5_isVehicleOrType = 0;
                        goto label_calc_distance;
                    }
                }
                const uint32_t entVt = rd32(rdram, ent + 0x5Cu);
                const uint32_t fn28 = rd32(rdram, entVt + 0x28u);
                if (!callEntityVirtual(rdram, ctx, runtime, fn28, 0x0035F234u, ent))
                {
                    deferGuard.popNow();
                    bak.restoreArgsAndFrame(ctx);
                    return false;
                }
                bak.restoreSavedOnly(ctx);
                SET_GPR_S32(ctx, 2, 0);
                return true;
            }
        }

        // Non-miType-3 entity or fallthrough from miType == 1
        {
            const uint32_t curMiType = rd8(rdram, mi + 0x10u);
            if (curMiType != 1u && curMiType != 4u)
            {
                uint32_t playerEnt = 0u;
                if (!callU32_0(rdram, ctx, runtime, FUN_0024a2d0_0x24a2d0, 0x0024A2D0u, 0x0035F2A0u, playerEnt))
                {
                    deferGuard.popNow();
                    bak.restoreArgsAndFrame(ctx);
                    return false;
                }
                if (playerEnt == ent)
                {
                    const uint32_t camBase = 0x006F44D0u;
                    const uint32_t modeOff = static_cast<uint32_t>(rd8(rdram, camBase + 0x50u)) * 0x270u;
                    const int16_t camType = rdS16(rdram, camBase + modeOff + 0x70u);
                    if (camType == 0x10 &&
                        rd32(rdram, 0x004E4910u + static_cast<uint32_t>(rd8(rdram, 0x004CD128u)) * 0x190u + 4u) == 0u)
                    {
                        const uint32_t targetEnt = rd32(rdram, camBase + modeOff + 0xE4u);
                        uint32_t pEnt2 = 0u;
                        if (!callU32_0(rdram, ctx, runtime, FUN_0024a2d0_0x24a2d0, 0x0024A2D0u, 0x0035F304u, pEnt2))
                        {
                            deferGuard.popNow();
                            bak.restoreArgsAndFrame(ctx);
                            return false;
                        }
                        bool skipTo35F3F4 = false;
                        if (rd32(rdram, pEnt2 + 0x354u) == 6u)
                        {
                            uint32_t pEnt3 = 0u;
                            if (!callU32_0(rdram, ctx, runtime, FUN_0024a2d0_0x24a2d0, 0x0024A2D0u, 0x0035F31Cu, pEnt3))
                            {
                                deferGuard.popNow();
                                bak.restoreArgsAndFrame(ctx);
                                return false;
                            }
                            if ((rd64(rdram, pEnt3 + 0x588u) & (1ULL << 36)) != 0ULL)
                                skipTo35F3F4 = true;
                        }
                        if (!skipTo35F3F4)
                        {
                            if (targetEnt == 3u || miIdx == 0xF6 || miIdx == 0xED || miIdx == -966 ||
                                rd32(rdram, camBase + 0x7D4u) != 0u)
                            {
                                wr64(rdram, ent + 0x48u, rd64(rdram, ent + 0x48u) | (1ULL << 37));
                                bak.restoreSavedOnly(ctx);
                                SET_GPR_S32(ctx, 2, 2);
                                return true;
                            }
                            if (targetEnt != 0u)
                            {
                                wr32(rdram, 0x004CD5A8u, ent);
                                wr64(rdram, ent + 0x48u, rd64(rdram, ent + 0x48u) & ~(1ULL << 37));
                                bak.restoreSavedOnly(ctx);
                                SET_GPR_S32(ctx, 2, 2);
                                return true;
                            }
                            const uint32_t pObj = rd32(rdram, ent + 0x160u);
                            if ((rd32(rdram, pObj + 0xD0u) & 0x40000000u) != 0u)
                            {
                                wr64(rdram, ent + 0x48u, rd64(rdram, ent + 0x48u) | (1ULL << 37));
                                bak.restoreSavedOnly(ctx);
                                SET_GPR_S32(ctx, 2, 2);
                                return true;
                            }
                            if (rd32(rdram, ent + 0x354u) != static_cast<uint32_t>(s5_isVehicleOrType) ||
                                miIdx == 0xD6 || miIdx == 0xFA || miIdx == 0xDB || miIdx == 0x116)
                            {
                                wr32(rdram, 0x004CD5A8u, ent);
                                wr64(rdram, ent + 0x48u, rd64(rdram, ent + 0x48u) & ~(1ULL << 37));
                                bak.restoreSavedOnly(ctx);
                                SET_GPR_S32(ctx, 2, 2);
                                return true;
                            }
                        }
                    }
                }

                // 0x35F3F4 path
                if (rd32(rdram, ent + 0x50u) != 0u && (rd64(rdram, ent + 0x48u) & 0x80000ULL) != 0ULL)
                {
                    int32_t vis = 0;
                    if (!invokeTestEntitySphereVisible(rdram, ctx, runtime, ent, 0x0035F414u, vis))
                    {
                        deferGuard.popNow();
                        bak.restoreArgsAndFrame(ctx);
                        return false;
                    }
                    if (vis == 0)
                    {
                        bak.restoreSavedOnly(ctx);
                        SET_GPR_S32(ctx, 2, 2);
                        return true;
                    }
                    uint32_t occl = 0u;
                    if (!callU32_1(rdram, ctx, runtime, FUN_003fd028_0x3fd028, 0x003FD028u, 0x0035F424u, ent, occl))
                    {
                        deferGuard.popNow();
                        bak.restoreArgsAndFrame(ctx);
                        return false;
                    }
                    if (occl != 0u)
                    {
                        bak.restoreSavedOnly(ctx);
                        SET_GPR_S32(ctx, 2, 2);
                        return true;
                    }
                    const uint64_t ef = rd64(rdram, ent + 0x48u);
                    if ((ef & (1ULL << 36)) == 0ULL)
                    {
                        bak.restoreSavedOnly(ctx);
                        SET_GPR_S32(ctx, 2, 1);
                        return true;
                    }
                    const float dx = rdF32(rdram, ent + 0x30u) - rdF32(rdram, 0x006F3C00u);
                    const float dy = rdF32(rdram, ent + 0x34u) - rdF32(rdram, 0x006F3C04u);
                    const float dz = rdF32(rdram, ent + 0x38u) - rdF32(rdram, 0x006F3C08u);
                    const float d = std::sqrt((dx * dx + dy * dy) + dz * dz);
                    int32_t added = 0;
                    if (!invokeInsertEntitySortedFade(rdram, ctx, runtime, ent, d, 0x0035F758u, added))
                    {
                        deferGuard.popNow();
                        bak.restoreArgsAndFrame(ctx);
                        return false;
                    }
                    wr64(rdram, ent + 0x48u, rd64(rdram, ent + 0x48u) & ~(1ULL << 39));
                    bak.restoreSavedOnly(ctx);
                    SET_GPR_S32(ctx, 2, 0);
                    return true;
                }
                bak.restoreSavedOnly(ctx);
                SET_GPR_S32(ctx, 2, 0);
                return true;
            }
        }

        // 0x35F4A4 path (curMiType == 1 || curMiType == 4)
        if ((flags & (1ULL << 44)) != 0ULL)
        {
            if (rd32(rdram, ent + 0x50u) == 0u)
            {
                bak.restoreSavedOnly(ctx);
                SET_GPR_S32(ctx, 2, 0);
                return true;
            }
            if ((flags & 0x80000ULL) == 0ULL)
            {
                bak.restoreSavedOnly(ctx);
                SET_GPR_S32(ctx, 2, 0);
                return true;
            }
            int32_t vis = 0;
            if (!invokeTestEntitySphereVisible(rdram, ctx, runtime, ent, 0x0035F4DCu, vis))
            {
                deferGuard.popNow();
                bak.restoreArgsAndFrame(ctx);
                return false;
            }
            if (vis == 0)
            {
                bak.restoreSavedOnly(ctx);
                SET_GPR_S32(ctx, 2, 2);
                return true;
            }
            uint32_t occl = 0u;
            if (!callU32_1(rdram, ctx, runtime, FUN_003fd028_0x3fd028, 0x003FD028u, 0x0035F4ECu, ent, occl))
            {
                deferGuard.popNow();
                bak.restoreArgsAndFrame(ctx);
                return false;
            }
            if (occl != 0u)
            {
                bak.restoreSavedOnly(ctx);
                SET_GPR_S32(ctx, 2, 2);
                return true;
            }
            if ((rd64(rdram, ent + 0x48u) & (1ULL << 36)) == 0ULL)
            {
                bak.restoreSavedOnly(ctx);
                SET_GPR_S32(ctx, 2, 1);
                return true;
            }
            const float dx = rdF32(rdram, ent + 0x30u) - rdF32(rdram, 0x006F3C00u);
            const float dy = rdF32(rdram, ent + 0x34u) - rdF32(rdram, 0x006F3C04u);
            const float dz = rdF32(rdram, ent + 0x38u) - rdF32(rdram, 0x006F3C08u);
            const float d = std::sqrt((dx * dx + dy * dy) + dz * dz);
            int32_t added = 0;
            if (!invokeInsertEntitySortedFade(rdram, ctx, runtime, ent, d, 0x0035F758u, added))
            {
                deferGuard.popNow();
                bak.restoreArgsAndFrame(ctx);
                return false;
            }
            wr64(rdram, ent + 0x48u, rd64(rdram, ent + 0x48u) & ~(1ULL << 39));
            bak.restoreSavedOnly(ctx);
            SET_GPR_S32(ctx, 2, 0);
            return true;
        }

    label_calc_distance:
        const float dx = rdF32(rdram, ent + 0x30u) - rdF32(rdram, 0x006F3C00u);
        const float dy = rdF32(rdram, ent + 0x34u) - rdF32(rdram, 0x006F3C04u);
        const float dz = rdF32(rdram, ent + 0x38u) - rdF32(rdram, 0x006F3C08u);
        float dist = std::sqrt((dx * dx + dy * dy) + dz * dz);

        // 0x35F5AC: 330.0f check
        if (ps2Lt(330.0f, dist))
        {
            float maxLod = 0.0f;
            if (!invokeLargestLodDist(rdram, ctx, runtime, mi, ent, 0x0035F5ECu, maxLod))
            {
                deferGuard.popNow();
                bak.restoreArgsAndFrame(ctx);
                return false;
            }
            if (ps2Lt(dist, maxLod))
            {
                float maxLod2 = 0.0f;
                if (!invokeLargestLodDist(rdram, ctx, runtime, mi, ent, 0x0035F604u, maxLod2))
                {
                    deferGuard.popNow();
                    bak.restoreArgsAndFrame(ctx);
                    return false;
                }
                dist = maxLod2;
            }
        }

        const uint64_t efNow = rd64(rdram, ent + 0x48u);
        if ((efNow & 0xEu) == 8ULL && (efNow & 0x02000000ULL) != 0ULL)
        {
            wr16(rdram, mi + 0x3Au, rd16(rdram, mi + 0x3Au) | 8u);
        }

        uint32_t atomic = 0u;
        if (!invokeAtomicFromDistance(rdram, ctx, runtime, mi, dist, ent, 0x0035F64Cu, atomic))
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }

        if (atomic != 0u)
        {
            wr16(rdram, mi + 0x3Au, rd16(rdram, mi + 0x3Au) & 0xFFF7u);
            if (rd32(rdram, ent + 0x50u) == 0u)
            {
                const uint32_t vt = rd32(rdram, ent + 0x5Cu);
                const uint32_t fn24 = rd32(rdram, vt + 0x24u);
                if (!callEntityVirtual(rdram, ctx, runtime, fn24, 0x0035F680u, ent))
                {
                    deferGuard.popNow();
                    bak.restoreArgsAndFrame(ctx);
                    return false;
                }
            }
            const uint32_t rwObj = rd32(rdram, ent + 0x50u);
            const uint32_t atomicFrm = rd32(rdram, atomic + 0x14u);
            const uint32_t rwFrm = rd32(rdram, rwObj + 0x14u);
            if (atomicFrm != rwFrm)
            {
                uint32_t ign = 0u;
                if (!callU32_3(rdram, ctx, runtime, FUN_0039e160_0x39e160, 0x0039E160u, 0x0035F69Cu,
                               rwObj, atomicFrm, 4u, ign))
                {
                    deferGuard.popNow();
                    bak.restoreArgsAndFrame(ctx);
                    return false;
                }
            }
            const uint8_t curAlpha = rd8(rdram, mi + 0x39u);
            wr8(rdram, mi + 0x39u, (curAlpha < 0xEFu) ? static_cast<uint8_t>(curAlpha + 0x10u) : 0xFFu);

            if (rd32(rdram, ent + 0x50u) == 0u || (rd64(rdram, ent + 0x48u) & 0x80000ULL) == 0ULL)
            {
                bak.restoreSavedOnly(ctx);
                SET_GPR_S32(ctx, 2, 0);
                return true;
            }
            int32_t vis = 0;
            if (!invokeTestEntitySphereVisible(rdram, ctx, runtime, ent, 0x0035F6D8u, vis))
            {
                deferGuard.popNow();
                bak.restoreArgsAndFrame(ctx);
                return false;
            }
            if (vis == 0)
            {
                wr8(rdram, mi + 0x39u, 0xFFu);
                bak.restoreSavedOnly(ctx);
                SET_GPR_S32(ctx, 2, 2);
                return true;
            }
            uint32_t occl = 0u;
            if (!callU32_1(rdram, ctx, runtime, FUN_003fd028_0x3fd028, 0x003FD028u, 0x0035F6E8u, ent, occl))
            {
                deferGuard.popNow();
                bak.restoreArgsAndFrame(ctx);
                return false;
            }
            if (occl != 0u)
            {
                wr8(rdram, mi + 0x39u, 0xFFu);
                bak.restoreSavedOnly(ctx);
                SET_GPR_S32(ctx, 2, 2);
                return true;
            }
            const uint8_t postAlpha = rd8(rdram, mi + 0x39u);
            if (postAlpha != 0xFFu)
            {
                int32_t added = 0;
                if (!invokeInsertEntitySortedFade(rdram, ctx, runtime, ent, dist, 0x0035F708u, added))
                {
                    deferGuard.popNow();
                    bak.restoreArgsAndFrame(ctx);
                    return false;
                }
                wr64(rdram, ent + 0x48u, rd64(rdram, ent + 0x48u) | (1ULL << 39));
                bak.restoreSavedOnly(ctx);
                SET_GPR_S32(ctx, 2, 0);
                return true;
            }
            if ((rd16(rdram, mi + 0x3Au) & 0x40u) == 0u &&
                (rd64(rdram, ent + 0x48u) & (1ULL << 36)) == 0ULL)
            {
                bak.restoreSavedOnly(ctx);
                SET_GPR_S32(ctx, 2, 1);
                return true;
            }
            int32_t added = 0;
            if (!invokeInsertEntitySortedFade(rdram, ctx, runtime, ent, dist, 0x0035F758u, added))
            {
                deferGuard.popNow();
                bak.restoreArgsAndFrame(ctx);
                return false;
            }
            wr64(rdram, ent + 0x48u, rd64(rdram, ent + 0x48u) & ~(1ULL << 39));
            bak.restoreSavedOnly(ctx);
            SET_GPR_S32(ctx, 2, 0);
            return true;
        }
        else
        {
            const uint16_t f3A = rd16(rdram, mi + 0x3Au);
            wr16(rdram, mi + 0x3Au, f3A & 0xFFF7u);
            if ((f3A & 0x20u) != 0u)
            {
                float maxLod = 0.0f;
                if (!invokeLargestLodDist(rdram, ctx, runtime, mi, ent, 0x0035F7A0u, maxLod))
                {
                    deferGuard.popNow();
                    bak.restoreArgsAndFrame(ctx);
                    return false;
                }
                if (ps2Lt(dist - 30.0f, maxLod))
                {
                    bak.restoreSavedOnly(ctx);
                    SET_GPR_S32(ctx, 2, s5_isVehicleOrType == 0 ? 0 : 3);
                    return true;
                }
                bak.restoreSavedOnly(ctx);
                SET_GPR_S32(ctx, 2, 0);
                return true;
            }
            uint32_t fAtomic = 0u;
            if (!invokeFunc3A53E0(rdram, ctx, runtime, mi, dist - 20.0f, ent, 0x0035F7E4u, fAtomic))
            {
                deferGuard.popNow();
                bak.restoreArgsAndFrame(ctx);
                return false;
            }
            wr16(rdram, mi + 0x3Au, rd16(rdram, mi + 0x3Au) & 0xFFF7u);
            if (fAtomic != 0u)
            {
                if (rd32(rdram, ent + 0x50u) == 0u)
                {
                    const uint32_t vt = rd32(rdram, ent + 0x5Cu);
                    const uint32_t fn24 = rd32(rdram, vt + 0x24u);
                    if (!callEntityVirtual(rdram, ctx, runtime, fn24, 0x0035F814u, ent))
                    {
                        deferGuard.popNow();
                        bak.restoreArgsAndFrame(ctx);
                        return false;
                    }
                }
                const uint32_t rwObj = rd32(rdram, ent + 0x50u);
                const uint32_t atomicFrm = rd32(rdram, fAtomic + 0x14u);
                const uint32_t rwFrm = rd32(rdram, rwObj + 0x14u);
                if (atomicFrm != rwFrm)
                {
                    uint32_t ign = 0u;
                    if (!callU32_3(rdram, ctx, runtime, FUN_0039e160_0x39e160, 0x0039E160u, 0x0035F830u,
                                   rwObj, atomicFrm, 4u, ign))
                    {
                        deferGuard.popNow();
                        bak.restoreArgsAndFrame(ctx);
                        return false;
                    }
                }
                const uint8_t curAlpha = rd8(rdram, mi + 0x39u);
                wr8(rdram, mi + 0x39u, (curAlpha < 0xEFu) ? static_cast<uint8_t>(curAlpha + 0x10u) : 0xFFu);

                if (rd32(rdram, ent + 0x50u) == 0u || (rd64(rdram, ent + 0x48u) & 0x80000ULL) == 0ULL)
                {
                    bak.restoreSavedOnly(ctx);
                    SET_GPR_S32(ctx, 2, 0);
                    return true;
                }
                int32_t vis = 0;
                if (!invokeTestEntitySphereVisible(rdram, ctx, runtime, ent, 0x0035F86Cu, vis))
                {
                    deferGuard.popNow();
                    bak.restoreArgsAndFrame(ctx);
                    return false;
                }
                if (vis != 0)
                {
                    uint32_t occl = 0u;
                    if (!callU32_1(rdram, ctx, runtime, FUN_003fd028_0x3fd028, 0x003FD028u, 0x0035F87Cu, ent, occl))
                    {
                        deferGuard.popNow();
                        bak.restoreArgsAndFrame(ctx);
                        return false;
                    }
                    if (occl == 0u)
                    {
                        int32_t added = 0;
                        if (!invokeInsertEntitySortedFade(rdram, ctx, runtime, ent, dist, 0x0035F890u, added))
                        {
                            deferGuard.popNow();
                            bak.restoreArgsAndFrame(ctx);
                            return false;
                        }
                        wr64(rdram, ent + 0x48u, rd64(rdram, ent + 0x48u) | (1ULL << 39));
                        bak.restoreSavedOnly(ctx);
                        SET_GPR_S32(ctx, 2, 2);
                        return true;
                    }
                }
                wr8(rdram, mi + 0x39u, 0xFFu);
                bak.restoreSavedOnly(ctx);
                SET_GPR_S32(ctx, 2, 2);
                return true;
            }
            else
            {
                float maxLod = 0.0f;
                if (!invokeLargestLodDist(rdram, ctx, runtime, mi, ent, 0x0035F8BCu, maxLod))
                {
                    deferGuard.popNow();
                    bak.restoreArgsAndFrame(ctx);
                    return false;
                }
                const int32_t retVal = ps2Lt(dist - 50.0f, maxLod) ? (s5_isVehicleOrType ? 3 : 0) : 0;
                bak.restoreSavedOnly(ctx);
                SET_GPR_S32(ctx, 2, retVal);
                return true;
            }
        }
    }

    // 0x0035F910(ent a0, pos a1) -> int
    bool setupBigBuildingVisibilityImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        CheckpointDeferGuard deferGuard;
        CallArgsBackup bak(ctx);

        const uint32_t ent = GPR_U32(ctx, 4);
        const uint32_t pos = GPR_U32(ctx, 5);
        const int32_t miIdx = rdS16(rdram, ent + 0x56u);
        const uint32_t mi = getModelInfoByIndex(rdram, miIdx);
        uint8_t miType = rd8(rdram, mi + 0x10u);

        if (miType == 1u)
        {
            const uint32_t typeBits = static_cast<uint32_t>(rd64(rdram, ent + 0x48u) & 0xEu);
            if (typeBits == 12u || typeBits == 8u)
            {
                if (rd32(rdram, 0x0048F4F8u) == 0u)
                {
                    if (!callVoid1(rdram, ctx, runtime, FUN_00471458_0x471458, 0x00471458u, 0x0035F9C0u, 0u))
                    {
                        deferGuard.popNow();
                        bak.restoreArgsAndFrame(ctx);
                        return false;
                    }
                }
                uint32_t r241 = 0u;
                if (hookActive(kH_RetZero241A80) && (!verifyEnabled() || t_verifyDepth == 0))
                    r241 = 0u;
                else if (!callU32_3(rdram, ctx, runtime, g_hooks[kH_RetZero241A80].outer,
                                    0x00241A80u, 0x0035F9D0u, rd32(rdram, 0x0048F4F8u),
                                    static_cast<uint32_t>(miIdx), 0u, r241))
                {
                    deferGuard.popNow();
                    bak.restoreArgsAndFrame(ctx);
                    return false;
                }
                if (r241 != 0u)
                {
                    bak.restoreSavedOnly(ctx);
                    SET_GPR_S32(ctx, 2, 0);
                    return true;
                }
                miType = rd8(rdram, mi + 0x10u);
            }
        }
        if (miType == 3u)
        {
            uint32_t r228 = 0u;
            if (!callU32_2(rdram, ctx, runtime, FUN_002287e8_0x2287e8, 0x002287E8u, 0x0035F9F4u,
                           rd8(rdram, mi + 0x40u), rd8(rdram, mi + 0x44u), r228))
            {
                deferGuard.popNow();
                bak.restoreArgsAndFrame(ctx);
                return false;
            }
            if (r228 == 0u)
            {
                bak.restoreSavedOnly(ctx);
                SET_GPR_S32(ctx, 2, 0);
                return true;
            }
        }

        const float dx = rdF32(rdram, ent + 0x30u) - rdF32(rdram, pos + 0u);
        const float dy = rdF32(rdram, ent + 0x34u) - rdF32(rdram, pos + 4u);
        const float dz = rdF32(rdram, ent + 0x38u) - rdF32(rdram, pos + 8u);
        const float dist = std::sqrt((dx * dx + dy * dy) + dz * dz);

        const bool hasBit5 = (rd16(rdram, mi + 0x3Au) & 0x20u) != 0u;
        float maxLod = 0.0f;
        if (!invokeLargestLodDist(rdram, ctx, runtime, mi, ent,
                                  hasBit5 ? 0x0035FA70u : 0x0035FAA0u, maxLod))
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }
        const int32_t res = ps2Lt(dist - (hasBit5 ? 30.0f : 50.0f), maxLod) ? 1 : 0;
        bak.restoreSavedOnly(ctx);
        SET_GPR_S32(ctx, 2, res);
        return true;
    }

    inline bool invokeSetupEntityVisibility(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                            uint32_t ent, uint32_t retAddr, int32_t &outVis) noexcept
    {
        if (hookActive(kH_SetupEntityVisibility) && (!verifyEnabled() || t_verifyDepth == 0) &&
            g_hooks[kH_SetupEntityVisibility].outer == g_hooks[kH_SetupEntityVisibility].hook)
        {
            SET_GPR_S32(ctx, 4, static_cast<int32_t>(ent));
            if (setupEntityVisibilityImpl(rdram, ctx, runtime))
            {
                outVis = GPR_S32(ctx, 2);
                return true;
            }
        }
        uint32_t r = 0u;
        if (!callU32_1(rdram, ctx, runtime, g_hooks[kH_SetupEntityVisibility].outer,
                       0x0035EFD8u, retAddr, ent, r))
            return false;
        outVis = static_cast<int32_t>(r);
        return true;
    }

    template <bool IsBigBuilding, bool IsRequestModels>
    bool scanListCommon(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        CheckpointDeferGuard deferGuard;
        CallArgsBackup bak(ctx);

        const uint32_t sector = GPR_U32(ctx, 4);
        const uint16_t curScan = rd16(rdram, 0x00486468u);

        // Sector has 12 linked lists at offsets +0x08..+0x34
        for (int listIdx = 0; listIdx < 12; ++listIdx)
        {
            uint32_t node = rd32(rdram, sector + 0x08u + static_cast<uint32_t>(listIdx) * 4u);
            while (node != 0u)
            {
                const uint32_t ent = rd32(rdram, node + 0x00u);
                node = rd32(rdram, node + 0x08u);

                if (rd16(rdram, ent + 0x54u) == curScan)
                    continue;

                if constexpr (IsRequestModels)
                {
                    const uint64_t ef = rd64(rdram, ent + 0x48u);
                    if ((ef & (1ULL << 35)) == 0ULL)
                    {
                        wr16(rdram, ent + 0x54u, curScan);
                        const uint32_t tb = static_cast<uint32_t>(ef & 0xEu);
                        if (tb == 8u || tb == 6u || ((ef >> 49) & 1ULL) == 0ULL)
                            continue;
                    }
                    else
                    {
                        wr16(rdram, ent + 0x54u, curScan);
                    }
                }
                else
                {
                    wr16(rdram, ent + 0x54u, curScan);
                    wr64(rdram, ent + 0x48u, rd64(rdram, ent + 0x48u) & ~(1ULL << 42));
                }

                int32_t vis = 0;
                const uint32_t setupRet = IsRequestModels ? 0x003603C4u : (IsBigBuilding ? 0x0035FF64u : 0x0035FB7Cu);
                if (!invokeSetupEntityVisibility(rdram, ctx, runtime, ent, setupRet, vis))
                {
                    deferGuard.popNow();
                    bak.restoreArgsAndFrame(ctx);
                    return false;
                }

                if constexpr (!IsRequestModels)
                {
                    const uint64_t ef = rd64(rdram, ent + 0x48u);
                    if ((ef & (1ULL << 50)) != 0ULL)
                    {
                        wr64(rdram, ent + 0x48u, (ef & ~(1ULL << 50)) | 0x80000ULL);
                    }
                }

                if (vis == 1)
                {
                    const uint64_t ef = rd64(rdram, ent + 0x48u);
                    const uint32_t tb = static_cast<uint32_t>(ef & 0xEu);
                    if (tb == 4u || tb == 6u || ((ef >> 48) & 1ULL) != 0ULL || ((ef >> 49) & 1ULL) != 0ULL)
                    {
                        const uint32_t n = rd32(rdram, 0x004CD59Cu);
                        wr32(rdram, 0x00744AC0u + n * 4u, ent);
                        wr32(rdram, 0x004CD59Cu, n + 1u);
                    }
                    else
                    {
                        bool fade = false;
                        if (tb == 10u)
                        {
                            const int32_t entMi = rdS16(rdram, ent + 0x56u);
                            if (rd32(rdram, 0x0048F050u) == 0u)
                            {
                                uint32_t ign = 0u;
                                const uint32_t rPC = IsRequestModels ? 0x0036048Cu : (IsBigBuilding ? 0x003600F0u : 0x0035FCCCu);
                                callU32_0(rdram, ctx, runtime, FUN_0046ffb8_0x46ffb8, 0x0046FFB8u, rPC, ign);
                            }
                            const int32_t curArea = rdS32(rdram, rd32(rdram, 0x0048F050u) + 0x11Cu);
                            if (entMi != curArea)
                                fade = true;
                        }
                        if (!fade && tb == 8u && ((rd64(rdram, ent + 0x1E0u) >> 28) & 1ULL) != 0ULL)
                            fade = true;
                        if (fade)
                        {
                            const uint32_t n = rd32(rdram, 0x004CD5A0u);
                            wr32(rdram, 0x00746C80u + n * 4u, ent);
                            wr32(rdram, 0x004CD5A0u, n + 1u);
                        }
                        else
                        {
                            const uint32_t n = rd32(rdram, 0x004CD598u);
                            wr32(rdram, 0x00742B80u + n * 4u, ent);
                            wr32(rdram, 0x004CD598u, n + 1u);
                        }
                    }
                }
                else if (vis == 0)
                {
                    if constexpr (!IsRequestModels)
                    {
                        const uint32_t mi = getModelInfo(rdram, ent);
                        const uint8_t mt = rd8(rdram, mi + 0x10u);
                        bool hasBit = false;
                        if (mt == 1u || mt == 3u)
                            hasBit = (rd16(rdram, mi + 0x3Au) & 0x6000u) != 0u;
                        if (hasBit)
                        {
                            wr64(rdram, ent + 0x48u, rd64(rdram, ent + 0x48u) | (1ULL << 42));
                            const float dx = rdF32(rdram, 0x006F3C00u) - rdF32(rdram, ent + 0x30u);
                            const float dy = rdF32(rdram, 0x006F3C04u) - rdF32(rdram, ent + 0x34u);
                            if (ps2Lt(-30.0f, dx) && ps2Lt(dx, 30.0f) && ps2Lt(-30.0f, dy) && ps2Lt(dy, 30.0f))
                            {
                                const int32_t n = rdS32(rdram, 0x004CD5A4u);
                                if (n < 0x95)
                                {
                                    wr32(rdram, 0x00746A00u + static_cast<uint32_t>(n) * 4u, ent);
                                    wr32(rdram, 0x004CD5A4u, static_cast<uint32_t>(n + 1));
                                }
                            }
                        }
                    }
                }
                else if (vis == 2)
                {
                    if constexpr (!IsRequestModels)
                        wr64(rdram, ent + 0x48u, rd64(rdram, ent + 0x48u) | (1ULL << 42));
                    const float box = IsRequestModels ? 65.0f : 30.0f;
                    const float dx = rdF32(rdram, 0x006F3C00u) - rdF32(rdram, ent + 0x30u);
                    const float dy = rdF32(rdram, 0x006F3C04u) - rdF32(rdram, ent + 0x34u);
                    if (ps2Lt(-box, dx) && ps2Lt(dx, box) && ps2Lt(-box, dy) && ps2Lt(dy, box))
                    {
                        const int32_t n = rdS32(rdram, 0x004CD5A4u);
                        if (n < 0x95)
                        {
                            wr32(rdram, 0x00746A00u + static_cast<uint32_t>(n) * 4u, ent);
                            wr32(rdram, 0x004CD5A4u, static_cast<uint32_t>(n + 1));
                        }
                    }
                }
                else if (vis == 3)
                {
                    if constexpr (!IsRequestModels)
                    {
                        uint32_t disabled = 0u;
                        const uint32_t ccRet = IsBigBuilding ? 0x0035FFE4u : 0x0035FBFCu;
                        if (!callU32_0(rdram, ctx, runtime, FUN_003cc008_0x3cc008, 0x003CC008u, ccRet, disabled))
                        {
                            deferGuard.popNow();
                            bak.restoreArgsAndFrame(ctx);
                            return false;
                        }
                        if (disabled == 0u)
                        {
                            const int32_t miIdx = rdS16(rdram, ent + 0x56u);
                            if constexpr (IsBigBuilding)
                            {
                                uint32_t prio = 0u;
                                if ((rd64(rdram, ent + 0x48u) & 0xEu) == 8ULL)
                                {
                                    uint32_t avail = 0u;
                                    if (!callU32_1(rdram, ctx, runtime, FUN_003cba78_0x3cba78, 0x003CBA78u, 0x00360014u,
                                                   static_cast<uint32_t>(miIdx), avail))
                                    {
                                        deferGuard.popNow();
                                        bak.restoreArgsAndFrame(ctx);
                                        return false;
                                    }
                                    if (avail == 0u)
                                        prio = 0x20u;
                                }
                                uint32_t ign = 0u;
                                const auto reqFn = resolveOuterOrRaw(0x003CB900u, g_outer3CB900, FUN_003cb900_0x3cb900, runtime);
                                if (!callU32_3(rdram, ctx, runtime, reqFn, 0x003CB900u, 0x00360030u,
                                               static_cast<uint32_t>(miIdx), prio, 0x004B8900u, ign))
                                {
                                    deferGuard.popNow();
                                    bak.restoreArgsAndFrame(ctx);
                                    return false;
                                }
                                uint32_t loadedNow = 0u;
                                if (!callU32_1(rdram, ctx, runtime, FUN_003cba78_0x3cba78, 0x003CBA78u, 0x00360038u,
                                               static_cast<uint32_t>(miIdx), loadedNow))
                                {
                                    deferGuard.popNow();
                                    bak.restoreArgsAndFrame(ctx);
                                    return false;
                                }
                                if (loadedNow == 0u)
                                    wr32(rdram, 0x004CD5ACu, 1u);
                            }
                            else
                            {
                                if (rd32(rdram, 0x004CD5ACu) == 0u)
                                {
                                    uint32_t ign = 0u;
                                    if (!callU32_2(rdram, ctx, runtime, FUN_003cb938_0x3cb938, 0x003CB938u, 0x0035FC1Cu,
                                                   static_cast<uint32_t>(miIdx), 0x004B8900u, ign))
                                    {
                                        deferGuard.popNow();
                                        bak.restoreArgsAndFrame(ctx);
                                        return false;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        bak.restoreSavedOnly(ctx);
        return true;
    }

    bool scanSectorListImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        return scanListCommon<false, false>(rdram, ctx, runtime);
    }

    bool scanBigBuildingListImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        return scanListCommon<true, false>(rdram, ctx, runtime);
    }

    bool scanSectorRequestModelsImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        return scanListCommon<false, true>(rdram, ctx, runtime);
    }

    // 0x0035E620()
    bool scanWorldImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        CheckpointDeferGuard deferGuard;
        CallArgsBackup bak(ctx);

        if (rd32(rdram, 0x006F3BD0u) == 0u)
            wr32(rdram, 0x006F3BD0u, 1u);
        if (rd32(rdram, 0x006F3BF0u) == 0u)
            wr32(rdram, 0x006F3BF0u, 1u);

        const uint32_t camBase = 0x006F44D0u;
        const uint32_t rwCam = rd32(rdram, camBase + 0x7F4u);

        uint32_t vwPtr = 0u;
        if (!callU32_1(rdram, ctx, runtime, FUN_0018f4c0_0x18f4c0, 0x0018F4C0u, 0x0035E68Cu, rwCam, vwPtr))
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }
        // Call 0x0018F3F8(rwCam) -> f0 farClip
        SET_GPR_S32(ctx, 4, static_cast<int32_t>(rwCam));
        if (!callWrapped(rdram, ctx, runtime, FUN_0018f3f8_0x18f3f8, 0x0018F3F8u, 0x0035E698u, 0x20u))
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }
        const float farClip = ctx->f[0];
        const float vw = rdF32(rdram, vwPtr + 0u);
        const float vh = rdF32(rdram, vwPtr + 4u);

        uint32_t camFrame = 0u;
        if (!callU32_1(rdram, ctx, runtime, FUN_0018f440_0x18f440, 0x0018F440u, 0x0035E748u, rwCam, camFrame))
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }
        const uint32_t camMat = camFrame + 0x10u;

        wr32(rdram, 0x004CD5A8u, 0u);
        // ClearEntityAlphaLists
        if (hookActive(kH_ClearEntityAlphaLists) && (!verifyEnabled() || t_verifyDepth == 0) &&
            g_hooks[kH_ClearEntityAlphaLists].outer == g_hooks[kH_ClearEntityAlphaLists].hook)
        {
            if (!clearEntityAlphaListsImpl(rdram, ctx, runtime))
            {
                deferGuard.popNow();
                bak.restoreArgsAndFrame(ctx);
                return false;
            }
        }
        else if (!callWrapped(rdram, ctx, runtime, g_hooks[kH_ClearEntityAlphaLists].outer,
                              0x0036EDB0u, 0x0035E758u, 0x20u))
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }

        uint16_t scanCode = rd16(rdram, 0x00486468u);
        if (scanCode > 0xFFFEu)
        {
            if (!callWrapped(rdram, ctx, runtime, FUN_001943b8_0x1943b8, 0x001943B8u, 0x0035E774u, 0x20u))
            {
                deferGuard.popNow();
                bak.restoreArgsAndFrame(ctx);
                return false;
            }
            scanCode = 1u;
        }
        else
        {
            scanCode = static_cast<uint16_t>(scanCode + 1u);
        }
        wr16(rdram, 0x00486468u, scanCode);

        // Compare camera position / forward vs cached (0x6F3BC0 / 0x6F3BE0)
        {
            const float dx = rdF32(rdram, camBase + 0x30u) - rdF32(rdram, 0x006F3BC0u);
            const float dy = rdF32(rdram, camBase + 0x34u) - rdF32(rdram, 0x006F3BC4u);
            const float dz = rdF32(rdram, camBase + 0x38u) - rdF32(rdram, 0x006F3BC8u);
            const float d2 = (dx * dx + dy * dy) + dz * dz;
            uint32_t still = 0u;
            if (ps2Lt(d2, 16.0f))
            {
                const float dot = (rdF32(rdram, camBase + 0x10u) * rdF32(rdram, 0x006F3BE0u) +
                                   rdF32(rdram, camBase + 0x14u) * rdF32(rdram, 0x006F3BE4u)) +
                                  rdF32(rdram, camBase + 0x18u) * rdF32(rdram, 0x006F3BE8u);
                if (ps2Lt(0.98f, dot))
                    still = 1u;
            }
            wr32(rdram, 0x006F3C58u, still);
            wrF32(rdram, 0x006F3BC0u, rdF32(rdram, camBase + 0x30u));
            wrF32(rdram, 0x006F3BC4u, rdF32(rdram, camBase + 0x34u));
            wrF32(rdram, 0x006F3BC8u, rdF32(rdram, camBase + 0x38u));
            wrF32(rdram, 0x006F3BCCu, rdF32(rdram, camBase + 0x3Cu));
            wrF32(rdram, 0x006F3BE0u, rdF32(rdram, camBase + 0x10u));
            wrF32(rdram, 0x006F3BE4u, rdF32(rdram, camBase + 0x14u));
            wrF32(rdram, 0x006F3BE8u, rdF32(rdram, camBase + 0x18u));
            wrF32(rdram, 0x006F3BECu, rdF32(rdram, camBase + 0x1Cu));
        }

        // Stack buffer for 9 local points + 24B tri buffer + 16B bbox
        const uint32_t baseSp = (bak.sp - 0x140u) & ~0xFu;
        auto wrVec = [&](uint32_t off, float x, float y, float z) noexcept {
            wrF32(rdram, baseSp + off + 0u, x);
            wrF32(rdram, baseSp + off + 4u, y);
            wrF32(rdram, baseSp + off + 8u, z);
        };
        wrVec(0x00u, 0.0f, 0.0f, 0.0f);
        wrVec(0x10u, (-vw) * farClip, vh * farClip, farClip);
        wrVec(0x20u, vw * farClip, vh * farClip, farClip);
        wrVec(0x30u, vw * farClip, (-vh) * farClip, farClip);
        wrVec(0x40u, (-vw) * farClip, (-vh) * farClip, farClip);

        const float invFar = 1.0f / farClip;
        const float r300 = 300.0f;
        const uint32_t idxA = ps2Le(rdF32(rdram, camMat + 0x28u), 0.0f) ? 0x10u : 0x40u;
        const uint32_t idxB = (idxA == 0x10u) ? 0x20u : 0x30u;
        const float ax0 = invFar * (rdF32(rdram, baseSp + idxA + 0u) * r300);
        const float ay0 = invFar * (rdF32(rdram, baseSp + idxA + 4u) * r300);
        const float az0 = invFar * (rdF32(rdram, baseSp + idxA + 8u) * r300);
        const float bx0 = invFar * (rdF32(rdram, baseSp + idxB + 0u) * r300);
        const float by0 = invFar * (rdF32(rdram, baseSp + idxB + 4u) * r300);
        const float bz0 = invFar * (rdF32(rdram, baseSp + idxB + 8u) * r300);
        wrVec(0x50u, ax0, ay0, az0);
        wrVec(0x60u, bx0, by0, bz0);
        wrVec(0x70u, ax0 * 0.2f, ay0 * 0.2f, az0);
        wrVec(0x80u, bx0 * 0.2f, by0 * 0.2f, bz0);

        // Transform all 9 points in place by camMat via 0x175338
        SET_GPR_S32(ctx, 29, static_cast<int32_t>(baseSp));
        SET_GPR_S32(ctx, 4, static_cast<int32_t>(baseSp));
        SET_GPR_S32(ctx, 5, static_cast<int32_t>(baseSp));
        SET_GPR_S32(ctx, 6, 9);
        SET_GPR_S32(ctx, 7, static_cast<int32_t>(camMat));
        if (!callWrapped(rdram, ctx, runtime, FUN_00175338_0x175338, 0x00175338u, 0x0035EA18u, 0x20u))
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }

        wr32(rdram, 0x004CD5ACu, 0u);
        const uint8_t camModeIdx = rd8(rdram, camBase + 0x50u);
        const int16_t camType = rdS16(rdram, camBase + static_cast<uint32_t>(camModeIdx) * 0x270u + 0x70u);

        auto triScan = [&](float x0, float y0, float x1, float y1, float x2, float y2,
                           uint32_t scanFnAddr, uint32_t retPC) noexcept -> bool {
            const uint32_t tri = baseSp + 0x90u;
            wrF32(rdram, tri + 0x00u, x0);
            wrF32(rdram, tri + 0x04u, y0);
            wrF32(rdram, tri + 0x08u, x1);
            wrF32(rdram, tri + 0x0Cu, y1);
            wrF32(rdram, tri + 0x10u, x2);
            wrF32(rdram, tri + 0x14u, y2);
            const auto fn35DEA8 = resolveOuterOrRaw(0x0035DEA8u, g_outer35DEA8, FUN_0035dea8_0x35dea8, runtime);
            uint32_t ign = 0u;
            return callU32_3(rdram, ctx, runtime, fn35DEA8, 0x0035DEA8u, retPC, tri, 3u, scanFnAddr, ign);
        };

        auto toSectorXY = [&](float wx, float wy, float &sx, float &sy) noexcept {
            const float c = 0.0125f;
            sx = wx * c + 30.0f;
            sy = wy * c + 25.0f;
        };

        if (camType == 1 || camType == 0x25)
        {
            // Top-down / overhead sector box clipping against z = -100.0f
            const float zClip = -100.0f;
            uint32_t secBufSp = baseSp + 0xB0u;
            if (!callVoid1(rdram, ctx, runtime, FUN_002bf1a8_0x2bf1a8, 0x002BF1A8u, 0x0035EC80u, secBufSp))
            {
                deferGuard.popNow();
                bak.restoreArgsAndFrame(ctx);
                return false;
            }
            for (uint32_t cornerOff : {0x10u, 0x20u, 0x30u, 0x40u})
            {
                float clipPt[3]{};
                if (hookActive(kH_ClipEdgeZ) && (!verifyEnabled() || t_verifyDepth == 0))
                {
                    const uint32_t tmpOut = baseSp + 0xC0u;
                    SET_GPR_S32(ctx, 4, static_cast<int32_t>(tmpOut));
                    SET_GPR_S32(ctx, 5, static_cast<int32_t>(baseSp + cornerOff));
                    SET_GPR_S32(ctx, 6, static_cast<int32_t>(baseSp));
                    ctx->f[12] = zClip;
                    clipEdgeZImpl(rdram, ctx);
                    clipPt[0] = rdF32(rdram, tmpOut + 0u);
                    clipPt[1] = rdF32(rdram, tmpOut + 4u);
                }
                else
                {
                    const uint32_t tmpOut = baseSp + 0xC0u;
                    SET_GPR_S32(ctx, 4, static_cast<int32_t>(tmpOut));
                    SET_GPR_S32(ctx, 5, static_cast<int32_t>(baseSp + cornerOff));
                    SET_GPR_S32(ctx, 6, static_cast<int32_t>(baseSp));
                    ctx->f[12] = zClip;
                    if (!callWrapped(rdram, ctx, runtime, g_hooks[kH_ClipEdgeZ].outer, 0x0035E590u, 0x0035EC90u, 0x20u))
                    {
                        deferGuard.popNow();
                        bak.restoreArgsAndFrame(ctx);
                        return false;
                    }
                    clipPt[0] = rdF32(rdram, tmpOut + 0u);
                    clipPt[1] = rdF32(rdram, tmpOut + 4u);
                }
                if (ps2Lt(clipPt[0], rdF32(rdram, secBufSp + 0x00u)))
                    wrF32(rdram, secBufSp + 0x00u, clipPt[0]);
                if (ps2Lt(rdF32(rdram, secBufSp + 0x08u), clipPt[0]))
                    wrF32(rdram, secBufSp + 0x08u, clipPt[0]);
                if (ps2Lt(clipPt[1], rdF32(rdram, secBufSp + 0x0Cu)))
                    wrF32(rdram, secBufSp + 0x0Cu, clipPt[1]);
                if (ps2Lt(rdF32(rdram, secBufSp + 0x04u), clipPt[1]))
                    wrF32(rdram, secBufSp + 0x04u, clipPt[1]);
            }
            const float c = 0.0125f;
            const int32_t t1 = ps2CvtWS(rdF32(rdram, secBufSp + 0x00u) * c + 30.0f);
            const int32_t t2 = ps2CvtWS(rdF32(rdram, secBufSp + 0x08u) * c + 30.0f);
            const int32_t t3 = ps2CvtWS(rdF32(rdram, secBufSp + 0x0Cu) * c + 25.0f);
            const int32_t t4 = ps2CvtWS(rdF32(rdram, secBufSp + 0x04u) * c + 25.0f);

            int32_t t0 = (0x31 < t1) ? 0x31 : t1;
            int32_t a3 = (0x31 < t2) ? 0x31 : t2;
            int32_t a1 = (0x31 < t3) ? 0x31 : t3;
            int32_t a2 = (0x31 < t4) ? 0x31 : t4;
            if (t0 < 0) t0 = 0;
            if (a3 < 0) a3 = 0;
            if (a1 < 0) a1 = 0;
            if (a2 < 0) a2 = 0;

            const int32_t xMin = (0 < t0) ? t0 : 0;
            const int32_t xMax = (a3 < 0x31) ? a3 : 0x31;
            const int32_t yMin = (0 < a1) ? a1 : 0;
            const int32_t yMax = (a2 < 0x31) ? a2 : 0x31;

            if (static_cast<uint32_t>(xMin) <= static_cast<uint32_t>(xMax) &&
                static_cast<uint32_t>(yMin) <= static_cast<uint32_t>(yMax))
            {
                for (uint32_t sx = static_cast<uint32_t>(xMin); sx <= static_cast<uint32_t>(xMax); ++sx)
                {
                    for (uint32_t sy = static_cast<uint32_t>(yMin); sy <= static_cast<uint32_t>(yMax); ++sy)
                    {
                        uint32_t secPtr = 0u;
                        if (!callU32_2(rdram, ctx, runtime, FUN_00472698_0x472698, 0x00472698u, 0x0035EF78u,
                                       sx, sy, secPtr))
                        {
                            deferGuard.popNow();
                            bak.restoreArgsAndFrame(ctx);
                            return false;
                        }
                        SET_GPR_S32(ctx, 4, static_cast<int32_t>(secPtr));
                        if (!callWrapped(rdram, ctx, runtime, g_hooks[kH_ScanSectorList].outer,
                                         0x0035FAE8u, 0x0035EF84u, 0x20u))
                        {
                            deferGuard.popNow();
                            bak.restoreArgsAndFrame(ctx);
                            return false;
                        }
                    }
                }
            }
            bak.restoreSavedOnly(ctx);
            return true;
        }

        uint32_t playerEnt = 0u;
        if (!callU32_0(rdram, ctx, runtime, FUN_0024a3e0_0x24a3e0, 0x0024A3E0u, 0x0035EA54u, playerEnt))
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }

        if (playerEnt != 0u && ps2Lt(rdF32(rdram, playerEnt + 0x38u), 0.0f))
        {
            float sx0, sy0, sx5, sy5, sx6, sy6;
            toSectorXY(rdF32(rdram, baseSp + 0x00u), rdF32(rdram, baseSp + 0x04u), sx0, sy0);
            toSectorXY(rdF32(rdram, baseSp + 0x50u), rdF32(rdram, baseSp + 0x54u), sx5, sy5);
            toSectorXY(rdF32(rdram, baseSp + 0x60u), rdF32(rdram, baseSp + 0x64u), sx6, sy6);
            if (!triScan(sx0, sy0, sx5, sy5, sx6, sy6, 0x003602F8u, 0x0035EC68u))
            {
                deferGuard.popNow();
                bak.restoreArgsAndFrame(ctx);
                return false;
            }
            bak.restoreSavedOnly(ctx);
            return true;
        }

        if (ps2Lt(300.0f, farClip))
        {
            float sx0, sy0, sx7, sy7, sx8, sy8;
            const float c = 0.0125f;
            sx0 = rdF32(rdram, baseSp + 0x00u) * c + 30.0f;
            sy0 = rdF32(rdram, baseSp + 0x04u) * c + 25.0f;
            sx7 = rdF32(rdram, baseSp + 0x70u) * c + 30.0f;
            sy7 = rdF32(rdram, baseSp + 0x74u) * c + 25.0f;
            sx8 = rdF32(rdram, baseSp + 0x80u) * c + 30.0f;
            sy8 = rdF32(rdram, baseSp + 0x84u) * c + 25.0f;
            if (!triScan(sx0, sy0, sx7, sy7, sx8, sy8, 0x0035FED0u, 0x0035EB5Cu))
            {
                deferGuard.popNow();
                bak.restoreArgsAndFrame(ctx);
                return false;
            }
            float sx5, sy5, sx6, sy6;
            toSectorXY(rdF32(rdram, baseSp + 0x50u), rdF32(rdram, baseSp + 0x54u), sx5, sy5);
            toSectorXY(rdF32(rdram, baseSp + 0x60u), rdF32(rdram, baseSp + 0x64u), sx6, sy6);
            if (!triScan(sx0, sy0, sx5, sy5, sx6, sy6, 0x0035FAE8u, 0x0035EBD0u))
            {
                deferGuard.popNow();
                bak.restoreArgsAndFrame(ctx);
                return false;
            }
            bak.restoreSavedOnly(ctx);
            return true;
        }

        {
            float sx0, sy0, sx1, sy1, sx2, sy2;
            toSectorXY(rdF32(rdram, baseSp + 0x00u), rdF32(rdram, baseSp + 0x04u), sx0, sy0);
            toSectorXY(rdF32(rdram, baseSp + 0x10u), rdF32(rdram, baseSp + 0x14u), sx1, sy1);
            toSectorXY(rdF32(rdram, baseSp + 0x20u), rdF32(rdram, baseSp + 0x24u), sx2, sy2);
            if (!triScan(sx0, sy0, sx1, sy1, sx2, sy2, 0x0035FAE8u, 0x0035EC68u))
            {
                deferGuard.popNow();
                bak.restoreArgsAndFrame(ctx);
                return false;
            }
        }
        bak.restoreSavedOnly(ctx);
        return true;
    }

    // 0x0035C310()
    bool scanWorldWrapperImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        CheckpointDeferGuard deferGuard;
        CallArgsBackup bak(ctx);
        if (!callWrapped(rdram, ctx, runtime, FUN_003fbac0_0x3fbac0, 0x003FBAC0u, 0x0035C320u, 0x20u))
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }
        const uint32_t cam = 0x006F44D0u;
        wrF32(rdram, 0x006F3C00u, rdF32(rdram, cam + 0x30u));
        wrF32(rdram, 0x006F3C04u, rdF32(rdram, cam + 0x34u));
        wrF32(rdram, 0x006F3C08u, rdF32(rdram, cam + 0x38u));
        wrF32(rdram, 0x006F3C0Cu, rdF32(rdram, cam + 0x3Cu));
        wr16(rdram, 0x006F3C56u, 0u);
        wr16(rdram, 0x006F3C54u, 0u);

        if (hookActive(kH_ScanWorld) && (!verifyEnabled() || t_verifyDepth == 0) &&
            g_hooks[kH_ScanWorld].outer == g_hooks[kH_ScanWorld].hook)
        {
            const bool ok = scanWorldImpl(rdram, ctx, runtime);
            bak.restoreSavedOnly(ctx);
            return ok;
        }
        const bool ok = callWrapped(rdram, ctx, runtime, g_hooks[kH_ScanWorld].outer, 0x0035E620u, 0x0035C368u, 0x20u);
        bak.restoreSavedOnly(ctx);
        return ok;
    }

    // ---- Group C Implementations -----------------------------------------------------------

    // 0x0035C2C8()
    bool resetVisibleListsImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        CheckpointDeferGuard deferGuard;
        CallArgsBackup bak(ctx);
        wr32(rdram, 0x004CD598u, 0u);
        wr32(rdram, 0x004CD59Cu, 0u);
        wr32(rdram, 0x004CD5A0u, 0u);
        wr32(rdram, 0x004CD5A4u, 0u);
        const bool ok = callVoid1(rdram, ctx, runtime, FUN_0047b938_0x47b938, 0x0047B938u, 0x0035C2FCu, 0x006F3C10u);
        bak.restoreSavedOnly(ctx);
        return ok;
    }

    // 0x0035C618()
    bool renderBodiesImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        CheckpointDeferGuard deferGuard;
        CallArgsBackup bak(ctx);
        if (!callU32_2(rdram, ctx, runtime, FUN_00175198_0x175198, 0x00175198u, 0x0035C644u, 0xBu, 1u,
                       reinterpret_cast<uint32_t &>(bak.r[0]))) // dummy out
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }
        uint32_t ign = 0u;
        if (!callU32_0(rdram, ctx, runtime, FUN_001a6f50_0x1a6f50, 0x001A6F50u, 0x0035C64Cu, ign) ||
            !callU32_0(rdram, ctx, runtime, FUN_001a6fb0_0x1a6fb0, 0x001A6FB0u, 0x0035C654u, ign))
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }
        const int32_t count = rdS32(rdram, 0x004CD598u);
        for (int32_t i = 0; i < count; ++i)
        {
            const uint32_t ent = rd32(rdram, 0x00742B80u + static_cast<uint32_t>(i) * 4u);
            if ((rd64(rdram, ent + 0x48u) & 0xEu) == 2ULL)
            {
                const uint32_t mi = getModelInfo(rdram, ent);
                if ((rd16(rdram, mi + 0x3Au) & 4u) != 0u)
                {
                    SET_GPR_S32(ctx, 4, static_cast<int32_t>(ent));
                    if (!callWrapped(rdram, ctx, runtime, g_hooks[kH_RenderOneEntity].outer,
                                     0x0035D5E8u, 0x0035C6D8u, 0x20u))
                    {
                        deferGuard.popNow();
                        bak.restoreArgsAndFrame(ctx);
                        return false;
                    }
                }
            }
        }
        bak.restoreSavedOnly(ctx);
        return true;
    }

    // 0x0035C710()
    bool renderUnderwaterAlphaImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        CheckpointDeferGuard deferGuard;
        CallArgsBackup bak(ctx);
        uint32_t ign = 0u;
        if (!callU32_2(rdram, ctx, runtime, FUN_00175198_0x175198, 0x00175198u, 0x0035C724u, 0xBu, 1u, ign) ||
            !callU32_0(rdram, ctx, runtime, FUN_001a6f50_0x1a6f50, 0x001A6F50u, 0x0035C72Cu, ign) ||
            !callU32_0(rdram, ctx, runtime, FUN_001a6fb0_0x1a6fb0, 0x001A6FB0u, 0x0035C734u, ign) ||
            !callWrapped(rdram, ctx, runtime, g_hooks[kH_RenderFadingInEntities].outer, 0x0036F0A0u, 0x0035C73Cu, 0x20u))
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }
        bak.restoreSavedOnly(ctx);
        return true;
    }

    inline bool shouldDrawFadeEntityCommon(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                           uint32_t ent, bool isVehicleOrBuildingBranch,
                                           uint32_t getAlphaRetPC, uint32_t insertRetPC) noexcept
    {
        // Shared helper matching 0x35C820..0x35C890 (RenderRoads) & 0x35CF18..0x35D018 (RenderEverythingBarRoads)
        // & 0x35D3B4..0x35D4D0 (RenderAlphaAndSortedEntities)
        (void)isVehicleOrBuildingBranch;
        uint32_t alpha = 0xFFu;
        const uint32_t camMode354 = rd32(rdram, ent + 0x354u);
        bool testFade = false;
        if (camMode354 == 1u || camMode354 == 2u)
        {
            const uint32_t camBase = 0x006F44D0u;
            const int16_t camType = rdS16(rdram, camBase + static_cast<uint32_t>(rd8(rdram, camBase + 0x50u)) * 0x270u + 0x70u);
            if (camType == 0xE)
            {
                testFade = false;
            }
            else if (camType != 0x10)
            {
                uint32_t m = 0u;
                if (!callU32_1(rdram, ctx, runtime, g_hooks[kH_GetPedFadeAlphaMode].outer,
                               0x003774A8u, (getAlphaRetPC == 0x0035CF88u ? 0x0035CF60u : 0x0035D404u), camBase, m))
                    return false;
                if (m != 3u)
                {
                    if (!callU32_1(rdram, ctx, runtime, g_hooks[kH_GetPedFadeAlphaMode].outer,
                                   0x003774A8u, (getAlphaRetPC == 0x0035CF88u ? 0x0035CF74u : 0x0035D418u), camBase, m))
                        return false;
                    if (m != 0u)
                        return true; // s2 = 0 -> already skipped at 35CF74
                }
                const auto getAlphaFn = resolveOuterOrRaw(0x0036ED68u, g_outer36ED68, FUN_0036ed68_0x36ed68, runtime);
                if (!callU32_1(rdram, ctx, runtime, getAlphaFn, 0x0036ED68u, getAlphaRetPC, rd32(rdram, ent + 0x50u), alpha))
                    return false;
                testFade = (alpha == 0xFFu);
            }
            else
            {
                const auto getAlphaFn = resolveOuterOrRaw(0x0036ED68u, g_outer36ED68, FUN_0036ed68_0x36ed68, runtime);
                if (!callU32_1(rdram, ctx, runtime, getAlphaFn, 0x0036ED68u, getAlphaRetPC, rd32(rdram, ent + 0x50u), alpha))
                    return false;
                testFade = (alpha == 0xFFu);
            }
        }
        else
        {
            testFade = (rd64(rdram, ent + 0xE8u) & (1ULL << 58)) != 0ULL;
        }
        (void)insertRetPC;
        return testFade;
    }

    // 0x0035C748()
    bool renderRoadsImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        CheckpointDeferGuard deferGuard;
        CallArgsBackup bak(ctx);
        const int32_t count = rdS32(rdram, 0x004CD598u);
        for (int32_t i = 0; i < count; ++i)
        {
            const uint32_t ent = rd32(rdram, 0x00742B80u + static_cast<uint32_t>(i) * 4u);
            const uint64_t ef = rd64(rdram, ent + 0x48u);
            const uint32_t tb = static_cast<uint32_t>(ef & 0xEu);
            if (tb == 2u)
            {
                const uint32_t mi = getModelInfo(rdram, ent);
                if ((rd16(rdram, mi + 0x3Au) & 4u) != 0u)
                    continue;
            }
            if (tb == 6u || ((ef >> 49) & 1ULL) != 0ULL)
            {
                const auto getAlphaFn = resolveOuterOrRaw(0x0036ED68u, g_outer36ED68, FUN_0036ed68_0x36ed68, runtime);
                uint32_t alpha = 0u;
                if (!callU32_1(rdram, ctx, runtime, getAlphaFn, 0x0036ED68u, 0x0035C828u, rd32(rdram, ent + 0x50u), alpha))
                {
                    deferGuard.popNow();
                    bak.restoreArgsAndFrame(ctx);
                    return false;
                }
                if (alpha != 0xFFu)
                {
                    const float dx = rdF32(rdram, 0x006F3C00u) - rdF32(rdram, ent + 0x30u);
                    const float dy = rdF32(rdram, 0x006F3C04u) - rdF32(rdram, ent + 0x34u);
                    const float dz = rdF32(rdram, 0x006F3C08u) - rdF32(rdram, ent + 0x38u);
                    const float dist = std::sqrt((dx * dx + dy * dy) + dz * dz);
                    int32_t added = 0;
                    if (!invokeInsertEntitySortedFade(rdram, ctx, runtime, ent, dist, 0x0035C88Cu, added))
                    {
                        deferGuard.popNow();
                        bak.restoreArgsAndFrame(ctx);
                        return false;
                    }
                    if (added != 0)
                        continue;
                }
            }
            SET_GPR_S32(ctx, 4, static_cast<int32_t>(ent));
            SET_GPR_S32(ctx, 5, 0);
            if (!callWrapped(rdram, ctx, runtime, g_hooks[kH_RenderEntityWithAlpha].outer,
                             0x0035D610u, 0x0035C8B4u, 0x20u))
            {
                deferGuard.popNow();
                bak.restoreArgsAndFrame(ctx);
                return false;
            }
        }
        bak.restoreSavedOnly(ctx);
        return true;
    }

    // 0x0035CD98()
    bool renderEverythingBarRoadsImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        CheckpointDeferGuard deferGuard;
        CallArgsBackup bak(ctx);
        uint32_t ign = 0u;
        if (!callU32_2(rdram, ctx, runtime, FUN_00175198_0x175198, 0x00175198u, 0x0035CDD8u, 8u, 5u, ign) ||
            !callU32_2(rdram, ctx, runtime, FUN_00175198_0x175198, 0x00175198u, 0x0035CDE4u, 9u, 6u, ign) ||
            !callWrapped(rdram, ctx, runtime, FUN_0023e470_0x23e470, 0x0023E470u, 0x0035CDECu, 0x20u))
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }
        wr32(rdram, 0x004864C8u, 1u);
        if (!callU32_2(rdram, ctx, runtime, FUN_00175198_0x175198, 0x00175198u, 0x0035CE00u, 0xBu, 1u, ign) ||
            !callWrapped(rdram, ctx, runtime, g_hooks[kH_ClearEntityAlphaLists].outer, 0x0036EDB0u, 0x0035CE08u, 0x20u) ||
            !callWrapped(rdram, ctx, runtime, g_hooks[kH_ClearWeaponAlphaList].outer, 0x0036ED90u, 0x0035CE10u, 0x20u))
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }

        const int32_t count = rdS32(rdram, 0x004CD59Cu);
        for (int32_t i = 0; i < count; ++i)
        {
            const uint32_t ent = rd32(rdram, 0x00744AC0u + static_cast<uint32_t>(i) * 4u);
            const uint64_t ef = rd64(rdram, ent + 0x48u);
            const bool isPedBit = ((ef >> 49) & 1ULL) != 0ULL;
            uint32_t a0_cam3 = 0u;
            if (isPedBit && rd32(rdram, ent + 0x8C4u) == 0x37u)
            {
                uint32_t mode = 0u;
                if (!callU32_1(rdram, ctx, runtime, g_hooks[kH_GetPedFadeAlphaMode].outer,
                               0x003774A8u, 0x0035CE68u, 0x006F44D0u, mode))
                {
                    deferGuard.popNow();
                    bak.restoreArgsAndFrame(ctx);
                    return false;
                }
                a0_cam3 = (mode == 3u) ? 1u : 0u;
            }
            const uint32_t tb = static_cast<uint32_t>(ef & 0xEu);
            const bool path35CEBC = ((tb == 6u) || isPedBit) &&
                                    (rd32(rdram, ent + 0x8C4u) != 0x37u) &&
                                    (a0_cam3 == 0u);
            if (path35CEBC)
            {
                uint32_t defer = 0u;
                if (isPedBit)
                {
                    defer = static_cast<uint32_t>((rd64(rdram, ent + 0xE8u) >> 40) & 1ULL);
                }
                else
                {
                    if (!callU32_1(rdram, ctx, runtime, FUN_0035c058_0x35c058, 0x0035C058u, 0x0035CEE4u, ent, defer))
                    {
                        deferGuard.popNow();
                        bak.restoreArgsAndFrame(ctx);
                        return false;
                    }
                }
                if (defer == 0u)
                {
                    SET_GPR_S32(ctx, 4, static_cast<int32_t>(ent));
                    SET_GPR_S32(ctx, 5, 0);
                    if (!callWrapped(rdram, ctx, runtime, g_hooks[kH_RenderEntityWithAlpha].outer,
                                     0x0035D610u, 0x0035CEF8u, 0x20u))
                    {
                        deferGuard.popNow();
                        bak.restoreArgsAndFrame(ctx);
                        return false;
                    }
                }
                continue;
            }

            if (tb == 4u)
            {
                bool s2_skip = false;
                const uint32_t cMode = rd32(rdram, ent + 0x354u);
                if (cMode == 1u || cMode == 2u)
                {
                    const uint32_t camBase = 0x006F44D0u;
                    const int16_t camType = rdS16(rdram, camBase + static_cast<uint32_t>(rd8(rdram, camBase + 0x50u)) * 0x270u + 0x70u);
                    if (camType == 0xE)
                    {
                        s2_skip = false;
                    }
                    else if (camType != 0x10)
                    {
                        uint32_t m = 0u;
                        if (!callU32_1(rdram, ctx, runtime, g_hooks[kH_GetPedFadeAlphaMode].outer, 0x003774A8u, 0x0035CF60u, camBase, m))
                        {
                            deferGuard.popNow();
                            bak.restoreArgsAndFrame(ctx);
                            return false;
                        }
                        bool checkAlpha = (m == 3u);
                        if (!checkAlpha)
                        {
                            if (!callU32_1(rdram, ctx, runtime, g_hooks[kH_GetPedFadeAlphaMode].outer, 0x003774A8u, 0x0035CF74u, camBase, m))
                            {
                                deferGuard.popNow();
                                bak.restoreArgsAndFrame(ctx);
                                return false;
                            }
                            checkAlpha = (m == 0u);
                        }
                        if (checkAlpha)
                        {
                            const auto gaFn = resolveOuterOrRaw(0x0036ED68u, g_outer36ED68, FUN_0036ed68_0x36ed68, runtime);
                            uint32_t al = 0u;
                            if (!callU32_1(rdram, ctx, runtime, gaFn, 0x0036ED68u, 0x0035CF88u, rd32(rdram, ent + 0x50u), al))
                            {
                                deferGuard.popNow();
                                bak.restoreArgsAndFrame(ctx);
                                return false;
                            }
                            s2_skip = (al == 0xFFu);
                        }
                        else
                        {
                            s2_skip = true;
                        }
                    }
                    else
                    {
                        const auto gaFn = resolveOuterOrRaw(0x0036ED68u, g_outer36ED68, FUN_0036ed68_0x36ed68, runtime);
                        uint32_t al = 0u;
                        if (!callU32_1(rdram, ctx, runtime, gaFn, 0x0036ED68u, 0x0035CF88u, rd32(rdram, ent + 0x50u), al))
                        {
                            deferGuard.popNow();
                            bak.restoreArgsAndFrame(ctx);
                            return false;
                        }
                        s2_skip = (al == 0xFFu);
                    }
                }
                else
                {
                    s2_skip = (rd64(rdram, ent + 0xE8u) & (1ULL << 58)) != 0ULL;
                }
                if (!s2_skip)
                {
                    const float dx = rdF32(rdram, 0x006F3C00u) - rdF32(rdram, ent + 0x30u);
                    const float dy = rdF32(rdram, 0x006F3C04u) - rdF32(rdram, ent + 0x34u);
                    const float dz = rdF32(rdram, 0x006F3C08u) - rdF32(rdram, ent + 0x38u);
                    const float d = std::sqrt((dx * dx + dy * dy) + dz * dz);
                    int32_t added = 0;
                    if (!invokeInsertEntitySortedFade(rdram, ctx, runtime, ent, d, 0x0035D010u, added))
                    {
                        deferGuard.popNow();
                        bak.restoreArgsAndFrame(ctx);
                        return false;
                    }
                    if (added == 0)
                    {
                        SET_GPR_S32(ctx, 4, static_cast<int32_t>(ent));
                        SET_GPR_S32(ctx, 5, 0);
                        if (!callWrapped(rdram, ctx, runtime, g_hooks[kH_RenderEntityWithAlpha].outer,
                                         0x0035D610u, 0x0035D024u, 0x20u))
                        {
                            deferGuard.popNow();
                            bak.restoreArgsAndFrame(ctx);
                            return false;
                        }
                    }
                }
            }
        }

        if (!callWrapped(rdram, ctx, runtime, g_hooks[kH_RenderFadingInEntities].outer, 0x0036F0A0u, 0x0035D044u, 0x20u) ||
            !callWrapped(rdram, ctx, runtime, FUN_0022cff0_0x22cff0, 0x0022CFF0u, 0x0035D04Cu, 0x20u))
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }
        wr32(rdram, 0x00489EA8u, 0u);
        if (rd32(rdram, 0x0048F4FCu) == 0u)
        {
            if (!callWrapped(rdram, ctx, runtime, FUN_004714a0_0x4714a0, 0x004714A0u, 0x0035D068u, 0x20u))
            {
                deferGuard.popNow();
                bak.restoreArgsAndFrame(ctx);
                return false;
            }
        }
        if (!callVoid1(rdram, ctx, runtime, FUN_00108f30_0x108f30, 0x00108F30u, 0x0035D074u, rd32(rdram, 0x0048F4FCu)) ||
            !callU32_2(rdram, ctx, runtime, FUN_00175198_0x175198, 0x00175198u, 0x0035D080u, 0xBu, 0u, ign))
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }
        wr32(rdram, 0x004864C8u, 0u);
        bak.restoreSavedOnly(ctx);
        return true;
    }

    // 0x0035D0D0()
    bool renderOpaqueVehiclePropsImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        CheckpointDeferGuard deferGuard;
        CallArgsBackup bak(ctx);
        uint32_t ign = 0u;
        if (!callU32_2(rdram, ctx, runtime, FUN_00175198_0x175198, 0x00175198u, 0x0035D10Cu, 0xBu, 1u, ign))
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }
        wr32(rdram, 0x004864C8u, 1u);
        wr32(rdram, 0x004864CCu, 1u);
        if (!callWrapped(rdram, ctx, runtime, FUN_0019b118_0x19b118, 0x0019B118u, 0x0035D124u, 0x20u))
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }

        const int32_t count = rdS32(rdram, 0x004CD5A0u);
        for (int32_t i = 0; i < count; ++i)
        {
            const uint32_t ent = rd32(rdram, 0x00746C80u + static_cast<uint32_t>(i) * 4u);
            if ((rd64(rdram, ent + 0x48u) & 0xEu) == 10ULL)
            {
                SET_GPR_S32(ctx, 4, static_cast<int32_t>(ent));
                SET_GPR_S32(ctx, 5, 0);
                if (!callWrapped(rdram, ctx, runtime, g_hooks[kH_RenderEntityWithAlpha].outer,
                                 0x0035D610u, 0x0035D174u, 0x20u))
                {
                    deferGuard.popNow();
                    bak.restoreArgsAndFrame(ctx);
                    return false;
                }
                const uint32_t mi = getModelInfo(rdram, ent);
                if (mi != 0u && rd8(rdram, mi + 0x38u) >= 2u)
                {
                    const uint32_t atomicsPtr = rd32(rdram, mi + 0x28u);
                    const uint32_t hiAtomic = atomicsPtr ? rd32(rdram, atomicsPtr + 4u) : 0u;
                    const uint32_t rwObj = rd32(rdram, ent + 0x50u);
                    if (hiAtomic != 0u && rwObj != 0u)
                    {
                        const uint32_t frm0 = rd32(rdram, rwObj + 0x14u);
                        const uint32_t frmHi = rd32(rdram, hiAtomic + 0x14u);
                        if (!callU32_3(rdram, ctx, runtime, FUN_00120c40_0x120c40, 0x00120C40u, 0x0035D1F0u,
                                       frm0, 0x0035D0B8u, 0u, ign) ||
                            !callU32_3(rdram, ctx, runtime, FUN_00120c40_0x120c40, 0x00120C40u, 0x0035D200u,
                                       frmHi, 0x0035D0B8u, 0u, ign))
                        {
                            deferGuard.popNow();
                            bak.restoreArgsAndFrame(ctx);
                            return false;
                        }
                        wr32(rdram, 0x00489EB8u, 1u);
                        if (!callU32_3(rdram, ctx, runtime, FUN_0039e160_0x39e160, 0x0039E160u, 0x0035D218u,
                                       rwObj, frmHi, 4u, ign))
                        {
                            deferGuard.popNow();
                            bak.restoreArgsAndFrame(ctx);
                            return false;
                        }
                        SET_GPR_S32(ctx, 4, static_cast<int32_t>(ent));
                        SET_GPR_S32(ctx, 5, 0);
                        if (!callWrapped(rdram, ctx, runtime, g_hooks[kH_RenderEntityWithAlpha].outer,
                                         0x0035D610u, 0x0035D224u, 0x20u))
                        {
                            deferGuard.popNow();
                            bak.restoreArgsAndFrame(ctx);
                            return false;
                        }
                        const uint32_t baseAtomPtr = rd32(rdram, mi + 0x28u);
                        const uint32_t baseAtomic = baseAtomPtr ? rd32(rdram, baseAtomPtr) : 0u;
                        const uint32_t baseFrm = baseAtomic ? rd32(rdram, baseAtomic + 0x14u) : 0u;
                        if (!callU32_3(rdram, ctx, runtime, FUN_0039e160_0x39e160, 0x0039E160u, 0x0035D244u,
                                       rwObj, baseFrm, 4u, ign))
                        {
                            deferGuard.popNow();
                            bak.restoreArgsAndFrame(ctx);
                            return false;
                        }
                        wr32(rdram, 0x00489EB8u, 0u);
                    }
                }
            }
        }
        wr32(rdram, 0x004864CCu, 0u);
        const int32_t postCount = rdS32(rdram, 0x004CD5A0u);
        for (int32_t i = 0; i < postCount; ++i)
        {
            const uint32_t ent = rd32(rdram, 0x00746C80u + static_cast<uint32_t>(i) * 4u);
            const uint32_t rwObj = rd32(rdram, ent + 0x50u);
            if (!callU32_3(rdram, ctx, runtime, FUN_00120c40_0x120c40, 0x00120C40u, 0x0035D298u,
                           rd32(rdram, rwObj + 0x14u), 0x0035D0B8u, 0u, ign))
            {
                deferGuard.popNow();
                bak.restoreArgsAndFrame(ctx);
                return false;
            }
            if ((rd64(rdram, ent + 0x48u) & 0xEu) == 8ULL)
            {
                SET_GPR_S32(ctx, 4, static_cast<int32_t>(ent));
                SET_GPR_S32(ctx, 5, 0);
                if (!callWrapped(rdram, ctx, runtime, g_hooks[kH_RenderEntityWithAlpha].outer,
                                 0x0035D610u, 0x0035D2C0u, 0x20u))
                {
                    deferGuard.popNow();
                    bak.restoreArgsAndFrame(ctx);
                    return false;
                }
            }
        }
        bak.restoreSavedOnly(ctx);
        return true;
    }

    // 0x0035D308()
    bool renderAlphaAndSortedImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        CheckpointDeferGuard deferGuard;
        CallArgsBackup bak(ctx);
        wr32(rdram, 0x004864C8u, 1u);
        uint32_t ign = 0u;
        if (!callU32_2(rdram, ctx, runtime, FUN_00175198_0x175198, 0x00175198u, 0x0035D358u, 0xBu, 1u, ign) ||
            !callWrapped(rdram, ctx, runtime, FUN_002ea6a8_0x2ea6a8, 0x002EA6A8u, 0x0035D360u, 0x20u) ||
            !callWrapped(rdram, ctx, runtime, g_hooks[kH_ClearWeaponAlphaList].outer, 0x0036ED90u, 0x0035D368u, 0x20u) ||
            !callVoid1(rdram, ctx, runtime, FUN_0047b938_0x47b938, 0x0047B938u, 0x0035D370u, 0x006F3C10u))
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }

        const uint32_t itemSp = (bak.sp - 0x20u) & ~0xFu;
        const int32_t count = rdS32(rdram, 0x004CD59Cu);
        for (int32_t i = 0; i < count; ++i)
        {
            const uint32_t ent = rd32(rdram, 0x00744AC0u + static_cast<uint32_t>(i) * 4u);
            const uint64_t ef = rd64(rdram, ent + 0x48u);
            const uint32_t tb = static_cast<uint32_t>(ef & 0xEu);
            bool addIt = false;
            if (tb == 4u)
            {
                const uint32_t cMode = rd32(rdram, ent + 0x354u);
                if (cMode == 1u || cMode == 2u)
                {
                    const uint32_t camBase = 0x006F44D0u;
                    const int16_t camType = rdS16(rdram, camBase + static_cast<uint32_t>(rd8(rdram, camBase + 0x50u)) * 0x270u + 0x70u);
                    if (camType == 0xE)
                        addIt = false;
                    else if (camType != 0x10)
                    {
                        uint32_t m = 0u;
                        if (!callU32_1(rdram, ctx, runtime, g_hooks[kH_GetPedFadeAlphaMode].outer, 0x003774A8u, 0x0035D404u, camBase, m))
                        {
                            deferGuard.popNow();
                            bak.restoreArgsAndFrame(ctx);
                            return false;
                        }
                        bool checkAlpha = (m == 3u);
                        if (!checkAlpha)
                        {
                            if (!callU32_1(rdram, ctx, runtime, g_hooks[kH_GetPedFadeAlphaMode].outer, 0x003774A8u, 0x0035D418u, camBase, m))
                            {
                                deferGuard.popNow();
                                bak.restoreArgsAndFrame(ctx);
                                return false;
                            }
                            checkAlpha = (m == 0u);
                        }
                        if (checkAlpha)
                        {
                            const auto gaFn = resolveOuterOrRaw(0x0036ED68u, g_outer36ED68, FUN_0036ed68_0x36ed68, runtime);
                            uint32_t al = 0u;
                            if (!callU32_1(rdram, ctx, runtime, gaFn, 0x0036ED68u, 0x0035D42Cu, rd32(rdram, ent + 0x50u), al))
                            {
                                deferGuard.popNow();
                                bak.restoreArgsAndFrame(ctx);
                                return false;
                            }
                            addIt = (al == 0xFFu);
                        }
                    }
                    else
                    {
                        const auto gaFn = resolveOuterOrRaw(0x0036ED68u, g_outer36ED68, FUN_0036ed68_0x36ed68, runtime);
                        uint32_t al = 0u;
                        if (!callU32_1(rdram, ctx, runtime, gaFn, 0x0036ED68u, 0x0035D42Cu, rd32(rdram, ent + 0x50u), al))
                        {
                            deferGuard.popNow();
                            bak.restoreArgsAndFrame(ctx);
                            return false;
                        }
                        addIt = (al == 0xFFu);
                    }
                }
                else
                {
                    addIt = (rd64(rdram, ent + 0xE8u) & (1ULL << 58)) != 0ULL;
                }
            }
            else if (tb == 6u)
            {
                uint32_t defer = 0u;
                if (!callU32_1(rdram, ctx, runtime, FUN_0035c058_0x35c058, 0x0035C058u, 0x0035D470u, ent, defer))
                {
                    deferGuard.popNow();
                    bak.restoreArgsAndFrame(ctx);
                    return false;
                }
                addIt = (defer != 0u);
            }

            if (addIt)
            {
                const float dx = rdF32(rdram, 0x006F3C00u) - rdF32(rdram, ent + 0x30u);
                const float dy = rdF32(rdram, 0x006F3C04u) - rdF32(rdram, ent + 0x34u);
                const float dz = rdF32(rdram, 0x006F3C08u) - rdF32(rdram, ent + 0x38u);
                const float dSq = (dx * dx + dy * dy) + dz * dz;
                wr32(rdram, itemSp + 0u, ent);
                wrF32(rdram, itemSp + 4u, dSq);
                SET_GPR_S32(ctx, 29, static_cast<int32_t>(itemSp));
                if (!callU32_2(rdram, ctx, runtime, FUN_0047b990_0x47b990, 0x0047B990u, 0x0035D4D4u,
                               0x006F3C10u, itemSp, ign))
                {
                    deferGuard.popNow();
                    bak.restoreArgsAndFrame(ctx);
                    return false;
                }
            }
        }

        const uint32_t listHead = 0x006F3C10u;
        uint32_t cur = rd32(rdram, listHead + 0x18u);
        while (cur != listHead)
        {
            const uint32_t ent = rd32(rdram, cur + 0u);
            cur = rd32(rdram, cur + 8u);
            SET_GPR_S32(ctx, 4, static_cast<int32_t>(ent));
            SET_GPR_S32(ctx, 5, 0);
            if (!callWrapped(rdram, ctx, runtime, g_hooks[kH_RenderEntityWithAlpha].outer,
                             0x0035D610u, 0x0035D508u, 0x20u))
            {
                deferGuard.popNow();
                bak.restoreArgsAndFrame(ctx);
                return false;
            }
        }

        if (!callU32_2(rdram, ctx, runtime, FUN_00175198_0x175198, 0x00175198u, 0x0035D524u, 0xBu, 0u, ign))
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }
        wr32(rdram, 0x004864C8u, 0u);
        bak.restoreSavedOnly(ctx);
        return true;
    }

    // 0x0035D560()
    bool renderFirstPersonVehicleImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t fpEnt = rd32(rdram, 0x004CD5A8u);
        if (fpEnt == 0u)
            return true;
        CheckpointDeferGuard deferGuard;
        CallArgsBackup bak(ctx);
        uint32_t ign = 0u;
        static constexpr std::pair<uint32_t, uint32_t> kStates[6] = {
            {0xBu, 1u}, {6u, 1u}, {4u, 1u}, {0xAu, 1u}, {8u, 5u}, {9u, 6u}};
        for (int i = 0; i < 6; ++i)
        {
            if (!callU32_2(rdram, ctx, runtime, FUN_00175198_0x175198, 0x00175198u,
                           static_cast<uint32_t>(0x0035D584u + i * 12), kStates[i].first, kStates[i].second, ign))
            {
                deferGuard.popNow();
                bak.restoreArgsAndFrame(ctx);
                return false;
            }
        }
        SET_GPR_S32(ctx, 4, static_cast<int32_t>(fpEnt));
        SET_GPR_S32(ctx, 5, 0);
        if (!callWrapped(rdram, ctx, runtime, g_hooks[kH_RenderEntityWithAlpha].outer,
                         0x0035D610u, 0x0035D5CCu, 0x20u) ||
            !callU32_2(rdram, ctx, runtime, FUN_00175198_0x175198, 0x00175198u, 0x0035D5D8u, 0xBu, 0u, ign))
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }
        bak.restoreSavedOnly(ctx);
        return true;
    }

    // 0x0035D5E8(ent a0)
    bool renderOneEntityImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ent = GPR_U32(ctx, 4);
        const uint32_t vt = rd32(rdram, ent + 0x5Cu);
        const uint32_t fn44 = rd32(rdram, vt + 0x44u);
        CheckpointDeferGuard deferGuard;
        CallArgsBackup bak(ctx);
        const bool ok = callEntityVirtual(rdram, ctx, runtime, fn44, 0x0035D600u, ent);
        if (!ok)
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }
        bak.restoreSavedOnly(ctx);
        return true;
    }

    // 0x0035D610(ent a0, flag a1)
    bool renderOneEntitySubWithAlpha(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                     uint32_t ent, uint32_t a1Flag) noexcept
    {
        CheckpointDeferGuard deferGuard;
        CallArgsBackup bak(ctx);
        uint32_t dmaHead = 0u;
        if (!callU32_1(rdram, ctx, runtime, FUN_00476770_0x476770, 0x00476770u, 0x0035D640u, 0x00520F90u, dmaHead))
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }
        if ((0x000E6670u - dmaHead) <= 0x00012BFFu)
        {
            bak.restoreSavedOnly(ctx);
            return true;
        }

        const uint64_t ef = rd64(rdram, ent + 0x48u);
        const uint32_t tb = static_cast<uint32_t>(ef & 0xEu);
        if (a1Flag != 0u)
        {
            bool deferFade = false;
            if (tb == 10u)
            {
                if (rd32(rdram, 0x0048F050u) == 0u)
                {
                    uint32_t ign = 0u;
                    callU32_0(rdram, ctx, runtime, FUN_0046ffb8_0x46ffb8, 0x0046FFB8u, 0x0035D6A0u, ign);
                }
                const uint32_t curArea = rd32(rdram, rd32(rdram, 0x0048F050u) + 0x11Cu);
                if (static_cast<uint32_t>(rdS16(rdram, ent + 0x56u)) != curArea)
                    deferFade = true;
            }
            if (!deferFade && tb == 8u && ((rd64(rdram, ent + 0x1E0u) >> 28) & 1ULL) != 0ULL)
                deferFade = true;
            if (deferFade)
            {
                const uint32_t n = rd32(rdram, 0x004CD5A0u);
                wr32(rdram, 0x00746C80u + n * 4u, ent);
                wr32(rdram, 0x004CD5A0u, n + 1u);
                bak.restoreSavedOnly(ctx);
                return true;
            }
        }

        if (tb == 6u && ((ef >> 49) & 1ULL) != 0ULL)
        {
            //Wait: check 0x35D718..0x35D74C carefully:
            // If type==6, checks bit 49 (0x35D734). Even if type != 6, wait: `beql $v0, $zero, 0x35D758` falls to 0x35D754 if type==6!
            // If (type==6) -> checks 0x8C4(ent) == 0x37 -> bail! Wait if type==6 checks `ld 0x48; andi 0xE; xori 6`:
            // When type == 6 (v0 == 1): `bnel $v0, $zero, 0x35D748` takes `lw $v1, 0x8C4($s1)` -> if == 0x37 bail!
            // When type != 6 (v0 == 0): checks bit 49 -> if set checks `0x8C4($s1) == 0x37` -> bail!
        }
        if (tb == 6u || ((ef >> 49) & 1ULL) != 0ULL)
        {
            if (rd32(rdram, ent + 0x8C4u) == 0x37u)
            {
                bak.restoreSavedOnly(ctx);
                return true;
            }
        }

        const uint32_t vt = rd32(rdram, ent + 0x5Cu);
        uint32_t s4_effects = 0u;
        if (!callEntityVirtual(rdram, ctx, runtime, rd32(rdram, vt + 0x4Cu), 0x0035D764u, ent, 0u, false, &s4_effects))
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }
        int32_t inAct = 0;
        {
            SET_GPR_S32(ctx, 4, static_cast<int32_t>(ent));
            if (hookActive(kH_IsEntityInActiveInterior) && (!verifyEnabled() || t_verifyDepth == 0))
            {
                isEntityInActiveInteriorImpl(rdram, ctx);
                inAct = GPR_S32(ctx, 2);
            }
            else
            {
                uint32_t r = 0u;
                if (!callU32_1(rdram, ctx, runtime, g_hooks[kH_IsEntityInActiveInterior].outer,
                               0x003C34D0u, 0x0035D770u, ent, r))
                {
                    deferGuard.popNow();
                    bak.restoreArgsAndFrame(ctx);
                    return false;
                }
                inAct = static_cast<int32_t>(r);
            }
        }
        if (inAct != 0 && ps2Lt(rdF32(rdram, ent + 0x38u), -150.0f))
            wr32(rdram, rd32(rdram, ent + 0x50u) + 0x30u, 1u);

        int32_t s2_useLighting = 0;
        if (tb == 14u)
        {
            const uint32_t pedObj = rd32(rdram, ent + 0x160u);
            const uint32_t pedVt = rd32(rdram, pedObj + 0x84u);
            uint32_t r = 0u;
            if (!callEntityVirtual(rdram, ctx, runtime, rd32(rdram, pedVt + 0xCu), 0x0035D7D4u,
                                   pedObj, 0x00470C20u, true, &r))
            {
                deferGuard.popNow();
                bak.restoreArgsAndFrame(ctx);
                return false;
            }
            s2_useLighting = static_cast<int32_t>(r);
        }
        else
        {
            s2_useLighting = (tb == 4u) ? 1 : 0;
        }

        if (s2_useLighting != 0)
        {
            const auto fnDAA8 = runtime ? runtime->lookupFunction(0x0036DAA8u) : nullptr;
            if (!callVoid1(rdram, ctx, runtime, fnDAA8 ? fnDAA8 : FUN_0036daa8_0x36daa8, 0x0036DAA8u, 0x0035D7FCu,
                           rd32(rdram, ent + 0x50u)))
            {
                deferGuard.popNow();
                bak.restoreArgsAndFrame(ctx);
                return false;
            }
            if (!callWrapped(rdram, ctx, runtime, g_hooks[kH_ClearVehicleAlphaList].outer,
                             0x0036ED70u, 0x0035D804u, 0x20u))
            {
                deferGuard.popNow();
                bak.restoreArgsAndFrame(ctx);
                return false;
            }
        }

        if (tb == 4u)
        {
            const uint32_t drv = rd32(rdram, ent + 0x384u);
            if (drv != 0u && rd32(rdram, drv + 0x8C4u) == 0x37u)
            {
                const uint32_t dVt = rd32(rdram, drv + 0x5Cu);
                if (!callEntityVirtual(rdram, ctx, runtime, rd32(rdram, dVt + 0x44u), 0x0035D84Cu, drv))
                {
                    deferGuard.popNow();
                    bak.restoreArgsAndFrame(ctx);
                    return false;
                }
            }
            const uint8_t nPass = rd8(rdram, ent + 0x235u);
            for (int32_t p = 0; p < static_cast<int32_t>(nPass); ++p)
            {
                uint32_t passPtr = 0u;
                if (!callU32_2(rdram, ctx, runtime, FUN_003f3968_0x3f3968, 0x003F3968u, 0x0035D868u,
                               ent, static_cast<uint32_t>(p), passPtr))
                {
                    deferGuard.popNow();
                    bak.restoreArgsAndFrame(ctx);
                    return false;
                }
                if (passPtr != 0u)
                {
                    if (!callU32_2(rdram, ctx, runtime, FUN_003f3968_0x3f3968, 0x003F3968u, 0x0035D878u,
                                   ent, static_cast<uint32_t>(p), passPtr))
                    {
                        deferGuard.popNow();
                        bak.restoreArgsAndFrame(ctx);
                        return false;
                    }
                    if (rd32(rdram, passPtr + 0x8C4u) == 0x37u)
                    {
                        if (!callU32_2(rdram, ctx, runtime, FUN_003f3968_0x3f3968, 0x003F3968u, 0x0035D890u,
                                       ent, static_cast<uint32_t>(p), passPtr))
                        {
                            deferGuard.popNow();
                            bak.restoreArgsAndFrame(ctx);
                            return false;
                        }
                        const uint32_t pVt = rd32(rdram, passPtr + 0x5Cu);
                        if (!callEntityVirtual(rdram, ctx, runtime, rd32(rdram, pVt + 0x44u), 0x0035D8A4u, passPtr))
                        {
                            deferGuard.popNow();
                            bak.restoreArgsAndFrame(ctx);
                            return false;
                        }
                    }
                }
            }
        }

        if (s2_useLighting != 0)
        {
            if (!callWrapped(rdram, ctx, runtime, FUN_0023e470_0x23e470, 0x0023E470u, 0x0035D8C8u, 0x20u))
            {
                deferGuard.popNow();
                bak.restoreArgsAndFrame(ctx);
                return false;
            }
        }
        if (!callEntityVirtual(rdram, ctx, runtime, rd32(rdram, vt + 0x44u), 0x0035D8D8u, ent))
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }
        if (s2_useLighting != 0)
        {
            if (!callWrapped(rdram, ctx, runtime, FUN_0023e470_0x23e470, 0x0023E470u, 0x0035D8E8u, 0x20u) ||
                !callWrapped(rdram, ctx, runtime, g_hooks[kH_RenderWeaponAlphaList].outer, 0x0036EED0u, 0x0035D8F0u, 0x20u) ||
                !callWrapped(rdram, ctx, runtime, FUN_0023e498_0x23e498, 0x0023E498u, 0x0035D8F8u, 0x20u))
            {
                deferGuard.popNow();
                bak.restoreArgsAndFrame(ctx);
                return false;
            }
        }
        if (!callEntityVirtual(rdram, ctx, runtime, rd32(rdram, vt + 0x50u), 0x0035D90Cu, ent, s4_effects, true))
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }
        bak.restoreSavedOnly(ctx);
        return true;
    }

    bool renderEntityWithAlphaImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ent = GPR_U32(ctx, 4);
        const uint32_t a1Flag = GPR_U32(ctx, 5);
        return renderOneEntitySubWithAlpha(rdram, ctx, runtime, ent, a1Flag);
    }

    // 0x0035DA40(a0, a1 alphaFlag)
    bool setAlphaAndRenderStateImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (GPR_U32(ctx, 5) == 0u)
            return true;
        CheckpointDeferGuard deferGuard;
        CallArgsBackup bak(ctx);
        uint32_t ign = 0u;
        if (!callU32_0(rdram, ctx, runtime, FUN_001a6fb0_0x1a6fb0, 0x001A6FB0u, 0x0035DA54u, ign))
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }
        uint32_t r265 = 0u;
        if (!callU32_0(rdram, ctx, runtime, FUN_00265880_0x265880, 0x00265880u, 0x0035DA5Cu, r265))
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }
        const uint32_t r8 = rd32(rdram, r265 + 8u);
        if (!callU32_2(rdram, ctx, runtime, FUN_001cedf8_0x1cedf8, 0x001CEDF8u, 0x0035DA68u, r8, 0xFFu, ign))
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }
        bak.restoreSavedOnly(ctx);
        return true;
    }

    // 0x0036D480(ent a0, val a1)
    bool insertEntityIntoListImpl(uint8_t *rdram, R5900Context *ctx)
    {
        const uint32_t ent = GPR_U32(ctx, 4);
        const uint32_t val = GPR_U32(ctx, 5);
        if ((rd64(rdram, ent + 0x48u) & 0xEu) == 2ULL)
            return true;
        uint32_t cur = rd32(rdram, ent + 0x44u);
        while (cur != 0u)
        {
            if (rd32(rdram, cur + 4u) == val)
                return true;
            cur = rd32(rdram, cur + 0u);
        }
        const uint32_t freeNode = rd32(rdram, 0x004CD5B8u);
        if (freeNode != 0u)
        {
            wr32(rdram, 0x004CD5B8u, rd32(rdram, freeNode + 0u));
            wr32(rdram, freeNode + 0u, rd32(rdram, ent + 0x44u));
            wr32(rdram, ent + 0x44u, freeNode);
            wr32(rdram, freeNode + 4u, val);
        }
        return true;
    }

    // 0x0036DB80(rwObj a0) -> rwObj
    bool renderClumpDistImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        CheckpointDeferGuard deferGuard;
        CallArgsBackup bak(ctx);
        const uint32_t rwObj = GPR_U32(ctx, 4);
        const auto fnDA30 = resolveOuterOrRaw(0x0036DA30u, g_outer36DA30, FUN_0036da30_0x36da30, runtime);
        uint32_t mi = 0u;
        if (!callU32_1(rdram, ctx, runtime, fnDA30, 0x0036DA30u, 0x0036DB94u, rwObj, mi))
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }
        const float dist = std::sqrt(rdF32(rdram, 0x006F415Cu));
        uint32_t atomic = 0u;
        if (!invokeAtomicFromDistance(rdram, ctx, runtime, mi, dist, GPR_U32(ctx, 16), 0x0036DBB0u, atomic))
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }
        if (atomic != 0u)
        {
            const uint32_t frmA = rd32(rdram, atomic + 0x14u);
            const uint32_t frmObj = rd32(rdram, rwObj + 0x14u);
            if (frmA != frmObj)
            {
                uint32_t ign = 0u;
                if (!callU32_3(rdram, ctx, runtime, FUN_0039e160_0x39e160, 0x0039E160u, 0x0036DBD0u,
                               rwObj, frmA, 4u, ign))
                {
                    deferGuard.popNow();
                    bak.restoreArgsAndFrame(ctx);
                    return false;
                }
            }
            const auto fn39EC18 = resolveOuterOrRaw(0x0039EC18u, g_outer39EC18, FUN_0039ec18_0x39ec18, runtime);
            if (!callVoid1(rdram, ctx, runtime, fn39EC18, 0x0039EC18u, 0x0036DBD8u, rwObj))
            {
                deferGuard.popNow();
                bak.restoreArgsAndFrame(ctx);
                return false;
            }
        }
        bak.restoreSavedOnly(ctx);
        SET_GPR_S32(ctx, 2, static_cast<int32_t>(rwObj));
        return true;
    }

    // 0x0036DBF8(atomic a0, alpha a1)
    bool renderAtomicAlphaCbImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        CheckpointDeferGuard deferGuard;
        CallArgsBackup bak(ctx);
        const uint32_t atomic = GPR_U32(ctx, 4);
        const uint32_t alpha = GPR_U32(ctx, 5);
        const uint32_t geom = rd32(rdram, atomic + 0x14u);
        const uint8_t origFlags = rd8(rdram, geom + 2u);
        wr8(rdram, geom + 2u, static_cast<uint8_t>(origFlags | 0x40u));

        uint32_t ign = 0u;
        if (!callU32_3(rdram, ctx, runtime, FUN_00120c40_0x120c40, 0x00120C40u, 0x0036DC3Cu,
                       geom, 0x0036DB70u, alpha, ign))
        {
            wr8(rdram, geom + 2u, origFlags);
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }
        const auto fn39EC18 = resolveOuterOrRaw(0x0039EC18u, g_outer39EC18, FUN_0039ec18_0x39ec18, runtime);
        if (!callVoid1(rdram, ctx, runtime, fn39EC18, 0x0039EC18u, 0x0036DC44u, atomic))
        {
            wr8(rdram, geom + 2u, origFlags);
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }
        if (!callU32_3(rdram, ctx, runtime, FUN_00120c40_0x120c40, 0x00120C40u, 0x0036DC54u,
                       geom, 0x0036DB70u, 0xFFu, ign))
        {
            wr8(rdram, geom + 2u, origFlags);
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }
        wr8(rdram, geom + 2u, origFlags);
        bak.restoreSavedOnly(ctx);
        return true;
    }

    // 0x0036EB70(node a0) -> node
    bool renderWeaponPedAlphaCbImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        CheckpointDeferGuard deferGuard;
        CallArgsBackup bak(ctx);
        const uint32_t node = GPR_U32(ctx, 4);
        const uint32_t itemPtr = rd32(rdram, node + 0x18u);
        const uint32_t rwObj = rd32(rdram, itemPtr + 4u);

        SET_GPR_S32(ctx, 4, static_cast<int32_t>(rwObj));
        if (!callWrapped(rdram, ctx, runtime, g_hooks[kH_GetCamDistSq].outer, 0x0036F0C0u, 0x0036EB90u, 0x20u))
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }
        const float distSq = ctx->f[0];
        if (ps2Lt(distSq, rdF32(rdram, 0x004CD5CCu)))
        {
            const auto gaFn = resolveOuterOrRaw(0x0036ED68u, g_outer36ED68, FUN_0036ed68_0x36ed68, runtime);
            uint32_t alpha = 0u;
            if (!callU32_1(rdram, ctx, runtime, gaFn, 0x0036ED68u, 0x0036EBB0u, itemPtr, alpha))
            {
                deferGuard.popNow();
                bak.restoreArgsAndFrame(ctx);
                return false;
            }
            if (!callU32_2(rdram, ctx, runtime, g_hooks[kH_RenderAtomicAlphaCb].outer,
                           0x0036DBF8u, 0x0036EBBCu, node, alpha, alpha))
            {
                deferGuard.popNow();
                bak.restoreArgsAndFrame(ctx);
                return false;
            }
        }
        bak.restoreSavedOnly(ctx);
        SET_GPR_S32(ctx, 2, static_cast<int32_t>(node));
        return true;
    }

    // 0x0036ED60(obj a0, alpha a1)
    bool setAtomicAlphaImpl(uint8_t *rdram, R5900Context *ctx)
    {
        wr32(rdram, GPR_U32(ctx, 4) + 0x14u, GPR_U32(ctx, 5));
        return true;
    }

    // 0x0036ED70()
    bool clearVehicleAlphaListImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        CheckpointDeferGuard deferGuard;
        CallArgsBackup bak(ctx);
        const bool ok = callVoid1(rdram, ctx, runtime, FUN_0047c140_0x47c140, 0x0047C140u, 0x0036ED84u, 0x006F4030u);
        bak.restoreSavedOnly(ctx);
        return ok;
    }

    // 0x0036ED90()
    bool clearWeaponAlphaListImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        CheckpointDeferGuard deferGuard;
        CallArgsBackup bak(ctx);
        const bool ok = callVoid1(rdram, ctx, runtime, FUN_0047c1f8_0x47c1f8, 0x0047C1F8u, 0x0036EDA4u, 0x006F4078u);
        bak.restoreSavedOnly(ctx);
        return ok;
    }

    // 0x0036EDB0()
    bool clearEntityAlphaListsImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        CheckpointDeferGuard deferGuard;
        CallArgsBackup bak(ctx);
        if (!callVoid1(rdram, ctx, runtime, FUN_0047c140_0x47c140, 0x0047C140u, 0x0036EDC4u, 0x006F40D0u) ||
            !callVoid1(rdram, ctx, runtime, FUN_0047c140_0x47c140, 0x0047C140u, 0x0036EDD0u, 0x006F4118u))
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }
        bak.restoreSavedOnly(ctx);
        return true;
    }

    // 0x0036EE70(ent a0, dist f12) -> int
    bool insertEntitySortedFadeImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        CheckpointDeferGuard deferGuard;
        CallArgsBackup bak(ctx);
        const uint32_t ent = GPR_U32(ctx, 4);
        const float dist = ctx->f[12];
        const uint32_t spItem = (bak.sp - 0x20u) & ~0xFu;
        wr32(rdram, spItem + 0u, ent);
        wrF32(rdram, spItem + 4u, dist);
        SET_GPR_S32(ctx, 29, static_cast<int32_t>(spItem));

        uint32_t nodePtr = 0u;
        const bool hiSort = (rd64(rdram, ent + 0x48u) & (1ULL << 45)) != 0ULL;
        if (hiSort)
        {
            if (!callU32_2(rdram, ctx, runtime, FUN_0047c2f0_0x47c2f0, 0x0047C2F0u, 0x0036EEA4u,
                           0x006F4118u, spItem, nodePtr))
            {
                deferGuard.popNow();
                bak.restoreArgsAndFrame(ctx);
                return false;
            }
            if (nodePtr != 0u)
            {
                bak.restoreSavedOnly(ctx);
                SET_GPR_S32(ctx, 2, 1);
                return true;
            }
        }
        if (!callU32_2(rdram, ctx, runtime, FUN_0047c2f0_0x47c2f0, 0x0047C2F0u, 0x0036EEBCu,
                       0x006F40D0u, spItem, nodePtr))
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }
        bak.restoreSavedOnly(ctx);
        SET_GPR_S32(ctx, 2, nodePtr != 0u ? 1 : 0);
        return true;
    }

    // 0x0036EED0()
    bool renderWeaponAlphaListImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        CheckpointDeferGuard deferGuard;
        CallArgsBackup bak(ctx);
        const uint32_t head = 0x006F4030u;
        uint32_t cur = rd32(rdram, head + 0x18u);
        const auto fn39EC18 = resolveOuterOrRaw(0x0039EC18u, g_outer39EC18, FUN_0039ec18_0x39ec18, runtime);
        while (cur != head)
        {
            const uint32_t obj = rd32(rdram, cur + 0u);
            cur = rd32(rdram, cur + 8u);
            if (!callVoid1(rdram, ctx, runtime, fn39EC18, 0x0039EC18u, 0x0036EF00u, obj))
            {
                deferGuard.popNow();
                bak.restoreArgsAndFrame(ctx);
                return false;
            }
        }
        bak.restoreSavedOnly(ctx);
        return true;
    }

    // 0x0036EF38()
    bool renderBoatAlphaListImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        CheckpointDeferGuard deferGuard;
        CallArgsBackup bak(ctx);
        const uint32_t head = 0x006F4078u;
        uint32_t cur = rd32(rdram, head + 0x20u);
        const auto fn39EC18 = resolveOuterOrRaw(0x0039EC18u, g_outer39EC18, FUN_0039ec18_0x39ec18, runtime);
        uint32_t ign = 0u;
        while (cur != head)
        {
            const uint32_t atomic = rd32(rdram, cur + 0u);
            const uint32_t alpha = rd8(rdram, cur + 8u);
            cur = rd32(rdram, cur + 0xCu);
            if (!callU32_3(rdram, ctx, runtime, FUN_00120c40_0x120c40, 0x00120C40u, 0x0036EF78u,
                           rd32(rdram, atomic + 0x14u), 0x0036EF28u, alpha, ign) ||
                !callVoid1(rdram, ctx, runtime, fn39EC18, 0x0039EC18u, 0x0036EF80u, atomic))
            {
                deferGuard.popNow();
                bak.restoreArgsAndFrame(ctx);
                return false;
            }
        }
        bak.restoreSavedOnly(ctx);
        return true;
    }

    // 0x0036EFA8(list a0)
    bool renderSortedFadeListImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        CheckpointDeferGuard deferGuard;
        CallArgsBackup bak(ctx);
        const uint32_t list = GPR_U32(ctx, 4);
        uint32_t cur = rd32(rdram, list + 0x18u);
        uint32_t ign = 0u;
        while (cur != list)
        {
            const uint32_t ent = rd32(rdram, cur + 0u);
            cur = rd32(rdram, cur + 8u);
            if (rd32(rdram, ent + 0x50u) == 0u)
                continue;
            const uint32_t mi = getModelInfo(rdram, ent);
            const uint8_t mt = rd8(rdram, mi + 0x10u);
            const bool needZ = (mt == 1u) && ((rd16(rdram, mi + 0x3Au) & 0x400u) != 0u);
            if (needZ)
            {
                if (!callU32_2(rdram, ctx, runtime, FUN_00175198_0x175198, 0x00175198u, 0x0036F03Cu, 6u, 0u, ign))
                {
                    deferGuard.popNow();
                    bak.restoreArgsAndFrame(ctx);
                    return false;
                }
            }
            SET_GPR_S32(ctx, 4, static_cast<int32_t>(ent));
            SET_GPR_S32(ctx, 5, 1);
            if (!callWrapped(rdram, ctx, runtime, g_hooks[kH_RenderEntityWithAlpha].outer,
                             0x0035D610u, 0x0036F048u, 0x20u))
            {
                deferGuard.popNow();
                bak.restoreArgsAndFrame(ctx);
                return false;
            }
            if (needZ)
            {
                if (!callU32_2(rdram, ctx, runtime, FUN_00175198_0x175198, 0x00175198u, 0x0036F06Cu, 6u, 1u, ign))
                {
                    deferGuard.popNow();
                    bak.restoreArgsAndFrame(ctx);
                    return false;
                }
            }
        }
        bak.restoreSavedOnly(ctx);
        return true;
    }

    // 0x0036F0A0()
    bool renderFadingInEntitiesImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        CheckpointDeferGuard deferGuard;
        CallArgsBackup bak(ctx);
        SET_GPR_S32(ctx, 4, static_cast<int32_t>(0x006F40D0u));
        bool ok = false;
        if (hookActive(kH_RenderSortedFadeList) && (!verifyEnabled() || t_verifyDepth == 0) &&
            g_hooks[kH_RenderSortedFadeList].outer == g_hooks[kH_RenderSortedFadeList].hook)
        {
            ok = renderSortedFadeListImpl(rdram, ctx, runtime);
        }
        else
        {
            ok = callWrapped(rdram, ctx, runtime, g_hooks[kH_RenderSortedFadeList].outer,
                             0x0036EFA8u, 0x0036F0B4u, 0x20u);
        }
        if (!ok)
        {
            deferGuard.popNow();
            bak.restoreArgsAndFrame(ctx);
            return false;
        }
        bak.restoreSavedOnly(ctx);
        return true;
    }

    // ---- Verification Harness & Generic Runners --------------------------------------------

    inline void logVerifyDiff(int hookId, const char *detail) noexcept
    {
        g_verifyStats.diffs.fetch_add(1, std::memory_order_relaxed);
        g_verifyStats.perHookDiffs[hookId].fetch_add(1, std::memory_order_relaxed);
        if (g_verifyStats.perHookPrinted[hookId].fetch_add(1, std::memory_order_relaxed) < 8u)
        {
            std::fprintf(stderr, "[renderer-verify] DIFF hook=%d (0x%08X): %s\n",
                         hookId, g_hooks[hookId].addr, detail);
        }
    }

    enum class RetCmp { Int, Float, Void };

    template <typename SnapFn, typename RestoreFn, typename CmpFn, typename NatFn>
    void verifyAndRun(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, int hookId,
                      SnapFn &&captureSnap, RestoreFn &&restoreSnap, CmpFn &&compareSnap,
                      NatFn &&runNat, RetCmp retCmp = RetCmp::Int) noexcept
    {
        CallArgsBackup inBak(ctx);
        auto preSnap = captureSnap();

        {
            VerifyDepthGuard dg;
            g_hooks[hookId].orig(rdram, ctx, runtime);
        }
        if (runtime->isStopRequested() || g_checkpointUnwind)
            return;

        const uint32_t origV0 = GPR_U32(ctx, 2);
        const uint32_t origF0Bits = std::bit_cast<uint32_t>(ctx->f[0]);
        auto origSnap = captureSnap();

        restoreSnap(preSnap);
        inBak.restoreArgsAndFrame(ctx);

        bool natOk = false;
        {
            VerifyDepthGuard dg;
            natOk = runNat();
        }
        if (!natOk || g_checkpointUnwind)
        {
            // Restore orig outcome cleanly on fallback
            restoreSnap(origSnap);
            inBak.restoreArgsAndFrame(ctx);
            SET_GPR_S32(ctx, 2, static_cast<int32_t>(origV0));
            ctx->f[0] = std::bit_cast<float>(origF0Bits);
            g_verifyStats.fallbacks.fetch_add(1, std::memory_order_relaxed);
            g_verifyStats.perHookFallbacks[hookId].fetch_add(1, std::memory_order_relaxed);
            finishNative(ctx);
            return;
        }

        const uint32_t natV0 = GPR_U32(ctx, 2);
        const uint32_t natF0Bits = std::bit_cast<uint32_t>(ctx->f[0]);
        auto natSnap = captureSnap();

        g_verifyStats.checked.fetch_add(1, std::memory_order_relaxed);
        g_verifyStats.perHookChecked[hookId].fetch_add(1, std::memory_order_relaxed);

        char diffBuf[256];
        diffBuf[0] = '\0';
        if (retCmp == RetCmp::Float)
        {
            if (origF0Bits != natF0Bits)
                std::snprintf(diffBuf, sizeof(diffBuf), "f0 orig=0x%08X nat=0x%08X", origF0Bits, natF0Bits);
        }
        else if (retCmp == RetCmp::Int)
        {
            if (origV0 != natV0)
                std::snprintf(diffBuf, sizeof(diffBuf), "v0 orig=0x%08X nat=0x%08X", origV0, natV0);
        }
        if (diffBuf[0] == '\0')
            compareSnap(origSnap, natSnap, diffBuf, sizeof(diffBuf));

        if (diffBuf[0] != '\0')
        {
            logVerifyDiff(hookId, diffBuf);
            // Keep orig state so verify doesn't diverge gameplay if a bug existed
            restoreSnap(origSnap);
            SET_GPR_S32(ctx, 2, static_cast<int32_t>(origV0));
            ctx->f[0] = std::bit_cast<float>(origF0Bits);
        }
        else if (retCmp == RetCmp::Void)
        {
            SET_GPR_S32(ctx, 2, static_cast<int32_t>(origV0));
            ctx->f[0] = std::bit_cast<float>(origF0Bits);
        }
        inBak.restoreSavedOnly(ctx);
        finishNative(ctx);
    }

    // ---- Hook Entrypoints ------------------------------------------------------------------

    struct EmptySnap {};
    inline EmptySnap capEmpty() noexcept { return {}; }
    inline void restEmpty(const EmptySnap &) noexcept {}
    inline void cmpEmpty(const EmptySnap &, const EmptySnap &, char *, size_t) noexcept {}

    PS2_GUEST_CC void hookPointInBox(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (bypassVerifyChild(rdram, ctx, runtime, kH_PointInBox)) return;
        if (verifyEnabled())
        {
            verifyAndRun(rdram, ctx, runtime, kH_PointInBox, capEmpty, restEmpty, cmpEmpty,
                         [&]() { return pointInBoxImpl(rdram, ctx); });
            return;
        }
        if (!pointInBoxImpl(rdram, ctx)) { g_hooks[kH_PointInBox].orig(rdram, ctx, runtime); return; }
        finishNative(ctx);
    }

    PS2_GUEST_CC void hookZoneBoxLookup(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (bypassVerifyChild(rdram, ctx, runtime, kH_ZoneBoxLookup)) return;
        if (verifyEnabled())
        {
            verifyAndRun(rdram, ctx, runtime, kH_ZoneBoxLookup, capEmpty, restEmpty, cmpEmpty,
                         [&]() { return zoneBoxLookupImpl(rdram, ctx); });
            return;
        }
        if (!zoneBoxLookupImpl(rdram, ctx)) { g_hooks[kH_ZoneBoxLookup].orig(rdram, ctx, runtime); return; }
        finishNative(ctx);
    }

    PS2_GUEST_CC void hookSectorBoxScan1(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (bypassVerifyChild(rdram, ctx, runtime, kH_SectorBoxScan1)) return;
        if (verifyEnabled())
        {
            verifyAndRun(rdram, ctx, runtime, kH_SectorBoxScan1, capEmpty, restEmpty, cmpEmpty,
                         [&]() { return sectorBoxScan1Impl(rdram, ctx); });
            return;
        }
        if (!sectorBoxScan1Impl(rdram, ctx)) { g_hooks[kH_SectorBoxScan1].orig(rdram, ctx, runtime); return; }
        finishNative(ctx);
    }

    PS2_GUEST_CC void hookSectorBoxScan2(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (bypassVerifyChild(rdram, ctx, runtime, kH_SectorBoxScan2)) return;
        if (verifyEnabled())
        {
            verifyAndRun(rdram, ctx, runtime, kH_SectorBoxScan2, capEmpty, restEmpty, cmpEmpty,
                         [&]() { return sectorBoxScan2Impl(rdram, ctx); });
            return;
        }
        if (!sectorBoxScan2Impl(rdram, ctx)) { g_hooks[kH_SectorBoxScan2].orig(rdram, ctx, runtime); return; }
        finishNative(ctx);
    }

    PS2_GUEST_CC void hookClipEdgeZ(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (bypassVerifyChild(rdram, ctx, runtime, kH_ClipEdgeZ)) return;
        if (verifyEnabled())
        {
            const uint32_t outPtr = GPR_U32(ctx, 4);
            struct VecSnap { uint32_t w[3]; };
            verifyAndRun(rdram, ctx, runtime, kH_ClipEdgeZ,
                         [&]() { VecSnap s{}; std::memcpy(s.w, rdram + (outPtr & kRamMask), 12); return s; },
                         [&](const VecSnap &s) { std::memcpy(rdram + (outPtr & kRamMask), s.w, 12); },
                         [](const VecSnap &a, const VecSnap &b, char *buf, size_t n) {
                             if (std::memcmp(a.w, b.w, 12) != 0)
                                 std::snprintf(buf, n, "vec3 diff (%08X,%08X,%08X vs %08X,%08X,%08X)",
                                               a.w[0], a.w[1], a.w[2], b.w[0], b.w[1], b.w[2]);
                         },
                         [&]() { SET_GPR_S32(ctx, 2, 0); return clipEdgeZImpl(rdram, ctx); },
                         RetCmp::Void);
            return;
        }
        if (!clipEdgeZImpl(rdram, ctx)) { g_hooks[kH_ClipEdgeZ].orig(rdram, ctx, runtime); return; }
        finishNative(ctx);
    }

    PS2_GUEST_CC void hookGetCamDistSq(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ensureOuterHooks(runtime);
        if (bypassVerifyChild(rdram, ctx, runtime, kH_GetCamDistSq)) return;
        if (verifyEnabled())
        {
            verifyAndRun(rdram, ctx, runtime, kH_GetCamDistSq, capEmpty, restEmpty, cmpEmpty,
                         [&]() { return getCamDistSqImpl(rdram, ctx, runtime); }, RetCmp::Float);
            return;
        }
        if (!getCamDistSqImpl(rdram, ctx, runtime)) { g_hooks[kH_GetCamDistSq].orig(rdram, ctx, runtime); return; }
        finishNative(ctx);
    }

    PS2_GUEST_CC void hookIsEntityInActiveInterior(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (bypassVerifyChild(rdram, ctx, runtime, kH_IsEntityInActiveInterior)) return;
        if (verifyEnabled())
        {
            verifyAndRun(rdram, ctx, runtime, kH_IsEntityInActiveInterior, capEmpty, restEmpty, cmpEmpty,
                         [&]() { return isEntityInActiveInteriorImpl(rdram, ctx); });
            return;
        }
        if (!isEntityInActiveInteriorImpl(rdram, ctx)) { g_hooks[kH_IsEntityInActiveInterior].orig(rdram, ctx, runtime); return; }
        finishNative(ctx);
    }

    PS2_GUEST_CC void hookCalcFrustumPlanes(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ensureOuterHooks(runtime);
        if (bypassVerifyChild(rdram, ctx, runtime, kH_CalcFrustumPlanes)) return;
        if (verifyEnabled())
        {
            const uint32_t cam = GPR_U32(ctx, 4);
            const uint32_t rwCam = rd32(rdram, cam + 0x7F4u);
            struct CamFrustSnap { uint8_t rwPlanes[0x40]; uint8_t camView[0x40]; };
            verifyAndRun(rdram, ctx, runtime, kH_CalcFrustumPlanes,
                         [&]() {
                             CamFrustSnap s{};
                             std::memcpy(s.rwPlanes, rdram + ((rwCam + 0x20u) & kRamMask), 0x40);
                             std::memcpy(s.camView, rdram + ((cam + 0xB10u) & kRamMask), 0x40);
                             return s;
                         },
                         [&](const CamFrustSnap &s) {
                             std::memcpy(rdram + ((rwCam + 0x20u) & kRamMask), s.rwPlanes, 0x40);
                             std::memcpy(rdram + ((cam + 0xB10u) & kRamMask), s.camView, 0x40);
                         },
                         [](const CamFrustSnap &a, const CamFrustSnap &b, char *buf, size_t n) {
                             if (std::memcmp(a.rwPlanes, b.rwPlanes, 0x40) != 0)
                                 std::snprintf(buf, n, "rwPlanes mismatch");
                             else if (std::memcmp(a.camView, b.camView, 0x40) != 0)
                                 std::snprintf(buf, n, "camView mismatch");
                         },
                         [&]() { SET_GPR_S32(ctx, 2, 0); return calcFrustumPlanesImpl(rdram, ctx, runtime); },
                         RetCmp::Void);
            return;
        }
        if (!calcFrustumPlanesImpl(rdram, ctx, runtime)) { g_hooks[kH_CalcFrustumPlanes].orig(rdram, ctx, runtime); return; }
        finishNative(ctx);
    }

    PS2_GUEST_CC void hookIsSphereVisible(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (bypassVerifyChild(rdram, ctx, runtime, kH_IsSphereVisible)) return;
        if (verifyEnabled())
        {
            verifyAndRun(rdram, ctx, runtime, kH_IsSphereVisible, capEmpty, restEmpty, cmpEmpty,
                         [&]() { return isSphereVisibleImpl(rdram, ctx); });
            return;
        }
        if (!isSphereVisibleImpl(rdram, ctx)) { g_hooks[kH_IsSphereVisible].orig(rdram, ctx, runtime); return; }
        finishNative(ctx);
    }

    PS2_GUEST_CC void hookRetZero241A80(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (bypassVerifyChild(rdram, ctx, runtime, kH_RetZero241A80)) return;
        if (verifyEnabled())
        {
            verifyAndRun(rdram, ctx, runtime, kH_RetZero241A80, capEmpty, restEmpty, cmpEmpty,
                         [&]() { return retZero241A80Impl(rdram, ctx); });
            return;
        }
        if (!retZero241A80Impl(rdram, ctx)) { g_hooks[kH_RetZero241A80].orig(rdram, ctx, runtime); return; }
        finishNative(ctx);
    }

    PS2_GUEST_CC void hookGetPedFadeAlphaMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (bypassVerifyChild(rdram, ctx, runtime, kH_GetPedFadeAlphaMode)) return;
        if (verifyEnabled())
        {
            verifyAndRun(rdram, ctx, runtime, kH_GetPedFadeAlphaMode, capEmpty, restEmpty, cmpEmpty,
                         [&]() { return getPedFadeAlphaModeImpl(rdram, ctx); });
            return;
        }
        if (!getPedFadeAlphaModeImpl(rdram, ctx)) { g_hooks[kH_GetPedFadeAlphaMode].orig(rdram, ctx, runtime); return; }
        finishNative(ctx);
    }

    PS2_GUEST_CC void hookTestEntitySphereGate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ensureOuterHooks(runtime);
        if (bypassVerifyChild(rdram, ctx, runtime, kH_TestEntitySphereGate)) return;
        if (verifyEnabled())
        {
            verifyAndRun(rdram, ctx, runtime, kH_TestEntitySphereGate, capEmpty, restEmpty, cmpEmpty,
                         [&]() { return testEntitySphereGateImpl(rdram, ctx, runtime); });
            return;
        }
        if (!testEntitySphereGateImpl(rdram, ctx, runtime)) { g_hooks[kH_TestEntitySphereGate].orig(rdram, ctx, runtime); return; }
        finishNative(ctx);
    }

    PS2_GUEST_CC void hookTestEntitySphereVisible(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ensureOuterHooks(runtime);
        if (bypassVerifyChild(rdram, ctx, runtime, kH_TestEntitySphereVisible)) return;
        if (verifyEnabled())
        {
            verifyAndRun(rdram, ctx, runtime, kH_TestEntitySphereVisible, capEmpty, restEmpty, cmpEmpty,
                         [&]() { return testEntitySphereVisibleImpl(rdram, ctx, runtime); });
            return;
        }
        if (!testEntitySphereVisibleImpl(rdram, ctx, runtime)) { g_hooks[kH_TestEntitySphereVisible].orig(rdram, ctx, runtime); return; }
        finishNative(ctx);
    }

    // ---- SetupEntityVisibility / SetupBigBuildingVisibility (with bounded snapshots) ------

    struct EntityVisSnap
    {
        uint32_t ent = 0;
        uint32_t mi = 0;
        uint32_t atomicFrm = 0;
        uint64_t entFlags = 0;
        uint32_t miLod0Bits = 0;
        uint8_t miAlpha = 0;
        uint16_t miFlags = 0;
        uint32_t fpVeh = 0;
        uint32_t g48F4F8 = 0;
        LinkListSnap listD0;
        LinkListSnap list118;
    };

    EntityVisSnap captureEntVisSnap(const uint8_t *rdram, uint32_t ent)
    {
        EntityVisSnap s{};
        s.ent = ent;
        s.mi = getModelInfo(rdram, ent);
        s.entFlags = rd64(rdram, ent + 0x48u);
        if (s.mi)
        {
            s.miLod0Bits = rd32(rdram, s.mi + 0x2Cu);
            s.miAlpha = rd8(rdram, s.mi + 0x39u);
            s.miFlags = rd16(rdram, s.mi + 0x3Au);
        }
        s.fpVeh = rd32(rdram, 0x004CD5A8u);
        s.g48F4F8 = rd32(rdram, 0x0048F4F8u);
        s.listD0.capture(rdram, 0x006F40D0u);
        s.list118.capture(rdram, 0x006F4118u);
        return s;
    }

    void restoreEntVisSnap(uint8_t *rdram, const EntityVisSnap &s) noexcept
    {
        wr64(rdram, s.ent + 0x48u, s.entFlags);
        if (s.mi)
        {
            wr32(rdram, s.mi + 0x2Cu, s.miLod0Bits);
            wr8(rdram, s.mi + 0x39u, s.miAlpha);
            wr16(rdram, s.mi + 0x3Au, s.miFlags);
        }
        wr32(rdram, 0x004CD5A8u, s.fpVeh);
        wr32(rdram, 0x0048F4F8u, s.g48F4F8);
        s.listD0.restore(rdram);
        s.list118.restore(rdram);
    }

    PS2_GUEST_CC void hookSetupEntityVisibility(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ensureOuterHooks(runtime);
        if (bypassVerifyChild(rdram, ctx, runtime, kH_SetupEntityVisibility)) return;
        const uint32_t ent = GPR_U32(ctx, 4);
        if (verifyEnabled())
        {
            // Only dual-exec when entity already has RwObject (no CreateRwObject heap alloc) and doesn't touch 241D28
            const uint32_t mi = getModelInfo(rdram, ent);
            const uint8_t mt = mi ? rd8(rdram, mi + 0x10u) : 0u;
            const uint64_t f = rd64(rdram, ent + 0x48u);
            const bool unsafeAlloc = (rd32(rdram, ent + 0x50u) == 0u) || (mt == 3u) ||
                                     ((f & 0xEu) == 8ULL && (f & (1ULL << 19)) != 0ULL) ||
                                     (rd32(rdram, 0x0048F4F8u) == 0u);
            if (unsafeAlloc)
            {
                g_verifyStats.fallbacks.fetch_add(1, std::memory_order_relaxed);
                g_verifyStats.perHookFallbacks[kH_SetupEntityVisibility].fetch_add(1, std::memory_order_relaxed);
                g_hooks[kH_SetupEntityVisibility].orig(rdram, ctx, runtime);
                return;
            }
            verifyAndRun(rdram, ctx, runtime, kH_SetupEntityVisibility,
                         [&]() { return captureEntVisSnap(rdram, ent); },
                         [&](const EntityVisSnap &s) { restoreEntVisSnap(rdram, s); },
                         [](const EntityVisSnap &a, const EntityVisSnap &b, char *buf, size_t n) {
                             if (a.entFlags != b.entFlags)
                                 std::snprintf(buf, n, "entFlags %016llX vs %016llX",
                                               (unsigned long long)a.entFlags, (unsigned long long)b.entFlags);
                             else if (a.miAlpha != b.miAlpha || a.miFlags != b.miFlags || a.miLod0Bits != b.miLod0Bits)
                                 std::snprintf(buf, n, "mi a=%u/%u f=%X/%X lod=%08X/%08X",
                                               a.miAlpha, b.miAlpha, a.miFlags, b.miFlags,
                                               a.miLod0Bits, b.miLod0Bits);
                             else if (a.fpVeh != b.fpVeh)
                                 std::snprintf(buf, n, "fpVeh %08X vs %08X", a.fpVeh, b.fpVeh);
                             else if (!a.listD0.equal(b.listD0) || !a.list118.equal(b.list118))
                                 std::snprintf(buf, n, "fade list diff");
                         },
                         [&]() { return setupEntityVisibilityImpl(rdram, ctx, runtime); });
            return;
        }
        if (!setupEntityVisibilityImpl(rdram, ctx, runtime))
        {
            g_hooks[kH_SetupEntityVisibility].orig(rdram, ctx, runtime);
            return;
        }
        finishNative(ctx);
    }

    PS2_GUEST_CC void hookSetupBigBuildingVis(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ensureOuterHooks(runtime);
        if (bypassVerifyChild(rdram, ctx, runtime, kH_SetupBigBuildingVis)) return;
        if (verifyEnabled())
        {
            if (rd32(rdram, 0x0048F4F8u) == 0u)
            {
                g_verifyStats.fallbacks.fetch_add(1, std::memory_order_relaxed);
                g_verifyStats.perHookFallbacks[kH_SetupBigBuildingVis].fetch_add(1, std::memory_order_relaxed);
                g_hooks[kH_SetupBigBuildingVis].orig(rdram, ctx, runtime);
                return;
            }
            verifyAndRun(rdram, ctx, runtime, kH_SetupBigBuildingVis, capEmpty, restEmpty, cmpEmpty,
                         [&]() { return setupBigBuildingVisibilityImpl(rdram, ctx, runtime); });
            return;
        }
        if (!setupBigBuildingVisibilityImpl(rdram, ctx, runtime))
        {
            g_hooks[kH_SetupBigBuildingVis].orig(rdram, ctx, runtime);
            return;
        }
        finishNative(ctx);
    }

    // ---- Sector Scan Snapshot & Entrypoints ------------------------------------------------

    struct SectorScanSnap
    {
        struct EntState
        {
            uint32_t ent;
            uint16_t scan;
            uint64_t flags;
            uint32_t mi;
            uint32_t miLod0Bits;
            uint8_t miAlpha;
            uint16_t miFlags;
        };
        std::vector<EntState> ents;
        uint32_t visCounts[5]; // 0x4CD598..0x4CD5A8 (and +0x10 = 0x4CD5AC)
        std::vector<uint32_t> visLists[4]; // newly appended slices
        LinkListSnap listD0;
        LinkListSnap list118;
    };

    SectorScanSnap captureSectorSnap(const uint8_t *rdram, uint32_t sector)
    {
        SectorScanSnap s{};
        for (int i = 0; i < 5; ++i)
            s.visCounts[i] = rd32(rdram, 0x004CD598u + static_cast<uint32_t>(i) * 4u);
        s.listD0.capture(rdram, 0x006F40D0u);
        s.list118.capture(rdram, 0x006F4118u);

        static constexpr uint32_t kListBases[4] = {0x00742B80u, 0x00744AC0u, 0x00746C80u, 0x00746A00u};
        for (int i = 0; i < 4; ++i)
        {
            const uint32_t n = std::min<uint32_t>(s.visCounts[i], 2048u);
            s.visLists[i].resize(n);
            if (n > 0)
                std::memcpy(s.visLists[i].data(), rdram + (kListBases[i] & kRamMask), n * 4u);
        }

        for (int listIdx = 0; listIdx < 12; ++listIdx)
        {
            uint32_t node = rd32(rdram, sector + 0x08u + static_cast<uint32_t>(listIdx) * 4u);
            int guard = 0;
            while (validRam(node) && guard++ < 4096)
            {
                const uint32_t ent = rd32(rdram, node);
                node = rd32(rdram, node + 8u);
                if (!validRam(ent))
                    continue;
                SectorScanSnap::EntState es{};
                es.ent = ent;
                es.scan = rd16(rdram, ent + 0x54u);
                es.flags = rd64(rdram, ent + 0x48u);
                es.mi = getModelInfo(rdram, ent);
                if (es.mi)
                {
                    es.miLod0Bits = rd32(rdram, es.mi + 0x2Cu);
                    es.miAlpha = rd8(rdram, es.mi + 0x39u);
                    es.miFlags = rd16(rdram, es.mi + 0x3Au);
                }
                s.ents.push_back(es);
            }
        }
        return s;
    }

    void restoreSectorSnap(uint8_t *rdram, const SectorScanSnap &s) noexcept
    {
        for (int i = 0; i < 5; ++i)
            wr32(rdram, 0x004CD598u + static_cast<uint32_t>(i) * 4u, s.visCounts[i]);
        static constexpr uint32_t kListBases[4] = {0x00742B80u, 0x00744AC0u, 0x00746C80u, 0x00746A00u};
        for (int i = 0; i < 4; ++i)
        {
            if (!s.visLists[i].empty())
                std::memcpy(rdram + (kListBases[i] & kRamMask), s.visLists[i].data(), s.visLists[i].size() * 4u);
        }
        for (const auto &es : s.ents)
        {
            wr16(rdram, es.ent + 0x54u, es.scan);
            wr64(rdram, es.ent + 0x48u, es.flags);
            if (es.mi)
            {
                wr32(rdram, es.mi + 0x2Cu, es.miLod0Bits);
                wr8(rdram, es.mi + 0x39u, es.miAlpha);
                wr16(rdram, es.mi + 0x3Au, es.miFlags);
            }
        }
        s.listD0.restore(rdram);
        s.list118.restore(rdram);
    }

    bool sectorHasUnsafeEntities(const uint8_t *rdram, uint32_t sector) noexcept
    {
        if (rd32(rdram, 0x0048F4F8u) == 0u || rd32(rdram, 0x0048F050u) == 0u)
            return true;
        const uint16_t curScan = rd16(rdram, 0x00486468u);
        for (int listIdx = 0; listIdx < 12; ++listIdx)
        {
            uint32_t node = rd32(rdram, sector + 0x08u + static_cast<uint32_t>(listIdx) * 4u);
            int guard = 0;
            while (validRam(node) && guard++ < 4096)
            {
                const uint32_t ent = rd32(rdram, node);
                node = rd32(rdram, node + 8u);
                if (!validRam(ent) || rd16(rdram, ent + 0x54u) == curScan)
                    continue;
                if (rd32(rdram, ent + 0x50u) == 0u)
                    return true;
                const uint32_t mi = getModelInfo(rdram, ent);
                if (!mi || rd8(rdram, mi + 0x10u) == 3u)
                    return true;
                if (rd16(rdram, mi + 0x3Au) & 0x20u) // could request model (vis==3)
                    return true;
            }
        }
        return false;
    }

    template <int HookIdx, bool (*ImplFn)(uint8_t *, R5900Context *, PS2Runtime *)>
    PS2_GUEST_CC void hookSectorGeneric(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ensureOuterHooks(runtime);
        if (bypassVerifyChild(rdram, ctx, runtime, HookIdx)) return;
        const uint32_t sector = GPR_U32(ctx, 4);
        if (verifyEnabled())
        {
            if (sectorHasUnsafeEntities(rdram, sector))
            {
                g_verifyStats.fallbacks.fetch_add(1, std::memory_order_relaxed);
                g_verifyStats.perHookFallbacks[HookIdx].fetch_add(1, std::memory_order_relaxed);
                g_hooks[HookIdx].orig(rdram, ctx, runtime);
                return;
            }
            verifyAndRun(rdram, ctx, runtime, HookIdx,
                         [&]() { return captureSectorSnap(rdram, sector); },
                         [&](const SectorScanSnap &s) { restoreSectorSnap(rdram, s); },
                         [](const SectorScanSnap &a, const SectorScanSnap &b, char *buf, size_t n) {
                             for (int i = 0; i < 4; ++i)
                             {
                                 if (a.visCounts[i] != b.visCounts[i] || a.visLists[i] != b.visLists[i])
                                 {
                                     std::snprintf(buf, n, "list[%d] cnt %u vs %u", i, a.visCounts[i], b.visCounts[i]);
                                     return;
                                 }
                             }
                             if (a.visCounts[4] != b.visCounts[4])
                             {
                                 std::snprintf(buf, n, "fpVeh %08X vs %08X", a.visCounts[4], b.visCounts[4]);
                                 return;
                             }
                             for (size_t i = 0; i < a.ents.size(); ++i)
                             {
                                 if (a.ents[i].scan != b.ents[i].scan || a.ents[i].flags != b.ents[i].flags)
                                 {
                                     std::snprintf(buf, n, "ent %08X flags/scan diff", a.ents[i].ent);
                                     return;
                                 }
                             }
                             if (!a.listD0.equal(b.listD0) || !a.list118.equal(b.list118))
                                 std::snprintf(buf, n, "fade list diff");
                         },
                         [&]() { SET_GPR_S32(ctx, 2, 0); return ImplFn(rdram, ctx, runtime); },
                         RetCmp::Void);
            return;
        }
        if (!ImplFn(rdram, ctx, runtime)) { g_hooks[HookIdx].orig(rdram, ctx, runtime); return; }
        finishNative(ctx);
    }

    PS2_GUEST_CC void hookScanSectorList(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        hookSectorGeneric<kH_ScanSectorList, scanSectorListImpl>(rdram, ctx, runtime);
    }

    PS2_GUEST_CC void hookScanBigBuildingList(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        hookSectorGeneric<kH_ScanBigBuildingList, scanBigBuildingListImpl>(rdram, ctx, runtime);
    }

    PS2_GUEST_CC void hookScanSectorRequestModels(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        hookSectorGeneric<kH_ScanSectorRequestModels, scanSectorRequestModelsImpl>(rdram, ctx, runtime);
    }

    PS2_GUEST_CC void hookScanWorld(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ensureOuterHooks(runtime);
        if (bypassVerifyChild(rdram, ctx, runtime, kH_ScanWorld)) return;
        // ScanWorld clears 0x6F40D0/0x6F4118, increments scan stamp 0x486468, scans entire world (potentially requesting models/allocating RwObjects)
        // So ScanWorld executes natively (while all pure/entity/box checks inside verify when enabled? Notice verify runs at caller level!)
        if (!scanWorldImpl(rdram, ctx, runtime)) { g_hooks[kH_ScanWorld].orig(rdram, ctx, runtime); return; }
        finishNative(ctx);
    }

    PS2_GUEST_CC void hookScanWorldWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ensureOuterHooks(runtime);
        if (bypassVerifyChild(rdram, ctx, runtime, kH_ScanWorldWrapper)) return;
        if (!scanWorldWrapperImpl(rdram, ctx, runtime)) { g_hooks[kH_ScanWorldWrapper].orig(rdram, ctx, runtime); return; }
        finishNative(ctx);
    }

    // ---- Group C Entrypoints ---------------------------------------------------------------

    PS2_GUEST_CC void hookResetVisibleLists(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ensureOuterHooks(runtime);
        if (bypassVerifyChild(rdram, ctx, runtime, kH_ResetVisibleLists)) return;
        if (verifyEnabled())
        {
            struct ResetSnap { uint32_t c[4]; LinkListSnap l; };
            verifyAndRun(rdram, ctx, runtime, kH_ResetVisibleLists,
                         [&]() {
                             ResetSnap s{};
                             for (int i = 0; i < 4; ++i) s.c[i] = rd32(rdram, 0x004CD598u + i * 4u);
                             s.l.capture(rdram, 0x006F3C10u);
                             return s;
                         },
                         [&](const ResetSnap &s) {
                             for (int i = 0; i < 4; ++i) wr32(rdram, 0x004CD598u + i * 4u, s.c[i]);
                             s.l.restore(rdram);
                         },
                         [](const ResetSnap &a, const ResetSnap &b, char *buf, size_t n) {
                             if (std::memcmp(a.c, b.c, 16) != 0 || !a.l.equal(b.l))
                                 std::snprintf(buf, n, "reset visible lists diff");
                         },
                         [&]() { SET_GPR_S32(ctx, 2, 0); return resetVisibleListsImpl(rdram, ctx, runtime); },
                         RetCmp::Void);
            return;
        }
        if (!resetVisibleListsImpl(rdram, ctx, runtime)) { g_hooks[kH_ResetVisibleLists].orig(rdram, ctx, runtime); return; }
        finishNative(ctx);
    }

    PS2_GUEST_CC void hookInsertEntityIntoList(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (bypassVerifyChild(rdram, ctx, runtime, kH_InsertEntityIntoList)) return;
        if (verifyEnabled())
        {
            const uint32_t ent = GPR_U32(ctx, 4);
            verifyAndRun(rdram, ctx, runtime, kH_InsertEntityIntoList,
                         [&]() { EntityLinkSnap s; s.capture(rdram, ent); return s; },
                         [&](const EntityLinkSnap &s) { s.restore(rdram); },
                         [](const EntityLinkSnap &a, const EntityLinkSnap &b, char *buf, size_t n) {
                             if (!a.equal(b))
                                 std::snprintf(buf, n, "ent linklist diff head %08X vs %08X", a.entHead44, b.entHead44);
                         },
                         [&]() { SET_GPR_S32(ctx, 2, 0); return insertEntityIntoListImpl(rdram, ctx); },
                         RetCmp::Void);
            return;
        }
        if (!insertEntityIntoListImpl(rdram, ctx)) { g_hooks[kH_InsertEntityIntoList].orig(rdram, ctx, runtime); return; }
        finishNative(ctx);
    }

    PS2_GUEST_CC void hookSetAtomicAlpha(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (bypassVerifyChild(rdram, ctx, runtime, kH_SetAtomicAlpha)) return;
        if (verifyEnabled())
        {
            const uint32_t obj = GPR_U32(ctx, 4);
            struct ValSnap { uint32_t v; };
            verifyAndRun(rdram, ctx, runtime, kH_SetAtomicAlpha,
                         [&]() { return ValSnap{rd32(rdram, obj + 0x14u)}; },
                         [&](const ValSnap &s) { wr32(rdram, obj + 0x14u, s.v); },
                         [](const ValSnap &a, const ValSnap &b, char *buf, size_t n) {
                             if (a.v != b.v) std::snprintf(buf, n, "alpha %08X vs %08X", a.v, b.v);
                         },
                         [&]() { SET_GPR_S32(ctx, 2, 0); return setAtomicAlphaImpl(rdram, ctx); },
                         RetCmp::Void);
            return;
        }
        if (!setAtomicAlphaImpl(rdram, ctx)) { g_hooks[kH_SetAtomicAlpha].orig(rdram, ctx, runtime); return; }
        finishNative(ctx);
    }

    PS2_GUEST_CC void hookClearVehicleAlphaList(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (bypassVerifyChild(rdram, ctx, runtime, kH_ClearVehicleAlphaList)) return;
        if (verifyEnabled())
        {
            verifyAndRun(rdram, ctx, runtime, kH_ClearVehicleAlphaList,
                         [&]() { LinkListSnap s; s.capture(rdram, 0x006F4030u); return s; },
                         [&](const LinkListSnap &s) { s.restore(rdram); },
                         [](const LinkListSnap &a, const LinkListSnap &b, char *buf, size_t n) {
                             if (!a.equal(b)) std::snprintf(buf, n, "clearVehAlpha diff");
                         },
                         [&]() { SET_GPR_S32(ctx, 2, 0); return clearVehicleAlphaListImpl(rdram, ctx, runtime); },
                         RetCmp::Void);
            return;
        }
        if (!clearVehicleAlphaListImpl(rdram, ctx, runtime)) { g_hooks[kH_ClearVehicleAlphaList].orig(rdram, ctx, runtime); return; }
        finishNative(ctx);
    }

    PS2_GUEST_CC void hookClearWeaponAlphaList(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (bypassVerifyChild(rdram, ctx, runtime, kH_ClearWeaponAlphaList)) return;
        if (verifyEnabled())
        {
            verifyAndRun(rdram, ctx, runtime, kH_ClearWeaponAlphaList,
                         [&]() { LinkListSnap s; s.capture(rdram, 0x006F4078u); return s; },
                         [&](const LinkListSnap &s) { s.restore(rdram); },
                         [](const LinkListSnap &a, const LinkListSnap &b, char *buf, size_t n) {
                             if (!a.equal(b)) std::snprintf(buf, n, "clearWpnAlpha diff");
                         },
                         [&]() { SET_GPR_S32(ctx, 2, 0); return clearWeaponAlphaListImpl(rdram, ctx, runtime); },
                         RetCmp::Void);
            return;
        }
        if (!clearWeaponAlphaListImpl(rdram, ctx, runtime)) { g_hooks[kH_ClearWeaponAlphaList].orig(rdram, ctx, runtime); return; }
        finishNative(ctx);
    }

    PS2_GUEST_CC void hookClearEntityAlphaLists(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (bypassVerifyChild(rdram, ctx, runtime, kH_ClearEntityAlphaLists)) return;
        if (verifyEnabled())
        {
            struct DualListSnap { LinkListSnap a, b; };
            verifyAndRun(rdram, ctx, runtime, kH_ClearEntityAlphaLists,
                         [&]() { DualListSnap s; s.a.capture(rdram, 0x006F40D0u); s.b.capture(rdram, 0x006F4118u); return s; },
                         [&](const DualListSnap &s) { s.a.restore(rdram); s.b.restore(rdram); },
                         [](const DualListSnap &x, const DualListSnap &y, char *buf, size_t n) {
                             if (!x.a.equal(y.a) || !x.b.equal(y.b)) std::snprintf(buf, n, "clearEntAlpha diff");
                         },
                         [&]() { SET_GPR_S32(ctx, 2, 0); return clearEntityAlphaListsImpl(rdram, ctx, runtime); },
                         RetCmp::Void);
            return;
        }
        if (!clearEntityAlphaListsImpl(rdram, ctx, runtime)) { g_hooks[kH_ClearEntityAlphaLists].orig(rdram, ctx, runtime); return; }
        finishNative(ctx);
    }

    PS2_GUEST_CC void hookInsertEntitySortedFade(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (bypassVerifyChild(rdram, ctx, runtime, kH_InsertEntitySortedFade)) return;
        if (verifyEnabled())
        {
            struct DualListSnap { LinkListSnap a, b; };
            verifyAndRun(rdram, ctx, runtime, kH_InsertEntitySortedFade,
                         [&]() { DualListSnap s; s.a.capture(rdram, 0x006F40D0u); s.b.capture(rdram, 0x006F4118u); return s; },
                         [&](const DualListSnap &s) { s.a.restore(rdram); s.b.restore(rdram); },
                         [](const DualListSnap &x, const DualListSnap &y, char *buf, size_t n) {
                             if (!x.a.equal(y.a) || !x.b.equal(y.b)) std::snprintf(buf, n, "insertSortedFade diff");
                         },
                         [&]() { return insertEntitySortedFadeImpl(rdram, ctx, runtime); });
            return;
        }
        if (!insertEntitySortedFadeImpl(rdram, ctx, runtime)) { g_hooks[kH_InsertEntitySortedFade].orig(rdram, ctx, runtime); return; }
        finishNative(ctx);
    }

    template <int HookIdx, bool (*ImplFn)(uint8_t *, R5900Context *, PS2Runtime *)>
    PS2_GUEST_CC void hookRenderDispatch(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ensureOuterHooks(runtime);
        if (bypassVerifyChild(rdram, ctx, runtime, HookIdx)) return;
        if (!ImplFn(rdram, ctx, runtime))
        {
            g_hooks[HookIdx].orig(rdram, ctx, runtime);
            return;
        }
        finishNative(ctx);
    }

    PS2_GUEST_CC void hookRenderBodies(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        hookRenderDispatch<kH_RenderBodies, renderBodiesImpl>(rdram, ctx, runtime);
    }
    PS2_GUEST_CC void hookRenderUnderwaterAlpha(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        hookRenderDispatch<kH_RenderUnderwaterAlpha, renderUnderwaterAlphaImpl>(rdram, ctx, runtime);
    }
    PS2_GUEST_CC void hookRenderRoads(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        hookRenderDispatch<kH_RenderRoads, renderRoadsImpl>(rdram, ctx, runtime);
    }
    PS2_GUEST_CC void hookRenderEverythingBarRoads(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        hookRenderDispatch<kH_RenderEverythingBarRoads, renderEverythingBarRoadsImpl>(rdram, ctx, runtime);
    }
    PS2_GUEST_CC void hookRenderOpaqueVehicleProps(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        hookRenderDispatch<kH_RenderOpaqueVehicleProps, renderOpaqueVehiclePropsImpl>(rdram, ctx, runtime);
    }
    PS2_GUEST_CC void hookRenderAlphaAndSorted(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        hookRenderDispatch<kH_RenderAlphaAndSorted, renderAlphaAndSortedImpl>(rdram, ctx, runtime);
    }
    PS2_GUEST_CC void hookRenderFirstPersonVehicle(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        hookRenderDispatch<kH_RenderFirstPersonVehicle, renderFirstPersonVehicleImpl>(rdram, ctx, runtime);
    }
    PS2_GUEST_CC void hookRenderOneEntity(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        hookRenderDispatch<kH_RenderOneEntity, renderOneEntityImpl>(rdram, ctx, runtime);
    }
    PS2_GUEST_CC void hookRenderEntityWithAlpha(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        hookRenderDispatch<kH_RenderEntityWithAlpha, renderEntityWithAlphaImpl>(rdram, ctx, runtime);
    }
    PS2_GUEST_CC void hookSetAlphaAndRenderState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        hookRenderDispatch<kH_SetAlphaAndRenderState, setAlphaAndRenderStateImpl>(rdram, ctx, runtime);
    }
    PS2_GUEST_CC void hookRenderClumpDist(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        hookRenderDispatch<kH_RenderClumpDist, renderClumpDistImpl>(rdram, ctx, runtime);
    }
    PS2_GUEST_CC void hookRenderAtomicAlphaCb(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        hookRenderDispatch<kH_RenderAtomicAlphaCb, renderAtomicAlphaCbImpl>(rdram, ctx, runtime);
    }
    PS2_GUEST_CC void hookRenderWeaponPedAlphaCb(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        hookRenderDispatch<kH_RenderWeaponPedAlphaCb, renderWeaponPedAlphaCbImpl>(rdram, ctx, runtime);
    }
    PS2_GUEST_CC void hookRenderWeaponAlphaList(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        hookRenderDispatch<kH_RenderWeaponAlphaList, renderWeaponAlphaListImpl>(rdram, ctx, runtime);
    }
    PS2_GUEST_CC void hookRenderBoatAlphaList(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        hookRenderDispatch<kH_RenderBoatAlphaList, renderBoatAlphaListImpl>(rdram, ctx, runtime);
    }
    PS2_GUEST_CC void hookRenderSortedFadeList(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        hookRenderDispatch<kH_RenderSortedFadeList, renderSortedFadeListImpl>(rdram, ctx, runtime);
    }
    PS2_GUEST_CC void hookRenderFadingInEntities(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        hookRenderDispatch<kH_RenderFadingInEntities, renderFadingInEntitiesImpl>(rdram, ctx, runtime);
    }

    HookSlot g_hooks[kNumHooks] = {
        {0x00367528u, &hookPointInBox, &FUN_00367528_0x367528, nullptr},
        {0x003679D8u, &hookZoneBoxLookup, &FUN_003679d8_0x3679d8, nullptr},
        {0x00367AC8u, &hookSectorBoxScan1, &FUN_00367ac8_0x367ac8, nullptr},
        {0x00367BB0u, &hookSectorBoxScan2, &FUN_00367bb0_0x367bb0, nullptr},
        {0x0035E590u, &hookClipEdgeZ, &FUN_0035e590_0x35e590, nullptr},
        {0x0036F0C0u, &hookGetCamDistSq, &FUN_0036f0c0_0x36f0c0, nullptr},
        {0x003C34D0u, &hookIsEntityInActiveInterior, &FUN_003c34d0_0x3c34d0, nullptr},
        {0x003814A0u, &hookCalcFrustumPlanes, &FUN_003814a0_0x3814a0, nullptr},
        {0x003816F8u, &hookIsSphereVisible, &FUN_003816f8_0x3816f8, nullptr},
        {0x00241A80u, &hookRetZero241A80, &FUN_00241a80_0x241a80, nullptr},
        {0x003774A8u, &hookGetPedFadeAlphaMode, &FUN_003774a8_0x3774a8, nullptr},
        {0x003C42C0u, &hookTestEntitySphereGate, &FUN_003c42c0_0x3c42c0, nullptr},
        {0x003C4318u, &hookTestEntitySphereVisible, &FUN_003c4318_0x3c4318, nullptr},

        {0x0035EFD8u, &hookSetupEntityVisibility, &FUN_0035efd8_0x35efd8, nullptr},
        {0x0035F910u, &hookSetupBigBuildingVis, &FUN_0035f910_0x35f910, nullptr},
        {0x0035FAE8u, &hookScanSectorList, &FUN_0035fae8_0x35fae8, nullptr},
        {0x0035FED0u, &hookScanBigBuildingList, &FUN_0035fed0_0x35fed0, nullptr},
        {0x003602F8u, &hookScanSectorRequestModels, &FUN_003602f8_0x3602f8, nullptr},
        {0x0035E620u, &hookScanWorld, &FUN_0035e620_0x35e620, nullptr},
        {0x0035C310u, &hookScanWorldWrapper, &FUN_0035c310_0x35c310, nullptr},

        {0x0035C2C8u, &hookResetVisibleLists, &FUN_0035c2c8_0x35c2c8, nullptr},
        {0x0035C618u, &hookRenderBodies, &FUN_0035c618_0x35c618, nullptr},
        {0x0035C710u, &hookRenderUnderwaterAlpha, &FUN_0035c710_0x35c710, nullptr},
        {0x0035C748u, &hookRenderRoads, &FUN_0035c748_0x35c748, nullptr},
        {0x0035CD98u, &hookRenderEverythingBarRoads, &FUN_0035cd98_0x35cd98, nullptr},
        {0x0035D0D0u, &hookRenderOpaqueVehicleProps, &FUN_0035d0d0_0x35d0d0, nullptr},
        {0x0035D308u, &hookRenderAlphaAndSorted, &FUN_0035d308_0x35d308, nullptr},
        {0x0035D560u, &hookRenderFirstPersonVehicle, &FUN_0035d560_0x35d560, nullptr},
        {0x0035D5E8u, &hookRenderOneEntity, &FUN_0035d5e8_0x35d5e8, nullptr},
        {0x0035D610u, &hookRenderEntityWithAlpha, &FUN_0035d610_0x35d610, nullptr},
        {0x0035DA40u, &hookSetAlphaAndRenderState, &FUN_0035da40_0x35da40, nullptr},
        {0x0036D480u, &hookInsertEntityIntoList, &FUN_0036d480_0x36d480, nullptr},
        {0x0036DB80u, &hookRenderClumpDist, &FUN_0036db80_0x36db80, nullptr},
        {0x0036DBF8u, &hookRenderAtomicAlphaCb, &FUN_0036dbf8_0x36dbf8, nullptr},
        {0x0036EB70u, &hookRenderWeaponPedAlphaCb, &FUN_0036eb70_0x36eb70, nullptr},
        {0x0036ED60u, &hookSetAtomicAlpha, &FUN_0036ed60_0x36ed60, nullptr},
        {0x0036ED70u, &hookClearVehicleAlphaList, &FUN_0036ed70_0x36ed70, nullptr},
        {0x0036ED90u, &hookClearWeaponAlphaList, &FUN_0036ed90_0x36ed90, nullptr},
        {0x0036EDB0u, &hookClearEntityAlphaLists, &FUN_0036edb0_0x36edb0, nullptr},
        {0x0036EE70u, &hookInsertEntitySortedFade, &FUN_0036ee70_0x36ee70, nullptr},
        {0x0036EED0u, &hookRenderWeaponAlphaList, &FUN_0036eed0_0x36eed0, nullptr},
        {0x0036EF38u, &hookRenderBoatAlphaList, &FUN_0036ef38_0x36ef38, nullptr},
        {0x0036EFA8u, &hookRenderSortedFadeList, &FUN_0036efa8_0x36efa8, nullptr},
        {0x0036F0A0u, &hookRenderFadingInEntities, &FUN_0036f0a0_0x36f0a0, nullptr},
    };

    void dumpPerHookStats(const char *tag, bool includeZero = false)
    {
        for (int i = 0; i < kNumHooks; ++i)
        {
            const uint64_t chk = g_verifyStats.perHookChecked[i].load(std::memory_order_relaxed);
            const uint64_t df = g_verifyStats.perHookDiffs[i].load(std::memory_order_relaxed);
            const uint64_t fb = g_verifyStats.perHookFallbacks[i].load(std::memory_order_relaxed);
            if (df > 0 || fb > 0 || (includeZero && chk > 0))
            {
                std::fprintf(stderr,
                             "[renderer-verify]   %s hook=%d (0x%08X): checked=%llu diffs=%llu fallbacks=%llu\n",
                             tag, i, g_hooks[i].addr,
                             static_cast<unsigned long long>(chk),
                             static_cast<unsigned long long>(df),
                             static_cast<unsigned long long>(fb));
            }
        }
    }

    void dumpVerifySummary()
    {
        if (!verifyEnabled())
            return;
        if (g_verifyStats.dumpDone.exchange(true, std::memory_order_relaxed))
            return;
        std::fprintf(stderr,
                     "[renderer-verify] summary: frames=%u checked=%llu diffs=%llu fallbacks=%llu\n",
                     g_verifyStats.frames.load(std::memory_order_relaxed),
                     static_cast<unsigned long long>(g_verifyStats.checked.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(g_verifyStats.diffs.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(g_verifyStats.fallbacks.load(std::memory_order_relaxed)));
        dumpPerHookStats("sum", true);
    }
} // namespace

void install(PS2Runtime &runtime)
{
    if (!enabled())
    {
        std::fprintf(stderr, "[renderer] disabled via VCS_NATIVE_RENDERER=0\n");
        return;
    }

    const char *verEnv = std::getenv("VCS_NATIVE_RENDERER_VERIFY");
    g_verify = verEnv && verEnv[0] == '1';

    g_rangeLo = 0;
    g_rangeHi = kNumHooks - 1;
    if (const char *rng = std::getenv("VCS_NATIVE_RENDERER_RANGE"); rng && *rng)
    {
        int lo = 0, hi = kNumHooks - 1;
        if (std::sscanf(rng, "%d:%d", &lo, &hi) == 2)
        {
            g_rangeLo = std::max(0, lo);
            g_rangeHi = std::min(kNumHooks - 1, hi);
        }
        else if (std::sscanf(rng, "%d", &lo) == 1)
        {
            g_rangeLo = lo;
            g_rangeHi = lo;
        }
    }
    g_fullRange = (g_rangeLo == 0 && g_rangeHi == kNumHooks - 1);

    int installed = 0;
    for (int i = 0; i < kNumHooks; ++i)
    {
        if (i < g_rangeLo || i > g_rangeHi)
            continue;
        // Preserve already-installed wrapper (e.g. vcs_display::sphereVisible on 0x003816F8) as orig
        if (const auto prev = runtime.lookupFunction(g_hooks[i].addr))
            g_hooks[i].orig = prev;
        runtime.replaceFunction(g_hooks[i].addr, g_hooks[i].hook);
        ++installed;
    }

    if (verifyEnabled())
    {
        static bool registered = false;
        if (!registered)
        {
            registered = true;
            std::atexit(&dumpVerifySummary);
        }
    }

    std::fprintf(stderr, "[renderer] installed %d/%d native renderer hooks (range=%d:%d verify=%d)\n",
                 installed, kNumHooks, g_rangeLo, g_rangeHi, verifyEnabled() ? 1 : 0);
}

void onFrameKick()
{
    if (!verifyEnabled())
        return;
    const uint32_t f = g_verifyStats.frames.fetch_add(1, std::memory_order_relaxed) + 1u;
    if ((f % 180u) == 0u)
    {
        std::fprintf(stderr,
                     "[renderer-verify] frame=%u checked=%llu diffs=%llu fallbacks=%llu\n",
                     f,
                     static_cast<unsigned long long>(g_verifyStats.checked.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(g_verifyStats.diffs.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(g_verifyStats.fallbacks.load(std::memory_order_relaxed)));
        dumpPerHookStats("f", f == 1800u);
    }
}
} // namespace vcs::renderer
