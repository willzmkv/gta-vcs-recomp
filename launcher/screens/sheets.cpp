// Classic 1980s GTA: Vice City Menu implementation for MODS (drawMods), CONFIGURAÇÕES/OPTIONS (drawConfig),
// and SOBRE/ABOUT (drawAbout), plus legacy slide-over sheets (drawSheets).
#include "shell.h"

#include "model/os.h"
#include "model/scskiller.h"
#include "model/sound.h"
#include "model/system.h"
#include "platform/platform.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>

namespace fs = std::filesystem;

namespace screens
{
    using namespace render;
    using core::alphaOf;
    using core::mix;
    using core::rgb;
    using core::withAlpha;
    using ui::ButtonStyle;

    namespace
    {
        int indexInList(int value, const int *list, int count, int fallbackIdx = 0)
        {
            for (int i = 0; i < count; ++i)
                if (list[i] == value)
                    return i;
            return fallbackIdx;
        }

        // Draws the top horizontal Vice City sub-tab bar (flush rectangular cells, active = #FE7FB4 Flamingo Pink)
        void drawVcCategoryBar(Rect barR, const char *const *labels, int count, int &activeTab)
        {
            if (activeTab < 0 || activeTab >= count)
                activeTab = 0;
            const float cellW = std::floor(barR.w / float(std::max(1, count)));
            for (int i = 0; i < count; ++i)
            {
                const float x = barR.x + float(i) * cellW;
                const float w = (i + 1 == count) ? (barR.right() - x) : cellW;
                const Rect cell{x, barR.y, w, barR.h};
                const std::string idStr = std::string("#vc-subtab-") + std::to_string(i) + "-" + labels[i];
                if (ui::vcSubTab(idStr, cell, labels[i], activeTab == i))
                    activeTab = i;
            }
        }

        // Fills any remaining vertical space in tableR with subtle alternating Vice City stripes so the
        // menu sheet always connects cleanly to the lower performance inspector panel.
        void fillRemainingVcStripes(Rect tableR, float currentY, float rowH, int &rowIdx)
        {
            while (currentY + px(8) < tableR.bottom())
            {
                const float h = std::min(rowH, tableR.bottom() - currentY);
                const Color bg = (rowIdx % 2 == 0) ? rgb(0xEDF9FC, 185) : rgb(0xDDF1F7, 185);
                rect({tableR.x, currentY, tableR.w, h}, bg);
                currentY += h;
                ++rowIdx;
            }
        }

        struct VcOptionInspector
        {
            std::string title;
            std::string desc;
            std::string badge;
            int gpuDeltaPct = 0;
            int cpuDeltaPct = 0;
            float streamDeltaMb = 0.0f;
        };

        // Dedicated lower Vice City Performance Inspector & Centered GPU / CPU / STREAMING Panel.
        // Occupies the lower rectangular area below the options table.
        void drawVcInspectorPanel(Rect panelR, const VcOptionInspector &info,
                                  int totalGpuPct, int totalCpuPct, float totalStreamMb,
                                  float streamCapacityMb = 30.0f,
                                  std::string_view extraStatusRight = {})
        {
            // Outer Vice City sheet frame
            rect(panelR, rgb(0xE8F7FB, 238));
            rect({panelR.x, panelR.y, panelR.w, px(2)}, rgb(0x006A82, 160));
            rect({panelR.x, panelR.bottom() - px(2), panelR.w, px(2)}, rgb(0x006A82, 140));

            // Top section: Per-option performance impact inspector
            const float topH = px(64);
            const Rect topR{panelR.x, panelR.y + px(2), panelR.w, topH};
            rect(topR, rgb(0xF1FAFC, 242));
            // Left Flamingo Pink vertical accent bar
            rect({topR.x, topR.y, px(6), topR.h}, rgb(0xFE7FB4));

            // Option title (top-left)
            const float padL = topR.x + px(18);
            const float badgeW = info.badge.empty() ? 0.0f : (measure(kFontSemibold, px(12.0f), info.badge) + px(22));
            const Rect titleR{padL, topR.y + px(7), topR.w - px(36) - badgeW, px(22)};
            ui::label(kFontSemibold, px(13.5f), titleR, rgb(0x004E60), info.title, ui::Align::Left);

            // Option cost badge (top-right, sharp Vice City pink/teal tag)
            if (!info.badge.empty())
            {
                const Rect badgeR{topR.right() - badgeW - px(14), topR.y + px(7), badgeW, px(22)};
                rect(badgeR, rgb(0xFE7FB4));
                rect({badgeR.x, badgeR.bottom() - px(2), badgeR.w, px(2)}, rgb(0xC91E63));
                ui::label(kFontSemibold, px(11.5f), badgeR, rgb(0xFFFFFF), info.badge, ui::Align::Centre);
            }

            // Detailed option description (second line)
            const Rect descR{padL, topR.y + px(32), topR.w - px(32), px(24)};
            ui::label(kFontSemibold, px(12.5f), descR, rgb(0x00647C), info.desc, ui::Align::Left);

            // Divider between option inspector and centered hardware gauges
            const float divY = topR.bottom();
            rect({panelR.x, divY, panelR.w, px(1.5f)}, rgb(0x9FD3E2, 210));

            // Bottom section: Centered GPU, CPU, and STREAMING gauges
            const Rect botR{panelR.x, divY + px(1.5f), panelR.w, panelR.bottom() - (divY + px(1.5f)) - px(2)};
            rect(botR, rgb(0xD6EEF5, 238));

            const float gaugesTotalW = std::min(botR.w - px(40), px(780));
            const float startX = std::floor((botR.x + botR.w * 0.5f) - gaugesTotalW * 0.5f);
            const float gap = px(28);
            const float colW = std::floor((gaugesTotalW - gap * 2.0f) / 3.0f);

            struct GaugeCol
            {
                const char *title;
                std::string valueText;
                std::string subText;
                float frac;
                float deltaFrac;
            };

            char gpuVal[64], cpuVal[64], memVal[64];
            if (info.gpuDeltaPct != 0)
                std::snprintf(gpuVal, sizeof(gpuVal), "%d%% (%+d%%)", totalGpuPct, info.gpuDeltaPct);
            else
                std::snprintf(gpuVal, sizeof(gpuVal), "%d%%", totalGpuPct);

            if (info.cpuDeltaPct != 0)
                std::snprintf(cpuVal, sizeof(cpuVal), "%d%% (%+d%%)", totalCpuPct, info.cpuDeltaPct);
            else
                std::snprintf(cpuVal, sizeof(cpuVal), "%d%%", totalCpuPct);

            const float capMb = std::max(1.0f, streamCapacityMb);
            if (std::fabs(info.streamDeltaMb) >= 0.05f)
                std::snprintf(memVal, sizeof(memVal), "%.1f MB (%+.1f MB)", totalStreamMb, info.streamDeltaMb);
            else
                std::snprintf(memVal, sizeof(memVal), "%.1f MB / %.0f MB", totalStreamMb, capMb);

            const GaugeCol cols[3] = {
                {
                    tr("CARGA DE GPU", "GPU LOAD"),
                    gpuVal,
                    tr("Placa de Vídeo (GS / Shaders)", "Graphics Card (GS / Shaders)"),
                    core::clamp01(float(totalGpuPct) / 100.0f),
                    core::clamp01(float(std::max(0, info.gpuDeltaPct)) / 100.0f),
                },
                {
                    tr("CARGA DE CPU", "CPU LOAD"),
                    cpuVal,
                    tr("Processador (Mundo / IA / Física)", "Processor (World / AI / Physics)"),
                    core::clamp01(float(totalCpuPct) / 100.0f),
                    core::clamp01(float(std::max(0, info.cpuDeltaPct)) / 100.0f),
                },
                {
                    "STREAMING",
                    memVal,
                    extraStatusRight.empty()
                        ? std::string(tr("Pool de Memória & Mods Ativos", "Memory Pool & Active Mods"))
                        : std::string(extraStatusRight),
                    core::clamp01(totalStreamMb / capMb),
                    core::clamp01(std::max(0.0f, info.streamDeltaMb) / capMb),
                },
            };

            for (int i = 0; i < 3; ++i)
            {
                const float cx = startX + float(i) * (colW + gap);
                const Rect cR{cx, botR.y + px(6), colW, botR.h - px(10)};

                // Vertical separator between centered columns
                if (i > 0)
                {
                    const float sepX = std::floor(cx - gap * 0.5f);
                    rect({sepX, botR.y + px(10), px(1.5f), botR.h - px(20)}, rgb(0x006A82, 65));
                }

                // Centered Header: "GPU  •  51%"
                const std::string headerStr = std::string(cols[i].title) + "  •  " + cols[i].valueText;
                ui::label(kFontSemibold, px(13.0f), {cR.x, cR.y, cR.w, px(20)},
                          rgb(0x004E60), headerStr, ui::Align::Centre);

                // Centered Pink Progress Bar
                const float barW = std::min(cR.w - px(16), px(210));
                const float barH = px(10);
                const Rect barR{std::floor((cR.x + cR.w * 0.5f) - barW * 0.5f), cR.y + px(24), barW, barH};
                rectEx(barR, rgb(0xB4DCE7), rgb(0xB4DCE7), {}, rgb(0x00586E, 130), px(1.5f));

                const float fillW = std::round((barR.w - px(2)) * cols[i].frac);
                if (fillW > 0.5f)
                {
                    const Rect fillR{barR.x + px(1), barR.y + px(1), fillW, barR.h - px(2)};
                    gradient(fillR, rgb(0xFF2A85), rgb(0xFE7FB4), {}, Gradient::Horizontal);
                    // Highlight delta portion on right edge of fill if hovering an option with positive cost
                    if (cols[i].deltaFrac > 0.005f)
                    {
                        const float dW = std::min(fillW, std::max(px(4), std::round((barR.w - px(2)) * cols[i].deltaFrac)));
                        rect({fillR.right() - dW, fillR.y, dW, fillR.h}, rgb(0x27FDF5));
                    }
                }

                // Centered Subtitle
                ui::label(kFontSemibold, px(11.0f), {cR.x, barR.bottom() + px(4), cR.w, px(18)},
                          rgb(0x00647C), cols[i].subText, ui::Align::Centre);
            }
        }
    }

    // the Vice City sub-tab bar and table stripes for other screens (the game menu, gamemenu.cpp)
    void vcCategoryBar(Rect barR, const char *const *labels, int count, int &activeTab)
    {
        drawVcCategoryBar(barR, labels, count, activeTab);
    }
    void vcFillStripes(Rect tableR, float currentY, float rowH, int &rowIdx)
    {
        fillRemainingVcStripes(tableR, currentY, rowH, rowIdx);
    }

    // =================================================================================================================
    // TAB 2: MODS — Classic 1980s GTA: Vice City Menu Style
    // Layout requested by user: [BOTÃO DE LIGA/DESLIGA] - [NOME DO MOD] - [SLIDER ROSA] on the same page
    // =================================================================================================================
    void drawMods(Rect r)
    {
        App &app = *shell.app;
        launcher::GameConfig &cfg = app.gameConfig;
        const Rect area = r.inset(px(38), px(10));

        const char *subTabs[3] = {
            tr("TODOS OS MODS", "ALL MODS"),
            tr("COMPATÍVEIS", "COMPATIBLE"),
            tr("INCOMPATÍVEIS", "INCOMPATIBLE"),
        };
        const Rect tabBar{area.x, area.y, area.w, px(46)};
        drawVcCategoryBar(tabBar, subTabs, 3, shell.modsSubTab);

        const float inspH = px(140);
        const Rect inspR{area.x, area.bottom() - inspH, area.w, inspH};
        const Rect tableR{area.x, tabBar.bottom() + px(6), area.w, inspR.y - tabBar.bottom() - px(12)};

        int compatTotal = 0, incompatTotal = 0, activeTotal = 0;
        for (const auto &m : cfg.mods)
        {
            if (m.compat == launcher::ModCompat::Incompatible)
                ++incompatTotal;
            else
            {
                ++compatTotal;
                if (m.enabled)
                    ++activeTotal;
            }
        }

        VcOptionInspector inspector{
            tr("GERENCIADOR DE MODS & PRIORIDADES (MODLOADER)", "MODLOADER & PRIORITY MANAGER"),
            tr("Use o botão à esquerda para ligar/desligar o mod e arraste o slider rosa à direita para ajustar a prioridade (1..100).",
               "Use the left toggle button to enable/disable a mod and drag the pink slider on the right to set priority (1..100)."),
            tr("BOTÃO LIGA/DESLIGA  •  MOD  •  SLIDER", "TOGGLE  •  MOD NAME  •  PINK SLIDER"),
            0,
            0,
            float(activeTotal) * 0.8f,
        };

        const ui::Id scrollId = ui::id("#vc-mods-scroll", shell.modsSubTab);
        ui::beginScroll(scrollId, tableR);
        float y = tableR.y;
        const float rowH = px(44);
        int rowIdx = 0;

        // Global ModLoader switch & HD Texture switch at the top of the page
        if (shell.modsSubTab != 2)
        {
            {
                int enIdx = cfg.modsEnabled ? 1 : 0;
                bool hov = false;
                if (ui::vcCycleRow("#mod-opt-master", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Mod Loader Global", "Global Mod Loader"),
                                   {tr("Desligado", "Off"), tr("Ligado", "On")}, enIdx, &hov))
                {
                    cfg.modsEnabled = enIdx;
                    cfg.recalculateStreamingMemory();
                    app.saveGameConfig();
                }
                if (hov)
                {
                    inspector.title = tr("MOD LOADER GLOBAL", "GLOBAL MOD LOADER");
                    inspector.desc = tr("Ativa ou desativa o carregamento de todas as pastas de mods dentro de modloader/ ao iniciar o jogo.",
                                        "Enables or disables loading all mod folders inside modloader/ when launching the game.");
                    inspector.badge = cfg.modsEnabled ? tr("ATIVO", "ENABLED") : tr("DESATIVADO", "DISABLED");
                }
                y += rowH;
            }
            if (cfg.hasTextureMods())
            {
                int texIdx = cfg.graphicsTextureReplacements ? 1 : 0;
                bool hov = false;
                if (ui::vcCycleRow("#mod-opt-tex", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Substituição de Texturas HD", "HD Texture Replacements"),
                                   {tr("Desligado", "Off"), tr("Ligado", "On")}, texIdx, &hov))
                {
                    cfg.graphicsTextureReplacements = texIdx;
                    cfg.recalculateStreamingMemory();
                    app.saveGameConfig();
                }
                if (hov)
                {
                    inspector.title = tr("SUBSTITUIÇÃO DE TEXTURAS HD", "HD TEXTURE REPLACEMENTS");
                    inspector.desc = tr("Permite substituir texturas originais do PS2 por pacotes de texturas HD em alta resolução.",
                                        "Allows replacing original PS2 textures with high-resolution PNG or DDS texture packs.");
                    inspector.badge = tr("IMPACTO GPU: +4%  •  STREAMING: +2.0 MB", "GPU COST: +4%  •  STREAMING: +2.0 MB");
                    inspector.gpuDeltaPct = 4;
                    inspector.streamDeltaMb = 2.0f;
                }
                y += rowH;
            }
        }

        // Unified Mod rows: [BOTÃO DE TOGGLE] - [NOME DO MOD] - [SLIDER ROSA]
        int shownMods = 0;
        for (size_t i = 0; i < cfg.mods.size(); ++i)
        {
            launcher::ModEntry &m = cfg.mods[i];
            const bool isIncompat = (m.compat == launcher::ModCompat::Incompatible);
            if (shell.modsSubTab == 1 && isIncompat)
                continue;
            if (shell.modsSubTab == 2 && !isIncompat)
                continue;

            ++shownMods;
            bool hov = false;
            const Rect rowR{tableR.x, y, tableR.w, rowH};
            if (isIncompat)
            {
                const std::string rightTag = tr("Incompatível (x86)", "Incompatible (x86)");
                ui::vcActionRow("#mod-inc-" + m.key, rowR, rowIdx++, m.folderName, rightTag, rgb(0xC91E63), &hov, true);
                if (hov)
                {
                    inspector.title = m.folderName + tr(" — INCOMPATÍVEL", " — INCOMPATIBLE");
                    inspector.desc = m.summary + " • " + m.detail;
                    inspector.badge = tr("INCOMPATÍVEL", "INCOMPATIBLE");
                }
            }
            else
            {
                const std::string compatTag = (m.compat == launcher::ModCompat::Partial)
                                                  ? tr("  •  Parcial", "  •  Partial")
                                                  : tr("  •  Compatível", "  •  Compatible");
                const std::string title = m.folderName + compatTag;
                if (ui::vcModRow("#mod-row-" + m.key, rowR, rowIdx++, m.enabled,
                                 title, m.priority, 1, 100, 1, "",
                                 &hov, cfg.modsEnabled != 0))
                {
                    cfg.modStates[m.key] = m.enabled ? 1 : 0;
                    cfg.recalculateStreamingMemory();
                    app.saveGameConfig();
                }
                if (hov)
                {
                    inspector.title = m.folderName + (m.enabled ? tr(" — MOD ATIVO", " — ACTIVE MOD")
                                                                : tr(" — MOD DESATIVADO", " — MOD DISABLED"));
                    inspector.desc = m.summary + " • " +
                                     tr("Prioridade maior no slider rosa prevalece quando dois mods alteram o mesmo arquivo.",
                                        "Higher pink slider priority wins when two mods replace the same file.");
                    char bbuf[96];
                    std::snprintf(bbuf, sizeof(bbuf),
                                  app.isPt() ? "PRIORIDADE: %d" : "PRIORITY: %d",
                                  m.priority);
                    inspector.badge = bbuf;
                    inspector.gpuDeltaPct = m.enabled ? 0 : 2;
                    inspector.cpuDeltaPct = m.enabled ? 0 : 1;
                    inspector.streamDeltaMb = m.enabled ? -1.2f : +1.2f;
                }
            }
            y += rowH;
        }

        if (shownMods == 0)
        {
            ui::vcActionRow("#mod-empty", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                            tr("Nenhum mod encontrado nesta categoria",
                               "No mods found in this category"),
                            "", rgb(0x007A96));
            y += rowH;
        }

        // Standard Vice City bottom table actions
        {
            bool hov = false;
            if (ui::vcActionRow("#mod-open-dir", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                tr("Abrir pasta de mods", "Open mods folder"),
                                tr("< Abrir Pasta >", "< Open Folder >"), 0, &hov))
            {
                std::error_code ec;
                const fs::path modsDir = launcher::GameConfig::modloaderDir(app.root, app.build);
                fs::create_directories(modsDir, ec);
                launcher::openFolder(modsDir);
            }
            if (hov)
            {
                inspector.title = tr("PASTA DE MODS", "MODS DIRECTORY");
                inspector.desc = tr("Abre a pasta modloader no Windows Explorer para instalar novas pastas de mods.",
                                    "Opens the modloader folder in Windows Explorer to install new mod folders.");
                inspector.badge = tr("EXPLORADOR", "EXPLORER");
            }
            y += rowH;
        }
        {
            bool hov = false;
            if (ui::vcActionRow("#mod-refresh-list", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                tr("Atualizar lista de mods", "Refresh mod list"),
                                tr("< Atualizar >", "< Refresh >"), 0, &hov))
            {
                app.refreshMods();
                toast(tr("Lista de mods atualizada.", "Mod list refreshed."));
            }
            if (hov)
            {
                inspector.title = tr("REESCANEAR PASTA DE MODS", "RESCAN MODS FOLDER");
                inspector.desc = tr("Verifica todas as subpastas de mods e classifica automaticamente a compatibilidade.",
                                    "Scans all mod subfolders and automatically classifies mod compatibility.");
                inspector.badge = tr("VERIFICAÇÃO AUTOMÁTICA", "AUTO SCAN");
            }
            y += rowH;
        }

        fillRemainingVcStripes(tableR, y, rowH, rowIdx);
        ui::endScroll(scrollId, y);

        char rightStat[128];
        std::snprintf(rightStat, sizeof(rightStat),
                      app.isPt() ? "Mods Ativos: %d/%d  •  Bloqueados: %d" : "Active Mods: %d/%d  •  Blocked: %d",
                      activeTotal, compatTotal, incompatTotal);
        drawVcInspectorPanel(inspR, inspector,
                             cfg.estimatedGpuCostPercent(),
                             cfg.estimatedCpuCostPercent(),
                             cfg.estimatedStreamingLoadMb(),
                             cfg.streamingPoolCapacityMb(),
                             rightStat);
    }

    // =================================================================================================================
    // TAB 3: CONTROLES / CONTROLS — Classic 1980s GTA: Vice City Visual Layout & Mapping Menu
    // Visual layouts with brand-neutral controller illustrations (PS2, Xbox, PS4/PS5, Switch) + Settings & Keybindings
    // =================================================================================================================
    void drawControls(Rect r)
    {
        App &app = *shell.app;
        launcher::GameConfig &cfg = app.gameConfig;
        const Rect area = r.inset(px(38), px(10));

        const char *subTabs[7] = {
            "PS2",
            "XBOX",
            "DUALSHOCK 4",
            "DUALSENSE (PS5)",
            "NINTENDO SWITCH",
            tr("TECLADO & MOUSE", "KEYBOARD & MOUSE"),
            tr("AJUSTES & TECLAS", "SETTINGS & KEYS"),
        };
        const Rect tabBar{area.x, area.y, area.w, px(46)};
        drawVcCategoryBar(tabBar, subTabs, 7, shell.controlsSubTab);

        const float inspH = px(140);
        const Rect inspR{area.x, area.bottom() - inspH, area.w, inspH};
        const Rect tableR{area.x, tabBar.bottom() + px(6), area.w, inspR.y - tabBar.bottom() - px(12)};

        VcOptionInspector inspector{
            tr("LAYOUT & CONFIGURAÇÕES DE CONTROLE", "CONTROLLER LAYOUT & SETTINGS"),
            tr("Visualize os comandos de cada botão no controle selecionado ou alterne para Ajustes & Mapeamento para personalizar os gatilhos e sensibilidade.",
               "Inspect button bindings on the selected controller or switch to Mapping & Settings to customize triggers and sensitivity."),
            tr("SDL3 CONTROLLER API", "SDL3 CONTROLLER API"),
            0,
            0,
            0.0f,
        };

        if (shell.controlsSubTab >= 0 && shell.controlsSubTab <= 5)
        {
            // Banner at top of the diagram view
            const float bannerH = px(38);
            const Rect bannerR{tableR.x, tableR.y, tableR.w, bannerH};
            rect(bannerR, rgb(0xE2F4F8, 220));
            rect({bannerR.x, bannerR.bottom() - px(1.5f), bannerR.w, px(1.5f)}, rgb(0x006A82, 90));

            if (shell.controlsSubTab <= 4)
            {
                ui::label(kFontSemibold, px(12.5f), {bannerR.x + px(16), bannerR.y, px(170), bannerR.h},
                          rgb(0x004E60), tr("PREDEFINIÇÃO ATIVA:", "ACTIVE PRESET:"), ui::Align::Left);

                const float pillW = px(248);
                const float pillH = px(28);
                const float pillY = bannerR.y + px(5);

                // Pill 1: Clássico PS2
                const Rect p1R{bannerR.x + px(180), pillY, pillW, pillH};
                const bool p1Active = (cfg.padPreset == 1);
                const ui::Interaction inP1 = ui::interact(ui::id("#ctrl-preset-ps2"), p1R, true);
                const Color p1Bg = p1Active ? rgb(0xFE7FB4) : core::mix(rgb(0xF4FCFD), rgb(0xDEF2F6), inP1.w->hover);
                const Color p1Txt = p1Active ? rgb(0xFFFFFF) : rgb(0x00586E);
                rectEx(p1R, p1Bg, p1Bg, px(1.5f), p1Active ? rgb(0xC91E63) : rgb(0x006A82, 80), px(1.2f));
                ui::label(kFontSemibold, px(11.0f), p1R, p1Txt,
                          tr("1. CLÁSSICO PS2 (III / VC / SA / VCS)", "1. CLASSIC PS2 (III / VC / SA / VCS)"), ui::Align::Centre);
                if (inP1.clicked && !p1Active)
                {
                    cfg.applyPadPreset(1);
                    app.saveGameConfig();
                    sound::play(sound::Sfx::Nav);
                }

                // Pill 2: Moderno GTA IV / V
                const Rect p2R{p1R.right() + px(10), pillY, pillW, pillH};
                const bool p2Active = (cfg.padPreset == 2);
                const ui::Interaction inP2 = ui::interact(ui::id("#ctrl-preset-modern"), p2R, true);
                const Color p2Bg = p2Active ? rgb(0xFE7FB4) : core::mix(rgb(0xF4FCFD), rgb(0xDEF2F6), inP2.w->hover);
                const Color p2Txt = p2Active ? rgb(0xFFFFFF) : rgb(0x00586E);
                rectEx(p2R, p2Bg, p2Bg, px(1.5f), p2Active ? rgb(0xC91E63) : rgb(0x006A82, 80), px(1.2f));
                ui::label(kFontSemibold, px(11.0f), p2R, p2Txt,
                          tr("2. MODERNO (GTA IV / V - GATILHOS)", "2. MODERN (GTA IV / V - TRIGGERS)"), ui::Align::Centre);
                if (inP2.clicked && !p2Active)
                {
                    cfg.applyPadPreset(2);
                    app.saveGameConfig();
                    sound::play(sound::Sfx::Nav);
                }
            }
            else // Tab 5: Keyboard & Mouse
            {
                ui::label(kFontSemibold, px(12.5f), {bannerR.x + px(16), bannerR.y, px(240), bannerR.h},
                          rgb(0x004E60), tr("DISPOSITIVO DE ENTRADA:", "INPUT DEVICE:"), ui::Align::Left);

                const Rect tagR{bannerR.x + px(210), bannerR.y + px(5), px(280), px(28)};
                rectEx(tagR, rgb(0xFE7FB4), rgb(0xFE7FB4), px(1.5f), rgb(0xC91E63), px(1.2f));
                ui::label(kFontSemibold, px(11.0f), tagR, rgb(0xFFFFFF),
                          tr("TECLADO WASD + MOUSE 360° LIVRE", "KEYBOARD WASD + 360° FREE MOUSE"), ui::Align::Centre);

                ui::label(kFontBody, px(11.0f), {tagR.right() + px(16), bannerR.y, bannerR.right() - tagR.right() - px(24), bannerR.h},
                          rgb(0x006A82), tr("Reatribuição de teclas em Ajustes & Teclas.", "Full key rebinding in Settings & Keys."), ui::Align::Left);
            }

            // Diagram display area
            const Rect diagramR{tableR.x, bannerR.bottom(), tableR.w, tableR.bottom() - bannerR.bottom()};
            rect(diagramR, rgb(0xEDF9FC, 185));

            // Select image based on subTab
            ImageId imgId = ImageId::ControllerPs2;
            if (shell.controlsSubTab == 1)
                imgId = ImageId::ControllerXbox;
            else if (shell.controlsSubTab == 2)
                imgId = ImageId::ControllerPs4;
            else if (shell.controlsSubTab == 3)
                imgId = ImageId::ControllerPs5;
            else if (shell.controlsSubTab == 4)
                imgId = ImageId::ControllerSwitch;
            else if (shell.controlsSubTab == 5)
                imgId = ImageId::ControllerKbMouse;

            const bool isKbMouse = (shell.controlsSubTab == 5);
            const float cW = isKbMouse ? px(440) : px(390);
            const float cH = isKbMouse ? px(330) : px(292.5f);
            const Rect ctrlR{diagramR.cx() - cW * 0.5f, diagramR.cy() - cH * 0.5f - px(4), cW, cH};

            if (imageInfo(imgId).valid)
                image(ctrlR, imgId);
            else
                rectEx(ctrlR, rgb(0xD0EBF2), rgb(0xD0EBF2), px(8), rgb(0x006A82, 100), px(1.5f));

            auto drawCallout = [&](std::string_view idStr, Rect badgeR,
                                   std::string_view keyTag, std::string_view actionText,
                                   std::string_view inspTitle, std::string_view inspDesc,
                                   float normX, float normY, bool alignRight)
            {
                const ui::Interaction in = ui::interact(ui::id(idStr), badgeR, false);
                const float hov = in.w->hover;

                // Vice City Box: Crisp rectangle (1.5px radius), pale cyan bg, dark teal border
                const Color bg = core::mix(rgb(0xEDF8FB, 235), rgb(0xDEF2F6, 255), hov);
                const Color border = core::mix(rgb(0x006A82, 95), rgb(0xFE7FB4), hov);
                rectEx(badgeR, bg, bg, px(1.5f), border, px(1.2f));

                // Wire target coordinates on the controller
                const float targetX = ctrlR.x + normX * ctrlR.w;
                const float targetY = ctrlR.y + normY * ctrlR.h;

                // Wire start coordinates from callout card
                const float startX = alignRight ? badgeR.x : badgeR.right();
                const float startY = badgeR.cy();

                // 2-segment schematic leader line with elbow
                const float elbowX = alignRight ? (startX - px(16)) : (startX + px(16));
                const Color wireCol = core::mix(rgb(0x006A82, 110), rgb(0xFE7FB4, 255), hov);
                const float wireW = hov > 0.05f ? px(1.8f) : px(1.2f);

                line(startX, startY, elbowX, startY, wireW, wireCol);
                line(elbowX, startY, targetX, targetY, wireW, wireCol);

                // Cyan/Pink glow ring and white anchor center dot on button target
                circle(targetX, targetY, px(4.0f), core::mix(rgb(0x27FDF5, 200), rgb(0xFE7FB4, 255), hov));
                circle(targetX, targetY, px(2.0f), rgb(0xFFFFFF));

                if (hov > 0.05f)
                {
                    inspector.title = std::string(inspTitle);
                    inspector.desc = std::string(inspDesc);
                    inspector.badge = std::string(keyTag);
                }

                // Key tag badge: Rectangular pill, Vice City Flamingo Pink (#FE7FB4) or Teal on hover
                const float tagW = measure(kFontSemibold, px(10.5f), keyTag) + px(14);
                if (!alignRight)
                {
                    const Rect tagR{badgeR.x + px(4), badgeR.y + px(3.5f), tagW, badgeR.h - px(7)};
                    const Color tagBg = core::mix(rgb(0xFE7FB4), rgb(0x006A82), hov);
                    rect(tagR, tagBg, px(1.5f));
                    ui::label(kFontSemibold, px(10.0f), tagR, rgb(0xFFFFFF), keyTag, ui::Align::Centre);

                    const Rect txtR{tagR.right() + px(8), badgeR.y, badgeR.w - tagW - px(12), badgeR.h};
                    ui::label(kFontSemibold, px(10.5f), txtR, core::mix(rgb(0x004E60), rgb(0x002B36), hov), actionText, ui::Align::Left);
                }
                else
                {
                    const Rect tagR{badgeR.right() - tagW - px(4), badgeR.y + px(3.5f), tagW, badgeR.h - px(7)};
                    const Color tagBg = core::mix(rgb(0xFE7FB4), rgb(0x006A82), hov);
                    rect(tagR, tagBg, px(1.5f));
                    ui::label(kFontSemibold, px(10.0f), tagR, rgb(0xFFFFFF), keyTag, ui::Align::Centre);

                    const Rect txtR{badgeR.x + px(6), badgeR.y, badgeR.w - tagW - px(12), badgeR.h};
                    ui::label(kFontSemibold, px(10.5f), txtR, core::mix(rgb(0x004E60), rgb(0x002B36), hov), actionText, ui::Align::Right);
                }
            };

            const float colW = isKbMouse ? px(190) : px(210);
            const float badgeH = px(34);
            float leftX = diagramR.x + px(10);
            float rightX = diagramR.right() - colW - px(10);
            if (shell.gameMenuControllerLayout)
            {
                // in the game menu's wide column: the callouts close to the model, a margin left (the player's look)
                const float near = px(80);
                leftX = std::max(leftX, ctrlR.x - near - colW);
                rightX = std::min(rightX, ctrlR.right() + near);
            }
            const float gapY = px(42);
            const float startY = diagramR.cy() - (gapY * 2.0f) - badgeH * 0.5f;

            const bool isModern = (cfg.padPreset == 2);

            if (shell.controlsSubTab == 0) // PS2 DualShock 2
            {
                // Left Badges
                drawCallout("#c-ps2-l2", {leftX, startY + gapY * 0, colW, badgeH},
                            "L2", isModern ? tr("Frear / Ré (Carro)", "Brake / Reverse (Car)") : tr("Trocar de Arma (Esq.)", "Previous Weapon"),
                            tr("GATILHO L2 (ESQUERDO)", "LEFT TRIGGER L2"),
                            isModern ? tr("No modo Moderno: Atua como freio e marcha a ré analógica em veículos.", "Modern mode: Analog brake and reverse in vehicles.")
                                     : tr("No modo Clássico: Seleciona a arma anterior no inventário.", "Classic mode: Cycle to previous weapon in inventory."),
                            0.270f, 0.180f, false);

                drawCallout("#c-ps2-l1", {leftX, startY + gapY * 1, colW, badgeH},
                            "L1", isModern ? tr("Olhar Esq. / Mirar", "Look Left / Aim") : tr("Olhar Atrás / Rádio", "Look Behind / Radio"),
                            tr("BOTÃO L1 (OMBRO ESQUERDO)", "LEFT BUMPER L1"),
                            tr("Muda a estação de rádio no veículo ou olha para a esquerda durante tiroteios.", "Changes vehicle radio station or looks left during drive-by."),
                            0.280f, 0.237f, false);

                drawCallout("#c-ps2-dpad", {leftX, startY + gapY * 2, colW, badgeH},
                            "D-PAD", isModern ? tr("Armas / Empilhadeira", "Weapons / Forklift") : tr("Menus / Empilhadeira", "Menus / Forklift"),
                            tr("DIRECIONAL DIGITAL (D-PAD)", "DIRECTIONAL PAD (D-PAD)"),
                            tr("Troca de armas, navega em menus e ergue ou abaixa os garfos da empilhadeira.", "Cycles weapons, navigates menus and moves forklift forks up/down."),
                            0.275f, 0.410f, false);

                drawCallout("#c-ps2-sel", {leftX, startY + gapY * 3, colW, badgeH},
                            "SELECT", tr("Modos de Câmera", "Camera Modes"),
                            tr("BOTÃO SELECT", "SELECT BUTTON"),
                            tr("Alterna a distância e o ângulo da câmera de acompanhamento de Vic Vance e dos veículos.",
                               "Cycles camera distances and perspectives for Vic Vance and vehicles."),
                            0.435f, 0.410f, false);

                drawCallout("#c-ps2-l3", {leftX, startY + gapY * 4, colW, badgeH},
                            "L3 / ESQ.", tr("Mover Vic / Buzina", "Move Vic / Horn"),
                            tr("ANALÓGICO ESQUERDO (L3)", "LEFT ANALOG STICK (L3)"),
                            tr("Controla a movimentação de Vic Vance a pé ou a direção de veículos; clique ativa a buzina.",
                               "Controls Vic Vance's movement on foot or vehicle steering; click triggers horn."),
                            0.380f, 0.560f, false);

                // Right Badges
                drawCallout("#c-ps2-r2", {rightX, startY + gapY * 0, colW, badgeH},
                            "R2", isModern ? tr("Acelerar / Disparar", "Accelerate / Fire") : tr("Trocar de Arma (Dir.)", "Next Weapon"),
                            tr("GATILHO R2 (DIREITO)", "RIGHT TRIGGER R2"),
                            isModern ? tr("No modo Moderno: Acelera veículos e dispara armas com mira livre.", "Modern mode: Accelerates vehicles and fires weapons with free aim.")
                                     : tr("No modo Clássico: Seleciona a próxima arma no inventário.", "Classic mode: Cycle to next weapon in inventory."),
                            0.730f, 0.180f, true);

                drawCallout("#c-ps2-r1", {rightX, startY + gapY * 1, colW, badgeH},
                            "R1", isModern ? tr("Trava / Freio de Mão", "Lock-On / Handbrake") : tr("Trava de Mira / Freio Mão", "Lock-On / Handbrake"),
                            tr("BOTÃO R1 (OMBRO DIREITO)", "RIGHT BUMPER R1"),
                            tr("Trava a mira automaticamente no inimigo mais próximo a pé; freio de mão em carros.",
                               "Locks aim onto nearest enemy target on foot; acts as handbrake in cars."),
                            0.720f, 0.237f, true);

                drawCallout("#c-ps2-face", {rightX, startY + gapY * 2, colW, badgeH},
                            "△ ▢ ◯ ✕", isModern ? tr("Entrar • Pular • Melee • Correr", "Enter • Jump • Melee • Sprint") : tr("Ação • Pulo • Ataque • Sprint", "Vehicle • Jump • Attack • Sprint"),
                            tr("BOTÕES DE AÇÃO PLAYSTATION", "PLAYSTATION ACTION BUTTONS"),
                            tr("Triângulo: Entrar/Sair • Quadrado: Pular/Freio • Círculo: Atacar/Melee • Cruz: Correr/Acelerar.",
                               "Triangle: Enter/Exit • Square: Jump/Brake • Circle: Attack/Melee • Cross: Sprint/Accelerate."),
                            0.720f, 0.410f, true);

                drawCallout("#c-ps2-sta", {rightX, startY + gapY * 3, colW, badgeH},
                            "START", tr("Pausa / Menu (Esc)", "Pause / Menu (Esc)"),
                            tr("BOTÃO START", "START BUTTON"),
                            tr("Abre o menu de pausa, mapa da cidade, histórico de diálogos e estatísticas.",
                               "Opens the pause menu, city map, mission briefs and statistics."),
                            0.565f, 0.410f, true);

                drawCallout("#c-ps2-r3", {rightX, startY + gapY * 4, colW, badgeH},
                            "R3 / DIR.", tr("Câmera 360° / Agachar", "360° Camera / Crouch"),
                            tr("ANALÓGICO DIREITO (R3)", "RIGHT ANALOG STICK (R3)"),
                            tr("Giro suave de câmera 360°, mira livre estilo moderno e agachar com clique (R3).",
                               "Smooth 360° camera rotation, modern free aim and crouch on click (R3)."),
                            0.620f, 0.560f, true);
            }
            else if (shell.controlsSubTab == 1) // Xbox One / Series
            {
                // Left Badges
                drawCallout("#c-xb-lt", {leftX, startY + gapY * 0, colW, badgeH},
                            "LT", isModern ? tr("Frear / Ré (Carro)", "Brake / Reverse (Car)") : tr("Trocar Arma (Esq.)", "Previous Weapon"),
                            tr("GATILHO ESQUERDO (LT)", "LEFT TRIGGER (LT)"),
                            tr("Freio analógico suave e marcha à ré em veículos no layout moderno.", "Smooth analog brake and reverse in modern scheme."),
                            0.330f, 0.200f, false);

                drawCallout("#c-xb-lb", {leftX, startY + gapY * 1, colW, badgeH},
                            "LB", isModern ? tr("Olhar Esq. / Mirar", "Look Left / Aim") : tr("Olhar Atrás / Rádio", "Look Behind / Radio"),
                            tr("BOTÃO LB (OMBRO ESQUERDO)", "LEFT BUMPER (LB)"),
                            tr("Troca de estação de rádio ou mira de tiro lateral durante perseguições.", "Changes radio stations or drive-by lateral aiming."),
                            0.280f, 0.235f, false);

                drawCallout("#c-xb-ls", {leftX, startY + gapY * 2, colW, badgeH},
                            "LS / ESQ.", tr("Mover Vic / Buzina", "Move Vic / Horn"),
                            tr("ANALÓGICO ESQUERDO (LS)", "LEFT THUMBSTICK (LS)"),
                            tr("Controle analógico preciso da caminhada e corrida de Vic Vance e direção de veículos.",
                               "Precision analog movement for Vic Vance walking/running and vehicle steering."),
                            0.325f, 0.370f, false);

                drawCallout("#c-xb-view", {leftX, startY + gapY * 3, colW, badgeH},
                            "VIEW", tr("Modos de Câmera", "Camera Modes"),
                            tr("BOTÃO VIEW (BACK)", "VIEW BUTTON (BACK)"),
                            tr("Alterna entre as perspectivas da câmera no jogo.", "Cycles through available camera viewpoints."),
                            0.450f, 0.370f, false);

                drawCallout("#c-xb-dpad", {leftX, startY + gapY * 4, colW, badgeH},
                            "D-PAD", isModern ? tr("Armas / Empilhadeira", "Weapons / Forklift") : tr("Menus / Empilhadeira", "Menus / Forklift"),
                            tr("DIRECIONAL DIGITAL (D-PAD)", "DIRECTIONAL PAD (D-PAD)"),
                            tr("Troca de armas rápida e controle dos garfos da empilhadeira.", "Quick weapon selection and forklift controls."),
                            0.415f, 0.520f, false);

                // Right Badges
                drawCallout("#c-xb-rt", {rightX, startY + gapY * 0, colW, badgeH},
                            "RT", isModern ? tr("Acelerar / Disparar", "Accelerate / Fire") : tr("Trocar de Arma (Dir.)", "Next Weapon"),
                            tr("GATILHO DIREITO (RT)", "RIGHT TRIGGER (RT)"),
                            tr("Acelerador progressivo em veículos e disparo de armas no modo moderno.", "Progressive vehicle acceleration and weapon firing in modern mode."),
                            0.670f, 0.200f, true);

                drawCallout("#c-xb-rb", {rightX, startY + gapY * 1, colW, badgeH},
                            "RB", tr("Trava de Mira / Freio Mão", "Lock-On / Handbrake"),
                            tr("BOTÃO RB (OMBRO DIREITO)", "RIGHT BUMPER (RB)"),
                            tr("Trava de mira automática no inimigo mais próximo a pé e freio de mão em carros.",
                               "Auto lock-on targeting on foot and handbrake in vehicles."),
                            0.720f, 0.235f, true);

                drawCallout("#c-xb-face", {rightX, startY + gapY * 2, colW, badgeH},
                            "Y / X / B / A", isModern ? tr("Entrar • Pular • Melee • Correr", "Enter • Jump • Melee • Run") : tr("Ação • Pulo • Ataque • Correr", "Vehicle • Jump • Attack • Run"),
                            tr("BOTÕES ABXY DO XBOX", "XBOX ABXY BUTTONS"),
                            tr("Y: Entrar/Sair • X: Pular/Freio • B: Atacar/Melee • A: Correr/Acelerar.",
                               "Y: Enter/Exit • X: Jump/Brake • B: Attack/Melee • A: Sprint/Accelerate."),
                            0.685f, 0.370f, true);

                drawCallout("#c-xb-menu", {rightX, startY + gapY * 3, colW, badgeH},
                            "MENU", tr("Pausa / Menu (Esc)", "Pause / Menu (Esc)"),
                            tr("BOTÃO MENU (START)", "MENU BUTTON (START)"),
                            tr("Abre o menu de pausa e mapa do jogo.", "Opens game pause menu and world map."),
                            0.550f, 0.370f, true);

                drawCallout("#c-xb-rs", {rightX, startY + gapY * 4, colW, badgeH},
                            "RS / DIR.", tr("Câmera 360° / Agachar", "360° Camera / Crouch"),
                            tr("ANALÓGICO DIREITO (RS)", "RIGHT THUMBSTICK (RS)"),
                            tr("Câmera suave e mira livre no estilo moderno de tiro.", "Smooth camera rotation and modern free aim."),
                            0.585f, 0.520f, true);
            }
            else if (shell.controlsSubTab == 2) // PlayStation 4 DualShock 4
            {
                // Left Badges
                drawCallout("#c-ds4-l2", {leftX, startY + gapY * 0, colW, badgeH},
                            "L2", isModern ? tr("Frear / Ré (Carro)", "Brake / Reverse (Car)") : tr("Trocar de Arma (Esq.)", "Previous Weapon"),
                            tr("GATILHO L2", "LEFT TRIGGER L2"),
                            isModern ? tr("Freio e ré analógicos em veículos no modo moderno.", "Analog brake and reverse in modern mode.")
                                     : tr("Troca de arma para a esquerda no modo clássico.", "Cycles previous weapon in classic mode."),
                            0.275f, 0.210f, false);

                drawCallout("#c-ds4-l1", {leftX, startY + gapY * 1, colW, badgeH},
                            "L1", isModern ? tr("Olhar Esq. / Mirar", "Look Left / Aim") : tr("Olhar Atrás / Rádio", "Look Behind / Radio"),
                            tr("BOTÃO L1 (OMBRO ESQUERDO)", "LEFT BUMPER L1"),
                            tr("Troca de estação de rádio ou visão lateral/traseira.", "Radio station cycle or rearview camera."),
                            0.285f, 0.245f, false);

                drawCallout("#c-ds4-pad", {leftX, startY + gapY * 2, colW, badgeH},
                            "TOUCHPAD", tr("Trocar Câmera / Rádio", "Camera Cycle / Radio"),
                            tr("TOUCHPAD DUALSHOCK 4", "DUALSHOCK 4 TOUCHPAD"),
                            tr("Toque central alterna distância da câmera no padrão GTA V; deslize troca de estação de rádio.",
                               "Center tap cycles camera distance matching GTA V; swipe changes radio stations."),
                            0.410f, 0.280f, false);

                drawCallout("#c-ds4-dpad", {leftX, startY + gapY * 3, colW, badgeH},
                            "D-PAD", isModern ? tr("Armas / Empilhadeira", "Weapons / Forklift") : tr("Menus / Empilhadeira", "Menus / Forklift"),
                            tr("DIRECIONAL DIGITAL", "DIRECTIONAL PAD"),
                            tr("Seleção rápida de armas e controle dos garfos da empilhadeira.", "Quick weapon selection and forklift controls."),
                            0.285f, 0.385f, false);

                drawCallout("#c-ds4-l3", {leftX, startY + gapY * 4, colW, badgeH},
                            "L3 / ESQ.", tr("Mover Vic / Buzina", "Move Vic / Horn"),
                            tr("ANALÓGICO ESQUERDO (L3)", "LEFT ANALOG STICK (L3)"),
                            tr("Movimentação de Vic e direção de veículos; clique aciona a buzina.", "Movement and vehicle steering; click triggers horn."),
                            0.390f, 0.520f, false);

                // Right Badges
                drawCallout("#c-ds4-r2", {rightX, startY + gapY * 0, colW, badgeH},
                            "R2", isModern ? tr("Acelerar / Disparar", "Accelerate / Fire") : tr("Trocar de Arma (Dir.)", "Next Weapon"),
                            tr("GATILHO R2", "RIGHT TRIGGER R2"),
                            isModern ? tr("Aceleração progressiva de veículos e disparo de armas no modo moderno.", "Progressive acceleration and weapon fire in modern mode.")
                                     : tr("Troca de arma para a direita no modo clássico.", "Cycles next weapon in classic mode."),
                            0.725f, 0.210f, true);

                drawCallout("#c-ds4-r1", {rightX, startY + gapY * 1, colW, badgeH},
                            "R1", isModern ? tr("Trava / Freio de Mão", "Lock-On / Handbrake") : tr("Trava de Mira / Freio Mão", "Lock-On / Handbrake"),
                            tr("BOTÃO R1 (OMBRO DIREITO)", "RIGHT BUMPER R1"),
                            tr("Trava automática de mira em inimigos e freio de mão.", "Auto lock-on targeting and handbrake."),
                            0.715f, 0.245f, true);

                drawCallout("#c-ds4-opt", {rightX, startY + gapY * 2, colW, badgeH},
                            "OPTIONS", tr("Pausa / Menu (Esc)", "Pause / Menu (Esc)"),
                            tr("BOTÃO OPTIONS (START)", "OPTIONS BUTTON (START)"),
                            tr("Abre o menu de pausa e mapa do jogo; barra de luz reflete a vida e nível de procurado.",
                               "Opens game pause menu and map; lightbar reflects player health and wanted level."),
                            0.640f, 0.285f, true);

                drawCallout("#c-ds4-face", {rightX, startY + gapY * 3, colW, badgeH},
                            "△ ▢ ◯ ✕", isModern ? tr("Entrar • Pular • Melee • Correr", "Enter • Jump • Melee • Sprint") : tr("Ação • Pulo • Ataque • Sprint", "Vehicle • Jump • Attack • Sprint"),
                            tr("BOTÕES PLAYSTATION", "PLAYSTATION BUTTONS"),
                            tr("Triângulo: Entrar/Sair • Quadrado: Pular/Freio • Círculo: Atacar/Melee • Cruz: Correr/Acelerar.",
                               "Triangle: Enter/Exit • Square: Jump/Brake • Circle: Attack/Melee • Cross: Sprint/Accelerate."),
                            0.730f, 0.385f, true);

                drawCallout("#c-ds4-r3", {rightX, startY + gapY * 4, colW, badgeH},
                            "R3 / DIR.", tr("Câmera 360° / Agachar", "360° Camera / Crouch"),
                            tr("ANALÓGICO DIREITO (R3)", "RIGHT ANALOG STICK (R3)"),
                            tr("Câmera suave e mira livre moderna.", "Smooth camera rotation and modern free aim."),
                            0.620f, 0.520f, true);
            }
            else if (shell.controlsSubTab == 3) // PlayStation 5 DualSense — Native Features
            {
                // Left Badges
                drawCallout("#c-ps5-l2", {leftX, startY + gapY * 0, colW, badgeH},
                            "L2 GATILHO", isModern ? (cfg.dualsenseTriggers ? tr("Gatilho Adaptável: Freio", "Adaptive: Dynamic Brake") : tr("Frear / Marcha a Ré", "Brake / Reverse"))
                                                   : (cfg.dualsenseTriggers ? tr("Gatilho Adaptável", "Adaptive Trigger") : tr("Arma Anterior", "Previous Weapon")),
                            tr("GATILHO ADAPTÁVEL L2 (DUALSENSE)", "DUALSENSE ADAPTIVE TRIGGER L2"),
                            tr("Suporte nativo a gatilhos adaptáveis: Simula a resistência e rigidez real do pedal de freio dos veículos.",
                               "Native adaptive trigger support: Simulates physical brake pedal resistance and vehicle weight."),
                            0.275f, 0.190f, false);

                drawCallout("#c-ps5-l1", {leftX, startY + gapY * 1, colW, badgeH},
                            "L1 OMBRO", isModern ? tr("Olhar Esq. / Mirar", "Look Left / Aim") : tr("Olhar Atrás / Rádio", "Look Behind / Radio"),
                            tr("BOTÃO L1 (OMBRO ESQUERDO)", "LEFT BUMPER L1"),
                            tr("Troca de estação de rádio ou visão lateral/retrovisora.", "Radio station cycle or rearview camera."),
                            0.280f, 0.225f, false);

                drawCallout("#c-ps5-pad", {leftX, startY + gapY * 2, colW, badgeH},
                            "TOUCHPAD", tr("Trocar Câmera (GTA V) / Rádio", "Camera (GTA V) / Radio"),
                            tr("TOUCHPAD MULTITOQUE DUALSENSE", "DUALSENSE MULTI-TOUCH TOUCHPAD"),
                            tr("Toque central alterna distância da câmera no padrão GTA V; deslize horizontal troca estações de rádio.",
                               "Center tap cycles camera distance matching GTA V; horizontal swipe changes radio stations."),
                            0.380f, 0.260f, false);

                drawCallout("#c-ps5-dpad", {leftX, startY + gapY * 3, colW, badgeH},
                            "D-PAD", isModern ? tr("Armas / Empilhadeira", "Weapons / Forklift") : tr("Menus / Empilhadeira", "Menus / Forklift"),
                            tr("DIRECIONAL DIGITAL", "DIRECTIONAL PAD"),
                            tr("Seleção rápida de armas e controle de elevação dos garfos da empilhadeira.", "Quick weapon selection and forklift controls."),
                            0.275f, 0.365f, false);

                drawCallout("#c-ps5-l3", {leftX, startY + gapY * 4, colW, badgeH},
                            "L3 / ESQ.", tr("Mover Vic / Buzina", "Move Vic / Horn"),
                            tr("ANALÓGICO ESQUERDO (L3)", "LEFT ANALOG STICK (L3)"),
                            tr("Controle analógico de alta precisão de Vic e direção de veículos; clique ativa a buzina.",
                               "High precision analog movement for Vic and steering; click activates horn."),
                            0.380f, 0.500f, false);

                // Right Badges
                drawCallout("#c-ps5-r2", {rightX, startY + gapY * 0, colW, badgeH},
                            "R2 GATILHO", isModern ? (cfg.dualsenseTriggers ? tr("Gatilho Adaptável: Recuo", "Adaptive: Gun Recoil") : tr("Acelerar / Disparar", "Accelerate / Fire"))
                                                   : (cfg.dualsenseTriggers ? tr("Gatilho Adaptável", "Adaptive Trigger") : tr("Próxima Arma", "Next Weapon")),
                            tr("GATILHO ADAPTÁVEL R2 (DUALSENSE)", "DUALSENSE ADAPTIVE TRIGGER R2"),
                            tr("No DualSense: Simula a parada mecânica do gatilho e o coice exato de cada arma (pistola, escopeta, fuzil).",
                               "DualSense adaptive trigger simulates mechanical trigger break and distinct recoil per weapon."),
                            0.725f, 0.190f, true);

                drawCallout("#c-ps5-r1", {rightX, startY + gapY * 1, colW, badgeH},
                            "R1 OMBRO", isModern ? tr("Trava / Freio de Mão", "Lock-On / Handbrake") : tr("Trava de Mira / Freio Mão", "Lock-On / Handbrake"),
                            tr("BOTÃO R1 (OMBRO DIREITO)", "RIGHT BUMPER R1"),
                            tr("Trava automática de mira em inimigos a pé e freio de mão em curvas.", "Auto lock-on targeting and handbrake."),
                            0.720f, 0.225f, true);

                drawCallout("#c-ps5-face", {rightX, startY + gapY * 2, colW, badgeH},
                            "△ ▢ ◯ ✕", isModern ? tr("Entrar • Pular • Melee • Correr", "Enter • Jump • Melee • Sprint") : tr("Ação • Pulo • Ataque • Sprint", "Vehicle • Jump • Attack • Sprint"),
                            tr("BOTÕES PLAYSTATION", "PLAYSTATION BUTTONS"),
                            tr("Triângulo: Entrar/Sair • Quadrado: Pular/Freio • Círculo: Atacar/Melee • Cruz: Correr/Acelerar.",
                               "Triangle: Enter/Exit • Square: Jump/Brake • Circle: Attack/Melee • Cross: Sprint/Accelerate."),
                            0.725f, 0.365f, true);

                drawCallout("#c-ps5-audio", {rightX, startY + gapY * 3, colW, badgeH},
                            "ÁUDIO & HAPTICS", tr("Rádio Policial & Vibração", "Police Radio & Haptics"),
                            tr("ALTO-FALANTE INTEGRADO & FEEDBACK HÁPTICO", "BUILT-IN SPEAKER & HAPTIC FEEDBACK"),
                            tr("Transmissões de rádio da polícia e toques de pager tocam no alto-falante do controle; feedback háptico detalha o asfalto e colisões.",
                               "Police dispatch radio chatter and pager chimes play through controller speaker; haptics convey surfaces and impacts."),
                            0.500f, 0.435f, true);

                drawCallout("#c-ps5-r3", {rightX, startY + gapY * 4, colW, badgeH},
                            "R3 / DIR.", tr("Câmera 360° / Agachar", "360° Camera / Crouch"),
                            tr("ANALÓGICO DIREITO (R3)", "RIGHT ANALOG STICK (R3)"),
                            tr("Câmera suave e mira livre moderna.", "Smooth camera rotation and modern free aim."),
                            0.615f, 0.500f, true);
            }
            else if (shell.controlsSubTab == 4) // Nintendo Switch Pro
            {
                // Left Badges
                drawCallout("#c-sw-zl", {leftX, startY + gapY * 0, colW, badgeH},
                            "ZL", isModern ? tr("Frear / Ré (Carro)", "Brake / Reverse (Car)") : tr("Trocar de Arma (Esq.)", "Previous Weapon"),
                            tr("GATILHO ZL", "ZL TRIGGER"),
                            tr("Freio e ré em veículos no modo moderno ou seleção de arma.", "Brake/reverse in modern mode or weapon cycling."),
                            0.270f, 0.190f, false);

                drawCallout("#c-sw-l", {leftX, startY + gapY * 1, colW, badgeH},
                            "L", isModern ? tr("Olhar Esq. / Mirar", "Look Left / Aim") : tr("Olhar Atrás / Rádio", "Look Behind / Radio"),
                            tr("BOTÃO L", "L BUTTON"),
                            tr("Troca de estação de rádio ou visão traseira.", "Radio station cycle or rearview camera."),
                            0.330f, 0.180f, false);

                drawCallout("#c-sw-min", {leftX, startY + gapY * 2, colW, badgeH},
                            "- (MENOS)", tr("Modos de Câmera", "Camera Modes"),
                            tr("BOTÃO MENOS (-)", "MINUS BUTTON (-)"),
                            tr("Alterna entre as perspectivas de câmera.", "Cycles camera perspectives."),
                            0.400f, 0.290f, false);

                drawCallout("#c-sw-ls", {leftX, startY + gapY * 3, colW, badgeH},
                            "L-STICK", tr("Mover Vic / Buzina", "Move Vic / Horn"),
                            tr("ANALÓGICO ESQUERDO", "LEFT ANALOG STICK"),
                            tr("Movimentação e controle da direção do veículo.", "Movement and vehicle steering."),
                            0.295f, 0.350f, false);

                drawCallout("#c-sw-dpad", {leftX, startY + gapY * 4, colW, badgeH},
                            "D-PAD", isModern ? tr("Armas / Empilhadeira", "Weapons / Forklift") : tr("Menus / Empilhadeira", "Menus / Forklift"),
                            tr("DIRECIONAL DIGITAL", "DIRECTIONAL PAD"),
                            tr("Troca de armas e garfos da empilhadeira.", "Weapons and forklift forks."),
                            0.380f, 0.490f, false);

                // Right Badges
                drawCallout("#c-sw-zr", {rightX, startY + gapY * 0, colW, badgeH},
                            "ZR", isModern ? tr("Acelerar / Disparar", "Accelerate / Fire") : tr("Trocar de Arma (Dir.)", "Next Weapon"),
                            tr("GATILHO ZR", "ZR TRIGGER"),
                            tr("Acelerador de veículos e disparo de armas no modo moderno.", "Vehicle throttle and fire in modern mode."),
                            0.730f, 0.190f, true);

                drawCallout("#c-sw-r", {rightX, startY + gapY * 1, colW, badgeH},
                            "R", tr("Trava de Mira / Freio Mão", "Lock-On / Handbrake"),
                            tr("BOTÃO R", "R BUTTON"),
                            tr("Trava de mira automática e freio de mão.", "Auto lock-on and handbrake."),
                            0.670f, 0.180f, true);

                drawCallout("#c-sw-plus", {rightX, startY + gapY * 2, colW, badgeH},
                            "+ (MAIS)", tr("Pausa / Menu (Esc)", "Pause / Menu (Esc)"),
                            tr("BOTÃO MAIS (+)", "PLUS BUTTON (+)"),
                            tr("Menu de pausa e mapa da cidade.", "Pause menu and city map."),
                            0.580f, 0.290f, true);

                drawCallout("#c-sw-face", {rightX, startY + gapY * 3, colW, badgeH},
                            "X / Y / A / B", isModern ? tr("Entrar • Pular • Melee • Correr", "Enter • Jump • Melee • Run") : tr("Ação • Pulo • Ataque • Correr", "Vehicle • Jump • Attack • Run"),
                            tr("BOTÕES DE AÇÃO DO SWITCH", "SWITCH ACTION BUTTONS"),
                            tr("X: Entrar/Sair • Y: Pular/Freio • A: Atacar/Melee • B: Correr/Acelerar.",
                               "X: Enter/Exit • Y: Jump/Brake • A: Attack/Melee • B: Sprint/Accelerate."),
                            0.680f, 0.360f, true);

                drawCallout("#c-sw-rs", {rightX, startY + gapY * 4, colW, badgeH},
                            "R-STICK", tr("Câmera 360° / Agachar", "360° Camera / Crouch"),
                            tr("ANALÓGICO DIREITO", "RIGHT ANALOG STICK"),
                            tr("Câmera suave e mira livre no estilo moderno.", "Smooth camera rotation and modern free aim."),
                            0.585f, 0.490f, true);
            }
            else if (shell.controlsSubTab == 5) // Keyboard & Mouse
            {
                // Left Badges (Keyboard + Mouse Left Click)
                drawCallout("#c-kb-tab", {leftX, startY + gapY * 0, colW, badgeH},
                            "TAB", tr("Inventário (Tab)", "Weapons (Tab)"),
                            tr("TAB / NÚMEROS DO TECLADO", "TAB / NUMBER KEYS"),
                            tr("Exibe o mostrador de inventário e troca rapidamente a arma selecionada.",
                               "Displays status and inventory overlay, quickly selecting weapons."),
                            0.120f, 0.245f, false);

                drawCallout("#c-kb-wasd", {leftX, startY + gapY * 1, colW, badgeH},
                            "W A S D", tr("Mover Vic / Direção", "Move Vic / Steering"),
                            tr("TECLAS DE MOVIMENTAÇÃO (W, A, S, D)", "MOVEMENT KEYS (W, A, S, D)"),
                            tr("Controle padrão de movimentação de Vic Vance a pé e direção de veículos terrestres, barcos e aeronaves.",
                               "Standard movement keys for Vic Vance on foot and steering land vehicles, boats and aircraft."),
                            0.275f, 0.285f, false);

                drawCallout("#c-kb-shift", {leftX, startY + gapY * 2, colW, badgeH},
                            "SHIFT", tr("Corrida Rápida", "Sprint (Run)"),
                            tr("TECLA SHIFT (SPRINT)", "SHIFT KEY (SPRINT)"),
                            tr("Faz Vic correr em velocidade máxima a pé consumindo a barra de estamina.",
                               "Makes Vic Vance sprint at maximum speed on foot, draining stamina."),
                            0.145f, 0.400f, false);

                drawCallout("#c-kb-space", {leftX, startY + gapY * 3, colW, badgeH},
                            "ESPAÇO", tr("Pulo / Freio de Mão", "Jump / Handbrake"),
                            tr("BARRA DE ESPAÇO", "SPACEBAR"),
                            tr("Pula obstáculos a pé; aciona o freio de mão em veículos para derrapagens e curvas fechadas.",
                               "Jumps obstacles on foot; triggers handbrake in vehicles for drift maneuvers and tight turns."),
                            0.450f, 0.480f, false);

                drawCallout("#c-mo-lmb", {leftX, startY + gapY * 4, colW, badgeH},
                            "LMB (ESQ.)", tr("Atirar / Melee", "Fire / Melee"),
                            tr("BOTÃO ESQUERDO DO MOUSE (LMB)", "LEFT MOUSE BUTTON (LMB)"),
                            tr("Dispara a arma de fogo equipada com sensibilidade precisa ou desfere socos em combate.",
                               "Fires equipped firearm with precise sensitivity or punches in melee combat."),
                            0.345f, 0.700f, false);

                // Right Badges (Keyboard + Mouse)
                drawCallout("#c-kb-enter", {rightX, startY + gapY * 0, colW, badgeH},
                            "ENTER / F", tr("Entrar no Veículo", "Enter Vehicle"),
                            tr("ENTRAR E SAIR DE VEÍCULOS (F / ENTER)", "ENTER / EXIT VEHICLE (F / ENTER)"),
                            tr("Entra ou sai de qualquer veículo próximo ou assume o assento do passageiro.",
                               "Enters or exits any nearby vehicle or assumes passenger seat."),
                            0.885f, 0.320f, true);

                drawCallout("#c-kb-crouch", {rightX, startY + gapY * 1, colW, badgeH},
                            "C / RÁDIO (R)", tr("Agachar / Rádio", "Crouch / Radio"),
                            tr("TECLAS DE AGACHAR E RÁDIO (C / R)", "CROUCH AND RADIO KEYS (C / R)"),
                            tr("Tecla C agacha para proteção atrás de coberturas; tecla R alterna as emissoras de rádio.",
                               "C key crouches for cover; R key cycles Vice City radio stations."),
                            0.780f, 0.400f, true);

                drawCallout("#c-mo-rmb", {rightX, startY + gapY * 2, colW, badgeH},
                            "RMB (DIR.)", tr("Mirar com Precisão", "Aim / Precision Zoom"),
                            tr("BOTÃO DIREITO DO MOUSE (RMB)", "RIGHT MOUSE BUTTON (RMB)"),
                            tr("Ativa mira livre sobre o ombro ou amplia a visão telescópica em rifles de precisão.",
                               "Activates over-the-shoulder free aim or zooms in with sniper scopes."),
                            0.655f, 0.700f, true);

                drawCallout("#c-mo-wheel", {rightX, startY + gapY * 3, colW, badgeH},
                            "RODA", tr("Trocar de Arma", "Scroll Weapons"),
                            tr("RODA DE ROLAGEM DO MOUSE", "MOUSE SCROLL WHEEL"),
                            tr("Girar para cima ou para baixo seleciona instantaneamente a próxima arma do inventário.",
                               "Scrolling up or down instantly cycles inventory weapons."),
                            0.655f, 0.830f, true);

                drawCallout("#c-mo-look", {rightX, startY + gapY * 4, colW, badgeH},
                            "MOUSE 360°", tr("Câmera Livre 360°", "360° Free Camera"),
                            tr("MOVIMENTO LIVRE DO MOUSE", "FREE MOUSE LOOK"),
                            tr("Controle fluido de câmera 360° com suporte a sensibilidade independente para X e Y.",
                               "Fluid 360° camera control with independent sensitivity for horizontal and vertical axes."),
                            0.500f, 0.850f, true);
            }
        }
        else // Subtab 6: AJUSTES & MAPEAMENTO
        {
            const ui::Id scrollId = ui::id("#vc-ctrls-scroll");
            ui::beginScroll(scrollId, tableR);
            float y = tableR.y;
            const float rowH = px(44);
            int rowIdx = 0;
            bool changed = false;

            // SECTION 1: PREDEFINIÇÃO GERAL
            {
                const Rect headR{tableR.x, y, tableR.w, px(34)};
                rect(headR, rgb(0x006A82, 45));
                rect({headR.x, headR.y, px(5), headR.h}, rgb(0xFE7FB4));
                ui::label(kFontSemibold, px(12.5f), {headR.x + px(16), headR.y, headR.w - px(24), headR.h},
                          rgb(0x004E60), tr("PREDEFINIÇÕES GERAIS DE CONTROLE", "GENERAL CONTROL PRESETS"), ui::Align::Left);
                y += px(34);
            }
            {
                int prIdx = cfg.padPreset;
                bool hov = false;
                if (ui::vcCycleRow("#ctrl-opt-preset", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Esquema de Botões e Gatilhos", "Button & Trigger Scheme"),
                                   {tr("Personalizado", "Custom"),
                                    tr("Clássico PS2 (GTA III / VC / SA / VCS)", "Classic PS2 (GTA III / VC / SA / VCS)"),
                                    tr("Moderno (GTA IV / V - Gatilhos)", "Modern (GTA IV / V - Triggers)")},
                                   prIdx, &hov))
                {
                    cfg.applyPadPreset(prIdx);
                    changed = true;
                }
                if (hov)
                {
                    inspector.title = tr("PREDEFINIÇÃO DE CONTROLES", "CONTROLLER PRESET");
                    inspector.desc = tr("Alterna instantaneamente entre o layout clássico de PS2 e o moderno de GTA IV/V com gatilhos analógicos.",
                                        "Instantly switches between the classic PS2 scheme and modern GTA IV/V trigger controls.");
                    inspector.badge = tr("PREDEFINIÇÃO", "PRESET");
                }
                y += rowH;
            }

            // SECTION 2: GATILHOS E MIRA
            {
                const Rect headR{tableR.x, y, tableR.w, px(34)};
                rect(headR, rgb(0x006A82, 45));
                rect({headR.x, headR.y, px(5), headR.h}, rgb(0xFE7FB4));
                ui::label(kFontSemibold, px(12.5f), {headR.x + px(16), headR.y, headR.w - px(24), headR.h},
                          rgb(0x004E60), tr("JOGABILIDADE NO GAMEPAD (GATILHOS & MIRA)", "GAMEPAD GAMEPLAY (TRIGGERS & AIM)"), ui::Align::Left);
                y += px(34);
            }
            {
                bool hov = false;
                if (ui::vcCycleRow("#ctrl-opt-trigdrive", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Acelerar / Frear nos Gatilhos (R2 / L2)", "Drive with Triggers (R2 / L2)"),
                                   {tr("Não", "Off"), tr("Sim", "On")}, cfg.padTriggerDrive, &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("ACELERAR E FREAR NOS GATILHOS", "DRIVE WITH TRIGGERS");
                    inspector.desc = tr("Faz com que R2 acelere e L2 freie/dê ré em veículos. Olhar para os lados move para o D-Pad.",
                                        "Makes R2 accelerate and L2 brake/reverse. Looking sideways moves to the D-Pad.");
                    inspector.badge = tr("VEÍCULOS", "VEHICLES");
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcCycleRow("#ctrl-opt-trigshoot", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Disparar / Mirar nos Gatilhos (R2 / L2)", "Shoot with Triggers (R2 / L2)"),
                                   {tr("Não", "Off"), tr("Sim", "On")}, cfg.padTriggerShoot, &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("DISPARAR E MIRAR NOS GATILHOS", "SHOOT WITH TRIGGERS");
                    inspector.desc = tr("Faz com que R2 dispare e L2 mire armas a pé, no layout moderno dos jogos de tiro.",
                                        "Makes R2 fire and L2 aim weapons on foot, matching modern shooter standards.");
                    inspector.badge = tr("COMBATE", "COMBAT");
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcCycleRow("#ctrl-opt-freeaim", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Mira Livre no Analógico (Free Aim)", "Gamepad Free Aim"),
                                   {tr("Não (Trava PS2)", "Off (PS2 Lock-On)"), tr("Sim (Mira Livre 360°)", "On (360° Free Aim)")},
                                   cfg.padFreeAim, &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("MIRA LIVRE NO GAMEPAD", "GAMEPAD FREE AIM");
                    inspector.desc = tr("Permite mirar livremente com o analógico direito e disparar através do retículo em 360°.",
                                        "Enables free aiming with the right analog stick and firing through the reticle in 360°.");
                    inspector.badge = tr("MIRA LIVRE", "FREE AIM");
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcCycleRow("#ctrl-opt-assist", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Assistência de Mira GTA V (Aim Assist)", "GTA V Aim Assist"),
                                   {tr("Não", "Off"), tr("Sim", "On")}, cfg.padAimAssist, &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("ASSISTÊNCIA DE MIRA ESTILO GTA V", "GTA V STYLE AIM ASSIST");
                    inspector.desc = tr("Trava inicialmente no alvo mais próximo e permite transição suave para mira livre ao mover o analógico.",
                                        "Initially locks on to target then smoothly transitions to free aim when moving stick.");
                    inspector.badge = tr("ASSISTÊNCIA", "ASSIST");
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcSliderRow("#ctrl-opt-deadzone", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                    tr("Ponto Morto dos Analógicos (Deadzone)", "Analog Stick Deadzone"),
                                    cfg.padDeadzone, 0, 40, 1, "%", &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("ZONA MORTA DOS ANALÓGICOS", "ANALOG STICK DEADZONE");
                    inspector.desc = tr("Ajusta o ponto morto central dos analógicos para evitar drift involuntário em controles desgastados.",
                                        "Adjusts central stick deadzone to eliminate involuntary stick drift.");
                    inspector.badge = tr("CALIBRAÇÃO", "CALIBRATION");
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcSliderRow("#ctrl-opt-camspeed", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                    tr("Velocidade da Câmera (Camera Speed)", "Gamepad Camera Speed"),
                                    cfg.padCameraSpeed, 10, 400, 5, "%", &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("VELOCIDADE DA CÂMERA NO GAMEPAD", "GAMEPAD CAMERA SPEED");
                    inspector.desc = tr("Controla a velocidade de rotação e resposta da câmera conduzida pelo analógico direito.",
                                        "Scales camera rotation speed and responsiveness driven by the right analog stick.");
                    inspector.badge = tr("SENSIBILIDADE", "SENSITIVITY");
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcSliderRow("#ctrl-opt-vib", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                    tr("Intensidade de Vibração dos Motores", "Vibration Force Feedback"),
                                    cfg.padVibration, 0, 200, 5, "%", &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("VIBRAÇÃO E FORCE FEEDBACK", "VIBRATION FEEDBACK");
                    inspector.desc = tr("Escala a força dos motores de vibração do controle para batidas, disparos e explosões.",
                                        "Scales vibration rumble motors for crashes, weapon recoil and explosions.");
                    inspector.badge = tr("RUMBLE", "RUMBLE");
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcCycleRow("#ctrl-opt-inverty", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Inverter Eixo Y da Câmera", "Invert Vertical Axis (Y)"),
                                   {tr("Padrão", "Normal"), tr("Invertido", "Inverted")}, cfg.padInvertY, &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("INVERTER EIXO VERTICAL DO ANALÓGICO", "INVERT ANALOG Y AXIS");
                    inspector.desc = tr("Inverte o movimento vertical da câmera (cima olha para baixo e baixo olha para cima).",
                                        "Inverts vertical camera movement (up looks down, down looks up).");
                    inspector.badge = tr("CÂMERA", "CAMERA");
                }
                y += rowH;
            }

            // SECTION 3: RECURSOS DUALSENSE (PS5)
            {
                const Rect headR{tableR.x, y, tableR.w, px(34)};
                rect(headR, rgb(0x006A82, 45));
                rect({headR.x, headR.y, px(5), headR.h}, rgb(0xFE7FB4));
                ui::label(kFontSemibold, px(12.5f), {headR.x + px(16), headR.y, headR.w - px(24), headR.h},
                          rgb(0x004E60), tr("RECURSOS ESPECIAIS PLAYSTATION DUALSENSE (PS5)", "PLAYSTATION DUALSENSE FEATURES (PS5)"), ui::Align::Left);
                y += px(34);
            }
            {
                bool hov = false;
                if (ui::vcCycleRow("#ctrl-opt-ds-trig", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Gatilhos Adaptáveis DualSense", "DualSense Adaptive Triggers"),
                                   {tr("Desligado", "Off"), tr("Ligado", "On")}, cfg.dualsenseTriggers, &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("GATILHOS ADAPTÁVEIS DUALSENSE", "DUALSENSE ADAPTIVE TRIGGERS");
                    inspector.desc = tr("Reproduz a resistência mecânica e o recuo característico de cada calibre nos gatilhos L2/R2.",
                                        "Simulates realistic weapon trigger wall break and recoil kicks on L2/R2.");
                    inspector.badge = "DUALSENSE";
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcCycleRow("#ctrl-opt-ds-light", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Barra de Luz do Controle (Lightbar)", "DualSense / DS4 Lightbar"),
                                   {tr("Desligada", "Off"), tr("Saúde de Vic", "Health Status"), tr("Rosa Vice City", "Vice Pink")},
                                   cfg.dualsenseLightbar, &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("BARRA DE LUZ RGB DUALSENSE / DUALSHOCK 4", "CONTROLLER RGB LIGHTBAR");
                    inspector.desc = tr("Define a cor da barra de luz do controle durante o jogo.",
                                        "Configures controller LED lightbar color during gameplay.");
                    inspector.badge = "RGB LIGHTBAR";
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcCycleRow("#ctrl-opt-ds-flash", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Efeito de Sirene Policial (Police Flash)", "Police Siren Flash"),
                                   {tr("Desligado", "Off"), tr("Ligado", "On")}, cfg.dualsensePoliceFlash, &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("SIRENE POLICIAL NA BARRA DE LUZ", "POLICE LIGHTBAR FLASH");
                    inspector.desc = tr("Pisca em vermelho e azul na frequência da polícia de Vice City quando você adquire nível de procurado.",
                                        "Flashes red and blue matching Vice City police frequencies during wanted levels.");
                    inspector.badge = "POLICE FLASH";
                }
                y += rowH;
            }
            {
                bool hov = false;
                // the game's pad.ds_speaker values in the order shown (2, the police radio, waits for one: VCS has none)
                static const int kSpeakerValues[] = {0, 1};
                int speakerIdx = indexInList(cfg.dualsenseSpeaker, kSpeakerValues, 2, 0);
                if (ui::vcCycleRow("#ctrl-opt-ds-spk", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Áudio no Alto-Falante do Controle", "Controller Speaker Audio"),
                                   {tr("Desligado", "Off"), tr("Todo o som", "All sound")}, speakerIdx, &hov))
                {
                    cfg.dualsenseSpeaker = kSpeakerValues[speakerIdx];
                    changed = true;
                }
                if (hov)
                {
                    inspector.title = tr("ALTO-FALANTE DO CONTROLE", "CONTROLLER SPEAKER");
                    inspector.desc = tr("Reproduz o áudio do jogo diretamente no alto-falante integrado do DualSense.",
                                        "Routes game audio directly to the DualSense built-in speaker.");
                    inspector.badge = "SPEAKER";
                }
                y += rowH;
            }

            // SECTION 4: MOUSE & TECLADO
            {
                const Rect headR{tableR.x, y, tableR.w, px(34)};
                rect(headR, rgb(0x006A82, 45));
                rect({headR.x, headR.y, px(5), headR.h}, rgb(0xFE7FB4));
                ui::label(kFontSemibold, px(12.5f), {headR.x + px(16), headR.y, headR.w - px(24), headR.h},
                          rgb(0x004E60), tr("MOUSE & CONTROLES DE PC", "PC MOUSE & CONTROLS"), ui::Align::Left);
                y += px(34);
            }
            {
                bool hov = false;
                if (ui::vcCycleRow("#ctrl-opt-mouscam", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Câmera Livre no Mouse (Mouse Camera)", "PC Mouse Camera"),
                                   {tr("Não", "Off"), tr("Sim", "On")}, cfg.mouseCamera, &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("CÂMERA LIVRE NO MOUSE", "PC MOUSE CAMERA");
                    inspector.desc = tr("Permite girar a câmera suavemente com o movimento do mouse a pé e em veículos.",
                                        "Allows smooth 1:1 camera rotation with mouse movement on foot and behind vehicles.");
                    inspector.badge = "MOUSE";
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcSliderRow("#ctrl-opt-mousens", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                    tr("Sensibilidade do Mouse", "Mouse Sensitivity"),
                                    cfg.mouseSensitivity, 10, 400, 5, "%", &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("SENSIBILIDADE DO MOUSE", "MOUSE SENSITIVITY");
                    inspector.desc = tr("Ajusta a velocidade de rotação da câmera guiada pelo mouse.",
                                        "Scales camera rotation speed when driven by the mouse.");
                    inspector.badge = tr("SENSIBILIDADE", "SENSITIVITY");
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcCycleRow("#ctrl-opt-moufreeaim", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Mira Livre com o Mouse (Botão Direito)", "Mouse Free Aim (Right Click)"),
                                   {tr("Não (Trava PS2)", "Off (PS2 Lock-On)"), tr("Sim (Mira PC)", "On (PC Crosshair)")},
                                   cfg.mouseFreeAim, &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("MIRA LIVRE COM O MOUSE", "MOUSE FREE AIM");
                    inspector.desc = tr("Com armas de fogo, segurar o botão direito abre a mira livre guiada pelo mouse em vez da trava automática de PS2.",
                                        "With firearms, holding right click activates mouse-steered free aim with reticle instead of PS2 lock-on.");
                    inspector.badge = tr("RECOMENDADO: SIM", "RECOMMENDED: ON");
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcCycleRow("#ctrl-opt-mouinv", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Inverter Eixo Y do Mouse", "Invert Mouse Up / Down"),
                                   {tr("Não", "Off"), tr("Sim", "On")}, cfg.mouseInvertY, &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("INVERTER EIXO Y DO MOUSE", "INVERT MOUSE UP / DOWN");
                    inspector.desc = tr("Inverte o movimento vertical da câmera ao mover o mouse para frente ou para trás.",
                                        "Inverts vertical camera pitch when moving the mouse forward or backward.");
                    inspector.badge = "AXIS Y";
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcSliderRow("#ctrl-opt-moucarsens", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                    tr("Sensibilidade da Câmera em Veículos", "Car Camera Sensitivity"),
                                    cfg.mouseCarSensitivity, 10, 400, 5, "%", &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("SENSIBILIDADE DA CÂMERA EM VEÍCULOS", "CAR CAMERA SENSITIVITY");
                    inspector.desc = tr("Multiplicador extra da velocidade do mouse enquanto dirige carros, motos ou barcos.",
                                        "Multiplier applied to mouse camera sensitivity while driving vehicles.");
                    inspector.badge = "CAR CAMERA";
                }
                y += rowH;
            }
            {
                static const int kCarReturnVals[] = {5, 10, 15, 20, 30, 50, -1};
                int retIdx = indexInList(cfg.mouseCarReturn, kCarReturnVals, 7, 2);
                bool hov = false;
                if (ui::vcCycleRow("#ctrl-opt-moucarret", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Retorno da Câmera em Veículos", "Car Camera Return"),
                                   {"0.5 s", "1 s", "1.5 s", "2 s", "3 s", "5 s", tr("Nunca", "Never")}, retIdx, &hov))
                {
                    cfg.mouseCarReturn = kCarReturnVals[retIdx];
                    changed = true;
                }
                if (hov)
                {
                    inspector.title = tr("RETORNO DA CÂMERA EM VEÍCULOS", "CAR CAMERA AUTO-CENTER");
                    inspector.desc = tr("Tempo após soltar o mouse para a câmera retornar suavemente atrás do veículo.",
                                        "Delay after mouse stops moving before the camera smoothly centers behind the vehicle.");
                    inspector.badge = "AUTO-CENTER";
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcCycleRow("#ctrl-opt-mouwheel", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Roda do Mouse (Troca de Armas & Zoom)", "Mouse Wheel Weapons & Zoom"),
                                   {tr("Não", "Off"), tr("Sim", "On")}, cfg.mouseWheel, &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("RODA DO MOUSE (SCROLL WHEEL)", "MOUSE SCROLL WHEEL");
                    inspector.desc = tr("Gira para alternar armas rapidamente, aplicar zoom em lunetas e trocar de rádio.",
                                        "Scroll to cycle weapons rapidly, zoom sniper scopes and change radio stations.");
                    inspector.badge = "WHEEL";
                }
                y += rowH;
            }

            // Restore Defaults Action
            {
                bool hov = false;
                if (ui::vcActionRow("#ctrl-restore-defaults", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                    tr("Restaurar Padrões de Controle", "Restore Control Defaults"),
                                    tr("< Restaurar Padrões >", "< Restore Defaults >"), 0, &hov))
                {
                    cfg.applyPadPreset(1);
                    cfg.padDeadzone = 12;
                    cfg.padCameraSpeed = 100;
                    cfg.padVibration = 100;
                    cfg.padInvertY = 0;
                    cfg.dualsenseTriggers = 1;
                    cfg.dualsenseLightbar = 2;
                    cfg.dualsensePoliceFlash = 1;
                    cfg.dualsenseSpeaker = 0;
                    cfg.mouseCamera = 1;
                    cfg.mouseSensitivity = 100;
                    cfg.mouseCarSensitivity = 100;
                    cfg.mouseCarReturn = 15;
                    cfg.mouseFreeAim = 1;
                    cfg.mouseInvertY = 0;
                    cfg.mouseWheel = 1;
                    changed = true;
                    toast(tr("Padrões de controle restaurados.", "Control defaults restored."));
                }
                if (hov)
                {
                    inspector.title = tr("RESTAURAR PADRÕES DE CONTROLE", "RESTORE CONTROL DEFAULTS");
                    inspector.desc = tr("Redefine todas as opções de controles, gatilhos, deadzone e mouse para os valores de fábrica.",
                                        "Resets all control presets, triggers, deadzones and mouse settings to factory defaults.");
                    inspector.badge = tr("PADRÃO", "DEFAULT");
                }
                y += rowH;
            }

            if (changed)
                app.saveGameConfig();

            fillRemainingVcStripes(tableR, y, rowH, rowIdx);
            ui::endScroll(scrollId, y);
        }

        drawVcInspectorPanel(inspR, inspector,
                             cfg.estimatedGpuCostPercent(),
                             cfg.estimatedCpuCostPercent(),
                             cfg.estimatedStreamingLoadMb(),
                             cfg.streamingPoolCapacityMb());
    }

    // =================================================================================================================
    // TAB 4: CONFIGURAÇÕES / OPTIONS — Classic 1980s GTA: Vice City Menu Style
    // =================================================================================================================
    void drawConfig(Rect r)
    {
        App &app = *shell.app;
        launcher::GameConfig &cfg = app.gameConfig;
        const Rect area = r.inset(px(38), px(10));

        // 5 Classic Vice City Sub-Tabs
        const char *subTabs[5] = {
            tr("TELA", "DISPLAY"),
            tr("GRÁFICOS", "GRAPHICS"),
            tr("MUNDO", "WORLD"),
            tr("POPULAÇÃO", "POPULATION"),
            tr("ÁUDIO & JOGO", "AUDIO & GAME"),
        };
        const Rect tabBar{area.x, area.y, area.w, px(46)};
        drawVcCategoryBar(tabBar, subTabs, 5, shell.configSubTab);

        const float inspH = px(140);
        const Rect inspR{area.x, area.bottom() - inspH, area.w, inspH};
        const Rect tableR{area.x, tabBar.bottom() + px(6), area.w, inspR.y - tabBar.bottom() - px(12)};

        VcOptionInspector inspector{
            tr("INSPEÇÃO DE IMPACTO DE DESEMPENHO (TEMPO REAL)", "REAL-TIME PERFORMANCE IMPACT INSPECTOR"),
            tr("Passe o mouse sobre qualquer opção ou arraste os sliders rosas para inspecionar o custo real em GPU, CPU e Streaming.",
               "Hover over any option or drag the pink sliders to inspect real hardware cost on GPU, CPU and Streaming."),
            tr("GRAVAÇÃO DIRETA EM VCS.INI", "LIVE VCS.INI SYNC"),
            0,
            0,
            0.0f,
        };

        const ui::Id scrollId = ui::id("#vc-config-scroll", shell.configSubTab);
        ui::beginScroll(scrollId, tableR);
        float y = tableR.y;
        const float rowH = px(44);
        int rowIdx = 0;
        bool changed = false;

        if (shell.configSubTab == 0)
        {
            // ---- SUB-TAB 0: TELA / DISPLAY ----
            {
                bool hov = false;
                if (ui::vcCycleRow("#vc-disp-win", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Modo de Exibição", "Display Mode"),
                                   {tr("Tela Cheia Sem Bordas", "Borderless Fullscreen"), tr("Janela", "Windowed")},
                                   cfg.displayWindow, &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("MODO DE EXIBIÇÃO", "DISPLAY MODE");
                    inspector.desc = tr("Alterna entre janela redimensionável na área de trabalho e tela cheia sem bordas com flip imediato.",
                                        "Switches between resizable desktop window and borderless fullscreen with immediate flip presentation.");
                    inspector.badge = tr("IMPACTO GPU: 0%  •  CPU: 0%", "GPU COST: 0%  •  CPU: 0%");
                }
                y += rowH;
            }
            {
                static const int kAspects[] = {0, 43, 1610, 169, 219, 329};
                int idx = indexInList(cfg.displayAspect, kAspects, 6, 3);
                bool hov = false;
                if (ui::vcCycleRow("#vc-disp-aspect", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Proporção da Tela", "Aspect Ratio"),
                                   {tr("Automático", "Auto"), "4:3", "16:10", "16:9", "21:9", "32:9"}, idx, &hov))
                {
                    cfg.displayAspect = kAspects[idx];
                    changed = true;
                }
                if (hov)
                {
                    inspector.title = tr("PROPORÇÃO DA TELA", "ASPECT RATIO");
                    inspector.desc = tr("Expande o campo de visão horizontal (FOV) da câmera 3D sem esticar os personagens ou veículos.",
                                        "Expands horizontal 3D camera field of view (FOV) without stretching characters or vehicles.");
                    inspector.badge = tr("IMPACTO GPU: +2% (ULTRAWIDE)", "GPU COST: +2% (ULTRAWIDE)");
                    inspector.gpuDeltaPct = 2;
                    inspector.cpuDeltaPct = 1;
                }
                y += rowH;
            }
            {
                static const int kHudVals[] = {2, 1, 0};
                int idx = indexInList(cfg.displayHud, kHudVals, 3, 0);
                bool hov = false;
                if (ui::vcCycleRow("#vc-disp-hud", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Proporção do HUD e Radar", "HUD & Radar Aspect"),
                                   {"Widescreen", tr("Centralizado 4:3", "Centered 4:3"), tr("Esticado", "Stretched")},
                                   idx, &hov))
                {
                    cfg.displayHud = kHudVals[idx];
                    changed = true;
                }
                if (hov)
                {
                    inspector.title = tr("PROPORÇÃO DO HUD E RADAR", "HUD & RADAR ASPECT");
                    inspector.desc = tr("Mantém o radar circular, mira e textos ancorados corretamente nas bordas de monitores 16:9 e 21:9.",
                                        "Keeps the radar circular, crosshair and HUD texts properly anchored on 16:9 and 21:9 monitors.");
                    inspector.badge = tr("IMPACTO: 0%", "COST: 0%");
                }
                y += rowH;
            }
            {
                static const int kFpsVals[] = {30, 60, 100, 120, 144, 165, 240, -1, 0};
                int idx = indexInList(cfg.displayFps, kFpsVals, 9, 1);
                bool hov = false;
                if (ui::vcCycleRow("#vc-disp-fps", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Limite de Taxa de Quadros", "Frame Rate Limit"),
                                   {"30 FPS", "60 FPS", "100 FPS", "120 FPS", "144 FPS", "165 FPS", "240 FPS",
                                    tr("Taxa do Monitor", "Monitor Refresh"), tr("Ilimitado", "Unlimited")},
                                   idx, &hov))
                {
                    cfg.displayFps = kFpsVals[idx];
                    changed = true;
                }
                if (hov)
                {
                    const int f = (cfg.displayFps <= 0) ? 144 : cfg.displayFps;
                    const int dCpu = (f <= 30) ? 0 : (f <= 60 ? 12 : 24);
                    const int dGpu = (f <= 30) ? 0 : (f <= 60 ? 10 : 20);
                    inspector.title = tr("LIMITE DE TAXA DE QUADROS", "FRAME RATE LIMIT");
                    inspector.desc = tr("60+ FPS duplica a frequência de simulação e comandos de desenho em relação aos 30 FPS originais do PS2.",
                                        "60+ FPS doubles simulation tick rate and draw submissions compared to original 30 FPS PS2 mode.");
                    inspector.badge = tr("IMPACTO CPU: MÉDIO  •  GPU: MÉDIO", "CPU COST: MEDIUM  •  GPU: MEDIUM");
                    inspector.gpuDeltaPct = dGpu;
                    inspector.cpuDeltaPct = dCpu;
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcCycleRow("#vc-disp-vsync", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Sincronização Vertical", "Vertical Sync"),
                                   {tr("Não", "Off"), tr("Sim", "On"), tr("Adaptativo", "Adaptive")},
                                   cfg.displayVsync, &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("SINCRONIZAÇÃO VERTICAL", "VERTICAL SYNC");
                    inspector.desc = tr("Sincroniza a apresentação de quadros com o monitor para eliminar cortes horizontais na imagem (tearing).",
                                        "Synchronizes frame presentation with monitor refresh rate to eliminate horizontal screen tearing.");
                    inspector.badge = tr("IMPACTO GPU: 0%", "GPU COST: 0%");
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcCycleRow("#vc-disp-timing", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Padrão de Timing", "Simulation Timing"),
                                   {"PAL (25/50 Hz)", "NTSC (30/60 Hz)"}, cfg.displayTiming, &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("PADRÃO DE TIMING DA SIMULAÇÃO", "SIMULATION TIMING");
                    inspector.desc = tr("NTSC (30/60 Hz) é o padrão nativo da mídia americana SLUS-21590 usada pelo VCS.rec.",
                                        "NTSC (30/60 Hz) is the native timing standard for the North American SLUS-21590 disc.");
                    inspector.badge = tr("RECOMENDADO: NTSC", "RECOMMENDED: NTSC");
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcCycleRow("#vc-disp-cutbars", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Barras Pretas nas Cutscenes", "Cutscene Letterbox Bars"),
                                   {tr("Não", "Off"), tr("Sim", "On")}, cfg.displayCutsceneBars, &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("BARRAS PRETAS NAS CUTSCENES", "CUTSCENE LETTERBOX BARS");
                    inspector.desc = tr("Exibe ou remove as faixas pretas cinematográficas superior e inferior durante as cenas de história.",
                                        "Shows or removes top and bottom cinematic letterbox bars during story cutscenes.");
                    inspector.badge = tr("IMPACTO: 0%", "COST: 0%");
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcCycleRow("#vc-disp-fpscnt", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Contador de FPS na Tela", "On-Screen FPS Counter"),
                                   {tr("Não", "Off"), tr("Superior Esq.", "Top Left"), tr("Superior Dir.", "Top Right"),
                                    tr("Inferior Esq.", "Bottom Left"), tr("Inferior Dir.", "Bottom Right")},
                                   cfg.displayFpsCounter, &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("CONTADOR DE FPS NA TELA", "ON-SCREEN FPS COUNTER");
                    inspector.desc = tr("Exibe a taxa de quadros por segundo em tempo real no canto selecionado da tela.",
                                        "Displays real-time frames per second in the selected corner of the screen.");
                    inspector.badge = tr("IMPACTO: 0%", "COST: 0%");
                }
                y += rowH;
            }
        }
        else if (shell.configSubTab == 1)
        {
            // ---- SUB-TAB 1: GRÁFICOS / GRAPHICS ----
            {
                int presetIdx = static_cast<int>(cfg.detectPreset());
                bool hov = false;
                if (ui::vcCycleRow("#vc-gfx-preset", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Predefinição Gráfica", "Graphics Preset"),
                                   {tr("Personalizado", "Custom"),
                                    tr("Original (PS2)", "Original (PS2)"),
                                    tr("Baixo", "Low"),
                                    tr("Médio", "Medium"),
                                    tr("Alto", "High"),
                                    "Vice Ultra"},
                                   presetIdx, &hov))
                {
                    if (presetIdx > 0)
                    {
                        cfg.applyPreset(static_cast<launcher::GraphicsPreset>(presetIdx));
                        changed = true;
                    }
                }
                if (hov)
                {
                    inspector.title = tr("PREDEFINIÇÃO GRÁFICA COMPLETA", "COMPLETE GRAPHICS PRESET");
                    inspector.desc = tr("Alterne entre Original (PS2), Baixo, Médio, Alto e Vice Ultra para configurar automaticamente Resolução, MSAA, Mundo HD e Densidade.",
                                        "Cycle between Original (PS2), Low, Medium, High and Vice Ultra to automatically configure Resolution, MSAA, HD World and Density.");
                    inspector.badge = tr("5 PERFIS PREDEFINIDOS", "5 PRESET PROFILES");
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcCycleRow("#vc-gfx-api", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("API de Renderização", "Graphics Backend API"),
                                   {"Direct3D 11", "Direct3D 12", "Vulkan"}, cfg.displayRenderer, &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("API DE RENDERIZAÇÃO", "GRAPHICS BACKEND API");
                    inspector.desc = tr("Direct3D 12 oferece menor overhead de CPU e suporte ao Cache de Shaders SCSKiller; D3D11 é o mais compatível.",
                                        "Direct3D 12 offers lower CPU overhead and SCSKiller PSO Shader Cache support; D3D11 has widest compatibility.");
                    inspector.badge = (cfg.displayRenderer == 2) ? "VULKAN" : (cfg.displayRenderer == 1 ? "DIRECT3D 12" : "DIRECT3D 11");
                }
                y += rowH;
            }
            {
                static const int kScales[] = {0, 1, 2, 3, 4, 6, 8};
                int idx = indexInList(cfg.displayScale, kScales, 7, 0);
                bool hov = false;
                if (ui::vcCycleRow("#vc-gfx-scale", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Escala de Resolução Interna", "Internal Resolution Scale"),
                                   {tr("Nativa (Janela)", "Match Window"), "1x (PS2 448p)", "2x (720p)", "3x (1080p)",
                                    "4x (1440p)", "6x (4K)", "8x (Supersampling)"},
                                   idx, &hov))
                {
                    cfg.displayScale = kScales[idx];
                    changed = true;
                }
                if (hov)
                {
                    const int s = (cfg.displayScale == 0) ? 3 : cfg.displayScale;
                    const int dGpu = (s <= 1) ? 0 : (s == 2 ? 8 : (s == 3 ? 16 : (s == 4 ? 26 : (s == 6 ? 40 : 54))));
                    inspector.title = tr("ESCALA DE RESOLUÇÃO INTERNA", "INTERNAL RESOLUTION SCALE");
                    inspector.desc = tr("Multiplica a resolução interna do framebuffer GS. É a opção de maior impacto direto na placa de vídeo (GPU).",
                                        "Multiplies internal GS framebuffer resolution. This setting has the highest direct impact on GPU fillrate.");
                    inspector.badge = tr("IMPACTO GPU: ALTO (+8% A +54%)  •  CPU: 0%", "GPU COST: HIGH (+8% TO +54%)  •  CPU: 0%");
                    inspector.gpuDeltaPct = dGpu;
                }
                y += rowH;
            }
            {
                static const int kMsaa[] = {1, 2, 4, 8};
                int idx = indexInList(cfg.displayMsaa, kMsaa, 4, 2);
                bool hov = false;
                if (ui::vcCycleRow("#vc-gfx-msaa", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   "Anti-Aliasing (MSAA)",
                                   {tr("Não", "Off"), "2x MSAA", "4x MSAA", "8x MSAA"}, idx, &hov))
                {
                    cfg.displayMsaa = kMsaa[idx];
                    changed = true;
                }
                if (hov)
                {
                    const int dGpu = (cfg.displayMsaa <= 1) ? 0 : (cfg.displayMsaa == 2 ? 4 : (cfg.displayMsaa == 4 ? 9 : 18));
                    inspector.title = tr("ANTI-ALIASING (MSAA)", "ANTI-ALIASING (MSAA)");
                    inspector.desc = tr("Remove o serrilhado nas bordas de veículos, fios de postes e prédios usando múltiplas amostras por pixel.",
                                        "Smooths jagged edges on vehicles, power lines and buildings using hardware multi-sampling.");
                    inspector.badge = tr("IMPACTO GPU: +4% (2X) A +18% (8X)", "GPU COST: +4% (2X) TO +18% (8X)");
                    inspector.gpuDeltaPct = dGpu;
                }
                y += rowH;
            }
            {
                static const int kAniso[] = {0, 2, 4, 8, 16};
                int idx = indexInList(cfg.displayAniso, kAniso, 5, 4);
                bool hov = false;
                if (ui::vcCycleRow("#vc-gfx-aniso", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Filtro Anisotrópico", "Anisotropic Filtering"),
                                   {"Trilinear", "2x", "4x", "8x", "16x"}, idx, &hov))
                {
                    cfg.displayAniso = kAniso[idx];
                    changed = true;
                }
                if (hov)
                {
                    inspector.title = tr("FILTRO ANISOTRÓPICO DE TEXTURAS", "ANISOTROPIC TEXTURE FILTERING");
                    inspector.desc = tr("Elimina o borrado de texturas de asfalto, faixas de pedestres e calçadas vistas em ângulos rasos.",
                                        "Eliminates blurriness on road asphalt, crosswalks and sidewalks viewed at oblique angles.");
                    inspector.badge = tr("IMPACTO GPU: MUITO BAIXO (+2% EM 16X)", "GPU COST: VERY LOW (+2% AT 16X)");
                    inspector.gpuDeltaPct = 2;
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcCycleRow("#vc-gfx-post", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Pós-Processamento (Trails & Bloom)", "Post-Processing (Trails & Bloom)"),
                                   {tr("Não", "Off"), tr("Sim (Trails + Bloom)", "On (Trails + Bloom)")},
                                   cfg.graphicsPostProcess, &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("PÓS-PROCESSAMENTO ANOS 80", "1980S POST-PROCESSING");
                    inspector.desc = tr("Recria o brilho quente de pôr do sol e o halo neon característico de Vice City Stories no PS2.",
                                        "Recreates the signature warm 1980s sunset glow and neon bloom from the original PS2 release.");
                    inspector.badge = tr("IMPACTO GPU: BAIXO (+5%)", "GPU COST: LOW (+5%)");
                    inspector.gpuDeltaPct = 5;
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcSliderRow("#vc-gfx-exp", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                     tr("Intensidade do Brilho (Exposição)", "Bloom & Exposure Intensity"),
                                     cfg.graphicsExposure, 0, 200, 5, "%", &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("INTENSIDADE DO BRILHO NEON", "NEON BLOOM INTENSITY");
                    inspector.desc = tr("Arraste o slider rosa para controlar a força do bloom nos letreiros de Ocean Drive e no pôr do sol (100% = padrão).",
                                        "Drag the pink slider to adjust neon sign and sunset bloom strength (100% = default).");
                    inspector.badge = tr("IMPACTO GPU: 0%", "GPU COST: 0%");
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcCycleRow("#vc-gfx-drop", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Gotas de Chuva na Lente", "Rain Droplets"),
                                   {tr("Desligado (PS2)", "Off (PS2)"), tr("Ligado (Xbox)", "On (Xbox)")},
                                   cfg.graphicsRainDroplets, &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("GOTAS DE CHUVA NA LENTE", "XBOX RAIN DROPLETS");
                    inspector.desc = tr("Simula gotas refratando na lente da câmera durante chuva ou respingos d'água (atrás do HUD, pausado em interiores/túneis/menus).",
                                        "Refractive screen-space water droplets on the camera lens during rain and splashes (pre-HUD, paused indoors and in menus).");
                    inspector.badge = tr("IMPACTO GPU: BAIXO (+1%)", "GPU COST: LOW (+1%)");
                    inspector.gpuDeltaPct = 1;
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcCycleRow("#vc-gfx-stoch", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Shader Stochastic", "Stochastic Texture Tiling"),
                                   {tr("Desligado (PS2)", "Off (PS2)"), tr("Ligado", "On")},
                                   cfg.graphicsStochastic, &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("SHADER STOCHASTIC", "STOCHASTIC TEXTURE TILING");
                    inspector.desc = tr("Quebra a repetição em grama, areia, terra e asfalto usando mistura aleatória em malha triangular (Deliot & Heitz, estilo SkyGfx).",
                                        "Breaks visible tiling grids on grass, sand, dirt and asphalt via procedural simplex cell blending (Deliot & Heitz, SkyGfx style).");
                    inspector.badge = tr("IMPACTO GPU: BAIXO (+2%)", "GPU COST: LOW (+2%)");
                    inspector.gpuDeltaPct = 2;
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcActionRow("#vc-gfx-reset", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                    tr("Restaurar padrões", "Restore Defaults"),
                                    tr("< Restaurar >", "< Restore >"), 0, &hov))
                {
                    cfg.applyPreset(launcher::GraphicsPreset::Medium);
#ifdef _WIN32
                    cfg.displayRenderer = 1;
#else
                    cfg.displayRenderer = 2;
#endif
                    changed = true;
                    toast(tr("Configurações restauradas para o padrão Médio.", "Settings restored to Medium defaults."));
                }
                if (hov)
                {
                    inspector.title = tr("RESTAURAR PADRÕES RECOMENDADOS", "RESTORE RECOMMENDED DEFAULTS");
                    inspector.desc = tr("Redefine todas as opções gráficas, de mundo e população para o perfil Médio (Recomendado).",
                                        "Resets all graphics, world and population settings back to the Medium profile (Recommended).");
                    inspector.badge = tr("PERFIL MÉDIO", "MEDIUM PROFILE");
                }
                y += rowH;
            }
        }
        else if (shell.configSubTab == 2)
        {
            // ---- SUB-TAB 2: MUNDO / WORLD ----
            {
                bool hov = false;
                if (ui::vcCycleRow("#vc-wld-hd", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Raio de Mundo HD", "HD World Streaming Radius"),
                                   {"~260m (PS2)", "~450m", "~580m", "~720m"}, cfg.worldHdRadius, &hov))
                    changed = true;
                if (hov)
                {
                    const int dCpu = cfg.worldHdRadius * 7;
                    const int dGpu = cfg.worldHdRadius * 4;
                    const float dMem = float(cfg.worldHdRadius) * 2.2f;
                    inspector.title = tr("RAIO DE MUNDO HD", "HD WORLD RADIUS");
                    inspector.desc = tr("Substitui blocos LOD de baixa contagem poligonal por edifícios e interiores HD completos à distância.",
                                        "Replaces distant low-polygon LOD blocks with full high-detail buildings and geometry.");
                    inspector.badge = tr("IMPACTO CPU: +7% A +21%  •  STREAMING: +6.6 MB", "CPU COST: +7% TO +21%  •  STREAMING: +6.6 MB");
                    inspector.gpuDeltaPct = dGpu;
                    inspector.cpuDeltaPct = dCpu;
                    inspector.streamDeltaMb = dMem;
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcSliderRow("#vc-wld-view", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                    tr("Distância de Visão Geral", "General View Distance"),
                                    cfg.worldViewDistance, 0, 100, 5, "%", &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("DISTÂNCIA DE VISÃO GERAL", "GENERAL VIEW DISTANCE");
                    inspector.desc = tr("Amplia a distância de corte (far clip) de prédios, pontes e palmeiras através das duas ilhas de Vice City.",
                                        "Extends far draw distance for buildings, bridges and palm trees across both Vice City islands.");
                    inspector.badge = tr("IMPACTO GPU: +8%  •  CPU: +8%", "GPU COST: +8%  •  CPU: +8%");
                    inspector.gpuDeltaPct = int(std::round(float(cfg.worldViewDistance) * 0.08f));
                    inspector.cpuDeltaPct = int(std::round(float(cfg.worldViewDistance) * 0.08f));
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcSliderRow("#vc-wld-detail", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                    tr("Distância de Objetos e Detalhes", "Object & Detail Distance"),
                                    cfg.worldDetailDistance, 0, 100, 5, "%", &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("DISTÂNCIA DE OBJETOS E DETALHES", "OBJECT & DETAIL DISTANCE");
                    inspector.desc = tr("Controla quão longe postes, semáforos, cercas, bancos e letreiros permanecem visíveis sem desaparecer.",
                                        "Controls how far streetlamps, traffic lights, fences and benches remain visible without popping in.");
                    inspector.badge = tr("IMPACTO CPU: +6%  •  GPU: +4%", "CPU COST: +6%  •  GPU: +4%");
                    inspector.gpuDeltaPct = int(std::round(float(cfg.worldDetailDistance) * 0.04f));
                    inspector.cpuDeltaPct = int(std::round(float(cfg.worldDetailDistance) * 0.06f));
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcSliderRow("#vc-wld-fogdist", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                    tr("Alcance da Neblina Atmosférica", "Atmospheric Fog Range"),
                                    cfg.fogDistance, 10, 100, 5, "%", &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("ALCANCE DA NEBLINA ATMOSFÉRICA", "ATMOSPHERIC FOG RANGE");
                    inspector.desc = tr("Empurra a parede de neblina do horizonte para trás para acompanhar distâncias de visão maiores.",
                                        "Pushes the horizon fog wall back to match extended view distances across the map.");
                    inspector.badge = tr("IMPACTO GPU: BAIXO (+2%)", "GPU COST: LOW (+2%)");
                    inspector.gpuDeltaPct = 2;
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcSliderRow("#vc-fogstart", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                    tr("Início da Neblina", "Fog Start Distance"),
                                    cfg.fogStart, 0, 300, 10, "m", &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("INÍCIO DA NEBLINA", "FOG START DISTANCE");
                    inspector.desc = tr("Define a distância em metros a partir da câmera onde a névoa tropical começa a se formar.",
                                        "Sets the distance in meters from the camera where tropical atmospheric haze begins.");
                    inspector.badge = tr("IMPACTO: 0%", "COST: 0%");
                }
                y += rowH;
            }
            {
                static const int kMemVals[] = {0, 12, 20, 30};
                int idx = indexInList(cfg.memoryStreaming, kMemVals, 4, 0);
                bool hov = false;
                if (ui::vcCycleRow("#vc-wld-mem", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Pool de Memória de Streaming", "Streaming Memory Pool"),
                                   {"5.4 MB (PS2)", "12 MB", "20 MB", "30 MB"}, idx, &hov))
                {
                    cfg.memoryStreaming = kMemVals[idx];
                    changed = true;
                }
                if (hov)
                {
                    inspector.title = tr("POOL DE MEMÓRIA DE STREAMING", "STREAMING MEMORY POOL");
                    inspector.desc = tr("Reserva de RAM para modelos e texturas carregados. Use 20 MB ou 30 MB ao ativar Mundo HD e pacotes de Mods.",
                                        "RAM pool for streamed models and textures. Use 20 MB or 30 MB when enabling HD World and Mod packs.");
                    inspector.badge = tr("EVITA POP-IN DE TEXTURAS", "PREVENTS TEXTURE POP-IN");
                }
                y += rowH;
            }
        }
        else if (shell.configSubTab == 3)
        {
            // ---- SUB-TAB 3: POPULAÇÃO / POPULATION ----
            {
                bool hov = false;
                if (ui::vcSliderRow("#vc-pop-dist", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                    tr("Distância de Spawn da População", "Population Spawn Distance"),
                                    cfg.worldPopulationDistance, 0, 100, 5, "%", &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("DISTÂNCIA DE SPAWN DA POPULAÇÃO", "POPULATION SPAWN DISTANCE");
                    inspector.desc = tr("Amplia o raio ao redor do jogador onde pedestres e veículos com física completa permanecem ativos.",
                                        "Expands the radius around the player where physics-enabled pedestrians and vehicles stay active.");
                    inspector.badge = tr("IMPACTO CPU: +10%  •  GPU: +4%", "CPU COST: +10%  •  GPU: +4%");
                    inspector.cpuDeltaPct = int(std::round(float(cfg.worldPopulationDistance) * 0.10f));
                    inspector.gpuDeltaPct = int(std::round(float(cfg.worldPopulationDistance) * 0.04f));
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcSliderRow("#vc-pop-veh", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                    tr("Densidade de Veículos", "Vehicle Traffic Density"),
                                    cfg.worldVehicleDensity, 0, 100, 5, "%", &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("DENSIDADE DE VEÍCULOS NAS VIAS", "ROAD VEHICLE TRAFFIC DENSITY");
                    inspector.desc = tr("Aumenta o número de carros, motos e barcos circulando simultaneamente com IA de trânsito e colisão.",
                                        "Increases the number of cars, bikes and boats circulating simultaneously with traffic AI and collision.");
                    inspector.badge = tr("IMPACTO CPU: +12%  •  GPU: +6%", "CPU COST: +12%  •  GPU: +6%");
                    inspector.cpuDeltaPct = int(std::round(float(cfg.worldVehicleDensity) * 0.12f));
                    inspector.gpuDeltaPct = int(std::round(float(cfg.worldVehicleDensity) * 0.06f));
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcSliderRow("#vc-pop-ped", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                    tr("Densidade de Pedestres", "Pedestrian Density"),
                                    cfg.worldPedestrianDensity, 0, 100, 5, "%", &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("DENSIDADE DE PEDESTRES NAS CALÇADAS", "SIDEWALK PEDESTRIAN DENSITY");
                    inspector.desc = tr("Controla a quantidade de pedestres e banhistas caminhando pelas praias e avenidas de Vice City.",
                                        "Controls the number of pedestrians and beachgoers walking along Vice City beaches and avenues.");
                    inspector.badge = tr("IMPACTO CPU: +10%  •  GPU: +4%", "CPU COST: +10%  •  GPU: +4%");
                    inspector.cpuDeltaPct = int(std::round(float(cfg.worldPedestrianDensity) * 0.10f));
                    inspector.gpuDeltaPct = int(std::round(float(cfg.worldPedestrianDensity) * 0.04f));
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcSliderRow("#vc-pop-dveh", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                    tr("Tráfego Distante", "Distant Traffic Lights"),
                                    cfg.worldDistantTraffic, 0, 100, 5, "%", &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("TRÁFEGO DISTANTE", "DISTANT TRAFFIC");
                    inspector.desc = tr("Renderiza veículos e faróis leves em pontes e avenidas distantes sem sobrecarregar a física da CPU.",
                                        "Renders lightweight vehicles and headlights on distant bridges and avenues without CPU physics overhead.");
                    inspector.badge = tr("IMPACTO GPU: +4%  •  CPU: +3%", "GPU COST: +4%  •  CPU: +3%");
                    inspector.gpuDeltaPct = int(std::round(float(cfg.worldDistantTraffic) * 0.04f));
                    inspector.cpuDeltaPct = int(std::round(float(cfg.worldDistantTraffic) * 0.03f));
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcSliderRow("#vc-pop-dped", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                    tr("Multidões Distantes", "Distant Crowds"),
                                    cfg.worldDistantCrowds, 0, 100, 5, "%", &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("MULTIDÕES DISTANTES", "DISTANT CROWDS");
                    inspector.desc = tr("Desenha pedestres distantes além do anel principal de simulação para manter as ruas vivas.",
                                        "Draws distant pedestrians beyond the primary simulation ring to keep streets looking lively.");
                    inspector.badge = tr("IMPACTO GPU: +3%  •  CPU: +3%", "GPU COST: +3%  •  CPU: +3%");
                    inspector.gpuDeltaPct = int(std::round(float(cfg.worldDistantCrowds) * 0.03f));
                    inspector.cpuDeltaPct = int(std::round(float(cfg.worldDistantCrowds) * 0.03f));
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcCycleRow("#vc-pop-bands", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Bandas Extras de Multidão", "Extra Crowd Bands"),
                                   {tr("Não", "Off"), tr("Sim", "On")}, cfg.worldCrowdBands, &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("BANDAS EXTRAS DE MULTIDÃO", "EXTRA CROWD SPAWN BANDS");
                    inspector.desc = tr("Ativa anéis externos adicionais de geração de pedestres em calçadões largos como Ocean Drive.",
                                        "Enables additional outer pedestrian spawn rings along wide promenades such as Ocean Drive.");
                    inspector.badge = tr("IMPACTO CPU: +4%", "CPU COST: +4%");
                    inspector.cpuDeltaPct = 4;
                }
                y += rowH;
            }
            {
                static const int kTargets[] = {0, 45, 60, 70, 90, 120, 144};
                int idx = indexInList(cfg.worldFpsTarget, kTargets, 7, 2);
                bool hov = false;
                if (ui::vcCycleRow("#vc-pop-target", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Alvo de FPS Dinâmico da População", "Dynamic Population FPS Target"),
                                   {tr("Desativado", "Off"), "45 FPS", "60 FPS", "70 FPS", "90 FPS", "120 FPS", "144 FPS"},
                                   idx, &hov))
                {
                    cfg.worldFpsTarget = kTargets[idx];
                    changed = true;
                }
                if (hov)
                {
                    inspector.title = tr("ALVO DE FPS DINÂMICO", "DYNAMIC POPULATION FPS TARGET");
                    inspector.desc = tr("Ajusta automaticamente o tráfego distante em momentos de ação intensa para sustentar o FPS desejado.",
                                        "Automatically scales back distant traffic during heavy action to sustain your target framerate.");
                    inspector.badge = tr("ESTABILIZADOR DE FPS", "FPS STABILIZER");
                }
                y += rowH;
            }
        }
        else
        {
            // ---- SUB-TAB 4: ÁUDIO & JOGO / AUDIO & GAME ----
            {
                int langIdx = app.isPt() ? 0 : 1;
                bool hov = false;
                if (ui::vcCycleRow("#vc-aud-lang", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Idioma do Launcher", "Launcher Language"),
                                   {"Português do Brasil", "English"}, langIdx, &hov))
                {
                    app.settings.language = (langIdx == 0) ? "pt" : "en";
                    app.settings.languageChosen = true;
                    app.saveSettings();
                }
                if (hov)
                {
                    inspector.title = tr("IDIOMA DA INTERFACE DO LAUNCHER", "LAUNCHER INTERFACE LANGUAGE");
                    inspector.desc = tr("Altera instantaneamente todos os botões, abas e descrições entre Português do Brasil e Inglês.",
                                        "Instantly switches all launcher buttons, tabs and descriptions between Brazilian Portuguese and English.");
                    inspector.badge = "PT-BR / EN-US";
                }
                y += rowH;
            }
            {
                const bool ptBrModInstalled = cfg.hasModKeyInIni(launcher::App::kPtBrModKey) ||
                                              std::any_of(cfg.mods.begin(), cfg.mods.end(),
                                                          [](const launcher::ModEntry &m) { return m.key == launcher::App::kPtBrModKey; });
                const bool extGxt = cfg.hasExternalGxtMod();
                int gameLangIdx = app.isGameLanguagePt() ? 1 : 0;
                bool hov = false;
                if (ui::vcCycleRow("#vc-aud-gamelang", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Idioma do Jogo", "Game Language"),
                                   {tr("English (Original)", "English (Original)"), "Português (Brasil — PT-BR)"},
                                   gameLangIdx, &hov))
                {
                    app.setGameLanguagePt(gameLangIdx == 1);
                }
                if (hov)
                {
                    inspector.title = tr("IDIOMA DO JOGO (TEXTOS E LEGENDAS)", "GAME LANGUAGE (TEXTS & SUBTITLES)");
                    if (extGxt)
                        inspector.desc = tr("Outro mod de idioma (.GXT) está ativo na pasta modloader/ e pode sobrescrever os textos do jogo.",
                                            "Another translation mod (.GXT) is currently active in modloader/ and may override in-game text.");
                    else if (!ptBrModInstalled)
                        inspector.desc = tr("Pasta 'PT-BR (VCS.rec)' não encontrada em modloader/. Sem ela o jogo iniciará em Inglês.",
                                            "Folder 'PT-BR (VCS.rec)' was not found in modloader/. Without it, the game starts in English.");
                    else
                        inspector.desc = tr("Ativa ou desativa automaticamente o pacote de tradução PT-BR (VCS.rec) antes de iniciar o jogo.",
                                            "Automatically enables or disables the PT-BR (VCS.rec) translation mod in vcs.ini before launch.");
                    inspector.badge = extGxt ? tr("MOD EXTERNO (.GXT)", "EXTERNAL MOD (.GXT)") : (gameLangIdx == 1 ? "PT-BR MOD" : "ENGLISH");
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcSliderRow("#vc-aud-vol", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                    tr("Volume Principal", "Master Volume"),
                                    cfg.audioVolume, 0, 100, 5, "%", &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("VOLUME PRINCIPAL DO JOGO", "MASTER GAME VOLUME");
                    inspector.desc = tr("Arraste o slider rosa para ajustar o volume geral de estações de rádio, motores, tiros e vozes.",
                                        "Drag the pink slider to adjust overall volume for radio stations, engines, gunfire and voices.");
                    inspector.badge = tr("IMPACTO: 0%", "COST: 0%");
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcSliderRow("#vc-aud-mov", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                    tr("Volume dos Vídeos (FMV)", "Video Cutscene Volume (FMV)"),
                                    cfg.audioMovieVolume, 0, 100, 5, "%", &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("VOLUME DOS VÍDEOS DE ABERTURA", "INTRO & FMV VIDEO VOLUME");
                    inspector.desc = tr("Ajusta o volume dos vídeos MPEG pré-renderizados e da abertura clássica do Vice City Stories.",
                                        "Adjusts the volume of pre-rendered MPEG videos and the classic Vice City Stories intro sequence.");
                    inspector.badge = tr("IMPACTO: 0%", "COST: 0%");
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcCycleRow("#vc-aud-out", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Modo de Saída de Áudio", "Audio Output Mode"),
                                   {tr("Estéreo", "Stereo"), "Mono", "Dolby Pro Logic II"}, cfg.audioOutput, &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("MODO DE SAÍDA DE ÁUDIO", "AUDIO OUTPUT MODE");
                    inspector.desc = tr("Define a matriz de mixagem de canais da SPU2 recompilada (Estéreo, Mono ou Surround Pro Logic II).",
                                        "Selects the channel mixing matrix for the recompiled SPU2 audio engine.");
                    inspector.badge = tr("IMPACTO: 0%", "COST: 0%");
                }
                y += rowH;
            }
            {
                bool hov = false;
                // the game's enhance.prompts values in the order shown (Nintendo Switch is the game's 5, after Keyboard)
                static const int kPromptValues[] = {0, 1, 2, 3, 5, 4};
                int promptIdx = indexInList(cfg.enhanceButtonPrompts, kPromptValues, 6, 1);
                if (ui::vcCycleRow("#vc-aud-prompts", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Ícones de Botões do Controle", "Controller Button Prompts"),
                                   {"Original (PS2)", tr("Automático", "Automatic"), "PlayStation", "Xbox", "Nintendo Switch",
                                    tr("Teclado e mouse", "Keyboard & mouse")},
                                   promptIdx, &hov))
                {
                    cfg.enhanceButtonPrompts = kPromptValues[promptIdx];
                    changed = true;
                }
                if (hov)
                {
                    inspector.title = tr("ÍCONES DE BOTÕES NA TELA", "ON-SCREEN CONTROLLER PROMPTS");
                    inspector.desc = tr("Escolhe o estilo de ícones exibidos nas dicas do HUD (Automático detecta DualSense/Xbox conectado).",
                                        "Selects the button icon style shown in HUD prompts (Auto detects connected DualSense/Xbox gamepads).");
                    inspector.badge = tr("IMPACTO: 0%", "COST: 0%");
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcCycleRow("#vc-aud-prompt-style", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Estilo dos Ícones", "Prompt Icon Style"),
                                   {"Kenney", tr("Clássico", "Classic")}, cfg.enhancePromptStyle, &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("ESTILO DOS ÍCONES", "PROMPT ICON STYLE");
                    inspector.desc = tr("Kenney: ícones modernos e coloridos para cada controle e tecla. Clássico: os botões desenhados no estilo do próprio jogo.",
                                        "Kenney: modern coloured icons for every controller and key. Classic: the buttons drawn in the game's own style.");
                    inspector.badge = tr("IMPACTO: 0%", "COST: 0%");
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcCycleRow("#vc-aud-sub-font", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Fonte das Legendas", "Subtitle Font"),
                                   {"Original", "Oswald"}, cfg.subtitleFont, &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("FONTE DAS LEGENDAS", "SUBTITLE FONT");
                    inspector.desc = tr("Oswald: legendas e caixas de ajuda numa fonte moderna, nítida em qualquer resolução, com contorno. As opções abaixo valem com ela.",
                                        "Oswald: subtitles and help boxes in a modern font, sharp at any resolution, with an outline. The options below apply with it.");
                    inspector.badge = tr("IMPACTO: 0%", "COST: 0%");
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcSliderRow("#vc-aud-sub-size", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                    tr("Tamanho das Legendas", "Subtitle Size"), cfg.subtitleSize, 80, 150, 5, "%", &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("TAMANHO DAS LEGENDAS", "SUBTITLE SIZE");
                    inspector.desc = tr("O tamanho das legendas em relação ao do jogo (com a Oswald). O jogo quebra e centraliza as linhas para ele.",
                                        "How big the subtitles are compared to the game's (with Oswald). The game wraps and centres the lines for it.");
                    inspector.badge = tr("IMPACTO: 0%", "COST: 0%");
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcSliderRow("#vc-aud-sub-outline", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                    tr("Contorno das Legendas", "Subtitle Outline"), cfg.subtitleOutline, 0, 100, 5, "%", &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("CONTORNO DAS LEGENDAS", "SUBTITLE OUTLINE");
                    inspector.desc = tr("A espessura da borda escura em volta das letras (com a Oswald). 0: sem contorno.",
                                        "How thick the dark edge round the letters is (with Oswald). 0: none.");
                    inspector.badge = tr("IMPACTO: 0%", "COST: 0%");
                }
                y += rowH;
            }
            {
                bool hov = false;
                int shadowIdx = cfg.subtitleShadow ? 0 : 1;
                if (ui::vcCycleRow("#vc-aud-sub-shadow", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Sombra das Legendas", "Subtitle Shadow"),
                                   {tr("Sim", "On"), tr("Não", "Off")}, shadowIdx, &hov))
                {
                    cfg.subtitleShadow = shadowIdx == 0 ? 1 : 0;
                    changed = true;
                }
                if (hov)
                {
                    inspector.title = tr("SOMBRA DAS LEGENDAS", "SUBTITLE SHADOW");
                    inspector.desc = tr("A sombra projetada do jogo sob as legendas (com a Oswald). Não: só o contorno.",
                                        "The game's drop shadow under the subtitles (with Oswald). Off: the outline alone.");
                    inspector.badge = tr("IMPACTO: 0%", "COST: 0%");
                }
                y += rowH;
            }
            {
                static const int kBackgrounds[] = {0, 25, 50, 75};
                int bgIdx = indexInList(cfg.subtitleBackground, kBackgrounds, 4, 0);
                bool hov = false;
                if (ui::vcCycleRow("#vc-aud-sub-bg", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Fundo das Legendas", "Subtitle Background"),
                                   {tr("Não", "Off"), tr("Claro (25%)", "Light (25%)"), tr("Médio (50%)", "Medium (50%)"),
                                    tr("Escuro (75%)", "Dark (75%)")},
                                   bgIdx, &hov))
                {
                    cfg.subtitleBackground = kBackgrounds[bgIdx];
                    changed = true;
                }
                if (hov)
                {
                    inspector.title = tr("FUNDO DAS LEGENDAS", "SUBTITLE BACKGROUND");
                    inspector.desc = tr("Uma faixa escura atrás de cada linha da legenda (com a Oswald), como nos jogos modernos: legível sobre qualquer cena.",
                                        "A dark box behind each subtitle line (with Oswald), as in modern games: readable over anything.");
                    inspector.badge = tr("IMPACTO: 0%", "COST: 0%");
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcCycleRow("#vc-aud-sub-colour", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Cor das Legendas", "Subtitle Colour"),
                                   {tr("Do jogo", "Game's"), tr("Branca", "White")}, cfg.subtitleColour, &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("COR DAS LEGENDAS", "SUBTITLE COLOUR");
                    inspector.desc = tr("Do jogo: as cores do jogo (nomes e palavras-chave destacados). Branca: todo o texto em branco (com a Oswald).",
                                        "Game's: the game's colours (names and key words highlighted). White: all of the text white (with Oswald).");
                    inspector.badge = tr("IMPACTO: 0%", "COST: 0%");
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcCycleRow("#vc-aud-driveby", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Cadência de Tiro em Veículos", "Drive-By Firing Rate"),
                                   {"PS2 Original", tr("Cadência Real da Arma", "Normal Weapon Rate")},
                                   cfg.enhanceDrivebyRate, &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("CADÊNCIA DE TIRO EM VEÍCULOS", "VEHICLE DRIVE-BY FIRING RATE");
                    inspector.desc = tr("Permite que submetralhadoras usem sua cadência real de disparo ao atirar de dentro de veículos.",
                                        "Allows SMGs to fire at their true rate of fire when shooting from inside vehicles.");
                    inspector.badge = tr("MELHORIA DE JOGABILIDADE", "GAMEPLAY ENHANCEMENT");
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcCycleRow("#vc-aud-skip", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Pular Abertura ao Iniciar", "Skip Intro Logos"),
                                   {tr("Não", "Off"), tr("Sim", "On")}, cfg.displaySkipIntro, &hov))
                    changed = true;
                if (hov)
                {
                    inspector.title = tr("PULAR VÍDEOS DE ABERTURA", "SKIP INTRO LOGOS");
                    inspector.desc = tr("Pula os logotipos iniciais e vai diretamente para a tela de carregamento ao abrir o jogo.",
                                        "Skips initial splash logos and jumps straight to the loading screen on startup.");
                    inspector.badge = tr("INICIALIZAÇÃO RÁPIDA", "FAST STARTUP");
                }
                y += rowH;
            }
            {
                static const char *kAfterVals[] = {"minimize", "close", "stay"};
                int afterIdx = app.settings.afterLaunch == "close" ? 1 : app.settings.afterLaunch == "stay" ? 2 : 0;
                bool hov = false;
                if (ui::vcCycleRow("#vc-aud-after", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                   tr("Ação do Launcher ao Iniciar o Jogo", "When Launching the Game"),
                                   {tr("Minimizar", "Minimize"), tr("Fechar", "Close"), tr("Manter Aberto", "Keep Open")},
                                   afterIdx, &hov))
                {
                    app.settings.afterLaunch = kAfterVals[afterIdx];
                    app.saveSettings();
                }
                if (hov)
                {
                    inspector.title = tr("COMPORTAMENTO DO LAUNCHER AO INICIAR", "LAUNCHER BEHAVIOR ON GAME START");
                    inspector.desc = tr("Define se a janela do VCS.rec Launcher deve minimizar, fechar ou permanecer aberta ao iniciar o jogo.",
                                        "Controls whether the VCS.rec Launcher window minimizes, closes or stays open when launching the game.");
                    inspector.badge = "LAUNCHER";
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcActionRow("#vc-aud-open-controls", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                    tr("Configurações & Layout de Controles", "Controller Layout & Settings"),
                                    tr("< Abrir Controles >", "< Open Controls >"), rgb(0x00586E), &hov))
                {
                    shell.tab = Tab::Controls;
                    shell.tabSince = ui::state.time;
                    sound::play(sound::Sfx::Nav);
                }
                if (hov)
                {
                    inspector.title = tr("MENU DEDICADO DE CONTROLES", "DEDICATED CONTROLS MENU");
                    inspector.desc = tr("Acessa os esquemas visuais para PS2, Xbox, PS4/PS5, Switch e mapeamento de botões.",
                                        "Accesses visual layouts for PS2, Xbox, PS4/PS5, Switch and key bindings.");
                    inspector.badge = tr("CONTROLES", "CONTROLS");
                }
                y += rowH;
            }
        }

        if (changed)
            app.saveGameConfig();

        fillRemainingVcStripes(tableR, y, rowH, rowIdx);
        ui::endScroll(scrollId, y);

        drawVcInspectorPanel(inspR, inspector,
                             cfg.estimatedGpuCostPercent(),
                             cfg.estimatedCpuCostPercent(),
                             cfg.estimatedStreamingLoadMb(),
                             cfg.streamingPoolCapacityMb());
    }

    // =================================================================================================================
    // TAB 4: SOBRE / ABOUT — Classic 1980s GTA: Vice City Menu Style
    // =================================================================================================================
    void drawAbout(Rect r)
    {
        App &app = *shell.app;
        launcher::GameConfig &cfg = app.gameConfig;
        if (!shell.backupsAsked && !app.busy())
        {
            shell.backupsAsked = true;
            app.startBackupList();
        }

        const Rect area = r.inset(px(38), px(10));
        const char *subTabs[3] = {
            tr("PROJETO VCS.REC", "VCS.REC PROJECT"),
            tr("DISCO & INSTALAÇÃO", "DISC & INSTALLATION"),
            "MEMORY CARD (SAVES)",
        };
        const Rect tabBar{area.x, area.y, area.w, px(46)};
        drawVcCategoryBar(tabBar, subTabs, 3, shell.aboutSubTab);

        const float inspH = px(140);
        const Rect inspR{area.x, area.bottom() - inspH, area.w, inspH};
        const Rect tableR{area.x, tabBar.bottom() + px(6), area.w, inspR.y - tabBar.bottom() - px(12)};

        const launcher::ScsKillerStatus scStatus = launcher::inspectShaderCache(app.build);

        VcOptionInspector inspector{
            tr("GRAND THEFT AUTO: VICE CITY STORIES (VCS.REC)", "GRAND THEFT AUTO: VICE CITY STORIES (VCS.REC)"),
            tr("Recompilação estática nativa PS2 MIPS R5900 (SLUS-21590) para Windows x64 com suporte a Cache de Shaders SCSKiller e Auto-Update.",
               "Static PS2 MIPS R5900 (SLUS-21590) to Windows x64 recompilation with SCSKiller Shader Cache & Auto-Update support."),
            "BUILD 1.0.0 (" + app.sourceVersion + ")",
            0,
            0,
            0.0f,
        };

        const ui::Id scrollId = ui::id("#vc-about-scroll", shell.aboutSubTab);
        ui::beginScroll(scrollId, tableR);
        float y = tableR.y;
        const float rowH = px(44);
        int rowIdx = 0;

        if (shell.aboutSubTab == 0)
        {
            ui::vcActionRow("#abt-title", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                            tr("Projeto", "Project"), "Grand Theft Auto: Vice City Stories (VCS.rec)", rgb(0x00586E));
            y += rowH;

            ui::vcActionRow("#abt-ver", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                            tr("Versão da Build", "Build Version"),
                            "Build 1.0.0 (" + app.sourceVersion + ")", rgb(0x00586E));
            y += rowH;

            {
                bool hov = false;
                if (ui::vcActionRow("#abt-autoupdate", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                    tr("Buscar Atualizações e Atualizar Build", "Check for Updates & Rebuild Project"),
                                    tr("< Buscar e Atualizar >", "< Check & Update >"), 0, &hov, !app.busy()))
                {
                    shell.tab = Tab::Game;
                    shell.buildConfirmOpen = false;
                    shell.c64TerminalOpen = true;
                    app.startAutoUpdateBuild();
                }
                if (hov)
                {
                    inspector.title = tr("ATUALIZAÇÃO AUTOMÁTICA DA BUILD", "AUTOMATIC BUILD UPDATER");
                    inspector.desc = tr("Sincroniza os commits mais recentes do repositório oficial e recompila o executável vcs.exe no terminal C64.",
                                        "Syncs the latest commits from the official repository and rebuilds vcs.exe inside the C64 terminal.");
                    inspector.badge = "AUTO-UPDATER";
                }
                y += rowH;
            }
            {
                bool hov = false;
                char cacheRight[128];
                if (scStatus.exists)
                {
                    std::snprintf(cacheRight, sizeof(cacheRight),
                                  app.isPt() ? "< Reconstruir (%u PSOs / %u DXBC) >" : "< Rebuild (%u PSOs / %u DXBC) >",
                                  scStatus.psoCount, scStatus.shaderCount);
                }
                else
                {
                    std::snprintf(cacheRight, sizeof(cacheRight), "%s",
                                  tr("< Construir Cache SCSKiller >", "< Build SCSKiller Cache >"));
                }
                if (ui::vcActionRow("#abt-scskiller", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                    tr("Cache de Shaders Pré-Compilado", "Pre-Compiled Shader Cache"),
                                    cacheRight, 0, &hov, !app.busy()))
                {
                    shell.tab = Tab::Game;
                    shell.buildConfirmOpen = false;
                    shell.c64TerminalOpen = true;
                    app.startShaderCacheBuild();
                }
                if (hov)
                {
                    inspector.title = tr("CACHE DE SHADERS PRÉ-COMPILADO", "PRE-COMPILED SHADER CACHE");
                    inspector.desc = tr("Pré-compila todas as permutações de PSO D3D12 e Shaders D3D11 diretamente no DXCache do driver da GPU antes do jogo iniciar.",
                                        "Pre-compiles all D3D12 PSO permutations and D3D11 shaders directly into your GPU driver DXCache before launching.");
                    inspector.badge = scStatus.exists ? tr("CACHE PRONTO", "CACHE READY") : tr("NÃO COMPILADO", "NOT BUILT");
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcActionRow("#abt-gh", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                    tr("Repositório Oficial no GitHub", "Official GitHub Repository"),
                                    "< github.com/willzmkv/gta-vcs-recomp >", 0, &hov))
                {
                    launcher::openUrl("https://github.com/willzmkv/gta-vcs-recomp");
                }
                if (hov)
                {
                    inspector.title = tr("REPOSITÓRIO OFICIAL DO FORK VCS.REC", "OFFICIAL VCS.REC FORK REPOSITORY");
                    inspector.desc = tr("Abre a página do projeto no GitHub com documentação técnica, código-fonte e releases.",
                                        "Opens the project GitHub page with technical documentation, source code and releases.");
                    inspector.badge = "GITHUB";
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcActionRow("#abt-steam", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                    tr("Integração com a Biblioteca Steam", "Steam Library Integration"),
                                    tr("< Adicionar à Steam >", "< Add to Steam >"), 0, &hov))
                {
                    std::string msg;
                    const bool ok = app.addToSteam(msg);
                    toast(msg, ok ? theme::kTeal : theme::kRed);
                }
                if (hov)
                {
                    inspector.title = tr("ADICIONAR VCS.REC À BIBLIOTECA STEAM", "ADD VCS.REC TO STEAM LIBRARY");
                    inspector.desc = tr("Configura automaticamente o atalho no Steam com Hero, Grid, Logo .REC e Ícone personalizados.",
                                        "Automatically configures the non-Steam shortcut with custom Hero, Grid, .REC Logo and Icon artwork.");
                    inspector.badge = "STEAM VDF";
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcActionRow("#abt-desk", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                    tr("Atalho na Área de Trabalho", "Desktop Shortcut"),
                                    tr("< Criar Atalho >", "< Create Shortcut >"), 0, &hov))
                {
                    std::string err;
                    toast(app.shortcut(err) ? tr("Atalho criado na Área de Trabalho!", "Desktop shortcut created!")
                                            : ("Erro: " + err),
                          err.empty() ? theme::kTeal : theme::kRed);
                }
                if (hov)
                {
                    inspector.title = tr("ATALHO NA ÁREA DE TRABALHO", "DESKTOP SHORTCUT");
                    inspector.desc = tr("Cria um atalho direto para o VCS.rec Launcher na sua Área de Trabalho do Windows.",
                                        "Creates a direct shortcut to the VCS.rec Launcher on your Windows Desktop.");
                    inspector.badge = "WINDOWS .LNK";
                }
                y += rowH;
            }
        }
        else if (shell.aboutSubTab == 1)
        {
            {
                bool hov = false;
                const std::string isoStatus = app.disc.ok
                                                  ? launcher::pathText(app.iso.filename()) + " (SLUS-21590)"
                                                  : tr("< Selecionar ISO >", "< Select ISO >");
                if (ui::vcActionRow("#abt-iso", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                    tr("Imagem de Disco PS2", "PS2 Disc Image"), isoStatus, 0, &hov))
                {
                    chooseIso();
                }
                if (hov)
                {
                    inspector.title = tr("IMAGEM DE DISCO ORIGINAL", "ORIGINAL DISC IMAGE");
                    inspector.desc = app.disc.ok ? launcher::pathText(app.iso)
                                                 : tr("Clique para selecionar sua ISO original de GTA: Vice City Stories (PS2).",
                                                      "Click to select your original GTA: Vice City Stories PS2 ISO image.");
                    inspector.badge = app.disc.ok ? tr("DISCO PRONTO", "DISC READY") : tr("PENDENTE", "MISSING");
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcActionRow("#abt-builddir", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                    tr("Pasta de Instalação / Build", "Installation / Build Folder"),
                                    launcher::pathText(app.build), 0, &hov))
                {
                    launcher::openFolder(app.build);
                }
                if (hov)
                {
                    inspector.title = tr("PASTA DE INSTALAÇÃO DO JOGO", "GAME INSTALLATION DIRECTORY");
                    inspector.desc = tr("Clique para abrir a pasta onde o executável vcs.exe, shaders e vcs.ini estão instalados.",
                                        "Click to open the folder where vcs.exe, shaders and vcs.ini are installed.");
                    inspector.badge = "VCS.EXE / VCS.INI";
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcActionRow("#abt-changebuild", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                    tr("Alterar Pasta de Instalação", "Change Installation Folder"),
                                    tr("< Escolher Pasta >", "< Choose Folder >"), 0, &hov, !app.busy()))
                {
                    chooseBuildDir(app.build);
                }
                if (hov)
                {
                    inspector.title = tr("ALTERAR PASTA DE INSTALAÇÃO FINAL", "CHANGE TARGET INSTALLATION FOLDER");
                    inspector.desc = tr("Seleciona outro diretório ou unidade de disco para instalar e compilar o jogo.",
                                        "Selects another directory or drive to install and build the game.");
                    inspector.badge = tr("SELECIONAR PASTA", "SELECT FOLDER");
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcActionRow("#abt-setup", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                    tr("Diagnóstico de Ferramentas de Build", "Build Tools Diagnostics"),
                                    tr("< Verificar Ambiente >", "< Check Environment >"), 0, &hov))
                {
                    openSetup(-1);
                }
                if (hov)
                {
                    inspector.title = tr("DIAGNÓSTICO DE FERRAMENTAS DE COMPILAÇÃO", "BUILD TOOLS ENVIRONMENT DIAGNOSTICS");
                    inspector.desc = tr("Verifica a presença do CMake, Ninja, Clang-CL, Windows SDK e Python necessários para compilar.",
                                        "Verifies CMake, Ninja, Clang-CL, Windows SDK and Python required for building from source.");
                    inspector.badge = "CMAKE / CLANG-CL";
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcActionRow("#abt-report", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                    tr("Relatório Técnico de Diagnóstico", "Technical Diagnostic Report"),
                                    tr("< Gerar Relatório >", "< Generate Report >"), 0, &hov, !app.busy()))
                {
                    app.startReport();
                }
                if (hov)
                {
                    inspector.title = tr("RELATÓRIO TÉCNICO DE DIAGNÓSTICO", "TECHNICAL DIAGNOSTIC REPORT");
                    inspector.desc = tr("Gera um resumo técnico completo do ambiente para suporte e resolução de problemas.",
                                        "Generates a complete technical summary of your setup for troubleshooting.");
                    inspector.badge = "DIAGNÓSTICO";
                }
                y += rowH;
            }
        }
        else
        {
            {
                bool hov = false;
                if (ui::vcActionRow("#abt-savesdir", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                    tr("Pasta do Memory Card (Saves)", "Memory Card Folder (Saves)"),
                                    tr("< Abrir Pasta >", "< Open Folder >"), 0, &hov))
                {
                    launcher::openFolder(app.savesFolder());
                }
                if (hov)
                {
                    inspector.title = tr("PASTA DO MEMORY CARD (SAVES)", "MEMORY CARD SAVES FOLDER");
                    inspector.desc = launcher::pathText(app.savesFolder());
                    inspector.badge = "SAVES PS2 / PC";
                }
                y += rowH;
            }
            {
                bool hov = false;
                if (ui::vcActionRow("#abt-backup-now", {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                    tr("Criar Novo Backup do Memory Card", "Create New Memory Card Backup"),
                                    tr("< Fazer Backup Agora >", "< Backup Now >"), 0, &hov, !app.busy()))
                {
                    app.startBackup();
                    shell.backupsAsked = false;
                }
                if (hov)
                {
                    inspector.title = tr("CRIAR BACKUP DO MEMORY CARD", "CREATE MEMORY CARD BACKUP");
                    inspector.desc = tr("Cria uma cópia de segurança datada de todos os seus slots de salvamento atuais.",
                                        "Creates a timestamped backup copy of all your current save slots.");
                    inspector.badge = "BACKUP SEGURO";
                }
                y += rowH;
            }

            for (size_t i = 0; i < app.backups.size(); ++i)
            {
                const launcher::Json &b = app.backups[i];
                const std::string name = b["name"].str();
                bool hov = false;
                if (ui::vcActionRow("#abt-restore-" + name, {tableR.x, y, tableR.w, rowH}, rowIdx++,
                                    "Backup: " + name,
                                    tr("< Restaurar >", "< Restore >"), 0, &hov, !app.busy() && !app.gameRunning))
                {
                    shell.restoreName = name;
                    openDialog(Dialog::Restore);
                }
                if (hov)
                {
                    inspector.title = "BACKUP: " + name;
                    inspector.desc = b["saves"].str();
                    inspector.badge = tr("RESTAURAR SAVE", "RESTORE SAVE");
                }
                y += rowH;
            }
        }

        fillRemainingVcStripes(tableR, y, rowH, rowIdx);
        ui::endScroll(scrollId, y);

        char cacheSummary[128];
        if (scStatus.exists)
            std::snprintf(cacheSummary, sizeof(cacheSummary), "Shader Cache: %u PSOs OK", scStatus.psoCount);
        else
            std::snprintf(cacheSummary, sizeof(cacheSummary), "%s",
                          tr("Shader Cache: Não compilado", "Shader Cache: Not built"));

        drawVcInspectorPanel(inspR, inspector,
                             cfg.estimatedGpuCostPercent(),
                             cfg.estimatedCpuCostPercent(),
                             cfg.estimatedStreamingLoadMb(),
                             cfg.streamingPoolCapacityMb(),
                             cacheSummary);
    }

    // =================================================================================================================
    // Legacy slide-over sheets (kept for Setup / Folder / CLI --screen sheet:...)
    // =================================================================================================================
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
        core::cutTop(area, px(56));
        const Layer previous = setLayer(Layer::Overlay);
        ui::beginLayer(kLayerSheet, shell.sheet != Sheet::None);

        rect(area, withAlpha(theme::kScrim, alphaOf(theme::kScrim) * core::clamp01(open)));
        const ui::Interaction scrim = ui::interact(ui::id("#scrim"), area, false);
        if (scrim.clicked)
            shell.sheet = Sheet::None;

        const float width = std::min(px(520), area.w * 0.9f);
        const Rect panel{area.right() - width * open, area.y, width, area.h};
        ui::blockInput(panel);
        shadow(panel.offset(-px(6), 0), theme::shade(0.8f * core::clamp01(open)), px(24), 0);
        rectEx(panel, theme::kBase, mix(theme::kBase, theme::kNight, 0.5f), {px(14), 0, 0, 0}, theme::ink(0.06f), ui::hairline());

        Rect inner = panel.inset(px(28), 0);
        float y = panel.y + px(24);
        drawText(kFontSemibold, px(22), inner.x, y + metrics(kFontSemibold, px(22)).ascent, theme::kText,
                 tr("Pasta de Build", "Build Folder"));
        if (ui::iconButton("#closesheet", theme::icon::kClose, {panel.right() - px(52), y - px(2), px(34), px(34)},
                           tr("Fechar (Esc)", "Close (Esc)"), true, px(12)))
            shell.sheet = Sheet::None;
        y += px(44);

        const Rect view{panel.x, y, panel.w, panel.bottom() - y};
        const ui::Id scroll = ui::id("#sheet-scroll", int(shell.sheetShown));
        ui::beginScroll(scroll, view);
        const Rect content{inner.x, y + px(4), inner.w, 0};
        float bottom = y + px(4);
        drawFolderBody(content, bottom);
        (void)app;
        ui::endScroll(scroll, bottom + px(24));
        ui::endLayer();
        setLayer(previous);
    }
}
