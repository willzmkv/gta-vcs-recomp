#include "sound.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

#ifdef _WIN32 // (waveOut; elsewhere the launcher is silent for now - see the end)
namespace sound
{
    namespace
    {
        constexpr int kSampleRate = 44100;
        constexpr int kChannels = 2;
        constexpr int kBufferSize = 2048; // ~46.4ms buffer
        constexpr int kMaxVoices = 8;

        struct PcmClip
        {
            std::vector<float> samples; // Interleaved stereo L, R float [-1.0f, +1.0f]
        };

        PcmClip g_clips[3]; // Nav = 0, Back = 1, Play = 2

        struct Voice
        {
            Sfx sfx = Sfx::Nav;
            size_t cursor = 0;
            float volume = 1.0f;
            float attackGain = 0.0f;    // ramps 0.0 -> 1.0 over ~2ms to eliminate DC click
            float attackStep = 0.0f;
            float fadeOutSpeed = 0.0f;  // > 0 means fading out over ~30ms
            bool active = false;
        };

        Voice g_voices[kMaxVoices];
        std::mutex g_audioMutex;
        std::atomic<bool> g_running{false};
        std::thread g_audioThread;
        HWAVEOUT g_hWaveOut = NULL;

        // Debounce / rate limiter to prevent crackling if triggered in the same millisecond
        std::chrono::steady_clock::time_point g_lastPlayTime[3]{};

        // Decodes a standard 16-bit PCM WAV and resamples with linear interpolation to 44100 Hz stereo
        bool loadAndResampleWav(const fs::path &path, PcmClip &outClip)
        {
            std::ifstream in(path, std::ios::binary | std::ios::ate);
            if (!in)
                return false;
            const auto sz = in.tellg();
            if (sz < 44)
                return false;
            std::vector<uint8_t> raw(static_cast<size_t>(sz));
            in.seekg(0, std::ios::beg);
            in.read(reinterpret_cast<char *>(raw.data()), sz);

            if (std::memcmp(raw.data(), "RIFF", 4) != 0 || std::memcmp(raw.data() + 8, "WAVE", 4) != 0)
                return false;

            size_t idx = 12;
            int channels = 1;
            int srcRate = 44100;
            int bits = 16;
            const uint8_t *pcmData = nullptr;
            size_t pcmBytes = 0;

            while (idx + 8 <= raw.size())
            {
                const char *chunkId = reinterpret_cast<const char *>(raw.data() + idx);
                uint32_t chunkSz = *reinterpret_cast<const uint32_t *>(raw.data() + idx + 4);
                idx += 8;

                if (std::memcmp(chunkId, "fmt ", 4) == 0 && idx + 16 <= raw.size())
                {
                    uint16_t audioFmt = *reinterpret_cast<const uint16_t *>(raw.data() + idx);
                    if (audioFmt != 1) // PCM
                        return false;
                    channels = *reinterpret_cast<const uint16_t *>(raw.data() + idx + 2);
                    srcRate = *reinterpret_cast<const uint32_t *>(raw.data() + idx + 4);
                    bits = *reinterpret_cast<const uint16_t *>(raw.data() + idx + 14);
                }
                else if (std::memcmp(chunkId, "data", 4) == 0)
                {
                    pcmData = raw.data() + idx;
                    pcmBytes = std::min<size_t>(chunkSz, raw.size() - idx);
                    break;
                }
                idx += chunkSz;
                if (chunkSz % 2 == 1)
                    idx += 1;
            }

            if (!pcmData || pcmBytes < sizeof(int16_t) || bits != 16 || channels < 1 || srcRate <= 0)
                return false;

            const size_t totalSrcFrames = pcmBytes / (channels * sizeof(int16_t));
            const int16_t *srcSamples = reinterpret_cast<const int16_t *>(pcmData);

            // Resample to 44100 Hz stereo float
            const double ratio = static_cast<double>(srcRate) / static_cast<double>(kSampleRate);
            const size_t totalDstFrames = static_cast<size_t>(std::ceil(totalSrcFrames / ratio));

            outClip.samples.resize(totalDstFrames * kChannels);

            for (size_t df = 0; df < totalDstFrames; ++df)
            {
                const double srcPos = df * ratio;
                const size_t sf0 = std::min<size_t>(static_cast<size_t>(srcPos), totalSrcFrames - 1);
                const size_t sf1 = std::min<size_t>(sf0 + 1, totalSrcFrames - 1);
                const float frac = static_cast<float>(srcPos - sf0);

                float sL = 0.0f, sR = 0.0f;
                if (channels == 1)
                {
                    const float v0 = srcSamples[sf0] / 32768.0f;
                    const float v1 = srcSamples[sf1] / 32768.0f;
                    const float val = v0 + (v1 - v0) * frac;
                    sL = val;
                    sR = val;
                }
                else
                {
                    const float l0 = srcSamples[sf0 * 2 + 0] / 32768.0f;
                    const float l1 = srcSamples[sf1 * 2 + 0] / 32768.0f;
                    const float r0 = srcSamples[sf0 * 2 + 1] / 32768.0f;
                    const float r1 = srcSamples[sf1 * 2 + 1] / 32768.0f;
                    sL = l0 + (l1 - l0) * frac;
                    sR = r0 + (r1 - r0) * frac;
                }

                outClip.samples[df * 2 + 0] = sL;
                outClip.samples[df * 2 + 1] = sR;
            }

            return true;
        }

#ifdef _WIN32
        void audioWorker()
        {
            WAVEHDR headers[2]{};
            std::vector<int16_t> buffers[2];
            buffers[0].resize(kBufferSize * kChannels);
            buffers[1].resize(kBufferSize * kChannels);

            HANDLE hBufferEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

            WAVEFORMATEX wfx{};
            wfx.wFormatTag = WAVE_FORMAT_PCM;
            wfx.nChannels = kChannels;
            wfx.nSamplesPerSec = kSampleRate;
            wfx.nAvgBytesPerSec = kSampleRate * kChannels * sizeof(int16_t);
            wfx.nBlockAlign = kChannels * sizeof(int16_t);
            wfx.wBitsPerSample = 16;

            if (waveOutOpen(&g_hWaveOut, WAVE_MAPPER, &wfx, reinterpret_cast<DWORD_PTR>(hBufferEvent), 0, CALLBACK_EVENT) != MMSYSERR_NOERROR)
            {
                CloseHandle(hBufferEvent);
                return;
            }

            for (int i = 0; i < 2; ++i)
            {
                std::memset(&headers[i], 0, sizeof(WAVEHDR));
                headers[i].lpData = reinterpret_cast<LPSTR>(buffers[i].data());
                headers[i].dwBufferLength = static_cast<DWORD>(buffers[i].size() * sizeof(int16_t));
                waveOutPrepareHeader(g_hWaveOut, &headers[i], sizeof(WAVEHDR));
                headers[i].dwFlags |= WHDR_DONE;
            }

            int bufIdx = 0;
            while (g_running.load(std::memory_order_relaxed))
            {
                WAVEHDR &hdr = headers[bufIdx];
                if (!(hdr.dwFlags & WHDR_DONE))
                {
                    WaitForSingleObject(hBufferEvent, 20);
                    continue;
                }

                std::vector<int16_t> &buf = buffers[bufIdx];

                {
                    std::lock_guard<std::mutex> lock(g_audioMutex);
                    for (int f = 0; f < kBufferSize; ++f)
                    {
                        float sumL = 0.0f, sumR = 0.0f;
                        for (auto &v : g_voices)
                        {
                            if (!v.active)
                                continue;

                            const auto &clip = g_clips[static_cast<int>(v.sfx)];
                            if (v.cursor + 1 >= clip.samples.size())
                            {
                                v.active = false;
                                continue;
                            }

                            // Anti-click attack envelope
                            if (v.attackGain < 1.0f)
                            {
                                v.attackGain = std::min(1.0f, v.attackGain + v.attackStep);
                            }

                            // Effective sample gain
                            const float gain = v.volume * v.attackGain;
                            sumL += clip.samples[v.cursor] * gain;
                            sumR += clip.samples[v.cursor + 1] * gain;
                            v.cursor += 2;

                            // Smooth fade-out decrement
                            if (v.fadeOutSpeed > 0.0f)
                            {
                                v.volume -= v.fadeOutSpeed;
                                if (v.volume <= 0.0f)
                                {
                                    v.volume = 0.0f;
                                    v.active = false;
                                }
                            }
                        }

                        // Soft saturation limiter to prevent digital clipping
                        sumL = std::clamp(sumL, -0.98f, 0.98f);
                        sumR = std::clamp(sumR, -0.98f, 0.98f);
                        buf[f * 2 + 0] = static_cast<int16_t>(sumL * 32767.0f);
                        buf[f * 2 + 1] = static_cast<int16_t>(sumR * 32767.0f);
                    }
                }

                hdr.dwFlags &= ~WHDR_DONE;
                waveOutWrite(g_hWaveOut, &hdr, sizeof(WAVEHDR));
                bufIdx = 1 - bufIdx;
            }

            waveOutReset(g_hWaveOut);
            for (int i = 0; i < 2; ++i)
                waveOutUnprepareHeader(g_hWaveOut, &headers[i], sizeof(WAVEHDR));
            waveOutClose(g_hWaveOut);
            CloseHandle(hBufferEvent);
        }
#endif
    }

    void init(const fs::path &assetsDir)
    {
        const fs::path sfxDir = assetsDir / "sfx";

        // 1. Load Nav SFX
        if (!loadAndResampleWav(sfxDir / "nav.wav", g_clips[0]))
            loadAndResampleWav(sfxDir / "navegando entre menus.wav", g_clips[0]);

        // 2. Load Back SFX
        if (!loadAndResampleWav(sfxDir / "back.wav", g_clips[1]))
            loadAndResampleWav(sfxDir / "voltar.wav", g_clips[1]);

        // 3. Load Play SFX
        if (!loadAndResampleWav(sfxDir / "play.wav", g_clips[2]))
            loadAndResampleWav(sfxDir / "clicando em jogar.wav", g_clips[2]);

#ifdef _WIN32
        if (!g_running)
        {
            g_running = true;
            g_audioThread = std::thread(audioWorker);
        }
#endif
    }

    void play(Sfx sfx)
    {
#ifdef _WIN32
        const int sfxIdx = static_cast<int>(sfx);
        if (sfxIdx < 0 || sfxIdx >= 3 || g_clips[sfxIdx].samples.empty())
            return;

        const auto now = std::chrono::steady_clock::now();

        // 1. Debounce check: If exact same SFX was played < 28ms ago, skip to prevent micro-stutter
        auto &lastTime = g_lastPlayTime[sfxIdx];
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTime).count();
        if (elapsedMs < 28)
            return;
        lastTime = now;

        std::lock_guard<std::mutex> lock(g_audioMutex);

        // 2. Smooth Crossfade / Fade-out on any active voice of the SAME SFX:
        // Instead of hard cutting or letting loud sounds stack additively, apply a fast 30ms smooth fade-out
        for (auto &v : g_voices)
        {
            if (v.active && v.sfx == sfx)
            {
                v.fadeOutSpeed = 1.0f / (kSampleRate * 0.030f); // 30ms smooth decay
            }
        }

        // 3. Allocate a free voice (or the oldest / quietest one)
        Voice *targetVoice = nullptr;
        float lowestVol = 999.0f;
        for (auto &v : g_voices)
        {
            if (!v.active)
            {
                targetVoice = &v;
                break;
            }
            if (v.fadeOutSpeed > 0.0f && v.volume < lowestVol)
            {
                lowestVol = v.volume;
                targetVoice = &v;
            }
        }

        if (!targetVoice)
            targetVoice = &g_voices[0];

        // 4. Initialize voice with 2ms anti-click attack and full volume
        targetVoice->active = true;
        targetVoice->sfx = sfx;
        targetVoice->cursor = 0;
        targetVoice->volume = 1.0f;
        targetVoice->attackGain = 0.0f;
        targetVoice->attackStep = 1.0f / (kSampleRate * 0.002f); // 2ms soft attack
        targetVoice->fadeOutSpeed = 0.0f;
#endif
    }

    void shutdown()
    {
#ifdef _WIN32
        if (g_running)
        {
            g_running = false;
            if (g_audioThread.joinable())
                g_audioThread.join();
        }
#endif
    }
}
#else
// Linux (and anything without waveOut): no sounds yet - the calls do nothing
namespace sound
{
    void init(const fs::path &) {}
    void play(Sfx) {}
    void shutdown() {}
}
#endif
