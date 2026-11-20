// Setup: the guided sequence until the game can be played. A rail of the four steps (Folder, Disc, Build tools,
// Game) fills in as they are done; one step at a time below it, large. A step that turns good plays its check, holds
// a moment, and the next slides in. Any step can be looked at from the rail.
#include "shell.h"

#include "model/os.h"
#include "model/system.h"
#include "platform/platform.h"

#include <cmath>

namespace fs = std::filesystem;

namespace screens
{
    using namespace render;
    using core::mix;
    using core::withAlpha;
    using core::rgb;
    using ui::ButtonStyle;

    namespace
    {
        uint64_t g_lastFrame = 0;
        int g_direction = 1;

        bool setupNeeded(const App &app)
        {
            if (!app.disc.ok)
                return true;
            if (!app.doctorDone || app.doctorFailed())
                return false;
            const std::string next = app.next();
            return next == "tools" || cpuUnsupported(app) || (next == "build" && !app.doctor["build"]["built"].boolean());
        }

        int autoStep(const App &app)
        {
            const bool built = app.doctor["build"]["built"].boolean();
            if (app.doctorDone && !app.doctorCached && folderStatus(app) == ui::Status::Warning && !shell.folderSeen && !built)
                return kStepFolder;
            if (!app.disc.ok)
                return kStepDisc;
            const ui::Status tools = toolsStatus(app);
            if (tools != ui::Status::Good && tools != ui::Status::Warning)
                return kStepTools;
            return kStepGame;
        }

        // a button cut from the footer row: from the right (the primary end) or the left
        bool footerButton(Rect &row, std::string_view text, ButtonStyle style, char32_t icon = 0, bool enabled = true,
                          bool fromRight = true)
        {
            const float w = ui::buttonWidth(text, icon);
            const Rect r = fromRight ? core::cutRight(row, w) : core::cutLeft(row, w);
            if (fromRight)
                core::cutRight(row, px(10));
            else
                core::cutLeft(row, px(8));
            bool clicked = ui::button(text, core::centred(r, r.w, px(40)), style, icon, enabled);
            // Enter does the primary thing (unless the keyboard is walking the controls)
            if (style == ButtonStyle::Primary && enabled && ui::inputAllowed() && !ui::state.focusVisible &&
                platform::input.key(platform::kKeyEnter))
                clicked = true;
            return clicked;
        }

        void heading(Rect r, float &y, std::string_view text)
        {
            ui::label(kFontSemibold, px(15.5f), {r.x, y, r.w, px(24)}, theme::kText, text);
            y += px(28);
        }

        void note(Rect r, float &y, std::string_view text, Color color = theme::kDim)
        {
            y += ui::paragraph(kFontBody, px(theme::kBody), {r.x, y, std::min(r.w, px(640)), 0}, color, text) + px(6);
        }

        // a card inside the step for something that needs a look (amber) or is wrong (red)
        void callout(Rect r, float &y, Color color, std::string_view title, std::string_view body)
        {
            const float textW = r.w - px(56);
            const float h = px(18) + px(22) + ui::paragraphHeight(kFontBody, px(theme::kBody), textW, body) + px(16);
            const Rect box{r.x, y, r.w, h};
            rectEx(box, withAlpha(color, 0.08f), withAlpha(color, 0.05f), px(10), withAlpha(color, 0.25f), ui::hairline());
            drawIcon(kFontIcons, px(15), box.x + px(24), box.y + px(28), color, color == theme::kRed ? theme::icon::kCancel : theme::icon::kWarning);
            ui::label(kFontSemibold, px(theme::kBody), {box.x + px(44), box.y + px(16), textW, px(22)}, theme::kText, title);
            ui::paragraph(kFontBody, px(theme::kBody), {box.x + px(44), box.y + px(40), textW, 0}, theme::kDim, body);
            y += h + px(12);
        }

        void checkFailed(App &app, Rect r, float &y)
        {
            callout(r, y, theme::kRed, "Couldn't check", app.doctorError);
            if (ui::button(app.doctorRunning ? "Checking..." : "Check again", {r.x, y, ui::buttonWidth("Check again", theme::icon::kRefresh), px(38)},
                           ButtonStyle::Secondary, theme::icon::kRefresh, !app.doctorRunning))
                app.refresh();
            y += px(50);
        }

        void checking(Rect r, float &y)
        {
            ui::spinner(r.x + px(9), y + px(11), px(7), theme::kDim, px(1.8f));
            ui::label(kFontBody, px(theme::kBody), {r.x + px(26), y, r.w, px(22)}, theme::kDim, "Checking this PC...");
            y += px(34);
        }

        // ---- the steps ---------------------------------------------------------------------------------------------

        // Each step is drawn twice a frame: its body inside the card's scroll area (returns where it ended), then its
        // footer's buttons under it (footer pass: body not drawn).
        float stepFolder(App &app, Rect body, Rect &footer, bool footerPass)
        {
            if (footerPass)
            {
                if (footerButton(footer, "Looks good", ButtonStyle::Primary, theme::icon::kCheck, app.doctorDone))
                {
                    shell.folderSeen = true;
                    shell.pinnedStep = -1;
                }
                return 0;
            }
            float y = body.y;
            y = header({body.x, y, body.w, 0}, "STEP 1 OF 4", theme::kPink, "Where it's built",
                       "The game is compiled into a folder of its own. It can go on another drive with more room; a short local folder works best.");
            drawFolderBody(body, y);
            return y;
        }

        void discFooter(App &app, Rect &footer)
        {
            const bool ok = app.discChecked && app.disc.ok;
            if (ok)
            {
                if (footerButton(footer, "Continue", ButtonStyle::Primary, theme::icon::kChevronRight))
                    shell.pinnedStep = -1;
                if (footerButton(footer, "Choose another", ButtonStyle::Secondary))
                    chooseIso();
            }
            else if (footerButton(footer, "Choose your .iso", ButtonStyle::Primary, theme::icon::kOpenFile))
                chooseIso();
            // tools/rip_disc.py is Windows only (for now)
            if (launcher::os::kWindows &&
                footerButton(footer, "Copy from a DVD drive", ButtonStyle::Ghost, theme::icon::kDisc, !app.busy(), false))
            {
                app.drives = launcher::Json();
                app.startRipList();
                shell.showDrives = true;
            }
            if (footerButton(footer, "Search again", ButtonStyle::Ghost, theme::icon::kRefresh, true, false))
            {
                app.scanForDisc();
                if (!app.disc.ok)
                    toast("No Vice City Stories .iso in Downloads, Desktop, Documents or this folder.", theme::kAmber);
            }
        }

        float stepDisc(App &app, Rect body, Rect &footer, bool footerPass)
        {
            if (footerPass)
            {
                discFooter(app, footer);
                return 0;
            }
            float y = body.y;
            y = header({body.x, y, body.w, 0}, "STEP 2 OF 4", theme::kPink, "Your disc",
                       "Your own copy of GTA: Vice City Stories for the PS2 (USA, SLUS-21590) as a disc image. The game is "
                       "recompiled from it and reads its files from it while you play.");

            // the drop zone: it answers while a file is dragged over the window
            const bool ok = app.discChecked && app.disc.ok;
            const Rect zone{body.x, y, body.w, px(132)};
            const ui::Interaction in = ui::interact(ui::id("#dropzone"), zone, true);
            ui::Widget &w = *in.w;
            const bool over = platform::input.dragOver;
            ui::animate(w.t, over ? 1.0f : 0.0f, 14);
            const float glow = w.t;
            const float breathe = over ? 0.5f + 0.5f * std::sin(float(ui::state.time) * 5.0f) : 0.0f;
            if (over)
                ui::keepAnimating();
            const Rect box = zone.scaled(1.0f + 0.012f * glow);
            if (glow > 0.01f)
                shadow(box, withAlpha(theme::kPink, 0.20f * glow + 0.08f * breathe), px(18), px(14));
            const Color edge = ok && glow < 0.5f ? withAlpha(theme::kTeal, 0.35f) : mix(theme::ink(0.10f + 0.06f * w.hover), theme::kPink, glow);
            rectEx(box, mix(theme::kWell, theme::kWellGlow, glow), mix(theme::kWellBottom, theme::kWellGlowBottom, glow), px(14), edge,
                   px(1.5f) + px(0.5f) * glow);
            const Color iconColor = glow > 0.5f ? theme::kPink : ok ? theme::kTeal : app.discChecked ? theme::kRed : theme::kDim;
            const float iconCx = box.x + px(64), iconCy = box.cy();
            circle(iconCx, iconCy, px(34), withAlpha(iconColor, 0.10f));
            drawIcon(kFontIcons, px(30), iconCx, iconCy, iconColor, theme::icon::kDisc);
            const float tx = box.x + px(120), tw = box.right() - tx - px(24);
            if (glow > 0.5f)
            {
                ui::label(kFontSemibold, px(18), {tx, box.y, tw, box.h}, theme::kText, "Drop it to use it");
            }
            else if (app.discChecked)
            {
                const std::string &code = app.disc.code;
                const char *tag = ok ? "VERIFIED" : code == "not_found" ? "NOT FOUND" : code == "not_iso" ? "NOT AN ISO" : "NOT SUPPORTED";
                const float pw = ui::pillWidth(tag);
                ui::label(kFontSemibold, px(16), {tx, box.y + px(30), tw - pw - px(12), px(24)}, theme::kText,
                          launcher::pathText(app.iso.filename()));
                ui::pill({box.right() - pw - px(22), box.y + px(31), pw, px(22)}, tag, ok ? theme::kTeal : theme::kRed);
                ui::label(kFontBody, px(theme::kSmall), {tx, box.y + px(56), tw, px(20)}, theme::kFaint,
                          launcher::pathText(app.iso.parent_path()));
                ui::paragraph(kFontBody, px(theme::kSmall), {tx, box.y + px(80), tw, 0}, ok ? theme::kDim : theme::kRed,
                              app.disc.message + (ok && app.discFound ? "  -  found automatically" : ""));
            }
            else
            {
                ui::label(kFontSemibold, px(17), {tx, box.y + px(36), tw, px(26)}, theme::kText, "Drop your .iso onto this window");
                ui::label(kFontBody, px(theme::kBody), {tx, box.y + px(66), tw, px(22)}, theme::kDim,
                          launcher::os::kWindows ? "Or choose it, or copy it from a disc in a DVD drive." : "Or choose it.");
            }
            if (in.clicked)
                chooseIso();
            ui::tooltip(ui::id("#dropzone"), zone, "Choose a disc image");
            y += zone.h + px(18);

            if (shell.showDrives)
            {
                heading(body, y, "Copy from a DVD drive");
                note(body, y, "Put the disc in a DVD drive of this PC. It is copied to an .iso (about 3.9 GB, a few minutes) and used from then on.");
                if (app.busy() && app.task->kind == "rip_list")
                    checking(body, y);
                else if (!app.drives.size())
                    note(body, y, "No DVD drive found on this PC.", theme::kAmber);
                else
                    for (size_t i = 0; i < app.drives.size(); ++i)
                    {
                        const launcher::Json &d = app.drives[i];
                        const std::string drive = d["drive"].str();
                        const std::string detail = d["ok"].boolean() ? d["message"].str() + "  (" + d["boot"].str() + ")" : d["message"].str();
                        if (ui::choiceRow("drive" + drive, {body.x, y, body.w, px(54)}, drive, detail, shell.ripDrive == drive, d["ok"].boolean()))
                            shell.ripDrive = drive;
                        y += px(58);
                    }
                float bx = body.x;
                const float w1 = ui::buttonWidth("Copy to an .iso...");
                if (ui::button("Copy to an .iso...", {bx, y, w1, px(38)}, ButtonStyle::Secondary, 0, !shell.ripDrive.empty() && !app.busy()))
                    chooseRipTarget();
                bx += w1 + px(8);
                const float w2 = ui::buttonWidth("Look again");
                if (ui::button("Look again", {bx, y, w2, px(38)}, ButtonStyle::Ghost, 0, !app.busy()))
                    app.startRipList();
                bx += w2 + px(8);
                if (ui::button("Close##drives", {bx, y, ui::buttonWidth("Close"), px(38)}, ButtonStyle::Ghost))
                    shell.showDrives = false;
                y += px(52);
            }
            return y;
        }

        // one checked thing: a badge, its name, what was found
        void checkRow(Rect body, float &y, std::string_view idLabel, ui::Status status, std::string_view name, std::string_view value)
        {
            const float h = std::max(px(44), ui::paragraphHeight(kFontBody, px(theme::kBody), body.w - px(190), value, 1.4f) + px(20));
            ui::statusBadge(ui::id(idLabel), body.x + px(14), y + px(22), px(11), status);
            ui::label(kFontSemibold, px(theme::kBody), {body.x + px(40), y + px(11), px(140), px(22)}, theme::kText, name);
            ui::paragraph(kFontBody, px(theme::kBody), {body.x + px(180), y + px(12), body.w - px(190), 0},
                          status == ui::Status::Failed || status == ui::Status::Missing ? theme::kRed : theme::kDim, value, 1.4f);
            y += h;
            rect({body.x + px(40), y - 1, body.w - px(40), 1}, theme::ink(0.05f));
        }

        float stepTools(App &app, Rect body, Rect &footer, bool footerPass)
        {
            const launcher::Json &d = app.doctor;
            const launcher::Json &c = d["compiler"], &cpu = d["cpu"], &dl = d["downloads"];
            const bool cached = app.doctorCached;
            const std::string code = c["code"].str();
            const bool ready = app.doctorDone && !app.doctorFailed();
            const bool needCompiler = ready && !c["ok"].boolean() && !cached;
            if (footerPass)
            {
                if (needCompiler)
                {
                    const char *label = !launcher::os::kWindows ? "Install packages"
                                        : code == "no_clang"   ? "Add the Clang tools"
                                        : code == "old_clang"  ? "Update Visual Studio"
                                                               : "Install build tools";
                    if (footerButton(footer, label, ButtonStyle::Primary, theme::icon::kDownload, !app.busy()))
                        app.startInstallTools();
                    if (!launcher::os::kWindows)
                    {
                        // the same command in a terminal: where there is no password prompt for pkexec to show
                        if (footerButton(footer, "Install in a terminal", ButtonStyle::Ghost, theme::icon::kCode, !app.busy(), false))
                            launcher::openTerminal(c["command"].str());
                    }
                    else if (code != "no_clang" && code != "old_clang")
                        ui::label(kFontBody, px(theme::kSmall), core::cutRight(footer, px(84)), theme::kFaint, "about 6 GB", ui::Align::Right);
                }
                else if (footerButton(footer, "Continue", ButtonStyle::Primary, theme::icon::kChevronRight, ready && !cpuUnsupported(app) && !cached))
                    shell.pinnedStep = -1;
                if (footerButton(footer, app.doctorRunning ? "Checking..." : "Check again", ButtonStyle::Ghost, theme::icon::kRefresh, !app.doctorRunning, false))
                    app.refresh();
                return 0;
            }
            float y = body.y;
            y = header({body.x, y, body.w, 0}, "STEP 3 OF 4", theme::kPink, "Build tools",
                       launcher::os::kWindows && c["toolchain"].str() == "msvc"
                           ? "The game is compiled on your PC with Microsoft's free C++ tools and clang. Nothing of the game "
                             "is downloaded - only the compiler."
                       : launcher::os::kWindows
                           ? "The game is compiled on your PC with clang (llvm-mingw), which the first build downloads by "
                             "itself. Nothing of the game is downloaded - only the compiler."
                           : "The game is compiled on your PC with clang and your distribution's packages. Nothing of the "
                             "game is downloaded - only the compiler and libraries.");
            if (!app.doctorDone)
            {
                checking(body, y);
                return y;
            }
            if (app.doctorFailed())
            {
                checkFailed(app, body, y);
                return y;
            }
            if (cpuUnsupported(app) && !cached)
                callout(body, y, theme::kRed, launcher::errorHelp("cpu_unsupported").title,
                        cpu["message"].str() + ". " + launcher::errorHelp("cpu_unsupported").fix);
            if (needCompiler && launcher::os::kWindows)
                callout(body, y, theme::kAmber, launcher::errorHelp(code).title,
                        c["message"].str() + ". The launcher can run Microsoft's installer with the right options ticked; "
                        "Windows asks for permission first, and the installer shows its own progress.");
            else if (needCompiler)
                callout(body, y, theme::kAmber, launcher::errorHelp(code).title,
                        c["message"].str() + ". The launcher installs them with your distribution's package manager and "
                        "asks for your password first. The command: " + c["command"].str());
            auto status = [&](bool good, bool bad) {
                return cached ? (good ? ui::Status::Good : ui::Status::Checking) : good ? ui::Status::Good : bad ? ui::Status::Failed : ui::Status::Warning;
            };
            if (cpu.isObject())
                checkRow(body, y, "#cpu", status(cpu["ok"].boolean(), true), "Processor", cpu["message"].str());
            checkRow(body, y, "#compiler", app.busy() && app.task->kind == "tools" ? ui::Status::Running : status(c["ok"].boolean(), true),
                     "Compiler", c["ok"].boolean() ? c["message"].str() : "Missing");
            checkRow(body, y, "#libraries", status(dl["ok"].boolean(), true), "Libraries",
                     dl["bundled"].boolean() ? "Bundled with this release" : dl["message"].str());
            checkRow(body, y, "#python", ui::Status::Good, "Python",
                     d["python"]["version"].str() + (d["python"]["bundled"].boolean() ? "  (bundled)" : "  (" + app.pythonText + ")"));
            checkRow(body, y, "#memory", status(d["memory"]["ok"].boolean(), false), "Memory", d["memory"]["message"].str());
            y += px(12);
            if (!dl["ok"].boolean() && !cached && launcher::os::kWindows) // Linux: git is one of the packages
            {
                if (ui::button("Get Git", {body.x, y, ui::buttonWidth("Get Git", theme::icon::kOpen), px(38)}, ButtonStyle::Secondary, theme::icon::kOpen))
                    launcher::openUrl("https://git-scm.com/download/win");
                y += px(50);
            }
            return y;
        }

        const char *optNote(const std::string &opt)
        {
            if (opt == "none")
                return "Builds faster; the game runs a little slower.";
            if (opt == "pgo")
                return "Adds about 30 minutes: the game is played once, hidden, to tune the build.";
            return "Optimised (ThinLTO). What most players want.";
        }

        float stepGame(App &app, Rect body, Rect &footer, bool footerPass)
        {
            const launcher::Json &b = app.doctor["build"];
            const bool built = b["built"].boolean(), current = b["current"].boolean();
            if (footerPass)
            {
                const char *label = current ? "Build again" : built ? "Update the build" : "Build the game";
                if (footerButton(footer, label, ButtonStyle::Primary, theme::icon::kRepair,
                                 !app.busy() && app.disc.ok && app.doctorDone && !app.doctorCached && !app.doctorFailed()))
                    app.startBuild();
                if (footerButton(footer, app.doctorRunning ? "Checking..." : "Check again", ButtonStyle::Ghost, theme::icon::kRefresh, !app.doctorRunning, false))
                    app.refresh();
                return 0;
            }
            float y = body.y;
            y = header({body.x, y, body.w, 0}, "STEP 4 OF 4", theme::kPink, "Build the game",
                       std::string("Recompiled from your disc and compiled into ") + launcher::os::kGameExe + ", on this PC.");
            if (!app.doctorDone)
            {
                checking(body, y);
                return y;
            }
            if (app.doctorFailed())
            {
                checkFailed(app, body, y);
                return y;
            }
            const char *tag = current ? "UP TO DATE" : built ? "NEEDS AN UPDATE" : "NOT BUILT YET";
            ui::pill({body.x, y, ui::pillWidth(tag), px(22)}, tag, current ? theme::kTeal : built ? theme::kAmber : theme::kDim);
            y += px(34);
            if (current)
                note(body, y, "Built from this version (" + buildSummary(app) + "). Press Play.", theme::kText);
            else if (built)
            {
                for (size_t i = 0; i < b["reasons"].size(); ++i)
                    note(body, y, "\xE2\x80\xA2  " + b["reasons"][i].str());
                note(body, y, "Updating takes minutes: only what changed is compiled again.", theme::kText);
            }
            else
                note(body, y, "The first build takes 10 to 60 minutes depending on the PC, once. The PC stays usable meanwhile, and "
                              "if it stops, building again continues where it stopped.", theme::kText);
            y += px(14);

            heading(body, y, "Build type");
            static const char *opts[] = {"lto", "none", "pgo"};
            std::string opt = app.settings.opt;
            if (opt.empty())
            {
                const std::string had = b["info"]["opt"].str();
                opt = had.rfind("none", 0) == 0 ? "none" : had.rfind("pgo", 0) == 0 ? "pgo" : "lto";
            }
            int selected = opt == "none" ? 1 : opt == "pgo" ? 2 : 0;
            if (ui::segmented("#opt", {body.x, y, std::min(body.w, px(460)), px(40)}, {"Standard", "Quick to build", "Fastest game"}, selected, !app.busy()))
            {
                app.settings.opt = opts[selected];
                app.saveSettings();
            }
            y += px(50);
            note(body, y, optNote(opts[selected]), theme::kFaint);
            y += px(10);
            note(body, y, "The game's own settings - graphics, widescreen, controls, sound - are in the game: press F2 while playing.", theme::kFaint);
            return y;
        }

        void drawStep(App &app, int step, Rect card)
        {
            const Rect inner = card.inset(px(36), 0);
            Rect body = inner;
            core::cutTop(body, px(30));
            const Rect footer = core::cutBottom(body, px(72));
            rect({card.x, footer.y, card.w, 1}, theme::ink(0.05f));
            const ui::Id scrollId = ui::id("#setup-scroll", step);
            using StepFn = float (*)(App &, Rect, Rect &, bool);
            const StepFn fn = step == kStepFolder ? stepFolder : step == kStepDisc ? stepDisc : step == kStepTools ? stepTools : stepGame;
            ui::beginScroll(scrollId, {card.x, body.y, card.w, body.h - px(6)});
            Rect unused = footer;
            const float bottom = fn(app, body, unused, false);
            ui::endScroll(scrollId, bottom + px(16));
            // the step's state, in the card's top right corner (it stays put while the card scrolls)
            ui::statusBadge(ui::id("#stepbadge", step), card.right() - px(40), card.y + px(40), px(15), stepStatus(app, step), step + 1);
            Rect row = footer.inset(0, px(14));
            fn(app, body, row, true);
            // everything ready and Setup opened from Home: a way back, beside the step's own buttons
            if (shell.setupOpen && !setupNeeded(app) && footerButton(row, "Close##setup", ButtonStyle::Secondary))
            {
                shell.setupOpen = false;
                shell.pinnedStep = -1;
            }
        }

        // The rail: the four steps' titles (Pricedown when the player has it) on a line that fills in teal as they
        // are done, and a solid box behind the step shown, as in Vice City's menus: each step has a box of its own
        // shape (four corners set off its text a little, each differently), and moving to another step carries the
        // box there while its corners travel from the old shape into the new one. The step's state is a badge on its
        // card, not here.
        void drawRail(App &app, Rect r, int shown)
        {
            const int n = kStepCount;
            const float x0 = r.x + px(40), x1 = r.right() - px(40);
            const float gap = (x1 - x0) / float(n - 1);
            const bool brand = fontFound(kFontBrand);
            const Font titleFont = brand ? kFontBrand : kFontSemibold;
            const float titleSize = brand ? px(23) : px(15.5f), lineSize = px(12);
            const float titleY = r.y + px(6), lineY = r.y + px(34);
            const float titleMid = titleY + px(13);

            // done steps fill the line in teal, on a spring
            int done = 0;
            while (done < n && stepStatus(app, done) == ui::Status::Good)
                ++done;
            ui::Widget &w = ui::widget(ui::id("#rail"));
            if (w.fresh)
                w.spring.snap(float(std::max(done - 1, 0)));
            ui::spring(w.spring, float(std::max(done - 1, 0)), 120, 0.9f);

            // each step's box: around its title and line, then its corners pushed in or out (logical units:
            // x, y of top-left, top-right, bottom-right, bottom-left; + is right / down)
            static const float kShapes[kStepCount][8] = {
                {-6, 3, 8, -4, 4, 5, -9, -2},
                {7, -5, -5, 3, 9, -3, -3, 6},
                {-8, -3, 5, 6, -4, -4, 8, 3},
                {4, 5, -9, -2, 6, 4, -5, -5},
            };
            std::string lines[kStepCount];
            Rect boxes[kStepCount];
            float textW[kStepCount];
            float shapes[kStepCount][8];
            for (int i = 0; i < n; ++i)
            {
                lines[i] = stepLine(app, i);
                textW[i] = std::max(measure(titleFont, titleSize, stepName(i)), measure(kFontBody, lineSize, lines[i]));
                const float bw = std::min(textW[i] + px(44), gap * 0.9f);
                boxes[i] = {x0 + gap * float(i) - bw * 0.5f, r.y - px(6), bw, px(66)};
                const Rect &b = boxes[i];
                const float corners[8] = {b.x, b.y, b.right(), b.y, b.right(), b.bottom(), b.x, b.bottom()};
                for (int k = 0; k < 8; ++k)
                    shapes[i][k] = corners[k] + px(kShapes[i][k]);
            }

            // the line, in pieces between the steps' text, at the titles' height
            const float lineH = px(2);
            const float fillTo = x0 + gap * w.spring.value;
            for (int i = 0; i + 1 < n; ++i)
            {
                const float a = x0 + gap * float(i) + textW[i] * 0.5f + px(18);
                const float b = x0 + gap * float(i + 1) - textW[i + 1] * 0.5f - px(18);
                if (b <= a)
                    continue;
                rect({a, titleMid - lineH * 0.5f, b - a, lineH}, theme::ink(0.08f), lineH * 0.5f);
                if (done > 0 && fillTo > a)
                    gradient({a, titleMid - lineH * 0.5f, std::min(b, fillTo) - a, lineH}, theme::kTeal,
                             mix(theme::kTeal, rgb(0xFFFFFF), 0.2f), lineH * 0.5f, Gradient::Horizontal);
            }

            // the box: where it is along the rail (a spring, no overshoot past a step), its corners blended between
            // the shapes of the two steps it is between
            ui::Widget &box = ui::widget(ui::id("#railbox"));
            const int target = std::clamp(shown, 0, n - 1);
            if (box.fresh)
                box.spring.snap(float(target));
            ui::spring(box.spring, float(target), 600, 1.0f); // there in about a quarter of a second
            const float pos = std::clamp(box.spring.value, 0.0f, float(n - 1));
            const int from = std::min(int(pos), n - 2);
            const float t = pos - float(from);
            float corners[8];
            for (int k = 0; k < 8; ++k)
                corners[k] = core::lerp(shapes[from][k], shapes[from + 1][k], t);
            quad(corners, theme::kPink);
            const float boxCx = (corners[0] + corners[2] + corners[4] + corners[6]) * 0.25f;

            for (int i = 0; i < n; ++i)
            {
                const float cx = x0 + gap * float(i);
                // the step's own box is where it takes clicks: nothing beside the rail is covered
                const Rect hit = boxes[i];
                const ui::Interaction in = ui::interact(ui::id("#railstep", i), hit, true);
                if (in.clicked)
                {
                    shell.pinnedStep = i;
                    if (i == kStepFolder)
                        shell.folderSeen = true;
                }
                // how far inside the moving box this step is: on the box its text turns white
                const float inside = core::clamp01(1.0f - std::fabs(boxCx - cx) / (gap * 0.5f));
                if (i != target && in.w->hover > 0.01f)
                    quad(shapes[i], theme::ink(0.05f * in.w->hover * (1 - inside)));
                const ui::Status status = stepStatus(app, i);
                const Color titleColor = mix(mix(theme::kDim, theme::kText, in.w->hover), theme::kOnPink, inside);
                ui::label(titleFont, titleSize, {cx - gap * 0.5f, titleY, gap, px(26)}, titleColor, stepName(i), ui::Align::Centre);
                const Color lineColor = mix(status == ui::Status::Warning ? theme::kAmber : theme::kFaint, withAlpha(theme::kOnPink, 0.88f), inside);
                ui::label(kFontBody, lineSize, {cx - gap * 0.5f, lineY, gap, px(18)}, lineColor, lines[i], ui::Align::Centre);
                if (in.w->focus > 0.01f)
                    outline(hit.inset(-px(4)), withAlpha(theme::kText, in.w->focus), px(2), px(6));
            }
        }
    }

    void drawFolderBody(Rect body, float &y)
    {
        App &app = *shell.app;
        const bool locked = app.busy() || app.gameRunning;
        // the build folder: the launcher knows it without the check
        {
            const bool custom = app.buildCustom();
            const char *tag = custom ? "CHOSEN" : "DEFAULT";
            ui::label(kFontSemibold, px(15.5f), {body.x, y, body.w, px(24)}, theme::kText, "Build folder");
            ui::pill({body.right() - ui::pillWidth(tag), y + px(1), ui::pillWidth(tag), px(22)}, tag, custom ? theme::kPink : theme::kFaint);
            y += px(28);
            note(body, y, "The built game, its settings and logs: about 1 to 3 GB.", theme::kFaint);
            y += row({body.x, y, body.w, 0}, "Location", launcher::pathText(app.build));
            if (app.doctor.isObject())
                y += row({body.x, y, body.w, 0}, "Free space",
                         gigabytes(app.doctor["folder"]["free_bytes"].num()) + " on that drive  (a build needs about " +
                             gigabytes(app.doctor["folder"]["need_bytes"].num()) + ")");
            y += px(6);
            float bx = body.x;
            const float w1 = ui::buttonWidth("Choose another folder", theme::icon::kFolder);
            if (ui::button("Choose another folder", {bx, y, w1, px(38)}, ButtonStyle::Secondary, theme::icon::kFolder, !locked))
                chooseBuildDir(app.build.parent_path());
            bx += w1 + px(8);
            if (custom)
            {
                const float w2 = ui::buttonWidth("Use the default");
                if (ui::button("Use the default", {bx, y, w2, px(38)}, ButtonStyle::Ghost, 0, !locked))
                {
                    app.startBuildDirCheck(app.root / "build");
                    shell.awaitingBuildDir = true;
                }
                bx += w2 + px(8);
            }
            if (ui::button("Open##build", {bx, y, ui::buttonWidth("Open"), px(38)}, ButtonStyle::Ghost))
                launcher::openFolder(app.build);
            y += px(48);
            if (app.gameRunning)
                note(body, y, "Quit the game to change it.", theme::kAmber);
        }
        y += px(14);
        {
            ui::label(kFontSemibold, px(15.5f), {body.x, y, body.w, px(24)}, theme::kText, "Source folder");
            y += px(28);
            note(body, y, "The launcher, its scripts and the recompiled code (0.2 GB). Move it by moving the whole folder.", theme::kFaint);
            y += row({body.x, y, body.w, 0}, "Location", launcher::pathText(app.root));
            if (app.doctor.isObject())
            {
                const launcher::Json &m = app.doctor["memory"];
                y += row({body.x, y, body.w, 0}, "PC memory", m["message"].str(), m["ok"].boolean() ? theme::kText : theme::kAmber);
            }
            y += px(6);
            const char *openLabel = launcher::os::kWindows ? "Open in Explorer" : "Open the folder";
            if (ui::button(openLabel, {body.x, y, ui::buttonWidth(openLabel, theme::icon::kOpen), px(38)}, ButtonStyle::Ghost, theme::icon::kOpen))
                launcher::openFolder(app.root);
            y += px(52);
        }
        if (!app.doctorDone || app.doctorCached)
        {
            if (!app.doctorDone)
                checking(body, y);
            return;
        }
        if (app.doctorFailed())
        {
            checkFailed(app, body, y);
            return;
        }
        const launcher::Json &f = app.doctor["folder"];
        for (size_t i = 0; i < f["warnings"].size(); ++i)
            callout(body, y, theme::kAmber, "Worth fixing", f["warnings"][i]["message"].str());
    }

    void drawSetup(Rect r)
    {
        App &app = *shell.app;
        const bool firstFrame = g_lastFrame + 1 < ui::state.frame;
        g_lastFrame = ui::state.frame;

        // which step: the one picked on the rail, else the first that isn't done (after a moment on one just done)
        int target = shell.pinnedStep >= 0 ? shell.pinnedStep : autoStep(app);
        if (firstFrame)
        {
            shell.step = target;
            shell.previousStep = -1;
            shell.stepGoodSince = -1;
        }
        if (target != shell.step && shell.pinnedStep < 0 && stepStatus(app, shell.step) == ui::Status::Good)
        {
            if (shell.stepGoodSince < 0)
                shell.stepGoodSince = ui::state.time;
            if (ui::state.time - shell.stepGoodSince < 0.9)
            {
                target = shell.step;
                ui::keepAnimating();
            }
        }
        if (target != shell.step)
        {
            g_direction = target > shell.step ? 1 : -1;
            shell.previousStep = shell.step;
            shell.step = target;
            shell.stepSince = ui::state.time;
            shell.stepGoodSince = -1;
            shell.showDrives = false;
        }

        const double since = shell.mode == Mode::Setup ? shell.modeSince : ui::state.time;
        Rect area = r.inset(px(40), 0);
        core::cutTop(area, px(26));
        core::cutBottom(area, px(28));
        const float width = std::min(area.w, px(800));
        area = {area.cx() - width * 0.5f, area.y, width, area.h};

        pushReveal(since, 0, 10);
        drawRail(app, core::cutTop(area, px(64)), shell.step);
        popReveal();
        core::cutTop(area, px(14));

        // the card; the step inside slides in from the side it comes from
        pushReveal(since, 2, 18);
        ui::card(area);
        render::pushClip(area);
        const float t = ui::state.motion > 0 ? float((ui::state.time - shell.stepSince) / 0.42) : 1.0f;
        if (t < 1 && shell.previousStep >= 0 && shell.previousStep != shell.step)
        {
            const float e = core::easeOutCubic(t);
            ui::keepAnimating();
            ui::pushDisabled();
            render::pushOpacity(1 - core::clamp01(t * 2.2f));
            render::pushOffset(-g_direction * e * px(60), 0);
            drawStep(app, shell.previousStep, area);
            render::popOffset();
            render::popOpacity();
            ui::popDisabled();
            render::pushOpacity(core::clamp01((t - 0.15f) / 0.6f));
            render::pushOffset(g_direction * (1 - e) * px(60), 0);
            drawStep(app, shell.step, area);
            render::popOffset();
            render::popOpacity();
        }
        else
            drawStep(app, shell.step, area);
        render::popClip();
        popReveal();
    }
}
