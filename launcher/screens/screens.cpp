// The window's frame: which mode is shown and the transitions between them, the readings of the model the screens
// share, and the actions several screens start.
#include "shell.h"

#include "model/os.h"
#include "model/system.h"
#include "platform/platform.h"

#include <cmath>
#include <ctime>

namespace fs = std::filesystem;

namespace screens
{
    Shell shell;

    namespace
    {
        int g_forcedMode = -1; // --screen: a mode shown whatever the model says
        float g_hero = 0;
        bool g_checkedUpdates = false;
        bool g_wasPlaying = false;
        float g_dark = -1; // the palette, 1 dark, 0 light: fades across when it changes

        bool hasWarnings(const App &app) { return app.doctor["folder"]["warnings"].size() > 0; }

        void demoTask(App &app, const std::string &demo)
        {
            if (demo == "playing")
            {
                app.gameRunning = true;
                app.gameStart = ui::state.wall - 754;
                return;
            }
            if (demo == "update")
            {
                app.updateInfo = launcher::Json::parse(R"({"newer": true, "latest": "0.0.4", "url": "https://github.com"})");
                return;
            }
            if (demo == "updated") // an update put in place: Restart / Later
            {
                app.task = std::make_unique<launcher::Task>();
                launcher::Task &t = *app.task;
                const double now = ui::state.wall;
                t.kind = "update";
                t.title = "Updating";
                t.stages = {{"download", "Downloading the new release", now - 40, now - 6},
                            {"apply", "Putting the new release in place", now - 6, now}};
                t.startTime = now - 40;
                t.endTime = now;
                t.finished = true;
                t.result = launcher::Json::parse(R"({"result": "ok", "version": "0.2.1", "restart": true})");
                app.updatedTo = "0.2.1";
                return;
            }
            if (demo == "stale")
                return; // Home reads it: the build out of date
            if (demo == "gameerror")
            {
                app.gameEnded = true;
                app.gameExit = int(0xC0000005u);
                return;
            }
            if (demo == "buildconfirm")
            {
                shell.buildConfirmOpen = true;
                shell.stagedBuildDir = app.build;
                return;
            }
            if (demo == "c64build")
            {
                shell.c64TerminalOpen = true;
            }
            if (demo == "scskiller")
            {
                shell.c64TerminalOpen = true;
                app.task = std::make_unique<launcher::Task>();
                launcher::Task &st = *app.task;
                st.kind = "shader_cache";
                st.title = "Construindo Cache de Shaders (SCSKiller)";
                const double now = ui::state.wall;
                st.startTime = now - 1.25;
                st.done = 1134;
                st.total = 1134;
                st.note = "PSOs";
                st.finished = true;
                st.exitCode = 0;
                st.endTime = now;
                st.log = {
                    "[SCSKiller] ================================================================",
                    "[SCSKiller] VCS.rec Shader Compilation Stutter Killer (Pipeline Precompiler)",
                    "[SCSKiller] Based on BlueHeisenberg/SCSKiller PSO & Driver Cache architecture",
                    "[SCSKiller] ================================================================",
                    "[SCSKiller] GPU Adapter : AMD Radeon RX 7700 XT",
                    "[SCSKiller] Hardware ID : Vendor 0x1002 | Device 0x747E | VRAM 12080 MB",
                    "[SCSKiller] [D3D12] Root Signature reconstructed (4 CBVs, 8 SRVs, 8 Samplers)",
                    "[SCSKiller] [D3D12] Queued 1134 Pipeline State Objects across 16 worker threads...",
                    "[SCSKiller] [D3D12] Pre-compiled  350 / 1134 PSOs  (Native VCS World)",
                    "[SCSKiller] [D3D12] Pre-compiled  770 / 1134 PSOs  (Native Distant Instanced)",
                    "[SCSKiller] [D3D12] Pre-compiled 1134 / 1134 PSOs  (GS ShaderDepth Fallback)",
                    "[SCSKiller] [D3D11] Pre-compiled 40 / 40 DXBC Shaders into GPU driver cache",
                    "[SCSKiller] Cache File  : build/shaders/scskiller_pso_cache.bin (17881 KB)",
                    "[SCSKiller] Summary     : 1134 D3D12 PSOs + 40 D3D11 Shaders in 1249 ms",
                    "[SCSKiller] Driver Cache: Populated (0ms runtime shader stutter ready!)",
                };
                return;
            }
            if (demo == "en")
            {
                app.settings.language = "en";
                app.settings.languageChosen = true;
                return;
            }
            if (demo == "hoverplay")
                return;
            app.task = std::make_unique<launcher::Task>();
            launcher::Task &t = *app.task;
            t.kind = "build";
            t.title = "Building the game";
            const double now = ui::state.wall;
            t.stages = {{"disc", "Extracting the executable from your disc", now - 900, now - 899},
                        {"recompiler", "Building the recompiler", now - 899, now - 860},
                        {"translate", "Translating MIPS to C++", now - 860, now - 770},
                        {"vu1", "Compiling VU1 microcode", now - 770, now - 766},
                        {"compile", "Compiling the game", now - 766, 0}};
            t.startTime = now - 900;
            t.done = 1180;
            t.total = 3400;
            t.eta = 1420;
            t.note = "compiling";
            for (int i = 0; i < 60; ++i)
                t.log.push_back("[build] [" + std::to_string(400 + i * 13) + "/3400] Building CXX object CMakeFiles/ps2EntryRunner.dir/generated/ps2_recomp_" +
                                std::to_string(i) + ".cpp.obj");
            if (demo == "link" || demo == "firstlink") // the ThinLTO link: timed against the last one, or not at all
            {
                t.stages.back().end = now - 62;
                t.stages.push_back({"link", "Linking and optimising the game (ThinLTO)", now - 62, 0});
                t.done = demo == "link" ? 62 : 0;
                t.total = demo == "link" ? 133 : 0;
                t.eta = demo == "link" ? 71 : -1;
                t.note = "linking";
            }
            if (demo == "error")
            {
                t.finished = true;
                t.exitCode = 1;
                t.endTime = now;
                t.stages.back().end = now;
                t.errorCode = "disk_full";
                t.errorMessage = "building ps2EntryRunner failed";
                t.errorLog = "build/build.log";
                t.log.push_back("LLVM ERROR: IO failure on output stream: no space on device");
            }
            if (demo == "done")
            {
                t.finished = true;
                t.exitCode = 0;
                t.endTime = now;
                t.stages.back().end = now;
                t.done = t.total;
            }
        }

        Mode decideMode(const App &app)
        {
            if (g_forcedMode >= 0)
                return Mode(g_forcedMode);
            if (app.task && takesOver(*app.task))
                return Mode::Task;
            if (shell.setupOpen)
                return Mode::Setup;
            return Mode::Home;
        }

        void drawMode(Mode mode, Rect content)
        {
            switch (mode)
            {
            case Mode::Setup: drawSetup(content); break;
            case Mode::Home:
                switch (shell.tab)
                {
                case Tab::Game: drawHome(content); break;
                case Tab::Mods: drawMods(content); break;
                case Tab::Controls: drawControls(content); break;
                case Tab::Settings: drawConfig(content); break;
                case Tab::About: drawAbout(content); break;
                }
                break;
            case Mode::Task: drawTask(content); break;
            }
        }

        void keyboard(App &app)
        {
            const platform::Input &in = platform::input;
            if (in.key(platform::kKeyEscape))
            {
                if (shell.dialog != Dialog::None)
                    shell.dialog = Dialog::None;
                else if (shell.sheet != Sheet::None)
                    shell.sheet = Sheet::None;
                else if (shell.setupOpen && shell.mode == Mode::Setup)
                    shell.setupOpen = false;
            }
            if (in.key(platform::kKeyF1))
                openSheet(shell.sheet == Sheet::Help ? Sheet::None : Sheet::Help);
            if (in.ctrl && in.key('L') && app.task)
                shell.showLog = !shell.showLog;
            if (in.key(platform::kKeyF12) && in.ctrl && in.shift)
                g_forcedMode = -1;
        }
    }

    // ---- readings ------------------------------------------------------------------------------------------------

    bool cpuUnsupported(const App &app)
    {
        const launcher::Json &cpu = app.doctor["cpu"];
        return cpu.isObject() && !cpu["ok"].boolean();
    }

    namespace
    {
        // while the cached answer shows, only "fine" is believed; anything else is still being checked
        ui::Status believed(const App &app, ui::Status s)
        {
            return app.doctorCached && s != ui::Status::Good ? ui::Status::Checking : s;
        }
    }

    ui::Status folderStatus(const App &app)
    {
        if (!app.doctorDone)
            return ui::Status::Checking;
        if (app.doctorFailed())
            return ui::Status::Warning;
        return believed(app, hasWarnings(app) ? ui::Status::Warning : ui::Status::Good);
    }

    ui::Status discStatus(const App &app)
    {
        if (app.discChecked && app.disc.ok)
            return ui::Status::Good;
        return app.discChecked ? ui::Status::Failed : ui::Status::Missing;
    }

    ui::Status toolsStatus(const App &app)
    {
        if (!app.doctorDone)
            return ui::Status::Checking;
        if (app.doctorFailed())
            return ui::Status::Warning;
        const launcher::Json &d = app.doctor;
        ui::Status s = ui::Status::Good;
        if (cpuUnsupported(app))
            s = ui::Status::Failed;
        else if (!d["compiler"]["ok"].boolean() || !d["downloads"]["ok"].boolean())
            s = d["build"]["current"].boolean() ? ui::Status::Warning : ui::Status::Missing;
        else if (!d["memory"]["ok"].boolean())
            s = ui::Status::Warning;
        if (app.busy() && app.task->kind == "tools")
            s = ui::Status::Running;
        return believed(app, s);
    }

    ui::Status gameStatus(const App &app)
    {
        if (app.busy() && app.task->kind == "build")
            return ui::Status::Running;
        if (!app.doctorDone)
            return ui::Status::Checking;
        if (app.doctorFailed())
            return ui::Status::Warning;
        const launcher::Json &b = app.doctor["build"];
        return believed(app, b["current"].boolean() ? ui::Status::Good : b["built"].boolean() ? ui::Status::Warning : ui::Status::Missing);
    }

    ui::Status stepStatus(const App &app, int step)
    {
        switch (step)
        {
        case kStepFolder: return folderStatus(app);
        case kStepDisc: return discStatus(app);
        case kStepTools: return toolsStatus(app);
        default: return gameStatus(app);
        }
    }

    std::string folderLine(const App &app)
    {
        if (!app.doctorDone || app.doctorCached)
            return "Checking";
        if (app.doctorFailed())
            return "Couldn't check";
        const size_t n = app.doctor["folder"]["warnings"].size();
        if (n)
            return n == 1 ? "1 thing to look at" : std::to_string(n) + " things to look at";
        return gigabytes(app.doctor["folder"]["free_bytes"].num()) + " free";
    }

    std::string discLine(const App &app)
    {
        if (!app.discChecked)
            return "Not chosen yet";
        if (app.disc.ok)
            return "USA, verified";
        return app.disc.code == "not_found" ? "File not found" : app.disc.code == "not_iso" ? "Not an .iso" : "Not the supported disc";
    }

    std::string toolsLine(const App &app)
    {
        if (app.busy() && app.task->kind == "tools")
            return "Installing";
        if (!app.doctorDone || app.doctorCached)
            return "Checking";
        if (app.doctorFailed())
            return "Couldn't check";
        if (cpuUnsupported(app))
            return "Processor too old";
        if (!app.doctor["compiler"]["ok"].boolean())
            return "Needs installing";
        if (!app.doctor["downloads"]["ok"].boolean())
            return "Git missing";
        return "Ready";
    }

    std::string gameLine(const App &app)
    {
        if (app.busy() && app.task->kind == "build")
            return "Building";
        if (!app.doctorDone || app.doctorCached)
            return "Checking";
        if (app.doctorFailed())
            return "Couldn't check";
        const launcher::Json &b = app.doctor["build"];
        if (b["current"].boolean())
            return "Built, up to date";
        return b["built"].boolean() ? "Needs an update" : "Not built yet";
    }

    std::string stepLine(const App &app, int step)
    {
        switch (step)
        {
        case kStepFolder: return folderLine(app);
        case kStepDisc: return discLine(app);
        case kStepTools: return toolsLine(app);
        default: return gameLine(app);
        }
    }

    const char *stepName(int step)
    {
        static const char *names[] = {"Folder", "Disc", "Build tools", "Game"};
        return names[std::clamp(step, 0, 3)];
    }

    bool takesOver(const launcher::Task &t)
    {
        // Build, Auto-Update, and SCSKiller Shader Cache tasks run directly inside the Commodore 64 Terminal on Home
        if (t.kind == "build" || t.kind == "auto_update" || t.kind == "shader_cache")
            return false;
        return t.kind == "tools" || t.kind == "update" || t.kind == "rip" || t.kind == "report" ||
               t.kind == "restore" || t.kind == "install" || (t.finished && !t.ok() && !t.cancelled);
    }

    std::string duration(double seconds)
    {
        const int s = std::max(0, int(seconds));
        char text[32];
        if (s >= 3600)
            std::snprintf(text, sizeof(text), "%dh %02dm", s / 3600, s / 60 % 60);
        else if (s >= 60)
            std::snprintf(text, sizeof(text), "%dm %02ds", s / 60, s % 60);
        else
            std::snprintf(text, sizeof(text), "%ds", s);
        return text;
    }

    std::string gigabytes(double bytes)
    {
        char text[32];
        const double gb = bytes / double(1ull << 30);
        std::snprintf(text, sizeof(text), gb < 10 ? "%.1f GB" : "%.0f GB", gb);
        return text;
    }

    std::string buildSummary(const App &app)
    {
        const launcher::Json &info = app.doctor["build"]["info"];
        std::string opt = info["opt"].str();
        opt = opt.substr(0, opt.find(" - "));
        const std::string kind = opt == "lto" || opt.rfind("ThinLTO", 0) == 0 ? "Standard" : opt == "none" ? "Quick build"
                                 : opt == "pgo" || opt.find("PGO") != std::string::npos ? "Fastest" : opt;
        std::string out;
        if (const double t = info["time"].num(); t > 0)
        {
            const double ago = double(std::time(nullptr)) - t;
            const int days = int(ago / 86400);
            out = ago < 120 ? "Built just now" : ago < 3600 ? "Built " + std::to_string(int(ago / 60)) + " minutes ago"
                  : ago < 86400 ? "Built " + std::to_string(int(ago / 3600)) + (ago < 7200 ? " hour ago" : " hours ago")
                  : days == 1 ? "Built yesterday" : "Built " + std::to_string(days) + " days ago";
        }
        if (!kind.empty())
            out += (out.empty() ? "" : "  \xC2\xB7  ") + kind;
        if (const std::string cpu = info["cpu"].str(); !cpu.empty())
            out += "  \xC2\xB7  " + cpu;
        return out;
    }

    // the launcher's own setting, off until the player turns it on. Not Windows' "Animate controls and elements inside
    // windows": that one is off on many PCs tuned for speed (this one too), and new installs started without motion
    bool reduceMotion(const App &app) { return app.settings.reduceMotion; }

    bool darkTheme(const App &app)
    {
        if (app.settings.theme == "light" || app.settings.theme == "dark")
            return app.settings.theme == "dark";
        return platform::window.darkMode;
    }

    bool canPlay(const App &app) { return app.next() == "play" && !app.doctorCached && !app.gameRunning; }

    // ---- actions -------------------------------------------------------------------------------------------------

    void chooseIso()
    {
        shell.pending = [] {
            fs::path picked;
            if (platform::openFileDialog(picked, "Choose your GTA: Vice City Stories disc image",
                                         {{"PS2 disc image (*.iso)", "*.iso"}, {"All files", "*"}}))
            {
                shell.app->chooseDisc(picked);
                if (!shell.app->disc.ok)
                    openSetup(kStepDisc);
            }
        };
    }

    void chooseBuildDir(const fs::path &start)
    {
        shell.pending = [start] {
            fs::path picked;
            if (platform::openFolderDialog(picked, tr("Escolha a pasta de compilação do jogo", "Choose where the game is built"), start))
            {
                shell.app->startBuildDirCheck(picked);
                shell.awaitingBuildDir = true;
            }
        };
    }

    void chooseInstallDirForBuild()
    {
        const fs::path start = shell.stagedBuildDir.empty() ? shell.app->build : shell.stagedBuildDir;
        shell.pending = [start] {
            fs::path picked;
            if (platform::openFolderDialog(picked,
                                           tr("Escolha a pasta de instalação final do jogo",
                                              "Choose the final installation folder for the game"),
                                           start))
            {
                shell.stagedBuildDir = picked;
                shell.buildConfirmOpen = true;
                shell.c64TerminalOpen = false;
            }
            else if (shell.stagedBuildDir.empty())
            {
                shell.stagedBuildDir = shell.app->build;
                shell.buildConfirmOpen = true;
                shell.c64TerminalOpen = false;
            }
        };
    }

    void chooseRipTarget()
    {
        shell.pending = [] {
            fs::path picked;
            const fs::path suggested = launcher::knownFolder(launcher::Folder::Documents) / "Grand Theft Auto - Vice City Stories (USA).iso";
            if (platform::saveFileDialog(picked, "Save the disc image as", suggested, {{"Disc image (*.iso)", "*.iso"}}, "iso"))
                shell.app->startRip(shell.ripDrive, picked);
        };
    }

    void play(App &app)
    {
        app.dismissTask();
        std::string error;
        if (!app.play(error))
        {
            toast(error, theme::kRed);
            return;
        }
        shell.gameErrorDismissed = false;
        if (shell.options.screenshot)
            return;
        if (app.settings.afterLaunch == "close")
            shell.quitNow = true;
        else if (app.settings.afterLaunch == "minimize")
        {
            platform::minimize();
            shell.minimizedForGame = true;
        }
    }

    void restartLauncher(App &app)
    {
        if (shell.options.screenshot)
            return;
        std::string error;
        if (!app.restart(error))
        {
            toast("Could not start the new launcher: " + error + ". Close this one and start it again.", theme::kRed);
            return;
        }
        app.dismissTask();
        shell.quitNow = true; // the new one waits for this one to end
    }

    void openSheet(Sheet sheet)
    {
        shell.sheet = sheet;
        if (sheet == Sheet::Saves)
            shell.backupsAsked = false; // the list again
    }

    void openDialog(Dialog dialog) { shell.dialog = dialog; }

    void openSetup(int step)
    {
        shell.setupOpen = true;
        shell.pinnedStep = step;
        shell.sheet = Sheet::None;
    }

    void toast(const std::string &text, Color color)
    {
        shell.toasts.push_back({text, ui::state.wall, color ? color : theme::kTeal});
        if (shell.toasts.size() > 3)
            shell.toasts.erase(shell.toasts.begin());
    }

    // ---- layout helpers ------------------------------------------------------------------------------------------

    float reveal(double since, int index)
    {
        if (ui::state.motion <= 0)
            return 1;
        const float t = float((ui::state.time - since - index * 0.04) / 0.45);
        if (t < 1)
            ui::keepAnimating();
        return core::easeOutCubic(t);
    }

    void pushReveal(double since, int index, float distance)
    {
        const float e = reveal(since, index);
        render::pushOpacity(e);
        render::pushOffset(0, std::round((1 - e) * px(distance)));
    }

    void popReveal()
    {
        render::popOffset();
        render::popOpacity();
    }

    float header(Rect r, std::string_view eyebrowText, Color eyebrowColor, std::string_view title, std::string_view description)
    {
        float y = r.y;
        if (!eyebrowText.empty())
        {
            ui::eyebrow(r.x, y, eyebrowColor, eyebrowText);
            y += px(22);
        }
        const float ts = px(theme::kTitle);
        const render::FontMetrics m = render::metrics(render::kFontSemibold, ts);
        render::drawText(render::kFontSemibold, ts, r.x - px(1), y + m.ascent, theme::kText, title);
        y += m.ascent + m.descent + px(8);
        if (!description.empty())
            y += ui::paragraph(render::kFontBody, px(theme::kBody), {r.x, y, std::min(r.w, px(620)), 0}, theme::kDim, description) + px(4);
        return y + px(18);
    }

    float row(Rect r, std::string_view name, std::string_view value, Color valueColor)
    {
        const float nameW = px(130);
        ui::label(render::kFontBody, px(theme::kSmall), {r.x, r.y, nameW, px(24)}, theme::kFaint, name);
        const float h = std::max(px(24), ui::paragraphHeight(render::kFontBody, px(theme::kBody), r.w - nameW, value, 1.4f));
        ui::paragraph(render::kFontBody, px(theme::kBody), {r.x + nameW, r.y + px(1), r.w - nameW, 0},
                      valueColor ? valueColor : theme::kText, value, 1.4f);
        return h + px(6);
    }

    float windowScale() { return ui::state.scale; }

    // ---- the frame -----------------------------------------------------------------------------------------------

    void init(App &app, const Options &options)
    {
        shell = Shell();
        shell.app = &app;
        shell.options = options;
        g_checkedUpdates = options.screenshot;
        if (!options.demo.empty())
            demoTask(app, options.demo);
        if (!options.screen.empty())
            show(app, options.screen);
        shell.mode = decideMode(app);
        shell.previousMode = shell.mode;
        shell.modeSince = ui::state.time;
        shell.stepSince = ui::state.time;
    }

    void show(App &app, const std::string &screen)
    {
        const size_t sep = screen.find_first_of(":.");
        const std::string kind = screen.substr(0, sep), which = sep == std::string::npos ? "" : screen.substr(sep + 1);
        if (kind == "setup")
        {
            static const char *steps[] = {"folder", "disc", "tools", "game"};
            int step = -1;
            for (int i = 0; i < 4; ++i)
                if (which == steps[i])
                    step = i;
            openSetup(step);
            g_forcedMode = int(Mode::Setup);
        }
        else if (kind == "home" || kind == "jogo")
        {
            shell.setupOpen = false;
            shell.tab = Tab::Game;
            g_forcedMode = int(Mode::Home);
        }
        else if (kind == "mods")
        {
            shell.setupOpen = false;
            shell.tab = Tab::Mods;
            if (!which.empty() && which[0] >= '0' && which[0] <= '3')
                shell.modsSubTab = which[0] - '0';
            g_forcedMode = int(Mode::Home);
        }
        else if (kind == "controls" || kind == "controles")
        {
            shell.setupOpen = false;
            shell.tab = Tab::Controls;
            if (!which.empty() && which[0] >= '0' && which[0] <= '6')
                shell.controlsSubTab = which[0] - '0';
            g_forcedMode = int(Mode::Home);
        }
        else if (kind == "settings" || kind == "config")
        {
            shell.setupOpen = false;
            shell.tab = Tab::Settings;
            if (!which.empty() && which[0] >= '0' && which[0] <= '6')
                shell.configSubTab = which[0] - '0';
            g_forcedMode = int(Mode::Home);
        }
        else if (kind == "about" || kind == "sobre")
        {
            shell.setupOpen = false;
            shell.tab = Tab::About;
            if (!which.empty() && which[0] >= '0' && which[0] <= '2')
                shell.aboutSubTab = which[0] - '0';
            g_forcedMode = int(Mode::Home);
        }
        else if (kind == "lang" || kind == "language")
        {
            shell.setupOpen = false;
            shell.tab = Tab::Game;
            shell.forceLanguagePrompt = true;
            g_forcedMode = int(Mode::Home);
        }
        else if (kind == "task")
        {
            if (!app.task)
                demoTask(app, "running");
            g_forcedMode = int(Mode::Task);
        }
        else if (kind == "sheet")
            openSheet(which == "saves" ? Sheet::Saves : which == "settings" ? Sheet::Settings : which == "help" ? Sheet::Help : Sheet::Folder);
        else if (kind == "dialog")
        {
            if (which == "restore")
                shell.restoreName = "2026-10-31_21-04-12";
            if (which == "cancel" && !app.task)
                demoTask(app, "running");
            openDialog(which == "restore" ? Dialog::Restore : which == "quit" ? Dialog::Quit : Dialog::CancelTask);
        }
        else if (kind == "gallery")
            g_forcedMode = 100;
    }

    void requestQuit(App &app)
    {
        if (app.busy() && takesOver(*app.task))
            openDialog(Dialog::Quit);
        else
            shell.quitNow = true;
    }

    bool closing() { return shell.quitNow; }

    void runPending()
    {
        if (!shell.pending)
            return;
        auto pending = std::move(shell.pending);
        shell.pending = nullptr;
        pending();
    }

    Color clearColor() { return theme::kNight; }

    void frame(App &app)
    {
        shell.app = &app;
        const platform::Input &in = platform::input;
        const Rect window{0, 0, float(platform::window.width), float(platform::window.height)};
        // the launcher's own setting decides, both ways (Windows' "Animate controls and elements inside windows" is
        // off on many PCs tuned for speed - it only gives the starting value)
        ui::state.motion = reduceMotion(app) ? 0.0f : 1.0f;

        // light or dark: the whole palette crossfades (a quarter of a second), the window's frame switches with it
        const bool dark = darkTheme(app);
        if (g_dark < 0)
            g_dark = dark ? 1.0f : 0.0f;
        ui::animate(g_dark, dark ? 1.0f : 0.0f, 14);
        theme::apply(g_dark);
        platform::setDarkFrame(dark);

        // files dropped on the window: a disc
        for (const fs::path &file : in.dropped)
        {
            app.chooseDisc(file);
            if (!app.disc.ok)
                openSetup(kStepDisc);
            else
                toast("Disc verified: " + launcher::pathText(file.filename()));
            break;
        }
        // the model's messages become toasts
        if (!app.toast.empty() && (app.toast != shell.lastToast || app.toastTime != shell.lastToastTime))
        {
            shell.lastToast = app.toast;
            shell.lastToastTime = app.toastTime;
            toast(app.toast);
        }
        // back from the game (always after an error, which Home explains)
        if (g_wasPlaying && !app.gameRunning && shell.minimizedForGame)
        {
            platform::restoreAndRaise();
            shell.minimizedForGame = false;
        }
        g_wasPlaying = app.gameRunning;
        if (app.canUpdate() && !g_checkedUpdates && app.settings.checkUpdates && app.doctorDone && !app.doctorCached &&
            !app.busy())
        {
            g_checkedUpdates = true;
            app.startUpdateCheck(true);
        }
        // a build folder checked by tools/build_dir.py: confirm when the answer comes
        if (shell.awaitingBuildDir && !app.busy())
        {
            shell.awaitingBuildDir = false;
            if (app.buildDirCheck.has("path"))
                openDialog(Dialog::BuildDir);
        }
        keyboard(app);
        // --demo count: the sample build gains 1% every 0.8 s (the rolling percentage, frame by frame)
        if (shell.options.demo == "count" && app.task && !app.task->finished)
        {
            launcher::Task &t = *app.task;
            const int steps = int(ui::state.wall / 0.8);
            t.done = std::min(t.total, 1180 + steps * t.total / 100);
            ui::wakeAt((steps + 1) * 0.8);
        }

        if (g_forcedMode == 100)
        {
            drawBackdrop(window, 0);
            Rect content = window;
            core::cutTop(content, drawTitleBar(window));
            drawGallery(content);
            drawToasts(window);
            return;
        }

        const Mode mode = decideMode(app);
        if (mode != shell.mode)
        {
            shell.previousMode = shell.mode;
            shell.mode = mode;
            shell.modeSince = ui::state.time;
            shell.showLog = false;
            if (mode == Mode::Task)
            {
                // Setup opened from Home is done with once something runs: after it, Home (or Setup, if still needed)
                shell.setupOpen = false;
                shell.pinnedStep = -1;
            }
        }
        ui::animate(g_hero, (shell.mode == Mode::Home && shell.tab == Tab::Game) ? 1.0f : 0.15f, 5.0f);
        const Rect sidebar{window.x, window.y, px(kSidebarWidth), window.h};
        const Rect mainArea{window.x + px(kSidebarWidth), window.y, std::max(0.0f, window.w - px(kSidebarWidth)), window.h};

        if (render::imageInfo(render::ImageId::Background).valid)
        {
            render::imageCover(mainArea, render::ImageId::Background);
            const float scrimAlpha = 1.0f - core::clamp01((g_hero - 0.15f) / 0.85f);
            if (scrimAlpha > 0.01f)
            {
                // 1980s GTA: Vice City Tropical Cyan/Aqua gradient wash over the palm tree silhouette
                render::gradient(mainArea,
                                 core::withAlpha(core::rgb(0xD2EFF4), 0.86f * scrimAlpha),
                                 core::withAlpha(core::rgb(0x02CFDB), 0.88f * scrimAlpha));
            }
            else
            {
                // Subtle bottom gradient vignette for the bottom info bar on JOGO
                render::gradient({mainArea.x, mainArea.bottom() - px(110), mainArea.w, px(110)},
                                 core::rgb(0x100822, 0), core::rgb(0x100822, 160));
            }
        }
        else
        {
            drawBackdrop(mainArea, g_hero, !app.gameRunning);
        }

        drawSidebar(sidebar);
        const float topH = drawTitleBar(window);
        Rect content{mainArea.x, mainArea.y + topH, mainArea.w, std::max(0.0f, mainArea.h - topH)};

        // the mode that is leaving fades and drifts up while the new one rises in
        const float out = ui::state.motion > 0 ? float((ui::state.time - shell.modeSince) / 0.22) : 1.0f;
        if (out < 1 && shell.previousMode != shell.mode)
        {
            ui::keepAnimating();
            ui::pushDisabled();
            render::pushOpacity(1 - core::easeOutCubic(out));
            render::pushOffset(0, -std::round(core::easeOutCubic(out) * px(10)));
            drawMode(shell.previousMode, content);
            render::popOffset();
            render::popOpacity();
            ui::popDisabled();
        }
        drawMode(shell.mode, content);

        drawSheets(window);
        drawDialogs(window);

        // First-launch Language Selection Prompt (Português do Brasil / English)
        if ((!app.settings.languageChosen && !shell.options.screenshot) || shell.forceLanguagePrompt)
        {
            ui::beginLayer(kLayerDialog, true);
            render::rect(window, core::rgb(0x06040C, 210));
            const float boxW = px(480);
            const float boxH = px(250);
            const Rect box{window.cx() - boxW * 0.5f, window.cy() - boxH * 0.5f, boxW, boxH};

            render::shadow(box.offset(0, px(10)), core::rgb(0x000000, 185), px(28), px(8));
            render::rect(box, core::rgb(0xFBFDFE), px(6));
            // Top Flamingo Pink header bar matching the Vice City menu
            const Rect headR{box.x, box.y, box.w, px(52)};
            render::rect(headR, core::rgb(0xFE7FB4), {px(6), px(6), 0, 0});
            ui::label(render::kFontSemibold, px(16.0f), headR, core::rgb(0xFFFFFF),
                      "SELECIONE O IDIOMA  /  SELECT LANGUAGE", ui::Align::Centre);

            ui::label(render::kFontSemibold, px(14.0f), {box.x + px(24), headR.bottom() + px(22), box.w - px(48), px(22)},
                      core::rgb(0x007A96), "Escolha o idioma da interface do VCS.rec Launcher:", ui::Align::Centre);
            ui::label(render::kFontBody, px(13.0f), {box.x + px(24), headR.bottom() + px(44), box.w - px(48), px(20)},
                      core::rgb(0x007A96, 190), "Choose your preferred language for the VCS.rec Launcher:", ui::Align::Centre);

            const float btnW = px(196);
            const float btnH = px(52);
            const float btnY = box.bottom() - btnH - px(28);
            const Rect ptR{box.cx() - btnW - px(10), btnY, btnW, btnH};
            const Rect enR{box.cx() + px(10), btnY, btnW, btnH};

            auto langChoiceBtn = [&](std::string_view idStr, std::string_view titleStr, std::string_view subStr, Rect rBtn) {
                const ui::Interaction lin = ui::interact(ui::id(idStr), rBtn, true);
                const float h = lin.w->hover;
                const Rect b = rBtn.scaled(1.0f - 0.04f * lin.w->press);
                const Color bg = core::mix(core::rgb(0xE8F7FA), core::rgb(0xFE7FB4), h);
                const Color border = core::mix(core::rgb(0x0084A1, 90), core::rgb(0xFE7FB4), h);
                render::rectEx(b, bg, bg, px(6), border, px(1.5f));
                const Color txtCol = core::mix(core::rgb(0x007A96), core::rgb(0xFFFFFF), h);
                ui::label(render::kFontSemibold, px(14.5f), {b.x, b.y + px(8), b.w, px(20)}, txtCol, titleStr, ui::Align::Centre);
                ui::label(render::kFontBody, px(11.5f), {b.x, b.y + px(27), b.w, px(16)}, txtCol, subStr, ui::Align::Centre);
                return lin.clicked;
            };

            if (langChoiceBtn("#first-lang-pt", "PORTUGUÊS", "Brasil (PT-BR)", ptR))
            {
                app.settings.language = "pt";
                app.settings.languageChosen = true;
                shell.forceLanguagePrompt = false;
                app.saveSettings();
            }
            if (langChoiceBtn("#first-lang-en", "ENGLISH", "United States (EN)", enR))
            {
                app.settings.language = "en";
                app.settings.languageChosen = true;
                shell.forceLanguagePrompt = false;
                app.saveSettings();
            }
            ui::endLayer();
        }

        drawToasts(window);
    }
}
