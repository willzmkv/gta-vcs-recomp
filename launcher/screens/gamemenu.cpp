// The in-game menu (T12, the plans' 2026-11-17-global-menu.md): ESC / Start pauses the game and shows this, full
// screen, in place of the game's own pause menu and the F2 / F5 menus. Phase 0: the screens laid out here, in the
// launcher, with sample data, for the player to approve (--screenshot ... --screen gamemenu:N); the same code is then
// drawn inside the game by the shared UI kit. Settings, Controls and Mods are the launcher's own pages.
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
    using ui::ButtonStyle;

    namespace
    {
        constexpr int kTabCount = 8;

        // a key cap and what it does, for the footer
        float prompt(float x, float cy, std::string_view key, std::string_view what)
        {
            const float keyW = std::max(px(28), measure(kFontSemibold, px(12), key) + px(14));
            const Rect cap{x, cy - px(12), keyW, px(24)};
            rect(cap, rgb(0xFE7FB4), Corners(px(3)));
            rect({cap.x, cap.bottom() - px(2), cap.w, px(2)}, rgb(0xC91E63));
            ui::label(kFontSemibold, px(12), cap, rgb(0xFFFFFF), key, ui::Align::Centre);
            const float textX = cap.right() + px(8);
            const float textW = measure(kFontSemibold, px(13.5f), what);
            ui::label(kFontSemibold, px(13.5f), {textX, cy - px(12), textW + px(4), px(24)}, rgb(0xF2F8FA), what);
            return textX + textW + px(28);
        }

        // a pastel Vice City panel with a heading strip, as the option tables
        Rect panel(Rect r, std::string_view title)
        {
            rect(r, rgb(0xEDF8FB, 238));
            const Rect head{r.x, r.y, r.w, px(38)};
            rect(head, rgb(0xFE7FB4));
            ui::label(kFontSemibold, px(14), head.inset(px(16), 0), rgb(0xFFFFFF), title);
            return {r.x, head.bottom(), r.w, r.h - head.h};
        }

        // ---- Map ------------------------------------------------------------------------------------------------
        struct Blip
        {
            float x, y; // 0..1 across the map
            uint32_t color;
            const char *pt, *en;
        };

        void drawMap(Rect r)
        {
            const float legendW = px(330);
            const Rect mapR{r.x, r.y, r.w - legendW - px(16), r.h};
            // the city: the game's map textures go here (the phase 5 work); a stand-in sea, islands and grid now
            rect(mapR, rgb(0x0B4F6C));
            rect({mapR.x, mapR.y, mapR.w, px(2)}, rgb(0x27FDF5, 120));
            auto island = [&](float x0, float y0, float x1, float y1, float corner) {
                const Rect i{mapR.x + mapR.w * x0, mapR.y + mapR.h * y0, mapR.w * (x1 - x0), mapR.h * (y1 - y0)};
                rect(i, rgb(0xE9DDB8), Corners(corner));
                rect(i.inset(px(10), px(10)), rgb(0x9DBF8A), Corners(corner * 0.7f));
            };
            island(0.08f, 0.10f, 0.42f, 0.88f, px(40)); // the mainland
            island(0.55f, 0.06f, 0.70f, 0.55f, px(30)); // the beach islands
            island(0.62f, 0.58f, 0.80f, 0.92f, px(30));
            island(0.46f, 0.40f, 0.52f, 0.50f, px(14)); // Starfish Island
            for (int i = 1; i < 10; ++i)
            {
                rect({mapR.x + mapR.w * float(i) / 10.0f, mapR.y, px(1), mapR.h}, rgb(0xFFFFFF, 28));
                rect({mapR.x, mapR.y + mapR.h * float(i) / 10.0f, mapR.w, px(1)}, rgb(0xFFFFFF, 28));
            }
            static const Blip kBlips[] = {
                {0.30f, 0.62f, 0xFE7FB4, "Você", "You"},
                {0.22f, 0.70f, 0xFFFFFF, "Esconderijo", "Safehouse"},
                {0.36f, 0.30f, 0xF5B544, "Missão: Martinez", "Mission: Martinez"},
                {0.63f, 0.24f, 0x3DDBC6, "Ammu-Nation", "Ammu-Nation"},
                {0.70f, 0.74f, 0x3D8BDB, "Pay 'n' Spray", "Pay 'n' Spray"},
                {0.18f, 0.40f, 0xF26868, "Hospital", "Hospital"},
                {0.66f, 0.40f, 0x2E6BFF, "Polícia", "Police"},
            };
            for (const Blip &b : kBlips)
            {
                const float cx = mapR.x + mapR.w * b.x, cy = mapR.y + mapR.h * b.y;
                circle(cx, cy, px(9), rgb(0x0A0A0B, 200));
                circle(cx, cy, px(7), rgb(b.color));
            }
            // the marked destination
            const float tx = mapR.x + mapR.w * 0.48f, ty = mapR.y + mapR.h * 0.18f;
            line(tx - px(12), ty, tx + px(12), ty, px(2.5f), rgb(0xFF1493));
            line(tx, ty - px(12), tx, ty + px(12), px(2.5f), rgb(0xFF1493));
            ui::label(kFontSemibold, px(12), {mapR.x + px(12), mapR.bottom() - px(30), mapR.w - px(24), px(22)},
                      rgb(0xFFFFFF, 200),
                      tr("Prévia de layout: o mapa real vem das texturas do próprio jogo",
                         "Layout preview: the real map comes from the game's own textures"));

            const Rect legendR{mapR.right() + px(16), r.y, legendW, r.h};
            const Rect body = panel(legendR, tr("LEGENDA", "LEGEND"));
            float y = body.y;
            int idx = 0;
            for (const Blip &b : kBlips)
            {
                const Rect row{body.x, y, body.w, px(40)};
                ui::vcActionRow(std::string("#gm-legend-") + b.en, row, idx++, "");
                circle(row.x + px(24), row.y + row.h * 0.5f, px(7), rgb(b.color));
                ui::label(kFontSemibold, px(14), {row.x + px(44), row.y, row.w - px(50), row.h}, rgb(0x00566A),
                          tr(b.pt, b.en));
                y += px(40);
            }
            y += px(14);
            const char *hints[] = {
                tr("Roda / RB LB: zoom", "Wheel / RB LB: zoom"),
                tr("Arrastar / analógico: mover", "Drag / stick: move"),
                tr("Botão direito / X: marcar destino", "Right button / X: set destination"),
            };
            for (const char *h : hints)
            {
                ui::label(kFontSemibold, px(13), {body.x + px(16), y, body.w - px(32), px(24)}, rgb(0x00647C), h);
                y += px(26);
            }
        }

        // ---- Brief ----------------------------------------------------------------------------------------------
        void drawBrief(Rect r)
        {
            struct Line
            {
                const char *who, *pt, *en;
            };
            static const Line kLines[] = {
                {"Martinez", "Vá até a loja de bebidas em Little Havana e lembre ao dono quem manda no bairro.",
                 "Go to the liquor store in Little Havana and remind the owner who runs the neighbourhood."},
                {"", "Destrua as vitrines para chamar a atenção dele.", "Smash the windows to get his attention."},
                {"Phil", "Os Cholos estão no armazém. Pegue o caminhão e traga a carga de volta.",
                 "The Cholos are at the warehouse. Take the truck and bring the cargo back."},
                {"", "Você está sendo procurado. Despiste a polícia.", "You're wanted. Lose the cops."},
                {"Louise", "Me encontre no Hotel Harrison às 21h.", "Meet me at the Hotel Harrison at 9 pm."},
            };
            const Rect body = panel(r, tr("ÚLTIMAS MENSAGENS DA MISSÃO", "RECENT MISSION MESSAGES"));
            float y = body.y + px(6);
            int idx = 0;
            for (const Line &l : kLines)
            {
                const std::string_view text = tr(l.pt, l.en);
                const float textH = ui::paragraphHeight(kFontSemibold, px(15), body.w - px(220), text);
                const float h = std::max(px(48), textH + px(24));
                const Rect row{body.x, y, body.w, h};
                rect(row, idx++ % 2 == 0 ? rgb(0xEDF9FC, 185) : rgb(0xDDF1F7, 185));
                if (*l.who)
                    ui::label(kFontSemibold, px(14), {row.x + px(18), row.y, px(170), px(48)}, rgb(0xFF1493), l.who);
                ui::paragraph(kFontSemibold, px(15), {row.x + px(200), row.y + px(12), row.w - px(220), textH},
                              rgb(0x00566A), text);
                y += h;
            }
            vcFillStripes(body, y, px(48), idx);
        }

        // ---- Stats ----------------------------------------------------------------------------------------------
        void drawStats(Rect r)
        {
            struct Stat
            {
                const char *pt, *en, *value;
            };
            static const Stat kLeft[] = {
                {"Progresso da história", "Story progress", "23%"},
                {"Missões concluídas", "Missions passed", "21 / 54"},
                {"Tempo de jogo", "Time played", "6 h 42 min"},
                {"Dinheiro gasto em armas", "Money spent on weapons", "$ 18.450"},
                {"Pacotes escondidos", "Hidden packages", "12 / 99"},
                {"Saltos únicos", "Unique jumps", "4 / 30"},
                {"Império: negócios", "Empire: businesses", "3"},
            };
            static const Stat kRight[] = {
                {"Pessoas eliminadas", "People killed", "312"},
                {"Veículos destruídos", "Vehicles destroyed", "57"},
                {"Tiros disparados", "Shots fired", "4.802"},
                {"Precisão", "Accuracy", "38%"},
                {"Maior nível de procurado", "Highest wanted level", "4"},
                {"Distância a pé", "Distance on foot", "41,2 km"},
                {"Distância de carro", "Distance by car", "287,9 km"},
            };
            const float gap = px(16), colW = (r.w - gap) * 0.5f;
            auto column = [&](Rect c, const char *pt, const char *en, const Stat *stats, int n) {
                const Rect body = panel(c, tr(pt, en));
                float y = body.y;
                int idx = 0;
                for (int i = 0; i < n; ++i)
                {
                    ui::vcActionRow(std::string("#gm-stat-") + stats[i].en, {body.x, y, body.w, px(44)}, idx++,
                                    tr(stats[i].pt, stats[i].en), stats[i].value, rgb(0xFF1493));
                    y += px(44);
                }
                vcFillStripes(body, y, px(44), idx);
            };
            column({r.x, r.y, colW, r.h}, "PROGRESSO", "PROGRESS", kLeft, 7);
            column({r.x + colW + gap, r.y, colW, r.h}, "COMBATE E ESTRADA", "COMBAT AND ROAD", kRight, 7);
        }

        // ---- Game: saves ----------------------------------------------------------------------------------------
        void drawGame(Rect r)
        {
            struct Slot
            {
                const char *name, *when, *where;
                int percent;
            };
            static const Slot kSlots[] = {
                {"Cholo Vitorioso", "17/11/2026 09:12", "Little Havana", 23},
                {"Matar Phil: Parte 2", "16/11/2026 22:40", "Fort Baxter", 21},
                {"Shakedown", "15/11/2026 19:05", "Little Havana", 14},
                {"", "", "", 0},
                {"", "", "", 0},
                {"", "", "", 0},
                {"", "", "", 0},
                {"", "", "", 0},
            };
            const float detailW = px(420);
            const Rect listR{r.x, r.y, r.w - detailW - px(16), r.h};
            const Rect body = panel(listR, tr("MEMORY CARD (PS2) · 8 ESPAÇOS", "MEMORY CARD (PS2) · 8 SLOTS"));
            float y = body.y;
            int idx = 0;
            for (int i = 0; i < 8; ++i)
            {
                const Slot &s = kSlots[i];
                const bool empty = !*s.name;
                const std::string title = std::to_string(i + 1) + ".  " + (empty ? std::string(tr("Vazio", "Empty")) : s.name);
                const Rect row{body.x, y, body.w, px(50)};
                if (ui::vcActionRow(std::string("#gm-slot-") + std::to_string(i), row, idx++, title,
                                    empty ? "" : s.when, rgb(0x00647C)))
                    shell.gameMenuSlot = i;
                if (shell.gameMenuSlot == i)
                    rect({row.x, row.y, px(5), row.h}, rgb(0xFF1493));
                y += px(50);
            }
            vcFillStripes(body, y, px(50), idx);

            // the selected slot, and what can be done with it
            const Slot &sel = kSlots[shell.gameMenuSlot];
            const Rect detR{listR.right() + px(16), r.y, detailW, r.h};
            const Rect d = panel(detR, tr("ESPAÇO SELECIONADO", "SELECTED SLOT"));
            float dy = d.y + px(18);
            const bool empty = !*sel.name;
            ui::label(kFontBrand, px(30), {d.x + px(20), dy, d.w - px(40), px(40)}, rgb(0xFF1493),
                      empty ? tr("Espaço vazio", "Empty slot") : sel.name);
            dy += px(52);
            if (!empty)
            {
                const std::string pct = std::to_string(sel.percent) + "%";
                const char *names[][2] = {{"Salvo em", "Saved"}, {"Local", "Place"}, {"Progresso", "Progress"}};
                const std::string values[] = {sel.when, sel.where, pct};
                for (int i = 0; i < 3; ++i)
                {
                    ui::label(kFontSemibold, px(14), {d.x + px(20), dy, px(140), px(26)}, rgb(0x00647C), tr(names[i][0], names[i][1]));
                    ui::label(kFontSemibold, px(15), {d.x + px(160), dy, d.w - px(180), px(26)}, rgb(0x00566A), values[i]);
                    dy += px(32);
                }
            }
            dy += px(16);
            const float bw = d.w - px(40);
            ui::button(tr("Salvar aqui", "Save here"), {d.x + px(20), dy, bw, px(44)}, ButtonStyle::Primary);
            dy += px(54);
            ui::button(tr("Carregar", "Load"), {d.x + px(20), dy, bw, px(44)}, ButtonStyle::Secondary, 0, !empty);
            dy += px(54);
            ui::button(tr("Apagar", "Delete"), {d.x + px(20), dy, bw, px(44)}, ButtonStyle::Danger, 0, !empty);
            ui::label(kFontSemibold, px(12.5f), {d.x + px(20), d.bottom() - px(118), bw, px(22)}, rgb(0x00647C),
                      tr("Salvar fora de um esconderijo não é permitido pelo jogo.",
                         "The game only lets you save at a safehouse."));
            ui::button(tr("Sair para o Windows", "Quit to Windows"), {d.x + px(20), d.bottom() - px(84), bw, px(44)},
                       ButtonStyle::Secondary);
        }

        // ---- Trainer --------------------------------------------------------------------------------------------
        void drawTrainer(Rect r)
        {
            static int god = 0, wanted = 0, vehicle = 3, place = 0, weather = 0, hour = 14, speed = 1;
            const Rect body = panel(r, tr("TRAINER · NADA DISTO É SALVO", "TRAINER · NOTHING HERE IS SAVED"));
            float y = body.y;
            int idx = 0;
            const float h = px(46);
            auto next = [&]() { y += h; return Rect{body.x, y - h, body.w, h}; };
            ui::vcCycleRow("#gm-tr-god", next(), idx++, tr("Modo Deus", "God mode"), {tr("Não", "Off"), tr("Sim", "On")}, god);
            ui::vcSliderRow("#gm-tr-wanted", next(), idx++, tr("Nível de procurado", "Wanted level"), wanted, 0, 6, 1);
            ui::vcActionRow("#gm-tr-money", next(), idx++, tr("Dinheiro", "Money"), "+ $ 10.000", rgb(0xFF1493));
            ui::vcActionRow("#gm-tr-weapons", next(), idx++, tr("Armas e munição", "Weapons and ammo"),
                            tr("Pacote completo", "Full set"), rgb(0xFF1493));
            ui::vcCycleRow("#gm-tr-vehicle", next(), idx++, tr("Gerar veículo", "Spawn a vehicle"),
                           {"Admiral", "Banshee", "Cheetah", "Infernus", "PCJ-600", "Squalo", "Maverick"}, vehicle);
            ui::vcCycleRow("#gm-tr-place", next(), idx++, tr("Teletransportar", "Teleport"),
                           {"Little Havana", "Downtown", "Ocean Beach", "Vice Point", "Fort Baxter", "Escobar"}, place);
            ui::vcSliderRow("#gm-tr-hour", next(), idx++, tr("Hora do dia", "Time of day"), hour, 0, 23, 1, "h");
            ui::vcCycleRow("#gm-tr-weather", next(), idx++, tr("Clima", "Weather"),
                           {tr("Ensolarado", "Sunny"), tr("Nublado", "Cloudy"), tr("Chuva", "Rain"), tr("Neblina", "Fog")}, weather);
            ui::vcCycleRow("#gm-tr-speed", next(), idx++, tr("Velocidade do jogo", "Game speed"), {"0.5x", "1x", "2x"}, speed);
            vcFillStripes(body, y, h, idx);
        }
    }

    void drawGameMenu(Rect window)
    {
        // behind: the game's last frame, frozen (in the game: the real one); the launcher's art stands in here
        if (imageInfo(ImageId::Background).valid)
            imageCover(window, ImageId::Background);
        rect(window, rgb(0x07040A, 214));
        gradient({window.x, window.y, window.w, px(170)}, rgb(0x3A0E24, 200), rgb(0x3A0E24, 0));

        const Rect area = window.inset(px(38), px(18));
        // header: the game's logo, PAUSADO, where the story stands
        const float headH = px(76);
        float logoW = 0;
        if (imageInfo(ImageId::GameLogo).valid)
        {
            const float lh = headH;
            logoW = lh * imageInfo(ImageId::GameLogo).aspect;
            image({area.x, area.y, logoW, lh}, ImageId::GameLogo);
        }
        const float tx = area.x + logoW + px(22);
        ui::label(kFontBrand, px(40), {tx, area.y + px(2), px(400), px(46)}, rgb(0xFE7FB4), tr("Pausado", "Paused"));
        ui::label(kFontSemibold, px(14), {tx, area.y + px(48), px(700), px(24)}, rgb(0xF2F8FA, 220),
                  tr("Missão: Shakedown  ·  Little Havana  ·  14:32  ·  $ 12.480",
                     "Mission: Shakedown  ·  Little Havana  ·  2:32 pm  ·  $ 12,480"));

        // the pages
        const char *tabs[kTabCount] = {
            tr("MAPA", "MAP"),        tr("MISSÃO", "BRIEF"),          tr("ESTATÍSTICAS", "STATS"), tr("JOGO", "GAME"),
            tr("CONFIGURAÇÕES", "OPTIONS"), tr("CONTROLES", "CONTROLS"), "MODS",                  "TRAINER",
        };
        const Rect tabBar{area.x, area.y + headH + px(14), area.w, px(48)};
        vcCategoryBar(tabBar, tabs, kTabCount, shell.gameMenuTab);

        const float footH = px(44);
        const Rect content{area.x, tabBar.bottom() + px(10), area.w, area.bottom() - footH - px(10) - tabBar.bottom() - px(10)};
        // the pages are drawn for the launcher's light sky: an opaque pastel under them, so their translucent stripes
        // read the same over the dark frozen frame
        rect(content, rgb(0xDDF1F7));
        switch (shell.gameMenuTab)
        {
        case 0: drawMap(content); break;
        case 1: drawBrief(content); break;
        case 2: drawStats(content); break;
        case 3: drawGame(content); break;
        case 4: drawConfig(content.inset(-px(38), -px(10))); break; // the launcher's pages carry their own margins
        case 5: drawControls(content.inset(-px(38), -px(10))); break;
        case 6: drawMods(content.inset(-px(38), -px(10))); break;
        default: drawTrainer(content); break;
        }

        // footer: what the keys do (the gamepad's glyphs when one is in use)
        const float cy = area.bottom() - footH * 0.5f;
        float x = area.x;
        x = prompt(x, cy, "ESC", tr("Voltar ao jogo", "Back to the game"));
        x = prompt(x, cy, "Q  E", tr("Trocar de aba", "Switch tab"));
        x = prompt(x, cy, "↑ ↓", tr("Navegar", "Move"));
        prompt(x, cy, "ENTER", tr("Selecionar", "Select"));
    }
}
