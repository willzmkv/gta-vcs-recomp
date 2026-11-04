// D3D11 backend of rhi.h (feature level 11_0). The only file that includes d3d11 / dxgi headers.
#include "rhi/rhi.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include "imgui.h"
#include "imgui_impl_dx11.h"

#include <algorithm>
#include <iterator>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace rhi
{
    namespace
    {
        constexpr DXGI_FORMAT kBackbufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        constexpr UINT kBackbufferCount = 2;

        // resource format / view formats
        DXGI_FORMAT resourceFormat(Format f)
        {
            switch (f)
            {
            case Format::RGBA8: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case Format::R32F: return DXGI_FORMAT_R32_FLOAT;
            case Format::D32F: return DXGI_FORMAT_R32_TYPELESS; // DSV D32_FLOAT, SRV R32_FLOAT
            case Format::BGRA8: return DXGI_FORMAT_B8G8R8A8_UNORM;
            case Format::BC1: return DXGI_FORMAT_BC1_UNORM;
            case Format::BC2: return DXGI_FORMAT_BC2_UNORM;
            case Format::BC3: return DXGI_FORMAT_BC3_UNORM;
            case Format::BC4: return DXGI_FORMAT_BC4_UNORM;
            case Format::BC5: return DXGI_FORMAT_BC5_UNORM;
            case Format::BC7: return DXGI_FORMAT_BC7_UNORM;
            default: return DXGI_FORMAT_UNKNOWN;
            }
        }

        DXGI_FORMAT shaderViewFormat(Format f) { return f == Format::D32F ? DXGI_FORMAT_R32_FLOAT : resourceFormat(f); }

        uint32_t bytesPerPixel(Format) { return 4; }

        void setName(ID3D11DeviceChild *object, const char *name)
        {
            if (object && name)
                object->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(std::strlen(name)), name);
        }

        std::wstring wide(const char *s)
        {
            std::wstring w;
            for (; s && *s; ++s)
                w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*s)));
            return w;
        }

        std::string hex(HRESULT hr)
        {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned>(hr));
            return buf;
        }

        std::string narrow(const wchar_t *w)
        {
            const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
            std::string s(n > 0 ? n - 1 : 0, '\0');
            if (n > 1)
                WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
            return s;
        }

        D3D11_BLEND blendFactor(BlendFactor f)
        {
            switch (f)
            {
            case BlendFactor::Zero: return D3D11_BLEND_ZERO;
            case BlendFactor::One: return D3D11_BLEND_ONE;
            case BlendFactor::SrcAlpha: return D3D11_BLEND_SRC_ALPHA;
            case BlendFactor::InvSrcAlpha: return D3D11_BLEND_INV_SRC_ALPHA;
            case BlendFactor::DstAlpha: return D3D11_BLEND_DEST_ALPHA;
            case BlendFactor::InvDstAlpha: return D3D11_BLEND_INV_DEST_ALPHA;
            case BlendFactor::Src1Alpha: return D3D11_BLEND_SRC1_ALPHA;
            case BlendFactor::InvSrc1Alpha: return D3D11_BLEND_INV_SRC1_ALPHA;
            case BlendFactor::Constant: return D3D11_BLEND_BLEND_FACTOR;
            case BlendFactor::InvConstant: return D3D11_BLEND_INV_BLEND_FACTOR;
            }
            return D3D11_BLEND_ONE;
        }

        D3D11_BLEND_OP blendOp(BlendOp op)
        {
            switch (op)
            {
            case BlendOp::Add: return D3D11_BLEND_OP_ADD;
            case BlendOp::Subtract: return D3D11_BLEND_OP_SUBTRACT;
            case BlendOp::ReverseSubtract: return D3D11_BLEND_OP_REV_SUBTRACT;
            case BlendOp::Min: return D3D11_BLEND_OP_MIN;
            case BlendOp::Max: return D3D11_BLEND_OP_MAX;
            }
            return D3D11_BLEND_OP_ADD;
        }

        D3D11_COMPARISON_FUNC compare(Compare c)
        {
            switch (c)
            {
            case Compare::Never: return D3D11_COMPARISON_NEVER;
            case Compare::Less: return D3D11_COMPARISON_LESS;
            case Compare::Equal: return D3D11_COMPARISON_EQUAL;
            case Compare::LessEqual: return D3D11_COMPARISON_LESS_EQUAL;
            case Compare::Greater: return D3D11_COMPARISON_GREATER;
            case Compare::NotEqual: return D3D11_COMPARISON_NOT_EQUAL;
            case Compare::GreaterEqual: return D3D11_COMPARISON_GREATER_EQUAL;
            case Compare::Always: return D3D11_COMPARISON_ALWAYS;
            }
            return D3D11_COMPARISON_ALWAYS;
        }

        DXGI_FORMAT vertexFormat(VertexFormat f)
        {
            switch (f)
            {
            case VertexFormat::Float1: return DXGI_FORMAT_R32_FLOAT;
            case VertexFormat::Float2: return DXGI_FORMAT_R32G32_FLOAT;
            case VertexFormat::Float3: return DXGI_FORMAT_R32G32B32_FLOAT;
            case VertexFormat::Float4: return DXGI_FORMAT_R32G32B32A32_FLOAT;
            case VertexFormat::UByte4Norm: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case VertexFormat::UInt1: return DXGI_FORMAT_R32_UINT;
            }
            return DXGI_FORMAT_UNKNOWN;
        }

        D3D11_PRIMITIVE_TOPOLOGY topology(Topology t)
        {
            switch (t)
            {
            case Topology::Triangles: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            case Topology::TriangleStrip: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
            case Topology::Lines: return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
            case Topology::Points: return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
            }
            return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        }

        class D3DTexture final : public Texture
        {
        public:
            explicit D3DTexture(const TextureDesc &d) { m_desc = d; }
            ComPtr<ID3D11Texture2D> texture;
            ComPtr<ID3D11ShaderResourceView> srv;
            ComPtr<ID3D11RenderTargetView> rtv;
            ComPtr<ID3D11DepthStencilView> dsv;
            bool dynamic = false;
        };

        class D3DBuffer final : public Buffer
        {
        public:
            explicit D3DBuffer(const BufferDesc &d) { m_desc = d; }
            ComPtr<ID3D11Buffer> buffer;
        };

        class D3DSampler final : public Sampler
        {
        public:
            ComPtr<ID3D11SamplerState> state;
        };

        class D3DPipeline final : public Pipeline
        {
        public:
            ComPtr<ID3D11VertexShader> vs;
            ComPtr<ID3D11PixelShader> ps;
            ComPtr<ID3D11InputLayout> layout;
            ComPtr<ID3D11BlendState> blend;
            ComPtr<ID3D11DepthStencilState> depth;
            ComPtr<ID3D11RasterizerState> raster;
            D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            UINT stride = 0;
        };

        class D3D11Device final : public Device
        {
        public:
            bool create(const DeviceDesc &desc, std::string &error)
            {
                m_hwnd = static_cast<HWND>(desc.nativeWindow);
                UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
                if (desc.debug)
                    flags |= D3D11_CREATE_DEVICE_DEBUG;
                const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
                HRESULT hr = E_FAIL;
                for (int attempt = 0; attempt < 3 && FAILED(hr); ++attempt)
                {
                    // retry without the debug layer (not installed) and without 11_1 (old runtimes reject it)
                    const D3D_FEATURE_LEVEL *first = attempt == 2 ? levels + 1 : levels;
                    const UINT count = attempt == 2 ? 1u : 2u;
                    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, first, count, D3D11_SDK_VERSION,
                                           &m_device, nullptr, &m_context);
                    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG))
                    {
                        std::fprintf(stderr, "[rhi] D3D11 debug layer not available\n");
                        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
                    }
                }
                if (FAILED(hr))
                {
                    error = "D3D11CreateDevice failed (" + hex(hr) + ")";
                    return false;
                }
                m_context.As(&m_annotation);

                ComPtr<IDXGIDevice> dxgiDevice;
                ComPtr<IDXGIAdapter> adapter;
                ComPtr<IDXGIFactory2> factory;
                m_device.As(&dxgiDevice);
                dxgiDevice->GetAdapter(&adapter);
                DXGI_ADAPTER_DESC adapterDesc{};
                adapter->GetDesc(&adapterDesc);
                m_adapterName = narrow(adapterDesc.Description);
                if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))
                {
                    error = "no IDXGIFactory2";
                    return false;
                }
                ComPtr<IDXGIFactory5> factory5;
                BOOL tearing = FALSE;
                if (SUCCEEDED(factory.As(&factory5)))
                    factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
                m_swapFlags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;

                DXGI_SWAP_CHAIN_DESC1 sc{};
                sc.Width = std::max(1u, desc.width);
                sc.Height = std::max(1u, desc.height);
                sc.Format = kBackbufferFormat;
                sc.SampleDesc.Count = 1;
                sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                sc.BufferCount = kBackbufferCount;
                sc.Scaling = DXGI_SCALING_STRETCH;
                sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                sc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
                sc.Flags = m_swapFlags;
                hr = factory->CreateSwapChainForHwnd(m_device.Get(), m_hwnd, &sc, nullptr, nullptr, &m_swapchain);
                if (FAILED(hr))
                {
                    error = "CreateSwapChainForHwnd failed (" + hex(hr) + ")";
                    return false;
                }
                factory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER);
                m_width = sc.Width;
                m_height = sc.Height;
                return makeBackbufferView();
            }

            ~D3D11Device() override
            {
                uiShutdown();
                if (m_context)
                {
                    m_context->ClearState();
                    m_context->Flush();
                }
            }

            Backend backend() const override { return Backend::D3D11; }
            const std::string &adapterName() const override { return m_adapterName; }

            // ---- resources ----------------------------------------------------------------------------------------
            std::unique_ptr<Texture> createTexture(const TextureDesc &desc, const void *data, uint32_t rowPitch) override
            {
                auto t = std::make_unique<D3DTexture>(desc);
                D3D11_TEXTURE2D_DESC td{};
                td.Width = std::max(1u, desc.width);
                td.Height = std::max(1u, desc.height);
                const bool msaa = desc.samples > 1u;
                const bool mips = desc.mipLevels != 1u && !msaa;
                td.MipLevels = mips ? desc.mipLevels : 1u; // 0: the full chain
                td.ArraySize = 1;
                td.Format = resourceFormat(desc.format);
                td.SampleDesc.Count = msaa ? desc.samples : 1u;
                t->dynamic = (desc.usage & UsageDynamic) && !(desc.usage & (UsageRenderTarget | UsageDepth));
                td.Usage = t->dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
                td.CPUAccessFlags = t->dynamic ? D3D11_CPU_ACCESS_WRITE : 0u;
                if (desc.usage & UsageSampled)
                    td.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
                if (desc.usage & UsageRenderTarget)
                    td.BindFlags |= D3D11_BIND_RENDER_TARGET;
                if (desc.usage & UsageDepth)
                    td.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
                if (mips)
                {
                    // generateMips needs a render target and shader view; level 0 is uploaded after creation
                    td.BindFlags |= D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                    td.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
                }
                D3D11_SUBRESOURCE_DATA init{data, rowPitch ? rowPitch : td.Width * bytesPerPixel(desc.format), 0};
                if (FAILED(m_device->CreateTexture2D(&td, (data && !mips) ? &init : nullptr, &t->texture)))
                {
                    std::fprintf(stderr, "[rhi] CreateTexture2D %ux%u failed\n", td.Width, td.Height);
                    return nullptr;
                }
                setName(t->texture.Get(), desc.name);
                // PS2X_RHI_LOG_CREATES=1: textures of 256x256 and more as they are created (a target re-created every
                // frame shows as a stream of these)
                static const bool logCreates = std::getenv("PS2X_RHI_LOG_CREATES") != nullptr;
                if (logCreates && td.Width * td.Height >= 256u * 256u)
                {
                    static uint64_t created = 0;
                    if (++created <= 2000u)
                        std::fprintf(stderr, "[rhi] create %ux%u x%u '%s'\n", td.Width, td.Height, td.SampleDesc.Count,
                                     desc.name ? desc.name : "");
                }
                if (desc.usage & UsageSampled)
                {
                    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
                    sv.Format = shaderViewFormat(desc.format);
                    sv.ViewDimension = msaa ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
                    sv.Texture2D.MipLevels = mips ? static_cast<UINT>(-1) : 1u;
                    m_device->CreateShaderResourceView(t->texture.Get(), &sv, &t->srv);
                }
                if ((desc.usage & UsageRenderTarget) || mips)
                {
                    D3D11_RENDER_TARGET_VIEW_DESC rv{};
                    rv.Format = resourceFormat(desc.format);
                    rv.ViewDimension = msaa ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
                    m_device->CreateRenderTargetView(t->texture.Get(), &rv, &t->rtv);
                }
                if (desc.usage & UsageDepth)
                {
                    D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
                    dv.Format = DXGI_FORMAT_D32_FLOAT;
                    dv.ViewDimension = msaa ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
                    m_device->CreateDepthStencilView(t->texture.Get(), &dv, &t->dsv);
                }
                if (mips && data)
                {
                    m_context->UpdateSubresource(t->texture.Get(), 0, nullptr, data, init.SysMemPitch, 0);
                    m_context->GenerateMips(t->srv.Get());
                }
                return t;
            }

            void updateTexture(Texture &texture, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const void *data,
                               uint32_t rowPitch) override
            {
                auto &t = static_cast<D3DTexture &>(texture);
                const uint32_t bpp = bytesPerPixel(t.desc().format);
                const uint32_t pitch = rowPitch ? rowPitch : width * bpp;
                if (t.dynamic)
                {
                    // dynamic textures are rewritten whole (WRITE_DISCARD)
                    D3D11_MAPPED_SUBRESOURCE m{};
                    if (FAILED(m_context->Map(t.texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
                        return;
                    for (uint32_t row = 0; row < height; ++row)
                        std::memcpy(static_cast<uint8_t *>(m.pData) + (y + row) * m.RowPitch + x * bpp,
                                    static_cast<const uint8_t *>(data) + row * pitch, width * bpp);
                    m_context->Unmap(t.texture.Get(), 0);
                    return;
                }
                const D3D11_BOX box{x, y, 0, x + width, y + height, 1};
                m_context->UpdateSubresource(t.texture.Get(), 0, &box, data, pitch, 0);
            }

            bool readTexture(Texture &texture, uint32_t x, uint32_t y, uint32_t width, uint32_t height, void *out,
                             uint32_t rowPitch) override
            {
                auto &t = static_cast<D3DTexture &>(texture);
                D3D11_TEXTURE2D_DESC td{};
                t.texture->GetDesc(&td);
                td.Width = width;
                td.Height = height;
                td.Usage = D3D11_USAGE_STAGING;
                td.BindFlags = 0;
                td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                td.MiscFlags = 0;
                ComPtr<ID3D11Texture2D> staging;
                if (FAILED(m_device->CreateTexture2D(&td, nullptr, &staging)))
                    return false;
                const D3D11_BOX box{x, y, 0, x + width, y + height, 1};
                m_context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, t.texture.Get(), 0, &box);
                D3D11_MAPPED_SUBRESOURCE m{};
                if (FAILED(m_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)))
                    return false;
                const uint32_t bpp = bytesPerPixel(t.desc().format);
                const uint32_t pitch = rowPitch ? rowPitch : width * bpp;
                for (uint32_t row = 0; row < height; ++row)
                    std::memcpy(static_cast<uint8_t *>(out) + row * pitch, static_cast<const uint8_t *>(m.pData) + row * m.RowPitch,
                                width * bpp);
                m_context->Unmap(staging.Get(), 0);
                return true;
            }

            void copyTexture(Texture &dst, uint32_t dstX, uint32_t dstY, Texture &src, uint32_t srcX, uint32_t srcY, uint32_t width,
                             uint32_t height) override
            {
                const D3D11_BOX box{srcX, srcY, 0, srcX + width, srcY + height, 1};
                m_context->CopySubresourceRegion(static_cast<D3DTexture &>(dst).texture.Get(), 0, dstX, dstY, 0,
                                                 static_cast<D3DTexture &>(src).texture.Get(), 0, &box);
            }

            void resolveTexture(Texture &dst, Texture &src) override
            {
                m_context->ResolveSubresource(static_cast<D3DTexture &>(dst).texture.Get(), 0, static_cast<D3DTexture &>(src).texture.Get(),
                                              0, resourceFormat(src.desc().format));
            }

            void generateMips(Texture &texture) override
            {
                if (auto &t = static_cast<D3DTexture &>(texture); t.srv)
                    m_context->GenerateMips(t.srv.Get());
            }

            bool freeThreadedCreation() const override { return true; } // only m_device: no immediate-context calls there

            std::unique_ptr<Texture> createTextureLevels(const TextureDesc &desc, const void *const *levels, uint32_t levelCount) override
            {
                TextureDesc sized = desc;
                sized.width = std::max(1u, desc.width);
                sized.height = std::max(1u, desc.height);
                sized.mipLevels = std::max(1u, levelCount);
                auto t = std::make_unique<D3DTexture>(sized);
                D3D11_TEXTURE2D_DESC td{};
                td.Width = sized.width;
                td.Height = sized.height;
                td.MipLevels = sized.mipLevels;
                td.ArraySize = 1;
                td.Format = resourceFormat(desc.format);
                td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_IMMUTABLE; // written once: the driver may place it as it likes
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                D3D11_SUBRESOURCE_DATA init[16]{};
                for (uint32_t i = 0; i < td.MipLevels && i < 16u; ++i)
                    init[i] = {levels[i], rowBytes(desc.format, std::max(1u, td.Width >> i)), 0};
                td.MipLevels = std::min(td.MipLevels, 16u);
                if (FAILED(m_device->CreateTexture2D(&td, init, &t->texture)))
                {
                    std::fprintf(stderr, "[rhi] CreateTexture2D %ux%u (%u levels) failed\n", td.Width, td.Height, td.MipLevels);
                    return nullptr;
                }
                setName(t->texture.Get(), desc.name);
                D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
                sv.Format = shaderViewFormat(desc.format);
                sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                sv.Texture2D.MipLevels = td.MipLevels;
                m_device->CreateShaderResourceView(t->texture.Get(), &sv, &t->srv);
                return t;
            }

            std::unique_ptr<Buffer> createBuffer(const BufferDesc &desc, const void *data) override
            {
                auto b = std::make_unique<D3DBuffer>(desc);
                D3D11_BUFFER_DESC bd{};
                bd.ByteWidth = static_cast<UINT>(desc.usage == BufferUsage::Constant ? (desc.size + 15u) & ~size_t(15) : desc.size);
                bd.Usage = desc.dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
                bd.CPUAccessFlags = desc.dynamic ? D3D11_CPU_ACCESS_WRITE : 0u;
                bd.BindFlags = desc.usage == BufferUsage::Vertex  ? D3D11_BIND_VERTEX_BUFFER
                               : desc.usage == BufferUsage::Index ? D3D11_BIND_INDEX_BUFFER
                                                                  : D3D11_BIND_CONSTANT_BUFFER;
                D3D11_SUBRESOURCE_DATA init{data, 0, 0};
                if (FAILED(m_device->CreateBuffer(&bd, data ? &init : nullptr, &b->buffer)))
                {
                    std::fprintf(stderr, "[rhi] CreateBuffer %u bytes failed\n", bd.ByteWidth);
                    return nullptr;
                }
                setName(b->buffer.Get(), desc.name);
                return b;
            }

            void updateBuffer(Buffer &buffer, const void *data, size_t size) override
            {
                auto &b = static_cast<D3DBuffer &>(buffer);
                if (!b.desc().dynamic)
                {
                    m_context->UpdateSubresource(b.buffer.Get(), 0, nullptr, data, 0, 0);
                    return;
                }
                D3D11_MAPPED_SUBRESOURCE m{};
                if (FAILED(m_context->Map(b.buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
                    return;
                std::memcpy(m.pData, data, std::min(size, b.desc().size));
                m_context->Unmap(b.buffer.Get(), 0);
            }

            std::unique_ptr<Sampler> createSampler(const SamplerDesc &desc) override
            {
                auto s = std::make_unique<D3DSampler>();
                D3D11_SAMPLER_DESC sd{};
                sd.Filter = desc.filter == Filter::Linear ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
                if (desc.filter == Filter::Linear && desc.anisotropy > 1u)
                {
                    sd.Filter = D3D11_FILTER_ANISOTROPIC;
                    sd.MaxAnisotropy = std::min(desc.anisotropy, 16u);
                }
                sd.AddressU = desc.wrapU == Wrap::Repeat ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
                sd.AddressV = desc.wrapV == Wrap::Repeat ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
                sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
                sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
                sd.MaxLOD = D3D11_FLOAT32_MAX;
                if (FAILED(m_device->CreateSamplerState(&sd, &s->state)))
                    return nullptr;
                return s;
            }

            std::unique_ptr<Pipeline> createPipeline(const PipelineDesc &desc) override
            {
                auto p = std::make_unique<D3DPipeline>();
                // shader objects are shared by every pipeline made from the same bytecode
                ComPtr<ID3D11VertexShader> &vs = m_vertexShaders[desc.vertex.data];
                ComPtr<ID3D11PixelShader> &ps = m_pixelShaders[desc.pixel.data];
                if ((!vs && FAILED(m_device->CreateVertexShader(desc.vertex.data, desc.vertex.size, nullptr, &vs))) ||
                    (!ps && FAILED(m_device->CreatePixelShader(desc.pixel.data, desc.pixel.size, nullptr, &ps))))
                {
                    std::fprintf(stderr, "[rhi] shaders of pipeline '%s' rejected\n", desc.name ? desc.name : "?");
                    return nullptr;
                }
                p->vs = vs;
                p->ps = ps;
                if (!desc.attributes.empty())
                {
                    std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
                    for (const VertexAttribute &a : desc.attributes)
                        elements.push_back({a.semantic, a.semanticIndex, vertexFormat(a.format), 0, a.offset,
                                            D3D11_INPUT_PER_VERTEX_DATA, 0});
                    if (FAILED(m_device->CreateInputLayout(elements.data(), static_cast<UINT>(elements.size()), desc.vertex.data,
                                                           desc.vertex.size, &p->layout)))
                    {
                        std::fprintf(stderr, "[rhi] input layout of pipeline '%s' rejected\n", desc.name ? desc.name : "?");
                        return nullptr;
                    }
                }
                D3D11_BLEND_DESC bd{};
                auto &rt = bd.RenderTarget[0];
                rt.BlendEnable = desc.blend.enable;
                rt.SrcBlend = blendFactor(desc.blend.srcColor);
                rt.DestBlend = blendFactor(desc.blend.dstColor);
                rt.BlendOp = blendOp(desc.blend.colorOp);
                rt.SrcBlendAlpha = blendFactor(desc.blend.srcAlpha);
                rt.DestBlendAlpha = blendFactor(desc.blend.dstAlpha);
                rt.BlendOpAlpha = blendOp(desc.blend.alphaOp);
                rt.RenderTargetWriteMask = desc.blend.writeMask & 0xF;
                D3D11_DEPTH_STENCIL_DESC dd{};
                dd.DepthEnable = desc.depth.test || desc.depth.write;
                dd.DepthWriteMask = desc.depth.write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
                dd.DepthFunc = desc.depth.test ? compare(desc.depth.func) : D3D11_COMPARISON_ALWAYS;
                D3D11_RASTERIZER_DESC rd{};
                rd.FillMode = D3D11_FILL_SOLID;
                rd.CullMode = D3D11_CULL_NONE;
                rd.DepthClipEnable = desc.depthClip ? TRUE : FALSE;
                rd.ScissorEnable = TRUE; // beginPass sets the scissor to the whole target
                if (FAILED(m_device->CreateBlendState(&bd, &p->blend)) || FAILED(m_device->CreateDepthStencilState(&dd, &p->depth)) ||
                    FAILED(m_device->CreateRasterizerState(&rd, &p->raster)))
                    return nullptr;
                p->topology = topology(desc.topology);
                p->stride = desc.stride;
                return p;
            }

            // ---- drawing ------------------------------------------------------------------------------------------
            void beginPass(const PassDesc &pass) override
            {
                ID3D11RenderTargetView *rtv = m_backRtv.Get();
                uint32_t w = m_width, h = m_height;
                if (pass.depthOnly)
                {
                    rtv = nullptr;
                    if (pass.depth)
                    {
                        w = pass.depth->desc().width;
                        h = pass.depth->desc().height;
                    }
                }
                else if (pass.color)
                {
                    auto &t = static_cast<D3DTexture &>(*pass.color);
                    rtv = t.rtv.Get();
                    w = t.desc().width;
                    h = t.desc().height;
                }
                ID3D11DepthStencilView *dsv = pass.depth ? static_cast<D3DTexture &>(*pass.depth).dsv.Get() : nullptr;
                // a target may still be bound as a shader input from an earlier pass
                ID3D11ShaderResourceView *none[8] = {};
                m_context->PSSetShaderResources(0, 8, none);
                std::fill(std::begin(m_cache.srv), std::end(m_cache.srv), nullptr);
                m_context->OMSetRenderTargets(rtv ? 1u : 0u, rtv ? &rtv : nullptr, dsv);
                if (pass.clearColor && rtv)
                    m_context->ClearRenderTargetView(rtv, pass.clearRgba);
                if (pass.clearDepth && dsv)
                    m_context->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, pass.clearDepthValue, 0);
                setViewport(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
                setScissor(0, 0, static_cast<int>(w), static_cast<int>(h));
                if (pass.name)
                    pushMarker(pass.name);
                m_passMarker = pass.name != nullptr;
            }

            void endPass() override
            {
                if (m_passMarker)
                    popMarker();
                m_passMarker = false;
            }

            void setViewport(float x, float y, float width, float height) override
            {
                const D3D11_VIEWPORT vp{x, y, width, height, 0.0f, 1.0f};
                m_context->RSSetViewports(1, &vp);
            }

            void setScissor(int x, int y, int width, int height) override
            {
                const D3D11_RECT r{x, y, x + width, y + height};
                m_context->RSSetScissorRects(1, &r);
            }

            void setBlendConstant(const float rgba[4]) override
            {
                std::memcpy(m_blendConstant, rgba, sizeof(m_blendConstant));
                if (m_pipeline)
                    m_context->OMSetBlendState(m_pipeline->blend.Get(), m_blendConstant, 0xFFFFFFFFu);
            }

            void setPipeline(Pipeline &pipeline) override
            {
                auto &p = static_cast<D3DPipeline &>(pipeline);
                m_pipeline = &p;
                if (!stateCacheOn())
                    m_cache = {};
                StateCache &c = m_cache;
                if (c.layout != p.layout.Get())
                    m_context->IASetInputLayout(c.layout = p.layout.Get());
                if (c.topology != p.topology)
                    m_context->IASetPrimitiveTopology(c.topology = p.topology);
                if (c.vs != p.vs.Get())
                    m_context->VSSetShader(c.vs = p.vs.Get(), nullptr, 0);
                if (c.ps != p.ps.Get())
                    m_context->PSSetShader(c.ps = p.ps.Get(), nullptr, 0);
                if (!c.gsCleared)
                {
                    m_context->GSSetShader(nullptr, nullptr, 0);
                    c.gsCleared = true;
                }
                if (c.blend != p.blend.Get() || std::memcmp(c.blendConstant, m_blendConstant, sizeof(m_blendConstant)) != 0)
                {
                    m_context->OMSetBlendState(c.blend = p.blend.Get(), m_blendConstant, 0xFFFFFFFFu);
                    std::memcpy(c.blendConstant, m_blendConstant, sizeof(m_blendConstant));
                }
                if (c.depth != p.depth.Get())
                    m_context->OMSetDepthStencilState(c.depth = p.depth.Get(), 0);
                if (c.raster != p.raster.Get())
                    m_context->RSSetState(c.raster = p.raster.Get());
            }

            void setVertexBuffer(Buffer &buffer, uint32_t offset) override
            {
                ID3D11Buffer *b = static_cast<D3DBuffer &>(buffer).buffer.Get();
                const UINT stride = m_pipeline ? m_pipeline->stride : 0u;
                if (!stateCacheOn())
                    m_cache = {};
                StateCache &c = m_cache;
                if (c.vb == b && c.vbStride == stride && c.vbOffset == offset)
                    return;
                c.vb = b;
                c.vbStride = stride;
                c.vbOffset = offset;
                m_context->IASetVertexBuffers(0, 1, &b, &stride, &offset);
            }

            void setIndexBuffer(Buffer &buffer, bool uint32Indices) override
            {
                ID3D11Buffer *b = static_cast<D3DBuffer &>(buffer).buffer.Get();
                const DXGI_FORMAT format = uint32Indices ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
                if (!stateCacheOn())
                    m_cache = {};
                if (m_cache.ib == b && m_cache.ibFormat == format)
                    return;
                m_cache.ib = b;
                m_cache.ibFormat = format;
                m_context->IASetIndexBuffer(b, format, 0);
            }

            void setTexture(uint32_t slot, Texture *texture, Sampler *sampler) override
            {
                ID3D11ShaderResourceView *srv = texture ? static_cast<D3DTexture *>(texture)->srv.Get() : nullptr;
                ID3D11SamplerState *s = sampler ? static_cast<D3DSampler *>(sampler)->state.Get() : nullptr;
                const bool cached = slot < StateCache::kSlots && stateCacheOn();
                if (!cached || m_cache.srv[slot] != srv)
                {
                    m_context->PSSetShaderResources(slot, 1, &srv);
                    if (cached)
                        m_cache.srv[slot] = srv;
                }
                if (!cached || m_cache.sampler[slot] != s)
                {
                    m_context->PSSetSamplers(slot, 1, &s);
                    if (cached)
                        m_cache.sampler[slot] = s;
                }
            }

            void setConstants(uint32_t slot, Buffer &buffer) override
            {
                ID3D11Buffer *b = static_cast<D3DBuffer &>(buffer).buffer.Get();
                if (!stateCacheOn())
                    m_cache = {};
                if (slot < StateCache::kSlots && m_cache.cb[slot] == b)
                    return; // a WRITE_DISCARD map keeps the binding (the runtime renames the buffer behind it)
                if (slot < StateCache::kSlots)
                    m_cache.cb[slot] = b;
                m_context->VSSetConstantBuffers(slot, 1, &b);
                m_context->PSSetConstantBuffers(slot, 1, &b);
            }

            void draw(uint32_t vertexCount, uint32_t firstVertex) override { m_context->Draw(vertexCount, firstVertex); }

            void drawIndexed(uint32_t indexCount, uint32_t firstIndex, int32_t baseVertex) override
            {
                m_context->DrawIndexed(indexCount, firstIndex, baseVertex);
            }

            void pushMarker(const char *name) override
            {
                if (m_annotation)
                    m_annotation->BeginEvent(wide(name).c_str());
                if (gpuTiming())
                    m_timer.push(*this, name);
            }

            void popMarker() override
            {
                if (m_annotation)
                    m_annotation->EndEvent();
                if (gpuTiming())
                    m_timer.pop(*this);
            }

            // ---- swapchain ----------------------------------------------------------------------------------------
            void backbufferSize(uint32_t &width, uint32_t &height) const override
            {
                width = m_width;
                height = m_height;
            }

            void resize(uint32_t width, uint32_t height) override
            {
                if (!width || !height || (width == m_width && height == m_height))
                    return;
                m_context->OMSetRenderTargets(0, nullptr, nullptr);
                m_backRtv.Reset();
                m_context->Flush();
                if (FAILED(m_swapchain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, m_swapFlags)))
                {
                    std::fprintf(stderr, "[rhi] ResizeBuffers %ux%u failed\n", width, height);
                    makeBackbufferView();
                    return;
                }
                m_width = width;
                m_height = height;
                makeBackbufferView();
            }

            void present(PresentMode mode) override
            {
                const bool tearing = mode == PresentMode::Tearing && (m_swapFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
                const HRESULT hr = m_swapchain->Present(mode == PresentMode::Vsync ? 1u : 0u, tearing ? DXGI_PRESENT_ALLOW_TEARING : 0u);
                if ((hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) && !m_lostReported)
                {
                    m_lostReported = true;
                    std::fprintf(stderr, "[rhi] graphics device lost (0x%08X, reason 0x%08X)\n", static_cast<unsigned>(hr),
                                 static_cast<unsigned>(m_device->GetDeviceRemovedReason()));
                }
                m_pipeline = nullptr; // flip model unbinds the back buffer; state is set again next frame
                m_cache = {};
                if (gpuTiming())
                    m_timer.endFrame(*this);
            }

            // ---- Dear ImGui ---------------------------------------------------------------------------------------
            bool uiInit() override
            {
                if (!m_ui)
                    m_ui = ImGui_ImplDX11_Init(m_device.Get(), m_context.Get());
                return m_ui;
            }

            void uiNewFrame() override
            {
                if (m_ui)
                    ImGui_ImplDX11_NewFrame();
            }

            void uiRender(void *imDrawData) override
            {
                if (m_ui && imDrawData)
                    ImGui_ImplDX11_RenderDrawData(static_cast<ImDrawData *>(imDrawData));
                m_pipeline = nullptr; // the backend changed the state
                m_cache = {};
            }

            void uiShutdown() override
            {
                if (m_ui)
                    ImGui_ImplDX11_Shutdown();
                m_ui = false;
            }

        private:
            // PS2X_GPU_TIMING=1: GPU time between each pushMarker / popMarker pair (timestamp queries), summed per marker
            // name and reported every 5 s as ms per frame - which passes a frame's GPU time goes to. A marker's time
            // includes the markers inside it.
            static bool gpuTiming()
            {
                static const bool on = std::getenv("PS2X_GPU_TIMING") != nullptr;
                return on;
            }
            struct GpuTimer
            {
                struct Span
                {
                    std::string name;
                    ComPtr<ID3D11Query> begin, end;
                };
                struct FrameQueries
                {
                    ComPtr<ID3D11Query> disjoint;
                    std::vector<Span> spans;
                };
                std::vector<ComPtr<ID3D11Query>> pool;
                std::vector<Span> open;
                FrameQueries current;
                std::vector<FrameQueries> pending;
                std::unordered_map<std::string, std::pair<double, uint64_t>> totals; // ms, spans
                uint64_t frames = 0;
                std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();

                ComPtr<ID3D11Query> timestamp(D3D11Device &d)
                {
                    ComPtr<ID3D11Query> q;
                    if (!pool.empty())
                    {
                        q = std::move(pool.back());
                        pool.pop_back();
                        return q;
                    }
                    const D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP, 0};
                    d.m_device->CreateQuery(&qd, &q);
                    return q;
                }
                void push(D3D11Device &d, const char *name)
                {
                    if (!current.disjoint)
                    {
                        const D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
                        d.m_device->CreateQuery(&qd, &current.disjoint);
                        if (current.disjoint)
                            d.m_context->Begin(current.disjoint.Get());
                    }
                    Span sp;
                    sp.name = name ? name : "?";
                    sp.begin = timestamp(d);
                    if (sp.begin)
                        d.m_context->End(sp.begin.Get());
                    open.push_back(std::move(sp));
                }
                void pop(D3D11Device &d)
                {
                    if (open.empty())
                        return;
                    Span sp = std::move(open.back());
                    open.pop_back();
                    sp.end = timestamp(d);
                    if (sp.end)
                        d.m_context->End(sp.end.Get());
                    if (sp.begin && sp.end)
                        current.spans.push_back(std::move(sp));
                }
                void endFrame(D3D11Device &d)
                {
                    if (current.disjoint && open.empty())
                    {
                        d.m_context->End(current.disjoint.Get());
                        pending.push_back(std::move(current));
                        current = {};
                    }
                    while (!pending.empty()) // finished frames, oldest first
                    {
                        FrameQueries &f = pending.front();
                        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
                        if (d.m_context->GetData(f.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
                            break;
                        ++frames;
                        for (Span &sp : f.spans)
                        {
                            UINT64 t0 = 0, t1 = 0;
                            if (!dj.Disjoint && dj.Frequency &&
                                d.m_context->GetData(sp.begin.Get(), &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                                d.m_context->GetData(sp.end.Get(), &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && t1 >= t0)
                            {
                                auto &t = totals[sp.name];
                                t.first += static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(dj.Frequency);
                                ++t.second;
                            }
                            pool.push_back(std::move(sp.begin));
                            pool.push_back(std::move(sp.end));
                        }
                        pending.erase(pending.begin());
                    }
                    if (!frames || std::chrono::steady_clock::now() - lastReport < std::chrono::seconds(5))
                        return;
                    lastReport = std::chrono::steady_clock::now();
                    std::vector<std::pair<std::string, std::pair<double, uint64_t>>> rows(totals.begin(), totals.end());
                    std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.second.first > b.second.first; });
                    std::fprintf(stderr, "[gpu-timing] %llu frames, GPU ms per frame by marker:\n", static_cast<unsigned long long>(frames));
                    for (const auto &[name, t] : rows)
                        std::fprintf(stderr, "[gpu-timing] %8.3f ms %8.1f spans  %s\n", t.first / frames,
                                     static_cast<double>(t.second) / frames, name.c_str());
                    totals.clear();
                    frames = 0;
                }
            };
            GpuTimer m_timer;

            bool makeBackbufferView()
            {
                ComPtr<ID3D11Texture2D> back;
                if (FAILED(m_swapchain->GetBuffer(0, IID_PPV_ARGS(&back))) ||
                    FAILED(m_device->CreateRenderTargetView(back.Get(), nullptr, &m_backRtv)))
                {
                    std::fprintf(stderr, "[rhi] no back buffer view\n");
                    return false;
                }
                return true;
            }

            HWND m_hwnd = nullptr;
            ComPtr<ID3D11Device> m_device;
            ComPtr<ID3D11DeviceContext> m_context;
            ComPtr<ID3DUserDefinedAnnotation> m_annotation;
            ComPtr<IDXGISwapChain1> m_swapchain;
            ComPtr<ID3D11RenderTargetView> m_backRtv;
            UINT m_swapFlags = 0;
            uint32_t m_width = 0, m_height = 0;
            std::string m_adapterName;
            D3DPipeline *m_pipeline = nullptr;
            float m_blendConstant[4] = {0, 0, 0, 0};
            // The state last given to the context: a draw's pipeline, buffers, textures and constants reach the driver only
            // when they change (AMD's D3D11 driver costs ~4-5 us a draw on the main thread, ~16 calls each; most of a
            // frame's draws repeat the previous one's state). Reset where something else sets state: the present (flip
            // model), Dear ImGui, a pass's start (its shader inputs).
            struct StateCache
            {
                static constexpr uint32_t kSlots = 8;
                ID3D11InputLayout *layout = nullptr;
                D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
                ID3D11VertexShader *vs = nullptr;
                ID3D11PixelShader *ps = nullptr;
                bool gsCleared = false;
                ID3D11BlendState *blend = unknown<ID3D11BlendState>();
                float blendConstant[4] = {-1, -1, -1, -1};
                ID3D11DepthStencilState *depth = unknown<ID3D11DepthStencilState>();
                ID3D11RasterizerState *raster = unknown<ID3D11RasterizerState>();
                ID3D11Buffer *vb = unknown<ID3D11Buffer>();
                UINT vbStride = 0, vbOffset = 0;
                ID3D11Buffer *ib = unknown<ID3D11Buffer>();
                DXGI_FORMAT ibFormat = DXGI_FORMAT_UNKNOWN;
                ID3D11Buffer *cb[kSlots];
                ID3D11ShaderResourceView *srv[kSlots];
                ID3D11SamplerState *sampler[kSlots];
                template <typename T>
                static T *unknown() { return reinterpret_cast<T *>(~uintptr_t(0)); } // never a real object: set it the first time
                StateCache()
                {
                    for (uint32_t i = 0; i < kSlots; ++i)
                    {
                        cb[i] = unknown<ID3D11Buffer>();
                        srv[i] = unknown<ID3D11ShaderResourceView>();
                        sampler[i] = unknown<ID3D11SamplerState>();
                    }
                }
            } m_cache;
            // VCS_D3D11_STATE_CACHE=0: every call reaches the driver (the cache's reference)
            static bool stateCacheOn()
            {
                static const bool on = !(std::getenv("VCS_D3D11_STATE_CACHE") && std::getenv("VCS_D3D11_STATE_CACHE")[0] == '0');
                return on;
            }
            std::unordered_map<const void *, ComPtr<ID3D11VertexShader>> m_vertexShaders; // by bytecode
            std::unordered_map<const void *, ComPtr<ID3D11PixelShader>> m_pixelShaders;
            bool m_passMarker = false;
            bool m_ui = false;
            bool m_lostReported = false;
        };
    }

    std::unique_ptr<Device> createD3D11Device(const DeviceDesc &desc, std::string &error)
    {
        auto device = std::make_unique<D3D11Device>();
        if (!device->create(desc, error))
            return nullptr;
        return device;
    }
}
