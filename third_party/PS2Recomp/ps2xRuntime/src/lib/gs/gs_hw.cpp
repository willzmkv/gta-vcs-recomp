// [gta-recomp] Hardware GS renderer (D3D11 through the RHI). See runtime/gs/gs_hw.h.
#include "runtime/ps2_pad.h" // [gta-recomp] the frame's input time
#include "runtime/ps2_present.h" // [gta-recomp] ps2NoteFramesTaken
#include "runtime/ps2_perf.h" // [gta-recomp]
#include "runtime/gs/gs_hw.h"
#include "runtime/gs/ps2_gs_memory.h" // [gta-recomp] RectBlocks (lazy uploads)
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_draw_source.h" // [gta-recomp] DrawSources

#include "runtime/ps2_control.h" // [gta-recomp] keys: physical or injected

#include "render/gs_shaders.h" // [gta-recomp] D3D11 (RHI) execution
#include "render/texture_replacements.h" // [gta-recomp] PCSX2-named PNG packs
#include "rhi/rhi.h"
#include "runtime/ps2_hle_survey.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem> // [gta-recomp] VCS_TEXTURE_DUMP
#include <thread>
#include <mutex>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cmath>  // [gta-recomp] libstdc++ (Linux) does not bring it in

namespace
{
    std::atomic<float> g_hudSqueeze{1.0f}; // [gta-recomp] gsHwSetHudSqueeze
    std::atomic<GSHwAnchorFn> g_hudClassifier{nullptr}; // [gta-recomp] gsHwSetHudClassifier
    std::atomic<bool> g_hudLayer{false};                // [gta-recomp] gsHwSetHudLayer
    std::atomic<uint32_t> g_outputW{0}, g_outputH{0};   // [gta-recomp] gsHwSetOutputSize
    std::atomic<float> g_aspectWidening{1.0f};          // [gta-recomp] gsHwSetAspectWidening
    std::atomic<uint32_t> g_frameW{0}, g_frameH{0};     // [gta-recomp] gsHwSetFrameSize
    struct TargetRole                                   // [gta-recomp] gsHwSetTargetRole
    {
        uint32_t fbp = 0, fbw = 0;
        const char *name = "";
        bool windowScale = false;
    };
    TargetRole g_targetRoles[16];
    std::atomic<uint32_t> g_targetRoleCount{0};
    const TargetRole *targetRole(uint32_t fbp, uint32_t fbw)
    {
        const uint32_t n = g_targetRoleCount.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < n; ++i)
            if (g_targetRoles[i].fbp == fbp && (g_targetRoles[i].fbw == fbw || fbw == 0u))
                return &g_targetRoles[i];
        return nullptr;
    }
    // [gta-recomp] any role at fbp (whatever its width) wants the window's scale
    bool windowScaleAt(uint32_t fbp)
    {
        const uint32_t n = g_targetRoleCount.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < n; ++i)
            if (g_targetRoles[i].fbp == fbp && g_targetRoles[i].windowScale)
                return true;
        return false;
    }
}

namespace
{
    thread_local uint32_t t_uploadSourceAddress = 0, t_uploadSourceBytes = 0; // [gta-recomp] gsHwSetUploadSource
    std::unordered_map<uint32_t, uint64_t> g_rasterTextures;                // [gta-recomp] raster address -> texture key
}

void gsHwSetUploadSource(uint32_t address, uint32_t bytes)
{
    t_uploadSourceAddress = address;
    t_uploadSourceBytes = bytes;
}

void gsHwTakeUploadSource(uint32_t &address, uint32_t &bytes)
{
    address = t_uploadSourceAddress;
    bytes = t_uploadSourceBytes;
    t_uploadSourceAddress = t_uploadSourceBytes = 0u;
}

uint64_t gsHwTextureForRaster(uint32_t address)
{
    const auto it = g_rasterTextures.find(address);
    return it != g_rasterTextures.end() ? it->second : 0u;
}

void gsHwSetTargetRole(uint32_t fbp, uint32_t fbw, const char *name, bool windowScale)
{
    const uint32_t n = g_targetRoleCount.load(std::memory_order_relaxed);
    if (n >= 16u)
        return;
    g_targetRoles[n] = {fbp, fbw, name ? name : "", windowScale};
    g_targetRoleCount.store(n + 1u, std::memory_order_release);
}

void gsHwSetFrameSize(uint32_t width, uint32_t height)
{
    g_frameW.store(width, std::memory_order_relaxed);
    g_frameH.store(height, std::memory_order_relaxed);
}

void gsHwSetAspectWidening(float widening) { g_aspectWidening.store(widening, std::memory_order_relaxed); }

// [gta-recomp] gsHwShownInputNs (gs_hw.h)
static int64_t steadyNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
// [gta-recomp] gsHwFrameBuildNs: input snapshot -> frame ready for the window (game, render worker, GS stage), the 95th
// percentile of the last 64 frames - the frame lead's estimate (docs/PERFORMANCE.md 19). GS stage thread writes.
static std::atomic<int64_t> g_frameBuildNs{0};
static void noteFrameBuild(int64_t inputNs, int64_t readyNs)
{
    static int64_t recent[64] = {};
    static uint32_t count = 0;
    if (inputNs <= 0 || readyNs <= inputNs || readyNs - inputNs > 200000000ll)
        return;
    recent[count++ % 64u] = readyNs - inputNs;
    const uint32_t n = std::min<uint32_t>(count, 64u);
    int64_t sorted[64];
    std::copy(recent, recent + n, sorted);
    std::nth_element(sorted, sorted + n * 95 / 100, sorted + n);
    g_frameBuildNs.store(sorted[n * 95 / 100], std::memory_order_relaxed);
}
int64_t gsHwFrameBuildNs() { return g_frameBuildNs.load(std::memory_order_relaxed); }

// PS2X_RHI_STATS=1: where a shown frame's input delay goes before the GPU, every 5 s (the RHI reports the total)
static void noteDelayStages(int64_t inputNs, int64_t queuedNs, int64_t takenNs)
{
    static const bool on = std::getenv("PS2X_RHI_STATS") != nullptr;
    static double toQueue = 0.0, inQueue = 0.0;
    static uint64_t n = 0;
    static int64_t last = takenNs;
    if (!on || inputNs <= 0 || queuedNs < inputNs || takenNs < queuedNs)
        return;
    toQueue += (queuedNs - inputNs) / 1e6;
    inQueue += (takenNs - queuedNs) / 1e6;
    ++n;
    if (takenNs - last >= 5000000000ll)
    {
        std::fprintf(stderr, "[gs-hw] input delay by stage: input -> queued %.1f ms (game, render worker, GS stage), queued -> taken %.1f ms\n",
                     toQueue / n, inQueue / n);
        toQueue = inQueue = 0.0;
        n = 0;
        last = takenNs;
    }
}
static std::atomic<int64_t> g_shownInputNs{0};
int64_t gsHwShownInputNs() { return g_shownInputNs.load(std::memory_order_relaxed); }

// [gta-recomp] gsHwSetShowEveryFrame (gs_hw.h). PS2X_HW_QUEUE=<n> / PS2X_HW_BATCH_MAX=<n> (0: all) override either mode.
static std::atomic<bool> g_showEveryFrame{false};
void gsHwSetShowEveryFrame(bool on) { g_showEveryFrame.store(on, std::memory_order_relaxed); }

// [gta-recomp] gsHwSetTextureReplacements: the pack the GS stage takes at its next frame (generation: one per call)
static std::mutex g_packMutex;
static std::shared_ptr<render::TextureReplacements> g_pack;
static std::atomic<uint64_t> g_packGeneration{0};
void gsHwSetTextureReplacements(std::shared_ptr<render::TextureReplacements> pack)
{
    std::lock_guard<std::mutex> lock(g_packMutex);
    g_pack = std::move(pack);
    g_packGeneration.fetch_add(1, std::memory_order_release);
}
namespace
{
    size_t envCount(const char *name, size_t fallback)
    {
        const char *e = std::getenv(name);
        return e && *e ? std::strtoul(e, nullptr, 0) : fallback;
    }
    size_t frameQueueDepth()
    {
        static const size_t forced = envCount("PS2X_HW_QUEUE", 0u);
        return forced ? forced : (g_showEveryFrame.load(std::memory_order_relaxed) ? 1u : 3u);
    }
    size_t frameBatchMax() // 0: no limit
    {
        static const size_t forced = envCount("PS2X_HW_BATCH_MAX", ~size_t(0));
        return forced != ~size_t(0) ? forced : (g_showEveryFrame.load(std::memory_order_relaxed) ? 1u : 0u);
    }
}

void gsHwSetHudSqueeze(float factor) { g_hudSqueeze.store(factor, std::memory_order_relaxed); }
void gsHwSetHudClassifier(GSHwAnchorFn fn) { g_hudClassifier.store(fn, std::memory_order_relaxed); }
void gsHwSetHudLayer(bool on) { g_hudLayer.store(on, std::memory_order_relaxed); }
void gsHwSetOutputSize(uint32_t width, uint32_t height)
{
    g_outputW.store(width, std::memory_order_relaxed);
    g_outputH.store(height, std::memory_order_relaxed);
}

GSHwOptions GSHwOptions::fromEnvironment()
{
    GSHwOptions o;
    if (const char *s = std::getenv("PS2X_HW_SCALE"))
    {
        // [gta-recomp] 0: the native frame (the picture's targets at the window's resolution, the rest at 1x)
        const int v = std::atoi(s);
        o.nativeFrame = v == 0 && s[0] == '0';
        o.scale = o.nativeFrame ? 1u : std::clamp<uint32_t>(static_cast<uint32_t>(v), 1u, 8u);
    }
    if (const char *s = std::getenv("PS2X_HW_MSAA")) // [gta-recomp] 2, 4, 8
    {
        const int v = std::atoi(s);
        o.msaa = v >= 8 ? 8u : v >= 4 ? 4u : v >= 2 ? 2u : 1u;
    }
    if (const char *s = std::getenv("PS2X_HW_ANISO")) // [gta-recomp] 2..16
        o.anisotropy = static_cast<uint32_t>(std::clamp(std::atoi(s), 0, 16));
    return o;
}

namespace
{
    // ------------------------------------------------------------------------------------------------------
    // Recorded data (game thread -> main thread)
    // ------------------------------------------------------------------------------------------------------
    struct HwVertex
    {
        float x, y, z;    // PS2 pixels in the target; depth 0..1
        float s, t, q;    // STQ, or U/V in texels with q = 1 (FST)
        uint8_t r, g, b, a;
        float fog;        // 0..1 (1 = no fog)
    };

    // Everything that selects GPU state for a draw. Compared bytewise to merge consecutive primitives.
    struct HwState
    {
        uint32_t fbp, fbw, fpsm, fbmsk;
        uint32_t zbp, zpsm;
        uint16_t scx0, scy0, scx1, scy1;
        uint8_t zte, ztst, zmsk, ate, atst, aref, afail, abe;
        uint8_t ba, bb, bc, bd, fix, tme, tfx, tcc;
        uint8_t fst, linear, wms, wmt, fge, fogR, fogG, fogB;
        uint8_t iip, mode, texIsRt, rtFull; // rtFull [gta-recomp]: a render target read 1:1 (rtFullScale)
        uint16_t tw, th, minu, maxu, minv, maxv;
        uint32_t rtFbp;
        uint16_t rtOx, rtOy;
        uint64_t texKey;
    };
    static_assert(std::is_trivially_copyable_v<HwState>);

    enum class CmdType : uint8_t
    {
        Draw,
        TexCreate,
        TexDelete,
        RtUpload,
        RtDrop,
        Clear,
        DepthFill,
        MeshCreate, // [gta-recomp] packet-level HLE (GSNativeDraw)
        MeshDelete,
        NativeDraw,
        HudDraw, // [gta-recomp] a DrawCmd for the native HUD layer (gsHwSetHudLayer)
    };

    struct Cmd
    {
        CmdType type;
        uint32_t index; // into the frame's typed array
    };

    struct DrawCmd
    {
        HwState s;
        uint32_t first, count;
        uint32_t source = 0u; // [gta-recomp] gsCurrentDrawSource() when recorded (DrawSources)
        // [gta-recomp] a HudDraw's widescreen squeeze, applied in the layer after its pixel snapping (appendHudDraw):
        // x' = pivot + (x - pivot) * squeeze in GS pixel-centre coordinates, the scissor alike (1 = none)
        float hudPivot = 0.0f, hudSqueeze = 1.0f;
    };

    // [gta-recomp] A texture made on a helper thread (a free-threaded device: D3D11, Vulkan; HwBackend::makeTextureEarly): its
    // pixels go there, the texture comes back; the main thread takes it when it executes the TexCreate.
    struct EarlyTexture
    {
        uint32_t w = 0, h = 0;
        bool mipChain = false; // anisotropic filtering: buildMipChain there
        std::vector<uint32_t> pixels, mips;
        // [gta-recomp] a pack DDS made from its stored levels (levels: how many - 1 without anisotropic filtering)
        std::shared_ptr<const render::ReplacementImage> replacement;
        uint32_t levels = 1;
        size_t bytes = 0;
        std::unique_ptr<rhi::Texture> made;
        std::atomic<bool> done{false};
    };

    struct TexCreate
    {
        uint64_t key;
        uint32_t w, h;
        std::vector<uint32_t> pixels;
        std::vector<uint32_t> mips; // [gta-recomp] anisotropic filtering: levels 1.. after each other (buildMipChain)
        std::shared_ptr<EarlyTexture> early; // [gta-recomp] being made on a helper thread (pixels moved there)
        std::shared_ptr<const render::ReplacementImage> replacement; // [gta-recomp] decoded asynchronously
    };

    // [gta-recomp] The levels of a texture: level 0 (pixels) then its mips, as createTextureLevels takes them.
    uint32_t mipLevels(const std::vector<uint32_t> &pixels, const std::vector<uint32_t> &mips, uint32_t w, uint32_t h,
                       const void *(&levels)[16])
    {
        uint32_t count = 0;
        levels[count++] = pixels.data();
        size_t at = 0;
        for (uint32_t lw = w, lh = h; (lw > 1u || lh > 1u) && count < 16u;)
        {
            lw = std::max(1u, lw >> 1), lh = std::max(1u, lh >> 1);
            levels[count++] = mips.data() + at;
            at += size_t(lw) * lh;
        }
        return count;
    }

    // [gta-recomp] The mip chain of a w x h RGBA8 texture, levels 1.. down to 1x1 one after the other: each texel the
    // rounded mean of the 2x2 texels above it (edge texels repeated where a level is 1 wide or high) - what
    // GenerateMips / linear blits made on the GPU before, built on the GS stage instead (PERFORMANCE.md 21: a burst of new
    // textures was a burst of render-target textures and mip passes on the main thread).
    void buildMipChain(const std::vector<uint32_t> &base, uint32_t w, uint32_t h, std::vector<uint32_t> &out)
    {
        out.clear();
        size_t total = 0;
        for (uint32_t lw = w, lh = h; lw > 1u || lh > 1u;)
        {
            lw = std::max(1u, lw >> 1), lh = std::max(1u, lh >> 1);
            total += size_t(lw) * lh;
        }
        out.resize(total);
        const uint32_t *src = base.data();
        uint32_t *dst = out.data();
        for (uint32_t sw = w, sh = h; sw > 1u || sh > 1u;)
        {
            const uint32_t dw = std::max(1u, sw >> 1), dh = std::max(1u, sh >> 1);
            for (uint32_t y = 0; y < dh; ++y)
            {
                const uint32_t *r0 = src + size_t(std::min(2u * y, sh - 1u)) * sw;
                const uint32_t *r1 = src + size_t(std::min(2u * y + 1u, sh - 1u)) * sw;
                for (uint32_t x = 0; x < dw; ++x)
                {
                    const uint32_t x0 = std::min(2u * x, sw - 1u), x1 = std::min(2u * x + 1u, sw - 1u);
                    const uint32_t a = r0[x0], b = r0[x1], c = r1[x0], d = r1[x1];
                    // two channels at a time in 16-bit lanes: (a + b + c + d + 2) >> 2 per channel
                    const uint32_t lo = ((a & 0x00FF00FFu) + (b & 0x00FF00FFu) + (c & 0x00FF00FFu) + (d & 0x00FF00FFu) + 0x00020002u) >> 2;
                    const uint32_t hi = (((a >> 8) & 0x00FF00FFu) + ((b >> 8) & 0x00FF00FFu) + ((c >> 8) & 0x00FF00FFu) +
                                         ((d >> 8) & 0x00FF00FFu) + 0x00020002u) >> 2;
                    dst[size_t(y) * dw + x] = (lo & 0x00FF00FFu) | ((hi & 0x00FF00FFu) << 8);
                }
            }
            src = dst;
            dst += size_t(dw) * dh;
            sw = dw, sh = dh;
        }
    }

    struct RtUpload
    {
        uint32_t fbp, fbw, x, y, w, h;
        std::vector<uint32_t> pixels;
    };

    struct ClearCmd
    {
        HwState s; // target + scissor
        uint32_t rgba;
    };

    // [gta-recomp] A flat colour draw into the memory of a depth buffer, as a depth clear of the rows it covers.
    struct DepthFillCmd
    {
        uint32_t zbp, width, y0, y1; // depth-buffer pixels
        float value;                 // 0..1, same scale as HwVertex::z
    };

    // [gta-recomp] static vertex data of a native draw, kept on the GPU by content
    struct MeshCreate
    {
        uint64_t key = 0;
        uint32_t stride = 0, count = 0;
        std::vector<uint8_t> bytes;
    };

    struct NativeDrawCmd
    {
        HwState s;
        uint64_t meshKey = 0;
        uint32_t kind = 0, drawCount = 0;
        bool transient = false;
        uint32_t transientOffset = 0; // bytes into the frame's nativeBytes
        float constants[128] = {};    // the kind's 124 + (XYOFFSET x, y in pixels, Z scale, 0)
    };

    struct Frame
    {
        std::vector<Cmd> cmds;
        std::vector<HwVertex> vertices;
        std::vector<DrawCmd> draws;
        std::vector<TexCreate> textures;
        std::vector<uint64_t> texDeletes;
        std::vector<RtUpload> rtUploads;
        std::vector<uint32_t> rtDrops;
        std::vector<ClearCmd> clears;
        std::vector<DepthFillCmd> depthFills;
        std::vector<MeshCreate> meshes;
        std::vector<uint64_t> meshDeletes;
        std::vector<NativeDrawCmd> nativeDraws;
        std::vector<uint8_t> nativeBytes; // transient native vertices
        uint64_t nativeVertices = 0;
        bool had3d = false; // [gta-recomp] any depth-tested or native draw (widescreen presentation)
        int64_t inputNs = 0; // [gta-recomp] the input snapshot the game read for this frame (gsHwShownInputNs)
        int64_t queuedNs = 0, takenNs = 0; // [gta-recomp] put in / taken from the queue (PS2X_RHI_STATS: input delay by stage)
        int8_t wide3d = -1; // [gta-recomp] recorded for a wide picture (frameWide): draws squeezed into the buffer; -1: not asked
        int8_t shown = -1;  // [gta-recomp] its shape, decided when executed (decideShape): 1 wide, 0 4:3
        bool menu = false;  // [gta-recomp] has primitives the classifier names as the game's menu (GSHwAnchorMenu)
        uint64_t privRegs[15]{};
        // Display from CPU VRAM when no render target holds the displayed buffer.
        std::vector<uint32_t> cpuDisplay;
        uint32_t cpuDisplayW = 0, cpuDisplayH = 0;
        bool displayFromRt = false;
        uint32_t displayFbp = 0, displayX = 0, displayY = 0, displayW = 0, displayH = 0;
    };

    // [gta-recomp] Draw sources (gsHwDrawSourceReport): per executed frame, what the native renderer drew (native draws,
    // HUD layer, native post-effect passes) and what was still drawn from GS primitives, by where their GIF packets came
    // from (runtime/gs/gs_draw_source.h: a VU1 program, a C++ reproduction of one, PATH2, PATH3). Totals go to the perf
    // counters; the per-kind table is kept once asked for. Main thread (the executor and the control channel).
    class DrawSources
    {
    public:
        DrawSources()
        {
            const char *e = std::getenv("PS2X_HW_DRAW_SOURCES");
            m_log = e && e[0] && e[0] != '0';
            m_table = m_log;
        }

        void gsDraw(const Frame &f, const DrawCmd &d)
        {
            if (gsDrawSourcePacked(d.source)) // recorded from whole packets, not through the GS front end
            {
                ++m_frame.packed;
                m_frame.packedVertices += d.count;
                return;
            }
            ++m_frame.gs;
            m_frame.gsVertices += d.count;
            const uint32_t path = gsDrawSourcePath(d.source);
            ++(path == 1u ? (gsDrawSourceNative(d.source) ? m_frame.hle : m_frame.vu1) : path == 2u ? m_frame.path2 : m_frame.path3);
            if (!m_table)
                return;
            const HwState &s = d.s;
            float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
            bool far = true;
            for (uint32_t i = 0; i < d.count; ++i)
            {
                const HwVertex &v = f.vertices[d.first + i];
                x0 = std::min(x0, v.x);
                x1 = std::max(x1, v.x);
                y0 = std::min(y0, v.y);
                y1 = std::max(y1, v.y);
                far = far && v.z >= 0.99999f;
            }
            // covers its whole scissor (screen effects, fades, copies)
            const bool full = x0 <= s.scx0 + 1.0f && x1 >= s.scx1 && y0 <= s.scy0 + 1.0f && y1 >= s.scy1;
            const uint64_t key = (uint64_t(gsDrawSourceProgram(d.source)) << 48) | (uint64_t(d.source & 7u) << 45) |
                                 (uint64_t(s.fbp & 0x1FFu) << 36) | (uint64_t(s.fbw & 0x3Fu) << 30) | (uint64_t(s.tme & 1u) << 29) |
                                 (uint64_t(s.texIsRt & 1u) << 28) | (uint64_t(s.mode & 3u) << 26) | (uint64_t(s.abe & 1u) << 25) |
                                 (uint64_t(s.zte & 1u) << 24) | (uint64_t(s.ztst & 3u) << 22) | (uint64_t(s.zmsk & 1u) << 21) |
                                 (uint64_t(s.fst & 1u) << 20) | (uint64_t(s.fge & 1u) << 19) | (uint64_t(s.ate & 1u) << 18) |
                                 (uint64_t(full) << 17) | (uint64_t(far) << 16) |
                                 (uint64_t((s.ba & 3u) | (s.bb & 3u) << 2 | (s.bc & 3u) << 4 | (s.bd & 3u) << 6) << 8);
            Kind &k = m_kinds[key];
            ++k.draws;
            k.vertices += d.count;
            k.state = s;
        }
        void nativeDraw() { ++m_frame.native; }
        void hudDraw() { ++m_frame.hud; }
        void postDraws(uint32_t n) { m_frame.post += n; }

        // end of an executed frame
        void endFrame()
        {
            using C = ps2perf::Counter;
            ps2perf::add(C::GsDraws, m_frame.gs);
            ps2perf::add(C::GsVertices, m_frame.gsVertices);
            ps2perf::add(C::GsVu1, m_frame.vu1);
            ps2perf::add(C::GsHle, m_frame.hle);
            ps2perf::add(C::GsPath2, m_frame.path2);
            ps2perf::add(C::GsPath3, m_frame.path3);
            ps2perf::add(C::NativeDraws, m_frame.native);
            ps2perf::add(C::HudDraws, m_frame.hud);
            ps2perf::add(C::PostDraws, m_frame.post);
            ps2perf::add(C::PackedDraws, m_frame.packed);
            ps2perf::add(C::PackedVertices, m_frame.packedVertices);
            m_frame = {};
            ++m_frames;
            if (m_log)
            {
                const auto now = std::chrono::steady_clock::now();
                if (now - m_lastLog >= std::chrono::seconds(5))
                {
                    m_lastLog = now;
                    std::fprintf(stderr, "%s", report(true).c_str());
                }
            }
        }

        std::string report(bool reset)
        {
            if (!m_table)
            {
                m_table = true;
                m_frames = 0;
                m_kinds.clear();
                return "[gsdraws] collecting from now\n";
            }
            std::vector<std::pair<uint64_t, Kind>> kinds(m_kinds.begin(), m_kinds.end());
            std::sort(kinds.begin(), kinds.end(), [](const auto &a, const auto &b) { return a.second.draws > b.second.draws; });
            const double n = std::max<double>(1.0, static_cast<double>(m_frames));
            std::string out;
            char line[512];
            double draws = 0.0, vertices = 0.0;
            for (const auto &[key, k] : kinds)
            {
                draws += k.draws;
                vertices += k.vertices;
            }
            std::snprintf(line, sizeof(line), "[gsdraws] %llu frames: %.1f GS-path draws, %.0f vertices per frame, %zu kinds\n",
                          static_cast<unsigned long long>(m_frames), draws / n, vertices / n, kinds.size());
            out += line;
            for (const auto &[key, k] : kinds)
            {
                const uint32_t src = static_cast<uint32_t>(key >> 45) & 7u, prog = static_cast<uint32_t>(key >> 48);
                char from[32];
                if ((src & 3u) == 1u)
                    std::snprintf(from, sizeof(from), "%s:0x%04x", (src & 4u) ? "hle" : "vu1", prog);
                else
                    std::snprintf(from, sizeof(from), "path%u", src & 3u);
                const HwState &s = k.state;
                static const char *const kModes[] = {"tri", "line", "point", "?"};
                static const char *const kZtst[] = {"never", "always", "gequal", "greater"};
                std::snprintf(line, sizeof(line),
                              "[gsdraws] %8.2f draws %9.1f verts  %-11s fb %3u/%-2u %-5s %s%s blend %s(%u%u%u%u) z %s%s%s%s%s%s%s\n",
                              k.draws / n, k.vertices / n, from, s.fbp, s.fbw, kModes[s.mode & 3u],
                              s.tme ? (s.texIsRt ? "tex=rt" : "tex") : "flat", s.tme ? (s.fst ? " uv" : " stq") : "",
                              s.abe ? "on" : "off", s.ba, s.bb, s.bc, s.bd, s.zte ? kZtst[s.ztst & 3u] : "off",
                              s.zmsk ? " nowrite" : "", s.ate ? " atest" : "", s.fge ? " fog" : "",
                              (key >> 17) & 1u ? " full" : "", (key >> 16) & 1u ? " far" : "",
                              s.texIsRt ? (" rt=" + std::to_string(s.rtFbp)).c_str() : "");
                out += line;
            }
            if (reset)
            {
                m_frames = 0;
                m_kinds.clear();
            }
            return out;
        }

    private:
        struct Kind
        {
            uint64_t draws = 0, vertices = 0;
            HwState state{}; // the latest draw's
        };
        struct Counts
        {
            uint64_t gs = 0, gsVertices = 0, vu1 = 0, hle = 0, path2 = 0, path3 = 0, native = 0, hud = 0, post = 0;
            uint64_t packed = 0, packedVertices = 0;
        };
        Counts m_frame;
        uint64_t m_frames = 0;
        bool m_log = false, m_table = false;
        std::unordered_map<uint64_t, Kind> m_kinds;
        std::chrono::steady_clock::time_point m_lastLog = std::chrono::steady_clock::now();
    };

    std::mutex g_drawSourcesMutex;
    DrawSources g_drawSources; // guarded by g_drawSourcesMutex (the executor and gsHwDrawSourceReport)

    // [gta-recomp] The GS samples pixel X (coverage, colour, texture coordinates) at its integer position X; D3D at
    // X + 0.5. Draws move by half a pixel so each pixel samples where the GS would: without it a plain bilinear copy
    // (u = x + 0.5: texel X's centre on the GS) averaged texels X and X + 1, and VCS copies its frame every frame
    // (the shown picture, motion blur) - softer than the PS2 and shifted. PS2X_HW_PIXEL_CENTRE=0 = before.
    float gsPixelCentreOffset()
    {
        static const float offset = [] {
            const char *e = std::getenv("PS2X_HW_PIXEL_CENTRE");
            return (e && e[0] == '0') ? 0.0f : 0.5f;
        }();
        return offset;
    }

    // GS page geometry per PSM: 8 KiB pages.
    // [gta-recomp] PS2X_HW_LOG_DROPS, read once (it is tested per upload - thousands of times a second)
    bool logDrops()
    {
        static const bool on = std::getenv("PS2X_HW_LOG_DROPS") != nullptr;
        return on;
    }
    // [gta-recomp] PS2X_HW_RT_REUSE=0: render targets re-created as before - a new texture at every drop and growth, the
    // reflection map's FBP scaled by each draw's width (HwBackend::makeTarget, scaleFor)
    bool reuseTargets()
    {
        static const bool on = [] {
            const char *e = std::getenv("PS2X_HW_RT_REUSE");
            return !(e && e[0] == '0');
        }();
        return on;
    }
    // [gta-recomp] PS2X_GPU_TIMING (the RHI's GPU timer): a marker around each draw, named by its kind and target
    bool gpuTimingMarkers()
    {
        static const bool on = std::getenv("PS2X_GPU_TIMING") != nullptr;
        return on;
    }

    void pageSize(uint32_t psm, uint32_t &w, uint32_t &h)
    {
        switch (psm)
        {
        case GS_PSM_CT16: case GS_PSM_CT16S: case GS_PSM_Z16: case GS_PSM_Z16S: w = 64; h = 64; break;
        case GS_PSM_T8: case GS_PSM_T8H: w = 128; h = 64; break;
        case GS_PSM_T4: case GS_PSM_T4HL: case GS_PSM_T4HH: w = 128; h = 128; break;
        default: w = 64; h = 32; break;
        }
        // T8H/T4HL/T4HH live in a CT32 layout
        if (psm == GS_PSM_T8H || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH)
        {
            w = 64;
            h = 32;
        }
    }

    // Pages [first, first+count) touched by a w x h rect at (x,y) of a buffer at `blockBase` (256-byte blocks).
    void pageRange(uint32_t blockBase, uint32_t bufWidth64, uint32_t psm, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                   uint32_t &first, uint32_t &last)
    {
        uint32_t pw, ph;
        pageSize(psm, pw, ph);
        const uint32_t perRow = std::max<uint32_t>(1u, (std::max<uint32_t>(bufWidth64, 1u) * 64u + pw - 1u) / pw);
        const uint32_t basePage = blockBase / 32u;
        const uint32_t row0 = y / ph, row1 = (y + std::max<uint32_t>(h, 1u) - 1u) / ph;
        (void)x;
        (void)w;
        first = basePage + row0 * perRow;
        last = basePage + (row1 + 1u) * perRow + ((blockBase % 32u) ? 1u : 0u); // exclusive
    }

    uint64_t mix64(uint64_t h, uint64_t v)
    {
        h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        return h * 0xFF51AFD7ED558CCDull;
    }

    // ------------------------------------------------------------------------------------------------------
    // [gta-recomp] Execution on the renderer's RHI device (main thread). Render targets keep PS2 rows top-down
    // (texture row = PS2 row); the vertex shader flips Y for D3D's NDC.
    // ------------------------------------------------------------------------------------------------------
    struct DrawConstants // render/shaders/gs_draw.hlsl, cbuffer Draw
    {
        float targetRtFix[4];
        int32_t tmeTfxTccFst[4];
        int32_t linearIipFgeAte[4];
        int32_t atstArefAfailRt[4];
        int32_t texSizeWrap[4];
        int32_t region[4];
        float rtOrigin[4];
        int32_t blendCPremul[4];
        float fogColor[4];
    };

    struct QuadConstants // render/shaders/gs_util.hlsl, cbuffer Quad
    {
        float rect[4];
        float uvRect[4];
        float color[4];
        float params[4];
    };

    // GS blend (A - B) * C / 128 + D as a fixed-function blend (C from the shader's second output); false when not
    // expressible (drawn unblended).
    struct GsBlend
    {
        bool enable = false;
        rhi::BlendFactor src = rhi::BlendFactor::One;
        rhi::BlendFactor dst = rhi::BlendFactor::Zero;
        rhi::BlendOp op = rhi::BlendOp::Add;
        int premul = 0; // 0: none, 1: rgb *= C, 2: rgb *= 1 + C
    };

    bool gsBlend(const HwState &s, GsBlend &b)
    {
        using F = rhi::BlendFactor;
        using O = rhi::BlendOp;
        b = GsBlend{};
        if (!s.abe)
            return true;
        const int A = s.ba, B = s.bb, D = s.bd;
        O op = O::Add;
        F src = F::One, dst = F::Zero;
        if (A == B)
        {
            // (X - X) * C + D = D
            if (D == 0) { src = F::One; dst = F::Zero; }
            else if (D == 1) { src = F::Zero; dst = F::One; }
            else { src = F::Zero; dst = F::Zero; }
        }
        else if (A == 0 && B == 1)
        {
            if (D == 1) { src = F::Src1Alpha; dst = F::InvSrc1Alpha; }                          // Cs*C + Cd*(1-C)
            else if (D == 0) { b.premul = 2; op = O::Subtract; src = F::One; dst = F::Src1Alpha; } // Cs*(1+C) - Cd*C
            else { src = F::Src1Alpha; dst = F::Src1Alpha; op = O::Subtract; }                 // (Cs - Cd)*C
        }
        else if (A == 0 && B == 2)
        {
            b.premul = 1; // Cs*C + D
            src = F::One;
            dst = (D == 1) ? F::One : F::Zero;
            if (D == 0) { b.premul = 2; src = F::One; dst = F::Zero; }
        }
        else if (A == 1 && B == 0)
        {
            if (D == 0) { src = F::InvSrc1Alpha; dst = F::Src1Alpha; } // Cs*(1-C) + Cd*C
            else return false;
        }
        else if (A == 2 && B == 0)
        {
            b.premul = 1; // D - Cs*C
            if (D == 1) { op = O::ReverseSubtract; src = F::One; dst = F::One; }
            else { src = F::Zero; dst = F::Zero; }
        }
        else if (A == 2 && B == 1)
        {
            if (D == 1) { src = F::Zero; dst = F::InvSrc1Alpha; }                               // Cd*(1-C)
            else if (D == 0) { op = O::ReverseSubtract; src = F::One; dst = F::Src1Alpha; }       // Cs - Cd*C
            else return false;
        }
        else if (A == 1 && B == 2)
        {
            if (D == 0) { src = F::One; dst = F::Src1Alpha; }  // Cs + Cd*C
            else if (D == 2) { src = F::Zero; dst = F::Src1Alpha; } // Cd*C
            else return false;
        }
        else
        {
            return false;
        }
        b.enable = true;
        b.src = src;
        b.dst = dst;
        b.op = op;
        return true;
    }

    // F12 / PS2X_HW_DUMP: one line per draw of the dumped frame (state + bounds).
    void logDraw(const Frame &f, const DrawCmd &d)
    {
        const HwState &s = d.s;
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, z0 = 1e9f, z1 = -1e9f;
        uint32_t rgbaOr = 0, fogMin = 255;
        for (uint32_t i = 0; i < d.count; ++i)
        {
            const HwVertex &v = f.vertices[d.first + i];
            x0 = std::min(x0, v.x); x1 = std::max(x1, v.x); y0 = std::min(y0, v.y); y1 = std::max(y1, v.y);
            z0 = std::min(z0, v.z); z1 = std::max(z1, v.z);
            rgbaOr |= v.r | (v.g << 8) | (v.b << 16) | (uint32_t(v.a) << 24);
            fogMin = std::min<uint32_t>(fogMin, static_cast<uint32_t>(v.fog * 255.0f));
        }
        std::fprintf(stderr,
                     "[draw] n=%u fbp=%u zbp=%u zte=%u ztst=%u zmsk=%u tme=%u tex(psm? key=%llx %ux%u rt=%u) tfx=%u tcc=%u abe=%u blend=%u%u%u%u fix=%u ate=%u atst=%u aref=%u afail=%u fge=%u fogMin=%u fogCol=%u,%u,%u fbmsk=%x prim=%u iip=%u fst=%u lin=%u wrap=%u,%u bounds=(%.0f,%.0f)-(%.0f,%.0f) z=%.8f..%.8f rgbaOr=%08x sc=%u,%u-%u,%u\n",
                     d.count, s.fbp, s.zbp, s.zte, s.ztst, s.zmsk, s.tme, (unsigned long long)s.texKey, s.tw, s.th, s.texIsRt,
                     s.tfx, s.tcc, s.abe, s.ba, s.bb, s.bc, s.bd, s.fix, s.ate, s.atst, s.aref, s.afail, s.fge, fogMin,
                     s.fogR, s.fogG, s.fogB, s.fbmsk, s.mode, s.iip, s.fst, s.linear, s.wms, s.wmt, x0, y0, x1, y1, z0, z1, rgbaOr,
                     s.scx0, s.scy0, s.scx1, s.scy1);
        if (s.texIsRt) // [gta-recomp] which render target a frame copy / screen effect reads, and where in it
            std::fprintf(stderr, "[draw]   src rt fbp=%u at %u,%u tex %ux%u tbw? st=(%.1f,%.1f)\n", s.rtFbp, s.rtOx, s.rtOy, s.tw, s.th,
                         d.count ? f.vertices[d.first].s : 0.0f, d.count ? f.vertices[d.first].t : 0.0f);
        // PS2X_HW_DUMP_ALLVERTS=1: every vertex of every draw (post-effect strips)
        static const bool allVerts = std::getenv("PS2X_HW_DUMP_ALLVERTS") != nullptr;
        if (d.count <= 96 || s.texIsRt || allVerts) // [gta-recomp] small draws (overlays, fades) and copies: their vertices
            for (uint32_t i = 0; i < (allVerts ? d.count : std::min<uint32_t>(d.count, 96u)); ++i)
            {
                const HwVertex &v = f.vertices[d.first + i];
                std::fprintf(stderr, "[draw]   v%u (%.1f, %.1f, %.6f) rgba %02x %02x %02x %02x stq %.4f %.4f %.3f\n", i, v.x, v.y, v.z,
                             v.r, v.g, v.b, v.a, v.s, v.t, v.q);
            }
    }

    void writePpm(const std::string &path, const uint8_t *rgba, uint32_t w, uint32_t h)
    {
        if (FILE *f = std::fopen(path.c_str(), "wb"))
        {
            std::fprintf(f, "P6 %u %u 255 ", w, h);
            for (size_t i = 0; i < size_t(w) * h; ++i)
                std::fwrite(&rgba[i * 4u], 1, 3, f);
            std::fclose(f);
            std::fprintf(stderr, "[gs-hw] dumped %s (%ux%u)\n", path.c_str(), w, h);
        }
    }

    class RhiExecutor
    {
    public:
        RhiExecutor(rhi::Device &device, uint32_t scale, uint32_t msaa, uint32_t anisotropy)
            : m_dev(device), m_scale(scale), m_msaa(msaa), m_anisotropy(anisotropy > 1u ? anisotropy : 0u)
        {
            const render::GsShaders &sh = render::gsShaders(m_dev.backend()); // [gta-recomp] the device's backend (DXBC / SPIR-V)
            m_drawCb = m_dev.createBuffer({sizeof(DrawConstants), rhi::BufferUsage::Constant, true, "gs draw constants"});
            m_quadCb = m_dev.createBuffer({sizeof(QuadConstants), rhi::BufferUsage::Constant, true, "gs quad constants"});
            m_worldCb = m_dev.createBuffer({128u * sizeof(float), rhi::BufferUsage::Constant, true, "gs native draw constants"});
            m_point = m_dev.createSampler({rhi::Filter::Nearest, rhi::Wrap::Clamp, rhi::Wrap::Clamp});
            m_linear = m_dev.createSampler({rhi::Filter::Linear, rhi::Wrap::Clamp, rhi::Wrap::Clamp});
            for (int u = 0; u < 2; ++u) // [gta-recomp] the GS bilinear's texels by Gather, per wrap mode (U, V: repeat / clamp)
                for (int v = 0; v < 2; ++v)
                    m_gatherSampler[u][v] = m_dev.createSampler({rhi::Filter::Nearest, u ? rhi::Wrap::Repeat : rhi::Wrap::Clamp,
                                                                 v ? rhi::Wrap::Repeat : rhi::Wrap::Clamp});
            if (m_anisotropy) // [gta-recomp] per wrap mode (U, V: repeat / clamp)
                for (int u = 0; u < 2; ++u)
                    for (int v = 0; v < 2; ++v)
                        m_anisoSampler[u][v] = m_dev.createSampler({rhi::Filter::Linear, u ? rhi::Wrap::Repeat : rhi::Wrap::Clamp,
                                                                    v ? rhi::Wrap::Repeat : rhi::Wrap::Clamp, m_anisotropy});
            rhi::PipelineDesc q;
            q.vertex = sh.quadVS;
            q.topology = rhi::Topology::TriangleStrip;
            q.pixel = sh.blitPS;
            q.name = "gs blit";
            m_blit = m_dev.createPipeline(q);
            q.pixel = sh.fillColorPS;
            q.name = "gs fill colour";
            m_fillColor = m_dev.createPipeline(q);
            q.pixel = sh.fillDepthPS;
            q.blend.writeMask = 0;
            q.depth = {true, true, rhi::Compare::Always};
            q.name = "gs fill depth";
            m_fillDepth = m_dev.createPipeline(q);
            m_ready = m_drawCb && m_quadCb && m_worldCb && m_point && m_linear && m_blit && m_fillColor && m_fillDepth;
        }

        bool ready() const { return m_ready; }

        // [gta-recomp] Native frame: the picture's targets at w x h (0: the integer scale like every other target)
        void setFrameSize(uint32_t w, uint32_t h)
        {
            m_frameScale = (w >= 64u && h >= 64u && w <= 8192u && h <= 8192u) ? Scale{w / 640.0f, h / 448.0f} : Scale{0.0f, 0.0f};
        }
        rhi::Device &device() { return m_dev; }
        rhi::Texture *display() const { return m_display.get(); }
        uint32_t displayWidth() const { return m_display ? m_display->desc().width : m_displayW * m_scale; }
        uint32_t displayHeight() const { return m_display ? m_display->desc().height : m_displayH * m_scale; }

        void execute(const Frame &f, bool logDraws)
        {
            m_execWide = f.shown != 0; // [gta-recomp] the shape the frame's drawing is for (shapeOf)
            m_dev.pushMarker("gs frame");
            const size_t bytes = f.vertices.size() * sizeof(HwVertex);
            if (bytes)
            {
                if (!m_vb || m_vbCapacity < bytes)
                {
                    m_vbCapacity = std::max<size_t>(bytes + bytes / 2, 1u << 20);
                    m_vb = m_dev.createBuffer({m_vbCapacity, rhi::BufferUsage::Vertex, true, "gs vertices"});
                }
                m_dev.updateBuffer(*m_vb, f.vertices.data(), bytes);
            }
            if (!f.nativeBytes.empty())
            {
                if (!m_nativeVb || m_nativeVbCapacity < f.nativeBytes.size())
                {
                    m_nativeVbCapacity = std::max<size_t>(f.nativeBytes.size() + f.nativeBytes.size() / 2, 1u << 20);
                    m_nativeVb = m_dev.createBuffer({m_nativeVbCapacity, rhi::BufferUsage::Vertex, true, "gs native transient vertices"});
                }
                m_dev.updateBuffer(*m_nativeVb, f.nativeBytes.data(), f.nativeBytes.size());
            }
            std::lock_guard<std::mutex> sourcesLock(g_drawSourcesMutex); // [gta-recomp] DrawSources
            DrawSources &sources = g_drawSources;
            for (size_t ci = 0; ci < f.cmds.size(); ++ci)
            {
                const Cmd &c = f.cmds[ci];
                // [gta-recomp] VCS's bloom / trails chains as native passes (nativePost): the commands it took are done
                if (c.type == CmdType::Draw)
                    if (const size_t taken = nativePost(f, ci, logDraws))
                    {
                        uint32_t draws = 0;
                        for (size_t i = ci; i < ci + taken && i < f.cmds.size(); ++i)
                            draws += f.cmds[i].type == CmdType::Draw ? 1u : 0u;
                        sources.postDraws(draws);
                        ci += taken - 1;
                        continue;
                    }
                switch (c.type)
                {
                case CmdType::TexCreate:
                {
                    const TexCreate &t = f.textures[c.index];
                    rhi::TextureDesc d;
                    d.width = t.w;
                    d.height = t.h;
                    d.usage = rhi::UsageSampled;
                    d.name = t.replacement ? "replacement texture" : "gs texture"; // [gta-recomp]
                    d.mipLevels = m_anisotropy ? 0u : 1u; // [gta-recomp] anisotropic filtering samples a mip chain
                    const auto createStart = std::chrono::steady_clock::now(); // [gta-recomp] tex_create_us / tex_kb
                    if (t.early) // [gta-recomp] made on a helper thread (makeTextureEarly / makeReplacementEarly)
                    {
                        t.early->done.wait(false);
                        m_textures[t.key] = std::move(t.early->made);
                    }
                    else if (t.replacement && t.replacement->dds()) // [gta-recomp] a pack DDS: its levels as stored (level 0
                    {                                          // alone without anisotropic filtering, which reads mips)
                        const render::ReplacementImage &r = *t.replacement;
                        const void *levels[16];
                        const uint32_t n = m_anisotropy ? std::min<uint32_t>(uint32_t(r.levelOffsets.size()), 16u) : 1u;
                        for (uint32_t i = 0; i < n; ++i)
                            levels[i] = r.levels.data() + r.levelOffsets[i];
                        d.format = r.format;
                        m_textures[t.key] = m_dev.createTextureLevels(d, levels, n);
                        if (!m_textures[t.key])
                            std::fprintf(stderr, "[textures] texture %ux%u (format %u) not made\n", t.w, t.h, unsigned(r.format));
                    }
                    else if (t.replacement) // [gta-recomp] a pack PNG (decoded on the loader thread); mips by the GPU (d.mipLevels)
                        m_textures[t.key] = m_dev.createTexture(d, t.replacement->pixels.data(), t.w * 4u);
                    else if (!t.mips.empty()) // [gta-recomp] the chain built on the GS stage: one plain creation (buildMipChain)
                    {
                        const void *levels[16];
                        m_textures[t.key] = m_dev.createTextureLevels(d, levels, mipLevels(t.pixels, t.mips, t.w, t.h, levels));
                    }
                    else
                        m_textures[t.key] = m_dev.createTexture(d, t.pixels.data(), t.w * 4u);
                    ps2perf::add(ps2perf::Counter::TexCreateUs, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                                    std::chrono::steady_clock::now() - createStart).count()));
                    ps2perf::add(ps2perf::Counter::TexKBytes,
                                 ((t.early ? t.early->bytes : t.replacement ? t.replacement->gpuBytes(m_anisotropy != 0) : (t.pixels.size() + t.mips.size()) * 4u) + 1023u) / 1024u);
                    // [gta-recomp] the dump reads RGBA8 texels: a PNG's, or the GS's when they stayed here (not moved to a helper)
                    const std::vector<uint32_t> &texels = t.replacement ? t.replacement->pixels : t.pixels;
                    const uint32_t *pixels = texels.empty() ? nullptr : texels.data();
                    // [gta-recomp] PS2X_HW_DUMP_TEX=<key hex>: write that decoded texture (hw_tex_<key>.ppm, alpha dropped)
                    static const uint64_t dumpTex = std::getenv("PS2X_HW_DUMP_TEX") ? std::strtoull(std::getenv("PS2X_HW_DUMP_TEX"), nullptr, 16) : 0u;
                    if (dumpTex && t.key == dumpTex && pixels)
                    {
                        char name[64];
                        std::snprintf(name, sizeof(name), "hw_tex_%llx.ppm", static_cast<unsigned long long>(t.key));
                        writePpm(name, reinterpret_cast<const uint8_t *>(pixels), t.w, t.h);
                        std::vector<uint32_t> alpha(size_t(t.w) * t.h);
                        for (size_t i = 0; i < alpha.size(); ++i)
                            alpha[i] = (pixels[i] >> 24) * 0x010101u; // alpha as grey
                        std::snprintf(name, sizeof(name), "hw_tex_%llx_alpha.ppm", static_cast<unsigned long long>(t.key));
                        writePpm(name, reinterpret_cast<const uint8_t *>(alpha.data()), t.w, t.h);
                    }
                    break;
                }
                case CmdType::TexDelete:
                {
                    const auto deleteStart = std::chrono::steady_clock::now(); // [gta-recomp] tex_create_us: releases too
                    m_textures.erase(f.texDeletes[c.index]);
                    ps2perf::add(ps2perf::Counter::TexCreateUs, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                                    std::chrono::steady_clock::now() - deleteStart).count()));
                    break;
                }
                case CmdType::RtDrop:
                    endPass();
                    dropRt(f.rtDrops[c.index]);
                    m_hud.erase(f.rtDrops[c.index]);
                    break;
                case CmdType::RtUpload:
                    hudSceneDraw(f.rtUploads[c.index].fbp);
                    rtUpload(f.rtUploads[c.index]);
                    break;
                case CmdType::Clear:
                    hudSceneDraw(f.clears[c.index].s.fbp);
                    clear(f.clears[c.index]);
                    break;
                case CmdType::HudDraw:
                {
                    const DrawCmd &d = f.draws[c.index];
                    sources.hudDraw();
                    if (logDraws)
                    {
                        std::fprintf(stderr, "[draw] (hud layer)\n");
                        logDraw(f, d);
                    }
                    m_shapeOf[d.s.fbp] = m_execWide; // [gta-recomp] the pause menu draws only into the layer
                    HudLayer &layer = m_hud[d.s.fbp];
                    if (layer.stale || layer.closed)
                    {
                        layer.vertices.clear();
                        layer.draws.clear();
                        layer.stale = layer.closed = false;
                    }
                    appendHudDraw(layer, f, d);
                    break;
                }
                case CmdType::DepthFill:
                    depthFill(f.depthFills[c.index], logDraws);
                    break;
                case CmdType::Draw:
                {
                    if (logDraws)
                        logDraw(f, f.draws[c.index]);
                    // [gta-recomp] PS2X_HW_SKIP=<mask> (diagnostics): 1 skips draws that sample a render target (frame
                    // copies, screen effects), 2 skips untextured full-screen draws, 4 skips everything but native draws
                    static const unsigned skip = std::getenv("PS2X_HW_SKIP") ? std::strtoul(std::getenv("PS2X_HW_SKIP"), nullptr, 0) : 0u;
                    const HwState &ds = f.draws[c.index].s;
                    hudOnDraw(ds);
                    if (skip && (((skip & 1u) && ds.texIsRt) || (skip & 4u)))
                        break;
                    sources.gsDraw(f, f.draws[c.index]);
                    const bool timed = gpuTimingMarkers();
                    if (timed)
                    {
                        char name[96];
                        std::snprintf(name, sizeof(name), "gs draw fbp %u%s%s tex %llx %ux%u", ds.fbp,
                                      ds.tme ? (ds.texIsRt ? " tex=rt" : " tex") : "", ds.abe ? " blend" : "",
                                      static_cast<unsigned long long>(ds.tme ? ds.texKey : 0u), ds.tw, ds.th);
                        m_dev.pushMarker(name);
                    }
                    draw(ds, f.draws[c.index].first, f.draws[c.index].count, nullptr);
                    if (timed)
                    {
                        endPass();
                        m_dev.popMarker();
                    }
                    // [gta-recomp] PS2X_HW_DUMP_STEPS=1: while draws are logged, write the target after every draw that
                    // covers the whole screen or reads a render target (where a frame goes black, step by step)
                    static const bool steps = std::getenv("PS2X_HW_DUMP_STEPS") != nullptr;
                    if (steps && logDraws)
                    {
                        const DrawCmd &dc = f.draws[c.index];
                        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
                        for (uint32_t i = 0; i < dc.count; ++i)
                        {
                            const HwVertex &v = f.vertices[dc.first + i];
                            x0 = std::min(x0, v.x), x1 = std::max(x1, v.x), y0 = std::min(y0, v.y), y1 = std::max(y1, v.y);
                        }
                        const bool full = x0 <= 1.0f && x1 >= 639.0f && y0 <= 1.0f && y1 >= 447.0f;
                        auto it = m_rts.find(ds.fbp);
                        if ((full || ds.texIsRt) && it != m_rts.end())
                        {
                            endPass();
                            static int step = 0;
                            dumpTexture(readable(it->second), "step" + std::to_string(step++) + "_fbp" + std::to_string(ds.fbp));
                        }
                    }
                    break;
                }
                case CmdType::MeshCreate:
                {
                    const MeshCreate &m = f.meshes[c.index];
                    if (m.bytes.empty()) // [gta-recomp] a merged native group's mesh that was cached (nativeMerge)
                        break;
                    m_meshes[m.key] = m_dev.createBuffer({m.bytes.size(), rhi::BufferUsage::Vertex, false, "gs native mesh"}, m.bytes.data());
                    break;
                }
                case CmdType::MeshDelete:
                    m_meshes.erase(f.meshDeletes[c.index]);
                    break;
                case CmdType::NativeDraw:
                {
                    const NativeDrawCmd &n = f.nativeDraws[c.index];
                    if (logDraws)
                        std::fprintf(stderr, "[draw] native kind=%u n=%u mesh=%016llx fbp=%u zbp=%u tme=%u abe=%u fge=%u\n", n.kind, n.drawCount,
                                     (unsigned long long)n.meshKey, n.s.fbp, n.s.zbp, n.s.tme, n.s.abe, n.s.fge);
                    hudSceneDraw(n.s.fbp);
                    sources.nativeDraw();
                    const bool timed = gpuTimingMarkers();
                    if (timed)
                    {
                        char name[96];
                        std::snprintf(name, sizeof(name), "native kind %u fbp %u%s%s%s", n.kind, n.s.fbp, n.s.tme ? " tex" : "",
                                      n.s.abe ? " blend" : "", n.s.zte ? "" : " noz");
                        m_dev.pushMarker(name);
                    }
                    draw(n.s, 0, n.drawCount, &n);
                    if (timed)
                    {
                        endPass();
                        m_dev.popMarker();
                    }
                    break;
                }
                }
            }
            sources.endFrame();
            endPass();
            m_dev.popMarker();
        }

        void composeDisplay(const Frame &f)
        {
            m_displayW = f.displayW;
            m_displayH = f.displayH;
            uint32_t w = m_displayW * m_scale, h = m_displayH * m_scale;
            // [gta-recomp] native HUD layer: the picture at the output size (its aspect), the HUD drawn over it there
            const HudLayer *layer = nullptr;
            if ((g_hudLayer.load(std::memory_order_relaxed) || m_frameScale.x > 0.0f) && w && h)
            {
                uint32_t ow = g_outputW.load(std::memory_order_relaxed), oh = g_outputH.load(std::memory_order_relaxed);
                // PS2X_HW_OUTPUT=WxH (testing): the output size of a full-screen window whatever the window
                static const std::pair<uint32_t, uint32_t> forced = [] {
                    unsigned fw = 0, fh = 0;
                    if (const char *e = std::getenv("PS2X_HW_OUTPUT"))
                        if (std::sscanf(e, "%ux%u", &fw, &fh) != 2)
                            fw = fh = 0;
                    return std::pair<uint32_t, uint32_t>(fw, fh);
                }();
                if (forced.first && forced.second)
                {
                    ow = forced.first;
                    oh = forced.second;
                }
                if (ow >= 64u && oh >= 64u && ow <= 8192u && oh <= 8192u)
                {
                    // never below the render targets' resolution (a small window; screenshots are taken larger)
                    const double up = std::max(1.0, static_cast<double>(h) / oh);
                    w = static_cast<uint32_t>(std::lround(ow * up));
                    h = static_cast<uint32_t>(std::lround(oh * up));
                }
                auto it = m_hud.find(f.displayFbp);
                if (f.displayFromRt && it != m_hud.end() && !it->second.stale && !it->second.draws.empty())
                    layer = &it->second;
                static const bool rtLog = std::getenv("PS2X_HW_RT_LOG") != nullptr; // [gta-recomp] the targets, every 5 s
                static auto rtLast = std::chrono::steady_clock::now();
                if (rtLog && std::chrono::steady_clock::now() - rtLast > std::chrono::seconds(5))
                {
                    rtLast = std::chrono::steady_clock::now();
                    for (const auto &[fbp, rt] : m_rts)
                    {
                        const TargetRole *role = targetRole(fbp, 0u);
                        std::fprintf(stderr, "[gs-hw] target fbp %u: %ux%u PS2 pixels, %ux%u texels (scale %.2f x %.2f, %u samples)%s%s\n", fbp,
                                     rt.w, rt.h, rt.color ? rt.color->desc().width : 0u, rt.color ? rt.color->desc().height : 0u, rt.scale.x,
                                     rt.scale.y, rt.samples, role ? " - " : "", role ? role->name : "");
                    }
                }
                static const bool log = std::getenv("PS2X_HW_HUD_LOG") != nullptr; // the layers, every 2 s
                static auto last = std::chrono::steady_clock::now();
                if (log && std::chrono::steady_clock::now() - last > std::chrono::seconds(2))
                {
                    last = std::chrono::steady_clock::now();
                    std::string s;
                    for (const auto &[fbp, l] : m_hud)
                        s += " " + std::to_string(fbp) + (l.stale ? "(stale," : "(") + std::to_string(l.draws.size()) + " draws)";
                    std::fprintf(stderr, "[gs-hw] hud layer: display fbp %u at %u,%u fromRt %d, output %ux%u, layers:%s\n", f.displayFbp,
                                 f.displayX, f.displayY, f.displayFromRt ? 1 : 0, w, h, s.c_str());
                }
            }
            if (!w || !h)
                return;
            if (!m_display || m_display->desc().width != w || m_display->desc().height != h)
            {
                endPass();
                m_display = m_dev.createTexture({w, h, rhi::Format::RGBA8, rhi::UsageRenderTarget | rhi::UsageSampled, "gs display"});
            }
            auto it = m_rts.find(f.displayFbp);
            if (f.displayFromRt && it != m_rts.end())
            {
                // the PS2 frame buffer's alpha is not shown: written as 1
                Rt &rt = it->second;
                rhi::Texture &shown = readable(rt);
                const float tw = static_cast<float>(shown.desc().width), th = static_cast<float>(shown.desc().height);
                const Scale rs = rt.scale;
                const float x0 = f.displayX * rs.x, y0 = f.displayY * rs.y;
                const float sw = m_displayW * rs.x, sh = m_displayH * rs.y;
                const bool scaled = std::fabs(w - sw) > 0.5f || std::fabs(h - sh) > 0.5f;
                blit(shown, {x0 / tw, y0 / th, (x0 + sw) / tw, (y0 + sh) / th}, *m_display, 0, 0, w, h, scaled, true);
                if (layer)
                    drawHudLayer(*layer, f.displayX, f.displayY);
            }
            else if (!f.cpuDisplay.empty())
            {
                static int logged = 0; // [gta-recomp] normal for uploaded images (loading screens), suspicious in play
                if (logged < 50 && f.had3d)
                {
                    ++logged;
                    std::fprintf(stderr, "[gs-hw] 3D frame displayed from CPU VRAM: fbp=%u fromRt=%d; targets:%s\n", f.displayFbp,
                                 f.displayFromRt ? 1 : 0, describeTargets().c_str());
                }
                auto tmp = m_dev.createTexture({f.cpuDisplayW, f.cpuDisplayH, rhi::Format::RGBA8, rhi::UsageSampled, "gs cpu display"},
                                               f.cpuDisplay.data(), f.cpuDisplayW * 4u);
                if (tmp)
                    blit(*tmp, {0.0f, 0.0f, 1.0f, 1.0f}, *m_display, 0, 0, w, h, true, true);
            }
            else
            {
                // [gta-recomp] nothing to show: a black frame. Logged - a candidate for the black-flash bug
                static int logged = 0;
                if (logged < 50)
                {
                    ++logged;
                    std::fprintf(stderr, "[gs-hw] display filled black: fbp=%u fromRt=%d %ux%u at %u,%u; targets:%s\n", f.displayFbp,
                                 f.displayFromRt ? 1 : 0, f.displayW, f.displayH, f.displayX, f.displayY, describeTargets().c_str());
                }
                fill(m_display.get(), nullptr, 0, 0, w, h, {0.0f, 0.0f, 0.0f, 1.0f}, 0.0f);
            }
            endPass();
        }

        // F12 / PS2X_HW_DUMP: render targets and the display as PPM files.
        void dump()
        {
            endPass();
            std::vector<uint8_t> px;
            for (auto &[fbp, rt] : m_rts)
                dumpTexture(readable(rt), "rt_" + std::to_string(fbp));
            if (m_display)
                dumpTexture(*m_display, "display");
        }

        std::string describeTargets() const
        {
            std::string s;
            for (const auto &[fbp, rt] : m_rts)
                s += " " + std::to_string(fbp) + "(" + std::to_string(rt.w) + "x" + std::to_string(rt.h) + ")";
            return s;
        }

    private:
        // [gta-recomp] target pixels per PS2 pixel: the integer internal scale, or for the picture's own targets in the
        // native frame the window's size over 640x448 (setFrameSize)
        struct Scale
        {
            float x = 1.0f, y = 1.0f;
            bool operator==(const Scale &o) const { return x == o.x && y == o.y; }
            bool operator!=(const Scale &o) const { return !(*this == o); }
        };
        struct Rt
        {
            std::unique_ptr<rhi::Texture> color;
            uint32_t w = 0, h = 0; // PS2 pixels
            Scale scale;
            uint32_t samples = 1;                    // [gta-recomp] MSAA (GSHwOptions::msaa)
            std::unique_ptr<rhi::Texture> resolved;  // multisampled: what reads see (readable)
            bool dirty = false;                      // multisampled: drawn since the last resolve
        };
        struct Depth
        {
            std::unique_ptr<rhi::Texture> tex;
            uint32_t w = 0, h = 0; // PS2 pixels
            Scale scale;
        };

        // the picture's targets (640 wide: the drawn frame, the shown copy, the Z buffer) take the native frame's scale;
        // [gta-recomp] so do the targets the game names windowScale (gsHwSetTargetRole: the reflection map), unless the GS
        // bloom draws into the same memory (its look is its 1x buffers). One scale per FBP (the GPU target is one per FBP):
        // the native bloom's write-back into FBP 420 at width 4 takes the reflection map's scale too - by its own width's
        // role it flipped the target between the two scales, a new texture and a rescaling blit each time, several times a
        // frame (PS2X_HW_RT_REUSE=0: by the draw's width as before)
        Scale scaleFor(uint32_t fbp, uint32_t fbw) const
        {
            if (m_frameScale.x > 0.0f && fbw == 10u)
                return m_frameScale;
            static const bool roles = [] {
                const char *e = std::getenv("PS2X_HW_REFLECTION_SCALE");
                return !(e && e[0] == '0');
            }();
            if (m_frameScale.x > 0.0f && roles && (postNative() & 1u))
            {
                if (reuseTargets() ? windowScaleAt(fbp) : [&] {
                        const TargetRole *r = targetRole(fbp, fbw);
                        return r && r->windowScale;
                    }())
                    return m_frameScale;
            }
            return {static_cast<float>(m_scale), static_cast<float>(m_scale)};
        }
        // [gta-recomp] MSAA (GSHwOptions::msaa): the picture's targets and their depth buffers are multisampled; reads
        // of such a target (render-target textures, copies, the display) see a resolved copy, made when it was drawn to
        // since the last read
        uint32_t samplesFor(uint32_t fbw) const { return fbw == 10u ? m_msaa : 1u; }

        rhi::Texture &readable(Rt &rt)
        {
            if (rt.samples <= 1u)
                return *rt.color;
            const rhi::TextureDesc &cd = rt.color->desc();
            if (!rt.resolved || rt.resolved->desc().width != cd.width || rt.resolved->desc().height != cd.height)
            {
                endPass();
                rt.resolved = m_dev.createTexture({cd.width, cd.height, rhi::Format::RGBA8, rhi::UsageSampled | rhi::UsageRenderTarget,
                                                   "gs target resolved"});
                rt.dirty = true;
            }
            if (rt.dirty)
            {
                endPass();
                m_dev.resolveTexture(*rt.resolved, *rt.color);
                rt.dirty = false;
            }
            return *rt.resolved;
        }

        // a PS2 coordinate (an edge) in target pixels
        static uint32_t px(float v, float scale) { return static_cast<uint32_t>(std::max(0l, std::lround(v * scale))); }

        void dumpTexture(rhi::Texture &t, const std::string &name)
        {
            const uint32_t w = t.desc().width, h = t.desc().height;
            std::vector<uint8_t> px(size_t(w) * h * 4u);
            if (m_dev.readTexture(t, 0, 0, w, h, px.data()))
                writePpm("hw_" + name + ".ppm", px.data(), w, h);
        }

        void endPass()
        {
            if (m_inPass)
                m_dev.endPass();
            m_inPass = false;
            m_passColor = m_passDepth = nullptr;
        }

        // color null + depth: depth only
        void bindPass(rhi::Texture *color, rhi::Texture *depth)
        {
            if (m_inPass && color == m_passColor && depth == m_passDepth)
                return;
            endPass();
            rhi::PassDesc p;
            p.color = color;
            p.depth = depth;
            p.depthOnly = color == nullptr;
            m_dev.beginPass(p);
            m_inPass = true;
            m_passColor = color;
            m_passDepth = depth;
        }

        static void ndcRect(float x, float y, float w, float h, float targetW, float targetH, float out[4])
        {
            out[0] = x / targetW * 2.0f - 1.0f;
            out[1] = 1.0f - y / targetH * 2.0f;
            out[2] = (x + w) / targetW * 2.0f - 1.0f;
            out[3] = 1.0f - (y + h) / targetH * 2.0f;
        }

        // src (uv rect, 0..1) -> dst pixel rect, point or bilinear; opaque: alpha written as 1
        void blit(rhi::Texture &src, std::array<float, 4> uv, rhi::Texture &dst, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                  bool linear, bool opaque)
        {
            bindPass(&dst, nullptr);
            QuadConstants q{};
            ndcRect(static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h),
                    static_cast<float>(dst.desc().width), static_cast<float>(dst.desc().height), q.rect);
            std::memcpy(q.uvRect, uv.data(), sizeof(q.uvRect));
            q.params[1] = opaque ? 1.0f : 0.0f;
            m_dev.setScissor(0, 0, static_cast<int>(dst.desc().width), static_cast<int>(dst.desc().height));
            m_dev.setPipeline(*m_blit);
            m_dev.updateBuffer(*m_quadCb, &q, sizeof(q));
            m_dev.setConstants(0, *m_quadCb);
            m_dev.setTexture(0, &src, linear ? m_linear.get() : m_point.get());
            m_dev.draw(4);
            m_dev.setTexture(0, nullptr, nullptr);
        }

        // solid colour into `color`, or a depth value into `depth` (depth-only), inside a pixel rect
        void fill(rhi::Texture *color, rhi::Texture *depth, uint32_t x, uint32_t y, uint32_t w, uint32_t h, std::array<float, 4> rgba,
                  float depthValue)
        {
            rhi::Texture *target = color ? color : depth;
            bindPass(color, color ? nullptr : depth);
            const int tw = static_cast<int>(target->desc().width), th = static_cast<int>(target->desc().height);
            const int x0 = std::min<int>(static_cast<int>(x), tw), y0 = std::min<int>(static_cast<int>(y), th);
            m_dev.setScissor(x0, y0, std::min<int>(static_cast<int>(w), tw - x0), std::min<int>(static_cast<int>(h), th - y0));
            QuadConstants q{};
            q.rect[0] = -1.0f; q.rect[1] = 1.0f; q.rect[2] = 1.0f; q.rect[3] = -1.0f;
            std::memcpy(q.color, rgba.data(), sizeof(q.color));
            q.params[0] = depthValue;
            m_dev.setPipeline(color ? *m_fillColor : *m_fillDepth);
            m_dev.updateBuffer(*m_quadCb, &q, sizeof(q));
            m_dev.setConstants(0, *m_quadCb);
            m_dev.draw(4);
        }

        std::unique_ptr<rhi::Texture> makeTarget(uint32_t w, uint32_t h, bool depth, Scale s, uint32_t samples = 1)
        {
            rhi::TextureDesc d;
            d.samples = samples;
            d.width = std::max(1u, px(static_cast<float>(w), s.x));
            d.height = std::max(1u, px(static_cast<float>(h), s.y));
            d.format = depth ? rhi::Format::D32F : rhi::Format::RGBA8;
            d.usage = depth ? rhi::UsageDepth : (rhi::UsageRenderTarget | rhi::UsageSampled);
            d.name = depth ? "gs depth" : "gs target";
            // [gta-recomp] a colour target of this size given back by a drop or a growth (spareTarget), cleared below like
            // a new one: VCS drops its effect buffers every frame (a CLUT uploaded into them), and each re-creation was a
            // new texture of up to 1536x1536 at 3x
            std::unique_ptr<rhi::Texture> t;
            if (!depth && reuseTargets())
                for (auto it = m_spareTargets.begin(); it != m_spareTargets.end(); ++it)
                {
                    const rhi::TextureDesc &sd = (*it)->desc();
                    if (sd.width == d.width && sd.height == d.height && sd.samples == d.samples)
                    {
                        t = std::move(*it);
                        m_spareTargets.erase(it);
                        break;
                    }
                }
            if (!t)
                t = m_dev.createTexture(d);
            endPass();
            rhi::PassDesc p;
            p.name = "gs target clear";
            if (depth)
            {
                p.depth = t.get();
                p.depthOnly = true;
                p.clearDepth = true;
                p.clearDepthValue = 0.0f;
            }
            else
            {
                p.color = t.get();
                p.clearColor = true;
                p.clearRgba[3] = 0.0f;
            }
            m_dev.beginPass(p);
            m_dev.endPass();
            return t;
        }

        // grows the target to at least w x h (PS2 pixels) at scale s, keeping its contents (rescaled when s changed:
        // the native frame follows the window)
        void growRt(Rt &rt, uint32_t w, uint32_t h, Scale s, uint32_t samples)
        {
            if (rt.color && rt.w >= w && rt.h >= h && rt.scale == s && rt.samples == samples)
                return;
            Rt grown;
            grown.w = std::max(rt.w, w);
            grown.h = std::max(rt.h, h);
            grown.scale = s;
            grown.samples = samples;
            grown.color = makeTarget(grown.w, grown.h, false, s, samples);
            grown.dirty = true;
            if (rt.color && rt.scale == s && rt.samples == 1u && samples == 1u)
                m_dev.copyTexture(*grown.color, 0, 0, *rt.color, 0, 0, rt.color->desc().width, rt.color->desc().height);
            else if (rt.color)
            {
                const uint32_t ow = rt.color->desc().width, oh = rt.color->desc().height;
                rhi::Texture &old = readable(rt);
                const bool same = rt.scale == s;
                blit(old, {0.0f, 0.0f, 1.0f, 1.0f}, *grown.color, 0, 0, same ? ow : px(static_cast<float>(rt.w), s.x),
                     same ? oh : px(static_cast<float>(rt.h), s.y), !same, false);
            }
            spareTarget(std::move(rt.color)); // [gta-recomp] its texture for the next target of its size
            rt = std::move(grown);
        }

        Rt &rtFor(uint32_t fbp, uint32_t fbw, uint32_t minH)
        {
            Rt &rt = m_rts[fbp];
            uint32_t w = std::max<uint32_t>(fbw, 1u) * 64u, h = std::max<uint32_t>(minH, 512u);
            const Scale s = scaleFor(fbp, fbw);
            // [gta-recomp] re-made after a drop: at the size it had (the frame grows it there anyway, a copy at each step)
            if (!rt.color && reuseTargets())
                if (auto it = m_droppedSize.find(fbp); it != m_droppedSize.end() && it->second.scale == s)
                {
                    w = std::max(w, it->second.w);
                    h = std::max(h, it->second.h);
                }
            growRt(rt, w, h, s, samplesFor(fbw));
            return rt;
        }

        // [gta-recomp] a colour target no longer used (dropped, outgrown) kept for makeTarget; a few, the oldest let go
        void spareTarget(std::unique_ptr<rhi::Texture> t)
        {
            if (!t || !reuseTargets() || t->desc().format != rhi::Format::RGBA8)
                return;
            m_spareTargets.push_back(std::move(t));
            if (m_spareTargets.size() > 8u)
                m_spareTargets.erase(m_spareTargets.begin());
        }

        // [gta-recomp] a dropped target: its texture kept for reuse, its size remembered for its re-creation
        void dropRt(uint32_t fbp)
        {
            auto it = m_rts.find(fbp);
            if (it == m_rts.end())
                return;
            if (it->second.color)
                m_droppedSize[fbp] = {it->second.w, it->second.h, it->second.scale};
            spareTarget(std::move(it->second.color));
            m_rts.erase(it);
        }

        // grows (recreated, cleared to 0) to at least w x h at scale s; one depth buffer per scale (the picture's
        // targets and the effect buffers may share a Z address at different scales)
        Depth &depthFor(uint32_t zbp, uint32_t w, uint32_t h, Scale s, uint32_t samples)
        {
            const bool frame = m_frameScale.x > 0.0f && s == m_frameScale;
            Depth &d = m_depth[zbp | (frame ? 0x80000000u : 0u) | (samples > 1u ? 0x40000000u : 0u)];
            if (d.tex && d.w >= w && d.h >= h && d.scale == s)
                return d;
            d.w = std::max(d.w, w);
            d.h = std::max(d.h, h);
            d.scale = s;
            d.tex = makeTarget(d.w, d.h, true, s, samples);
            return d;
        }

        void rtUpload(const RtUpload &u)
        {
            Rt &rt = rtFor(u.fbp, u.fbw, u.y + u.h);
            rt.dirty = true;
            // uploaded at 1x, then drawn into the target at its scale
            auto tmp = m_dev.createTexture({u.w, u.h, rhi::Format::RGBA8, rhi::UsageSampled, "gs rt upload"}, u.pixels.data(), u.w * 4u);
            const Scale s = rt.scale;
            const uint32_t x0 = px(static_cast<float>(u.x), s.x), y0 = px(static_cast<float>(u.y), s.y);
            if (tmp)
                blit(*tmp, {0.0f, 0.0f, 1.0f, 1.0f}, *rt.color, x0, y0, px(static_cast<float>(u.x + u.w), s.x) - x0,
                     px(static_cast<float>(u.y + u.h), s.y) - y0, false, false);
        }

        void clear(const ClearCmd &cl)
        {
            Rt &rt = rtFor(cl.s.fbp, cl.s.fbw, cl.s.scy1 + 1u);
            rt.dirty = true;
            const std::array<float, 4> rgba = {(cl.rgba & 0xFF) / 255.0f, ((cl.rgba >> 8) & 0xFF) / 255.0f, ((cl.rgba >> 16) & 0xFF) / 255.0f,
                                               ((cl.rgba >> 24) & 0xFF) / 255.0f};
            const Scale s = rt.scale;
            const uint32_t x0 = px(cl.s.scx0, s.x), y0 = px(cl.s.scy0, s.y);
            fill(rt.color.get(), nullptr, x0, y0, px(cl.s.scx1 + 1.0f, s.x) - x0, px(cl.s.scy1 + 1.0f, s.y) - y0, rgba, 0.0f);
        }

        void depthFill(const DepthFillCmd &f, bool log)
        {
            for (auto &[key, d] : m_depth) // created (cleared) on first use
            {
                if ((key & 0x3FFFFFFFu) != f.zbp || !d.tex)
                    continue;
                const uint32_t y0 = px(static_cast<float>(f.y0), d.scale.y);
                fill(nullptr, d.tex.get(), 0, y0, px(static_cast<float>(f.width), d.scale.x), px(static_cast<float>(f.y1), d.scale.y) - y0,
                     {0, 0, 0, 0}, f.value);
            }
            if (log)
                std::fprintf(stderr, "[draw] depth fill zbp=%u rows %u..%u width %u value %.6f\n", f.zbp, f.y0, f.y1, f.width, f.value);
        }

        // [gta-recomp] The pixel shader variant for a draw's texture read (gs_draw.hlsl SAMPLE_MODE), from the constants the
        // full shader branches on - the same path, so the same pixels; only the path's code, so fewer registers.
        // PS2X_HW_SPECIALISE=0: the full shader for every draw but the Gather ones (as before 2026-10-30)
        static uint8_t drawSampleMode(const DrawConstants &c)
        {
            static const bool specialise = [] {
                const char *e = std::getenv("PS2X_HW_SPECIALISE");
                return !(e && e[0] == '0');
            }();
            const int linear = c.linearIipFgeAte[0];
            if (linear == 3)
                return 5;                   // Gather
            if (!specialise || linear == 4) // PS2X_HW_GATHER=verify needs the full shader
                return 0;
            if (c.tmeTfxTccFst[0] != 1)
                return 1;                   // no texture
            if (c.atstArefAfailRt[3] == 2)
                return 6;                   // a render target 1:1 at full resolution
            if (linear == 2)
                return 4;                   // the hardware sampler
            return linear == 0 ? 2 : 3;     // nearest / the GS's bilinear by four loads
        }

        // [gta-recomp] Does the GS pixel shader write the depth (SV_Depth)? A shader depth turns early depth rejection off:
        // every hidden pixel of the city's overdraw shaded and textured (on the laptop's HD 520 ~1.8 of ~10.7 ms of the
        // frame, PERFORMANCE.md 26). D3D / Vulkan take the 0..1 depth as given, so the rasterised value is the shader's
        // (the explicit write was GL's: its NDC lost the precision). PS2X_HW_SHADER_DEPTH=1 / 0 forces it; unset: off.
        // [gta-recomp] D3D11 kept the shader depth until checked against the Windows reference; with it the game's fire
        // was gone (the flame-thrower's, molotovs', explosions', burning cars': additive GS sprites, ZTST GEQUAL) whenever
        // the pixel shader took a bilinear path (four loads, Gather, the anisotropic sampler) - the same SV_Depth as the
        // nearest and full shaders, which drew it. The rasterised depth draws it on every path (game-internals.md 6m).
        bool shaderDepth() const
        {
            static const int forced = [] {
                const char *e = std::getenv("PS2X_HW_SHADER_DEPTH");
                return e && *e ? (*e == '0' ? 0 : 1) : -1;
            }();
            return forced == 1;
        }

        rhi::Pipeline &drawPipeline(const GsBlend &b, uint8_t mask, bool depthTest, uint32_t ztst, bool zwrite, uint8_t mode,
                                    uint8_t sample = 0)
        {
            const bool dual = b.enable && (b.src == rhi::BlendFactor::Src1Alpha || b.src == rhi::BlendFactor::InvSrc1Alpha ||
                                           b.dst == rhi::BlendFactor::Src1Alpha || b.dst == rhi::BlendFactor::InvSrc1Alpha);
            const uint32_t key = (b.enable ? 1u : 0u) | (static_cast<uint32_t>(b.src) << 1) | (static_cast<uint32_t>(b.dst) << 5) |
                                 (static_cast<uint32_t>(b.op) << 9) | (uint32_t(mask & 0xF) << 12) | ((depthTest ? 1u : 0u) << 16) |
                                 ((ztst & 3u) << 17) | ((zwrite ? 1u : 0u) << 19) | (uint32_t(mode & 7u) << 20) | ((dual ? 1u : 0u) << 23) |
                                 (uint32_t(sample & 7u) << 24);
            std::unique_ptr<rhi::Pipeline> &p = m_drawPipelines[key];
            if (!p)
            {
                const render::GsShaders &sh = render::gsShaders(m_dev.backend()); // [gta-recomp] the device's backend (DXBC / SPIR-V)
                rhi::PipelineDesc d;
                // [gta-recomp] the pixel shader built for the draw's texture read (drawSampleMode)
                // [gta-recomp] ... and without the depth output where the rasterised depth does (shaderDepth)
                d.pixel = (shaderDepth() ? sh.drawPS : sh.drawPSNoDepth)[sample < render::GsShaders::kSampleModes ? sample : 0][dual ? 1 : 0];
                if (mode == 4)
                {
                    // [gta-recomp] native VCS lit model (GSNativeDraw::VcsLit): skinned on the CPU, lit / transformed here
                    d.vertex = sh.litVS;
                    d.attributes = {
                        {"POSITION", 0, rhi::VertexFormat::Float3, 0},
                        {"TEXCOORD", 0, rhi::VertexFormat::Float3, 12},
                        {"NORMAL", 0, rhi::VertexFormat::Float3, 24},
                    };
                    d.stride = 48;
                    d.topology = rhi::Topology::TriangleStrip;
                    d.depthClip = false;
                }
                else if (mode == 3)
                {
                    // [gta-recomp] native VCS world draw (GSNativeDraw::VcsWorld): 32-byte vertices, transformed on the GPU
                    d.vertex = sh.worldVS;
                    d.attributes = {
                        {"POSITION", 0, rhi::VertexFormat::Float3, 0},
                        {"TEXCOORD", 0, rhi::VertexFormat::Float3, 12},
                        {"COLOR", 0, rhi::VertexFormat::UInt1, 24},
                    };
                    d.stride = 32;
                    d.topology = rhi::Topology::TriangleStrip;
                    d.depthClip = false; // the PS2 clip planes come as clip distances
                }
                else
                {
                    d.vertex = sh.drawVS;
                    d.attributes = {
                        {"POSITION", 0, rhi::VertexFormat::Float3, static_cast<uint32_t>(offsetof(HwVertex, x))},
                        {"TEXCOORD", 0, rhi::VertexFormat::Float3, static_cast<uint32_t>(offsetof(HwVertex, s))},
                        {"COLOR", 0, rhi::VertexFormat::UByte4Norm, static_cast<uint32_t>(offsetof(HwVertex, r))},
                        {"TEXCOORD", 1, rhi::VertexFormat::Float1, static_cast<uint32_t>(offsetof(HwVertex, fog))},
                    };
                    d.stride = sizeof(HwVertex);
                    d.topology = mode == 1 ? rhi::Topology::Lines : mode == 2 ? rhi::Topology::Points : rhi::Topology::Triangles;
                }
                d.blend.enable = b.enable;
                d.blend.srcColor = b.src;
                d.blend.dstColor = b.dst;
                d.blend.colorOp = b.op;
                d.blend.srcAlpha = rhi::BlendFactor::One;
                d.blend.dstAlpha = rhi::BlendFactor::Zero;
                d.blend.writeMask = mask;
                d.depth.test = depthTest;
                d.depth.write = depthTest && zwrite;
                d.depth.func = ztst == 1 ? rhi::Compare::Always : ztst == 2 ? rhi::Compare::GreaterEqual : rhi::Compare::Greater;
                d.name = "gs draw";
                const auto made = std::chrono::steady_clock::now();
                p = m_dev.createPipeline(d);
                // [gta-recomp] PS2X_HW_LOG_PIPELINES=1: each draw pipeline as it is made, with the time it took (a first
                // use mid-game is a main-thread stall)
                static const bool logPipelines = std::getenv("PS2X_HW_LOG_PIPELINES") != nullptr;
                if (logPipelines)
                    std::fprintf(stderr, "[gs-hw] pipeline %08x (sample %u, mode %u) made in %.2f ms, %zu so far\n", key, sample, mode,
                                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - made).count(),
                                 m_drawPipelines.size());
            }
            return *p;
        }

        // [gta-recomp] Native HUD layer (gsHwSetHudLayer): per frame buffer, the HUD primitives of the latest frame drawn
        // into it, drawn over the displayed picture at its output size (composeDisplay). Any other drawing into the
        // buffer is the next frame's scene: the layer is stale, and the next HUD draw starts it afresh.
        struct HudLayer
        {
            std::vector<HwVertex> vertices;
            std::vector<DrawCmd> draws;
            bool stale = false;  // the buffer has new drawing: this HUD is gone
            bool closed = false; // copied out as a finished frame: still shown from the copy, the next HUD replaces it
        };
        struct LayerTarget // where draw() puts a HUD layer draw, or a native post-effect pass (see nativePost)
        {
            rhi::Texture *color = nullptr, *depth = nullptr; // null color: the draw's own render target
            float scaleX = 1.0f, scaleY = 1.0f; // target pixels per PS2 pixel
            int originX = 0, originY = 0;       // the PS2 pixel at the target's top-left (vertices come shifted by it)
            float scissorScaleX = 1.0f;         // the draw's scissor x in target units (a widened native buffer)
            float squeezePivot = 0.0f, squeeze = 1.0f; // a HUD layer draw's widescreen squeeze (DrawCmd::hudSqueeze)
            bool opaque = true;                 // alpha not written (the shown picture)
            rhi::Texture *texture = nullptr;    // read instead of the draw's texture, as a plain texture
            rhi::Buffer *vertices = nullptr;    // null: the HUD layer's
        };

        // A HUD draw into its layer. Textured quads (two triangles over one rectangle of a clamped texture) sample only
        // the texels inside their own rectangle (REGION_CLAMP): the HUD's fonts and icons are atlases with cells side by
        // side, and drawn larger than the PS2's 640x448 the pixels along a quad's edge sample within half a texel of
        // the cell border, where bilinear filtering pulls in the next cell's outline (lines under the clock's digits,
        // beside its colon). At 640x448 the PS2's samples stay further inside.
        void appendHudDraw(HudLayer &layer, const Frame &f, const DrawCmd &d)
        {
            const HwVertex *v = f.vertices.data() + d.first;
            const bool clampable = d.s.tme && !d.s.texIsRt && d.s.mode == 0 && d.s.wms == 1u && d.s.wmt == 1u && d.s.tw && d.s.th;
            auto add = [&](const HwState &s, const HwVertex *from, uint32_t count) {
                const size_t start = layer.vertices.size();
                layer.vertices.insert(layer.vertices.end(), from, from + count);
                const uint32_t kept = snapToPixels(layer.vertices, start, s.mode);
                layer.vertices.resize(start + kept);
                if (!kept)
                    return;
                const float squeeze = f.shown != 0 ? d.hudSqueeze : 1.0f; // [gta-recomp] a frame shown at 4:3: none
                if (squeeze != 1.0f)
                {
                    // widescreen: squeezed after the snap, so the pieces keep the 4:3 picture's pixel edges (the
                    // radar's mask and scissor meet), in the GS pixel-centre coordinates the scissor uses too (draw)
                    const float o = pixelCentreOffset();
                    for (size_t k = start; k < layer.vertices.size(); ++k)
                        layer.vertices[k].x = d.hudPivot + (layer.vertices[k].x + o - d.hudPivot) * squeeze - o;
                }
                const DrawCmd &last = layer.draws.empty() ? d : layer.draws.back();
                if (!layer.draws.empty() && std::memcmp(&last.s, &s, sizeof(HwState)) == 0 && last.hudPivot == d.hudPivot &&
                    last.hudSqueeze == squeeze && last.first + last.count == start)
                    layer.draws.back().count += kept;
                else
                    layer.draws.push_back({s, static_cast<uint32_t>(start), kept, d.source, d.hudPivot, squeeze});
            };
            if (!clampable)
            {
                add(d.s, v, d.count);
                return;
            }
            const float su = d.s.fst ? 1.0f : static_cast<float>(d.s.tw), sv = d.s.fst ? 1.0f : static_cast<float>(d.s.th);
            uint32_t i = 0;
            for (; i + 6u <= d.count; i += 6u)
            {
                // a quad: both triangles cover the same position and texel rectangle, no perspective
                float b[2][8];
                bool ok = true;
                for (int tri = 0; tri < 2 && ok; ++tri)
                {
                    float *r = b[tri];
                    r[0] = r[2] = r[4] = r[6] = 1e30f;
                    r[1] = r[3] = r[5] = r[7] = -1e30f;
                    for (int k = 0; k < 3; ++k)
                    {
                        const HwVertex &p = v[i + tri * 3 + k];
                        if (!d.s.fst && p.q != 1.0f)
                            ok = false;
                        const float vals[4] = {p.x, p.y, p.s * su, p.t * sv};
                        for (int c = 0; c < 4; ++c)
                        {
                            r[2 * c] = std::min(r[2 * c], vals[c]);
                            r[2 * c + 1] = std::max(r[2 * c + 1], vals[c]);
                        }
                    }
                }
                if (ok)
                    for (int c = 0; c < 8; ++c)
                        ok = ok && b[0][c] == b[1][c];
                if (!ok)
                {
                    add(d.s, v + i, 6u);
                    continue;
                }
                // texels whose centres lie inside the rectangle
                auto range = [](float lo, float hi, uint16_t size, uint16_t &mn, uint16_t &mx) {
                    int a = static_cast<int>(std::ceil(lo - 0.5f)), z = static_cast<int>(std::floor(hi - 0.5f));
                    if (a > z)
                        a = z = static_cast<int>(std::floor((lo + hi) * 0.5f));
                    mn = static_cast<uint16_t>(std::clamp(a, 0, size - 1));
                    mx = static_cast<uint16_t>(std::clamp(z, 0, size - 1));
                };
                HwState s = d.s;
                s.wms = s.wmt = 2u; // REGION_CLAMP
                range(b[0][4], b[0][5], s.tw, s.minu, s.maxu);
                range(b[0][6], b[0][7], s.th, s.minv, s.maxv);
                add(s, v + i, 6u);
            }
            if (i < d.count)
                add(d.s, v + i, d.count - i);
        }

        // The PS2 at 640x448 covers whole pixels: pixel X is drawn when its centre X + 0.5 is inside. Drawn larger, the
        // fraction of a pixel between an edge and the next pixel boundary gets pixels of its own, and the HUD's pieces
        // stop lining up: the radar map is scissored from pixel 27, the mask that hides its corners starts at x 27.4,
        // and at 1080p the map showed in a line of pixels between them. So the layer's vertices move to the pixel
        // boundaries the PS2's coverage gives (x' = ceil(x - 0.5)), each triangle's other values (STQ, colour, Z, fog)
        // re-evaluated there from the original triangle, so a covered pixel samples what it sampled before. Triangles
        // that cover nothing then are dropped. Returns the vertices kept from `start` on.
        static uint32_t snapToPixels(std::vector<HwVertex> &vs, size_t start, uint8_t mode)
        {
            // the first pixel an edge at x covers is ceil(x) on the GS (its pixels sample at integer positions); the
            // boundary of that pixel's area, in drawing coordinates, is half a pixel before it (pixelCentreOffset)
            const float o = pixelCentreOffset();
            auto snap = [o](float x) { return std::ceil(x - 0.5f + o) - o; };
            const uint32_t count = static_cast<uint32_t>(vs.size() - start);
            if (mode != 0 || count % 3u != 0u)
            {
                for (size_t i = start; i < vs.size(); ++i)
                {
                    vs[i].x = snap(vs[i].x);
                    vs[i].y = snap(vs[i].y);
                }
                return count;
            }
            size_t out = start;
            for (size_t i = start; i + 3 <= vs.size(); i += 3)
            {
                const HwVertex t[3] = {vs[i], vs[i + 1], vs[i + 2]};
                const double area = (double(t[1].x) - t[0].x) * (double(t[2].y) - t[0].y) - (double(t[2].x) - t[0].x) * (double(t[1].y) - t[0].y);
                float sx[3], sy[3];
                for (int k = 0; k < 3; ++k)
                {
                    sx[k] = snap(t[k].x);
                    sy[k] = snap(t[k].y);
                }
                const double snappedArea = (double(sx[1]) - sx[0]) * (double(sy[2]) - sy[0]) - (double(sx[2]) - sx[0]) * (double(sy[1]) - sy[0]);
                if (std::fabs(area) < 1e-6 || snappedArea == 0.0)
                    continue; // covers no pixel
                for (int k = 0; k < 3; ++k)
                {
                    // barycentric coordinates of the snapped position in the original triangle
                    const double px = sx[k] - t[0].x, py = sy[k] - t[0].y;
                    const double l1 = (px * (double(t[2].y) - t[0].y) - py * (double(t[2].x) - t[0].x)) / area;
                    const double l2 = (py * (double(t[1].x) - t[0].x) - px * (double(t[1].y) - t[0].y)) / area;
                    const double l0 = 1.0 - l1 - l2;
                    auto lerp3 = [&](float a, float b, float c) { return static_cast<float>(l0 * a + l1 * b + l2 * c); };
                    auto lerpByte = [&](uint8_t a, uint8_t b, uint8_t c) {
                        return static_cast<uint8_t>(std::clamp(std::lround(l0 * a + l1 * b + l2 * c), 0l, 255l));
                    };
                    HwVertex v = t[k];
                    v.x = sx[k];
                    v.y = sy[k];
                    v.z = std::clamp(lerp3(t[0].z, t[1].z, t[2].z), 0.0f, 1.0f);
                    v.s = lerp3(t[0].s, t[1].s, t[2].s);
                    v.t = lerp3(t[0].t, t[1].t, t[2].t);
                    v.q = lerp3(t[0].q, t[1].q, t[2].q);
                    v.r = lerpByte(t[0].r, t[1].r, t[2].r);
                    v.g = lerpByte(t[0].g, t[1].g, t[2].g);
                    v.b = lerpByte(t[0].b, t[1].b, t[2].b);
                    v.a = lerpByte(t[0].a, t[1].a, t[2].a);
                    v.fog = std::clamp(lerp3(t[0].fog, t[1].fog, t[2].fog), 0.0f, 1.0f);
                    vs[out + k] = v;
                }
                out += 3;
            }
            return static_cast<uint32_t>(out - start);
        }

        static float pixelCentreOffset() { return gsPixelCentreOffset(); }

        // ---- [gta-recomp] Native post effects (docs/NATIVE_RENDERER.md "Post effects") ----------------------------
        // VCS's bloom and trails are chains of draws through effect buffers inside VRAM (bloom: 256x256 at FBP 420 /
        // 452, trails: 512x512 at FBP 384; game-internals.md 6g). Recognised here, they run against buffers of our own
        // instead, with the same draws, shaders and blend states - so at 4:3 the result is the emulation's - and the
        // buffers widened by the display's widening w: positions in a buffer scale by w, texture coordinates into it
        // become u + (w - 1) * s * x (s: buffer texels per target unit at 4:3), so the bloom's +-1 texel taps and the
        // trails' warp offsets (in texels) cover the same distance on screen as at 4:3 instead of w times more.
        // PS2X_HW_POSTFX=0: the draws run as recorded; else the letters of those to run natively: b bloom, t trails,
        // c frame copies / motion blur (default all).
        static unsigned postNative() // bit 0 bloom, bit 1 trails, bit 2 frame copies
        {
            static const unsigned on = [] {
                const char *e = std::getenv("PS2X_HW_POSTFX");
                if (!e || !e[0])
                    return 7u;
                if (e[0] == '0')
                    return 0u;
                unsigned m = 0;
                for (const char *p = e; *p; ++p)
                    m |= *p == 'b' ? 1u : *p == 't' ? 2u : *p == 'c' ? 4u : 0u;
                return m;
            }();
            return on;
        }

        static bool blendIs(const HwState &s, unsigned a, unsigned b, unsigned c, unsigned d)
        {
            return s.abe && s.ba == a && s.bb == b && s.bc == c && s.bd == d;
        }

        const DrawCmd *drawAt(const Frame &f, size_t k) const
        {
            return k < f.cmds.size() && f.cmds[k].type == CmdType::Draw ? &f.draws[f.cmds[k].index] : nullptr;
        }

        struct PostItem
        {
            const DrawCmd *d;
            HwState s;              // the draw's state, texture size adjusted for a native buffer it reads
            uint32_t first;         // into m_postVerts
            rhi::Texture *target;   // a native buffer, or null: the draw's own render target
            rhi::Texture *texture;  // a native buffer read instead of the draw's texture, or null
        };

        // Adds a draw to the pass list: its vertices copied into m_postVerts, positions widened when it draws into a
        // native buffer, texture coordinates remapped when it reads one (texWBase: that buffer's width at 4:3).
        void postAdd(std::vector<PostItem> &items, const Frame &f, const DrawCmd *d, rhi::Texture *target, rhi::Texture *texture,
                     float w, float texWBase)
        {
            PostItem it{d, d->s, static_cast<uint32_t>(m_postVerts.size()), target, texture};
            const float targetUnits = static_cast<float>(d->s.fbw) * 64.0f; // target width at 4:3
            const float slope = texture ? texWBase / targetUnits : 0.0f;
            const float su = d->s.fst ? 1.0f : static_cast<float>(d->s.tw);
            if (texture)
            {
                it.s.tw = static_cast<uint16_t>(texture->desc().width);
                it.s.th = static_cast<uint16_t>(texture->desc().height);
            }
            for (uint32_t i = 0; i < d->count; ++i)
            {
                HwVertex v = f.vertices[d->first + i];
                if (texture)
                {
                    const float u = v.s / v.q * su + (w - 1.0f) * slope * v.x; // texels
                    v.s = d->s.fst ? u : u / static_cast<float>(it.s.tw) * v.q;
                    if (!d->s.fst)
                        v.t = v.t / v.q * static_cast<float>(d->s.th) / static_cast<float>(it.s.th) * v.q;
                }
                if (target)
                    v.x *= w;
                m_postVerts.push_back(v);
            }
            items.push_back(it);
        }

        rhi::Texture *postBuffer(std::unique_ptr<rhi::Texture> &t, uint32_t w, uint32_t h, const char *name)
        {
            if (!t || t->desc().width != w || t->desc().height != h)
            {
                endPass();
                t = m_dev.createTexture({w, h, rhi::Format::RGBA8, rhi::UsageRenderTarget | rhi::UsageSampled, name});
            }
            return t.get();
        }

        void postRun(const std::vector<PostItem> &items, float w, bool log, const char *what)
        {
            const size_t bytes = m_postVerts.size() * sizeof(HwVertex);
            if (!m_postVb || m_postVbCapacity < bytes)
            {
                m_postVbCapacity = std::max<size_t>(bytes + bytes / 2, 64u << 10);
                m_postVb = m_dev.createBuffer({m_postVbCapacity, rhi::BufferUsage::Vertex, true, "gs post vertices"});
            }
            if (!m_postVb)
                return;
            m_dev.updateBuffer(*m_postVb, m_postVerts.data(), bytes);
            m_dev.pushMarker(what);
            for (const PostItem &it : items)
            {
                hudSceneDraw(it.d->s.fbp);
                LayerTarget t;
                t.color = it.target;
                t.scissorScaleX = it.target ? w : 1.0f;
                t.opaque = false;
                t.texture = it.texture;
                t.vertices = m_postVb.get();
                draw(it.s, it.first, it.d->count, nullptr, &t);
            }
            m_dev.popMarker();
            if (log)
                std::fprintf(stderr, "[draw] native %s: %zu draws, widening %.3f\n", what, items.size(), w);
        }

        // what the recorded chain leaves in the effect buffers (VRAM) for later readers: the native buffer, squeezed
        void postWriteBack(rhi::Texture *native, uint32_t fbp, uint32_t fbw, uint32_t w, uint32_t h)
        {
            Rt &rt = rtFor(fbp, fbw, h);
            rt.dirty = true;
            blit(*native, {0.0f, 0.0f, 1.0f, 1.0f}, *rt.color, 0, 0, px(static_cast<float>(w), rt.scale.x), px(static_cast<float>(h), rt.scale.y),
                 false, false);
        }

        size_t nativePost(const Frame &f, size_t ci, bool log)
        {
            const unsigned on = postNative();
            if (on & 1u)
                if (const size_t n = nativeBloom(f, ci, log))
                    return n;
            if (on & 2u)
                if (const size_t n = nativeTrails(f, ci, log))
                    return n;
            return (on & 4u) ? nativeFrameCopy(f, ci, log) : 0u;
        }

        // Frame copy / motion blur: a full-screen 1:1 read of another 640-wide target, u = x + 0.5 (on the GS an exact
        // copy), which the game draws as 40 strips. A plain copy (the shown buffer) becomes a texture copy; a blended
        // one (motion blur: the previous picture at FIX 16, colour 127/128) one full-screen quad with the draw's state.
        size_t nativeFrameCopy(const Frame &f, size_t ci, bool log)
        {
            const DrawCmd *d = drawAt(f, ci);
            if (!d || d->count < 3u)
                return 0;
            const HwState &s = d->s;
            if (!s.tme || !s.texIsRt || !s.rtFull || !s.fst || s.fbw != 10u || s.rtFbp == s.fbp || s.rtOx || s.rtOy || s.fbmsk ||
                s.mode != 0u || s.scx0 != 0u || s.scy0 != 0u || s.scx1 < 639u || s.scy1 < 447u)
                return 0;
            auto src = m_rts.find(s.rtFbp);
            if (src == m_rts.end() || !src->second.color)
                return 0;
            // the whole screen, texel = position + 0.5
            const HwVertex *v = f.vertices.data() + d->first;
            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
            bool mapped = true;
            for (uint32_t i = 0; i < d->count && mapped; ++i)
            {
                x0 = std::min(x0, v[i].x), x1 = std::max(x1, v[i].x), y0 = std::min(y0, v[i].y), y1 = std::max(y1, v[i].y);
                mapped = std::fabs(v[i].s - v[i].x - 0.5f) < 0.01f && std::fabs(v[i].t - v[i].y - 0.5f) < 0.01f &&
                         std::memcmp(&v[i].r, &v[0].r, 4) == 0;
            }
            if (!mapped || x0 > 0.0f || y0 > 0.0f || x1 < 640.0f || y1 < 448.0f)
                return 0;
            const bool decal = s.tfx == 1u && s.tcc;
            const bool plain = !s.abe && !s.ate && !s.fge && decal && (!s.zte || (s.ztst == 1u && s.zmsk));
            Rt &dst = rtFor(s.fbp, s.fbw, s.scy1 + 1u);
            hudOnDraw(s);
            if (plain && dst.scale == src->second.scale && dst.samples == 1u) // a multisampled copy is drawn (below)
            {
                rhi::Texture &from = readable(src->second);
                endPass();
                const uint32_t w = std::min(px(640.0f, dst.scale.x), std::min(dst.color->desc().width, from.desc().width));
                const uint32_t h = std::min(px(448.0f, dst.scale.y), std::min(dst.color->desc().height, from.desc().height));
                m_dev.copyTexture(*dst.color, 0, 0, from, 0, 0, w, h);
                dst.dirty = true;
                if (log)
                    std::fprintf(stderr, "[draw] native frame copy %u -> %u\n", s.rtFbp, s.fbp);
                return 1;
            }
            // one quad over the screen with the draw's own state and colour
            m_postVerts.clear();
            const float xs[6] = {0, 640, 0, 640, 640, 0}, ys[6] = {0, 0, 448, 0, 448, 448};
            for (int i = 0; i < 6; ++i)
            {
                HwVertex q = v[0];
                q.x = xs[i], q.y = ys[i], q.s = xs[i] + 0.5f, q.t = ys[i] + 0.5f, q.q = 1.0f;
                m_postVerts.push_back(q);
            }
            const size_t bytes = m_postVerts.size() * sizeof(HwVertex);
            if (!m_postVb || m_postVbCapacity < bytes)
            {
                m_postVbCapacity = std::max<size_t>(bytes + bytes / 2, 64u << 10);
                m_postVb = m_dev.createBuffer({m_postVbCapacity, rhi::BufferUsage::Vertex, true, "gs post vertices"});
            }
            if (!m_postVb)
                return 0;
            m_dev.updateBuffer(*m_postVb, m_postVerts.data(), bytes);
            LayerTarget t;
            t.opaque = false;
            t.vertices = m_postVb.get();
            draw(s, 0u, 6u, nullptr, &t);
            if (log)
                std::fprintf(stderr, "[draw] native frame %s %u -> %u\n", s.abe ? "blend" : "copy (quad)", s.rtFbp, s.fbp);
            return 1;
        }

        // Bloom: downsample the frame into a 256x256 buffer, subtract a threshold, n passes of a 4-tap cross blur
        // (ping-pong: clear, add the taps), added back into the frame.
        size_t nativeBloom(const Frame &f, size_t ci, bool log)
        {
            const DrawCmd *down = drawAt(f, ci);
            if (!down || !down->s.tme || !down->s.texIsRt || down->s.abe || down->s.fbw != 4u || down->s.rtFbp == down->s.fbp)
                return 0;
            const uint32_t frame = down->s.rtFbp, first = down->s.fbp;
            const DrawCmd *thr = drawAt(f, ci + 1);
            if (!thr || thr->s.fbp != first || thr->s.tme || !blendIs(thr->s, 2u, 0u, 2u, 1u))
                return 0;
            std::vector<std::pair<const DrawCmd *, const DrawCmd *>> passes; // (clear, taps)
            size_t k = ci + 2;
            uint32_t last = first;
            for (;;)
            {
                const DrawCmd *clr = drawAt(f, k), *taps = drawAt(f, k + 1);
                if (!clr || !taps || clr->s.tme || clr->s.fbw != 4u || !blendIs(clr->s, 0u, 2u, 2u, 2u))
                    break;
                if (taps->s.fbp != clr->s.fbp || !taps->s.tme || !taps->s.texIsRt || taps->s.rtFbp != last || !blendIs(taps->s, 0u, 2u, 2u, 1u))
                    break;
                passes.push_back({clr, taps});
                last = clr->s.fbp;
                k += 2;
            }
            const DrawCmd *add = drawAt(f, k);
            if (passes.empty() || !add || add->s.fbp != frame || !add->s.tme || !add->s.texIsRt || add->s.rtFbp != last ||
                !blendIs(add->s, 0u, 2u, 2u, 1u))
                return 0;
            const float w = std::max(1.0f, g_aspectWidening.load(std::memory_order_relaxed));
            const uint32_t bw = static_cast<uint32_t>(std::lround(256.0f * w)), bh = 256u;
            rhi::Texture *bufs[2] = {postBuffer(m_postBloom[0], bw, bh, "gs post bloom 0"), postBuffer(m_postBloom[1], bw, bh, "gs post bloom 1")};
            if (!bufs[0] || !bufs[1])
                return 0;
            const uint32_t other = passes[0].first->s.fbp == first ? passes[0].second->s.rtFbp : passes[0].first->s.fbp;
            auto buf = [&](uint32_t fbp) { return fbp == first ? bufs[0] : bufs[1]; };
            m_postVerts.clear();
            std::vector<PostItem> items;
            postAdd(items, f, down, buf(first), nullptr, w, 0.0f);
            postAdd(items, f, thr, buf(first), nullptr, w, 0.0f);
            for (const auto &[clr, taps] : passes)
            {
                postAdd(items, f, clr, buf(clr->s.fbp), nullptr, w, 0.0f);
                postAdd(items, f, taps, buf(taps->s.fbp), buf(taps->s.rtFbp), w, 256.0f);
            }
            postAdd(items, f, add, nullptr, buf(last), w, 256.0f);
            postRun(items, w, log, "bloom");
            postWriteBack(buf(first), first, 4u, 256u, 256u);
            if (other != first)
                postWriteBack(buf(other), other, 4u, 256u, 256u);
            return k + 1 - ci;
        }

        // Trails: downsample the frame into a 512x512 buffer, blend it back through warped strips (FIX ~8) and add it
        // with a tint.
        size_t nativeTrails(const Frame &f, size_t ci, bool log)
        {
            const DrawCmd *down = drawAt(f, ci);
            if (!down || !down->s.tme || !down->s.texIsRt || down->s.abe || down->s.fbw != 8u || down->s.rtFbp == down->s.fbp)
                return 0;
            const uint32_t frame = down->s.rtFbp, buffer = down->s.fbp;
            auto back = [&](const DrawCmd *d) { return d && d->s.fbp == frame && d->s.tme && d->s.texIsRt && d->s.rtFbp == buffer; };
            const DrawCmd *blend = drawAt(f, ci + 1);
            if (!back(blend) || !blendIs(blend->s, 0u, 1u, 2u, 1u))
                return 0;
            const DrawCmd *tint = drawAt(f, ci + 2);
            if (!back(tint) || !blendIs(tint->s, 0u, 2u, 2u, 1u))
                tint = nullptr;
            const float w = std::max(1.0f, g_aspectWidening.load(std::memory_order_relaxed));
            const uint32_t th = std::max<uint32_t>(down->s.scy1, 1u);
            rhi::Texture *t = postBuffer(m_postTrails, static_cast<uint32_t>(std::lround(512.0f * w)), th, "gs post trails");
            if (!t)
                return 0;
            m_postVerts.clear();
            std::vector<PostItem> items;
            postAdd(items, f, down, t, nullptr, w, 0.0f);
            postAdd(items, f, blend, nullptr, t, w, 512.0f);
            if (tint)
                postAdd(items, f, tint, nullptr, t, w, 512.0f);
            postRun(items, w, log, "trails");
            postWriteBack(t, buffer, 8u, 512u, th);
            return tint ? 3u : 2u;
        }

        // the HUD layers' bookkeeping for a draw into the frame's buffers: a new scene makes the target's layer stale; a
        // plain copy of another buffer takes its HUD along (VCS draws into one buffer and copies the finished frame into
        // the displayed one at the start of the next)
        void hudOnDraw(const HwState &ds)
        {
            hudSceneDraw(ds.fbp);
            if (ds.tme && ds.texIsRt && !ds.abe && ds.rtFbp != ds.fbp) // [gta-recomp] a copy takes its picture's shape along
                if (auto it = m_shapeOf.find(ds.rtFbp); it != m_shapeOf.end())
                    m_shapeOf[ds.fbp] = it->second;
            if (ds.tme && ds.texIsRt && !ds.abe && ds.rtFbp != ds.fbp && !m_hud.empty())
                if (auto it = m_hud.find(ds.rtFbp); it != m_hud.end() && !it->second.stale)
                {
                    it->second.closed = true; // the copied frame is finished: the next HUD draw starts afresh
                    HudLayer copy = it->second;
                    copy.closed = false;
                    m_hud[ds.fbp] = std::move(copy);
                }
        }

        void hudSceneDraw(uint32_t fbp)
        {
            m_shapeOf[fbp] = m_execWide; // [gta-recomp] drawn for the executing frame's shape
            if (!m_hud.empty())
                if (auto it = m_hud.find(fbp); it != m_hud.end())
                    it->second.stale = true;
        }

        // the layer over m_display, which shows the buffer from PS2 pixel (displayX, displayY)
        void drawHudLayer(const HudLayer &layer, uint32_t displayX, uint32_t displayY)
        {
            const size_t bytes = layer.vertices.size() * sizeof(HwVertex);
            if (!bytes || !m_display)
                return;
            const std::vector<HwVertex> *vertices = &layer.vertices;
            if (displayX || displayY)
            {
                m_hudShifted = layer.vertices;
                for (HwVertex &v : m_hudShifted)
                {
                    v.x -= static_cast<float>(displayX);
                    v.y -= static_cast<float>(displayY);
                }
                vertices = &m_hudShifted;
            }
            if (!m_hudVb || m_hudVbCapacity < bytes)
            {
                m_hudVbCapacity = std::max<size_t>(bytes + bytes / 2, 256u << 10);
                m_hudVb = m_dev.createBuffer({m_hudVbCapacity, rhi::BufferUsage::Vertex, true, "gs hud vertices"});
            }
            if (!m_hudVb)
                return;
            m_dev.updateBuffer(*m_hudVb, vertices->data(), bytes);
            const uint32_t w = m_display->desc().width, h = m_display->desc().height;
            if (!m_hudDepth || m_hudDepth->desc().width != w || m_hudDepth->desc().height != h)
            {
                endPass();
                m_hudDepth = m_dev.createTexture({w, h, rhi::Format::D32F, rhi::UsageDepth, "gs hud depth"});
            }
            if (!m_hudDepth)
                return;
            // the game clears Z to 0 just before its HUD (a full-screen Z-only draw); the HUD's depth tests start there
            fill(nullptr, m_hudDepth.get(), 0, 0, w, h, {0, 0, 0, 0}, 0.0f);
            LayerTarget t;
            t.color = m_display.get();
            t.depth = m_hudDepth.get();
            t.scaleX = static_cast<float>(w) / static_cast<float>(std::max(m_displayW, 1u));
            t.scaleY = static_cast<float>(h) / static_cast<float>(std::max(m_displayH, 1u));
            t.originX = static_cast<int>(displayX);
            t.originY = static_cast<int>(displayY);
            m_dev.pushMarker("gs hud layer");
            for (const DrawCmd &d : layer.draws)
            {
                t.squeezePivot = d.hudPivot;
                t.squeeze = d.hudSqueeze;
                draw(d.s, d.first, d.count, nullptr, &t);
            }
            m_dev.popMarker();
            endPass();
        }

        // [gta-recomp] A constant buffer written only when its contents change: consecutive draws often share them (a
        // model's meshes its world constants, a run of draws its draw state). Each write was a new slice and descriptor
        // push per draw on Vulkan and a Map(DISCARD) on D3D11, and on the GPU a constant reload (PERFORMANCE.md 26).
        // `last` holds what the buffer has now. PS2X_HW_CB_DEDUPE=0: every draw writes, as before; PS2X_HW_CB_LOG=1:
        // writes and skips every 5 s.
        void updateConstants(rhi::Buffer &buffer, const void *data, size_t size, std::vector<uint8_t> &last, int which)
        {
            static const bool dedupe = [] {
                const char *e = std::getenv("PS2X_HW_CB_DEDUPE");
                return !(e && *e == '0');
            }();
            static const bool log = std::getenv("PS2X_HW_CB_LOG") != nullptr;
            const bool same = dedupe && last.size() == size && std::memcmp(last.data(), data, size) == 0;
            if (log)
            {
                static uint64_t n[2][2] = {};
                static auto since = std::chrono::steady_clock::now();
                ++n[which][same ? 1 : 0];
                if (std::chrono::steady_clock::now() - since > std::chrono::seconds(5))
                {
                    since = std::chrono::steady_clock::now();
                    std::fprintf(stderr, "[gs-hw] constants: draw %llu written, %llu skipped; world %llu written, %llu skipped\n",
                                 (unsigned long long)n[0][0], (unsigned long long)n[0][1], (unsigned long long)n[1][0],
                                 (unsigned long long)n[1][1]);
                    std::memset(n, 0, sizeof(n));
                }
            }
            if (same)
                return;
            m_dev.updateBuffer(buffer, data, size);
            last.assign(static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + size);
        }

        void draw(const HwState &s, uint32_t first, uint32_t count, const NativeDrawCmd *native, const LayerTarget *layer = nullptr)
        {
            rhi::Buffer *vertices = layer ? (layer->vertices ? layer->vertices : m_hudVb.get()) : m_vb.get();
            uint32_t vertexOffset = 0;
            if (native && native->transient)
            {
                vertices = m_nativeVb.get();
                vertexOffset = native->transientOffset;
                if (vertices)
                    updateConstants(*m_worldCb, native->constants, sizeof(native->constants), m_worldCbLast, 1);
            }
            else if (native)
            {
                auto mesh = m_meshes.find(native->meshKey);
                vertices = mesh != m_meshes.end() ? mesh->second.get() : nullptr;
                if (vertices)
                    updateConstants(*m_worldCb, native->constants, sizeof(native->constants), m_worldCbLast, 1);
            }
            if (!count || !vertices)
                return;
            // [gta-recomp] ZTST ALWAYS without Z writes needs no depth buffer (the effect buffers carry the frame's ZBP)
            const bool depthOn = s.zte != 0u && !(s.ztst == 1u && s.zmsk);
            rhi::Texture *colorTarget = nullptr, *depthTarget = nullptr;
            float targetW = 0.0f, targetH = 0.0f; // PS2 pixels
            Scale targetScale;
            const LayerTarget *target = layer && layer->color ? layer : nullptr; // a target other than the draw's own
            if (target)
            {
                colorTarget = target->color;
                depthTarget = depthOn ? target->depth : nullptr;
                targetW = static_cast<float>(target->color->desc().width) / target->scaleX;
                targetH = static_cast<float>(target->color->desc().height) / target->scaleY;
            }
            else
            {
                Rt &rt = rtFor(s.fbp, s.fbw, s.scy1 + 1u);
                Depth *depth = nullptr;
                if (depthOn)
                {
                    // D3D binds a depth buffer only with a colour target of the same size: grow whichever is smaller
                    depth = &depthFor(s.zbp, rt.w, rt.h, rt.scale, rt.samples);
                    growRt(rt, depth->w, depth->h, rt.scale, rt.samples);
                }
                rt.dirty = true;
                colorTarget = rt.color.get();
                depthTarget = depth ? depth->tex.get() : nullptr;
                targetW = static_cast<float>(rt.w);
                targetH = static_cast<float>(rt.h);
                targetScale = rt.scale;
            }

            // texture: decoded VRAM, another render target, or this target itself (sampled from a copy)
            rhi::Texture *tex = nullptr;
            Scale rtScale; // the render target read: its pixels per PS2 pixel
            const bool plainTexture = layer && layer->texture; // a native buffer, read like decoded VRAM
            if (s.tme && plainTexture)
                tex = layer->texture;
            else if (s.tme)
            {
                if (s.texIsRt)
                {
                    auto it = m_rts.find(s.rtFbp);
                    if (it != m_rts.end())
                    {
                        tex = &readable(it->second); // multisampled: a resolved copy, not the bound target
                        rtScale = it->second.scale;
                        if (s.rtFbp == s.fbp && it->second.samples <= 1u)
                            tex = feedbackCopy(*tex);
                    }
                }
                else
                {
                    auto it = m_textures.find(s.texKey);
                    tex = it != m_textures.end() ? it->second.get() : nullptr;
                }
            }

            bindPass(colorTarget, depthTarget);
            {
                // [gta-recomp] the scissor passes GS pixels scx0..scx1, sampled at their integer positions: moved by the
                // pixel-centre offset like the geometry, a target pixel passes when its centre does
                const float o = pixelCentreOffset();
                const float sx = target ? target->scaleX : targetScale.x, sy = target ? target->scaleY : targetScale.y;
                const float ox = target ? static_cast<float>(target->originX) : 0.0f, oy = target ? static_cast<float>(target->originY) : 0.0f;
                const float scw = target ? target->scissorScaleX : 1.0f;
                const int tw = static_cast<int>(colorTarget->desc().width), th = static_cast<int>(colorTarget->desc().height);
                auto edge = [&](float e, float scale, float origin, int size) {
                    return std::clamp(static_cast<int>(std::ceil((e + o - origin) * scale - 0.5f)), 0, size);
                };
                // a squeezed HUD layer draw: its scissor edges go where its vertices went (appendHudDraw), unrounded
                const float pv = target ? target->squeezePivot : 0.0f, sq = target ? target->squeeze : 1.0f;
                auto edgeX = [&](float e) { return sq == 1.0f ? e : pv + (e + o - pv) * sq - o; };
                const int x0 = edge(edgeX(s.scx0 * scw), sx, ox, tw), x1 = std::max(x0, edge(edgeX((s.scx1 + 1.0f) * scw), sx, ox, tw));
                const int y0 = edge(static_cast<float>(s.scy0), sy, oy, th), y1 = std::max(y0, edge(s.scy1 + 1.0f, sy, oy, th));
                m_dev.setScissor(x0, y0, x1 - x0, y1 - y0);
            }

            DrawConstants c{};
            c.targetRtFix[0] = targetW;
            c.targetRtFix[1] = targetH;
            c.targetRtFix[2] = rtScale.x;
            c.targetRtFix[3] = s.fix;
            c.tmeTfxTccFst[0] = s.tme;
            c.tmeTfxTccFst[1] = s.tfx;
            c.tmeTfxTccFst[2] = s.tcc;
            c.tmeTfxTccFst[3] = s.fst;
            c.linearIipFgeAte[0] = s.linear;
            // [gta-recomp] Replacement dimensions are independent of GS dimensions (including non-integer scales).
            const bool replacement = !s.texIsRt && !plainTexture && tex &&
                (s.texKey & 0xFFFFFFFF00000000ull) == render::TextureReplacements::key(0);
            // [gta-recomp] anisotropic filtering (GSHwOptions::anisotropy): a bilinear-filtered game texture with plain
            // repeat / clamp wrapping is sampled by the hardware - trilinear over a mip chain, anisotropic - instead of
            // the GS's own bilinear; region wraps, render targets and nearest filtering keep the exact GS path
            rhi::Sampler *hwSampler = nullptr;
            if (m_anisotropy && s.tme && s.linear && !s.texIsRt && !plainTexture && tex && s.wms <= 1u && s.wmt <= 1u &&
                tex->desc().mipLevels != 1u)
            {
                hwSampler = m_anisoSampler[s.wms == 0u ? 1 : 0][s.wmt == 0u ? 1 : 0].get();
                if (hwSampler)
                    c.linearIipFgeAte[0] = 2;
            }
            // [gta-recomp] the same bilinear-filtered textures (power-of-two, no mip chain) on the exact path: the 2x2 texels by
            // Gather through a nearest sampler with their wrapping instead of four loads with the wrap worked out per tap -
            // the same values, a fraction of the shader (gs_draw.hlsl mode 3). PS2X_HW_GATHER=0: the four loads; =verify: both,
            // a pixel whose texels differ drawn magenta (mode 4)
            static const int gather = [] {
                const char *e = std::getenv("PS2X_HW_GATHER");
                return (e && e[0] == '0') ? 0 : (e && e[0] == 'v') ? 4 : 3;
            }();
            if (gather && !hwSampler && s.tme && s.linear && !s.texIsRt && !plainTexture && tex && s.wms <= 1u && s.wmt <= 1u &&
                tex->desc().mipLevels == 1u && tex->desc().width == s.tw && tex->desc().height == s.th && s.tw &&
                !(s.tw & (s.tw - 1u)) && s.th && !(s.th & (s.th - 1u)))
            {
                hwSampler = m_gatherSampler[s.wms == 0u ? 1 : 0][s.wmt == 0u ? 1 : 0].get();
                if (hwSampler)
                    c.linearIipFgeAte[0] = gather;
            }
            static const bool afLog = std::getenv("PS2X_HW_ANISO_LOG") != nullptr; // which textured draws take it
            if (afLog && s.tme && !s.texIsRt)
            {
                static uint64_t n[5] = {}, total = 0;
                ++n[c.linearIipFgeAte[0] == 2 ? 0 : c.linearIipFgeAte[0] >= 3 ? 4 : !s.linear ? 1 : (s.wms > 1u || s.wmt > 1u) ? 2 : 3];
                if (++total % 20000u == 0u)
                    std::fprintf(stderr, "[gs-hw] aniso: hw %llu, gather %llu, nearest %llu, region wrap %llu, other %llu\n",
                                 (unsigned long long)n[0], (unsigned long long)n[4], (unsigned long long)n[1], (unsigned long long)n[2],
                                 (unsigned long long)n[3]);
            }
            c.linearIipFgeAte[1] = s.iip;
            c.linearIipFgeAte[2] = s.fge;
            c.linearIipFgeAte[3] = s.ate;
            c.atstArefAfailRt[0] = s.atst;
            c.atstArefAfailRt[1] = s.aref;
            c.atstArefAfailRt[2] = 0;
            c.atstArefAfailRt[3] = (s.texIsRt && !plainTexture) ? (s.rtFull ? 2 : 1) : 0; // [gta-recomp] 2: read 1:1 at full resolution
            c.texSizeWrap[0] = s.tw;
            c.texSizeWrap[1] = s.th;
            c.texSizeWrap[2] = s.wms;
            c.texSizeWrap[3] = s.wmt;
            c.region[0] = s.minu;
            c.region[1] = s.maxu;
            c.region[2] = s.minv;
            c.region[3] = s.maxv;
            c.rtOrigin[0] = s.rtOx;
            c.rtOrigin[1] = s.rtOy;
            c.rtOrigin[2] = pixelCentreOffset();
            c.rtOrigin[3] = rtScale.y;
            if (replacement && !hwSampler) // [gta-recomp] full-resolution fetches with GS wrap/region rules
            {
                c.atstArefAfailRt[3] = 2;
                c.targetRtFix[2] = float(tex->desc().width) / std::max<uint32_t>(s.tw, 1u);
                c.rtOrigin[0] = c.rtOrigin[1] = 0.0f;
                c.rtOrigin[3] = float(tex->desc().height) / std::max<uint32_t>(s.th, 1u);
            }
            c.fogColor[0] = s.fogR;
            c.fogColor[1] = s.fogG;
            c.fogColor[2] = s.fogB;

            GsBlend b;
            if (!gsBlend(s, b))
            {
                b.enable = false;
                if (m_reportedBlend.insert(s.ba | (s.bb << 2) | (s.bd << 6)).second)
                    std::fprintf(stderr, "[gs-hw] unsupported blend A=%u B=%u C=%u D=%u (drawn opaque)\n", s.ba, s.bb, s.bc, s.bd);
            }
            c.blendCPremul[0] = s.bc;
            c.blendCPremul[1] = b.premul;

            // frame write mask (whole channels only)
            const uint8_t mr = (s.fbmsk & 0xFFu) != 0xFFu, mg = (s.fbmsk & 0xFF00u) != 0xFF00u, mb = (s.fbmsk & 0xFF0000u) != 0xFF0000u,
                          ma = (s.fbmsk & 0xFF000000u) != 0xFF000000u;
            uint8_t mask = static_cast<uint8_t>(mr | (mg << 1) | (mb << 2) | (ma << 3));
            if (layer && layer->opaque)
                mask &= 7u; // the shown picture stays opaque (the GS writes the source alpha, the window would show it)
            bool zwrite = depthOn && !s.zmsk;

            // pass 1: pixels passing the alpha test (or all of them)
            auto submit = [&](rhi::Pipeline &pipeline)
            {
                m_dev.setPipeline(pipeline);
                m_dev.setVertexBuffer(*vertices, vertexOffset);
                updateConstants(*m_drawCb, &c, sizeof(c), m_drawCbLast, 0);
                m_dev.setConstants(0, *m_drawCb);
                if (native)
                    m_dev.setConstants(1, *m_worldCb);
                m_dev.setTexture(0, tex, hwSampler);
                m_dev.draw(count, first);
            };
            if (s.ate && s.atst != 1u && s.afail != 0u)
            {
                // AFAIL other than KEEP: draw the passing pixels normally, the failing ones with the AFAIL mask
                submit(drawPipeline(b, mask, depthOn, s.ztst, zwrite, s.mode, drawSampleMode(c)));
                c.atstArefAfailRt[2] = 1;
                if (s.afail == 1u) zwrite = false;                      // FB_ONLY
                else if (s.afail == 2u) mask = 0;                       // ZB_ONLY
                else { mask = static_cast<uint8_t>(mask & 7u); zwrite = false; } // RGB_ONLY
            }
            else if (!(s.ate && s.atst != 1u))
            {
                c.linearIipFgeAte[3] = 0; // no alpha test (ATST ALWAYS)
            }
            submit(drawPipeline(b, mask, depthOn, s.ztst, zwrite, s.mode, drawSampleMode(c)));
        }

        // a draw that samples its own target reads a copy (D3D cannot sample a bound render target)
        rhi::Texture *feedbackCopy(rhi::Texture &target)
        {
            const rhi::TextureDesc &td = target.desc();
            if (!m_feedback || m_feedback->desc().width != td.width || m_feedback->desc().height != td.height)
                m_feedback = m_dev.createTexture({td.width, td.height, rhi::Format::RGBA8, rhi::UsageSampled, "gs feedback copy"});
            m_dev.copyTexture(*m_feedback, 0, 0, target, 0, 0, td.width, td.height);
            if (m_feedbackReported++ == 0)
                std::fprintf(stderr, "[gs-hw] a draw samples its own render target: reading a copy\n");
            return m_feedback.get();
        }

        rhi::Device &m_dev;
        uint32_t m_scale;
        bool m_ready = false;
        std::unique_ptr<rhi::Buffer> m_vb, m_drawCb, m_quadCb, m_worldCb;
        std::vector<uint8_t> m_drawCbLast, m_worldCbLast; // [gta-recomp] updateConstants: what the two buffers hold
        std::unordered_map<uint64_t, std::unique_ptr<rhi::Buffer>> m_meshes; // native draws' vertex data
        std::unique_ptr<rhi::Buffer> m_nativeVb; // transient native vertices of the frame
        size_t m_nativeVbCapacity = 0;
        size_t m_vbCapacity = 0;
        std::unique_ptr<rhi::Sampler> m_point, m_linear;
        uint32_t m_msaa = 1, m_anisotropy = 0;           // [gta-recomp] GSHwOptions
        std::unique_ptr<rhi::Sampler> m_anisoSampler[2][2]; // [gta-recomp] [U repeat][V repeat]
        std::unique_ptr<rhi::Sampler> m_gatherSampler[2][2]; // [gta-recomp] nearest, [U repeat][V repeat]: gs_draw.hlsl mode 3
        std::unique_ptr<rhi::Pipeline> m_blit, m_fillColor, m_fillDepth;
        std::unordered_map<uint32_t, std::unique_ptr<rhi::Pipeline>> m_drawPipelines;
        std::unordered_map<uint32_t, Rt> m_rts;
        std::vector<std::unique_ptr<rhi::Texture>> m_spareTargets; // [gta-recomp] spareTarget / makeTarget
        struct DroppedSize
        {
            uint32_t w = 0, h = 0;
            Scale scale;
        };
        std::unordered_map<uint32_t, DroppedSize> m_droppedSize; // [gta-recomp] dropRt / rtFor
        std::unordered_map<uint32_t, Depth> m_depth;
        std::unordered_map<uint64_t, std::unique_ptr<rhi::Texture>> m_textures;
        std::unique_ptr<rhi::Texture> m_display, m_feedback;
        std::unordered_map<uint32_t, HudLayer> m_hud; // [gta-recomp] native HUD layer per frame buffer
        // [gta-recomp] Per frame buffer, the shape (wide or 4:3) of the frame that last drew into it; a plain copy carries
        // it along like the HUD layer. VCS shows the previous frame (its finished picture is copied into the displayed
        // buffer at the start of the next), so the picture on screen at a switch is the other shape's.
        std::unordered_map<uint32_t, bool> m_shapeOf;
        bool m_execWide = true;

    public:
        bool shapeOf(uint32_t fbp, bool fallback) const
        {
            auto it = m_shapeOf.find(fbp);
            return it != m_shapeOf.end() ? it->second : fallback;
        }

    private:
        std::unique_ptr<rhi::Buffer> m_hudVb;
        std::vector<HwVertex> m_hudShifted;
        Scale m_frameScale{0.0f, 0.0f}; // [gta-recomp] the native frame's scale (x 0: off; setFrameSize)
        std::unique_ptr<rhi::Texture> m_postBloom[2], m_postTrails; // [gta-recomp] native post-effect buffers
        std::unique_ptr<rhi::Buffer> m_postVb;
        size_t m_postVbCapacity = 0;
        std::vector<HwVertex> m_postVerts;
        size_t m_hudVbCapacity = 0;
        std::unique_ptr<rhi::Texture> m_hudDepth;
        uint32_t m_displayW = 640, m_displayH = 448;
        std::unordered_set<uint32_t> m_reportedBlend;
        uint64_t m_feedbackReported = 0;
        bool m_inPass = false;
        rhi::Texture *m_passColor = nullptr, *m_passDepth = nullptr;
    };

    // ------------------------------------------------------------------------------------------------------
    class HwBackend final : public GSHwBackendHandle, public GSHwPresenter
    {
    public:
        explicit HwBackend(const GSHwOptions &o)
            : m_scale(o.scale), m_nativeFrame(o.nativeFrame), m_msaa(o.msaa), m_anisotropy(o.anisotropy)
        {
            m_frame = std::make_unique<Frame>();
        }
        ~HwBackend() override { stopTextureMakers(); } // [gta-recomp]

        // ---- GSRasterBackend (game thread) ----------------------------------------------------------------
        void Initialize(uint8_t *vram, uint32_t vramSize) override
        {
            m_cpu.Initialize(vram, vramSize);
            m_textureVram = vram; // [gta-recomp] read-only, after materializing queued texture uploads
        }
        void Reset() override { m_cpu.Reset(); }

        void Submit(const GSPrimitiveBatch &batch) override
        {
            if (batch.vertexCount == 0u)
                return;
            HwState s{};
            if (!buildState(batch, s))
                return;
            const uint8_t type = batch.state.prim.type;
            const auto &ctx = batch.state.context;
            const float ofx = ctx.xyoffset.ofx / 16.0f, ofy = ctx.xyoffset.ofy / 16.0f;
            const float zs = (ctx.zbuf.psm == GS_PSM_Z32) ? (1.0f / 4294967296.0f)
                           : (ctx.zbuf.psm == GS_PSM_Z16 || ctx.zbuf.psm == GS_PSM_Z16S) ? (1.0f / 65536.0f)
                                                                                         : (1.0f / 16777216.0f);
            auto mk = [&](const GSVertex &v) { return hwVertex(batch.state, v, ofx, ofy, zs); };
            HwVertex out[6];
            uint32_t n = 0;
            if (type == GS_PRIM_SPRITE && batch.vertexCount >= 2)
            {
                // Sprite: corners from v0/v1, everything else from v1.
                HwVertex a = mk(batch.vertices[0]), b = mk(batch.vertices[1]);
                HwVertex c = b, d = b;
                c.x = a.x; c.s = a.s; d.y = a.y; d.t = a.t;
                a.r = b.r; a.g = b.g; a.b = b.b; a.a = b.a; a.z = b.z; a.fog = b.fog; a.q = b.q;
                c.q = b.q; d.q = b.q;
                out[0] = a; out[1] = d; out[2] = c; out[3] = d; out[4] = b; out[5] = c;
                n = 6;
                s.mode = 0;
            }
            else if ((type == GS_PRIM_LINE || type == GS_PRIM_LINESTRIP) && batch.vertexCount >= 2)
            {
                out[0] = mk(batch.vertices[0]);
                out[1] = mk(batch.vertices[1]);
                n = 2;
                s.mode = 1;
            }
            else if (type == GS_PRIM_POINT)
            {
                out[0] = mk(batch.vertices[0]);
                n = 1;
                s.mode = 2;
            }
            else if (batch.vertexCount >= 3)
            {
                out[0] = mk(batch.vertices[0]);
                out[1] = mk(batch.vertices[1]);
                out[2] = mk(batch.vertices[2]);
                n = 3;
                s.mode = 0;
            }
            if (!n)
                return;
            recordPrimitive(s, out, n, &batch, ofx, ofy, gsCurrentDrawSource(), true);
        }

        static float depthScale(const GSContext &ctx)
        {
            return (ctx.zbuf.psm == GS_PSM_Z32) ? (1.0f / 4294967296.0f)
                 : (ctx.zbuf.psm == GS_PSM_Z16 || ctx.zbuf.psm == GS_PSM_Z16S) ? (1.0f / 65536.0f)
                                                                               : (1.0f / 16777216.0f);
        }

        static HwVertex hwVertex(const GSDrawState &state, const GSVertex &v, float ofx, float ofy, float zs)
        {
            HwVertex h{};
            h.x = v.x - ofx;
            h.y = v.y - ofy;
            h.z = std::min(static_cast<float>(v.z * zs), 1.0f);
            if (state.prim.fst)
            {
                h.s = v.u / 16.0f;
                h.t = v.v / 16.0f;
                h.q = 1.0f;
            }
            else
            {
                h.s = v.s;
                h.t = v.t;
                h.q = v.q;
            }
            h.r = v.r; h.g = v.g; h.b = v.b; h.a = v.a;
            h.fog = v.fog / 255.0f;
            return h;
        }

        // [gta-recomp] A strip / fan / triangle list at once (GS::submitPackedKick, the C++ reproductions' packets): the
        // primitives Submit would get one vertex kick at a time, recorded by the same code (recordPrimitive) - same
        // vertices, state and merging. Not asked: the HUD classifier (3D packets are never HUD), and the depth buffer /
        // target bookkeeping runs once for the packet. Draws that could be depth fills go the per-vertex way.
        bool SubmitPacked(const GSDrawState &state, const GSVertex *vertices, const uint8_t *kick, uint32_t count) override
        {
            static const bool verify = std::getenv("PS2X_HW_PACKED_VERIFY") != nullptr;
            const bool handled = recordPacked(state, vertices, kick, count);
            if (verify && handled) // after the real recording: texture uploads stay in the real frame
                verifyPacked(state, vertices, kick, count);
            return handled;
        }

        // PS2X_HW_PACKED_VERIFY=1: the packet recorded both ways into scratch frames - packed, and one Submit per
        // primitive as the GS front end would - and every vertex with its draw state compared (the source tag aside)
        struct PackedCheck
        {
            uint64_t packets = 0, exact = 0, bad = 0, split = 0;
        } m_packedCheck;

        void verifyPacked(const GSDrawState &state, const GSVertex *vertices, const uint8_t *kick, uint32_t count)
        {
            std::unique_ptr<Frame> real = std::move(m_frame);
            m_frame = std::make_unique<Frame>();
            const bool handled = recordPacked(state, vertices, kick, count);
            std::unique_ptr<Frame> packed = std::move(m_frame);
            m_frame = std::make_unique<Frame>();
            const uint8_t type = state.prim.type;
            for (uint32_t i = 1; handled && (type == GS_PRIM_LINE || type == GS_PRIM_LINESTRIP) && i < count; ++i)
            {
                if ((type == GS_PRIM_LINE && i % 2u != 1u) || !kick[i])
                    continue;
                GSPrimitiveBatch batch{};
                batch.state = state;
                batch.vertexCount = 2;
                batch.vertices[0] = vertices[i - 1u];
                batch.vertices[1] = vertices[i];
                Submit(batch);
            }
            for (uint32_t i = 2; handled && type != GS_PRIM_LINE && type != GS_PRIM_LINESTRIP && i < count; ++i)
            {
                if ((type == GS_PRIM_TRIANGLE && i % 3u != 2u) || !kick[i])
                    continue;
                GSPrimitiveBatch batch{};
                batch.state = state;
                batch.vertexCount = 3;
                batch.vertices[0] = vertices[type == GS_PRIM_TRIFAN ? 0u : i - 2u];
                batch.vertices[1] = vertices[i - 1u];
                batch.vertices[2] = vertices[i];
                Submit(batch);
            }
            const std::unique_ptr<Frame> perVertex = std::move(m_frame);
            m_frame = std::move(real);
            if (!handled)
                return;
            const Frame &a = *packed, &b = *perVertex;
            // [gta-recomp] each vertex with its state and whether it went to the HUD layer (PATH2 packets are classified)
            const auto expand = [](const Frame &f) {
                std::vector<std::tuple<HwState, HwVertex, bool>> out;
                for (const Cmd &c : f.cmds)
                    if (c.type == CmdType::Draw || c.type == CmdType::HudDraw)
                    {
                        const DrawCmd &d = f.draws[c.index];
                        for (uint32_t k = 0; k < d.count; ++k)
                            out.push_back({d.s, f.vertices[d.first + k], c.type == CmdType::HudDraw});
                    }
                return out;
            };
            const auto drawCmds = [](const Frame &f) {
                return std::count_if(f.cmds.begin(), f.cmds.end(), [](const Cmd &c) { return c.type == CmdType::Draw || c.type == CmdType::HudDraw; });
            };
            const auto ea = expand(a), eb = expand(b);
            bool same = ea.size() == eb.size();
            for (size_t k = 0; same && k < ea.size(); ++k)
                same = std::memcmp(&std::get<0>(ea[k]), &std::get<0>(eb[k]), sizeof(HwState)) == 0 &&
                       std::memcmp(&std::get<1>(ea[k]), &std::get<1>(eb[k]), sizeof(HwVertex)) == 0 &&
                       std::get<2>(ea[k]) == std::get<2>(eb[k]);
            ++m_packedCheck.packets;
            (same ? m_packedCheck.exact : m_packedCheck.bad) += 1;
            if (same && drawCmds(a) != drawCmds(b)) // the same vertices and states, split into more draw calls
                ++m_packedCheck.split;
            if (!same && m_packedCheck.bad <= 8)
            {
                std::fprintf(stderr, "[gs-hw] packed draw differs: %zu / %zu vertices, %zu / %zu draw commands (prim %u, %u vertices, path %u)",
                             ea.size(), eb.size(), static_cast<size_t>(drawCmds(a)), static_cast<size_t>(drawCmds(b)), type, count, gsDrawSourcePath(gsCurrentDrawSource()));
                for (size_t k = 0; k < std::min(ea.size(), eb.size()); ++k)
                {
                    const bool st = std::memcmp(&std::get<0>(ea[k]), &std::get<0>(eb[k]), sizeof(HwState)) != 0;
                    const bool vx = std::memcmp(&std::get<1>(ea[k]), &std::get<1>(eb[k]), sizeof(HwVertex)) != 0;
                    const bool hud = std::get<2>(ea[k]) != std::get<2>(eb[k]);
                    if (!st && !vx && !hud)
                        continue;
                    std::fprintf(stderr, ": vertex %zu differs in%s%s%s, packed (%.3f, %.3f) hud %d, per vertex (%.3f, %.3f) hud %d", k,
                                 st ? " state" : "", vx ? " vertex" : "", hud ? " HUD layer" : "", std::get<1>(ea[k]).x,
                                 std::get<1>(ea[k]).y, int(std::get<2>(ea[k])), std::get<1>(eb[k]).x, std::get<1>(eb[k]).y,
                                 int(std::get<2>(eb[k])));
                    break;
                }
                std::fprintf(stderr, "\n");
            }
            if (m_packedCheck.packets % 20000u == 0u)
                std::fprintf(stderr, "[gs-hw] packed draws checked: %llu packets, %llu exact (%llu split differently), %llu differ\n",
                             (unsigned long long)m_packedCheck.packets, (unsigned long long)m_packedCheck.exact,
                             (unsigned long long)m_packedCheck.split, (unsigned long long)m_packedCheck.bad);
        }

        bool recordPacked(const GSDrawState &state, const GSVertex *vertices, const uint8_t *kick, uint32_t count)
        {
            const uint8_t type = state.prim.type;
            const bool lines = type == GS_PRIM_LINE || type == GS_PRIM_LINESTRIP; // [gta-recomp] Im2D's lines (FULLY_NATIVE.md 5e)
            if (type != GS_PRIM_TRIANGLE && type != GS_PRIM_TRISTRIP && type != GS_PRIM_TRIFAN && !lines)
                return false;
            GSPrimitiveBatch batch{};
            batch.state = state;
            HwState s{};
            if (!buildState(batch, s))
                return true; // ZTST NEVER: Submit draws nothing either
            if (lines) // as Submit draws a line: its two vertices, mode 1, one primitive at a time
            {
                const auto &lctx = state.context;
                const float lofx = lctx.xyoffset.ofx / 16.0f, lofy = lctx.xyoffset.ofy / 16.0f, lzs = depthScale(lctx);
                const uint32_t lsource = gsCurrentDrawSource() | kGsDrawSourcePacked;
                const bool lclassify = gsDrawSourcePath(lsource) == 2u;
                s.mode = 1;
                GSPrimitiveBatch prim{};
                prim.state = state;
                prim.vertexCount = 2;
                thread_local std::vector<HwVertex> lhv;
                lhv.resize(count);
                bool any = false;
                for (uint32_t i = 0; i < count; ++i)
                    lhv[i] = hwVertex(state, vertices[i], lofx, lofy, lzs);
                for (uint32_t i = 1; i < count; ++i)
                {
                    if ((type == GS_PRIM_LINE && i % 2u != 1u) || !kick[i])
                        continue;
                    HwVertex out[2] = {lhv[i - 1u], lhv[i]};
                    any = true;
                    if (lclassify)
                    {
                        prim.vertices[0] = vertices[i - 1u];
                        prim.vertices[1] = vertices[i];
                    }
                    recordPrimitive(s, out, 2u, lclassify ? &prim : nullptr, lofx, lofy, lsource, false);
                }
                if (any)
                {
                    if (s.zte && s.ztst != 1u && s.zbp != s.fbp)
                        noteDepthBuffer(s);
                    noteTarget(s.fbp, s.fbw, s.fpsm, lhv.data(), count, s.scy1);
                }
                return true;
            }
            if (!s.tme && !s.abe && s.fbmsk == 0u && depthBufferAt(s.fbp))
                return false;
            s.mode = 0;
            const auto &ctx = state.context;
            const float ofx = ctx.xyoffset.ofx / 16.0f, ofy = ctx.xyoffset.ofy / 16.0f, zs = depthScale(ctx);
            thread_local std::vector<HwVertex> hv;
            hv.resize(count);
            for (uint32_t i = 0; i < count; ++i)
                hv[i] = hwVertex(state, vertices[i], ofx, ofy, zs);
            const uint32_t source = gsCurrentDrawSource() | kGsDrawSourcePacked;
            // [gta-recomp] PATH2 (the game's Im2D: HUD, text, menus, sprites) is asked about by the HUD classifier, with each
            // primitive's GS vertices as the front end's Submit gives them; the VU1 reproductions' 3D packets are never HUD
            const bool classify = gsDrawSourcePath(source) == 2u;
            // [gta-recomp] The 3D packets (not PATH2, no render-target texture, no widescreen HUD squeeze to decide per
            // primitive): what recordPrimitive does per triangle - flat colour, the 3D flag - inline, then one state
            // check for the whole packet and its triangles appended at once (was a state copy + memcmp per triangle,
            // ~10% of the render worker in the city). The same draws and vertices as the per-primitive path.
            const bool wide = g_hudSqueeze.load(std::memory_order_relaxed) < 0.9999f && frameWide();
            if (!classify && !s.texIsRt && !(wide && s.fbw == 10u))
            {
                Frame &f = *m_frame;
                const size_t firstVertex = f.vertices.size();
                const bool depthTested = s.zte && s.ztst >= 2u;
                bool had3d = false;
                for (uint32_t i = 2; i < count; ++i)
                {
                    if ((type == GS_PRIM_TRIANGLE && i % 3u != 2u) || !kick[i])
                        continue;
                    const uint32_t a = type == GS_PRIM_TRIFAN ? 0u : i - 2u;
                    HwVertex out[3] = {hv[a], hv[i - 1u], hv[i]};
                    if (!s.iip)
                        for (int k = 0; k < 2; ++k)
                        {
                            out[k].r = out[2].r;
                            out[k].g = out[2].g;
                            out[k].b = out[2].b;
                            out[k].a = out[2].a;
                        }
                    if (depthTested && !(out[0].z >= 0.99999f && out[1].z >= 0.99999f && out[2].z >= 0.99999f))
                        had3d = true;
                    f.vertices.insert(f.vertices.end(), out, out + 3);
                }
                const uint32_t added = static_cast<uint32_t>(f.vertices.size() - firstVertex);
                if (added == 0u)
                    return true;
                if (had3d)
                    f.had3d = true;
                if (f.draws.empty() || f.cmds.empty() || f.cmds.back().type != CmdType::Draw || f.draws.back().source != source ||
                    std::memcmp(&f.draws.back().s, &s, sizeof(HwState)) != 0)
                {
                    f.cmds.push_back({CmdType::Draw, static_cast<uint32_t>(f.draws.size())});
                    f.draws.push_back({s, static_cast<uint32_t>(firstVertex), 0u, source});
                }
                f.draws.back().count += added;
                ps2hle::g_gsVerticesRecorded += added;
                if (s.zte && s.ztst != 1u && s.zbp != s.fbp)
                    noteDepthBuffer(s);
                noteTarget(s.fbp, s.fbw, s.fpsm, hv.data(), count, s.scy1);
                return true;
            }
            GSPrimitiveBatch prim{};
            prim.state = state;
            prim.vertexCount = 3;
            bool any = false;
            for (uint32_t i = 2; i < count; ++i)
            {
                if (type == GS_PRIM_TRIANGLE && i % 3u != 2u)
                    continue;
                if (!kick[i])
                    continue;
                const uint32_t a = type == GS_PRIM_TRIFAN ? 0u : i - 2u;
                HwVertex out[3] = {hv[a], hv[i - 1u], hv[i]};
                any = true;
                if (classify)
                {
                    prim.vertices[0] = vertices[a];
                    prim.vertices[1] = vertices[i - 1u];
                    prim.vertices[2] = vertices[i];
                }
                recordPrimitive(s, out, 3u, classify ? &prim : nullptr, ofx, ofy, source, false);
            }
            if (any)
            {
                if (s.zte && s.ztst != 1u && s.zbp != s.fbp)
                    noteDepthBuffer(s);
                noteTarget(s.fbp, s.fbw, s.fpsm, hv.data(), count, s.scy1);
            }
            return true;
        }

        void noteDepthBuffer(const HwState &s) // a depth buffer in real use (tested against)
        {
            DepthInfo &di = m_depthBufs[s.zbp];
            di.zpsm = s.zpsm;
            di.fbw = s.fbw;
            di.height = std::max<uint32_t>(di.height, std::min<uint32_t>(s.scy1 + 1u, 1024u));
        }

        // One primitive's vertices (out, n) into the frame: flat colour, render-target sampling, the widescreen HUD
        // squeeze / native HUD layer (batch: the GS vertices for the HUD classifier; nullptr = not asked), then merged
        // into the current draw. bookkeeping: depth buffer and target size here (SubmitPacked does them per packet).
        void recordPrimitive(HwState s, HwVertex *out, uint32_t n, const GSPrimitiveBatch *batch, float ofx, float ofy,
                             uint32_t source, bool bookkeeping)
        {
            if (!s.iip) // [gta-recomp] flat: the GS colours with the last vertex; give every vertex that colour
                for (uint32_t i = 0; i + 1 < n; ++i)
                {
                    out[i].r = out[n - 1].r;
                    out[i].g = out[n - 1].g;
                    out[i].b = out[n - 1].b;
                    out[i].a = out[n - 1].a;
                }
            if (s.texIsRt && rtFullScale())
                s.rtFull = isOneToOne(s, out, n) ? 1u : 0u;
            // [gta-recomp] VCS draws its HUD, text, radar and menus (RwIm2D) at the largest Z, whatever the depth test
            // (RenderWare keeps ZTE on: ZTE=0 is prohibited on the GS); projected sprites (clouds, coronas) take the
            // same path at their real depth.
            bool far = true;
            float x0 = out[0].x, x1 = out[0].x;
            for (uint32_t i = 0; i < n; ++i)
            {
                x0 = std::min(x0, out[i].x);
                x1 = std::max(x1, out[i].x);
                far = far && out[i].z >= 0.99999f; // the largest Z24 / Z32 value
            }
            // 3D (widescreen presentation): depth-tested, not at the far plane
            if (s.zte && s.ztst >= 2u && !far)
                m_frame->had3d = true;
            // widescreen: the HUD keeps its 4:3 shape in the middle (gsHwSetHudSqueeze) while the picture is shown
            // wide - draws at the far plane, except full-width ones (sky, fades) and the screen effects that sample the
            // frame (drawn in strips). The scissor goes with them (the radar and menu labels are clipped by it).
            // [gta-recomp] With a HUD classifier (gsHwSetHudClassifier) the game's own HUD primitives are squeezed towards their
            // element's anchor instead, whatever their depth.
            const float squeeze = g_hudSqueeze.load(std::memory_order_relaxed);
            const GSHwAnchorFn classify = g_hudClassifier.load(std::memory_order_relaxed);
            bool hud = false; // a HUD primitive for the native layer
            float hudPivot = 0.0f, hudSqueeze = 1.0f; // its squeeze, applied in the layer (DrawCmd::hudSqueeze)
            // the classifier also names menus in frames without 3D (the pause menu); the squeeze is for 3D frames only.
            // [gta-recomp] A HUD layer draw records its squeeze in any widescreen frame and the layer applies it when the
            // frame turns out wide (decideShape); a draw squeezed into the buffer goes by the recording-time guess
            const bool widescreen = squeeze < 0.9999f;
            const bool wide = widescreen && frameWide();
            if ((wide || classify) && s.fbw == 10u)
            {
                uint8_t anchor = GSHwAnchorNone;
                // [gta-recomp] only PATH2 (the game's Im2D) is HUD: a 3D triangle whose corners happen to land on HUD vertex
                // positions was taken into the HUD layer and squeezed (seen at 16:9 with the per-vertex path)
                if (classify && batch && gsDrawSourcePath(source) == 2u)
                {
                    float xy[2 * 8];
                    const uint32_t count = std::min<uint32_t>(batch->vertexCount, 8u);
                    for (uint32_t i = 0; i < count; ++i)
                    {
                        xy[2 * i] = batch->vertices[i].x - ofx;
                        xy[2 * i + 1] = batch->vertices[i].y - ofy;
                    }
                    anchor = classify(xy, count);
                    if (anchor & GSHwAnchorMenu) // [gta-recomp] the game's menu: the frame may switch to 4:3 at once
                    {
                        m_frame->menu = true;
                        anchor &= static_cast<uint8_t>(~GSHwAnchorMenu);
                    }
                    hud = anchor != GSHwAnchorNone && g_hudLayer.load(std::memory_order_relaxed);
                }
                if (anchor == GSHwAnchorNone && far && !s.texIsRt && !(x0 <= 1.0f && x1 >= 639.0f) && wide)
                    anchor = GSHwAnchorCentre;
                if (anchor != GSHwAnchorNone && anchor != GSHwAnchorStretch && (hud ? widescreen : wide))
                {
                    // x' = pivot + (x - pivot) * squeeze: the pivot stays where it is (an edge or the middle)
                    const float pivot = anchor == GSHwAnchorLeft ? 0.0f : anchor == GSHwAnchorRight ? 640.0f : 320.0f;
                    // [gta-recomp] the native layer squeezes after snapping to the PS2's pixels: squeezed first, a PS2 pixel
                    // spans several screen pixels (6 at 32:9) and the radar's scissor and corner mask rounded to different
                    // ones - a column of map beside the circle
                    if (hud)
                    {
                        hudPivot = pivot;
                        hudSqueeze = squeeze;
                    }
                    for (uint32_t i = 0; i < n && !hud; ++i)
                        out[i].x = pivot + (out[i].x - pivot) * squeeze;
                    if (!hud && (s.scx0 > 0u || s.scx1 < 639u))
                    {
                        // the squeezed scissor passes the pixels whose sample position falls in the squeezed span of
                        // the original pixels (GS: pixel X samples at X); outward rounding let the radar map show in
                        // a pixel column beside its corner mask
                        const float a = pivot + (s.scx0 - pivot) * squeeze, b = pivot + (s.scx1 + 1.0f - pivot) * squeeze;
                        const bool gs = gsPixelCentreOffset() > 0.0f;
                        s.scx0 = static_cast<uint16_t>(std::max(0.0f, gs ? std::ceil(a) : std::floor(a)));
                        s.scx1 = static_cast<uint16_t>(std::min(639.0f, std::ceil(b) - 1.0f));
                    }
                }
            }
            if (bookkeeping)
            {
                if (s.zte && s.ztst != 1u && s.zbp != s.fbp)
                    noteDepthBuffer(s);
                if (batch && depthFill(*batch, s, out, n))
                    return;
                noteTarget(s.fbp, s.fbw, s.fpsm, out, n, s.scy1);
            }
            Frame &f = *m_frame;
            const CmdType cmdType = hud ? CmdType::HudDraw : CmdType::Draw;
            if (f.draws.empty() || f.cmds.empty() || f.cmds.back().type != cmdType || f.draws.back().source != source ||
                f.draws.back().hudPivot != hudPivot || f.draws.back().hudSqueeze != hudSqueeze ||
                std::memcmp(&f.draws.back().s, &s, sizeof(HwState)) != 0)
            {
                f.cmds.push_back({cmdType, static_cast<uint32_t>(f.draws.size())});
                f.draws.push_back({s, static_cast<uint32_t>(f.vertices.size()), 0u, source, hudPivot, hudSqueeze});
            }
            f.vertices.insert(f.vertices.end(), out, out + n);
            f.draws.back().count += n;
            ps2hle::g_gsVerticesRecorded += n; // [gta-recomp] PS2X_HLE_SURVEY
        }

        // [gta-recomp] A render target read 1:1: one texel per pixel, no rotation (VCS's copies of its picture: the shown
        // buffer, the self-copy over the sky, motion blur). The shader then reads the target at its own resolution
        // (sampleRtFull) - otherwise every copy brought the picture back to 640x448 and a higher internal resolution
        // was lost. Resampling reads (the bloom and trails downsamples, their blends back) keep PS2-texel sampling, so
        // those effects look as at 640x448. PS2X_HW_RT_FULL=0 = PS2-texel sampling everywhere.
        static bool rtFullScale()
        {
            static const bool on = [] {
                const char *e = std::getenv("PS2X_HW_RT_FULL");
                return !(e && e[0] == '0');
            }();
            return on;
        }

        static bool isOneToOne(const HwState &s, const HwVertex *v, uint32_t n)
        {
            if (n < 3u || s.mode != 0u)
                return false;
            const float su = s.fst ? 1.0f : static_cast<float>(s.tw), sv = s.fst ? 1.0f : static_cast<float>(s.th);
            auto u = [&](const HwVertex &p) { return p.s / p.q * su; };
            auto w = [&](const HwVertex &p) { return p.t / p.q * sv; };
            const double x1 = v[1].x - v[0].x, y1 = v[1].y - v[0].y, x2 = v[2].x - v[0].x, y2 = v[2].y - v[0].y;
            const double det = x1 * y2 - x2 * y1;
            if (std::fabs(det) < 1e-6)
                return false;
            const double u1 = u(v[1]) - u(v[0]), u2 = u(v[2]) - u(v[0]), v1 = w(v[1]) - w(v[0]), v2 = w(v[2]) - w(v[0]);
            const double dudx = (u1 * y2 - u2 * y1) / det, dudy = (u2 * x1 - u1 * x2) / det;
            const double dvdx = (v1 * y2 - v2 * y1) / det, dvdy = (v2 * x1 - v1 * x2) / det;
            constexpr double tol = 0.01; // the self-copy maps 448 rows to 450 texels (1.004)
            return std::fabs(dudx - 1.0) < tol && std::fabs(dvdy - 1.0) < tol && std::fabs(dudy) < tol && std::fabs(dvdx) < tol;
        }

        struct DepthInfo // [gta-recomp] depth buffers seen in use (see depthFill)
        {
            uint32_t zpsm = 0, fbw = 1, height = 1; // height: lowest scissor row + 1 of the draws using it
        };

        // [gta-recomp] Games clear Z by drawing a flat colour sprite into the depth buffer's memory. VCS (Z24 640x448
        // at zbp 280) draws one 640x224 CT32 sprite with FBP = 280 and ZBP = 350, Z test ALWAYS, Z write on: the colour
        // write clears the first half of the Z buffer's pages, its own depth write (Z 0 at page 350 onwards) the second
        // half. Our depth buffers are GPU textures, not VRAM, so both writes become depth clears of the rows of the real
        // depth buffer whose pages they cover. Without this the bottom half of the Z buffer kept stale depth: the world
        // vanished wherever the camera had moved (ground and sky gone when driving, after camera cuts).
        bool depthFill(const GSPrimitiveBatch &batch, const HwState &s, const HwVertex *v, uint32_t n)
        {
            if (!depthBufferAt(s.fbp) || s.tme || s.abe || s.fbmsk != 0u)
                return false;
            if (s.fpsm != GS_PSM_CT32 && s.fpsm != GS_PSM_CT24 && s.fpsm != GS_PSM_CT16 && s.fpsm != GS_PSM_CT16S)
                return false;
            float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
            for (uint32_t i = 0; i < n; ++i)
            {
                x0 = std::min(x0, v[i].x);
                x1 = std::max(x1, v[i].x);
                y0 = std::min(y0, v[i].y);
                y1 = std::max(y1, v[i].y);
            }
            // VCS draws the clear as 40 narrow column strips; each clears whole rows here (the strips together
            // cover them anyway), so the x extent is not checked
            (void)x0;
            (void)x1;
            y0 = std::max(y0, static_cast<float>(s.scy0));
            y1 = std::min(y1, static_cast<float>(s.scy1) + 1.0f);
            if (y1 <= y0)
                return true;
            const GSVertex &last = batch.vertices[batch.vertexCount - 1u];
            const uint32_t rgba = last.r | (uint32_t(last.g) << 8) | (uint32_t(last.b) << 16) | (uint32_t(last.a) << 24);
            // the colour write
            fillDepthPages(s.fbp, s.fbw, s.fpsm, y0, y1, [&](uint32_t zpsm) {
                return (zpsm == GS_PSM_Z32) ? static_cast<float>(rgba / 4294967296.0)
                     : (zpsm == GS_PSM_Z16 || zpsm == GS_PSM_Z16S) ? (rgba & 0xFFFFu) / 65536.0f
                                                                   : (rgba & 0xFFFFFFu) / 16777216.0f;
            });
            // the draw's own depth write, when it lands inside a depth buffer too
            if (s.zte && s.ztst == 1u && !s.zmsk)
            {
                const float z = v[n - 1u].z;
                fillDepthPages(s.zbp, s.fbw, s.zpsm, y0, y1, [&](uint32_t) { return z; });
            }
            return true;
        }

        // The depth buffer whose pages contain `page`, if any.
        const std::pair<const uint32_t, DepthInfo> *depthBufferAt(uint32_t page) const
        {
            for (const auto &entry : m_depthBufs)
            {
                uint32_t zpw, zph;
                pageSize(entry.second.zpsm, zpw, zph);
                const uint32_t perRow = std::max<uint32_t>(1u, (std::max<uint32_t>(entry.second.fbw, 1u) * 64u + zpw - 1u) / zpw);
                if (page >= entry.first && page < entry.first + perRow * ((entry.second.height + zph - 1u) / zph))
                    return &entry;
            }
            return nullptr;
        }

        // Rows y0..y1 of a buffer at `base` (width fbw, format psm) as a depth clear of the depth buffer holding them.
        template <typename ValueFn>
        void fillDepthPages(uint32_t base, uint32_t fbw, uint32_t psm, float y0, float y1, ValueFn value)
        {
            const auto *depth = depthBufferAt(base);
            if (!depth)
                return;
            const DepthInfo &di = depth->second;
            uint32_t pw, ph, zpw, zph;
            pageSize(psm, pw, ph);
            pageSize(di.zpsm, zpw, zph);
            const uint32_t perRow = std::max<uint32_t>(1u, (std::max<uint32_t>(fbw, 1u) * 64u + pw - 1u) / pw);
            const uint32_t depthPerRow = std::max<uint32_t>(1u, (std::max<uint32_t>(di.fbw, 1u) * 64u + zpw - 1u) / zpw);
            const uint32_t pageFirst = base - depth->first + static_cast<uint32_t>(y0) / ph * perRow;
            const uint32_t pageLast = base - depth->first + (static_cast<uint32_t>(y1) + ph - 1u) / ph * perRow; // exclusive
            DepthFillCmd fill;
            fill.zbp = depth->first;
            fill.width = di.fbw * 64u;
            fill.y0 = pageFirst / depthPerRow * zph;
            fill.y1 = (pageLast + depthPerRow - 1u) / depthPerRow * zph;
            fill.value = value(di.zpsm);
            static int logged = 0;
            static const bool hwDebug = std::getenv("PS2X_HW_DEBUG") != nullptr; // [gta-recomp] read once
            if (hwDebug && logged++ < 8)
                std::fprintf(stderr, "[gs-hw] depth fill: base=%u fbw=%u psm=%u y=%.0f..%.0f -> zbp=%u zpsm=%u rows %u..%u value %f\n", base,
                             fbw, psm, y0, y1, fill.zbp, di.zpsm, fill.y0, fill.y1, fill.value);
            Frame &f = *m_frame;
            f.cmds.push_back({CmdType::DepthFill, static_cast<uint32_t>(f.depthFills.size())});
            f.depthFills.push_back(fill);
            static int extra = 0; // a clear is two fills per frame; more would be a misread draw
            if (f.depthFills.size() > 2u && hwDebug && extra++ < 20)
                std::fprintf(stderr, "[gs-hw] unexpected depth fill #%zu this frame: base=%u fbw=%u psm=%u rows %u..%u\n", f.depthFills.size(),
                             base, fbw, psm, fill.y0, fill.y1);
        }

        void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override
        {
            static const bool debug = std::getenv("PS2X_HW_DEBUG") != nullptr;
            if (debug && tex0.cld != 0u && m_loggedTransfers < 400u)
            {
                ++m_loggedTransfers;
                std::fprintf(stderr, "[gs-hw] clut cld=%u cbp=%u cpsm=%u csm=%u csa=%u psm=%u\n", tex0.cld, tex0.cbp, tex0.cpsm, tex0.csm,
                             tex0.csa, tex0.psm);
            }
            if (tex0.cld != 0u && loadClutFromQueuedUpload(tex0, texclut))
            {
                ++m_texStateVersion;
                return;
            }
            if (tex0.cld != 0u)
            {
                const bool eightBit = tex0.psm == GS_PSM_T8 || tex0.psm == GS_PSM_T8H;
                if (materializeRectKeepTransfer(tex0.cpsm, tex0.cbp, 1u, 0u, 0u, eightBit ? 16u : 8u, eightBit ? 16u : 2u))
                    ++m_lazyWhy[0];
            }
            m_cpu.LoadClut(tex0, texclut);
            ++m_texStateVersion;
        }

        // [gta-recomp] docs/FULLY_NATIVE.md step 3c: a texture decoded on a cache miss from the queued upload that wrote it,
        // not from CPU VRAM. Each texel is read at its own VRAM address (GSMem::PixelBits) out of the upload's pixels at
        // theirs - the GS's own relation between the two formats, so a T8 texture the game sent as CT32 decodes too -
        // then converted as DecodeTexture does (CLUT / TEXA). Used when one queued upload is the latest writer of every
        // block the texture reads and holds every texel; otherwise VRAM as before. PS2X_HW_DECODE_FROM_UPLOAD=0: always
        // VRAM. PS2X_HW_DECODE_VERIFY=1: the VRAM decode runs as well and every pixel is compared.
        bool decodeFromQueuedUpload(const GSDrawState &d, std::vector<uint32_t> &out)
        {
            static const bool on = [] {
                const char *e = std::getenv("PS2X_HW_DECODE_FROM_UPLOAD");
                return !(e && e[0] == '0');
            }();
            static const bool verify = std::getenv("PS2X_HW_DECODE_VERIFY") != nullptr;
            if (!on || m_pendingUploads.empty())
                return false;
            const GSTex0Reg &t = d.context.tex0;
            const uint32_t w = d.textureWidth, h = d.textureHeight;
            m_scratchRead.clear();
            rectBlocks(t.psm, t.tbp0, t.tbw, 0u, 0u, w, h, m_scratchRead, nullptr);
            const PendingUpload *source = soleQueuedWriter(m_scratchRead);
            if (!source)
                return false;
            const GSTransferCommand &c = source->cmd;
            const uint32_t ubits = static_cast<uint32_t>(transferBitsPerPixel(c.bitbltbuf.dpsm));
            if (ubits != 32u && ubits != 16u && ubits != 8u && ubits != 4u)
                return false;
            const uint32_t uw = c.trxreg.rrw, uh = c.trxreg.rrh;
            // [gta-recomp] (2026-10-26) the upload wrote the texture in its own format, at its base, width and origin: the
            // texel at (x, y) is the upload's pixel at (x, y) - the swizzle maps both to the same VRAM bits - as long as no
            // row is wider than the buffer (where addresses alias). The texels straight from the pixels, without three
            // GSMem::PixelBits per texel (most of the GS stage's time in a texture-streaming burst). PS2X_HW_DECODE_FAST=0:
            // the general mapping always.
            static const bool fast = [] {
                const char *e = std::getenv("PS2X_HW_DECODE_FAST");
                return !(e && e[0] == '0');
            }();
            if (fast && c.bitbltbuf.dpsm == t.psm && c.bitbltbuf.dbp == t.tbp0 && c.bitbltbuf.dbw == t.tbw && c.trxpos.dsax == 0u &&
                c.trxpos.dsay == 0u && uw >= w && uh >= h && t.tbw != 0u && uw <= t.tbw * 64u &&
                static_cast<uint64_t>(uw) * uh * ubits <= static_cast<uint64_t>(source->data.size()) * 8u)
            {
                uint32_t width = 0, bit = 0;
                if (!GSMem::PixelBits(t.psm, t.tbp0, t.tbw, 0u, 0u, bit, width) || width != ubits)
                    goto general; // (a format whose texels are not the upload's whole pixels: P8H / P4HL / P4HH)
                thread_local std::vector<uint32_t> direct;
                direct.resize(static_cast<size_t>(w) * h);
                const uint8_t *px = source->data.data();
                for (uint32_t y = 0; y < h; ++y)
                {
                    const size_t row = static_cast<size_t>(y) * uw;
                    uint32_t *o = direct.data() + static_cast<size_t>(y) * w;
                    if (ubits == 32u)
                        std::memcpy(o, px + row * 4u, static_cast<size_t>(w) * 4u);
                    else if (ubits == 16u)
                        for (uint32_t x = 0; x < w; ++x)
                        {
                            uint16_t v;
                            std::memcpy(&v, px + (row + x) * 2u, 2);
                            o[x] = v;
                        }
                    else if (ubits == 8u)
                        for (uint32_t x = 0; x < w; ++x)
                            o[x] = px[row + x];
                    else // 4 bits, low nibble first
                        for (uint32_t x = 0; x < w; ++x)
                            o[x] = (px[(row + x) >> 1] >> (((row + x) & 1u) * 4u)) & 0xFu;
                }
                m_cpu.DecodeTextureFromTexels(d, direct, out);
                ++m_decodeFromUpload;
                ++m_decodeFast;
                if (verify)
                {
                    std::vector<uint32_t> fromVram;
                    materializeRectKeepTransfer(t.psm, t.tbp0, t.tbw, 0u, 0u, w, h);
                    m_cpu.DecodeTexture(d, fromVram);
                    ++(fromVram == out ? m_decodeVerifySame : m_decodeVerifyDiffer);
                    if ((m_decodeVerifySame + m_decodeVerifyDiffer) % 500u == 0u)
                        std::fprintf(stderr, "[gs-hw] textures from upload checked: %llu the same as from VRAM, %llu differ\n",
                                     (unsigned long long)m_decodeVerifySame, (unsigned long long)m_decodeVerifyDiffer);
                }
                return true;
            }
        general:
            // the upload's pixels by VRAM address (in units of its own pixels)
            uint32_t lo = ~0u, hi = 0u;
            for (uint32_t pass = 0; pass < 2u; ++pass)
            {
                for (uint32_t v = 0; v < uh; ++v)
                    for (uint32_t u = 0; u < uw; ++u)
                    {
                        uint32_t bit = 0, width = 0;
                        if (!GSMem::PixelBits(c.bitbltbuf.dpsm, c.bitbltbuf.dbp, c.bitbltbuf.dbw, c.trxpos.dsax + u, c.trxpos.dsay + v, bit, width))
                            return false;
                        const uint32_t key = bit / ubits;
                        if (pass == 0u)
                        {
                            lo = std::min(lo, key);
                            hi = std::max(hi, key);
                        }
                        else
                            m_scratchPixelAt[key - lo] = static_cast<int32_t>(v * uw + u);
                    }
                if (pass == 0u)
                {
                    if (lo > hi || hi - lo > (64u << 20) / 4u)
                        return false;
                    m_scratchPixelAt.assign(hi - lo + 1u, -1);
                }
            }
            // each texel's bits out of the upload's pixel holding them
            thread_local std::vector<uint32_t> texels;
            texels.resize(static_cast<size_t>(w) * h);
            const uint8_t *bytes = source->data.data();
            const size_t dataBits = source->data.size() * 8u;
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x)
                {
                    uint32_t bit = 0, width = 0;
                    if (!GSMem::PixelBits(t.psm, t.tbp0, t.tbw, x, y, bit, width) || width > 32u)
                        return false;
                    const uint32_t key = bit / ubits, within = bit % ubits;
                    if (key < lo || key > hi || within + width > ubits || m_scratchPixelAt[key - lo] < 0)
                        return false; // a texel the upload does not hold
                    const size_t at = static_cast<size_t>(m_scratchPixelAt[key - lo]) * ubits + within;
                    if (at + width > dataBits)
                        return false;
                    uint64_t word = 0;
                    std::memcpy(&word, bytes + at / 8u, std::min<size_t>(8u, source->data.size() - at / 8u));
                    texels[static_cast<size_t>(y) * w + x] = static_cast<uint32_t>((word >> (at % 8u)) & ((width >= 32u) ? 0xFFFFFFFFull : ((1ull << width) - 1ull)));
                }
            m_cpu.DecodeTextureFromTexels(d, texels, out);
            ++m_decodeFromUpload;
            if (c.bitbltbuf.dpsm != t.psm)
                ++m_decodeCrossFormat;
            if (verify)
            {
                std::vector<uint32_t> fromVram;
                materializeRectKeepTransfer(t.psm, t.tbp0, t.tbw, 0u, 0u, w, h);
                m_cpu.DecodeTexture(d, fromVram);
                const bool same = fromVram == out;
                ++(same ? m_decodeVerifySame : m_decodeVerifyDiffer);
                if (!same && m_decodeVerifyDiffer <= 8u)
                {
                    size_t k = 0;
                    while (k < out.size() && k < fromVram.size() && out[k] == fromVram[k])
                        ++k;
                    std::fprintf(stderr, "[gs-hw] texture from upload differs: tbp %u tbw %u psm %u %ux%u from upload dpsm %u %ux%u at (%u, %u); "
                                         "first at texel %zu: %08x vs VRAM %08x\n", t.tbp0, t.tbw, t.psm, w, h, c.bitbltbuf.dpsm, uw, uh,
                                 c.trxpos.dsax, c.trxpos.dsay, k, k < out.size() ? out[k] : 0u, k < fromVram.size() ? fromVram[k] : 0u);
                }
                if ((m_decodeVerifySame + m_decodeVerifyDiffer) % 500u == 0u)
                    std::fprintf(stderr, "[gs-hw] textures from upload checked: %llu the same as from VRAM, %llu differ\n",
                                 (unsigned long long)m_decodeVerifySame, (unsigned long long)m_decodeVerifyDiffer);
            }
            return true;
        }

        // [gta-recomp] docs/FULLY_NATIVE.md step 3: a CLUT load reads its palette from the queued upload that wrote it - the
        // linear rectangle the game sent - instead of writing that upload (and every queued upload overlapping it) into
        // CPU VRAM to read it back. VCS uploads each texture's CLUT with the texture on every bind, so this was ~80% of
        // the swizzled VRAM writes while driving. Used when one queued whole upload at the CLUT's base, in its format,
        // is the latest writer of every block the load reads and covers the pixels it reads; otherwise VRAM as before.
        // PS2X_HW_CLUT_FROM_UPLOAD=0: always VRAM. PS2X_HW_CLUT_VERIFY=1: the VRAM load runs as well and the 512 CLUT
        // entries are compared.
        bool loadClutFromQueuedUpload(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
        {
            static const bool on = [] {
                const char *e = std::getenv("PS2X_HW_CLUT_FROM_UPLOAD");
                return !(e && e[0] == '0');
            }();
            static const bool verify = std::getenv("PS2X_HW_CLUT_VERIFY") != nullptr;
            if (!on || m_pendingUploads.empty())
                return false;
            const bool eightBit = tex0.psm == GS_PSM_T8 || tex0.psm == GS_PSM_T8H;
            const bool csm2 = tex0.csm != 0u;
            const uint32_t bw = csm2 ? std::max<uint32_t>(texclut.cbw, 1u) : 1u;
            const uint32_t x0 = csm2 ? texclut.cou * 16u : 0u, y0 = csm2 ? texclut.cov : 0u;
            const uint32_t w = csm2 ? (eightBit ? 256u : 16u) : (eightBit ? 16u : 8u), h = csm2 ? 1u : (eightBit ? 16u : 2u);
            m_scratchRead.clear();
            rectBlocks(tex0.cpsm, tex0.cbp, bw, x0, y0, w, h, m_scratchRead, nullptr);
            const PendingUpload *source = soleQueuedWriter(m_scratchRead);
            if (!source)
                return false;
            const GSTransferCommand &c = source->cmd;
            if (c.bitbltbuf.dbp != tex0.cbp || c.bitbltbuf.dpsm != tex0.cpsm || c.trxpos.dsax != 0u || c.trxpos.dsay != 0u ||
                (c.bitbltbuf.dpsm != GS_PSM_CT32 && c.bitbltbuf.dpsm != GS_PSM_CT16))
                return false;
            const int loaded = m_cpu.LoadClutFromRect(tex0, texclut, source->data.data(), c.trxreg.rrw, c.trxreg.rrh);
            if (loaded < 0)
                return false;
            ++m_clutFromUpload;
            if (verify && loaded > 0)
            {
                const std::array<uint16_t, 512> fromUpload = m_cpu.ClutEntries();
                materializeRectKeepTransfer(tex0.cpsm, tex0.cbp, bw, x0, y0, w, h);
                m_cpu.ReloadClutFromVram(tex0, texclut);
                const bool same = fromUpload == m_cpu.ClutEntries();
                ++(same ? m_clutVerifySame : m_clutVerifyDiffer);
                if (!same && m_clutVerifyDiffer <= 8u)
                    std::fprintf(stderr, "[gs-hw] clut from upload differs: cbp %u cpsm %u csm %u csa %u psm %u upload %ux%u\n", tex0.cbp,
                                 tex0.cpsm, tex0.csm, tex0.csa, tex0.psm, c.trxreg.rrw, c.trxreg.rrh);
                if ((m_clutVerifySame + m_clutVerifyDiffer) % 100000u == 0u)
                    std::fprintf(stderr, "[gs-hw] clut from upload checked: %llu the same as from VRAM, %llu differ\n",
                                 (unsigned long long)m_clutVerifySame, (unsigned long long)m_clutVerifyDiffer);
            }
            return true;
        }

        void BeginTransfer(const GSTransferCommand &command) override
        {
            if (command.direction != 0u) // local -> host / local -> local read VRAM right away
                materializeAll();
            m_transfer = command;
            m_cpu.BeginTransfer(command);
            static const bool debug = std::getenv("PS2X_HW_DEBUG") != nullptr;
            if (debug && m_loggedTransfers < 400u)
            {
                ++m_loggedTransfers;
                std::fprintf(stderr, "[gs-hw] transfer dir=%u sbp=%u sbw=%u spsm=%u ss=%u,%u -> dbp=%u dbw=%u dpsm=%u ds=%u,%u size=%ux%u\n",
                             command.direction, command.bitbltbuf.sbp, command.bitbltbuf.sbw, command.bitbltbuf.spsm,
                             command.trxpos.ssax, command.trxpos.ssay, command.bitbltbuf.dbp, command.bitbltbuf.dbw,
                             command.bitbltbuf.dpsm, command.trxpos.dsax, command.trxpos.dsay, command.trxreg.rrw, command.trxreg.rrh);
            }
            if (command.direction == 2u) // local -> local: destination pages change
                touchTransferDest();
        }

        void UploadImage(const uint8_t *data, uint32_t sizeBytes) override
        {
            static const bool debug = std::getenv("PS2X_HW_DEBUG") != nullptr;
            if (debug && m_transfer.bitbltbuf.dbp >= 13584u && m_transfer.bitbltbuf.dbp < 13600u && m_loggedTransfers < 400u)
            {
                std::fprintf(stderr, "[gs-hw]   data %u bytes: %02x%02x%02x%02x %02x%02x%02x%02x\n", sizeBytes, data[0], data[1], data[2],
                             data[3], data[4], data[5], data[6], data[7]);
                static int dumpedCluts = 0;
                if (sizeBytes == 1024u && dumpedCluts < 8)
                {
                    char name[32];
                    std::snprintf(name, sizeof(name), "hw_clut%d.bin", dumpedCluts++);
                    if (FILE *f = std::fopen(name, "wb"))
                    {
                        std::fwrite(data, 1, sizeBytes, f);
                        std::fclose(f);
                    }
                }
            }
            if (debug && sizeBytes >= 65536u)
            {
                uint64_t h = 1469598103934665603ull;
                for (uint32_t i = 0; i < sizeBytes; ++i)
                    h = (h ^ data[i]) * 1099511628211ull;
                std::fprintf(stderr, "[gs-hw] upload %u bytes dpsm=%u fnv=%016llx first=%02x%02x%02x%02x%02x%02x%02x%02x\n", sizeBytes,
                             m_transfer.bitbltbuf.dpsm, static_cast<unsigned long long>(h), data[0], data[1], data[2], data[3], data[4],
                             data[5], data[6], data[7]);
                static int dumped = 0;
                if (dumped < 8 && sizeBytes == 262144u)
                {
                    char name[32];
                    std::snprintf(name, sizeof(name), "hw_upload%d.bin", dumped++);
                    if (FILE *f = std::fopen(name, "wb"))
                    {
                        std::fwrite(data, 1, sizeBytes, f);
                        std::fclose(f);
                    }
                }
            }
            // [gta-recomp] No data: VCS's upload header packet ends with the IMAGE tag and the pixels follow in the next DIRECT
            // (PS2Memory::submitVif1DirectData), so the front end hands over 0 bytes first. Nothing is written - and nothing
            // queued may be forced out or have its page generations bumped for it (it was ~240k forced writes / 5 s driving).
            if (sizeBytes == 0u)
                return;
            // [gta-recomp] the raster this upload sends (gsHwSetUploadSource), when its size says it is that one
            const uint32_t raster = t_uploadSourceBytes == sizeBytes ? t_uploadSourceAddress : 0u;
            t_uploadSourceBytes = 0u;
            // [gta-recomp] Games re-send the same textures every frame. A whole-transfer upload identical to the last
            // one to the same place, with nothing else written to those pages since, changes nothing: skip the
            // swizzled write and keep the page generations (so cached GPU textures stay valid).
            const GSTransferCommand &t = m_transfer;
            uint32_t first = 0, last = 0;
            pageRange(t.bitbltbuf.dbp, t.bitbltbuf.dbw, t.bitbltbuf.dpsm, t.trxpos.dsax, t.trxpos.dsay, t.trxreg.rrw, t.trxreg.rrh,
                      first, last);
            const uint64_t bitsPerPixel = transferBitsPerPixel(t.bitbltbuf.dpsm);
            const bool wholeTransfer = bitsPerPixel != 0u && last <= kPages &&
                                       static_cast<uint64_t>(t.trxreg.rrw) * t.trxreg.rrh * bitsPerPixel / 8u == sizeBytes;
            uint64_t uploadKey = 0, dataHash = 0;
            { // PS2X_HW_UPSTATS=1: upload kinds every 5 s
                static const bool stats = std::getenv("PS2X_HW_UPSTATS") != nullptr;
                static uint64_t whole = 0, partial = 0, bytesWhole = 0, bytesPartial = 0;
                static auto last = std::chrono::steady_clock::now();
                if (stats)
                {
                    (wholeTransfer ? whole : partial) += 1;
                    (wholeTransfer ? bytesWhole : bytesPartial) += sizeBytes;
                    if (!wholeTransfer && partial < 20)
                        std::fprintf(stderr, "[upload] partial: %u bytes, rect %ux%u psm %u (expects %llu)\n", sizeBytes, t.trxreg.rrw,
                                     t.trxreg.rrh, t.bitbltbuf.dpsm,
                                     static_cast<unsigned long long>(uint64_t(t.trxreg.rrw) * t.trxreg.rrh * bitsPerPixel / 8u));
                    const auto now = std::chrono::steady_clock::now();
                    if (now - last > std::chrono::seconds(5))
                    {
                        last = now;
                        std::fprintf(stderr, "[upload] whole %llu (%llu KB) partial %llu (%llu KB) skipped %llu\n",
                                     (unsigned long long)whole, (unsigned long long)(bytesWhole >> 10), (unsigned long long)partial,
                                     (unsigned long long)(bytesPartial >> 10), (unsigned long long)m_skippedUploads);
                        whole = partial = bytesWhole = bytesPartial = 0;
                        m_skippedUploads = 0;
                    }
                }
            }
            if (wholeTransfer)
            {
                uploadKey = mix64(mix64(0x51ull, t.bitbltbuf.dbp | (uint64_t(t.bitbltbuf.dbw) << 16) | (uint64_t(t.bitbltbuf.dpsm) << 24) |
                                                     (uint64_t(t.trxpos.dsax) << 32) | (uint64_t(t.trxpos.dsay) << 48)),
                                  t.trxreg.rrw | (uint64_t(t.trxreg.rrh) << 16) | (uint64_t(sizeBytes) << 32));
                dataHash = hashBytes(data, sizeBytes);
                auto it = m_uploads.find(uploadKey);
                if (it != m_uploads.end() && it->second.hash == dataHash && it->second.first == first && it->second.gens.size() == last - first)
                {
                    bool untouched = true;
                    for (uint32_t p = first; p < last && untouched; ++p)
                        untouched = m_pageGen[p] == it->second.gens[p - first];
                    if (untouched)
                    {
                        ++m_skippedUploads;
                        return;
                    }
                }
            }
            const uint32_t dpsm = t.bitbltbuf.dpsm;
            if (wholeTransfer && sizeBytes != 0u && lazyUploadsEnabled() &&
                (dpsm == GS_PSM_T4 || dpsm == GS_PSM_T8 || dpsm == GS_PSM_CT32 || dpsm == GS_PSM_CT16) && !overlapsRenderTarget(first, last))
            {
                m_uploadRaster = raster;
                deferUpload(data, sizeBytes);
            }
            else
            {
                { // PS2X_HW_UPSTATS=1: why an upload is written straight away
                    static const bool stats = std::getenv("PS2X_HW_UPSTATS") != nullptr;
                    if (stats)
                    {
                        static std::map<std::string, uint64_t> why;
                        static auto lastWhy = std::chrono::steady_clock::now();
                        char key[96];
                        std::snprintf(key, sizeof(key), "%s psm %u %ux%u %u bytes%s", !wholeTransfer ? "partial" : "whole", dpsm, t.trxreg.rrw, t.trxreg.rrh, sizeBytes,
                                      wholeTransfer && overlapsRenderTarget(first, last) ? " over a render target" : "");
                        ++why[key];
                        if (std::chrono::steady_clock::now() - lastWhy > std::chrono::seconds(5))
                        {
                            lastWhy = std::chrono::steady_clock::now();
                            std::vector<std::pair<uint64_t, std::string>> top;
                            for (const auto &[k, n] : why)
                                top.push_back({n, k});
                            std::sort(top.rbegin(), top.rend());
                            for (size_t i = 0; i < top.size() && i < 8u; ++i)
                                std::fprintf(stderr, "[upload] direct %llu: %s\n", (unsigned long long)top[i].first, top[i].second.c_str());
                            why.clear();
                        }
                    }
                }
                if (prepareDirectUpload() && ++m_lazyWhy[2])
                    m_cpu.BeginTransfer(m_transfer); // replaying queued uploads replaced the CPU backend's transfer state
                m_cpu.UploadImage(data, sizeBytes);
            }
            touchTransferDest();
            if (wholeTransfer)
            {
                UploadInfo &info = m_uploadAt[t.bitbltbuf.dbp];
                info.hash = dataHash;
                info.raster = raster;
                info.dbw = t.bitbltbuf.dbw;
                info.dpsm = t.bitbltbuf.dpsm;
                info.x = t.trxpos.dsax;
                info.y = t.trxpos.dsay;
                info.w = t.trxreg.rrw;
                info.h = t.trxreg.rrh;
                info.first = first;
                info.gens.assign(m_pageGen + first, m_pageGen + last);
                if (m_uploads.size() > 4096u)
                    m_uploads.clear();
                UploadRecord &rec = m_uploads[uploadKey];
                rec.hash = dataHash;
                rec.first = first;
                rec.gens.assign(m_pageGen + first, m_pageGen + last);
            }
        }

        static uint64_t transferBitsPerPixel(uint32_t psm)
        {
            switch (psm)
            {
            case GS_PSM_CT32: case GS_PSM_Z32: return 32u;
            case GS_PSM_CT24: case GS_PSM_Z24: return 24u;
            case GS_PSM_CT16: case GS_PSM_CT16S: case GS_PSM_Z16: case GS_PSM_Z16S: return 16u;
            case GS_PSM_T8: case GS_PSM_T8H: return 8u;
            case GS_PSM_T4: case GS_PSM_T4HL: case GS_PSM_T4HH: return 4u;
            default: return 0u;
            }
        }

        static uint64_t hashBytes(const uint8_t *data, uint32_t size)
        {
            // [gta-recomp] four independent lanes (one multiply chain was the cost of hashing every streamed upload)
            uint64_t h[4] = {0x9E3779B97F4A7C15ull ^ size, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0x27D4EB2F165667C5ull};
            uint32_t i = 0;
            for (; i + 32u <= size; i += 32u)
                for (int l = 0; l < 4; ++l)
                {
                    uint64_t v;
                    std::memcpy(&v, data + i + l * 8u, 8);
                    h[l] = (h[l] ^ (v * 0xBF58476D1CE4E5B9ull)) * 0x94D049BB133111EBull;
                    h[l] ^= h[l] >> 29;
                }
            for (; i + 8u <= size; i += 8u)
            {
                uint64_t v;
                std::memcpy(&v, data + i, 8);
                h[0] = (h[0] ^ (v * 0xBF58476D1CE4E5B9ull)) * 0x94D049BB133111EBull;
                h[0] ^= h[0] >> 29;
            }
            for (; i < size; ++i)
                h[0] = (h[0] ^ data[i]) * 0x100000001B3ull;
            return mix64(mix64(mix64(h[0], h[1]), h[2]), h[3]);
        }

        // [gta-recomp] queued uploads' data buffers, kept with their capacity (an allocation and a free per streamed
        // texture before)
        std::vector<std::vector<uint8_t>> m_uploadBufferPool;
        void recycleUploadData(std::vector<uint8_t> &data)
        {
            if (m_uploadBufferPool.size() < 256u && data.capacity() <= (1u << 20))
            {
                data.clear();
                m_uploadBufferPool.push_back(std::move(data));
            }
            std::vector<uint8_t>().swap(data);
        }

        void Flush() override { m_cpu.Flush(); }
        void TextureFlush() override { m_cpu.TextureFlush(); }
        void Sync(GSSyncReason reason) override { m_cpu.Sync(reason); }
        PresentationFrame Present(const GSPresentationRequest &request) override { return m_cpu.Present(request); }

        bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override
        {
            materializeAllKeepTransfer();
            HwState s{};
            s.fbp = context.frame.fbp;
            s.fbw = context.frame.fbw;
            s.fpsm = context.frame.psm;
            s.scx0 = context.scissor.x0;
            s.scy0 = context.scissor.y0;
            s.scx1 = context.scissor.x1;
            s.scy1 = context.scissor.y1;
            Frame &f = *m_frame;
            f.cmds.push_back({CmdType::Clear, static_cast<uint32_t>(f.clears.size())});
            f.clears.push_back({s, rgba});
            m_cpu.ClearFramebuffer(context, rgba);
            return true;
        }

        uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override
        {
            materializeAllKeepTransfer();
            return m_cpu.ConsumeLocalToHostBytes(dst, maxBytes);
        }
        uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override
        {
            const_cast<HwBackend *>(this)->materializeAllKeepTransfer();
            return m_cpu.ReadVram(psm, base, bw, x, y);
        }
        void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override
        {
            materializeAllKeepTransfer();
            m_cpu.WriteVram(psm, base, bw, x, y, value);
        }
        void SnapshotVram(std::vector<uint8_t> &out) const override
        {
            const_cast<HwBackend *>(this)->materializeAllKeepTransfer();
            m_cpu.SnapshotVram(out);
        }
        GSTransferSnapshot GetTransferSnapshot() const override
        {
            const_cast<HwBackend *>(this)->materializeAllKeepTransfer();
            return m_cpu.GetTransferSnapshot();
        }

        // ---- [gta-recomp] lazy texture uploads ---------------------------------------------------------------------
        // VCS re-sends its T4/T8 model textures into shared VRAM every frame (~20 MB/s). The GPU side finds them in
        // its content-keyed texture cache, so the swizzled copy in CPU VRAM is only needed when something reads those
        // bytes: a texture decode on a cache miss, a CLUT load, a readback, a local transfer, a clear. Whole T4/T8
        // uploads (not over a render target) are therefore queued and written into CPU VRAM - in order, together with
        // every earlier queued upload overlapping them - only when such a read touches their 256-byte blocks. Blocks,
        // not pages: VCS keeps each texture's CLUT in the same page. A queued upload whose blocks have all been
        // completely overwritten by later uploads is dropped (the steady state: the same slots refilled each frame).
        // PS2X_HW_LAZY_UPLOADS=0 writes every upload immediately (as before).
        static constexpr uint32_t kBlocks = 16384u; // 4 MiB / 256 B

        struct PendingUpload
        {
            GSTransferCommand cmd;
            std::vector<uint8_t> data;
            std::vector<uint16_t> blocks; // touched
            uint32_t live = 0;            // blocks not yet completely overwritten by a later upload
            bool done = false;            // written or dropped
            uint32_t takeStamp = 0;       // == m_wantGeneration: chosen by the current materializeBlocks
            uint32_t raster = 0;          // [gta-recomp] the game's raster it sends (step 3d), 0 unknown
        };
        uint32_t m_uploadRaster = 0; // [gta-recomp] UploadImage -> deferUpload

        static bool lazyUploadsEnabled()
        {
            static const bool on = [] {
                const char *v = std::getenv("PS2X_HW_LAZY_UPLOADS");
                return !(v && v[0] == '0');
            }();
            return on;
        }

        bool overlapsRenderTarget(uint32_t first, uint32_t last) const
        {
            for (const auto &[fbp, rt] : m_rts)
                if (first < fbp + rt.pageRows * rt.fbw && last > fbp)
                    return true;
            return false;
        }

        // Blocks of a rectangle; formats RectBlocks does not know count as their whole pages.
        void rectBlocks(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t w, uint32_t h, std::vector<uint16_t> &touched,
                        std::vector<uint16_t> *full)
        {
            if (GSMem::RectBlocks(psm, bp, bw, x, y, w, h, touched, full))
                return;
            uint32_t first, last;
            pageRange(bp, bw, psm, x, y, w, h, first, last);
            for (uint32_t pg = first; pg < last; ++pg)
                for (uint32_t b = 0; b < 32u; ++b)
                    touched.push_back(static_cast<uint16_t>(((pg * 32u) + b) % kBlocks));
        }

        PendingUpload *pendingById(uint64_t id)
        {
            if (id < m_pendingFrontId || id - m_pendingFrontId >= m_pendingUploads.size())
                return nullptr;
            PendingUpload &u = m_pendingUploads[static_cast<size_t>(id - m_pendingFrontId)];
            return u.done ? nullptr : &u;
        }

        // [gta-recomp] The queued upload that is the latest writer of every one of these blocks, if one is (none: a block
        // was written before the queue, or its latest writers differ)
        const PendingUpload *soleQueuedWriter(const std::vector<uint16_t> &blocks)
        {
            const PendingUpload *source = nullptr;
            for (uint16_t b : blocks)
            {
                const PendingUpload *latest = nullptr;
                uint64_t latestId = 0;
                for (uint64_t id : m_blockUploads[b])
                    if (const PendingUpload *u = pendingById(id); u && (!latest || id > latestId))
                    {
                        latest = u;
                        latestId = id;
                    }
                if (!latest || (source && latest != source))
                    return nullptr;
                source = latest;
            }
            return source;
        }

        void retire(PendingUpload &u)
        {
            u.done = true;
            for (uint16_t b : u.blocks)
                --m_blockPending[b];
            m_pendingBytes -= u.data.size();
            recycleUploadData(u.data);
            while (!m_pendingUploads.empty() && m_pendingUploads.front().done)
            {
                m_pendingUploads.pop_front();
                ++m_pendingFrontId;
            }
        }

        void deferUpload(const uint8_t *data, uint32_t sizeBytes)
        {
            const GSTransferCommand &t = m_transfer;
            m_scratchTouched.clear();
            m_scratchFull.clear();
            rectBlocks(t.bitbltbuf.dpsm, t.bitbltbuf.dbp, t.bitbltbuf.dbw, t.trxpos.dsax, t.trxpos.dsay, t.trxreg.rrw, t.trxreg.rrh,
                       m_scratchTouched, &m_scratchFull);
            // blocks this upload overwrites completely: earlier queued writers of them lose those blocks
            for (uint16_t b : m_scratchFull)
            {
                for (uint64_t id : m_blockWriters[b])
                    if (PendingUpload *old = pendingById(id))
                        if (old->live && --old->live == 0u)
                        {
                            retire(*old);
                            ++m_lazyDropped;
                        }
                m_blockWriters[b].clear();
            }
            PendingUpload pu;
            pu.cmd = t;
            pu.raster = m_uploadRaster;
            if (!m_uploadBufferPool.empty())
            {
                pu.data = std::move(m_uploadBufferPool.back());
                m_uploadBufferPool.pop_back();
            }
            pu.data.assign(data, data + sizeBytes);
            pu.blocks = m_scratchTouched;
            std::sort(pu.blocks.begin(), pu.blocks.end());
            pu.blocks.erase(std::unique(pu.blocks.begin(), pu.blocks.end()), pu.blocks.end());
            pu.live = static_cast<uint32_t>(pu.blocks.size());
            const uint64_t id = m_pendingFrontId + m_pendingUploads.size();
            for (uint16_t b : pu.blocks)
            {
                ++m_blockPending[b];
                m_blockWriters[b].push_back(id);
                std::vector<uint64_t> &uploads = m_blockUploads[b];
                if (uploads.size() >= 64u) // ids of written / dropped uploads leave when met; blocks nobody reads, here
                    uploads.erase(std::remove_if(uploads.begin(), uploads.end(), [this](uint64_t old) { return !pendingById(old); }),
                                  uploads.end());
                uploads.push_back(id);
            }
            m_pendingBytes += sizeBytes;
            m_pendingUploads.push_back(std::move(pu));
            ++m_lazyDeferred;
            if (m_pendingUploads.size() > 16384u || m_pendingBytes > (64u << 20))
                materializeAll(); // bounded memory; deferring carries on afterwards
            reportLazyStats();
        }

        // Writes the queued uploads touching `blocks` - and, to keep the order, every earlier queued upload overlapping
        // those - into CPU VRAM. True if anything was written.
        bool materializeBlocks(const std::vector<uint16_t> &blocks, bool all = false)
        {
            if (m_pendingBytes == 0u && m_pendingUploads.empty())
                return false;
            bool any = all;
            for (size_t i = 0; i < blocks.size() && !any; ++i)
                any = m_blockPending[blocks[i]] != 0u;
            if (!any)
                return false;
            // wanted blocks: m_wantStamp[b] == stamp (no 16 KB clear per call)
            if (++m_wantGeneration == 0u)
            {
                std::fill(m_wantStamp.begin(), m_wantStamp.end(), 0u);
                for (PendingUpload &u : m_pendingUploads)
                    u.takeStamp = 0u;
                m_wantGeneration = 1u;
            }
            const uint32_t stamp = m_wantGeneration;
            // The queued uploads to write: those touching a wanted block, where the blocks of every upload taken are
            // wanted too (closure), written in queue order. PS2X_HW_LAZY_INDEX=0: found by rescanning the whole queue
            // until nothing changes (the original; quadratic in the queue, 8% of the game thread when driving)
            static const bool indexed = [] {
                const char *v = std::getenv("PS2X_HW_LAZY_INDEX");
                return !(v && *v == '0');
            }();
            std::vector<size_t> &takeList = m_scratchTakeList;
            takeList.clear();
            if (all)
            {
                for (size_t i = 0; i < m_pendingUploads.size(); ++i)
                    if (!m_pendingUploads[i].done)
                        takeList.push_back(i);
            }
            else if (indexed)
            {
                std::vector<uint16_t> &work = m_scratchWork;
                work.clear();
                auto want = [&](uint16_t b) {
                    if (m_wantStamp[b] == stamp)
                        return;
                    m_wantStamp[b] = stamp;
                    if (m_blockPending[b])
                        work.push_back(b);
                };
                for (uint16_t b : blocks)
                    want(b);
                while (!work.empty())
                {
                    const uint16_t b = work.back();
                    work.pop_back();
                    std::vector<uint64_t> &ids = m_blockUploads[b];
                    size_t kept = 0;
                    for (size_t k = 0; k < ids.size(); ++k)
                    {
                        PendingUpload *u = pendingById(ids[k]);
                        if (!u)
                            continue; // written or dropped: pruned
                        ids[kept++] = ids[k];
                        if (u->takeStamp == stamp)
                            continue;
                        u->takeStamp = stamp;
                        takeList.push_back(static_cast<size_t>(ids[k] - m_pendingFrontId));
                        for (uint16_t ub : u->blocks)
                            want(ub);
                    }
                    ids.resize(kept);
                }
                std::sort(takeList.begin(), takeList.end());
            }
            else
            {
                auto wanted = [&](uint16_t b) { return m_wantStamp[b] == stamp; };
                for (uint16_t b : blocks)
                    m_wantStamp[b] = stamp;
                std::vector<uint8_t> &take = m_scratchTake;
                take.assign(m_pendingUploads.size(), 0u);
                for (bool changed = true; changed;)
                {
                    changed = false;
                    for (size_t i = 0; i < m_pendingUploads.size(); ++i)
                    {
                        const PendingUpload &u = m_pendingUploads[i];
                        if (take[i] || u.done)
                            continue;
                        bool hit = false;
                        for (size_t k = 0; k < u.blocks.size() && !hit; ++k)
                            hit = wanted(u.blocks[k]);
                        if (!hit)
                            continue;
                        take[i] = 1u;
                        changed = true;
                        for (uint16_t b : u.blocks)
                            m_wantStamp[b] = stamp;
                    }
                }
                for (size_t i = 0; i < take.size(); ++i)
                    if (take[i])
                        takeList.push_back(i);
            }
            bool wrote = false;
            for (size_t i : takeList)
            {
                PendingUpload &u = m_pendingUploads[i];
                m_cpu.BeginTransfer(u.cmd);
                m_cpu.UploadImage(u.data.data(), static_cast<uint32_t>(u.data.size()));
                u.done = true;
                for (uint16_t b : u.blocks)
                    --m_blockPending[b];
                m_pendingBytes -= u.data.size();
                recycleUploadData(u.data);
                ++m_lazyWritten;
                wrote = true;
            }
            while (!m_pendingUploads.empty() && m_pendingUploads.front().done)
            {
                m_pendingUploads.pop_front();
                ++m_pendingFrontId;
            }
            return wrote;
        }

        // Before an upload that is written straight away: queued uploads it overwrites completely are dropped; queued
        // uploads it only partly overwrites, or that keep other live blocks, are written first (in order), so a later
        // write of theirs cannot land on top of this one. True if anything was written.
        bool prepareDirectUpload()
        {
            if (m_pendingUploads.empty())
                return false;
            const GSTransferCommand &t = m_transfer;
            m_scratchTouched.clear();
            m_scratchFull.clear();
            rectBlocks(t.bitbltbuf.dpsm, t.bitbltbuf.dbp, t.bitbltbuf.dbw, t.trxpos.dsax, t.trxpos.dsay, t.trxreg.rrw, t.trxreg.rrh,
                       m_scratchTouched, &m_scratchFull);
            for (uint16_t b : m_scratchFull)
            {
                if (!m_blockPending[b])
                    continue;
                for (uint64_t id : m_blockWriters[b])
                    if (PendingUpload *old = pendingById(id))
                        if (old->live && --old->live == 0u)
                        {
                            retire(*old);
                            ++m_lazyDropped;
                        }
                m_blockWriters[b].clear();
            }
            // whatever is still queued on these blocks (partly covered, or alive elsewhere) goes out first
            return materializeBlocks(m_scratchTouched);
        }

        bool materializeRect(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
        {
            if (m_pendingUploads.empty())
                return false;
            m_scratchRead.clear();
            rectBlocks(psm, bp, bw, x, y, w, h, m_scratchRead, nullptr);
            return materializeBlocks(m_scratchRead);
        }

        bool materializeAll()
        {
            if (m_pendingUploads.empty())
                return false;
            static const std::vector<uint16_t> none;
            return materializeBlocks(none, true);
        }

        // Outside an upload: afterwards the CPU backend's transfer state is put back to the current transfer (an
        // upload that has not received its data yet - the usual case between GIF packets).
        bool materializeRectKeepTransfer(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
        {
            if (!materializeRect(psm, bp, bw, x, y, w, h))
                return false;
            if (m_transfer.direction == 0u)
                m_cpu.BeginTransfer(m_transfer);
            return true;
        }
        void materializeAllKeepTransfer()
        {
            if (materializeAll())
            {
                ++m_lazyWhy[3];
                if (m_transfer.direction == 0u)
                    m_cpu.BeginTransfer(m_transfer);
            }
        }

        void reportLazyStats()
        {
            static const bool stats = std::getenv("PS2X_HW_UPSTATS") != nullptr;
            static auto lastReport = std::chrono::steady_clock::now();
            if (!stats || std::chrono::steady_clock::now() - lastReport < std::chrono::seconds(5))
                return;
            lastReport = std::chrono::steady_clock::now();
            std::fprintf(stderr, "[upload] lazy: deferred %llu, written later %llu, made obsolete %llu, queued now %zu (%zu KB); forced by clut %llu, decode %llu, direct upload %llu, other %llu\n",
                         (unsigned long long)m_lazyDeferred, (unsigned long long)m_lazyWritten, (unsigned long long)m_lazyDropped,
                         m_pendingUploads.size(), m_pendingBytes >> 10, (unsigned long long)m_lazyWhy[0], (unsigned long long)m_lazyWhy[1],
                         (unsigned long long)m_lazyWhy[2], (unsigned long long)m_lazyWhy[3]);
            std::fprintf(stderr, "[upload] clut loads from the queued upload: %llu; textures decoded from it: %llu (%llu in another format, %llu straight from its pixels); from VRAM: %llu\n",
                         (unsigned long long)m_clutFromUpload, (unsigned long long)m_decodeFromUpload, (unsigned long long)m_decodeCrossFormat,
                         (unsigned long long)m_decodeFast, (unsigned long long)m_decodeFromVram);
            std::fprintf(stderr, "[upload] textures made: %llu named by their raster, %llu not (%zu rasters known)\n",
                         (unsigned long long)m_texNamed, (unsigned long long)m_texUnnamed, g_rasterTextures.size());
            std::fprintf(stderr, "[upload] texture binds: %llu, %llu as the last one, %llu looked up (%llu found cached)\n",
                         (unsigned long long)m_bindCalls, (unsigned long long)m_bindRepeats, (unsigned long long)m_bindLookups,
                         (unsigned long long)m_bindLookupHits);
            std::fprintf(stderr, "[upload] texture binds from the memo: %llu\n", (unsigned long long)m_bindMemoHits);
            m_bindMemoHits = 0;
            m_texNamed = m_texUnnamed = 0;
            m_bindCalls = m_bindRepeats = m_bindLookups = m_bindLookupHits = 0;
            m_clutFromUpload = m_decodeFromUpload = m_decodeCrossFormat = m_decodeFromVram = m_decodeFast = 0;
            m_lazyDeferred = m_lazyWritten = m_lazyDropped = 0;
            for (uint64_t &w : m_lazyWhy)
                w = 0;
        }

        // ---- frame hand-over (game thread) -------------------------------------------------------------------
        // [gta-recomp] gsHwSetTextureReplacements: the old pack's textures go - TexDeletes in this frame's commands, after
        // the draws already recorded with them -, its lookups and the bind memos start again, and the new pack (or none)
        // takes over: the game's textures, or the new pack's, come back as they are bound.
        void switchTexturePack(uint64_t generation)
        {
            std::shared_ptr<render::TextureReplacements> pack;
            {
                std::lock_guard<std::mutex> lock(g_packMutex);
                pack = g_pack;
            }
            m_packGeneration = generation;
            Frame &f = *m_frame;
            size_t dropped = 0;
            for (auto it = m_texLru.begin(); it != m_texLru.end();)
            {
                if (!render::TextureReplacements::isKey(it->first))
                {
                    ++it;
                    continue;
                }
                f.cmds.push_back({CmdType::TexDelete, static_cast<uint32_t>(f.texDeletes.size())});
                f.texDeletes.push_back(it->first);
                it = m_texLru.erase(it);
                ++dropped;
            }
            for (auto it = g_rasterTextures.begin(); it != g_rasterTextures.end();)
                it = render::TextureReplacements::isKey(it->second) ? g_rasterTextures.erase(it) : std::next(it);
            m_replacementAliases.clear();
            m_replacementSizes.clear();
            m_replacementBytes = 0;
            ++m_texStateVersion;
            ++m_texEvictions;
            m_bindCacheValid = false;
            m_replacements = pack ? std::move(pack)
                                  : std::make_shared<render::TextureReplacements>(std::vector<render::TextureReplacements::Source>{});
            m_replacements->setBlockCompression(m_blockCompression);
            std::fprintf(stderr, "[textures] pack switched: %zu replacement textures dropped\n", dropped);
        }

        void endFrame(const uint64_t (&privRegs)[15]) override
        {
            finishNativeGroup(); // [gta-recomp] the last native group's mesh (nativeMerge)
            if (const uint64_t generation = g_packGeneration.load(std::memory_order_acquire); generation != m_packGeneration)
                switchTexturePack(generation); // [gta-recomp]
            // [gta-recomp] Publish images in command order. Current draws still name their originals; future binds
            // use the shared replacement key. Invalidate both texture-bind memo paths when images become ready.
            auto ready = m_replacements->takeReady();
            if (!ready.empty())
            {
                for (auto &image : ready)
                {
                    const uint64_t key = render::TextureReplacements::key(image->id);
                    TexCreate t;
                    t.key = key; t.w = image->width; t.h = image->height;
                    const size_t bytes = image->gpuBytes(m_anisotropy != 0); // [gta-recomp] a DDS's own size (block-compressed)
                    // [gta-recomp] a DDS is made by the helper threads (makeTextureEarly's): a burst of pack textures - large
                    // BC7 chains with anisotropic filtering - held the main thread for 4-12 ms (the stutter rig, T10). A PNG
                    // still gets its chain from the GPU there (GenerateMips: the immediate context).
                    if (image->dds())
                        makeReplacementEarly(t, image);
                    t.replacement = std::move(image);
                    m_frame->cmds.push_back({CmdType::TexCreate, uint32_t(m_frame->textures.size())});
                    m_frame->textures.push_back(std::move(t));
                    m_texLru[key] = m_frameIndex;
                    m_replacementBytes += bytes;
                    m_replacementSizes[key] = bytes;
                }
                ++m_texStateVersion; ++m_texEvictions;
                m_bindCacheValid = false;
            }
            if (m_replacementBytes > m_replacements->budget()) evictTextures();
            Frame &f = *m_frame;
            std::memcpy(f.privRegs, privRegs, sizeof(f.privRegs));
            static const bool debug = std::getenv("PS2X_HW_DEBUG") != nullptr;
            if (debug && m_frameIndex >= 100u && m_frameIndex < 130u)
                std::fprintf(stderr, "[gs-hw] vblank %llu: draws=%zu dispfb1=%llx dispfb2=%llx last draw fbp=%u\n",
                             static_cast<unsigned long long>(m_frameIndex), f.draws.size(), static_cast<unsigned long long>(privRegs[7]),
                             static_cast<unsigned long long>(privRegs[9]), f.draws.empty() ? 9999u : f.draws.back().s.fbp);
            if (debug && m_frameIndex >= 100u && m_frameIndex < 104u)
                for (const DrawCmd &dc : f.draws)
                    std::fprintf(stderr, "[gs-hw]   draw fbp=%u fbw=%u psm=%u zbp=%u zte=%u n=%u mode=%u tme=%u rt=%u(%u @%u,%u) tex=%llx %ux%u tfx=%u abe=%u fbmsk=%x sc=%u,%u-%u,%u\n",
                                 dc.s.fbp, dc.s.fbw, dc.s.fpsm, dc.s.zbp, dc.s.zte, dc.count, dc.s.mode, dc.s.tme, dc.s.texIsRt,
                                 dc.s.rtFbp, dc.s.rtOx, dc.s.rtOy, static_cast<unsigned long long>(dc.s.texKey), dc.s.tw, dc.s.th,
                                 dc.s.tfx, dc.s.abe, dc.s.fbmsk, dc.s.scx0, dc.s.scy0, dc.s.scx1, dc.s.scy1);
            // Which buffer is displayed: circuit 1 if enabled, else 2.
            const uint64_t pmode = privRegs[0];
            const bool use1 = (pmode & 1u) != 0u || (pmode & 2u) == 0u;
            const uint64_t dispfb = use1 ? privRegs[7] : privRegs[9];
            const uint64_t display = use1 ? privRegs[8] : privRegs[10];
            const uint32_t fbp = static_cast<uint32_t>(dispfb & 0x1FFu);
            const uint32_t fbw = static_cast<uint32_t>((dispfb >> 9) & 0x3Fu);
            const uint32_t psm = static_cast<uint32_t>((dispfb >> 15) & 0x1Fu);
            const uint32_t dbx = static_cast<uint32_t>((dispfb >> 32) & 0x7FFu);
            const uint32_t dby = static_cast<uint32_t>((dispfb >> 43) & 0x7FFu);
            const uint32_t magh = static_cast<uint32_t>((display >> 23) & 0xFu) + 1u;
            const uint32_t magv = static_cast<uint32_t>((display >> 27) & 0x3u) + 1u;
            uint32_t w = (static_cast<uint32_t>((display >> 32) & 0xFFFu) + 1u) / magh;
            uint32_t h = (static_cast<uint32_t>((display >> 44) & 0x7FFu) + 1u) / magv;
            w = std::clamp<uint32_t>(w, 1u, 1024u);
            h = std::clamp<uint32_t>(h, 1u, 1024u);
            f.displayFbp = fbp;
            f.displayX = dbx;
            f.displayY = dby;
            f.displayW = w;
            f.displayH = h;
            f.displayFromRt = m_rts.count(fbp) != 0;
            if (!f.displayFromRt && display != 0u && fbw != 0u)
            {
                // Not drawn by the GPU this session: show what CPU VRAM holds (uploaded images).
                f.cpuDisplayW = w;
                f.cpuDisplayH = h;
                f.cpuDisplay.resize(static_cast<size_t>(w) * h);
                for (uint32_t y = 0; y < h; ++y)
                    for (uint32_t x = 0; x < w; ++x)
                    {
                        uint32_t v = m_cpu.ReadVram(psm, fbp * 32u, fbw, dbx + x, dby + y);
                        if (psm == GS_PSM_CT16 || psm == GS_PSM_CT16S)
                            v = ((v & 0x1Fu) << 3) | (((v >> 5) & 0x1Fu) << 11) | (((v >> 10) & 0x1Fu) << 19);
                        f.cpuDisplay[static_cast<size_t>(y) * w + x] = v | 0xFF000000u;
                    }
            }
            ps2perf::add(ps2perf::Counter::Draws, f.draws.size() + f.nativeDraws.size()); // [gta-recomp] once per frame
            ps2perf::add(ps2perf::Counter::Vertices, f.vertices.size() + f.nativeVertices);
            ps2perf::add(ps2perf::Counter::TexUploads, f.textures.size());
            ++m_frameIndex;
            shrinkTargets(); // [gta-recomp]
            {
                noteFrameBuild(m_frame->inputNs, steadyNs()); // [gta-recomp] input -> ready (gsHwFrameBuildNs), before any queue wait
                std::unique_lock<std::mutex> lock(m_queueMutex);
                // Don't let the game run more than a few frames ahead of the main thread.
                const size_t depth = frameQueueDepth(); // [gta-recomp] gsHwSetShowEveryFrame
                m_queueCv.wait_for(lock, std::chrono::milliseconds(100), [&] { return m_queue.size() < depth; });
                m_frame->inputNs = PSPadBackend::lastInputNs();
                m_frame->queuedNs = steadyNs();
                m_queue.push_back(std::move(m_frame));
            }
            m_frame = takeFreeFrame();
        }

        // [gta-recomp] Frames are recycled with their vectors' capacity: building each from empty vectors cost
        // reallocation copies on the game thread and page frees on the main thread (~10% of a busy frame).
        std::unique_ptr<Frame> takeFreeFrame()
        {
            std::unique_ptr<Frame> frame;
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                if (!m_freeFrames.empty())
                {
                    frame = std::move(m_freeFrames.back());
                    m_freeFrames.pop_back();
                }
            }
            if (!frame)
                return std::make_unique<Frame>();
            Frame &f = *frame;
            f.cmds.clear();
            f.vertices.clear();
            f.draws.clear();
            f.textures.clear(); // owns pixel vectors; their memory goes with them
            f.texDeletes.clear();
            f.rtUploads.clear();
            f.rtDrops.clear();
            f.clears.clear();
            f.depthFills.clear();
            f.meshes.clear();
            f.meshDeletes.clear();
            f.nativeDraws.clear();
            f.nativeBytes.clear();
            f.nativeVertices = 0;
            f.had3d = false;
            f.wide3d = -1;
            f.shown = -1;
            f.menu = false;
            std::memset(f.privRegs, 0, sizeof(f.privRegs));
            f.cpuDisplay.clear();
            f.cpuDisplayW = f.cpuDisplayH = 0;
            f.displayFromRt = false;
            f.displayFbp = f.displayX = f.displayY = f.displayW = f.displayH = 0;
            return frame;
        }

        // [gta-recomp] packet-level HLE: a draw the GPU transforms (see GSNativeDraw)
        bool AcceptsNative(const GSNativeDraw &draw) const override
        {
            return (draw.kind == GSNativeDraw::VcsWorld || draw.kind == GSNativeDraw::VcsLit) && draw.drawCount && draw.vertexCount &&
                   draw.constantCount <= 124u;
        }

        bool SubmitNative(const GSDrawState &state, const GSNativeDraw &draw) override
        {
            if (!AcceptsNative(draw)) // (the only refusal: the GS stage relies on it)
                return false;
            GSPrimitiveBatch batch{};
            batch.state = state;
            HwState s{};
            if (!buildState(batch, s))
                return true; // ZTST NEVER: nothing drawn
            s.mode = draw.kind == GSNativeDraw::VcsLit ? 4 : 3; // native strip kinds (pipeline / merge key only)
            if (s.zte && s.ztst != 1u && s.zbp != s.fbp)
            {
                DepthInfo &di = m_depthBufs[s.zbp];
                di.zpsm = s.zpsm;
                di.fbw = s.fbw;
                di.height = std::max<uint32_t>(di.height, std::min<uint32_t>(s.scy1 + 1u, 1024u));
            }
            HwVertex extent{};
            extent.y = static_cast<float>(s.scy1); // the scissor bounds what the draw can touch
            noteTarget(s.fbp, s.fbw, s.fpsm, &extent, 1, s.scy1);
            Frame &f = *m_frame;
            NativeDrawCmd nd;
            nd.s = s;
            nd.meshKey = draw.meshKey;
            nd.kind = draw.kind;
            nd.drawCount = draw.drawCount;
            nd.transient = draw.transient;
            std::memcpy(nd.constants, draw.constants, sizeof(float) * draw.constantCount);
            const float zs = (s.zpsm == GS_PSM_Z32) ? (1.0f / 4294967296.0f)
                           : (s.zpsm == GS_PSM_Z16 || s.zpsm == GS_PSM_Z16S) ? (1.0f / 65536.0f)
                                                                             : (1.0f / 16777216.0f);
            nd.constants[124] = state.context.xyoffset.ofx / 16.0f;
            nd.constants[125] = state.context.xyoffset.ofy / 16.0f;
            nd.constants[126] = zs;
            f.nativeVertices += draw.drawCount;
            f.had3d = true;
            ps2hle::g_gsVerticesRecorded += draw.drawCount;
            if (extendNativeGroup(f, nd, draw))
                return true;
            finishNativeGroup();
            // a new group: the draw as it is; while nothing else is recorded after it, the next draws of the same state
            // and constants join it (extendNativeGroup)
            NativeGroup &g = m_group;
            g.frame = &f;
            g.members = 1;
            g.key = draw.meshKey;
            g.stride = draw.vertexStride;
            g.mesh = UINT32_MAX;
            if (draw.transient)
            {
                nd.transientOffset = static_cast<uint32_t>(f.nativeBytes.size());
                const auto *bytes = static_cast<const uint8_t *>(draw.vertices);
                f.nativeBytes.insert(f.nativeBytes.end(), bytes, bytes + size_t(draw.vertexStride) * draw.vertexCount);
            }
            else if (nativeMerge())
            {
                // the mesh is made (or found cached) when the group is finished: its creation goes here, ahead of the draw
                g.mesh = static_cast<uint32_t>(f.meshes.size());
                f.cmds.push_back({CmdType::MeshCreate, g.mesh});
                f.meshes.emplace_back();
                const auto *bytes = static_cast<const uint8_t *>(draw.vertices);
                g.bytes.assign(bytes, bytes + size_t(draw.vertexStride) * draw.vertexCount);
            }
            else
                useMesh(f, draw.meshKey, draw.vertexStride, static_cast<const uint8_t *>(draw.vertices), draw.vertexCount);
            g.cmd = static_cast<uint32_t>(f.cmds.size());
            f.cmds.push_back({CmdType::NativeDraw, static_cast<uint32_t>(f.nativeDraws.size())});
            f.nativeDraws.push_back(nd);
            return true;
        }

        // [gta-recomp] Native draws merged (PERFORMANCE.md 32): consecutive native draws with the same state, kind and
        // constants - the VU1 batches of one mesh of one model, half of all native draws - become one draw, their strips
        // joined by two repeated vertices (degenerate triangles: the rasteriser does not cull, nothing is drawn by them).
        // Each draw went through the whole draw path and a driver call (on AMD's D3D11 driver ~40% of the main thread
        // with VC car mods). The joined mesh is cached by its parts' keys. PS2X_HW_NATIVE_MERGE=0: one draw per batch.
        static bool nativeMerge()
        {
            static const bool on = !(std::getenv("PS2X_HW_NATIVE_MERGE") && *std::getenv("PS2X_HW_NATIVE_MERGE") == '0');
            return on;
        }

        struct NativeGroup
        {
            Frame *frame = nullptr;
            uint32_t cmd = 0;             // the group's NativeDraw in frame->cmds
            uint32_t mesh = UINT32_MAX;   // its MeshCreate in frame->meshes (cached meshes: filled when finished)
            uint32_t members = 0, stride = 0;
            uint64_t key = 0;             // one member: its mesh key; more: theirs mixed
            std::vector<uint8_t> bytes;   // cached meshes: the vertices (one member: all; more: the drawn strips joined)
        } m_group;

        // a mesh used by a draw: cached, or created ahead of it
        void useMesh(Frame &f, uint64_t key, uint32_t stride, const uint8_t *bytes, uint32_t count, uint32_t slot = UINT32_MAX)
        {
            auto lru = m_meshLru.find(key);
            if (lru != m_meshLru.end())
            {
                lru->second = m_frameIndex;
                return;
            }
            MeshCreate mc;
            mc.key = key;
            mc.stride = stride;
            mc.count = count;
            mc.bytes.assign(bytes, bytes + size_t(stride) * count);
            if (slot == UINT32_MAX)
            {
                f.cmds.push_back({CmdType::MeshCreate, static_cast<uint32_t>(f.meshes.size())});
                f.meshes.push_back(std::move(mc));
            }
            else
                f.meshes[slot] = std::move(mc);
            m_meshLru[key] = m_frameIndex;
            if (m_meshLru.size() > kMaxMeshes)
                evictMeshes();
        }

        bool extendNativeGroup(Frame &f, const NativeDrawCmd &nd, const GSNativeDraw &draw)
        {
            NativeGroup &g = m_group;
            if (!nativeMerge() || g.frame != &f || g.cmd + 1u != f.cmds.size() || f.cmds.back().type != CmdType::NativeDraw ||
                g.stride != draw.vertexStride || !draw.drawCount || draw.vertexStride > 64u)
                return false;
            NativeDrawCmd &p = f.nativeDraws[f.cmds.back().index];
            if (p.kind != nd.kind || p.transient != nd.transient || std::memcmp(&p.s, &nd.s, sizeof(HwState)) != 0 ||
                std::memcmp(p.constants, nd.constants, sizeof(nd.constants)) != 0 || !p.drawCount)
                return false;
            const size_t stride = g.stride;
            std::vector<uint8_t> &out = draw.transient ? f.nativeBytes : g.bytes;
            const size_t start = draw.transient ? p.transientOffset : 0u;
            out.resize(start + size_t(p.drawCount) * stride); // the strip drawn so far (a single member: all its vertices)
            const auto *bytes = static_cast<const uint8_t *>(draw.vertices);
            uint8_t lastVertex[64];
            std::memcpy(lastVertex, out.data() + out.size() - stride, stride); // (out may move as it grows)
            out.insert(out.end(), lastVertex, lastVertex + stride);
            out.insert(out.end(), bytes, bytes + stride);
            out.insert(out.end(), bytes, bytes + size_t(draw.drawCount) * stride);
            if (g.members == 1u)
                g.key = mixMeshKey(0x4E4D45524745ull, g.key, p.drawCount);
            g.key = mixMeshKey(g.key, draw.meshKey, draw.drawCount);
            ++g.members;
            p.drawCount += 2u + draw.drawCount;
            return true;
        }

        static uint64_t mixMeshKey(uint64_t a, uint64_t b, uint32_t n)
        {
            uint64_t x = a ^ (b + 0x9E3779B97F4A7C15ull + (a << 6) + (a >> 2)) ^ (uint64_t(n) << 32);
            x ^= x >> 31;
            x *= 0xBF58476D1CE4E5B9ull;
            return x ^ (x >> 29);
        }

        // the open group's mesh: cached, or its creation filled in ahead of its draw
        void finishNativeGroup()
        {
            NativeGroup &g = m_group;
            if (!g.frame || g.frame != m_frame.get())
            {
                g.frame = nullptr;
                return;
            }
            Frame &f = *g.frame;
            if (g.mesh != UINT32_MAX)
            {
                NativeDrawCmd &d = f.nativeDraws[f.cmds[g.cmd].index];
                d.meshKey = g.key;
                useMesh(f, g.key, g.stride, g.bytes.data(), static_cast<uint32_t>(g.bytes.size() / g.stride), g.mesh);
            }
            g.frame = nullptr;
        }

        void evictMeshes()
        {
            std::vector<std::pair<uint64_t, uint64_t>> byAge(m_meshLru.begin(), m_meshLru.end());
            std::sort(byAge.begin(), byAge.end(), [](auto &a, auto &b) { return a.second < b.second; });
            Frame &f = *m_frame;
            for (size_t i = 0; i < byAge.size() / 4; ++i)
            {
                if (byAge[i].second == m_frameIndex)
                    break;
                f.cmds.push_back({CmdType::MeshDelete, static_cast<uint32_t>(f.meshDeletes.size())});
                f.meshDeletes.push_back(byAge[i].first);
                m_meshLru.erase(byAge[i].first);
            }
        }

        GSHwPresenter &presenter() override { return *this; }

        // [gta-recomp] Helper thread count for early texture decoding & mip creation:
        // 1) PS2X_HW_TEXTURE_HELPERS=N (exact number: 0 = disabled, >0 = N threads)
        // 2) PS2X_HW_EARLY_TEXTURES: 0 off, 1 on; unset: auto based on backend (on for D3D11 & D3D12, off for Vulkan)
        // 3) Auto-scaling: according to machine cores (hardware_concurrency), leaving cores for game & render threads.
        static uint32_t computeTextureHelpers(rhi::Backend backend)
        {
            if (const char *th = std::getenv("PS2X_HW_TEXTURE_HELPERS"))
            {
                int n = std::atoi(th);
                return n > 0 ? static_cast<uint32_t>(std::clamp(n, 1, 32)) : 0u;
            }
            static const int early = [] {
                const char *e = std::getenv("PS2X_HW_EARLY_TEXTURES");
                return e && *e ? (*e == '0' ? 0 : 1) : -1;
            }();
            const bool earlyOn = early == 1 || (early < 0 && (backend == rhi::Backend::D3D11 || backend == rhi::Backend::D3D12));
            if (!earlyOn)
                return 0u;
            unsigned int hw = std::thread::hardware_concurrency();
            if (hw <= 2)
                return 1u;
            if (hw <= 4)
                return 2u;
            return std::clamp<uint32_t>(hw / 2, 2u, 8u);
        }

        // ---- GSHwPresenter (main thread) ---------------------------------------------------------------------
        bool executeAndGetDisplay(rhi::Device &device, rhi::Texture *&texture, uint32_t &width, uint32_t &height) override
        {
            if (!m_rhi)
            {
                m_rhi = std::make_unique<RhiExecutor>(device, m_scale, m_msaa, m_anisotropy);
                std::fprintf(stderr, m_rhi->ready() ? "[gs-hw] renderer ready (%s), scale %ux\n" : "[gs-hw] renderer failed (%s)\n",
                             device.adapterName().c_str(), m_scale);
            }
            if (!m_rhi->ready())
                return false;
            m_blockCompression = device.blockCompression(); // [gta-recomp] texture packs' block-compressed DDS
            m_replacements->setBlockCompression(m_blockCompression);
            // [gta-recomp] Texture helper threads: PS2X_HW_TEXTURE_HELPERS=N or PS2X_HW_EARLY_TEXTURES
            // Unset: auto-scaled to machine hardware concurrency (half of CPU cores, clamped to 2..8 on D3D11/D3D12)
            static const uint32_t helperCount = computeTextureHelpers(device.backend());
            m_helperThreads = helperCount;
            if (helperCount > 0 && !m_earlyDevice.load(std::memory_order_relaxed) && device.freeThreadedCreation())
            {
                m_earlyDevice.store(&device, std::memory_order_release);
                static bool logged = false;
                if (!logged)
                {
                    logged = true;
                    std::fprintf(stderr, "[gs-hw] early texture helpers: %u threads (hardware concurrency: %u)\n",
                                 helperCount, std::thread::hardware_concurrency());
                }
            }
            if (m_nativeFrame) // [gta-recomp] the picture's targets follow the window (gsHwSetFrameSize)
            {
                // PS2X_HW_FRAME=WxH (testing): a full-screen window's frame size whatever the window
                static const std::pair<uint32_t, uint32_t> forced = [] {
                    unsigned fw = 0, fh = 0;
                    if (const char *e = std::getenv("PS2X_HW_FRAME"))
                        if (std::sscanf(e, "%ux%u", &fw, &fh) != 2)
                            fw = fh = 0;
                    return std::pair<uint32_t, uint32_t>(fw, fh);
                }();
                if (forced.first)
                    m_rhi->setFrameSize(forced.first, forced.second);
                else
                    m_rhi->setFrameSize(g_frameW.load(std::memory_order_relaxed), g_frameH.load(std::memory_order_relaxed));
            }
            else
            {
                // [gta-recomp] an integer scale in widescreen: the picture's targets as many times wider as the view is
                // (gsHwSetAspectWidening), so the wider picture has the 4:3 picture's pixels per degree across instead
                // of 640 x scale stretched (2.67 times softer across than down at 32:9); at most 8192 wide.
                // PS2X_HW_WIDE_SCALE=0: every target at the plain integer scale (before 2026-10-27)
                static const bool wide = [] {
                    const char *e = std::getenv("PS2X_HW_WIDE_SCALE");
                    return !(e && e[0] == '0');
                }();
                const float r = g_aspectWidening.load(std::memory_order_relaxed);
                if (wide && r > 1.0001f)
                    m_rhi->setFrameSize(std::min<uint32_t>(8192u, static_cast<uint32_t>(std::lround(640.0 * m_scale * r))), 448u * m_scale);
                else
                    m_rhi->setFrameSize(0u, 0u);
            }
            std::deque<std::unique_ptr<Frame>> frames;
            {
                // [gta-recomp] frames per host frame (gsHwSetShowEveryFrame): all queued ones (the last shown), or one
                const size_t batchMax = frameBatchMax();
                std::lock_guard<std::mutex> lock(m_queueMutex);
                if (batchMax && m_queue.size() > batchMax)
                    for (size_t i = 0; i < batchMax; ++i)
                    {
                        frames.push_back(std::move(m_queue.front()));
                        m_queue.pop_front();
                    }
                else
                    frames.swap(m_queue);
            }
            m_queueCv.notify_all();
            ps2NoteFramesTaken(static_cast<uint32_t>(frames.size())); // [gta-recomp] frames on their way (ps2_present.h)
            const int64_t takenNs = steadyNs();
            if (!frames.empty())
            {
                // [gta-recomp] F12 (or PS2X_HW_DUMP=<frame>): list the next frame's draws and dump its targets
                static const long dumpAt = std::getenv("PS2X_HW_DUMP") ? std::atol(std::getenv("PS2X_HW_DUMP")) : -1;
                const bool f12 = ps2control::keyDown(301 /* KEY_F12 */);
                if (f12 && !m_f12Down)
                    m_dumpPending = true;
                m_f12Down = f12;
                if (dumpAt >= 0 && !m_dumped && m_executedFrames + frames.size() >= static_cast<uint64_t>(dumpAt))
                    m_dumpPending = true;
                // the next batch that draws anything (the game draws in every other VBlank at 30 fps)
                const bool dumpNow = m_dumpPending &&
                                     std::any_of(frames.begin(), frames.end(), [](const auto &fr) { return !fr->cmds.empty(); });
                // PS2X_HW_DUMP_BATCHES=<n>: list the draws of n consecutive batches (a game frame's 3D and its
                // post-effect / HUD frame can arrive in different batches); the targets are written after the last
                static const int dumpBatches = std::max(1, std::getenv("PS2X_HW_DUMP_BATCHES") ? std::atoi(std::getenv("PS2X_HW_DUMP_BATCHES")) : 1);
                if (dumpNow)
                {
                    m_dumpPending = false;
                    m_dumpBatchesLeft = dumpBatches;
                }
                const bool logThis = m_dumpBatchesLeft > 0;
                const bool dumpTargets = logThis && m_dumpBatchesLeft == 1;
                for (auto &frame : frames)
                {
                    if (logThis)
                        std::fprintf(stderr, "[draw] --- frame (%zu draws, %zu native, 3d %d) ---\n", frame->draws.size(),
                                     frame->nativeDraws.size(), frame->had3d ? 1 : 0);
                    decideShape(*frame);
                    m_rhi->execute(*frame, logThis);
                }
                if (m_dumpBatchesLeft > 0)
                    --m_dumpBatchesLeft;
                m_rhi->composeDisplay(*frames.back());
                g_shownInputNs.store(frames.back()->inputNs, std::memory_order_relaxed); // [gta-recomp] gsHwShownInputNs
                noteDelayStages(frames.back()->inputNs, frames.back()->queuedNs, takenNs);
                // [gta-recomp] the shape on screen is the displayed picture's own (shapeOf), not the newest frame's
                {
                    const Frame &shown = *frames.back();
                    m_shownWide = shown.displayFromRt ? m_rhi->shapeOf(shown.displayFbp, shown.shown != 0) : shown.shown != 0;
                }
                {
                    // [gta-recomp] lastFrameInfo: per frame of the batch: draws / native draws / clears / rt drops,
                    // 3D, displayed buffer
                    std::string info = "batch " + std::to_string(frames.size()) + ":";
                    for (auto &fr : frames)
                    {
                        char b[192];
                        std::snprintf(b, sizeof(b), " [draws %zu native %zu clears %zu drops %zu 3d %d menu %d wide %d/%d disp fbp %u rt %d]",
                                      fr->draws.size(), fr->nativeDraws.size(), fr->clears.size(), fr->rtDrops.size(),
                                      fr->had3d ? 1 : 0, fr->menu ? 1 : 0, fr->wide3d, fr->shown, fr->displayFbp, fr->displayFromRt ? 1 : 0);
                        info += b;
                    }
                    std::lock_guard<std::mutex> lock(m_infoMutex);
                    m_lastInfo = std::move(info);
                }
                m_executedFrames += frames.size();
                if (dumpTargets)
                {
                    m_dumped = true;
                    m_rhi->dump();
                }
                static const bool debug = std::getenv("PS2X_HW_DEBUG") != nullptr;
                if (debug)
                {
                    static auto since = std::chrono::steady_clock::now();
                    static uint64_t nFrames = 0, nDraws = 0, nVerts = 0, nTex = 0;
                    for (auto &fr : frames)
                    {
                        ++nFrames;
                        nDraws += fr->draws.size();
                        nVerts += fr->vertices.size();
                        nTex += fr->textures.size();
                    }
                    const auto now = std::chrono::steady_clock::now();
                    if (std::chrono::duration<double>(now - since).count() >= 1.0)
                    {
                        const Frame &l = *frames.back();
                        std::fprintf(stderr, "[gs-hw] %llu frames, %llu draws, %llu verts, %llu tex | display fbp=%u %ux%u at %u,%u fromRt=%d cpu=%zu | rts:%s",
                                     (unsigned long long)nFrames, (unsigned long long)nDraws, (unsigned long long)nVerts,
                                     (unsigned long long)nTex, l.displayFbp, l.displayW, l.displayH, l.displayX, l.displayY,
                                     l.displayFromRt ? 1 : 0, l.cpuDisplay.size(), m_rhi->describeTargets().c_str());
                        std::fprintf(stderr, " pmode=%llx dispfb1=%llx display1=%llx dispfb2=%llx display2=%llx\n",
                                     (unsigned long long)l.privRegs[0], (unsigned long long)l.privRegs[7], (unsigned long long)l.privRegs[8],
                                     (unsigned long long)l.privRegs[9], (unsigned long long)l.privRegs[10]);
                        nFrames = nDraws = nVerts = nTex = 0;
                        since = now;
                    }
                }
                std::lock_guard<std::mutex> lock(m_queueMutex);
                for (auto &frame : frames)
                    if (m_freeFrames.size() < 6u)
                        m_freeFrames.push_back(std::move(frame));
            }
            texture = m_rhi->display();
            if (!texture)
                return false;
            width = m_rhi->displayWidth();
            height = m_rhi->displayHeight();
            return true;
        }

    private:
        // ---- recording helpers (game thread) -----------------------------------------------------------------
        bool buildState(const GSPrimitiveBatch &batch, HwState &s)
        {
            const GSDrawState &d = batch.state;
            const GSContext &c = d.context;
            s.fbp = c.frame.fbp;
            s.fbw = std::max<uint32_t>(1u, c.frame.fbw);
            s.fpsm = c.frame.psm;
            s.fbmsk = c.frame.fbmsk;
            s.zbp = c.zbuf.zbp;
            s.zpsm = c.zbuf.psm;
            s.zmsk = c.zbuf.zmask ? 1u : 0u;
            s.scx0 = c.scissor.x0;
            s.scy0 = c.scissor.y0;
            s.scx1 = c.scissor.x1;
            s.scy1 = c.scissor.y1;
            const uint64_t test = c.test;
            s.ate = test & 1u;
            s.atst = (test >> 1) & 7u;
            s.aref = (test >> 4) & 0xFFu;
            s.afail = (test >> 12) & 3u;
            s.zte = (test >> 16) & 1u;
            s.ztst = (test >> 17) & 3u;
            if (s.zte && s.ztst == 0u)
                return false; // ZTST NEVER
            s.abe = d.prim.abe ? 1u : 0u;
            const uint64_t a = c.alpha;
            s.ba = a & 3u;
            s.bb = (a >> 2) & 3u;
            s.bc = (a >> 4) & 3u;
            s.bd = (a >> 6) & 3u;
            s.fix = (a >> 32) & 0xFFu;
            s.iip = d.prim.iip ? 1u : 0u;
            s.fge = d.prim.fge ? 1u : 0u;
            s.fogR = d.fogR;
            s.fogG = d.fogG;
            s.fogB = d.fogB;
            s.tme = d.prim.tme ? 1u : 0u;
            if (s.tme)
            {
                const GSTex0Reg &t = c.tex0;
                s.tfx = t.tfx;
                s.tcc = t.tcc;
                s.fst = d.prim.fst ? 1u : 0u;
                s.linear = d.linearFilter ? 1u : 0u;
                s.tw = d.textureWidth;
                s.th = d.textureHeight;
                const uint64_t cl = c.clamp;
                s.wms = cl & 3u;
                s.wmt = (cl >> 2) & 3u;
                s.minu = (cl >> 4) & 0x3FFu;
                s.maxu = (cl >> 14) & 0x3FFu;
                s.minv = (cl >> 24) & 0x3FFu;
                s.maxv = (cl >> 34) & 0x3FFu;
                if (!bindTexture(d, s))
                    s.tme = 0u;
            }
            else
            {
                s.fst = d.prim.fst ? 1u : 0u;
            }
            return true;
        }

        // Texture source: a render target (GPU) or decoded CPU VRAM (cached).
        bool bindTexture(const GSDrawState &d, HwState &s)
        {
            // [gta-recomp] Consecutive primitives mostly use the same texture: reuse the last binding while the texture
            // registers and everything the key depends on (page generations, CLUT, render targets) are unchanged.
            // Recomputing it per primitive (page-generation hash, CLUT hash, cache lookup) was ~4% of a busy frame.
            const GSTex0Reg &t = d.context.tex0;
            TexBindInput in;
            std::memset(&in, 0, sizeof(in)); // compared bytewise, padding included
            in.version = m_texStateVersion;
            in.tbp0 = t.tbp0;
            in.cbp = t.cbp;
            in.tbw = t.tbw;
            in.psm = t.psm;
            in.cpsm = t.cpsm;
            in.csm = t.csm;
            in.csa = t.csa;
            in.ta0 = d.texa.ta0;
            in.ta1 = d.texa.ta1;
            in.aem = d.texa.aem ? 1u : 0u;
            in.w = d.textureWidth;
            in.h = d.textureHeight;
            ++m_bindCalls;
            if (m_bindCacheValid && std::memcmp(&in, &m_bindIn, sizeof(in)) == 0)
            {
                ++m_bindRepeats;
                s.texIsRt = m_bindOut.texIsRt;
                s.rtFbp = m_bindOut.rtFbp;
                s.rtOx = m_bindOut.rtOx;
                s.rtOy = m_bindOut.rtOy;
                s.texKey = m_bindOut.texKey;
                if (!s.texIsRt && m_bindTouchedFrame != m_frameIndex)
                {
                    m_texLru[s.texKey] = m_frameIndex;
                    m_bindTouchedFrame = m_frameIndex;
                }
                return m_bindResult;
            }
            // [gta-recomp] Second level: recent bindings by their registers (the version left out), each valid while what
            // its result depends on is unchanged - the texture's VRAM page generations, the CLUT (paletted), the render
            // targets, the texture cache's evictions. VCS reloads a CLUT or uploads a texture before most draws, so the
            // version above changes all the time; ~840 binds a frame found their texture again through the full
            // lookup (four hash maps) at a busy city spot.
            TexBindInput memoIn = in;
            memoIn.version = 0;
            const bool paletted = t.psm == GS_PSM_T4 || t.psm == GS_PSM_T8 || t.psm == GS_PSM_T8H || t.psm == GS_PSM_T4HL ||
                                  t.psm == GS_PSM_T4HH;
            uint32_t first = 0, last = 0;
            pageRange(t.tbp0, t.tbw, t.psm, 0, 0, d.textureWidth, d.textureHeight, first, last);
            uint64_t deps = mix64(m_rtVersion, m_texEvictions);
            if (paletted)
                deps = mix64(deps, m_cpu.ClutHash(t));
            for (uint32_t p = first; p < last && p < kPages; ++p)
                deps = mix64(deps, m_pageGen[p]);
            uint64_t memoHash = 0x2545F4914F6CDD1Dull;
            {
                uint64_t words[4] = {};
                static_assert(sizeof(TexBindInput) <= sizeof(words));
                std::memcpy(words, &memoIn, sizeof(memoIn));
                for (uint64_t w : words)
                    memoHash = mix64(memoHash, w);
            }
            BindMemo &memo = m_bindMemo[memoHash & (kBindMemoSize - 1u)];
            if (memo.valid && memo.deps == deps && std::memcmp(&memo.in, &memoIn, sizeof(memoIn)) == 0)
            {
                ++m_bindMemoHits;
                s.texIsRt = memo.texIsRt;
                s.rtFbp = memo.rtFbp;
                s.rtOx = memo.rtOx;
                s.rtOy = memo.rtOy;
                s.texKey = memo.texKey;
                if (!s.texIsRt && memo.touchedFrame != m_frameIndex)
                    m_texLru[s.texKey] = m_frameIndex;
                memo.touchedFrame = m_frameIndex;
                m_bindIn = in;
                m_bindOut = s;
                m_bindResult = memo.result;
                m_bindTouchedFrame = m_frameIndex;
                m_bindCacheValid = true;
                return memo.result;
            }
            const bool result = bindTextureUncached(d, s);
            // the deps again: binding can create a texture (and evict others) or note a render target
            uint64_t after = mix64(m_rtVersion, m_texEvictions);
            if (paletted)
                after = mix64(after, m_cpu.ClutHash(t));
            for (uint32_t p = first; p < last && p < kPages; ++p)
                after = mix64(after, m_pageGen[p]);
            memo.valid = true;
            memo.in = memoIn;
            memo.deps = after;
            memo.texIsRt = s.texIsRt;
            memo.rtFbp = s.rtFbp;
            memo.rtOx = s.rtOx;
            memo.rtOy = s.rtOy;
            memo.texKey = s.texKey;
            memo.result = result;
            memo.touchedFrame = m_frameIndex;
            m_bindIn = in;
            m_bindOut = s;
            m_bindResult = result;
            m_bindTouchedFrame = m_frameIndex;
            m_bindCacheValid = true;
            return result;
        }

        // [gta-recomp] PCSX2 HashTextureLevel: raw GS blocks in texture block order; sub-block textures and formats
        // occupying only part of a word are hashed as expanded indices/colours. Hash only on cache misses and only
        // for a format/size present in the pack. The original cache key remains independent of PCSX2's identity.
        bool appendReplacementHashLevel(const GSDrawState &d)
        {
            const auto &t = d.context.tex0;
            uint32_t bw = 8, bh = 8;
            switch (t.psm)
            {
            case GS_PSM_T4: bw = 32; bh = 16; break;
            case GS_PSM_T8: bw = 16; bh = 16; break;
            case GS_PSM_CT16: case GS_PSM_CT16S: bw = 16; break;
            case GS_PSM_CT32: case GS_PSM_CT24: case GS_PSM_T8H: case GS_PSM_T4HL: case GS_PSM_T4HH: break;
            default: return false;
            }
            materializeRectKeepTransfer(t.psm, t.tbp0, t.tbw, 0, 0, d.textureWidth, d.textureHeight);
            const bool indexed = t.psm == GS_PSM_T4 || t.psm == GS_PSM_T8 || t.psm == GS_PSM_T8H ||
                                 t.psm == GS_PSM_T4HL || t.psm == GS_PSM_T4HH;
            if (d.textureWidth < bw || d.textureHeight < bh || t.psm == GS_PSM_CT24 ||
                t.psm == GS_PSM_CT16 || t.psm == GS_PSM_CT16S ||
                t.psm == GS_PSM_T8H || t.psm == GS_PSM_T4HL || t.psm == GS_PSM_T4HH)
            {
                if (indexed)
                    for (uint32_t y = 0; y < d.textureHeight; ++y)
                        for (uint32_t x = 0; x < d.textureWidth; ++x)
                            m_replacementHashBytes.push_back(uint8_t(m_cpu.ReadVram(t.psm, t.tbp0, t.tbw, x, y)));
                else
                {
                    m_cpu.DecodeTexture(d, m_replacementScratch);
                    const auto *p = reinterpret_cast<const uint8_t *>(m_replacementScratch.data());
                    m_replacementHashBytes.insert(m_replacementHashBytes.end(), p, p + m_replacementScratch.size() * 4);
                }
                return true;
            }
            for (uint32_t y = 0; y < d.textureHeight; y += bh)
                for (uint32_t x = 0; x < d.textureWidth; x += bw)
                {
                    uint32_t bit = 0, width = 0;
                    if (!GSMem::PixelBits(t.psm, t.tbp0, t.tbw, x, y, bit, width)) return false;
                    const auto *p = m_textureVram + ((bit >> 11) * 256u);
                    m_replacementHashBytes.insert(m_replacementHashBytes.end(), p, p + 256);
                }
            return true;
        }

        // [gta-recomp] PCSX2's format bits of a texture (its file name's last field)
        static uint32_t replacementBits(const GSDrawState &d)
        {
            const auto &t = d.context.tex0;
            uint32_t bits = t.psm | (uint32_t(t.tw) << 6) | (uint32_t(t.th) << 10);
            if (t.psm == GS_PSM_CT24 || t.psm == GS_PSM_CT16 || t.psm == GS_PSM_CT16S)
                bits |= (uint32_t(d.texa.ta0) << 15) | (uint32_t(d.texa.aem) << 23) | (uint32_t(d.texa.ta1) << 24);
            return bits;
        }

        // [gta-recomp] PCSX2's palette hash: the loaded CLUT expanded (indexed formats; 0 for the others)
        uint64_t replacementPalette(const GSDrawState &d)
        {
            const auto &t = d.context.tex0;
            const bool four = t.psm == GS_PSM_T4 || t.psm == GS_PSM_T4HL || t.psm == GS_PSM_T4HH;
            if (!four && t.psm != GS_PSM_T8 && t.psm != GS_PSM_T8H)
                return 0;
            std::vector<uint32_t> indices(four ? 16 : 256);
            for (uint32_t i = 0; i < indices.size(); ++i) indices[i] = i;
            m_cpu.DecodeTextureFromTexels(d, indices, m_replacementScratch);
            return render::textureHash(m_replacementScratch.data(), m_replacementScratch.size() * 4);
        }

        // [gta-recomp] VCS_TEXTURE_DUMP=1 (docs/TEXTURE_PACKS.md): a texture the game decodes, once per name, as an
        // uncompressed DDS (RGBA8, the GS's alpha: 128 = opaque) under PCSX2's base-level name in the dump folder - what a
        // pack is made from (convert to BC7 / BC1 with texconv or Compressonator, keep the name, put it in replacements/).
        void dumpTexture(const GSDrawState &d, const std::vector<uint32_t> &pixels)
        {
            static const std::string folder = [] {
                const char *dir = std::getenv("VCS_TEXTURE_DUMP_DIR");
                return std::string(dir && *dir ? dir : "textures/SLUS-21590/dumps");
            }();
            const uint32_t w = d.textureWidth, h = d.textureHeight;
            if (!m_textureVram || pixels.size() != size_t(w) * h)
                return;
            const uint32_t bits = replacementBits(d);
            const uint64_t palette = replacementPalette(d);
            m_replacementHashBytes.clear();
            if (!appendReplacementHashLevel(d))
                return;
            const uint64_t hash = render::textureHash(m_replacementHashBytes.data(), m_replacementHashBytes.size());
            char name[64];
            if (palette)
                std::snprintf(name, sizeof(name), "%llx-%llx-%08x.dds", (unsigned long long)hash, (unsigned long long)palette, bits);
            else
                std::snprintf(name, sizeof(name), "%llx-%08x.dds", (unsigned long long)hash, bits);
            if (!m_texturesDumped.insert(name).second)
                return;
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::u8path(folder), ec);
            const std::filesystem::path path = std::filesystem::u8path(folder) / name;
            if (std::filesystem::exists(path, ec))
                return; // from an earlier run
            // DDS_HEADER: caps | height | width | pixel format | pitch; DDPF_RGB | DDPF_ALPHAPIXELS, RGBA masks
            uint32_t header[32] = {0x20534444u, 124u, 0x100Fu, h, w, w * 4u};
            header[19] = 32u;
            header[20] = 0x41u;
            header[22] = 32u;
            header[23] = 0x000000FFu;
            header[24] = 0x0000FF00u;
            header[25] = 0x00FF0000u;
            header[26] = 0xFF000000u;
            header[27] = 0x1000u; // DDSCAPS_TEXTURE
            if (FILE *f = std::fopen(path.string().c_str(), "wb"))
            {
                std::fwrite(header, 4, 32, f);
                std::fwrite(pixels.data(), 4, pixels.size(), f);
                std::fclose(f);
                if (m_texturesDumped.size() <= 20u || m_texturesDumped.size() % 100u == 0u)
                    std::fprintf(stderr, "[textures] dumped %s (%ux%u; %zu so far, %s)\n", name, w, h, m_texturesDumped.size(), folder.c_str());
            }
        }

        uint32_t findReplacement(const GSDrawState &d)
        {
            const uint32_t bits = replacementBits(d);
            if (!m_replacements->candidate(bits) || !m_textureVram) return 0;
            const uint64_t palette = replacementPalette(d);
            m_replacementHashBytes.clear();
            GSDrawState level = d;
            // Packs dumped with mipmapping disabled hash just level 0; enabled packs hash the mip chain too.
            const uint32_t levels = std::min<uint32_t>((d.context.tex1 >> 2) & 7u, 6u);
            for (uint32_t mip = 0; mip <= levels; ++mip)
            {
                if (mip)
                {
                    const uint64_t reg = mip <= 3 ? d.context.miptbp1 : d.context.miptbp2;
                    const uint32_t shift = ((mip - 1) % 3) * 20;
                    level.context.tex0.tbp0 = uint32_t((reg >> shift) & 0x3FFFu);
                    level.context.tex0.tbw = uint8_t((reg >> (shift + 14)) & 0x3Fu);
                    level.textureWidth = std::max<uint16_t>(1, d.textureWidth >> mip);
                    level.textureHeight = std::max<uint16_t>(1, d.textureHeight >> mip);
                }
                if (!appendReplacementHashLevel(level)) return 0;
                if (const uint32_t id = m_replacements->find(render::textureHash(m_replacementHashBytes.data(), m_replacementHashBytes.size()), palette, bits))
                    return id;
            }
            return 0;
        }

        bool bindTextureUncached(const GSDrawState &d, HwState &s)
        {
            const GSTex0Reg &t = d.context.tex0;
            const uint32_t texStart = t.tbp0 * 256u;
            if (t.psm == GS_PSM_CT32 || t.psm == GS_PSM_CT24 || t.psm == GS_PSM_CT16 || t.psm == GS_PSM_CT16S)
            {
                // Targets can overlap in VRAM (a 640x448 buffer's tail is the next buffer's head): use the target
                // whose base is the closest at or below the texture.
                const RtInfo *best = nullptr;
                uint32_t bestFbp = 0;
                for (const auto &[fbp, rt] : m_rts)
                {
                    const uint32_t rtStart = fbp * 8192u;
                    const uint32_t rtEnd = rtStart + rt.pageRows * rt.fbw * 8192u;
                    if (texStart >= rtStart && texStart < rtEnd && (!best || fbp > bestFbp))
                    {
                        best = &rt;
                        bestFbp = fbp;
                    }
                }
                if (best)
                {
                    const uint32_t page = (texStart - bestFbp * 8192u) / 8192u;
                    s.texIsRt = 1u;
                    s.rtFbp = bestFbp;
                    s.rtOx = static_cast<uint16_t>((page % best->fbw) * 64u);
                    s.rtOy = static_cast<uint16_t>((page / best->fbw) * 32u);
                    return true;
                }
            }
            uint32_t first, last;
            pageRange(t.tbp0, t.tbw, t.psm, 0, 0, d.textureWidth, d.textureHeight, first, last);
            uint64_t key = 0x1234567ull;
            key = mix64(key, t.tbp0 | (uint64_t(t.tbw) << 16) | (uint64_t(t.psm) << 24) | (uint64_t(d.textureWidth) << 32) |
                                 (uint64_t(d.textureHeight) << 48));
            key = mix64(key, uint64_t(d.texa.ta0) | (uint64_t(d.texa.ta1) << 8) | (uint64_t(d.texa.aem) << 16));
            const bool paletted = t.psm == GS_PSM_T4 || t.psm == GS_PSM_T8 || t.psm == GS_PSM_T8H || t.psm == GS_PSM_T4HL ||
                                  t.psm == GS_PSM_T4HH;
            if (paletted)
                key = mix64(key, m_cpu.ClutHash(t));
            // [gta-recomp] Texture = an intact whole upload (same base/format/width, covers it, pages untouched since):
            // key it by the upload's content instead of the page generations, so a texture that is streamed into a
            // shared VRAM area every frame finds its decoded GPU copy again.
            bool contentKeyed = false;
            uint32_t raster = 0;
            if (auto up = m_uploadAt.find(t.tbp0); up != m_uploadAt.end())
            {
                const UploadInfo &u = up->second;
                if (u.dpsm == t.psm && u.dbw == t.tbw && u.x == 0u && u.y == 0u && u.w >= d.textureWidth && u.h >= d.textureHeight &&
                    first >= u.first && last <= u.first + u.gens.size())
                {
                    contentKeyed = true;
                    for (uint32_t p = first; p < last && contentKeyed; ++p)
                        contentKeyed = m_pageGen[p] == u.gens[p - u.first];
                    if (contentKeyed)
                    {
                        key = mix64(mix64(key ^ 0xC0FFEEull, u.hash), u.w | (uint64_t(u.h) << 16));
                        raster = u.raster;
                    }
                }
                // [gta-recomp] step 3d: named by the raster of the upload at its base when that upload's pages hold the
                // texture and are untouched since - in any format (VCS sends T4 as CT16 and T8 as CT32, keyed by page
                // generations above)
                else if (u.x == 0u && u.y == 0u && first >= u.first && last <= u.first + u.gens.size())
                {
                    bool untouched = true;
                    for (uint32_t p = first; p < last && untouched; ++p)
                        untouched = m_pageGen[p] == u.gens[p - u.first];
                    if (untouched)
                        raster = u.raster;
                }
            }
            if (!raster && !m_pendingUploads.empty()) // [gta-recomp] step 3d: or the queued upload that wrote all its blocks
            {
                m_scratchRead.clear();
                rectBlocks(t.psm, t.tbp0, t.tbw, 0u, 0u, d.textureWidth, d.textureHeight, m_scratchRead, nullptr);
                if (const PendingUpload *w = soleQueuedWriter(m_scratchRead))
                    raster = w->raster;
            }
            if (!contentKeyed)
                for (uint32_t p = first; p < last && p < kPages; ++p)
                    key = mix64(key, m_pageGen[p]);
            s.texKey = key;
            // [gta-recomp] Cache both matches and misses under the existing content/invalidation key. All aliases of
            // one pack image share a single GPU texture. A streamed-out replacement is requested again on demand.
            if (m_replacements->candidate(t.psm | (uint32_t(t.tw) << 6) | (uint32_t(t.th) << 10)))
            {
                if (m_replacementAliases.size() >= 32768u) m_replacementAliases.clear();
                auto [alias, added] = m_replacementAliases.try_emplace(key, 0);
                if (added) alias->second = findReplacement(d);
                if (alias->second)
                {
                    const uint64_t replacementKey = render::TextureReplacements::key(alias->second);
                    if (auto cached = m_texLru.find(replacementKey); cached != m_texLru.end())
                    {
                        cached->second = m_frameIndex;
                        s.texKey = replacementKey;
                        if (raster) g_rasterTextures[raster] = replacementKey;
                        return true;
                    }
                    m_replacements->request(alias->second);
                }
            }
            if (raster) // [gta-recomp] step 3d: the texture by the raster it came from
                g_rasterTextures[raster] = key;
            ++m_bindLookups;
            auto it = m_texLru.find(key);
            if (it != m_texLru.end())
            {
                ++m_bindLookupHits;
                it->second = m_frameIndex;
                return true;
            }
            ++(raster ? m_texNamed : m_texUnnamed);
            {
                static const bool stats = std::getenv("PS2X_HW_UPSTATS") != nullptr;
                static int logged = 0;
                if (stats && !raster && logged++ < 40)
                {
                    const auto up = m_uploadAt.find(t.tbp0);
                    std::fprintf(stderr, "[upload] texture without a raster: tbp %u tbw %u psm %u %ux%u, %s\n", t.tbp0, t.tbw, t.psm, d.textureWidth,
                                 d.textureHeight, !contentKeyed ? (up == m_uploadAt.end() ? "no whole upload at its base" : "its upload's pages written since or not covering it")
                                                                : "its upload's source unknown");
                }
            }
            static const bool debug = std::getenv("PS2X_HW_DEBUG") != nullptr;
            if (debug && m_loggedTextures < 60u)
            {
                ++m_loggedTextures;
                std::fprintf(stderr, "[gs-hw] texture tbp=%u tbw=%u psm=%u %ux%u cbp=%u cpsm=%u csm=%u csa=%u tfx=%u\n", t.tbp0, t.tbw, t.psm,
                             d.textureWidth, d.textureHeight, t.cbp, t.cpsm, t.csm, t.csa, t.tfx);
            }
            Frame &f = *m_frame;
            TexCreate tc;
            tc.key = key;
            tc.w = d.textureWidth;
            tc.h = d.textureHeight;
            const auto decodeStart = std::chrono::steady_clock::now(); // [gta-recomp] tex_decode_us
            if (!decodeFromQueuedUpload(d, tc.pixels)) // [gta-recomp] step 3c
            {
                if (materializeRectKeepTransfer(t.psm, t.tbp0, t.tbw, 0u, 0u, d.textureWidth, d.textureHeight))
                    ++m_lazyWhy[1];
                m_cpu.DecodeTexture(d, tc.pixels);
                ++m_decodeFromVram;
                static const bool stats = std::getenv("PS2X_HW_UPSTATS") != nullptr;
                static int logged = 0;
                if (stats && logged++ < 12)
                    std::fprintf(stderr, "[upload] texture from VRAM: tbp %u tbw %u psm %u %ux%u (%s)\n", t.tbp0, t.tbw, t.psm, d.textureWidth,
                                 d.textureHeight, m_pendingUploads.empty() ? "nothing queued" : "not one queued upload's");

            }
            static const bool dump = std::getenv("VCS_TEXTURE_DUMP") && *std::getenv("VCS_TEXTURE_DUMP") == '1'; // [gta-recomp]
            if (dump)
                dumpTexture(d, tc.pixels);
            // [gta-recomp] anisotropic filtering samples a mip chain: built here, on the GS stage, not on the main thread's
            // GPU queue (PS2X_HW_CPU_MIPS=0: a render-target texture and GenerateMips each, as before)
            static const bool cpuMips = [] {
                const char *e = std::getenv("PS2X_HW_CPU_MIPS");
                return !(e && *e == '0');
            }();
            const bool mipChain = m_anisotropy > 1u && cpuMips && tc.w <= 4096u && tc.h <= 4096u &&
                                  tc.pixels.size() == size_t(tc.w) * tc.h;
            if (!makeTextureEarly(tc, mipChain) && mipChain)
                buildMipChain(tc.pixels, tc.w, tc.h, tc.mips);
            ps2perf::add(ps2perf::Counter::TexDecodeUs, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                            std::chrono::steady_clock::now() - decodeStart).count()));
            f.cmds.push_back({CmdType::TexCreate, static_cast<uint32_t>(f.textures.size())});
            f.textures.push_back(std::move(tc));
            m_texLru[key] = m_frameIndex;
            if (m_texLru.size() > kMaxTextures)
                evictTextures();
            return true;
        }

        void ensureMakersLocked(rhi::Device &dev)
        {
            if (m_makers.empty() && m_helperThreads > 0)
                for (uint32_t i = 0; i < m_helperThreads; ++i)
                    m_makers.emplace_back([this, &dev] { textureMaker(dev); });
        }

        // [gta-recomp] With a free-threaded device (D3D11, Vulkan) a new texture is made by helper threads, right after its decode:
        // the driver's creation and first upload ran on the main thread between its draws, and a burst of new textures
        // (driving into a new area: 100-600 in a frame) held the picture back there (PERFORMANCE.md 21, issue #29); made
        // on the GS stage itself, the burst held back the frame being built. The helpers make it - with its mip
        // chain for anisotropic filtering - while the GS stage goes on; the main thread takes it when it executes the
        // TexCreate (waiting only if it is not done yet). True when handed off (tc.pixels moved there).
        // Configurable via PS2X_HW_TEXTURE_HELPERS=N or PS2X_HW_EARLY_TEXTURES.
        bool makeTextureEarly(TexCreate &tc, bool mipChain)
        {
            rhi::Device *dev = m_earlyDevice.load(std::memory_order_acquire);
            if (!dev || tc.pixels.size() != size_t(tc.w) * tc.h)
                return false;
            if (m_anisotropy > 1u && !mipChain)
                return false; // a chain by GenerateMips (PS2X_HW_CPU_MIPS=0): the immediate context, main thread only
            static const bool dumping = std::getenv("PS2X_HW_DUMP_TEX") != nullptr; // the executor writes t.pixels
            if (dumping)
                return false;
            auto job = std::make_shared<EarlyTexture>();
            job->w = tc.w;
            job->h = tc.h;
            job->mipChain = mipChain;
            job->pixels = std::move(tc.pixels);
            tc.pixels.clear();
            tc.early = job;
            {
                std::lock_guard<std::mutex> lock(m_makeMutex);
                ensureMakersLocked(*dev);
                m_makeQueue.push_back(std::move(job));
            }
            m_makeCv.notify_one();
            return true;
        }

        // [gta-recomp] a pack DDS's creation handed to the helpers (makeTextureEarly); false when they do not run
        bool makeReplacementEarly(TexCreate &tc, const std::shared_ptr<const render::ReplacementImage> &image)
        {
            rhi::Device *dev = m_earlyDevice.load(std::memory_order_acquire);
            if (!dev)
                return false;
            auto job = std::make_shared<EarlyTexture>();
            job->w = tc.w;
            job->h = tc.h;
            job->replacement = image;
            job->levels = m_anisotropy ? 16u : 1u; // level 0 alone without anisotropic filtering, which reads mips
            tc.early = job;
            {
                std::lock_guard<std::mutex> lock(m_makeMutex);
                ensureMakersLocked(*dev);
                m_makeQueue.push_back(std::move(job));
            }
            m_makeCv.notify_one();
            return true;
        }

        void textureMaker(rhi::Device &dev)
        {
            for (;;)
            {
                std::shared_ptr<EarlyTexture> job;
                {
                    std::unique_lock<std::mutex> lock(m_makeMutex);
                    m_makeCv.wait(lock, [&] { return m_makeStop || !m_makeQueue.empty(); });
                    if (m_makeQueue.empty())
                        return;
                    job = std::move(m_makeQueue.front());
                    m_makeQueue.pop_front();
                }
                rhi::TextureDesc d;
                d.width = job->w;
                d.height = job->h;
                d.usage = rhi::UsageSampled;
                d.name = "gs texture";
                if (job->replacement) // [gta-recomp] a pack DDS: its stored levels, as the main thread made it before
                {
                    const render::ReplacementImage &r = *job->replacement;
                    const void *levels[16];
                    const uint32_t n = std::min<uint32_t>(job->levels, std::min<uint32_t>(uint32_t(r.levelOffsets.size()), 16u));
                    for (uint32_t i = 0; i < n; ++i)
                        levels[i] = r.levels.data() + r.levelOffsets[i];
                    d.name = "replacement texture";
                    d.format = r.format;
                    job->made = dev.createTextureLevels(d, levels, n);
                    if (!job->made)
                        std::fprintf(stderr, "[textures] texture %ux%u (format %u) not made\n", job->w, job->h, unsigned(r.format));
                    job->bytes = r.gpuBytes(n > 1u);
                    job->replacement.reset();
                    job->done.store(true, std::memory_order_release);
                    job->done.notify_all();
                    continue;
                }
                if (job->mipChain)
                {
                    buildMipChain(job->pixels, job->w, job->h, job->mips);
                    const void *levels[16];
                    job->made = dev.createTextureLevels(d, levels, mipLevels(job->pixels, job->mips, job->w, job->h, levels));
                }
                else
                    job->made = dev.createTexture(d, job->pixels.data(), job->w * 4u);
                job->bytes = (job->pixels.size() + job->mips.size()) * 4u;
                std::vector<uint32_t>().swap(job->pixels); // the memory back now, not when the frame is recycled
                std::vector<uint32_t>().swap(job->mips);
                job->done.store(true, std::memory_order_release);
                job->done.notify_all();
            }
        }

        void stopTextureMakers()
        {
            {
                std::lock_guard<std::mutex> lock(m_makeMutex);
                m_makeStop = true;
            }
            m_makeCv.notify_all();
            for (std::thread &t : m_makers)
                t.join();
            m_makers.clear();
        }

        void evictTextures()
        {
            ++m_texEvictions;
            std::vector<std::pair<uint64_t, uint64_t>> byAge(m_texLru.begin(), m_texLru.end());
            std::sort(byAge.begin(), byAge.end(), [](auto &a, auto &b) { return a.second < b.second; });
            Frame &f = *m_frame;
            // [gta-recomp] Evict by bytes as well as count. At 30 fps alternate VBlanks have no draws: protect the
            // two previous frames too, or a small budget evicts the entire active HUD on each empty VBlank.
            for (size_t i = 0; i < byAge.size() &&
                 (i < byAge.size() / 4 || m_replacementBytes > m_replacements->budget()); ++i)
            {
                if (byAge[i].second + 2u >= m_frameIndex)
                    break;
                f.cmds.push_back({CmdType::TexDelete, static_cast<uint32_t>(f.texDeletes.size())});
                f.texDeletes.push_back(byAge[i].first);
                ++m_texStateVersion;
                m_texLru.erase(byAge[i].first);
                if (auto size = m_replacementSizes.find(byAge[i].first); size != m_replacementSizes.end())
                {
                    m_replacementBytes -= size->second;
                    m_replacementSizes.erase(size);
                }
                m_replacementAliases.erase(byAge[i].first);
            }
        }

        // [gta-recomp] scissorMaxY: the GS writes nothing below the scissor, so a draw whose vertices reach further
        // (VCS's sky gradient goes to y 738 on a 448-line screen) must not grow the target past it. An oversized target
        // claims VRAM pages it never uses, and an upload there (the game puts a texture into the Z-buffer area at
        // night) dropped the whole picture: the evening black frames.
        void noteTarget(uint32_t fbp, uint32_t fbw, uint32_t psm, const HwVertex *v, uint32_t n, uint32_t scissorMaxY)
        {
            auto found = m_rts.find(fbp);
            if (found == m_rts.end())
            {
                ++m_texStateVersion; // a new render target: textures there now come from it
                ++m_rtVersion;
            }
            RtInfo &rt = found != m_rts.end() ? found->second : m_rts[fbp];
            if (fbw > rt.fbw)
                ++m_rtVersion;
            rt.fbw = std::max(rt.fbw, fbw);
            rt.psm = psm;
            float maxY = 0.0f;
            for (uint32_t i = 0; i < n; ++i)
                maxY = std::max(maxY, v[i].y);
            maxY = std::min(maxY, static_cast<float>(scissorMaxY) + 1.0f); // vertex y is an edge: pixels up to maxY - 1
            uint32_t pw, ph;
            pageSize(psm, pw, ph);
            const uint32_t lastLine = static_cast<uint32_t>(std::max(maxY - 1.0f, 0.0f));
            const uint32_t rows = std::min<uint32_t>(32u, lastLine / ph + 1u);
            rt.recentRows = std::max(rt.recentRows, rows);
            if (rows > rt.pageRows)
            {
                ++m_texStateVersion;
                ++m_rtVersion;
                static int logged = 0;
                if (rows > 15u && logged < 40 && logDrops())
                {
                    ++logged;
                    std::fprintf(stderr, "[gs-hw] rt fbp=%u fbw=%u grows to %u page rows: draw of %u vertices, max y %.0f, scissor %u\n", fbp,
                                 fbw, rows, n, maxY, scissorMaxY);
                }
            }
            rt.pageRows = std::max(rt.pageRows, std::max<uint32_t>(rows, 1u));
        }

        void touchTransferDest()
        {
            ++m_texStateVersion;
            const GSTransferCommand &t = m_transfer;
            uint32_t first, last;
            pageRange(t.bitbltbuf.dbp, t.bitbltbuf.dbw, t.bitbltbuf.dpsm, t.trxpos.dsax, t.trxpos.dsay, t.trxreg.rrw,
                      t.trxreg.rrh, first, last);
            for (uint32_t p = first; p < last && p < kPages; ++p)
                ++m_pageGen[p];
            std::vector<uint16_t> written; // [gta-recomp] blocks this upload wrote (for patchTarget), filled on demand
            bool writtenKnown = false;
            auto uploadBlocks = [&]() -> const std::vector<uint16_t> & {
                if (!writtenKnown)
                {
                    rectBlocks(t.bitbltbuf.dpsm, t.bitbltbuf.dbp, t.bitbltbuf.dbw, t.trxpos.dsax, t.trxpos.dsay, t.trxreg.rrw,
                               t.trxreg.rrh, written, nullptr);
                    writtenKnown = true;
                }
                return written;
            };
            // An upload over a render target: copy the uploaded rect into it (same base), copy the blocks it wrote
            // (other base), or drop the target (formats without a block layout).
            for (auto it = m_rts.begin(); it != m_rts.end();)
            {
                const uint32_t rtFirst = it->first * 32u / 32u * 1u; // fbp is in 8 KiB pages already
                const uint32_t rtLast = it->first + it->second.pageRows * it->second.fbw;
                if (first < rtLast && last > rtFirst)
                {
                    Frame &f = *m_frame;
                    if (t.bitbltbuf.dbp == it->first * 32u && t.bitbltbuf.dbw == it->second.fbw && t.trxreg.rrw && t.trxreg.rrh)
                    {
                        RtUpload up;
                        up.fbp = it->first;
                        up.fbw = it->second.fbw;
                        up.x = t.trxpos.dsax;
                        up.y = t.trxpos.dsay;
                        up.w = std::min<uint32_t>(t.trxreg.rrw, 2048u);
                        up.h = std::min<uint32_t>(t.trxreg.rrh, 2048u);
                        up.pixels.resize(static_cast<size_t>(up.w) * up.h);
                        const uint32_t psm = it->second.psm;
                        for (uint32_t y = 0; y < up.h; ++y)
                            for (uint32_t x = 0; x < up.w; ++x)
                            {
                                uint32_t v = m_cpu.ReadVram(psm, it->first * 32u, up.fbw, up.x + x, up.y + y);
                                if (psm == GS_PSM_CT16 || psm == GS_PSM_CT16S)
                                    v = ((v & 0x1Fu) << 3) | (((v >> 5) & 0x1Fu) << 11) | (((v >> 10) & 0x1Fu) << 19) |
                                        ((v & 0x8000u) ? 0x80000000u : 0u);
                                up.pixels[static_cast<size_t>(y) * up.w + x] = v;
                            }
                        f.cmds.push_back({CmdType::RtUpload, static_cast<uint32_t>(f.rtUploads.size())});
                        f.rtUploads.push_back(std::move(up));
                        ++it;
                    }
                    else if (patchTargets() && patchTarget(it->first, it->second, uploadBlocks()))
                    {
                        ++it; // [gta-recomp] the written blocks were copied into the target
                    }
                    else
                    {
                        static int logged = 0; // [gta-recomp] which uploads drop render targets (evening black frames)
                        if (logged < 200 && logDrops())
                        {
                            ++logged;
                            std::fprintf(stderr,
                                         "[gs-hw] upload dbp=%u dbw=%u psm=%u %ux%u at %u,%u (pages %u-%u) drops rt fbp=%u fbw=%u rows=%u "
                                         "(pages %u-%u)\n",
                                         t.bitbltbuf.dbp, t.bitbltbuf.dbw, t.bitbltbuf.dpsm, t.trxreg.rrw, t.trxreg.rrh, t.trxpos.dsax,
                                         t.trxpos.dsay, first, last, it->first, it->second.fbw, it->second.pageRows, rtFirst, rtLast);
                        }
                        f.cmds.push_back({CmdType::RtDrop, static_cast<uint32_t>(f.rtDrops.size())});
                        f.rtDrops.push_back(it->first);
                        it = m_rts.erase(it);
                        ++m_rtVersion;
                    }
                }
                else
                {
                    ++it;
                }
            }
        }

        // ---- state --------------------------------------------------------------------------------------------
        struct RtInfo
        {
            uint32_t fbw = 1, psm = 0, pageRows = 1;
            uint32_t recentRows = 0; // [gta-recomp] rows drawn since the last trim (shrinkTargets)
        };

        // [gta-recomp] A target grows to the tallest draw ever made into it (VCS clears all of VRAM through FBP 0 at
        // boot: 1472 lines); every 300 frames trim each target to what was drawn in that time, so it stops claiming
        // pages other data lives in (an upload there would drop or rewrite it for nothing).
        void shrinkTargets()
        {
            if (m_frameIndex % 300u != 0u)
                return;
            for (auto &[fbp, rt] : m_rts)
            {
                if (rt.recentRows && rt.recentRows < rt.pageRows)
                {
                    if (logDrops())
                        std::fprintf(stderr, "[gs-hw] rt fbp=%u trimmed %u -> %u page rows\n", fbp, rt.pageRows, rt.recentRows);
                    rt.pageRows = rt.recentRows;
                    ++m_texStateVersion;
                    ++m_rtVersion;
                }
                rt.recentRows = 0;
            }
        }

        // [gta-recomp] Block (8x8 / 16x8 pixels) position inside a page for each of its 32 blocks, per format; false for
        // formats GSMem::RectBlocks does not know.
        bool blockLayout(uint32_t psm, std::array<std::pair<uint8_t, uint8_t>, 32> &where, uint32_t &bw, uint32_t &bh)
        {
            uint32_t pw, ph;
            pageSize(psm, pw, ph);
            bw = pw / 8u;
            bh = ph / 4u;
            if (psm == GS_PSM_CT16 || psm == GS_PSM_CT16S)
            {
                bw = 16u;
                bh = 8u;
            }
            if (pw / bw * (ph / bh) != 32u)
                return false;
            std::vector<uint16_t> touched;
            for (uint32_t by = 0; by < ph / bh; ++by)
                for (uint32_t bx = 0; bx < pw / bw; ++bx)
                {
                    touched.clear();
                    if (!GSMem::RectBlocks(psm, 0u, pw / 64u, bx * bw, by * bh, bw, bh, touched, nullptr) || touched.size() != 1u ||
                        touched[0] >= 32u)
                        return false;
                    where[touched[0]] = {static_cast<uint8_t>(bx), static_cast<uint8_t>(by)};
                }
            return true;
        }

        // [gta-recomp] PS2X_HW_PATCH_TARGETS=1 turns patchTarget on. Off by default: VCS streams textures into the VRAM
        // around its bloom buffer (FBP 420) every frame - ~6000 small uploads a second - and patching each one costs
        // more than the game frame (3 fps). Doing it right needs lazy patching (dirty blocks resolved only when the
        // target is read); until then overlapped targets are dropped as before.
        static bool patchTargets()
        {
            static const bool on = [] {
                const char *e = std::getenv("PS2X_HW_PATCH_TARGETS");
                return e && *e && *e != '0';
            }();
            return on;
        }

        // [gta-recomp] An upload that overlaps a render target at another base: copy just the blocks it wrote into the
        // target (as the GS would - the rest of the target keeps what was drawn), read back from CPU VRAM in the
        // target's own layout. VCS puts a CLUT inside its bloom buffer and a texture in the Z-buffer area each frame;
        // dropping the whole target there lost the picture. False when the format is not supported (then drop).
        bool patchTarget(uint32_t fbp, const RtInfo &rt, const std::vector<uint16_t> &written)
        {
            std::array<std::pair<uint8_t, uint8_t>, 32> where{};
            uint32_t bw = 0, bh = 0, pw = 0, ph = 0;
            if (!blockLayout(rt.psm, where, bw, bh))
                return false;
            pageSize(rt.psm, pw, ph);
            const uint32_t firstBlock = fbp * 32u, lastBlock = (fbp + rt.pageRows * rt.fbw) * 32u;
            std::vector<uint32_t> perPage(rt.pageRows * rt.fbw, 0u); // bitmask of written blocks per target page
            uint32_t count = 0;
            for (uint16_t b : written)
                if (b >= firstBlock && b < lastBlock)
                {
                    perPage[(b - firstBlock) / 32u] |= 1u << ((b - firstBlock) % 32u);
                    ++count;
                }
            if (!count)
                return true;
            // Large overwrites (VCS: a 512x512 texture into the Z-buffer area every frame, seen through the Z-clear view
            // FBP 280) are dropped as before: reading them back per pixel cost ~200 ms a frame, and a target mostly
            // overwritten has little worth keeping. Small ones (a CLUT inside the bloom buffer) are patched.
            if (count > 256u)
                return false;
            static uint64_t patches = 0, blocks = 0;
            ++patches;
            blocks += count;
            if (logDrops() && (patches & (patches - 1)) == 0) // powers of two
                std::fprintf(stderr, "[gs-hw] patched %llu uploads into targets (%llu blocks; last: fbp %u, %u blocks)\n",
                             static_cast<unsigned long long>(patches), static_cast<unsigned long long>(blocks), fbp, count);
            Frame &f = *m_frame;
            auto emit = [&](uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
                RtUpload up;
                up.fbp = fbp;
                up.fbw = rt.fbw;
                up.x = x;
                up.y = y;
                up.w = w;
                up.h = h;
                up.pixels.resize(static_cast<size_t>(w) * h);
                for (uint32_t yy = 0; yy < h; ++yy)
                    for (uint32_t xx = 0; xx < w; ++xx)
                    {
                        uint32_t v = m_cpu.ReadVram(rt.psm, fbp * 32u, rt.fbw, x + xx, y + yy);
                        if (rt.psm == GS_PSM_CT16 || rt.psm == GS_PSM_CT16S)
                            v = ((v & 0x1Fu) << 3) | (((v >> 5) & 0x1Fu) << 11) | (((v >> 10) & 0x1Fu) << 19) |
                                ((v & 0x8000u) ? 0x80000000u : 0u);
                        up.pixels[static_cast<size_t>(yy) * w + xx] = v;
                    }
                f.cmds.push_back({CmdType::RtUpload, static_cast<uint32_t>(f.rtUploads.size())});
                f.rtUploads.push_back(std::move(up));
            };
            for (uint32_t pg = 0; pg < perPage.size(); ++pg)
            {
                const uint32_t mask = perPage[pg];
                if (!mask)
                    continue;
                const uint32_t px = (pg % rt.fbw) * pw, py = (pg / rt.fbw) * ph;
                if (mask == 0xFFFFFFFFu)
                {
                    emit(px, py, pw, ph);
                    continue;
                }
                for (uint32_t k = 0; k < 32u; ++k)
                    if (mask & (1u << k))
                        emit(px + where[k].first * bw, py + where[k].second * bh, bw, bh);
            }
            return true;
        }
        static constexpr uint32_t kPages = 512u; // 4 MiB / 8 KiB
        static constexpr size_t kMaxTextures = 3000u;
        static constexpr size_t kMaxMeshes = 12000u; // [gta-recomp] native draws' vertex data (~3-10 KB each)

        uint32_t m_scale;
        bool m_nativeFrame = false; // [gta-recomp] GSHwOptions::nativeFrame
        uint32_t m_msaa = 1, m_anisotropy = 0; // [gta-recomp] GSHwOptions
        uint32_t m_helperThreads = 0; // [gta-recomp] PS2X_HW_TEXTURE_HELPERS / PS2X_HW_EARLY_TEXTURES
        std::atomic<rhi::Device *> m_earlyDevice{nullptr}; // [gta-recomp] makeTextureEarly: set by the main thread
        std::mutex m_makeMutex;                             // [gta-recomp] makeTextureEarly's helper threads
        std::condition_variable m_makeCv;
        std::deque<std::shared_ptr<EarlyTexture>> m_makeQueue;
        std::vector<std::thread> m_makers;
        bool m_makeStop = false;
        GSCpuBackend m_cpu;
        // [gta-recomp] Optional PCSX2 texture packs; none of these data are game assets. Never null: an empty pack until
        // gsHwSetTextureReplacements gives one (switchTexturePack)
        std::shared_ptr<render::TextureReplacements> m_replacements =
            std::make_shared<render::TextureReplacements>(std::vector<render::TextureReplacements::Source>{});
        uint64_t m_packGeneration = 0;
        bool m_blockCompression = true; // the device's, for every pack
        uint8_t *m_textureVram = nullptr;
        std::vector<uint8_t> m_replacementHashBytes;
        std::vector<uint32_t> m_replacementScratch;
        std::unordered_set<std::string> m_texturesDumped; // [gta-recomp] VCS_TEXTURE_DUMP: names written this run
        std::unordered_map<uint64_t, uint32_t> m_replacementAliases;
        std::unordered_map<uint64_t, size_t> m_replacementSizes;
        size_t m_replacementBytes = 0;
        GSTransferCommand m_transfer{};
        std::unique_ptr<Frame> m_frame;
        uint64_t m_frameIndex = 0;
        uint32_t m_pageGen[kPages]{};
        struct UploadRecord // [gta-recomp] last whole-transfer upload per destination (see UploadImage)
        {
            uint64_t hash = 0;
            uint32_t first = 0;
            std::vector<uint32_t> gens;
        };
        std::unordered_map<uint64_t, UploadRecord> m_uploads;
        struct UploadInfo // [gta-recomp] last whole upload per base pointer (content-keyed textures)
        {
            uint64_t hash = 0;
            uint32_t dbw = 0, dpsm = 0, x = 0, y = 0, w = 0, h = 0, first = 0;
            uint32_t raster = 0; // [gta-recomp] the game's raster it sent (RAM address), 0 unknown
            std::vector<uint32_t> gens;
        };
        uint64_t m_texNamed = 0, m_texUnnamed = 0; // [gta-recomp] textures made, by whether their raster is known
        uint64_t m_bindCalls = 0, m_bindRepeats = 0, m_bindLookups = 0, m_bindLookupHits = 0; // [gta-recomp] PS2X_HW_UPSTATS
        std::unordered_map<uint32_t, UploadInfo> m_uploadAt;
        uint64_t m_skippedUploads = 0;
        std::unordered_map<uint64_t, uint64_t> m_texLru;  // key -> last frame used
        std::unordered_map<uint64_t, uint64_t> m_meshLru; // [gta-recomp] native draws' meshes: key -> last frame used
        std::unordered_map<uint32_t, RtInfo> m_rts;       // fbp -> extent (game thread's view)
        struct TexBindInput // [gta-recomp] bindTexture cache (see there)
        {
            uint64_t version;
            uint32_t tbp0, cbp;
            uint16_t w, h;
            uint8_t tbw, psm, cpsm, csm, csa, ta0, ta1, aem;
        };
        uint64_t m_texStateVersion = 1;
        TexBindInput m_bindIn{};
        HwState m_bindOut{};
        bool m_bindResult = false, m_bindCacheValid = false;
        uint64_t m_bindTouchedFrame = 0;
        struct BindMemo // [gta-recomp] bindTexture's second level
        {
            TexBindInput in{};
            uint64_t deps = 0, texKey = 0, touchedFrame = 0;
            uint32_t rtFbp = 0;
            uint16_t rtOx = 0, rtOy = 0;
            uint8_t texIsRt = 0;
            bool result = false, valid = false;
        };
        static constexpr size_t kBindMemoSize = 4096;
        std::vector<BindMemo> m_bindMemo = std::vector<BindMemo>(kBindMemoSize);
        uint64_t m_rtVersion = 1;    // [gta-recomp] the render targets (m_rts) changed: bindTexture's memo
        uint64_t m_texEvictions = 0; // [gta-recomp] evictTextures ran: bindTexture's memo
        uint64_t m_bindMemoHits = 0;
        std::deque<PendingUpload> m_pendingUploads;       // [gta-recomp] lazy uploads, in submission order
        uint64_t m_pendingFrontId = 0;                    // id of m_pendingUploads.front()
        std::vector<uint16_t> m_blockPending = std::vector<uint16_t>(kBlocks, 0u); // queued uploads touching each block
        std::vector<std::vector<uint64_t>> m_blockWriters = std::vector<std::vector<uint64_t>>(kBlocks); // their ids
        // ids of every queued upload touching each block, also after a later one overwrote it completely (m_blockWriters
        // forgets those) - materializeBlocks' index; ids of uploads no longer queued are pruned when met
        std::vector<std::vector<uint64_t>> m_blockUploads = std::vector<std::vector<uint64_t>>(kBlocks);
        std::vector<uint16_t> m_scratchWork;
        std::vector<size_t> m_scratchTakeList;
        std::vector<uint16_t> m_scratchTouched, m_scratchFull, m_scratchRead;
        std::vector<uint32_t> m_wantStamp = std::vector<uint32_t>(kBlocks, 0u);
        uint32_t m_wantGeneration = 0;
        std::vector<uint8_t> m_scratchTake;
        size_t m_pendingBytes = 0;
        uint64_t m_lazyDeferred = 0, m_lazyWritten = 0, m_lazyDropped = 0;
        uint64_t m_clutFromUpload = 0, m_clutVerifySame = 0, m_clutVerifyDiffer = 0; // [gta-recomp] loadClutFromQueuedUpload
        uint64_t m_decodeFromUpload = 0, m_decodeVerifySame = 0, m_decodeVerifyDiffer = 0; // [gta-recomp] decodeFromQueuedUpload
        uint64_t m_decodeCrossFormat = 0, m_decodeFromVram = 0, m_decodeFast = 0;
        std::vector<int32_t> m_scratchPixelAt; // decodeFromQueuedUpload: upload pixel index by VRAM address
        uint64_t m_lazyWhy[4]{}; // what forced queued uploads out: clut load, texture decode, direct upload, other
        std::unordered_map<uint32_t, DepthInfo> m_depthBufs; // zbp -> format / width

        std::mutex m_queueMutex;
        std::condition_variable m_queueCv;
        std::deque<std::unique_ptr<Frame>> m_queue;
        std::vector<std::unique_ptr<Frame>> m_freeFrames; // executed frames for reuse (m_queueMutex)

        // main thread
        std::unique_ptr<RhiExecutor> m_rhi; // [gta-recomp] the renderer (D3D11)
        uint64_t m_executedFrames = 0;
        uint32_t m_loggedTransfers = 0, m_loggedTextures = 0;
        bool m_dumped = false;
        bool m_f12Down = false; // [gta-recomp]
        bool m_dumpPending = false; // [gta-recomp] F12 / PS2X_HW_DUMP waiting for a frame with draws
        int m_dumpBatchesLeft = 0;  // [gta-recomp] PS2X_HW_DUMP_BATCHES
        uint32_t m_3dHistory = ~0u; // [gta-recomp] showing3d: had3d of the last 32 frames, newest in bit 0
        std::atomic<bool> m_showing3d{true}; // main thread writes, the game thread's Submit reads
        std::atomic<bool> m_shownWide{true}; // [gta-recomp] the displayed picture's shape: the presented one (showing3d)
        bool m_menuShown = false;            // [gta-recomp] 4:3 because of the game's menu (decideShape)

        // 3D or not over the last 32 frames (the game draws 3D in every other VBlank at 30 fps): 3D in 4 or more of them
        // shows 3D, none shows 2D, in between keeps the current shape (fades, loading flashes).
        // [gta-recomp] Decided per frame when it is executed, from all of it: the game's menu over no 3D (the pause menu)
        // is 4:3 at once, and the first 3D frame after it wide at once (half a second of the menu's backdrop stretched
        // on the way in and four frames of the game squashed into 4:3 on the way out before; PS2X_HW_MENU_SWITCH=0: the
        // history alone, as before).
        void decideShape(Frame &frame)
        {
            static const bool menuSwitch = [] {
                const char *e = std::getenv("PS2X_HW_MENU_SWITCH");
                return !(e && e[0] == '0');
            }();
            m_3dHistory = (m_3dHistory << 1) | (frame.had3d ? 1u : 0u);
            if (menuSwitch && frame.menu && !frame.had3d)
            {
                m_3dHistory = 0u;
                m_menuShown = true;
            }
            else if (menuSwitch && frame.had3d && m_menuShown)
                m_3dHistory = ~0u;
            const int n = std::popcount(m_3dHistory);
            if (n >= 4)
            {
                m_showing3d = true;
                m_menuShown = false;
            }
            else if (n == 0)
                m_showing3d = false;
            frame.shown = m_showing3d.load(std::memory_order_relaxed) ? 1 : 0;
        }

    public:
        bool showing3d() const override { return m_shownWide.load(std::memory_order_relaxed); }

        // [gta-recomp] The recording side's guess whether the frame is shown wide, for the draws squeezed into the
        // buffer itself (the HUD layer's squeeze waits for decideShape): m_showing3d when the frame first asks, kept for
        // the whole frame.
        bool frameWide()
        {
            Frame &f = *m_frame;
            if (f.wide3d < 0)
                f.wide3d = m_showing3d.load(std::memory_order_relaxed) ? 1 : 0;
            return f.wide3d != 0;
        }
        std::string lastFrameInfo() const override
        {
            std::lock_guard<std::mutex> lock(m_infoMutex);
            return m_lastInfo;
        }
        mutable std::mutex m_infoMutex; // [gta-recomp] lastFrameInfo
        std::string m_lastInfo;
    };
}

std::unique_ptr<GSHwBackendHandle> makeHwRasterBackend(const GSHwOptions &options)
{
    return std::make_unique<HwBackend>(options);
}

std::string gsHwDrawSourceReport(bool reset) // [gta-recomp]
{
    std::lock_guard<std::mutex> lock(g_drawSourcesMutex);
    return g_drawSources.report(reset);
}
