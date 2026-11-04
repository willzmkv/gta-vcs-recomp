// GTA: Vice City Stories (PS2) native launcher.
//
// Finds the user's disc image, checks it holds the supported executable,
// maps it as cdrom0: and boots the recompiled game.
//
//   vcs.exe [path\to\Grand Theft Auto - Vice City Stories (USA).iso]
//
// Without an argument it uses "iso=" from vcs.ini next to the exe, then the
// first *.iso in the exe's folder or the current folder.
#include "ps2_runtime.h"
#include "runtime/gs/gs_hw.h"
#include "runtime/ps2_disc_image.h"
#include "runtime/ps2_perf.h"

#include "debug/crash.h"
#include "debug/game_info.h"
#include "debug/profiler.h"
#include "debug/rig.h"
#include "game/vcs_trainer.h"
#include "debug/selftest.h"
#include "debug/watchdog.h"
#include "disc/iso_image.h"
#include "disc/vcs_disc_id.h"
#include "game/vcs_display.h"
#include "game/vcs_framerate.h"
#include "game/vcs_movie.h"
#include "game/vcs_hud.h"
#include "game/vcs_input.h"
#include "game/vcs_volume.h"
#include "game/vcs_fog.h"
#include "game/vcs_distance.h"
#include "game/vcs_distant.h"
#include "game/vcs_memory.h"
#include "game/vcs_world.h"
#include "mods/modloader.h"
#include "mods/rwconv.h"
#include "platform/platform.h"
#include "render/presenter.h"
#include "rhi/rhi.h"
#include "settings/settings.h"
#include "ui/settings_menu.h"
#include "ui/trainer_menu.h"
#include "runtime/ps2_audio_out.h"
#include "runtime/ps2_control.h"
#include "util/sha1.h"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#if defined(PS2X_PGO_GEN)
extern "C" int __llvm_profile_write_file(void); // clang's profile runtime (instrumented build, tools/build.py --opt pgo)
#endif

namespace fs = std::filesystem;

namespace vcs
{
    void configureWatch(uint8_t *rdram); // debug/watch.cpp
}

namespace
{
    using vcs::disc::kElfName;
    using vcs::disc::kElfSha1;

    class MappedDisc final : public IPS2DiscImage
    {
    public:
        explicit MappedDisc(const vcs::IsoImage &iso) : m_iso(iso) {}

        bool findFile(std::string_view ps2Path, uint32_t &lsn, uint32_t &sizeBytes, bool &isDirectory) const override
        {
            vcs::IsoEntry entry;
            if (!m_iso.find(ps2Path, entry))
                return false;
            lsn = entry.lba;
            sizeBytes = entry.size;
            isDirectory = entry.isDirectory;
            return true;
        }

        bool readBytes(uint64_t offset, size_t byteCount, void *destination) const override
        {
            return m_iso.readBytes(offset, byteCount, destination);
        }

        uint32_t totalSectors() const override { return m_iso.totalSectors(); }

        bool listDirectory(std::string_view ps2Path, std::vector<DirEntry> &out) const override
        {
            const vcs::IsoImage::Children *kids = m_iso.children(ps2Path);
            if (!kids)
                return false;
            out.clear();
            out.reserve(kids->size());
            for (const auto &[name, entry] : *kids)
                out.push_back(DirEntry{name, entry.lba, entry.size, entry.isDirectory});
            return true;
        }

    private:
        const vcs::IsoImage &m_iso;
    };

    fs::path exeDirectory()
    {
#if defined(_WIN32)
        wchar_t buffer[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        if (n > 0 && n < MAX_PATH)
            return fs::path(buffer).parent_path();
#elif defined(__linux__)
        std::error_code ec;
        const fs::path self = fs::read_symlink("/proc/self/exe", ec);
        if (!ec && !self.empty())
            return self.parent_path();
#endif
        return fs::current_path();
    }

    fs::path dataDirectory()
    {
        // VCS_DATA_DIR: another save / cache folder (scripted test runs start from an empty memory card)
        if (const char *dir = std::getenv("VCS_DATA_DIR"); dir && *dir)
            return fs::path(dir);
#if defined(_WIN32)
        if (const char *local = std::getenv("LOCALAPPDATA"))
            return fs::path(local) / "VCSRecomp";
#endif
        if (const char *xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg == '/') // absolute only (the XDG spec)
            return fs::path(xdg) / "VCSRecomp";
        if (const char *home = std::getenv("HOME"))
            return fs::path(home) / ".local/share/VCSRecomp";
        return exeDirectory() / "userdata";
    }

    fs::path isoFromIni(const fs::path &ini)
    {
        std::ifstream in(ini);
        std::string line;
        while (std::getline(in, line))
        {
            if (line.rfind("iso=", 0) == 0)
            {
                std::string value = line.substr(4);
                while (!value.empty() && (value.back() == '\r' || value.back() == ' ' || value.back() == '"'))
                    value.pop_back();
                if (!value.empty() && value.front() == '"')
                    value.erase(0, 1);
                return fs::u8path(value);
            }
        }
        return {};
    }

    void setEnv(const char *name, const std::string &value)
    {
#if defined(_WIN32)
        _putenv_s(name, value.c_str());
#else
        setenv(name, value.c_str(), 1);
#endif
    }

    // Display aspect limits: narrower than 4:3 would cut the original view (Hor+ only widens), wider than 32:9 is not
    // checked (the horizontal view reaches 124 degrees at 32:9 with the game's 70-degree FOV).
    constexpr float kNarrowestAspect = 4.0f / 3.0f, kWidestAspect = 32.0f / 9.0f;
    float g_appliedAspect = 0.0f;

    // The window's shape for "Match window", false while there is no window (or it is minimised).
    bool windowAspect(float &aspect)
    {
        int w = 0, h = 0;
        platform::windowSize(w, h);
        if (w <= 0 || h <= 0)
            return false;
        aspect = std::clamp(static_cast<float>(w) / static_cast<float>(h), kNarrowestAspect, kWidestAspect);
        return true;
    }

    // display.aspect: 43, 1610, 169, 219, 329, or 0 = the window's shape
    float aspectFor(int code)
    {
        switch (code)
        {
        case 1610:
            return 16.0f / 10.0f;
        case 169:
            return 16.0f / 9.0f;
        case 219:
            return 21.0f / 9.0f;
        case 329:
            return 32.0f / 9.0f;
        case 0:
        {
            float a = g_appliedAspect > 0.0f ? g_appliedAspect : 4.0f / 3.0f; // no window yet: followWindowAspect
            windowAspect(a);
            return a;
        }
        default:
            return 4.0f / 3.0f;
        }
    }

    // Display aspect: the presenter's ratio, the game camera's Hor+ widening and the HUD squeeze (game/vcs_display.h).
    void applyAspect()
    {
        const float aspect = aspectFor(settings::value("display.aspect"));
        g_appliedAspect = aspect;
        present::setDisplayAspect(aspect);
        vcs::setAspectWidening(aspect / (4.0f / 3.0f));
        gsHwSetAspectWidening(aspect / (4.0f / 3.0f)); // post effects keep their 4:3 proportions
        const int hud = settings::value("display.hud");
        gsHwSetHudSqueeze(hud != 1 ? (4.0f / 3.0f) / aspect : 1.0f);
        vcs::setHudAnchoring(hud == 2); // per-element placement (game/vcs_hud.h)
        std::fprintf(stderr, "[settings] display aspect %.3f, HUD %s\n", aspect, hud == 0 ? "4:3" : hud == 2 ? "screen edges" : "stretched");
    }

    // Each host frame: Alt+Enter switches between full screen and the window (the game's pad leaves Enter alone while
    // Alt is held, so it does not also press Start). An environment override (VCS_FULLSCREEN) switches for this run only.
    void altEnter()
    {
        auto down = [](int k) { return platform::keyDown(k) || ps2control::keyDown(k); };
        auto pressed = [](int k) { return platform::keyPressed(k) || ps2control::keyPressed(k); };
        if (!(down(platform::KEY_LEFT_ALT) || down(platform::KEY_RIGHT_ALT)) ||
            !(pressed(platform::KEY_ENTER) || pressed(platform::KEY_KP_ENTER)))
            return;
        const int next = platform::fullscreen() ? 0 : 1;
        if (!settings::set("display.window", next))
            platform::setFullscreen(next != 0);
        std::fprintf(stderr, "[settings] Alt+Enter: %s\n", next ? "full screen" : "windowed");
    }

    // Each host frame, before the menus draw: the screenshot key (Keyboard > Menus, F8) saves this frame's picture as
    // shown - menus and overlays included, at the window's size - as screenshots/vcs_<date>_<time>.png next to vcs.exe
    // (VCS_SCREENSHOT_DIR: another folder). The notices of earlier shots are left out of it; nothing while the settings
    // menu waits for a key to bind.
    void screenshotKey()
    {
        const int key = settings::value("controls.screenshot_key");
        if (ui::settingsMenuWaitingForKey() || !(platform::keyPressed(key) || ps2control::keyPressed(key)))
            return;
        ui::hideScreenshotNotices();
        const char *env = std::getenv("VCS_SCREENSHOT_DIR");
        const fs::path dir = env && *env ? fs::path(env) : exeDirectory() / "screenshots";
        std::error_code ec;
        fs::create_directories(dir, ec);
        const std::time_t t = std::time(nullptr);
        std::tm local{};
#if defined(_WIN32)
        localtime_s(&local, &t);
#else
        localtime_r(&t, &local);
#endif
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "vcs_%Y-%m-%d_%H-%M-%S", &local);
        fs::path path = dir / (std::string(stamp) + ".png");
        for (int n = 2; fs::exists(path, ec); ++n) // several in one second
            path = dir / (std::string(stamp) + "_" + std::to_string(n) + ".png");
        const auto asked = std::chrono::steady_clock::now();
        ps2control::requestScreenshot(path.string(), [path, asked](const std::string &size) {
            if (size.empty())
            {
                std::fprintf(stderr, "[screenshot] could not write %s\n", path.string().c_str());
                vcs::trainer::notifyScreenshot("Screenshot failed: cannot write " + path.parent_path().string());
                return;
            }
            std::fprintf(stderr, "[screenshot] %s (%s, %.0f ms)\n", path.string().c_str(), size.c_str(),
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - asked).count());
            vcs::trainer::notifyScreenshot("Screenshot saved: " + path.filename().string());
        });
    }

    // Each host frame: "Match window" follows the window as it is resized, moved to another monitor or made full screen.
    void followWindowAspect()
    {
        float aspect = 0.0f;
        if (settings::value("display.aspect") == 0 && windowAspect(aspect) && std::fabs(aspect - g_appliedAspect) > 1e-3f)
            applyAspect();
    }

    // Display / Graphics presets: the settings each one sets (the rest stay as they are). Authentic is their defaults,
    // except the frame rate: the default is 60 fps with frame-rate independent timing (DECISIONS.md 2026-10-30), the
    // PS2's is 30 with its own timing.
    struct Preset
    {
        const char *key;
        int value;
    };
    const Preset kEnhanced[] = {{"display.aspect", 0},   {"display.hud", 2},    {"display.scale", 0},
                                {"display.msaa", 4},     {"display.aniso", 16}, {"display.timing", 1},
                                {"display.fps", vcs::kCapRefresh}, {"display.vsync", 0}};
    const Preset kLight[] = {{"display.aspect", 0}, {"display.hud", 2},    {"display.scale", 1},  {"display.msaa", 1},
                             {"display.aniso", 4},  {"display.timing", 1}, {"display.fps", 60}, {"display.vsync", 0}};
    const char *const kPresetKeys[] = {"display.aspect", "display.hud", "display.scale", "display.msaa",
                                       "display.aniso",  "display.timing", "display.fps", "display.vsync"};

    template <size_t N> void applyPreset(const char *name, const Preset (&values)[N])
    {
        for (const Preset &p : values)
            settings::set(p.key, p.value);
        std::fprintf(stderr, "[settings] preset %s\n", name);
    }

    const Preset kAuthenticTiming[] = {{"display.timing", 0}, {"display.fps", 30}};

    void applyAuthentic()
    {
        for (const char *key : kPresetKeys)
            for (const settings::Spec &s : settings::all())
                if (s.key == key)
                    settings::set(s.key, s.defaultValue);
        for (const Preset &p : kAuthenticTiming)
            settings::set(p.key, p.value);
        std::fprintf(stderr, "[settings] preset Authentic\n");
    }

    // Each host frame: the frame-rate counter (Display > Show frame rate) - game frames per second, measured over half
    // a second, in the corner the setting names.
    void drawFrameRateCounter()
    {
        const int corner = settings::value("display.fps_counter");
        if (corner == 0 || ps2control::hiddenMode())
            return;
        static auto since = std::chrono::steady_clock::now();
        static uint64_t framesThen = vcs::rig::gameFrames();
        static float fps = 0.0f;
        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - since).count();
        if (seconds >= 0.5)
        {
            const uint64_t frames = vcs::rig::gameFrames();
            fps = static_cast<float>((frames - framesThen) / seconds);
            framesThen = frames;
            since = now;
        }
        char text[32];
        std::snprintf(text, sizeof(text), "%.0f fps", fps);
        const ImGuiViewport *vp = ImGui::GetMainViewport();
        const float size = std::max(16.0f, vp->Size.y / 40.0f);
        ImFont *font = ImGui::GetFont();
        const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
        const float margin = size * 0.6f;
        const float x = corner == 1 || corner == 3 ? vp->Pos.x + margin : vp->Pos.x + vp->Size.x - margin - extent.x;
        const float y = corner == 1 || corner == 2 ? vp->Pos.y + margin : vp->Pos.y + vp->Size.y - margin - extent.y;
        ImDrawList *draw = ImGui::GetForegroundDrawList();
        draw->AddText(font, size, ImVec2(x + 1.5f, y + 1.5f), IM_COL32(0, 0, 0, 200), text);
        draw->AddText(font, size, ImVec2(x, y), IM_COL32(255, 255, 160, 255), text);
    }

    // The player settings (settings/settings.h). The menu shows pages (tabs) in the order they first appear here, each
    // page's sections likewise, and the rows of a section in this order. vcs.ini next to vcs.exe holds them along
    // with iso=; a setting's vcs.ini key stays the same when it moves to another page.
    void registerSettings()
    {
        using settings::Apply;
        using settings::Type;
        settings::Spec s;

        s = {};
        s.key = "presets.authentic";
        s.page = "Display";
        s.section = "Presets";
        s.label = "Authentic (PS2)";
        s.help = "The PS2's picture and timing: 4:3, original resolution, no anti-aliasing, 30 fps. Sets the Display and "
                 "Graphics pages; controls, sound and the window mode stay as they are.";
        s.type = Type::Action;
        s.button = "Apply";
        s.onChange = [](int) { applyAuthentic(); };
        settings::add(s);

        s = {};
        s.key = "presets.enhanced";
        s.page = "Display";
        s.section = "Presets";
        s.label = "Enhanced";
        s.help = "Widescreen in the window's shape with the HUD at the screen edges, rendered at the window's resolution, "
                 "4x anti-aliasing, 16x texture filtering and frame-rate independent timing at the display's refresh rate. "
                 "Needs a reasonably modern graphics card; resolution, anti-aliasing and timing apply after a restart.";
        s.type = Type::Action;
        s.button = "Apply";
        s.onChange = [](int) { applyPreset("Enhanced", kEnhanced); };
        settings::add(s);

        s = {};
        s.key = "presets.light";
        s.page = "Display";
        s.section = "Presets";
        s.label = "Widescreen, light";
        s.help = "Widescreen and 60 fps for older PCs and integrated graphics: the original resolution, no anti-aliasing, "
                 "4x texture filtering. Resolution and timing apply after a restart.";
        s.type = Type::Action;
        s.button = "Apply";
        s.onChange = [](int) { applyPreset("Widescreen, light", kLight); };
        settings::add(s);

        s = {};
        s.key = "display.window";
        s.page = "Display";
        s.section = "Screen";
        s.label = "Window mode";
        s.help = "Full screen covers the monitor the window is on, at the desktop's resolution (borderless: switching "
                 "and Alt+Tab are instant). Alt+Enter switches too.";
        s.type = Type::Choice;
        s.choices = {{"Windowed", 0}, {"Full screen", 1}};
        s.defaultValue = 0;
        s.env = "VCS_FULLSCREEN";
        s.onChange = [](int v) { platform::setFullscreen(v != 0); };
        s.read = [] { return platform::fullscreen() ? 1 : 0; };
        settings::add(s);

        s = {};
        s.key = "display.aspect";
        s.page = "Display";
        s.section = "Screen";
        s.label = "Aspect ratio";
        s.help = "Widescreen and ultrawide show more to the sides (Hor+); 4:3 is the original picture. Match window "
                 "follows the window's shape (between 4:3 and 32:9), for monitors such as 3440x1440 that are not "
                 "exactly 21:9.";
        s.type = Type::Choice;
        s.choices = {{"4:3 (original)", 43}, {"16:10", 1610}, {"16:9", 169}, {"21:9", 219}, {"32:9", 329}, {"Match window", 0}};
        s.defaultValue = 43;
        s.env = "VCS_ASPECT";
        s.onChange = [](int) { applyAspect(); };
        settings::add(s);

        s = {};
        s.key = "display.hud";
        s.page = "Display";
        s.section = "Screen";
        s.label = "HUD";
        s.help = "In widescreen: keep the HUD, text and menus at 4:3 in the middle, move each HUD element to the screen "
                 "edge it is near (keeping its 4:3 shape), or stretch them to the screen.";
        s.type = Type::Choice;
        s.choices = {{"Keep 4:3", 0}, {"Screen edges", 2}, {"Stretch", 1}};
        s.env = "VCS_HUD";
        s.defaultValue = 0;
        s.onChange = [](int) { applyAspect(); };
        settings::add(s);

        s = {};
        s.key = "display.scale";
        s.alias = "scale";
        s.page = "Graphics";
        s.section = "Image quality";
        s.label = "Internal resolution";
        s.help = "Renders the game at a multiple of 640x448 (in widescreen as many times wider as the view: 1x at 16:9 "
                 "is 853x448), or at the window's own resolution (sharpest; the effects keep the original's look). "
                 "Costs GPU time.";
        s.type = Type::Choice;
        s.choices = {{"1x (original)", 1}, {"2x", 2}, {"3x", 3}, {"4x", 4}, {"6x", 6}, {"8x", 8}, {"Match window", 0}};
        s.defaultValue = 1;
        s.apply = Apply::Restart;
        s.env = "PS2X_HW_SCALE";
        s.onChange = [](int v) { setEnv("PS2X_HW_SCALE", std::to_string(v)); }; // read when the renderer starts
        settings::add(s);

        auto applyVolume = [](int) {
            auto gain = [](int percent) { return (percent / 100.0f) * (percent / 100.0f); }; // even steps to the ear
            ps2AudioSetVolume(gain(settings::value("audio.volume")), gain(settings::value("audio.movie_volume")));
        };
        s = {};
        s.key = "audio.volume";
        s.page = "Audio";
        s.section = "Volume";
        s.label = "Master volume";
        s.help = "Everything the game plays, movies included.";
        s.type = Type::Integer;
        s.min = 0;
        s.max = 100;
        s.format = "%d%%";
        s.defaultValue = 100;
        s.onChange = applyVolume;
        settings::add(s);

        vcs::volume::registerSettings(); // music / sound effects: the game's own levels (game/vcs_volume.h)

        s = {};
        s.key = "audio.movie_volume";
        s.page = "Audio";
        s.section = "Volume";
        s.label = "Movie volume";
        s.help = "The intro movie and the other full-motion videos, relative to the master volume.";
        s.type = Type::Integer;
        s.min = 0;
        s.max = 100;
        s.format = "%d%%";
        s.defaultValue = 100;
        s.onChange = applyVolume;
        settings::add(s);

        s = {};
        s.key = "audio.output";
        s.page = "Audio";
        s.section = "Output";
        s.label = "Speakers";
        s.help = "Mono mixes left and right together (one speaker or ear); Swapped exchanges them (headphones worn the other "
                 "way round).";
        s.type = Type::Choice;
        s.choices = {{"Stereo", 0}, {"Mono", 1}, {"Stereo, swapped", 2}};
        s.defaultValue = 0;
        s.onChange = [](int v) { ps2AudioSetOutputMode(v); };
        settings::add(s);

        s = {};
        s.key = "display.timing";
        s.page = "Display";
        s.section = "Frame rate";
        s.label = "Game timing";
        s.help = "Frame-rate independent (the default): paced by the PC's clock to the frame rate cap, game time follows "
                 "real time. Original: the game paces itself on the PS2's 60 Hz VBlank (30 fps, or 60 with a cap of 60 or "
                 "more) and slows down when the PC can't keep up. Behaviour at high frame rates is still being fixed.";
        s.type = Type::Choice;
        s.choices = {{"Original (PS2)", 0}, {"Frame-rate independent", 1}};
        s.defaultValue = 1;
        s.apply = Apply::Restart;
        s.env = "VCS_TIMING";
        s.onChange = [](int v) { vcs::setTiming(v == 1 ? vcs::Timing::Independent : vcs::Timing::Original); };
        settings::add(s);

        s = {};
        s.key = "display.fps";
        s.page = "Display";
        s.section = "Frame rate";
        s.label = "Frame rate cap";
        s.help = "60 is the default, 30 the original. With original timing anything above 30 runs at 60 (the VBlank "
                 "allows no more); "
                 "with frame-rate independent timing the game runs up to the cap - at most 240 (above that the game's "
                 "shortest time step makes it run slightly fast).";
        s.type = Type::Choice;
        s.choices = {{"30 (original)", 30},
                     {"60", 60},
                     {"100", 100},
                     {"120", 120},
                     {"144", 144},
                     {"165", 165},
                     {"240", 240},
                     {"Display refresh (up to 240)", vcs::kCapRefresh},
                     {"Unlimited (up to 240)", vcs::kCapUnlimited}};
        s.defaultValue = 60;
        s.env = "VCS_FPS";
        s.onChange = [](int v) { vcs::setFrameCap(v); };
        settings::add(s);

        s = {};
        s.key = "display.vsync";
        s.page = "Display";
        s.section = "Frame rate";
        s.label = "Vertical sync";
        s.help = "Off: each frame is shown as soon as it is ready, never torn. On: frames wait for the monitor's refresh "
                 "(smoothest with a cap of Display refresh, at a little more input delay). Off, allow tearing: the lowest "
                 "delay, and for G-Sync / FreeSync monitors (with a cap a few fps below the refresh rate).";
        s.type = Type::Choice;
        s.choices = {{"Off", 0}, {"On", 1}, {"Off, allow tearing (VRR)", 2}};
        s.defaultValue = 0;
        s.env = "VCS_VSYNC";
        s.onChange = [](int v) { present::setPresentMode(v); };
        settings::add(s);

        s = {};
        s.key = "display.fps_counter";
        s.page = "Display";
        s.section = "Frame rate";
        s.label = "Show frame rate";
        s.help = "The game's frames per second in a corner of the screen (F3 shows the full performance overlay).";
        s.type = Type::Choice;
        s.choices = {{"Off", 0}, {"Top left", 1}, {"Top right", 2}, {"Bottom left", 3}, {"Bottom right", 4}};
        s.defaultValue = 0;
        settings::add(s);

        s = {};
        s.key = "display.skip_intro";
        s.page = "Game";
        s.section = "Start-up";
        s.label = "Skip intro movies";
        s.help = "The Rockstar logos and the intro movie at start-up. Off plays them as on the PS2 (Start or Cross skips the "
                 "intro, not the logos); on goes straight to the game.";
        s.type = Type::Toggle;
        s.defaultValue = 0;
        s.env = "VCS_SKIP_MOVIES";
        s.onChange = [](int v) { vcs::setSkipIntroMovies(v != 0); };
        settings::add(s);

        s = {};
        s.key = "game.background";
        s.page = "Game";
        s.section = "Window";
        s.label = "In the background";
        s.help = "When another window is in front (Alt+Tab): the game keeps running and playing sound as on a console, runs "
                 "silently, or pauses until you come back.";
        s.type = Type::Choice;
        s.choices = {{"Keep running", 0}, {"Mute", 1}, {"Pause", 2}};
        s.defaultValue = 0;
        settings::add(s);

        s = {};
        s.key = "display.msaa";
        s.page = "Graphics";
        s.section = "Image quality";
        s.label = "Anti-aliasing (MSAA)";
        s.help = "Smooths the edges of the 3D picture. Off is the original. Costs GPU time.";
        s.type = Type::Choice;
        s.choices = {{"Off (original)", 1}, {"2x", 2}, {"4x", 4}, {"8x", 8}};
        s.defaultValue = 1;
        s.apply = Apply::Restart;
        s.env = "PS2X_HW_MSAA";
        s.onChange = [](int v) { setEnv("PS2X_HW_MSAA", std::to_string(v)); };
        settings::add(s);

        s = {};
        s.key = "display.aniso";
        s.page = "Graphics";
        s.section = "Image quality";
        s.label = "Texture filtering";
        s.help = "Anisotropic filtering keeps textures sharp at a slant (roads, walls into the distance). Off is the "
                 "original bilinear filtering.";
        s.type = Type::Choice;
        s.choices = {{"Bilinear (original)", 0}, {"Anisotropic 2x", 2}, {"Anisotropic 4x", 4}, {"Anisotropic 8x", 8},
                     {"Anisotropic 16x", 16}};
        s.defaultValue = 0;
        s.apply = Apply::Restart;
        s.env = "PS2X_HW_ANISO";
        s.onChange = [](int v) { setEnv("PS2X_HW_ANISO", std::to_string(v)); };
        settings::add(s);

        // Post-processing (native equivalent of shader/enhancement mods such as ProperShaders):
        // opt-in, authentic default (off). Applied by the presenter as a full-screen pass.
        s = {};
        s.key = "graphics.post_process";
        s.page = "Graphics";
        s.section = "Image quality";
        s.label = "Post-processing";
        s.help = "Exposure / colour tweaks applied to the final picture (native enhancement, not the PS2 look). Off is authentic.";
        s.type = Type::Toggle;
        s.defaultValue = 0;
        s.env = "VCS_POST_PROCESS";
        s.onChange = [](int v) {
            present::setPostProcessEnabled(v != 0);
            // when the toggle is switched on and exposure is still at its default, give a
            // visible nudge so the effect is immediately apparent; switching off restores it
            if (v != 0 && settings::value("graphics.exposure") == 100)
                settings::set("graphics.exposure", 120);
            else if (v == 0)
                settings::set("graphics.exposure", 100);
        };
        settings::add(s);

        s = {};
        s.key = "graphics.exposure";
        s.page = "Graphics";
        s.section = "Image quality";
        s.label = "Exposure";
        s.help = "Brightness of the final picture, percent (100 = original). Needs Post-processing on.";
        s.type = Type::Integer;
        s.min = 0;
        s.max = 200;
        s.defaultValue = 100;
        s.format = "%d%%";
        s.env = "VCS_EXPOSURE";
        s.onChange = [](int v) { present::setExposurePercent(v); };
        settings::add(s);

        vcs::fog::registerSettings(); // the game's own distance fog: distance / start (game/vcs_fog.h)
        vcs::distance::registerSettings(); // the objects' draw distance (game/vcs_distance.h)
        vcs::distant::registerSettings();  // decorative distant traffic (game/vcs_distant.h)
        vcs::memory::registerSettings(); // the streaming heap's size (game/vcs_memory.h)

        // The graphics API, when the build has more than one (Windows with VCS_RHI_VULKAN); VCS_RHI=d3d11|vulkan|null
        // chooses for one run (render/presenter.h).
        s = {};
        s.key = "display.renderer";
        s.page = "Graphics";
        s.section = "Renderer";
        s.label = "Graphics API";
        s.help = "The graphics API the game draws with. D3D11 is the reference on Windows; Vulkan is the one on Linux.";
        s.type = Type::Choice;
        for (rhi::Backend b : {rhi::Backend::D3D11, rhi::Backend::Vulkan})
            if (rhi::backendBuilt(b))
                s.choices.push_back({rhi::backendName(b), static_cast<int>(b)});
        s.defaultValue = static_cast<int>(rhi::defaultBackend());
        s.apply = Apply::Restart;
        s.onChange = [](int v) { present::setBackend(v); };
        if (s.choices.size() > 1)
            settings::add(s);

        vcs::input::registerSettings(); // the Keyboard & mouse and Gamepad pages (game/vcs_input.h)
        vcs::mods::registerSettings(exeDirectory()); // the Mods page: modloader/ and texture packs (mods/modloader.h)

        s = {};
        s.key = "graphics.perf_overlay";
        s.page = "Advanced";
        s.section = "Diagnostics";
        s.label = "Performance overlay";
        s.help = "Frame times and counters in the corner (F3 toggles it too; F4 records a CSV).";
        s.type = Type::Toggle;
        s.env = "VCS_PERF_OVERLAY";
        s.onChange = [](int v) { ps2perf::setOverlay(v != 0); };
        s.read = [] { return ps2perf::overlayOn() ? 1 : 0; };
        settings::add(s);

        s = {};
        s.key = "advanced.reset_all";
        s.page = "Advanced";
        s.section = "Reset";
        s.label = "Reset all settings";
        s.help = "Every setting on every page back to its default (the PS2's picture at 60 fps with frame-rate independent "
                 "timing, the default controls). "
                 "The disc path and the window's place are kept.";
        s.type = Type::Action;
        s.button = "Reset everything";
        s.confirm = true;
        s.onChange = [](int) {
            for (const settings::Spec &each : settings::all())
                if (each.type != Type::Action)
                    settings::set(each.key, each.defaultValue);
            std::fprintf(stderr, "[settings] all reset to defaults\n");
        };
        settings::add(s);
    }

    // The window opens where it was last closed (monitor, position, size, maximized): window.* in vcs.ini, written
    // by the platform layer's callback whenever the window settles somewhere new. Deleting those lines resets it.
    void restoreWindowPlacement()
    {
        platform::WindowPlacement p;
        p.display = settings::state("window.display");
        p.displayX = std::atoi(settings::state("window.display_x").c_str());
        p.displayY = std::atoi(settings::state("window.display_y").c_str());
        p.x = std::atoi(settings::state("window.x").c_str());
        p.y = std::atoi(settings::state("window.y").c_str());
        p.width = std::atoi(settings::state("window.width").c_str());
        p.height = std::atoi(settings::state("window.height").c_str());
        p.maximized = settings::state("window.maximized") == "1";
        if (!p.display.empty() && p.width > 0 && p.height > 0)
            platform::setStartPlacement(p);
        platform::onPlacementChanged([](const platform::WindowPlacement &now) {
            settings::setState({
                {"window.display", now.display},
                {"window.display_x", std::to_string(now.displayX)},
                {"window.display_y", std::to_string(now.displayY)},
                {"window.x", std::to_string(now.x)},
                {"window.y", std::to_string(now.y)},
                {"window.width", std::to_string(now.width)},
                {"window.height", std::to_string(now.height)},
                {"window.maximized", now.maximized ? "1" : "0"},
            });
        });
    }

    fs::path firstIsoIn(const fs::path &dir)
    {
        std::error_code ec;
        for (const auto &entry : fs::directory_iterator(dir, ec))
        {
            std::string ext = entry.path().extension().string();
            for (char &c : ext)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (entry.is_regular_file() && ext == ".iso")
                return entry.path();
        }
        return {};
    }

    fs::path locateIso(int argc, char *argv[])
    {
        if (argc >= 2 && argv[1] && argv[1][0])
            return fs::u8path(argv[1]);
        if (fs::path p = isoFromIni(exeDirectory() / "vcs.ini"); !p.empty())
            return p;
        if (fs::path p = firstIsoIn(exeDirectory()); !p.empty())
            return p;
        return firstIsoIn(fs::current_path());
    }

    // Writes the guest ELF from the disc into the cache folder (runtime loads it from a file). 0, or vcs.exe's exit
    // code for the failure (vcs_disc_id.h) with `error` set.
    int stageElf(const vcs::IsoImage &iso, const fs::path &out, std::string &error)
    {
        vcs::IsoEntry entry;
        if (!iso.find(kElfName, entry))
        {
            error = std::string(kElfName) + " not found: this is not GTA: Vice City Stories (USA)";
            return vcs::disc::kExitWrongDisc;
        }
        std::vector<uint8_t> elf(entry.size);
        iso.readBytes(uint64_t(entry.lba) * vcs::IsoImage::kSectorSize, elf.size(), elf.data());
        const std::string sha1 = vcs::sha1Hex(elf.data(), elf.size());
        if (sha1 != kElfSha1)
        {
            error = "unsupported game build (executable sha1 " + sha1 + ")";
            return vcs::disc::kExitWrongDisc;
        }
        std::error_code ec;
        if (fs::exists(out, ec) && fs::file_size(out, ec) == elf.size())
            return 0;
        fs::create_directories(out.parent_path(), ec);
        std::ofstream file(out, std::ios::binary);
        file.write(reinterpret_cast<const char *>(elf.data()), static_cast<std::streamsize>(elf.size()));
        if (!file.good())
        {
            error = "cannot write " + out.string();
            return vcs::disc::kExitError;
        }
        return 0;
    }
}

int main(int argc, char *argv[])
{
    if (const int code = vcs::rwconv::commandLine(argc, argv); code >= 0) // --rwconv-dff / --rwconv-txd: no game
        return code;
    vcs::installCrashHandlers();
    try
    {
        // the hardware (D3D11) renderer unless PS2X_GS says otherwise: the runtime's own default is the software GS
        // (~5 fps), which only tools/run.py and the test tools used to override - vcs.exe started on its own got it
        if (const char *gs = std::getenv("PS2X_GS"); !gs || !*gs)
            setEnv("PS2X_GS", "hw");
        registerSettings();
        settings::load(exeDirectory() / "vcs.ini");
        if (ps2control::hiddenMode())
            settings::keepFile(); // a test run never changes the player's settings
        settings::applyAll();
        restoreWindowPlacement();
        const fs::path isoPath = locateIso(argc, argv);
        if (isoPath.empty())
        {
            std::cerr << "No disc image found. Pass your Vice City Stories (USA) .iso as the first argument,\n"
                         "put iso=<path> in vcs.ini next to vcs.exe, or place the .iso next to vcs.exe."
                      << std::endl;
            return vcs::disc::kExitNoDisc;
        }

        vcs::IsoImage iso;
        if (!iso.open(isoPath))
        {
            std::cerr << "Cannot read disc image " << isoPath.string() << ": " << iso.error() << std::endl;
            return vcs::disc::kExitDiscUnreadable;
        }

        const fs::path data = dataDirectory();
        const fs::path elfPath = data / "cache" / kElfName;
        std::string error;
        if (const int code = stageElf(iso, elfPath, error))
        {
            std::cerr << error << std::endl;
            return code;
        }

        MappedDisc disc(iso);
        ps2_disc::setImage(&disc);

        // the trainer's mission list names each mission from MAIN.SCM (game/vcs_trainer.h)
        if (vcs::IsoEntry scm; iso.find("MAIN.SCM", scm) && !scm.isDirectory)
        {
            std::vector<uint8_t> bytes((scm.size + vcs::IsoImage::kSectorSize - 1) / vcs::IsoImage::kSectorSize *
                                       vcs::IsoImage::kSectorSize);
            if (iso.readSectors(scm.lba, static_cast<uint32_t>(bytes.size() / vcs::IsoImage::kSectorSize), bytes.data()))
            {
                bytes.resize(scm.size);
                vcs::trainer::setMissionScript(bytes);
            }
        }

        // the card in our data folder from the start: the runtime's default (the working directory) left empty mc0 / mc1
        // folders next to vcs.exe, made before loadELF below set the paths
        {
            PS2Runtime::IoPaths early = PS2Runtime::getIoPaths();
            early.mcRoot = data / "mc0";
            PS2Runtime::setIoPaths(early);
        }

        PS2Runtime runtime;
        // the settings menu draws in the runtime's UI hook (registered before initialize(), which runs the init hook)
        runtime.setDebugUiCallbacks([](PS2Runtime &, void *) {},
                                    [](PS2Runtime &, void *) {
                                        altEnter();
                                        screenshotKey();
                                        followWindowAspect();
                                        ui::drawSettingsMenu();
                                        ui::drawTrainerMenu();
                                        ps2control::setGameInputBlocked(ui::settingsMenuBlocksInput() || ui::trainerMenuBlocksInput());
                                        vcs::input::onHostFrame(ui::settingsMenuOpen() || ui::trainerMenuBlocksInput());
                                        vcs::volume::onHostFrame();
                                        vcs::mods::onHostFrame(); // mod / texture switches, live (mods/modloader.h)
                                        drawFrameRateCounter();
                                    },
                                    nullptr, nullptr);
        if (!runtime.initialize("GTA: Vice City Stories"))
        {
            std::cerr << "Failed to initialize runtime" << std::endl;
            return vcs::disc::kExitInitFailed;
        }
        if (!runtime.loadELF(elfPath.string()))
        {
            std::cerr << "Failed to load " << elfPath.string() << std::endl;
            return 1;
        }

        // loadELF points every root at the cache folder; keep saves in our data folder
        // and route cdrom0: through the mapped disc (cdImage is used by the IOP fallback).
        PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
        paths.cdImage = isoPath;
        paths.mcRoot = data / "mc0";
        PS2Runtime::setIoPaths(paths);

        vcs::rig::install(runtime); // control-channel automation commands (debug/rig.h)
        vcs::trainer::install(runtime); // the trainer's control-channel command (game/vcs_trainer.h)
        vcs::input::install(runtime);   // the player's bindings feed the pad (game/vcs_input.h)
        vcs::volume::install(runtime);  // the game's music / effects levels follow the settings (game/vcs_volume.h)
        vcs::fog::install(runtime);     // the fog settings scale the game's fog range (game/vcs_fog.h)
        vcs::memory::install(runtime);  // the streaming heap in the upper 32 MB when asked (game/vcs_memory.h)
        vcs::world::install(runtime);   // the world's area streaming (game/vcs_world.h; VCS_TRACE_AREAS=1)
        vcs::gameinfo::install(runtime); // the F3 overlay's game-state panel, the `gameinfo` command (debug/game_info.h)
        // setting <key> [value] -> ok <value>: a setting read or changed (a hidden run never writes vcs.ini)
        ps2control::addCommand("setting", [](const std::string &args, ps2control::Reply reply) {
            const size_t space = args.find(' ');
            const std::string key = args.substr(0, space);
            const auto spec = std::find_if(settings::all().begin(), settings::all().end(), [&](const settings::Spec &s) { return s.key == key; });
            if (spec == settings::all().end())
                return reply("error unknown setting " + key);
            if (spec->type == settings::Type::Action) // a button: pressed (presets, reset)
            {
                spec->onChange(0);
                return reply("ok pressed");
            }
            if (space != std::string::npos && !settings::set(key, std::atoi(args.c_str() + space + 1)))
                return reply("error refused");
            reply("ok " + std::to_string(settings::value(key)));
        });
        // menu <page>|close|scroll <0..1> -> ok: the settings menu opened on a page (screenshots of it in hidden runs)
        ps2control::addCommand("menu", [](const std::string &args, ps2control::Reply reply) {
            if (args == "close")
                ui::closeSettingsMenu();
            else if (args.rfind("scroll ", 0) == 0)
                ui::scrollSettingsMenu(static_cast<float>(std::atof(args.c_str() + 7)));
            else
                ui::openSettingsMenu(args);
            reply("ok");
        });
        vcs::configureWatch(runtime.memory().getRDRAM());
        vcs::runSelfTest(runtime);
        vcs::startWatchdog(runtime);
        vcs::startProfiler(runtime);
        runtime.run();
#if defined(PS2X_PGO_GEN)
        __llvm_profile_write_file(); // instrumented build: _Exit skips the atexit handler that writes the profile
#endif
        std::cout.flush();
        std::cerr.flush();
        std::_Exit(0);
    }
    catch (const std::exception &e)
    {
        std::cerr << "[main] fatal exception: " << e.what() << std::endl;
    }
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(1);
}
