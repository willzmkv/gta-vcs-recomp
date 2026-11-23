// See vcs_footcam.h and game-internals.md 12b ("On foot: the stick, the camera on its string, walls").
#include "game/vcs_footcam.h"

#include "game/vcs_freecam.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_control.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vcs::footcam
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        constexpr uint32_t kFollowPed = 0x0029DAB0u;     // CCam::FollowPed (mode 4): a0 CCam, a1 target pos
        constexpr uint32_t kTheCamera = 0x006F44D0u;
        constexpr uint32_t kActiveCam = kTheCamera + 0x50u;
        constexpr uint32_t kCams = kTheCamera + 0x70u, kCamSize = 0x270u;
        constexpr uint32_t kBehindPlayer = kTheCamera + 0x58u;
        constexpr uint32_t kInFrontOfPlayer = kTheCamera + 0x858u;
        constexpr uint32_t kLooksAtPlayer = kTheCamera + 0x888u;
        constexpr uint32_t kCamTargetEntity = 0x70u, kCamRestart = 0xACu;
        constexpr uint32_t kCamPitchSpeed = 0x130u, kCamHeadingSpeed = 0x140u;
        constexpr uint32_t kCamPoints[2] = {0x220u, 0x230u};
        constexpr uint32_t kModeFollowPed = 4;
        constexpr uint32_t kTable = 0x00487B30u, kTableRow = 0x3Cu, kSwingCap = 0x2Cu;
        constexpr uint32_t kCentring = 0x00487E10u;
        constexpr uint32_t kCamStickLocked = 0x00488EB4u;
        constexpr uint32_t kPad0 = 0x005147A8u; // +0xA L1, +0x96 DisablePlayerControls
        constexpr uint32_t kPlayerPed = 0x004E4910u;
        constexpr uint32_t kFrameCounter = 0x004CD124u;
        constexpr uint32_t kUserPause = 0x004CD2ACu, kCodePause = 0x004CD11Cu;

        uint8_t *g_rdram = nullptr;
        PS2Runtime::RecompiledFunction g_origCamera = nullptr;
        std::atomic<bool> g_installed{false};

        // Game thread state
        std::atomic<bool> g_enabled{true};
        std::atomic<bool> g_stick{false};

        uint32_t g_frame = 0;
        float g_lastTarget[3] = {};
        bool g_swingOff = false;
        float g_swingCaps[2] = {};

        uint32_t rd32(uint32_t a)
        {
            uint32_t v = 0;
            std::memcpy(&v, g_rdram + (a & kRamMask), 4);
            return v;
        }
        int16_t rd16(uint32_t a)
        {
            int16_t v = 0;
            std::memcpy(&v, g_rdram + (a & kRamMask), 2);
            return v;
        }
        float rdf(uint32_t a)
        {
            float v = 0.0f;
            std::memcpy(&v, g_rdram + (a & kRamMask), 4);
            return v;
        }
        void wr32(uint32_t a, uint32_t v) { std::memcpy(g_rdram + (a & kRamMask), &v, 4); }
        void wrf(uint32_t a, float v) { std::memcpy(g_rdram + (a & kRamMask), &v, 4); }

        void swingOff(bool off)
        {
            if (off == g_swingOff)
                return;
            for (int i = 0; i < 2; ++i)
            {
                const uint32_t at = kTable + i * kTableRow + kSwingCap;
                if (off)
                    g_swingCaps[i] = rdf(at);
                wrf(at, off ? 0.0f : g_swingCaps[i]);
            }
            g_swingOff = off;
        }

        void prepare(uint32_t cam, uint32_t target)
        {
            const bool stick = g_stick.exchange(false, std::memory_order_relaxed);
            if (freecam::flying())
            {
                swingOff(false);
                return;
            }

            const uint32_t frame = rd32(kFrameCounter);
            const bool gap = frame - g_frame > 1;
            g_frame = frame;

            float moved[3] = {};
            for (int i = 0; i < 3; ++i)
            {
                const float p = rdf(target + i * 4);
                moved[i] = gap ? 0.0f : p - g_lastTarget[i];
                g_lastTarget[i] = p;
            }

            const uint32_t ped = rd32(kPlayerPed);
            if (cam != kCams + g_rdram[kActiveCam] * kCamSize ||
                (rd32(cam) & 0xFFFFu) != kModeFollowPed ||
                !ped || rd32(cam + kCamTargetEntity) != ped ||
                rd32(cam + kCamRestart) != 0 ||
                rd32(kBehindPlayer) != 0 || rd32(kInFrontOfPlayer) != 0 ||
                rd32(kLooksAtPlayer) != 1 ||
                !g_enabled.load(std::memory_order_relaxed) ||
                rd16(kPad0 + 0x96u) != 0 ||
                rd32(kCamStickLocked) != 0 || g_rdram[kUserPause] ||
                g_rdram[kCodePause] || rd16(kPad0 + 0xAu) != 0 || stick || gap)
            {
                swingOff(false);
                return;
            }

            // Translate the camera's source points alongside Vic and suppress PS2 auto-swing so
            // sideways/backward keys (A/D/S) walk cleanly without pulling the camera into circles.
            swingOff(true);
            for (uint32_t off : kCamPoints)
                for (int i = 0; i < 3; ++i)
                    wrf(cam + off + i * 4, rdf(cam + off + i * 4) + moved[i]);
            wrf(cam + kCamHeadingSpeed, 0.0f);
            wrf(cam + kCamPitchSpeed, 0.0f);
            wr32(kCentring, 0);
        }

        PS2_GUEST_CC void followPed(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            prepare(GPR_U32(ctx, 4), GPR_U32(ctx, 5));
            g_origCamera(rdram, ctx, runtime);
        }
    }

    void install(PS2Runtime &runtime)
    {
        g_rdram = runtime.memory().getRDRAM();
        if (const char *e = std::getenv("VCS_FOOTCAM"); e && e[0] == '0')
        {
            std::fprintf(stderr, "[footcam] off (VCS_FOOTCAM=0)\n");
            return;
        }
        g_origCamera = runtime.lookupFunction(kFollowPed);
        if (!g_origCamera)
        {
            std::fprintf(stderr, "[footcam] not installed: FollowPed missing\n");
            return;
        }
        runtime.replaceFunction(kFollowPed, &followPed);
        g_installed = true;
        ps2control::addCommand("footcam", [](const std::string &, ps2control::Reply reply) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "ok enabled=%d swingoff=%d",
                          g_enabled.load(std::memory_order_relaxed) ? 1 : 0,
                          g_swingOff ? 1 : 0);
            reply(buf);
        });
    }

    void enable(bool on) { g_enabled.store(on, std::memory_order_relaxed); }
    void stickMoved() { g_stick.store(true, std::memory_order_relaxed); }
}
