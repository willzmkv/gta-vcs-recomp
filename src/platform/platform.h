// Host platform layer: window, events, keyboard / mouse / gamepad, audio output, frame pacing and the Dear ImGui
// frame. The runtime and the renderer talk to the host only through this header (docs/NATIVE_RENDERER.md);
// platform_sdl3.cpp implements it with SDL3. No SDL, windows.h or graphics-API types appear here.
//
// Threads: init / shutdown / pumpEvents / the frame calls and keyPressed belong to the main thread. The input
// queries (keyDown, mouseDown, gamepad*) read a snapshot taken by pumpEvents and may be called from any thread
// (the game thread reads the pad).
#pragma once

#include <cstdint>
#include <string>

namespace platform
{
    // Key codes: the values raylib / GLFW use, so existing call sites and the control channel keep their numbers.
    enum Key : int
    {
        KEY_NULL = 0,
        KEY_SPACE = 32,
        KEY_APOSTROPHE = 39,
        KEY_COMMA = 44,
        KEY_MINUS = 45,
        KEY_PERIOD = 46,
        KEY_SLASH = 47,
        KEY_ZERO = 48, KEY_ONE, KEY_TWO, KEY_THREE, KEY_FOUR, KEY_FIVE, KEY_SIX, KEY_SEVEN, KEY_EIGHT, KEY_NINE,
        KEY_SEMICOLON = 59,
        KEY_EQUAL = 61,
        KEY_A = 65, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O,
        KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
        KEY_LEFT_BRACKET = 91,
        KEY_BACKSLASH = 92,
        KEY_RIGHT_BRACKET = 93,
        KEY_GRAVE = 96,
        KEY_ESCAPE = 256,
        KEY_ENTER = 257,
        KEY_TAB = 258,
        KEY_BACKSPACE = 259,
        KEY_INSERT = 260,
        KEY_DELETE = 261,
        KEY_RIGHT = 262,
        KEY_LEFT = 263,
        KEY_DOWN = 264,
        KEY_UP = 265,
        KEY_PAGE_UP = 266,
        KEY_PAGE_DOWN = 267,
        KEY_HOME = 268,
        KEY_END = 269,
        KEY_CAPS_LOCK = 280,
        KEY_F1 = 290, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
        KEY_KP_0 = 320, KEY_KP_1, KEY_KP_2, KEY_KP_3, KEY_KP_4, KEY_KP_5, KEY_KP_6, KEY_KP_7, KEY_KP_8, KEY_KP_9,
        KEY_KP_DECIMAL = 330,
        KEY_KP_DIVIDE = 331,
        KEY_KP_MULTIPLY = 332,
        KEY_KP_SUBTRACT = 333,
        KEY_KP_ADD = 334,
        KEY_KP_ENTER = 335,
        KEY_LEFT_SHIFT = 340,
        KEY_LEFT_CONTROL = 341,
        KEY_LEFT_ALT = 342,
        KEY_RIGHT_SHIFT = 344,
        KEY_RIGHT_CONTROL = 345,
        KEY_RIGHT_ALT = 346,
        // Mouse buttons and wheel notches as keys, so a control binding can be either (keyDown works for them; a wheel
        // notch is a short press: down 50 ms, then up 50 ms before the next one).
        KEY_MOUSE_LEFT = 400,
        KEY_MOUSE_RIGHT = 401,
        KEY_MOUSE_MIDDLE = 402,
        KEY_MOUSE_X1 = 403,
        KEY_MOUSE_X2 = 404,
        KEY_WHEEL_UP = 405,
        KEY_WHEEL_DOWN = 406,
        KEY_COUNT = 512,
    };

    enum MouseButton : int
    {
        MOUSE_LEFT,
        MOUSE_RIGHT,
        MOUSE_MIDDLE,
        MOUSE_BUTTON_COUNT,
    };

    // Positions, not labels: SOUTH = Cross / A, EAST = Circle / B, WEST = Square / X, NORTH = Triangle / Y.
    enum GamepadButton : int
    {
        GAMEPAD_SOUTH,
        GAMEPAD_EAST,
        GAMEPAD_WEST,
        GAMEPAD_NORTH,
        GAMEPAD_BACK,
        GAMEPAD_START,
        GAMEPAD_LEFT_STICK,
        GAMEPAD_RIGHT_STICK,
        GAMEPAD_LEFT_SHOULDER,
        GAMEPAD_RIGHT_SHOULDER,
        GAMEPAD_DPAD_UP,
        GAMEPAD_DPAD_DOWN,
        GAMEPAD_DPAD_LEFT,
        GAMEPAD_DPAD_RIGHT,
        GAMEPAD_BUTTON_COUNT,
    };

    // Sticks -1..1 (down / right positive), triggers 0..1.
    enum GamepadAxis : int
    {
        AXIS_LEFT_X,
        AXIS_LEFT_Y,
        AXIS_RIGHT_X,
        AXIS_RIGHT_Y,
        AXIS_LEFT_TRIGGER,
        AXIS_RIGHT_TRIGGER,
        AXIS_COUNT,
    };

    struct WindowDesc
    {
        const char *title = "";
        int width = 640;
        int height = 448;
        bool hidden = false; // not on the desktop or taskbar (scripted test runs)
        bool vulkan = false; // for a Vulkan renderer (the surface comes from vulkanCreateSurface)
    };

    // Where the window was on the desktop, to open it there again (main.cpp keeps it in vcs.ini as window.*).
    // x / y / width / height: the restored (not maximized) client area, in screen coordinates relative to the
    // top-left of the monitor it is on. The monitor is found again by name, and by its desktop origin between two of
    // the same model; when it is gone the window opens at the default place.
    struct WindowPlacement
    {
        std::string display;            // monitor name
        int displayX = 0, displayY = 0; // that monitor's desktop origin
        int x = 0, y = 0, width = 0, height = 0;
        bool maximized = false;
    };
    using PlacementChanged = void (*)(const WindowPlacement &placement);

    // Before init: open the window at `placement` instead of centred on the primary monitor at the default size.
    void setStartPlacement(const WindowPlacement &placement);
    // Called on the main thread once the window has settled after a move / resize / maximize, and at shutdown. Not
    // used for hidden windows (they are neither placed nor remembered).
    void onPlacementChanged(PlacementChanged callback);

    // Main thread. Full screen: borderless at the desktop's resolution on the monitor the window is on (no display mode
    // change, so Alt+Tab and other monitors stay as they are); off: the window as it was before. Before init: how the
    // window opens. A hidden window only remembers it. In full screen the mouse cursor hides after 2 s without moving.
    void setFullscreen(bool on);
    bool fullscreen();

    // Main thread. Creates the window (no graphics API attached: the renderer makes its swapchain on
    // nativeWindow()) and the Dear ImGui context with its input backend; the ImGui renderer belongs to the RHI.
    bool init(const WindowDesc &desc);
    void shutdown();
    bool ready();
    void *nativeWindow(); // HWND on Windows, null elsewhere

    bool windowHidden(); // made with WindowDesc::hidden

    // For the Vulkan renderer (rhi::VulkanWindow), on a window made with WindowDesc::vulkan. Main thread.
    const char *const *vulkanInstanceExtensions(uint32_t *count); // the instance extensions the surface needs
    bool vulkanCreateSurface(void *instance, uint64_t *surface);  // VkInstance -> VkSurfaceKHR

    // Main thread, once per host frame, in this order:
    //   pumpEvents()      events + input snapshot
    //   beginFrame()      new Dear ImGui frame (overlays / debug UI may draw from here on; the renderer's ImGui
    //                     backend starts its frame just before)
    //   endUiFrame()      Dear ImGui output finished (ImGui::GetDrawData), for the renderer to draw
    //   ...the renderer draws and presents...
    //   waitNextFrame()   sleeps until the next host frame (1/60 s unless setFrameRate says otherwise)
    // frameStart false: an input refresh between frames (frame lead): the snapshot is new, keyPressed edges are kept for
    // the next frame-start pump, which reports every press since the last one
    void pumpEvents(bool frameStart = true);
    // When the input snapshot the queries read was taken (std::chrono::steady_clock, ns): the start of a frame's input
    // delay (any thread).
    int64_t inputSnapshotNs();
    bool quitRequested(); // window closed
    void beginFrame();
    void endUiFrame();
    void waitNextFrame();
    // Frame lead (main thread): waitNextFrame wakes `leadNs` before the next host frame and calls `atLead` (an input
    // refresh and the start of the game's next frame, so it is ready by then: docs/PERFORMANCE.md 19); 0 = none.
    // frameLeadRan(): whether the last waitNextFrame did (then the frame start need not start the game's frame).
    using FrameLead = void (*)();
    void setFrameLead(uint64_t leadNs, FrameLead atLead);
    bool frameLeadRan();
    bool frameSlept(); // the last waitNextFrame waited for its time (false: the host frame took the whole period or more)
    void setFrameRate(double hz); // host frames per second for waitNextFrame (any thread; 60 by default)
    double displayRefreshHz();    // the window's monitor's refresh rate (60 when unknown)
    void delayPrecise(uint64_t ns); // sleep with sub-millisecond precision (any thread)

    void windowSize(int &width, int &height); // drawable pixels
    bool windowFocused(); // the window has the keyboard focus (main thread; false for a hidden window)
    // Shows a folder in the system's file manager (Explorer; the desktop's default on Linux). `utf8Path`: absolute.
    bool openFolder(const std::string &utf8Path);

    // Input (any thread; snapshot from the last pumpEvents).
    bool keyDown(int key);
    bool keyPressed(int key); // main thread: went down at the last pumpEvents
    bool mouseDown(MouseButton button);
    // The cursor in drawable pixels from the window's top-left (windowSize's units); false while it is not over the
    // window, the mouse is captured or the window is hidden.
    bool mousePosition(float &x, float &y);
    bool gamepadConnected();
    bool gamepadButton(GamepadButton button);
    float gamepadAxis(GamepadAxis axis);
    const char *gamepadName(); // "" without a gamepad (main thread)
    // What kind of gamepad it is, from its maker's layout (any thread; None without one): the button prompts' glyphs.
    enum class GamepadFamily : int
    {
        None,
        Xbox,
        PlayStation,
        Nintendo,
        Other,
    };
    GamepadFamily gamepadFamily();
    // Vibration, motors 0..1 (low = the large, slow motor; high = the small, fast one). Any thread; applied at the next
    // pumpEvents and held until changed (0, 0 stops).
    void gamepadRumble(float low, float high);

    // The connected pad's family for the DualSense features (PS5 adaptive triggers, PS4 / PS5 light bar). Separate from
    // GamepadFamily above (used for prompt glyphs) because it tells PS4 from PS5. Snapshot (any thread).
    enum DsPadFamily : int
    {
        GAMEPAD_FAMILY_GENERIC, // an Xbox-style or unknown pad
        GAMEPAD_FAMILY_PS4,     // DualShock 4 (a light bar, no adaptive triggers)
        GAMEPAD_FAMILY_PS5,     // DualSense (a light bar and adaptive triggers)
    };
    DsPadFamily dsPadFamily();
    bool gamepadHasLED();            // an RGB light bar (DualShock 4 / DualSense)
    bool gamepadHasTriggerEffects(); // adaptive triggers through gamepadSendEffect (DualSense)

    // The light bar's colour, 0..255 (DualShock 4 / DualSense); a no-op without one. Main thread, applied at once and
    // held until changed. False when the pad took none (e.g. a DualSense still enabling its enhanced reports).
    bool gamepadSetLED(uint8_t red, uint8_t green, uint8_t blue);
    // A raw gamepad effect packet (SDL_SendGamepadEffect): the DualSense output-report body (a DS5EffectsState_t) that
    // drives its adaptive triggers. Main thread; false when the pad takes none. The caller sets only the fields it
    // owns, so a triggers-only packet leaves the light bar and rumble alone.
    bool gamepadSendEffect(const void *data, int size);

    // Mouse capture (main thread): the cursor hidden and held in the window, motion reported as relative counts. Off
    // while the window is in the background.
    void setMouseCaptured(bool on);
    bool mouseCaptured();
    // Mouse motion since the last call, in mouse counts (any thread; one consumer).
    void takeMouseMotion(float &dx, float &dy);

    // A key's name for menus ("A", "F2", "Left Shift", "Mouse 4", "Wheel up"); "?" for codes without one.
    const char *keyName(int key);

    // Audio output: interleaved 16-bit stereo at `rate`, pulled by `fill` on the audio thread.
    using AudioFill = void (*)(int16_t *stereo, uint32_t frames);
    bool audioAvailable(); // a playback device exists
    bool audioOpen(uint32_t rate, uint32_t bufferFrames, AudioFill fill);
    void audioClose();
    // Route the game's sound to the playback device whose name contains `nameSubstr` (e.g. a DualSense's speaker), or
    // the system default when null / empty / no match. Main thread; reopens the stream (works before or after
    // audioOpen). Returns whether sound is playing somewhere after the call.
    bool audioUseDevice(const char *nameSubstr);
    const char *audioDeviceName(); // the output device the stream is on ("" = none / the default)

    // PNG (8-bit RGB, rows top-down) - screenshots.
    bool writePng(const char *path, const uint8_t *rgb, int width, int height);
}
