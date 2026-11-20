// Shows the game's picture in the window through the RHI (stage 2 of docs/NATIVE_RENDERER.md): the swapchain, the
// letterboxed picture, Dear ImGui on top, and off-screen screenshots. Main thread only.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace rhi
{
    class Device;
    class Texture;
}

namespace present
{
    // A picture to show: made on the CPU (software GS, paraLLEl-GS readback) or a renderer's own texture.
    struct Picture
    {
        std::shared_ptr<rhi::Texture> texture;
        int width = 0;
        int height = 0;
        bool linear = false; // bilinear filtering when scaled
    };

    // The renderer (RHI backend) to start with: VCS_RHI=d3d11|vulkan|null for one run, else the Graphics > Renderer
    // setting (setBackend, before init), else rhi::defaultBackend(). One the build lacks falls back to the default.
    void setBackend(int backend); // rhi::Backend
    int backend();                // rhi::Backend init uses (or used)
    bool backendNeedsVulkanWindow(); // platform::WindowDesc::vulkan
    // The graphics card on a PC with more than one (Graphics > Graphics card; before init): an rhi::Gpu id.
    void setGpu(int id);

    // On the platform layer's window (platform::init first).
    bool init(int width, int height);
    void shutdown();
    rhi::Device *device();

    Picture createPicture(int width, int height, bool linear);
    void updatePicture(const Picture &picture, const void *rgba); // whole picture, rows top-down, tightly packed

    // Per host frame: beginFrame() before platform::beginFrame() (Dear ImGui renderer), drawWindow() after
    // platform::endUiFrame(), then optional captures, then present().
    void beginFrame();
    // The window's back buffer (resized to windowWidth x windowHeight): black, the top-left src part of the picture
    // letterboxed at display aspect `aspect` (width / height) inside the game's frame (fitRect), the Dear ImGui output
    // on top.
    void drawWindow(const Picture &picture, int srcWidth, int srcHeight, float aspect, int windowWidth, int windowHeight,
                    float frameAspect = 0.0f);
    void present();
    void setFrameInputTime(int64_t steadyNs); // the shown frame's input snapshot (rhi::Device::setFrameInputTime)
    // How present() shows frames (the Display > Vertical sync setting; main thread).
    void setPresentMode(int mode); // rhi::Device::PresentMode: 0 Immediate, 1 Vsync, 2 Tearing

    // Post-processing (opt-in enhancement): exposure percent (100 = original) applied as a full-screen pass.
    void setPostProcessEnabled(bool on);
    void setExposurePercent(int percent);
    bool postProcessEnabled();
    int exposurePercent();

    // The same picture and Dear ImGui output (scaled) rendered off-screen at width x height; RGB rows top-down.
    bool capture(const Picture &picture, int srcWidth, int srcHeight, float aspect, int width, int height, std::vector<uint8_t> &rgb,
                 float frameAspect = 0.0f);

    // Where a picture of display aspect `aspect` goes in a width x height target: as large as fits inside the game's
    // frame - the target letterboxed at frameAspect (0: the whole target) - and centred. A 4:3 menu in a 32:9 game on a
    // 16:9 monitor sits in the 32:9 strip, between the bars, not over them.
    void fitRect(float aspect, float frameAspect, float width, float height, float &x0, float &y0, float &w, float &h);

    // Brightness probe (automation rig, docs/NATIVE_PORT_ROADMAP.md milestone 1): the picture's middle region
    // (HUD margins left out) shrunk to 32x24 and read back; mean luma 0..255. Stalls the GPU briefly: opt-in.
    bool frameLuma(const Picture &picture, int srcWidth, int srcHeight, float &mean);

    // Display aspect of the game picture (docs/SETTINGS_MENU_WIDESCREEN.md): 4:3 as on a PS2's TV, or the widescreen
    // ratio. displayAspect(false) is for frames without 3D (loading screens, menus): always 4:3. Any thread.
    void setDisplayAspect(float aspect);
    float displayAspect(bool showing3d);
}
