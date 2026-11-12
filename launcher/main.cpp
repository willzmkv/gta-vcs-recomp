// VCSLauncher: from "downloaded the release" to playing, without a terminal (docs/LAUNCHER.md).
//
//   VCSLauncher.exe                     the window
//   VCSLauncher.exe --check             tools/doctor.py's report in the terminal
//   VCSLauncher.exe --build [--iso X]   tools/build.py --recompile-if-needed, output in the terminal
//   VCSLauncher.exe --play [--iso X]    start the game and wait for it; its exit code
//   VCSLauncher.exe --self-test [--iso X]   the launcher's own checks (CI): repository, Python, doctor, disc check
//   ... --out FILE                      the output to a file instead (scripts, CI)
//   VCSLauncher.exe --screenshot F.png  the window rendered offscreen and saved (tests, docs; never a window):
//       [--screen S] [--demo D]         what is shown (screens/screens.h: setup:disc, home, task, sheet:saves,
//                                       dialog:quit, gallery...; demo: running, count (1% every 0.8 s), link, firstlink, error, done,
//                                       playing, update, updated (Restart after an update), stale (the build out of date))
//       [--then S --at T]               switch to S after T seconds (transitions)
//       [--frames N]                    N consecutive frames from the start (or from --then): F-000.png...
//       [--size WxH] [--scale K]        logical size (1100x700) and pixels per unit (1)
//       [--theme light|dark]            the palette (dark when not given, whatever launcher.ini or Windows say)
//       [--clock S]                     Home's moving scene S seconds in (its jets, clouds, windows...), plus the run's time
//       [--step N]                      the old page numbers: 0-3 the setup steps, 4 Saves, 5 Settings, 6 Help
//       [--action build --every S] [--iso X] [--wait S] [--pick-build-dir PATH]
//   VCSLauncher.exe --stats             the window with a frame-time overlay (also Ctrl+Shift+F11)
//   VCSLauncher.exe --after-update PID  (the Restart button after an update) wait for the old launcher to end, then
//                                       the window
//   VCSLauncher.exe --hidden --quit-after S [--out F]   the real window and frame loop, never shown, for S seconds;
//                                       then the frames drawn and the CPU time used (the idle cost; tests)
//
// The command-line forms print into the terminal that started them (a GUI exe: in PowerShell use
// `Start-Process -Wait -NoNewWindow`, in cmd `start /wait`).
//
// The frame loop (uppies'): sleep in the OS until input, a wake from a tool's output, a timer or an animation wants
// a frame; then wait until the display can take one, read input, build the UI, one draw, present. An idle window
// uses no CPU or GPU.
#include "core/png.h"
#include "model/disc_check.h"
#include "model/system.h"
#include "model/tasks.h"
#include "platform/platform.h"
#include "render/draw.h"
#include "render/font.h"
#include "render/renderer.h"
#include "screens/screens.h"
#include "ui/theme.h"
#include "ui/ui.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace launcher;

namespace
{
    std::string argValue(const std::vector<std::string> &args, const char *name)
    {
        for (size_t i = 0; i + 1 < args.size(); ++i)
            if (args[i] == name)
                return args[i + 1];
        return {};
    }

    bool hasArg(const std::vector<std::string> &args, const char *name)
    {
        for (const auto &a : args)
            if (a == name)
                return true;
        return false;
    }

    int runInConsole(const std::vector<std::string> &argv, const fs::path &cwd)
    {
        Process p;
        Process::Options options;
        options.cwd = cwd;
        options.inheritConsole = true;
        options.env = {{"PYTHONUTF8", "1"}};
        std::string error;
        if (!p.start(argv, options, error))
        {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return 1;
        }
        return p.wait();
    }

    int commandLine(const std::vector<std::string> &args)
    {
        if (const std::string out = argValue(args, "--out"); !out.empty())
            redirectOutput(pathFrom(out));
        else
            attachParentConsole();
        App app;
        std::string error;
        // init() runs the doctor in the background and scans for a disc; the command-line forms do their own
        if (!app.init(error))
        {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return 1;
        }
        // a Linux AppImage puts its files in place first (tools/update.py install): the forms below run them
        if (app.installing())
        {
            std::printf("putting the game's files in place: %s\n", pathText(app.root).c_str());
            while (app.installing())
            {
                app.update(platform::now());
                platform::sleepFor(0.02);
            }
            if (app.task) // still there: it failed (a finished install goes, the folder opens)
            {
                for (const auto &line : app.task->log)
                    std::printf("%s\n", line.c_str());
                std::fprintf(stderr, "error: %s\n", app.task->errorMessage.c_str());
                return 1;
            }
        }
        const std::string iso = argValue(args, "--iso");
        if (!iso.empty())
            app.chooseDisc(pathFrom(iso));
        auto tool = [&](const char *name) {
            std::vector<std::string> argv = app.python;
            argv.insert(argv.end(), {"-I", pathText(app.root / "tools" / name)});
            return argv;
        };
        if (hasArg(args, "--check"))
            return runInConsole(tool("doctor.py"), app.root);
        if (hasArg(args, "--build"))
        {
            auto argv = tool("build.py");
            argv.push_back("--recompile-if-needed");
            if (app.disc.ok)
                argv.push_back(pathText(app.iso));
            return runInConsole(argv, app.root);
        }
        if (hasArg(args, "--play"))
        {
            if (!app.disc.ok)
            {
                std::fprintf(stderr, "error: no disc: %s\n", app.disc.message.c_str());
                return 2;
            }
            if (!app.play(error))
            {
                std::fprintf(stderr, "error: %s\n", error.c_str());
                return 1;
            }
            std::printf("playing; the game's output: %s\n", pathText(app.gameLog).c_str());
            return app.game->wait();
        }
        if (hasArg(args, "--self-test"))
        {
            int failed = 0;
            auto check = [&](bool ok, const std::string &what) {
                std::printf("[%s] %s\n", ok ? "ok" : "FAIL", what.c_str());
                failed += ok ? 0 : 1;
            };
            check(fs::is_regular_file(app.root / "tools" / "build.py"), "repository: " + pathText(app.root));
            check(!app.python.empty(), "python: " + app.pythonText);
            check(app.toolsProblem.empty(), app.toolsProblem.empty() ? "scripts: all in tools" : app.toolsProblem);
            for (int i = 0; i < 6000 && (app.doctorRunning || !app.doctorDone || app.doctorCached); ++i)
            {
                app.update(i * 0.01);
                platform::sleepFor(0.01);
            }
            check(app.doctor.isObject(), "doctor.py --json" + (app.doctorError.empty() ? "" : ": " + app.doctorError));
            check(!app.doctor["next"].str().empty(), "doctor's next step: " + app.doctor["next"].str());
            if (!iso.empty())
                check(app.disc.ok, "disc: " + app.disc.message);
            const DiscResult missing = checkDisc(app.root / "no-such-disc.iso");
            check(!missing.ok && missing.code == "not_found", "disc check refuses a missing file");
            const DiscResult text = checkDisc(app.root / "README.md");
            check(!text.ok && text.code == "not_iso", "disc check refuses a text file: " + text.message);
            // the renderer and the fonts, offscreen (a GPU, or WARP without one)
            std::string renderError;
            const bool rendered = render::rendererInit(nullptr, 64, 64, renderError) && render::fontsInit(renderError);
            check(rendered, rendered ? "renderer: " + render::rendererStats.api + " (" + render::rendererStats.adapter +
                                           "), " + render::fontRasterizer()
                                     : "renderer: " + renderError);
            render::fontsShutdown();
            render::rendererShutdown();
            std::printf("%s\n", failed ? "self-test FAILED" : "self-test ok");
            return failed ? 1 : 0;
        }
        std::fprintf(stderr, "usage: VCSLauncher [--check | --build | --play | --self-test] [--iso PATH] [--out FILE]\n");
        return 1;
    }

    // ---- the window ------------------------------------------------------------------------------------------------

    struct Shots
    {
        std::string path, screen, demo, then, action, iso, pickBuildDir, theme;
        double wait = 3.0, every = 0, at = 1.0, clock = 0;
        int frames = 0;
        int width = 1100, height = 700;
        float scale = 1;
    };

    constexpr int kWidth = 1100, kHeight = 700, kMinWidth = 920, kMinHeight = 600;
    constexpr float kMaxFrameDt = 0.1f; // a hitch or a breakpoint doesn't jump animations

    struct Loop
    {
        App *app = nullptr;
        const Shots *shots = nullptr;
        double lastTime = 0;
        bool resumed = true;     // woke from sleeping: assume one refresh for dt
        bool timedWake = false;  // the sleep ended on its timeout: one frame (no settle frame, unlike input)
        int settleFrames = 0;
        bool stats = false;
        double fixedTime = 0;    // screenshots: a fixed 60 Hz clock
        float cpuMs = 0, intervalMs = 0, renderMs = 0; // the frame, and the renderer's part of it (upload + submit)
        long long frames = 0;
        double cpuTotalMs = 0, cpuMaxMs = 0, renderTotalMs = 0;
    };
    Loop g_loop;

    void drawStats()
    {
        using namespace render;
        const Layer previous = setLayer(Layer::Overlay);
        const float s = ui::state.scale;
        const core::Rect box{draw.width - 236 * s, 52 * s, 220 * s, 92 * s};
        rect(box, core::rgb(0x000000, 190), 8 * s);
        char lines[4][96];
        std::snprintf(lines[0], sizeof(lines[0]), "frame %.2f ms (%.2f render), %.1f apart", g_loop.cpuMs, g_loop.renderMs,
                      g_loop.intervalMs);
        std::snprintf(lines[1], sizeof(lines[1]), "%d instances, %d draw, %zu B up", rendererStats.instances,
                      rendererStats.drawCalls, rendererStats.uploadBytes);
        std::snprintf(lines[2], sizeof(lines[2]), "atlas %d, %s", atlas.size, rendererStats.warp ? "WARP" : "GPU");
        std::snprintf(lines[3], sizeof(lines[3]), "%.0f Hz, scale %.2f%s", platform::window.refreshHz, s,
                      platform::window.reducedMotion ? ", reduced motion" : "");
        for (int i = 0; i < 4; ++i)
            drawText(kFontMono, 11.5f * s, box.x + 10 * s, box.y + (20 + 19 * i) * s, core::rgb(0xD8F5EE), lines[i]);
        setLayer(previous);
    }

    void runFrame()
    {
        Loop &L = g_loop;
        const double start = platform::now();
        double dt;
        if (L.shots)
        {
            // a fixed 60 Hz clock (the same picture every run); a real build runs on real time
            dt = 1.0 / 60.0;
            L.fixedTime = L.shots->action.empty() ? L.fixedTime + dt : std::max(L.fixedTime + dt, start);
        }
        else
        {
            dt = start - L.lastTime;
            L.intervalMs = float(dt * 1000.0);
            // after sleeping, the gap says nothing about how long this frame will be on screen: one refresh
            if (L.resumed)
                dt = 1.0 / platform::window.refreshHz;
        }
        L.lastTime = start;
        L.resumed = false;
        L.timedWake = false;
        // an immediate-mode UI settles a frame after input (a click can change what is drawn above it)
        if (platform::window.needsRedraw)
            L.settleFrames = 1;
        else if (L.settleFrames > 0)
            --L.settleFrames;
        platform::window.needsRedraw = false;

        const double wall = L.shots ? L.fixedTime : start;
        L.app->update(wall);
        if (platform::input.key(platform::kKeyF12 - 1) && platform::input.ctrl && platform::input.shift)
            L.stats = !L.stats;
        if (platform::input.key(platform::kKeyF12 - 2) && platform::input.ctrl && platform::input.shift)
            render::rendererSimulateDeviceLoss(); // Ctrl+Shift+F10: as after a driver reset
        render::fontsBeginFrame();
        render::beginFrame(float(platform::window.width), float(platform::window.height));
        ui::beginFrame(std::min(float(dt), kMaxFrameDt), wall);
        screens::frame(*L.app);
        if (L.stats)
            drawStats();
        ui::endFrame();
        platform::endFrame();
        render::endFrame();
        const double renderStart = platform::now();
        render::rendererRender(screens::clearColor(), platform::window.width, platform::window.height);
        const double end = platform::now();
        L.renderMs = float((end - renderStart) * 1000.0);
        L.renderTotalMs += L.renderMs;
        L.cpuMs = float((end - start) * 1000.0);
        L.frames++;
        L.cpuTotalMs += L.cpuMs;
        L.cpuMaxMs = std::max(L.cpuMaxMs, double(L.cpuMs));
    }

    bool wantsFrame()
    {
        const double now = g_loop.shots ? g_loop.fixedTime : platform::now();
        return platform::window.needsRedraw || g_loop.settleFrames > 0 || ui::state.animating || g_loop.timedWake ||
               (ui::state.wakeAt > 0 && now >= ui::state.wakeAt);
    }

    // frames Win32's modal loops ask for (the window dragged or resized, a file dialog up)
    void modalFrame()
    {
        render::rendererWaitForFrame();
        runFrame();
        render::rendererPresent(true);
    }

    std::string numbered(const std::string &path, int index)
    {
        char suffix[16];
        std::snprintf(suffix, sizeof(suffix), "-%03d", index);
        const size_t dot = path.rfind('.');
        return path.substr(0, dot) + suffix + (dot == std::string::npos ? "" : path.substr(dot));
    }

    bool saveShot(const std::string &path)
    {
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        if (!render::rendererReadback(rgba, w, h) || !core::writePng(pathFrom(path), w, h, rgba.data()))
        {
            std::fprintf(stderr, "error: could not save %s\n", path.c_str());
            return false;
        }
        std::printf("screenshot: %s (%.1f s)\n", path.c_str(), g_loop.fixedTime);
        return true;
    }

    int gui(const Shots *shots, bool stats, bool hidden = false, double quitAfter = -1)
    {
        App app;
        std::string error;
        if (!app.init(error))
        {
            if (shots)
                std::fprintf(stderr, "error: %s\n", error.c_str());
            else
                platform::errorBox(error);
            return 1;
        }
        g_loop.app = &app;
        g_loop.shots = shots;
        g_loop.stats = stats;
        if (shots)
        {
            // no window: a fixed size and scale, drawn into a texture
            platform::window.width = int(float(shots->width) * shots->scale);
            platform::window.height = int(float(shots->height) * shots->scale);
            platform::window.dpiScale = shots->scale;
            platform::window.refreshHz = 60;
            platform::window.reducedMotion = false;
            app.settings.theme = shots->theme == "light" ? "light" : "dark";
            if (!shots->iso.empty())
                app.chooseDisc(pathFrom(shots->iso));
        }
        else if (!platform::init("GTA: Vice City Stories - Launcher", kWidth, kHeight, kMinWidth, kMinHeight, error))
        {
            platform::errorBox(error);
            return 1;
        }
        // Linux: a hidden run draws offscreen - presenting to a window that is never shown can block forever (Wayland's
        // FIFO waits for a frame callback that never comes); Windows presents to its hidden window as before
#ifdef _WIN32
        void *target = shots ? nullptr : platform::nativeHandle();
#else
        void *target = shots || hidden ? nullptr : platform::nativeHandle();
#endif
        if (!render::rendererInit(target, platform::window.width,
                                  platform::window.height, error) ||
            !render::fontsInit(error))
        {
            if (shots)
                std::fprintf(stderr, "error: %s\n", error.c_str());
            else
                platform::errorBox(error);
            return 1;
        }
        Process::setWakeHook(platform::wake);
        platform::setModalFrame(modalFrame);
        ui::state.scale = platform::window.dpiScale;

        screens::Options options;
        options.screenshot = shots != nullptr;
        if (shots)
        {
            options.screen = shots->screen;
            options.demo = shots->demo;
            options.clock = shots->clock;
        }
        ui::state.wall = shots ? 0 : platform::now();
        screens::init(app, options);

        // the first frame is drawn before the window appears: it never flashes empty
        g_loop.lastTime = platform::now();
        render::rendererWaitForFrame();
        runFrame();
        render::rendererPresent(true);
        if (!shots && !hidden)
            platform::show();
        const double cpuAtStart = processCpuSeconds(), timeAtStart = platform::now();
        const long long framesAtStart = g_loop.frames;

        float lastScale = platform::window.dpiScale;
        bool startedAction = false, switched = false;
        int shotIndex = 0, framesSaved = 0;
        double lastShot = -1e9, framesFrom = 0;
        while (!screens::closing())
        {
            if (shots)
            {
                const double t = g_loop.fixedTime;
                if (!shots->pickBuildDir.empty() && !startedAction && app.doctorDone && !app.doctorCached)
                {
                    startedAction = true; // as if the folder dialog had answered
                    app.startBuildDirCheck(pathFrom(shots->pickBuildDir));
                }
                if (shots->action == "build" && !startedAction && app.doctorDone && !app.doctorCached)
                {
                    startedAction = true;
                    app.startBuild();
                }
                if (!shots->then.empty() && !switched && t >= shots->at)
                {
                    switched = true;
                    screens::show(app, shots->then);
                    framesFrom = t;
                }
                runFrame();
                const double now = g_loop.fixedTime;
                // frames of an animation: from the start, or from the switch
                if (shots->frames > 0 && (shots->then.empty() || switched) && framesSaved < shots->frames && now >= framesFrom)
                {
                    saveShot(numbered(shots->path, framesSaved++));
                    if (framesSaved == shots->frames)
                        break;
                    continue;
                }
                const bool settled = app.doctorDone && !app.doctorRunning && now >= shots->wait && (shots->then.empty() || switched);
                const bool actionDone = shots->action.empty() ? settled : startedAction && !app.busy() && settled;
                const bool periodic = shots->every > 0 && now - lastShot >= shots->every;
                if (shots->frames == 0 && (periodic || actionDone))
                {
                    saveShot(shots->every > 0 && !actionDone ? numbered(shots->path, shotIndex++) : shots->path);
                    lastShot = now;
                    if (actionDone)
                        break;
                }
                // the model runs on real time (the tools take theirs); keep a modest pace while waiting on them
                if (!actionDone && app.doctorRunning)
                    platform::sleepFor(0.005);
                if (shots->action == "build" && app.busy())
                    platform::sleepFor(0.05);
                if (now > 3600 * 3)
                    break;
                continue;
            }

            if (quitAfter > 0 && platform::now() - timeAtStart >= quitAfter)
                break;
            platform::pumpEvents();
            if (platform::window.quitRequested)
            {
                platform::window.quitRequested = false;
                screens::requestQuit(app);
                if (screens::closing())
                    break;
            }
            if (platform::window.dpiScale != lastScale)
            {
                // moved to a monitor with another scale: glyphs rasterise again at the new sizes
                lastScale = platform::window.dpiScale;
                ui::state.scale = lastScale;
                render::fontsClearCache();
            }
            if (platform::window.minimized || !wantsFrame())
            {
                // sleep until input, a tool's output, or a frame asked for at a time
                double timeout = -1;
                // (minimized, a frame asked for is drawn on restore: a time already past would wake this at once,
                // again and again - the playing clock while the game runs)
                if (ui::state.wakeAt > 0 && !platform::window.minimized)
                    timeout = std::max(ui::state.wakeAt - platform::now(), 0.0);
                if (app.doctorRunning || app.busy() || app.gameRunning)
                    timeout = timeout < 0 ? 1.0 : std::min(timeout, 1.0); // clocks on screen; a tool's grace after exit
                if (quitAfter > 0)
                {
                    const double left = std::max(0.0, quitAfter - (platform::now() - timeAtStart));
                    timeout = timeout < 0 ? left : std::min(timeout, left);
                }
                platform::waitForEvents(timeout);
                g_loop.resumed = true;
                if (timeout >= 0 && !platform::window.minimized)
                    g_loop.timedWake = true;
                continue;
            }
#ifndef _WIN32
            // a hidden run draws offscreen, where nothing paces frames: the display's rate, as a window would have
            if (hidden)
            {
                static double nextFrame = 0;
                const double t = platform::now();
                platform::sleepFor(nextFrame - t);
                nextFrame = std::max(nextFrame, t) + 1.0 / platform::window.refreshHz;
            }
#endif
            // every wait is paired with a present, or the swap chain's latency budget drifts
            render::rendererWaitForFrame();
            platform::pumpEvents(); // input that arrived while waiting makes this frame
            render::rendererWaitForFrame(); // a modal loop entered during the pump may have presented meanwhile
            runFrame();
            render::rendererPresent(true);
            screens::runPending(); // file dialogs: between frames, never inside one
        }

        if (quitAfter > 0)
        {
            const long long frames = g_loop.frames - framesAtStart;
            std::printf("frames drawn: %lld in %.1f s; frame %.3f ms average (renderer %.3f), %.3f ms worst; process CPU %.0f ms "
                        "(doctor.py ran %s)\n",
                        frames, platform::now() - timeAtStart, g_loop.cpuTotalMs / double(std::max(g_loop.frames, 1LL)),
                        g_loop.renderTotalMs / double(std::max(g_loop.frames, 1LL)), g_loop.cpuMaxMs,
                        (processCpuSeconds() - cpuAtStart) * 1000.0, app.doctorCached ? "on" : "to the end");
        }
        const int code = shots && app.task && app.task->finished && !app.task->ok() ? 1 : 0;
        app.task.reset(); // a task cancelled on the way out: its job ends the tree
        Process::setWakeHook(nullptr);
        render::fontsShutdown();
        render::rendererShutdown();
        if (!shots)
            platform::shutdown();
        return code;
    }
}

int launcherMain(const std::vector<std::string> &argsIn)
{
    std::vector<std::string> args = argsIn;
    // started by the Restart button after an update (App::restart): the old launcher first ends, so its renamed exe
    // (VCSLauncher.exe.old) can go and an AppImage's old mount is gone; then the window as usual
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == "--after-update")
        {
            waitForProcessExit(std::strtoul(args[i + 1].c_str(), nullptr, 10), 15.0);
            args.erase(args.begin() + long(i), args.begin() + long(i) + 2);
            break;
        }
    if (hasArg(args, "--screenshot"))
    {
        if (const std::string out = argValue(args, "--out"); !out.empty())
            redirectOutput(pathFrom(out));
        else
            attachParentConsole();
        Shots shots;
        shots.path = argValue(args, "--screenshot");
        shots.screen = argValue(args, "--screen");
        if (const std::string step = argValue(args, "--step"); !step.empty())
        {
            static const char *pages[] = {"setup:folder", "setup:disc", "setup:tools", "setup:game", "sheet:saves", "sheet:settings", "sheet:help"};
            const int n = std::atoi(step.c_str());
            if (n >= 0 && n < 7)
                shots.screen = pages[n];
        }
        shots.demo = argValue(args, "--demo");
        shots.theme = argValue(args, "--theme");
        shots.then = argValue(args, "--then");
        shots.action = argValue(args, "--action");
        shots.iso = argValue(args, "--iso");
        shots.pickBuildDir = argValue(args, "--pick-build-dir");
        if (const std::string v = argValue(args, "--wait"); !v.empty())
            shots.wait = std::atof(v.c_str());
        if (const std::string v = argValue(args, "--every"); !v.empty())
            shots.every = std::atof(v.c_str());
        if (const std::string v = argValue(args, "--clock"); !v.empty())
            shots.clock = std::atof(v.c_str());
        if (const std::string v = argValue(args, "--at"); !v.empty())
            shots.at = std::atof(v.c_str());
        if (const std::string v = argValue(args, "--frames"); !v.empty())
            shots.frames = std::atoi(v.c_str());
        if (const std::string v = argValue(args, "--scale"); !v.empty())
            shots.scale = std::max(0.5f, float(std::atof(v.c_str())));
        if (const std::string v = argValue(args, "--size"); !v.empty())
            std::sscanf(v.c_str(), "%dx%d", &shots.width, &shots.height);
        return gui(&shots, false);
    }
    if (hasArg(args, "--hidden"))
    {
        if (const std::string out = argValue(args, "--out"); !out.empty())
            redirectOutput(pathFrom(out));
        else
            attachParentConsole();
        const std::string after = argValue(args, "--quit-after");
        return gui(nullptr, false, true, after.empty() ? 10.0 : std::atof(after.c_str()));
    }
    if (!args.empty() && !hasArg(args, "--stats"))
        return commandLine(args);
    return gui(nullptr, hasArg(args, "--stats"));
}
