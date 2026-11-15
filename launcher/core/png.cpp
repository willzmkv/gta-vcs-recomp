#include "png.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 8192
#include "../../third_party/stb/stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

namespace core
{
    bool readPng(const std::filesystem::path &path, int &width, int &height, std::vector<uint8_t> &rgba)
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in)
            return false;
        const auto size = in.tellg();
        if (size <= 0 || size > 64 * 1024 * 1024)
            return false;
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        in.seekg(0);
        if (!in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
            return false;
        int w = 0, h = 0, ch = 0;
        uint8_t *decoded = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &ch, 4);
        if (!decoded || w <= 0 || h <= 0)
        {
            if (decoded)
                stbi_image_free(decoded);
            return false;
        }
        width = w;
        height = h;
        rgba.assign(decoded, decoded + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
        stbi_image_free(decoded);
        return true;
    }

    void resizeRgba(const uint8_t *src, int srcW, int srcH, uint8_t *dst, int dstW, int dstH)
    {
        if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0)
            return;
        if (srcW == dstW && srcH == dstH)
        {
            std::memcpy(dst, src, static_cast<size_t>(dstW) * static_cast<size_t>(dstH) * 4);
            return;
        }
        // Box-filter + bilinear area averaging so downscaling large artwork (e.g. 2172x1500 -> 1920x1326) is crisp
        const float scaleX = static_cast<float>(srcW) / static_cast<float>(dstW);
        const float scaleY = static_cast<float>(srcH) / static_cast<float>(dstH);
        for (int y = 0; y < dstH; ++y)
        {
            const float sy0 = y * scaleY;
            const float sy1 = std::min(static_cast<float>(srcH), (y + 1) * scaleY);
            const int iy0 = std::clamp(static_cast<int>( std::floor(sy0)), 0, srcH - 1);
            const int iy1 = std::clamp(static_cast<int>( std::ceil(sy1)) - 1, iy0, srcH - 1);
            for (int x = 0; x < dstW; ++x)
            {
                const float sx0 = x * scaleX;
                const float sx1 = std::min(static_cast<float>(srcW), (x + 1) * scaleX);
                const int ix0 = std::clamp(static_cast<int>( std::floor(sx0)), 0, srcW - 1);
                const int ix1 = std::clamp(static_cast<int>( std::ceil(sx1)) - 1, ix0, srcW - 1);
                float r = 0, g = 0, b = 0, a = 0, wSum = 0;
                for (int sy = iy0; sy <= iy1; ++sy)
                {
                    const float wy = std::min(sy + 1.0f, sy1) - std::max(static_cast<float>(sy), sy0);
                    for (int sx = ix0; sx <= ix1; ++sx)
                    {
                        const float wx = std::min(sx + 1.0f, sx1) - std::max(static_cast<float>(sx), sx0);
                        const float w = std::max(0.0001f, wx * wy);
                        const uint8_t *p = src + (static_cast<size_t>(sy) * static_cast<size_t>(srcW) + static_cast<size_t>(sx)) * 4;
                        // Premultiplied accumulation prevents dark fringes on transparent logos
                        const float pa = (p[3] / 255.0f) * w;
                        r += p[0] * pa;
                        g += p[1] * pa;
                        b += p[2] * pa;
                        a += p[3] * w;
                        wSum += w;
                    }
                }
                uint8_t *out = dst + (static_cast<size_t>(y) * static_cast<size_t>(dstW) + static_cast<size_t>(x)) * 4;
                const float finalA = wSum > 0 ? a / wSum : 0.0f;
                const float totalPremulA = a / 255.0f;
                if (totalPremulA > 1e-4f)
                {
                    out[0] = static_cast<uint8_t>(std::clamp(r / totalPremulA + 0.5f, 0.0f, 255.0f));
                    out[1] = static_cast<uint8_t>(std::clamp(g / totalPremulA + 0.5f, 0.0f, 255.0f));
                    out[2] = static_cast<uint8_t>(std::clamp(b / totalPremulA + 0.5f, 0.0f, 255.0f));
                }
                else
                {
                    out[0] = out[1] = out[2] = 0;
                }
                out[3] = static_cast<uint8_t>(std::clamp(finalA + 0.5f, 0.0f, 255.0f));
            }
        }
    }
    namespace
    {
        uint32_t crc32(const uint8_t *data, size_t size, uint32_t crc = 0)
        {
            static uint32_t table[256];
            if (!table[1])
                for (uint32_t i = 0; i < 256; ++i)
                {
                    uint32_t c = i;
                    for (int k = 0; k < 8; ++k)
                        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    table[i] = c;
                }
            crc = ~crc;
            for (size_t i = 0; i < size; ++i)
                crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
            return ~crc;
        }

        void put32(std::vector<uint8_t> &out, uint32_t v)
        {
            out.push_back(uint8_t(v >> 24));
            out.push_back(uint8_t(v >> 16));
            out.push_back(uint8_t(v >> 8));
            out.push_back(uint8_t(v));
        }

        void chunk(std::vector<uint8_t> &out, const char *type, const std::vector<uint8_t> &data)
        {
            put32(out, uint32_t(data.size()));
            const size_t start = out.size();
            out.insert(out.end(), type, type + 4);
            out.insert(out.end(), data.begin(), data.end());
            put32(out, crc32(out.data() + start, out.size() - start));
        }
    }

    bool writePng(const std::filesystem::path &path, int width, int height, const uint8_t *rgba)
    {
        std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        std::vector<uint8_t> header;
        put32(header, uint32_t(width));
        put32(header, uint32_t(height));
        header.insert(header.end(), {8, 6, 0, 0, 0}); // 8 bits, RGBA, deflate, no filter method, no interlace
        chunk(png, "IHDR", header);

        // scanlines with filter byte 0, in a zlib stream of stored blocks
        const size_t row = size_t(width) * 4;
        std::vector<uint8_t> raw;
        raw.reserve((row + 1) * size_t(height));
        for (int y = 0; y < height; ++y)
        {
            raw.push_back(0);
            raw.insert(raw.end(), rgba + row * size_t(y), rgba + row * size_t(y + 1));
        }
        std::vector<uint8_t> z = {0x78, 0x01};
        for (size_t at = 0; at < raw.size() || raw.empty();)
        {
            const size_t n = std::min<size_t>(raw.size() - at, 65535);
            const bool last = at + n == raw.size();
            z.push_back(last ? 1 : 0);
            z.push_back(uint8_t(n));
            z.push_back(uint8_t(n >> 8));
            z.push_back(uint8_t(~n));
            z.push_back(uint8_t(~n >> 8));
            z.insert(z.end(), raw.begin() + ptrdiff_t(at), raw.begin() + ptrdiff_t(at + n));
            at += n;
            if (last)
                break;
        }
        uint32_t a = 1, b = 0; // Adler-32
        for (uint8_t c : raw)
        {
            a = (a + c) % 65521;
            b = (b + a) % 65521;
        }
        put32(z, (b << 16) | a);
        chunk(png, "IDAT", z);
        chunk(png, "IEND", {});

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(png.data()), std::streamsize(png.size()));
        return bool(out);
    }
}
