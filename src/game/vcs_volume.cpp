// The game's own Music / SFX volume as settings, kept in step with its pause menu (docs/game-internals.md 9c).
//
// The game keeps each level, 0..127, twice: in cSampleManager (0x4CDA08: +4 effects, +5 music), which the sound code
// uses and the pause menu's Audio page draws as eight bars (16 a step, the last step to 127), and in the menu's prefs
// (*0x48F3D0: +0x38 effects, +0x3C music), which saves carry. The menu's setters FUN_002FA538 (effects) / FUN_002FA5B0
// (music) store both; a save load (FUN_003ADAE0, at boot too) and the defaults routine FUN_003163D8 (0x70 / 0x50) call
// them as well.
//
// The settings hold the level in percent of 127. A changed setting is written to both places at the next frame kick; a
// change made in the pause menu (the setters called from its Audio page, return addresses 0x2AEF84 / 0x2AEF9C) is
// taken into the setting and vcs.ini. Every other call of the setters gets the setting's level, so loading a save or
// a new game keeps the player's levels, as PC games keep their settings across saves.
//
// VCS_GAME_VOLUME=0 leaves the game's levels alone (no hooks, no sync): the settings then do nothing.
#include "game/vcs_volume.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_control.h"
#include "settings/settings.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace vcs::volume
{
    namespace
    {
        constexpr uint32_t kSampleManager = 0x004CDA08u; // cSampleManager
        constexpr uint32_t kMenuPrefs = 0x0048F3D0u;     // pointer to the menu's prefs (0 until the menu exists)
        bool g_enabled = false;

        struct Level
        {
            const char *key;
            const char *name;
            uint32_t byte;       // offset in cSampleManager (u8)
            uint32_t prefs;      // offset in the menu's prefs (u32)
            uint32_t setter;     // the menu's setter (a0 = menu, a1 = level)
            uint32_t menuReturn; // its call from the pause menu's Audio page
            PS2Runtime::RecompiledFunction original = nullptr;
            std::atomic<int> target{-1};  // the level to put into the game, 0..127 (-1: none yet)
            std::atomic<bool> dirty{false};
            std::atomic<int> seen{-1};    // the game's level at the last frame kick
            std::atomic<int> adopted{-1}; // a change in the pause menu, for the host thread
        };
        Level g_levels[2] = {
            {"audio.music_volume", "music", 5, 0x3C, 0x002FA5B0u, 0x002AEF84u},
            {"audio.sfx_volume", "effects", 4, 0x38, 0x002FA538u, 0x002AEF9Cu},
        };

        int toGame(int percent) { return (percent * 127 + 50) / 100; }
        int toPercent(int level) { return (level * 100 + 63) / 127; }

        void onLevelChange(Level &l, int percent)
        {
            // the game's level when it is what the setting shows (a pause-menu step, 16 apart, isn't a whole percent)
            const int seen = l.seen.load();
            l.target.store(seen >= 0 && toPercent(seen) == percent ? seen : toGame(percent));
            l.dirty.store(true);
        }

        template <int I>
        PS2_GUEST_CC void setter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            Level &l = g_levels[I];
            const int target = l.target.load();
            if (GPR_U32(ctx, 31) != l.menuReturn && target >= 0)
            {
                if (static_cast<int>(GPR_U32(ctx, 5) & 0xFF) != target)
                    std::fprintf(stderr, "[volume] the game sets the %s level %u (ra=0x%x): the player's %d instead\n", l.name,
                                 GPR_U32(ctx, 5) & 0xFF, GPR_U32(ctx, 31), target);
                SET_GPR_U32(ctx, 5, static_cast<uint32_t>(target));
            }
            l.original(rdram, ctx, runtime);
        }
    }

    void registerSettings()
    {
        using settings::Type;
        settings::Spec s;
        s.key = "audio.music_volume";
        s.page = "Audio";
        s.section = "Volume";
        s.label = "Music volume";
        s.help = "The radio stations and the other music. The same level as Music Volume in the game's pause menu "
                 "(Audio): a change in either shows in both, and loading a save keeps it.";
        s.type = Type::Integer;
        s.min = 0;
        s.max = 100;
        s.format = "%d%%";
        s.defaultValue = toPercent(0x50); // the game's default (5 bars)
        s.onChange = [](int v) { onLevelChange(g_levels[0], v); };
        settings::add(s);

        s = {};
        s.key = "audio.sfx_volume";
        s.page = "Audio";
        s.section = "Volume";
        s.label = "Sound effects volume";
        s.help = "Everything but the music: vehicles, weapons, the city, people. The same level as SFX Volume in the game's "
                 "pause menu (Audio): a change in either shows in both, and loading a save keeps it.";
        s.type = Type::Integer;
        s.min = 0;
        s.max = 100;
        s.format = "%d%%";
        s.defaultValue = toPercent(0x70); // the game's default (7 bars)
        s.onChange = [](int v) { onLevelChange(g_levels[1], v); };
        settings::add(s);
    }

    void install(PS2Runtime &runtime)
    {
        const char *env = std::getenv("VCS_GAME_VOLUME");
        g_enabled = !(env && env[0] == '0');
        if (!g_enabled)
            std::fprintf(stderr, "[volume] the game's music / effects levels left alone (VCS_GAME_VOLUME=0)\n");
        auto hook = [&](Level &l, PS2Runtime::RecompiledFunction fn) {
            l.original = runtime.lookupFunction(l.setter);
            if (l.original)
                runtime.replaceFunction(l.setter, fn);
            else
                std::fprintf(stderr, "[volume] the %s setter 0x%x not found: a save load sets its own level\n", l.name, l.setter);
        };
        if (g_enabled)
        {
            hook(g_levels[0], &setter<0>);
            hook(g_levels[1], &setter<1>);
        }
        // soundstate -> ok music=<0..127> effects=<0..127> (the game's levels at the last frame kick; tools/controls_check.py)
        ps2control::addCommand("soundstate", [](const std::string &, ps2control::Reply reply) {
            char text[64];
            std::snprintf(text, sizeof(text), "ok music=%d effects=%d", g_levels[0].seen.load(), g_levels[1].seen.load());
            reply(text);
        });
    }

    void onGameFrame(uint8_t *rdram)
    {
        if (!g_enabled)
            return;
        uint32_t prefs;
        std::memcpy(&prefs, rdram + kMenuPrefs, 4);
        prefs &= PS2_RAM_MASK;
        if (!prefs)
            return;
        for (Level &l : g_levels)
        {
            if (l.dirty.exchange(false) && l.target.load() >= 0)
            {
                const int target = l.target.load();
                const uint32_t level = static_cast<uint32_t>(target);
                rdram[kSampleManager + l.byte] = static_cast<uint8_t>(level);
                std::memcpy(rdram + prefs + l.prefs, &level, 4);
                l.seen.store(target);
                continue;
            }
            const int now = rdram[kSampleManager + l.byte];
            const int seen = l.seen.exchange(now);
            if (seen >= 0 && now != seen) // the pause menu's slider
            {
                l.target.store(now);
                l.adopted.store(now);
            }
        }
    }

    void onHostFrame()
    {
        for (Level &l : g_levels)
            if (const int level = l.adopted.exchange(-1); level >= 0)
                settings::set(l.key, toPercent(level));
    }
}
