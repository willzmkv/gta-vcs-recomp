// Vehicle physics that acts a fixed amount every frame, put on the game's time step (docs/game-internals.md 4).
//
// The PS2 game ran at 30 fps, and some vehicle physics is applied once a frame without CTimer::ms_fTimeStep: at 144 fps
// it acted 4.8 times as often per second. The cases here are scaled by the frame's step over the PS2's (50/30) when
// the frame is shorter, so they act as much per second as at 30 fps; longer frames keep the game's amount, as the PS2
// did when it dropped below 30. Only while the frame isn't the PS2's (independent timing, or original timing at 60).
//
// 1. The push from a body under a wheel. FUN_003F34F0 runs after each wheel's ProcessWheel, for cars (FUN_00152E18,
//    from CAutomobile::ProcessControl) and bikes (CBike::ProcessControl, FUN_00337208). When the player drives, the
//    vehicle moves and a ped near the driver is dead or knocked down within 1 m (horizontally) of the wheel's contact
//    point, it gives the vehicle an upward push at that wheel: ApplyTurnForce((0, 0, mass * 0.01 (* 1.5 for cars)),
//    contact point). A car's front wheels over a body pitched it up so fast at 144 fps that its rear hit the ground and
//    threw it up (3.3 m/s against 1.4 at 30 fps); a bike rolling past the rider it was stolen from stood on its nose.
//    The hook on ApplyTurnForce scales that one call's force (f14, return address 0x3F36D4).
// 2. Rolling resistance. CVehicle::ProcessWheel (FUN_003E6C18 cars, FUN_003E7618 bikes) brakes a wheel with neither
//    gas nor brake by 0.9 / mass (other constants for light vehicles, the RC Bandit and bikes) per frame: a car coasted
//    down at 16 m/s^2 at 144 fps instead of 3.4. Patched in game/vcs/overrides.toml to read those constants from guest
//    RAM 0x7FF00.. (kBrakeSlots); the ProcessWheel hooks write them, scaled, before every call - always, also with this
//    off, since the patched code has no constant of its own any more.
// 3. A bike's brake wobble (GitHub issue 64). CBike::ProcessControl (at 0x33BD18..0x33BE54), while the brake leads the
//    gas by more than 0.9, the bike rolls forwards faster than 0.02 and a wheel is on the ground: a phase (+0x5FC)
//    grows by random(0.5..1) * 0.2 * step and the bike is pushed sideways at a wheel's contact point, ApplyTurnForce(
//    min(speed, 0.2) * sin(phase) (FUN_0044A2D0) * turn mass * (0.03 with the rear wheel down, else 0.007) * right) - once a frame,
//    the step only in the phase. At 144 fps the push came 4.8 times as often per second: braking hard from speed
//    turned the bike's yaw 3.7 times as fast, it fishtailed (heading swinging -21 to +68 degrees, sideways at 19 m/s;
//    3 degrees at 30 fps), and at 240 it went down on its side. The hook scales that call's force (f12-f14, return
//    address 0x33BE5C).
// 4. A rotor blade against the world (GitHub issue 102). CVehicle's blade test (FUN_003EE308: radius f12, damage
//    multiplier f13, the vehicle a0; called for each rotor while it turns) sets the vehicle's damage impulse (+0x10C)
//    to max(the contact's impulse, a constant x mass / 3000) x the multiplier whenever a blade touches something, and
//    CAutomobile::ProcessControl's VehicleDamage(0) turns that into health (+0x27C) the same frame: a Maverick standing
//    with its rotor in a wall lost 12.75 a frame - 382 a second at 30 fps, 1590 at 144, on fire within a second. The
//    damage has a threshold, so the impulse cannot simply be scaled; instead the blades count on one frame in every
//    1/30 s, as on the PS2: a per-vehicle sum of the frames' steps lets the multiplier through when it reaches a PS2
//    frame and zeroes it otherwise (the sparks and the push stay every frame).
// 5. The fire truck's water cannon (GitHub issue 144, Hose the Hoes). CWaterCannon (3 at 0x4E3A60, 0x250 bytes: +0 the
//    vehicle, +4 s16 the newest of 16 points, +8 the time it was set up, +0x10 positions, +0x110 velocities, +0x210
//    in use) is a trail of 16 points: the truck writes the nozzle's position and velocity into the newest every frame
//    (FUN_00186CB8), and Update_OncePerFrame (FUN_00186AA8) moves them by the step, puts out fire within 4 m of the
//    first point that reaches one (FUN_002BEAB8: -0.012 x step of the fire's strength) and - once 150 ms have passed
//    since the set-up - starts a new point every frame, dropping the oldest. So the water lived 16 frames: 0.53 s and
//    about 26 m of reach at 30 fps, 0.11 s and 5.6 m at 144. It fell short of the brothel's upper floor and the fires
//    never went out. The hook lets the trail move on once per 1/30 s of game time (the steps summed per cannon, as the
//    blades in 4.) by writing the set-up time (+8) as now on the frames in between, the real one otherwise; the rest
//    (the points' motion, the fire, PushPeds every 4th frame) stays per frame, on the step.
// 6. Pushing vehicles in CPhysical::ApplyCollision (GitHub issue 205; 0x34CD5C..0x34D60C). When a body without flag
//    0x2000 (vehicles, world geometry; peds carry 0x2000 so walking against a parked car takes this path) hits a moving
//    or movable object and the contact falls through the normal bounce exit, ApplyCollision applies 0.3 / 0.8 * impulse
//    to both objects through ApplyMoveForce (0x34D4A8 obj2, 0x34D5E0 obj1) and ApplyTurnForce (0x34D4C8 obj2, 0x34D604
//    obj1) once a frame without ms_fTimeStep. Above 30 fps that acted every shorter frame, letting Vic shove cars down
//    the street at ~3.4 m/s at 120 fps instead of barely budging them at 30 fps. Both move and turn hooks scale those
//    calls by frameScale.
// 7. Skimmer / boat planing lift in CBoat::ProcessControl (GitHub issue 205; 0x1A1260..0x1A1358). FUN_0036A090 already
//    multiplies the buoyancy impulse written to 0x28($sp) by CTimer::ms_fTimeStep, and the planing lift block reads
//    0x28($sp) and multiplies by ms_fTimeStep a second time at 0x1A12AC before ApplyMoveForce (0x1A1360). With dt^2 per
//    frame, integrated lift drops linearly with frame rate (4x weaker per second at 120 fps), keeping the Skimmer's
//    hull deep in the water where quadratic drag pins it around 5 m/s instead of taking off. Dividing ApplyMoveForce at
//    0x1A1360 by frameScale cancels the duplicate dt factor without touching <= 30 fps.
// 8. Angular air damping in CPhysical::ApplyAirResistance (0x348628): on the airRes <= 0.1f branch (0x348740),
//    m_vecTurnSpeed (+0x70..+0x78) is multiplied by literal 0.99f once per frame while linear velocity uses powf.
// 9. Burnout steering torque (FUN_00152E18 at 0x152FF8 when veh+0x714 != 0) and CBike upright / roll-damping torque
//    (CBike::ProcessControl 0x337208 at 0x33CDA0 / 0x33CE34): ApplyTurnForce impulses applied without ms_fTimeStep.
// 10. Airborne free-wheel spin decay / accel (CAutomobile FUN_00152E18 +0x6FC..+0x708: *0.95 / +-0.1 / +0.05;
//     CBike 0x337208 +0x598 / +0x59C: *0.95 / +0.1 / -0.2 and unscaled wheel-angle integration into +0x590 / +0x594).
//
// VCS_PHYSICS_TIMESTEP=0 keeps all ten at the game's unscaled amounts.
#include "game/vcs_physics_timing.h"

#include "game/vcs_framerate.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vcs
{
    namespace
    {
        constexpr uint32_t kApplyMoveForce = 0x00347D20u;        // CPhysical::ApplyMoveForce(f12-f14 force, a0)
        constexpr uint32_t kApplyTurnForce = 0x00347E78u;        // CPhysical::ApplyTurnForce(f12-f14 force, f15-f17 point, a0)
        constexpr uint32_t kBodyPushReturn = 0x003F36D4u;        // FUN_003F34F0's call: the push from a body under a wheel
        constexpr uint32_t kBrakeWobbleReturn = 0x0033BE5Cu;     // CBike::ProcessControl's call: the brake wobble (3.)
        constexpr uint32_t kCollisionPushMove2 = 0x0034D4A8u;    // CPhysical::ApplyCollision's obj2 push (6.)
        constexpr uint32_t kCollisionPushTurn2 = 0x0034D4C8u;
        constexpr uint32_t kCollisionPushMove1 = 0x0034D5E0u;    // CPhysical::ApplyCollision's obj1 push (6.)
        constexpr uint32_t kCollisionPushTurn1 = 0x0034D604u;
        constexpr uint32_t kBoatPlaningLiftReturn = 0x001A1360u; // CBoat::ProcessControl's planing lift (7.)
        constexpr uint32_t kBurnoutPivotReturn = 0x00152FF8u;    // FUN_00152E18 burnout steer pivot (veh+0x714 != 0) (9.)
        constexpr uint32_t kBikeUprightReturn1 = 0x0033CDA0u;    // CBike::ProcessControl uprighting torque (-0.1) (9.)
        constexpr uint32_t kBikeUprightReturn2 = 0x0033CE34u;    // CBike::ProcessControl dismounted roll torque (-0.15) (9.)
        constexpr uint32_t kApplyAirResistance = 0x00348628u;    // CPhysical::ApplyAirResistance(a0) (8.)
        constexpr uint32_t kCarAxleWheels = 0x00152E18u;         // CAutomobile axle wheel loop (10.)
        constexpr uint32_t kBikeProcessControl = 0x00337208u;    // CBike::ProcessControl(a0) (10.)
        constexpr uint32_t kBladeCollision = 0x003EE308u;        // CVehicle's rotor blade test (4.): f13 the damage multiplier
        constexpr uint32_t kProcessWheelCar = 0x003E6C18u;       // CVehicle::ProcessWheel
        constexpr uint32_t kProcessWheelBike = 0x003E7618u;
        constexpr uint32_t kBrakeSlots = 0x0007FF00u;            // the patched ProcessWheels' rolling-resistance constants
        constexpr float kBrakeConstants[] = {0.54f, 0.09f, 0.9f, 0.18f}; // as the game loaded them (0x3F0A3D71 ...)
        constexpr uint32_t kTimeStep = 0x004CD110u;              // CTimer::ms_fTimeStep (1.0 = 1/50 s)
        constexpr uint32_t kFrameCounter = 0x004CD124u;          // CTimer::m_FrameCounter
        constexpr uint32_t kTimeMs = 0x004CD104u;                // CTimer::m_snTimeInMilliseconds
        constexpr uint32_t kCannonUpdate = 0x00186AA8u;          // CWaterCannon::Update_OncePerFrame(cannon a0, index a1) (5.)
        constexpr uint32_t kCannons = 0x004E3A60u, kCannonSize = 0x250u, kCannonCount = 3u;
        constexpr float kPs2Step = 50.0f / 30.0f;                // a 30 fps frame

        PS2Runtime::RecompiledFunction g_origMoveForce = nullptr, g_origTurnForce = nullptr,
                                       g_origWheelCar = nullptr, g_origWheelBike = nullptr,
                                       g_origBlade = nullptr, g_origCannon = nullptr,
                                       g_origAirRes = nullptr, g_origCarAxleWheels = nullptr,
                                       g_origBikeProcessControl = nullptr;
        uint32_t g_trackBike = 0;
        uint8_t g_bikeWheelHitMask = 0;

        bool enabled()
        {
            static const bool on = [] {
                const char *v = std::getenv("VCS_PHYSICS_TIMESTEP");
                return !(v && v[0] == '0');
            }();
            return on;
        }
        // the frame isn't the PS2's 1/30 s: independent timing, or original timing at 60 (one VBlank a frame)
        bool active() { return enabled() && (timing() == Timing::Independent || frameCap() != 30); }

        // the frame's step over the PS2's when it is shorter, else 1 (the game's own amount)
        float frameScale(const uint8_t *rdram)
        {
            if (!active())
                return 1.0f;
            float step;
            std::memcpy(&step, rdram + kTimeStep, 4);
            return step >= 0.0f && step < kPs2Step ? step / kPs2Step : 1.0f;
        }

        float rdF(const uint8_t *rdram, uint32_t addr)
        {
            float v;
            std::memcpy(&v, rdram + (addr & 0x1FFFFFFu), 4);
            return v;
        }
        uint32_t rdU(const uint8_t *rdram, uint32_t addr)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (addr & 0x1FFFFFFu), 4);
            return v;
        }
        float dot3(const uint8_t *rdram, uint32_t a, uint32_t b)
        {
            return rdF(rdram, a) * rdF(rdram, b) + rdF(rdram, a + 4) * rdF(rdram, b + 4) + rdF(rdram, a + 8) * rdF(rdram, b + 8);
        }

        // VCS_TURN_LOG=<model>: every ApplyTurnForce on vehicles of that model, with its caller and what it does to the
        // turn speed about the vertical (yaw) and the vehicle's right axis (pitch, nose up > 0) - summed by caller over a
        // stretch of frames, it shows which push grows with the frame rate (how the brake wobble was found)
        void logTurnForce(const uint8_t *rdram, R5900Context *ctx)
        {
            static const int logModel = [] {
                const char *v = std::getenv("VCS_TURN_LOG");
                return v ? std::atoi(v) : 0;
            }();
            if (!logModel)
                return;
            const uint32_t veh = GPR_U32(ctx, 4);
            int16_t model;
            std::memcpy(&model, rdram + ((veh + 0x56u) & 0x1FFFFFFu), 2);
            if (model != logModel)
                return;
            auto f = [&](uint32_t off) { return rdF(rdram, veh + off); };
            float com[3]; // the centre of mass (+0xC0) in world orientation
            for (uint32_t i = 0; i < 3; ++i)
                com[i] = f(0x00 + 4 * i) * f(0xC0) + f(0x10 + 4 * i) * f(0xC4) + f(0x20 + 4 * i) * f(0xC8);
            const float px = ctx->f[15] - com[0], py = ctx->f[16] - com[1], pz = ctx->f[17] - com[2];
            const float tx = py * ctx->f[14] - pz * ctx->f[13], ty = pz * ctx->f[12] - px * ctx->f[14];
            const float tz = px * ctx->f[13] - py * ctx->f[12];
            const float turnMass = f(0xD4);
            std::fprintf(stderr, "[turn] f %u veh %x step %.3f ra %x yaw %.7f pitch %.7f\n", rdU(rdram, kFrameCounter), veh,
                         rdF(rdram, kTimeStep), GPR_U32(ctx, 31), tz / turnMass,
                         (tx * f(0x00) + ty * f(0x04) + tz * f(0x08)) / turnMass);
        }

        void scaleForce3(R5900Context *ctx, float s)
        {
            ctx->f[12] *= s;
            ctx->f[13] *= s;
            ctx->f[14] *= s;
        }

        PS2_GUEST_CC void applyMoveForce(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = GPR_U32(ctx, 31);
            if (ra == kCollisionPushMove2 || ra == kCollisionPushMove1)
                scaleForce3(ctx, frameScale(rdram));
            else if (ra == kBoatPlaningLiftReturn)
            {
                // CBoat::ProcessControl multiplies 0x28($sp) (already ms_fTimeStep-scaled by FUN_0036A090) by
                // ms_fTimeStep again at 0x1A12AC; dividing by frameScale (< 1 above 30 fps) cancels that factor.
                const float scale = frameScale(rdram);
                if (scale > 1e-4f)
                    scaleForce3(ctx, 1.0f / scale);
            }
            g_origMoveForce(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void applyTurnForce(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = GPR_U32(ctx, 31);
            if (ra == kBodyPushReturn)
                ctx->f[14] *= frameScale(rdram);
            else if (ra == kBrakeWobbleReturn || ra == kCollisionPushTurn2 || ra == kCollisionPushTurn1 ||
                     ra == kBurnoutPivotReturn || ra == kBikeUprightReturn1 || ra == kBikeUprightReturn2)
                scaleForce3(ctx, frameScale(rdram));
            logTurnForce(rdram, ctx);
            g_origTurnForce(rdram, ctx, runtime);
        }

        // 4.: which vehicles' blades count this frame. A handful of helicopters at most turn their rotors at once; the
        // slot not used for longest is taken for a new one. Game thread only.
        struct BladeClock
        {
            uint32_t vehicle = 0, frame = 0;
            float sum = 0.0f;  // the steps since the blades last counted
            bool counts = true;
        };
        BladeClock g_blades[8];

        PS2_GUEST_CC void bladeCollision(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (active())
            {
                const uint32_t vehicle = GPR_U32(ctx, 4), frame = rdU(rdram, kFrameCounter);
                BladeClock *slot = nullptr, *oldest = &g_blades[0];
                for (BladeClock &b : g_blades)
                {
                    if (b.vehicle == vehicle)
                        slot = &b;
                    if (frame - b.frame > frame - oldest->frame)
                        oldest = &b;
                }
                if (!slot)
                {
                    slot = oldest;
                    *slot = {vehicle, frame - 1u, 0.0f, true};
                }
                if (slot->frame != frame) // once a frame, whatever the number of rotors
                {
                    const float step = rdF(rdram, kTimeStep);
                    // a gap (the rotor stood still, the game was elsewhere) starts the count again
                    slot->sum = frame - slot->frame > 8u ? 0.0f : slot->sum + (step > 0.0f ? step : 0.0f);
                    slot->frame = frame;
                    // a frame about as long as the PS2's counts on its own (independent timing at 30 fps steps 33 /
                    // 34 ms: 1.65 / 1.70); what is over - or the little that was short - is carried, so the blades
                    // count once per 1/30 s on average at any frame length; a longer frame counts once, as on a PS2
                    // that dropped frames
                    slot->counts = slot->sum >= kPs2Step * 0.9f;
                    if (slot->counts)
                        slot->sum = std::clamp(slot->sum - kPs2Step, -0.1f * kPs2Step, kPs2Step);
                }
                if (!slot->counts)
                    ctx->f[13] = 0.0f;
            }
            g_origBlade(rdram, ctx, runtime);
        }

        // 5.: per cannon, the steps since its trail last moved on, and the set-up time the game wrote (Init writes it
        // anew when a cannon is taken again, which is how a new one is recognised). Game thread only.
        struct CannonClock
        {
            bool known = false;
            uint32_t vehicle = 0, setUp = 0, written = 0, frame = 0;
            float sum = 0.0f;
        };
        CannonClock g_cannons[kCannonCount];

        void wrU(uint8_t *rdram, uint32_t addr, uint32_t v) { std::memcpy(rdram + (addr & 0x1FFFFFFu), &v, 4); }
        void wrF(uint8_t *rdram, uint32_t addr, float v) { std::memcpy(rdram + (addr & 0x1FFFFFFu), &v, 4); }

        // 8.: fix unscaled 0.99f per-frame m_vecTurnSpeed decay on the airRes <= 0.1f path.
        PS2_GUEST_CC void applyAirResistance(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ent = GPR_U32(ctx, 4);
            const float airRes = ent ? rdF(rdram, ent + 0xDCu) : 1.0f;
            g_origAirRes(rdram, ctx, runtime);
            if (!active() || !ent || airRes > 0.1f)
                return;
            const float step = rdF(rdram, kTimeStep);
            if (step >= 0.0f && step < kPs2Step)
            {
                const float s = std::pow(0.99f, step / kPs2Step) / 0.99f;
                for (uint32_t off = 0x70u; off <= 0x78u; off += 4u)
                    wrF(rdram, ent + off, rdF(rdram, ent + off) * s);
            }
        }

        // 10. (cars): scale airborne free-wheel spin decay (*0.95) and gas/brake step (-0.1/+0.05) on veh+0x6FC..+0x708.
        PS2_GUEST_CC void carAxleWheels(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t veh = GPR_U32(ctx, 4), w0 = GPR_U32(ctx, 5), w1 = GPR_U32(ctx, 6);
            float oldSpin[2] = {};
            if (veh && w0 < 4u && w1 < 4u)
            {
                oldSpin[0] = rdF(rdram, veh + 0x6FCu + 4u * w0);
                oldSpin[1] = rdF(rdram, veh + 0x6FCu + 4u * w1);
            }
            g_origCarAxleWheels(rdram, ctx, runtime);
            if (!veh || w0 >= 4u || w1 >= 4u)
                return;
            const float scale = frameScale(rdram);
            if (scale >= 1.0f)
                return;
            const uint32_t modelInfo = rdU(rdram, veh + 0x160u);
            if (!modelInfo || (rdU(rdram, modelInfo + 0xD0u) & 0x2000000u))
                return;
            const float step = rdF(rdram, kTimeStep);
            for (int k = 0; k < 2; ++k)
            {
                const uint32_t w = k == 0 ? w0 : w1;
                if (rdF(rdram, veh + 0x690u + 4u * w) > 0.0f)
                    continue;
                const uint32_t spinAddr = veh + 0x6FCu + 4u * w;
                const uint32_t angAddr = veh + 0x6DCu + 4u * w;
                const float rawSpin = rdF(rdram, spinAddr), old = oldSpin[k];
                const bool isDecay = std::fabs(rawSpin - old * 0.95f) <= 1e-5f * (1.0f + std::fabs(old));
                const float corrSpin = isDecay ? old * std::pow(0.95f, scale) : old + (rawSpin - old) * scale;
                wrF(rdram, spinAddr, corrSpin);
                wrF(rdram, angAddr, rdF(rdram, angAddr) + (corrSpin - rawSpin) * step);
            }
        }

        // 10. (bikes): scale airborne front wheel coast-down (*0.95) and rear wheel spin step (+0.1/-0.2) + unscaled integration.
        PS2_GUEST_CC void bikeProcessControl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t bike = GPR_U32(ctx, 4);
            const float old590 = bike ? rdF(rdram, bike + 0x590u) : 0.0f;
            const float old594 = bike ? rdF(rdram, bike + 0x594u) : 0.0f;
            const float old598 = bike ? rdF(rdram, bike + 0x598u) : 0.0f;
            const float old59C = bike ? rdF(rdram, bike + 0x59Cu) : 0.0f;
            const uint32_t prevBike = g_trackBike;
            const uint8_t prevMask = g_bikeWheelHitMask;
            g_trackBike = bike;
            g_bikeWheelHitMask = 0;
            g_origBikeProcessControl(rdram, ctx, runtime);
            const uint8_t hitMask = g_bikeWheelHitMask;
            g_trackBike = prevBike;
            g_bikeWheelHitMask = prevMask;
            if (!bike)
                return;
            const float scale = frameScale(rdram);
            if (scale >= 1.0f)
                return;
            if (((rdU(rdram, bike + 0x48u) >> 4u) & 0x1Fu) == 2u) // STATUS_SIMPLE handles wheel rotation via 0x3E6B68
                return;
            if (!(hitMask & 1u) && std::fabs(old598) > 1e-6f)
            {
                const float raw598 = rdF(rdram, bike + 0x598u);
                const float new590 = rdF(rdram, bike + 0x590u);
                if (std::fabs(raw598 - old598 * 0.95f) <= 1e-5f * (1.0f + std::fabs(old598)) &&
                    std::fabs((new590 - old590) - raw598) <= 1e-5f * (1.0f + std::fabs(old598)))
                {
                    const float corr598 = old598 * std::pow(0.95f, scale);
                    wrF(rdram, bike + 0x598u, corr598);
                    wrF(rdram, bike + 0x590u, old590 + corr598 * scale);
                }
            }
            if (!(hitMask & 2u) && !(rdU(rdram, bike + 0x264u) & 0x2000u))
            {
                const float raw59C = rdF(rdram, bike + 0x59Cu);
                const float new594 = rdF(rdram, bike + 0x594u);
                if ((old59C != 0.0f || raw59C != 0.0f) &&
                    std::fabs(raw59C - old59C) <= 0.25f &&
                    std::fabs((new594 - old594) - raw59C) <= 1e-5f * (1.0f + std::fabs(raw59C)))
                {
                    const float corr59C = old59C + (raw59C - old59C) * scale;
                    wrF(rdram, bike + 0x59Cu, corr59C);
                    wrF(rdram, bike + 0x594u, old594 + corr59C * scale);
                }
            }
        }

        PS2_GUEST_CC void cannonUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t cannon = GPR_U32(ctx, 4);
            const uint32_t n = (cannon - kCannons) / kCannonSize;
            if (cannon >= kCannons && n < kCannonCount && (cannon - kCannons) % kCannonSize == 0u)
            {
                CannonClock &c = g_cannons[n];
                const uint32_t vehicle = rdU(rdram, cannon), setUp = rdU(rdram, cannon + 8u);
                const uint32_t frame = rdU(rdram, kFrameCounter);
                if (!active())
                {
                    if (c.known && setUp == c.written) // switched off mid-trail: the game's own time back
                        wrU(rdram, cannon + 8u, c.setUp);
                    c.known = false;
                }
                else
                {
                    if (!c.known || c.vehicle != vehicle || setUp != c.written)
                        c = {true, vehicle, setUp, setUp, frame - 1u, 0.0f};
                    else if (frame - c.frame > 8u) // a gap (the game was elsewhere): the count starts again
                        c.sum = 0.0f, c.frame = frame - 1u;
                    if (c.frame != frame) // once a frame
                    {
                        const float step = rdF(rdram, kTimeStep);
                        c.sum += step > 0.0f ? step : 0.0f;
                        c.frame = frame;
                        // as the blades in 4.: a frame about as long as the PS2's moves on by itself, shorter ones carry
                        const bool moves = c.sum >= kPs2Step * 0.9f;
                        if (moves)
                            c.sum = std::clamp(c.sum - kPs2Step, -0.1f * kPs2Step, kPs2Step);
                        // the game moves the trail on when set-up + 150 ms < now: the real time lets its own test
                        // decide (nothing in the first 150 ms), now holds it
                        c.written = moves ? c.setUp : rdU(rdram, kTimeMs);
                        wrU(rdram, cannon + 8u, c.written);
                    }
                }
            }
            g_origCannon(rdram, ctx, runtime);
        }

        void writeBrakeConstants(uint8_t *rdram)
        {
            const float scale = frameScale(rdram);
            for (size_t i = 0; i < sizeof(kBrakeConstants) / sizeof(kBrakeConstants[0]); ++i)
            {
                const float v = kBrakeConstants[i] * scale; // exact at 1
                std::memcpy(rdram + kBrakeSlots + 4u * i, &v, 4);
            }
        }

        template <PS2Runtime::RecompiledFunction *Original>
        PS2_GUEST_CC void processWheel(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            // VCS_WHEEL_LOG=1: each bike wheel's inputs - thrust, brake and grip (adhesion * step) for the frame, the
            // contact point's speed along the wheel (forward / sideways), wheels on the ground, and the wheel's state from
            // the last frame (0 rolling, 1 spinning, 2 skidding, 3 locked; through the pointer at the caller's sp)
            static const bool log = std::getenv("VCS_WHEEL_LOG") != nullptr;
            if (log && Original == &g_origWheelBike)
            {
                const uint32_t fwd = GPR_U32(ctx, 5), right = GPR_U32(ctx, 6), speed = GPR_U32(ctx, 7);
                std::fprintf(stderr, "[wheel] f %u veh %x step %.4f thrust %.5f brake %.5f adh %.5f p4 %.3f fwd %.5f side %.5f n %u state %u\n",
                             rdU(rdram, kFrameCounter), GPR_U32(ctx, 4), rdF(rdram, kTimeStep), ctx->f[12], ctx->f[13],
                             ctx->f[14] * rdF(rdram, kTimeStep), ctx->f[15], dot3(rdram, speed, fwd),
                             dot3(rdram, speed, right), GPR_U32(ctx, 9), rdU(rdram, rdU(rdram, GPR_U32(ctx, 29))));
            }
            if (Original == &g_origWheelBike && g_trackBike)
            {
                const uint32_t outSpinPtr = GPR_U32(ctx, 11); // $t3 = param_12
                if (outSpinPtr == g_trackBike + 0x598u)
                    g_bikeWheelHitMask |= 1u;
                else if (outSpinPtr == g_trackBike + 0x59Cu)
                    g_bikeWheelHitMask |= 2u;
            }
            writeBrakeConstants(rdram);
            (*Original)(rdram, ctx, runtime);
        }
    }

    void installPhysicsTimingHooks(PS2Runtime &runtime)
    {
        g_origMoveForce = runtime.lookupFunction(kApplyMoveForce);
        g_origTurnForce = runtime.lookupFunction(kApplyTurnForce);
        g_origWheelCar = runtime.lookupFunction(kProcessWheelCar);
        g_origWheelBike = runtime.lookupFunction(kProcessWheelBike);
        if (g_origMoveForce)
            runtime.replaceFunction(kApplyMoveForce, &applyMoveForce);
        else
            std::fprintf(stderr, "[physics-timing] ApplyMoveForce 0x%x not found: vehicle pushing and boat planing lift stay on the frame rate\n",
                         kApplyMoveForce);
        if (g_origTurnForce)
            runtime.replaceFunction(kApplyTurnForce, &applyTurnForce);
        else
            std::fprintf(stderr, "[physics-timing] ApplyTurnForce 0x%x not found: the push from bodies under wheels, brake wobble and vehicle pushing stay on the frame rate\n",
                         kApplyTurnForce);
        g_origBlade = runtime.lookupFunction(kBladeCollision);
        if (g_origBlade)
            runtime.replaceFunction(kBladeCollision, &bladeCollision);
        else
            std::fprintf(stderr, "[physics-timing] the blade test 0x%x not found: rotor damage stays on the frame rate\n", kBladeCollision);
        g_origCannon = runtime.lookupFunction(kCannonUpdate);
        if (g_origCannon)
            runtime.replaceFunction(kCannonUpdate, &cannonUpdate);
        else
            std::fprintf(stderr, "[physics-timing] the water cannon's update 0x%x not found: its reach stays on the frame rate\n", kCannonUpdate);
        g_origAirRes = runtime.lookupFunction(kApplyAirResistance);
        if (g_origAirRes)
            runtime.replaceFunction(kApplyAirResistance, &applyAirResistance);
        else
            std::fprintf(stderr, "[physics-timing] ApplyAirResistance 0x%x not found: angular air damping stays on the frame rate\n",
                         kApplyAirResistance);
        g_origCarAxleWheels = runtime.lookupFunction(kCarAxleWheels);
        if (g_origCarAxleWheels)
            runtime.replaceFunction(kCarAxleWheels, &carAxleWheels);
        else
            std::fprintf(stderr, "[physics-timing] CAutomobile axle wheels 0x%x not found: airborne wheel spin stays on the frame rate\n",
                         kCarAxleWheels);
        g_origBikeProcessControl = runtime.lookupFunction(kBikeProcessControl);
        if (g_origBikeProcessControl)
            runtime.replaceFunction(kBikeProcessControl, &bikeProcessControl);
        else
            std::fprintf(stderr, "[physics-timing] CBike::ProcessControl 0x%x not found: bike airborne wheel spin stays on the frame rate\n",
                         kBikeProcessControl);
        // The patched ProcessWheels read their constants from kBrakeSlots, so these hooks are not optional.
        if (g_origWheelCar)
            runtime.replaceFunction(kProcessWheelCar, &processWheel<&g_origWheelCar>);
        if (g_origWheelBike)
            runtime.replaceFunction(kProcessWheelBike, &processWheel<&g_origWheelBike>);
        if (!g_origWheelCar || !g_origWheelBike)
            std::fprintf(stderr, "[physics-timing] ProcessWheel 0x%x / 0x%x not found: vehicles roll without resistance\n",
                         kProcessWheelCar, kProcessWheelBike);
    }
}
