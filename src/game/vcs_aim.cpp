// See vcs_aim.h and game-internals.md 12c.
#include "game/vcs_aim.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_control.h"
#include "game/vcs_display.h"
#include "settings/settings.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace vcs::aim
{
    namespace
    {
        constexpr uint32_t kRamMask = PS2_RAM_MASK;
        constexpr uint32_t kPlayerPed = 0x004E4910u; // CWorld::Players[0].m_pPed
        // the ped (game-internals.md 12, 12c)
        constexpr uint32_t kPedPos = 0x30u, kPedVehicle = 0x480u, kPedState = 0x8C4u, kPedSlot = 0x789u, kPedWeapons = 0x578u;
        constexpr uint32_t kPedTarget = 0x820u;     // what the player is locked on to (the dummy object in free aim)
        constexpr uint32_t kPedAimPoint = 0xC90u;   // the free-aim point (the game keeps the dummy there)
        constexpr uint32_t kPedFreeAimWord = 0xCCCu; // the high word of the flags at +0xCC8: bit 51 = free aim
        constexpr uint32_t kFreeAimBit = 0x00080000u;
        constexpr uint32_t kForcedBit = 0x00200000u; // bit 53: a script's forced free aim (SET_PLAYER_FORCE_FREE_AIMING)
        constexpr uint32_t kEntityFlags = 0x48u;    // & 0xE: 6 ped, 8 object
        constexpr uint32_t kPedAttachedTo = 0x858u; // the vehicle a script attached him to (ATTACH_CHAR_TO_CAR)
        // TheCamera and its CCams (game-internals.md 12b)
        constexpr uint32_t kTheCamera = 0x006F44D0u;
        constexpr uint32_t kActiveCam = kTheCamera + 0x50u;
        constexpr uint32_t kCams = kTheCamera + 0x70u, kCamSize = 0x270u;
        constexpr uint32_t kCamSource = 0x20u, kCamAlpha = 0x78u, kCamBeta = 0x7Cu;
        constexpr uint32_t kCamModeLockOn = 11;
        constexpr uint32_t kCamModeMountedGun = 45; // first person behind a gun on a vehicle (VC's HELICANNON_1STPERSON)
        constexpr uint32_t kTimeStep = 0x004CD110u; // CTimer::ms_fTimeStep (1.0 = 1/50 s)
        // the camera's blend from one CCam to the next (game-internals.md 12c): a byte set while it runs, and its
        // durations in ms (400 into the lock-on / free-aim camera, 350 out of it)
        constexpr uint32_t kTransition = kTheCamera + 0x8D0u;
        constexpr uint32_t kTransitionDurations[2] = {kTheCamera + 0x8DCu, kTheCamera + 0x8E0u};
        constexpr uint32_t kBlendInMs = 100, kBlendOutMs = 200;

        // the hooked functions
        constexpr uint32_t kGameProcess = 0x003BA8D8u;   // CGame::Process: scripts, the world (peds move, guns fire)
        constexpr uint32_t kCameraProcess = 0x00378DD8u; // CCamera::Process: after the world, before the HUD
        constexpr uint32_t kShotLine = 0x002D0870u;      // the weapons' line test (CWorld::ProcessLineOfSight, 8 args)
        constexpr uint32_t kMovesAiming = 0x001F1698u;   // the ped's weapon lets him move while aiming (info byte 0 bit 7)
        constexpr uint32_t kMovesAimingCamReturn = 0x00375B44u; // its call in CCamera::CamControl (FUN_00372450)
        constexpr uint32_t kShotLineReturn = 0x002C47A4u; // its call in FireInstantHit (0x2c3e48) for a locked-on shooter
        // Guest memory for the shot's start point: the EE kernel's area below the game (0x00000-0x7FFFF), unused by the
        // recompiled runtime and the game (all zero in play, 2026-10-31).
        constexpr uint32_t kScratch = 0x00060000u;

        // The free-aim circle is the HUD dictionary's texture "sitem16" (VC's M16 sight name; game-internals.md 12c):
        // 64 x 64, 4-bit linear indices (the low nibble the left pixel), its 16-colour CLUT (RGBA, alpha 0x80 opaque)
        // right after the pixels. 0x6EDD94 holds its texture record: +0 the raster (+8 the pixels), +0x10 the name.
        // Found 2026-11-12 from a free-aim RAM dump: the dumped texture's 16 colours as a CLUT, the pixels before it.
        constexpr uint32_t kReticleTexture = 0x006EDD94u;
        constexpr uint32_t kReticlePixels = 64 * 64 / 2, kReticleBytes = kReticlePixels + 16 * 4;
        enum Reticle { ReticleCircle = 0, ReticleDot = 1 };
        std::atomic<int> g_reticle{ReticleCircle};
        uint32_t g_reticleAt = 0;            // the pixels the original below was read from (0: none)
        uint8_t g_reticleOriginal[kReticleBytes];
        uint8_t g_reticleDot[kReticleBytes];     // a small white dot with a soft dark rim, for the aspect below
        uint8_t g_reticleWritten[kReticleBytes]; // what was last written there
        float g_reticleWidening = 0.0f;          // the aspect widening the dot was made for (0: not made)

        constexpr float kDummyDistance = 12.0f; // metres from Vic: the lock drops past ~30 (the weapons' lock-on range)
        constexpr float kPivotHeight = 0.6f;    // the dummy turns about Vic's shoulders
        constexpr float kPitchLimit = 1.2f;
        constexpr float kStartPitchLimit = 0.35f; // aiming starts about level, not where a steep camera looked
        constexpr uint32_t kCamModeFollowPed = 4;
        constexpr float kStickRate = 2.0f;      // radians a second at full tilt (gamepad)
        // in a car (the game's drive-by free aim, FUN_002343E0): the dummy at the gun's range from Vic, between 5 deg
        // below and 20 deg above level (the game's limits for the stick there: 70..95 deg from Vic's up)
        constexpr float kCarPitchDown = -0.0872665f, kCarPitchUp = 0.3490659f;
        // on foot (the forced free aim): 50..110 deg from Vic's up (20 deg below level, 40 above), and 110 deg either side
        // of his heading
        constexpr float kFootPitchDown = -0.3490659f, kFootPitchUp = 0.6981317f, kFootYawLimit = 1.9198622f;
        constexpr uint32_t kWeaponInfo = 0x00712770u; // *(*(this) + 4) + type * 0x70: the weapon's info, +8 its range

        PS2Runtime::RecompiledFunction g_origGame = nullptr, g_origCamera = nullptr, g_origShotLine = nullptr,
                                       g_origMovesAiming = nullptr;
        uint8_t *g_rdram = nullptr;

        enum class State
        {
            Idle,     // not aiming
            Entering, // R1 + L3 sent: waiting for the game's free-aim lock
            Active,   // the dummy is ours
            Released, // R1 up for a few reports (a lock that went wrong: a ped instead of the dummy, or none), then
                      // Idle again - the game never sees R1 held long enough to lock on to a ped
            Held,     // the gun went (ran dry: the game puts the fists in his hand; or changed) while the button is
                      // held: R1 up until it is let go or a gun is back in hand (then free aim again)
            Script,   // the game's own free aim a script started (a car's drive-by mode, the forced free aim on foot): the
                      // dummy is ours, no buttons
            Mounted,  // a gun on a vehicle a script attached him to (Farewell To Arms' helicopter door): its camera is ours
            Assist,   // aim assist: plain R1, the game's lock-on, until the right stick moves (then free aim from where
                      // the lock-on camera looks) or nothing was locked on to after a few reports
        };
        // game thread
        State g_state = State::Idle;
        int g_wait = 0;               // Entering: frames waited; Released: frames left with R1 up
        constexpr int kRetryFrames = 8;
        constexpr int kAssistFrames = 6;    // Assist: reports to wait for the game's lock-on before free aim
        constexpr float kAssistStick = 0.1f; // Assist: the right stick (rate, squared curve: ~0.3 of the throw) leaves it
        bool g_assistDone = false;           // this hold of the button went on from the lock-on to free aim
        uint32_t g_dummy = 0;
        float g_yaw = 0, g_pitch = 0; // the aim from Vic's shoulders: yaw from +x toward +y, pitch up
        int g_quickBlend = 0;         // frames left to shorten the camera's blend into / out of free aim
        uint32_t g_blendMs = kBlendInMs; // to this
        int g_handBack = 0;           // frames left to give the follow camera the aim's heading when it is back
        float g_handBeta = 0;
        std::atomic<bool> g_active{false};
        std::atomic<uint32_t> g_shots{0}; // shots sent down the camera ray ("aimstate")

        uint32_t rd32(uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, g_rdram + (a & kRamMask), 4);
            return v;
        }
        void wr32(uint32_t a, uint32_t v) { std::memcpy(g_rdram + (a & kRamMask), &v, 4); }
        float rdf(uint32_t a)
        {
            float v;
            std::memcpy(&v, g_rdram + (a & kRamMask), 4);
            return v;
        }
        void wrf(uint32_t a, float v) { std::memcpy(g_rdram + (a & kRamMask), &v, 4); }
        void wrVec(uint32_t a, const float v[3])
        {
            for (int i = 0; i < 3; ++i)
                wrf(a + i * 4, v[i]);
        }

        uint32_t activeCam() { return kCams + g_rdram[kActiveCam] * kCamSize; }
        bool isObject(uint32_t e) { return e && (rd32(e + kEntityFlags) & 0xEu) == 8u; }
        bool freeAimBit(uint32_t ped) { return rd32(ped + kPedFreeAimWord) & kFreeAimBit; }
        void setFreeAimBit(uint32_t ped, bool on)
        {
            const uint32_t w = rd32(ped + kPedFreeAimWord);
            wr32(ped + kPedFreeAimWord, on ? w | kFreeAimBit : w & ~kFreeAimBit);
        }

        // Weapon types (docs/TRAINER.md): 19..29 pistols, shotguns, SMGs, rifles; 33..35 flame-thrower, M249, minigun.
        // Not 30..32 (the snipers and the rocket launcher aim through their scopes), 38 / 39 (camera, binoculars),
        // melee (1..11) or thrown (12..17).
        bool isGun(uint32_t type) { return (type >= 19 && type <= 29) || (type >= 33 && type <= 35); }

        bool onFoot(uint32_t ped) { return ped && !rd32(ped + kPedVehicle) && rd32(ped + kPedState) != 0x37; }

        // The game's free aim, started by a script: bit 51 and the dummy object as the target, in a car (a mission's
        // drive-by mode) or forced (bit 53: the shooting range). Not the player's own (R1 + L3 on foot: neither).
        bool scriptFreeAim(uint32_t ped)
        {
            return ped && freeAimBit(ped) && isObject(rd32(ped + kPedTarget)) &&
                   (rd32(ped + kPedVehicle) || (rd32(ped + kPedFreeAimWord) & kForcedBit));
        }

        // Attached to a vehicle by a script (ATTACH_CHAR_TO_CAR with a weapon) and behind its gun: Vic does not aim
        // through the free aim there, the camera is the aim (game-internals.md 12c).
        bool mountedGun(uint32_t ped)
        {
            return ped && rd32(ped + kPedAttachedTo) && (rd32(activeCam()) & 0xFFFFu) == kCamModeMountedGun;
        }

        float weaponRange(uint32_t ped)
        {
            const int8_t slot = static_cast<int8_t>(g_rdram[(ped + kPedSlot) & kRamMask]);
            const uint32_t type = slot >= 0 && slot < 10 ? rd32(ped + kPedWeapons + slot * 0x1Cu) : 0;
            const uint32_t table = rd32(rd32(kWeaponInfo) + 4);
            const float range = type && type < 60 && table ? rdf(table + type * 0x70u + 8) : 0.0f;
            return range >= 5.0f && range <= 200.0f ? range : 30.0f;
        }

        void dummyPlace(uint32_t ped)
        {
            const float cp = std::cos(g_pitch);
            const float at[3] = {rdf(ped + kPedPos) + kDummyDistance * cp * std::cos(g_yaw),
                                 rdf(ped + kPedPos + 4) + kDummyDistance * cp * std::sin(g_yaw),
                                 rdf(ped + kPedPos + 8) + kPivotHeight + kDummyDistance * std::sin(g_pitch)};
            wrVec(g_dummy + kPedPos, at);
            wrVec(ped + kPedAimPoint, at);
        }

        void stop(const char *why)
        {
            if (g_state == State::Active)
            {
                std::fprintf(stderr, "[aim] free aim off (%s)\n", why);
                // the follow camera comes back looking where the aim did (game-internals.md 12c)
                g_handBeta = rdf(activeCam() + kCamBeta);
                g_handBack = 30;
                g_quickBlend = 10;
                g_blendMs = kBlendOutMs;
            }
            g_state = State::Idle;
            g_active = false;
        }

        // Turns the follow camera's view about Vic by `delta` radians (about z): its heading, its source points
        // (+0x220 / +0x230), and this frame's Source, Front, Up and target point.
        void turnFollowCamera(uint32_t cam, uint32_t ped, float delta)
        {
            const float px = rdf(ped + kPedPos), py = rdf(ped + kPedPos + 4), c = std::cos(delta), s = std::sin(delta);
            const float beta = std::fmod(rdf(cam + kCamBeta) + delta, 6.2831855f);
            wrf(cam + kCamBeta, beta < 0.0f ? beta + 6.2831855f : beta);
            for (uint32_t at : {cam + 0x220u, cam + 0x230u, cam + kCamSource, cam + 0x1A0u})
            {
                const float ox = rdf(at) - px, oy = rdf(at + 4) - py;
                wrf(at, px + ox * c - oy * s);
                wrf(at + 4, py + ox * s + oy * c);
            }
            for (uint32_t at : {cam + 0x10u, cam + 0x60u})
            {
                const float x = rdf(at), y = rdf(at + 4);
                wrf(at, x * c - y * s);
                wrf(at + 4, x * s + y * c);
            }
        }

        // CGame::Process: the world runs with the free-aim bit cleared - to the game a lock-on to an object, so Vic
        // walks / strafes / stands as he does locked on with this weapon, and the left stick moves him (in the game's
        // free aim it moves the dummy, and Vic stands still).
        // The dot: white inside 2.25 texels of the centre, a dark rim out to 3.25 (seen on a bright sky), 4 x 4 samples a
        // texel; each texel the nearest of 16 colours (clear, eight whites, seven dark greys by alpha), premultiplied.
        // The game draws the sprite for 4:3, so a wider picture stretches it sideways (`widening`, vcs_display.h: 4/3 at
        // 16:9 - the player saw an oval): the dot is made that much narrower in the texture, round on the screen.
        void makeDot(float widening)
        {
            g_reticleWidening = widening;
            uint8_t clut[16][4] = {};
            for (int k = 1; k <= 8; ++k)
                clut[k][0] = clut[k][1] = clut[k][2] = 0xFF, clut[k][3] = static_cast<uint8_t>(k * 16);
            for (int k = 9; k < 16; ++k)
                clut[k][0] = clut[k][1] = clut[k][2] = 0x20, clut[k][3] = static_cast<uint8_t>((k - 8) * 16);
            std::memset(g_reticleDot, 0, kReticlePixels);
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 64; ++x)
                {
                    float white = 0.0f, dark = 0.0f;
                    for (int s = 0; s < 16; ++s)
                    {
                        const float dx = (x + (s % 4 + 0.5f) / 4.0f - 32.0f) * widening, dy = y + (s / 4 + 0.5f) / 4.0f - 32.0f;
                        const float r = std::sqrt(dx * dx + dy * dy);
                        if (r < 2.25f)
                            white += 1.0f / 16.0f;
                        else if (r < 3.25f)
                            dark += 1.0f / 16.0f;
                    }
                    const float alpha = white + dark * 0.75f, grey = white * 255.0f + dark * 0.75f * 32.0f;
                    int best = 0;
                    float bestErr = 1e30f;
                    for (int k = 0; k < 16; ++k)
                    {
                        const float a = clut[k][3] / 128.0f, ea = (a - alpha) * 255.0f, eg = clut[k][0] * a - grey;
                        if (ea * ea + eg * eg < bestErr)
                            bestErr = ea * ea + eg * eg, best = k;
                    }
                    g_reticleDot[(y * 64 + x) / 2] |= static_cast<uint8_t>(x & 1 ? best << 4 : best);
                }
            std::memcpy(g_reticleDot + kReticlePixels, clut, sizeof(clut));
        }

        // Game thread, each frame: the circle's texels and CLUT as the setting wants them. The original is kept from the
        // first look (the texture's address checked by its record's name) and put back when the setting goes back.
        void updateReticle()
        {
            const uint32_t rec = rd32(kReticleTexture);
            if (rec < 0x100000u || rec >= 0x2000000u || std::memcmp(g_rdram + rec + 0x10u, "sitem16", 8) != 0)
                return;
            const uint32_t pixels = rd32(rd32(rec) + 8u);
            if (pixels < 0x100000u || pixels + kReticleBytes > 0x2000000u)
                return;
            uint8_t *at = g_rdram + pixels;
            // ours: what is there is what was written (else the game's: the original, or reloaded)
            const bool ours = g_reticleAt == pixels && std::memcmp(at, g_reticleWritten, kReticleBytes) == 0;
            if (g_reticle.load(std::memory_order_relaxed) == ReticleDot)
            {
                const float widening = std::clamp(vcs::aspectWidening(), 0.5f, 4.0f);
                if (widening != g_reticleWidening)
                    makeDot(widening);
                if (ours && std::memcmp(g_reticleWritten, g_reticleDot, kReticleBytes) == 0)
                    return;
                if (!ours)
                {
                    std::memcpy(g_reticleOriginal, at, kReticleBytes);
                    g_reticleAt = pixels;
                }
                std::memcpy(at, g_reticleDot, kReticleBytes);
                std::memcpy(g_reticleWritten, g_reticleDot, kReticleBytes);
            }
            else if (ours)
            {
                std::memcpy(at, g_reticleOriginal, kReticleBytes);
                g_reticleAt = 0;
            }
        }

        PS2_GUEST_CC void gameProcess(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            updateReticle();
            if (g_state == State::Active)
                if (const uint32_t ped = rd32(kPlayerPed))
                    setFreeAimBit(ped, false);
            g_origGame(rdram, ctx, runtime);
        }

        // CCamera::Process: the camera and then the HUD see free aim again (its camera centres the dummy, its circle is
        // drawn on it).
        // Also: the blend between the follow camera and the free-aim camera, started by the update when the mode changes
        // (400 ms in, 350 ms out, eased), is cut to 100 ms in and 200 ms out the frames after free aim starts or ends -
        // a PC shooter's aim comes up at once; going out takes about as long as Vic's raising the gun takes going in
        // (the user's ask). Other blends (cutscenes, vehicles, the PS2 lock-on) are the game's.
        PS2_GUEST_CC void cameraProcess(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_state == State::Active)
                if (const uint32_t ped = rd32(kPlayerPed); ped && rd32(ped + kPedTarget) == g_dummy)
                    setFreeAimBit(ped, true);
            if (g_quickBlend > 0)
            {
                --g_quickBlend;
                if (rdram[kTransition])
                    for (uint32_t at : kTransitionDurations)
                        wr32(at, std::min(rd32(at), g_blendMs));
            }
            g_origCamera(rdram, ctx, runtime);
        }

        // Does the ped's weapon let him move while aiming (FUN_001F1698, a0 the ped)? CCamera::CamControl asks when the
        // camera goes from the lock-on camera back to the follow camera: with a gun that stands Vic still (shotguns,
        // rifles, the SMG, heavy weapons) it cuts to the follow camera at once, with one that walks it blends (game-
        // internals.md 12c). Leaving free aim, that question is answered yes for the camera, so every gun's aim ends
        // in the same blend (the user's report: the stand-still guns' end was instant). Nothing else asks it here.
        PS2_GUEST_CC void movesAiming(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_handBack > 0 && GPR_U32(ctx, 31) == kMovesAimingCamReturn)
            {
                SET_GPR_U64(ctx, 2, 1);
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
            g_origMovesAiming(rdram, ctx, runtime);
        }

        // The weapons' line test (from a0 to a1; t1 the shooter). For the player's free-aim shot (FireInstantHit's call
        // for a locked-on shooter) the line is the camera's through the circle: from the point on it level with the gun
        // to as far as the gun reaches. The gun's own start (a0) is left for the tracer and the impact's effects; the
        // end (a1, FireInstantHit's) gets the camera line's end, so the tracer goes where the shot went.
        PS2_GUEST_CC void shotLine(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ped = rd32(kPlayerPed);
            if ((g_state == State::Active || g_state == State::Script) && GPR_U32(ctx, 31) == kShotLineReturn && ped &&
                GPR_U32(ctx, 9) == ped && rd32(ped + kPedTarget) == g_dummy)
            {
                const uint32_t from = GPR_U32(ctx, 4), to = GPR_U32(ctx, 5), cam = activeCam();
                const float src[3] = {rdf(cam + kCamSource), rdf(cam + kCamSource + 4), rdf(cam + kCamSource + 8)};
                const float gun[3] = {rdf(from), rdf(from + 4), rdf(from + 8)};
                const float end[3] = {rdf(to), rdf(to + 4), rdf(to + 8)};
                float dir[3] = {rdf(g_dummy + kPedPos) - src[0], rdf(g_dummy + kPedPos + 4) - src[1],
                                rdf(g_dummy + kPedPos + 8) - src[2]};
                const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
                const float range = std::sqrt((end[0] - gun[0]) * (end[0] - gun[0]) + (end[1] - gun[1]) * (end[1] - gun[1]) +
                                              (end[2] - gun[2]) * (end[2] - gun[2]));
                if (len > 0.01f && range > 0.01f)
                {
                    for (float &d : dir)
                        d /= len;
                    const float along = std::max(0.0f, (gun[0] - src[0]) * dir[0] + (gun[1] - src[1]) * dir[1] +
                                                           (gun[2] - src[2]) * dir[2]);
                    const float start[3] = {src[0] + dir[0] * along, src[1] + dir[1] * along, src[2] + dir[2] * along};
                    const float stop[3] = {start[0] + dir[0] * range, start[1] + dir[1] * range, start[2] + dir[2] * range};
                    wrVec(kScratch, start);
                    wr32(kScratch + 12, 0);
                    wrVec(to, stop);
                    SET_GPR_U32(ctx, 4, kScratch);
                    g_shots.fetch_add(1, std::memory_order_relaxed);
                }
            }
            g_origShotLine(rdram, ctx, runtime);
        }
    }

    void install(PS2Runtime &runtime)
    {
        g_rdram = runtime.memory().getRDRAM();
        if (const char *e = std::getenv("VCS_FREEAIM"); e && e[0] == '0')
        {
            std::fprintf(stderr, "[aim] free aim off (VCS_FREEAIM=0)\n");
            return;
        }
        g_origGame = runtime.lookupFunction(kGameProcess);
        g_origCamera = runtime.lookupFunction(kCameraProcess);
        g_origShotLine = runtime.lookupFunction(kShotLine);
        g_origMovesAiming = runtime.lookupFunction(kMovesAiming);
        if (!g_origGame || !g_origCamera || !g_origShotLine || !g_origMovesAiming)
            return;
        runtime.replaceFunction(kGameProcess, &gameProcess);
        runtime.replaceFunction(kCameraProcess, &cameraProcess);
        runtime.replaceFunction(kShotLine, &shotLine);
        runtime.replaceFunction(kMovesAiming, &movesAiming);
        // aimstate -> ok active=<0|1> shots=<shots down the camera ray so far> (tools/controls_check.py)
        ps2control::addCommand("aimstate", [](const std::string &, ps2control::Reply reply) {
            char text[96];
            std::snprintf(text, sizeof(text), "ok active=%d shots=%u", g_active.load() ? 1 : 0, g_shots.load());
            reply(text);
        });
    }

    void registerSettings()
    {
        settings::Spec s;
        s.key = "enhance.reticle";
        s.page = "Enhancements";
        s.section = "Combat";
        s.label = "Free aim reticle";
        s.help = "What free aim (Keyboard / Gamepad > Free aim) puts where the shots go. Circle: the game's white circle "
                 "with a dot. Dot: a small white dot alone, with a thin dark rim so it shows on a bright sky.";
        s.type = settings::Type::Choice;
        s.choices = {{"Circle", ReticleCircle}, {"Dot", ReticleDot}};
        s.defaultValue = ReticleCircle;
        s.env = "VCS_RETICLE";
        s.onChange = [](int v) { g_reticle.store(v, std::memory_order_relaxed); };
        settings::add(s);
    }

    bool gunInHand()
    {
        const uint32_t ped = g_rdram ? rd32(kPlayerPed) : 0;
        if (!ped)
            return false;
        const int8_t slot = static_cast<int8_t>(g_rdram[(ped + kPedSlot) & kRamMask]);
        return slot >= 0 && slot < 10 && isGun(rd32(ped + kPedWeapons + slot * 0x1Cu));
    }

    bool firearmInHand()
    {
        const uint32_t ped = g_rdram ? rd32(kPlayerPed) : 0;
        if (!ped)
            return false;
        const int8_t slot = static_cast<int8_t>(g_rdram[(ped + kPedSlot) & kRamMask]);
        const uint32_t type = slot >= 0 && slot < 10 ? rd32(ped + kPedWeapons + slot * 0x1Cu) : 0;
        return isGun(type) || (type >= 30 && type <= 32);
    }

    bool aiming() { return g_active.load(std::memory_order_relaxed); }

    // The follow camera's first update after free aim starts it afresh behind Vic (a heading of its own, whatever the
    // aim's), and the view showed that for a frame (the user's report: a one-frame snap to another angle). Here, after
    // that update and before the frame's camera is built from it, the whole view is turned back to the aim's heading;
    // the follow camera carries on from there.
    void cameraUpdated(uint32_t cam)
    {
        if (g_handBack <= 0 || !g_rdram || cam != activeCam() || (rd32(cam) & 0xFFFFu) != kCamModeFollowPed)
            return;
        g_handBack = 0;
        if (const uint32_t ped = rd32(kPlayerPed))
            turnFollowCamera(cam, ped, std::remainder(g_handBeta - rdf(cam + kCamBeta), 6.2831855f));
    }

    namespace
    {
        // The game's free aim started by a script (game-internals.md 12c): in a car (a mission's drive-by mode) the game
        // moves the dummy with the car, on foot (the forced free aim: the shooting range) Vic stands still; either way
        // the left stick turns it (FUN_002343E0, in the world update after this pad read), within limits, then it is
        // pulled onto whatever the camera's line to it hits. Here the mouse / right stick turn it instead (the left
        // stick centred), within the same limits (the game's apply only while its stick moves it), placed at the gun's
        // range from Vic as the game keeps it, and the lock-on camera is turned by the same amount first, as on foot.
        // No button: the game aims all the time in this mode, and the fire button shoots (through the circle: the shot
        // line as on foot). The gamepad's R1 fires too; the keyboard / mouse R1 is left to the game (it does nothing
        // here) - our own free aim's R1 + L3 would end the script's.
        Output scripted(uint32_t ped, const Input &in)
        {
            Output out;
            const uint32_t cam = activeCam();
            const bool car = rd32(ped + kPedVehicle) != 0;
            if (g_state != State::Script)
            {
                if (g_state == State::Active)
                    stop("a script's free aim");
                // aim where the camera looks, level
                g_yaw = std::atan2(rdf(cam + 0x14), rdf(cam + 0x10));
                g_pitch = 0.0f;
                g_dummy = rd32(ped + kPedTarget);
                g_state = State::Script;
                g_active = true;
                std::fprintf(stderr, "[aim] the game's free aim (%s, dummy 0x%x)\n",
                             car ? "a car's drive-by" : "forced by a script", g_dummy);
            }
            g_dummy = rd32(ped + kPedTarget);
            // the gamepad's R1 fires (the user's ask: Jive Drive, then the range - the right thumb is on the stick)
            out.camera = out.leftStick = out.padR1Fires = true;

            const float dt = std::clamp(rdf(kTimeStep), 0.05f, 3.4f) / 50.0f;
            float turn = in.turn + in.turnRate * kStickRate * dt;
            if (!car)
            {
                // within 110 deg of Vic's heading (his forward, the matrix's +0x10)
                const float heading = std::atan2(rdf(ped + 0x14), rdf(ped + 0x10));
                const float to = std::clamp(std::remainder(g_yaw + turn - heading, 6.2831855f), -kFootYawLimit, kFootYawLimit);
                turn = std::remainder(heading + to - g_yaw, 6.2831855f);
            }
            const float pitch = std::clamp(g_pitch + in.tilt + in.tiltRate * kStickRate * dt, car ? kCarPitchDown : kFootPitchDown,
                                           car ? kCarPitchUp : kFootPitchUp);
            const float tilt = pitch - g_pitch;
            g_yaw = std::remainder(g_yaw + turn, 6.2831855f);
            g_pitch = pitch;
            if (rd32(cam) == kCamModeLockOn && (turn != 0.0f || tilt != 0.0f))
            {
                const float beta = std::fmod(rdf(cam + kCamBeta) + turn, 6.2831855f);
                wrf(cam + kCamBeta, beta < 0.0f ? beta + 6.2831855f : beta);
                wrf(cam + kCamAlpha, rdf(cam + kCamAlpha) + tilt);
            }
            const float range = weaponRange(ped), cp = std::cos(g_pitch);
            const float at[3] = {rdf(ped + kPedPos) + range * cp * std::cos(g_yaw),
                                 rdf(ped + kPedPos + 4) + range * cp * std::sin(g_yaw),
                                 rdf(ped + kPedPos + 8) + (car ? 0.0f : kPivotHeight) + range * std::sin(g_pitch)};
            wrVec(g_dummy + kPedPos, at);
            wrVec(ped + kPedAimPoint, at);
            return out;
        }

        // A gun on a vehicle (game-internals.md 12c: Farewell To Arms puts Vic in its helicopter's door with an M249): the
        // camera (mode 45, FUN_00297cb8) is the aim, its circle in the middle, and the left stick turns it - its heading
        // (+0x7C) is the turn from the side Vic faces, signed, kept within the attachment's limit (70 deg there), its
        // pitch within the script's. Here the mouse / right stick turn it instead (the left stick centred), the gamepad's
        // R1 fires, as in a script's free aim; the camera's update keeps both in its limits.
        Output mounted(uint32_t ped, const Input &in)
        {
            Output out;
            if (g_state != State::Mounted)
            {
                if (g_state == State::Active)
                    stop("a gun on a vehicle");
                g_state = State::Mounted;
                g_active = true;
                std::fprintf(stderr, "[aim] a gun on a vehicle (attached to 0x%x)\n", rd32(ped + kPedAttachedTo));
            }
            out.camera = out.leftStick = out.padR1Fires = true;
            const uint32_t cam = activeCam();
            const float dt = std::clamp(rdf(kTimeStep), 0.05f, 3.4f) / 50.0f;
            const float turn = in.turn + in.turnRate * kStickRate * dt;
            const float tilt = in.tilt + in.tiltRate * kStickRate * dt;
            // signed: wrapped into 0..2 pi a turn right of the middle became the limit on the left (the user's report)
            wrf(cam + kCamBeta, std::remainder(rdf(cam + kCamBeta) + turn, 6.2831855f));
            wrf(cam + kCamAlpha, rdf(cam + kCamAlpha) + tilt);
            return out;
        }
    }

    Output update(const Input &in)
    {
        Output out;
        const uint32_t ped = g_rdram && g_origGame ? rd32(kPlayerPed) : 0;
        if (g_handBack > 0)
            --g_handBack; // the camera hasn't come back to the follow camera in time: leave it
        if (in.on && scriptFreeAim(ped))
            return scripted(ped, in);
        if (in.on && mountedGun(ped))
            return mounted(ped, in);
        if (g_state == State::Script || g_state == State::Mounted)
        {
            std::fprintf(stderr, g_state == State::Script ? "[aim] the game's free aim off\n" : "[aim] the gun on a vehicle off\n");
            g_state = State::Idle;
            g_active = false;
        }
        if (!ped || !in.want || !onFoot(ped))
        {
            stop(!in.want ? "released" : "not on foot");
            g_assistDone = false;
            return out;
        }
        if (!gunInHand())
        {
            // fists or melee from the start: the PS2's R1 (lock-on). The gun gone during a free aim (it ran dry and
            // the game put the fists in his hand, or the player changed weapon): R1 stays up until the button is let
            // go - no lock-on on whoever is near, and no free aim flicking on and off (the user's report)
            if (g_state == State::Idle)
                return out;
            stop("no gun in hand");
            g_state = State::Held;
            out.own = true;
            return out;
        }
        out.own = true;
        const uint32_t cam = activeCam();
        if (g_state == State::Idle && in.assist && !g_assistDone)
        {
            // aim assist: the game's own lock-on first (plain R1; its camera)
            g_state = State::Assist;
            g_wait = 0;
            out.r1 = true;
            return out;
        }
        switch (g_state)
        {
        case State::Released:
            if (--g_wait <= 0)
                g_state = State::Idle; // R1 up until now; the next report enters again
            return out;
        case State::Assist:
        {
            const uint32_t target = rd32(ped + kPedTarget);
            const bool stick = std::fabs(in.turnRate) > kAssistStick || std::fabs(in.tiltRate) > kAssistStick;
            if (in.assist && !stick && (target ? !isObject(target) : ++g_wait < kAssistFrames))
            {
                out.r1 = true; // locked on (or still looking for someone): the game's
                return out;
            }
            // on to free aim, from where the camera looks: from a lock, R1 up a moment first (the game keeps a ped's
            // lock through R1 + L3); with no one locked on to, R1 + L3 at once
            g_assistDone = true;
            if (target)
            {
                g_state = State::Released;
                g_wait = 2;
                return out;
            }
            [[fallthrough]];
        }
        case State::Held: // a gun in hand again, the button still held: free aim again
        case State::Idle:
        {
            // aim where the camera looks
            const float fx = rdf(cam + 0x10), fy = rdf(cam + 0x14), fz = rdf(cam + 0x18);
            g_yaw = std::atan2(fy, fx);
            g_pitch = std::clamp(std::asin(std::clamp(fz, -1.0f, 1.0f)), -kStartPitchLimit, kStartPitchLimit);
            g_state = State::Entering;
            g_wait = 0;
            g_quickBlend = 30; // until a few frames after the free-aim lock is there
            g_blendMs = kBlendInMs;
            g_handBack = 0;
            out.camera = true;
            out.r1 = out.l3 = true;
            return out;
        }
        case State::Entering:
        {
            const uint32_t target = rd32(ped + kPedTarget);
            if (!(isObject(target) && freeAimBit(ped)))
            {
                // not yet (Vic may still be busy: firing, reloading, changing weapon). The game took R1 without L3
                // and locked on to a ped, or ~20 frames went by: let go a few frames, then try again
                out.camera = true;
                out.r1 = !target && ++g_wait < 20;
                if (!out.r1)
                {
                    g_state = State::Released;
                    g_wait = kRetryFrames;
                }
                return out;
            }
            g_dummy = target;
            g_state = State::Active;
            g_active = true;
            std::fprintf(stderr, "[aim] free aim on (dummy 0x%x, %d frame(s) after R1 + L3)\n", target, g_wait + 1);
            break;
        }
        case State::Active:
            if (rd32(ped + kPedTarget) != g_dummy)
            {
                stop("lock lost");
                g_state = State::Released;
                g_wait = kRetryFrames;
                return out;
            }
            break;
        }
        out.camera = true;

        // turn the aim, and the camera by the same amount before its update (as the mouse turns the follow camera:
        // the camera then has nothing left to catch up, game-internals.md 12c)
        const float dt = std::clamp(rdf(kTimeStep), 0.05f, 3.4f) / 50.0f;
        const float turn = in.turn + in.turnRate * kStickRate * dt;
        const float pitch = std::clamp(g_pitch + in.tilt + in.tiltRate * kStickRate * dt, -kPitchLimit, kPitchLimit);
        const float tilt = pitch - g_pitch;
        g_yaw = std::remainder(g_yaw + turn, 6.2831855f);
        g_pitch = pitch;
        if (rd32(cam) == kCamModeLockOn && (turn != 0.0f || tilt != 0.0f))
        {
            const float beta = std::fmod(rdf(cam + kCamBeta) + turn, 6.2831855f);
            wrf(cam + kCamBeta, beta < 0.0f ? beta + 6.2831855f : beta);
            wrf(cam + kCamAlpha, rdf(cam + kCamAlpha) + tilt);
        }
        dummyPlace(ped);
        out.r1 = true;
        return out;
    }
}
