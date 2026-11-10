// Phase 4: Streaming trace hooks for props and model streaming
// Hooks ScanSectorList, ScanBigBuildingList, ScanSectorList_FrustumTriangle,
// SetupEntityVisibility, and CStreaming::RequestModelFlags
#include "game/vcs_streaming_trace.h"

#include "game/vcs_memory.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_control.h"
#include "settings/settings.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <mutex>

namespace vcs::streaming_trace
{
    namespace
    {
        // Function addresses from game-internals.md
        constexpr uint32_t kScanSectorList = 0x0035FAE8u;
        constexpr uint32_t kScanBigBuildingList = 0x0035FED0u;
        constexpr uint32_t kScanFrustumTriangle = 0x0035DEA8u;
        constexpr uint32_t kSetupEntityVisibility = 0x0035EFD8u;
        constexpr uint32_t kRequestModelFlags = 0x003C7ED8u;  // CStreaming::RequestModelFlags

        // Key addresses
        constexpr uint32_t kCameraPos = 0x006F4500u;
        constexpr uint32_t kCameraFront = 0x006F44E0u;
        constexpr uint32_t kFrameCounter = 0x004CD124u;
        constexpr uint32_t kPlayerVel = 0x004E4910u;
        constexpr uint32_t kVehicleVel = 0x140u;

        // Settings
        std::atomic<bool> g_enabled{false};
        std::atomic<int> g_rate_limit{30};  // log once per N frames

        // Original function pointers
        PS2Runtime::RecompiledFunction g_origScanSectorList = nullptr;
        PS2Runtime::RecompiledFunction g_origScanBigBuildingList = nullptr;
        PS2Runtime::RecompiledFunction g_origScanFrustumTriangle = nullptr;
        PS2Runtime::RecompiledFunction g_origSetupEntityVisibility = nullptr;
        PS2Runtime::RecompiledFunction g_origRequestModelFlags = nullptr;

        // Rate limiting
        struct RateLimiter {
            std::atomic<uint32_t> last_frame{0};
            uint32_t interval;
            RateLimiter(uint32_t i = 30) : interval(i) {}
            bool should_log(uint32_t frame) {
                uint32_t last = last_frame.load(std::memory_order_relaxed);
                if (frame - last >= interval) {
                    last_frame.store(frame, std::memory_order_relaxed);
                    return true;
                }
                return false;
            }
        };

        RateLimiter g_scan_sector_limiter(30);
        RateLimiter g_scan_big_building_limiter(30);
        RateLimiter g_frustum_triangle_limiter(30);
        RateLimiter g_setup_visibility_limiter(30);
        RateLimiter g_request_model_limiter(30);

        // Stats
        std::atomic<uint64_t> g_scan_sector_count{0};
        std::atomic<uint64_t> g_scan_big_building_count{0};
        std::atomic<uint64_t> g_frustum_triangle_count{0};
        std::atomic<uint64_t> g_setup_visibility_count{0};
        std::atomic<uint64_t> g_request_model_count{0};

        // Helper to read guest memory safely
        inline uint32_t rd32(const uint8_t *rdram, uint32_t addr) {
            addr &= 0x1FFFFFFFu;
            if (addr + 4 > 0x2000000u) return 0;
            uint32_t v;
            std::memcpy(&v, rdram + addr, 4);
            return v;
        }

        inline float rdf(const uint8_t *rdram, uint32_t addr) {
            addr &= 0x1FFFFFFFu;
            if (addr + 4 > 0x2000000u) return 0.0f;
            float f;
            std::memcpy(&f, rdram + addr, 4);
            return f;
        }

        // Hook: ScanSectorList (FUN_0035FAE8)
        PS2_GUEST_CC void scanSectorListHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!g_enabled.load(std::memory_order_relaxed)) {
                if (g_origScanSectorList) g_origScanSectorList(rdram, ctx, runtime);
                return;
            }

            uint32_t frame = rd32(rdram, 0x004CD124u);  // CTimer::m_FrameCounter
            
            // Log sector scan
            if (g_scan_sector_limiter.should_log(frame)) {
                uint32_t entity = rd32(rdram, 0x004E4910u);  // player vehicle
                float camX = rdf(rdram, 0x006F4500u);
                float camY = rdf(rdram, 0x006F4504u);
                float camZ = rdf(rdram, 0x006F4508u);
                std::fprintf(stderr, "[stream_trace] %.1f ScanSectorList frame=%u cam=(%.1f,%.1f,%.1f)\n",
                    0.0, frame, camX, camY, camZ);
            }

            g_scan_sector_count.fetch_add(1, std::memory_order_relaxed);
            
            if (g_origScanSectorList) g_origScanSectorList(rdram, ctx, runtime);
        }

        // Hook: ScanBigBuildingList (FUN_0035FED0)
        PS2_GUEST_CC void scanBigBuildingListHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!g_enabled.load(std::memory_order_relaxed)) {
                if (g_origScanBigBuildingList) g_origScanBigBuildingList(rdram, ctx, runtime);
                return;
            }

            uint32_t frame = rd32(rdram, 0x004CD124u);
            
            if (g_scan_big_building_limiter.should_log(frame)) {
                std::fprintf(stderr, "[stream_trace] %.1f ScanBigBuildingList frame=%u\n", 0.0, frame);
            }

            g_scan_big_building_count.fetch_add(1, std::memory_order_relaxed);
            
            if (g_origScanBigBuildingList) g_origScanBigBuildingList(rdram, ctx, runtime);
        }

        // Hook: ScanSectorList_FrustumTriangle (FUN_0035DEA8)
        PS2_GUEST_CC void scanFrustumTriangleHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!g_enabled.load(std::memory_order_relaxed)) {
                if (g_origScanFrustumTriangle) g_origScanFrustumTriangle(rdram, ctx, runtime);
                return;
            }

            uint32_t frame = rd32(rdram, 0x004CD124u);
            
            if (g_frustum_triangle_limiter.should_log(frame)) {
                std::fprintf(stderr, "[stream_trace] %.1f ScanFrustumTriangle frame=%u\n", 0.0, frame);
            }

            g_frustum_triangle_count.fetch_add(1, std::memory_order_relaxed);
            
            if (g_origScanFrustumTriangle) g_origScanFrustumTriangle(rdram, ctx, runtime);
        }

        // Hook: SetupEntityVisibility (FUN_0035EFD8)
        PS2_GUEST_CC void setupEntityVisibilityHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!g_enabled.load(std::memory_order_relaxed)) {
                if (g_origSetupEntityVisibility) g_origSetupEntityVisibility(rdram, ctx, runtime);
                return;
            }

            uint32_t frame = rd32(rdram, 0x004CD124u);
            
            if (g_setup_visibility_limiter.should_log(frame)) {
                // The entity being checked is in $a0 (register 4)
                uint32_t entity = GPR_U32(ctx, 4);
                std::fprintf(stderr, "[stream_trace] %.1f SetupEntityVisibility frame=%u entity=0x%08x\n", 0.0, frame, entity);
            }

            g_setup_visibility_count.fetch_add(1, std::memory_order_relaxed);
            
            if (g_origSetupEntityVisibility) g_origSetupEntityVisibility(rdram, ctx, runtime);
        }

        // Hook: CStreaming::RequestModelFlags (FUN_003C7ED8)
        PS2_GUEST_CC void requestModelFlagsHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!g_enabled.load(std::memory_order_relaxed)) {
                if (g_origRequestModelFlags) g_origRequestModelFlags(rdram, ctx, runtime);
                return;
            }

            uint32_t frame = rd32(rdram, 0x004CD124u);
            
            // Model ID is in $a0 (register 4)
            uint32_t model_id = GPR_U32(ctx, 4);
            
            if (g_request_model_limiter.should_log(frame)) {
                std::fprintf(stderr, "[stream_trace] %.1f RequestModelFlags frame=%u model_id=%u\n", 0.0, frame, model_id);
            }

            g_request_model_count.fetch_add(1, std::memory_order_relaxed);
            
            if (g_origRequestModelFlags) g_origRequestModelFlags(rdram, ctx, runtime);
        }

    } // anonymous namespace

    void registerSettings()
    {
        settings::Spec s;
        s.key = "stream_trace.enabled";
        s.page = "Debug";
        s.section = "Streaming trace";
        s.label = "Enable streaming trace";
        s.help = "Enable hooks on ScanSectorList, ScanBigBuildingList, FrustumTriangle, SetupEntityVisibility, RequestModelFlags";
        s.type = settings::Type::Toggle;
        s.defaultValue = 0;
        s.env = "VCS_STREAM_TRACE";
        s.onChange = [](int v) { g_enabled.store(v != 0, std::memory_order_relaxed); };
        settings::add(s);

        s = {};
        s.key = "stream_trace.rate_limit";
        s.page = "Debug";
        s.section = "Streaming trace";
        s.label = "Rate limit (frames)";
        s.help = "Log once per N frames per event type";
        s.type = settings::Type::Integer;
        s.min = 1;
        s.max = 1000;
        s.format = "%d";
        s.defaultValue = 30;
        s.env = "VCS_STREAM_TRACE_RATE";
        s.onChange = [](int v) { 
            uint32_t rate = std::clamp(v, 1, 1000);
            g_scan_sector_limiter.interval = rate;
            g_scan_big_building_limiter.interval = rate;
            g_frustum_triangle_limiter.interval = rate;
            g_setup_visibility_limiter.interval = rate;
            g_request_model_limiter.interval = rate;
        };
        settings::add(s);
    }

    void install(PS2Runtime &runtime)
    {
        if (!g_enabled.load(std::memory_order_relaxed))
            return;

        if ((g_origScanSectorList = runtime.lookupFunction(0x0035FAE8u)))
            runtime.replaceFunction(0x0035FAE8u, &scanSectorListHook);
        
        if ((g_origScanBigBuildingList = runtime.lookupFunction(0x0035FED0u)))
            runtime.replaceFunction(0x0035FED0u, &scanBigBuildingListHook);
        
        if ((g_origScanFrustumTriangle = runtime.lookupFunction(0x0035DEA8u)))
            runtime.replaceFunction(0x0035DEA8u, &scanFrustumTriangleHook);
        
        if ((g_origSetupEntityVisibility = runtime.lookupFunction(0x0035EFD8u)))
            runtime.replaceFunction(0x0035EFD8u, &setupEntityVisibilityHook);
        
        if ((g_origRequestModelFlags = runtime.lookupFunction(0x003C7ED8u)))
            runtime.replaceFunction(0x003C7ED8u, &requestModelFlagsHook);

        // Debug command
        ps2control::addCommand("stream_trace", [](const std::string &args, ps2control::Reply reply) {
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                "stream_trace: enabled=%d\n"
                "  ScanSectorList: %llu calls\n"
                "  ScanBigBuildingList: %llu calls\n"
                "  ScanFrustumTriangle: %llu calls\n"
                "  SetupEntityVisibility: %llu calls\n"
                "  RequestModelFlags: %llu calls",
                g_enabled.load(),
                (unsigned long long)g_scan_sector_count.load(),
                (unsigned long long)g_scan_big_building_count.load(),
                (unsigned long long)g_frustum_triangle_count.load(),
                (unsigned long long)g_setup_visibility_count.load(),
                (unsigned long long)g_request_model_count.load());
            reply(buf);
        });

        std::fprintf(stderr, "[streaming_trace] hooks installed\n");
    }
}