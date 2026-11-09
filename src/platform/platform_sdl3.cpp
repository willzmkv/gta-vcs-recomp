// SDL3 implementation of platform.h.
#include "platform/platform.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace platform
{
    namespace
    {
        SDL_Window *g_window = nullptr;
        bool g_quit = false;
        bool g_imgui = false;
        uint64_t g_nextFrameNs = 0;
        std::atomic<uint64_t> g_framePeriodNs{1000000000ull / 60u}; // setFrameRate

        std::array<std::atomic<bool>, KEY_COUNT> g_keys{};
        std::array<bool, KEY_COUNT> g_keysPrev{}; // main thread
        std::array<bool, KEY_COUNT> g_keysPressed{};
        std::array<std::atomic<bool>, MOUSE_BUTTON_COUNT> g_mouse{};
        std::atomic<float> g_mouseX{0.0f}, g_mouseY{0.0f}; // mousePosition
        std::atomic<bool> g_mouseOver{false};
        std::array<std::atomic<bool>, GAMEPAD_BUTTON_COUNT> g_padButtons{};
        std::array<std::atomic<float>, AXIS_COUNT> g_padAxes{};
        std::atomic<bool> g_padConnected{false};
        std::atomic<int> g_padFamily{0}; // GamepadFamily
        SDL_Gamepad *g_gamepad = nullptr;
        std::atomic<int> g_dsPadFamily{GAMEPAD_FAMILY_GENERIC}; // DsPadFamily: our PS5/PS4/generic split for LED + triggers
        std::atomic<bool> g_padHasLED{false};                  // an RGB light bar
        std::atomic<bool> g_padHasTriggerFx{false};            // DualSense adaptive triggers (effect packets)

        SDL_AudioStream *g_audio = nullptr;
        AudioFill g_audioFill = nullptr;
        std::vector<int16_t> g_audioScratch;
        uint32_t g_audioRate = 0, g_audioBufferFrames = 0; // the last audioOpen's, so it can reopen on another device
        std::string g_audioDeviceWant;                     // a playback device name to prefer (empty = the system default)
        std::string g_audioDeviceName;                     // the device the stream is on now

        // window placement (setStartPlacement / onPlacementChanged); main thread
        WindowPlacement g_startPlacement;
        bool g_hasStartPlacement = false;
        PlacementChanged g_placementChanged = nullptr;
        bool g_trackPlacement = false;   // a visible window
        SDL_Rect g_normalRect{};         // the last restored (not maximized / minimized) client area, screen coordinates
        bool g_maximized = false;
        uint64_t g_placementDirtyNs = 0; // time of the last move / resize not reported yet
        WindowPlacement g_reported;      // what the callback last saw (or what the window opened with)
        constexpr uint64_t kPlacementSettleNs = 500000000ull;

        // full screen (setFullscreen); main thread
        bool g_fullscreen = false;
        bool g_hiddenWindow = false;
        std::atomic<int64_t> g_snapshotNs{0}; // inputSnapshotNs
        // frame lead (setFrameLead), main thread
        uint64_t g_leadNs = 0;
        FrameLead g_atLead = nullptr;
        bool g_leadRan = false;
        bool g_slept = false; // the last waitNextFrame waited (frameSlept)
        // keyPressed edges seen by input refreshes between frames, reported at the next frame-start pump
        std::array<bool, KEY_COUNT> g_edgesPending{};
        uint64_t g_mouseMovedNs = 0;
        bool g_cursorHidden = false;
        constexpr uint64_t kCursorIdleNs = 2000000000ull;

        // mouse capture and wheel notches (main thread); motion for takeMouseMotion (any thread)
        bool g_wantCapture = false;
        bool g_captured = false;
        std::mutex g_motionMutex;
        float g_motionX = 0.0f, g_motionY = 0.0f;
        struct WheelPulses // one direction's notches, played out as short presses
        {
            int pending = 0;
            uint64_t phaseEndNs = 0; // end of the current down or up half
            bool down = false;
        };
        WheelPulses g_wheel[2]; // up, down
        constexpr uint64_t kWheelHalfNs = 50000000ull;

        // vibration (gamepadRumble): wanted motor levels 0..65535, applied by pumpEvents
        std::atomic<uint32_t> g_rumbleWanted{0}; // low << 16 | high
        uint32_t g_rumbleSent = 0;
        uint64_t g_rumbleSentNs = 0;

        struct ScanKey
        {
            SDL_Scancode scancode;
            int key;
        };

        // SDL scancodes (physical positions) for the keys platform.h names.
        const std::vector<ScanKey> &scanKeys()
        {
            static const std::vector<ScanKey> keys = [] {
                std::vector<ScanKey> k = {
                    {SDL_SCANCODE_SPACE, KEY_SPACE}, {SDL_SCANCODE_APOSTROPHE, KEY_APOSTROPHE}, {SDL_SCANCODE_COMMA, KEY_COMMA},
                    {SDL_SCANCODE_MINUS, KEY_MINUS}, {SDL_SCANCODE_PERIOD, KEY_PERIOD}, {SDL_SCANCODE_SLASH, KEY_SLASH},
                    {SDL_SCANCODE_SEMICOLON, KEY_SEMICOLON}, {SDL_SCANCODE_EQUALS, KEY_EQUAL},
                    {SDL_SCANCODE_LEFTBRACKET, KEY_LEFT_BRACKET}, {SDL_SCANCODE_BACKSLASH, KEY_BACKSLASH},
                    {SDL_SCANCODE_RIGHTBRACKET, KEY_RIGHT_BRACKET}, {SDL_SCANCODE_GRAVE, KEY_GRAVE},
                    {SDL_SCANCODE_ESCAPE, KEY_ESCAPE}, {SDL_SCANCODE_RETURN, KEY_ENTER}, {SDL_SCANCODE_TAB, KEY_TAB},
                    {SDL_SCANCODE_BACKSPACE, KEY_BACKSPACE}, {SDL_SCANCODE_INSERT, KEY_INSERT}, {SDL_SCANCODE_DELETE, KEY_DELETE},
                    {SDL_SCANCODE_RIGHT, KEY_RIGHT}, {SDL_SCANCODE_LEFT, KEY_LEFT}, {SDL_SCANCODE_DOWN, KEY_DOWN},
                    {SDL_SCANCODE_UP, KEY_UP}, {SDL_SCANCODE_PAGEUP, KEY_PAGE_UP}, {SDL_SCANCODE_PAGEDOWN, KEY_PAGE_DOWN},
                    {SDL_SCANCODE_HOME, KEY_HOME}, {SDL_SCANCODE_END, KEY_END}, {SDL_SCANCODE_KP_ENTER, KEY_KP_ENTER},
                    {SDL_SCANCODE_LSHIFT, KEY_LEFT_SHIFT}, {SDL_SCANCODE_LCTRL, KEY_LEFT_CONTROL}, {SDL_SCANCODE_LALT, KEY_LEFT_ALT},
                    {SDL_SCANCODE_RSHIFT, KEY_RIGHT_SHIFT}, {SDL_SCANCODE_RCTRL, KEY_RIGHT_CONTROL}, {SDL_SCANCODE_RALT, KEY_RIGHT_ALT},
                    {SDL_SCANCODE_0, KEY_ZERO}, {SDL_SCANCODE_CAPSLOCK, KEY_CAPS_LOCK},
                    {SDL_SCANCODE_KP_PERIOD, KEY_KP_DECIMAL}, {SDL_SCANCODE_KP_DIVIDE, KEY_KP_DIVIDE},
                    {SDL_SCANCODE_KP_MULTIPLY, KEY_KP_MULTIPLY}, {SDL_SCANCODE_KP_MINUS, KEY_KP_SUBTRACT},
                    {SDL_SCANCODE_KP_PLUS, KEY_KP_ADD},
                };
                for (int i = 0; i < 26; ++i)
                    k.push_back({static_cast<SDL_Scancode>(SDL_SCANCODE_A + i), KEY_A + i});
                for (int i = 0; i < 9; ++i) // SDL orders 1..9 then 0
                    k.push_back({static_cast<SDL_Scancode>(SDL_SCANCODE_1 + i), KEY_ONE + i});
                for (int i = 0; i < 12; ++i)
                    k.push_back({static_cast<SDL_Scancode>(SDL_SCANCODE_F1 + i), KEY_F1 + i});
                k.push_back({SDL_SCANCODE_KP_0, KEY_KP_0});
                for (int i = 0; i < 9; ++i)
                    k.push_back({static_cast<SDL_Scancode>(SDL_SCANCODE_KP_1 + i), KEY_KP_1 + i});
                return k;
            }();
            return keys;
        }

        void noteGamepadCaps()
        {
            DsPadFamily family = GAMEPAD_FAMILY_GENERIC;
            bool hasLED = false, hasTriggerFx = false;
            if (g_gamepad)
            {
                switch (SDL_GetGamepadType(g_gamepad))
                {
                case SDL_GAMEPAD_TYPE_PS5: family = GAMEPAD_FAMILY_PS5; break;
                case SDL_GAMEPAD_TYPE_PS4: family = GAMEPAD_FAMILY_PS4; break;
                default: family = GAMEPAD_FAMILY_GENERIC; break;
                }
                const SDL_PropertiesID props = SDL_GetGamepadProperties(g_gamepad);
                hasLED = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, false);
                // SDL exposes no "adaptive triggers" cap; the DualSense is the only pad it sends effect packets to.
                hasTriggerFx = family == GAMEPAD_FAMILY_PS5;
            }
            g_dsPadFamily.store(family, std::memory_order_relaxed);
            g_padHasLED.store(hasLED, std::memory_order_relaxed);
            g_padHasTriggerFx.store(hasTriggerFx, std::memory_order_relaxed);
        }

        GamepadFamily familyOf(SDL_GamepadType type)
        {
            switch (type)
            {
            case SDL_GAMEPAD_TYPE_XBOX360:
            case SDL_GAMEPAD_TYPE_XBOXONE:
                return GamepadFamily::Xbox;
            case SDL_GAMEPAD_TYPE_PS3:
            case SDL_GAMEPAD_TYPE_PS4:
            case SDL_GAMEPAD_TYPE_PS5:
                return GamepadFamily::PlayStation;
            case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
            case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
            case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
            case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
                return GamepadFamily::Nintendo;
            default:
                return GamepadFamily::Other;
            }
        }

        void openGamepad(SDL_JoystickID id)
        {
            if (g_gamepad)
                return;
            g_gamepad = SDL_OpenGamepad(id);
            if (g_gamepad)
            {
                noteGamepadCaps();
                std::fprintf(stderr, "[platform] gamepad: %s%s%s\n", SDL_GetGamepadName(g_gamepad),
                             g_padHasLED.load(std::memory_order_relaxed) ? " [light bar]" : "",
                             g_padHasTriggerFx.load(std::memory_order_relaxed) ? " [adaptive triggers]" : "");
            }
        }

        void snapshotGamepad()
        {
            static constexpr SDL_GamepadButton kButtons[GAMEPAD_BUTTON_COUNT] = {
                SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
                SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK,
                SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_DPAD_UP,
                SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
            };
            static constexpr SDL_GamepadAxis kAxes[AXIS_COUNT] = {
                SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY,
                SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
            };
            g_padConnected.store(g_gamepad != nullptr, std::memory_order_relaxed);
            g_padFamily.store(static_cast<int>(g_gamepad ? familyOf(SDL_GetGamepadType(g_gamepad)) : GamepadFamily::None),
                              std::memory_order_relaxed);
            for (int b = 0; b < GAMEPAD_BUTTON_COUNT; ++b)
                g_padButtons[b].store(g_gamepad && SDL_GetGamepadButton(g_gamepad, kButtons[b]), std::memory_order_relaxed);
            for (int a = 0; a < AXIS_COUNT; ++a)
            {
                const float v = g_gamepad ? SDL_GetGamepadAxis(g_gamepad, kAxes[a]) / 32767.0f : 0.0f;
                g_padAxes[a].store(v < -1.0f ? -1.0f : v, std::memory_order_relaxed);
            }
        }

        // The monitor `p` was on: the one with its name and desktop origin, else its name, else its origin (renamed by a
        // driver); 0 when it is gone.
        SDL_DisplayID findDisplay(const WindowPlacement &p)
        {
            int count = 0;
            SDL_DisplayID *ids = SDL_GetDisplays(&count);
            SDL_DisplayID best = 0;
            int bestScore = 0;
            for (int i = 0; ids && i < count; ++i)
            {
                const char *name = SDL_GetDisplayName(ids[i]);
                SDL_Rect b{};
                SDL_GetDisplayBounds(ids[i], &b);
                const int score = (!p.display.empty() && name && p.display == name ? 2 : 0) +
                                  (b.x == p.displayX && b.y == p.displayY ? 1 : 0);
                if (score > bestScore)
                {
                    best = ids[i];
                    bestScore = score;
                }
            }
            SDL_free(ids);
            return best;
        }

        // Puts the new (still hidden) window at the start placement, inside its monitor's usable area (the taskbar left
        // out) with the frame on screen, so a changed resolution cannot strand it. False when the monitor is gone.
        bool placeWindow(const WindowPlacement &p)
        {
            const SDL_DisplayID display = findDisplay(p);
            SDL_Rect bounds{}, usable{};
            if (!display || !SDL_GetDisplayBounds(display, &bounds) || !SDL_GetDisplayUsableBounds(display, &usable))
                return false;
            int top = 0, left = 0, bottom = 0, right = 0;
            SDL_GetWindowBordersSize(g_window, &top, &left, &bottom, &right);
            const int w = std::clamp(p.width, 160, std::max(160, usable.w - left - right));
            const int h = std::clamp(p.height, 120, std::max(120, usable.h - top - bottom));
            const int x = std::clamp(bounds.x + p.x, usable.x + left, std::max(usable.x + left, usable.x + usable.w - right - w));
            const int y = std::clamp(bounds.y + p.y, usable.y + top, std::max(usable.y + top, usable.y + usable.h - bottom - h));
            SDL_SetWindowSize(g_window, w, h);
            SDL_SetWindowPosition(g_window, x, y);
            if (p.maximized)
                SDL_MaximizeWindow(g_window); // on the monitor of the restored area; applied when shown
            g_normalRect = {x, y, w, h};
            g_maximized = p.maximized;
            return true;
        }

        bool currentPlacement(WindowPlacement &p)
        {
            if (g_normalRect.w <= 0 || g_normalRect.h <= 0)
                return false;
            const SDL_DisplayID display = SDL_GetDisplayForRect(&g_normalRect);
            SDL_Rect b{};
            if (!display || !SDL_GetDisplayBounds(display, &b))
                return false;
            const char *name = SDL_GetDisplayName(display);
            p.display = name ? name : "";
            p.displayX = b.x;
            p.displayY = b.y;
            p.x = g_normalRect.x - b.x;
            p.y = g_normalRect.y - b.y;
            p.width = g_normalRect.w;
            p.height = g_normalRect.h;
            p.maximized = g_maximized;
            return true;
        }

        bool samePlacement(const WindowPlacement &a, const WindowPlacement &b)
        {
            return a.display == b.display && a.displayX == b.displayX && a.displayY == b.displayY && a.x == b.x &&
                   a.y == b.y && a.width == b.width && a.height == b.height && a.maximized == b.maximized;
        }

        // A move / resize / maximize / restore: remember the restored area (a maximized or minimized window keeps the
        // one it had before).
        void notePlacement()
        {
            if (!g_trackPlacement)
                return;
            const SDL_WindowFlags flags = SDL_GetWindowFlags(g_window);
            if (flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_FULLSCREEN))
                return; // full screen keeps the window it came from (vcs.ini's window.* stay the windowed place)
            g_maximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
            if (!g_maximized)
            {
                SDL_GetWindowPosition(g_window, &g_normalRect.x, &g_normalRect.y);
                SDL_GetWindowSize(g_window, &g_normalRect.w, &g_normalRect.h);
            }
            g_placementDirtyNs = SDL_GetTicksNS();
        }

        void reportPlacement()
        {
            g_placementDirtyNs = 0;
            WindowPlacement p;
            if (!g_placementChanged || !currentPlacement(p) || samePlacement(p, g_reported))
                return;
            g_reported = p;
            g_placementChanged(p);
        }

        void SDLCALL audioCallback(void *, SDL_AudioStream *stream, int additional, int)
        {
            const uint32_t frames = static_cast<uint32_t>(additional) / 4u;
            if (!frames || !g_audioFill)
                return;
            g_audioScratch.resize(frames * 2u);
            g_audioFill(g_audioScratch.data(), frames);
            SDL_PutAudioStreamData(stream, g_audioScratch.data(), static_cast<int>(frames * 4u));
        }

        // ---- PNG ----------------------------------------------------------------------------------------------
        uint32_t pngCrc32(uint32_t crc, const uint8_t *p, size_t n) // (not zlib's crc32)
        {
            static const auto table = [] {
                std::array<uint32_t, 256> t{};
                for (uint32_t i = 0; i < 256; ++i)
                {
                    uint32_t c = i;
                    for (int k = 0; k < 8; ++k)
                        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    t[i] = c;
                }
                return t;
            }();
            crc = ~crc;
            while (n--)
                crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
            return ~crc;
        }

        void put32(std::vector<uint8_t> &v, uint32_t x)
        {
            v.push_back(static_cast<uint8_t>(x >> 24));
            v.push_back(static_cast<uint8_t>(x >> 16));
            v.push_back(static_cast<uint8_t>(x >> 8));
            v.push_back(static_cast<uint8_t>(x));
        }

        void chunk(std::vector<uint8_t> &out, const char *type, const std::vector<uint8_t> &data)
        {
            put32(out, static_cast<uint32_t>(data.size()));
            const size_t start = out.size();
            out.insert(out.end(), type, type + 4);
            out.insert(out.end(), data.begin(), data.end());
            put32(out, pngCrc32(0, out.data() + start, out.size() - start));
        }
    }

    void setStartPlacement(const WindowPlacement &placement)
    {
        g_startPlacement = placement;
        g_hasStartPlacement = true;
    }

    void onPlacementChanged(PlacementChanged callback) { g_placementChanged = callback; }

    void setFullscreen(bool on)
    {
        if (on == g_fullscreen)
            return;
        g_fullscreen = on;
        g_mouseMovedNs = SDL_GetTicksNS(); // the cursor stays for a moment after the switch
        if (!g_window || g_hiddenWindow)
            return;
        // the fullscreen mode stays NULL: borderless at the desktop's resolution (SDL3's default)
        if (!SDL_SetWindowFullscreen(g_window, on))
            std::fprintf(stderr, "[platform] SDL_SetWindowFullscreen(%d): %s\n", on ? 1 : 0, SDL_GetError());
        SDL_SyncWindow(g_window);
        std::fprintf(stderr, "[platform] %s\n", on ? "full screen" : "windowed");
    }

    bool fullscreen() { return g_fullscreen; }

    bool init(const WindowDesc &desc)
    {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
        {
            std::fprintf(stderr, "[platform] SDL_Init: %s\n", SDL_GetError());
            return false;
        }
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
            std::fprintf(stderr, "[platform] no audio: %s\n", SDL_GetError());

        // created hidden, placed, then shown: no flash at the default place
        g_window = SDL_CreateWindow(desc.title, desc.width, desc.height,
                                    SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | (desc.vulkan ? SDL_WINDOW_VULKAN : 0));
        if (!g_window)
        {
            std::fprintf(stderr, "[platform] SDL_CreateWindow: %s\n", SDL_GetError());
            return false;
        }
        g_hiddenWindow = desc.hidden;
        if (!desc.hidden)
        {
            g_trackPlacement = true;
            const bool placed = g_hasStartPlacement && placeWindow(g_startPlacement);
            if (g_hasStartPlacement && !placed)
                std::fprintf(stderr, "[platform] monitor '%s' not found: window at the default place\n",
                             g_startPlacement.display.c_str());
            if (g_fullscreen) // on the monitor the window was placed on; leaving it restores that place
            {
                SDL_SetWindowFullscreen(g_window, true);
                std::fprintf(stderr, "[platform] full screen\n");
            }
            g_mouseMovedNs = SDL_GetTicksNS();
            SDL_ShowWindow(g_window);
            SDL_SyncWindow(g_window);
            if (!placed)
                notePlacement();
            currentPlacement(g_reported);
            g_placementDirtyNs = 0;
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::StyleColorsDark();
        g_imgui = ImGui_ImplSDL3_InitForOther(g_window);
        if (!g_imgui)
            std::fprintf(stderr, "[platform] Dear ImGui input backend failed\n");
        return true;
    }

    void shutdown()
    {
        if (!g_window)
            return;
        if (g_placementDirtyNs)
            reportPlacement();
        g_trackPlacement = false;
        audioClose();
        if (g_imgui)
        {
            ImGui_ImplSDL3_Shutdown();
            g_imgui = false;
        }
        if (ImGui::GetCurrentContext())
            ImGui::DestroyContext();
        if (g_gamepad)
            SDL_CloseGamepad(g_gamepad);
        g_gamepad = nullptr;
        if (g_window)
            SDL_DestroyWindow(g_window);
        g_window = nullptr;
        SDL_Quit();
    }

    bool ready() { return g_window != nullptr; }

    void *nativeWindow()
    {
        if (!g_window)
            return nullptr;
#if defined(_WIN32)
        return SDL_GetPointerProperty(SDL_GetWindowProperties(g_window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#else
        return nullptr;
#endif
    }

    bool windowHidden() { return g_hiddenWindow; }

    const char *const *vulkanInstanceExtensions(uint32_t *count)
    {
        Uint32 n = 0;
        const char *const *names = SDL_Vulkan_GetInstanceExtensions(&n);
        if (count)
            *count = names ? n : 0;
        if (!names)
            std::fprintf(stderr, "[platform] SDL_Vulkan_GetInstanceExtensions: %s\n", SDL_GetError());
        return names;
    }

    bool vulkanCreateSurface(void *instance, uint64_t *surface)
    {
        VkSurfaceKHR made{};
        if (!g_window || !SDL_Vulkan_CreateSurface(g_window, static_cast<VkInstance>(instance), nullptr, &made))
        {
            std::fprintf(stderr, "[platform] SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
            return false;
        }
        *surface = reinterpret_cast<uint64_t>(made);
        return true;
    }

    namespace
    {
        // a key's went-down edge: kept by refreshes between frames, reported (with the kept ones) at a frame start
        void noteEdge(int k, bool edge, bool frameStart)
        {
            if (!frameStart)
            {
                g_edgesPending[k] = g_edgesPending[k] || edge;
                return;
            }
            g_keysPressed[k] = edge || g_edgesPending[k];
            g_edgesPending[k] = false;
        }
    }

    void pumpEvents(bool frameStart)
    {
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            if (g_imgui)
                ImGui_ImplSDL3_ProcessEvent(&e);
            switch (e.type)
            {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                g_quit = true;
                break;
            case SDL_EVENT_WINDOW_MOVED:
            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_MAXIMIZED:
            case SDL_EVENT_WINDOW_RESTORED:
            case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
                notePlacement();
                break;
            case SDL_EVENT_MOUSE_MOTION:
                g_mouseMovedNs = SDL_GetTicksNS();
                if (g_captured)
                {
                    std::lock_guard<std::mutex> lock(g_motionMutex);
                    g_motionX += e.motion.xrel;
                    g_motionY += e.motion.yrel;
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                g_mouseMovedNs = SDL_GetTicksNS();
                break;
            case SDL_EVENT_MOUSE_WHEEL:
            {
                const float y = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
                const int notches = static_cast<int>(y > 0.0f ? std::ceil(y - 0.01f) : std::floor(y + 0.01f));
                if (notches != 0)
                {
                    WheelPulses &p = g_wheel[notches > 0 ? 0 : 1];
                    p.pending = std::min(8, p.pending + std::abs(notches));
                }
                break;
            }
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                if (g_captured) // Alt+Tab: the cursor is free while the window is in the background
                {
                    SDL_SetWindowRelativeMouseMode(g_window, false);
                    g_captured = false;
                }
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                openGamepad(e.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                if (g_gamepad && SDL_GetGamepadID(g_gamepad) == e.gdevice.which)
                {
                    SDL_CloseGamepad(g_gamepad);
                    g_gamepad = nullptr;
                    noteGamepadCaps(); // reset the light bar / trigger caps until another pad opens
                    int count = 0;
                    if (SDL_JoystickID *ids = SDL_GetGamepads(&count))
                    {
                        if (count > 0)
                            openGamepad(ids[0]);
                        SDL_free(ids);
                    }
                }
                break;
            default:
                break;
            }
        }
        // reported once it has settled (a drag sends a stream of moves), so a killed process still keeps it
        if (g_placementDirtyNs && SDL_GetTicksNS() - g_placementDirtyNs >= kPlacementSettleNs)
            reportPlacement();

        int count = 0;
        const bool *state = SDL_GetKeyboardState(&count);
        std::array<bool, KEY_COUNT> now{};
        for (const ScanKey &k : scanKeys())
            if (k.scancode < count && state[k.scancode])
                now[k.key] = true;
        for (int k = 0; k < KEY_COUNT; ++k)
        {
            if (k >= KEY_MOUSE_LEFT && k <= KEY_WHEEL_DOWN)
                continue; // below
            noteEdge(k, now[k] && !g_keysPrev[k], frameStart);
            g_keysPrev[k] = now[k];
            g_keys[k].store(now[k], std::memory_order_relaxed);
        }
        float mouseX = 0.0f, mouseY = 0.0f;
        const SDL_MouseButtonFlags mouse = SDL_GetMouseState(&mouseX, &mouseY);
        {
            // the cursor in drawable pixels (SDL reports window coordinates), while it is over our window
            const float density = g_window ? SDL_GetWindowPixelDensity(g_window) : 1.0f;
            const bool over = g_window && !g_hiddenWindow && !g_captured && SDL_GetMouseFocus() == g_window;
            g_mouseX.store(mouseX * (density > 0.0f ? density : 1.0f), std::memory_order_relaxed);
            g_mouseY.store(mouseY * (density > 0.0f ? density : 1.0f), std::memory_order_relaxed);
            g_mouseOver.store(over, std::memory_order_relaxed);
        }
        g_mouse[MOUSE_LEFT].store((mouse & SDL_BUTTON_LMASK) != 0, std::memory_order_relaxed);
        g_mouse[MOUSE_RIGHT].store((mouse & SDL_BUTTON_RMASK) != 0, std::memory_order_relaxed);
        g_mouse[MOUSE_MIDDLE].store((mouse & SDL_BUTTON_MMASK) != 0, std::memory_order_relaxed);
        std::array<bool, KEY_WHEEL_DOWN + 1> pseudo{}; // the mouse buttons and wheel notches as keys
        pseudo[KEY_MOUSE_LEFT] = (mouse & SDL_BUTTON_LMASK) != 0;
        pseudo[KEY_MOUSE_RIGHT] = (mouse & SDL_BUTTON_RMASK) != 0;
        pseudo[KEY_MOUSE_MIDDLE] = (mouse & SDL_BUTTON_MMASK) != 0;
        pseudo[KEY_MOUSE_X1] = (mouse & SDL_BUTTON_X1MASK) != 0;
        pseudo[KEY_MOUSE_X2] = (mouse & SDL_BUTTON_X2MASK) != 0;
        const uint64_t nowNs = SDL_GetTicksNS();
        for (int w = 0; w < 2; ++w)
        {
            WheelPulses &p = g_wheel[w];
            if (nowNs >= p.phaseEndNs)
            {
                if (p.down) // an up half before the next notch, so each one is a press of its own
                {
                    p.down = false;
                    p.phaseEndNs = nowNs + kWheelHalfNs;
                }
                else if (p.pending > 0)
                {
                    --p.pending;
                    p.down = true;
                    p.phaseEndNs = nowNs + kWheelHalfNs;
                }
            }
            pseudo[w == 0 ? KEY_WHEEL_UP : KEY_WHEEL_DOWN] = p.down;
        }
        for (int k = KEY_MOUSE_LEFT; k <= KEY_WHEEL_DOWN; ++k)
        {
            noteEdge(k, pseudo[k] && !g_keysPrev[k], frameStart);
            g_keysPrev[k] = pseudo[k];
            g_keys[k].store(pseudo[k], std::memory_order_relaxed);
        }
        snapshotGamepad();

        // capture follows the wish while the window has the focus (it was let go on focus loss)
        const bool capture = g_wantCapture && windowFocused();
        if (capture != g_captured && g_window)
        {
            SDL_SetWindowRelativeMouseMode(g_window, capture);
            g_captured = capture;
            std::lock_guard<std::mutex> lock(g_motionMutex);
            g_motionX = g_motionY = 0.0f;
        }

        // vibration: sent on a change, and renewed while it lasts (SDL ends a rumble after its duration)
        const uint32_t rumble = g_rumbleWanted.load(std::memory_order_relaxed);
        if (g_gamepad && (rumble != g_rumbleSent || (rumble && nowNs - g_rumbleSentNs > 500000000ull)))
        {
            SDL_RumbleGamepad(g_gamepad, static_cast<Uint16>(rumble >> 16), static_cast<Uint16>(rumble & 0xFFFF), rumble ? 1000 : 0);
            g_rumbleSent = rumble;
            g_rumbleSentNs = nowNs;
        }
        g_snapshotNs.store(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(),
                           std::memory_order_release);
    }

    int64_t inputSnapshotNs() { return g_snapshotNs.load(std::memory_order_acquire); }

    bool quitRequested() { return g_quit; }

    void beginFrame()
    {
        // full screen: the cursor hides while the mouse is still (Dear ImGui's backend would show it again each frame)
        const bool hideCursor = g_fullscreen && !g_hiddenWindow && SDL_GetTicksNS() - g_mouseMovedNs > kCursorIdleNs;
        if (hideCursor != g_cursorHidden)
        {
            g_cursorHidden = hideCursor;
            if (ImGui::GetCurrentContext())
            {
                ImGuiIO &io = ImGui::GetIO();
                io.ConfigFlags = hideCursor ? io.ConfigFlags | ImGuiConfigFlags_NoMouseCursorChange
                                            : io.ConfigFlags & ~ImGuiConfigFlags_NoMouseCursorChange;
            }
            hideCursor ? SDL_HideCursor() : SDL_ShowCursor();
        }
        if (!g_imgui)
            return;
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
    }

    void endUiFrame()
    {
        if (g_imgui)
            ImGui::Render();
    }

    void setFrameRate(double hz)
    {
        g_framePeriodNs.store(static_cast<uint64_t>(1e9 / std::clamp(hz, 24.0, 1000.0)), std::memory_order_relaxed);
    }

    double displayRefreshHz()
    {
        const SDL_DisplayID display = g_window ? SDL_GetDisplayForWindow(g_window) : SDL_GetPrimaryDisplay();
        const SDL_DisplayMode *mode = display ? SDL_GetCurrentDisplayMode(display) : nullptr;
        return mode && mode->refresh_rate > 1.0f ? mode->refresh_rate : 60.0;
    }

    void delayPrecise(uint64_t ns) { SDL_DelayPrecise(ns); }

    void waitNextFrame()
    {
        // 60 host frames per second by default, as raylib's SetTargetFPS(60) did; setFrameRate changes it
        const uint64_t kPeriodNs = g_framePeriodNs.load(std::memory_order_relaxed);
        const uint64_t now = SDL_GetTicksNS();
        g_nextFrameNs = g_nextFrameNs ? g_nextFrameNs + kPeriodNs : now + kPeriodNs;
        g_leadRan = false;
        g_slept = false;
        if (g_nextFrameNs < now)
        {
            g_nextFrameNs = now; // fell behind: don't try to catch up
            return;
        }
        g_slept = true;
        // the frame lead: wake early for its call, then sleep the rest (none when its time has passed)
        const uint64_t lead = std::min(g_leadNs, kPeriodNs);
        if (g_atLead && lead && g_nextFrameNs - lead > now)
        {
            SDL_DelayPrecise(g_nextFrameNs - lead - now);
            g_atLead();
            g_leadRan = true;
            const uint64_t after = SDL_GetTicksNS();
            if (g_nextFrameNs > after)
                SDL_DelayPrecise(g_nextFrameNs - after);
            return;
        }
        SDL_DelayPrecise(g_nextFrameNs - now);
    }

    void setFrameLead(uint64_t leadNs, FrameLead atLead)
    {
        g_leadNs = leadNs;
        g_atLead = atLead;
    }

    bool frameLeadRan() { return g_leadRan; }

    bool frameSlept() { return g_slept; }

    void windowSize(int &width, int &height)
    {
        width = height = 0;
        if (g_window)
            SDL_GetWindowSizeInPixels(g_window, &width, &height);
    }

    bool windowFocused()
    {
        return g_window && !g_hiddenWindow && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    }

    bool openFolder(const std::string &utf8Path)
    {
        std::string url = utf8Path.rfind('/', 0) == 0 ? "file://" : "file:///"; // /home/... or D:/...
        for (const char c : utf8Path)
            url += c == '\\' ? '/' : c;
        if (SDL_OpenURL(url.c_str()))
            return true;
        std::fprintf(stderr, "[platform] could not open %s: %s\n", utf8Path.c_str(), SDL_GetError());
        return false;
    }

    bool keyDown(int key) { return key >= 0 && key < KEY_COUNT && g_keys[key].load(std::memory_order_relaxed); }
    bool keyPressed(int key) { return key >= 0 && key < KEY_COUNT && g_keysPressed[key]; }
    bool mouseDown(MouseButton button) { return g_mouse[button].load(std::memory_order_relaxed); }
    bool mousePosition(float &x, float &y)
    {
        x = g_mouseX.load(std::memory_order_relaxed);
        y = g_mouseY.load(std::memory_order_relaxed);
        return g_mouseOver.load(std::memory_order_relaxed);
    }
    bool gamepadConnected() { return g_padConnected.load(std::memory_order_relaxed); }
    bool gamepadButton(GamepadButton button) { return g_padButtons[button].load(std::memory_order_relaxed); }
    float gamepadAxis(GamepadAxis axis) { return g_padAxes[axis].load(std::memory_order_relaxed); }

    GamepadFamily gamepadFamily() { return static_cast<GamepadFamily>(g_padFamily.load(std::memory_order_relaxed)); }

    const char *gamepadName()
    {
        const char *name = g_gamepad ? SDL_GetGamepadName(g_gamepad) : nullptr;
        return name ? name : "";
    }

    void gamepadRumble(float low, float high)
    {
        auto level = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 65535.0f + 0.5f); };
        g_rumbleWanted.store(level(low) << 16 | level(high), std::memory_order_relaxed);
    }

    DsPadFamily dsPadFamily() { return static_cast<DsPadFamily>(g_dsPadFamily.load(std::memory_order_relaxed)); }
    bool gamepadHasLED() { return g_padHasLED.load(std::memory_order_relaxed); }
    bool gamepadHasTriggerEffects() { return g_padHasTriggerFx.load(std::memory_order_relaxed); }

    bool gamepadSetLED(uint8_t red, uint8_t green, uint8_t blue)
    {
        return g_gamepad && SDL_SetGamepadLED(g_gamepad, red, green, blue);
    }

    bool gamepadSendEffect(const void *data, int size)
    {
        return g_gamepad && data && size > 0 && SDL_SendGamepadEffect(g_gamepad, data, size);
    }

    void setMouseCaptured(bool on) { g_wantCapture = on && !g_hiddenWindow; }
    bool mouseCaptured() { return g_captured; }

    void takeMouseMotion(float &dx, float &dy)
    {
        std::lock_guard<std::mutex> lock(g_motionMutex);
        dx = g_motionX;
        dy = g_motionY;
        g_motionX = g_motionY = 0.0f;
    }

    const char *keyName(int key)
    {
        static thread_local char buffer[16];
        if ((key >= KEY_A && key <= KEY_Z) || (key >= KEY_ZERO && key <= KEY_NINE))
            return (buffer[0] = static_cast<char>(key), buffer[1] = 0, buffer);
        if (key >= KEY_F1 && key <= KEY_F12)
            return (std::snprintf(buffer, sizeof(buffer), "F%d", key - KEY_F1 + 1), buffer);
        if (key >= KEY_KP_0 && key <= KEY_KP_9)
            return (std::snprintf(buffer, sizeof(buffer), "Keypad %d", key - KEY_KP_0), buffer);
        switch (key)
        {
        case KEY_SPACE: return "Space";
        case KEY_GRAVE: return "`";
        case KEY_MINUS: return "-";
        case KEY_EQUAL: return "=";
        case KEY_LEFT_BRACKET: return "[";
        case KEY_RIGHT_BRACKET: return "]";
        case KEY_BACKSLASH: return "\\";
        case KEY_SEMICOLON: return ";";
        case KEY_APOSTROPHE: return "'";
        case KEY_COMMA: return ",";
        case KEY_PERIOD: return ".";
        case KEY_SLASH: return "/";
        case KEY_ESCAPE: return "Escape";
        case KEY_ENTER: return "Enter";
        case KEY_TAB: return "Tab";
        case KEY_BACKSPACE: return "Backspace";
        case KEY_INSERT: return "Insert";
        case KEY_DELETE: return "Delete";
        case KEY_RIGHT: return "Right";
        case KEY_LEFT: return "Left";
        case KEY_DOWN: return "Down";
        case KEY_UP: return "Up";
        case KEY_HOME: return "Home";
        case KEY_END: return "End";
        case KEY_PAGE_UP: return "Page Up";
        case KEY_PAGE_DOWN: return "Page Down";
        case KEY_CAPS_LOCK: return "Caps Lock";
        case KEY_KP_DECIMAL: return "Keypad .";
        case KEY_KP_DIVIDE: return "Keypad /";
        case KEY_KP_MULTIPLY: return "Keypad *";
        case KEY_KP_SUBTRACT: return "Keypad -";
        case KEY_KP_ADD: return "Keypad +";
        case KEY_KP_ENTER: return "Keypad Enter";
        case KEY_LEFT_SHIFT: return "Shift"; // control bindings take either side (game/vcs_input.cpp)
        case KEY_LEFT_CONTROL: return "Ctrl";
        case KEY_LEFT_ALT: return "Alt";
        case KEY_RIGHT_SHIFT: return "Right Shift";
        case KEY_RIGHT_CONTROL: return "Right Ctrl";
        case KEY_RIGHT_ALT: return "Right Alt";
        case KEY_MOUSE_LEFT: return "Left mouse";
        case KEY_MOUSE_RIGHT: return "Right mouse";
        case KEY_MOUSE_MIDDLE: return "Middle mouse";
        case KEY_MOUSE_X1: return "Mouse 4";
        case KEY_MOUSE_X2: return "Mouse 5";
        case KEY_WHEEL_UP: return "Wheel up";
        case KEY_WHEEL_DOWN: return "Wheel down";
        default: return "?";
        }
    }

    bool audioAvailable() { return (SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) != 0; }

    namespace
    {
        // The playback device whose name contains g_audioDeviceWant (case-insensitive), else the system default.
        SDL_AudioDeviceID resolveAudioDevice(std::string &nameOut)
        {
            nameOut.clear();
            if (g_audioDeviceWant.empty())
                return SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
            auto lower = [](std::string s) { for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; };
            const std::string want = lower(g_audioDeviceWant);
            int count = 0;
            SDL_AudioDeviceID chosen = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
            if (SDL_AudioDeviceID *ids = SDL_GetAudioPlaybackDevices(&count))
            {
                for (int i = 0; i < count; ++i)
                {
                    const char *n = SDL_GetAudioDeviceName(ids[i]);
                    if (n && lower(n).find(want) != std::string::npos)
                    {
                        chosen = ids[i];
                        nameOut = n;
                        break;
                    }
                }
                SDL_free(ids);
            }
            return chosen;
        }

        // Opens g_audio on the resolved device with the stored format. The caller sets g_audioRate / BufferFrames / Fill.
        bool openAudioStream()
        {
            if (g_audio || !audioAvailable() || !g_audioFill)
                return false;
            char frames[16];
            std::snprintf(frames, sizeof(frames), "%u", g_audioBufferFrames);
            SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, frames);
            SDL_AudioSpec spec{SDL_AUDIO_S16, 2, static_cast<int>(g_audioRate)};
            std::string name;
            const SDL_AudioDeviceID device = resolveAudioDevice(name);
            g_audio = SDL_OpenAudioDeviceStream(device, &spec, audioCallback, nullptr);
            if (!g_audio)
            {
                std::fprintf(stderr, "[platform] audio device: %s\n", SDL_GetError());
                return false;
            }
            g_audioDeviceName = name;
            SDL_ResumeAudioStreamDevice(g_audio);
            return true;
        }
    }

    bool audioOpen(uint32_t rate, uint32_t bufferFrames, AudioFill fill)
    {
        if (g_audio || !audioAvailable())
            return false;
        g_audioRate = rate;
        g_audioBufferFrames = bufferFrames;
        g_audioFill = fill;
        return openAudioStream();
    }

    void audioClose()
    {
        if (g_audio)
            SDL_DestroyAudioStream(g_audio);
        g_audio = nullptr;
    }

    bool audioUseDevice(const char *nameSubstr)
    {
        const std::string want = nameSubstr ? nameSubstr : "";
        if (want == g_audioDeviceWant)
            return g_audio != nullptr; // already on it
        g_audioDeviceWant = want;
        if (!g_audioFill) // audio not opened yet: openAudioStream will honour the wish later
            return false;
        const bool was = g_audio != nullptr;
        audioClose();
        const bool ok = openAudioStream();
        std::fprintf(stderr, "[platform] audio -> %s\n", g_audioDeviceName.empty() ? "default device" : g_audioDeviceName.c_str());
        return ok || !was;
    }

    const char *audioDeviceName() { return g_audioDeviceName.c_str(); }

    bool writePng(const char *path, const uint8_t *rgb, int width, int height)
    {
        // Each row with the PNG filter (none / sub / up / paeth) whose bytes sum smallest as signed values - the
        // usual heuristic - then zlib. A 1080p screenshot was 6.2 MB with stored blocks.
        const size_t stride = static_cast<size_t>(width) * 3u;
        std::vector<uint8_t> raw(static_cast<size_t>(height) * (stride + 1u));
        std::vector<uint8_t> trial(stride);
        for (int y = 0; y < height; ++y)
        {
            const uint8_t *row = rgb + static_cast<size_t>(y) * stride;
            const uint8_t *above = y ? row - stride : nullptr;
            uint8_t *out = raw.data() + static_cast<size_t>(y) * (stride + 1u);
            uint64_t best = ~0ull;
            for (uint8_t filter = 0; filter < 5; ++filter)
            {
                if (filter == 3)
                    continue; // average: rarely better here
                uint64_t cost = 0;
                for (size_t i = 0; i < stride; ++i)
                {
                    const int a = i >= 3 ? row[i - 3] : 0, b = above ? above[i] : 0, c = (i >= 3 && above) ? above[i - 3] : 0;
                    int predicted = 0;
                    if (filter == 1)
                        predicted = a;
                    else if (filter == 2)
                        predicted = b;
                    else if (filter == 4)
                    {
                        const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                        predicted = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                    }
                    trial[i] = static_cast<uint8_t>(row[i] - predicted);
                    cost += static_cast<uint64_t>(std::abs(static_cast<int8_t>(trial[i])));
                }
                if (cost < best)
                {
                    best = cost;
                    out[0] = filter;
                    std::memcpy(out + 1, trial.data(), stride);
                }
            }
        }
        uLongf zSize = compressBound(static_cast<uLong>(raw.size()));
        std::vector<uint8_t> z(zSize);
        if (compress2(z.data(), &zSize, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK)
            return false;
        z.resize(zSize);

        std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        std::vector<uint8_t> ihdr;
        put32(ihdr, static_cast<uint32_t>(width));
        put32(ihdr, static_cast<uint32_t>(height));
        ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0}); // 8-bit RGB
        chunk(png, "IHDR", ihdr);
        chunk(png, "IDAT", z);
        chunk(png, "IEND", {});
        FILE *f = std::fopen(path, "wb");
        if (!f)
            return false;
        const bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
        return std::fclose(f) == 0 && ok;
    }
}
