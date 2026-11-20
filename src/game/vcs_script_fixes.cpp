// Mission scripts that got stuck above 30 fps (docs/game-internals.md 11h; GitHub issue 41).
//
// GON_C3 "Purple Haze" (mission 94): at the meeting place a truck rams the player's van (SET_CAR_FORWARD_SPEED 25,
// SET_CAR_RAM_CAR), the screen fades out, the two thugs are created beside the van, and half a second later the
// mission parks the truck out of the way. At 30 fps the truck hits the van squarely, pushes it and has stopped behind
// it by then. With a shorter frame the same hit turns the van instead of pushing it (the contact differs with how far
// the truck gets into the van in one step), the truck rolls on past it at 8 m/s - and over the spot where the first
// thug is created. Dead, he takes his dialogue line with him: the audio monitor script (AUDIMON) frees the slot of a
// line whose speaker died, and the mission, which had asked for the line and waits for it to load, never looks again.
// The screen stays faded out: the game's "Loading..." for ever.
//
// Two fixes, both in the script VM's handlers (the IP is past the opcode when a handler runs, the script in a0):
// - The wait loop (main script 0xA9F9: `NOT IS_ANY_BIT_SET 4 $G606[1L]`, repeated every frame until the slot is
//   "loaded") gets the mask 5: loaded, or free again. The script then goes on without the line, the way it does when
//   a speaker dies after his line has loaded. The same loop held MAR_A1 "Shakedown" (game-internals.md 12).
// - Purple Haze's truck is parked before the thugs are created: the commands of the mission's own block that parks
//   it (+0x4229 .. +0x4261: coordinates, heading, status, no mission, speed 0, frozen) run as a script of their own
//   from the next frame on, when the mission reaches the first CREATE_CHAR (+0x41A2). The screen is black then and
//   the mission runs its block half a second later, so nothing else changes. Not at the PS2's own frame (original
//   timing at 30 fps), where the truck is standing by then.
#include "game/vcs_script_fixes.h"

#include "debug/rig.h"
#include "game/vcs_framerate.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vcs
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        constexpr uint32_t kScriptSpace = 0x0048741Cu;    // -> MAIN.SCM in memory; an IP is an offset from it
        constexpr uint32_t kMissionStart = 0x004CD3D8u;   // IP of the loaded mission's first byte
        constexpr uint32_t kMissionRunning = 0x004CD3E7u; // u8
        constexpr uint32_t kVehiclePool = 0x00487A98u;    // -> CPool {entries, flags, size} (game-internals.md 12)

        // CRunningScript (game-internals.md 11b)
        constexpr uint32_t kScriptIp = 0x10u, kScriptLocals = 0x54u, kScriptFrame = 0x1FCu;

        // the handlers (opcode table 0x4BEAF0, read in the running game 2026-11-10)
        constexpr uint32_t kHandlerAnyBitSet = 0x00100908u;  // 048F IS_ANY_BIT_SET
        constexpr uint32_t kHandlerCreateChar = 0x004120D8u; // 003E CREATE_CHAR

        // main script: NOT IS_ANY_BIT_SET 4 $G606[1L sz5] - the mask is the fourth byte
        constexpr uint32_t kAudioWait = 0xA9F9u;
        constexpr uint8_t kAudioWaitCode[] = {0x8F, 0x84, 0x07, 0x04, 0xEC, 0x06, 0x01, 0x05};
        constexpr uint32_t kAudioSlots = 0x606u; // $G606[5]: 1 free, 2 loading, 4 loaded, 8 playing (+32 locked, +64 cancel)

        // GON_C3: CREATE_CHAR 22 22 36L[1L] 38L[1L] -100.0 $G1602 (the first thug)
        constexpr uint32_t kHazeThug = 0x41A2u;
        constexpr uint8_t kHazeThugCode[] = {0x3E, 0x00, 0x07, 0x16, 0x07, 0x16, 0x91, 0x01, 0x02,
                                             0x93, 0x01, 0x02, 0x04, 0xC8, 0xC2, 0xE3, 0x02};
        // ... and its block that parks the truck $G151E at ($G15AB, $G15AC, $G15AD): eight commands (ground height,
        // coordinates, heading 177.691, status 3, no mission, forward and cruise speed 0, frozen), known here by their
        // length and checksum (FNV-1a 64) - the game's own script is not kept in this repository
        constexpr uint32_t kHazePark = 0x4229u, kHazeParkSize = 56u;
        constexpr uint64_t kHazeParkHash = 0x9AE2DDDA4D4104C1ull;
        constexpr uint32_t kHazeTruck = 0x151Eu; // the global with the truck's handle
        // What runs early: the same commands without the ground height. GET_GROUND_Z_FOR_3D_COORD writes the height
        // back into $G15AD, and run a second time from there it found the next surface down - the mission's own block
        // then parked the truck under the street (z 1.7 for 10.7). SET_CAR_COORDINATES finds the ground itself for
        // z = -100, and the mission's globals stay as the PS2 has them.
        constexpr uint8_t kHazeFixCode[] = {
            0x4E, 0x00, 0xE2, 0x1E, 0xE2, 0xAB, 0xE2, 0xAC, 0x09, 0x00, 0x00, 0xC8, 0xC2, // SET_CAR_COORDINATES x y -100.0
            0xD1, 0x00, 0xE2, 0x1E, 0x09, 0xCB, 0xB0, 0x31, 0x43,                         // SET_CAR_HEADING 177.691
            0x36, 0x02, 0xE2, 0x1E, 0x07, 0x03,                                           // SET_CAR_STATUS 3
            0x52, 0x00, 0xE2, 0x1E, 0x01,                                                 // SET_CAR_MISSION 0
            0xE9, 0x02, 0xE2, 0x1E, 0x01,                                                 // SET_CAR_FORWARD_SPEED 0
            0x50, 0x00, 0xE2, 0x1E, 0x01,                                                 // SET_CAR_CRUISE_SPEED 0
            0x25, 0x03, 0xE2, 0x1E, 0x07, 0x01,                                           // FREEZE_CAR_POSITION 1
            0x23, 0x00};                                                                  // TERMINATE_THIS_SCRIPT

        PS2Runtime::RecompiledFunction g_origAnyBitSet = nullptr, g_origCreateChar = nullptr;

        uint32_t read32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (address & kRamMask), 4);
            return v;
        }

        uint64_t fnv1a(const uint8_t *bytes, uint32_t count)
        {
            uint64_t h = 0xCBF29CE484222325ull;
            for (uint32_t i = 0; i < count; ++i)
                h = (h ^ bytes[i]) * 0x100000001B3ull;
            return h;
        }

        bool enabled()
        {
            static const bool on = [] {
                const char *v = std::getenv("VCS_SCRIPT_FIXES");
                return !(v && v[0] == '0');
            }();
            return on;
        }
        // the frame isn't the PS2's 1/30 s: independent timing, or original timing at 60 (one VBlank a frame)
        bool shortFrames() { return timing() == Timing::Independent || frameCap() != 30; }

        // A vehicle handle that names a vehicle in the pool (slot = handle >> 8, the low byte the slot's flag byte)
        bool vehicleExists(const uint8_t *rdram, uint32_t handle)
        {
            const uint32_t pool = read32(rdram, kVehiclePool);
            if (!pool || static_cast<int32_t>(handle) <= 0)
                return false;
            const uint32_t flags = read32(rdram, pool + 4), size = read32(rdram, pool + 8), slot = handle >> 8;
            return slot < size && rdram[(flags + slot) & kRamMask] == (handle & 0xFFu);
        }

        PS2_GUEST_CC void anyBitSetHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t script = GPR_U32(ctx, 4);
            if (enabled() && read32(rdram, script + kScriptIp) - 2u == kAudioWait)
            {
                const uint32_t space = read32(rdram, kScriptSpace);
                uint8_t *code = rdram + ((space + kAudioWait) & kRamMask);
                if (std::memcmp(code, kAudioWaitCode, sizeof(kAudioWaitCode)) == 0)
                {
                    code[3] = 0x05; // loaded (4) or free again (1); a load of the script file puts the 4 back
                    std::fprintf(stderr, "[script-fix] mission audio: the wait for a line to load also ends when its slot is freed\n");
                }
                // the slot this pass looks at ($G606[1L]): free means the line was dropped while the script waited
                const int32_t slot = static_cast<int32_t>(read32(rdram, script + kScriptLocals + 4u * (read32(rdram, script + kScriptFrame) + 1u)));
                if (code[3] == 0x05 && slot >= 0 && slot < 5 && (read32(rdram, space + 4u * (kAudioSlots + static_cast<uint32_t>(slot))) & 1u))
                    std::fprintf(stderr, "[script-fix] mission audio slot %d was freed while its line loaded (the speaker died?): "
                                         "the script goes on without the line\n", slot);
            }
            g_origAnyBitSet(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void createCharHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t script = GPR_U32(ctx, 4);
            if (enabled() && shortFrames() && rdram[kMissionRunning & kRamMask])
            {
                const uint32_t space = read32(rdram, kScriptSpace), mission = read32(rdram, kMissionStart);
                const auto at = [&](uint32_t offset) { return rdram + ((space + mission + offset) & kRamMask); };
                if (read32(rdram, script + kScriptIp) - 2u == mission + kHazeThug &&
                    std::memcmp(at(kHazeThug), kHazeThugCode, sizeof(kHazeThugCode)) == 0 &&
                    fnv1a(at(kHazePark), kHazeParkSize) == kHazeParkHash &&
                    vehicleExists(rdram, read32(rdram, space + 4u * kHazeTruck)))
                {
                    std::fprintf(stderr, "[script-fix] Purple Haze: the truck is parked before the thugs are created\n");
                    // between frames, as every script we start (debug/rig.h): it runs with the next frame's scripts
                    rig::runOnGameThread([](uint8_t *ram, R5900Context *, PS2Runtime *) {
                        const uint32_t space = read32(ram, kScriptSpace);
                        if (!ram[kMissionRunning & kRamMask] || !vehicleExists(ram, read32(ram, space + 4u * kHazeTruck)))
                            return;
                        if (!rig::startCodeScript(ram, kHazeFixCode, sizeof(kHazeFixCode), "hazefix"))
                            std::fprintf(stderr, "[script-fix] Purple Haze: no free script for the truck\n");
                    });
                }
            }
            g_origCreateChar(rdram, ctx, runtime);
        }
    }

    void installScriptFixHooks(PS2Runtime &runtime)
    {
        g_origAnyBitSet = runtime.lookupFunction(kHandlerAnyBitSet);
        g_origCreateChar = runtime.lookupFunction(kHandlerCreateChar);
        if (!g_origAnyBitSet || !g_origCreateChar)
        {
            std::fprintf(stderr, "[script-fix] handlers not found: the mission script fixes are off\n");
            return;
        }
        runtime.replaceFunction(kHandlerAnyBitSet, &anyBitSetHook);
        runtime.replaceFunction(kHandlerCreateChar, &createCharHook);
    }
}
