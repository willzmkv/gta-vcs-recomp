// D3D12 backend of rhi.h (feature level 11_0). The only file that includes d3d12 headers. The player's choice for the
// main thread's per-draw cost (docs/DECISIONS.md 2026-11-04): AMD's D3D11 driver validates and tracks every draw on
// the calling thread; here a draw is a few root arguments and a pipeline switch, recorded into a command list.
//
// Built as rhi_vulkan.cpp is (an explicit API behind the same D3D11-shaped interface), with D3D11's shaders (the DXBC
// fxc makes runs on D3D12 as it is):
// - one root signature for every pipeline: b0..b3 as root constant buffer views (vertex + pixel), t0..t7 and s0..s7
//   as two descriptor tables (pixel), the tables copied into shader-visible heaps when the bindings change;
// - dynamic buffers keep their contents on the CPU and get a slice of the frame's upload ring at their first use after
//   an update (D3D11's WRITE_DISCARD); static ones live in GPU memory, their first contents copied by the set-up
//   commands, which run before the frame's own;
// - pipelines are made per target format / depth format / sample count at the first draw that needs one;
// - textures carry their resource state; a pass's targets go into target states for the pass and back after;
// - generateMips is a draw per level (D3D12 has no GenerateMips);
// - textures made on other threads (freeThreadedCreation) stage their contents in an upload buffer of their own; the
//   owner records the copies into the set-up commands at its next submit;
// - Dear ImGui is drawn through the RHI by render::UiRenderer, as with Vulkan.
#include "rhi/rhi.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cfloat>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace rhi
{
    namespace
    {
        constexpr UINT kFrames = 2;      // frames in flight
        constexpr UINT kBackbuffers = 3; // flip model
        constexpr DXGI_FORMAT kBackbufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        constexpr uint32_t kUniformSlots = 4; // cbuffer b0..b3 (as rhi_vulkan.cpp)
        constexpr uint32_t kTextureSlots = 8; // t0..t7 / s0..s7 (beginPass unbinds 8, as rhi_d3d11.cpp)
        constexpr UINT64 kRingChunk = 8ull << 20;
        constexpr UINT kViewsPerFrame = 131072; // shader-visible SRV descriptors a frame (tables of 8, cached per frame)
        constexpr UINT kSamplerHeap = 2048;     // the most a shader-visible sampler heap may hold
        constexpr UINT kRootTextures = kUniformSlots, kRootSamplers = kUniformSlots + 1;

        UINT64 alignUp(UINT64 v, UINT64 a) { return (v + a - 1) & ~(a - 1); }

        // PS2X_RHI_STATS=1: per-frame counts every 5 s (what a frame asks of the GPU and of the driver)
        struct Stats
        {
            std::atomic<uint64_t> frames{0}, draws{0}, barriers{0}, barrierCalls{0}, passes{0}, psos{0}, textures{0}, buffers{0},
                tables{0}, uploads{0}, uploadBytes{0}, waits{0};
        } g_stats;
        bool statsOn()
        {
            static const bool on = std::getenv("PS2X_RHI_STATS") && std::getenv("PS2X_RHI_STATS")[0] != '0';
            return on;
        }

        // PS2X_HOST_SPIKES=<ms> (as rhi_vulkan.cpp): a frame of the main thread longer than that, with where its D3D12 time
        // went - pipelines compiled, textures / buffers / memory made, waits for the GPU, the present
        double spikeMs()
        {
            static const double ms = std::getenv("PS2X_HOST_SPIKES") ? std::atof(std::getenv("PS2X_HOST_SPIKES")) : 0.0;
            return ms;
        }
        struct Spans
        {
            double pso = 0, texture = 0, buffer = 0, memory = 0, wait = 0, present = 0, collect = 0;
            uint32_t psos = 0, textures = 0, buffers = 0, memories = 0, waits = 0, flushes = 0, collected = 0;
        };
        thread_local Spans t_spans; // the owner's (helper threads' textures are not its time)
        struct Span
        {
            double &to;
            std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
            explicit Span(double &t) : to(t) {}
            ~Span() { to += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); }
        };

        DXGI_FORMAT resourceFormat(Format f)
        {
            switch (f)
            {
            case Format::RGBA8: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case Format::R32F: return DXGI_FORMAT_R32_FLOAT;
            case Format::D32F: return DXGI_FORMAT_R32_TYPELESS; // DSV D32_FLOAT, SRV R32_FLOAT
            case Format::BGRA8: return DXGI_FORMAT_B8G8R8A8_UNORM;
            case Format::BC1: return DXGI_FORMAT_BC1_UNORM;
            case Format::BC2: return DXGI_FORMAT_BC2_UNORM;
            case Format::BC3: return DXGI_FORMAT_BC3_UNORM;
            case Format::BC4: return DXGI_FORMAT_BC4_UNORM;
            case Format::BC5: return DXGI_FORMAT_BC5_UNORM;
            case Format::BC7: return DXGI_FORMAT_BC7_UNORM;
            default: return DXGI_FORMAT_UNKNOWN;
            }
        }

        DXGI_FORMAT viewFormat(Format f) { return f == Format::D32F ? DXGI_FORMAT_R32_FLOAT : resourceFormat(f); }

        D3D12_BLEND blendFactor(BlendFactor f)
        {
            switch (f)
            {
            case BlendFactor::Zero: return D3D12_BLEND_ZERO;
            case BlendFactor::One: return D3D12_BLEND_ONE;
            case BlendFactor::SrcAlpha: return D3D12_BLEND_SRC_ALPHA;
            case BlendFactor::InvSrcAlpha: return D3D12_BLEND_INV_SRC_ALPHA;
            case BlendFactor::DstAlpha: return D3D12_BLEND_DEST_ALPHA;
            case BlendFactor::InvDstAlpha: return D3D12_BLEND_INV_DEST_ALPHA;
            case BlendFactor::Src1Alpha: return D3D12_BLEND_SRC1_ALPHA;
            case BlendFactor::InvSrc1Alpha: return D3D12_BLEND_INV_SRC1_ALPHA;
            case BlendFactor::Constant: return D3D12_BLEND_BLEND_FACTOR;
            case BlendFactor::InvConstant: return D3D12_BLEND_INV_BLEND_FACTOR;
            }
            return D3D12_BLEND_ONE;
        }

        D3D12_BLEND_OP blendOp(BlendOp op)
        {
            switch (op)
            {
            case BlendOp::Add: return D3D12_BLEND_OP_ADD;
            case BlendOp::Subtract: return D3D12_BLEND_OP_SUBTRACT;
            case BlendOp::ReverseSubtract: return D3D12_BLEND_OP_REV_SUBTRACT;
            case BlendOp::Min: return D3D12_BLEND_OP_MIN;
            case BlendOp::Max: return D3D12_BLEND_OP_MAX;
            }
            return D3D12_BLEND_OP_ADD;
        }

        D3D12_COMPARISON_FUNC compare(Compare c)
        {
            switch (c)
            {
            case Compare::Never: return D3D12_COMPARISON_FUNC_NEVER;
            case Compare::Less: return D3D12_COMPARISON_FUNC_LESS;
            case Compare::Equal: return D3D12_COMPARISON_FUNC_EQUAL;
            case Compare::LessEqual: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
            case Compare::Greater: return D3D12_COMPARISON_FUNC_GREATER;
            case Compare::NotEqual: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
            case Compare::GreaterEqual: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
            case Compare::Always: return D3D12_COMPARISON_FUNC_ALWAYS;
            }
            return D3D12_COMPARISON_FUNC_ALWAYS;
        }

        DXGI_FORMAT vertexFormat(VertexFormat f)
        {
            switch (f)
            {
            case VertexFormat::Float1: return DXGI_FORMAT_R32_FLOAT;
            case VertexFormat::Float2: return DXGI_FORMAT_R32G32_FLOAT;
            case VertexFormat::Float3: return DXGI_FORMAT_R32G32B32_FLOAT;
            case VertexFormat::Float4: return DXGI_FORMAT_R32G32B32A32_FLOAT;
            case VertexFormat::UByte4Norm: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case VertexFormat::UInt1: return DXGI_FORMAT_R32_UINT;
            }
            return DXGI_FORMAT_UNKNOWN;
        }

        D3D_PRIMITIVE_TOPOLOGY topology(Topology t)
        {
            switch (t)
            {
            case Topology::Triangles: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            case Topology::TriangleStrip: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
            case Topology::Lines: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
            case Topology::Points: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
            }
            return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        }

        D3D12_PRIMITIVE_TOPOLOGY_TYPE topologyType(Topology t)
        {
            switch (t)
            {
            case Topology::Lines: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
            case Topology::Points: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
            default: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            }
        }

        std::string hex(HRESULT hr)
        {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned>(hr));
            return buf;
        }

        std::string narrow(const wchar_t *w)
        {
            const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
            std::string s(n > 0 ? n - 1 : 0, '\0');
            if (n > 1)
                WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
            return s;
        }

        void setName(ID3D12Object *object, const char *name)
        {
            if (!object || !name)
                return;
            std::wstring w;
            for (const char *s = name; *s; ++s)
                w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*s)));
            object->SetName(w.c_str());
        }

        D3D12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE type)
        {
            D3D12_HEAP_PROPERTIES h{};
            h.Type = type;
            h.CreationNodeMask = 1;
            h.VisibleNodeMask = 1;
            return h;
        }

        D3D12_RESOURCE_DESC bufferDesc(UINT64 size)
        {
            D3D12_RESOURCE_DESC d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            d.Width = std::max<UINT64>(size, 1);
            d.Height = 1;
            d.DepthOrArraySize = 1;
            d.MipLevels = 1;
            d.SampleDesc.Count = 1;
            d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            return d;
        }

        // CPU-only descriptors of one type (thread-safe: textures are made on helper threads): heaps of `page`
        // descriptors, freed ones reused
        class DescriptorPool
        {
        public:
            void init(ID3D12Device *device, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT page)
            {
                m_device = device;
                m_type = type;
                m_page = page;
                m_increment = device->GetDescriptorHandleIncrementSize(type);
            }

            D3D12_CPU_DESCRIPTOR_HANDLE alloc()
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_free.empty())
                {
                    D3D12_DESCRIPTOR_HEAP_DESC hd{m_type, m_page, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
                    ComPtr<ID3D12DescriptorHeap> heap;
                    if (FAILED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap))))
                    {
                        std::fprintf(stderr, "[rhi] D3D12 descriptor heap (%u) failed\n", m_page);
                        std::abort();
                    }
                    const SIZE_T base = heap->GetCPUDescriptorHandleForHeapStart().ptr;
                    for (UINT i = m_page; i-- > 0;)
                        m_free.push_back(base + SIZE_T(i) * m_increment);
                    m_heaps.push_back(std::move(heap));
                }
                const SIZE_T h = m_free.back();
                m_free.pop_back();
                return {h};
            }

            void free(D3D12_CPU_DESCRIPTOR_HANDLE h)
            {
                if (!h.ptr)
                    return;
                std::lock_guard<std::mutex> lock(m_mutex);
                m_free.push_back(h.ptr);
            }

        private:
            ID3D12Device *m_device = nullptr;
            D3D12_DESCRIPTOR_HEAP_TYPE m_type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            UINT m_page = 0, m_increment = 0;
            std::mutex m_mutex;
            std::vector<ComPtr<ID3D12DescriptorHeap>> m_heaps;
            std::vector<SIZE_T> m_free;
        };

        // Static buffers' memory: ranges of 32 MB GPU buffers. The native meshes are static buffers, tens of new ones a
        // frame while driving; a committed resource each was a driver allocation each (up to 125 a frame).
        struct BufferPages
        {
            static constexpr UINT64 kPage = 32ull << 20;
            struct Page
            {
                ComPtr<ID3D12Resource> buffer;
                D3D12_GPU_VIRTUAL_ADDRESS va = 0;
                UINT64 size = 0;
                std::map<UINT64, UINT64> free; // offset -> bytes
            };
            std::vector<std::unique_ptr<Page>> pages;
            std::mutex mutex;

            static UINT64 rounded(UINT64 size) { return alignUp(std::max<UINT64>(size, 256), 256); }

            Page *alloc(ID3D12Device *device, UINT64 size, UINT64 &offset)
            {
                size = rounded(size);
                std::lock_guard<std::mutex> lock(mutex);
                for (auto &p : pages)
                    for (auto it = p->free.begin(); it != p->free.end(); ++it)
                        if (it->second >= size)
                        {
                            offset = it->first;
                            const UINT64 rest = it->second - size;
                            p->free.erase(it);
                            if (rest)
                                p->free.emplace(offset + size, rest);
                            return p.get();
                        }
                auto p = std::make_unique<Page>();
                p->size = std::max(kPage, size);
                const D3D12_HEAP_PROPERTIES heap = heapProps(D3D12_HEAP_TYPE_DEFAULT);
                const D3D12_RESOURCE_DESC bd = bufferDesc(p->size);
                if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                           IID_PPV_ARGS(&p->buffer))))
                    return nullptr;
                setName(p->buffer.Get(), "static buffers");
                p->va = p->buffer->GetGPUVirtualAddress();
                offset = 0;
                if (p->size > size)
                    p->free.emplace(size, p->size - size);
                pages.push_back(std::move(p));
                return pages.back().get();
            }

            void release(Page *p, UINT64 offset, UINT64 size)
            {
                UINT64 start = offset, length = rounded(size);
                std::lock_guard<std::mutex> lock(mutex);
                auto next = p->free.lower_bound(start);
                if (next != p->free.begin())
                {
                    auto prev = std::prev(next);
                    if (prev->first + prev->second == start)
                    {
                        start = prev->first;
                        length += prev->second;
                        p->free.erase(prev);
                    }
                }
                if (next != p->free.end() && start + length == next->first)
                {
                    length += next->second;
                    p->free.erase(next);
                }
                p->free.emplace(start, length);
            }
        };

        // The free ranges of a block of memory (offset -> bytes): first fit with an alignment, neighbours joined when freed
        struct Ranges
        {
            std::map<UINT64, UINT64> free;

            void init(UINT64 size)
            {
                free.clear();
                free.emplace(0, size);
            }

            bool alloc(UINT64 size, UINT64 align, UINT64 &offset)
            {
                for (auto it = free.begin(); it != free.end(); ++it)
                {
                    const UINT64 start = it->first, end = it->first + it->second, at = alignUp(start, align);
                    if (at + size > end)
                        continue;
                    free.erase(it);
                    if (at > start)
                        free.emplace(start, at - start);
                    if (at + size < end)
                        free.emplace(at + size, end - at - size);
                    offset = at;
                    return true;
                }
                return false;
            }

            void release(UINT64 offset, UINT64 size)
            {
                UINT64 start = offset, length = size;
                auto next = free.lower_bound(start);
                if (next != free.begin())
                {
                    auto prev = std::prev(next);
                    if (prev->first + prev->second == start)
                    {
                        start = prev->first;
                        length += prev->second;
                        free.erase(prev);
                    }
                }
                if (next != free.end() && start + length == next->first)
                {
                    length += next->second;
                    free.erase(next);
                }
                free.emplace(start, length);
            }
        };

        // Upload memory for textures made on helper threads: ranges of 64 MB mapped buffers, given back after the frame
        // that copies them. An upload buffer of its own per texture was a driver allocation each (cruise.py: a 184-texture
        // burst held the picture 200 ms).
        struct StagingPool
        {
            static constexpr UINT64 kChunk = 64ull << 20;
            struct Chunk
            {
                ComPtr<ID3D12Resource> buffer;
                uint8_t *data = nullptr;
                Ranges ranges;
            };
            std::vector<std::unique_ptr<Chunk>> chunks;
            std::mutex mutex;

            Chunk *alloc(ID3D12Device *device, UINT64 size, UINT64 &offset)
            {
                size = alignUp(std::max<UINT64>(size, 512), 512);
                std::lock_guard<std::mutex> lock(mutex);
                for (auto &c : chunks)
                    if (c->ranges.alloc(size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, offset))
                        return c.get();
                auto c = std::make_unique<Chunk>();
                const UINT64 bytes = std::max(kChunk, size);
                const D3D12_HEAP_PROPERTIES heap = heapProps(D3D12_HEAP_TYPE_UPLOAD);
                const D3D12_RESOURCE_DESC bd = bufferDesc(bytes);
                void *mapped = nullptr;
                if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                           IID_PPV_ARGS(&c->buffer))) ||
                    FAILED(c->buffer->Map(0, nullptr, &mapped)))
                    return nullptr;
                c->data = static_cast<uint8_t *>(mapped);
                c->ranges.init(bytes);
                c->ranges.alloc(size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, offset);
                chunks.push_back(std::move(c));
                return chunks.back().get();
            }

            void release(Chunk *c, UINT64 offset, UINT64 size)
            {
                std::lock_guard<std::mutex> lock(mutex);
                c->ranges.release(offset, alignUp(std::max<UINT64>(size, 512), 512));
            }
        };

        // Sampled textures with first contents (the GS's, texture packs') placed in 128 MB heaps: a committed resource each
        // was ~0.35 ms of a driver allocation. Targets and multisampled textures stay committed (few, made once).
        struct TextureHeaps
        {
            static constexpr UINT64 kHeap = 128ull << 20;
            struct Heap
            {
                ComPtr<ID3D12Heap> heap;
                Ranges ranges;
            };
            std::vector<std::unique_ptr<Heap>> heaps;
            std::mutex mutex;

            Heap *alloc(ID3D12Device *device, UINT64 size, UINT64 align, UINT64 &offset)
            {
                if (size > kHeap / 2)
                    return nullptr; // its own allocation
                std::lock_guard<std::mutex> lock(mutex);
                for (auto &h : heaps)
                    if (h->ranges.alloc(size, align, offset))
                        return h.get();
                auto h = std::make_unique<Heap>();
                D3D12_HEAP_DESC hd{};
                hd.SizeInBytes = kHeap;
                hd.Properties = heapProps(D3D12_HEAP_TYPE_DEFAULT);
                hd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
                hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES; // (resource heap tier 1 needs it)
                if (FAILED(device->CreateHeap(&hd, IID_PPV_ARGS(&h->heap))))
                    return nullptr;
                h->ranges.init(kHeap);
                h->ranges.alloc(size, align, offset);
                heaps.push_back(std::move(h));
                return heaps.back().get();
            }

            void release(Heap *h, UINT64 offset, UINT64 size)
            {
                std::lock_guard<std::mutex> lock(mutex);
                h->ranges.release(offset, size);
            }
        };

        // ---- the device's lifetime: shared by every object made from it ------------------------------------------
        // As rhi_vulkan.cpp's Core: objects may outlive the Device object; they free their D3D12 objects through the
        // frame queues while it lives (after the GPU is done with the frame), at once after (it waited for the GPU).
        struct Core
        {
            ComPtr<ID3D12Device> device;
            DescriptorPool srvs, rtvs, dsvs, samplers;
            BufferPages bufferPages;
            StagingPool staging;
            TextureHeaps textureHeaps;
            bool alive = false;
            uint32_t frame = 0;
            std::array<std::vector<std::function<void()>>, kFrames> garbage;
            std::function<void(const void *)> unbind;
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

            // A frame slot's frees, once the GPU is done with it: to the reaper thread. Releasing a D3D12 resource costs
            // ~0.25 ms in the driver and the texture cache lets go of hundreds at once - cruise.py: 750-3800 objects,
            // 160-200 ms of the main thread in one frame. D3D12 objects are free-threaded; the pools the frees give
            // ranges and descriptors back to take their own locks.
            void collect(uint32_t slot)
            {
                std::vector<std::function<void()>> items;
                items.swap(garbage[slot]);
                if (items.empty())
                    return;
                if (!reaper.joinable())
                {
                    for (auto &f : items)
                        f();
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(reaperMutex);
                    for (auto &f : items)
                        reaperQueue.push_back(std::move(f));
                }
                reaperWake.notify_one();
            }

            void startReaper()
            {
                reaper = std::thread([this] {
                    SetThreadDescription(GetCurrentThread(), L"RhiReaper");
                    std::unique_lock<std::mutex> lock(reaperMutex);
                    for (;;)
                    {
                        reaperWake.wait(lock, [this] { return reaperStop || !reaperQueue.empty(); });
                        if (reaperQueue.empty() && reaperStop)
                            return;
                        std::vector<std::function<void()>> items;
                        items.swap(reaperQueue);
                        lock.unlock();
                        for (auto &f : items)
                            f();
                        lock.lock();
                    }
                });
            }

            ~Core()
            {
                for (uint32_t i = 0; i < kFrames; ++i)
                    collect(i);
                for (auto &f : takeOrphans())
                    f();
                if (reaper.joinable())
                {
                    {
                        std::lock_guard<std::mutex> lock(reaperMutex);
                        reaperStop = true;
                    }
                    reaperWake.notify_one();
                    reaper.join(); // (it empties the queue first)
                }
            }

            std::mutex reaperMutex;
            std::condition_variable reaperWake;
            std::vector<std::function<void()>> reaperQueue;
            bool reaperStop = false;
            std::thread reaper;
        };

        class DxTexture final : public Texture
        {
        public:
            DxTexture(std::shared_ptr<Core> core, const TextureDesc &d) : core(std::move(core)) { m_desc = d; }
            ~DxTexture() override
            {
                core->forget(this);
                Core *c = core.get();
                ComPtr<ID3D12Resource> r = resource;
                std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> views = levelSrv, targets = levelRtv;
                const D3D12_CPU_DESCRIPTOR_HANDLE s = srv, t = rtv, d = dsv;
                TextureHeaps::Heap *heap = placedHeap;
                const UINT64 offset = placedOffset, size = placedSize;
                core->defer([c, r, views, targets, s, t, d, heap, offset, size]() mutable {
                    c->srvs.free(s);
                    c->rtvs.free(t);
                    c->dsvs.free(d);
                    for (auto h : views)
                        c->srvs.free(h);
                    for (auto h : targets)
                        c->rtvs.free(h);
                    r.Reset(); // (before its range is given to another texture)
                    if (heap)
                        c->textureHeaps.release(heap, offset, size);
                });
            }

            std::shared_ptr<Core> core;
            ComPtr<ID3D12Resource> resource;
            D3D12_CPU_DESCRIPTOR_HANDLE srv{}, rtv{}, dsv{};
            std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> levelSrv, levelRtv; // generateMips: one level each (made at first use)
            TextureHeaps::Heap *placedHeap = nullptr; // placed in a texture heap: where
            UINT64 placedOffset = 0, placedSize = 0;
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
            uint32_t mips = 1, samples = 1;
            D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;   // now (every subresource)
            D3D12_RESOURCE_STATES resting = D3D12_RESOURCE_STATE_COMMON; // between operations, outside a pass that targets it
        };

        class DxBuffer final : public Buffer
        {
        public:
            DxBuffer(std::shared_ptr<Core> core, const BufferDesc &d) : core(std::move(core)) { m_desc = d; }
            ~DxBuffer() override
            {
                core->forget(this);
                freeRange();
            }

            // the static range back to its page once the frames that use it are done
            void freeRange()
            {
                if (!page)
                    return;
                Core *c = core.get();
                BufferPages::Page *p = page;
                const UINT64 o = offset, n = bytes;
                core->defer([c, p, o, n] { c->bufferPages.release(p, o, n); });
                page = nullptr;
            }

            std::shared_ptr<Core> core;
            // static buffers (desc.dynamic false): a range of a page in GPU memory (an update takes a new range: the draws
            // recorded before it keep the old contents, as D3D11's UpdateSubresource orders it)
            BufferPages::Page *page = nullptr;
            UINT64 offset = 0, bytes = 0;
            // dynamic buffers: the latest contents and where the current frame has them in its ring
            std::vector<uint8_t> shadow;
            size_t used = 0; // vertex / index buffers: the bytes the last update wrote (the rest is undefined: WRITE_DISCARD)
            D3D12_GPU_VIRTUAL_ADDRESS sliceVa = 0;
            uint64_t sliceFrame = 0; // the frame serial the slice belongs to (0: none yet)
        };

        class DxSampler final : public Sampler
        {
        public:
            explicit DxSampler(std::shared_ptr<Core> core) : core(std::move(core)) {}
            ~DxSampler() override
            {
                core->forget(this);
                Core *c = core.get();
                const D3D12_CPU_DESCRIPTOR_HANDLE h = handle;
                core->defer([c, h] { c->samplers.free(h); });
            }
            std::shared_ptr<Core> core;
            D3D12_CPU_DESCRIPTOR_HANDLE handle{};
        };

        class DxPipeline final : public Pipeline
        {
        public:
            explicit DxPipeline(std::shared_ptr<Core> core) : core(std::move(core)) {}
            ~DxPipeline() override
            {
                core->forget(this);
                std::vector<ComPtr<ID3D12PipelineState>> all;
                for (auto &[key, p] : variants)
                    all.push_back(p);
                core->defer([all]() mutable { all.clear(); });
            }
            std::shared_ptr<Core> core;
            PipelineDesc desc;
            std::string name;
            std::vector<uint8_t> vs, ps;
            std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> variants; // by target formats / samples
            D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        };

        // A frame's upload memory: mapped chunks, reset when the frame slot comes round again
        struct Ring
        {
            struct Chunk
            {
                ComPtr<ID3D12Resource> buffer;
                uint8_t *data = nullptr;
                D3D12_GPU_VIRTUAL_ADDRESS va = 0;
                UINT64 size = 0, used = 0;
            };
            std::vector<Chunk> chunks;
            size_t current = 0;
        };

        struct Slice
        {
            ID3D12Resource *buffer = nullptr;
            UINT64 offset = 0;
            uint8_t *data = nullptr;
            D3D12_GPU_VIRTUAL_ADDRESS va = 0;
        };

        // eight descriptors of a table (textures or samplers), as a hash key
        struct Table
        {
            SIZE_T h[kTextureSlots] = {};
            bool operator==(const Table &o) const { return std::memcmp(h, o.h, sizeof(h)) == 0; }
        };
        struct TableHash
        {
            size_t operator()(const Table &t) const
            {
                uint64_t x = 0xCBF29CE484222325ull;
                for (SIZE_T v : t.h)
                    x = (x ^ v) * 0x100000001B3ull;
                return static_cast<size_t>(x ^ (x >> 29));
            }
        };

        struct FrameSlot
        {
            std::vector<ComPtr<ID3D12CommandAllocator>> allocators;
            size_t used = 0;
            UINT64 fence = 0; // the value its last submit signals
            Ring ring;
            UINT viewsUsed = 0; // in its part of the shader-visible SRV heap
            std::unordered_map<Table, UINT, TableHash> tables;
        };

        class D3D12Device final : public Device
        {
        public:
            D3D12Device() : m_core(std::make_shared<Core>()) {}

            bool create(const DeviceDesc &desc, std::string &error)
            {
                m_hwnd = static_cast<HWND>(desc.nativeWindow);
                UINT factoryFlags = 0;
                if (desc.debug)
                {
                    ComPtr<ID3D12Debug> debug;
                    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
                    {
                        debug->EnableDebugLayer();
                        factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
                    }
                    else
                        std::fprintf(stderr, "[rhi] D3D12 debug layer not available\n");
                }
                if (FAILED(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory))))
                {
                    error = "CreateDXGIFactory2 failed";
                    return false;
                }
                // the high-performance GPU first (a laptop's discrete one), hardware only
                ComPtr<IDXGIFactory6> factory6;
                ComPtr<IDXGIAdapter1> adapter;
                auto tryAdapter = [&](IDXGIAdapter1 *a) {
                    DXGI_ADAPTER_DESC1 ad{};
                    a->GetDesc1(&ad);
                    if (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                        return false;
                    if (FAILED(D3D12CreateDevice(a, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_core->device))))
                        return false;
                    m_adapterName = narrow(ad.Description);
                    return true;
                };
                if (SUCCEEDED(m_factory.As(&factory6)))
                    for (UINT i = 0; !m_core->device &&
                                     factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) !=
                                         DXGI_ERROR_NOT_FOUND;
                         ++i)
                        tryAdapter(adapter.Get());
                for (UINT i = 0; !m_core->device && m_factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
                    tryAdapter(adapter.Get());
                if (!m_core->device)
                {
                    error = "no D3D12 device (feature level 11_0)";
                    return false;
                }
                ID3D12Device *device = m_core->device.Get();
                if (desc.debug)
                    m_core->device.As(&m_infoQueue); // the debug layer's messages, printed at each present
                const D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
                if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_queue))) ||
                    FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence))))
                {
                    error = "D3D12 queue / fence failed";
                    return false;
                }
                m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                m_core->srvs.init(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4096);
                m_core->rtvs.init(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 512);
                m_core->dsvs.init(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 128);
                m_core->samplers.init(device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 256);
                if (!createRootSignature(error) || !createHeaps(error))
                    return false;

                // what an empty binding reads: a null texture (zeros), point sampling, a zero constant buffer
                m_nullSrv = m_core->srvs.alloc();
                D3D12_SHADER_RESOURCE_VIEW_DESC nv{};
                nv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                nv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                nv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                nv.Texture2D.MipLevels = 1;
                device->CreateShaderResourceView(nullptr, &nv, m_nullSrv);
                m_defaultSampler = createSampler({});
                SamplerDesc linear;
                linear.filter = Filter::Linear;
                m_linearSampler = createSampler(linear);
                const D3D12_HEAP_PROPERTIES upload = heapProps(D3D12_HEAP_TYPE_UPLOAD);
                const D3D12_RESOURCE_DESC zd = bufferDesc(65536);
                if (FAILED(device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &zd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                           IID_PPV_ARGS(&m_zeroBuffer))))
                {
                    error = "D3D12 zero buffer failed";
                    return false;
                }
                void *zero = nullptr;
                if (SUCCEEDED(m_zeroBuffer->Map(0, nullptr, &zero)))
                {
                    std::memset(zero, 0, 65536);
                    m_zeroBuffer->Unmap(0, nullptr);
                }

                ComPtr<IDXGIFactory5> factory5;
                BOOL tearing = FALSE;
                if (SUCCEEDED(m_factory.As(&factory5)))
                    factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
                m_swapFlags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;
                DXGI_SWAP_CHAIN_DESC1 sc{};
                sc.Width = std::max(1u, desc.width);
                sc.Height = std::max(1u, desc.height);
                sc.Format = kBackbufferFormat;
                sc.SampleDesc.Count = 1;
                sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                sc.BufferCount = kBackbuffers;
                sc.Scaling = DXGI_SCALING_STRETCH;
                sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                sc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
                sc.Flags = m_swapFlags;
                ComPtr<IDXGISwapChain1> swapchain1;
                HRESULT hr = m_factory->CreateSwapChainForHwnd(m_queue.Get(), m_hwnd, &sc, nullptr, nullptr, &swapchain1);
                if (FAILED(hr) || FAILED(swapchain1.As(&m_swapchain)))
                {
                    error = "CreateSwapChainForHwnd failed (" + hex(hr) + ")";
                    return false;
                }
                m_factory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER);
                m_width = sc.Width;
                m_height = sc.Height;
                for (UINT i = 0; i < kBackbuffers; ++i)
                    m_backRtv[i] = m_core->rtvs.alloc();
                if (!makeBackbufferViews())
                {
                    error = "no D3D12 back buffers";
                    return false;
                }
                m_core->alive = true;
                m_core->unbind = [this](const void *object) { unbind(object); };
                m_core->startReaper();
                beginFrame();
                return true;
            }

            ~D3D12Device() override
            {
                if (m_recording)
                {
                    endPassInternal();
                    backToPresent();
                    flushBarriers();
                    submit(false);
                }
                waitIdle();
                m_core->alive = false;
                m_core->unbind = nullptr;
                for (uint32_t i = 0; i < kFrames; ++i)
                    m_core->collect(i);
                for (auto &f : m_core->takeOrphans())
                    f();
                for (UINT i = 0; i < kBackbuffers; ++i)
                    m_core->rtvs.free(m_backRtv[i]);
                m_core->srvs.free(m_nullSrv);
                if (m_fenceEvent)
                    CloseHandle(m_fenceEvent);
            }

            Backend backend() const override { return Backend::D3D12; }
            const std::string &adapterName() const override { return m_adapterName; }

            // ---- resources ----------------------------------------------------------------------------------------
            bool freeThreadedCreation() const override { return true; } // off the owner thread: stage()

            std::unique_ptr<Texture> createTexture(const TextureDesc &desc, const void *data, uint32_t rowPitch) override
            {
                const bool copied = data != nullptr;
                auto t = makeTexture(desc, copied);
                if (!t)
                    return nullptr;
                const uint32_t w = t->desc().width;
                const void *levels[1] = {data};
                const uint32_t pitches[1] = {rowPitch ? rowPitch : w * 4u};
                if (!m_core->onOwner()) // a helper thread (freeThreadedCreation): one level with data, sampled, no target
                {
                    if (!data || t->mips > 1u || (desc.usage & (UsageRenderTarget | UsageDepth)) || !stage(*t, levels, pitches, 1u))
                    {
                        std::fprintf(stderr, "[rhi] createTexture %ux%u off the render thread: not a sampled texture with data\n", w,
                                     t->desc().height);
                        return nullptr;
                    }
                    return t;
                }
                if (copied)
                {
                    // the data in the set-up commands (before anything recorded so far can use it), then the chain
                    ID3D12GraphicsCommandList *cmd = initCmd();
                    uploadLevels(cmd, *t, levels, pitches, 1u);
                    stateBarrier(cmd, *t, D3D12_RESOURCE_STATE_COPY_DEST, t->resting);
                    t->state = t->resting;
                    if (t->mips > 1u)
                        mipChain(*t);
                }
                return t;
            }

            void updateTexture(Texture &texture, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const void *data,
                               uint32_t rowPitch) override
            {
                auto &t = static_cast<DxTexture &>(texture);
                if (!width || !height || !data)
                    return;
                const Format f = t.desc().format;
                const uint32_t row = rowBytes(f, width), rows = rowCount(f, height);
                const uint32_t pitch = rowPitch ? rowPitch : row;
                const UINT64 dstPitch = alignUp(row, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
                const Slice s = ringAlloc(dstPitch * rows, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
                for (uint32_t i = 0; i < rows; ++i)
                    std::memcpy(s.data + i * dstPitch, static_cast<const uint8_t *>(data) + size_t(i) * pitch, row);
                transition(t, D3D12_RESOURCE_STATE_COPY_DEST);
                flushBarriers();
                ++g_stats.uploads;
                g_stats.uploadBytes += dstPitch * rows;
                D3D12_TEXTURE_COPY_LOCATION dst{t.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
                dst.SubresourceIndex = 0;
                D3D12_TEXTURE_COPY_LOCATION src{s.buffer, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
                src.PlacedFootprint.Offset = s.offset;
                src.PlacedFootprint.Footprint = {t.format, blockCompressed(f) ? (width + 3u) & ~3u : width,
                                                 blockCompressed(f) ? (height + 3u) & ~3u : height, 1, static_cast<UINT>(dstPitch)};
                m_cmd->CopyTextureRegion(&dst, x, y, 0, &src, nullptr);
                transition(t, homeState(t));
            }

            bool readTexture(Texture &texture, uint32_t x, uint32_t y, uint32_t width, uint32_t height, void *out,
                             uint32_t rowPitch) override
            {
                auto &t = static_cast<DxTexture &>(texture);
                if (!out || !width || !height || t.samples != 1u)
                    return false;
                const UINT64 pitch = alignUp(UINT64(width) * 4u, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
                // one read-back buffer, kept and grown (the rig's brightness probe reads every frame: a buffer made and
                // freed each time was a third of the main thread)
                if (!m_readback || m_readbackSize < pitch * height)
                {
                    if (ComPtr<ID3D12Resource> old = m_readback)
                        m_core->defer([old]() mutable { old.Reset(); });
                    m_readback.Reset();
                    m_readbackSize = alignUp(pitch * height, 65536);
                    const D3D12_HEAP_PROPERTIES readback = heapProps(D3D12_HEAP_TYPE_READBACK);
                    const D3D12_RESOURCE_DESC bd = bufferDesc(m_readbackSize);
                    if (FAILED(m_core->device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                                       nullptr, IID_PPV_ARGS(&m_readback))))
                    {
                        m_readbackSize = 0;
                        return false;
                    }
                }
                ID3D12Resource *staging = m_readback.Get();
                transition(t, D3D12_RESOURCE_STATE_COPY_SOURCE);
                flushBarriers();
                D3D12_TEXTURE_COPY_LOCATION dst{staging, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
                dst.PlacedFootprint.Offset = 0;
                dst.PlacedFootprint.Footprint = {t.format, width, height, 1, static_cast<UINT>(pitch)};
                D3D12_TEXTURE_COPY_LOCATION src{t.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
                src.SubresourceIndex = 0;
                const D3D12_BOX box{x, y, 0, x + width, y + height, 1};
                m_cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
                transition(t, homeState(t));
                flushBarriers();
                flushAndWait();
                void *mapped = nullptr;
                const D3D12_RANGE range{0, static_cast<SIZE_T>(pitch * height)};
                if (FAILED(staging->Map(0, &range, &mapped)))
                    return false;
                const uint32_t outPitch = rowPitch ? rowPitch : width * 4u;
                for (uint32_t row = 0; row < height; ++row)
                    std::memcpy(static_cast<uint8_t *>(out) + size_t(row) * outPitch, static_cast<const uint8_t *>(mapped) + row * pitch,
                                width * 4u);
                const D3D12_RANGE none{0, 0};
                staging->Unmap(0, &none);
                return true;
            }

            void copyTexture(Texture &dst, uint32_t dstX, uint32_t dstY, Texture &src, uint32_t srcX, uint32_t srcY, uint32_t width,
                             uint32_t height) override
            {
                auto &d = static_cast<DxTexture &>(dst);
                auto &s = static_cast<DxTexture &>(src);
                if (!width || !height || &d == &s)
                    return;
                transition(s, D3D12_RESOURCE_STATE_COPY_SOURCE);
                transition(d, D3D12_RESOURCE_STATE_COPY_DEST);
                flushBarriers();
                D3D12_TEXTURE_COPY_LOCATION to{d.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
                to.SubresourceIndex = 0;
                D3D12_TEXTURE_COPY_LOCATION from{s.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
                from.SubresourceIndex = 0;
                const D3D12_BOX box{srcX, srcY, 0, srcX + width, srcY + height, 1};
                m_cmd->CopyTextureRegion(&to, dstX, dstY, 0, &from, &box);
                transition(s, homeState(s));
                transition(d, homeState(d));
            }

            void resolveTexture(Texture &dst, Texture &src) override
            {
                auto &d = static_cast<DxTexture &>(dst);
                auto &s = static_cast<DxTexture &>(src);
                if (s.samples == 1u || s.desc().format == Format::D32F)
                    return;
                transition(s, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
                transition(d, D3D12_RESOURCE_STATE_RESOLVE_DEST);
                flushBarriers();
                m_cmd->ResolveSubresource(d.resource.Get(), 0, s.resource.Get(), 0, s.format);
                transition(s, homeState(s));
                transition(d, homeState(d));
            }

            void generateMips(Texture &texture) override
            {
                auto &t = static_cast<DxTexture &>(texture);
                if (t.mips > 1u)
                    mipChain(t);
            }

            std::unique_ptr<Texture> createTextureLevels(const TextureDesc &desc, const void *const *levels, uint32_t levelCount) override
            {
                TextureDesc d = desc;
                d.usage = UsageSampled;
                d.samples = 1;
                d.mipLevels = std::max(1u, std::min(levelCount, 16u));
                auto t = makeTexture(d, true);
                if (!t)
                    return nullptr;
                uint32_t pitches[16];
                const uint32_t n = std::min(t->mips, std::min(levelCount, 16u));
                for (uint32_t i = 0; i < n; ++i)
                    pitches[i] = rowBytes(d.format, std::max(1u, t->desc().width >> i));
                if (!m_core->onOwner()) // a helper thread (freeThreadedCreation): every level staged
                {
                    if (!stage(*t, levels, pitches, n))
                        return nullptr;
                    return t;
                }
                ID3D12GraphicsCommandList *cmd = initCmd();
                uploadLevels(cmd, *t, levels, pitches, n);
                stateBarrier(cmd, *t, D3D12_RESOURCE_STATE_COPY_DEST, t->resting);
                t->state = t->resting;
                return t;
            }

            std::unique_ptr<Buffer> createBuffer(const BufferDesc &desc, const void *data) override
            {
                auto b = std::make_unique<DxBuffer>(m_core, desc);
                const size_t size = std::max<size_t>(desc.size, 4);
                if (desc.dynamic)
                {
                    // a constant buffer's root view covers 256-byte units: its slices are the whole rounded size
                    b->shadow.assign(desc.usage == BufferUsage::Constant ? alignUp(size, 256) : size, 0);
                    b->used = b->shadow.size();
                    if (data)
                        std::memcpy(b->shadow.data(), data, desc.size);
                    return b;
                }
                if (!makeStatic(*b, data, desc.size))
                    return nullptr;
                return b;
            }

            void updateBuffer(Buffer &buffer, const void *data, size_t size) override
            {
                auto &b = static_cast<DxBuffer &>(buffer);
                size = std::min(size, b.desc().size);
                if (!data || !size)
                    return;
                if (b.desc().dynamic)
                {
                    // WRITE_DISCARD: the rest of the buffer is undefined in D3D; ours keeps the old bytes. A vertex / index
                    // buffer's slice holds only what was written (rhi_vulkan.cpp, PERFORMANCE.md 27); a constant buffer's
                    // is whole
                    std::memcpy(b.shadow.data(), data, size);
                    b.used = b.desc().usage == BufferUsage::Constant ? b.shadow.size() : std::max<size_t>(size, 4);
                    b.sliceFrame = 0; // a new slice at the next use
                    if (&b == m_vertexBuffer || &b == m_indexBuffer)
                        m_buffersDirty = true;
                    return;
                }
                // a static buffer updated (the renderer never does; its meshes are made once): a new resource with these
                // contents, zeros after them - the draws recorded before keep reading the old one
                std::vector<uint8_t> whole(b.desc().size, 0);
                std::memcpy(whole.data(), data, size);
                b.freeRange();
                makeStatic(b, whole.data(), whole.size());
                if (&b == m_vertexBuffer || &b == m_indexBuffer)
                    m_buffersDirty = true;
            }

            std::unique_ptr<Sampler> createSampler(const SamplerDesc &desc) override
            {
                auto s = std::make_unique<DxSampler>(m_core);
                D3D12_SAMPLER_DESC sd{};
                const bool linear = desc.filter == Filter::Linear;
                sd.Filter = linear ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
                if (linear && desc.anisotropy > 1u)
                {
                    sd.Filter = D3D12_FILTER_ANISOTROPIC;
                    sd.MaxAnisotropy = std::min(desc.anisotropy, 16u);
                }
                sd.AddressU = desc.wrapU == Wrap::Repeat ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                sd.AddressV = desc.wrapV == Wrap::Repeat ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
                sd.MaxLOD = D3D12_FLOAT32_MAX;
                s->handle = m_core->samplers.alloc();
                m_core->device->CreateSampler(&sd, s->handle);
                return s;
            }

            std::unique_ptr<Pipeline> createPipeline(const PipelineDesc &desc) override
            {
                if (!desc.vertex.data || !desc.pixel.data || !desc.vertex.size || !desc.pixel.size)
                    return nullptr;
                auto p = std::make_unique<DxPipeline>(m_core);
                p->desc = desc;
                p->name = desc.name ? desc.name : "";
                p->desc.name = nullptr;
                const auto *v = static_cast<const uint8_t *>(desc.vertex.data);
                const auto *f = static_cast<const uint8_t *>(desc.pixel.data);
                p->vs.assign(v, v + desc.vertex.size);
                p->ps.assign(f, f + desc.pixel.size);
                p->desc.vertex = {p->vs.data(), p->vs.size()};
                p->desc.pixel = {p->ps.data(), p->ps.size()};
                p->topology = topology(desc.topology);
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
                m_passDepth = pass.depth ? static_cast<DxTexture *>(pass.depth) : nullptr;
                m_passToBack = false;
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
                    m_passColor = static_cast<DxTexture *>(pass.color);
                    w = m_passColor->desc().width;
                    h = m_passColor->desc().height;
                }
                else
                {
                    m_passToBack = true;
                    m_backIndex = m_swapchain->GetCurrentBackBufferIndex();
                    if (m_backState[m_backIndex] != D3D12_RESOURCE_STATE_RENDER_TARGET)
                    {
                        barrier(m_backbuffers[m_backIndex].Get(), m_backState[m_backIndex], D3D12_RESOURCE_STATE_RENDER_TARGET);
                        m_backState[m_backIndex] = D3D12_RESOURCE_STATE_RENDER_TARGET;
                    }
                }
                m_passWidth = w;
                m_passHeight = h;
                m_inPass = true;
                ++g_stats.passes;
                if (m_passColor)
                    transition(*m_passColor, D3D12_RESOURCE_STATE_RENDER_TARGET);
                if (m_passDepth)
                    transition(*m_passDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                flushBarriers();
                bindPassTargets();
                if (pass.clearColor && (m_passColor || m_passToBack))
                    m_cmd->ClearRenderTargetView(m_passToBack ? m_backRtv[m_backIndex] : m_passColor->rtv, pass.clearRgba, 0, nullptr);
                if (pass.clearDepth && m_passDepth)
                    m_cmd->ClearDepthStencilView(m_passDepth->dsv, D3D12_CLEAR_FLAG_DEPTH, pass.clearDepthValue, 0, 0, nullptr);
                // as D3D11's beginPass: textures unbound, viewport and scissor the whole target
                for (uint32_t i = 0; i < kTextureSlots; ++i)
                {
                    m_textures[i] = nullptr;
                    m_samplers[i] = nullptr;
                }
                m_texturesDirty = true;
                setViewport(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
                setScissor(0, 0, static_cast<int>(w), static_cast<int>(h));
                if (!m_passName.empty())
                    pushMarker(m_passName.c_str());
                m_passMarker = !m_passName.empty();
            }

            void endPass() override { endPassInternal(); }

            void setViewport(float x, float y, float width, float height) override
            {
                m_viewport = {x, y, width, height, 0.0f, 1.0f};
                m_viewportDirty = true;
            }

            void setScissor(int x, int y, int width, int height) override
            {
                m_scissor = {x, y, x + std::max(width, 0), y + std::max(height, 0)};
                m_scissorDirty = true;
            }

            void setBlendConstant(const float rgba[4]) override
            {
                std::memcpy(m_blend, rgba, sizeof(m_blend));
                m_blendDirty = true;
            }

            void setPipeline(Pipeline &pipeline) override { m_pipeline = static_cast<DxPipeline *>(&pipeline); }

            void setVertexBuffer(Buffer &buffer, uint32_t offset) override
            {
                m_vertexBuffer = static_cast<DxBuffer *>(&buffer);
                m_vertexOffset = offset;
                m_buffersDirty = true;
            }

            void setIndexBuffer(Buffer &buffer, bool uint32Indices) override
            {
                m_indexBuffer = static_cast<DxBuffer *>(&buffer);
                m_index32 = uint32Indices;
                m_buffersDirty = true;
            }

            void setTexture(uint32_t slot, Texture *texture, Sampler *sampler) override
            {
                if (slot >= kTextureSlots)
                    return;
                m_textures[slot] = static_cast<DxTexture *>(texture);
                m_samplers[slot] = static_cast<DxSampler *>(sampler);
                m_texturesDirty = true;
            }

            void setConstants(uint32_t slot, Buffer &buffer) override
            {
                if (slot < kUniformSlots)
                    m_uniforms[slot] = static_cast<DxBuffer *>(&buffer);
            }

            void draw(uint32_t vertexCount, uint32_t firstVertex) override
            {
                if (vertexCount && prepareDraw(false))
                {
                    m_cmd->DrawInstanced(vertexCount, 1, firstVertex, 0);
                    ++g_stats.draws;
                }
            }

            void drawIndexed(uint32_t indexCount, uint32_t firstIndex, int32_t baseVertex) override
            {
                if (indexCount && prepareDraw(true))
                {
                    m_cmd->DrawIndexedInstanced(indexCount, 1, firstIndex, baseVertex, 0);
                    ++g_stats.draws;
                }
            }

            void pushMarker(const char *name) override
            {
                if (!name || !m_recording)
                    return;
                m_cmd->BeginEvent(1, name, static_cast<UINT>(std::strlen(name) + 1)); // 1: an ANSI string (PIX's encoding)
                ++m_markerDepth;
            }

            void popMarker() override
            {
                if (m_markerDepth <= 0 || !m_recording)
                    return;
                m_cmd->EndEvent();
                --m_markerDepth;
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
                // the work so far runs to the end, the buffers are made again, recording goes on
                endPassInternal();
                backToPresent();
                flushBarriers();
                submit(false);
                waitIdle();
                for (UINT i = 0; i < kBackbuffers; ++i)
                    m_backbuffers[i].Reset();
                if (FAILED(m_swapchain->ResizeBuffers(kBackbuffers, width, height, DXGI_FORMAT_UNKNOWN, m_swapFlags)))
                    std::fprintf(stderr, "[rhi] D3D12 ResizeBuffers %ux%u failed\n", width, height);
                else
                {
                    m_width = width;
                    m_height = height;
                }
                makeBackbufferViews();
                beginCommands();
            }

            void present(PresentMode mode) override
            {
                endPassInternal();
                backToPresent();
                flushBarriers();
                submit(true);
                const bool tearing = mode == PresentMode::Tearing && (m_swapFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
                HRESULT hr;
                {
                    Span span(t_spans.present);
                    hr = m_swapchain->Present(mode == PresentMode::Vsync ? 1u : 0u, tearing ? DXGI_PRESENT_ALLOW_TEARING : 0u);
                }
                if ((hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) && !m_lostReported)
                {
                    m_lostReported = true;
                    std::fprintf(stderr, "[rhi] graphics device lost (0x%08X, reason 0x%08X)\n", static_cast<unsigned>(hr),
                                 static_cast<unsigned>(m_core->device->GetDeviceRemovedReason()));
                }
                printDebugMessages();
                if (statsOn())
                    reportStats();
                beginFrame();
                reportSpike();
            }

            // ---- Dear ImGui: drawn through the RHI by render::UiRenderer (as Vulkan) ---------------------------------
            bool uiInit() override { return false; }
            void uiNewFrame() override {}
            void uiRender(void *) override {}
            void uiShutdown() override {}

        private:
            // after the frame's beginFrame (its fence wait counted)
            void reportSpike()
            {
                const auto now = std::chrono::steady_clock::now();
                const double span = std::chrono::duration<double, std::milli>(now - m_spikeFrameEnd).count();
                const Spans f = t_spans;
                t_spans = {};
                const bool known = m_spikeFrameEnd.time_since_epoch().count() != 0;
                m_spikeFrameEnd = now;
                if (spikeMs() <= 0.0 || !known || span <= spikeMs())
                    return;
                std::fprintf(stderr,
                             "[rhi-spike] d3d12 frame %.1f ms: pipelines %u (%.1f ms), textures %u (%.1f ms), buffers %u (%.1f ms), "
                             "memory %u (%.1f ms), GPU waits %u (%.1f ms, %u flushes), present %.1f ms, freed %u (%.1f ms)\n",
                             span, f.psos, f.pso, f.textures, f.texture, f.buffers, f.buffer, f.memories, f.memory, f.waits, f.wait,
                             f.flushes, f.present, f.collected, f.collect);
            }

            void reportStats()
            {
                ++g_stats.frames;
                static auto last = std::chrono::steady_clock::now();
                const auto now = std::chrono::steady_clock::now();
                if (now - last < std::chrono::seconds(5))
                    return;
                last = now;
                const double f = static_cast<double>(std::max<uint64_t>(1, g_stats.frames.exchange(0)));
                std::fprintf(stderr,
                             "[rhi-stats] d3d12 per frame: %.0f draws, %.0f passes, %.0f barriers (%.0f calls), %.1f texture uploads "
                             "(%.0f KB), %.2f textures made, %.2f buffers made, %.2f pipelines made, %.0f tables, %.2f fence waits\n",
                             g_stats.draws.exchange(0) / f, g_stats.passes.exchange(0) / f, g_stats.barriers.exchange(0) / f,
                             g_stats.barrierCalls.exchange(0) / f, g_stats.uploads.exchange(0) / f, g_stats.uploadBytes.exchange(0) / f / 1024.0,
                             g_stats.textures.exchange(0) / f, g_stats.buffers.exchange(0) / f, g_stats.psos.exchange(0) / f,
                             g_stats.tables.exchange(0) / f, g_stats.waits.exchange(0) / f);
            }

            // PS2X_RHI_DEBUG=1: the debug layer's warnings and errors into the log (the first 500)
            void printDebugMessages()
            {
                if (!m_infoQueue)
                    return;
                const UINT64 n = m_infoQueue->GetNumStoredMessages();
                for (UINT64 i = 0; i < n && m_debugPrinted < 500u; ++i)
                {
                    SIZE_T size = 0;
                    if (FAILED(m_infoQueue->GetMessage(i, nullptr, &size)) || !size)
                        continue;
                    std::vector<uint8_t> buffer(size);
                    auto *m = reinterpret_cast<D3D12_MESSAGE *>(buffer.data());
                    if (FAILED(m_infoQueue->GetMessage(i, m, &size)) || m->Severity > D3D12_MESSAGE_SEVERITY_WARNING ||
                        std::strstr(m->pDescription, "did not pass any clear value")) // (clears to any colour: no use)
                        continue;
                    std::fprintf(stderr, "[rhi] d3d12 %s: %s\n", m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR ? "error" : "warning",
                                 m->pDescription);
                    ++m_debugPrinted;
                }
                m_infoQueue->ClearStoredMessages();
            }

            // ---- set-up -------------------------------------------------------------------------------------------
            bool createRootSignature(std::string &error)
            {
                D3D12_DESCRIPTOR_RANGE textures{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kTextureSlots, 0, 0, 0};
                D3D12_DESCRIPTOR_RANGE samplers{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, kTextureSlots, 0, 0, 0};
                D3D12_ROOT_PARAMETER params[kUniformSlots + 2]{};
                for (uint32_t i = 0; i < kUniformSlots; ++i)
                {
                    params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
                    params[i].Descriptor.ShaderRegister = i;
                    params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
                }
                params[kRootTextures].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                params[kRootTextures].DescriptorTable = {1, &textures};
                params[kRootTextures].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                params[kRootSamplers].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                params[kRootSamplers].DescriptorTable = {1, &samplers};
                params[kRootSamplers].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                const D3D12_ROOT_SIGNATURE_DESC rd{kUniformSlots + 2, params, 0, nullptr,
                                                   D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
                ComPtr<ID3DBlob> blob, errors;
                if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
                    FAILED(m_core->device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_root))))
                {
                    error = "D3D12 root signature failed";
                    if (errors)
                        error += std::string(": ") + static_cast<const char *>(errors->GetBufferPointer());
                    return false;
                }
                return true;
            }

            bool createHeaps(std::string &error)
            {
                ID3D12Device *device = m_core->device.Get();
                const D3D12_DESCRIPTOR_HEAP_DESC vd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kViewsPerFrame * kFrames,
                                                    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
                const D3D12_DESCRIPTOR_HEAP_DESC sd{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerHeap, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
                if (FAILED(device->CreateDescriptorHeap(&vd, IID_PPV_ARGS(&m_views))) ||
                    FAILED(device->CreateDescriptorHeap(&sd, IID_PPV_ARGS(&m_samplerHeap))))
                {
                    error = "D3D12 shader-visible descriptor heaps failed";
                    return false;
                }
                m_viewIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                m_samplerIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
                return true;
            }

            bool makeBackbufferViews()
            {
                for (UINT i = 0; i < kBackbuffers; ++i)
                {
                    if (FAILED(m_swapchain->GetBuffer(i, IID_PPV_ARGS(&m_backbuffers[i]))))
                    {
                        std::fprintf(stderr, "[rhi] D3D12 back buffer %u missing\n", i);
                        return false;
                    }
                    m_core->device->CreateRenderTargetView(m_backbuffers[i].Get(), nullptr, m_backRtv[i]);
                    m_backState[i] = D3D12_RESOURCE_STATE_PRESENT;
                }
                return true;
            }

            // The resource and its views, no contents yet. Thread-safe: only device calls and the descriptor pools.
            // `copied`: first contents come by a copy (made in COPY_DEST), else made in the state it rests in.
            std::unique_ptr<DxTexture> makeTexture(const TextureDesc &desc, bool copied)
            {
                TextureDesc sized = desc;
                sized.width = std::max(1u, desc.width);
                sized.height = std::max(1u, desc.height);
                auto t = std::make_unique<DxTexture>(m_core, sized);
                const uint32_t w = sized.width, h = sized.height;
                const bool msaa = desc.samples > 1u;
                t->samples = msaa ? desc.samples : 1u;
                t->mips = 1;
                if (desc.mipLevels != 1u && !msaa)
                {
                    uint32_t full = 1;
                    for (uint32_t s = std::max(w, h); s > 1u; s >>= 1)
                        ++full;
                    t->mips = desc.mipLevels == 0 ? full : std::min(desc.mipLevels, full);
                }
                t->format = resourceFormat(desc.format);
                const bool sampled = (desc.usage & UsageSampled) || t->mips > 1u;
                // a chain generateMips may draw: its levels are targets (never block-compressed: those come as data)
                const bool drawnChain = t->mips > 1u && !blockCompressed(desc.format);
                D3D12_RESOURCE_DESC rd{};
                rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                rd.Width = w;
                rd.Height = h;
                rd.DepthOrArraySize = 1;
                rd.MipLevels = static_cast<UINT16>(t->mips);
                rd.Format = t->format;
                rd.SampleDesc.Count = t->samples;
                rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
                if ((desc.usage & UsageRenderTarget) || drawnChain)
                    rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                if (desc.usage & UsageDepth)
                    rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
                t->resting = sampled                             ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                             : (desc.usage & UsageRenderTarget) ? D3D12_RESOURCE_STATE_RENDER_TARGET
                             : (desc.usage & UsageDepth)        ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                                                                : D3D12_RESOURCE_STATE_COMMON;
                t->state = copied ? D3D12_RESOURCE_STATE_COPY_DEST : t->resting;
                const D3D12_HEAP_PROPERTIES heap = heapProps(D3D12_HEAP_TYPE_DEFAULT);
                HRESULT hr = E_FAIL;
                {
                    Span span(t_spans.texture);
                    ++t_spans.textures;
                    ID3D12Device *dev = m_core->device.Get();
                    // placed when it is a plain sampled texture whose contents come by a copy (placed memory starts
                    // undefined): 4 KB alignment for the small ones (<= 64 KB top mip per D3D12 spec), else 64 KB
                    if (copied && !(rd.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) &&
                        t->samples == 1u)
                    {
                        const UINT64 topMipBytes = static_cast<UINT64>(rowBytes(desc.format, w)) * rowCount(desc.format, h);
                        if (topMipBytes <= 65536ull)
                            rd.Alignment = D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;
                        else
                            rd.Alignment = 0;
                        D3D12_RESOURCE_ALLOCATION_INFO info = dev->GetResourceAllocationInfo(0, 1, &rd);
                        if (rd.Alignment != 0 && info.Alignment != D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT)
                        {
                            rd.Alignment = 0;
                            info = dev->GetResourceAllocationInfo(0, 1, &rd);
                        }
                        UINT64 offset = 0;
                        if (info.SizeInBytes != UINT64_MAX)
                            if (TextureHeaps::Heap *h = m_core->textureHeaps.alloc(dev, info.SizeInBytes, info.Alignment, offset))
                            {
                                hr = dev->CreatePlacedResource(h->heap.Get(), offset, &rd, t->state, nullptr, IID_PPV_ARGS(&t->resource));
                                if (SUCCEEDED(hr))
                                {
                                    t->placedHeap = h;
                                    t->placedOffset = offset;
                                    t->placedSize = info.SizeInBytes;
                                }
                                else
                                    m_core->textureHeaps.release(h, offset, info.SizeInBytes);
                            }
                        rd.Alignment = 0;
                    }
                    if (!t->resource)
                        hr = dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, t->state, nullptr, IID_PPV_ARGS(&t->resource));
                }
                if (FAILED(hr))
                {
                    std::fprintf(stderr, "[rhi] D3D12 texture %ux%u failed (%s)\n", w, h, hex(hr).c_str());
                    return nullptr;
                }
                setName(t->resource.Get(), desc.name);
                ++g_stats.textures;
                static const bool logCreates = std::getenv("PS2X_RHI_LOG_CREATES") != nullptr;
                if (logCreates && w * h >= 256u * 256u)
                {
                    static std::atomic<uint64_t> created{0};
                    if (++created <= 2000u)
                        std::fprintf(stderr, "[rhi] create %ux%u x%u '%s'\n", w, h, t->samples, desc.name ? desc.name : "");
                }
                ID3D12Device *device = m_core->device.Get();
                if (sampled)
                {
                    t->srv = m_core->srvs.alloc();
                    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
                    sv.Format = viewFormat(desc.format);
                    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    if (msaa)
                        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
                    else
                    {
                        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                        sv.Texture2D.MipLevels = t->mips;
                    }
                    device->CreateShaderResourceView(t->resource.Get(), &sv, t->srv);
                }
                if (desc.usage & UsageRenderTarget)
                {
                    t->rtv = m_core->rtvs.alloc();
                    D3D12_RENDER_TARGET_VIEW_DESC rv{};
                    rv.Format = t->format;
                    rv.ViewDimension = msaa ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
                    device->CreateRenderTargetView(t->resource.Get(), &rv, t->rtv);
                }
                if (desc.usage & UsageDepth)
                {
                    t->dsv = m_core->dsvs.alloc();
                    D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
                    dv.Format = DXGI_FORMAT_D32_FLOAT;
                    dv.ViewDimension = msaa ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
                    device->CreateDepthStencilView(t->resource.Get(), &dv, t->dsv);
                }
                return t;
            }

            bool makeStatic(DxBuffer &b, const void *data, size_t size)
            {
                const UINT64 bytes = BufferPages::rounded(std::max<size_t>(b.desc().size, 4));
                UINT64 offset = 0;
                Span span(t_spans.buffer);
                ++t_spans.buffers;
                BufferPages::Page *page = m_core->bufferPages.alloc(m_core->device.Get(), bytes, offset);
                if (!page)
                {
                    std::fprintf(stderr, "[rhi] D3D12 buffer %zu bytes failed\n", b.desc().size);
                    return false;
                }
                b.page = page;
                b.offset = offset;
                b.bytes = bytes;
                ++g_stats.buffers;
                // the data (zeros after it: a range may have held another buffer) in the set-up commands. The page is
                // promoted from COMMON to COPY_DEST there and decays back after that submit; the draws promote it to
                // their read states
                const Slice s = ringAlloc(bytes, 16);
                const size_t n = data ? std::min<size_t>(size, bytes) : 0;
                if (n)
                    std::memcpy(s.data, data, n);
                std::memset(s.data + n, 0, bytes - n);
                initCmd()->CopyBufferRegion(page->buffer.Get(), offset, s.buffer, s.offset, bytes);
                return true;
            }

            // ---- frames and command lists -------------------------------------------------------------------------
            FrameSlot &frame() { return m_frames[m_core->frame]; }

            ComPtr<ID3D12GraphicsCommandList> newList()
            {
                FrameSlot &f = frame();
                if (f.used == f.allocators.size())
                {
                    ComPtr<ID3D12CommandAllocator> a;
                    m_core->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
                    f.allocators.push_back(a);
                }
                ID3D12CommandAllocator *a = f.allocators[f.used++].Get();
                ComPtr<ID3D12GraphicsCommandList> list;
                if (!m_freeLists.empty())
                {
                    list = std::move(m_freeLists.back());
                    m_freeLists.pop_back();
                    list->Reset(a, nullptr);
                }
                else
                    m_core->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, nullptr, IID_PPV_ARGS(&list));
                return list;
            }

            // The commands of a frame (or of the part after a wait): the main list and a set-up list for new resources'
            // first contents, submitted before it
            void beginCommands()
            {
                m_cmd = newList();
                m_init.Reset();
                m_recording = true;
                ID3D12DescriptorHeap *heaps[2] = {m_views.Get(), m_samplerHeap.Get()};
                m_cmd->SetDescriptorHeaps(2, heaps);
                m_cmd->SetGraphicsRootSignature(m_root.Get());
                // a new list has no state: everything is set again at the next draw
                m_boundPso = nullptr;
                m_emittedTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
                std::fill(std::begin(m_emittedCbv), std::end(m_emittedCbv), D3D12_GPU_VIRTUAL_ADDRESS(~0ull));
                m_emittedTextures = m_emittedSamplers = ~0ull;
                m_emittedVb = {};
                m_emittedIb = {};
                m_texturesDirty = m_buffersDirty = m_viewportDirty = m_scissorDirty = m_blendDirty = true;
                m_markerDepth = 0;
            }

            ID3D12GraphicsCommandList *initCmd()
            {
                if (!m_init)
                    m_init = newList();
                return m_init.Get();
            }

            void beginFrame()
            {
                m_core->owner.store(std::this_thread::get_id(), std::memory_order_relaxed);
                m_core->frame = (m_core->frame + 1) % kFrames;
                FrameSlot &f = frame();
                waitFor(f.fence);
                {
                    Span span(t_spans.collect);
                    t_spans.collected += static_cast<uint32_t>(m_core->garbage[m_core->frame].size());
                    m_core->collect(m_core->frame);
                }
                for (size_t i = 0; i < f.used; ++i)
                    f.allocators[i]->Reset();
                f.used = 0;
                for (Ring::Chunk &c : f.ring.chunks)
                    c.used = 0;
                f.ring.current = 0;
                f.viewsUsed = 0;
                f.tables.clear();
                ++m_frameSerial;
                beginCommands();
            }

            // Ends the lists and submits them (set-up first); `final`: the frame's last submit (its fence)
            void submit(bool final)
            {
                if (!m_recording)
                    return;
                for (; m_markerDepth > 0; --m_markerDepth) // events end with their list
                    m_cmd->EndEvent();
                drainStaged();
                if (m_init)
                {
                    m_init->Close();
                    ID3D12CommandList *lists[1] = {m_init.Get()};
                    m_queue->ExecuteCommandLists(1, lists); // its own submit: buffers it wrote decay to COMMON after it
                    m_freeLists.push_back(std::move(m_init));
                }
                m_cmd->Close();
                ID3D12CommandList *lists[1] = {m_cmd.Get()};
                m_queue->ExecuteCommandLists(1, lists);
                m_freeLists.push_back(std::move(m_cmd));
                if (final)
                {
                    m_queue->Signal(m_fence.Get(), ++m_fenceValue);
                    frame().fence = m_fenceValue;
                }
                m_recording = false;
            }

            void waitFor(UINT64 value)
            {
                if (!value || m_fence->GetCompletedValue() >= value)
                    return;
                ++g_stats.waits;
                Span span(t_spans.wait);
                ++t_spans.waits;
                m_fence->SetEventOnCompletion(value, m_fenceEvent);
                WaitForSingleObject(m_fenceEvent, INFINITE);
            }

            void waitIdle()
            {
                if (!m_queue || !m_fence)
                    return;
                m_queue->Signal(m_fence.Get(), ++m_fenceValue);
                waitFor(m_fenceValue);
            }

            // The commands so far run to the end (readbacks); recording goes on in new lists of this frame
            void flushAndWait()
            {
                ++t_spans.flushes;
                submit(false);
                waitIdle();
                beginCommands();
                if (m_inPass)
                    bindPassTargets();
            }

            // An object about to be destroyed: no binding may keep pointing at it
            void unbind(const void *object)
            {
                if (object == m_vertexBuffer)
                    m_vertexBuffer = nullptr;
                if (object == m_indexBuffer)
                    m_indexBuffer = nullptr;
                for (DxBuffer *&u : m_uniforms)
                    if (u == object)
                        u = nullptr;
                for (uint32_t i = 0; i < kTextureSlots; ++i)
                {
                    if (m_textures[i] == object)
                        m_textures[i] = nullptr, m_texturesDirty = true;
                    if (m_samplers[i] == object)
                        m_samplers[i] = nullptr, m_texturesDirty = true;
                }
                if (object == m_pipeline)
                    m_pipeline = nullptr;
                if (m_inPass && (object == m_passColor || object == m_passDepth))
                {
                    // a target destroyed inside its pass: no target of it any more (its resource lives to the frame's end)
                    if (object == m_passColor)
                        m_passColor = nullptr;
                    if (object == m_passDepth)
                        m_passDepth = nullptr;
                    bindPassTargets();
                }
            }

            // ---- passes -------------------------------------------------------------------------------------------
            D3D12_RESOURCE_STATES homeState(const DxTexture &t) const
            {
                if (m_inPass && &t == m_passColor)
                    return D3D12_RESOURCE_STATE_RENDER_TARGET;
                if (m_inPass && &t == m_passDepth)
                    return D3D12_RESOURCE_STATE_DEPTH_WRITE;
                return t.resting;
            }

            void bindPassTargets()
            {
                if (!m_inPass)
                    return;
                D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
                if (m_passToBack)
                    rtv = m_backRtv[m_backIndex];
                else if (m_passColor)
                    rtv = m_passColor->rtv;
                const D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_passDepth ? m_passDepth->dsv : D3D12_CPU_DESCRIPTOR_HANDLE{};
                m_cmd->OMSetRenderTargets(rtv.ptr ? 1u : 0u, rtv.ptr ? &rtv : nullptr, FALSE, dsv.ptr ? &dsv : nullptr);
                m_boundPso = nullptr; // the pipeline variant may change with the targets
            }

            void endPassInternal()
            {
                if (!m_inPass)
                    return;
                if (m_passMarker)
                    popMarker();
                m_passMarker = false;
                m_inPass = false;
                if (m_passColor)
                    transition(*m_passColor, m_passColor->resting);
                if (m_passDepth)
                    transition(*m_passDepth, m_passDepth->resting);
                m_passColor = nullptr;
                m_passDepth = nullptr;
                m_passToBack = false;
            }

            void backToPresent()
            {
                for (UINT i = 0; i < kBackbuffers; ++i)
                    if (m_backState[i] != D3D12_RESOURCE_STATE_PRESENT && m_backbuffers[i])
                    {
                        barrier(m_backbuffers[i].Get(), m_backState[i], D3D12_RESOURCE_STATE_PRESENT);
                        m_backState[i] = D3D12_RESOURCE_STATE_PRESENT;
                    }
            }

            // ---- barriers (whole resources: correctness first) ------------------------------------------------------
            void barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to,
                         UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
            {
                if (from == to)
                    return;
                // a resource moved and moved again before the batch is recorded: one barrier (or none when it comes back)
                for (size_t i = m_barriers.size(); i-- > 0;)
                {
                    D3D12_RESOURCE_TRANSITION_BARRIER &t = m_barriers[i].Transition;
                    if (t.pResource != r)
                        continue;
                    if (t.Subresource == subresource && t.StateAfter == from)
                    {
                        if (t.StateBefore == to)
                            m_barriers.erase(m_barriers.begin() + static_cast<ptrdiff_t>(i));
                        else
                            t.StateAfter = to;
                        return;
                    }
                    break;
                }
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition = {r, subresource, from, to};
                m_barriers.push_back(b);
            }

            void transition(DxTexture &t, D3D12_RESOURCE_STATES to)
            {
                barrier(t.resource.Get(), t.state, to);
                t.state = to;
            }

            void flushBarriers()
            {
                if (m_barriers.empty() || !m_recording)
                    return;
                m_cmd->ResourceBarrier(static_cast<UINT>(m_barriers.size()), m_barriers.data());
                g_stats.barriers += m_barriers.size();
                ++g_stats.barrierCalls;
                m_barriers.clear();
            }

            // a barrier recorded at once into another list (the set-up commands)
            void stateBarrier(ID3D12GraphicsCommandList *cmd, DxTexture &t, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
            {
                if (from == to)
                    return;
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition = {t.resource.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
                cmd->ResourceBarrier(1, &b);
            }

            // ---- draws --------------------------------------------------------------------------------------------
            Slice ringAlloc(UINT64 size, UINT64 align)
            {
                Ring &ring = frame().ring;
                for (;;)
                {
                    while (ring.current < ring.chunks.size())
                    {
                        Ring::Chunk &c = ring.chunks[ring.current];
                        const UINT64 at = alignUp(c.used, align);
                        if (at + size <= c.size)
                        {
                            c.used = at + size;
                            return {c.buffer.Get(), at, c.data + at, c.va + at};
                        }
                        ++ring.current;
                    }
                    Span span(t_spans.memory);
                    ++t_spans.memories;
                    Ring::Chunk c;
                    c.size = std::max(kRingChunk, alignUp(size + align, 65536));
                    const D3D12_HEAP_PROPERTIES heap = heapProps(D3D12_HEAP_TYPE_UPLOAD);
                    const D3D12_RESOURCE_DESC bd = bufferDesc(c.size);
                    void *mapped = nullptr;
                    if (FAILED(m_core->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                                       nullptr, IID_PPV_ARGS(&c.buffer))) ||
                        FAILED(c.buffer->Map(0, nullptr, &mapped)))
                    {
                        std::fprintf(stderr, "[rhi] D3D12 upload memory (%llu bytes) failed\n", static_cast<unsigned long long>(c.size));
                        std::abort();
                    }
                    c.data = static_cast<uint8_t *>(mapped);
                    c.va = c.buffer->GetGPUVirtualAddress();
                    ring.chunks.push_back(std::move(c));
                    ring.current = ring.chunks.size() - 1;
                }
            }

            // a buffer's address for this frame's draws (a dynamic one's contents copied into the ring once per update
            // and frame)
            D3D12_GPU_VIRTUAL_ADDRESS addressOf(DxBuffer &b, UINT &size)
            {
                if (!b.desc().dynamic)
                {
                    size = static_cast<UINT>(std::max<size_t>(b.desc().size, 4));
                    return b.page ? b.page->va + b.offset : 0;
                }
                const size_t n = std::min(b.used, b.shadow.size());
                if (b.sliceFrame != m_frameSerial)
                {
                    const Slice s = ringAlloc(n, 256);
                    std::memcpy(s.data, b.shadow.data(), n);
                    b.sliceVa = s.va;
                    b.sliceFrame = m_frameSerial;
                }
                size = static_cast<UINT>(n);
                return b.sliceVa;
            }

            // eight descriptors into a table of a shader-visible heap: the textures' per frame (cached by contents), the
            // samplers' kept while the heap lasts
            D3D12_GPU_DESCRIPTOR_HANDLE textureTable(const Table &key)
            {
                FrameSlot &f = frame();
                auto it = f.tables.find(key);
                UINT at;
                if (it != f.tables.end())
                    at = it->second;
                else
                {
                    if (f.viewsUsed + kTextureSlots > kViewsPerFrame)
                    {
                        // the frame's part of the heap is full: everything so far finishes, the part is reused
                        flushAndWait();
                        f.viewsUsed = 0;
                        f.tables.clear();
                    }
                    at = m_core->frame * kViewsPerFrame + f.viewsUsed;
                    f.viewsUsed += kTextureSlots;
                    copyTable(m_views.Get(), at, m_viewIncrement, key, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                    ++g_stats.tables;
                    f.tables.emplace(key, at);
                }
                return {m_views->GetGPUDescriptorHandleForHeapStart().ptr + UINT64(at) * m_viewIncrement};
            }

            D3D12_GPU_DESCRIPTOR_HANDLE samplerTable(const Table &key)
            {
                auto it = m_samplerTables.find(key);
                UINT at;
                if (it != m_samplerTables.end())
                    at = it->second;
                else
                {
                    if (m_samplerNext + kTextureSlots > kSamplerHeap)
                    {
                        flushAndWait(); // every table in use is done with: the heap starts again
                        m_samplerTables.clear();
                        m_samplerNext = 0;
                    }
                    at = m_samplerNext;
                    m_samplerNext += kTextureSlots;
                    copyTable(m_samplerHeap.Get(), at, m_samplerIncrement, key, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
                    m_samplerTables.emplace(key, at);
                }
                return {m_samplerHeap->GetGPUDescriptorHandleForHeapStart().ptr + UINT64(at) * m_samplerIncrement};
            }

            void copyTable(ID3D12DescriptorHeap *heap, UINT at, UINT increment, const Table &key, D3D12_DESCRIPTOR_HEAP_TYPE type)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE src[kTextureSlots];
                UINT ones[kTextureSlots];
                for (uint32_t i = 0; i < kTextureSlots; ++i)
                {
                    src[i].ptr = key.h[i];
                    ones[i] = 1;
                }
                const D3D12_CPU_DESCRIPTOR_HANDLE dst{heap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(at) * increment};
                const UINT count = kTextureSlots;
                m_core->device->CopyDescriptors(1, &dst, &count, kTextureSlots, src, ones, type);
            }

            bool prepareDraw(bool indexed)
            {
                if (!m_pipeline || !m_inPass || (indexed && !m_indexBuffer))
                    return false;
                if (m_texturesDirty)
                {
                    // textures the shader reads: readable, not the pass's own targets (D3D unbinds a texture that is
                    // also the target: it reads zeros)
                    Table views, samplers;
                    for (uint32_t i = 0; i < kTextureSlots; ++i)
                    {
                        DxTexture *t = m_textures[i];
                        if (t && (t == m_passColor || t == m_passDepth || t->samples != 1u || !t->srv.ptr))
                            t = nullptr;
                        if (t && t->state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
                            transition(*t, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                        views.h[i] = t ? t->srv.ptr : m_nullSrv.ptr;
                        samplers.h[i] = (m_samplers[i] ? m_samplers[i] : static_cast<DxSampler *>(m_defaultSampler.get()))->handle.ptr;
                    }
                    flushBarriers();
                    // the renderer sets its textures before every draw: most are the last draw's (no lookup then)
                    if (m_emittedTextures == ~0ull || !(views == m_lastViews))
                    {
                        const D3D12_GPU_DESCRIPTOR_HANDLE vt = textureTable(views);
                        if (vt.ptr != m_emittedTextures)
                        {
                            m_cmd->SetGraphicsRootDescriptorTable(kRootTextures, vt);
                            m_emittedTextures = vt.ptr;
                        }
                        m_lastViews = views;
                    }
                    if (m_emittedSamplers == ~0ull || !(samplers == m_lastSamplers))
                    {
                        const D3D12_GPU_DESCRIPTOR_HANDLE st = samplerTable(samplers);
                        if (st.ptr != m_emittedSamplers)
                        {
                            m_cmd->SetGraphicsRootDescriptorTable(kRootSamplers, st);
                            m_emittedSamplers = st.ptr;
                        }
                        m_lastSamplers = samplers;
                    }
                    m_texturesDirty = false;
                }
                // the pipeline for the pass's targets
                const DXGI_FORMAT color = m_passToBack ? kBackbufferFormat : m_passColor ? m_passColor->format : DXGI_FORMAT_UNKNOWN;
                const DXGI_FORMAT depth = m_passDepth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
                const uint32_t samples = m_passColor ? m_passColor->samples : m_passDepth ? m_passDepth->samples : 1u;
                ID3D12PipelineState *pso = variant(*m_pipeline, color, depth, samples);
                if (!pso)
                    return false;
                if (pso != m_boundPso)
                {
                    m_cmd->SetPipelineState(pso);
                    m_boundPso = pso;
                }
                if (m_pipeline->topology != m_emittedTopology)
                {
                    m_cmd->IASetPrimitiveTopology(m_pipeline->topology);
                    m_emittedTopology = m_pipeline->topology;
                }
                for (uint32_t i = 0; i < kUniformSlots; ++i)
                {
                    UINT size = 0;
                    const D3D12_GPU_VIRTUAL_ADDRESS va = m_uniforms[i] ? addressOf(*m_uniforms[i], size) : m_zeroBuffer->GetGPUVirtualAddress();
                    if (va != m_emittedCbv[i])
                    {
                        m_cmd->SetGraphicsRootConstantBufferView(i, va);
                        m_emittedCbv[i] = va;
                    }
                }
                if (m_viewportDirty)
                {
                    m_cmd->RSSetViewports(1, &m_viewport);
                    m_viewportDirty = false;
                }
                if (m_scissorDirty)
                {
                    D3D12_RECT r = m_scissor;
                    r.left = std::clamp<LONG>(r.left, 0, static_cast<LONG>(m_passWidth));
                    r.top = std::clamp<LONG>(r.top, 0, static_cast<LONG>(m_passHeight));
                    r.right = std::clamp<LONG>(r.right, r.left, static_cast<LONG>(m_passWidth));
                    r.bottom = std::clamp<LONG>(r.bottom, r.top, static_cast<LONG>(m_passHeight));
                    m_cmd->RSSetScissorRects(1, &r);
                    m_scissorDirty = false;
                }
                if (m_blendDirty)
                {
                    m_cmd->OMSetBlendFactor(m_blend);
                    m_blendDirty = false;
                }
                // a dynamic buffer updated since its slice was made gets a new one (its address moves)
                if ((m_vertexBuffer && m_vertexBuffer->desc().dynamic && m_vertexBuffer->sliceFrame != m_frameSerial) ||
                    (indexed && m_indexBuffer->desc().dynamic && m_indexBuffer->sliceFrame != m_frameSerial))
                    m_buffersDirty = true;
                const UINT stride = m_pipeline->desc.stride;
                if (m_buffersDirty || (m_vertexBuffer && m_emittedVb.StrideInBytes != stride))
                {
                    if (m_vertexBuffer)
                    {
                        UINT size = 0;
                        const D3D12_GPU_VIRTUAL_ADDRESS va = addressOf(*m_vertexBuffer, size);
                        D3D12_VERTEX_BUFFER_VIEW vb{va + m_vertexOffset, size > m_vertexOffset ? size - m_vertexOffset : 0u, stride};
                        if (std::memcmp(&vb, &m_emittedVb, sizeof(vb)) != 0)
                        {
                            m_cmd->IASetVertexBuffers(0, 1, &vb);
                            m_emittedVb = vb;
                        }
                    }
                    if (m_indexBuffer)
                    {
                        UINT size = 0;
                        const D3D12_GPU_VIRTUAL_ADDRESS va = addressOf(*m_indexBuffer, size);
                        D3D12_INDEX_BUFFER_VIEW ib{va, size, m_index32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT};
                        if (std::memcmp(&ib, &m_emittedIb, sizeof(ib)) != 0)
                        {
                            m_cmd->IASetIndexBuffer(&ib);
                            m_emittedIb = ib;
                        }
                    }
                    m_buffersDirty = false;
                }
                return true;
            }

            ID3D12PipelineState *variant(DxPipeline &p, DXGI_FORMAT color, DXGI_FORMAT depth, uint32_t samples)
            {
                const uint64_t key = (uint64_t(color) << 32) | (uint64_t(depth) << 8) | samples;
                auto it = p.variants.find(key);
                if (it != p.variants.end())
                    return it->second.Get();
                const PipelineDesc &d = p.desc;
                std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
                for (const VertexAttribute &a : d.attributes)
                    elements.push_back({a.semantic, a.semanticIndex, vertexFormat(a.format), 0, a.offset,
                                        D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0});
                D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
                pd.pRootSignature = m_root.Get();
                pd.VS = {d.vertex.data, d.vertex.size};
                pd.PS = {d.pixel.data, d.pixel.size};
                D3D12_RENDER_TARGET_BLEND_DESC &rt = pd.BlendState.RenderTarget[0];
                rt.BlendEnable = d.blend.enable;
                rt.SrcBlend = blendFactor(d.blend.srcColor);
                rt.DestBlend = blendFactor(d.blend.dstColor);
                rt.BlendOp = blendOp(d.blend.colorOp);
                rt.SrcBlendAlpha = blendFactor(d.blend.srcAlpha);
                rt.DestBlendAlpha = blendFactor(d.blend.dstAlpha);
                rt.BlendOpAlpha = blendOp(d.blend.alphaOp);
                rt.LogicOp = D3D12_LOGIC_OP_NOOP;
                rt.RenderTargetWriteMask = d.blend.writeMask & 0xF;
                pd.SampleMask = UINT_MAX;
                pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
                pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                pd.RasterizerState.DepthClipEnable = d.depthClip ? TRUE : FALSE;
                pd.RasterizerState.MultisampleEnable = samples > 1u;
                const bool depthOn = (d.depth.test || d.depth.write) && depth != DXGI_FORMAT_UNKNOWN;
                pd.DepthStencilState.DepthEnable = depthOn;
                pd.DepthStencilState.DepthWriteMask = d.depth.write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
                pd.DepthStencilState.DepthFunc = d.depth.test ? compare(d.depth.func) : D3D12_COMPARISON_FUNC_ALWAYS;
                const D3D12_DEPTH_STENCILOP_DESC keep{D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                                      D3D12_COMPARISON_FUNC_ALWAYS};
                pd.DepthStencilState.FrontFace = keep;
                pd.DepthStencilState.BackFace = keep;
                pd.InputLayout = {elements.empty() ? nullptr : elements.data(), static_cast<UINT>(elements.size())};
                pd.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
                pd.PrimitiveTopologyType = topologyType(d.topology);
                pd.NumRenderTargets = color != DXGI_FORMAT_UNKNOWN ? 1u : 0u;
                pd.RTVFormats[0] = color;
                pd.DSVFormat = depth;
                pd.SampleDesc.Count = samples;
                ComPtr<ID3D12PipelineState> pso;
                HRESULT hr;
                {
                    Span span(t_spans.pso);
                    ++t_spans.psos;
                    hr = m_core->device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso));
                }
                if (FAILED(hr))
                {
                    std::fprintf(stderr, "[rhi] D3D12 pipeline '%s' (%u / %u x%u) rejected (%s)\n", p.name.c_str(), color, depth, samples,
                                 hex(hr).c_str());
                    p.variants[key] = nullptr; // not tried again
                    return nullptr;
                }
                setName(pso.Get(), p.name.empty() ? nullptr : p.name.c_str());
                p.variants[key] = pso;
                ++g_stats.psos;
                return pso.Get();
            }

            // ---- copies -------------------------------------------------------------------------------------------
            // levels' data into the texture (made in COPY_DEST): through the ring (owner) - the footprints D3D12 wants
            void uploadLevels(ID3D12GraphicsCommandList *cmd, DxTexture &t, const void *const *levels, const uint32_t *pitches,
                              uint32_t count)
            {
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[16];
                UINT rows[16];
                UINT64 rowSizes[16], total = 0;
                const D3D12_RESOURCE_DESC rd = t.resource->GetDesc();
                count = std::min(count, 16u);
                m_core->device->GetCopyableFootprints(&rd, 0, count, 0, fp, rows, rowSizes, &total);
                const Slice s = ringAlloc(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
                fillLevels(s.data, t, fp, rows, levels, pitches, count);
                for (uint32_t i = 0; i < count; ++i)
                {
                    D3D12_TEXTURE_COPY_LOCATION dst{t.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
                    dst.SubresourceIndex = i;
                    D3D12_TEXTURE_COPY_LOCATION src{s.buffer, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
                    src.PlacedFootprint = fp[i];
                    src.PlacedFootprint.Offset += s.offset;
                    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                }
            }

            static void fillLevels(uint8_t *base, const DxTexture &t, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT *fp, const UINT *rows,
                                   const void *const *levels, const uint32_t *pitches, uint32_t count)
            {
                const Format f = t.desc().format;
                for (uint32_t i = 0; i < count; ++i)
                {
                    const uint32_t w = std::max(1u, t.desc().width >> i), h = std::max(1u, t.desc().height >> i);
                    const uint32_t row = rowBytes(f, w), n = std::min<uint32_t>(rowCount(f, h), rows[i]);
                    const auto *src = static_cast<const uint8_t *>(levels[i]);
                    for (uint32_t y = 0; y < n; ++y)
                        std::memcpy(base + fp[i].Offset + UINT64(y) * fp[i].Footprint.RowPitch, src + size_t(y) * pitches[i], row);
                }
            }

            // ---- textures made on other threads (freeThreadedCreation) ---------------------------------------------
            // As rhi_vulkan.cpp's stage(): the first contents copied here into an upload buffer of their own; the copies go
            // into the set-up commands at the owner's next submit (drainStaged). The texture counts as resting from now
            // on: the owner gets it only after this returns, so what it records with it runs after the copies.
            struct Staged
            {
                ComPtr<ID3D12Resource> texture;
                StagingPool::Chunk *chunk = nullptr; // the data's range of the staging pool
                UINT64 offset = 0, size = 0;
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[16];
                uint32_t count = 0;
                D3D12_RESOURCE_STATES resting = D3D12_RESOURCE_STATE_COMMON;
            };

            bool stage(DxTexture &t, const void *const *levels, const uint32_t *pitches, uint32_t count)
            {
                Staged st;
                st.texture = t.resource;
                st.resting = t.resting;
                st.count = std::min(count, 16u);
                UINT rows[16];
                UINT64 rowSizes[16], total = 0;
                const D3D12_RESOURCE_DESC rd = t.resource->GetDesc();
                m_core->device->GetCopyableFootprints(&rd, 0, st.count, st.offset, st.fp, rows, rowSizes, &total);
                st.chunk = m_core->staging.alloc(m_core->device.Get(), total, st.offset);
                if (!st.chunk)
                {
                    std::fprintf(stderr, "[rhi] D3D12 staging memory (%llu bytes) failed\n", static_cast<unsigned long long>(total));
                    return false;
                }
                st.size = total;
                // the footprints at the range's offset
                m_core->device->GetCopyableFootprints(&rd, 0, st.count, st.offset, st.fp, rows, rowSizes, &total);
                fillLevels(st.chunk->data, t, st.fp, rows, levels, pitches, st.count);
                t.state = t.resting;
                std::lock_guard<std::mutex> lock(m_stagedMutex);
                m_staged.push_back(std::move(st));
                return true;
            }

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
                    ID3D12GraphicsCommandList *cmd = initCmd();
                    for (Staged &st : staged)
                    {
                        for (uint32_t i = 0; i < st.count; ++i)
                        {
                            D3D12_TEXTURE_COPY_LOCATION dst{st.texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
                            dst.SubresourceIndex = i;
                            D3D12_TEXTURE_COPY_LOCATION src{st.chunk->buffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
                            src.PlacedFootprint = st.fp[i];
                            cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                        }
                        if (st.resting != D3D12_RESOURCE_STATE_COPY_DEST)
                        {
                            D3D12_RESOURCE_BARRIER b{};
                            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                            b.Transition = {st.texture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, st.resting};
                            cmd->ResourceBarrier(1, &b);
                        }
                        Core *c = m_core.get();
                        ComPtr<ID3D12Resource> texture = st.texture;
                        StagingPool::Chunk *chunk = st.chunk;
                        const UINT64 offset = st.offset, size = st.size;
                        m_core->garbage[m_core->frame].push_back([c, texture, chunk, offset, size]() mutable {
                            texture.Reset();
                            c->staging.release(chunk, offset, size);
                        });
                    }
                }
                for (auto &f : orphans)
                    m_core->garbage[m_core->frame].push_back(std::move(f));
            }

            // ---- mip chains: a draw per level, each level from the one above with linear filtering -----------------
            void mipChain(DxTexture &t)
            {
                if (t.mips <= 1u || !(t.resource->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))
                    return;
                ID3D12PipelineState *pso = mipPipeline(t.format);
                if (!pso)
                    return;
                ID3D12Device *device = m_core->device.Get();
                if (t.levelSrv.empty())
                {
                    for (uint32_t i = 0; i < t.mips; ++i)
                    {
                        D3D12_CPU_DESCRIPTOR_HANDLE s = m_core->srvs.alloc(), r = m_core->rtvs.alloc();
                        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
                        sv.Format = t.format;
                        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                        sv.Texture2D.MostDetailedMip = i;
                        sv.Texture2D.MipLevels = 1;
                        device->CreateShaderResourceView(t.resource.Get(), &sv, s);
                        D3D12_RENDER_TARGET_VIEW_DESC rv{};
                        rv.Format = t.format;
                        rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                        rv.Texture2D.MipSlice = i;
                        device->CreateRenderTargetView(t.resource.Get(), &rv, r);
                        t.levelSrv.push_back(s);
                        t.levelRtv.push_back(r);
                    }
                }
                transition(t, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                flushBarriers();
                m_cmd->SetPipelineState(pso);
                m_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                Table samplers;
                for (auto &h : samplers.h)
                    h = static_cast<DxSampler *>(m_linearSampler.get())->handle.ptr;
                m_cmd->SetGraphicsRootDescriptorTable(kRootSamplers, samplerTable(samplers));
                for (uint32_t level = 1; level < t.mips; ++level)
                {
                    Table views;
                    for (auto &h : views.h)
                        h = m_nullSrv.ptr;
                    views.h[0] = t.levelSrv[level - 1].ptr;
                    m_cmd->SetGraphicsRootDescriptorTable(kRootTextures, textureTable(views));
                    barrier(t.resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET, level);
                    flushBarriers();
                    m_cmd->OMSetRenderTargets(1, &t.levelRtv[level], FALSE, nullptr);
                    const float w = static_cast<float>(std::max(1u, t.desc().width >> level));
                    const float h = static_cast<float>(std::max(1u, t.desc().height >> level));
                    const D3D12_VIEWPORT vp{0.0f, 0.0f, w, h, 0.0f, 1.0f};
                    const D3D12_RECT sc{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
                    m_cmd->RSSetViewports(1, &vp);
                    m_cmd->RSSetScissorRects(1, &sc);
                    m_cmd->DrawInstanced(3, 1, 0, 0);
                    barrier(t.resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, level);
                }
                flushBarriers();
                transition(t, homeState(t));
                // the draws' state is set again at the next draw; the pass's targets bound again
                m_boundPso = nullptr;
                m_emittedTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
                m_emittedTextures = m_emittedSamplers = ~0ull;
                m_texturesDirty = m_viewportDirty = m_scissorDirty = true;
                bindPassTargets();
            }

            ID3D12PipelineState *mipPipeline(DXGI_FORMAT format)
            {
                auto it = m_mipPipelines.find(format);
                if (it != m_mipPipelines.end())
                    return it->second.Get();
                if (!m_mipVs)
                {
                    static const char kSource[] =
                        "struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
                        "V vs_main(uint id : SV_VertexID) { V o; float2 p = float2((id << 1) & 2, id & 2); o.uv = p;\n"
                        "  o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
                        "Texture2D t0 : register(t0); SamplerState s0 : register(s0);\n"
                        "float4 ps_main(V i) : SV_Target { return t0.SampleLevel(s0, i.uv, 0); }\n";
                    ComPtr<ID3DBlob> errors;
                    if (FAILED(D3DCompile(kSource, sizeof(kSource) - 1, "mips", nullptr, nullptr, "vs_main", "vs_5_0", 0, 0, &m_mipVs, &errors)) ||
                        FAILED(D3DCompile(kSource, sizeof(kSource) - 1, "mips", nullptr, nullptr, "ps_main", "ps_5_0", 0, 0, &m_mipPs, &errors)))
                    {
                        std::fprintf(stderr, "[rhi] D3D12 mip shaders failed: %s\n",
                                     errors ? static_cast<const char *>(errors->GetBufferPointer()) : "?");
                        m_mipVs.Reset();
                        m_mipPipelines[format] = nullptr;
                        return nullptr;
                    }
                }
                D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
                pd.pRootSignature = m_root.Get();
                pd.VS = {m_mipVs->GetBufferPointer(), m_mipVs->GetBufferSize()};
                pd.PS = {m_mipPs->GetBufferPointer(), m_mipPs->GetBufferSize()};
                pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
                pd.SampleMask = UINT_MAX;
                pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
                pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                pd.RasterizerState.DepthClipEnable = TRUE;
                pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                pd.NumRenderTargets = 1;
                pd.RTVFormats[0] = format;
                pd.SampleDesc.Count = 1;
                ComPtr<ID3D12PipelineState> pso;
                if (FAILED(m_core->device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso))))
                    std::fprintf(stderr, "[rhi] D3D12 mip pipeline (format %u) failed\n", format);
                m_mipPipelines[format] = pso;
                return pso.Get();
            }

            std::shared_ptr<Core> m_core;
            ComPtr<ID3D12InfoQueue> m_infoQueue;
            std::chrono::steady_clock::time_point m_spikeFrameEnd{};
            uint32_t m_debugPrinted = 0;
            HWND m_hwnd = nullptr;
            ComPtr<IDXGIFactory4> m_factory;
            ComPtr<ID3D12CommandQueue> m_queue;
            ComPtr<ID3D12Fence> m_fence;
            HANDLE m_fenceEvent = nullptr;
            UINT64 m_fenceValue = 0;
            ComPtr<IDXGISwapChain3> m_swapchain;
            ComPtr<ID3D12Resource> m_backbuffers[kBackbuffers];
            D3D12_CPU_DESCRIPTOR_HANDLE m_backRtv[kBackbuffers]{};
            D3D12_RESOURCE_STATES m_backState[kBackbuffers]{};
            UINT m_backIndex = 0;
            UINT m_swapFlags = 0;
            uint32_t m_width = 0, m_height = 0;
            std::string m_adapterName;
            bool m_lostReported = false;

            ComPtr<ID3D12RootSignature> m_root;
            ComPtr<ID3D12DescriptorHeap> m_views, m_samplerHeap;
            UINT m_viewIncrement = 0, m_samplerIncrement = 0;
            std::unordered_map<Table, UINT, TableHash> m_samplerTables;
            UINT m_samplerNext = 0;
            D3D12_CPU_DESCRIPTOR_HANDLE m_nullSrv{};
            std::unique_ptr<Sampler> m_defaultSampler, m_linearSampler;
            ComPtr<ID3D12Resource> m_zeroBuffer;
            ComPtr<ID3D12Resource> m_readback; // readTexture's
            UINT64 m_readbackSize = 0;

            FrameSlot m_frames[kFrames];
            uint64_t m_frameSerial = 0;
            ComPtr<ID3D12GraphicsCommandList> m_cmd, m_init;
            std::vector<ComPtr<ID3D12GraphicsCommandList>> m_freeLists;
            bool m_recording = false;
            int m_markerDepth = 0;
            std::vector<D3D12_RESOURCE_BARRIER> m_barriers;

            // what the renderer set
            DxPipeline *m_pipeline = nullptr;
            DxBuffer *m_vertexBuffer = nullptr, *m_indexBuffer = nullptr;
            uint32_t m_vertexOffset = 0;
            bool m_index32 = false;
            DxTexture *m_textures[kTextureSlots] = {};
            DxSampler *m_samplers[kTextureSlots] = {};
            DxBuffer *m_uniforms[kUniformSlots] = {};
            D3D12_VIEWPORT m_viewport{};
            D3D12_RECT m_scissor{};
            float m_blend[4] = {0, 0, 0, 0};
            bool m_texturesDirty = true, m_buffersDirty = true, m_viewportDirty = true, m_scissorDirty = true, m_blendDirty = true;
            // what the current list has
            ID3D12PipelineState *m_boundPso = nullptr;
            D3D_PRIMITIVE_TOPOLOGY m_emittedTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
            D3D12_GPU_VIRTUAL_ADDRESS m_emittedCbv[kUniformSlots]{};
            UINT64 m_emittedTextures = ~0ull, m_emittedSamplers = ~0ull;
            Table m_lastViews, m_lastSamplers; // what those tables hold
            D3D12_VERTEX_BUFFER_VIEW m_emittedVb{};
            D3D12_INDEX_BUFFER_VIEW m_emittedIb{};

            // the pass
            PassDesc m_pass;
            std::string m_passName;
            bool m_inPass = false, m_passMarker = false, m_passToBack = false;
            DxTexture *m_passColor = nullptr, *m_passDepth = nullptr;
            uint32_t m_passWidth = 0, m_passHeight = 0;

            std::mutex m_stagedMutex;
            std::vector<Staged> m_staged;

            ComPtr<ID3DBlob> m_mipVs, m_mipPs;
            std::unordered_map<DXGI_FORMAT, ComPtr<ID3D12PipelineState>> m_mipPipelines;
        };
    }

    std::unique_ptr<Device> createD3D12Device(const DeviceDesc &desc, std::string &error)
    {
        auto device = std::make_unique<D3D12Device>();
        if (!device->create(desc, error))
            return nullptr;
        return device;
    }
}
