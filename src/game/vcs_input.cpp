// See vcs_input.h.
//
// The game reads the pad once per game frame (scePadRead, game thread). The bindings live in the settings registry
// (main thread); every change rebuilds one Map, which the game thread copies under a mutex at each read. The read also
// looks at the game (game-internals.md 12b): whether the player drives (the Driving keys) and which camera is active
// (the mouse turns the on-foot camera itself).
#include "game/vcs_input.h"

#include "game/vcs_aim.h"
#include "game/vcs_freecam.h"
#include "platform/platform.h"
#include "ps2_runtime.h"
#include "runtime/ps2_audio_out.h"
#include "runtime/ps2_control.h"
#include "runtime/ps2_pad.h"
#include "settings/settings.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vcs::input
{
    namespace
    {
        // DualShock 2 buttons in the report's 16-bit word (active low)
        constexpr uint16_t kSelect = 0x0001, kL3 = 0x0002, kR3 = 0x0004, kStart = 0x0008, kUp = 0x0010, kRight = 0x0020,
                           kDown = 0x0040, kLeft = 0x0080, kL2 = 0x0100, kR2 = 0x0200, kL1 = 0x0400, kR1 = 0x0800,
                           kTriangle = 0x1000, kCircle = 0x2000, kCross = 0x4000, kSquare = 0x8000;

        // Gamepad sources for a PS2 button (the Gamepad page's choices): platform::GamepadButton, or a trigger.
        constexpr int kPadNone = -1, kPadLT = 20, kPadRT = 21;

        enum class Target
        {
            Button, // `mask`
            Stick,  // `axis` (0 lx, 1 ly, 2 rx, 3 ry) toward `sign`
            Walk,   // left stick held at walking speed
        };

        struct Control
        {
            const char *id;       // vcs.ini: keys.<id>, keys.<id>2, pad.<id>
            const char *label;
            const char *section;  // on the Keyboard & mouse page
            Target target;
            uint16_t mask;
            int axis, sign;
            int key1, key2;       // default keys
            int pad;              // default gamepad source (buttons only)
            bool driving = false; // only while the player drives; its keys then do nothing else
        };

        using namespace platform;
        const Control kControls[] = {
            {"move_up", "Move forward", "Movement", Target::Stick, 0, 1, -1, KEY_W, 0, kPadNone},
            {"move_down", "Move back", "Movement", Target::Stick, 0, 1, 1, KEY_S, 0, kPadNone},
            {"move_left", "Move left", "Movement", Target::Stick, 0, 0, -1, KEY_A, 0, kPadNone},
            {"move_right", "Move right", "Movement", Target::Stick, 0, 0, 1, KEY_D, 0, kPadNone},
            {"walk", "Walk (hold)", "Movement", Target::Walk, 0, 0, 0, 0, 0, kPadNone},
            {"cam_up", "Camera up (right stick)", "Camera", Target::Stick, 0, 3, -1, KEY_I, 0, kPadNone},
            {"cam_down", "Camera down", "Camera", Target::Stick, 0, 3, 1, KEY_K, 0, kPadNone},
            {"cam_left", "Camera left", "Camera", Target::Stick, 0, 2, -1, KEY_J, 0, kPadNone},
            {"cam_right", "Camera right", "Camera", Target::Stick, 0, 2, 1, KEY_L, 0, kPadNone},
            {"cross", "Cross (sprint, accelerate)", "Buttons", Target::Button, kCross, 0, 0, KEY_LEFT_SHIFT, 0, GAMEPAD_SOUTH},
            {"square", "Square (jump, brake)", "Buttons", Target::Button, kSquare, 0, 0, KEY_SPACE, 0, GAMEPAD_WEST},
            {"triangle", "Triangle (enter vehicle)", "Buttons", Target::Button, kTriangle, 0, 0, KEY_F, 0, GAMEPAD_NORTH},
            {"circle", "Circle (attack)", "Buttons", Target::Button, kCircle, 0, 0, KEY_LEFT_CONTROL, KEY_MOUSE_LEFT, GAMEPAD_EAST},
            {"l1", "L1", "Buttons", Target::Button, kL1, 0, 0, KEY_Q, 0, GAMEPAD_LEFT_SHOULDER},
            {"r1", "R1 (target)", "Buttons", Target::Button, kR1, 0, 0, KEY_E, KEY_MOUSE_RIGHT, GAMEPAD_RIGHT_SHOULDER},
            {"l2", "L2", "Buttons", Target::Button, kL2, 0, 0, KEY_ONE, KEY_Z, kPadLT},
            {"r2", "R2", "Buttons", Target::Button, kR2, 0, 0, KEY_THREE, KEY_X, kPadRT},
            {"l3", "L3 (left stick click)", "Buttons", Target::Button, kL3, 0, 0, KEY_C, 0, GAMEPAD_LEFT_STICK},
            {"r3", "R3 (right stick click)", "Buttons", Target::Button, kR3, 0, 0, KEY_V, 0, GAMEPAD_RIGHT_STICK},
            {"start", "Start (pause menu)", "Buttons", Target::Button, kStart, 0, 0, KEY_ENTER, 0, GAMEPAD_START},
            {"select", "Select", "Buttons", Target::Button, kSelect, 0, 0, KEY_BACKSPACE, 0, GAMEPAD_BACK},
            {"dpad_up", "D-pad up", "D-pad", Target::Button, kUp, 0, 0, KEY_UP, 0, GAMEPAD_DPAD_UP},
            {"dpad_down", "D-pad down", "D-pad", Target::Button, kDown, 0, 0, KEY_DOWN, 0, GAMEPAD_DPAD_DOWN},
            {"dpad_left", "D-pad left", "D-pad", Target::Button, kLeft, 0, 0, KEY_LEFT, 0, GAMEPAD_DPAD_LEFT},
            {"dpad_right", "D-pad right", "D-pad", Target::Button, kRight, 0, 0, KEY_RIGHT, 0, GAMEPAD_DPAD_RIGHT},
            // the game's driving buttons (Cross accelerates, Square brakes / reverses, R1 is the handbrake; checked in
            // game), on the keys a PC driving game uses
            {"drive_accelerate", "Accelerate (Cross)", "Driving", Target::Button, kCross, 0, 0, KEY_W, 0, kPadNone, true},
            {"drive_brake", "Brake / reverse (Square)", "Driving", Target::Button, kSquare, 0, 0, KEY_S, 0, kPadNone, true},
            {"drive_handbrake", "Handbrake (R1)", "Driving", Target::Button, kR1, 0, 0, KEY_SPACE, 0, kPadNone, true},
            {"drive_lean_forward", "Lean forward / nose down", "Driving", Target::Stick, 0, 1, -1, KEY_KP_8, 0, kPadNone, true},
            {"drive_lean_back", "Lean back / nose up", "Driving", Target::Stick, 0, 1, 1, KEY_KP_5, 0, kPadNone, true},
        };
        constexpr size_t kControlCount = sizeof(kControls) / sizeof(kControls[0]);

        // ---- The game (game-internals.md 12, 12b) --------------------------------------------------------------------
        constexpr uint32_t kRamMask = PS2_RAM_MASK;
        constexpr uint32_t kPlayerPed = 0x004E4910u;   // CWorld::Players[0].m_pPed
        constexpr uint32_t kPedVehicle = 0x480u;       // the vehicle it is in (0 on foot)
        constexpr uint32_t kPedState = 0x8C4u;         // 0x37 driving
        constexpr uint32_t kTheCamera = 0x006F44D0u;
        constexpr uint32_t kLooksAtPlayer = kTheCamera + 0x888u; // 0 under a script camera
        constexpr uint32_t kCam = kTheCamera + 0x70u;  // the active CCam: +0 mode, +0x78 alpha (pitch), +0x7C beta (heading),
                                                       // +0x134 FOV
        constexpr uint32_t kCamModeFollowPed = 4;      // on foot (11 locked on, 18 in a vehicle)
        // the first-person weapon cameras (FUN_00297cb8, game-internals.md 12c): sniper 7, rocket launcher 8, M16 34,
        // camera 46, binoculars 47, 45; they turn by +0x78 / +0x7C as the follow camera, and 7 / 46 / 47 zoom (+0x134)
        bool scopeMode(uint32_t mode) { return mode == 7 || mode == 8 || mode == 34 || (mode >= 45 && mode <= 47); }
        bool zoomMode(uint32_t mode) { return mode == 7 || mode == 46 || mode == 47; }
        constexpr uint32_t kScopeZoom = 0x00487E2Cu; // the FOV a zooming scope eases toward
        constexpr uint32_t kPad0 = 0x005147A8u;        // CPad 0: +0x96 DisablePlayerControls (s16)
        constexpr uint32_t kCamStickLocked = 0x00488EB4u; // the follow camera ignores the stick's turn while set
        constexpr uint32_t kCamTable = 0x00487B30u;    // the follow camera's tuning, outdoors (+0x3C indoors): +0x34 / +0x38
                                                       // the pitch limits up / down
        constexpr uint32_t kCamIndoors = 0x00489F7Cu;  // the table indoors when neither 0 nor 4 (FUN_0029dab0)
        constexpr uint32_t kFrameCounter = 0x004CD124u; // CTimer::m_FrameCounter
        constexpr uint32_t kUserPause = 0x004CD2ACu;   // CTimer::m_UserPause (the pause menu), m_CodePause: bytes
        constexpr uint32_t kCodePause = 0x004CD11Cu;
        uint8_t *g_rdram = nullptr;

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

        struct GameState
        {
            bool driving = false;
            bool footCamera = false; // the on-foot follow camera, turned by the player (no script camera, controls on)
        };

        GameState gameState()
        {
            GameState s;
            if (!g_rdram)
                return s;
            const uint32_t ped = rd32(kPlayerPed);
            s.driving = ped && rd32(ped + kPedVehicle) && rd32(ped + kPedState) == 0x37;
            int16_t disabled;
            std::memcpy(&disabled, g_rdram + kPad0 + 0x96, 2);
            const bool paused = g_rdram[kUserPause] || g_rdram[kCodePause];
            s.footCamera = ped && !s.driving && !paused && rd32(kCam) == kCamModeFollowPed && rd32(kLooksAtPlayer) == 1 &&
                           disabled == 0 && rd32(kCamStickLocked) == 0;
            return s;
        }

        // Everything a pad read needs, rebuilt from the settings on every change.
        struct Map
        {
            std::array<std::array<int, 2>, kControlCount> keys{};
            std::array<int, kControlCount> pad{};
            bool mouseCamera = true;
            float mouseScale = 1.0f; // stick deflection per mouse count per second (the cameras the stick turns)
            float mouseLook = 0.0025f; // radians per mouse count (the on-foot camera, turned directly)
            bool mouseInvertY = false;
            bool freeAimMouse = false, freeAimPad = false; // free aim (game/vcs_aim.h) on the keyboard / mouse, the gamepad
            bool mouseWheel = true;                        // the wheel changes weapons on foot and zooms scopes
            bool swapSticks = false, invertX = false, invertY = false;
            float deadzoneLeft = 0.12f, deadzoneRight = 0.12f, cameraScale = 1.0f, trigger = 0.3f;
            float vibration = 1.0f;
            float walk = 0.45f;
        };

        std::mutex g_mapMutex;
        Map g_map;
        bool g_enabled = true; // VCS_INPUT_MAP
        std::atomic<bool> g_backgroundPause{false}; // game.background = Pause and the window is behind others
        std::atomic<uint64_t> g_lastReport{0};       // buttons | rx << 16 | ry << 24 | lx << 32 | ly << 40 ("padstate")
        std::atomic<uint32_t> g_reads{0};            // pad reads so far ("padstate")

        std::string keyId(const Control &c, int slot) { return std::string("keys.") + c.id + (slot ? "2" : ""); }
        std::string padId(const Control &c) { return std::string("pad.") + c.id; }

        void rebuild()
        {
            Map m;
            for (size_t i = 0; i < kControlCount; ++i)
            {
                m.keys[i][0] = settings::value(keyId(kControls[i], 0));
                m.keys[i][1] = settings::value(keyId(kControls[i], 1));
                m.pad[i] = kControls[i].target == Target::Button && !kControls[i].driving ? settings::value(padId(kControls[i])) : kPadNone;
            }
            m.mouseCamera = settings::value("mouse.camera") != 0;
            // 100%: a mouse moving 1,500 counts a second (a steady turn at 800 DPI) is a full stick; on foot 400 counts
            // (half an inch at 800 DPI) turn a radian, as the PC GTAs' default
            m.mouseScale = settings::value("mouse.sensitivity") / 100.0f / 1500.0f;
            m.mouseLook = settings::value("mouse.sensitivity") / 100.0f * 0.0025f;
            m.mouseInvertY = settings::value("mouse.invert_y") != 0;
            m.freeAimMouse = settings::value("mouse.free_aim") != 0;
            m.freeAimPad = settings::value("pad.free_aim") != 0;
            m.mouseWheel = settings::value("mouse.wheel") != 0;
            m.swapSticks = settings::value("pad.swap_sticks") != 0;
            m.invertX = settings::value("pad.invert_x") != 0;
            m.invertY = settings::value("pad.invert_y") != 0;
            m.deadzoneLeft = settings::value("pad.deadzone_left") / 100.0f;
            m.deadzoneRight = settings::value("pad.deadzone_right") / 100.0f;
            m.cameraScale = settings::value("pad.camera_speed") / 100.0f;
            m.trigger = settings::value("pad.trigger") / 100.0f;
            m.vibration = settings::value("pad.vibration") / 100.0f;
            m.walk = settings::value("keys.walk_speed") / 100.0f;
            std::lock_guard<std::mutex> lock(g_mapMutex);
            g_map = m;
        }

        // A bound key is down: the key itself (physical or injected over the control channel), the other side's
        // Shift / Ctrl / Alt, Keypad Enter for Enter. Enter with Alt held is Alt+Enter (full screen), not a binding.
        bool bound(int key, bool alt)
        {
            if (key <= 0)
                return false;
            auto down = [](int k) { return ps2control::keyDown(k); };
            switch (key)
            {
            case KEY_LEFT_SHIFT: return down(KEY_LEFT_SHIFT) || down(KEY_RIGHT_SHIFT);
            case KEY_LEFT_CONTROL: return down(KEY_LEFT_CONTROL) || down(KEY_RIGHT_CONTROL);
            case KEY_LEFT_ALT: return down(KEY_LEFT_ALT) || down(KEY_RIGHT_ALT);
            case KEY_ENTER: return !alt && (down(KEY_ENTER) || down(KEY_KP_ENTER));
            default: return down(key);
            }
        }

        // "gamepad ..." (tests): a gamepad of buttons and axes set over the control channel, in place of the real one
        // while on, physical input or not
        struct InjectedPad
        {
            bool on = false;
            std::array<bool, GAMEPAD_BUTTON_COUNT> buttons{};
            std::array<float, AXIS_COUNT> axes{};
        };
        std::mutex g_injectedPadMutex;
        InjectedPad g_injectedPad;

        InjectedPad injectedPad()
        {
            std::lock_guard<std::mutex> lock(g_injectedPadMutex);
            return g_injectedPad;
        }
        bool padConnected(bool physical) { return injectedPad().on || (physical && gamepadConnected()); }
        bool padButton(GamepadButton b)
        {
            const InjectedPad p = injectedPad();
            return p.on ? p.buttons[b] : gamepadButton(b);
        }
        float padAxis(GamepadAxis a)
        {
            const InjectedPad p = injectedPad();
            return p.on ? p.axes[a] : gamepadAxis(a);
        }

        bool padSource(int source, float trigger)
        {
            if (source >= 0 && source < GAMEPAD_BUTTON_COUNT)
                return padButton(static_cast<GamepadButton>(source));
            if (source == kPadLT)
                return padAxis(AXIS_LEFT_TRIGGER) > trigger;
            if (source == kPadRT)
                return padAxis(AXIS_RIGHT_TRIGGER) > trigger;
            return false;
        }

        // Radial deadzone, the rest of the range stretched to 0..1 so a small tilt past it still counts.
        void deadzone(float &x, float &y, float dz)
        {
            const float m = std::sqrt(x * x + y * y);
            if (m <= dz || dz >= 0.99f)
            {
                x = y = 0.0f;
                return;
            }
            const float k = std::min(1.0f, (m - dz) / (1.0f - dz)) / m;
            x *= k;
            y *= k;
        }

        uint8_t toByte(float v)
        {
            v = std::clamp(v, -1.0f, 1.0f);
            return static_cast<uint8_t>(v < 0.0f ? 128.0f + v * 128.0f : 128.0f + v * 127.0f);
        }

        // The mouse camera. On foot the mouse turns the follow camera itself: its heading and pitch are written in the
        // game, with the points the camera stands at turned to match. Every other camera (vehicles, aiming) gets the
        // mouse's speed (counts per second) as the right stick, so it turns as far for the same hand movement at any
        // frame rate.
        struct MouseCamera
        {
            std::chrono::steady_clock::time_point last{};
            uint32_t frame = 0;        // the game frame (m_FrameCounter) it was worked out for
            float x = 0.0f, y = 0.0f;  // the right stick's deflection from the mouse
            aim::Output aim;           // free aim's say over R1 / L3 / the right stick, for the frame's reads
            std::chrono::steady_clock::time_point hipUntil{}; // free aim held this long after the last shot from the hip
            bool aimHeld = false;      // the target button was held at the last game frame
            bool wheelWas[2] = {};     // a wheel notch (up, down) was down at the last game frame
            int wheelSteps = 0;        // weapon changes still to press (+ next / R2, - previous / L2), one a frame
            uint16_t wheelButton = 0;  // the button the wheel presses for this frame
        } g_mouse;

        std::mutex g_injectedMutex;
        float g_injectedX = 0.0f, g_injectedY = 0.0f; // "mouse <dx> <dy>" (tests): counts, as from the mouse

        // The game's stick curve (FUN_002842e8, game-internals.md 12a): v = byte - 128; |v| <= 42 is 0, past that
        // (|v| - 42) 127 / 85, truncated - the first third of the travel does nothing. The smallest deflection the game
        // reads as `g` (-127..127) or a little more, and that deflection as the float toByte turns back into exactly
        // that byte.
        int rawFor(int g)
        {
            if (g == 0)
                return 0;
            const int a = std::min(42 + (std::abs(g) * 85 + 126) / 127, g < 0 ? 128 : 127);
            return g < 0 ? -a : a;
        }
        float stickFloat(int raw) { return raw == 0 ? 0.0f : (raw + 0.5f) / (raw > 0 ? 127.0f : 128.0f); }

        bool readPad(uint8_t *data)
        {
            Map m;
            {
                std::lock_guard<std::mutex> lock(g_mapMutex);
                m = g_map;
            }
            uint16_t buttons = 0;
            float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;
            const bool physical = ps2control::physicalInputOn();
            // the free camera has the controls (and the mouse, takeFreeCameraInput): the game gets a pad left alone
            const bool freeCamera = freecam::flying();

            bool padR1 = false, keyR1 = false, keyCircle = false; // the target / fire buttons, by device (free aim)
            float padRx = 0.0f, padRy = 0.0f;
            if (padConnected(physical) && !freeCamera)
            {
                for (size_t i = 0; i < kControlCount; ++i)
                    if (kControls[i].target == Target::Button && padSource(m.pad[i], m.trigger))
                    {
                        buttons |= kControls[i].mask;
                        padR1 = padR1 || kControls[i].mask == kR1;
                    }
                lx = padAxis(AXIS_LEFT_X);
                ly = padAxis(AXIS_LEFT_Y);
                rx = padAxis(AXIS_RIGHT_X);
                ry = padAxis(AXIS_RIGHT_Y);
                if (m.swapSticks)
                {
                    std::swap(lx, rx);
                    std::swap(ly, ry);
                }
                deadzone(lx, ly, m.deadzoneLeft);
                deadzone(rx, ry, m.deadzoneRight);
                rx *= (m.invertX ? -1.0f : 1.0f) * m.cameraScale;
                ry *= (m.invertY ? -1.0f : 1.0f) * m.cameraScale;
                padRx = rx;
                padRy = ry;
            }

            // keyboard and mouse buttons (blocked while a menu has the input, injected keys included). While the player
            // drives the Driving keys apply, and other bindings of the same keys do not (W accelerates, not also the
            // stick).
            const GameState game = gameState();
            const bool blocked = ps2control::gameInputBlocked() || freeCamera;
            const bool alt = !blocked && (ps2control::keyDown(KEY_LEFT_ALT) || ps2control::keyDown(KEY_RIGHT_ALT));
            auto drivingKey = [&](int key) {
                for (size_t i = 0; i < kControlCount; ++i)
                    if (kControls[i].driving && key > 0 && (m.keys[i][0] == key || m.keys[i][1] == key))
                        return true;
                return false;
            };
            std::array<int, 4> neg{}, pos{}; // per axis: a bound key toward each side is down
            bool walk = false;
            for (size_t i = 0; i < kControlCount && !blocked; ++i)
            {
                const Control &c = kControls[i];
                if (c.driving && !game.driving)
                    continue;
                auto active = [&](int key) { return bound(key, alt) && !(game.driving && !c.driving && drivingKey(key)); };
                if (!active(m.keys[i][0]) && !active(m.keys[i][1]))
                    continue;
                if (c.target == Target::Button)
                {
                    buttons |= c.mask;
                    keyR1 = keyR1 || (c.mask == kR1 && !c.driving);
                    keyCircle = keyCircle || c.mask == kCircle;
                }
                else if (c.target == Target::Stick)
                    (c.sign < 0 ? neg : pos)[c.axis] = 1;
                else
                    walk = true;
            }
            float *axes[4] = {&lx, &ly, &rx, &ry};
            for (int a = 0; a < 4; ++a)
                if (neg[a] != pos[a])
                    *axes[a] = neg[a] ? -1.0f : 1.0f;
            if (walk)
            {
                const float mag = std::sqrt(lx * lx + ly * ly);
                if (mag > m.walk)
                {
                    lx *= m.walk / mag;
                    ly *= m.walk / mag;
                }
            }

            // the mouse camera, on top of the right stick: worked out once per game frame (the game reads the pad three
            // times a frame; the frame's other reads repeat the deflection)
            const auto now = std::chrono::steady_clock::now();
            const float dt = g_mouse.last.time_since_epoch().count() ? std::chrono::duration<float>(now - g_mouse.last).count() : 0.0f;
            const uint32_t frame = g_rdram ? rd32(kFrameCounter) : 0;
            if (freeCamera)
                g_mouse = {};
            else if (frame != g_mouse.frame || dt >= 0.25f || dt == 0.0f)
            {
                g_mouse.last = now;
                g_mouse.frame = frame;
                float dx = 0.0f, dy = 0.0f;
                takeMouseMotion(dx, dy);
                if (!physical)
                    dx = dy = 0.0f;
                {
                    std::lock_guard<std::mutex> lock(g_injectedMutex);
                    dx += g_injectedX;
                    dy += g_injectedY;
                    g_injectedX = g_injectedY = 0.0f;
                }
                const bool on = m.mouseCamera && !blocked;
                // up (+) for a mouse moved forward (SDL: y grows downwards); the game looks up for a stick pulled
                // back (y > 0)
                const float up = -dy * (m.mouseInvertY ? -1.0f : 1.0f);
                g_mouse.x = g_mouse.y = 0.0f;
                const uint32_t cam = kTheCamera + 0x70u + g_rdram[kTheCamera + 0x50u] * 0x270u; // the active CCam
                const uint32_t mode = rd32(cam) & 0xFFFFu;
                float fov = rdf(cam + 0x134);
                fov = fov >= 1.0f && fov <= 130.0f ? fov : 70.0f;

                // free aim (game/vcs_aim.h): the target button with a gun - R1 (its keys or mouse button) when Keyboard >
                // Free aim is on, the gamepad's R1 when Gamepad > Free aim is; with the keyboard / mouse also a shot
                // from the hip (Circle), held a moment after it so a burst stays one aim
                g_mouse.aim = {};
                if (m.freeAimMouse || m.freeAimPad || aim::aiming())
                {
                    // the hold is for shots from the hip only: letting go of the target button ends the aim at once
                    if (keyR1 || g_mouse.aimHeld)
                        g_mouse.hipUntil = {};
                    else if (m.freeAimMouse && keyCircle && aim::gunInHand())
                        g_mouse.hipUntil = now + std::chrono::milliseconds(600);
                    g_mouse.aimHeld = keyR1;
                    const bool mouseWant = m.freeAimMouse && (keyR1 || now < g_mouse.hipUntil);
                    const bool padWant = m.freeAimPad && padR1;
                    aim::Input in;
                    in.want = !blocked && (mouseWant || padWant);
                    in.on = m.freeAimMouse || m.freeAimPad;
                    if (!blocked)
                    {
                        // zoomed views turn slower for the same hand movement
                        const float look = m.mouseLook * fov / 70.0f;
                        in.turn = -dx * look;
                        in.tilt = up * look;
                        // the stick on a curve (fine aim near the centre), left / up +
                        in.turnRate = -padRx * std::fabs(padRx);
                        in.tiltRate = -padRy * std::fabs(padRy);
                    }
                    g_mouse.aim = aim::update(in);
                }

                if (g_mouse.aim.camera)
                {
                    // free aim turns the camera itself (aim::update)
                }
                else if (on && scopeMode(mode) && !game.driving)
                {
                    // the scopes and first-person weapon views turn by the mouse itself, as the follow camera's heading:
                    // their update adds the stick's speed to +0x78 / +0x7C and keeps them in its limits
                    // (FUN_00297cb8); zoomed in they turn slower
                    const float look = m.mouseLook * fov / 70.0f;
                    const float turn = -dx * look;
                    float beta = std::fmod(rdf(cam + 0x7C) + turn, 6.2831855f);
                    wrf(cam + 0x7C, beta < 0.0f ? beta + 6.2831855f : beta);
                    wrf(cam + 0x78, rdf(cam + 0x78) + up * look);
                }
                else if (on && game.footCamera)
                {
                    // heading: right turns clockwise (the angle falls), kept in 0..2 pi as the game keeps it. The camera
                    // eases its heading back toward where it stands (its source points, +0x220 / +0x230, placed around
                    // the player from the heading at the end of each update), so those turn with it.
                    const float turn = -dx * m.mouseLook;
                    if (turn != 0.0f)
                    {
                        float beta = std::fmod(rdf(kCam + 0x7C) + turn, 6.2831855f);
                        wrf(kCam + 0x7C, beta < 0.0f ? beta + 6.2831855f : beta);
                        const uint32_t ped = rd32(kPlayerPed);
                        const float px = rdf(ped + 0x30), py = rdf(ped + 0x34), c = std::cos(turn), s = std::sin(turn);
                        for (uint32_t at : {kCam + 0x220u, kCam + 0x230u})
                        {
                            const float ox = rdf(at) - px, oy = rdf(at + 4) - py;
                            wrf(at, px + ox * c - oy * s);
                            wrf(at + 4, py + ox * s + oy * c);
                        }
                    }
                    // pitch (up +), as the heading: the camera eases it toward the pitch its source points stand at, seen
                    // from the target point (+0x1A0, which it placed them around), so those turn up / down about it by
                    // the same angle. Through the stick it was capped at the stick's top speed (0.03 a step) and then
                    // eased (20% of the gap a step), slower than the heading. Kept in the game's limits.
                    const uint32_t indoors = rd32(kCamIndoors);
                    const uint32_t table = kCamTable + (indoors != 0 && indoors != 4 ? 0x3Cu : 0u);
                    const float alpha = rdf(kCam + 0x78);
                    const float tilt = std::clamp(alpha + up * m.mouseLook, -rdf(table + 0x38), rdf(table + 0x34)) - alpha;
                    if (tilt != 0.0f)
                    {
                        wrf(kCam + 0x78, alpha + tilt);
                        const float tx = rdf(kCam + 0x1A0), ty = rdf(kCam + 0x1A4), tz = rdf(kCam + 0x1A8);
                        for (uint32_t at : {kCam + 0x220u, kCam + 0x230u})
                        {
                            const float ox = rdf(at) - tx, oy = rdf(at + 4) - ty, oz = rdf(at + 8) - tz;
                            const float h = std::sqrt(ox * ox + oy * oy), d = std::sqrt(h * h + oz * oz);
                            if (h < 1e-4f)
                                continue;
                            // looking up is a source point below the target
                            const float e = std::clamp(std::atan2(-oz, h) + tilt, -1.55f, 1.55f);
                            const float k = d * std::cos(e) / h;
                            wrf(at, tx + ox * k);
                            wrf(at + 4, ty + oy * k);
                            wrf(at + 8, tz - d * std::sin(e));
                        }
                    }
                }
                else
                {
                    if (on && dt > 0.0f && dt < 0.25f)
                    {
                        // the mouse's speed as the game's stick value, past its dead third
                        auto tilt = [](float t) { return stickFloat(rawFor(static_cast<int>(std::lround(std::clamp(t, -1.0f, 1.0f) * 127.0f)))); };
                        g_mouse.x = tilt(dx / dt * m.mouseScale);
                        g_mouse.y = tilt(up / dt * m.mouseScale);
                    }
                }

                // the wheel (Keyboard > Mouse wheel): a notch is one weapon change on foot (R2 next, L2 previous: one
                // press a frame, so the game sees each), or a zoom step in a scope that zooms. Not while a binding uses
                // the wheel, nor while aiming (L2 / R2 change the lock-on's target there).
                const uint16_t pressedLast = g_mouse.wheelButton;
                g_mouse.wheelButton = 0;
                bool notch[2] = {};
                for (int w = 0; w < 2; ++w)
                {
                    const bool down = !blocked && ps2control::keyDown(w ? KEY_WHEEL_DOWN : KEY_WHEEL_UP);
                    notch[w] = down && !g_mouse.wheelWas[w];
                    g_mouse.wheelWas[w] = down;
                }
                bool wheelBound = false;
                for (size_t i = 0; i < kControlCount; ++i)
                    for (int k : m.keys[i])
                        wheelBound = wheelBound || k == KEY_WHEEL_UP || k == KEY_WHEEL_DOWN;
                if (!m.mouseWheel || wheelBound || game.driving || g_mouse.aim.camera || aim::aiming())
                    g_mouse.wheelSteps = 0;
                else if (zoomMode(mode))
                {
                    // the zoom the scope eases its FOV toward (0x487E2C, FUN_00297cb8), in the game's own limits: 70
                    // down to 15 (4.8 for the camera)
                    const float zoomed = rdf(kScopeZoom) * (notch[0] ? 0.8f : 1.0f) * (notch[1] ? 1.25f : 1.0f);
                    wrf(kScopeZoom, std::clamp(zoomed, mode == 46 ? 4.8f : 15.0f, 70.0f));
                    g_mouse.wheelSteps = 0;
                }
                else if (mode == kCamModeFollowPed)
                {
                    g_mouse.wheelSteps = std::clamp(g_mouse.wheelSteps + (notch[1] ? 1 : 0) - (notch[0] ? 1 : 0), -3, 3);
                    // a press, then a frame up before the next one
                    if (g_mouse.wheelSteps && !pressedLast)
                    {
                        g_mouse.wheelButton = g_mouse.wheelSteps > 0 ? kR2 : kL2;
                        g_mouse.wheelSteps += g_mouse.wheelSteps > 0 ? -1 : 1;
                    }
                }
                else
                    g_mouse.wheelSteps = 0;
            }
            rx += g_mouse.x;
            ry += g_mouse.y;
            buttons |= g_mouse.wheelButton;
            if (g_mouse.aim.own)
            {
                // free aim has the target button (R1 + L3 to enter the game's free aim, then R1 held; R1 up while the
                // button is held after the gun ran dry) and, aiming, the camera
                buttons = static_cast<uint16_t>((buttons & ~(kR1 | kL3)) | (g_mouse.aim.r1 ? kR1 : 0) | (g_mouse.aim.l3 ? kL3 : 0));
                if (g_mouse.aim.camera)
                    rx = ry = 0.0f;
            }
            else if (g_mouse.aim.camera)
            {
                // the game's own free aim in a car (aim::update): the mouse / right stick turn it, not the left stick
                rx = ry = 0.0f;
                if (g_mouse.aim.leftStick)
                    lx = ly = 0.0f;
                // the right thumb is on the stick: the gamepad's R1 fires there (the user's ask), and is not R1 then
                if (g_mouse.aim.padR1Fires && padR1)
                    buttons = static_cast<uint16_t>((buttons & ~(keyR1 ? 0 : kR1)) | kCircle);
            }

            const uint16_t word = static_cast<uint16_t>(~buttons);
            data[2] = static_cast<uint8_t>(word & 0xFF);
            data[3] = static_cast<uint8_t>(word >> 8);
            data[4] = toByte(rx);
            data[5] = toByte(ry);
            data[6] = toByte(lx);
            data[7] = toByte(ly);
            g_reads.fetch_add(1, std::memory_order_relaxed);
            g_lastReport.store(static_cast<uint64_t>(word) | static_cast<uint64_t>(data[4]) << 16 | static_cast<uint64_t>(data[5]) << 24 |
                                   static_cast<uint64_t>(data[6]) << 32 | static_cast<uint64_t>(data[7]) << 40,
                               std::memory_order_relaxed);
            return true;
        }

        // The game's motors: the small one is on / off (the fast, high-frequency motor of a modern gamepad), the large
        // one 0..255 (the slow one).
        void actuators(uint8_t small, uint8_t large)
        {
            float strength = 1.0f;
            {
                std::lock_guard<std::mutex> lock(g_mapMutex);
                strength = g_map.vibration;
            }
            static bool logged = false;
            if ((small || large) && !logged)
            {
                logged = true;
                std::fprintf(stderr, "[input] the game vibrates the pad (small %u, large %u)\n", small, large);
            }
            if (!ps2control::physicalInputOn())
                strength = 0.0f;
            gamepadRumble(std::min(1.0f, large / 255.0f * strength), small ? std::min(1.0f, strength) : 0.0f);
        }

        // Gamepad sources, labelled for both Xbox and PlayStation pads.
        std::vector<settings::Choice> padChoices()
        {
            return {{"A / Cross", GAMEPAD_SOUTH},
                    {"B / Circle", GAMEPAD_EAST},
                    {"X / Square", GAMEPAD_WEST},
                    {"Y / Triangle", GAMEPAD_NORTH},
                    {"LB / L1", GAMEPAD_LEFT_SHOULDER},
                    {"RB / R1", GAMEPAD_RIGHT_SHOULDER},
                    {"LT / L2", kPadLT},
                    {"RT / R2", kPadRT},
                    {"Left stick click", GAMEPAD_LEFT_STICK},
                    {"Right stick click", GAMEPAD_RIGHT_STICK},
                    {"Start / Options", GAMEPAD_START},
                    {"Back / View / Share", GAMEPAD_BACK},
                    {"D-pad up", GAMEPAD_DPAD_UP},
                    {"D-pad down", GAMEPAD_DPAD_DOWN},
                    {"D-pad left", GAMEPAD_DPAD_LEFT},
                    {"D-pad right", GAMEPAD_DPAD_RIGHT},
                    {"None", kPadNone}};
        }
    }

    void registerSettings()
    {
        using settings::Type;
        settings::Spec s;
        auto onChange = [](int) { rebuild(); };
        const std::string kKeys = "Keyboard", kPad = "Gamepad";

        for (const Control &c : kControls)
        {
            if (std::string(c.section) == "Buttons" && std::string(c.id) == "cross")
            {
                // the mouse settings sit between the camera keys and the buttons
                s = {};
                s.key = "mouse.camera";
                s.page = kKeys;
                s.section = "Mouse";
                s.label = "Mouse camera";
                s.help = "Moving the mouse turns the camera, and the mouse is held in the window while you play (free again "
                         "in the menus and when the window is in the background). On foot it turns the camera itself, as "
                         "in a PC game; in vehicles and while aiming it moves the right stick. Off: the mouse only clicks.";
                s.type = Type::Toggle;
                s.defaultValue = 1;
                s.onChange = onChange;
                settings::add(s);

                s = {};
                s.key = "mouse.sensitivity";
                s.page = kKeys;
                s.section = "Mouse";
                s.label = "Mouse sensitivity";
                s.help = "How far the camera turns for a mouse movement (100%: 400 counts, half an inch at 800 DPI, turn the "
                         "camera on foot a radian). Looking up / down and the cameras the stick turns have a top speed "
                         "(the stick's), so very fast flicks there turn no faster.";
                s.type = Type::Integer;
                s.min = 10;
                s.max = 400;
                s.format = "%d%%";
                s.defaultValue = 100;
                s.onChange = onChange;
                settings::add(s);

                s = {};
                s.key = "mouse.invert_y";
                s.page = kKeys;
                s.section = "Mouse";
                s.label = "Invert mouse up / down";
                s.help = "Moving the mouse forward looks down instead of up.";
                s.type = Type::Toggle;
                s.defaultValue = 0;
                s.onChange = onChange;
                settings::add(s);

                s = {};
                s.key = "mouse.free_aim";
                s.page = kKeys;
                s.section = "Mouse";
                s.label = "Free aim";
                s.help = "With a gun, the target button (right mouse button / E) aims where the mouse points, with the "
                         "game's aiming circle, instead of locking on; shots go through the circle. You move while aiming "
                         "as you do locked on with that gun. Fists, melee and scoped weapons lock on as on the PS2.";
                s.type = Type::Toggle;
                s.defaultValue = 0;
                s.onChange = onChange;
                settings::add(s);

                s = {};
                s.key = "mouse.wheel";
                s.page = kKeys;
                s.section = "Mouse";
                s.label = "Mouse wheel";
                s.help = "On foot the wheel changes weapon (down: next, up: previous); in a sniper scope, the camera and "
                         "binoculars it zooms. Off, or with the wheel bound to a control, it does only that.";
                s.type = Type::Toggle;
                s.defaultValue = 1;
                s.onChange = onChange;
                settings::add(s);
            }

            s = {};
            s.key = keyId(c, 0);
            s.page = kKeys;
            s.section = c.section;
            s.label = c.label;
            s.help = c.driving ? (c.target == Target::Stick
                                      ? "While you drive: the left stick in this direction (bikes lean, aircraft pitch). "
                                        "While you drive this key does nothing else."
                                      : std::string("While you drive: ") + c.label + ". While you drive this key does nothing "
                                                                                    "else; on foot it does what its on-foot "
                                                                                    "binding says.")
                 : c.target == Target::Walk
                     ? "While held, you walk instead of running (the left stick at a small tilt, as on the PS2)."
                 : c.target == Target::Stick ? "The PS2's analog stick in this direction, all the way."
                                             : std::string("Presses the PS2's ") + c.label + ".";
            s.type = Type::Key;
            s.optional = true;
            // the driving keys are a group of their own: they may share keys with the on-foot bindings
            s.group = c.driving ? "drive" : "keys";
            s.partner = keyId(c, 1);
            s.defaultValue = c.key1;
            s.onChange = onChange;
            settings::add(s);
            s.key = keyId(c, 1);
            s.partner.clear();
            s.hidden = true;
            s.defaultValue = c.key2;
            settings::add(s);

            if (c.target == Target::Walk)
            {
                s = {};
                s.key = "keys.walk_speed";
                s.page = kKeys;
                s.section = c.section;
                s.label = "Walking speed";
                s.help = "How far the left stick is tilted while Walk is held (the game walks below about half).";
                s.type = Type::Integer;
                s.min = 20;
                s.max = 80;
                s.format = "%d%%";
                s.defaultValue = 45;
                s.onChange = onChange;
                settings::add(s);
            }
        }

        // the menu keys, on the same page (their group keeps them apart from the bindings)
        s = {};
        s.key = "controls.menu_key";
        s.page = kKeys;
        s.section = "Menus";
        s.label = "Settings menu";
        s.help = "Opens and closes this menu. On a gamepad: press Back + Start together.";
        s.type = Type::Key;
        s.group = "keys";
        s.defaultValue = KEY_F2;
        settings::add(s);

        s = {};
        s.key = "controls.trainer_key";
        s.page = kKeys;
        s.section = "Menus";
        s.label = "Trainer menu";
        s.help = "Opens and closes the trainer (god mode, weapons, vehicles, teleports...). On a gamepad: press Back + RB "
                 "together.";
        s.type = Type::Key;
        s.group = "keys";
        s.defaultValue = KEY_F5;
        settings::add(s);

        s = {};
        s.key = "controls.freecam_key";
        s.page = kKeys;
        s.section = "Menus";
        s.label = "Free camera";
        s.help = "Turns the trainer's free camera on and off: fly with W / A / S / D, E and Q, the mouse; Shift faster, "
                 "Ctrl slower. On a gamepad: press Back + LB together.";
        s.type = Type::Key;
        s.group = "keys";
        s.defaultValue = KEY_F6;
        settings::add(s);

        s = {};
        s.key = "controls.screenshot_key";
        s.page = kKeys;
        s.section = "Menus";
        s.label = "Screenshot";
        s.help = "Saves what is on screen (menus and overlays included) as a PNG in the screenshots folder next to vcs.exe.";
        s.type = Type::Key;
        s.group = "keys";
        s.defaultValue = KEY_F8;
        settings::add(s);

        // ---- Gamepad ---------------------------------------------------------------------------------------------
        for (const Control &c : kControls)
        {
            if (c.target != Target::Button || c.driving)
                continue;
            s = {};
            s.key = padId(c);
            s.page = kPad;
            s.section = "Buttons";
            s.label = c.label;
            s.help = std::string("The gamepad button for the PS2's ") + c.label + ". Several PS2 buttons can share one.";
            s.type = Type::Choice;
            s.choices = padChoices();
            s.defaultValue = c.pad;
            s.onChange = onChange;
            settings::add(s);
        }

        struct Percent
        {
            const char *key, *label, *help;
            int min, max, def;
        };
        const Percent sticks[] = {
            {"pad.deadzone_left", "Left stick deadzone", "How far the left stick moves before it counts (worn sticks rest a "
                                                          "little off centre). The rest of its range still reaches full tilt.",
             0, 40, 12},
            {"pad.deadzone_right", "Right stick deadzone", "The same for the right stick (the camera).", 0, 40, 12},
            {"pad.camera_speed", "Camera speed", "How fast the right stick turns the camera (above 100% full speed comes before "
                                                 "full tilt).",
             25, 300, 100},
            {"pad.trigger", "Trigger press point", "How far LT / RT go down before they press (L2 / R2 are buttons on the PS2).",
             5, 95, 30},
        };
        for (const Percent &p : sticks)
        {
            s = {};
            s.key = p.key;
            s.page = kPad;
            s.section = "Sticks and triggers";
            s.label = p.label;
            s.help = p.help;
            s.type = Type::Integer;
            s.min = p.min;
            s.max = p.max;
            s.format = "%d%%";
            s.defaultValue = p.def;
            s.onChange = onChange;
            settings::add(s);
        }
        struct Flag
        {
            const char *key, *label, *help;
        };
        const Flag flags[] = {
            {"pad.swap_sticks", "Swap sticks", "Move with the right stick and turn the camera with the left (left-handed)."},
            {"pad.invert_x", "Invert camera left / right", "The right stick turns the camera the other way."},
            {"pad.invert_y", "Invert camera up / down", "Pushing the right stick up looks down."},
        };
        for (const Flag &f : flags)
        {
            s = {};
            s.key = f.key;
            s.page = kPad;
            s.section = "Sticks and triggers";
            s.label = f.label;
            s.help = f.help;
            s.type = Type::Toggle;
            s.onChange = onChange;
            settings::add(s);
        }

        s = {};
        s.key = "pad.free_aim";
        s.page = kPad;
        s.section = "Sticks and triggers";
        s.label = "Free aim";
        s.help = "With a gun, R1's button (RB unless changed above; give R1 the LT to aim with the trigger) aims where "
                 "the right stick points, with the game's aiming circle, instead of locking on. No aim assist.";
        s.type = Type::Toggle;
        s.defaultValue = 0;
        s.onChange = onChange;
        settings::add(s);

        s = {};
        s.key = "pad.vibration";
        s.page = kPad;
        s.section = "Vibration";
        s.label = "Vibration";
        s.help = "The DualShock 2's rumble on your gamepad when the game asks for it (crashes, gunfire, rough ground): as "
                 "strong as on the PS2 at 100%, weaker or stronger; 0% is off.";
        s.type = Type::Integer;
        s.min = 0;
        s.max = 200;
        s.format = "%d%%";
        s.defaultValue = 100;
        s.onChange = onChange;
        settings::add(s);
    }

    void install(PS2Runtime &runtime)
    {
        g_rdram = runtime.memory().getRDRAM();
        const char *env = std::getenv("VCS_INPUT_MAP");
        g_enabled = !(env && env[0] == '0');
        rebuild();
        if (g_enabled)
            PSPadBackend::setReader(&readPad);
        PSPadBackend::setActuatorSink(&actuators);
        std::fprintf(stderr, "[input] %s\n", g_enabled ? "player bindings" : "built-in mapping (VCS_INPUT_MAP=0)");
        // padstate -> ok buttons=<pressed, hex> rx= ry= lx= ly= reads= (the last report the bindings made, the reads so
        // far; tools/controls_check.py)
        ps2control::addCommand("padstate", [](const std::string &, ps2control::Reply reply) {
            const uint64_t r = g_lastReport.load(std::memory_order_relaxed);
            char text[128];
            std::snprintf(text, sizeof(text), "ok buttons=%04x rx=%u ry=%u lx=%u ly=%u reads=%u", static_cast<unsigned>(~r & 0xFFFF),
                          static_cast<unsigned>(r >> 16 & 0xFF), static_cast<unsigned>(r >> 24 & 0xFF),
                          static_cast<unsigned>(r >> 32 & 0xFF), static_cast<unsigned>(r >> 40 & 0xFF),
                          g_reads.load(std::memory_order_relaxed));
            reply(text);
        });
        // mouse <dx> <dy> -> ok: mouse motion (counts; y grows downwards) for the next pad read, physical input or not
        // (the mouse camera's checks in tools/controls_check.py)
        ps2control::addCommand("mouse", [](const std::string &args, ps2control::Reply reply) {
            float dx = 0.0f, dy = 0.0f;
            if (std::sscanf(args.c_str(), "%f %f", &dx, &dy) != 2)
                return reply("error usage: mouse <dx> <dy>");
            std::lock_guard<std::mutex> lock(g_injectedMutex);
            g_injectedX += dx;
            g_injectedY += dy;
            reply("ok");
        });
        // gamepad on|off | gamepad <a b x y lb rb back start ls rs up down left right> <0|1> | gamepad <lx ly rx ry lt rt>
        // <value> -> ok: a gamepad set from here in place of the real one while on (free aim's checks in
        // tools/controls_check.py; sticks -1..1, down / right +)
        ps2control::addCommand("gamepad", [](const std::string &args, ps2control::Reply reply) {
            static const char *const kButtons[GAMEPAD_BUTTON_COUNT] = {"a",  "b",  "x",  "y",  "back", "start", "ls",
                                                                       "rs", "lb", "rb", "up", "down", "left",  "right"};
            static const char *const kAxes[AXIS_COUNT] = {"lx", "ly", "rx", "ry", "lt", "rt"};
            char name[16] = {};
            float value = 0.0f;
            const int n = std::sscanf(args.c_str(), "%15s %f", name, &value);
            std::lock_guard<std::mutex> lock(g_injectedPadMutex);
            if (n == 1 && (std::strcmp(name, "on") == 0 || std::strcmp(name, "off") == 0))
            {
                g_injectedPad = {};
                g_injectedPad.on = name[1] == 'n';
                return reply("ok");
            }
            for (int b = 0; n == 2 && b < GAMEPAD_BUTTON_COUNT; ++b)
                if (std::strcmp(name, kButtons[b]) == 0)
                {
                    g_injectedPad.buttons[b] = value != 0.0f;
                    return reply("ok");
                }
            for (int a = 0; n == 2 && a < AXIS_COUNT; ++a)
                if (std::strcmp(name, kAxes[a]) == 0)
                {
                    g_injectedPad.axes[a] = std::clamp(value, -1.0f, 1.0f);
                    return reply("ok");
                }
            reply("error usage: gamepad on|off | gamepad <button> <0|1> | gamepad <lx|ly|rx|ry|lt|rt> <value>");
        });
    }

    void onHostFrame(bool menuOpen)
    {
        bool camera = false;
        {
            std::lock_guard<std::mutex> lock(g_mapMutex);
            camera = g_map.mouseCamera || g_map.freeAimMouse;
        }
        setMouseCaptured(((g_enabled && camera) || freecam::flying()) && !menuOpen && ps2control::physicalInputOn());

        // the window in the background (Game > In the background): a hidden test run never is
        const bool behind = !ps2control::hiddenMode() && !windowFocused();
        const int mode = settings::value("game.background");
        ps2AudioSetMuted(behind && mode >= 1);
        const bool pause = behind && mode == 2;
        if (pause != g_backgroundPause.exchange(pause))
        {
            if (pause)
                stopVibration();
            std::fprintf(stderr, "[input] window %s\n", pause ? "in the background: game paused" : "in front: game runs");
        }
    }

    void holdInBackground(PS2Runtime *runtime)
    {
        while (g_backgroundPause.load(std::memory_order_relaxed) && !runtime->isStopRequested())
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    void stopVibration() { gamepadRumble(0.0f, 0.0f); }

    FreeCamInput takeFreeCameraInput()
    {
        Map m;
        {
            std::lock_guard<std::mutex> lock(g_mapMutex);
            m = g_map;
        }
        FreeCamInput in;
        const bool physical = ps2control::physicalInputOn();
        float dx = 0.0f, dy = 0.0f;
        takeMouseMotion(dx, dy);
        if (!physical)
            dx = dy = 0.0f;
        {
            std::lock_guard<std::mutex> lock(g_injectedMutex);
            dx += g_injectedX;
            dy += g_injectedY;
            g_injectedX = g_injectedY = 0.0f;
        }
        if (ps2control::gameInputBlocked()) // a menu has the input: the camera holds still
            return in;

        // W / A / S / D move, E / Space up, Q down, Shift fast, Ctrl slow, Z / X zoom out / in, the mouse looks
        auto key = [](int k) { return ps2control::keyDown(k) ? 1.0f : 0.0f; };
        in.forward = key(KEY_W) - key(KEY_S);
        in.strafe = key(KEY_D) - key(KEY_A);
        in.rise = std::max(key(KEY_E), key(KEY_SPACE)) - key(KEY_Q);
        in.zoom = key(KEY_X) - key(KEY_Z);
        in.fast = bound(KEY_LEFT_SHIFT, false);
        in.slow = bound(KEY_LEFT_CONTROL, false);
        in.turn = -dx * m.mouseLook;
        in.tilt = -dy * m.mouseLook * (m.mouseInvertY ? -1.0f : 1.0f);

        // the gamepad: left stick moves, right stick looks, RT up, LT down, A fast, B slow, D-pad up / down zoom
        if (physical && gamepadConnected())
        {
            float lx = gamepadAxis(AXIS_LEFT_X), ly = gamepadAxis(AXIS_LEFT_Y);
            float rx = gamepadAxis(AXIS_RIGHT_X), ry = gamepadAxis(AXIS_RIGHT_Y);
            if (m.swapSticks)
            {
                std::swap(lx, rx);
                std::swap(ly, ry);
            }
            deadzone(lx, ly, m.deadzoneLeft);
            deadzone(rx, ry, m.deadzoneRight);
            in.strafe = std::clamp(in.strafe + lx, -1.0f, 1.0f);
            in.forward = std::clamp(in.forward - ly, -1.0f, 1.0f);
            in.turnRate = -rx * (m.invertX ? -1.0f : 1.0f) * m.cameraScale;
            in.tiltRate = -ry * (m.invertY ? -1.0f : 1.0f) * m.cameraScale;
            in.rise = std::clamp(in.rise + gamepadAxis(AXIS_RIGHT_TRIGGER) - gamepadAxis(AXIS_LEFT_TRIGGER), -1.0f, 1.0f);
            in.zoom = std::clamp(in.zoom + (gamepadButton(GAMEPAD_DPAD_UP) ? 1.0f : 0.0f) - (gamepadButton(GAMEPAD_DPAD_DOWN) ? 1.0f : 0.0f),
                                 -1.0f, 1.0f);
            in.fast = in.fast || gamepadButton(GAMEPAD_SOUTH);
            in.slow = in.slow || gamepadButton(GAMEPAD_EAST);
        }
        return in;
    }
}
