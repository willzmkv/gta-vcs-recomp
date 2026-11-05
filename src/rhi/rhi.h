// Render hardware interface (docs/NATIVE_RENDERER.md): the small set of GPU operations the renderer needs, so the
// renderer never sees a graphics API. One backend per API: D3D11 (rhi/d3d11, Windows), D3D12 (rhi/d3d12, Windows: the
// lower per-draw cost), Vulkan (rhi/vulkan, Linux, and Windows to compare) and null (rhi/null, no GPU work). Immutable pipelines, explicit passes, copies and readbacks as
// plain operations.
//
// Threading: one device, used from one thread (the main thread). No API types appear in this header.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rhi
{
    enum class Format : uint8_t
    {
        Unknown,
        RGBA8,  // 8-bit unorm, R in the lowest byte
        R32F,
        D32F,   // depth
        BGRA8,  // 8-bit unorm, B in the lowest byte (sampled only, createTextureLevels: texture packs)
        // block-compressed unorm, 4x4 texels a block (sampled only, createTextureLevels: texture packs,
        // docs/TEXTURE_PACKS.md); Device::blockCompression() says whether they can be made
        BC1,
        BC2,
        BC3,
        BC4,
        BC5,
        BC7,
    };

    constexpr bool blockCompressed(Format f) { return f >= Format::BC1 && f <= Format::BC7; }
    // a level's rows as stored: rows of texels, or of 4x4 blocks (BC1 / BC4 8 bytes a block, the others 16)
    constexpr uint32_t rowBytes(Format f, uint32_t width)
    {
        return blockCompressed(f) ? ((width + 3u) / 4u) * (f == Format::BC1 || f == Format::BC4 ? 8u : 16u) : width * 4u;
    }
    constexpr uint32_t rowCount(Format f, uint32_t height) { return blockCompressed(f) ? (height + 3u) / 4u : height; }

    enum TextureUsage : uint32_t
    {
        UsageSampled = 1u << 0,
        UsageRenderTarget = 1u << 1,
        UsageDepth = 1u << 2,
        UsageDynamic = 1u << 3, // rewritten by the CPU often (whole texture)
    };

    struct TextureDesc
    {
        uint32_t width = 0;
        uint32_t height = 0;
        Format format = Format::RGBA8;
        uint32_t usage = UsageSampled;
        const char *name = nullptr; // debug name
        uint32_t samples = 1;       // > 1: multisampled render target / depth (not sampled: resolveTexture first)
        uint32_t mipLevels = 1;     // 0: the full chain, made from level 0 by generateMips (sampled + render target)
    };

    class Texture
    {
    public:
        virtual ~Texture() = default;
        const TextureDesc &desc() const { return m_desc; }

    protected:
        TextureDesc m_desc;
    };

    enum class BufferUsage : uint8_t
    {
        Vertex,
        Index,
        Constant,
    };

    struct BufferDesc
    {
        size_t size = 0;
        BufferUsage usage = BufferUsage::Vertex;
        bool dynamic = true; // rewritten by the CPU (whole buffer per update)
        const char *name = nullptr;
    };

    class Buffer
    {
    public:
        virtual ~Buffer() = default;
        const BufferDesc &desc() const { return m_desc; }

    protected:
        BufferDesc m_desc;
    };

    enum class Filter : uint8_t
    {
        Nearest,
        Linear,
    };

    enum class Wrap : uint8_t
    {
        Clamp,
        Repeat,
    };

    struct SamplerDesc
    {
        Filter filter = Filter::Nearest;
        Wrap wrapU = Wrap::Clamp;
        Wrap wrapV = Wrap::Clamp;
        uint32_t anisotropy = 0; // > 1: anisotropic filtering with up to that many samples (Linear)
    };

    class Sampler
    {
    public:
        virtual ~Sampler() = default;
    };

    enum class Topology : uint8_t
    {
        Triangles,
        TriangleStrip,
        Lines,
        Points,
    };

    enum class BlendFactor : uint8_t
    {
        Zero,
        One,
        SrcAlpha,
        InvSrcAlpha,
        DstAlpha,
        InvDstAlpha,
        Src1Alpha,    // dual-source blending
        InvSrc1Alpha,
        Constant,     // the pass's blend constant
        InvConstant,
    };

    enum class BlendOp : uint8_t
    {
        Add,
        Subtract,        // src - dst
        ReverseSubtract, // dst - src
        Min,
        Max,
    };

    struct BlendState
    {
        bool enable = false;
        BlendFactor srcColor = BlendFactor::One;
        BlendFactor dstColor = BlendFactor::Zero;
        BlendOp colorOp = BlendOp::Add;
        BlendFactor srcAlpha = BlendFactor::One;
        BlendFactor dstAlpha = BlendFactor::Zero;
        BlendOp alphaOp = BlendOp::Add;
        uint8_t writeMask = 0xF; // RGBA bits
    };

    enum class Compare : uint8_t
    {
        Never,
        Less,
        Equal,
        LessEqual,
        Greater,
        NotEqual,
        GreaterEqual,
        Always,
    };

    struct DepthState
    {
        bool test = false;
        bool write = false;
        Compare func = Compare::Always;
    };

    enum class VertexFormat : uint8_t
    {
        Float1,
        Float2,
        Float3,
        Float4,
        UByte4Norm,
        UInt1,
    };

    struct VertexAttribute
    {
        const char *semantic = "TEXCOORD"; // HLSL semantic name; GLSL/MSL backends use the order
        uint32_t semanticIndex = 0;
        VertexFormat format = VertexFormat::Float4;
        uint32_t offset = 0;
    };

    // Compiled shader for this device (D3D11 / D3D12: DXBC from fxc; Vulkan: SPIR-V from tools/hlsl_spirv.py).
    struct ShaderCode
    {
        const void *data = nullptr;
        size_t size = 0;
    };

    struct PipelineDesc
    {
        ShaderCode vertex;
        ShaderCode pixel;
        std::vector<VertexAttribute> attributes; // empty: vertices come from the vertex id
        uint32_t stride = 0;
        Topology topology = Topology::Triangles;
        BlendState blend;
        DepthState depth;
        bool scissor = false;
        bool depthClip = true; // false: depth outside 0..1 is not clipped (clip distances still apply)
        const char *name = nullptr;
    };

    class Pipeline
    {
    public:
        virtual ~Pipeline() = default;
    };

    // A pass draws into one colour target (null = the swapchain's back buffer, unless depthOnly) and an optional
    // depth target. Colour and depth targets bound together must be the same size.
    struct PassDesc
    {
        Texture *color = nullptr;
        Texture *depth = nullptr;
        bool depthOnly = false; // no colour target: only `depth`
        bool clearColor = false;
        float clearRgba[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        bool clearDepth = false;
        float clearDepthValue = 0.0f;
        const char *name = nullptr;
    };

    enum class Backend : uint8_t
    {
        D3D11,
        Vulkan,
        Null,  // no GPU work: textures read back as zeros (bring-up and tests of the CPU side)
        D3D12, // (after Null: vcs.ini keeps the numbers of the others)
    };
    const char *backendName(Backend backend);         // "D3D11", "D3D12", "Vulkan", "null"
    bool backendFromName(const char *name, Backend &out); // d3d11 | d3d12 | vulkan | null, any case
    bool backendBuilt(Backend backend);               // compiled into this build
    Backend defaultBackend();                         // D3D12 on Windows (D3D11 without it), Vulkan elsewhere

    // What the Vulkan backend needs from the window (the platform layer's SDL): opaque to the RHI.
    struct VulkanWindow
    {
        const char *const *(*instanceExtensions)(uint32_t *count) = nullptr; // for the surface
        bool (*createSurface)(void *instance, uint64_t *surface) = nullptr;  // VkInstance -> VkSurfaceKHR
    };

    struct DeviceDesc
    {
        Backend backend = defaultBackend();
        void *nativeWindow = nullptr; // D3D11 / D3D12: the HWND
        VulkanWindow vulkan;          // Vulkan
        uint32_t width = 0;
        uint32_t height = 0;
        bool debug = false;     // API validation layer (when installed)
        bool offscreen = false; // Vulkan: no swapchain, the back buffer is a texture (hidden windows: test runs)
    };

    class Device
    {
    public:
        virtual ~Device() = default;

        virtual Backend backend() const = 0;
        virtual const std::string &adapterName() const = 0;

        // ---- resources --------------------------------------------------------------------------------------------
        // `data`: initial contents, rows top-down, `rowPitch` bytes apart (0 = tightly packed).
        virtual std::unique_ptr<Texture> createTexture(const TextureDesc &desc, const void *data = nullptr, uint32_t rowPitch = 0) = 0;
        virtual void updateTexture(Texture &texture, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const void *data,
                                   uint32_t rowPitch = 0) = 0;
        // Waits for the GPU and copies a region out (RGBA8 / R32F only). Slow: screenshots, dumps, tests.
        virtual bool readTexture(Texture &texture, uint32_t x, uint32_t y, uint32_t width, uint32_t height, void *out,
                                 uint32_t rowPitch = 0) = 0;
        virtual void copyTexture(Texture &dst, uint32_t dstX, uint32_t dstY, Texture &src, uint32_t srcX, uint32_t srcY,
                                 uint32_t width, uint32_t height) = 0;
        // a multisampled texture averaged into a single-sampled one of the same size and format
        virtual void resolveTexture(Texture &dst, Texture &src) = 0;
        // the mip chain of a texture made with mipLevels 0, from level 0
        virtual void generateMips(Texture &texture) = 0;
        // A sampled texture of desc.format (RGBA8, BGRA8 or block-compressed) with its mip chain given: `levels[i]` is
        // level i (max(1, width >> i) x max(1, height >> i), tightly packed rows of rowBytes / rowCount), `levelCount` of
        // them. Not a render target and no generateMips: a burst of new textures costs one plain creation each
        // (PERFORMANCE.md 21).
        virtual std::unique_ptr<Texture> createTextureLevels(const TextureDesc &desc, const void *const *levels, uint32_t levelCount) = 0;
        // Whether block-compressed textures can be made (D3D11: always, feature level 11; Vulkan: textureCompressionBC).
        virtual bool blockCompression() const { return true; }
        // True when createTexture (one level, with data, not a target) and createTextureLevels may be called from any
        // thread while the owner draws (D3D11: device creation calls are free-threaded; Vulkan: the image is made there
        // and its first contents staged, then recorded into the set-up commands at the owner's next submit).
        virtual bool freeThreadedCreation() const { return false; }

        virtual std::unique_ptr<Buffer> createBuffer(const BufferDesc &desc, const void *data = nullptr) = 0;
        virtual void updateBuffer(Buffer &buffer, const void *data, size_t size) = 0; // from offset 0, the rest discarded

        virtual std::unique_ptr<Sampler> createSampler(const SamplerDesc &desc) = 0;
        virtual std::unique_ptr<Pipeline> createPipeline(const PipelineDesc &desc) = 0;

        // ---- drawing ----------------------------------------------------------------------------------------------
        // beginPass sets the viewport to the whole target and clears the scissor to it.
        virtual void beginPass(const PassDesc &pass) = 0;
        virtual void endPass() = 0;
        virtual void setViewport(float x, float y, float width, float height) = 0;
        virtual void setScissor(int x, int y, int width, int height) = 0;
        virtual void setBlendConstant(const float rgba[4]) = 0;
        virtual void setPipeline(Pipeline &pipeline) = 0;
        virtual void setVertexBuffer(Buffer &buffer, uint32_t offset = 0) = 0;
        virtual void setIndexBuffer(Buffer &buffer, bool uint32Indices) = 0;
        virtual void setTexture(uint32_t slot, Texture *texture, Sampler *sampler) = 0; // pixel shader
        virtual void setConstants(uint32_t slot, Buffer &buffer) = 0;                    // vertex + pixel shader
        virtual void draw(uint32_t vertexCount, uint32_t firstVertex = 0) = 0;
        virtual void drawIndexed(uint32_t indexCount, uint32_t firstIndex = 0, int32_t baseVertex = 0) = 0;

        virtual void pushMarker(const char *name) = 0; // debugger / profiler annotations
        virtual void popMarker() = 0;

        // ---- swapchain --------------------------------------------------------------------------------------------
        virtual void backbufferSize(uint32_t &width, uint32_t &height) const = 0;
        virtual void resize(uint32_t width, uint32_t height) = 0;
        // Immediate: as soon as it is drawn, never torn (flip model: a newer frame replaces one not shown yet);
        // Vsync: waits for the display's refresh; Tearing: at once, mid-refresh if need be (lowest latency, VRR - falls
        // back to Immediate where the system can't).
        enum class PresentMode : uint8_t
        {
            Immediate,
            Vsync,
            Tearing,
        };
        virtual void present(PresentMode mode) = 0;
        // The input snapshot time (steady_clock ns) of the frame about to be presented: a backend that can tell when the
        // GPU finished it reports the input-to-screen delay (Vulkan: PS2X_RHI_STATS). Before present.
        virtual void setFrameInputTime(int64_t) {}

        // ---- Dear ImGui renderer (the ImGui context belongs to the platform layer) ---------------------------------
        virtual bool uiInit() = 0;
        virtual void uiNewFrame() = 0;               // before ImGui::NewFrame
        virtual void uiRender(void *imDrawData) = 0; // ImDrawData *, into the current pass
        virtual void uiShutdown() = 0;
    };

    // The device of desc.backend. Null with `error` set when that backend isn't built or no device / swapchain could be
    // made.
    std::unique_ptr<Device> createDevice(const DeviceDesc &desc, std::string &error);

    // The backends (rhi.cpp calls them; each exists only when built).
    std::unique_ptr<Device> createD3D11Device(const DeviceDesc &desc, std::string &error);
    std::unique_ptr<Device> createD3D12Device(const DeviceDesc &desc, std::string &error);
    std::unique_ptr<Device> createVulkanDevice(const DeviceDesc &desc, std::string &error);
    std::unique_ptr<Device> createNullDevice(const DeviceDesc &desc, std::string &error);
}
