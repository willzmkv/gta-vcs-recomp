// [gta-recomp] Lockstep SPSC upload & native-mesh byte cache between RenderWorker and GsStage.
//
// VCS re-submits immutable Op::Direct raster/CLUT packets (~700 MB/s) and static native mesh vertices (~1 GB/s) every frame.
// Because RenderWorker -> GsStage is a single FIFO queue, both ends execute an identical 4-way FIFO replacement policy
// per set over the stream: once a (key, size) entry has been sent once, RenderWorker sends a compact command token with
// zero payload memcpy, and GsStage resolves the cached bytes in place (also pinning GIF slots inside PendingUpload so
// lazy uploads skip XXH3 hashing and PendingUpload::data copies until evicted).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace ps2_upload_cache
{
    static constexpr uint32_t kGifSets = 2048u;
    static constexpr uint32_t kGifWays = 4u;
    static constexpr uint32_t kGifSlots = kGifSets * kGifWays;

    static constexpr uint32_t kNativeSets = 4096u;
    static constexpr uint32_t kNativeWays = 4u;
    static constexpr uint32_t kNativeSlots = kNativeSets * kNativeWays;

    static constexpr uint32_t kMaxCachedBytes = 1024u * 1024u;

    static constexpr uint32_t kCmdGifCachedPacket = 5u;
    static constexpr uint32_t kCmdGifStorePacket = 6u;
    static constexpr uint32_t kCmdGifCachedImage = 7u;
    static constexpr uint32_t kCmdGifStoreImage = 8u;
    static constexpr uint32_t kCmdNativeCached = 9u;
    static constexpr uint32_t kCmdNativeStore = 10u;

    inline uint64_t mix64(uint64_t x, uint64_t y)
    {
        uint64_t z = x ^ (y * 0x9E3779B97F4A7C15ull + 0x517CC1B727220A95ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    inline bool gifCacheEnabled()
    {
        static const bool on = [] {
            const char *e = std::getenv("PS2X_HW_UPLOAD_CACHE");
            return !(e && e[0] == '0');
        }();
        return on;
    }

    inline bool nativeCacheEnabled()
    {
        static const bool on = [] {
            const char *e = std::getenv("PS2X_HW_NATIVE_CACHE");
            return !(e && e[0] == '0');
        }();
        return on;
    }

    struct SenderCache
    {
        struct Meta
        {
            uint64_t key = 0;
            uint32_t size = 0;
            bool valid = false;
        };

        std::vector<Meta> gifMeta = std::vector<Meta>(kGifSlots);
        std::vector<uint8_t> gifRr = std::vector<uint8_t>(kGifSets, 0u);
        std::vector<Meta> nativeMeta = std::vector<Meta>(kNativeSlots);
        std::vector<uint8_t> nativeRr = std::vector<uint8_t>(kNativeSets, 0u);

        uint64_t statImgHit = 0, statImgMiss = 0;
        uint64_t statPktHit = 0, statPktMiss = 0;
        uint64_t statNatHit = 0, statNatMiss = 0, statNatSkip = 0;
        uint64_t statGifFallback = 0, statImgFallback = 0;

        bool findGif(uint64_t key, uint32_t size, uint32_t &outSlot) const
        {
            const uint32_t set = static_cast<uint32_t>(mix64(key, size) & (kGifSets - 1u));
            const uint32_t base = set * kGifWays;
            for (uint32_t w = 0; w < kGifWays; ++w)
            {
                const Meta &m = gifMeta[base + w];
                if (m.valid && m.key == key && m.size == size)
                {
                    outSlot = base + w;
                    return true;
                }
            }
            return false;
        }

        uint32_t insertGif(uint64_t key, uint32_t size)
        {
            const uint32_t set = static_cast<uint32_t>(mix64(key, size) & (kGifSets - 1u));
            const uint32_t w = gifRr[set];
            gifRr[set] = static_cast<uint8_t>((w + 1u) & (kGifWays - 1u));
            const uint32_t slot = set * kGifWays + w;
            gifMeta[slot] = {key, size, true};
            return slot;
        }

        bool findNative(uint64_t key, uint32_t size, uint32_t &outSlot) const
        {
            const uint32_t set = static_cast<uint32_t>(mix64(key, size) & (kNativeSets - 1u));
            const uint32_t base = set * kNativeWays;
            for (uint32_t w = 0; w < kNativeWays; ++w)
            {
                const Meta &m = nativeMeta[base + w];
                if (m.valid && m.key == key && m.size == size)
                {
                    outSlot = base + w;
                    return true;
                }
            }
            return false;
        }

        uint32_t insertNative(uint64_t key, uint32_t size)
        {
            const uint32_t set = static_cast<uint32_t>(mix64(key, size) & (kNativeSets - 1u));
            const uint32_t w = nativeRr[set];
            nativeRr[set] = static_cast<uint8_t>((w + 1u) & (kNativeWays - 1u));
            const uint32_t slot = set * kNativeWays + w;
            nativeMeta[slot] = {key, size, true};
            return slot;
        }
    };

    struct GifSlot
    {
        uint64_t key = 0;
        uint32_t size = 0;
        std::vector<uint8_t> bytes;
        uint64_t uploadHash = 0;
        uint32_t uploadOff = 0;
        uint32_t uploadSize = 0;
        bool valid = false;
        bool hasUploadHash = false;
    };

    struct NativeSlot
    {
        uint64_t key = 0;
        uint32_t size = 0;
        std::vector<uint8_t> bytes;
        bool valid = false;
    };

    struct ReceiverCache
    {
        using EvictGifFn = void (*)(void *ctx, uint32_t slotIdx, const uint8_t *oldBytes, uint32_t oldSize);
        using EvictNativeFn = void (*)(void *ctx, uint32_t slotIdx);

        std::vector<GifSlot> gifSlots = std::vector<GifSlot>(kGifSlots);
        std::vector<uint8_t> gifRr = std::vector<uint8_t>(kGifSets, 0u);
        std::vector<NativeSlot> nativeSlots = std::vector<NativeSlot>(kNativeSlots);
        std::vector<uint8_t> nativeRr = std::vector<uint8_t>(kNativeSets, 0u);
        EvictGifFn onEvictGif = nullptr;
        EvictNativeFn onEvictNative = nullptr;
        void *evictCtx = nullptr;

        uint32_t findGif(uint64_t key, uint32_t size) const
        {
            const uint32_t set = static_cast<uint32_t>(mix64(key, size) & (kGifSets - 1u));
            const uint32_t base = set * kGifWays;
            for (uint32_t w = 0; w < kGifWays; ++w)
            {
                const GifSlot &s = gifSlots[base + w];
                if (s.valid && s.key == key && s.size == size)
                    return base + w;
            }
            return kGifSlots;
        }

        uint32_t insertGif(uint64_t key, uint32_t size, const uint8_t *data, bool hasUploadHash, uint64_t uploadHash)
        {
            const uint32_t set = static_cast<uint32_t>(mix64(key, size) & (kGifSets - 1u));
            const uint32_t w = gifRr[set];
            gifRr[set] = static_cast<uint8_t>((w + 1u) & (kGifWays - 1u));
            const uint32_t idx = set * kGifWays + w;
            GifSlot &s = gifSlots[idx];
            if (s.valid && onEvictGif)
                onEvictGif(evictCtx, idx, s.bytes.data(), s.size);
            s.key = key;
            s.size = size;
            s.bytes.assign(data, data + size);
            s.valid = true;
            s.hasUploadHash = hasUploadHash;
            s.uploadHash = uploadHash;
            s.uploadOff = 0u;
            s.uploadSize = hasUploadHash ? size : 0u;
            return idx;
        }

        uint32_t findNative(uint64_t key, uint32_t size) const
        {
            const uint32_t set = static_cast<uint32_t>(mix64(key, size) & (kNativeSets - 1u));
            const uint32_t base = set * kNativeWays;
            for (uint32_t w = 0; w < kNativeWays; ++w)
            {
                const NativeSlot &s = nativeSlots[base + w];
                if (s.valid && s.key == key && s.size == size)
                    return base + w;
            }
            return kNativeSlots;
        }

        uint32_t insertNative(uint64_t key, uint32_t size, const uint8_t *data)
        {
            const uint32_t set = static_cast<uint32_t>(mix64(key, size) & (kNativeSets - 1u));
            const uint32_t w = nativeRr[set];
            nativeRr[set] = static_cast<uint8_t>((w + 1u) & (kNativeWays - 1u));
            const uint32_t idx = set * kNativeWays + w;
            NativeSlot &s = nativeSlots[idx];
            if (s.valid && onEvictNative)
                onEvictNative(evictCtx, idx);
            s.key = key;
            s.size = size;
            s.bytes.assign(data, data + size);
            s.valid = true;
            return idx;
        }
    };

    inline SenderCache &senderCache()
    {
        static SenderCache cache;
        return cache;
    }

    inline ReceiverCache &receiverCache()
    {
        static ReceiverCache cache;
        return cache;
    }

    // Worker-side precomputed hash hint passed from vcs_rsl_world -> submitVif1DirectData without altering headers.
    inline thread_local uint64_t t_directHintHash = 0;
    inline thread_local uint32_t t_directHintBytes = 0;

    inline void setDirectHint(uint64_t hash, uint32_t bytes)
    {
        t_directHintHash = hash;
        t_directHintBytes = bytes;
    }

    inline uint64_t takeDirectHint(uint32_t bytes)
    {
        const uint64_t h = (t_directHintBytes == bytes) ? t_directHintHash : 0ull;
        t_directHintHash = 0ull;
        t_directHintBytes = 0u;
        return h;
    }

    // GsStage-side active GIF/Native slot while executing a cached/stored command.
    inline thread_local uint32_t t_activeGifSlot = UINT32_MAX;
    inline thread_local uint32_t t_activeNativeSlot = UINT32_MAX;

    struct ActiveGifSlotScope
    {
        uint32_t prev;
        explicit ActiveGifSlotScope(uint32_t slotIdx) : prev(t_activeGifSlot) { t_activeGifSlot = slotIdx; }
        ~ActiveGifSlotScope() { t_activeGifSlot = prev; }
    };

    struct ActiveNativeSlotScope
    {
        uint32_t prev;
        explicit ActiveNativeSlotScope(uint32_t slotIdx) : prev(t_activeNativeSlot) { t_activeNativeSlot = slotIdx; }
        ~ActiveNativeSlotScope() { t_activeNativeSlot = prev; }
    };

    // Bridges declared in ps2_vif1_interpreter.cpp <-> gs_frontend.cpp (only compiled on those TUs).
} // namespace ps2_upload_cache

bool ps2GsSendVif1CachedImage(uint64_t imageTag, const uint8_t *data, uint32_t sizeBytes, uint64_t dataHash, bool directHl);
bool ps2GsSendDirectPacketCached(uint8_t path, const uint8_t *data, uint32_t sizeBytes, uint64_t packetHash, bool directHl);
