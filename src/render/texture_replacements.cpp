#include "render/texture_replacements.h"

#define XXH_INLINE_ALL
#include "../../third_party/xxhash/xxhash.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 8192
#include "../../third_party/stb/stb_image.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace render
{
    uint64_t textureHash(const void *data, size_t bytes) { return XXH3_64bits(data, bytes); }

    namespace
    {
        struct Name
        {
            uint64_t pixels = 0, palette = 0;
            uint32_t bits = 0;
            bool operator==(const Name &) const = default;
        };
        struct NameHash
        {
            size_t operator()(const Name &n) const { return size_t(n.pixels ^ (n.palette * 31) ^ n.bits); }
        };
        bool parseName(const std::string &stem, Name &n)
        {
            // Full-texture base images; reject region/mip filenames instead of accidentally aliasing them.
            const size_t a = stem.find('-'), b = a == stem.npos ? a : stem.find('-', a + 1);
            auto hex = [](std::string_view s, auto &v) {
                if (s.empty()) return false;
                const auto r = std::from_chars(s.data(), s.data() + s.size(), v, 16);
                return r.ec == std::errc{} && r.ptr == s.data() + s.size();
            };
            if (a == stem.npos || !hex(std::string_view(stem).substr(0, a), n.pixels)) return false;
            if (b != stem.npos && !hex(std::string_view(stem).substr(a + 1, b - a - 1), n.palette)) return false;
            if (!hex(std::string_view(stem).substr((b == stem.npos ? a : b) + 1), n.bits)) return false;
            n.bits &= ~(1u << 14); // old PCSX2 TCC flag; modern PCSX2 ignores it too
            return true;
        }
        size_t memoryBudget()
        {
            const char *e = std::getenv("VCS_TEXTURE_CACHE_MB");
            return size_t(e ? std::clamp(std::atoi(e), 64, 4096) : 512) * 1024 * 1024;
        }
        uint32_t le32(const std::vector<uint8_t> &b, size_t at)
        {
            uint32_t v;
            std::memcpy(&v, b.data() + at, 4);
            return v;
        }

        // A DDS (DDS_HEADER, + DDS_HEADER_DXT10 for "DX10"): a 2D texture's levels copied as stored. The sRGB variants
        // are read as unorm - the values are used as they are, as a PNG's are (PCSX2 does the same). `why`: the reason
        // a file is refused.
        std::shared_ptr<ReplacementImage> loadDds(const std::vector<uint8_t> &file, uint32_t id, const char *&why)
        {
            using rhi::Format;
            why = "not a DDS";
            if (file.size() < 128 || le32(file, 0) != 0x20534444u || le32(file, 4) != 124u) return {};
            const uint32_t flags = le32(file, 8), height = le32(file, 12), width = le32(file, 16);
            const uint32_t mipCount = le32(file, 28), pfFlags = le32(file, 80), fourCC = le32(file, 84);
            const uint32_t rgbBits = le32(file, 88), rMask = le32(file, 92), gMask = le32(file, 96), bMask = le32(file, 100);
            const uint32_t aMask = le32(file, 104), caps2 = le32(file, 112);
            auto cc = [](const char *s) { return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 | uint32_t(uint8_t(s[3])) << 24; };
            size_t data = 128;
            Format format = Format::Unknown;
            why = "an unsupported DDS format (BC1-BC5, BC7, RGBA8 or BGRA8 with alpha)";
            if ((pfFlags & 0x4u) && fourCC == cc("DX10"))
            {
                if (file.size() < 148) return {};
                data = 148;
                if (le32(file, 132) != 3u || le32(file, 140) > 1u || (le32(file, 136) & 0x4u))
                {
                    why = "not a single 2D texture (an array, cube map or volume)";
                    return {};
                }
                switch (le32(file, 128)) // DXGI_FORMAT
                {
                case 28: case 29: format = Format::RGBA8; break;
                case 87: case 91: format = Format::BGRA8; break;
                case 71: case 72: format = Format::BC1; break;
                case 74: case 75: format = Format::BC2; break;
                case 77: case 78: format = Format::BC3; break;
                case 80: format = Format::BC4; break;
                case 83: format = Format::BC5; break;
                case 98: case 99: format = Format::BC7; break;
                default: return {};
                }
            }
            else if (pfFlags & 0x4u)
            {
                if (fourCC == cc("DXT1")) format = Format::BC1;
                else if (fourCC == cc("DXT2") || fourCC == cc("DXT3")) format = Format::BC2;
                else if (fourCC == cc("DXT4") || fourCC == cc("DXT5")) format = Format::BC3;
                else if (fourCC == cc("ATI1") || fourCC == cc("BC4U")) format = Format::BC4;
                else if (fourCC == cc("ATI2") || fourCC == cc("BC5U")) format = Format::BC5;
                else return {};
            }
            else if ((pfFlags & 0x40u) && (pfFlags & 0x1u) && rgbBits == 32 && aMask == 0xFF000000u)
            {
                if (rMask == 0xFFu && gMask == 0xFF00u && bMask == 0xFF0000u) format = Format::RGBA8;
                else if (rMask == 0xFF0000u && gMask == 0xFF00u && bMask == 0xFFu) format = Format::BGRA8;
                else return {};
            }
            else
                return {};
            if (caps2 & 0x200200u) // DDSCAPS2_CUBEMAP, DDSCAPS2_VOLUME
            {
                why = "not a single 2D texture (an array, cube map or volume)";
                return {};
            }
            why = "a size the GPU can't take (1-8192 texels; a multiple of 4 for block compression)";
            if (width == 0 || height == 0 || width > 8192 || height > 8192) return {};
            if (rhi::blockCompressed(format) && ((width | height) & 3u)) return {}; // D3D11's rule for level 0
            uint32_t chain = 1;
            for (uint32_t s = std::max(width, height); s > 1; s >>= 1) ++chain;
            const uint32_t wanted = std::min({(flags & 0x20000u) && mipCount ? mipCount : 1u, chain, 16u});
            auto image = std::make_shared<ReplacementImage>();
            image->id = id;
            image->width = width;
            image->height = height;
            image->format = format;
            size_t at = data;
            for (uint32_t i = 0; i < wanted; ++i) // the levels the file really holds
            {
                const uint32_t w = std::max(1u, width >> i), h = std::max(1u, height >> i);
                const size_t bytes = size_t(rhi::rowBytes(format, w)) * rhi::rowCount(format, h);
                if (at + bytes > file.size()) break;
                image->levelOffsets.push_back(at - data);
                at += bytes;
            }
            why = "shorter than its first level";
            if (image->levelOffsets.empty()) return {};
            image->levels.assign(file.begin() + std::ptrdiff_t(data), file.begin() + std::ptrdiff_t(at));
            return image;
        }

        std::shared_ptr<ReplacementImage> loadPng(const std::vector<uint8_t> &encoded, uint32_t id)
        {
            int w = 0, h = 0, channels = 0;
            if (!stbi_info_from_memory(encoded.data(), int(encoded.size()), &w, &h, &channels) ||
                w <= 0 || h <= 0 || w > 8192 || h > 8192 || uint64_t(w) * h > 64 * 1024 * 1024) return {};
            auto *rgba = stbi_load_from_memory(encoded.data(), int(encoded.size()), &w, &h, &channels, 4);
            if (!rgba) return {};
            auto image = std::make_shared<ReplacementImage>();
            image->id = id;
            image->width = uint32_t(w);
            image->height = uint32_t(h);
            image->pixels.resize(size_t(w) * h);
            std::memcpy(image->pixels.data(), rgba, image->pixels.size() * 4);
            stbi_image_free(rgba);
            return image;
        }

        bool isDds(const std::filesystem::path &path)
        {
            auto ext = path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            return ext == ".dds";
        }

        std::shared_ptr<ReplacementImage> load(const std::filesystem::path &path, uint32_t id, bool blockCompression)
        {
            std::ifstream f(path, std::ios::binary | std::ios::ate);
            const auto bytes = f ? f.tellg() : std::streampos(-1);
            if (bytes <= 0 || bytes > 256 * 1024 * 1024) return {};
            std::vector<uint8_t> file(static_cast<size_t>(bytes));
            f.seekg(0);
            if (!f.read(reinterpret_cast<char *>(file.data()), file.size())) return {};
            if (!isDds(path))
                return loadPng(file, id);
            const char *why = nullptr;
            auto image = loadDds(file, id, why);
            if (image && rhi::blockCompressed(image->format) && !blockCompression)
            {
                why = "block-compressed, which this GPU / driver can't sample (Vulkan: no textureCompressionBC)";
                image.reset();
            }
            if (!image)
                std::fprintf(stderr, "[textures] %s: %s\n", path.filename().string().c_str(), why);
            return image;
        }
    }

    size_t ReplacementImage::gpuBytes(bool mipChain) const
    {
        if (dds())
            return mipChain || levelOffsets.size() < 2 ? levels.size() : levelOffsets[1];
        return pixels.size() * 4 * (mipChain ? 4u : 3u) / 3u;
    }

    struct TextureReplacements::Impl
    {
        struct File
        {
            std::filesystem::path path;
            int source = 0, priority = 0;
            bool dds = false, pending = false, failed = false, matched = false;
        };
        std::vector<File> files{1}; // id 0 = no match
        std::vector<size_t> perSource; // names each source supplies (after priorities)
        std::unordered_map<Name, uint32_t, NameHash> names;
        std::unordered_set<uint32_t> shapes;
        size_t gpuBudget = memoryBudget(), readyBytes = 0;
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<uint32_t> queue;
        // find()'s "[textures] matched" lines: written by the worker, not by the GS stage that finds them (the log is
        // unbuffered - a call to the file system per line; 145 first matches in one frame held the GS stage ~9 ms,
        // PERFORMANCE.md 41)
        std::vector<std::string> matchedLines;
        std::vector<std::shared_ptr<const ReplacementImage>> ready;
        std::thread worker;
        bool stopping = false;
        std::atomic<bool> blockCompression{true}; // the device's (setBlockCompression)
        uint32_t matched = 0, loaded = 0;

        void run()
        {
            for (;;)
            {
                uint32_t id;
                std::vector<std::string> lines;
                {
                    std::unique_lock lock(mutex);
                    cv.wait(lock, [&] { return stopping || !queue.empty() || !matchedLines.empty(); });
                    if (stopping) return;
                    lines.swap(matchedLines);
                    if (queue.empty())
                        id = 0;
                    else
                    {
                        id = queue.front();
                        queue.pop_front();
                    }
                }
                for (const std::string &line : lines)
                    std::fputs(line.c_str(), stderr);
                if (!id)
                    continue;
                auto image = load(files[id].path, id, blockCompression.load(std::memory_order_relaxed));
                std::unique_lock lock(mutex);
                if (!image)
                {
                    files[id].failed = true;
                    files[id].pending = false;
                    std::fprintf(stderr, "[textures] failed to load %s; keeping original\n", files[id].path.string().c_str());
                    continue;
                }
                // Bound loaded images waiting for the GS stage. One large image may exceed 128 MiB on its own.
                const size_t bytes = image->pixels.size() * 4 + image->levels.size();
                cv.wait(lock, [&] { return stopping || ready.empty() || readyBytes + bytes <= 128 * 1024 * 1024; });
                if (stopping) return;
                readyBytes += bytes;
                ++loaded;
                static const char *const formats[] = {"?", "RGBA8", "R32F", "D32F", "BGRA8", "BC1", "BC2", "BC3", "BC4", "BC5", "BC7"};
                std::fprintf(stderr, "[textures] loaded %s (%ux%u %s, %zu levels, %.2f MiB)\n", files[id].path.filename().string().c_str(),
                             image->width, image->height, image->dds() ? formats[size_t(image->format)] : "PNG",
                             image->dds() ? image->levelOffsets.size() : size_t(1), bytes / 1048576.0);
                ready.push_back(std::move(image));
            }
        }
    };

    TextureReplacements::TextureReplacements() : m(std::make_unique<Impl>())
    {
        // the environment's one pack (tools, tests): VCS_TEXTURE_REPLACEMENTS=1 + VCS_TEXTURE_PACK
        const char *enabled = std::getenv("VCS_TEXTURE_REPLACEMENTS");
        if (!enabled || enabled[0] != '1') return;
        const char *dir = std::getenv("VCS_TEXTURE_PACK");
        if (!dir || !*dir)
        {
            std::fprintf(stderr, "[textures] no pack directory configured; keeping originals\n");
            return;
        }
        index({{std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t *>(dir))), 50, "VCS_TEXTURE_PACK"}});
    }

    TextureReplacements::TextureReplacements(const std::vector<Source> &sources) : m(std::make_unique<Impl>()) { index(sources); }

    void TextureReplacements::index(const std::vector<Source> &sources)
    {
        uint32_t rejected = 0, duplicates = 0;
        bool scanFailed = false;
        m->perSource.assign(sources.size(), 0);
        for (size_t s = 0; s < sources.size(); ++s)
        {
            auto root = sources[s].path;
            std::error_code ec;
            if (std::filesystem::is_directory(root / "replacements", ec)) root /= "replacements";
            std::vector<std::filesystem::path> paths;
            for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec))
            {
                if (it->is_directory(ec) && it->path().filename() == "dumps")
                    it.disable_recursion_pending(); // PCSX2's own dumps beside a pack's replacements are not replacements
                else if (it->is_regular_file(ec))
                    paths.push_back(it->path());
            }
            scanFailed |= bool(ec);
            std::sort(paths.begin(), paths.end()); // deterministic duplicate handling
            for (const auto &path : paths)
            {
                auto ext = path.extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                if (ext != ".png" && ext != ".dds") continue;
                Name n;
                if (!parseName(path.stem().string(), n)) { ++rejected; continue; }
                const Impl::File file{path, int(s), sources[s].priority, ext == ".dds"};
                const auto [it, added] = m->names.emplace(n, uint32_t(m->files.size()));
                if (!added)
                {
                    ++duplicates;
                    // one name twice: the higher priority (a mod over another), then the DDS (the GPU's own format)
                    Impl::File &held = m->files[it->second];
                    if (file.priority > held.priority || (file.priority == held.priority && file.dds && !held.dds))
                        held = file;
                    continue;
                }
                m->files.push_back(file);
                m->shapes.insert(n.bits & 0x3FFFu);
            }
        }
        size_t dds = 0;
        for (size_t i = 1; i < m->files.size(); ++i)
        {
            ++m->perSource[size_t(m->files[i].source)];
            dds += m->files[i].dds;
        }
        std::fprintf(stderr, "[textures] indexed %zu replacements (%zu DDS, %zu PNG) from %zu source(s) (%u unsupported names, %u duplicates); cache %zu MiB%s\n",
                     m->names.size(), dds, m->names.size() - dds, sources.size(), rejected, duplicates, m->gpuBudget >> 20,
                     scanFailed ? "; a directory scan failed" : "");
        for (size_t s = 0; s < sources.size(); ++s)
            std::fprintf(stderr, "[textures]   %s (priority %d): %zu in use, %s\n", sources[s].name.c_str(), sources[s].priority,
                         m->perSource[s], sources[s].path.string().c_str());
        if (!m->names.empty()) m->worker = std::thread([this] { m->run(); });
    }

    const std::vector<size_t> &TextureReplacements::perSource() const { return m->perSource; }
    TextureReplacements::~TextureReplacements()
    {
        { std::lock_guard lock(m->mutex); m->stopping = true; }
        m->cv.notify_all();
        if (m->worker.joinable()) m->worker.join();
        for (const std::string &line : m->matchedLines)
            std::fputs(line.c_str(), stderr);
        if (!m->names.empty())
            std::fprintf(stderr, "[textures] summary: %u/%zu pack files matched, %u image loads\n", m->matched, m->names.size(), m->loaded);
    }
    bool TextureReplacements::candidate(uint32_t bits) const { return m->shapes.contains(bits & 0x3FFFu); }
    uint32_t TextureReplacements::find(uint64_t pixels, uint64_t palette, uint32_t bits)
    {
        // Opt-in diagnostic: exact names computed by the renderer (never texture data).
        static const bool trace = std::getenv("VCS_TEXTURE_TRACE") != nullptr;
        if (trace)
            std::fprintf(stderr, "[textures] lookup %llx-%llx-%08x\n", (unsigned long long)pixels,
                         (unsigned long long)palette, bits);
        const auto it = m->names.find({pixels, palette, bits});
        if (it == m->names.end()) return 0;
        auto &file = m->files[it->second];
        if (!file.matched)
        {
            file.matched = true; ++m->matched;
            char line[512];
            std::snprintf(line, sizeof(line), "[textures] matched %s (%u/%zu)\n", file.path.filename().string().c_str(), m->matched,
                          m->names.size());
            {
                std::lock_guard lock(m->mutex);
                m->matchedLines.emplace_back(line);
            }
            m->cv.notify_all();
        }
        return it->second;
    }
    void TextureReplacements::request(uint32_t id)
    {
        std::lock_guard lock(m->mutex);
        auto &file = m->files.at(id);
        if (file.pending || file.failed) return;
        file.pending = true;
        m->queue.push_back(id);
        m->cv.notify_all();
    }
    std::vector<std::shared_ptr<const ReplacementImage>> TextureReplacements::takeReady()
    {
        std::vector<std::shared_ptr<const ReplacementImage>> result;
        if (!m->worker.joinable()) return result;
        {
            std::lock_guard lock(m->mutex);
            result.swap(m->ready);
            m->readyBytes = 0;
            for (const auto &image : result) m->files[image->id].pending = false;
        }
        m->cv.notify_all();
        return result;
    }
    size_t TextureReplacements::budget() const { return m->gpuBudget; }
    void TextureReplacements::setBlockCompression(bool supported) { m->blockCompression.store(supported, std::memory_order_relaxed); }
}
