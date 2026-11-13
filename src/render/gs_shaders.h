// Compiled shaders of the presenter (shaders/present.hlsl) and the HW GS renderer (shaders/gs_draw.hlsl,
// shaders/gs_util.hlsl) for an RHI backend.
#pragma once

#include "rhi/rhi.h"

namespace render
{
    struct GsShaders
    {
        rhi::ShaderCode drawVS;
        rhi::ShaderCode worldVS;      // native VCS world draws
        rhi::ShaderCode litVS;        // native VCS lit / skinned models
        static constexpr int kSampleModes = 7;
        // [SAMPLE_MODE][dual]: the GS pixel shader built for one way of reading the texture (gs_draw.hlsl; 0: every way,
        // chosen per draw); dual: + the blend factor as the second output (Src1Alpha blending)
        rhi::ShaderCode drawPS[kSampleModes][2];
        rhi::ShaderCode quadVS;
        rhi::ShaderCode blitPS;
        rhi::ShaderCode fillColorPS;
        rhi::ShaderCode fillDepthPS;
        rhi::ShaderCode presentVS;    // the window's picture (render/presenter.cpp)
        rhi::ShaderCode presentPS;
        rhi::ShaderCode uiVS;         // Dear ImGui through the RHI (render/ui_renderer.cpp)
        rhi::ShaderCode uiPS;
        // drawPS without the depth output (SHADER_DEPTH=0): the rasterised depth, early depth rejection on
        rhi::ShaderCode drawPSNoDepth[kSampleModes][2];
        rhi::ShaderCode litInstVS;    // litVS instanced: per-instance matrices and lights (the distant proxies)
    };

    // The backend's shaders (empty for the null backend, which never runs them).
    const GsShaders &gsShaders(rhi::Backend backend);
}
