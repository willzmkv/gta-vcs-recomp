// VCS disc streaming without the IOP (docs/NATIVE_PORT_ROADMAP.md milestone 6 step 1; docs/game-internals.md 9).
//
// The game reads everything after boot through CDSTREAM.IRX's stream server (SIF RPC 0x65686577, EE client 0x505B48),
// called from three EE functions:
//   FUN_00221818(channel, EE dest, LSN, size)  fn 1: start a read - size in sectors, or -bytes; returns 1 (accepted).
//                                               Afterwards FUN_00221678(the reply's error word) shows a disc error.
//   FUN_002218f0(channel)                        fn 2: 0 when the channel's read is done, 0xFA while busy.
//   FUN_00221968()                               fn 5: 0 (the IOP never answered anything else).
// Here they are replaced: reads are queued for a host thread that copies from the disc image into EE RAM; a poll
// says busy until the copy is done. The game never touches a buffer before its poll says done, as with the IOP.
#include "game/vcs_stream.h"
#include "mods/rwconv.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_disc_image.h"
#include "runtime/ps2_iop_native.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

namespace vcs
{
    namespace
    {
        constexpr uint32_t kSid = 0x65686577u;
        constexpr uint32_t kRead = 0x00221818u;
        constexpr uint32_t kPoll = 0x002218F0u;
        constexpr uint32_t kFn5 = 0x00221968u;
        constexpr uint32_t kBusy = 0xFAu;
        constexpr uint32_t kChannels = 16u; // the game uses 0..5 (init asks for 6)

        struct Request
        {
            uint32_t channel, dest, lsn, bytes;
        };

        struct Reader
        {
            std::mutex mutex;
            std::condition_variable cv;
            std::condition_variable doneCv; // a read finished
            std::deque<Request> queue;
            std::atomic<uint32_t> pending[kChannels]{}; // reads queued or in flight per channel
            uint8_t *rdram = nullptr;
            bool started = false;
            // statistics
            std::atomic<uint64_t> reads{0}, bytes{0}, nanoseconds{0};
        };

        Reader &reader()
        {
            static Reader *r = new Reader(); // never destroyed: the thread may still wait on it at exit
            return *r;
        }

        // ---- game files from mods (setStreamFiles) ----------------------------------------------------------------
        // The tables (docs/game-internals.md 9d): the disc's file table at 0x5038A8 ({name ptr, 0, LSN, bytes}, count
        // at 0x486E8C), the streaming info *(*(0x489FF8) + 200) (0x18 a resource: +0x10 offset and +0x14 size in
        // sectors inside GTA3PS2.IMG, 0xFFFFFFFF = none), the model infos *(0x486518) (+0x8: ~CRC32 of the upper-case name).
        constexpr uint32_t kFileTable = 0x5038A8u, kFileCount = 0x486E8Cu, kStreaming = 0x489FF8u, kModelInfos = 0x486518u;
        constexpr uint32_t kResources = 8482u; // streaming info entries (measured; the array ends there)
        // The TXD store's pool *(0x489ECC) {items, flags, size, ...}: 0x1C an item {texture list, refs, name[20]}; slot i is
        // streaming entry *(*(0x489FF8) + 8) + i (7534 + i on this disc; docs/game-internals.md 9d)
        constexpr uint32_t kTxdPool = 0x489ECCu, kTxdItem = 0x1Cu;

        struct Overlay
        {
            uint32_t lsn, sectors;
            std::vector<uint8_t> data; // the mod's file (the rest of the entry's sectors read as zeros)
        };
        std::mutex g_filesMutex;
        std::vector<StreamFile> g_files;
        uint64_t g_filesGeneration = 0, g_resolvedGeneration = 0;
        std::vector<Overlay> g_overlays;

        // A file larger than the disc's: its record (streaming info, or the disc's file table) is pointed at sectors past
        // the end of the disc (kVirtualLsn on), where only the mod's bytes are served, so the game allocates and reads the
        // mod's size and the entry touches no neighbour. The game reads GTA3PS2.IMG as an open file (FUN_00220EC8: the
        // open-file records at 0x5058A8, 0x14 bytes {?, LSN, bytes, position, ?}) whose position is held to its size, so the
        // archive's size is raised to reach there too (file table and open records). The game's values come back at the
        // next resolve. 0x180000 sectors = 3 GB: past this disc (~2.1 GB), and its distance from the archive still fits a
        // 32-bit byte count.
        constexpr uint32_t kVirtualLsn = 0x00180000u;
        constexpr uint32_t kOpenFiles = 0x5058A8u, kOpenFileCount = 64u;
        struct Patched
        {
            uint32_t at, old; // a guest word and the game's value
        };
        std::vector<Patched> g_patched;

        void wr32(uint8_t *rdram, uint32_t addr, uint32_t v) { std::memcpy(rdram + (addr & (PS2_RAM_MASK & ~3u)), &v, 4); }

        uint32_t rd32(const uint8_t *rdram, uint32_t addr)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (addr & (PS2_RAM_MASK & ~3u)), 4);
            return v;
        }

        // CKeyGen's key: CRC32 of the upper-case name without the final inversion
        uint32_t keyOf(const std::string &name)
        {
            static uint32_t table[256] = {};
            if (!table[1])
                for (uint32_t i = 0; i < 256; ++i)
                {
                    uint32_t c = i;
                    for (int k = 0; k < 8; ++k)
                        c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    table[i] = c;
                }
            uint32_t c = 0xFFFFFFFFu;
            for (const char ch : name)
                c = table[(c ^ static_cast<uint8_t>(std::toupper(static_cast<unsigned char>(ch)))) & 0xFFu] ^ (c >> 8);
            return c;
        }

        std::vector<uint8_t> readWhole(const std::string &path)
        {
            std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
            return in ? std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()) : std::vector<uint8_t>();
        }

        // A PC file converted (src/mods/rwconv.h) - kept while the files (and the entry it goes onto) stay the same.
        struct Converted
        {
            std::string stamp;
            std::vector<uint8_t> data;
        };
        std::map<std::string, Converted> g_converted;
        std::mutex g_convertedMutex; // conversions run on several threads at once (resolveFiles)

        std::string stampOf(const std::string &path)
        {
            std::error_code ec;
            const auto p = std::filesystem::u8path(path);
            const auto size = std::filesystem::file_size(p, ec);
            const auto time = std::filesystem::last_write_time(p, ec).time_since_epoch().count();
            return path + "|" + std::to_string(size) + "|" + std::to_string(static_cast<long long>(time));
        }

        // The bytes a mod's file gives its entry: the file itself, or a PC file converted (a .dff onto the disc's own
        // entry, read from `templateLsn`). Empty when it cannot be used.
        // `weapon`: a weapon's name - its model moves to VCS's place in the hand, its dictionary's texture of that name
        // (the HUD icon) stays VCS's (rwconv.h).
        std::vector<uint8_t> entryBytes(const StreamFile &f, uint32_t index, uint32_t templateLsn, uint32_t templateSectors,
                                        const std::string &weapon)
        {
            if (f.format == StreamFile::Format::Vcs)
                return readWhole(f.path);
            const std::string stamp = stampOf(f.path) + "|" + (f.companion.empty() ? "" : stampOf(f.companion)) + "|" + std::to_string(index);
            {
                std::lock_guard<std::mutex> lock(g_convertedMutex);
                const auto cached = g_converted.find(f.path);
                if (cached != g_converted.end() && cached->second.stamp == stamp)
                    return cached->second.data;
            }
            const auto start = std::chrono::steady_clock::now();
            std::vector<uint8_t> out;
            std::string report;
            bool ok = false;
            // the disc's own entry: the model the .dff goes onto, or the dictionary whose other textures the .txd keeps
            std::vector<uint8_t> templ(size_t(templateSectors) * 2048u);
            IPS2DiscImage *disc = ps2_disc::image();
            if (!disc || !disc->readBytes(uint64_t(templateLsn) * 2048u, static_cast<uint32_t>(templ.size()), templ.data()))
                report = "the disc's entry could not be read";
            else if (f.format == StreamFile::Format::PcTxd)
                ok = rwconv::txdToXtx(readWhole(f.path), 128, &templ, out, report, weapon);
            else
            {
                const std::vector<uint8_t> txd = f.companion.empty() ? std::vector<uint8_t>() : readWhole(f.companion);
                ok = rwconv::dffToMdl(readWhole(f.path), templ, f.companion.empty() ? nullptr : &txd, out, report, weapon);
            }
            std::fprintf(stderr, "[mods] %s: %s converted%s in %.0f ms: %s\n", f.mod.c_str(), f.path.c_str(), ok ? "" : " - FAILED, not used",
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(), report.c_str());
            if (!ok)
                out.clear();
            std::lock_guard<std::mutex> lock(g_convertedMutex);
            g_converted[f.path] = {stamp, out};
            return out;
        }

        // Stream thread, before a read, when the list changed: each file to its entry's sectors (the game's tables are
        // read once they exist). False while the game has not built them yet.
        bool resolveFiles(uint8_t *rdram)
        {
            const uint32_t streamingOwner = rd32(rdram, kStreaming), modelInfos = rd32(rdram, kModelInfos);
            const uint32_t count = rd32(rdram, kFileCount);
            if (!streamingOwner || !modelInfos || !count || count > 4096u)
                return false;
            for (auto p = g_patched.rbegin(); p != g_patched.rend(); ++p) // the game's own records first
                wr32(rdram, p->at, p->old);
            g_patched.clear();
            auto patch = [&](uint32_t at, uint32_t value) {
                g_patched.push_back({at, rd32(rdram, at)});
                wr32(rdram, at, value);
            };
            uint32_t virtualLsn = kVirtualLsn;
            uint32_t imgLsn = 0, imgSectors = 0;
            for (uint32_t i = 0; i < count && !imgLsn; ++i)
            {
                const uint32_t entry = kFileTable + i * 16u, name = rd32(rdram, entry);
                char text[48] = {};
                std::memcpy(text, rdram + (name & PS2_RAM_MASK), sizeof(text) - 1);
                if (std::strstr(text, "GTA3PS2.IMG"))
                {
                    imgLsn = rd32(rdram, entry + 8u);
                    imgSectors = (rd32(rdram, entry + 12u) + 2047u) / 2048u;
                }
            }
            const uint32_t streaming = rd32(rdram, streamingOwner + 200u);
            if (!imgLsn || !streaming)
                return false;
            std::vector<StreamFile> files;
            {
                std::lock_guard<std::mutex> lock(g_filesMutex);
                files = g_files;
                g_resolvedGeneration = g_filesGeneration;
            }
            std::stable_sort(files.begin(), files.end(), [](const StreamFile &a, const StreamFile &b) { return a.priority > b.priority; });
            std::vector<Overlay> overlays;
            // PC files are converted for vehicles and weapons (the model info's type +0x10: 6, 4; FORK.md T9, T11). Other
            // models' resources are laid out otherwise (converted as a car one drew nothing): those names' .dff and .txd are
            // left out. A weapon's model moves to VCS's place in the hand; its dictionary holds its HUD icon, named like it:
            // VCS's icon stays (`weapon`).
            std::vector<std::string> notVehicles, weapons;
            for (const StreamFile &f : files)
                if (f.format != StreamFile::Format::Vcs && std::find(notVehicles.begin(), notVehicles.end(), f.name) == notVehicles.end() &&
                    std::find(weapons.begin(), weapons.end(), f.name) == weapons.end())
                {
                    const uint32_t key = keyOf(f.name);
                    for (uint32_t i = 0; i < kResources; ++i)
                    {
                        const uint32_t info = rd32(rdram, modelInfos + i * 4u);
                        if (info >= 0x100000u && info < PS2_RAM_SIZE && rd32(rdram, info + 8u) == key)
                        {
                            const uint8_t type = rdram[(info + 0x10u) & PS2_RAM_MASK];
                            if (type == 4u)
                                weapons.push_back(f.name);
                            else if (type != 6u)
                            {
                                notVehicles.push_back(f.name);
                                std::fprintf(stderr, "[mods] %s: %s - \"%s\" is no vehicle or weapon in VCS (model type %u): PC "
                                                     "models of those are not supported yet; its .dff and .txd not used\n",
                                             f.mod.c_str(), f.path.c_str(), f.name.c_str(), type);
                            }
                            break;
                        }
                    }
                }
            // a file's archive entry: "#<index>", a texture dictionary by the TXD store's names, a model by its key
            auto indexOf = [&](const StreamFile &f) {
                uint32_t index = kResources;
                if (!f.name.empty() && f.name[0] == '#')
                    index = static_cast<uint32_t>(std::strtoul(f.name.c_str() + 1, nullptr, 10));
                else if (f.textures || f.format == StreamFile::Format::PcTxd)
                {
                    const uint32_t pool = rd32(rdram, kTxdPool), base = rd32(rdram, streamingOwner + 8u);
                    const uint32_t items = pool ? rd32(rdram, pool) : 0u, size = pool ? rd32(rdram, pool + 8u) : 0u;
                    for (uint32_t i = 0; items && i < size && i < 4096u && index == kResources; ++i)
                    {
                        char text[21] = {};
                        std::memcpy(text, rdram + ((items + i * kTxdItem + 8u) & PS2_RAM_MASK), 20);
                        const std::string txd = text;
                        if (txd.size() == f.name.size() &&
                            std::equal(txd.begin(), txd.end(), f.name.begin(), [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); }))
                            index = base + i;
                    }
                }
                else
                {
                    const uint32_t key = keyOf(f.name);
                    for (uint32_t i = 0; i < kResources && index == kResources; ++i)
                    {
                        const uint32_t info = rd32(rdram, modelInfos + i * 4u);
                        if (info >= 0x100000u && info < PS2_RAM_SIZE && rd32(rdram, info + 8u) == key)
                            index = i;
                    }
                }
                return index;
            };
            // PC files are converted first, on every core at once (a car mod's 160 files took 4 s on this thread alone,
            // while the game waited for its first read): the loop below then finds them in g_converted. The same entries
            // as the loop takes - by priority, an entry once - from the records as the game has them (none patched yet).
            {
                std::vector<std::function<void()>> jobs;
                std::vector<uint32_t> claimed;
                for (const StreamFile &f : files)
                {
                    if (f.name.find('.') != std::string::npos ||
                        (f.format != StreamFile::Format::Vcs && std::find(notVehicles.begin(), notVehicles.end(), f.name) != notVehicles.end()))
                        continue;
                    const uint32_t index = indexOf(f);
                    if (index >= kResources || std::find(claimed.begin(), claimed.end(), index) != claimed.end())
                        continue;
                    claimed.push_back(index);
                    const uint32_t offset = rd32(rdram, streaming + index * 0x18u + 0x10u), sectors = rd32(rdram, streaming + index * 0x18u + 0x14u);
                    if (f.format == StreamFile::Format::Vcs || offset == 0xFFFFFFFFu || !sectors || offset + sectors > imgSectors)
                        continue;
                    const std::string weapon = std::find(weapons.begin(), weapons.end(), f.name) != weapons.end() ? f.name : std::string();
                    jobs.push_back([&f, index, lsn = imgLsn + offset, sectors, weapon] { entryBytes(f, index, lsn, sectors, weapon); });
                }
                if (!jobs.empty())
                {
                    const auto start = std::chrono::steady_clock::now();
                    const unsigned threads = std::max(1u, std::min<unsigned>(std::thread::hardware_concurrency(), static_cast<unsigned>(jobs.size())));
                    std::atomic<size_t> next{0};
                    std::vector<std::thread> pool;
                    for (unsigned k = 0; k < threads; ++k)
                        pool.emplace_back([&] {
                            for (size_t i; (i = next.fetch_add(1)) < jobs.size();)
                                jobs[i]();
                        });
                    for (std::thread &t : pool)
                        t.join();
                    std::fprintf(stderr, "[mods] %zu PC file(s) ready on %u thread(s) in %.0f ms\n", jobs.size(), threads,
                                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
                }
            }
            std::vector<uint32_t> taken;
            bool imgMoved = false;
            for (const StreamFile &f : files)
            {
                if (f.format != StreamFile::Format::Vcs && std::find(notVehicles.begin(), notVehicles.end(), f.name) != notVehicles.end())
                    continue;
                if (f.name.find('.') != std::string::npos) // a loose file (MAIN.SCM, AMERICAN.GXT): the disc file of that name
                {
                    uint32_t lsn = 0, bytes = 0, record = 0;
                    for (uint32_t i = 0; i < count && !lsn; ++i)
                    {
                        const uint32_t entry = kFileTable + i * 16u;
                        char text[64] = {};
                        std::memcpy(text, rdram + (rd32(rdram, entry) & PS2_RAM_MASK), sizeof(text) - 1);
                        std::string disc = text;
                        disc = disc.substr(disc.find_last_of('\\') + 1);
                        disc = disc.substr(0, disc.find(';'));
                        if (disc.size() == f.name.size() &&
                            std::equal(disc.begin(), disc.end(), f.name.begin(), [](char a, char b) { return std::toupper(static_cast<unsigned char>(a)) == std::toupper(static_cast<unsigned char>(b)); }))
                        {
                            lsn = rd32(rdram, entry + 8u);
                            bytes = rd32(rdram, entry + 12u);
                            record = entry + 8u;
                        }
                    }
                    if (!lsn || std::find_if(overlays.begin(), overlays.end(), [&](const Overlay &o) { return o.lsn == lsn; }) != overlays.end())
                    {
                        if (!lsn)
                            std::fprintf(stderr, "[mods] %s: %s is no file on the disc; not used\n", f.mod.c_str(), f.path.c_str());
                        continue;
                    }
                    std::ifstream in(f.path, std::ios::binary | std::ios::ate);
                    const auto size = in ? static_cast<uint64_t>(in.tellg()) : 0u;
                    if (!size)
                        continue;
                    if (size > bytes) // larger: the file table's record moved past the disc
                    {
                        lsn = virtualLsn;
                        bytes = static_cast<uint32_t>(size);
                        virtualLsn += (bytes + 2047u) / 2048u;
                        patch(record, lsn);
                        patch(record + 4u, bytes);
                    }
                    Overlay o{lsn, (bytes + 2047u) / 2048u, std::vector<uint8_t>(static_cast<size_t>(size))};
                    in.seekg(0);
                    in.read(reinterpret_cast<char *>(o.data.data()), static_cast<std::streamsize>(size));
                    std::fprintf(stderr, "[mods] %s: %s -> disc file %s (LSN %u, %u bytes)\n", f.mod.c_str(), f.path.c_str(), f.name.c_str(),
                                 lsn, bytes);
                    overlays.push_back(std::move(o));
                    continue;
                }
                const uint32_t index = indexOf(f);
                const uint32_t at = streaming + index * 0x18u;
                const uint32_t offset = index < kResources ? rd32(rdram, at + 0x10u) : 0xFFFFFFFFu;
                const uint32_t sectors = index < kResources ? rd32(rdram, at + 0x14u) : 0u;
                if (offset == 0xFFFFFFFFu || !sectors || offset + sectors > imgSectors)
                {
                    if (f.format == StreamFile::Format::Vcs)
                        std::fprintf(stderr, "[mods] %s: %s is no entry of GTA3PS2.IMG; not used\n", f.mod.c_str(), f.path.c_str());
                    else if (f.name == "wheels") // VC's models/generic/wheels.dff: VCS's wheel models are resident (GAME.DTZ)
                        std::fprintf(stderr, "[mods] %s: %s - VCS keeps its wheels in memory from the start, not in GTA3PS2.IMG: "
                                             "not supported yet (FORK.md T10); not used\n", f.mod.c_str(), f.path.c_str());
                    else
                        std::fprintf(stderr, "[mods] %s: %s - VCS has no vehicle or weapon \"%s\" (rename the files as the one it "
                                             "should replace); not used\n", f.mod.c_str(), f.path.c_str(), f.name.c_str());
                    continue;
                }
                if (std::find(taken.begin(), taken.end(), index) != taken.end())
                    continue; // a mod with a higher priority has it
                const bool weapon = std::find(weapons.begin(), weapons.end(), f.name) != weapons.end();
                std::vector<uint8_t> bytes = entryBytes(f, index, imgLsn + offset, sectors, weapon ? f.name : std::string());
                const auto size = static_cast<uint64_t>(bytes.size());
                if (!size)
                    continue;
                uint32_t first = imgLsn + offset, span = sectors;
                if (size > uint64_t(sectors) * 2048u) // larger: the streaming record moved past the disc (offset and size)
                {
                    span = static_cast<uint32_t>((size + 2047u) / 2048u);
                    first = virtualLsn;
                    virtualLsn += span;
                    patch(at + 0x10u, first - imgLsn);
                    patch(at + 0x14u, span);
                    imgMoved = true;
                }
                Overlay o{first, span, std::move(bytes)};
                taken.push_back(index);
                std::fprintf(stderr, "[mods] %s: %s -> GTA3PS2.IMG entry %u (LSN %u, %u sectors%s)\n", f.mod.c_str(), f.path.c_str(),
                             index, o.lsn, o.sectors, o.sectors > sectors ? ", larger than the disc's: moved" : "");
                overlays.push_back(std::move(o));
            }
            if (imgMoved) // the archive reaches the moved entries: its size in the file table and in its open records
            {
                const uint32_t bytes = (virtualLsn - imgLsn) * 2048u;
                for (uint32_t i = 0; i < count; ++i)
                    if (rd32(rdram, kFileTable + i * 16u + 8u) == imgLsn)
                        patch(kFileTable + i * 16u + 12u, bytes);
                for (uint32_t i = 0; i < kOpenFileCount; ++i)
                    if (rd32(rdram, kOpenFiles + i * 0x14u + 4u) == imgLsn)
                        patch(kOpenFiles + i * 0x14u + 8u, bytes);
            }
            std::lock_guard<std::mutex> lock(g_filesMutex);
            g_overlays = std::move(overlays);
            return true;
        }

        // the mods' bytes over a disc read that covers their entries (whole or in part)
        void overlay(uint32_t lsn, uint32_t bytes, uint8_t *dest)
        {
            std::lock_guard<std::mutex> lock(g_filesMutex);
            if (g_overlays.empty())
                return;
            const uint64_t start = uint64_t(lsn) * 2048u, end = start + bytes;
            for (const Overlay &o : g_overlays)
            {
                const uint64_t from = std::max<uint64_t>(start, uint64_t(o.lsn) * 2048u);
                const uint64_t to = std::min<uint64_t>(end, (uint64_t(o.lsn) + o.sectors) * 2048u);
                if (from < to)
                {
                    const uint64_t inEntry = from - uint64_t(o.lsn) * 2048u;
                    const uint64_t span = to - from;
                    const uint64_t copyBytes = inEntry < o.data.size() ? std::min(span, o.data.size() - inEntry) : 0u;
                    if (copyBytes > 0)
                        std::memcpy(dest + (from - start), o.data.data() + inEntry, static_cast<size_t>(copyBytes));
                    if (span > copyBytes)
                        std::memset(dest + (from - start) + copyBytes, 0, static_cast<size_t>(span - copyBytes));
                }
            }
        }

        void run()
        {
            Reader &r = reader();
            for (;;)
            {
                Request request;
                {
                    std::unique_lock<std::mutex> lock(r.mutex);
                    r.cv.wait(lock, [&] { return !r.queue.empty(); });
                    request = r.queue.front();
                    r.queue.pop_front();
                }
                const auto start = std::chrono::steady_clock::now();
                IPS2DiscImage *disc = ps2_disc::image();
                const uint32_t dest = request.dest & PS2_RAM_MASK;
                const uint32_t bytes = std::min<uint32_t>(request.bytes, PS2_RAM_SIZE - dest);
                if (request.lsn >= kVirtualLsn) // a mod's larger file: nothing of the disc there
                {
                    static int logged = 0;
                    if (logged++ < 8)
                        std::fprintf(stderr, "[stream] read of a mod's moved file: LSN %u, %u bytes, channel %u\n", request.lsn, bytes,
                                     request.channel);
                }
                if (request.lsn >= kVirtualLsn)
                    std::memset(r.rdram + dest, 0, bytes);
                else if (!disc || !disc->readBytes(static_cast<uint64_t>(request.lsn) * 2048u, bytes, r.rdram + dest))
                    std::fprintf(stderr, "[stream] read of %u bytes at LSN 0x%x failed\n", bytes, request.lsn);
                bool resolve;
                {
                    std::lock_guard<std::mutex> lock(g_filesMutex);
                    resolve = g_resolvedGeneration != g_filesGeneration;
                }
                if (resolve)
                    resolveFiles(r.rdram);
                overlay(request.lsn, bytes, r.rdram + dest);
                r.reads.fetch_add(1, std::memory_order_relaxed);
                r.bytes.fetch_add(bytes, std::memory_order_relaxed);
                r.nanoseconds.fetch_add(static_cast<uint64_t>(
                                            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()),
                                        std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> lock(r.mutex);
                    r.pending[request.channel % kChannels].fetch_sub(1, std::memory_order_release); // the data before "done"
                }
                r.doneCv.notify_all();
            }
        }

        void report()
        {
            static const bool log = std::getenv("VCS_STREAM_LOG") != nullptr;
            static auto last = std::chrono::steady_clock::now();
            if (!log || std::chrono::steady_clock::now() - last < std::chrono::seconds(5))
                return;
            last = std::chrono::steady_clock::now();
            Reader &r = reader();
            std::fprintf(stderr, "[stream] %llu reads, %.1f MB, %.1f ms reading (last 5 s)\n",
                         static_cast<unsigned long long>(r.reads.exchange(0)), r.bytes.exchange(0) / 1048576.0,
                         r.nanoseconds.exchange(0) / 1e6);
        }

        // FUN_00221818(channel, dest, lsn, size)
        PS2_GUEST_CC void streamRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            Reader &r = reader();
            const int32_t size = static_cast<int32_t>(GPR_U32(ctx, 7));
            Request request{GPR_U32(ctx, 4), GPR_U32(ctx, 5), GPR_U32(ctx, 6),
                            size < 0 ? static_cast<uint32_t>(-size) : static_cast<uint32_t>(size) * 2048u};
            {
                std::lock_guard<std::mutex> lock(r.mutex);
                if (!r.started)
                {
                    r.rdram = rdram;
                    r.started = true;
                    std::thread(run).detach();
                }
                r.pending[request.channel % kChannels].fetch_add(1, std::memory_order_relaxed);
                r.queue.push_back(request);
            }
            r.cv.notify_one();
            report();
            SET_GPR_U64(ctx, 2, 1); // accepted (the reply word)
            ctx->pc = GPR_U32(ctx, 31);
        }

        // FUN_002218f0(channel): a read still in flight is waited for up to 20 ms - the game polls right after starting
        // most reads and otherwise loses a frame per read to "busy" (boot took 1 s longer than through the IOP)
        PS2_GUEST_CC void streamPoll(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            Reader &r = reader();
            std::atomic<uint32_t> &pending = r.pending[GPR_U32(ctx, 4) % kChannels];
            bool busy = pending.load(std::memory_order_acquire) != 0u;
            if (busy)
            {
                std::unique_lock<std::mutex> lock(r.mutex);
                busy = !r.doneCv.wait_for(lock, std::chrono::milliseconds(20),
                                          [&] { return pending.load(std::memory_order_acquire) == 0u; });
            }
            SET_GPR_U64(ctx, 2, busy ? kBusy : 0u);
            ctx->pc = GPR_U32(ctx, 31);
        }

        // FUN_00221968()
        PS2_GUEST_CC void streamFn5(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            SET_GPR_U64(ctx, 2, 0);
            ctx->pc = GPR_U32(ctx, 31);
        }
    }

    void setStreamFiles(std::vector<StreamFile> files)
    {
        std::lock_guard<std::mutex> lock(g_filesMutex);
        g_files = std::move(files);
        ++g_filesGeneration; // resolved at the next read - also an empty list: the game's records put back
    }

    bool installStreamHooks(PS2Runtime &runtime)
    {
        const char *v = std::getenv("VCS_NATIVE_STREAM");
        if (v && *v == '0')
        {
            std::fprintf(stderr, "[stream] through the IOP (VCS_NATIVE_STREAM=0)\n");
            return false;
        }
        runtime.replaceFunction(kRead, &streamRead);
        runtime.replaceFunction(kPoll, &streamPoll);
        runtime.replaceFunction(kFn5, &streamFn5);
        return true;
    }

    void retireIop(bool nativeComplete)
    {
        const char *v = std::getenv("PS2X_IOP");
        if ((v && *v == '1') || (!v && !nativeComplete))
        {
            ps2_iop_native::setEmulatedIop(true);
            std::fprintf(stderr, "[iop] emulated IOP on%s\n", nativeComplete ? " (PS2X_IOP=1)" : " (sound or streaming through it)");
            return;
        }
        ps2_iop_native::setEmulatedIop(false);
        // The only call that still reaches the stream server: fn 0 at boot (CDSTREAM 0x0000: its read queues and CD
        // thread for 6 channels - the IOP's own sound code reads through channels 2 and 3), no reply. Reads, polls and
        // fn 5 never get to it (the EE functions are replaced).
        ps2_iop_native::registerServer(kSid, [](uint8_t *, const ps2x::iop::RpcRequest &request)
                                       {
                                           if (request.function != 0)
                                               std::fprintf(stderr, "[stream] unexpected call %u to the stream server\n",
                                                            request.function);
                                       });
        std::fprintf(stderr, "[iop] emulated IOP off: no IRX module loaded or run (PS2X_IOP=1 runs it)\n");
    }
}
