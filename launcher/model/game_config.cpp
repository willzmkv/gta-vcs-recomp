#include "game_config.h"

#include "os.h"
#include "system.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace launcher
{
    namespace
    {
        std::string trim(std::string s)
        {
            while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t'))
                s.pop_back();
            size_t i = 0;
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
                ++i;
            return s.substr(i);
        }

        std::string toLowerAscii(std::string_view s)
        {
            std::string out;
            out.reserve(s.size());
            for (unsigned char c : s)
                out.push_back(char(std::tolower(c)));
            return out;
        }

        bool iequals(std::string_view a, std::string_view b)
        {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                    return false;
            return true;
        }

        int parseInt(const std::string &s, int fallback)
        {
            if (s.empty())
                return fallback;
            char *end = nullptr;
            const long v = std::strtol(s.c_str(), &end, 10);
            return (end && end != s.c_str()) ? int(v) : fallback;
        }
    }

    std::string GameConfig::normalizeModKey(std::string_view folderName)
    {
        std::string out;
        out.reserve(folderName.size());
        for (unsigned char c : folderName)
        {
            if (std::isalnum(c))
                out.push_back(char(std::tolower(c)));
            else
                out.push_back('_');
        }
        return out.empty() ? "mod" : out;
    }

    bool GameConfig::loadIni(const fs::path &iniPath)
    {
        graphicsConfigured = false;
        explicitModKeys.clear();
        modStates.clear();
        bool promptsSeen = false; // enhance.prompts read: the old enhance.button_prompts no longer counts
        std::ifstream in(iniPath);
        if (!in)
            return false;
        std::string line;
        while (std::getline(in, line))
        {
            line = trim(line);
            if (line.empty() || line[0] == '#' || line[0] == ';')
                continue;
            const size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            const std::string key = trim(line.substr(0, eq));
            const std::string val = trim(line.substr(eq + 1));
            if (key == "display.scale" || key == "display.msaa" || key == "world.hd_radius" || key == "world.view_distance")
                graphicsConfigured = true;

            if (key == "display.window") displayWindow = std::clamp(parseInt(val, displayWindow), 0, 1);
            else if (key == "display.aspect") displayAspect = parseInt(val, displayAspect);
            else if (key == "display.hud") displayHud = std::clamp(parseInt(val, displayHud), 0, 2);
            else if (key == "display.cutscene_bars") displayCutsceneBars = std::clamp(parseInt(val, displayCutsceneBars), 0, 1);
            else if (key == "display.timing") displayTiming = std::clamp(parseInt(val, displayTiming), 0, 1);
            else if (key == "display.fps") displayFps = parseInt(val, displayFps);
            else if (key == "display.vsync") displayVsync = std::clamp(parseInt(val, displayVsync), 0, 2);
            else if (key == "display.fps_counter") displayFpsCounter = std::clamp(parseInt(val, displayFpsCounter), 0, 4);
            else if (key == "display.renderer")
            {
                // UI index: 0 = D3D11, 1 = D3D12, 2 = Vulkan; vcs.ini stores rhi::Backend (0 = D3D11, 1 = Vulkan, 3 = D3D12).
                const int raw = parseInt(val, -1);
                if (raw == 0)
                    displayRenderer = 0;
                else if (raw == 3)
                    displayRenderer = 1;
                else if (raw == 1)
                    displayRenderer = 2;
            }
            else if (key == "display.scale") displayScale = std::clamp(parseInt(val, displayScale), 0, 8);
            else if (key == "display.msaa") displayMsaa = std::clamp(parseInt(val, displayMsaa), 1, 8);
            else if (key == "display.aniso") displayAniso = std::clamp(parseInt(val, displayAniso), 0, 16);
            else if (key == "graphics.post_process") graphicsPostProcess = std::clamp(parseInt(val, graphicsPostProcess), 0, 1);
            else if (key == "graphics.exposure") graphicsExposure = std::clamp(parseInt(val, graphicsExposure), 0, 200);
            else if (key == "world.hd_radius") worldHdRadius = std::clamp(parseInt(val, worldHdRadius), 0, 3);
            else if (key == "world.view_distance") worldViewDistance = std::clamp(parseInt(val, worldViewDistance), 0, 100);
            else if (key == "world.detail_distance") worldDetailDistance = std::clamp(parseInt(val, worldDetailDistance), 0, 100);
            else if (key == "fog.distance") fogDistance = std::clamp(parseInt(val, fogDistance), 10, 100);
            else if (key == "fog.start") fogStart = std::clamp(parseInt(val, fogStart), 0, 300);
            else if (key == "memory.streaming") memoryStreaming = parseInt(val, memoryStreaming);
            else if (key == "world.population_distance") worldPopulationDistance = std::clamp(parseInt(val, worldPopulationDistance), 0, 100);
            else if (key == "world.vehicle_density") worldVehicleDensity = std::clamp(parseInt(val, worldVehicleDensity), 0, 100);
            else if (key == "world.pedestrian_density") worldPedestrianDensity = std::clamp(parseInt(val, worldPedestrianDensity), 0, 100);
            else if (key == "world.crowd_bands") worldCrowdBands = std::clamp(parseInt(val, worldCrowdBands), 0, 1);
            else if (key == "world.fps_target") worldFpsTarget = parseInt(val, worldFpsTarget);
            else if (key == "world.distant_traffic") worldDistantTraffic = std::clamp(parseInt(val, worldDistantTraffic), 0, 100);
            else if (key == "world.distant_crowds") worldDistantCrowds = std::clamp(parseInt(val, worldDistantCrowds), 0, 100);
            else if (key == "audio.volume") audioVolume = std::clamp(parseInt(val, audioVolume), 0, 100);
            else if (key == "audio.movie_volume") audioMovieVolume = std::clamp(parseInt(val, audioMovieVolume), 0, 100);
            else if (key == "audio.output") audioOutput = std::clamp(parseInt(val, audioOutput), 0, 2);
            else if (key == "display.skip_intro") displaySkipIntro = std::clamp(parseInt(val, displaySkipIntro), 0, 1);
            else if (key == "game.background") gameBackground = std::clamp(parseInt(val, gameBackground), 0, 2);
            else if (key == "enhance.prompts")
            {
                enhanceButtonPrompts = std::clamp(parseInt(val, enhanceButtonPrompts), 0, 5);
                promptsSeen = true;
            }
            else if (key == "enhance.button_prompts" && !promptsSeen)
            {
                // the launcher's own key before 2026-11-17, numbered Auto / PS2 / PS4-PS5 / Xbox / Switch: the game's
                // enhance.prompts (Original / Automatic / PlayStation / Xbox / Keyboard) wins when both are there
                static const int kFromOld[] = {1, 0, 2, 3, 5};
                enhanceButtonPrompts = kFromOld[std::clamp(parseInt(val, 1), 0, 4)];
            }
            else if (key == "enhance.prompt_style") enhancePromptStyle = std::clamp(parseInt(val, enhancePromptStyle), 0, 1);
            else if (key == "enhance.subtitle_font") subtitleFont = std::clamp(parseInt(val, subtitleFont), 0, 1);
            else if (key == "enhance.subtitle_outline") subtitleOutline = std::clamp(parseInt(val, subtitleOutline), 0, 100);
            else if (key == "enhance.subtitle_size") subtitleSize = std::clamp(parseInt(val, subtitleSize), 80, 150);
            else if (key == "enhance.subtitle_shadow") subtitleShadow = std::clamp(parseInt(val, subtitleShadow), 0, 1);
            else if (key == "enhance.subtitle_background") subtitleBackground = std::clamp(parseInt(val, subtitleBackground), 0, 75);
            else if (key == "enhance.subtitle_colour") subtitleColour = std::clamp(parseInt(val, subtitleColour), 0, 1);
            else if (key == "enhance.driveby_rate") enhanceDrivebyRate = std::clamp(parseInt(val, enhanceDrivebyRate), 0, 1);
            else if (key == "pad.preset") padPreset = std::clamp(parseInt(val, padPreset), 0, 2);
            else if (key == "pad.trigger_drive") padTriggerDrive = std::clamp(parseInt(val, padTriggerDrive), 0, 1);
            else if (key == "pad.trigger_shoot") padTriggerShoot = std::clamp(parseInt(val, padTriggerShoot), 0, 1);
            else if (key == "pad.free_aim") padFreeAim = std::clamp(parseInt(val, padFreeAim), 0, 1);
            else if (key == "pad.aim_assist") padAimAssist = std::clamp(parseInt(val, padAimAssist), 0, 1);
            else if (key == "pad.deadzone_left" || key == "pad.deadzone") padDeadzone = std::clamp(parseInt(val, padDeadzone), 0, 40);
            else if (key == "pad.camera_speed") padCameraSpeed = std::clamp(parseInt(val, padCameraSpeed), 10, 400);
            else if (key == "pad.vibration") padVibration = std::clamp(parseInt(val, padVibration), 0, 200);
            else if (key == "pad.invert_y") padInvertY = std::clamp(parseInt(val, padInvertY), 0, 1);
            else if (key == "pad.ds_triggers" || key == "dualsense.triggers") dualsenseTriggers = std::clamp(parseInt(val, dualsenseTriggers), 0, 1);
            else if (key == "pad.ds_lightbar" || key == "dualsense.lightbar") dualsenseLightbar = std::clamp(parseInt(val, dualsenseLightbar), 0, 2);
            else if (key == "pad.ds_police" || key == "dualsense.police_flash") dualsensePoliceFlash = std::clamp(parseInt(val, dualsensePoliceFlash), 0, 1);
            else if (key == "pad.ds_speaker" || key == "dualsense.speaker") dualsenseSpeaker = std::clamp(parseInt(val, dualsenseSpeaker), 0, 2);
            else if (key == "mouse.camera") mouseCamera = std::clamp(parseInt(val, mouseCamera), 0, 1);
            else if (key == "mouse.sensitivity") mouseSensitivity = std::clamp(parseInt(val, mouseSensitivity), 10, 400);
            else if (key == "mouse.car_sensitivity") mouseCarSensitivity = std::clamp(parseInt(val, mouseCarSensitivity), 10, 400);
            else if (key == "mouse.car_return")
            {
                const int v = parseInt(val, mouseCarReturn);
                mouseCarReturn = (v < 0) ? -1 : std::clamp(v, 5, 50);
            }
            else if (key == "mouse.free_aim") mouseFreeAim = std::clamp(parseInt(val, mouseFreeAim), 0, 1);
            else if (key == "mouse.invert_y") mouseInvertY = std::clamp(parseInt(val, mouseInvertY), 0, 1);
            else if (key == "mouse.wheel") mouseWheel = std::clamp(parseInt(val, mouseWheel), 0, 1);
            else if (key == "mods.enabled") modsEnabled = std::clamp(parseInt(val, modsEnabled), 0, 1);
            else if (key == "graphics.texture_replacements") graphicsTextureReplacements = std::clamp(parseInt(val, graphicsTextureReplacements), 0, 1);
            else if (key.rfind("mods.", 0) == 0)
            {
                explicitModKeys.insert(key);
                modStates[key] = parseInt(val, 1) ? 1 : 0;
            }
        }
        for (ModEntry &m : mods)
            if (auto it = modStates.find(m.key); it != modStates.end())
                m.enabled = it->second != 0;
        return true;
    }

    bool GameConfig::saveIni(const fs::path &iniPath) const
    {
        std::vector<std::pair<std::string, std::string>> known = {
            {"display.window", std::to_string(displayWindow)},
            {"display.aspect", std::to_string(displayAspect)},
            {"display.hud", std::to_string(displayHud)},
            {"display.cutscene_bars", std::to_string(displayCutsceneBars)},
            {"display.timing", std::to_string(displayTiming)},
            {"display.fps", std::to_string(displayFps)},
            {"display.vsync", std::to_string(displayVsync)},
            {"display.fps_counter", std::to_string(displayFpsCounter)},
            {"display.renderer", std::to_string(displayRenderer == 1 ? 3 : displayRenderer == 2 ? 1 : 0)},
            {"display.scale", std::to_string(displayScale)},
            {"display.msaa", std::to_string(displayMsaa)},
            {"display.aniso", std::to_string(displayAniso)},
            {"graphics.post_process", std::to_string(graphicsPostProcess)},
            {"graphics.exposure", std::to_string(graphicsExposure)},
            {"world.hd_radius", std::to_string(worldHdRadius)},
            {"world.view_distance", std::to_string(worldViewDistance)},
            {"world.detail_distance", std::to_string(worldDetailDistance)},
            {"fog.distance", std::to_string(fogDistance)},
            {"fog.start", std::to_string(fogStart)},
            {"memory.streaming", std::to_string(memoryStreaming)},
            {"world.population_distance", std::to_string(worldPopulationDistance)},
            {"world.vehicle_density", std::to_string(worldVehicleDensity)},
            {"world.pedestrian_density", std::to_string(worldPedestrianDensity)},
            {"world.crowd_bands", std::to_string(worldCrowdBands)},
            {"world.fps_target", std::to_string(worldFpsTarget)},
            {"world.distant_traffic", std::to_string(worldDistantTraffic)},
            {"world.distant_crowds", std::to_string(worldDistantCrowds)},
            {"audio.volume", std::to_string(audioVolume)},
            {"audio.movie_volume", std::to_string(audioMovieVolume)},
            {"audio.output", std::to_string(audioOutput)},
            {"display.skip_intro", std::to_string(displaySkipIntro)},
            {"game.background", std::to_string(gameBackground)},
            {"enhance.prompts", std::to_string(enhanceButtonPrompts)},
            {"enhance.prompt_style", std::to_string(enhancePromptStyle)},
            {"enhance.subtitle_font", std::to_string(subtitleFont)},
            {"enhance.subtitle_outline", std::to_string(subtitleOutline)},
            {"enhance.subtitle_size", std::to_string(subtitleSize)},
            {"enhance.subtitle_shadow", std::to_string(subtitleShadow)},
            {"enhance.subtitle_background", std::to_string(subtitleBackground)},
            {"enhance.subtitle_colour", std::to_string(subtitleColour)},
            {"enhance.driveby_rate", std::to_string(enhanceDrivebyRate)},
            {"pad.preset", std::to_string(padPreset)},
            {"pad.trigger_drive", std::to_string(padTriggerDrive)},
            {"pad.trigger_shoot", std::to_string(padTriggerShoot)},
            {"pad.free_aim", std::to_string(padFreeAim)},
            {"pad.aim_assist", std::to_string(padAimAssist)},
            {"pad.deadzone_left", std::to_string(padDeadzone)},
            {"pad.deadzone_right", std::to_string(padDeadzone)},
            {"pad.camera_speed", std::to_string(padCameraSpeed)},
            {"pad.vibration", std::to_string(padVibration)},
            {"pad.invert_y", std::to_string(padInvertY)},
            {"pad.ds_triggers", std::to_string(dualsenseTriggers)},
            {"pad.ds_lightbar", std::to_string(dualsenseLightbar)},
            {"pad.ds_police", std::to_string(dualsensePoliceFlash)},
            {"pad.ds_speaker", std::to_string(dualsenseSpeaker)},
            {"mouse.camera", std::to_string(mouseCamera)},
            {"mouse.sensitivity", std::to_string(mouseSensitivity)},
            {"mouse.car_sensitivity", std::to_string(mouseCarSensitivity)},
            {"mouse.car_return", std::to_string(mouseCarReturn)},
            {"mouse.free_aim", std::to_string(mouseFreeAim)},
            {"mouse.invert_y", std::to_string(mouseInvertY)},
            {"mouse.wheel", std::to_string(mouseWheel)},
            {"mods.enabled", std::to_string(modsEnabled)},
            {"graphics.texture_replacements", std::to_string(graphicsTextureReplacements)},
        };
        for (const ModEntry &m : mods)
            known.push_back({m.key, m.enabled ? "1" : "0"});
        for (const auto &[k, v] : modStates)
        {
            bool inMods = false;
            for (const ModEntry &m : mods)
                if (m.key == k)
                    inMods = true;
            if (!inMods)
                known.push_back({k, v ? "1" : "0"});
        }

        std::map<std::string, std::string> lookup;
        for (const auto &[k, v] : known)
            lookup[k] = v;

        std::vector<std::string> lines;
        std::set<std::string> written;
        {
            std::ifstream in(iniPath);
            std::string raw;
            while (std::getline(in, raw))
            {
                if (!raw.empty() && raw.back() == '\r')
                    raw.pop_back();
                const std::string t = trim(raw);
                const size_t eq = t.find('=');
                if (!t.empty() && t[0] != '#' && t[0] != ';' && eq != std::string::npos)
                {
                    const std::string key = trim(t.substr(0, eq));
                    if (auto it = lookup.find(key); it != lookup.end())
                    {
                        lines.push_back(key + "=" + it->second);
                        written.insert(key);
                        continue;
                    }
                }
                lines.push_back(raw);
            }
        }
        for (const auto &[k, v] : known)
            if (!written.count(k))
                lines.push_back(k + "=" + v);

        std::error_code ec;
        if (iniPath.has_parent_path())
            fs::create_directories(iniPath.parent_path(), ec);
        std::ofstream out(iniPath, std::ios::trunc | std::ios::binary);
        if (!out)
            return false;
        for (const std::string &l : lines)
            out << l << "\n";
        return bool(out);
    }

    void GameConfig::applyPreset(GraphicsPreset preset)
    {
        switch (preset)
        {
        case GraphicsPreset::OriginalPS2:
            displayScale = 1; // 1x PS2 / PSP nativo (480p)
            displayMsaa = 1;  // Sem MSAA
            displayAniso = 0; // Sem filtro anisotrópico
            graphicsPostProcess = 1;
            worldHdRadius = 0; // ~260m
            worldViewDistance = 0;
            worldDetailDistance = 0;
            fogDistance = 20;
            fogStart = 0;
            memoryStreaming = 0; // 5.4 MB pool PS2 original
            worldPopulationDistance = 0;
            worldVehicleDensity = 0;
            worldPedestrianDensity = 0;
            worldCrowdBands = 0;
            worldDistantTraffic = 0;
            worldDistantCrowds = 0;
            break;
        case GraphicsPreset::Low:
            displayScale = 1; // 1x Nativo (sem supersampling)
            displayMsaa = 2;  // 2x MSAA suave
            displayAniso = 4; // 4x Anisotrópico
            graphicsPostProcess = 1;
            worldHdRadius = 1; // ~450m
            worldViewDistance = 25;
            worldDetailDistance = 20;
            fogDistance = 35;
            fogStart = 30;
            memoryStreaming = 12; // 12 MB
            worldPopulationDistance = 20;
            worldVehicleDensity = 15;
            worldPedestrianDensity = 15;
            worldCrowdBands = 0;
            worldDistantTraffic = 20;
            worldDistantCrowds = 15;
            break;
        case GraphicsPreset::Medium:
            displayScale = 0; // Match window (1080p nativo)
            displayMsaa = 4;  // 4x MSAA
            displayAniso = 8; // 8x Anisotrópico
            graphicsPostProcess = 1;
            worldHdRadius = 1; // ~450m
            worldViewDistance = 45;
            worldDetailDistance = 40;
            fogDistance = 50;
            fogStart = 60;
            memoryStreaming = 20; // 20 MB
            worldPopulationDistance = 40;
            worldVehicleDensity = 30;
            worldPedestrianDensity = 30;
            worldCrowdBands = 1;
            worldDistantTraffic = 40;
            worldDistantCrowds = 35;
            break;
        case GraphicsPreset::High:
            displayScale = 2; // 2x Scale (~1440p supersampling)
            displayMsaa = 4;  // 4x MSAA
            displayAniso = 16; // 16x Anisotrópico
            graphicsPostProcess = 1;
            worldHdRadius = 2; // ~580m
            worldViewDistance = 75;
            worldDetailDistance = 70;
            fogDistance = 75;
            fogStart = 120;
            memoryStreaming = 30; // 30 MB
            worldPopulationDistance = 65;
            worldVehicleDensity = 55;
            worldPedestrianDensity = 55;
            worldCrowdBands = 1;
            worldDistantTraffic = 65;
            worldDistantCrowds = 60;
            break;
        case GraphicsPreset::ViceUltra:
            displayScale = 4; // 4x Scale (~4K UHD supersampling)
            displayMsaa = 8;  // 8x MSAA
            displayAniso = 16; // 16x Anisotrópico máximo
            graphicsPostProcess = 1;
            worldHdRadius = 3; // ~720m (Mundo HD completo)
            worldViewDistance = 100;
            worldDetailDistance = 100;
            fogDistance = 100;
            fogStart = 180;
            memoryStreaming = 30; // 30 MB
            worldPopulationDistance = 85;
            worldVehicleDensity = 75;
            worldPedestrianDensity = 75;
            worldCrowdBands = 1;
            worldDistantTraffic = 80;
            worldDistantCrowds = 80;
            break;
        case GraphicsPreset::Custom:
        default:
            break;
        }
    }

    GraphicsPreset GameConfig::detectPreset() const
    {
        if (displayScale == 1 && displayMsaa == 1 && displayAniso == 0 && worldHdRadius == 0 && worldViewDistance == 0 && memoryStreaming == 0)
            return GraphicsPreset::OriginalPS2;
        if (displayScale == 1 && displayMsaa == 2 && displayAniso == 4 && worldHdRadius == 1 && worldViewDistance == 25 && memoryStreaming == 12)
            return GraphicsPreset::Low;
        if (displayScale == 0 && displayMsaa == 4 && displayAniso == 8 && worldHdRadius == 1 && worldViewDistance == 45 && memoryStreaming == 20)
            return GraphicsPreset::Medium;
        if (displayScale == 2 && displayMsaa == 4 && displayAniso == 16 && worldHdRadius == 2 && worldViewDistance == 75 && memoryStreaming == 30)
            return GraphicsPreset::High;
        if (displayScale == 4 && displayMsaa == 8 && displayAniso == 16 && worldHdRadius == 3 && worldViewDistance == 100 && memoryStreaming == 30)
            return GraphicsPreset::ViceUltra;
        return GraphicsPreset::Custom;
    }

    bool GameConfig::hasTextureMods() const
    {
        for (const ModEntry &m : mods)
        {
            if (m.hasTextures)
                return true;
        }
        return false;
    }

    void GameConfig::applyPadPreset(int preset)
    {
        padPreset = preset;
        if (preset == 1) // Original PS2
        {
            padTriggerDrive = 0;
            padTriggerShoot = 0;
            padFreeAim = 0;
            padAimAssist = 1;
        }
        else if (preset == 2) // Modern GTA IV / V
        {
            padTriggerDrive = 1;
            padTriggerShoot = 1;
            padFreeAim = 1;
            padAimAssist = 1;
        }
    }

    int GameConfig::estimatedGpuCostPercent() const
    {
        int score = 12; // Base PS2 1x cost
        switch (displayScale)
        {
        case 1: score += 0; break;
        case 2: score += 8; break;
        case 0: score += 16; break;
        case 3: score += 16; break;
        case 4: score += 28; break;
        case 6: score += 45; break;
        case 8: score += 62; break;
        default: score += 16; break;
        }
        if (displayMsaa == 2) score += 4;
        else if (displayMsaa == 4) score += 9;
        else if (displayMsaa >= 8) score += 18;
        if (displayAniso >= 8) score += 2;
        if (graphicsPostProcess) score += 4;
        score += worldHdRadius * 4;
        score += (worldViewDistance + worldDetailDistance) / 20;
        return std::clamp(score, 5, 100);
    }

    int GameConfig::estimatedCpuCostPercent() const
    {
        int score = 15; // Base recompiled PS2 MIPS + GS submission
        score += worldHdRadius * 11;
        score += worldViewDistance / 8;
        score += worldDetailDistance / 10;
        score += (worldPopulationDistance + worldVehicleDensity + worldPedestrianDensity) / 15;
        score += (worldDistantTraffic + worldDistantCrowds) / 25;
        if (worldCrowdBands) score += 3;
        return std::clamp(score, 10, 100);
    }

    float GameConfig::estimatedStreamingLoadMb() const
    {
        // PS2 baseline streaming memory load: 5.4 MB for models and world
        float load = 5.4f;
        if (worldHdRadius == 1)
            load += 2.2f;
        else if (worldHdRadius == 2)
            load += 4.4f;
        else if (worldHdRadius >= 3)
            load += 6.6f;

        if (modsEnabled)
        {
            if (graphicsTextureReplacements)
                load += 2.0f;

            for (const auto &m : mods)
            {
                if (m.enabled && m.compat != ModCompat::Incompatible)
                {
                    float cost = 1.2f;
                    if (m.folderName.find("Texture") != std::string::npos || m.summary.find("Textura") != std::string::npos ||
                        m.folderName.find("HD") != std::string::npos)
                        cost = 2.6f;
                    else if (m.folderName.find("Vehicle") != std::string::npos || m.folderName.find("Car") != std::string::npos ||
                             m.summary.find("Veículo") != std::string::npos)
                        cost = 1.8f;
                    load += cost;
                }
            }
        }
        return load;
    }

    float GameConfig::streamingPoolCapacityMb() const
    {
        if (memoryStreaming > 0)
            return float(memoryStreaming);

        // Automatic pool sizing: ensures streaming heap always exceeds active mod load
        const float load = estimatedStreamingLoadMb();
        if (load > 20.0f)
            return 30.0f;
        if (load > 12.0f)
            return 20.0f;
        return 12.0f;
    }

    float GameConfig::estimatedStreamingMb() const
    {
        return estimatedStreamingLoadMb();
    }

    void GameConfig::recalculateStreamingMemory()
    {
        const float load = estimatedStreamingLoadMb();
        if (memoryStreaming > 0)
        {
            // If user explicitly configured a pool that is too small for active mods, scale up
            if (load > float(memoryStreaming))
            {
                if (load > 20.0f)
                    memoryStreaming = 30;
                else if (load > 12.0f)
                    memoryStreaming = 20;
            }
        }
        else
        {
            // Automatic mode: pick optimal pool
            if (load > 20.0f)
                memoryStreaming = 30;
            else if (load > 12.0f)
                memoryStreaming = 20;
            else
                memoryStreaming = 12;
        }
    }

    ModEntry GameConfig::classifyModFolder(const fs::path &modDir)
    {
        ModEntry entry;
        entry.folderName = pathText(modDir.filename());
        entry.key = "mods." + normalizeModKey(entry.folderName);

        const std::string lowerModName = toLowerAscii(entry.folderName);
        if (lowerModName.find("texture") != std::string::npos || lowerModName.find("textura") != std::string::npos)
            entry.hasTextures = true;

        static const std::set<std::string> kCompatibleExts = {
            ".chk", ".xtx", ".gxt", ".dat", ".cfg", ".ped", ".ide", ".ipl", ".zon", ".cut",
            ".dir", ".img", ".col", ".ifp", ".txd", ".dff", ".fxt", ".scm", ".adf", ".vag",
            ".nam", ".sfx", ".raw", ".sdt", ".wav", ".ogg", ".mp3", ".png", ".dds", ".p2s",
            ".prx", ".ovl", ".rel", ".vcs", ".mdl", ".psp"
        };
        static const std::set<std::string> kIncompatibleCodeExts = {
            ".asi", ".dll", ".cleo", ".cs", ".csa", ".csi", ".flt", ".sf", ".elf", ".so"
        };
        static const std::set<std::string> kDocExts = {
            ".txt", ".md", ".ini", ".json", ".jpg", ".jpeg", ".bmp", ".url", ".pdf"
        };

        int codeFiles = 0;
        int unknownFiles = 0;
        std::string firstCodeExt;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(modDir, ec), end; it != end && !ec; it.increment(ec))
        {
            if (!it->is_regular_file(ec))
                continue;
            const std::string ext = toLowerAscii(pathText(it->path().extension()));
            if (kCompatibleExts.count(ext))
            {
                ++entry.compatibleCount;
                if (ext == ".gxt")
                    entry.hasGxt = true;
            }
            else if (kIncompatibleCodeExts.count(ext))
            {
                ++codeFiles;
                ++entry.unsupportedCount;
                if (firstCodeExt.empty())
                    firstCodeExt = ext;
            }
            else if (!kDocExts.count(ext) && !ext.empty())
            {
                ++unknownFiles;
                ++entry.unsupportedCount;
            }
        }

        if (codeFiles > 0 && entry.compatibleCount == 0)
        {
            entry.compat = ModCompat::Incompatible;
            entry.summary = "Incompatível • Plugin nativo (" + firstCodeExt + ")";
            entry.detail = "Contém código compilado para GTA III/VC/SA de PC (" + firstCodeExt +
                           ") que não pode ser executado na engine recompilada do VCS.rec.";
        }
        else if (codeFiles > 0 && entry.compatibleCount > 0)
        {
            entry.compat = ModCompat::Partial;
            entry.summary = "Parcial • " + std::to_string(entry.compatibleCount) + " assets + plugin ignorado (" + firstCodeExt + ")";
            entry.detail = "Os " + std::to_string(entry.compatibleCount) +
                           " arquivos de dados/texturas serão carregados, mas scripts/DLLs (" + firstCodeExt + ") serão ignorados.";
        }
        else if (entry.compatibleCount > 0 && unknownFiles > 0)
        {
            entry.compat = ModCompat::Partial;
            entry.summary = "Parcial • " + std::to_string(entry.compatibleCount) + " suportados, " +
                            std::to_string(unknownFiles) + " ignorados";
            entry.detail = "A maioria dos arquivos é compatível com o Mod Loader do VCS.rec, mas há arquivos extras ignorados.";
        }
        else if (entry.compatibleCount > 0)
        {
            entry.compat = ModCompat::Compatible;
            entry.summary = "Compatível • " + std::to_string(entry.compatibleCount) +
                            (entry.compatibleCount == 1 ? " arquivo suportado" : " arquivos suportados");
            entry.detail = "100% compatível com o sistema de substituição de arquivos e texturas HD do VCS.rec.";
        }
        else
        {
            entry.compat = ModCompat::Partial;
            entry.summary = "Pasta vazia • Aguardando arquivos do mod";
            entry.detail = "Coloque arquivos .chk, .xtx, .gxt, .dat, .cfg, .img ou texturas .png/.dds dentro desta pasta.";
        }
        return entry;
    }

    fs::path GameConfig::modloaderDir(const fs::path &rootDir, const fs::path &buildDir)
    {
        fs::path dir = rootDir / "modloader";
        std::error_code ec;
        if (!fs::is_directory(dir, ec) && !buildDir.empty() && fs::is_directory(buildDir / "modloader", ec))
            dir = buildDir / "modloader";
        return dir;
    }

    void GameConfig::scanMods(const fs::path &rootDir, const fs::path &buildDir)
    {
        mods.clear();
        const fs::path dir = modloaderDir(rootDir, buildDir);
        std::error_code ec;
        if (!fs::is_directory(dir, ec))
            fs::create_directories(dir, ec);

        // Read priorities from modloader/modloader.ini ([Profiles.Default.Priority])
        std::map<std::string, int> priorities;
        {
            std::ifstream in(dir / "modloader.ini");
            std::string line, section;
            while (std::getline(in, line))
            {
                line = trim(line);
                if (line.empty() || line[0] == '#' || line[0] == ';')
                    continue;
                if (line.front() == '[' && line.back() == ']')
                {
                    section = toLowerAscii(trim(line.substr(1, line.size() - 2)));
                    continue;
                }
                const size_t eq = line.find('=');
                if (eq != std::string::npos && section.find("priority") != std::string::npos)
                {
                    const std::string k = toLowerAscii(trim(line.substr(0, eq)));
                    const int p = std::clamp(parseInt(trim(line.substr(eq + 1)), 50), 1, 100);
                    priorities[k] = p;
                }
            }
        }

        for (fs::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec))
        {
            if (!it->is_directory(ec))
                continue;
            const std::string name = pathText(it->path().filename());
            if (name.empty() || name[0] == '.')
                continue;
            ModEntry entry = classifyModFolder(it->path());
            if (auto pit = priorities.find(toLowerAscii(name)); pit != priorities.end())
                entry.priority = pit->second;
            if (auto sit = modStates.find(entry.key); sit != modStates.end())
                entry.enabled = sit->second != 0;
            else
                entry.enabled = entry.compat != ModCompat::Incompatible && entry.key != "mods.pt_br__vcs_rec_";
            modStates[entry.key] = entry.enabled ? 1 : 0;
            mods.push_back(std::move(entry));
        }

        std::sort(mods.begin(), mods.end(), [](const ModEntry &a, const ModEntry &b) {
            if (a.priority != b.priority)
                return a.priority > b.priority;
            return toLowerAscii(a.folderName) < toLowerAscii(b.folderName);
        });
    }

    bool GameConfig::hasExternalGxtMod() const
    {
        for (const ModEntry &m : mods)
            if (m.enabled && m.hasGxt && m.key != "mods.pt_br__vcs_rec_")
                return true;
        return false;
    }

    void GameConfig::setModEnabledByKey(const std::string &key, bool enabled)
    {
        modStates[key] = enabled ? 1 : 0;
        explicitModKeys.insert(key);
        for (ModEntry &m : mods)
            if (m.key == key)
                m.enabled = enabled;
    }

    bool GameConfig::modEnabledByKey(const std::string &key, bool fallback) const
    {
        for (const ModEntry &m : mods)
            if (m.key == key)
                return m.enabled;
        if (auto it = modStates.find(key); it != modStates.end())
            return it->second != 0;
        return fallback;
    }

    bool GameConfig::saveModPriorities(const fs::path &rootDir, const fs::path &buildDir) const
    {
        const fs::path dir = modloaderDir(rootDir, buildDir);
        std::error_code ec;
        fs::create_directories(dir, ec);
        std::ofstream out(dir / "modloader.ini", std::ios::trunc | std::ios::binary);
        if (!out)
            return false;
        out << "; Gerenciado pelo VCS.rec Launcher\n"
            << "[Profiles.Default.Priority]\n";
        for (const ModEntry &m : mods)
            out << m.folderName << "=" << std::clamp(m.priority, 1, 100) << "\n";
        return bool(out);
    }

    uint32_t GameConfig::steamAppId(const std::string &exeQuoted, const std::string &appName)
    {
        const std::string input = exeQuoted + appName;
        uint32_t crc = 0xFFFFFFFFu;
        for (unsigned char b : input)
        {
            crc ^= uint32_t(b);
            for (int i = 0; i < 8; ++i)
                crc = (crc >> 1) ^ (0xEDB88320u & (-(int32_t(crc & 1u))));
        }
        return (~crc) | 0x80000000u;
    }

    namespace
    {
        void appendByte(std::vector<uint8_t> &buf, uint8_t b) { buf.push_back(b); }

        void appendCString(std::vector<uint8_t> &buf, std::string_view s)
        {
            buf.insert(buf.end(), s.begin(), s.end());
            buf.push_back(0);
        }

        void appendStringKV(std::vector<uint8_t> &buf, std::string_view key, std::string_view val)
        {
            appendByte(buf, 0x01);
            appendCString(buf, key);
            appendCString(buf, val);
        }

        void appendUint32KV(std::vector<uint8_t> &buf, std::string_view key, uint32_t val)
        {
            appendByte(buf, 0x02);
            appendCString(buf, key);
            buf.push_back(uint8_t(val & 0xFFu));
            buf.push_back(uint8_t((val >> 8) & 0xFFu));
            buf.push_back(uint8_t((val >> 16) & 0xFFu));
            buf.push_back(uint8_t((val >> 24) & 0xFFu));
        }

        std::vector<uint8_t> buildShortcutEntryBody(uint32_t appId, const std::string &appName,
                                                    const std::string &exeQuoted, const std::string &startDirQuoted,
                                                    const std::string &iconPath)
        {
            std::vector<uint8_t> body;
            appendUint32KV(body, "appid", appId);
            appendStringKV(body, "AppName", appName);
            appendStringKV(body, "Exe", exeQuoted);
            appendStringKV(body, "StartDir", startDirQuoted);
            appendStringKV(body, "icon", iconPath);
            appendStringKV(body, "ShortcutPath", "");
            appendStringKV(body, "LaunchOptions", "");
            appendUint32KV(body, "IsHidden", 0);
            appendUint32KV(body, "AllowDesktopConfig", 1);
            appendUint32KV(body, "AllowOverlay", 1);
            appendUint32KV(body, "OpenVR", 0);
            appendUint32KV(body, "Devkit", 0);
            appendStringKV(body, "DevkitGameID", "");
            appendUint32KV(body, "DevkitOverrideAppID", 0);
            appendUint32KV(body, "LastPlayTime", 0);
            appendStringKV(body, "FlatpakAppID", "");
            // tags sub-map
            appendByte(body, 0x00);
            appendCString(body, "tags");
            appendStringKV(body, "0", "VCS.rec");
            appendByte(body, 0x08);
            return body;
        }

        struct ParsedVdfEntry
        {
            std::string appName;
            std::string exe;
            std::vector<uint8_t> body;
        };

        bool readCString(const std::vector<uint8_t> &buf, size_t &pos, std::string &out)
        {
            out.clear();
            while (pos < buf.size() && buf[pos] != 0)
            {
                out.push_back(char(buf[pos]));
                ++pos;
            }
            if (pos >= buf.size())
                return false;
            ++pos; // consume null terminator
            return true;
        }

        bool skipVdfMap(const std::vector<uint8_t> &buf, size_t &pos, ParsedVdfEntry *info)
        {
            while (pos < buf.size())
            {
                const uint8_t type = buf[pos++];
                if (type == 0x08)
                    return true;
                std::string key;
                if (!readCString(buf, pos, key))
                    return false;
                if (type == 0x00)
                {
                    if (!skipVdfMap(buf, pos, nullptr))
                        return false;
                }
                else if (type == 0x01)
                {
                    std::string val;
                    if (!readCString(buf, pos, val))
                        return false;
                    if (info)
                    {
                        if (iequals(key, "AppName"))
                            info->appName = val;
                        else if (iequals(key, "Exe"))
                            info->exe = val;
                    }
                }
                else if (type == 0x02)
                {
                    if (pos + 4 > buf.size())
                        return false;
                    pos += 4;
                }
                else if (type == 0x07)
                {
                    if (pos + 8 > buf.size())
                        return false;
                    pos += 8;
                }
                else
                {
                    return false;
                }
            }
            return false;
        }
    }

    std::vector<uint8_t> GameConfig::upsertShortcutsVdf(const std::vector<uint8_t> &existingVdf, uint32_t appId,
                                                        const std::string &appName, const std::string &exeQuoted,
                                                        const std::string &startDirQuoted, const std::string &iconPath)
    {
        std::vector<ParsedVdfEntry> entries;
        if (existingVdf.size() >= 12 && existingVdf[0] == 0x00)
        {
            size_t pos = 1;
            std::string rootKey;
            if (readCString(existingVdf, pos, rootKey) && iequals(rootKey, "shortcuts"))
            {
                while (pos < existingVdf.size())
                {
                    const uint8_t type = existingVdf[pos++];
                    if (type == 0x08)
                        break;
                    if (type != 0x00)
                    {
                        entries.clear();
                        break;
                    }
                    std::string idxKey;
                    if (!readCString(existingVdf, pos, idxKey))
                    {
                        entries.clear();
                        break;
                    }
                    const size_t bodyStart = pos;
                    ParsedVdfEntry e;
                    if (!skipVdfMap(existingVdf, pos, &e) || pos == 0)
                    {
                        entries.clear();
                        break;
                    }
                    const size_t bodyEnd = pos - 1; // exclude trailing 0x08 of the entry
                    e.body.assign(existingVdf.begin() + ptrdiff_t(bodyStart), existingVdf.begin() + ptrdiff_t(bodyEnd));
                    entries.push_back(std::move(e));
                }
            }
        }

        const std::vector<uint8_t> ourBody = buildShortcutEntryBody(appId, appName, exeQuoted, startDirQuoted, iconPath);
        bool updated = false;
        for (ParsedVdfEntry &e : entries)
        {
            if (iequals(e.appName, appName) || iequals(e.exe, exeQuoted))
            {
                e.appName = appName;
                e.exe = exeQuoted;
                e.body = ourBody;
                updated = true;
                break;
            }
        }
        if (!updated)
            entries.push_back({appName, exeQuoted, ourBody});

        std::vector<uint8_t> out;
        appendByte(out, 0x00);
        appendCString(out, "shortcuts");
        for (size_t i = 0; i < entries.size(); ++i)
        {
            appendByte(out, 0x00);
            appendCString(out, std::to_string(i));
            out.insert(out.end(), entries[i].body.begin(), entries[i].body.end());
            appendByte(out, 0x08);
        }
        appendByte(out, 0x08);
        appendByte(out, 0x08);
        return out;
    }

    namespace
    {
        fs::path findSteamPath()
        {
#ifdef _WIN32
            wchar_t buf[MAX_PATH * 2] = {};
            DWORD size = sizeof(buf);
            if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS && buf[0])
            {
                fs::path p(buf);
                std::error_code ec;
                if (fs::is_directory(p, ec))
                    return p;
            }
            for (const char *candidate : {"C:\\Program Files (x86)\\Steam", "C:\\Program Files\\Steam", "D:\\Steam"})
            {
                fs::path p = pathFrom(candidate);
                std::error_code ec;
                if (fs::is_directory(p / "userdata", ec))
                    return p;
            }
#else
            if (const char *home = std::getenv("HOME"); home && *home)
            {
                for (const char *rel : {".steam/steam", ".local/share/Steam"})
                {
                    fs::path p = pathFrom(home) / rel;
                    std::error_code ec;
                    if (fs::is_directory(p / "userdata", ec))
                        return p;
                }
            }
#endif
            return {};
        }
    }

    bool GameConfig::addToSteam(const fs::path &launcherExe, const fs::path &rootDir, const fs::path &assetsDir,
                                std::string &messageOut)
    {
        const fs::path steamPath = findSteamPath();
        if (steamPath.empty())
        {
            messageOut = "Steam não foi encontrada neste computador (userdata ausente).";
            return false;
        }
        const fs::path userdata = steamPath / "userdata";
        std::error_code ec;
        if (!fs::is_directory(userdata, ec))
        {
            messageOut = "Pasta userdata da Steam não encontrada em " + pathText(steamPath);
            return false;
        }

        const std::string appName = "Grand Theft Auto: Vice City Stories";
        const std::string exeQuoted = "\"" + pathText(launcherExe) + "\"";
        const std::string startDirQuoted = "\"" + pathText(rootDir) + "\"";
        const fs::path iconFile = fs::is_regular_file(assetsDir / "icone.png", ec) ? (assetsDir / "icone.png") : launcherExe;
        const std::string iconPath = pathText(iconFile);
        const uint32_t appId = steamAppId(exeQuoted, appName);
        const std::string appIdStr = std::to_string(appId);

        int updatedAccounts = 0;
        for (fs::directory_iterator it(userdata, ec), end; it != end && !ec; it.increment(ec))
        {
            if (!it->is_directory(ec))
                continue;
            const std::string idName = pathText(it->path().filename());
            if (idName == "0" || idName == "ac" || idName.empty())
                continue;
            bool digitsOnly = true;
            for (char c : idName)
                if (!std::isdigit(static_cast<unsigned char>(c)))
                    digitsOnly = false;
            if (!digitsOnly)
                continue;

            const fs::path configDir = it->path() / "config";
            const fs::path gridDir = configDir / "grid";
            fs::create_directories(gridDir, ec);

            const fs::path vdfPath = configDir / "shortcuts.vdf";
            std::vector<uint8_t> existing;
            {
                std::ifstream in(vdfPath, std::ios::binary);
                if (in)
                    existing.assign(std::istreambuf_iterator<char>(in), {});
            }

            const std::vector<uint8_t> updatedVdf =
                upsertShortcutsVdf(existing, appId, appName, exeQuoted, startDirQuoted, iconPath);
            {
                std::ofstream out(vdfPath, std::ios::binary | std::ios::trunc);
                if (!out)
                    continue;
                out.write(reinterpret_cast<const char *>(updatedVdf.data()), std::streamsize(updatedVdf.size()));
                if (!out)
                    continue;
            }

            // Copy Steam grid artwork:
            // <appid>p.png (vertical grid), <appid>_hero.png (hero), <appid>_logo.png (logo), <appid>_icon.png (icon)
            struct ArtCopy
            {
                const char *src;
                std::string dst;
            };
            const ArtCopy artFiles[] = {
                {"grid.png", appIdStr + "p.png"},
                {"hero.png", appIdStr + "_hero.png"},
                {"logo.png", appIdStr + "_logo.png"},
                {"icone.png", appIdStr + "_icon.png"},
                {"hero.png", appIdStr + ".png"}, // Horizontal grid fallback
            };
            for (const ArtCopy &art : artFiles)
            {
                const fs::path srcPath = assetsDir / art.src;
                if (fs::is_regular_file(srcPath, ec))
                    fs::copy_file(srcPath, gridDir / art.dst, fs::copy_options::overwrite_existing, ec);
            }
            ++updatedAccounts;
        }

        if (updatedAccounts == 0)
        {
            messageOut = "Nenhuma conta Steam encontrada em " + pathText(userdata);
            return false;
        }
        messageOut = "Adicionado à Steam com capa, hero, logo e ícone (AppID " + appIdStr +
                     ")! Reinicie a Steam para ver na Biblioteca.";
        return true;
    }
}
