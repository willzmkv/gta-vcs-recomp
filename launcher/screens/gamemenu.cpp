// The in-game menu (T12, the plans' 2026-11-17-global-menu.md): ESC / Start pauses the game and shows this, full
// screen, in place of the game's own pause menu and the F2 / F5 menus. Phase 0: the screens laid out here, in the
// launcher, with sample data, for the player to approve (--screenshot ... --screen gamemenu:N[S], S an Options tab).
//
// The look is GTA: Vice City - The Definitive Edition's options menu (the player's reference, 2026-11-17): the logo top
// left, the pages as a column of capitals bottom left with a slanted pink bar under the one shown, a row of tabs top
// right and the rows under them, pastel and alternating, with chevrons; the launcher's pink sliders and switches (the player's choice). Measured on a 1920x1080
// picture of it and laid out in those units (u()), centred on wider windows. The launcher's fonts (the player's
// choice: no Pricedown). Behind: the game's frame the player paused on, blurred and tinted cyan (in the game the real
// one; here menu_backdrop.png, a test frame never committed). Above the tabs, where the story stands.
#include "shell.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace screens
{
    using namespace render;
    using core::rgb;
    using core::withAlpha;

    namespace
    {
        // the reference's colours
        constexpr uint32_t kPink = 0xF98BB6, kPinkDeep = 0xF567BB, kTeal = 0x1592A8, kTealText = 0x1E89A3,
                           kTealValue = 0x1E7F98, kTealShadow = 0x2AB9C9, kRowLight = 0xF6FCFD, kRowCyan = 0xE1F6F9;

        float g_unit = 1, g_ox = 0; // pixels per reference pixel; the layout's left edge
        float u(float v) { return v * g_unit; }
        float ux(float v) { return g_ox + v * g_unit; }

        // the right column: x 529..1821, tabs at y 65..142, rows of 80 from 152
        constexpr float kColX = 529, kColR = 1821, kTabY = 65, kTabH = 77, kRowY = 152, kRowH = 80;

        void text(Font font, float size, Rect r, uint32_t color, std::string_view s, ui::Align a = ui::Align::Left)
        {
            ui::label(font, u(size), r, rgb(color), s, a);
        }

        // ---- the right column's parts ------------------------------------------------------------------------------
        void tabs(const std::vector<std::string> &names, int active)
        {
            const float gap = 4, w = (kColR - kColX - gap * float(names.size() - 1)) / float(names.size());
            for (size_t i = 0; i < names.size(); ++i)
            {
                const Rect r{ux(kColX + float(i) * (w + gap)), u(kTabY), u(w), u(kTabH)};
                const bool on = int(i) == active;
                rect(r, on ? rgb(kPink) : rgb(0xF0FBFC, 232));
                text(kFontDisplay, 28, r, on ? 0xFFFFFF : kTeal, names[i], ui::Align::Centre);
            }
        }

        Rect rowRect(float top, int i) { return {ux(kColX), u(top + float(i) * kRowH), u(kColR - kColX), u(kRowH)}; }

        Rect row(float top, int i, std::string_view label)
        {
            const Rect r = rowRect(top, i);
            rect(r, i % 2 == 0 ? rgb(kRowLight, 236) : rgb(kRowCyan, 226));
            text(kFontSemibold, 24, {r.x + u(20), r.y, r.w * 0.55f, r.h}, kTealText, label);
            return r;
        }

        void chevron(float cx, float cy, bool right)
        {
            const float s = u(9), d = right ? 1.0f : -1.0f;
            line(cx - d * s * 0.5f, cy - s, cx + d * s * 0.5f, cy, u(3.2f), rgb(kTealValue));
            line(cx + d * s * 0.5f, cy, cx - d * s * 0.5f, cy + s, u(3.2f), rgb(kTealValue));
        }

        // < value > at the reference's places (chevrons at x 1676 and 1782, the value between)
        void cycle(float top, int i, std::string_view label, std::string_view value)
        {
            const Rect r = row(top, i, label);
            const float cy = r.y + r.h * 0.5f;
            const float valueW = std::max(u(60), measure(kFontSemibold, u(24), value) + u(10));
            const float right = ux(1782), left = std::min(ux(1676), right - valueW - u(44));
            chevron(left, cy, false);
            chevron(right, cy, true);
            text(kFontSemibold, 24, {left + u(14), r.y, right - left - u(28), r.h}, kTealValue, value, ui::Align::Centre);
        }

        // the launcher's own controls (ui::vcSliderTrack / vcToggleSwitch: identical drawings, the player's request),
        // scaled from its 44-pixel rows to these 80-pixel ones
        float controlScale() { return u(kRowH / 44.0f) / px(1); }

        // the launcher's pink slider on the reference's track (x 1256..1786), the value left of it
        void slider(float top, int i, std::string_view label, float frac, std::string_view value = {})
        {
            const Rect r = row(top, i, label);
            const float cy = r.y + r.h * 0.5f, s = controlScale(), h = px(8) * s;
            ui::vcSliderTrack({ux(1266), cy - h * 0.5f, u(510), h}, std::clamp(frac, 0.0f, 1.0f), 0.0f, s);
            if (!value.empty())
                text(kFontSemibold, 20, {ux(1100), r.y, u(140), r.h}, kTealValue, value, ui::Align::Right);
        }

        // the launcher's switch, at the right end of the row
        void toggle(float top, int i, std::string_view label, bool on)
        {
            const Rect r = row(top, i, label);
            const float s = controlScale() * 0.85f, w = px(52) * s, h = px(26) * s; // a little smaller (the player's look)
            ui::vcToggleSwitch({ux(1792) - w, std::floor(r.y + (r.h - h) * 0.5f), w, h}, on ? 1.0f : 0.0f, 0.0f, s);
        }

        // a value on the right, read-only (stats, slots)
        void value(float top, int i, std::string_view label, std::string_view v, uint32_t color = kTealValue)
        {
            const Rect r = row(top, i, label);
            text(kFontSemibold, 24, {r.x + r.w * 0.45f, r.y, r.w * 0.55f - u(28), r.h}, color, v, ui::Align::Right);
        }

        // ---- the pages --------------------------------------------------------------------------------------------
        void pageMap()
        {
            // the map in a slanted panel, as the PS2's own map page (the player's mapa.webp); its tiles come from the
            // game's textures in phase 5 - a stand-in city here
            const float x0 = ux(kColX), y0 = u(kTabY), x1 = ux(kColR), y1 = u(1015);
            const float frame[8] = {x0, y0 + u(26), x1, y0, x1 - u(10), y1 - u(24), x0 + u(14), y1};
            quad(frame, rgb(0x101820, 230));
            const float m = u(14);
            const float sea[8] = {x0 + m, y0 + u(26) + m, x1 - m, y0 + m, x1 - u(10) - m, y1 - u(24) - m, x0 + u(14) + m, y1 - m};
            quad(sea, rgb(0x93D1F5));
            auto land = [&](float a, float b, float c, float d, uint32_t col) {
                rect({x0 + (x1 - x0) * a, y0 + (y1 - y0) * b, (x1 - x0) * (c - a), (y1 - y0) * (d - b)}, rgb(col), Corners(u(18)));
            };
            land(0.30f, 0.12f, 0.46f, 0.92f, 0x2E9E5B); // the mainland
            land(0.32f, 0.16f, 0.44f, 0.88f, 0xE9EDF0);
            land(0.56f, 0.10f, 0.70f, 0.86f, 0x2E9E5B); // the beach
            land(0.58f, 0.14f, 0.66f, 0.82f, 0xE9EDF0);
            land(0.66f, 0.12f, 0.72f, 0.84f, 0xF4EBC4);
            land(0.47f, 0.48f, 0.53f, 0.56f, 0x2E9E5B); // Starfish Island
            const float px0 = x0 + (x1 - x0) * 0.40f, py0 = y0 + (y1 - y0) * 0.50f;
            line(x0 + m, py0, x1 - m, py0 - u(10), u(2), rgb(kPinkDeep, 200));
            line(px0, y0 + m + u(20), px0 + u(6), y1 - m, u(2), rgb(kPinkDeep, 200));
            const float tri[8] = {px0 - u(10), py0 - u(14), px0 + u(14), py0, px0 - u(10), py0 + u(14), px0 - u(4), py0};
            quad(tri, rgb(0xFFFFFF));
            text(kFontDisplay, 40, {x1 - u(330), y1 - u(110), u(280), u(50)}, 0xFFFFFF, "Little Havana", ui::Align::Right);
            text(kFontSemibold, 16, {x0 + u(40), y1 - u(56), u(700), u(26)}, 0x30566A,
                 tr("Prévia de layout - o mapa real vem das texturas do jogo",
                    "Layout preview - the real map comes from the game's textures"));
        }

        void pageBrief()
        {
            struct Line
            {
                const char *who, *pt, *en;
            };
            static const Line kLines[] = {
                {"Martinez", "Vá até a loja de bebidas e lembre ao dono quem manda no bairro.",
                 "Go to the liquor store and remind the owner who runs the neighbourhood."},
                {"", "Destrua as vitrines para chamar a atenção dele.", "Smash the windows to get his attention."},
                {"Phil", "Os Cholos estão no armazém. Traga o caminhão de volta.", "The Cholos are at the warehouse. Bring the truck back."},
                {"", "Você está sendo procurado. Despiste a polícia.", "You're wanted. Lose the cops."},
                {"Louise", "Me encontre no Hotel Harrison às 21h.", "Meet me at the Hotel Harrison at 9 pm."},
            };
            int i = 0;
            for (const Line &l : kLines)
            {
                const Rect r = row(kTabY, i++, l.who);
                text(kFontSemibold, 22, {r.x + u(250), r.y, r.w - u(270), r.h}, kTealValue, tr(l.pt, l.en));
            }
        }

        void pageStats()
        {
            struct Stat
            {
                const char *pt, *en, *v;
            };
            static const Stat kStats[] = {
                {"Progresso da história", "Story progress", "23%"},
                {"Missões concluídas", "Missions passed", "21 de 54"},
                {"Tempo de jogo", "Time played", "6 h 42 min"},
                {"Pacotes escondidos", "Hidden packages", "12 de 99"},
                {"Saltos únicos", "Unique jumps", "4 de 30"},
                {"Pessoas eliminadas", "People killed", "312"},
                {"Veículos destruídos", "Vehicles destroyed", "57"},
                {"Precisão", "Accuracy", "38%"},
                {"Maior nível de procurado", "Highest wanted level", "4"},
                {"Distância percorrida", "Distance travelled", "329,1 km"},
                {"Império: negócios", "Empire: businesses", "3"},
            };
            int i = 0;
            for (const Stat &s : kStats)
                value(kTabY, i++, tr(s.pt, s.en), s.v);
        }

        void pageGame()
        {
            tabs({tr("SALVAR", "SAVE"), tr("CARREGAR", "LOAD")}, 0);
            struct Slot
            {
                const char *name, *when;
            };
            static const Slot kSlots[] = {
                {"Cholo Vitorioso", "17/11  09:12  ·  23%"}, {"Matar Phil: Parte 2", "16/11  22:40  ·  21%"},
                {"Shakedown", "15/11  19:05  ·  14%"},       {"", ""}, {"", ""}, {"", ""}, {"", ""}, {"", ""},
            };
            for (int i = 0; i < 8; ++i)
            {
                const std::string name = std::to_string(i + 1) + "    " + (*kSlots[i].name ? kSlots[i].name : tr("Vazio", "Empty"));
                value(kRowY, i, name, kSlots[i].when);
                if (shell.gameMenuSlot == i)
                    rect({ux(kColX), u(kRowY + float(i) * kRowH), u(6), u(kRowH)}, rgb(kPinkDeep));
            }
            row(kRowY, 8, tr("Sair para o Windows", "Quit to Windows"));
        }

        void pageOptions()
        {
            launcher::GameConfig &cfg = shell.app->gameConfig;
            const std::vector<std::string> names = {tr("CONTROLES", "CONTROLS"), tr("TELA", "DISPLAY"),
                                                    tr("GRÁFICOS", "GRAPHICS"), tr("MUNDO", "WORLD"),
                                                    tr("POPULAÇÃO", "PEOPLE"), tr("ÁUDIO", "AUDIO")};
            const int sub = std::clamp(shell.configSubTab, 0, 5);
            tabs(names, sub);
            auto pct = [](int v) { return std::to_string(v) + "%"; };
            int i = 0;
            switch (sub)
            {
            case 0:
                if (shell.gameMenuControllerLayout)
                {
                    // the launcher's controller page as it is (the real controllers, their callouts, the schemes)
                    const Rect area{ux(kColX), u(kRowY), u(kColR - kColX), u(1015 - kRowY)};
                    rect(area, rgb(0xDDF1F7));
                    // everything on it 15% larger than in the launcher (the player's look): the UI scale for this page
                    const float scale = ui::state.scale;
                    ui::state.scale = scale * 1.15f;
                    drawControls(area.inset(-px(38), -px(10))); // its own margins, at the new scale: flush with the column
                    ui::state.scale = scale;
                    return;
                }
                row(kRowY, i++, tr("Ajustar o layout do controle", "Adjust Control Layout"));
                cycle(kRowY, i++, tr("Esquema", "Scheme"), cfg.padPreset == 2 ? tr("Moderno (GTA IV / V)", "Modern (GTA IV / V)") : tr("Clássico PS2", "Classic PS2"));
                toggle(kRowY, i++, tr("Inverter visão", "Invert Look"), cfg.mouseInvertY != 0);
                slider(kRowY, i++, tr("Sensibilidade do mouse", "Mouse Sensitivity"), (cfg.mouseSensitivity - 10) / 390.0f, pct(cfg.mouseSensitivity));
                slider(kRowY, i++, tr("Velocidade da câmera (controle)", "Camera Speed (gamepad)"), (cfg.padCameraSpeed - 10) / 390.0f, pct(cfg.padCameraSpeed));
                toggle(kRowY, i++, tr("Mira livre", "Free Aim"), cfg.padFreeAim != 0);
                slider(kRowY, i++, tr("Vibração", "Vibration"), cfg.padVibration / 200.0f, pct(cfg.padVibration));
                row(kRowY, i++, tr("Restaurar padrões", "Restore Defaults"));
                break;
            case 1:
                cycle(kRowY, i++, tr("Modo de exibição", "Display Mode"), cfg.displayWindow ? tr("Janela", "Windowed") : tr("Tela cheia", "Fullscreen"));
                cycle(kRowY, i++, tr("Proporção da tela", "Aspect Ratio"), cfg.displayAspect == 169 ? "16:9" : cfg.displayAspect == 43 ? "4:3" : tr("Automático", "Auto"));
                cycle(kRowY, i++, "HUD", cfg.displayHud == 2 ? tr("Nas bordas", "At the edges") : cfg.displayHud == 1 ? "4:3" : tr("Esticado", "Stretched"));
                cycle(kRowY, i++, tr("Taxa de quadros", "Frame Rate"), cfg.displayFps == -1 ? tr("Do monitor", "Display's") : std::to_string(cfg.displayFps) + " fps");
                toggle(kRowY, i++, tr("Sincronização vertical", "Vertical Sync"), cfg.displayVsync != 0);
                toggle(kRowY, i++, tr("Barras pretas nas cutscenes", "Cutscene Black Bars"), cfg.displayCutsceneBars != 0);
                slider(kRowY, i++, tr("Exposição", "Exposure"), cfg.graphicsExposure / 200.0f, pct(cfg.graphicsExposure));
                toggle(kRowY, i++, tr("Legendas", "Subtitles"), true);
                break;
            case 2:
                cycle(kRowY, i++, tr("Predefinição", "Preset"), tr("Alto", "High"));
                cycle(kRowY, i++, tr("Resolução interna", "Internal Resolution"), cfg.displayScale == 0 ? tr("Da janela", "Window's") : std::to_string(cfg.displayScale) + "x");
                cycle(kRowY, i++, tr("Anti-serrilhado (MSAA)", "Anti-aliasing (MSAA)"), std::to_string(cfg.displayMsaa) + "x");
                cycle(kRowY, i++, tr("Filtro anisotrópico", "Anisotropic Filtering"), std::to_string(cfg.displayAniso) + "x");
                toggle(kRowY, i++, tr("Rastros e brilho", "Trails and Bloom"), cfg.graphicsPostProcess != 0);
                cycle(kRowY, i++, tr("API gráfica", "Graphics API"), cfg.displayRenderer == 1 ? "Direct3D 12" : cfg.displayRenderer == 2 ? "Vulkan" : "Direct3D 11");
                break;
            case 3:
                cycle(kRowY, i++, tr("Cidade detalhada", "Detailed City"), cfg.worldHdRadius == 0 ? std::string(tr("Original (~260 m)", "Original (~260 m)")) : "~" + std::to_string(260 + cfg.worldHdRadius * 155) + " m");
                slider(kRowY, i++, tr("Distância de visão", "View Distance"), cfg.worldViewDistance / 100.0f, pct(cfg.worldViewDistance));
                slider(kRowY, i++, tr("Distância de detalhes", "Detail Distance"), cfg.worldDetailDistance / 100.0f, pct(cfg.worldDetailDistance));
                slider(kRowY, i++, tr("Neblina", "Fog"), cfg.fogDistance / 100.0f, pct(cfg.fogDistance));
                cycle(kRowY, i++, tr("Memória de streaming", "Streaming Memory"), cfg.memoryStreaming == 0 ? std::string("PS2 (5.4 MB)") : std::to_string(cfg.memoryStreaming) + " MB");
                break;
            case 4:
                slider(kRowY, i++, tr("Distância da população", "Population Distance"), cfg.worldPopulationDistance / 100.0f, pct(cfg.worldPopulationDistance));
                slider(kRowY, i++, tr("Densidade de veículos", "Vehicle Density"), cfg.worldVehicleDensity / 100.0f, pct(cfg.worldVehicleDensity));
                slider(kRowY, i++, tr("Densidade de pedestres", "Pedestrian Density"), cfg.worldPedestrianDensity / 100.0f, pct(cfg.worldPedestrianDensity));
                toggle(kRowY, i++, tr("Faixas de multidão", "Crowd Bands"), cfg.worldCrowdBands != 0);
                slider(kRowY, i++, tr("Trânsito distante", "Distant Traffic"), cfg.worldDistantTraffic / 100.0f, pct(cfg.worldDistantTraffic));
                slider(kRowY, i++, tr("Multidões distantes", "Distant Crowds"), cfg.worldDistantCrowds / 100.0f, pct(cfg.worldDistantCrowds));
                break;
            default:
                slider(kRowY, i++, tr("Volume geral", "Master Volume"), cfg.audioVolume / 100.0f, pct(cfg.audioVolume));
                slider(kRowY, i++, tr("Música", "Music"), 0.8f, "80%");
                slider(kRowY, i++, tr("Efeitos sonoros", "Sound Effects"), 0.9f, "90%");
                slider(kRowY, i++, tr("Volume dos filmes", "Movie Volume"), cfg.audioMovieVolume / 100.0f, pct(cfg.audioMovieVolume));
                cycle(kRowY, i++, tr("Saída de áudio", "Audio Output"), cfg.audioOutput == 1 ? std::string("Mono") : cfg.audioOutput == 2 ? std::string("Pro Logic II") : std::string(tr("Estéreo", "Stereo")));
                cycle(kRowY, i++, tr("Em segundo plano", "In the Background"), tr("Continuar tocando", "Keep playing"));
                break;
            }
        }

        void pageMods()
        {
            const launcher::GameConfig &cfg = shell.app->gameConfig;
            int i = 0;
            if (cfg.mods.empty())
            {
                toggle(kTabY, i++, "HD Hands", true);
                toggle(kTabY, i++, "HD Hud", true);
            }
            for (const launcher::ModEntry &m : cfg.mods)
            {
                if (i >= 10)
                    break;
                slider(kTabY, i++, m.folderName, m.enabled ? m.priority / 100.0f : 0.0f,
                       m.enabled ? std::string(tr("prioridade ", "priority ")) + std::to_string(m.priority) : std::string(tr("Desligado", "Off")));
            }
            row(kTabY, i, tr("Recarregar os mods", "Rescan Mods"));
        }

        void pageTrainer()
        {
            int i = 0;
            toggle(kTabY, i++, tr("Modo Deus", "God Mode"), false);
            slider(kTabY, i++, tr("Nível de procurado", "Wanted Level"), 0.0f, "0");
            value(kTabY, i++, tr("Dinheiro", "Money"), "+ $ 10.000", kPinkDeep);
            value(kTabY, i++, tr("Armas e munição", "Weapons and Ammo"), tr("Pacote completo", "Full set"), kPinkDeep);
            cycle(kTabY, i++, tr("Gerar veículo", "Spawn a Vehicle"), "Infernus");
            cycle(kTabY, i++, tr("Teletransportar", "Teleport"), "Little Havana");
            slider(kTabY, i++, tr("Hora do dia", "Time of Day"), 14 / 23.0f, "14h");
            cycle(kTabY, i++, tr("Clima", "Weather"), tr("Ensolarado", "Sunny"));
            cycle(kTabY, i++, tr("Velocidade do jogo", "Game Speed"), "1x");
            row(kTabY, i, tr("Nada disto é salvo no seu jogo", "Nothing here is saved in your game"));
        }

        // the left column: the pages as words, the one shown on a slanted pink bar (the reference's 105..420 wide)
        void menuColumn(int active)
        {
            const char *items[] = {tr("CONTINUAR", "RESUME"), tr("MAPA", "MAP"),   tr("MISSÃO", "BRIEF"),
                                   tr("ESTATÍSTICAS", "STATS"), tr("JOGO", "GAME"), tr("OPÇÕES", "OPTIONS"),
                                   "MODS",                       "TRAINER"};
            const int n = 8;
            const float step = 62, last = 991, cx = 263;
            for (int i = 0; i < n; ++i)
            {
                const float cy = last - float(n - 1 - i) * step;
                if (i == active)
                {
                    // the bar: a quadrilateral leaning a little, its lower right edge in teal
                    const float shadow[8] = {ux(112), u(cy - 27), ux(424), u(cy - 22), ux(420), u(cy + 34), ux(108), u(cy + 36)};
                    quad(shadow, rgb(kTealShadow));
                    const float bar[8] = {ux(110), u(cy - 31), ux(421), u(cy - 26), ux(416), u(cy + 29), ux(105), u(cy + 32)};
                    quad(bar, rgb(kPink));
                }
                const Rect r{ux(cx - 170), u(cy - 26), u(340), u(52)};
                text(kFontDisplay, 33, r.offset(u(2), u(2)), 0x0B6C80, items[i], ui::Align::Centre);
                text(kFontDisplay, 33, r, 0xFFFFFF, items[i], ui::Align::Centre);
            }
        }
    }

    void drawGameMenu(Rect window)
    {
        g_unit = window.h / 1080.0f;
        g_ox = std::max(0.0f, (window.w - 1920.0f * g_unit) * 0.5f);

        // behind: the game's frame, blurred, tinted to the reference's cyan
        if (imageInfo(ImageId::MenuBackdrop).valid)
            imageCover(window, ImageId::MenuBackdrop);
        else
            gradient(window, rgb(0xBFF2F7), rgb(0x06C2D4));
        gradient(window, rgb(0x9FEFF6, 175), rgb(0x00C2D6, 195));

        // the logo, top left
        if (imageInfo(ImageId::GameLogo).valid)
        {
            const float h = u(380), w = h * imageInfo(ImageId::GameLogo).aspect;
            image({ux(263) - w * 0.5f, u(100), w, h}, ImageId::GameLogo);
        }

        // where the story stands, above the tabs
        text(kFontDisplay, 30, {ux(kColX), u(14), u(220), u(44)}, kPinkDeep, tr("PAUSADO", "PAUSED"));
        text(kFontSemibold, 20, {ux(kColX + 180), u(14), u(kColR - kColX - 180), u(44)}, 0xFFFFFF,
             tr("Missão: Shakedown   ·   Little Havana   ·   14:32   ·   $ 12.480   ·   23%",
                "Mission: Shakedown   ·   Little Havana   ·   2:32 pm   ·   $ 12,480   ·   23%"),
             ui::Align::Right);

        // the page; shell.gameMenuTab: 0 map, 1 brief, 2 stats, 3 game, 4 options, 5 mods, 6 trainer
        const int tab = std::clamp(shell.gameMenuTab, 0, 6);
        menuColumn(tab + 1);
        switch (tab)
        {
        case 0: pageMap(); break;
        case 1: pageBrief(); break;
        case 2: pageStats(); break;
        case 3: pageGame(); break;
        case 4: pageOptions(); break;
        case 5: pageMods(); break;
        default: pageTrainer(); break;
        }

        // what the keys do, quietly, bottom right
        text(kFontSemibold, 18, {ux(kColX), u(1030), u(kColR - kColX), u(34)}, 0xFFFFFF,
             tr("ESC  voltar ao jogo      Q / E  abas      ENTER  selecionar      ← →  mudar",
                "ESC  back to the game      Q / E  tabs      ENTER  select      ← →  change"),
             ui::Align::Right);
    }
}
