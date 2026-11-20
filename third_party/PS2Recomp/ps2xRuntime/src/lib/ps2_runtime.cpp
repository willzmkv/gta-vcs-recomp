#include "runtime/ps2_render_worker.h" // [gta-recomp]
#include "runtime/ps2_present.h" // [gta-recomp]
#include "runtime/ps2_iop_native.h" // [gta-recomp]
#include "ps2_runtime.h"
#include "runtime/gs/gs_gpu.h"
#include "runtime/gs/gs_hw.h"
#include "ps2_log.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "game_overrides.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_draw_source.h" // [gta-recomp]
#include "runtime/ee_scheduler.h"
#include "ThreadNaming.h"
#include "Kernel/Stubs/Audio.h"
#include "Kernel/Stubs/GS.h"
#include "Kernel/Stubs/MPEG.h"
#include "ps2_host_backend.h"
#include "render/presenter.h"
#include "rhi/rhi.h" // [gta-recomp] backendName
#include "ps2_iop_host.h"
#include "ps2x/iop/iop_subsystem.h"
#include "runtime/ps2_vu0_native.h"
#include "runtime/ps2_audio_out.h"
#include "runtime/ps2_perf.h"
#include "runtime/ps2_control.h"
#include "runtime/ps2_hle_survey.h"

#include <iostream>
#include <stdexcept>
#include <fstream>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <chrono>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <map>
#include <unordered_set>
#include <cfenv>
#include <string>
#include <vector>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <ctime> // [gta-recomp] clock_gettime (PS2X_HOST_SPIKES)
#include "runtime/ps2_host_std.h"

namespace ps2_stubs
{
    void resetSifState();
}

#define ELF_MAGIC 0x464C457F // "\x7FELF" in little endian
#define ET_EXEC 2            // Executable file
#define EM_MIPS 8            // MIPS architecture
#define PT_LOAD 1            // Loadable segment

static constexpr int FB_WIDTH = 640;
static constexpr int FB_HEIGHT = 512;
static constexpr int DEFAULT_DISPLAY_HEIGHT = 448;
static constexpr uint32_t DEFAULT_FB_SIZE = FB_WIDTH * FB_HEIGHT * 4;
static constexpr uint32_t DEFAULT_FB_ADDR = (PS2_RAM_SIZE - DEFAULT_FB_SIZE - 0x10000u);
#if defined(PLATFORM_VITA)
static constexpr int HOST_WINDOW_WIDTH = 960;
static constexpr int HOST_WINDOW_HEIGHT = 544;
#else
static constexpr int HOST_WINDOW_WIDTH = FB_WIDTH;
static constexpr int HOST_WINDOW_HEIGHT = DEFAULT_DISPLAY_HEIGHT;
#endif
struct ElfHeader
{
    uint32_t magic;
    uint8_t elf_class;
    uint8_t endianness;
    uint8_t version;
    uint8_t os_abi;
    uint8_t abi_version;
    uint8_t padding[7];
    uint16_t type;
    uint16_t machine;
    uint32_t version2;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct ProgramHeader
{
    uint32_t type;
    uint32_t offset;
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
    uint32_t align;
};

namespace
{
    constexpr uint32_t kGuestHeapDefaultBase = 0x00100000u;
    constexpr uint32_t kGuestHeapDefaultAlignment = 16u;
    constexpr uint32_t kGuestHeapSafetyPad = 0x1000u;
    constexpr uint32_t kGuestHeapHardLimit = 0x01F00000u;

    constexpr uint32_t COP0_CAUSE_EXCCODE_MASK = 0x0000007Cu;
    constexpr uint32_t COP0_CAUSE_BD = 0x80000000u;
    constexpr uint32_t COP0_STATUS_EXL = 0x00000002u;
    constexpr uint32_t COP0_STATUS_BEV = 0x00400000u;
    constexpr uint32_t EXCEPTION_VECTOR_GENERAL = 0x80000080u;
    constexpr uint32_t EXCEPTION_VECTOR_TLB_REFILL = 0x80000000u;
    constexpr uint32_t EXCEPTION_VECTOR_BOOT = 0xBFC00200u;

    struct DispatchHistory
    {
        std::array<uint32_t, 64> pcs{};
        uint32_t next = 0u;
        bool wrapped = false;
    };

    thread_local DispatchHistory g_dispatchHistory;

    bool computeFileCrc32(const std::string &path, uint32_t &crcOut)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }

        static const std::array<uint32_t, 256> table = []
        {
            std::array<uint32_t, 256> values{};
            for (uint32_t i = 0; i < values.size(); ++i)
            {
                uint32_t value = i;
                for (uint32_t bit = 0; bit < 8; ++bit)
                {
                    value = (value & 1u) ? (0xEDB88320u ^ (value >> 1u)) : (value >> 1u);
                }
                values[i] = value;
            }
            return values;
        }();

        uint32_t crc = 0xFFFFFFFFu;
        std::array<uint8_t, 16 * 1024> buffer{};
        while (file.good())
        {
            file.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = file.gcount();
            for (std::streamsize i = 0; i < count; ++i)
            {
                crc = table[(crc ^ buffer[static_cast<size_t>(i)]) & 0xFFu] ^ (crc >> 8u);
            }
        }
        if (file.bad())
        {
            return false;
        }
        crcOut = ~crc;
        return true;
    }

    void pushDispatchPc(uint32_t pc)
    {
        DispatchHistory &h = g_dispatchHistory;
        h.pcs[h.next] = pc;
        h.next = (h.next + 1u) % static_cast<uint32_t>(h.pcs.size());
        if (h.next == 0u)
        {
            h.wrapped = true;
        }
    }

    std::string formatDispatchHistory()
    {
        const DispatchHistory &h = g_dispatchHistory;
        const uint32_t count = h.wrapped ? static_cast<uint32_t>(h.pcs.size()) : h.next;
        if (count == 0u)
        {
            return "(empty)";
        }

        std::ostringstream oss;
        bool first = true;
        for (uint32_t i = 0u; i < count; ++i)
        {
            const uint32_t idx = (h.next + h.pcs.size() - count + i) % static_cast<uint32_t>(h.pcs.size());
            if (!first)
            {
                oss << " -> ";
            }
            first = false;
            oss << "0x" << std::hex << h.pcs[idx];
        }
        return oss.str();
    }

    uint32_t selectExceptionVector(const R5900Context *ctx, bool tlbRefill)
    {
        if (ctx->cop0_status & COP0_STATUS_BEV)
        {
            return EXCEPTION_VECTOR_BOOT;
        }
        return tlbRefill ? EXCEPTION_VECTOR_TLB_REFILL : EXCEPTION_VECTOR_GENERAL;
    }

    void seedVu0IdleSuccess(R5900Context *ctx)
    {
        if (!ctx)
        {
            return;
        }

        ctx->vu0_clip_flags = 0;
        ctx->vu0_clip_flags2 = 0;
        ctx->vu0_mac_flags = 0;
        ctx->vu0_status = 0;
        ctx->vu0_q = 1.0f;
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3F800000));
        ctx->vu0_vpu_stat = 0;
        ctx->vu0_vpu_stat2 = 0;
    }

    void copyVu0ContextToState(const R5900Context *ctx, VU1State &state)
    {
        std::memset(&state, 0, sizeof(state));

        for (uint32_t i = 0; i < 32u; ++i)
        {
            _mm_storeu_ps(state.vf[i], ctx->vu0_vf[i]);
        }
        for (uint32_t i = 0; i < 16u; ++i)
        {
            state.vi[i] = static_cast<int16_t>(ctx->vi[i]);
        }

        _mm_storeu_ps(state.acc, ctx->vu0_acc);
        state.q = ctx->vu0_q;
        state.p = ctx->vu0_p;
        state.i = ctx->vu0_i;
        alignas(16) uint32_t rWords[4]{};
        _mm_storeu_si128(reinterpret_cast<__m128i *>(rWords), _mm_castps_si128(ctx->vu0_r));
        state.r = 0x3F800000u | (rWords[0] & 0x007FFFFFu);
        state.pc = ctx->vu0_pc;
        state.mac = ctx->vu0_mac_flags;
        state.clip = ctx->vu0_clip_flags;
        state.status = ctx->vu0_status;
        state.itop = ctx->vu0_itop;
        state.dBitEnabled = (ctx->vu0_fbrst & (1u << 2)) != 0u;
        state.tBitEnabled = (ctx->vu0_fbrst & (1u << 3)) != 0u;

        state.vf[0][0] = 0.0f;
        state.vf[0][1] = 0.0f;
        state.vf[0][2] = 0.0f;
        state.vf[0][3] = 1.0f;
        state.vi[0] = 0;
    }

    void copyVu0StateToContext(const VU1State &state, R5900Context *ctx)
    {
        for (uint32_t i = 0; i < 32u; ++i)
        {
            ctx->vu0_vf[i] = _mm_loadu_ps(state.vf[i]);
        }
        for (uint32_t i = 0; i < 16u; ++i)
        {
            ctx->vi[i] = static_cast<uint16_t>(state.vi[i]);
        }

        ctx->vu0_acc = _mm_loadu_ps(state.acc);
        ctx->vu0_q = state.q;
        ctx->vu0_p = state.p;
        ctx->vu0_i = state.i;
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(static_cast<int32_t>(state.r)));
        ctx->vu0_mac_flags = state.mac;
        ctx->vu0_clip_flags = state.clip;
        ctx->vu0_clip_flags2 = state.clip;
        ctx->vu0_status = static_cast<uint16_t>(state.status);
        ctx->vu0_itop = state.itop;
        ctx->vu0_pc = state.pc;
        ctx->vu0_tpc = state.pc;
        ctx->vu0_vpu_stat = (ctx->vu0_vpu_stat & 0xFF00u) | (state.stoppedByD ? (1u << 1) : 0u) | (state.stoppedByT ? (1u << 2) : 0u);
        ctx->vu0_vpu_stat2 = 0;

        ctx->vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
        ctx->vi[0] = 0;
    }

    void raiseCop0Exception(R5900Context *ctx, uint32_t exceptionCode, bool tlbRefill = false)
    {
        if (ctx->in_delay_slot)
        {
            ctx->cop0_epc = ctx->branch_pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~COP0_CAUSE_EXCCODE_MASK) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK) |
                              COP0_CAUSE_BD;
        }
        else
        {
            ctx->cop0_epc = ctx->pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~(COP0_CAUSE_EXCCODE_MASK | COP0_CAUSE_BD)) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK);
        }

        ctx->cop0_status |= COP0_STATUS_EXL;
        ctx->pc = selectExceptionVector(ctx, tlbRefill);
        ctx->in_delay_slot = false;
    }

    std::filesystem::path normalizeAbsolutePath(const std::filesystem::path &path)
    {
        if (path.empty())
        {
            return {};
        }

#if defined(PLATFORM_VITA)
        const std::string generic = path.generic_string();
        const std::size_t colon = generic.find(':');
        if (colon != std::string::npos && colon != 0u)
        {
            const std::size_t slash = generic.find_first_of("/\\");
            if (slash == std::string::npos || colon < slash)
            {
                return path.lexically_normal();
            }
        }
#endif

        std::error_code ec;
        const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
        if (ec)
        {
            return path.lexically_normal();
        }
        return absolute.lexically_normal();
    }

    PS2Runtime::IoPaths &runtimeIoPaths()
    {
        static PS2Runtime::IoPaths paths = []()
        {
            PS2Runtime::IoPaths defaults;
            std::error_code ec;
            const std::filesystem::path cwd = std::filesystem::current_path(ec);
            defaults.elfDirectory = ec ? std::filesystem::path(".") : cwd.lexically_normal();
            defaults.hostRoot = defaults.elfDirectory;
            defaults.cdRoot = defaults.elfDirectory;
            defaults.mcRoot = defaults.elfDirectory / "mc0";
            return defaults;
        }();

        return paths;
    }

    std::string readGuestPrintableString(const uint8_t *rdram, uint32_t addr, size_t maxLen)
    {
        std::string out;
        if (!rdram || maxLen == 0)
        {
            return out;
        }

        out.reserve(std::min<size_t>(maxLen, 64));
        for (size_t i = 0; i < maxLen; ++i)
        {
            const char ch = static_cast<char>(rdram[(addr + static_cast<uint32_t>(i)) & PS2_RAM_MASK]);
            if (ch == '\0')
            {
                break;
            }
            if (ch >= 0x20 && ch < 0x7F)
            {
                out.push_back(ch);
            }
            else
            {
                out.push_back('.');
            }
        }
        return out;
    }
}

// [gta-recomp] Set while generated code unwinds to the scheduler because a checkpoint fired
// (eeCheckpointDue / the call-site check in dispatchGuestBranch). Upstream treated a callee
// that came back with pc == its own entry as "returned normally"; when a *recursive* call is
// the one preempted, pc == entry too, and the outer frame carried on with the inner frames
// still on the stack (sp off by their sizes -> $ra loaded from someone's locals).
bool g_checkpointUnwind = false; // [gta-recomp] extern in ps2_runtime.h (the inline dispatch)
void ps2ClearCheckpointUnwind() { g_checkpointUnwind = false; }

// [gta-recomp] PS2X_TRACE_VU1=1: per VU1 program start address, runs / XGKICKs / where it stopped, every 5 s.
extern uint64_t g_vu1XgkickCount;
static void traceVu1Program(uint32_t startPc, uint64_t kicks, uint32_t endPc)
{
    static const bool trace = std::getenv("PS2X_TRACE_VU1") != nullptr;
    if (!trace)
        return;
    struct Stat { uint64_t runs = 0, kicks = 0, zeroKick = 0; uint32_t lastEnd = 0; };
    static std::map<uint32_t, Stat> stats;
    static auto last = ps2x::hostNow();
    Stat &st = stats[startPc];
    ++st.runs;
    st.kicks += kicks;
    st.zeroKick += kicks == 0u ? 1u : 0u;
    st.lastEnd = endPc;
    const auto now = ps2x::hostNow();
    if (now - last < std::chrono::seconds(5))
        return;
    last = now;
    for (const auto &[pc, v] : stats)
        std::fprintf(stderr, "[vu1] start=0x%04x runs=%llu kicks=%llu runsWithoutKick=%llu lastEnd=0x%04x\n", pc,
                     (unsigned long long)v.runs, (unsigned long long)v.kicks, (unsigned long long)v.zeroKick, v.lastEnd);
    stats.clear();
}

// [gta-recomp] Report each distinct unlisted mid-body entry once (see function_emitter.cpp).
void ps2ReportUnlistedResume(const R5900Context *ctx, uint32_t functionStart)
{
    static std::mutex s_mutex;
    static std::unordered_set<uint32_t> s_seen;
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_seen.insert(ctx->pc).second || s_seen.size() > 64u)
        return;
    std::cerr << "[guest-branch:unlisted-resume] pc=0x" << std::hex << ctx->pc << " function=0x" << functionStart
              << " ra=0x" << GPR_U32(ctx, 31) << " sp=0x" << GPR_U32(ctx, 29) << " trace=" << formatDispatchHistory()
              << std::dec << std::endl;
}

// [gta-recomp] the "burst" control command's state (main thread)
static struct
{
    std::string dir;
    int left = 0, index = 0;
} g_burst;

// [gta-recomp] Shows the GPU renderer's newest picture. False when there is no GPU renderer.
// `owned` is our texture for pictures made on the CPU; `shown` is what the window draws (owned, or the HW renderer's).
static bool UploadGpuFrame(present::Picture &owned, present::Picture &shown, PS2Runtime *rt, uint32_t &outWidth,
                           uint32_t &outHeight)
{
    if (GSHwBackendHandle *hw = rt->hwRenderer())
    {
        // [gta-recomp] the HW renderer draws on the window's device: its display texture is shown directly
        rhi::Texture *display = nullptr;
        uint32_t w = 0, h = 0;
        {
            // [gta-recomp] the size the picture is shown at: the window's letterboxed area (the native HUD layer)
            int windowW = 0, windowH = 0;
            platform::windowSize(windowW, windowH);
            // the window's letterboxed area at an aspect, inside the game's frame as the presenter places it
            // ([gta-recomp] present::fitRect: a 4:3 menu sits in the frame, not over the window)
            auto letterbox = [&](float aspect, uint32_t &outW, uint32_t &outH) {
                outW = outH = 0;
                if (windowW <= 0 || windowH <= 0 || aspect <= 0.0f)
                    return;
                float x0 = 0.0f, y0 = 0.0f, w = 0.0f, h = 0.0f;
                present::fitRect(aspect, present::displayAspect(true), static_cast<float>(windowW), static_cast<float>(windowH), x0, y0, w, h);
                outW = static_cast<uint32_t>(std::lround(w));
                outH = static_cast<uint32_t>(std::lround(h));
            };
            uint32_t outW = 0, outH = 0, frameW = 0, frameH = 0;
            letterbox(present::displayAspect(hw->presenter().showing3d()), outW, outH);
            // the native frame keeps the 3D frames' size when a menu shows at 4:3 (no resizing back and forth)
            letterbox(present::displayAspect(true), frameW, frameH);
            gsHwSetOutputSize(outW, outH);
            gsHwSetFrameSize(frameW, frameH);
        }
        if (hw->presenter().executeAndGetDisplay(*present::device(), display, w, h))
        {
            shown.texture = std::shared_ptr<rhi::Texture>(display, [](rhi::Texture *) {}); // owned by the HW renderer
            shown.width = static_cast<int>(w);
            shown.height = static_cast<int>(h);
            shown.linear = true;
            outWidth = w;
            outHeight = h;
        }
        else
        {
            outWidth = static_cast<uint32_t>(shown.width);
            outHeight = static_cast<uint32_t>(shown.height);
        }
        return true;
    }
    GSGpuRenderer *gpu = rt->gpuRenderer();
    if (!gpu)
        return false;
    static uint64_t s_lastSeq = 0u;
    static uint32_t s_width = 640u, s_height = 448u;
    int windowW = 0, windowH = 0;
    platform::windowSize(windowW, windowH);
    gpu->setOutputSize(static_cast<uint32_t>(windowW), static_cast<uint32_t>(windowH));
    static const bool s_noShow = std::getenv("PS2X_GPU_NOSHOW") != nullptr; // debugging
    if (const GSGpuFrame *frame = s_noShow ? nullptr : gpu->takeFrame(s_lastSeq))
    {
        if (frame->width && frame->height && frame->rgba.size() >= size_t(frame->width) * frame->height * 4u)
        {
            if (owned.width != static_cast<int>(frame->width) || owned.height != static_cast<int>(frame->height))
            {
                std::fprintf(stderr, "[gs] presenting %ux%u (internal %ux%u)\n", frame->width, frame->height, frame->internalWidth, frame->internalHeight);
                owned = present::createPicture(static_cast<int>(frame->width), static_cast<int>(frame->height), true);
            }
            present::updatePicture(owned, frame->rgba.data());
            shown = owned;
            s_width = frame->width;
            s_height = frame->height;
        }
    }
    outWidth = s_width;
    outHeight = s_height;
    return true;
}

namespace
{
    // [gta-recomp] runtime/ps2_present.h
    GSHwBackendHandle *g_presentHw = nullptr;
    PS2Memory *g_presentMemory = nullptr;
    std::atomic<bool> g_presentOnGameFrames{false};
    std::atomic<int64_t> g_lastGameFrameNs{0};
    // [gta-recomp] ps2HostTick / ps2WaitHostTick
    std::mutex g_tickMutex;
    std::condition_variable g_tickCv;
    uint64_t g_hostTick = 0, g_tickTaken = 0;
    std::atomic<bool> g_hostPaced{false};
    std::atomic<uint64_t> g_framesPosted{0}, g_framesTaken{0}; // frames on their way to the window
    int g_deferredTick = -1; // a tick held back by a backlog: frames allowed on their way when it goes (g_tickMutex)

    // a tick, when no more than `allowed` frames are on their way; held back until then otherwise (g_tickMutex held)
    bool releaseTickLocked(uint64_t allowed)
    {
        const uint64_t posted = g_framesPosted.load(std::memory_order_relaxed), taken = g_framesTaken.load(std::memory_order_relaxed);
        if (posted > taken && posted - taken > allowed)
            return false;
        ++g_hostTick;
        return true;
    }

    int64_t presentNowNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(ps2x::hostNow().time_since_epoch()).count();
    }

    // the display registers as they are now; the frame ends after the GS work posted before it
    void postFrameEnd(GSHwBackendHandle *hw, PS2Memory &memory)
    {
        struct Regs
        {
            uint64_t v[15];
        } regs;
        std::memcpy(regs.v, &memory.gs(), sizeof(regs.v));
        ps2perf::PhaseScope renderTime(ps2perf::Phase::Render); // waiting for the worker counts as render time
        g_framesPosted.fetch_add(1, std::memory_order_relaxed);
        ps2_render_worker::post([hw, regs] { ps2_render_worker::runOnGsStage([hw, regs] { hw->endFrame(regs.v); }, true); }, true);
    }
}

static uint32_t g_lastFramesTaken = 0; // [gta-recomp] PS2X_HOST_SPIKES (main thread)
uint32_t ps2LastFramesTaken() { return g_lastFramesTaken; }

void ps2NoteFramesTaken(uint32_t frames)
{
    g_lastFramesTaken = frames;
    g_framesTaken.fetch_add(frames, std::memory_order_relaxed);
    bool released = false;
    {
        std::lock_guard<std::mutex> lock(g_tickMutex);
        if (g_deferredTick >= 0 && releaseTickLocked(static_cast<uint64_t>(g_deferredTick)))
        {
            g_deferredTick = -1;
            released = true;
        }
    }
    if (released)
        g_tickCv.notify_all();
}

void ps2HostTick(bool lead, bool paced)
{
    // A backlog (ps2_present.h): with paced host frames the frame starts at the next tick instead - which brings its phase
    // back to ready-just-in-time (started on a later release it would be a host frame late again). Unpaced (the GPU or
    // the CPU is the limit) the frame waiting is what keeps the GPU fed: the tick goes when the main thread takes it
    // (ps2NoteFramesTaken), a few ms later in the same host frame.
    {
        std::lock_guard<std::mutex> lock(g_tickMutex);
        const uint64_t allowed = lead ? 0u : 1u;
        if (!releaseTickLocked(allowed))
        {
            g_deferredTick = (lead || paced) ? -1 : static_cast<int>(allowed);
            return;
        }
        g_deferredTick = -1;
    }
    g_tickCv.notify_all();
}

bool ps2WaitHostTick()
{
    // PS2X_HOST_PACED=0: the game's own deadline, as before 2026-10-30
    static const bool allowed = [] {
        const char *e = std::getenv("PS2X_HOST_PACED");
        return !(e && e[0] == '0');
    }();
    if (!allowed || !g_hostPaced.load(std::memory_order_relaxed))
        return false;
    ps2perf::PhaseScope waiting(ps2perf::Phase::Wait);
    std::unique_lock<std::mutex> lock(g_tickMutex);
    // the 100 ms timeout is a guard (a main thread held up: a window being dragged), not pacing
    ps2x::waitFor(g_tickCv, lock, std::chrono::milliseconds(100), [] { return g_hostTick != g_tickTaken; });
    g_tickTaken = g_hostTick;
    return true;
}

void ps2SetHostPacedFrames(bool on) { g_hostPaced.store(on, std::memory_order_relaxed); }

void ps2PresentGameFrame()
{
    if (!g_presentHw || !g_presentMemory || !g_presentOnGameFrames.load(std::memory_order_relaxed))
        return;
    g_lastGameFrameNs.store(presentNowNs(), std::memory_order_relaxed);
    postFrameEnd(g_presentHw, *g_presentMemory);
}

void ps2SetPresentOnGameFrames(bool on)
{
    g_presentOnGameFrames.store(on, std::memory_order_relaxed);
}

namespace
{
    // [gta-recomp] movie pictures (ps2_present.h): the latest one, taken by the main loop
    struct MovieFrame
    {
        std::mutex mutex;
        std::vector<uint8_t> rgba;
        int width = 0, height = 0;
        float aspect = 4.0f / 3.0f;
        bool active = false;
        uint64_t sequence = 0;
    } g_movieFrame;
}

void ps2ShowMovieFrame(const uint8_t *rgba, int width, int height, float aspect)
{
    std::lock_guard<std::mutex> lock(g_movieFrame.mutex);
    g_movieFrame.rgba.assign(rgba, rgba + static_cast<size_t>(width) * height * 4u);
    g_movieFrame.width = width;
    g_movieFrame.height = height;
    g_movieFrame.aspect = aspect;
    g_movieFrame.active = true;
    ++g_movieFrame.sequence;
}

void ps2EndMovieFrames()
{
    std::lock_guard<std::mutex> lock(g_movieFrame.mutex);
    g_movieFrame.active = false;
    std::vector<uint8_t>().swap(g_movieFrame.rgba);
}

void PS2Runtime::presentGpuVBlank(uint64_t vsyncTick)
{
    ps2perf::markVBlank(); // [gta-recomp]
    // [gta-recomp] VCS_FPS_LOG=1: count game frames (display buffer flips) per second of host time. (Was VCS_FPS, which
    // is also the frame rate setting's override, so setting the frame rate turned this log on.)
    static const bool s_fps = std::getenv("VCS_FPS_LOG") != nullptr;
    if (s_fps)
    {
        static uint64_t s_lastFb = 0, s_flips = 0, s_vblanks = 0;
        static auto s_since = ps2x::hostNow();
        const uint64_t fb = m_memory.gs().dispfb1 ^ (m_memory.gs().dispfb2 << 1);
        s_flips += fb != s_lastFb;
        s_lastFb = fb;
        ++s_vblanks;
        const auto now = ps2x::hostNow();
        const double secs = std::chrono::duration<double>(now - s_since).count();
        if (secs >= 2.0)
        {
            std::fprintf(stderr, "[fps] %.1f game fps, %.1f vblanks/s\n", s_flips / secs, s_vblanks / secs);
            s_flips = s_vblanks = 0;
            s_since = now;
        }
    }
    // [gta-recomp] with present-on-game-frames (runtime/ps2_present.h) a VBlank ends a frame only when no game frame
    // did for 100 ms (loading screens, movies)
    if (m_hw && !(g_presentOnGameFrames.load(std::memory_order_relaxed) &&
                  presentNowNs() - g_lastGameFrameNs.load(std::memory_order_relaxed) < 100000000))
        postFrameEnd(m_hw, m_memory);
    if (!m_gpu)
        return;
    uint64_t regs[GSGpuRenderer::kPrivRegCount];
    std::memcpy(regs, &m_memory.gs(), sizeof(regs)); // PMODE .. BGCOLOR, GS register order
    m_gpu->present(regs, vsyncTick, true);
    // Don't let the game run more than two frames ahead of the graphics card.
    (void)m_gpu->waitPresentBacklog(2u, m_stopRequested);
}

void PS2Runtime::shutdownGpuRenderer()
{
    if (m_gpu)
        m_gpu->shutdown();
}

static void UploadFrame(present::Picture &tex, present::Picture &shown, PS2Runtime *rt, uint32_t &outWidth,
                        uint32_t &outHeight)
{
    if (UploadGpuFrame(tex, shown, rt, outWidth, outHeight))
        return;
    shown = tex;
    static uint64_t s_lastPresentationTick = std::numeric_limits<uint64_t>::max();
    static bool s_hasLatchedInitialFrame = false;
    static uint32_t s_lastDisplayFbp = std::numeric_limits<uint32_t>::max();
    static uint32_t s_lastSourceFbp = std::numeric_limits<uint32_t>::max();
    static bool s_lastPreferred = false;
    static uint32_t s_lastWidth = 0u;
    static uint32_t s_lastHeight = 0u;
    static bool s_hasUploadedFrame = false;
    static std::vector<uint8_t> s_scratch;
    static std::vector<uint8_t> s_uploadBuffer(DEFAULT_FB_SIZE, 0u);

    const uint64_t currentTick = rt->eeScheduler().currentVSyncTick();
    const bool needsLatch = !s_hasLatchedInitialFrame || currentTick != s_lastPresentationTick;
    if (needsLatch)
    {
        rt->gs().latchHostPresentationFrame();
        s_lastPresentationTick = currentTick;
        s_hasLatchedInitialFrame = true;
    }
    else if (s_hasUploadedFrame)
    {
        outWidth = (s_lastWidth != 0u) ? s_lastWidth : FB_WIDTH;
        outHeight = (s_lastHeight != 0u) ? s_lastHeight : DEFAULT_DISPLAY_HEIGHT;
        return;
    }

    s_scratch.clear();
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t displayFbp = 0u;
    uint32_t sourceFbp = 0u;
    bool usedPreferredDisplaySource = false;
    if (!rt->gs().copyLatchedHostPresentationFrame(s_scratch,
                                                   width,
                                                   height,
                                                   &displayFbp,
                                                   &sourceFbp,
                                                   &usedPreferredDisplaySource))
    {
        static const std::vector<uint32_t> s_magenta(static_cast<size_t>(FB_WIDTH) * FB_HEIGHT, 0xFFFF00FFu);
        present::updatePicture(tex, s_magenta.data());
        outWidth = FB_WIDTH;
        outHeight = DEFAULT_DISPLAY_HEIGHT;
        s_lastWidth = outWidth;
        s_lastHeight = outHeight;
        s_hasUploadedFrame = true;
        return;
    }

    PS2_IF_AGRESSIVE_LOGS({
        static uint32_t s_uploadDebugCount = 0u;
        if (s_uploadDebugCount < 128u ||
            displayFbp != s_lastDisplayFbp ||
            sourceFbp != s_lastSourceFbp ||
            usedPreferredDisplaySource != s_lastPreferred ||
            width != s_lastWidth ||
            height != s_lastHeight)
        {
            std::cout << "[frame:upload] idx=" << s_uploadDebugCount
                      << " tick=" << currentTick
                      << " displayFbp=" << displayFbp
                      << " sourceFbp=" << sourceFbp
                      << " size=" << width << "x" << height
                      << " preferred=" << static_cast<uint32_t>(usedPreferredDisplaySource ? 1u : 0u)
                      << std::endl;
        }
        ++s_uploadDebugCount;
    });
    s_lastDisplayFbp = displayFbp;
    s_lastSourceFbp = sourceFbp;
    s_lastPreferred = usedPreferredDisplaySource;
    s_lastWidth = width;
    s_lastHeight = height;

    std::fill(s_uploadBuffer.begin(), s_uploadBuffer.end(), 0u);
    if (!s_scratch.empty() && width != 0u && height != 0u)
    {
        const uint32_t copyWidth = std::min<uint32_t>(width, FB_WIDTH);
        const uint32_t copyHeight = std::min<uint32_t>(height, FB_HEIGHT);
        const size_t srcRowBytes = static_cast<size_t>(width) * 4u;
        const size_t dstRowBytes = static_cast<size_t>(FB_WIDTH) * 4u;
        const size_t copyRowBytes = static_cast<size_t>(copyWidth) * 4u;
        for (uint32_t y = 0; y < copyHeight; ++y)
        {
            const size_t srcOffset = static_cast<size_t>(y) * srcRowBytes;
            const size_t dstOffset = static_cast<size_t>(y) * dstRowBytes;
            if (srcOffset + copyRowBytes > s_scratch.size() ||
                dstOffset + copyRowBytes > s_uploadBuffer.size())
            {
                break;
            }
            std::memcpy(s_uploadBuffer.data() + dstOffset, s_scratch.data() + srcOffset, copyRowBytes);
        }
    }

    present::updatePicture(tex, s_uploadBuffer.data());
    outWidth = width;
    outHeight = height;
    s_hasUploadedFrame = true;
}

PS2Runtime::PS2Runtime()
{
    m_iopHost = std::make_unique<PS2IopHostAdapter>(*this);
    m_iopSubsystem = std::make_unique<ps2x::iop::IopSubsystem>(*m_iopHost);

    m_eeScheduler = std::make_unique<EeScheduler>(*this);
    { // [gta-recomp] inline dispatch / checkpoint fast paths (ps2_runtime.h); PS2X_FAST_DISPATCH=0 turns them off
        const char *fast = std::getenv("PS2X_FAST_DISPATCH");
        m_eeClock = (fast && fast[0] == '0') ? nullptr : &m_eeScheduler->fastClock();
    }

    // Assign rather than memset: R5900Context's constructor zeroes itself and
    // then applies the COP0 reset values, which a memset here would discard.
    m_cpuContext = R5900Context{};

    // R0 is always zero in MIPS
    m_cpuContext.r[0] = _mm_set1_epi32(0);
    m_cpuContext.vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    m_cpuContext.vu0_q = 1.0f;
    m_cpuContext.vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3F800000));

    // Stack pointer (SP) and global pointer (GP) will be set by the loaded ELF

    m_loadedModules.clear();
    m_guestHeapBlocks.clear();
    m_guestHeapBase = kGuestHeapDefaultBase;
    m_guestHeapEnd = kGuestHeapDefaultBase;
    m_guestHeapLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    m_guestHeapSuggestedBase = kGuestHeapDefaultBase;
    m_guestHeapConfigured = false;
    // [gta-recomp] Interrupt/callback stacks live in the kernel-reserved area below
    // the ELF (0x80000..0x100000). Games put their main stack at the top of RAM
    // (VCS: SetupThread(0x1FE0000, 0x20000)), so carving from there clobbered it.
    m_asyncCallbackStackFloor = kAsyncCallbackStackFloor;
    m_asyncCallbackStackTop = kAsyncCallbackStackTop;
}

void PS2Runtime::setDebugUiCallbacks(DebugUiCallback initCallback,
                                     DebugUiCallback drawCallback,
                                     DebugUiCallback shutdownCallback,
                                     void *userData)
{
    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }

    m_debugUiInitCallback = initCallback;
    m_debugUiDrawCallback = drawCallback;
    m_debugUiShutdownCallback = shutdownCallback;
    m_debugUiUserData = userData;
}

PS2Runtime::~PS2Runtime()
{
    try
    {
        requestStop();
        m_iopSubsystem.reset();
        m_iopHost.reset();
#if defined(PLATFORM_VITA)
        m_audioBackend.stopAll();
        m_audioBackend.setAudioReady(false);
#else
        ps2AudioShutdown(); // [gta-recomp]
        m_audioBackend.setAudioReady(false);
#endif
        if (m_debugUiInitialized && m_debugUiShutdownCallback)
        {
            m_debugUiShutdownCallback(*this, m_debugUiUserData);
            m_debugUiInitialized = false;
        }

        if (platform::ready())
        {
            present::shutdown();
            platform::shutdown();
        }

        m_loadedModules.clear();
    }
    catch (const std::exception &e)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: unknown" << std::endl;
    }
}

namespace
{
    int32_t nextNativeModuleId() // [gta-recomp] module ids handed out while the emulated IOP is off
    {
        static std::atomic<int32_t> ids{0};
        return ++ids;
    }
}

ps2x::iop::ModuleLoadResult PS2Runtime::loadIopModule(std::string_view path, const void *arguments, uint32_t argumentSize)
{
    if (!ps2_iop_native::emulatedIop()) // [gta-recomp] no emulated IOP: the module "loads" and is never run
        return {true, nextNativeModuleId(), 0};
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->loadModule(path, arguments, argumentSize);
}

ps2x::iop::ModuleLoadResult PS2Runtime::loadIopModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
{
    if (!ps2_iop_native::emulatedIop()) // [gta-recomp]
        return {true, nextNativeModuleId(), 0};
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->loadModuleBuffer(guestAddress, arguments, argumentSize);
}

bool PS2Runtime::stopIopModule(int32_t moduleId, int32_t *result)
{
    if (!ps2_iop_native::emulatedIop()) // [gta-recomp] nothing runs: the module "stops"
    {
        if (result)
            *result = 0;
        return true;
    }
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->stopModule(moduleId, result);
}

ps2x::iop::RpcAbi PS2Runtime::selectIopRpcAbi(const ps2x::iop::RpcAbiRequest &request) const
{
    return m_iopSubsystem->selectRpcAbi(request);
}

bool PS2Runtime::canBindIopRpc(uint32_t sid) const noexcept
{
    if (ps2_iop_native::hasServer(sid)) // [gta-recomp] native servers (runtime/ps2_iop_native.h)
        return true;
    return ps2_iop_native::emulatedIop() && m_iopSubsystem->canBindRpc(sid);
}

ps2x::iop::RpcResult PS2Runtime::handleIopRpc(uint8_t *rdram, R5900Context *ctx, ps2x::iop::RpcRequest request)
{
    const bool recording = ps2_iop_native::recording(); // [gta-recomp] PS2X_RPC_RECORD: the call, then its reply
    if (recording)
        ps2_iop_native::record(rdram, request,
                               ps2_iop_native::emulatedIop() ? m_iopSubsystem->debugSnapshot().emulatorCycles * 8u
                                                             : ps2_iop_native::eeCycles());
    const auto handle = [&]() -> ps2x::iop::RpcResult
    {
        ps2x::iop::RpcResult native; // [gta-recomp] a native server wins over the emulated IOP's
        if (ps2_iop_native::dispatch(rdram, request, native))
            return native;
        if (!ps2_iop_native::emulatedIop())
        {
            static int reported = 0;
            if (reported++ < 16)
                std::fprintf(stderr, "[iop-native] no native server for sid 0x%x (fn 0x%x) with the emulated IOP off\n",
                             request.sid, request.function);
            return {};
        }
        auto scope = m_iopHost->enterCall(ctx, rdram);
        request.callToken = scope.token();
        return m_iopSubsystem->handleRpc(request);
    };
    const ps2x::iop::RpcResult result = handle();
    if (recording)
        ps2_iop_native::recordReply(rdram, request);
    return result;
}

void PS2Runtime::notifyIopSifTransfer(uint8_t *rdram, const ps2x::iop::SifTransfer &transfer)
{
    auto scope = m_iopHost->enterCall(nullptr, rdram);
    m_iopSubsystem->onSifTransfer(transfer);
}

void PS2Runtime::advanceIopEeCycles(uint64_t eeCycles) noexcept
{
    // [gta-recomp] Batch IOP time: the EE scheduler accounts cycles at every guest call, and running
    // the IOP scheduler (thread scan, timers, DMA) that often was a third of the frame. ~14 us of
    // emulated time per batch is far below anything the game can observe (RPCs are synchronous).
    static constexpr uint64_t kIopBatchEeCycles = 4096u;
    m_iopPendingEeCycles += eeCycles;
    if (m_iopPendingEeCycles < kIopBatchEeCycles)
        return;
    uint64_t cycles = m_iopPendingEeCycles;
    m_iopPendingEeCycles = 0u;

    // [gta-recomp] Keep IOP time (and so the SPU2 sample clock) up with the wall clock. On hardware the IOP and SPU2
    // run in real time however slowly the EE game loop goes; here IOP time is derived from accounted EE cycles, so
    // whenever the EE fell below full speed the SPU2 produced too few samples and the audio stuttered. Each batch
    // brings IOP time up to the host clock (at most 50 ms at once); a lag over 1 s (a stall, a debugger) is
    // forgiven rather than replayed. PS2X_IOP_REALTIME=0 restores pure EE-derived time.
    static const bool realtime = []
    {
        const char *v = std::getenv("PS2X_IOP_REALTIME");
        return !(v && v[0] == '0');
    }();
    if (realtime)
    {
        using Clock = std::chrono::steady_clock;
        static constexpr uint64_t kEeHz = 294912000u;
        static constexpr uint64_t kMaxExtra = kEeHz / 20u; // 50 ms per batch
        static constexpr uint64_t kMaxLag = kEeHz;         // 1 s
        static Clock::time_point origin = ps2x::hostNow();
        static uint64_t given = 0u; // EE-cycle equivalents handed to the IOP since `origin`
        const auto hostNs = std::chrono::duration_cast<std::chrono::nanoseconds>(ps2x::hostNow() - origin).count();
        const uint64_t hostCycles = static_cast<uint64_t>(static_cast<double>(hostNs) * (kEeHz / 1e9));
        given += cycles;
        if (hostCycles > given + kMaxLag)
            given = hostCycles - kMaxLag;
        if (hostCycles > given)
        {
            const uint64_t extra = std::min<uint64_t>(hostCycles - given, kMaxExtra);
            cycles += extra;
            given += extra;
        }
    }
    ps2perf::PhaseScope iopTime(ps2perf::Phase::Iop); // [gta-recomp]
    ps2_iop_native::tick(cycles);                     // [gta-recomp] native IOP-side time (runtime/ps2_iop_native.h)
    if (ps2_iop_native::emulatedIop())
        m_iopSubsystem->runEeCycles(cycles);
}

void PS2Runtime::resetIop()
{
    m_iopSubsystem->reset();
}

ps2x::iop::DebugSnapshot PS2Runtime::iopDebugSnapshot() const
{
    return m_iopSubsystem->debugSnapshot();
}

uint32_t PS2Runtime::allocateIopMemory(uint32_t size, uint32_t alignment)
{
    return m_iopSubsystem ? m_iopSubsystem->allocateMemory(size, alignment) : 0u;
}

bool PS2Runtime::freeIopMemory(uint32_t address)
{
    return m_iopSubsystem && m_iopSubsystem->freeMemory(address);
}

bool PS2Runtime::readIopMemory(uint32_t address, void *destination, size_t size) const
{
    return m_iopSubsystem && m_iopSubsystem->readMemory(address, destination, size);
}

bool PS2Runtime::writeIopMemory(uint32_t address, const void *source, size_t size)
{
    return m_iopSubsystem && m_iopSubsystem->writeMemory(address, source, size);
}

bool PS2Runtime::zeroIopMemory(uint32_t address, size_t size)
{
    return m_iopSubsystem && m_iopSubsystem->zeroMemory(address, size);
}

bool PS2Runtime::isIopMemoryRange(uint32_t address, size_t size) const
{
    return m_iopSubsystem && m_iopSubsystem->isMemoryRange(address, size);
}

bool PS2Runtime::syncCoreSubsystems()
{
    uint8_t *const rdram = m_memory.getRDRAM();
    uint8_t *const gsVram = m_memory.getGSVRAM();
    if (!rdram || !gsVram)
    {
        return false;
    }

    if (m_boundRdram == rdram && m_boundGSVram == gsVram)
    {
        return true;
    }

    m_gs.init(gsVram, static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &m_memory.gs());
    ps2hle::setGs(&m_gs); // [gta-recomp] packet-level HLE draws through it
    // [gta-recomp] GPU GS renderer: PS2X_GS=gpu (default) | cpu | both
    if (!m_gpu)
    {
        const char *gsEnv = std::getenv("PS2X_GS");
        if (gsEnv && std::strcmp(gsEnv, "hw") == 0 && !m_hw)
        {
            auto hw = makeHwRasterBackend(GSHwOptions::fromEnvironment());
            m_hw = hw.get();
            g_presentHw = m_hw; // [gta-recomp] ps2_present.h
            g_presentMemory = &m_memory;
            m_gs.setRasterBackend(std::move(hw));
            std::cerr << "[gs] hardware GS renderer (" << rhi::backendName(static_cast<rhi::Backend>(present::backend())) << ")" << std::endl; // [gta-recomp]
        }
        const GSRendererMode mode = (gsEnv && std::strcmp(gsEnv, "hw") == 0) ? GSRendererMode::Cpu : gsRendererModeFromEnvironment();
        if (mode != GSRendererMode::Cpu)
        {
            std::string whyNot;
            m_gpu = mode == GSRendererMode::Null ? makeNullGpuRenderer()
                                                 : makeGpuRenderer(GSGpuOptions::fromEnvironment(), whyNot);
            if (m_gpu)
                std::cerr << "[gs] GPU renderer: " << m_gpu->deviceName() << " (" << gsRendererModeName(mode) << ")" << std::endl;
            else
                std::cerr << "[gs] GPU renderer unavailable (" << whyNot << "), using the CPU renderer" << std::endl;
        }
        m_gs.setGpuRenderer(m_gpu.get(), mode == GSRendererMode::Gpu || mode == GSRendererMode::Null);
        // [gta-recomp] render submission on its own thread with the hardware renderer (runtime/ps2_render_worker.h)
        ps2_render_worker::configure(m_hw != nullptr);
        // [gta-recomp] and the GS stage behind it (the hardware renderer only; not while the HLE verify / survey modes
        // read what the GS recorded from the worker)
        ps2_render_worker::configureGsStage(m_hw != nullptr && !m_gpu && !std::getenv("PS2X_HLE_VERIFY") &&
                                            !std::getenv("PS2X_HLE_SURVEY"));
    }
    m_gifArbiter.setProcessPacketFn([this](const uint8_t *data, uint32_t size)
                                    { m_gs.processGIFPacket(data, size); });
    m_memory.setGifArbiter(&m_gifArbiter);
    // [gta-recomp] the same processing for a PATH2 packet handled in place (PS2Memory::submitGifPacket)
    m_memory.setGifPacketCallback([this](const uint8_t *data, uint32_t size) { m_gs.processGIFPacket(data, size); });
    m_memory.setVu1MscalCallback([this](uint32_t startPC, uint32_t top, uint32_t itop)
                                 {
                                     // [gta-recomp] On the render worker the game thread's context is not ours to read or
                                     // write (the scheduler's thread table, FBRST, VPU-STAT): VU1 runs without D / T stops
                                     // there - VCS never enables them.
                                     R5900Context *cpuContext = nullptr;
                                     if (!ps2_render_worker::onWorker())
                                     {
                                         cpuContext = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                                         if (!cpuContext)
                                         {
                                             cpuContext = &m_cpuContext;
                                         }
                                     }
                                     m_vu1.state().dBitEnabled =
                                         cpuContext && (cpuContext->vu0_fbrst & (1u << 10)) != 0u;
                                     m_vu1.state().tBitEnabled =
                                         cpuContext && (cpuContext->vu0_fbrst & (1u << 11)) != 0u;
                                     { // [gta-recomp] PS2X_VU1_DUMP=418,478,...: dump VU1 code+data before the 50th run of these programs
                                         static std::map<uint32_t, uint32_t> dumpRuns = []
                                         {
                                             std::map<uint32_t, uint32_t> m;
                                             if (const char *e = std::getenv("PS2X_VU1_DUMP"))
                                                 for (const char *p = e; *p;)
                                                 {
                                                     char *end = nullptr;
                                                     const unsigned long v = std::strtoul(p, &end, 16);
                                                     if (end == p)
                                                         break;
                                                     m[static_cast<uint32_t>(v)] = 0u;
                                                     p = *end ? end + 1 : end;
                                                 }
                                             return m;
                                         }();
                                         auto it = dumpRuns.find(startPC);
                                         if (it != dumpRuns.end() && ++it->second == 50u)
                                         {
                                             char name[64];
                                             std::snprintf(name, sizeof(name), "vu1_code_%04x.bin", startPC);
                                             if (FILE *f = std::fopen(name, "wb")) { std::fwrite(m_memory.getVU1Code(), 1, PS2_VU1_CODE_SIZE, f); std::fclose(f); }
                                             std::snprintf(name, sizeof(name), "vu1_data_%04x.bin", startPC);
                                             if (FILE *f = std::fopen(name, "wb")) { std::fwrite(m_memory.getVU1Data(), 1, PS2_VU1_DATA_SIZE, f); std::fclose(f); }
                                             std::fprintf(stderr, "[vu1] dumped program 0x%04x (top=0x%x itop=0x%x)\n", startPC, top, itop);
                                         }
                                     }
                                     const uint64_t kicksBefore = g_vu1XgkickCount;
                                     gsSetPath1Producer(startPC, false); // [gta-recomp] draw-source counters
                                     m_vu1.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                   m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                   m_gs, &m_memory, startPC, top, itop, 65536);
                                     traceVu1Program(startPC, g_vu1XgkickCount - kicksBefore, m_vu1.state().pc);
                                     ps2perf::add(ps2perf::Counter::Vu1Programs); // [gta-recomp]
                                     if (cpuContext)
                                     cpuContext->vu0_vpu_stat =
                                         (cpuContext->vu0_vpu_stat & ~0x0600u) |
                                         (m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                                         (m_vu1.state().stoppedByT ? 0x0400u : 0u); });
    m_memory.setVu1MscntCallback([this](uint32_t top, uint32_t itop)
                                 {
                                     // [gta-recomp] On the render worker the game thread's context is not ours to read or
                                     // write (the scheduler's thread table, FBRST, VPU-STAT): VU1 runs without D / T stops
                                     // there - VCS never enables them.
                                     R5900Context *cpuContext = nullptr;
                                     if (!ps2_render_worker::onWorker())
                                     {
                                         cpuContext = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                                         if (!cpuContext)
                                         {
                                             cpuContext = &m_cpuContext;
                                         }
                                     }
                                     m_vu1.state().dBitEnabled =
                                         cpuContext && (cpuContext->vu0_fbrst & (1u << 10)) != 0u;
                                     m_vu1.state().tBitEnabled =
                                         cpuContext && (cpuContext->vu0_fbrst & (1u << 11)) != 0u;
                                     gsSetPath1Producer(m_vu1.state().pc, false); // [gta-recomp] draw-source counters
                                     m_vu1.resume(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                  m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                  m_gs, &m_memory, top, itop, 65536);
                                     if (cpuContext)
                                     cpuContext->vu0_vpu_stat =
                                         (cpuContext->vu0_vpu_stat & ~0x0600u) |
                                         (m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                                         (m_vu1.state().stoppedByT ? 0x0400u : 0u); });
    resetIop();
    m_vu0.reset();
    m_vu1.reset();

    m_boundRdram = rdram;
    m_boundGSVram = gsVram;
    return true;
}

bool PS2Runtime::initialize(const char *title)
{
    try
    {
        if (!m_memory.initialize())
        {
            std::cerr << "Failed to initialize PS2 memory" << std::endl;
            return false;
        }

        if (!syncCoreSubsystems())
        {
            std::cerr << "Failed to bind runtime core subsystems" << std::endl;
            return false;
        }
#if defined(PLATFORM_VITA)
        InitWindow(HOST_WINDOW_WIDTH, HOST_WINDOW_HEIGHT, title); // raylib vita does not support audio
#else
        // [gta-recomp] Host window, input and audio through the platform layer (SDL3); PS2X_HIDDEN=1: no window on the
        // desktop (runtime/ps2_control.h). The window is drawn through the RHI (D3D11 / Vulkan, render/presenter.h);
        // host frames are paced to 60/s by platform::waitNextFrame().
        platform::WindowDesc window;
        window.title = title;
        window.width = HOST_WINDOW_WIDTH;
        window.height = HOST_WINDOW_HEIGHT;
        window.hidden = ps2control::hiddenMode();
        // [gta-recomp] PS2X_HIDDEN_WINDOW=WxH: a hidden window of that size (a player's window in a test: the screenshot key)
        if (const char *e = std::getenv("PS2X_HIDDEN_WINDOW"); e && window.hidden)
        {
            int w = 0, h = 0;
            if (std::sscanf(e, "%dx%d", &w, &h) == 2 && w > 0 && h > 0)
                window.width = w, window.height = h;
        }
        window.vulkan = present::backendNeedsVulkanWindow();
        if (!platform::init(window) || !present::init(window.width, window.height))
        {
            std::cerr << "Failed to create the window" << std::endl;
            return false;
        }
        // [gta-recomp] "burst DIR N": the next N presented frames as DIR/burst_NNNN.png, one line each in DIR/burst.txt (the
        // shape shown and the renderer's batch) - a transition frame by frame (screenshots manage one every ~50 ms)
        ps2control::addCommand("burst", [](const std::string &args, ps2control::Reply reply) {
            char dir[512] = {};
            int n = 0;
            if (std::sscanf(args.c_str(), "%511s %d", dir, &n) != 2 || n <= 0 || n > 600)
            {
                reply("error usage: burst DIR N (1..600)");
                return;
            }
            g_burst.dir = dir;
            g_burst.left = n;
            g_burst.index = 0;
            if (FILE *f = std::fopen((g_burst.dir + "/burst.txt").c_str(), "w"))
                std::fclose(f);
            reply("ok");
        });
        ps2control::start();
        ps2AudioInit(); // [gta-recomp] SPU2 output stream
        m_audioBackend.setAudioReady(platform::audioAvailable());
#endif
        if (m_debugUiInitCallback)
        {
            m_debugUiInitCallback(*this, m_debugUiUserData);
            m_debugUiInitialized = true;
        }

        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Failed to initialize PS2 runtime: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "Failed to initialize PS2 runtime: unknown exception" << std::endl;
    }

    return false;
}

bool PS2Runtime::loadELF(const std::string &elfPath)
{
    configureIoPathsFromElf(elfPath);

    std::ifstream file(elfPath, std::ios::binary);
    if (!file)
    {
        std::cerr << "Failed to open ELF file: " << elfPath << std::endl;
        return false;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    if (fileSize < static_cast<std::streamoff>(sizeof(ElfHeader)))
    {
        std::cerr << "ELF file is too small: " << elfPath << std::endl;
        return false;
    }
    file.seekg(0, std::ios::beg);

    ElfHeader header{};
    if (!file.read(reinterpret_cast<char *>(&header), sizeof(header)))
    {
        std::cerr << "Failed to read ELF header from: " << elfPath << std::endl;
        return false;
    }

    if (header.magic != ELF_MAGIC)
    {
        std::cerr << "Invalid ELF magic number" << std::endl;
        return false;
    }

    if (header.elf_class != 1u || header.endianness != 1u)
    {
        std::cerr << "Unsupported ELF format (expected 32-bit little-endian)." << std::endl;
        return false;
    }

    if (header.machine != EM_MIPS || header.type != ET_EXEC)
    {
        std::cerr << "Not a MIPS executable ELF file" << std::endl;
        return false;
    }

    if (header.phnum != 0u && header.phentsize < sizeof(ProgramHeader))
    {
        std::cerr << "Unsupported ELF program-header entry size: " << header.phentsize << std::endl;
        return false;
    }

    const uint64_t programHeaderTableEnd =
        static_cast<uint64_t>(header.phoff) +
        static_cast<uint64_t>(header.phnum) * static_cast<uint64_t>(header.phentsize);
    if (programHeaderTableEnd > static_cast<uint64_t>(fileSize))
    {
        std::cerr << "ELF program-header table is out of range." << std::endl;
        return false;
    }

    m_cpuContext.pc = header.entry;
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);

    uint32_t maxLoadedRdramEnd = kGuestHeapDefaultBase;
    uint32_t moduleBase = std::numeric_limits<uint32_t>::max();
    uint32_t moduleEnd = 0u;
    bool loadedAnySegment = false;

    for (uint16_t i = 0; i < header.phnum; i++)
    {
        const uint64_t phOffset =
            static_cast<uint64_t>(header.phoff) +
            static_cast<uint64_t>(i) * static_cast<uint64_t>(header.phentsize);
        if (phOffset + sizeof(ProgramHeader) > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF program header " << i << " is out of range." << std::endl;
            return false;
        }

        ProgramHeader ph{};
        file.seekg(static_cast<std::streamoff>(phOffset), std::ios::beg);
        if (!file.read(reinterpret_cast<char *>(&ph), sizeof(ph)))
        {
            std::cerr << "Failed to read ELF program header " << i << std::endl;
            return false;
        }

        if (ph.type != PT_LOAD || ph.memsz == 0u)
        {
            continue;
        }

        if (ph.filesz > ph.memsz)
        {
            std::cerr << "ELF segment " << i << " has filesz > memsz." << std::endl;
            return false;
        }

        const uint64_t segmentFileEnd = static_cast<uint64_t>(ph.offset) + static_cast<uint64_t>(ph.filesz);
        if (segmentFileEnd > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF segment " << i << " exceeds file bounds." << std::endl;
            return false;
        }

        const bool scratch =
            ph.vaddr >= PS2_SCRATCHPAD_BASE &&
            ph.vaddr < (PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE);

        uint32_t physAddr = 0u;
        try
        {
            physAddr = m_memory.translateAddress(ph.vaddr);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Failed to translate ELF segment " << i
                      << " virtual address 0x" << std::hex << ph.vaddr
                      << std::dec << ": " << e.what() << std::endl;
            return false;
        }
        const uint64_t regionSize = scratch ? static_cast<uint64_t>(PS2_SCRATCHPAD_SIZE)
                                            : static_cast<uint64_t>(PS2_RAM_SIZE);
        const uint64_t segmentMemEnd = static_cast<uint64_t>(physAddr) + static_cast<uint64_t>(ph.memsz);
        if (segmentMemEnd > regionSize)
        {
            std::cerr << "ELF segment " << i << " exceeds "
                      << (scratch ? "scratchpad" : "RDRAM")
                      << " bounds (vaddr=0x" << std::hex << ph.vaddr
                      << " memsz=0x" << ph.memsz << std::dec << ")." << std::endl;
            return false;
        }

        uint8_t *destBase = scratch ? m_memory.getScratchpad() : m_memory.getRDRAM();
        if (!destBase)
        {
            std::cerr << "ELF segment " << i << " has no destination memory backing." << std::endl;
            return false;
        }

        uint8_t *dest = destBase + physAddr;
        if (ph.filesz > 0u)
        {
            file.seekg(static_cast<std::streamoff>(ph.offset), std::ios::beg);
            if (!file.read(reinterpret_cast<char *>(dest), ph.filesz))
            {
                std::cerr << "Failed to read ELF segment " << i << " payload." << std::endl;
                return false;
            }
        }

        if (ph.memsz > ph.filesz)
        {
            std::memset(dest + ph.filesz, 0, ph.memsz - ph.filesz);
        }

        RUNTIME_LOG("Loading segment: 0x" << std::hex << ph.vaddr
                                          << " - 0x" << (static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz))
                                          << " (filesz: 0x" << ph.filesz
                                          << ", memsz: 0x" << ph.memsz << ")"
                                          << std::dec << std::endl);

        if (!scratch)
        {
            maxLoadedRdramEnd = std::max(maxLoadedRdramEnd, static_cast<uint32_t>(segmentMemEnd));
        }

        if (ph.flags & 0x1u) // PF_X
        {
            const uint64_t execEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.filesz);
            if (execEnd <= std::numeric_limits<uint32_t>::max())
            {
                m_memory.registerCodeRegion(ph.vaddr, static_cast<uint32_t>(execEnd));
            }
        }

        loadedAnySegment = true;
        moduleBase = std::min(moduleBase, ph.vaddr);
        const uint64_t segmentVirtualEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz);
        const uint32_t clampedVirtualEnd =
            (segmentVirtualEnd > std::numeric_limits<uint32_t>::max())
                ? std::numeric_limits<uint32_t>::max()
                : static_cast<uint32_t>(segmentVirtualEnd);
        moduleEnd = std::max(moduleEnd, clampedVirtualEnd);
    }

    if (!loadedAnySegment)
    {
        std::cerr << "ELF contains no loadable PT_LOAD segments." << std::endl;
        return false;
    }

    if (maxLoadedRdramEnd > PS2_RAM_SIZE)
    {
        maxLoadedRdramEnd = PS2_RAM_SIZE;
    }

    const uint32_t paddedEnd = (maxLoadedRdramEnd > (PS2_RAM_SIZE - kGuestHeapSafetyPad))
                                   ? PS2_RAM_SIZE
                                   : (maxLoadedRdramEnd + kGuestHeapSafetyPad);
    const uint32_t suggestedHeapBase = alignGuestHeapValue(paddedEnd, kGuestHeapDefaultAlignment);
    {
        std::lock_guard<std::mutex> lock(m_guestHeapMutex);
        if (!m_guestHeapConfigured)
        {
            const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
            m_guestHeapSuggestedBase = std::min(suggestedHeapBase, hardLimit);
            m_guestHeapBase = m_guestHeapSuggestedBase;
            m_guestHeapEnd = m_guestHeapSuggestedBase;
            m_guestHeapLimit = hardLimit;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
        const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
        (void)hardLimit;
        m_asyncCallbackStackFloor = kAsyncCallbackStackFloor; // [gta-recomp] see reset
        m_asyncCallbackStackTop = kAsyncCallbackStackTop;
    }

    LoadedModule module;
    module.name = elfPath.substr(elfPath.find_last_of("/\\") + 1);
    module.baseAddress = (moduleBase == std::numeric_limits<uint32_t>::max()) ? 0x00100000u : moduleBase;
    module.size = (moduleEnd > module.baseAddress) ? static_cast<size_t>(moduleEnd - module.baseAddress) : 0u;
    module.active = true;

    m_loadedModules.push_back(module);

    uint32_t elfCrc32 = 0u;
    const bool elfCrc32Valid = computeFileCrc32(elfPath, elfCrc32);
    if (!elfCrc32Valid)
    {
        std::cerr << "[ps2xIOP] failed to compute ELF CRC32 for '" << elfPath << "'" << std::endl;
    }
    ps2x::iop::GameIdentity identity;
    identity.elfName = module.name;
    identity.entryPoint = m_cpuContext.pc;
    identity.crc32 = elfCrc32;
    std::string romError;
    if (!m_romDevice.configure(identity, &romError))
    {
        std::cerr << "[ROM0] failed to configure profile: " << romError << std::endl;
        return false;
    }

    m_iopSubsystem->reset();

    ps2_game_overrides::applyMatching(*this,
                                      elfPath,
                                      m_cpuContext.pc,
                                      elfCrc32,
                                      elfCrc32Valid);

    RUNTIME_LOG("ELF file loaded successfully. Entry point: 0x" << std::hex << m_cpuContext.pc << std::dec);
    return true;
}

const PS2Runtime::IoPaths &PS2Runtime::getIoPaths()
{
    return runtimeIoPaths();
}

void PS2Runtime::setIoPaths(const IoPaths &paths)
{
    IoPaths normalized = paths;
    normalized.elfPath = normalizeAbsolutePath(normalized.elfPath);
    normalized.elfDirectory = normalizeAbsolutePath(normalized.elfDirectory);
    normalized.hostRoot = normalizeAbsolutePath(normalized.hostRoot);
    normalized.cdRoot = normalizeAbsolutePath(normalized.cdRoot);
    normalized.mcRoot = normalizeAbsolutePath(normalized.mcRoot);
    normalized.cdImage = normalizeAbsolutePath(normalized.cdImage);

    if (normalized.elfDirectory.empty() && !normalized.elfPath.empty())
    {
        normalized.elfDirectory = normalized.elfPath.parent_path();
    }

    if (normalized.hostRoot.empty())
    {
        normalized.hostRoot = normalized.elfDirectory;
    }
    if (normalized.cdRoot.empty())
    {
        normalized.cdRoot = normalized.elfDirectory;
    }
    if (normalized.mcRoot.empty())
    {
        normalized.mcRoot = normalized.elfDirectory / "mc0";
    }

    runtimeIoPaths() = normalized;
}

void PS2Runtime::configureIoPathsFromElf(const std::string &elfPath)
{
    IoPaths paths = runtimeIoPaths();
    paths.elfPath = normalizeAbsolutePath(std::filesystem::path(elfPath));
    if (!paths.elfPath.empty())
    {
        paths.elfDirectory = paths.elfPath.parent_path();
    }

    if (!paths.elfDirectory.empty())
    {
        paths.hostRoot = paths.elfDirectory;
        paths.cdRoot = paths.elfDirectory;
        paths.mcRoot = paths.elfDirectory / "mc0";
    }

    setIoPaths(paths);
}

namespace
{
    bool generatedFunctionTableSlot(uint32_t address, uint32_t &slot)
    {
        if ((address & 3u) != 0u || g_ps2RecompiledFunctionTableSlotCount == 0u)
        {
            return false;
        }

        if (address < g_ps2RecompiledFunctionTableBase || address >= g_ps2RecompiledFunctionTableEnd)
        {
            return false;
        }

        const uint32_t offset = address - g_ps2RecompiledFunctionTableBase;
        slot = offset >> 2;
        return slot < g_ps2RecompiledFunctionTableSlotCount;
    }
}

bool PS2Runtime::replaceFunction(uint32_t address, RecompiledFunction func)
{
    uint32_t slot = 0u;
    if (!generatedFunctionTableSlot(address, slot))
    {
        std::cerr << "[function-table] cannot replace guest PC 0x" << std::hex << address
                  << ": outside generated dense table [0x" << g_ps2RecompiledFunctionTableBase
                  << ", 0x" << g_ps2RecompiledFunctionTableEnd << ")"
                  << std::dec << std::endl;
        return false;
    }

    g_ps2RecompiledFunctionTable[slot] = func;
    return true;
}

bool PS2Runtime::registerFunction(uint32_t address, RecompiledFunction func)
{
    return replaceFunction(address, func);
}

bool PS2Runtime::hasFunction(uint32_t address) const
{
    uint32_t slot = 0u;
    return generatedFunctionTableSlot(address, slot) && g_ps2RecompiledFunctionTable[slot] != nullptr;
}

const char *describeGuestBranchKind(PS2Runtime::GuestBranchKind kind)
{
    switch (kind)
    {
    case PS2Runtime::GuestBranchKind::DirectJump:
        return "DirectJump";
    case PS2Runtime::GuestBranchKind::DirectCall:
        return "DirectCall";
    case PS2Runtime::GuestBranchKind::IndirectJump:
        return "IndirectJump";
    case PS2Runtime::GuestBranchKind::IndirectCall:
        return "IndirectCall";
    case PS2Runtime::GuestBranchKind::Return:
        return "Return";
    default:
        return "Unknown";
    }
}

PS2Runtime::RecompiledFunction PS2Runtime::lookupFunction(uint32_t address)
{
    pushDispatchPc(address);

    uint32_t slot = 0u;
    if (generatedFunctionTableSlot(address, slot))
    {
        RecompiledFunction fn = g_ps2RecompiledFunctionTable[slot];
        if (fn != nullptr)
        {
            return fn;
        }
    }

    std::cerr << "Error: No exact recompiled function for guest PC 0x" << std::hex << address
              << " tableBase=0x" << g_ps2RecompiledFunctionTableBase
              << " tableEnd=0x" << g_ps2RecompiledFunctionTableEnd
              << " codeRegion=" << (m_memory.isCodeAddress(address) ? "yes" : "no")
              << " trace=" << formatDispatchHistory()
              << std::dec << std::endl;

    // [gta-recomp] a named function, not a lambda: lambdas convert only to the platform's calling convention
    struct Missing
    {
        static PS2_GUEST_CC void function(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t badPc = ctx->pc;
            runtime->reportMissingFunction(rdram,
                                           ctx,
                                           badPc,
                                           0u,
                                           PS2Runtime::GuestBranchKind::IndirectJump,
                                           "dispatch");
        }
    };

    return &Missing::function;
}

void PS2Runtime::setMissingFunctionPolicy(MissingFunctionPolicy policy)
{
    m_missingFunctionPolicy.store(static_cast<uint32_t>(policy), std::memory_order_release);
}

PS2Runtime::MissingFunctionPolicy PS2Runtime::missingFunctionPolicy() const
{
    return static_cast<MissingFunctionPolicy>(m_missingFunctionPolicy.load(std::memory_order_acquire));
}

void PS2Runtime::resetMissingFunctionReportOnce()
{
    m_missingFunctionReported.store(false, std::memory_order_release);
}

void PS2Runtime::reportMissingFunction(uint8_t *rdram,
                                       R5900Context *ctx,
                                       uint32_t targetPc,
                                       uint32_t sourcePc,
                                       GuestBranchKind kind,
                                       const char *debugName)
{
    const MissingFunctionPolicy policy = missingFunctionPolicy();
    const bool firstReport = !m_missingFunctionReported.exchange(true, std::memory_order_acq_rel);

    const uint32_t pc = ctx->pc;
    const uint32_t ra = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
    const uint32_t sp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0));
    const uint32_t gp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0));
    const uint32_t a0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0));
    const uint32_t a1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[5], 0));
    const uint32_t a2 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[6], 0));
    const uint32_t a3 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[7], 0));
    const uint32_t s0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[16], 0));
    const uint32_t s1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[17], 0));
    const uint32_t v0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[2], 0));
    const uint32_t v1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[3], 0));

    auto readGuestU32At = [rdram](uint32_t addr, uint32_t &out) -> bool
    {
        // TODO this !rdram exist only because of test fix those test later
        if (!rdram || addr > PS2_RAM_SIZE - sizeof(uint32_t))
        {
            out = 0u;
            return false;
        }

        std::memcpy(&out, rdram + addr, sizeof(uint32_t));
        return true;
    };

    auto readGuestU32Offset = [&readGuestU32At](uint32_t base, uint32_t offset, uint32_t &out) -> bool
    {
        if (base > PS2_RAM_SIZE - sizeof(uint32_t) || offset > PS2_RAM_SIZE - sizeof(uint32_t) - base)
        {
            out = 0u;
            return false;
        }

        return readGuestU32At(base + offset, out);
    };

    uint32_t a0Word0 = 0u;
    uint32_t a0Word4 = 0u;
    uint32_t a0Word8 = 0u;
    uint32_t a0WordC = 0u;
    const bool a0Readable =
        readGuestU32Offset(a0, 0x00u, a0Word0) &&
        readGuestU32Offset(a0, 0x04u, a0Word4) &&
        readGuestU32Offset(a0, 0x08u, a0Word8) &&
        readGuestU32Offset(a0, 0x0cu, a0WordC);

    uint32_t s0Word0 = 0u;
    uint32_t s0Word4 = 0u;
    uint32_t s0Word8 = 0u;
    uint32_t s0WordC = 0u;
    const bool s0Readable =
        readGuestU32Offset(s0, 0x00u, s0Word0) &&
        readGuestU32Offset(s0, 0x04u, s0Word4) &&
        readGuestU32Offset(s0, 0x08u, s0Word8) &&
        readGuestU32Offset(s0, 0x0cu, s0WordC);

    uint32_t recordWord0 = 0u;
    uint32_t recordWord4 = 0u;
    uint32_t recordWord8 = 0u;
    uint32_t recordWordC = 0u;
    const bool recordReadable =
        s0Readable && s0Word4 != 0u &&
        readGuestU32Offset(s0Word4, 0x00u, recordWord0) &&
        readGuestU32Offset(s0Word4, 0x04u, recordWord4) &&
        readGuestU32Offset(s0Word4, 0x08u, recordWord8) &&
        readGuestU32Offset(s0Word4, 0x0cu, recordWordC);

    uint32_t vtableSlot0 = 0u;
    uint32_t vtableSlot4 = 0u;
    uint32_t vtableSlot8 = 0u;
    uint32_t vtableSlotC = 0u;
    const bool vtableReadable =
        a0Readable && a0Word0 != 0u &&
        readGuestU32Offset(a0Word0, 0x00u, vtableSlot0) &&
        readGuestU32Offset(a0Word0, 0x04u, vtableSlot4) &&
        readGuestU32Offset(a0Word0, 0x08u, vtableSlot8) &&
        readGuestU32Offset(a0Word0, 0x0cu, vtableSlotC);

    if (firstReport)
    {
        std::ostringstream oss;
        oss << "[guest-branch:missing-target] kind=" << describeGuestBranchKind(kind)
            << " op=" << (debugName ? debugName : "<unknown>")
            << " source=0x" << std::hex << sourcePc
            << " target=0x" << targetPc
            << " pc=0x" << pc
            << " ra=0x" << ra
            << " sp=0x" << sp
            << " gp=0x" << gp
            << " a0=0x" << a0
            << " a1=0x" << a1
            << " a2=0x" << a2
            << " a3=0x" << a3
            << " s0=0x" << s0
            << " s1=0x" << s1
            << " v0=0x" << v0
            << " v1=0x" << v1
            << " a0Readable=" << (a0Readable ? "yes" : "no")
            << " a0[0]=0x" << a0Word0
            << " a0[4]=0x" << a0Word4
            << " a0[8]=0x" << a0Word8
            << " a0[c]=0x" << a0WordC
            << " s0Readable=" << (s0Readable ? "yes" : "no")
            << " s0[0]=0x" << s0Word0
            << " s0[4]=0x" << s0Word4
            << " s0[8]=0x" << s0Word8
            << " s0[c]=0x" << s0WordC
            << " recordReadable=" << (recordReadable ? "yes" : "no")
            << " record[0]=0x" << recordWord0
            << " record[4]=0x" << recordWord4
            << " record[8]=0x" << recordWord8
            << " record[c]=0x" << recordWordC
            << " vtableReadable=" << (vtableReadable ? "yes" : "no")
            << " vtbl[0]=0x" << vtableSlot0
            << " vtbl[4]=0x" << vtableSlot4
            << " vtbl[8]=0x" << vtableSlot8
            << " vtbl[c]=0x" << vtableSlotC
            << " codeRegion=" << (m_memory.isCodeAddress(targetPc) ? "yes" : "no")
            << " policy=" << static_cast<uint32_t>(policy)
            << " trace=" << formatDispatchHistory();
        // [gta-recomp] a corrupted saved $ra: show the stack slots just below $sp holding the bad target
        oss << " stackHits=";
        for (uint32_t off = 8u; off <= 0x400u; off += 4u)
        {
            uint32_t w = 0u;
            if (readGuestU32Offset(sp - off, 0u, w) && w == targetPc)
                oss << "0x" << (sp - off) << ",";
        }
        oss << std::dec;

        static std::mutex s_missingFunctionLogMutex;
        {
            std::lock_guard<std::mutex> lock(s_missingFunctionLogMutex);
            std::cerr << oss.str() << std::endl;
        }
    }

    if (firstReport && policy == MissingFunctionPolicy::BreakOnce)
    {
#if defined(_MSC_VER)
        __debugbreak();
#elif defined(__clang__)
        __builtin_debugtrap(); // [gta-recomp] Linux: SIGTRAP, stops in a debugger
#endif
    }

    if (ctx)
    {
        ctx->pc = targetPc;
    }

    if (policy == MissingFunctionPolicy::Stop)
    {
        requestStop();
    }
}

bool PS2Runtime::dispatchGuestBranchSlow(uint8_t *rdram,
                                     R5900Context *ctx,
                                     uint32_t targetPc,
                                     uint32_t sourcePc,
                                     uint32_t fallthroughPc,
                                     GuestBranchKind kind,
                                     const char *debugName)
{
    ctx->pc = targetPc;
    const bool isCall = (kind == GuestBranchKind::DirectCall || kind == GuestBranchKind::IndirectCall);

    // Every inter-function transfer is also a deterministic EE safe point.
    // Backward edges inside generated functions use eeCheckpointDue(), while
    // this charge bounds straight-line call chains that have no local loop.
    if (m_eeScheduler && m_eeScheduler->checkpointDue(EeScheduler::kGuestDispatchCycles))
    {
        g_checkpointUnwind = true;
        return false;
    }

    if (!isCall)
    {
        if (!hasFunction(targetPc))
        {
            reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, debugName);
        }

        ctx->pc = targetPc;
        return false;
    }

    if (!hasFunction(targetPc))
    {
        reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, debugName);

        const MissingFunctionPolicy policy = missingFunctionPolicy();

        if (policy == MissingFunctionPolicy::SkipCallDebug && isCall)
        {
            ctx->pc = fallthroughPc;
            return true;
        }

        if (policy == MissingFunctionPolicy::ContinueToTarget)
        {
            ctx->pc = targetPc;
            // if you need the app to keep open to open debug pannel change this to false
            return false;
        }

        return false;
    }

    RecompiledFunction targetFn = lookupFunction(targetPc);
    const uint32_t entryPc = ctx->pc;
    targetFn(rdram, ctx, this);

    if (isStopRequested() || ctx->pc == 0u || g_checkpointUnwind)
    {
        return false;
    }

    if (ctx->pc == entryPc)
    {
        ctx->pc = fallthroughPc;
    }

    return ctx->pc == fallthroughPc;
}

void PS2Runtime::SignalException(R5900Context *ctx, PS2Exception exception)
{
    if (exception == EXCEPTION_INTEGER_OVERFLOW)
    {
        HandleIntegerOverflow(ctx);
        return;
    }

    raiseCop0Exception(ctx, static_cast<uint32_t>(exception),
                       exception == EXCEPTION_TLB_REFILL);
}

// ---- [gta-recomp] native VU0 programs (runtime/ps2_vu0_native.h) ----------------------------------------------
namespace
{
    struct Vu0NativeEntry
    {
        uint32_t startPc = 0;
        uint32_t rangeStart = 0; // [gta-recomp] the checked code: [rangeStart, rangeStart + codeBytes)
        uint32_t codeBytes = 0;
        uint64_t codeHash = 0;
        Ps2Vu0NativeFn fn = nullptr;
        const char *name = "";
        std::vector<uint8_t> verifiedCode; // the bytes once the hash matched: later checks are a memcmp
        uint64_t verifyRuns = 0, verifyMismatches = 0;
    };

    std::vector<Vu0NativeEntry> &vu0Natives()
    {
        static std::vector<Vu0NativeEntry> natives;
        return natives;
    }

    const Vu0NativeEntry *findVu0Native(uint32_t startPc, const uint8_t *vu0Code)
    {
        static const bool disabled = []
        {
            const char *e = std::getenv("PS2X_VU0_NATIVE");
            return e && e[0] == '0';
        }();
        if (disabled)
            return nullptr;
        for (Vu0NativeEntry &entry : vu0Natives())
        {
            if (entry.startPc != startPc || entry.rangeStart + entry.codeBytes > PS2_VU0_CODE_SIZE)
                continue;
            const uint8_t *code = vu0Code + entry.rangeStart;
            if (!entry.verifiedCode.empty())
            {
                if (std::memcmp(entry.verifiedCode.data(), code, entry.codeBytes) == 0)
                    return &entry;
                continue;
            }
            if (ps2Vu0CodeHash(code, entry.codeBytes) == entry.codeHash)
            {
                entry.verifiedCode.assign(code, code + entry.codeBytes);
                std::fprintf(stderr, "[vu0native] running %s natively (VU0 0x%04x, %u bytes)\n", entry.name, startPc, entry.codeBytes);
                return &entry;
            }
        }
        return nullptr;
    }

    // PS2X_VU0_VERIFY=1: native result (a) vs interpreter (b): VF1..31, VI1..15, ACC, VU0 data
    void verifyVu0Native(const Vu0NativeEntry &entryConst, const R5900Context &a, const uint8_t *aData, const R5900Context &b,
                         const uint8_t *bData)
    {
        Vu0NativeEntry &entry = const_cast<Vu0NativeEntry &>(entryConst);
        ++entry.verifyRuns;
        std::string what;
        char buf[160];
        for (int r = 1; r < 32 && what.empty(); ++r)
            if (std::memcmp(&a.vu0_vf[r], &b.vu0_vf[r], 16) != 0)
            {
                alignas(16) uint32_t x[4], y[4];
                std::memcpy(x, &a.vu0_vf[r], 16);
                std::memcpy(y, &b.vu0_vf[r], 16);
                std::snprintf(buf, sizeof(buf), "vf%d native %08x %08x %08x %08x interp %08x %08x %08x %08x", r, x[0], x[1], x[2], x[3], y[0],
                              y[1], y[2], y[3]);
                what = buf;
            }
        for (int r = 1; r < 16 && what.empty(); ++r)
            if (a.vi[r] != b.vi[r])
            {
                std::snprintf(buf, sizeof(buf), "vi%d native %04x interp %04x", r, a.vi[r], b.vi[r]);
                what = buf;
            }
        if (what.empty() && std::memcmp(&a.vu0_acc, &b.vu0_acc, 16) != 0)
            what = "acc";
        if (what.empty() && std::memcmp(aData, bData, PS2_VU0_DATA_SIZE) != 0)
        {
            uint32_t at = 0;
            while (aData[at] == bData[at])
                ++at;
            std::snprintf(buf, sizeof(buf), "data at 0x%x", at);
            what = buf;
        }
        if (!what.empty() && ++entry.verifyMismatches <= 20u)
            std::fprintf(stderr, "[vu0verify] %s run %llu differs: %s\n", entry.name, static_cast<unsigned long long>(entry.verifyRuns), what.c_str());
        if ((entry.verifyRuns % 20000u) == 0u)
            std::fprintf(stderr, "[vu0verify] %s: %llu runs, %llu differed\n", entry.name, static_cast<unsigned long long>(entry.verifyRuns),
                         static_cast<unsigned long long>(entry.verifyMismatches));
    }
}

uint64_t ps2Vu0CodeHash(const uint8_t *code, uint32_t bytes)
{
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < bytes; ++i)
        h = (h ^ code[i]) * 1099511628211ull;
    return h;
}

void ps2RegisterVu0Native(uint32_t startPc, uint32_t codeBytes, uint64_t codeHash, Ps2Vu0NativeFn fn, const char *name)
{
    Vu0NativeEntry entry;
    entry.startPc = startPc;
    entry.rangeStart = startPc;
    entry.codeBytes = codeBytes;
    entry.codeHash = codeHash;
    entry.fn = fn;
    entry.name = name;
    vu0Natives().push_back(std::move(entry));
}

void ps2RegisterVu0NativeRange(uint32_t startPc, uint32_t rangeStart, uint32_t rangeBytes, uint64_t codeHash, Ps2Vu0NativeFn fn,
                               const char *name) // [gta-recomp]
{
    Vu0NativeEntry entry;
    entry.startPc = startPc;
    entry.rangeStart = rangeStart;
    entry.codeBytes = rangeBytes;
    entry.codeHash = codeHash;
    entry.fn = fn;
    entry.name = name;
    vu0Natives().push_back(std::move(entry));
}

void PS2Runtime::executeVU0Microprogram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    (void)rdram;

    uint8_t *const vu0Code = m_memory.getVU0Code();
    uint8_t *const vu0Data = m_memory.getVU0Data();
    const uint32_t startPC = address & ~0x7u;

    if (!vu0Code || !vu0Data || startPC + 8u > PS2_VU0_CODE_SIZE)
    {
        seedVu0IdleSuccess(ctx);
        return;
    }

    static const bool traceVu0 = std::getenv("PS2X_TRACE_VU0") != nullptr;
    const auto statStart = traceVu0 ? ps2x::hostNow() : std::chrono::steady_clock::time_point{};
    // [gta-recomp] the native for a start pc is looked up (code compared / hashed) again only after VU0 code memory
    // changed: per call that memcmp of the program was ~1% of the game thread
    struct NativeSlot
    {
        uint64_t generation = ~0ull;
        const Vu0NativeEntry *entry = nullptr;
    };
    static NativeSlot nativeSlots[PS2_VU0_CODE_SIZE / 8u];
    NativeSlot &slot = nativeSlots[startPC / 8u];
    const uint64_t codeGeneration = m_memory.getVU0CodeGeneration();
    if (slot.generation != codeGeneration)
    {
        slot.entry = findVu0Native(startPC, vu0Code);
        slot.generation = codeGeneration;
    }
    const Vu0NativeEntry *native = slot.entry;
    static const bool verifyNative = std::getenv("PS2X_VU0_VERIFY") != nullptr;
    if (native && !verifyNative)
    {
        // [gta-recomp] native replacement (see runtime/ps2_vu0_native.h), run in the VU's rounding mode: toward zero
        // in MXCSR alone (fesetround also reprograms the x87 unit, unused on x64 - ~0.5% of the game thread)
        const unsigned csr = _mm_getcsr();
        _mm_setcsr((csr & ~0x6000u) | 0x6000u);
        native->fn(ctx, vu0Data, PS2_VU0_DATA_SIZE);
        _mm_setcsr(csr);
    }
    else
    {
        R5900Context nativeCtx;
        std::vector<uint8_t> nativeData;
        if (native)
        {
            nativeCtx = *ctx;
            nativeData.assign(vu0Data, vu0Data + PS2_VU0_DATA_SIZE);
            const int rounding = std::fegetround();
            std::fesetround(FE_TOWARDZERO);
            native->fn(&nativeCtx, nativeData.data(), PS2_VU0_DATA_SIZE);
            std::fesetround(rounding);
        }
        m_vu0.reset();
        copyVu0ContextToState(ctx, m_vu0.state());
        m_vu0.execute(vu0Code, PS2_VU0_CODE_SIZE,
                      vu0Data, PS2_VU0_DATA_SIZE,
                      m_gs, &m_memory,
                      startPC, 0u, ctx->vu0_itop, 4096);
        copyVu0StateToContext(m_vu0.state(), ctx);
        if (native)
            verifyVu0Native(*native, nativeCtx, nativeData.data(), *ctx, vu0Data);
    }
    { // [gta-recomp] PS2X_VU0_DUMP=<start hex>: VU0 code + data memory at the first call of that program (vu0_<start>.bin)
        static const long dumpAt = std::getenv("PS2X_VU0_DUMP") ? std::strtol(std::getenv("PS2X_VU0_DUMP"), nullptr, 16) : -1;
        static bool dumped = false;
        if (!dumped && dumpAt >= 0 && startPC == static_cast<uint32_t>(dumpAt))
        {
            dumped = true;
            char name[64];
            std::snprintf(name, sizeof(name), "vu0_%04x.bin", static_cast<unsigned>(startPC));
            if (FILE *f = std::fopen(name, "wb"))
            {
                std::fwrite(m_memory.getVU0Code(), 1, PS2_VU0_CODE_SIZE, f);
                std::fwrite(m_memory.getVU0Data(), 1, PS2_VU0_DATA_SIZE, f);
                std::fclose(f);
                std::fprintf(stderr, "[vu0] dumped %s (code %u + data %u bytes)\n", name, PS2_VU0_CODE_SIZE, PS2_VU0_DATA_SIZE);
            }
        }
    }
    { // [gta-recomp] PS2X_TRACE_VU0=1: VU0 microprogram calls and host time per start address, every 5 s
        if (traceVu0)
        {
            static std::map<uint32_t, std::pair<uint64_t, double>> stats;
            static auto last = ps2x::hostNow();
            const auto now = ps2x::hostNow();
            auto &st = stats[startPC];
            ++st.first;
            st.second += std::chrono::duration<double, std::milli>(now - statStart).count();
            if (now - last > std::chrono::seconds(5))
            {
                last = now;
                for (const auto &[pc, v] : stats)
                    std::fprintf(stderr, "[vu0] start=0x%04x calls=%llu host=%.1f ms\n", pc, static_cast<unsigned long long>(v.first), v.second);
                stats.clear();
            }
        }
    }
}

void PS2Runtime::vu0StartMicroProgram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    // VCALLMS and VCALLMSR both route here.
    executeVU0Microprogram(rdram, ctx, address);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx)
{
    handleSyscall(rdram, ctx, 0);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx, uint32_t encodedSyscallId)
{
    if (ctx->in_delay_slot)
    {
        throw std::runtime_error("Attempted to execute a syscall inside a branch delay slot! "
                                 "This breaks the atomic basic block model and is structurally unsupported by the emulator.");
    }

    const uint32_t syscallId = (encodedSyscallId != 0u)
                                   ? encodedSyscallId
                                   : getRegU32(ctx, 3); // $v1 / $3 is the EE kernel syscall number

    if (ps2_syscalls::dispatchNumericSyscall(syscallId, rdram, ctx, this))
    {
        return;
    }

    // God help you
    ps2_syscalls::TODO(rdram, ctx, this, encodedSyscallId);
}

void PS2Runtime::handleBreak(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_BREAKPOINT);
}

void PS2Runtime::drainCompletedDmacHandlers(uint8_t *rdram)
{
    if (!m_memory.hasCompletedDmacCauses()) // [gta-recomp] the common case, without the mutex
        return;
    for (uint32_t cause : m_memory.consumeCompletedDmacCauses())
    {
        ps2_syscalls::dispatchDmacHandlersForCause(rdram, this, cause);
    }
}

void PS2Runtime::handleTrap(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_TRAP);
}

void PS2Runtime::handleTLBR(uint8_t *rdram, R5900Context *ctx)
{
    uint32_t vpn = 0;
    uint32_t pfn = 0;
    uint32_t mask = 0;
    bool valid = false;

    const uint32_t index = ctx->cop0_index & 0x3Fu;
    if (!m_memory.tlbRead(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Preserve low ASID bits in EntryHi.
    ctx->cop0_entryhi = (ctx->cop0_entryhi & 0x00000FFFu) | (vpn & 0xFFFFF000u);
    ctx->cop0_entrylo0 = (ctx->cop0_entrylo0 & ~0x03FFFFC2u) |
                         ((pfn & 0x000FFFFFu) << 6) |
                         (valid ? 0x2u : 0u);
    ctx->cop0_pagemask = mask & 0x01FFE000u;
}

void PS2Runtime::handleTLBWI(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t index = ctx->cop0_index & 0x3Fu;
    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
    }
}

void PS2Runtime::handleTLBWR(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t entryCount = static_cast<uint32_t>(m_memory.tlbEntryCount());
    if (entryCount == 0)
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    const uint32_t wired = std::min(ctx->cop0_wired, entryCount - 1);
    uint32_t random = ctx->cop0_random % entryCount;
    if (random < wired)
    {
        random = wired;
    }

    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(random, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Keep COP0 bookkeeping in sync with the selected slot.
    ctx->cop0_index = (ctx->cop0_index & ~0x3Fu) | (random & 0x3Fu);
    ctx->cop0_random = (random <= wired) ? (entryCount - 1) : (random - 1);
}

void PS2Runtime::handleTLBP(uint8_t *rdram, R5900Context *ctx)
{
    const int32_t index = m_memory.tlbProbe(ctx->cop0_entryhi & 0xFFFFF000u);
    if (index >= 0)
    {
        ctx->cop0_index = (ctx->cop0_index & ~0x8000003Fu) |
                          (static_cast<uint32_t>(index) & 0x3Fu);
    }
    else
    {
        // MIPS sets probe failure bit (P) in Index[31].
        ctx->cop0_index |= 0x80000000u;
    }
}

void PS2Runtime::clearLLBit(R5900Context *ctx)
{
    // LL/SC reservation is tracked separately from COP0 Status.
    ctx->llbit = 0;
    ctx->lladdr = 0;
}

uint32_t PS2Runtime::alignGuestHeapValue(uint32_t value, uint32_t alignment)
{
    if (alignment == 0)
    {
        return value;
    }

    const uint32_t mask = alignment - 1u;
    if (value > (std::numeric_limits<uint32_t>::max() - mask))
    {
        return std::numeric_limits<uint32_t>::max();
    }
    return (value + mask) & ~mask;
}

bool PS2Runtime::isGuestHeapAlignmentValid(uint32_t alignment)
{
    return alignment != 0u && (alignment & (alignment - 1u)) == 0u;
}

uint32_t PS2Runtime::normalizeGuestHeapAlignment(uint32_t alignment)
{
    if (!isGuestHeapAlignmentValid(alignment))
    {
        return kGuestHeapDefaultAlignment;
    }
    return std::max(alignment, kGuestHeapDefaultAlignment);
}

uint32_t PS2Runtime::clampGuestHeapBase(uint32_t guestBase) const
{
    uint32_t normalized = guestBase;
    if (normalized >= PS2_RAM_SIZE)
    {
        normalized &= PS2_RAM_MASK;
    }
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    return std::min(normalized, hardLimit);
}

uint32_t PS2Runtime::clampGuestHeapLimit(uint32_t guestLimit) const
{
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    if (guestLimit == 0u || guestLimit > hardLimit)
    {
        return hardLimit;
    }
    return guestLimit;
}

void PS2Runtime::resetGuestHeapLocked(uint32_t guestBase, uint32_t guestLimit)
{
    uint32_t base = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    uint32_t limit = clampGuestHeapLimit(guestLimit);
    if (base == 0u)
    {
        const uint32_t fallbackBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
        base = alignGuestHeapValue(clampGuestHeapBase(fallbackBase), kGuestHeapDefaultAlignment);
    }

    if (limit <= base)
    {
        base = alignGuestHeapValue(clampGuestHeapBase(m_guestHeapSuggestedBase), kGuestHeapDefaultAlignment);
        limit = clampGuestHeapLimit(0u);
    }

    if (limit <= base)
    {
        base = 0u;
        limit = 0u;
    }

    m_guestHeapBlocks.clear();
    if (limit > base)
    {
        m_guestHeapBlocks.push_back({base, limit - base, true});
    }

    m_guestHeapBase = base;
    m_guestHeapEnd = base;
    m_guestHeapLimit = limit;
    m_guestHeapConfigured = true;
}

void PS2Runtime::ensureGuestHeapInitializedLocked()
{
    if (m_guestHeapConfigured)
    {
        return;
    }

    const uint32_t suggested = (m_guestHeapSuggestedBase == 0u) ? kGuestHeapDefaultBase : m_guestHeapSuggestedBase;
    resetGuestHeapLocked(suggested, clampGuestHeapLimit(0u));
}

int32_t PS2Runtime::findGuestHeapBlockIndexLocked(uint32_t guestAddr) const
{
    const uint32_t normalizedAddr = guestAddr & PS2_RAM_MASK;
    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock &block = m_guestHeapBlocks[i];
        if (!block.free && block.addr == normalizedAddr)
        {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

uint32_t PS2Runtime::allocateGuestBlockLocked(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    if (size > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock block = m_guestHeapBlocks[i];
        if (!block.free)
        {
            continue;
        }

        const uint64_t blockStart = block.addr;
        const uint64_t blockEnd = blockStart + static_cast<uint64_t>(block.size);
        const uint32_t alignedAddr = alignGuestHeapValue(block.addr, normalizedAlignment);
        if (alignedAddr < block.addr)
        {
            continue;
        }

        const uint64_t alignedStart = alignedAddr;
        if (alignedStart > blockEnd)
        {
            continue;
        }

        const uint64_t allocEnd = alignedStart + static_cast<uint64_t>(allocSize);
        if (allocEnd > blockEnd)
        {
            continue;
        }

        const uint32_t prefixSize = static_cast<uint32_t>(alignedStart - blockStart);
        const uint32_t suffixSize = static_cast<uint32_t>(blockEnd - allocEnd);

        std::vector<GuestHeapBlock> replacement;
        replacement.reserve(3);
        if (prefixSize > 0u)
        {
            replacement.push_back({block.addr, prefixSize, true});
        }
        replacement.push_back({alignedAddr, allocSize, false});
        if (suffixSize > 0u)
        {
            replacement.push_back({static_cast<uint32_t>(allocEnd), suffixSize, true});
        }

        m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
        m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i),
                                 replacement.begin(),
                                 replacement.end());

        m_guestHeapEnd = std::max(m_guestHeapEnd, static_cast<uint32_t>(allocEnd));
        return alignedAddr;
    }

    return 0u;
}

void PS2Runtime::coalesceGuestHeapLocked()
{
    if (m_guestHeapBlocks.empty())
    {
        return;
    }

    size_t i = 1;
    while (i < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &prev = m_guestHeapBlocks[i - 1];
        GuestHeapBlock &curr = m_guestHeapBlocks[i];
        const uint64_t prevEnd = static_cast<uint64_t>(prev.addr) + static_cast<uint64_t>(prev.size);
        if (prev.free && curr.free && prevEnd == curr.addr)
        {
            prev.size += curr.size;
            m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
}

void PS2Runtime::freeGuestBlockLocked(uint32_t guestAddr)
{
    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return;
    }

    m_guestHeapBlocks[static_cast<size_t>(index)].free = true;
    coalesceGuestHeapLocked();
}

void PS2Runtime::configureGuestHeap(uint32_t guestBase, uint32_t guestLimit)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    uint32_t normalizedBase = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    if (normalizedBase == 0u)
    {
        normalizedBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
    }
    m_guestHeapSuggestedBase = normalizedBase;
    resetGuestHeapLocked(normalizedBase, guestLimit);
}

uint32_t PS2Runtime::guestMalloc(uint32_t size, uint32_t alignment)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    return allocateGuestBlockLocked(size, alignment);
}

uint32_t PS2Runtime::guestCalloc(uint32_t count, uint32_t size, uint32_t alignment)
{
    if (count == 0u || size == 0u)
    {
        return 0u;
    }
    if (count > (std::numeric_limits<uint32_t>::max() / size))
    {
        return 0u;
    }

    const uint32_t totalSize = count * size;
    const uint32_t guestAddr = guestMalloc(totalSize, alignment);
    if (guestAddr != 0u)
    {
        uint8_t *rdram = m_memory.getRDRAM();
        if (rdram)
        {
            uint32_t physAddr = guestAddr & PS2_RAM_MASK;
            if (physAddr + totalSize <= PS2_RAM_SIZE)
                std::memset(rdram + physAddr, 0, totalSize);
        }
    }

    return guestAddr;
}

uint32_t PS2Runtime::guestRealloc(uint32_t guestAddr, uint32_t newSize, uint32_t alignment)
{
    if (guestAddr == 0u)
    {
        return guestMalloc(newSize, alignment);
    }
    if (newSize == 0u)
    {
        guestFree(guestAddr);
        return 0u;
    }

    if (newSize > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t requestedSize = alignGuestHeapValue(newSize, kGuestHeapDefaultAlignment);

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();

    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return 0u;
    }

    const size_t blockIndex = static_cast<size_t>(index);
    const uint32_t oldAddr = m_guestHeapBlocks[blockIndex].addr;
    const uint32_t oldSize = m_guestHeapBlocks[blockIndex].size;

    if (requestedSize <= oldSize)
    {
        if (requestedSize < oldSize)
        {
            const uint32_t tailAddr = oldAddr + requestedSize;
            const uint32_t tailSize = oldSize - requestedSize;
            m_guestHeapBlocks[blockIndex].size = requestedSize;
            m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u),
                                     GuestHeapBlock{tailAddr, tailSize, true});
            coalesceGuestHeapLocked();
        }
        return oldAddr;
    }

    if (blockIndex + 1u < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &next = m_guestHeapBlocks[blockIndex + 1u];
        const uint64_t blockEnd = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].addr) +
                                  static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size);
        if (next.free && blockEnd == next.addr)
        {
            const uint64_t combined = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size) +
                                      static_cast<uint64_t>(next.size);
            if (combined >= requestedSize)
            {
                const uint32_t extraNeeded = requestedSize - m_guestHeapBlocks[blockIndex].size;
                m_guestHeapBlocks[blockIndex].size = requestedSize;
                if (next.size == extraNeeded)
                {
                    m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u));
                }
                else
                {
                    next.addr += extraNeeded;
                    next.size -= extraNeeded;
                }
                m_guestHeapEnd = std::max(m_guestHeapEnd, oldAddr + requestedSize);
                return oldAddr;
            }
        }
    }

    const uint32_t newAddr = allocateGuestBlockLocked(newSize, normalizedAlignment);
    if (newAddr == 0u)
    {
        return 0u;
    }

    uint8_t *rdram = m_memory.getRDRAM();
    if (rdram)
    {
        const uint32_t copyBytes = std::min(oldSize, newSize);
        uint32_t dstPhys = newAddr & PS2_RAM_MASK;
        uint32_t srcPhys = oldAddr & PS2_RAM_MASK;
        if (dstPhys + copyBytes <= PS2_RAM_SIZE && srcPhys + copyBytes <= PS2_RAM_SIZE)
            std::memmove(rdram + dstPhys, rdram + srcPhys, copyBytes);
    }

    freeGuestBlockLocked(oldAddr);
    return newAddr;
}

void PS2Runtime::guestFree(uint32_t guestAddr)
{
    if (guestAddr == 0u)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    freeGuestBlockLocked(guestAddr);
}

uint32_t PS2Runtime::guestHeapBase() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapBase : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapEnd() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapEnd : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapLimit() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapLimit : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::reserveAsyncCallbackStack(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
    uint32_t top = m_asyncCallbackStackTop;
    if (top > PS2_RAM_SIZE)
    {
        top = PS2_RAM_SIZE;
    }
    top &= ~(kGuestHeapDefaultAlignment - 1u);

    if (top <= allocSize)
    {
        return 0u;
    }

    uint32_t base = top - allocSize;
    base &= ~(normalizedAlignment - 1u);
    if (base < m_asyncCallbackStackFloor || base >= top)
    {
        return 0u;
    }

    m_asyncCallbackStackTop = base;
    return top - 0x10u;
}

uint8_t PS2Runtime::loadSlow8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read8(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint16_t PS2Runtime::loadSlow16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read16(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint32_t PS2Runtime::loadSlow32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read32(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint64_t PS2Runtime::loadSlow64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    if ((vaddr & 0x1FFFFFFFu) == PS2_GS_PRIV_REG_BASE + 0x1000u && !m_csrNoFencePcs.empty() && // [gta-recomp] CSR
        std::find(m_csrNoFencePcs.begin(), m_csrNoFencePcs.end(), ctx->pc) != m_csrNoFencePcs.end())
        return m_memory.gs().csr.load(std::memory_order_acquire);
    try
    {
        return m_memory.read64(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

__m128i PS2Runtime::loadSlow128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read128(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return _mm_setzero_si128();
    }
}

void PS2Runtime::storeSlow8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint8_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 1u, value, 0u, "WRITE8", ctx);
    try
    {
        m_memory.write8(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::storeSlow16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint16_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 2u, value, 0u, "WRITE16", ctx);
    try
    {
        m_memory.write16(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::storeSlow32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint32_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 4u, value, 0u, "WRITE32", ctx);
    try
    {
        m_memory.write32(vaddr, value);
        drainCompletedDmacHandlers(rdram);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::storeSlow64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint64_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 8u, value, 0u, "WRITE64", ctx);
    try
    {
        m_memory.write64(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::storeSlow128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, __m128i value)
{
    alignas(16) uint64_t _parts[2];
    _mm_storeu_si128(reinterpret_cast<__m128i *>(_parts), value);
    ps2TraceGuestWrite(rdram, vaddr, 16u, _parts[0], _parts[1], "WRITE128", ctx);
    try
    {
        m_memory.write128(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::kickGifDmaChainFromMMIO(uint8_t *rdram,
                                         R5900Context *ctx,
                                         uint32_t dPcrValue,
                                         uint32_t dStatValue,
                                         uint32_t tadr,
                                         uint32_t chcr)
{
    constexpr uint32_t D_PCR = 0x1000E020u;
    constexpr uint32_t D_STAT = 0x1000E010u;
    constexpr uint32_t GIF_TADR = 0x1000A030u;
    constexpr uint32_t GIF_CHCR = 0x1000A000u;

    ps2TraceGuestWrite(rdram, D_PCR, 4u, dPcrValue, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(D_PCR, dPcrValue);
    ps2TraceGuestWrite(rdram, D_STAT, 4u, dStatValue, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(D_STAT, dStatValue);
    ps2TraceGuestWrite(rdram, GIF_TADR, 4u, tadr, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(GIF_TADR, tadr);
    ps2TraceGuestWrite(rdram, GIF_CHCR, 4u, chcr, 0u, "WRITE32", ctx);
    if (m_memory.tryProcessNativeGifImageUploadChain(m_gs, tadr, chcr))
    {
        drainCompletedDmacHandlers(rdram);
        return;
    }
    if (m_memory.tryProcessNativeGifPackedChain(m_gs, tadr, chcr))
    {
        drainCompletedDmacHandlers(rdram);
        return;
    }
    m_memory.writeIORegister(GIF_CHCR, chcr);
    m_memory.processPendingTransfers();
    drainCompletedDmacHandlers(rdram);
}

void PS2Runtime::requestStop()
{
    m_stopRequested.store(true, std::memory_order_relaxed);
    if (m_eeScheduler)
    {
        m_eeScheduler->requestStop();
    }
}

bool PS2Runtime::isStopRequested() const
{
    return m_stopRequested.load(std::memory_order_relaxed);
}

EeScheduler &PS2Runtime::eeScheduler()
{
    return *m_eeScheduler;
}

const EeScheduler &PS2Runtime::eeScheduler() const
{
    return *m_eeScheduler;
}

void PS2Runtime::postEeEvent(EeEvent event)
{
    m_eeScheduler->postEvent(event);
}

bool PS2Runtime::eeCheckpointDueSlow(uint32_t cycles) noexcept
{
    const bool due = m_eeScheduler->checkpointDue(cycles);
    if (due)
        g_checkpointUnwind = true; // [gta-recomp] see g_checkpointUnwind
    return due;
}

[[noreturn]] void PS2Runtime::eeWaitVSyncTicks(uint32_t ticks, uint32_t resumePc)
{
    const uint64_t currentTick = m_eeScheduler->currentVSyncTick();
    const uint64_t waitTicks = std::max<uint64_t>(1u, ticks);
    m_eeScheduler->waitVSync(currentTick + waitTicks - 1u,
                             0,
                             [resumePc](R5900Context &context)
                             {
                                 context.pc = resumePc;
                             });
}

void PS2Runtime::addEeExitHandler(int threadId, uint32_t function, uint32_t argument)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    m_eeExitHandlers[threadId].push_back({function, argument});
}

std::vector<PS2Runtime::EeExitHandlerRegistration> PS2Runtime::takeEeExitHandlers(int threadId)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    auto it = m_eeExitHandlers.find(threadId);
    if (it == m_eeExitHandlers.end())
    {
        return {};
    }
    auto handlers = std::move(it->second);
    m_eeExitHandlers.erase(it);
    return handlers;
}

void PS2Runtime::removeEeExitHandlers(int threadId)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    m_eeExitHandlers.erase(threadId);
}

bool PS2Runtime::findEeSyscallOverride(uint32_t syscallNumber, uint32_t &handler) const
{
    std::lock_guard lock(m_eeKernelStateMutex);
    const auto it = m_eeSyscallOverrides.find(syscallNumber);
    if (it == m_eeSyscallOverrides.end())
    {
        return false;
    }
    handler = it->second;
    return true;
}

void PS2Runtime::setEeSyscallOverride(uint8_t *rdram, uint32_t syscallNumber, uint32_t handler)
{
    constexpr uint32_t kTableBase = 0x80011F80u & 0x1FFFFFFFu;
    constexpr uint32_t kMirrorLimit = 0x00080000u;
    const int64_t offset = static_cast<int64_t>(static_cast<int32_t>(syscallNumber)) * 4;
    const int64_t address = static_cast<int64_t>(kTableBase) + offset;

    std::lock_guard lock(m_eeKernelStateMutex);
    if (handler == 0u)
    {
        m_eeSyscallOverrides.erase(syscallNumber);
    }
    else
    {
        m_eeSyscallOverrides[syscallNumber] = handler;
    }
    if (!rdram || address < 0 || address + 4 > kMirrorLimit)
    {
        return;
    }
    const uint32_t guestAddress = static_cast<uint32_t>(address);
    std::memcpy(rdram + guestAddress, &handler, sizeof(handler));
    if (handler == 0u)
    {
        m_eeSyscallMirrorAddresses.erase(guestAddress);
    }
    else
    {
        m_eeSyscallMirrorAddresses.insert(guestAddress);
    }
}

void PS2Runtime::initializeEeKernelState(uint8_t *rdram)
{
    if (!rdram)
    {
        return;
    }
    constexpr uint32_t kTableGuestBase = 0x80011F80u;
    constexpr uint32_t kTableBase = kTableGuestBase & 0x1FFFFFFFu;
    constexpr uint32_t kMirrorLimit = 0x00080000u;
    constexpr uint32_t kProbeBase = 0x000002F0u;

    std::lock_guard lock(m_eeKernelStateMutex);
    for (const uint32_t address : m_eeSyscallMirrorAddresses)
    {
        const uint32_t zero = 0u;
        std::memcpy(rdram + address, &zero, sizeof(zero));
    }
    m_eeSyscallMirrorAddresses.clear();
    const uint32_t high = kTableGuestBase >> 16;
    const uint32_t low = kTableGuestBase & 0xFFFFu;
    std::memcpy(rdram + kProbeBase, &high, sizeof(high));
    std::memcpy(rdram + kProbeBase + 8u, &low, sizeof(low));
    m_eeSyscallMirrorAddresses.insert(kProbeBase);
    m_eeSyscallMirrorAddresses.insert(kProbeBase + 8u);

    for (const auto &[syscallNumber, handler] : m_eeSyscallOverrides)
    {
        const int64_t offset = static_cast<int64_t>(static_cast<int32_t>(syscallNumber)) * 4;
        const int64_t address = static_cast<int64_t>(kTableBase) + offset;
        if (address < 0 || address + 4 > kMirrorLimit)
        {
            continue;
        }
        const uint32_t guestAddress = static_cast<uint32_t>(address);
        std::memcpy(rdram + guestAddress, &handler, sizeof(handler));
        m_eeSyscallMirrorAddresses.insert(guestAddress);
    }
}

void PS2Runtime::HandleIntegerOverflow(R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_INTEGER_OVERFLOW);
}

void PS2Runtime::run()
{
    m_stopRequested.store(false, std::memory_order_relaxed);
    ps2_stubs::resetSifState();
    resetIop();
    ps2_stubs::resetAudioStubState();
    ps2_stubs::resetMpegStubState();
    initializeEeKernelState(m_memory.getRDRAM());
    m_cpuContext.r[4] = _mm_setzero_si128();
    m_cpuContext.r[5] = _mm_setzero_si128();
    m_cpuContext.r[29] = _mm_set_epi64x(0, static_cast<int64_t>(PS2_RAM_SIZE - 0x10u));
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);
    m_debugRa.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)), std::memory_order_relaxed);
    m_debugSp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[29], 0)), std::memory_order_relaxed);
    m_debugGp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[28], 0)), std::memory_order_relaxed);

    RUNTIME_LOG("Starting execution at address 0x" << std::hex << m_cpuContext.pc << std::dec);
    ThreadNaming::SetCurrentThreadName("MainThread"); // [gta-recomp] window, presenting, the HW backend's frames (VCS_PROFILE_THREAD)

    // A blank image to use as a framebuffer
    present::Picture frameTex = present::createPicture(FB_WIDTH, FB_HEIGHT, false);
    {
        const std::vector<uint32_t> blank(static_cast<size_t>(FB_WIDTH) * FB_HEIGHT, 0u);
        present::updatePicture(frameTex, blank.data());
    }
    present::Picture shownTex = frameTex; // [gta-recomp] what the window shows (frameTex or another renderer's)

    std::atomic<bool> gameThreadFinished{false};

    std::thread gameThread([&]()
                           {
        ThreadNaming::SetCurrentThreadName("GameThread");
        try
        {
            m_eeScheduler->reset(m_memory.getRDRAM(), m_cpuContext);
            m_eeScheduler->run();
            uint32_t pc = m_debugPc.load(std::memory_order_relaxed);
            RUNTIME_LOG("Game thread returned. PC=0x" << std::hex << pc
                      << " RA=0x" << static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)) << std::dec << std::endl);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error during program execution: " << e.what() << std::endl;
        }
        catch (...)
        {
            std::cerr << "Error during program execution: unknown exception" << std::endl;
        }
        gameThreadFinished.store(true, std::memory_order_release); });

    uint64_t tick = 0;
    while (!isStopRequested() && !gameThreadFinished.load(std::memory_order_acquire))
    {
        PS2_IF_AGRESSIVE_LOGS({
            tick++;
            if ((tick % 120) == 0)
            {
                uint64_t curDma = m_memory.dmaStartCount();
                uint64_t curGif = m_memory.gifCopyCount();
                uint64_t curGs = m_memory.gsWriteCount();
                uint64_t curVif = m_memory.vifWriteCount();
                const GSRegisters &gs = m_memory.gs();
                const uint32_t dbgPc = m_debugPc.load(std::memory_order_relaxed);
                const uint32_t dbgRa = m_debugRa.load(std::memory_order_relaxed);
                const uint32_t dbgSp = m_debugSp.load(std::memory_order_relaxed);
                const uint32_t dbgGp = m_debugGp.load(std::memory_order_relaxed);
                const auto eeSnapshot = m_eeScheduler->snapshot();

                RUNTIME_LOG("[run:tick] tick=" << tick
                                               << " pc=0x" << std::hex << dbgPc
                                               << " ra=0x" << dbgRa
                                               << " sp=0x" << dbgSp
                                               << " gp=0x" << dbgGp
                                               << " dispfb1=0x" << gs.dispfb1
                                               << " display1=0x" << gs.display1
                                               << std::dec
                                               << " activeThreads=" << eeSnapshot.threads.size()
                                               << " dma=" << curDma
                                               << " gif=" << curGif
                                               << " gsw=" << curGs
                                               << " vif=" << curVif
                                               << std::endl);

            }
        });
        // [gta-recomp] PS2X_HOST_SPIKES=<ms>: a host frame whose work (not its sleep) took longer, with its phases
        static const double spikeMs = std::getenv("PS2X_HOST_SPIKES") ? std::atof(std::getenv("PS2X_HOST_SPIKES")) : 0.0;
        using SpikeClock = std::chrono::steady_clock;
        const auto spikeT0 = SpikeClock::now();
        const auto threadCpuMs = []() -> double { // the main thread's own CPU time: wall minus this = blocked or preempted
#if defined(_WIN32)
            return -1.0; // not on Windows yet (windows.h stays out of this file)
#else
            timespec ts{};
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
            return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
#endif
        };
        const double spikeCpu0 = spikeMs > 0.0 ? threadCpuMs() : 0.0;
        double spikeCpuFrames = 0.0;
        SpikeClock::time_point spikeT[6];
        const bool spikeLead = platform::frameLeadRan();
        platform::pumpEvents(); // [gta-recomp]
        // [gta-recomp] host-paced game frames start on fresh input (ps2_present.h): here, unless the frame lead started
        // this one already, early enough to be ready now (below)
        if (!platform::frameLeadRan())
            ps2HostTick(false, platform::frameSlept());
        spikeT[0] = SpikeClock::now();
        present::beginFrame();
        platform::beginFrame();
        spikeT[1] = SpikeClock::now();
        const double spikeCpu1 = spikeMs > 0.0 ? threadCpuMs() : 0.0;
        uint32_t presentWidth = FB_WIDTH;
        uint32_t presentHeight = DEFAULT_DISPLAY_HEIGHT;
        const auto uploadStart = ps2x::hostNow();
        UploadFrame(frameTex, shownTex, this, presentWidth, presentHeight);
        spikeT[2] = SpikeClock::now();
        if (spikeMs > 0.0)
            spikeCpuFrames = threadCpuMs() - spikeCpu1;
        ps2perf::hostFrame(std::chrono::duration<double, std::milli>(ps2x::hostNow() - uploadStart).count()); // [gta-recomp]

        // [gta-recomp] display aspect: 4:3 (the PS2's TV), the widescreen ratio for 3D frames (render/presenter.h)
        float aspect = present::displayAspect(!hwRenderer() || hwRenderer()->presenter().showing3d());
        // [gta-recomp] the game's frame (the 3D frames' shape): 4:3 frames (menus, loading screens) are placed inside it
        float frameAspect = present::displayAspect(true);
        { // [gta-recomp] a movie playing (ps2_present.h): its picture instead of the game's
            static present::Picture moviePicture;
            static uint64_t movieSequence = 0;
            std::lock_guard<std::mutex> lock(g_movieFrame.mutex);
            if (g_movieFrame.active && g_movieFrame.width > 0)
            {
                if (moviePicture.width != g_movieFrame.width || moviePicture.height != g_movieFrame.height)
                    moviePicture = present::createPicture(g_movieFrame.width, g_movieFrame.height, true);
                if (movieSequence != g_movieFrame.sequence)
                {
                    present::updatePicture(moviePicture, g_movieFrame.rgba.data());
                    movieSequence = g_movieFrame.sequence;
                }
                shownTex = moviePicture;
                presentWidth = static_cast<uint32_t>(g_movieFrame.width);
                presentHeight = static_cast<uint32_t>(g_movieFrame.height);
                aspect = g_movieFrame.aspect;
                frameAspect = 0.0f; // movies: the whole window, as before
            }
        }
        static float lastAspect = 0.0f; // [gta-recomp] log the presentation shape when it changes
        if (aspect != lastAspect)
        {
            std::fprintf(stderr, "[present] display aspect %.3f\n", aspect);
            lastAspect = aspect;
        }
        const int srcWidth = static_cast<int>(std::max<uint32_t>(1u, presentWidth));
        const int srcHeight = static_cast<int>(std::max<uint32_t>(1u, presentHeight));
        // [gta-recomp] brightness probe (black-frame detector for the automation rig)
        static const bool frameStats = [] {
            // PS2X_FRAME_STATS=0 turns it off with the control channel on: its read-back waits for the GPU every host
            // frame, which hides GPU-side hitches from a stutter measurement (tools/cruise.py)
            const char *a = std::getenv("PS2X_FRAME_STATS"), *b = std::getenv("PS2X_CONTROL");
            if (a && *a)
                return *a != '0';
            return b && *b && *b != '0';
        }();
        if (frameStats)
        {
            static uint64_t hostFrame = 0;
            float luma = 0.0f;
            if (present::frameLuma(shownTex, srcWidth, srcHeight, luma))
            {
                const bool started = ps2perf::noteFrameLuma(luma, hostFrame);
                // PS2X_FRAME_DUMP_DIR: the picture going black or much darker than the scene (a dip) - save that
                // frame and the next two, and say what the renderer drew (first 60 such events)
                static const char *dumpDir = std::getenv("PS2X_FRAME_DUMP_DIR");
                static float prevLuma = 0.0f;
                static int dumpLeft = 0, events = 0;
                if (dumpDir && *dumpDir && started && events < 60)
                {
                    ++events;
                    dumpLeft = 3;
                }
                if (dumpLeft > 0)
                {
                    --dumpLeft;
                    const std::string path = std::string(dumpDir) + "/black_" + std::to_string(hostFrame) + ".png";
                    std::vector<uint8_t> rgb;
                    if (present::capture(shownTex, srcWidth, srcHeight, aspect, 640, 448, rgb, frameAspect))
                        platform::writePng(path.c_str(), rgb.data(), 640, 448);
                    std::fprintf(stderr, "[frames] dump host frame %llu luma %.1f (before %.1f): %s\n",
                                 static_cast<unsigned long long>(hostFrame), luma, prevLuma,
                                 hwRenderer() ? hwRenderer()->presenter().lastFrameInfo().c_str() : "(no hw renderer)");
                }
                prevLuma = luma;
            }
            ++hostFrame;
        }
        ps2perf::drawOverlay(); // [gta-recomp] F3 overlay, F4 CSV recording
        if (m_debugUiInitialized && m_debugUiDrawCallback)
        {
            m_debugUiDrawCallback(*this, m_debugUiUserData);
        }
        platform::endUiFrame();
        int windowWidth = 0, windowHeight = 0;
        platform::windowSize(windowWidth, windowHeight);
        spikeT[3] = SpikeClock::now();
        present::drawWindow(shownTex, srcWidth, srcHeight, aspect, windowWidth, windowHeight, frameAspect);
        spikeT[4] = SpikeClock::now();

        // [gta-recomp] control channel (runtime/ps2_control.h): screenshots render the same picture off-screen, so
        // they work with the window hidden or covered
        auto capturePng = [&](const std::string &path) -> std::string
            {
                int w = std::max(windowWidth, 960), h = std::max(windowHeight, 672); // readable when hidden
                // PS2X_CAPTURE_SIZE=WxH: a fixed size whatever the window (comparable test screenshots)
                static const std::pair<int, int> fixedSize = [] {
                    int fw = 0, fh = 0;
                    if (const char *e = std::getenv("PS2X_CAPTURE_SIZE"))
                        std::sscanf(e, "%dx%d", &fw, &fh);
                    return std::make_pair(fw, fh);
                }();
                if (fixedSize.first > 0 && fixedSize.second > 0)
                    w = fixedSize.first, h = fixedSize.second;
                std::vector<uint8_t> rgb;
                if (!present::capture(shownTex, srcWidth, srcHeight, aspect, w, h, rgb, frameAspect) || !platform::writePng(path.c_str(), rgb.data(), w, h))
                    return {};
                return std::to_string(w) + "x" + std::to_string(h);
            };
        // [gta-recomp] the player's screenshot key (ps2control::requestScreenshot): the window's own pixels - what was on
        // screen, not the test size above (run.py sets PS2X_CAPTURE_SIZE) - read back here; the PNG is written on a thread
        // of its own (at 2560x1440 compressing it held the frame ~0.5 s)
        auto shootPng = [&](const std::string &path, ps2control::Done done)
            {
                const int w = windowWidth, h = windowHeight;
                auto rgb = std::make_shared<std::vector<uint8_t>>();
                if (w <= 0 || h <= 0 || !present::capture(shownTex, srcWidth, srcHeight, aspect, w, h, *rgb, frameAspect))
                {
                    if (done)
                        done({});
                    return;
                }
                std::thread([path, rgb, w, h, done = std::move(done)] {
                    const bool ok = platform::writePng(path.c_str(), rgb->data(), w, h);
                    if (done)
                        done(ok ? std::to_string(w) + "x" + std::to_string(h) : std::string());
                }).detach();
            };
        if (g_burst.left > 0) // [gta-recomp] "burst" (above)
        {
            char name[32];
            std::snprintf(name, sizeof(name), "/burst_%04d.png", g_burst.index);
            capturePng(g_burst.dir + name);
            if (FILE *f = std::fopen((g_burst.dir + "/burst.txt").c_str(), "a"))
            {
                std::fprintf(f, "%04d aspect %.3f frame %.3f %s\n", g_burst.index, aspect, frameAspect,
                             hwRenderer() ? hwRenderer()->presenter().lastFrameInfo().c_str() : "");
                std::fclose(f);
            }
            ++g_burst.index;
            --g_burst.left;
        }
        ps2control::pump(capturePng, shootPng, [this] { requestStop(); });
        present::setFrameInputTime(gsHwShownInputNs()); // [gta-recomp] input-to-screen delay (PS2X_RHI_STATS)
        present::present();
        spikeT[5] = SpikeClock::now();
        if (spikeMs > 0.0)
        {
            const auto ms = [](SpikeClock::time_point a, SpikeClock::time_point b) {
                return std::chrono::duration<double, std::milli>(b - a).count();
            };
            const double work = ms(spikeT0, spikeT[5]);
            if (work > spikeMs)
                std::fprintf(stderr,
                             "[host-spike] t=%.3f work %.1f ms (cpu %.1f): events+tick %.1f, begin %.1f, frames %.1f (cpu %.1f), ui %.1f, "
                             "window %.1f, present %.1f (lead %d, frames taken %u)\n",
                             std::chrono::duration<double>(spikeT0.time_since_epoch()).count(), work, threadCpuMs() - spikeCpu0,
                             ms(spikeT0, spikeT[0]), ms(spikeT[0], spikeT[1]), ms(spikeT[1], spikeT[2]), spikeCpuFrames,
                             ms(spikeT[2], spikeT[3]), ms(spikeT[3], spikeT[4]), ms(spikeT[4], spikeT[5]), spikeLead ? 1 : 0,
                             ps2LastFramesTaken());
        }
        // [gta-recomp] frame lead (docs/PERFORMANCE.md 19): with host-paced game frames, the next one starts - on input
        // read then - as long before the next host frame as it takes to build (gsHwFrameBuildNs) plus a margin, so it is
        // ready just in time instead of a host frame late. PS2X_FRAME_LEAD=0: at the host frame's start (no lead);
        // PS2X_FRAME_LEAD_MARGIN_US=<us> (1500).
        {
            static const bool leadOn = [] {
                const char *e = std::getenv("PS2X_FRAME_LEAD");
                return !(e && e[0] == '0');
            }();
            static const int64_t marginNs = [] {
                const char *e = std::getenv("PS2X_FRAME_LEAD_MARGIN_US");
                return (e && *e ? std::atoll(e) : 1500ll) * 1000ll;
            }();
            const int64_t build = gsHwFrameBuildNs();
            const bool lead = leadOn && g_hostPaced.load(std::memory_order_relaxed) && build > 0;
            platform::setFrameLead(lead ? static_cast<uint64_t>(build + marginNs) : 0u, [] {
                platform::pumpEvents(false);
                ps2HostTick(true);
            });
        }
        platform::waitNextFrame();

        if (platform::quitRequested())
        {
            RUNTIME_LOG("[run] window close requested, breaking out of loop");
            requestStop();
            break;
        }
    }

    requestStop();
    if (gameThread.joinable())
    {
        gameThread.join();
    }
    ps2_render_worker::shutdown(); // [gta-recomp] what the game posted runs, then the worker ends

    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }
    shutdownGpuRenderer(); // [gta-recomp]
    ps2perf::shutdown();   // [gta-recomp]
    ps2control::stop();    // [gta-recomp]
    frameTex = {};
    shownTex = {};
    present::shutdown();
    platform::shutdown();

    RUNTIME_LOG("[run] exiting loop");
}
