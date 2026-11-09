// See vcs_dualsense.h.
//
// The DualSense output report SDL sends for us is a DS5EffectsState_t (SDL's SDL_hidapi_ps5.c): 47 bytes, each field a
// byte. SDL_SendGamepadEffect copies our bytes in raw and zeroes the rest, and it builds its own report for the light
// bar (SDL_SetGamepadLED) and the rumble (SDL_RumbleGamepad) from zero too - so a report only touches the fields whose
// enable bits it sets. We set the two trigger-effect enable bits and the trigger arrays and nothing else, so our
// trigger reports leave SDL's light bar and the game's rumble alone, and theirs leave our triggers alone. The light bar
// goes through SDL_SetGamepadLED (platform::gamepadSetLED), which also works on a DualShock 4.
//
// The trigger-effect arrays are the community encoding (Nielk1's TriggerEffectGenerator): a mode byte then a bitfield
// of the trigger's ten zones. SDL never writes them, so this is the only place they are built.
#include "game/vcs_dualsense.h"

#include "platform/platform.h"
#include "ps2_runtime.h"
#include "runtime/ps2_control.h"
#include "settings/settings.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vcs::dualsense
{
    using namespace platform;

    namespace
    {
        // ---- The game (addresses as in game/vcs_input.cpp and vcs_trainer.cpp) --------------------------------------
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        constexpr uint32_t kPlayerPed = 0x004E4910u; // CWorld::Players[0].m_pPed (0 before a game is running)
        constexpr uint32_t kPedVehicle = 0x480u;     // the vehicle the ped is in (0 on foot)
        constexpr uint32_t kPedState = 0x8C4u;        // 0x37 while driving
        constexpr uint32_t kPedHealth = 0x4E4u;       // float, 100 full for Vic
        constexpr uint32_t kDrivingState = 0x37u;
        constexpr uint32_t kUserPause = 0x004CD2ACu;  // the pause menu (byte)
        constexpr uint32_t kCodePause = 0x004CD11Cu;  // the game's own pause (byte)
        // The wanted level (police stars) as a 32-bit int inside the player ped, at ped + 0x920. Found by a
        // changed-value memory scan while the trainer set the level 0..6: it read 0,1,2,3,4 and then held (this save,
        // early in the story, caps the wanted level at 4). The byte follows the stars exactly (game-internals.md 12e).
        // Override the offset at runtime with VCS_WANTED_OFF=<hex> if a build disagrees; the `dualsense` command shows
        // the live value.
        constexpr uint32_t kPedWanted = 0x920u;

        uint8_t *g_rdram = nullptr;
        uint32_t g_wantedOffset = kPedWanted;

        uint32_t rd32(uint32_t a)
        {
            uint32_t v = 0;
            if (g_rdram)
                std::memcpy(&v, g_rdram + (a & kRamMask), 4);
            return v;
        }
        float rdf(uint32_t a)
        {
            float v = 0.0f;
            if (g_rdram)
                std::memcpy(&v, g_rdram + (a & kRamMask), 4);
            return v;
        }
        uint8_t rd8(uint32_t a) { return g_rdram ? g_rdram[a & kRamMask] : 0; }

        struct GameState
        {
            bool inGame = false; // a player ped exists and the game is not paused
            bool driving = false;
            int wanted = 0;      // 0..6 police stars
            float health = 0.0f; // 0..100
            float speed = 0.0f;  // the vehicle's speed, units per 1/50 s (game-internals.md 12a); ~0.3 is fast
            int vehType = 0;     // 0 car, 1 boat, 4 heli, 6 bike (game-internals.md 12e)
            float mass = 0.0f;   // the vehicle's mass (moped ~350, car ~1500, truck ~10000, tank ~25000)
            float slip = 0.0f;   // 0 going straight .. 1 sliding fully sideways (traction loss)
            int weapon = 0;      // on foot: the gun type in hand (0 = none / not a gun)
        };

        constexpr uint32_t kVehicleVel = 0x140u;  // the vehicle's velocity vector (3 floats), units per 1/50 s
        constexpr uint32_t kVehicleFwd = 0x010u;  // the vehicle's forward vector (3 floats)
        constexpr uint32_t kVehicleMass = 0x0D0u; // the vehicle's mass (float)
        constexpr uint32_t kVehicleType = 0x354u; // 0 car, 1 boat, 4 heli, 6 bike
        constexpr uint32_t kPedSlot = 0x789u;     // the active weapon slot (s8), game-internals.md 12c
        constexpr uint32_t kPedWeapons = 0x578u;  // slot n's weapon at +kPedWeapons + n*0x1C (type first)

        // The gun types (game/vcs_aim.cpp isGun): 19..29 pistols / shotguns / SMGs / rifles, 33..35 flame-thrower /
        // M249 / minigun. Automatics fire while held (SMGs, rifles, the Spaz, M249, minigun, flame-thrower); the rest
        // (pistols, pump shotguns) are one shot a pull.
        bool isGun(int t) { return (t >= 19 && t <= 29) || (t >= 33 && t <= 35); }
        bool isAutomatic(int t) { return t == 22 || (t >= 24 && t <= 29) || (t >= 33 && t <= 35); }

        GameState gameState()
        {
            GameState s;
            if (!g_rdram)
                return s;
            const uint32_t ped = rd32(kPlayerPed);
            const bool paused = rd8(kUserPause) || rd8(kCodePause);
            s.inGame = ped != 0 && !paused;
            if (!s.inGame)
                return s;
            const uint32_t vehicle = rd32(ped + kPedVehicle);
            s.driving = vehicle != 0 && rd32(ped + kPedState) == kDrivingState;
            if (s.driving)
            {
                const float vx = rdf(vehicle + kVehicleVel), vy = rdf(vehicle + kVehicleVel + 4), vz = rdf(vehicle + kVehicleVel + 8);
                s.speed = std::sqrt(vx * vx + vy * vy + vz * vz);
                s.vehType = static_cast<int>(rd32(vehicle + kVehicleType));
                s.mass = rdf(vehicle + kVehicleMass);
                // sideways slide: how much of the horizontal speed is across the vehicle's facing, not along it
                const float fx = rdf(vehicle + kVehicleFwd), fy = rdf(vehicle + kVehicleFwd + 4);
                const float speedH = std::sqrt(vx * vx + vy * vy), fh = std::sqrt(fx * fx + fy * fy);
                if (speedH > 0.02f && fh > 0.01f)
                {
                    const float along = (vx * fx + vy * fy) / fh;
                    const float lateral = std::sqrt(std::max(0.0f, speedH * speedH - along * along));
                    s.slip = std::min(1.0f, lateral / speedH);
                }
            }
            else // on foot: the gun in hand, for the fire trigger's weapon effect
            {
                const int slot = static_cast<int8_t>(rd8(ped + kPedSlot));
                if (slot >= 0 && slot < 10)
                {
                    const int wt = static_cast<int>(rd32(ped + kPedWeapons + static_cast<uint32_t>(slot) * 0x1Cu));
                    if (isGun(wt))
                        s.weapon = wt;
                }
            }
            s.health = rdf(ped + kPedHealth);
            const uint32_t w = rd32(ped + g_wantedOffset);
            s.wanted = w <= 6 ? static_cast<int>(w) : 0; // a bad read counts as "no stars", never a false flash
            return s;
        }

        // ---- Settings, cached on change (main thread) ---------------------------------------------------------------
        enum LightbarMode { LB_OFF = 0, LB_HEALTH = 1, LB_THEME = 2 };
        struct Config
        {
            int lightbar = LB_THEME;
            bool police = true;
            bool triggers = true;
            bool triggerDrive = false; // R2 / L2 are the throttle / brake (pad.trigger_drive): live resistance
            bool triggerShoot = false; // R2 fires on foot (pad.trigger_shoot): the gun's trigger feel
            bool speaker = false;      // route the game's sound to the DualSense's speaker
            float brightness = 1.0f;   // 0..1
        };
        Config g_cfg;
        void applySpeaker(); // below

        void rebuild()
        {
            g_cfg.lightbar = settings::value("pad.ds_lightbar");
            g_cfg.police = settings::value("pad.ds_police") != 0;
            g_cfg.triggers = settings::value("pad.ds_triggers") != 0;
            g_cfg.triggerDrive = settings::value("pad.trigger_drive") != 0;
            g_cfg.triggerShoot = settings::value("pad.trigger_shoot") != 0;
            g_cfg.speaker = settings::value("pad.ds_speaker") != 0;
            g_cfg.brightness = std::clamp(settings::value("pad.ds_brightness") / 100.0f, 0.1f, 1.0f);
            applySpeaker();
        }

        // ---- The game's rumble intent (game thread -> main thread), held briefly so a single pulse is felt ----------
        std::atomic<uint32_t> g_actSmall{0}; // the fast motor, 0 / 1
        std::atomic<uint32_t> g_actLarge{0}; // the slow motor, 0..255
        std::atomic<int64_t> g_actNs{0};     // when either was last non-zero
        constexpr int64_t kActHoldNs = 110000000; // 110 ms
        std::atomic<int64_t> g_fireNs{0};    // when the player last fired a gun on foot (onShooting)
        constexpr int64_t kFireHoldNs = 90000000; // 90 ms: a buzz lingers past a single shot
        float g_prevSpeed = 0.0f;            // last frame's vehicle speed, for acceleration (main thread)
        int64_t g_prevSpeedNs = 0;

        int64_t nowNs()
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // ---- The DualSense trigger-effect arrays (11 bytes each; Nielk1's encoding) ---------------------------------
        using Trigger = std::array<uint8_t, 11>;

        Trigger triggerOff()
        {
            Trigger t{};
            t[0] = 0x05; // Off
            return t;
        }
        // Uniform resistance from `position` (0..9) to the end, `strength` 1..8.
        Trigger triggerFeedback(int position, int strength)
        {
            if (strength <= 0)
                return triggerOff();
            position = std::clamp(position, 0, 9);
            strength = std::clamp(strength, 1, 8);
            const uint32_t force = static_cast<uint32_t>((strength - 1) & 0x07);
            uint32_t forceZones = 0;
            uint16_t activeZones = 0;
            for (int i = position; i < 10; ++i)
            {
                forceZones |= force << (3 * i);
                activeZones |= static_cast<uint16_t>(1u << i);
            }
            Trigger t{};
            t[0] = 0x21; // Feedback
            t[1] = static_cast<uint8_t>(activeZones & 0xFF);
            t[2] = static_cast<uint8_t>(activeZones >> 8);
            t[3] = static_cast<uint8_t>(forceZones & 0xFF);
            t[4] = static_cast<uint8_t>((forceZones >> 8) & 0xFF);
            t[5] = static_cast<uint8_t>((forceZones >> 16) & 0xFF);
            t[6] = static_cast<uint8_t>((forceZones >> 24) & 0xFF);
            return t;
        }
        // A buzz from `position` to the end while the trigger is pressed; `amplitude` 1..8, `frequency` in Hz.
        Trigger triggerVibration(int position, int amplitude, int frequency)
        {
            if (amplitude <= 0 || frequency <= 0)
                return triggerOff();
            position = std::clamp(position, 0, 9);
            amplitude = std::clamp(amplitude, 1, 8);
            const uint32_t amp = static_cast<uint32_t>((amplitude - 1) & 0x07);
            uint32_t ampZones = 0;
            uint16_t activeZones = 0;
            for (int i = position; i < 10; ++i)
            {
                ampZones |= amp << (3 * i);
                activeZones |= static_cast<uint16_t>(1u << i);
            }
            Trigger t{};
            t[0] = 0x26; // Vibration
            t[1] = static_cast<uint8_t>(activeZones & 0xFF);
            t[2] = static_cast<uint8_t>(activeZones >> 8);
            t[3] = static_cast<uint8_t>(ampZones & 0xFF);
            t[4] = static_cast<uint8_t>((ampZones >> 8) & 0xFF);
            t[5] = static_cast<uint8_t>((ampZones >> 16) & 0xFF);
            t[6] = static_cast<uint8_t>((ampZones >> 24) & 0xFF);
            t[9] = static_cast<uint8_t>(std::clamp(frequency, 1, 255));
            return t;
        }

        // A resistance that builds between `start` and `end` (0..9) and gives way past it - a gun's trigger: you pull
        // against it and it "breaks" as the shot fires. start 2..7, end start+1..8, strength 1..8.
        Trigger triggerWeapon(int start, int end, int strength)
        {
            start = std::clamp(start, 2, 7);
            end = std::clamp(end, start + 1, 8);
            strength = std::clamp(strength, 1, 8);
            Trigger t{};
            t[0] = 0x25; // Weapon
            const uint16_t zones = static_cast<uint16_t>((1u << start) | (1u << end));
            t[1] = static_cast<uint8_t>(zones & 0xFF);
            t[2] = static_cast<uint8_t>(zones >> 8);
            t[3] = static_cast<uint8_t>(strength - 1);
            return t;
        }

        // The DS5EffectsState_t enable bits for the trigger motors (the DualSense convention; SDL leaves these to us).
        constexpr uint8_t kEnableRightTrigger = 0x04;
        constexpr uint8_t kEnableLeftTrigger = 0x08;

        // ---- What was last sent, so a report goes only on a change --------------------------------------------------
        bool g_ledValid = false;
        uint8_t g_ledR = 0, g_ledG = 0, g_ledB = 0;
        bool g_trigValid = false;
        std::array<uint8_t, 47> g_trig{};
        DsPadFamily g_lastFamily = GAMEPAD_FAMILY_GENERIC;

        void sendLED(uint8_t r, uint8_t g, uint8_t b)
        {
            if (g_ledValid && r == g_ledR && g == g_ledG && b == g_ledB)
                return;
            if (!gamepadSetLED(r, g, b)) // e.g. a DualSense still enabling its enhanced reports: retry next frame
                return;
            g_ledValid = true;
            g_ledR = r;
            g_ledG = g;
            g_ledB = b;
        }

        void sendTriggers(const Trigger &right, const Trigger &left)
        {
            std::array<uint8_t, 47> report{};
            report[0] = kEnableRightTrigger | kEnableLeftTrigger; // ucEnableBits1
            std::memcpy(report.data() + 10, right.data(), right.size());
            std::memcpy(report.data() + 21, left.data(), left.size());
            if (g_trigValid && report == g_trig)
                return;
            if (gamepadSendEffect(report.data(), static_cast<int>(report.size())))
            {
                g_trig = report;
                g_trigValid = true;
            }
        }

        // ---- The light bar colour ----------------------------------------------------------------------------------
        void lightbarColour(const GameState &game, uint8_t &r, uint8_t &g, uint8_t &b)
        {
            r = g = b = 0;
            // The police flash wins over the base colour, like GTA V: red / blue, faster with more stars.
            if (g_cfg.police && game.inGame && game.wanted > 0)
            {
                // Alternate red / blue like a police light bar: a calm pace (GTA V's), a little quicker with more
                // stars. periodMs is one full red+blue cycle, so each colour shows half of it.
                const int periodMs = std::max(640, 1100 - game.wanted * 70);
                const bool blue = (nowNs() / (static_cast<int64_t>(periodMs) * 1000000 / 2)) & 1;
                if (blue)
                    b = 255;
                else
                    r = 255;
                return;
            }
            switch (g_cfg.lightbar)
            {
            case LB_HEALTH:
            {
                const float t = std::clamp(game.health / 100.0f, 0.0f, 1.0f); // red (hurt) -> green (full)
                r = static_cast<uint8_t>((1.0f - t) * 220.0f);
                g = static_cast<uint8_t>(t * 200.0f);
                b = 0;
                break;
            }
            case LB_THEME: // Vice City neon pink
                r = 255;
                g = 45;
                b = 130;
                break;
            case LB_OFF:
            default:
                break;
            }
        }

        // ---- The adaptive triggers ---------------------------------------------------------------------------------
        // How hard the trigger pushes back for a vehicle's mass (vehicle +0xD0; moped ~350, car ~1500, van ~2600,
        // truck ~3800, big truck / tank 10000+). Strength 3..8 (1..2 are barely felt), spread across what you actually
        // drive so a bike and a van feel clearly different, not only a car and a tank.
        int massStrength(float m)
        {
            if (m < 700.0f) return 3;   // mopeds, bikes
            if (m < 1300.0f) return 4;  // small cars
            if (m < 1900.0f) return 5;  // cars
            if (m < 2800.0f) return 6;  // big cars, vans (Boxville)
            if (m < 5000.0f) return 7;  // trucks
            return 8;                   // the heaviest (fire truck, tank)
        }
        // Where the resistance begins in the pull (0 earliest .. 9 latest): a heavier vehicle resists through more of
        // the travel, a light one only near the top. A second cue for weight on top of the strength.
        int massPosition(float m)
        {
            if (m < 700.0f) return 4;
            if (m < 1900.0f) return 3;
            if (m < 3500.0f) return 2;
            return 1;
        }

        void chooseTriggers(const GameState &game, Trigger &right, Trigger &left)
        {
            right = triggerOff();
            left = triggerOff();
            if (!g_cfg.triggers || !game.inGame)
            {
                g_prevSpeedNs = 0;
                return;
            }

            const int64_t now = nowNs();
            const int64_t actAge = now - g_actNs.load(std::memory_order_relaxed);
            const int large = (actAge >= 0 && actAge < kActHoldNs) ? static_cast<int>(g_actLarge.load(std::memory_order_relaxed)) : 0;
            const int64_t fireAge = now - g_fireNs.load(std::memory_order_relaxed);
            const bool firing = fireAge >= 0 && fireAge < kFireHoldNs;

            if (game.driving)
            {
                // Weight sets the base (mass at vehicle +0xD0, game-internals.md 12e): strength 3..8 and how early in the
                // pull it resists. A crash / heavy bump (the game's big-motor rumble) judders both.
                const int str = massStrength(game.mass);
                const int pos = massPosition(game.mass);
                // acceleration (change in speed per second), for the dynamic throttle / brake
                float accel = 0.0f;
                if (g_prevSpeedNs)
                {
                    const float dt = static_cast<float>(now - g_prevSpeedNs) / 1e9f;
                    if (dt > 0.001f && dt < 0.5f)
                        accel = (game.speed - g_prevSpeed) / dt;
                }
                g_prevSpeed = game.speed;
                g_prevSpeedNs = now;

                if (large > 70)
                {
                    const int amp = std::clamp(1 + large * 7 / 255, 1, 8);
                    right = triggerVibration(1, amp, 9);
                    left = triggerVibration(1, amp, 9);
                }
                else if (g_cfg.triggerDrive)
                {
                    // R2 is the throttle, L2 the brake - a live feel like GT7 / GTA V. The throttle buzzes when the
                    // vehicle loses grip (wheelspin / a slide), otherwise firms up the harder you accelerate; the brake
                    // firms up the harder you press (see below).
                    if (game.slip > 0.4f && game.speed > 0.06f)
                    {
                        const int amp = std::clamp(2 + static_cast<int>(game.slip * 4.0f), 2, 6);
                        right = triggerVibration(pos, amp, 22); // a smooth buzz, not clicky
                    }
                    else
                    {
                        const int t = std::clamp(str + static_cast<int>(std::max(0.0f, accel) * 14.0f), str, 8);
                        right = triggerFeedback(pos, t);
                    }
                    // No ABS on a 1984 car (and a trigger flutter clicks audibly): the brake simply gets firmer the
                    // harder you press (stronger deceleration), like a hydraulic pedal, and stays silent.
                    const int brake = std::clamp(str + 1 + static_cast<int>(std::max(0.0f, -accel) * 8.0f), str, 8);
                    left = triggerFeedback(std::max(0, pos - 1), brake);
                }
                else
                {
                    // R2 / L2 only look around: steady weight-based resistance (turn on "Drive with the triggers" for the
                    // live throttle / brake above).
                    right = triggerFeedback(pos, str);
                    left = triggerFeedback(pos, str);
                }
            }
            else
            {
                g_prevSpeedNs = 0;
                // On foot, the fire trigger (R2) feels the gun - but only with "Shoot with the triggers" on, where R2
                // is the trigger you actually pull. (Firing with Circle leaves R2 alone, so it does not buzz for a
                // trigger you are not using.) A single-shot gun gives a firm wall that breaks at the shot; an automatic
                // rests on a light wall and buzzes while it fires.
                if (g_cfg.triggerShoot && isGun(game.weapon))
                {
                    if (isAutomatic(game.weapon))
                        right = firing ? triggerVibration(0, 7, 16) : triggerWeapon(3, 6, 4);
                    else
                        right = triggerWeapon(4, 7, 7);
                }
            }
        }

        // Route the game's sound to the DualSense's speaker when the option is on and one is connected, else back to the
        // system default. The DualSense's speaker is a playback device named "Wireless Controller" on Windows.
        void applySpeaker()
        {
            const bool on = g_cfg.speaker && dsPadFamily() == GAMEPAD_FAMILY_PS5;
            audioUseDevice(on ? "Wireless Controller" : nullptr);
        }

        bool g_released = true; // true after reset(): nothing on the pad

        void releaseNow()
        {
            if (g_released)
                return;
            sendLED(0, 0, 0);
            sendTriggers(triggerOff(), triggerOff());
            g_released = true;
        }
    }

    void registerSettings()
    {
        using settings::Type;
        auto onChange = [](int) { rebuild(); };
        settings::Spec s;

        s = {};
        s.key = "pad.ds_lightbar";
        s.page = "Gamepad";
        s.section = "DualSense";
        s.label = "Light bar";
        s.help = "The colour of a DualSense's or DualShock 4's light bar during play. Health: green when full, red when "
                 "hurt. Vice theme: a steady neon pink. Off: dark. A wanted level flashes it red / blue on top (below).";
        s.type = Type::Choice;
        s.choices = {{"Off", LB_OFF}, {"Health", LB_HEALTH}, {"Vice theme", LB_THEME}};
        s.defaultValue = LB_THEME;
        s.onChange = onChange;
        settings::add(s);

        s = {};
        s.key = "pad.ds_brightness";
        s.page = "Gamepad";
        s.section = "DualSense";
        s.label = "Light bar brightness";
        s.help = "How bright the light bar is.";
        s.type = Type::Integer;
        s.min = 10;
        s.max = 100;
        s.format = "%d%%";
        s.defaultValue = 100;
        s.onChange = onChange;
        settings::add(s);

        s = {};
        s.key = "pad.ds_police";
        s.page = "Gamepad";
        s.section = "DualSense";
        s.label = "Police flash";
        s.help = "The light bar flashes red and blue while the police are after you (a wanted level), faster the more "
                 "stars - as the lights do in GTA V. Turns the light bar back to its colour above when you lose them.";
        s.type = Type::Toggle;
        s.defaultValue = 1;
        s.onChange = onChange;
        settings::add(s);

        s = {};
        s.key = "pad.ds_triggers";
        s.page = "Gamepad";
        s.section = "DualSense";
        s.label = "Adaptive triggers";
        s.help = "On a DualSense, R2 / L2 push back while you drive - heavier vehicles (a truck, a tank) resist far more "
                 "than a moped, and both judder on a crash or rough ground. With \"Drive with the triggers\" on (Sticks "
                 "and triggers) the throttle also firms up as you accelerate and flutters on a slide, and the brake "
                 "pulses like ABS. With \"Shoot with the triggers\" on, the fire trigger feels each gun. No effect on "
                 "other pads.";
        s.type = Type::Toggle;
        s.defaultValue = 1;
        s.onChange = onChange;
        settings::add(s);

        s = {};
        s.key = "pad.ds_speaker";
        s.page = "Gamepad";
        s.section = "DualSense";
        s.label = "Sound through the controller";
        s.help = "Play the game's sound - the radio, the police scanner, everything - through the DualSense's built-in "
                 "speaker instead of your speakers or headphones. The controller's speaker is small, so it sounds "
                 "thinner. Needs the DualSense connected; turns itself off when it is unplugged.";
        s.type = Type::Toggle;
        s.defaultValue = 0;
        s.onChange = onChange;
        settings::add(s);
    }

    void install(PS2Runtime &runtime)
    {
        g_rdram = runtime.memory().getRDRAM();
        if (const char *env = std::getenv("VCS_WANTED_OFF"))
        {
            const uint32_t off = static_cast<uint32_t>(std::strtoul(env, nullptr, 0));
            if (off)
            {
                g_wantedOffset = off;
                std::fprintf(stderr, "[dualsense] wanted level at ped + 0x%X (VCS_WANTED_OFF)\n", g_wantedOffset);
            }
        }
        rebuild();

        // dualsense -> ok family=<generic|ps4|ps5> led=<0|1> triggers=<0|1> wanted=<n>@ped+<hex> health=<h>
        // driving=<0|1> ingame=<0|1>: the pad's features and the game reads that drive them (tools/).
        ps2control::addCommand("dualsense", [](const std::string &, ps2control::Reply reply) {
            const GameState g = gameState();
            const char *fam = dsPadFamily() == GAMEPAD_FAMILY_PS5   ? "ps5"
                              : dsPadFamily() == GAMEPAD_FAMILY_PS4 ? "ps4"
                                                                      : "generic";
            char text[256];
            std::snprintf(text, sizeof(text),
                          "ok family=%s led=%d triggers=%d wanted=%d@ped+0x%X health=%.0f driving=%d ingame=%d "
                          "speed=%.2f vehtype=%d mass=%.0f slip=%.2f weapon=%d",
                          fam, gamepadHasLED() ? 1 : 0, gamepadHasTriggerEffects() ? 1 : 0, g.wanted, g_wantedOffset,
                          g.health, g.driving ? 1 : 0, g.inGame ? 1 : 0, g.speed, g.vehType, g.mass, g.slip, g.weapon);
            reply(text);
        });
        std::fprintf(stderr, "[dualsense] ready\n");
    }

    void onActuators(unsigned small, unsigned large)
    {
        g_actSmall.store(small ? 1u : 0u, std::memory_order_relaxed);
        g_actLarge.store(std::min(large, 255u), std::memory_order_relaxed);
        if (small || large)
            g_actNs.store(nowNs(), std::memory_order_relaxed);
    }

    void onShooting(bool firing)
    {
        if (firing)
            g_fireNs.store(nowNs(), std::memory_order_relaxed);
    }

    void onHostFrame(bool menuOpen)
    {
        const DsPadFamily family = dsPadFamily();
        if (family != g_lastFamily) // a pad came or went: re-send everything next, and re-route the speaker
        {
            g_ledValid = g_trigValid = false;
            g_lastFamily = family;
            applySpeaker();
        }

        const bool canLED = gamepadHasLED();
        const bool canTriggers = gamepadHasTriggerEffects();
        // Nothing to drive, a hidden test run, or the window in the background: let the pad go, once. (Not gated on
        // physicalInputOn, which also goes false while a cutscene blocks input - the light bar should stay lit then,
        // not go dark the moment you are arrested and lose your stars.)
        if ((!canLED && !canTriggers) || ps2control::hiddenMode() || !windowFocused())
        {
            releaseNow();
            return;
        }

        const GameState game = gameState();

        if (canLED && !menuOpen) // a host menu leaves the light bar at its last colour
        {
            uint8_t r = 0, g = 0, b = 0;
            lightbarColour(game, r, g, b);
            sendLED(static_cast<uint8_t>(r * g_cfg.brightness), static_cast<uint8_t>(g * g_cfg.brightness),
                    static_cast<uint8_t>(b * g_cfg.brightness));
        }
        if (canTriggers)
        {
            Trigger right, left;
            if (menuOpen)
                right = left = triggerOff(); // the triggers scroll menus: no resistance there
            else
                chooseTriggers(game, right, left);
            sendTriggers(right, left);
        }
        g_released = false;
    }

    void reset()
    {
        releaseNow();
        g_ledValid = g_trigValid = false; // the next frame re-sends from scratch
    }
}
