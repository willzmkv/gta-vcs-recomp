// Vulkan backend of rhi.h (Vulkan 1.3: dynamic rendering, synchronization2, push descriptors). The only file that
// includes Vulkan headers. It keeps D3D11's behaviour, which the renderer was written against (rhi_d3d11.cpp):
//
// - D3D's Y-up NDC through a negative viewport height; depth 0..1 in both; pixel centres at +0.5 in both.
// - Bindings follow the D3D registers (tools/hlsl_spirv.py): cbuffer bN -> binding N, texture tN -> 16 + N, sampler
//   sN -> 32 + N, one push-descriptor set. Unbound slots read zeros (a 1x1 black texture, a zero buffer), as D3D's
//   null views do; so does a texture that is also the pass's target (D3D unbinds it).
// - State (pipeline, buffers, textures, viewport, scissor) stays set across draws and passes, as in D3D11; beginPass
//   unbinds the textures and resets the viewport and scissor to the target.
// - Dynamic buffers rewritten whole (WRITE_DISCARD): each update takes a new slice of the frame's upload ring, so
//   draws recorded earlier keep their data; a buffer drawn in a later frame without an update is copied again from
//   its CPU copy.
// - Copies, uploads and readbacks may come inside a pass: the pass's rendering is suspended around them (counted in
//   the perf line, PS2X_RHI_STATS=1) and resumed with its contents loaded.
// - Textures rest in SHADER_READ_ONLY (sampled) or their attachment layout and go back there after every operation;
//   a pass's targets are in their attachment layouts while it lasts.
// - New render targets and depth textures start cleared to zero (D3D drivers hand them out that way).
// - Objects freed while the GPU may still use them are destroyed when their frame's fence has passed.
#include "rhi/rhi.h"

#include <volk.h>

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif
#include <vk_mem_alloc.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace rhi
{
    namespace
    {
        constexpr uint32_t kFrames = 2;          // frames in flight
        constexpr uint32_t kUniformSlots = 4;    // cbuffer b0..b3
        constexpr uint32_t kTextureSlots = 8;    // t0..t7 / s0..s7 (beginPass unbinds 8, as rhi_d3d11.cpp)
        constexpr uint32_t kTextureBinding = 16; // tools/hlsl_spirv.py
        constexpr uint32_t kSamplerBinding = 32;
        constexpr VkDeviceSize kRingChunk = 8u << 20;
        constexpr VkDeviceSize kRingAlign = 256; // >= minUniformBufferOffsetAlignment everywhere
        // who reads a buffer the RHI updates by a copy: vertex / index input, constants in both shader stages
        constexpr VkPipelineStageFlags2 kBufferReaders = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                                                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        constexpr VkAccessFlags2 kBufferReads = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT;

        // PS2X_RHI_LOG_MEMORY=1: every device memory allocation / free VMA makes, with its size and the time it took
        // (block churn while driving: a new block is a driver buffer made, cleared and mapped on the calling thread)
        bool logMemory()
        {
            static const bool on = std::getenv("PS2X_RHI_LOG_MEMORY") != nullptr;
            return on;
        }
        PFN_vkAllocateMemory g_realAllocateMemory = nullptr;
        PFN_vkFreeMemory g_realFreeMemory = nullptr;
        double nowSeconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
        VKAPI_ATTR VkResult VKAPI_CALL loggedAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *cb,
                                                            VkDeviceMemory *memory)
        {
            const double t0 = nowSeconds();
            const VkResult r = g_realAllocateMemory(device, info, cb, memory);
            std::fprintf(stderr, "[vk-mem] t=%.3f allocate %.1f MB type %u: %.2f ms\n", t0, info->allocationSize / 1048576.0,
                         info->memoryTypeIndex, (nowSeconds() - t0) * 1e3);
            return r;
        }
        VKAPI_ATTR void VKAPI_CALL loggedFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *cb)
        {
            const double t0 = nowSeconds();
            g_realFreeMemory(device, memory, cb);
            std::fprintf(stderr, "[vk-mem] t=%.3f free: %.2f ms\n", t0, (nowSeconds() - t0) * 1e3);
        }

        // PS2X_HOST_SPIKES: a clock for the per-draw timers, 0 when off (no clock reads then)
        int64_t spikeClock()
        {
            static const bool on = std::getenv("PS2X_HOST_SPIKES") != nullptr;
            return on ? std::chrono::steady_clock::now().time_since_epoch().count() : 0;
        }

        // Depth test / write / compare as dynamic state (core Vulkan 1.3): pipelines that differ only there are one
        // VkPipeline, so a depth change is no pipeline bind - two thirds of the binds while driving were only that
        // (PERFORMANCE.md 27). PS2X_RHI_STATIC_DEPTH=1: the depth state in each pipeline, as before.
        bool dynamicDepth()
        {
            static const bool on = [] {
                const char *e = std::getenv("PS2X_RHI_STATIC_DEPTH");
                return !(e && *e && *e != '0');
            }();
            return on;
        }

        // PS2X_RHI_FULL_COPY=1: a dynamic vertex / index buffer's whole capacity copied into the frame's ring, as before
        bool fullCopy()
        {
            static const bool on = [] {
                const char *e = std::getenv("PS2X_RHI_FULL_COPY");
                return e && *e && *e != '0';
            }();
            return on;
        }

        // PS2X_RHI_REDUNDANT=1: viewport, scissor and buffers recorded at every draw that set them, as before
        bool redundantState()
        {
            static const bool on = [] {
                const char *e = std::getenv("PS2X_RHI_REDUNDANT");
                return e && *e && *e != '0';
            }();
            return on;
        }

        bool statsOn()
        {
            static const bool on = std::getenv("PS2X_RHI_STATS") != nullptr;
            return on;
        }

        const char *vkResultName(VkResult r)
        {
            switch (r)
            {
            case VK_SUCCESS: return "VK_SUCCESS";
            case VK_NOT_READY: return "VK_NOT_READY";
            case VK_TIMEOUT: return "VK_TIMEOUT";
            case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
            case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
            case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
            case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
            case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
            case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
            case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
            case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
            case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
            case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
            case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
            default: return "VkResult";
            }
        }

        VkFormat vkFormat(Format f)
        {
            switch (f)
            {
            case Format::RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
            case Format::R32F: return VK_FORMAT_R32_SFLOAT;
            case Format::D32F: return VK_FORMAT_D32_SFLOAT;
            case Format::BGRA8: return VK_FORMAT_B8G8R8A8_UNORM;
            case Format::BC1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
            case Format::BC2: return VK_FORMAT_BC2_UNORM_BLOCK;
            case Format::BC3: return VK_FORMAT_BC3_UNORM_BLOCK;
            case Format::BC4: return VK_FORMAT_BC4_UNORM_BLOCK;
            case Format::BC5: return VK_FORMAT_BC5_UNORM_BLOCK;
            case Format::BC7: return VK_FORMAT_BC7_UNORM_BLOCK;
            default: return VK_FORMAT_UNDEFINED;
            }
        }

        VkBlendFactor blendFactor(BlendFactor f)
        {
            switch (f)
            {
            case BlendFactor::Zero: return VK_BLEND_FACTOR_ZERO;
            case BlendFactor::One: return VK_BLEND_FACTOR_ONE;
            case BlendFactor::SrcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
            case BlendFactor::InvSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            case BlendFactor::DstAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
            case BlendFactor::InvDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
            case BlendFactor::Src1Alpha: return VK_BLEND_FACTOR_SRC1_ALPHA;
            case BlendFactor::InvSrc1Alpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
            case BlendFactor::Constant: return VK_BLEND_FACTOR_CONSTANT_COLOR;
            case BlendFactor::InvConstant: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
            }
            return VK_BLEND_FACTOR_ONE;
        }

        VkBlendOp blendOp(BlendOp op)
        {
            switch (op)
            {
            case BlendOp::Add: return VK_BLEND_OP_ADD;
            case BlendOp::Subtract: return VK_BLEND_OP_SUBTRACT;
            case BlendOp::ReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
            case BlendOp::Min: return VK_BLEND_OP_MIN;
            case BlendOp::Max: return VK_BLEND_OP_MAX;
            }
            return VK_BLEND_OP_ADD;
        }

        VkCompareOp compare(Compare c)
        {
            switch (c)
            {
            case Compare::Never: return VK_COMPARE_OP_NEVER;
            case Compare::Less: return VK_COMPARE_OP_LESS;
            case Compare::Equal: return VK_COMPARE_OP_EQUAL;
            case Compare::LessEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
            case Compare::Greater: return VK_COMPARE_OP_GREATER;
            case Compare::NotEqual: return VK_COMPARE_OP_NOT_EQUAL;
            case Compare::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
            case Compare::Always: return VK_COMPARE_OP_ALWAYS;
            }
            return VK_COMPARE_OP_ALWAYS;
        }

        VkFormat vertexFormat(VertexFormat f)
        {
            switch (f)
            {
            case VertexFormat::Float1: return VK_FORMAT_R32_SFLOAT;
            case VertexFormat::Float2: return VK_FORMAT_R32G32_SFLOAT;
            case VertexFormat::Float3: return VK_FORMAT_R32G32B32_SFLOAT;
            case VertexFormat::Float4: return VK_FORMAT_R32G32B32A32_SFLOAT;
            case VertexFormat::UByte4Norm: return VK_FORMAT_R8G8B8A8_UNORM;
            case VertexFormat::UInt1: return VK_FORMAT_R32_UINT;
            }
            return VK_FORMAT_UNDEFINED;
        }

        VkPrimitiveTopology topology(Topology t)
        {
            switch (t)
            {
            case Topology::Triangles: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            case Topology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            case Topology::Lines: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
            case Topology::Points: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
            }
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        }

        // ---- the device's lifetime: shared by every object made from it ------------------------------------------
        // Textures / buffers / pipelines may outlive the Device object (the presenter drops it first); they keep the
        // core alive and destroy their Vulkan objects at once when the device is gone (it waited for the GPU).
        struct Core
        {
            VkInstance instance = VK_NULL_HANDLE;
            VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
            VkPhysicalDevice physical = VK_NULL_HANDLE;
            VkDevice device = VK_NULL_HANDLE;
            VmaAllocator allocator = VK_NULL_HANDLE;
            bool alive = false; // the Device object exists: frees go through its frame queues
            uint32_t frame = 0; // the frame slot being recorded
            std::array<std::vector<std::function<void()>>, kFrames> garbage;
            // the device's bindings forget an object being destroyed (D3D11 kept a reference to what is bound; the
            // renderer replaces buffers and textures that are still bound)
            std::function<void(const void *)> unbind;
            // The thread that records (set at each beginFrame). Textures may also be made on other threads
            // (freeThreadedCreation: the GS stage's texture makers); one released there was never bound, and its frees
            // wait in `orphans` until the owner's next submit puts them into that frame's queue.
            std::atomic<std::thread::id> owner{};
            std::mutex orphanMutex;
            std::vector<std::function<void()>> orphans;

            bool onOwner() const { return owner.load(std::memory_order_relaxed) == std::this_thread::get_id(); }

            void forget(const void *object)
            {
                if (alive && unbind && onOwner())
                    unbind(object);
            }

            void defer(std::function<void()> free)
            {
                if (!onOwner())
                {
                    std::lock_guard<std::mutex> lock(orphanMutex);
                    orphans.push_back(std::move(free));
                }
                else if (alive)
                    garbage[frame].push_back(std::move(free));
                else
                    free();
            }

            std::vector<std::function<void()>> takeOrphans()
            {
                std::lock_guard<std::mutex> lock(orphanMutex);
                std::vector<std::function<void()>> out;
                out.swap(orphans);
                return out;
            }

            void collect(uint32_t slot)
            {
                std::vector<std::function<void()>> items;
                items.swap(garbage[slot]);
                for (auto &f : items)
                    f();
            }

            ~Core()
            {
                if (device)
                    vkDeviceWaitIdle(device);
                for (uint32_t i = 0; i < kFrames; ++i)
                    collect(i);
                for (auto &f : takeOrphans())
                    f();
                if (allocator)
                    vmaDestroyAllocator(allocator);
                if (device)
                    vkDestroyDevice(device, nullptr);
                if (messenger)
                    vkDestroyDebugUtilsMessengerEXT(instance, messenger, nullptr);
                if (instance)
                    vkDestroyInstance(instance, nullptr);
            }
        };

        class VkTexture final : public Texture
        {
        public:
            VkTexture(std::shared_ptr<Core> core, const TextureDesc &d) : core(std::move(core)) { m_desc = d; }
            ~VkTexture() override
            {
                core->forget(this);
                Core *c = core.get();
                VkImage img = image;
                VkImageView sv = sampledView, av = attachmentView;
                VmaAllocation a = allocation;
                core->defer([c, img, sv, av, a] {
                    if (sv)
                        vkDestroyImageView(c->device, sv, nullptr);
                    if (av && av != sv)
                        vkDestroyImageView(c->device, av, nullptr);
                    if (img)
                        vmaDestroyImage(c->allocator, img, a);
                });
            }

            std::shared_ptr<Core> core;
            VkImage image = VK_NULL_HANDLE;
            VmaAllocation allocation = VK_NULL_HANDLE;
            VkImageView sampledView = VK_NULL_HANDLE;    // every mip level
            VkImageView attachmentView = VK_NULL_HANDLE; // level 0
            VkFormat format = VK_FORMAT_UNDEFINED;
            VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
            VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
            uint32_t mips = 1;
            VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;  // now (every level)
            VkImageLayout resting = VK_IMAGE_LAYOUT_UNDEFINED; // between operations, outside a pass that targets it
        };

        class VkBuf final : public Buffer
        {
        public:
            VkBuf(std::shared_ptr<Core> core, const BufferDesc &d) : core(std::move(core)) { m_desc = d; }
            ~VkBuf() override
            {
                core->forget(this);
                if (!buffer)
                    return;
                Core *c = core.get();
                VkBuffer b = buffer;
                VmaAllocation a = allocation;
                core->defer([c, b, a] { vmaDestroyBuffer(c->allocator, b, a); });
            }

            std::shared_ptr<Core> core;
            // static buffers (desc.dynamic false): their own memory, updated by copies
            VkBuffer buffer = VK_NULL_HANDLE;
            VmaAllocation allocation = VK_NULL_HANDLE;
            // dynamic buffers: the latest contents and where the current frame has them in its ring
            std::vector<uint8_t> shadow;
            size_t used = 0; // vertex / index buffers: the bytes the last update wrote (the rest is undefined: WRITE_DISCARD)
            VkBuffer sliceBuffer = VK_NULL_HANDLE;
            VkDeviceSize sliceOffset = 0;
            uint64_t sliceFrame = 0; // the frame serial the slice belongs to (0: none yet)
        };

        class VkSamp final : public Sampler
        {
        public:
            VkSamp(std::shared_ptr<Core> core) : core(std::move(core)) {}
            ~VkSamp() override
            {
                core->forget(this);
                Core *c = core.get();
                VkSampler s = sampler;
                core->defer([c, s] { vkDestroySampler(c->device, s, nullptr); });
            }
            std::shared_ptr<Core> core;
            VkSampler sampler = VK_NULL_HANDLE;
        };

        class VkPipe final : public Pipeline
        {
        public:
            VkPipe(std::shared_ptr<Core> core) : core(std::move(core)) {}
            ~VkPipe() override
            {
                core->forget(this);
                if (sharedVariants) // the device's shared pipelines (dynamic depth): destroyed with the device
                    return;
                Core *c = core.get();
                std::vector<VkPipeline> all;
                for (auto &[key, p] : variants)
                    all.push_back(p);
                core->defer([c, all] {
                    for (VkPipeline p : all)
                        vkDestroyPipeline(c->device, p, nullptr);
                });
            }
            std::shared_ptr<Core> core;
            PipelineDesc desc;
            std::string name;
            VkShaderModule vs = VK_NULL_HANDLE, ps = VK_NULL_HANDLE; // owned by the device's module cache
            std::unordered_map<uint64_t, VkPipeline> variants;     // by target formats / samples
            bool sharedVariants = false;                            // variants from the device's shared cache (dynamicDepth)
        };

        // A frame's upload memory: host-visible chunks, reset when the frame slot comes round again
        struct Ring
        {
            struct Chunk
            {
                VkBuffer buffer = VK_NULL_HANDLE;
                VmaAllocation allocation = VK_NULL_HANDLE;
                uint8_t *data = nullptr;
                VkDeviceSize size = 0, used = 0;
            };
            std::vector<Chunk> chunks;
            size_t current = 0;
        };

        struct Slice
        {
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceSize offset = 0;
            uint8_t *data = nullptr;
        };

        struct FrameSlot
        {
            VkCommandPool pool = VK_NULL_HANDLE;
            std::vector<VkCommandBuffer> buffers;
            size_t used = 0;
            VkFence fence = VK_NULL_HANDLE;
            VkSemaphore acquired = VK_NULL_HANDLE;
            bool submitted = false;
            Ring ring;
        };

        VKAPI_ATTR VkBool32 VKAPI_CALL debugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                                    const VkDebugUtilsMessengerCallbackDataEXT *data, void *)
        {
            static uint32_t count = 0;
            if (++count <= 500u)
                std::fprintf(stderr, "[rhi] vulkan %s: %s\n",
                             (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "error" : "warning",
                             data && data->pMessage ? data->pMessage : "");
            return VK_FALSE;
        }

        // The cards as rhi::listGpus lists them: discrete first, then integrated, virtual, others (enumeration order
        // within a kind), software renderers (llvmpipe) and cards older than Vulkan 1.3 left out. `index`: each one's
        // place in vkEnumeratePhysicalDevices. pickDevice numbers its cards the same way, so the ids agree.
        std::vector<Gpu> orderGpus(const std::vector<VkPhysicalDevice> &devices, std::vector<uint32_t> &index)
        {
            auto kind = [](VkPhysicalDeviceType t) {
                return t == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 0 : t == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1
                                                                     : t == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU    ? 2
                                                                                                                   : 3;
            };
            std::vector<std::pair<int, uint32_t>> order; // kind, index
            std::vector<VkPhysicalDeviceProperties> props(devices.size());
            for (uint32_t i = 0; i < devices.size(); ++i)
            {
                vkGetPhysicalDeviceProperties(devices[i], &props[i]);
                if (props[i].deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU && props[i].apiVersion >= VK_API_VERSION_1_3)
                    order.push_back({kind(props[i].deviceType), i});
            }
            std::stable_sort(order.begin(), order.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
            std::vector<Gpu> gpus;
            index.clear();
            for (const auto &o : order)
            {
                const VkPhysicalDeviceProperties &p = props[o.second];
                Gpu g;
                g.name = p.deviceName;
                g.vendorId = p.vendorID;
                g.deviceId = p.deviceID;
                g.integrated = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
                gpus.push_back(g);
                index.push_back(o.second);
            }
            numberGpus(gpus);
            return gpus;
        }

        class VulkanDevice final : public Device
        {
        public:
            VulkanDevice() : m_core(std::make_shared<Core>()) {}

            bool create(const DeviceDesc &desc, std::string &error)
            {
                if (volkInitialize() != VK_SUCCESS)
                {
                    error = "no Vulkan loader (libvulkan.so.1 / vulkan-1.dll): install the GPU's Vulkan driver";
                    return false;
                }
                if (volkGetInstanceVersion() < VK_API_VERSION_1_3)
                {
                    error = "the Vulkan loader is older than 1.3";
                    return false;
                }
                m_offscreen = desc.offscreen || !desc.vulkan.createSurface;
                m_core->owner.store(std::this_thread::get_id(), std::memory_order_relaxed); // beginFrame keeps it current
                if (!createInstance(desc, error) || !pickDevice(desc, error) || !createDevice(error))
                    return false;
                m_core->alive = true;
                m_core->unbind = [this](const void *object) { unbind(object); };
                if (!createLayout(error) || !createFrames(error) || !createDummies(error))
                    return false;
                m_width = std::max(1u, desc.width);
                m_height = std::max(1u, desc.height);
                beginFrame();
                if (!m_offscreen && !createSwapchain(error))
                    return false;
                if (m_offscreen && !makeOffscreenBack())
                {
                    error = "off-screen back buffer";
                    return false;
                }
                return true;
            }

            ~VulkanDevice() override
            {
                if (!m_core->device)
                    return;
                if (m_recording)
                {
                    endPassInternal();
                    submit(false, VK_NULL_HANDLE);
                }
                vkDeviceWaitIdle(m_core->device);
                for (uint32_t i = 0; i < kFrames; ++i)
                    m_core->collect(i);
                for (auto &f : m_core->takeOrphans())
                    f();
                {
                    std::lock_guard<std::mutex> lock(m_stagedMutex); // made, never recorded: only their staging memory
                    for (Staged &st : m_staged)
                        vmaDestroyBuffer(m_core->allocator, st.buffer, st.allocation);
                    m_staged.clear();
                }
                m_dummyTexture.reset();
                m_offscreenBack.reset();
                m_defaultSampler.reset();
                m_zeroBuffer.reset();
                m_core->alive = false;
                m_core->unbind = nullptr;
                for (uint32_t i = 0; i < kFrames; ++i)
                    m_core->collect(i);
                VkDevice d = m_core->device;
                for (FrameSlot &f : m_frames)
                {
                    for (Ring::Chunk &c : f.ring.chunks)
                        vmaDestroyBuffer(m_core->allocator, c.buffer, c.allocation);
                    if (f.pool)
                        vkDestroyCommandPool(d, f.pool, nullptr);
                    if (f.fence)
                        vkDestroyFence(d, f.fence, nullptr);
                    if (f.acquired)
                        vkDestroySemaphore(d, f.acquired, nullptr);
                }
                if (m_waitFence)
                    vkDestroyFence(d, m_waitFence, nullptr);
                for (auto &slot : m_timer.slots)
                    if (slot.pool)
                        vkDestroyQueryPool(d, slot.pool, nullptr);
                destroySwapchain();
                if (m_surface)
                    vkDestroySurfaceKHR(m_core->instance, m_surface, nullptr);
                for (auto &[code, module] : m_modules)
                    vkDestroyShaderModule(d, module, nullptr);
                if (m_pipelineLayout)
                    vkDestroyPipelineLayout(d, m_pipelineLayout, nullptr);
                if (m_setLayout)
                    vkDestroyDescriptorSetLayout(d, m_setLayout, nullptr);
                for (auto &[key, pipeline] : m_sharedPipelines)
                    if (pipeline)
                        vkDestroyPipeline(d, pipeline, nullptr);
                if (m_pipelineCache)
                    vkDestroyPipelineCache(d, m_pipelineCache, nullptr);
            }

            Backend backend() const override { return Backend::Vulkan; }
            const std::string &adapterName() const override { return m_adapterName; }

            // ---- resources ----------------------------------------------------------------------------------------
            // The image and its views, no contents yet (layout undefined). Thread-safe: only device calls and VMA.
            std::unique_ptr<VkTexture> makeImage(const TextureDesc &desc)
            {
                TextureDesc sized = desc;
                sized.width = std::max(1u, desc.width);
                sized.height = std::max(1u, desc.height);
                auto t = std::make_unique<VkTexture>(m_core, sized);
                const uint32_t w = sized.width, h = sized.height;
                const bool msaa = desc.samples > 1u;
                t->samples = msaa ? static_cast<VkSampleCountFlagBits>(desc.samples) : VK_SAMPLE_COUNT_1_BIT;
                t->mips = 1;
                if (desc.mipLevels != 1u && !msaa)
                {
                    uint32_t full = 1;
                    for (uint32_t s = std::max(w, h); s > 1u; s >>= 1)
                        ++full;
                    t->mips = desc.mipLevels == 0 ? full : std::min(desc.mipLevels, full);
                }
                t->format = vkFormat(desc.format);
                const bool depth = desc.format == Format::D32F;
                t->aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
                VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                const bool sampled = (desc.usage & UsageSampled) || t->mips > 1;
                if (sampled)
                    usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
                if (desc.usage & UsageRenderTarget)
                    usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
                if (desc.usage & UsageDepth)
                    usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
                VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
                ci.imageType = VK_IMAGE_TYPE_2D;
                ci.format = t->format;
                ci.extent = {w, h, 1};
                ci.mipLevels = t->mips;
                ci.arrayLayers = 1;
                ci.samples = t->samples;
                ci.tiling = VK_IMAGE_TILING_OPTIMAL;
                ci.usage = usage;
                ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                VmaAllocationCreateInfo ai{};
                ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
                if ((desc.usage & (UsageRenderTarget | UsageDepth)) && w * h >= 1024u * 1024u)
                    ai.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT; // large targets: their own memory
                const VkResult r = vmaCreateImage(m_core->allocator, &ci, &ai, &t->image, &t->allocation, nullptr);
                if (r != VK_SUCCESS)
                {
                    std::fprintf(stderr, "[rhi] vkCreateImage %ux%u failed (%s)\n", w, h, vkResultName(r));
                    return nullptr;
                }
                nameObject(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(t->image), desc.name);
                static const bool logCreates = std::getenv("PS2X_RHI_LOG_CREATES") != nullptr;
                if (logCreates && w * h >= 256u * 256u)
                {
                    static std::atomic<uint64_t> created{0}; // helper threads make textures too
                    if (++created <= 2000u)
                        std::fprintf(stderr, "[rhi] create %ux%u x%u '%s'\n", w, h, desc.samples, desc.name ? desc.name : "");
                }
                VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                vi.image = t->image;
                vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
                vi.format = t->format;
                vi.subresourceRange = {t->aspect, 0, t->mips, 0, 1};
                if (vkCreateImageView(m_core->device, &vi, nullptr, &t->sampledView) != VK_SUCCESS)
                    return nullptr;
                t->attachmentView = t->sampledView;
                if (t->mips > 1)
                {
                    vi.subresourceRange.levelCount = 1;
                    if (vkCreateImageView(m_core->device, &vi, nullptr, &t->attachmentView) != VK_SUCCESS)
                        return nullptr;
                }
                t->resting = sampled                             ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                             : (desc.usage & UsageRenderTarget) ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                             : (desc.usage & UsageDepth)        ? VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL
                                                                : VK_IMAGE_LAYOUT_GENERAL;
                return t;
            }

            bool freeThreadedCreation() const override { return true; } // off the owner thread: stage() (PERFORMANCE.md 25)
            bool blockCompression() const override { return m_blockCompression; }

            std::unique_ptr<Texture> createTexture(const TextureDesc &desc, const void *data, uint32_t rowPitch) override
            {
                auto t = makeImage(desc);
                if (!t)
                    return nullptr;
                const uint32_t w = t->desc().width, h = t->desc().height;
                const bool depth = desc.format == Format::D32F;
                if (!m_core->onOwner()) // a helper thread (freeThreadedCreation): one level with data, sampled, no target
                {
                    const void *levels[1] = {data};
                    const uint32_t pitches[1] = {rowPitch ? rowPitch : w * 4u};
                    if (!data || t->mips > 1u || (desc.usage & (UsageRenderTarget | UsageDepth)) || !stage(*t, levels, pitches, 1u))
                    {
                        std::fprintf(stderr, "[rhi] createTexture %ux%u off the render thread: not a sampled texture with data\n", w, h);
                        return nullptr;
                    }
                    return t;
                }

                // first contents, in the frame's set-up commands (before anything recorded so far can use it): the data,
                // or zeros for targets
                VkCommandBuffer cmd = initCmd();
                barrier(cmd, *t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                if (data)
                {
                    const uint32_t pitch = rowPitch ? rowPitch : w * 4u;
                    uploadRegion(cmd, *t, 0, 0, w, h, data, pitch);
                    if (t->mips > 1)
                        mipChain(cmd, *t);
                }
                else if (desc.usage & (UsageRenderTarget | UsageDepth))
                {
                    const VkImageSubresourceRange range{t->aspect, 0, t->mips, 0, 1};
                    if (depth)
                    {
                        const VkClearDepthStencilValue zero{0.0f, 0};
                        vkCmdClearDepthStencilImage(cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
                    }
                    else
                    {
                        const VkClearColorValue zero{};
                        vkCmdClearColorImage(cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
                    }
                }
                barrier(cmd, *t, t->resting);
                return t;
            }

            void updateTexture(Texture &texture, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const void *data,
                               uint32_t rowPitch) override
            {
                auto &t = static_cast<VkTexture &>(texture);
                if (!width || !height || !data)
                    return;
                outsideRendering();
                barrier(m_cmd, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                uploadRegion(m_cmd, t, x, y, width, height, data, rowPitch ? rowPitch : width * 4u);
                barrier(m_cmd, t, homeLayout(t));
            }

            bool readTexture(Texture &texture, uint32_t x, uint32_t y, uint32_t width, uint32_t height, void *out,
                             uint32_t rowPitch) override
            {
                auto &t = static_cast<VkTexture &>(texture);
                if (!out || !width || !height || t.samples != VK_SAMPLE_COUNT_1_BIT)
                    return false;
                const VkDeviceSize size = static_cast<VkDeviceSize>(width) * height * 4u;
                VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                bi.size = size;
                bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                VmaAllocationCreateInfo ai{};
                ai.usage = VMA_MEMORY_USAGE_AUTO;
                ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
                VkBuffer staging = VK_NULL_HANDLE;
                VmaAllocation allocation = VK_NULL_HANDLE;
                VmaAllocationInfo info{};
                if (vmaCreateBuffer(m_core->allocator, &bi, &ai, &staging, &allocation, &info) != VK_SUCCESS)
                    return false;
                outsideRendering();
                barrier(m_cmd, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                VkBufferImageCopy copy{};
                copy.imageSubresource = {t.aspect, 0, 0, 1};
                copy.imageOffset = {static_cast<int32_t>(x), static_cast<int32_t>(y), 0};
                copy.imageExtent = {width, height, 1};
                vkCmdCopyImageToBuffer(m_cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &copy);
                barrier(m_cmd, t, homeLayout(t));
                memoryBarrier(m_cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                              VK_ACCESS_2_HOST_READ_BIT);
                flushAndWait();
                vmaInvalidateAllocation(m_core->allocator, allocation, 0, VK_WHOLE_SIZE);
                const uint32_t pitch = rowPitch ? rowPitch : width * 4u;
                for (uint32_t row = 0; row < height; ++row)
                    std::memcpy(static_cast<uint8_t *>(out) + static_cast<size_t>(row) * pitch,
                                static_cast<const uint8_t *>(info.pMappedData) + static_cast<size_t>(row) * width * 4u, width * 4u);
                vmaDestroyBuffer(m_core->allocator, staging, allocation);
                return true;
            }

            void copyTexture(Texture &dst, uint32_t dstX, uint32_t dstY, Texture &src, uint32_t srcX, uint32_t srcY, uint32_t width,
                             uint32_t height) override
            {
                auto &d = static_cast<VkTexture &>(dst);
                auto &s = static_cast<VkTexture &>(src);
                if (!width || !height || &d == &s)
                    return;
                outsideRendering();
                barrier(m_cmd, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                barrier(m_cmd, d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkImageCopy copy{};
                copy.srcSubresource = {s.aspect, 0, 0, 1};
                copy.srcOffset = {static_cast<int32_t>(srcX), static_cast<int32_t>(srcY), 0};
                copy.dstSubresource = {d.aspect, 0, 0, 1};
                copy.dstOffset = {static_cast<int32_t>(dstX), static_cast<int32_t>(dstY), 0};
                copy.extent = {width, height, 1};
                vkCmdCopyImage(m_cmd, s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &copy);
                barrier(m_cmd, s, homeLayout(s));
                barrier(m_cmd, d, homeLayout(d));
            }

            void resolveTexture(Texture &dst, Texture &src) override
            {
                auto &d = static_cast<VkTexture &>(dst);
                auto &s = static_cast<VkTexture &>(src);
                if (s.aspect != VK_IMAGE_ASPECT_COLOR_BIT || s.samples == VK_SAMPLE_COUNT_1_BIT)
                    return;
                outsideRendering();
                barrier(m_cmd, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                barrier(m_cmd, d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkImageResolve r{};
                r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                r.extent = {std::min(s.desc().width, d.desc().width), std::min(s.desc().height, d.desc().height), 1};
                vkCmdResolveImage(m_cmd, s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                  &r);
                barrier(m_cmd, s, homeLayout(s));
                barrier(m_cmd, d, homeLayout(d));
            }

            void generateMips(Texture &texture) override
            {
                auto &t = static_cast<VkTexture &>(texture);
                if (t.mips <= 1)
                    return;
                outsideRendering();
                barrier(m_cmd, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                mipChain(m_cmd, t);
                barrier(m_cmd, t, homeLayout(t));
            }

            std::unique_ptr<Texture> createTextureLevels(const TextureDesc &desc, const void *const *levels, uint32_t levelCount) override
            {
                // the image with its chain (created empty, then every level from the data in the set-up commands)
                TextureDesc d = desc;
                d.usage = UsageSampled;
                d.mipLevels = std::max(1u, levelCount);
                if (!m_core->onOwner()) // a helper thread (freeThreadedCreation): every level staged
                {
                    auto t = makeImage(d);
                    if (!t)
                        return nullptr;
                    uint32_t pitches[16];
                    const uint32_t n = std::min({t->mips, levelCount, 16u});
                    for (uint32_t i = 0; i < n; ++i)
                        pitches[i] = rowBytes(d.format, std::max(1u, t->desc().width >> i));
                    if (!stage(*t, levels, pitches, n))
                        return nullptr;
                    return t;
                }
                auto base = createTexture(d, nullptr, 0);
                if (!base)
                    return nullptr;
                auto &t = static_cast<VkTexture &>(*base);
                VkCommandBuffer cmd = initCmd();
                barrier(cmd, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                for (uint32_t i = 0; i < t.mips && i < levelCount; ++i)
                {
                    const uint32_t w = std::max(1u, t.desc().width >> i), h = std::max(1u, t.desc().height >> i);
                    uploadRegion(cmd, t, 0, 0, w, h, levels[i], rowBytes(d.format, w), i);
                }
                barrier(cmd, t, t.resting);
                return base;
            }

            std::unique_ptr<Buffer> createBuffer(const BufferDesc &desc, const void *data) override
            {
                auto b = std::make_unique<VkBuf>(m_core, desc);
                const size_t size = std::max<size_t>(desc.size, 4);
                if (desc.dynamic)
                {
                    b->shadow.assign(size, 0);
                    b->used = size;
                    if (data)
                        std::memcpy(b->shadow.data(), data, desc.size);
                    return b;
                }
                VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                bi.size = (size + 15u) & ~size_t(15);
                bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | (desc.usage == BufferUsage::Vertex  ? VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                                                               : desc.usage == BufferUsage::Index ? VK_BUFFER_USAGE_INDEX_BUFFER_BIT
                                                                                                  : VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
                VmaAllocationCreateInfo ai{};
                ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
                if (vmaCreateBuffer(m_core->allocator, &bi, &ai, &b->buffer, &b->allocation, nullptr) != VK_SUCCESS)
                {
                    std::fprintf(stderr, "[rhi] vkCreateBuffer %zu bytes failed\n", desc.size);
                    return nullptr;
                }
                nameObject(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(b->buffer), desc.name);
                // zeros or the data first, in the set-up commands (nothing recorded so far uses it)
                VkCommandBuffer cmd = initCmd();
                if (data)
                    copyToBuffer(cmd, *b, data, desc.size);
                else
                    vkCmdFillBuffer(cmd, b->buffer, 0, VK_WHOLE_SIZE, 0);
                memoryBarrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, kBufferReaders, kBufferReads);
                return b;
            }

            void updateBuffer(Buffer &buffer, const void *data, size_t size) override
            {
                auto &b = static_cast<VkBuf &>(buffer);
                size = std::min(size, b.desc().size);
                if (!data || !size)
                    return;
                if (b.desc().dynamic)
                {
                    // WRITE_DISCARD: the rest of the buffer is undefined in D3D; ours keeps the old bytes. A vertex / index
                    // buffer's slice holds only what was written: the renderer's are sized for the largest frame seen and
                    // each frame copied all of it (~1 ms a frame on the laptop, PERFORMANCE.md 27; PS2X_RHI_FULL_COPY=1); a constant buffer's
                    // descriptor covers the whole buffer, so it is copied whole
                    std::memcpy(b.shadow.data(), data, size);
                    b.used = (b.desc().usage == BufferUsage::Constant || fullCopy()) ? b.shadow.size() : std::max<size_t>(size, 4);
                    b.sliceFrame = 0; // a new slice at the next use
                    if (&b == m_vertexBuffer || &b == m_indexBuffer)
                        m_buffersDirty = true;
                    for (VkBuf *u : m_uniforms)
                        if (u == &b)
                            m_descriptorsDirty = true;
                    return;
                }
                outsideRendering();
                memoryBarrier(m_cmd, kBufferReaders, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                copyToBuffer(m_cmd, b, data, size);
                memoryBarrier(m_cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, kBufferReaders, kBufferReads);
            }

            std::unique_ptr<Sampler> createSampler(const SamplerDesc &desc) override
            {
                auto s = std::make_unique<VkSamp>(m_core);
                VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
                const bool linear = desc.filter == Filter::Linear;
                si.magFilter = si.minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
                si.mipmapMode = linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
                si.addressModeU = desc.wrapU == Wrap::Repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                si.addressModeV = desc.wrapV == Wrap::Repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                if (linear && desc.anisotropy > 1u && m_anisotropy)
                {
                    si.anisotropyEnable = VK_TRUE;
                    si.maxAnisotropy = std::min(static_cast<float>(std::min(desc.anisotropy, 16u)), m_maxAnisotropy);
                }
                si.maxLod = VK_LOD_CLAMP_NONE;
                if (vkCreateSampler(m_core->device, &si, nullptr, &s->sampler) != VK_SUCCESS)
                    return nullptr;
                return s;
            }

            std::unique_ptr<Pipeline> createPipeline(const PipelineDesc &desc) override
            {
                auto p = std::make_unique<VkPipe>(m_core);
                p->desc = desc;
                p->name = desc.name ? desc.name : "";
                p->desc.name = nullptr;
                p->vs = shaderModule(desc.vertex);
                p->ps = shaderModule(desc.pixel);
                if (!p->vs || !p->ps)
                {
                    std::fprintf(stderr, "[rhi] shaders of pipeline '%s' rejected\n", p->name.c_str());
                    return nullptr;
                }
                return p;
            }

            // ---- drawing ------------------------------------------------------------------------------------------
            void beginPass(const PassDesc &pass) override
            {
                if (m_inPass)
                    endPassInternal();
                m_pass = pass;
                m_passName = pass.name ? pass.name : "";
                m_pass.name = nullptr;
                m_passColor = nullptr;
                m_passDepth = pass.depth ? static_cast<VkTexture *>(pass.depth) : nullptr;
                uint32_t w = m_width, h = m_height;
                if (pass.depthOnly)
                {
                    if (m_passDepth)
                    {
                        w = m_passDepth->desc().width;
                        h = m_passDepth->desc().height;
                    }
                }
                else if (pass.color)
                {
                    m_passColor = static_cast<VkTexture *>(pass.color);
                    w = m_passColor->desc().width;
                    h = m_passColor->desc().height;
                }
                else if (!acquireBackbuffer())
                    m_passColor = m_offscreenBack.get(); // drawn, not shown
                else if (m_offscreen)
                    m_passColor = m_offscreenBack.get();
                m_passWidth = w;
                m_passHeight = h;
                m_passToBackbuffer = !pass.depthOnly && !pass.color && !m_offscreen && m_imageIndex != kNoImage;
                m_inPass = true;
                m_passStarted = false;
                // targets into their attachment layouts (outside the rendering, which starts at the first draw)
                if (m_passColor)
                    barrier(m_cmd, *m_passColor, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                if (m_passToBackbuffer)
                    swapBarrier(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                if (m_passDepth)
                    barrier(m_cmd, *m_passDepth, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
                // as D3D11's beginPass: textures unbound, viewport and scissor the whole target
                for (uint32_t i = 0; i < kTextureSlots; ++i)
                {
                    m_textures[i] = nullptr;
                    m_samplers[i] = nullptr;
                }
                m_descriptorsDirty = true;
                setViewport(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
                setScissor(0, 0, static_cast<int>(w), static_cast<int>(h));
                if (!m_passName.empty())
                    pushMarker(m_passName.c_str());
                m_passMarker = !m_passName.empty();
            }

            void endPass() override { endPassInternal(); }

            void setViewport(float x, float y, float width, float height) override
            {
                // D3D's Y-up NDC: the viewport upside down
                m_viewport = {x, y + height, width, -height, 0.0f, 1.0f};
                m_viewportDirty = true;
            }

            void setScissor(int x, int y, int width, int height) override
            {
                int x0 = std::max(x, 0), y0 = std::max(y, 0);
                int x1 = std::max(x + width, x0), y1 = std::max(y + height, y0);
                m_scissor = {{x0, y0}, {static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)}};
                m_scissorDirty = true;
            }

            void setBlendConstant(const float rgba[4]) override
            {
                std::memcpy(m_blendConstant, rgba, sizeof(m_blendConstant));
                m_blendDirty = true;
            }

            void setPipeline(Pipeline &pipeline) override
            {
                auto *p = static_cast<VkPipe *>(&pipeline);
                if (p != m_pipeline)
                {
                    m_pipeline = p;
                    m_pipelineDirty = true;
                }
            }

            void setVertexBuffer(Buffer &buffer, uint32_t offset) override
            {
                m_vertexBuffer = static_cast<VkBuf *>(&buffer);
                m_vertexOffset = offset;
                m_buffersDirty = true;
            }

            void setIndexBuffer(Buffer &buffer, bool uint32Indices) override
            {
                m_indexBuffer = static_cast<VkBuf *>(&buffer);
                m_index32 = uint32Indices;
                m_buffersDirty = true;
            }

            void setTexture(uint32_t slot, Texture *texture, Sampler *sampler) override
            {
                if (slot >= kTextureSlots)
                    return;
                m_textures[slot] = static_cast<VkTexture *>(texture);
                m_samplers[slot] = static_cast<VkSamp *>(sampler);
                m_descriptorsDirty = true;
            }

            void setConstants(uint32_t slot, Buffer &buffer) override
            {
                if (slot >= kUniformSlots)
                    return;
                m_uniforms[slot] = static_cast<VkBuf *>(&buffer);
                m_descriptorsDirty = true;
            }

            void draw(uint32_t vertexCount, uint32_t firstVertex) override
            {
                const int64_t t0 = spikeClock();
                if (!vertexCount || !prepareDraw(false))
                    return;
                const int64_t t1 = spikeClock();
                vkCmdDraw(m_cmd, vertexCount, 1, firstVertex, 0);
                ++m_stats.draws;
                if (t0)
                {
                    const int64_t t2 = spikeClock();
                    m_stats.prepareNs += t1 - t0;
                    m_stats.drawNs += t2 - t1;
                }
            }

            void drawIndexed(uint32_t indexCount, uint32_t firstIndex, int32_t baseVertex) override
            {
                if (!indexCount || !prepareDraw(true))
                    return;
                vkCmdDrawIndexed(m_cmd, indexCount, 1, firstIndex, baseVertex, 0);
                ++m_stats.draws;
            }

            void pushMarker(const char *name) override
            {
                if (m_debugUtils && name)
                {
                    VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
                    label.pLabelName = name;
                    vkCmdBeginDebugUtilsLabelEXT(m_cmd, &label);
                    ++m_markerDepth;
                }
                if (m_timer.markers)
                    m_timer.open.push_back({name ? name : "?", timestamp()});
            }

            void popMarker() override
            {
                if (m_debugUtils && m_markerDepth > 0)
                {
                    vkCmdEndDebugUtilsLabelEXT(m_cmd);
                    --m_markerDepth;
                }
                if (m_timer.markers && !m_timer.open.empty())
                {
                    auto [name, begin] = std::move(m_timer.open.back());
                    m_timer.open.pop_back();
                    const uint32_t end = timestamp();
                    if (begin != kNoQuery && end != kNoQuery)
                        m_timer.slots[m_core->frame].spans.push_back({std::move(name), begin, end});
                }
            }

            // ---- swapchain ----------------------------------------------------------------------------------------
            void backbufferSize(uint32_t &width, uint32_t &height) const override
            {
                width = m_width;
                height = m_height;
            }

            void resize(uint32_t width, uint32_t height) override
            {
                if (!width || !height || (width == m_width && height == m_height))
                    return;
                m_width = width;
                m_height = height;
                if (m_offscreen)
                    makeOffscreenBack();
                else
                    m_swapchainDirty = true; // made again at the next frame's first use
            }

            void present(PresentMode mode) override
            {
                if (m_inPass)
                    endPassInternal();
                const bool show = m_imageIndex != kNoImage;
                if (show)
                    swapBarrier(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
                if (m_timer.on)
                {
                    auto &slot = m_timer.slots[m_core->frame];
                    const uint32_t end = timestamp();
                    if (m_timer.markers && m_timer.frameBegin != kNoQuery && end != kNoQuery)
                        slot.spans.push_back({"(frame)", m_timer.frameBegin, end});
                    m_timer.open.clear(); // spans left open don't cross frames
                    // the shown frame's input delay ends when the GPU is done with it (collectTimes)
                    slot.frameEnd = end;
                    slot.inputNs = m_pendingInputNs;
                }
                VkSemaphore done = show ? m_renderDone[m_imageIndex] : VK_NULL_HANDLE;
                submit(true, done);
                if (show)
                {
                    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
                    pi.waitSemaphoreCount = 1;
                    pi.pWaitSemaphores = &done;
                    pi.swapchainCount = 1;
                    pi.pSwapchains = &m_swapchain;
                    pi.pImageIndices = &m_imageIndex;
                    const auto t0 = std::chrono::steady_clock::now();
                    const VkResult r = vkQueuePresentKHR(m_queue, &pi);
                    m_stats.presentMs += msSince(t0);
                    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
                        m_swapchainDirty = true;
                    else if (r != VK_SUCCESS)
                        reportLost(r, "vkQueuePresentKHR");
                    m_imageIndex = kNoImage;
                }
                if (mode != m_presentMode)
                {
                    m_presentMode = mode;
                    m_swapchainDirty = !m_offscreen;
                }
                // PS2X_HOST_SPIKES=<ms>: a frame whose span (the end of the last one to this present) or waits went over
                // it, with its own counts (the runtime's [host-spike] line gives the main loop's phases)
                static const double spikeMs = std::getenv("PS2X_HOST_SPIKES") ? std::atof(std::getenv("PS2X_HOST_SPIKES")) : 0.0;
                const auto spikeNow = std::chrono::steady_clock::now();
                const Stats &n = m_stats, &o = m_spikePrev;
                const double span = std::chrono::duration<double, std::milli>(spikeNow - m_spikeFrameEnd).count();
                const double waited = (n.acquireMs - o.acquireMs) + (n.submitMs - o.submitMs) + (n.presentMs - o.presentMs) +
                                      (n.pipelineMs - o.pipelineMs);
                char spikeLine[512] = "";
                if (spikeMs > 0.0 && m_spikeFrameEnd.time_since_epoch().count() && (span > spikeMs || waited > spikeMs * 0.5))
                    std::snprintf(spikeLine, sizeof(spikeLine),
                                  "[rhi-spike] t=%.3f span %.1f ms: acquire %.1f, submit %.1f, present %.1f, new pipelines %llu (%.1f ms); "
                                  "%llu draws (prepare %.1f ms, vkCmdDraw %.1f), %llu pipeline binds (%.1f ms), %llu pushes (%.1f ms), "
                                  "%llu passes, %llu suspended, %llu barriers, %llu waits, %llu descriptor writes, %.0f KB up",
                                  std::chrono::duration<double>(spikeNow.time_since_epoch()).count(), span, n.acquireMs - o.acquireMs,
                                  n.submitMs - o.submitMs, n.presentMs - o.presentMs,
                                  static_cast<unsigned long long>(n.pipelines - o.pipelines), n.pipelineMs - o.pipelineMs,
                                  static_cast<unsigned long long>(n.draws - o.draws), (n.prepareNs - o.prepareNs) / 1e6,
                                  (n.drawNs - o.drawNs) / 1e6, static_cast<unsigned long long>(n.pipelineBinds - o.pipelineBinds),
                                  (n.bindNs - o.bindNs) / 1e6, static_cast<unsigned long long>(n.pushes - o.pushes), (n.pushNs - o.pushNs) / 1e6,
                                  static_cast<unsigned long long>(n.passes - o.passes),
                                  static_cast<unsigned long long>(n.suspends - o.suspends),
                                  static_cast<unsigned long long>(n.barriers - o.barriers), static_cast<unsigned long long>(n.waits - o.waits),
                                  static_cast<unsigned long long>(n.descriptorWrites - o.descriptorWrites), (n.uploaded - o.uploaded) / 1024.0);
                if (statsOn())
                    reportStats();
                const double fence0 = m_stats.fenceMs;
                beginFrame();
                const double fence = m_stats.fenceMs - fence0;
                if (spikeLine[0] || (spikeMs > 0.0 && fence > spikeMs * 0.5))
                    std::fprintf(stderr, "%s%sfence after it %.1f ms\n", spikeLine[0] ? spikeLine : "[rhi-spike]", spikeLine[0] ? "; " : " ",
                                 fence);
                m_spikePrev = m_stats;
                m_spikePrev.inputDelays.clear();
                m_spikeFrameEnd = std::chrono::steady_clock::now();
            }

            void setFrameInputTime(int64_t steadyNs) override { m_pendingInputNs = steadyNs; }

            // ---- Dear ImGui: drawn through the RHI by render::UiRenderer (this backend has no renderer of its own) ----
            bool uiInit() override { return false; }
            void uiNewFrame() override {}
            void uiRender(void *) override {}
            void uiShutdown() override {}

        private:
            static constexpr uint32_t kNoImage = ~0u;

            // ---- set-up ---------------------------------------------------------------------------------------------
            bool createInstance(const DeviceDesc &desc, std::string &error)
            {
                std::vector<const char *> extensions;
                if (!m_offscreen && desc.vulkan.instanceExtensions)
                {
                    uint32_t n = 0;
                    const char *const *names = desc.vulkan.instanceExtensions(&n);
                    if (!names)
                    {
                        std::fprintf(stderr, "[rhi] no window surface extensions: drawing off-screen\n");
                        m_offscreen = true;
                    }
                    for (uint32_t i = 0; names && i < n; ++i)
                        extensions.push_back(names[i]);
                }
                uint32_t count = 0;
                vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
                std::vector<VkExtensionProperties> available(count);
                vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());
                for (const VkExtensionProperties &e : available)
                    if (std::strcmp(e.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0)
                        m_debugUtils = true;
                if (m_debugUtils)
                    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                std::vector<const char *> layers;
                if (desc.debug)
                {
                    vkEnumerateInstanceLayerProperties(&count, nullptr);
                    std::vector<VkLayerProperties> all(count);
                    vkEnumerateInstanceLayerProperties(&count, all.data());
                    for (const VkLayerProperties &l : all)
                        if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0)
                            layers.push_back("VK_LAYER_KHRONOS_validation");
                    if (layers.empty())
                        std::fprintf(stderr, "[rhi] Vulkan validation layer not installed (vulkan-validationlayers)\n");
                }
                VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
                app.pApplicationName = "Vice City Stories";
                app.pEngineName = "vcs-recomp";
                app.apiVersion = VK_API_VERSION_1_3;
                VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
                ci.pApplicationInfo = &app;
                ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
                ci.ppEnabledExtensionNames = extensions.data();
                ci.enabledLayerCount = static_cast<uint32_t>(layers.size());
                ci.ppEnabledLayerNames = layers.data();
                VkResult r = vkCreateInstance(&ci, nullptr, &m_core->instance);
                if (r != VK_SUCCESS)
                {
                    error = std::string("vkCreateInstance failed (") + vkResultName(r) + ")";
                    return false;
                }
                volkLoadInstanceOnly(m_core->instance);
                if (m_debugUtils && !layers.empty())
                {
                    VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
                    mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
                    mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
                    mi.pfnUserCallback = debugMessage;
                    vkCreateDebugUtilsMessengerEXT(m_core->instance, &mi, nullptr, &m_core->messenger);
                    std::fprintf(stderr, "[rhi] Vulkan validation layer on\n");
                }
                if (!m_offscreen)
                {
                    uint64_t surface = 0;
                    if (!desc.vulkan.createSurface(m_core->instance, &surface))
                    {
                        error = "no Vulkan surface for the window";
                        return false;
                    }
                    m_surface = reinterpret_cast<VkSurfaceKHR>(surface);
                }
                return true;
            }

            bool pickDevice(const DeviceDesc &desc, std::string &error)
            {
                uint32_t count = 0;
                vkEnumeratePhysicalDevices(m_core->instance, &count, nullptr);
                std::vector<VkPhysicalDevice> devices(count);
                vkEnumeratePhysicalDevices(m_core->instance, &count, devices.data());
                // VCS_VK_DEVICE=<number or part of the name>: that one; else the card chosen in Graphics > Graphics card
                // (desc.gpu, an rhi::Gpu id); else the best kind (discrete > integrated > ...)
                const char *wanted = std::getenv("VCS_VK_DEVICE");
                std::vector<uint32_t> listed;
                const std::vector<Gpu> gpus = orderGpus(devices, listed);
                std::vector<int> ids(count, 0);
                for (size_t k = 0; k < gpus.size(); ++k)
                    ids[listed[k]] = gpus[k].id;
                int best = -1, bestScore = -1;
                std::string why;
                std::vector<std::string> lacks(count); // what a device that cannot be used is missing (for the log)
                std::string others; // devices the setting's list leaves out: software renderers, Vulkan older than 1.3
                for (uint32_t i = 0; i < count; ++i)
                {
                    VkPhysicalDeviceProperties props{};
                    vkGetPhysicalDeviceProperties(devices[i], &props);
                    uint32_t family = 0;
                    std::string missing;
                    const bool ok = usable(devices[i], props, family, missing);
                    if (!ids[i])
                        others += (others.empty() ? "" : ", ") + std::string(props.deviceName) +
                                  (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? " (software)" : "") +
                                  (ok ? "" : " (not usable: needs " + missing + ")");
                    if (!ok)
                    {
                        why += std::string("; ") + props.deviceName + ": " + missing;
                        lacks[i] = missing;
                        continue;
                    }
                    int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 4
                                : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 3
                                : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU    ? 2
                                : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU            ? 0
                                                                                             : 1;
                    if (desc.gpu && ids[i] == desc.gpu)
                        score = 50;
                    if (wanted && *wanted &&
                        (std::to_string(i) == wanted || std::strstr(props.deviceName, wanted) != nullptr))
                        score = 100;
                    if (score > bestScore)
                    {
                        best = static_cast<int>(i);
                        bestScore = score;
                        m_queueFamily = family;
                    }
                }
                // The log's line, as D3D11's: the cards as Graphics > Graphics card lists them, with their ids (VCS_GPU) -
                // and here what a card that cannot be used lacks, then the devices that are not in the list.
                std::string names;
                for (size_t k = 0; k < gpus.size(); ++k)
                {
                    const std::string &lack = lacks[listed[k]];
                    names += (names.empty() ? "" : ", ") + gpus[k].name + (gpus[k].integrated ? " (integrated)" : "") + " [" +
                             std::to_string(gpus[k].id) + "]" + (lack.empty() ? "" : " (not usable: needs " + lack + ")");
                }
                std::string using_ = "none";
                if (best >= 0)
                {
                    VkPhysicalDeviceProperties props{};
                    vkGetPhysicalDeviceProperties(devices[best], &props);
                    using_ = std::string(props.deviceName) + (bestScore == 100 ? " (VCS_VK_DEVICE)" : "");
                }
                std::fprintf(stderr, "[rhi] graphics cards, fastest first: %s%s%s; using %s\n",
                             names.empty() ? "none" : names.c_str(), others.empty() ? "" : "; not in the list: ", others.c_str(),
                             using_.c_str());
                if (best < 0)
                {
                    error = "no GPU with Vulkan 1.3, dynamic rendering, push descriptors and dual-source blending" + why;
                    return false;
                }
                m_core->physical = devices[best];
                VkPhysicalDeviceProperties props{};
                vkGetPhysicalDeviceProperties(m_core->physical, &props);
                // Mesa's llvmpipe / lavapipe: what is left when no card's Vulkan driver can be used (not installed, or
                // the card's device not open to this user) - the game then draws on the processor, a few frames a second.
                // (Score 100: asked for by VCS_VK_DEVICE.)
                if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
                    std::fprintf(stderr, "[rhi] Vulkan: drawing with the software renderer %s - the game will be very slow%s\n",
                                 props.deviceName,
                                 bestScore == 100 ? "" : " (no graphics card can be used: is its Vulkan driver installed?)");
                m_adapterName = props.deviceName;
                m_maxAnisotropy = props.limits.maxSamplerAnisotropy;
                VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
                VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
                p2.pNext = &driver;
                vkGetPhysicalDeviceProperties2(m_core->physical, &p2);
                if (driver.driverName[0])
                    m_adapterName += std::string(" (") + driver.driverName + " " + driver.driverInfo + ")";
                return true;
            }

            bool usable(VkPhysicalDevice device, const VkPhysicalDeviceProperties &props, uint32_t &family, std::string &missing)
            {
                if (props.apiVersion < VK_API_VERSION_1_3)
                {
                    missing = "Vulkan 1.3";
                    return false;
                }
                uint32_t count = 0;
                vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
                std::vector<VkExtensionProperties> exts(count);
                vkEnumerateDeviceExtensionProperties(device, nullptr, &count, exts.data());
                auto has = [&](const char *name) {
                    return std::any_of(exts.begin(), exts.end(), [&](const VkExtensionProperties &e) { return std::strcmp(e.extensionName, name) == 0; });
                };
                if (!has(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME))
                {
                    missing = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
                    return false;
                }
                if (!m_offscreen && !has(VK_KHR_SWAPCHAIN_EXTENSION_NAME))
                {
                    missing = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
                    return false;
                }
                VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
                VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
                f2.pNext = &f13;
                vkGetPhysicalDeviceFeatures2(device, &f2);
                if (!f13.dynamicRendering || !f13.synchronization2 || !f13.shaderDemoteToHelperInvocation || !f2.features.dualSrcBlend ||
                    !f2.features.depthClamp || !f2.features.shaderClipDistance)
                {
                    missing = "a required feature (dynamic rendering, synchronization2, demote, dual-source blend, depth clamp, clip distance)";
                    return false;
                }
                vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
                std::vector<VkQueueFamilyProperties> families(count);
                vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
                for (uint32_t i = 0; i < count; ++i)
                {
                    if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
                        continue;
                    VkBool32 present = VK_TRUE;
                    if (!m_offscreen)
                        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_surface, &present);
                    if (present)
                    {
                        family = i;
                        return true;
                    }
                }
                missing = "a graphics queue that can show the window";
                return false;
            }

            bool createDevice(std::string &error)
            {
                VkPhysicalDeviceFeatures supported{};
                vkGetPhysicalDeviceFeatures(m_core->physical, &supported);
                m_anisotropy = supported.samplerAnisotropy == VK_TRUE;
                m_blockCompression = supported.textureCompressionBC == VK_TRUE; // texture packs' DDS (docs/TEXTURE_PACKS.md)
                const float priority = 1.0f;
                VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
                qi.queueFamilyIndex = m_queueFamily;
                qi.queueCount = 1;
                qi.pQueuePriorities = &priority;
                VkPhysicalDeviceVulkan12Features have12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
                VkPhysicalDeviceFeatures2 have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
                have.pNext = &have12;
                vkGetPhysicalDeviceFeatures2(m_core->physical, &have);
                VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
                f12.hostQueryReset = have12.hostQueryReset; // GPU timing (PS2X_GPU_TIMING)
                VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
                f13.pNext = &f12;
                f13.dynamicRendering = VK_TRUE;
                f13.synchronization2 = VK_TRUE;
                f13.shaderDemoteToHelperInvocation = VK_TRUE;
                VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
                f2.pNext = &f13;
                f2.features.dualSrcBlend = VK_TRUE;
                f2.features.depthClamp = VK_TRUE;
                f2.features.shaderClipDistance = VK_TRUE;
                f2.features.samplerAnisotropy = supported.samplerAnisotropy;
                f2.features.textureCompressionBC = supported.textureCompressionBC;
                std::vector<const char *> extensions{VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
                if (!m_offscreen)
                    extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
                // GPU time on the CPU's clock (the input delay, PS2X_RHI_STATS): CLOCK_MONOTONIC is steady_clock on Linux
                if (statsOn() && deviceHas(VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME))
                {
                    uint32_t n = 0;
                    vkGetPhysicalDeviceCalibrateableTimeDomainsKHR(m_core->physical, &n, nullptr);
                    std::vector<VkTimeDomainKHR> domains(n);
                    vkGetPhysicalDeviceCalibrateableTimeDomainsKHR(m_core->physical, &n, domains.data());
                    const bool dev = std::find(domains.begin(), domains.end(), VK_TIME_DOMAIN_DEVICE_KHR) != domains.end();
                    const bool mono = std::find(domains.begin(), domains.end(), VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR) != domains.end();
                    if (dev && mono)
                    {
                        extensions.push_back(VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
                        m_calibration = true;
                    }
                }
                VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
                ci.pNext = &f2;
                ci.queueCreateInfoCount = 1;
                ci.pQueueCreateInfos = &qi;
                ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
                ci.ppEnabledExtensionNames = extensions.data();
                VkResult r = vkCreateDevice(m_core->physical, &ci, nullptr, &m_core->device);
                if (r != VK_SUCCESS)
                {
                    error = std::string("vkCreateDevice failed (") + vkResultName(r) + ")";
                    return false;
                }
                volkLoadDevice(m_core->device);
                vkGetDeviceQueue(m_core->device, m_queueFamily, 0, &m_queue);
                timerInit(f12.hostQueryReset == VK_TRUE);

                VmaVulkanFunctions fns{};
                fns.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
                fns.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
                if (logMemory())
                {
                    g_realAllocateMemory = vkAllocateMemory;
                    g_realFreeMemory = vkFreeMemory;
                    fns.vkAllocateMemory = loggedAllocateMemory;
                    fns.vkFreeMemory = loggedFreeMemory;
                }
                VmaAllocatorCreateInfo ai{};
                ai.vulkanApiVersion = VK_API_VERSION_1_3;
                ai.physicalDevice = m_core->physical;
                ai.device = m_core->device;
                ai.instance = m_core->instance;
                ai.pVulkanFunctions = &fns;
                r = vmaCreateAllocator(&ai, &m_core->allocator);
                if (r != VK_SUCCESS)
                {
                    error = std::string("vmaCreateAllocator failed (") + vkResultName(r) + ")";
                    return false;
                }
                VkPipelineCacheCreateInfo pc{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
                vkCreatePipelineCache(m_core->device, &pc, nullptr, &m_pipelineCache);
                return true;
            }

            bool createLayout(std::string &error)
            {
                std::vector<VkDescriptorSetLayoutBinding> bindings;
                for (uint32_t i = 0; i < kUniformSlots; ++i)
                    bindings.push_back({i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                        nullptr});
                for (uint32_t i = 0; i < kTextureSlots; ++i)
                    bindings.push_back({kTextureBinding + i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
                for (uint32_t i = 0; i < kTextureSlots; ++i)
                    bindings.push_back({kSamplerBinding + i, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
                VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
                li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
                li.bindingCount = static_cast<uint32_t>(bindings.size());
                li.pBindings = bindings.data();
                VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
                pi.setLayoutCount = 1;
                pi.pSetLayouts = &m_setLayout;
                if (vkCreateDescriptorSetLayout(m_core->device, &li, nullptr, &m_setLayout) != VK_SUCCESS ||
                    vkCreatePipelineLayout(m_core->device, &pi, nullptr, &m_pipelineLayout) != VK_SUCCESS)
                {
                    error = "pipeline layout";
                    return false;
                }
                return true;
            }

            bool createFrames(std::string &error)
            {
                for (FrameSlot &f : m_frames)
                {
                    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
                    pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
                    pi.queueFamilyIndex = m_queueFamily;
                    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
                    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
                    if (vkCreateCommandPool(m_core->device, &pi, nullptr, &f.pool) != VK_SUCCESS ||
                        vkCreateFence(m_core->device, &fi, nullptr, &f.fence) != VK_SUCCESS ||
                        vkCreateSemaphore(m_core->device, &si, nullptr, &f.acquired) != VK_SUCCESS)
                    {
                        error = "command pools / fences";
                        return false;
                    }
                }
                VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
                if (vkCreateFence(m_core->device, &fi, nullptr, &m_waitFence) != VK_SUCCESS)
                {
                    error = "fence";
                    return false;
                }
                return true;
            }

            // Needs the frame's command buffers: the first frame is begun for the set-up uploads.
            bool createDummies(std::string &error)
            {
                beginFrame();
                const uint32_t black = 0;
                TextureDesc td;
                td.width = td.height = 1;
                td.name = "unbound texture";
                m_dummyTexture.reset(static_cast<VkTexture *>(createTexture(td, &black, 0).release()));
                SamplerDesc sd;
                m_defaultSampler.reset(static_cast<VkSamp *>(createSampler(sd).release()));
                BufferDesc bd;
                bd.size = 4096; // larger than any cbuffer: the shaders' ranges stay inside it
                bd.usage = BufferUsage::Constant;
                bd.dynamic = false;
                bd.name = "unbound constants";
                m_zeroBuffer.reset(static_cast<VkBuf *>(createBuffer(bd, nullptr).release()));
                if (!m_dummyTexture || !m_defaultSampler || !m_zeroBuffer)
                {
                    error = "default resources";
                    return false;
                }
                // the first frame's commands go now, so the swapchain can be made on an idle device
                submit(false, VK_NULL_HANDLE);
                vkDeviceWaitIdle(m_core->device);
                m_recording = false;
                return true;
            }

            bool makeOffscreenBack()
            {
                TextureDesc td;
                td.width = m_width;
                td.height = m_height;
                td.usage = UsageRenderTarget | UsageSampled;
                td.name = "back buffer (off-screen)";
                m_offscreenBack.reset(static_cast<VkTexture *>(createTexture(td, nullptr, 0).release()));
                return m_offscreenBack != nullptr;
            }

            bool createSwapchain(std::string &error)
            {
                VkSurfaceCapabilitiesKHR caps{};
                vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_core->physical, m_surface, &caps);
                uint32_t count = 0;
                vkGetPhysicalDeviceSurfaceFormatsKHR(m_core->physical, m_surface, &count, nullptr);
                std::vector<VkSurfaceFormatKHR> formats(count);
                vkGetPhysicalDeviceSurfaceFormatsKHR(m_core->physical, m_surface, &count, formats.data());
                if (formats.empty())
                {
                    error = "the window offers no surface formats";
                    return false;
                }
                // 8-bit UNORM as D3D11's R8G8B8A8_UNORM back buffer (no sRGB conversion)
                VkSurfaceFormatKHR chosen = formats[0];
                for (VkFormat want : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM})
                {
                    auto it = std::find_if(formats.begin(), formats.end(), [&](const VkSurfaceFormatKHR &f) {
                        return f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
                    });
                    if (it != formats.end())
                    {
                        chosen = *it;
                        break;
                    }
                }
                vkGetPhysicalDeviceSurfacePresentModesKHR(m_core->physical, m_surface, &count, nullptr);
                std::vector<VkPresentModeKHR> modes(count);
                vkGetPhysicalDeviceSurfacePresentModesKHR(m_core->physical, m_surface, &count, modes.data());
                auto offered = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
                // Immediate: newest frame, never torn (mailbox); Vsync: FIFO; Tearing: immediate (mailbox without it)
                VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
                if (m_presentMode == PresentMode::Immediate)
                    mode = offered(VK_PRESENT_MODE_MAILBOX_KHR) ? VK_PRESENT_MODE_MAILBOX_KHR
                           : offered(VK_PRESENT_MODE_IMMEDIATE_KHR) ? VK_PRESENT_MODE_IMMEDIATE_KHR
                                                                     : VK_PRESENT_MODE_FIFO_KHR;
                else if (m_presentMode == PresentMode::Tearing)
                    mode = offered(VK_PRESENT_MODE_IMMEDIATE_KHR) ? VK_PRESENT_MODE_IMMEDIATE_KHR
                           : offered(VK_PRESENT_MODE_MAILBOX_KHR) ? VK_PRESENT_MODE_MAILBOX_KHR
                                                                   : VK_PRESENT_MODE_FIFO_KHR;
                VkExtent2D extent = caps.currentExtent;
                if (extent.width == 0xFFFFFFFFu)
                    extent = {m_width, m_height};
                extent.width = std::clamp(extent.width, caps.minImageExtent.width, std::max(caps.minImageExtent.width, caps.maxImageExtent.width));
                extent.height =
                    std::clamp(extent.height, caps.minImageExtent.height, std::max(caps.minImageExtent.height, caps.maxImageExtent.height));
                if (!extent.width || !extent.height)
                    return true; // minimised: no swapchain until the window has a size again
                uint32_t images = std::max(caps.minImageCount, 3u);
                if (caps.maxImageCount)
                    images = std::min(images, caps.maxImageCount);
                VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
                ci.surface = m_surface;
                ci.minImageCount = images;
                ci.imageFormat = chosen.format;
                ci.imageColorSpace = chosen.colorSpace;
                ci.imageExtent = extent;
                ci.imageArrayLayers = 1;
                ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
                ci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                                                                                     : caps.currentTransform;
                ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
                for (VkCompositeAlphaFlagBitsKHR a : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
                                                      VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR})
                    if (caps.supportedCompositeAlpha & a)
                    {
                        ci.compositeAlpha = a;
                        break;
                    }
                ci.presentMode = mode;
                ci.clipped = VK_TRUE;
                ci.oldSwapchain = m_swapchain;
                VkSwapchainKHR made = VK_NULL_HANDLE;
                const VkResult r = vkCreateSwapchainKHR(m_core->device, &ci, nullptr, &made);
                destroySwapchain();
                if (r != VK_SUCCESS)
                {
                    error = std::string("vkCreateSwapchainKHR failed (") + vkResultName(r) + ")";
                    return false;
                }
                m_swapchain = made;
                m_swapFormat = chosen.format;
                m_swapExtent = extent;
                vkGetSwapchainImagesKHR(m_core->device, m_swapchain, &count, nullptr);
                m_swapImages.resize(count);
                vkGetSwapchainImagesKHR(m_core->device, m_swapchain, &count, m_swapImages.data());
                m_swapLayouts.assign(count, VK_IMAGE_LAYOUT_UNDEFINED);
                for (VkImage image : m_swapImages)
                {
                    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                    vi.image = image;
                    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
                    vi.format = m_swapFormat;
                    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    VkImageView view = VK_NULL_HANDLE;
                    vkCreateImageView(m_core->device, &vi, nullptr, &view);
                    m_swapViews.push_back(view);
                    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
                    VkSemaphore done = VK_NULL_HANDLE;
                    vkCreateSemaphore(m_core->device, &si, nullptr, &done);
                    m_renderDone.push_back(done);
                }
                static const char *const names[] = {"immediate", "mailbox", "fifo", "fifo relaxed"};
                std::fprintf(stderr, "[rhi] Vulkan swapchain %ux%u, %u images, %s\n", extent.width, extent.height, count,
                             mode <= VK_PRESENT_MODE_FIFO_RELAXED_KHR ? names[mode] : "?");
                return true;
            }

            // The old swapchain's views and semaphores (its images belong to it); the swapchain itself when replaced
            void destroySwapchain()
            {
                VkDevice d = m_core->device;
                for (VkImageView v : m_swapViews)
                    vkDestroyImageView(d, v, nullptr);
                for (VkSemaphore s : m_renderDone)
                    vkDestroySemaphore(d, s, nullptr);
                m_swapViews.clear();
                m_renderDone.clear();
                m_swapImages.clear();
                m_swapLayouts.clear();
                if (m_swapchain)
                    vkDestroySwapchainKHR(d, m_swapchain, nullptr);
                m_swapchain = VK_NULL_HANDLE;
            }

            bool acquireBackbuffer()
            {
                if (m_offscreen)
                    return true;
                if (m_imageIndex != kNoImage)
                    return true;
                for (int attempt = 0; attempt < 2; ++attempt)
                {
                    if (m_swapchainDirty || !m_swapchain)
                    {
                        // a new swapchain on an idle GPU; the commands so far go first
                        submit(false, VK_NULL_HANDLE);
                        vkDeviceWaitIdle(m_core->device);
                        beginCommands();
                        std::string error;
                        m_swapchainDirty = false;
                        if (!createSwapchain(error))
                        {
                            std::fprintf(stderr, "[rhi] %s\n", error.c_str());
                            return false;
                        }
                        if (!m_swapchain)
                            return false; // minimised
                    }
                    const auto t0 = std::chrono::steady_clock::now();
                    const VkResult r = vkAcquireNextImageKHR(m_core->device, m_swapchain, UINT64_MAX, frame().acquired, VK_NULL_HANDLE,
                                                             &m_imageIndex);
                    m_stats.acquireMs += msSince(t0);
                    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR)
                    {
                        m_waitAcquire = true;
                        if (r == VK_SUBOPTIMAL_KHR)
                            m_swapchainDirty = true;
                        return true;
                    }
                    m_imageIndex = kNoImage;
                    if (r != VK_ERROR_OUT_OF_DATE_KHR)
                    {
                        reportLost(r, "vkAcquireNextImageKHR");
                        return false;
                    }
                    m_swapchainDirty = true;
                }
                return false;
            }

            // ---- frames and command buffers ------------------------------------------------------------------------
            FrameSlot &frame() { return m_frames[m_core->frame]; }

            VkCommandBuffer newCommandBuffer()
            {
                FrameSlot &f = frame();
                if (f.used == f.buffers.size())
                {
                    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
                    ai.commandPool = f.pool;
                    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                    ai.commandBufferCount = 1;
                    VkCommandBuffer cb = VK_NULL_HANDLE;
                    vkAllocateCommandBuffers(m_core->device, &ai, &cb);
                    f.buffers.push_back(cb);
                }
                VkCommandBuffer cb = f.buffers[f.used++];
                VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vkBeginCommandBuffer(cb, &bi);
                return cb;
            }

            // The commands of a frame (or of the part after a readback): the main buffer, and a set-up buffer for new
            // resources' first contents that runs before it.
            void beginCommands()
            {
                m_cmd = newCommandBuffer();
                m_init = VK_NULL_HANDLE;
                m_recording = true;
                m_rendering = false;
                // a new command buffer has no state: everything is set again at the next draw
                m_boundPipeline = VK_NULL_HANDLE;
                m_pushed.valid = false;
                m_emitted = {};
                m_pipelineDirty = m_descriptorsDirty = m_buffersDirty = true;
                m_viewportDirty = m_scissorDirty = m_blendDirty = true;
                m_markerDepth = 0;
            }

            VkCommandBuffer initCmd()
            {
                if (!m_init)
                    m_init = newCommandBuffer();
                return m_init;
            }

            void beginFrame()
            {
                m_core->owner.store(std::this_thread::get_id(), std::memory_order_relaxed);
                m_core->frame = (m_core->frame + 1) % kFrames;
                FrameSlot &f = frame();
                if (f.submitted)
                {
                    const auto t0 = std::chrono::steady_clock::now();
                    vkWaitForFences(m_core->device, 1, &f.fence, VK_TRUE, UINT64_MAX);
                    m_stats.fenceMs += msSince(t0);
                    vkResetFences(m_core->device, 1, &f.fence);
                    f.submitted = false;
                }
                if (m_timer.on)
                    collectTimes(m_core->frame);
                m_core->collect(m_core->frame);
                vkResetCommandPool(m_core->device, f.pool, 0);
                f.used = 0;
                for (Ring::Chunk &c : f.ring.chunks)
                    c.used = 0;
                f.ring.current = 0;
                ++m_frameSerial;
                beginCommands();
                if (m_timer.markers)
                    m_timer.frameBegin = timestamp();
            }

            // Ends the commands and submits them; `final`: the frame's last submit (its fence, the present's semaphore)
            void submit(bool final, VkSemaphore signal)
            {
                if (!m_recording)
                    return;
                if (m_rendering)
                {
                    vkCmdEndRendering(m_cmd);
                    m_rendering = false;
                }
                for (; m_markerDepth > 0; --m_markerDepth) // debug labels end with their command buffer; GPU timer spans go on
                    vkCmdEndDebugUtilsLabelEXT(m_cmd);
                drainStaged();
                VkCommandBuffer cbs[2];
                uint32_t n = 0;
                if (m_init)
                {
                    vkEndCommandBuffer(m_init);
                    cbs[n++] = m_init;
                }
                vkEndCommandBuffer(m_cmd);
                cbs[n++] = m_cmd;
                VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; // the swap image's first use
                VkSemaphore wait = frame().acquired;
                if (m_waitAcquire)
                {
                    si.waitSemaphoreCount = 1;
                    si.pWaitSemaphores = &wait;
                    si.pWaitDstStageMask = &waitStage;
                    m_waitAcquire = false;
                }
                si.commandBufferCount = n;
                si.pCommandBuffers = cbs;
                if (signal)
                {
                    si.signalSemaphoreCount = 1;
                    si.pSignalSemaphores = &signal;
                }
                VkFence fence = final ? frame().fence : VK_NULL_HANDLE;
                const auto t0 = std::chrono::steady_clock::now();
                const VkResult r = vkQueueSubmit(m_queue, 1, &si, fence);
                m_stats.submitMs += msSince(t0);
                if (r != VK_SUCCESS)
                    reportLost(r, "vkQueueSubmit");
                else if (final)
                    frame().submitted = true;
                m_recording = false;
                m_init = VK_NULL_HANDLE;
            }

            // The commands so far run to the end (readbacks); recording goes on in new command buffers of this frame.
            void flushAndWait()
            {
                const bool inPass = m_inPass;
                submit(false, VK_NULL_HANDLE);
                VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                vkQueueSubmit(m_queue, 1, &si, m_waitFence);
                vkWaitForFences(m_core->device, 1, &m_waitFence, VK_TRUE, UINT64_MAX);
                vkResetFences(m_core->device, 1, &m_waitFence);
                beginCommands();
                m_inPass = inPass;
                m_passStarted = m_passStarted && inPass;
                ++m_stats.waits;
            }

            // An object about to be destroyed: no binding may keep pointing at it
            void unbind(const void *object)
            {
                if (object == m_vertexBuffer)
                    m_vertexBuffer = nullptr;
                if (object == m_indexBuffer)
                    m_indexBuffer = nullptr;
                for (VkBuf *&u : m_uniforms)
                    if (u == object)
                    {
                        u = nullptr;
                        m_descriptorsDirty = true;
                    }
                for (uint32_t i = 0; i < kTextureSlots; ++i)
                {
                    if (m_textures[i] == object)
                        m_textures[i] = nullptr, m_descriptorsDirty = true;
                    if (m_samplers[i] == object)
                        m_samplers[i] = nullptr, m_descriptorsDirty = true;
                }
                if (object == m_pipeline)
                {
                    m_pipeline = nullptr;
                    m_boundPipeline = VK_NULL_HANDLE;
                }
                if (object == m_boundPipe)
                    m_boundPipe = nullptr;
                if (m_inPass && (object == m_passColor || object == m_passDepth))
                {
                    // a target destroyed inside its pass: the pass ends here
                    outsideRendering();
                    if (object == m_passColor)
                        m_passColor = nullptr;
                    if (object == m_passDepth)
                        m_passDepth = nullptr;
                }
            }

            // ---- passes ---------------------------------------------------------------------------------------------
            VkImageLayout homeLayout(const VkTexture &t) const
            {
                if (m_inPass && &t == m_passColor)
                    return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                if (m_inPass && &t == m_passDepth)
                    return VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                return t.resting;
            }

            // Copies and barriers can't be recorded inside the rendering: it stops and goes on at the next draw
            // `suspend`: inside a pass, to go on later (counted); false at the pass's end
            void outsideRendering(bool suspend = true)
            {
                if (!m_rendering)
                    return;
                vkCmdEndRendering(m_cmd);
                m_rendering = false;
                if (suspend)
                    ++m_stats.suspends;
            }

            void startRendering()
            {
                if (m_rendering || !m_inPass)
                    return;
                VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
                ri.renderArea = {{0, 0}, {m_passWidth, m_passHeight}};
                ri.layerCount = 1;
                const bool first = !m_passStarted;
                if (m_passColor || m_passToBackbuffer)
                {
                    if (m_passToBackbuffer)
                    {
                        color.imageView = m_swapViews[m_imageIndex];
                        ri.renderArea.extent = {std::min(m_passWidth, m_swapExtent.width), std::min(m_passHeight, m_swapExtent.height)};
                    }
                    else
                        color.imageView = m_passColor->attachmentView;
                    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    color.loadOp = first && m_pass.clearColor ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
                    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    std::memcpy(color.clearValue.color.float32, m_pass.clearRgba, sizeof(m_pass.clearRgba));
                    ri.colorAttachmentCount = 1;
                    ri.pColorAttachments = &color;
                }
                if (m_passDepth)
                {
                    depth.imageView = m_passDepth->attachmentView;
                    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                    depth.loadOp = first && m_pass.clearDepth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
                    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    depth.clearValue.depthStencil = {m_pass.clearDepthValue, 0};
                    ri.pDepthAttachment = &depth;
                }
                // targets an operation inside the pass moved: back into their attachment layouts
                if (m_passColor)
                    barrier(m_cmd, *m_passColor, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                if (m_passDepth)
                    barrier(m_cmd, *m_passDepth, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
                vkCmdBeginRendering(m_cmd, &ri);
                m_rendering = true;
                m_passStarted = true;
                ++m_stats.passes;
            }

            void endPassInternal()
            {
                if (!m_inPass)
                    return;
                if (!m_passStarted && (m_pass.clearColor || m_pass.clearDepth))
                    startRendering(); // a pass with only its clears
                outsideRendering(false);
                if (m_passMarker)
                    popMarker();
                m_passMarker = false;
                m_inPass = false;
                if (m_passColor)
                    barrier(m_cmd, *m_passColor, m_passColor->resting);
                if (m_passDepth)
                    barrier(m_cmd, *m_passDepth, m_passDepth->resting);
                m_passColor = nullptr;
                m_passDepth = nullptr;
                m_passToBackbuffer = false;
            }

            // ---- draws ----------------------------------------------------------------------------------------------
            Slice ringAlloc(VkDeviceSize size)
            {
                Ring &ring = frame().ring;
                size = (size + kRingAlign - 1) & ~(kRingAlign - 1);
                while (ring.current < ring.chunks.size() && ring.chunks[ring.current].used + size > ring.chunks[ring.current].size)
                    ++ring.current;
                if (ring.current == ring.chunks.size())
                {
                    Ring::Chunk c;
                    c.size = std::max(kRingChunk, size);
                    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                    bi.size = c.size;
                    bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
                    VmaAllocationCreateInfo ai{};
                    ai.usage = VMA_MEMORY_USAGE_AUTO;
                    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
                    ai.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
                    VmaAllocationInfo info{};
                    if (vmaCreateBuffer(m_core->allocator, &bi, &ai, &c.buffer, &c.allocation, &info) != VK_SUCCESS)
                    {
                        std::fprintf(stderr, "[rhi] upload memory (%llu bytes) failed\n", static_cast<unsigned long long>(c.size));
                        std::abort();
                    }
                    c.data = static_cast<uint8_t *>(info.pMappedData);
                    ring.chunks.push_back(c);
                }
                Ring::Chunk &c = ring.chunks[ring.current];
                Slice s{c.buffer, c.used, c.data + c.used};
                c.used += size;
                m_stats.uploaded += size;
                return s;
            }

            // A dynamic buffer's contents in this frame's ring (copied from its CPU copy once per update / frame)
            void place(VkBuf &b)
            {
                if (b.sliceFrame == m_frameSerial)
                    return;
                const size_t n = std::min(b.used, b.shadow.size());
                const Slice s = ringAlloc(n);
                std::memcpy(s.data, b.shadow.data(), n);
                b.sliceBuffer = s.buffer;
                b.sliceOffset = s.offset;
                b.sliceFrame = m_frameSerial;
            }

            VkBuffer bufferOf(VkBuf &b, VkDeviceSize &offset)
            {
                if (!b.desc().dynamic)
                {
                    offset = 0;
                    return b.buffer;
                }
                place(b);
                offset = b.sliceOffset;
                return b.sliceBuffer;
            }

            bool prepareDraw(bool indexed)
            {
                if (!m_pipeline || !m_inPass || (indexed && !m_indexBuffer))
                    return false;
                // textures the shader reads: readable (outside the rendering), not the pass's own targets
                VkTexture *sampled[kTextureSlots];
                for (uint32_t i = 0; i < kTextureSlots; ++i)
                {
                    VkTexture *t = m_textures[i];
                    if (t && (t == m_passColor || t == m_passDepth || t->samples != VK_SAMPLE_COUNT_1_BIT || t->resting != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
                        t = nullptr; // D3D unbinds a texture that is also the target: reads zeros
                    sampled[i] = t ? t : m_dummyTexture.get();
                    if (sampled[i]->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                    {
                        outsideRendering();
                        barrier(m_cmd, *sampled[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    }
                }
                // dynamic buffers changed since the last draw: new slices (no commands)
                VkDeviceSize uniformOffsets[kUniformSlots] = {};
                VkBuffer uniformBuffers[kUniformSlots] = {};
                for (uint32_t i = 0; i < kUniformSlots; ++i)
                {
                    VkBuf *b = m_uniforms[i] ? m_uniforms[i] : m_zeroBuffer.get();
                    if (b->desc().dynamic && b->sliceFrame != m_frameSerial)
                        m_descriptorsDirty = true;
                    uniformBuffers[i] = bufferOf(*b, uniformOffsets[i]);
                }
                if ((m_vertexBuffer && m_vertexBuffer->desc().dynamic && m_vertexBuffer->sliceFrame != m_frameSerial) ||
                    (indexed && m_indexBuffer->desc().dynamic && m_indexBuffer->sliceFrame != m_frameSerial))
                    m_buffersDirty = true;

                startRendering();
                const VkFormat colorFormat = m_passToBackbuffer ? m_swapFormat : m_passColor ? m_passColor->format : VK_FORMAT_UNDEFINED;
                const VkFormat depthFormat = m_passDepth ? m_passDepth->format : VK_FORMAT_UNDEFINED;
                const VkSampleCountFlagBits samples = m_passColor ? m_passColor->samples : m_passDepth ? m_passDepth->samples : VK_SAMPLE_COUNT_1_BIT;
                VkPipeline pipeline = variant(*m_pipeline, colorFormat, depthFormat, samples);
                if (!pipeline)
                    return false;
                if (pipeline != m_boundPipeline)
                {
                    const int64_t t0 = spikeClock();
                    vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                    m_boundPipeline = pipeline;
                    ++m_stats.pipelineBinds;
                    if (statsOn() && m_boundPipe) // what changed: the shaders / vertex layout, or only fixed-function state
                    {
                        const PipelineDesc &a = m_boundPipe->desc, &b = m_pipeline->desc;
                        if (m_boundPipe->vs != m_pipeline->vs || m_boundPipe->ps != m_pipeline->ps)
                            ++m_stats.bindsShader;
                        else if (a.stride != b.stride || a.topology != b.topology || a.attributes.size() != b.attributes.size())
                            ++m_stats.bindsLayout;
                        else
                        {
                            ++m_stats.bindsState;
                            m_stats.bindsBlend += std::memcmp(&a.blend, &b.blend, sizeof(a.blend)) != 0;
                            m_stats.bindsDepth += a.depth.test != b.depth.test || a.depth.write != b.depth.write || a.depth.func != b.depth.func;
                        }
                    }
                    m_boundPipe = m_pipeline;
                    if (t0)
                        m_stats.bindNs += spikeClock() - t0;
                }
                m_pipelineDirty = false;
                if (dynamicDepth()) // the pipeline's depth state, set only when it differs from what the command buffer has
                {
                    const DepthState &ds = m_pipeline->desc.depth;
                    const VkBool32 test = (ds.test || ds.write) ? VK_TRUE : VK_FALSE, write = ds.write ? VK_TRUE : VK_FALSE;
                    const VkCompareOp op = ds.test ? compare(ds.func) : VK_COMPARE_OP_ALWAYS;
                    if (!m_emitted.depthValid || test != m_emitted.depthTest)
                        vkCmdSetDepthTestEnable(m_cmd, test);
                    if (!m_emitted.depthValid || write != m_emitted.depthWrite)
                        vkCmdSetDepthWriteEnable(m_cmd, write);
                    if (!m_emitted.depthValid || op != m_emitted.depthOp)
                        vkCmdSetDepthCompareOp(m_cmd, op);
                    m_emitted.depthTest = test;
                    m_emitted.depthWrite = write;
                    m_emitted.depthOp = op;
                    m_emitted.depthValid = true;
                }
                // [redundant state] viewport, scissor and buffers are recorded only when they differ from what this
                // command buffer has: the renderer sets them before every draw, and each one recorded made the driver
                // send that state again at the draw (Intel: ~5 us a draw on the laptop, PERFORMANCE.md 26)
                if (m_viewportDirty)
                {
                    if (redundantState() || !m_emitted.viewportValid || std::memcmp(&m_emitted.viewport, &m_viewport, sizeof(m_viewport)) != 0)
                    {
                        vkCmdSetViewport(m_cmd, 0, 1, &m_viewport);
                        m_emitted.viewport = m_viewport;
                        m_emitted.viewportValid = true;
                    }
                    m_viewportDirty = false;
                }
                if (m_scissorDirty)
                {
                    VkRect2D r = m_scissor;
                    r.extent.width = std::min<uint32_t>(r.extent.width, r.offset.x < static_cast<int32_t>(m_passWidth) ? m_passWidth - r.offset.x : 0);
                    r.extent.height = std::min<uint32_t>(r.extent.height, r.offset.y < static_cast<int32_t>(m_passHeight) ? m_passHeight - r.offset.y : 0);
                    if (redundantState() || !m_emitted.scissorValid || std::memcmp(&m_emitted.scissor, &r, sizeof(r)) != 0)
                    {
                        vkCmdSetScissor(m_cmd, 0, 1, &r);
                        m_emitted.scissor = r;
                        m_emitted.scissorValid = true;
                    }
                    m_scissorDirty = false;
                }
                if (m_blendDirty)
                {
                    if (redundantState() || !m_emitted.blendValid || std::memcmp(m_emitted.blend, m_blendConstant, sizeof(m_blendConstant)) != 0)
                    {
                        vkCmdSetBlendConstants(m_cmd, m_blendConstant);
                        std::memcpy(m_emitted.blend, m_blendConstant, sizeof(m_blendConstant));
                        m_emitted.blendValid = true;
                    }
                    m_blendDirty = false;
                }
                if (m_descriptorsDirty)
                {
                    // only the slots that differ from what this command buffer last pushed (push descriptors keep the
                    // rest); a new command buffer pushes everything
                    VkDescriptorBufferInfo ubo[kUniformSlots];
                    VkDescriptorImageInfo images[kTextureSlots], samplers[kTextureSlots];
                    VkWriteDescriptorSet writes[kUniformSlots + 2 * kTextureSlots];
                    uint32_t n = 0;
                    auto write = [&](uint32_t binding, VkDescriptorType type, const VkDescriptorBufferInfo *b, const VkDescriptorImageInfo *i) {
                        VkWriteDescriptorSet &w = writes[n++];
                        w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                        w.dstBinding = binding;
                        w.descriptorCount = 1;
                        w.descriptorType = type;
                        w.pBufferInfo = b;
                        w.pImageInfo = i;
                    };
                    for (uint32_t i = 0; i < kUniformSlots; ++i)
                    {
                        VkBuf *b = m_uniforms[i] ? m_uniforms[i] : m_zeroBuffer.get();
                        ubo[i] = {uniformBuffers[i], uniformOffsets[i], std::max<VkDeviceSize>((b->desc().size + 15u) & ~VkDeviceSize(15), 16)};
                        auto &was = m_pushed.ubo[i];
                        if (!m_pushed.valid || was.buffer != ubo[i].buffer || was.offset != ubo[i].offset || was.range != ubo[i].range)
                        {
                            was = ubo[i];
                            write(i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &ubo[i], nullptr);
                            ++m_stats.writesUbo;
                        }
                    }
                    for (uint32_t i = 0; i < kTextureSlots; ++i)
                    {
                        images[i] = {VK_NULL_HANDLE, sampled[i]->sampledView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                        if (!m_pushed.valid || m_pushed.view[i] != images[i].imageView)
                        {
                            m_pushed.view[i] = images[i].imageView;
                            write(kTextureBinding + i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, nullptr, &images[i]);
                            ++m_stats.writesTexture;
                        }
                        samplers[i] = {(m_samplers[i] ? m_samplers[i] : m_defaultSampler.get())->sampler, VK_NULL_HANDLE,
                                       VK_IMAGE_LAYOUT_UNDEFINED};
                        if (!m_pushed.valid || m_pushed.sampler[i] != samplers[i].sampler)
                        {
                            m_pushed.sampler[i] = samplers[i].sampler;
                            write(kSamplerBinding + i, VK_DESCRIPTOR_TYPE_SAMPLER, nullptr, &samplers[i]);
                            ++m_stats.writesSampler;
                        }
                    }
                    if (n)
                    {
                        const int64_t t0 = spikeClock();
                        vkCmdPushDescriptorSetKHR(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, n, writes);
                        ++m_stats.pushes;
                        if (t0)
                            m_stats.pushNs += spikeClock() - t0;
                    }
                    m_pushed.valid = true;
                    m_stats.descriptorWrites += n;
                    m_descriptorsDirty = false;
                }
                if (m_buffersDirty)
                {
                    if (m_vertexBuffer)
                    {
                        VkDeviceSize offset = 0;
                        VkBuffer vb = bufferOf(*m_vertexBuffer, offset);
                        offset += m_vertexOffset;
                        if (redundantState() || vb != m_emitted.vertexBuffer || offset != m_emitted.vertexOffset)
                        {
                            vkCmdBindVertexBuffers(m_cmd, 0, 1, &vb, &offset);
                            m_emitted.vertexBuffer = vb;
                            m_emitted.vertexOffset = offset;
                        }
                    }
                    if (m_indexBuffer)
                    {
                        VkDeviceSize offset = 0;
                        VkBuffer ib = bufferOf(*m_indexBuffer, offset);
                        const VkIndexType type = m_index32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
                        if (redundantState() || ib != m_emitted.indexBuffer || offset != m_emitted.indexOffset || type != m_emitted.indexType)
                        {
                            vkCmdBindIndexBuffer(m_cmd, ib, offset, type);
                            m_emitted.indexBuffer = ib;
                            m_emitted.indexOffset = offset;
                            m_emitted.indexType = type;
                        }
                    }
                    m_buffersDirty = false;
                }
                return true;
            }

            VkShaderModule shaderModule(const ShaderCode &code)
            {
                if (!code.data || code.size < 20 || code.size % 4)
                    return VK_NULL_HANDLE;
                VkShaderModule &m = m_modules[code.data];
                if (!m)
                {
                    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
                    ci.codeSize = code.size;
                    // the headers hold bytes; SPIR-V is read as words (copied: the arrays need not be 4-aligned)
                    std::vector<uint32_t> words(code.size / 4);
                    std::memcpy(words.data(), code.data, code.size);
                    ci.pCode = words.data();
                    if (vkCreateShaderModule(m_core->device, &ci, nullptr, &m) != VK_SUCCESS)
                        m = VK_NULL_HANDLE;
                }
                return m;
            }

            VkPipeline variant(VkPipe &p, VkFormat color, VkFormat depth, VkSampleCountFlagBits samples)
            {
                const uint64_t key = static_cast<uint64_t>(color) | (static_cast<uint64_t>(depth) << 24) | (static_cast<uint64_t>(samples) << 48);
                auto it = p.variants.find(key);
                if (it != p.variants.end())
                    return it->second;
                const PipelineDesc &d = p.desc;
                // dynamic depth: everything the VkPipeline holds but the depth state, as a key into the shared cache
                std::string shared;
                if (dynamicDepth())
                {
                    auto put = [&shared](const void *v, size_t n) { shared.append(static_cast<const char *>(v), n); };
                    put(&p.vs, sizeof(p.vs));
                    put(&p.ps, sizeof(p.ps));
                    put(&d.stride, sizeof(d.stride));
                    put(&d.topology, sizeof(d.topology));
                    for (const VertexAttribute &a : d.attributes)
                    {
                        put(&a.format, sizeof(a.format));
                        put(&a.offset, sizeof(a.offset));
                    }
                    const uint8_t blend[] = {d.blend.enable, static_cast<uint8_t>(d.blend.srcColor), static_cast<uint8_t>(d.blend.dstColor),
                                             static_cast<uint8_t>(d.blend.colorOp), static_cast<uint8_t>(d.blend.srcAlpha),
                                             static_cast<uint8_t>(d.blend.dstAlpha), static_cast<uint8_t>(d.blend.alphaOp), d.blend.writeMask,
                                             d.scissor, d.depthClip};
                    put(blend, sizeof(blend));
                    put(&key, sizeof(key));
                    p.sharedVariants = true;
                    auto found = m_sharedPipelines.find(shared);
                    if (found != m_sharedPipelines.end())
                    {
                        p.variants[key] = found->second;
                        return found->second;
                    }
                }
                VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
                stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
                stages[0].module = p.vs;
                stages[0].pName = "main";
                stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
                stages[1].module = p.ps;
                stages[1].pName = "main";
                VkVertexInputBindingDescription binding{0, d.stride, VK_VERTEX_INPUT_RATE_VERTEX};
                std::vector<VkVertexInputAttributeDescription> attrs;
                for (size_t i = 0; i < d.attributes.size(); ++i) // the shader's inputs in order (rhi.h)
                    attrs.push_back({static_cast<uint32_t>(i), 0, vertexFormat(d.attributes[i].format), d.attributes[i].offset});
                VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
                if (!attrs.empty())
                {
                    vi.vertexBindingDescriptionCount = 1;
                    vi.pVertexBindingDescriptions = &binding;
                    vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
                    vi.pVertexAttributeDescriptions = attrs.data();
                }
                VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
                ia.topology = topology(d.topology);
                VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
                vp.viewportCount = 1;
                vp.scissorCount = 1;
                VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
                rs.depthClampEnable = d.depthClip ? VK_FALSE : VK_TRUE; // D3D11 DepthClipEnable=FALSE: clamped, not clipped
                rs.polygonMode = VK_POLYGON_MODE_FILL;
                rs.cullMode = VK_CULL_MODE_NONE;
                rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
                rs.lineWidth = 1.0f;
                VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
                ms.rasterizationSamples = samples;
                VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
                ds.depthTestEnable = (d.depth.test || d.depth.write) ? VK_TRUE : VK_FALSE;
                ds.depthWriteEnable = d.depth.write ? VK_TRUE : VK_FALSE;
                ds.depthCompareOp = d.depth.test ? compare(d.depth.func) : VK_COMPARE_OP_ALWAYS;
                VkPipelineColorBlendAttachmentState att{};
                att.blendEnable = d.blend.enable ? VK_TRUE : VK_FALSE;
                att.srcColorBlendFactor = blendFactor(d.blend.srcColor);
                att.dstColorBlendFactor = blendFactor(d.blend.dstColor);
                att.colorBlendOp = blendOp(d.blend.colorOp);
                att.srcAlphaBlendFactor = blendFactor(d.blend.srcAlpha);
                att.dstAlphaBlendFactor = blendFactor(d.blend.dstAlpha);
                att.alphaBlendOp = blendOp(d.blend.alphaOp);
                att.colorWriteMask = d.blend.writeMask & 0xF;
                VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
                cb.attachmentCount = color != VK_FORMAT_UNDEFINED ? 1u : 0u;
                cb.pAttachments = &att;
                const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT,           VK_DYNAMIC_STATE_SCISSOR,
                                              VK_DYNAMIC_STATE_BLEND_CONSTANTS,    VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
                                              VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP};
                VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
                dy.dynamicStateCount = dynamicDepth() ? 6u : 3u;
                dy.pDynamicStates = dyn;
                VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
                ri.colorAttachmentCount = cb.attachmentCount;
                ri.pColorAttachmentFormats = &color;
                ri.depthAttachmentFormat = depth;
                VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
                ci.pNext = &ri;
                ci.stageCount = 2;
                ci.pStages = stages;
                ci.pVertexInputState = &vi;
                ci.pInputAssemblyState = &ia;
                ci.pViewportState = &vp;
                ci.pRasterizationState = &rs;
                ci.pMultisampleState = &ms;
                ci.pDepthStencilState = &ds;
                ci.pColorBlendState = &cb;
                ci.pDynamicState = &dy;
                ci.layout = m_pipelineLayout;
                VkPipeline made = VK_NULL_HANDLE;
                const auto t0 = std::chrono::steady_clock::now();
                const VkResult r = vkCreateGraphicsPipelines(m_core->device, m_pipelineCache, 1, &ci, nullptr, &made);
                if (r != VK_SUCCESS)
                {
                    std::fprintf(stderr, "[rhi] pipeline '%s' failed (%s)\n", p.name.c_str(), vkResultName(r));
                    made = VK_NULL_HANDLE;
                }
                p.variants[key] = made;
                if (!shared.empty() && made)
                    m_sharedPipelines[shared] = made;
                ++m_stats.pipelines;
                m_stats.pipelineMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                return made;
            }

            // ---- textures made on other threads (freeThreadedCreation) ---------------------------------------------
            // A new texture's first contents off the owner thread: copied there into a staging buffer of their own; the
            // copies go into the set-up commands at the owner's next submit (drainStaged). The texture counts as resting
            // from now on: the owner gets it only after this returns, so anything it records with it comes after the
            // drain's copies in the same submit (the set-up commands run first) or in a later one.
            bool stage(VkTexture &t, const void *const *levels, const uint32_t *pitches, uint32_t count)
            {
                Staged st;
                st.image = t.image;
                st.aspect = t.aspect;
                st.mips = t.mips;
                st.resting = t.resting;
                const Format format = t.desc().format;
                VkDeviceSize size = 0;
                for (uint32_t i = 0; i < count; ++i)
                {
                    const uint32_t w = std::max(1u, t.desc().width >> i), h = std::max(1u, t.desc().height >> i);
                    VkBufferImageCopy &c = st.regions[st.count++];
                    c = {};
                    c.bufferOffset = size;
                    c.imageSubresource = {t.aspect, i, 0, 1};
                    c.imageExtent = {w, h, 1};
                    size += (static_cast<VkDeviceSize>(rowBytes(format, w)) * rowCount(format, h) + 15u) & ~VkDeviceSize(15);
                }
                VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                bi.size = std::max<VkDeviceSize>(size, 16);
                bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
                VmaAllocationCreateInfo ai{};
                ai.usage = VMA_MEMORY_USAGE_AUTO;
                ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
                ai.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT; // visible to the GPU at the submit
                VmaAllocationInfo info{};
                if (vmaCreateBuffer(m_core->allocator, &bi, &ai, &st.buffer, &st.allocation, &info) != VK_SUCCESS)
                {
                    std::fprintf(stderr, "[rhi] staging memory (%llu bytes) failed\n", static_cast<unsigned long long>(bi.size));
                    return false;
                }
                for (uint32_t i = 0; i < st.count; ++i)
                {
                    const VkBufferImageCopy &c = st.regions[i];
                    const uint32_t row = rowBytes(format, c.imageExtent.width), rows = rowCount(format, c.imageExtent.height);
                    uint8_t *dst = static_cast<uint8_t *>(info.pMappedData) + c.bufferOffset;
                    const auto *src = static_cast<const uint8_t *>(levels[i]);
                    if (pitches[i] == row)
                        std::memcpy(dst, src, static_cast<size_t>(row) * rows);
                    else
                        for (uint32_t y = 0; y < rows; ++y)
                            std::memcpy(dst + static_cast<size_t>(y) * row, src + static_cast<size_t>(y) * pitches[i], row);
                }
                t.layout = t.resting;
                std::lock_guard<std::mutex> lock(m_stagedMutex);
                m_staged.push_back(st);
                return true;
            }

            // The owner, at each submit: the staged textures' copies into the set-up commands; their staging buffers and
            // the frees of textures released off this thread go with this frame (after its fence)
            void drainStaged()
            {
                auto orphans = m_core->takeOrphans(); // first: an orphan's own copy is staged before it can be released
                std::vector<Staged> staged;
                {
                    std::lock_guard<std::mutex> lock(m_stagedMutex);
                    staged.swap(m_staged);
                }
                if (!staged.empty())
                {
                    VkCommandBuffer cmd = initCmd();
                    for (const Staged &st : staged)
                    {
                        imageBarrier(cmd, st.image, st.aspect, 0, st.mips, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                        vkCmdCopyBufferToImage(cmd, st.buffer, st.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, st.count, st.regions);
                        imageBarrier(cmd, st.image, st.aspect, 0, st.mips, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, st.resting);
                        Core *c = m_core.get();
                        const VkBuffer b = st.buffer;
                        const VmaAllocation a = st.allocation;
                        m_core->garbage[m_core->frame].push_back([c, b, a] { vmaDestroyBuffer(c->allocator, b, a); });
                    }
                }
                for (auto &f : orphans)
                    m_core->garbage[m_core->frame].push_back(std::move(f));
            }

            // ---- copies -------------------------------------------------------------------------------------------
            void uploadRegion(VkCommandBuffer cmd, VkTexture &t, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void *data, uint32_t pitch,
                              uint32_t level = 0)
            {
                const uint32_t row = rowBytes(t.desc().format, w), rows = rowCount(t.desc().format, h); // texels or 4x4 blocks
                const Slice s = ringAlloc(static_cast<VkDeviceSize>(row) * rows);
                for (uint32_t i = 0; i < rows; ++i)
                    std::memcpy(s.data + static_cast<size_t>(i) * row, static_cast<const uint8_t *>(data) + static_cast<size_t>(i) * pitch, row);
                VkBufferImageCopy copy{};
                copy.bufferOffset = s.offset;
                copy.imageSubresource = {t.aspect, level, 0, 1};
                copy.imageOffset = {static_cast<int32_t>(x), static_cast<int32_t>(y), 0};
                copy.imageExtent = {w, h, 1};
                vkCmdCopyBufferToImage(cmd, s.buffer, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            }

            void copyToBuffer(VkCommandBuffer cmd, VkBuf &b, const void *data, size_t size)
            {
                const Slice s = ringAlloc(size);
                std::memcpy(s.data, data, size);
                const VkBufferCopy copy{s.offset, 0, size};
                vkCmdCopyBuffer(cmd, s.buffer, b.buffer, 1, &copy);
            }

            // Levels 1.. from level 0 by linear blits; the image enters and leaves in TRANSFER_DST (every level).
            void mipChain(VkCommandBuffer cmd, VkTexture &t)
            {
                int32_t w = static_cast<int32_t>(t.desc().width), h = static_cast<int32_t>(t.desc().height);
                for (uint32_t level = 1; level < t.mips; ++level)
                {
                    levelBarrier(cmd, t, level - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                    VkImageBlit blit{};
                    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
                    blit.srcOffsets[1] = {w, h, 1};
                    w = std::max(w / 2, 1);
                    h = std::max(h / 2, 1);
                    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
                    blit.dstOffsets[1] = {w, h, 1};
                    vkCmdBlitImage(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                                   VK_FILTER_LINEAR);
                    levelBarrier(cmd, t, level - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                }
            }

            // ---- barriers (whole-pipeline: correctness first) --------------------------------------------------------
            // What uses an image in a layout: the stages and the accesses to wait for (`src`: the writes to make
            // available) or to make wait (the reads and writes after). Narrow masks matter on Intel: ALL_COMMANDS /
            // MEMORY flushed and invalidated every cache at each of ~175 barriers a frame (PERFORMANCE.md 16).
            // PS2X_RHI_FULL_BARRIERS=1: the old whole-pipeline barriers.
            static void layoutScope(VkImageLayout layout, bool src, VkPipelineStageFlags2 &stage, VkAccessFlags2 &access)
            {
                switch (layout)
                {
                case VK_IMAGE_LAYOUT_UNDEFINED:
                    stage = VK_PIPELINE_STAGE_2_NONE;
                    access = VK_ACCESS_2_NONE;
                    return;
                case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
                    stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
                    access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                    return;
                case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
                    stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
                    access = src ? VK_ACCESS_2_NONE : VK_ACCESS_2_TRANSFER_READ_BIT;
                    return;
                case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
                    stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                    access = src ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
                                 : VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                    return;
                case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
                    stage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
                    access = src ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                                 : VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                    return;
                case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: // sampled by pixel shaders only (rhi.h setTexture)
                    stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                    access = src ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                    return;
                default:
                    stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                    access = src ? VK_ACCESS_2_MEMORY_WRITE_BIT : VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
                    return;
                }
            }

            // `swap`: a swapchain image - chained to the acquire / present semaphores at COLOR_ATTACHMENT_OUTPUT on both
            // sides (the usual pattern; NONE there left the transitions unordered with them)
            void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, uint32_t baseLevel, uint32_t levels,
                              VkImageLayout from, VkImageLayout to, bool swap = false)
            {
                VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                static const bool full = std::getenv("PS2X_RHI_FULL_BARRIERS") != nullptr;
                if (swap)
                {
                    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                    b.srcAccessMask = from == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_2_NONE;
                    b.dstAccessMask = to == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                          ? VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
                                          : VK_ACCESS_2_NONE;
                }
                else if (full)
                {
                    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
                    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
                }
                else
                {
                    layoutScope(from, true, b.srcStageMask, b.srcAccessMask);
                    layoutScope(to, false, b.dstStageMask, b.dstAccessMask);
                }
                b.oldLayout = from;
                b.newLayout = to;
                b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = image;
                b.subresourceRange = {aspect, baseLevel, levels, 0, 1};
                VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                di.imageMemoryBarrierCount = 1;
                di.pImageMemoryBarriers = &b;
                vkCmdPipelineBarrier2(cmd, &di);
                ++m_stats.barriers;
            }

            // The whole texture into `to`; a write-after-write barrier when it is there already (a copy after a copy)
            void barrier(VkCommandBuffer cmd, VkTexture &t, VkImageLayout to)
            {
                if (t.layout == to && to != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
                    return;
                imageBarrier(cmd, t.image, t.aspect, 0, t.mips, t.layout, to);
                t.layout = to;
            }

            void levelBarrier(VkCommandBuffer cmd, VkTexture &t, uint32_t level, VkImageLayout from, VkImageLayout to)
            {
                imageBarrier(cmd, t.image, t.aspect, level, 1, from, to);
            }

            void swapBarrier(VkImageLayout to)
            {
                if (m_imageIndex == kNoImage || m_swapLayouts[m_imageIndex] == to)
                    return;
                outsideRendering();
                imageBarrier(m_cmd, m_swapImages[m_imageIndex], VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, m_swapLayouts[m_imageIndex], to, true);
                m_swapLayouts[m_imageIndex] = to;
            }

            void memoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                               VkAccessFlags2 dstAccess)
            {
                VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                b.srcStageMask = srcStage;
                b.srcAccessMask = srcAccess;
                b.dstStageMask = dstStage;
                b.dstAccessMask = dstAccess;
                VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                di.memoryBarrierCount = 1;
                di.pMemoryBarriers = &b;
                vkCmdPipelineBarrier2(cmd, &di);
            }

            // ---- misc -----------------------------------------------------------------------------------------------
            void nameObject(VkObjectType type, uint64_t handle, const char *name)
            {
                if (!m_debugUtils || !name || !vkSetDebugUtilsObjectNameEXT)
                    return;
                VkDebugUtilsObjectNameInfoEXT ni{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
                ni.objectType = type;
                ni.objectHandle = handle;
                ni.pObjectName = name;
                vkSetDebugUtilsObjectNameEXT(m_core->device, &ni);
            }

            void reportLost(VkResult r, const char *what)
            {
                if (m_lostReported)
                    return;
                m_lostReported = true;
                std::fprintf(stderr, "[rhi] %s: %s%s\n", what, vkResultName(r), r == VK_ERROR_DEVICE_LOST ? " (graphics device lost)" : "");
            }

            static double msSince(std::chrono::steady_clock::time_point t0)
            {
                return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            }

            // ---- PS2X_GPU_TIMING=1: GPU time between each pushMarker / popMarker pair (timestamps), summed per marker name
            // and reported every 5 s as ms per frame, as rhi_d3d11.cpp does; "(frame)" is the whole frame ----------------
            static constexpr uint32_t kNoQuery = ~0u;
            static constexpr uint32_t kQueries = 16384; // per frame slot

            void timerInit(bool hostReset)
            {
                // PS2X_RHI_GPU_TIME=1: the same, without gs_hw's marker (and pass break) around every draw - the passes as
                // they are, and the whole frame. PS2X_RHI_STATS=1: only each frame's end, for the input-to-GPU-done delay
                m_timer.markers = std::getenv("PS2X_GPU_TIMING") || std::getenv("PS2X_RHI_GPU_TIME");
                if (!m_timer.markers && !statsOn())
                    return;
                uint32_t count = 0;
                vkGetPhysicalDeviceQueueFamilyProperties(m_core->physical, &count, nullptr);
                std::vector<VkQueueFamilyProperties> families(count);
                vkGetPhysicalDeviceQueueFamilyProperties(m_core->physical, &count, families.data());
                VkPhysicalDeviceProperties props{};
                vkGetPhysicalDeviceProperties(m_core->physical, &props);
                if (!hostReset || m_queueFamily >= count || !families[m_queueFamily].timestampValidBits)
                {
                    std::fprintf(stderr, "[gpu-timing] this GPU / driver has no usable timestamps\n");
                    return;
                }
                m_timer.periodNs = props.limits.timestampPeriod;
                const uint32_t bits = families[m_queueFamily].timestampValidBits;
                m_timer.validMask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
                for (auto &slot : m_timer.slots)
                {
                    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
                    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
                    qi.queryCount = kQueries;
                    if (vkCreateQueryPool(m_core->device, &qi, nullptr, &slot.pool) != VK_SUCCESS)
                        return;
                    vkResetQueryPool(m_core->device, slot.pool, 0, kQueries);
                }
                m_timer.on = true;
            }

            uint32_t timestamp()
            {
                auto &slot = m_timer.slots[m_core->frame];
                if (!m_recording || slot.used >= kQueries)
                    return kNoQuery;
                const uint32_t q = slot.used++;
                vkCmdWriteTimestamp2(m_cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, slot.pool, q);
                return q;
            }

            // a frame slot's results, after its fence
            void collectTimes(uint32_t index)
            {
                auto &slot = m_timer.slots[index];
                if (slot.used)
                {
                    std::vector<uint64_t> t(slot.used);
                    if (vkGetQueryPoolResults(m_core->device, slot.pool, 0, slot.used, t.size() * sizeof(uint64_t), t.data(), sizeof(uint64_t),
                                              VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
                    {
                        ++m_timer.frames;
                        if (slot.inputNs > 0 && slot.frameEnd != kNoQuery)
                            noteInputDelay(t[slot.frameEnd], slot.inputNs);
                        for (const auto &sp : slot.spans)
                            if (t[sp.end] >= t[sp.begin])
                            {
                                auto &total = m_timer.totals[sp.name];
                                total.first += static_cast<double>(t[sp.end] - t[sp.begin]) * m_timer.periodNs / 1e6;
                                ++total.second;
                            }
                    }
                    vkResetQueryPool(m_core->device, slot.pool, 0, slot.used);
                }
                slot.used = 0;
                slot.spans.clear();
                slot.frameEnd = kNoQuery;
                slot.inputNs = 0;
                if (!m_timer.markers || !m_timer.frames || std::chrono::steady_clock::now() - m_timer.lastReport < std::chrono::seconds(5))
                    return;
                m_timer.lastReport = std::chrono::steady_clock::now();
                std::vector<std::pair<std::string, std::pair<double, uint64_t>>> rows(m_timer.totals.begin(), m_timer.totals.end());
                std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.second.first > b.second.first; });
                std::fprintf(stderr, "[gpu-timing] %llu frames, GPU ms per frame by marker:\n", static_cast<unsigned long long>(m_timer.frames));
                for (const auto &[name, total] : rows)
                    std::fprintf(stderr, "[gpu-timing] %8.3f ms %8.1f spans  %s\n", total.first / m_timer.frames,
                                 static_cast<double>(total.second) / m_timer.frames, name.c_str());
                m_timer.totals.clear();
                m_timer.frames = 0;
            }

            struct GpuTimer
            {
                struct Span
                {
                    std::string name;
                    uint32_t begin, end;
                };
                struct Slot
                {
                    VkQueryPool pool = VK_NULL_HANDLE;
                    uint32_t used = 0;
                    std::vector<Span> spans;
                    uint32_t frameEnd = kNoQuery; // the frame's last timestamp
                    int64_t inputNs = 0;          // its input snapshot (setFrameInputTime)
                };
                bool on = false;      // query pools exist
                bool markers = false; // spans between markers reported ([gpu-timing])
                double periodNs = 1.0;
                uint64_t validMask = ~0ull;
                // GPU ticks <-> steady_clock (VK_KHR_calibrated_timestamps, CLOCK_MONOTONIC: Linux's steady_clock)
                bool calibrated = false;
                uint64_t gpuRef = 0;
                int64_t cpuRef = 0;
                std::chrono::steady_clock::time_point calibratedAt{};
                std::array<Slot, kFrames> slots;
                std::vector<std::pair<std::string, uint32_t>> open;
                uint32_t frameBegin = kNoQuery;
                std::unordered_map<std::string, std::pair<double, uint64_t>> totals; // ms, spans
                uint64_t frames = 0;
                std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
            } m_timer;
            int64_t m_pendingInputNs = 0;

            // The input-to-GPU-done delay of a shown frame: its last timestamp on the CPU's clock minus its input snapshot
            void noteInputDelay(uint64_t gpuEnd, int64_t inputNs)
            {
                if (!m_calibration || !vkGetCalibratedTimestampsKHR)
                    return;
                const auto now = std::chrono::steady_clock::now();
                if (!m_timer.calibrated || now - m_timer.calibratedAt > std::chrono::seconds(1))
                {
                    VkCalibratedTimestampInfoKHR info[2] = {{VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR},
                                                            {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR}};
                    info[0].timeDomain = VK_TIME_DOMAIN_DEVICE_KHR;
                    info[1].timeDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;
                    uint64_t ts[2] = {}, deviation = 0;
                    if (vkGetCalibratedTimestampsKHR(m_core->device, 2, info, ts, &deviation) != VK_SUCCESS)
                        return;
                    m_timer.gpuRef = ts[0];
                    m_timer.cpuRef = static_cast<int64_t>(ts[1]);
                    m_timer.calibrated = true;
                    m_timer.calibratedAt = now;
                }
                // signed tick difference within the counter's valid bits
                uint64_t d = (gpuEnd - m_timer.gpuRef) & m_timer.validMask;
                int64_t ticks = static_cast<int64_t>(d);
                if (m_timer.validMask != ~0ull && d > (m_timer.validMask >> 1))
                    ticks = -static_cast<int64_t>((m_timer.gpuRef - gpuEnd) & m_timer.validMask);
                const int64_t doneNs = m_timer.cpuRef + static_cast<int64_t>(static_cast<double>(ticks) * m_timer.periodNs);
                const double ms = (doneNs - inputNs) / 1e6;
                if (ms > 0.0 && ms < 1000.0)
                    m_stats.inputDelays.push_back(ms);
            }

            // PS2X_RHI_STATS=1: per-frame counts every 5 s (suspended passes are the cost of copies inside a pass)
            void reportStats()
            {
                ++m_stats.frames;
                const auto now = std::chrono::steady_clock::now();
                const double s = std::chrono::duration<double>(now - m_statsStart).count();
                if (s < 5.0)
                    return;
                const double f = static_cast<double>(std::max<uint64_t>(m_stats.frames, 1));
                std::fprintf(stderr,
                             "[rhi] vulkan per frame: %.0f draws, %.0f descriptor writes, %.1f passes, %.1f suspended, %.1f barriers, "
                             "%.1f waits, %.0f KB uploaded; %llu new pipelines (%.0f ms); CPU ms waiting: fence %.2f, acquire %.2f, "
                             "submit %.2f, present %.2f\n",
                             m_stats.draws / f, m_stats.descriptorWrites / f, m_stats.passes / f, m_stats.suspends / f, m_stats.barriers / f,
                             m_stats.waits / f, m_stats.uploaded / f / 1024.0, static_cast<unsigned long long>(m_stats.pipelines),
                             m_stats.pipelineMs, m_stats.fenceMs / f, m_stats.acquireMs / f, m_stats.submitMs / f, m_stats.presentMs / f);
                std::fprintf(stderr,
                             "[rhi] per frame: %.0f pipeline binds (%.0f shaders, %.0f vertex layout, %.0f state only: %.0f blend, %.0f "
                             "depth); %.0f pushes (%.0f buffer, %.0f texture, %.0f sampler writes); CPU ms (PS2X_HOST_SPIKES): prepareDraw "
                             "%.2f (binds %.2f, pushes %.2f), vkCmdDraw %.2f\n",
                             m_stats.pipelineBinds / f, m_stats.bindsShader / f, m_stats.bindsLayout / f, m_stats.bindsState / f,
                             m_stats.bindsBlend / f, m_stats.bindsDepth / f, m_stats.pushes / f, m_stats.writesUbo / f,
                             m_stats.writesTexture / f, m_stats.writesSampler / f, m_stats.prepareNs / f / 1e6, m_stats.bindNs / f / 1e6,
                             m_stats.pushNs / f / 1e6, m_stats.drawNs / f / 1e6);
                if (!m_stats.inputDelays.empty())
                {
                    std::vector<double> d = m_stats.inputDelays;
                    std::sort(d.begin(), d.end());
                    double sum = 0.0;
                    for (double x : d)
                        sum += x;
                    std::fprintf(stderr, "[rhi] input to GPU done: avg %.1f ms, p50 %.1f, p95 %.1f, max %.1f (%zu frames; the display adds up to a refresh)\n",
                                 sum / d.size(), d[d.size() / 2], d[std::min(d.size() - 1, d.size() * 95 / 100)], d.back(), d.size());
                }
                m_stats = {};
                m_statsStart = now;
            }

            std::shared_ptr<Core> m_core;
            VkQueue m_queue = VK_NULL_HANDLE;
            uint32_t m_queueFamily = 0;
            VkSurfaceKHR m_surface = VK_NULL_HANDLE;
            bool m_offscreen = false;
            bool m_calibration = false; // VK_KHR_calibrated_timestamps on (noteInputDelay)

            bool deviceHas(const char *name)
            {
                uint32_t count = 0;
                vkEnumerateDeviceExtensionProperties(m_core->physical, nullptr, &count, nullptr);
                std::vector<VkExtensionProperties> exts(count);
                vkEnumerateDeviceExtensionProperties(m_core->physical, nullptr, &count, exts.data());
                return std::any_of(exts.begin(), exts.end(), [&](const VkExtensionProperties &e) { return std::strcmp(e.extensionName, name) == 0; });
            }
            bool m_debugUtils = false;
            bool m_anisotropy = false;
            bool m_blockCompression = false; // textureCompressionBC
            float m_maxAnisotropy = 1.0f;
            std::string m_adapterName;
            VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
            VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
            VkPipelineCache m_pipelineCache = VK_NULL_HANDLE;
            std::unordered_map<const void *, VkShaderModule> m_modules; // by shader bytes (static arrays)

            std::array<FrameSlot, kFrames> m_frames;
            VkFence m_waitFence = VK_NULL_HANDLE;
            struct Staged // a texture made off the owner thread, its first contents waiting (stage / drainStaged)
            {
                VkImage image = VK_NULL_HANDLE;
                VkImageAspectFlags aspect = 0;
                uint32_t mips = 1, count = 0;
                VkImageLayout resting = VK_IMAGE_LAYOUT_UNDEFINED;
                VkBuffer buffer = VK_NULL_HANDLE;
                VmaAllocation allocation = VK_NULL_HANDLE;
                VkBufferImageCopy regions[16];
            };
            std::mutex m_stagedMutex;
            std::vector<Staged> m_staged;
            uint64_t m_frameSerial = 0;
            VkCommandBuffer m_cmd = VK_NULL_HANDLE, m_init = VK_NULL_HANDLE;
            bool m_recording = false, m_rendering = false;
            int m_markerDepth = 0;

            // swapchain
            VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
            VkFormat m_swapFormat = VK_FORMAT_UNDEFINED;
            VkExtent2D m_swapExtent{};
            std::vector<VkImage> m_swapImages;
            std::vector<VkImageView> m_swapViews;
            std::vector<VkImageLayout> m_swapLayouts;
            std::vector<VkSemaphore> m_renderDone; // per image
            uint32_t m_imageIndex = kNoImage;
            bool m_waitAcquire = false, m_swapchainDirty = false;
            PresentMode m_presentMode = PresentMode::Immediate;
            uint32_t m_width = 0, m_height = 0;
            std::unique_ptr<VkTexture> m_offscreenBack;

            // the pass
            PassDesc m_pass;
            std::string m_passName;
            VkTexture *m_passColor = nullptr, *m_passDepth = nullptr;
            bool m_passToBackbuffer = false, m_inPass = false, m_passStarted = false, m_passMarker = false;
            uint32_t m_passWidth = 0, m_passHeight = 0;

            // bound state (D3D11-style, kept across draws and passes)
            VkPipe *m_pipeline = nullptr;
            VkPipeline m_boundPipeline = VK_NULL_HANDLE;
            VkPipe *m_boundPipe = nullptr; // PS2X_RHI_STATS: the RHI pipeline last bound (what a bind changes)
            std::unordered_map<std::string, VkPipeline> m_sharedPipelines; // dynamicDepth: one VkPipeline per all-but-depth state
            VkBuf *m_vertexBuffer = nullptr, *m_indexBuffer = nullptr;
            uint32_t m_vertexOffset = 0;
            bool m_index32 = false;
            std::array<VkBuf *, kUniformSlots> m_uniforms{};
            std::array<VkTexture *, kTextureSlots> m_textures{};
            std::array<VkSamp *, kTextureSlots> m_samplers{};
            VkViewport m_viewport{};
            VkRect2D m_scissor{};
            float m_blendConstant[4] = {0, 0, 0, 0};
            bool m_pipelineDirty = true, m_descriptorsDirty = true, m_buffersDirty = true;
            // what the current command buffer's push descriptors hold (a destroyed object's handle may come back as a
            // new one's: compared by value, so that case pushes again only when the value differs - and an equal
            // handle is the same Vulkan object at that point)
            struct
            {
                bool valid = false;
                VkDescriptorBufferInfo ubo[kUniformSlots]{};
                VkImageView view[kTextureSlots]{};
                VkSampler sampler[kTextureSlots]{};
            } m_pushed;
            bool m_viewportDirty = true, m_scissorDirty = true, m_blendDirty = true;
            struct Emitted // what the command buffer being recorded has (redundant state; reset by beginCommands)
            {
                VkViewport viewport{};
                VkRect2D scissor{};
                float blend[4] = {};
                bool viewportValid = false, scissorValid = false, blendValid = false, depthValid = false;
                VkBool32 depthTest = VK_FALSE, depthWrite = VK_FALSE;
                VkCompareOp depthOp = VK_COMPARE_OP_ALWAYS;
                VkBuffer vertexBuffer = VK_NULL_HANDLE, indexBuffer = VK_NULL_HANDLE;
                VkDeviceSize vertexOffset = 0, indexOffset = 0;
                VkIndexType indexType = VK_INDEX_TYPE_MAX_ENUM;
            } m_emitted;

            // what unbound slots read
            std::unique_ptr<VkTexture> m_dummyTexture;
            std::unique_ptr<VkSamp> m_defaultSampler;
            std::unique_ptr<VkBuf> m_zeroBuffer;

            bool m_lostReported = false;
            struct Stats
            {
                uint64_t frames = 0, draws = 0, passes = 0, suspends = 0, barriers = 0, waits = 0, uploaded = 0, pipelines = 0;
                uint64_t descriptorWrites = 0;
                uint64_t pipelineBinds = 0, pushes = 0;                          // PS2X_HOST_SPIKES
                uint64_t bindsShader = 0, bindsLayout = 0, bindsState = 0, bindsBlend = 0, bindsDepth = 0; // PS2X_RHI_STATS
                uint64_t writesUbo = 0, writesTexture = 0, writesSampler = 0;
                int64_t prepareNs = 0, drawNs = 0, bindNs = 0, pushNs = 0; // ... the CPU time in prepareDraw / vkCmdDraw / binds / pushes
                std::vector<double> inputDelays; // ms, input snapshot -> GPU done, per shown frame
                double pipelineMs = 0.0, fenceMs = 0.0, acquireMs = 0.0, submitMs = 0.0, presentMs = 0.0;
            } m_stats;
            Stats m_spikePrev;                                       // PS2X_HOST_SPIKES: m_stats at the last frame's end
            std::chrono::steady_clock::time_point m_spikeFrameEnd{}; // ... and when it ended
            std::chrono::steady_clock::time_point m_statsStart = std::chrono::steady_clock::now();
        };
    }

    std::unique_ptr<Device> createVulkanDevice(const DeviceDesc &desc, std::string &error)
    {
        auto device = std::make_unique<VulkanDevice>();
        if (!device->create(desc, error))
            return nullptr;
        return device;
    }

    std::vector<Gpu> listVulkanGpus()
    {
        // an instance of its own, only to list the cards (before the settings load, so before the device exists)
        if (volkInitialize() != VK_SUCCESS || volkGetInstanceVersion() < VK_API_VERSION_1_3)
            return {};
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "Vice City Stories";
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo = &app;
        VkInstance instance = VK_NULL_HANDLE;
        if (vkCreateInstance(&ci, nullptr, &instance) != VK_SUCCESS)
            return {};
        volkLoadInstanceOnly(instance);
        uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance, &count, devices.data());
        devices.resize(count);
        std::vector<uint32_t> index;
        std::vector<Gpu> gpus = orderGpus(devices, index);
        vkDestroyInstance(instance, nullptr);
        return gpus;
    }
}
