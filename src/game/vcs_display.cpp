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
// The game's own widescreen preference is Vert- (same horizontal view, vertical view cut) and stays as the game has
// it; VCS_WIDESCREEN=0|1 forces it (testing only).
#include "game/vcs_display.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/gs_hw.h"
#include <ps2_recompiled_functions.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vcs
{
    namespace
    {
        constexpr uint32_t kPrefsPointer = 0x0048F3D0u; // -> the game's preferences / menu object
        constexpr uint32_t kWidescreenField = 0x30u;    // int: 1 = the game's (Vert-) widescreen; FUN_002af658 reads it
        constexpr uint32_t kRamMask = PS2_RAM_MASK;

        constexpr uint32_t kRightPlane = 0xB60u; // CCamera: camera-space frustum normals, x right, y forward, z up
        constexpr uint32_t kLeftPlane = 0xB70u;

        std::atomic<float> g_widening{1.0f};
        std::atomic<int> g_gamePreference{-1}; // -1: leave the game's preference alone
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

        // FUN_001ab9e0(camera a0, raster rect a1 (null: 640x448), tan(FOV / 2) f12, aspect f13)
        PS2_GUEST_CC void setViewWindow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const float r = g_widening.load(std::memory_order_relaxed);
            if (r > 1.0001f || r < 0.9999f)
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
    }

    void setAspectWidening(float r) { g_widening.store(r, std::memory_order_relaxed); }

    float aspectWidening() { return g_widening.load(std::memory_order_relaxed); }

    void installDisplayHooks(PS2Runtime &runtime)
    {
        runtime.replaceFunction(0x001AB9E0u, &setViewWindow);
        if (const char *e = std::getenv("VCS_WIDE_CULL"); e && *e == '0')
            g_wideCull = false;
        runtime.replaceFunction(0x003816F8u, &sphereVisible); // VCS_TRACE_VISIBLE replaces it again, calling prepareSphereTest
        if (const char *e = std::getenv("VCS_WIDESCREEN"); e && *e) // testing: force the game's own (Vert-) preference
            g_gamePreference.store(*e == '0' ? 0 : 1, std::memory_order_relaxed);
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
