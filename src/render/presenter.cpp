// See presenter.h.
#include "render/presenter.h"

#include "platform/platform.h"
#include "render/gs_shaders.h"
#include "render/ui_renderer.h"
#include "rhi/rhi.h"

#include "imgui.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace present
{
    namespace
    {
        struct Params
        {
            float rect[4];  // NDC left, top, right, bottom
            float uvMax[2];
            float exposure; // 1.0 = original; post-process exposure multiplier
            float pad;
        };

        std::unique_ptr<rhi::Device> g_device;
        std::unique_ptr<rhi::Pipeline> g_pipeline;
        std::unique_ptr<rhi::Buffer> g_params;
        std::unique_ptr<rhi::Sampler> g_nearest, g_linear;
        std::unique_ptr<rhi::Texture> g_captureTarget;
        std::unique_ptr<rhi::Texture> g_lumaTarget; // frameLuma
        std::vector<uint8_t> g_captureRgba;
        // Dear ImGui: the backend's own renderer (D3D11) or render::UiRenderer through the RHI (Vulkan)
        std::unique_ptr<render::UiRenderer> g_ui;

        void uiRender(ImDrawData *data)
        {
            if (g_ui)
                g_ui->render(data);
            else
                g_device->uiRender(data);
        }

        std::atomic<float> g_displayAspect{4.0f / 3.0f};
        int g_backendSetting = -1; // rhi::Backend; -1: the default
        std::atomic<bool> g_postProcessEnabled{false};
        std::atomic<int> g_exposurePercent{100};

        rhi::Backend chooseBackend()
        {
            rhi::Backend b = rhi::defaultBackend();
            const char *env = std::getenv("VCS_RHI");
            if (env && *env)
            {
                if (!rhi::backendFromName(env, b))
                    std::fprintf(stderr, "[present] VCS_RHI=%s not understood (d3d11, vulkan or null)\n", env);
            }
            else if (g_backendSetting >= 0)
                b = static_cast<rhi::Backend>(g_backendSetting);
            if (!rhi::backendBuilt(b))
            {
                std::fprintf(stderr, "[present] the %s renderer is not part of this build: %s\n", rhi::backendName(b),
                             rhi::backendName(rhi::defaultBackend()));
                b = rhi::defaultBackend();
            }
            return b;
        }

        // The bound pass's target is targetWidth x targetHeight; the src region is shown at display aspect `aspect`
        // inside the game's frame (fitRect)
        void drawPicture(const Picture &picture, int srcWidth, int srcHeight, float aspect, int targetWidth, int targetHeight,
                         float frameAspect = 0.0f)
        {
            if (!picture.texture || srcWidth <= 0 || srcHeight <= 0 || targetWidth <= 0 || targetHeight <= 0 || aspect <= 0.0f)
                return;
            float x0 = 0.0f, y0 = 0.0f, w = 0.0f, h = 0.0f;
            fitRect(aspect, frameAspect, static_cast<float>(targetWidth), static_cast<float>(targetHeight), x0, y0, w, h);
            auto ndcX = [&](float px) { return px / targetWidth * 2.0f - 1.0f; };
            auto ndcY = [&](float py) { return 1.0f - py / targetHeight * 2.0f; };
            const rhi::TextureDesc &td = picture.texture->desc();
            const float exposure = g_postProcessEnabled.load(std::memory_order_relaxed)
                                       ? static_cast<float>(g_exposurePercent.load(std::memory_order_relaxed)) / 100.0f
                                       : 1.0f;
            const Params params{{ndcX(x0), ndcY(y0), ndcX(x0 + w), ndcY(y0 + h)},
                                {static_cast<float>(srcWidth) / td.width, static_cast<float>(srcHeight) / td.height},
                                exposure,
                                0.0f};
            g_device->updateBuffer(*g_params, &params, sizeof(params));
            g_device->setPipeline(*g_pipeline);
            g_device->setConstants(0, *g_params);
            g_device->setTexture(0, picture.texture.get(), picture.linear ? g_linear.get() : g_nearest.get());
            g_device->draw(4);
            g_device->setTexture(0, nullptr, nullptr);
        }
    }

    void setBackend(int backend) { g_backendSetting = backend; }

    int backend() { return static_cast<int>(g_device ? g_device->backend() : chooseBackend()); }

    bool backendNeedsVulkanWindow() { return chooseBackend() == rhi::Backend::Vulkan; }

    void setPostProcessEnabled(bool on) { g_postProcessEnabled.store(on, std::memory_order_relaxed); }
    void setExposurePercent(int percent) { g_exposurePercent.store(percent, std::memory_order_relaxed); }
    bool postProcessEnabled() { return g_postProcessEnabled.load(std::memory_order_relaxed); }
    int exposurePercent() { return g_exposurePercent.load(std::memory_order_relaxed); }

    bool init(int width, int height)
    {
        rhi::DeviceDesc desc;
        desc.backend = chooseBackend();
        desc.nativeWindow = platform::nativeWindow();
        desc.vulkan.instanceExtensions = platform::vulkanInstanceExtensions;
        desc.vulkan.createSurface = platform::vulkanCreateSurface;
        desc.offscreen = platform::windowHidden();
        desc.width = static_cast<uint32_t>(std::max(width, 1));
        desc.height = static_cast<uint32_t>(std::max(height, 1));
        // the API's validation layer: D3D11 debug layer (Graphics Tools), Vulkan's Khronos layer
        const char *debug = std::getenv("PS2X_RHI_DEBUG");
        if (!debug || !*debug)
            debug = std::getenv("PS2X_D3D_DEBUG");
        desc.debug = debug && *debug && *debug != '0';
        std::string error;
        g_device = rhi::createDevice(desc, error);
        if (!g_device && desc.backend == rhi::Backend::D3D12 && rhi::backendBuilt(rhi::Backend::D3D11))
        {
            // no D3D12 device (an old driver, a GPU without it): D3D11, the reference, for this run
            std::fprintf(stderr, "[present] D3D12: %s - D3D11 instead\n", error.c_str());
            desc.backend = rhi::Backend::D3D11;
            g_device = rhi::createDevice(desc, error);
        }
        if (!g_device)
        {
            std::fprintf(stderr, "[present] %s: %s\n", rhi::backendName(desc.backend), error.c_str());
            return false;
        }
        std::fprintf(stderr, "[present] %s on %s\n", rhi::backendName(g_device->backend()), g_device->adapterName().c_str());

        const render::GsShaders &shaders = render::gsShaders(g_device->backend());
        rhi::PipelineDesc pd;
        pd.vertex = shaders.presentVS;
        pd.pixel = shaders.presentPS;
        pd.topology = rhi::Topology::TriangleStrip;
        // alpha-blended over black, as raylib's DrawTexturePro did
        pd.blend.enable = true;
        pd.blend.srcColor = pd.blend.srcAlpha = rhi::BlendFactor::SrcAlpha;
        pd.blend.dstColor = pd.blend.dstAlpha = rhi::BlendFactor::InvSrcAlpha;
        pd.name = "present";
        g_pipeline = g_device->createPipeline(pd);
        g_params = g_device->createBuffer({sizeof(Params), rhi::BufferUsage::Constant, true, "present params"});
        g_nearest = g_device->createSampler({rhi::Filter::Nearest, rhi::Wrap::Clamp, rhi::Wrap::Clamp});
        g_linear = g_device->createSampler({rhi::Filter::Linear, rhi::Wrap::Clamp, rhi::Wrap::Clamp});
        if (!g_pipeline || !g_params || !g_nearest || !g_linear)
        {
            std::fprintf(stderr, "[present] could not create the present pipeline\n");
            return false;
        }
        if (!g_device->uiInit())
        {
            g_ui = std::make_unique<render::UiRenderer>();
            if (!g_ui->init(*g_device))
            {
                std::fprintf(stderr, "[present] Dear ImGui renderer failed\n");
                g_ui.reset();
            }
        }
        return true;
    }

    void shutdown()
    {
        if (g_ui)
            g_ui->shutdown();
        g_ui.reset();
        g_captureTarget.reset();
        g_lumaTarget.reset();
        g_pipeline.reset();
        g_params.reset();
        g_nearest.reset();
        g_linear.reset();
        g_device.reset();
    }

    rhi::Device *device() { return g_device.get(); }

    Picture createPicture(int width, int height, bool linear)
    {
        Picture p;
        rhi::TextureDesc desc;
        desc.width = static_cast<uint32_t>(std::max(width, 1));
        desc.height = static_cast<uint32_t>(std::max(height, 1));
        desc.usage = rhi::UsageSampled | rhi::UsageDynamic;
        desc.name = "picture";
        p.texture = g_device->createTexture(desc);
        p.width = width;
        p.height = height;
        p.linear = linear;
        return p;
    }

    void updatePicture(const Picture &picture, const void *rgba)
    {
        if (picture.texture)
            g_device->updateTexture(*picture.texture, 0, 0, static_cast<uint32_t>(picture.width), static_cast<uint32_t>(picture.height),
                                    rgba);
    }

    void beginFrame()
    {
        if (!g_ui)
            g_device->uiNewFrame();
    }

    void setDisplayAspect(float aspect) { g_displayAspect.store(aspect, std::memory_order_relaxed); }

    float displayAspect(bool showing3d) { return showing3d ? g_displayAspect.load(std::memory_order_relaxed) : 4.0f / 3.0f; }

    void fitRect(float aspect, float frameAspect, float width, float height, float &x0, float &y0, float &w, float &h)
    {
        // the frame: the target letterboxed at the game's aspect
        float fw = width, fh = height;
        if (frameAspect > 0.0f)
        {
            fh = fw / frameAspect;
            if (fh > height)
            {
                fh = height;
                fw = fh * frameAspect;
            }
        }
        // the picture: letterboxed in the frame
        w = fw;
        h = aspect > 0.0f ? w / aspect : fh;
        if (h > fh)
        {
            h = fh;
            w = h * aspect;
        }
        x0 = (width - w) * 0.5f;
        y0 = (height - h) * 0.5f;
    }

    void drawWindow(const Picture &picture, int srcWidth, int srcHeight, float aspect, int windowWidth, int windowHeight,
                    float frameAspect)
    {
        if (windowWidth > 0 && windowHeight > 0)
            g_device->resize(static_cast<uint32_t>(windowWidth), static_cast<uint32_t>(windowHeight));
        uint32_t w = 0, h = 0;
        g_device->backbufferSize(w, h);
        rhi::PassDesc pass;
        pass.clearColor = true;
        pass.name = "window";
        g_device->beginPass(pass);
        drawPicture(picture, srcWidth, srcHeight, aspect, static_cast<int>(w), static_cast<int>(h), frameAspect);
        uiRender(ImGui::GetDrawData());
        g_device->endPass();
    }

    rhi::Device::PresentMode g_presentMode = rhi::Device::PresentMode::Immediate;

    void setPresentMode(int mode) { g_presentMode = static_cast<rhi::Device::PresentMode>(mode); }

    void present() { g_device->present(g_presentMode); }

    void setFrameInputTime(int64_t steadyNs) { g_device->setFrameInputTime(steadyNs); }

    bool frameLuma(const Picture &picture, int srcWidth, int srcHeight, float &mean)
    {
        constexpr int kW = 32, kH = 24;
        if (!picture.texture || srcWidth <= 0 || srcHeight <= 0)
            return false;
        if (!g_lumaTarget)
        {
            rhi::TextureDesc desc;
            desc.width = kW;
            desc.height = kH;
            desc.usage = rhi::UsageRenderTarget | rhi::UsageSampled;
            desc.name = "luma probe";
            g_lumaTarget = g_device->createTexture(desc);
            if (!g_lumaTarget)
                return false;
        }
        rhi::PassDesc pass;
        pass.color = g_lumaTarget.get();
        pass.clearColor = true;
        pass.name = "luma probe";
        g_device->beginPass(pass);
        drawPicture(picture, srcWidth, srcHeight, static_cast<float>(kW) / kH, kW, kH);
        g_device->endPass();
        uint8_t rgba[kW * kH * 4];
        if (!g_device->readTexture(*g_lumaTarget, 0, 0, kW, kH, rgba))
            return false;
        // the middle cells only (20%..80% across, 25%..80% down): the HUD stays out
        double sum = 0.0;
        int n = 0;
        for (int y = kH / 4; y < kH * 4 / 5; ++y)
            for (int x = kW / 5; x < kW * 4 / 5; ++x, ++n)
            {
                const uint8_t *px = rgba + (y * kW + x) * 4;
                sum += 0.299 * px[0] + 0.587 * px[1] + 0.114 * px[2];
            }
        mean = static_cast<float>(sum / n);
        return true;
    }

    bool capture(const Picture &picture, int srcWidth, int srcHeight, float aspect, int width, int height, std::vector<uint8_t> &rgb,
                 float frameAspect)
    {
        if (width <= 0 || height <= 0)
            return false;
        if (!g_captureTarget || g_captureTarget->desc().width != static_cast<uint32_t>(width) ||
            g_captureTarget->desc().height != static_cast<uint32_t>(height))
        {
            rhi::TextureDesc desc;
            desc.width = static_cast<uint32_t>(width);
            desc.height = static_cast<uint32_t>(height);
            desc.usage = rhi::UsageRenderTarget | rhi::UsageSampled;
            desc.name = "capture";
            g_captureTarget = g_device->createTexture(desc);
            if (!g_captureTarget)
                return false;
        }
        rhi::PassDesc pass;
        pass.color = g_captureTarget.get();
        pass.clearColor = true;
        pass.name = "capture";
        g_device->beginPass(pass);
        drawPicture(picture, srcWidth, srcHeight, aspect, width, height, frameAspect);
        if (ImDrawData *data = ImGui::GetDrawData(); data && data->Valid && data->DisplaySize.x > 0.0f && data->DisplaySize.y > 0.0f)
        {
            ImDrawData scaled = *data; // the window's UI, stretched to the capture size
            scaled.FramebufferScale = ImVec2(width / data->DisplaySize.x, height / data->DisplaySize.y);
            uiRender(&scaled);
        }
        g_device->endPass();
        g_captureRgba.resize(static_cast<size_t>(width) * height * 4);
        if (!g_device->readTexture(*g_captureTarget, 0, 0, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                                   g_captureRgba.data()))
            return false;
        rgb.resize(static_cast<size_t>(width) * height * 3);
        for (size_t i = 0, n = static_cast<size_t>(width) * height; i < n; ++i)
        {
            rgb[i * 3 + 0] = g_captureRgba[i * 4 + 0];
            rgb[i * 3 + 1] = g_captureRgba[i * 4 + 1];
            rgb[i * 3 + 2] = g_captureRgba[i * 4 + 2];
        }
        return true;
    }
}
