// VCS sound without the IOP (docs/NATIVE_PORT_ROADMAP.md milestone 6 step 2; docs/game-internals.md 9).
//
// On the PS2 the game's sound runs in Rockstar's CDSTREAM.IRX on the IOP, driving the SPU2 through Sony's libsd.
// This is that code ported to the host, function by function (module offsets in the comments; read with
// tools/irxdis.py --decompile), on sound/libsd's native libsd and an SPU2 core of its own:
//
//   0x65686579 sound effects   68 banks of ADPCM sounds in SPU2 RAM (\AUDIO\SETn\SFXm_PS.RAW, their sound tables in
//                              SFXm_PS.SDT), loaded on request; every frame the EE sends a block with the state of
//                              the 44 effect voices (sound, bank, rate, volume, pan, key on / off) which is applied.
//   0x65686578 music           one stereo .VB stream at a time (radio stations, cutscene audio: \AUDIO\MUSIC\...,
//                              \AUDIO\CUTSCENE\... - 112 files): 256 KB disc buffers feeding two 16 KB SPU2 ring
//                              buffers played by core 1 voices 22 / 23, refilled half by half on SPU2 IRQs.
//
// The IOP's threads become steps run on the EE thread: an RPC's work happens in the call, the music thread's loop
// runs one step per semaphore signal (max count 1, as created) from RPCs, transfer completions and SPU2 IRQs. Disc
// reads complete at once. Tables the module carries (file names, SPU2 addresses) are read from the disc's own
// MODULES\CDSTREAM.IRX at start; nothing from the game is in this source.
#include "game/vcs_sound.h"

#include "ps2_runtime.h"
#include "ps2x/iop/iop_spu2.h"
#include "runtime/ps2_audio_out.h"
#include "runtime/ps2_disc_image.h"
#include "runtime/ps2_iop_native.h"
#include "sound/libsd.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace vcs
{
    namespace
    {
        using Spu2 = ps2x::iop::detail::IopSpu2;

        constexpr uint32_t kSidMusic = 0x65686578u;
        constexpr uint32_t kSidSfx = 0x65686579u;
        constexpr uint32_t kBanks = 0x44;         // 68 sound banks
        constexpr uint32_t kVoices = 44;          // effect voices: core 0 voices 0-23, core 1 voices 0-19
        constexpr uint32_t kMusicFiles = 0x70;    // 112 streams
        constexpr uint32_t kBlockBytes = 0x5A4;   // the per-frame voice block
        constexpr uint32_t kBankBuffer = 0x20000; // the IOP buffer bank data passes through
        constexpr uint32_t kStreamBuffer = 0x40000;
        constexpr uint32_t kMusicThreadId = 13; // the IOP thread id music init returns on the emulated IOP (non-zero = ok)

        // First sound of each bank in the sound table (CDSTREAM 0x3B10's constants, module data 0x9FC0)
        constexpr uint32_t kBankFirstSound[kBanks] = {
            0x0000, 0x0124, 0x0125, 0x16BB, 0x16C3, 0x16C6, 0x16C9, 0x16CC, 0x16CF, 0x16D2, 0x16D5, 0x16D8, 0x16DB, 0x16DE,
            0x16E1, 0x16E4, 0x16E7, 0x16EA, 0x16ED, 0x16EF, 0x16F0, 0x16F4, 0x16F8, 0x16FC, 0x1700, 0x1703, 0x1707, 0x1708,
            0x1709, 0x170A, 0x170B, 0x170F, 0x1710, 0x1711, 0x1712, 0x1713, 0x1718, 0x1719, 0x171A, 0x171B, 0x171C, 0x171D,
            0x171E, 0x171F, 0x1720, 0x1721, 0x1722, 0x1723, 0x1724, 0x1725, 0x1726, 0x1727, 0x1728, 0x1729, 0x172A, 0x172D,
            0x172E, 0x1732, 0x1733, 0x1734, 0x1735, 0x1736, 0x173A, 0x173F, 0x1744, 0x1749, 0x174D, 0x1751,
        };
        // SPU2 addresses of the special-sound slots by type 0-4 (0x4B8C / 0x50DC)
        constexpr uint32_t kTypedSlot[5] = {0x3A810, 0x45010, 0x4F810, 0x5A010, 0x64810};

        struct DiscFile // a sceCdlFILE as sceCdSearchFile fills it: the parts used
        {
            uint32_t lsn = 0, size = 0;
        };

        bool findDiscFile(const std::string &path, DiscFile &file)
        {
            IPS2DiscImage *disc = ps2_disc::image();
            bool directory = false;
            return disc && disc->findFile(path, file.lsn, file.size, directory) && !directory;
        }

        // sectors from the disc into `dest` (room for `capacity` bytes)
        bool readSectors(uint32_t lsn, uint32_t sectors, uint8_t *dest, size_t capacity)
        {
            IPS2DiscImage *disc = ps2_disc::image();
            const size_t bytes = std::min<size_t>(static_cast<size_t>(sectors) * 2048u, capacity);
            if (disc && bytes && disc->readBytes(static_cast<uint64_t>(lsn) * 2048u, bytes, dest))
                return true;
            if (bytes)
                std::fprintf(stderr, "[sound] disc read of %u sectors at LSN 0x%x failed\n", sectors, lsn);
            return false;
        }

        // CDSTREAM.IRX as loaded at address 0: its sections at their addresses (the tables are read from here)
        struct Module
        {
            std::vector<uint8_t> image;
            uint32_t word(uint32_t address) const
            {
                uint32_t v = 0;
                if (address + 4 <= image.size())
                    std::memcpy(&v, image.data() + address, 4);
                return v;
            }
            std::string string(uint32_t address) const
            {
                std::string s;
                for (uint32_t a = address; a < image.size() && image[a]; ++a)
                    s += static_cast<char>(image[a]);
                return s;
            }
        };

        bool loadModule(Module &module)
        {
            DiscFile file;
            if (!findDiscFile("cdrom0:\\MODULES\\CDSTREAM.IRX;1", file) || file.size != 48540u)
                return false;
            std::vector<uint8_t> elf((file.size + 2047u) & ~2047u);
            if (!readSectors(file.lsn, static_cast<uint32_t>(elf.size() / 2048u), elf.data(), elf.size()))
                return false;
            auto u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, elf.data() + o, 4); return v; };
            auto u16 = [&](size_t o) { uint16_t v; std::memcpy(&v, elf.data() + o, 2); return v; };
            const uint32_t shoff = u32(0x20), shnum = u16(0x30), shstr = u16(0x32);
            if (shoff + shnum * 40u > file.size || shstr >= shnum)
                return false;
            const uint32_t names = u32(shoff + shstr * 40u + 16u);
            uint32_t sizes = 0;
            module.image.assign(0x8200, 0);
            for (uint32_t i = 0; i < shnum; ++i)
            {
                const size_t h = shoff + i * 40u;
                const char *name = reinterpret_cast<const char *>(elf.data() + names + u32(h));
                const uint32_t addr = u32(h + 12), offset = u32(h + 16), size = u32(h + 20);
                const bool wanted = !std::strcmp(name, ".text") || !std::strcmp(name, ".rodata") || !std::strcmp(name, ".data");
                if (!wanted)
                    continue;
                if (addr + size > module.image.size() || offset + size > file.size)
                    return false;
                std::memcpy(module.image.data() + addr, elf.data() + offset, size);
                sizes += size;
            }
            return sizes == 0x63C0u + 0x1A70u + 0x370u; // SLUS-21590's module: .text + .rodata + .data
        }

        // A playing stream's stop waits for the next refill IRQ, as on the IOP (Engine::musicCommand 0x80700)
        bool g_stopAtIrq = [] {
            const char *e = std::getenv("VCS_MUSIC_STOP_WAIT");
            return e && *e == '1';
        }();

        class Engine
        {
        public:
            bool start()
            {
                if (!loadModule(m_module))
                {
                    std::fprintf(stderr, "[sound] MODULES\\CDSTREAM.IRX not found or not the expected build - sound stays on the IOP\n");
                    return false;
                }
                for (uint32_t i = 0; i < kBanks; ++i)
                    m_bankSpu[i] = m_module.word(0x8000 + i * 4); // module data 0x8000
                m_musicSpu[0] = m_module.word(0x7F48);
                m_musicSpu[1] = m_module.word(0x7F4C);
                m_voice[0] = m_module.word(0x7F58);
                m_voice[1] = m_module.word(0x7F5C);
                m_rate = static_cast<int32_t>(m_module.word(0x7F94));
                m_spu.setSampleSink([](const int16_t *stereo, uint32_t frames) { ps2AudioPush(stereo, frames); });
                return true;
            }

            void call(uint8_t *rdram, const ps2x::iop::RpcRequest &request)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                uint8_t *send = guest(rdram, request.send.address, request.send.size);
                std::array<uint8_t, kBlockBytes> buffer{};
                if (send)
                    std::memcpy(buffer.data(), send, std::min<uint32_t>(request.send.size, kBlockBytes));
                int32_t first;
                std::memcpy(&first, buffer.data(), 4);
                uint32_t &reply = request.sid == kSidSfx ? m_sfxReply : m_musicReply;
                if (request.sid == kSidSfx)
                    sfxCommand(request.function, first, buffer.data());
                else
                    musicCommand(request.function, first);
                pump();
                // the reply: the server's result word, then what follows it in IOP memory - zeros here
                if (uint8_t *receive = guest(rdram, request.receive.address, request.receive.size))
                {
                    std::memset(receive, 0, request.receive.size);
                    std::memcpy(receive, &reply, std::min<uint32_t>(request.receive.size, 4));
                }
            }

            void setSink(Spu2::SampleSink sink) { m_spu.setSampleSink(std::move(sink)); }

            // IOP time: the SPU2 mixes up to it in steps of 32 samples, so its IRQs reach the music stream on time
            void clock(uint64_t eeCycles)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_eeCycles += eeCycles;
                const uint64_t target = m_eeCycles / 8u;
                while (m_iopCycle < target)
                {
                    // to the next transfer completion at the latest, so the stream's next step follows it in time
                    m_iopCycle = std::min({target, m_iopCycle + Spu2::kCyclesPerSample * 32u, std::max(m_sd.nextEvent(), m_iopCycle + 1)});
                    m_spu.advanceTo(m_iopCycle);
                    m_sd.advance(m_iopCycle);
                    if (m_spu.takeIrq())
                        m_sd.serviceIrq();
                    pump();
                }
            }

        private:
            static uint8_t *guest(uint8_t *rdram, uint32_t address, uint32_t size)
            {
                address &= 0x1FFFFFFFu;
                if (!rdram || !address || !size || address >= PS2_RAM_SIZE || size > PS2_RAM_SIZE - address)
                    return nullptr;
                return rdram + address;
            }

            // Transfer callbacks and music steps until nothing is pending (callbacks run after the code that started
            // the transfer, as from the DMA interrupt).
            void pump()
            {
                for (int guard = 0; guard < 1000; ++guard)
                {
                    bool ran = m_sd.flushCallbacks();
                    if (m_musicSignal && !m_waitingStart && m_musicThread)
                    {
                        m_musicSignal = false;
                        musicStep();
                        ran = true;
                    }
                    if (!ran)
                        return;
                }
                std::fprintf(stderr, "[sound] music stream did not settle\n");
            }

            void musicSignal() { m_musicSignal = true; } // SignalSema(0x7F54): max count 1

            // ======== sound effects (server 0x65686579, handler 0x57C0) ==================================================
            void sfxCommand(uint32_t command, int32_t first, const uint8_t *buffer)
            {
                const uint32_t index = command & 0xFF;
                const int32_t high = first >> 8, low = first & 0xFF;
                switch (command & 0xFFFF00)
                {
                case 0xA0000: m_sfxReply = sfxInit(first); break;
                case 0xA0200: sfxShutdown(); break;
                case 0xA0A00: m_sfxReply = voicePlaying(index); break;
                case 0xA0B00: m_sfxReply = soundLength(first); break;
                case 0xA0C00: m_sfxReply = soundRate(first); break;
                case 0xA0D00: m_sfxReply = static_cast<uint32_t>(requestBank(static_cast<uint32_t>(high), low)); break;
                case 0xA0F00: if (m_sfxReady) m_sfxVolume = static_cast<int8_t>(first); break; // 0x4110
                case 0xB0000: voiceBlock(buffer); break;
                case 0xB0100: m_sfxReply = bankStatus(static_cast<uint32_t>(high), low); break;
                case 0xB0200: m_sfxReply = static_cast<uint32_t>(specialReady(static_cast<uint32_t>(high), low)); break;
                case 0xB0300: m_sfxReply = static_cast<uint32_t>(requestSpecial(high, low)); break;
                default: break;
                }
            }

            uint32_t sfxInit(int32_t extraSounds) // 0x39D8 / 0x3B10
            {
                if (m_sfxReady)
                    return 1;
                m_sd.init(0);
                m_sd.setCoreAttr(10, 0x800);
                for (uint32_t e : {0x980u, 0xA80u, 0x981u, 0xA81u})
                    m_sd.setParam(e, 0);
                m_bankBuffer.assign(kBankBuffer, 0);
                m_soundTable.assign(static_cast<size_t>(std::max(0, extraSounds + 0x1752)) * 3u, 0);
                for (uint32_t e : {0x980u, 0xA80u, 0x981u, 0xA81u})
                    m_sd.setParam(e, 0x3FFF);
                if (!loadSoundTables(extraSounds))
                    return 0;
                m_sfxVolume = 0x7F;
                m_block.fill(0);
                for (uint32_t v = 0; v < kVoices; ++v)
                    for (uint32_t array : {0u, 1u, 4u, 5u, 6u}) // sound, bank, rate, pan, volume: none
                        setBlockWord(9 + array * kVoices + v, -1);
                m_bankMaxVolume.fill(0);
                m_bankLoaded.fill(0);
                m_slotAddr.fill(0xFFFFFFFFu);
                m_slotSound.fill(-1);
                m_slotCursor = 0;
                m_specialSound = m_specialLoad = -1;
                m_typedSound.fill(-1);
                m_buildingSel = -1;
                m_buildingSlot = {0x45, 0x45};
                m_sfxReady = true;
                m_sd.setCoreAttr(10, 0x800);
                for (uint32_t v = 0; v < 24; ++v)
                {
                    m_sd.setParam(v << 1 | 0x300, 0xF0F); // ADSR: instant attack, full sustain
                    m_sd.setParam(v << 1 | 0x400, 0x1FC7);
                    m_sd.setParam(v << 1 | 0x301, 0xF0F);
                    m_sd.setParam(v << 1 | 0x401, 0x1FC7);
                }
                return 1;
            }

            bool loadSoundTables(int32_t extraSounds) // 0x4878: every bank's .SDT into the sound table
            {
                for (uint32_t bank = 0; bank < kBanks; ++bank)
                {
                    const std::string name = std::to_string(bank / 10) + "\\SFX" + std::to_string(bank);
                    findDiscFile("cdrom0:\\AUDIO\\SET" + name + "_PS.SDT;1", m_sdtFile[bank]);
                    findDiscFile("cdrom0:\\AUDIO\\SET" + name + "_PS.RAW;1", m_rawFile[bank]);
                    const int32_t sounds = bank < kBanks - 1 ? static_cast<int32_t>(kBankFirstSound[bank + 1] - kBankFirstSound[bank])
                                                             : extraSounds - static_cast<int32_t>(kBankFirstSound[bank]) + 0x1751;
                    const int32_t bytes = sounds * 12;
                    if (bytes > 0x1FFFF || ((bytes + 0x7FF) >> 11) < 1)
                        return false;
                    readSectors(m_sdtFile[bank].lsn, static_cast<uint32_t>((bytes + 0x7FF) >> 11), m_bankBuffer.data(), m_bankBuffer.size());
                    const size_t at = static_cast<size_t>(kBankFirstSound[bank]) * 3u;
                    if (at * 4u + static_cast<size_t>(bytes) <= m_soundTable.size() * 4u)
                        std::memcpy(m_soundTable.data() + at, m_bankBuffer.data(), static_cast<size_t>(bytes));
                }
                m_specialLsn = m_rawFile[2].lsn; // module 0x81A0
                return true;
            }

            void sfxShutdown() // 0x408C
            {
                if (!m_sfxReady)
                    return;
                m_sfxReady = false;
                m_sd.setSwitch(0x1600, 0xFFFFFF);
                m_sd.setSwitch(0x1601, 0xFFFFFF);
            }

            static bool voiceOf(uint32_t voice, uint32_t &core, uint32_t &index) // 0x4ABC
            {
                if (voice >= kVoices)
                    return false;
                core = voice >= 24 ? 1 : 0;
                index = voice - core * 24;
                return true;
            }

            uint32_t voicePlaying(uint32_t voice) // 0x4138: envelope above zero
            {
                uint32_t core, index;
                return voiceOf(voice, core, index) && m_sd.getParam(core | index << 1 | 0x500) != 0 ? 1u : 0u;
            }

            const uint32_t *sound(int32_t id) const
            {
                const size_t at = static_cast<size_t>(id) * 3u;
                return id >= 0 && at + 3u <= m_soundTable.size() ? m_soundTable.data() + at : nullptr;
            }

            uint32_t soundLength(int32_t id) const // 0x4184: samples (bytes * 28 / 16)
            {
                const uint32_t *s = m_sfxReady ? sound(id) : nullptr;
                return s ? (s[1] * 7u) >> 2 : 0xFFFFFFFFu;
            }

            uint32_t soundRate(int32_t id) const // 0x41CC
            {
                const uint32_t *s = m_sfxReady ? sound(id) : nullptr;
                return s ? s[2] : 0xFFFFFFFFu;
            }

            int32_t blockWord(uint32_t word) const
            {
                int32_t v;
                std::memcpy(&v, m_block.data() + word * 4u, 4);
                return v;
            }
            void setBlockWord(uint32_t word, int32_t v) { std::memcpy(m_block.data() + word * 4u, &v, 4); }

            // 0x4AF8 + the voice thread 0x4B8C: words 3-4 key on per core, 5-6 key off, then per voice from word 9 on:
            // sound, bank, -, -, rate (Hz), pan (0-127), volume, - (arrays of 44)
            void voiceBlock(const uint8_t *buffer)
            {
                if (!m_sfxReady)
                    return;
                std::memcpy(m_block.data(), buffer, kBlockBytes);
                uint32_t blocked[2] = {0, 0}; // voices whose bank is not loaded: not keyed on
                for (uint32_t core = 0; core < 2; ++core)
                    if (const int32_t off = blockWord(5 + core))
                        m_sd.setSwitch(core | 0x1600, static_cast<uint32_t>(off));
                for (uint32_t v = 0; v < kVoices; ++v)
                {
                    uint32_t core, index;
                    voiceOf(v, core, index);
                    const uint32_t entry = core | index << 1;
                    const int32_t id = blockWord(9 + v), bank = blockWord(9 + kVoices + v);
                    if (id != -1 && bank != -1)
                    {
                        m_voiceSound[v] = id;
                        m_voiceBank[v] = bank;
                        if (bank < 0 || bank >= static_cast<int32_t>(kBanks) || m_bankLoaded[bank] != 1)
                        {
                            blocked[core] |= 1u << index;
                            continue;
                        }
                        uint32_t address;
                        if (bank == 2 || bank == 0x43)
                        {
                            if (v >= 0x26 && v <= 0x2A)
                                address = kTypedSlot[v - 0x26];
                            else if (static_cast<uint32_t>(id - 0x1869E) < 2u)
                                address = 0x74010;
                            else
                            {
                                bool found = false;
                                address = 0x3A810;
                                for (int32_t k = 0; k < 2 && !found; ++k)
                                {
                                    int32_t slot = static_cast<int8_t>(static_cast<uint8_t>(m_slotCursor) - k - 1);
                                    if (slot < 0)
                                        slot = static_cast<int8_t>(slot + 5);
                                    if (m_slotSound[slot] == id)
                                    {
                                        address = m_slotAddr[slot];
                                        found = true;
                                    }
                                }
                                if (!found)
                                    continue;
                            }
                        }
                        else
                        {
                            const uint32_t *s = sound(id);
                            address = (s ? s[0] : 0u) + m_bankSpu[bank];
                        }
                        m_sd.setAddr(entry | 0x2040, address + 0x30); // past the 48-byte VAG header
                    }
                    const int32_t rate = blockWord(9 + 4 * kVoices + v);
                    if (rate != -1)
                        m_sd.setParam(entry | 0x200, static_cast<uint16_t>((rate << 12) / 48000));
                    const int32_t volume = blockWord(9 + 6 * kVoices + v), pan = blockWord(9 + 5 * kVoices + v);
                    if (volume != -1 && pan != -1)
                    {
                        const int32_t left = std::min(((volume * (127 - pan)) / 127) * 2, volume);
                        const int32_t right = std::min(((volume * pan) / 127) * 2, volume);
                        m_sd.setParam(entry, static_cast<uint16_t>(left * m_sfxVolume));
                        m_sd.setParam(entry | 0x100, static_cast<uint16_t>(right * m_sfxVolume));
                    }
                }
                for (uint32_t core = 0; core < 2; ++core)
                {
                    const uint32_t on = ~blocked[core] & static_cast<uint32_t>(blockWord(3 + core)) & 0xFFFFFFu;
                    if (on)
                    {
                        m_sd.setSwitch(core | 0x1600, on);
                        m_sd.setSwitch(core | 0x1500, on);
                    }
                }
                // (ours) VCS_SOUND_VOICES=1: every sound a voice is given, with its bank (finding a sound's id: play it)
                static const bool voiceLog = std::getenv("VCS_SOUND_VOICES") != nullptr;
                if (voiceLog)
                    for (uint32_t v = 0; v < kVoices; ++v)
                        if (blockWord(9 + v) != -1 && blockWord(9 + kVoices + v) != -1)
                            std::fprintf(stderr, "[sound-voice] voice %u sound 0x%x bank 0x%x volume %d pan %d\n", v, m_voiceSound[v],
                                         m_voiceBank[v], blockWord(9 + 6 * kVoices + v), blockWord(9 + 5 * kVoices + v));
                pickAux();
            }

            // the second output's voices (setSecondOutput): asked at every voice block
            void pickAux()
            {
                uint32_t mask[2] = {0, 0};
                if (m_pick)
                    for (uint32_t v = 0; v < kVoices; ++v)
                        if (m_voiceSound[v] != -1 && m_pick(m_voiceSound[v], m_voiceBank[v], v))
                        {
                            uint32_t core, index;
                            voiceOf(v, core, index);
                            mask[core] |= 1u << index;
                        }
                m_spu.setAuxVoices(0, mask[0]);
                m_spu.setAuxVoices(1, mask[1]);
            }

        public:
            void setPick(VoicePick pick)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_pick = pick;
                if (pick)
                    m_spu.setAuxSink([](const int16_t *stereo, uint32_t frames) { ps2AudioAuxPush(stereo, frames); });
                else
                    m_spu.setAuxSink(nullptr);
                pickAux();
            }

        private:

            // 0x4524: bank `bank` wanted (mode 0xFF loads it). Level banks 3-0x27 share one SPU2 area, building banks
            // 0x28-0x3C two slots (the quieter one is replaced), 0x3D-0x42 one area.
            int32_t requestBank(uint32_t bank, int32_t mode)
            {
                if (!m_sfxReady || m_loading != -1)
                    return 0;
                if (bank >= kBanks)
                    return -1;
                if (m_bankLoaded[bank] != 0)
                    return 1;
                if (mode != 0xFF)
                    return 0;
                if (bank - 3u < 0x25u)
                {
                    for (uint32_t b = 3; b < 0x28; ++b)
                        m_bankLoaded[b] = 0;
                }
                else if (bank - 0x28u < 0x15u)
                {
                    int32_t free = -1;
                    if (m_buildingSlot[0] == 0x45)
                        free = 0;
                    else if (m_buildingSlot[1] == 0x45)
                        free = 1;
                    if (free >= 0)
                        m_buildingSel = free;
                    else
                    {
                        m_bankMaxVolume.fill(0);
                        for (uint32_t v = 0; v < kVoices; ++v)
                        {
                            const int32_t b = blockWord(9 + kVoices + v), volume = blockWord(9 + 6 * kVoices + v);
                            if (b != -1 && b >= 0 && b < static_cast<int32_t>(kBanks) && m_bankMaxVolume[b] < volume)
                                m_bankMaxVolume[b] = volume;
                        }
                        m_buildingSel = 0;
                        if (maxVolume(m_buildingSlot[1]) < maxVolume(m_buildingSlot[0]))
                            m_buildingSel = 1;
                        const int32_t evicted = m_buildingSlot[m_buildingSel];
                        if (evicted >= 0 && evicted < static_cast<int32_t>(kBanks))
                            m_bankLoaded[evicted] = 0;
                    }
                }
                else if (bank - 0x3Du < 6u)
                {
                    if (static_cast<int32_t>(bank) == m_currentArea)
                        return 1;
                    if (m_currentArea >= 0 && m_currentArea < static_cast<int32_t>(kBanks))
                        m_bankLoaded[m_currentArea] = 0;
                }
                m_loading = m_loadingBank = static_cast<int32_t>(bank);
                loadBank();
                return 1;
            }

            int32_t maxVolume(int32_t bank) const
            {
                return bank >= 0 && bank < static_cast<int32_t>(kBanks) ? m_bankMaxVolume[bank] : 0;
            }

            uint32_t bankStatus(uint32_t bank, int32_t mode) const // 0x47B4
            {
                if (bank >= kBanks)
                    return 0xFFFFFFFFu;
                if (m_loading == -1 || m_loading == 2 || m_loading == 0x43)
                    return mode == 0xFF && m_bankLoaded[bank] == 1 ? 1u : 0u;
                return mode == 0xFF && m_loading == static_cast<int32_t>(bank) ? 2u : 0u;
            }

            // 0x4208: a special sound (speech, mission audio) into slot `type` (0-7; 0xFF = the 5-entry ring). Sounds
            // 0x125-0x16BA live in bank 2's file, those above 0x1750 in bank 0x43's.
            int32_t requestSpecial(int32_t id, int32_t type)
            {
                if (!m_sfxReady || m_loading != -1 || m_specialSound != -1)
                    return 0;
                if (static_cast<uint32_t>(id - 0x125) < 0x1596u)
                    m_loading = m_loadingBank = 2;
                else if (id > 0x1750)
                    m_loading = m_loadingBank = 0x43;
                else
                    return 1;
                m_specialSound = m_specialLoad = id;
                m_specialType = type;
                loadBank();
                return 1;
            }

            bool inRing(int32_t id) const
            {
                for (int32_t k = 0; k < 2; ++k)
                {
                    int32_t slot = m_slotCursor - k - 1;
                    if (slot < 0)
                        slot = m_slotCursor - k + 4;
                    if (slot >= 0 && slot < 5 && m_slotSound[slot] == id)
                        return true;
                }
                return false;
            }

            int32_t specialReady(uint32_t id, int32_t type) const // 0x42E4
            {
                const int32_t sid = static_cast<int32_t>(id);
                if (m_specialSound == -1)
                {
                    if (type == 0xFF)
                        return inRing(sid) ? 1 : 0;
                    if (type < 5 && type >= 0)
                        return m_typedSound[type] == sid && m_bankLoaded[0x43] == 1 ? 1 : 0;
                    if (type == 5)
                        return sid == m_typed5 ? 1 : 0;
                    if (type == 6)
                        return sid == m_typed6 ? 1 : 0;
                    if (type == 7)
                        return sid == m_typed7 ? 1 : 0;
                    return 0;
                }
                if (m_specialSound == sid)
                    return type == m_specialType ? 2 : 0;
                if (type == 0xFF)
                    return static_cast<int32_t>(id) <= 0x16B9 && inRing(sid) ? 1 : 0;
                if (type >= 0 && type < 5)
                    return sid == m_typedSound[type] ? 1 : 0;
                if (type == 5)
                    return sid == m_typed5 ? 1 : 0;
                if (type == 6)
                    return sid == m_typed6 ? 1 : 0;
                if (type == 7)
                    return sid == m_typed7 ? 1 : 0;
                return 0;
            }

            void abortLoad() // 0x5074
            {
                if (m_loadingBank - 0x28u < 0x15u)
                    m_buildingSel = -1;
                else if (m_loadingBank == 2 || m_loadingBank == 0x43)
                    m_specialSound = m_specialLoad = -1;
                m_loading = m_loadingBank = -1;
            }

            // The loader thread's work (0x50DC): the bank's .RAW (or one special sound out of it) through the IOP
            // buffer into SPU2 RAM, 128 KB at a time.
            void loadBank()
            {
                const int32_t bank = m_loadingBank;
                const int32_t cursor = m_slotCursor;
                int32_t ringSlot = 0;
                if (bank < 0 || bank >= static_cast<int32_t>(kBanks))
                    return finishLoad();
                if (bank - 0x28u < 0x15u)
                {
                    std::fprintf(stderr, "[sound] BUILDING BANKS NOT USED. SHOULD NOT BE DOING THIS.\n");
                    if (m_buildingSel != -1)
                        m_bankSpu[bank] = 0x21810;
                }
                else if (bank == 2)
                {
                    if (m_specialType == 0)
                        m_bankSpu[2] = 0x3A810;
                    else if (m_specialType == 1)
                        m_bankSpu[2] = 0x45010;
                    else if (m_specialType == 5 || m_specialType == 6 || m_specialType == 7)
                        m_bankSpu[2] = 0x74010;
                    else if (m_specialType == 0xFF)
                    {
                        m_bankSpu[2] = static_cast<uint32_t>(cursor) * 0x5000u + 0x21810u;
                        m_slotAddr[cursor] = m_bankSpu[2];
                        ringSlot = cursor;
                    }
                }
                else if (bank == 0x43 && m_specialType >= 0 && m_specialType < 5)
                    m_bankSpu[0x43] = kTypedSlot[m_specialType];

                uint32_t remaining, firstSector = 0, skip = 0;
                if (bank == 2 || bank == 0x43)
                {
                    if (m_specialType == 0xFF)
                        m_slotCursor = (m_slotCursor + 1) % 5; // 0x5050
                    const uint32_t *s = sound(m_specialLoad);
                    const uint32_t offset = s ? s[0] : 0u;
                    remaining = s ? s[1] : 0u;
                    firstSector = offset >> 11;
                    skip = offset & 0x7FF;
                }
                else
                    remaining = m_rawFile[bank].size;
                const int32_t sectors = static_cast<int32_t>((remaining + skip + 0x7FF) >> 11);
                int32_t left = sectors;
                uint32_t spuOffset = 0;
                while (static_cast<int32_t>(remaining) > 0)
                {
                    const int32_t count = std::min(left, 0x40);
                    const uint32_t chunk = left > 0x40 ? 0x20000u : remaining;
                    uint32_t lsn;
                    if (bank == 2)
                        lsn = m_specialLsn + firstSector + static_cast<uint32_t>(sectors - left);
                    else if (bank == 0x43)
                        lsn = m_rawFile[0x43].lsn + firstSector + static_cast<uint32_t>(sectors - left);
                    else
                        lsn = m_rawFile[bank].lsn + static_cast<uint32_t>(sectors - left);
                    left -= count;
                    remaining -= chunk;
                    readSectors(lsn, static_cast<uint32_t>(count), m_bankBuffer.data(), m_bankBuffer.size());
                    if (static_cast<uint32_t>(count) * 0x800u > skip)
                        std::memmove(m_bankBuffer.data(), m_bankBuffer.data() + skip, static_cast<uint32_t>(count) * 0x800u - skip);
                    if (static_cast<int32_t>(chunk) < 1)
                        return abortLoad();
                    m_sd.voiceTransStatus(0, 1);
                    m_sd.voiceTransStatus(1, 1);
                    if (m_sd.voiceTrans(0, 0, m_bankBuffer.data(), kBankBuffer, m_bankSpu[bank] + spuOffset, chunk) < 0)
                        return abortLoad();
                    m_sd.voiceTransStatus(0, 1);
                    spuOffset += 0x20000;
                }
                if (bank - 0x28u < 0x15u)
                {
                    if (m_buildingSel >= 0 && m_buildingSel < 2)
                        m_buildingSlot[m_buildingSel] = bank;
                    m_buildingSel = -1;
                }
                else if (bank != 2 && bank != 0x43)
                {
                    if (bank - 0x3Du < 6u)
                        m_currentArea = bank;
                }
                else
                {
                    if (m_specialType == 0xFF)
                        m_slotSound[ringSlot] = m_specialLoad;
                    else if (m_specialType >= 0 && m_specialType < 5)
                    {
                        m_specialLoad = m_specialSound;
                        m_typedSound[m_specialType] = m_specialLoad;
                    }
                    else if (m_specialType == 5)
                        m_typed5 = m_specialSound;
                    else if (m_specialType == 6)
                        m_typed6 = m_specialSound;
                    else if (m_specialType == 7)
                        m_typed7 = m_specialSound;
                    m_specialSound = m_specialLoad = -1;
                }
                finishLoad();
            }

            void finishLoad()
            {
                const int32_t bank = m_loadingBank;
                m_loading = m_loadingBank = -1;
                if (bank >= 0 && bank < static_cast<int32_t>(kBanks))
                    m_bankLoaded[bank] = 1;
            }

            // ======== music (server 0x65686578, handler 0x36DC; thread 0x26E8) ==========================================
            void musicCommand(uint32_t command, int32_t first)
            {
                const uint32_t index = command & 0xFF;
                // VCS_MUSIC_LOG=1: the music commands other than the frequent polls / volume steps, with the state found
                static const bool log = std::getenv("VCS_MUSIC_LOG") != nullptr;
                if (log && (command & 0xFFF00) != 0x80E00 && (command & 0xFFF00) != 0x00B00 && (command & 0xFFF00) != 0x90100)
                    std::fprintf(stderr, "[music] cmd 0x%05x arg %d (state 0x%x, file %d)\n", command, first, m_state, m_file);
                switch (command & 0xFFF00)
                {
                case 0x00100: musicShutdown(); break;
                case 0x00B00: m_volumePending = true; m_volume = static_cast<uint32_t>(first); break; // 0x21EC
                case 0x00C00: setMusicVolume(static_cast<uint32_t>(first)); break;
                case 0x00D00: // 0x235C: a core's master volume
                    m_sd.setParam((index & 0xFFFF) | 0x980, static_cast<uint16_t>(static_cast<uint32_t>(first) >> 16));
                    m_sd.setParam((index & 0xFFFF) | 0xA80, static_cast<uint16_t>(first));
                    break;
                case 0x7FF00: break;
                case 0x80000: m_musicReply = musicInit(); break;
                case 0x80200: m_musicReply = openStream(static_cast<int32_t>(index), first) ? 1u : 0u; break;
                case 0x80700:
                    m_musicReply = static_cast<uint32_t>(requestStop());
                    // A stop while the stream plays is seen at the music thread's next step - on the IOP the next refill
                    // IRQ, up to 448 ms away (the handler 0x200C only sets the flag). The game waits for it: the level swap
                    // (FUN_003C6D08, Beach <-> Mainland) stops the music and polls the state in DelayThread(1000) steps,
                    // the whole game frozen meanwhile (PERFORMANCE.md 21). Here the step runs at once, so the swap does
                    // not wait on the stream; the music only ends that much sooner. VCS_MUSIC_STOP_WAIT=1 (and the
                    // replay comparison): at the IRQ, as the IOP.
                    if (m_stopRequest && m_state == 0x5000 && !g_stopAtIrq)
                        musicSignal();
                    break;
                case 0x80E00: m_musicReply = static_cast<uint32_t>(m_state); break;
                case 0x90000: m_musicReply = streamLength(static_cast<uint32_t>(first)); break;
                case 0x90100: m_musicReply = static_cast<uint32_t>(streamPosition()); break;
                case 0x90200: m_musicReply = preloadSynced(static_cast<int32_t>(index)); break;
                case 0x90300: m_musicReply = startSynced() ? 1u : 0u; break;
                case 0x90400:
                    m_mono = first != 0;
                    setMusicVolume(m_volume);
                    break;
                case 0x90500: m_loop = static_cast<uint32_t>(first); break;
                default: std::fprintf(stderr, "[sound] musicstream: unknown command %u\n", command); break;
                }
            }

            uint32_t musicInit() // 0x1060
            {
                m_musicThread = true;
                m_sd.setTransCallback(1, [this](uint32_t) { musicSignal(); });                  // 0x25B8
                m_sd.setIrqCallback([this](uint32_t) { m_irqFlag = true; musicSignal(); });     // 0x25F0
                for (StreamBuffer &b : m_buffers)
                    b.data.assign(kStreamBuffer + 0x800, 0);
                for (uint32_t i = 0; i < kMusicFiles; ++i)
                    if (!findDiscFile("cdrom0:" + m_module.string(0x6410 + i * 0x28), m_musicFile[i]))
                        std::fprintf(stderr, "[sound] music file %u (%s) not found\n", i, m_module.string(0x6410 + i * 0x28).c_str());
                return kMusicThreadId;
            }

            void musicShutdown() // 0x12D8
            {
                m_musicThread = false;
                for (StreamBuffer &b : m_buffers)
                    b.data.clear();
            }

            bool openStream(int32_t file, int32_t positionMs) // 0x1484
            {
                if (m_state != 0 || file < 0 || positionMs < 0)
                    return false;
                static const bool voiceLog = std::getenv("VCS_SOUND_VOICES") != nullptr; // (ours: with the voices)
                if (voiceLog && file < static_cast<int32_t>(kMusicFiles))
                    std::fprintf(stderr, "[sound-stream] open %d %s at %d ms\n", file, m_module.string(0x6410 + file * 0x28).c_str(), positionMs);
                m_state = 0x80200;
                m_file = file;
                m_startMs = static_cast<uint32_t>(positionMs);
                musicSignal();
                return true;
            }

            int32_t requestStop() // 0x200C
            {
                switch (m_state)
                {
                case 0x1000: case 0x2000: case 0x3000: case 0x4000: case 0x5000: case 0x7000: case 0x8000: case 0x80200:
                    m_stopRequest = true;
                    return 0;
                default: return -1;
                }
            }

            bool stopRequested() // 0x26B4
            {
                if (!m_stopRequest)
                    return false;
                m_stopRequest = false;
                m_state = 0x6000;
                return true;
            }

            uint32_t streamLength(uint32_t file) const // 0x23B8: ms
            {
                return file < kMusicFiles ? (m_musicFile[file].size * 7u) >> 8 : 0xFFFFFFFFu;
            }

            // 0x1394: stop what plays, preload `file` from its start and hold it until 0x90300 starts it (cutscenes).
            // On the IOP the call blocks until the stop has happened and the preload is done; here both are done in
            // the call.
            uint32_t preloadSynced(int32_t file)
            {
                if (m_syncStart)
                    return 1;
                requestStop();
                if (m_state != 0)
                {
                    musicSignal(); // the stop is seen at the music thread's next step (an IRQ or transfer on the IOP)
                    pump();
                }
                m_syncStart = true;
                m_loop = 0;
                openStream(file, 0);
                pump();
                return 1;
            }

            bool startSynced() // 0x143C
            {
                if (!m_syncStart)
                    return false;
                if (m_waitingStart)
                {
                    m_waitingStart = false;
                    finishStart();
                }
                return true;
            }

            void setMusicVolume(uint32_t volume) // 0x2208: left voice left only, right voice right only (or mono)
            {
                m_volume = volume;
                const uint32_t l = (m_voice[0] & 0x7FFF) << 1 | 1, r = (m_voice[1] & 0x7FFF) << 1 | 1;
                if (!m_mono)
                {
                    m_sd.setParam(l, static_cast<uint16_t>(volume >> 16));
                    m_sd.setParam(l | 0x100, 0);
                    m_sd.setParam(r, 0);
                    m_sd.setParam(r | 0x100, static_cast<uint16_t>(volume));
                }
                else
                {
                    const uint16_t m = static_cast<uint16_t>(((volume >> 16) + (volume & 0xFFFF)) / 3);
                    m_sd.setParam(l, m);
                    m_sd.setParam(l | 0x100, m);
                    m_sd.setParam(r, m);
                    m_sd.setParam(r | 0x100, m);
                }
                m_sd.setParam(0x981, 0x3FFF);
                m_sd.setParam(0xA81, 0x3FFF);
            }

            void muteMusicVoices() // 0x2630
            {
                const uint32_t l = (m_voice[0] & 0x7FFF) << 1 | 1, r = (m_voice[1] & 0x7FFF) << 1 | 1;
                m_sd.setParam(l, 0);
                m_sd.setParam(l | 0x100, 0);
                m_sd.setParam(r, 0);
                m_sd.setParam(r | 0x100, 0);
            }

            void setupVoice(uint32_t which, uint32_t voice) // 0x20C8: voice 0x2E / 0x2F = core 1 voice 22 / 23
            {
                const uint32_t core = voice > 0x17 ? 1u : 0u;
                const uint32_t index = voice - core * 0x18;
                m_voice[which] = index;
                const uint32_t e = core | (index << 1);
                m_sd.setParam(e, 0);
                m_sd.setParam(e | 0x100, 0);
                m_sd.setParam(e | 0x200, static_cast<uint16_t>((m_rate << 12) / 48000));
                m_sd.setParam(e | 0x300, 0xF);
                m_sd.setParam(e | 0x400, 0x1FC0);
                if (voice == 0x2E)
                    m_sd.setAddr((index << 1) | 0x2041, m_spuL);
                else if (voice == 0x2F)
                    m_sd.setAddr((index << 1) | 0x2041, m_spuR);
            }

            // 0x1524: where `m_startMs` lies in the file - piecewise over eighths of its length, 16 KB aligned
            void seekStart()
            {
                const uint32_t size = m_musicFile[m_file].size;
                m_fileSize = size;
                const uint32_t lengthMs = (size * 7u) >> 8;
                if (static_cast<int64_t>(static_cast<int32_t>(lengthMs)) <= static_cast<int64_t>(static_cast<int32_t>(m_startMs)))
                    m_startMs = 0;
                int64_t offset = 0;
                if (static_cast<int32_t>(m_startMs) > 0)
                {
                    const int64_t q4 = size >> 2, q8 = size >> 3, q16 = size >> 4;
                    const int64_t sec = static_cast<int32_t>(m_startMs) / 1000;
                    const int64_t len = static_cast<int64_t>(static_cast<int32_t>(lengthMs)) / 1000;
                    const int64_t l2 = len >> 1, l4 = len >> 2, l8 = len >> 3;
                    if (l8 > 0)
                    {
                        auto part = [&](int64_t from) { return q16 * (sec - from) / l8; };
                        if (l2 < sec)
                        {
                            if (l2 + l4 < sec)
                                offset = sec <= l2 + l4 + l8 ? q4 + q8 + part(l2 + l4) : q4 + q8 + q16 + part(l2 + l4 + l8);
                            else
                                offset = l2 + l8 < sec ? q4 + q16 + part(l2 + l8) : q4 + part(l2);
                        }
                        else if (l4 < sec)
                            offset = l4 + l8 < sec ? q8 + q16 + part(l4 + l8) : q8 + part(l4);
                        else
                            offset = sec <= l8 ? q16 * sec / l8 : q16 + part(l8);
                    }
                }
                m_readPos = static_cast<uint32_t>(offset) << 1;
                m_readPos = size - 0x8000u < m_readPos ? 0u : (m_readPos & 0xFFFFC000u);
                m_startMs = 0;
                for (StreamBuffer &b : m_buffers)
                    b.consumed = b.filled = 0;
                m_rate = 32000;
                m_spuL = m_musicSpu[0];
                m_spuR = m_musicSpu[1];
            }

            bool readStream(uint32_t buffer, uint32_t at, uint32_t lsn, uint32_t sectors) // fn 0x468 on channel 2
            {
                StreamBuffer &b = m_buffers[buffer];
                if (at >= b.data.size())
                    return true;
                readSectors(lsn, sectors, b.data.data() + at, b.data.size() - at);
                return true;
            }

            void fillBuffer(uint32_t i) // 0x1B90: the first fill of a disc buffer
            {
                StreamBuffer &b = m_buffers[i];
                m_bufState[i] = 2;
                uint32_t remain = m_fileSize - m_readPos;
                bool go = false;
                if (m_loop != 0)
                {
                    if (static_cast<int32_t>(remain) > 0x7FF)
                        go = true;
                    else
                    {
                        m_readPos = 0;
                        remain = m_fileSize;
                    }
                }
                if (!go && static_cast<int32_t>(remain) < 0x800)
                {
                    b.filled = 0;
                    m_bufState[i] = 4;
                    return;
                }
                if (static_cast<int32_t>(remain) > 0x40000)
                    remain = 0x40000;
                const uint32_t start = m_readPos + 8;
                m_readPos = start & 0xFFFFFFF0u;
                const uint32_t base = m_musicFile[m_file].lsn;
                readStream(i, 0, base + (start >> 11), static_cast<uint32_t>(static_cast<int32_t>(remain) >> 11));
                b.filled = remain;
                m_readPos += remain;
                if (m_fileSize <= m_readPos && m_loop != 0)
                {
                    m_readPos = 0;
                    if (remain < 0x40000)
                    {
                        readStream(i, remain, base, (0x40000u - remain) >> 11);
                        b.filled = 0x40000;
                        m_readPos = 0x40000u - remain;
                    }
                }
                m_bufState[i] = 3;
            }

            int32_t streamPosition() const // 0x2488: ms played
            {
                if (m_file == -1)
                    return 0;
                int32_t p = static_cast<int32_t>(m_readPos), x = p * 8;
                const int32_t st = m_bufState[m_cur];
                if (st == 1 || st == 3)
                {
                    p = static_cast<int32_t>(m_readPos);
                    if (m_bufState[m_cur == 0 ? 1 : 0] == 3)
                    {
                        p = static_cast<int32_t>(m_readPos) - 0x40000;
                        if (p < 0)
                            p += static_cast<int32_t>(m_fileSize);
                    }
                    if (p < 0x40000)
                        p += static_cast<int32_t>(m_fileSize);
                    p = p + static_cast<int32_t>(m_buffers[m_cur].consumed) - 0x40000;
                    x = p * 8;
                }
                x -= p;
                if (x < 0)
                    x += 7;
                const int32_t perMs = m_rate / 1000;
                return perMs ? (x >> 3) / perMs : 0;
            }

            // ADPCM block flags in a 8 KB chunk: its first block starts the loop (first half) or just repeats, the
            // last block of the 16 KB ring jumps back
            void markChunk(uint32_t at, bool firstHalf, uint32_t lastOffset, uint8_t lastFlag)
            {
                std::vector<uint8_t> &d = m_buffers[m_cur].data;
                if (at + lastOffset < d.size())
                {
                    d[at + 1] = firstHalf ? 6 : 2;
                    d[at + 0x11] = 2;
                    d[at + lastOffset] = lastFlag;
                }
            }

            void transfer(uint32_t at, uint32_t spuAddress) // VoiceTrans(1, ...) of 8 KB from the current buffer
            {
                std::vector<uint8_t> &d = m_buffers[m_cur].data;
                if (at + 0x2000 > d.size())
                    return;
                const int32_t r = m_sd.voiceTrans(1, 0, d.data() + at, static_cast<uint32_t>(d.size() - at), spuAddress, 0x2000);
                if (r < 0)
                    std::fprintf(stderr, "[sound] music sceSdVoiceTrans returned %d\n", r);
            }

            void stopStream() // 0x35B0
            {
                m_sd.setCoreAttr(5, 0);
                m_irqFlag = false;
                m_other = 1;
                muteMusicVoices();
                m_sd.setSwitch(0x1601, (1u << (m_voice[0] & 31)) | (1u << (m_voice[1] & 31)));
                m_file = -1;
                m_bufState[0] = m_bufState[1] = 0;
                m_cur = 0;
                m_stopRequest = false;
                m_startRequest = 0;
                m_state = 0;
            }

            // L362C: preloaded and asked to start - key the two voices on
            void tryStart()
            {
                if (!m_startRequest || m_state != 0x4000)
                    return;
                m_startRequest = 0;
                m_state = 0x5000;
                if (m_syncStart)
                {
                    m_waitingStart = true; // until 0x90300
                    return;
                }
                finishStart();
            }

            void finishStart()
            {
                m_syncStart = false;
                setMusicVolume(m_volume);
                m_sd.setCoreAttr(5, 1); // core 1 IRQ on
                m_sd.setSwitch(0x1501, (1u << (m_voice[0] & 31)) | (1u << (m_voice[1] & 31)));
            }

            // One pass of the music thread's loop (0x26E8) after its semaphore was signalled
            void musicStep()
            {
                if (m_volumePending)
                {
                    m_volumePending = false;
                    setMusicVolume(m_volume);
                }
                if (m_state == 0x5000)
                    refillStep();
                switch (m_state)
                {
                case 0x4000: break;
                case 0x1000: // preload: left voice, first half
                    if (stopRequested())
                        return stopStream();
                    m_phase = 1;
                    m_half = 0;
                    markChunk(0, true, 0x5FF1, 3);
                    m_state = 0x7000;
                    m_sd.voiceTransStatus(1, 1);
                    transfer(0, m_spuL);
                    break;
                case 0x7000: // left, second half
                    if (stopRequested())
                        return stopStream();
                    m_state = 0x2000;
                    m_sd.voiceTransStatus(1, 1);
                    transfer(0x4000, m_spuL + 0x2000);
                    break;
                case 0x2000: // right, first half
                {
                    if (stopRequested())
                        return stopStream();
                    StreamBuffer &b = m_buffers[m_cur];
                    b.consumed += 0x2000;
                    m_phase = 2;
                    markChunk(b.consumed, true, 0x5FF1, 3);
                    m_state = 0x8000;
                    m_sd.voiceTransStatus(1, 1);
                    transfer(b.consumed, m_spuR);
                    break;
                }
                case 0x8000: // right, second half
                {
                    if (stopRequested())
                        return stopStream();
                    m_state = 0x3000;
                    m_sd.voiceTransStatus(1, 1);
                    StreamBuffer &b = m_buffers[m_cur];
                    transfer(b.consumed + 0x4000, m_spuR + 0x2000);
                    b.consumed += 0x6000;
                    break;
                }
                case 0x3000: // preloaded
                    if (stopRequested())
                        return stopStream();
                    m_phase = 0;
                    m_sd.setAddr(0x1F01, m_spuL + 0x2000); // core 1 IRQ when the second half starts playing
                    m_state = 0x4000;
                    if (m_startCount > 0)
                    {
                        m_startCount = 0;
                        ++m_startRequest;
                    }
                    break;
                case 0x80200: // open
                    if (stopRequested())
                        return stopStream();
                    seekStart();
                    setupVoice(0, 0x2E);
                    setupVoice(1, 0x2F);
                    for (int tries = 0;; ++tries) // on the IOP: until the fill starts or a stop is asked for
                    {
                        if (beginFill() == 0)
                            break;
                        if (stopRequested() || tries > 1000)
                            return stopStream();
                    }
                    break;
                case 0x5000:
                    if (stopRequested())
                        return stopStream();
                    playStep();
                    break;
                case 0x6000: return stopStream();
                default: break;
                }
                tryStart();
            }

            int32_t beginFill() // 0x1FC0 / 0x1F30
            {
                ++m_startCount;
                m_phase = 0;
                if (m_state != 0x80200)
                {
                    m_startCount = 0;
                    return -1;
                }
                setMusicVolume(m_volume);
                fillBuffer(0);
                fillBuffer(1);
                m_state = 0x1000;
                musicSignal();
                return 0;
            }

            // The disc side while playing: refill the idle disc buffer (state 5 -> 2 -> 3)
            void refillStep()
            {
                StreamBuffer &cur = m_buffers[m_cur];
                if (m_bufState[m_cur] == 4 && m_fileSize <= m_readPos && cur.filled <= cur.consumed)
                    m_state = 0x6000;
                const int32_t st = m_bufState[m_other];
                const uint32_t base = m_musicFile[m_file >= 0 ? m_file : 0].lsn;
                if (st == 5)
                {
                    bool ok;
                    if (static_cast<int32_t>(m_wrapBytes) < 1)
                        ok = readStream(m_other, 0, base + (m_readPos >> 11), static_cast<uint32_t>(static_cast<int32_t>(m_refillBytes) >> 11));
                    else
                        ok = readStream(m_other, m_refillBytes, base, static_cast<uint32_t>(static_cast<int32_t>(m_wrapBytes) >> 11));
                    if (ok)
                        m_bufState[m_other] = 2;
                }
                else if (st == 2) // the read is done (fn 0x63C: 0)
                {
                    StreamBuffer &b = m_buffers[m_other];
                    b.consumed = 0;
                    if (static_cast<int32_t>(m_wrapBytes) < 1)
                    {
                        m_readPos += m_refillBytes;
                        b.filled = m_refillBytes;
                    }
                    else
                    {
                        m_readPos = m_wrapBytes;
                        b.filled += m_wrapBytes;
                    }
                    m_wrapBytes = 0;
                    const bool atEnd = m_fileSize <= m_readPos;
                    m_bufState[m_other] = 3;
                    if (atEnd && m_loop != 0)
                    {
                        m_readPos = 0;
                        m_wrapBytes = 0x40000u - m_refillBytes;
                        if (static_cast<int32_t>(m_wrapBytes) < 0x40000)
                            m_bufState[m_other] = 5;
                    }
                }
            }

            // The SPU2 side while playing: on each IRQ refill one 8 KB half of both rings (left, then right on its
            // completion), then move the IRQ to the half just filled
            void playStep()
            {
                if (m_bufState[m_cur] == 3)
                    m_bufState[m_cur] = 1;
                if (m_bufState[m_cur] != 1)
                    return;
                StreamBuffer &b = m_buffers[m_cur];
                const uint32_t lastOffset = 0x1FF1;
                if (m_phase == 1)
                {
                    markChunk(b.consumed, m_half == 0, lastOffset, m_half == 0 ? 2 : 3);
                    m_phase = 2;
                    m_sd.voiceTransStatus(1, 1);
                    transfer(b.consumed, m_spuR + m_half * 0x2000);
                    const bool irq = m_irqFlag;
                    b.consumed += 0x2000;
                    if (irq)
                    {
                        m_irqFlag = false;
                        m_sd.setCoreAttr(5, 1);
                    }
                }
                else if (m_phase == 0)
                {
                    m_sd.setCoreAttr(5, 0);
                    markChunk(b.consumed, m_half == 0, lastOffset, m_half == 0 ? 2 : 3);
                    m_phase = 1;
                    m_sd.voiceTransStatus(1, 1);
                    transfer(b.consumed, m_spuL + m_half * 0x2000);
                    b.consumed += 0x2000;
                }
                else if (m_phase == 2)
                {
                    m_phase = 0;
                    m_sd.setAddr(0x1F01, m_spuL + m_half * 0x2000);
                    const uint32_t old = m_cur;
                    m_half = m_half == 0 ? 1 : 0;
                    if (b.filled <= b.consumed)
                    {
                        if (m_bufState[m_other] == 5 || m_bufState[m_other] == 2)
                        {
                            std::fprintf(stderr, "[sound] Previous load hasnt finished\n");
                            b.consumed = 0;
                        }
                        else
                        {
                            m_bufState[m_cur] = 4;
                            m_cur = m_cur == 0 ? 1 : 0;
                            m_other = old;
                            if (m_loop != 0 && m_fileSize <= m_readPos)
                                m_readPos = 0;
                            if (m_readPos < m_fileSize)
                            {
                                m_bufState[old] = 5;
                                m_refillBytes = std::min<uint32_t>(m_fileSize - m_readPos, 0x40000);
                            }
                        }
                    }
                    m_sd.setCoreAttr(5, 1);
                }
            }

            std::mutex m_mutex;
            Spu2 m_spu;
            sound::LibSd m_sd{m_spu};
            Module m_module;
            uint64_t m_eeCycles = 0, m_iopCycle = 0;

            // sound effects (module data in the comments)
            bool m_sfxReady = false;                     // 0x7FF0
            int32_t m_sfxVolume = 0x7F;                  // 0x7FF8
            uint32_t m_sfxReply = 0;                     // 0x8160
            std::vector<uint8_t> m_bankBuffer;           // 0x7FFC
            std::vector<uint32_t> m_soundTable;          // 0x81A4: offset in the .RAW, bytes, rate - per sound
            std::array<uint32_t, kBanks> m_bankSpu{};    // 0x8000: SPU2 address of each bank
            std::array<uint8_t, kBanks> m_bankLoaded{};  // 0xA0D0
            std::array<int32_t, kBanks> m_bankMaxVolume{}; // 0x8904
            std::array<DiscFile, kBanks> m_sdtFile{}, m_rawFile{}; // 0xAAD0, 0xA140
            uint32_t m_specialLsn = 0;                   // 0x81A0
            int32_t m_loading = -1, m_loadingBank = -1;  // 0x8110, 0x8114
            int32_t m_slotCursor = 0;                    // 0x8118
            int32_t m_specialSound = -1, m_specialLoad = -1; // 0x811C, 0x8120
            std::array<int32_t, 5> m_typedSound{};       // 0x8124
            int32_t m_specialType = 0xFF;                // 0x8138
            int32_t m_currentArea = -1;                  // 0x813C
            int32_t m_typed5 = -1, m_typed6 = -1, m_typed7 = -1; // 0x8140..0x8148
            int32_t m_buildingSel = -1;                  // 0x814C
            std::array<int32_t, 2> m_buildingSlot{0x45, 0x45}; // 0x8A14
            std::array<uint32_t, 5> m_slotAddr{};        // 0x9FA0
            std::array<int32_t, 5> m_slotSound{};        // 0xA120
            std::array<uint8_t, kBlockBytes> m_block{};  // 0x8360: the last voice block
            // (ours) each voice's sound and bank since its last key-on: the second output's choice (setSecondOutput)
            std::array<int32_t, kVoices> m_voiceSound = [] { std::array<int32_t, kVoices> a; a.fill(-1); return a; }();
            std::array<int32_t, kVoices> m_voiceBank = m_voiceSound;
            VoicePick m_pick = nullptr;

            // music
            struct StreamBuffer
            {
                std::vector<uint8_t> data; // 0x8300 + i * 0xC
                uint32_t consumed = 0;     // 0x82F8 + i * 0xC: sent to the SPU2
                uint32_t filled = 0;       // 0x82FC + i * 0xC
            };
            bool m_musicThread = false;
            bool m_musicSignal = false, m_waitingStart = false;
            uint32_t m_musicReply = 0;                   // 0x7FD0
            int32_t m_state = 0;                         // 0x7F88
            int32_t m_file = -1;                         // 0x7F98
            uint32_t m_cur = 0, m_other = 1;             // 0x7F9C, the thread's "other"
            int32_t m_bufState[2] = {0, 0};              // 0x7FA0: 1 playing, 2 reading, 3 full, 4 drained, 5 to read
            bool m_irqFlag = false;                      // 0x7FA8
            int32_t m_phase = 0;                         // 0x7F90
            int32_t m_startCount = 0, m_startRequest = 0; // 0x7F6C, 0x7F70
            bool m_stopRequest = false, m_syncStart = false, m_mono = false, m_volumePending = false; // 0x7F74 78 7C 68
            uint32_t m_volume = 0, m_loop = 0;           // 0x8298, 0x82F4
            uint32_t m_fileSize = 0, m_readPos = 0, m_startMs = 0; // 0x82EC, 0x82F0, 0x8310
            uint32_t m_half = 0, m_refillBytes = 0, m_wrapBytes = 0; // the thread's locals
            StreamBuffer m_buffers[2];
            uint32_t m_spuL = 0, m_spuR = 0;             // 0x8314, 0x8318
            uint32_t m_musicSpu[2] = {0, 0};             // 0x7F48, 0x7F4C
            uint32_t m_voice[2] = {0x16, 0x17};          // 0x7F58
            int32_t m_rate = 32000;                      // 0x7F94
            std::array<DiscFile, kMusicFiles> m_musicFile{}; // 0x8FE0
        };

        Engine *g_engine = nullptr;

        void clockTick(uint64_t eeCycles)
        {
            g_engine->clock(eeCycles);
        }

        // VCS_SOUND_REPLAY=<record>: feed RPCs recorded from a run (PS2X_RPC_RECORD, runtime/ps2_iop_native.h) to the
        // native driver at their recorded IOP times and write its SPU2 output to VCS_SOUND_REPLAY_OUT (a WAV), then
        // exit - the native driver checked against the emulated IOP's run on identical input (tools/soundcheck.py).
        uint32_t g_replayWaits = 0, g_replayWaitMax = 0; // opens that waited for a native stop to finish (ms)

        void replay(const char *recordPath)
        {
            g_stopAtIrq = true; // the IOP's timing: the recording was made against it
            std::FILE *in = std::fopen(recordPath, "rb");
            const char *outPath = std::getenv("VCS_SOUND_REPLAY_OUT");
            std::FILE *out = outPath ? std::fopen(outPath, "wb") : nullptr;
            static std::FILE *s_out = out;
            static uint32_t s_frames = 0;
            auto header = [](std::FILE *f, uint32_t frames)
            {
                const uint32_t bytes = frames * 4u;
                const uint32_t h[11] = {0x46464952u, 36u + bytes, 0x45564157u, 0x20746D66u, 16u, 0x00020001u, 48000u, 192000u,
                                        0x00100004u, 0x61746164u, bytes};
                std::fseek(f, 0, SEEK_SET);
                std::fwrite(h, 4, 11, f);
                std::fseek(f, 0, SEEK_END);
            };
            if (!in || !out)
            {
                std::fprintf(stderr, "[sound] replay: cannot open %s or VCS_SOUND_REPLAY_OUT\n", recordPath);
                std::_Exit(1);
            }
            header(out, 0);
            Engine engine;
            if (!engine.start())
                std::_Exit(1);
            engine.setSink([](const int16_t *stereo, uint32_t frames)
                           {
                               std::fwrite(stereo, 4, frames, s_out);
                               s_frames += frames;
                           });
            std::vector<uint8_t> ram(0x400000);
            uint64_t now = 0, time = 0;
            uint32_t h[4], calls = 0, expected = 0;
            std::map<uint32_t, std::pair<uint32_t, uint32_t>> differ; // command group -> (calls, replies that differ)
            std::map<uint32_t, std::string> examples;
            while (std::fread(&time, 8, 1, in) == 1 && std::fread(h, 4, 4, in) == 4)
            {
                if (h[2] > 0x100000u || (h[2] && std::fread(ram.data() + 0x100000, 1, h[2], in) != h[2]) ||
                    std::fread(&expected, 4, 1, in) != 1)
                    break;
                if (time > now)
                    engine.clock(time - now);
                now = std::max(now, time);
                ps2x::iop::RpcRequest request{};
                request.sid = h[0];
                request.function = h[1];
                request.send = {0x100000u, h[2]};
                request.receive = {0x200000u, std::min<uint32_t>(h[3], 0x100000u)};
                if (request.sid == kSidMusic && (request.function & 0xFFFF00u) == 0x80200u && expected == 1u)
                {
                    // An open the IOP took while the native engine is still finishing a stop: the game only opens once
                    // its state polls (0x80E00) read 0 - live it would poll a little longer. Let native time run as
                    // those polls would (up to 1 s), so the replay stays in step instead of refusing the open.
                    ps2x::iop::RpcRequest poll{};
                    poll.sid = kSidMusic;
                    poll.function = 0x80E00u;
                    poll.receive = {0x200000u, 4u};
                    for (uint32_t waited = 0;; waited += 1)
                    {
                        engine.call(ram.data(), poll);
                        uint32_t state;
                        std::memcpy(&state, ram.data() + 0x200000, 4);
                        if (state == 0u || waited >= 1000u)
                        {
                            if (waited)
                            {
                                ++g_replayWaits;
                                g_replayWaitMax = std::max(g_replayWaitMax, waited);
                            }
                            break;
                        }
                        engine.clock(294912u); // 1 ms of IOP time (EE cycles)
                        now += 294912u;
                    }
                }
                if (request.sid == kSidSfx || request.sid == kSidMusic)
                {
                    engine.call(ram.data(), request);
                    ++calls;
                    if (request.receive.size >= 4)
                    {
                        uint32_t reply;
                        std::memcpy(&reply, ram.data() + 0x200000, 4);
                        const uint32_t group = (request.sid & 0xF) << 24 | (request.function & 0xFFFF00);
                        auto &d = differ[group];
                        ++d.first;
                        if (reply != expected)
                        {
                            if (!d.second++)
                            {
                                char text[96];
                                std::snprintf(text, sizeof(text), "fn 0x%x at sample %llu: 0x%x, the IOP 0x%x", request.function,
                                              static_cast<unsigned long long>(time / 8u / Spu2::kCyclesPerSample), reply, expected);
                                examples[group] = text;
                            }
                        }
                    }
                }
            }
            for (const auto &[group, d] : differ)
                if (d.second)
                    std::fprintf(stderr, "[sound] replay: replies differ: server %x commands 0x%05x: %u of %u (first: %s)\n",
                                 0x65686570 | (group >> 24), group & 0xFFFFFF, d.second, d.first, examples[group].c_str());
            if (g_replayWaits)
                std::fprintf(stderr, "[sound] replay: %u stream opens waited for a stop to finish natively (at most %u ms), as the game's polls would\n",
                             g_replayWaits, g_replayWaitMax);
            std::fprintf(stderr, "[sound] replay: %u replies compared\n", [&] { uint32_t n = 0; for (auto &e : differ) n += e.second.first; return n; }());
            engine.clock(294912000ull); // one more second
            header(out, s_frames);
            std::fclose(out);
            std::fprintf(stderr, "[sound] replay: %u calls, %.1f s of sound\n", calls, s_frames / 48000.0);
            std::fflush(stderr);
            std::_Exit(0);
        }
    }

    void setSecondOutput(VoicePick pick)
    {
        if (g_engine)
            g_engine->setPick(pick);
    }

    bool installSoundHooks(PS2Runtime &)
    {
        if (const char *record = std::getenv("VCS_SOUND_REPLAY"))
            replay(record);
        const char *v = std::getenv("VCS_NATIVE_SOUND");
        if (v && *v == '0')
        {
            std::fprintf(stderr, "[sound] through the IOP (VCS_NATIVE_SOUND=0)\n");
            return false;
        }
        g_engine = new Engine(); // lives until exit: the clock and RPCs may run while the runtime shuts down
        if (!g_engine->start())
            return false;
        ps2_iop_native::registerServer(kSidSfx, [](uint8_t *rdram, const ps2x::iop::RpcRequest &r) { g_engine->call(rdram, r); });
        ps2_iop_native::registerServer(kSidMusic, [](uint8_t *rdram, const ps2x::iop::RpcRequest &r) { g_engine->call(rdram, r); });
        ps2_iop_native::setEmulatedAudioMuted(true);
        ps2_iop_native::setClock(&clockTick);
        std::fprintf(stderr, "[sound] native sound driver (CDSTREAM's servers on the host)\n");
        return true;
    }
}
