// Vulkan backend (Linux; docs/LAUNCHER.md). The same frame as Direct3D 11's: one instanced draw of the draw list with
// the one ui.hlsl (compiled to SPIR-V by glslang), the glyph atlas uploaded by its dirty rectangle.
//
// - No link to libvulkan: it is opened at run time (dlopen) and the ~90 entry points are loaded here, so a PC without
//   a Vulkan driver gets a message instead of a launcher that doesn't start. Vulkan 1.1 is enough (the flipped
//   viewport keeps ui.hlsl's D3D clip space as it is); the shaders target SPIR-V 1.3.
// - Presentation: FIFO with vsync, else MAILBOX or IMMEDIATE (tearing). With VK_KHR_present_wait, the frame loop waits
//   until the last frame is on screen before reading input (DXGI's latency waitable at one frame); without it, on the
//   frame's fence. Two frames in flight, each with its own persistently mapped instance buffer: an upload is a memcpy.
// - The GPU it picks: integrated before discrete (a launcher shouldn't wake a laptop's dGPU), then virtual, then
//   llvmpipe / lavapipe (CI, VMs: rendererStats.warp). VCS_LAUNCHER_VK_DEVICE=<part of the name> overrides it.
// - A lost device (or Ctrl+Shift+F10) is rebuilt from CPU-side state, the atlas included.
// - VCS_LAUNCHER_VK_DEBUG=1: the Khronos validation layer, its messages on stderr as [vulkan].
#include "render/renderer.h"

#include "platform/platform.h"
#include "render/draw.h"
#include "render/font.h"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#include "ui_ps.spv.h"
#include "ui_vs.spv.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <vector>

namespace render
{
    RendererStats rendererStats;

    namespace
    {
        // the window-system structures, declared here so no X11 / Wayland header comes in (vulkan_xlib.h needs Xlib's,
        // whose macros - None, Bool, Status - collide with everything)
        struct XlibSurfaceCreateInfo
        {
            VkStructureType sType;
            const void *pNext;
            VkFlags flags;
            void *dpy;
            unsigned long window;
        };
        struct WaylandSurfaceCreateInfo
        {
            VkStructureType sType;
            const void *pNext;
            VkFlags flags;
            void *display;
            void *surface;
        };
        using PFN_CreateXlibSurface = VkResult(VKAPI_PTR *)(VkInstance, const XlibSurfaceCreateInfo *,
                                                            const VkAllocationCallbacks *, VkSurfaceKHR *);
        using PFN_CreateWaylandSurface = VkResult(VKAPI_PTR *)(VkInstance, const WaylandSurfaceCreateInfo *,
                                                               const VkAllocationCallbacks *, VkSurfaceKHR *);

#define VK_GLOBAL_FUNCTIONS(X)                                                                                         \
    X(vkCreateInstance)                                                                                                \
    X(vkEnumerateInstanceExtensionProperties)                                                                          \
    X(vkEnumerateInstanceLayerProperties)

#define VK_INSTANCE_FUNCTIONS(X)                                                                                       \
    X(vkDestroyInstance)                                                                                               \
    X(vkEnumeratePhysicalDevices)                                                                                      \
    X(vkGetPhysicalDeviceProperties)                                                                                   \
    X(vkGetPhysicalDeviceFeatures2)                                                                                    \
    X(vkGetPhysicalDeviceQueueFamilyProperties)                                                                        \
    X(vkGetPhysicalDeviceMemoryProperties)                                                                             \
    X(vkEnumerateDeviceExtensionProperties)                                                                            \
    X(vkCreateDevice)                                                                                                  \
    X(vkGetDeviceProcAddr)                                                                                             \
    X(vkDestroySurfaceKHR)                                                                                             \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)                                                                            \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                                                                       \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)                                                                            \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)                                                                       \
    X(vkCreateDebugUtilsMessengerEXT)                                                                                  \
    X(vkDestroyDebugUtilsMessengerEXT)

#define VK_DEVICE_FUNCTIONS(X)                                                                                         \
    X(vkDestroyDevice)                                                                                                 \
    X(vkGetDeviceQueue)                                                                                                \
    X(vkDeviceWaitIdle)                                                                                                \
    X(vkCreateSwapchainKHR)                                                                                            \
    X(vkDestroySwapchainKHR)                                                                                           \
    X(vkGetSwapchainImagesKHR)                                                                                         \
    X(vkAcquireNextImageKHR)                                                                                           \
    X(vkQueuePresentKHR)                                                                                               \
    X(vkWaitForPresentKHR)                                                                                             \
    X(vkCreateImageView)                                                                                               \
    X(vkDestroyImageView)                                                                                              \
    X(vkCreateImage)                                                                                                   \
    X(vkDestroyImage)                                                                                                  \
    X(vkGetImageMemoryRequirements)                                                                                    \
    X(vkBindImageMemory)                                                                                               \
    X(vkCreateBuffer)                                                                                                  \
    X(vkDestroyBuffer)                                                                                                 \
    X(vkGetBufferMemoryRequirements)                                                                                   \
    X(vkBindBufferMemory)                                                                                              \
    X(vkAllocateMemory)                                                                                                \
    X(vkFreeMemory)                                                                                                    \
    X(vkMapMemory)                                                                                                     \
    X(vkUnmapMemory)                                                                                                   \
    X(vkCreateRenderPass)                                                                                              \
    X(vkDestroyRenderPass)                                                                                             \
    X(vkCreateFramebuffer)                                                                                             \
    X(vkDestroyFramebuffer)                                                                                            \
    X(vkCreateShaderModule)                                                                                            \
    X(vkDestroyShaderModule)                                                                                           \
    X(vkCreatePipelineLayout)                                                                                          \
    X(vkDestroyPipelineLayout)                                                                                         \
    X(vkCreateGraphicsPipelines)                                                                                       \
    X(vkDestroyPipeline)                                                                                               \
    X(vkCreateDescriptorSetLayout)                                                                                     \
    X(vkDestroyDescriptorSetLayout)                                                                                    \
    X(vkCreateDescriptorPool)                                                                                          \
    X(vkDestroyDescriptorPool)                                                                                         \
    X(vkAllocateDescriptorSets)                                                                                        \
    X(vkUpdateDescriptorSets)                                                                                          \
    X(vkCreateSampler)                                                                                                 \
    X(vkDestroySampler)                                                                                                \
    X(vkCreateCommandPool)                                                                                             \
    X(vkDestroyCommandPool)                                                                                            \
    X(vkAllocateCommandBuffers)                                                                                        \
    X(vkFreeCommandBuffers)                                                                                            \
    X(vkResetCommandPool)                                                                                              \
    X(vkBeginCommandBuffer)                                                                                            \
    X(vkEndCommandBuffer)                                                                                              \
    X(vkCmdBeginRenderPass)                                                                                            \
    X(vkCmdEndRenderPass)                                                                                              \
    X(vkCmdBindPipeline)                                                                                               \
    X(vkCmdBindVertexBuffers)                                                                                          \
    X(vkCmdBindDescriptorSets)                                                                                         \
    X(vkCmdPushConstants)                                                                                              \
    X(vkCmdSetViewport)                                                                                                \
    X(vkCmdSetScissor)                                                                                                 \
    X(vkCmdDraw)                                                                                                       \
    X(vkCmdPipelineBarrier)                                                                                            \
    X(vkCmdCopyBufferToImage)                                                                                          \
    X(vkCmdCopyImageToBuffer)                                                                                          \
    X(vkQueueSubmit)                                                                                                   \
    X(vkCreateFence)                                                                                                   \
    X(vkDestroyFence)                                                                                                  \
    X(vkWaitForFences)                                                                                                 \
    X(vkResetFences)                                                                                                   \
    X(vkCreateSemaphore)                                                                                               \
    X(vkDestroySemaphore)

#define VK_DECLARE(name) PFN_##name name = nullptr;
        struct Api
        {
            void *library = nullptr;
            PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
            VK_GLOBAL_FUNCTIONS(VK_DECLARE)
            VK_INSTANCE_FUNCTIONS(VK_DECLARE)
            VK_DEVICE_FUNCTIONS(VK_DECLARE)
            PFN_CreateXlibSurface vkCreateXlibSurfaceKHR = nullptr;
            PFN_CreateWaylandSurface vkCreateWaylandSurfaceKHR = nullptr;
        };
#undef VK_DECLARE
        Api vk;

        constexpr int kFramesInFlight = 2;
        constexpr VkFormat kOffscreenFormat = VK_FORMAT_R8G8B8A8_UNORM;
        constexpr uint64_t kAcquireTimeout = 250'000'000; // ns: a compositor that stops answering skips frames

        struct Buffer
        {
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            void *mapped = nullptr;
            size_t capacity = 0; // bytes
        };

        struct Frame
        {
            VkCommandPool pool = VK_NULL_HANDLE;
            VkCommandBuffer commands = VK_NULL_HANDLE;
            VkFence fence = VK_NULL_HANDLE;
            VkSemaphore acquired = VK_NULL_HANDLE;
            Buffer instances, staging, imageStaging;
            bool submitted = false;
        };

        struct Target // a swapchain image, or the offscreen image
        {
            VkImage image = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            VkFramebuffer framebuffer = VK_NULL_HANDLE;
            VkSemaphore rendered = VK_NULL_HANDLE; // per image: presentation may still hold the last one
        };

        struct State
        {
            const platform::NativeSurface *window = nullptr;
            VkInstance instance = VK_NULL_HANDLE;
            VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
            VkSurfaceKHR surface = VK_NULL_HANDLE;
            VkPhysicalDevice gpu = VK_NULL_HANDLE;
            VkPhysicalDeviceMemoryProperties memory{};
            VkDevice device = VK_NULL_HANDLE;
            uint32_t queueFamily = 0;
            VkQueue queue = VK_NULL_HANDLE;
            bool presentWait = false;
            bool hasMailbox = false, hasImmediate = false;

            VkSwapchainKHR swapchain = VK_NULL_HANDLE;
            VkFormat format = kOffscreenFormat;
            VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
            bool vsync = true;
            bool transferSrc = false; // the swapchain's images can be read back
            std::vector<Target> targets;
            VkDeviceMemory offscreenMemory = VK_NULL_HANDLE;
            int width = 0, height = 0;
            bool swapchainStale = false;

            VkRenderPass renderPass = VK_NULL_HANDLE;
            VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
            VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
            VkPipeline pipeline = VK_NULL_HANDLE;
            VkSampler sampler = VK_NULL_HANDLE;
            VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
            VkDescriptorSet descriptorSet = VK_NULL_HANDLE;

            VkImage atlasImage = VK_NULL_HANDLE;
            VkImageView atlasView = VK_NULL_HANDLE;
            VkDeviceMemory atlasMemory = VK_NULL_HANDLE;
            int atlasSize = 0;
            uint32_t atlasGeneration = 0;
            bool atlasFresh = false; // created, never written: its layout is UNDEFINED

            VkImage imageAtlasImage = VK_NULL_HANDLE;
            VkImageView imageAtlasView = VK_NULL_HANDLE;
            VkDeviceMemory imageAtlasMemory = VK_NULL_HANDLE;
            uint32_t imageAtlasGeneration = 0;

            Frame frames[kFramesInFlight];
            int frame = 0;
            uint32_t imageIndex = 0;
            bool acquired = false; // an image acquired and rendered, not yet presented
            bool rendered = false; // this frame was submitted (readback has something to read)
            bool frameWaited = false;
            uint64_t presentId = 0;
            bool loseDevice = false;
        };
        State d;

        bool fail(std::string &error, const char *what, VkResult r)
        {
            char text[160];
            std::snprintf(text, sizeof(text), "Vulkan: %s failed (%d)", what, int(r));
            error = text;
            return false;
        }

        bool loadLibrary(std::string &error)
        {
            if (vk.library)
                return true;
            for (const char *name : {"libvulkan.so.1", "libvulkan.so"})
                if ((vk.library = dlopen(name, RTLD_NOW | RTLD_LOCAL)))
                    break;
            if (!vk.library)
            {
                error = "Vulkan isn't installed (libvulkan.so.1 not found): install your distribution's Vulkan loader "
                        "and driver (Debian / Ubuntu: libvulkan1 mesa-vulkan-drivers)";
                return false;
            }
            vk.vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(vk.library, "vkGetInstanceProcAddr"));
            if (!vk.vkGetInstanceProcAddr)
            {
                error = "libvulkan.so.1 has no vkGetInstanceProcAddr";
                return false;
            }
#define VK_LOAD(name) vk.name = reinterpret_cast<PFN_##name>(vk.vkGetInstanceProcAddr(nullptr, #name));
            VK_GLOBAL_FUNCTIONS(VK_LOAD)
#undef VK_LOAD
            return vk.vkCreateInstance != nullptr;
        }

        void loadInstanceFunctions()
        {
#define VK_LOAD(name) vk.name = reinterpret_cast<PFN_##name>(vk.vkGetInstanceProcAddr(d.instance, #name));
            VK_INSTANCE_FUNCTIONS(VK_LOAD)
#undef VK_LOAD
            vk.vkCreateXlibSurfaceKHR =
                reinterpret_cast<PFN_CreateXlibSurface>(vk.vkGetInstanceProcAddr(d.instance, "vkCreateXlibSurfaceKHR"));
            vk.vkCreateWaylandSurfaceKHR =
                reinterpret_cast<PFN_CreateWaylandSurface>(vk.vkGetInstanceProcAddr(d.instance, "vkCreateWaylandSurfaceKHR"));
        }

        void loadDeviceFunctions()
        {
#define VK_LOAD(name) vk.name = reinterpret_cast<PFN_##name>(vk.vkGetDeviceProcAddr(d.device, #name));
            VK_DEVICE_FUNCTIONS(VK_LOAD)
#undef VK_LOAD
        }

        VKAPI_ATTR VkBool32 VKAPI_CALL debugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT *data,
                                                    void *)
        {
            if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
                std::fprintf(stderr, "[vulkan] %s\n", data->pMessage);
            return VK_FALSE;
        }

        bool hasExtension(const std::vector<VkExtensionProperties> &list, const char *name)
        {
            for (const auto &e : list)
                if (std::strcmp(e.extensionName, name) == 0)
                    return true;
            return false;
        }

        bool createInstance(std::string &error)
        {
            uint32_t count = 0;
            vk.vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
            std::vector<VkExtensionProperties> available(count);
            vk.vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());

            std::vector<const char *> extensions, layers;
            if (d.window)
            {
                const char *system = d.window->system == platform::NativeSurface::Wayland ? "VK_KHR_wayland_surface"
                                                                                           : "VK_KHR_xlib_surface";
                for (const char *name : {"VK_KHR_surface", system})
                {
                    if (!hasExtension(available, name))
                    {
                        error = std::string("the Vulkan driver can't draw into this window (") + name + " missing)";
                        return false;
                    }
                    extensions.push_back(name);
                }
            }
            const char *debugEnv = std::getenv("VCS_LAUNCHER_VK_DEBUG");
            const bool debug = debugEnv && *debugEnv && *debugEnv != '0';
            if (debug)
            {
                vk.vkEnumerateInstanceLayerProperties(&count, nullptr);
                std::vector<VkLayerProperties> layerList(count);
                vk.vkEnumerateInstanceLayerProperties(&count, layerList.data());
                for (const auto &l : layerList)
                    if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0)
                        layers.push_back("VK_LAYER_KHRONOS_validation");
                if (layers.empty())
                    std::fprintf(stderr, "[vulkan] VCS_LAUNCHER_VK_DEBUG: the validation layer isn't installed\n");
                if (hasExtension(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
                    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            }

            VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
            app.pApplicationName = "VCS Launcher";
            app.apiVersion = VK_API_VERSION_1_1;
            VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
            info.pApplicationInfo = &app;
            info.enabledExtensionCount = uint32_t(extensions.size());
            info.ppEnabledExtensionNames = extensions.data();
            info.enabledLayerCount = uint32_t(layers.size());
            info.ppEnabledLayerNames = layers.data();
            if (VkResult r = vk.vkCreateInstance(&info, nullptr, &d.instance); r != VK_SUCCESS)
            {
                if (r == VK_ERROR_INCOMPATIBLE_DRIVER)
                {
                    error = "no Vulkan 1.1 driver found: install your graphics driver's Vulkan support (Debian / Ubuntu: "
                            "mesa-vulkan-drivers)";
                    return false;
                }
                return fail(error, "vkCreateInstance", r);
            }
            loadInstanceFunctions();
            if (debug && vk.vkCreateDebugUtilsMessengerEXT)
            {
                VkDebugUtilsMessengerCreateInfoEXT m{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
                m.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
                m.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
                m.pfnUserCallback = debugMessage;
                vk.vkCreateDebugUtilsMessengerEXT(d.instance, &m, nullptr, &d.messenger);
            }
            return true;
        }

        bool createSurface(std::string &error)
        {
            VkResult r = VK_ERROR_EXTENSION_NOT_PRESENT;
            if (d.window->system == platform::NativeSurface::Wayland && vk.vkCreateWaylandSurfaceKHR)
            {
                WaylandSurfaceCreateInfo info{VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR, nullptr, 0,
                                              d.window->display, d.window->surface};
                r = vk.vkCreateWaylandSurfaceKHR(d.instance, &info, nullptr, &d.surface);
            }
            else if (d.window->system == platform::NativeSurface::X11 && vk.vkCreateXlibSurfaceKHR)
            {
                XlibSurfaceCreateInfo info{VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR, nullptr, 0, d.window->display,
                                           d.window->window};
                r = vk.vkCreateXlibSurfaceKHR(d.instance, &info, nullptr, &d.surface);
            }
            return r == VK_SUCCESS || fail(error, "creating the window surface", r);
        }

        int deviceRank(VkPhysicalDeviceType type)
        {
            switch (type)
            {
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 4;
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 3;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
            case VK_PHYSICAL_DEVICE_TYPE_CPU: return 1;
            default: return 0;
            }
        }

        bool pickDevice(std::string &error)
        {
            uint32_t count = 0;
            vk.vkEnumeratePhysicalDevices(d.instance, &count, nullptr);
            std::vector<VkPhysicalDevice> gpus(count);
            vk.vkEnumeratePhysicalDevices(d.instance, &count, gpus.data());
            const char *wanted = std::getenv("VCS_LAUNCHER_VK_DEVICE");
            int bestRank = -1;
            for (VkPhysicalDevice gpu : gpus)
            {
                VkPhysicalDeviceProperties props{};
                vk.vkGetPhysicalDeviceProperties(gpu, &props);
                if (props.apiVersion < VK_API_VERSION_1_1)
                    continue;
                uint32_t extCount = 0;
                vk.vkEnumerateDeviceExtensionProperties(gpu, nullptr, &extCount, nullptr);
                std::vector<VkExtensionProperties> exts(extCount);
                vk.vkEnumerateDeviceExtensionProperties(gpu, nullptr, &extCount, exts.data());
                if (d.window && !hasExtension(exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
                    continue;
                uint32_t familyCount = 0;
                vk.vkGetPhysicalDeviceQueueFamilyProperties(gpu, &familyCount, nullptr);
                std::vector<VkQueueFamilyProperties> families(familyCount);
                vk.vkGetPhysicalDeviceQueueFamilyProperties(gpu, &familyCount, families.data());
                int family = -1;
                for (uint32_t i = 0; i < familyCount && family < 0; ++i)
                {
                    if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
                        continue;
                    VkBool32 present = VK_TRUE;
                    if (d.window)
                        vk.vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, d.surface, &present);
                    if (present)
                        family = int(i);
                }
                if (family < 0)
                    continue;
                int rank = deviceRank(props.deviceType);
                if (wanted && *wanted && std::strstr(props.deviceName, wanted))
                    rank = 100;
                if (rank > bestRank)
                {
                    bestRank = rank;
                    d.gpu = gpu;
                    d.queueFamily = uint32_t(family);
                }
            }
            if (!d.gpu)
            {
                error = d.window ? "no Vulkan device can draw into this window" : "no Vulkan 1.1 device";
                return false;
            }
            VkPhysicalDeviceProperties props{};
            vk.vkGetPhysicalDeviceProperties(d.gpu, &props);
            rendererStats.adapter = props.deviceName;
            rendererStats.warp = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
            char api[48];
            std::snprintf(api, sizeof(api), "Vulkan %u.%u", VK_API_VERSION_MAJOR(props.apiVersion),
                          VK_API_VERSION_MINOR(props.apiVersion));
            rendererStats.api = api;
            vk.vkGetPhysicalDeviceMemoryProperties(d.gpu, &d.memory);
            return true;
        }

        bool createDevice(std::string &error)
        {
            uint32_t extCount = 0;
            vk.vkEnumerateDeviceExtensionProperties(d.gpu, nullptr, &extCount, nullptr);
            std::vector<VkExtensionProperties> exts(extCount);
            vk.vkEnumerateDeviceExtensionProperties(d.gpu, nullptr, &extCount, exts.data());
            std::vector<const char *> enabled;
            VkPhysicalDevicePresentIdFeaturesKHR presentId{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
            VkPhysicalDevicePresentWaitFeaturesKHR presentWait{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
            if (d.window)
            {
                enabled.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
                if (hasExtension(exts, VK_KHR_PRESENT_ID_EXTENSION_NAME) && hasExtension(exts, VK_KHR_PRESENT_WAIT_EXTENSION_NAME))
                {
                    presentId.pNext = &presentWait;
                    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
                    features.pNext = &presentId;
                    vk.vkGetPhysicalDeviceFeatures2(d.gpu, &features);
                    d.presentWait = presentId.presentId && presentWait.presentWait;
                    if (d.presentWait)
                    {
                        enabled.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
                        enabled.push_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
                    }
                }
            }
            const float priority = 1.0f;
            VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            queue.queueFamilyIndex = d.queueFamily;
            queue.queueCount = 1;
            queue.pQueuePriorities = &priority;
            VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
            info.pNext = d.presentWait ? &presentId : nullptr;
            presentId.presentId = presentWait.presentWait = d.presentWait ? VK_TRUE : VK_FALSE;
            info.queueCreateInfoCount = 1;
            info.pQueueCreateInfos = &queue;
            info.enabledExtensionCount = uint32_t(enabled.size());
            info.ppEnabledExtensionNames = enabled.data();
            if (VkResult r = vk.vkCreateDevice(d.gpu, &info, nullptr, &d.device); r != VK_SUCCESS)
                return fail(error, "vkCreateDevice", r);
            loadDeviceFunctions();
            vk.vkGetDeviceQueue(d.device, d.queueFamily, 0, &d.queue);
            return true;
        }

        uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags want)
        {
            for (uint32_t i = 0; i < d.memory.memoryTypeCount; ++i)
                if ((bits & (1u << i)) && (d.memory.memoryTypes[i].propertyFlags & want) == want)
                    return i;
            return UINT32_MAX;
        }

        void destroyBuffer(Buffer &b)
        {
            if (b.buffer)
                vk.vkDestroyBuffer(d.device, b.buffer, nullptr);
            if (b.memory)
                vk.vkFreeMemory(d.device, b.memory, nullptr); // unmaps
            b = Buffer();
        }

        // a host-visible, coherent buffer kept mapped: what the CPU writes, the GPU reads at the next submit
        bool ensureBuffer(Buffer &b, size_t bytes, VkBufferUsageFlags usage)
        {
            if (b.capacity >= bytes)
                return true;
            size_t capacity = std::max<size_t>(b.capacity * 2, 64 * 1024);
            while (capacity < bytes)
                capacity *= 2;
            destroyBuffer(b);
            VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            info.size = capacity;
            info.usage = usage;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vk.vkCreateBuffer(d.device, &info, nullptr, &b.buffer) != VK_SUCCESS)
                return false;
            VkMemoryRequirements req{};
            vk.vkGetBufferMemoryRequirements(d.device, b.buffer, &req);
            VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            alloc.allocationSize = req.size;
            alloc.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (alloc.memoryTypeIndex == UINT32_MAX || vk.vkAllocateMemory(d.device, &alloc, nullptr, &b.memory) != VK_SUCCESS ||
                vk.vkBindBufferMemory(d.device, b.buffer, b.memory, 0) != VK_SUCCESS ||
                vk.vkMapMemory(d.device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped) != VK_SUCCESS)
            {
                destroyBuffer(b);
                return false;
            }
            b.capacity = capacity;
            return true;
        }

        bool createImage(VkFormat format, int width, int height, VkImageUsageFlags usage, VkImage &image,
                         VkDeviceMemory &memory, VkImageView &view)
        {
            VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            info.imageType = VK_IMAGE_TYPE_2D;
            info.format = format;
            info.extent = {uint32_t(width), uint32_t(height), 1};
            info.mipLevels = info.arrayLayers = 1;
            info.samples = VK_SAMPLE_COUNT_1_BIT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = usage;
            info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vk.vkCreateImage(d.device, &info, nullptr, &image) != VK_SUCCESS)
                return false;
            VkMemoryRequirements req{};
            vk.vkGetImageMemoryRequirements(d.device, image, &req);
            VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            alloc.allocationSize = req.size;
            alloc.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (alloc.memoryTypeIndex == UINT32_MAX)
                alloc.memoryTypeIndex = memoryType(req.memoryTypeBits, 0);
            if (vk.vkAllocateMemory(d.device, &alloc, nullptr, &memory) != VK_SUCCESS ||
                vk.vkBindImageMemory(d.device, image, memory, 0) != VK_SUCCESS)
                return false;
            VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            v.image = image;
            v.viewType = VK_IMAGE_VIEW_TYPE_2D;
            v.format = format;
            v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            return vk.vkCreateImageView(d.device, &v, nullptr, &view) == VK_SUCCESS;
        }

        VkSemaphore makeSemaphore()
        {
            VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VkSemaphore s = VK_NULL_HANDLE;
            vk.vkCreateSemaphore(d.device, &info, nullptr, &s);
            return s;
        }

        void destroyTargets()
        {
            for (Target &t : d.targets)
            {
                if (t.framebuffer)
                    vk.vkDestroyFramebuffer(d.device, t.framebuffer, nullptr);
                if (t.view)
                    vk.vkDestroyImageView(d.device, t.view, nullptr);
                if (t.rendered)
                    vk.vkDestroySemaphore(d.device, t.rendered, nullptr);
                if (!d.swapchain && t.image)
                    vk.vkDestroyImage(d.device, t.image, nullptr); // the offscreen image is ours
            }
            d.targets.clear();
            if (d.offscreenMemory)
                vk.vkFreeMemory(d.device, d.offscreenMemory, nullptr);
            d.offscreenMemory = VK_NULL_HANDLE;
        }

        bool createFramebuffers()
        {
            for (Target &t : d.targets)
            {
                VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
                info.renderPass = d.renderPass;
                info.attachmentCount = 1;
                info.pAttachments = &t.view;
                info.width = uint32_t(d.width);
                info.height = uint32_t(d.height);
                info.layers = 1;
                if (vk.vkCreateFramebuffer(d.device, &info, nullptr, &t.framebuffer) != VK_SUCCESS)
                    return false;
                t.rendered = makeSemaphore();
            }
            return true;
        }

        // the format the swapchain will have: needed before the render pass, which the pipeline needs
        bool chooseFormat(std::string &error)
        {
            if (!d.window)
            {
                d.format = kOffscreenFormat;
                return true;
            }
            uint32_t count = 0;
            vk.vkGetPhysicalDeviceSurfaceFormatsKHR(d.gpu, d.surface, &count, nullptr);
            std::vector<VkSurfaceFormatKHR> formats(count);
            vk.vkGetPhysicalDeviceSurfaceFormatsKHR(d.gpu, d.surface, &count, formats.data());
            if (formats.empty())
                return fail(error, "vkGetPhysicalDeviceSurfaceFormatsKHR", VK_ERROR_FORMAT_NOT_SUPPORTED);
            // UNORM, like D3D11's: the shader blends in sRGB values and writes them as they are
            d.format = VK_FORMAT_UNDEFINED;
            for (VkFormat want : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_A2B10G10R10_UNORM_PACK32})
                for (const auto &f : formats)
                    if (d.format == VK_FORMAT_UNDEFINED && f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                        d.format = want;
            if (d.format == VK_FORMAT_UNDEFINED)
                d.format = formats[0].format;
            uint32_t modeCount = 0;
            vk.vkGetPhysicalDeviceSurfacePresentModesKHR(d.gpu, d.surface, &modeCount, nullptr);
            std::vector<VkPresentModeKHR> modes(modeCount);
            vk.vkGetPhysicalDeviceSurfacePresentModesKHR(d.gpu, d.surface, &modeCount, modes.data());
            d.hasMailbox = std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != modes.end();
            d.hasImmediate = std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_IMMEDIATE_KHR) != modes.end();
            rendererStats.tearing = d.hasImmediate;
            return true;
        }

        // false when the window has no area (minimised); the frame is skipped then
        bool createSwapchain(int width, int height)
        {
            VkSurfaceCapabilitiesKHR caps{};
            if (vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(d.gpu, d.surface, &caps) != VK_SUCCESS)
                return false;
            VkExtent2D extent = caps.currentExtent;
            if (extent.width == UINT32_MAX) // Wayland: the size is ours to say
                extent = {uint32_t(std::clamp<int64_t>(width, caps.minImageExtent.width, caps.maxImageExtent.width)),
                          uint32_t(std::clamp<int64_t>(height, caps.minImageExtent.height, caps.maxImageExtent.height))};
            if (extent.width == 0 || extent.height == 0)
                return false;
            d.presentMode = d.vsync ? VK_PRESENT_MODE_FIFO_KHR
                            : d.hasMailbox ? VK_PRESENT_MODE_MAILBOX_KHR
                            : d.hasImmediate ? VK_PRESENT_MODE_IMMEDIATE_KHR
                                             : VK_PRESENT_MODE_FIFO_KHR;
            uint32_t images = std::max<uint32_t>(caps.minImageCount, 2);
            if (caps.maxImageCount)
                images = std::min(images, caps.maxImageCount);
            VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
            if (!(caps.supportedCompositeAlpha & alpha))
                for (VkCompositeAlphaFlagBitsKHR a : {VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                                       VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR})
                    if (caps.supportedCompositeAlpha & a)
                    {
                        alpha = a;
                        break;
                    }
            d.transferSrc = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;

            VkSwapchainKHR old = d.swapchain;
            VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
            info.surface = d.surface;
            info.minImageCount = images;
            info.imageFormat = d.format;
            info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            info.imageExtent = extent;
            info.imageArrayLayers = 1;
            info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (d.transferSrc ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0u);
            info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            info.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                                    ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                    : caps.currentTransform;
            info.compositeAlpha = alpha;
            info.presentMode = d.presentMode;
            info.clipped = VK_TRUE;
            info.oldSwapchain = old;
            VkSwapchainKHR created = VK_NULL_HANDLE;
            const VkResult r = vk.vkCreateSwapchainKHR(d.device, &info, nullptr, &created);
            destroyTargets();
            if (old)
                vk.vkDestroySwapchainKHR(d.device, old, nullptr);
            d.swapchain = VK_NULL_HANDLE;
            if (r != VK_SUCCESS)
                return false;
            d.swapchain = created;
            d.width = int(extent.width);
            d.height = int(extent.height);
            uint32_t count = 0;
            vk.vkGetSwapchainImagesKHR(d.device, d.swapchain, &count, nullptr);
            std::vector<VkImage> list(count);
            vk.vkGetSwapchainImagesKHR(d.device, d.swapchain, &count, list.data());
            for (VkImage image : list)
            {
                Target t;
                t.image = image;
                VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                v.image = image;
                v.viewType = VK_IMAGE_VIEW_TYPE_2D;
                v.format = d.format;
                v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vk.vkCreateImageView(d.device, &v, nullptr, &t.view);
                d.targets.push_back(t);
            }
            d.swapchainStale = false;
            return createFramebuffers();
        }

        bool createOffscreen(int width, int height)
        {
            destroyTargets();
            d.width = std::max(width, 1);
            d.height = std::max(height, 1);
            Target t;
            if (!createImage(kOffscreenFormat, d.width, d.height,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, t.image,
                             d.offscreenMemory, t.view))
            {
                d.targets.push_back(t);
                destroyTargets();
                return false;
            }
            d.targets.push_back(t);
            return createFramebuffers();
        }

        // resize / vsync change / out of date: everything that holds the old images is idle first
        bool recreateTargets(int width, int height)
        {
            vk.vkDeviceWaitIdle(d.device);
            for (Frame &f : d.frames)
                f.submitted = false;
            return d.window ? createSwapchain(width, height) : createOffscreen(width, height);
        }

        bool createPipeline(std::string &error)
        {
            VkAttachmentDescription color{};
            color.format = d.format;
            color.samples = VK_SAMPLE_COUNT_1_BIT;
            color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            color.finalLayout = d.window ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &ref;
            VkSubpassDependency deps[2]{};
            // the acquire semaphore waits at colour output; the layout change from UNDEFINED happens after it
            deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
            deps[0].dstSubpass = 0;
            deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            // offscreen: readback copies after the pass
            deps[1].srcSubpass = 0;
            deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
            deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
            deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
            rp.attachmentCount = 1;
            rp.pAttachments = &color;
            rp.subpassCount = 1;
            rp.pSubpasses = &subpass;
            rp.dependencyCount = 2;
            rp.pDependencies = deps;
            if (VkResult r = vk.vkCreateRenderPass(d.device, &rp, nullptr, &d.renderPass); r != VK_SUCCESS)
                return fail(error, "vkCreateRenderPass", r);

            VkSamplerCreateInfo s{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            s.magFilter = s.minFilter = VK_FILTER_LINEAR;
            s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            s.addressModeU = s.addressModeV = s.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            s.maxLod = 0;
            if (VkResult r = vk.vkCreateSampler(d.device, &s, nullptr, &d.sampler); r != VK_SUCCESS)
                return fail(error, "vkCreateSampler", r);

            // the bindings tools/hlsl_spirv.py gives the registers: texture t0 -> 16, texture t1 -> 17, sampler s0 -> 32
            VkDescriptorSetLayoutBinding bindings[3]{};
            bindings[0] = {16, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
            bindings[1] = {17, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
            bindings[2] = {32, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
            VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            sl.bindingCount = 3;
            sl.pBindings = bindings;
            if (VkResult r = vk.vkCreateDescriptorSetLayout(d.device, &sl, nullptr, &d.setLayout); r != VK_SUCCESS)
                return fail(error, "vkCreateDescriptorSetLayout", r);
            VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
            VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            pl.setLayoutCount = 1;
            pl.pSetLayouts = &d.setLayout;
            pl.pushConstantRangeCount = 1;
            pl.pPushConstantRanges = &push;
            if (VkResult r = vk.vkCreatePipelineLayout(d.device, &pl, nullptr, &d.pipelineLayout); r != VK_SUCCESS)
                return fail(error, "vkCreatePipelineLayout", r);

            VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1}};
            VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            dp.maxSets = 1;
            dp.poolSizeCount = 2;
            dp.pPoolSizes = sizes;
            if (VkResult r = vk.vkCreateDescriptorPool(d.device, &dp, nullptr, &d.descriptorPool); r != VK_SUCCESS)
                return fail(error, "vkCreateDescriptorPool", r);
            VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            da.descriptorPool = d.descriptorPool;
            da.descriptorSetCount = 1;
            da.pSetLayouts = &d.setLayout;
            if (VkResult r = vk.vkAllocateDescriptorSets(d.device, &da, &d.descriptorSet); r != VK_SUCCESS)
                return fail(error, "vkAllocateDescriptorSets", r);

            VkShaderModule modules[2] = {};
            VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            sm.codeSize = sizeof(g_ui_vs_spv);
            sm.pCode = reinterpret_cast<const uint32_t *>(g_ui_vs_spv);
            vk.vkCreateShaderModule(d.device, &sm, nullptr, &modules[0]);
            sm.codeSize = sizeof(g_ui_ps_spv);
            sm.pCode = reinterpret_cast<const uint32_t *>(g_ui_ps_spv);
            vk.vkCreateShaderModule(d.device, &sm, nullptr, &modules[1]);
            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, modules[0], "main", nullptr};
            stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, modules[1], "main", nullptr};

            // every attribute is per instance; the vertex shader makes the corners from the vertex index. Locations in
            // the order of ui.hlsl's Instance (glslang numbers them so)
            VkVertexInputBindingDescription binding{0, sizeof(Instance), VK_VERTEX_INPUT_RATE_INSTANCE};
            const VkVertexInputAttributeDescription attributes[] = {
                {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, uint32_t(offsetof(Instance, rect))},
                {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, uint32_t(offsetof(Instance, uv))},
                {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, uint32_t(offsetof(Instance, radii))},
                {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, uint32_t(offsetof(Instance, clip))},
                {4, 0, VK_FORMAT_R8G8B8A8_UNORM, uint32_t(offsetof(Instance, color0))},
                {5, 0, VK_FORMAT_R8G8B8A8_UNORM, uint32_t(offsetof(Instance, color1))},
                {6, 0, VK_FORMAT_R8G8B8A8_UNORM, uint32_t(offsetof(Instance, border))},
                {7, 0, VK_FORMAT_R32_UINT, uint32_t(offsetof(Instance, kind))},
                {8, 0, VK_FORMAT_R32G32B32A32_SFLOAT, uint32_t(offsetof(Instance, params))},
            };
            VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            vi.vertexBindingDescriptionCount = 1;
            vi.pVertexBindingDescriptions = &binding;
            vi.vertexAttributeDescriptionCount = uint32_t(std::size(attributes));
            vi.pVertexAttributeDescriptions = attributes;
            VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            vp.viewportCount = vp.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            rs.polygonMode = VK_POLYGON_MODE_FILL;
            rs.cullMode = VK_CULL_MODE_NONE;
            rs.lineWidth = 1.0f;
            VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineColorBlendAttachmentState blend{}; // premultiplied alpha
            blend.blendEnable = VK_TRUE;
            blend.srcColorBlendFactor = blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blend.dstColorBlendFactor = blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blend.colorBlendOp = blend.alphaBlendOp = VK_BLEND_OP_ADD;
            blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            cb.attachmentCount = 1;
            cb.pAttachments = &blend;
            const VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
            dy.dynamicStateCount = 2;
            dy.pDynamicStates = dynamic;
            VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            gp.stageCount = 2;
            gp.pStages = stages;
            gp.pVertexInputState = &vi;
            gp.pInputAssemblyState = &ia;
            gp.pViewportState = &vp;
            gp.pRasterizationState = &rs;
            gp.pMultisampleState = &ms;
            gp.pColorBlendState = &cb;
            gp.pDynamicState = &dy;
            gp.layout = d.pipelineLayout;
            gp.renderPass = d.renderPass;
            const VkResult r = modules[0] && modules[1]
                                   ? vk.vkCreateGraphicsPipelines(d.device, VK_NULL_HANDLE, 1, &gp, nullptr, &d.pipeline)
                                   : VK_ERROR_INVALID_SHADER_NV;
            for (VkShaderModule m : modules)
                if (m)
                    vk.vkDestroyShaderModule(d.device, m, nullptr);
            return r == VK_SUCCESS || fail(error, "vkCreateGraphicsPipelines", r);
        }

        bool createFrames(std::string &error)
        {
            for (Frame &f : d.frames)
            {
                VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
                pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
                pool.queueFamilyIndex = d.queueFamily;
                if (VkResult r = vk.vkCreateCommandPool(d.device, &pool, nullptr, &f.pool); r != VK_SUCCESS)
                    return fail(error, "vkCreateCommandPool", r);
                VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
                alloc.commandPool = f.pool;
                alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                alloc.commandBufferCount = 1;
                vk.vkAllocateCommandBuffers(d.device, &alloc, &f.commands);
                VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
                fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
                vk.vkCreateFence(d.device, &fence, nullptr, &f.fence);
                f.acquired = makeSemaphore();
            }
            return true;
        }

        void destroyAtlas()
        {
            if (d.atlasView)
                vk.vkDestroyImageView(d.device, d.atlasView, nullptr);
            if (d.atlasImage)
                vk.vkDestroyImage(d.device, d.atlasImage, nullptr);
            if (d.atlasMemory)
                vk.vkFreeMemory(d.device, d.atlasMemory, nullptr);
            d.atlasView = VK_NULL_HANDLE;
            d.atlasImage = VK_NULL_HANDLE;
            d.atlasMemory = VK_NULL_HANDLE;
            d.atlasSize = 0;
        }

        void destroyImageAtlas()
        {
            if (d.imageAtlasView)
                vk.vkDestroyImageView(d.device, d.imageAtlasView, nullptr);
            if (d.imageAtlasImage)
                vk.vkDestroyImage(d.device, d.imageAtlasImage, nullptr);
            if (d.imageAtlasMemory)
                vk.vkFreeMemory(d.device, d.imageAtlasMemory, nullptr);
            d.imageAtlasView = VK_NULL_HANDLE;
            d.imageAtlasImage = VK_NULL_HANDLE;
            d.imageAtlasMemory = VK_NULL_HANDLE;
            d.imageAtlasGeneration = 0;
        }

        void destroy()
        {
            if (d.device)
            {
                vk.vkDeviceWaitIdle(d.device);
                for (Frame &f : d.frames)
                {
                    destroyBuffer(f.instances);
                    destroyBuffer(f.staging);
                    destroyBuffer(f.imageStaging);
                    if (f.acquired)
                        vk.vkDestroySemaphore(d.device, f.acquired, nullptr);
                    if (f.fence)
                        vk.vkDestroyFence(d.device, f.fence, nullptr);
                    if (f.pool)
                        vk.vkDestroyCommandPool(d.device, f.pool, nullptr);
                    f = Frame();
                }
                destroyImageAtlas();
                destroyAtlas();
                destroyTargets();
                if (d.swapchain)
                    vk.vkDestroySwapchainKHR(d.device, d.swapchain, nullptr);
                if (d.pipeline)
                    vk.vkDestroyPipeline(d.device, d.pipeline, nullptr);
                if (d.pipelineLayout)
                    vk.vkDestroyPipelineLayout(d.device, d.pipelineLayout, nullptr);
                if (d.descriptorPool)
                    vk.vkDestroyDescriptorPool(d.device, d.descriptorPool, nullptr);
                if (d.setLayout)
                    vk.vkDestroyDescriptorSetLayout(d.device, d.setLayout, nullptr);
                if (d.sampler)
                    vk.vkDestroySampler(d.device, d.sampler, nullptr);
                if (d.renderPass)
                    vk.vkDestroyRenderPass(d.device, d.renderPass, nullptr);
                vk.vkDestroyDevice(d.device, nullptr);
            }
            if (d.instance)
            {
                if (d.surface)
                    vk.vkDestroySurfaceKHR(d.instance, d.surface, nullptr);
                if (d.messenger)
                    vk.vkDestroyDebugUtilsMessengerEXT(d.instance, d.messenger, nullptr);
                vk.vkDestroyInstance(d.instance, nullptr);
            }
            const platform::NativeSurface *window = d.window;
            const int w = d.width, h = d.height;
            const bool vsync = d.vsync;
            d = State();
            d.window = window;
            d.width = w;
            d.height = h;
            d.vsync = vsync;
        }

        bool create(std::string &error)
        {
            if (!loadLibrary(error) || !createInstance(error))
                return false;
            if (d.window && !createSurface(error))
                return false;
            if (!pickDevice(error) || !createDevice(error) || !chooseFormat(error) || !createPipeline(error) ||
                !createFrames(error))
                return false;
            const int w = d.width, h = d.height;
            if (d.window)
                createSwapchain(w, h); // none while minimised: made at the first frame with an area
            else if (!createOffscreen(w, h))
                return fail(error, "the offscreen image", VK_ERROR_OUT_OF_DEVICE_MEMORY);
            return true;
        }

        void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess,
                          VkAccessFlags dstAccess, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
        {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.srcAccessMask = srcAccess;
            b.dstAccessMask = dstAccess;
            b.oldLayout = from;
            b.newLayout = to;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vk.vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
        }

        // the glyph atlas: recreated when it grew or was cleared (rare; everything idle first), else only the rectangle
        // new glyphs touched, through this frame's staging buffer
        void uploadAtlas(Frame &f, VkCommandBuffer cmd)
        {
            if (!d.atlasImage || d.atlasGeneration != atlas.generation || d.atlasSize != atlas.size)
            {
                vk.vkDeviceWaitIdle(d.device);
                destroyAtlas();
                if (!createImage(VK_FORMAT_R8_UNORM, atlas.size, atlas.size,
                                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, d.atlasImage, d.atlasMemory,
                                 d.atlasView))
                {
                    destroyAtlas();
                    return;
                }
                d.atlasSize = atlas.size;
                d.atlasGeneration = atlas.generation;
                d.atlasFresh = true;
                atlas.markAllDirty();
                VkDescriptorImageInfo image{VK_NULL_HANDLE, d.atlasView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                VkDescriptorImageInfo sampler{d.sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
                VkWriteDescriptorSet writes[2]{};
                writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, d.descriptorSet, 16, 0, 1,
                             VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &image, nullptr, nullptr};
                writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, d.descriptorSet, 32, 0, 1,
                             VK_DESCRIPTOR_TYPE_SAMPLER, &sampler, nullptr, nullptr};
                vk.vkUpdateDescriptorSets(d.device, 2, writes, 0, nullptr);
            }
            if (atlas.dirtyX0 >= atlas.dirtyX1 || atlas.dirtyY0 >= atlas.dirtyY1)
                return;
            const int w = atlas.dirtyX1 - atlas.dirtyX0, h = atlas.dirtyY1 - atlas.dirtyY0;
            const size_t bytes = size_t(w) * size_t(h);
            if (!ensureBuffer(f.staging, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
                return;
            auto *dst = static_cast<uint8_t *>(f.staging.mapped);
            for (int y = 0; y < h; ++y)
                std::memcpy(dst + size_t(y) * size_t(w),
                            atlas.pixels.data() + size_t(atlas.dirtyY0 + y) * size_t(atlas.size) + size_t(atlas.dirtyX0), size_t(w));
            imageBarrier(cmd, d.atlasImage, d.atlasFresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, d.atlasFresh ? 0 : VK_ACCESS_SHADER_READ_BIT,
                         VK_ACCESS_TRANSFER_WRITE_BIT,
                         d.atlasFresh ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy region{};
            region.bufferRowLength = uint32_t(w);
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageOffset = {atlas.dirtyX0, atlas.dirtyY0, 0};
            region.imageExtent = {uint32_t(w), uint32_t(h), 1};
            vk.vkCmdCopyBufferToImage(cmd, f.staging.buffer, d.atlasImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            imageBarrier(cmd, d.atlasImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            d.atlasFresh = false;
            rendererStats.uploadBytes += bytes;
            atlas.dirtyX0 = atlas.dirtyY0 = atlas.size;
            atlas.dirtyX1 = atlas.dirtyY1 = 0;
        }

        void uploadImageAtlas(Frame &f, VkCommandBuffer cmd)
        {
            if (d.imageAtlasImage && d.imageAtlasGeneration == imageAtlas.generation)
                return;
            vk.vkDeviceWaitIdle(d.device);
            destroyImageAtlas();
            const bool hasImages = !imageAtlas.pixels.empty() && imageAtlas.width > 0 && imageAtlas.height > 0;
            const int w = hasImages ? imageAtlas.width : 1;
            const int h = hasImages ? imageAtlas.height : 1;
            if (!createImage(VK_FORMAT_R8G8B8A8_UNORM, w, h,
                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, d.imageAtlasImage,
                             d.imageAtlasMemory, d.imageAtlasView))
            {
                destroyImageAtlas();
                return;
            }
            d.imageAtlasGeneration = imageAtlas.generation;
            VkDescriptorImageInfo image{VK_NULL_HANDLE, d.imageAtlasView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, d.descriptorSet, 17, 0, 1,
                                       VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &image, nullptr, nullptr};
            vk.vkUpdateDescriptorSets(d.device, 1, &write, 0, nullptr);

            const size_t bytes = size_t(w) * size_t(h) * 4;
            if (!ensureBuffer(f.imageStaging, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
                return;
            if (hasImages)
                std::memcpy(f.imageStaging.mapped, imageAtlas.pixels.data(), bytes);
            else
                std::memset(f.imageStaging.mapped, 0, bytes);
            imageBarrier(cmd, d.imageAtlasImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy region{};
            region.bufferRowLength = uint32_t(w);
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageOffset = {0, 0, 0};
            region.imageExtent = {uint32_t(w), uint32_t(h), 1};
            vk.vkCmdCopyBufferToImage(cmd, f.imageStaging.buffer, d.imageAtlasImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            imageBarrier(cmd, d.imageAtlasImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            if (hasImages)
                rendererStats.uploadBytes += bytes;
        }

        void rebuild()
        {
            // driver reset, GPU gone: start over from what the CPU has
            destroy();
            std::string error;
            if (!create(error))
            {
                std::fprintf(stderr, "[vulkan] could not recreate the device: %s\n", error.c_str());
                destroy();
            }
            atlas.markAllDirty();
        }
    }

    bool rendererInit(void *window, int width, int height, std::string &error)
    {
        d = State();
        d.window = static_cast<const platform::NativeSurface *>(window);
        d.width = std::max(width, 1);
        d.height = std::max(height, 1);
        if (create(error))
            return true;
        destroy();
        return false;
    }

    void rendererShutdown()
    {
        destroy();
        d = State();
    }

    // One wait per present, as on Direct3D 11. The last frame on screen (present wait) - else this slot's previous
    // frame finished on the GPU, which is what reusing its buffers needs anyway.
    void rendererWaitForFrame()
    {
        if (d.frameWaited || !d.device)
            return;
        d.frameWaited = true;
        if (d.presentWait && d.swapchain && d.presentId > 0)
        {
            const VkResult r = vk.vkWaitForPresentKHR(d.device, d.swapchain, d.presentId, 100'000'000);
            if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR)
                d.swapchainStale = true;
        }
        Frame &f = d.frames[d.frame];
        if (f.submitted)
            vk.vkWaitForFences(d.device, 1, &f.fence, VK_TRUE, 1'000'000'000);
    }

    void rendererRender(core::Color clear, int width, int height)
    {
        rendererStats.instances = rendererStats.drawCalls = 0;
        rendererStats.uploadBytes = 0;
        d.acquired = d.rendered = false;
        if (!d.device || width <= 0 || height <= 0)
            return;
        Frame &f = d.frames[d.frame];
        if (f.submitted && vk.vkWaitForFences(d.device, 1, &f.fence, VK_TRUE, 1'000'000'000) == VK_ERROR_DEVICE_LOST)
        {
            d.loseDevice = true;
            return;
        }

        if (d.window)
        {
            if (!d.swapchain || d.swapchainStale || width != d.width || height != d.height)
                if (!recreateTargets(width, height))
                    return; // no area yet (minimised): nothing to draw into
            VkResult r = vk.vkAcquireNextImageKHR(d.device, d.swapchain, kAcquireTimeout, f.acquired, VK_NULL_HANDLE, &d.imageIndex);
            if (r == VK_ERROR_OUT_OF_DATE_KHR)
            {
                if (!recreateTargets(width, height))
                    return;
                r = vk.vkAcquireNextImageKHR(d.device, d.swapchain, kAcquireTimeout, f.acquired, VK_NULL_HANDLE, &d.imageIndex);
            }
            if (r == VK_SUBOPTIMAL_KHR)
                d.swapchainStale = true; // drawn and shown this once; remade before the next
            else if (r == VK_ERROR_DEVICE_LOST)
            {
                d.loseDevice = true;
                return;
            }
            else if (r != VK_SUCCESS)
                return; // timeout / not ready: the compositor holds every image; skip this frame
        }
        else
        {
            if ((width != d.width || height != d.height) && !recreateTargets(width, height))
                return;
            d.imageIndex = 0;
        }
        const Target &target = d.targets[d.imageIndex];

        vk.vkResetCommandPool(d.device, f.pool, 0);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VkCommandBuffer cmd = f.commands;
        vk.vkBeginCommandBuffer(cmd, &begin);
        uploadAtlas(f, cmd);
        uploadImageAtlas(f, cmd);

        const size_t count = draw.instances.size();
        const bool drawing = count && d.atlasImage &&
                             ensureBuffer(f.instances, count * sizeof(Instance), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        if (drawing)
        {
            std::memcpy(f.instances.mapped, draw.instances.data(), count * sizeof(Instance));
            rendererStats.uploadBytes += count * sizeof(Instance);
        }

        VkClearValue clearValue{};
        for (int i = 0; i < 4; ++i)
            clearValue.color.float32[i] = float((clear >> (i * 8)) & 0xFF) / 255.0f;
        VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rp.renderPass = d.renderPass;
        rp.framebuffer = target.framebuffer;
        rp.renderArea = {{0, 0}, {uint32_t(d.width), uint32_t(d.height)}};
        rp.clearValueCount = 1;
        rp.pClearValues = &clearValue;
        vk.vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
        if (drawing)
        {
            // flipped (Vulkan 1.1 core): ui.hlsl's clip space is Direct3D's, y up
            const VkViewport viewport{0, float(d.height), float(d.width), -float(d.height), 0, 1};
            const VkRect2D scissor{{0, 0}, {uint32_t(d.width), uint32_t(d.height)}};
            const float constants[4] = {float(d.width), float(d.height), 1.0f / float(std::max(atlas.size, 1)),
                                        1.0f / float(std::max(atlas.size, 1))};
            const VkDeviceSize offset = 0;
            vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d.pipeline);
            vk.vkCmdSetViewport(cmd, 0, 1, &viewport);
            vk.vkCmdSetScissor(cmd, 0, 1, &scissor);
            vk.vkCmdPushConstants(cmd, d.pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                  sizeof(constants), constants);
            vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d.pipelineLayout, 0, 1, &d.descriptorSet, 0, nullptr);
            vk.vkCmdBindVertexBuffers(cmd, 0, 1, &f.instances.buffer, &offset);
            vk.vkCmdDraw(cmd, 4, uint32_t(count), 0, 0);
            rendererStats.instances = int(count);
            rendererStats.drawCalls = 1;
        }
        vk.vkCmdEndRenderPass(cmd);
        vk.vkEndCommandBuffer(cmd);

        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        if (d.window)
        {
            submit.waitSemaphoreCount = 1;
            submit.pWaitSemaphores = &f.acquired;
            submit.pWaitDstStageMask = &waitStage;
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &target.rendered;
        }
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        vk.vkResetFences(d.device, 1, &f.fence);
        const VkResult r = vk.vkQueueSubmit(d.queue, 1, &submit, f.fence);
        if (r == VK_ERROR_DEVICE_LOST)
        {
            d.loseDevice = true;
            return;
        }
        f.submitted = r == VK_SUCCESS;
        d.rendered = f.submitted;
        d.acquired = d.window && f.submitted;
    }

    void rendererPresent(bool vsync)
    {
        d.frameWaited = false;
        if (vsync != d.vsync)
        {
            d.vsync = vsync;
            d.swapchainStale = true; // another present mode: a new swapchain at the next frame
        }
        if (d.acquired)
        {
            d.acquired = false;
            VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            present.waitSemaphoreCount = 1;
            present.pWaitSemaphores = &d.targets[d.imageIndex].rendered;
            present.swapchainCount = 1;
            present.pSwapchains = &d.swapchain;
            present.pImageIndices = &d.imageIndex;
            VkPresentIdKHR id{VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
            const uint64_t next = d.presentId + 1;
            if (d.presentWait)
            {
                id.swapchainCount = 1;
                id.pPresentIds = &next;
                present.pNext = &id;
            }
            const VkResult r = vk.vkQueuePresentKHR(d.queue, &present);
            if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR)
                d.presentId = next;
            if (r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_OUT_OF_DATE_KHR)
                d.swapchainStale = true;
            else if (r == VK_ERROR_DEVICE_LOST || r == VK_ERROR_SURFACE_LOST_KHR)
                d.loseDevice = true;
        }
        if (d.device)
            d.frame = (d.frame + 1) % kFramesInFlight;
        if (d.loseDevice)
            rebuild();
    }

    bool rendererReadback(std::vector<uint8_t> &rgba, int &width, int &height)
    {
        if (!d.device || !d.rendered || d.targets.empty())
            return false;
        const bool swapchain = d.swapchain != VK_NULL_HANDLE;
        if (swapchain && !d.transferSrc)
            return false;
        vk.vkDeviceWaitIdle(d.device);
        const Target &target = d.targets[d.imageIndex];
        const size_t bytes = size_t(d.width) * size_t(d.height) * 4;
        Buffer readback;
        {
            VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            info.size = bytes;
            info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            if (vk.vkCreateBuffer(d.device, &info, nullptr, &readback.buffer) != VK_SUCCESS)
                return false;
            VkMemoryRequirements req{};
            vk.vkGetBufferMemoryRequirements(d.device, readback.buffer, &req);
            VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            alloc.allocationSize = req.size;
            alloc.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (alloc.memoryTypeIndex == UINT32_MAX || vk.vkAllocateMemory(d.device, &alloc, nullptr, &readback.memory) != VK_SUCCESS ||
                vk.vkBindBufferMemory(d.device, readback.buffer, readback.memory, 0) != VK_SUCCESS ||
                vk.vkMapMemory(d.device, readback.memory, 0, VK_WHOLE_SIZE, 0, &readback.mapped) != VK_SUCCESS)
            {
                destroyBuffer(readback);
                return false;
            }
        }
        // a one-off command buffer from this frame's pool (the frame is idle: everything was waited for)
        Frame &f = d.frames[d.frame];
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = f.pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        vk.vkAllocateCommandBuffers(d.device, &alloc, &cmd);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk.vkBeginCommandBuffer(cmd, &begin);
        if (swapchain)
            imageBarrier(cmd, target.image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0,
                         VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {uint32_t(d.width), uint32_t(d.height), 1};
        vk.vkCmdCopyImageToBuffer(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region);
        if (swapchain)
            imageBarrier(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                         VK_ACCESS_TRANSFER_READ_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        vk.vkEndCommandBuffer(cmd);
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        const bool ok = vk.vkQueueSubmit(d.queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS &&
                        vk.vkDeviceWaitIdle(d.device) == VK_SUCCESS;
        if (ok)
        {
            width = d.width;
            height = d.height;
            rgba.resize(bytes);
            const auto *src = static_cast<const uint8_t *>(readback.mapped);
            const bool bgra = (swapchain ? d.format : kOffscreenFormat) == VK_FORMAT_B8G8R8A8_UNORM;
            for (size_t i = 0; i < bytes; i += 4)
            {
                rgba[i + 0] = src[i + (bgra ? 2 : 0)];
                rgba[i + 1] = src[i + 1];
                rgba[i + 2] = src[i + (bgra ? 0 : 2)];
                rgba[i + 3] = 255; // opaque, as the window shows it
            }
        }
        vk.vkFreeCommandBuffers(d.device, f.pool, 1, &cmd);
        destroyBuffer(readback);
        return ok;
    }

    void rendererSimulateDeviceLoss() { d.loseDevice = true; }
}
