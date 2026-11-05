// See gs_shaders.h. The headers come from src/render/CMakeLists.txt: DXBC from fxc (shaders/dxbc), SPIR-V from
// tools/hlsl_spirv.py (shaders/spirv).
#include "render/gs_shaders.h"

#if defined(VCS_HAVE_D3D11)
using BYTE = unsigned char; // fxc's /Fh headers declare `const BYTE name[]`
#include "dxbc/gs_draw_vs.h"
#include "dxbc/gs_world_vs.h"
#include "dxbc/gs_lit_vs.h"
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
#endif
#if defined(VCS_HAVE_VULKAN)
#include "spirv/gs_draw_vs.h"
#include "spirv/gs_world_vs.h"
#include "spirv/gs_lit_vs.h"
#include "spirv/gs_draw_ps0.h"
#include "spirv/gs_draw_ps0_dual.h"
#include "spirv/gs_draw_ps1.h"
#include "spirv/gs_draw_ps1_dual.h"
#include "spirv/gs_draw_ps2.h"
#include "spirv/gs_draw_ps2_dual.h"
#include "spirv/gs_draw_ps3.h"
#include "spirv/gs_draw_ps3_dual.h"
#include "spirv/gs_draw_ps4.h"
#include "spirv/gs_draw_ps4_dual.h"
#include "spirv/gs_draw_ps5.h"
#include "spirv/gs_draw_ps5_dual.h"
#include "spirv/gs_draw_ps6.h"
#include "spirv/gs_draw_ps6_dual.h"
#include "spirv/gs_quad_vs.h"
#include "spirv/gs_blit_ps.h"
#include "spirv/gs_fill_color_ps.h"
#include "spirv/gs_fill_depth_ps.h"
#include "spirv/present_vs.h"
#include "spirv/present_ps.h"
#include "spirv/ui_vs.h"
#include "spirv/ui_ps.h"
#include "spirv/gs_draw_ps0_noz.h"
#include "spirv/gs_draw_ps0_dual_noz.h"
#include "spirv/gs_draw_ps1_noz.h"
#include "spirv/gs_draw_ps1_dual_noz.h"
#include "spirv/gs_draw_ps2_noz.h"
#include "spirv/gs_draw_ps2_dual_noz.h"
#include "spirv/gs_draw_ps3_noz.h"
#include "spirv/gs_draw_ps3_dual_noz.h"
#include "spirv/gs_draw_ps4_noz.h"
#include "spirv/gs_draw_ps4_dual_noz.h"
#include "spirv/gs_draw_ps5_noz.h"
#include "spirv/gs_draw_ps5_dual_noz.h"
#include "spirv/gs_draw_ps6_noz.h"
#include "spirv/gs_draw_ps6_dual_noz.h"
#endif

namespace render
{
    const GsShaders &gsShaders(rhi::Backend backend)
    {
#if defined(VCS_HAVE_D3D11)
        static const GsShaders dxbc{
            {kGsDrawVS, sizeof(kGsDrawVS)},
            {kGsWorldVS, sizeof(kGsWorldVS)},
            {kGsLitVS, sizeof(kGsLitVS)},
            {
                {{kGsDrawPS0, sizeof(kGsDrawPS0)}, {kGsDrawPS0Dual, sizeof(kGsDrawPS0Dual)}},
                {{kGsDrawPS1, sizeof(kGsDrawPS1)}, {kGsDrawPS1Dual, sizeof(kGsDrawPS1Dual)}},
                {{kGsDrawPS2, sizeof(kGsDrawPS2)}, {kGsDrawPS2Dual, sizeof(kGsDrawPS2Dual)}},
                {{kGsDrawPS3, sizeof(kGsDrawPS3)}, {kGsDrawPS3Dual, sizeof(kGsDrawPS3Dual)}},
                {{kGsDrawPS4, sizeof(kGsDrawPS4)}, {kGsDrawPS4Dual, sizeof(kGsDrawPS4Dual)}},
                {{kGsDrawPS5, sizeof(kGsDrawPS5)}, {kGsDrawPS5Dual, sizeof(kGsDrawPS5Dual)}},
                {{kGsDrawPS6, sizeof(kGsDrawPS6)}, {kGsDrawPS6Dual, sizeof(kGsDrawPS6Dual)}},
            },
            {kGsQuadVS, sizeof(kGsQuadVS)},
            {kGsBlitPS, sizeof(kGsBlitPS)},
            {kGsFillColorPS, sizeof(kGsFillColorPS)},
            {kGsFillDepthPS, sizeof(kGsFillDepthPS)},
            {kPresentVS, sizeof(kPresentVS)},
            {kPresentPS, sizeof(kPresentPS)},
            {kUiVS, sizeof(kUiVS)},
            {kUiPS, sizeof(kUiPS)},
            {
                {{kGsDrawPS0NoZ, sizeof(kGsDrawPS0NoZ)}, {kGsDrawPS0DualNoZ, sizeof(kGsDrawPS0DualNoZ)}},
                {{kGsDrawPS1NoZ, sizeof(kGsDrawPS1NoZ)}, {kGsDrawPS1DualNoZ, sizeof(kGsDrawPS1DualNoZ)}},
                {{kGsDrawPS2NoZ, sizeof(kGsDrawPS2NoZ)}, {kGsDrawPS2DualNoZ, sizeof(kGsDrawPS2DualNoZ)}},
                {{kGsDrawPS3NoZ, sizeof(kGsDrawPS3NoZ)}, {kGsDrawPS3DualNoZ, sizeof(kGsDrawPS3DualNoZ)}},
                {{kGsDrawPS4NoZ, sizeof(kGsDrawPS4NoZ)}, {kGsDrawPS4DualNoZ, sizeof(kGsDrawPS4DualNoZ)}},
                {{kGsDrawPS5NoZ, sizeof(kGsDrawPS5NoZ)}, {kGsDrawPS5DualNoZ, sizeof(kGsDrawPS5DualNoZ)}},
                {{kGsDrawPS6NoZ, sizeof(kGsDrawPS6NoZ)}, {kGsDrawPS6DualNoZ, sizeof(kGsDrawPS6DualNoZ)}},
            },
        };
        if (backend == rhi::Backend::D3D11 || backend == rhi::Backend::D3D12) // (DXBC runs on D3D12 as it is)
            return dxbc;
#endif
#if defined(VCS_HAVE_VULKAN)
        static const GsShaders spirv{
            {kGsDrawVS_spv, sizeof(kGsDrawVS_spv)},
            {kGsWorldVS_spv, sizeof(kGsWorldVS_spv)},
            {kGsLitVS_spv, sizeof(kGsLitVS_spv)},
            {
                {{kGsDrawPS0_spv, sizeof(kGsDrawPS0_spv)}, {kGsDrawPS0Dual_spv, sizeof(kGsDrawPS0Dual_spv)}},
                {{kGsDrawPS1_spv, sizeof(kGsDrawPS1_spv)}, {kGsDrawPS1Dual_spv, sizeof(kGsDrawPS1Dual_spv)}},
                {{kGsDrawPS2_spv, sizeof(kGsDrawPS2_spv)}, {kGsDrawPS2Dual_spv, sizeof(kGsDrawPS2Dual_spv)}},
                {{kGsDrawPS3_spv, sizeof(kGsDrawPS3_spv)}, {kGsDrawPS3Dual_spv, sizeof(kGsDrawPS3Dual_spv)}},
                {{kGsDrawPS4_spv, sizeof(kGsDrawPS4_spv)}, {kGsDrawPS4Dual_spv, sizeof(kGsDrawPS4Dual_spv)}},
                {{kGsDrawPS5_spv, sizeof(kGsDrawPS5_spv)}, {kGsDrawPS5Dual_spv, sizeof(kGsDrawPS5Dual_spv)}},
                {{kGsDrawPS6_spv, sizeof(kGsDrawPS6_spv)}, {kGsDrawPS6Dual_spv, sizeof(kGsDrawPS6Dual_spv)}},
            },
            {kGsQuadVS_spv, sizeof(kGsQuadVS_spv)},
            {kGsBlitPS_spv, sizeof(kGsBlitPS_spv)},
            {kGsFillColorPS_spv, sizeof(kGsFillColorPS_spv)},
            {kGsFillDepthPS_spv, sizeof(kGsFillDepthPS_spv)},
            {kPresentVS_spv, sizeof(kPresentVS_spv)},
            {kPresentPS_spv, sizeof(kPresentPS_spv)},
            {kUiVS_spv, sizeof(kUiVS_spv)},
            {kUiPS_spv, sizeof(kUiPS_spv)},
            {
                {{kGsDrawPS0NoZ_spv, sizeof(kGsDrawPS0NoZ_spv)}, {kGsDrawPS0DualNoZ_spv, sizeof(kGsDrawPS0DualNoZ_spv)}},
                {{kGsDrawPS1NoZ_spv, sizeof(kGsDrawPS1NoZ_spv)}, {kGsDrawPS1DualNoZ_spv, sizeof(kGsDrawPS1DualNoZ_spv)}},
                {{kGsDrawPS2NoZ_spv, sizeof(kGsDrawPS2NoZ_spv)}, {kGsDrawPS2DualNoZ_spv, sizeof(kGsDrawPS2DualNoZ_spv)}},
                {{kGsDrawPS3NoZ_spv, sizeof(kGsDrawPS3NoZ_spv)}, {kGsDrawPS3DualNoZ_spv, sizeof(kGsDrawPS3DualNoZ_spv)}},
                {{kGsDrawPS4NoZ_spv, sizeof(kGsDrawPS4NoZ_spv)}, {kGsDrawPS4DualNoZ_spv, sizeof(kGsDrawPS4DualNoZ_spv)}},
                {{kGsDrawPS5NoZ_spv, sizeof(kGsDrawPS5NoZ_spv)}, {kGsDrawPS5DualNoZ_spv, sizeof(kGsDrawPS5DualNoZ_spv)}},
                {{kGsDrawPS6NoZ_spv, sizeof(kGsDrawPS6NoZ_spv)}, {kGsDrawPS6DualNoZ_spv, sizeof(kGsDrawPS6DualNoZ_spv)}},
            },
        };
        if (backend == rhi::Backend::Vulkan)
            return spirv;
#endif
        (void)backend;
        static const GsShaders none{};
        return none;
    }
}
