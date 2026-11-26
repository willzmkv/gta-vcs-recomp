#pragma STDC FP_CONTRACT OFF

#include "game/vcs_rsl_render_native.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

extern void ps2PushCheckpointDefer() noexcept;
extern void ps2PopCheckpointDefer() noexcept;

namespace
{
    // Addresses referenced across the RSL / RenderWare submission pipeline (SLUS-21590).
    constexpr uint32_t kRamMask = PS2_RAM_MASK;
    constexpr uint32_t kRamSize = PS2_RAM_SIZE;

    constexpr uint32_t kRenderStatePtr = 0x0048F820u;     // initialized by FUN_00472CF8
    constexpr uint32_t kRenderInitFlag = 0x005102ECu;     // initialized by FUN_002654A0
    constexpr uint32_t kRenderSysBase = 0x005102C0u;      // FUN_00265880 return
    constexpr uint32_t kUvMtxInitFlag = 0x00520F80u;      // initialized by FUN_004037E8 + FUN_00460530
    constexpr uint32_t kUvMtxBase = 0x00520F50u;          // active UV tex matrix
    constexpr uint32_t kTexDictCount = 0x004CD10Cu;       // active TXD table length
    constexpr uint32_t kTexDictTable = 0x00486518u;       // active TXD table base ptr
    constexpr uint32_t kEnvStateBase = 0x006EDCA0u;       // UV/env texture state (0x6F0000 - 0x2360)
    constexpr uint32_t kDrawOrigin = 0x006FE980u;         // camera/world origin offset (3 floats)
    constexpr uint32_t kFadeAlpha = 0x004CD114u;          // global alpha scale
    constexpr uint32_t kForceZeroAlpha = 0x00489EB0u;     // force alpha=0 flag
    constexpr uint32_t kForceFullAlpha = 0x00489EB4u;     // force alpha=255 flag
    constexpr uint32_t kForceShadowPass = 0x00486E0Cu;    // force-opaque / strip skip flag
    constexpr uint32_t kRimLightFlag = 0x00489EB8u;       // extra rim/ambient setup flag
    constexpr uint32_t kFixedLightDir = 0x00711730u;      // fixed light direction vec3

    constexpr uint32_t kLightAmbientPtr = 0x004864D0u;
    constexpr uint32_t kLightMainDirPtr = 0x004864D4u;
    constexpr uint32_t kLightExtraPtrs = 0x004864D8u;
    constexpr uint32_t kLightExtraCount = 0x004864E4u;
    constexpr uint32_t kAmbientEnableFlag = 0x004864CCu;
    constexpr uint32_t kAmbientWorldObjPtr = 0x00486838u; // +0x3280 ambient RGB, +0x3378 dir flag
    constexpr uint32_t kDefaultAmbientRgb = 0x004A6EE0u;
    constexpr uint32_t kOverrideAmbientRgb = 0x004E5BB8u;

    constexpr uint32_t kDirtyFlags = 0x004882A0u;
    constexpr uint32_t kBoundFrameCtx = 0x004882A4u;
    constexpr uint32_t kBoundObjCtx = 0x004882A8u;
    constexpr uint32_t kPipelineMode = 0x004882ACu;
    constexpr uint32_t kAmbientColorPacked = 0x004882B4u;
    constexpr uint32_t kBoneScalePtr = 0x004882B8u;
    constexpr uint32_t kViewportScaleXArr = 0x004882C0u;
    constexpr uint32_t kViewportScaleYArr = 0x004882C8u;
    constexpr uint32_t kViewportMode = 0x004882D0u;
    constexpr uint32_t kCamDirtyFlags = 0x004882D4u;
    constexpr uint32_t kCamProgStructPtr = 0x004882D8u;
    constexpr uint32_t kGsTex0State = 0x004882E0u;
    constexpr uint32_t kGsTex1State = 0x004882F0u;
    constexpr uint32_t kClutCachePtrs = 0x00488300u;
    constexpr uint32_t kGsQueueCount = 0x00488308u;
    constexpr uint32_t kCallFlags830C = 0x0048830Cu;
    constexpr uint32_t kUseTreeUpload = 0x00488318u;

    constexpr uint32_t kGsQueueBase = 0x00729510u; // 0x730000 - 0x6AF0
    constexpr uint32_t kPktBufBase = 0x00520F90u;
    constexpr uint32_t kPktBufCursor = 0x00520F90u + 0x18u;
    constexpr uint32_t kPktBufTail = 0x00520F90u + 0x1Cu;

    constexpr uint32_t kModelMatrix = 0x00520860u;
    constexpr uint32_t kViewMatrix = 0x005208A0u;
    constexpr uint32_t kViewportMatrix = 0x00520920u;
    constexpr uint32_t kViewportDims = 0x00520AC0u;
    constexpr uint32_t kFrustumPlanes = 0x00520AD0u;
    constexpr uint32_t kQuantScaleMirror = 0x00520D10u;
    constexpr uint32_t kLog2PolyCoeffs = 0x00520D50u;

    constexpr uint32_t kRslViewRect = 0x00742AC0u;
    constexpr uint32_t kRslRectScratch = 0x00742AD0u;
    constexpr uint32_t kRslGsTestRegs = 0x00742AE0u;
    constexpr uint32_t kRslClipProgOff = 0x00742B10u;
    constexpr uint32_t kRslTexVramBase = 0x00742B30u;
    constexpr uint32_t kRslClutVramBase = 0x00742B40u;
    constexpr uint32_t kRslTexCacheKeys = 0x00742B50u;

    constexpr uint32_t kTxdSlotIndex = 0x00520FA4u;
    constexpr uint32_t kTxdSlotCache = 0x0048A028u;
    constexpr uint32_t kTxdPoolPtr = 0x00489ECCu;
    constexpr uint32_t kDummyAlloc52F420 = 0x0052F420u;

    constexpr uint32_t kDirtyMaskTable08 = 0x004B4C08u;
    constexpr uint32_t kPsmJumpTable = 0x004B4C10u;
    constexpr uint32_t kDirtyMaskTableC8 = 0x004B4CC8u;

    constexpr uint32_t kAlphaRefScale = 0x004CD570u;
    constexpr uint32_t kAlphaBlendMode = 0x004CD578u;
    constexpr uint32_t kRimMaskFlag = 0x004CD57Cu;
    constexpr uint32_t kFrustumResult = 0x004CD580u;
    constexpr uint32_t kRslAlphaRef = 0x004E4878u;

    constexpr uint32_t kGuestPacketEnd = 0x000E6670u;

    struct Vu0RoundGuard
    {
        uint32_t saved;
        Vu0RoundGuard() noexcept : saved(_mm_getcsr()) { _mm_setcsr((saved & ~0x6000u) | 0x6000u); }
        ~Vu0RoundGuard() noexcept { _mm_setcsr(saved); }
        Vu0RoundGuard(const Vu0RoundGuard &) = delete;
        Vu0RoundGuard &operator=(const Vu0RoundGuard &) = delete;
    };

    inline bool validRam(uint32_t addr, uint32_t bytes = 4u) noexcept
    {
        return addr != 0u && addr < kRamSize && bytes <= kRamSize && addr <= kRamSize - bytes;
    }

    inline uint8_t rd8(const uint8_t *rdram, uint32_t addr) noexcept
    {
        return rdram[addr & kRamMask];
    }

    inline int16_t rdS16(const uint8_t *rdram, uint32_t addr) noexcept
    {
        int16_t v;
        std::memcpy(&v, rdram + (addr & kRamMask), 2);
        return v;
    }

    inline uint16_t rdU16(const uint8_t *rdram, uint32_t addr) noexcept
    {
        uint16_t v;
        std::memcpy(&v, rdram + (addr & kRamMask), 2);
        return v;
    }

    inline uint32_t rd32(const uint8_t *rdram, uint32_t addr) noexcept
    {
        uint32_t v;
        std::memcpy(&v, rdram + (addr & kRamMask), 4);
        return v;
    }

    inline int32_t rdS32(const uint8_t *rdram, uint32_t addr) noexcept
    {
        int32_t v;
        std::memcpy(&v, rdram + (addr & kRamMask), 4);
        return v;
    }

    inline uint64_t rd64(const uint8_t *rdram, uint32_t addr) noexcept
    {
        uint64_t v;
        std::memcpy(&v, rdram + (addr & kRamMask), 8);
        return v;
    }

    inline float rdF32(const uint8_t *rdram, uint32_t addr) noexcept
    {
        float v;
        std::memcpy(&v, rdram + (addr & kRamMask), 4);
        return v;
    }

    inline void wr8(uint8_t *rdram, uint32_t addr, uint8_t v) noexcept
    {
        rdram[addr & kRamMask] = v;
    }

    inline void wr16(uint8_t *rdram, uint32_t addr, uint16_t v) noexcept
    {
        std::memcpy(rdram + (addr & kRamMask), &v, 2);
    }

    inline void wr32(uint8_t *rdram, uint32_t addr, uint32_t v) noexcept
    {
        std::memcpy(rdram + (addr & kRamMask), &v, 4);
    }

    inline void wr64(uint8_t *rdram, uint32_t addr, uint64_t v) noexcept
    {
        std::memcpy(rdram + (addr & kRamMask), &v, 8);
    }

    inline void wrF32(uint8_t *rdram, uint32_t addr, float v) noexcept
    {
        std::memcpy(rdram + (addr & kRamMask), &v, 4);
    }

    inline float bitsToF32(uint32_t bits) noexcept
    {
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }

    inline uint32_t f32ToBits(float f) noexcept
    {
        uint32_t b;
        std::memcpy(&b, &f, 4);
        return b;
    }

    inline bool watchpointsActive() noexcept
    {
        return g_ps2ReadWatch.span != 0u || g_ps2WriteWatch.span != 0u;
    }

    // Hooked addresses table — originals saved BEFORE vcs::distant / vcs::rslnative / vcs::rslworld install.
    struct HookSlot
    {
        uint32_t addr;
        PS2Runtime::RecompiledFunction hook;
        PS2Runtime::RecompiledFunction orig;
        PS2Runtime::RecompiledFunction outer;
    };

    enum HookIdx : size_t
    {
        kH_EntityRender = 0, // 0x003C3E58
        kH_ClumpRender,      // 0x002C0270
        kH_AtomicRender,     // 0x0039EC18
        kH_StripRender,      // 0x0039E828
        kH_AmbientPack,      // 0x0039E7F0
        kH_TexBind,          // 0x00408F30 (wrapped by vcs::rslworld)
        kH_TexApply,         // 0x00317FB8
        kH_EnvMtx,           // 0x00317F10
        kH_UvSet,            // 0x00317D48
        kH_SetTexture,       // 0x002F62B8 (wrapped by vcs::rslworld)
        kH_SetClut,          // 0x002F6A78 (wrapped by vcs::rslworld)
        kH_DrawModel,        // 0x002F7D58 (wrapped by vcs::rslworld)
        kH_DrawModelB,       // 0x002F80A0 (wrapped by vcs::rslworld)
        kH_DrawWorld,        // 0x002F7EB0 (wrapped by vcs::rslworld)
        kH_GsSendA,          // 0x002F7908 (wrapped by vcs::rslworld)
        kH_GsSendB,          // 0x002F7B30 (wrapped by vcs::rslworld)
        kH_SendTexMtx,       // 0x002F7090 (wrapped by vcs::rslworld)
        kH_FrustumSphere,    // 0x002F7430
        kH_FrustumBox,       // 0x002F7528
        kH_BuildClipPlanes,  // 0x002F7620
        kH_FrameGetLtm,      // 0x001B50B0
        kH_FrameUpdateRoot,  // 0x001B5040
        kH_TxdSlotTouch,     // 0x003CD678
        kH_AtomicGetTxd,     // 0x0036DA30
        kH_SetupLights,      // 0x001A73E0
        kH_ResetLights,      // 0x001A7038
        kH_SetAmbientLight,  // 0x001A70B0
        kH_SetDirLight,      // 0x001A7260
        kH_SetLightAmbientG, // 0x001CECE8
        kH_SetLightSpecG,    // 0x001CED88
        kH_SetLightDirVec,   // 0x0040F428
        kH_SetLightDirRgb,   // 0x0040F4B8
        kH_SetLightAmbRgb,   // 0x0040F530
        kH_SetLightUnkRgb,   // 0x0040F5A0
        kH_SlotInc,          // 0x001CEB00
        kH_SlotIncCheck,     // 0x001CEA60
        kH_SlotClearCheck,   // 0x001CEA90
        kH_PoolIncRef,       // 0x003A5B98
        kH_PoolRelease,      // 0x003A5BF0
        kH_ResFlag,          // 0x002BCF88
        kH_SkinWrite,        // 0x002A9228
        kH_MatSetAlpha,      // 0x002F3908
        kH_SetModelMtx,      // 0x002F5560
        kH_SetBoneScalePtr,  // 0x002F55B0
        kH_SetBoneScaleDef,  // 0x002F5608
        kH_SetFrameCtx,      // 0x002F5CD8
        kH_SetPipelineMode,  // 0x002F5D40
        kH_SetRimFlag,       // 0x002F5D68
        kH_SetClipMode,      // 0x002F5E30
        kH_SetAmbPacked,     // 0x002F5EA0
        kH_SetScissorRect,   // 0x002F6088
        kH_IsClipNear,       // 0x002F6190
        kH_SetGsTestBits,    // 0x002F61C0
        kH_ModGsTestFlag,    // 0x002F6268
        kH_SetAlphaTest,     // 0x0023E720
        kH_SetRimAlpha,      // 0x0023E4C8
        kH_SetDirty4,        // 0x002F7370
        kH_SetupViewport,    // 0x002F5A18
        kH_SetViewportAlpha, // 0x002F5C38
        kH_SetViewportMode,  // 0x002F5C80
        kH_EvalLogPoly,      // 0x002F7008
        kH_SetTex1Fields,    // 0x002F6F58
        kH_EmitTexUpload,    // 0x00266608
        kH_PktRoom,          // 0x00478628
        kH_PktRoomRaw,       // 0x00476770
        kH_SetTexMipBits,    // 0x0047A850
        kH_SysGet,           // 0x00265880
        kH_VuDot4,           // 0x0032E3D0
        kH_VuNorm3,          // 0x0032E440
        kH_BuildVpMatrix,    // 0x003F9FA0
        kH_IdentityMtx,      // 0x003F9E50
        kH_Copy64,           // 0x001AA998
        kH_VuSphereClip,     // 0x0030C8A8
        kH_VuBoxClip,        // 0x0030C8D4 (optionally wrapped by vcs::overrides)
        kH_VuStore12,        // 0x0030C900
        kH_VuMul4,           // 0x0030C940
        kH_VuSetVi15,        // 0x0030C978
        kH_CvtW,             // 0x0030C984
        kH_UvMtxIdentity,    // 0x004037E8
        kH_UvMtxSet,         // 0x00403848
        kH_MatGetRGBA,       // 0x0042C0E8
        kH_MatGetType,       // 0x0042C0F0
        kH_MatGetFloat,      // 0x0042C2D0
        kH_GeomGetSkin,      // 0x0024E650
        kH_SkinNumBones,     // 0x0024E668
        kH_HAnimGetLtms,     // 0x0024E878
        kH_SkinInvBind,      // 0x0024EB90
        kH_AtomicGetHAnim,   // 0x0024EBB0
        kH_Noop2E6218,       // 0x002E6218
        kH_Count
    };

    extern std::array<HookSlot, kH_Count> g_hooks;
    PS2Runtime::RecompiledFunction g_outerFlush = nullptr;      // 0x002F8710 (vcs::rslnative)
    PS2Runtime::RecompiledFunction g_outerUpload = nullptr;     // 0x003298D8 (vcs::rslworld)
    PS2Runtime::RecompiledFunction g_outerFrameUpd = nullptr;   // 0x001B4FB8 (vcs::overrides::rwFrameUpdateNative)
    std::once_flag g_outerOnce;

    bool g_verify = false;
    thread_local int t_verifyDepth = 0;

    void initOuterHooks(PS2Runtime *runtime)
    {
        for (auto &h : g_hooks)
            h.outer = runtime->lookupFunction(h.addr);
        g_outerFlush = runtime->lookupFunction(0x002F8710u);
        g_outerUpload = runtime->lookupFunction(0x003298D8u);
        g_outerFrameUpd = runtime->lookupFunction(0x001B4FB8u);
    }

    inline void ensureOuterHooks(PS2Runtime *runtime)
    {
        std::call_once(g_outerOnce, [runtime]() { initOuterHooks(runtime); });
    }

    // Invokes a downstream guest/wrapped function cleanly with decremented 16B-aligned $sp and explicit $ra.
    inline bool callWrapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                            PS2Runtime::RecompiledFunction fn, uint32_t targetPc, uint32_t retAddr, uint32_t frameBytes = 0x20u)
    {
        const uint32_t savedSp = GPR_U32(ctx, 29);
        const uint32_t savedRa = GPR_U32(ctx, 31);
        const uint32_t newSp = (savedSp - frameBytes) & ~0xFu;
        SET_GPR_U64(ctx, 29, static_cast<uint64_t>(static_cast<int32_t>(newSp)));
        SET_GPR_U64(ctx, 31, static_cast<uint64_t>(static_cast<int32_t>(retAddr)));
        ctx->pc = targetPc;
        fn(rdram, ctx, runtime);
        const uint32_t postPc = ctx->pc;
        SET_GPR_U64(ctx, 29, static_cast<uint64_t>(static_cast<int32_t>(savedSp)));
        SET_GPR_U64(ctx, 31, static_cast<uint64_t>(static_cast<int32_t>(savedRa)));
        return !runtime->isStopRequested() && !g_checkpointUnwind && postPc == retAddr;
    }

    // -------------------------------------------------------------------------
    // Packet buffer / leaf utility helpers
    // -------------------------------------------------------------------------

    inline uint32_t pktAvailRaw(const uint8_t *rdram, uint32_t pktHdr) noexcept
    {
        const uint32_t sizeOrFlags = rd32(rdram, pktHdr + 0x14u);
        const uint32_t cur = rd32(rdram, pktHdr + 0x18u);
        const uint32_t tail = rd32(rdram, pktHdr + 0x1Cu);
        const int32_t used = static_cast<int32_t>(cur - ((pktHdr + 0x10u) + sizeOrFlags * kGuestPacketEnd)) >> 4;
        const uint32_t total = (pktHdr + 0x20u + sizeOrFlags * kGuestPacketEnd) + kGuestPacketEnd - tail;
        return total + static_cast<uint32_t>(used << 4);
    }

    inline uint32_t pktAvail(const uint8_t *rdram, uint32_t pktHdr) noexcept
    {
        return kGuestPacketEnd - pktAvailRaw(rdram, pktHdr);
    }

    inline void dirtyOr(uint8_t *rdram, uint32_t mask) noexcept
    {
        wr32(rdram, kDirtyFlags, rd32(rdram, kDirtyFlags) | mask);
    }

    inline void camDirtyOr(uint8_t *rdram, uint32_t mask) noexcept
    {
        wr32(rdram, kCamDirtyFlags, rd32(rdram, kCamDirtyFlags) | mask);
    }

    // Exact match for FUN_0032E440 (VU0 normalize3 via RZ rounding, zeroes +0x0C).
    inline bool runVuNorm3(uint8_t *rdram, uint32_t vecAddr) noexcept
    {
        if (!validRam(vecAddr, 16u))
            return false;
        const uint32_t aligned = vecAddr & ~0xFu;
        const __m128 vf1 = _mm_loadu_ps(reinterpret_cast<const float *>(rdram + aligned));
        const __m128 vf0 = _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f);
        const __m128 vf2xyz = PS2_VMUL(vf1, vf1);
        __m128 acc = PS2_VMUL(vf0, _mm_shuffle_ps(vf2xyz, vf2xyz, _MM_SHUFFLE(0, 0, 0, 0)));
        acc = PS2_VADD(acc, PS2_VMUL(vf0, _mm_shuffle_ps(vf2xyz, vf2xyz, _MM_SHUFFLE(1, 1, 1, 1))));
        const __m128 lenSqW = PS2_VADD(acc, PS2_VMUL(vf0, _mm_shuffle_ps(vf2xyz, vf2xyz, _MM_SHUFFLE(2, 2, 2, 2))));
        const float ft = _mm_cvtss_f32(_mm_shuffle_ps(lenSqW, lenSqW, _MM_SHUFFLE(0, 0, 0, 3)));
        const float q = (ft > 0.0f) ? (1.0f / std::sqrt(ft)) : 0.0f;
        const __m128 res = PS2_VMUL(vf1, _mm_set1_ps(q));
        _mm_storeu_ps(reinterpret_cast<float *>(rdram + aligned), res);
        wr32(rdram, vecAddr + 0x0Cu, 0u);
        return true;
    }

    // Exact match for FUN_0032E3D0 (VU0 4-lane dot product).
    inline float runVuDot4(const uint8_t *rdram, uint32_t aAddr, uint32_t bAddr) noexcept
    {
        Vu0RoundGuard guard;
        const __m128 vf0 = _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f);
        const __m128 vf4x = PS2_VADD(vf0, _mm_shuffle_ps(vf0, vf0, _MM_SHUFFLE(3, 3, 3, 3))); // 1.0 in w->x
        const __m128 vf1 = _mm_loadu_ps(reinterpret_cast<const float *>(rdram + (aAddr & ~0xFu)));
        const __m128 vf2 = _mm_loadu_ps(reinterpret_cast<const float *>(rdram + (bAddr & ~0xFu)));
        const __m128 vf3 = PS2_VMUL(vf2, vf1);
        __m128 acc = PS2_VMUL(vf4x, _mm_shuffle_ps(vf3, vf3, _MM_SHUFFLE(0, 0, 0, 0)));
        acc = PS2_VADD(acc, PS2_VMUL(vf4x, _mm_shuffle_ps(vf3, vf3, _MM_SHUFFLE(1, 1, 1, 1))));
        acc = PS2_VADD(acc, PS2_VMUL(vf4x, _mm_shuffle_ps(vf3, vf3, _MM_SHUFFLE(2, 2, 2, 2))));
        const __m128 out = PS2_VADD(acc, PS2_VMUL(vf4x, _mm_shuffle_ps(vf3, vf3, _MM_SHUFFLE(3, 3, 3, 3))));
        return _mm_cvtss_f32(out);
    }

    // Exact match for FUN_003F9E50.
    inline void identityMtx(uint8_t *rdram, uint32_t dstAddr) noexcept
    {
        std::memset(rdram + (dstAddr & kRamMask), 0, 64u);
        wrF32(rdram, dstAddr + 0x00u, 1.0f);
        wrF32(rdram, dstAddr + 0x14u, 1.0f);
        wrF32(rdram, dstAddr + 0x28u, 1.0f);
        wrF32(rdram, dstAddr + 0x3Cu, 1.0f);
    }

    // Exact match for FUN_002F7008.
    inline float evalLogPoly(const uint8_t *rdram, float x) noexcept
    {
        const uint32_t bits = f32ToBits(x);
        const float m = bitsToF32((bits & 0xC07FFFFFu) | 0x3F800000u);
        const int32_t exp = static_cast<int32_t>((static_cast<int32_t>(bits) >> 23) & 0x7F) - 0x7F;
        const float m2 = FPU_MUL_S(m, m);
        const float m3 = FPU_MUL_S(m2, m);

        Vu0RoundGuard guard;
        const __m128 vf0 = _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f);
        const __m128 vf4x = PS2_VADD(vf0, _mm_shuffle_ps(vf0, vf0, _MM_SHUFFLE(3, 3, 3, 3)));
        const __m128 vf1 = _mm_setr_ps(m3, m2, m, 1.0f);
        const __m128 vf2 = _mm_loadu_ps(reinterpret_cast<const float *>(rdram + (kLog2PolyCoeffs & ~0xFu)));
        const __m128 vf3 = PS2_VMUL(vf2, vf1);
        __m128 acc = PS2_VMUL(vf4x, _mm_shuffle_ps(vf3, vf3, _MM_SHUFFLE(0, 0, 0, 0)));
        acc = PS2_VADD(acc, PS2_VMUL(vf4x, _mm_shuffle_ps(vf3, vf3, _MM_SHUFFLE(1, 1, 1, 1))));
        acc = PS2_VADD(acc, PS2_VMUL(vf4x, _mm_shuffle_ps(vf3, vf3, _MM_SHUFFLE(2, 2, 2, 2))));
        const __m128 out = PS2_VADD(acc, PS2_VMUL(vf4x, _mm_shuffle_ps(vf3, vf3, _MM_SHUFFLE(3, 3, 3, 3))));
        const float poly = _mm_cvtss_f32(out);
        return FPU_ADD_S(FPU_CVT_S_W(exp), poly);
    }

    // Exact match for FUN_002F6F58.
    inline void setTex1Fields(uint8_t *rdram, uint32_t unit, uint32_t l, uint32_t k, uint32_t low) noexcept
    {
        const uint32_t slotAddr = kGsTex1State + (unit << 3);
        const uint64_t prev = rd64(rdram, slotAddr);
        const uint64_t bits = (static_cast<uint64_t>(k) << 32) | (static_cast<uint64_t>(l) << 19) | static_cast<uint64_t>(low);
        if ((prev & 0x00000FFF00180001ULL) != bits)
        {
            wr64(rdram, slotAddr, (prev & 0xFFFFF000FFE7FFFEULL) | bits);
            dirtyOr(rdram, rd32(rdram, kDirtyMaskTableC8 + (unit << 2)));
        }
    }

    // Exact match for FUN_0047A850.
    inline void setTexMipBits(uint8_t *rdram, uint32_t unit, uint32_t numMips) noexcept
    {
        const uint32_t slotAddr = kGsTex1State + (unit << 3);
        const uint64_t prev = rd64(rdram, slotAddr);
        const uint64_t val = static_cast<uint64_t>(static_cast<uint32_t>((numMips - 1u) << 2));
        if ((prev & 0x1CuLL) != val)
        {
            wr64(rdram, slotAddr, (prev & ~0x1CuLL) | val);
            dirtyOr(rdram, rd32(rdram, kDirtyMaskTableC8 + (unit << 2)));
        }
    }

    // Exact match for FUN_002F61C0.
    inline void setGsTestBits(uint8_t *rdram, uint32_t unit, uint32_t a1) noexcept
    {
        uint32_t val = a1;
        if (unit != 0u)
            val |= 0x200u;
        else
            val &= ~0x200u;
        const uint32_t addr = kRslGsTestRegs + (unit << 4);
        const uint64_t prev = rd64(rdram, addr);
        const uint64_t next = (prev & 0xFE007FFFFFFFFFFFULL) | (static_cast<uint64_t>(val) << 47);
        if (next != prev)
        {
            wr64(rdram, addr, next);
            dirtyOr(rdram, 8u);
        }
        const uint32_t hi = (rd32(rdram, addr + 4u) & 0xFFFC7FFFu) | 0x00028000u;
        wr32(rdram, addr + 0x0Cu, hi);
    }

    // Exact match for FUN_002F6268 + FUN_0023E720.
    inline void setAlphaTestBit(uint8_t *rdram, uint32_t enable) noexcept
    {
        const uint64_t prev = rd64(rdram, kRslGsTestRegs);
        uint32_t top = static_cast<uint32_t>(prev >> 47);
        if (enable != 0u)
            top |= 0x10u;
        else
            top &= ~0x10u;
        setGsTestBits(rdram, 0u, top);
    }

    // Exact match for FUN_002F6088.
    inline void emitScissor(uint8_t *rdram, uint32_t unit, uint32_t rectAddr) noexcept
    {
        const float fx = rdF32(rdram, rectAddr + 0u);
        const float fy = rdF32(rdram, rectAddr + 4u);
        const float fw = rdF32(rdram, rectAddr + 8u);
        const float fh = rdF32(rdram, rectAddr + 12u);
        const int32_t x0 = FPU_CVT_W_S(fx);
        const int32_t y0 = FPU_CVT_W_S(fy);
        const int32_t x1 = FPU_CVT_W_S(FPU_SUB_S(FPU_ADD_S(fx, fw), 1.0f));
        const int32_t y1 = FPU_CVT_W_S(FPU_SUB_S(FPU_ADD_S(fy, fh), 1.0f));

        const uint64_t packed =
            (static_cast<uint64_t>(static_cast<uint32_t>(y1)) << 48) |
            (static_cast<uint64_t>(static_cast<uint32_t>(y0) & 0xFFFFu) << 32) |
            (static_cast<uint64_t>(static_cast<uint32_t>(x1) & 0xFFFFu) << 16) |
            static_cast<uint64_t>(static_cast<uint32_t>(x0));
        const uint64_t reg = (unit != 0u) ? 0x41ULL : 0x40ULL;
        const uint32_t count = rd32(rdram, kGsQueueCount);
        const uint32_t dst = kGsQueueBase + (count << 4);
        wr64(rdram, dst + 0u, packed);
        wr64(rdram, dst + 8u, reg);
        wr32(rdram, kGsQueueCount, count + 1u);
    }

    // Exact match for FUN_002F5A18.
    inline void setupViewport(uint8_t *rdram, uint32_t rectAddr, uint32_t mode, float sx, float sy) noexcept
    {
        const float scaleShift = bitsToF32(0x3D800000u); // 0.0625f
        const uint32_t vpIdx = rd32(rdram, kViewportMode);
        const float s0 = -rdF32(rdram, kViewportScaleXArr + (vpIdx << 2));
        const float s1 = -rdF32(rdram, kViewportScaleYArr + (vpIdx << 2));
        const float mask = static_cast<float>(static_cast<int32_t>((1u << mode) - 1u));
        const float f0 = FPU_ADD_S(FPU_MUL_S(s0, mask), mask);
        const float f1 = FPU_ADD_S(FPU_MUL_S(s1, mask), mask);

        std::memcpy(rdram + kRslRectScratch, rdram + (rectAddr & kRamMask), 16u);
        wr32(rdram, kAlphaBlendMode, mode);
        wrF32(rdram, kAlphaRefScale + 0u, sx);
        wrF32(rdram, kAlphaRefScale + 4u, sy);

        // Inline FUN_003F9FA0 into kViewportMatrix.
        const float p0 = rdF32(rdram, rectAddr + 0u);
        const float p1 = rdF32(rdram, rectAddr + 4u);
        const float p2 = rdF32(rdram, rectAddr + 8u);
        const float p3 = rdF32(rdram, rectAddr + 12u);
        const float half = 0.5f;
        const float negHalf = -0.5f;
        const float m21 = FPU_MUL_S(FPU_ADD_S(FPU_MUL_S(f0, scaleShift), FPU_MUL_S(f1, scaleShift)), half);
        const float m22 = FPU_MUL_S(FPU_SUB_S(FPU_MUL_S(f1, scaleShift), FPU_MUL_S(f0, scaleShift)), half);
        const float m24 = FPU_MUL_S(p2, half);
        const float m23 = FPU_MUL_S(p3, half);
        const float m20 = FPU_ADD_S(FPU_ADD_S(m24, sx), p0);
        const float m23b = FPU_ADD_S(FPU_ADD_S(m23, sy), p1);
        const float m25 = FPU_MUL_S(p3, negHalf);
        identityMtx(rdram, kViewportMatrix);
        wrF32(rdram, kViewportMatrix + 0x00u, m24);
        wrF32(rdram, kViewportMatrix + 0x14u, m25);
        wrF32(rdram, kViewportMatrix + 0x28u, m22);
        wrF32(rdram, kViewportMatrix + 0x30u, m20);
        wrF32(rdram, kViewportMatrix + 0x34u, m23b);
        wrF32(rdram, kViewportMatrix + 0x38u, m21);

        const float px = FPU_ADD_S(FPU_ADD_S(sx, p0), FPU_MUL_S(p2, half));
        const float py = FPU_ADD_S(FPU_ADD_S(sy, p1), FPU_MUL_S(p3, half));
        const float f3 = FPU_MUL_S(p2, half);
        const float f0h = FPU_MUL_S(p3, half);
        const float divX = FPU_C_OLT_S(px, 2048.0f) ? px : FPU_SUB_S(4096.0f, px);
        const float wNorm = FPU_DIV_S(f3, divX);
        wrF32(rdram, kViewportDims + 0u, wNorm);
        const float divY = FPU_C_OLT_S(py, 2048.0f) ? py : FPU_SUB_S(4096.0f, py);
        const float hNorm = FPU_DIV_S(f0h, divY);
        wrF32(rdram, kViewportDims + 4u, hNorm);
        wrF32(rdram, kViewportDims + 8u, 1.0f);
        wrF32(rdram, kViewportDims + 12u, 1.0f);

        emitScissor(rdram, 0u, rectAddr);
        emitScissor(rdram, 1u, rectAddr);
        dirtyOr(rdram, 0x1000u);
        camDirtyOr(rdram, 1u);
    }

    inline void setViewportAlpha(uint8_t *rdram, float x, float y) noexcept
    {
        wrF32(rdram, kViewportScaleXArr + 4u, x);
        wrF32(rdram, kViewportScaleYArr + 4u, y);
        setupViewport(rdram, kRslRectScratch, rd32(rdram, kAlphaBlendMode),
                      rdF32(rdram, kAlphaRefScale), rdF32(rdram, kAlphaRefScale + 4u));
    }

    inline void setViewportMode(uint8_t *rdram, uint32_t m) noexcept
    {
        wr32(rdram, kViewportMode, m != 0u ? 1u : 0u);
        setupViewport(rdram, kRslRectScratch, rd32(rdram, kAlphaBlendMode),
                      rdF32(rdram, kAlphaRefScale), rdF32(rdram, kAlphaRefScale + 4u));
    }

    inline void setRimAlpha(uint8_t *rdram, float v) noexcept
    {
        if (FPU_C_EQ_S(v, 0.0f))
        {
            setViewportMode(rdram, 0u);
        }
        else
        {
            setViewportMode(rdram, 1u);
            const float c0 = bitsToF32(0x358637BDu);
            const float c1 = bitsToF32(0x3C23D70Au);
            const float c2 = bitsToF32(0x3F7D70A4u);
            const float s = FPU_MUL_S(v, c0);
            setViewportAlpha(rdram, FPU_SUB_S(c1, s), FPU_SUB_S(c2, s));
        }
    }

    // Exact match for FUN_002BCF88.
    inline uint32_t resFlag(uint8_t *rdram, uint32_t ptr, uint32_t setBit) noexcept
    {
        const uint32_t hdr = ptr - 8u;
        uint32_t f = rd32(rdram, hdr + 4u);
        if (setBit != 0u)
        {
            f |= 0x10000000u;
            wr32(rdram, hdr + 4u, f);
            return f;
        }
        f &= 0xEFFFFFFFu;
        wr32(rdram, hdr + 4u, f);
        return hdr;
    }

    // Exact match for FUN_003A5B98 + FUN_003A5BF0.
    inline uint32_t lookupTxdEntry(const uint8_t *rdram, int32_t idx) noexcept
    {
        if (idx < 0)
            return 0u;
        const uint32_t pool = rd32(rdram, kTxdPoolPtr);
        if (idx >= static_cast<int32_t>(rd32(rdram, pool + 8u)))
            return 0u;
        if ((rd8(rdram, rd32(rdram, pool + 4u) + static_cast<uint32_t>(idx)) & 0x80u) != 0u)
            return 0u;
        return rd32(rdram, pool) + static_cast<uint32_t>(idx) * 0x1Cu;
    }

    inline void txdSlotTouch(uint8_t *rdram, uint32_t txdObj) noexcept
    {
        if (rd32(rdram, txdObj + 0x0Cu) == 0u)
            return;
        const uint32_t slot = rd32(rdram, kTxdSlotIndex);
        const uint32_t slotOff = slot << 2;
        if (rd32(rdram, txdObj + slotOff) != 0u)
            return;
        wr16(rdram, txdObj + 0x1Cu, static_cast<uint16_t>(rdU16(rdram, txdObj + 0x1Cu) + 1u));
        const int16_t idx = rdS16(rdram, txdObj + 0x1Eu);
        if (idx != -1)
        {
            const uint32_t entry = lookupTxdEntry(rdram, idx);
            wr16(rdram, entry + 4u, static_cast<uint16_t>(rdU16(rdram, entry + 4u) + 1u));
        }
        {
            const uint32_t entry = lookupTxdEntry(rdram, idx);
            const uint16_t c4 = static_cast<uint16_t>(rdU16(rdram, entry + 4u) + 1u);
            const int16_t c6 = rdS16(rdram, entry + 6u);
            wr16(rdram, entry + 4u, c4);
            if (c6 == 0)
            {
                const uint32_t texPtr = rd32(rdram, entry);
                if (texPtr != 0u)
                    resFlag(rdram, texPtr - 0x20u, 0u);
            }
            wr16(rdram, entry + 6u, static_cast<uint16_t>(rdU16(rdram, entry + 6u) + 1u));
        }
        const uint32_t cacheEntry = kTxdSlotCache + slotOff;
        const uint32_t prev = rd32(rdram, cacheEntry);
        wr32(rdram, txdObj + slotOff, prev);
        wr32(rdram, cacheEntry, txdObj);
        resFlag(rdram, rd32(rdram, txdObj + 0x0Cu), 0u);
    }

    inline uint32_t atomicGetTxd(const uint8_t *rdram, uint32_t atomic) noexcept
    {
        const int16_t idx = rdS16(rdram, atomic + 0x28u);
        if (idx == -1)
            return 0u;
        if (idx < static_cast<int32_t>(rd32(rdram, kTexDictCount)))
            return rd32(rdram, rd32(rdram, kTexDictTable) + static_cast<uint32_t>(idx) * 4u);
        return 0u;
    }

    // -------------------------------------------------------------------------
    // Lights & render state setters
    // -------------------------------------------------------------------------

    inline void setLightDirVec(uint8_t *rdram, uint32_t sysLight, uint32_t axis, uint32_t vecAddr) noexcept
    {
        std::array<uint8_t, 16> tmp{};
        std::memcpy(tmp.data(), rdram + (vecAddr & kRamMask), 16u);
        const __m128 vf1 = _mm_loadu_ps(reinterpret_cast<const float *>(tmp.data()));
        const __m128 vf0 = _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f);
        const __m128 vf2xyz = PS2_VMUL(vf1, vf1);
        __m128 acc = PS2_VMUL(vf0, _mm_shuffle_ps(vf2xyz, vf2xyz, _MM_SHUFFLE(0, 0, 0, 0)));
        acc = PS2_VADD(acc, PS2_VMUL(vf0, _mm_shuffle_ps(vf2xyz, vf2xyz, _MM_SHUFFLE(1, 1, 1, 1))));
        const __m128 lenSqW = PS2_VADD(acc, PS2_VMUL(vf0, _mm_shuffle_ps(vf2xyz, vf2xyz, _MM_SHUFFLE(2, 2, 2, 2))));
        const float ft = _mm_cvtss_f32(_mm_shuffle_ps(lenSqW, lenSqW, _MM_SHUFFLE(0, 0, 0, 3)));
        const float q = (ft > 0.0f) ? (1.0f / std::sqrt(ft)) : 0.0f;
        float out[4];
        _mm_storeu_ps(out, PS2_VMUL(vf1, _mm_set1_ps(q)));
        const uint32_t off = axis << 2;
        wrF32(rdram, sysLight + off, FPU_NEG_S(out[0]));
        wrF32(rdram, sysLight + 0x10u + off, FPU_NEG_S(out[1]));
        wrF32(rdram, sysLight + 0x20u + off, FPU_NEG_S(out[2]));
        dirtyOr(rdram, 0x10u);
        wr32(rdram, sysLight + 0x90u, 1u);
    }

    inline void setLightDirRgb(uint8_t *rdram, uint32_t sysLight, uint32_t slot, uint32_t r, uint32_t g, uint32_t b) noexcept
    {
        const uint32_t dst = sysLight + (slot << 4);
        wrF32(rdram, dst + 0x30u, FPU_CVT_S_W(static_cast<int32_t>(r & 0xFFu)));
        wrF32(rdram, dst + 0x34u, FPU_CVT_S_W(static_cast<int32_t>(g & 0xFFu)));
        wrF32(rdram, dst + 0x38u, FPU_CVT_S_W(static_cast<int32_t>(b & 0xFFu)));
        wr32(rdram, dst + 0x3Cu, 0u);
        dirtyOr(rdram, 0x10u);
        wr32(rdram, sysLight + 0x90u, 1u);
    }

    inline void setLightAmbRgb(uint8_t *rdram, uint32_t sysLight, uint32_t r, uint32_t g, uint32_t b) noexcept
    {
        wr32(rdram, sysLight + 0x7Cu, 0u);
        wrF32(rdram, sysLight + 0x70u, FPU_CVT_S_W(static_cast<int32_t>(r & 0xFFu)));
        wrF32(rdram, sysLight + 0x74u, FPU_CVT_S_W(static_cast<int32_t>(g & 0xFFu)));
        wrF32(rdram, sysLight + 0x78u, FPU_CVT_S_W(static_cast<int32_t>(b & 0xFFu)));
        dirtyOr(rdram, 0x10u);
        wr32(rdram, sysLight + 0x90u, 1u);
    }

    inline void setLightUnkRgb(uint8_t *rdram, uint32_t sysLight, uint32_t r, uint32_t g, uint32_t b) noexcept
    {
        wrF32(rdram, sysLight + 0x80u, FPU_CVT_S_W(static_cast<int32_t>(r & 0xFFu)));
        wrF32(rdram, sysLight + 0x84u, FPU_CVT_S_W(static_cast<int32_t>(g & 0xFFu)));
        wrF32(rdram, sysLight + 0x88u, FPU_CVT_S_W(static_cast<int32_t>(b & 0xFFu)));
    }

    inline void setLightAmbientGlobal(uint8_t *rdram, uint32_t lightObj, uint32_t r, uint32_t g, uint32_t b, uint32_t boost) noexcept
    {
        const float k = bitsToF32(0x3B808081u); // 1/255
        float fr = FPU_MUL_S(FPU_CVT_S_W(static_cast<int32_t>(r & 0xFFu)), k);
        float fg = FPU_MUL_S(FPU_CVT_S_W(static_cast<int32_t>(g & 0xFFu)), k);
        float fb = FPU_MUL_S(FPU_CVT_S_W(static_cast<int32_t>(b & 0xFFu)), k);
        wrF32(rdram, lightObj + 0u, fr);
        wrF32(rdram, lightObj + 4u, fg);
        wrF32(rdram, lightObj + 8u, fb);
        if (boost != 0u)
        {
            const float c = bitsToF32(0x3F839CE7u);
            fb = FPU_MUL_S(fb, c);
            fr = FPU_MUL_S(fr, c);
            fg = FPU_MUL_S(fg, c);
            wrF32(rdram, lightObj + 8u, fb);
            wrF32(rdram, lightObj + 0u, fr);
            wrF32(rdram, lightObj + 4u, fg);
        }
        dirtyOr(rdram, 0x10u);
    }

    inline void setLightSpecGlobal(uint8_t *rdram, uint32_t lightObj, uint32_t r, uint32_t g, uint32_t b) noexcept
    {
        const float half = 0.5f;
        wr32(rdram, lightObj + 0x1Cu, 0u);
        wrF32(rdram, lightObj + 0x10u, FPU_MUL_S(FPU_CVT_S_W(static_cast<int32_t>(r & 0xFFu)), half));
        wrF32(rdram, lightObj + 0x14u, FPU_MUL_S(FPU_CVT_S_W(static_cast<int32_t>(g & 0xFFu)), half));
        wrF32(rdram, lightObj + 0x18u, FPU_MUL_S(FPU_CVT_S_W(static_cast<int32_t>(b & 0xFFu)), half));
        dirtyOr(rdram, 0x10u);
    }

    inline uint32_t clamp8u(int32_t v) noexcept
    {
        return v < 0 ? 0u : (v > 0xFF ? 0xFFu : static_cast<uint32_t>(v));
    }

    inline float u32ToF32Ee(int32_t v) noexcept
    {
        if (v >= 0)
            return FPU_CVT_S_W(v);
        const uint32_t u = static_cast<uint32_t>(v);
        const float hf = FPU_CVT_S_W(static_cast<int32_t>((u >> 1) | (u & 1u)));
        return FPU_ADD_S(hf, hf);
    }

    // Exact match for FUN_001A70B0.
    inline void setAmbientLight(uint8_t *rdram, uint32_t ambObj) noexcept
    {
        if (ambObj == 0u)
            return;
        const float scale255 = bitsToF32(0x437F0000u); // 255.0f
        const float scale16 = bitsToF32(0x3FCCCCCCu + 1u); // 0x3FCCCCCD
        const int32_t cb = FPU_CVT_W_S(FPU_MUL_S(rdF32(rdram, ambObj + 0x20u), scale255));
        const int32_t cg = FPU_CVT_W_S(FPU_MUL_S(rdF32(rdram, ambObj + 0x1Cu), scale255));
        const int32_t cr = FPU_CVT_W_S(FPU_MUL_S(rdF32(rdram, ambObj + 0x18u), scale255));
        const uint32_t sysLight = rd32(rdram, kRenderSysBase + 0x10u);
        uint32_t r = static_cast<uint32_t>(FPU_CVT_W_S(FPU_MUL_S(u32ToF32Ee(cr), scale16)));
        uint32_t g = static_cast<uint32_t>(FPU_CVT_W_S(FPU_MUL_S(u32ToF32Ee(cg), scale16)));
        uint32_t b = static_cast<uint32_t>(FPU_CVT_W_S(FPU_MUL_S(u32ToF32Ee(cb), scale16)));
        if (r >= 0x100u) r = 0xFFu;
        if (g >= 0x100u) g = 0xFFu;
        if (b >= 0x100u) b = 0xFFu;
        setLightAmbRgb(rdram, sysLight, r & 0xFFu, g & 0xFFu, b & 0xFFu);
        setLightUnkRgb(rdram, sysLight, 0xFFu, 0xFFu, 0xFFu);
    }

    inline void resetLights(uint8_t *rdram) noexcept
    {
        const uint32_t sysLight = rd32(rdram, kRenderSysBase + 0x10u);
        setLightAmbRgb(rdram, sysLight, 0u, 0u, 0u);
        for (uint32_t i = 0u; i < 4u; ++i)
            setLightDirRgb(rdram, sysLight, i, 0u, 0u, 0u);
    }

    inline bool frameGetLtmImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t frame, uint32_t &outLtm,
                                uint32_t retAddr1B50B0, uint32_t retAddr1B5040) noexcept;

    inline bool frameUpdateRootImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t rootFrame, uint32_t retAddr) noexcept
    {
        const uint32_t flags = rd8(rdram, rootFrame + 3u);
        if (flags & 2u)
            std::memcpy(rdram + ((rootFrame + 0x50u) & kRamMask), rdram + ((rootFrame + 0x10u) & kRamMask), 64u);
        const uint32_t child = rd32(rdram, rootFrame + 0x90u);
        SET_GPR_U64(ctx, 4, static_cast<uint64_t>(static_cast<int32_t>(child)));
        SET_GPR_U64(ctx, 5, static_cast<uint64_t>(static_cast<int32_t>(flags)));
        if (!callWrapped(rdram, ctx, runtime, g_outerFrameUpd, 0x001B4FB8u, retAddr, 0x20u))
            return false;
        wr8(rdram, rootFrame + 3u, static_cast<uint8_t>(flags & 0xFCu));
        return true;
    }

    inline bool frameGetLtmImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t frame, uint32_t &outLtm,
                                uint32_t retAddr1B50B0, uint32_t retAddr1B5040) noexcept
    {
        const uint32_t root = rd32(rdram, frame + 0x98u);
        if ((rd8(rdram, root + 3u) & 1u) != 0u)
        {
            if (!frameUpdateRootImpl(rdram, ctx, runtime, root, retAddr1B5040))
                return false;
            (void)retAddr1B50B0;
        }
        outLtm = frame + 0x50u;
        return true;
    }

    // Exact match for FUN_001A7260.
    inline bool setDirLight(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t light, uint32_t slot, uint32_t retBase) noexcept
    {
        const float scale255 = bitsToF32(0x437F0000u);
        uint32_t b = static_cast<uint32_t>(FPU_CVT_W_S(FPU_MUL_S(rdF32(rdram, light + 0x20u), scale255)));
        uint32_t g = static_cast<uint32_t>(FPU_CVT_W_S(FPU_MUL_S(rdF32(rdram, light + 0x1Cu), scale255)));
        uint32_t r = static_cast<uint32_t>(FPU_CVT_W_S(FPU_MUL_S(rdF32(rdram, light + 0x18u), scale255)));
        uint32_t ltm = 0u;
        if (!frameGetLtmImpl(rdram, ctx, runtime, rd32(rdram, light + 4u), ltm, retBase, 0x001A72A4u))
            return false;
        if (g >= 0x100u) g = 0xFFu;
        if (b >= 0x100u) b = 0xFFu;
        if (r >= 0x100u) r = 0xFFu;
        // Original 0x001A7260 writes -0x711730 to unused $sp+0 then unconditionally passes ltm+0x20..0x28 ($sp+0x10).
        const float dirVec[4]{
            rdF32(rdram, ltm + 0x20u),
            rdF32(rdram, ltm + 0x24u),
            rdF32(rdram, ltm + 0x28u),
            1.0f,
        };
        // 0x40F428 expects a vec4 in guest RAM (use scratch on guest $sp - 0x20).
        const uint32_t tmpAddr = GPR_U32(ctx, 29) - 0x20u;
        std::memcpy(rdram + (tmpAddr & kRamMask), dirVec, 16u);
        const uint32_t sysLight = rd32(rdram, kRenderSysBase + 0x10u);
        setLightDirVec(rdram, sysLight, slot & 0xFFu, tmpAddr);
        setLightDirRgb(rdram, sysLight, slot & 0xFFu, r & 0xFFu, g & 0xFFu, b & 0xFFu);
        return true;
    }

    // Exact match for FUN_001A73E0.
    inline bool setupLights(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t atomic, uint32_t useDir) noexcept
    {
        const uint32_t geom = rd32(rdram, atomic + 0x14u);
        const uint32_t flags = rd32(rdram, geom + 0x34u);
        if (flags & 2u)
        {
            const uint32_t mainDir = rd32(rdram, kLightMainDirPtr);
            if (rd8(rdram, mainDir + 2u) & 1u)
            {
                setAmbientLight(rdram, rd32(rdram, kLightAmbientPtr));
                if (!setDirLight(rdram, ctx, runtime, mainDir, 0u, 0x001A7438u))
                    return false;
                const int32_t extraCount = rdS32(rdram, kLightExtraCount);
                for (int32_t i = 0; i < extraCount;)
                {
                    const uint32_t lPtr = rd32(rdram, kLightExtraPtrs + (static_cast<uint32_t>(i) << 2));
                    ++i;
                    if (!setDirLight(rdram, ctx, runtime, lPtr, static_cast<uint32_t>(i) & 0xFFu, 0x001A7470u))
                        return false;
                }
            }
            else if (useDir != 0u)
            {
                setAmbientLight(rdram, rd32(rdram, kLightAmbientPtr));
            }
            return true;
        }
        if (!(flags & 8u))
        {
            resetLights(rdram);
            return true;
        }

        const uint32_t amb = rd32(rdram, kLightAmbientPtr);
        std::memcpy(rdram + ((amb + 0x18u) & kRamMask), rdram + kDefaultAmbientRgb, 16u);
        setAmbientLight(rdram, amb);
        const float scale255 = bitsToF32(0x437F0000u);
        if (rd32(rdram, kAmbientEnableFlag) != 0u)
        {
            const uint32_t wo = rd32(rdram, kAmbientWorldObjPtr);
            const uint32_t r = clamp8u(FPU_CVT_W_S(FPU_MUL_S(rdF32(rdram, wo + 0x3280u), scale255)));
            const uint32_t g = clamp8u(FPU_CVT_W_S(FPU_MUL_S(rdF32(rdram, wo + 0x3284u), scale255)));
            const uint32_t b = clamp8u(FPU_CVT_W_S(FPU_MUL_S(rdF32(rdram, wo + 0x3288u), scale255)));
            setLightAmbientGlobal(rdram, rd32(rdram, kRenderSysBase + 8u), r, g, b, 1u);
        }
        const uint32_t br = static_cast<uint32_t>(FPU_CVT_W_S(FPU_MUL_S(rdF32(rdram, kOverrideAmbientRgb + 0u), scale255)));
        const uint32_t bg = static_cast<uint32_t>(FPU_CVT_W_S(FPU_MUL_S(rdF32(rdram, kOverrideAmbientRgb + 4u), scale255)));
        const uint32_t bb = static_cast<uint32_t>(FPU_CVT_W_S(FPU_MUL_S(rdF32(rdram, kOverrideAmbientRgb + 8u), scale255)));
        setLightSpecGlobal(rdram, rd32(rdram, kRenderSysBase + 8u), br, bg, bb);
        return true;
    }

    // Exact match for FUN_002F5560.
    inline void setModelMatrix(uint8_t *rdram, const float *mtx64) noexcept
    {
        std::memcpy(rdram + kModelMatrix, mtx64, 64u);
        wr32(rdram, kBoneScalePtr, 0u);
        dirtyOr(rdram, 0x200u);
        camDirtyOr(rdram, 4u);
    }

    inline void setBoneScalePtr(uint8_t *rdram, uint32_t ptr) noexcept
    {
        if (ptr != rd32(rdram, kBoneScalePtr))
        {
            wr32(rdram, kBoneScalePtr, ptr);
            std::memcpy(rdram + kModelMatrix, rdram + (ptr & kRamMask), 64u);
        }
        dirtyOr(rdram, 0x200u);
        camDirtyOr(rdram, 4u);
    }

    inline void setFrameCtx(uint8_t *rdram, uint32_t pipeObj) noexcept
    {
        if (pipeObj != rd32(rdram, kBoundObjCtx))
        {
            wr32(rdram, kBoundObjCtx, pipeObj);
            dirtyOr(rdram, 2u);
        }
        const uint32_t f50 = rd32(rdram, pipeObj + 0x50u);
        if (f50 != rd32(rdram, kBoundFrameCtx))
        {
            const uint32_t cur = rd32(rdram, kDirtyFlags);
            wr32(rdram, kDirtyFlags, cur | 5u);
            wr32(rdram, kBoundFrameCtx, rd32(rdram, pipeObj + 0x50u));
            if (rd32(rdram, kRimMaskFlag) != 0u)
                wr32(rdram, kDirtyFlags, cur | 0x2005u);
        }
    }

    inline void setPipelineMode(uint8_t *rdram, uint32_t mode) noexcept
    {
        if (mode != rd32(rdram, kPipelineMode))
        {
            wr32(rdram, kPipelineMode, mode);
            dirtyOr(rdram, 4u);
        }
    }

    inline void setRimFlag(uint8_t *rdram, uint32_t v) noexcept
    {
        if (v != rd32(rdram, kRimMaskFlag))
        {
            wr32(rdram, kRimMaskFlag, v);
            dirtyOr(rdram, 0x2000u);
        }
    }

    inline void setClipMode(uint8_t *rdram, bool nearClip) noexcept
    {
        const uint32_t nearOff = (0x002B3EC8u - 0x002B3ED0u) & 0xFFFFFFFFu;
        (void)nearOff;
        constexpr uint32_t kNearVal = (0x002BC878u - 0x002BC130u) >> 3;
        constexpr uint32_t kFarVal = (0x002BC820u - 0x002BC130u) >> 3;
        const bool currentlyNear = (rd32(rdram, kRslClipProgOff) ^ kNearVal) < 1u;
        if (currentlyNear != nearClip)
        {
            wr32(rdram, kRslClipProgOff, nearClip ? kNearVal : kFarVal);
            dirtyOr(rdram, 8u);
        }
    }

    inline void setAmbPacked(uint8_t *rdram, uint32_t rgba) noexcept
    {
        if (rgba != rd32(rdram, kAmbientColorPacked))
        {
            wr32(rdram, kAmbientColorPacked, rgba);
            dirtyOr(rdram, 8u);
            wr32(rdram, kRslGsTestRegs + 0x20u, rgba & 0xFFu);
            wr32(rdram, kRslGsTestRegs + 0x24u, (rgba >> 8) & 0xFFu);
            wr32(rdram, kRslGsTestRegs + 0x28u, (rgba >> 16) & 0xFFu);
            wr32(rdram, kRslGsTestRegs + 0x2Cu, rgba >> 24);
        }
    }

    inline void matSetAlpha(uint8_t *rdram, uint32_t obj, uint32_t a) noexcept
    {
        const uint32_t av = a & 0xFFu;
        if (rd32(rdram, obj + 0x20u) != av)
        {
            wr32(rdram, obj + 0x20u, av);
            wr32(rdram, obj + 0x24u, rd32(rdram, obj + 0x24u) | 1u);
            dirtyOr(rdram, 0x10u);
        }
    }

    inline void uvMtxSet(uint8_t *rdram, uint32_t m, float uScale, float uShear, float vScale, float vShear) noexcept
    {
        wrF32(rdram, m + 0x00u, uScale);
        wrF32(rdram, m + 0x28u, 1.0f);
        wrF32(rdram, m + 0x20u, uShear);
        wrF32(rdram, m + 0x14u, vScale);
        wrF32(rdram, m + 0x24u, vShear);
        wr32(rdram, m + 0x10u, 0u);
        wr32(rdram, m + 0x04u, 0u);
        wr32(rdram, m + 0x08u, 0u);
        wr32(rdram, m + 0x18u, 0u);
    }

    // -------------------------------------------------------------------------
    // Frustum planes & clip tests
    // -------------------------------------------------------------------------

    inline void invokeVu0Prog(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t vuProg) noexcept
    {
        const uint32_t targetByteAddr = (vuProg & 0x1FFu) << 3;
        ctx->vu0_cmsar0 = vuProg;
        ctx->vu0_pc = targetByteAddr;
        runtime->vu0StartMicroProgram(rdram, ctx, targetByteAddr);
    }

    inline bool buildClipPlanesImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) noexcept
    {
        const uint32_t cam = rd32(rdram, kCamProgStructPtr);
        if (!validRam(cam, 0x10u))
            return false;

        if (rd32(rdram, kCamDirtyFlags) & 1u)
        {
            float f21 = rdF32(rdram, kRslViewRect + 0u);
            float f20 = rdF32(rdram, kRslViewRect + 4u);
            float f22 = rdF32(rdram, kViewportDims + 4u);
            const float f0 = rdF32(rdram, kViewportDims + 0u);
            const float f1a = FPU_NEG_S(f21);
            f20 = FPU_DIV_S(f21, f20);
            f21 = FPU_DIV_S(f21, f0);
            f22 = FPU_DIV_S(f20, f22);

            const uint32_t tmpAddr = (GPR_U32(ctx, 29) - 0x20u) & ~0xFu;
            wrF32(rdram, tmpAddr + 0u, 1.0f);
            wrF32(rdram, tmpAddr + 4u, f1a);
            wr32(rdram, tmpAddr + 8u, 0u);
            wr32(rdram, tmpAddr + 12u, 0u);
            if (!runVuNorm3(rdram, tmpAddr))
                return false;

            float t0 = rdF32(rdram, tmpAddr + 0u);
            float t1 = rdF32(rdram, tmpAddr + 4u);
            wrF32(rdram, kFrustumPlanes + 0x88u, FPU_NEG_S(t0));
            wrF32(rdram, kFrustumPlanes + 0x8Cu, t0);
            wrF32(rdram, kFrustumPlanes + 0xA8u, t1);
            wrF32(rdram, kFrustumPlanes + 0xACu, t1);
            wr32(rdram, kFrustumPlanes + 0x98u, 0u);
            wr32(rdram, kFrustumPlanes + 0x9Cu, 0u);
            wr32(rdram, kFrustumPlanes + 0xB8u, 0u);
            wr32(rdram, kFrustumPlanes + 0xBCu, 0u);

            f20 = FPU_NEG_S(f20);
            wrF32(rdram, tmpAddr + 0u, 1.0f);
            wrF32(rdram, tmpAddr + 4u, f20);
            if (!runVuNorm3(rdram, tmpAddr))
                return false;

            const float f4b = rdF32(rdram, tmpAddr + 0u);
            const float f3b = rdF32(rdram, tmpAddr + 4u);
            const float farClip = rdF32(rdram, kRslViewRect + 0x0Cu);
            const float nearClip = rdF32(rdram, kRslViewRect + 0x08u);
            wrF32(rdram, kFrustumPlanes + 0xA0u, f3b);
            wrF32(rdram, kFrustumPlanes + 0xA4u, f3b);
            wrF32(rdram, kFrustumPlanes + 0x90u, FPU_NEG_S(f4b));
            wrF32(rdram, kFrustumPlanes + 0x94u, f4b);
            wrF32(rdram, kFrustumPlanes + 0x7Cu, FPU_NEG_S(farClip));
            wrF32(rdram, kFrustumPlanes + 0x78u, nearClip);
            wr32(rdram, kFrustumPlanes + 0x84u, 0u);
            wr32(rdram, kFrustumPlanes + 0xB4u, 0u);
            wr32(rdram, kFrustumPlanes + 0x80u, 0u);
            wr32(rdram, kFrustumPlanes + 0xB0u, 0u);
            wr32(rdram, kFrustumPlanes + 0x4Cu, 0u);
            wr32(rdram, kFrustumPlanes + 0x5Cu, 0u);
            wrF32(rdram, kFrustumPlanes + 0x6Cu, 1.0f);
            wr32(rdram, kFrustumPlanes + 0x48u, 0u);
            wr32(rdram, kFrustumPlanes + 0x58u, 0u);
            wrF32(rdram, kFrustumPlanes + 0x68u, -1.0f);

            f21 = FPU_NEG_S(f21);
            wrF32(rdram, tmpAddr + 0u, 1.0f);
            wrF32(rdram, tmpAddr + 4u, f21);
            if (!runVuNorm3(rdram, tmpAddr))
                return false;

            t0 = rdF32(rdram, tmpAddr + 0u);
            t1 = rdF32(rdram, tmpAddr + 4u);
            wrF32(rdram, kFrustumPlanes + 0x60u, t1);
            wrF32(rdram, kFrustumPlanes + 0x64u, t1);
            wrF32(rdram, kFrustumPlanes + 0x40u, FPU_NEG_S(t0));
            wrF32(rdram, kFrustumPlanes + 0x44u, t0);
            wr32(rdram, kFrustumPlanes + 0x54u, 0u);
            wr32(rdram, kFrustumPlanes + 0x74u, 0u);
            wr32(rdram, kFrustumPlanes + 0x50u, 0u);
            wr32(rdram, kFrustumPlanes + 0x70u, 0u);

            f22 = FPU_NEG_S(f22);
            wrF32(rdram, tmpAddr + 0u, 1.0f);
            wrF32(rdram, tmpAddr + 4u, f22);
            if (!runVuNorm3(rdram, tmpAddr))
                return false;

            const float f4d = rdF32(rdram, tmpAddr + 0u);
            const float f2d = rdF32(rdram, tmpAddr + 4u);
            wrF32(rdram, kFrustumPlanes + 0x24u, 1.0f);
            wrF32(rdram, kFrustumPlanes + 0x28u, f2d);
            wrF32(rdram, kFrustumPlanes + 0x18u, FPU_NEG_S(f4d));
            wrF32(rdram, kFrustumPlanes + 0x34u, FPU_NEG_S(farClip));
            wr32(rdram, kFrustumPlanes + 0x00u, 0u);
            wrF32(rdram, kFrustumPlanes + 0x20u, -1.0f);
            wrF32(rdram, kFrustumPlanes + 0x30u, nearClip);
            wr32(rdram, kFrustumPlanes + 0x0Cu, 0u);
            wrF32(rdram, kFrustumPlanes + 0x1Cu, f4d);
            wrF32(rdram, kFrustumPlanes + 0x2Cu, f2d);
            wr32(rdram, kFrustumPlanes + 0x3Cu, 0u);
            wr32(rdram, kFrustumPlanes + 0x08u, 0u);
            wr32(rdram, kFrustumPlanes + 0x38u, 0u);
            wr32(rdram, kFrustumPlanes + 0x04u, 0u);
            wr32(rdram, kFrustumPlanes + 0x14u, 0u);
            wr32(rdram, kFrustumPlanes + 0x10u, 0u);

            const uint32_t progBase = rd32(rdram, cam + 8u);
            const uint32_t offBase = rd32(rdram, cam + 0x0Cu);
            const uint32_t prog = ((0x002D2410u - offBase) >> 3) + progBase;
            for (uint32_t i = 0u; i < 12u; ++i)
                ctx->vu0_vf[1u + i] = _mm_castsi128_ps(READ128((kFrustumPlanes + (i << 4)) & ~0xFu));
            invokeVu0Prog(rdram, ctx, runtime, prog);
            camDirtyOr(rdram, 6u);
        }

        auto mul4 = [&](uint32_t immAddr, uint32_t srcMat) {
            const uint32_t progBase = rd32(rdram, cam + 8u);
            const uint32_t offBase = rd32(rdram, cam + 0x0Cu);
            const uint32_t prog = ((immAddr - offBase) >> 3) + progBase;
            for (uint32_t i = 0u; i < 4u; ++i)
                ctx->vu0_vf[1u + i] = _mm_castsi128_ps(READ128((srcMat + (i << 4)) & ~0xFu));
            invokeVu0Prog(rdram, ctx, runtime, prog);
        };

        if (rd32(rdram, kCamDirtyFlags) & 2u)
        {
            mul4(0x002D2478u, kViewMatrix);
            camDirtyOr(rdram, 4u);
        }
        if (rd32(rdram, kCamDirtyFlags) & 4u)
            mul4(0x002D2498u, kModelMatrix);

        wr32(rdram, kCamDirtyFlags, 0u);
        return true;
    }

    inline bool frustumTestImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                uint32_t geomAddr, uint32_t localFlag, bool isBox, int32_t &outRes) noexcept
    {
        const uint32_t cam = rd32(rdram, kCamProgStructPtr);
        if (!validRam(cam, 0x10u) || !validRam(geomAddr, isBox ? 0x20u : 0x10u))
            return false;

        ctx->vi[15] = 0xDCu;
        if (rd32(rdram, kCamDirtyFlags) != 0u)
        {
            if (!buildClipPlanesImpl(rdram, ctx, runtime))
                return false;
        }

        const uint32_t imm = isBox ? (localFlag ? 0x002D22D8u : 0x002D22C8u)
                                   : (localFlag ? 0x002D21F0u : 0x002D21E0u);
        const uint32_t progBase = rd32(rdram, cam + 8u);
        const uint32_t offBase = rd32(rdram, cam + 0x0Cu);
        const uint32_t prog = ((imm - offBase) >> 3) + progBase;

        uint32_t vi1 = 0u, vi2 = 0u;
        if (isBox)
        {
            if (g_hooks[kH_VuBoxClip].outer != g_hooks[kH_VuBoxClip].hook)
            {
                const uint32_t spAddr = (GPR_U32(ctx, 29) - 0x20u) & ~0xFu;
                SET_GPR_U64(ctx, 4, static_cast<uint64_t>(static_cast<int32_t>(prog)));
                SET_GPR_U64(ctx, 5, static_cast<uint64_t>(static_cast<int32_t>(geomAddr)));
                SET_GPR_U64(ctx, 6, static_cast<uint64_t>(static_cast<int32_t>(spAddr)));
                SET_GPR_U64(ctx, 7, static_cast<uint64_t>(static_cast<int32_t>(spAddr + 4u)));
                if (!callWrapped(rdram, ctx, runtime, g_hooks[kH_VuBoxClip].outer, 0x0030C8D4u, 0x002F75A8u, 0x30u))
                    return false;
                vi1 = rd32(rdram, spAddr);
                vi2 = rd32(rdram, spAddr + 4u);
            }
            else
            {
                ctx->vu0_vf[1] = _mm_castsi128_ps(READ128((geomAddr + 0u) & ~0xFu));
                ctx->vu0_vf[2] = _mm_castsi128_ps(READ128((geomAddr + 16u) & ~0xFu));
                invokeVu0Prog(rdram, ctx, runtime, prog);
                vi1 = static_cast<uint32_t>(ctx->vi[1]) >> 4;
                vi2 = static_cast<uint32_t>(ctx->vi[2]) >> 4;
            }
        }
        else
        {
            ctx->vu0_vf[1] = _mm_castsi128_ps(READ128(geomAddr & ~0xFu));
            invokeVu0Prog(rdram, ctx, runtime, prog);
            vi1 = static_cast<uint32_t>(ctx->vi[1]) >> 4;
            vi2 = static_cast<uint32_t>(ctx->vi[2]) >> 4;
        }

        const uint32_t hi = vi2 & 0xFFFu;
        const uint32_t lo = vi1 & 0xFFFu;
        if ((hi & 0x3Fu) == 0x3Fu)
            outRes = 1;
        else if (isBox ? ((lo & 0x3Fu) != 0x3Fu) : ((lo & 0x3Fu) != 0u))
            outRes = 0;
        else if ((hi & 0xFC0u) == 0xFC0u)
            outRes = 1;
        else
            outRes = 2;
        wr32(rdram, kFrustumResult, static_cast<uint32_t>(outRes));
        return true;
    }

    // -------------------------------------------------------------------------
    // Texture / CLUT / UV / Draw packet emitters (calling outer vcs::rslworld when wrapped)
    // -------------------------------------------------------------------------

    inline void callGsSendImpl(uint8_t *rdram) noexcept
    {
        uint32_t dirty = rd32(rdram, kDirtyFlags);
        auto appendReg = [&](uint32_t regValAddr, uint64_t regAddr) {
            const uint32_t cnt = rd32(rdram, kGsQueueCount);
            const uint32_t dst = kGsQueueBase + (cnt << 4);
            wr64(rdram, dst + 0u, rd64(rdram, regValAddr));
            wr64(rdram, dst + 8u, regAddr);
            wr32(rdram, kGsQueueCount, cnt + 1u);
        };
        if (dirty & 0x80u) appendReg(kGsTex0State + 0u, 6ULL);
        if (dirty & 0x100u) appendReg(kGsTex0State + 8u, 7ULL);
        if (dirty & 0x20u) appendReg(kGsTex1State + 0u, 0x14ULL);
        if (dirty & 0x40u) appendReg(kGsTex1State + 8u, 0x15ULL);
        dirty &= 0xFFFFFE1Fu;
        wr32(rdram, kDirtyFlags, dirty);

        const uint32_t count = rd32(rdram, kGsQueueCount);
        if (count == 0u)
            return;
        const uint32_t qwc = count + 2u;
        const uint32_t bytes = qwc << 4;
        const uint32_t cur = rd32(rdram, kPktBufCursor);
        uint32_t p = 0u;
        if (pktAvail(rdram, kPktBufBase) >= bytes)
        {
            p = cur;
            wr32(rdram, kPktBufCursor, cur + bytes);
        }
        const uint32_t payloadQwc = count + 1u;
        wr64(rdram, p + 0u, static_cast<uint64_t>(0x10000000u | payloadQwc));
        wr32(rdram, p + 8u, 0x10000000u);
        wr32(rdram, p + 12u, 0x51000000u | payloadQwc);
        wr64(rdram, p + 16u, 0x1000000000008000ULL | static_cast<uint64_t>(count));
        wr64(rdram, p + 24u, 0xEULL);
        std::memcpy(rdram + ((p + 32u) & kRamMask), rdram + kGsQueueBase, count << 4);
        wr32(rdram, kGsQueueCount, 0u);
    }

    inline void flushGsQueueOnly(uint8_t *rdram) noexcept
    {
        const uint32_t count = rd32(rdram, kGsQueueCount);
        if (count == 0u)
            return;
        const uint32_t qwc = count + 2u;
        const uint32_t bytes = qwc << 4;
        const uint32_t cur = rd32(rdram, kPktBufCursor);
        uint32_t p = 0u;
        if (pktAvail(rdram, kPktBufBase) >= bytes)
        {
            p = cur;
            wr32(rdram, kPktBufCursor, cur + bytes);
        }
        const uint32_t payloadQwc = count + 1u;
        wr64(rdram, p + 0u, static_cast<uint64_t>(0x10000000u | payloadQwc));
        wr32(rdram, p + 8u, 0x10000000u);
        wr32(rdram, p + 12u, 0x51000000u | payloadQwc);
        wr64(rdram, p + 16u, 0x1000000000008000ULL | static_cast<uint64_t>(count));
        wr64(rdram, p + 24u, 0xEULL);
        std::memcpy(rdram + ((p + 32u) & kRamMask), rdram + kGsQueueBase, count << 4);
        wr32(rdram, kGsQueueCount, 0u);
    }

    inline bool drawPacketImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                               uint32_t a0, uint32_t a1, int kind, uint32_t retFromFlush) noexcept
    {
        const uint32_t curCheck = rd32(rdram, kPktBufCursor);
        if (!validRam(curCheck, 64u))
            return false;

        if (!callWrapped(rdram, ctx, runtime, g_outerFlush, 0x002F8710u, retFromFlush, 0x40u))
            return false;

        flushGsQueueOnly(rdram);

        if (kind == 1) // 0x002F80A0 ModelB
        {
            const int32_t dirtyS = rdS32(rdram, kDirtyFlags);
            if (dirtyS < 0)
            {
                const uint32_t f830c = rd32(rdram, kCallFlags830C);
                const uint32_t c = rd32(rdram, kPktBufCursor);
                wr64(rdram, c + 0u, (static_cast<uint64_t>(f830c) << 32) | 0x50000000ULL);
                wr64(rdram, c + 8u, 0ULL);
                wr32(rdram, kPktBufCursor, c + 16u);
                wr32(rdram, kDirtyFlags, static_cast<uint32_t>(dirtyS) & 0x7FFFFFFFu);
            }
        }

        const uint32_t c = rd32(rdram, kPktBufCursor);
        if (kind == 2) // 0x002F7EB0 World REF
        {
            const uint32_t qwc = (a1 << 4) >> 4;
            wr64(rdram, c + 0u, (static_cast<uint64_t>(a0) << 32) | 0x30000000ULL | static_cast<uint64_t>(qwc));
        }
        else // Model CALL
        {
            wr64(rdram, c + 0u, (static_cast<uint64_t>(a0) << 32) | 0x50000000ULL);
        }
        wr64(rdram, c + 8u, 0ULL);
        wr32(rdram, kPktBufCursor, c + 16u);
        return true;
    }

    inline void sendTexMatrixImpl(uint8_t *rdram, uint32_t m) noexcept
    {
        const uint32_t cur = rd32(rdram, kPktBufCursor);
        const uint32_t regOff = (0x00494A70u - 0x004947B0u) >> 4;
        wr64(rdram, cur + 0u, 0x10000003ULL);
        wr64(rdram, cur + 8u, (static_cast<uint64_t>(0x6C030000u | regOff) << 32) | 0x10000000ULL);
        const uint32_t dst0 = cur + 16u;
        wr32(rdram, kPktBufCursor, dst0);
        uint32_t writeDst = 0u;
        if (pktAvail(rdram, kPktBufBase) >= 0x30u)
        {
            writeDst = dst0;
            wr32(rdram, kPktBufCursor, dst0 + 0x30u);
        }
        std::memcpy(rdram + (writeDst & kRamMask), rdram + (m & kRamMask), 0x30u);
    }

    // Direct 64-bit translation of FUN_00266608 (checked line-by-line against p21_mips.txt:1646-1722).
    inline void emitTexUploadExact(uint8_t *rdram, uint32_t dst, uint32_t psm, uint32_t wLog2, uint32_t hLog2,
                                   uint32_t bytes, uint32_t srcPtr, uint32_t destVram) noexcept
    {
        const uint32_t qwc = bytes >> 4;
        const uint32_t bwShift = (5u < wLog2) ? (wLog2 - 6u) : 0u;
        const uint32_t dbw = 1u << bwShift;
        const uint32_t rrw = 1u << wLog2;
        const uint32_t rrh = 1u << hLog2;
        const uint32_t dbp = destVram >> 6;

        const uint64_t bitblt = (static_cast<uint64_t>(psm) << 56) | (static_cast<uint64_t>(dbw) << 48) |
                                (static_cast<uint64_t>(dbp) << 32);
        const uint64_t trxreg = (static_cast<uint64_t>(rrh) << 32) | static_cast<uint64_t>(rrw);
        const uint64_t refA = 0x0000800000000000ULL | static_cast<uint64_t>(qwc);
        const uint64_t refB = (static_cast<uint64_t>(srcPtr) << 32) | 0x30000000ULL | static_cast<uint64_t>(qwc);

        wr64(rdram, dst + 0x00u, 0x10000006ULL);
        wr32(rdram, dst + 0x08u, 0x10000000u);
        wr32(rdram, dst + 0x0Cu, 0x51000006u);
        wr64(rdram, dst + 0x10u, 0x1000000000008004ULL);
        wr64(rdram, dst + 0x18u, 0xEULL);
        wr64(rdram, dst + 0x20u, bitblt);
        wr64(rdram, dst + 0x28u, 0x50ULL);
        wr64(rdram, dst + 0x30u, 0ULL);
        wr64(rdram, dst + 0x38u, 0x51ULL);
        wr64(rdram, dst + 0x40u, trxreg);
        wr64(rdram, dst + 0x48u, 0x52ULL);
        wr64(rdram, dst + 0x50u, 0ULL);
        wr64(rdram, dst + 0x58u, 0x53ULL);
        wr64(rdram, dst + 0x60u, refA);
        wr64(rdram, dst + 0x68u, 0ULL);
        wr64(rdram, dst + 0x70u, refB);
        wr32(rdram, dst + 0x78u, 0u);
        wr32(rdram, dst + 0x7Cu, 0x51000000u | qwc);
    }

    inline bool setTextureImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                               uint32_t unit, uint32_t psm, uint32_t wLog2, uint32_t hLog2,
                               uint32_t numMips, uint32_t tcc, uint32_t mipMax, uint32_t dataPtr, uint32_t mipFlags) noexcept
    {
        if (unit > 1u || psm >= 0x2Du)
            return false;
        const uint32_t jtTarget = rd32(rdram, kPsmJumpTable + (psm << 2));
        uint32_t bpp = 0u;
        switch (jtTarget)
        {
        case 0x2F6420u: bpp = 32u; break;
        case 0x2F642Cu: bpp = 24u; break;
        case 0x2F6438u: bpp = 16u; break;
        case 0x2F6444u: bpp = 8u; break;
        case 0x2F6450u: bpp = 4u; break;
        default: return false;
        }
        if (numMips == 0u || numMips > 7u)
            return false;
        const uint32_t cacheKeyAddr = kRslTexCacheKeys + (unit << 3);
        const uint64_t newKey = static_cast<uint64_t>(dataPtr) |
                                (static_cast<uint64_t>(wLog2 & 0xFFu) << 32) |
                                (static_cast<uint64_t>(hLog2 & 0xFFu) << 40) |
                                (static_cast<uint64_t>(numMips & 0xFFu) << 48) |
                                (static_cast<uint64_t>(psm & 0xFFu) << 56) |
                                (tcc != 0u ? 0x80000000ULL : 0ULL);
        if (newKey == rd64(rdram, cacheKeyAddr))
            return true;

        const uint32_t useTree = rd32(rdram, kUseTreeUpload);
        const uint32_t needAlloc = useTree ? ((numMips << 5) & 0xFFFFFFF0u) : ((numMips << 7) & 0xFFFFFFF0u);
        if (dataPtr != 0u && pktAvail(rdram, kPktBufBase) < needAlloc + 0x50u)
            return false;

        wr64(rdram, cacheKeyAddr, newKey);
        if (dataPtr == 0u)
            return true;

        uint32_t curTreeBuf = 0u;
        uint32_t curFlatBuf = 0u;
        if (useTree == 0u)
        {
            const uint32_t cur = rd32(rdram, kPktBufCursor);
            curFlatBuf = cur;
            wr32(rdram, kPktBufCursor, cur + needAlloc);
        }
        else
        {
            const uint32_t cur = rd32(rdram, kPktBufCursor);
            curTreeBuf = cur;
            wr32(rdram, kPktBufCursor, cur + needAlloc);
        }

        uint32_t dbpArr[8]{};
        uint32_t tbwArr[8]{};
        uint32_t vramBase = rd32(rdram, kRslTexVramBase + (unit << 3));
        uint32_t srcCur = dataPtr;
        uint32_t w = wLog2;
        uint32_t h = hLog2;
        uint32_t mFlags = mipFlags;

        for (uint32_t i = 0u; i < numMips; ++i)
        {
            dbpArr[i] = vramBase;
            const uint32_t sizeBytes = (bpp << (w + h)) >> 3;
            uint32_t argPsm = psm, argW = w, argH = h;
            const uint32_t nextW = w - 1u;
            const uint32_t nextH = h - 1u;
            if (mFlags & 1u)
            {
                if (psm == 0x13u)
                {
                    argPsm = 0u;
                    argW = nextW;
                    argH = nextH;
                }
                else if (psm == 0x14u)
                {
                    argPsm = 2u;
                    argW = w - 1u;
                    argH = h - 1u;
                }
            }
            mFlags >>= 1;
            if (useTree == 0u)
            {
                emitTexUploadExact(rdram, curFlatBuf, argPsm, argW, argH, sizeBytes, srcCur, vramBase);
            }
            else
            {
                SET_GPR_U64(ctx, 4, static_cast<uint64_t>(static_cast<int32_t>(curTreeBuf)));
                SET_GPR_U64(ctx, 5, static_cast<uint64_t>(static_cast<int32_t>(argPsm)));
                SET_GPR_U64(ctx, 6, static_cast<uint64_t>(static_cast<int32_t>(argW)));
                SET_GPR_U64(ctx, 7, static_cast<uint64_t>(static_cast<int32_t>(argH)));
                SET_GPR_U64(ctx, 8, static_cast<uint64_t>(static_cast<int32_t>(sizeBytes)));
                SET_GPR_U64(ctx, 9, static_cast<uint64_t>(static_cast<int32_t>(srcCur)));
                SET_GPR_U64(ctx, 10, static_cast<uint64_t>(static_cast<int32_t>(vramBase)));
                if (!callWrapped(rdram, ctx, runtime, g_outerUpload, 0x003298D8u, 0x002F6614u, 0x190u))
                    return false;
            }
            tbwArr[i] = (w < 6u) ? 0u : (w - 6u);
            vramBase = (vramBase + (sizeBytes >> 2) + 0x3Fu) & ~0x3Fu;
            srcCur += sizeBytes;
            if (w != 0u) w = nextW;
            if (h != 0u) h = nextH;
            curTreeBuf += 0x20u;
            curFlatBuf += 0x80u;
        }

        const uint32_t p = rd32(rdram, kPktBufCursor);
        wr32(rdram, kPktBufCursor, p + 16u);
        wr64(rdram, p + 0u, 0x10000005ULL);
        wr64(rdram, p + 8u, 0x5100000500000000ULL);
        const uint32_t d = p + 16u;
        wr32(rdram, kPktBufCursor, d + 0x50u);

        wr64(rdram, d + 0x00u, 0x1000000000008004ULL);
        wr64(rdram, d + 0x08u, 0xEULL);
        wr64(rdram, d + 0x10u, static_cast<uint64_t>(mipMax & 0xFFu) << 32);
        wr64(rdram, d + 0x18u, 0x3BULL);

        const uint64_t miptbp1 =
            (static_cast<uint64_t>(1u << tbwArr[3]) << 54) |
            (static_cast<uint64_t>(dbpArr[3] >> 6) << 40) |
            (static_cast<uint64_t>(1u << tbwArr[2]) << 34) |
            (static_cast<uint64_t>(dbpArr[2] >> 6) << 20) |
            (static_cast<uint64_t>(1u << tbwArr[1]) << 14) |
            static_cast<uint64_t>(dbpArr[1] >> 6);
        wr64(rdram, d + 0x20u, miptbp1);
        wr64(rdram, d + 0x28u, static_cast<uint64_t>(0x34u + unit));

        const uint64_t miptbp2 =
            (static_cast<uint64_t>(1u << tbwArr[6]) << 54) |
            (static_cast<uint64_t>(dbpArr[6] >> 6) << 40) |
            (static_cast<uint64_t>(1u << tbwArr[5]) << 34) |
            (static_cast<uint64_t>(dbpArr[5] >> 6) << 20) |
            (static_cast<uint64_t>(1u << tbwArr[4]) << 14) |
            static_cast<uint64_t>(dbpArr[4] >> 6);
        wr64(rdram, d + 0x30u, miptbp2);
        wr64(rdram, d + 0x38u, static_cast<uint64_t>(0x36u + unit));
        wr64(rdram, d + 0x40u, 0x42ULL);
        wr64(rdram, d + 0x48u, 0x3FULL);

        const uint32_t tbp0 = rd32(rdram, kRslTexVramBase + (unit << 3));
        const uint64_t tex0 =
            (static_cast<uint64_t>(tbp0 >> 6)) |
            (static_cast<uint64_t>(1u << tbwArr[0]) << 14) |
            (static_cast<uint64_t>(psm) << 20) |
            (static_cast<uint64_t>(wLog2) << 26) |
            (static_cast<uint64_t>(hLog2) << 30) |
            (tcc != 0u ? 0x0000000400000000ULL : 0ULL);

        const uint32_t tex0Slot = kGsTex0State + (unit << 3);
        const uint64_t prevTex0 = rd64(rdram, tex0Slot);
        wr64(rdram, tex0Slot, (prevTex0 & 0xFFFFFFF800000000ULL) | tex0);
        dirtyOr(rdram, rd32(rdram, kDirtyMaskTable08 + (unit << 2)));
        setTexMipBits(rdram, unit, numMips);
        return true;
    }

    inline bool setClutImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                            uint32_t unit, uint32_t clutPtr, uint32_t psm, uint32_t numColors, uint32_t uploadFlag) noexcept
    {
        if (unit > 1u || psm == 1u)
            return false;
        const uint32_t cacheAddr = kClutCachePtrs + (unit << 2);
        if (clutPtr == rd32(rdram, cacheAddr))
            return true;
        if (clutPtr != 0u && pktAvail(rdram, kPktBufBase) < 0xA0u)
            return false;

        wr32(rdram, cacheAddr, clutPtr);
        if (clutPtr == 0u)
            return true;

        const uint32_t useTree = rd32(rdram, kUseTreeUpload);
        const uint32_t bpp = (psm == 2u || psm == 10u) ? 0x10u : 0x20u;
        const uint32_t argW = (numColors == 0x100u) ? 4u : 3u;
        const uint32_t argH = (numColors == 0x100u) ? 4u : 1u;
        const uint32_t argBytes = (numColors == 0x100u) ? (bpp << 5) : (bpp << 1);
        const uint32_t clutVram = rd32(rdram, kRslClutVramBase + (unit << 3));

        if (useTree == 0u)
        {
            const uint32_t dst = rd32(rdram, kPktBufCursor);
            wr32(rdram, kPktBufCursor, dst + 0x80u);
            emitTexUploadExact(rdram, dst, psm, argW, argH, argBytes, clutPtr, clutVram);
        }
        else
        {
            const uint32_t cur = rd32(rdram, kPktBufCursor);
            wr32(rdram, kPktBufCursor, cur + 0x20u);
            SET_GPR_U64(ctx, 4, static_cast<uint64_t>(static_cast<int32_t>(cur)));
            SET_GPR_U64(ctx, 5, static_cast<uint64_t>(static_cast<int32_t>(psm)));
            SET_GPR_U64(ctx, 6, static_cast<uint64_t>(static_cast<int32_t>(argW)));
            SET_GPR_U64(ctx, 7, static_cast<uint64_t>(static_cast<int32_t>(argH)));
            SET_GPR_U64(ctx, 8, static_cast<uint64_t>(static_cast<int32_t>(argBytes)));
            SET_GPR_U64(ctx, 9, static_cast<uint64_t>(static_cast<int32_t>(clutPtr)));
            SET_GPR_U64(ctx, 10, static_cast<uint64_t>(static_cast<int32_t>(clutVram)));
            const uint32_t raTarget = (numColors == 0x100u) ? 0x002F6C60u : 0x002F6C94u;
            if (!callWrapped(rdram, ctx, runtime, g_outerUpload, 0x003298D8u, raTarget, 0x70u))
                return false;
        }

        if (uploadFlag != 0u)
        {
            const uint32_t p = rd32(rdram, kPktBufCursor);
            wr32(rdram, kPktBufCursor, p + 16u);
            wr64(rdram, p + 0u, 0x10000002ULL);
            wr64(rdram, p + 8u, 0x5100000200000000ULL);
            const uint32_t d = rd32(rdram, kPktBufCursor);
            wr32(rdram, kPktBufCursor, d + 0x20u);
            wr64(rdram, d + 0x00u, 0x1000000000008001ULL);
            wr64(rdram, d + 0x08u, 0xEULL);
            wr64(rdram, d + 0x10u, 0x42ULL);
            wr64(rdram, d + 0x18u, 0x3FULL);
        }

        const uint32_t tex0Slot = kGsTex0State + (unit << 3);
        const uint64_t prev = rd64(rdram, tex0Slot);
        const uint64_t cbpBits = static_cast<uint64_t>(clutVram >> 6) << 37;
        const uint64_t next = (prev & 0x0000001FFFFFFFFFULL) | cbpBits | (static_cast<uint64_t>(psm) << 51) | 0x2000000000000000ULL;
        wr64(rdram, tex0Slot, next);
        dirtyOr(rdram, rd32(rdram, kDirtyMaskTable08 + (unit << 2)));
        return true;
    }

    inline bool texApplyImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                             uint32_t stateBase, uint32_t raster, uint32_t unit, uint32_t clamp) noexcept
    {
        const uint32_t slot = stateBase + 0x1Cu + (unit << 2);
        if (rd32(rdram, slot) == raster)
            return true;
        wr32(rdram, slot, raster);
        if (raster == 0u)
            return true;

        const uint32_t flags = rd32(rdram, raster + 0x0Cu);
        const uint32_t wLog2 = flags & 0x3Fu;
        const uint32_t hLog2 = (flags >> 6) & 0x3Fu;
        if (wLog2 < 3u || hLog2 < 3u)
            return true;

        const uint32_t numMips = (flags >> 20) & 0xFu;
        const uint32_t fmtBits = flags & 0x3F000u;
        uint32_t curPtr = rd32(rdram, raster + 8u);
        const uint32_t dataPtr = curPtr;

        uint32_t stepSize = (((flags >> 12) & 0x3Fu) << wLog2) >> 3;
        stepSize <<= hLog2;
        for (uint32_t i = 0u; i < numMips; ++i)
        {
            curPtr += stepSize;
            stepSize >>= 2;
        }

        const uint32_t texPsm = (fmtBits == 0x8000u) ? 0x13u : 0x14u;
        const uint32_t numColors = (fmtBits == 0x8000u) ? 0x100u : 0x10u;
        const uint32_t tcc = (clamp < 1u) ? 1u : 0u;

        if (g_hooks[kH_SetClut].outer != g_hooks[kH_SetClut].hook)
        {
            SET_GPR_U64(ctx, 4, static_cast<uint64_t>(static_cast<int32_t>(unit)));
            SET_GPR_U64(ctx, 5, static_cast<uint64_t>(static_cast<int32_t>(curPtr)));
            SET_GPR_U64(ctx, 6, 0ULL);
            SET_GPR_U64(ctx, 7, static_cast<uint64_t>(static_cast<int32_t>(numColors)));
            SET_GPR_U64(ctx, 8, 0ULL);
            if (!callWrapped(rdram, ctx, runtime, g_hooks[kH_SetClut].outer, 0x002F6A78u, 0x003180CCu, 0x60u))
                return false;
        }
        else if (!setClutImpl(rdram, ctx, runtime, unit, curPtr, 0u, numColors, 0u))
            return false;

        const uint32_t mipFlags = rd8(rdram, raster + 0x0Fu);
        if (g_hooks[kH_SetTexture].outer != g_hooks[kH_SetTexture].hook)
        {
            const uint32_t newSp = (GPR_U32(ctx, 29) - 0x60u) & ~0xFu;
            const uint32_t savedSp = GPR_U32(ctx, 29);
            const uint32_t savedRa = GPR_U32(ctx, 31);
            wr32(rdram, newSp, mipFlags);
            SET_GPR_U64(ctx, 29, static_cast<uint64_t>(static_cast<int32_t>(newSp)));
            SET_GPR_U64(ctx, 31, static_cast<uint64_t>(static_cast<int32_t>(0x003180F8u)));
            SET_GPR_U64(ctx, 4, static_cast<uint64_t>(static_cast<int32_t>(unit)));
            SET_GPR_U64(ctx, 5, static_cast<uint64_t>(static_cast<int32_t>(texPsm)));
            SET_GPR_U64(ctx, 6, static_cast<uint64_t>(static_cast<int32_t>(wLog2)));
            SET_GPR_U64(ctx, 7, static_cast<uint64_t>(static_cast<int32_t>(hLog2)));
            SET_GPR_U64(ctx, 8, static_cast<uint64_t>(static_cast<int32_t>(numMips)));
            SET_GPR_U64(ctx, 9, static_cast<uint64_t>(static_cast<int32_t>(tcc)));
            SET_GPR_U64(ctx, 10, 0xFFULL);
            SET_GPR_U64(ctx, 11, static_cast<uint64_t>(static_cast<int32_t>(dataPtr)));
            ctx->pc = 0x002F62B8u;
            g_hooks[kH_SetTexture].outer(rdram, ctx, runtime);
            const uint32_t postPc = ctx->pc;
            SET_GPR_U64(ctx, 29, static_cast<uint64_t>(static_cast<int32_t>(savedSp)));
            SET_GPR_U64(ctx, 31, static_cast<uint64_t>(static_cast<int32_t>(savedRa)));
            return !runtime->isStopRequested() && !g_checkpointUnwind && postPc == 0x003180F8u;
        }
        return setTextureImpl(rdram, ctx, runtime, unit, texPsm, wLog2, hLog2, numMips, tcc, 0xFFu, dataPtr, mipFlags);
    }

    inline bool texBindImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t raster) noexcept
    {
        if (raster != 0u && ((raster - 0x00100001u) > 0x07EFFFFFu || (raster & 3u) != 0u))
            return true;
        return texApplyImpl(rdram, ctx, runtime, kEnvStateBase, raster, 0u, 0u);
    }

    inline void envMapMtxImpl(uint8_t *rdram, uint32_t envObj, uint32_t dataPtr) noexcept
    {
        const uint32_t prevPtr = rd32(rdram, envObj + 0x10u);
        const float rslRef = rdF32(rdram, kRslAlphaRef);
        float curScale = rdF32(rdram, envObj + 0x14u);
        if (prevPtr == dataPtr && FPU_C_EQ_S(rslRef, curScale))
            return;
        wr32(rdram, envObj + 0x10u, dataPtr);
        curScale = rdF32(rdram, envObj + 0x14u);
        uint32_t mipOff;
        if (FPU_C_EQ_S(rslRef, curScale))
        {
            mipOff = rd32(rdram, envObj + 0x18u);
        }
        else
        {
            wrF32(rdram, envObj + 0x14u, rslRef);
            const float v = evalLogPoly(rdram, rslRef);
            const int32_t iv = FPU_CVT_W_S(FPU_MUL_S(v, 16.0f));
            wr32(rdram, envObj + 0x18u, static_cast<uint32_t>(iv));
            mipOff = static_cast<uint32_t>(iv);
        }
        setTex1Fields(rdram, 0u, 0u, dataPtr + mipOff, 0u);
    }

    inline bool uvSetImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t envObj, float uScale, float vScale) noexcept
    {
        if (FPU_C_EQ_S(uScale, rdF32(rdram, envObj)) && FPU_C_EQ_S(vScale, rdF32(rdram, envObj + 4u)))
            return true;
        if (rd32(rdram, kUvMtxInitFlag) == 0u)
            return false;
        wrF32(rdram, envObj + 0u, uScale);
        wrF32(rdram, envObj + 4u, vScale);
        wr32(rdram, kUvMtxInitFlag, 1u);
        uvMtxSet(rdram, kUvMtxBase, uScale, 0.0f, vScale, 0.0f);
        if (g_hooks[kH_SendTexMtx].outer != g_hooks[kH_SendTexMtx].hook)
        {
            SET_GPR_U64(ctx, 4, static_cast<uint64_t>(static_cast<int32_t>(kUvMtxBase)));
            return callWrapped(rdram, ctx, runtime, g_hooks[kH_SendTexMtx].outer, 0x002F7090u, 0x00317DE4u, 0x30u);
        }
        sendTexMatrixImpl(rdram, kUvMtxBase);
        return true;
    }

    inline bool invokeUvSetHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                uint32_t envObj, float uScale, float vScale, uint32_t retAddr) noexcept
    {
        if (g_hooks[kH_UvSet].outer != g_hooks[kH_UvSet].hook)
        {
            SET_GPR_U64(ctx, 4, static_cast<uint64_t>(static_cast<int32_t>(envObj)));
            ctx->f[12] = uScale;
            ctx->f[13] = vScale;
            return callWrapped(rdram, ctx, runtime, g_hooks[kH_UvSet].outer, 0x00317D48u, retAddr, 0x30u);
        }
        return uvSetImpl(rdram, ctx, runtime, envObj, uScale, vScale);
    }

    // -------------------------------------------------------------------------
    // Shared strip submission pass (handles both FUN_0039EC18 and FUN_0039E828)
    // -------------------------------------------------------------------------

    inline bool invokeTexBindHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t raster, uint32_t retAddr) noexcept
    {
        if (g_hooks[kH_TexBind].outer != g_hooks[kH_TexBind].hook)
        {
            SET_GPR_U64(ctx, 4, static_cast<uint64_t>(static_cast<int32_t>(raster)));
            ctx->f[12] = 1.0f;
            return callWrapped(rdram, ctx, runtime, g_hooks[kH_TexBind].outer, 0x00408F30u, retAddr, 0x20u);
        }
        return texBindImpl(rdram, ctx, runtime, raster);
    }

    inline bool invokeDrawModelHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t pktAddr, uint32_t retAddr) noexcept
    {
        if (g_hooks[kH_DrawModel].outer != g_hooks[kH_DrawModel].hook)
        {
            SET_GPR_U64(ctx, 4, static_cast<uint64_t>(static_cast<int32_t>(pktAddr)));
            return callWrapped(rdram, ctx, runtime, g_hooks[kH_DrawModel].outer, 0x002F7D58u, retAddr, 0x40u);
        }
        return drawPacketImpl(rdram, ctx, runtime, pktAddr, 0u, 0, 0x002F7D84u);
    }

    inline uint32_t matGetType(const uint8_t *rdram, uint32_t mat) noexcept
    {
        const uint32_t p = rd32(rdram, mat + 0x0Cu);
        return (p != 0u) ? rd32(rdram, p + 0x0Cu) : 0u;
    }

    inline uint8_t computeAlphaByte(const uint8_t *rdram, uint32_t mat) noexcept
    {
        const float raw = rdF32(rdram, rd32(rdram, mat + 0x0Cu) + 8u);
        float a = FPU_MUL_S(raw, FPU_MUL_S(rdF32(rdram, kFadeAlpha), 0.5f));
        if (FPU_C_OLT_S(a, 0.0f))
            a = 0.0f;
        else
            a = FPU_C_OLT_S(a, 1.0f) ? a : 1.0f;
        const int32_t q1 = FPU_CVT_W_S(FPU_MUL_S(a, 255.0f));
        int32_t alpha = FPU_CVT_W_S(FPU_CVT_S_W(q1));
        if (alpha >= 0x100)
            alpha = 0xFF;
        if (rd32(rdram, kForceZeroAlpha) != 0u || rd32(rdram, kForceShadowPass) != 0u)
            alpha = 0;
        if (rd32(rdram, kForceFullAlpha) != 0u)
            alpha = 0xFF;
        return static_cast<uint8_t>(alpha & 0xFF);
    }

    inline void packAndSetAmbient(uint8_t *rdram, uint32_t mat) noexcept
    {
        const uint32_t rgba = rd32(rdram, mat + 4u);
        const uint32_t a = ((rgba >> 24) + 1u) >> 1;
        setAmbPacked(rdram, (rgba & 0x00FFFFFFu) | (a << 24));
    }

    inline bool submitStrip(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                            uint32_t stripAddr, uint32_t mat, uint32_t pktBase,
                            uint32_t envRetAddr, uint32_t drawRetAddr) noexcept
    {
        const uint32_t texPtr = rd32(rdram, mat);
        const uint32_t raster = (texPtr != 0u) ? rd32(rdram, texPtr) : 0u;
        packAndSetAmbient(rdram, mat);
        if (raster != 0u)
        {
            const uint32_t mipOff = (rd32(rdram, raster + 0x0Cu) & 0x3Fu) << 4;
            envMapMtxImpl(rdram, kEnvStateBase, rd32(rdram, stripAddr + 0x18u) + mipOff);
            const float uS = rdF32(rdram, stripAddr + 0x10u);
            const float vS = rdF32(rdram, stripAddr + 0x14u);
            if (!invokeUvSetHook(rdram, ctx, runtime, kEnvStateBase, FPU_ADD_S(uS, uS), FPU_ADD_S(vS, vS), envRetAddr - 0x10u))
                return false;
            if (!invokeTexBindHook(rdram, ctx, runtime, raster, envRetAddr))
                return false;
            setAlphaTestBit(rdram, 1u);
        }
        else
        {
            setAlphaTestBit(rdram, 0u);
        }
        return invokeDrawModelHook(rdram, ctx, runtime, pktBase + rd32(rdram, stripAddr + 0x1Cu), drawRetAddr);
    }

    // -------------------------------------------------------------------------
    // FUN_0039E828 & FUN_0039EC18 implementations
    // -------------------------------------------------------------------------

    inline bool stripRenderImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t atomic) noexcept
    {
        const uint32_t geom = rd32(rdram, atomic + 0x14u);
        const uint32_t ps = kRenderSysBase;
        if (!setupLights(rdram, ctx, runtime, atomic, 1u))
            return false;
        setPipelineMode(rdram, 0x47u);
        const uint32_t geomData = geom + 0x20u;
        const uint32_t pktBase = geomData + rdU16(rdram, geom + 0x3Au);
        setFrameCtx(rdram, rd32(rdram, ps + 0x0Cu));

        const uint32_t stripCount = rd32(rdram, geom + 0x30u) >> 20;
        const uint32_t matArray = rd32(rdram, geom + 0x0Cu);

        // Pass 1: opaque strips (!(matType & 2)); preserves FUN_0039E828's double draw when env != 0.
        for (uint32_t i = 0u; i < stripCount; ++i)
        {
            const uint32_t strip = geomData + 0x40u + i * 0x30u;
            const uint32_t mat = rd32(rdram, matArray + (static_cast<uint32_t>(rdU16(rdram, strip + 0x22u)) << 2));
            if (rd32(rdram, kForceShadowPass) != 0u || (matGetType(rdram, mat) & 2u) != 0u)
                continue;
            const uint32_t texPtr = rd32(rdram, mat);
            const uint32_t raster = (texPtr != 0u) ? rd32(rdram, texPtr) : 0u;
            if (!submitStrip(rdram, ctx, runtime, strip, mat, pktBase, 0x0039E974u, 0x0039E988u))
                return false;
            if (raster != 0u)
            {
                if (!invokeDrawModelHook(rdram, ctx, runtime, pktBase + rd32(rdram, strip + 0x1Cu), 0x0039E9A4u))
                    return false;
            }
        }

        // Pass 2: alpha strips (matType & 2).
        bool alphaInit = false;
        for (uint32_t i = 0u; i < stripCount; ++i)
        {
            const uint32_t strip = geomData + 0x40u + i * 0x30u;
            const uint32_t mat = rd32(rdram, matArray + (static_cast<uint32_t>(rdU16(rdram, strip + 0x22u)) << 2));
            if (rd32(rdram, kForceShadowPass) != 0u || (matGetType(rdram, mat) & 2u) == 0u)
                continue;
            if (!alphaInit)
            {
                const uint32_t rState = rd32(rdram, kRenderStatePtr);
                if (rd32(rdram, rState + 0x20u) == 0u)
                {
                    alphaInit = true;
                    setFrameCtx(rdram, rd32(rdram, ps + 0x18u));
                }
            }
            matSetAlpha(rdram, rd32(rdram, ps + 0x1Cu), computeAlphaByte(rdram, mat));
            if (!submitStrip(rdram, ctx, runtime, strip, mat, pktBase, 0x0039EB80u, 0x0039EBA4u))
                return false;
        }

        setAlphaTestBit(rdram, 1u);
        resetLights(rdram);
        return invokeUvSetHook(rdram, ctx, runtime, kEnvStateBase, 1.0f, 1.0f, 0x0039EBE4u);
    }

    inline bool atomicRenderImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t atomic) noexcept
    {
        if (rd32(rdram, kRenderStatePtr) == 0u || rd32(rdram, kRenderInitFlag) == 0u || rd32(rdram, kUvMtxInitFlag) == 0u)
            return false;
        if (!validRam(atomic, 0x34u))
            return false;

        const uint32_t geom = rd32(rdram, atomic + 0x14u);
        if (!validRam(geom, 0x60u))
            return false;
        const uint32_t geomData = geom + 0x20u;
        const uint32_t skin = rd32(rdram, geom + 0x18u);
        const uint32_t txdObj = atomicGetTxd(rdram, atomic);
        if (txdObj != 0u)
            txdSlotTouch(rdram, txdObj);

        const uint32_t ps = kRenderSysBase;
        uint32_t ltm = 0u;
        if (!frameGetLtmImpl(rdram, ctx, runtime, rd32(rdram, atomic + 4u), ltm, 0x0039ECECu, 0x0039ECECu))
            return false;

        float mtx[16]{};
        mtx[0] = rdF32(rdram, ltm + 0x00u); mtx[1] = rdF32(rdram, ltm + 0x04u); mtx[2] = rdF32(rdram, ltm + 0x08u);
        mtx[4] = rdF32(rdram, ltm + 0x10u); mtx[5] = rdF32(rdram, ltm + 0x14u); mtx[6] = rdF32(rdram, ltm + 0x18u);
        mtx[8] = rdF32(rdram, ltm + 0x20u); mtx[9] = rdF32(rdram, ltm + 0x24u); mtx[10] = rdF32(rdram, ltm + 0x28u);
        mtx[12] = rdF32(rdram, ltm + 0x30u); mtx[13] = rdF32(rdram, ltm + 0x34u); mtx[14] = rdF32(rdram, ltm + 0x38u);
        mtx[15] = 1.0f;

        if (rd32(rdram, atomic + 0x30u) != 0u && FPU_C_OLT_S(mtx[14], -180.0f) && FPU_C_OLT_S(0.0f, mtx[12]))
            mtx[14] = -200.0f;

        const float origX = rdF32(rdram, kDrawOrigin + 0u);
        const float origY = rdF32(rdram, kDrawOrigin + 4u);
        const float origZ = rdF32(rdram, kDrawOrigin + 8u);
        mtx[12] = FPU_ADD_S(mtx[12], origX);
        mtx[13] = FPU_ADD_S(mtx[13], origY);
        mtx[14] = FPU_ADD_S(mtx[14], origZ);
        setModelMatrix(rdram, mtx);

        int32_t vis = 0;
        if (skin == 0u)
        {
            if (!frustumTestImpl(rdram, ctx, runtime, geomData, 1u, false, vis))
                return false;
        }
        else
        {
            if ((rd32(rdram, geom + 0x30u) & 0xFFFFFu) == 0xE8B0u)
            {
                vis = 2;
            }
            else
            {
                const uint32_t sphTmp = (GPR_U32(ctx, 29) - 0x40u) & ~0xFu;
                wrF32(rdram, sphTmp + 0u, rdF32(rdram, geom + 0x20u));
                wrF32(rdram, sphTmp + 4u, rdF32(rdram, geom + 0x24u));
                wrF32(rdram, sphTmp + 8u, rdF32(rdram, geom + 0x28u));
                wrF32(rdram, sphTmp + 12u, FPU_MUL_S(rdF32(rdram, geom + 0x2Cu), 3.5f));
                if (!frustumTestImpl(rdram, ctx, runtime, sphTmp, 1u, false, vis))
                    return false;
            }
        }

        if (vis == 0)
            return true;

        setClipMode(rdram, vis == 2);
        const float qtx = rdF32(rdram, geom + 0x54u);
        const float qty = rdF32(rdram, geom + 0x58u);
        const float qtz = rdF32(rdram, geom + 0x5Cu);
        const float qsx = rdF32(rdram, geom + 0x48u);
        const float qsy = rdF32(rdram, geom + 0x4Cu);
        const float qsz = rdF32(rdram, geom + 0x50u);

        const float d0 = FPU_ADD_S(FPU_ADD_S(FPU_MUL_S(mtx[0], qtx), FPU_MUL_S(mtx[4], qty)), FPU_MUL_S(mtx[8], qtz));
        const float d1 = FPU_ADD_S(FPU_ADD_S(FPU_MUL_S(mtx[1], qtx), FPU_MUL_S(mtx[5], qty)), FPU_MUL_S(mtx[9], qtz));
        const float d2 = FPU_ADD_S(FPU_ADD_S(FPU_MUL_S(mtx[2], qtx), FPU_MUL_S(mtx[6], qty)), FPU_MUL_S(mtx[10], qtz));
        mtx[0] = FPU_MUL_S(mtx[0], qsx); mtx[1] = FPU_MUL_S(mtx[1], qsx); mtx[2] = FPU_MUL_S(mtx[2], qsx);
        mtx[4] = FPU_MUL_S(mtx[4], qsy); mtx[5] = FPU_MUL_S(mtx[5], qsy); mtx[6] = FPU_MUL_S(mtx[6], qsy);
        mtx[8] = FPU_MUL_S(mtx[8], qsz); mtx[9] = FPU_MUL_S(mtx[9], qsz); mtx[10] = FPU_MUL_S(mtx[10], qsz);
        mtx[12] = FPU_ADD_S(mtx[12], d0);
        mtx[13] = FPU_ADD_S(mtx[13], d1);
        mtx[14] = FPU_ADD_S(mtx[14], d2);
        setModelMatrix(rdram, mtx);

        uint32_t pipeline = rd32(rdram, geom + 0x34u);
        uint32_t frameCtx = 0u;
        if (pipeline == 0x17u)
            frameCtx = rd32(rdram, ps + 0x24u);
        else if (pipeline == 7u || pipeline == 0x27u)
        {
            pipeline = 7u;
            frameCtx = rd32(rdram, ps + 0x0Cu);
        }
        else if (pipeline == 0x0Du)
            frameCtx = rd32(rdram, ps + 0x00u);
        else if (pipeline == 0x47u)
        {
            return (g_hooks[kH_StripRender].outer != g_hooks[kH_StripRender].hook)
                       ? [&]() {
                             SET_GPR_U64(ctx, 4, static_cast<uint64_t>(static_cast<int32_t>(atomic)));
                             return callWrapped(rdram, ctx, runtime, g_hooks[kH_StripRender].outer, 0x0039E828u, 0x0039F064u, 0x60u);
                         }()
                       : stripRenderImpl(rdram, ctx, runtime, atomic);
        }
        else
            return true;

        if (rd32(rdram, kRimLightFlag) != 0u)
            setRimAlpha(rdram, 3.0f);

        if (!setupLights(rdram, ctx, runtime, atomic, (frameCtx == rd32(rdram, ps + 0x0Cu)) ? 1u : 0u))
            return false;
        setPipelineMode(rdram, pipeline);

        const uint32_t pktBase = geomData + rdU16(rdram, geom + 0x3Au);
        if (skin != 0u)
        {
            setRimFlag(rdram, 1u);
            const uint32_t invBind = rd32(rdram, skin + 0x0Cu);
            const uint32_t hier = rd32(rdram, atomic + 0x2Cu);
            const uint32_t boneLtms = (hier != 0u) ? rd32(rdram, hier + 0x28u) : 0u;
            if (invBind != 0u && boneLtms != 0u)
            {
                setBoneScalePtr(rdram, kQuantScaleMirror);
                const uint32_t numBones = rd32(rdram, skin + 0u);
                const uint32_t needBytes = numBones << 6;
                const uint32_t newTail = rd32(rdram, kPktBufTail) - needBytes;
                wr32(rdram, kPktBufTail, newTail);
                uint32_t dstBones = 0u;
                if (pktAvail(rdram, kPktBufBase) >= needBytes)
                    dstBones = newTail;
                const uint32_t skinHdr = rd32(rdram, ps + 0x20u);
                wr32(rdram, skinHdr + 0x18u, 1u);
                wr32(rdram, skinHdr + 0x0Cu, 0u);
                wr32(rdram, skinHdr + 0x10u, numBones);
                wr32(rdram, skinHdr + 0x04u, dstBones);
                wr32(rdram, skinHdr + 0x08u, 0u);
                dirtyOr(rdram, 0x10u);

                for (uint32_t b = 0u; b < numBones; ++b)
                {
                    const uint32_t ib = invBind + (b << 6);
                    const uint32_t bl = boneLtms + (b << 6);
                    const uint32_t out = dstBones + (b << 6);

                    const float i00 = rdF32(rdram, ib + 0x00u), i01 = rdF32(rdram, ib + 0x04u), i02 = rdF32(rdram, ib + 0x08u);
                    const float i10 = rdF32(rdram, ib + 0x10u), i11 = rdF32(rdram, ib + 0x14u), i12 = rdF32(rdram, ib + 0x18u);
                    const float i20 = rdF32(rdram, ib + 0x20u), i21 = rdF32(rdram, ib + 0x24u), i22 = rdF32(rdram, ib + 0x28u);
                    const float i30 = rdF32(rdram, ib + 0x30u), i31 = rdF32(rdram, ib + 0x34u), i32 = rdF32(rdram, ib + 0x38u);

                    const float l00 = rdF32(rdram, bl + 0x00u), l01 = rdF32(rdram, bl + 0x04u), l02 = rdF32(rdram, bl + 0x08u);
                    const float l10 = rdF32(rdram, bl + 0x10u), l11 = rdF32(rdram, bl + 0x14u), l12 = rdF32(rdram, bl + 0x18u);
                    const float l20 = rdF32(rdram, bl + 0x20u), l21 = rdF32(rdram, bl + 0x24u), l22 = rdF32(rdram, bl + 0x28u);
                    const float l30 = rdF32(rdram, bl + 0x30u), l31 = rdF32(rdram, bl + 0x34u), l32 = rdF32(rdram, bl + 0x38u);

                    const float b00 = FPU_ADD_S(FPU_ADD_S(FPU_MUL_S(l00, i00), FPU_MUL_S(l10, i01)), FPU_MUL_S(l20, i02));
                    const float b01 = FPU_ADD_S(FPU_ADD_S(FPU_MUL_S(l01, i00), FPU_MUL_S(l11, i01)), FPU_MUL_S(l21, i02));
                    const float b02 = FPU_ADD_S(FPU_ADD_S(FPU_MUL_S(l02, i00), FPU_MUL_S(l12, i01)), FPU_MUL_S(l22, i02));

                    const float b10 = FPU_ADD_S(FPU_ADD_S(FPU_MUL_S(l00, i10), FPU_MUL_S(l10, i11)), FPU_MUL_S(l20, i12));
                    const float b11 = FPU_ADD_S(FPU_ADD_S(FPU_MUL_S(l01, i10), FPU_MUL_S(l11, i11)), FPU_MUL_S(l21, i12));
                    const float b12 = FPU_ADD_S(FPU_ADD_S(FPU_MUL_S(l02, i10), FPU_MUL_S(l12, i11)), FPU_MUL_S(l22, i12));

                    const float b20 = FPU_ADD_S(FPU_ADD_S(FPU_MUL_S(l00, i20), FPU_MUL_S(l10, i21)), FPU_MUL_S(l20, i22));
                    const float b21 = FPU_ADD_S(FPU_ADD_S(FPU_MUL_S(l01, i20), FPU_MUL_S(l11, i21)), FPU_MUL_S(l21, i22));
                    const float b22 = FPU_ADD_S(FPU_ADD_S(FPU_MUL_S(l02, i20), FPU_MUL_S(l12, i21)), FPU_MUL_S(l22, i22));

                    const float b30 = FPU_ADD_S(FPU_ADD_S(FPU_ADD_S(FPU_MUL_S(l00, i30), FPU_MUL_S(l10, i31)), FPU_MUL_S(l20, i32)), l30);
                    const float b31 = FPU_ADD_S(FPU_ADD_S(FPU_ADD_S(FPU_MUL_S(l01, i30), FPU_MUL_S(l11, i31)), FPU_MUL_S(l21, i32)), l31);
                    const float b32 = FPU_ADD_S(FPU_ADD_S(FPU_ADD_S(FPU_MUL_S(l02, i30), FPU_MUL_S(l12, i31)), FPU_MUL_S(l22, i32)), l32);

                    const float t0 = FPU_ADD_S(FPU_ADD_S(FPU_MUL_S(b00, qtx), FPU_MUL_S(b10, qty)), FPU_MUL_S(b20, qtz));
                    const float t1 = FPU_ADD_S(FPU_ADD_S(FPU_MUL_S(b01, qtx), FPU_MUL_S(b11, qty)), FPU_MUL_S(b21, qtz));
                    const float t2 = FPU_ADD_S(FPU_ADD_S(FPU_MUL_S(b02, qtx), FPU_MUL_S(b12, qty)), FPU_MUL_S(b22, qtz));

                    wrF32(rdram, out + 0x00u, FPU_MUL_S(b00, qsx));
                    wrF32(rdram, out + 0x04u, FPU_MUL_S(b01, qsx));
                    wrF32(rdram, out + 0x08u, FPU_MUL_S(b02, qsx));
                    wr32(rdram, out + 0x0Cu, 0u);

                    wrF32(rdram, out + 0x10u, FPU_MUL_S(b10, qsy));
                    wrF32(rdram, out + 0x14u, FPU_MUL_S(b11, qsy));
                    wrF32(rdram, out + 0x18u, FPU_MUL_S(b12, qsy));
                    wr32(rdram, out + 0x1Cu, 0u);

                    wrF32(rdram, out + 0x20u, FPU_MUL_S(b20, qsz));
                    wrF32(rdram, out + 0x24u, FPU_MUL_S(b21, qsz));
                    wrF32(rdram, out + 0x28u, FPU_MUL_S(b22, qsz));
                    wr32(rdram, out + 0x2Cu, 0u);

                    wrF32(rdram, out + 0x30u, FPU_ADD_S(FPU_ADD_S(b30, t0), origX));
                    wrF32(rdram, out + 0x34u, FPU_ADD_S(FPU_ADD_S(b31, t1), origY));
                    wrF32(rdram, out + 0x38u, FPU_ADD_S(FPU_ADD_S(b32, t2), origZ));
                    wrF32(rdram, out + 0x3Cu, 1.0f);
                }
            }
        }

        const uint32_t stripCount = rd32(rdram, geom + 0x30u) >> 20;
        const uint32_t matArray = rd32(rdram, geom + 0x0Cu);

        // Pass 1: alpha strips (matType & 2)
        bool alphaInit = false;
        for (uint32_t i = 0u; i < stripCount; ++i)
        {
            const uint32_t strip = geomData + 0x40u + i * 0x30u;
            const uint32_t mat = rd32(rdram, matArray + (static_cast<uint32_t>(rdU16(rdram, strip + 0x22u)) << 2));
            if (rd32(rdram, kForceShadowPass) != 0u || (matGetType(rdram, mat) & 2u) == 0u)
                continue;
            if (!alphaInit)
            {
                const uint32_t rState = rd32(rdram, kRenderStatePtr);
                if (rd32(rdram, rState + 0x20u) == 0u)
                {
                    alphaInit = true;
                    setFrameCtx(rdram, rd32(rdram, ps + 0x18u));
                }
            }
            matSetAlpha(rdram, rd32(rdram, ps + 0x1Cu), computeAlphaByte(rdram, mat));
            if (!submitStrip(rdram, ctx, runtime, strip, mat, pktBase, 0x0039F8F0u, 0x0039F918u))
                return false;
        }

        setFrameCtx(rdram, frameCtx);

        // Pass 2: opaque strips (!(matType & 2))
        for (uint32_t i = 0u; i < stripCount; ++i)
        {
            const uint32_t strip = geomData + 0x40u + i * 0x30u;
            const uint32_t mat = rd32(rdram, matArray + (static_cast<uint32_t>(rdU16(rdram, strip + 0x22u)) << 2));
            if (rd32(rdram, kForceShadowPass) != 0u || (matGetType(rdram, mat) & 2u) != 0u)
                continue;
            if (!submitStrip(rdram, ctx, runtime, strip, mat, pktBase, 0x0039FA14u, 0x0039FA3Cu))
                return false;
        }

        if (skin != 0u)
            setRimFlag(rdram, 0u);
        setAlphaTestBit(rdram, 1u);
        resetLights(rdram);
        return invokeUvSetHook(rdram, ctx, runtime, kEnvStateBase, 1.0f, 1.0f, 0x0039FA90u);
    }

    inline bool dispatchAtomicCallback(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                       uint32_t cb, uint32_t objAddr, uint32_t sourcePc, uint32_t retAddr) noexcept
    {
        if (cb == 0u || cb == 0xAAAAAAAAu)
        {
            if (g_hooks[kH_AtomicRender].outer != g_hooks[kH_AtomicRender].hook)
            {
                SET_GPR_U64(ctx, 4, static_cast<uint64_t>(static_cast<int32_t>(objAddr)));
                return callWrapped(rdram, ctx, runtime, g_hooks[kH_AtomicRender].outer, 0x0039EC18u, retAddr, 0x30u);
            }
            return atomicRenderImpl(rdram, ctx, runtime, objAddr);
        }
        if (cb == 0x0039EC18u && g_hooks[kH_AtomicRender].outer == g_hooks[kH_AtomicRender].hook)
            return atomicRenderImpl(rdram, ctx, runtime, objAddr);
        const uint32_t savedSp = GPR_U32(ctx, 29);
        const uint32_t savedRa = GPR_U32(ctx, 31);
        const uint32_t newSp = (savedSp - 0x30u) & ~0xFu;
        SET_GPR_U64(ctx, 4, static_cast<uint64_t>(static_cast<int32_t>(objAddr)));
        SET_GPR_U64(ctx, 29, static_cast<uint64_t>(static_cast<int32_t>(newSp)));
        SET_GPR_U64(ctx, 31, static_cast<uint64_t>(static_cast<int32_t>(retAddr)));
        ctx->pc = cb;
        const bool ok = runtime->dispatchGuestBranch(rdram, ctx, cb, sourcePc, retAddr,
                                                      PS2Runtime::GuestBranchKind::IndirectCall, "JALR");
        SET_GPR_U64(ctx, 29, static_cast<uint64_t>(static_cast<int32_t>(savedSp)));
        SET_GPR_U64(ctx, 31, static_cast<uint64_t>(static_cast<int32_t>(savedRa)));
        return ok;
    }

    inline bool clumpRenderImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t clump) noexcept
    {
        if (!validRam(clump, 0x20u))
            return false;
        const uint32_t head = clump + 8u;
        uint32_t cur = rd32(rdram, head);
        uint32_t guardCount = 0u;
        while (cur != head)
        {
            if (!validRam(cur, 4u) || cur < 0x1Cu || !validRam(cur - 0x1Cu, 0x34u) || ++guardCount > 4096u)
                return false;
            const uint32_t atomic = cur - 0x1Cu;
            const uint32_t next = rd32(rdram, cur);
            if ((rd8(rdram, atomic + 2u) & 4u) == 0u)
            {
                cur = next;
                continue;
            }
            uint32_t ltm = 0u;
            if (!frameGetLtmImpl(rdram, ctx, runtime, rd32(rdram, atomic + 4u), ltm, 0x002C02C0u, 0x002C02C0u))
                return false;
            const uint32_t cb = rd32(rdram, atomic + 0x24u);
            if (!dispatchAtomicCallback(rdram, ctx, runtime, cb, atomic, 0x002C02D4u, 0x002C02DCu))
                return false;
            cur = rd32(rdram, cur);
        }
        return true;
    }

    inline bool entityRenderImpl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t entity) noexcept
    {
        if (rd32(rdram, kRenderStatePtr) == 0u || rd32(rdram, kRenderInitFlag) == 0u || rd32(rdram, kUvMtxInitFlag) == 0u)
            return false;
        if (!validRam(entity, 0x1E8u))
            return false;
        const uint32_t rwObj = rd32(rdram, entity + 0x50u);
        if (rwObj == 0u)
            return true;
        const uint64_t f48 = rd64(rdram, entity + 0x48u);
        if ((f48 & 0xEULL) == 0x8ULL)
        {
            const uint64_t f1e0 = rd64(rdram, entity + 0x1E0u);
            if ((f1e0 >> 16) & 1ULL)
            {
                const uint32_t rState = rd32(rdram, kRenderStatePtr);
                if (rState == 0u || rd32(rdram, rState + 0x1Cu) != 0u)
                    return true;
            }
        }
        if ((f48 >> 52) & 1ULL)
            return true;

        if (rd8(rdram, rwObj) == 1u)
        {
            const uint32_t cb = rd32(rdram, rwObj + 0x24u);
            return dispatchAtomicCallback(rdram, ctx, runtime, cb, rwObj, 0x003C3F18u, 0x003C3F20u);
        }
        if (g_hooks[kH_ClumpRender].outer != g_hooks[kH_ClumpRender].hook)
        {
            SET_GPR_U64(ctx, 4, static_cast<uint64_t>(static_cast<int32_t>(rwObj)));
            return callWrapped(rdram, ctx, runtime, g_hooks[kH_ClumpRender].outer, 0x002C0270u, 0x003C3F40u, 0x20u);
        }
        return clumpRenderImpl(rdram, ctx, runtime, rwObj);
    }

    // -------------------------------------------------------------------------
    // Snapshot & parity verifier for VCS_NATIVE_RSL_RENDER_VERIFY=1
    // -------------------------------------------------------------------------

    struct VerifyStats
    {
        std::atomic<uint64_t> checked{0};
        std::atomic<uint64_t> diffs{0};
        std::atomic<uint64_t> fallbacks{0};
    } g_verifyStats;

    struct StateSnapshot
    {
        struct Range { uint32_t addr, size; };
        static constexpr Range kGlobalRanges[] = {
            {0x004864CCu, 0x20u},  // lights ptrs
            {0x004882A0u, 0x80u},  // dirty flags + GS caches
            {0x0048A028u, 0x20u},  // TXD slot cache
            {0x004CD570u, 0x20u},  // viewport / frustum / rim
            {0x005102C0u, 0x30u},  // renderSys struct
            {0x00520860u, 0x760u}, // matrices, clip planes, pkt header/cursors
            {0x006EDCA0u, 0x60u},  // env state
            {0x00729510u, 0x100u}, // GS queue
            {0x00742AC0u, 0x100u}, // view/GS/tex VRAM/cache keys
        };
        std::array<std::vector<uint8_t>, 9> globals;
        std::vector<uint8_t> renderSys8;
        std::vector<uint8_t> renderSys10;
        std::vector<uint8_t> renderSys1C;
        std::vector<uint8_t> renderSys20;
        std::vector<uint8_t> lightAmb;
        std::array<uint8_t, 0x70u> treeRoot{};
        std::vector<uint8_t> treePool;
        uint32_t sys8Addr = 0, sys10Addr = 0, sys1CAddr = 0, sys20Addr = 0, ambAddr = 0;
        uint32_t treePoolAddr = 0, treeCount = 0;

        struct DynBlock { uint32_t addr; std::vector<uint8_t> data; };
        std::vector<DynBlock> extra;

        uint32_t oldCur = 0, oldTail = 0;
        uint32_t txdObj = 0, txdEntry = 0;

        void capture(const uint8_t *rdram, uint32_t atomicOrEntity)
        {
            for (size_t i = 0; i < 9; ++i)
            {
                globals[i].resize(kGlobalRanges[i].size);
                std::memcpy(globals[i].data(), rdram + kGlobalRanges[i].addr, kGlobalRanges[i].size);
            }
            sys8Addr = rd32(rdram, kRenderSysBase + 8u);
            if (validRam(sys8Addr, 0x40u))
            {
                renderSys8.resize(0x40u);
                std::memcpy(renderSys8.data(), rdram + sys8Addr, 0x40u);
            }
            sys10Addr = rd32(rdram, kRenderSysBase + 0x10u);
            if (validRam(sys10Addr, 0xA0u))
            {
                renderSys10.resize(0xA0u);
                std::memcpy(renderSys10.data(), rdram + sys10Addr, 0xA0u);
            }
            sys1CAddr = rd32(rdram, kRenderSysBase + 0x1Cu);
            if (validRam(sys1CAddr, 0x40u))
            {
                renderSys1C.resize(0x40u);
                std::memcpy(renderSys1C.data(), rdram + sys1CAddr, 0x40u);
            }
            sys20Addr = rd32(rdram, kRenderSysBase + 0x20u);
            if (validRam(sys20Addr, 0x24u))
            {
                renderSys20.resize(0x24u);
                std::memcpy(renderSys20.data(), rdram + sys20Addr, 0x24u);
            }
            ambAddr = rd32(rdram, kLightAmbientPtr);
            if (validRam(ambAddr, 0x40u))
            {
                lightAmb.resize(0x40u);
                std::memcpy(lightAmb.data(), rdram + ambAddr, 0x40u);
            }
            std::memcpy(treeRoot.data(), rdram + 0x006EE3F0u, 0x70u);
            treeCount = rd32(rdram, 0x004CD58Cu);
            treePoolAddr = rd32(rdram, 0x00488760u);
            treePool.clear();
            if (treeCount > 0u && treeCount <= 16384u && validRam(treePoolAddr, treeCount * 0x70u))
            {
                treePool.resize(treeCount * 0x70u);
                std::memcpy(treePool.data(), rdram + treePoolAddr, treePool.size());
            }
            oldCur = rd32(rdram, kPktBufCursor);
            oldTail = rd32(rdram, kPktBufTail);

            extra.clear();
            auto saveBlk = [&](uint32_t a, uint32_t sz) {
                if (!validRam(a, sz)) return;
                for (const auto &b : extra) if (b.addr == a && b.data.size() >= sz) return;
                DynBlock db{a, std::vector<uint8_t>(sz)};
                std::memcpy(db.data.data(), rdram + a, sz);
                extra.push_back(std::move(db));
            };

            auto saveFrameTree = [&](uint32_t frame) {
                if (!validRam(frame, 0xA0u)) return;
                uint32_t root = rd32(rdram, frame + 0x98u);
                std::vector<uint32_t> q;
                if (validRam(root, 0xA0u)) q.push_back(root);
                if (validRam(frame, 0xA0u)) q.push_back(frame);
                size_t head = 0;
                while (head < q.size() && q.size() < 256u)
                {
                    uint32_t f = q[head++];
                    saveBlk(f, 0xA0u);
                    uint32_t ch = rd32(rdram, f + 0x90u);
                    uint32_t sib = rd32(rdram, f + 0x94u);
                    if (validRam(ch, 0xA0u)) q.push_back(ch);
                    if (validRam(sib, 0xA0u)) q.push_back(sib);
                }
            };

            auto inspectAtomic = [&](uint32_t at) {
                if (!validRam(at, 0x34u)) return;
                saveFrameTree(rd32(rdram, at + 4u));
                uint32_t txd = atomicGetTxd(rdram, at);
                if (validRam(txd, 0x24u))
                {
                    saveBlk(txd, 0x24u);
                    uint32_t r12 = rd32(rdram, txd + 0x0Cu);
                    if (validRam(r12 - 8u, 8u)) saveBlk(r12 - 8u, 8u);
                    int16_t idx = rdS16(rdram, txd + 0x1Eu);
                    uint32_t ent = lookupTxdEntry(rdram, idx);
                    if (validRam(ent, 0x1Cu))
                    {
                        saveBlk(ent, 0x1Cu);
                        uint32_t tp = rd32(rdram, ent);
                        if (tp > 0x28u && validRam(tp - 0x28u, 8u)) saveBlk(tp - 0x28u, 8u);
                    }
                }
            };

            if (validRam(atomicOrEntity, 0x54u))
            {
                const uint8_t ty = rd8(rdram, atomicOrEntity);
                if (ty == 1u) inspectAtomic(atomicOrEntity);
                else if (ty == 2u)
                {
                    uint32_t h = atomicOrEntity + 8u, c = rd32(rdram, h);
                    for (int n = 0; n < 128 && c != h && validRam(c - 0x1Cu, 0x34u); ++n)
                    {
                        inspectAtomic(c - 0x1Cu);
                        c = rd32(rdram, c);
                    }
                }
                else
                {
                    uint32_t obj = rd32(rdram, atomicOrEntity + 0x50u);
                    if (validRam(obj, 0x28u))
                    {
                        if (rd8(rdram, obj) == 1u) inspectAtomic(obj);
                        else
                        {
                            uint32_t h = obj + 8u, c = rd32(rdram, h);
                            for (int n = 0; n < 128 && c != h && validRam(c - 0x1Cu, 0x34u); ++n)
                            {
                                inspectAtomic(c - 0x1Cu);
                                c = rd32(rdram, c);
                            }
                        }
                    }
                }
            }
        }

        void restore(uint8_t *rdram) const
        {
            for (const auto &b : extra)
                std::memcpy(rdram + b.addr, b.data.data(), b.data.size());
            if (!renderSys8.empty()) std::memcpy(rdram + sys8Addr, renderSys8.data(), renderSys8.size());
            if (!renderSys10.empty()) std::memcpy(rdram + sys10Addr, renderSys10.data(), renderSys10.size());
            if (!renderSys1C.empty()) std::memcpy(rdram + sys1CAddr, renderSys1C.data(), renderSys1C.size());
            if (!renderSys20.empty()) std::memcpy(rdram + sys20Addr, renderSys20.data(), renderSys20.size());
            if (!lightAmb.empty()) std::memcpy(rdram + ambAddr, lightAmb.data(), lightAmb.size());
            std::memcpy(rdram + 0x006EE3F0u, treeRoot.data(), 0x70u);
            if (!treePool.empty()) std::memcpy(rdram + treePoolAddr, treePool.data(), treePool.size());
            for (size_t i = 0; i < 9; ++i)
                std::memcpy(rdram + kGlobalRanges[i].addr, globals[i].data(), kGlobalRanges[i].size);
        }
    };

    inline bool isPackedAdCntBlock(const uint8_t *buf, uint32_t off, uint32_t size, uint32_t &qwcOut, uint32_t &nloopOut) noexcept
    {
        if (off + 32u > size)
            return false;
        const uint64_t dmaLo = rd64(buf, off);
        const uint32_t qwc = static_cast<uint32_t>(dmaLo & 0xFFFFULL);
        const uint32_t id = static_cast<uint32_t>((dmaLo >> 28) & 7u);
        const uint32_t vif2 = rd32(buf, off + 8u);
        const uint32_t vif3 = rd32(buf, off + 12u);
        if (id != 1u || qwc < 2u || (vif2 != 0u && vif2 != 0x10000000u) ||
            ((vif3 & 0xFE000000u) != 0x50000000u) || ((vif3 & 0xFFFFu) != qwc))
            return false;
        if (off + 16u + qwc * 16u > size)
            return false;
        const uint64_t gifLo = rd64(buf, off + 16u);
        const uint64_t gifHi = rd64(buf, off + 24u);
        const uint32_t nloop = static_cast<uint32_t>(gifLo & 0x7FFFULL);
        if (nloop + 1u != qwc || ((gifLo >> 58) & 0x3FULL) != 0x04ULL || gifHi != 0xEULL)
            return false;
        qwcOut = qwc;
        nloopOut = nloop;
        return true;
    }

    // Look ahead from a MIPTBP1/MIPTBP2 GS reg (inside a CNT A+D block) for a deferred TEX1_1/2 flush
    // in subsequent draw commands, before that unit's MIPTBP is overwritten.
    inline bool findUpcomingTex1Mxl(const uint8_t *buf, uint32_t curTagOff, uint32_t curRegOff,
                                    uint32_t size, uint32_t unit, uint32_t &mxlOut, bool &overwrittenOut) noexcept
    {
        overwrittenOut = false;
        uint32_t off = curTagOff;
        bool firstTag = true;
        while (off + 16u <= size)
        {
            const uint64_t dmaLo = rd64(buf, off);
            const uint32_t qwc = static_cast<uint32_t>(dmaLo & 0xFFFFULL);
            const uint32_t id = static_cast<uint32_t>((dmaLo >> 28) & 7u);
            const uint32_t stride = (id == 1u) ? (16u + qwc * 16u) : 16u;
            if (off + stride > size)
                break;

            uint32_t blkQwc = 0u, nloop = 0u;
            if (isPackedAdCntBlock(buf, off, size, blkQwc, nloop))
            {
                const uint32_t startReg = firstTag ? ((curRegOff - (off + 32u)) / 16u + 1u) : 0u;
                for (uint32_t i = startReg; i < nloop; ++i)
                {
                    const uint32_t rOff = off + 32u + i * 16u;
                    const uint64_t reg = rd64(buf, rOff + 8u);
                    if (reg == 0x14ULL + unit)
                    {
                        mxlOut = static_cast<uint32_t>((rd64(buf, rOff) >> 2) & 7ULL);
                        return true;
                    }
                    if (reg == 0x34ULL + unit || reg == 0x36ULL + unit)
                    {
                        overwrittenOut = true;
                        return false;
                    }
                }
            }
            firstTag = false;
            off += stride;
        }
        return false;
    }

    inline bool pktBytesEqual(const uint8_t *a, const uint8_t *b, uint32_t size,
                              uint32_t preMxl0, uint32_t preMxl1, uint32_t postMxl0, uint32_t postMxl1) noexcept
    {
        if (size == 0u || std::memcmp(a, b, size) == 0)
            return true;
        if ((size & 15u) != 0u)
            return false;

        uint32_t curMxl[2] = {preMxl0, preMxl1};
        const uint32_t finalMxl[2] = {postMxl0, postMxl1};
        uint32_t off = 0u;
        while (off + 16u <= size)
        {
            if (std::memcmp(a + off, b + off, 16u) != 0)
                return false;
            const uint64_t dmaLo = rd64(a, off);
            const uint32_t qwc = static_cast<uint32_t>(dmaLo & 0xFFFFULL);
            const uint32_t id = static_cast<uint32_t>((dmaLo >> 28) & 7u);
            const uint32_t payload = (id == 1u) ? (qwc * 16u) : 0u;
            if (off + 16u + payload > size)
                return std::memcmp(a + off + 16u, b + off + 16u, size - (off + 16u)) == 0;

            uint32_t blkQwc = 0u, nloop = 0u;
            if (!isPackedAdCntBlock(a, off, size, blkQwc, nloop))
            {
                if (payload > 0u && std::memcmp(a + off + 16u, b + off + 16u, payload) != 0)
                    return false;
                off += 16u + payload;
                continue;
            }

            if (std::memcmp(a + off + 16u, b + off + 16u, 16u) != 0)
                return false;
            for (uint32_t i = 0u; i < nloop; ++i)
            {
                const uint32_t rOff = off + 32u + i * 16u;
                const uint64_t regA = rd64(a, rOff + 8u);
                const uint64_t regB = rd64(b, rOff + 8u);
                if (regA != regB)
                    return false;
                const uint64_t valA = rd64(a, rOff);
                const uint64_t valB = rd64(b, rOff);
                if (regA == 0x14ULL || regA == 0x15ULL)
                {
                    if (valA != valB)
                        return false;
                    curMxl[regA - 0x14ULL] = static_cast<uint32_t>((valA >> 2) & 7ULL);
                }
                else if (regA == 0x34ULL || regA == 0x35ULL || regA == 0x36ULL || regA == 0x37ULL)
                {
                    // FUN_002f62b8 only initializes mip slots < numMips and leaves higher slots as uninit stack.
                    const uint32_t unit = (regA == 0x35ULL || regA == 0x37ULL) ? 1u : 0u;
                    const bool isBp2 = (regA == 0x36ULL || regA == 0x37ULL);
                    uint32_t mxl = 0u;
                    bool overwritten = false;
                    if (!findUpcomingTex1Mxl(a, off, rOff, size, unit, mxl, overwritten))
                        mxl = overwritten ? curMxl[unit] : finalMxl[unit];
                    const uint64_t mask = isBp2
                        ? ((mxl <= 3u) ? 0ULL : ((mxl >= 6u) ? ~0ULL : ((1ULL << (20u * (mxl - 3u))) - 1ULL)))
                        : ((mxl >= 3u) ? ~0ULL : ((1ULL << (20u * mxl)) - 1ULL));
                    if ((valA & mask) != (valB & mask))
                        return false;
                }
                else if (valA != valB)
                {
                    return false;
                }
            }
            off += 16u + payload;
        }
        return true;
    }

    inline bool skinTailEqual(const uint8_t *a, const uint8_t *b, uint32_t size) noexcept
    {
        if ((size & 0x3Fu) != 0u)
            return std::memcmp(a, b, size) == 0;
        for (uint32_t off = 0u; off < size; off += 64u)
        {
            // Bone rows write 0..11, 16..27, 32..43, 48..63; +0x0C/+0x1C/+0x2C are uninit stack slots in MIPS.
            if (std::memcmp(a + off + 0u, b + off + 0u, 12u) != 0 ||
                std::memcmp(a + off + 16u, b + off + 16u, 12u) != 0 ||
                std::memcmp(a + off + 32u, b + off + 32u, 12u) != 0 ||
                std::memcmp(a + off + 48u, b + off + 48u, 16u) != 0)
                return false;
        }
        return true;
    }

    struct CallArgsBackup
    {
        uint64_t r[8];
        uint64_t s[9];
        uint32_t sp, ra, pc;
        float f[4];

        void save(const R5900Context *ctx) noexcept
        {
            for (int i = 0; i < 8; ++i)
                r[i] = GPR_U64(ctx, 4 + i);
            for (int i = 0; i < 8; ++i)
                s[i] = GPR_U64(ctx, 16 + i);
            s[8] = GPR_U64(ctx, 30);
            sp = GPR_U32(ctx, 29);
            ra = GPR_U32(ctx, 31);
            pc = ctx->pc;
            f[0] = ctx->f[12];
            f[1] = ctx->f[13];
            f[2] = ctx->f[14];
            f[3] = ctx->f[15];
        }

        void restore(R5900Context *ctx) const noexcept
        {
            for (int i = 0; i < 8; ++i)
                SET_GPR_U64(ctx, 4 + i, r[i]);
            for (int i = 0; i < 8; ++i)
                SET_GPR_U64(ctx, 16 + i, s[i]);
            SET_GPR_U64(ctx, 30, s[8]);
            SET_GPR_U64(ctx, 29, static_cast<uint64_t>(static_cast<int32_t>(sp)));
            SET_GPR_U64(ctx, 31, static_cast<uint64_t>(static_cast<int32_t>(ra)));
            ctx->pc = pc;
            ctx->f[12] = f[0];
            ctx->f[13] = f[1];
            ctx->f[14] = f[2];
            ctx->f[15] = f[3];
        }
    };

    struct CheckpointDeferGuard
    {
        bool active = false;
        CheckpointDeferGuard() noexcept
        {
            ps2PushCheckpointDefer();
            active = true;
        }
        void popNow() noexcept
        {
            if (active)
            {
                active = false;
                ps2PopCheckpointDefer();
            }
        }
        ~CheckpointDeferGuard() noexcept
        {
            popNow();
        }
    };

    template <typename Fn>
    inline void runHookWithFallback(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                    HookIdx slotIdx, Fn &&fn)
    {
        CallArgsBackup args;
        args.save(ctx);
        bool ok = false;
        {
            CheckpointDeferGuard defer;
            ok = fn();
        }
        if (!ok)
        {
            args.restore(ctx);
            g_hooks[slotIdx].orig(rdram, ctx, runtime);
            return;
        }
        ctx->pc = GPR_U32(ctx, 31);
    }

    template <typename Fn>
    void verifyAgainstOrig(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                           HookIdx slotIdx, uint32_t atomicOrEntity, Fn &&nativeBody)
    {
        const uint32_t addr = g_hooks[slotIdx].addr;
        auto orig = g_hooks[slotIdx].orig;
        if (g_verify && t_verifyDepth == 0)
        {
            StateSnapshot snap;
            snap.capture(rdram, atomicOrEntity);
            const R5900Context savedCtx = *ctx;
            const bool unwindBefore = g_checkpointUnwind;

            bool ran = false;
            {
                CheckpointDeferGuard defer;
                ++t_verifyDepth;
                ran = nativeBody();
                --t_verifyDepth;
            }

            if (!ran)
            {
                g_verifyStats.fallbacks.fetch_add(1u, std::memory_order_relaxed);
                snap.restore(rdram);
                *ctx = savedCtx;
                g_checkpointUnwind = unwindBefore;
                orig(rdram, ctx, runtime);
                return;
            }

            const uint32_t natCur = rd32(rdram, kPktBufCursor);
            const uint32_t natTail = rd32(rdram, kPktBufTail);
            const uint32_t natDirty = rd32(rdram, kDirtyFlags);
            const uint32_t natFrust = rd32(rdram, kFrustumResult);
            const uint64_t natV0 = GPR_U64(ctx, 2);

            std::vector<uint8_t> natPkt;
            if (natCur > snap.oldCur && validRam(snap.oldCur, natCur - snap.oldCur))
            {
                natPkt.resize(natCur - snap.oldCur);
                std::memcpy(natPkt.data(), rdram + snap.oldCur, natPkt.size());
            }
            std::vector<uint8_t> natTailBuf;
            if (snap.oldTail > natTail && validRam(natTail, snap.oldTail - natTail))
            {
                natTailBuf.resize(snap.oldTail - natTail);
                std::memcpy(natTailBuf.data(), rdram + natTail, natTailBuf.size());
            }

            snap.restore(rdram);
            *ctx = savedCtx;
            g_checkpointUnwind = unwindBefore;
            {
                CheckpointDeferGuard defer;
                ++t_verifyDepth;
                orig(rdram, ctx, runtime);
                --t_verifyDepth;
            }

            const uint32_t origCur = rd32(rdram, kPktBufCursor);
            const uint32_t origTail = rd32(rdram, kPktBufTail);
            const uint32_t origDirty = rd32(rdram, kDirtyFlags);
            const uint32_t origFrust = rd32(rdram, kFrustumResult);
            const uint64_t origV0 = GPR_U64(ctx, 2);

            const uint32_t preMxl0 = static_cast<uint32_t>((rd64(snap.globals[1].data(), 0x50u) >> 2) & 7ULL);
            const uint32_t preMxl1 = static_cast<uint32_t>((rd64(snap.globals[1].data(), 0x58u) >> 2) & 7ULL);
            const uint32_t postMxl0 = static_cast<uint32_t>((rd64(rdram, kGsTex1State + 0u) >> 2) & 7ULL);
            const uint32_t postMxl1 = static_cast<uint32_t>((rd64(rdram, kGsTex1State + 8u) >> 2) & 7ULL);
            const bool checkV0 = (slotIdx == kH_AtomicRender);
            const bool samePkt = (natCur == origCur) &&
                                 (natCur <= snap.oldCur ||
                                  pktBytesEqual(natPkt.data(), rdram + snap.oldCur, natCur - snap.oldCur,
                                                preMxl0, preMxl1, postMxl0, postMxl1));
            const bool sameTail = (natTail == origTail) &&
                                  (snap.oldTail <= origTail || skinTailEqual(natTailBuf.data(), rdram + origTail, snap.oldTail - origTail));
            const bool sameMeta = (natDirty == origDirty) && (natFrust == origFrust) && (!checkV0 || natV0 == origV0);

            const uint64_t n = g_verifyStats.checked.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (!samePkt || !sameTail || !sameMeta)
            {
                const uint64_t d = g_verifyStats.diffs.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (d <= 20u)
                {
                    std::fprintf(stderr,
                                 "[rsl-render-verify] DIFF #%llu fn=0x%08X obj=0x%08X cur=(%u vs %u) tail=(%u vs %u) dirty=(0x%X vs 0x%X) frust=(%d vs %d) v0=(0x%llX vs 0x%llX) pktSame=%d tailSame=%d\n",
                                 static_cast<unsigned long long>(d), addr, atomicOrEntity, natCur - snap.oldCur, origCur - snap.oldCur,
                                 snap.oldTail - natTail, snap.oldTail - origTail, natDirty, origDirty,
                                 static_cast<int>(natFrust), static_cast<int>(origFrust),
                                 static_cast<unsigned long long>(natV0), static_cast<unsigned long long>(origV0),
                                 samePkt ? 1 : 0, sameTail ? 1 : 0);
                }
            }
            if ((n >= 4096u && (n & (n - 1u)) == 0u) || (n & 0xFFFFu) == 0u)
            {
                std::fprintf(stderr, "[rsl-render-verify] checked=%llu diffs=%llu fallbacks=%llu\n",
                             static_cast<unsigned long long>(n),
                             static_cast<unsigned long long>(g_verifyStats.diffs.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(g_verifyStats.fallbacks.load(std::memory_order_relaxed)));
            }
            return;
        }

        if (t_verifyDepth > 0)
        {
            orig(rdram, ctx, runtime);
            return;
        }
        CallArgsBackup args;
        args.save(ctx);
        bool ok = false;
        {
            CheckpointDeferGuard defer;
            ok = nativeBody();
        }
        if (!ok)
        {
            args.restore(ctx);
            orig(rdram, ctx, runtime);
            return;
        }
        ctx->pc = GPR_U32(ctx, 31);
    }

    // -------------------------------------------------------------------------
    // Guest entrypoint shims
    // -------------------------------------------------------------------------

    PS2_GUEST_CC void hookEntityRender(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x003C3E58u || watchpointsActive())
        {
            g_hooks[kH_EntityRender].orig(rdram, ctx, runtime);
            return;
        }
        ensureOuterHooks(runtime);
        const uint32_t entity = GPR_U32(ctx, 4);
        verifyAgainstOrig(rdram, ctx, runtime, kH_EntityRender, entity, [&]() {
            return entityRenderImpl(rdram, ctx, runtime, entity);
        });
    }

    PS2_GUEST_CC void hookClumpRender(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x002C0270u || watchpointsActive())
        {
            g_hooks[kH_ClumpRender].orig(rdram, ctx, runtime);
            return;
        }
        ensureOuterHooks(runtime);
        const uint32_t clump = GPR_U32(ctx, 4);
        verifyAgainstOrig(rdram, ctx, runtime, kH_ClumpRender, clump, [&]() {
            return validRam(clump, 0x20u) && clumpRenderImpl(rdram, ctx, runtime, clump);
        });
    }

    PS2_GUEST_CC void hookAtomicRender(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x0039EC18u || watchpointsActive())
        {
            g_hooks[kH_AtomicRender].orig(rdram, ctx, runtime);
            return;
        }
        ensureOuterHooks(runtime);
        const uint32_t atomic = GPR_U32(ctx, 4);
        verifyAgainstOrig(rdram, ctx, runtime, kH_AtomicRender, atomic, [&]() {
            if (!atomicRenderImpl(rdram, ctx, runtime, atomic))
                return false;
            SET_GPR_U64(ctx, 2, static_cast<uint64_t>(static_cast<int32_t>(atomic)));
            return true;
        });
    }

    PS2_GUEST_CC void hookStripRender(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t atomic = GPR_U32(ctx, 4);
        if (ctx->pc != 0x0039E828u || watchpointsActive() || (g_verify && t_verifyDepth > 0) ||
            rd32(rdram, kRenderStatePtr) == 0u || rd32(rdram, kRenderInitFlag) == 0u || rd32(rdram, kUvMtxInitFlag) == 0u ||
            !validRam(atomic, 0x34u) || !validRam(rd32(rdram, atomic + 0x14u), 0x60u))
        {
            g_hooks[kH_StripRender].orig(rdram, ctx, runtime);
            return;
        }
        ensureOuterHooks(runtime);
        runHookWithFallback(rdram, ctx, runtime, kH_StripRender, [&]() {
            if (!stripRenderImpl(rdram, ctx, runtime, atomic))
                return false;
            SET_GPR_U64(ctx, 2, static_cast<uint64_t>(static_cast<int32_t>(atomic)));
            return true;
        });
    }

    PS2_GUEST_CC void hookAmbientPack(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x0039E7F0u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_AmbientPack].orig(rdram, ctx, runtime);
            return;
        }
        const uint32_t rgba = GPR_U32(ctx, 4);
        const uint32_t a = ((rgba >> 24) + 1u) >> 1;
        setAmbPacked(rdram, (rgba & 0x00FFFFFFu) | (a << 24));
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookTexBind(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x00408F30u || watchpointsActive())
        {
            g_hooks[kH_TexBind].orig(rdram, ctx, runtime);
            return;
        }
        ensureOuterHooks(runtime);
        const uint32_t raster = GPR_U32(ctx, 4);
        verifyAgainstOrig(rdram, ctx, runtime, kH_TexBind, raster, [&]() {
            return texBindImpl(rdram, ctx, runtime, raster);
        });
    }

    PS2_GUEST_CC void hookTexApply(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x00317FB8u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_TexApply].orig(rdram, ctx, runtime);
            return;
        }
        ensureOuterHooks(runtime);
        const uint32_t a0 = GPR_U32(ctx, 4), a1 = GPR_U32(ctx, 5), a2 = GPR_U32(ctx, 6), a3 = GPR_U32(ctx, 7);
        runHookWithFallback(rdram, ctx, runtime, kH_TexApply, [&]() {
            return texApplyImpl(rdram, ctx, runtime, a0, a1, a2, a3);
        });
    }

    PS2_GUEST_CC void hookEnvMtx(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x00317F10u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_EnvMtx].orig(rdram, ctx, runtime);
            return;
        }
        envMapMtxImpl(rdram, GPR_U32(ctx, 4), GPR_U32(ctx, 5));
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookUvSet(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x00317D48u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_UvSet].orig(rdram, ctx, runtime);
            return;
        }
        ensureOuterHooks(runtime);
        const uint32_t a0 = GPR_U32(ctx, 4);
        const float u = ctx->f[12], v = ctx->f[13];
        runHookWithFallback(rdram, ctx, runtime, kH_UvSet, [&]() {
            return uvSetImpl(rdram, ctx, runtime, a0, u, v);
        });
    }

    PS2_GUEST_CC void hookSetTexture(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x002F62B8u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_SetTexture].orig(rdram, ctx, runtime);
            return;
        }
        ensureOuterHooks(runtime);
        const uint32_t a0 = GPR_U32(ctx, 4), a1 = GPR_U32(ctx, 5), a2 = GPR_U32(ctx, 6), a3 = GPR_U32(ctx, 7);
        const uint32_t t0 = GPR_U32(ctx, 8), t1 = GPR_U32(ctx, 9), t2 = GPR_U32(ctx, 10), t3 = GPR_U32(ctx, 11);
        const uint32_t mipFlags = rd32(rdram, GPR_U32(ctx, 29));
        runHookWithFallback(rdram, ctx, runtime, kH_SetTexture, [&]() {
            return setTextureImpl(rdram, ctx, runtime, a0, a1, a2, a3, t0, t1, t2, t3, mipFlags);
        });
    }

    PS2_GUEST_CC void hookSetClut(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x002F6A78u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_SetClut].orig(rdram, ctx, runtime);
            return;
        }
        ensureOuterHooks(runtime);
        const uint32_t a0 = GPR_U32(ctx, 4), a1 = GPR_U32(ctx, 5), a2 = GPR_U32(ctx, 6), a3 = GPR_U32(ctx, 7), t0 = GPR_U32(ctx, 8);
        runHookWithFallback(rdram, ctx, runtime, kH_SetClut, [&]() {
            return setClutImpl(rdram, ctx, runtime, a0, a1, a2, a3, t0);
        });
    }

    PS2_GUEST_CC void hookDrawModel(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x002F7D58u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_DrawModel].orig(rdram, ctx, runtime);
            return;
        }
        ensureOuterHooks(runtime);
        const uint32_t a0 = GPR_U32(ctx, 4);
        runHookWithFallback(rdram, ctx, runtime, kH_DrawModel, [&]() {
            return drawPacketImpl(rdram, ctx, runtime, a0, 0u, 0, 0x002F7D84u);
        });
    }

    PS2_GUEST_CC void hookDrawModelB(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x002F80A0u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_DrawModelB].orig(rdram, ctx, runtime);
            return;
        }
        ensureOuterHooks(runtime);
        const uint32_t a0 = GPR_U32(ctx, 4);
        runHookWithFallback(rdram, ctx, runtime, kH_DrawModelB, [&]() {
            return drawPacketImpl(rdram, ctx, runtime, a0, 0u, 1, 0x002F80CCu);
        });
    }

    PS2_GUEST_CC void hookDrawWorld(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x002F7EB0u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_DrawWorld].orig(rdram, ctx, runtime);
            return;
        }
        ensureOuterHooks(runtime);
        const uint32_t a0 = GPR_U32(ctx, 4), a1 = GPR_U32(ctx, 5);
        runHookWithFallback(rdram, ctx, runtime, kH_DrawWorld, [&]() {
            return drawPacketImpl(rdram, ctx, runtime, a0, a1, 2, 0x002F7EE4u);
        });
    }

    PS2_GUEST_CC void hookGsSendA(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x002F7908u || watchpointsActive() || (g_verify && t_verifyDepth > 0) ||
            !validRam(rd32(rdram, kPktBufCursor), 64u))
        {
            g_hooks[kH_GsSendA].orig(rdram, ctx, runtime);
            return;
        }
        callGsSendImpl(rdram);
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookGsSendB(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x002F7B30u || watchpointsActive() || (g_verify && t_verifyDepth > 0) ||
            !validRam(rd32(rdram, kPktBufCursor), 64u))
        {
            g_hooks[kH_GsSendB].orig(rdram, ctx, runtime);
            return;
        }
        callGsSendImpl(rdram);
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookSendTexMtx(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x002F7090u || watchpointsActive() || (g_verify && t_verifyDepth > 0) ||
            !validRam(GPR_U32(ctx, 4), 0x30u) || pktAvail(rdram, kPktBufBase) < 0x30u)
        {
            g_hooks[kH_SendTexMtx].orig(rdram, ctx, runtime);
            return;
        }
        sendTexMatrixImpl(rdram, GPR_U32(ctx, 4));
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookFrustumSphere(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x002F7430u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_FrustumSphere].orig(rdram, ctx, runtime);
            return;
        }
        ensureOuterHooks(runtime);
        const uint32_t geom = GPR_U32(ctx, 4), localFlag = GPR_U32(ctx, 5);
        runHookWithFallback(rdram, ctx, runtime, kH_FrustumSphere, [&]() {
            int32_t res = 0;
            if (!frustumTestImpl(rdram, ctx, runtime, geom, localFlag, false, res))
                return false;
            SET_GPR_U64(ctx, 2, static_cast<uint64_t>(static_cast<int64_t>(res)));
            return true;
        });
    }

    PS2_GUEST_CC void hookFrustumBox(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x002F7528u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_FrustumBox].orig(rdram, ctx, runtime);
            return;
        }
        ensureOuterHooks(runtime);
        const uint32_t geom = GPR_U32(ctx, 4), localFlag = GPR_U32(ctx, 5);
        runHookWithFallback(rdram, ctx, runtime, kH_FrustumBox, [&]() {
            int32_t res = 0;
            if (!frustumTestImpl(rdram, ctx, runtime, geom, localFlag, true, res))
                return false;
            SET_GPR_U64(ctx, 2, static_cast<uint64_t>(static_cast<int64_t>(res)));
            return true;
        });
    }

    PS2_GUEST_CC void hookBuildClipPlanes(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x002F7620u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_BuildClipPlanes].orig(rdram, ctx, runtime);
            return;
        }
        runHookWithFallback(rdram, ctx, runtime, kH_BuildClipPlanes, [&]() {
            return buildClipPlanesImpl(rdram, ctx, runtime);
        });
    }

    PS2_GUEST_CC void hookFrameGetLtm(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t frame = GPR_U32(ctx, 4);
        if (ctx->pc != 0x001B50B0u || watchpointsActive() || (g_verify && t_verifyDepth > 0) ||
            !validRam(frame, 0xA0u))
        {
            g_hooks[kH_FrameGetLtm].orig(rdram, ctx, runtime);
            return;
        }
        ensureOuterHooks(runtime);
        runHookWithFallback(rdram, ctx, runtime, kH_FrameGetLtm, [&]() {
            uint32_t ltm = 0u;
            if (!frameGetLtmImpl(rdram, ctx, runtime, frame, ltm, 0x001B50DCu, 0x001B5094u))
                return false;
            SET_GPR_U64(ctx, 2, static_cast<uint64_t>(static_cast<int32_t>(ltm)));
            return true;
        });
    }

    PS2_GUEST_CC void hookFrameUpdateRoot(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t root = GPR_U32(ctx, 4);
        if (ctx->pc != 0x001B5040u || watchpointsActive() || (g_verify && t_verifyDepth > 0) ||
            !validRam(root, 0xA0u))
        {
            g_hooks[kH_FrameUpdateRoot].orig(rdram, ctx, runtime);
            return;
        }
        ensureOuterHooks(runtime);
        runHookWithFallback(rdram, ctx, runtime, kH_FrameUpdateRoot, [&]() {
            return frameUpdateRootImpl(rdram, ctx, runtime, root, 0x001B5094u);
        });
    }

    PS2_GUEST_CC void hookTxdSlotTouch(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t obj = GPR_U32(ctx, 4);
        if (ctx->pc != 0x003CD678u || watchpointsActive() || (g_verify && t_verifyDepth > 0) ||
            !validRam(obj, 0x24u))
        {
            g_hooks[kH_TxdSlotTouch].orig(rdram, ctx, runtime);
            return;
        }
        txdSlotTouch(rdram, obj);
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookAtomicGetTxd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t atomic = GPR_U32(ctx, 4);
        if (ctx->pc != 0x0036DA30u || watchpointsActive() || (g_verify && t_verifyDepth > 0) ||
            !validRam(atomic, 0x2Cu))
        {
            g_hooks[kH_AtomicGetTxd].orig(rdram, ctx, runtime);
            return;
        }
        SET_GPR_U64(ctx, 2, static_cast<uint64_t>(static_cast<int32_t>(atomicGetTxd(rdram, atomic))));
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookSetupLights(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x001A73E0u || watchpointsActive() || (g_verify && t_verifyDepth > 0) ||
            rd32(rdram, kRenderInitFlag) == 0u)
        {
            g_hooks[kH_SetupLights].orig(rdram, ctx, runtime);
            return;
        }
        ensureOuterHooks(runtime);
        const uint32_t atomic = GPR_U32(ctx, 4), useDir = GPR_U32(ctx, 5);
        runHookWithFallback(rdram, ctx, runtime, kH_SetupLights, [&]() {
            return setupLights(rdram, ctx, runtime, atomic, useDir);
        });
    }

    PS2_GUEST_CC void hookResetLights(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x001A7038u || watchpointsActive() || (g_verify && t_verifyDepth > 0) ||
            rd32(rdram, kRenderInitFlag) == 0u)
        {
            g_hooks[kH_ResetLights].orig(rdram, ctx, runtime);
            return;
        }
        resetLights(rdram);
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookSetAmbientLight(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x001A70B0u || watchpointsActive() || (g_verify && t_verifyDepth > 0) ||
            rd32(rdram, kRenderInitFlag) == 0u)
        {
            g_hooks[kH_SetAmbientLight].orig(rdram, ctx, runtime);
            return;
        }
        setAmbientLight(rdram, GPR_U32(ctx, 4));
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookSetDirLight(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x001A7260u || watchpointsActive() || (g_verify && t_verifyDepth > 0) ||
            rd32(rdram, kRenderInitFlag) == 0u)
        {
            g_hooks[kH_SetDirLight].orig(rdram, ctx, runtime);
            return;
        }
        ensureOuterHooks(runtime);
        const uint32_t light = GPR_U32(ctx, 4), slot = GPR_U32(ctx, 5);
        runHookWithFallback(rdram, ctx, runtime, kH_SetDirLight, [&]() {
            return setDirLight(rdram, ctx, runtime, light, slot, 0x001A72A4u);
        });
    }

    PS2_GUEST_CC void hookSetLightAmbientG(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x001CECE8u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_SetLightAmbientG].orig(rdram, ctx, runtime);
            return;
        }
        setLightAmbientGlobal(rdram, GPR_U32(ctx, 4), GPR_U32(ctx, 5), GPR_U32(ctx, 6), GPR_U32(ctx, 7), GPR_U32(ctx, 8));
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookSetLightSpecG(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x001CED88u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_SetLightSpecG].orig(rdram, ctx, runtime);
            return;
        }
        setLightSpecGlobal(rdram, GPR_U32(ctx, 4), GPR_U32(ctx, 5), GPR_U32(ctx, 6), GPR_U32(ctx, 7));
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookSetLightDirVec(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x0040F428u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_SetLightDirVec].orig(rdram, ctx, runtime);
            return;
        }
        setLightDirVec(rdram, GPR_U32(ctx, 4), GPR_U32(ctx, 5), GPR_U32(ctx, 6));
        SET_GPR_U64(ctx, 2, 1ULL);
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookSetLightDirRgb(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x0040F4B8u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_SetLightDirRgb].orig(rdram, ctx, runtime);
            return;
        }
        setLightDirRgb(rdram, GPR_U32(ctx, 4), GPR_U32(ctx, 5), GPR_U32(ctx, 6), GPR_U32(ctx, 7), GPR_U32(ctx, 8));
        SET_GPR_U64(ctx, 2, 1ULL);
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookSetLightAmbRgb(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x0040F530u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_SetLightAmbRgb].orig(rdram, ctx, runtime);
            return;
        }
        setLightAmbRgb(rdram, GPR_U32(ctx, 4), GPR_U32(ctx, 5), GPR_U32(ctx, 6), GPR_U32(ctx, 7));
        SET_GPR_U64(ctx, 2, 1ULL);
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookSetLightUnkRgb(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x0040F5A0u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_SetLightUnkRgb].orig(rdram, ctx, runtime);
            return;
        }
        setLightUnkRgb(rdram, GPR_U32(ctx, 4), GPR_U32(ctx, 5), GPR_U32(ctx, 6), GPR_U32(ctx, 7));
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookPoolIncRef(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x003A5B98u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_PoolIncRef].orig(rdram, ctx, runtime);
            return;
        }
        const uint32_t entry = lookupTxdEntry(rdram, GPR_S32(ctx, 4));
        const uint16_t v = static_cast<uint16_t>(rdU16(rdram, entry + 4u) + 1u);
        wr16(rdram, entry + 4u, v);
        SET_GPR_U64(ctx, 2, static_cast<uint64_t>(v));
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookPoolRelease(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x003A5BF0u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_PoolRelease].orig(rdram, ctx, runtime);
            return;
        }
        const uint32_t entry = lookupTxdEntry(rdram, GPR_S32(ctx, 4));
        const uint16_t c4 = static_cast<uint16_t>(rdU16(rdram, entry + 4u) + 1u);
        const int16_t c6 = rdS16(rdram, entry + 6u);
        wr16(rdram, entry + 4u, c4);
        if (c6 == 0)
        {
            const uint32_t tp = rd32(rdram, entry);
            if (tp != 0u)
                resFlag(rdram, tp - 0x20u, 0u);
        }
        const uint16_t next6 = static_cast<uint16_t>(static_cast<uint16_t>(c6) + 1u);
        wr16(rdram, entry + 6u, next6);
        SET_GPR_U64(ctx, 2, static_cast<uint64_t>(next6));
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookSlotInc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x001CEB00u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_SlotInc].orig(rdram, ctx, runtime);
            return;
        }
        const uint32_t obj = GPR_U32(ctx, 4);
        wr16(rdram, obj + 0x1Cu, static_cast<uint16_t>(rdU16(rdram, obj + 0x1Cu) + 1u));
        const int16_t idx = rdS16(rdram, obj + 0x1Eu);
        if (idx != -1)
        {
            const uint32_t entry = lookupTxdEntry(rdram, idx);
            wr16(rdram, entry + 4u, static_cast<uint16_t>(rdU16(rdram, entry + 4u) + 1u));
        }
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookSlotIncCheck(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x001CEA60u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_SlotIncCheck].orig(rdram, ctx, runtime);
            return;
        }
        const int16_t idx = rdS16(rdram, GPR_U32(ctx, 4) + 0x1Eu);
        if (idx != -1)
        {
            const uint32_t entry = lookupTxdEntry(rdram, idx);
            wr16(rdram, entry + 4u, static_cast<uint16_t>(rdU16(rdram, entry + 4u) + 1u));
        }
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookSlotClearCheck(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x001CEA90u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_SlotClearCheck].orig(rdram, ctx, runtime);
            return;
        }
        const int16_t idx = rdS16(rdram, GPR_U32(ctx, 4) + 0x1Eu);
        const uint32_t entry = lookupTxdEntry(rdram, idx);
        const uint16_t c4 = static_cast<uint16_t>(rdU16(rdram, entry + 4u) + 1u);
        const int16_t c6 = rdS16(rdram, entry + 6u);
        wr16(rdram, entry + 4u, c4);
        if (c6 == 0)
        {
            const uint32_t tp = rd32(rdram, entry);
            if (tp != 0u)
                resFlag(rdram, tp - 0x20u, 0u);
        }
        const uint16_t next6 = static_cast<uint16_t>(static_cast<uint16_t>(c6) + 1u);
        wr16(rdram, entry + 6u, next6);
        SET_GPR_U64(ctx, 2, static_cast<uint64_t>(next6));
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookResFlag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ptr = GPR_U32(ctx, 5);
        if (ctx->pc != 0x002BCF88u || watchpointsActive() || (g_verify && t_verifyDepth > 0) ||
            ptr < 8u || !validRam(ptr - 8u, 8u))
        {
            g_hooks[kH_ResFlag].orig(rdram, ctx, runtime);
            return;
        }
        const uint32_t retVal = resFlag(rdram, ptr, GPR_U32(ctx, 6));
        SET_GPR_U64(ctx, 2, static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(retVal))));
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookSkinWrite(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x002A9228u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_SkinWrite].orig(rdram, ctx, runtime);
            return;
        }
        const uint32_t a0 = GPR_U32(ctx, 4);
        wr32(rdram, a0 + 0x18u, 1u);
        wr32(rdram, a0 + 0x0Cu, GPR_U32(ctx, 7) != 0u ? 1u : 0u);
        wr32(rdram, a0 + 0x10u, GPR_U32(ctx, 6));
        wr32(rdram, a0 + 0x04u, GPR_U32(ctx, 5));
        wr32(rdram, a0 + 0x08u, 0u);
        dirtyOr(rdram, 0x10u);
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookMatSetAlpha(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x002F3908u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_MatSetAlpha].orig(rdram, ctx, runtime);
            return;
        }
        matSetAlpha(rdram, GPR_U32(ctx, 4), GPR_U32(ctx, 5));
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookSetModelMtx(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t m = GPR_U32(ctx, 4);
        if (ctx->pc != 0x002F5560u || watchpointsActive() || (g_verify && t_verifyDepth > 0) || !validRam(m, 64u))
        {
            g_hooks[kH_SetModelMtx].orig(rdram, ctx, runtime);
            return;
        }
        setModelMatrix(rdram, reinterpret_cast<const float *>(rdram + (m & kRamMask)));
        SET_GPR_U64(ctx, 2, static_cast<uint64_t>(static_cast<int32_t>(m)));
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookSetBoneScalePtr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t p = GPR_U32(ctx, 4);
        if (ctx->pc != 0x002F55B0u || watchpointsActive() || (g_verify && t_verifyDepth > 0) || !validRam(p, 64u))
        {
            g_hooks[kH_SetBoneScalePtr].orig(rdram, ctx, runtime);
            return;
        }
        setBoneScalePtr(rdram, p);
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookSetBoneScaleDef(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x002F5608u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_SetBoneScaleDef].orig(rdram, ctx, runtime);
            return;
        }
        setBoneScalePtr(rdram, kQuantScaleMirror);
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookSetFrameCtx(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x002F5CD8u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_SetFrameCtx].orig(rdram, ctx, runtime);
            return;
        }
        setFrameCtx(rdram, GPR_U32(ctx, 4));
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookSetPipelineMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x002F5D40u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_SetPipelineMode].orig(rdram, ctx, runtime);
            return;
        }
        setPipelineMode(rdram, GPR_U32(ctx, 4));
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookSetRimFlag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x002F5D68u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_SetRimFlag].orig(rdram, ctx, runtime);
            return;
        }
        setRimFlag(rdram, GPR_U32(ctx, 4));
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookSetClipMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x002F5E30u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_SetClipMode].orig(rdram, ctx, runtime);
            return;
        }
        setClipMode(rdram, GPR_U32(ctx, 4) != 0u);
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookSetAmbPacked(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x002F5EA0u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_SetAmbPacked].orig(rdram, ctx, runtime);
            return;
        }
        setAmbPacked(rdram, GPR_U32(ctx, 4));
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookSetScissorRect(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x002F6088u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_SetScissorRect].orig(rdram, ctx, runtime);
            return;
        }
        emitScissor(rdram, GPR_U32(ctx, 4), GPR_U32(ctx, 5));
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookIsClipNear(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x002F6190u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_IsClipNear].orig(rdram, ctx, runtime);
            return;
        }
        constexpr uint32_t kNearVal = (0x002BC878u - 0x002BC130u) >> 3;
        const uint64_t nearFlag = ((rd32(rdram, kRslClipProgOff) ^ kNearVal) < 1u) ? 1ULL : 0ULL;
        SET_GPR_U64(ctx, 2, nearFlag);
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookSetGsTestBits(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x002F61C0u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_SetGsTestBits].orig(rdram, ctx, runtime);
            return;
        }
        setGsTestBits(rdram, GPR_U32(ctx, 4), GPR_U32(ctx, 5));
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookModGsTestFlag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x002F6268u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_ModGsTestFlag].orig(rdram, ctx, runtime);
            return;
        }
        const uint32_t unit = GPR_U32(ctx, 4), mask = GPR_U32(ctx, 5), en = GPR_U32(ctx, 6);
        uint32_t cur = static_cast<uint32_t>(rd64(rdram, kRslGsTestRegs + (unit << 4)) >> 47);
        if (en != 0u) cur |= mask; else cur &= ~mask;
        setGsTestBits(rdram, unit, cur);
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookSetAlphaTest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x0023E720u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_SetAlphaTest].orig(rdram, ctx, runtime);
            return;
        }
        setAlphaTestBit(rdram, GPR_U32(ctx, 4));
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookSetRimAlpha(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x0023E4C8u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_SetRimAlpha].orig(rdram, ctx, runtime);
            return;
        }
        setRimAlpha(rdram, ctx->f[12]);
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookSetDirty4(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x002F7370u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_SetDirty4].orig(rdram, ctx, runtime);
            return;
        }
        dirtyOr(rdram, 0x10u);
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookSetupViewport(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x002F5A18u || watchpointsActive() || (g_verify && t_verifyDepth > 0) ||
            !validRam(GPR_U32(ctx, 4), 16u))
        {
            g_hooks[kH_SetupViewport].orig(rdram, ctx, runtime);
            return;
        }
        setupViewport(rdram, GPR_U32(ctx, 4), GPR_U32(ctx, 5), ctx->f[12], ctx->f[13]);
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookSetViewportAlpha(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x002F5C38u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_SetViewportAlpha].orig(rdram, ctx, runtime);
            return;
        }
        setViewportAlpha(rdram, ctx->f[12], ctx->f[13]);
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookSetViewportMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x002F5C80u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_SetViewportMode].orig(rdram, ctx, runtime);
            return;
        }
        setViewportMode(rdram, GPR_U32(ctx, 4));
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookEvalLogPoly(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x002F7008u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_EvalLogPoly].orig(rdram, ctx, runtime);
            return;
        }
        ctx->f[0] = evalLogPoly(rdram, ctx->f[12]);
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookSetTex1Fields(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x002F6F58u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_SetTex1Fields].orig(rdram, ctx, runtime);
            return;
        }
        setTex1Fields(rdram, GPR_U32(ctx, 4), GPR_U32(ctx, 5), GPR_U32(ctx, 6), GPR_U32(ctx, 7));
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookEmitTexUpload(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dst = GPR_U32(ctx, 4);
        if (ctx->pc != 0x00266608u || watchpointsActive() || (g_verify && t_verifyDepth > 0) || !validRam(dst, 0x80u))
        {
            g_hooks[kH_EmitTexUpload].orig(rdram, ctx, runtime);
            return;
        }
        emitTexUploadExact(rdram, dst, GPR_U32(ctx, 5), GPR_U32(ctx, 6), GPR_U32(ctx, 7),
                           GPR_U32(ctx, 8), GPR_U32(ctx, 9), GPR_U32(ctx, 10));
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookPktRoom(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x00478628u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_PktRoom].orig(rdram, ctx, runtime);
            return;
        }
        SET_GPR_U64(ctx, 2, static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(pktAvail(rdram, GPR_U32(ctx, 4))))));
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookPktRoomRaw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x00476770u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_PktRoomRaw].orig(rdram, ctx, runtime);
            return;
        }
        SET_GPR_U64(ctx, 2, static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(pktAvailRaw(rdram, GPR_U32(ctx, 4))))));
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookSetTexMipBits(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x0047A850u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_SetTexMipBits].orig(rdram, ctx, runtime);
            return;
        }
        setTexMipBits(rdram, GPR_U32(ctx, 4), GPR_U32(ctx, 5));
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookSysGet(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x00265880u || watchpointsActive() || (g_verify && t_verifyDepth > 0) ||
            rd32(rdram, kRenderInitFlag) == 0u)
        {
            g_hooks[kH_SysGet].orig(rdram, ctx, runtime);
            return;
        }
        SET_GPR_U64(ctx, 2, static_cast<uint64_t>(static_cast<int32_t>(kRenderSysBase)));
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookVuDot4(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x0032E3D0u || watchpointsActive() || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_VuDot4].orig(rdram, ctx, runtime);
            return;
        }
        ctx->f[0] = runVuDot4(rdram, GPR_U32(ctx, 4), GPR_U32(ctx, 5));
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookVuNorm3(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x0032E440u || watchpointsActive() || (g_verify && t_verifyDepth > 0) ||
            !runVuNorm3(rdram, GPR_U32(ctx, 4)))
        {
            g_hooks[kH_VuNorm3].orig(rdram, ctx, runtime);
            return;
        }
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookBuildVpMatrix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t m = GPR_U32(ctx, 4), r = GPR_U32(ctx, 5);
        if (ctx->pc != 0x003F9FA0u || watchpointsActive() || (g_verify && t_verifyDepth > 0) ||
            !validRam(m, 64u) || !validRam(r, 16u))
        {
            g_hooks[kH_BuildVpMatrix].orig(rdram, ctx, runtime);
            return;
        }
        const float half = 0.5f, negHalf = -0.5f;
        const float f22s = ctx->f[13];
        const float f21 = FPU_MUL_S(FPU_ADD_S(ctx->f[12], f22s), half);
        const float f22 = FPU_MUL_S(FPU_SUB_S(f22s, ctx->f[12]), half);
        const float r2 = rdF32(rdram, r + 8u), r3 = rdF32(rdram, r + 12u);
        const float r1 = rdF32(rdram, r + 4u), r0 = rdF32(rdram, r + 0u);
        const float f24 = FPU_MUL_S(r2, half);
        const float f23 = FPU_MUL_S(r3, half);
        const float f20 = FPU_ADD_S(FPU_ADD_S(f24, ctx->f[14]), r0);
        const float f23b = FPU_ADD_S(FPU_ADD_S(f23, ctx->f[15]), r1);
        const float f25 = FPU_MUL_S(r3, negHalf);
        identityMtx(rdram, m);
        wrF32(rdram, m + 0x38u, f21);
        wrF32(rdram, m + 0x00u, f24);
        wrF32(rdram, m + 0x30u, f20);
        wrF32(rdram, m + 0x14u, f25);
        wrF32(rdram, m + 0x34u, f23b);
        wrF32(rdram, m + 0x28u, f22);
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookIdentityMtx(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t m = GPR_U32(ctx, 4);
        if (ctx->pc != 0x003F9E50u || watchpointsActive() || (g_verify && t_verifyDepth > 0) || !validRam(m, 64u))
        {
            g_hooks[kH_IdentityMtx].orig(rdram, ctx, runtime);
            return;
        }
        identityMtx(rdram, m);
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookCopy64(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dst = GPR_U32(ctx, 4), src = GPR_U32(ctx, 5);
        if (ctx->pc != 0x001AA998u || watchpointsActive() || (g_verify && t_verifyDepth > 0) ||
            !validRam(dst & ~0xFu, 64u) || !validRam(src & ~0xFu, 64u))
        {
            g_hooks[kH_Copy64].orig(rdram, ctx, runtime);
            return;
        }
        std::memcpy(rdram + ((dst & ~0xFu) & kRamMask), rdram + ((src & ~0xFu) & kRamMask), 64u);
        SET_GPR_U64(ctx, 2, static_cast<uint64_t>(static_cast<int32_t>(src)));
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookVuSphereClip(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t a1 = GPR_U32(ctx, 5), a2 = GPR_U32(ctx, 6), a3 = GPR_U32(ctx, 7);
        if (ctx->pc != 0x0030C8A8u || watchpointsActive() || (g_verify && t_verifyDepth > 0) ||
            !validRam(a1 & ~0xFu, 16u))
        {
            g_hooks[kH_VuSphereClip].orig(rdram, ctx, runtime);
            return;
        }
        ctx->vu0_vf[1] = _mm_castsi128_ps(READ128(a1 & ~0xFu));
        invokeVu0Prog(rdram, ctx, runtime, GPR_U32(ctx, 4));
        wr32(rdram, a2, static_cast<uint32_t>(ctx->vi[1]) >> 4);
        wr32(rdram, a3, static_cast<uint32_t>(ctx->vi[2]) >> 4);
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookVuBoxClip(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t a1 = GPR_U32(ctx, 5), a2 = GPR_U32(ctx, 6), a3 = GPR_U32(ctx, 7);
        if (ctx->pc != 0x0030C8D4u || watchpointsActive() || (g_verify && t_verifyDepth > 0) ||
            !validRam(a1 & ~0xFu, 32u))
        {
            g_hooks[kH_VuBoxClip].orig(rdram, ctx, runtime);
            return;
        }
        ctx->vu0_vf[1] = _mm_castsi128_ps(READ128(a1 & ~0xFu));
        ctx->vu0_vf[2] = _mm_castsi128_ps(READ128((a1 + 16u) & ~0xFu));
        invokeVu0Prog(rdram, ctx, runtime, GPR_U32(ctx, 4));
        wr32(rdram, a2, static_cast<uint32_t>(ctx->vi[1]) >> 4);
        wr32(rdram, a3, static_cast<uint32_t>(ctx->vi[2]) >> 4);
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookVuStore12(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t a1 = GPR_U32(ctx, 5);
        if (ctx->pc != 0x0030C900u || watchpointsActive() || (g_verify && t_verifyDepth > 0) ||
            !validRam(a1 & ~0xFu, 192u))
        {
            g_hooks[kH_VuStore12].orig(rdram, ctx, runtime);
            return;
        }
        for (uint32_t i = 0u; i < 12u; ++i)
            ctx->vu0_vf[1u + i] = _mm_castsi128_ps(READ128((a1 + (i << 4)) & ~0xFu));
        invokeVu0Prog(rdram, ctx, runtime, GPR_U32(ctx, 4));
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookVuMul4(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t a1 = GPR_U32(ctx, 5);
        if (ctx->pc != 0x0030C940u || watchpointsActive() || (g_verify && t_verifyDepth > 0) ||
            !validRam(a1 & ~0xFu, 64u))
        {
            g_hooks[kH_VuMul4].orig(rdram, ctx, runtime);
            return;
        }
        for (uint32_t i = 0u; i < 4u; ++i)
            ctx->vu0_vf[1u + i] = _mm_castsi128_ps(READ128((a1 + (i << 4)) & ~0xFu));
        invokeVu0Prog(rdram, ctx, runtime, GPR_U32(ctx, 4));
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookVuSetVi15(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x0030C978u || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_VuSetVi15].orig(rdram, ctx, runtime);
            return;
        }
        ctx->vi[15] = static_cast<uint16_t>(GPR_U32(ctx, 4));
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookCvtW(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x0030C984u || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_CvtW].orig(rdram, ctx, runtime);
            return;
        }
        const int32_t v = FPU_CVT_W_S(ctx->f[12]);
        SET_GPR_U64(ctx, 2, static_cast<uint64_t>(static_cast<int64_t>(v)));
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookUvMtxIdentity(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t m = GPR_U32(ctx, 4);
        if (ctx->pc != 0x004037E8u || watchpointsActive() || (g_verify && t_verifyDepth > 0) || !validRam(m, 0x2Cu))
        {
            g_hooks[kH_UvMtxIdentity].orig(rdram, ctx, runtime);
            return;
        }
        wr32(rdram, m + 0x10u, 0u); wrF32(rdram, m + 0x28u, 1.0f); wrF32(rdram, m + 0x00u, 1.0f);
        wr32(rdram, m + 0x20u, 0u); wr32(rdram, m + 0x04u, 0u);   wrF32(rdram, m + 0x14u, 1.0f);
        wr32(rdram, m + 0x24u, 0u); wr32(rdram, m + 0x08u, 0u);   wr32(rdram, m + 0x18u, 0u);
        SET_GPR_U64(ctx, 2, static_cast<uint64_t>(static_cast<int32_t>(m)));
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookUvMtxSet(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t m = GPR_U32(ctx, 4);
        if (ctx->pc != 0x00403848u || watchpointsActive() || (g_verify && t_verifyDepth > 0) || !validRam(m, 0x2Cu))
        {
            g_hooks[kH_UvMtxSet].orig(rdram, ctx, runtime);
            return;
        }
        uvMtxSet(rdram, m, ctx->f[12], ctx->f[13], ctx->f[14], ctx->f[15]);
        ctx->pc = GPR_U32(ctx, 31);
    }

    // Small RenderWare leaf accessors.
    template <HookIdx Idx, uint32_t Addr, uint32_t Off>
    PS2_GUEST_CC void hookReadField32(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t p = GPR_U32(ctx, 4);
        if (ctx->pc != Addr || watchpointsActive() || (g_verify && t_verifyDepth > 0) || !validRam(p, Off + 4u))
        {
            g_hooks[Idx].orig(rdram, ctx, runtime);
            return;
        }
        SET_GPR_U64(ctx, 2, static_cast<uint64_t>(static_cast<int64_t>(rdS32(rdram, p + Off))));
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookMatGetRGBA(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x0042C0E8u || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_MatGetRGBA].orig(rdram, ctx, runtime);
            return;
        }
        SET_GPR_U64(ctx, 2, static_cast<uint64_t>(static_cast<int32_t>(GPR_U32(ctx, 4) + 4u)));
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookMatGetType(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t mat = GPR_U32(ctx, 4);
        if (ctx->pc != 0x0042C0F0u || watchpointsActive() || (g_verify && t_verifyDepth > 0) || !validRam(mat, 0x10u))
        {
            g_hooks[kH_MatGetType].orig(rdram, ctx, runtime);
            return;
        }
        SET_GPR_U64(ctx, 2, static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(matGetType(rdram, mat)))));
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookMatGetFloat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t mat = GPR_U32(ctx, 4);
        if (ctx->pc != 0x0042C2D0u || watchpointsActive() || (g_verify && t_verifyDepth > 0) || !validRam(mat, 0x10u))
        {
            g_hooks[kH_MatGetFloat].orig(rdram, ctx, runtime);
            return;
        }
        ctx->f[0] = rdF32(rdram, rd32(rdram, mat + 0x0Cu) + 8u);
        ctx->pc = GPR_U32(ctx, 31);
    }

    PS2_GUEST_CC void hookNoop2E6218(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != 0x002E6218u || (g_verify && t_verifyDepth > 0))
        {
            g_hooks[kH_Noop2E6218].orig(rdram, ctx, runtime);
            return;
        }
        ctx->pc = GPR_U32(ctx, 31);
    }

    std::array<HookSlot, kH_Count> g_hooks = {{
        {0x003C3E58u, &hookEntityRender, nullptr, nullptr},
        {0x002C0270u, &hookClumpRender, nullptr, nullptr},
        {0x0039EC18u, &hookAtomicRender, nullptr, nullptr},
        {0x0039E828u, &hookStripRender, nullptr, nullptr},
        {0x0039E7F0u, &hookAmbientPack, nullptr, nullptr},
        {0x00408F30u, &hookTexBind, nullptr, nullptr},
        {0x00317FB8u, &hookTexApply, nullptr, nullptr},
        {0x00317F10u, &hookEnvMtx, nullptr, nullptr},
        {0x00317D48u, &hookUvSet, nullptr, nullptr},
        {0x002F62B8u, &hookSetTexture, nullptr, nullptr},
        {0x002F6A78u, &hookSetClut, nullptr, nullptr},
        {0x002F7D58u, &hookDrawModel, nullptr, nullptr},
        {0x002F80A0u, &hookDrawModelB, nullptr, nullptr},
        {0x002F7EB0u, &hookDrawWorld, nullptr, nullptr},
        {0x002F7908u, &hookGsSendA, nullptr, nullptr},
        {0x002F7B30u, &hookGsSendB, nullptr, nullptr},
        {0x002F7090u, &hookSendTexMtx, nullptr, nullptr},
        {0x002F7430u, &hookFrustumSphere, nullptr, nullptr},
        {0x002F7528u, &hookFrustumBox, nullptr, nullptr},
        {0x002F7620u, &hookBuildClipPlanes, nullptr, nullptr},
        {0x001B50B0u, &hookFrameGetLtm, nullptr, nullptr},
        {0x001B5040u, &hookFrameUpdateRoot, nullptr, nullptr},
        {0x003CD678u, &hookTxdSlotTouch, nullptr, nullptr},
        {0x0036DA30u, &hookAtomicGetTxd, nullptr, nullptr},
        {0x001A73E0u, &hookSetupLights, nullptr, nullptr},
        {0x001A7038u, &hookResetLights, nullptr, nullptr},
        {0x001A70B0u, &hookSetAmbientLight, nullptr, nullptr},
        {0x001A7260u, &hookSetDirLight, nullptr, nullptr},
        {0x001CECE8u, &hookSetLightAmbientG, nullptr, nullptr},
        {0x001CED88u, &hookSetLightSpecG, nullptr, nullptr},
        {0x0040F428u, &hookSetLightDirVec, nullptr, nullptr},
        {0x0040F4B8u, &hookSetLightDirRgb, nullptr, nullptr},
        {0x0040F530u, &hookSetLightAmbRgb, nullptr, nullptr},
        {0x0040F5A0u, &hookSetLightUnkRgb, nullptr, nullptr},
        {0x001CEB00u, &hookSlotInc, nullptr, nullptr},
        {0x001CEA60u, &hookSlotIncCheck, nullptr, nullptr},
        {0x001CEA90u, &hookSlotClearCheck, nullptr, nullptr},
        {0x003A5B98u, &hookPoolIncRef, nullptr, nullptr},
        {0x003A5BF0u, &hookPoolRelease, nullptr, nullptr},
        {0x002BCF88u, &hookResFlag, nullptr, nullptr},
        {0x002A9228u, &hookSkinWrite, nullptr, nullptr},
        {0x002F3908u, &hookMatSetAlpha, nullptr, nullptr},
        {0x002F5560u, &hookSetModelMtx, nullptr, nullptr},
        {0x002F55B0u, &hookSetBoneScalePtr, nullptr, nullptr},
        {0x002F5608u, &hookSetBoneScaleDef, nullptr, nullptr},
        {0x002F5CD8u, &hookSetFrameCtx, nullptr, nullptr},
        {0x002F5D40u, &hookSetPipelineMode, nullptr, nullptr},
        {0x002F5D68u, &hookSetRimFlag, nullptr, nullptr},
        {0x002F5E30u, &hookSetClipMode, nullptr, nullptr},
        {0x002F5EA0u, &hookSetAmbPacked, nullptr, nullptr},
        {0x002F6088u, &hookSetScissorRect, nullptr, nullptr},
        {0x002F6190u, &hookIsClipNear, nullptr, nullptr},
        {0x002F61C0u, &hookSetGsTestBits, nullptr, nullptr},
        {0x002F6268u, &hookModGsTestFlag, nullptr, nullptr},
        {0x0023E720u, &hookSetAlphaTest, nullptr, nullptr},
        {0x0023E4C8u, &hookSetRimAlpha, nullptr, nullptr},
        {0x002F7370u, &hookSetDirty4, nullptr, nullptr},
        {0x002F5A18u, &hookSetupViewport, nullptr, nullptr},
        {0x002F5C38u, &hookSetViewportAlpha, nullptr, nullptr},
        {0x002F5C80u, &hookSetViewportMode, nullptr, nullptr},
        {0x002F7008u, &hookEvalLogPoly, nullptr, nullptr},
        {0x002F6F58u, &hookSetTex1Fields, nullptr, nullptr},
        {0x00266608u, &hookEmitTexUpload, nullptr, nullptr},
        {0x00478628u, &hookPktRoom, nullptr, nullptr},
        {0x00476770u, &hookPktRoomRaw, nullptr, nullptr},
        {0x0047A850u, &hookSetTexMipBits, nullptr, nullptr},
        {0x00265880u, &hookSysGet, nullptr, nullptr},
        {0x0032E3D0u, &hookVuDot4, nullptr, nullptr},
        {0x0032E440u, &hookVuNorm3, nullptr, nullptr},
        {0x003F9FA0u, &hookBuildVpMatrix, nullptr, nullptr},
        {0x003F9E50u, &hookIdentityMtx, nullptr, nullptr},
        {0x001AA998u, &hookCopy64, nullptr, nullptr},
        {0x0030C8A8u, &hookVuSphereClip, nullptr, nullptr},
        {0x0030C8D4u, &hookVuBoxClip, nullptr, nullptr},
        {0x0030C900u, &hookVuStore12, nullptr, nullptr},
        {0x0030C940u, &hookVuMul4, nullptr, nullptr},
        {0x0030C978u, &hookVuSetVi15, nullptr, nullptr},
        {0x0030C984u, &hookCvtW, nullptr, nullptr},
        {0x004037E8u, &hookUvMtxIdentity, nullptr, nullptr},
        {0x00403848u, &hookUvMtxSet, nullptr, nullptr},
        {0x0042C0E8u, &hookMatGetRGBA, nullptr, nullptr},
        {0x0042C0F0u, &hookMatGetType, nullptr, nullptr},
        {0x0042C2D0u, &hookMatGetFloat, nullptr, nullptr},
        {0x0024E650u, &hookReadField32<kH_GeomGetSkin, 0x0024E650u, 0x18u>, nullptr, nullptr},
        {0x0024E668u, &hookReadField32<kH_SkinNumBones, 0x0024E668u, 0x00u>, nullptr, nullptr},
        {0x0024E878u, &hookReadField32<kH_HAnimGetLtms, 0x0024E878u, 0x28u>, nullptr, nullptr},
        {0x0024EB90u, &hookReadField32<kH_SkinInvBind, 0x0024EB90u, 0x0Cu>, nullptr, nullptr},
        {0x0024EBB0u, &hookReadField32<kH_AtomicGetHAnim, 0x0024EBB0u, 0x2Cu>, nullptr, nullptr},
        {0x002E6218u, &hookNoop2E6218, nullptr, nullptr},
    }};
}

namespace vcs::rslrender
{
    void install(PS2Runtime &runtime)
    {
        const char *env = std::getenv("VCS_NATIVE_RSL_RENDER");
        if (env && *env == '0')
            return;
        const char *ver = std::getenv("VCS_NATIVE_RSL_RENDER_VERIFY");
        g_verify = ver != nullptr && *ver != '\0' && *ver != '0';

        for (auto &h : g_hooks)
        {
            h.orig = runtime.lookupFunction(h.addr);
            if (h.orig)
                runtime.replaceFunction(h.addr, h.hook);
        }
    }
}
