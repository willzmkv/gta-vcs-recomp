# Render hardware interface (rhi.h) and its backends. Only rhi/d3d11 sees D3D11 / DXGI, only rhi/vulkan sees Vulkan.
# D3D11: Windows (its default). Vulkan: Linux (the only one there), opt-in on Windows to compare with D3D11 on the same
# machine. The null backend (VCS_RHI=null) is always there: no GPU work, for bring-up and tests of the CPU side.
include(FetchContent)

if(WIN32)
    set(_vcs_vulkan_default OFF)
else()
    set(_vcs_vulkan_default ON)
endif()
option(VCS_RHI_VULKAN "Build the Vulkan RHI backend" ${_vcs_vulkan_default})
set(VCS_RHI_D3D11 ${WIN32} CACHE INTERNAL "")
option(VCS_RHI_D3D12 "Build the D3D12 RHI backend (Windows)" ${WIN32})

add_library(vcs_rhi STATIC rhi.cpp null/rhi_null.cpp)
target_include_directories(vcs_rhi PUBLIC "${CMAKE_SOURCE_DIR}/src" PRIVATE "${VCS_IMGUI_BACKENDS_DIR}")
target_link_libraries(vcs_rhi PUBLIC imgui)

if(VCS_RHI_D3D11)
    target_sources(vcs_rhi PRIVATE d3d11/rhi_d3d11.cpp "${VCS_IMGUI_BACKENDS_DIR}/imgui_impl_dx11.cpp")
    target_compile_definitions(vcs_rhi PUBLIC VCS_HAVE_D3D11=1)
    target_link_libraries(vcs_rhi PRIVATE d3d11 dxgi d3dcompiler dxguid)
endif()

if(WIN32 AND VCS_RHI_D3D12)
    target_sources(vcs_rhi PRIVATE d3d12/rhi_d3d12.cpp)
    target_compile_definitions(vcs_rhi PUBLIC VCS_HAVE_D3D12=1)
    target_link_libraries(vcs_rhi PRIVATE d3d12 dxgi d3dcompiler dxguid)
endif()

if(VCS_RHI_VULKAN)
    # Headers, loader (volk: no link against libvulkan, no SDK needed) and allocator, all from source
    FetchContent_Declare(
        VulkanHeaders
        GIT_REPOSITORY "https://github.com/KhronosGroup/Vulkan-Headers.git"
        GIT_TAG "vulkan-sdk-1.4.350.1"
        GIT_SHALLOW TRUE
    )
    FetchContent_Declare(
        volk
        GIT_REPOSITORY "https://github.com/zeux/volk.git"
        GIT_TAG "vulkan-sdk-1.4.350.1"
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR "no-cmake" # sources only: volk.c is built into vcs_rhi below
    )
    FetchContent_Declare(
        VulkanMemoryAllocator
        GIT_REPOSITORY "https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git"
        GIT_TAG "v3.3.0"
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR "no-cmake" # header only
    )
    FetchContent_MakeAvailable(VulkanHeaders volk VulkanMemoryAllocator)

    # Dear ImGui is drawn through the RHI (render/ui_renderer.cpp); the window's surface comes from the platform layer
    # (rhi::VulkanWindow)
    target_sources(vcs_rhi PRIVATE vulkan/rhi_vulkan.cpp "${volk_SOURCE_DIR}/volk.c")
    target_include_directories(vcs_rhi PRIVATE "${volk_SOURCE_DIR}" "${vulkanmemoryallocator_SOURCE_DIR}/include")
    target_compile_definitions(vcs_rhi PUBLIC VCS_HAVE_VULKAN=1 PRIVATE VK_NO_PROTOTYPES)
    target_link_libraries(vcs_rhi PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS})
endif()
