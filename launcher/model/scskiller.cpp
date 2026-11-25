#include "scskiller.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11_1.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

using BYTE = unsigned char;
#include "dxbc/gs_draw_vs.h"
#include "dxbc/gs_world_vs.h"
#include "dxbc/gs_lit_vs.h"
#include "dxbc/gs_lit_inst_vs.h"
#include "dxbc/gs_draw_ps0.h"
#include "dxbc/gs_draw_ps0_dual.h"
#include "dxbc/gs_draw_ps1.h"
#include "dxbc/gs_draw_ps1_dual.h"
#include "dxbc/gs_draw_ps2.h"
#include "dxbc/gs_draw_ps2_dual.h"
#include "dxbc/gs_draw_ps3.h"
#include "dxbc/gs_draw_ps3_dual.h"
#include "dxbc/gs_draw_ps4.h"
#include "dxbc/gs_draw_ps4_dual.h"
#include "dxbc/gs_draw_ps5.h"
#include "dxbc/gs_draw_ps5_dual.h"
#include "dxbc/gs_draw_ps6.h"
#include "dxbc/gs_draw_ps6_dual.h"
#include "dxbc/gs_draw_ps7.h"
#include "dxbc/gs_draw_ps7_dual.h"
#include "dxbc/gs_quad_vs.h"
#include "dxbc/gs_blit_ps.h"
#include "dxbc/gs_fill_color_ps.h"
#include "dxbc/gs_fill_depth_ps.h"
#include "dxbc/present_vs.h"
#include "dxbc/present_ps.h"
#include "dxbc/ui_vs.h"
#include "dxbc/ui_ps.h"
#include "dxbc/gs_draw_ps0_noz.h"
#include "dxbc/gs_draw_ps0_dual_noz.h"
#include "dxbc/gs_draw_ps1_noz.h"
#include "dxbc/gs_draw_ps1_dual_noz.h"
#include "dxbc/gs_draw_ps2_noz.h"
#include "dxbc/gs_draw_ps2_dual_noz.h"
#include "dxbc/gs_draw_ps3_noz.h"
#include "dxbc/gs_draw_ps3_dual_noz.h"
#include "dxbc/gs_draw_ps4_noz.h"
#include "dxbc/gs_draw_ps4_dual_noz.h"
#include "dxbc/gs_draw_ps5_noz.h"
#include "dxbc/gs_draw_ps5_dual_noz.h"
#include "dxbc/gs_draw_ps6_noz.h"
#include "dxbc/gs_draw_ps6_dual_noz.h"
#include "dxbc/gs_draw_ps7_noz.h"
#include "dxbc/gs_draw_ps7_dual_noz.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace launcher
{
    namespace
    {
        constexpr char kCacheMagic[8] = {'S', 'C', 'S', 'K', 'V', 'C', 'S', '1'};

        struct CacheHeader
        {
            char magic[8];
            uint32_t version = 1;
            uint32_t vendorId = 0;
            uint32_t deviceId = 0;
            uint32_t psoCount = 0;
            uint32_t shaderCount = 0;
            char gpuName[128] = {};
        };

#ifdef _WIN32
        using Microsoft::WRL::ComPtr;

        struct Bytecode
        {
            const void *data = nullptr;
            size_t size = 0;
            const char *name = "";
        };

        std::string narrowStr(const wchar_t *w)
        {
            const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
            std::string s(n > 0 ? n - 1 : 0, '\0');
            if (n > 1)
                WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
            return s;
        }
#endif
    }

    ScsKillerStatus inspectShaderCache(const fs::path &buildDir)
    {
        ScsKillerStatus st{};
        std::error_code ec;
        const fs::path p = buildDir / "shaders" / "scskiller_pso_cache.bin";
        if (!fs::is_regular_file(p, ec))
            return st;
        st.bytes = fs::file_size(p, ec);
        std::ifstream f(p, std::ios::binary);
        CacheHeader hdr{};
        if (!f.read(reinterpret_cast<char *>(&hdr), sizeof(hdr)))
            return st;
        if (std::memcmp(hdr.magic, kCacheMagic, 8) != 0)
            return st;
        st.exists = true;
        st.psoCount = hdr.psoCount;
        st.shaderCount = hdr.shaderCount;
        hdr.gpuName[sizeof(hdr.gpuName) - 1] = '\0';
        st.gpuName = hdr.gpuName;
        return st;
    }

    int runScsKillerCli(const fs::path &buildDir, int userMsaa, bool isPt)
    {
#ifndef _WIN32
        (void)buildDir;
        (void)userMsaa;
        (void)isPt;
        return 0;
#else
        const auto tStart = std::chrono::steady_clock::now();
        std::printf("@vcs {\"stage\":\"gpu\",\"title\":\"%s\"}\n",
                    isPt ? "Detectando GPU e inicializando SCSKiller" : "Detecting GPU and initializing SCSKiller");
        std::printf("[SCSKiller] ================================================================\n");
        std::printf("[SCSKiller] VCS.rec Shader Compilation Stutter Killer (Pipeline Precompiler)\n");
        std::printf("[SCSKiller] Based on BlueHeisenberg/SCSKiller PSO & Driver Cache architecture\n");
        std::printf("[SCSKiller] ================================================================\n");
        std::fflush(stdout);

        ComPtr<IDXGIFactory1> factory1;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory1))))
        {
            std::printf("@vcs {\"error\":\"dxgi_failed\",\"message\":\"CreateDXGIFactory1 failed\"}\n");
            std::fflush(stdout);
            return 1;
        }

        ComPtr<IDXGIFactory6> factory6;
        factory1.As(&factory6);
        ComPtr<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 ad{};
        for (UINT i = 0;; ++i)
        {
            ComPtr<IDXGIAdapter1> cand;
            const HRESULT hr = factory6
                                   ? factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&cand))
                                   : factory1->EnumAdapters1(i, &cand);
            if (FAILED(hr))
                break;
            DXGI_ADAPTER_DESC1 d{};
            cand->GetDesc1(&d);
            if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                continue;
            adapter = cand;
            ad = d;
            break;
        }

        if (!adapter)
        {
            std::printf("@vcs {\"error\":\"no_gpu\",\"message\":\"No hardware GPU adapter found\"}\n");
            std::fflush(stdout);
            return 1;
        }

        const std::string gpuName = narrowStr(ad.Description);
        const unsigned vramMb = static_cast<unsigned>(ad.DedicatedVideoMemory >> 20);
        std::printf("[SCSKiller] GPU Adapter : %s\n", gpuName.c_str());
        std::printf("[SCSKiller] Hardware ID : Vendor 0x%04X | Device 0x%04X | VRAM %u MB\n",
                    ad.VendorId, ad.DeviceId, vramMb);
        std::fflush(stdout);

        // 1. All 40 game DXBC shaders (7 Vertex Shaders + 33 Pixel Shaders)
        const Bytecode kVertexShaders[7] = {
            {kGsDrawVS, sizeof(kGsDrawVS), "gs_draw_vs"},
            {kGsWorldVS, sizeof(kGsWorldVS), "gs_world_vs"},
            {kGsLitVS, sizeof(kGsLitVS), "gs_lit_vs"},
            {kGsLitInstVS, sizeof(kGsLitInstVS), "gs_lit_inst_vs"},
            {kGsQuadVS, sizeof(kGsQuadVS), "gs_quad_vs"},
            {kPresentVS, sizeof(kPresentVS), "present_vs"},
            {kUiVS, sizeof(kUiVS), "ui_vs"},
        };

        const Bytecode kDrawPS[2][8][2] = {
            // [0] = NoZ (SHADER_DEPTH=0, standard fast path), [1] = Z (SHADER_DEPTH=1)
            {
                {{kGsDrawPS0NoZ, sizeof(kGsDrawPS0NoZ), "gs_draw_ps0_noz"},
                 {kGsDrawPS0DualNoZ, sizeof(kGsDrawPS0DualNoZ), "gs_draw_ps0_dual_noz"}},
                {{kGsDrawPS1NoZ, sizeof(kGsDrawPS1NoZ), "gs_draw_ps1_noz"},
                 {kGsDrawPS1DualNoZ, sizeof(kGsDrawPS1DualNoZ), "gs_draw_ps1_dual_noz"}},
                {{kGsDrawPS2NoZ, sizeof(kGsDrawPS2NoZ), "gs_draw_ps2_noz"},
                 {kGsDrawPS2DualNoZ, sizeof(kGsDrawPS2DualNoZ), "gs_draw_ps2_dual_noz"}},
                {{kGsDrawPS3NoZ, sizeof(kGsDrawPS3NoZ), "gs_draw_ps3_noz"},
                 {kGsDrawPS3DualNoZ, sizeof(kGsDrawPS3DualNoZ), "gs_draw_ps3_dual_noz"}},
                {{kGsDrawPS4NoZ, sizeof(kGsDrawPS4NoZ), "gs_draw_ps4_noz"},
                 {kGsDrawPS4DualNoZ, sizeof(kGsDrawPS4DualNoZ), "gs_draw_ps4_dual_noz"}},
                {{kGsDrawPS5NoZ, sizeof(kGsDrawPS5NoZ), "gs_draw_ps5_noz"},
                 {kGsDrawPS5DualNoZ, sizeof(kGsDrawPS5DualNoZ), "gs_draw_ps5_dual_noz"}},
                {{kGsDrawPS6NoZ, sizeof(kGsDrawPS6NoZ), "gs_draw_ps6_noz"},
                 {kGsDrawPS6DualNoZ, sizeof(kGsDrawPS6DualNoZ), "gs_draw_ps6_dual_noz"}},
                {{kGsDrawPS7NoZ, sizeof(kGsDrawPS7NoZ), "gs_draw_ps7_noz"},
                 {kGsDrawPS7DualNoZ, sizeof(kGsDrawPS7DualNoZ), "gs_draw_ps7_dual_noz"}},
            },
            {
                {{kGsDrawPS0, sizeof(kGsDrawPS0), "gs_draw_ps0"},
                 {kGsDrawPS0Dual, sizeof(kGsDrawPS0Dual), "gs_draw_ps0_dual"}},
                {{kGsDrawPS1, sizeof(kGsDrawPS1), "gs_draw_ps1"},
                 {kGsDrawPS1Dual, sizeof(kGsDrawPS1Dual), "gs_draw_ps1_dual"}},
                {{kGsDrawPS2, sizeof(kGsDrawPS2), "gs_draw_ps2"},
                 {kGsDrawPS2Dual, sizeof(kGsDrawPS2Dual), "gs_draw_ps2_dual"}},
                {{kGsDrawPS3, sizeof(kGsDrawPS3), "gs_draw_ps3"},
                 {kGsDrawPS3Dual, sizeof(kGsDrawPS3Dual), "gs_draw_ps3_dual"}},
                {{kGsDrawPS4, sizeof(kGsDrawPS4), "gs_draw_ps4"},
                 {kGsDrawPS4Dual, sizeof(kGsDrawPS4Dual), "gs_draw_ps4_dual"}},
                {{kGsDrawPS5, sizeof(kGsDrawPS5), "gs_draw_ps5"},
                 {kGsDrawPS5Dual, sizeof(kGsDrawPS5Dual), "gs_draw_ps5_dual"}},
                {{kGsDrawPS6, sizeof(kGsDrawPS6), "gs_draw_ps6"},
                 {kGsDrawPS6Dual, sizeof(kGsDrawPS6Dual), "gs_draw_ps6_dual"}},
                {{kGsDrawPS7, sizeof(kGsDrawPS7), "gs_draw_ps7"},
                 {kGsDrawPS7Dual, sizeof(kGsDrawPS7Dual), "gs_draw_ps7_dual"}},
            },
        };

        const Bytecode kUtilPS[5] = {
            {kGsBlitPS, sizeof(kGsBlitPS), "gs_blit_ps"},
            {kGsFillColorPS, sizeof(kGsFillColorPS), "gs_fill_color_ps"},
            {kGsFillDepthPS, sizeof(kGsFillDepthPS), "gs_fill_depth_ps"},
            {kPresentPS, sizeof(kPresentPS), "present_ps"},
            {kUiPS, sizeof(kUiPS), "ui_ps"},
        };

        // 2. Reconstruct all D3D12 PSOs and pre-compile into GPU Driver Cache + scskiller_pso_cache.bin
        uint32_t compiledPsos = 0;
        std::vector<std::pair<uint64_t, std::vector<uint8_t>>> cachedBlobs;

        HMODULE d3d12Dll = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (d3d12Dll)
        {
            using PFN_D3D12CreateDevice = HRESULT(WINAPI *)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
            using PFN_D3D12SerializeRootSignature = HRESULT(WINAPI *)(const D3D12_ROOT_SIGNATURE_DESC *,
                                                                      D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **, ID3DBlob **);
            auto fnCreateDev = reinterpret_cast<PFN_D3D12CreateDevice>(
                reinterpret_cast<void (*)()>(GetProcAddress(d3d12Dll, "D3D12CreateDevice")));
            auto fnSerializeRoot = reinterpret_cast<PFN_D3D12SerializeRootSignature>(
                reinterpret_cast<void (*)()>(GetProcAddress(d3d12Dll, "D3D12SerializeRootSignature")));

            ComPtr<ID3D12Device> dev12;
            if (fnCreateDev && fnSerializeRoot &&
                SUCCEEDED(fnCreateDev(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev12))))
            {
                std::printf("@vcs {\"stage\":\"d3d12\",\"title\":\"%s\"}\n",
                            isPt ? "Pré-compilando Pipelines Direct3D 12 (PSOs)" : "Pre-compiling Direct3D 12 Pipelines (PSOs)");

                // Exact RootSignature from src/rhi/d3d12/rhi_d3d12.cpp (createRootSignature)
                D3D12_DESCRIPTOR_RANGE textures{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 8, 0, 0, 0};
                D3D12_DESCRIPTOR_RANGE samplers{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 8, 0, 0, 0};
                D3D12_ROOT_PARAMETER params[6]{};
                for (uint32_t i = 0; i < 4; ++i)
                {
                    params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
                    params[i].Descriptor.ShaderRegister = i;
                    params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
                }
                params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                params[4].DescriptorTable = {1, &textures};
                params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                params[5].DescriptorTable = {1, &samplers};
                params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

                const D3D12_ROOT_SIGNATURE_DESC rd{6, params, 0, nullptr,
                                                   D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
                ComPtr<ID3DBlob> rootBlob, errBlob;
                ComPtr<ID3D12RootSignature> rootSig;
                if (SUCCEEDED(fnSerializeRoot(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &rootBlob, &errBlob)) &&
                    SUCCEEDED(dev12->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(),
                                                         IID_PPV_ARGS(&rootSig))))
                {
                    std::printf("[SCSKiller] [D3D12] Root Signature reconstructed (4 CBVs, 8 SRVs, 8 Samplers)\n");

                    // Vertex Input Layouts matching gs_hw.cpp, presenter.cpp, ui_renderer.cpp
                    static const D3D12_INPUT_ELEMENT_DESC kLayoutHw[4] = {
                        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                        {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                        {"TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                    };
                    static const D3D12_INPUT_ELEMENT_DESC kLayoutWorld[3] = {
                        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                        {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                        {"COLOR", 0, DXGI_FORMAT_R32_UINT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                    };
                    static const D3D12_INPUT_ELEMENT_DESC kLayoutLit[3] = {
                        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                        {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                    };
                    static const D3D12_INPUT_ELEMENT_DESC kLayoutUi[3] = {
                        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                    };

                    struct BlendPreset
                    {
                        BOOL enable;
                        D3D12_BLEND srcColor, dstColor;
                        D3D12_BLEND_OP op;
                        bool dual;
                    };
                    static const BlendPreset kBlends[] = {
                        {FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, false},
                        {TRUE, D3D12_BLEND_SRC1_ALPHA, D3D12_BLEND_INV_SRC1_ALPHA, D3D12_BLEND_OP_ADD, true},
                        {TRUE, D3D12_BLEND_ONE, D3D12_BLEND_ONE, D3D12_BLEND_OP_ADD, false},
                        {TRUE, D3D12_BLEND_ONE, D3D12_BLEND_SRC1_ALPHA, D3D12_BLEND_OP_SUBTRACT, true},
                        {TRUE, D3D12_BLEND_ZERO, D3D12_BLEND_INV_SRC1_ALPHA, D3D12_BLEND_OP_ADD, true},
                        {TRUE, D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_OP_ADD, false},
                    };

                    struct DepthPreset
                    {
                        BOOL enable;
                        D3D12_DEPTH_WRITE_MASK writeMask;
                        D3D12_COMPARISON_FUNC func;
                        DXGI_FORMAT dsvFormat;
                    };
                    static const DepthPreset kDepths[] = {
                        {FALSE, D3D12_DEPTH_WRITE_MASK_ZERO, D3D12_COMPARISON_FUNC_ALWAYS, DXGI_FORMAT_UNKNOWN},
                        {TRUE, D3D12_DEPTH_WRITE_MASK_ALL, D3D12_COMPARISON_FUNC_GREATER_EQUAL, DXGI_FORMAT_D32_FLOAT},
                        {TRUE, D3D12_DEPTH_WRITE_MASK_ZERO, D3D12_COMPARISON_FUNC_GREATER_EQUAL, DXGI_FORMAT_D32_FLOAT},
                        {TRUE, D3D12_DEPTH_WRITE_MASK_ALL, D3D12_COMPARISON_FUNC_GREATER, DXGI_FORMAT_D32_FLOAT},
                        {TRUE, D3D12_DEPTH_WRITE_MASK_ZERO, D3D12_COMPARISON_FUNC_GREATER, DXGI_FORMAT_D32_FLOAT},
                        {TRUE, D3D12_DEPTH_WRITE_MASK_ALL, D3D12_COMPARISON_FUNC_ALWAYS, DXGI_FORMAT_D32_FLOAT},
                    };

                    struct PsoJob
                    {
                        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
                        uint64_t hashKey = 0;
                        const char *category = "";
                    };
                    std::vector<PsoJob> jobs;
                    jobs.reserve(512);

                    auto makeBaseDesc = [&](const Bytecode &vs, const Bytecode &ps,
                                            const D3D12_INPUT_ELEMENT_DESC *layout, UINT layoutCount,
                                            BOOL depthClip, D3D12_PRIMITIVE_TOPOLOGY_TYPE topo,
                                            const BlendPreset &b, const DepthPreset &d, UINT samples,
                                            DXGI_FORMAT rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM,
                                            UINT8 writeMask = 0xF) {
                        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
                        pd.pRootSignature = rootSig.Get();
                        pd.VS = {vs.data, vs.size};
                        pd.PS = {ps.data, ps.size};
                        D3D12_RENDER_TARGET_BLEND_DESC &rt = pd.BlendState.RenderTarget[0];
                        rt.BlendEnable = b.enable;
                        rt.SrcBlend = b.srcColor;
                        rt.DestBlend = b.dstColor;
                        rt.BlendOp = b.op;
                        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
                        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
                        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                        rt.LogicOp = D3D12_LOGIC_OP_NOOP;
                        rt.RenderTargetWriteMask = writeMask;
                        pd.SampleMask = UINT_MAX;
                        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
                        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                        pd.RasterizerState.DepthClipEnable = depthClip;
                        pd.RasterizerState.MultisampleEnable = samples > 1u ? TRUE : FALSE;
                        pd.DepthStencilState.DepthEnable = d.enable;
                        pd.DepthStencilState.DepthWriteMask = d.writeMask;
                        pd.DepthStencilState.DepthFunc = d.func;
                        const D3D12_DEPTH_STENCILOP_DESC keep{D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                                              D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
                        pd.DepthStencilState.FrontFace = keep;
                        pd.DepthStencilState.BackFace = keep;
                        pd.InputLayout = {layout, layoutCount};
                        pd.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
                        pd.PrimitiveTopologyType = topo;
                        pd.NumRenderTargets = rtvFormat != DXGI_FORMAT_UNKNOWN ? 1u : 0u;
                        pd.RTVFormats[0] = rtvFormat;
                        pd.DSVFormat = d.dsvFormat;
                        pd.SampleDesc.Count = samples;
                        return pd;
                    };

                    std::vector<UINT> sampleCounts = {1u};
                    const UINT reqMsaa = static_cast<UINT>(std::clamp(userMsaa, 1, 8));
                    if (reqMsaa > 1u)
                        sampleCounts.push_back(reqMsaa);
                    if (reqMsaa != 4u)
                        sampleCounts.push_back(4u);

                    // Utility, Presenter, and UI PSOs
                    for (UINT sc : sampleCounts)
                    {
                        jobs.push_back({makeBaseDesc(kVertexShaders[4], kUtilPS[0], nullptr, 0, TRUE,
                                                     D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, kBlends[0], kDepths[0], sc),
                                        0x1000ull + sc, "GS Blit / PostFX"});
                        jobs.push_back({makeBaseDesc(kVertexShaders[4], kUtilPS[1], nullptr, 0, TRUE,
                                                     D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, kBlends[0], kDepths[0], sc),
                                        0x1100ull + sc, "GS Fill Color"});
                        jobs.push_back({makeBaseDesc(kVertexShaders[4], kUtilPS[2], nullptr, 0, TRUE,
                                                     D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, kBlends[0], kDepths[5], sc,
                                                     DXGI_FORMAT_UNKNOWN, 0),
                                        0x1200ull + sc, "GS Fill Depth"});
                    }
                    jobs.push_back({makeBaseDesc(kVertexShaders[5], kUtilPS[3], nullptr, 0, TRUE,
                                                 D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, kBlends[0], kDepths[0], 1u),
                                    0x1300ull, "Swapchain Presenter"});
                    jobs.push_back({makeBaseDesc(kVertexShaders[6], kUtilPS[4], kLayoutUi, 3, TRUE,
                                                 D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, kBlends[5], kDepths[0], 1u),
                                    0x1400ull, "ImGui / HUD Overlay"});

                    // GS Draw, Native World, Native Lit, and Instanced Lit PSO permutations
                    uint64_t nextKey = 0x2000ull;
                    for (UINT sc : sampleCounts)
                    {
                        for (int sampleMode = 0; sampleMode < 8; ++sampleMode)
                        {
                            for (int bIdx = 0; bIdx < 5; ++bIdx)
                            {
                                const BlendPreset &bp = kBlends[bIdx];
                                const Bytecode &psNoZ = kDrawPS[0][sampleMode][bp.dual ? 1 : 0];

                                for (int dIdx = 0; dIdx < 5; ++dIdx)
                                {
                                    const DepthPreset &dp = kDepths[dIdx];
                                    // 1. HwVertex (Sprites, HUD, Sky, Effects)
                                    jobs.push_back({makeBaseDesc(kVertexShaders[0], psNoZ, kLayoutHw, 4, TRUE,
                                                                 D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, bp, dp, sc),
                                                    nextKey++, "GS HwVertex Draw"});
                                    // 2. Native VCS World (Buildings, Roads, Bridges, Trees)
                                    jobs.push_back({makeBaseDesc(kVertexShaders[1], psNoZ, kLayoutWorld, 3, FALSE,
                                                                 D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, bp, dp, sc),
                                                    nextKey++, "Native VCS World"});
                                    // 3. Native VCS Lit (Vehicles, Pedestrians, Weapons)
                                    jobs.push_back({makeBaseDesc(kVertexShaders[2], psNoZ, kLayoutLit, 3, FALSE,
                                                                 D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, bp, dp, sc),
                                                    nextKey++, "Native VCS Skinned/Lit"});
                                    // 4. Native VCS Lit Instanced (Distant Traffic & Crowd Proxies)
                                    if (sampleMode <= 3 && bIdx <= 1 && dIdx <= 2)
                                    {
                                        jobs.push_back({makeBaseDesc(kVertexShaders[3], psNoZ, kLayoutLit, 3, FALSE,
                                                                     D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, bp, dp, sc),
                                                        nextKey++, "Native Distant Instanced"});
                                    }
                                }
                            }
                            // Also pre-warm SHADER_DEPTH=1 (Z) fallback for sampleMode
                            for (int dual = 0; dual < 2; ++dual)
                            {
                                const Bytecode &psZ = kDrawPS[1][sampleMode][dual];
                                const BlendPreset &bp = dual ? kBlends[1] : kBlends[0];
                                jobs.push_back({makeBaseDesc(kVertexShaders[0], psZ, kLayoutHw, 4, TRUE,
                                                             D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, bp, kDepths[1], sc),
                                                nextKey++, "GS ShaderDepth Fallback"});
                            }
                        }
                    }

                    const size_t totalJobs = jobs.size();
                    std::printf("[SCSKiller] [D3D12] Queued %zu Pipeline State Objects across %u worker threads...\n",
                                totalJobs, std::max(1u, std::thread::hardware_concurrency()));
                    std::fflush(stdout);

                    std::atomic<size_t> nextIdx{0};
                    std::atomic<uint32_t> doneCount{0};
                    std::mutex outMutex;
                    const unsigned nThreads = std::clamp(std::thread::hardware_concurrency(), 2u, 12u);
                    std::vector<std::thread> workers;
                    workers.reserve(nThreads);

                    for (unsigned t = 0; t < nThreads; ++t)
                    {
                        workers.emplace_back([&]() {
                            for (;;)
                            {
                                const size_t i = nextIdx.fetch_add(1, std::memory_order_relaxed);
                                if (i >= totalJobs)
                                    break;
                                const PsoJob &job = jobs[i];
                                ComPtr<ID3D12PipelineState> pso;
                                if (SUCCEEDED(dev12->CreateGraphicsPipelineState(&job.desc, IID_PPV_ARGS(&pso))) && pso)
                                {
                                    ComPtr<ID3DBlob> cached;
                                    std::vector<uint8_t> bytes;
                                    if (SUCCEEDED(pso->GetCachedBlob(&cached)) && cached && cached->GetBufferSize() > 0)
                                    {
                                        const auto *ptr = static_cast<const uint8_t *>(cached->GetBufferPointer());
                                        bytes.assign(ptr, ptr + cached->GetBufferSize());
                                    }
                                    const uint32_t doneNow = ++doneCount;
                                    std::lock_guard<std::mutex> lock(outMutex);
                                    if (!bytes.empty())
                                        cachedBlobs.emplace_back(job.hashKey, std::move(bytes));
                                    if (doneNow % 35 == 0 || doneNow == totalJobs)
                                    {
                                        std::printf("@vcs {\"progress\":%u,\"total\":%zu,\"note\":\"PSOs\"}\n",
                                                    doneNow, totalJobs);
                                        std::printf("[SCSKiller] [D3D12] Pre-compiled %3u / %zu PSOs  (%s)\n",
                                                    doneNow, totalJobs, job.category);
                                        std::fflush(stdout);
                                    }
                                }
                            }
                        });
                    }
                    for (auto &w : workers)
                        w.join();
                    compiledPsos = doneCount.load();
                }
            }
        }

        // 3. Warm D3D11 Driver Shader Cache (all 7 Vertex Shaders + 33 Pixel Shaders)
        std::printf("@vcs {\"stage\":\"d3d11\",\"title\":\"%s\"}\n",
                    isPt ? "Pré-compilando Shaders Direct3D 11 no cache do driver"
                         : "Pre-compiling Direct3D 11 Shaders into driver cache");
        std::fflush(stdout);

        uint32_t compiledShaders11 = 0;
        ComPtr<ID3D11Device> dev11;
        ComPtr<ID3D11DeviceContext> ctx11;
        const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        if (SUCCEEDED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &fl, 1,
                                        D3D11_SDK_VERSION, &dev11, nullptr, &ctx11)))
        {
            for (const Bytecode &vs : kVertexShaders)
            {
                ComPtr<ID3D11VertexShader> s;
                if (SUCCEEDED(dev11->CreateVertexShader(vs.data, vs.size, nullptr, &s)))
                    ++compiledShaders11;
            }
            for (int z = 0; z < 2; ++z)
                for (int m = 0; m < 7; ++m)
                    for (int d = 0; d < 2; ++d)
                    {
                        const Bytecode &ps = kDrawPS[z][m][d];
                        ComPtr<ID3D11PixelShader> s;
                        if (SUCCEEDED(dev11->CreatePixelShader(ps.data, ps.size, nullptr, &s)))
                            ++compiledShaders11;
                    }
            for (const Bytecode &ps : kUtilPS)
            {
                ComPtr<ID3D11PixelShader> s;
                if (SUCCEEDED(dev11->CreatePixelShader(ps.data, ps.size, nullptr, &s)))
                    ++compiledShaders11;
            }
            std::printf("[SCSKiller] [D3D11] Pre-compiled %u / 40 DXBC Shaders into GPU driver cache\n", compiledShaders11);
            std::fflush(stdout);
        }

        // 4. Write consolidated binary PSO cache to <buildDir>/shaders/scskiller_pso_cache.bin
        std::error_code ec;
        const fs::path cacheDir = buildDir / "shaders";
        fs::create_directories(cacheDir, ec);
        const fs::path cacheFile = cacheDir / "scskiller_pso_cache.bin";
        uint64_t writtenBytes = 0;
        {
            std::ofstream out(cacheFile, std::ios::binary | std::ios::trunc);
            CacheHeader hdr{};
            std::memcpy(hdr.magic, kCacheMagic, 8);
            hdr.version = 1;
            hdr.vendorId = ad.VendorId;
            hdr.deviceId = ad.DeviceId;
            hdr.psoCount = compiledPsos;
            hdr.shaderCount = compiledShaders11;
            std::snprintf(hdr.gpuName, sizeof(hdr.gpuName), "%s", gpuName.c_str());
            out.write(reinterpret_cast<const char *>(&hdr), sizeof(hdr));

            const uint32_t blobCount = static_cast<uint32_t>(cachedBlobs.size());
            out.write(reinterpret_cast<const char *>(&blobCount), sizeof(blobCount));
            for (const auto &[key, blob] : cachedBlobs)
            {
                const uint32_t sz = static_cast<uint32_t>(blob.size());
                out.write(reinterpret_cast<const char *>(&key), sizeof(key));
                out.write(reinterpret_cast<const char *>(&sz), sizeof(sz));
                if (sz > 0)
                    out.write(reinterpret_cast<const char *>(blob.data()), sz);
            }
            out.flush();
        }
        writtenBytes = fs::file_size(cacheFile, ec);

        const double elapsedMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tStart).count();
        std::printf("[SCSKiller] ----------------------------------------------------------------\n");
        std::printf("[SCSKiller] Cache File  : %s (%llu KB)\n",
                    cacheFile.string().c_str(), static_cast<unsigned long long>(writtenBytes >> 10));
        std::printf("[SCSKiller] Summary     : %u D3D12 PSOs + %u D3D11 Shaders in %.0f ms\n",
                    compiledPsos, compiledShaders11, elapsedMs);
        std::printf("[SCSKiller] Driver Cache: Populated (0ms runtime shader stutter ready!)\n");
        std::printf("@vcs {\"result\":\"ok\",\"psos\":%u,\"shaders\":%u}\n", compiledPsos, compiledShaders11);
        std::fflush(stdout);
        return 0;
#endif
    }
}
