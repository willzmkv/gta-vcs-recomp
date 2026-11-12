// Sheets: Saves, Settings, Help and the build folder slide in from the right over a dimmed window, on a spring.
// Escape, the scrim or the title bar's button close them.
#include "shell.h"

#include "model/os.h"
#include "model/system.h"
#include "platform/platform.h"

namespace screens
{
    using namespace render;
    using core::alphaOf;
    using core::mix;
    using core::withAlpha;
    using core::rgb;
    using ui::ButtonStyle;

    namespace
    {
        void section(Rect r, float &y, std::string_view title)
        {
            ui::eyebrow(r.x, y, theme::kFaint, title);
            y += px(24);
        }

        void text(Rect r, float &y, std::string_view t, Color c = theme::kDim)
        {
            y += ui::paragraph(kFontBody, px(theme::kBody), {r.x, y, r.w, 0}, c, t) + px(8);
        }

        bool action(Rect r, float &y, std::string_view label, ButtonStyle style, char32_t icon = 0, bool enabled = true)
        {
            const bool clicked = ui::button(label, {r.x, y, ui::buttonWidth(label, icon), px(38)}, style, icon, enabled);
            y += px(48);
            return clicked;
        }

        void divider(Rect r, float &y)
        {
            y += px(10);
            rect({r.x, y, r.w, 1}, theme::ink(0.06f));
            y += px(22);
        }

        float saves(App &app, Rect r)
        {
            float y = r.y;
            if (!shell.backupsAsked && !app.busy())
            {
                shell.backupsAsked = true;
                app.startBackupList();
            }
            section(r, y, "MEMORY CARD");
            text(r, y, "Your saves are kept apart from the game, so updates and rebuilds never touch them.");
            y += row({r.x, y, r.w, 0}, "Folder", launcher::pathText(app.savesFolder()));
            y += px(6);
            const float w1 = ui::buttonWidth("Back up now", theme::icon::kSave);
            if (ui::button("Back up now", {r.x, y, w1, px(38)}, ButtonStyle::Primary, theme::icon::kSave, !app.busy()))
            {
                app.startBackup();
                shell.backupsAsked = false; // the list again afterwards
            }
            if (ui::button("Open the folder", {r.x + w1 + px(8), y, ui::buttonWidth("Open the folder", theme::icon::kFolder), px(38)},
                           ButtonStyle::Ghost, theme::icon::kFolder))
                launcher::openFolder(app.savesFolder());
            y += px(48);
            divider(r, y);

            section(r, y, "BACKUPS");
            if (app.busy() && (app.task->kind == "backup_list" || app.task->kind == "backup"))
            {
                ui::spinner(r.x + px(9), y + px(11), px(7), theme::kDim, px(1.8f));
                ui::label(kFontBody, px(theme::kBody), {r.x + px(26), y, r.w, px(22)}, theme::kDim, "Loading...");
                y += px(34);
            }
            else if (!app.backups.size())
                text(r, y, "None yet. A backup is a zip of the card, with the order the game needs.", theme::kFaint);
            else
                for (size_t i = 0; i < app.backups.size(); ++i)
                {
                    const launcher::Json &b = app.backups[i];
                    const std::string name = b["name"].str();
                    const Rect item{r.x - px(10), y, r.w + px(20), px(56)};
                    ui::pushId(int(i));
                    const bool over = ui::mouseOver(item);
                    rect(item, theme::ink(over ? 0.035f : 0.0f), px(10));
                    drawIcon(kFontIcons, px(15), item.x + px(26), item.cy(), theme::kPink, theme::icon::kSave);
                    ui::label(kFontSemibold, px(theme::kBody), {item.x + px(48), item.y + px(8), item.w - px(170), px(22)}, theme::kText, name);
                    ui::label(kFontBody, px(theme::kSmall), {item.x + px(48), item.y + px(29), item.w - px(170), px(20)}, theme::kFaint, b["saves"].str());
                    const float bw = ui::buttonWidth("Restore", theme::icon::kUndo);
                    if (ui::button("Restore", {item.right() - bw - px(10), item.y + px(10), bw, px(36)}, ButtonStyle::Ghost, theme::icon::kUndo,
                                   !app.busy() && !app.gameRunning))
                    {
                        shell.restoreName = name;
                        openDialog(Dialog::Restore);
                    }
                    ui::popId();
                    y += item.h + px(4);
                }
            if (app.gameRunning)
                text(r, y, "Quit the game to restore a backup.", theme::kAmber);
            y += px(4);
            if (action(r, y, "Open the backups folder", ButtonStyle::Ghost, theme::icon::kFolder))
                launcher::openFolder(app.savesFolder() / "backups");
            return y;
        }

        float settings(App &app, Rect r)
        {
            float y = r.y;
            bool changed = false;
            section(r, y, "WHEN THE GAME STARTS");
            const char *values[] = {"minimize", "close", "stay"};
            const char *titles[] = {"Minimize the launcher", "Close the launcher", "Keep the launcher open"};
            const char *details[] = {"It comes back when the game ends", "", ""};
            for (int i = 0; i < 3; ++i)
            {
                const float h = details[i][0] ? px(56) : px(44);
                if (ui::choiceRow(values[i], {r.x - px(10), y, r.w + px(20), h}, titles[i], details[i], app.settings.afterLaunch == values[i]))
                {
                    app.settings.afterLaunch = values[i];
                    changed = true;
                }
                y += h + px(2);
            }
            divider(r, y);

            section(r, y, "UPDATES");
            if (!app.canUpdate())
            {
                // a Linux clone (an AppImage updates itself): git
                y += row({r.x, y, r.w, 0}, "This version", app.sourceVersion);
                text(r, y, "Update this folder with git pull, then build again (Update the build on Home).", theme::kFaint);
            }
            else if (!app.updatedTo.empty())
            {
                // updated, the restart put off ("Later"): this window is still the old launcher
                const std::string tag = "VERSION " + app.updatedTo + " INSTALLED";
                ui::pill({r.x, y, ui::pillWidth(tag), px(22)}, tag, theme::kTeal);
                y += px(34);
                text(r, y, "Restart the launcher to use the new version, then update the build.", theme::kFaint);
                if (action(r, y, "Restart", ButtonStyle::Primary, theme::icon::kRefresh))
                    restartLauncher(app);
            }
            else if (app.updateInfo["newer"].boolean())
            {
                const std::string tag = "VERSION " + app.updateInfo["latest"].str() + " AVAILABLE";
                ui::pill({r.x, y, ui::pillWidth(tag), px(22)}, tag, theme::kPink);
                y += px(34);
                const float w1 = ui::buttonWidth("Update now", theme::icon::kDownload);
                if (ui::button("Update now", {r.x, y, w1, px(38)}, ButtonStyle::Primary, theme::icon::kDownload, !app.busy() && !app.gameRunning))
                {
                    shell.sheet = Sheet::None;
                    app.startUpdate();
                }
                if (ui::button("What's new", {r.x + w1 + px(8), y, ui::buttonWidth("What's new", theme::icon::kOpen), px(38)}, ButtonStyle::Ghost, theme::icon::kOpen))
                    launcher::openUrl(app.updateInfo["url"].str());
                y += px(52);
            }
            else
                y += row({r.x, y, r.w, 0}, "This version", app.sourceVersion);
            if (app.canUpdate())
            {
                const Rect line{r.x, y, r.w, px(30)};
                ui::label(kFontBody, px(theme::kBody), {line.x, line.y, line.w - px(60), line.h}, theme::kText, "Check for a new release at start");
                changed |= ui::toggle("#check-updates", {line.right() - px(44), line.y, px(44), line.h}, app.settings.checkUpdates);
                y += px(34);
            }
            if (app.canUpdate())
            {
                text(r, y, "One request to GitHub; nothing about you or your PC is sent.", theme::kFaint);
                if (action(r, y, app.busy() && app.task->kind == "update_check" ? "Checking..." : "Check now", ButtonStyle::Secondary, theme::icon::kSync, !app.busy()))
                    app.startUpdateCheck(false);
            }
            divider(r, y);

            section(r, y, "APPEARANCE");
            {
                static const char *themes[] = {"system", "light", "dark"};
                int selected = 0;
                for (int i = 0; i < 3; ++i)
                    if (app.settings.theme == themes[i])
                        selected = i;
                if (ui::segmented("#theme", {r.x, y, std::min(r.w, px(340)), px(38)}, {"System", "Light", "Dark"}, selected))
                {
                    app.settings.theme = themes[selected];
                    changed = true;
                }
                y += px(48);
                text(r, y, selected == 0 ? std::string(launcher::os::kWindows ? "Follows Windows' app mode, now " : "Follows the desktop's style, now ") + (platform::window.darkMode ? "dark." : "light.")
                                         : std::string("Always ") + (selected == 1 ? "light." : "dark."),
                     theme::kFaint);
            }
            divider(r, y);

            section(r, y, "THIS LAUNCHER");
            {
                const Rect line{r.x, y, r.w, px(30)};
                ui::label(kFontBody, px(theme::kBody), {line.x, line.y, line.w - px(60), line.h}, theme::kText, "Reduce motion");
                bool reduce = reduceMotion(app);
                if (ui::toggle("#reduce-motion", {line.right() - px(44), line.y, px(44), line.h}, reduce))
                {
                    app.settings.reduceMotion = reduce;
                    changed = true;
                }
                y += px(34);
                text(r, y, "Everything moves at once instead of animating, and Home's scene stands still.", theme::kFaint);
            }
            if (action(r, y, "Add a shortcut to the desktop", ButtonStyle::Secondary, theme::icon::kLink))
            {
                std::string error;
                toast(app.shortcut(error) ? "Shortcut \"GTA Vice City Stories\" added to the desktop" : "No shortcut: " + error,
                      error.empty() ? 0 : theme::kRed);
            }
            text(r, y, "The game's own settings - graphics, widescreen, controls, sound - are in the game: press F2 while playing.", theme::kFaint);
            if (changed)
                app.saveSettings();
            return y;
        }

        float help(App &app, Rect r)
        {
            float y = r.y;
            section(r, y, "REPORT A PROBLEM");
            text(r, y, "Makes a zip of the build's and the game's logs and what this PC has - no game files, no saves - to attach to your bug report.");
            if (action(r, y, "Make a report", ButtonStyle::Primary, theme::icon::kBug, !app.busy()))
            {
                shell.sheet = Sheet::None;
                app.startReport();
            }
            divider(r, y);
            section(r, y, "FILES");
            struct File
            {
                const char *label;
                std::filesystem::path path;
            };
            const File files[] = {{"The game's last log", app.build / "run" / "launcher_game.log"},
                                  {"Build log", app.build / "build.log"},
                                  {"README", app.root / "README.md"}};
            for (const File &f : files)
            {
                std::error_code ec;
                if (ui::button(f.label, {r.x - px(4), y, r.w + px(8), px(36)}, ButtonStyle::Ghost, theme::icon::kDocument,
                               std::filesystem::is_regular_file(f.path, ec)))
                    launcher::openFile(f.path);
                y += px(40);
            }
            divider(r, y);
            section(r, y, "KEYBOARD");
            const char *keys[][2] = {{"Enter", "The highlighted action (Play, Build...)"},
                                     {"Esc", "Close a sheet or a dialog"},
                                     {"F1", "This help"},
                                     {"Ctrl+L", "The log of a running task"},
                                     {"Tab", "Move between controls"}};
            for (const auto &k : keys)
            {
                const float kw = measure(kFontSemibold, px(12), k[0]) + px(16);
                rectEx({r.x, y + px(3), kw, px(22)}, theme::kSurfaceHi, theme::kSurfaceHi, px(5), theme::ink(0.10f), ui::hairline());
                ui::label(kFontSemibold, px(12), {r.x, y + px(3), kw, px(22)}, theme::kText, k[0], ui::Align::Centre);
                ui::label(kFontBody, px(theme::kSmall), {r.x + px(84), y, r.w - px(84), px(28)}, theme::kDim, k[1]);
                y += px(32);
            }
            divider(r, y);
            section(r, y, "ABOUT");
            y += row({r.x, y, r.w, 0}, "Version", app.sourceVersion);
            y += row({r.x, y, r.w, 0}, "Folder", launcher::pathText(app.root));
            y += row({r.x, y, r.w, 0}, "Python", app.pythonText);
            y += px(6);
            text(r, y, "Unofficial. Grand Theft Auto is property of Rockstar Games / Take-Two Interactive; this port contains no game code or data.", theme::kFaint);
            return y;
        }

        float folder(App &, Rect r)
        {
            float y = r.y;
            drawFolderBody(r, y);
            return y;
        }
    }

    void drawSheets(Rect window)
    {
        App &app = *shell.app;
        if (shell.sheet != Sheet::None)
            shell.sheetShown = shell.sheet;
        ui::Widget &w = ui::widget(ui::id("#sheet"));
        if (w.fresh)
            shell.sheetSpring.snap(0);
        ui::spring(shell.sheetSpring, shell.sheet != Sheet::None ? 1.0f : 0.0f, 340, 0.88f);
        const float open = shell.sheetSpring.value;
        if (open <= 0.002f && shell.sheet == Sheet::None)
        {
            shell.sheetShown = Sheet::None;
            return;
        }
        Rect area = window;
        core::cutTop(area, px(theme::kTitleBar));
        const Layer previous = setLayer(Layer::Overlay);
        ui::beginLayer(kLayerSheet, shell.sheet != Sheet::None);

        // the scrim: clicking it closes the sheet
        rect(area, withAlpha(theme::kScrim, alphaOf(theme::kScrim) * core::clamp01(open)));
        const ui::Interaction scrim = ui::interact(ui::id("#scrim"), area, false);
        if (scrim.clicked)
            shell.sheet = Sheet::None;

        const float width = std::min(px(480), area.w * 0.9f);
        const Rect panel{area.right() - width * open, area.y, width, area.h};
        ui::blockInput(panel);
        shadow(panel.offset(-px(6), 0), theme::shade(0.8f * core::clamp01(open)), px(24), 0);
        rectEx(panel, theme::kBase, mix(theme::kBase, theme::kNight, 0.5f), {px(14), 0, 0, 0}, theme::ink(0.06f), ui::hairline());

        const char *title = "";
        const char *subtitle = "";
        switch (shell.sheetShown)
        {
        case Sheet::Saves: title = "Saves"; subtitle = "Back up and restore your memory card."; break;
        case Sheet::Settings: title = "Settings"; subtitle = "The launcher's own options. The game's are in the game (F2)."; break;
        case Sheet::Help: title = "Help"; subtitle = "When something goes wrong, the logs say why."; break;
        case Sheet::Folder: title = "Build folder"; subtitle = "Where the game is built."; break;
        default: break;
        }
        Rect inner = panel.inset(px(32), 0);
        float y = panel.y + px(26);
        drawText(kFontSemibold, px(24), inner.x, y + metrics(kFontSemibold, px(24)).ascent, theme::kText, title);
        if (ui::iconButton("#closesheet", theme::icon::kClose, {panel.right() - px(56), y - px(2), px(34), px(34)}, "Close (Esc)", true, px(12)))
            shell.sheet = Sheet::None;
        y += px(38);
        ui::label(kFontBody, px(theme::kSmall), {inner.x, y, inner.w, px(20)}, theme::kDim, subtitle);
        y += px(36);

        const Rect view{panel.x, y, panel.w, panel.bottom() - y};
        const ui::Id scroll = ui::id("#sheet-scroll", int(shell.sheetShown));
        ui::beginScroll(scroll, view);
        const Rect content{inner.x, y + px(4), inner.w, 0};
        // the content follows a beat behind the panel
        render::pushOffset(std::round((1 - core::clamp01(open)) * px(40)), 0);
        float bottom = y;
        switch (shell.sheetShown)
        {
        case Sheet::Saves: bottom = saves(app, content); break;
        case Sheet::Settings: bottom = settings(app, content); break;
        case Sheet::Help: bottom = help(app, content); break;
        case Sheet::Folder: bottom = folder(app, content); break;
        default: break;
        }
        render::popOffset();
        ui::endScroll(scroll, bottom + px(24));
        ui::endLayer();
        setLayer(previous);
    }
}
