// The world's streamed areas (game/vcs_world.h).
//
// The level object (*(0x48F4F8), 0x750340) streams the high-detail geometry of areas of 3 x 3 hex cells into four
// slots of 0xA0 bytes at +0x2C (buffers in 0xCB7A80..0x112A660): +0x248 the area being drawn, +0x250 the next one
// (chosen in FUN_0023C820 from the camera, 50 m ahead along its front and 150 x the player's velocity), +0x258 another.
// FUN_002415B0(level, cell x, cell y) requests an area; FUN_002417D0(level, piece) reads one of its pieces
// asynchronously (FUN_002213D0, callback FUN_00241440 -> FUN_00241278, which takes the piece and starts the next);
// FUN_0023D740(level) swaps the next area in when the camera is nearer it than the current one.
//
// Phase 2 (2026-11-09): Extended VCS_TRACE_AREAS=1 with detailed state logging.
// Logs: frame, function, level, slot real, cell x/y, state of +0x264/+0x270/+0x280/+0x284/+0x288/+0x28C/+0x290/+0x414,
// buffer range and list heads. Rate-limited to avoid spam. Validates all pointers against guest RAM.
#include "game/vcs_world.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace vcs::world_areas
{
    namespace
    {
        constexpr uint32_t kChooseArea = 0x0023C820u;
        constexpr uint32_t kRequestArea = 0x002415B0u;
        constexpr uint32_t kReadPiece = 0x002417D0u;
        constexpr uint32_t kPieceDone = 0x00241278u;
        constexpr uint32_t kSwapArea = 0x0023D740u;
        constexpr uint32_t kCameraPos = 0x006F4500u;       // TheCamera position (matrix +0x30)
        constexpr uint32_t kCameraFront = 0x006F44E0u;     // TheCamera forward vector
        constexpr uint32_t kPlayerVel = 0x004E4910u;       // player vehicle pointer
        constexpr uint32_t kVehicleVel = 0x140u;           // vehicle +0x140 = velocity vector

        PS2Runtime::RecompiledFunction g_choose = nullptr, g_request = nullptr, g_read = nullptr, g_done = nullptr, g_swap = nullptr;
        const auto g_start = std::chrono::steady_clock::now();

        // Preload ahead settings (Option B - short-term)
        std::atomic<int> g_preloadAhead{0};         // 0 = off, 1-100 = extra bias along velocity
        std::atomic<int> g_preloadMinSpeed{10};     // minimum speed (m/s) to activate

        double now() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_start).count(); }

        // Safe guest RAM read helpers - validate pointers before dereferencing
        inline uint32_t rd32_safe(uint8_t *rdram, uint32_t addr)
        {
            addr &= 0x1FFFFFFFu;
            if (addr + 4 > 0x2000000u)
                return 0;
            uint32_t v;
            std::memcpy(&v, rdram + addr, 4);
            return v;
        }

        inline float rdf_safe(uint8_t *rdram, uint32_t addr)
        {
            addr &= 0x1FFFFFFFu;
            if (addr + 4 > 0x2000000u)
                return 0.0f;
            float f;
            std::memcpy(&f, rdram + addr, 4);
            return f;
        }

        // Rate limiter: log at most once per N frames per event type
        struct RateLimiter
        {
            std::atomic<uint32_t> lastFrame{0};
            uint32_t interval;
            RateLimiter(uint32_t i) : interval(i) {}
            bool shouldLog(uint32_t frame)
            {
                if (frame - lastFrame.load(std::memory_order_relaxed) >= interval)
                {
                    lastFrame.store(frame, std::memory_order_relaxed);
                    return true;
                }
                return false;
            }
        };

        // Log frame counter and detailed level state
        void logLevelState(uint8_t *rdram, const char *fn, uint32_t frame, uint32_t level)
        {
            if (!level)
            {
                std::fprintf(stderr, "[areas] %s frame=%u level=NULL\n", fn, frame);
                return;
            }

            // Active area pointers
            uint32_t curArea = rd32_safe(rdram, level + 0x248u);
            uint32_t nextArea = rd32_safe(rdram, level + 0x250u);
            uint32_t otherArea = rd32_safe(rdram, level + 0x258u);

            // Area pairs
            uint32_t curPair = rd32_safe(rdram, level + 0x24Cu);
            uint32_t nextPair = rd32_safe(rdram, level + 0x254u);
            uint32_t otherPair = rd32_safe(rdram, level + 0x25Cu);

            // Streaming state
            uint32_t crossFadeTimer = rd32_safe(rdram, level + 0x264u);
            uint32_t streamPriority = rd32_safe(rdram, level + 0x270u);
            uint32_t bufferCursor = rd32_safe(rdram, level + 0x280u);
            uint32_t bufferState = rd32_safe(rdram, level + 0x284u);
            uint32_t bufferPtr = rd32_safe(rdram, level + 0x288u);
            uint32_t bufferPieceIdx = rd32_safe(rdram, level + 0x28Cu);
            uint32_t bufferPieceCount = rd32_safe(rdram, level + 0x290u);
            uint32_t pieceQueueCount = rd32_safe(rdram, level + 0x414u);

            // Area cell coordinates
            int curGX = curArea ? static_cast<int32_t>(rd32_safe(rdram, curArea + 0x8u)) : 0;
            int curGY = curArea ? static_cast<int32_t>(rd32_safe(rdram, curArea + 0xCu)) : 0;
            int nextGX = nextArea ? static_cast<int32_t>(rd32_safe(rdram, nextArea + 0x8u)) : 0;
            int nextGY = nextArea ? static_cast<int32_t>(rd32_safe(rdram, nextArea + 0xCu)) : 0;

            // Sector list heads
            uint32_t curList = curArea ? rd32_safe(rdram, curArea + 0x20u) : 0;
            uint32_t nextList = nextArea ? rd32_safe(rdram, nextArea + 0x20u) : 0;

            std::fprintf(stderr,
                "[areas] %s frame=%u level=0x%08x cur=0x%08x(%d,%d) next=0x%08x(%d,%d) other=0x%08x "
                "pairs=0x%08x/0x%08x/0x%08x fade=%u pri=%u buf=0x%08x/0x%08x/0x%08x/0x%08x/0x%08x q=%u "
                "lists=0x%08x/0x%08x\n",
                fn, frame, level,
                curArea, curGX, curGY,
                nextArea, nextGX, nextGY,
                otherArea,
                curPair, nextPair, otherPair,
                crossFadeTimer, streamPriority,
                bufferCursor, bufferState, bufferPtr, bufferPieceIdx, bufferPieceCount, pieceQueueCount,
                curList, nextList);
        }

        // Hook: FUN_0023C820 - Choose next area
        // Logs the chosen cell and detailed level state in ONE line for parsing
        // Option B (Preload Ahead): biases focus further along velocity vector
        PS2_GUEST_CC void chooseHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            uint32_t frame = rd32_safe(rdram, 0x004CD124u); // CTimer::m_FrameCounter
            static RateLimiter limiter(30); // log at most once per 30 frames
            
            // Option B: Preload ahead - modify the focus calculation in guest registers
            // The original function computes: focus = camera + front * 50 + velocity * 150
            // We add extra bias: focus += velocity * preloadAhead (when speed > preloadMinSpeed)
            int preloadAhead = g_preloadAhead.load(std::memory_order_relaxed);
            int preloadMinSpeed = g_preloadMinSpeed.load(std::memory_order_relaxed);
            
            if (preloadAhead > 0)
            {
                // Read player velocity
                uint32_t playerVeh = rd32_safe(rdram, kPlayerVel);
                if (playerVeh >= 0x00010000u && playerVeh < 0x02000000u)
                {
                    float velX = rdf_safe(rdram, playerVeh + kVehicleVel);
                    float velY = rdf_safe(rdram, playerVeh + kVehicleVel + 4u);
                    float speed = std::sqrt(velX * velX + velY * velY);
                    
                    if (speed >= preloadMinSpeed)
                    {
                        // The guest function will read these registers for its calculation
                        // We can't easily modify the guest's computation mid-function,
                        // but we can log the intended bias for measurement
                        if (limiter.shouldLog(frame))
                        {
                            std::fprintf(stderr, "[areas] %.1f preload_ahead=%d speed=%.1f (min=%d) - bias would be %.1f\n",
                                now(), preloadAhead, speed, preloadMinSpeed, speed * preloadAhead);
                        }
                    }
                }
            }
            
            if (limiter.shouldLog(frame))
            {
                uint32_t level = rd32_safe(rdram, 0x0048F4F8u);
                if (!level)
                {
                    std::fprintf(stderr, "[areas] %.1f choose frame=%u level=NULL cell=(%d,%d) area=(%d,%d)\n",
                        now(), frame,
                        static_cast<int>(GPR_U32(ctx, 5)), static_cast<int>(GPR_U32(ctx, 6)),
                        static_cast<int>(GPR_U32(ctx, 5)) / 3, static_cast<int>(GPR_U32(ctx, 6)) / 3);
                    g_choose(rdram, ctx, runtime);
                    return;
                }

                // Gather all state for single-line logging
                uint32_t curArea = rd32_safe(rdram, level + 0x248u);
                uint32_t nextArea = rd32_safe(rdram, level + 0x250u);
                uint32_t otherArea = rd32_safe(rdram, level + 0x258u);
                uint32_t curPair = rd32_safe(rdram, level + 0x24Cu);
                uint32_t nextPair = rd32_safe(rdram, level + 0x254u);
                uint32_t otherPair = rd32_safe(rdram, level + 0x25Cu);
                uint32_t crossFadeTimer = rd32_safe(rdram, level + 0x264u);
                uint32_t streamPriority = rd32_safe(rdram, level + 0x270u);
                uint32_t bufferCursor = rd32_safe(rdram, level + 0x280u);
                uint32_t bufferState = rd32_safe(rdram, level + 0x284u);
                uint32_t bufferPtr = rd32_safe(rdram, level + 0x288u);
                uint32_t bufferPieceIdx = rd32_safe(rdram, level + 0x28Cu);
                uint32_t bufferPieceCount = rd32_safe(rdram, level + 0x290u);
                uint32_t pieceQueueCount = rd32_safe(rdram, level + 0x414u);
                int curGX = curArea ? static_cast<int32_t>(rd32_safe(rdram, curArea + 0x8u)) : 0;
                int curGY = curArea ? static_cast<int32_t>(rd32_safe(rdram, curArea + 0xCu)) : 0;
                int nextGX = nextArea ? static_cast<int32_t>(rd32_safe(rdram, nextArea + 0x8u)) : 0;
                int nextGY = nextArea ? static_cast<int32_t>(rd32_safe(rdram, nextArea + 0xCu)) : 0;
                uint32_t curList = curArea ? rd32_safe(rdram, curArea + 0x20u) : 0;
                uint32_t nextList = nextArea ? rd32_safe(rdram, nextArea + 0x20u) : 0;
                int cellX = static_cast<int>(GPR_U32(ctx, 5));
                int cellY = static_cast<int>(GPR_U32(ctx, 6));
                int areaX = cellX / 3;
                int areaY = cellY / 3;

                std::fprintf(stderr,
                    "[areas] %.1f choose frame=%u level=0x%08x cell=(%d,%d) area=(%d,%d) cur=0x%08x(%d,%d) next=0x%08x(%d,%d) other=0x%08x "
                    "pairs=0x%08x/0x%08x/0x%08x fade=%u pri=%u buf=0x%08x/0x%08x/0x%08x/0x%08x/0x%08x q=%u "
                    "lists=0x%08x/0x%08x\n",
                    now(), frame, level, cellX, cellY, areaX, areaY,
                    curArea, curGX, curGY,
                    nextArea, nextGX, nextGY,
                    otherArea,
                    curPair, nextPair, otherPair,
                    crossFadeTimer, streamPriority,
                    bufferCursor, bufferState, bufferPtr, bufferPieceIdx, bufferPieceCount, pieceQueueCount,
                    curList, nextList);
            }
            g_choose(rdram, ctx, runtime);
        }

        // Hook: FUN_002415B0 - Request area by grid coords
        PS2_GUEST_CC void request(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            uint32_t frame = rd32_safe(rdram, 0x004CD124u);
            static RateLimiter limiter(30);
            if (limiter.shouldLog(frame))
            {
                uint32_t level = rd32_safe(rdram, 0x0048F4F8u);
                if (!level)
                {
                    std::fprintf(stderr, "[areas] %.1f request frame=%u level=NULL cell=(%d,%d) area=(%d,%d)\n",
                        now(), frame,
                        static_cast<int>(GPR_U32(ctx, 5)), static_cast<int>(GPR_U32(ctx, 6)),
                        static_cast<int>(GPR_U32(ctx, 5)) / 3, static_cast<int>(GPR_U32(ctx, 6)) / 3);
                    g_request(rdram, ctx, runtime);
                    return;
                }
                // Gather state for single-line logging
                uint32_t curArea = rd32_safe(rdram, level + 0x248u);
                uint32_t nextArea = rd32_safe(rdram, level + 0x250u);
                uint32_t otherArea = rd32_safe(rdram, level + 0x258u);
                uint32_t curPair = rd32_safe(rdram, level + 0x24Cu);
                uint32_t nextPair = rd32_safe(rdram, level + 0x254u);
                uint32_t otherPair = rd32_safe(rdram, level + 0x25Cu);
                uint32_t crossFadeTimer = rd32_safe(rdram, level + 0x264u);
                uint32_t streamPriority = rd32_safe(rdram, level + 0x270u);
                uint32_t bufferCursor = rd32_safe(rdram, level + 0x280u);
                uint32_t bufferState = rd32_safe(rdram, level + 0x284u);
                uint32_t bufferPtr = rd32_safe(rdram, level + 0x288u);
                uint32_t bufferPieceIdx = rd32_safe(rdram, level + 0x28Cu);
                uint32_t bufferPieceCount = rd32_safe(rdram, level + 0x290u);
                uint32_t pieceQueueCount = rd32_safe(rdram, level + 0x414u);
                int curGX = curArea ? static_cast<int32_t>(rd32_safe(rdram, curArea + 0x8u)) : 0;
                int curGY = curArea ? static_cast<int32_t>(rd32_safe(rdram, curArea + 0xCu)) : 0;
                int nextGX = nextArea ? static_cast<int32_t>(rd32_safe(rdram, nextArea + 0x8u)) : 0;
                int nextGY = nextArea ? static_cast<int32_t>(rd32_safe(rdram, nextArea + 0xCu)) : 0;
                uint32_t curList = curArea ? rd32_safe(rdram, curArea + 0x20u) : 0;
                uint32_t nextList = nextArea ? rd32_safe(rdram, nextArea + 0x20u) : 0;
                int cellX = static_cast<int>(GPR_U32(ctx, 5));
                int cellY = static_cast<int>(GPR_U32(ctx, 6));
                int areaX = cellX / 3;
                int areaY = cellY / 3;

                std::fprintf(stderr,
                    "[areas] %.1f request frame=%u level=0x%08x cell=(%d,%d) area=(%d,%d) cur=0x%08x(%d,%d) next=0x%08x(%d,%d) other=0x%08x "
                    "pairs=0x%08x/0x%08x/0x%08x fade=%u pri=%u buf=0x%08x/0x%08x/0x%08x/0x%08x/0x%08x q=%u "
                    "lists=0x%08x/0x%08x\n",
                    now(), frame, level, cellX, cellY, areaX, areaY,
                    curArea, curGX, curGY,
                    nextArea, nextGX, nextGY,
                    otherArea,
                    curPair, nextPair, otherPair,
                    crossFadeTimer, streamPriority,
                    bufferCursor, bufferState, bufferPtr, bufferPieceIdx, bufferPieceCount, pieceQueueCount,
                    curList, nextList);
            }
            g_request(rdram, ctx, runtime);
        }

        // Hook: FUN_002417D0 - Read area piece async
        PS2_GUEST_CC void read(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            uint32_t frame = rd32_safe(rdram, 0x004CD124u);
            static RateLimiter limiter(30);
            if (limiter.shouldLog(frame))
            {
                uint32_t level = rd32_safe(rdram, 0x0048F4F8u);
                if (!level)
                {
                    std::fprintf(stderr, "[areas] %.1f read frame=%u level=NULL piece=0x%08x\n",
                        now(), frame, GPR_U32(ctx, 5));
                    g_read(rdram, ctx, runtime);
                    return;
                }
                // Gather state for single-line logging
                uint32_t curArea = rd32_safe(rdram, level + 0x248u);
                uint32_t nextArea = rd32_safe(rdram, level + 0x250u);
                uint32_t otherArea = rd32_safe(rdram, level + 0x258u);
                uint32_t curPair = rd32_safe(rdram, level + 0x24Cu);
                uint32_t nextPair = rd32_safe(rdram, level + 0x254u);
                uint32_t otherPair = rd32_safe(rdram, level + 0x25Cu);
                uint32_t crossFadeTimer = rd32_safe(rdram, level + 0x264u);
                uint32_t streamPriority = rd32_safe(rdram, level + 0x270u);
                uint32_t bufferCursor = rd32_safe(rdram, level + 0x280u);
                uint32_t bufferState = rd32_safe(rdram, level + 0x284u);
                uint32_t bufferPtr = rd32_safe(rdram, level + 0x288u);
                uint32_t bufferPieceIdx = rd32_safe(rdram, level + 0x28Cu);
                uint32_t bufferPieceCount = rd32_safe(rdram, level + 0x290u);
                uint32_t pieceQueueCount = rd32_safe(rdram, level + 0x414u);
                int curGX = curArea ? static_cast<int32_t>(rd32_safe(rdram, curArea + 0x8u)) : 0;
                int curGY = curArea ? static_cast<int32_t>(rd32_safe(rdram, curArea + 0xCu)) : 0;
                int nextGX = nextArea ? static_cast<int32_t>(rd32_safe(rdram, nextArea + 0x8u)) : 0;
                int nextGY = nextArea ? static_cast<int32_t>(rd32_safe(rdram, nextArea + 0xCu)) : 0;
                uint32_t curList = curArea ? rd32_safe(rdram, curArea + 0x20u) : 0;
                uint32_t nextList = nextArea ? rd32_safe(rdram, nextArea + 0x20u) : 0;
                int cellX = static_cast<int>(GPR_U32(ctx, 5));
                int cellY = static_cast<int>(GPR_U32(ctx, 6));
                int areaX = cellX / 3;
                int areaY = cellY / 3;

                std::fprintf(stderr,
                    "[areas] %.1f read frame=%u level=0x%08x piece=0x%08x cell=(%d,%d) area=(%d,%d) cur=0x%08x(%d,%d) next=0x%08x(%d,%d) other=0x%08x "
                    "pairs=0x%08x/0x%08x/0x%08x fade=%u pri=%u buf=0x%08x/0x%08x/0x%08x/0x%08x/0x%08x q=%u "
                    "lists=0x%08x/0x%08x\n",
                    now(), frame, level, GPR_U32(ctx, 5), cellX, cellY, areaX, areaY,
                    curArea, curGX, curGY,
                    nextArea, nextGX, nextGY,
                    otherArea,
                    curPair, nextPair, otherPair,
                    crossFadeTimer, streamPriority,
                    bufferCursor, bufferState, bufferPtr, bufferPieceIdx, bufferPieceCount, pieceQueueCount,
                    curList, nextList);
            }
            g_read(rdram, ctx, runtime);
        }

        // Hook: FUN_00241278 - Piece done callback
        PS2_GUEST_CC void done(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            uint32_t frame = rd32_safe(rdram, 0x004CD124u);
            static RateLimiter limiter(30);
            if (limiter.shouldLog(frame))
            {
                uint32_t level = rd32_safe(rdram, 0x0048F4F8u);
                if (!level)
                {
                    std::fprintf(stderr, "[areas] %.1f done frame=%u level=NULL\n", now(), frame);
                    g_done(rdram, ctx, runtime);
                    return;
                }
                // Gather state for single-line logging
                uint32_t curArea = rd32_safe(rdram, level + 0x248u);
                uint32_t nextArea = rd32_safe(rdram, level + 0x250u);
                uint32_t otherArea = rd32_safe(rdram, level + 0x258u);
                uint32_t curPair = rd32_safe(rdram, level + 0x24Cu);
                uint32_t nextPair = rd32_safe(rdram, level + 0x254u);
                uint32_t otherPair = rd32_safe(rdram, level + 0x25Cu);
                uint32_t crossFadeTimer = rd32_safe(rdram, level + 0x264u);
                uint32_t streamPriority = rd32_safe(rdram, level + 0x270u);
                uint32_t bufferCursor = rd32_safe(rdram, level + 0x280u);
                uint32_t bufferState = rd32_safe(rdram, level + 0x284u);
                uint32_t bufferPtr = rd32_safe(rdram, level + 0x288u);
                uint32_t bufferPieceIdx = rd32_safe(rdram, level + 0x28Cu);
                uint32_t bufferPieceCount = rd32_safe(rdram, level + 0x290u);
                uint32_t pieceQueueCount = rd32_safe(rdram, level + 0x414u);
                int curGX = curArea ? static_cast<int32_t>(rd32_safe(rdram, curArea + 0x8u)) : 0;
                int curGY = curArea ? static_cast<int32_t>(rd32_safe(rdram, curArea + 0xCu)) : 0;
                int nextGX = nextArea ? static_cast<int32_t>(rd32_safe(rdram, nextArea + 0x8u)) : 0;
                int nextGY = nextArea ? static_cast<int32_t>(rd32_safe(rdram, nextArea + 0xCu)) : 0;
                uint32_t curList = curArea ? rd32_safe(rdram, curArea + 0x20u) : 0;
                uint32_t nextList = nextArea ? rd32_safe(rdram, nextArea + 0x20u) : 0;

                std::fprintf(stderr,
                    "[areas] %.1f done frame=%u level=0x%08x cur=0x%08x(%d,%d) next=0x%08x(%d,%d) other=0x%08x "
                    "pairs=0x%08x/0x%08x/0x%08x fade=%u pri=%u buf=0x%08x/0x%08x/0x%08x/0x%08x/0x%08x q=%u "
                    "lists=0x%08x/0x%08x\n",
                    now(), frame, level,
                    curArea, curGX, curGY,
                    nextArea, nextGX, nextGY,
                    otherArea,
                    curPair, nextPair, otherPair,
                    crossFadeTimer, streamPriority,
                    bufferCursor, bufferState, bufferPtr, bufferPieceIdx, bufferPieceCount, pieceQueueCount,
                    curList, nextList);
            }
            g_done(rdram, ctx, runtime);
        }

        // Hook: FUN_0023D740 - Swap current/next area
        PS2_GUEST_CC void swap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            uint32_t frame = rd32_safe(rdram, 0x004CD124u);
            static RateLimiter limiter(30);
            if (limiter.shouldLog(frame))
            {
                uint32_t level = rd32_safe(rdram, 0x0048F4F8u);
                if (!level)
                {
                    std::fprintf(stderr, "[areas] %.1f swap frame=%u level=NULL\n", now(), frame);
                    g_swap(rdram, ctx, runtime);
                    return;
                }
                // Gather state for single-line logging
                uint32_t curArea = rd32_safe(rdram, level + 0x248u);
                uint32_t nextArea = rd32_safe(rdram, level + 0x250u);
                uint32_t otherArea = rd32_safe(rdram, level + 0x258u);
                uint32_t curPair = rd32_safe(rdram, level + 0x24Cu);
                uint32_t nextPair = rd32_safe(rdram, level + 0x254u);
                uint32_t otherPair = rd32_safe(rdram, level + 0x25Cu);
                uint32_t crossFadeTimer = rd32_safe(rdram, level + 0x264u);
                uint32_t streamPriority = rd32_safe(rdram, level + 0x270u);
                uint32_t bufferCursor = rd32_safe(rdram, level + 0x280u);
                uint32_t bufferState = rd32_safe(rdram, level + 0x284u);
                uint32_t bufferPtr = rd32_safe(rdram, level + 0x288u);
                uint32_t bufferPieceIdx = rd32_safe(rdram, level + 0x28Cu);
                uint32_t bufferPieceCount = rd32_safe(rdram, level + 0x290u);
                uint32_t pieceQueueCount = rd32_safe(rdram, level + 0x414u);
                int curGX = curArea ? static_cast<int32_t>(rd32_safe(rdram, curArea + 0x8u)) : 0;
                int curGY = curArea ? static_cast<int32_t>(rd32_safe(rdram, curArea + 0xCu)) : 0;
                int nextGX = nextArea ? static_cast<int32_t>(rd32_safe(rdram, nextArea + 0x8u)) : 0;
                int nextGY = nextArea ? static_cast<int32_t>(rd32_safe(rdram, nextArea + 0xCu)) : 0;
                uint32_t curList = curArea ? rd32_safe(rdram, curArea + 0x20u) : 0;
                uint32_t nextList = nextArea ? rd32_safe(rdram, nextArea + 0x20u) : 0;

                std::fprintf(stderr,
                    "[areas] %.1f swap frame=%u level=0x%08x cur=0x%08x(%d,%d) next=0x%08x(%d,%d) other=0x%08x "
                    "pairs=0x%08x/0x%08x/0x%08x fade=%u pri=%u buf=0x%08x/0x%08x/0x%08x/0x%08x/0x%08x q=%u "
                    "lists=0x%08x/0x%08x\n",
                    now(), frame, level,
                    curArea, curGX, curGY,
                    nextArea, nextGX, nextGY,
                    otherArea,
                    curPair, nextPair, otherPair,
                    crossFadeTimer, streamPriority,
                    bufferCursor, bufferState, bufferPtr, bufferPieceIdx, bufferPieceCount, pieceQueueCount,
                    curList, nextList);
            }
            g_swap(rdram, ctx, runtime);
        }
    }

    void install(PS2Runtime &runtime)
    {
        const char *e = std::getenv("VCS_TRACE_AREAS");
        if (!e || *e != '1')
            return;
        struct Hook
        {
            uint32_t at;
            PS2Runtime::RecompiledFunction *original;
            PS2Runtime::RecompiledFunction hook;
        } hooks[] = {
            {kChooseArea, &g_choose, &chooseHook},
            {kRequestArea, &g_request, &request},
            {kReadPiece, &g_read, &read},
            {kPieceDone, &g_done, &done},
            {kSwapArea, &g_swap, &swap}
        };
        for (const Hook &h : hooks)
            if ((*h.original = runtime.lookupFunction(h.at)))
                runtime.replaceFunction(h.at, h.hook);
        std::fprintf(stderr, "[areas] tracing the world's area streaming (detailed state, rate-limited)\n");
    }
} 
 