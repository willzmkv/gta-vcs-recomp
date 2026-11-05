// See rhi.h: backend names and the device factory.
#include "rhi/rhi.h"

#include <cctype>
#include <cstring>

namespace rhi
{
    const char *backendName(Backend backend)
    {
        switch (backend)
        {
        case Backend::D3D11: return "D3D11";
        case Backend::D3D12: return "D3D12";
        case Backend::Vulkan: return "Vulkan";
        case Backend::Null: return "null";
        }
        return "?";
    }

    bool backendFromName(const char *name, Backend &out)
    {
        if (!name)
            return false;
        char lower[16] = {};
        for (size_t i = 0; name[i] && i + 1 < sizeof(lower); ++i)
            lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[i])));
        if (std::strcmp(lower, "d3d11") == 0)
            out = Backend::D3D11;
        else if (std::strcmp(lower, "d3d12") == 0)
            out = Backend::D3D12;
        else if (std::strcmp(lower, "vulkan") == 0)
            out = Backend::Vulkan;
        else if (std::strcmp(lower, "null") == 0)
            out = Backend::Null;
        else
            return false;
        return true;
    }

    bool backendBuilt(Backend backend)
    {
        switch (backend)
        {
#if defined(VCS_HAVE_D3D11)
        case Backend::D3D11: return true;
#endif
#if defined(VCS_HAVE_D3D12)
        case Backend::D3D12: return true;
#endif
#if defined(VCS_HAVE_VULKAN)
        case Backend::Vulkan: return true;
#endif
        case Backend::Null: return true;
        default: return false;
        }
    }

    // D3D12 on Windows since 2026-11-05 (DECISIONS.md: the player's dense streets lost the D3D11 slowdown with it); the
    // presenter falls back to D3D11 when no D3D12 device can be made
    Backend defaultBackend()
    {
#if defined(VCS_HAVE_D3D12)
        return Backend::D3D12;
#elif defined(VCS_HAVE_D3D11)
        return Backend::D3D11;
#elif defined(VCS_HAVE_VULKAN)
        return Backend::Vulkan;
#else
        return Backend::Null;
#endif
    }

    std::unique_ptr<Device> createDevice(const DeviceDesc &desc, std::string &error)
    {
        switch (desc.backend)
        {
#if defined(VCS_HAVE_D3D11)
        case Backend::D3D11: return createD3D11Device(desc, error);
#endif
#if defined(VCS_HAVE_D3D12)
        case Backend::D3D12: return createD3D12Device(desc, error);
#endif
#if defined(VCS_HAVE_VULKAN)
        case Backend::Vulkan: return createVulkanDevice(desc, error);
#endif
        case Backend::Null: return createNullDevice(desc, error);
        default: break;
        }
        error = std::string("the ") + backendName(desc.backend) + " renderer is not part of this build";
        return nullptr;
    }
}
