// Disc-free Vulkan regression: the hardware GS's depth-only HUD quad.
#include "rhi/rhi.h"
#include "vs_main.h"
#include "ps_fill_depth.h"

#include <cstdio>

int main()
{
    rhi::DeviceDesc desc{};
    desc.backend = rhi::Backend::Vulkan;
    desc.width = desc.height = 32;
    desc.offscreen = true;
    desc.debug = true;
    std::string error;
    auto dev = rhi::createVulkanDevice(desc, error);
    if (!dev)
    {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }

    auto depth = dev->createTexture({32, 32, rhi::Format::D32F, rhi::UsageDepth, "HUD depth smoke"});
    const float quad[16] = {-1, 1, 1, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0.25f, 0, 0, 0};
    auto constants = dev->createBuffer({sizeof(quad), rhi::BufferUsage::Constant}, quad);
    rhi::PipelineDesc pipeline{};
    pipeline.vertex = {kQuadVS, sizeof(kQuadVS)};
    pipeline.pixel = {kFillDepthPS, sizeof(kFillDepthPS)};
    pipeline.topology = rhi::Topology::TriangleStrip;
    pipeline.blend.writeMask = 0;
    pipeline.depth = {true, true, rhi::Compare::Always};
    auto fill = dev->createPipeline(pipeline);
    if (!depth || !constants || !fill)
        return 1;

    rhi::PassDesc pass{};
    pass.depth = depth.get();
    pass.depthOnly = true;
    pass.clearDepth = true;
    pass.clearDepthValue = 1.0f;
    dev->beginPass(pass);
    dev->setPipeline(*fill);
    dev->setConstants(0, *constants);
    dev->draw(4);
    dev->endPass();

    // The Vulkan backend also supports D32F readback; this verifies that the draw wasn't skipped.
    float result = -1.0f;
    if (!dev->readTexture(*depth, 16, 16, 1, 1, &result) || result != 0.25f)
    {
        std::fprintf(stderr, "Expected 0.25 from HUD depth fill, got %g\n", result);
        return 1;
    }
    std::puts("HUD depth fill passed");
}
