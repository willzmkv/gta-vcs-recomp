// VCS movies (docs/NATIVE_PORT_ROADMAP.md milestone 5; docs/game-internals.md 3 "Movies").
//
// At boot FUN_0021CA70 plays cdrom0:\MOVIES\STINGS.PSS (the logos; skip threshold 1,000,000 frames - never) and
// INTRO.PSS (threshold 10) through CMoviePlayer::Play(path, 0, threshold) at 0x40D8B8, which drives libmpeg / the
// IPU and streams the sound through the IOP. Here Play is replaced: the file is read from the disc and demuxed
// (MPEG-2 program stream: video in stream 0xE0; sound in private stream 1, sub-stream 0xFF, behind a 4-byte header -
// a Sony "SShd" header (type 1 = 16-bit little-endian PCM, 48 kHz, 2 channels, 512-byte interleave: 256 left samples,
// then 256 right), "SSbd" + size, then the samples), the video decoded with FFmpeg into RGBA and shown by the presenter
// at 4:3 (runtime/ps2_present.h), the sound mixed into the audio output (runtime/ps2_audio_out.h) - its played-sample
// count is the clock the pictures follow. The game thread waits in here, as it did in the game's own player.
#include "game/vcs_movie.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_audio_out.h"
#include "runtime/ps2_disc_image.h"
#include "runtime/ps2_pad.h"
#include "runtime/ps2_present.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if __has_include(<libavcodec/avcodec.h>)
#define VCS_MOVIES 1
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}
#else
#define VCS_MOVIES 0
#endif

namespace vcs
{
    namespace
    {
        constexpr uint32_t kMoviePlay = 0x0040D8B8u; // CMoviePlayer::Play(path, 0, frames before a skip is allowed)
        std::atomic<bool> g_skipIntro{false};

        // The PSS file, split into its video elementary stream and its sound.
        struct Pss
        {
            std::vector<uint8_t> video;
            std::vector<uint8_t> sound; // the private stream's bytes, sub-header removed (SShd / SSbd / samples)
            double videoStart = 0.0, soundStart = 0.0; // seconds: the first PTS of each
        };

        double readPts(const uint8_t *p)
        {
            const uint64_t pts = (static_cast<uint64_t>((p[0] >> 1) & 7u) << 30) | (static_cast<uint64_t>(p[1]) << 22) |
                                 (static_cast<uint64_t>(p[2] >> 1) << 15) | (static_cast<uint64_t>(p[3]) << 7) | (p[4] >> 1);
            return pts / 90000.0;
        }

        bool demux(const std::vector<uint8_t> &file, Pss &out)
        {
            bool videoPts = false, soundPts = false;
            size_t i = 0;
            const size_t n = file.size();
            while (i + 6 <= n)
            {
                if (file[i] != 0 || file[i + 1] != 0 || file[i + 2] != 1)
                {
                    ++i;
                    continue;
                }
                const uint8_t id = file[i + 3];
                if (id == 0xBA) // pack header (MPEG-2): 14 bytes + stuffing
                {
                    if (i + 14 > n)
                        break;
                    i += 14u + (file[i + 13] & 7u);
                    continue;
                }
                if (id == 0xB9) // end
                    break;
                if (id == 0xBB || id == 0xBD || id == 0xBE || id == 0xBF || (id >= 0xC0 && id <= 0xEF))
                {
                    const size_t length = (static_cast<size_t>(file[i + 4]) << 8) | file[i + 5];
                    const size_t body = i + 6, end = std::min(n, body + length);
                    if (id == 0xE0 || id == 0xBD)
                    {
                        if (body + 3 > end)
                            break;
                        const uint8_t flags = file[body + 1], headerLength = file[body + 2];
                        size_t payload = body + 3 + headerLength;
                        if (payload > end)
                            break;
                        if (id == 0xE0)
                        {
                            if ((flags & 0x80) && !videoPts)
                            {
                                out.videoStart = readPts(&file[body + 3]);
                                videoPts = true;
                            }
                            out.video.insert(out.video.end(), file.begin() + payload, file.begin() + end);
                        }
                        else if (payload + 4 <= end && file[payload] == 0xFF) // sub-stream 0xFF: the sound
                        {
                            if ((flags & 0x80) && !soundPts)
                            {
                                out.soundStart = readPts(&file[body + 3]);
                                soundPts = true;
                            }
                            out.sound.insert(out.sound.end(), file.begin() + payload + 4, file.begin() + end);
                        }
                    }
                    i = end;
                    continue;
                }
                ++i;
            }
            return !out.video.empty();
        }

        // The SShd / SSbd sound as interleaved stereo frames (empty if the format is not the PCM VCS uses).
        std::vector<int16_t> soundFrames(const std::vector<uint8_t> &sound)
        {
            std::vector<int16_t> out;
            auto u32 = [&](size_t at) {
                uint32_t v;
                std::memcpy(&v, &sound[at], 4);
                return v;
            };
            if (sound.size() < 40 || std::memcmp(sound.data(), "SShd", 4) != 0 || std::memcmp(&sound[32], "SSbd", 4) != 0)
                return out;
            const uint32_t type = u32(8), rate = u32(12), channels = u32(16), interleave = u32(20);
            if (type != 1u || rate != 48000u || channels != 2u || interleave == 0u || interleave % 2u)
            {
                std::fprintf(stderr, "[movie] sound type %u, %u Hz, %u channels, interleave %u: not played\n", type, rate, channels,
                             interleave);
                return out;
            }
            const size_t dataSize = std::min<size_t>(u32(36), sound.size() - 40);
            const uint8_t *data = &sound[40];
            const size_t samplesPerBlock = interleave / 2u;
            const size_t pairs = dataSize / (interleave * 2u);
            out.resize(pairs * samplesPerBlock * 2u);
            for (size_t b = 0; b < pairs; ++b)
            {
                const uint8_t *left = data + b * interleave * 2u, *right = left + interleave;
                for (size_t k = 0; k < samplesPerBlock; ++k)
                {
                    int16_t l, r;
                    std::memcpy(&l, left + k * 2u, 2);
                    std::memcpy(&r, right + k * 2u, 2);
                    out[(b * samplesPerBlock + k) * 2u] = l;
                    out[(b * samplesPerBlock + k) * 2u + 1u] = r;
                }
            }
            return out;
        }

        // Start / Cross on the emulated pad (keyboard, gamepad or injected input; active low)
        bool skipPressed()
        {
            uint8_t pad[32] = {};
            PSPadBackend backend;
            if (!backend.readState(0, 0, pad, sizeof(pad)))
                return false;
            const uint16_t buttons = static_cast<uint16_t>(~(pad[2] | (pad[3] << 8)));
            return (buttons & (0x0008u | 0x4000u)) != 0u;
        }

        double nowSeconds()
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

#if VCS_MOVIES
        // Plays one movie; false if it could not be read or decoded (then the game goes on as if it was skipped).
        bool play(const std::string &path, uint32_t skipAfterFrames, PS2Runtime *runtime)
        {
            IPS2DiscImage *disc = ps2_disc::image();
            uint32_t lsn = 0, size = 0;
            bool directory = false;
            if (!disc || !disc->findFile(path, lsn, size, directory) || directory || size == 0)
            {
                std::fprintf(stderr, "[movie] %s not found on the disc\n", path.c_str());
                return false;
            }
            std::vector<uint8_t> file(size);
            if (!disc->readBytes(static_cast<uint64_t>(lsn) * 2048u, size, file.data()))
                return false;
            Pss pss;
            if (!demux(file, pss))
                return false;
            std::vector<uint8_t>().swap(file);
            const std::vector<int16_t> sound = soundFrames(pss.sound);

            const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
            AVCodecContext *decoder = codec ? avcodec_alloc_context3(codec) : nullptr;
            AVCodecParserContext *parser = av_parser_init(AV_CODEC_ID_MPEG2VIDEO);
            if (!decoder || !parser || avcodec_open2(decoder, codec, nullptr) < 0)
            {
                avcodec_free_context(&decoder);
                if (parser)
                    av_parser_close(parser);
                return false;
            }
            AVPacket *packet = av_packet_alloc();
            AVFrame *frame = av_frame_alloc();
            SwsContext *scaler = nullptr;
            std::vector<uint8_t> rgba;

            // the sound starts the clock (or the host clock without audio output)
            const bool audio = ps2AudioMovieActive() && !sound.empty();
            ps2AudioMovieReset();
            size_t soundPushed = 0;
            const size_t soundFramesTotal = sound.size() / 2u;
            auto pushSound = [&] {
                if (!audio)
                    return;
                while (soundPushed < soundFramesTotal && ps2AudioMovieQueued() < 9600u) // keep ~200 ms queued
                {
                    const uint32_t chunk = static_cast<uint32_t>(std::min<size_t>(2400u, soundFramesTotal - soundPushed));
                    const uint32_t taken = ps2AudioMoviePush(&sound[soundPushed * 2u], chunk);
                    soundPushed += taken;
                    if (taken < chunk)
                        break;
                }
            };
            pushSound();
            const double hostStart = nowSeconds();
            // (the sound may end before the last pictures: from there the host clock carries on where it stopped)
            double soundClock = pss.soundStart, soundClockAt = hostStart;
            auto clock = [&] {
                if (!audio)
                    return pss.videoStart + (nowSeconds() - hostStart);
                if (soundPushed < soundFramesTotal || ps2AudioMovieQueued() > 0u)
                {
                    soundClock = pss.soundStart + ps2AudioMoviePlayed() / 48000.0;
                    soundClockAt = nowSeconds();
                    return soundClock;
                }
                return soundClock + (nowSeconds() - soundClockAt);
            };

            double frameRate = 30000.0 / 1001.0; // the stream's own rate once the decoder knows it
            uint64_t shown = 0, decoded = 0, dropped = 0;
            bool skipHeld = skipPressed(), skipped = false;
            size_t offset = 0;
            bool flushing = false, done = false;
            const double started = nowSeconds();
            while (!done && !runtime->isStopRequested())
            {
                // next decoded picture
                int got = avcodec_receive_frame(decoder, frame);
                if (got == AVERROR(EAGAIN))
                {
                    if (offset < pss.video.size())
                    {
                        uint8_t *data = nullptr;
                        int dataSize = 0;
                        const int used = av_parser_parse2(parser, decoder, &data, &dataSize, pss.video.data() + offset,
                                                          static_cast<int>(std::min<size_t>(4096u, pss.video.size() - offset)),
                                                          AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
                        offset += static_cast<size_t>(std::max(used, 0));
                        if (dataSize > 0)
                        {
                            packet->data = data;
                            packet->size = dataSize;
                            avcodec_send_packet(decoder, packet);
                        }
                    }
                    else if (!flushing)
                    {
                        avcodec_send_packet(decoder, nullptr);
                        flushing = true;
                    }
                    else
                        done = true;
                    continue;
                }
                if (got < 0)
                {
                    done = true;
                    continue;
                }
                if (decoder->framerate.num > 0 && decoder->framerate.den > 0)
                    frameRate = av_q2d(decoder->framerate);
                const double due = pss.videoStart + decoded / frameRate;
                ++decoded;
                // late by more than a frame: drop it (keeps the pictures with the sound)
                if (clock() > due + 1.0 / frameRate)
                {
                    ++dropped;
                    continue;
                }
                scaler = sws_getCachedContext(scaler, frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                                              frame->width, frame->height, AV_PIX_FMT_RGBA, SWS_POINT, nullptr, nullptr, nullptr);
                rgba.resize(static_cast<size_t>(frame->width) * frame->height * 4u);
                uint8_t *planes[1] = {rgba.data()};
                const int strides[1] = {frame->width * 4};
                sws_scale(scaler, frame->data, frame->linesize, 0, frame->height, planes, strides);
                // wait for the picture's moment, feeding the sound and watching for a skip
                while (clock() < due && !runtime->isStopRequested())
                {
                    pushSound();
                    const bool pressed = skipPressed();
                    const bool allowed = (nowSeconds() - started) * 60.0 >= skipAfterFrames; // the game's count, VBlanks
                    if (pressed && !skipHeld && allowed)
                    {
                        skipped = true;
                        break;
                    }
                    skipHeld = pressed;
                    static const bool log = std::getenv("VCS_MOVIE_LOG") != nullptr;
                    static double lastLog = 0.0;
                    if (log && nowSeconds() - lastLog > 1.0)
                    {
                        lastLog = nowSeconds();
                        std::fprintf(stderr, "[movie] %.2f s: pushed %zu, queued %u, played %llu, clock %.2f, due %.2f\n",
                                     lastLog - started, soundPushed, ps2AudioMovieQueued(),
                                     static_cast<unsigned long long>(ps2AudioMoviePlayed()), clock(), due);
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                if (skipped)
                    break;
                // the PS2 showed the 512x448 picture across a 4:3 TV
                ps2ShowMovieFrame(rgba.data(), frame->width, frame->height, 4.0f / 3.0f);
                ++shown;
                pushSound();
            }
            // the sound's tail
            while (!skipped && audio && !runtime->isStopRequested() && (soundPushed < soundFramesTotal || ps2AudioMovieQueued()))
            {
                pushSound();
                if (skipPressed() && !skipHeld && (nowSeconds() - started) * 60.0 >= skipAfterFrames)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            ps2AudioMovieReset();
            ps2EndMovieFrames();
            std::fprintf(stderr, "[movie] %s: %llu pictures shown, %llu dropped, %.1f s%s%s\n", path.c_str(),
                         static_cast<unsigned long long>(shown), static_cast<unsigned long long>(dropped), nowSeconds() - started,
                         skipped ? ", skipped" : "", audio ? "" : ", no sound output (host clock)");
            sws_freeContext(scaler);
            av_frame_free(&frame);
            av_packet_free(&packet);
            av_parser_close(parser);
            avcodec_free_context(&decoder);
            return true;
        }
#endif

        // replaces CMoviePlayer::Play(path, 0, skipAfterFrames) (0x40D8B8)
        PS2_GUEST_CC void moviePlay(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const std::string path(reinterpret_cast<const char *>(rdram + (GPR_U32(ctx, 4) & PS2_RAM_MASK)));
            const uint32_t skipAfter = GPR_U32(ctx, 6);
            if (g_skipIntro.load(std::memory_order_relaxed))
                std::fprintf(stderr, "[movie] %s skipped (Display > Intro movies)\n", path.c_str());
#if VCS_MOVIES
            else
                play(path, skipAfter, runtime);
#else
            else
                std::fprintf(stderr, "[movie] %s: built without FFmpeg, skipped\n", path.c_str());
            (void)skipAfter;
            (void)runtime;
#endif
            SET_GPR_U64(ctx, 2, 0);
            ctx->pc = GPR_U32(ctx, 31);
        }
    }

    void setSkipIntroMovies(bool skip) { g_skipIntro.store(skip, std::memory_order_relaxed); }

    void installMovieHooks(PS2Runtime &runtime)
    {
        runtime.replaceFunction(kMoviePlay, &moviePlay);
    }
}
