// [gta-recomp] SPU2 emulation, see iop_spu2.h. Register map, ADPCM, ADSR and AutoDMA layout follow the public
// PS1 SPU / PS2 SPU2 documentation (psx-spx, PCSX2's SPU2 notes).
#include "iop_spu2.h"

#include <algorithm>
#include <string>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr int32_t kFilterPos[5] = {0, 60, 115, 98, 122};
        constexpr int32_t kFilterNeg[5] = {0, 0, -52, -55, -60};
        constexpr uint32_t kAddrMask = IopSpu2::kRamHalfwords - 1u;

        int16_t clamp16(int32_t v)
        {
            return static_cast<int16_t>(std::clamp(v, -32768, 32767));
        }

        uint32_t setHigh(uint32_t addr, uint16_t value) { return ((static_cast<uint32_t>(value) & 0xFu) << 16) | (addr & 0xFFFFu); }
        uint32_t setLow(uint32_t addr, uint16_t value) { return (addr & 0xF0000u) | value; }

        // 4-point Gaussian interpolation table (psx-spx "4-Point Gaussian Interpolation"; SPU2 uses the same one)
        constexpr int16_t kGauss[512] = {
#include "iop_spu2_gauss.inc"
        };

        // 39-tap half-band FIR the reverb unit resamples its input / output with (psx-spx "Reverb Buffer Resampling")
        constexpr int32_t kReverbFir[39] = {
            -0x0001, 0, 0x0002, 0, -0x000A, 0, 0x0023, 0, -0x0067, 0, 0x010A, 0, -0x0268, 0, 0x0534, 0, -0x0B90, 0, 0x2806, 0x4000,
            0x2806, 0, -0x0B90, 0, 0x0534, 0, -0x0268, 0, 0x010A, 0, -0x0067, 0, 0x0023, 0, -0x000A, 0, 0x0002, 0, -0x0001,
        };

        int32_t fir39(const int16_t *window) // window[0..38], oldest first
        {
            int32_t sum = 0;
            for (uint32_t i = 0; i < 39; ++i)
                sum += kReverbFir[i] * window[i];
            return sum;
        }

        int32_t mul15(int32_t a, int32_t b) { return (a * b) >> 15; }

        // One output-rate tick of an envelope (ADSR phase or volume sweep), psx-spx "Envelope Operation depending on
        // Shift/Step/Mode/Direction". stepValue 0..3 means +7..+4 (or -8..-5 when decreasing).
        void envelopeTick(int32_t &level, uint32_t &counter, int32_t shift, int32_t stepValue, bool exponential, bool decrease,
                          bool negative = false)
        {
            int32_t step = 7 - stepValue;
            if (decrease != negative)
                step = ~step;
            step *= 1 << std::max(0, 11 - shift);
            uint32_t increment = 0x8000u >> std::max(0, shift - 11);
            if (exponential && !decrease && level > 0x6000)
            {
                if (shift < 10)
                    step >>= 2;
                else if (shift >= 11)
                    increment >>= 2;
                else
                {
                    step >>= 1;
                    increment >>= 1;
                }
            }
            else if (exponential && decrease)
                step = (step * level) >> 15;
            if ((stepValue | (shift << 2)) != 0x7F)
                increment = std::max(increment, 1u);
            counter += increment;
            if ((counter & 0x8000u) == 0u)
                return;
            counter = 0;
            level += step;
            if (!decrease)
                level = std::clamp(level, -0x8000, 0x7FFF);
            else if (negative)
                level = std::clamp(level, -0x8000, 0);
            else
                level = std::max(level, 0);
        }
    }

    IopSpu2::IopSpu2()
    {
        static std::atomic<uint32_t> instances{0};
        m_instance = instances++;
        if (const char *path = std::getenv("PS2X_SPU2_TRACE"))
            m_trace = std::fopen((std::string(path) + "." + std::to_string(m_instance)).c_str(), "w");
        reset();
    }

    void IopSpu2::reset()
    {
        m_ram.assign(kRamHalfwords, 0u);
        m_cores = {};
        for (Core &c : m_cores)
            c.statx = 0x80;
        m_irqInfo = 0;
        std::memset(m_spdif, 0, sizeof(m_spdif));
        m_irqPending = false;
        m_cycle = 0;
        m_nextSampleCycle = kCyclesPerSample;
        m_outBuffer.clear();
    }

    int16_t IopSpu2::fixedVolume(uint16_t reg, Sweep &current)
    {
        if ((reg & 0x8000u) == 0u)
        {
            current.level = static_cast<int16_t>(reg << 1);
            return current.level;
        }
        // sweep mode: bit 14 exponential, 13 decrease, 12 negative phase, 2-6 shift, 0-1 step
        int32_t level = current.level;
        envelopeTick(level, current.counter, (reg >> 2) & 0x1F, reg & 3, (reg & 0x4000u) != 0u, (reg & 0x2000u) != 0u,
                     (reg & 0x1000u) != 0u);
        current.level = static_cast<int16_t>(level);
        return current.level;
    }

    void IopSpu2::checkIrq(uint32_t lo, uint32_t hi)
    {
        for (uint32_t i = 0; i < 2; ++i)
        {
            const Core &c = m_cores[i];
            if ((c.attr & 0x40u) != 0u && c.irqa >= lo && c.irqa < hi)
            {
                m_irqInfo |= static_cast<uint16_t>(4u << i);
                m_irqPending = true;
                ++m_stats.irqs;
            }
        }
    }

    // ---- registers ------------------------------------------------------------------------------------------------
    uint16_t IopSpu2::read16(uint32_t offset)
    {
        offset &= kRegSize - 1u;
        static const bool trace = std::getenv("PS2X_TRACE_SPU") != nullptr;
        static int traced = 0;
        if (trace && traced++ < 40000)
            std::fprintf(stderr, "[spu2] r 0x%03x\n", offset);
        if (offset >= 0x760u && offset < 0x7B0u)
        {
            const uint32_t core = (offset - 0x760u) / 0x28u;
            const uint32_t reg = (offset - 0x760u) % 0x28u;
            const Core &c = m_cores[core];
            switch (reg)
            {
            case 0x00: return c.mvolL;
            case 0x02: return c.mvolR;
            case 0x04: return c.evolL;
            case 0x06: return c.evolR;
            case 0x08: return c.avolL;
            case 0x0A: return c.avolR;
            case 0x0C: return c.bvolL;
            case 0x0E: return c.bvolR;
            case 0x10: return static_cast<uint16_t>(c.curMvolL.level);
            case 0x12: return static_cast<uint16_t>(c.curMvolR.level);
            default: return c.raw[(0x160u + reg) / 2u];
            }
        }
        if (offset >= 0x7C0u)
        {
            if (offset == 0x7C2u)
                return m_irqInfo;
            return m_spdif[((offset - 0x7C0u) / 2u) & 7u];
        }
        return readCore(offset >> 10, offset & 0x3FFu);
    }

    void IopSpu2::write16(uint32_t offset, uint16_t value)
    {
        offset &= kRegSize - 1u;
        if (m_trace)
            std::fprintf(m_trace, "%llu W %03x %04x\n", static_cast<unsigned long long>(m_nextSampleCycle / kCyclesPerSample), offset, value);
        static const bool trace = std::getenv("PS2X_TRACE_SPU") != nullptr;
        static int traced = 0;
        if (trace && traced++ < 40000)
            std::fprintf(stderr, "[spu2] w 0x%03x = 0x%04x\n", offset, value);
        if (offset >= 0x760u && offset < 0x7B0u)
        {
            const uint32_t core = (offset - 0x760u) / 0x28u;
            const uint32_t reg = (offset - 0x760u) % 0x28u;
            Core &c = m_cores[core];
            switch (reg)
            {
            case 0x00: c.mvolL = value; break;
            case 0x02: c.mvolR = value; break;
            case 0x04: c.evolL = value; break;
            case 0x06: c.evolR = value; break;
            case 0x08: c.avolL = value; break;
            case 0x0A: c.avolR = value; break;
            case 0x0C: c.bvolL = value; break;
            case 0x0E: c.bvolR = value; break;
            default:
                c.raw[(0x160u + reg) / 2u] = value;
                if (reg >= 0x14u && reg <= 0x26u) // 0x774..0x786: reverb volumes
                    c.revbVol[(reg - 0x14u) / 2u] = static_cast<int16_t>(value);
                break;
            }
            return;
        }
        if (offset >= 0x7C0u)
        {
            if (offset == 0x7C2u)
                m_irqInfo = value;
            else
                m_spdif[((offset - 0x7C0u) / 2u) & 7u] = value;
            return;
        }
        writeCore(offset >> 10, offset & 0x3FFu, value);
    }

    uint16_t IopSpu2::readCore(uint32_t core, uint32_t reg)
    {
        Core &c = m_cores[core];
        if (reg < 0x180u)
        {
            const Voice &v = c.voices[reg >> 4];
            switch ((reg & 0xFu) >> 1)
            {
            case 0: return v.volL;
            case 1: return v.volR;
            case 2: return v.pitch;
            case 3: return v.adsr1;
            case 4: return v.adsr2;
            case 5: return static_cast<uint16_t>(v.level);
            case 6: return static_cast<uint16_t>(v.curVolL.level);
            default: return static_cast<uint16_t>(v.curVolR.level);
            }
        }
        if (reg >= 0x1C0u && reg < 0x1C0u + 24u * 0xCu)
        {
            const Voice &v = c.voices[(reg - 0x1C0u) / 0xCu];
            switch (((reg - 0x1C0u) % 0xCu) >> 1)
            {
            case 0: return static_cast<uint16_t>(v.ssa >> 16);
            case 1: return static_cast<uint16_t>(v.ssa);
            case 2: return static_cast<uint16_t>(v.lsa >> 16);
            case 3: return static_cast<uint16_t>(v.lsa);
            case 4: return static_cast<uint16_t>(v.nax >> 16);
            default: return static_cast<uint16_t>(v.nax);
            }
        }
        switch (reg)
        {
        case 0x180: return static_cast<uint16_t>(c.pmon);
        case 0x182: return static_cast<uint16_t>(c.pmon >> 16);
        case 0x184: return static_cast<uint16_t>(c.non);
        case 0x186: return static_cast<uint16_t>(c.non >> 16);
        case 0x188: return static_cast<uint16_t>(c.vmixL);
        case 0x18A: return static_cast<uint16_t>(c.vmixL >> 16);
        case 0x18C: return static_cast<uint16_t>(c.vmixEL);
        case 0x18E: return static_cast<uint16_t>(c.vmixEL >> 16);
        case 0x190: return static_cast<uint16_t>(c.vmixR);
        case 0x192: return static_cast<uint16_t>(c.vmixR >> 16);
        case 0x194: return static_cast<uint16_t>(c.vmixER);
        case 0x196: return static_cast<uint16_t>(c.vmixER >> 16);
        case 0x198: return c.mmix;
        case 0x19A: return c.attr;
        case 0x19C: return static_cast<uint16_t>(c.irqa >> 16);
        case 0x19E: return static_cast<uint16_t>(c.irqa);
        case 0x1A0: return static_cast<uint16_t>(c.konLatch);
        case 0x1A2: return static_cast<uint16_t>(c.konLatch >> 16);
        case 0x1A4: return static_cast<uint16_t>(c.kofLatch);
        case 0x1A6: return static_cast<uint16_t>(c.kofLatch >> 16);
        case 0x1A8: return static_cast<uint16_t>(c.tsa >> 16);
        case 0x1AA: return static_cast<uint16_t>(c.tsa);
        case 0x1AC:
        {
            const uint16_t v = m_ram[c.tsa];
            c.tsa = (c.tsa + 1u) & kAddrMask;
            return v;
        }
        case 0x1B0: return c.admas;
        case 0x340: return static_cast<uint16_t>(c.endx);
        case 0x342: return static_cast<uint16_t>(c.endx >> 16);
        case 0x344:
            // bit 7: transfer ready, only while ATTR selects a transfer mode (bits 4-5); otherwise idle (0).
            // sceSdInit waits for (STATX & 0x7FF) == 0 after resetting a core; transfers wait for bit 7.
            return static_cast<uint16_t>((c.attr & 0x30u) != 0u ? 0x80u : 0u);
        default: return c.raw[(reg >> 1) & 0x1FFu];
        }
    }

    void IopSpu2::writeCore(uint32_t core, uint32_t reg, uint16_t value)
    {
        Core &c = m_cores[core];
        c.raw[(reg >> 1) & 0x1FFu] = value;
        if (reg < 0x180u)
        {
            Voice &v = c.voices[reg >> 4];
            switch ((reg & 0xFu) >> 1)
            {
            case 0: v.volL = value; break;
            case 1: v.volR = value; break;
            case 2: v.pitch = value; break;
            case 3: v.adsr1 = value; break;
            case 4: v.adsr2 = value; break;
            case 5: v.level = static_cast<int16_t>(value); break;
            default: break;
            }
            return;
        }
        if (reg >= 0x1C0u && reg < 0x1C0u + 24u * 0xCu)
        {
            Voice &v = c.voices[(reg - 0x1C0u) / 0xCu];
            switch (((reg - 0x1C0u) % 0xCu) >> 1)
            {
            case 0: v.ssa = setHigh(v.ssa, value); break;
            case 1: v.ssa = setLow(v.ssa, value); break;
            case 2: v.lsa = setHigh(v.lsa, value); v.lsaSetBySoftware = true; break;
            case 3: v.lsa = setLow(v.lsa, value); v.lsaSetBySoftware = true; break;
            case 4: v.nax = setHigh(v.nax, value); break;
            default: v.nax = setLow(v.nax, value); break;
            }
            return;
        }
        if (reg >= 0x2E0u && reg < 0x33Cu) // ESA, then the reverb address registers: hi / lo pairs
        {
            const uint32_t index = (reg - 0x2E0u) / 4u;
            uint32_t &addr = index == 0u ? c.esa : c.revbAddr[index - 1u];
            addr = (reg & 2u) != 0u ? setLow(addr, value) : setHigh(addr, value);
            if (index == 0u)
                c.revbPos = 0; // writing ESA restarts the buffer
            return;
        }
        switch (reg)
        {
        case 0x33C: c.eea = ((static_cast<uint32_t>(value) & 0xFu) << 16) | 0xFFFFu; break; // EEA: upper bits only
        case 0x180: c.pmon = (c.pmon & 0xFF0000u) | value; break;
        case 0x182: c.pmon = (c.pmon & 0xFFFFu) | ((value & 0xFFu) << 16); break;
        case 0x184: c.non = (c.non & 0xFF0000u) | value; break;
        case 0x186: c.non = (c.non & 0xFFFFu) | ((value & 0xFFu) << 16); break;
        case 0x188: c.vmixL = (c.vmixL & 0xFF0000u) | value; break;
        case 0x18A: c.vmixL = (c.vmixL & 0xFFFFu) | ((value & 0xFFu) << 16); break;
        case 0x18C: c.vmixEL = (c.vmixEL & 0xFF0000u) | value; break;
        case 0x18E: c.vmixEL = (c.vmixEL & 0xFFFFu) | ((value & 0xFFu) << 16); break;
        case 0x190: c.vmixR = (c.vmixR & 0xFF0000u) | value; break;
        case 0x192: c.vmixR = (c.vmixR & 0xFFFFu) | ((value & 0xFFu) << 16); break;
        case 0x194: c.vmixER = (c.vmixER & 0xFF0000u) | value; break;
        case 0x196: c.vmixER = (c.vmixER & 0xFFFFu) | ((value & 0xFFu) << 16); break;
        case 0x198: c.mmix = value; break;
        case 0x19A:
            c.attr = value;
            if ((value & 0x40u) == 0u)
                m_irqInfo &= static_cast<uint16_t>(~(4u << core));
            break;
        case 0x19C: c.irqa = setHigh(c.irqa, value); break;
        case 0x19E: c.irqa = setLow(c.irqa, value); break;
        case 0x1A0: c.konLatch = value; keyOn(c, core, value); break;
        case 0x1A2: c.konLatch = static_cast<uint32_t>(value) << 16; keyOn(c, core, (static_cast<uint32_t>(value) & 0xFFu) << 16); break;
        case 0x1A4: c.kofLatch = value; keyOff(c, value); break;
        case 0x1A6: c.kofLatch = static_cast<uint32_t>(value) << 16; keyOff(c, (static_cast<uint32_t>(value) & 0xFFu) << 16); break;
        case 0x1A8: c.tsa = setHigh(c.tsa, value); break;
        case 0x1AA: c.tsa = setLow(c.tsa, value); break;
        case 0x1AC:
            m_ram[c.tsa] = value;
            checkIrq(c.tsa, c.tsa + 1u);
            c.tsa = (c.tsa + 1u) & kAddrMask;
            break;
        case 0x1B0:
            c.admas = value;
            if (value == 0u)
                c.adma.clear();
            break;
        case 0x340:
        case 0x342: c.endx = 0; break; // writing ENDX clears it
        case 0x344: break;             // STATX is read-only here (always ready)
        default: break;
        }
    }

    void IopSpu2::keyOn(Core &c, uint32_t coreIndex, uint32_t mask)
    {
        for (uint32_t i = 0; i < 24; ++i)
        {
            if ((mask & (1u << i)) == 0u)
                continue;
            Voice &v = c.voices[i];
            ++m_stats.keyOns;
            v.nax = v.ssa & kAddrMask;
            v.counter = 0;
            v.hist1 = v.hist2 = 0;
            std::memset(v.samples, 0, sizeof(v.samples));
            v.out = 0;
            v.blockValid = false;
            v.lsaSetBySoftware = false;
            v.phase = Voice::Phase::Attack;
            v.level = 0;
            v.envCounter = 0;
            c.endx &= ~(1u << i);
            decodeBlock(c, coreIndex, v);
            if (m_voiceLog)
                std::fprintf(stderr, "[spu2-voice] %llu KON %u:%u ssa %05x pitch %04x adsr %04x/%04x vol %04x/%04x header %04x\n",
                             static_cast<unsigned long long>(m_nextSampleCycle / kCyclesPerSample), coreIndex, i, v.ssa, v.pitch,
                             v.adsr1, v.adsr2, v.volL, v.volR, m_ram[v.ssa & kAddrMask]);
        }
    }

    // PS2X_SPU2_VOICE_LOG=1: every 0.1 s, each sounding voice's address, envelope and output peaks since the last report
    void IopSpu2::reportVoices()
    {
        for (uint32_t ci = 0; ci < 2; ++ci)
            for (uint32_t vi = 0; vi < 24; ++vi)
            {
                Voice &v = m_cores[ci].voices[vi];
                if (v.phase == Voice::Phase::Off && v.dbgPeak == 0)
                    continue;
                std::fprintf(stderr, "[spu2-voice] %llu %u:%u phase %d nax %05x lsa %05x env %d sample peak %d out peak %d vol %04x/%04x\n",
                             static_cast<unsigned long long>(m_nextSampleCycle / kCyclesPerSample), ci, vi, static_cast<int>(v.phase),
                             v.nax, v.lsa, v.level, v.dbgSamplePeak, v.dbgPeak, v.volL, v.volR);
                v.dbgPeak = v.dbgSamplePeak = 0;
            }
    }

    void IopSpu2::keyOff(Core &c, uint32_t mask)
    {
        for (uint32_t i = 0; i < 24; ++i)
            if ((mask & (1u << i)) != 0u && c.voices[i].phase != Voice::Phase::Off)
            {
                c.voices[i].phase = Voice::Phase::Release;
                c.voices[i].envCounter = 0;
            }
    }

    // ---- voices ---------------------------------------------------------------------------------------------------
    void IopSpu2::decodeBlock(Core &c, uint32_t coreIndex, Voice &v)
    {
        (void)coreIndex;
        const uint32_t addr = v.nax & kAddrMask;
        checkIrq(addr, addr + 8u);
        const uint16_t header = m_ram[addr];
        uint32_t shift = header & 0xFu;
        if (shift > 12u)
            shift = 9u;
        const uint32_t filter = std::min<uint32_t>((header >> 4) & 0x7u, 4u);
        v.blockFlags = static_cast<uint8_t>(header >> 8);
        v.blockAddr = addr;
        if ((v.blockFlags & 0x4u) != 0u && !v.lsaSetBySoftware)
            v.lsa = addr;
        std::memcpy(v.samples, v.samples + 28, 3 * sizeof(int16_t)); // keep the previous block's last three
        for (uint32_t i = 0; i < 28; ++i)
        {
            const uint16_t word = m_ram[(addr + 1u + i / 4u) & kAddrMask];
            const int32_t nibble = static_cast<int16_t>(static_cast<uint16_t>(((word >> ((i & 3u) * 4u)) & 0xFu) << 12)) >> shift;
            int32_t s = nibble + ((v.hist1 * kFilterPos[filter] + v.hist2 * kFilterNeg[filter] + 32) >> 6);
            s = clamp16(s);
            v.hist2 = v.hist1;
            v.hist1 = s;
            v.samples[3u + i] = static_cast<int16_t>(s);
        }
        v.blockValid = true;
        (void)c;
    }

    void IopSpu2::stepEnvelope(Voice &v)
    {
        int32_t rate = 0; // shift << 2 | step
        bool exponential = false, decrease = false;
        switch (v.phase)
        {
        case Voice::Phase::Off:
            v.level = 0;
            return;
        case Voice::Phase::Attack:
            rate = (v.adsr1 >> 8) & 0x7F;
            exponential = (v.adsr1 & 0x8000u) != 0u;
            break;
        case Voice::Phase::Decay:
            rate = ((v.adsr1 >> 4) & 0xF) << 2;
            exponential = true;
            decrease = true;
            break;
        case Voice::Phase::Sustain:
            rate = (v.adsr2 >> 6) & 0x7F;
            exponential = (v.adsr2 & 0x8000u) != 0u;
            decrease = (v.adsr2 & 0x4000u) != 0u;
            break;
        case Voice::Phase::Release:
            rate = (v.adsr2 & 0x1F) << 2;
            exponential = (v.adsr2 & 0x20u) != 0u;
            decrease = true;
            break;
        }
        envelopeTick(v.level, v.envCounter, rate >> 2, rate & 3, exponential, decrease);
        v.level = std::clamp(v.level, 0, 0x7FFF); // the ADSR level is never negative
        switch (v.phase)
        {
        case Voice::Phase::Attack:
            if (v.level >= 0x7FFF)
            {
                v.phase = Voice::Phase::Decay;
                v.envCounter = 0;
            }
            break;
        case Voice::Phase::Decay:
        {
            const int32_t sustain = std::min<int32_t>(((v.adsr1 & 0xF) + 1) * 0x800, 0x7FFF);
            if (v.level <= sustain)
            {
                v.phase = Voice::Phase::Sustain;
                v.envCounter = 0;
            }
            break;
        }
        case Voice::Phase::Release:
            if (v.level <= 0)
                v.phase = Voice::Phase::Off;
            break;
        default:
            break;
        }
    }

    int32_t IopSpu2::voiceSample(Core &c, uint32_t coreIndex, uint32_t voiceIndex)
    {
        Voice &v = c.voices[voiceIndex];
        if (v.phase == Voice::Phase::Off)
            return 0;
        if (!v.blockValid)
            decodeBlock(c, coreIndex, v);
        int32_t sample;
        if ((c.non & (1u << voiceIndex)) != 0u)
        {
            sample = static_cast<int16_t>(c.noiseLevel);
            ++m_stats.noiseSamples;
        }
        else
        {
            // 4-point Gaussian over the current sample and the three before it (psx-spx)
            const uint32_t index = std::min<uint32_t>(v.counter >> 12, 27u);
            const uint32_t g = (v.counter >> 4) & 0xFFu;
            const int16_t *s = v.samples + index; // s[3] = current ("new"), s[0] = oldest
            sample = ((kGauss[0x0FFu - g] * s[0]) >> 15) + ((kGauss[0x1FFu - g] * s[1]) >> 15) +
                     ((kGauss[0x100u + g] * s[2]) >> 15) + ((kGauss[g] * s[3]) >> 15);
        }
        stepEnvelope(v);
        const int32_t out = (sample * v.level) >> 15;
        v.out = static_cast<int16_t>(out);

        uint32_t step = v.pitch;
        if (voiceIndex > 0u && (c.pmon & (1u << voiceIndex)) != 0u)
        {
            // pitch modulation by the previous voice's output: step * (1 + out / 0x8000), with the hardware's sign glitch
            const int32_t factor = c.voices[voiceIndex - 1u].out + 0x8000;
            step = static_cast<uint32_t>((static_cast<int32_t>(static_cast<int16_t>(v.pitch)) * factor) >> 15) & 0xFFFFu;
            ++m_stats.pmonSamples;
        }
        v.counter += std::min<uint32_t>(step, 0x4000u);
        while (v.counter >= (28u << 12))
        {
            v.counter -= 28u << 12;
            const uint8_t flags = v.blockFlags;
            if ((flags & 0x1u) != 0u)
            {
                c.endx |= 1u << voiceIndex;
                if ((flags & 0x2u) != 0u)
                    v.nax = v.lsa & kAddrMask;
                else
                {
                    v.phase = Voice::Phase::Off;
                    v.level = 0;
                    v.nax = v.lsa & kAddrMask;
                }
            }
            else
            {
                v.nax = (v.blockAddr + 8u) & kAddrMask;
            }
            decodeBlock(c, coreIndex, v);
        }
        return out;
    }

    void IopSpu2::stepNoise(Core &c)
    {
        // psx-spx "SPU Noise Generator": ATTR bits 10-13 shift, bits 8-9 step (4..7), clocked at the output rate
        const int32_t shift = (c.attr >> 10) & 0xF;
        c.noiseTimer -= 4 + ((c.attr >> 8) & 3);
        if (c.noiseTimer < 0)
        {
            const uint32_t n = c.noiseLevel;
            const uint32_t parity = ((n >> 15) ^ (n >> 12) ^ (n >> 11) ^ (n >> 10) ^ 1u) & 1u;
            c.noiseLevel = static_cast<uint16_t>((n << 1) | parity);
            c.noiseTimer += 0x20000 >> shift;
            if (c.noiseTimer < 0)
                c.noiseTimer += 0x20000 >> shift;
        }
    }

    // Reverb, psx-spx "SPU Reverb Formula" with SPU2 registers: one 48 kHz tick computes the left side, the next the
    // right side (each channel at 24 kHz), the input downsampled and the output upsampled with the 39-tap FIR. The
    // buffer address advances after the right side and wraps within ESA..EEA. ATTR bit 7 gates the buffer writes only.
    void IopSpu2::reverb(Core &c, int32_t inL, int32_t inR, int32_t &outL, int32_t &outR)
    {
        const uint32_t pos = c.revbTick & 63u;
        c.revbIn[0][pos] = c.revbIn[0][pos | 64u] = clamp16(inL);
        c.revbIn[1][pos] = c.revbIn[1][pos | 64u] = clamp16(inR);
        const uint32_t side = c.revbTick & 1u; // 0 = left, 1 = right
        int32_t wet = 0;
        if (c.esa < c.eea)
        {
            const uint32_t size = c.eea - c.esa + 1u;
            auto at = [&](uint32_t reg, uint32_t back = 0u) {
                const uint32_t rel = (c.revbPos + c.revbAddr[reg]) % size;
                return (c.esa + (rel + size - back % size) % size) & kAddrMask;
            };
            auto rd = [&](uint32_t a) { return static_cast<int32_t>(static_cast<int16_t>(m_ram[a])); };
            const bool write = (c.attr & 0x80u) != 0u;
            auto wr = [&](uint32_t a, int32_t value) {
                if (write)
                    m_ram[a] = static_cast<uint16_t>(clamp16(value));
            };
            const int16_t *vol = c.revbVol;
            const int32_t vIIR = vol[0], vWALL = vol[5], vAPF1 = vol[6], vAPF2 = vol[7];
            const int32_t in = mul15(vol[8 + side], clamp16(fir39(&c.revbIn[side][(pos - 38u) & 63u]) >> 15));
            // register indices (revbAddr): 0 dAPF1, 1 dAPF2, 2/3 mSAME, 4/5 mCOMB1, 6/7 mCOMB2, 8/9 dSAME, 10/11 mDIFF,
            // 12/13 mCOMB3, 14/15 mCOMB4, 16/17 dDIFF, 18/19 mAPF1, 20/21 mAPF2 (left / right)
            const uint32_t mSame = at(2 + side), mSamePrev = at(2 + side, 1u), dSame = at(8 + side);
            const uint32_t mDiff = at(10 + side), mDiffPrev = at(10 + side, 1u), dDiff = at(17 - side); // other side
            const int32_t same = mul15(in + mul15(rd(dSame), vWALL) - rd(mSamePrev), vIIR) + rd(mSamePrev);
            const int32_t diff = mul15(in + mul15(rd(dDiff), vWALL) - rd(mDiffPrev), vIIR) + rd(mDiffPrev);
            wr(mSame, same);
            wr(mDiff, diff);
            int32_t out = mul15(vol[1], rd(at(4 + side))) + mul15(vol[2], rd(at(6 + side))) + mul15(vol[3], rd(at(12 + side))) +
                          mul15(vol[4], rd(at(14 + side)));
            const uint32_t apf1 = at(18 + side), apf1Src = at(18 + side, c.revbAddr[0]);
            const int32_t a1 = out - mul15(vAPF1, rd(apf1Src));
            wr(apf1, a1);
            out = mul15(clamp16(a1), vAPF1) + rd(apf1Src);
            const uint32_t apf2 = at(20 + side), apf2Src = at(20 + side, c.revbAddr[1]);
            const int32_t a2 = out - mul15(vAPF2, rd(apf2Src));
            wr(apf2, a2);
            out = mul15(clamp16(a2), vAPF2) + rd(apf2Src);
            wet = clamp16(out);
            if (side == 1u)
                c.revbPos = (c.revbPos + 1u) % size;
        }
        c.revbOut[side][pos] = c.revbOut[side][pos | 64u] = static_cast<int16_t>(wet);
        c.revbOut[side ^ 1u][pos] = c.revbOut[side ^ 1u][pos | 64u] = 0;
        // zero-stuffed 24 kHz -> 48 kHz: gain 2 (>> 14)
        outL = clamp16(fir39(&c.revbOut[0][(pos - 38u) & 63u]) >> 14);
        outR = clamp16(fir39(&c.revbOut[1][(pos - 38u) & 63u]) >> 14);
        ++c.revbTick;
    }

    void IopSpu2::mixOne(int16_t &outL, int16_t &outR)
    {
        static const bool reverbOn = [] {
            const char *e = std::getenv("PS2X_SPU2_REVERB");
            return !(e && e[0] == '0');
        }();
        int32_t core0L = 0, core0R = 0;
        for (uint32_t ci = 0; ci < 2; ++ci)
        {
            Core &c = m_cores[ci];
            if (c.non != 0u)
                stepNoise(c);
            int32_t dryL = 0, dryR = 0, wetL = 0, wetR = 0, auxL = 0, auxR = 0;
            const uint32_t auxMask = m_auxSink ? m_auxMask[ci] : 0u;
            for (uint32_t vi = 0; vi < 24; ++vi)
            {
                Voice &v = c.voices[vi];
                if (v.phase == Voice::Phase::Off)
                {
                    v.out = 0;
                    continue;
                }
                const int32_t s = voiceSample(c, ci, vi);
                const int32_t l = (s * fixedVolume(v.volL, v.curVolL)) >> 15;
                const int32_t r = (s * fixedVolume(v.volR, v.curVolR)) >> 15;
                if (m_voiceLog)
                {
                    v.dbgPeak = std::max<int32_t>(v.dbgPeak, std::max(std::abs(l), std::abs(r)));
                    v.dbgSamplePeak = std::max<int32_t>(v.dbgSamplePeak, std::abs(s));
                }
                const uint32_t bit = 1u << vi;
                if (auxMask & bit) // [gta-recomp] an aux voice: the second output only
                {
                    auxL += l;
                    auxR += r;
                    continue;
                }
                if (c.vmixL & bit)
                    dryL += l;
                if (c.vmixR & bit)
                    dryR += r;
                if (c.vmixEL & bit)
                    wetL += l;
                if (c.vmixER & bit)
                    wetR += r;
            }
            // MMIX gates (PCSX2's naming): 0x800/0x400 voices dry L/R, 0x200/0x100 voices wet, 0x80/0x40 input dry,
            // 0x20/0x10 input wet, 0x08/0x04 external (core 0 into core 1) dry, 0x02/0x01 external wet
            int32_t mixL = (c.mmix & 0x800u) ? dryL : 0;
            int32_t mixR = (c.mmix & 0x400u) ? dryR : 0;
            int32_t fxL = (c.mmix & 0x200u) ? wetL : 0;
            int32_t fxR = (c.mmix & 0x100u) ? wetR : 0;
            // AutoDMA input
            if (!c.adma.empty())
            {
                const auto [inL, inR] = c.adma.front();
                c.adma.pop_front();
                const uint32_t base = 0x2000u + ci * 0x400u;
                checkIrq(base + c.admaReadPos, base + c.admaReadPos + 1u);
                checkIrq(base + 0x200u + c.admaReadPos, base + 0x200u + c.admaReadPos + 1u);
                c.admaReadPos = (c.admaReadPos + 1u) & 0x1FFu;
                // [gta-recomp] BVOL ("sound data input volume") scales the AutoDMA input: a plain signed 16-bit volume
                // (no sweep mode), as PCSX2 has it
                const int32_t l = (inL * static_cast<int16_t>(c.bvolL)) >> 15;
                const int32_t r = (inR * static_cast<int16_t>(c.bvolR)) >> 15;
                if (c.mmix & 0x80u)
                    mixL += l;
                if (c.mmix & 0x40u)
                    mixR += r;
                if (c.mmix & 0x20u)
                    fxL += l;
                if (c.mmix & 0x10u)
                    fxR += r;
            }
            // core 0 output into core 1, scaled by core 1's AVOL ("core external input volume", only core 1), a plain
            // signed 16-bit volume. [gta-recomp] Was BVOL, decoded as a sweepable volume: libsd sets core 1 AVOL 0x7FFF
            // (full) / BVOL 0, so everything on core 0 came out silent (0x7FFF read as a fixed volume is -2).
            if (ci == 1u)
            {
                const int32_t l = (core0L * static_cast<int16_t>(c.avolL)) >> 15;
                const int32_t r = (core0R * static_cast<int16_t>(c.avolR)) >> 15;
                if (c.mmix & 0x08u)
                    mixL += l;
                if (c.mmix & 0x04u)
                    mixR += r;
                if (c.mmix & 0x02u)
                    fxL += l;
                if (c.mmix & 0x01u)
                    fxR += r;
            }
            if (fxL != 0 || fxR != 0)
                ++m_stats.wetSamples[ci];
            // reverb: skipped while it can neither write its buffer nor be heard
            if (reverbOn && ((c.attr & 0x80u) != 0u || c.evolL != 0u || c.evolR != 0u))
            {
                int32_t revL = 0, revR = 0;
                reverb(c, fxL, fxR, revL, revR);
                revL = mul15(revL, static_cast<int16_t>(c.evolL));
                revR = mul15(revR, static_cast<int16_t>(c.evolR));
                if (revL != 0 || revR != 0)
                {
                    ++m_stats.reverbSamples[ci];
                    m_stats.reverbPeak = std::max(m_stats.reverbPeak, std::max(std::abs(revL), std::abs(revR)));
                }
                mixL += revL;
                mixR += revR;
            }
            const int32_t mvL = fixedVolume(c.mvolL, c.curMvolL), mvR = fixedVolume(c.mvolR, c.curMvolR);
            const int32_t l = clamp16((mixL * mvL) >> 15);
            const int32_t r = clamp16((mixR * mvR) >> 15);
            if (ci == 0u)
            {
                core0L = l;
                core0R = r;
                m_auxL = (auxL * mvL) >> 15; // [gta-recomp] (core 1's main volume below, as core 0's mix gets it)
                m_auxR = (auxR * mvR) >> 15;
            }
            else
            {
                outL = static_cast<int16_t>(l);
                outR = static_cast<int16_t>(r);
                m_auxL = clamp16(((m_auxL * mvL) >> 15) + ((auxL * mvL) >> 15));
                m_auxR = clamp16(((m_auxR * mvR) >> 15) + ((auxR * mvR) >> 15));
            }
        }
    }

    void IopSpu2::reportStats()
    {
        static const bool trace = std::getenv("PS2X_TRACE_SPU2") != nullptr;
        uint32_t active = 0;
        for (const Core &c : m_cores)
            for (const Voice &v : c.voices)
                active += v.phase != Voice::Phase::Off ? 1u : 0u;
        if (trace)
            std::fprintf(stderr,
                         "[spu2] 5 s: key-ons %llu, active now %u, dma %llu KB, adma %llu samples, irqs %llu, peak %d | core0 attr %04x mmix %04x "
                         "mvol %04x/%04x admas %x | core1 attr %04x mmix %04x mvol %04x/%04x admas %x\n",
                         (unsigned long long)m_stats.keyOns, active, (unsigned long long)(m_stats.dmaBytes >> 10),
                         (unsigned long long)m_stats.admaSamples, (unsigned long long)m_stats.irqs, m_stats.peak, m_cores[0].attr,
                         m_cores[0].mmix, m_cores[0].mvolL, m_cores[0].mvolR, m_cores[0].admas, m_cores[1].attr, m_cores[1].mmix,
                         m_cores[1].mvolL, m_cores[1].mvolR, m_cores[1].admas);
        if (trace)
            for (uint32_t ci = 0; ci < 2; ++ci)
            {
                const Core &c = m_cores[ci];
                std::fprintf(stderr,
                             "[spu2]   core%u fx: wet %llu, reverb out %llu (peak %d), esa %05x eea %05x evol %04x/%04x vmixE %06x/%06x "
                             "vIIR %04x vLIN %04x | pmon %06x non %06x (pmon samples %llu, noise %llu)\n",
                             ci, (unsigned long long)m_stats.wetSamples[ci], (unsigned long long)m_stats.reverbSamples[ci],
                             m_stats.reverbPeak, c.esa, c.eea, c.evolL, c.evolR, c.vmixEL, c.vmixER, (uint16_t)c.revbVol[0],
                             (uint16_t)c.revbVol[8], c.pmon, c.non, (unsigned long long)m_stats.pmonSamples,
                             (unsigned long long)m_stats.noiseSamples);
            }
        m_stats = {};
    }

    void IopSpu2::advanceTo(uint64_t cycle)
    {
        m_cycle = cycle;
        while (m_nextSampleCycle <= cycle)
        {
            int16_t l = 0, r = 0;
            mixOne(l, r);
            m_stats.peak = std::max<int32_t>(m_stats.peak, std::max(std::abs(l), std::abs(r)));
            if ((++m_stats.frames % (48000u * 5u)) == 0u)
                reportStats();
            if (m_voiceLog && (m_nextSampleCycle / kCyclesPerSample) % 4800u == 0u)
                reportVoices();
            m_outBuffer.push_back(l);
            m_outBuffer.push_back(r);
            if (m_auxSink) // [gta-recomp] the second output, sample for sample with the first
            {
                m_auxBuffer.push_back(static_cast<int16_t>(m_auxL));
                m_auxBuffer.push_back(static_cast<int16_t>(m_auxR));
                if (m_auxBuffer.size() >= 512u)
                {
                    m_auxSink(m_auxBuffer.data(), static_cast<uint32_t>(m_auxBuffer.size() / 2u));
                    m_auxBuffer.clear();
                }
            }
            m_nextSampleCycle += kCyclesPerSample;
            if (m_outBuffer.size() >= 512u)
            {
                if (m_sink)
                    m_sink(m_outBuffer.data(), static_cast<uint32_t>(m_outBuffer.size() / 2u));
                m_outBuffer.clear();
                if (m_trace)
                    std::fflush(m_trace); // the game leaves through _Exit
            }
        }
    }

    // ---- transfers ------------------------------------------------------------------------------------------------
    uint64_t IopSpu2::dmaTransfer(uint32_t core, uint8_t *iopRam, uint32_t iopRamSize, uint32_t madr, uint32_t bytes, bool toSpu)
    {
        core &= 1u;
        Core &c = m_cores[core];
        madr &= 0x1FFFFFu;
        if (madr >= iopRamSize)
            return 64u;
        bytes = std::min(bytes, iopRamSize - madr);
        const uint32_t halfwords = bytes / 2u;
        m_stats.dmaBytes += bytes;
        const uint16_t *src = reinterpret_cast<const uint16_t *>(iopRam + madr);
        if (m_trace)
        {
            uint32_t sum = 0; // FNV-1a over the data
            for (uint32_t i = 0; i < bytes; ++i)
                sum = (sum ^ iopRam[madr + i]) * 16777619u;
            std::fprintf(m_trace, "%llu D %u %05x %x %d %08x\n", static_cast<unsigned long long>(m_nextSampleCycle / kCyclesPerSample), core, c.tsa, bytes,
                         toSpu ? 1 : 0, sum);
        }
        if (toSpu && (c.admas & (1u << core)) != 0u)
        {
            // AutoDMA: 512-byte blocks per channel (256 L samples, then 256 R samples)
            for (uint32_t block = 0; block + 512u <= halfwords; block += 512u)
                for (uint32_t i = 0; i < 256u; ++i)
                    c.adma.emplace_back(static_cast<int16_t>(src[block + i]), static_cast<int16_t>(src[block + 256u + i]));
            m_stats.admaSamples += halfwords / 2u;
            // Done when the SPU has played down to one block: the next one is then queued in time.
            const uint64_t queued = c.adma.size();
            return queued > 256u ? (queued - 256u) * kCyclesPerSample : 64u;
        }
        if (toSpu)
        {
            for (uint32_t i = 0; i < halfwords; ++i)
                m_ram[(c.tsa + i) & kAddrMask] = src[i];
            checkIrq(c.tsa, c.tsa + halfwords);
            c.tsa = (c.tsa + halfwords) & kAddrMask;
        }
        else
        {
            uint16_t *dst = reinterpret_cast<uint16_t *>(iopRam + madr);
            for (uint32_t i = 0; i < halfwords; ++i)
                dst[i] = m_ram[(c.tsa + i) & kAddrMask];
            c.tsa = (c.tsa + halfwords) & kAddrMask;
        }
        return std::max<uint64_t>(64u, halfwords);
    }
}
