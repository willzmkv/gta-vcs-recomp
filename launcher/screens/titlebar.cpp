// Rockstar Games Launcher-style Left Sidebar and Top Navigation Bar (JOGO/GAME, MODS, CONFIGURAÇÕES/OPTIONS, SOBRE/ABOUT) + Window Controls.
#include "shell.h"

#include "model/sound.h"
#include "platform/platform.h"

namespace screens
{
    using namespace render;
    using core::mix;
    using core::rgb;
    using core::withAlpha;
    using ui::label;

    namespace
    {
        bool windowButton(std::string_view idLabel, char32_t icon, Rect r, bool close, bool darkInk)
        {
            const ui::Interaction in = ui::interact(ui::id(idLabel), r, false);
            const float hover = in.w->hover;
            if (close)
                rect(r, withAlpha(rgb(0xC42B1C), hover * (1.0f - 0.25f * in.w->press)));
            else
                rect(r, withAlpha(darkInk ? rgb(0x000000) : rgb(0xFFFFFF), 0.08f * hover + 0.05f * in.w->press));

            const Color baseCol = darkInk ? rgb(0x140D20) : theme::kDim;
            const Color hoverCol = close ? rgb(0xFFFFFF) : (darkInk ? rgb(0x000000) : theme::kText);
            const Color c = mix(baseCol, hoverCol, hover);
            drawIcon(kFontIcons, px(10.5f), r.cx(), r.cy(), platform::window.focused ? c : withAlpha(c, 0.55f), icon);
            return in.clicked;
        }

        bool topTabButton(std::string_view idLabel, std::string_view text, Rect r, bool active, bool darkInk,
                          std::vector<Rect> &hits)
        {
            hits.push_back(r);
            const ui::Interaction in = ui::interact(ui::id(idLabel), r, true);
            ui::Widget &w = *in.w;
            if (w.fresh)
                w.t = active ? 1.0f : 0.0f;
            ui::animate(w.t, active ? 1.0f : 0.0f, 18.0f);

            const Color idleCol = darkInk ? rgb(0x181428, 225) : rgb(0xD0CADC, 200);
            const Color activeCol = darkInk ? rgb(0x090610) : rgb(0xFFFFFF);
            const Color textCol = mix(idleCol, activeCol, std::max(w.hover * 0.7f, w.t));
            const Font font = (active || w.t > 0.5f) ? kFontSemibold : kFontBody;

            label(font, px(13.5f), {r.x, r.y, r.w, r.h - px(6)}, textCol, text, ui::Align::Centre);

            if (w.t > 0.01f)
            {
                const float barW = (r.w - px(12)) * core::clamp01(w.t);
                const Rect underline{r.cx() - barW * 0.5f, r.bottom() - px(7), barW, px(3)};
                rect(underline, rgb(0xFF2A85), px(1.5f));
            }
            else if (w.hover > 0.01f)
            {
                const float barW = (r.w - px(18)) * w.hover;
                const Rect underline{r.cx() - barW * 0.5f, r.bottom() - px(7), barW, px(2)};
                rect(underline, withAlpha(rgb(0xFF2A85), 0.45f * w.hover), px(1.0f));
            }
            return in.clicked;
        }
    }

    void drawSidebar(Rect sidebar)
    {
        App &app = *shell.app;
        // Solid obsidian Rockstar sidebar background
        rect(sidebar, rgb(0x050508));
        rect({sidebar.right() - 1, sidebar.y, 1, sidebar.h}, rgb(0xFFFFFF, 18));

        // Top Brand Header: Custom user logo (minha_logo.png) WITHOUT any bounding box border
        const Rect logoRect{sidebar.x + px(16), sidebar.y + px(22), px(68), px(31)};
        if (imageInfo(ImageId::BrandLogo).valid)
        {
            image(logoRect, ImageId::BrandLogo);
        }
        else
        {
            label(kFontSemibold, px(13), logoRect, theme::kPinkHi, "W.mkv", ui::Align::Centre);
        }
        const float brandTextX = logoRect.right() + px(10);
        const float brandTextW = sidebar.right() - brandTextX - px(10);
        label(kFontSemibold, px(12.5f), {brandTextX, sidebar.y + px(20), brandTextW, px(18)}, rgb(0xF2F0F6), "VCS.rec");
        label(kFontBody, px(11.5f), {brandTextX, sidebar.y + px(37), brandTextW, px(17)}, rgb(0xB8B2C6), "Launcher");

        // Horizontal divider
        const float divY = sidebar.y + px(80);
        rect({sidebar.x + px(16), divY, sidebar.w - px(32), 1}, rgb(0xFFFFFF, 22));

        // Section Header: "Meus jogos" / "My Games"
        label(kFontSemibold, px(13.0f), {sidebar.x + px(20), divY + px(18), sidebar.w - px(32), px(22)}, rgb(0xF2F0F6),
              tr("Meus jogos", "My Games"));

        // Selected Game Entry Card
        const Rect card{sidebar.x + px(6), divY + px(50), sidebar.w - px(12), px(52)};
        const ui::Interaction in = ui::interact(ui::id("#sidebar-vcs-game"), card, true);
        const float hover = in.w->hover;
        const bool isGameActive = (shell.mode == Mode::Home && shell.tab == Tab::Game && shell.sheet == Sheet::None);

        const Color fillTop = mix(rgb(0x2D1628), rgb(0x3B1D35), std::max(hover, isGameActive ? 0.35f : 0.0f));
        const Color fillBot = mix(rgb(0x22111F), rgb(0x2E162A), std::max(hover, isGameActive ? 0.35f : 0.0f));
        rectEx(card, fillTop, fillBot, px(6), rgb(0xFF2A85, uint8_t(40 + 50 * hover)), ui::hairline());

        // Left hot-pink indicator bar
        pushClip(card);
        rect({card.x, card.y, px(4), card.h}, rgb(0xFF2A85), {px(6), 0, 0, px(6)});
        popClip();

        // Game Icon (icone.png)
        const Rect iconRect{card.x + px(12), card.cy() - px(17), px(34), px(34)};
        if (imageInfo(ImageId::GameIcon).valid)
            image(iconRect, ImageId::GameIcon, px(5));
        else
            rect(iconRect, theme::kPink, px(5));

        // Game Title ("Grand Theft Auto:\nVice City Stories")
        const float tx = iconRect.right() + px(10);
        const float tw = card.right() - tx - px(8);
        label(kFontSemibold, px(11.0f), {tx, card.y + px(9), tw, px(16)}, rgb(0xFFFFFF), "Grand Theft Auto:");
        label(kFontBody, px(11.0f), {tx, card.y + px(25), tw, px(16)}, rgb(0xE0DCE8), "Vice City Stories");

        if (in.clicked)
        {
            shell.tab = Tab::Game;
            shell.tabSince = ui::state.time;
            shell.setupOpen = false;
            shell.sheet = Sheet::None;
            sound::play(sound::Sfx::Nav);
        }

        // Bottom Language Switcher: PT-BR | EN
        const Rect langBox{sidebar.x + px(16), sidebar.bottom() - px(44), sidebar.w - px(32), px(28)};
        const Rect ptBtn{langBox.x, langBox.y, langBox.w * 0.5f - px(3), langBox.h};
        const Rect enBtn{langBox.x + langBox.w * 0.5f + px(3), langBox.y, langBox.w * 0.5f - px(3), langBox.h};
        const bool isPt = app.isPt();

        auto drawLangPill = [&](std::string_view idStr, std::string_view txt, Rect b, bool active) {
            const ui::Interaction lin = ui::interact(ui::id(idStr), b, true);
            const Color bg = active ? rgb(0xFF2A85, 210) : rgb(0xFFFFFF, uint8_t(12 + 20 * lin.w->hover));
            rect(b.scaled(1.0f - 0.04f * lin.w->press), bg, px(5));
            label(kFontSemibold, px(11.0f), b, active ? rgb(0xFFFFFF) : rgb(0xB8B2C6), txt, ui::Align::Centre);
            return lin.clicked;
        };

        if (drawLangPill("#lang-pt", "PT-BR", ptBtn, isPt) && !isPt)
        {
            app.settings.language = "pt";
            app.settings.languageChosen = true;
            app.setGameLanguagePt(true);
            sound::play(sound::Sfx::Nav);
            app.saveSettings();
            toast("Idioma: Português do Brasil (Launcher e Jogo).");
        }
        if (drawLangPill("#lang-en", "ENGLISH", enBtn, !isPt) && isPt)
        {
            app.settings.language = "en";
            app.settings.languageChosen = true;
            app.setGameLanguagePt(false);
            sound::play(sound::Sfx::Nav);
            app.saveSettings();
            toast("Language: English (Launcher and Game).");
        }
    }

    float drawTitleBar(Rect window)
    {
        App &app = *shell.app;
        const float h = px(56);
        const Rect bar{window.x, window.y, window.w, h};
        ui::beginLayer(kLayerSheet, false);
        struct EndLayer
        {
            ~EndLayer() { ui::endLayer(); }
        } endLayer;

        // Both JOGO (bright sunset sky) and MODS/CONFIGURAÇÕES/SOBRE (1980s Vice City tropical cyan sky) have bright top skies
        const bool onBrightSky = (shell.mode == Mode::Home && shell.sheet == Sheet::None && !shell.setupOpen);

        std::vector<Rect> hits;
        Rect right = bar;
        const float bw = px(44);
        const Rect closeR = core::cutRight(right, bw);
        const Rect maxR = core::cutRight(right, bw);
        const Rect minR = core::cutRight(right, bw);
        hits.push_back(closeR);
        hits.push_back(minR);

        if (windowButton("#close", theme::icon::kClose, closeR, true, onBrightSky))
            requestQuit(app);
        if (windowButton("#max", platform::window.maximized ? theme::icon::kRestore : theme::icon::kMaximize, maxR, false, onBrightSky))
            platform::toggleMaximize();
        if (windowButton("#min", theme::icon::kMinimize, minR, false, onBrightSky))
            platform::minimize();

        // Top Navigation Tabs: JOGO/GAME | MODS | CONTROLES/CONTROLS | CONFIGURAÇÕES/OPTIONS | SOBRE/ABOUT
        struct NavTab
        {
            const char *id;
            const char *label;
            Tab tab;
        };
        const NavTab tabs[] = {
            {"#nav-game", tr("JOGO", "GAME"), Tab::Game},
            {"#nav-mods", "MODS", Tab::Mods},
            {"#nav-controls", tr("CONTROLES", "CONTROLS"), Tab::Controls},
            {"#nav-settings", tr("CONFIGURAÇÕES", "OPTIONS"), Tab::Settings},
            {"#nav-about", tr("SOBRE", "ABOUT"), Tab::About},
        };

        const float mainLeft = window.x + px(kSidebarWidth);
        const float mainW = window.w - px(kSidebarWidth);
        float totalTabsW = 0;
        constexpr int kTabCount = 5;
        float tabWidths[kTabCount] = {};
        for (int i = 0; i < kTabCount; ++i)
        {
            tabWidths[i] = std::ceil(measure(kFontSemibold, px(13.5f), tabs[i].label) + px(20));
            totalTabsW += tabWidths[i];
        }
        const float tabGap = px(14);
        totalTabsW += tabGap * (kTabCount - 1);

        float tabX = std::max(mainLeft + px(20), mainLeft + mainW * 0.28f);
        if (tabX + totalTabsW > minR.x - px(16))
            tabX = std::max(mainLeft + px(16), minR.x - px(16) - totalTabsW);

        for (int i = 0; i < kTabCount; ++i)
        {
            const Rect tabR{tabX, bar.y + px(8), tabWidths[i], h - px(10)};
            const bool active = (shell.tab == tabs[i].tab && !shell.setupOpen && shell.mode != Mode::Task);
            if (topTabButton(tabs[i].id, tabs[i].label, tabR, active, onBrightSky, hits))
            {
                shell.tab = tabs[i].tab;
                shell.tabSince = ui::state.time;
                shell.setupOpen = false;
                shell.sheet = Sheet::None;
                sound::play(sound::Sfx::Nav);
                if (tabs[i].tab == Tab::Mods)
                    app.refreshMods();
            }
            tabX += tabWidths[i] + tabGap;
        }

        platform::Chrome chrome;
        chrome.caption = bar;
        chrome.maximizeButton = maxR;
        chrome.buttons = hits;
        platform::setChrome(chrome);
        return h;
    }
}
