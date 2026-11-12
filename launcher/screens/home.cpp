// Home: everything is ready. The name over the setting sun, one big button for the next thing to do (Play, Update the
// build, Check again), what was built under it, and three quiet cards along the bottom: the setup, the saves, updates.
#include "shell.h"

#include "model/system.h"
#include "platform/platform.h"

#include <cctype>
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
        // a translucent card over the backdrop; clicking it does `action`
        bool glassCard(std::string_view idLabel, Rect r, char32_t icon, Color iconColor, std::string_view title, std::string_view line)
        {
            const ui::Interaction in = ui::interact(ui::id(idLabel), r, true);
            ui::Widget &w = *in.w;
            const Rect box = r.offset(0, -w.hover * px(2));
            shadow(box.offset(0, px(10)), theme::shade(0.45f + 0.15f * w.hover), px(20), px(theme::kRadius));
            rectEx(box, withAlpha(mix(theme::kSurface, theme::kSurfaceHi, w.hover), 0.78f), withAlpha(theme::kSurface, 0.70f),
                   px(theme::kRadius), theme::ink(0.07f + 0.05f * w.hover), ui::hairline());
            circle(box.x + px(34), box.y + px(36), px(16), withAlpha(iconColor, 0.14f));
            drawIcon(kFontIcons, px(15), box.x + px(34), box.y + px(36), iconColor, icon);
            ui::label(kFontSemibold, px(theme::kBody), {box.x + px(60), box.y + px(18), box.w - px(80), px(22)}, theme::kText, title);
            ui::label(kFontBody, px(theme::kSmall), {box.x + px(60), box.y + px(40), box.w - px(80), px(20)}, theme::kDim, line);
            drawIcon(kFontIcons, px(11), box.right() - px(22), box.y + px(36), withAlpha(theme::kDim, 0.4f + 0.6f * w.hover), theme::icon::kChevronRight);
            if (w.focus > 0.01f)
                outline(box.inset(-px(3)), withAlpha(theme::kPink, w.focus), px(2), px(theme::kRadius + 3));
            return in.clicked;
        }

        // a playing indicator in place of the button and the time played. Still: the launcher draws once a second
        // while the game plays, never continuously (the game wants the GPU)
        void playing(App &app, Rect r)
        {
            rectEx(r, withAlpha(theme::kPink, 0.12f), withAlpha(theme::kPink, 0.08f), px(14), withAlpha(theme::kPink, 0.35f), ui::hairline());
            const float cx = r.x + px(28), cy = r.cy();
            shadow({cx - px(5), cy - px(5), px(10), px(10)}, withAlpha(theme::kPink, 0.6f), px(4), px(5));
            circle(cx, cy, px(5), theme::kPink);
            const double played = ui::state.wall - app.gameStart;
            ui::label(kFontSemibold, px(17), {r.x + px(48), r.y, r.w - px(56), r.h}, theme::kText, "Playing  \xC2\xB7  " + duration(played));
        }
    }

    void drawHome(Rect r)
    {
        App &app = *shell.app;
        const double since = shell.mode == Mode::Home ? shell.modeSince : ui::state.time;
        const float left = r.x + std::max(px(56), r.w * 0.07f);
        const float colW = std::min(px(560), r.w * 0.55f);
        float y = r.y + std::max(px(40), r.h * 0.17f);

        pushReveal(since, 0);
        ui::eyebrow(left, y, theme::kPink, "GRAND THEFT AUTO");
        popReveal();
        y += px(24);
        pushReveal(since, 1);
        {
            // the name in Pricedown when the player has it (kFontBrand), else in the display font
            const float size = fontFound(kFontBrand) ? px(66) : px(58);
            const FontMetrics m = metrics(kFontBrand, size);
            // a soft glow under the name
            shadow({left, y + m.ascent * 0.35f, measure(kFontBrand, size, "Vice City Stories"), m.ascent * 0.6f},
                   withAlpha(theme::kPink, 0.16f), px(26), px(10));
            drawText(kFontBrand, size, left - px(2), y + m.ascent, theme::kText, "Vice City Stories");
            y += m.ascent + m.descent + px(4);
        }
        popReveal();
        pushReveal(since, 2);
        ui::label(kFontBody, px(16), {left, y, colW, px(26)}, theme::kDim, "The PS2 classic, recompiled for this PC.");
        popReveal();
        y += px(48);

        // the one thing to do next
        pushReveal(since, 3);
        // --demo stale: the build out of date, as after a change to the source (screenshots)
        const bool staleDemo = shell.options.demo == "stale" && app.doctorDone && !app.doctorCached;
        const std::string next = staleDemo ? "build" : app.next();
        const Rect big{left, y, px(232), px(56)};
        // the big button's label is the other place for Pricedown
        const float heroSize = fontFound(kFontBrand) ? px(31) : px(18); // Pricedown is small on its body
        // as wide as its label needs (Pricedown at that size is wide), never narrower than the usual 232
        auto fit = [&](std::string_view label, char32_t icon) {
            return Rect{big.x, big.y, std::max(big.w, ui::buttonWidth(label, icon, heroSize, kFontBrand) + px(12)), big.h};
        };
        bool primary = false;
        std::string below;
        Color belowColor = theme::kFaint;
        if (app.gameRunning)
        {
            playing(app, {big.x, big.y, px(300), big.h});
            below = "The launcher comes back when the game ends.";
        }
        else if (!app.doctorDone || app.doctorCached)
        {
            // the last answer is on screen; this run's check is still out: nothing to press yet
            const Rect b = fit("Play", theme::icon::kPlay);
            ui::button("Play", b, ButtonStyle::Primary, theme::icon::kPlay, false, heroSize, kFontBrand);
            ui::spinner(b.right() + px(26), b.cy(), px(8), theme::kDim, px(2));
            ui::label(kFontBody, px(theme::kBody), {b.right() + px(44), b.y, px(200), b.h}, theme::kDim, "Checking");
            below = app.doctorCached ? buildSummary(app) : "";
        }
        else if (app.doctorFailed())
        {
            const char *label = app.doctorRunning ? "Checking..." : "Check again";
            primary = ui::button(label, fit(label, theme::icon::kRefresh), ButtonStyle::Primary, theme::icon::kRefresh,
                                 !app.doctorRunning, heroSize, kFontBrand);
            if (primary || (platform::input.key(platform::kKeyEnter) && !ui::state.focusVisible && ui::inputAllowed()))
                app.refresh();
            below = "The check of this PC gave no answer.";
            belowColor = theme::kRed;
        }
        else if (next == "build")
        {
            // an update is offered, never forced: Play stays the big button (the game that is built still runs), the
            // update a quieter one under it (the user's choice, 2026-11-01)
            primary = ui::button("Play", fit("Play", theme::icon::kPlay), ButtonStyle::Primary, theme::icon::kPlay, true, heroSize, kFontBrand);
            if (primary || (platform::input.key(platform::kKeyEnter) && !ui::state.focusVisible && ui::inputAllowed()))
                play(app);
            // the update says it is new with a badge; why, in its tooltip
            y += big.h + px(14);
            constexpr const char *kUpdate = "Update the build";
            const Rect update{left, y, ui::buttonWidth(kUpdate, theme::icon::kRepair, 0, kFontSemibold, "NEW"), px(40)};
            if (ui::button(kUpdate, update, ButtonStyle::Secondary, theme::icon::kRepair, !app.busy(), 0, kFontSemibold, "NEW"))
                app.startBuild();
            const launcher::Json &reasons = app.doctor["build"]["reasons"];
            std::string why = staleDemo ? "the source changed since the last build" : reasons.size() ? reasons[0].str() : "";
            if (!why.empty())
                why[0] = char(std::toupper(static_cast<unsigned char>(why[0])));
            ui::tooltip(ui::id(kUpdate), update, why.empty() ? "A newer build is ready to make." : why + ": updating builds the game again.");
            y += px(40) - big.h; // the lines under it follow the smaller button
            below = buildSummary(app);
        }
        else if (next == "play")
        {
            primary = ui::button("Play", fit("Play", theme::icon::kPlay), ButtonStyle::Primary, theme::icon::kPlay, true, heroSize, kFontBrand);
            if (primary || (platform::input.key(platform::kKeyEnter) && !ui::state.focusVisible && ui::inputAllowed()))
                play(app);
            below = buildSummary(app);
        }
        else
        {
            if (ui::button("Continue setup", fit("Continue setup", theme::icon::kChevronRight), ButtonStyle::Primary, theme::icon::kChevronRight, true, heroSize, kFontBrand))
                openSetup(-1);
        }
        y += big.h + px(16);
        if (!below.empty())
            ui::label(kFontBody, px(theme::kSmall), {left, y, colW, px(20)}, belowColor, below);
        popReveal();
        y += px(40);

        // the game ended with an error, or the check failed: say why, under the button
        if (app.gameEnded && app.gameExit != 0 && !shell.gameErrorDismissed && !app.gameRunning)
        {
            pushReveal(since, 4);
            const std::string text = app.gameExitText();
            const float h = ui::paragraphHeight(kFontBody, px(theme::kBody), colW - px(40), text) + px(70);
            const Rect box{left, y, colW, h};
            ui::card(box, theme::kErrorFill, theme::kRed);
            ui::paragraph(kFontBody, px(theme::kBody), {box.x + px(22), box.y + px(16), box.w - px(40), 0}, theme::kText, text);
            float bx = box.x + px(16);
            const float by = box.bottom() - px(44);
            const float w1 = ui::buttonWidth("Open the log", theme::icon::kDocument);
            if (ui::button("Open the log", {bx, by, w1, px(34)}, ButtonStyle::Ghost, theme::icon::kDocument))
                launcher::openFile(app.gameLog);
            bx += w1 + px(6);
            if (ui::button("Dismiss", {bx, by, ui::buttonWidth("Dismiss"), px(34)}, ButtonStyle::Ghost))
                shell.gameErrorDismissed = true;
            popReveal();
        }
        else if (app.doctorFailed() && !app.doctorError.empty())
        {
            pushReveal(since, 4);
            const float h = std::min(px(160), ui::paragraphHeight(kFontMono, px(12), colW - px(40), app.doctorError, 1.4f) + px(32));
            const Rect box{left, y, colW, h};
            ui::card(box, theme::kErrorFill, theme::kRed);
            render::pushClip(box.inset(px(4)));
            ui::paragraph(kFontMono, px(12), {box.x + px(22), box.y + px(16), box.w - px(40), 0}, theme::kDim, app.doctorError, 1.4f);
            render::popClip();
            popReveal();
        }

        // the cards along the bottom
        const float cardsH = px(74);
        const float gap = px(16);
        Rect strip{left, r.bottom() - px(36) - cardsH, r.right() - left - std::max(px(56), r.w * 0.07f), cardsH};
        if (strip.y < y + px(12))
            return; // a short window: the cards make room for the essentials
        const float cw = (strip.w - 2 * gap) / 3;
        pushReveal(since, 5, 20);
        {
            const ui::Status status = gameStatus(app);
            int good = 0;
            for (int i = 0; i < kStepCount; ++i)
                good += stepStatus(app, i) == ui::Status::Good ? 1 : 0;
            const std::string line = good == kStepCount ? "Everything is ready" : std::to_string(good) + " of 4 steps ready  \xC2\xB7  " + gameLine(app);
            if (glassCard("#card-setup", {strip.x, strip.y, cw, cardsH}, theme::icon::kRepair,
                          status == ui::Status::Good ? theme::kTeal : theme::kAmber, "Setup", line))
                openSetup(-1);
        }
        popReveal();
        pushReveal(since, 6, 20);
        {
            std::string line = "Your memory card, backed up on demand";
            if (app.backups.size())
                line = "Last backup " + app.backups[0]["name"].str();
            if (glassCard("#card-saves", {strip.x + cw + gap, strip.y, cw, cardsH}, theme::icon::kSave, theme::kPink, "Saves", line))
                openSheet(Sheet::Saves);
        }
        popReveal();
        pushReveal(since, 7, 20);
        {
            const bool newer = app.updateInfo["newer"].boolean();
            const bool installed = !app.updatedTo.empty(); // put in place, the restart put off
            const std::string line = installed ? "Version " + app.updatedTo + " is installed"
                                     : newer   ? "Version " + app.updateInfo["latest"].str() + " is available"
                                               : "Version " + app.sourceVersion;
            if (glassCard("#card-updates", {strip.x + 2 * (cw + gap), strip.y, cw, cardsH},
                          installed ? theme::icon::kRefresh : newer ? theme::icon::kDownload : theme::icon::kSync,
                          installed ? theme::kTeal : newer ? theme::kPink : theme::kDim,
                          installed ? "Restart to finish" : newer ? "Update available" : "Updates", line))
                openSheet(Sheet::Settings);
        }
        popReveal();
        if (app.gameRunning)
            ui::wakeAt(std::floor(ui::state.wall) + 1.0); // the clock on the playing indicator
    }
}
