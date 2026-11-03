// zlib 1.2.2 inflate_fast natively (vcs_inflate.h).
#include "game/vcs_inflate.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{
    constexpr uint32_t kInflateFast = 0x00184428u;
    constexpr uint32_t kRamSize = PS2_RAM_SIZE;
    constexpr uint32_t kModeType = 11u; // inflate_mode TYPE (BAD = 27, game-internals.md 8)

    // z_stream (EE build: 64-bit long) and inflate_state field offsets
    constexpr uint32_t kNextIn = 0x00, kAvailIn = 0x04, kNextOut = 0x10, kAvailOut = 0x14, kState = 0x24;
    constexpr uint32_t kMode = 0x00, kWsize = 0x2C, kWhave = 0x30, kWrite = 0x34, kWindow = 0x38, kHold = 0x40, kBits = 0x48,
                       kLencode = 0x58, kDistcode = 0x5C, kLenbits = 0x60, kDistbits = 0x64;

    PS2Runtime::RecompiledFunction g_original = nullptr;
    bool g_verify = false, g_log = false;

    uint32_t rd32(const uint8_t *ram, uint32_t a)
    {
        uint32_t v;
        std::memcpy(&v, ram + a, 4);
        return v;
    }
    void wr32(uint8_t *ram, uint32_t a, uint32_t v) { std::memcpy(ram + a, &v, 4); }

    // a guest address of a range of main RAM -> its offset (kRamSize: not plain main RAM)
    uint32_t ramOffset(uint32_t address, uint32_t bytes)
    {
        const uint32_t a = address & 0x1FFFFFFFu;
        return a < kRamSize && bytes <= kRamSize - a ? a : kRamSize;
    }

    struct Code // zlib's `code`
    {
        uint8_t op, bits;
        uint16_t val;
    };

    // What one call decoded: the state it leaves (written back unless verifying)
    struct Result
    {
        uint32_t in, out; // offsets in RAM of next_in / next_out
        uint32_t availIn, availOut;
        uint64_t hold;
        uint32_t bits;
        bool endOfBlock = false;
    };

    // inffast.c of zlib 1.2.2 line for line, as the EE build has it: OFF = 1, so `in` / `out` / `from` point one before
    // the next byte and every access pre-increments (PUP); all of them are RAM offsets. Output bytes go through outAt
    // (the guest RAM itself, or a shadow copy when verifying); the window is read where it is. Returns false where zlib
    // reports bad data (the caller hands the call to the original).
    //
    // Fast (PERFORMANCE.md 24): the code bits come from a 64-bit buffer `buf` refilled 8 bytes at a time at the top of
    // each symbol (a symbol takes at most 48 bits: one refill, no branch per byte), while `in` / `bits` still count what
    // zlib's own hold has loaded, byte for byte as zlib pulls them - only counted, not read. So the loop test (`in <
    // last`, zlib's input pointer) and the state left (next_in, bits, hold: the unconsumed bits, zlib's own count of
    // them) are zlib's, and the output is the same: the same codes are read from the same bits.
    template <bool Fast>
    bool decodeT(const uint8_t *ram, uint8_t *outBase, uint32_t outOrigin, uint32_t strm, uint32_t start, Result &r)
    {
        const uint32_t state = rd32(ram, strm + kState) & 0x1FFFFFFFu;
        uint32_t in = (rd32(ram, strm + kNextIn) & 0x1FFFFFFFu) - 1u;
        const uint32_t last = in + (rd32(ram, strm + kAvailIn) - 5u);
        uint32_t out = (rd32(ram, strm + kNextOut) & 0x1FFFFFFFu) - 1u;
        const uint32_t availOut = rd32(ram, strm + kAvailOut);
        const uint32_t beg = out - (start - availOut);
        const uint32_t end = out + (availOut - 257u);
        const uint32_t wsize = rd32(ram, state + kWsize), whave = rd32(ram, state + kWhave), write = rd32(ram, state + kWrite);
        const uint32_t window = rd32(ram, state + kWindow) & 0x1FFFFFFFu;
        uint64_t hold;
        std::memcpy(&hold, ram + state + kHold, 8);
        uint32_t bits = rd32(ram, state + kBits);
        const uint32_t lcode = rd32(ram, state + kLencode) & 0x1FFFFFFFu, dcode = rd32(ram, state + kDistcode) & 0x1FFFFFFFu;
        const uint32_t lmask = (1u << rd32(ram, state + kLenbits)) - 1u, dmask = (1u << rd32(ram, state + kDistbits)) - 1u;
        if ((wsize && ramOffset(window, wsize) == kRamSize) || ramOffset(lcode, (lmask + 1u) * 4u) == kRamSize ||
            ramOffset(dcode, (dmask + 1u) * 4u) == kRamSize)
            return false;
        // Fast: the bit buffer - the unconsumed bits from the low end (at least as many as zlib's hold has), `next` the
        // next byte to load
        uint64_t buf = 0;
        uint32_t have = 0, next = 0;
        if constexpr (Fast)
        {
            if (bits >= 57u)
                return false; // (never: zlib holds at most ~32 bits between calls)
            buf = hold & ((uint64_t(1) << bits) - 1u);
            have = bits;
            next = in + 1u;
        }
        auto pull = [&]() { // PULLBYTE: zlib loads the next byte into its hold (Fast: counted only)
            if constexpr (Fast)
                ++in;
            else
                hold += uint64_t(ram[++in]) << bits;
            bits += 8u;
        };
        auto peek = [&]() -> uint32_t {
            if constexpr (Fast)
                return static_cast<uint32_t>(buf);
            else
                return static_cast<uint32_t>(hold);
        };
        auto drop = [&](uint32_t n) {
            if constexpr (Fast)
            {
                buf >>= n;
                have -= n;
            }
            else
                hold >>= n;
            bits -= n;
        };
        auto outAt = [&](uint32_t a) -> uint8_t & { return outBase[a - outOrigin]; };
        auto code = [&](uint32_t table, uint32_t index) {
            Code c;
            std::memcpy(&c, ram + table + index * 4u, 4);
            return c;
        };
        // n bytes as zlib's byte loops copy them (PUP on both sides). From the window: never the output's bytes - one
        // copy. From the output: one copy when the distance is at least n; closer, the bytes being written are read
        // again (the repeat a short distance encodes) - byte by byte, as zlib.
        // Fast: the short copies most matches are (3..32 bytes) as two fixed-size moves of the first and last bytes
        // instead of a memcpy call - exactly n bytes written (the source does not overlap the n bytes written)
        auto copySmall = [](uint8_t *dst, const uint8_t *src, uint32_t n) {
            if constexpr (Fast)
            {
                if (n <= 32u)
                {
                    if (n >= 16u)
                    {
                        uint8_t a[16], b[16];
                        std::memcpy(a, src, 16);
                        std::memcpy(b, src + n - 16u, 16);
                        std::memcpy(dst, a, 16);
                        std::memcpy(dst + n - 16u, b, 16);
                    }
                    else if (n >= 8u)
                    {
                        uint64_t a, b;
                        std::memcpy(&a, src, 8);
                        std::memcpy(&b, src + n - 8u, 8);
                        std::memcpy(dst, &a, 8);
                        std::memcpy(dst + n - 8u, &b, 8);
                    }
                    else if (n >= 4u)
                    {
                        uint32_t a, b;
                        std::memcpy(&a, src, 4);
                        std::memcpy(&b, src + n - 4u, 4);
                        std::memcpy(dst, &a, 4);
                        std::memcpy(dst + n - 4u, &b, 4);
                    }
                    else
                        for (uint32_t i = 0; i < n; ++i)
                            dst[i] = src[i];
                    return;
                }
            }
            std::memcpy(dst, src, n);
        };
        auto copyFromWindow = [&](uint32_t &from, uint32_t n) {
            copySmall(&outAt(out + 1u), ram + from + 1u, n);
            out += n;
            from += n;
        };
        auto copyFromOutput = [&](uint32_t &from, uint32_t n) {
            const uint32_t dist = out - from;
            if (dist >= n)
            {
                copySmall(&outAt(out + 1u), &outAt(from + 1u), n);
                out += n;
                from += n;
                return;
            }
            if constexpr (Fast)
            {
                // a repeat closer than its length: zlib's byte loop reads bytes it has just written. Distance 1 is one byte
                // repeated; from 8 on, each 8-byte step reads only bytes written before it - the same bytes either way
                uint8_t *dst = &outAt(out + 1u);
                if (dist == 1u)
                {
                    std::memset(dst, dst[-1], n);
                    out += n;
                    from += n;
                    return;
                }
                if (dist >= 8u)
                {
                    const uint8_t *src = dst - dist;
                    uint32_t i = 0;
                    for (; i + 8u <= n; i += 8u)
                    {
                        uint64_t v;
                        std::memcpy(&v, src + i, 8);
                        std::memcpy(dst + i, &v, 8);
                    }
                    for (; i < n; ++i)
                        dst[i] = src[i];
                    out += n;
                    from += n;
                    return;
                }
            }
            while (n--)
                outAt(++out) = outAt(++from);
        };
        do
        {
            if (in + 8u >= kRamSize) // (an iteration reads at most 7 bytes: never past main RAM - bad data or a buffer the
                return false;         // game said was longer: the original's call)
            if constexpr (Fast)
            {
                if (next > kRamSize - 8u)
                    return false;
                if (have <= 56u)
                {
                    uint64_t w;
                    std::memcpy(&w, ram + next, 8);
                    buf |= w << have; // (bytes past `have` already there are the same stream bytes)
                    next += (63u - have) >> 3;
                    have |= 56u;
                }
            }
            if (bits < 15u)
            {
                pull();
                pull();
            }
            Code here = code(lcode, peek() & lmask);
        dolen:
            uint32_t op = here.bits;
            drop(op);
            op = here.op;
            if (op == 0u) // literal
                outAt(++out) = static_cast<uint8_t>(here.val);
            else if (op & 16u) // length base
            {
                uint32_t len = here.val;
                op &= 15u; // number of extra bits
                if (op)
                {
                    if (bits < op)
                        pull();
                    len += peek() & ((1u << op) - 1u);
                    drop(op);
                }
                if (bits < 15u)
                {
                    pull();
                    pull();
                }
                here = code(dcode, peek() & dmask);
            dodist:
                op = here.bits;
                drop(op);
                op = here.op;
                if (op & 16u) // distance base
                {
                    uint32_t dist = here.val;
                    op &= 15u; // number of extra bits
                    if (bits < op)
                    {
                        pull();
                        if (bits < op)
                            pull();
                    }
                    dist += peek() & ((1u << op) - 1u);
                    drop(op);
                    op = out - beg; // max distance in output
                    if (dist > op) // see if copy from window
                    {
                        op = dist - op; // distance back in window
                        if (op > whave)
                            return false; // "invalid distance too far back"
                        uint32_t from = window - 1u;
                        bool inWindow = true; // `from` reads the window (else the output)
                        if (write == 0u) // very common case
                        {
                            from += wsize - op;
                            if (op < len) // some from window
                            {
                                len -= op;
                                copyFromWindow(from, op);
                                op = 0u;
                                from = out - dist; // rest from output
                                inWindow = false;
                            }
                        }
                        else if (write < op) // wrap around window
                        {
                            from += wsize + write - op;
                            op -= write;
                            if (op < len) // some from end of window
                            {
                                len -= op;
                                copyFromWindow(from, op);
                                op = 0u;
                                from = window - 1u;
                                if (write < len) // some from start of window
                                {
                                    op = write;
                                    len -= op;
                                    copyFromWindow(from, op);
                                    op = 0u;
                                    from = out - dist; // rest from output
                                    inWindow = false;
                                }
                            }
                        }
                        else // contiguous in window
                        {
                            from += write - op;
                            if (op < len) // some from window
                            {
                                len -= op;
                                copyFromWindow(from, op);
                                op = 0u;
                                from = out - dist; // rest from output
                                inWindow = false;
                            }
                        }
                        if (inWindow) // (zlib: three at a time, then the one or two left - len bytes)
                            copyFromWindow(from, len);
                        else
                            copyFromOutput(from, len);
                    }
                    else
                    {
                        uint32_t from = out - dist; // copy direct from output (minimum length is three: len bytes)
                        copyFromOutput(from, len);
                    }
                }
                else if ((op & 64u) == 0u) // 2nd level distance code
                {
                    here = code(dcode, here.val + (peek() & ((1u << op) - 1u)));
                    goto dodist;
                }
                else
                    return false; // "invalid distance code"
            }
            else if ((op & 64u) == 0u) // 2nd level length code
            {
                here = code(lcode, here.val + (peek() & ((1u << op) - 1u)));
                goto dolen;
            }
            else if (op & 32u) // end-of-block
            {
                r.endOfBlock = true;
                break;
            }
            else
                return false; // "invalid literal/length code"
        } while (in < last && out < end);
        // return unused bytes (on entry, bits < 8, so in won't go too far back)
        const uint32_t len = bits >> 3;
        in -= len;
        bits -= len << 3;
        if constexpr (Fast)
            hold = buf & ((uint64_t(1) << bits) - 1u); // (the buffer starts with zlib's unconsumed bits)
        else
            hold &= (1u << bits) - 1u;
        r.in = in + 1u; // next_in = in + OFF
        r.out = out + 1u;
        r.availIn = in < last ? 5u + (last - in) : 5u - (in - last);
        r.availOut = out < end ? 257u + (end - out) : 257u - (out - end);
        r.hold = hold;
        r.bits = bits;
        return true;
    }

    // VCS_NATIVE_INFLATE_FAST=0: the byte-wise reads (decodeT<false>), as before 2026-11-02
    bool decode(const uint8_t *ram, uint8_t *outBase, uint32_t outOrigin, uint32_t strm, uint32_t start, Result &r)
    {
        static const bool fast = [] {
            const char *e = std::getenv("VCS_NATIVE_INFLATE_FAST");
            return !(e && *e == '0');
        }();
        return fast ? decodeT<true>(ram, outBase, outOrigin, strm, start, r) : decodeT<false>(ram, outBase, outOrigin, strm, start, r);
    }

    // counters (VCS_NATIVE_INFLATE_LOG)
    std::atomic<uint64_t> g_calls{0}, g_bytes{0}, g_fallbacks{0}, g_verifySame{0}, g_verifyDiffer{0};
    double g_ms = 0.0, g_maxMs = 0.0; // (game thread)
    uint32_t g_maxBytes = 0;

    // VCS_NATIVE_INFLATE_VERIFY: the native decode of the last call, checked at the next
    struct Check
    {
        bool armed = false;
        uint32_t strm = 0;         // the stream it decoded
        uint32_t from = 0, to = 0; // the output it wrote (RAM offsets)
        std::vector<uint8_t> bytes;
    } g_check;

    std::atomic<uint64_t> g_verifyUnchecked{0};

    // at the next call: only when it goes on with the same stream past the bytes checked (else the game may have used or
    // moved the output in between - counted as unchecked)
    void check(const uint8_t *ram, uint32_t strm)
    {
        if (!g_check.armed)
            return;
        g_check.armed = false;
        if (strm != g_check.strm || (rd32(ram, strm + kNextOut) & 0x1FFFFFFFu) < g_check.to)
        {
            ++g_verifyUnchecked;
            return;
        }
        const bool same = std::memcmp(ram + g_check.from, g_check.bytes.data(), g_check.to - g_check.from) == 0;
        ++(same ? g_verifySame : g_verifyDiffer);
        static int logged = 0;
        if (!same && logged++ < 10)
        {
            uint32_t at = 0, count = 0;
            for (uint32_t i = 0; i < g_check.to - g_check.from; ++i)
                if (ram[g_check.from + i] != g_check.bytes[i])
                    at = count++ ? at : i;
            std::fprintf(stderr, "[inflate] verify: output 0x%08x..0x%08x differs from the original's: %u bytes, first at +0x%x (native %02x %02x %02x %02x, guest %02x %02x %02x %02x)\n",
                         g_check.from, g_check.to, count, at, g_check.bytes[at], g_check.bytes[at + 1], g_check.bytes[at + 2],
                         g_check.bytes[at + 3], ram[g_check.from + at], ram[g_check.from + at + 1], ram[g_check.from + at + 2],
                         ram[g_check.from + at + 3]);
        }
    }

    void report()
    {
        static auto at = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (!g_log || now - at < std::chrono::seconds(10))
            return;
        at = now;
        std::fprintf(stderr, "[inflate] %llu calls natively, %.1f MB out in %.1f ms (slowest call %.2f ms, %u bytes), %llu to the original%s",
                     (unsigned long long)g_calls.exchange(0), g_bytes.exchange(0) / 1048576.0, g_ms, g_maxMs, g_maxBytes,
                     (unsigned long long)g_fallbacks.exchange(0), g_verify ? "" : "\n");
        g_ms = g_maxMs = 0.0;
        g_maxBytes = 0;
        if (g_verify)
            std::fprintf(stderr, "; verify: %llu the same, %llu differ, %llu unchecked (the stream did not go on)\n",
                         (unsigned long long)g_verifySame.exchange(0), (unsigned long long)g_verifyDiffer.exchange(0),
                         (unsigned long long)g_verifyUnchecked.exchange(0));
    }

    PS2_GUEST_CC void inflateFastHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        report();
        const uint32_t strm = GPR_U32(ctx, 4) & 0x1FFFFFFFu, start = GPR_U32(ctx, 5);
        if (g_verify)
            check(rdram, strm);
        if (ramOffset(strm, 0x40u) == kRamSize || ramOffset(rd32(rdram, strm + kState), 0x68u) == kRamSize)
        {
            ++g_fallbacks;
            g_original(rdram, ctx, runtime);
            return;
        }
        const uint32_t nextIn = rd32(rdram, strm + kNextIn), availIn = rd32(rdram, strm + kAvailIn);
        const uint32_t nextOut = rd32(rdram, strm + kNextOut), availOut = rd32(rdram, strm + kAvailOut);
        const uint32_t outBeg = (nextOut & 0x1FFFFFFFu) - (start - availOut);
        // (the input only by its start: the game passes avail_in "more than enough" - ~16 MB - for its whole-file inflates;
        // decode stops before reading past main RAM)
        if (availIn < 6u || availOut < 258u || ramOffset(nextIn, 1u) == kRamSize || ramOffset(outBeg, start) == kRamSize)
        {
            static int logged = 0;
            if (g_log && logged++ < 20)
                std::fprintf(stderr, "[inflate] to the original: in 0x%08x (%u), out 0x%08x (avail %u, start %u)\n", nextIn, availIn, nextOut,
                             availOut, start);
            ++g_fallbacks;
            g_original(rdram, ctx, runtime);
            return;
        }
        Result r{};
        if (g_verify)
        {
            // into a copy of the output (what the original reads back from it too), compared at the next call
            thread_local std::vector<uint8_t> shadow;
            shadow.assign(rdram + outBeg, rdram + outBeg + start);
            if (decode(rdram, shadow.data(), outBeg, strm, start, r))
            {
                const uint32_t from = nextOut & 0x1FFFFFFFu;
                g_check.strm = strm;
                g_check.from = from;
                g_check.to = r.out;
                g_check.bytes.assign(shadow.begin() + (from - outBeg), shadow.begin() + (r.out - outBeg));
                g_check.armed = true;
            }
            g_original(rdram, ctx, runtime);
            return;
        }
        static const bool fastVerify = std::getenv("VCS_NATIVE_INFLATE_FAST_VERIFY") != nullptr;
        if (fastVerify) // the fast reader against the byte-wise one: each into its own copy of the output, every call
        {
            thread_local std::vector<uint8_t> a, b;
            a.assign(rdram + outBeg, rdram + outBeg + start);
            b = a;
            Result ra{}, rb{};
            const bool okA = decodeT<false>(rdram, a.data(), outBeg, strm, start, ra);
            const bool okB = decodeT<true>(rdram, b.data(), outBeg, strm, start, rb);
            static uint64_t same = 0, differ = 0;
            const bool equal = okA == okB && (!okA || (ra.in == rb.in && ra.out == rb.out && ra.availIn == rb.availIn &&
                                                       ra.availOut == rb.availOut && ra.hold == rb.hold && ra.bits == rb.bits &&
                                                       ra.endOfBlock == rb.endOfBlock && a == b));
            ++(equal ? same : differ);
            if (!equal && differ <= 10)
                std::fprintf(stderr, "[inflate] fast verify: call differs (ok %d/%d, in %x/%x, out %x/%x, bits %u/%u, hold %llx/%llx)\n",
                             okA, okB, ra.in, rb.in, ra.out, rb.out, ra.bits, rb.bits, (unsigned long long)ra.hold,
                             (unsigned long long)rb.hold);
            if ((same + differ) % 200u == 0u)
                std::fprintf(stderr, "[inflate] fast verify: %llu calls the same, %llu differ\n", (unsigned long long)same,
                             (unsigned long long)differ);
        }
        const auto t0 = std::chrono::steady_clock::now();
        if (!decode(rdram, rdram, 0u, strm, start, r)) // bad data: zlib's error path is the original's (it redoes the call
        {                                              // from the same input - nothing of the state was written)
            ++g_fallbacks;
            g_original(rdram, ctx, runtime);
            return;
        }
        const uint32_t seg = nextIn & 0xE0000000u, segOut = nextOut & 0xE0000000u; // (the pointers' segment bits kept)
        const uint32_t state = rd32(rdram, strm + kState) & 0x1FFFFFFFu;
        wr32(rdram, strm + kNextIn, seg | r.in);
        wr32(rdram, strm + kNextOut, segOut | r.out);
        wr32(rdram, strm + kAvailIn, r.availIn);
        wr32(rdram, strm + kAvailOut, r.availOut);
        std::memcpy(rdram + state + kHold, &r.hold, 8);
        wr32(rdram, state + kBits, r.bits);
        if (r.endOfBlock)
            wr32(rdram, state + kMode, kModeType);
        ++g_calls;
        g_bytes += r.out - (nextOut & 0x1FFFFFFFu);
        if (g_log)
        {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            g_ms += ms;
            if (ms > g_maxMs)
            {
                g_maxMs = ms;
                g_maxBytes = r.out - (nextOut & 0x1FFFFFFFu);
            }
        }
        ctx->pc = GPR_U32(ctx, 31);
    }
}

namespace vcs
{
    void installInflate(PS2Runtime &runtime)
    {
        const char *v = std::getenv("VCS_NATIVE_INFLATE");
        if (v && *v == '0')
            return;
        g_original = runtime.lookupFunction(kInflateFast);
        if (!g_original)
            return;
        g_verify = std::getenv("VCS_NATIVE_INFLATE_VERIFY") != nullptr;
        g_log = g_verify || std::getenv("VCS_NATIVE_INFLATE_LOG") != nullptr;
        runtime.replaceFunction(kInflateFast, &inflateFastHook);
    }
}
