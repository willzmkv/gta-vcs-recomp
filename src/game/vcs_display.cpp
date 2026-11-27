// VCS display settings applied to the game (docs/SETTINGS_MENU_WIDESCREEN.md).
//
// Widescreen is Hor+: FUN_001ab9e0 (sets the camera's view window: x = tan(FOV / 2), y = x / aspect, then
// RwCameraSetViewWindow) is wrapped so the horizontal extent grows by r = display aspect / (4:3) while the vertical
// one stays (both arguments times r). RenderWare derives the frustum (EE culling) and the VU matrices from the view
// window, so the 3D, the sector culling and both renderers follow; the picture (still 640 wide) is presented at the
// wider aspect and the renderer keeps the HUD at 4:3 (gsHwSetHudSqueeze).
//
// Entities are culled by the game's own camera planes, which do not come from the view window:
// CCamera::IsSphereVisible (FUN_003816F8) tests camera-space side planes that FUN_0037b2c8 builds from the FOV global
// alone (right (cos h, -sin h) at cam+0xB60, left (-cos h, -sin h) at +0xB70, h = FOV / 2: the 4:3 view). Without a
// fix buildings, peds and cars at the screen edges vanish in widescreen. The test is wrapped: before it runs, side
// planes the game has rebuilt are widened to the Hor+ view (edge at tan h x r). VCS_WIDE_CULL=0: the game's planes.
//
// The game's own widescreen preference (pause menu Display, option 14; *(*0x48F3D0 + 0x30)) is the PS2's anamorphic
// mode for a TV that stretches the picture: a Vert- 16:9 aspect, the HUD, text, pause map and menu layouts squeezed to
// 3/4 width, and no cutscene letterbox (game-internals.md 5a). On a PC every part of it but the bars fights the Hor+
// view and the renderer's HUD squeeze, so it is kept off (the user's decision, DECISIONS.md 2026-11-06): its setter
// FUN_002AF660 stores 0 - the pause menu's row is hidden (vcs_frontend.cpp) and a save's 1 (block +0x80, restored
// at 0x3AD0F8) is not taken - and the frame kick puts 0 back if anything wrote the field directly. The bars are a
// setting of their own (Display > Cutscene black bars): FUN_002804C8, which draws only them, is skipped when off.
// VCS_WIDESCREEN=1 forces the PS2's mode on (comparisons), VCS_WIDESCREEN=game leaves it to the game (before).
#include "game/vcs_display.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/gs_hw.h"
#include <ps2_recompiled_functions.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern void ps2PushCheckpointDefer() noexcept;
extern void ps2PopCheckpointDefer() noexcept;

namespace vcs
{
    namespace
    {
        class CheckpointDeferGuard
        {
        public:
            CheckpointDeferGuard() noexcept { ps2PushCheckpointDefer(); }
            ~CheckpointDeferGuard() noexcept { ps2PopCheckpointDefer(); }
            CheckpointDeferGuard(const CheckpointDeferGuard &) = delete;
            CheckpointDeferGuard &operator=(const CheckpointDeferGuard &) = delete;
        };

        constexpr uint32_t kPrefsPointer = 0x0048F3D0u; // -> the game's preferences / menu object
        constexpr uint32_t kWidescreenField = 0x30u;    // int: 1 = the game's (Vert-) widescreen; FUN_002af658 reads it
        constexpr uint32_t kRamMask = PS2_RAM_MASK;

        constexpr uint32_t kRightPlane = 0xB60u; // CCamera: camera-space frustum normals, x right, y forward, z up
        constexpr uint32_t kLeftPlane = 0xB70u;

        std::atomic<float> g_widening{1.0f};
        std::atomic<int> g_gamePreference{0}; // the value the game's preference is kept at; -1: left to the game
        std::atomic<bool> g_cutsceneBars{true};
        bool g_wideCull = true;

        // Per camera object, its side planes as the game built them and as they were widened (game thread only: the
        // camera's FUN_0037b2c8 and every IsSphereVisible run there). In practice one camera (TheCamera, 0x6F44D0).
        struct SidePlanes
        {
            uint32_t camera = 0;
            float r = 1.0f;
            float game[4] = {}; // right x, y, left x, y
            float wide[4] = {};
        };
        SidePlanes g_planes[4];
        unsigned g_nextPlanes = 0;

        uint32_t load32(const uint8_t *rdram, uint32_t addr)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (addr & kRamMask), 4);
            return v;
        }

        void store32(uint8_t *rdram, uint32_t addr, uint32_t v) { std::memcpy(rdram + (addr & kRamMask), &v, 4); }

        // Cutscene framing (Display > Screen): while the camera's cinematic widescreen is on (m_WideScreenOn, TheCamera +
        // 0x844: cutscenes and the scripts' SWITCH_WIDESCREEN; the game's aspect then 1.25), the PS2 showed the 4:3 width
        // with bars over the top and bottom, and scenes keep actors just outside that width until they walk in - Hor+
        // showed them waiting at the edges (Soldier: Vic standing at the right of the jet, the player's report,
        // 2026-11-18). PS2 framing keeps the 4:3 width and takes the height from the display aspect (Vert-: a 16:9
        // picture of what the bars left, square pixels), and the bars are not drawn over it.
        constexpr uint32_t kCinematic = 0x006F4D14u;
        std::atomic<bool> g_cutsceneFraming{true};
        bool ps2Framing(const uint8_t *rdram, float r)
        {
            return g_cutsceneFraming.load(std::memory_order_relaxed) && r > 1.0001f && load32(rdram, kCinematic) != 0;
        }

        // FUN_001ab9e0(camera a0, raster rect a1 (null: 640x448), tan(FOV / 2) f12, aspect f13)
        PS2_GUEST_CC void setViewWindow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const float r = g_widening.load(std::memory_order_relaxed);
            if (ps2Framing(rdram, r))
                ctx->f[13] = (4.0f / 3.0f) * r; // the 4:3 width (f12 as it is), the display's height
            else if (r > 1.0001f || r < 0.9999f)
            {
                ctx->f[12] *= r; // horizontal extent x r
                ctx->f[13] *= r; // vertical extent (x / aspect) unchanged
            }
            FUN_001ab9e0_0x1ab9e0(rdram, ctx, runtime);
        }

        float loadFloat(const uint8_t *rdram, uint32_t addr)
        {
            float v;
            std::memcpy(&v, rdram + (addr & kRamMask), 4);
            return v;
        }

        void storeFloat(uint8_t *rdram, uint32_t addr, float v) { std::memcpy(rdram + (addr & kRamMask), &v, 4); }

        // A side plane (a, b) = (+-cos h, -sin h) has its edge at x / y = tan h; the Hor+ edge is at tan h x r:
        // (a, r b), normalised (the test compares the distance with the sphere's radius).
        void widenPlane(const float *game, float r, float *wide)
        {
            const float a = game[0], b = game[1] * r;
            const float len = std::sqrt(a * a + b * b);
            wide[0] = len > 0.0f ? a / len : game[0];
            wide[1] = len > 0.0f ? b / len : game[1];
        }
    }

    void prepareSphereTest(uint8_t *rdram, uint32_t camera)
    {
        if (!g_wideCull)
            return;
        const float r = g_widening.load(std::memory_order_relaxed);
        SidePlanes *found = nullptr;
        for (SidePlanes &e : g_planes)
            if (e.camera == camera)
                found = &e;
        if (r == 1.0f && (!found || found->r == 1.0f))
            return; // 4:3: the game's planes
        float now[4] = {loadFloat(rdram, camera + kRightPlane), loadFloat(rdram, camera + kRightPlane + 4),
                        loadFloat(rdram, camera + kLeftPlane), loadFloat(rdram, camera + kLeftPlane + 4)};
        const bool ours = found && std::memcmp(now, found->wide, sizeof(now)) == 0;
        if (ours && found->r == r)
            return; // widened already (the game rebuilds them in FUN_0037b2c8, once per camera update)
        SidePlanes &p = found ? *found : g_planes[g_nextPlanes++ % 4u];
        if (!ours)
            std::memcpy(p.game, now, sizeof(now)); // rebuilt by the game: the 4:3 planes
        // else: the ratio changed since they were widened (r = 1: the game's planes go back)
        p.camera = camera;
        p.r = r;
        if (r == 1.0f)
            std::memcpy(p.wide, p.game, sizeof(p.wide));
        else
        {
            widenPlane(p.game, r, p.wide);
            widenPlane(p.game + 2, r, p.wide + 2);
        }
        storeFloat(rdram, camera + kRightPlane, p.wide[0]);
        storeFloat(rdram, camera + kRightPlane + 4, p.wide[1]);
        storeFloat(rdram, camera + kLeftPlane, p.wide[2]);
        storeFloat(rdram, camera + kLeftPlane + 4, p.wide[3]);
    }

    namespace
    {
        // CCamera::IsSphereVisible(camera a0, centre a1, camera matrix a2, radius f12): a leaf
        PS2_GUEST_CC void sphereVisible(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            prepareSphereTest(rdram, GPR_U32(ctx, 4));
            FUN_003816f8_0x3816f8(rdram, ctx, runtime);
        }

        // FUN_002AF660(prefs a0, value a1): the widescreen preference's setter (a leaf: sw a1, 0x30(a0)). Callers: the
        // pause menu's Display page (0x2AF06C) and the save load (0x3AD0F8).
        PS2_GUEST_CC void setWidescreenPreference(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const int wanted = g_gamePreference.load(std::memory_order_relaxed);
            if (wanted >= 0 && static_cast<int>(GPR_U32(ctx, 5)) != wanted)
            {
                static bool logged = false;
                if (!logged)
                    std::fprintf(stderr, "[display] the game sets its widescreen preference %u (ra=0x%x): kept at %d\n",
                                 GPR_U32(ctx, 5), GPR_U32(ctx, 31), wanted);
                logged = true;
                SET_GPR_U32(ctx, 5, static_cast<uint32_t>(wanted));
            }
            FUN_002af660_0x2af660(rdram, ctx, runtime);
        }

        PS2Runtime::RecompiledFunction g_origCloudsFogReset = nullptr;
        PS2Runtime::RecompiledFunction g_origCalcScreenCoors = nullptr;
        PS2Runtime::RecompiledFunction g_origRwRenderStateSet = nullptr;
        PS2Runtime::RecompiledFunction g_origRenderOneXluSprite = nullptr;
        bool g_moonFix = true;

        uint16_t load16(const uint8_t *rdram, uint32_t addr)
        {
            uint16_t v;
            std::memcpy(&v, rdram + (addr & kRamMask), 2);
            return v;
        }

        void storeBytes(uint8_t *rdram, uint32_t addr, const void *src, size_t n)
        {
            std::memcpy(rdram + (addr & kRamMask), src, n);
        }

        // Fixed Moon pass for CClouds::Render (0x0018CE80):
        // 1) Vanilla skips Moon whenever |Hours*60+Min+Sec/60 - 180| >= 180 (missing from dusk until 00:01 and very dim
        //    near midnight), and sets alpha=0 whenever CloudCoverage==1.0 (e.g. clear-looking cloudy night weather=1).
        //    Draw across night hours (fade in 19:30..20:30, peak 180 across 20:30..05:00, fade out 05:00..06:00),
        //    damping by fog/rain while keeping clouds semi-transparent (1 - 0.55*cloud).
        // 2) Normalize halfW for the actual display aspect ratio so Hor+ widescreen keeps a 1:1 circular Moon.
        // 3) Bracket guest sub-calls with CheckpointDeferGuard and match unique return PCs (0x0018CF4C / 0x0018D1AC)
        //    rather than a thread-local bool, preventing EE scheduler unwinds from leaking a stretched vanilla Moon.
        void drawFixedMoon(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!g_origCalcScreenCoors || !g_origRwRenderStateSet || !g_origRenderOneXluSprite)
                return;
            const uint8_t hours = rdram[0x004CD148u & kRamMask];
            const uint8_t mins = rdram[0x004CD149u & kRamMask];
            const uint16_t secs = load16(rdram, 0x004CD2C4u);
            const float t = static_cast<float>(hours) * 60.0f + static_cast<float>(mins) + static_cast<float>(secs) / 60.0f;
            float timeFrac = 0.0f;
            if (t >= 1170.0f && t < 1230.0f)
                timeFrac = (t - 1170.0f) / 60.0f;
            else if (t >= 1230.0f || t <= 300.0f)
                timeFrac = 1.0f;
            else if (t > 300.0f && t < 360.0f)
                timeFrac = (360.0f - t) / 60.0f;
            if (timeFrac <= 0.0f)
                return;

            const float fog = loadFloat(rdram, 0x004CD14Cu);
            const float rain = loadFloat(rdram, 0x004CD144u);
            const float cloud = loadFloat(rdram, 0x004CD2C8u);
            const float weatherFade = std::clamp(1.0f - std::max(fog, rain * 0.8f), 0.0f, 1.0f) *
                                      (1.0f - 0.55f * std::clamp(cloud, 0.0f, 1.0f));
            const int alpha = std::clamp(static_cast<int>(timeFrac * 180.0f * weatherFade), 0, 255);
            if (alpha <= 0)
                return;

            const uint32_t sp = (GPR_U32(ctx, 29) - 64u) & ~0xFu;
            if (sp < 0x00100000u || (sp & kRamMask) + 64u > PS2_RAM_SIZE)
                return;

            CheckpointDeferGuard deferGuard;

            const float moonPos[3] = {loadFloat(rdram, 0x006F4500u), loadFloat(rdram, 0x006F4504u) - 100.0f,
                                      loadFloat(rdram, 0x006F4508u) + 15.0f};
            const float zeros[5] = {};
            storeBytes(rdram, sp, moonPos, sizeof(moonPos));
            storeBytes(rdram, sp + 16u, zeros, 20u);

            SET_GPR_U64(ctx, 4, sp);
            SET_GPR_U64(ctx, 5, sp + 16u);
            SET_GPR_U64(ctx, 6, sp + 32u);
            SET_GPR_U64(ctx, 7, sp + 36u);
            SET_GPR_U64(ctx, 8, 0u);
            SET_GPR_U32(ctx, 29, sp);
            SET_GPR_U32(ctx, 31, 0x0018CF4Cu);
            ctx->pc = 0x0039FC48u;
            g_origCalcScreenCoors(rdram, ctx, runtime);
            if (GPR_S32(ctx, 2) == 0)
                return;

            const float sx = loadFloat(rdram, sp + 16u);
            const float sy = loadFloat(rdram, sp + 20u);
            const float sz = loadFloat(rdram, sp + 24u);
            const float outH = loadFloat(rdram, sp + 36u);
            if (!(sz > 0.0f) || !(outH > 0.0f))
                return;

            const uint32_t texPtr = load32(rdram, 0x00487760u);
            if (texPtr < 0x00100000u || (texPtr & kRamMask) + 4u > PS2_RAM_SIZE)
                return;
            const uint32_t tex = load32(rdram, texPtr);
            if (tex == 0u)
                return;

            SET_GPR_U64(ctx, 4, 1u);
            SET_GPR_U64(ctx, 5, tex);
            SET_GPR_U32(ctx, 29, sp);
            SET_GPR_U32(ctx, 31, 0x0018CF4Cu);
            ctx->pc = 0x00175198u;
            g_origRwRenderStateSet(rdram, ctx, runtime);

            const uint32_t moonSize = load32(rdram, 0x004878B4u);
            const uint8_t activeCam = rdram[0x006F4520u & kRamMask];
            const float fov = loadFloat(rdram, 0x006F44D0u + static_cast<uint32_t>(activeCam) * 0x270u + 0x1A8u);
            const float sizeBase = static_cast<float>(moonSize) * 2.0f + 4.0f;
            const float halfH = sizeBase * outH * fov;
            const float wid = std::clamp(g_widening.load(std::memory_order_relaxed), 0.5f, 4.0f);
            const float displayAspect = (4.0f / 3.0f) * wid;
            const float halfW = halfH * (640.0f / 448.0f) / displayAspect;

            ctx->f[12] = sx;
            ctx->f[13] = sy;
            ctx->f[14] = sz;
            ctx->f[15] = halfW;
            ctx->f[16] = halfH;
            ctx->f[17] = 1.0f / sz;
            SET_GPR_U64(ctx, 4, static_cast<uint64_t>(alpha));
            SET_GPR_U64(ctx, 5, static_cast<uint64_t>(alpha));
            SET_GPR_U64(ctx, 6, static_cast<uint64_t>(alpha));
            SET_GPR_U64(ctx, 7, 255u);
            SET_GPR_U64(ctx, 8, 255u);
            SET_GPR_U32(ctx, 29, sp);
            SET_GPR_U32(ctx, 31, 0x0018CF4Cu);
            ctx->pc = 0x003A03A8u;
            g_origRenderOneXluSprite(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void cloudsFogReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = GPR_U32(ctx, 31);
            const bool fromClouds = g_moonFix && ra == 0x0018CF4Cu;
            g_origCloudsFogReset(rdram, ctx, runtime);
            if (fromClouds)
            {
                const R5900Context saved = *ctx;
                drawFixedMoon(rdram, ctx, runtime);
                *ctx = saved;
                ctx->pc = ra;
            }
        }

        PS2_GUEST_CC void renderOneXluSprite(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            // Skip vanilla Moon call inside CClouds::Render (drawn by drawFixedMoon instead),
            // matched strictly by unique return address 0x0018D1AC so scheduler unwinds cannot bypass it.
            if (g_moonFix && GPR_U32(ctx, 31) == 0x0018D1ACu)
            {
                ctx->pc = 0x0018D1ACu;
                return;
            }
            g_origRenderOneXluSprite(rdram, ctx, runtime);
        }

        // FUN_002804C8(cutscene manager a0): the cutscene letterbox - two black bars, 0..34 and 238..272 of the 2D pass's
        // 272, while the manager's +0x34 is set and the widescreen preference is off. Draws nothing else.
        PS2_GUEST_CC void cutsceneBars(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!g_cutsceneBars.load(std::memory_order_relaxed) || ps2Framing(rdram, g_widening.load(std::memory_order_relaxed)))
            {
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
            FUN_002804c8_0x2804c8(rdram, ctx, runtime);
        }
    }

    void setCutsceneBars(bool on) { g_cutsceneBars.store(on, std::memory_order_relaxed); }

    void setCutsceneFraming(bool ps2) { g_cutsceneFraming.store(ps2, std::memory_order_relaxed); }

    void setAspectWidening(float r) { g_widening.store(r, std::memory_order_relaxed); }

    float aspectWidening() { return g_widening.load(std::memory_order_relaxed); }

    // The reflection map is the 128x128 copy of the frame into FBP 420 that car paint samples (game-internals.md 6g): a
    // flat panel shows one of its texels, so the GS's four taps out of ~5 x 3.5 frame pixels make the panel jump as the
    // camera moves. Smooth reads the copy through the renderer's filter (gsHwSetTargetFilter); the bloom at FBP 420 is
    // 256 wide (FBW 4), so only the reflection map's draws are filtered.
    void setReflectionFilter(int mode) { gsHwSetTargetFilter(420u, 2u, mode == 1 ? 1 : mode == 2 ? 2 : 0); }

    void installDisplayHooks(PS2Runtime &runtime)
    {
        runtime.replaceFunction(0x001AB9E0u, &setViewWindow);
        if (const char *e = std::getenv("VCS_WIDE_CULL"); e && *e == '0')
            g_wideCull = false;
        runtime.replaceFunction(0x003816F8u, &sphereVisible); // VCS_TRACE_VISIBLE replaces it again, calling prepareSphereTest
        if (const char *e = std::getenv("VCS_WIDESCREEN"); e && *e) // testing: the PS2's own (Vert-, anamorphic) mode
            g_gamePreference.store(std::strcmp(e, "game") == 0 ? -1 : *e == '0' ? 0 : 1, std::memory_order_relaxed);
        if (g_gamePreference.load(std::memory_order_relaxed) >= 0)
            runtime.replaceFunction(0x002AF660u, &setWidescreenPreference);
        runtime.replaceFunction(0x002804C8u, &cutsceneBars);
        if (const char *e = std::getenv("VCS_MOON_FIX"); e && *e == '0')
            g_moonFix = false;
        if (g_moonFix)
        {
            g_origCloudsFogReset = runtime.lookupFunction(0x0039FED8u);
            g_origCalcScreenCoors = runtime.lookupFunction(0x0039FC48u);
            g_origRwRenderStateSet = runtime.lookupFunction(0x00175198u);
            g_origRenderOneXluSprite = runtime.lookupFunction(0x003A03A8u);
            if (g_origCloudsFogReset && g_origCalcScreenCoors &&
                g_origRwRenderStateSet && g_origRenderOneXluSprite)
            {
                runtime.replaceFunction(0x0039FED8u, &cloudsFogReset);
                runtime.replaceFunction(0x003A03A8u, &renderOneXluSprite);
            }
        }
        // VCS's render targets by what they are (game-internals.md 6g; docs/FULLY_NATIVE.md step 3d). The reflection map
        // is only ever sampled (vehicles' paint), so it follows "Internal resolution: Match window" like the picture.
        gsHwSetTargetRole(140u, 10u, "frame", false);
        gsHwSetTargetRole(0u, 10u, "shown picture (the previous frame)", false);
        gsHwSetTargetRole(280u, 10u, "Z buffer", false);
        gsHwSetTargetRole(420u, 2u, "reflection map (128x128 sample of the frame + sun glow)", true);
        gsHwSetTargetRole(420u, 4u, "bloom", false);
        gsHwSetTargetRole(452u, 4u, "bloom (second buffer)", false);
        gsHwSetTargetRole(384u, 8u, "trails", false);
        // The flip (FUN_003706c8) reads GS CSR at 0x370714 for the FIELD bit alone ($s4 = (CSR >> 13) & 1, the rest of
        // the value is overwritten in the next delay slot). FIELD is set at VBlank on the game thread: no need to wait
        // for the render worker to finish the frame's GS work (that fence was ~25% of the game thread uncapped,
        // game-internals.md 6). VCS_CSR_FENCE=1: fence as before.
        if (!std::getenv("VCS_CSR_FENCE"))
            runtime.addCsrReadWithoutFence(0x00370714u);
    }

    void applyDisplay(uint8_t *rdram)
    {
        const int wanted = g_gamePreference.load(std::memory_order_relaxed);
        if (wanted < 0)
            return;
        const uint32_t prefs = load32(rdram, kPrefsPointer);
        if (prefs == 0u || (prefs & kRamMask) + kWidescreenField + 4u > PS2_RAM_SIZE)
            return; // the game has not created its preferences yet
        if (static_cast<int>(load32(rdram, prefs + kWidescreenField)) != wanted)
            store32(rdram, prefs + kWidescreenField, static_cast<uint32_t>(wanted)); // FUN_002653f0 picks the aspect up
    }
}
