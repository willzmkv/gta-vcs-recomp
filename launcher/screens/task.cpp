// Task: a build, the installer, a disc copy, an update, a report or a restore takes the window. A large percentage
// whose digits roll, a bar with a sheen, the time so far and left, the stages as a timeline whose spinners turn into
// checks, and the log in a drawer that rises from the bottom. A failure slides in a card with what went wrong and what
// to do; success says so and offers what's next.
#include "shell.h"

#include "model/system.h"
#include "platform/platform.h"

#include <cmath>

namespace screens
{
    using namespace render;
    using core::mix;
    using core::withAlpha;
    using core::rgb;
    using ui::ButtonStyle;

    namespace
    {
        core::Spring g_drawer;
        double g_finishedAt = -1;
        const launcher::Task *g_seenTask = nullptr;

        std::string doneMessage(const launcher::Task &t, double elapsed)
        {
            if (t.kind == "tools")
                return t.result["message"].str().empty() ? "The build tools are installed." : t.result["message"].str();
            if (t.kind == "update")
                return t.result["restart"].boolean()
                           ? "Updated to " + t.result["version"].str() + ". Restart the launcher to use it, then update the build."
                           : "You already have the newest release (" + t.result["version"].str() + ").";
            if (t.kind == "install")
                return "The game's files are in place.";
            if (t.kind == "rip")
                return "Copied to " + t.result["iso"].str() + ".";
            if (t.kind == "report")
                return "The report is ready: a zip of the logs, no game files, no saves.";
            if (t.kind == "restore")
                return "Saves restored. The card as it was is in a backup too.";
            return "Done in " + duration(elapsed) + "." + (t.kind == "build" ? " The game is ready." : "");
        }

        // one row of the timeline
        float stageRow(const launcher::Task &t, size_t i, Rect r, double now, bool failed)
        {
            const launcher::Task::Stage &s = t.stages[i];
            const bool last = i + 1 == t.stages.size();
            const bool running = !t.finished && last;
            const bool broke = failed && last;
            ui::Status status = ui::Status::Good;
            if (running)
                status = ui::Status::Running;
            else if (broke)
                status = ui::Status::Failed;
            else if (t.cancelled && last)
                status = ui::Status::Missing;
            const float h = px(36);
            // a line joins the badges
            if (i > 0)
                rect({r.x + px(12) - px(1), r.y - px(12), px(2), px(12) + px(2)}, theme::ink(0.07f));
            ui::statusBadge(ui::id("#stage", int(i)), r.x + px(12), r.y + h * 0.5f, px(10), status);
            const Color c = running ? theme::kText : broke ? theme::kRed : theme::kDim;
            ui::label(running ? kFontSemibold : kFontBody, px(theme::kBody), {r.x + px(36), r.y, r.w - px(120), h}, c, s.title);
            if (s.end != 0 || running)
                ui::label(kFontBody, px(theme::kSmall), {r.right() - px(90), r.y, px(90), h}, theme::kFaint,
                          duration(std::max(0.0, (s.end != 0 ? s.end : now) - s.start)), ui::Align::Right);
            return h + px(12);
        }
    }

    void drawTask(Rect r)
    {
        App &app = *shell.app;
        if (!app.task)
            return;
        launcher::Task &t = *app.task;
        const double since = shell.mode == Mode::Task ? shell.modeSince : ui::state.time;
        const double now = ui::state.wall;
        const bool failed = t.finished && !t.ok() && !t.cancelled;
        if (g_seenTask != &t)
        {
            g_seenTask = &t;
            g_finishedAt = t.finished ? ui::state.time - 10 : -1;
        }
        if (t.finished && g_finishedAt < 0)
            g_finishedAt = ui::state.time;
        const double elapsed = (t.finished ? t.endTime : now) - t.startTime;

        // the log drawer takes the bottom of the window when it's open
        const bool drawerOpen = shell.showLog || failed;
        ui::spring(g_drawer, drawerOpen ? 1.0f : 0.0f, 220, 0.86f);
        const float drawerH = std::min(r.h * 0.42f, px(300));
        const float drawerY = r.bottom() - drawerH * core::clamp01(g_drawer.value);

        const float width = std::min(r.w - px(80), px(820));
        Rect col{r.cx() - width * 0.5f, r.y + px(34), width, drawerY - r.y - px(34)};
        float y = col.y;

        pushReveal(since, 0);
        const char *eyebrow = !t.finished ? "IN PROGRESS" : t.cancelled ? "STOPPED" : failed ? "DIDN'T WORK" : "DONE";
        const Color eyebrowColor = !t.finished ? theme::kPink : failed ? theme::kRed : t.cancelled ? theme::kDim : theme::kTeal;
        y = header({col.x, y, col.w, 0}, eyebrow, eyebrowColor, t.title, "");
        popReveal();
        y -= px(6);

        if (!t.finished)
        {
            pushReveal(since, 1);
            const std::string stageTitle = t.stages.empty() ? "Starting" : t.stages.back().title;
            if (t.total > 0)
            {
                const int pct = int(100.0 * t.done / t.total);
                const float size = fontFound(kFontBrand) ? px(80) : px(64);
                const FontMetrics m = metrics(kFontBrand, size); // Pricedown when installed: GTA's money counter
                const float base = y + m.ascent;
                const float numW = ui::rollingNumber(ui::id("#pct"), kFontBrand, size, col.x - px(2), base, theme::kText, pct);
                drawText(kFontBrand, size * 0.5f, col.x + numW + px(4), base, theme::kDim, "%");
                const float tx = col.x + numW + px(44);
                ui::label(kFontSemibold, px(17), {tx, base - px(46), col.right() - tx, px(26)}, theme::kText, stageTitle);
                std::string line = duration(elapsed) + " so far";
                if (t.note == "compiling")
                    line += "  \xC2\xB7  " + std::to_string(t.done) + " of " + std::to_string(t.total) + " steps";
                if (t.eta >= 0)
                    line += "  \xC2\xB7  about " + duration(t.eta) + " left in this step";
                else if (t.note == "linking") // the link's progress is its time against the last link's
                    line += "  \xC2\xB7  longer than the last time";
                ui::label(kFontBody, px(theme::kSmall), {tx, base - px(18), col.right() - tx, px(20)}, theme::kFaint, line);
                y = base + m.descent + px(14);
                ui::progressBar(ui::id("#bar"), {col.x, y, col.w, px(8)}, float(t.done) / float(t.total), theme::kPink, theme::kPinkHi);
            }
            else
            {
                ui::label(kFontSemibold, px(22), {col.x, y, col.w, px(34)}, theme::kText, stageTitle);
                y += px(42);
                std::string line = duration(elapsed) + " so far";
                if (t.note == "linking") // the first optimised link: nothing to estimate it from
                    line += "  \xC2\xB7  one long step: the whole game is optimised at once";
                ui::label(kFontBody, px(theme::kSmall), {col.x, y, col.w, px(20)}, theme::kFaint, line);
                y += px(30);
                ui::progressBar(ui::id("#bar"), {col.x, y, col.w, px(8)}, -1, theme::kPink, theme::kPinkHi);
            }
            y += px(36);
            popReveal();
            ui::wakeAt(std::floor(now) + 1.0); // the clocks
        }
        else if (failed)
        {
            // the card slides in and shakes once
            const launcher::ErrorHelp &help = launcher::errorHelp(t.errorCode);
            const float at = float(ui::state.time - g_finishedAt);
            const float shake = ui::state.motion > 0 && at < 0.5f ? std::sin(at * 46.0f) * (1 - at / 0.5f) * px(7) : 0.0f;
            if (ui::state.motion > 0 && at < 0.5f)
                ui::keepAnimating();
            const float textW = col.w - px(64);
            const float h = px(26) + px(28) + (t.errorMessage.empty() ? 0 : ui::paragraphHeight(kFontBody, px(theme::kBody), textW, t.errorMessage) + px(8)) +
                            ui::paragraphHeight(kFontBody, px(15), textW, help.fix) + px(78);
            pushReveal(g_finishedAt, 0, 16);
            render::pushOffset(shake, 0);
            const Rect box{col.x, y, col.w, h};
            ui::card(box, theme::kErrorFill, theme::kRed);
            float by = box.y + px(24);
            drawIcon(kFontIcons, px(18), box.x + px(32), by + px(12), theme::kRed, theme::icon::kWarning);
            ui::label(kFontSemibold, px(18), {box.x + px(56), by, textW, px(26)}, theme::kText, help.title);
            by += px(34);
            if (!t.errorMessage.empty())
                by += ui::paragraph(kFontBody, px(theme::kBody), {box.x + px(56), by, textW - px(24), 0}, theme::kDim, t.errorMessage) + px(8);
            by += ui::paragraph(kFontBody, px(15), {box.x + px(56), by, textW - px(24), 0}, theme::kText, help.fix);
            Rect actions{box.x + px(48), box.bottom() - px(58), box.w - px(72), px(40)};
            const bool retry = t.kind == "build" || t.kind == "tools";
            auto cut = [&](std::string_view text, char32_t icon, ButtonStyle style) {
                const float w = ui::buttonWidth(text, icon);
                const Rect b = core::cutLeft(actions, w);
                core::cutLeft(actions, px(8));
                return ui::button(text, b, style, icon);
            };
            bool again = false, report = false, close = false;
            if (retry)
                again = cut("Try again", theme::icon::kRefresh, ButtonStyle::Primary) ||
                        (platform::input.key(platform::kKeyEnter) && !ui::state.focusVisible && ui::inputAllowed());
            if (!t.errorLog.empty() && cut("Open the log", theme::icon::kDocument, ButtonStyle::Secondary))
                launcher::openFile(app.root / launcher::pathFrom(t.errorLog));
            report = cut("Make a report", theme::icon::kBug, ButtonStyle::Ghost);
            close = cut(retry ? "Close" : "OK", 0, ButtonStyle::Ghost);
            render::popOffset();
            popReveal();
            y += h + px(24);
            if (again || report || close)
            {
                const std::string kind = t.kind;
                app.dismissTask();
                if (again && kind == "build")
                    app.startBuild();
                else if (again && kind == "tools")
                    app.startInstallTools();
                else if (report)
                    app.startReport();
                return; // the task is gone
            }
        }
        else
        {
            // done (or stopped): a check that draws itself, what happened, and what's next
            pushReveal(g_finishedAt, 0, 12);
            const Rect box{col.x, y, col.w, px(116)};
            ui::card(box, t.cancelled ? 0 : theme::kDoneFill);
            ui::statusBadge(ui::id("#donebadge"), box.x + px(46), box.cy(), px(20), t.cancelled ? ui::Status::Missing : ui::Status::Good);
            const std::string message = t.cancelled ? "Stopped. Starting again continues where it stopped." : doneMessage(t, elapsed);
            ui::paragraph(kFontSemibold, px(16), {box.x + px(88), box.y + px(24), box.w - px(300), 0}, theme::kText, message);
            Rect actions{box.x + px(88), box.bottom() - px(52), box.w - px(110), px(38)};
            bool ok = false;
            const bool enter = platform::input.key(platform::kKeyEnter) && !ui::state.focusVisible && ui::inputAllowed();
            if (!t.cancelled && t.kind == "build" && canPlay(app))
            {
                const float w = ui::buttonWidth("Play", theme::icon::kPlay);
                if (ui::button("Play##done", core::cutRight(actions, w), ButtonStyle::Primary, theme::icon::kPlay) || enter)
                {
                    play(app);
                    popReveal();
                    return;
                }
                core::cutRight(actions, px(8));
                ok = ui::button("Close##done", core::cutRight(actions, ui::buttonWidth("Close")), ButtonStyle::Ghost);
            }
            else if (!t.cancelled && t.kind == "update" && t.result["restart"].boolean())
            {
                // the new release is in place, this launcher is the old one: it starts the new one and closes
                const float w = ui::buttonWidth("Restart", theme::icon::kRefresh);
                if (ui::button("Restart##done", core::cutRight(actions, w), ButtonStyle::Primary, theme::icon::kRefresh) || enter)
                {
                    restartLauncher(app);
                    popReveal();
                    return;
                }
                core::cutRight(actions, px(8));
                ok = ui::button("Later##done", core::cutRight(actions, ui::buttonWidth("Later")), ButtonStyle::Ghost);
            }
            else
            {
                ok = ui::button("OK##done", core::cutRight(actions, ui::buttonWidth("OK") + px(20)), ButtonStyle::Primary) || enter;
                core::cutRight(actions, px(8));
                if (t.kind == "report" && !t.cancelled &&
                    ui::button("Show the report", core::cutRight(actions, ui::buttonWidth("Show the report", theme::icon::kFolder)),
                               ButtonStyle::Secondary, theme::icon::kFolder))
                    launcher::revealFile(launcher::pathFrom(t.result["path"].str()));
                if (t.cancelled && (t.kind == "build" || t.kind == "tools"))
                {
                    if (ui::button("Start again", core::cutRight(actions, ui::buttonWidth("Start again", theme::icon::kRefresh)),
                                   ButtonStyle::Secondary, theme::icon::kRefresh))
                    {
                        const std::string kind = t.kind;
                        app.dismissTask();
                        kind == "build" ? app.startBuild() : app.startInstallTools();
                        popReveal();
                        return;
                    }
                }
            }
            popReveal();
            y += box.h + px(24);
            if (ok)
            {
                app.dismissTask();
                return;
            }
        }

        // the stages as a timeline, scrolling if there are many
        if (!t.stages.empty())
        {
            pushReveal(since, 2);
            const float listBottom = drawerY - px(84);
            if (listBottom > y + px(40))
            {
                const Rect view{col.x, y, col.w, listBottom - y};
                const ui::Id scroll = ui::id("#stages");
                ui::beginScroll(scroll, view, !t.finished);
                float sy = view.y + px(4);
                for (size_t i = 0; i < t.stages.size(); ++i)
                    sy += stageRow(t, i, {view.x, sy, view.w - px(14), 0}, now, failed);
                ui::endScroll(scroll, sy);
            }
            popReveal();
        }

        // the bar of actions above the drawer
        pushReveal(since, 3);
        {
            Rect bar{col.x, drawerY - px(64), col.w, px(40)};
            const char *logLabel = drawerOpen ? "Hide details" : "Show details";
            if (!failed && ui::button(logLabel, core::cutLeft(bar, ui::buttonWidth(logLabel, theme::icon::kList)), ButtonStyle::Ghost, theme::icon::kList))
                shell.showLog = !shell.showLog;
            if (!t.finished && t.kind != "install") // a few seconds, redone at the next start: nothing to cancel
            {
                const char *cancel = t.cancelled ? "Stopping..." : "Cancel";
                if (ui::button(cancel, core::cutRight(bar, ui::buttonWidth(cancel, theme::icon::kStop)), ButtonStyle::Secondary, theme::icon::kStop, !t.cancelled))
                    openDialog(Dialog::CancelTask);
            }
        }
        popReveal();

        // the drawer
        if (g_drawer.value > 0.01f)
        {
            const Rect drawer{r.x, drawerY, r.w, r.bottom() - drawerY + px(20)};
            shadow(drawer.offset(0, -px(4)), theme::shade(0.7f * core::clamp01(g_drawer.value)), px(16), px(16));
            rectEx(drawer, theme::kBase, theme::kNight, {px(16), px(16), 0, 0}, theme::ink(0.06f), ui::hairline());
            rect({drawer.cx() - px(18), drawer.y + px(8), px(36), px(4)}, theme::ink(0.12f), px(2));
            const Rect inner{col.x - px(12), drawer.y + px(22), col.w + px(24), r.bottom() - drawer.y - px(36)};
            if (inner.h > px(40))
                ui::logView(ui::id("#log"), inner, t.log);
        }
    }
}
