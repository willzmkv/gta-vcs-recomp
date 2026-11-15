// SCSKiller-inspired Shader Cache & Pipeline State Object (PSO) Precompiler for VCS.rec.
// Reconstructs the game's graphics pipelines (Root Signature, 40 DXBC Vertex/Pixel Shaders, Input Layouts,
// Blend/Depth/Rasterizer states, and MSAA permutations) and pre-compiles them directly into the GPU driver's
// global shader cache (NVIDIA DXCache / AMD DxCache) as well as <build_dir>/shaders/scskiller_pso_cache.bin
// before the game launches, eliminating mid-game shader compilation stutter.
#pragma once

#include <filesystem>
#include <string>

namespace launcher
{
    struct ScsKillerStatus
    {
        bool exists = false;
        uint32_t psoCount = 0;
        uint32_t shaderCount = 0;
        uint64_t bytes = 0;
        std::string gpuName;
    };

    // Inspects <buildDir>/shaders/scskiller_pso_cache.bin if present
    ScsKillerStatus inspectShaderCache(const std::filesystem::path &buildDir);

    // Runs the standalone CLI pass invoked by `VCSLauncher.exe --scskiller <buildDir> [--msaa N] [--lang pt|en]`
    int runScsKillerCli(const std::filesystem::path &buildDir, int userMsaa, bool isPt);
}
