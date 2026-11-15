// The look: hot pink for the thing to do, on a deep black (dark) or on white and blush (light); teal for done, amber
// and red for attention, and nothing else. Which of the two follows Windows' app mode unless the player picks one
// (Settings). Every colour the UI draws comes from here; sizes are logical units.
#pragma once

#include "core/math.h"

namespace theme
{
    using core::Color;
    using core::rgb;

    struct Palette
    {
        // surfaces, back to front
        Color night;      // behind everything
        Color base;       // sheets, drawers
        Color surface;    // cards
        Color surfaceHi;  // hovered cards, controls
        Color surfaceTop; // what sits on controls
        // text
        Color text, dim, faint;
        // signal colours
        Color pink;   // the primary: the thing to do, the selection
        Color pinkHi; // its gradients' lighter end
        Color onPink; // text and marks on pink
        Color teal, amber, red;
        // what is laid over surfaces at an alpha: hover tints, hairlines, dividers (its own alpha scales them)
        Color ink;
        Color shadow; // its alpha scales every shadow
        Color scrim;  // behind dialogs and sheets
        // particular fills
        Color errorFill, doneFill;                     // a failure's card, a finished task's card
        Color dangerFill, dangerFillHi, dangerText;    // the danger button
        Color logFill;                                 // the log's well
        Color well, wellBottom, wellGlow, wellGlowBottom; // the disc drop zone, idle and with a file over it
        Color sky, sea;                                // the backdrop's top and its sea
        Color horizon;                                 // the sky's glow at the horizon
        Color skyline, cloud, lights;                  // silhouettes (towers), cloud streaks, lit windows
        Color contrail;                                // jet trails, lit by the low sun
    };

    inline constexpr Palette kDarkPalette{
        .night = rgb(0x0A0A0B),
        .base = rgb(0x101012),
        .surface = rgb(0x17171A),
        .surfaceHi = rgb(0x1F1F23),
        .surfaceTop = rgb(0x2A2A2F),
        .text = rgb(0xF6F4F5),
        .dim = rgb(0xA9A5A8),
        .faint = rgb(0x6E6A6D),
        .pink = rgb(0xFF2E8B),
        .pinkHi = rgb(0xFF62A8),
        .onPink = rgb(0xFFFFFF),
        .teal = rgb(0x3DDBC6),
        .amber = rgb(0xF5B544),
        .red = rgb(0xF26868),
        .ink = rgb(0xFFFFFF),
        .shadow = rgb(0x000000),
        .scrim = rgb(0x050405, 180),
        .errorFill = rgb(0x26131A),
        .doneFill = rgb(0x12211E),
        .dangerFill = rgb(0x3A1C25),
        .dangerFillHi = rgb(0x4A222E),
        .dangerText = rgb(0xFFC9C9),
        .logFill = rgb(0x0C0C0E),
        .well = rgb(0x131315),
        .wellBottom = rgb(0x0E0E10),
        .wellGlow = rgb(0x2C0F1F),
        .wellGlowBottom = rgb(0x210B17),
        .sky = rgb(0x140A0F),
        .sea = rgb(0x0F080B),
        .horizon = rgb(0x3A0E24),
        .skyline = rgb(0x0B0709),
        .cloud = rgb(0x1E0B15),
        .lights = rgb(0xFFB8D6),
        .contrail = rgb(0xFFC2DA),
    };

    inline constexpr Palette kLightPalette{
        .night = rgb(0xFFF4F8),
        .base = rgb(0xFFFFFF),
        .surface = rgb(0xFFFFFF),
        .surfaceHi = rgb(0xFFF0F6),
        .surfaceTop = rgb(0xFFE3EE),
        .text = rgb(0x1C1517),
        .dim = rgb(0x675F62),
        .faint = rgb(0x9F9699),
        .pink = rgb(0xEC1A62),
        .pinkHi = rgb(0xFF4C84),
        .onPink = rgb(0xFFFFFF),
        .teal = rgb(0x0B9C84),
        .amber = rgb(0xB57700),
        .red = rgb(0xD6334A),
        .ink = rgb(0x3A1626, 200),
        .shadow = rgb(0x8C2457, 85),
        .scrim = rgb(0xFFF6FA, 160), // light: the content behind washes out
        .errorFill = rgb(0xFFF0F2),
        .doneFill = rgb(0xEEF9F6),
        .dangerFill = rgb(0xFFE4E8),
        .dangerFillHi = rgb(0xFFD4DB),
        .dangerText = rgb(0xB0243A),
        .logFill = rgb(0xFFFAFC),
        .well = rgb(0xFFFFFF),
        .wellBottom = rgb(0xFFF8FB),
        .wellGlow = rgb(0xFFE8F2),
        .wellGlowBottom = rgb(0xFFDBEA),
        .sky = rgb(0xFFD9E9),
        .sea = rgb(0xFFE8F1),
        .horizon = rgb(0xFFBBD6),
        .skyline = rgb(0xD9618F),
        .cloud = rgb(0xFFFAFC),
        .lights = rgb(0xFFF1F6),
        .contrail = rgb(0xFFFFFF),
    };

    // the palette in use: between the two while it changes (apply)
    inline Palette current = kDarkPalette;
    inline float darkness = 1; // 1 dark, 0 light

    // every colour, blended: 0 is light, 1 is dark
    inline void apply(float dark)
    {
        static_assert(sizeof(Palette) % sizeof(Color) == 0);
        constexpr int n = int(sizeof(Palette) / sizeof(Color));
        const Color *a = reinterpret_cast<const Color *>(&kLightPalette);
        const Color *b = reinterpret_cast<const Color *>(&kDarkPalette);
        Color *out = reinterpret_cast<Color *>(&current);
        for (int i = 0; i < n; ++i)
            out[i] = core::mix(a[i], b[i], dark);
        darkness = dark;
    }

    // the names the UI draws with
    inline Color &kNight = current.night;
    inline Color &kBase = current.base;
    inline Color &kSurface = current.surface;
    inline Color &kSurfaceHi = current.surfaceHi;
    inline Color &kSurfaceTop = current.surfaceTop;
    inline Color &kText = current.text;
    inline Color &kDim = current.dim;
    inline Color &kFaint = current.faint;
    inline Color &kPink = current.pink;
    inline Color &kPinkHi = current.pinkHi;
    inline Color &kOnPink = current.onPink;
    inline Color &kTeal = current.teal;
    inline Color &kAmber = current.amber;
    inline Color &kRed = current.red;
    inline Color &kScrim = current.scrim;
    inline Color &kErrorFill = current.errorFill;
    inline Color &kDoneFill = current.doneFill;
    inline Color &kDangerFill = current.dangerFill;
    inline Color &kDangerFillHi = current.dangerFillHi;
    inline Color &kDangerText = current.dangerText;
    inline Color &kLogFill = current.logFill;
    inline Color &kWell = current.well;
    inline Color &kWellBottom = current.wellBottom;
    inline Color &kWellGlow = current.wellGlow;
    inline Color &kWellGlowBottom = current.wellGlowBottom;
    inline Color &kSky = current.sky;
    inline Color &kSea = current.sea;
    inline Color &kHorizon = current.horizon;
    inline Color &kSkyline = current.skyline;
    inline Color &kCloud = current.cloud;
    inline Color &kLights = current.lights;
    inline Color &kContrail = current.contrail;

    // a tint over a surface (white on dark, a deep pink on light)
    inline Color ink(float a) { return core::withAlpha(current.ink, a * core::alphaOf(current.ink)); }
    // a shadow's colour at strength a (light mode's are softer and pinker)
    inline Color shade(float a) { return core::withAlpha(current.shadow, a * core::alphaOf(current.shadow)); }

    // type sizes
    constexpr float kWordmark = 46;
    constexpr float kTitle = 30;
    constexpr float kHeading = 19;
    constexpr float kBody = 14.5f;
    constexpr float kSmall = 13;
    constexpr float kEyebrow = 11.5f;

    // shape
    constexpr float kRadius = 12;      // cards
    constexpr float kRadiusSmall = 8;  // buttons, controls
    constexpr float kTitleBar = 44;

    // the icons render/icons.cpp draws (their Segoe Fluent Icons code points, kept as identifiers)
    namespace icon
    {
        constexpr char32_t kPlay = 0xE768;
        constexpr char32_t kSettings = 0xE713;
        constexpr char32_t kHelp = 0xE897;
        constexpr char32_t kSave = 0xE74E;
        constexpr char32_t kFolder = 0xED25;   // an open folder
        constexpr char32_t kOpenFile = 0xE8E5; // a page with an arrow: choosing a file
        constexpr char32_t kDisc = 0xE958;
        constexpr char32_t kDownload = 0xE896;
        constexpr char32_t kRefresh = 0xE72C;
        constexpr char32_t kMinimize = 0xE921;
        constexpr char32_t kMaximize = 0xE922;
        constexpr char32_t kRestore = 0xE923;
        constexpr char32_t kClose = 0xE8BB;
        constexpr char32_t kCheck = 0xE73E;
        constexpr char32_t kCancel = 0xE711;
        constexpr char32_t kWarning = 0xE7BA;
        constexpr char32_t kInfo = 0xE946;
        constexpr char32_t kBack = 0xE72B;
        constexpr char32_t kChevronRight = 0xE76C;
        constexpr char32_t kChevronDown = 0xE70D;
        constexpr char32_t kDocument = 0xE8A5;
        constexpr char32_t kRepair = 0xE90F;
        constexpr char32_t kCode = 0xE943;
        constexpr char32_t kChip = 0xE950;
        constexpr char32_t kClock = 0xE823;
        constexpr char32_t kSync = 0xE895;
        constexpr char32_t kLink = 0xE71B;
        constexpr char32_t kUndo = 0xE7A7;
        constexpr char32_t kBug = 0xEBE8;
        constexpr char32_t kDrive = 0xEDA2;
        constexpr char32_t kOpen = 0xE8A7; // open in new window
        constexpr char32_t kList = 0xEA37;
        constexpr char32_t kGame = 0xE7FC;
        constexpr char32_t kStop = 0xE71A;
        constexpr char32_t kPlayFilled = 0xE769;
        constexpr char32_t kCrossedTools = 0xE910;
        constexpr char32_t kSteam = 0xE911;
        constexpr char32_t kMod = 0xEA86;
    }
}
