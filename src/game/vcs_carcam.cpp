// See vcs_carcam.h and game-internals.md 12b ("The camera behind a vehicle").
#include "game/vcs_carcam.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_control.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace vcs::carcam
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        constexpr uint32_t kFollowCar = 0x0029FAF0u; // the camera behind a vehicle (modes 18 / 22), a0 the CCam
        constexpr uint32_t kTheCamera = 0x006F44D0u;
        constexpr uint32_t kActiveCam = kTheCamera + 0x50u; // byte: which of Cams[3] is active
        constexpr uint32_t kCams = kTheCamera + 0x70u, kCamSize = 0x270u;
        // the CCam: +0 mode, +0x70 the entity it follows, +0x74 3 while the player may turn it (the stick counts only
        // then), +0x78 alpha (pitch), +0x7C beta (heading), +0x130 / +0x140 their speeds, +0x1A0 the point it looked at,
        // +0x220 / +0x230 where it stands (the heading and pitch are measured from these at the next update)
        constexpr uint32_t kCamTargetEntity = 0x70u, kCamControl = 0x74u, kCamAlpha = 0x78u, kCamBeta = 0x7Cu;
        constexpr uint32_t kCamPitchSpeed = 0x130u, kCamHeadingSpeed = 0x140u, kCamTarget = 0x1A0u;
        constexpr uint32_t kCamPoints[2] = {0x220u, 0x230u};
        constexpr uint32_t kLookBehind = 0x006F4D4Cu;  // set: the heading is taken from Front (looking behind)
        constexpr uint32_t kLastAlpha = 0x00487E88u;   // last update's alpha / beta: a change below a small step is
        constexpr uint32_t kLastBeta = 0x00487E8Cu;    // undone (snapped back to these)
        // the camera's tuning per vehicle class (9 rows of 0x3C from 0x487BE8): +0x2C the cap of its turn toward where
        // the vehicle drives, a step
        constexpr uint32_t kPullCap = 0x00487C14u, kTuningRow = 0x3Cu;
        constexpr int kClasses = 9;
        constexpr uint32_t kTimeStep = 0x004CD110u; // CTimer::ms_fTimeStep (1.0 = 1/50 s)
        constexpr uint32_t kFrameCounter = 0x004CD124u; // CTimer::m_FrameCounter
        constexpr uint32_t kEntityPos = 0x30u, kEntityForward = 0x10u, kMoveSpeed = 0x140u;
        constexpr uint32_t kVehicleType = 0x354u;   // 4: a boat (the camera follows its nose, not where it moves)

        constexpr float kTwoPi = 6.2831855f;
        constexpr float kReturnTime = 0.6f;  // the way back eases out: 1 - e^(-t / this) of what is left (95% in 1.8 s)
        constexpr float kReturnRate = 2.0f;  // ... at most this many radians a second
        constexpr float kReturnLimit = 4.0f; // seconds; the game's own camera has it after that in any case

        uint8_t *g_rdram = nullptr;
        PS2Runtime::RecompiledFunction g_orig = nullptr;
        std::atomic<bool> g_installed{false};
        std::atomic<float> g_delay{1.5f};

        // game thread
        enum class State
        {
            Idle,   // the game's camera
            Hold,   // the mouse moved: the camera stays where it is put
            Return, // easing back behind the vehicle
        };
        State g_state = State::Idle;
        std::atomic<int> g_stateShown{0};
        float g_turn = 0.0f, g_tilt = 0.0f; // the mouse since the last update
        bool g_stick = false;               // the right stick since the last update
        uint32_t g_vehicle = 0, g_frame = 0;
        float g_idle = 0.0f, g_returning = 0.0f; // seconds of game time without the mouse, on the way back
        float g_pitch = 0.0f;                    // the pitch before the mouse moved it, to go back to
        float g_lastPos[3] = {};
        bool g_pullOff = false;
        float g_pullCaps[kClasses] = {};

        uint32_t rd32(uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, g_rdram + (a & kRamMask), 4);
            return v;
        }
        float rdf(uint32_t a)
        {
            float v;
            std::memcpy(&v, g_rdram + (a & kRamMask), 4);
            return v;
        }
        void wrf(uint32_t a, float v) { std::memcpy(g_rdram + (a & kRamMask), &v, 4); }

        float wrap(float a)
        {
            a = std::fmod(a, kTwoPi);
            return a < 0.0f ? a + kTwoPi : a;
        }

        void setState(State s)
        {
            g_state = s;
            g_stateShown.store(static_cast<int>(s), std::memory_order_relaxed);
        }

        // The turn toward where the vehicle drives: off while the mouse holds the camera, the game's own otherwise.
        void pullOff(bool off)
        {
            if (off == g_pullOff)
                return;
            for (int i = 0; i < kClasses; ++i)
            {
                const uint32_t at = kPullCap + i * kTuningRow;
                if (off)
                    g_pullCaps[i] = rdf(at);
                wrf(at, off ? 0.0f : g_pullCaps[i]);
            }
            g_pullOff = off;
        }

        // The view's pitch from a source point, about the point looked at (looking up: the point below it).
        float pitchOf(uint32_t at, const float p[3])
        {
            const float ox = rdf(at) - p[0], oy = rdf(at + 4) - p[1], oz = rdf(at + 8) - p[2];
            return std::atan2(-oz, std::sqrt(ox * ox + oy * oy));
        }

        // Turns the camera about the point it looks at: `turn` about z (left +), `tilt` up / down (up +). Its heading and
        // pitch, the last update's (or the game would snap a small change back), and the source points the next update
        // measures them from.
        void turnCamera(uint32_t cam, const float p[3], float turn, float tilt)
        {
            if (turn != 0.0f)
            {
                wrf(cam + kCamBeta, wrap(rdf(cam + kCamBeta) + turn));
                wrf(kLastBeta, wrap(rdf(kLastBeta) + turn));
                const float c = std::cos(turn), s = std::sin(turn);
                for (uint32_t off : kCamPoints)
                {
                    const uint32_t at = cam + off;
                    const float ox = rdf(at) - p[0], oy = rdf(at + 4) - p[1];
                    wrf(at, p[0] + ox * c - oy * s);
                    wrf(at + 4, p[1] + ox * s + oy * c);
                }
            }
            if (tilt != 0.0f)
            {
                wrf(cam + kCamAlpha, rdf(cam + kCamAlpha) + tilt);
                wrf(kLastAlpha, rdf(kLastAlpha) + tilt);
                for (uint32_t off : kCamPoints)
                {
                    const uint32_t at = cam + off;
                    const float ox = rdf(at) - p[0], oy = rdf(at + 4) - p[1], oz = rdf(at + 8) - p[2];
                    const float h = std::sqrt(ox * ox + oy * oy), d = std::sqrt(h * h + oz * oz);
                    if (h < 1e-4f)
                        continue;
                    const float e = std::clamp(std::atan2(-oz, h) + tilt, -1.4f, 1.4f);
                    const float k = d * std::cos(e) / h;
                    wrf(at, p[0] + ox * k);
                    wrf(at + 4, p[1] + oy * k);
                    wrf(at + 8, p[2] - d * std::sin(e));
                }
            }
        }

        // The heading the camera has behind the vehicle: opposite to where it moves (the game's choice past 0.02 units
        // a step), or to its nose (slower, and always for a boat). Beta is the angle of the target-to-camera direction.
        float behind(uint32_t vehicle)
        {
            float x = rdf(vehicle + kMoveSpeed), y = rdf(vehicle + kMoveSpeed + 4);
            if (rd32(vehicle + kVehicleType) == 4 || std::sqrt(x * x + y * y) <= 0.02f)
            {
                x = rdf(vehicle + kEntityForward);
                y = rdf(vehicle + kEntityForward + 4);
            }
            return wrap(std::atan2(-y, -x));
        }

        void reset(uint32_t vehicle)
        {
            pullOff(false);
            setState(State::Idle);
            g_vehicle = vehicle;
            if (vehicle)
                for (int i = 0; i < 3; ++i)
                    g_lastPos[i] = rdf(vehicle + kEntityPos + i * 4);
        }

        // Before the game's update of the camera behind a vehicle (the vehicle has moved this frame).
        void prepare(uint32_t cam)
        {
            const bool stick = g_stick;
            const float turn = stick ? 0.0f : g_turn, tilt = stick ? 0.0f : g_tilt;
            g_turn = g_tilt = 0.0f;
            g_stick = false;
            const uint32_t mode = rd32(cam) & 0xFFFFu;
            const uint32_t vehicle = rd32(cam + kCamTargetEntity);
            const bool usable = cam == kCams + g_rdram[kActiveCam] * kCamSize && (mode == 18 || mode == 22) && vehicle &&
                                rd32(cam + kCamControl) == 3 && rd32(kLookBehind) == 0;
            // a frame without this camera (on foot, a cutscene) starts it afresh: the game places it behind the car again
            const uint32_t frame = rd32(kFrameCounter);
            const bool gap = frame - g_frame > 1;
            g_frame = frame;
            // the stick turns it as the game does (the speeds it builds up are zeroed here while held, and its small
            // steps snapped back)
            if (!usable || vehicle != g_vehicle || gap || stick)
            {
                reset(usable ? vehicle : 0);
                if (!usable)
                    return;
            }

            const float step = rdf(kTimeStep);
            const float dt = std::clamp(step, 0.0f, 5.0f) / 50.0f;
            float moved[3];
            for (int i = 0; i < 3; ++i)
            {
                const float now = rdf(vehicle + kEntityPos + i * 4);
                moved[i] = now - g_lastPos[i];
                g_lastPos[i] = now;
            }
            // the point looked at this frame: the last one moved with the vehicle (the game's own, a1, is lifted and on a
            // bike moved along its lean by the update, which is measured from the last one)
            const float p[3] = {rdf(cam + kCamTarget) + moved[0], rdf(cam + kCamTarget + 4) + moved[1],
                                rdf(cam + kCamTarget + 8) + moved[2]};

            if (turn != 0.0f || tilt != 0.0f)
            {
                if (g_state != State::Hold)
                    g_pitch = pitchOf(cam + kCamPoints[0], p);
                setState(State::Hold);
                g_idle = 0.0f;
            }
            else if (g_state == State::Hold)
            {
                g_idle += dt;
                const float delay = g_delay.load(std::memory_order_relaxed);
                if (delay >= 0.0f && g_idle >= delay)
                {
                    setState(State::Return);
                    g_returning = 0.0f;
                }
            }

            if (g_state == State::Idle)
            {
                pullOff(false);
                return;
            }
            // held and on the way back: the source points move with the vehicle (so it does not drag the camera after
            // it, as on a string), the turn toward where it drives is off, and the speeds left from before stop - the
            // camera goes only where it is turned here, as slowly back while driving as standing
            pullOff(true);
            for (uint32_t off : kCamPoints)
                for (int i = 0; i < 3; ++i)
                    wrf(cam + off + i * 4, rdf(cam + off + i * 4) + moved[i]);
            wrf(cam + kCamHeadingSpeed, 0.0f);
            wrf(cam + kCamPitchSpeed, 0.0f);
            if (g_state == State::Hold)
                turnCamera(cam, p, turn, tilt);
            else
            {
                // back behind the vehicle and to the pitch from before, easing out; then the game's camera has it
                g_returning += dt;
                const float k = 1.0f - std::exp(-dt / kReturnTime), cap = kReturnRate * dt;
                const float heading = std::remainder(behind(vehicle) - rdf(cam + kCamBeta), kTwoPi);
                const float pitch = g_pitch - pitchOf(cam + kCamPoints[0], p);
                turnCamera(cam, p, std::clamp(heading * k, -cap, cap), std::clamp(pitch * k, -cap, cap));
                if ((std::fabs(heading) < 0.01f && std::fabs(pitch) < 0.01f) || g_returning >= kReturnLimit)
                    setState(State::Idle);
            }
        }

        // FUN_0029FAF0 (CCam::Process's cases for modes 18 / 22): the mouse's say first, then the game's update.
        PS2_GUEST_CC void followCar(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            prepare(GPR_U32(ctx, 4));
            g_orig(rdram, ctx, runtime);
        }
    }

    void install(PS2Runtime &runtime)
    {
        g_rdram = runtime.memory().getRDRAM();
        if (const char *e = std::getenv("VCS_CARCAM"); e && e[0] == '0')
        {
            std::fprintf(stderr, "[carcam] off (VCS_CARCAM=0)\n");
            return;
        }
        g_orig = runtime.lookupFunction(kFollowCar);
        if (!g_orig)
            return;
        runtime.replaceFunction(kFollowCar, &followCar);
        g_installed = true;
        // carcam -> ok state=<idle|hold|return> (tools/controls_check.py)
        ps2control::addCommand("carcam", [](const std::string &, ps2control::Reply reply) {
            static const char *const names[] = {"idle", "hold", "return"};
            reply(std::string("ok state=") + names[g_stateShown.load(std::memory_order_relaxed)]);
        });
    }

    bool turns(uint32_t mode) { return g_installed.load(std::memory_order_relaxed) && (mode == 18 || mode == 22); }

    void mouse(float turn, float tilt)
    {
        g_turn += turn;
        g_tilt += tilt;
    }

    void stickMoved() { g_stick = true; }

    void setReturnDelay(float seconds) { g_delay.store(seconds, std::memory_order_relaxed); }
}
