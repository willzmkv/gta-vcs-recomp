// Null backend of rhi.h (VCS_RHI=null): every operation is accepted and does nothing; readbacks give zeros. The CPU
// side of the game runs as with a real renderer, so it can be brought up and checked on a machine (or a build) without
// one, and renderer bugs stay apart from the rest (docs/LINUX.md).
#include "rhi/rhi.h"

#include "imgui.h"

#include <cstring>

namespace rhi
{
    namespace
    {
        class NullTexture final : public Texture
        {
        public:
            explicit NullTexture(const TextureDesc &desc) { m_desc = desc; }
        };

        class NullBuffer final : public Buffer
        {
        public:
            explicit NullBuffer(const BufferDesc &desc) { m_desc = desc; }
        };

        class NullSampler final : public Sampler
        {
        };

        class NullPipeline final : public Pipeline
        {
        };

        class NullDevice final : public Device
        {
        public:
            explicit NullDevice(const DeviceDesc &desc) : m_width(desc.width), m_height(desc.height) {}

            Backend backend() const override { return Backend::Null; }
            const std::string &adapterName() const override { return m_name; }

            std::unique_ptr<Texture> createTexture(const TextureDesc &desc, const void *, uint32_t) override
            {
                return std::make_unique<NullTexture>(desc);
            }
            void updateTexture(Texture &, uint32_t, uint32_t, uint32_t, uint32_t, const void *, uint32_t) override {}
            bool readTexture(Texture &texture, uint32_t, uint32_t, uint32_t width, uint32_t height, void *out,
                             uint32_t rowPitch) override
            {
                const uint32_t bpp = texture.desc().format == Format::RGBA8 || texture.desc().format == Format::R32F ? 4 : 0;
                if (!bpp || !out)
                    return false;
                const uint32_t pitch = rowPitch ? rowPitch : width * bpp;
                for (uint32_t y = 0; y < height; ++y)
                    std::memset(static_cast<uint8_t *>(out) + static_cast<size_t>(y) * pitch, 0, static_cast<size_t>(width) * bpp);
                return true;
            }
            void copyTexture(Texture &, uint32_t, uint32_t, Texture &, uint32_t, uint32_t, uint32_t, uint32_t) override {}
            void resolveTexture(Texture &, Texture &) override {}
            void generateMips(Texture &) override {}
            std::unique_ptr<Texture> createTextureLevels(const TextureDesc &desc, const void *const *levels, uint32_t) override
            {
                return createTexture(desc, levels ? levels[0] : nullptr, 0);
            }

            std::unique_ptr<Buffer> createBuffer(const BufferDesc &desc, const void *) override
            {
                return std::make_unique<NullBuffer>(desc);
            }
            void updateBuffer(Buffer &, const void *, size_t) override {}

            std::unique_ptr<Sampler> createSampler(const SamplerDesc &) override { return std::make_unique<NullSampler>(); }
            std::unique_ptr<Pipeline> createPipeline(const PipelineDesc &) override { return std::make_unique<NullPipeline>(); }

            void beginPass(const PassDesc &) override {}
            void endPass() override {}
            void setViewport(float, float, float, float) override {}
            void setScissor(int, int, int, int) override {}
            void setBlendConstant(const float[4]) override {}
            void setPipeline(Pipeline &) override {}
            void setVertexBuffer(Buffer &, uint32_t) override {}
            void setIndexBuffer(Buffer &, bool) override {}
            void setTexture(uint32_t, Texture *, Sampler *) override {}
            void setConstants(uint32_t, Buffer &) override {}
            void draw(uint32_t, uint32_t) override {}
            void drawInstanced(uint32_t, uint32_t, uint32_t) override {}
            void drawIndexed(uint32_t, uint32_t, int32_t) override {}
            void pushMarker(const char *) override {}
            void popMarker() override {}

            void backbufferSize(uint32_t &width, uint32_t &height) const override
            {
                width = m_width;
                height = m_height;
            }
            void resize(uint32_t width, uint32_t height) override
            {
                m_width = width;
                m_height = height;
            }
            void present(PresentMode) override {}

            // Dear ImGui 1.92 asks the renderer to create / update / destroy its textures (the font atlas): taken as done.
            bool uiInit() override
            {
                ImGuiIO &io = ImGui::GetIO();
                io.BackendRendererName = "vcs_rhi_null";
                io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
                return true;
            }
            void uiNewFrame() override {}
            void uiRender(void *imDrawData) override
            {
                const ImDrawData *data = static_cast<const ImDrawData *>(imDrawData);
                if (!data || !data->Textures)
                    return;
                for (ImTextureData *t : *data->Textures)
                {
                    if (t->Status == ImTextureStatus_WantCreate || t->Status == ImTextureStatus_WantUpdates)
                    {
                        t->SetTexID(static_cast<ImTextureID>(1));
                        t->SetStatus(ImTextureStatus_OK);
                    }
                    else if (t->Status == ImTextureStatus_WantDestroy && t->UnusedFrames > 0)
                    {
                        t->SetTexID(ImTextureID_Invalid);
                        t->SetStatus(ImTextureStatus_Destroyed);
                    }
                }
            }
            void uiShutdown() override {}

        private:
            std::string m_name = "none (null renderer)";
            uint32_t m_width = 0, m_height = 0;
        };
    }

    std::unique_ptr<Device> createNullDevice(const DeviceDesc &desc, std::string &)
    {
        return std::make_unique<NullDevice>(desc);
    }
}
