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
            if (!app.disc.ok)
                return Mode::Setup;
            if (!app.doctorDone || app.doctorFailed())
                return Mode::Home; // Home says it is checking, or why it couldn't
            const std::string next = app.next();
            if (next == "tools" || cpuUnsupported(app))
                return Mode::Setup;
            if (next == "build" && !app.doctor["build"]["built"].boolean())
                return Mode::Setup; // the first build is the last setup step; an update is Home's
            return Mode::Home;
        }

        void drawMode(Mode mode, Rect content)
        {
            switch (mode)
            {
            case Mode::Setup: drawSetup(content); break;
            case Mode::Home: drawHome(content); break;
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
        return t.kind == "build" || t.kind == "tools" || t.kind == "update" || t.kind == "rip" || t.kind == "report" ||
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
            if (platform::openFolderDialog(picked, "Choose where the game is built", start))
            {
                shell.app->startBuildDirCheck(picked);
                shell.awaitingBuildDir = true;
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
        const size_t colon = screen.find(':');
        const std::string kind = screen.substr(0, colon), which = colon == std::string::npos ? "" : screen.substr(colon + 1);
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
        else if (kind == "home")
        {
            shell.setupOpen = false;
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
        ui::animate(g_hero, shell.mode == Mode::Home ? 1.0f : 0.0f, 3.5f);
        drawBackdrop(window, g_hero, !app.gameRunning);
        Rect content = window;
        core::cutTop(content, drawTitleBar(window));

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
        drawToasts(window);
    }
}
