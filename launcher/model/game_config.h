// Game configuration (build/vcs.ini), Mod Loader scanner + compatibility analyzer, and Steam shortcuts.vdf + grid
// artwork installer for the VCS.rec Rockstar-style launcher.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace launcher
{
    enum class GraphicsPreset
    {
        Custom = 0,
        OriginalPS2 = 1,
        Low = 2,
        Medium = 3,
        High = 4,
        ViceUltra = 5,
    };

    enum class ModCompat
    {
        Compatible = 0,
        Partial = 1,
        Incompatible = 2,
    };

    struct ModEntry
    {
        std::string folderName;
        std::string key; // mods.<normalized_name> in vcs.ini
        bool enabled = true;
        int priority = 50; // 1..100 in modloader/modloader.ini
        ModCompat compat = ModCompat::Compatible;
        int compatibleCount = 0;
        int unsupportedCount = 0;
        bool hasTextures = false;
        std::string summary;
        std::string detail;
    };

    struct GameConfig
    {
        // Display & Timing
        int displayWindow = 1;       // 0 = Borderless Fullscreen, 1 = Windowed
        int displayAspect = 169;     // 43, 1610, 169, 219, 329, 0 (Window)
        int displayHud = 2;          // 0 = Stretched, 2 = Widescreen, 1 = Centered 4:3
        int displayCutsceneBars = 1; // 0 = Off, 1 = On
        int displayTiming = 1;       // 0 = PAL (25/50), 1 = NTSC (30/60)
        int displayFps = 60;         // 30, 60, 100, 120, 144, 165, 240, -1 (Refresh), 0 (Unlimited)
        int displayVsync = 1;        // 0 = Off, 1 = On, 2 = Adaptive
        int displayFpsCounter = 0;   // 0..4

        // Graphics & Post-Process
        int displayRenderer = 0;        // 0 = D3D11, 1 = D3D12, 2 = Vulkan
        int displayScale = 0;           // 1, 2, 3, 4, 6, 8, 0 = Match window
        int displayMsaa = 4;            // 1, 2, 4, 8
        int displayAniso = 16;          // 0, 2, 4, 8, 16
        int graphicsPostProcess = 1;    // 0 = Off, 1 = Trails + Bloom
        int graphicsExposure = 100;     // 0..200

        // World, Draw Distance & Fog
        int worldHdRadius = 0;          // 0 = ~260m, 1 = ~450m, 2 = ~580m, 3 = ~720m
        int worldViewDistance = 20;     // 0..100
        int worldDetailDistance = 0;    // 0..100
        int fogDistance = 20;           // 10..100
        int fogStart = 0;               // 0..300

        // Population, Traffic & Streaming Memory
        int memoryStreaming = 0;        // 0 (5.4 MB PS2), 12, 20, 30 MB
        int worldPopulationDistance = 0;// 0..100
        int worldVehicleDensity = 0;    // 0..100
        int worldPedestrianDensity = 0; // 0..100
        int worldCrowdBands = 0;        // 0 = Off, 1 = On
        int worldFpsTarget = 60;        // 0 = Off, 45, 60, 70, 90, 120, 144
        int worldDistantTraffic = 0;    // 0..100
        int worldDistantCrowds = 0;     // 0..100

        // Audio & Enhancements
        int audioVolume = 100;          // 0..100
        int audioMovieVolume = 100;     // 0..100
        int audioOutput = 0;            // 0 = Stereo, 1 = Mono, 2 = Pro Logic II
        int displaySkipIntro = 0;       // 0 = Off, 1 = On
        int gameBackground = 0;         // 0 = Pause, 1 = Mute, 2 = Play
        int enhanceButtonPrompts = 0;   // enhance.prompts, the game's: 0 Original (PS2), 1 Automatic, 2 PlayStation, 3 Xbox, 4 Keyboard, 5 Nintendo Switch
        int enhancePromptStyle = 0;     // enhance.prompt_style: 0 Kenney, 1 Classic
        // the subtitles (the game's defaults): enhance.subtitle_font 0 Original / 1 Oswald, _outline 0..100, _size 80..150,
        // _shadow 1 On / 0 Off, _background 0 / 25 / 50 / 75, _colour 0 Game's / 1 White
        int subtitleFont = 0;
        int subtitleOutline = 50;
        int subtitleSize = 100;
        int subtitleShadow = 1;
        int subtitleBackground = 0;
        int subtitleColour = 0;
        int enhanceDrivebyRate = 0;     // 0 = Original PS2, 1 = Normal weapon rate

        // Gamepad & Controls
        int padPreset = 1;              // 0 = Custom, 1 = Original PS2, 2 = Modern GTA IV/V
        int padTriggerDrive = 0;        // 0 = Off (Cross/Square), 1 = On (R2/L2)
        int padTriggerShoot = 0;        // 0 = Off (Circle/R1), 1 = On (R2/L2)
        int padFreeAim = 0;             // 0 = Off, 1 = On
        int padAimAssist = 1;           // 0 = Off, 1 = On (GTA V style)
        int padDeadzone = 12;           // 0..40%
        int padCameraSpeed = 100;       // 10..400%
        int padVibration = 100;         // 0..200%
        int padInvertY = 0;             // 0 = Normal, 1 = Inverted
        int dualsenseTriggers = 1;      // Adaptive triggers
        int dualsenseLightbar = 2;      // 0 = Off, 1 = Health, 2 = Vice Pink
        int dualsensePoliceFlash = 1;   // 0 = Off, 1 = On
        int dualsenseSpeaker = 0;       // pad.ds_speaker: 0 = Off, 1 = All sound, 2 = Police radio
        int mouseCamera = 1;            // 0 = Off, 1 = On
        int mouseSensitivity = 100;     // 10..400%
        int mouseFreeAim = 0;           // 0 = Off, 1 = On
        int mouseInvertY = 0;           // 0 = Off, 1 = On
        int mouseWheel = 1;             // 0 = Off, 1 = On

        // Mod Loader
        int modsEnabled = 1;            // 0 = Off, 1 = On
        int graphicsTextureReplacements = 1; // 0 = Off, 1 = On
        std::vector<ModEntry> mods;
        std::map<std::string, int> modStates; // key -> 0/1 from vcs.ini
        bool hasTextureMods() const;
        void applyPadPreset(int preset);

        bool graphicsConfigured = false; // the last vcs.ini read held graphics settings (not a first install)
        bool loadIni(const std::filesystem::path &iniPath);
        bool saveIni(const std::filesystem::path &iniPath) const;

        void applyPreset(GraphicsPreset preset);
        GraphicsPreset detectPreset() const;

        // Real hardware cost estimators (0..100%)
        int estimatedGpuCostPercent() const;
        int estimatedCpuCostPercent() const;
        float estimatedStreamingMb() const;
        float estimatedStreamingLoadMb() const;
        float streamingPoolCapacityMb() const;
        void recalculateStreamingMemory();

        // Mod Loader scanning & priority persistence
        static std::string normalizeModKey(std::string_view folderName);
        static ModEntry classifyModFolder(const std::filesystem::path &modDir);
        void scanMods(const std::filesystem::path &rootDir, const std::filesystem::path &buildDir);
        bool saveModPriorities(const std::filesystem::path &rootDir, const std::filesystem::path &buildDir) const;

        // Steam non-Steam shortcut + grid artwork registration
        static uint32_t steamAppId(const std::string &exeQuoted, const std::string &appName);
        static std::vector<uint8_t> upsertShortcutsVdf(const std::vector<uint8_t> &existingVdf, uint32_t appId,
                                                       const std::string &appName, const std::string &exeQuoted,
                                                       const std::string &startDirQuoted, const std::string &iconPath);
        static bool addToSteam(const std::filesystem::path &launcherExe, const std::filesystem::path &rootDir,
                               const std::filesystem::path &assetsDir, std::string &messageOut);
    };
}
