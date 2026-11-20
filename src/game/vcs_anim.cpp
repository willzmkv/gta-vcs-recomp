// See vcs_anim.h and PERFORMANCE.md 48.
#include "game/vcs_anim.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_control.h"

#include <immintrin.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(VCS_ANIM_WORKER)
namespace vcs_anim_worker
{
    using Fn = void (*)(uint8_t *, R5900Context *__restrict, PS2Runtime *);
}
vcs_anim_worker::Fn vcsAnimWorkerCallback(uint32_t address); // generated/anim/anim_worker.cpp
#endif

namespace vcs::anim
{
    namespace
    {
        // FUN_00401ad8(ad $a0, callback $a1, args $a2): for each of the clump's nodes (ad + 8 the count, ad + 0x10 the
        // array, 0x18 bytes each) the callback (node, args). The callbacks: 0x155068 quaternion + translation (skinned
        // clumps: peds), 0x155c08 3x3 matrix + position, 0x1552F8 root motion only (an animation update that skips the
        // pose). args: a flag word, then the node-state pointer of each association (node i's state at + 0x20 i), 0
        // ended; each callback moves every pointer on by 0x20 (the next node's), so a slice of nodes from node s starts
        // with the pointers + 0x20 s. A root-motion node (its flags & 8, the clump's delta ad + 0xC set) also writes
        // that delta: one node a clump; more, and the loop stays in one piece (in order).
        constexpr uint32_t kNodeLoop = 0x00401AD8u;
        // the clump update (FUN_00154d10): verify compares the last loop at its entry - before a resync of node states
        // (FUN_00154f70) or anything else of the next clump's update changes them
        constexpr uint32_t kClumpUpdate = 0x00154D10u;
        constexpr uint32_t kQuatCallback = 0x00155068u, kMatrixCallback = 0x00155C08u;
        constexpr uint32_t kNodeSize = 0x18u, kStateSize = 0x20u;
        constexpr uint32_t kFrameCounter = 0x004CD124u; // the game's frame number (verify: same-frame compares only)
        constexpr uint32_t kMaxArgs = 12u; // the flag, at most 11 associations (FUN_00154d10's limit)
        // Guest memory of the workers: the EE kernel's area below the game, unused in play (game-internals.md 12c: all
        // zero but a few words at 0x10000, 0x74000, 0x7C000; vcs_aim.cpp keeps 0x60000): 16 KB each, a slice's copy of
        // args at its start, its stack down from its end.
        constexpr uint32_t kGuestArea = 0x00020000u, kGuestSlot = 0x4000u, kMaxSlots = 16u;

        enum class Mode
        {
            Off,
            Serial,
            Parallel,
            Verify,
            Slices, // the slices against the copies in one piece
        };
        Mode g_mode = Mode::Parallel;
        PS2Runtime::RecompiledFunction g_origLoop = nullptr, g_origUpdate = nullptr;
        PS2Runtime *g_runtime = nullptr;

        uint32_t rd32(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (a & PS2_RAM_MASK), 4);
            return v;
        }
        void wr32(uint8_t *rdram, uint32_t a, uint32_t v) { std::memcpy(rdram + (a & PS2_RAM_MASK), &v, 4); }

        std::atomic<uint64_t> g_loops{0}, g_parallel{0}, g_nodes{0}, g_verified{0}, g_mismatches{0}, g_fallback{0}, g_freed{0}, g_late{0}, g_together{0};

#if defined(VCS_ANIM_WORKER)
        // One slice of a clump's nodes: [first, end), on its own context and guest slot.
        struct Slice
        {
            vcs_anim_worker::Fn fn = nullptr;
            uint32_t ad = 0, first = 0, end = 0, args = 0, argCount = 0, slot = 0;
            const R5900Context *base = nullptr;
            bool advance = false; // the args' pointers move on 0x20 a node (args[1] set)
            uint32_t csr = 0;     // the game thread's MXCSR (rounding, flush to zero): the same on every thread
        };

        void runSlice(uint8_t *rdram, const Slice &s)
        {
            _mm_setcsr(s.csr);
            // its copy of args, the pointers where node `first` finds them
            const uint32_t args = kGuestArea + s.slot * kGuestSlot;
            for (uint32_t k = 0; k < s.argCount; ++k)
            {
                uint32_t v = rd32(rdram, s.args + k * 4u);
                if (k > 0 && v && s.advance)
                    v += kStateSize * s.first;
                wr32(rdram, args + k * 4u, v);
            }
            wr32(rdram, args + s.argCount * 4u, 0);
            alignas(16) R5900Context ctx = *s.base;
            const uint32_t nodes = rd32(rdram, s.ad + 0x10u);
            for (uint32_t i = s.first; i < s.end; ++i)
            {
                SET_GPR_U32(&ctx, 4, nodes + i * kNodeSize);
                SET_GPR_U32(&ctx, 5, args);
                SET_GPR_U32(&ctx, 29, kGuestArea + (s.slot + 1u) * kGuestSlot - 0x40u);
                SET_GPR_U32(&ctx, 31, 0x00401B24u); // after the loop's JALR
                ctx.pc = s.base->pc;
                s.fn(rdram, &ctx, g_runtime);
            }
        }

        // ---- The workers: slices handed out per clump; the game thread takes the first and waits for the rest -------
        class Pool
        {
        public:
            explicit Pool(unsigned workers)
            {
                for (unsigned i = 0; i < workers; ++i)
                    m_threads.emplace_back([this, i] { loop(i); });
            }
            ~Pool()
            {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_quit = true;
                }
                m_wake.notify_all();
                for (std::thread &t : m_threads)
                    t.join();
            }
            unsigned workers() const { return static_cast<unsigned>(m_threads.size()); }

            // slices[0] on the calling thread, the others on the workers; back when all are done
            void run(uint8_t *rdram, const Slice *slices, unsigned count)
            {
                m_rdram = rdram;
                m_slices = slices;
                m_left.store(count - 1, std::memory_order_relaxed);
                // a slice is claimed with one compare-exchange of (run, its slice count, next index): a worker still
                // finishing the last run can never take one of this run's, nor judge this run's index by the last
                // run's count (the reviewer's find: a slice run twice, m_left wrong)
                const uint64_t run = ++m_run & 0xFFFFFFFFu;
                m_claim.store(run << 32 | uint64_t(count) << 16 | 1u, std::memory_order_seq_cst);
                if (m_sleeping.load(std::memory_order_seq_cst)) // (seq_cst both sides: a worker going to sleep sees the run)
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_wake.notify_all();
                }
                runSlice(rdram, slices[0]);
                // the game thread helps with what no worker took yet
                for (unsigned i; claim(run, i);)
                {
                    runSlice(rdram, slices[i]);
                    m_left.fetch_sub(1, std::memory_order_acq_rel);
                }
                while (m_left.load(std::memory_order_acquire) != 0)
                    _mm_pause();
            }

        private:
            // the next slice of run `run`; false when it has none left (or another run began)
            bool claim(uint64_t run, unsigned &index)
            {
                uint64_t c = m_claim.load(std::memory_order_acquire);
                for (;;)
                {
                    if ((c >> 32) != run || (c & 0xFFFFu) >= ((c >> 16) & 0xFFFFu))
                        return false;
                    if (m_claim.compare_exchange_weak(c, c + 1u, std::memory_order_acq_rel, std::memory_order_acquire))
                    {
                        index = static_cast<unsigned>(c & 0xFFFFu);
                        return true;
                    }
                }
            }

            void loop(unsigned)
            {
                uint64_t seen = 0;
                for (;;)
                {
                    // spin a while for the next clump (they come every few microseconds through the world's update),
                    // then sleep until woken
                    uint64_t run = m_claim.load(std::memory_order_acquire) >> 32;
                    for (int spin = 0; run == seen && spin < 4000; ++spin)
                    {
                        _mm_pause();
                        run = m_claim.load(std::memory_order_acquire) >> 32;
                    }
                    if (run == seen)
                    {
                        std::unique_lock<std::mutex> lock(m_mutex);
                        m_sleeping.fetch_add(1, std::memory_order_seq_cst);
                        m_wake.wait(lock, [&] { return m_quit || (m_claim.load(std::memory_order_seq_cst) >> 32) != seen; });
                        m_sleeping.fetch_sub(1, std::memory_order_acq_rel);
                        if (m_quit)
                            return;
                        run = m_claim.load(std::memory_order_acquire) >> 32;
                    }
                    seen = run;
                    for (unsigned i; claim(run, i);)
                    {
                        runSlice(m_rdram, m_slices[i]);
                        m_left.fetch_sub(1, std::memory_order_acq_rel);
                    }
                }
            }

            std::vector<std::thread> m_threads;
            std::mutex m_mutex;
            std::condition_variable m_wake;
            bool m_quit = false;
            uint64_t m_run = 0;                // the game thread's
            std::atomic<uint64_t> m_claim{0}; // (run << 32) | (slice count << 16) | the next slice index
            std::atomic<unsigned> m_sleeping{0}, m_left{0};
            uint8_t *m_rdram = nullptr;
            const Slice *m_slices = nullptr;
        };
        std::unique_ptr<Pool> g_pool;

        // ---- Verify: the copies' results kept, the guest memory put back, the game's loop compared at the next one --
        struct Region
        {
            uint32_t addr, size;
        };
        struct Pending
        {
            std::vector<Region> regions;
            std::vector<uint8_t> native; // the copies' bytes, region after region
            uint32_t callback = 0;
            uint32_t frame = 0; // the game frame it ran in
            uint32_t nodes = 0, slices = 0;
            bool open = false;
            bool strict = false; // compared right away (Slices): every difference counts
        } g_pending;

        // what a loop writes: each node's frame (+0x10 -> 0x50 bytes covers both output forms), each association's node
        // states, the root delta. Not the args: they are on the caller's stack, written over by the next clump before the
        // compare (every verify differed there and only there); their end - each pointer count nodes on - is plain.
        std::vector<Region> regionsOf(const uint8_t *rdram, uint32_t ad, uint32_t args, uint32_t argCount)
        {
            std::vector<Region> out;
            const uint32_t count = rd32(rdram, ad + 8u), nodes = rd32(rdram, ad + 0x10u);
            for (uint32_t i = 0; i < count; ++i)
                out.push_back({rd32(rdram, nodes + i * kNodeSize + 0x10u), 0x50u});
            for (uint32_t k = 1; k < argCount; ++k)
                out.push_back({rd32(rdram, args + k * 4u), kStateSize * count});
            if (const uint32_t delta = rd32(rdram, ad + 0xCu))
                out.push_back({delta, 16u});
            return out;
        }
        void save(const uint8_t *rdram, const std::vector<Region> &regions, std::vector<uint8_t> &bytes)
        {
            bytes.clear();
            for (const Region &r : regions)
                bytes.insert(bytes.end(), rdram + (r.addr & PS2_RAM_MASK), rdram + (r.addr & PS2_RAM_MASK) + r.size);
        }
        // (a pointer read from the game near the top of RAM: its region would run past the end)
        void bound(std::vector<Region> &regions)
        {
            regions.erase(std::remove_if(regions.begin(), regions.end(),
                                         [](const Region &r) { return (r.addr & PS2_RAM_MASK) + uint64_t(r.size) > PS2_RAM_MASK + 1ull; }),
                          regions.end());
        }
        void restore(uint8_t *rdram, const std::vector<Region> &regions, const std::vector<uint8_t> &bytes)
        {
            size_t at = 0;
            for (const Region &r : regions)
            {
                std::memcpy(rdram + (r.addr & PS2_RAM_MASK), bytes.data() + at, r.size);
                at += r.size;
            }
        }
        void compare(const uint8_t *rdram)
        {
            if (!g_pending.open)
                return;
            g_pending.open = false;
            // only within the same game frame: later the game itself turns some bones after the update (a head looking,
            // a torso aiming) - the last clump of the world's pass compared after that showed it
            if (rd32(rdram, kFrameCounter) != g_pending.frame)
            {
                g_late.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            std::vector<uint8_t> guest;
            save(rdram, g_pending.regions, guest);
            g_verified.fetch_add(1, std::memory_order_relaxed);
            if (guest == g_pending.native)
                return;
            // a region all zero on the game's side was freed in between (an association blended out and deleted by the
            // next clump's update, its node states cleared): not compared - unless nothing ran in between (Slices)
            bool real = g_pending.strict;
            {
                size_t at = 0;
                for (const Region &r : g_pending.regions)
                {
                    const bool same = std::memcmp(guest.data() + at, g_pending.native.data() + at, r.size) == 0;
                    const bool cleared = std::all_of(guest.begin() + at, guest.begin() + at + r.size, [](uint8_t b) { return b == 0; });
                    real = real || (!same && !cleared);
                    at += r.size;
                }
            }
            if (!real)
            {
                g_freed.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            const uint64_t n = g_mismatches.fetch_add(1, std::memory_order_relaxed);
            if (n < 8)
            {
                size_t at = 0;
                unsigned index = 0;
                for (const Region &r : g_pending.regions)
                {
                    for (uint32_t b = 0; b < r.size; ++b)
                        if (guest[at + b] != g_pending.native[at + b])
                        {
                            std::fprintf(stderr, "[anim] verify: callback 0x%x, region %u of %zu (0x%x+0x%x; the first %u are the "
                                                 "frames) byte 0x%x: game %02x, copy %02x; %u slice(s)\n",
                                         g_pending.callback, index, g_pending.regions.size(), r.addr, r.size, g_pending.nodes, b,
                                         guest[at + b], g_pending.native[at + b], g_pending.slices);
                            break;
                        }
                    at += r.size;
                    ++index;
                }
            }
        }

        // Each node's states must be its own: an association's node states are as many as its animation's nodes (the
        // animation at association + 0x20, its node count at + 0x18; the association from a node state's + 0x18), and
        // a clump with more nodes reads past the array - into another association's, which another node advances too
        // (the order then decides: in slices it would not be the game's). So: every array as long as the clump, none
        // overlapping another; else the loop stays in one piece. (Verify found one clump differing in slices only.)
        bool statesApart(const uint8_t *rdram, uint32_t ad, uint32_t callback, uint32_t args, uint32_t argCount, uint32_t count)
        {
            // and each node's frame its own: what one writes ([+8, +0x24) quaternion and position, [+0x10, +0x4C) the
            // matrix form) apart from every other node's
            const uint32_t nodes = rd32(rdram, ad + 0x10u);
            const uint32_t wLo = callback == kQuatCallback ? 8u : 0x10u, wHi = callback == kQuatCallback ? 0x24u : 0x4Cu;
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t fi = rd32(rdram, nodes + i * kNodeSize + 0x10u);
                for (uint32_t j = 0; j < i; ++j)
                {
                    const uint32_t fj = rd32(rdram, nodes + j * kNodeSize + 0x10u);
                    if (fi + wLo < fj + wHi && fj + wLo < fi + wHi)
                        return false;
                }
            }
            // the arrays: what each one's nodes read - all `count` of them - must not reach what another one's nodes write
            // (its own: the animation's node count; an animation of fewer nodes than the clump is common - a ped's: the
            // nodes past it find no sequence there and skip, writing nothing; slices against one piece: 0 differences in
            // 16189 loops with no guard at all)
            uint32_t lo[kMaxArgs], readHi[kMaxArgs], writeHi[kMaxArgs];
            for (uint32_t k = 1; k < argCount; ++k)
            {
                const uint32_t states = rd32(rdram, args + k * 4u);
                const uint32_t assoc = rd32(rdram, states + 0x18u);
                const uint32_t anim = assoc ? rd32(rdram, assoc + 0x20u) : 0u;
                if (!anim)
                    return false;
                const int32_t own = std::min<int32_t>(static_cast<int32_t>(rd32(rdram, anim + 0x18u)), static_cast<int32_t>(count));
                lo[k] = states;
                readHi[k] = states + kStateSize * count;
                writeHi[k] = states + kStateSize * static_cast<uint32_t>(std::max(own, 0));
            }
            for (uint32_t k = 1; k < argCount; ++k)
                for (uint32_t j = 1; j < argCount; ++j)
                    if (j != k && lo[k] < writeHi[j] && lo[j] < readHi[k])
                        return false;
            return true;
        }

        // the loop with the copies: in slices on the pool (Parallel), or in one piece here
        unsigned runCopies(uint8_t *rdram, R5900Context *ctx, vcs_anim_worker::Fn fn, uint32_t callback, bool parallel)
        {
            const uint32_t ad = GPR_U32(ctx, 4), args = GPR_U32(ctx, 6);
            const uint32_t count = rd32(rdram, ad + 8u);
            if (static_cast<int32_t>(count) <= 0)
                return 0;
            uint32_t argCount = 1;
            while (argCount < kMaxArgs && rd32(rdram, args + argCount * 4u))
                ++argCount;
            const bool advance = argCount > 1;
            // a second root-motion node: in one piece (both write the clump's delta, the last one's stays)
            const uint32_t nodes = rd32(rdram, ad + 0x10u);
            unsigned roots = 0;
            if (rd32(rdram, ad + 0xCu))
                for (uint32_t i = 0; i < count; ++i)
                    roots += (rdram[(nodes + i * kNodeSize) & PS2_RAM_MASK] & 8u) != 0;
            alignas(16) R5900Context base = *ctx;
            base.pc = callback;
            unsigned slices = 1;
            static const bool guard = !(std::getenv("VCS_ANIM_GUARD") && std::getenv("VCS_ANIM_GUARD")[0] == '0');
            // (the check only where the loop would be sliced at all)
            const unsigned want = parallel && g_pool ? std::min<unsigned>(g_pool->workers() + 1u, count / 6u) : 1u;
            if (want > 1 && roots <= 1 && (!guard || statesApart(rdram, ad, callback, args, argCount, count)))
                slices = want;
            else if (want > 1)
                g_together.fetch_add(1, std::memory_order_relaxed);
            slices = std::max(1u, std::min(slices, kMaxSlots));
            Slice s[kMaxSlots];
            const uint32_t csr = _mm_getcsr();
            for (unsigned i = 0; i < slices; ++i)
            {
                s[i].csr = csr;
                s[i].fn = fn;
                s[i].ad = ad;
                s[i].first = count * i / slices;
                s[i].end = count * (i + 1) / slices;
                s[i].args = args;
                s[i].argCount = argCount;
                s[i].slot = i;
                s[i].base = &base;
                s[i].advance = advance;
            }
            if (slices > 1)
            {
                g_pool->run(rdram, s, slices);
                g_parallel.fetch_add(1, std::memory_order_relaxed);
            }
            else
                runSlice(rdram, s[0]);
            // the args as the game's loop leaves them: every pointer count nodes on
            for (uint32_t k = 1; k < argCount; ++k)
                wr32(rdram, args + k * 4u, rd32(rdram, args + k * 4u) + (advance ? kStateSize * count : 0u));
            g_nodes.fetch_add(count, std::memory_order_relaxed);
            return slices; // how many slices it ran in
        }

        // verify only: the last loop compared before the next clump update begins, then the update as it is
        PS2_GUEST_CC void clumpUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kClumpUpdate)
                compare(rdram);
            g_origUpdate(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void nodeLoop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            // the workers' guest area must be unused: looked at once, at the first loop (the game running; at install the
            // RAM is still all zero)
            static bool areaChecked = false;
            if (!areaChecked && ctx->pc == kNodeLoop)
            {
                areaChecked = true;
                for (uint32_t a = kGuestArea; a < kGuestArea + kMaxSlots * kGuestSlot && g_mode != Mode::Off; a += 4)
                    if (rd32(rdram, a))
                    {
                        std::fprintf(stderr, "[anim] guest 0x%x is in use: the game's node loop\n", a);
                        g_mode = Mode::Off;
                    }
            }
            if (ctx->pc != kNodeLoop || g_mode == Mode::Off)
            {
                g_origLoop(rdram, ctx, runtime);
                return;
            }
            if (g_mode == Mode::Verify)
                compare(rdram);
            const uint32_t callback = GPR_U32(ctx, 5);
            const vcs_anim_worker::Fn fn =
                callback == kQuatCallback || callback == kMatrixCallback ? vcsAnimWorkerCallback(callback) : nullptr;
            if (!fn)
            {
                g_fallback.fetch_add(1, std::memory_order_relaxed);
                g_origLoop(rdram, ctx, runtime);
                return;
            }
            g_loops.fetch_add(1, std::memory_order_relaxed);
            if (g_mode == Mode::Verify)
            {
                // the copies' result kept, everything put back, the game's loop runs; compared at the next loop
                uint32_t argCount = 1;
                while (argCount < kMaxArgs && rd32(rdram, GPR_U32(ctx, 6) + argCount * 4u))
                    ++argCount;
                std::vector<Region> regions = regionsOf(rdram, GPR_U32(ctx, 4), GPR_U32(ctx, 6), argCount);
                bound(regions);
                const std::vector<Region> argsRegion{{GPR_U32(ctx, 6), (argCount + 1u) * 4u}};
                std::vector<uint8_t> before, argsBefore;
                save(rdram, regions, before);
                save(rdram, argsRegion, argsBefore);
                g_pending.slices = runCopies(rdram, ctx, fn, callback, g_pool != nullptr);
                g_pending.nodes = rd32(rdram, GPR_U32(ctx, 4) + 8u);
                save(rdram, regions, g_pending.native);
                restore(rdram, regions, before);
                restore(rdram, argsRegion, argsBefore); // (not compared, but the game's loop starts from them)
                g_pending.regions = std::move(regions);
                g_pending.callback = callback;
                g_pending.frame = rd32(rdram, kFrameCounter);
                g_pending.strict = false;
                g_pending.open = true;
                g_origLoop(rdram, ctx, runtime);
                return;
            }
            if (g_mode == Mode::Slices)
            {
                // the copies in one piece and in slices on the same input, compared at once (no game code in between):
                // the slicing's own check; the slices' result stays
                uint32_t argCount = 1;
                while (argCount < kMaxArgs && rd32(rdram, GPR_U32(ctx, 6) + argCount * 4u))
                    ++argCount;
                std::vector<Region> regions = regionsOf(rdram, GPR_U32(ctx, 4), GPR_U32(ctx, 6), argCount);
                regions.push_back({GPR_U32(ctx, 6), (argCount + 1u) * 4u});
                bound(regions);
                std::vector<uint8_t> before, serial, sliced;
                save(rdram, regions, before);
                runCopies(rdram, ctx, fn, callback, false);
                save(rdram, regions, serial);
                restore(rdram, regions, before);
                g_pending.slices = runCopies(rdram, ctx, fn, callback, true);
                save(rdram, regions, sliced);
                if (g_pending.slices > 1)
                {
                    if (serial == sliced)
                        g_verified.fetch_add(1, std::memory_order_relaxed);
                    else
                    {
                        g_pending.strict = true;
                        g_pending.regions = regions;
                        g_pending.native = serial;
                        g_pending.callback = callback;
                        g_pending.nodes = rd32(rdram, GPR_U32(ctx, 4) + 8u);
                        g_pending.frame = rd32(rdram, kFrameCounter);
                        g_pending.open = true;
                        compare(rdram); // (counts it, prints the first ones: "game" = in slices, "copy" = in one piece)
                    }
                }
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
            runCopies(rdram, ctx, fn, callback, g_mode == Mode::Parallel);
            ctx->pc = GPR_U32(ctx, 31); // replaced entirely (the hook rule)
        }
#endif
    }

    void install(PS2Runtime &runtime)
    {
#if defined(VCS_ANIM_WORKER)
        g_runtime = &runtime;
        const char *e = std::getenv("VCS_NATIVE_ANIM");
        const std::string mode = e ? e : "";
        g_mode = mode == "0" ? Mode::Off : mode == "serial" ? Mode::Serial : mode == "verify" ? Mode::Verify : mode == "slices" ? Mode::Slices : Mode::Parallel;
        if (g_mode == Mode::Off)
        {
            std::fprintf(stderr, "[anim] the game's node loop (VCS_NATIVE_ANIM=0)\n");
            return;
        }
        g_origLoop = runtime.lookupFunction(kNodeLoop);
        if (!g_origLoop)
            return;
        runtime.replaceFunction(kNodeLoop, &nodeLoop);
        if (g_mode == Mode::Verify && (g_origUpdate = runtime.lookupFunction(kClumpUpdate)))
            runtime.replaceFunction(kClumpUpdate, &clumpUpdate);
        unsigned workers = 0;
        if (g_mode != Mode::Serial)
        {
            // half the hardware threads less the game's own busy ones (game, render worker, GS stage, main), 1 to 3: a ped's
            // ~25 nodes make 4 slices at most (6 a slice), and spinning workers take cores from the render worker
            const int cores = static_cast<int>(std::thread::hardware_concurrency());
            workers = static_cast<unsigned>(std::clamp(cores / 2 - 2, 1, 3));
            if (const char *t = std::getenv("VCS_ANIM_THREADS"))
                workers = static_cast<unsigned>(std::clamp(std::atoi(t), 0, static_cast<int>(kMaxSlots) - 1));
            if (workers)
                g_pool = std::make_unique<Pool>(workers);
        }
        std::fprintf(stderr, "[anim] node loop: the game's code copied, %s, %u worker(s)\n",
                     g_mode == Mode::Verify ? "verified against the game" : g_mode == Mode::Serial ? "on the game thread" : "in slices",
                     workers);
        // animstate -> ok loops=<native loops> parallel=<in slices> nodes=<n> fallback=<game's loops> verified=<n>
        //              mismatches=<n> freed=<verified loops whose memory the game cleared before the compare>
        ps2control::addCommand("animstate", [](const std::string &, ps2control::Reply reply) {
            char text[200];
            std::snprintf(text, sizeof(text), "ok loops=%llu parallel=%llu nodes=%llu fallback=%llu verified=%llu mismatches=%llu freed=%llu late=%llu together=%llu",
                          (unsigned long long)g_loops.load(), (unsigned long long)g_parallel.load(),
                          (unsigned long long)g_nodes.load(), (unsigned long long)g_fallback.load(),
                          (unsigned long long)g_verified.load(), (unsigned long long)g_mismatches.load(),
                          (unsigned long long)g_freed.load(), (unsigned long long)g_late.load(),
                          (unsigned long long)g_together.load());
            reply(text);
        });
#else
        (void)runtime;
        std::fprintf(stderr, "[anim] no worker copies built (tools/anim_worker_gen.py): the game's node loop\n");
#endif
    }
}
