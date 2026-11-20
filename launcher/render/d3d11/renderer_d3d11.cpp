// Direct3D 11 backend (uppies' design, docs/LAUNCHER.md).
//
// A frame is one instanced draw, so D3D12's strengths (cheap submission, explicit threading) don't apply; what
// matters is presentation, done the modern way:
// - a flip-model swap chain (FLIP_DISCARD): no copy into a redirection surface, independent flip and overlays;
// - a frame latency waitable with a maximum latency of one: the frame loop waits until the display can take a frame
//   *before* reading input, so what is shown is never more than a frame old;
// - tearing allowed when vsync is off (uncapped, variable refresh rate);
// - a lost device (driver update, TDR) is recreated from CPU-side state, the atlas included;
// - WARP when there is no hardware device (remote desktop, CI).
#include "render/renderer.h"

#include "render/draw.h"
#include "render/font.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>

#include "ui_ps.h" // after windows.h: fxc headers use BYTE
#include "ui_vs.h"

#include <cstdio>
#include <cstring>

namespace render
{
    RendererStats rendererStats;

    namespace
    {
        constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

        struct State
        {
            HWND hwnd = nullptr;
            ID3D11Device *device = nullptr;
            ID3D11DeviceContext *context = nullptr;
            IDXGISwapChain1 *swapChain = nullptr;
            HANDLE frameWaitable = nullptr;
            bool frameWaited = false; // waited, not presented since
            UINT swapFlags = 0;
            bool tearing = false;
            ID3D11Texture2D *offscreen = nullptr; // no window: the target
            ID3D11RenderTargetView *rtv = nullptr;
            int width = 0, height = 0;

            ID3D11VertexShader *vs = nullptr;
            ID3D11PixelShader *ps = nullptr;
            ID3D11InputLayout *layout = nullptr;
            ID3D11BlendState *blend = nullptr;
            ID3D11RasterizerState *raster = nullptr;
            ID3D11SamplerState *sampler = nullptr;
            ID3D11Buffer *constants = nullptr;
            ID3D11Buffer *instances = nullptr;
            size_t instanceCapacity = 0;
            ID3D11Texture2D *atlasTexture = nullptr;
            ID3D11ShaderResourceView *atlasView = nullptr;
            int atlasSize = 0;
            uint32_t atlasGeneration = 0;
            ID3D11Texture2D *imageTexture = nullptr;
            ID3D11ShaderResourceView *imageView = nullptr;
            uint32_t imageGeneration = 0;
            bool loseDevice = false;
        };
        State d3d;

        struct FrameConstants
        {
            float viewport[2];
            float atlasInvSize[2];
        };

        template <typename T> void release(T *&p)
        {
            if (p)
                p->Release();
            p = nullptr;
        }

        bool fail(std::string &error, const char *what, HRESULT hr)
        {
            char text[160];
            std::snprintf(text, sizeof(text), "Direct3D: %s failed (0x%08lX)", what, static_cast<unsigned long>(hr));
            error = text;
            return false;
        }

        bool createDevice(std::string &error)
        {
            const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                                D3D_FEATURE_LEVEL_10_0};
            const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED;
            HRESULT hr = E_FAIL;
            for (D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP})
            {
                hr = D3D11CreateDevice(nullptr, type, nullptr, flags, levels, UINT(std::size(levels)), D3D11_SDK_VERSION,
                                       &d3d.device, nullptr, &d3d.context);
                if (FAILED(hr)) // a runtime without 11_1
                    hr = D3D11CreateDevice(nullptr, type, nullptr, flags, levels + 1, UINT(std::size(levels) - 1),
                                           D3D11_SDK_VERSION, &d3d.device, nullptr, &d3d.context);
                if (SUCCEEDED(hr))
                {
                    rendererStats.warp = type == D3D_DRIVER_TYPE_WARP;
                    break;
                }
            }
            if (FAILED(hr))
                return fail(error, "D3D11CreateDevice", hr);
            IDXGIDevice *dxgi = nullptr;
            IDXGIAdapter *adapter = nullptr;
            if (SUCCEEDED(d3d.device->QueryInterface(IID_PPV_ARGS(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&adapter)))
            {
                DXGI_ADAPTER_DESC desc{};
                adapter->GetDesc(&desc);
                char name[128] = {};
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
                rendererStats.adapter = name;
                rendererStats.api = "Direct3D 11";
            }
            release(adapter);
            release(dxgi);
            return true;
        }

        bool createTarget(std::string &error)
        {
            ID3D11Texture2D *buffer = nullptr;
            HRESULT hr;
            if (d3d.swapChain)
                hr = d3d.swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer));
            else
            {
                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = UINT(d3d.width);
                desc.Height = UINT(d3d.height);
                desc.MipLevels = desc.ArraySize = 1;
                desc.Format = kFormat;
                desc.SampleDesc.Count = 1;
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = D3D11_BIND_RENDER_TARGET;
                hr = d3d.device->CreateTexture2D(&desc, nullptr, &d3d.offscreen);
                if (SUCCEEDED(hr))
                {
                    buffer = d3d.offscreen;
                    buffer->AddRef();
                }
            }
            if (FAILED(hr))
                return fail(error, "back buffer", hr);
            hr = d3d.device->CreateRenderTargetView(buffer, nullptr, &d3d.rtv);
            buffer->Release();
            return SUCCEEDED(hr) || fail(error, "CreateRenderTargetView", hr);
        }

        void releaseTarget()
        {
            if (d3d.context)
                d3d.context->OMSetRenderTargets(0, nullptr, nullptr);
            release(d3d.rtv);
            release(d3d.offscreen);
        }

        bool createSwapChain(std::string &error)
        {
            IDXGIDevice *dxgi = nullptr;
            IDXGIAdapter *adapter = nullptr;
            IDXGIFactory2 *factory = nullptr;
            HRESULT hr = d3d.device->QueryInterface(IID_PPV_ARGS(&dxgi));
            if (SUCCEEDED(hr))
                hr = dxgi->GetAdapter(&adapter);
            if (SUCCEEDED(hr))
                hr = adapter->GetParent(IID_PPV_ARGS(&factory));
            release(adapter);
            release(dxgi);
            if (FAILED(hr))
                return fail(error, "IDXGIFactory2", hr);
            IDXGIFactory5 *factory5 = nullptr;
            if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory5))))
            {
                BOOL allow = FALSE;
                d3d.tearing = SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))) && allow;
                factory5->Release();
            }
            d3d.swapFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
                            (d3d.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);
            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Width = UINT(d3d.width);
            desc.Height = UINT(d3d.height);
            desc.Format = kFormat;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2;
            desc.Scaling = DXGI_SCALING_NONE; // while resizing, old frames unstretched
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            desc.Flags = d3d.swapFlags;
            hr = factory->CreateSwapChainForHwnd(d3d.device, d3d.hwnd, &desc, nullptr, nullptr, &d3d.swapChain);
            if (SUCCEEDED(hr))
                factory->MakeWindowAssociation(d3d.hwnd, DXGI_MWA_NO_ALT_ENTER);
            factory->Release();
            if (FAILED(hr))
                return fail(error, "CreateSwapChainForHwnd", hr);
            IDXGISwapChain2 *chain2 = nullptr;
            if (SUCCEEDED(d3d.swapChain->QueryInterface(IID_PPV_ARGS(&chain2))))
            {
                chain2->SetMaximumFrameLatency(1);
                d3d.frameWaitable = chain2->GetFrameLatencyWaitableObject();
                chain2->Release();
            }
            rendererStats.tearing = d3d.tearing;
            return true;
        }

        bool createPipeline(std::string &error)
        {
            HRESULT hr = d3d.device->CreateVertexShader(g_ui_vs, sizeof(g_ui_vs), nullptr, &d3d.vs);
            if (SUCCEEDED(hr))
                hr = d3d.device->CreatePixelShader(g_ui_ps, sizeof(g_ui_ps), nullptr, &d3d.ps);
            if (FAILED(hr))
                return fail(error, "shaders", hr);
            // every attribute is per instance; the vertex shader makes the corners from SV_VertexID
            auto element = [](const char *name, UINT index, DXGI_FORMAT format, UINT offset) {
                return D3D11_INPUT_ELEMENT_DESC{name, index, format, 0, offset, D3D11_INPUT_PER_INSTANCE_DATA, 1};
            };
            const D3D11_INPUT_ELEMENT_DESC layout[] = {
                element("RECT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, offsetof(Instance, rect)),
                element("UV", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, offsetof(Instance, uv)),
                element("RADII", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, offsetof(Instance, radii)),
                element("CLIP", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, offsetof(Instance, clip)),
                element("COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, offsetof(Instance, color0)),
                element("COLOR", 1, DXGI_FORMAT_R8G8B8A8_UNORM, offsetof(Instance, color1)),
                element("COLOR", 2, DXGI_FORMAT_R8G8B8A8_UNORM, offsetof(Instance, border)),
                element("KIND", 0, DXGI_FORMAT_R32_UINT, offsetof(Instance, kind)),
                element("PARAMS", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, offsetof(Instance, params)),
            };
            hr = d3d.device->CreateInputLayout(layout, UINT(std::size(layout)), g_ui_vs, sizeof(g_ui_vs), &d3d.layout);
            if (FAILED(hr))
                return fail(error, "CreateInputLayout", hr);

            D3D11_BLEND_DESC blend{}; // premultiplied alpha
            D3D11_RENDER_TARGET_BLEND_DESC &rt = blend.RenderTarget[0];
            rt.BlendEnable = TRUE;
            rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
            rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
            rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            D3D11_RASTERIZER_DESC raster{};
            raster.FillMode = D3D11_FILL_SOLID;
            raster.CullMode = D3D11_CULL_NONE;
            raster.DepthClipEnable = TRUE;
            D3D11_SAMPLER_DESC sampler{};
            sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            D3D11_BUFFER_DESC cb{};
            cb.ByteWidth = 16;
            cb.Usage = D3D11_USAGE_DYNAMIC;
            cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(hr = d3d.device->CreateBlendState(&blend, &d3d.blend)) ||
                FAILED(hr = d3d.device->CreateRasterizerState(&raster, &d3d.raster)) ||
                FAILED(hr = d3d.device->CreateSamplerState(&sampler, &d3d.sampler)) ||
                FAILED(hr = d3d.device->CreateBuffer(&cb, nullptr, &d3d.constants)))
                return fail(error, "pipeline state", hr);
            return true;
        }

        // one map per frame; WRITE_DISCARD hands over fresh memory while the GPU may read last frame's: no stall
        size_t uploadInstances()
        {
            const size_t count = draw.instances.size();
            if (!count)
                return 0;
            if (count > d3d.instanceCapacity)
            {
                release(d3d.instances);
                size_t capacity = std::max<size_t>(d3d.instanceCapacity * 2, 4096);
                while (capacity < count)
                    capacity *= 2;
                D3D11_BUFFER_DESC desc{};
                desc.ByteWidth = UINT(capacity * sizeof(Instance));
                desc.Usage = D3D11_USAGE_DYNAMIC;
                desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                if (FAILED(d3d.device->CreateBuffer(&desc, nullptr, &d3d.instances)))
                {
                    d3d.instanceCapacity = 0;
                    return 0;
                }
                d3d.instanceCapacity = capacity;
            }
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(d3d.context->Map(d3d.instances, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
                return 0;
            std::memcpy(mapped.pData, draw.instances.data(), count * sizeof(Instance));
            d3d.context->Unmap(d3d.instances, 0);
            rendererStats.uploadBytes += count * sizeof(Instance);
            return count;
        }

        // only the part of the atlas new glyphs touched
        void uploadAtlas()
        {
            if (!d3d.atlasTexture || d3d.atlasGeneration != atlas.generation || d3d.atlasSize != atlas.size)
            {
                release(d3d.atlasView);
                release(d3d.atlasTexture);
                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = desc.Height = UINT(atlas.size);
                desc.MipLevels = desc.ArraySize = 1;
                desc.Format = DXGI_FORMAT_R8_UNORM;
                desc.SampleDesc.Count = 1;
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                if (FAILED(d3d.device->CreateTexture2D(&desc, nullptr, &d3d.atlasTexture)) ||
                    FAILED(d3d.device->CreateShaderResourceView(d3d.atlasTexture, nullptr, &d3d.atlasView)))
                    return;
                d3d.atlasGeneration = atlas.generation;
                d3d.atlasSize = atlas.size;
                atlas.markAllDirty();
            }
            if (atlas.dirtyX0 >= atlas.dirtyX1 || atlas.dirtyY0 >= atlas.dirtyY1)
                return;
            const D3D11_BOX box = {UINT(atlas.dirtyX0), UINT(atlas.dirtyY0), 0, UINT(atlas.dirtyX1), UINT(atlas.dirtyY1), 1};
            const uint8_t *src = atlas.pixels.data() + size_t(atlas.dirtyY0) * size_t(atlas.size) + size_t(atlas.dirtyX0);
            d3d.context->UpdateSubresource(d3d.atlasTexture, 0, &box, src, UINT(atlas.size), 0);
            rendererStats.uploadBytes += size_t(atlas.dirtyX1 - atlas.dirtyX0) * size_t(atlas.dirtyY1 - atlas.dirtyY0);
            atlas.dirtyX0 = atlas.dirtyY0 = atlas.size;
            atlas.dirtyX1 = atlas.dirtyY1 = 0;
        }

        void uploadImageAtlas()
        {
            if (d3d.imageTexture && d3d.imageGeneration == imageAtlas.generation)
                return;
            release(d3d.imageView);
            release(d3d.imageTexture);
            const bool hasImages = !imageAtlas.pixels.empty() && imageAtlas.width > 0 && imageAtlas.height > 0;
            const uint32_t zero = 0;
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = UINT(hasImages ? imageAtlas.width : 1);
            desc.Height = UINT(hasImages ? imageAtlas.height : 1);
            desc.MipLevels = desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_IMMUTABLE;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA sub{};
            sub.pSysMem = hasImages ? static_cast<const void *>(imageAtlas.pixels.data()) : static_cast<const void *>(&zero);
            sub.SysMemPitch = UINT(desc.Width * 4);
            if (FAILED(d3d.device->CreateTexture2D(&desc, &sub, &d3d.imageTexture)) ||
                FAILED(d3d.device->CreateShaderResourceView(d3d.imageTexture, nullptr, &d3d.imageView)))
                return;
            d3d.imageGeneration = imageAtlas.generation;
            if (hasImages)
                rendererStats.uploadBytes += imageAtlas.pixels.size();
        }

        bool resize(int width, int height)
        {
            if (width <= 0 || height <= 0)
                return false;
            releaseTarget();
            std::string error;
            if (d3d.swapChain)
            {
                d3d.context->Flush();
                if (FAILED(d3d.swapChain->ResizeBuffers(0, UINT(width), UINT(height), DXGI_FORMAT_UNKNOWN, d3d.swapFlags)))
                    return false;
            }
            d3d.width = width;
            d3d.height = height;
            return createTarget(error);
        }

        void destroy()
        {
            if (d3d.context)
                d3d.context->ClearState();
            releaseTarget();
            if (d3d.frameWaitable)
                CloseHandle(d3d.frameWaitable);
            d3d.frameWaitable = nullptr;
            release(d3d.imageView);
            release(d3d.imageTexture);
            release(d3d.atlasView);
            release(d3d.atlasTexture);
            release(d3d.instances);
            release(d3d.constants);
            release(d3d.sampler);
            release(d3d.raster);
            release(d3d.blend);
            release(d3d.layout);
            release(d3d.ps);
            release(d3d.vs);
            release(d3d.swapChain);
            release(d3d.context);
            release(d3d.device);
            d3d.instanceCapacity = 0;
            d3d.atlasGeneration = 0;
            d3d.imageGeneration = 0;
            d3d.frameWaited = false;
        }

        bool create(std::string &error)
        {
            if (!createDevice(error))
                return false;
            if (d3d.hwnd && !createSwapChain(error))
                return false;
            return createTarget(error) && createPipeline(error);
        }
    }

    bool rendererInit(void *window, int width, int height, std::string &error)
    {
        d3d.hwnd = static_cast<HWND>(window);
        d3d.width = std::max(width, 1);
        d3d.height = std::max(height, 1);
        if (create(error))
            return true;
        destroy();
        return false;
    }

    void rendererShutdown()
    {
        destroy();
        d3d = State();
    }

    // One wait per present: a second wait before presenting would block on a frame never queued (Win32's modal move
    // loop can draw and present between the main loop's wait and its present).
    void rendererWaitForFrame()
    {
        if (d3d.frameWaited)
            return;
        if (d3d.frameWaitable)
            WaitForSingleObjectEx(d3d.frameWaitable, 1000, TRUE);
        d3d.frameWaited = true;
    }

    void rendererRender(core::Color clear, int width, int height)
    {
        rendererStats.instances = rendererStats.drawCalls = 0;
        rendererStats.uploadBytes = 0;
        if (!d3d.device)
            return;
        if ((width != d3d.width || height != d3d.height) && !resize(width, height))
            return;
        if (!d3d.rtv)
            return;
        uploadAtlas();
        uploadImageAtlas();
        const size_t count = uploadInstances();

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(d3d.context->Map(d3d.constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            auto *c = static_cast<FrameConstants *>(mapped.pData);
            c->viewport[0] = float(d3d.width);
            c->viewport[1] = float(d3d.height);
            c->atlasInvSize[0] = c->atlasInvSize[1] = 1.0f / float(std::max(atlas.size, 1));
            d3d.context->Unmap(d3d.constants, 0);
        }
        float colour[4];
        for (int i = 0; i < 4; ++i)
            colour[i] = float((clear >> (i * 8)) & 0xFF) / 255.0f;
        d3d.context->OMSetRenderTargets(1, &d3d.rtv, nullptr);
        d3d.context->ClearRenderTargetView(d3d.rtv, colour);
        if (!count)
            return;
        const D3D11_VIEWPORT viewport = {0, 0, float(d3d.width), float(d3d.height), 0, 1};
        const UINT stride = sizeof(Instance), offset = 0;
        d3d.context->RSSetViewports(1, &viewport);
        d3d.context->RSSetState(d3d.raster);
        d3d.context->OMSetBlendState(d3d.blend, nullptr, 0xFFFFFFFF);
        d3d.context->IASetInputLayout(d3d.layout);
        d3d.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        d3d.context->IASetVertexBuffers(0, 1, &d3d.instances, &stride, &offset);
        d3d.context->VSSetShader(d3d.vs, nullptr, 0);
        d3d.context->VSSetConstantBuffers(0, 1, &d3d.constants);
        d3d.context->PSSetShader(d3d.ps, nullptr, 0);
        d3d.context->PSSetConstantBuffers(0, 1, &d3d.constants);
        ID3D11ShaderResourceView *srvs[2] = {d3d.atlasView, d3d.imageView};
        d3d.context->PSSetShaderResources(0, 2, srvs);
        d3d.context->PSSetSamplers(0, 1, &d3d.sampler);
        d3d.context->DrawInstanced(4, UINT(count), 0, 0);
        rendererStats.instances = int(count);
        rendererStats.drawCalls = 1;
    }

    void rendererPresent(bool vsync)
    {
        d3d.frameWaited = false;
        HRESULT hr = S_OK;
        if (d3d.swapChain)
            hr = vsync ? d3d.swapChain->Present(1, 0) : d3d.swapChain->Present(0, d3d.tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
        else if (d3d.context)
            d3d.context->Flush();
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || d3d.loseDevice)
        {
            // driver update, GPU reset, adapter removed: start over from what the CPU has
            d3d.loseDevice = false;
            HWND hwnd = d3d.hwnd;
            const int w = d3d.width, h = d3d.height;
            destroy();
            d3d.hwnd = hwnd;
            d3d.width = w;
            d3d.height = h;
            std::string error;
            if (!create(error))
                destroy();
            atlas.markAllDirty();
        }
    }

    bool rendererReadback(std::vector<uint8_t> &rgba, int &width, int &height)
    {
        if (!d3d.device || !d3d.rtv)
            return false;
        ID3D11Resource *source = nullptr;
        d3d.rtv->GetResource(&source);
        D3D11_TEXTURE2D_DESC desc{};
        static_cast<ID3D11Texture2D *>(source)->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        ID3D11Texture2D *staging = nullptr;
        bool ok = SUCCEEDED(d3d.device->CreateTexture2D(&desc, nullptr, &staging));
        if (ok)
        {
            d3d.context->CopyResource(staging, source);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            ok = SUCCEEDED(d3d.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped));
            if (ok)
            {
                width = int(desc.Width);
                height = int(desc.Height);
                rgba.resize(size_t(width) * size_t(height) * 4);
                for (int y = 0; y < height; ++y)
                {
                    const uint8_t *src = static_cast<const uint8_t *>(mapped.pData) + size_t(y) * mapped.RowPitch;
                    uint8_t *dst = &rgba[size_t(y) * size_t(width) * 4];
                    for (int x = 0; x < width; ++x)
                    {
                        dst[x * 4 + 0] = src[x * 4 + 2]; // BGRA -> RGBA, opaque
                        dst[x * 4 + 1] = src[x * 4 + 1];
                        dst[x * 4 + 2] = src[x * 4 + 0];
                        dst[x * 4 + 3] = 255;
                    }
                }
                d3d.context->Unmap(staging, 0);
            }
        }
        release(staging);
        source->Release();
        return ok;
    }

    void rendererSimulateDeviceLoss() { d3d.loseDevice = true; }
}
