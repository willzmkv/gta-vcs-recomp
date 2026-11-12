// [gta-recomp] Host audio output for the SPU2 mix: a ring buffer filled in IOP time (PS2IopHostAdapter::audioSamples)
// and drained by the platform layer's audio callback (SDL3 audio stream) at 48 kHz stereo.
// The emulator produces audio in bursts (IOP time is caught up with the host clock once per IOP batch), so the
// buffer keeps a target fill that adapts: it starts at 60 ms, grows by 30 ms after every underrun (max 250 ms) and
// shrinks back by ~1 ms per second. Clock drift is absorbed by resampling within +-0.5% (inaudible), never by the
// large pitch swings an earlier version used. After an underrun playback waits for the target fill before
// resuming (one short gap instead of a crackle). PS2X_AUDIO_STATS=1 prints underruns / fill every 5 s.
#include "runtime/ps2_audio_out.h"
#include "runtime/ps2_control.h"

#include "platform/platform.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>
#include "runtime/ps2_host_std.h"

namespace
{
    constexpr uint32_t kRate = 48000u;
    constexpr size_t kCapacityFrames = kRate;             // 1 s ring
    constexpr double kMinTargetFrames = kRate * 0.06;     // 60 ms
    constexpr double kMaxTargetFrames = kRate * 0.25;     // 250 ms
    constexpr double kMaxRateOffset = 0.005;              // +-0.5% resampling for drift
    constexpr size_t kMaxQueuedFrames = kRate * 40 / 100; // hard latency cap (400 ms)

    std::mutex g_mutex;
    std::vector<int16_t> g_ring(kCapacityFrames * 2u);
    size_t g_read = 0, g_write = 0, g_queued = 0;
    double g_frac = 0.0; // fractional position between g_read and g_read + 1
    double g_rate = 1.0; // smoothed playback rate
    double g_target = kMinTargetFrames; // adaptive target fill
    bool g_primed = false;
    int16_t g_last[2] = {0, 0};
    bool g_started = false;
    FILE *g_dump = nullptr; // PS2X_AUDIO_DUMP=<file.wav>: everything the SPU2 produced
    uint32_t g_dumpFrames = 0;
    uint64_t g_underrunFrames = 0, g_underruns = 0, g_droppedFrames = 0, g_playedFrames = 0, g_pushedFrames = 0;
    double g_minRate = 2.0, g_maxRate = 0.0;
    std::atomic<uint64_t> g_underrunsTotal{0};
    std::atomic<float> g_queuedMs{0.0f};

    // [gta-recomp] movie sound (g_mutex): mixed 1:1 after the SPU2 output
    std::vector<int16_t> g_movieRing(kCapacityFrames * 2u);
    size_t g_movieRead = 0, g_movieWrite = 0, g_movieQueued = 0;
    std::atomic<uint64_t> g_moviePlayed{0};

    // [gta-recomp] player settings (ps2AudioSetVolume / SetOutputMode / SetMuted): gains in 1/65536
    std::atomic<int> g_masterGain{65536}, g_movieGain{65536};
    std::atomic<int> g_outputMode{0};
    std::atomic<bool> g_muted{false};

    void writeWavHeader(FILE *f, uint32_t frames)
    {
        const uint32_t dataBytes = frames * 4u;
        const uint32_t riffSize = 36u + dataBytes;
        const uint16_t fmt[] = {1, 2};
        const uint32_t rate = kRate, byteRate = kRate * 4u;
        const uint16_t align = 4, bits = 16;
        const uint32_t fmtSize = 16;
        std::fseek(f, 0, SEEK_SET);
        std::fwrite("RIFF", 1, 4, f);
        std::fwrite(&riffSize, 4, 1, f);
        std::fwrite("WAVEfmt ", 1, 8, f);
        std::fwrite(&fmtSize, 4, 1, f);
        std::fwrite(fmt, 2, 2, f);
        std::fwrite(&rate, 4, 1, f);
        std::fwrite(&byteRate, 4, 1, f);
        std::fwrite(&align, 2, 1, f);
        std::fwrite(&bits, 2, 1, f);
        std::fwrite("data", 1, 4, f);
        std::fwrite(&dataBytes, 4, 1, f);
        std::fseek(f, 0, SEEK_END);
    }

    int16_t at(size_t frame, int channel) { return g_ring[(frame % kCapacityFrames) * 2u + channel]; }

    void callback(int16_t *out, uint32_t frames)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_primed && g_queued >= g_target)
            g_primed = true;
        g_target = std::max(kMinTargetFrames, g_target - frames / 1000.0);
        // steer the rate toward the target fill, smoothed so the pitch never jumps
        const double wanted = 1.0 + std::clamp((static_cast<double>(g_queued) - g_target) / (kRate * 4.0), -kMaxRateOffset, kMaxRateOffset);
        g_rate += (wanted - g_rate) * 0.1;
        g_queuedMs.store(g_queued * 1000.0f / kRate, std::memory_order_relaxed);
        g_minRate = std::min(g_minRate, g_rate);
        g_maxRate = std::max(g_maxRate, g_rate);
        for (unsigned int i = 0; i < frames; ++i)
        {
            if (!g_primed || g_queued < 2u)
            {
                if (g_primed)
                {
                    g_primed = false;
                    ++g_underruns;
                    g_underrunsTotal.fetch_add(1, std::memory_order_relaxed);
                    g_target = std::min(kMaxTargetFrames, g_target + kRate * 0.03);
                }
                // hold the last sample and fade it out instead of clicking to zero
                for (int c = 0; c < 2; ++c)
                {
                    g_last[c] = static_cast<int16_t>(g_last[c] * 31 / 32);
                    out[i * 2u + c] = g_last[c];
                }
                ++g_underrunFrames;
                continue;
            }
            for (int c = 0; c < 2; ++c)
            {
                const double a = at(g_read, c), b = at(g_read + 1u, c);
                g_last[c] = static_cast<int16_t>(a + (b - a) * g_frac);
                out[i * 2u + c] = g_last[c];
            }
            g_frac += g_rate;
            while (g_frac >= 1.0 && g_queued >= 2u)
            {
                g_frac -= 1.0;
                g_read = (g_read + 1u) % kCapacityFrames;
                --g_queued;
            }
            ++g_playedFrames;
        }
        // [gta-recomp] movie sound on top
        const int movieGain = g_movieGain.load(std::memory_order_relaxed);
        uint32_t mixed = 0;
        for (; mixed < frames && g_movieQueued; ++mixed)
        {
            for (int c = 0; c < 2; ++c)
            {
                const int v = out[mixed * 2u + c] +
                              static_cast<int>((static_cast<int64_t>(g_movieRing[g_movieRead * 2u + c]) * movieGain) >> 16);
                out[mixed * 2u + c] = static_cast<int16_t>(std::clamp(v, -32768, 32767));
            }
            g_movieRead = (g_movieRead + 1u) % kCapacityFrames;
            --g_movieQueued;
        }
        g_moviePlayed.fetch_add(mixed, std::memory_order_relaxed);

        // [gta-recomp] the player's volume and output mode, last (the state above runs on unchanged when muted)
        const int gain = g_muted.load(std::memory_order_relaxed) ? 0 : g_masterGain.load(std::memory_order_relaxed);
        const int mode = g_outputMode.load(std::memory_order_relaxed);
        if (gain == 65536 && mode == 0)
            return;
        for (uint32_t i = 0; i < frames; ++i)
        {
            int l = out[i * 2u], r = out[i * 2u + 1u];
            if (mode == 1)
                l = r = (l + r) / 2;
            else if (mode == 2)
                std::swap(l, r);
            out[i * 2u] = static_cast<int16_t>((static_cast<int64_t>(l) * gain) >> 16);
            out[i * 2u + 1u] = static_cast<int16_t>((static_cast<int64_t>(r) * gain) >> 16);
        }
    }

    void reportStats()
    {
        static const bool stats = std::getenv("PS2X_AUDIO_STATS") != nullptr;
        static auto last = ps2x::hostNow();
        if (!stats)
            return;
        const auto now = ps2x::hostNow();
        if (now - last < std::chrono::seconds(5))
            return;
        const double secs = std::chrono::duration<double>(now - last).count();
        last = now;
        std::fprintf(stderr, "[audio] produced %.0f%% of real time; played %llu, underruns %llu (%llu frames), dropped %llu; queued %.0f / target %.0f ms, rate %.4f (%.4f..%.4f)\n",
                     g_pushedFrames * 100.0 / (secs * kRate), static_cast<unsigned long long>(g_playedFrames),
                     static_cast<unsigned long long>(g_underruns), static_cast<unsigned long long>(g_underrunFrames),
                     static_cast<unsigned long long>(g_droppedFrames), g_queued * 1000.0 / kRate, g_target * 1000.0 / kRate, g_rate, g_minRate, g_maxRate);
        g_minRate = 2.0;
        g_maxRate = 0.0;
        g_playedFrames = g_underruns = g_underrunFrames = g_droppedFrames = g_pushedFrames = 0;
    }
}

void ps2AudioSetVolume(float master, float movies)
{
    auto gain = [](float v) { return static_cast<int>(std::clamp(v, 0.0f, 1.0f) * 65536.0f + 0.5f); };
    g_masterGain.store(gain(master), std::memory_order_relaxed);
    g_movieGain.store(gain(movies), std::memory_order_relaxed);
}

void ps2AudioSetOutputMode(int mode) { g_outputMode.store(std::clamp(mode, 0, 2), std::memory_order_relaxed); }

void ps2AudioSetMuted(bool muted) { g_muted.store(muted, std::memory_order_relaxed); }

void ps2AudioInit()
{
    const char *setting = std::getenv("PS2X_AUDIO");
    const bool muted = setting ? setting[0] == '0' : ps2control::hiddenMode(); // hidden test runs are silent by default
    if (g_started || muted || !platform::audioAvailable())
        return;
    g_started = platform::audioOpen(kRate, 1024, &callback);
}

void ps2AudioPush(const int16_t *stereo, uint32_t frames)
{
    static const char *dumpPath = std::getenv("PS2X_AUDIO_DUMP");
    if (dumpPath && !g_dump && (g_dump = std::fopen(dumpPath, "wb")) != nullptr)
        writeWavHeader(g_dump, 0);
    if (g_dump)
    {
        std::fwrite(stereo, 4, frames, g_dump);
        g_dumpFrames += frames;
        if ((g_dumpFrames % (kRate * 2u)) < frames) // keep the header valid every ~2 s
            writeWavHeader(g_dump, g_dumpFrames);
        std::fflush(g_dump);
    }
    if (!g_started)
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pushedFrames += frames;
    for (uint32_t i = 0; i < frames; ++i)
    {
        g_ring[g_write * 2u] = stereo[i * 2u];
        g_ring[g_write * 2u + 1u] = stereo[i * 2u + 1u];
        g_write = (g_write + 1u) % kCapacityFrames;
        if (g_queued < kCapacityFrames)
            ++g_queued;
        else
            g_read = (g_read + 1u) % kCapacityFrames;
    }
    while (g_queued > kMaxQueuedFrames)
    {
        g_read = (g_read + 1u) % kCapacityFrames;
        --g_queued;
        ++g_droppedFrames;
    }
    reportStats();
}

void ps2AudioShutdown()
{
    if (!g_started)
        return;
    platform::audioClose();
    g_started = false;
}

float ps2AudioQueuedMs() { return g_queuedMs.load(std::memory_order_relaxed); }
uint64_t ps2AudioUnderrunCount() { return g_underrunsTotal.load(std::memory_order_relaxed); }

bool ps2AudioMovieActive() { return g_started; }

uint32_t ps2AudioMoviePush(const int16_t *stereo, uint32_t frames)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    uint32_t taken = 0;
    for (; taken < frames && g_movieQueued < kCapacityFrames; ++taken)
    {
        g_movieRing[g_movieWrite * 2u] = stereo[taken * 2u];
        g_movieRing[g_movieWrite * 2u + 1u] = stereo[taken * 2u + 1u];
        g_movieWrite = (g_movieWrite + 1u) % kCapacityFrames;
        ++g_movieQueued;
    }
    return taken;
}

uint64_t ps2AudioMoviePlayed() { return g_moviePlayed.load(std::memory_order_relaxed); }

uint32_t ps2AudioMovieQueued()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return static_cast<uint32_t>(g_movieQueued);
}

void ps2AudioMovieReset()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_movieRead = g_movieWrite = g_movieQueued = 0;
    g_moviePlayed.store(0, std::memory_order_relaxed);
}
