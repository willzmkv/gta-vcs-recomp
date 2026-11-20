# Renderer pieces on top of the RHI: the window presenter and the HW GS renderer's shaders (docs/NATIVE_RENDERER.md).
# One HLSL source per shader, compiled at build time into headers for each backend that is built:
#   D3D11: DXBC from D3DCompile (tools/hlsl_dxbc.py) -> shaders/dxbc/<header>, array <name>
#   Vulkan: SPIR-V from glslang (tools/hlsl_spirv.py) -> shaders/spirv/<header>, array <name>_spv
set(VCS_SHADER_OUT "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${VCS_SHADER_OUT}/dxbc" "${VCS_SHADER_OUT}/spirv")

find_package(Python3 REQUIRED COMPONENTS Interpreter)
# D3D11: Windows' own d3dcompiler_47.dll through tools/hlsl_dxbc.py, the same DXBC as the SDK's fxc (checked
# byte for byte; DECISIONS.md 2026-11-03) - no Windows SDK. VCS_DXBC_CHECK_FXC=<fxc.exe> compares every shader with
# that fxc's output and fails on a difference.
set(VCS_DXBC_CHECK_FXC "" CACHE FILEPATH "fxc.exe to compare tools/hlsl_dxbc.py's DXBC with (empty: no check)")
if(VCS_RHI_VULKAN)
    find_program(VCS_GLSLANG NAMES glslangValidator glslang HINTS "$ENV{VULKAN_SDK}/bin")
    if(NOT VCS_GLSLANG)
        message(FATAL_ERROR "glslangValidator not found (Vulkan shaders): install glslang-tools (Debian / Ubuntu), "
                            "glslang (Fedora / Arch) or the Vulkan SDK - docs/LINUX.md")
    endif()
endif()

set(VCS_SHADER_HEADERS "")
# vcs_shader(<file.hlsl> <entry> <profile> <array name> <output header> [NAME=VALUE defines...])
function(vcs_shader source entry profile name header)
    set(src "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
    if(VCS_RHI_D3D11)
        set(out "${VCS_SHADER_OUT}/dxbc/${header}")
        set(defines "")
        foreach(d IN LISTS ARGN)
            list(APPEND defines -D ${d})
        endforeach()
        set(check "")
        if(VCS_DXBC_CHECK_FXC)
            set(check --check-fxc "${VCS_DXBC_CHECK_FXC}")
        endif()
        add_custom_command(
            OUTPUT "${out}"
            COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/tools/hlsl_dxbc.py" --profile ${profile}
                    --entry ${entry} --name ${name} --out "${out}" ${defines} ${check} "${src}"
            DEPENDS "${src}" "${CMAKE_SOURCE_DIR}/tools/hlsl_dxbc.py"
            COMMENT "hlsl_dxbc ${source} ${entry}"
            VERBATIM)
        list(APPEND VCS_SHADER_HEADERS "${out}")
    endif()
    if(VCS_RHI_VULKAN)
        set(out "${VCS_SHADER_OUT}/spirv/${header}")
        string(SUBSTRING "${profile}" 0 2 kind)
        if(kind STREQUAL "vs")
            set(stage vert)
        else()
            set(stage frag)
        endif()
        set(defines "")
        foreach(d IN LISTS ARGN)
            list(APPEND defines -D ${d})
        endforeach()
        add_custom_command(
            OUTPUT "${out}"
            COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/tools/hlsl_spirv.py" --glslang "${VCS_GLSLANG}"
                    --stage ${stage} --entry ${entry} --name ${name}_spv --out "${out}" ${defines} "${src}"
            DEPENDS "${src}" "${CMAKE_SOURCE_DIR}/tools/hlsl_spirv.py"
            COMMENT "glslang ${source} ${entry}"
            VERBATIM)
        list(APPEND VCS_SHADER_HEADERS "${out}")
    endif()
    set(VCS_SHADER_HEADERS ${VCS_SHADER_HEADERS} PARENT_SCOPE)
endfunction()
vcs_shader(shaders/present.hlsl vs_main vs_5_0 kPresentVS present_vs.h)
vcs_shader(shaders/present.hlsl ps_main ps_5_0 kPresentPS present_ps.h)
vcs_shader(shaders/gs_draw.hlsl vs_main vs_5_0 kGsDrawVS gs_draw_vs.h)
vcs_shader(shaders/gs_draw.hlsl vs_world vs_5_0 kGsWorldVS gs_world_vs.h)
vcs_shader(shaders/gs_draw.hlsl vs_lit vs_5_0 kGsLitVS gs_lit_vs.h)
vcs_shader(shaders/gs_draw.hlsl vs_lit_inst vs_5_0 kGsLitInstVS gs_lit_inst_vs.h)
# the GS pixel shader per texture read (SAMPLE_MODE 0..6, gs_draw.hlsl) and with / without the dual-source output
foreach(mode RANGE 0 6)
    vcs_shader(shaders/gs_draw.hlsl ps_main ps_5_0 kGsDrawPS${mode} gs_draw_ps${mode}.h DUAL_SOURCE=0 SAMPLE_MODE=${mode})
    vcs_shader(shaders/gs_draw.hlsl ps_main ps_5_0 kGsDrawPS${mode}Dual gs_draw_ps${mode}_dual.h DUAL_SOURCE=1 SAMPLE_MODE=${mode})
    # ... and without writing the depth (SHADER_DEPTH=0: early depth rejection; docs/PERFORMANCE.md 26)
    vcs_shader(shaders/gs_draw.hlsl ps_main ps_5_0 kGsDrawPS${mode}NoZ gs_draw_ps${mode}_noz.h DUAL_SOURCE=0 SAMPLE_MODE=${mode} SHADER_DEPTH=0)
    vcs_shader(shaders/gs_draw.hlsl ps_main ps_5_0 kGsDrawPS${mode}DualNoZ gs_draw_ps${mode}_dual_noz.h DUAL_SOURCE=1 SAMPLE_MODE=${mode} SHADER_DEPTH=0)
endforeach()
vcs_shader(shaders/gs_util.hlsl vs_main vs_5_0 kGsQuadVS gs_quad_vs.h)
vcs_shader(shaders/gs_util.hlsl ps_blit ps_5_0 kGsBlitPS gs_blit_ps.h)
vcs_shader(shaders/gs_util.hlsl ps_fill_color ps_5_0 kGsFillColorPS gs_fill_color_ps.h)
vcs_shader(shaders/gs_util.hlsl ps_fill_depth ps_5_0 kGsFillDepthPS gs_fill_depth_ps.h)
vcs_shader(shaders/ui.hlsl vs_main vs_5_0 kUiVS ui_vs.h)
vcs_shader(shaders/ui.hlsl ps_main ps_5_0 kUiPS ui_ps.h)

add_library(vcs_render STATIC
    presenter.cpp
    gs_shaders.cpp
    ui_renderer.cpp
    texture_replacements.cpp
    ${VCS_SHADER_HEADERS}
)
target_include_directories(vcs_render PUBLIC "${CMAKE_SOURCE_DIR}/src" PRIVATE "${VCS_SHADER_OUT}")
target_link_libraries(vcs_render PUBLIC vcs_rhi vcs_platform imgui)
