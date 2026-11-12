// Enhancements > Combat: the fire rate of people shooting from vehicles (docs/game-internals.md 12c).
//
// The NPC drive-by (FUN_00216768, and FUN_002168E8 for the other vehicle kinds) calls CWeapon::Fire (FUN_002C0F98)
// whenever its burst gate FUN_00216690 says so - every frame of a 0.5-1 s burst - and Fire never checks the weapon's
// timer: for the instant-hit guns it sets the timer to "now", leaving the pace to the firing animation, which a
// passenger in a car does not have. So the original game fires a shot every frame: 30 shots/s on the PS2 (the SMG's own
// rate is 90 ms, 11 a second), and 60 / 144 a second at 60 / 144 fps.
//
// The hook lets a call from those two places through only when the shooter's next shot is due; otherwise it returns 0
// as Fire does when it cannot fire, and the gate and the animation go on as before. The next shot is due one interval
// after the last one, counted from when it was due rather than when it came, so the rate is exact whatever the frame
// length (a late shot does not delay the next; after a pause or a reload the count starts again). The interval:
//   As on the PS2 (default)  1/30 s - the original at 30 fps (each frame then lets its shot through: 2 ms tolerance
//                            for the clock's whole milliseconds), and the same rate above 30 fps.
//   The gun's own rate       the weapon's firing rate (weapon info +0xC: 90 ms for the SMG, Scorpion and Mac 10, 120
//                            for the Micro SMG) - what the player's own drive-bys use; fixes the original game's.
// Below 30 fps every frame is due: the original either way.
//
// VCS_DRIVEBY_HOOKS=0 hooks nothing (the original: a shot every frame). `driveby` -> ok mode let skipped.
#include "game/vcs_driveby.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_control.h"
#include "settings/settings.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace vcs::driveby
{
    namespace
    {
        constexpr uint32_t kFire = 0x002C0F98u;        // CWeapon::Fire(weapon a0, shooter a1, source a2)
        constexpr uint32_t kFromCar = 0x002168B4u;     // its call in FUN_00216768 (the NPC drive-by), return address
        constexpr uint32_t kFromOther = 0x002169D4u;   // its call in FUN_002168E8 (the same for the other vehicle kinds)
        constexpr uint32_t kTimeMs = 0x004CD104u;      // CTimer::m_snTimeInMilliseconds
        constexpr uint32_t kWeaponInfo = 0x00712770u;  // *(*this + 4) + type * 0x70; +0xC the firing rate (ms)
        constexpr uint32_t kPedPool = 0x00487A94u;     // *(*this): the first ped, 0xD20 each
        constexpr uint32_t kPedSize = 0xD20u;
        constexpr double kPs2Interval = 1000.0 / 30.0; // a frame on the PS2
        constexpr double kTolerance = 2.0;             // ms: the clock's whole milliseconds (33 / 34 ms frames at 30 fps)

        enum Mode
        {
            Ps2 = 0,
            Gun = 1,
        };

        std::atomic<int> g_mode{Ps2};
        bool g_installed = false;
        PS2Runtime::RecompiledFunction g_origFire = nullptr;
        std::atomic<uint64_t> g_let{0}, g_skipped{0};

        // per ped pool slot (game thread): the shooter, the last call let through (its time), the last shot counted (the
        // weapon timer Fire set), when the next is due
        struct Shooter
        {
            uint32_t ped = 0;
            uint32_t letAt = 0;
            uint32_t lastShot = 0;
            double due = -1.0; // < 0: none (fire)
        };
        Shooter g_shooters[256];

        uint32_t rd32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & 0x01FFFFFFu), 4);
            return v;
        }

        double interval(const uint8_t *ram, uint32_t type)
        {
            if (g_mode.load(std::memory_order_relaxed) == Gun && type < 64)
            {
                if (const uint32_t table = rd32(ram, kWeaponInfo) & 0x01FFFFFFu)
                    if (const uint32_t info = rd32(ram, table + 4) & 0x01FFFFFFu)
                    {
                        const int32_t rate = static_cast<int32_t>(rd32(ram, info + type * 0x70u + 0xCu));
                        if (rate > 0 && rate <= 5000)
                            return rate;
                    }
            }
            return kPs2Interval;
        }

        PS2_GUEST_CC void fire(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = GPR_U32(ctx, 31);
            if (ra == kFromCar || ra == kFromOther)
            {
                const uint32_t weapon = GPR_U32(ctx, 4), ped = GPR_U32(ctx, 5);
                const uint32_t base = rd32(rdram, rd32(rdram, kPedPool) & 0x01FFFFFFu);
                const uint32_t slot = (ped - base) / kPedSize;
                if (weapon && ped >= base && slot < 256 && (ped - base) % kPedSize == 0)
                {
                    Shooter &s = g_shooters[slot];
                    if (s.ped != ped)
                        s = Shooter{ped, 0, 0, -1.0};
                    const uint32_t now = rd32(rdram, kTimeMs);
                    const uint32_t timer = rd32(rdram, weapon + 0x14);
                    const double step = interval(rdram, rd32(rdram, weapon + 4));
                    // the call let through fired: Fire left the timer at that moment (the weapon's update puts the
                    // state back to ready a frame later, so the state does not tell; a reload's timer is its end, never
                    // a call's time). The next shot is one interval after the last one was due - or after the shot
                    // itself when it came long after (a new burst, a reload)
                    if (s.letAt && timer == s.letAt && timer != s.lastShot)
                    {
                        s.due = (s.due >= 0.0 && timer < s.due + step) ? s.due + step : timer + step;
                        s.lastShot = timer;
                    }
                    if (s.due >= 0.0 && now + kTolerance < s.due)
                    {
                        g_skipped.fetch_add(1, std::memory_order_relaxed);
                        SET_GPR_U32(ctx, 2, 0u); // as Fire when it cannot fire (the callers ignore it)
                        ctx->pc = ra;
                        return;
                    }
                    s.letAt = now;
                    g_let.fetch_add(1, std::memory_order_relaxed);
                }
            }
            g_origFire(rdram, ctx, runtime);
        }
    }

    void registerSettings()
    {
        settings::Spec s;
        s.key = "enhance.driveby_rate";
        s.page = "Enhancements";
        s.section = "Combat";
        s.label = "Drive-by fire rate";
        s.help = "How fast people shooting from vehicles fire. As on the PS2: 30 shots a second at any frame rate (the "
                 "original game fires once a frame, so faster above 30 fps). The gun's own rate: as the weapon fires "
                 "elsewhere (11 a second for the SMG) - the original game's drive-bys were too fast.";
        s.type = settings::Type::Choice;
        s.choices = {{"As on the PS2", Ps2}, {"The gun's own rate", Gun}};
        s.defaultValue = Ps2;
        s.env = "VCS_DRIVEBY_RATE";
        s.onChange = [](int v) { g_mode.store(v, std::memory_order_relaxed); };
        settings::add(s);
    }

    void install(PS2Runtime &runtime)
    {
        if (const char *env = std::getenv("VCS_DRIVEBY_HOOKS"); env && env[0] == '0')
            std::fprintf(stderr, "[driveby] NPC drive-bys left alone: a shot every frame (VCS_DRIVEBY_HOOKS=0)\n");
        else if ((g_origFire = runtime.lookupFunction(kFire)))
        {
            runtime.replaceFunction(kFire, &fire);
            g_installed = true;
        }
        else
            std::fprintf(stderr, "[driveby] CWeapon::Fire not found: NPC drive-bys left alone\n");

        // driveby -> ok hooked mode let skipped (calls from the NPC drive-by let through / held back, since the start)
        ps2control::addCommand("driveby", [](const std::string &, ps2control::Reply reply) {
            char text[128];
            std::snprintf(text, sizeof(text), "ok hooked=%d mode=%d let=%llu skipped=%llu", g_installed ? 1 : 0,
                          g_mode.load(), (unsigned long long)g_let.load(), (unsigned long long)g_skipped.load());
            reply(text);
        });
    }
}
